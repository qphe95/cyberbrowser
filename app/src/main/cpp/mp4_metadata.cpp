/* Minimal MP4/M4A album art embedder
   Injects a udta/meta/ilst/covr atom hierarchy into an existing MP4 file.
   Updates all stco/co64 chunk offsets to account for the inserted bytes. */

#include "mp4_metadata.h"
#include "utf8_filename.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

typedef struct {
    uint64_t offset;
    uint64_t size;
    char type[5];
} Box;

static bool read_box(const uint8_t *data, size_t data_size, size_t pos, Box *out) {
    if (pos + 8 > data_size) return false;
    uint64_t sz = be32(data + pos);
    if (sz == 0) sz = data_size - pos;
    else if (sz == 1) {
        if (pos + 16 > data_size) return false;
        sz = ((uint64_t)be32(data + pos + 8) << 32) | be32(data + pos + 12);
    }
    memcpy(out->type, data + pos + 4, 4);
    out->type[4] = '\0';
    out->offset = pos;
    out->size = sz;
    return true;
}

static bool find_box(const uint8_t *data, size_t data_size, const char *type, Box *out) {
    size_t pos = 0;
    while (pos < data_size) {
        Box b;
        if (!read_box(data, data_size, pos, &b)) return false;
        if (strcmp(b.type, type) == 0) {
            *out = b;
            return true;
        }
        pos += b.size;
    }
    return false;
}

static uint8_t *read_all(const char *path, size_t *out_size) {
    FILE *f = fopen_utf8(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

static bool write_all(const char *path, const uint8_t *data, size_t size) {
    FILE *f = fopen_utf8(path, "wb");
    if (!f) return false;
    size_t written = fwrite(data, 1, size, f);
    fclose(f);
    return written == size;
}

/* Recursively update stco / co64 chunk offsets by delta. */
static void patch_offsets(uint8_t *data, size_t data_size, int64_t delta) {
    size_t pos = 0;
    while (pos < data_size) {
        Box b;
        if (!read_box(data, data_size, pos, &b)) break;

        if (strcmp(b.type, "stco") == 0 && b.size >= 16) {
            uint32_t entries = be32(data + pos + 12);
            for (uint32_t i = 0; i < entries && pos + 16 + i * 4 + 4 <= data_size; i++) {
                size_t off = pos + 16 + i * 4;
                uint32_t old = be32(data + off);
                write_be32(data + off, old + (uint32_t)delta);
            }
        } else if (strcmp(b.type, "co64") == 0 && b.size >= 16) {
            uint32_t entries = be32(data + pos + 12);
            for (uint32_t i = 0; i < entries && pos + 16 + i * 8 + 8 <= data_size; i++) {
                size_t off = pos + 16 + i * 8;
                uint64_t old = ((uint64_t)be32(data + off) << 32) | be32(data + off + 4);
                old += (uint64_t)delta;
                write_be32(data + off, (uint32_t)(old >> 32));
                write_be32(data + off + 4, (uint32_t)old);
            }
        } else if (b.size > 8 && (
                   strcmp(b.type, "moov") == 0 || strcmp(b.type, "trak") == 0 ||
                   strcmp(b.type, "mdia") == 0 || strcmp(b.type, "minf") == 0 ||
                   strcmp(b.type, "stbl") == 0 || strcmp(b.type, "dinf") == 0)) {
            patch_offsets(data + pos + 8, (size_t)(b.size - 8), delta);
        }
        pos += b.size;
    }
}

/* Build just a covr atom (container: size + type + data_atom) */
static uint8_t *build_covr(const uint8_t *img, size_t img_len, bool is_jpeg,
                           size_t *out_len) {
    size_t data_atom_len = 16 + img_len;
    uint8_t *data_atom = (uint8_t *)malloc(data_atom_len);
    if (!data_atom) return NULL;
    write_be32(data_atom, (uint32_t)data_atom_len);
    memcpy(data_atom + 4, "data", 4);
    write_be32(data_atom + 8, 0);
    write_be32(data_atom + 12, is_jpeg ? 0x0000000DU : 0x0000000EU);
    memcpy(data_atom + 16, img, img_len);

    size_t covr_len = 8 + data_atom_len;
    uint8_t *covr = (uint8_t *)malloc(covr_len);
    if (!covr) { free(data_atom); return NULL; }
    write_be32(covr, (uint32_t)covr_len);
    memcpy(covr + 4, "covr", 4);
    memcpy(covr + 8, data_atom, data_atom_len);
    free(data_atom);

    *out_len = covr_len;
    return covr;
}

/* Build a ©nam atom (container: size + type + data_atom) */
static uint8_t *build_nam(const char *title, size_t *out_len) {
    size_t title_len = strlen(title);
    size_t data_atom_len = 16 + title_len;
    uint8_t *data_atom = (uint8_t *)malloc(data_atom_len);
    if (!data_atom) return NULL;
    write_be32(data_atom, (uint32_t)data_atom_len);
    memcpy(data_atom + 4, "data", 4);
    write_be32(data_atom + 8, 0);          /* version/flags */
    write_be32(data_atom + 12, 1);         /* well-known type: UTF-8 text */
    memcpy(data_atom + 16, title, title_len);

    size_t nam_len = 8 + data_atom_len;
    uint8_t *nam = (uint8_t *)malloc(nam_len);
    if (!nam) { free(data_atom); return NULL; }
    write_be32(nam, (uint32_t)nam_len);
    memcpy(nam + 4, "\xA9nam", 4);        /* ©nam */
    memcpy(nam + 8, data_atom, data_atom_len);
    free(data_atom);

    *out_len = nam_len;
    return nam;
}

/* Build udta -> meta -> hdlr -> ilst -> covr -> data  hierarchy.
   Returns allocated buffer; caller frees. */
static uint8_t *build_meta(const uint8_t *img, size_t img_len, bool is_jpeg,
                           size_t *out_len) {
    size_t covr_len = 0;
    uint8_t *covr = build_covr(img, img_len, is_jpeg, &covr_len);
    if (!covr) return NULL;

    /* ilst atom */
    size_t ilst_len = 8 + covr_len;
    uint8_t *ilst = (uint8_t *)malloc(ilst_len);
    if (!ilst) { free(covr); return NULL; }
    write_be32(ilst, (uint32_t)ilst_len);
    memcpy(ilst + 4, "ilst", 4);
    memcpy(ilst + 8, covr, covr_len);
    free(covr);

    /* hdlr atom (minimal, 33 bytes) */
    static const uint8_t hdlr[33] = {
        0x00, 0x00, 0x00, 0x21,
        'h','d','l','r',
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        'm','d','i','r',
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00
    };

    /* meta atom (full atom: size + type + version/flags + hdlr + ilst) */
    size_t meta_len = 12 + sizeof(hdlr) + ilst_len;
    uint8_t *meta = (uint8_t *)malloc(meta_len);
    if (!meta) { free(ilst); return NULL; }
    write_be32(meta, (uint32_t)meta_len);
    memcpy(meta + 4, "meta", 4);
    write_be32(meta + 8, 0);
    memcpy(meta + 12, hdlr, sizeof(hdlr));
    memcpy(meta + 12 + sizeof(hdlr), ilst, ilst_len);
    free(ilst);

    /* udta atom */
    size_t udta_len = 8 + meta_len;
    uint8_t *udta = (uint8_t *)malloc(udta_len);
    if (!udta) { free(meta); return NULL; }
    write_be32(udta, (uint32_t)udta_len);
    memcpy(udta + 4, "udta", 4);
    memcpy(udta + 8, meta, meta_len);
    free(meta);

    *out_len = udta_len;
    return udta;
}

bool mp4_embed_album_art(const char *mp4_path, const uint8_t *image_data,
                         size_t image_size, bool is_jpeg) {
    size_t file_len = 0;
    uint8_t *file = read_all(mp4_path, &file_len);
    if (!file) return false;

    Box moov;
    if (!find_box(file, file_len, "moov", &moov)) {
        free(file);
        return false;
    }

    /* Try to find existing udta -> meta -> ilst chain inside moov */
    Box udta, meta, ilst;
    bool has_ilst = false;
    if (find_box(file + moov.offset + 8, (size_t)(moov.size - 8), "udta", &udta)) {
        udta.offset += moov.offset + 8;
        if (find_box(file + udta.offset + 8, (size_t)(udta.size - 8), "meta", &meta)) {
            meta.offset += udta.offset + 8;
            /* meta is a full atom: skip version/flags */
            if (find_box(file + meta.offset + 12, (size_t)(meta.size - 12), "ilst", &ilst)) {
                ilst.offset += meta.offset + 12;
                has_ilst = true;
            }
        }
    }

    if (has_ilst) {
        /* Insert covr at end of existing ilst */
        size_t covr_len = 0;
        uint8_t *covr = build_covr(image_data, image_size, is_jpeg, &covr_len);
        if (!covr) {
            free(file);
            return false;
        }

        size_t ilst_end = (size_t)(ilst.offset + ilst.size);
        size_t new_len = file_len + covr_len;
        uint8_t *out = (uint8_t *)malloc(new_len);
        if (!out) {
            free(file); free(covr);
            return false;
        }

        memcpy(out, file, ilst_end);
        memcpy(out + ilst_end, covr, covr_len);
        memcpy(out + ilst_end + covr_len, file + ilst_end, file_len - ilst_end);

        /* Grow ilst, meta, udta, moov sizes */
        write_be32(out + ilst.offset, (uint32_t)(ilst.size + covr_len));
        write_be32(out + meta.offset, (uint32_t)(meta.size + covr_len));
        write_be32(out + udta.offset, (uint32_t)(udta.size + covr_len));
        write_be32(out + moov.offset, (uint32_t)(moov.size + covr_len));

        /* Patch chunk offsets */
        patch_offsets(out + moov.offset + 8, (size_t)(moov.size + covr_len - 8),
                      (int64_t)covr_len);

        bool ok = write_all(mp4_path, out, new_len);
        free(file);
        free(covr);
        free(out);
        return ok;
    }

    /* Fall back: insert full udta->meta->ilst->covr at end of moov */
    size_t full_meta_len = 0;
    uint8_t *full_meta = build_meta(image_data, image_size, is_jpeg, &full_meta_len);
    if (!full_meta) {
        free(file);
        return false;
    }

    size_t moov_end = (size_t)(moov.offset + moov.size);
    size_t new_len = file_len + full_meta_len;
    uint8_t *out = (uint8_t *)malloc(new_len);
    if (!out) {
        free(file); free(full_meta);
        return false;
    }

    memcpy(out, file, moov_end);
    memcpy(out + moov_end, full_meta, full_meta_len);
    memcpy(out + moov_end + full_meta_len, file + moov_end, file_len - moov_end);

    write_be32(out + moov.offset, (uint32_t)(moov.size + full_meta_len));
    patch_offsets(out + moov.offset + 8, (size_t)(moov.size + full_meta_len - 8),
                  (int64_t)full_meta_len);

    bool ok = write_all(mp4_path, out, new_len);

    free(file);
    free(full_meta);
    free(out);
    return ok;
}

bool mp4_set_title(const char *mp4_path, const char *title) {
    if (!title || !title[0]) return false;

    size_t file_len = 0;
    uint8_t *file = read_all(mp4_path, &file_len);
    if (!file) return false;

    Box moov;
    if (!find_box(file, file_len, "moov", &moov)) {
        free(file);
        return false;
    }

    /* Try to find existing udta -> meta -> ilst chain inside moov */
    Box udta, meta, ilst;
    bool has_ilst = false;
    if (find_box(file + moov.offset + 8, (size_t)(moov.size - 8), "udta", &udta)) {
        udta.offset += moov.offset + 8;
        if (find_box(file + udta.offset + 8, (size_t)(udta.size - 8), "meta", &meta)) {
            meta.offset += udta.offset + 8;
            /* meta is a full atom: skip version/flags */
            if (find_box(file + meta.offset + 12, (size_t)(meta.size - 12), "ilst", &ilst)) {
                ilst.offset += meta.offset + 12;
                has_ilst = true;
            }
        }
    }

    size_t nam_len = 0;
    uint8_t *nam = build_nam(title, &nam_len);
    if (!nam) {
        free(file);
        return false;
    }

    if (has_ilst) {
        /* Insert ©nam at end of existing ilst */
        size_t ilst_end = (size_t)(ilst.offset + ilst.size);
        size_t new_len = file_len + nam_len;
        uint8_t *out = (uint8_t *)malloc(new_len);
        if (!out) {
            free(file); free(nam);
            return false;
        }

        memcpy(out, file, ilst_end);
        memcpy(out + ilst_end, nam, nam_len);
        memcpy(out + ilst_end + nam_len, file + ilst_end, file_len - ilst_end);

        /* Grow ilst, meta, udta, moov sizes */
        write_be32(out + ilst.offset, (uint32_t)(ilst.size + nam_len));
        write_be32(out + meta.offset, (uint32_t)(meta.size + nam_len));
        write_be32(out + udta.offset, (uint32_t)(udta.size + nam_len));
        write_be32(out + moov.offset, (uint32_t)(moov.size + nam_len));

        /* Patch chunk offsets */
        patch_offsets(out + moov.offset + 8, (size_t)(moov.size + nam_len - 8),
                      (int64_t)nam_len);

        bool ok = write_all(mp4_path, out, new_len);
        free(file);
        free(nam);
        free(out);
        return ok;
    }

    /* Fall back: insert full udta->meta->ilst->©nam at end of moov */
    /* Build minimal udta->meta->hdlr->ilst->©nam hierarchy */
    size_t ilst_len = 8 + nam_len;
    uint8_t *ilst_buf = (uint8_t *)malloc(ilst_len);
    if (!ilst_buf) { free(file); free(nam); return false; }
    write_be32(ilst_buf, (uint32_t)ilst_len);
    memcpy(ilst_buf + 4, "ilst", 4);
    memcpy(ilst_buf + 8, nam, nam_len);

    static const uint8_t hdlr[33] = {
        0x00, 0x00, 0x00, 0x21,
        'h','d','l','r',
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        'm','d','i','r',
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00
    };

    size_t meta_len = 12 + sizeof(hdlr) + ilst_len;
    uint8_t *meta_buf = (uint8_t *)malloc(meta_len);
    if (!meta_buf) { free(file); free(nam); free(ilst_buf); return false; }
    write_be32(meta_buf, (uint32_t)meta_len);
    memcpy(meta_buf + 4, "meta", 4);
    write_be32(meta_buf + 8, 0);
    memcpy(meta_buf + 12, hdlr, sizeof(hdlr));
    memcpy(meta_buf + 12 + sizeof(hdlr), ilst_buf, ilst_len);

    size_t udta_len = 8 + meta_len;
    uint8_t *udta_buf = (uint8_t *)malloc(udta_len);
    if (!udta_buf) { free(file); free(nam); free(ilst_buf); free(meta_buf); return false; }
    write_be32(udta_buf, (uint32_t)udta_len);
    memcpy(udta_buf + 4, "udta", 4);
    memcpy(udta_buf + 8, meta_buf, meta_len);

    size_t moov_end = (size_t)(moov.offset + moov.size);
    size_t new_len = file_len + udta_len;
    uint8_t *out = (uint8_t *)malloc(new_len);
    if (!out) {
        free(file); free(nam); free(ilst_buf); free(meta_buf); free(udta_buf);
        return false;
    }

    memcpy(out, file, moov_end);
    memcpy(out + moov_end, udta_buf, udta_len);
    memcpy(out + moov_end + udta_len, file + moov_end, file_len - moov_end);

    write_be32(out + moov.offset, (uint32_t)(moov.size + udta_len));
    patch_offsets(out + moov.offset + 8, (size_t)(moov.size + udta_len - 8),
                  (int64_t)udta_len);

    bool ok = write_all(mp4_path, out, new_len);

    free(file);
    free(nam);
    free(ilst_buf);
    free(meta_buf);
    free(udta_buf);
    free(out);
    return ok;
}

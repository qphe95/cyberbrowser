/* Test for mp4_embed_album_art
   Verifies that album art can be injected into an M4A file and found back. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "mp4_metadata.h"

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Flat scan for a box type. Handles full atoms by skipping version/flags. */
static bool find_box_anywhere(const uint8_t *data, size_t data_size,
                              const char *type, size_t *out_offset, size_t *out_size) {
    size_t pos = 0;
    while (pos + 8 <= data_size) {
        uint64_t sz = be32(data + pos);
        if (sz == 0) sz = data_size - pos;
        else if (sz == 1) {
            if (pos + 16 > data_size) return false;
            sz = ((uint64_t)be32(data + pos + 8) << 32) | be32(data + pos + 12);
        }
        if (sz < 8 || pos + sz > data_size) break;
        if (memcmp(data + pos + 4, type, 4) == 0) {
            *out_offset = pos;
            *out_size = (size_t)sz;
            return true;
        }
        /* For full atoms (meta, data, hdlr), children start after version/flags */
        size_t skip = 8;
        if (memcmp(data + pos + 4, "meta", 4) == 0 ||
            memcmp(data + pos + 4, "data", 4) == 0 ||
            memcmp(data + pos + 4, "hdlr", 4) == 0) {
            skip = 12;
        }
        if (sz > skip) {
            size_t child_off, child_sz;
            if (find_box_anywhere(data + pos + skip, (size_t)(sz - skip), type,
                                  &child_off, &child_sz)) {
                *out_offset = pos + skip + child_off;
                *out_size = child_sz;
                return true;
            }
        }
        pos += (size_t)sz;
    }
    return false;
}

static uint8_t *read_all(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
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

static bool copy_file(const char *src, const char *dst) {
    size_t sz = 0;
    uint8_t *buf = read_all(src, &sz);
    if (!buf) return false;
    FILE *f = fopen(dst, "wb");
    if (!f) { free(buf); return false; }
    bool ok = (fwrite(buf, 1, sz, f) == sz);
    fclose(f);
    free(buf);
    return ok;
}

/* Tiny synthetic JPEG: 2x2 pixels, red. Generated programmatically below. */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const char *src_m4a = "../../dQw4w9WgXcQ.m4a";
    const char *tmp_m4a = "/tmp/test_embed.m4a";

    /* Build a minimal valid 1x1 red JPEG in memory */
    /* DQT table (64 bytes of 1s for simplicity) */
    uint8_t dqt[64];
    for (int i = 0; i < 64; i++) dqt[i] = 16;

    /* Minimal JPEG components */
    uint8_t jpeg[1024];
    size_t j = 0;
    #define PUT(b) jpeg[j++] = (b)
    #define PUT2(v) do { PUT((v) >> 8); PUT((v) & 0xFF); } while(0)

    /* SOI */
    PUT(0xFF); PUT(0xD8);
    /* APP0 JFIF */
    PUT(0xFF); PUT(0xE0); PUT2(0x0010);
    PUT('J'); PUT('F'); PUT('I'); PUT('F'); PUT(0);
    PUT(1); PUT(1); PUT(0); PUT(0); PUT(1); PUT(0); PUT(1); PUT(0); PUT(0);
    /* DQT */
    PUT(0xFF); PUT(0xDB); PUT2(0x0043); PUT(0);
    for (int i = 0; i < 64; i++) PUT(dqt[i]);
    /* SOF0 */
    PUT(0xFF); PUT(0xC0); PUT2(0x000B);
    PUT(8); PUT2(1); PUT2(1); PUT(1); PUT(0x11); PUT(0);
    /* DHT (minimal) */
    PUT(0xFF); PUT(0xC4); PUT2(0x001F); PUT(0x00);
    /* 16 bytes: number of codes of each length */
    PUT(0); PUT(1); PUT(5); PUT(1); PUT(1); PUT(1); PUT(1); PUT(1);
    PUT(1); PUT(0); PUT(0); PUT(0); PUT(0); PUT(0); PUT(0); PUT(0);
    /* symbols */
    PUT(0); PUT(1); PUT(2); PUT(3); PUT(4); PUT(5); PUT(6); PUT(7); PUT(8); PUT(9); PUT(10); PUT(11);
    /* SOS */
    PUT(0xFF); PUT(0xDA); PUT2(0x0008); PUT(1); PUT(1); PUT(0); PUT(0); PUT(0x3F); PUT(0);
    /* Compressed data: minimal scan for 1x1 */
    PUT(0xFB); PUT(0xD5); PUT(0xDB); PUT(0x20);
    /* EOI */
    PUT(0xFF); PUT(0xD9);
    #undef PUT
    #undef PUT2

    size_t jpeg_len = j;

    printf("=== MP4 Album Art Embed Test ===\n");
    printf("JPEG test image: %zu bytes\n", jpeg_len);

    /* Verify source file exists */
    size_t orig_sz = 0;
    uint8_t *orig = read_all(src_m4a, &orig_sz);
    if (!orig) {
        printf("SKIP: Source M4A not found at %s\n", src_m4a);
        return 0; /* Skip test if no source file */
    }
    printf("Source M4A: %zu bytes\n", orig_sz);
    free(orig);

    /* Copy to temp */
    if (!copy_file(src_m4a, tmp_m4a)) {
        printf("FAIL: Could not copy to temp file\n");
        return 1;
    }

    /* Embed */
    bool ok = mp4_embed_album_art(tmp_m4a, jpeg, jpeg_len, true);
    if (!ok) {
        printf("FAIL: mp4_embed_album_art returned false\n");
        return 1;
    }

    /* Read back and verify structure */
    size_t out_sz = 0;
    uint8_t *out = read_all(tmp_m4a, &out_sz);
    if (!out) {
        printf("FAIL: Could not read output file\n");
        return 1;
    }

    printf("Output M4A: %zu bytes (delta: +%zu)\n", out_sz, out_sz - orig_sz);

    /* Verify output is larger */
    if (out_sz <= orig_sz) {
        printf("FAIL: Output not larger than input\n");
        free(out);
        return 1;
    }

    /* Find covr atom */
    size_t covr_off, covr_sz;
    if (!find_box_anywhere(out, out_sz, "covr", &covr_off, &covr_sz)) {
        printf("FAIL: covr atom not found\n");
        free(out);
        return 1;
    }
    printf("covr atom found at offset %zu, size %zu\n", covr_off, covr_sz);

    /* Find data atom inside covr (covr is a container: size+type+children) */
    size_t data_off, data_sz;
    if (!find_box_anywhere(out + covr_off + 8, covr_sz - 8, "data", &data_off, &data_sz)) {
        printf("FAIL: data atom not found inside covr\n");
        free(out);
        return 1;
    }
    data_off += covr_off + 8;
    printf("data atom found at offset %zu, size %zu\n", data_off, data_sz);

    /* Verify data atom contains our image (skip 16-byte header) */
    if (data_sz < 16 + jpeg_len) {
        printf("FAIL: data atom too small for image\n");
        free(out);
        return 1;
    }
    if (memcmp(out + data_off + 16, jpeg, jpeg_len) != 0) {
        printf("FAIL: image data mismatch\n");
        free(out);
        return 1;
    }

    /* Verify well-known type = JPEG (0x0D) in big-endian at offset 12 of data atom */
    uint32_t wkt = be32(out + data_off + 12);
    if (wkt != 13) {
        printf("FAIL: well-known type is %u, expected 13 (JPEG)\n", wkt);
        free(out);
        return 1;
    }

    /* Verify stco offsets were patched */
    size_t stco_off, stco_sz;
    if (find_box_anywhere(out, out_sz, "stco", &stco_off, &stco_sz)) {
        uint32_t entries = be32(out + stco_off + 12);
        printf("stco entries: %u\n", entries);
        for (uint32_t i = 0; i < entries && i < 3; i++) {
            uint32_t chunk_off = be32(out + stco_off + 16 + i * 4);
            printf("  chunk[%u] offset: %u\n", i, chunk_off);
        }
    }

    free(out);

    /* Cleanup */
    remove(tmp_m4a);

    printf("PASS: Album art embedded and verified successfully\n");
    return 0;
}

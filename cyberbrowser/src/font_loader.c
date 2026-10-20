/* Web font decoding (WOFF 1.0 and WOFF2) to plain sfnt bytes.
 *
 * WOFF 1.0 (https://www.w3.org/TR/WOFF/): a table directory whose tables are
 * individually zlib-compressed; inflated with stb_image's zlib decoder.
 *
 * WOFF2 (https://www.w3.org/TR/WOFF2/): one Brotli stream holding every
 * table, with glyf / loca (and optionally hmtx) stored in a transformed,
 * more compressible form that is rebuilt here.  Rebuilt glyphs use plain
 * int16 coordinates and loca is always written in the long format (head's
 * indexToLocFormat is patched to match). */

#include "font_loader.h"

#include <stdlib.h>
#include <string.h>

#include <brotli/decode.h>

/* stb_image (implemented in image_cache.c) carries the zlib inflater. */
extern int stbi_zlib_decode_buffer(char *obuffer, int olen, const char *ibuffer, int ilen);

#define TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* ---- Readers / writers ---- */

typedef struct {
    const uint8_t *p;
    size_t len, pos;
    bool err;
} Rd;

static Rd rd_make(const uint8_t *p, size_t len) { Rd r = { p, len, 0, false }; return r; }

static uint8_t rd_u8(Rd *r)
{
    if (r->pos + 1 > r->len) { r->err = true; return 0; }
    return r->p[r->pos++];
}

static uint16_t rd_u16(Rd *r)
{
    if (r->pos + 2 > r->len) { r->err = true; return 0; }
    uint16_t v = (uint16_t)((r->p[r->pos] << 8) | r->p[r->pos + 1]);
    r->pos += 2;
    return v;
}

static uint32_t rd_u32(Rd *r)
{
    if (r->pos + 4 > r->len) { r->err = true; return 0; }
    uint32_t v = ((uint32_t)r->p[r->pos] << 24) | ((uint32_t)r->p[r->pos + 1] << 16) |
                 ((uint32_t)r->p[r->pos + 2] << 8) | (uint32_t)r->p[r->pos + 3];
    r->pos += 4;
    return v;
}

/* WOFF2 UIntBase128: 1-5 bytes, 7 bits each, high bit = more. */
static uint32_t rd_base128(Rd *r)
{
    uint32_t v = 0;
    for (int i = 0; i < 5; i++) {
        uint8_t b = rd_u8(r);
        if (r->err || (i == 0 && b == 0x80) || (v & 0xFE000000u)) { r->err = true; return 0; }
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) return v;
    }
    r->err = true;
    return 0;
}

/* WOFF2 255UInt16. */
static uint16_t rd_255u16(Rd *r)
{
    uint8_t code = rd_u8(r);
    if (code == 253) return rd_u16(r);
    if (code == 255) return (uint16_t)(rd_u8(r) + 253);
    if (code == 254) return (uint16_t)(rd_u8(r) + 253 * 2);
    return code;
}

typedef struct {
    uint8_t *p;
    size_t len, cap;
    bool err;
} Buf;

static bool buf_reserve(Buf *b, size_t extra)
{
    if (b->err) return false;
    if (b->len + extra <= b->cap) return true;
    size_t nc = b->cap ? b->cap * 2 : 4096;
    while (nc < b->len + extra) nc *= 2;
    uint8_t *np = (uint8_t *)realloc(b->p, nc);
    if (!np) { b->err = true; return false; }
    b->p = np;
    b->cap = nc;
    return true;
}

static void buf_bytes(Buf *b, const void *src, size_t n)
{
    if (!n || !buf_reserve(b, n)) return;
    memcpy(b->p + b->len, src, n);
    b->len += n;
}

static void buf_u8(Buf *b, uint8_t v) { buf_bytes(b, &v, 1); }

static void buf_u16(Buf *b, uint16_t v)
{
    uint8_t t[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    buf_bytes(b, t, 2);
}

static void buf_u32(Buf *b, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    buf_bytes(b, t, 4);
}

static void buf_pad4(Buf *b)
{
    while (!b->err && (b->len & 3)) buf_u8(b, 0);
}

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* ---- sfnt assembly ---- */

typedef FontSfntTable SfntTable;

static int table_cmp(const void *a, const void *b)
{
    uint32_t x = ((const SfntTable *)a)->tag, y = ((const SfntTable *)b)->tag;
    return x < y ? -1 : x > y;
}

static uint32_t table_checksum(const uint8_t *p, size_t len)
{
    uint32_t sum = 0;
    size_t i = 0;
    for (; i + 4 <= len; i += 4)
        sum += ((uint32_t)p[i] << 24) | ((uint32_t)p[i + 1] << 16) | ((uint32_t)p[i + 2] << 8) | p[i + 3];
    if (i < len) {
        uint8_t t[4] = { 0, 0, 0, 0 };
        memcpy(t, p + i, len - i);
        sum += ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) | ((uint32_t)t[2] << 8) | t[3];
    }
    return sum;
}

/* Write an sfnt from `tables` (sorted here).  A 'head' table gets
 * indexToLocFormat = loca_format when loca_format >= 0. */
static bool sfnt_build(uint32_t flavor, SfntTable *tables, int n, int loca_format,
                       uint8_t **out, size_t *out_len)
{
    qsort(tables, (size_t)n, sizeof(*tables), table_cmp);
    Buf b = { 0 };
    int sel = 0;
    while ((2 << sel) <= n) sel++;
    uint16_t search = (uint16_t)(16 << sel);
    buf_u32(&b, flavor);
    buf_u16(&b, (uint16_t)n);
    buf_u16(&b, search);
    buf_u16(&b, (uint16_t)sel);
    buf_u16(&b, (uint16_t)(n * 16 - search));
    size_t dir = b.len;
    for (int i = 0; i < n * 4; i++) buf_u32(&b, 0);
    for (int i = 0; i < n; i++) {
        size_t off = b.len;
        buf_bytes(&b, tables[i].data, tables[i].len);
        if (b.err) break;
        if (tables[i].tag == TAG('h', 'e', 'a', 'd') && tables[i].len >= 54) {
            put_u32(b.p + off + 8, 0);                     /* checksumAdjustment */
            if (loca_format >= 0) put_u16(b.p + off + 50, (uint16_t)loca_format);
        }
        uint8_t *rec = b.p + dir + (size_t)i * 16;
        put_u32(rec, tables[i].tag);
        put_u32(rec + 4, table_checksum(b.p + off, tables[i].len));
        put_u32(rec + 8, (uint32_t)off);
        put_u32(rec + 12, (uint32_t)tables[i].len);
        buf_pad4(&b);
    }
    if (b.err) { free(b.p); return false; }
    *out = b.p;
    *out_len = b.len;
    return true;
}

/* ---- WOFF 1.0 ---- */

static bool woff1_decode(const uint8_t *data, size_t len, uint8_t **out, size_t *out_len)
{
    Rd r = rd_make(data, len);
    rd_u32(&r);                         /* 'wOFF' */
    uint32_t flavor = rd_u32(&r);
    rd_u32(&r);                         /* length */
    uint16_t ntables = rd_u16(&r);
    if (r.err || ntables == 0 || ntables > 512) return false;
    r.pos = 44;
    SfntTable *tables = (SfntTable *)calloc(ntables, sizeof(*tables));
    uint8_t **owned = (uint8_t **)calloc(ntables, sizeof(*owned));
    bool ok = tables && owned;
    for (int i = 0; ok && i < ntables; i++) {
        uint32_t tag = rd_u32(&r), off = rd_u32(&r), comp = rd_u32(&r), orig = rd_u32(&r);
        rd_u32(&r);                     /* origChecksum */
        if (r.err || off > len || comp > len - off || orig > (64u << 20)) { ok = false; break; }
        tables[i].tag = tag;
        tables[i].len = orig;
        if (comp == orig) {
            tables[i].data = data + off;
        } else {
            owned[i] = (uint8_t *)malloc(orig ? orig : 1);
            if (!owned[i]) { ok = false; break; }
            int got = stbi_zlib_decode_buffer((char *)owned[i], (int)orig,
                                              (const char *)(data + off), (int)comp);
            if (got != (int)orig) { ok = false; break; }
            tables[i].data = owned[i];
        }
    }
    if (ok) ok = sfnt_build(flavor, tables, ntables, -1, out, out_len);
    if (owned) for (int i = 0; i < ntables; i++) free(owned[i]);
    free(owned);
    free(tables);
    return ok;
}

/* ---- WOFF2 ---- */

static const uint32_t k_woff2_tags[63] = {
    TAG('c','m','a','p'), TAG('h','e','a','d'), TAG('h','h','e','a'), TAG('h','m','t','x'),
    TAG('m','a','x','p'), TAG('n','a','m','e'), TAG('O','S','/','2'), TAG('p','o','s','t'),
    TAG('c','v','t',' '), TAG('f','p','g','m'), TAG('g','l','y','f'), TAG('l','o','c','a'),
    TAG('p','r','e','p'), TAG('C','F','F',' '), TAG('V','O','R','G'), TAG('E','B','D','T'),
    TAG('E','B','L','C'), TAG('g','a','s','p'), TAG('h','d','m','x'), TAG('k','e','r','n'),
    TAG('L','T','S','H'), TAG('P','C','L','T'), TAG('V','D','M','X'), TAG('v','h','e','a'),
    TAG('v','m','t','x'), TAG('B','A','S','E'), TAG('G','D','E','F'), TAG('G','P','O','S'),
    TAG('G','S','U','B'), TAG('E','B','S','C'), TAG('J','S','T','F'), TAG('M','A','T','H'),
    TAG('C','B','D','T'), TAG('C','B','L','C'), TAG('C','O','L','R'), TAG('C','P','A','L'),
    TAG('S','V','G',' '), TAG('s','b','i','x'), TAG('a','c','n','t'), TAG('a','v','a','r'),
    TAG('b','d','a','t'), TAG('b','l','o','c'), TAG('b','s','l','n'), TAG('c','v','a','r'),
    TAG('f','d','s','c'), TAG('f','e','a','t'), TAG('f','m','t','x'), TAG('f','v','a','r'),
    TAG('g','v','a','r'), TAG('h','s','t','y'), TAG('j','u','s','t'), TAG('l','c','a','r'),
    TAG('m','o','r','t'), TAG('m','o','r','x'), TAG('o','p','b','d'), TAG('p','r','o','p'),
    TAG('t','r','a','k'), TAG('Z','a','p','f'), TAG('S','i','l','f'), TAG('G','l','a','t'),
    TAG('G','l','o','c'), TAG('F','e','a','t'), TAG('S','i','l','l'),
};

typedef struct {
    uint32_t tag;
    bool transformed;
    uint32_t orig_len;
    uint32_t stored_len;     /* bytes in the decompressed stream */
    const uint8_t *data;     /* into the decompressed stream */
} W2Table;

static int with_sign(int flag, int base) { return (flag & 1) ? base : -base; }

/* Rebuild glyf + loca from the transformed glyf table.  Also returns each
 * glyph's xMin (for an hmtx transform) in *xmins (malloc'd). */
static bool woff2_rebuild_glyf(const uint8_t *src, size_t len, Buf *glyf, Buf *loca,
                               uint16_t *out_num_glyphs, int16_t **xmins)
{
    Rd h = rd_make(src, len);
    rd_u16(&h);                                  /* reserved */
    uint16_t option_flags = rd_u16(&h);
    uint16_t num_glyphs = rd_u16(&h);
    rd_u16(&h);                                  /* indexFormat (we always write long) */
    uint32_t sizes[7];
    for (int i = 0; i < 7; i++) sizes[i] = rd_u32(&h);
    if (h.err) return false;
    size_t off = h.pos;
    Rd st[7];
    for (int i = 0; i < 7; i++) {
        if (sizes[i] > len - off) return false;
        st[i] = rd_make(src + off, sizes[i]);
        off += sizes[i];
    }
    Rd *n_contour = &st[0], *n_points = &st[1], *flags = &st[2], *glyphs = &st[3],
       *composite = &st[4], *bbox = &st[5], *instr = &st[6];
    const uint8_t *overlap = NULL;
    if (option_flags & 1) {
        size_t olen = ((size_t)num_glyphs + 7) / 8;
        if (olen > len - off) return false;
        overlap = src + off;
    }
    size_t bitmap_len = (((size_t)num_glyphs + 31) >> 5) << 2;
    if (bbox->len < bitmap_len) return false;
    const uint8_t *bbox_bitmap = bbox->p;
    bbox->pos = bitmap_len;

    *xmins = (int16_t *)calloc(num_glyphs ? num_glyphs : 1, sizeof(int16_t));
    if (!*xmins) return false;
    int32_t *xs = NULL, *ys = NULL;
    uint8_t *on = NULL;
    uint16_t *end_pts = NULL;
    size_t pts_cap = 0, ends_cap = 0;
    bool ok = true;

    for (uint32_t g = 0; ok && g < num_glyphs; g++) {
        buf_u32(loca, (uint32_t)glyf->len);
        int16_t nc = (int16_t)rd_u16(n_contour);
        bool has_bbox = (bbox_bitmap[g >> 3] & (0x80 >> (g & 7))) != 0;
        if (n_contour->err) { ok = false; break; }
        if (nc == 0) continue;                    /* empty glyph */
        size_t start = glyf->len;
        if (nc > 0) {
            if ((size_t)nc > ends_cap) {
                ends_cap = (size_t)nc * 2;
                uint16_t *ne = (uint16_t *)realloc(end_pts, ends_cap * sizeof(*ne));
                if (!ne) { ok = false; break; }
                end_pts = ne;
            }
            uint32_t total = 0;
            for (int c = 0; c < nc; c++) {
                total += rd_255u16(n_points);
                if (total > 0xFFFF) { ok = false; break; }
                end_pts[c] = (uint16_t)(total - 1);
            }
            if (!ok || n_points->err) { ok = false; break; }
            if (total > pts_cap) {
                pts_cap = total * 2;
                int32_t *nx = (int32_t *)realloc(xs, pts_cap * sizeof(*nx));
                int32_t *ny = nx ? (int32_t *)realloc(ys, pts_cap * sizeof(*ny)) : NULL;
                uint8_t *no = ny ? (uint8_t *)realloc(on, pts_cap) : NULL;
                if (nx) xs = nx;
                if (ny) ys = ny;
                if (no) on = no;
                if (!nx || !ny || !no) { ok = false; break; }
            }
            int32_t x = 0, y = 0;
            for (uint32_t i = 0; i < total; i++) {
                uint8_t fl = rd_u8(flags);
                on[i] = !(fl >> 7);
                int f = fl & 0x7F;
                int dx, dy;
                if (f < 10) {
                    dx = 0;
                    dy = with_sign(f, ((f & 14) << 7) + rd_u8(glyphs));
                } else if (f < 20) {
                    dx = with_sign(f, (((f - 10) & 14) << 7) + rd_u8(glyphs));
                    dy = 0;
                } else if (f < 84) {
                    int b0 = f - 20, b1 = rd_u8(glyphs);
                    dx = with_sign(f, 1 + (b0 & 0x30) + (b1 >> 4));
                    dy = with_sign(f >> 1, 1 + ((b0 & 0x0C) << 2) + (b1 & 0x0F));
                } else if (f < 120) {
                    int b0 = f - 84, d0 = rd_u8(glyphs), d1 = rd_u8(glyphs);
                    dx = with_sign(f, 1 + ((b0 / 12) << 8) + d0);
                    dy = with_sign(f >> 1, 1 + (((b0 % 12) >> 2) << 8) + d1);
                } else if (f < 124) {
                    int d0 = rd_u8(glyphs), d1 = rd_u8(glyphs), d2 = rd_u8(glyphs);
                    dx = with_sign(f, (d0 << 4) + (d1 >> 4));
                    dy = with_sign(f >> 1, ((d1 & 0x0F) << 8) + d2);
                } else {
                    int d0 = rd_u8(glyphs), d1 = rd_u8(glyphs), d2 = rd_u8(glyphs), d3 = rd_u8(glyphs);
                    dx = with_sign(f, (d0 << 8) + d1);
                    dy = with_sign(f >> 1, (d2 << 8) + d3);
                }
                x += dx;
                y += dy;
                xs[i] = x;
                ys[i] = y;
            }
            uint16_t ilen = rd_255u16(glyphs);
            if (flags->err || glyphs->err || ilen > instr->len - instr->pos) { ok = false; break; }
            int16_t bb[4] = { 0, 0, 0, 0 };
            if (has_bbox) {
                for (int k = 0; k < 4; k++) bb[k] = (int16_t)rd_u16(bbox);
            } else if (total) {
                int32_t x0 = xs[0], y0 = ys[0], x1 = xs[0], y1 = ys[0];
                for (uint32_t i = 1; i < total; i++) {
                    if (xs[i] < x0) x0 = xs[i];
                    if (xs[i] > x1) x1 = xs[i];
                    if (ys[i] < y0) y0 = ys[i];
                    if (ys[i] > y1) y1 = ys[i];
                }
                bb[0] = (int16_t)x0; bb[1] = (int16_t)y0; bb[2] = (int16_t)x1; bb[3] = (int16_t)y1;
            }
            (*xmins)[g] = bb[0];
            buf_u16(glyf, (uint16_t)nc);
            for (int k = 0; k < 4; k++) buf_u16(glyf, (uint16_t)bb[k]);
            for (int c = 0; c < nc; c++) buf_u16(glyf, end_pts[c]);
            buf_u16(glyf, ilen);
            buf_bytes(glyf, instr->p + instr->pos, ilen);
            instr->pos += ilen;
            bool ov = overlap && (overlap[g >> 3] & (0x80 >> (g & 7)));
            /* Flags: on-curve bit only, so both coordinates are int16 deltas. */
            for (uint32_t i = 0; i < total; i++)
                buf_u8(glyf, (uint8_t)((on[i] ? 1 : 0) | (ov && i == 0 ? 0x40 : 0)));
            int32_t prev = 0;
            for (uint32_t i = 0; i < total; i++) { buf_u16(glyf, (uint16_t)(int16_t)(xs[i] - prev)); prev = xs[i]; }
            prev = 0;
            for (uint32_t i = 0; i < total; i++) { buf_u16(glyf, (uint16_t)(int16_t)(ys[i] - prev)); prev = ys[i]; }
        } else {
            /* Composite: components are stored verbatim in the composite
             * stream; the bbox is always explicit. */
            if (!has_bbox) { ok = false; break; }
            int16_t bb[4];
            for (int k = 0; k < 4; k++) bb[k] = (int16_t)rd_u16(bbox);
            (*xmins)[g] = bb[0];
            size_t cstart = composite->pos;
            bool more = true, have_instr = false;
            while (more && !composite->err) {
                uint16_t cf = rd_u16(composite);
                rd_u16(composite);                       /* glyphIndex */
                size_t skip = (cf & 0x0001) ? 4 : 2;
                if (cf & 0x0008) skip += 2;
                else if (cf & 0x0040) skip += 4;
                else if (cf & 0x0080) skip += 8;
                if (skip > composite->len - composite->pos) composite->err = true;
                else composite->pos += skip;
                if (cf & 0x0100) have_instr = true;
                more = (cf & 0x0020) != 0;
            }
            if (composite->err) { ok = false; break; }
            buf_u16(glyf, (uint16_t)nc);
            for (int k = 0; k < 4; k++) buf_u16(glyf, (uint16_t)bb[k]);
            buf_bytes(glyf, composite->p + cstart, composite->pos - cstart);
            if (have_instr) {
                uint16_t ilen = rd_255u16(glyphs);
                if (glyphs->err || ilen > instr->len - instr->pos) { ok = false; break; }
                buf_u16(glyf, ilen);
                buf_bytes(glyf, instr->p + instr->pos, ilen);
                instr->pos += ilen;
            }
        }
        if (bbox->err) ok = false;
        (void)start;
        buf_pad4(glyf);
    }
    buf_u32(loca, (uint32_t)glyf->len);
    free(xs);
    free(ys);
    free(on);
    free(end_pts);
    if (glyf->err || loca->err) ok = false;
    *out_num_glyphs = num_glyphs;
    return ok;
}

/* xMin of each glyph from an untransformed glyf / loca pair. */
static int16_t *glyf_xmins(const W2Table *glyf, const W2Table *loca, int loca_format, uint16_t num_glyphs)
{
    int16_t *xm = (int16_t *)calloc(num_glyphs ? num_glyphs : 1, sizeof(int16_t));
    if (!xm || !glyf || !loca) return xm;
    for (uint32_t g = 0; g < num_glyphs; g++) {
        size_t a, b;
        if (loca_format) {
            if ((g + 2) * 4 > loca->orig_len) break;
            const uint8_t *p = loca->data + g * 4;
            a = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
            b = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
        } else {
            if ((g + 2) * 2 > loca->orig_len) break;
            a = (size_t)get_u16(loca->data + g * 2) * 2;
            b = (size_t)get_u16(loca->data + g * 2 + 2) * 2;
        }
        if (b > a && a + 4 <= glyf->orig_len) xm[g] = (int16_t)get_u16(glyf->data + a + 2);
    }
    return xm;
}

/* Rebuild hmtx from its transformed form (WOFF2 §5.4). */
static bool woff2_rebuild_hmtx(const uint8_t *src, size_t len, uint16_t num_glyphs,
                               uint16_t num_hmetrics, const int16_t *xmins, Buf *out)
{
    Rd r = rd_make(src, len);
    uint8_t fl = rd_u8(&r);
    if (num_hmetrics == 0 || num_hmetrics > num_glyphs) return false;
    uint16_t *adv = (uint16_t *)calloc(num_hmetrics, sizeof(*adv));
    if (!adv) return false;
    for (int i = 0; i < num_hmetrics; i++) adv[i] = rd_u16(&r);
    for (int i = 0; i < num_hmetrics; i++) {
        int16_t lsb = (fl & 1) ? xmins[i] : (int16_t)rd_u16(&r);
        buf_u16(out, adv[i]);
        buf_u16(out, (uint16_t)lsb);
    }
    for (int i = num_hmetrics; i < num_glyphs; i++) {
        int16_t lsb = (fl & 2) ? xmins[i] : (int16_t)rd_u16(&r);
        buf_u16(out, (uint16_t)lsb);
    }
    free(adv);
    return !r.err && !out->err;
}

static bool woff2_decode(const uint8_t *data, size_t len, uint8_t **out, size_t *out_len)
{
    Rd r = rd_make(data, len);
    rd_u32(&r);                                   /* 'wOF2' */
    uint32_t flavor = rd_u32(&r);
    rd_u32(&r);                                   /* length */
    uint16_t ntables = rd_u16(&r);
    rd_u16(&r);                                   /* reserved */
    rd_u32(&r);                                   /* totalSfntSize */
    uint32_t comp_size = rd_u32(&r);
    r.pos = 48;
    if (r.err || ntables == 0 || ntables > 512 || flavor == TAG('t', 't', 'c', 'f')) return false;

    W2Table *t = (W2Table *)calloc(ntables, sizeof(*t));
    if (!t) return false;
    size_t total = 0;
    bool ok = true;
    for (int i = 0; i < ntables && ok; i++) {
        uint8_t fl = rd_u8(&r);
        uint32_t tag = (fl & 0x3F) == 0x3F ? rd_u32(&r) : k_woff2_tags[fl & 0x3F];
        int version = fl >> 6;
        t[i].tag = tag;
        t[i].orig_len = rd_base128(&r);
        bool is_gl = tag == TAG('g', 'l', 'y', 'f') || tag == TAG('l', 'o', 'c', 'a');
        t[i].transformed = is_gl ? version == 0 : (tag == TAG('h', 'm', 't', 'x') && version == 1);
        t[i].stored_len = t[i].transformed ? rd_base128(&r) : t[i].orig_len;
        if (r.err || t[i].stored_len > (64u << 20)) ok = false;
        total += t[i].stored_len;
    }
    uint8_t *stream = NULL;
    if (ok && (comp_size > len - r.pos || total > (128u << 20))) ok = false;
    if (ok) {
        stream = (uint8_t *)malloc(total ? total : 1);
        size_t dlen = total;
        ok = stream && BrotliDecoderDecompress(comp_size, data + r.pos, &dlen, stream) ==
                           BROTLI_DECODER_RESULT_SUCCESS && dlen == total;
    }
    W2Table *glyf = NULL, *loca = NULL, *hmtx = NULL, *head = NULL, *hhea = NULL, *maxp = NULL;
    if (ok) {
        size_t off = 0;
        for (int i = 0; i < ntables; i++) {
            t[i].data = stream + off;
            off += t[i].stored_len;
            if (t[i].tag == TAG('g', 'l', 'y', 'f')) glyf = &t[i];
            else if (t[i].tag == TAG('l', 'o', 'c', 'a')) loca = &t[i];
            else if (t[i].tag == TAG('h', 'm', 't', 'x')) hmtx = &t[i];
            else if (t[i].tag == TAG('h', 'e', 'a', 'd')) head = &t[i];
            else if (t[i].tag == TAG('h', 'h', 'e', 'a')) hhea = &t[i];
            else if (t[i].tag == TAG('m', 'a', 'x', 'p')) maxp = &t[i];
        }
        if (glyf && loca && glyf->transformed != loca->transformed) ok = false;
    }

    Buf gbuf = { 0 }, lbuf = { 0 }, hbuf = { 0 };
    int16_t *xmins = NULL;
    uint16_t num_glyphs = maxp && maxp->orig_len >= 6 ? get_u16(maxp->data + 4) : 0;
    int loca_format = -1;
    if (ok && glyf && glyf->transformed) {
        uint16_t ng = 0;
        ok = woff2_rebuild_glyf(glyf->data, glyf->stored_len, &gbuf, &lbuf, &ng, &xmins);
        if (ok) {
            glyf->data = gbuf.p; glyf->orig_len = glyf->stored_len = (uint32_t)gbuf.len;
            loca->data = lbuf.p; loca->orig_len = loca->stored_len = (uint32_t)lbuf.len;
            if (!num_glyphs) num_glyphs = ng;
            loca_format = 1;
        }
    }
    if (ok && hmtx && hmtx->transformed) {
        if (!xmins) {
            int lf = head && head->orig_len >= 54 ? (int16_t)get_u16(head->data + 50) : 0;
            xmins = glyf_xmins(glyf, loca, lf, num_glyphs);
        }
        uint16_t nhm = hhea && hhea->orig_len >= 36 ? get_u16(hhea->data + 34) : 0;
        ok = xmins && woff2_rebuild_hmtx(hmtx->data, hmtx->stored_len, num_glyphs, nhm, xmins, &hbuf);
        if (ok) { hmtx->data = hbuf.p; hmtx->orig_len = hmtx->stored_len = (uint32_t)hbuf.len; }
    }

    if (ok) {
        SfntTable *st = (SfntTable *)calloc(ntables, sizeof(*st));
        ok = st != NULL;
        for (int i = 0; ok && i < ntables; i++) {
            st[i].tag = t[i].tag;
            st[i].data = t[i].data;
            st[i].len = t[i].orig_len;
        }
        if (ok) ok = sfnt_build(flavor, st, ntables, loca_format, out, out_len);
        free(st);
    }
    free(gbuf.p);
    free(lbuf.p);
    free(hbuf.p);
    free(xmins);
    free(stream);
    free(t);
    return ok;
}

bool font_decode_web(const uint8_t *data, size_t len, uint8_t **out, size_t *out_len)
{
    if (!data || len < 12 || !out || !out_len) return false;
    uint32_t sig = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
    if (sig == TAG('w', 'O', 'F', '2')) return len >= 48 && woff2_decode(data, len, out, out_len);
    if (sig == TAG('w', 'O', 'F', 'F')) return len >= 44 && woff1_decode(data, len, out, out_len);
    if (sig == 0x00010000u || sig == TAG('O', 'T', 'T', 'O') || sig == TAG('t', 'r', 'u', 'e') ||
        sig == TAG('t', 't', 'c', 'f')) {
        *out = (uint8_t *)malloc(len);
        if (!*out) return false;
        memcpy(*out, data, len);
        *out_len = len;
        return true;
    }
    return false;
}

bool font_sfnt_build(uint32_t flavor, FontSfntTable *tables, int n, int loca_format,
                     uint8_t **out, size_t *out_len)
{
    return sfnt_build(flavor, tables, n, loca_format, out, out_len);
}

/* Variable font instancing (TrueType outlines).
 *
 * A variable font stores its default outlines in glyf and, in gvar, per-glyph
 * point deltas for regions of the design space (the fvar axes, optionally
 * remapped by avar).  An instance at given axis values is the default
 * outline plus each tuple's deltas scaled by how strongly its region
 * applies; points a tuple leaves out get interpolated deltas ("IUP").  Four
 * phantom points after the outline carry the horizontal origin and advance,
 * so widths vary too.  The result is written as a static font: glyf / loca /
 * hmtx rebuilt (plain int16 coordinates, long loca, no hinting), the
 * variation tables dropped.  CFF2 fonts are not handled. */

#include "font_loader.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TAG FONT_AXIS_TAG

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

typedef struct {
    uint8_t *p;
    size_t len, cap;
    bool err;
} Buf;

static void buf_bytes(Buf *b, const void *src, size_t n)
{
    if (!n || b->err) return;
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 65536;
        while (nc < b->len + n) nc *= 2;
        uint8_t *np = (uint8_t *)realloc(b->p, nc);
        if (!np) { b->err = true; return; }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, src, n);
    b->len += n;
}

static void buf_u8(Buf *b, uint8_t v) { buf_bytes(b, &v, 1); }
static void buf_u16(Buf *b, uint16_t v) { uint8_t t[2] = { (uint8_t)(v >> 8), (uint8_t)v }; buf_bytes(b, t, 2); }
static void buf_u32(Buf *b, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    buf_bytes(b, t, 4);
}
static void buf_pad4(Buf *b) { while (!b->err && (b->len & 3)) buf_u8(b, 0); }

static uint16_t get_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static float f2dot14(const uint8_t *p) { return (float)(int16_t)get_u16(p) / 16384.0f; }
static float fixed1616(const uint8_t *p) { return (float)(int32_t)get_u32(p) / 65536.0f; }

typedef struct {
    const uint8_t *p;
    uint32_t len;
} TableRef;

static TableRef sfnt_table(const uint8_t *sfnt, size_t len, uint32_t tag)
{
    TableRef t = { NULL, 0 };
    if (len < 12) return t;
    int n = get_u16(sfnt + 4);
    for (int i = 0; i < n && 12 + (size_t)(i + 1) * 16 <= len; i++) {
        const uint8_t *r = sfnt + 12 + i * 16;
        uint32_t off = get_u32(r + 8), tl = get_u32(r + 12);
        if (get_u32(r) == tag && off <= len && tl <= len - off) { t.p = sfnt + off; t.len = tl; return t; }
    }
    return t;
}

/* ---- Design space ---- */

typedef struct {
    uint32_t tag;
    float min, def, max;
    float coord;                 /* normalized, -1..1 */
} VarAxis;

/* Axes from fvar, the requested user values normalized and avar-mapped. */
static int var_axes(const uint8_t *sfnt, size_t len, const FontAxisValue *want, int nwant, VarAxis *axes, int cap)
{
    TableRef fvar = sfnt_table(sfnt, len, TAG('f', 'v', 'a', 'r'));
    if (fvar.len < 16) return 0;
    int aoff = get_u16(fvar.p + 4), count = get_u16(fvar.p + 8), asize = get_u16(fvar.p + 10);
    if (count > cap || asize < 20 || (size_t)aoff + (size_t)count * asize > fvar.len) return 0;
    for (int i = 0; i < count; i++) {
        const uint8_t *a = fvar.p + aoff + i * asize;
        VarAxis *x = &axes[i];
        x->tag = get_u32(a);
        x->min = fixed1616(a + 4);
        x->def = fixed1616(a + 8);
        x->max = fixed1616(a + 12);
        float v = x->def;
        for (int k = 0; k < nwant; k++) if (want[k].tag == x->tag) v = want[k].value;
        if (v < x->min) v = x->min;
        if (v > x->max) v = x->max;
        if (v < x->def) x->coord = x->def > x->min ? (v - x->def) / (x->def - x->min) : 0.0f;
        else x->coord = x->max > x->def ? (v - x->def) / (x->max - x->def) : 0.0f;
    }
    TableRef avar = sfnt_table(sfnt, len, TAG('a', 'v', 'a', 'r'));
    if (avar.len >= 8 && get_u16(avar.p + 6) == count) {
        size_t off = 8;
        for (int i = 0; i < count && off + 2 <= avar.len; i++) {
            int pairs = get_u16(avar.p + off);
            off += 2;
            if (off + (size_t)pairs * 4 > avar.len) break;
            const uint8_t *m = avar.p + off;
            float c = axes[i].coord;
            for (int k = 1; k < pairs; k++) {
                float f0 = f2dot14(m + (k - 1) * 4), t0 = f2dot14(m + (k - 1) * 4 + 2);
                float f1 = f2dot14(m + k * 4), t1 = f2dot14(m + k * 4 + 2);
                if (c >= f0 && c <= f1) {
                    axes[i].coord = f1 > f0 ? t0 + (c - f0) / (f1 - f0) * (t1 - t0) : t0;
                    break;
                }
            }
            off += (size_t)pairs * 4;
        }
    }
    return count;
}

/* How strongly a tuple's region applies at the instance (OpenType "scalar"). */
static float tuple_scalar(const VarAxis *axes, int n, const uint8_t *peak, const uint8_t *start, const uint8_t *end)
{
    float s = 1.0f;
    for (int i = 0; i < n; i++) {
        float p = f2dot14(peak + i * 2), v = axes[i].coord;
        if (p == 0.0f || v == p) continue;
        if (start && end) {
            float a = f2dot14(start + i * 2), b = f2dot14(end + i * 2);
            if (a > p || p > b || (a < 0.0f && b > 0.0f)) continue;   /* invalid region: ignore axis */
            if (v < a || v > b) return 0.0f;
            s *= v < p ? (p > a ? (v - a) / (p - a) : 1.0f) : (b > p ? (b - v) / (b - p) : 1.0f);
        } else {
            if (v == 0.0f || v < (p < 0 ? p : 0) || v > (p > 0 ? p : 0)) return 0.0f;
            s *= v / p;
        }
    }
    return s;
}

/* Packed point numbers; *all = true for "every point".  Count or -1. */
static int read_points(Rd *r, uint16_t *out, int cap, bool *all)
{
    int count = rd_u8(r);
    if (count & 0x80) count = ((count & 0x7F) << 8) | rd_u8(r);
    *all = count == 0;
    if (count == 0) return 0;
    int k = 0, last = 0;
    while (k < count && !r->err) {
        int ctl = rd_u8(r);
        int run = (ctl & 0x7F) + 1;
        for (int i = 0; i < run && k < count; i++) {
            last += (ctl & 0x80) ? rd_u16(r) : rd_u8(r);
            if (k < cap) out[k] = (uint16_t)last;
            k++;
        }
    }
    return r->err || count > cap ? -1 : count;
}

/* Packed deltas (zero / byte / word / long runs). */
static bool read_deltas(Rd *r, int n, float *out)
{
    int k = 0;
    while (k < n && !r->err) {
        int ctl = rd_u8(r);
        int run = (ctl & 0x3F) + 1;
        for (int i = 0; i < run && k < n; i++) {
            int32_t v;
            if ((ctl & 0xC0) == 0xC0) v = (int32_t)rd_u32(r);
            else if (ctl & 0x80) v = 0;
            else if (ctl & 0x40) v = (int16_t)rd_u16(r);
            else v = (int8_t)rd_u8(r);
            out[k++] = (float)v;
        }
    }
    return !r->err;
}

/* Interpolate the deltas of untouched points of one contour [start, end]
 * from their touched neighbours, per coordinate (gvar "IUP"). */
static void iup_contour(const int32_t *c, float *d, const bool *touched, int start, int end)
{
    int first = -1;
    for (int i = start; i <= end; i++) if (touched[i]) { first = i; break; }
    if (first < 0) return;
    int a = first;
    for (;;) {
        int b = a;
        do { b = b == end ? start : b + 1; } while (!touched[b] && b != first);
        for (int p = a == end ? start : a + 1; p != b; p = p == end ? start : p + 1) {
            int32_t ca = c[a], cb = c[b], cp = c[p];
            float da = d[a], db = d[b];
            if (ca == cb) {
                d[p] = da == db ? da : 0.0f;
            } else {
                int32_t lo = ca < cb ? ca : cb, hi = ca < cb ? cb : ca;
                float dlo = ca < cb ? da : db, dhi = ca < cb ? db : da;
                if (cp <= lo) d[p] = dlo;
                else if (cp >= hi) d[p] = dhi;
                else d[p] = dlo + (float)(cp - lo) / (float)(hi - lo) * (dhi - dlo);
            }
        }
        if (b == first) break;
        a = b;
    }
}

/* ---- HVAR: advance-width variations ----
 * An ItemVariationStore (regions x per-item deltas) plus an optional map from
 * glyph id to (outer, inner) item; without the map, outer 0 / inner gid. */

typedef struct {
    TableRef t;
    uint32_t store, map;
    float *region_scalar;         /* one per region at the instance */
    int nregions;
} Hvar;

static bool hvar_open(Hvar *hv, const uint8_t *sfnt, size_t len, const VarAxis *axes, int naxes)
{
    memset(hv, 0, sizeof(*hv));
    hv->t = sfnt_table(sfnt, len, TAG('H', 'V', 'A', 'R'));
    if (hv->t.len < 20) return false;
    const uint8_t *h = hv->t.p;
    hv->store = get_u32(h + 4);
    hv->map = get_u32(h + 8);
    if (hv->store + 8 > hv->t.len) return false;
    const uint8_t *st = h + hv->store;
    uint32_t rl = get_u32(st + 2);
    if ((size_t)hv->store + rl + 4 > hv->t.len) return false;
    const uint8_t *regions = st + rl;
    int ax = get_u16(regions), nr = get_u16(regions + 2);
    if (ax != naxes || (size_t)hv->store + rl + 4 + (size_t)nr * ax * 6 > hv->t.len) return false;
    hv->region_scalar = (float *)calloc((size_t)nr + 1, sizeof(float));
    if (!hv->region_scalar) return false;
    hv->nregions = nr;
    for (int r = 0; r < nr; r++) {
        float s = 1.0f;
        for (int i = 0; i < ax && s != 0.0f; i++) {
            const uint8_t *e = regions + 4 + ((size_t)r * ax + i) * 6;
            float a = f2dot14(e), p = f2dot14(e + 2), b = f2dot14(e + 4), v = axes[i].coord;
            if (p == 0.0f || v == p) continue;
            if (a > p || p > b || (a < 0.0f && b > 0.0f)) continue;
            if (v < a || v > b) { s = 0.0f; break; }
            s *= v < p ? (p > a ? (v - a) / (p - a) : 1.0f) : (b > p ? (b - v) / (b - p) : 1.0f);
        }
        hv->region_scalar[r] = s;
    }
    return true;
}

/* Advance delta of glyph `gid` in font units. */
static float hvar_advance_delta(const Hvar *hv, int gid)
{
    if (!hv->region_scalar) return 0.0f;
    const uint8_t *h = hv->t.p;
    uint32_t len = hv->t.len;
    uint32_t outer = 0, inner = (uint32_t)gid;
    if (hv->map && hv->map + 4 <= len) {
        const uint8_t *m = h + hv->map;
        int fmt = m[0], ef = m[1];
        uint32_t count = fmt == 1 ? get_u32(m + 2) : get_u16(m + 2);
        size_t data = fmt == 1 ? 6 : 4;
        int size = ((ef >> 4) & 3) + 1, ibits = (ef & 0x0F) + 1;
        if (count == 0) return 0.0f;
        uint32_t idx = (uint32_t)gid < count ? (uint32_t)gid : count - 1;
        size_t o = hv->map + data + (size_t)idx * size;
        if (o + size > len) return 0.0f;
        uint32_t entry = 0;
        for (int k = 0; k < size; k++) entry = (entry << 8) | h[o + k];
        outer = entry >> ibits;
        inner = entry & ((1u << ibits) - 1);
    }
    const uint8_t *st = h + hv->store;
    int ndata = get_u16(st + 6);
    if ((int)outer >= ndata || (size_t)hv->store + 8 + (size_t)(outer + 1) * 4 > len) return 0.0f;
    uint32_t doff = hv->store + get_u32(st + 8 + outer * 4);
    if (doff + 6 > len) return 0.0f;
    const uint8_t *d = h + doff;
    int items = get_u16(d), wc = get_u16(d + 2), nri = get_u16(d + 4);
    bool longs = (wc & 0x8000) != 0;
    wc &= 0x7FFF;
    if ((int)inner >= items || wc > nri) return 0.0f;
    size_t row = (size_t)wc * (longs ? 4 : 2) + (size_t)(nri - wc) * (longs ? 2 : 1);
    size_t base = doff + 6 + (size_t)nri * 2;
    if (base + row * items > len) return 0.0f;
    const uint8_t *rp = h + base + row * inner;
    float delta = 0.0f;
    for (int k = 0; k < nri; k++) {
        int ri = get_u16(d + 6 + k * 2);
        int32_t v;
        if (k < wc) { v = longs ? (int32_t)get_u32(rp) : (int16_t)get_u16(rp); rp += longs ? 4 : 2; }
        else { v = longs ? (int16_t)get_u16(rp) : (int8_t)rp[0]; rp += longs ? 2 : 1; }
        if (ri < hv->nregions) delta += hv->region_scalar[ri] * (float)v;
    }
    return delta;
}

/* ---- Glyphs ---- */

typedef struct {
    /* simple glyph */
    int npts, ncont;
    int32_t *x, *y;
    uint8_t *on;
    uint16_t *ends;
    bool overlap;
    /* composite glyph */
    int ncomp;
    uint16_t *cflags, *cglyph;
    int32_t *cdx, *cdy;
    uint8_t (*cxf)[8];               /* raw transform bytes (scale / 2x2) */
    /* state */
    int16_t bb[4];
    bool empty, composite, done;
} VGlyph;

static void vglyph_free(VGlyph *g)
{
    free(g->x); free(g->y); free(g->on); free(g->ends);
    free(g->cflags); free(g->cglyph); free(g->cdx); free(g->cdy); free(g->cxf);
}

static bool parse_simple(const uint8_t *p, size_t len, VGlyph *g)
{
    Rd r = rd_make(p, len);
    int nc = (int16_t)rd_u16(&r);
    for (int k = 0; k < 4; k++) g->bb[k] = (int16_t)rd_u16(&r);
    g->ncont = nc;
    g->ends = (uint16_t *)calloc((size_t)nc + 1, sizeof(uint16_t));
    if (!g->ends) return false;
    for (int c = 0; c < nc; c++) g->ends[c] = rd_u16(&r);
    int n = nc ? g->ends[nc - 1] + 1 : 0;
    uint16_t ilen = rd_u16(&r);
    r.pos += ilen;
    if (r.err || r.pos > len) return false;
    g->npts = n;
    g->x = (int32_t *)calloc((size_t)n + 1, sizeof(int32_t));
    g->y = (int32_t *)calloc((size_t)n + 1, sizeof(int32_t));
    g->on = (uint8_t *)calloc((size_t)n + 1, 1);
    uint8_t *fl = (uint8_t *)calloc((size_t)n + 1, 1);
    if (!g->x || !g->y || !g->on || !fl) { free(fl); return false; }
    for (int i = 0; i < n && !r.err;) {
        uint8_t f = rd_u8(&r);
        fl[i++] = f;
        if (f & 8) { int rep = rd_u8(&r); while (rep-- > 0 && i < n) fl[i++] = f; }
    }
    int32_t v = 0;
    for (int i = 0; i < n; i++) {
        if (fl[i] & 2) { int d = rd_u8(&r); v += (fl[i] & 16) ? d : -d; }
        else if (!(fl[i] & 16)) v += (int16_t)rd_u16(&r);
        g->x[i] = v;
    }
    v = 0;
    for (int i = 0; i < n; i++) {
        if (fl[i] & 4) { int d = rd_u8(&r); v += (fl[i] & 32) ? d : -d; }
        else if (!(fl[i] & 32)) v += (int16_t)rd_u16(&r);
        g->y[i] = v;
        g->on[i] = fl[i] & 1;
    }
    g->overlap = n && (fl[0] & 0x40);
    free(fl);
    return !r.err;
}

static bool parse_composite(const uint8_t *p, size_t len, VGlyph *g)
{
    Rd r = rd_make(p, len);
    rd_u16(&r);
    for (int k = 0; k < 4; k++) g->bb[k] = (int16_t)rd_u16(&r);
    int cap = 0;
    bool more = true;
    while (more && !r.err) {
        if (g->ncomp == cap) {
            cap = cap ? cap * 2 : 8;
            uint16_t *nf = (uint16_t *)realloc(g->cflags, (size_t)cap * sizeof(uint16_t));
            if (nf) g->cflags = nf;
            uint16_t *ngl = (uint16_t *)realloc(g->cglyph, (size_t)cap * sizeof(uint16_t));
            if (ngl) g->cglyph = ngl;
            int32_t *nx = (int32_t *)realloc(g->cdx, (size_t)cap * sizeof(int32_t));
            if (nx) g->cdx = nx;
            int32_t *ny = (int32_t *)realloc(g->cdy, (size_t)cap * sizeof(int32_t));
            if (ny) g->cdy = ny;
            uint8_t (*nt)[8] = realloc(g->cxf, (size_t)cap * sizeof(*g->cxf));
            if (nt) g->cxf = nt;
            if (!nf || !ngl || !nx || !ny || !nt) return false;
        }
        int i = g->ncomp++;
        uint16_t f = rd_u16(&r);
        g->cflags[i] = f;
        g->cglyph[i] = rd_u16(&r);
        if (f & 1) {
            g->cdx[i] = (f & 2) ? (int16_t)rd_u16(&r) : rd_u16(&r);
            g->cdy[i] = (f & 2) ? (int16_t)rd_u16(&r) : rd_u16(&r);
        } else {
            g->cdx[i] = (f & 2) ? (int8_t)rd_u8(&r) : rd_u8(&r);
            g->cdy[i] = (f & 2) ? (int8_t)rd_u8(&r) : rd_u8(&r);
        }
        int xl = (f & 0x0008) ? 2 : (f & 0x0040) ? 4 : (f & 0x0080) ? 8 : 0;
        memset(g->cxf[i], 0, 8);
        for (int k = 0; k < xl; k++) g->cxf[i][k] = rd_u8(&r);
        more = (f & 0x0020) != 0;
    }
    g->composite = true;
    return !r.err && g->ncomp > 0;
}

typedef struct {
    const uint8_t *glyf, *loca, *hmtx;
    uint32_t glyf_len, loca_len, hmtx_len;
    int loca_long, num_glyphs, num_hm;
    TableRef gvar;
    Hvar hvar;                    /* advance deltas, when the font has HVAR */
    bool has_hvar;
    const VarAxis *axes;
    int naxes;
    VGlyph *g;
    int *adv, *lsb;
} Inst;

/* The varied advance: HVAR when present, else the phantom points. */
static int varied_advance(const Inst *in, int gid, int adv, float pp[4][2])
{
    if (in->has_hvar) return (int)floorf((float)adv + hvar_advance_delta(&in->hvar, gid) + 0.5f);
    return (int)floorf(pp[1][0] - pp[0][0] + 0.5f);
}

/* Add every gvar tuple of glyph `gid` to its points and the phantom points. */
static void apply_gvar(const Inst *in, int gid, VGlyph *g, float pp[4][2])
{
    const TableRef *gvar = &in->gvar;
    if (!gvar->p || gvar->len < 20) return;
    const uint8_t *h = gvar->p;
    int axis_count = get_u16(h + 4), shared_count = get_u16(h + 6);
    uint32_t shared_off = get_u32(h + 8);
    int glyph_count = get_u16(h + 12), flags = get_u16(h + 14);
    uint32_t data_off = get_u32(h + 16);
    if (axis_count != in->naxes || gid >= glyph_count) return;
    size_t offs_len = (size_t)(glyph_count + 1) * ((flags & 1) ? 4 : 2);
    if (20 + offs_len > gvar->len) return;
    uint32_t a, b;
    if (flags & 1) { a = get_u32(h + 20 + gid * 4); b = get_u32(h + 24 + gid * 4); }
    else { a = (uint32_t)get_u16(h + 20 + gid * 2) * 2; b = (uint32_t)get_u16(h + 22 + gid * 2) * 2; }
    if (b <= a || (size_t)data_off + b > gvar->len ||
        (size_t)shared_off + (size_t)shared_count * in->naxes * 2 > gvar->len)
        return;
    const uint8_t *gv = h + data_off + a;
    uint32_t gvlen = b - a;
    int naxes = in->naxes;
    int n = g->composite ? g->ncomp : g->npts;
    int total = n + 4;

    int32_t *ox = (int32_t *)calloc((size_t)total, sizeof(int32_t));
    int32_t *oy = (int32_t *)calloc((size_t)total, sizeof(int32_t));
    uint16_t *pts = (uint16_t *)malloc((size_t)total * sizeof(uint16_t));
    uint16_t *shared_pts = (uint16_t *)malloc((size_t)total * sizeof(uint16_t));
    float *dx = (float *)calloc((size_t)total, sizeof(float));
    float *dy = (float *)calloc((size_t)total, sizeof(float));
    float *ax = (float *)calloc((size_t)total, sizeof(float));
    float *ay = (float *)calloc((size_t)total, sizeof(float));
    float *tmp = (float *)malloc((size_t)total * 2 * sizeof(float));
    bool *touched = (bool *)calloc((size_t)total, sizeof(bool));
    if (!ox || !oy || !pts || !shared_pts || !dx || !dy || !ax || !ay || !tmp || !touched) goto done;
    for (int i = 0; i < n; i++) {
        ox[i] = g->composite ? g->cdx[i] : g->x[i];
        oy[i] = g->composite ? g->cdy[i] : g->y[i];
    }
    for (int k = 0; k < 4; k++) { ox[n + k] = (int32_t)pp[k][0]; oy[n + k] = (int32_t)pp[k][1]; }

    Rd hr = rd_make(gv, gvlen);
    uint16_t tcount = rd_u16(&hr);
    uint16_t sdata = rd_u16(&hr);
    Rd dr = rd_make(gv, gvlen);
    dr.pos = sdata;
    bool shared_all = false;
    int nshared = 0;
    if (tcount & 0x8000) nshared = read_points(&dr, shared_pts, total, &shared_all);
    if (nshared < 0) goto done;
    for (int t = 0; t < (tcount & 0x0FFF) && !hr.err; t++) {
        uint16_t size = rd_u16(&hr);
        uint16_t tidx = rd_u16(&hr);
        const uint8_t *peak = NULL, *st = NULL, *en = NULL;
        if (tidx & 0x8000) { peak = gv + hr.pos; hr.pos += (size_t)naxes * 2; }
        else if ((tidx & 0x0FFF) < shared_count) peak = h + shared_off + (size_t)(tidx & 0x0FFF) * naxes * 2;
        if (tidx & 0x4000) {
            st = gv + hr.pos; hr.pos += (size_t)naxes * 2;
            en = gv + hr.pos; hr.pos += (size_t)naxes * 2;
        }
        if (hr.pos > gvlen) break;
        size_t next = dr.pos + size;
        float s = peak ? tuple_scalar(in->axes, naxes, peak, st, en) : 0.0f;
        if (s != 0.0f && next <= gvlen) {
            bool all = shared_all;
            int np = nshared;
            const uint16_t *ids = shared_pts;
            if (tidx & 0x2000) { np = read_points(&dr, pts, total, &all); ids = pts; }
            int cnt = all ? total : np;
            if (np >= 0 && read_deltas(&dr, cnt, tmp) && read_deltas(&dr, cnt, tmp + cnt)) {
                memset(dx, 0, (size_t)total * sizeof(float));
                memset(dy, 0, (size_t)total * sizeof(float));
                memset(touched, 0, (size_t)total * sizeof(bool));
                for (int k = 0; k < cnt; k++) {
                    int pi = all ? k : ids[k];
                    if (pi >= total) continue;
                    dx[pi] = tmp[k];
                    dy[pi] = tmp[cnt + k];
                    touched[pi] = true;
                }
                if (!all && !g->composite) {
                    int s0 = 0;
                    for (int c = 0; c < g->ncont; c++) {
                        int e = g->ends[c];
                        if (e >= n || e < s0) break;
                        iup_contour(ox, dx, touched, s0, e);
                        iup_contour(oy, dy, touched, s0, e);
                        s0 = e + 1;
                    }
                }
                for (int k = 0; k < total; k++) { ax[k] += s * dx[k]; ay[k] += s * dy[k]; }
            }
        }
        dr.pos = next;
    }
    for (int i = 0; i < n; i++) {
        int32_t nx = (int32_t)floorf((float)ox[i] + ax[i] + 0.5f);
        int32_t ny = (int32_t)floorf((float)oy[i] + ay[i] + 0.5f);
        if (g->composite) {
            if (g->cflags[i] & 2) { g->cdx[i] = nx; g->cdy[i] = ny; }   /* point-matched: not offsets */
        }
        else { g->x[i] = nx; g->y[i] = ny; }
    }
    for (int k = 0; k < 4; k++) { pp[k][0] += ax[n + k]; pp[k][1] += ay[n + k]; }
done:
    free(ox); free(oy); free(pts); free(shared_pts); free(dx); free(dy); free(ax); free(ay);
    free(tmp); free(touched);
}

static bool glyph_range(const Inst *in, int gid, uint32_t *a, uint32_t *b)
{
    if (in->loca_long) {
        if ((size_t)(gid + 2) * 4 > in->loca_len) return false;
        *a = get_u32(in->loca + gid * 4);
        *b = get_u32(in->loca + gid * 4 + 4);
    } else {
        if ((size_t)(gid + 2) * 2 > in->loca_len) return false;
        *a = (uint32_t)get_u16(in->loca + gid * 2) * 2;
        *b = (uint32_t)get_u16(in->loca + gid * 2 + 2) * 2;
    }
    return *a <= *b && *b <= in->glyf_len;
}

static void hmetric(const Inst *in, int gid, int *adv, int *lsb)
{
    int nh = in->num_hm;
    *adv = 0;
    *lsb = 0;
    if (nh <= 0) return;
    int ai = gid < nh ? gid : nh - 1;
    if ((size_t)ai * 4 + 2 <= in->hmtx_len) *adv = get_u16(in->hmtx + ai * 4);
    if (gid < nh) {
        if ((size_t)gid * 4 + 4 <= in->hmtx_len) *lsb = (int16_t)get_u16(in->hmtx + gid * 4 + 2);
    } else {
        size_t o = (size_t)nh * 4 + (size_t)(gid - nh) * 2;
        if (o + 2 <= in->hmtx_len) *lsb = (int16_t)get_u16(in->hmtx + o);
    }
}

/* Vary glyph `gid` (composites after their components, for their bbox). */
static void inst_glyph(Inst *in, int gid, int depth)
{
    VGlyph *g = &in->g[gid];
    if (g->done || depth > 8) return;
    g->done = true;
    int adv, lsb;
    hmetric(in, gid, &adv, &lsb);
    uint32_t a, b;
    if (!glyph_range(in, gid, &a, &b) || b - a < 10) {
        g->empty = true;
        float pp[4][2] = { { 0, 0 }, { (float)adv, 0 }, { 0, 0 }, { 0, 0 } };
        apply_gvar(in, gid, g, pp);
        in->adv[gid] = varied_advance(in, gid, adv, pp);
        in->lsb[gid] = 0;
        return;
    }
    int nc = (int16_t)get_u16(in->glyf + a);
    bool ok = nc >= 0 ? parse_simple(in->glyf + a, b - a, g) : parse_composite(in->glyf + a, b - a, g);
    if (!ok) { g->empty = true; in->adv[gid] = adv; in->lsb[gid] = 0; return; }
    float x0 = (float)(g->bb[0] - lsb);
    float pp[4][2] = { { x0, 0 }, { x0 + (float)adv, 0 }, { 0, 0 }, { 0, 0 } };
    apply_gvar(in, gid, g, pp);
    int shift = in->has_hvar ? 0 : (int)floorf(pp[0][0] - x0 + 0.5f);   /* the varied origin */
    in->adv[gid] = varied_advance(in, gid, adv, pp);
    if (!g->composite) {
        int32_t bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
        for (int i = 0; i < g->npts; i++) {
            g->x[i] -= shift;
            if (i == 0 || g->x[i] < bx0) bx0 = g->x[i];
            if (i == 0 || g->x[i] > bx1) bx1 = g->x[i];
            if (i == 0 || g->y[i] < by0) by0 = g->y[i];
            if (i == 0 || g->y[i] > by1) by1 = g->y[i];
        }
        g->bb[0] = (int16_t)bx0; g->bb[1] = (int16_t)by0; g->bb[2] = (int16_t)bx1; g->bb[3] = (int16_t)by1;
    } else {
        bool any = false;
        int32_t bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
        for (int i = 0; i < g->ncomp; i++) {
            uint16_t f = g->cflags[i];
            if (f & 2) g->cdx[i] -= shift;                  /* offsets, not point indices */
            int cg = g->cglyph[i];
            if (cg >= in->num_glyphs) continue;
            inst_glyph(in, cg, depth + 1);
            const VGlyph *c = &in->g[cg];
            if (c->empty) continue;
            float m00 = 1, m01 = 0, m10 = 0, m11 = 1;
            if (f & 0x0008) m00 = m11 = f2dot14(g->cxf[i]);
            else if (f & 0x0040) { m00 = f2dot14(g->cxf[i]); m11 = f2dot14(g->cxf[i] + 2); }
            else if (f & 0x0080) {
                m00 = f2dot14(g->cxf[i]); m01 = f2dot14(g->cxf[i] + 2);
                m10 = f2dot14(g->cxf[i] + 4); m11 = f2dot14(g->cxf[i] + 6);
            }
            float cx[2] = { c->bb[0], c->bb[2] }, cy[2] = { c->bb[1], c->bb[3] };
            for (int u = 0; u < 2; u++)
                for (int v = 0; v < 2; v++) {
                    int32_t px = (int32_t)floorf(cx[u] * m00 + cy[v] * m10 + 0.5f) + ((f & 2) ? g->cdx[i] : 0);
                    int32_t py = (int32_t)floorf(cx[u] * m01 + cy[v] * m11 + 0.5f) + ((f & 2) ? g->cdy[i] : 0);
                    if (!any || px < bx0) bx0 = px;
                    if (!any || px > bx1) bx1 = px;
                    if (!any || py < by0) by0 = py;
                    if (!any || py > by1) by1 = py;
                    any = true;
                }
        }
        if (any) { g->bb[0] = (int16_t)bx0; g->bb[1] = (int16_t)by0; g->bb[2] = (int16_t)bx1; g->bb[3] = (int16_t)by1; }
    }
    in->lsb[gid] = g->bb[0];
}

static void write_glyph(Buf *out, const VGlyph *g)
{
    if (g->empty) return;
    if (!g->composite) {
        buf_u16(out, (uint16_t)g->ncont);
        for (int k = 0; k < 4; k++) buf_u16(out, (uint16_t)g->bb[k]);
        for (int c = 0; c < g->ncont; c++) buf_u16(out, g->ends[c]);
        buf_u16(out, 0);                                    /* no instructions */
        for (int i = 0; i < g->npts; i++)
            buf_u8(out, (uint8_t)((g->on[i] ? 1 : 0) | (g->overlap && i == 0 ? 0x40 : 0)));
        int32_t prev = 0;
        for (int i = 0; i < g->npts; i++) { buf_u16(out, (uint16_t)(int16_t)(g->x[i] - prev)); prev = g->x[i]; }
        prev = 0;
        for (int i = 0; i < g->npts; i++) { buf_u16(out, (uint16_t)(int16_t)(g->y[i] - prev)); prev = g->y[i]; }
    } else {
        buf_u16(out, 0xFFFF);
        for (int k = 0; k < 4; k++) buf_u16(out, (uint16_t)g->bb[k]);
        for (int i = 0; i < g->ncomp; i++) {
            uint16_t f = (uint16_t)((g->cflags[i] | 0x0001) & ~0x0100);   /* word args, no instructions */
            if (i + 1 < g->ncomp) f |= 0x0020;
            else f &= (uint16_t)~0x0020;
            buf_u16(out, f);
            buf_u16(out, g->cglyph[i]);
            buf_u16(out, (uint16_t)g->cdx[i]);
            buf_u16(out, (uint16_t)g->cdy[i]);
            int xl = (f & 0x0008) ? 2 : (f & 0x0040) ? 4 : (f & 0x0080) ? 8 : 0;
            buf_bytes(out, g->cxf[i], (size_t)xl);
        }
    }
    buf_pad4(out);
}

/* ---- Public ---- */

bool font_is_variable(const uint8_t *sfnt, size_t len)
{
    return sfnt && sfnt_table(sfnt, len, TAG('f', 'v', 'a', 'r')).p && sfnt_table(sfnt, len, TAG('g', 'v', 'a', 'r')).p;
}

bool font_axis_range(const uint8_t *sfnt, size_t len, uint32_t tag, float *min, float *def, float *max)
{
    VarAxis axes[32];
    int n = sfnt ? var_axes(sfnt, len, NULL, 0, axes, 32) : 0;
    for (int i = 0; i < n; i++) {
        if (axes[i].tag != tag) continue;
        if (min) *min = axes[i].min;
        if (def) *def = axes[i].def;
        if (max) *max = axes[i].max;
        return true;
    }
    return false;
}

bool font_instance(const uint8_t *sfnt, size_t len, const FontAxisValue *values, int nvalues,
                   uint8_t **out, size_t *out_len)
{
    if (!sfnt || len < 12 || !out || !out_len || get_u32(sfnt) == TAG('t', 't', 'c', 'f')) return false;
    VarAxis axes[32];
    int naxes = var_axes(sfnt, len, values, nvalues, axes, 32);
    TableRef glyf = sfnt_table(sfnt, len, TAG('g', 'l', 'y', 'f'));
    TableRef loca = sfnt_table(sfnt, len, TAG('l', 'o', 'c', 'a'));
    TableRef head = sfnt_table(sfnt, len, TAG('h', 'e', 'a', 'd'));
    TableRef hhea = sfnt_table(sfnt, len, TAG('h', 'h', 'e', 'a'));
    TableRef hmtx = sfnt_table(sfnt, len, TAG('h', 'm', 't', 'x'));
    TableRef maxp = sfnt_table(sfnt, len, TAG('m', 'a', 'x', 'p'));
    if (naxes <= 0 || !glyf.p || !loca.p || head.len < 54 || hhea.len < 36 || !hmtx.p || maxp.len < 6)
        return false;

    Inst in;
    memset(&in, 0, sizeof(in));
    in.glyf = glyf.p; in.glyf_len = glyf.len;
    in.loca = loca.p; in.loca_len = loca.len;
    in.hmtx = hmtx.p; in.hmtx_len = hmtx.len;
    in.loca_long = (int16_t)get_u16(head.p + 50) != 0;
    in.num_glyphs = get_u16(maxp.p + 4);
    in.num_hm = get_u16(hhea.p + 34);
    in.gvar = sfnt_table(sfnt, len, TAG('g', 'v', 'a', 'r'));
    in.has_hvar = hvar_open(&in.hvar, sfnt, len, axes, naxes);
    in.axes = axes;
    in.naxes = naxes;
    int ng = in.num_glyphs;
    in.g = (VGlyph *)calloc((size_t)ng + 1, sizeof(VGlyph));
    in.adv = (int *)calloc((size_t)ng + 1, sizeof(int));
    in.lsb = (int *)calloc((size_t)ng + 1, sizeof(int));
    bool ok = in.g && in.adv && in.lsb;
    for (int gid = 0; ok && gid < ng; gid++) inst_glyph(&in, gid, 0);

    Buf gbuf = { 0 }, lbuf = { 0 }, hbuf = { 0 };
    uint8_t *head2 = NULL, *hhea2 = NULL;
    if (ok) {
        for (int gid = 0; gid < ng; gid++) {
            buf_u32(&lbuf, (uint32_t)gbuf.len);
            write_glyph(&gbuf, &in.g[gid]);
        }
        buf_u32(&lbuf, (uint32_t)gbuf.len);
        int maxadv = 0;
        for (int gid = 0; gid < ng; gid++) {
            int av = in.adv[gid] < 0 ? 0 : in.adv[gid];
            if (av > maxadv) maxadv = av;
            buf_u16(&hbuf, (uint16_t)av);
            buf_u16(&hbuf, (uint16_t)(int16_t)in.lsb[gid]);
        }
        head2 = (uint8_t *)malloc(head.len);
        hhea2 = (uint8_t *)malloc(hhea.len);
        ok = !gbuf.err && !lbuf.err && !hbuf.err && head2 && hhea2;
        if (ok) {
            memcpy(head2, head.p, head.len);
            memcpy(hhea2, hhea.p, hhea.len);
            put_u16(hhea2 + 10, (uint16_t)maxadv);
            put_u16(hhea2 + 34, (uint16_t)ng);            /* every glyph gets a long metric */
        }
    }
    if (ok) {
        static const uint32_t drop[] = {
            TAG('f', 'v', 'a', 'r'), TAG('g', 'v', 'a', 'r'), TAG('a', 'v', 'a', 'r'), TAG('H', 'V', 'A', 'R'),
            TAG('V', 'V', 'A', 'R'), TAG('M', 'V', 'A', 'R'), TAG('c', 'v', 'a', 'r'), TAG('S', 'T', 'A', 'T'),
            TAG('h', 'd', 'm', 'x'), TAG('L', 'T', 'S', 'H'), TAG('V', 'D', 'M', 'X'),
        };
        int n = get_u16(sfnt + 4);
        FontSfntTable *tables = (FontSfntTable *)calloc((size_t)n + 1, sizeof(*tables));
        int k = 0;
        for (int i = 0; tables && i < n && 12 + (size_t)(i + 1) * 16 <= len; i++) {
            const uint8_t *r = sfnt + 12 + i * 16;
            uint32_t tag = get_u32(r), off = get_u32(r + 8), tl = get_u32(r + 12);
            bool skip = off > len || tl > len - off;
            for (size_t d = 0; d < sizeof(drop) / sizeof(drop[0]); d++) if (tag == drop[d]) skip = true;
            if (skip) continue;
            tables[k].tag = tag;
            if (tag == TAG('g', 'l', 'y', 'f')) { tables[k].data = gbuf.p; tables[k].len = gbuf.len; }
            else if (tag == TAG('l', 'o', 'c', 'a')) { tables[k].data = lbuf.p; tables[k].len = lbuf.len; }
            else if (tag == TAG('h', 'm', 't', 'x')) { tables[k].data = hbuf.p; tables[k].len = hbuf.len; }
            else if (tag == TAG('h', 'e', 'a', 'd')) { tables[k].data = head2; tables[k].len = head.len; }
            else if (tag == TAG('h', 'h', 'e', 'a')) { tables[k].data = hhea2; tables[k].len = hhea.len; }
            else { tables[k].data = sfnt + off; tables[k].len = tl; }
            k++;
        }
        ok = tables && font_sfnt_build(get_u32(sfnt), tables, k, 1, out, out_len);
        free(tables);
    }
    for (int gid = 0; in.g && gid < ng; gid++) vglyph_free(&in.g[gid]);
    free(in.g); free(in.adv); free(in.lsb); free(in.hvar.region_scalar);
    free(gbuf.p); free(lbuf.p); free(hbuf.p); free(head2); free(hhea2);
    return ok;
}

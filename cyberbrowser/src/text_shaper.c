#include "text_shaper.h"

#include <stdatomic.h>
#include <stdbool.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdlib.h>
#include "bidi.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#include <pthread.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "font_registry.h"

#include "glyph_atlas.h"

/* The tab's glyph atlas: pages of 1024 x 1024, 4 of them at most. */
#define TS_ATLAS_PAGE   1024
#define TS_ATLAS_BUDGET 4

struct TextShaper {
    uint8_t        *ttf_data;
    size_t          ttf_size;
    bool            ttf_mapped;    /* a view of the font file, not malloc()ed */
    int             font_offset;
    int             embolden;      /* synthetic bold: extra pixels per glyph */
    stbtt_fontinfo  font;
    const uint8_t  *colr, *cpal;   /* (the color font tables, NULL none) */
    uint32_t        colr_len, cpal_len;
    float           size_pixels;
    float           scale;
    float           ascent;
    float           descent;
    float           line_gap;
    float           baseline;

    /* Thread-safe metrics (PARALLEL_STYLE_LAYOUT.md §8): measurement never
     * touches the atlas.  Both tables are insert-only and lock-free:
     *   metric_pages[cp >> 8][cp & 255] = (glyph_index + 1) << 32 | advance bits
     *   kern[] = ((g1 << 16 | g2) + 1) << 32 | (uint32_t)kern  (open addressing) */
    _Atomic(_Atomic uint64_t *) *metric_pages;
    _Atomic uint64_t *kern;
};

#define TS_METRIC_PAGES (0x110000 >> 8)
#define TS_KERN_SLOTS   (1u << 15)
#define TS_KERN_PROBES  16

/* ---- Strikes (FONT_PIPELINE_DESIGN.md) ----
 *
 * A strike is a font drawn at one size (in quarter pixels) with its
 * synthetic bold: per glyph drawn, the bitmap's box at that size and where
 * the bitmap is in the glyph atlas (until evicted; drawn again then).
 * Drawing happens on one thread (the display list's); measuring never comes
 * here. */

typedef struct {
    int      glyph_index;   /* -1: a free cell of the table */
    int16_t  xoff, yoff;
    uint16_t bw, bh;
    GaRef    ref;           /* id -1: not in the atlas */
} StrikeGlyph;

typedef struct Strike {
    const TextShaper *face;
    int          size_q;
    int          embolden;
    float        scale;
    StrikeGlyph *glyphs;    /* open addressing by glyph index */
    int          count, cap;
    struct Strike *next;    /* in its hash bucket */
} Strike;

#define TS_STRIKE_BUCKETS 1024
static Strike *g_strikes[TS_STRIKE_BUCKETS];
static GlyphAtlas *g_atlas;

static uint32_t utf8_decode(const char **ptr)
{
    const unsigned char *p = (const unsigned char *)*ptr;
    uint32_t c = *p++;
    if (c < 0x80) {
        *ptr = (const char *)p;
        /* (a control character other than tab and line feed: visible, as a
         * box, CSS Text 3 4.1.1) */
        if ((c > 0 && c < 0x20 && c != 0x09 && c != 0x0A) || c == 0x7F) return 0x25A1;
        return c;
    }
    if ((c & 0xE0) == 0xC0) {
        c = ((c & 0x1F) << 6) | (p[0] & 0x3F);
        *ptr = (const char *)(p + 1);
        return c;
    }
    if ((c & 0xF0) == 0xE0) {
        c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
        *ptr = (const char *)(p + 2);
        return c;
    }
    if ((c & 0xF8) == 0xF0) {
        c = ((c & 0x07) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *ptr = (const char *)(p + 3);
        return c;
    }
    *ptr = (const char *)p;
    return 0xFFFD; /* replacement character */
}

static uint32_t strike_bucket(const TextShaper *s, int size_q)
{
    uint32_t h = (uint32_t)((uintptr_t)s >> 4) * 0x9E3779B1u ^ (uint32_t)size_q * 0x85EBCA6Bu;
    return (h ^ h >> 16) & (TS_STRIKE_BUCKETS - 1);
}

/* Font s at size_q / 4 pixels, made the first time. */
static Strike *ts_strike(const TextShaper *s, int size_q)
{
    uint32_t b = strike_bucket(s, size_q);
    for (Strike *k = g_strikes[b]; k; k = k->next)
        if (k->face == s && k->size_q == size_q) return k;
    Strike *k = (Strike *)calloc(1, sizeof(Strike));
    if (!k) return NULL;
    float px = (float)size_q * 0.25f;
    k->face = s;
    k->size_q = size_q;
    k->scale = stbtt_ScaleForMappingEmToPixels(&s->font, px);
    k->embolden = s->embolden ? (int)roundf((float)s->embolden * px / s->size_pixels) : 0;
    if (s->embolden && k->embolden < 1) k->embolden = 1;
    k->next = g_strikes[b];
    g_strikes[b] = k;
    return k;
}

static void strike_free_all(const TextShaper *s)
{
    for (int b = 0; b < TS_STRIKE_BUCKETS; b++) {
        for (Strike **p = &g_strikes[b]; *p;) {
            Strike *k = *p;
            if (k->face == s) {
                *p = k->next;
                free(k->glyphs);
                free(k);
            } else {
                p = &k->next;
            }
        }
    }
}

static bool strike_grow(Strike *k)
{
    int cap = k->cap ? k->cap * 2 : 128;
    StrikeGlyph *ng = (StrikeGlyph *)malloc((size_t)cap * sizeof(StrikeGlyph));
    if (!ng) return false;
    for (int i = 0; i < cap; i++) ng[i].glyph_index = -1;
    for (int i = 0; i < k->cap; i++) {
        if (k->glyphs[i].glyph_index < 0) continue;
        uint32_t j = (uint32_t)k->glyphs[i].glyph_index * 0x9E3779B1u;
        while (ng[j & (cap - 1)].glyph_index >= 0) j++;
        ng[j & (cap - 1)] = k->glyphs[i];
    }
    free(k->glyphs);
    k->glyphs = ng;
    k->cap = cap;
    return true;
}

/* The vertical scale and shift that put glyph gi's outline top and bottom
 * on whole pixels (rounded from where they fall at the strike's scale), as
 * other engines' hinting does: an edge otherwise half covers a row (Ahem's
 * 0.8em ascent at 16px is 12.8px).  The plain scale for a glyph with no
 * height or one that would vanish. */
static void strike_vsnap(const Strike *k, int gi, float *sy, float *shift)
{
    *sy = k->scale;
    *shift = 0.0f;
    int x0, y0, x1, y1;
    if (!stbtt_GetGlyphBox(&k->face->font, gi, &x0, &y0, &x1, &y1) || y1 <= y0) return;
    float top = roundf((float)y1 * k->scale), bot = roundf((float)y0 * k->scale);
    if (top <= bot) return;
    *sy = (top - bot) / (float)(y1 - y0);
    *shift = (float)y1 * *sy - top;
}

/* Glyph gi of a strike: its box measured the first time. */
static StrikeGlyph *strike_glyph(Strike *k, int gi)
{
    if (gi < 0) return NULL;
    if (k->count * 2 >= k->cap && !strike_grow(k)) return NULL;
    uint32_t j = (uint32_t)gi * 0x9E3779B1u;
    for (;; j++) {
        StrikeGlyph *g = &k->glyphs[j & (k->cap - 1)];
        if (g->glyph_index == gi) return g;
        if (g->glyph_index >= 0) continue;
        int ix0, iy0, ix1, iy1;
        float sy, shift;
        strike_vsnap(k, gi, &sy, &shift);
        stbtt_GetGlyphBitmapBoxSubpixel(&k->face->font, gi, k->scale, sy, 0.0f, shift, &ix0, &iy0, &ix1, &iy1);
        int gw = ix1 - ix0;
        g->glyph_index = gi;
        g->xoff = (int16_t)ix0;
        g->yoff = (int16_t)iy0;
        g->bw = (uint16_t)(gw > 0 ? gw + k->embolden : 0);
        g->bh = (uint16_t)(iy1 > iy0 ? iy1 - iy0 : 0);
        g->ref.id = -1;
        g->ref.gen = 0;
        k->count++;
        return g;
    }
}

static GlyphAtlas *ts_atlas(void)
{
    if (!g_atlas) g_atlas = glyph_atlas_create(TS_ATLAS_PAGE, TS_ATLAS_BUDGET);
    return g_atlas;
}

/* Where glyph g's bitmap is in the atlas, rasterized into it if it is not
 * there; false when it has none or there is no room. */
static bool strike_place(Strike *k, StrikeGlyph *g, GaRect *r)
{
    if (g->bw == 0 || g->bh == 0 || !ts_atlas()) return false;
    if (g->ref.id >= 0 && glyph_atlas_get(g_atlas, g->ref, r)) return true;
    if (!glyph_atlas_alloc(g_atlas, g->bw, g->bh, &g->ref, r)) {
        g->ref.id = -1;
        return false;
    }
    int stride = glyph_atlas_page_size(g_atlas);
    uint8_t *dst = glyph_atlas_page_pixels(g_atlas, r->page) + (size_t)r->y * stride + r->x;
    int e = k->embolden, bw = g->bw, bh = g->bh, gw = bw - e;
    float sy, shift;
    strike_vsnap(k, g->glyph_index, &sy, &shift);
    stbtt_MakeGlyphBitmapSubpixel(&k->face->font, dst, gw, bh, stride, k->scale, sy, 0.0f, shift, g->glyph_index);
    /* Coverage of a percent or less is none, as the contrast tables of
     * other engines' text make it (an outline just past a pixel's edge
     * would otherwise tint it). */
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < gw; x++)
            if (dst[y * stride + x] <= 2) dst[y * stride + x] = 0;
    /* Synthetic bold: smear each row right by `e` pixels (max filter). */
    for (int y = 0; e > 0 && y < bh; y++) {
        uint8_t *row = dst + y * stride;
        for (int x = bw - 1; x >= 0; x--) {
            uint8_t m = 0;
            for (int i = 0; i <= e; i++) {
                int sx = x - i;
                if (sx >= 0 && sx < gw && row[sx] > m) m = row[sx];
            }
            row[x] = m;
        }
    }
    return true;
}

/* The font that draws a glyph: owner 0 is the shaper itself, owner k + 1 is
 * fallback k of the font registry. */
static __thread TextShaper *t_chain[6];
static __thread int t_chain_n;
void text_shaper_set_chain(TextShaper *const *fonts, int n)
{
    if (n < 0 || !fonts) n = 0;
    if (n > 6) n = 6;
    for (int i = 0; i < n; i++) t_chain[i] = fonts[i];
    t_chain_n = n;
}
static TextShaper *ts_owner_shaper(const TextShaper *s, int owner)
{
    if (owner >= 240) return owner - 240 < t_chain_n ? t_chain[owner - 240] : (TextShaper *)s;   /* (the font-family list's) */
    return owner == 0 ? (TextShaper *)s : font_registry_fallback(owner - 1);
}

/* Glyph index and advance (at the loaded size) of a codepoint, and which
 * font has it: the shaper, else the first registry fallback that does, else
 * the shaper's replacement glyph.  Safe from any thread. */
static void ts_metric_owned(const TextShaper *s, uint32_t cp, int *glyph_index, float *xadvance,
                            int *owner_out)
{
    if (cp > 0x10FFFF) cp = 0xFFFD;
    _Atomic uint64_t *page = atomic_load_explicit(&s->metric_pages[cp >> 8], memory_order_acquire);
    if (!page) {
        _Atomic uint64_t *fresh = calloc(256, sizeof(uint64_t));
        _Atomic uint64_t *expected = NULL;
        if (atomic_compare_exchange_strong_explicit(&s->metric_pages[cp >> 8], &expected, fresh,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            page = fresh;
        } else {
            free(fresh);
            page = expected;
        }
    }
    uint64_t v = atomic_load_explicit(&page[cp & 255], memory_order_relaxed);
    if (!v) {
        const TextShaper *f = s;
        int owner = 0;
        int gi = stbtt_FindGlyphIndex(&s->font, (int)cp);
        if (gi == 0 && cp > ' ' && cp != 0xFFFD) {
            int n = font_registry_fallback_count();
            for (int k = 0; k < n && n <= 254; k++) {
                const TextShaper *fb = font_registry_fallback(k);
                if (!fb || fb == s) continue;
                int g2 = stbtt_FindGlyphIndex(&fb->font, (int)cp);
                if (g2) { f = fb; owner = k + 1; gi = g2; break; }
            }
        }
        if (gi == 0 && cp != ' ') gi = stbtt_FindGlyphIndex(&s->font, 0xFFFD);
        int advance, lsb;
        stbtt_GetGlyphHMetrics(&f->font, gi, &advance, &lsb);
        float xa = (float)advance * f->scale + (float)f->embolden;
        uint32_t bits;
        memcpy(&bits, &xa, sizeof(bits));
        v = ((uint64_t)owner << 56) | ((uint64_t)((uint32_t)(gi + 1) & 0xFFFFFF) << 32) | bits;
        /* (bit 55: the font itself lacks it) */
        if (owner != 0 || stbtt_FindGlyphIndex(&s->font, (int)cp) == 0) v |= (uint64_t)1 << 55;
        atomic_store_explicit(&page[cp & 255], v, memory_order_relaxed);
    }
    if (t_chain_n > 0 && (v >> 55 & 1) && cp > ' ') {
        /* (the element's other fonts first: not cached, the list being the element's) */
        for (int k = 0; k < t_chain_n; k++) {
            const TextShaper *cf = t_chain[k];
            if (!cf || cf == s) continue;
            int g2 = stbtt_FindGlyphIndex(&cf->font, (int)cp);
            if (!g2) continue;
            int adv2, lsb2;
            stbtt_GetGlyphHMetrics(&cf->font, g2, &adv2, &lsb2);
            *glyph_index = g2;
            if (owner_out) *owner_out = 240 + k;
            *xadvance = (float)adv2 * cf->scale + (float)cf->embolden;
            return;
        }
    }
    *glyph_index = (int)((v >> 32) & 0x7FFFFF) - 1;
    if (owner_out) *owner_out = (int)(v >> 56) & 0xFF;
    uint32_t bits = (uint32_t)v;
    memcpy(xadvance, &bits, sizeof(*xadvance));
}

/* ---- OpenType GSUB: the features asked for, as one glyph map per font
 * (single substitutions, an alternate set's first, through extensions) ---- */
static __thread uint32_t t_ot_tags[24];
static __thread int t_ot_n;
static __thread uint64_t t_ot_key;   /* (the tags' hash) */

void text_shaper_set_features(const uint32_t *tags, int n)
{
    if (n < 0) n = 0;
    if (n > 24) n = 24;
    uint64_t key = 1469598103934665603ull;
    for (int i = 0; i < n; i++) { t_ot_tags[i] = tags[i]; key = (key ^ tags[i]) * 1099511628211ull; }
    t_ot_n = n;
    t_ot_key = key;
}

/* (a ligature: its first glyph, the others, the glyph they make) */
typedef struct { uint16_t first, n, comp[4], lig; uint32_t ord; } OtLig;
static int ot_lig_cmp(const void *a, const void *b)
{
    const OtLig *x = (const OtLig *)a, *y = (const OtLig *)b;
    if (x->first != y->first) return x->first < y->first ? -1 : 1;
    return x->ord < y->ord ? -1 : x->ord > y->ord;
}
/* (keyed by the font file, not the size: s is the font's data) */
typedef struct { const void *s; uint64_t key; uint16_t *map; int n; OtLig *ligs; int nlig; } OtMap;
/* (this thread's last lookup) */
static __thread const void *t_last_s;
static __thread uint64_t t_last_key;
static __thread const OtMap *t_last;
static __thread OtLig *t_build_ligs;
static __thread int t_build_nlig, t_build_cap;
static OtMap g_otmaps[1024];
static int g_otmaps_n;
static pthread_mutex_t g_ot_mu = PTHREAD_MUTEX_INITIALIZER;

static inline uint32_t ot_rd16(const uint8_t *d, size_t sz, size_t o) { return o + 2 <= sz ? (uint32_t)((d[o] << 8) | d[o + 1]) : 0; }
static inline uint32_t ot_rd32(const uint8_t *d, size_t sz, size_t o) { return o + 4 <= sz ? ((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) | ((uint32_t)d[o + 2] << 8) | d[o + 3] : 0; }

/* (glyph g's index in a coverage table, -1 when not covered) */
static int ot_coverage(const uint8_t *d, size_t sz, size_t cov, uint32_t g)
{
    uint32_t fmt = ot_rd16(d, sz, cov), n = ot_rd16(d, sz, cov + 2);
    if (fmt == 1) {
        int lo = 0, hi = (int)n - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            uint32_t v = ot_rd16(d, sz, cov + 4 + 2 * (size_t)mid);
            if (v == g) return mid;
            if (v < g) lo = mid + 1; else hi = mid - 1;
        }
    } else if (fmt == 2) {
        int lo = 0, hi = (int)n - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            size_t r = cov + 4 + 6 * (size_t)mid;
            uint32_t a = ot_rd16(d, sz, r), b = ot_rd16(d, sz, r + 2);
            if (g < a) hi = mid - 1;
            else if (g > b) lo = mid + 1;
            else return (int)(ot_rd16(d, sz, r + 4) + (g - a));
        }
    }
    return -1;
}

static uint16_t *ot_build(const TextShaper *s, const uint32_t *tags, int ntags)
{
    const uint8_t *d = s->font.data;
    size_t sz = s->ttf_size;
    uint32_t gsub = stbtt__find_table((stbtt_uint8 *)d, (stbtt_uint32)s->font.fontstart, "GSUB");
    int N = s->font.numGlyphs;
    if (!gsub || N <= 0 || N > 65536 || gsub >= sz) return NULL;
    size_t fl = gsub + ot_rd16(d, sz, gsub + 6), ll = gsub + ot_rd16(d, sz, gsub + 8);
    uint32_t nl = ot_rd16(d, sz, ll);
    if (!nl || nl > 4096) return NULL;
    uint8_t *use = (uint8_t *)calloc(nl, 1);
    if (!use) return NULL;
    uint32_t nf = ot_rd16(d, sz, fl);
    for (uint32_t i = 0; i < nf && i < 4096; i++) {
        size_t fr = fl + 2 + 6 * (size_t)i;
        uint32_t tag = ot_rd32(d, sz, fr);
        bool want = false;
        int fval = 1;
        for (int k = 0; k < ntags && !want; k++)
            if (tags[k] == tag) {
                want = true;
                /* (a value of its own: the pseudo tag after it, 0x01 then the value) */
                if (k + 1 < ntags && (tags[k + 1] >> 24) == 0x01) fval = (int)(tags[k + 1] & 0xFF);
            }
        if (!want) continue;
        size_t ft = fl + ot_rd16(d, sz, fr + 4);
        uint32_t cnt = ot_rd16(d, sz, ft + 2);
        for (uint32_t k = 0; k < cnt && k < 1024; k++) {
            uint32_t li = ot_rd16(d, sz, ft + 4 + 2 * (size_t)k);
            if (li < nl) use[li] = (uint8_t)(fval > 0 ? fval : 1);
        }
    }
    uint16_t *map = (uint16_t *)malloc((size_t)N * sizeof(uint16_t));
    if (!map) { free(use); return NULL; }
    for (int g = 0; g < N; g++) map[g] = (uint16_t)g;
    bool any = false;
    for (uint32_t li = 0; li < nl; li++) {
        if (!use[li]) continue;
        size_t lk = ll + ot_rd16(d, sz, ll + 2 + 2 * (size_t)li);
        uint32_t type = ot_rd16(d, sz, lk), nsub = ot_rd16(d, sz, lk + 4);
        for (uint32_t k = 0; k < nsub && k < 256; k++) {
            size_t st = lk + ot_rd16(d, sz, lk + 6 + 2 * (size_t)k);
            uint32_t t = type;
            if (t == 7) { t = ot_rd16(d, sz, st + 2); st = st + ot_rd32(d, sz, st + 4); }
            if (t == 4) {
                /* (ligatures: each covered first glyph's sets) */
                size_t cov4 = st + ot_rd16(d, sz, st + 2);
                uint32_t nset = ot_rd16(d, sz, st + 4);
                uint32_t cf = ot_rd16(d, sz, cov4), cn = ot_rd16(d, sz, cov4 + 2);
                for (uint32_t ci = 0, idx = 0; ci < cn && ci < 65536; ci++) {
                    uint32_t ga = 0, gb = 0;
                    if (cf == 1) { ga = gb = ot_rd16(d, sz, cov4 + 4 + 2 * (size_t)ci); }
                    else if (cf == 2) { size_t r = cov4 + 4 + 6 * (size_t)ci; ga = ot_rd16(d, sz, r); gb = ot_rd16(d, sz, r + 2); idx = ot_rd16(d, sz, r + 4); }
                    else break;
                    for (uint32_t g = ga; g <= gb && g < 65536; g++, idx++) {
                        if (idx >= nset) break;
                        size_t ls = st + ot_rd16(d, sz, st + 6 + 2 * (size_t)idx);
                        uint32_t nl2 = ot_rd16(d, sz, ls);
                        for (uint32_t li2 = 0; li2 < nl2 && li2 < 256; li2++) {
                            size_t lg = ls + ot_rd16(d, sz, ls + 2 + 2 * (size_t)li2);
                            uint32_t lgl = ot_rd16(d, sz, lg), cc = ot_rd16(d, sz, lg + 2);
                            if (cc < 2 || cc > 5) continue;
                            if (t_build_nlig == t_build_cap) {
                                int nc = t_build_cap ? t_build_cap * 2 : 64;
                                OtLig *nlp = (OtLig *)realloc(t_build_ligs, (size_t)nc * sizeof(OtLig));
                                if (!nlp) continue;
                                t_build_ligs = nlp;
                                t_build_cap = nc;
                            }
                            OtLig *L = &t_build_ligs[t_build_nlig++];
                            L->first = (uint16_t)g;
                            L->n = (uint16_t)(cc - 1);
                            for (uint32_t q = 0; q + 1 < cc; q++) L->comp[q] = (uint16_t)ot_rd16(d, sz, lg + 4 + 2 * (size_t)q);
                            L->lig = (uint16_t)lgl;
                            L->ord = (uint32_t)(t_build_nlig - 1);
                        }
                    }
                    if (cf == 1) idx = ci + 1;
                }
                continue;
            }
            if (t != 1 && t != 3) continue;
            uint32_t fmt = ot_rd16(d, sz, st);
            size_t cov = st + ot_rd16(d, sz, st + 2);
            for (int g = 0; g < N; g++) {
                int c = ot_coverage(d, sz, cov, map[g]);
                if (c < 0) continue;
                uint32_t to = map[g];
                if (t == 1 && fmt == 1) to = (map[g] + ot_rd16(d, sz, st + 4)) & 0xFFFF;
                else if (t == 1 && fmt == 2) { if ((uint32_t)c < ot_rd16(d, sz, st + 4)) to = ot_rd16(d, sz, st + 6 + 2 * (size_t)c); }
                else if (t == 3) {
                    if ((uint32_t)c < ot_rd16(d, sz, st + 4)) {
                        size_t as = st + ot_rd16(d, sz, st + 6 + 2 * (size_t)c);
                        /* (the feature's value: the alternate, from 1) */
                        uint32_t alt = use[li] ? (uint32_t)use[li] - 1 : 0;
                        if (alt < ot_rd16(d, sz, as)) to = ot_rd16(d, sz, as + 2 + 2 * (size_t)alt);
                    }
                }
                if ((int)to < N && to != map[g]) { map[g] = (uint16_t)to; any = true; }
            }
        }
    }
    free(use);
    if (!any && !t_build_nlig) { free(map); return NULL; }
    return map;
}

/* (the map for font s and the features set now; NULL: none changes a glyph) */
static const uint16_t *ot_map_for(const TextShaper *s, int *n)
{
    if (t_ot_n <= 0) return NULL;
    uint64_t key = t_ot_key;
    const void *fk = s->font.data + s->font.fontstart;
    if (t_last && t_last_s == fk && t_last_key == key) { *n = t_last->n; return t_last->map; }
    const uint16_t *m = NULL;
    pthread_mutex_lock(&g_ot_mu);
    for (int i = 0; i < g_otmaps_n; i++)
        if (g_otmaps[i].s == fk && g_otmaps[i].key == key) {
            m = g_otmaps[i].map; *n = g_otmaps[i].n;
            t_last = &g_otmaps[i]; t_last_s = fk; t_last_key = key;
            pthread_mutex_unlock(&g_ot_mu);
            return m;
        }
    pthread_mutex_unlock(&g_ot_mu);
    t_build_ligs = NULL;
    t_build_nlig = t_build_cap = 0;
    uint16_t *built = ot_build(s, t_ot_tags, t_ot_n);
    pthread_mutex_lock(&g_ot_mu);
    if (g_otmaps_n < (int)(sizeof g_otmaps / sizeof g_otmaps[0])) {
        g_otmaps[g_otmaps_n].s = fk;
        g_otmaps[g_otmaps_n].key = key;
        if (t_build_nlig > 1) qsort(t_build_ligs, (size_t)t_build_nlig, sizeof(OtLig), ot_lig_cmp);
        t_last = &g_otmaps[g_otmaps_n]; t_last_s = fk; t_last_key = key;
        g_otmaps[g_otmaps_n].map = built;
        g_otmaps[g_otmaps_n].n = s->font.numGlyphs;
        g_otmaps[g_otmaps_n].ligs = t_build_ligs;
        g_otmaps[g_otmaps_n].nlig = t_build_nlig;
        g_otmaps_n++;
    } else free(t_build_ligs);
    t_build_ligs = NULL;
    pthread_mutex_unlock(&g_ot_mu);
    *n = s->font.numGlyphs;
    return built;
}

/* (the ligatures for font s and the features now, by first glyph) */
static const OtLig *ot_ligs_for(const TextShaper *s, int *n)
{
    *n = 0;
    if (t_ot_n <= 0) return NULL;
    int nm = 0;
    (void)ot_map_for(s, &nm);
    const void *fk = s->font.data + s->font.fontstart;
    if (t_last && t_last_s == fk && t_last_key == t_ot_key) { *n = t_last->nlig; return t_last->ligs; }
    return NULL;
}

static void ts_metric(const TextShaper *s, uint32_t cp, int *glyph_index, float *xadvance, int *owner_out);
/* (a ligature starting with glyph gi at p (the next code points to end):
 * its glyph and advance, *p moved past the others; false when none) */
static bool ts_ligature(const TextShaper *s, int gi, const char **p, const char *end, int *lig, float *xa)
{
    int nl = 0;
    const OtLig *L = ot_ligs_for(s, &nl);
    if (!L || nl <= 0) return false;
    /* (the first with this first glyph: the sort kept each glyph's order) */
    int lo = 0, hi = nl;
    while (lo < hi) { int md = (lo + hi) / 2; if (L[md].first < gi) lo = md + 1; else hi = md; }
    for (int i = lo; i < nl && L[i].first == gi; i++) {
        const char *q = *p;
        bool ok = true;
        for (int c = 0; c < L[i].n && ok; c++) {
            if (q >= end) { ok = false; break; }
            uint32_t cp = utf8_decode(&q);
            int g2, o2;
            float x2;
            ts_metric(s, cp, &g2, &x2, &o2);
            ok = o2 == 0 && g2 == L[i].comp[c];
        }
        if (!ok || q > end) continue;
        *p = q;
        *lig = L[i].lig;
        int advance, lsb;
        stbtt_GetGlyphHMetrics(&s->font, *lig, &advance, &lsb);
        *xa = (float)advance * s->scale + (float)s->embolden;
        return true;
    }
    return false;
}

static __thread int t_force_gi = -1;   /* (the glyph ts_emit_glyph draws next, a ligature's) */
static __thread float t_force_xa;

/* (a code point's glyph and advance, the features' substitutions applied) */
static void ts_metric(const TextShaper *s, uint32_t cp, int *glyph_index, float *xadvance, int *owner_out)
{
    int owner = 0;
    if (t_force_gi >= 0) { *glyph_index = t_force_gi; *xadvance = t_force_xa; if (owner_out) *owner_out = 0; t_force_gi = -1; return; }
    ts_metric_owned(s, cp, glyph_index, xadvance, &owner);
    if (owner_out) *owner_out = owner;
    if (t_ot_n <= 0 || owner != 0 || *glyph_index < 0) return;
    int n = 0;
    const uint16_t *m = ot_map_for(s, &n);
    if (!m || *glyph_index >= n || m[*glyph_index] == *glyph_index) return;
    *glyph_index = m[*glyph_index];
    int advance, lsb;
    stbtt_GetGlyphHMetrics(&s->font, *glyph_index, &advance, &lsb);
    *xadvance = (float)advance * s->scale + (float)s->embolden;
}

/* Kerning between two glyphs in font units, cached.  Safe from any thread. */
static int ts_kern(const TextShaper *s, int g1, int g2)
{
    if (g1 < 0 || g2 < 0 || g1 > 0xFFFF || g2 > 0xFFFF)
        return stbtt_GetGlyphKernAdvance(&s->font, g1, g2);
    uint32_t key = (((uint32_t)g1 << 16) | (uint32_t)g2) + 1u;
    uint32_t h = (key * 0x9E3779B1u) >> 17;   /* 15 bits */
    for (uint32_t i = 0; i < TS_KERN_PROBES; i++) {
        _Atomic uint64_t *slot = &s->kern[(h + i) & (TS_KERN_SLOTS - 1)];
        uint64_t v = atomic_load_explicit(slot, memory_order_relaxed);
        if (v && (uint32_t)(v >> 32) == key) return (int)(int32_t)(uint32_t)v;
        if (!v) {
            int k = stbtt_GetGlyphKernAdvance(&s->font, g1, g2);
            uint64_t nv = ((uint64_t)key << 32) | (uint32_t)(int32_t)k;
            uint64_t expected = 0;
            if (atomic_compare_exchange_strong_explicit(slot, &expected, nv, memory_order_relaxed,
                                                        memory_order_relaxed) ||
                (uint32_t)(expected >> 32) == key)
                return k;
            /* Lost the slot to another pair: keep probing. */
        }
    }
    return stbtt_GetGlyphKernAdvance(&s->font, g1, g2);   /* neighbourhood full */
}

/* (where the font's COLR and CPAL tables are, when it has both in a form we read) */
static void ts_find_color_tables(TextShaper *s, int offset)
{
    const uint8_t *d = s->ttf_data;
    size_t sz = s->ttf_size, o = (size_t)offset;
    if (o + 12 > sz) return;
    uint32_t nt = (uint32_t)d[o + 4] << 8 | d[o + 5];
    for (uint32_t i = 0; i < nt && o + 12 + 16 * (size_t)(i + 1) <= sz; i++) {
        const uint8_t *r = d + o + 12 + 16 * i;
        uint32_t at = (uint32_t)r[8] << 24 | (uint32_t)r[9] << 16 | (uint32_t)r[10] << 8 | r[11];
        uint32_t len = (uint32_t)r[12] << 24 | (uint32_t)r[13] << 16 | (uint32_t)r[14] << 8 | r[15];
        if ((size_t)at + len > sz) continue;
        if (!memcmp(r, "COLR", 4) && len >= 14) { s->colr = d + at; s->colr_len = len; }
        else if (!memcmp(r, "CPAL", 4) && len >= 12) { s->cpal = d + at; s->cpal_len = len; }
    }
}

/* Record a glyph command drawing atlas rectangle r (none: a glyph without a
 * bitmap). */
static bool ts_add_glyph(DisplayList *dl, float x, float y, float w, float h, const GaRect *r, int gi, float cr,
                         float cg, float cb, float ca)
{
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    if (r) {
        float size = (float)glyph_atlas_page_size(g_atlas);
        u0 = (float)r->x / size;
        v0 = (float)r->y / size;
        u1 = (float)(r->x + r->w) / size;
        v1 = (float)(r->y + r->h) / size;
    }
    if (!display_list_add_glyph(dl, x, y, w, h, u0, v0, u1, v1, (uint32_t)gi, cr, cg, cb, ca)) return false;
    dl->cmds[dl->count - 1].u.glyph.font_slot = (uint8_t)(r ? r->page : 0);
    return true;
}

static int ts_size_q(float px)
{
    return (int)lroundf(px * 4.0f);
}

/* ---- COLR v0: a glyph drawn as a stack of other glyphs, each in a CPAL color ---- */
static inline uint32_t ts_be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline uint32_t ts_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static __thread int t_pal_kind, t_pal_index;   /* (0 normal, 1 light, 2 dark, 3 this palette) */
static __thread int t_pal_n;
static __thread int t_pal_over_i[16];
static __thread float t_pal_over[16][4];

void text_shaper_set_palette(int kind, int index, const int *over_index, const float (*over_rgba)[4], int n)
{
    t_pal_kind = kind;
    t_pal_index = index;
    t_pal_n = n > 16 ? 16 : n < 0 ? 0 : n;
    for (int i = 0; i < t_pal_n; i++) {
        t_pal_over_i[i] = over_index[i];
        memcpy(t_pal_over[i], over_rgba[i], sizeof t_pal_over[i]);
    }
}

/* (which of the font's palettes: the asked one, the first light or dark, else the first) */
static int ts_palette_pick(const TextShaper *o)
{
    const uint8_t *c = o->cpal;
    uint32_t len = o->cpal_len, np = ts_be16(c + 4);
    if (t_pal_kind == 3) return t_pal_index >= 0 && (uint32_t)t_pal_index < np ? t_pal_index : 0;
    if (t_pal_kind == 0 || ts_be16(c) < 1) return 0;
    uint32_t after = 12 + 2 * np;
    if (after + 4 > len) return 0;
    uint32_t types = ts_be32(c + after);
    if (!types || types + 4 * np > len) return 0;
    uint32_t want = t_pal_kind == 1 ? 1u : 2u;
    for (uint32_t i = 0; i < np; i++) if (ts_be32(c + types + 4 * i) & want) return (int)i;
    return 0;
}

/* (the color of entry e of the palette: an override's, else the font's; false none) */
static bool ts_palette_color(const TextShaper *o, int pal, uint32_t e, float *rgba)
{
    for (int i = t_pal_n - 1; i >= 0; i--)
        if ((uint32_t)t_pal_over_i[i] == e) { memcpy(rgba, t_pal_over[i], sizeof t_pal_over[i]); return true; }
    const uint8_t *c = o->cpal;
    uint32_t len = o->cpal_len;
    uint32_t ne = ts_be16(c + 2), first = ts_be32(c + 8), at = ts_be16(c + 12 + 2 * (uint32_t)pal);
    if (e >= ne || first + 4 * (at + e) + 4 > len) return false;
    const uint8_t *rec = c + first + 4 * (at + e);   /* (blue, green, red, alpha) */
    rgba[0] = rec[2] / 255.0f; rgba[1] = rec[1] / 255.0f; rgba[2] = rec[0] / 255.0f; rgba[3] = rec[3] / 255.0f;
    return true;
}

static bool ts_draw_glyph(DisplayList *dl, const TextShaper *o, int gi, float pen_x, float base_y, float scale,
                          float r, float g, float b, float a);

/* (glyph gi's color layers, if it has any: drawn; false: it's a plain glyph) */
static bool ts_draw_color_glyph(DisplayList *dl, const TextShaper *o, int gi, float pen_x, float base_y, float scale,
                                float r, float g, float b, float a, bool *ok)
{
    const uint8_t *c = o->colr;
    uint32_t len = o->colr_len;
    uint32_t nb = ts_be16(c + 2), bo = ts_be32(c + 4), lo = ts_be32(c + 8), nl = ts_be16(c + 12);
    if (bo + 6 * nb > len || lo + 4 * nl > len) return false;
    uint32_t lo_i = 0, hi = nb;
    while (lo_i < hi) {
        uint32_t m = (lo_i + hi) / 2, id = ts_be16(c + bo + 6 * m);
        if (id == (uint32_t)gi) {
            uint32_t first = ts_be16(c + bo + 6 * m + 2), n = ts_be16(c + bo + 6 * m + 4);
            if (first + n > nl) return false;
            int pal = o->cpal ? ts_palette_pick(o) : 0;
            for (uint32_t k = 0; k < n; k++) {
                const uint8_t *L = c + lo + 4 * (first + k);
                uint32_t lg = ts_be16(L), e = ts_be16(L + 2);
                float col[4] = { r, g, b, 1.0f };
                /* (0xFFFF: the text's color) */
                if (e != 0xFFFF && o->cpal && !ts_palette_color(o, pal, e, col)) continue;
                if (!ts_draw_glyph(dl, o, -(int)lg - 1, pen_x, base_y, scale, col[0], col[1], col[2], col[3] * a)) { *ok = false; return true; }
            }
            *ok = true;
            return true;
        }
        if (id < (uint32_t)gi) lo_i = m + 1; else hi = m;
    }
    return false;
}

/* Glyph gi of font o drawn with its pen at (pen_x, base_y), o's size times
 * scale: from o's strike at that size, where the glyph's box falls on the
 * right pixels (a scaled bitmap was a pixel or more off at large sizes, and
 * blurred), else (no room in the atlas, or a size past what a page holds)
 * from o's strike at its loaded size, scaled. */
static bool ts_draw_glyph(DisplayList *dl, const TextShaper *o, int gi, float pen_x, float base_y, float scale,
                          float r, float g, float b, float a)
{
    /* (a layer comes back as -gid - 1: drawn plainly) */
    if (gi < 0) gi = -gi - 1;
    else if (o->colr) {
        bool ok = true;
        if (ts_draw_color_glyph(dl, o, gi, pen_x, base_y, scale, r, g, b, a, &ok)) return ok;
    }
    float px = o->size_pixels * scale;
    GaRect rc;
    if (fabsf(scale - 1.0f) > 0.01f && px >= 4.0f && px <= 512.0f) {
        Strike *k = ts_strike(o, ts_size_q(px));
        StrikeGlyph *sg = k ? strike_glyph(k, gi) : NULL;
        if (sg) {
            bool placed = strike_place(k, sg, &rc);
            if (placed || sg->bw == 0 || sg->bh == 0)
                return ts_add_glyph(dl, pen_x + sg->xoff, base_y + sg->yoff, (float)sg->bw, (float)sg->bh,
                                    placed ? &rc : NULL, gi, r, g, b, a);
        }
    }
    Strike *k = ts_strike(o, ts_size_q(o->size_pixels));
    StrikeGlyph *sg = k ? strike_glyph(k, gi) : NULL;
    if (!sg) return true;
    bool placed = strike_place(k, sg, &rc);
    if (!placed && sg->bw && sg->bh) return true;          /* no room at all: not drawn */
    return ts_add_glyph(dl, pen_x + (float)sg->xoff * scale, base_y + (float)sg->yoff * scale, (float)sg->bw * scale,
                        (float)sg->bh * scale, placed ? &rc : NULL, gi, r, g, b, a);
}

/* letter-spacing and word-spacing at the loaded size (text_shaper_set_spacing). */
static __thread float t_letter_sp, t_word_sp;

void text_shaper_set_spacing(float letter_px, float word_px, float scale)
{
    if (scale <= 0.0f) scale = 1.0f;
    t_letter_sp = letter_px / scale;
    t_word_sp = word_px / scale;
}

/* (text-autospace: the mode, and the code point before) */
static __thread int t_autospace;
static __thread uint32_t t_as_prev;

void text_shaper_set_autospace(int mode) { t_autospace = mode; t_as_prev = 0; }

/* (1 an ideograph (Han, kana), 2 a letter of another script, 3 a digit, 0 else) */
static inline int ts_as_class(uint32_t cp)
{
    if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0x20000 && cp <= 0x3FFFF) || (cp >= 0x3041 && cp <= 0x3096) || (cp >= 0x30A1 && cp <= 0x30FA) ||
        cp == 0x3005 || cp == 0x3007 || (cp >= 0x31F0 && cp <= 0x31FF))
        return 1;
    if (cp >= '0' && cp <= '9') return 3;
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 && cp != 0xF7) ||
        (cp >= 0x370 && cp <= 0x3FF) || (cp >= 0x400 && cp <= 0x4FF))
        return 2;
    return 0;
}

/* (the space text-autospace puts before cp, at the loaded size) */
static inline float ts_autospace(const TextShaper *s, uint32_t cp)
{
    if (!t_autospace) return 0.0f;
    int p = ts_as_class(t_as_prev), c = ts_as_class(cp);
    t_as_prev = cp;
    if ((p == 1 && c == 2) || (p == 2 && c == 1)) return (t_autospace & 1) ? 0.125f * s->size_pixels : 0.0f;
    if ((p == 1 && c == 3) || (p == 3 && c == 1)) return (t_autospace & 2) ? 0.125f * s->size_pixels : 0.0f;
    return 0.0f;
}

/* The soft hyphen and the bidi controls (ALM, LRM, RLM, the embeddings, overrides and
 * isolates): no glyph, no advance, invisible to kerning. */
static inline bool ts_invisible(uint32_t cp)
{
    return cp == 0x00AD || cp == 0x061C || cp == 0x200C || cp == 0x200D || cp == 0x200E || cp == 0x200F || cp == 0x2060 || (cp >= 0x202A && cp <= 0x202E) ||
           (cp >= 0x2066 && cp <= 0x2069);
}

/* The spacing after codepoint cp (at the loaded size). */
static inline float ts_spacing(uint32_t cp)
{
    if (t_letter_sp == 0.0f && t_word_sp == 0.0f) return 0.0f;
    bool word = cp == ' ' || cp == 0xA0 || cp == 0x1361 || cp == 0x10100 || cp == 0x10101 || cp == 0x1039F ||
                cp == 0x1091F;
    return t_letter_sp + (word ? t_word_sp : 0.0f);
}

/* Codepoint cp of s (or the fallback font that has it) drawn at the pen,
 * kerned against the glyph before; the pen moves past it. */
static bool ts_emit_glyph(const TextShaper *s, uint32_t cp, float *pen_x, float base_y, float scale, int *prev_glyph,
                          float r, float g, float b, float a, DisplayList *dl)
{
    if (ts_invisible(cp)) return true;
    int gi, owner;
    float xa;
    ts_metric(s, cp, &gi, &xa, &owner);
    const TextShaper *gs = ts_owner_shaper(s, owner);
    if (!gs) gs = s;
    if (*prev_glyph >= 0 && owner == 0) *pen_x += (float)ts_kern(s, *prev_glyph, gi) * s->scale * scale;
    *pen_x += ts_autospace(s, cp) * scale;
    if (!ts_draw_glyph(dl, gs, gi, *pen_x, base_y, scale, r, g, b, a)) return false;
    *pen_x += (xa + ts_spacing(cp)) * scale;
    *prev_glyph = owner == 0 ? gi : -1;
    return true;
}

GlyphAtlas *text_shaper_glyph_atlas(void)
{
    return g_atlas;
}

bool text_shaper_glyph_rect(const TextShaper *s, float size_px, uint32_t codepoint, GaRect *out)
{
    if (!s || !out) return false;
    int gi, owner;
    float xa;
    ts_metric(s, codepoint, &gi, &xa, &owner);
    const TextShaper *o = ts_owner_shaper(s, owner);
    Strike *k = o ? ts_strike(o, ts_size_q(size_px)) : NULL;
    StrikeGlyph *g = k ? strike_glyph(k, gi) : NULL;
    return g && strike_place(k, g, out);
}

void text_shaper_frame_uses(const DisplayList *dl)
{
    if (!g_atlas || !dl) return;
    float size = (float)glyph_atlas_page_size(g_atlas);
    for (int i = 0; i < dl->count; i++) {
        const DisplayListCmd *c = &dl->cmds[i];
        if (c->type == DL_GLYPH && c->u.glyph.v1 > c->u.glyph.v0)
            glyph_atlas_touch(g_atlas, c->u.glyph.font_slot, (int)(c->u.glyph.v0 * size + 0.5f));
    }
}

void text_shaper_frame_sent(const DisplayList *dl)
{
    if (!g_atlas) return;
    text_shaper_frame_uses(dl);
    glyph_atlas_frame_sent(g_atlas);
}

TextShaper *text_shaper_create(const char *ttf_path, float size_pixels)
{
    if (!ttf_path || size_pixels <= 0) return NULL;

    FILE *f = fopen(ttf_path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    rewind(f);

    uint8_t *data = (uint8_t *)malloc((size_t)sz);
    if (!data) { fclose(f); return NULL; }
    if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        free(data); fclose(f); return NULL;
    }
    fclose(f);
    return text_shaper_create_from_memory(data, (size_t)sz, 0, size_pixels);
}

TextShaper *text_shaper_create_from_memory(uint8_t *data, size_t size, int font_index, float size_pixels)
{
    return text_shaper_create_from_memory_ex(data, size, font_index, size_pixels, 0);
}

/* A shaper's font data given back: unmapped, or freed. */
static void ts_release_data(uint8_t *data, bool mapped)
{
#ifdef _WIN32
    if (mapped) {
        if (data) UnmapViewOfFile(data);
        return;
    }
#endif
    (void)mapped;
    free(data);
}

static TextShaper *ts_create(uint8_t *data, size_t size, int font_index, float size_pixels,
                             int embolden, bool mapped)
{
    if (!data || size == 0 || size_pixels <= 0) { ts_release_data(data, mapped); return NULL; }
    int offset = stbtt_GetFontOffsetForIndex(data, font_index < 0 ? 0 : font_index);
    if (offset < 0) { ts_release_data(data, mapped); return NULL; }

    TextShaper *s = (TextShaper *)calloc(1, sizeof(TextShaper));
    if (!s) { ts_release_data(data, mapped); return NULL; }

    s->font_offset = offset;
    s->ttf_data = data;
    s->ttf_size = size;
    s->ttf_mapped = mapped;
    s->embolden = embolden > 0 ? embolden : 0;
    s->size_pixels = size_pixels;

    if (!stbtt_InitFont(&s->font, s->ttf_data, offset)) {
        text_shaper_destroy(s);
        return NULL;
    }
    ts_find_color_tables(s, offset);
    s->metric_pages = calloc(TS_METRIC_PAGES, sizeof(*s->metric_pages));
    s->kern = calloc(TS_KERN_SLOTS, sizeof(*s->kern));
    if (!s->metric_pages || !s->kern) {
        text_shaper_destroy(s);
        return NULL;
    }

    /* CSS sizes fonts by the em box, not the ascender-to-descender distance:
     * "16px Arial" means 16/2048 em units.  ScaleForPixelHeight would use
     * the hhea ascent+descent (2288 for Arial), making every glyph ~11%
     * narrower than Chrome's em-based metrics. */
    s->scale = stbtt_ScaleForMappingEmToPixels(&s->font, size_pixels);
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&s->font, &ascent, &descent, &line_gap);
    s->ascent   = (float)ascent * s->scale;
    s->descent  = (float)descent * s->scale;
    s->line_gap = (float)line_gap * s->scale;
    s->baseline = s->ascent;
    return s;
}

void text_shaper_destroy(TextShaper *s)
{
    if (!s) return;
    strike_free_all(s);       /* (their atlas entries go when evicted) */
    if (s->metric_pages) {
        for (int i = 0; i < TS_METRIC_PAGES; i++) free((void *)s->metric_pages[i]);
        free((void *)s->metric_pages);
    }
    free((void *)s->kern);
    ts_release_data(s->ttf_data, s->ttf_mapped);
    free(s);
}

TextShaper *text_shaper_create_from_memory_ex(uint8_t *data, size_t size, int font_index, float size_pixels,
                                              int embolden)
{
    return ts_create(data, size, font_index, size_pixels, embolden, false);
}

TextShaper *text_shaper_create_from_mapped(uint8_t *view, size_t size, int font_index, float size_pixels)
{
    return ts_create(view, size, font_index, size_pixels, 0, true);
}

/* The height of "x" and the advance of "0", as fractions of the em (CSS's
 * ex and ch); 0 when the font has no such glyph. */
float text_shaper_x_height(const TextShaper *s)
{
    int x0, y0, x1, y1;
    if (!s || !stbtt_FindGlyphIndex(&s->font, 'x') || !stbtt_GetCodepointBox(&s->font, 'x', &x0, &y0, &x1, &y1))
        return 0.0f;
    return (float)y1 * s->scale / s->size_pixels;
}

/* The height of "H" as a fraction of the em (the cap height); 0 when none. */
float text_shaper_cap_height(const TextShaper *s)
{
    int x0, y0, x1, y1;
    if (!s || !stbtt_FindGlyphIndex(&s->font, 'H') || !stbtt_GetCodepointBox(&s->font, 'H', &x0, &y0, &x1, &y1))
        return 0.0f;
    return (float)y1 * s->scale / s->size_pixels;
}

bool text_shaper_deco_metrics(const TextShaper *s, float *ul_pos, float *ul_th, float *st_pos, float *st_th)
{
    if (!s) return false;
    const stbtt_uint8 *d = s->font.data;
    stbtt_uint32 at = (stbtt_uint32)s->font.fontstart;
    float k = s->scale / s->size_pixels;
    bool any = false;
    stbtt_uint32 post = stbtt__find_table((stbtt_uint8 *)d, at, "post");
    if (post) {
        int pos = ttSHORT(d + post + 8), th = ttSHORT(d + post + 10);
        if (th > 0) { *ul_pos = (float)-pos * k; *ul_th = (float)th * k; any = true; }
    }
    stbtt_uint32 os2 = stbtt__find_table((stbtt_uint8 *)d, at, "OS/2");
    if (os2) {
        int sz = ttSHORT(d + os2 + 26), pos = ttSHORT(d + os2 + 28);
        if (sz > 0) { *st_pos = (float)pos * k; *st_th = (float)sz * k; }
    }
    return any;
}

float text_shaper_zero_advance(const TextShaper *s)
{
    int adv, lsb;
    /* (-1: no '0' glyph; its advance may be zero) */
    if (!s || !stbtt_FindGlyphIndex(&s->font, '0')) return -1.0f;
    stbtt_GetCodepointHMetrics(&s->font, '0', &adv, &lsb);
    return (float)adv * s->scale / s->size_pixels;
}

bool text_shaper_vmetrics(const TextShaper *s, float *ascent, float *descent, float *line_gap)
{
    if (!s || s->size_pixels <= 0.0f) return false;
    if (ascent) *ascent = s->ascent / s->size_pixels;
    if (descent) *descent = -s->descent / s->size_pixels;
    if (line_gap) *line_gap = s->line_gap / s->size_pixels;
    return true;
}

float text_shaper_normal_line_height(const TextShaper *s, float font_size)
{
    float a, d, g;
    if (!text_shaper_vmetrics(s, &a, &d, &g) || font_size <= 0.0f) return 0.0f;
    /* Browsers round ascent, descent and line gap to whole pixels. */
    return roundf(a * font_size) + roundf(d * font_size) + roundf(g * font_size);
}

bool text_shaper_measure(const TextShaper *s, const char *utf8,
                         float *out_width, float *out_height)
{
    if (!s || !utf8) return false;
    float x = 0.0f;
    int prev_glyph = -1;
    t_as_prev = 0;
    const char *p = utf8;
    uint32_t cp;
    while ((cp = utf8_decode(&p)) != 0) {
        if (cp == '\n') { x = 0; prev_glyph = -1; continue; }
        if (ts_invisible(cp)) continue;
        int gi, owner;
        float xa;
        ts_metric(s, cp, &gi, &xa, &owner);
        if (t_ot_n > 0 && owner == 0) {
            int lg;
            float lx;
            if (ts_ligature(s, gi, &p, p + strlen(p), &lg, &lx)) { gi = lg; xa = lx; }
        }
        if (prev_glyph >= 0 && owner == 0) {
            x += (float)ts_kern(s, prev_glyph, gi) * s->scale;
        }
        x += xa + ts_spacing(cp) + ts_autospace(s, cp);
        prev_glyph = owner == 0 ? gi : -1;
    }
    if (out_width)  *out_width  = x;
    if (out_height) *out_height = s->baseline - s->descent + s->line_gap;
    return true;
}

bool text_shaper_shape_to_display_list(const TextShaper *s,
                                       const char *utf8,
                                       float x, float y,
                                       float r, float g, float b, float a,
                                       DisplayList *dl)
{
    return text_shaper_shape_to_display_list_scaled(s, utf8, x, y, 1.0f, r, g, b, a, dl);
}

bool text_shaper_shape_to_display_list_scaled(const TextShaper *s,
                                       const char *utf8,
                                       float x, float y, float scale,
                                       float r, float g, float b, float a,
                                       DisplayList *dl)
{
    if (!s || !utf8 || !dl) return false;
    if (scale <= 0.0f) scale = 1.0f;
    float pen_x = x;
    float pen_y = y + s->baseline * scale;
    int prev_glyph = -1;
    const char *p = utf8;
    uint32_t cp;
    while ((cp = utf8_decode(&p)) != 0) {
        if (cp == '\n') {
            pen_x = x;
            pen_y += (s->baseline - s->descent + s->line_gap) * scale;
            prev_glyph = -1;
            continue;
        }
        if (!ts_emit_glyph(s, cp, &pen_x, pen_y, scale, &prev_glyph, r, g, b, a, dl))
            return false;
    }
    return true;
}

/* Measure the advance of the span [start, end) at the loaded size (unscaled). */
/* Fullwidth punctuation collapsing (css-text-4 text-spacing-trim, normal): of
 * two side by side, half the space between them goes.  Which class a code
 * point is of: 1 opening, 2 closing or a stop, 3 a middle dot or ideographic
 * space; 0 none. */
static int ts_cjk_punct(uint32_t c)
{
    switch (c) {
    case 0x3008: case 0x300A: case 0x300C: case 0x300E: case 0x3010: case 0x3014: case 0x3016: case 0x3018:
    case 0x301A: case 0x301D: case 0xFF08: case 0xFF3B: case 0xFF5B: case 0xFF5F:
        return 1;
    case 0x3009: case 0x300B: case 0x300D: case 0x300F: case 0x3011: case 0x3015: case 0x3017: case 0x3019:
    case 0x301B: case 0x301E: case 0x301F: case 0xFF09: case 0xFF3D: case 0xFF5D: case 0xFF60:
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E:
        return 2;
    case 0x30FB: case 0x3000: case 0xFF1A: case 0xFF1B:
        return 3;
    default:
        return 0;
    }
}

/* The start of the text a span is of, while one is measured or drawn: what
 * comes before it counts. */
static __thread const char *t_trim_text;

/* How much of cp's advance goes, by its neighbours (at [at, after) in the
 * text): first of the pair for a closing one, second for an opening one. */
static float ts_trim_of(uint32_t cp, const char *at, const char *after, float xa, bool *shift)
{
    *shift = false;
    int k = ts_cjk_punct(cp);
    if (!k || !t_trim_text || xa < 15.0f) return 0.0f;
    if (k == 2) {
        const char *q = after;
        uint32_t next = utf8_decode(&q);
        int nk = ts_cjk_punct(next);
        if (nk == 2 || nk == 3) return xa / 2.0f;
    } else if (k == 1 && at > t_trim_text) {
        const char *q = at - 1;
        while (q > t_trim_text && ((unsigned char)*q & 0xC0) == 0x80) q--;
        uint32_t prev = utf8_decode(&q);
        if (ts_cjk_punct(prev)) { *shift = true; return xa / 2.0f; }
    }
    return 0.0f;
}

static float ts_measure_span(const TextShaper *s, const char *start, const char *end)
{
    float x = 0.0f;
    t_as_prev = 0;
    int prev_glyph = -1;
    const char *p = start;
    while (p < end) {
        const char *save = p;
        uint32_t cp = utf8_decode(&p);
        if (cp == 0 || p > end) { p = save; break; }
        if (ts_invisible(cp)) continue;
        int gi, owner;
        float xa;
        ts_metric(s, cp, &gi, &xa, &owner);
        if (t_ot_n > 0 && owner == 0) {
            int lg;
            float lx;
            if (ts_ligature(s, gi, &p, end, &lg, &lx)) { gi = lg; xa = lx; }
        }
        if (prev_glyph >= 0 && owner == 0) {
            x += (float)ts_kern(s, prev_glyph, gi) * s->scale;
        }
        bool trim_shift;
        x -= ts_trim_of(cp, save, p, xa, &trim_shift);
        x += xa + ts_spacing(cp) + ts_autospace(s, cp);
        prev_glyph = owner == 0 ? gi : -1;
    }
    return x;
}

/* Letter test for hyphen break opportunities: ASCII letters plus a coarse
 * non-ASCII approximation (anything outside ASCII that is not a dash/quote
 * or an Arabic-Indic digit).  Good enough for Latin/CJK text; digits are
 * deliberately excluded so number ranges like "2018-19" never split. */
static bool ts_is_letter(uint32_t cp)
{
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) return true;
    if (cp >= 0x80) {
        if (cp >= 0x2010 && cp <= 0x2027) return false;  /* dashes, quotes */
        if ((cp >= 0x0660 && cp <= 0x0669) || (cp >= 0x06F0 && cp <= 0x06F9))
            return false;  /* Arabic-Indic digits */
        if (cp >= 0xFF10 && cp <= 0xFF19) return false;  /* fullwidth digits */
        return true;
    }
    return false;
}

/* Find the best hyphen break point inside [word_start, word_end) that fits
 * `avail` pixels: the longest prefix ending immediately after a hyphen
 * (U+002D/U+2010/U+2013) that is followed by a letter, per UAX #14's
 * break-after-hyphen rule.  Returns NULL when no such prefix fits. */
static const char *ts_find_hyphen_break(const TextShaper *s,
                                        const char *word_start,
                                        const char *word_end,
                                        float avail, float scale)
{
    const char *best = NULL;
    const char *q = word_start;
    while (q < word_end) {
        uint32_t c2 = utf8_decode(&q);
        if (c2 == 0) break;
        if ((c2 == '-' || c2 == 0x2010 || c2 == 0x2013) && q < word_end) {
            const char *nq = q;
            uint32_t nc = utf8_decode(&nq);
            if (nc != 0 && ts_is_letter(nc)) {
                float pw = ts_measure_span(s, word_start, q) * scale;
                if (pw <= avail + 0.001f) best = q;
                else break;  /* longer prefixes cannot fit either */
            }
        }
    }
    return best;
}

/* Emit glyphs for one span [gp, gend) advancing *pen_x.  Baseline gy is
 * precomputed by the caller (CSS strut model). */
static bool ts_emit_span_glyphs(const TextShaper *s, const char *gp,
                                const char *gend, float *pen_x, float gy,
                                float scale, int *prev_glyph,
                                float r, float g, float b, float a,
                                DisplayList *dl)
{
    while (gp < gend) {
        const char *at = gp;
        uint32_t gc = utf8_decode(&gp);
        if (gc == 0) break;
        if (gc < 0x20 && gc != ' ') continue; /* control chars: no glyph */
        /* (a trimmed opening one drawn into the half it gave up, a closing one
         * with the half after it gone) */
        float trim = 0.0f;
        bool shift = false;
        if (t_trim_text && ts_cjk_punct(gc)) {
            int gi, owner;
            float xa;
            ts_metric(s, gc, &gi, &xa, &owner);
            trim = ts_trim_of(gc, at, gp, xa, &shift) * scale;
        }
        if (shift) *pen_x -= trim;
        if (t_ot_n > 0) {
            int gi0, ow0, lg;
            float xa0, lx;
            ts_metric(s, gc, &gi0, &xa0, &ow0);
            if (ow0 == 0 && ts_ligature(s, gi0, &gp, gend, &lg, &lx)) { t_force_gi = lg; t_force_xa = lx; }
        }
        if (!ts_emit_glyph(s, gc, pen_x, gy, scale, prev_glyph, r, g, b, a, dl))
            return false;
        t_force_gi = -1;
        if (trim > 0.0f && !shift) *pen_x -= trim;
    }
    return true;
}

/* Where each wrapped line starts, recorded by ts_wrap_walk while a
 * truncation asks for it (line n starts at t_line_starts[n - 1]). */
static __thread const char **t_line_starts;
static __thread int t_line_starts_cap;

static void ts_note_line(int line, const char *start)
{
    if (t_line_starts && line >= 1 && line <= t_line_starts_cap) t_line_starts[line - 1] = start;
}

/* The lines ts_wrap_walk draws, recorded while text_shaper_wrap_lines asks
 * for them: each opened at its first drawn byte and closed at the byte
 * after its last. */
static __thread TsLine *t_lines;
static __thread int t_lines_cap, t_lines_n;
static __thread const char *t_lines_base;
static __thread float t_lines_em;   /* (line top to em box top: the half-leading) */

static void ts_line_open(const char *start, float x, float y)
{
    if (!t_lines || t_lines_n >= t_lines_cap) return;
    t_lines[t_lines_n].start = (uint32_t)(start - t_lines_base);
    t_lines[t_lines_n].x = x;
    t_lines[t_lines_n].y = y + t_lines_em;
}

static void ts_line_close(const char *end, float width)
{
    if (!t_lines) return;
    if (t_lines_n < t_lines_cap) {
        t_lines[t_lines_n].end = (uint32_t)(end - t_lines_base);
        t_lines[t_lines_n].width = width;
    }
    t_lines_n++;
}

static bool ts_wrap_walk(const TextShaper *s, const char *utf8,
                         float x, float y, float cont_x,
                         float first_width, float cont_width,
                         float scale, float line_advance,
                         int cont2_from_line, float cont2_x, float cont2_width,
                         bool emit, float r, float g, float b, float a,
                         DisplayList *dl, TsWrapResult *out);

#define TS_ELLIPSIS "\xE2\x80\xA6"

/* `head` (n bytes) shortened until head + "..." is at most `limit` wide, as a
 * malloc'd string with the ellipsis. */
static char *ts_fit_with_ellipsis(const TextShaper *s, const char *head, size_t n, float scale, float limit,
                                  const char *prefix, size_t prefix_len)
{
    float ew = ts_measure_span(s, TS_ELLIPSIS, TS_ELLIPSIS + 3) * scale;
    for (;;) {
        while (n && (head[n - 1] == ' ' || head[n - 1] == '\t' || head[n - 1] == '\n')) n--;
        if (n == 0 || ts_measure_span(s, head, head + n) * scale + ew <= limit + 0.01f) break;
        n--;
        while (n && ((unsigned char)head[n] & 0xC0) == 0x80) n--;     /* whole UTF-8 sequences */
    }
    char *out = (char *)malloc(prefix_len + n + 4);
    if (!out) return NULL;
    memcpy(out, prefix, prefix_len);
    memcpy(out + prefix_len, head, n);
    memcpy(out + prefix_len + n, TS_ELLIPSIS, 4);
    return out;
}

char *text_shaper_truncate_lines(const TextShaper *s, const char *utf8, float x, float cont_x,
                                 float first_width, float cont_width, float scale, int max_lines)
{
    if (!s || !utf8 || max_lines < 1 || max_lines > 64) return NULL;
    const char *starts[65] = { 0 };
    starts[0] = utf8;
    t_line_starts = starts;
    t_line_starts_cap = max_lines + 1;
    TsWrapResult wr;
    ts_wrap_walk(s, utf8, x, 0.0f, cont_x, first_width, cont_width, scale, 1.0f, 0, 0.0f, 0.0f,
                 false, 0, 0, 0, 0, NULL, &wr);
    t_line_starts = NULL;
    t_line_starts_cap = 0;
    if (!starts[max_lines]) return NULL;                 /* fits in max_lines */
    const char *last = starts[max_lines - 1] ? starts[max_lines - 1] : utf8;
    const char *cut = starts[max_lines];
    if (cut < last) return NULL;
    float limit = max_lines == 1 ? first_width : cont_width;
    return ts_fit_with_ellipsis(s, last, (size_t)(cut - last), scale, limit, utf8, (size_t)(last - utf8));
}

char *text_shaper_truncate_width(const TextShaper *s, const char *utf8, float scale, float width)
{
    if (!s || !utf8) return NULL;
    size_t n = strlen(utf8);
    if (ts_measure_span(s, utf8, utf8 + n) * scale <= width + 0.01f) return NULL;
    return ts_fit_with_ellipsis(s, utf8, n, scale, width, "", 0);
}

/* Shared word-wrap walker.  When emit is false this only measures.  Words are
 * maximal runs of non-space codepoints plus their trailing spaces; a line is
 * never broken before its first word. */
static bool ts_wrap_walk(const TextShaper *s, const char *utf8,
                         float x, float y, float cont_x,
                         float first_width, float cont_width,
                         float scale, float line_advance,
                         int cont2_from_line, float cont2_x, float cont2_width,
                         bool emit, float r, float g, float b, float a,
                         DisplayList *dl, TsWrapResult *out)
{
    if (!s || !utf8 || scale <= 0.0f) return false;
    if (line_advance <= 0.0f)
        line_advance = (s->baseline - s->descent + s->line_gap) * scale;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->line_advance = line_advance;
        out->last_end_x = x;
    }

    float pen_x = x;
    float pen_y = y;
    float line_start_x = x;
    float limit = first_width;
    int lines = 1;
    float max_width = 0.0f;
    int prev_glyph = -1;

    const char *word_start = utf8;
    const char *p = utf8;
    if (t_lines) {
        t_lines_em = (line_advance - s->size_pixels * scale) * 0.5f;
        ts_line_open(utf8, x, y);
    }
    for (;;) {
        /* A word ends after the next space (or at the string end). */
        uint32_t cp;
        const char *save = p;
        cp = utf8_decode(&p);
        bool at_end = (cp == 0);
        bool is_space = (cp == ' ' || cp == '\t' || cp == '\n');
        if (!at_end && !is_space) continue;

        /* Emit/measure the completed word [word_start, p) — for a space the
         * space itself is included as the word's trailing advance. */
        const char *word_end = at_end ? p : p;
        /* CSS white-space processing discards spaces at the end of a line, so
         * the fit test must use the width up to the last non-space codepoint;
         * the full width (with trailing spaces) still advances the pen when
         * the word stays on the line. */
        const char *trim_end = word_start;
        {
            const char *q = word_start;
            while (q < word_end) {
                const char *save = q;
                uint32_t c2 = utf8_decode(&q);
                if (c2 == 0) break;
                if (c2 != ' ' && c2 != '\t' && c2 != '\n') trim_end = q;
                (void)save;
            }
        }
        float word_w = ts_measure_span(s, word_start, word_end) * scale;
        float word_fit_w = ts_measure_span(s, word_start, trim_end) * scale;
        bool at_line_start = (pen_x <= line_start_x + 0.001f);
        /* UAX #14 hyphen break, case B: at a line start (including a run
         * that begins in a narrow sliver, e.g. a link starting mid-line that
         * the layout pushed to wrap mode) a word too wide for the whole line
         * still breaks at a hyphen when a prefix fits — Chrome splits
         * "comic-book" as "comic-"/"book" rather than overflowing. */
        if (at_line_start && word_fit_w > limit + 0.001f) {
            const char *bp = ts_find_hyphen_break(s, word_start, word_end,
                                                  limit, scale);
            if (bp && bp > word_start) {
                float half_leading = (line_advance - s->size_pixels * scale) * 0.5f;
                float gy = pen_y + half_leading + s->baseline * scale;
                if (emit) {
                    if (!ts_emit_span_glyphs(s, word_start, bp, &pen_x, gy,
                                             scale, &prev_glyph, r, g, b, a, dl))
                        return false;
                } else {
                    pen_x += ts_measure_span(s, word_start, bp) * scale;
                }
                float used = pen_x - line_start_x;
                if (used > max_width) max_width = used;
                lines++;
                ts_note_line(lines, bp);
                ts_line_close(bp, used);
                float cx = (cont2_from_line > 1 && lines >= cont2_from_line)
                         ? cont2_x : cont_x;
                float cw = (cont2_from_line > 1 && lines >= cont2_from_line)
                         ? cont2_width : cont_width;
                pen_x = cx;
                pen_y += line_advance;
                line_start_x = cx;
                limit = cw;
                prev_glyph = -1;
                word_start = bp;
                ts_line_open(bp, cx, pen_y);
                word_w = ts_measure_span(s, word_start, word_end) * scale;
                /* fall through: emit the remainder on the fresh line */
            }
        }
        if (pen_x > line_start_x + 0.001f && pen_x - line_start_x + word_fit_w > limit) {
            /* UAX #14 hyphen break, case A: the word doesn't fit the rest of
             * the line, but a prefix ending right after a hyphen does — break
             * there instead of wrapping the whole word. */
            const char *bp = ts_find_hyphen_break(s, word_start, word_end,
                                                  limit - (pen_x - line_start_x),
                                                  scale);
            if (bp && bp > word_start) {
                float half_leading = (line_advance - s->size_pixels * scale) * 0.5f;
                float gy = pen_y + half_leading + s->baseline * scale;
                if (emit) {
                    if (!ts_emit_span_glyphs(s, word_start, bp, &pen_x, gy,
                                             scale, &prev_glyph, r, g, b, a, dl))
                        return false;
                } else {
                    pen_x += ts_measure_span(s, word_start, bp) * scale;
                }
                float used = pen_x - line_start_x;
                if (used > max_width) max_width = used;
                lines++;
                ts_note_line(lines, bp);
                ts_line_close(bp, used);
                float cx = (cont2_from_line > 1 && lines >= cont2_from_line)
                         ? cont2_x : cont_x;
                float cw = (cont2_from_line > 1 && lines >= cont2_from_line)
                         ? cont2_width : cont_width;
                pen_x = cx;
                pen_y += line_advance;
                line_start_x = cx;
                limit = cw;
                prev_glyph = -1;
                word_start = bp;
                ts_line_open(bp, cx, pen_y);
                word_w = ts_measure_span(s, word_start, word_end) * scale;
                /* fall through: emit the remainder on the fresh line */
            } else {
            /* Wrap to the next line before this word.  Skip a leading space
             * on the new line: it belongs to the end of the old line. */
            float used = pen_x - line_start_x;
            if (used > max_width) max_width = used;
            lines++;
            ts_note_line(lines, word_start);
            ts_line_close(word_start, used);
            /* From cont2_from_line on, continuation lines use the second
             * geometry (e.g. full width below an expired float). */
            float cx = (cont2_from_line > 1 && lines >= cont2_from_line)
                     ? cont2_x : cont_x;
            float cw = (cont2_from_line > 1 && lines >= cont2_from_line)
                     ? cont2_width : cont_width;
            pen_x = cx;
            pen_y += line_advance;
            line_start_x = cx;
            limit = cw;
            prev_glyph = -1;
            /* Drop leading whitespace from the word on the new line. */
            while (word_start < word_end) {
                const char *ws = word_start;
                uint32_t wc = utf8_decode(&ws);
                if (wc == ' ' || wc == '\t' || wc == '\n') word_start = ws;
                else break;
            }
            ts_line_open(word_start, cx, pen_y);
            word_w = ts_measure_span(s, word_start, word_end) * scale;
            }
        }

        /* Emit glyphs for [word_start, word_end).  Baseline placement follows
         * the CSS strut model: the em box is centered in the line box, so the
         * baseline sits at half-leading + ascent from the line top. */
        if (emit) {
            float half_leading = (line_advance - s->size_pixels * scale) * 0.5f;
            float gy = pen_y + half_leading + s->baseline * scale;
            if (!ts_emit_span_glyphs(s, word_start, word_end, &pen_x, gy, scale, &prev_glyph, r, g, b, a, dl))
                return false;
        } else {
            pen_x += word_w;
        }

        word_start = word_end;
        if (at_end) break;
        if (cp == '\n') {
            /* Hard newline: flush line and reset. */
            float used = pen_x - line_start_x;
            if (used > max_width) max_width = used;
            lines++;
            ts_note_line(lines, p);
            ts_line_close(p, used);
            float cx = (cont2_from_line > 1 && lines >= cont2_from_line)
                     ? cont2_x : cont_x;
            float cw = (cont2_from_line > 1 && lines >= cont2_from_line)
                     ? cont2_width : cont_width;
            pen_x = cx;
            pen_y += line_advance;
            line_start_x = cx;
            limit = cw;
            prev_glyph = -1;
            ts_line_open(p, cx, pen_y);
        }
    }

    float used = pen_x - line_start_x;
    if (used > max_width) max_width = used;
    ts_line_close(utf8 + strlen(utf8), used);

    if (out) {
        out->lines = lines;
        out->height = (float)lines * line_advance;
        out->last_end_x = pen_x;
        out->max_width = max_width;
        out->line_advance = line_advance;
    }
    return true;
}

bool text_shaper_wrap_measure(const TextShaper *s, const char *utf8,
                              float x, float cont_x,
                              float first_width, float cont_width,
                              float scale, float line_advance,
                              int cont2_from_line, float cont2_x,
                              float cont2_width,
                              TsWrapResult *out)
{
    return ts_wrap_walk(s, utf8, x, 0.0f, cont_x,
                        first_width, cont_width, scale, line_advance,
                        cont2_from_line, cont2_x, cont2_width,
                        false, 0, 0, 0, 0, NULL, out)
           ? (out && out->lines > 0) : false;
}

bool text_shaper_wrap_shape(const TextShaper *s, const char *utf8,
                            float x, float y, float cont_x,
                            float first_width, float cont_width,
                            float scale, float line_advance,
                            int cont2_from_line, float cont2_x,
                            float cont2_width,
                            float r, float g, float b, float a,
                            DisplayList *dl, TsWrapResult *out)
{
    if (!dl) return false;
    return ts_wrap_walk(s, utf8, x, y, cont_x, first_width, cont_width,
                        scale, line_advance,
                        cont2_from_line, cont2_x, cont2_width,
                        true, r, g, b, a, dl, out);
}

int text_shaper_wrap_lines(const TextShaper *s, const char *utf8,
                           float x, float y, float cont_x,
                           float first_width, float cont_width,
                           float scale, float line_advance,
                           int cont2_from_line, float cont2_x,
                           float cont2_width,
                           TsLine *lines, int cap)
{
    if (!s || !utf8) return 0;
    t_lines = lines;
    t_lines_cap = cap;
    t_lines_n = 0;
    t_lines_base = utf8;
    TsWrapResult wr;
    ts_wrap_walk(s, utf8, x, y, cont_x, first_width, cont_width, scale, line_advance,
                 cont2_from_line, cont2_x, cont2_width, false, 0, 0, 0, 0, NULL, &wr);
    int n = t_lines_n;
    t_lines = NULL;
    t_lines_cap = t_lines_n = 0;
    return n;
}

float text_shaper_span_width(const TextShaper *s, const char *utf8, size_t start, size_t end, float scale)
{
    if (!s || !utf8 || end <= start) return 0.0f;
    t_trim_text = utf8;
    float w = ts_measure_span(s, utf8 + start, utf8 + end) * (scale > 0.0f ? scale : 1.0f);
    t_trim_text = NULL;
    return w;
}

bool text_shaper_shape_span(const TextShaper *s, const char *utf8, size_t start, size_t end,
                            float x, float y, float scale, bool rtl,
                            float r, float g, float b, float a, DisplayList *dl)
{
    if (!s || !utf8 || !dl || end <= start) return s && utf8 && dl;
    if (scale <= 0.0f) scale = 1.0f;
    float pen_x = x, gy = y + s->baseline * scale;
    int prev_glyph = -1;
    t_as_prev = 0;
    if (!rtl) {
        t_trim_text = utf8;
        bool drew = ts_emit_span_glyphs(s, utf8 + start, utf8 + end, &pen_x, gy, scale, &prev_glyph, r, g, b, a, dl);
        t_trim_text = NULL;
        return drew;
    }
    /* (right to left: the code points last first, each mirrored) */
    size_t cap = end - start;
    uint32_t stackbuf[256], *cps = cap <= 256 ? stackbuf : (uint32_t *)malloc(cap * sizeof(uint32_t));
    if (!cps) return false;
    size_t n = 0;
    for (const char *p = utf8 + start, *e = utf8 + end; p < e;) {
        uint32_t c = utf8_decode(&p);
        if (c == 0) break;
        cps[n++] = c;
    }
    bool ok = true;
    for (size_t i = n; i-- > 0 && ok;) {
        uint32_t c = cps[i];
        if (c < 0x20 && c != ' ') continue;
        ok = ts_emit_glyph(s, bidi_mirror(c), &pen_x, gy, scale, &prev_glyph, r, g, b, a, dl);
    }
    if (cps != stackbuf) free(cps);
    return ok;
}

bool text_shaper_has_glyph(const TextShaper *s, uint32_t codepoint)
{
    return s && stbtt_FindGlyphIndex(&s->font, (int)codepoint) != 0;
}

bool text_shaper_is_variable(const TextShaper *s)
{
    return s && stbtt__find_table(s->font.data, (stbtt_uint32)s->font.fontstart, "fvar") != 0;
}

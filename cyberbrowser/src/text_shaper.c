#include "text_shaper.h"

#include <stdatomic.h>
#include <stdbool.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "font_registry.h"

#ifndef TS_ATLAS_WIDTH
#define TS_ATLAS_WIDTH  1024
#endif
#ifndef TS_ATLAS_HEIGHT
#define TS_ATLAS_HEIGHT 1024
#endif

/* Pre-pack ASCII so common text has no runtime atlas cost. */
#define TS_PREPACK_FIRST 32
#define TS_PREPACK_LAST  126

typedef struct GlyphEntry {
    uint32_t codepoint;
    int      glyph_index;
    float    u0, v0, u1, v1;
    int      bw, bh;
    float    xoff, yoff;
    float    xadvance;
} GlyphEntry;

#define TS_SIZED_MAX 16

struct TextShaper {
    uint8_t        *ttf_data;
    size_t          ttf_size;
    bool            ttf_mapped;    /* a view of the font file, not malloc()ed */
    bool            ttf_borrowed;  /* the base shaper's (a size of it, below): not ours to release */
    int             font_offset;
    /* The same font rasterized at other sizes, for drawing text there
     * (ts_draw_glyph): each with its own atlas and slot. */
    struct TextShaper *sized[TS_SIZED_MAX];
    int             nsized;
    int             slot;          /* font-registry slot (-1 = none) */
    int             embolden;      /* synthetic bold: extra pixels per glyph */
    stbtt_fontinfo  font;
    float           size_pixels;
    float           scale;
    float           ascent;
    float           descent;
    float           line_gap;
    float           baseline;

    uint8_t        *atlas;         /* made at the first glyph drawn */
    int             atlas_w;
    int             atlas_h;
    int             pack_x;
    int             pack_y;
    int             pack_row_h;
    int             dirty_y0, dirty_y1;  /* atlas rows drawn since text_shaper_atlas_take_dirty */

    GlyphEntry     *glyphs;
    int             glyph_count;
    int             glyph_capacity;

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

static uint32_t utf8_decode(const char **ptr)
{
    const unsigned char *p = (const unsigned char *)*ptr;
    uint32_t c = *p++;
    if (c < 0x80) {
        *ptr = (const char *)p;
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

static GlyphEntry *find_glyph(TextShaper *s, uint32_t codepoint)
{
    for (int i = 0; i < s->glyph_count; i++) {
        if (s->glyphs[i].codepoint == codepoint) return &s->glyphs[i];
    }
    return NULL;
}

static GlyphEntry *add_glyph(TextShaper *s)
{
    if (s->glyph_count >= s->glyph_capacity) {
        int new_cap = s->glyph_capacity ? s->glyph_capacity * 2 : 256;
        GlyphEntry *ng = (GlyphEntry *)realloc(s->glyphs, new_cap * sizeof(GlyphEntry));
        if (!ng) return NULL;
        s->glyphs = ng;
        s->glyph_capacity = new_cap;
    }
    return &s->glyphs[s->glyph_count++];
}

/* The glyph atlas, made when the first glyph is drawn (rendering only):
 * fonts that are measured but never drawn need none.  NULL when out of
 * memory. */
static uint8_t *ts_atlas(TextShaper *s)
{
    if (!s->atlas) s->atlas = (uint8_t *)calloc(1, (size_t)s->atlas_w * (size_t)s->atlas_h);
    return s->atlas;
}

static bool pack_bitmap(TextShaper *s, int bw, int bh, int *out_x, int *out_y)
{
    if (bw <= 0 || bh <= 0) { *out_x = *out_y = 0; return true; }
    if (!ts_atlas(s)) return false;
    if (s->pack_x + bw > s->atlas_w) {
        s->pack_x = 0;
        s->pack_y += s->pack_row_h + 1;
        s->pack_row_h = 0;
    }
    if (s->pack_y + bh > s->atlas_h) {
        return false; /* atlas full */
    }
    *out_x = s->pack_x;
    *out_y = s->pack_y;
    s->pack_x += bw + 1;
    if (bh > s->pack_row_h) s->pack_row_h = bh;
    return true;
}

/* The font that draws a glyph: owner 0 is the shaper itself, owner k + 1 is
 * fallback k of the font registry. */
static TextShaper *ts_owner_shaper(const TextShaper *s, int owner)
{
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
        atomic_store_explicit(&page[cp & 255], v, memory_order_relaxed);
    }
    *glyph_index = (int)((v >> 32) & 0xFFFFFF) - 1;
    if (owner_out) *owner_out = (int)(v >> 56);
    uint32_t bits = (uint32_t)v;
    memcpy(xadvance, &bits, sizeof(*xadvance));
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

static GlyphEntry *ensure_glyph(TextShaper *s, uint32_t codepoint)
{
    GlyphEntry *g = find_glyph(s, codepoint);
    if (g) return g;

    int glyph_index = stbtt_FindGlyphIndex(&s->font, (int)codepoint);
    if (glyph_index == 0 && codepoint != ' ') {
        /* Missing glyph; fall back to the replacement character. */
        codepoint = 0xFFFD;
        glyph_index = stbtt_FindGlyphIndex(&s->font, (int)codepoint);
        if (find_glyph(s, codepoint)) return find_glyph(s, codepoint);
    }

    int advance, lsb;
    stbtt_GetGlyphHMetrics(&s->font, glyph_index, &advance, &lsb);

    int ix0, iy0, ix1, iy1;
    stbtt_GetGlyphBitmapBox(&s->font, glyph_index, s->scale, s->scale, &ix0, &iy0, &ix1, &iy1);
    int gw = ix1 - ix0;
    int bh = iy1 - iy0;
    int e = gw > 0 ? s->embolden : 0;
    int bw = gw + e;

    int px, py;
    if (!pack_bitmap(s, bw, bh, &px, &py)) return NULL;

    if (gw > 0 && bh > 0) {
        if (s->dirty_y1 <= s->dirty_y0) {
            s->dirty_y0 = py;
            s->dirty_y1 = py + bh;
        } else {
            if (py < s->dirty_y0) s->dirty_y0 = py;
            if (py + bh > s->dirty_y1) s->dirty_y1 = py + bh;
        }
        uint8_t *dst = s->atlas + py * s->atlas_w + px;
        stbtt_MakeGlyphBitmap(&s->font, dst, gw, bh, s->atlas_w, s->scale, s->scale, glyph_index);
        /* Synthetic bold: smear each row right by `e` pixels (max filter). */
        for (int y = 0; e > 0 && y < bh; y++) {
            uint8_t *row = dst + y * s->atlas_w;
            for (int x = bw - 1; x >= 0; x--) {
                uint8_t m = 0;
                for (int k = 0; k <= e; k++) {
                    int sx = x - k;
                    if (sx >= 0 && sx < gw && row[sx] > m) m = row[sx];
                }
                row[x] = m;
            }
        }
    }

    g = add_glyph(s);
    if (!g) return NULL;
    g->codepoint  = codepoint;
    g->glyph_index = glyph_index;
    g->u0 = (float)px / (float)s->atlas_w;
    g->v0 = (float)py / (float)s->atlas_h;
    g->u1 = (float)(px + bw) / (float)s->atlas_w;
    g->v1 = (float)(py + bh) / (float)s->atlas_h;
    g->bw = bw;
    g->bh = bh;
    g->xoff = (float)ix0;
    g->yoff = (float)iy0;
    g->xadvance = (float)advance * s->scale + (float)s->embolden;
    return g;
}

/* The atlas entry for a codepoint from whichever font draws it (see
 * ts_metric_owned); *owner_out is that font. */
static GlyphEntry *ensure_glyph_owned(const TextShaper *s, uint32_t codepoint, TextShaper **owner_out)
{
    int gi, owner;
    float xa;
    ts_metric_owned(s, codepoint, &gi, &xa, &owner);
    TextShaper *o = ts_owner_shaper(s, owner);
    if (!o) o = (TextShaper *)s;
    *owner_out = o;
    return ensure_glyph(o, codepoint);
}

/* Record a glyph command drawn from `owner`'s atlas. */
static bool ts_add_glyph(DisplayList *dl, const TextShaper *owner, float x, float y, float w, float h,
                         const GlyphEntry *ge, float r, float g, float b, float a)
{
    if (!display_list_add_glyph(dl, x, y, w, h, ge->u0, ge->v0, ge->u1, ge->v1,
                                (uint32_t)ge->glyph_index, r, g, b, a))
        return false;
    dl->cmds[dl->count - 1].u.glyph.font_slot = (uint8_t)(owner->slot < 0 ? 0 : owner->slot);
    return true;
}

static TextShaper *ts_sized(TextShaper *s, float px);

/* Glyph ge of owner (codepoint cp) drawn with its pen at (pen_x, base_y),
 * owner's size times scale: from owner rasterized at that size, where the
 * glyph's box falls on the right pixels (a scaled bitmap was a pixel or
 * more off at large sizes, and blurred), else owner's glyph scaled. */
static bool ts_draw_glyph(DisplayList *dl, TextShaper *owner, const GlyphEntry *ge, uint32_t cp, float pen_x,
                          float base_y, float scale, float r, float g, float b, float a)
{
    if (fabsf(scale - 1.0f) > 0.01f) {
        TextShaper *v = ts_sized(owner, owner->size_pixels * scale);
        GlyphEntry *vg = v ? ensure_glyph(v, cp) : NULL;
        if (vg)
            return ts_add_glyph(dl, v, pen_x + vg->xoff, base_y + vg->yoff, (float)vg->bw, (float)vg->bh, vg, r, g, b, a);
    }
    return ts_add_glyph(dl, owner, pen_x + ge->xoff * scale, base_y + ge->yoff * scale, (float)ge->bw * scale,
                        (float)ge->bh * scale, ge, r, g, b, a);
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
    s->slot = -1;
    s->embolden = embolden > 0 ? embolden : 0;
    s->size_pixels = size_pixels;
    s->atlas_w = TS_ATLAS_WIDTH;
    s->atlas_h = TS_ATLAS_HEIGHT;

    if (!stbtt_InitFont(&s->font, s->ttf_data, offset)) {
        text_shaper_destroy(s);
        return NULL;
    }
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

    /* Pre-pack ASCII so the atlas is immediately useful. */
    for (uint32_t cp = TS_PREPACK_FIRST; cp <= TS_PREPACK_LAST; cp++) {
        if (!ensure_glyph(s, cp)) {
            /* Non-fatal: a few glyphs may not fit, but the rest work. */
            break;
        }
    }
    return s;
}

void text_shaper_destroy(TextShaper *s)
{
    if (!s) return;
    for (int i = 0; i < s->nsized; i++) text_shaper_destroy(s->sized[i]);
    if (s->metric_pages) {
        for (int i = 0; i < TS_METRIC_PAGES; i++) free((void *)s->metric_pages[i]);
        free((void *)s->metric_pages);
    }
    free((void *)s->kern);
    free(s->glyphs);
    free(s->atlas);
    if (!s->ttf_borrowed) ts_release_data(s->ttf_data, s->ttf_mapped);
    free(s);
}

static int (*g_slot_alloc)(TextShaper *);

void text_shaper_set_slot_allocator(int (*alloc)(TextShaper *)) { g_slot_alloc = alloc; }

/* Base shaper s rasterized at px pixels (quantized to a quarter pixel): a
 * shaper of its own on the same font bytes, with an atlas and a slot of its
 * own (g_slot_alloc), made the first time; NULL when there is no room for
 * another (drawing scales s's glyphs then). */
static TextShaper *ts_sized(TextShaper *s, float px)
{
    if (!s || !g_slot_alloc || px < 4.0f || px > 512.0f) return NULL;
    px = roundf(px * 4.0f) / 4.0f;
    for (int i = 0; i < s->nsized; i++)
        if (s->sized[i]->size_pixels == px) return s->sized[i]->slot >= 0 ? s->sized[i] : NULL;
    if (s->nsized >= TS_SIZED_MAX) return NULL;
    TextShaper *v = (TextShaper *)calloc(1, sizeof(TextShaper));
    if (!v) return NULL;
    v->ttf_data = s->ttf_data;
    v->ttf_size = s->ttf_size;
    v->ttf_mapped = s->ttf_mapped;
    v->ttf_borrowed = true;
    v->font_offset = s->font_offset;
    v->slot = -1;
    v->embolden = s->embolden ? (int)roundf((float)s->embolden * px / s->size_pixels) : 0;
    if (s->embolden && v->embolden < 1) v->embolden = 1;
    v->size_pixels = px;
    v->atlas_w = TS_ATLAS_WIDTH;
    v->atlas_h = TS_ATLAS_HEIGHT;
    v->metric_pages = calloc(TS_METRIC_PAGES, sizeof(*v->metric_pages));
    v->kern = calloc(TS_KERN_SLOTS, sizeof(*v->kern));
    if (!v->metric_pages || !v->kern || !stbtt_InitFont(&v->font, v->ttf_data, v->font_offset)) {
        text_shaper_destroy(v);
        return NULL;
    }
    v->scale = stbtt_ScaleForMappingEmToPixels(&v->font, px);
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&v->font, &ascent, &descent, &line_gap);
    v->ascent = (float)ascent * v->scale;
    v->descent = (float)descent * v->scale;
    v->line_gap = (float)line_gap * v->scale;
    v->baseline = v->ascent;
    s->sized[s->nsized++] = v;          /* (kept even without a slot: not tried again) */
    if (g_slot_alloc(v) < 0) return NULL;
    return v;
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

const uint8_t *text_shaper_atlas_pixels(const TextShaper *s)
{
    return s ? ts_atlas((TextShaper *)s) : NULL;
}

int text_shaper_atlas_width(const TextShaper *s)
{
    return s ? s->atlas_w : 0;
}

int text_shaper_atlas_height(const TextShaper *s)
{
    return s ? s->atlas_h : 0;
}

int text_shaper_atlas_used_rows(const TextShaper *s)
{
    if (!s || !s->atlas) return 0;
    int rows = s->pack_y + s->pack_row_h;
    return rows < s->atlas_h ? rows : s->atlas_h;
}

bool text_shaper_atlas_take_dirty(TextShaper *s, int *y0, int *y1)
{
    if (!s || s->dirty_y1 <= s->dirty_y0) return false;
    *y0 = s->dirty_y0;
    *y1 = s->dirty_y1 < s->atlas_h ? s->dirty_y1 : s->atlas_h;
    s->dirty_y0 = s->dirty_y1 = 0;
    return true;
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

float text_shaper_zero_advance(const TextShaper *s)
{
    int adv, lsb;
    if (!s || !stbtt_FindGlyphIndex(&s->font, '0')) return 0.0f;
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
    const char *p = utf8;
    uint32_t cp;
    while ((cp = utf8_decode(&p)) != 0) {
        if (cp == '\n') { x = 0; prev_glyph = -1; continue; }
        int gi, owner;
        float xa;
        ts_metric_owned(s, cp, &gi, &xa, &owner);
        if (prev_glyph >= 0 && owner == 0) {
            x += (float)ts_kern(s, prev_glyph, gi) * s->scale;
        }
        x += xa;
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
        TextShaper *gs;
        GlyphEntry *ge = ensure_glyph_owned(s, cp, &gs);
        if (!ge) continue;
        if (prev_glyph >= 0 && gs == s) {
            pen_x += (float)ts_kern(s, prev_glyph, ge->glyph_index) * s->scale * scale;
        }
        if (!ts_draw_glyph(dl, gs, ge, cp, pen_x, pen_y, scale, r, g, b, a))
            return false;
        pen_x += ge->xadvance * scale;
        prev_glyph = gs == s ? ge->glyph_index : -1;
    }
    return true;
}

/* Measure the advance of the span [start, end) at the loaded size (unscaled). */
static float ts_measure_span(const TextShaper *s, const char *start, const char *end)
{
    float x = 0.0f;
    int prev_glyph = -1;
    const char *p = start;
    while (p < end) {
        const char *save = p;
        uint32_t cp = utf8_decode(&p);
        if (cp == 0 || p > end) { p = save; break; }
        int gi, owner;
        float xa;
        ts_metric_owned(s, cp, &gi, &xa, &owner);
        if (prev_glyph >= 0 && owner == 0) {
            x += (float)ts_kern(s, prev_glyph, gi) * s->scale;
        }
        x += xa;
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
        uint32_t gc = utf8_decode(&gp);
        if (gc == 0) break;
        if (gc < 0x20 && gc != ' ') continue; /* control chars: no glyph */
        TextShaper *gs;
        GlyphEntry *ge = ensure_glyph_owned(s, gc, &gs);
        if (!ge) continue;
        if (*prev_glyph >= 0 && gs == s) {
            *pen_x += (float)ts_kern(s, *prev_glyph, ge->glyph_index) * s->scale * scale;
        }
        if (!ts_draw_glyph(dl, gs, ge, gc, *pen_x, gy, scale, r, g, b, a))
            return false;
        *pen_x += ge->xadvance * scale;
        *prev_glyph = gs == s ? ge->glyph_index : -1;
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
                if (half_leading < 0.0f) half_leading = 0.0f;
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
                if (half_leading < 0.0f) half_leading = 0.0f;
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
                word_w = ts_measure_span(s, word_start, word_end) * scale;
                /* fall through: emit the remainder on the fresh line */
            } else {
            /* Wrap to the next line before this word.  Skip a leading space
             * on the new line: it belongs to the end of the old line. */
            float used = pen_x - line_start_x;
            if (used > max_width) max_width = used;
            lines++;
            ts_note_line(lines, word_start);
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
            word_w = ts_measure_span(s, word_start, word_end) * scale;
            }
        }

        /* Emit glyphs for [word_start, word_end).  Baseline placement follows
         * the CSS strut model: the em box is centered in the line box, so the
         * baseline sits at half-leading + ascent from the line top. */
        if (emit) {
            const char *gp = word_start;
            float half_leading = (line_advance - s->size_pixels * scale) * 0.5f;
            if (half_leading < 0.0f) half_leading = 0.0f;
            float gy = pen_y + half_leading + s->baseline * scale;
            while (gp < word_end) {
                uint32_t gc = utf8_decode(&gp);
                if (gc == 0) break;
                if (gc < 0x20 && gc != ' ') continue; /* control chars: no glyph */
                TextShaper *gs;
                GlyphEntry *ge = ensure_glyph_owned(s, gc, &gs);
                if (!ge) continue;
                if (prev_glyph >= 0 && gs == s) {
                    pen_x += (float)ts_kern(s, prev_glyph, ge->glyph_index) * s->scale * scale;
                }
                if (!ts_draw_glyph(dl, gs, ge, gc, pen_x, gy, scale, r, g, b, a))
                    return false;
                pen_x += ge->xadvance * scale;
                prev_glyph = gs == s ? ge->glyph_index : -1;
            }
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
            float cx = (cont2_from_line > 1 && lines >= cont2_from_line)
                     ? cont2_x : cont_x;
            float cw = (cont2_from_line > 1 && lines >= cont2_from_line)
                     ? cont2_width : cont_width;
            pen_x = cx;
            pen_y += line_advance;
            line_start_x = cx;
            limit = cw;
            prev_glyph = -1;
        }
    }

    float used = pen_x - line_start_x;
    if (used > max_width) max_width = used;

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

void text_shaper_set_slot(TextShaper *s, int slot)
{
    if (s) s->slot = slot;
}

int text_shaper_slot(const TextShaper *s)
{
    return s ? s->slot : -1;
}

bool text_shaper_has_glyph(const TextShaper *s, uint32_t codepoint)
{
    return s && stbtt_FindGlyphIndex(&s->font, (int)codepoint) != 0;
}

bool text_shaper_is_variable(const TextShaper *s)
{
    return s && stbtt__find_table(s->font.data, (stbtt_uint32)s->font.fontstart, "fvar") != 0;
}

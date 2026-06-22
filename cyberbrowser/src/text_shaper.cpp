#include "text_shaper.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

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

struct TextShaper {
    uint8_t        *ttf_data;
    size_t          ttf_size;
    stbtt_fontinfo  font;
    float           size_pixels;
    float           scale;
    float           ascent;
    float           descent;
    float           line_gap;
    float           baseline;

    uint8_t        *atlas;
    int             atlas_w;
    int             atlas_h;
    int             pack_x;
    int             pack_y;
    int             pack_row_h;

    GlyphEntry     *glyphs;
    int             glyph_count;
    int             glyph_capacity;
};

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

static bool pack_bitmap(TextShaper *s, int bw, int bh, int *out_x, int *out_y)
{
    if (bw <= 0 || bh <= 0) { *out_x = *out_y = 0; return true; }
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
    int bw = ix1 - ix0;
    int bh = iy1 - iy0;

    int px, py;
    if (!pack_bitmap(s, bw, bh, &px, &py)) return NULL;

    if (bw > 0 && bh > 0) {
        stbtt_MakeGlyphBitmap(&s->font, s->atlas + py * s->atlas_w + px,
                              bw, bh, s->atlas_w, s->scale, s->scale, glyph_index);
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
    g->xadvance = (float)advance * s->scale;
    return g;
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

    TextShaper *s = (TextShaper *)calloc(1, sizeof(TextShaper));
    if (!s) { free(data); return NULL; }

    s->ttf_data = data;
    s->ttf_size = (size_t)sz;
    s->size_pixels = size_pixels;
    s->atlas_w = TS_ATLAS_WIDTH;
    s->atlas_h = TS_ATLAS_HEIGHT;
    s->atlas = (uint8_t *)calloc(1, (size_t)s->atlas_w * (size_t)s->atlas_h);
    if (!s->atlas) { free(s); free(data); return NULL; }

    if (!stbtt_InitFont(&s->font, s->ttf_data, 0)) {
        text_shaper_destroy(s);
        return NULL;
    }

    s->scale = stbtt_ScaleForPixelHeight(&s->font, size_pixels);
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
    free(s->glyphs);
    free(s->atlas);
    free(s->ttf_data);
    free(s);
}

const uint8_t *text_shaper_atlas_pixels(const TextShaper *s)
{
    return s ? s->atlas : NULL;
}

int text_shaper_atlas_width(const TextShaper *s)
{
    return s ? s->atlas_w : 0;
}

int text_shaper_atlas_height(const TextShaper *s)
{
    return s ? s->atlas_h : 0;
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
        GlyphEntry *g = ensure_glyph((TextShaper *)s, cp);
        if (!g) continue;
        if (prev_glyph >= 0) {
            x += (float)stbtt_GetGlyphKernAdvance(&s->font, prev_glyph, g->glyph_index) * s->scale;
        }
        x += g->xadvance;
        prev_glyph = g->glyph_index;
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
    if (!s || !utf8 || !dl) return false;
    float pen_x = x;
    float pen_y = y + s->baseline;
    int prev_glyph = -1;
    const char *p = utf8;
    uint32_t cp;
    while ((cp = utf8_decode(&p)) != 0) {
        if (cp == '\n') {
            pen_x = x;
            pen_y += s->baseline - s->descent + s->line_gap;
            prev_glyph = -1;
            continue;
        }
        GlyphEntry *ge = ensure_glyph((TextShaper *)s, cp);
        if (!ge) continue;
        if (prev_glyph >= 0) {
            pen_x += (float)stbtt_GetGlyphKernAdvance(&s->font, prev_glyph, ge->glyph_index) * s->scale;
        }
        float gx = pen_x + ge->xoff;
        float gy = pen_y + ge->yoff;
        if (!display_list_add_glyph(dl, gx, gy, (float)ge->bw, (float)ge->bh,
                                    ge->u0, ge->v0, ge->u1, ge->v1,
                                    (uint32_t)ge->glyph_index,
                                    r, g, b, a)) {
            return false;
        }
        pen_x += ge->xadvance;
        prev_glyph = ge->glyph_index;
    }
    return true;
}

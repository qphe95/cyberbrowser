/* SVG rasterization through PlutoSVG (svg_render.h). */
#include "svg_render.h"

#include <plutosvg.h>
#include <stdlib.h>
#include <string.h>

/* PlutoSVG keeps pointers into the source, so documents parse a copy.  The
 * document owns it from here: a failed parse destroys the document, which
 * frees the copy. */
/* (cw, ch: the size its percentages are of; -1: none) */
static plutosvg_document_t *load_in(const char *data, size_t len, float cw, float ch);
static plutosvg_document_t *load(const char *data, size_t len) { return load_in(data, len, -1.0f, -1.0f); }
static plutosvg_document_t *load_in(const char *data, size_t len, float cw, float ch)
{
    if (!data || !len || len > 0x7fffffff) return NULL;
    char *copy = (char *)malloc(len);
    if (!copy) return NULL;
    memcpy(copy, data, len);
    return plutosvg_document_load_from_data(copy, (int)len, cw, ch, free, copy);
}

bool svg_intrinsic_size(const char *data, size_t len, float *w, float *h)
{
    *w = *h = 0.0f;
    plutosvg_document_t *doc = load(data, len);
    if (!doc) return false;
    *w = plutosvg_document_get_width(doc);
    *h = plutosvg_document_get_height(doc);
    plutosvg_document_destroy(doc);
    return true;
}

bool svg_render_rgba(const char *data, size_t len, int w, int h, uint32_t current_color,
                     uint8_t **out, int *out_w, int *out_h)
{
    *out = NULL;
    *out_w = *out_h = 0;
    /* (drawn at a size: its root's percentages of that) */
    plutosvg_document_t *doc = w > 0 && h > 0 ? load_in(data, len, (float)w, (float)h) : load(data, len);
    if (!doc) return false;
    plutovg_color_t cc;
    plutovg_color_init_rgba8(&cc, (int)(current_color >> 24), (int)(current_color >> 16 & 0xFF),
                             (int)(current_color >> 8 & 0xFF), (int)(current_color & 0xFF));
    plutovg_surface_t *s = plutosvg_document_render_to_surface(doc, NULL, w > 0 ? w : -1, h > 0 ? h : -1,
                                                               &cc, NULL, NULL);
    plutosvg_document_destroy(doc);
    if (!s) return false;
    int sw = plutovg_surface_get_width(s), sh = plutovg_surface_get_height(s);
    int stride = plutovg_surface_get_stride(s);
    uint8_t *px = (sw > 0 && sh > 0) ? (uint8_t *)malloc((size_t)sw * (size_t)sh * 4) : NULL;
    if (px) {
        /* Rows to a tight RGBA buffer, then un-premultiply in place. */
        const uint8_t *src = plutovg_surface_get_data(s);
        for (int y = 0; y < sh; y++) memcpy(px + (size_t)y * sw * 4, src + (size_t)y * stride, (size_t)sw * 4);
        plutovg_convert_argb_to_rgba(px, px, sw, sh, sw * 4);
        *out = px;
        *out_w = sw;
        *out_h = sh;
    }
    plutovg_surface_destroy(s);
    return px != NULL;
}

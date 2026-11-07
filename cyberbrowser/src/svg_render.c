/* SVG rasterization through PlutoSVG (svg_render.h). */
#include "svg_render.h"

#include "css_props.h"

#include <plutosvg.h>
#include <ctype.h>
#include <strings.h>
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

/* (the root <svg>'s background color from its style attribute, the CSS
 * background an image's root element paints under it; false: none) */
static bool root_background(const char *data, size_t len, double rgba[4])
{
    const char *end = data + len, *p = data;
    for (; p + 4 < end; p++)
        if (p[0] == '<' && !strncasecmp(p + 1, "svg", 3) && (isspace((unsigned char)p[4]) || p[4] == '>')) break;
    if (p + 4 >= end) return false;
    const char *gt = memchr(p, '>', (size_t)(end - p));
    if (!gt) return false;
    for (const char *q = p; q + 6 < gt; q++) {
        if (strncasecmp(q, "style", 5) || !(q == p || isspace((unsigned char)q[-1]))) continue;
        const char *v = q + 5;
        while (v < gt && isspace((unsigned char)*v)) v++;
        if (v >= gt || *v != '=') continue;
        v++;
        while (v < gt && isspace((unsigned char)*v)) v++;
        char quote = *v == '"' || *v == '\'' ? *v++ : 0;
        const char *ve = v;
        while (ve < gt && (quote ? *ve != quote : !isspace((unsigned char)*ve))) ve++;
        for (const char *b = v; b + 10 < ve; b++) {
            if (strncasecmp(b, "background", 10)) continue;
            const char *c = b + 10;
            if (!strncasecmp(c, "-color", 6)) c += 6;
            while (c < ve && isspace((unsigned char)*c)) c++;
            if (c >= ve || *c != ':') continue;
            c++;
            const char *ce = c;
            while (ce < ve && *ce != ';') ce++;
            char buf[128];
            size_t n = (size_t)(ce - c) < sizeof buf - 1 ? (size_t)(ce - c) : sizeof buf - 1;
            memcpy(buf, c, n);
            buf[n] = 0;
            char *t = buf;
            while (isspace((unsigned char)*t)) t++;
            size_t tl = strlen(t);
            while (tl && isspace((unsigned char)t[tl - 1])) t[--tl] = 0;
            return css_color_parse(t, rgba) && rgba[3] > 0.0;
        }
        return false;
    }
    return false;
}

/* (the root <svg>'s viewBox giving no area, a zero width or height: it renders nothing) */
static bool root_viewbox_empty(const char *data, size_t len)
{
    const char *end = data + len, *p = data;
    for (; p + 4 < end; p++)
        if (p[0] == '<' && !strncasecmp(p + 1, "svg", 3) && (isspace((unsigned char)p[4]) || p[4] == '>')) break;
    if (p + 4 >= end) return false;
    const char *gt = memchr(p, '>', (size_t)(end - p));
    if (!gt) return false;
    for (const char *q = p; q + 8 < gt; q++) {
        if (strncmp(q, "viewBox", 7)) continue;
        const char *v = q + 7;
        while (v < gt && (isspace((unsigned char)*v) || *v == '=' || *v == '"' || *v == '\'')) v++;
        double n[4];
        int k = 0;
        while (k < 4 && v < gt) {
            char *e;
            n[k] = strtod(v, &e);
            if (e == v) break;
            k++;
            v = e;
            while (v < gt && (isspace((unsigned char)*v) || *v == ',')) v++;
        }
        return k == 4 && (n[2] <= 0.0 || n[3] <= 0.0);
    }
    return false;
}

bool svg_render_rgba(const char *data, size_t len, int w, int h, uint32_t current_color,
                     uint8_t **out, int *out_w, int *out_h)
{
    *out = NULL;
    *out_w = *out_h = 0;
    if (root_viewbox_empty(data, len)) return false;
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
        /* (the root's background under what it draws) */
        double bg[4];
        if (root_background(data, len, bg)) {
            double ba = bg[3] > 1.0 ? 1.0 : bg[3];
            for (size_t k = 0; k < (size_t)sw * (size_t)sh; k++) {
                uint8_t *q = px + k * 4;
                double sa = q[3] / 255.0, oa = sa + ba * (1.0 - sa);
                for (int ch = 0; ch < 3; ch++) {
                    double c = oa > 0.0 ? (q[ch] * sa + bg[ch] * ba * (1.0 - sa)) / oa : 0.0;
                    q[ch] = (uint8_t)(c < 0.0 ? 0 : c > 255.0 ? 255 : c + 0.5);
                }
                q[3] = (uint8_t)(oa * 255.0 + 0.5);
            }
        }
        *out = px;
        *out_w = sw;
        *out_h = sh;
    }
    plutovg_surface_destroy(s);
    return px != NULL;
}

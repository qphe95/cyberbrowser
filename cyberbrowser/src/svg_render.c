/* SVG rasterization through PlutoSVG (svg_render.h). */
#include "svg_render.h"

#include "css_props.h"

#include <plutosvg.h>
#include "html_dom.h"
#include "svg_filter.h"
#include "css_layout.h"
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

/* (a background or background-color declaration's color in [b, e): false, none) */
static bool decl_background(const char *v, const char *ve, double rgba[4])
{
    for (const char *b = v; b + 10 < ve; b++) {
        if (strncasecmp(b, "background", 10)) continue;
        const char *c = b + 10;
        if (!strncasecmp(c, "-color", 6)) c += 6;
        while (c < ve && isspace((unsigned char)*c)) c++;
        if (c >= ve || *c != ':') continue;
        c++;
        const char *ce = c;
        while (ce < ve && *ce != ';' && *ce != '}') ce++;
        char buf[128];
        size_t n = (size_t)(ce - c) < sizeof buf - 1 ? (size_t)(ce - c) : sizeof buf - 1;
        memcpy(buf, c, n);
        buf[n] = 0;
        char *t = buf;
        while (isspace((unsigned char)*t)) t++;
        size_t tl = strlen(t);
        while (tl && isspace((unsigned char)t[tl - 1])) t[--tl] = 0;
        if (css_color_parse(t, rgba) && rgba[3] > 0.0) return true;
    }
    return false;
}

/* (the root's background from the image's own <style> sheets: a :root or svg rule's; never a linked sheet's,
 * which an image does not load) */
static bool sheet_root_background(const char *data, size_t len, double rgba[4])
{
    const char *end = data + len;
    bool got = false;
    for (const char *p = data; p + 6 < end; p++) {
        if (p[0] != '<' || strncasecmp(p + 1, "style", 5) || !(isspace((unsigned char)p[6]) || p[6] == '>')) continue;
        const char *gt = memchr(p, '>', (size_t)(end - p));
        if (!gt) break;
        const char *se = gt + 1;
        while (se + 7 < end && !(se[0] == '<' && se[1] == '/' && !strncasecmp(se + 2, "style", 5))) se++;
        for (const char *r = gt + 1; r < se;) {
            const char *ob = memchr(r, '{', (size_t)(se - r));
            if (!ob) break;
            const char *cb = memchr(ob, '}', (size_t)(se - ob));
            if (!cb) break;
            /* (its selector list: :root or svg on its own among them) */
            bool hit = false;
            const char *a = r;
            while (a < ob) {
                while (a < ob && (isspace((unsigned char)*a) || *a == ',')) a++;
                const char *ae = a;
                while (ae < ob && *ae != ',') ae++;
                const char *te = ae;
                while (te > a && isspace((unsigned char)te[-1])) te--;
                size_t sl = (size_t)(te - a);
                if ((sl == 5 && !strncasecmp(a, ":root", 5)) || (sl == 3 && !strncasecmp(a, "svg", 3))) hit = true;
                a = ae;
            }
            double c4[4];
            if (hit && decl_background(ob + 1, cb, c4)) { memcpy(rgba, c4, sizeof c4); got = true; }
            r = cb + 1;
        }
        p = se;
    }
    return got;
}

/* (the root <svg>'s background color from its style attribute, the CSS
 * background an image's root element paints under it; false: none) */
static bool root_attr_background(const char *data, size_t len, double rgba[4])
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

static bool root_background(const char *data, size_t len, double rgba[4])
{
    /* (its style attribute's, else its own sheets') */
    return root_attr_background(data, len, rgba) || sheet_root_background(data, len, rgba);
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

/* ---- filters in an SVG image: its own document's <filter>s, attributes only ---- */
static __thread HtmlDocument *t_img_doc;
static __thread char t_img_cif[16];   /* (the root <svg>'s color-interpolation-filters: inherited) */
static bool svg_has(const char *d, size_t n, const char *needle)
{
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= n; i++) if (d[i] == needle[0] && !memcmp(d + i, needle, k)) return true;
    return false;
}
static const char *img_flt_prop(void *ud, HtmlNode *n, const char *name, char *buf, size_t bufn)
{
    (void)ud;
    if (!n) return NULL;
    const char *v = NULL, *style = NULL;
    for (const HtmlAttribute *a = n->attributes; a; a = a->next) {
        if (!strcasecmp(a->name, name)) v = a->value;
        else if (!strcasecmp(a->name, "style")) style = a->value;
    }
    size_t nl = strlen(name);
    for (const char *q = style; q && (q = strstr(q, name)) != NULL; q += nl) {
        const char *r = q + nl;
        while (*r == ' ') r++;
        if (*r != ':') continue;
        r++;
        while (*r == ' ') r++;
        size_t l = strcspn(r, ";");
        while (l > 0 && r[l - 1] == ' ') l--;
        if (l >= bufn) l = bufn - 1;
        memcpy(buf, r, l);
        buf[l] = 0;
        return buf;
    }
    if (!v && !strcmp(name, "color-interpolation-filters") && t_img_cif[0]) v = t_img_cif;
    return v;
}
static bool img_flt_color(void *ud, const char *text, double *rgba) { (void)ud; return css_layout_parse_color_text(text, rgba); }
static void img_filter_hook(const char *value, const float box[4], unsigned char *rgba, int w, int h)
{
    if (!t_img_doc) return;
    SvgFilterEnv env = { t_img_doc, img_flt_prop, img_flt_color, NULL, NULL, NULL, { 0, 0, 0, 1 }, true, NULL, { plutosvg_filter_origin[0], plutosvg_filter_origin[1] } };
    svg_filter_apply(&env, value, box, 16.0f, rgba, w, h);
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
    /* (filters: the hook runs them against the image's own document) */
    plutosvg_filter_hook_t keep_hook = plutosvg_filter_hook;
    HtmlDocument *fdoc = NULL;
    if (!keep_hook && len < (4u << 20) && svg_has(data, len, "<filter")) fdoc = html_parse(data, len);   /* (a hook already set: the document's own, for inline SVG) */
    if (fdoc) {
        t_img_doc = fdoc;
        plutosvg_filter_hook = img_filter_hook;
        plutosvg_filter_margin = 128;   /* (an image: what is drawn past its edges can blur in, as browsers draw it) */
        t_img_cif[0] = 0;
        const char *rt = NULL;
        for (size_t i = 0; i + 4 < len; i++) if (data[i] == '<' && !strncmp(data + i + 1, "svg", 3)) { rt = data + i; break; }
        const char *gt = rt ? memchr(rt, '>', len - (size_t)(rt - data)) : NULL;
        for (const char *q = rt; q && gt && q + 27 < gt; q++) {
            if (strncmp(q, "color-interpolation-filters", 27)) continue;
            const char *v = q + 27;
            while (v < gt && (*v == '=' || *v == '"' || *v == '\'' || *v == ' ')) v++;
            size_t k = 0;
            while (v + k < gt && k + 1 < sizeof t_img_cif && isalnum((unsigned char)v[k])) { t_img_cif[k] = v[k]; k++; }
            t_img_cif[k] = 0;
            break;
        }
    }
    plutovg_surface_t *s = plutosvg_document_render_to_surface(doc, NULL, w > 0 ? w : -1, h > 0 ? h : -1,
                                                               &cc, NULL, NULL);
    if (fdoc) { plutosvg_filter_hook = keep_hook; plutosvg_filter_margin = 0; t_img_doc = NULL; html_document_free(fdoc); }
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

/*
 * canvas2d.c - the pixels of 2D canvases (HTML 4.12.5.1).
 *
 * Each <canvas> with a 2D context (keyed by its NodeId) and each
 * OffscreenCanvas / ImageBitmap (keyed by ids from C2D_OFF_BASE up) owns a
 * bitmap of straight (unpremultiplied) RGBA bytes; the arithmetic is on
 * premultiplied floats, rounded once on the way back (as SVG filters do).  canvas_api.js keeps the drawing
 * state and hands each drawing operation over: a path already in device
 * pixels, its paint, and the state that matters for it.
 *
 * A drawing operation is done as the standard describes it: plutovg
 * rasterizes the shape's coverage, the paint (color, linear / radial /
 * conic gradient, pattern or image) fills it to make the source image,
 * the shadow is made from that, and both are composited with the global
 * alpha and the composite operation (every Porter-Duff operator and blend
 * mode) inside the clipping region, a coverage mask of its own.
 *
 * display_list.c asks canvas2d_paint_key for a canvas's image: the bitmap
 * is put in the image cache under "cyber-c2d:<id>" when it changed.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include <plutovg.h>

#include "dom_js.h"
#include "dom_store.h"
#include "display_list.h"
#include "image_cache.h"
#include "font_registry.h"
#include "text_shaper.h"
#include "browser_api_impl_types.h"
#include "canvas2d.h"
#include "bidi.h"

#define C2D_MAX_PX (16u * 1024u * 1024u)

typedef struct C2dLayer {
    uint8_t *px;            /* (the bitmap drawn on before this layer began) */
    uint8_t *clip;          /* (its clip then) */
    int depth;              /* (the save depth it began at) */
    double alpha;           /* (how it is composited back: global alpha, operator, shadow, filter) */
    int op;
    double sh[7];
    double *filter; int nfilter;
    bool discard;           /* (opened with a non-invertible transform: never drawn) */
    bool own_filter;        /* (the filter given to beginLayer: global alpha after it; ctx.filter: before) */
    int w, h, ox, oy;       /* (the bitmap under it: size and origin) */
} C2dLayer;

typedef struct C2dSurf {
    uint64_t id;
    int w, h;
    uint8_t *px;            /* straight RGBA */
    uint8_t *clip;          /* coverage 0..255 per pixel; NULL: none */
    uint8_t **stack;        /* clips saved */
    int nstack, capstack;
    C2dLayer *layers;
    int nlayers, caplayers;
    bool opaque, dirty, shown;
    int ox, oy;             /* (inside a filtered layer: where the canvas origin is in px, which has a margin round it) */
    uint32_t gen;
    uint64_t epoch;
    /* (drawElementImage: the children drawn, where, in bitmap pixels) */
    struct { uint32_t el; float x, y, w, h, a, sx, sy, sw, sh; unsigned char fn, ft[8]; float fv[8], ds[7]; } *ed;   /* (s*: the part of it drawn, CSS px from its border box; sw 0 the box; f*: its filter as CSS filter's, ds a drop shadow) */
    int ned, caped;         /* (unique over the process: its image cache key) */
} C2dSurf;

static C2dSurf *g_s;
static int g_n, g_cap;
static uint64_t g_next_off = C2D_OFF_BASE;
static uint64_t g_epoch;

/* the coverage scratch */
static plutovg_surface_t *g_sc;
static plutovg_canvas_t *g_cv;
static int g_scw, g_sch;

static C2dSurf *c2d_find(uint64_t id)
{
    for (int i = 0; i < g_n; i++) if (g_s[i].id == id) return &g_s[i];
    return NULL;
}

static void c2d_free_state(C2dSurf *s)
{
    for (int i = 0; i < s->nstack; i++) free(s->stack[i]);
    s->nstack = 0;
    for (int i = 0; i < s->nlayers; i++) { free(s->layers[i].px); free(s->layers[i].clip); free(s->layers[i].filter); }
    s->nlayers = 0;
    free(s->clip); s->clip = NULL;
}

static C2dSurf *c2d_make(uint64_t id, int w, int h, bool opaque)
{
    C2dSurf *s = c2d_find(id);
    if (!s) {
        if (g_n == g_cap) {
            int nc = g_cap ? g_cap * 2 : 16;
            C2dSurf *nv = (C2dSurf *)realloc(g_s, (size_t)nc * sizeof *nv);
            if (!nv) return NULL;
            g_s = nv; g_cap = nc;
        }
        s = &g_s[g_n++];
        memset(s, 0, sizeof *s);
        s->id = id;
    }
    c2d_free_state(s);
    s->ned = 0;
    s->ox = s->oy = 0;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if ((uint64_t)w * (uint64_t)h > C2D_MAX_PX) w = h = 0;
    if (s->w != w || s->h != h || !s->px) {
        free(s->px);
        s->px = (w && h) ? (uint8_t *)calloc((size_t)w * h, 4) : NULL;
        s->w = s->px ? w : 0; s->h = s->px ? h : 0;
    } else memset(s->px, 0, (size_t)w * h * 4);
    s->opaque = opaque;
    if (opaque && s->px) for (size_t i = 0; i < (size_t)s->w * s->h; i++) s->px[i * 4 + 3] = 255;
    s->dirty = true;
    s->gen++;
    s->epoch = ++g_epoch;
    return s;
}

static void c2d_changed(C2dSurf *s)
{
    s->dirty = true;
    if (s->id < C2D_OFF_BASE && s->id) {
        dom_mark_dirty((NodeId)s->id, DOMF_DIRTY_ATTR);
        dom_request_layout();
    }
}

void canvas2d_reset_all(void)
{
    for (int i = 0; i < g_n; i++) { c2d_free_state(&g_s[i]); free(g_s[i].px); free(g_s[i].stack); free(g_s[i].layers); free(g_s[i].ed); }
    g_n = 0;
    g_next_off = C2D_OFF_BASE;
}

bool canvas2d_has(uint32_t id)
{
    C2dSurf *s = c2d_find(id);
    return s && s->px && s->shown;
}

bool canvas2d_paint_key(uint32_t id, char *key, size_t cap)
{
    C2dSurf *s = c2d_find(id);
    if (!s || !s->px || !s->shown) return false;
    snprintf(key, cap, "cyber-c2d:%llu", (unsigned long long)s->epoch);
    ImageCache *ic = display_list_get_image_cache();
    if (!ic) return false;
    if (s->dirty) {
        int bw_ = s->nlayers ? s->layers[0].w : s->w, bh_ = s->nlayers ? s->layers[0].h : s->h;
        size_t n = (size_t)bw_ * bh_;
        const uint8_t *base = s->nlayers ? s->layers[0].px : s->px;
        uint8_t *rgba = (uint8_t *)malloc(n * 4);
        if (!rgba) return false;
        for (size_t i = 0; i < n; i++) {
            const uint8_t *p = base + i * 4;
            uint8_t *q = rgba + i * 4;
            if (s->opaque) { for (int e = 0; e < 3; e++) q[e] = (uint8_t)((p[e] * p[3] + 127) / 255); q[3] = 255; continue; }
            memcpy(q, p, 4);
        }
        image_cache_put_rgba(ic, key, bw_, bh_, rgba);
        free(rgba);
        s->dirty = false;
    }
    return true;
}

/* ---- coverage ---- */
static bool c2d_scratch(int w, int h)
{
    if (g_sc && g_scw >= w && g_sch >= h) return true;
    if (g_cv) plutovg_canvas_destroy(g_cv);
    if (g_sc) plutovg_surface_destroy(g_sc);
    int nw = w > g_scw ? w : g_scw, nh = h > g_sch ? h : g_sch;
    g_sc = plutovg_surface_create(nw, nh);
    if (!g_sc) { g_cv = NULL; g_scw = g_sch = 0; return false; }
    g_cv = plutovg_canvas_create(g_sc);
    g_scw = nw; g_sch = nh;
    return g_cv != NULL;
}

typedef struct { int x0, y0, x1, y1; } C2dBox;

/* The stroke settings in a state array. */
typedef struct {
    double alpha; int op;
    double lw; int cap, join; double miter, dash_off; int ndash; const double *dash;
    double sh[7];           /* shadow r, g, b (0..255), a (0..1), offset x, y, blur */
    int rule; int smooth;
    double m[6];
    const double *filter; int nfilter;
    int quality;
} C2dState;

static bool c2d_state(const double *st, int n, C2dState *o)
{
    memset(o, 0, sizeof *o);
    if (n < 8) return false;
    o->alpha = st[0]; o->op = (int)st[1];
    o->lw = st[2]; o->cap = (int)st[3]; o->join = (int)st[4]; o->miter = st[5]; o->dash_off = st[6];
    o->ndash = (int)st[7];
    if (o->ndash < 0 || 8 + o->ndash + 16 > n) return false;
    o->dash = st + 8;
    int b = 8 + o->ndash;
    for (int i = 0; i < 7; i++) o->sh[i] = st[b + i];
    o->rule = (int)st[b + 7]; o->smooth = (int)st[b + 8];
    for (int i = 0; i < 6; i++) o->m[i] = st[b + 9 + i];
    o->nfilter = (int)st[b + 15];
    if (o->nfilter < 0 || b + 16 + o->nfilter > n) o->nfilter = 0;
    o->filter = st + b + 16;
    if (b + 16 + o->nfilter < n) o->quality = (int)st[b + 16 + o->nfilter];
    return true;
}

static void c2d_path_build(plutovg_path_t *p, const double *a, int n)
{
    int i = 0;
    while (i < n) {
        int op = (int)a[i++];
        if (op == 0 && i + 2 <= n) { plutovg_path_move_to(p, (float)a[i], (float)a[i + 1]); i += 2; }
        else if (op == 1 && i + 2 <= n) { plutovg_path_line_to(p, (float)a[i], (float)a[i + 1]); i += 2; }
        else if (op == 2 && i + 6 <= n) { plutovg_path_cubic_to(p, (float)a[i], (float)a[i + 1], (float)a[i + 2], (float)a[i + 3], (float)a[i + 4], (float)a[i + 5]); i += 6; }
        else if (op == 3) plutovg_path_close(p);
        else break;
    }
}

/* Rasterize path p (already in the space matrix m maps to device pixels) into the
 * scratch's alpha: filled, or stroked with st's line settings.  The box it covers
 * (inside w x h) in *bx; false when it covers nothing. */
/* (the last cover's extents, before cutting to the bitmap) */
static int g_cover_ext[4];
static bool c2d_cover(const plutovg_path_t *p, const plutovg_matrix_t *m, bool stroke, const C2dState *st, int w, int h, C2dBox *bx)
{
    if (!c2d_scratch(w, h)) return false;
    plutovg_canvas_save(g_cv);
    plutovg_canvas_reset_matrix(g_cv);
    plutovg_canvas_clip_rect(g_cv, 0, 0, (float)w, (float)h);
    plutovg_canvas_set_matrix(g_cv, m);
    plutovg_canvas_new_path(g_cv);
    plutovg_canvas_add_path(g_cv, p);
    plutovg_canvas_set_rgba(g_cv, 1, 1, 1, 1);
    plutovg_canvas_set_operator(g_cv, PLUTOVG_OPERATOR_SRC_OVER);
    plutovg_rect_t r = { 0, 0, 0, 0 };
    if (stroke) {
        plutovg_canvas_set_line_width(g_cv, (float)st->lw);
        plutovg_canvas_set_line_cap(g_cv, st->cap == 1 ? PLUTOVG_LINE_CAP_ROUND : st->cap == 2 ? PLUTOVG_LINE_CAP_SQUARE : PLUTOVG_LINE_CAP_BUTT);
        plutovg_canvas_set_line_join(g_cv, st->join == 1 ? PLUTOVG_LINE_JOIN_ROUND : st->join == 2 ? PLUTOVG_LINE_JOIN_BEVEL : PLUTOVG_LINE_JOIN_MITER);
        plutovg_canvas_set_miter_limit(g_cv, (float)st->miter);
        float dd[64];
        int nd = st->ndash > 64 ? 64 : st->ndash;
        double tot = 0;
        for (int i = 0; i < nd; i++) { dd[i] = (float)st->dash[i]; tot += st->dash[i]; }
        plutovg_canvas_set_dash(g_cv, (float)st->dash_off, dd, tot > 0 ? nd : 0);
        plutovg_canvas_stroke_extents(g_cv, &r);
    } else {
        plutovg_canvas_set_fill_rule(g_cv, st && st->rule ? PLUTOVG_FILL_RULE_EVEN_ODD : PLUTOVG_FILL_RULE_NON_ZERO);
        plutovg_canvas_fill_extents(g_cv, &r);
    }
    int x0 = (int)floorf(r.x), y0 = (int)floorf(r.y), x1 = (int)ceilf(r.x + r.w), y1 = (int)ceilf(r.y + r.h);
    g_cover_ext[0] = x0; g_cover_ext[1] = y0; g_cover_ext[2] = x1; g_cover_ext[3] = y1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0 || !(r.w > 0) || !(r.h > 0)) { plutovg_canvas_new_path(g_cv); plutovg_canvas_restore(g_cv); return false; }
    if (stroke) plutovg_canvas_stroke(g_cv); else plutovg_canvas_fill(g_cv);
    plutovg_canvas_restore(g_cv);
    bx->x0 = x0; bx->y0 = y0; bx->x1 = x1; bx->y1 = y1;
    return true;
}

static inline uint8_t c2d_cov_at(int x, int y)
{
    const uint8_t *d = plutovg_surface_get_data(g_sc);
    int stride = plutovg_surface_get_stride(g_sc);
    return d[(size_t)y * stride + (size_t)x * 4 + 3];
}

static void c2d_cover_clear(const C2dBox *b)
{
    uint8_t *d = plutovg_surface_get_data(g_sc);
    int stride = plutovg_surface_get_stride(g_sc);
    for (int y = b->y0; y < b->y1; y++) memset(d + (size_t)y * stride + (size_t)b->x0 * 4, 0, (size_t)(b->x1 - b->x0) * 4);
}

/* ---- paints ---- */
typedef struct { const uint8_t *px; int w, h, ch; bool premul; } C2dImg;

typedef struct {
    int kind;               /* 0 color, 1 linear, 2 radial, 3 conic, 4 pattern / image */
    float col[4];           /* (premultiplied) */
    double inv[6];          /* device -> paint space */
    double g[6];
    float lut[1024][4];     /* (gradients: premultiplied) */
    C2dImg img;
    int rx, ry;             /* (patterns: repeat on x, y) */
    double cl[4];           /* (images: the source rectangle samples stay in) */
    int clamp, smooth;
    bool none;              /* (paints nothing: a degenerate gradient) */
    int ox, oy;             /* (the bitmap's origin offset) */
} C2dPaint;

static bool c2d_invert(const double *m, double *o)
{
    double det = m[0] * m[3] - m[1] * m[2];
    if (!isfinite(det) || fabs(det) < 1e-12) return false;
    o[0] = m[3] / det; o[1] = -m[1] / det; o[2] = -m[2] / det; o[3] = m[0] / det;
    o[4] = -(o[0] * m[4] + o[2] * m[5]); o[5] = -(o[1] * m[4] + o[3] * m[5]);
    return true;
}

static void c2d_lut(C2dPaint *p, const double *stops, int ns)
{
    /* stops: (offset, r, g, b, a) with r, g, b 0..255, a 0..1, in order */
    for (int k = 0; k < 1024; k++) {
        double t = k / 1023.0;
        double c[4];
        if (ns <= 0) { c[0] = c[1] = c[2] = c[3] = 0; }
        else if (t <= stops[0]) { for (int q = 0; q < 4; q++) c[q] = stops[1 + q]; }
        else if (t >= stops[(ns - 1) * 5]) { for (int q = 0; q < 4; q++) c[q] = stops[(ns - 1) * 5 + 1 + q]; }
        else {
            int i = 0;
            while (i + 1 < ns && stops[(i + 1) * 5] <= t) i++;
            const double *a = stops + i * 5, *b = stops + (i + 1 < ns ? i + 1 : i) * 5;
            double f = b[0] > a[0] ? (t - a[0]) / (b[0] - a[0]) : 1.0;
            for (int q = 0; q < 4; q++) c[q] = a[1 + q] + (b[1 + q] - a[1 + q]) * f;
        }
        p->lut[k][3] = (float)c[3];
        for (int q = 0; q < 3; q++) p->lut[k][q] = (float)(c[q] / 255.0 * c[3]);
    }
}

static void c2d_img_of(JSContextHandle ctx, GCValue src, C2dImg *o);

/* paint array: [kind, m0..m5, ...] where m maps paint space to device pixels */
static bool c2d_paint(JSContextHandle ctx, const double *a, int n, GCValue src, int smooth, C2dPaint *p)
{
    memset(p, 0, sizeof *p);
    p->smooth = smooth;
    if (n < 1) return false;
    p->kind = (int)a[0];
    if (p->kind == 0) {
        if (n < 5) return false;
        double al = a[4] < 0 ? 0 : a[4] > 1 ? 1 : a[4];
        p->col[3] = (float)al;
        for (int q = 0; q < 3; q++) p->col[q] = (float)(a[1 + q] / 255.0 * al);
        return true;
    }
    if (n < 7) return false;
    if (!c2d_invert(a + 1, p->inv)) { p->none = true; return true; }
    if (p->kind >= 1 && p->kind <= 3) {
        int ng = p->kind == 2 ? 6 : p->kind == 1 ? 4 : 3;
        if (n < 7 + ng + 1) return false;
        for (int i = 0; i < ng; i++) p->g[i] = a[7 + i];
        int ns = (int)a[7 + ng];
        if (ns < 0 || 8 + ng + ns * 5 > n) return false;
        if (ns == 0) { p->none = true; return true; }
        c2d_lut(p, a + 8 + ng, ns);
        if (p->kind == 1 && p->g[0] == p->g[2] && p->g[1] == p->g[3]) p->none = true;
        if (p->kind == 2 && p->g[0] == p->g[3] && p->g[1] == p->g[4] && p->g[2] == p->g[5]) p->none = true;
        return true;
    }
    if (p->kind == 4) {
        if (n < 13) return false;
        int rep = (int)a[7];
        p->rx = rep == 0 || rep == 1;
        p->ry = rep == 0 || rep == 2;
        p->clamp = rep == 4;
        for (int i = 0; i < 4; i++) p->cl[i] = a[8 + i];
        if (n > 12) p->smooth = (int)a[12];
        c2d_img_of(ctx, src, &p->img);
        if (!p->img.px || p->img.w <= 0 || p->img.h <= 0) p->none = true;
        return true;
    }
    return false;
}

static inline void c2d_texel(const C2dImg *im, int x, int y, float *o)
{
    const uint8_t *q = im->px + ((size_t)y * im->w + x) * im->ch;
    if (im->ch == 4) {
        if (im->premul) { o[0] = q[0] / 255.f; o[1] = q[1] / 255.f; o[2] = q[2] / 255.f; o[3] = q[3] / 255.f; }
        else { float al = q[3] / 255.f; o[0] = q[0] / 255.f * al; o[1] = q[1] / 255.f * al; o[2] = q[2] / 255.f * al; o[3] = al; }
    } else if (im->ch == 3) { o[0] = q[0] / 255.f; o[1] = q[1] / 255.f; o[2] = q[2] / 255.f; o[3] = 1; }
    else { o[0] = o[1] = o[2] = q[0] / 255.f; o[3] = 1; }
}

static inline int c2d_wrap(int v, int n) { v %= n; return v < 0 ? v + n : v; }

static void c2d_sample(const C2dPaint *p, double u, double v, float *o)
{
    const C2dImg *im = &p->img;
    if (!p->smooth) {
        int x = (int)floor(u), y = (int)floor(v);
        if (p->clamp) {
            int cx0 = (int)floor(p->cl[0]), cy0 = (int)floor(p->cl[1]), cx1 = (int)ceil(p->cl[0] + p->cl[2]) - 1, cy1 = (int)ceil(p->cl[1] + p->cl[3]) - 1;
            if (x < cx0) x = cx0;
            if (y < cy0) y = cy0;
            if (x > cx1) x = cx1;
            if (y > cy1) y = cy1;
        }
        if (p->rx) x = c2d_wrap(x, im->w);
        if (p->ry) y = c2d_wrap(y, im->h);
        if (x < 0 || y < 0 || x >= im->w || y >= im->h) { o[0] = o[1] = o[2] = o[3] = 0; return; }
        c2d_texel(im, x, y, o);
        return;
    }
    double fx = u - 0.5, fy = v - 0.5;
    if (p->clamp) {
        double lo = p->cl[0], hi = p->cl[0] + p->cl[2] - 1.0;
        if (hi < lo) hi = lo;
        if (fx < lo) fx = lo;
        if (fx > hi) fx = hi;
        lo = p->cl[1]; hi = p->cl[1] + p->cl[3] - 1.0;
        if (hi < lo) hi = lo;
        if (fy < lo) fy = lo;
        if (fy > hi) fy = hi;
    }
    int x0 = (int)floor(fx), y0 = (int)floor(fy);
    float ax = (float)(fx - x0), ay = (float)(fy - y0);
    float c[4][4];
    for (int k = 0; k < 4; k++) {
        int x = x0 + (k & 1), y = y0 + (k >> 1);
        if (p->clamp) {
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (x >= im->w) x = im->w - 1;
            if (y >= im->h) y = im->h - 1;
        }
        if (p->rx) x = c2d_wrap(x, im->w);
        if (p->ry) y = c2d_wrap(y, im->h);
        if (x < 0 || y < 0 || x >= im->w || y >= im->h) { c[k][0] = c[k][1] = c[k][2] = c[k][3] = 0; continue; }   /* (past an unrepeated edge) */
        c2d_texel(im, x, y, c[k]);
    }
    for (int q = 0; q < 4; q++) {
        float t = c[0][q] + (c[1][q] - c[0][q]) * ax, b = c[2][q] + (c[3][q] - c[2][q]) * ax;
        o[q] = t + (b - t) * ay;
    }
}

/* The paint's premultiplied color at device pixel (x, y)'s centre. */
static void c2d_paint_at(const C2dPaint *p, int x, int y, float *o)
{
    if (p->kind == 0) { memcpy(o, p->col, sizeof p->col); return; }
    if (p->none) { o[0] = o[1] = o[2] = o[3] = 0; return; }
    double dx = x + 0.5 - p->ox, dy = y + 0.5 - p->oy;
    double u = p->inv[0] * dx + p->inv[2] * dy + p->inv[4], v = p->inv[1] * dx + p->inv[3] * dy + p->inv[5];
    double t = 0;
    if (p->kind == 1) {
        double gx = p->g[2] - p->g[0], gy = p->g[3] - p->g[1], l2 = gx * gx + gy * gy;
        t = ((u - p->g[0]) * gx + (v - p->g[1]) * gy) / l2;
    } else if (p->kind == 2) {
        double x0 = p->g[0], y0 = p->g[1], r0 = p->g[2], x1 = p->g[3], y1 = p->g[4], r1 = p->g[5];
        double cdx = x1 - x0, cdy = y1 - y0, dr = r1 - r0;
        double pdx = u - x0, pdy = v - y0;
        double A = cdx * cdx + cdy * cdy - dr * dr, B = pdx * cdx + pdy * cdy + r0 * dr, C = pdx * pdx + pdy * pdy - r0 * r0;
        double w = NAN;
        if (fabs(A) < 1e-9) {
            if (fabs(B) > 1e-12) { w = C / (2 * B); if (r0 + w * dr < 0) w = NAN; }
        } else {
            double disc = B * B - A * C;
            if (disc >= 0) {
                double sq = sqrt(disc), w1 = (B + sq) / A, w2 = (B - sq) / A;
                if (w1 < w2) { double tt = w1; w1 = w2; w2 = tt; }
                if (r0 + w1 * dr >= 0) w = w1;
                else if (r0 + w2 * dr >= 0) w = w2;
            }
        }
        if (isnan(w)) { o[0] = o[1] = o[2] = o[3] = 0; return; }
        t = w;
    } else if (p->kind == 3) {
        double ang = atan2(v - p->g[2], u - p->g[1]) - p->g[0];
        t = ang / (2 * M_PI);
        t -= floor(t);
    } else if (p->kind == 4) {
        c2d_sample(p, u, v, o);
        return;
    }
    if (!(t > 0)) t = 0;
    if (t > 1) t = 1;
    int k = (int)(t * 1023.0 + 0.5);
    memcpy(o, p->lut[k], sizeof p->lut[k]);
}

/* ---- compositing ---- */
enum { OP_SRC_OVER, OP_SRC_IN, OP_SRC_OUT, OP_SRC_ATOP, OP_DST_OVER, OP_DST_IN, OP_DST_OUT, OP_DST_ATOP,
       OP_LIGHTER, OP_COPY, OP_XOR, OP_MULTIPLY, OP_SCREEN, OP_OVERLAY, OP_DARKEN, OP_LIGHTEN, OP_DODGE,
       OP_BURN, OP_HARD, OP_SOFT, OP_DIFF, OP_EXCL, OP_HUE, OP_SAT, OP_COLOR, OP_LUM };

static bool c2d_unbounded(int op)
{
    return op == OP_SRC_IN || op == OP_SRC_OUT || op == OP_DST_IN || op == OP_DST_ATOP || op == OP_COPY;
}

static float c2d_blend1(int op, float b, float s)
{
    switch (op) {
    case OP_MULTIPLY: return b * s;
    case OP_SCREEN: return b + s - b * s;
    case OP_OVERLAY: return b <= 0.5f ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    case OP_DARKEN: return b < s ? b : s;
    case OP_LIGHTEN: return b > s ? b : s;
    case OP_DODGE: return b == 0 ? 0 : s >= 1 ? 1 : fminf(1, b / (1 - s));
    case OP_BURN: return b >= 1 ? 1 : s <= 0 ? 0 : 1 - fminf(1, (1 - b) / s);
    case OP_HARD: return s <= 0.5f ? b * 2 * s : b + (2 * s - 1) - b * (2 * s - 1);
    case OP_SOFT: {
        if (s <= 0.5f) return b - (1 - 2 * s) * b * (1 - b);
        float d = b <= 0.25f ? ((16 * b - 12) * b + 4) * b : sqrtf(b);
        return b + (2 * s - 1) * (d - b);
    }
    case OP_DIFF: return fabsf(b - s);
    case OP_EXCL: return b + s - 2 * b * s;
    }
    return s;
}

static float c2d_lumf(const float *c) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }
static void c2d_clipcolor(float *c)
{
    float l = c2d_lumf(c), n = fminf(c[0], fminf(c[1], c[2])), x = fmaxf(c[0], fmaxf(c[1], c[2]));
    for (int i = 0; i < 3; i++) {
        if (n < 0) c[i] = l + (c[i] - l) * l / (l - n);
        if (x > 1) c[i] = l + (c[i] - l) * (1 - l) / (x - l);
    }
}
static void c2d_setlum(float *c, float l) { float d = l - c2d_lumf(c); for (int i = 0; i < 3; i++) c[i] += d; c2d_clipcolor(c); }
static float c2d_satf(const float *c) { return fmaxf(c[0], fmaxf(c[1], c[2])) - fminf(c[0], fminf(c[1], c[2])); }
static void c2d_setsat(float *c, float s)
{
    int mx = 0, mn = 0;
    for (int i = 1; i < 3; i++) { if (c[i] > c[mx]) mx = i; if (c[i] < c[mn]) mn = i; }
    if (mx == mn) { c[0] = c[1] = c[2] = 0; return; }
    int md = 3 - mx - mn;
    if (c[mx] > c[mn]) { c[md] = (c[md] - c[mn]) * s / (c[mx] - c[mn]); c[mx] = s; }
    else c[md] = c[mx] = 0;
    c[mn] = 0;
}

/* d (premultiplied, 0..1) = s op d */
static void c2d_comp(int op, const float *s, float *d)
{
    float as = s[3], ab = d[3], fa, fb;
    switch (op) {
    case OP_SRC_OVER: fa = 1; fb = 1 - as; break;
    case OP_SRC_IN: fa = ab; fb = 0; break;
    case OP_SRC_OUT: fa = 1 - ab; fb = 0; break;
    case OP_SRC_ATOP: fa = ab; fb = 1 - as; break;
    case OP_DST_OVER: fa = 1 - ab; fb = 1; break;
    case OP_DST_IN: fa = 0; fb = as; break;
    case OP_DST_OUT: fa = 0; fb = 1 - as; break;
    case OP_DST_ATOP: fa = 1 - ab; fb = as; break;
    case OP_LIGHTER: fa = 1; fb = 1; break;
    case OP_COPY: fa = 1; fb = 0; break;
    case OP_XOR: fa = 1 - ab; fb = 1 - as; break;
    default: {
        /* (a blend mode, with source-over) */
        float cs[3], cb[3], r[3];
        for (int i = 0; i < 3; i++) { cs[i] = as > 0 ? s[i] / as : 0; cb[i] = ab > 0 ? d[i] / ab : 0; }
        if (op >= OP_HUE) {
            float t[3];
            if (op == OP_HUE) { memcpy(t, cs, sizeof t); c2d_setsat(t, c2d_satf(cb)); c2d_setlum(t, c2d_lumf(cb)); }
            else if (op == OP_SAT) { memcpy(t, cb, sizeof t); c2d_setsat(t, c2d_satf(cs)); c2d_setlum(t, c2d_lumf(cb)); }
            else if (op == OP_COLOR) { memcpy(t, cs, sizeof t); c2d_setlum(t, c2d_lumf(cb)); }
            else { memcpy(t, cb, sizeof t); c2d_setlum(t, c2d_lumf(cs)); }
            memcpy(r, t, sizeof r);
        } else for (int i = 0; i < 3; i++) r[i] = c2d_blend1(op, cb[i], cs[i]);
        for (int i = 0; i < 3; i++) d[i] = s[i] * (1 - ab) + d[i] * (1 - as) + as * ab * r[i];
        d[3] = as + ab * (1 - as);
        return;
    }
    }
    for (int i = 0; i < 4; i++) {
        float v = s[i] * fa + d[i] * fb;
        d[i] = v > 1 ? 1 : v < 0 ? 0 : v;
    }
}

static inline uint8_t c2d_u8(float v) { v = v * 255.f + 0.5f; return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
/* (a stored pixel as premultiplied floats, and back) */
static inline void c2d_load(const uint8_t *p, float *d)
{
    float a = p[3] / 255.f;
    d[0] = p[0] / 255.f * a; d[1] = p[1] / 255.f * a; d[2] = p[2] / 255.f * a; d[3] = a;
}
static inline void c2d_store(uint8_t *p, const float *o)
{
    float a = o[3] < 0 ? 0 : o[3] > 1 ? 1 : o[3];
    uint8_t A = c2d_u8(a);
    if (!A) { p[0] = p[1] = p[2] = p[3] = 0; return; }
    for (int i = 0; i < 3; i++) p[i] = c2d_u8(o[i] / a);
    p[3] = A;
}

/* Composite the source image src (premultiplied floats over box b, alpha already
 * multiplied in) onto s's bitmap with op inside the clip.  Unbounded operators
 * affect the whole bitmap (src transparent past b). */
static void c2d_composite(C2dSurf *s, const float *src, const C2dBox *b, int op)
{
    C2dBox r = *b;
    if (c2d_unbounded(op)) { r.x0 = 0; r.y0 = 0; r.x1 = s->w; r.y1 = s->h; }
    int bw = b->x1 - b->x0;
    for (int y = r.y0; y < r.y1; y++) {
        for (int x = r.x0; x < r.x1; x++) {
            float sv[4] = { 0, 0, 0, 0 };
            if (x >= b->x0 && x < b->x1 && y >= b->y0 && y < b->y1) memcpy(sv, src + ((size_t)(y - b->y0) * bw + (x - b->x0)) * 4, sizeof sv);
            else if (!c2d_unbounded(op)) continue;
            uint8_t *p = s->px + ((size_t)y * s->w + x) * 4;
            float cl = s->clip ? s->clip[(size_t)y * s->w + x] / 255.f : 1.f;
            if (cl <= 0) continue;
            float d[4], o[4], r[4];
            c2d_load(p, d);
            memcpy(o, d, sizeof o);
            c2d_comp(op, sv, o);
            for (int i = 0; i < 4; i++) r[i] = d[i] + (o[i] - d[i]) * cl;
            if (s->opaque && !s->nlayers) { for (int i = 0; i < 3; i++) p[i] = c2d_u8(r[i]); p[3] = 255; }
            else c2d_store(p, r);
        }
    }
}

/* (a true gaussian, kernel radius ceil(3 sigma), as svg_filter.c's feGaussianBlur: refs drawn with SVG filters match) */
static void c2d_blur(float *img, int w, int h, int ch, double sx, double sy)
{
    size_t n = (size_t)w * h * ch;
    float *tmp = (float *)malloc(n * sizeof(float));
    if (!tmp) return;
    for (int axis = 0; axis < 2; axis++) {
        double sg = axis == 0 ? sx : sy;
        if (!(sg > 0)) continue;
        int r = (int)ceil(sg * 3.0);
        if (r > 512) r = 512;
        float *kern = (float *)malloc((size_t)(2 * r + 1) * sizeof(float));
        if (!kern) break;
        double sum = 0.0;
        for (int i = -r; i <= r; i++) { double v = exp(-0.5 * i * i / (sg * sg)); kern[i + r] = (float)v; sum += v; }
        for (int i = 0; i <= 2 * r; i++) kern[i] = (float)(kern[i] / sum);
        bool horiz = axis == 0;
        int outer = horiz ? h : w, inner = horiz ? w : h;
        size_t so = horiz ? (size_t)w * ch : (size_t)ch, si = horiz ? (size_t)ch : (size_t)w * ch;
        for (int o = 0; o < outer; o++) {
            const float *line = img + (size_t)o * so;
            float *out = tmp + (size_t)o * so;
            for (int i = 0; i < inner; i++) {
                float c[4] = { 0, 0, 0, 0 };
                int k0 = i - r < 0 ? 0 : i - r, k1 = i + r >= inner ? inner - 1 : i + r;
                for (int k = k0; k <= k1; k++) {
                    float wt = kern[k - i + r];
                    const float *p = line + (size_t)k * si;
                    for (int q = 0; q < ch; q++) c[q] += p[q] * wt;
                }
                for (int q = 0; q < ch; q++) out[(size_t)i * si + q] = c[q];
            }
        }
        memcpy(img, tmp, n * sizeof(float));
        free(kern);
    }
    free(tmp);
}

/* ---- filters (ctx.filter, a layer's filter): a list of functions ----
 * encoded as (kind, n, values...): 1 blur(sd), 2 brightness, 3 contrast, 4 grayscale, 5 hue-rotate(deg),
 * 6 invert, 7 opacity, 8 saturate, 9 sepia, 10 drop-shadow(dx, dy, sd, r, g, b, a), 11 color matrix (20 values),
 * 12 blur(sx, sy) */
static void c2d_matrix_apply(float *img, size_t n, const float *m)
{
    for (size_t i = 0; i < n; i++) {
        float *p = img + i * 4, a = p[3], c[4];
        if (a <= 0) { c[0] = c[1] = c[2] = 0; } else { c[0] = p[0] / a; c[1] = p[1] / a; c[2] = p[2] / a; }
        c[3] = a;
        float o[4];
        for (int r = 0; r < 4; r++) {
            float v = m[r * 5 + 0] * c[0] + m[r * 5 + 1] * c[1] + m[r * 5 + 2] * c[2] + m[r * 5 + 3] * c[3] + m[r * 5 + 4];
            o[r] = v < 0 ? 0 : v > 1 ? 1 : v;
        }
        p[0] = o[0] * o[3]; p[1] = o[1] * o[3]; p[2] = o[2] * o[3]; p[3] = o[3];
    }
}

static void c2d_func_matrix(int kind, double v, float *m)
{
    memset(m, 0, sizeof(float) * 20);
    m[0] = m[6] = m[12] = m[18] = 1;
    float s = (float)v;
    switch (kind) {
    case 2: m[0] = m[6] = m[12] = s; break;
    case 3: m[0] = m[6] = m[12] = s; m[4] = m[9] = m[14] = 0.5f - 0.5f * s; break;
    case 4: { if (s > 1) s = 1; float a = 1 - s;
        m[0] = 0.2126f + 0.7874f * a; m[1] = 0.7152f - 0.7152f * a; m[2] = 0.0722f - 0.0722f * a;
        m[5] = 0.2126f - 0.2126f * a; m[6] = 0.7152f + 0.2848f * a; m[7] = 0.0722f - 0.0722f * a;
        m[10] = 0.2126f - 0.2126f * a; m[11] = 0.7152f - 0.7152f * a; m[12] = 0.0722f + 0.9278f * a; break; }
    case 5: { double r = v * M_PI / 180; float c = (float)cos(r), n = (float)sin(r);
        m[0] = 0.213f + c * 0.787f - n * 0.213f; m[1] = 0.715f - c * 0.715f - n * 0.715f; m[2] = 0.072f - c * 0.072f + n * 0.928f;
        m[5] = 0.213f - c * 0.213f + n * 0.143f; m[6] = 0.715f + c * 0.285f + n * 0.140f; m[7] = 0.072f - c * 0.072f - n * 0.283f;
        m[10] = 0.213f - c * 0.213f - n * 0.787f; m[11] = 0.715f - c * 0.715f + n * 0.715f; m[12] = 0.072f + c * 0.928f + n * 0.072f; break; }
    case 6: { if (s > 1) s = 1; m[0] = m[6] = m[12] = 1 - 2 * s; m[4] = m[9] = m[14] = s; break; }
    case 7: { if (s > 1) s = 1; m[18] = s; break; }
    case 8: m[0] = 0.213f + 0.787f * s; m[1] = 0.715f - 0.715f * s; m[2] = 0.072f - 0.072f * s;
        m[5] = 0.213f - 0.213f * s; m[6] = 0.715f + 0.285f * s; m[7] = 0.072f - 0.072f * s;
        m[10] = 0.213f - 0.213f * s; m[11] = 0.715f - 0.715f * s; m[12] = 0.072f + 0.928f * s; break;
    case 9: { if (s > 1) s = 1; float a = 1 - s;
        m[0] = 0.393f + 0.607f * a; m[1] = 0.769f - 0.769f * a; m[2] = 0.189f - 0.189f * a;
        m[5] = 0.349f - 0.349f * a; m[6] = 0.686f + 0.314f * a; m[7] = 0.168f - 0.168f * a;
        m[10] = 0.272f - 0.272f * a; m[11] = 0.534f - 0.534f * a; m[12] = 0.131f + 0.869f * a; break; }
    }
}

/* How far a filter list reaches past what it filters (blur, drop-shadow). */
static int c2d_filter_reach(const double *f, int n)
{
    int i = 0, reach = 0;
    while (i + 2 <= n) {
        int k = (int)f[i], c = (int)f[i + 1];
        if (i + 2 + c > n) break;
        const double *v = f + i + 2;
        if (k == 1 && c >= 1) reach += (int)ceil(v[0] * 3) + 1;
        if (k == 12 && c >= 2) reach += (int)ceil(fmax(v[0], v[1]) * 3) + 1;
        if (k == 10 && c >= 3) reach += (int)ceil(fabs(v[0]) + fabs(v[1]) + fmax(v[2], c >= 8 ? v[3] : 0) * 3) + 1;
        i += 2 + c;
    }
    return reach;
}

/* (filtered pixels rounded as an SVG image's filter output is on its way to a canvas: straight bytes (svg_filter.c),
 * premultiplied bytes (plutosvg), straight again (svg_render.c): refs drawn that way match to the byte) */
/* (no blur in the chain: refs made with premultiplied stores that truncate) */
static int C2D_QTRUNC, C2D_LAYER_F;
static int c2d_filter_sharp(const double *f, int n)
{
    for (int j = 0; j + 2 <= n; j += 2 + (int)f[j + 1]) {
        int k = (int)f[j], c = (int)f[j + 1];
        if (j + 2 + c > n) break;
        if ((k == 1 && c >= 1 && f[j + 2] > 0) || (k == 12 && c >= 2 && (f[j + 2] > 0 || f[j + 3] > 0)) || (k == 10 && c >= 3 && (f[j + 4] > 0 || (c >= 8 && f[j + 5] > 0)))) return 0;
    }
    return 1;
}
static void c2d_svg_quant(float *img, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float *p = img + i * 4, a = p[3] < 0 ? 0 : p[3] > 1 ? 1 : p[3];
        long A = lroundf(a * 255.0f);
        if (A <= 0) { p[0] = p[1] = p[2] = p[3] = 0; continue; }
        for (int k = 0; k < 3; k++) {
            float c = a > 0 ? p[k] / a : 0;
            c = c < 0 ? 0 : c > 1 ? 1 : c;
            long C = lroundf(c * 255.0f);
            long P = C2D_QTRUNC ? (C * A) / 255 : (C * A + 127) / 255;
            long C2 = A == 255 ? P : (P * 255 + A / 2) / A;
            if (C2 > 255) C2 = 255;
            p[k] = (float)C2 / 255.f * ((float)A / 255.f);
        }
        p[3] = (float)A / 255.f;
    }
}

static void c2d_filter_apply(float *img, int w, int h, const double *f, int n)
{
    int i = 0;
    size_t np = (size_t)w * h;
    C2D_QTRUNC = c2d_filter_sharp(f, n);
    while (i + 2 <= n) {
        int k = (int)f[i], c = (int)f[i + 1];
        if (i + 2 + c > n) break;
        const double *v = f + i + 2;
        float m[20];
        if (k == 1 && c >= 1) c2d_blur(img, w, h, 4, v[0], v[0]);
        else if (k == 12 && c >= 2) c2d_blur(img, w, h, 4, v[0], v[1]);
        else if (k >= 2 && k <= 9 && c >= 1) { c2d_func_matrix(k, v[0], m); c2d_matrix_apply(img, np, m); }
        else if (k == 13) {
            /* (component transfer: per channel (type, m, params...): 0 identity, 1 table, 2 discrete, 3 linear (slope, intercept), 4 gamma (amplitude, exponent, offset)) */
            const double *fn[4] = { 0 }; int ft[4] = { 0 }, fm[4] = { 0 };
            int at = 0;
            for (int ch = 0; ch < 4 && at + 2 <= c; ch++) { ft[ch] = (int)v[at]; fm[ch] = (int)v[at + 1]; fn[ch] = v + at + 2; at += 2 + fm[ch]; if (at > c) { ft[ch] = 0; break; } }
            for (size_t q = 0; q < np; q++) {
                float *p = img + q * 4, a = p[3], cc[4];
                for (int e = 0; e < 3; e++) cc[e] = a > 0 ? p[e] / a : 0;
                cc[3] = a;
                for (int ch = 0; ch < 4; ch++) {
                    double C = cc[ch], o = C, *dummy = 0; (void)dummy;
                    const double *V = fn[ch]; int n = fm[ch];
                    if (ft[ch] == 1 && n >= 1) { if (n == 1) o = V[0]; else { int kk = C >= 1 ? n - 2 : (int)floor(C * (n - 1)); if (kk < 0) kk = 0; o = V[kk] + (C - (double)kk / (n - 1)) * (n - 1) * (V[kk + 1] - V[kk]); } }
                    else if (ft[ch] == 2 && n >= 1) { int kk = C >= 1 ? n - 1 : (int)floor(C * n); if (kk < 0) kk = 0; o = V[kk]; }
                    else if (ft[ch] == 3 && n >= 2) o = V[0] * C + V[1];
                    else if (ft[ch] == 4 && n >= 3) o = V[0] * pow(C, V[1]) + V[2];
                    cc[ch] = (float)(o < 0 ? 0 : o > 1 ? 1 : o);
                }
                p[3] = cc[3];
                for (int e = 0; e < 3; e++) p[e] = cc[e] * cc[3];
            }
        }
        else if (k == 11 && c >= 20) { for (int q = 0; q < 20; q++) m[q] = (float)v[q]; c2d_matrix_apply(img, np, m); }
        else if (k == 10 && c >= 7) {
            float *sh = (float *)calloc(np * 4, sizeof(float));
            if (sh) {
                int ox = (int)lround(v[0]), oy = (int)lround(v[1]);
                /* (8 values: separate x and y deviations) */
                const double *cv_ = c >= 8 ? v + 4 : v + 3;
                double bsx = v[2], bsy = c >= 8 ? v[3] : v[2];
                float col[4] = { (float)(cv_[0] / 255 * cv_[3]), (float)(cv_[1] / 255 * cv_[3]), (float)(cv_[2] / 255 * cv_[3]), (float)cv_[3] };
                for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                    int sx = x - ox, sy = y - oy;
                    if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                    float a = img[((size_t)sy * w + sx) * 4 + 3];
                    for (int q = 0; q < 4; q++) sh[((size_t)y * w + x) * 4 + q] = col[q] * a;
                }
                c2d_blur(sh, w, h, 4, bsx, bsy);
                for (size_t q = 0; q < np; q++) {
                    float *p = img + q * 4, *s = sh + q * 4, a = p[3];
                    for (int e = 0; e < 4; e++) p[e] = p[e] + s[e] * (1 - a);
                }
                free(sh);
            }
        }
        i += 2 + c;
    }
    if (n > 0) c2d_svg_quant(img, np);
}

/* ---- one drawing operation ---- */
/* The source image: paint over coverage (the scratch's alpha in b), then the
 * filter, the shadow and the composite. */
/* (which parts c2d_finish composites: 0 both, 1 the shadow alone, 2 the shape alone) */
static int g_finish_part;
static void c2d_finish(C2dSurf *s, const C2dBox *b0, const C2dPaint *pt, const C2dState *st, const float *pre)
{
    C2dBox b = *b0;
    /* (a filter or shadow reaches past the shape: the source made over a larger box) */
    int reach = st->nfilter ? c2d_filter_reach(st->filter, st->nfilter) : 0;
    bool shadow = st->sh[3] > 0 && (st->sh[4] != 0 || st->sh[5] != 0 || st->sh[6] > 0 || g_finish_part == 1);
    if (g_finish_part == 2) shadow = false;
    C2dBox sb = b;
    if (reach) { sb.x0 -= reach; sb.y0 -= reach; sb.x1 += reach; sb.y1 += reach; }
    if (sb.x0 < 0) sb.x0 = 0;
    if (sb.y0 < 0) sb.y0 = 0;
    if (sb.x1 > s->w) sb.x1 = s->w;
    if (sb.y1 > s->h) sb.y1 = s->h;
    int w = sb.x1 - sb.x0, h = sb.y1 - sb.y0;
    if (w <= 0 || h <= 0) return;
    float *src = (float *)calloc((size_t)w * h * 4, sizeof(float));
    if (!src) return;
    for (int y = b.y0; y < b.y1; y++)
        for (int x = b.x0; x < b.x1; x++) {
            float *o = src + ((size_t)(y - sb.y0) * w + (x - sb.x0)) * 4;
            if (pre) { memcpy(o, pre + ((size_t)(y - b.y0) * (b.x1 - b.x0) + (x - b.x0)) * 4, sizeof(float) * 4); continue; }
            float cv = c2d_cov_at(x, y) / 255.f;
            if (cv <= 0) continue;
            float c[4];
            c2d_paint_at(pt, x, y, c);
            for (int q = 0; q < 4; q++) o[q] = c[q] * cv;
        }
    float ga = (float)(st->alpha < 0 ? 0 : st->alpha > 1 ? 1 : st->alpha);
    /* (global alpha on the source before its filter and shadow, as Chrome) */
    if (ga < 1) { for (size_t i = 0; i < (size_t)w * h * 4; i++) src[i] *= ga; ga = 1; }
    if (st->nfilter) c2d_filter_apply(src, w, h, st->filter, st->nfilter);
    if (shadow) {
        double sg = st->sh[6] / 2;
        int ox = (int)lround(st->sh[4]), oy = (int)lround(st->sh[5]), pad = (int)ceil(sg * 3) + 2;
        C2dBox hb = { sb.x0 + ox - pad, sb.y0 + oy - pad, sb.x1 + ox + pad, sb.y1 + oy + pad };
        int hw = hb.x1 - hb.x0, hh = hb.y1 - hb.y0;
        float *sh = (float *)calloc((size_t)hw * hh * 4, sizeof(float));
        if (sh) {
            float col[4] = { (float)(st->sh[0] / 255 * st->sh[3]), (float)(st->sh[1] / 255 * st->sh[3]), (float)(st->sh[2] / 255 * st->sh[3]), (float)st->sh[3] };
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                float a = src[((size_t)y * w + x) * 4 + 3];
                if (a <= 0) continue;
                float *o = sh + ((size_t)(y + pad) * hw + (x + pad)) * 4;
                for (int q = 0; q < 4; q++) o[q] = col[q] * a;
            }
            if (sg > 0) c2d_blur(sh, hw, hh, 4, sg, sg);
            /* (cut to the bitmap) */
            C2dBox cb = hb;
            if (cb.x0 < 0) cb.x0 = 0;
            if (cb.y0 < 0) cb.y0 = 0;
            if (cb.x1 > s->w) cb.x1 = s->w;
            if (cb.y1 > s->h) cb.y1 = s->h;
            if (cb.x1 > cb.x0 && cb.y1 > cb.y0) {
                int cw = cb.x1 - cb.x0, chh = cb.y1 - cb.y0;
                float *cs = (float *)malloc((size_t)cw * chh * 4 * sizeof(float));
                if (cs) {
                    for (int y = 0; y < chh; y++) for (int x = 0; x < cw; x++)
                        for (int q = 0; q < 4; q++) cs[((size_t)y * cw + x) * 4 + q] = sh[((size_t)(y + cb.y0 - hb.y0) * hw + (x + cb.x0 - hb.x0)) * 4 + q] * ga;
                    c2d_composite(s, cs, &cb, st->op);
                    free(cs);
                }
            }
            free(sh);
        }
    }
    if (g_finish_part == 1) { free(src); return; }
    if (ga < 1) for (size_t i = 0; i < (size_t)w * h * 4; i++) src[i] *= ga;
    c2d_composite(s, src, &sb, st->op);
    free(src);
}

/* ---- JS: argument helpers ---- */
static double *c2d_f64(JSContextHandle ctx, GCValue v, int *n)
{
    *n = 0;
    if (!JS_IsObject(v)) return NULL;
    size_t off = 0, len = 0, bpe = 0;
    GCValue buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf) || bpe != 8) return NULL;
    size_t sz = 0;
    uint8_t *d = JS_GetArrayBuffer(ctx, &sz, buf);
    if (!d || off + len > sz) return NULL;
    *n = (int)(len / 8);
    return (double *)(d + off);
}

static uint64_t c2d_id(JSContextHandle ctx, GCValue v)
{
    if (JS_IsNumber(v)) { double d = 0; JS_ToFloat64(ctx, &d, v); return d > 0 ? (uint64_t)d : 0; }
    if (JS_IsObject(v)) return (uint64_t)dom_node_of(v);
    return 0;
}

/* (the surface for id at w x h: made, or remade at another size) */
static C2dSurf *c2d_get(JSContextHandle ctx, GCValue idv, GCValue wv, GCValue hv)
{
    uint64_t id = c2d_id(ctx, idv);
    if (!id) return NULL;
    int32_t w = 0, h = 0;
    JS_ToInt32(ctx, &w, wv);
    JS_ToInt32(ctx, &h, hv);
    C2dSurf *s = c2d_find(id);
    int cw_ = s && s->nlayers ? s->layers[0].w : s ? s->w : 0, ch_ = s && s->nlayers ? s->layers[0].h : s ? s->h : 0;
    if (!s || cw_ != w || ch_ != h) s = c2d_make(id, w, h, s ? s->opaque : false);
    if (s && id < C2D_OFF_BASE) s->shown = true;
    return s && s->px ? s : NULL;
}

static void c2d_img_of(JSContextHandle ctx, GCValue src, C2dImg *o)
{
    memset(o, 0, sizeof *o);
    if (JS_IsNumber(src) || JS_IsObject(src)) {
        C2dSurf *s = c2d_find(c2d_id(ctx, src));
        if (s && s->px) { o->px = s->px; o->w = s->w; o->h = s->h; o->ch = 4; o->premul = false; return; }
        /* (a video element: the frame it shows now) */
        if (JS_IsObject(src)) {
            extern bool media_video_frame_for_node(uint32_t node, const uint8_t **rgba, int *w, int *h, uint64_t *frame_id);
            uint32_t vn = (uint32_t)dom_node_of(src);
            const uint8_t *vp = NULL;
            int vw = 0, vh = 0;
            uint64_t fid = 0;
            if (vn && media_video_frame_for_node(vn, &vp, &vw, &vh, &fid) && vp && vw > 0 && vh > 0) {
                o->px = (uint8_t *)vp; o->w = vw; o->h = vh; o->ch = 4; o->premul = false;
            }
        }
        return;
    }
    if (!JS_IsString(src)) return;
    const char *url = JS_ToCString(ctx, src);
    ImageCache *ic = display_list_get_image_cache();
    if (!url || !ic) return;
    /* (at its natural size when known: asking for no size re-decodes a vector image) */
    int nw0 = 0, nh0 = 0;
    image_cache_natural_size(ic, url, &nw0, &nh0);
    int hd = image_cache_request(ic, url, nw0, nh0);
    if (hd < 0) return;
    int w = 0, h = 0, ch = 0;
    uint8_t *px = NULL;
    if (!image_cache_get(ic, hd, &w, &h, &ch, &px)) {
        for (int k = 0; k < 100 && !image_cache_get(ic, hd, &w, &h, &ch, &px); k++) {
            image_cache_wait_pending_ms(ic, 50);
            image_cache_process_pending(ic);
            hd = image_cache_request(ic, url, nw0, nh0);
            if (hd < 0) return;
        }
        if (!px) return;
    }
    o->px = px; o->w = w; o->h = h; o->ch = ch; o->premul = false;
}

/* __c2dReset(id, w, h, opaque): a new, cleared bitmap; its clips and layers gone */
static GCValue js_c2d_reset(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 3) return JS_UNDEFINED;
    uint64_t id = c2d_id(ctx, argv[0]);
    int32_t w = 0, h = 0;
    JS_ToInt32(ctx, &w, argv[1]);
    JS_ToInt32(ctx, &h, argv[2]);
    bool opq = argc > 3 && JS_ToBool(ctx, argv[3]);
    if (!id) return JS_UNDEFINED;
    C2dSurf *s = c2d_make(id, w, h, opq);
    if (s) { s->shown = true; c2d_changed(s); }
    return JS_UNDEFINED;
}

/* __c2dNew(): an id for an offscreen bitmap */
/* __c2dNodeId(el): an element's id as canvas2d.c keys its bitmap (for another realm of this thread) */
static GCValue js_c2d_node_id(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    return JS_NewFloat64(ctx, argc > 0 ? (double)c2d_id(ctx, argv[0]) : 0.0);
}

static GCValue js_c2d_new(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, (double)g_next_off++);
}

static GCValue js_c2d_free(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    C2dSurf *s = c2d_find(c2d_id(ctx, argv[0]));
    if (s) {
        c2d_free_state(s);
        free(s->px); s->px = NULL; s->w = s->h = 0;
    }
    return JS_UNDEFINED;
}

static void c2d_clip_with(C2dSurf *s, const C2dBox *b, bool have)
{
    size_t n = (size_t)s->w * s->h;
    if (!s->clip) {
        s->clip = (uint8_t *)malloc(n);
        if (!s->clip) return;
        memset(s->clip, 255, n);
    }
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            uint8_t *c = &s->clip[(size_t)y * s->w + x];
            unsigned cv = have && x >= b->x0 && x < b->x1 && y >= b->y0 && y < b->y1 ? c2d_cov_at(x, y) : 0;
            *c = (uint8_t)((*c * cv + 127) / 255);
        }
}

/* __c2dDraw(id, w, h, mode, path, paint, state, src): mode 0 fill, 1 stroke, 2 clip, 3 clear */
static GCValue js_c2d_draw(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 7) return JS_UNDEFINED;
    C2dSurf *s = c2d_get(ctx, argv[0], argv[1], argv[2]);
    if (!s) return JS_UNDEFINED;
    int32_t mode = 0;
    JS_ToInt32(ctx, &mode, argv[3]);
    int pn = 0, qn = 0, sn = 0;
    double *pa = c2d_f64(ctx, argv[4], &pn), *qa = c2d_f64(ctx, argv[5], &qn), *sa = c2d_f64(ctx, argv[6], &sn);
    C2dState st;
    if (!pa || !c2d_state(sa, sn, &st)) return JS_UNDEFINED;
    /* (the paint first: fetching an image can run script (load events), which may draw on canvases and reuse the coverage scratch) */
    C2dPaint *pt = mode <= 1 ? (C2dPaint *)malloc(sizeof *pt) : NULL;
    bool pt_ok = pt && c2d_paint(ctx, qa, qn, argc > 7 ? argv[7] : JS_UNDEFINED, st.smooth, pt);
    s = c2d_get(ctx, argv[0], argv[1], argv[2]);
    if (!s) { free(pt); return JS_UNDEFINED; }
    plutovg_path_t *p = plutovg_path_create();
    if (!p) { free(pt); return JS_UNDEFINED; }
    c2d_path_build(p, pa, pn);
    if (s->ox || s->oy) { st.m[4] += s->ox; st.m[5] += s->oy; plutovg_matrix_t tm; plutovg_matrix_init_translate(&tm, (float)s->ox, (float)s->oy); plutovg_path_transform(p, &tm); }
    plutovg_matrix_t m;
    plutovg_matrix_init_identity(&m);
    if (mode == 1) {
        /* (a stroke: its path taken back to user space, stroked with the line width there) */
        double inv[6];
        if (!c2d_invert(st.m, inv)) { plutovg_path_destroy(p); free(pt); return JS_UNDEFINED; }
        plutovg_matrix_t im;
        plutovg_matrix_init(&im, (float)inv[0], (float)inv[1], (float)inv[2], (float)inv[3], (float)inv[4], (float)inv[5]);
        plutovg_path_transform(p, &im);
        plutovg_matrix_init(&m, (float)st.m[0], (float)st.m[1], (float)st.m[2], (float)st.m[3], (float)st.m[4], (float)st.m[5]);
    }
    C2dBox b;
    bool have = c2d_cover(p, &m, mode == 1, &st, s->w, s->h, &b);
    /* (a shadow of a shape reaching off the bitmap: the shadow made from the shape moved by its offset, and
     * painted with the paint moved too, before the shape itself) */
    bool split = false;
    if (mode <= 1 && pt_ok && !st.nfilter && st.sh[3] > 0 && (st.sh[4] != 0 || st.sh[5] != 0) &&
        (g_cover_ext[0] < 0 || g_cover_ext[1] < 0 || g_cover_ext[2] > s->w || g_cover_ext[3] > s->h)) {
        if (have) c2d_cover_clear(&b);
        int sox = (int)lround(st.sh[4]), soy = (int)lround(st.sh[5]);
        plutovg_matrix_t m2 = m;
        m2.e += (float)sox; m2.f += (float)soy;
        C2dBox b2;
        if (c2d_cover(p, &m2, mode == 1, &st, s->w, s->h, &b2)) {
            C2dState s2 = st;
            s2.sh[4] = s2.sh[5] = 0;
            pt->ox = s->ox + sox; pt->oy = s->oy + soy;
            g_finish_part = 1;
            c2d_finish(s, &b2, pt, &s2, NULL);
            g_finish_part = 0;
            c2d_cover_clear(&b2);
            c2d_changed(s);
        }
        split = true;
        have = c2d_cover(p, &m, mode == 1, &st, s->w, s->h, &b);
    }
    plutovg_path_destroy(p);
    if (mode == 2) {
        c2d_clip_with(s, &b, have);
        if (have) c2d_cover_clear(&b);
        free(pt);
        return JS_UNDEFINED;
    }
    if (!have) {
        /* (nothing covered: an unbounded operator still clears) */
        if (mode <= 1 && c2d_unbounded(st.op)) {
            C2dBox z = { 0, 0, 0, 0 };
            float none[4] = { 0, 0, 0, 0 };
            c2d_composite(s, none, &z, st.op);
            c2d_changed(s);
        }
        free(pt);
        return JS_UNDEFINED;
    }
    if (mode == 3) {
        for (int y = b.y0; y < b.y1; y++)
            for (int x = b.x0; x < b.x1; x++) {
                float cv = c2d_cov_at(x, y) / 255.f * (s->clip ? s->clip[(size_t)y * s->w + x] / 255.f : 1.f);
                if (cv <= 0) continue;
                uint8_t *q = s->px + ((size_t)y * s->w + x) * 4;
                if (s->opaque && !s->nlayers) { for (int i = 0; i < 3; i++) q[i] = c2d_u8(q[i] / 255.f * (1 - cv)); continue; }
                q[3] = c2d_u8(q[3] / 255.f * (1 - cv));
                if (!q[3]) q[0] = q[1] = q[2] = 0;
            }
        c2d_cover_clear(&b);
        c2d_changed(s);
        free(pt);
        return JS_UNDEFINED;
    }
    if (pt_ok) { pt->ox = s->ox; pt->oy = s->oy; g_finish_part = split ? 2 : 0; c2d_finish(s, &b, pt, &st, NULL); g_finish_part = 0; }
    free(pt);
    c2d_cover_clear(&b);
    c2d_changed(s);
    return JS_UNDEFINED;
}

static GCValue js_c2d_save(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    C2dSurf *s = c2d_find(c2d_id(ctx, argv[0]));
    if (!s || !s->px) return JS_UNDEFINED;
    if (s->nstack == s->capstack) {
        int nc = s->capstack ? s->capstack * 2 : 8;
        uint8_t **nv = (uint8_t **)realloc(s->stack, (size_t)nc * sizeof *nv);
        if (!nv) return JS_UNDEFINED;
        s->stack = nv; s->capstack = nc;
    }
    uint8_t *c = NULL;
    if (s->clip) { c = (uint8_t *)malloc((size_t)s->w * s->h); if (c) memcpy(c, s->clip, (size_t)s->w * s->h); }
    s->stack[s->nstack++] = c;
    return JS_UNDEFINED;
}

static GCValue js_c2d_restore(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    C2dSurf *s = c2d_find(c2d_id(ctx, argv[0]));
    if (!s || !s->nstack) return JS_UNDEFINED;
    free(s->clip);
    s->clip = s->stack[--s->nstack];
    return JS_UNDEFINED;
}

/* __c2dBeginLayer(id, alpha, op, shadow[7], filter): what follows drawn on a new transparent bitmap */
static GCValue js_c2d_begin_layer(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 4) return JS_UNDEFINED;
    C2dSurf *s = c2d_find(c2d_id(ctx, argv[0]));
    if (!s || !s->px) return JS_UNDEFINED;
    if (s->nlayers == s->caplayers) {
        int nc = s->caplayers ? s->caplayers * 2 : 4;
        C2dLayer *nv = (C2dLayer *)realloc(s->layers, (size_t)nc * sizeof *nv);
        if (!nv) return JS_UNDEFINED;
        s->layers = nv; s->caplayers = nc;
    }
    int fn0 = 0;
    double *f0 = argc > 4 ? c2d_f64(ctx, argv[4], &fn0) : NULL;
    /* (a filter reaching out: a margin round the canvas, so what is drawn past its edges can still reach in) */
    int M = f0 && fn0 > 0 ? c2d_filter_reach(f0, fn0) : 0;
    if (M > 1024) M = 1024;
    if ((uint64_t)(s->w + 2 * M) * (uint64_t)(s->h + 2 * M) > C2D_MAX_PX) M = 0;
    uint8_t *fresh = (uint8_t *)calloc((size_t)(s->w + 2 * M) * (s->h + 2 * M), 4);
    if (!fresh) return JS_UNDEFINED;
    C2dLayer *L = &s->layers[s->nlayers++];
    memset(L, 0, sizeof *L);
    L->w = s->w; L->h = s->h; L->ox = s->ox; L->oy = s->oy;
    s->w += 2 * M; s->h += 2 * M; s->ox += M; s->oy += M;
    L->px = s->px;
    L->clip = s->clip;
    s->clip = NULL;
    L->depth = s->nstack;
    JS_ToFloat64(ctx, &L->alpha, argv[1]);
    int32_t op = 0;
    JS_ToInt32(ctx, &op, argv[2]);
    L->op = op;
    int sn = 0, fn = 0;
    double *sh = c2d_f64(ctx, argv[3], &sn);
    for (int i = 0; i < 7 && i < sn; i++) L->sh[i] = sh[i];
    double *f = argc > 4 ? c2d_f64(ctx, argv[4], &fn) : NULL;
    if (argc > 5) L->discard = JS_ToBool(ctx, argv[5]) > 0;
    if (argc > 6) L->own_filter = JS_ToBool(ctx, argv[6]) > 0;
    if (f && fn > 0) { L->filter = (double *)malloc(sizeof(double) * fn); if (L->filter) { memcpy(L->filter, f, sizeof(double) * fn); L->nfilter = fn; } }
    s->px = fresh;
    return JS_UNDEFINED;
}

static GCValue js_c2d_end_layer(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    C2dSurf *s = c2d_find(c2d_id(ctx, argv[0]));
    if (!s || !s->nlayers) return JS_UNDEFINED;
    C2dLayer L = s->layers[--s->nlayers];
    uint8_t *layer = s->px;
    int LW = s->w, LH = s->h, mx = s->ox - L.ox, my = s->oy - L.oy;
    /* (clips saved inside the layer dropped) */
    while (s->nstack > L.depth) free(s->stack[--s->nstack]);
    free(s->clip);
    s->px = L.px;
    s->clip = L.clip;
    s->w = L.w; s->h = L.h; s->ox = L.ox; s->oy = L.oy;
    C2dBox b = { 0, 0, s->w, s->h };
    size_t n = (size_t)s->w * s->h, nl = (size_t)LW * LH;
    float *big = (float *)malloc(nl * 4 * sizeof(float));
    float *pre = (float *)calloc(n * 4, sizeof(float));
    if (big && pre && !L.discard) {
        /* (the filter over the whole layer, margin and all; then cut to the bitmap under it) */
        for (size_t i = 0; i < nl; i++) c2d_load(layer + i * 4, big + i * 4);
        float la = (float)(L.alpha < 0 ? 0 : L.alpha > 1 ? 1 : L.alpha);
        if (la < 1 && !L.own_filter) for (size_t i = 0; i < nl * 4; i++) big[i] *= la;
        if (L.nfilter) { C2D_LAYER_F = 1; c2d_filter_apply(big, LW, LH, L.filter, L.nfilter); C2D_LAYER_F = 0; }
        if (la < 1 && L.own_filter) for (size_t i = 0; i < nl * 4; i++) big[i] *= la;
        for (int y = 0; y < s->h; y++) {
            int Y = y + my;
            if (Y < 0 || Y >= LH) continue;
            for (int x = 0; x < s->w; x++) {
                int X = x + mx;
                if (X < 0 || X >= LW) continue;
                memcpy(pre + ((size_t)y * s->w + x) * 4, big + ((size_t)Y * LW + X) * 4, 4 * sizeof(float));
            }
        }
        C2dState st;
        memset(&st, 0, sizeof st);
        st.alpha = 1; st.op = L.op;
        memcpy(st.sh, L.sh, sizeof st.sh);
        c2d_finish(s, &b, NULL, &st, pre);
    }
    free(big); free(pre);
    free(layer);
    free(L.filter);
    c2d_changed(s);
    return JS_UNDEFINED;
}

/* __c2dGet(id, x, y, w, h): an ArrayBuffer of unpremultiplied RGBA (outside: transparent) */
static GCValue js_c2d_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 5) return JS_UNDEFINED;
    C2dSurf *s = c2d_find(c2d_id(ctx, argv[0]));
    int32_t x = 0, y = 0, w = 0, h = 0;
    JS_ToInt32(ctx, &x, argv[1]); JS_ToInt32(ctx, &y, argv[2]); JS_ToInt32(ctx, &w, argv[3]); JS_ToInt32(ctx, &h, argv[4]);
    if (w <= 0 || h <= 0 || (uint64_t)w * h > C2D_MAX_PX) return JS_UNDEFINED;
    uint8_t *o = (uint8_t *)calloc((size_t)w * h, 4);
    if (!o) return JS_UNDEFINED;
    if (s && s->px) {
        for (int j = 0; j < h; j++) {
            int sy = y + j;
            if (sy < 0 || sy >= s->h) continue;
            for (int i = 0; i < w; i++) {
                int sx = x + i;
                if (sx < 0 || sx >= s->w) continue;
                const uint8_t *p = s->px + ((size_t)sy * s->w + sx) * 4;
                uint8_t *q = o + ((size_t)j * w + i) * 4;
                if (s->opaque) { for (int e = 0; e < 3; e++) q[e] = (uint8_t)((p[e] * p[3] + 127) / 255); q[3] = 255; continue; }
                memcpy(q, p, 4);
            }
        }
    }
    GCValue r = JS_NewArrayBufferCopy(ctx, o, (size_t)w * h * 4);
    free(o);
    return r;
}

/* __c2dPut(id, cw, ch, data, iw, ih, dx, dy, x0, y0, x1, y1): ImageData's pixels written as they are */
static GCValue js_c2d_put(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 12) return JS_UNDEFINED;
    C2dSurf *s = c2d_get(ctx, argv[0], argv[1], argv[2]);
    if (!s) return JS_UNDEFINED;
    size_t off = 0, len = 0, bpe = 0;
    GCValue buf = JS_GetTypedArrayBuffer(ctx, argv[3], &off, &len, &bpe);
    if (JS_IsException(buf)) return JS_UNDEFINED;
    size_t sz = 0;
    uint8_t *d = JS_GetArrayBuffer(ctx, &sz, buf);
    int32_t v[8];
    for (int i = 0; i < 8; i++) JS_ToInt32(ctx, &v[i], argv[4 + i]);
    int iw = v[0], ih = v[1], dx = v[2], dy = v[3];
    if (!d || iw <= 0 || ih <= 0 || (size_t)iw * ih * 4 > len) return JS_UNDEFINED;
    d += off;
    for (int y = v[5]; y < v[7]; y++) {
        if (y < 0 || y >= ih || dy + y < 0 || dy + y >= s->h) continue;
        for (int x = v[4]; x < v[6]; x++) {
            if (x < 0 || x >= iw || dx + x < 0 || dx + x >= s->w) continue;
            const uint8_t *p = d + ((size_t)y * iw + x) * 4;
            uint8_t *q = s->px + ((size_t)(dy + y) * s->w + (dx + x)) * 4;
            memcpy(q, p, 4);
            if (!q[3]) q[0] = q[1] = q[2] = 0;
            if (s->opaque) q[3] = 255;
        }
    }
    c2d_changed(s);
    return JS_UNDEFINED;
}

/* __c2dCopy(dst, src[, sx, sy, sw, sh]): dst a copy of src's bitmap (or a part); returns its size or null */
static GCValue js_c2d_copy(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 2) return JS_NULL;
    uint64_t did = c2d_id(ctx, argv[0]);
    C2dImg im;
    c2d_img_of(ctx, argv[1], &im);
    if (!did || !im.px) return JS_NULL;
    int32_t sx = 0, sy = 0, sw = im.w, sh = im.h;
    if (argc >= 6) { JS_ToInt32(ctx, &sx, argv[2]); JS_ToInt32(ctx, &sy, argv[3]); JS_ToInt32(ctx, &sw, argv[4]); JS_ToInt32(ctx, &sh, argv[5]); }
    if (sw <= 0 || sh <= 0) return JS_NULL;
    C2dImg keep = im;
    uint8_t *copy = NULL;
    if (im.ch == 4 && (JS_IsNumber(argv[1]) || JS_IsObject(argv[1])) && c2d_find(c2d_id(ctx, argv[1])) == c2d_find(did)) {
        copy = (uint8_t *)malloc((size_t)im.w * im.h * 4);
        if (!copy) return JS_NULL;
        memcpy(copy, im.px, (size_t)im.w * im.h * 4);
        keep.px = copy;
    }
    C2dSurf *s = c2d_make(did, sw, sh, false);
    if (!s || !s->px) { free(copy); return JS_NULL; }
    if (did < C2D_OFF_BASE) { s->shown = true; c2d_changed(s); }
    for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) {
        int X = sx + x, Y = sy + y;
        if (X < 0 || Y < 0 || X >= keep.w || Y >= keep.h) continue;
        float c[4];
        c2d_texel(&keep, X, Y, c);
        uint8_t *q = s->px + ((size_t)y * sw + x) * 4;
        c2d_store(q, c);
    }
    free(copy);
    GCValue r = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, r, 0, JS_NewInt32(ctx, sw));
    JS_SetPropertyUint32(ctx, r, 1, JS_NewInt32(ctx, sh));
    return r;
}

/* __c2dSize(src): [w, h] of an image source's pixels, or null */
static GCValue js_c2d_size(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NULL;
    C2dImg im;
    c2d_img_of(ctx, argv[0], &im);
    if (!im.px) return JS_NULL;
    GCValue r = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, r, 0, JS_NewInt32(ctx, im.w));
    JS_SetPropertyUint32(ctx, r, 1, JS_NewInt32(ctx, im.h));
    return r;
}

typedef struct { uint8_t *d; size_t n, cap; } C2dBuf;
static void c2d_png_write(void *closure, void *data, int size)
{
    C2dBuf *b = (C2dBuf *)closure;
    if (b->n + (size_t)size > b->cap) {
        size_t nc = (b->cap ? b->cap * 2 : 4096);
        while (nc < b->n + (size_t)size) nc *= 2;
        uint8_t *nd = (uint8_t *)realloc(b->d, nc);
        if (!nd) return;
        b->d = nd; b->cap = nc;
    }
    memcpy(b->d + b->n, data, (size_t)size);
    b->n += (size_t)size;
}

/* __c2dPng(id[, jpegQuality]): the bitmap encoded (PNG, or JPEG when a quality is given), an ArrayBuffer */
static GCValue js_c2d_png(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NULL;
    C2dSurf *s = c2d_find(c2d_id(ctx, argv[0]));
    if (!s || !s->px) return JS_NULL;
    plutovg_surface_t *sf = plutovg_surface_create(s->w, s->h);
    if (!sf) return JS_NULL;
    uint8_t *d = plutovg_surface_get_data(sf);
    int stride = plutovg_surface_get_stride(sf);
    for (int y = 0; y < s->h; y++) for (int x = 0; x < s->w; x++) {
        const uint8_t *p = s->px + ((size_t)y * s->w + x) * 4;
        uint32_t a = s->opaque ? 255 : p[3], A = p[3];
        uint32_t v = a << 24 | (p[0] * A + 127) / 255 << 16 | (p[1] * A + 127) / 255 << 8 | (p[2] * A + 127) / 255;
        memcpy(d + (size_t)y * stride + (size_t)x * 4, &v, 4);
    }
    C2dBuf b = { 0 };
    double q = -1;
    if (argc > 1 && JS_IsNumber(argv[1])) JS_ToFloat64(ctx, &q, argv[1]);
    bool ok = q >= 0 ? plutovg_surface_write_to_jpg_stream(sf, c2d_png_write, &b, (int)(q * 100)) : plutovg_surface_write_to_png_stream(sf, c2d_png_write, &b);
    plutovg_surface_destroy(sf);
    GCValue r = ok && b.d ? JS_NewArrayBufferCopy(ctx, b.d, b.n) : JS_NULL;
    free(b.d);
    return r;
}

/* ---- text ---- */
/* (weight: + 1000 x the stretch in tenths of a percent; italic 1, oblique 2) */
static int c2d_font(JSContextHandle ctx, GCValue fam, int32_t weight, int italic)
{
    const char *f = JS_ToCString(ctx, fam);
    int stretch = weight / 1000;   /* (tenths) */
    weight %= 1000;
    const char *fl = f && *f ? f : "sans-serif";
    int q0 = font_registry_queued_count();
    int slot = font_registry_resolve_style(fl, weight > 0 ? weight : 400, italic == 1, italic == 2, 14.0f, stretch > 0 ? (float)stretch / 10.0f : 100.0f);
    /* (a web face it asked for now, not loaded: loaded, the script waited on the face set's ready for it) */
    if (font_registry_queued_count() > q0 && font_registry_load_pending() > 0)
        slot = font_registry_resolve_style(fl, weight > 0 ? weight : 400, italic == 1, italic == 2, 14.0f, stretch > 0 ? (float)stretch / 10.0f : 100.0f);
    if (slot < 0) slot = font_registry_default_slot();
    return slot;
}

/* __c2dSpanW(text, family, weight, italic, size, [a0, b0, a1, b1, ...]): the advance of each UTF-16 range [a, b) of text,
 * with what is around it counted (fullwidth punctuation side by side), as the page's text is measured */
static GCValue js_c2d_span_w(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 6) return JS_NULL;
    int pn = 0;
    double *pa0 = c2d_f64(ctx, argv[5], &pn);
    /* (a copy: the typed array's own storage is not ours to keep) */
    double *pa = pa0 && pn > 0 ? (double *)malloc((size_t)pn * sizeof(double)) : NULL;
    if (pa) memcpy(pa, pa0, (size_t)pn * sizeof(double));
    size_t n = 0;
    const char *txt = JS_ToCStringLen(ctx, &n, argv[0]);
    int32_t weight = 400, italic = 0;
    double size = 10;
    JS_ToInt32(ctx, &weight, argv[2]);
    JS_ToInt32(ctx, &italic, argv[3]);
    JS_ToFloat64(ctx, &size, argv[4]);
    int slot = c2d_font(ctx, argv[1], weight, italic);
    TextShaper *sh = slot >= 0 ? font_registry_slot(slot) : NULL;
    if (!txt || !sh || !(size > 0) || !pa) { free(pa); return JS_NULL; }
    char *t = (char *)malloc(n + 1);
    size_t *b16 = (size_t *)malloc((n + 2) * sizeof(size_t));   /* (the byte each UTF-16 offset is at) */
    if (!t || !b16) { free(t); free(b16); free(pa); return JS_NULL; }
    for (size_t i = 0; i < n; i++) { char c = txt[i]; t[i] = (c == '\t' || c == '\n' || c == '\f' || c == '\r') ? ' ' : c; }
    t[n] = 0;
    size_t u = 0;
    for (size_t i = 0; i < n; ) {
        unsigned char c = (unsigned char)t[i];
        size_t l = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        b16[u++] = i;
        if (l == 4) b16[u++] = i + l;   /* (a surrogate pair: its second unit, past it) */
        i += l;
    }
    b16[u] = n;
    /* (the scale: the page's measure of all of it to the outline's advance) */
    char *out = NULL;
    size_t len = 0, cap = 0;
    double adv = text_shaper_outline_path(sh, t, n, size, 0, 0, &out, &len, &cap);
    free(out);
    double full = text_shaper_span_width(sh, t, 0, n, 1.0f);
    double k = full > 0 ? adv / full : 0;
    GCValue r = JS_NewArray(ctx);
    for (int q = 0; q + 1 < pn; q += 2) {
        size_t a = (size_t)pa[q], b = (size_t)pa[q + 1];
        if (a > u) a = u;
        if (b > u) b = u;
        double w = b > a ? text_shaper_span_width(sh, t, b16[a], b16[b], 1.0f) * k : 0;
        JS_SetPropertyUint32(ctx, r, (uint32_t)(q / 2), JS_NewFloat64(ctx, w));
    }
    free(t); free(b16); free(pa);
    return r;
}

/* __c2dBidi(text, rtl): the bidi embedding level of each UTF-16 unit of text, a paragraph of base direction rtl (UAX #9, L1 applied) */
static GCValue js_c2d_bidi(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NULL;
    GCValue len_v = JS_GetPropertyStr(ctx, argv[0], "length");
    int32_t n = 0;
    JS_ToInt32(ctx, &n, len_v);
    if (n <= 0 || n > 100000) return JS_NewArray(ctx);
    int32_t rtl = 0;
    if (argc > 1) JS_ToInt32(ctx, &rtl, argv[1]);
    uint32_t *cp = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    int *unit = (int *)malloc((size_t)n * sizeof(int));
    uint8_t *lv = (uint8_t *)malloc((size_t)n);
    if (!cp || !unit || !lv) { free(cp); free(unit); free(lv); return JS_NULL; }
    GCValue cca = JS_GetPropertyStr(ctx, argv[0], "charCodeAt");
    int k = 0;
    uint32_t *cu = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    if (!cu) { free(cp); free(unit); free(lv); return JS_NULL; }
    for (int i = 0; i < n; i++) {
        GCValue a = JS_NewInt32(ctx, i);
        GCValue r = JS_Call(ctx, cca, argv[0], 1, &a);
        int32_t c = 0;
        JS_ToInt32(ctx, &c, r);
        cu[i] = (uint32_t)c;
    }
    for (int i = 0; i < n; i++) {
        uint32_t c = cu[i];
        unit[i] = k;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && cu[i + 1] >= 0xDC00 && cu[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (cu[i + 1] - 0xDC00);
            unit[i + 1] = k;
            i++;
        }
        cp[k++] = c;
    }
    int pl = bidi_resolve(cp, k, rtl ? 1 : 0, lv);
    if (pl < 0) pl = rtl ? 1 : 0;
    bidi_line_levels(cp, 0, k, pl, lv);
    GCValue out = JS_NewArray(ctx);
    for (int i = 0; i < n; i++) JS_SetPropertyUint32(ctx, out, (uint32_t)i, JS_NewInt32(ctx, lv[unit[i]]));
    free(cp); free(unit); free(lv); free(cu);
    return out;
}

/* __c2dFontScan(css, base): @font-face rules a script made (a FontFace added to a set), known to text drawing at once */
static GCValue js_c2d_font_scan(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    size_t n = 0;
    const char *css = JS_ToCStringLen(ctx, &n, argv[0]), *base = JS_ToCString(ctx, argv[1]);
    if (css && base) font_registry_scan_css(css, n, base);
    return JS_UNDEFINED;
}

/* (utf8 in visual order: the bidi algorithm with paragraph level rtl, mirrored glyphs at odd levels; malloc'ed) */
static char *c2d_visual(const char *t, size_t n, int rtl)
{
    uint32_t *cp = (uint32_t *)malloc((n + 1) * sizeof(uint32_t));
    uint8_t *lv = (uint8_t *)malloc(n + 1);
    int *ord = (int *)malloc((n + 1) * sizeof(int));
    char *out = (char *)malloc(n * 4 + 1);
    if (!cp || !lv || !ord || !out) { free(cp); free(lv); free(ord); free(out); return NULL; }
    int k = 0;
    const unsigned char *p = (const unsigned char *)t, *e = p + n;
    while (p < e) {
        uint32_t c = *p++;
        if (c >= 0xF0 && p + 2 < e + 0) { c = (c & 7) << 18 | (uint32_t)(p[0] & 63) << 12 | (uint32_t)(p[1] & 63) << 6 | (p[2] & 63); p += 3; }
        else if (c >= 0xF0 && p + 3 <= e) { c = (c & 7) << 18 | (uint32_t)(p[0] & 63) << 12 | (uint32_t)(p[1] & 63) << 6 | (p[2] & 63); p += 3; }
        else if (c >= 0xE0 && p + 2 <= e) { c = (c & 15) << 12 | (uint32_t)(p[0] & 63) << 6 | (p[1] & 63); p += 2; }
        else if (c >= 0xC0 && p + 1 <= e) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
        cp[k++] = c;
    }
    int pl = bidi_resolve(cp, k, rtl ? 1 : 0, lv);
    if (pl < 0) pl = rtl ? 1 : 0;
    bidi_line_levels(cp, 0, k, pl, lv);
    bidi_reorder(lv, k, ord);
    size_t o = 0;
    for (int i = 0; i < k; i++) {
        uint32_t c = cp[ord[i]];
        if (lv[ord[i]] & 1) c = bidi_mirror(c);
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); }
        else { out[o++] = (char)(0xF0 | c >> 18); out[o++] = (char)(0x80 | ((c >> 12) & 63)); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); }
    }
    out[o] = 0;
    free(cp); free(lv); free(ord);
    return out;
}

/* __c2dText(id, w, h, text, family, weight, italic, size, x, y, align, baseline, maxw, stroke, paint, state, src, letter, word)
 * align: 0 left, 1 right, 2 center; baseline: 0 alphabetic, 1 top, 2 hanging, 3 middle, 4 ideographic, 5 bottom */
/* (t_text_x/y: where the pen starts, baked into the outline: text drawn in pieces rounds as the whole does) */
static __thread double t_text_x, t_text_y;
extern _Thread_local bool text_shaper_outline_glyph_sep;
extern _Thread_local bool text_shaper_outline_no_kern;
/* __c2dNoKern(on): the text drawn and measured from now on without kerning (font-kerning: none) */
static GCValue js_c2d_nokern(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    text_shaper_outline_no_kern = argc > 0 && JS_ToBool(ctx, argv[0]) > 0;
    return JS_UNDEFINED;
}
static __thread char *t_text_raw;   /* (set: the outline's SVG data kept here, glyphs apart, as well) */
static __thread bool t_text_want_raw;
static bool c2d_text_path(JSContextHandle ctx, GCValue *argv, plutovg_path_t *p, double *adv_out, double *asc, double *desc, int rtl)
{
    const char *txt = JS_ToCString(ctx, argv[0]);
    int32_t weight = 400, italic = 0;
    double size = 10;
    JS_ToInt32(ctx, &weight, argv[2]);
    JS_ToInt32(ctx, &italic, argv[3]);
    JS_ToFloat64(ctx, &size, argv[4]);
    int slot = c2d_font(ctx, argv[1], weight, italic);
    TextShaper *sh = slot >= 0 ? font_registry_slot(slot) : NULL;
    if (!txt || !sh || !(size > 0)) return false;
    float a = 0.8f, d = 0.2f, g = 0;
    text_shaper_vmetrics(sh, &a, &d, &g);
    *asc = a * size; *desc = d * size;
    /* (white space other than a space: as spaces) */
    size_t n = strlen(txt);
    char *t = (char *)malloc(n + 1);
    if (!t) return false;
    for (size_t i = 0; i < n; i++) { char c = txt[i]; t[i] = (c == '\t' || c == '\n' || c == '\f' || c == '\r') ? ' ' : c; }
    t[n] = 0;
    char *vis = c2d_visual(t, n, rtl);
    if (vis) { free(t); t = vis; n = strlen(t); }
    char *out = NULL;
    size_t len = 0, cap = 0;
    text_shaper_outline_glyph_sep = t_text_want_raw;
    double adv = text_shaper_outline_path(sh, t, n, size, t_text_x, t_text_y, &out, &len, &cap) - t_text_x;
    text_shaper_outline_glyph_sep = false;
    free(t);
    *adv_out = adv;
    if (t_text_want_raw) {
        /* (the path without the separators; the data with them kept) */
        if (out && len) {
            char *clean = (char *)malloc(len + 1);
            size_t cl = 0;
            if (clean) { for (size_t i = 0; i < len; i++) if (out[i] != '|') clean[cl++] = out[i]; clean[cl] = 0; plutovg_path_parse(p, clean, (int)cl); free(clean); }
        }
        free(t_text_raw);
        t_text_raw = out;
        return true;
    }
    if (out && len) plutovg_path_parse(p, out, (int)len);
    free(out);
    return true;
}

/* __c2dMeasure(text, family, weight, italic, size): [width, ascent, descent] */
static GCValue js_c2d_measure(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 5) return JS_NULL;
    plutovg_path_t *p = plutovg_path_create();
    double adv = 0, asc = 0, desc = 0;
    int32_t rtl = 0;
    if (argc > 5) JS_ToInt32(ctx, &rtl, argv[5]);
    bool ok = c2d_text_path(ctx, argv, p, &adv, &asc, &desc, rtl);
    plutovg_rect_t ext = { 0, 0, 0, 0 };
    if (ok) plutovg_path_extents(p, &ext, true);
    plutovg_path_destroy(p);
    if (!ok) return JS_NULL;
    GCValue r = JS_NewArray(ctx);
    double v[7] = { adv, asc, desc, ext.x, ext.y, ext.x + ext.w, ext.y + ext.h };
    for (int i = 0; i < 7; i++) JS_SetPropertyUint32(ctx, r, (uint32_t)i, JS_NewFloat64(ctx, v[i]));
    return r;
}

static GCValue js_c2d_text(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 17) return JS_UNDEFINED;
    C2dSurf *s = c2d_get(ctx, argv[0], argv[1], argv[2]);
    if (!s) return JS_UNDEFINED;
    plutovg_path_t *p = plutovg_path_create();
    double adv = 0, asc = 0, desc = 0;
    int32_t rtl = 0;
    if (argc > 17) JS_ToInt32(ctx, &rtl, argv[17]);
    if (!c2d_text_path(ctx, argv + 3, p, &adv, &asc, &desc, rtl)) { plutovg_path_destroy(p); return JS_UNDEFINED; }
    double x = 0, y = 0, maxw = -1;
    int32_t align = 0, base = 0, stroke = 0;
    JS_ToFloat64(ctx, &x, argv[8]); JS_ToFloat64(ctx, &y, argv[9]);
    JS_ToInt32(ctx, &align, argv[10]); JS_ToInt32(ctx, &base, argv[11]);
    JS_ToFloat64(ctx, &maxw, argv[12]);
    JS_ToInt32(ctx, &stroke, argv[13]);
    int qn = 0, sn = 0;
    double *qa = c2d_f64(ctx, argv[14], &qn), *sa = c2d_f64(ctx, argv[15], &sn);
    C2dState st;
    if (!c2d_state(sa, sn, &st)) { plutovg_path_destroy(p); return JS_UNDEFINED; }
    double kx = 1;
    if (maxw >= 0 && adv > maxw) kx = adv > 0 ? maxw / adv : 0;
    if (maxw == 0) { plutovg_path_destroy(p); return JS_UNDEFINED; }
    double w = adv * kx;
    double ox = align == 1 ? -w : align == 2 ? -w / 2 : 0;
    double tot = asc + desc, na = tot > 0 ? asc / tot * (asc + desc) : asc;
    double em = 0;
    JS_ToFloat64(ctx, &em, argv[7]);
    if (tot > 0) na = asc / tot * em;
    double nd = tot > 0 ? desc / tot * em : desc;
    double oy = base == 1 ? na : base == 2 ? asc * 0.8 : base == 3 ? (na - nd) / 2 : base == 4 ? -desc : base == 5 ? -nd : 0;
    /* (user space: text at (x + ox, y + oy), squeezed along x) */
    double t[6] = { kx, 0, 0, 1, x + ox, y + oy };
    if (kx == 1) {
        /* (not squeezed: the outline made again with its pen at the place) */
        plutovg_path_t *p2 = plutovg_path_create();
        double a2 = 0, s2 = 0, d2 = 0;
        t_text_x = x + ox; t_text_y = y + oy;
        t_text_want_raw = stroke != 0;
        bool ok2 = p2 && c2d_text_path(ctx, argv + 3, p2, &a2, &s2, &d2, rtl);
        t_text_want_raw = false;
        t_text_x = t_text_y = 0;
        if (ok2) { plutovg_path_destroy(p); p = p2; t[4] = 0; t[5] = 0; }
        else if (p2) plutovg_path_destroy(p2);
        /* (stroked: each glyph's outline stroked and painted on its own, overlaps twice over, as browsers draw glyph runs) */
        char *raw = t_text_raw;
        t_text_raw = NULL;
        if (ok2 && stroke && raw && strchr(raw, '|')) {
            st.m[4] += s->ox; st.m[5] += s->oy;
            plutovg_matrix_t gm;
            plutovg_matrix_init(&gm, (float)st.m[0], (float)st.m[1], (float)st.m[2], (float)st.m[3], (float)st.m[4], (float)st.m[5]);
            for (char *seg = raw; seg && *seg; ) {
                char *bar = strchr(seg, '|');
                if (bar) *bar = 0;
                if (*seg) {
                    plutovg_path_t *gp = plutovg_path_create();
                    if (gp) {
                        plutovg_path_parse(gp, seg, (int)strlen(seg));
                        C2dBox gb;
                        if (c2d_cover(gp, &gm, true, &st, s->w, s->h, &gb)) {
                            C2dPaint *gpt = (C2dPaint *)malloc(sizeof *gpt);
                            if (gpt && c2d_paint(ctx, qa, qn, argv[16], st.smooth, gpt)) { gpt->ox = s->ox; gpt->oy = s->oy; c2d_finish(s, &gb, gpt, &st, NULL); }
                            free(gpt);
                            c2d_cover_clear(&gb);
                        }
                        plutovg_path_destroy(gp);
                    }
                }
                seg = bar ? bar + 1 : NULL;
            }
            free(raw);
            plutovg_path_destroy(p);
            c2d_changed(s);
            return JS_UNDEFINED;
        }
        free(raw);
    }
    st.m[4] += s->ox; st.m[5] += s->oy;
    double *M = st.m;
    double mm[6] = { M[0] * t[0] + M[2] * t[1], M[1] * t[0] + M[3] * t[1], M[0] * t[2] + M[2] * t[3], M[1] * t[2] + M[3] * t[3],
                     M[0] * t[4] + M[2] * t[5] + M[4], M[1] * t[4] + M[3] * t[5] + M[5] };
    plutovg_matrix_t m;
    C2dBox b;
    bool have;
    if (stroke) {
        /* (the outline in user space, stroked with the user space's line width) */
        plutovg_matrix_t tm;
        plutovg_matrix_init(&tm, (float)t[0], (float)t[1], (float)t[2], (float)t[3], (float)t[4], (float)t[5]);
        plutovg_path_transform(p, &tm);
        plutovg_matrix_init(&m, (float)M[0], (float)M[1], (float)M[2], (float)M[3], (float)M[4], (float)M[5]);
        have = c2d_cover(p, &m, true, &st, s->w, s->h, &b);
    } else {
        plutovg_matrix_init(&m, (float)mm[0], (float)mm[1], (float)mm[2], (float)mm[3], (float)mm[4], (float)mm[5]);
        st.rule = 0;
        have = c2d_cover(p, &m, false, &st, s->w, s->h, &b);
    }
    plutovg_path_destroy(p);
    if (!have) return JS_UNDEFINED;
    C2dPaint *pt = (C2dPaint *)malloc(sizeof *pt);
    if (pt && c2d_paint(ctx, qa, qn, argv[16], st.smooth, pt)) { pt->ox = s->ox; pt->oy = s->oy; c2d_finish(s, &b, pt, &st, NULL); }
    free(pt);
    c2d_cover_clear(&b);
    c2d_changed(s);
    return JS_UNDEFINED;
}

/* __c2dHit(w, h, path, rule, x, y, stroke, state): whether device point (x, y) is in the path's fill (or stroke) */
static GCValue js_c2d_hit(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 8) return JS_FALSE;
    int pn = 0, sn = 0;
    double *pa = c2d_f64(ctx, argv[2], &pn), *sa = c2d_f64(ctx, argv[7], &sn);
    int32_t rule = 0, stroke = 0;
    double x = 0, y = 0;
    JS_ToInt32(ctx, &rule, argv[3]);
    JS_ToFloat64(ctx, &x, argv[4]); JS_ToFloat64(ctx, &y, argv[5]);
    JS_ToInt32(ctx, &stroke, argv[6]);
    if (!pa || !isfinite(x) || !isfinite(y)) return JS_FALSE;
    plutovg_path_t *p = plutovg_path_create();
    c2d_path_build(p, pa, pn);
    C2dState st;
    if (!c2d_state(sa, sn, &st)) memset(&st, 0, sizeof st);
    st.rule = rule;
    /* (a small canvas round the point: the path moved so the point is at its centre) */
    plutovg_matrix_t m;
    plutovg_matrix_init_identity(&m);
    if (stroke) {
        double inv[6];
        if (!c2d_invert(st.m, inv)) { plutovg_path_destroy(p); return JS_FALSE; }
        plutovg_matrix_t im;
        plutovg_matrix_init(&im, (float)inv[0], (float)inv[1], (float)inv[2], (float)inv[3], (float)inv[4], (float)inv[5]);
        plutovg_path_transform(p, &im);
        plutovg_matrix_init(&m, (float)st.m[0], (float)st.m[1], (float)st.m[2], (float)st.m[3], (float)(st.m[4] - floor(x) + 8), (float)(st.m[5] - floor(y) + 8));
    } else plutovg_matrix_init_translate(&m, (float)(-floor(x) + 8), (float)(-floor(y) + 8));
    C2dBox b;
    bool have = c2d_cover(p, &m, stroke != 0, &st, 17, 17, &b);
    plutovg_path_destroy(p);
    if (!have) return JS_FALSE;
    bool in = c2d_cov_at(8, 8) >= 128;
    c2d_cover_clear(&b);
    return in ? JS_TRUE : JS_FALSE;
}

/* __c2dElem(id, w, h, el, x, y, dw, dh): a child element drawn at (x, y) (dw, dh <= 0: its own size) */
static GCValue js_c2d_elem(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 8) return JS_UNDEFINED;
    C2dSurf *s = c2d_get(ctx, argv[0], argv[1], argv[2]);
    NodeId el = dom_node_of(argv[3]);
    if (!s || !el) return JS_UNDEFINED;
    double v[4];
    for (int i = 0; i < 4; i++) if (JS_ToFloat64(ctx, &v[i], argv[4 + i])) return JS_UNDEFINED;
    int k = 0;
    while (k < s->ned && s->ed[k].el != (uint32_t)el) k++;
    if (k == s->ned) {
        if (s->ned == s->caped) {
            int nc = s->caped ? s->caped * 2 : 4;
            void *nv = realloc(s->ed, (size_t)nc * sizeof *s->ed);
            if (!nv) return JS_UNDEFINED;
            s->ed = nv; s->caped = nc;
        }
        s->ned++;
    }
    s->ed[k].el = (uint32_t)el; s->ed[k].x = (float)v[0]; s->ed[k].y = (float)v[1]; s->ed[k].w = (float)v[2]; s->ed[k].h = (float)v[3];
    { double ga = 1.0; if (argc > 8) JS_ToFloat64(ctx, &ga, argv[8]); s->ed[k].a = (float)(ga < 0 ? 0 : ga > 1 ? 1 : ga); }   /* (its global alpha then) */
    {
        double sr[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < 4 && argc > 9 + i; i++) if (JS_ToFloat64(ctx, &sr[i], argv[9 + i])) sr[i] = 0;
        s->ed[k].sx = (float)sr[0]; s->ed[k].sy = (float)sr[1]; s->ed[k].sw = (float)sr[2]; s->ed[k].sh = (float)sr[3];
    }
    /* (the context's filter and shadow then: "t:v,t:v|dx,dy,blur,r,g,b,a" in CSS filter's kinds) */
    s->ed[k].fn = 0;
    memset(s->ed[k].ds, 0, sizeof s->ed[k].ds);
    if (argc > 13 && JS_IsString(argv[13])) {
        const char *fx = JS_ToCString(ctx, argv[13]);
        if (fx) {
            const char *p = fx;
            while (*p && *p != '|' && s->ed[k].fn < 8) {
                int t = 0; double v = 0; int used = 0;
                if (sscanf(p, "%d:%lf%n", &t, &v, &used) < 2 || used <= 0) break;
                s->ed[k].ft[s->ed[k].fn] = (unsigned char)t; s->ed[k].fv[s->ed[k].fn] = (float)v; s->ed[k].fn++;
                p += used;
                if (*p == ',') p++;
            }
            const char *bar = strchr(p, '|');
            if (bar) {
                double d[7];
                if (sscanf(bar + 1, "%lf,%lf,%lf,%lf,%lf,%lf,%lf", &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6]) == 7)
                    for (int i = 0; i < 7; i++) s->ed[k].ds[i] = (float)d[i];
            }
        }
    }
    c2d_changed(s);
    return JS_UNDEFINED;
}

/* (the global alpha a child element was drawn with; 1 when not drawn) */
float canvas2d_elem_alpha(uint32_t canvas, uint32_t el)
{
    C2dSurf *s = c2d_find(canvas);
    for (int k = 0; s && k < s->ned; k++) if (s->ed[k].el == el) return s->ed[k].a;
    return 1.0f;
}

/* (the part of a child drawn: CSS px from its border box's corner; false the whole border box) */
bool canvas2d_elem_src(uint32_t canvas, uint32_t el, float out[4])
{
    C2dSurf *s = c2d_find(canvas);
    for (int k = 0; s && k < s->ned; k++)
        if (s->ed[k].el == el) {
            if (!(s->ed[k].sw > 0) || !(s->ed[k].sh > 0)) return false;
            out[0] = s->ed[k].sx; out[1] = s->ed[k].sy; out[2] = s->ed[k].sw; out[3] = s->ed[k].sh;
            return true;
        }
    return false;
}

/* (the filter a child was drawn through, as CSS filter's (t, v, n) with its drop shadow in ds; false none) */
bool canvas2d_elem_fx(uint32_t canvas, uint32_t el, unsigned char *n, unsigned char t[8], float v[8], float ds[7])
{
    C2dSurf *s = c2d_find(canvas);
    for (int k = 0; s && k < s->ned; k++)
        if (s->ed[k].el == el) {
            if (!s->ed[k].fn) return false;
            *n = s->ed[k].fn;
            memcpy(t, s->ed[k].ft, 8); memcpy(v, s->ed[k].fv, 8 * sizeof(float)); memcpy(ds, s->ed[k].ds, 7 * sizeof(float));
            return true;
        }
    return false;
}

bool canvas2d_elem_draw(uint32_t canvas, uint32_t el, float out[4], int *bw, int *bh)
{
    C2dSurf *s = c2d_find(canvas);
    if (!s) return false;
    for (int k = 0; k < s->ned; k++)
        if (s->ed[k].el == el) {
            out[0] = s->ed[k].x; out[1] = s->ed[k].y; out[2] = s->ed[k].w; out[3] = s->ed[k].h;
            *bw = s->nlayers ? s->layers[0].w : s->w; *bh = s->nlayers ? s->layers[0].h : s->h;
            return true;
        }
    return false;
}

/* __c2dGrad(css, w, h): a CSS gradient as an image of that size, its key (null: none) */
static GCValue js_c2d_grad(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 3) return JS_NULL;
    const char *css = JS_ToCString(ctx, argv[0]);
    int32_t w = 0, h = 0;
    JS_ToInt32(ctx, &w, argv[1]); JS_ToInt32(ctx, &h, argv[2]);
    char key[256];
    if (!css || !display_list_gradient_image(css, w, h, key, sizeof key)) return JS_NULL;
    return JS_NewString(ctx, key);
}

/* __c2dKey(id): the bitmap put in the image cache (a paint worklet's, drawn as an image); its key, or null */
static GCValue js_c2d_key(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NULL;
    C2dSurf *s = c2d_find(c2d_id(ctx, argv[0]));
    if (!s || !s->px) return JS_NULL;
    char key[64];
    ImageCache *ic = display_list_get_image_cache();
    if (!ic) return JS_NULL;
    snprintf(key, sizeof key, "cyber-c2d:%llu", (unsigned long long)s->epoch);
    size_t n = (size_t)s->w * s->h;
    uint8_t *rgba = (uint8_t *)malloc(n * 4);
    if (!rgba) return JS_NULL;
    memcpy(rgba, s->px, n * 4);
    image_cache_put_rgba(ic, key, s->w, s->h, rgba);
    free(rgba);
    return JS_NewString(ctx, key);
}

void canvas2d_api_install(JSContextHandle ctx)
{
    GCValue g = JS_GetGlobalObject(ctx);
    struct { const char *n; JSCFunction *f; int a; } fns[] = {
        { "__c2dReset", js_c2d_reset, 4 }, { "__c2dNew", js_c2d_new, 0 }, { "__c2dNodeId", js_c2d_node_id, 1 }, { "__c2dFree", js_c2d_free, 1 },
        { "__c2dDraw", js_c2d_draw, 8 }, { "__c2dSave", js_c2d_save, 1 }, { "__c2dRestore", js_c2d_restore, 1 },
        { "__c2dBeginLayer", js_c2d_begin_layer, 5 }, { "__c2dEndLayer", js_c2d_end_layer, 1 },
        { "__c2dGet", js_c2d_get, 5 }, { "__c2dPut", js_c2d_put, 12 }, { "__c2dCopy", js_c2d_copy, 6 },
        { "__c2dSize", js_c2d_size, 1 }, { "__c2dPng", js_c2d_png, 2 }, { "__c2dMeasure", js_c2d_measure, 6 },
        { "__c2dText", js_c2d_text, 18 }, { "__c2dHit", js_c2d_hit, 8 }, { "__c2dKey", js_c2d_key, 1 }, { "__c2dElem", js_c2d_elem, 14 }, { "__c2dNoKern", js_c2d_nokern, 1 }, { "__c2dGrad", js_c2d_grad, 3 },
        { "__c2dFontScan", js_c2d_font_scan, 2 }, { "__c2dBidi", js_c2d_bidi, 2 }, { "__c2dSpanW", js_c2d_span_w, 6 },
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++)
        JS_DefinePropertyValueStr(ctx, g, fns[i].n, JS_NewCFunction(ctx, fns[i].f, fns[i].n, fns[i].a), JS_PROP_CONFIGURABLE);
}

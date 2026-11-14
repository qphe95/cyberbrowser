/* The filter property's reference filters (Filter Effects 1): a <filter> element's
 * primitives evaluated on the CPU over what the box and its subtree painted.
 *
 * Every image is premultiplied RGBA floats over one area (the viewport cut to the
 * regions in play), kept in whichever color space the primitive that made it worked in
 * (the `lin` flag); a primitive converts its inputs to its own. */
#include "svg_filter.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct { float *p; bool lin; bool owned; float sr[4]; bool taint; } FImg;   /* (sr: its subregion: x0 y0 x1 y1; taint: from page content) */
typedef struct {
    int ax, ay, aw, ah;   /* the area */
    float box[4];         /* the reference box: x y w h */
    float region[4];      /* the filter's region: x0 y0 x1 y1 */
    bool prim_bbox;       /* primitiveUnits = objectBoundingBox */
    float fs;
    const SvgFilterEnv *env;
} FCtx;

static float srgb_to_lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float lin_to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f; }
static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

static float *img_new(const FCtx *C) { return (float *)calloc((size_t)C->aw * C->ah, 4 * sizeof(float)); }

/* (im in the space asked for, a copy unless it is there already) */
static float *img_in_space(const FCtx *C, const FImg *im, bool lin, bool *owned)
{
    if (im->lin == lin) { *owned = false; return im->p; }
    float *o = img_new(C);
    *owned = o != NULL;
    if (!o) return NULL;
    size_t n = (size_t)C->aw * C->ah;
    for (size_t i = 0; i < n; i++) {
        const float *s = im->p + i * 4;
        float *d = o + i * 4;
        float a = s[3];
        d[3] = a;
        if (a <= 0.0f) continue;
        for (int k = 0; k < 3; k++) {
            float c = clamp01(s[k] / a);
            d[k] = (lin ? srgb_to_lin(c) : lin_to_srgb(c)) * a;
        }
    }
    return o;
}

/* ---- attributes ---- */
static const char *attr(const FCtx *C, HtmlNode *n, const char *name, char *buf, size_t bn)
{
    return C->env->prop(C->env->ud, n, name, buf, bn);
}
static bool attr_num(const FCtx *C, HtmlNode *n, const char *name, double *v)
{
    char b[128];
    const char *s = attr(C, n, name, b, sizeof b);
    if (!s) return false;
    char *e;
    double d = strtod(s, &e);
    if (e == s) return false;
    *v = d;
    return true;
}
/* (one or two numbers) */
static int attr_nums2(const FCtx *C, HtmlNode *n, const char *name, double *a, double *b)
{
    char bf[128];
    const char *s = attr(C, n, name, bf, sizeof bf);
    if (!s) return 0;
    char *e;
    double d = strtod(s, &e);
    if (e == s) return -1;
    *a = d; *b = d;
    while (*e && (isspace((unsigned char)*e) || *e == ',')) e++;
    if (!*e) return 1;
    char *e2;
    double d2 = strtod(e, &e2);
    if (e2 == e) return -1;
    *b = d2;
    return 2;
}
static int attr_list(const FCtx *C, HtmlNode *n, const char *name, float *out, int cap)
{
    char bf[2048];
    const char *s = attr(C, n, name, bf, sizeof bf);
    if (!s) return -1;
    int k = 0;
    for (;;) {
        while (*s && (isspace((unsigned char)*s) || *s == ',')) s++;
        if (!*s || k >= cap) break;
        char *e;
        double d = strtod(s, &e);
        if (e == s) return -1;
        out[k++] = (float)d;
        s = e;
    }
    return k;
}
/* (a length of the filter's: number, or percent of base) */
static __thread double t_fl_fs = 16.0;   /* (the filter element's font size: its em) */
static bool frac_or_len(const char *s, double base, bool bbox_units, double *out)
{
    if (!s) return false;
    char *e;
    double d = strtod(s, &e);
    if (e == s) return false;
    if (*e == '%') { *out = d / 100.0 * base; return true; }
    /* (a length with a unit: resolved, then as any number) */
    if (!strncasecmp(e, "em", 2)) d *= t_fl_fs;
    *out = bbox_units ? d * base : d;
    return true;
}

static bool color_interp_linear(const FCtx *C, HtmlNode *n, HtmlNode *filter)
{
    char b[64];
    const char *s = attr(C, n, "color-interpolation-filters", b, sizeof b);
    if ((!s || !strcasecmp(s, "auto") || !strcasecmp(s, "inherit")) && filter) s = attr(C, filter, "color-interpolation-filters", b, sizeof b);
    return !(s && !strcasecmp(s, "sRGB"));
}

/* ---- the primitives' pieces ---- */
static void gaussian_1d(const FCtx *C, const float *src, float *dst, double sigma, bool horiz)
{
    int aw = C->aw, ah = C->ah;
    if (sigma <= 0.0) { memcpy(dst, src, (size_t)aw * ah * 4 * sizeof(float)); return; }
    int r = (int)ceil(sigma * 3.0);
    if (r > 512) r = 512;
    float *kern = (float *)malloc((size_t)(2 * r + 1) * sizeof(float));
    if (!kern) return;
    double sum = 0.0;
    for (int i = -r; i <= r; i++) { double v = exp(-0.5 * i * i / (sigma * sigma)); kern[i + r] = (float)v; sum += v; }
    for (int i = 0; i <= 2 * r; i++) kern[i] = (float)(kern[i] / sum);
    int outer = horiz ? ah : aw, inner = horiz ? aw : ah;
    size_t so = horiz ? (size_t)aw * 4 : 4, si = horiz ? 4 : (size_t)aw * 4;
    for (int a = 0; a < outer; a++) {
        const float *line = src + (size_t)a * so;
        float *out = dst + (size_t)a * so;
        for (int i = 0; i < inner; i++) {
            float c[4] = { 0, 0, 0, 0 };
            int k0 = i - r < 0 ? 0 : i - r, k1 = i + r >= inner ? inner - 1 : i + r;
            for (int k = k0; k <= k1; k++) {
                float w = kern[k - i + r];
                const float *s = line + (size_t)k * si;
                c[0] += s[0] * w; c[1] += s[1] * w; c[2] += s[2] * w; c[3] += s[3] * w;
            }
            memcpy(out + (size_t)i * si, c, sizeof c);
        }
    }
    free(kern);
}
static void gaussian(const FCtx *C, float *p, double sx, double sy)
{
    float *t = img_new(C);
    if (!t) return;
    if (sx > 0.0) { gaussian_1d(C, p, t, sx, true); memcpy(p, t, (size_t)C->aw * C->ah * 4 * sizeof(float)); }
    if (sy > 0.0) { gaussian_1d(C, p, t, sy, false); memcpy(p, t, (size_t)C->aw * C->ah * 4 * sizeof(float)); }
    free(t);
}
static void offset_img(const FCtx *C, const float *src, float *dst, double dx, double dy)
{
    int ox = (int)lround(dx), oy = (int)lround(dy);
    for (int y = 0; y < C->ah; y++)
        for (int x = 0; x < C->aw; x++) {
            int sx = x - ox, sy = y - oy;
            if (sx < 0 || sy < 0 || sx >= C->aw || sy >= C->ah) continue;
            memcpy(dst + ((size_t)y * C->aw + x) * 4, src + ((size_t)sy * C->aw + sx) * 4, 4 * sizeof(float));
        }
}
/* (out of the subregion: transparent) */
static void clip_sub(const FCtx *C, float *p, const float sr[4])
{
    for (int y = 0; y < C->ah; y++) {
        float cy = (float)(C->ay + y) + 0.5f;
        for (int x = 0; x < C->aw; x++) {
            float cx = (float)(C->ax + x) + 0.5f;
            if (cx < sr[0] || cx >= sr[2] || cy < sr[1] || cy >= sr[3]) memset(p + ((size_t)y * C->aw + x) * 4, 0, 4 * sizeof(float));
        }
    }
}

static float blend1(int m, float b, float s)
{
    switch (m) {
    case 1: return b * s;
    case 2: return b + s - b * s;
    case 3: return b <= 0.5f ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    case 4: return b < s ? b : s;
    case 5: return b > s ? b : s;
    case 6: return b <= 0 ? 0 : s >= 1 ? 1 : fminf(1, b / (1 - s));
    case 7: return b >= 1 ? 1 : s <= 0 ? 0 : 1 - fminf(1, (1 - b) / s);
    case 8: return s <= 0.5f ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    case 9: {
        if (s <= 0.5f) return b - (1 - 2 * s) * b * (1 - b);
        float d = b <= 0.25f ? ((16 * b - 12) * b + 4) * b : sqrtf(b);
        return b + (2 * s - 1) * (d - b);
    }
    case 10: return fabsf(b - s);
    case 11: return b + s - 2 * b * s;
    }
    return s;
}
static float lum3(const float *c) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }
static float sat3(const float *c) { return fmaxf(fmaxf(c[0], c[1]), c[2]) - fminf(fminf(c[0], c[1]), c[2]); }
static void clipc(float *c)
{
    float l = lum3(c), n = fminf(fminf(c[0], c[1]), c[2]), x = fmaxf(fmaxf(c[0], c[1]), c[2]);
    if (n < 0) for (int k = 0; k < 3; k++) c[k] = l + (c[k] - l) * l / (l - n);
    if (x > 1) for (int k = 0; k < 3; k++) c[k] = l + (c[k] - l) * (1 - l) / (x - l);
}
static void setlum(const float *c, float l, float *o)
{
    float d = l - lum3(c);
    for (int k = 0; k < 3; k++) o[k] = c[k] + d;
    clipc(o);
}
static void setsat(const float *c, float s, float *o)
{
    float mx = fmaxf(fmaxf(c[0], c[1]), c[2]), mn = fminf(fminf(c[0], c[1]), c[2]);
    for (int k = 0; k < 3; k++) o[k] = mx > mn ? (c[k] - mn) * s / (mx - mn) : 0.0f;
}
static void blend_px(int m, const float *cb, const float *cs, float *o)
{
    float t[3];
    if (m == 12) { setsat(cs, sat3(cb), t); setlum(t, lum3(cb), o); return; }
    if (m == 13) { setsat(cb, sat3(cs), t); setlum(t, lum3(cb), o); return; }
    if (m == 14) { setlum(cs, lum3(cb), o); return; }
    if (m == 15) { setlum(cb, lum3(cs), o); return; }
    for (int k = 0; k < 3; k++) o[k] = blend1(m, cb[k], cs[k]);
}
static int blend_mode_of(const char *s)
{
    static const char *const nm[] = { "normal", "multiply", "screen", "overlay", "darken", "lighten", "color-dodge", "color-burn",
                                      "hard-light", "soft-light", "difference", "exclusion", "hue", "saturation", "color", "luminosity" };
    if (s) for (int k = 0; k < 16; k++) if (!strcasecmp(s, nm[k])) return k;
    return 0;
}

static void color_matrix(const FCtx *C, const float *src, float *dst, const float *m)
{
    size_t n = (size_t)C->aw * C->ah;
    for (size_t i = 0; i < n; i++) {
        const float *s = src + i * 4;
        float a = s[3], c[3] = { 0, 0, 0 };
        if (a > 0.0f) for (int k = 0; k < 3; k++) c[k] = clamp01(s[k] / a);
        float o[4];
        for (int r = 0; r < 4; r++) o[r] = clamp01(m[r * 5] * c[0] + m[r * 5 + 1] * c[1] + m[r * 5 + 2] * c[2] + m[r * 5 + 3] * a + m[r * 5 + 4]);
        float *d = dst + i * 4;
        d[0] = o[0] * o[3]; d[1] = o[1] * o[3]; d[2] = o[2] * o[3]; d[3] = o[3];
    }
}
static void hue_matrix(float deg, float *m)
{
    float r = deg * 3.14159265f / 180.0f, cs = cosf(r), sn = sinf(r);
    float v[20] = { 0.213f + cs * 0.787f - sn * 0.213f, 0.715f - cs * 0.715f - sn * 0.715f, 0.072f - cs * 0.072f + sn * 0.928f, 0, 0,
                    0.213f - cs * 0.213f + sn * 0.143f, 0.715f + cs * 0.285f + sn * 0.140f, 0.072f - cs * 0.072f - sn * 0.283f, 0, 0,
                    0.213f - cs * 0.213f - sn * 0.787f, 0.715f - cs * 0.715f + sn * 0.715f, 0.072f + cs * 0.928f + sn * 0.072f, 0, 0,
                    0, 0, 0, 1, 0 };
    memcpy(m, v, sizeof v);
}
static void sat_matrix(float s, float *m)
{
    float v[20] = { 0.213f + 0.787f * s, 0.715f - 0.715f * s, 0.072f - 0.072f * s, 0, 0,
                    0.213f - 0.213f * s, 0.715f + 0.285f * s, 0.072f - 0.072f * s, 0, 0,
                    0.213f - 0.213f * s, 0.715f - 0.715f * s, 0.072f + 0.928f * s, 0, 0,
                    0, 0, 0, 1, 0 };
    memcpy(m, v, sizeof v);
}

/* (feComponentTransfer: a channel's function as a 1024 entry table) */
static void transfer_table(const FCtx *C, HtmlNode *fn, float *lut)
{
    char b[64];
    const char *ty = attr(C, fn, "type", b, sizeof b);
    int kind = !ty ? 0 : !strcasecmp(ty, "table") ? 1 : !strcasecmp(ty, "discrete") ? 2 : !strcasecmp(ty, "linear") ? 3 : !strcasecmp(ty, "gamma") ? 4 : 0;
    float tv[256];
    int tn = attr_list(C, fn, "tableValues", tv, 256);
    double slope = 1, inter = 0, amp = 1, exp_ = 1, off = 0;
    attr_num(C, fn, "slope", &slope); attr_num(C, fn, "intercept", &inter);
    attr_num(C, fn, "amplitude", &amp); attr_num(C, fn, "exponent", &exp_); attr_num(C, fn, "offset", &off);
    for (int i = 0; i < 1024; i++) {
        float c = (float)i / 1023.0f, o = c;
        if (kind == 1 && tn > 0) {
            if (tn == 1) o = tv[0];
            else {
                float f = c * (tn - 1);
                int k = (int)f;
                if (k >= tn - 1) o = tv[tn - 1];
                else o = tv[k] + (f - k) * (tv[k + 1] - tv[k]);
            }
        } else if (kind == 2 && tn > 0) {
            int k = (int)(c * tn);
            if (k >= tn) k = tn - 1;
            o = tv[k];
        } else if (kind == 3) o = (float)(slope * c + inter);
        else if (kind == 4) o = (float)(amp * pow(c, exp_) + off);
        lut[i] = clamp01(o);
    }
}

/* (Perlin noise, the specification's reference code) */
#define T_BSIZE 0x100
#define T_BM 0xff
#define T_PERLIN_N 0x1000
typedef struct { int lat[T_BSIZE + T_BSIZE + 2]; double grad[4][T_BSIZE + T_BSIZE + 2][2]; } TurbT;
static long t_random(long seed)
{
    long result = 16807 * (seed % 127773) - 2836 * (seed / 127773);
    if (result <= 0) result += 2147483647;
    return result;
}
static void t_init(TurbT *T, long seed)
{
    double s;
    int i, j, k;
    if (seed <= 0) seed = -(seed % (2147483647 - 1)) + 1;
    if (seed > 2147483646) seed = 2147483646;
    for (k = 0; k < 4; k++)
        for (i = 0; i < T_BSIZE; i++) {
            T->lat[i] = i;
            for (j = 0; j < 2; j++) {
                seed = t_random(seed);
                T->grad[k][i][j] = (double)(((seed) % (T_BSIZE + T_BSIZE)) - T_BSIZE) / T_BSIZE;
            }
            s = sqrt(T->grad[k][i][0] * T->grad[k][i][0] + T->grad[k][i][1] * T->grad[k][i][1]);
            T->grad[k][i][0] /= s;
            T->grad[k][i][1] /= s;
        }
    while (--i) {
        k = T->lat[i];
        seed = t_random(seed);
        T->lat[i] = T->lat[j = seed % T_BSIZE];
        T->lat[j] = k;
    }
    for (i = 0; i < T_BSIZE + 2; i++) {
        T->lat[T_BSIZE + i] = T->lat[i];
        for (k = 0; k < 4; k++)
            for (j = 0; j < 2; j++) T->grad[k][T_BSIZE + i][j] = T->grad[k][i][j];
    }
}
typedef struct { int w, h, wrapx, wrapy; } TStitch;
static double t_noise2(const TurbT *T, int ch, double x, double y, const TStitch *st)
{
    int bx0, bx1, by0, by1, b00, b10, b01, b11;
    double rx0, rx1, ry0, ry1, sx, sy, a, b, u, v, t;
    const double (*q)[2] = T->grad[ch];
    t = x + T_PERLIN_N; bx0 = (int)t; bx1 = bx0 + 1; rx0 = t - (int)t; rx1 = rx0 - 1.0;
    t = y + T_PERLIN_N; by0 = (int)t; by1 = by0 + 1; ry0 = t - (int)t; ry1 = ry0 - 1.0;
    if (st) {
        if (bx0 >= st->wrapx) bx0 -= st->w;
        if (bx1 >= st->wrapx) bx1 -= st->w;
        if (by0 >= st->wrapy) by0 -= st->h;
        if (by1 >= st->wrapy) by1 -= st->h;
    }
    bx0 &= T_BM; bx1 &= T_BM; by0 &= T_BM; by1 &= T_BM;
    int i = T->lat[bx0], j = T->lat[bx1];
    b00 = T->lat[i + by0]; b10 = T->lat[j + by0]; b01 = T->lat[i + by1]; b11 = T->lat[j + by1];
    sx = rx0 * rx0 * (3.0 - 2.0 * rx0);
    sy = ry0 * ry0 * (3.0 - 2.0 * ry0);
#define T_LERP(t, a, b) ((a) + (t) * ((b) - (a)))
    u = rx0 * q[b00][0] + ry0 * q[b00][1];
    v = rx1 * q[b10][0] + ry0 * q[b10][1];
    a = T_LERP(sx, u, v);
    u = rx0 * q[b01][0] + ry1 * q[b01][1];
    v = rx1 * q[b11][0] + ry1 * q[b11][1];
    b = T_LERP(sx, u, v);
    return T_LERP(sy, a, b);
}
static double t_turb(const TurbT *T, int ch, double px, double py, double fx, double fy, int oct, bool fractal, bool stitch,
                     double tx, double ty, double tw, double th)
{
    TStitch sd, *st = NULL;
    if (stitch) {
        if (fx != 0.0) {
            double lo = floor(tw * fx) / tw, hi = ceil(tw * fx) / tw;
            fx = fx / lo < hi / fx ? lo : hi;
        }
        if (fy != 0.0) {
            double lo = floor(th * fy) / th, hi = ceil(th * fy) / th;
            fy = fy / lo < hi / fy ? lo : hi;
        }
        st = &sd;
        sd.w = (int)(tw * fx + 0.5); sd.wrapx = (int)(tx * fx + T_PERLIN_N + sd.w);
        sd.h = (int)(th * fy + 0.5); sd.wrapy = (int)(ty * fy + T_PERLIN_N + sd.h);
    }
    double sum = 0.0, vx = px * fx, vy = py * fy, ratio = 1.0;
    for (int o = 0; o < oct; o++) {
        double n = t_noise2(T, ch, vx, vy, st);
        sum += (fractal ? n : fabs(n)) / ratio;
        vx *= 2; vy *= 2; ratio *= 2;
        if (st) {
            sd.w *= 2; sd.wrapx = 2 * sd.wrapx - T_PERLIN_N;
            sd.h *= 2; sd.wrapy = 2 * sd.wrapy - T_PERLIN_N;
        }
    }
    return sum;
}

/* ---- the graph ---- */
#define FMAX_NAMED 128
typedef struct { const char *name; FImg im; } FNamed;

static __thread bool t_flood_taint;
static void flood_of(const FCtx *C, HtmlNode *n, const char *cprop, const char *oprop, bool lin, float *out)
{
    char b[128];
    double rgba[4] = { 0, 0, 0, 1 };
    const char *cs = attr(C, n, cprop, b, sizeof b);
    /* (currentcolor: the element's color, the box's without one; marks it tainted) */
    bool cc_in = false;
    if (cs) {
        size_t nl = strlen(cs);
        for (size_t q = 0; q + 12 <= nl && !cc_in; q++) cc_in = !strncasecmp(cs + q, "currentcolor", 12);
    }
    if (cc_in) t_flood_taint = true;
    if (cs && !strcasecmp(cs, "currentcolor")) {
        char c2[128];
        const char *own = attr(C, n, "color", c2, sizeof c2);
        memcpy(rgba, C->env->current, sizeof rgba);
        if (own && C->env->color) C->env->color(C->env->ud, own, rgba);
        t_flood_taint = true;
    } else if (cs && C->env->color) C->env->color(C->env->ud, cs, rgba);
    double op = 1.0;
    {
        char ob[64];
        const char *os = attr(C, n, oprop, ob, sizeof ob);
        if (os) { char *e; double d = strtod(os, &e); if (e != os) op = *e == '%' ? d / 100.0 : d; }
    }
    float a = clamp01((float)(rgba[3] * op));
    for (int k = 0; k < 3; k++) {
        float c = clamp01((float)rgba[k]);
        if (lin) c = srgb_to_lin(c);
        out[k] = c * a;
    }
    out[3] = a;
}

static void lighting(const FCtx *C, HtmlNode *n, const float *in, float *dst, bool specular, const float *lightc)
{
    double surf = 1, kd = 1, ks = 1, spec = 1;
    attr_num(C, n, "surfaceScale", &surf);
    attr_num(C, n, "diffuseConstant", &kd);
    attr_num(C, n, "specularConstant", &ks);
    attr_num(C, n, "specularExponent", &spec);
    HtmlNode *lt = NULL;
    for (HtmlNode *k = html_node_first_child(C->env->doc, n); k; k = html_node_next_sibling(C->env->doc, k))
        if (k->tag_name && (!strcasecmp(k->tag_name, "feDistantLight") || !strcasecmp(k->tag_name, "fePointLight") || !strcasecmp(k->tag_name, "feSpotLight"))) { lt = k; break; }
    if (!lt) {
        /* (no light: black, opaque or not by kind) */
        size_t nn = (size_t)C->aw * C->ah;
        for (size_t i = 0; i < nn; i++) { dst[i * 4 + 3] = specular ? 0.0f : 1.0f; }
        return;
    }
    int lk = !strcasecmp(lt->tag_name, "feDistantLight") ? 0 : !strcasecmp(lt->tag_name, "fePointLight") ? 1 : 2;
    double az = 0, el = 0, lx = 0, ly = 0, lz = 0, px = 0, py = 0, pz = 0, spx = 1, cone = -1;
    attr_num(C, lt, "azimuth", &az); attr_num(C, lt, "elevation", &el);
    attr_num(C, lt, "x", &lx); attr_num(C, lt, "y", &ly); attr_num(C, lt, "z", &lz);
    attr_num(C, lt, "pointsAtX", &px); attr_num(C, lt, "pointsAtY", &py); attr_num(C, lt, "pointsAtZ", &pz);
    attr_num(C, lt, "specularExponent", &spx);
    bool has_cone = attr_num(C, lt, "limitingConeAngle", &cone);
    double dl[3] = { cos(az * M_PI / 180) * cos(el * M_PI / 180), sin(az * M_PI / 180) * cos(el * M_PI / 180), sin(el * M_PI / 180) };
    /* (user space: from the box's corner) */
    if (!C->env->user_zero) { lx += C->box[0]; ly += C->box[1]; px += C->box[0]; py += C->box[1]; }
    int W = C->aw, H = C->ah;
#define AT(xx, yy) (in[((size_t)(yy) * W + (xx)) * 4 + 3])
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int xl = x > 0 ? x - 1 : x, xr = x + 1 < W ? x + 1 : x, yt = y > 0 ? y - 1 : y, yb = y + 1 < H ? y + 1 : y;
            /* (Sobel, with the edge pixels' factors by how far it reached) */
            float nx = (AT(xr, yt) + 2 * AT(xr, y) + AT(xr, yb) - AT(xl, yt) - 2 * AT(xl, y) - AT(xl, yb)) * (x > 0 && x + 1 < W ? 0.25f : 0.5f);
            float ny = (AT(xl, yb) + 2 * AT(x, yb) + AT(xr, yb) - AT(xl, yt) - 2 * AT(x, yt) - AT(xr, yt)) * (y > 0 && y + 1 < H ? 0.25f : 0.5f);
            double Nv[3] = { -surf * nx, -surf * ny, 1.0 }, ln = sqrt(Nv[0] * Nv[0] + Nv[1] * Nv[1] + 1.0);
            Nv[0] /= ln; Nv[1] /= ln; Nv[2] /= ln;
            double L[3];
            double pz0 = surf * AT(x, y);
            if (lk == 0) { L[0] = dl[0]; L[1] = dl[1]; L[2] = dl[2]; }
            else {
                L[0] = lx - (C->ax + x); L[1] = ly - (C->ay + y); L[2] = lz - pz0;
                double l2 = sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
                if (l2 > 0) { L[0] /= l2; L[1] /= l2; L[2] /= l2; }
            }
            float lc[3] = { lightc[0], lightc[1], lightc[2] };
            if (lk == 2) {
                double S[3] = { px - lx, py - ly, pz - lz }, sl = sqrt(S[0] * S[0] + S[1] * S[1] + S[2] * S[2]);
                if (sl > 0) { S[0] /= sl; S[1] /= sl; S[2] /= sl; }
                double minus = -(L[0] * S[0] + L[1] * S[1] + L[2] * S[2]);
                if (minus <= 0.0 || (has_cone && minus < cos(cone * M_PI / 180))) { lc[0] = lc[1] = lc[2] = 0; }
                else { double f = pow(minus, spx); lc[0] *= (float)f; lc[1] *= (float)f; lc[2] *= (float)f; }
            }
            float *d = dst + ((size_t)y * W + x) * 4;
            if (!specular) {
                double nl = Nv[0] * L[0] + Nv[1] * L[1] + Nv[2] * L[2];
                if (nl < 0) nl = 0;
                for (int k = 0; k < 3; k++) d[k] = clamp01((float)(kd * nl * lc[k]));
                d[3] = 1.0f;
            } else {
                double Hv[3] = { L[0], L[1], L[2] + 1.0 }, hl = sqrt(Hv[0] * Hv[0] + Hv[1] * Hv[1] + Hv[2] * Hv[2]);
                double nh = hl > 0 ? (Nv[0] * Hv[0] + Nv[1] * Hv[1] + Nv[2] * Hv[2]) / hl : 0.0;
                if (nh < 0) nh = 0;
                double f = ks * pow(nh, spec);
                float mx = 0.0f;
                float o[3];
                for (int k = 0; k < 3; k++) { o[k] = clamp01((float)(f * lc[k])); if (o[k] > mx) mx = o[k]; }
                for (int k = 0; k < 3; k++) d[k] = o[k];   /* (premultiplied by its max) */
                d[3] = mx;
            }
        }
#undef AT
}

/* The filter element's graph on input `src` (its result in `*out`, new; false: no such filter, as if none). */
static bool run_filter(FCtx *C0, HtmlNode *flt, const FImg *src, FImg *out)
{
    FCtx CC = *C0, *C = &CC;
    const SvgFilterEnv *env = C->env;
    char b1[64], b2[64];
    const char *fu = attr(C, flt, "filterUnits", b1, sizeof b1), *pu = attr(C, flt, "primitiveUnits", b2, sizeof b2);
    C->prim_bbox = pu && !strcasecmp(pu, "objectBoundingBox");
    /* (region was found by svg_filter_region: the same area, here its numbers) */
    svg_filter_region(env, flt, C->box, C->region);
    (void)fu;
    size_t N = (size_t)C->aw * C->ah;
    FNamed named[FMAX_NAMED];
    int nnamed = 0;
    FImg prev = { 0 };
    bool have_prev = false;
    FImg alpha_src = { 0 };
    bool any = false;
    FImg to_free[FMAX_NAMED + 8];
    int nfree = 0;
    HtmlDocument *doc = env->doc;
#define IN_OF(attrname, var)                                                                                            \
    FImg var = have_prev ? prev : *src;                                                                                 \
    {                                                                                                                   \
        char inb[128];                                                                                                  \
        const char *ins = attr(C, n, attrname, inb, sizeof inb);                                                        \
        if (ins) {                                                                                                      \
            if (!strcmp(ins, "SourceGraphic")) var = *src;                                                              \
            else if (!strcmp(ins, "SourceAlpha")) {                                                                     \
                if (!alpha_src.p) {                                                                                     \
                    alpha_src.p = img_new(C); alpha_src.lin = src->lin;                                                 \
                    memcpy(alpha_src.sr, C->region, sizeof C->region);                                                  \
                    if (alpha_src.p) for (size_t q = 0; q < N; q++) alpha_src.p[q * 4 + 3] = src->p[q * 4 + 3];         \
                    to_free[nfree++] = alpha_src;                                                                       \
                }                                                                                                       \
                var = alpha_src;                                                                                        \
            } else if (!strcmp(ins, "BackgroundImage") || !strcmp(ins, "BackgroundAlpha") || !strcmp(ins, "FillPaint") || !strcmp(ins, "StrokePaint")) { \
                FImg ev = { img_new(C), false, true, { 0 } };                                                           \
                memcpy(ev.sr, C->region, sizeof C->region); ev.taint = true;                                            \
                to_free[nfree++] = ev;                                                                                  \
                var = ev;                                                                                               \
            } else {                                                                                                    \
                for (int q = nnamed - 1; q >= 0; q--)                                                                   \
                    if (!strcmp(named[q].name, ins)) { var = named[q].im; break; }                                      \
            }                                                                                                           \
        }                                                                                                               \
    }                                                                                                                   \
    prim_taint = prim_taint || var.taint;
    for (HtmlNode *n = html_node_first_child(doc, flt); n && nfree < FMAX_NAMED; n = html_node_next_sibling(doc, n)) {
        if (n->type != HTML_NODE_ELEMENT || !n->tag_name) continue;
        const char *tag = n->tag_name;
        if (strncasecmp(tag, "fe", 2)) continue;
        bool lin = color_interp_linear(C, n, flt);
        float *d = img_new(C);
        if (!d) break;
        /* the primitive subregion: 0 0 100% 100% of the region when not given */
        float sr[4] = { C->region[0], C->region[1], C->region[2], C->region[3] };
        {
            char xb[64];
            double bx = C->prim_bbox || !env->user_zero ? C->box[0] : 0.0, by = C->prim_bbox || !env->user_zero ? C->box[1] : 0.0, bw = C->box[2], bh = C->box[3], v;
            const char *a;
            double fw = C->region[2] - C->region[0], fh = C->region[3] - C->region[1];
            a = attr(C, n, "x", xb, sizeof xb);
            if (a && frac_or_len(a, C->prim_bbox ? bw : fw, C->prim_bbox, &v)) sr[0] = (float)(C->prim_bbox || !strchr(a, '%') ? bx + v : C->region[0] + v);
            double x0set = sr[0];
            a = attr(C, n, "y", xb, sizeof xb);
            if (a && frac_or_len(a, C->prim_bbox ? bh : fh, C->prim_bbox, &v)) sr[1] = (float)(C->prim_bbox || !strchr(a, '%') ? by + v : C->region[1] + v);
            double y0set = sr[1];
            a = attr(C, n, "width", xb, sizeof xb);
            if (a && frac_or_len(a, C->prim_bbox ? bw : fw, C->prim_bbox, &v)) sr[2] = (float)(x0set + v);
            a = attr(C, n, "height", xb, sizeof xb);
            if (a && frac_or_len(a, C->prim_bbox ? bh : fh, C->prim_bbox, &v)) sr[3] = (float)(y0set + v);
        }
        double sb = C->prim_bbox ? 1.0 : 0.0;
        double uw = sb ? C->box[2] : 1.0, uh = sb ? C->box[3] : 1.0;
        bool lin_out = lin, prim_taint = false;
        t_flood_taint = false;
        if (!strcasecmp(tag, "feFlood")) {
            float c[4];
            flood_of(C, n, "flood-color", "flood-opacity", lin, c);
            for (size_t i = 0; i < N; i++) memcpy(d + i * 4, c, sizeof c);
        } else if (!strcasecmp(tag, "feOffset")) {
            IN_OF("in", I);
            double dx = 0, dy = 0;
            attr_num(C, n, "dx", &dx); attr_num(C, n, "dy", &dy);
            offset_img(C, I.p, d, dx * uw, dy * uh);
            lin_out = I.lin;
        } else if (!strcasecmp(tag, "feGaussianBlur")) {
            IN_OF("in", I);
            bool own;
            float *ip = img_in_space(C, &I, lin, &own);
            if (ip) {
                memcpy(d, ip, N * 4 * sizeof(float));
                double sx = 0, sy = 0;
                int got = attr_nums2(C, n, "stdDeviation", &sx, &sy);
                if (got >= 0 && sx >= 0 && sy >= 0 && (got > 0)) gaussian(C, d, sx * uw, sy * uh);
                else if (got < 0 || sx < 0 || sy < 0) memset(d, 0, N * 4 * sizeof(float));
                if (own) free(ip);
            }
        } else if (!strcasecmp(tag, "feColorMatrix")) {
            IN_OF("in", I);
            bool own;
            float *ip = img_in_space(C, &I, lin, &own);
            char tb[64];
            const char *ty = attr(C, n, "type", tb, sizeof tb);
            float m[20] = { 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0 };
            float vals[20];
            int nv = attr_list(C, n, "values", vals, 20);
            if (ty && !strcasecmp(ty, "saturate")) sat_matrix(nv >= 1 ? vals[0] : 1.0f, m);
            else if (ty && !strcasecmp(ty, "hueRotate")) hue_matrix(nv >= 1 ? vals[0] : 0.0f, m);
            else if (ty && !strcasecmp(ty, "luminanceToAlpha")) {
                float v[20] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.2125f, 0.7154f, 0.0721f, 0, 0 };
                memcpy(m, v, sizeof v);
            } else if (nv == 20) memcpy(m, vals, sizeof m);
            if (ip) { color_matrix(C, ip, d, m); if (own) free(ip); }
        } else if (!strcasecmp(tag, "feComponentTransfer")) {
            IN_OF("in", I);
            bool own;
            float *ip = img_in_space(C, &I, lin, &own);
            static __thread float lut[4][1024];
            for (int k = 0; k < 4; k++) for (int i = 0; i < 1024; i++) lut[k][i] = (float)i / 1023.0f;
            for (HtmlNode *f = html_node_first_child(doc, n); f; f = html_node_next_sibling(doc, f)) {
                if (!f->tag_name) continue;
                int ch = !strcasecmp(f->tag_name, "feFuncR") ? 0 : !strcasecmp(f->tag_name, "feFuncG") ? 1 : !strcasecmp(f->tag_name, "feFuncB") ? 2 : !strcasecmp(f->tag_name, "feFuncA") ? 3 : -1;
                if (ch >= 0) transfer_table(C, f, lut[ch]);
            }
            if (ip) {
                for (size_t i = 0; i < N; i++) {
                    const float *s = ip + i * 4;
                    float a = s[3], c[3] = { 0, 0, 0 };
                    if (a > 0.0f) for (int k = 0; k < 3; k++) c[k] = clamp01(s[k] / a);
                    float na = lut[3][(int)(a * 1023.0f + 0.5f)];
                    for (int k = 0; k < 3; k++) d[i * 4 + k] = lut[k][(int)(c[k] * 1023.0f + 0.5f)] * na;
                    d[i * 4 + 3] = na;
                }
                if (own) free(ip);
            }
        } else if (!strcasecmp(tag, "feBlend") || !strcasecmp(tag, "feComposite")) {
            IN_OF("in", I1);
            IN_OF("in2", I2);
            bool o1, o2;
            float *a = img_in_space(C, &I1, lin, &o1), *b = img_in_space(C, &I2, lin, &o2);
            char mb[64];
            bool is_blend = !strcasecmp(tag, "feBlend");
            int mode = is_blend ? blend_mode_of(attr(C, n, "mode", mb, sizeof mb)) : 0;
            const char *ops = is_blend ? NULL : attr(C, n, "operator", mb, sizeof mb);
            int op = !ops ? 0 : !strcasecmp(ops, "in") ? 1 : !strcasecmp(ops, "out") ? 2 : !strcasecmp(ops, "atop") ? 3 : !strcasecmp(ops, "xor") ? 4 :
                     !strcasecmp(ops, "lighter") ? 5 : !strcasecmp(ops, "arithmetic") ? 6 : 0;
            double k1 = 0, k2 = 0, k3 = 0, k4 = 0;
            if (op == 6) { attr_num(C, n, "k1", &k1); attr_num(C, n, "k2", &k2); attr_num(C, n, "k3", &k3); attr_num(C, n, "k4", &k4); }
            if (a && b) for (size_t i = 0; i < N; i++) {
                const float *s = a + i * 4, *t = b + i * 4;
                float *o = d + i * 4;
                if (is_blend) {
                    float as = s[3], ab = t[3];
                    float cs[3] = { 0, 0, 0 }, cb[3] = { 0, 0, 0 }, mix[3];
                    if (as > 0) for (int k = 0; k < 3; k++) cs[k] = s[k] / as;
                    if (ab > 0) for (int k = 0; k < 3; k++) cb[k] = t[k] / ab;
                    blend_px(mode, cb, cs, mix);
                    for (int k = 0; k < 3; k++) o[k] = clamp01((1 - ab) * s[k] + (1 - as) * t[k] + as * ab * (mode == 0 ? cs[k] : mix[k]));
                    o[3] = as + ab - as * ab;
                    continue;
                }
                float fa, fb;
                switch (op) {
                case 1: fa = t[3]; fb = 0; break;
                case 2: fa = 1 - t[3]; fb = 0; break;
                case 3: fa = t[3]; fb = 1 - s[3]; break;
                case 4: fa = 1 - t[3]; fb = 1 - s[3]; break;
                case 5: fa = 1; fb = 1; break;
                case 6: fa = fb = 0; break;
                default: fa = 1; fb = 1 - s[3]; break;
                }
                if (op == 6) {
                    float oa = clamp01((float)(k1 * s[3] * t[3] + k2 * s[3] + k3 * t[3] + k4));
                    o[3] = oa;
                    for (int k = 0; k < 3; k++) {
                        float v = clamp01((float)(k1 * s[k] * t[k] + k2 * s[k] + k3 * t[k] + k4));
                        o[k] = v < oa ? v : oa;
                    }
                } else {
                    for (int k = 0; k < 4; k++) o[k] = clamp01(s[k] * fa + t[k] * fb);
                }
            }
            if (o1) free(a);
            if (o2) free(b);
        } else if (!strcasecmp(tag, "feMerge")) {
            for (HtmlNode *mn = html_node_first_child(doc, n); mn; mn = html_node_next_sibling(doc, mn)) {
                if (!mn->tag_name || strcasecmp(mn->tag_name, "feMergeNode")) continue;
                HtmlNode *keep = n;
                n = mn;
                IN_OF("in", I);
                n = keep;
                bool own;
                float *ip = img_in_space(C, &I, lin, &own);
                if (ip) for (size_t i = 0; i < N; i++) {
                    float ia = 1 - ip[i * 4 + 3];
                    for (int k = 0; k < 4; k++) d[i * 4 + k] = ip[i * 4 + k] + d[i * 4 + k] * ia;
                }
                if (own) free(ip);
            }
        } else if (!strcasecmp(tag, "feMorphology")) {
            IN_OF("in", I);
            bool own;
            float *ip = img_in_space(C, &I, lin, &own);
            double rx = 0, ry = 0;
            int got = attr_nums2(C, n, "radius", &rx, &ry);
            char ob[32];
            const char *ops = attr(C, n, "operator", ob, sizeof ob);
            bool dil = ops && !strcasecmp(ops, "dilate");
            int rxi = (int)(rx * uw + 0.5), ryi = (int)(ry * uh + 0.5);
            /* (0 or negative: disabled, transparent... a zero radius is the input itself in browsers) */
            if (ip && got >= 0 && rx >= 0 && ry >= 0 && (rxi > 0 || ryi > 0)) {
                float *tmp = img_new(C);
                if (tmp) {
                    for (int y = 0; y < C->ah; y++)
                        for (int x = 0; x < C->aw; x++)
                            for (int k = 0; k < 4; k++) {
                                float v = dil ? 0.0f : 1.0f;
                                for (int q = x - rxi; q <= x + rxi; q++) {
                                    float s = q >= 0 && q < C->aw ? ip[((size_t)y * C->aw + q) * 4 + k] : 0.0f;
                                    v = dil ? fmaxf(v, s) : fminf(v, s);
                                }
                                tmp[((size_t)y * C->aw + x) * 4 + k] = v;
                            }
                    for (int y = 0; y < C->ah; y++)
                        for (int x = 0; x < C->aw; x++)
                            for (int k = 0; k < 4; k++) {
                                float v = dil ? 0.0f : 1.0f;
                                for (int q = y - ryi; q <= y + ryi; q++) {
                                    float s = q >= 0 && q < C->ah ? tmp[((size_t)q * C->aw + x) * 4 + k] : 0.0f;
                                    v = dil ? fmaxf(v, s) : fminf(v, s);
                                }
                                d[((size_t)y * C->aw + x) * 4 + k] = v;
                            }
                    free(tmp);
                }
            } else if (ip && got >= 0 && rx >= 0 && ry >= 0) memcpy(d, ip, N * 4 * sizeof(float));
            if (own) free(ip);
        } else if (!strcasecmp(tag, "feTile")) {
            IN_OF("in", I);
            float tx0 = I.sr[0], ty0 = I.sr[1], tw = I.sr[2] - I.sr[0], th = I.sr[3] - I.sr[1];
            lin_out = I.lin;
            if (tw >= 1 && th >= 1)
                for (int y = 0; y < C->ah; y++)
                    for (int x = 0; x < C->aw; x++) {
                        float px = (float)(C->ax + x) - tx0, py = (float)(C->ay + y) - ty0;
                        px -= floorf(px / tw) * tw;
                        py -= floorf(py / th) * th;
                        int sx = (int)(tx0 + px) - C->ax, sy = (int)(ty0 + py) - C->ay;
                        if (sx < 0 || sy < 0 || sx >= C->aw || sy >= C->ah) continue;
                        memcpy(d + ((size_t)y * C->aw + x) * 4, I.p + ((size_t)sy * C->aw + sx) * 4, 4 * sizeof(float));
                    }
        } else if (!strcasecmp(tag, "feDropShadow")) {
            IN_OF("in", I);
            bool own;
            float *ip = img_in_space(C, &I, lin, &own);
            double dx = 2, dy = 2, sx = 2, sy = 2;
            attr_num(C, n, "dx", &dx); attr_num(C, n, "dy", &dy);
            attr_nums2(C, n, "stdDeviation", &sx, &sy);
            float c[4];
            flood_of(C, n, "flood-color", "flood-opacity", lin, c);
            float *sh = img_new(C);
            if (ip && sh) {
                offset_img(C, ip, sh, dx * uw, dy * uh);
                for (size_t i = 0; i < N; i++) { float a = sh[i * 4 + 3]; sh[i * 4] = sh[i * 4 + 1] = sh[i * 4 + 2] = 0; sh[i * 4 + 3] = a; }
                gaussian(C, sh, sx * uw, sy * uh);
                for (size_t i = 0; i < N; i++) {
                    float a = sh[i * 4 + 3], ia = 1 - ip[i * 4 + 3];
                    for (int k = 0; k < 4; k++) d[i * 4 + k] = ip[i * 4 + k] + c[k] * a * ia;
                }
            }
            free(sh);
            if (own) free(ip);
        } else if (!strcasecmp(tag, "feDisplacementMap")) {
            IN_OF("in", I1);
            IN_OF("in2", I2);
            bool o1, o2;
            float *a = img_in_space(C, &I1, lin, &o1), *m = img_in_space(C, &I2, lin, &o2);
            double scale = 0;
            attr_num(C, n, "scale", &scale);
            char cb[16];
            const char *xs = attr(C, n, "xChannelSelector", cb, sizeof cb);
            int xc = !xs ? 3 : *xs == 'R' ? 0 : *xs == 'G' ? 1 : *xs == 'B' ? 2 : 3;
            const char *ys = attr(C, n, "yChannelSelector", cb, sizeof cb);
            int yc = !ys ? 3 : *ys == 'R' ? 0 : *ys == 'G' ? 1 : *ys == 'B' ? 2 : 3;
            if (a && I2.taint) memcpy(d, a, N * 4 * sizeof(float));   /* (a tainted map: no displacement) */
            else if (a && m)
                for (int y = 0; y < C->ah; y++)
                    for (int x = 0; x < C->aw; x++) {
                        const float *mp = m + ((size_t)y * C->aw + x) * 4;
                        float ma = mp[3];
                        /* (the map's colors unpremultiplied; alpha as it is) */
                        float mx = xc == 3 ? ma : ma > 0 ? mp[xc] / ma : 0, my = yc == 3 ? ma : ma > 0 ? mp[yc] / ma : 0;
                        int sx = (int)floor(x + scale * uw * (mx - 0.5) + 0.5), sy = (int)floor(y + scale * uh * (my - 0.5) + 0.5);
                        if (sx < 0 || sy < 0 || sx >= C->aw || sy >= C->ah) continue;
                        memcpy(d + ((size_t)y * C->aw + x) * 4, a + ((size_t)sy * C->aw + sx) * 4, 4 * sizeof(float));
                    }
            if (o1) free(a);
            if (o2) free(m);
        } else if (!strcasecmp(tag, "feConvolveMatrix")) {
            IN_OF("in", I);
            bool own;
            float *ip = img_in_space(C, &I, lin, &own);
            double ox = 3, oy = 3;
            attr_nums2(C, n, "order", &ox, &oy);
            int ow = (int)ox, oh = (int)oy;
            float km[256];
            int kn = attr_list(C, n, "kernelMatrix", km, 256);
            double div = 0, bias = 0, tx = ow / 2, ty = oh / 2;
            bool hasdiv = attr_num(C, n, "divisor", &div);
            attr_num(C, n, "bias", &bias);
            attr_num(C, n, "targetX", &tx); attr_num(C, n, "targetY", &ty);
            char eb[32], pb[32];
            const char *em = attr(C, n, "edgeMode", eb, sizeof eb), *pa = attr(C, n, "preserveAlpha", pb, sizeof pb);
            bool preserve = pa && !strcasecmp(pa, "true");
            int edge = !em ? 0 : !strcasecmp(em, "wrap") ? 1 : !strcasecmp(em, "none") ? 2 : 0;
            if (!hasdiv || div == 0) { div = 0; for (int k = 0; k < kn; k++) div += km[k]; if (div == 0) div = 1; }
            if (ip && kn == ow * oh && ow > 0 && oh > 0) {
                /* (the input's edges: its subregion's pixels, what duplicate and wrap extend) */
                int ex0 = (int)ceilf(I.sr[0] - (float)C->ax - 0.5f), ey0 = (int)ceilf(I.sr[1] - (float)C->ay - 0.5f);
                int ex1 = (int)ceilf(I.sr[2] - (float)C->ax - 0.5f), ey1 = (int)ceilf(I.sr[3] - (float)C->ay - 0.5f);
                if (ex0 < 0) ex0 = 0;
                if (ey0 < 0) ey0 = 0;
                if (ex1 > C->aw) ex1 = C->aw;
                if (ey1 > C->ah) ey1 = C->ah;
                if (ex1 <= ex0 || ey1 <= ey0) { ex0 = 0; ey0 = 0; ex1 = C->aw; ey1 = C->ah; }
                int ew = ex1 - ex0, eh = ey1 - ey0;
                for (int y = 0; y < C->ah; y++)
                    for (int x = 0; x < C->aw; x++) {
                        float c[4] = { 0, 0, 0, 0 };
                        for (int j = 0; j < oh; j++)
                            for (int i = 0; i < ow; i++) {
                                int sx = x - (int)tx + i, sy = y - (int)ty + j;
                                float v[4] = { 0, 0, 0, 0 };
                                if (edge == 1) { sx = ex0 + (((sx - ex0) % ew) + ew) % ew; sy = ey0 + (((sy - ey0) % eh) + eh) % eh; }
                                else if (edge == 0) { sx = sx < ex0 ? ex0 : sx >= ex1 ? ex1 - 1 : sx; sy = sy < ey0 ? ey0 : sy >= ey1 ? ey1 - 1 : sy; }
                                if (sx >= 0 && sy >= 0 && sx < C->aw && sy < C->ah) {
                                    const float *s = ip + ((size_t)sy * C->aw + sx) * 4;
                                    memcpy(v, s, sizeof v);
                                    if (preserve && v[3] > 0) { v[0] /= v[3]; v[1] /= v[3]; v[2] /= v[3]; }
                                }
                                float w = km[(oh - 1 - j) * ow + (ow - 1 - i)];
                                for (int k = 0; k < 4; k++) c[k] += v[k] * w;
                            }
                        float *o = d + ((size_t)y * C->aw + x) * 4;
                        float sa = ip[((size_t)y * C->aw + x) * 4 + 3];
                        float oa = preserve ? sa : clamp01((float)(c[3] / div + bias));
                        o[3] = oa;
                        for (int k = 0; k < 3; k++) {
                            float v = (float)(c[k] / div + bias * (preserve ? 1.0 : oa));
                            v = preserve ? clamp01(v) * oa : (v < 0 ? 0 : v > oa ? oa : v);
                            o[k] = v;
                        }
                    }
            }
            if (own) free(ip);
        } else if (!strcasecmp(tag, "feTurbulence")) {
            double fx = 0, fy = 0, oct = 1, seed = 0;
            attr_nums2(C, n, "baseFrequency", &fx, &fy);
            attr_num(C, n, "numOctaves", &oct);
            attr_num(C, n, "seed", &seed);
            char tb[32], sb2[32];
            const char *ty = attr(C, n, "type", tb, sizeof tb), *st = attr(C, n, "stitchTiles", sb2, sizeof sb2);
            bool fractal = ty && !strcasecmp(ty, "fractalNoise"), stitch = st && !strcasecmp(st, "stitch");
            TurbT *T = (TurbT *)malloc(sizeof(TurbT));
            if (T && fx >= 0 && fy >= 0) {
                t_init(T, (long)floor(seed + 0.5));
                for (int y = 0; y < C->ah; y++)
                    for (int x = 0; x < C->aw; x++) {
                        double px = C->ax + x - C->box[0], py = C->ay + y - C->box[1];
                        float o[4];
                        for (int k = 0; k < 4; k++) {
                            double v = t_turb(T, k, px, py, fx, fy, (int)oct, fractal, stitch, sr[0] - C->box[0], sr[1] - C->box[1], sr[2] - sr[0], sr[3] - sr[1]);
                            o[k] = clamp01((float)(fractal ? (v + 1) / 2 : v));
                        }
                        float *dp = d + ((size_t)y * C->aw + x) * 4;
                        dp[0] = o[0] * o[3]; dp[1] = o[1] * o[3]; dp[2] = o[2] * o[3]; dp[3] = o[3];
                    }
            }
            free(T);
        } else if (!strcasecmp(tag, "feDiffuseLighting") || !strcasecmp(tag, "feSpecularLighting")) {
            IN_OF("in", I);
            float c[4];
            flood_of(C, n, "lighting-color", "lighting-opacity-none", lin, c);
            { char lb[64]; if (!attr(C, n, "lighting-color", lb, sizeof lb)) { c[0] = c[1] = c[2] = 1.0f; } }
            lighting(C, n, I.p, d, !strcasecmp(tag, "feSpecularLighting"), c);
        } else if (!strcasecmp(tag, "feImage")) {
            char hb[1024];
            const char *h = attr(C, n, "href", hb, sizeof hb);
            if (!h) h = attr(C, n, "xlink:href", hb, sizeof hb);
            const uint8_t *ipx = NULL;
            int iw = 0, ih = 0, ich = 0;
            if (h && h[0] == '#' && env->element) {
                /* (an element: as if used, in user space) */
                float ux = env->user_zero ? 0.0f : C->box[0], uy = env->user_zero ? 0.0f : C->box[1];
                if (env->element(env->ud, h + 1, C->aw, C->ah, &ipx, &ich)) {
                    int ox = (int)floorf(ux) - C->ax, oy = (int)floorf(uy) - C->ay;
                    for (int y = 0; y < C->ah; y++)
                        for (int x = 0; x < C->aw; x++) {
                            int sx = x - ox, sy = y - oy;
                            if (sx < 0 || sy < 0 || sx >= C->aw || sy >= C->ah) continue;
                            const uint8_t *s = ipx + ((size_t)sy * C->aw + sx) * 4;
                            float a = s[3] / 255.0f;
                            float *o = d + ((size_t)y * C->aw + x) * 4;
                            for (int k = 0; k < 3; k++) {
                                float cv = s[k] / 255.0f;
                                if (lin) cv = srgb_to_lin(cv);
                                o[k] = cv * a;
                            }
                            o[3] = a;
                        }
                    free((void *)ipx);
                }
            } else if (h && env->image && env->image(env->ud, h, &ipx, &iw, &ih, &ich) && iw > 0 && ih > 0) {
                /* (fit its subregion, preserveAspectRatio's default: xMidYMid meet) */
                char pb[64];
                const char *pa = attr(C, n, "preserveAspectRatio", pb, sizeof pb);
                bool none = pa && !strncasecmp(pa, "none", 4), slice = pa && strstr(pa, "slice");
                float rw = sr[2] - sr[0], rh = sr[3] - sr[1], sxx = rw / iw, syy = rh / ih;
                if (!none) { float s = slice ? fmaxf(sxx, syy) : fminf(sxx, syy); sxx = syy = s; }
                float dw = iw * sxx, dh = ih * syy, ox = sr[0], oy = sr[1];
                if (!none) {
                    int ax = !pa || strstr(pa, "xMid") ? 1 : strstr(pa, "xMax") ? 2 : 0, ay = !pa || strstr(pa, "YMid") ? 1 : strstr(pa, "YMax") ? 2 : 0;
                    ox += (rw - dw) * ax * 0.5f; oy += (rh - dh) * ay * 0.5f;
                }
                for (int y = 0; y < C->ah; y++)
                    for (int x = 0; x < C->aw; x++) {
                        float px = C->ax + x + 0.5f, py = C->ay + y + 0.5f;
                        if (px < ox || py < oy || px >= ox + dw || py >= oy + dh) continue;
                        int sx = (int)((px - ox) / sxx), sy = (int)((py - oy) / syy);
                        if (sx >= iw) sx = iw - 1;
                        if (sy >= ih) sy = ih - 1;
                        const uint8_t *s = ipx + ((size_t)sy * iw + sx) * ich;
                        float a = ich >= 4 ? s[3] / 255.0f : 1.0f;
                        float *o = d + ((size_t)y * C->aw + x) * 4;
                        for (int k = 0; k < 3; k++) {
                            float cv = s[ich >= 3 ? k : 0] / 255.0f;
                            if (lin) cv = srgb_to_lin(cv);
                            o[k] = cv * a;
                        }
                        o[3] = a;
                    }
            }
        } else {
            free(d);
            continue;
        }
        clip_sub(C, d, sr);
        {
            /* (in the filter's region) */
            clip_sub(C, d, C->region);
        }
        FImg res = { d, lin_out, true, { sr[0], sr[1], sr[2], sr[3] }, prim_taint || t_flood_taint };
        to_free[nfree++] = res;
        char rb[128];
        const char *rs = attr(C, n, "result", rb, sizeof rb);
        if (rs && nnamed < FMAX_NAMED) {
            /* (the name kept with the node: interned by the caller's document) */
            const HtmlAttribute *ra = NULL;
            for (const HtmlAttribute *aa = n->attributes; aa; aa = aa->next) if (!strcasecmp(aa->name, "result")) ra = aa;
            if (ra) { named[nnamed].name = ra->value; named[nnamed].im = res; nnamed++; }
        }
        prev = res;
        have_prev = true;
        any = true;
    }
#undef IN_OF
    /* (no primitives: nothing is painted; else the last result, out of the region clipped) */
    float *o = img_new(C);
    if (o) {
        if (any) {
            memcpy(o, prev.p, N * 4 * sizeof(float));
            out->lin = prev.lin;
        } else out->lin = false;
        out->p = o;
        memcpy(out->sr, C->region, sizeof C->region);
        out->owned = true;
    }
    for (int k = 0; k < nfree; k++) free(to_free[k].p);
    return o != NULL;
}

/* ---- the function list ---- */
void svg_filter_region(const SvgFilterEnv *env, HtmlNode *flt, const float box[4], float out[4])
{
    char b[64];
    FCtx C0;
    memset(&C0, 0, sizeof C0);
    C0.env = env;
    const char *fu = attr(&C0, flt, "filterUnits", b, sizeof b);
    bool user = fu && !strcasecmp(fu, "userSpaceOnUse");
    double x = -0.1, y = -0.1, w = 1.2, h = 1.2;
    if (user) {
        x = -0.1 * box[2]; y = -0.1 * box[3]; w = 1.2 * box[2]; h = 1.2 * box[3];
    } else { x *= box[2]; y *= box[3]; w *= box[2]; h *= box[3]; }
    char xb[64];
    double v;
    const char *a;
    t_fl_fs = 16.0;
    a = attr(&C0, flt, "font-size", xb, sizeof xb);
    if (a) { double f_ = strtod(a, NULL); if (f_ > 0.0) t_fl_fs = f_; }
    a = attr(&C0, flt, "x", xb, sizeof xb); if (a && frac_or_len(a, box[2], !user, &v)) x = v;
    a = attr(&C0, flt, "y", xb, sizeof xb); if (a && frac_or_len(a, box[3], !user, &v)) y = v;
    a = attr(&C0, flt, "width", xb, sizeof xb); if (a && frac_or_len(a, box[2], !user, &v)) w = v;
    a = attr(&C0, flt, "height", xb, sizeof xb); if (a && frac_or_len(a, box[3], !user, &v)) h = v;
    /* (lengths in user space: from its origin; fractions of the box from the box) */
    if (user && env->user_zero) { out[0] = (float)x; out[1] = (float)y; }
    else { out[0] = (float)(box[0] + x); out[1] = (float)(box[1] + y); }
    out[2] = out[0] + (float)w; out[3] = out[1] + (float)h;
}

static float parse_amt(const char *s, const char *e, double dflt)
{
    (void)e;
    char *end;
    double d = strtod(s, &end);
    if (end == s) return (float)dflt;
    if (*end == '%') d /= 100.0;
    return (float)d;
}
static void fn_matrix_of(const char *name, float a, float *m)
{
    float id[20] = { 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0 };
    memcpy(m, id, sizeof id);
    if (!strcmp(name, "grayscale")) {
        if (a > 1) a = 1;
        float s = 1 - a;
        float v[20] = { 0.2126f + 0.7874f * s, 0.7152f - 0.7152f * s, 0.0722f - 0.0722f * s, 0, 0,
                        0.2126f - 0.2126f * s, 0.7152f + 0.2848f * s, 0.0722f - 0.0722f * s, 0, 0,
                        0.2126f - 0.2126f * s, 0.7152f - 0.7152f * s, 0.0722f + 0.9278f * s, 0, 0,
                        0, 0, 0, 1, 0 };
        memcpy(m, v, sizeof v);
    } else if (!strcmp(name, "sepia")) {
        if (a > 1) a = 1;
        float s = 1 - a;
        float v[20] = { 0.393f + 0.607f * s, 0.769f - 0.769f * s, 0.189f - 0.189f * s, 0, 0,
                        0.349f - 0.349f * s, 0.686f + 0.314f * s, 0.168f - 0.168f * s, 0, 0,
                        0.272f - 0.272f * s, 0.534f - 0.534f * s, 0.131f + 0.869f * s, 0, 0,
                        0, 0, 0, 1, 0 };
        memcpy(m, v, sizeof v);
    } else if (!strcmp(name, "saturate")) sat_matrix(a, m);
    else if (!strcmp(name, "hue-rotate")) hue_matrix(a, m);
    else if (!strcmp(name, "invert")) {
        if (a > 1) a = 1;
        float k = 1 - 2 * a;
        m[0] = k; m[4] = a; m[6] = k; m[9] = a; m[12] = k; m[14] = a;
    } else if (!strcmp(name, "opacity")) { if (a > 1) a = 1; m[18] = a; }
    else if (!strcmp(name, "brightness")) { m[0] = a; m[6] = a; m[12] = a; }
    else if (!strcmp(name, "contrast")) { float i = 0.5f * (1 - a); m[0] = a; m[4] = i; m[6] = a; m[9] = i; m[12] = a; m[14] = i; }
}

/* The next function of a filter list: name (lowercase, up to '(') and its argument text. */
static const char *next_fn(const char *p, char *name, size_t nn, char *arg, size_t an)
{
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) return NULL;
    const char *op = strchr(p, '(');
    if (!op) return NULL;
    size_t l = (size_t)(op - p);
    if (l >= nn) l = nn - 1;
    for (size_t i = 0; i < l; i++) name[i] = (char)tolower((unsigned char)p[i]);
    name[l] = 0;
    const char *c = op + 1;
    int d = 1;
    while (*c && d) { if (*c == '(') d++; else if (*c == ')') d--; if (d) c++; }
    l = (size_t)(c - op - 1);
    if (l >= an) l = an - 1;
    memcpy(arg, op + 1, l);
    arg[l] = 0;
    return *c ? c + 1 : c;
}

bool svg_filter_apply(const SvgFilterEnv *env, const char *list, const float box[4], float fs,
                      uint8_t *layer, int W, int H)
{
    /* the area: all of the viewport (what's painted is there, and what's seen) */
    FCtx C;
    memset(&C, 0, sizeof C);
    C.env = env;
    C.ax = 0; C.ay = 0; C.aw = W; C.ah = H;
    memcpy(C.box, box, sizeof C.box);
    C.fs = fs;
    size_t N = (size_t)W * H;
    FImg cur = { img_new(&C), false, true, { 0, 0, (float)W, (float)H }, true };
    if (!cur.p) return false;
    for (size_t i = 0; i < N; i++) {
        const uint8_t *s = layer + i * 4;
        float a = s[3] / 255.0f;
        cur.p[i * 4] = s[0] / 255.0f * a; cur.p[i * 4 + 1] = s[1] / 255.0f * a; cur.p[i * 4 + 2] = s[2] / 255.0f * a; cur.p[i * 4 + 3] = a;
    }
    char name[32], arg[256];
    const char *p = list;
    while ((p = next_fn(p, name, sizeof name, arg, sizeof arg)) != NULL) {
        if (!strcmp(name, "url")) {
            char *q = arg;
            while (*q && (isspace((unsigned char)*q) || *q == '"' || *q == '\'')) q++;
            size_t l = strlen(q);
            while (l > 0 && (isspace((unsigned char)q[l - 1]) || q[l - 1] == '"' || q[l - 1] == '\'')) l--;
            q[l] = 0;
            const char *hash = strchr(q, '#');
            HtmlNode *flt = hash ? html_document_get_element_by_id(env->doc, hash + 1) : NULL;
            if (!flt || !flt->tag_name || strcasecmp(flt->tag_name, "filter")) continue;   /* (none such: as if none) */
            FImg o = { 0 };
            if (run_filter(&C, flt, &cur, &o)) {
                free(cur.p);
                cur = o;
                cur.taint = true;
                memcpy(cur.sr, (float[4]){ 0, 0, (float)W, (float)H }, 4 * sizeof(float));
            }
            continue;
        }
        /* (functions work in sRGB) */
        bool own;
        float *ip = img_in_space(&C, &cur, false, &own);
        if (!ip) break;
        float *d = img_new(&C);
        if (!d) { if (own) free(ip); break; }
        if (!strcmp(name, "blur")) {
            memcpy(d, ip, N * 4 * sizeof(float));
            char *e;
            double v = strtod(arg, &e);
            if (e != arg) {
                if (!strncasecmp(e, "em", 2)) v *= fs;
                gaussian(&C, d, v, v);
            }
        } else if (!strcmp(name, "drop-shadow")) {
            /* lengths and a color, either order */
            double len[3] = { 0, 0, 0 };
            int nl = 0;
            double rgba[4] = { env->current[0], env->current[1], env->current[2], env->current[3] };
            bool col = false;
            char *q = arg;
            while (*q) {
                while (isspace((unsigned char)*q)) q++;
                if (!*q) break;
                char *w0 = q;
                int d2 = 0;
                while (*q && (d2 > 0 || !isspace((unsigned char)*q))) { if (*q == '(') d2++; else if (*q == ')') d2--; q++; }
                char word[96];
                size_t wl = (size_t)(q - w0) < 95 ? (size_t)(q - w0) : 95;
                memcpy(word, w0, wl);
                word[wl] = 0;
                if ((isdigit((unsigned char)word[0]) || word[0] == '-' || word[0] == '.') && nl < 3) len[nl++] = strtod(word, NULL);
                else if (strcasecmp(word, "currentcolor") && env->color && env->color(env->ud, word, rgba)) col = true;
            }
            (void)col;
            float *sh = img_new(&C);
            if (sh) {
                offset_img(&C, ip, sh, len[0], len[1]);
                for (size_t i = 0; i < N; i++) { float a = sh[i * 4 + 3]; sh[i * 4] = sh[i * 4 + 1] = sh[i * 4 + 2] = 0; sh[i * 4 + 3] = a; }
                gaussian(&C, sh, len[2], len[2]);
                float c[3] = { (float)rgba[0], (float)rgba[1], (float)rgba[2] };
                for (size_t i = 0; i < N; i++) {
                    float a = sh[i * 4 + 3] * (float)rgba[3], ia = 1 - ip[i * 4 + 3];
                    for (int k = 0; k < 3; k++) d[i * 4 + k] = ip[i * 4 + k] + c[k] * a * ia;
                    d[i * 4 + 3] = ip[i * 4 + 3] + a * ia;
                }
                free(sh);
            }
        } else {
            float m[20];
            float amt = !strcmp(name, "hue-rotate") ? 0.0f : 1.0f;
            if (arg[0]) {
                amt = parse_amt(arg, NULL, amt);
                if (!strcmp(name, "hue-rotate")) {
                    char *e;
                    double d0 = strtod(arg, &e);
                    if (!strncasecmp(e, "turn", 4)) d0 *= 360.0;
                    else if (!strncasecmp(e, "rad", 3)) d0 *= 180.0 / M_PI;
                    else if (!strncasecmp(e, "grad", 4)) d0 *= 0.9;
                    amt = (float)d0;
                }
            }
            fn_matrix_of(name, amt, m);
            color_matrix(&C, ip, d, m);
        }
        if (own) free(ip);
        free(cur.p);
        cur.p = d;
        cur.lin = false;
    }
    /* back to the layer: straight sRGB */
    for (size_t i = 0; i < N; i++) {
        const float *s = cur.p + i * 4;
        uint8_t *o = layer + i * 4;
        float a = clamp01(s[3]);
        o[3] = (uint8_t)lroundf(a * 255.0f);
        for (int k = 0; k < 3; k++) {
            float c = a > 0.0f ? clamp01(s[k] / a) : 0.0f;
            if (cur.lin) c = lin_to_srgb(c);
            o[k] = (uint8_t)lroundf(c * 255.0f);
        }
    }
    free(cur.p);
    return true;
}

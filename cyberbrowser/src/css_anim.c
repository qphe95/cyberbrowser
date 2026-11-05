/*
 * css_anim.c - the store of running transitions and animations (css_anim.h).
 *
 * One row per thing that runs, its fields in flat arrays (a structure of
 * arrays), found from its element through a list per store slot:
 * head[slot] is the first row + 1, next[row] the one after.  Kinds:
 *   BASE   the last value layout saw of a property a transition covers
 *          (a, the value): where a change starts a transition;
 *   TRANS  a transition of property prop from a to b;
 *   ANIM   an animation (its timing and events, name);
 *   TRACK  the keyframes of one property of ANIM row owner (kfo/kfv; a, the
 *          value without it, for frames at 0% / 100% not given).
 * What happened to a row (events sent, seen by this layout) is in bitsets.
 * Freed rows go on a free list (through next) and are taken again.
 */
#include "css_anim.h"
#include "dom_store.h"
#include "css_parser.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "css_props.h"

enum { K_BASE, K_TRANS, K_ANIM, K_TRACK };
enum { E_LINEAR, E_CUBIC, E_STEPS };
enum { D_NORMAL, D_REVERSE, D_ALTERNATE, D_ALT_REVERSE };
enum { F_NONE = 0, F_FORWARDS = 1, F_BACKWARDS = 2, F_BOTH = 3 };

#define ANIM_KF 8           /* keyframes per property a track keeps */
#define ANIM_LIST 16        /* items per list in a specification */

static struct {
    int n, cap, free_head;  /* rows used (high water), allocated, first free + 1 */
    NodeId *el;
    uint8_t *ps, *kind, *prop, *ease, *dir, *fill, *kfn;
    float (*ep)[4];         /* the timing function's parameters */
    double *start, *delay, *dur, *iters, *pause_at;
    char (*name)[64];
    int *owner, *next, *iter;
    uint32_t *gen;
    AnimVal *a, *b;
    float (*kfo)[ANIM_KF];
    AnimVal (*kfv)[ANIM_KF];
    uint64_t *alive, *ev_run, *ev_start, *ev_end, *seen, *paused, *held;
    int *head;              /* per store slot */
    uint32_t head_n;
    int live_n;             /* rows alive */
} A;

static double (*g_clock)(void);

void anim_set_clock(double (*now)(void)) { g_clock = now; }
double anim_now(void) { return g_clock ? g_clock() : 0.0; }

static inline bool bit(const uint64_t *b, int i) { return (b[i >> 6] >> (i & 63)) & 1; }
static inline void bset(uint64_t *b, int i, bool on)
{
    if (on) b[i >> 6] |= 1ull << (i & 63);
    else b[i >> 6] &= ~(1ull << (i & 63));
}

/* ---- Properties ---- */

static const char *const k_props[ANIM_NPROP] = { "left", "top", "right", "bottom", "opacity", "transform", "width",
                                                  "height", "color", "background-color",
                                                  "padding-top", "padding-right", "padding-bottom", "padding-left",
                                                  "margin-top", "margin-right", "margin-bottom", "margin-left",
                                                  "border-top-width", "border-right-width", "border-bottom-width",
                                                  "border-left-width", "font-size", "letter-spacing", "word-spacing", "font-weight", "font-style" };

const char *anim_prop_name(int p) { return p >= 0 && p < ANIM_NPROP ? k_props[p] : ""; }

static __thread int t_flow_wm;
static __thread bool t_flow_rtl;

void anim_flow_set(int writing_mode, bool rtl)
{
    t_flow_wm = writing_mode;
    t_flow_rtl = rtl;
}

int anim_prop_of(const char *name)
{
    for (int p = 0; p < ANIM_NPROP; p++)
        if (!strcasecmp(name, k_props[p])) return p;
    /* (a translation: as the transform's) */
    if (!strcasecmp(name, "translate")) return ANIM_TRANSFORM;
    char phys[64];
    if (css_logical_physical(name, t_flow_wm, t_flow_rtl, phys, sizeof phys))
        for (int p = 0; p < ANIM_NPROP; p++)
            if (!strcasecmp(phys, k_props[p])) return p;
    return -1;
}

static bool val_same(const AnimVal *x, const AnimVal *y)
{
    return x->ok == y->ok && (!x->ok || (fabsf(x->x - y->x) < 1e-4f && fabsf(x->y - y->y) < 1e-4f &&
                                         fabsf(x->z - y->z) < 1e-4f && fabsf(x->w - y->w) < 1e-4f));
}

/* (colors: premultiplied, the alpha then kept to what alpha can be) */
static AnimVal color_mix(const AnimVal *x, const AnimVal *y, double t)
{
    if (!x->ok || !y->ok) return t < 0.5 ? *x : *y;
    double a = x->w + (y->w - x->w) * t;
    double r = x->x * x->w + (y->x * y->w - x->x * x->w) * t;
    double g = x->y * x->w + (y->y * y->w - x->y * x->w) * t;
    double b = x->z * x->w + (y->z * y->w - x->z * x->w) * t;
    AnimVal o = { 0, 0, 1, 0, 0 };
    if (a > 0) {
        if (a > 1) a = 1;
        o.x = (float)(r / a); o.y = (float)(g / a); o.z = (float)(b / a);
        o.w = (float)a;
    }
    return o;
}

static AnimVal val_mix(const AnimVal *x, const AnimVal *y, double t)
{
    if (!x->ok || !y->ok) return t < 0.5 ? *x : *y;
    AnimVal r = { (float)(x->x + (y->x - x->x) * t), (float)(x->y + (y->y - x->y) * t), 1,
                  (float)(x->z + (y->z - x->z) * t), (float)(x->w + (y->w - x->w) * t) };
    return r;
}

void anim_val_css(int prop, const AnimVal *v, char *buf, size_t cap)
{
    if (prop == ANIM_COLOR || prop == ANIM_BGCOLOR) {
        #define ANIM_C8(c) ((c) < 0.0f ? 0 : (c) > 1.0f ? 255 : (int)lround((c) * 255.0f + 0.002f))
        int r = ANIM_C8(v->x), g = ANIM_C8(v->y), b = (int)lround((v->z < 0.0f ? 0.0f : v->z > 1.0f ? 1.0f : v->z) * 255.0f);
        if (v->w >= 1.0f) snprintf(buf, cap, "rgb(%d, %d, %d)", r, g, b);
        else snprintf(buf, cap, "rgba(%d, %d, %d, %.6g)", r, g, b, v->w < 0.0f ? 0.0 : (double)v->w);
        return;
    }
    if (prop == ANIM_FONT_STYLE) {
        /* (x: the angle, y: italic) */
        double deg = v->x < -90 ? -90 : v->x > 90 ? 90 : v->x;
        if (v->y > 0.5f) snprintf(buf, cap, "italic");
        else if (fabs(deg) < 1e-4) snprintf(buf, cap, "normal");
        else snprintf(buf, cap, "oblique %.6gdeg", deg);
        return;
    }
    /* (what can't be below nothing) */
    bool floor0 = prop == ANIM_WIDTH || prop == ANIM_HEIGHT || prop == ANIM_FONT_SIZE ||
                  (prop >= ANIM_PADDING_TOP && prop <= ANIM_PADDING_LEFT) || (prop >= ANIM_BORDER_TOP && prop <= ANIM_BORDER_LEFT);
    if (prop == ANIM_TRANSFORM) snprintf(buf, cap, "matrix(1, 0, 0, 1, %.6g, %.6g)", v->x, v->y);
    else if (floor0 && v->x < 0) snprintf(buf, cap, "0px");
    else if (prop == ANIM_FONT_WEIGHT) snprintf(buf, cap, "%.6g", v->x < 1 ? 1.0 : v->x > 1000 ? 1000.0 : (double)v->x);
    else if (prop == ANIM_OPACITY || prop == ANIM_FONT_WEIGHT) snprintf(buf, cap, "%.6g", v->x);
    else snprintf(buf, cap, "%.6gpx", v->x);
}

/* ---- The specification ---- */

static const char *const k_fields[ANIM_NFIELD] = {
    "transition-property", "transition-duration", "transition-timing-function", "transition-delay",
    "animation-name", "animation-duration", "animation-timing-function", "animation-delay",
    "animation-iteration-count", "animation-direction", "animation-fill-mode", "animation-play-state",
};

int anim_spec_field(const char *prop)
{
    if (!strncasecmp(prop, "-webkit-", 8)) prop += 8;
    for (int f = 0; f < ANIM_NFIELD; f++)
        if (!strcasecmp(prop, k_fields[f])) return f;
    return -1;
}

/* The fields of spec: [start, end) of each (an empty one when spec is NULL). */
static void spec_split(const char *spec, const char *at[ANIM_NFIELD], const char *end[ANIM_NFIELD])
{
    const char *p = spec ? spec : "";
    for (int f = 0; f < ANIM_NFIELD; f++) {
        const char *q = p;
        while (*q && *q != 0x1F) q++;
        at[f] = p;
        end[f] = q;
        p = *q ? q + 1 : q;
    }
}

char *anim_spec_with(const char *spec, int field, const char *value)
{
    const char *at[ANIM_NFIELD], *end[ANIM_NFIELD];
    spec_split(spec, at, end);
    size_t vl = value ? strlen(value) : 0;
    /* (the initial value as "") */
    if (value && (!strcasecmp(value, "initial") || !strcasecmp(value, "unset") || !strcasecmp(value, "revert")))
        vl = 0;
    size_t total = vl + ANIM_NFIELD;
    for (int f = 0; f < ANIM_NFIELD; f++) total += (size_t)(end[f] - at[f]);
    char *r = (char *)malloc(total + 1), *w = r;
    if (!r) return NULL;
    for (int f = 0; f < ANIM_NFIELD; f++) {
        if (f) *w++ = 0x1F;
        const char *s = f == field ? value : at[f];
        size_t l = f == field ? vl : (size_t)(end[f] - at[f]);
        for (size_t i = 0; i < l; i++) w[i] = s[i] == 0x1F ? ' ' : s[i];
        w += l;
    }
    *w = 0;
    return r;
}

/* A field's comma-separated items (at most ANIM_LIST): their spans. */
typedef struct SpecList {
    int n;
    const char *at[ANIM_LIST];
    int len[ANIM_LIST];
} SpecList;

static void list_split(const char *s, const char *e, SpecList *L)
{
    L->n = 0;
    while (s < e && L->n < ANIM_LIST) {
        int depth = 0;
        const char *q = s;
        while (q < e && (depth || *q != ',')) {
            if (*q == '(') depth++;
            else if (*q == ')' && depth) depth--;
            q++;
        }
        const char *a = s, *b = q;
        while (a < b && (*a == ' ' || *a == '\t' || *a == '\n' || *a == '\r')) a++;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\n' || b[-1] == '\r')) b--;
        L->at[L->n] = a;
        L->len[L->n] = (int)(b - a);
        L->n++;
        s = q < e ? q + 1 : q;
    }
}

/* Item j of L (repeated as the list is short), into buf; def when empty. */
static const char *list_item(const SpecList *L, int j, const char *def, char *buf, size_t cap)
{
    if (!L->n) return def;
    int k = j % L->n;
    size_t l = (size_t)L->len[k] < cap - 1 ? (size_t)L->len[k] : cap - 1;
    memcpy(buf, L->at[k], l);
    buf[l] = 0;
    return l ? buf : def;
}

static double parse_time(const char *s)
{
    char *e = NULL;
    double v = strtod(s, &e);
    if (e == s) return 0;
    if (!strncasecmp(e, "ms", 2)) return v;
    if (*e == 's' || *e == 'S') return v * 1000.0;
    return 0;
}

/* A timing function (CSS Easing 1): its kind and parameters. */
static void parse_ease(const char *s, uint8_t *kind, float p[4])
{
    *kind = E_CUBIC;
    p[0] = 0.25f; p[1] = 0.1f; p[2] = 0.25f; p[3] = 1.0f;
    if (!strcasecmp(s, "linear") || !strncasecmp(s, "linear(", 7)) { *kind = E_LINEAR; return; }
    if (!strcasecmp(s, "ease-in")) { p[0] = 0.42f; p[1] = 0; p[2] = 1; p[3] = 1; return; }
    if (!strcasecmp(s, "ease-out")) { p[0] = 0; p[1] = 0; p[2] = 0.58f; p[3] = 1; return; }
    if (!strcasecmp(s, "ease-in-out")) { p[0] = 0.42f; p[1] = 0; p[2] = 0.58f; p[3] = 1; return; }
    if (!strcasecmp(s, "step-start")) { *kind = E_STEPS; p[0] = 1; p[1] = 0; return; }
    if (!strcasecmp(s, "step-end")) { *kind = E_STEPS; p[0] = 1; p[1] = 1; return; }
    if (!strncasecmp(s, "cubic-bezier(", 13)) {
        float v[4];
        if (sscanf(s + 13, "%f , %f , %f , %f", &v[0], &v[1], &v[2], &v[3]) == 4) memcpy(p, v, sizeof v);
        return;
    }
    if (!strncasecmp(s, "steps(", 6)) {
        *kind = E_STEPS;
        p[0] = (float)atoi(s + 6);
        if (p[0] < 1) p[0] = 1;
        p[1] = 1;   /* jump-end */
        const char *c = strchr(s, ',');
        if (c) {
            c++;
            while (*c == ' ') c++;
            if (!strncasecmp(c, "jump-start", 10) || !strncasecmp(c, "start", 5)) p[1] = 0;
            else if (!strncasecmp(c, "jump-none", 9)) p[1] = 2;
            else if (!strncasecmp(c, "jump-both", 9)) p[1] = 3;
        }
    }
}

static double bezier(double a, double b, double t)
{
    /* (one coordinate of a cubic Bezier from 0 to 1 with control points a, b) */
    double u = 1 - t;
    return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

static double ease_apply(uint8_t kind, const float p[4], double x)
{
    if (kind == E_LINEAR) return x;
    if (kind == E_STEPS) {
        int steps = (int)p[0], pos = (int)p[1];
        double cur = floor(x * steps);
        if (pos == 0 || pos == 3) cur += 1;
        int jumps = pos == 3 ? steps + 1 : pos == 2 ? steps - 1 : steps;
        if (jumps < 1) jumps = 1;
        if (x >= 0 && cur < 0) cur = 0;
        if (x <= 1 && cur > jumps) cur = jumps;
        return cur / jumps;
    }
    if (x <= 0 || x >= 1) return x <= 0 ? 0 : 1;
    /* t for x by bisection, then y at it */
    double lo = 0, hi = 1, t = x;
    for (int i = 0; i < 40; i++) {
        double bx = bezier(p[0], p[2], t);
        if (fabs(bx - x) < 1e-7) break;
        if (bx < x) lo = t;
        else hi = t;
        t = (lo + hi) / 2;
    }
    return bezier(p[1], p[3], t);
}

/* ---- Rows ---- */

static bool rows_grow(void)
{
    int cap = A.cap ? A.cap * 2 : 64;
#define G(f) do { void *q = realloc((void *)A.f, (size_t)cap * sizeof(*A.f)); if (!q) return false; A.f = q; } while (0)
    G(el); G(ps); G(kind); G(prop); G(ease); G(dir); G(fill); G(kfn); G(ep); G(start); G(delay); G(dur);
    G(iters); G(pause_at); G(name); G(owner); G(next); G(iter); G(gen); G(a); G(b); G(kfo); G(kfv);
#undef G
    size_t words = (size_t)cap / 64, old = (size_t)A.cap / 64;
    uint64_t **bits[] = { &A.alive, &A.ev_run, &A.ev_start, &A.ev_end, &A.seen, &A.paused, &A.held };
    for (size_t i = 0; i < sizeof bits / sizeof bits[0]; i++) {
        uint64_t *q = (uint64_t *)realloc(*bits[i], words * sizeof(uint64_t));
        if (!q) return false;
        memset(q + old, 0, (words - old) * sizeof(uint64_t));
        *bits[i] = q;
    }
    memset(A.gen + A.cap, 0, (size_t)(cap - A.cap) * sizeof(uint32_t));
    A.cap = cap;
    return true;
}

static int row_new(NodeId el, int ps, int kind, int prop)
{
    int r;
    if (A.free_head) {
        r = A.free_head - 1;
        A.free_head = A.next[r];
    } else {
        if (A.n == A.cap && !rows_grow()) return -1;
        r = A.n++;
    }
    uint32_t s = dom_slot(el);
    if (s >= A.head_n) {
        uint32_t n = A.head_n ? A.head_n : 1024;
        while (n <= s) n *= 2;
        int *h = (int *)realloc(A.head, (size_t)n * sizeof(int));
        if (!h) { A.next[r] = A.free_head; A.free_head = r + 1; return -1; }
        memset(h + A.head_n, 0, (size_t)(n - A.head_n) * sizeof(int));
        A.head = h;
        A.head_n = n;
    }
    A.el[r] = el;
    A.ps[r] = (uint8_t)ps;
    A.kind[r] = (uint8_t)kind;
    A.prop[r] = (uint8_t)prop;
    A.ease[r] = E_LINEAR;
    A.dir[r] = D_NORMAL;
    A.fill[r] = F_NONE;
    A.kfn[r] = 0;
    A.start[r] = A.delay[r] = A.dur[r] = 0;
    A.iters[r] = 1;
    A.name[r][0] = 0;
    A.owner[r] = -1;
    A.iter[r] = 0;
    A.a[r] = A.b[r] = (AnimVal){ 0, 0, 0 };
    bset(A.alive, r, true);
    bset(A.ev_run, r, false);
    bset(A.ev_start, r, false);
    bset(A.ev_end, r, false);
    bset(A.seen, r, false);
    bset(A.paused, r, false);
    bset(A.held, r, false);
    A.next[r] = A.head[s];
    A.head[s] = r + 1;
    A.live_n++;
    return r;
}

static void row_free(int r)
{
    if (r < 0 || !bit(A.alive, r)) return;
    uint32_t s = dom_slot(A.el[r]);
    for (int *link = &A.head[s]; *link; link = &A.next[*link - 1])
        if (*link - 1 == r) { *link = A.next[r]; break; }
    bset(A.alive, r, false);
    A.gen[r]++;
    A.next[r] = A.free_head;
    A.free_head = r + 1;
    A.live_n--;
}

/* An animation row and its tracks. */
static void anim_free(int r)
{
    uint32_t s = dom_slot(A.el[r]);
    for (int i = A.head[s]; i;) {
        int t = i - 1;
        i = A.next[t];
        if (A.kind[t] == K_TRACK && A.owner[t] == r) row_free(t);
    }
    row_free(r);
}

#define FOR_ROWS(r, id)                                                                         \
    for (int _i = dom_slot(id) < A.head_n ? A.head[dom_slot(id)] : 0, r; _i && ((r = _i - 1), (_i = A.next[r]), 1);) \
        if (A.el[r] == (id))

static int row_find(NodeId el, int ps, int kind, int prop, const char *name)
{
    FOR_ROWS(r, el)
        if (A.ps[r] == ps && A.kind[r] == kind && (prop < 0 || A.prop[r] == prop) && (!name || !strcmp(A.name[r], name)))
            return r;
    return -1;
}

bool anim_has(NodeId el, int ps)
{
    FOR_ROWS(r, el) if (A.ps[r] == ps) return true;
    return false;
}

/* Cancellations since the last tick (once it has been seen to run). */
static AnimEvent g_cancels[32];
static int g_ncancels;
bool anim_any(void) { return A.live_n > 0 || g_ncancels > 0; }
bool anim_cancels_due(void) { return g_ncancels > 0; }

/* ---- Values at a time ---- */

static double row_time(int r, double now);

static AnimVal trans_value(int r, double now)
{
    double t = row_time(r, now) - A.start[r] - A.delay[r];
    double x = A.dur[r] > 0 ? t / A.dur[r] : 1;
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    double ex = ease_apply(A.ease[r], A.ep[r], x);
    if (A.prop[r] == ANIM_FONT_STYLE && (A.a[r].y > 0.5f) != (A.b[r].y > 0.5f)) return ex < 0.5 ? A.a[r] : A.b[r];
    if (A.prop[r] == ANIM_COLOR || A.prop[r] == ANIM_BGCOLOR) return color_mix(&A.a[r], &A.b[r], ex);
    return val_mix(&A.a[r], &A.b[r], ex);
}

/* The time row r's animation has reached: a paused one's stays. */
static double row_time(int r, double now) { return bit(A.paused, r) ? A.pause_at[r] : now; }

/* The progress of animation row r at now (0 .. 1, eased), or false when it
 * has no effect then. */
static bool anim_progress(int r, double now, double *out)
{
    double t = row_time(r, now) - A.start[r] - A.delay[r];
    double dur = A.dur[r], iters = A.iters[r];
    double active = isinf(iters) ? INFINITY : dur * iters;
    double it, x;
    if (t < 0) {
        if (!(A.fill[r] & F_BACKWARDS)) return false;
        it = 0;
        x = 0;
    } else if (t >= active) {
        if (!(A.fill[r] & F_FORWARDS)) return false;
        double whole = floor(iters);
        it = iters == whole && iters > 0 ? iters - 1 : whole;
        x = iters == whole && iters > 0 ? 1 : iters - whole;
    } else if (dur <= 0) {
        it = 0;
        x = 1;
    } else {
        it = floor(t / dur);
        x = (t - it * dur) / dur;
    }
    bool rev = A.dir[r] == D_REVERSE || (A.dir[r] == D_ALTERNATE && fmod(it, 2) == 1) ||
               (A.dir[r] == D_ALT_REVERSE && fmod(it, 2) == 0);
    if (rev) x = 1 - x;
    *out = ease_apply(A.ease[r], A.ep[r], x);
    return true;
}

int anim_running(NodeId el, const char **names, double *progress, int cap, double now)
{
    int n = 0;
    FOR_ROWS(r, el) {
        double p;
        if (n < cap && A.ps[r] == 0 && A.kind[r] == K_ANIM && anim_progress(r, now, &p)) {
            names[n] = A.name[r];
            progress[n++] = p;
        }
    }
    return n;
}

static bool track_value(int r, double now, AnimVal *out)
{
    int o = A.owner[r];
    double p;
    if (o < 0 || !bit(A.alive, o) || !anim_progress(o, now, &p)) return false;
    /* (frames at 0% and 100% not given: the value without the animation) */
    float off[ANIM_KF + 2];
    AnimVal v[ANIM_KF + 2];
    int n = 0;
    if (!A.kfn[r] || A.kfo[r][0] > 0) { off[n] = 0; v[n++] = A.a[r]; }
    for (int i = 0; i < A.kfn[r]; i++) { off[n] = A.kfo[r][i]; v[n++] = A.kfv[r][i]; }
    if (off[n - 1] < 1) { off[n] = 1; v[n++] = A.a[r]; }
    int i = 0;
    while (i + 2 < n && off[i + 1] <= p) i++;
    double span = off[i + 1] - off[i];
    double local = span > 0 ? (p - off[i]) / span : 1;
    /* (italic doesn't go between) */
    if (A.prop[r] == ANIM_FONT_STYLE && (v[i].y > 0.5f) != (v[i + 1].y > 0.5f)) *out = local < 0.5 ? v[i] : v[i + 1];
    else *out = A.prop[r] == ANIM_COLOR || A.prop[r] == ANIM_BGCOLOR ? color_mix(&v[i], &v[i + 1], local) : val_mix(&v[i], &v[i + 1], local);
    return out->ok;
}

/* The parent's transition of a property that is going where this box's value is. */
static int inherited_run(NodeId el, int ps, int p, const AnimVal *v)
{
    NodeId up = ps ? el : dom_parent(el);
    if (!up) return -1;
    int r = row_find(up, 0, K_TRANS, p, NULL);
    return r >= 0 && val_same(&A.b[r], v) ? r : -1;
}

bool anim_value(NodeId el, int ps, int prop, double now, AnimVal *out)
{
    bool got = false;
    FOR_ROWS(r, el) {
        if (A.ps[r] != ps || A.prop[r] != prop) continue;
        AnimVal v;
        if (A.kind[r] == K_TRACK && !got && track_value(r, now, &v)) { *out = v; got = true; }
    }
    /* (transitions over animations: the cascade's transition origin) */
    FOR_ROWS(r, el)
        if (A.ps[r] == ps && A.prop[r] == prop && A.kind[r] == K_TRANS) { *out = trans_value(r, now); return true; }
    /* (what it inherits is moving) */
    if (!got) {
        int b = row_find(el, ps, K_BASE, prop, NULL);
        int up = b >= 0 ? inherited_run(el, ps, prop, &A.a[b]) : -1;
        if (up >= 0) { *out = trans_value(up, now); return true; }
    }
    return got;
}

/* ---- Layout ---- */

static void anim_timing(int r, const SpecList L[ANIM_NFIELD], int j, double now)
{
    char buf[128];
    bool paused = !strcasecmp(list_item(&L[ANIM_F_PLAY], j, "running", buf, sizeof buf), "paused");
    /* (script's pause() or play() has the say over the style's, Animations 2) */
    if (!bit(A.held, r)) {
        if (paused && !bit(A.paused, r)) A.pause_at[r] = now;
        /* (resumed: from where it stopped) */
        else if (!paused && bit(A.paused, r)) A.start[r] += now - A.pause_at[r];
        bset(A.paused, r, paused);
    }
    A.dur[r] = parse_time(list_item(&L[ANIM_F_DUR], j, "0s", buf, sizeof buf));
    A.delay[r] = parse_time(list_item(&L[ANIM_F_DELAY], j, "0s", buf, sizeof buf));
    parse_ease(list_item(&L[ANIM_F_EASE], j, "ease", buf, sizeof buf), &A.ease[r], A.ep[r]);
    const char *it = list_item(&L[ANIM_F_ITER], j, "1", buf, sizeof buf);
    A.iters[r] = !strcasecmp(it, "infinite") ? INFINITY : atof(it);
    if (A.iters[r] < 0) A.iters[r] = 1;
    const char *d = list_item(&L[ANIM_F_DIR], j, "normal", buf, sizeof buf);
    A.dir[r] = !strcasecmp(d, "reverse") ? D_REVERSE : !strcasecmp(d, "alternate") ? D_ALTERNATE :
               !strcasecmp(d, "alternate-reverse") ? D_ALT_REVERSE : D_NORMAL;
    const char *f = list_item(&L[ANIM_F_FILL], j, "none", buf, sizeof buf);
    A.fill[r] = !strcasecmp(f, "forwards") ? F_FORWARDS : !strcasecmp(f, "backwards") ? F_BACKWARDS :
                !strcasecmp(f, "both") ? F_BOTH : F_NONE;
}

static void note_cancel(int r, int type, double now)
{
    if (g_ncancels == (int)(sizeof g_cancels / sizeof g_cancels[0])) return;
    /* (only one whose run or start event has gone) */
    if (type == ANIM_EV_TCANCEL ? !bit(A.ev_run, r) : !bit(A.ev_start, r)) return;
    AnimEvent *e = &g_cancels[g_ncancels++];
    e->el = A.el[r];
    e->ps = A.ps[r];
    e->type = (uint8_t)type;
    snprintf(e->name, sizeof e->name, "%s", A.name[r]);
    double t = now - A.start[r] - A.delay[r];
    e->elapsed = (t > 0 ? t : 0) / 1000.0;
}

static void row_cancel_noted(int r, double now)
{
    if (A.kind[r] == K_TRANS) { note_cancel(r, ANIM_EV_TCANCEL, now); row_free(r); }
    else if (A.kind[r] == K_ANIM) {
        if (!bit(A.ev_end, r)) note_cancel(r, ANIM_EV_ACANCEL, now);
        anim_free(r);
    }
}

void anim_reset(void)
{
    for (int r = 0; r < A.n; r++) if (bit(A.alive, r)) row_free(r);
    g_ncancels = 0;
}

void anim_node_removed(NodeId root)
{
    if (A.live_n <= 0 || !dom_valid(root)) return;
    uint32_t top = dom_slot(root);
    double now = anim_now();
    for (int r = 0; r < A.n; r++) {
        if (!bit(A.alive, r) || !dom_valid(A.el[r])) continue;
        uint32_t s = dom_slot(A.el[r]);
        while (s && s != top) s = g_dom.parent[s] ? g_dom.parent[s] : g_dom.host[s];
        if (s) {
            if (A.kind[r] == K_BASE || A.kind[r] == K_TRACK) continue;
            row_cancel_noted(r, now);
        }
    }
}

void anim_pseudo_gone(NodeId el, int ps, double now)
{
    int cancel[ANIM_LIST], n = 0;
    FOR_ROWS(r, el)
        if (A.ps[r] == ps && (A.kind[r] == K_TRANS || A.kind[r] == K_ANIM) && n < ANIM_LIST) cancel[n++] = r;
    for (int i = 0; i < n; i++) row_cancel_noted(cancel[i], now);
}

void anim_seed(NodeId el, int ps, const AnimVal base[ANIM_NPROP])
{
    for (int p = 0; p < ANIM_NPROP; p++) {
        if (row_find(el, ps, K_BASE, p, NULL) >= 0) continue;
        int b = row_new(el, ps, K_BASE, p);
        if (b >= 0) A.a[b] = base[p];
    }
}

void anim_box(NodeId el, int ps, const char *spec, const AnimVal base[ANIM_NPROP], bool detect, double now,
              AnimKeyframesFn kf, AnimFrameFn frame, void *ud, AnimVal cur[ANIM_NPROP], bool set[ANIM_NPROP])
{
    const char *at[ANIM_NFIELD], *end[ANIM_NFIELD];
    spec_split(spec, at, end);
    SpecList L[ANIM_NFIELD];
    for (int f = 0; f < ANIM_NFIELD; f++) list_split(at[f], end[f], &L[f]);
    char buf[128];

    if (detect) {
        /* Transitions: the list item covering each property (-1: none). */
        int cover[ANIM_NPROP];
        for (int p = 0; p < ANIM_NPROP; p++) cover[p] = -1;
        int tn = L[ANIM_F_TPROP].n ? L[ANIM_F_TPROP].n : 1;
        for (int j = 0; j < tn; j++) {
            const char *nm = list_item(&L[ANIM_F_TPROP], j, "all", buf, sizeof buf);
            if (!strcasecmp(nm, "all")) for (int p = 0; p < ANIM_NPROP; p++) cover[p] = j;
            else if (anim_prop_of(nm) >= 0) cover[anim_prop_of(nm)] = j;
            /* (a shorthand: its longhands) */
            else if (!strcasecmp(nm, "padding")) for (int p = ANIM_PADDING_TOP; p <= ANIM_PADDING_LEFT; p++) cover[p] = j;
            else if (!strcasecmp(nm, "margin")) for (int p = ANIM_MARGIN_TOP; p <= ANIM_MARGIN_LEFT; p++) cover[p] = j;
            else if (!strcasecmp(nm, "border-width") || !strcasecmp(nm, "border"))
                for (int p = ANIM_BORDER_TOP; p <= ANIM_BORDER_LEFT; p++) cover[p] = j;
            else if (!strcasecmp(nm, "inset")) for (int p = ANIM_LEFT; p <= ANIM_BOTTOM; p++) cover[p] = j;
        }
        for (int p = 0; p < ANIM_NPROP; p++) {
            int b = row_find(el, ps, K_BASE, p, NULL), t = row_find(el, ps, K_TRANS, p, NULL);
            int j = cover[p];
            double dur = j >= 0 ? parse_time(list_item(&L[ANIM_F_TDUR], j, "0s", buf, sizeof buf)) : 0;
            double delay = j >= 0 ? parse_time(list_item(&L[ANIM_F_TDELAY], j, "0s", buf, sizeof buf)) : 0;
            if (j < 0 || dur + (delay > 0 ? delay : 0) <= 0) {
                /* (not transitioned: what ran is cancelled) */
                if (t >= 0) { note_cancel(t, ANIM_EV_TCANCEL, now); row_free(t); }
                if (b >= 0) row_free(b);
                continue;
            }
            if (b < 0) {
                b = row_new(el, ps, K_BASE, p);
                if (b >= 0) A.a[b] = base[p];
                continue;
            }
            if (val_same(&A.a[b], &base[p])) continue;
            AnimVal from = t >= 0 ? trans_value(t, now) : A.a[b];
            if (t >= 0) { note_cancel(t, ANIM_EV_TCANCEL, now); row_free(t); }
            A.a[b] = base[p];
            if (dur <= 0 || !from.ok || !base[p].ok || val_same(&from, &base[p])) continue;
            /* (italic doesn't transition) */
            if (p == ANIM_FONT_STYLE && (from.y > 0.5f || base[p].y > 0.5f)) continue;
            /* (what comes down from a parent that is transitioning it doesn't start one) */
            if (inherited_run(el, ps, p, &base[p]) >= 0) continue;
            t = row_new(el, ps, K_TRANS, p);
            if (t < 0) continue;
            A.a[t] = from;
            A.b[t] = base[p];
            A.start[t] = now;
            A.dur[t] = dur;
            A.delay[t] = delay;
            parse_ease(list_item(&L[ANIM_F_TEASE], j, "ease", buf, sizeof buf), &A.ease[t], A.ep[t]);
            snprintf(A.name[t], sizeof A.name[t], "%s", k_props[p]);
        }

        /* Animations: those animation-name names, started when new. */
        for (int j = 0; j < L[ANIM_F_NAME].n; j++) {
            const char *nm = list_item(&L[ANIM_F_NAME], j, "none", buf, sizeof buf);
            char name[64];
            snprintf(name, sizeof name, "%s", nm);
            size_t nl = strlen(name);
            if (nl >= 2 && (name[0] == '"' || name[0] == '\'') && name[nl - 1] == name[0]) {
                memmove(name, name + 1, nl - 2);
                name[nl - 2] = 0;
            }
            if (!name[0] || !strcasecmp(name, "none")) continue;
            int r = row_find(el, ps, K_ANIM, -1, name);
            if (r >= 0) {
                anim_timing(r, L, j, now);
                bset(A.seen, r, true);
                continue;
            }
            const float *offs;
            const void *const *decls;
            const int *counts;
            int fn = kf ? kf(ud, name, &offs, &decls, &counts) : -1;
            if (fn < 0) continue;
            r = row_new(el, ps, K_ANIM, ANIM_NPROP);
            if (r < 0) continue;
            snprintf(A.name[r], sizeof A.name[r], "%s", name);
            A.start[r] = now;
            anim_timing(r, L, j, now);
            bset(A.seen, r, true);
            /* (a track per property the frames set) */
            int track[ANIM_NPROP];
            for (int p = 0; p < ANIM_NPROP; p++) track[p] = -1;
            for (int i = 0; i < fn; i++) {
                AnimVal v[ANIM_NPROP];
                bool has[ANIM_NPROP] = { 0 };
                frame(ud, decls[i], counts[i], v, has);
                for (int p = 0; p < ANIM_NPROP; p++) {
                    if (!has[p]) continue;
                    if (track[p] < 0) {
                        track[p] = row_new(el, ps, K_TRACK, p);
                        if (track[p] < 0) continue;
                        A.owner[track[p]] = r;
                        A.a[track[p]] = base[p];
                    }
                    int t = track[p], k = A.kfn[t];
                    /* (frames at one offset: the last) */
                    if (k && A.kfo[t][k - 1] == offs[i]) k--;
                    if (k >= ANIM_KF) continue;
                    A.kfo[t][k] = offs[i];
                    A.kfv[t][k] = v[p];
                    A.kfn[t] = (uint8_t)(k + 1);
                }
            }
        }
        /* (the animations it no longer names stop; freed after the walk,
         * which their tracks are in) */
        int gone[ANIM_LIST], ng = 0;
        FOR_ROWS(r, el) {
            if (A.ps[r] != ps || A.kind[r] != K_ANIM) continue;
            if (!bit(A.seen, r)) { if (ng < ANIM_LIST) gone[ng++] = r; }
            else bset(A.seen, r, false);
        }
        for (int i = 0; i < ng; i++) {
            if (!bit(A.ev_end, gone[i])) note_cancel(gone[i], ANIM_EV_ACANCEL, now);
            anim_free(gone[i]);
        }
    }

    for (int p = 0; p < ANIM_NPROP; p++) {
        set[p] = false;
        int tr = row_find(el, ps, K_TRACK, p, NULL);
        if (tr >= 0) A.a[tr] = base[p];
        if (anim_value(el, ps, p, now, &cur[p])) set[p] = cur[p].ok;
    }
}

/* ---- Events ---- */

static int emit(AnimEvent *out, int n, int r, int type, double elapsed_ms)
{
    AnimEvent *e = &out[n];
    e->el = A.el[r];
    e->ps = A.ps[r];
    e->type = (uint8_t)type;
    snprintf(e->name, sizeof e->name, "%s", A.name[r]);
    e->elapsed = elapsed_ms / 1000.0;
    return n + 1;
}

int anim_tick(double now, AnimEvent *out, int cap, bool *live)
{
    int n = 0;
    *live = false;
    (void)now;
    while (g_ncancels > 0 && n + 4 < cap) out[n++] = g_cancels[--g_ncancels];
    for (int r = 0; r < A.n; r++) {
        if (!bit(A.alive, r)) continue;
        NodeId el = A.el[r];
        if (!dom_valid(el) || !dom_is_connected(el)) {
            /* (the element left: what ran is cancelled, and says so) */
            if (dom_valid(el)) {
                if (A.kind[r] == K_TRANS) note_cancel(r, ANIM_EV_TCANCEL, now);
                else if (A.kind[r] == K_ANIM && !bit(A.ev_end, r)) note_cancel(r, ANIM_EV_ACANCEL, now);
            }
            if (A.kind[r] == K_ANIM) anim_free(r);
            else if (A.kind[r] != K_TRACK) row_free(r);
            continue;
        }
        if (n + 3 > cap) { *live = true; break; }
        if (A.kind[r] == K_TRANS) {
            double t = row_time(r, now) - A.start[r];
            double el0 = A.delay[r] < 0 ? -A.delay[r] : 0;
            if (el0 > A.dur[r]) el0 = A.dur[r];
            if (!bit(A.ev_run, r)) { n = emit(out, n, r, ANIM_EV_TRUN, el0); bset(A.ev_run, r, true); }
            if (t >= A.delay[r] && !bit(A.ev_start, r)) { n = emit(out, n, r, ANIM_EV_TSTART, el0); bset(A.ev_start, r, true); }
            if (t >= A.delay[r] + A.dur[r]) {
                n = emit(out, n, r, ANIM_EV_TEND, A.dur[r]);
                row_free(r);
                continue;
            }
            *live = true;
        } else if (A.kind[r] == K_ANIM) {
            if (bit(A.ev_end, r)) continue;
            double t = row_time(r, now) - A.start[r] - A.delay[r];
            double active = isinf(A.iters[r]) ? INFINITY : A.dur[r] * A.iters[r];
            *live = true;
            if (t < 0) continue;
            if (!bit(A.ev_start, r)) {
                n = emit(out, n, r, ANIM_EV_ASTART, A.delay[r] < 0 ? -A.delay[r] : 0);
                bset(A.ev_start, r, true);
                /* (it starts in the iteration a negative delay puts it in) */
                if (A.dur[r] > 0) A.iter[r] = (int)floor((t < active ? t : 0) / A.dur[r]);
            }
            if (t >= active) {
                n = emit(out, n, r, ANIM_EV_AEND, active);
                bset(A.ev_end, r, true);
                continue;
            }
            if (A.dur[r] > 0) {
                int it = (int)floor(t / A.dur[r]);
                if (it > A.iter[r]) {
                    A.iter[r] = it;
                    n = emit(out, n, r, ANIM_EV_AITER, it * A.dur[r]);
                }
            }
        }
    }
    while (g_ncancels > 0 && n + 4 < cap) out[n++] = g_cancels[--g_ncancels];
    return n;
}

int anim_docs(NodeId *out, int cap)
{
    int n = 0;
    for (int r = 0; r < A.n; r++) {
        if (!bit(A.alive, r) || !dom_valid(A.el[r])) continue;
        NodeId d = dom_owner(A.el[r]);
        int i = 0;
        while (i < n && out[i] != d) i++;
        if (i == n && n < cap) out[n++] = d;
    }
    return n;
}

/* ---- getAnimations() ---- */

int anim_list(NodeId el, int *rows, int cap)
{
    int n = 0;
    /* (transitions first, then animations: CSS Transitions 2 / Animations 2
     * composite order) */
    for (int kind = K_TRANS; kind <= K_ANIM; kind++)
        FOR_ROWS(r, el)
            if (A.kind[r] == kind && !(kind == K_ANIM && bit(A.ev_end, r) && !(A.fill[r] & F_FORWARDS)) && n < cap)
                rows[n++] = r;
    /* (the list runs newest first) */
    for (int i = 0, j = n - 1; i < j; i++, j--) {
        if (A.kind[rows[i]] != A.kind[rows[j]]) continue;
        int t = rows[i]; rows[i] = rows[j]; rows[j] = t;
    }
    return n;
}

uint32_t anim_row_gen(int r) { return r >= 0 && r < A.n ? A.gen[r] : 0; }

bool anim_row_info(int r, uint32_t gen, int *kind, const char **name, NodeId *el, int *ps)
{
    if (r < 0 || r >= A.n || !bit(A.alive, r) || A.gen[r] != gen) return false;
    if (kind) *kind = A.kind[r] == K_TRANS ? 1 : 2;
    if (name) *name = A.name[r];
    if (el) *el = A.el[r];
    if (ps) *ps = A.ps[r];
    return true;
}

void anim_row_values(int r, double now, void (*fn)(void *ud, const char *prop, const char *value), void *ud)
{
    if (r < 0 || r >= A.n || !bit(A.alive, r)) return;
    char buf[96];
    if (A.kind[r] == K_TRANS) {
        AnimVal v = trans_value(r, now);
        anim_val_css(A.prop[r], &v, buf, sizeof buf);
        fn(ud, k_props[A.prop[r]], buf);
        return;
    }
    FOR_ROWS(t, A.el[r]) {
        AnimVal v;
        if (A.kind[t] != K_TRACK || A.owner[t] != r || !track_value(t, now, &v)) continue;
        anim_val_css(A.prop[t], &v, buf, sizeof buf);
        fn(ud, k_props[A.prop[t]], buf);
    }
}

bool anim_row_timing(int r, uint32_t gen, double now, AnimRowTiming *out)
{
    if (!anim_row_info(r, gen, NULL, NULL, NULL, NULL)) return false;
    out->start = A.start[r];
    out->delay = A.delay[r];
    out->duration = A.dur[r];
    out->iterations = A.kind[r] == K_TRANS ? 1 : A.iters[r];
    out->current = row_time(r, now) - A.start[r];
    out->paused = bit(A.paused, r);
    out->ended = A.kind[r] == K_ANIM && bit(A.ev_end, r);
    out->fill = A.fill[r];
    out->direction = A.dir[r];
    return true;
}

void anim_row_set_time(int r, uint32_t gen, double now, double t)
{
    if (!anim_row_info(r, gen, NULL, NULL, NULL, NULL)) return;
    A.start[r] = row_time(r, now) - t;
    if (A.kind[r] != K_ANIM) return;
    /* (back before its end or start: those can come again) */
    double local = t - A.delay[r], active = isinf(A.iters[r]) ? INFINITY : A.dur[r] * A.iters[r];
    if (local < active) bset(A.ev_end, r, false);
    if (local < 0) { bset(A.ev_start, r, false); A.iter[r] = 0; }
    /* (forward over an iteration's end: the next frame's event is for it; back: none) */
    else if (A.dur[r] > 0) {
        int it = (int)floor((local < active ? local : 0) / A.dur[r]);
        if (it < A.iter[r] || !bit(A.ev_start, r)) A.iter[r] = it;
    }
}

void anim_row_set_start(int r, uint32_t gen, double start)
{
    if (anim_row_info(r, gen, NULL, NULL, NULL, NULL)) A.start[r] = start;
}

void anim_row_pause(int r, uint32_t gen, double now, bool paused)
{
    if (!anim_row_info(r, gen, NULL, NULL, NULL, NULL)) return;
    if (paused && !bit(A.paused, r)) A.pause_at[r] = now;
    else if (!paused && bit(A.paused, r)) A.start[r] += now - A.pause_at[r];
    bset(A.paused, r, paused);
    bset(A.held, r, true);
}

void anim_row_cancel(int r)
{
    if (r < 0 || r >= A.n || !bit(A.alive, r)) return;
    if (A.kind[r] == K_ANIM) anim_free(r);
    else row_free(r);
}

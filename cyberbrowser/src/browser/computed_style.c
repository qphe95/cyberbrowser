/*
 * computed_style.c - getComputedStyle's values (CSSOM 9 "resolved value"):
 * a live snapshot of one element's style, from the layout's cascade.
 *
 * The snapshot is refreshed when the document changed since it was taken
 * (dom_change_stamp): the cascaded declarations of the element
 * (layout_query_cascade) give each longhand's specified value, shorthands
 * expanded and invalid declarations dropped.  A value is computed when it
 * is read: CSS-wide keywords and inheritance (through a snapshot of the
 * parent, taken when needed), then css_prop_compute and the rules of the
 * properties whose computed values depend on others (font-weight, border
 * widths, display, ...); a rendered element's sizes, margins and paddings
 * are their used values.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <quickjs.h>
#include "css_props.h"
#include "font_registry.h"
#include "text_shaper.h"
#include "css_parser.h"
#include "dom_js.h"
#include "layout_api.h"
#include "computed_style.h"
#include "css_anim.h"
#include "js_quickjs.h"

uint64_t dom_change_stamp(void);
/* (read once: a call for every declaration otherwise, and each one walks the environment) */
static int g_cs_trace = -1;
#define CST(...) do { if (g_cs_trace < 0) g_cs_trace = getenv("CS_TRACE") != NULL; if (g_cs_trace) { fprintf(stderr, "[cs] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } } while (0)

typedef struct Custom { char *name, *value; } Custom;

struct ComputedStyle {
    NodeId el;
    uint64_t stamp;
    bool taken, styled;
    char **spec;        /* [css_prop_count()] the cascaded value, or NULL */
    char **comp;        /* [css_prop_count()] computed values read so far */
    Custom *custom;
    int ncustom;
    LayoutQueryUsed used;
    double root_font_size, vw, vh;
    struct ComputedStyle *parent;
    int pseudo;
};

ComputedStyle *computed_style_new(NodeId el) {
    ComputedStyle *cs = calloc(1, sizeof *cs);
    if (cs) cs->el = el;
    return cs;
}

void computed_style_set_pseudo(ComputedStyle *cs, int ps) {
    if (cs) cs->pseudo = ps;
}

static void snapshot_clear(ComputedStyle *cs) {
    int n = css_prop_count();
    for (int i = 0; cs->spec && i < n; i++) if (cs->spec[i] || cs->comp[i]) { free(cs->spec[i]); free(cs->comp[i]); cs->spec[i] = cs->comp[i] = NULL; }
    for (int i = 0; i < cs->ncustom; i++) { free(cs->custom[i].name); free(cs->custom[i].value); }
    free(cs->custom);
    cs->custom = NULL;
    cs->ncustom = 0;
    if (cs->parent) { computed_style_free(cs->parent); cs->parent = NULL; }
    cs->taken = cs->styled = false;
}

void computed_style_free(ComputedStyle *cs) {
    if (!cs) return;
    snapshot_clear(cs);
    free(cs->spec);
    free(cs->comp);
    free(cs);
}

static int prop_idx(const CssPropDef *p) { return (int)(p - css_prop_at(0)); }

static void set_spec(ComputedStyle *cs, const CssPropDef *p, const char *v) {
    /* (logical: kept as the physical property it is here) */
    char phys[64];
    if (p->name && css_logical_physical(p->name, cs->used.writing_mode, cs->used.rtl, phys, sizeof phys)) {
        const CssPropDef *pp = css_prop_find(phys, strlen(phys));
        if (pp) p = pp;
    }
    int i = prop_idx(p);
    free(cs->spec[i]);
    /* (the sum it is) */
    if (strstr(v, "calc-mix(")) {
        extern char *css_calc_mix_expand(const char *value);
        char *sum = css_calc_mix_expand(v);
        if (sum) {
            cs->spec[i] = sum;
            return;
        }
    }
    cs->spec[i] = strdup(v);
}

/* One cascaded declaration (in cascade order: the last one wins). */
static bool marker_allows(const char *prop) {
    static const char *const pre[] = { "font", "animation", "transition", "text-emphasis", "color", "content", "cursor", "direction", "hyphens", "letter-spacing",
                                       "line-break", "line-height", "overflow-wrap", "quotes", "tab-size", "text-combine-upright", "text-decoration-skip-ink",
                                       "text-shadow", "text-transform", "unicode-bidi", "white-space", "word-break", "word-spacing", "text-wrap" };
    for (size_t k = 0; k < sizeof pre / sizeof pre[0]; k++)
        if (!strncmp(prop, pre[k], strlen(pre[k]))) return true;
    return false;
}

static bool first_line_allows(const char *prop) {
    static const char *const pre[] = { "background", "color", "font", "letter-spacing", "word-spacing", "opacity", "text-", "line-height",
                                       "vertical-align", "ruby-", "-webkit-text", "white-space" };
    for (size_t k = 0; k < sizeof pre / sizeof pre[0]; k++)
        if (!strncmp(prop, pre[k], strlen(pre[k]))) return true;
    return false;
}

static void collect(void *ud, const char *prop, const char *value, bool important) {
    CST("decl %s: %s", prop, value);
    (void)important;
    ComputedStyle *cs = ud;
    size_t pl = strlen(prop);
    if (css_prop_is_custom(prop, pl)) return;      /* (read from the layout when asked for) */
    /* (what a first line or first letter can have) */
    if (cs->pseudo == 4 && !first_line_allows(prop)) return;
    if (cs->pseudo == 5 && !marker_allows(prop)) return;
    if (cs->pseudo == 3 && (!strcmp(prop, "position") || !strncmp(prop, "transition", 10) || !strcmp(prop, "transform") || !strcmp(prop, "word-break"))) return;
    const CssPropDef *p = css_prop_find(prop, pl);
    if (!p) return;
    /* (all: every longhand but the two that say how text runs) */
    if (!strcmp(p->name, "all")) {
        for (int i = 0; i < css_prop_count(); i++) {
            const CssPropDef *lp = css_prop_at(i);
            if (lp->longhands || lp == p || !strcmp(lp->name, "direction") || !strcmp(lp->name, "unicode-bidi")) continue;
            set_spec(cs, lp, value);
        }
        return;
    }
    /* var() that did not resolve: invalid at computed-value time (unset) */
    bool unresolved = strstr(value, "var(") != NULL;
    if (p->longhands) {
        const char **names;
        int n = css_prop_longhands(p, &names);
        if (unresolved) {
            for (int i = 0; i < n; i++) set_spec(cs, css_prop_find(names[i], strlen(names[i])), "unset");
            return;
        }
        char **vals = NULL;
        if (css_prop_expand(p, value, &vals) != n) return;
        for (int i = 0; i < n; i++) {
            const CssPropDef *lp = css_prop_find(names[i], strlen(names[i]));
            if (lp) set_spec(cs, lp, vals[i][0] ? vals[i] : "unset");
            free(vals[i]);
        }
        free(vals);
        return;
    }
    if (unresolved) { set_spec(cs, p, "unset"); return; }
    char *v = css_prop_parse(p, value);
    if (!v) return;                 /* invalid: the declaration is ignored */
    set_spec(cs, p, v);
    free(v);
}

/* Take the snapshot again when the document changed since. */
static void refresh(JSContextHandle ctx, ComputedStyle *cs) {
    CST("refresh %u", (unsigned)cs->el);
    uint64_t stamp = dom_change_stamp();
    if (cs->taken && cs->stamp == stamp) return;
    int n = css_prop_count();
    if (!cs->spec) {
        cs->spec = calloc((size_t)n, sizeof(char *));
        cs->comp = calloc((size_t)n, sizeof(char *));
        if (!cs->spec || !cs->comp) return;
    }
    snapshot_clear(cs);
    cs->taken = true;
    GCValue el = dom_js(ctx, cs->el);
    CST("query");
    if (cs->pseudo) {
        memset(&cs->used, 0, sizeof cs->used);
        cs->styled = JS_IsObject(el) && dom_kind(cs->el) == DOM_K_ELEMENT;
        /* (a marker's own defaults) */
        if (cs->styled && cs->pseudo == 5) {
            static const char *const md[][2] = { { "unicode-bidi", "isolate" }, { "font-variant-numeric", "tabular-nums" }, { "text-transform", "none" }, { "text-indent", "0px" } };
            for (size_t k = 0; k < sizeof md / sizeof md[0]; k++) set_spec(cs, css_prop_find(md[k][0], strlen(md[k][0])), md[k][1]);
        }
        if (cs->styled) layout_query_pseudo_cascade(ctx, el, cs->pseudo, collect, cs);
    } else {
        cs->styled = JS_IsObject(el) && dom_kind(cs->el) == DOM_K_ELEMENT && dom_is_connected(cs->el) &&
                     layout_query_cascade(ctx, el, collect, cs, &cs->used);
    }
    CST("queried %d", cs->styled);
    cs->stamp = dom_change_stamp();     /* (the query may have laid out) */
    GCValue win = JS_GetGlobalObject(ctx);
    double v = 0;
    if (JS_ToFloat64(ctx, &v, JS_GetPropertyStr(ctx, win, "innerWidth")) == 0) cs->vw = v;
    if (JS_ToFloat64(ctx, &v, JS_GetPropertyStr(ctx, win, "innerHeight")) == 0) cs->vh = v;
    cs->root_font_size = 16;
    NodeId root = dom_node_of(JS_GetPropertyStr(ctx, JS_GetPropertyStr(ctx, win, "document"), "documentElement"));
    double rfs = 0;
    if (root && root != cs->el && layout_query_font_size(ctx, dom_js(ctx, root), &rfs) && rfs > 0) cs->root_font_size = rfs;
    else if (root == cs->el && cs->used.font_size > 0) cs->root_font_size = cs->used.font_size;
}

static ComputedStyle *parent_of(JSContextHandle ctx, ComputedStyle *cs) {
    if (cs->parent) return cs->parent;
    NodeId p = cs->pseudo ? cs->el : dom_parent(cs->el);
    /* (a shadow root's host is its children's parent for inheritance) */
    if (p && dom_kind(p) != DOM_K_ELEMENT) p = DOM_NULL;
    if (!p) return NULL;
    cs->parent = computed_style_new(p);
    if (cs->parent) refresh(ctx, cs->parent);
    return cs->parent;
}

extern double css_length_value(const char *value, double base, double viewport, double font_size);
static char *compute(JSContextHandle ctx, ComputedStyle *cs, const CssPropDef *p);

/* One percentage of a random(): a number, maybe times another. */
static bool random_pct(const char *a, const char *b, double *out) {
    char *t = strndup(a, (size_t)(b - a)), *e = NULL;
    double x = strtod(t, &e), y = 1.0;
    while (*e == ' ') e++;
    if (*e == '*') { char *e2 = NULL; y = strtod(e + 1, &e2); e = e2; while (*e == ' ') e++; }
    bool ok = *e == '%' && e[1 + strspn(e + 1, " ")] == 0;
    free(t);
    *out = x * y;
    return ok;
}

/* random(a%, b%) as it computes: random(fixed where-it-fell, a%, b%). */
static char *random_fixed(const char *spec, const char *now) {
    const char *r = strstr(spec, "random(");
    if (r != spec) return NULL;
    size_t sl = strlen(spec);
    if (spec[sl - 1] != ')') return NULL;
    const char *a = r + 7, *e = spec + sl - 1;
    const char *c1 = memchr(a, ',', (size_t)(e - a));
    if (!c1) return NULL;
    const char *first = a;
    while (*first == ' ') first++;
    if (isalpha((unsigned char)*first) || *first == '-') { a = c1 + 1; c1 = memchr(a, ',', (size_t)(e - a)); if (!c1) return NULL; }
    if (memchr(c1 + 1, ',', (size_t)(e - c1 - 1))) return NULL;
    double lo, hi;
    if (!random_pct(a, c1, &lo) || !random_pct(c1 + 1, e, &hi)) return NULL;
    double v = strtod(now, NULL), f = hi > lo ? (v - lo) / (hi - lo) : 0.0;
    if (f < 0.0) f = 0.0;
    if (f > 0.999999) f = 0.999999;
    char out[128];
    snprintf(out, sizeof out, "random(fixed %.6g, %.6g%%, %.6g%%)", f, lo, hi);
    return strdup(out);
}

static const char *computed_of(JSContextHandle ctx, ComputedStyle *cs, const char *name) {
    const CssPropDef *p = css_prop_find(name, strlen(name));
    if (!p) return "";
    int i = prop_idx(p);
    if (!cs->comp[i]) {
        cs->comp[i] = compute(ctx, cs, p);
        /* (flex-start and flex-end compute to flow-start and flow-end) */
        char *fx = cs->comp[i] && (name[0] == 'a' || name[0] == 'j') && (!strncmp(name, "align-", 6) || !strncmp(name, "justify-", 8)) ? strstr(cs->comp[i], "flex-") : NULL;
        if (fx && (!strcmp(fx, "flex-start") || !strcmp(fx, "flex-end"))) { fx[1] = 'l'; fx[2] = 'o'; fx[3] = 'w'; }
        /* (a track's lengths don't go below nothing) */
        if (cs->comp[i] && name[0] == 'g' && (!strcmp(name, "grid-auto-rows") || !strcmp(name, "grid-auto-columns") ||
                                              !strcmp(name, "grid-template-rows") || !strcmp(name, "grid-template-columns"))) {
            char *c = cs->comp[i];
            for (char *m = strchr(c, '-'); m; m = strchr(m + 1, '-')) {
                if (!(m == c || m[-1] == ' ' || m[-1] == '(') || !(isdigit((unsigned char)m[1]) || m[1] == '.')) continue;
                char *e = NULL;
                strtod(m, &e);
                if (strncmp(e, "px", 2)) continue;
                *m = '0';
                memmove(m + 1, e, strlen(e) + 1);
            }
        }
        /* (gaps don't go below nothing, however they're worked out) */
        if (cs->comp[i] && cs->comp[i][0] == '-' && (!strcmp(name, "row-gap") || !strcmp(name, "column-gap")) && !strchr(cs->comp[i], '%')) {
            free(cs->comp[i]);
            cs->comp[i] = strdup("0px");
        }
        /* (a percentage left as it is: its random() stays, with where it fell) */
        if (cs->comp[i] && cs->spec && cs->spec[i] && strstr(cs->spec[i], "random(") && strchr(cs->comp[i], '%') && !strstr(cs->comp[i], "random(")) {
            char *fx2 = random_fixed(cs->spec[i], cs->comp[i]);
            if (fx2) { free(cs->comp[i]); cs->comp[i] = fx2; }
        }
        /* (the same shape and box twice: once; each's default box goes without saying) */
        if (cs->comp[i] && name[0] == 'b' && !strcmp(name, "border-shape")) {
            char *v = cs->comp[i];
            size_t cl = strlen(v);
            if (strstr(v, "-box") && cl % 2 && v[cl / 2] == ' ' && !strncmp(v, v + cl / 2 + 1, cl / 2)) v[cl / 2] = 0;
            else {
                char *bb = strstr(v, ") border-box ");
                if (bb) memmove(bb + 1, bb + 12, strlen(bb + 12) + 1);
                cl = strlen(v);
                if (cl > 13 && !strcmp(v + cl - 13, ") padding-box")) v[cl - 12] = 0;
            }
            for (char *hb; (hb = strstr(v, " half-border-box")) != NULL; ) memmove(hb, hb + 16, strlen(hb + 16) + 1);
        }
        /* (a percentage in calc() and nothing else: the percentage) */
        if (cs->comp[i] && !strncmp(cs->comp[i], "calc(", 5)) {
            char *pe = NULL;
            strtod(cs->comp[i] + 5, &pe);
            if (pe != cs->comp[i] + 5 && pe[0] == '%' && pe[1] == ')' && !pe[2]) {
                size_t pl = (size_t)(pe + 1 - (cs->comp[i] + 5));
                memmove(cs->comp[i], cs->comp[i] + 5, pl);
                cs->comp[i][pl] = 0;
            }
        }
        /* (zoom: a number; none at all is normal) */
        if (cs->comp[i] && name[0] == 'z' && !strcmp(name, "zoom")) {
            char *e = NULL;
            double z = strtod(cs->comp[i], &e);
            if (e == cs->comp[i]) z = 1.0;
            else if (*e == '%') z /= 100.0;
            if (z <= 0.0) z = 1.0;
            char zb[32];
            snprintf(zb, sizeof zb, "%.6g", z);
            free(cs->comp[i]);
            cs->comp[i] = strdup(zb);
        }
        /* (the size a font of no size is kept at) */
        if (cs->comp[i] && name[0] == 'f' && !strcmp(name, "font-size") && !strcmp(cs->comp[i], "0.01px")) {
            free(cs->comp[i]);
            cs->comp[i] = strdup("0px");
        }
        /* (legacy: the parent's, if that's legacy with a direction) */
        if (cs->comp[i] && name[0] == 'j' && !strcmp(cs->comp[i], "legacy") && !strcmp(name, "justify-items")) {
            ComputedStyle *t = parent_of(ctx, cs);
            const char *pv = t && t->styled && t->spec ? computed_of(ctx, t, name) : NULL;
            char *got = pv && !strncmp(pv, "legacy ", 7) ? strdup(pv) : NULL;
            free(cs->comp[i]);
            cs->comp[i] = got ? got : strdup("normal");
        }
    }
    return cs->comp[i] ? cs->comp[i] : "";
}

static char *px(double v) {
    char tmp[64];
    if (v == (long long)v) snprintf(tmp, sizeof tmp, "%lldpx", (long long)v);
    else {
        snprintf(tmp, sizeof tmp, "%.6f", v);
        char *e = tmp + strlen(tmp) - 1;
        while (*e == '0') *e-- = '\0';
        if (*e == '.') *e = '\0';
        strcat(tmp, "px");
    }
    return strdup(tmp);
}

/* Does v contain word (ASCII case-insensitive)? */
static bool mentions(const char *v, const char *word) {
    size_t n = strlen(word);
    for (const char *p = v; *p; p++) if (!strncasecmp(p, word, n)) return true;
    return false;
}

static const char *computed_of(JSContextHandle ctx, ComputedStyle *cs, const char *name);

/* The element's font size in px: the layout's, or (without a styled box)
 * its computed font-size. */
static double font_px(JSContextHandle ctx, ComputedStyle *cs) {
    if (cs->used.font_size > 0) return cs->used.font_size;
    if (!cs->styled) return 16;
    double v = atof(computed_of(ctx, cs, "font-size"));
    return v > 0 ? v : 16;
}

static void env_of(JSContextHandle ctx, ComputedStyle *cs, const CssPropDef *p, const char *v, CssComputeEnv *env) {
    memset(env, 0, sizeof *env);
    /* (the font's own properties: of the parent's font) */
    bool is_fs = !strncmp(p->name, "font", 4), is_color = !strcmp(p->name, "color");
    ComputedStyle *par = (is_fs || is_color) ? parent_of(ctx, cs) : NULL;
    env->font_size = is_fs ? (par ? font_px(ctx, par) : 16) : font_px(ctx, cs);
    env->root_font_size = cs->root_font_size;
    /* (ch and ex: of the font it has) */
    if (v && (strstr(v, "ch") || strstr(v, "ex")) && (!is_fs || strchr(v, '('))) {
        const char *fam = computed_of(ctx, is_fs && par ? par : cs, "font-family");
        TextShaper *f = fam && fam[0] ? font_registry_slot(font_registry_resolve(fam, 400, false)) : NULL;
        if (f) {
            env->ch_ratio = text_shaper_zero_advance(f);
            env->ex_ratio = text_shaper_x_height(f);
        }
    }
    env->vw = cs->vw;
    env->vh = cs->vh;
    env->color[3] = 1;
    /* currentcolor: the color (for color itself, the parent's; a shadow with none
     * has it too) */
    if (!mentions(v, "currentcolor") && strcmp(p->name, "box-shadow") && strcmp(p->name, "text-shadow")) return;
    const char *c = is_color ? (par ? computed_of(ctx, par, "color") : "rgb(0, 0, 0)") : computed_of(ctx, cs, "color");
    double rgba[4];
    if (css_color_parse(c, rgba)) memcpy(env->color, rgba, sizeof rgba);
}

static int weight_num(const char *w) {
    if (!strcmp(w, "normal")) return 400;
    if (!strcmp(w, "bold")) return 700;
    return atoi(w);
}

/* The computed value of v for p (not a CSS-wide keyword). */
/* Each drop-shadow() in filter list r, its color first (none: the color). */
static char *filter_shadows(JSContextHandle ctx, ComputedStyle *cs, char *r)
{
    if (!strstr(r, "drop-shadow(")) return r;
    size_t cap = strlen(r) * 2 + 128, n = 0;
    char *out = malloc(cap);
    if (!out) return r;
    const char *cur = computed_of(ctx, cs, "color");
    const char *q = r;
    for (const char *h; (h = strstr(q, "drop-shadow(")); ) {
        size_t lead = (size_t)(h - q) + 12;
        memcpy(out + n, q, lead);
        n += lead;
        const char *a = h + 12, *e = a;
        for (int d = 0; *e && (d > 0 || *e != ')'); e++) d += *e == '(' ? 1 : *e == ')' ? -1 : 0;
        /* (the words at the top level: what starts like a number is a length) */
        const char *col = NULL, *col_e = NULL;
        size_t lens = 0;
        char lenbuf[256];
        for (const char *w = a; w < e; ) {
            while (w < e && *w == ' ') w++;
            const char *x = w;
            for (int d = 0; x < e && (d > 0 || *x != ' '); x++) d += *x == '(' ? 1 : *x == ')' ? -1 : 0;
            if (x == w) break;
            bool len = strchr("0123456789+-.", *w) || !strncmp(w, "calc(", 5);
            if (len && lens + (size_t)(x - w) + 2 < sizeof lenbuf) {
                if (lens) lenbuf[lens++] = ' ';
                memcpy(lenbuf + lens, w, (size_t)(x - w));
                lens += (size_t)(x - w);
            } else if (!len) { col = w; col_e = x; }
            w = x;
        }
        lenbuf[lens] = 0;
        size_t cl = col ? (size_t)(col_e - col) : (cur ? strlen(cur) : 0);
        if (n + cl + lens + (size_t)strlen(e) + 8 > cap) { free(out); return r; }
        if (cl) { memcpy(out + n, col ? col : cur, cl); n += cl; out[n++] = ' '; }
        memcpy(out + n, lenbuf, lens);
        n += lens;
        q = e;
    }
    size_t rest = strlen(q);
    if (n + rest + 1 > cap) { free(out); return r; }
    memcpy(out + n, q, rest + 1);
    free(r);
    return out;
}

/* A position's x- and y- words as sides, on this box's flow. */
static char *position_xy_sides(JSContextHandle ctx, ComputedStyle *cs, const char *v)
{
    const char *wm = computed_of(ctx, cs, "writing-mode"), *dir = computed_of(ctx, cs, "direction");
    bool rtl = dir && !strcmp(dir, "rtl");
    bool horiz = !wm || !strcmp(wm, "horizontal-tb");
    bool lr = wm && (!strcmp(wm, "vertical-lr") || !strcmp(wm, "sideways-lr"));
    bool slr = wm && !strcmp(wm, "sideways-lr");
    /* (the block axis starts where the mode says; the inline axis by direction) */
    bool x_rev = horiz ? rtl : !lr;
    bool y_rev = horiz ? false : (slr ? !rtl : rtl);
    size_t cap = strlen(v) * 2 + 16, o = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    for (const char *q = v; *q; ) {
        const char *rep = NULL;
        size_t skip = 0;
        if (!strncmp(q, "x-start", 7)) { rep = x_rev ? "right" : "left"; skip = 7; }
        else if (!strncmp(q, "x-end", 5)) { rep = x_rev ? "left" : "right"; skip = 5; }
        else if (!strncmp(q, "y-start", 7)) { rep = y_rev ? "bottom" : "top"; skip = 7; }
        else if (!strncmp(q, "y-end", 5)) { rep = y_rev ? "top" : "bottom"; skip = 5; }
        if (rep) { size_t l = strlen(rep); memcpy(out + o, rep, l); o += l; q += skip; }
        else out[o++] = *q++;
    }
    out[o] = 0;
    return out;
}

static char *compute_value(JSContextHandle ctx, ComputedStyle *cs, const CssPropDef *p, const char *v) {
    const char *n = p->name;
    if (!strcmp(n, "object-position") && (strstr(v, "x-start") || strstr(v, "x-end") || strstr(v, "y-start") || strstr(v, "y-end"))) {
        char *sides = position_xy_sides(ctx, cs, v);
        if (sides) {
            char *r = compute_value(ctx, cs, p, sides);
            free(sides);
            return r;
        }
    }
    if (!strcmp(n, "font-size")) {
        if (cs->used.font_size > 0) return px(cs->used.font_size);
        /* no styled box: from the declaration and the parent's size */
        ComputedStyle *par = parent_of(ctx, cs);
        double pf = par ? font_px(ctx, par) : 16;
        static const struct { const char *k; double px; } sizes[] = {
            { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 }, { "large", 18 },
            { "x-large", 24 }, { "xx-large", 32 }, { "xxx-large", 48 },
        };
        for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++)
            if (!strcmp(v, sizes[k].k)) return px(sizes[k].px);
        if (!strcmp(v, "larger")) return px(pf * 1.2);
        if (!strcmp(v, "smaller")) return px(pf / 1.2);
        if (!strcmp(v, "math")) return px(pf);
        if (strchr(v, '%') && !strchr(v, '(')) return px(pf * atof(v) / 100);
    }
    if (!strcmp(n, "font-weight")) {
        int w;
        if (!strcmp(v, "bolder") || !strcmp(v, "lighter")) {
            ComputedStyle *par = parent_of(ctx, cs);
            int pw = par ? atoi(computed_of(ctx, par, "font-weight")) : 400;
            if (!strcmp(v, "bolder")) w = pw < 350 ? 400 : pw < 550 ? 700 : pw < 900 ? 900 : pw;
            else w = pw < 100 ? pw : pw < 550 ? 100 : pw < 750 ? 400 : 700;
        } else if (strchr(v, '(')) {
            CssComputeEnv we;
            env_of(ctx, cs, p, v, &we);
            char *wr = css_prop_compute(p, v, &we);
            if (wr) return wr;
            w = 400;
        } else w = weight_num(v);
        char tmp[16];
        snprintf(tmp, sizeof tmp, "%d", w);
        return strdup(tmp);
    }
    if (!strcmp(n, "line-height") && strchr(v, '%') && !strchr(v, '(')) {
        double pct = atof(v);
        return px(pct / 100 * font_px(ctx, cs));
    }
    CssComputeEnv env;
    env_of(ctx, cs, p, v, &env);
    char *r = css_prop_compute(p, v, &env);
    if (!r) return strdup(v);
    if (!strcmp(n, "filter") || !strcmp(n, "backdrop-filter")) return filter_shadows(ctx, cs, r);
    if (!strcmp(n, "overflow-clip-margin") && strchr(r, '-')) {
        /* (the box first; padding-box is where it starts anyway; no margin needn't be said) */
        char a[64] = "", c[64] = "";
        sscanf(r, "%63s %63s", a, c);
        const char *box = !strcmp(a, "content-box") || !strcmp(a, "border-box") || !strcmp(a, "padding-box") ? a : c;
        const char *len = box == a ? c : a;
        char t[140];
        bool none = !len[0] || !strcmp(len, "0px");
        bool pb = !strcmp(box, "padding-box") || !box[0];
        snprintf(t, sizeof t, "%s%s%s", pb ? "" : box, !pb && !none ? " " : "", none ? (pb ? "0px" : "") : len);
        free(r);
        return strdup(t);
    }
    /* (a list of them, as in gap decorations) */
    if ((!strcmp(n, "column-rule-width") || !strcmp(n, "row-rule-width")) && (strchr(r, ',') || strchr(r, '('))) {
        static const struct { const char *kw, *px; } lw[] = { { "thin", "1px" }, { "medium", "3px" }, { "thick", "5px" } };
        for (size_t k = 0; k < 3; k++) {
            size_t kl = strlen(lw[k].kw);
            for (char *q; (q = strstr(r, lw[k].kw)); ) {
                char *t = malloc(strlen(r) + 8);
                memcpy(t, r, (size_t)(q - r));
                strcpy(t + (q - r), lw[k].px);
                strcat(t, q + kl);
                free(r);
                r = t;
            }
        }
    }
    /* line widths: thin, medium, thick */
    if (strstr(n, "-width") && (strstr(n, "border") || strstr(n, "outline") || strstr(n, "rule-width"))) {
        const char *kw = !strcmp(r, "thin") ? "1px" : !strcmp(r, "medium") ? "3px" : !strcmp(r, "thick") ? "5px" : NULL;
        if (kw) { free(r); r = strdup(kw); }
    }
    return r;
}

/* (computing for Typed OM: js_computed_unresolved) */
static bool g_cs_typed;

static bool display_blockified(JSContextHandle ctx, ComputedStyle *cs) {
    const char *pos = computed_of(ctx, cs, "position");
    if (!strcmp(pos, "absolute") || !strcmp(pos, "fixed")) return true;
    if (strcmp(computed_of(ctx, cs, "float"), "none")) return true;
    NodeId p = dom_parent(cs->el);
    return p && dom_kind(p) == DOM_K_DOCUMENT;     /* the root element */
}

/* ---- a transform list as the matrix it comes to (CSSOM: its resolved value) ---- */
static void m4_mul(double a[16], const double b[16])
{
    double r[16];
    for (int c = 0; c < 4; c++)
        for (int w = 0; w < 4; w++) {
            double v = 0.0;
            for (int k = 0; k < 4; k++) v += a[k * 4 + w] * b[c * 4 + k];
            r[c * 4 + w] = v;
        }
    memcpy(a, r, sizeof r);
}

static double tf_len(const char *t, double base)
{
    /* (a sum, as calc() computes to: each part of it) */
    if (!strncmp(t, "calc(", 5)) {
        double sum = 0.0, sign = 1.0;
        for (const char *q = t + 5; *q && *q != ')'; ) {
            if (*q == ' ') { q++; continue; }
            if ((*q == '-' || *q == '+') && q[1] == ' ') { sign = *q == '-' ? -1.0 : 1.0; q += 2; continue; }
            char *e = NULL;
            double v = strtod(q, &e);
            if (e == q) break;
            sum += sign * (*e == '%' ? v * base / 100.0 : v);
            sign = 1.0;
            q = e;
            while (*q && *q != ' ' && *q != ')') q++;
        }
        return sum;
    }
    char *e = NULL;
    double v = strtod(t, &e);
    if (e && *e == '%') return v * base / 100.0;
    return v;
}

static double tf_ang(const char *t)
{
    char *e = NULL;
    double v = strtod(t, &e);
    /* (an angle with no end is none) */
    if (!isfinite(v) || fabs(v) >= 1e30) return 0.0;
    if (!e) return v * 3.14159265358979323846 / 180.0;
    if (!strncmp(e, "rad", 3)) return v;
    if (!strncmp(e, "grad", 4)) return v * 3.14159265358979323846 / 200.0;
    if (!strncmp(e, "turn", 4)) return v * 2.0 * 3.14159265358979323846;
    return v * 3.14159265358979323846 / 180.0;
}

/* NULL when it can't be read (calc() left in it, say). */
static char *transform_matrix(const char *list, double w, double h)
{
    double m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    const char *p = list;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *open = strchr(p, '(');
        if (!open) return NULL;
        char name[24];
        size_t nl = (size_t)(open - p);
        if (nl >= sizeof name) return NULL;
        for (size_t i = 0; i < nl; i++) name[i] = (char)tolower((unsigned char)p[i]);
        name[nl] = 0;
        const char *close = open + 1;
        for (int d = 0; *close && (d > 0 || *close != ')'); close++) d += *close == '(' ? 1 : *close == ')' ? -1 : 0;
        if (!*close) return NULL;
        /* (inside only the translations take sums of lengths and percentages) */
        const char *nest = memchr(open + 1, '(', (size_t)(close - open - 1));
        if (nest && (strncmp(name, "translate", 9) || strncmp(nest - 4, "calc", 4))) return NULL;
        char args[16][96];
        int na = 0;
        for (const char *a = open + 1; a < close && na < 16; ) {
            while (a < close && (*a == ' ' || *a == ',')) a++;
            const char *e = a;
            for (int d = 0; e < close && (d > 0 || (*e != ' ' && *e != ',')); e++) d += *e == '(' ? 1 : *e == ')' ? -1 : 0;
            if (e > a) {
                if ((size_t)(e - a) >= sizeof args[0]) return NULL;
                memcpy(args[na], a, (size_t)(e - a));
                args[na++][e - a] = 0;
            }
            a = e;
        }
        double f[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        #define ARG(i) (i < na ? args[i] : "0")
        if (!strcmp(name, "matrix") && na == 6) {
            f[0] = atof(args[0]); f[1] = atof(args[1]); f[4] = atof(args[2]); f[5] = atof(args[3]); f[12] = atof(args[4]); f[13] = atof(args[5]);
        } else if (!strcmp(name, "matrix3d") && na == 16) {
            for (int i = 0; i < 16; i++) f[i] = atof(args[i]);
        } else if (!strcmp(name, "translate")) { f[12] = tf_len(ARG(0), w); f[13] = na > 1 ? tf_len(args[1], h) : 0.0; }
        else if (!strcmp(name, "translatex")) f[12] = tf_len(ARG(0), w);
        else if (!strcmp(name, "translatey")) f[13] = tf_len(ARG(0), h);
        else if (!strcmp(name, "translatez")) f[14] = tf_len(ARG(0), 0.0);
        else if (!strcmp(name, "translate3d")) { f[12] = tf_len(ARG(0), w); f[13] = tf_len(ARG(1), h); f[14] = tf_len(ARG(2), 0.0); }
        else if (!strcmp(name, "scale")) { f[0] = atof(ARG(0)); f[5] = na > 1 ? atof(args[1]) : f[0]; }
        else if (!strcmp(name, "scalex")) f[0] = atof(ARG(0));
        else if (!strcmp(name, "scaley")) f[5] = atof(ARG(0));
        else if (!strcmp(name, "scalez")) f[10] = atof(ARG(0));
        else if (!strcmp(name, "scale3d")) { f[0] = atof(ARG(0)); f[5] = atof(ARG(1)); f[10] = atof(ARG(2)); }
        else if (!strcmp(name, "rotate") || !strcmp(name, "rotatez")) { double a = tf_ang(ARG(0)); f[0] = cos(a); f[1] = sin(a); f[4] = -sin(a); f[5] = cos(a); }
        else if (!strcmp(name, "rotatex")) { double a = tf_ang(ARG(0)); f[5] = cos(a); f[6] = sin(a); f[9] = -sin(a); f[10] = cos(a); }
        else if (!strcmp(name, "rotatey")) { double a = tf_ang(ARG(0)); f[0] = cos(a); f[2] = -sin(a); f[8] = sin(a); f[10] = cos(a); }
        else if (!strcmp(name, "rotate3d") && na == 4) {
            double x = atof(args[0]), y = atof(args[1]), z = atof(args[2]), a = tf_ang(args[3]), l = sqrt(x * x + y * y + z * z);
            if (l > 0.0) {
                x /= l; y /= l; z /= l;
                double c = cos(a), s = sin(a), t = 1.0 - c;
                f[0] = t * x * x + c; f[1] = t * x * y + s * z; f[2] = t * x * z - s * y;
                f[4] = t * x * y - s * z; f[5] = t * y * y + c; f[6] = t * y * z + s * x;
                f[8] = t * x * z + s * y; f[9] = t * y * z - s * x; f[10] = t * z * z + c;
            }
        } else if (!strcmp(name, "skew")) { f[4] = tan(tf_ang(ARG(0))); f[1] = na > 1 ? tan(tf_ang(args[1])) : 0.0; }
        else if (!strcmp(name, "skewx")) f[4] = tan(tf_ang(ARG(0)));
        else if (!strcmp(name, "skewy")) f[1] = tan(tf_ang(ARG(0)));
        else if (!strcmp(name, "perspective")) {
            if (!(na > 0 && !strcmp(args[0], "none"))) {
                double d = tf_len(ARG(0), 0.0);
                f[11] = -1.0 / (d < 1.0 ? 1.0 : d);
            }
        } else return NULL;
        #undef ARG
        m4_mul(m, f);
        p = close + 1;
    }
    char out[400];
    bool flat = m[2] == 0 && m[3] == 0 && m[6] == 0 && m[7] == 0 && m[8] == 0 && m[9] == 0 && m[10] == 1 && m[11] == 0 && m[14] == 0 && m[15] == 1;
    for (int i = 0; i < 16; i++) if (fabs(m[i]) < 1e-9) m[i] = 0.0;
    if (flat) snprintf(out, sizeof out, "matrix(%.6g, %.6g, %.6g, %.6g, %.6g, %.6g)", m[0], m[1], m[4], m[5], m[12], m[13]);
    else {
        size_t n = (size_t)snprintf(out, sizeof out, "matrix3d(");
        for (int i = 0; i < 16; i++) n += (size_t)snprintf(out + n, sizeof out - n, "%.6g%s", m[i], i < 15 ? ", " : ")");
    }
    return strdup(out);
}

/* What the properties script animates last read as, by element: a transition
 * starts from it, when the change comes before the element has one. */
#define BEFORE_N 8192
static struct { NodeId el; const CssPropDef *p; char *v, *spec; bool held; } g_before[BEFORE_N];

/* (and what it was declared as: sizes interpolate by that) */
static char *before_swap(NodeId el, const CssPropDef *p, const char *now, const char *spec, char **old_spec)
{
    size_t k = (((size_t)el * 2654435761u) ^ ((size_t)p >> 4)) % BEFORE_N;
    bool same = g_before[k].el == el && g_before[k].p == p;
    char *old = same ? g_before[k].v : NULL;
    if (!old) free(g_before[k].v);
    if (old_spec && same) *old_spec = g_before[k].spec;
    else free(g_before[k].spec);
    if (!same) g_before[k].held = false;
    g_before[k].el = el;
    g_before[k].p = p;
    g_before[k].v = strdup(now ? now : "");
    g_before[k].spec = spec ? strdup(spec) : NULL;
    return old;
}

/* (script is running this one: it stays with script until that's over, whatever
 * the last read saw) */
static bool before_held(NodeId el, const CssPropDef *p)
{
    size_t k = (((size_t)el * 2654435761u) ^ ((size_t)p >> 4)) % BEFORE_N;
    return g_before[k].el == el && g_before[k].p == p && g_before[k].held;
}
static void before_hold(NodeId el, const CssPropDef *p, bool on)
{
    size_t k = (((size_t)el * 2654435761u) ^ ((size_t)p >> 4)) % BEFORE_N;
    if (g_before[k].el == el && g_before[k].p == p) g_before[k].held = on;
}

/* A size that takes more than a number to interpolate. */
static bool size_spec_special(const char *s)
{
    if (!s) return true;
    if (strstr(s, "calc-size(")) return true;
    /* (a percentage: what it comes to is the layout's) */
    if (strchr(s, '%')) return true;
    static const char *const kw[] = { "auto", "none", "min-content", "max-content", "fit-content", "stretch", "initial", "unset", "inherit", "revert", "revert-layer" };
    for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) if (!strcasecmp(s, kw[i])) return true;
    return false;
}

/* A color that isn't rgb(), rgba(), hsl(), hwb() or a name. */
static bool color_spec_wide(const char *s)
{
    return s && (strstr(s, "color(") || strstr(s, "lab(") || strstr(s, "lch(") || strstr(s, "color-mix("));
}

static bool before_color_wide(NodeId el, const CssPropDef *p)
{
    size_t k = (((size_t)el * 2654435761u) ^ ((size_t)p >> 4)) % BEFORE_N;
    return g_before[k].el == el && g_before[k].p == p && color_spec_wide(g_before[k].spec);
}

/* Whether what el's size was last read as was such a size. */
static bool before_pct(NodeId el, const CssPropDef *p)
{
    size_t k = (((size_t)el * 2654435761u) ^ ((size_t)p >> 4)) % BEFORE_N;
    return g_before[k].el == el && g_before[k].p == p && g_before[k].spec &&
           (strchr(g_before[k].spec, '%') || !strcasecmp(g_before[k].spec, "auto"));
}

static bool before_size_special(NodeId el, const CssPropDef *p)
{
    size_t k = (((size_t)el * 2654435761u) ^ ((size_t)p >> 4)) % BEFORE_N;
    return g_before[k].el == el && g_before[k].p == p && size_spec_special(g_before[k].spec);
}

static int g_in_hook, g_in_calc;

/* A new document: nothing read of the last one's elements counts. */
void computed_style_reset(void)
{
    extern int css_layout_take_restyled(NodeId *out, int cap);
    extern bool g_css_saw_transition;
    NodeId none[1];
    css_layout_take_restyled(none, 0);
    g_css_saw_transition = false;
    for (int i = 0; i < BEFORE_N; i++) {
        free(g_before[i].v);
        g_before[i].v = NULL;
        free(g_before[i].spec);
        g_before[i].spec = NULL;
        g_before[i].el = 0;
        g_before[i].p = NULL;
    }
}

static bool g_frame_pass;
void computed_style_after_restyle(JSContextHandle ctx);

/* A style pass styled these elements: of what was read of them, what it comes
 * to now, and a transition from what it was (that is when they start). */
/* A frame's own style pass: only of what the engine doesn't run itself, and the
 * list stays for the forced layout that sees to the rest. */
void computed_style_after_frame(JSContextHandle ctx)
{
    extern unsigned g_anim_restyled_gen;
    extern uint64_t g_anim_restyled_stamp;
    static unsigned seen_gen;
    if (seen_gen == g_anim_restyled_gen) return;
    /* (the document has moved on: reading it would lay out at this point) */
    if (dom_change_stamp() != g_anim_restyled_stamp) return;
    seen_gen = g_anim_restyled_gen;
    g_frame_pass = true;
    computed_style_after_restyle(ctx);
    g_frame_pass = false;
}

void computed_style_after_restyle(JSContextHandle ctx)
{
    extern int css_layout_take_restyled(NodeId *out, int cap);
    extern int css_layout_peek_restyled(NodeId *out, int cap);
    static NodeId els[1024];
    static uint64_t seen[64];
    int n = g_frame_pass ? css_layout_peek_restyled(els, 1024) : css_layout_take_restyled(els, 1024);
    if (!n || g_in_hook || g_in_calc) return;
    /* (a whole page restyled: as many as is reasonable, of those with transitions) */
    static int rot;
    int budget = n < 0 ? 48 : BEFORE_N;
    memset(seen, 0, sizeof seen);
    for (int k = 0; k < n; k++) seen[((uint32_t)els[k] * 2654435761u >> 20) >> 6] |= 1ull << (((uint32_t)els[k] * 2654435761u >> 20) & 63);
    GCValue hook = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "__cyberGenericAnim");
    g_in_hook++;
    /* (parents first: what comes down from a parent's transition starts none of its own) */
    static struct { int i, depth; } todo[BEFORE_N];
    int nt = 0;
    for (int w = 0; w < BEFORE_N && budget > 0; w++) {
        int i = (rot + w) % BEFORE_N;
        NodeId el = g_before[i].el;
        if (!el || !g_before[i].v) continue;
        if (g_frame_pass && anim_prop_of(g_before[i].p->name) >= 0) continue;
        bool hit = n < 0 && dom_valid(el) && layout_query_has_anim_spec(el);
        if (!hit && n >= 0) {
            uint32_t h = (uint32_t)el * 2654435761u >> 20;
            if (!(seen[h >> 6] & (1ull << (h & 63)))) continue;
            for (int k = 0; k < n && !hit; k++) hit = els[k] == el;
        }
        if (!hit || !dom_valid(el)) continue;
        budget--;
        if (n < 0) rot = (i + 1) % BEFORE_N;
        int depth = 0;
        for (NodeId a = dom_parent(el); a && depth < 4096; a = dom_parent(a)) depth++;
        int at = nt++;
        /* (an element's together: they share a style) */
        while (at > 0 && (todo[at - 1].depth > depth || (todo[at - 1].depth == depth && g_before[todo[at - 1].i].el > el))) { todo[at] = todo[at - 1]; at--; }
        todo[at].i = i;
        todo[at].depth = depth;
    }
    ComputedStyle *t = NULL;
    NodeId t_el = 0;
    for (int w = 0; w < nt; w++) {
        int i = todo[w].i;
        NodeId el = g_before[i].el;
        /* (a style of its own: a script's holds no values that skipped the transition) */
        if (t_el != el) {
            if (t) computed_style_free(t);
            t = computed_style_new(el);
            t_el = el;
            refresh(ctx, t);
        }
        const char *now = t->styled && t->spec ? computed_of(ctx, t, g_before[i].p->name) : NULL;
        if (now && strcmp(now, g_before[i].v)) {
            if (JS_IsFunction(ctx, hook) && (anim_has(el, 0) || layout_query_has_anim_spec(el))) {
                const char *spec_now = t->spec[prop_idx(g_before[i].p)];
                GCValue args[7] = { dom_js(ctx, el), JS_NewString(ctx, g_before[i].p->name), JS_NewString(ctx, now), JS_NewString(ctx, g_before[i].v), JS_FALSE,
                                    spec_now ? JS_NewString(ctx, spec_now) : JS_NULL,
                                    g_before[i].spec ? JS_NewString(ctx, g_before[i].spec) : JS_NULL };
                GCValue r = JS_Call(ctx, hook, JS_UNDEFINED, 7, args);
                if (JS_IsException(r)) JS_GetException(ctx);
            }
            free(g_before[i].v);
            g_before[i].v = strdup(now);
            free(g_before[i].spec);
            g_before[i].spec = t->spec[prop_idx(g_before[i].p)] ? strdup(t->spec[prop_idx(g_before[i].p)]) : NULL;
        }
    }
    if (t) computed_style_free(t);
    /* (what a transition names the engine doesn't run: from now on, what it
     * comes to is kept, for the next change to start from) */
    /* (a whole page restyled: those of them with a transition) */
    /* (on a page of modest size: a style each isn't for free) */
    int nreg = layout_query_box_count() > 600 ? 0 : n < 0 ? layout_query_anim_elements(els, 1024) : n;
    for (int k = 0, left = 64; k < nreg && left > 0; k++) {
        NodeId el = els[k];
        if (!dom_valid(el) || dom_kind(el) != DOM_K_ELEMENT) continue;
        const char *aspec = layout_query_anim_spec(el);
        if (!aspec) continue;
        /* (once for an element's list of them) */
        static struct { NodeId el; const char *spec; } looked[2048];
        size_t lk = ((size_t)el * 2654435761u) % 2048;
        if (looked[lk].el == el && looked[lk].spec == aspec) continue;
        looked[lk].el = el;
        looked[lk].spec = aspec;
        left--;
        t = computed_style_new(el);
        refresh(ctx, t);
        const char *names = t->styled && t->spec ? computed_of(ctx, t, "transition-property") : NULL;
        char word[64];
        /* (all: of what script runs, those that tend to be given) */
        static const char all_names[] = "min-width min-height max-width max-height line-height text-indent vertical-align "
            "outline-offset outline-width outline-color background-position-x background-position-y background-size "
            "border-top-left-radius border-top-right-radius border-bottom-left-radius border-bottom-right-radius "
            "border-top-color border-right-color border-bottom-color border-left-color column-gap row-gap column-width "
            "column-count column-rule-width column-rule-color column-height box-shadow text-shadow text-decoration-color "
            "text-decoration-inset object-view-box font-size-adjust font-stretch z-index flex-basis flex-grow flex-shrink order";
        if (names && !strcmp(names, "all")) names = all_names;
        for (const char *q = names; q && *q; ) {
            while (*q == ' ' || *q == ',') q++;
            size_t l = strcspn(q, " ,");
            if (!l) break;
            if (l < sizeof word) {
                memcpy(word, q, l);
                word[l] = 0;
                const CssPropDef *p = css_prop_find(word, l);
                /* (a shorthand names its longhands) */
                const char **lnames = NULL;
                const char *one = word;
                int ln = p && p->longhands ? css_prop_longhands(p, &lnames) : 1;
                for (int li = 0; p && li < ln; li++) {
                    const char *wn = p->longhands ? lnames[li] : one;
                    const CssPropDef *lp = p->longhands ? css_prop_find(wn, strlen(wn)) : p;
                    if (!lp || lp->longhands || anim_prop_of(wn) >= 0) continue;
                    size_t h = (((size_t)el * 2654435761u) ^ ((size_t)lp >> 4)) % BEFORE_N;
                    if (!(g_before[h].el == el && g_before[h].p == lp)) {
                        const char *cur = computed_of(ctx, t, wn);
                        char *old_spec = NULL;
                        free(before_swap(el, lp, cur, t->spec[prop_idx(lp)], &old_spec));
                        free(old_spec);
                    }
                }
            }
            q += l;
        }
        computed_style_free(t);
    }
    g_in_hook--;
}

/* How many tracks [s, e) of a track list says outright (no auto repeat in it). */
static int grid_tracks_said(const char *s, const char *e)
{
    int n = 0;
    while (s < e) {
        while (s < e && isspace((unsigned char)*s)) s++;
        if (s >= e) break;
        const char *q = s;
        int d = 0;
        while (q < e) {
            if (*q == '(' || *q == '[') d++;
            else if (*q == ')' || *q == ']') d--;
            else if (!d && isspace((unsigned char)*q)) break;
            q++;
        }
        if (!strncasecmp(s, "repeat(", 7)) {
            const char *c = memchr(s, ',', (size_t)(q - s));
            if (c) n += atoi(s + 7) * grid_tracks_said(c + 1, q - 1);
        } else if (*s != '[') n++;
        s = q;
    }
    return n;
}

/* grid-template-*: the used tracks, with the line names written. */
static void grid_tracks_walk(const char *s, const char *e, const double *tr, int nt, int *at, char *out, size_t cap, size_t *o, bool *sp)
{
    #define GT_PUT(txt, len) do { size_t l_ = (len); if (*o + l_ + 2 < cap) { if (*sp) out[(*o)++] = ' '; memcpy(out + *o, (txt), l_); *o += l_; *sp = true; } } while (0)
    while (s < e) {
        while (s < e && isspace((unsigned char)*s)) s++;
        if (s >= e) break;
        const char *q = s;
        int d = 0;
        while (q < e) {
            if (*q == '(' || *q == '[') d++;
            else if (*q == ')' || *q == ']') d--;
            else if (!d && isspace((unsigned char)*q)) break;
            q++;
        }
        if (*s == '[') {
            /* (no names: nothing to show) */
            const char *ne = s + 1;
            while (ne < q && (isspace((unsigned char)*ne))) ne++;
            if (*ne != ']') GT_PUT(s, (size_t)(q - s));
        } else if (!strncasecmp(s, "repeat(", 7)) {
            const char *c = memchr(s, ',', (size_t)(q - s));
            if (c) {
                int times = atoi(s + 7);
                bool aut = !times;
                /* (an auto repeat leaves the tracks after it for what says them) */
                int after = aut ? grid_tracks_said(q, e) : 0;
                for (int k = 0; (aut ? *at + after < nt : k < times) && k < 2000; k++) {
                    int was = *at;
                    grid_tracks_walk(c + 1, q - 1, tr, nt, at, out, cap, o, sp);
                    if (*at == was) break;
                }
            }
        } else if (*at < nt) {
            char b[40];
            snprintf(b, sizeof b, "%.6gpx", tr[(*at)++]);
            GT_PUT(b, strlen(b));
        }
        s = q;
    }
    #undef GT_PUT
}

static char *grid_tracks_resolved(const char *spec, const double *tr, int nt, int lead)
{
    char out[4096];
    size_t o = 0;
    bool sp = false;
    int at = 0;
    /* (implicit tracks before the explicit ones) */
    while (at < lead && at < nt && o + 48 < sizeof out) {
        if (sp) out[o++] = ' ';
        o += (size_t)snprintf(out + o, sizeof out - o, "%.6gpx", tr[at++]);
        sp = true;
    }
    tr += at;
    nt -= at;
    at = 0;
    if (strcasecmp(spec, "none")) grid_tracks_walk(spec, spec + strlen(spec), tr, nt, &at, out, sizeof out, &o, &sp);
    while (at < nt && o + 48 < sizeof out) {
        if (sp) out[o++] = ' ';
        o += (size_t)snprintf(out + o, sizeof out - o, "%.6gpx", tr[at++]);
        sp = true;
    }
    out[o] = 0;
    /* (names side by side are one list) */
    for (char *q; (q = strstr(out, "] [")); ) memmove(q, q + 2, strlen(q + 2) + 1), *q = ' ';
    return strdup(o ? out : "none");
}

static int g_comp_base;
/* Whether s's anchor functions can find an anchor here: only an absolutely
 * positioned box has one, and without a name, only by its position-anchor. */
static bool anchor_fns_valid(JSContextHandle ctx, ComputedStyle *cs, const char *s)
{
    const char *pos = computed_of(ctx, cs, "position");
    if (strcmp(pos, "absolute") && strcmp(pos, "fixed")) return false;
    const char *pa = computed_of(ctx, cs, "position-anchor");
    bool has_default = pa && strcmp(pa, "auto") && strcmp(pa, "none") && pa[0];
    for (const char *f = s; (f = strstr(f, "anchor")); f++) {
        const char *open = strchr(f, '(');
        if (!open || (f > s && (isalnum((unsigned char)f[-1]) || f[-1] == '-'))) continue;
        const char *a = open + 1;
        while (*a == ' ') a++;
        /* (a side or size without a name of its own) */
        bool named = a[0] == '-' && a[1] == '-';
        if (!named && !has_default) return false;
    }
    return true;
}

/* s with each anchor function as its fallback (malloc'd); NULL if one has none. */
static char *anchor_fallbacks(const char *s)
{
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; ) {
        bool fn = (!strncmp(s + i, "anchor(", 7) || !strncmp(s + i, "anchor-size(", 12)) &&
                  (i == 0 || !(isalnum((unsigned char)s[i - 1]) || s[i - 1] == '-'));
        if (!fn) { out[o++] = s[i++]; continue; }
        const char *open = strchr(s + i, '(');
        int depth = 0;
        const char *comma = NULL, *close = NULL;
        for (const char *c = open; *c; c++) {
            if (*c == '(') depth++;
            else if (*c == ')' && --depth == 0) { close = c; break; }
            else if (*c == ',' && depth == 1 && !comma) comma = c;
        }
        if (!close || !comma) { free(out); return NULL; }
        const char *fb = comma + 1;
        while (*fb == ' ') fb++;
        size_t fl = (size_t)(close - fb);
        while (fl && fb[fl - 1] == ' ') fl--;
        memcpy(out + o, fb, fl);
        o += fl;
        i = (size_t)(close - s) + 1;
    }
    out[o] = 0;
    return out;
}

static char *compute(JSContextHandle ctx, ComputedStyle *cs, const CssPropDef *p) {
    CST("compute %s", p->name);
    if (!cs->styled) return strdup("");
    {
        char phys[64];
        if (css_logical_physical(p->name, cs->used.writing_mode, cs->used.rtl, phys, sizeof phys))
            return strdup(computed_of(ctx, cs, phys));
    }
    int i = prop_idx(p);
    const char *s = cs->spec[i];
    const char *n = p->name;
    /* (an element's default display is the user agent's: what layout gave it) */
    if (!s && !strcmp(n, "display") && cs->used.display) s = cs->used.display;
    /* (anchor(): the number it came to; none: auto) */
    if (s && !cs->pseudo && strstr(s, "anchor") && (!strcmp(n, "top") || !strcmp(n, "right") || !strcmp(n, "bottom") || !strcmp(n, "left"))) {
        double v = 0.0;
        static __thread char anchor_px[48];
        int side = n[0] == 't' ? 0 : n[0] == 'r' ? 1 : n[0] == 'b' ? 2 : 3;
        if (layout_query_anchor_inset(cs->el, side, &v)) {
            snprintf(anchor_px, sizeof anchor_px, "%.6gpx", v);
            s = anchor_px;
        } else {
            s = "auto";
        }
    }
    /* (a pseudo-element's math is flow: only math elements have its layout) */
    if (s && cs->pseudo && !strcmp(n, "display") && strstr(s, "math")) s = strstr(s, "block") ? "block" : "inline";
    if (!s || !strcmp(s, "unset") || !strcmp(s, "revert") || !strcmp(s, "revert-layer"))
        s = p->inherited ? "inherit" : "initial";
    char *r;
    if (!strcmp(s, "inherit")) {
        ComputedStyle *par = parent_of(ctx, cs);
        r = par && par->styled ? strdup(computed_of(ctx, par, n)) : compute_value(ctx, cs, p, p->initial);
        /* (an inherited font-weight etc. is the parent's computed value) */
    } else {
        /* (anchor functions with nothing to anchor to: their fallbacks, or the property's start) */
        char *swapped = NULL;
        if ((strstr(s, "anchor(") || strstr(s, "anchor-size(")) && !anchor_fns_valid(ctx, cs, s)) {
            swapped = anchor_fallbacks(s);
            if (!swapped) s = "initial";
        }
        r = compute_value(ctx, cs, p, !strcmp(s, "initial") ? p->initial : swapped ? swapped : s);
        free(swapped);
        /* (Typed OM's computed value: currentcolor stays itself) */
        if (g_cs_typed && r && !strcasecmp(s, "currentcolor") && strcmp(n, "color")) { free(r); r = strdup("currentcolor"); }
    }
    /* the properties whose computed value depends on another one (not Typed OM's) */
    if (strstr(n, "-width") && !strncmp(n, "border-", 7) && !strstr(n, "image")) {
        char style[64];
        snprintf(style, sizeof style, "%.*sstyle", (int)(strlen(n) - 5), n);
        const char *st = g_cs_typed ? "" : computed_of(ctx, cs, style);
        if (!strcmp(st, "none") || !strcmp(st, "hidden")) { free(r); r = strdup("0px"); }
    } else if (!strcmp(n, "outline-width")) {
        if (!g_cs_typed && !strcmp(computed_of(ctx, cs, "outline-style"), "none")) { free(r); r = strdup("0px"); }
    } else if (!strcmp(n, "column-rule-width") || !strcmp(n, "row-rule-width")) {
        const char *st = computed_of(ctx, cs, n[0] == 'c' ? "column-rule-style" : "row-rule-style");
        if (!g_cs_typed && (!strcmp(st, "none") || !strcmp(st, "hidden"))) { free(r); r = strdup("0px"); }
    }
    /* (a line's width, and outline-offset's size, in whole device pixels: one at least) */
    if ((strstr(n, "-width") && (!strncmp(n, "border-", 7) || !strncmp(n, "outline", 7) || !strcmp(n, "column-rule-width") || !strcmp(n, "row-rule-width")) &&
         !strstr(n, "image")) || !strcmp(n, "outline-offset")) {
        char *e = NULL;
        double v = r ? strtod(r, &e) : 0.0;
        if (r && e != r && !strcmp(e, "px") && v != 0.0 && !anim_has(cs->el, 0)) {
            double m = fabs(v), snapped = m < 1.0 ? 1.0 : floor(m);
            if (snapped != m) {
                char b[48];
                snprintf(b, sizeof b, "%gpx", v < 0 ? -snapped : snapped);
                free(r);
                r = strdup(b);
            }
        }
    }
    if (0) {
    } else if (!strcmp(n, "float")) {
        const char *pos = computed_of(ctx, cs, "position");
        if (!strcmp(pos, "absolute") || !strcmp(pos, "fixed")) { free(r); r = strdup("none"); }
    } else if (!strcmp(n, "display") && display_blockified(ctx, cs)) {
        static const char *const map[][2] = {
            { "inline", "block" }, { "inline-block", "block" }, { "run-in", "block" }, { "inline-flex", "flex" },
            { "inline-grid", "grid" }, { "inline-table", "table" }, { "table-row-group", "block" },
            { "table-column", "block" }, { "table-column-group", "block" }, { "table-header-group", "block" },
            { "table-footer-group", "block" }, { "table-row", "block" }, { "table-cell", "block" },
            { "table-caption", "block" }, { "inline list-item", "block list-item" }, { "-webkit-inline-box", "-webkit-box" },
        };
        for (size_t k = 0; k < sizeof map / sizeof map[0]; k++)
            if (!strcmp(r, map[k][0])) { free(r); r = strdup(map[k][1]); break; }
    }
    /* resolved values: a rendered box's used sizes, margins and paddings */
    if (cs->used.rendered) {
        static const char *const sides[] = { "top", "right", "bottom", "left" };
        bool size = !strcmp(n, "width") || !strcmp(n, "height");
        bool inline_box = size && !strcmp(computed_of(ctx, cs, "display"), "inline");
        if (!strcmp(n, "width") && !inline_box) { free(r); r = px(cs->used.content_width); }
        else if (!strcmp(n, "height") && !inline_box) { free(r); r = px(cs->used.content_height); }
        for (int k = 0; k < 4; k++) {
            char m[32], pd[32];
            snprintf(m, sizeof m, "margin-%s", sides[k]);
            snprintf(pd, sizeof pd, "padding-%s", sides[k]);
            if (!strcmp(n, m)) { free(r); r = px(cs->used.margin[k]); }
            else if (!strcmp(n, pd)) { free(r); r = px(cs->used.padding[k]); }
        }
    }
    /* (a grid's tracks: as they came out) */
    /* (none stays none, implicit tracks or not) */
    if (cs->used.rendered && !cs->pseudo && r && (!strcmp(n, "grid-template-columns") || !strcmp(n, "grid-template-rows")) &&
        !strstr(r, "subgrid") && s && strcmp(s, "none")) {
        const char *dsp = computed_of(ctx, cs, "display");
        if (!strcmp(dsp, "grid") || !strcmp(dsp, "inline-grid") || strstr(dsp, "grid-lanes")) {
            static double tracks[2000];
            /* (its tracks are in its flow's axes) */
            bool rows_asked = n[14] == 'r';
            if (cs->used.writing_mode != 0) rows_asked = !rows_asked;
            int nt = layout_query_grid_tracks(cs->el, rows_asked, tracks, 2000);
            /* (no tracks but the one an empty grid has: none) */
            if (nt >= 0 && !(!strcmp(r, "none") && (nt == 0 || (nt == 1 && tracks[0] == 0.0)))) {
                char *g = grid_tracks_resolved(r, tracks, nt, layout_query_grid_lead(cs->el, rows_asked));
                free(r);
                r = g;
            }
        }
    }
    /* (a background's position along one axis: a length or percentage) */
    if (r && !strchr(r, '(') && (!strcmp(n, "background-position-x") || !strcmp(n, "background-position-y") ||
                                 !strcmp(n, "mask-position-x") || !strcmp(n, "mask-position-y")) &&
        (strstr(r, "left") || strstr(r, "right") || strstr(r, "top") || strstr(r, "bottom") || strstr(r, "center") || strstr(r, "x-") || strstr(r, "y-"))) {
        char out[512], *o = out;
        const char *q = r;
        while (*q && o < out + sizeof out - 48) {
            const char *e = strchr(q, ',');
            size_t l = e ? (size_t)(e - q) : strlen(q);
            char item[96];
            snprintf(item, sizeof item, "%.*s", (int)l, q);
            char kw[24] = "", len[48] = "";
            sscanf(item, " %23s %47s", kw, len);
            bool end = !strcmp(kw, "right") || !strcmp(kw, "bottom") || !strcmp(kw, "x-end") || !strcmp(kw, "y-end");
            bool start = !strcmp(kw, "left") || !strcmp(kw, "top") || !strcmp(kw, "x-start") || !strcmp(kw, "y-start");
            if (!strcmp(kw, "center")) o += sprintf(o, "50%%");
            else if (start && len[0]) o += sprintf(o, "%s", len);
            else if (start) o += sprintf(o, "0%%");
            else if (end && !len[0]) o += sprintf(o, "100%%");
            else if (end) o += sprintf(o, "calc(100%% - %s)", len);
            else o += sprintf(o, "%s", kw);
            q = e ? e + 1 : q + l;
            if (e) o += sprintf(o, ", ");
        }
        *o = 0;
        free(r);
        r = strdup(out);
    }
    /* (an origin: in pixels, where there is a box) */
    if (r && cs->used.rendered && !cs->pseudo && (!strcmp(n, "transform-origin") || !strcmp(n, "perspective-origin")) &&
        (strchr(r, '%') || strstr(r, "left") || strstr(r, "right") || strstr(r, "top") || strstr(r, "bottom") || strstr(r, "center")) && !strchr(r, '(')) {
        double w = cs->used.content_width + cs->used.padding[1] + cs->used.padding[3] + cs->used.border[1] + cs->used.border[3];
        double h = cs->used.content_height + cs->used.padding[0] + cs->used.padding[2] + cs->used.border[0] + cs->used.border[2];
        char a0[48] = "", a1[48] = "", a2[48] = "";
        int got = sscanf(r, "%47s %47s %47s", a0, a1, a2);
        double v[2] = { 0.5 * w, 0.5 * h };
        const char *word[2] = { a0, got > 1 ? a1 : "center" };
        /* (a vertical keyword first: the other way round) */
        if (!strcmp(a0, "top") || !strcmp(a0, "bottom") || !strcmp(a1, "left") || !strcmp(a1, "right")) { word[0] = got > 1 ? a1 : "center"; word[1] = a0; }
        for (int k = 0; k < 2; k++) {
            const char *t = word[k];
            double span = k ? h : w;
            if (!strcmp(t, "center")) v[k] = span / 2.0;
            else if (!strcmp(t, "left") || !strcmp(t, "top")) v[k] = 0.0;
            else if (!strcmp(t, "right") || !strcmp(t, "bottom")) v[k] = span;
            else if (strchr(t, '%')) v[k] = atof(t) * span / 100.0;
            else v[k] = atof(t);
        }
        char out[160];
        if (got > 2 && atof(a2) != 0.0) snprintf(out, sizeof out, "%.6gpx %.6gpx %s", v[0], v[1], a2);
        else snprintf(out, sizeof out, "%.6gpx %.6gpx", v[0], v[1]);
        free(r);
        r = strdup(out);
    }
    /* (a font's width: the percentage its keyword stands for) */
    if (r && (!strcmp(n, "font-stretch") || !strcmp(n, "font-width"))) {
        static const char *const fw[][2] = { { "ultra-condensed", "50%" }, { "extra-condensed", "62.5%" }, { "condensed", "75%" },
            { "semi-condensed", "87.5%" }, { "normal", "100%" }, { "semi-expanded", "112.5%" }, { "expanded", "125%" },
            { "extra-expanded", "150%" }, { "ultra-expanded", "200%" } };
        for (size_t k = 0; k < 9; k++) if (!strcmp(r, fw[k][0])) { free(r); r = strdup(fw[k][1]); break; }
    }
    /* (keywords of a corner's shape: the superellipse they are) */
    if (r && !strncmp(n, "corner-", 7) && strstr(n, "-shape")) {
        static const char *const kw[][2] = { { "round", "superellipse(1)" }, { "scoop", "superellipse(-1)" }, { "bevel", "superellipse(0)" },
            { "notch", "superellipse(-infinity)" }, { "square", "superellipse(infinity)" }, { "squircle", "superellipse(2)" } };
        for (size_t k = 0; k < 6; k++) if (!strcmp(r, kw[k][0])) { free(r); r = strdup(kw[k][1]); break; }
    }
    /* (a threshold is 0 to 1) */
    if (r && !strcmp(n, "shape-image-threshold")) {
        double t = atof(r);
        if (t < 0.0 || t > 1.0) { free(r); r = strdup(t < 0.0 ? "0" : "1"); }
    }
    /* (calc-size(): what it comes to, from what its basis would) */
    bool has_calc_size = s && strstr(s, "calc-size(");
    {
        #define in_calc g_in_calc
        bool sized = !strcmp(n, "width") || !strcmp(n, "height") || !strcmp(n, "min-width") || !strcmp(n, "min-height") ||
                     !strcmp(n, "max-width") || !strcmp(n, "max-height");
        /* (read in the middle of another's: its basis has none, so it doesn't go round) */
        if (sized && in_calc < 4 && !cs->pseudo && s && strstr(s, "calc-size(") && cs->used.rendered) {
            GCValue fn = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "__cyberCalcSize");
            if (JS_IsFunction(ctx, fn)) {
                GCValue args[3] = { dom_js(ctx, cs->el), JS_NewString(ctx, n), JS_NewString(ctx, s) };
                in_calc++;
                GCValue got = JS_Call(ctx, fn, JS_UNDEFINED, 3, args);
                in_calc--;
                /* (reading it there took the snapshot again, with what it tried in force:
                 * once more, now that's gone) */
                refresh(ctx, cs);
                s = cs->spec ? cs->spec[i] : NULL;
                if (JS_IsException(got)) JS_GetException(ctx);
                else if (JS_IsString(got)) {
                    const char *t = JS_ToCString(ctx, got);
                    if (t) { free(r); r = strdup(t); }
                }
            }
        }
        #undef in_calc
    }
    /* (scale in percentages: as the numbers they are, the last ones if they add nothing) */
    if (r && !strcmp(n, "scale") && strcmp(r, "none") && !strstr(r, "calc(")) {
        double v[3];
        int nv = 0;
        char *q = r, *e;
        while (nv < 3 && *q) {
            v[nv] = strtod(q, &e);
            if (e == q) { nv = -1; break; }
            if (*e == '%') { v[nv] /= 100.0; e++; }
            nv++;
            q = e;
            while (*q == ' ') q++;
        }
        if (nv > 0 && !*q) {
            if (nv == 3 && v[2] == 1.0) nv = 2;
            if (nv == 2 && v[0] == v[1]) nv = 1;
            char t[96];
            int o = 0;
            for (int k = 0; k < nv; k++) o += snprintf(t + o, sizeof t - (size_t)o, "%s%.6g", k ? " " : "", v[k]);
            free(r);
            r = strdup(t);
        }
    }
    /* (filter: amounts as the numbers they come to, each in its range) */
    if (r && (!strcmp(n, "filter") || !strcmp(n, "backdrop-filter")) && strcmp(r, "none")) {
        /* (left out: what does nothing) */
        static const struct { const char *e, *full; } empties[] = { { "blur()", "blur(0px)" }, { "hue-rotate()", "hue-rotate(0deg)" } };
        for (size_t k = 0; k < 2; k++) {
            char *at = strstr(r, empties[k].e);
            if (!at) continue;
            size_t pre = (size_t)(at - r), el = strlen(empties[k].e), fl = strlen(empties[k].full), tail = strlen(at + el);
            char *nr = (char *)malloc(pre + fl + tail + 1);
            if (!nr) break;
            memcpy(nr, r, pre);
            memcpy(nr + pre, empties[k].full, fl);
            memcpy(nr + pre + fl, at + el, tail + 1);
            free(r);
            r = nr;
        }
        static const struct { const char *f; bool capped; } am[] = {
            { "brightness(", false }, { "contrast(", false }, { "saturate(", false },
            { "grayscale(", true }, { "invert(", true }, { "opacity(", true }, { "sepia(", true } };
        for (size_t k = 0; k < sizeof am / sizeof am[0]; k++) {
            size_t fl = strlen(am[k].f);
            for (char *at = strstr(r, am[k].f); at; at = strstr(at + 1, am[k].f)) {
                char *open = at + fl, *close = open;
                int d = 1;
                while (*close && d) { if (*close == '(') d++; else if (*close == ')') d--; if (d) close++; }
                if (!*close) break;
                char arg[160];
                size_t al = (size_t)(close - open);
                if (al >= sizeof arg) continue;
                memcpy(arg, open, al);
                arg[al] = 0;
                double val = al ? css_length_value(arg, 1.0, 0.0, 16.0) : 1.0;
                if (al && strchr(arg, '%') && !strchr(arg, '(')) val = atof(arg) / 100.0;
                else if (al && !strchr(arg, '(') && !strchr(arg, '%')) val = atof(arg);
                if (val < 0.0) val = 0.0;
                if (am[k].capped && val > 1.0) val = 1.0;
                char num[48];
                int nl = snprintf(num, sizeof num, "%.6g", val);
                size_t pre = (size_t)(open - r), tail = strlen(close);
                char *nr = (char *)malloc(pre + (size_t)nl + tail + 1);
                if (!nr) break;
                memcpy(nr, r, pre);
                memcpy(nr + pre, num, (size_t)nl);
                memcpy(nr + pre + (size_t)nl, close, tail + 1);
                at = nr + (at - r);
                free(r);
                r = nr;
            }
        }
    }
    /* (shape(): the defaults of an arc go without saying) */
    if (r && strstr(r, "shape(") && (!strcmp(n, "clip-path") || !strcmp(n, "offset-path") || !strcmp(n, "shape-outside"))) {
        static const char *const words[] = { " ccw", " small" };
        for (int k = 0; k < 2; k++) {
            size_t wl = strlen(words[k]);
            for (char *w = strstr(r, words[k]); w; w = strstr(w, words[k])) {
                if (w[wl] == ',' || w[wl] == ' ' || w[wl] == ')') memmove(w, w + wl, strlen(w + wl) + 1);
                else w += wl;
            }
        }
    }
    /* (position-area: logical words short when there are two; beside span-all
     * a word of either axis says which) */
    if (r && !strcmp(n, "position-area") && strchr(r, ' ')) {
        char a[40] = "", b[40] = "", o[90];
        if (sscanf(r, "%39s %39s", a, b) == 2) {
            bool all0 = !strcmp(a, "span-all"), all1 = !strcmp(b, "span-all");
            if (all0 || all1) {
                /* (start: block first, inline after) */
                char *w = all0 ? b : a;
                const char *axis = all0 ? "inline-" : "block-";
                char *st = strstr(w, "start"), *en = strstr(w, "end");
                char *at = st ? st : en;
                if (at) {
                    size_t pre = (size_t)(at - w);
                    snprintf(o, sizeof o, "%.*s%s%s", (int)pre, w, axis, at);
                    free(r);
                    r = strdup(o);
                }
            } else {
                for (int k = 0; k < 2; k++) {
                    char *w = k ? b : a;
                    char *c;
                    if ((c = strstr(w, "block-")) || (c = strstr(w, "inline-"))) memmove(c, strchr(c, '-') + 1, strlen(strchr(c, '-') + 1) + 1);
                }
                if (!strcmp(a, b)) snprintf(o, sizeof o, "%s", a);
                else snprintf(o, sizeof o, "%s %s", a, b);
                free(r);
                r = strdup(o);
            }
        }
    }
    /* (a positioned box's insets: what they came to) */
    if (cs->used.rendered && !cs->pseudo && r) {
        int side = !strcmp(n, "top") ? 0 : !strcmp(n, "right") ? 1 : !strcmp(n, "bottom") ? 2 : !strcmp(n, "left") ? 3 : -1;
        if (side >= 0 && (!strcmp(r, "auto") || strchr(r, '%'))) {
            static const char *const opp[] = { "bottom", "left", "top", "right" };
            const CssPropDef *od = css_prop_find(opp[side], strlen(opp[side]));
            const char *os = od ? cs->spec[prop_idx(od)] : NULL;
            double v = 0.0;
            layout_query_flush(ctx);
            if (layout_query_used_inset(cs->el, side, !strcmp(r, "auto"), !os || !strcmp(os, "auto") || !strcmp(os, "initial") || !strcmp(os, "unset"), &v)) {
                free(r);
                r = px(v);
            }
        }
    }
    /* (clip's old form without commas: as the one with) */
    if (r && !strcmp(n, "clip") && !strncmp(r, "rect(", 5) && !strchr(r, ',')) {
        char t[160], *o = t;
        const char *q = r;
        while (*q && o < t + sizeof t - 2) {
            if (*q == ' ' && q[-1] != '(' && q[1] != ')') *o++ = ',';
            *o++ = *q++;
        }
        *o = 0;
        free(r);
        r = strdup(t);
    }
    /* a running transition or animation's value (css_anim.h) */
    bool is_transform = !strcmp(n, "transform");
    bool individual = !strcmp(n, "translate") || !strcmp(n, "rotate") || !strcmp(n, "scale");
    /* (sizes between keywords or calc-size()s: over their lengths, in script) */
    bool sized_prop = !strcmp(n, "width") || !strcmp(n, "height") || !strcmp(n, "min-width") || !strcmp(n, "min-height") ||
                      !strcmp(n, "max-width") || !strcmp(n, "max-height");
    /* (colors that interpolate in oklab: over what script works out) */
    bool color_prop = !strcmp(n, "color") || !strcmp(n, "background-color");
    bool wide_color = color_prop && !cs->pseudo && ((s && color_spec_wide(s)) || before_color_wide(cs->el, p));
    /* (a margin, padding or inset by percentage: the store has no length for it) */
    bool box_prop = !strncmp(n, "margin-", 7) || !strncmp(n, "padding-", 8) || !strcmp(n, "top") || !strcmp(n, "right") || !strcmp(n, "bottom") || !strcmp(n, "left");
    bool box_pct = box_prop && !cs->pseudo && ((s && (strchr(s, '%') || !strcasecmp(s, "auto"))) || before_pct(cs->el, p) || before_held(cs->el, p));
    bool generic_size = sized_prop && !cs->pseudo && (has_calc_size || before_held(cs->el, p) || (s && (size_spec_special(s) || before_size_special(cs->el, p))) ||
                        !strcmp(computed_of(ctx, cs, "interpolate-size"), "allow-keywords"));
    /* (script working out what a declaration comes to: the declaration alone) */
    int ap = is_transform || individual || generic_size || box_pct || wide_color || (sized_prop && (g_in_calc || g_in_hook)) ? -1 : anim_prop_of(n);
    AnimVal av;
    /* (what composition builds on is the value under the animation; the script works it out) */
    extern bool g_css_saw_composition;
    bool comp_path = g_css_saw_composition && !g_comp_base && ap >= 0 && !g_in_hook && !g_in_calc && !cs->pseudo && anim_has(cs->el, 0) && strncmp(n, "animation", 9);
    bool comp_used = false;
    if (comp_path) {
        bool held = cs->used.rendered;
        cs->used.rendered = false;
        g_comp_base++;
        char *under = compute(ctx, cs, p);
        g_comp_base--;
        /* (a font size is read from the box) */
        if (!strcmp(n, "font-size") && s && strstr(s, "px") && !strchr(s, '(')) { free(under); under = strdup(s); }
        cs->used.rendered = held;
        GCValue fn = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "__cyberGenericAnim");
        if (under && JS_IsFunction(ctx, fn)) {
            GCValue args[7] = { dom_js(ctx, cs->el), JS_NewString(ctx, n), JS_NewString(ctx, under), JS_UNDEFINED, JS_TRUE, s ? JS_NewString(ctx, s) : JS_NULL, JS_NULL };
            g_in_hook++;
            GCValue got = JS_Call(ctx, fn, JS_UNDEFINED, 7, args);
            g_in_hook--;
            if (JS_IsException(got)) JS_GetException(ctx);
            else if (JS_IsString(got)) {
                const char *t = JS_ToCString(ctx, got);
                if (t) { free(r); r = strdup(t); comp_used = true; }
            }
        }
        free(under);
    }
    if (!g_comp_base && !comp_used && ap >= 0 && anim_value(cs->el, cs->pseudo, ap, (double)anim_time_ms(), &av)) {
        char b[96];
        anim_val_css(ap, &av, b, sizeof b);
        /* (translate: its numbers on their own) */
        if (!strcmp(n, "translate")) {
            double tx = 0.0, ty = 0.0;
            if (sscanf(b, "matrix(1, 0, 0, 1, %lf, %lf)", &tx, &ty) == 2) {
                if (ty != 0.0) snprintf(b, sizeof b, "%.6gpx %.6gpx", tx, ty);
                else snprintf(b, sizeof b, "%.6gpx", tx);
            }
        }
        free(r);
        r = strdup(b);
    }
    /* (one the engine doesn't animate itself: over what it comes to, in script) */
    #define in_hook g_in_hook
    bool generic = ap < 0 && !in_hook && !g_comp_base && !g_in_calc && !cs->pseudo && strncmp(n, "animation", 9) && strncmp(n, "transition", 10);
    /* (one the engine animates, with nothing of its own on it: keyframes it can't read) */
    bool keyframes_only = false;
    if (!generic && (sized_prop || color_prop) && !in_hook && !g_in_calc && !cs->pseudo && anim_has(cs->el, 0) &&
        !anim_value(cs->el, cs->pseudo, ap, (double)anim_time_ms(), &av)) {
        generic = true;
        keyframes_only = true;
    }

    char *before_spec = NULL;
    char *now_spec = (sized_prop || color_prop || box_prop) && s ? strdup(s) : NULL;
    char *before = generic && !keyframes_only ? before_swap(cs->el, p, r, now_spec, &before_spec) : NULL;
    /* (a size read now may be what a later change transitions from) */
    if (!generic && (sized_prop || color_prop || box_prop) && !in_hook && !g_in_calc && !cs->pseudo) {
        char *old_spec = NULL;
        free(before_swap(cs->el, p, r, now_spec, &old_spec));
        free(old_spec);
    }
    if (generic && (anim_has(cs->el, 0) || layout_query_has_anim_spec(cs->el))) {
        GCValue fn = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "__cyberGenericAnim");
        if (JS_IsFunction(ctx, fn)) {
            GCValue args[7] = { dom_js(ctx, cs->el), JS_NewString(ctx, n), JS_NewString(ctx, r ? r : ""),
                                before ? JS_NewString(ctx, before) : JS_UNDEFINED, JS_NewBool(ctx, keyframes_only),
                                now_spec ? JS_NewString(ctx, now_spec) : JS_NULL,
                                before_spec ? JS_NewString(ctx, before_spec) : JS_NULL };
            in_hook++;
            GCValue got = JS_Call(ctx, fn, JS_UNDEFINED, 7, args);
            in_hook--;
            if (JS_IsException(got)) JS_GetException(ctx);
            else if (JS_IsString(got)) {
                const char *t = JS_ToCString(ctx, got);
                if (t) {
                    if (!keyframes_only && (sized_prop || box_prop)) before_hold(cs->el, p, !r || strcmp(r, t));
                    free(r);
                    r = strdup(t);
                }
            }
        }
    }
    free(before);
    free(before_spec);
    free(now_spec);
    /* (an opacity comes to a number from 0 to 1, however it was reached) */
    if (r && strstr(n, "opacity") && !g_in_calc) {
        const char *q = r;
        bool pct = strchr(q, '%') != NULL;
        if (!strncmp(q, "calc(", 5)) q += 5;
        char *e = NULL;
        double v = strtod(q, &e);
        if (e != q) {
            if (pct) v /= 100.0;
            v = v < 0 ? 0 : v > 1 ? 1 : v;
            char b[48];
            snprintf(b, sizeof b, "%.6g", v);
            free(r);
            r = strdup(b);
        }
    }
    /* (read from script's own interpolating, it stays a list) */
    if (is_transform && !in_hook && !g_in_calc && r && strcmp(r, "none")) {
        double w = cs->used.rendered ? cs->used.content_width + cs->used.padding[1] + cs->used.padding[3] + cs->used.border[1] + cs->used.border[3] : 0.0;
        double h = cs->used.rendered ? cs->used.content_height + cs->used.padding[0] + cs->used.padding[2] + cs->used.border[0] + cs->used.border[2] : 0.0;
        char *mx = transform_matrix(r, w, h);
        if (mx) { free(r); r = mx; }
    }
    #undef in_hook
    /* (light-dark() of images: the one the used color scheme picks) */
    if (r && strstr(r, "light-dark(") && (!strcmp(n, "background-image") || !strcmp(n, "list-style-image") ||
                                          !strcmp(n, "border-image-source") || !strcmp(n, "mask-image") || !strcmp(n, "content"))) {
        const char *scheme = computed_of(ctx, cs, "color-scheme");
        bool dark = scheme && strstr(scheme, "dark") && !strstr(scheme, "light");
        for (int round = 0; round < 4 && strstr(r, "light-dark("); round++) do {
            static const char clear[] = "image(rgba(0, 0, 0, 0))";
            size_t cap = strlen(r) * 2 + sizeof clear, o = 0;
            char *out = (char *)malloc(cap);
            if (!out) break;
            for (const char *p = r; *p; ) {
                if (strncmp(p, "light-dark(", 11) || (p > r && (isalnum((unsigned char)p[-1]) || p[-1] == '-'))) { out[o++] = *p++; continue; }
                const char *a = p + 11, *comma = NULL, *e = a;
                int d = 1;
                for (; *e && d; e++) {
                    if (*e == '(') d++;
                    else if (*e == ')') { if (--d == 0) break; }
                    else if (*e == ',' && d == 1 && !comma) comma = e;
                }
                if (!comma || !*e) { out[o++] = *p++; continue; }
                const char *x = dark ? comma + 1 : a, *y = dark ? e : comma;
                while (x < y && isspace((unsigned char)*x)) x++;
                while (y > x && isspace((unsigned char)y[-1])) y--;
                /* (none, picked: an image of nothing) */
                if (y - x == 4 && !strncmp(x, "none", 4)) { memcpy(out + o, clear, sizeof clear - 1); o += sizeof clear - 1; }
                else { memcpy(out + o, x, (size_t)(y - x)); o += (size_t)(y - x); }
                p = e + 1;
            }
            out[o] = 0;
            free(r);
            r = out;
        } while (0);
    }
    /* (gradients: what goes without saying, unsaid) */
    if (r && strstr(r, "gradient(") && (!strcmp(n, "background-image") || !strcmp(n, "list-style-image") || !strcmp(n, "border-image-source"))) {
        char *gc = css_gradient_computed(r);
        if (gc) { free(r); r = gc; }
    }
    /* (a shape in its computed form, an animated one too) */
    if (r && strchr(r, '(') && (!strcmp(n, "clip-path") || !strcmp(n, "shape-outside") || (!strcmp(n, "offset-path") && !strstr(r, "ray(")))) {
        /* (animations work from the commands as written) */
        g_css_shape_path_normal = !g_in_hook;
        char *sc = css_shape_computed(n, r);
        g_css_shape_path_normal = false;
        if (sc) { free(r); r = sc; }
    }
    return r;
}

/* A length as computed on an element, without touching its style or laying out. */
/* __cyberHasAnimSpec(el): as of the last layout, it declares a transition or animation. */
GCValue js_has_anim_spec(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)ctx; (void)this_val;
    NodeId id = argc > 0 && JS_IsObject(argv[0]) ? dom_node_of(argv[0]) : DOM_NULL;
    return JS_NewBool(ctx, id && layout_query_has_anim_spec(id));
}

/* __cyberComputedValue(el, prop): a value as computed, before any layout has
 * its say (percentages stay percentages). */
GCValue js_computed_unresolved(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    NodeId id = argc > 1 && JS_IsObject(argv[0]) ? dom_node_of(argv[0]) : DOM_NULL;
    if (!id || dom_kind(id) != DOM_K_ELEMENT) return JS_NULL;
    const char *prop = JS_ToCString(ctx, argv[1]);
    if (!prop) return JS_EXCEPTION;
    ComputedStyle *cs = computed_style_new(id);
    GCValue out = JS_NULL;
    if (cs) {
        refresh(ctx, cs);
        cs->used.rendered = false;
        /* (a third argument: Typed OM's computed value, not the resolved one) */
        g_cs_typed = argc > 2 && JS_ToBool(ctx, argv[2]);
        char *v = computed_style_value(ctx, cs, prop, strlen(prop));
        g_cs_typed = false;
        if (v) { out = JS_NewString(ctx, v); free(v); }
        computed_style_free(cs);
    }
    return out;
}

GCValue js_compute_length(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    NodeId id = argc > 1 ? dom_node_of(argv[0]) : 0;
    const char *v = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    const char *as = argc > 2 && JS_IsString(argv[2]) ? JS_ToCString(ctx, argv[2]) : "width";
    GCValue out = JS_NULL;
    if (id && v) {
        ComputedStyle *t = computed_style_new(id);
        refresh(ctx, t);
        const CssPropDef *p = css_prop_find(as, strlen(as));
        char *c = t->styled && p ? compute_value(ctx, t, p, v) : NULL;
        if (c) { out = JS_NewString(ctx, c); free(c); }
        computed_style_free(t);
    }
    return out;
}

/* One value of a registered property's <type>, as the property that takes that
 * type computes it. */
static char *registered_item(JSContextHandle ctx, ComputedStyle *cs, const char *type, const char *v) {
    const char *as = !strcmp(type, "length") ? "width" : !strcmp(type, "length-percentage") ? "width"
                   : !strcmp(type, "color") ? "background-color" : !strcmp(type, "transform-function") ? "transform"
                   : !strcmp(type, "transform-list") ? "transform" : !strcmp(type, "time") ? "transition-duration"
                   : !strcmp(type, "angle") ? "rotate"
                   : !strcmp(type, "percentage") && strchr(v, '(') ? "width" : NULL;
    if (!strcmp(type, "integer") || !strcmp(type, "number")) {
        const char *b = v;
        while (*b && isspace((unsigned char)*b)) b++;
        if (strncasecmp(b, "calc(", 5)) return strdup(v);
        double d;
        if (!css_calc_number(v, &d)) {
            /* (with units in its math: under this element's) */
            const CssPropDef *fg = css_prop_find("flex-grow", 9);
            CssComputeEnv ne;
            if (fg) env_of(ctx, cs, fg, v, &ne);
            char *nr = fg ? css_prop_compute(fg, v, &ne) : NULL;
            char *ne2 = NULL;
            d = nr ? strtod(nr, &ne2) : 0;
            bool got = nr && ne2 != nr && !*ne2;
            free(nr);
            if (!got) return strdup(v);
        }
        char tmp[64];
        if (!strcmp(type, "integer")) snprintf(tmp, sizeof tmp, "%lld", (long long)floor(d + 0.5));
        else snprintf(tmp, sizeof tmp, "%.6g", d);
        return strdup(tmp);
    }
    if (!as) return strdup(v);
    const CssPropDef *p = css_prop_find(as, strlen(as));
    if (!p) return strdup(v);
    char *c = compute_value(ctx, cs, p, v);
    if (!c) return strdup(v);
    /* (a function's name as written in the specs: the property's value has it folded) */
    if (!strcmp(as, "transform")) {
        static const char *const names[] = { "translateX", "translateY", "translateZ", "translate3d", "scaleX", "scaleY",
            "scaleZ", "scale3d", "rotateX", "rotateY", "rotateZ", "rotate3d", "skewX", "skewY", "matrix3d" };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            size_t n = strlen(names[i]);
            for (char *q = c; *q; q++) if (!strncasecmp(q, names[i], n)) memcpy(q, names[i], n);
        }
    }
    return c;
}

/* A registered custom property's value as computed: each of its values by type. */
static char *registered_computed(JSContextHandle ctx, ComputedStyle *cs, int reg, const char *v) {
    const char *syntax = css_reg_syntax(reg);
    char type[64], mult = 0;
    if (!v[0] || !css_syntax_component(syntax, v, type, sizeof type, &mult)) return NULL;
    refresh(ctx, cs);
    if (!mult) return registered_item(ctx, cs, type, v);
    /* (the list, each item computed; ", " or " " between) */
    size_t cap = strlen(v) * 4 + 64, used = 0;
    char *out = calloc(1, cap);
    const char *s = v;
    for (;;) {
        while (*s && (isspace((unsigned char)*s) || (mult == '#' && *s == ','))) s++;
        if (!*s) break;
        const char *e = s;
        int depth = 0;
        for (; *e; e++) {
            if (*e == '(') depth++;
            else if (*e == ')') depth--;
            else if (!depth && (mult == '#' ? *e == ',' : isspace((unsigned char)*e))) break;
        }
        char *item = strndup(s, (size_t)(e - s));
        size_t il = strlen(item);
        while (il && isspace((unsigned char)item[il - 1])) item[--il] = 0;
        char *c = registered_item(ctx, cs, type, item);
        used += (size_t)snprintf(out + used, used < cap ? cap - used : 0, "%s%s", used ? (mult == '#' ? ", " : " ") : "", c ? c : item);
        free(c);
        free(item);
        s = e;
    }
    return out;
}

char *computed_style_value(JSContextHandle ctx, ComputedStyle *cs, const char *prop, size_t len) {
    if (css_prop_is_custom(prop, len)) {
        /* straight from the layout's map (no snapshot: elements carry
         * hundreds of custom properties) */
        GCValue el = dom_js(ctx, cs->el);
        char *name = strndup(prop, len);
        char *v = JS_IsObject(el) ? layout_query_custom(ctx, el, name) : NULL;
        free(name);
        if (!v) return strdup("");
        /* (the computed value: without leading and trailing white space) */
        char *b = v;
        while (*b && isspace((unsigned char)*b)) b++;
        size_t n = strlen(b);
        while (n && isspace((unsigned char)b[n - 1])) n--;
        char *r = strndup(b, n);
        free(v);
        name = strndup(prop, len);
        int reg = css_reg_count() ? css_reg_find(name) : -1;
        free(name);
        if (reg >= 0) {
            char *c = registered_computed(ctx, cs, reg, r);
            if (c) { free(r); r = c; }
        }
        /* (under an animation from @keyframes) */
        if (anim_has(cs->el, 0) && !g_in_hook) {
            GCValue fn = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "__cyberCustomCssAnim");
            if (JS_IsFunction(ctx, fn)) {
                g_in_hook = true;
                GCValue a[3] = { el, JS_NewStringLen(ctx, prop, len), JS_NewString(ctx, r) };
                GCValue got = JS_Call(ctx, fn, JS_UNDEFINED, 3, a);
                g_in_hook = false;
                const char *t = JS_IsString(got) ? JS_ToCString(ctx, got) : NULL;
                if (t) { free(r); r = strdup(t); }
            }
        }
        return r;
    }
    refresh(ctx, cs);
    if (!cs->styled || !cs->spec) return strdup("");
    const CssPropDef *p = css_prop_find(prop, len);
    if (!p) return strdup("");
    if (p->longhands) {
        const char **names;
        int n = css_prop_longhands(p, &names);
        const char **vals = calloc((size_t)(n ? n : 1), sizeof(char *));
        /* (a copy: reading the next may style again, under an animation) */
        for (int i = 0; i < n; i++) {
            const char *v = computed_of(ctx, cs, names[i]);
            vals[i] = v ? strdup(v) : NULL;
        }
        char *s = css_prop_shorthand_serialize(p, vals);
        for (int i = 0; i < n; i++) free((char *)vals[i]);
        free(vals);
        return s ? s : strdup("");
    }
    return strdup(computed_of(ctx, cs, p->name));
}

/* The longhands, in order: the declarations of a computed style. */
static const CssPropDef **g_longhands;
static int g_nlonghands;

static void longhands_init(void) {
    if (g_longhands) return;
    g_longhands = calloc((size_t)css_prop_count(), sizeof(*g_longhands));
    for (int i = 0; i < css_prop_count(); i++)
        if (!css_prop_at(i)->longhands && strcmp(css_prop_at(i)->name, "all")) g_longhands[g_nlonghands++] = css_prop_at(i);
}

int computed_style_length(ComputedStyle *cs) {
    /* (nothing to compute outside the document) */
    if (!cs->pseudo && !dom_is_connected(cs->el)) return 0;
    longhands_init();
    return g_nlonghands;
}

const char *computed_style_item(ComputedStyle *cs, int i) {
    if (!cs->pseudo && !dom_is_connected(cs->el)) return NULL;
    longhands_init();
    return i >= 0 && i < g_nlonghands ? g_longhands[i]->name : NULL;
}

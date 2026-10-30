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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <quickjs.h>
#include "css_props.h"
#include "dom_js.h"
#include "layout_api.h"
#include "computed_style.h"

uint64_t dom_change_stamp(void);
#define CST(...) do { if (getenv("CS_TRACE")) { fprintf(stderr, "[cs] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } } while (0)

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
};

ComputedStyle *computed_style_new(NodeId el) {
    ComputedStyle *cs = calloc(1, sizeof *cs);
    if (cs) cs->el = el;
    return cs;
}

static void snapshot_clear(ComputedStyle *cs) {
    int n = css_prop_count();
    for (int i = 0; cs->spec && i < n; i++) { free(cs->spec[i]); free(cs->comp[i]); cs->spec[i] = cs->comp[i] = NULL; }
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
    int i = prop_idx(p);
    free(cs->spec[i]);
    cs->spec[i] = strdup(v);
}

/* One cascaded declaration (in cascade order: the last one wins). */
static void collect(void *ud, const char *prop, const char *value, bool important) {
    CST("decl %s: %s", prop, value);
    (void)important;
    ComputedStyle *cs = ud;
    size_t pl = strlen(prop);
    if (css_prop_is_custom(prop, pl)) return;      /* (read from the layout when asked for) */
    const CssPropDef *p = css_prop_find(prop, pl);
    if (!p) return;
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
    cs->styled = JS_IsObject(el) && dom_kind(cs->el) == DOM_K_ELEMENT &&
                 layout_query_cascade(ctx, el, collect, cs, &cs->used);
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
    NodeId p = dom_parent(cs->el);
    /* (a shadow root's host is its children's parent for inheritance) */
    if (p && dom_kind(p) != DOM_K_ELEMENT) p = DOM_NULL;
    if (!p) return NULL;
    cs->parent = computed_style_new(p);
    if (cs->parent) refresh(ctx, cs->parent);
    return cs->parent;
}

static char *compute(JSContextHandle ctx, ComputedStyle *cs, const CssPropDef *p);

static const char *computed_of(JSContextHandle ctx, ComputedStyle *cs, const char *name) {
    const CssPropDef *p = css_prop_find(name, strlen(name));
    if (!p) return "";
    int i = prop_idx(p);
    if (!cs->comp[i]) cs->comp[i] = compute(ctx, cs, p);
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
    bool is_fs = !strcmp(p->name, "font-size"), is_color = !strcmp(p->name, "color");
    ComputedStyle *par = (is_fs || is_color) ? parent_of(ctx, cs) : NULL;
    env->font_size = is_fs ? (par ? font_px(ctx, par) : 16) : font_px(ctx, cs);
    env->root_font_size = cs->root_font_size;
    env->vw = cs->vw;
    env->vh = cs->vh;
    env->color[3] = 1;
    /* currentcolor: the color (for color itself, the parent's) */
    if (!mentions(v, "currentcolor")) return;
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
static char *compute_value(JSContextHandle ctx, ComputedStyle *cs, const CssPropDef *p, const char *v) {
    const char *n = p->name;
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
    /* line widths: thin, medium, thick */
    if (strstr(n, "-width") && (strstr(n, "border") || strstr(n, "outline") || strstr(n, "column-rule"))) {
        const char *kw = !strcmp(r, "thin") ? "1px" : !strcmp(r, "medium") ? "3px" : !strcmp(r, "thick") ? "5px" : NULL;
        if (kw) { free(r); r = strdup(kw); }
    }
    return r;
}

static bool display_blockified(JSContextHandle ctx, ComputedStyle *cs) {
    const char *pos = computed_of(ctx, cs, "position");
    if (!strcmp(pos, "absolute") || !strcmp(pos, "fixed")) return true;
    if (strcmp(computed_of(ctx, cs, "float"), "none")) return true;
    NodeId p = dom_parent(cs->el);
    return p && dom_kind(p) == DOM_K_DOCUMENT;     /* the root element */
}

static char *compute(JSContextHandle ctx, ComputedStyle *cs, const CssPropDef *p) {
    CST("compute %s", p->name);
    if (!cs->styled) return strdup("");
    int i = prop_idx(p);
    const char *s = cs->spec[i];
    const char *n = p->name;
    /* (an element's default display is the user agent's: what layout gave it) */
    if (!s && !strcmp(n, "display") && cs->used.display) s = cs->used.display;
    if (!s || !strcmp(s, "unset") || !strcmp(s, "revert") || !strcmp(s, "revert-layer"))
        s = p->inherited ? "inherit" : "initial";
    char *r;
    if (!strcmp(s, "inherit")) {
        ComputedStyle *par = parent_of(ctx, cs);
        r = par && par->styled ? strdup(computed_of(ctx, par, n)) : compute_value(ctx, cs, p, p->initial);
        /* (an inherited font-weight etc. is the parent's computed value) */
    } else {
        r = compute_value(ctx, cs, p, !strcmp(s, "initial") ? p->initial : s);
    }
    /* the properties whose computed value depends on another one */
    if (strstr(n, "-width") && !strncmp(n, "border-", 7) && !strstr(n, "image")) {
        char style[64];
        snprintf(style, sizeof style, "%.*sstyle", (int)(strlen(n) - 5), n);
        const char *st = computed_of(ctx, cs, style);
        if (!strcmp(st, "none") || !strcmp(st, "hidden")) { free(r); r = strdup("0px"); }
    } else if (!strcmp(n, "outline-width")) {
        if (!strcmp(computed_of(ctx, cs, "outline-style"), "none")) { free(r); r = strdup("0px"); }
    } else if (!strcmp(n, "column-rule-width")) {
        const char *st = computed_of(ctx, cs, "column-rule-style");
        if (!strcmp(st, "none") || !strcmp(st, "hidden")) { free(r); r = strdup("0px"); }
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
    return r;
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
        for (int i = 0; i < n; i++) vals[i] = computed_of(ctx, cs, names[i]);
        char *s = css_prop_shorthand_serialize(p, vals);
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
        if (!css_prop_at(i)->longhands) g_longhands[g_nlonghands++] = css_prop_at(i);
}

int computed_style_length(ComputedStyle *cs) {
    (void)cs;
    longhands_init();
    return g_nlonghands;
}

const char *computed_style_item(ComputedStyle *cs, int i) {
    (void)cs;
    longhands_init();
    return i >= 0 && i < g_nlonghands ? g_longhands[i]->name : NULL;
}

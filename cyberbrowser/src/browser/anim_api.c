/*
 * anim_api.c - CSS transitions and animations for scripts: their events,
 * fired from the event loop (CSS Transitions 1 section 6, CSS Animations 1
 * section 4), the frames they need while they run, and
 * Element.getAnimations() (CSS Animations 2 / Web Animations: CSSAnimation
 * and CSSTransition objects with commitStyles() and cancel()).
 *
 * What runs is css_anim.c's store; layout starts and samples it.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <quickjs.h>
#include "browser_api_impl.h"
#include "browser_api_impl_types.h"
#include "browser_api_impl_internal.h"
#include "css_anim.h"
#include "css_parser.h"
#include "dom_store.h"
#include "dom_wrap.h"
#include "dom_js.h"
#include "js_quickjs.h"
#include "css_props.h"
#include "layout_api.h"



/* Frames sampled while something ran (layout_api.c lays out again when it
 * moved on). */
uint64_t g_anim_frame;

static double anim_page_clock(void) { return (double)anim_time_ms(); }

void anim_api_init(void) { anim_set_clock(anim_page_clock); }

static const char *const k_types[] = { "transitionrun", "transitionstart", "transitionend", "transitioncancel",
                                       "animationstart", "animationiteration", "animationend", "animationcancel" };

/* Fire e at its element, an event of the element's realm. */
static void anim_dispatch(JSContextHandle ctx, const AnimEvent *e)
{
    GCValue target = dom_js(ctx, e->el);
    if (!JS_IsObject(target)) return;
    GCValue dispatch = JS_GetPropertyStr(ctx, target, "dispatchEvent");
    if (!JS_IsFunction(ctx, dispatch)) return;
    JSContextHandle realm = JS_GetFunctionRealmOf(ctx, dispatch);
    if (!realm) realm = ctx;
    bool trans = e->type <= ANIM_EV_TCANCEL;
    GCValue ctor = JS_GetPropertyStr(realm, JS_GetGlobalObject(realm), trans ? "TransitionEvent" : "AnimationEvent");
    if (!JS_IsFunction(realm, ctor)) return;
    GCValue init = JS_NewObject(realm);
    JS_SetPropertyStr(realm, init, "bubbles", JS_TRUE);
    JS_SetPropertyStr(realm, init, trans ? "propertyName" : "animationName", JS_NewString(realm, e->name));
    JS_SetPropertyStr(realm, init, "elapsedTime", JS_NewFloat64(realm, e->elapsed));
    JS_SetPropertyStr(realm, init, "pseudoElement",
                      JS_NewString(realm, e->ps == 1 ? "::before" : e->ps == 2 ? "::after" : e->ps == 3 ? "::marker" : ""));
    GCValue args[2] = { JS_NewString(realm, k_types[e->type]), init };
    GCValue ev = JS_CallConstructor(realm, ctor, 2, args);
    if (JS_IsException(ev)) { JS_GetException(realm); return; }
    /* (trusted: the realm's engine dispatch, else the page's dispatchEvent) */
    GCValue trusted = JS_GetPropertyStr(realm, JS_GetGlobalObject(realm), "__cyberDispatchTrusted");
    GCValue targs[2] = { target, ev };
    GCValue r = JS_IsFunction(realm, trusted) ? JS_Call(realm, trusted, JS_UNDEFINED, 2, targs)
                                              : JS_Call(ctx, dispatch, target, 1, &ev);
    if (JS_IsException(r)) JS_GetException(ctx);
}

/* ---- Web Animations (web_animations.js): what each element's effects come to,
 * declarations the cascade takes after its inline style ---- */

static struct { NodeId *el; char **text; uint32_t n, cap; } g_web_styles;
#define WEB_REALMS 16
static JSContextHandle g_web_realms[WEB_REALMS];
static int g_web_realm_n;

const char *anim_api_web_style(uint32_t el)
{
    for (uint32_t i = 0; i < g_web_styles.n; i++)
        if (g_web_styles.el[i] == el) return g_web_styles.text[i];
    return NULL;
}

void anim_api_reset(void)
{
    for (uint32_t i = 0; i < g_web_styles.n; i++) free(g_web_styles.text[i]);
    g_web_styles.n = 0;
    g_web_realm_n = 0;
}

/* __cyberAnimStyle(el, text or null) */
static GCValue js_anim_style(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    NodeId el = argc > 0 ? dom_node_of(argv[0]) : DOM_NULL;
    if (!el) return JS_UNDEFINED;
    const char *text = argc > 1 && JS_IsString(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
    uint32_t i = 0;
    while (i < g_web_styles.n && g_web_styles.el[i] != el) i++;
    bool had = i < g_web_styles.n;
    if (!text && !had) return JS_UNDEFINED;
    if (text && had && !strcmp(text, g_web_styles.text[i])) return JS_UNDEFINED;
    if (had) free(g_web_styles.text[i]);
    if (!text) {
        g_web_styles.n--;
        g_web_styles.el[i] = g_web_styles.el[g_web_styles.n];
        g_web_styles.text[i] = g_web_styles.text[g_web_styles.n];
    } else {
        if (!had) {
            if (g_web_styles.n == g_web_styles.cap) {
                g_web_styles.cap = g_web_styles.cap ? g_web_styles.cap * 2 : 16;
                g_web_styles.el = realloc(g_web_styles.el, g_web_styles.cap * sizeof(NodeId));
                g_web_styles.text = realloc(g_web_styles.text, g_web_styles.cap * sizeof(char *));
            }
            g_web_styles.el[i] = el;
            g_web_styles.n++;
        }
        g_web_styles.text[i] = strdup(text);
    }
    dom_mark_dirty(el, DOMF_DIRTY_ATTR);
    dom_request_relayout();
    return JS_UNDEFINED;
}

/* __cyberAnimRow(op, row, gen, value): a CSS animation or transition for
 * script.  op 0: its timing, [start, delay, duration, iterations, current,
 * paused, ended, fill, direction, now], or null; 1 seek; 2 start time; 3 pause;
 * 4 play. */
static GCValue js_anim_row(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    int32_t op = 0, r = -1, g = 0;
    double v = 0.0;
    if (argc < 3 || JS_ToInt32(ctx, &op, argv[0]) || JS_ToInt32(ctx, &r, argv[1]) || JS_ToInt32(ctx, &g, argv[2])) return JS_NULL;
    if (argc > 3) JS_ToFloat64(ctx, &v, argv[3]);
    double now = (double)anim_time_ms();
    if (op == 0) {
        AnimRowTiming t;
        if (!anim_row_timing(r, (uint32_t)g, now, &t)) return JS_NULL;
        double values[] = { t.start, t.delay, t.duration, t.iterations, t.current, t.paused, t.ended, t.fill, t.direction, now };
        GCValue a = JS_NewArray(ctx);
        for (uint32_t i = 0; i < sizeof values / sizeof values[0]; i++) JS_SetPropertyUint32(ctx, a, i, JS_NewFloat64(ctx, values[i]));
        return a;
    }
    if (op == 1) anim_row_set_time(r, (uint32_t)g, now, v);
    else if (op == 2) anim_row_set_start(r, (uint32_t)g, v);
    else if (op == 3) anim_row_pause(r, (uint32_t)g, now, true);
    else if (op == 4) anim_row_pause(r, (uint32_t)g, now, false);
    g_anim_frame++;
    dom_request_relayout();
    timer_anim_wake(ctx);
    return JS_UNDEFINED;
}

/* __cyberCssIsShorthand(name). */
static GCValue js_css_is_shorthand(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    const char *n = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    const CssPropDef *p = n ? css_prop_find(n, strlen(n)) : NULL;
    return JS_NewBool(ctx, p && p->longhands);
}

/* __cyberAnimNow(): the time animations go by, in this task. */
static GCValue js_anim_now(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, (double)anim_time_ms());
}

/* __cyberCssLonghands(name): the longhands of a shorthand, or null. */
static GCValue js_css_longhands(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    const char *n = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    const CssPropDef *p = n ? css_prop_find(n, strlen(n)) : NULL;
    if (!p || !p->longhands) return JS_NULL;
    const char **names;
    int c = css_prop_longhands(p, &names);
    GCValue arr = JS_NewArray(ctx);
    for (int i = 0; i < c; i++) JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, names[i]));
    return arr;
}

/* __cyberCssExpand(name, value): { longhand: value }, or null. */
static GCValue js_css_expand(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    const char *n = argc > 1 ? JS_ToCString(ctx, argv[0]) : NULL;
    const char *v = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    const CssPropDef *p = n ? css_prop_find(n, strlen(n)) : NULL;
    if (!p || !p->longhands || !v || strstr(v, "var(")) return JS_NULL;
    const char **names;
    char **vals = NULL;
    int c = css_prop_longhands(p, &names);
    if (css_prop_expand(p, v, &vals) != c) return JS_NULL;
    GCValue o = JS_NewObject(ctx);
    bool ok = true;
    for (int i = 0; i < c; i++) {
        if (!vals[i][0]) ok = false;
        else JS_SetPropertyStr(ctx, o, names[i], JS_NewString(ctx, vals[i]));
        free(vals[i]);
    }
    free(vals);
    return ok ? o : JS_NULL;
}

/* __cyberAnimRunning(element): [name, progress, ...] of its CSS animations. */
static GCValue js_anim_running(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    GCValue out = JS_NewArray(ctx);
    NodeId el = argc > 0 ? dom_node_of(argv[0]) : DOM_NULL;
    if (!el) return out;
    const char *names[32];
    double p[32];
    int32_t ps = 0;
    if (argc > 1) JS_ToInt32(ctx, &ps, argv[1]);
    int n = anim_running_ps(el, ps, names, p, 32, (double)anim_time_ms());
    for (int i = 0; i < n; i++) {
        JS_SetPropertyUint32(ctx, out, (uint32_t)(2 * i), JS_NewString(ctx, names[i] ? names[i] : ""));
        JS_SetPropertyUint32(ctx, out, (uint32_t)(2 * i + 1), JS_NewFloat64(ctx, p[i]));
    }
    return out;
}

/* __cyberRenderFeature(bit): the rendering tick has this to look after. */
static GCValue js_render_feature(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    int32_t bit = 0;
    if (argc > 0 && !JS_ToInt32(ctx, &bit, argv[0])) g_css_render_features |= bit;
    return JS_UNDEFINED;
}

/* __cyberColorScheme("light" | "dark" | null) */
static GCValue js_color_scheme(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    const char *s = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(ctx, argv[0]) : NULL;
    if (css_media_set_color_scheme(!s ? NULL : !strcmp(s, "dark") ? "dark" : "light")) dom_request_relayout();
    return JS_UNDEFINED;
}

/* __cyberPrintSet(page area width, height, gap between pages' areas, left and top margins): print mode */
extern void page_set_viewport(int width, int height);
static GCValue js_print_set(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    double v[7] = { 0 };
    for (int i = 0; i < 7 && i < argc; i++) JS_ToFloat64(ctx, &v[i], argv[i]);
    g_css_print.skip = v[6] > 0.0 && v[6] < 1000.0 ? (int)v[6] : 0;   /* (pages before the first one shown) */
    g_css_print.rot = 0;
    memset(g_css_print.pdx, 0, sizeof g_css_print.pdx);
    memset(g_css_print.pdy, 0, sizeof g_css_print.pdy);
    g_css_print.pw = v[5];
    g_css_print.on = true;
    g_css_print.w = v[0]; g_css_print.h = v[1]; g_css_print.gap = v[2]; g_css_print.ml = v[3]; g_css_print.mt = v[4];
    page_set_viewport((int)v[0], (int)v[1]);
    dom_request_relayout();
    return JS_UNDEFINED;
}

/* __cyberPrintRotate(r): the page shown turned a quarter, 1 clockwise, -1 back (page-orientation) */
static GCValue js_print_rotate(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    int32_t r = 0;
    if (argc > 0) JS_ToInt32(ctx, &r, argv[0]);
    if (g_css_print.rot != r) { g_css_print.rot = r; dom_request_relayout(); }
    return JS_UNDEFINED;
}

/* __cyberPrintPageOff(page, dx, dy): where page's content goes from where the first page's margins put it */
static GCValue js_print_page_off(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    double v[3] = { 0 };
    for (int i = 0; i < 3 && i < argc; i++) JS_ToFloat64(ctx, &v[i], argv[i]);
    int k = (int)v[0];
    if (k >= 0 && k < 64 && (g_css_print.pdx[k] != (float)v[1] || g_css_print.pdy[k] != (float)v[2])) {
        g_css_print.pdx[k] = (float)v[1];
        g_css_print.pdy[k] = (float)v[2];
        dom_request_relayout();
    }
    return JS_UNDEFINED;
}

/* __cyberPrintPageName(page): its name, "" for none */
static GCValue js_print_page_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    double k = 0;
    if (argc > 0) JS_ToFloat64(ctx, &k, argv[0]);
    int i = (int)k;
    return JS_NewString(ctx, i >= 0 && i < 64 ? g_css_print.pname[i] : "");
}

/* __cyberPrintPages(): how many pages the last layout took */
static GCValue js_print_pages(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val; (void)argc; (void)argv; (void)ctx;
    return JS_NewInt32(ctx, g_css_print.pages);
}

/* __cyberMediaPref(feature, value | null) */
static GCValue js_media_pref(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const char *n = JS_ToCString(ctx, argv[0]);
    const char *v = argc > 1 && JS_IsString(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
    if (n && css_media_set_pref(n, v)) dom_request_relayout();
    if (n) JS_FreeCString(ctx, n);
    if (v) JS_FreeCString(ctx, v);
    return JS_UNDEFINED;
}

/* __cyberAnimWake(): this realm's animations want turns. */
static GCValue js_anim_wake(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    int i = 0;
    while (i < g_web_realm_n && g_web_realms[i] != ctx) i++;
    if (i == g_web_realm_n && g_web_realm_n < WEB_REALMS) g_web_realms[g_web_realm_n++] = ctx;
    timer_anim_wake(ctx);
    return JS_UNDEFINED;
}

void anim_api_install(JSContextHandle ctx, GCValue global)
{
    JS_DefinePropertyValueStr(ctx, global, "__cyberAnimStyle", JS_NewCFunction(ctx, js_anim_style, "__cyberAnimStyle", 2), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberAnimWake", JS_NewCFunction(ctx, js_anim_wake, "__cyberAnimWake", 0), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberCssIsShorthand", JS_NewCFunction(ctx, js_css_is_shorthand, "__cyberCssIsShorthand", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberCssLonghands", JS_NewCFunction(ctx, js_css_longhands, "__cyberCssLonghands", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberCssExpand", JS_NewCFunction(ctx, js_css_expand, "__cyberCssExpand", 2), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberAnimNow", JS_NewCFunction(ctx, js_anim_now, "__cyberAnimNow", 0), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberAnimRunning", JS_NewCFunction(ctx, js_anim_running, "__cyberAnimRunning", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberAnimRow", JS_NewCFunction(ctx, js_anim_row, "__cyberAnimRow", 4), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberRenderFeature", JS_NewCFunction(ctx, js_render_feature, "__cyberRenderFeature", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberColorScheme", JS_NewCFunction(ctx, js_color_scheme, "__cyberColorScheme", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberPrintPages", JS_NewCFunction(ctx, js_print_pages, "__cyberPrintPages", 0), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberPrintPageName", JS_NewCFunction(ctx, js_print_page_name, "__cyberPrintPageName", 1), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberPrintPageOff", JS_NewCFunction(ctx, js_print_page_off, "__cyberPrintPageOff", 3), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberPrintRotate", JS_NewCFunction(ctx, js_print_rotate, "__cyberPrintRotate", 1), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberPrintSet", JS_NewCFunction(ctx, js_print_set, "__cyberPrintSet", 5), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberMediaPref", JS_NewCFunction(ctx, js_media_pref, "__cyberMediaPref", 2), JS_PROP_CONFIGURABLE);
}

static void web_animations_tick(JSContextHandle ctx)
{
    for (int i = 0; i < g_web_realm_n; ) {
        JSContextHandle realm = g_web_realms[i];
        GCValue fn = JS_GetPropertyStr(realm, JS_GetGlobalObject(realm), "__cyberWebAnimTick");
        bool more = false;
        if (JS_IsFunction(realm, fn)) {
            GCValue r = JS_Call(realm, fn, JS_UNDEFINED, 0, NULL);
            if (JS_IsException(r)) JS_GetException(realm);
            else more = JS_ToBool(realm, r) > 0;
            timer_microtask_checkpoint(ctx);
        }
        if (more) { i++; continue; }
        g_web_realms[i] = g_web_realms[--g_web_realm_n];
    }
    if (g_web_realm_n) { g_anim_frame++; timer_anim_wake(ctx); }
}

/* A rendering opportunity: the events due, then (while anything runs) a
 * frame: the layout samples it again, and another turn comes. */
int anim_api_tick(JSContextHandle ctx)
{
    /* (a frame's style pass: what it starts in script, with what a transition names) */
    {
        extern void computed_style_after_frame(JSContextHandle ctx);
        computed_style_after_frame(ctx);
    }
    if (g_web_realm_n) web_animations_tick(ctx);
    if (!anim_any()) return 0;
    /* (the other documents with transitions see their style changes: the
     * main one's frames lay it out, theirs only queries do) */
    NodeId docs[16];
    int nd = anim_docs(docs, 16);
    for (int i = 0; i < nd; i++) {
        if (docs[i] == g_dom_document) continue;
        JSContextHandle realm = dom_realm_of_document(docs[i]);
        if (realm && realm != ctx) layout_api_update(realm);
    }
    double now = (double)anim_time_ms();
    AnimEvent ev[32];
    bool live = false;
    int fired = 0;
    for (;;) {
        int n = anim_tick(now, ev, 32, &live);
        for (int i = 0; i < n; i++) {
            anim_dispatch(ctx, &ev[i]);
            timer_microtask_checkpoint(ctx);
        }
        fired += n;
        if (n < 30) break;
    }
    if (live) {
        g_anim_frame++;
        dom_request_layout();
        timer_anim_wake(ctx);
    }
    return fired;
}

/* ---- getAnimations() ---- */

static bool anim_this(JSContextHandle ctx, GCValue this_val, int *row, uint32_t *gen)
{
    int32_t r = -1, g = 0;
    if (JS_ToInt32(ctx, &r, JS_GetPropertyStr(ctx, this_val, "__cyber_row")) ||
        JS_ToInt32(ctx, &g, JS_GetPropertyStr(ctx, this_val, "__cyber_gen")))
        return false;
    *row = r;
    *gen = (uint32_t)g;
    return anim_row_info(r, (uint32_t)g, NULL, NULL, NULL, NULL);
}

typedef struct CommitUd { JSContextHandle ctx; GCValue style, set; } CommitUd;

static void commit_one(void *ud, const char *prop, const char *value)
{
    CommitUd *c = (CommitUd *)ud;
    GCValue args[2] = { JS_NewString(c->ctx, prop), JS_NewString(c->ctx, value) };
    GCValue r = JS_Call(c->ctx, c->set, c->style, 2, args);
    if (JS_IsException(r)) JS_GetException(c->ctx);
}

/* Animation.commitStyles(): what it sets now, into the element's style. */
static GCValue js_anim_commit_styles(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)argc; (void)argv;
    int r;
    uint32_t g;
    NodeId el;
    int ps;
    if (!anim_this(ctx, this_val, &r, &g) || !anim_row_info(r, g, NULL, NULL, &el, &ps)) return JS_UNDEFINED;
    if (ps) return throw_dom_exception(ctx, "NoModificationAllowedError", "The target is a pseudo-element");
    GCValue target = dom_js(ctx, el);
    CommitUd c = { ctx, JS_GetPropertyStr(ctx, target, "style"), JS_UNDEFINED };
    if (!JS_IsObject(c.style)) return JS_UNDEFINED;
    c.set = JS_GetPropertyStr(ctx, c.style, "setProperty");
    if (!JS_IsFunction(ctx, c.set)) return JS_UNDEFINED;
    anim_row_values(r, (double)anim_time_ms(), commit_one, &c);
    return JS_UNDEFINED;
}

static GCValue js_anim_cancel(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)argc; (void)argv;
    int r;
    uint32_t g;
    if (anim_this(ctx, this_val, &r, &g)) {
        anim_row_cancel(r);
        dom_request_layout();
    }
    return JS_UNDEFINED;
}

static GCValue js_anim_play_state(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)argc; (void)argv;
    int r;
    uint32_t g;
    return JS_NewString(ctx, anim_this(ctx, this_val, &r, &g) ? "running" : "idle");
}

static GCValue js_anim_noop(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

GCValue js_element_get_animations(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)argc; (void)argv;
    GCValue arr = JS_NewArray(ctx);
    NodeId el = dom_node_of(this_val);
    if (!el) return arr;
    int rows[64];
    int n = anim_list(el, rows, 64);
    for (int i = 0; i < n; i++) {
        int kind;
        const char *name;
        NodeId e;
        int ps;
        uint32_t g = anim_row_gen(rows[i]);
        if (!anim_row_info(rows[i], g, &kind, &name, &e, &ps)) continue;
        GCValue a = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, a, "__cyber_row", JS_NewInt32(ctx, rows[i]), JS_PROP_CONFIGURABLE);
        JS_DefinePropertyValueStr(ctx, a, "__cyber_gen", JS_NewInt32(ctx, (int32_t)g), JS_PROP_CONFIGURABLE);
        JS_SetPropertyStr(ctx, a, kind == 1 ? "transitionProperty" : "animationName", JS_NewString(ctx, name));
        JS_SetPropertyStr(ctx, a, "row", JS_NewInt32(ctx, rows[i]));
        JS_SetPropertyStr(ctx, a, "gen", JS_NewInt32(ctx, (int32_t)g));
        JS_SetPropertyStr(ctx, a, "pseudo", JS_NewString(ctx, ps == 1 ? "::before" : ps == 2 ? "::after" : ps == 3 ? "::marker" : ""));
        JS_SetPropertyStr(ctx, a, "commitStyles", JS_NewCFunction(ctx, js_anim_commit_styles, "commitStyles", 0));
        JS_SetPropertyStr(ctx, a, "cancel", JS_NewCFunction(ctx, js_anim_cancel, "cancel", 0));
        JS_SetPropertyStr(ctx, a, "play", JS_NewCFunction(ctx, js_anim_noop, "play", 0));
        JS_SetPropertyStr(ctx, a, "pause", JS_NewCFunction(ctx, js_anim_noop, "pause", 0));
        JSAtom ps_atom = JS_NewAtom(ctx, "playState");
        JS_DefinePropertyGetSet(ctx, a, ps_atom, JS_NewCFunction(ctx, js_anim_play_state, "playState", 0),
                                JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, ps_atom);
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, a);
    }
    return arr;
}

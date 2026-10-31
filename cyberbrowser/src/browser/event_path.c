/*
 * event_path.c - the event path of a dispatch (DOM Standard 2.9, "dispatch"
 * steps 5-14) built from the node store for dom_events.js, which runs the
 * listeners:
 *
 *   __cyberEventPath(target, type, composed, relatedTarget, handlerType)
 *     -> [item, target, relatedTarget, flags, ...] (four per entry), or
 *        null when target is not a node (the JS path builder does those).
 *
 * "Get the parent": an element's assigned slot (a JS assignedSlot, which
 * shadow DOM polyfills define), else its parent; a shadow root's host when
 * the event is composed or was dispatched outside the shadow tree; a
 * document's defaultView except for load.  Targets are retargeted against
 * each item.  flags say what may run at the item, so the JS side visits
 * only those (handlerType: whether on<type> is one of the handler
 * accessors, which are not read here):
 *   EP_LISTENERS  listeners of the type
 *   EP_ON_ATTR    an on<type> content attribute
 *   EP_HANDLER    an on<type> handler set (dom_events.js's handler map)
 *   EP_ALWAYS     the window, or <body> (whose handlers forward to it)
 *   EP_ON_PROP    a function-valued on<type> property (another type)
 */
enum { EP_LISTENERS = 1, EP_ON_ATTR = 2, EP_HANDLER = 4, EP_ALWAYS = 8, EP_ON_PROP = 16 };
#include <stdio.h>
#include <string.h>

#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_internal.h"
#include "dom_js.h"
#include "css_style.h"

static bool is_shadow_root(uint32_t s) {
    return g_dom.kind[s] == DOM_K_FRAGMENT && (g_dom.flags[s] & DOMF_SHADOW_ROOT) && g_dom.host[s];
}

/* The node's root (its tree's topmost node: a document, a shadow root, a
 * fragment or a detached node). */
static uint32_t root_of(uint32_t s) {
    while (g_dom.parent[s]) s = g_dom.parent[s];
    return s;
}

/* Is a a shadow-including inclusive ancestor of b? */
static bool shadow_including_ancestor(uint32_t a, uint32_t b) {
    while (b) {
        if (a == b) return true;
        uint32_t r = root_of(b);
        if (r == a) return true;
        b = is_shadow_root(r) ? g_dom.host[r] : 0;
    }
    return false;
}

/* retarget(a, b) on slots (b 0: not a node). */
static uint32_t retarget(uint32_t a, uint32_t b) {
    while (a) {
        uint32_t r = root_of(a);
        if (!is_shadow_root(r)) return a;
        if (b && shadow_including_ancestor(r, b)) return a;
        a = g_dom.host[r];
    }
    return a;
}

/* Symbol.for(name)'s atom (the registry is the runtime's). */
static JSAtom symbol_atom(JSContextHandle ctx, const char *name, JSAtom *cache) {
    if (!*cache) {
        char src[64];
        snprintf(src, sizeof src, "Symbol.for('%s')", name);
        GCValue sym = JS_Eval(ctx, src, strlen(src), "<event_path>", JS_EVAL_TYPE_GLOBAL);
        if (!JS_IsException(sym)) *cache = JS_ValueToAtom(ctx, sym);
    }
    return *cache;
}

/* The atoms it looks up, made in the context of g_atoms_serial (atoms are
 * its runtime's: a document loaded in this process anew has another). */
static JSAtom g_length_atom, g_slot_atom, g_view_atom, g_listeners_atom, g_handlers_atom;
static uint32_t g_atoms_serial;

static void atoms_for(JSContextHandle ctx) {
    uint32_t s = JS_GetContextSerial(ctx);
    if (s == g_atoms_serial) return;
    g_atoms_serial = s;
    g_listeners_atom = g_handlers_atom = 0;
    g_length_atom = JS_NewAtom(ctx, "length");
    g_slot_atom = JS_NewAtom(ctx, "assignedSlot");
    g_view_atom = JS_NewAtom(ctx, "defaultView");
}

/* Does map (an object of type -> list or handler) have a live entry? */

static bool map_has(JSContextHandle ctx, GCValue obj, JSAtom key, JSAtom type, bool list) {
    if (!key) return true;      /* (unknown: let the JS side look) */
    GCValue map = JS_GetProperty(ctx, obj, key);
    if (!JS_IsObject(map)) return false;
    GCValue v = JS_GetProperty(ctx, map, type);
    if (!list) return JS_IsFunction(ctx, v) || JS_IsObject(v);
    uint32_t n = 0;
    return JS_IsObject(v) && !JS_ToUint32(ctx, &n, JS_GetProperty(ctx, v, g_length_atom)) && n > 0;
}

static uint32_t slot_of(GCValue v) {
    NodeId n = JS_IsObject(v) ? dom_node_of(v) : DOM_NULL;
    return n && dom_valid(n) ? dom_slot(n) : 0;
}

static GCValue js_event_path(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    uint32_t target = argc > 0 ? slot_of(argv[0]) : 0;
    if (!target) return JS_NULL;
    const char *type = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    if (!type) return JS_EXCEPTION;
    bool composed = argc > 2 && JS_ToBool(ctx, argv[2]) > 0;
    bool handler_type = argc > 4 && JS_ToBool(ctx, argv[4]) > 0;
    atoms_for(ctx);
    JSAtom la = symbol_atom(ctx, "cyber.listeners", &g_listeners_atom);
    JSAtom ha = symbol_atom(ctx, "cyber.eventHandlers", &g_handlers_atom);
    GCValue related_v = argc > 3 ? argv[3] : JS_NULL;
    bool has_related = JS_IsObject(related_v);
    uint32_t related = has_related ? slot_of(related_v) : 0;
    char on_attr[96], type_buf[88];
    snprintf(on_attr, sizeof on_attr, "on%s", type);
    snprintf(type_buf, sizeof type_buf, "%s", type);
    bool is_load = strcmp(type, "load") == 0;
    JS_FreeCString(ctx, type);
    type = type_buf;
    JSAtom type_atom = JS_NewAtom(ctx, type), on_atom = handler_type ? 0 : JS_NewAtom(ctx, on_attr);
    CssAtom on_css = css_atom_find(on_attr, strlen(on_attr));     /* (0: no element has one) */
    static CssAtom body_tag, html_tag;
    if (!body_tag) {
        css_atom_reserve(2);
        body_tag = css_atom_intern("body", 4);
        html_tag = css_atom_intern("html", 4);
    }

    GCValue out = JS_NewArray(ctx);
    uint32_t k = 0, first_root = root_of(target);
    GCValue item_v = argv[0];
    uint32_t item = target;
    for (int guard = 0; guard < 4096; guard++) {
        /* the entry: item, the target and related target retargeted to it */
        uint32_t t = retarget(target, item);
        JS_SetPropertyUint32(ctx, out, k++, item_v);
        JS_SetPropertyUint32(ctx, out, k++, t == target ? argv[0] : dom_js(ctx, dom_id_of(t)));
        if (!has_related) JS_SetPropertyUint32(ctx, out, k++, JS_NULL);
        else if (!related) JS_SetPropertyUint32(ctx, out, k++, related_v);
        else {
            uint32_t r = retarget(related, item);
            JS_SetPropertyUint32(ctx, out, k++, r == related ? related_v : dom_js(ctx, dom_id_of(r)));
        }
        int flags = 0;
        if (map_has(ctx, item_v, la, type_atom, true)) flags |= EP_LISTENERS;
        if (map_has(ctx, item_v, ha, type_atom, false)) flags |= EP_HANDLER;
        if (g_dom.kind[item] == DOM_K_ELEMENT) {
            if (on_css && dom_get_attr(dom_id_of(item), on_css, NULL)) flags |= EP_ON_ATTR;
            uint32_t p = g_dom.parent[item];
            if (handler_type && g_dom.tag[item] == body_tag && p && g_dom.tag[p] == html_tag &&
                g_dom.kind[g_dom.parent[p]] == DOM_K_DOCUMENT) flags |= EP_ALWAYS;
        }
        if (!handler_type && JS_IsFunction(ctx, JS_GetProperty(ctx, item_v, on_atom))) flags |= EP_ON_PROP;
        JS_SetPropertyUint32(ctx, out, k++, JS_NewInt32(ctx, flags));
        /* get the parent */
        uint32_t next = 0;
        GCValue next_v = JS_UNDEFINED;
        if (g_dom.kind[item] == DOM_K_DOCUMENT) {
            if (!is_load) next_v = JS_GetProperty(ctx, item_v, g_view_atom);
            if (JS_IsObject(next_v)) {      /* the window ends the path */
                uint32_t t2 = retarget(target, 0);
                JS_SetPropertyUint32(ctx, out, k++, next_v);
                JS_SetPropertyUint32(ctx, out, k++, t2 == target ? argv[0] : dom_js(ctx, dom_id_of(t2)));
                if (!has_related) JS_SetPropertyUint32(ctx, out, k++, JS_NULL);
                else if (!related) JS_SetPropertyUint32(ctx, out, k++, related_v);
                else {
                    uint32_t r2 = retarget(related, 0);
                    JS_SetPropertyUint32(ctx, out, k++, r2 == related ? related_v : dom_js(ctx, dom_id_of(r2)));
                }
                JS_SetPropertyUint32(ctx, out, k++, JS_NewInt32(ctx, EP_ALWAYS |
                                     (map_has(ctx, next_v, la, type_atom, true) ? EP_LISTENERS : 0)));
            }
            break;
        }
        if (is_shadow_root(item)) {
            /* an uncomposed event leaves its shadow tree only when it was
             * dispatched outside it */
            if (composed || first_root != item) next = g_dom.host[item];
        } else {
            GCValue slot = g_dom.kind[item] == DOM_K_ELEMENT || g_dom.kind[item] == DOM_K_TEXT
                           ? JS_GetProperty(ctx, item_v, g_slot_atom) : JS_UNDEFINED;
            if (JS_IsObject(slot)) {
                next = slot_of(slot);
                if (next) next_v = slot;
            }
            if (!next) next = g_dom.parent[item];
        }
        if (!next) break;
        item = next;
        item_v = JS_IsObject(next_v) ? next_v : dom_js(ctx, dom_id_of(next));
        if (!JS_IsObject(item_v)) break;
    }
    JS_FreeAtom(ctx, type_atom);
    if (on_atom) JS_FreeAtom(ctx, on_atom);
    return out;
}

void event_path_install(JSContextHandle ctx, GCValue global) {
    JS_DefinePropertyValueStr(ctx, global, "__cyberEventPath",
                              JS_NewCFunction(ctx, js_event_path, "__cyberEventPath", 5), JS_PROP_CONFIGURABLE);
}

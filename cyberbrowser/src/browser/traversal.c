/*
 * traversal.c - NodeIterator and TreeWalker (DOM Standard 6), walking the
 * node store; only a page's filter (a function or an object with
 * acceptNode) is called back.  document.createNodeIterator /
 * createTreeWalker make them; NodeFilter's constants stay in
 * platform_api.js.
 *
 * Each object keeps its root, current node (TreeWalker) or reference node
 * and pointer side (NodeIterator) as NodeIds, and its filter under a hidden
 * property.  Nodes that are not in the store (Attr objects) cannot be roots.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_internal.h"
#include "dom_js.h"

enum { ACCEPT = 1, REJECT = 2, SKIP = 3 };

typedef struct Trav {
    NodeId root, cur;           /* cur: TreeWalker's current node, NodeIterator's reference */
    uint32_t show;
    bool active, before;        /* before: NodeIterator's pointerBeforeReferenceNode */
} Trav;

typedef struct Cell { Trav *t; } Cell;

static JSClassID g_walker_class, g_iter_class;

static Trav *trav_of(JSContextHandle ctx, GCValue obj, JSClassID *which) {
    GCHandle h = JS_GetOpaqueHandle2(ctx, obj, g_walker_class);
    *which = g_walker_class;
    if (h == GC_HANDLE_NULL) { h = JS_GetOpaqueHandle2(ctx, obj, g_iter_class); *which = g_iter_class; }
    if (h == GC_HANDLE_NULL) return NULL;
    Cell *c = (Cell *)gc_deref(h);
    return c ? c->t : NULL;
}

static void finalize(GCValue v, JSClassID id) {
    GCHandle h = JS_GetOpaqueHandle(v, id);
    if (h == GC_HANDLE_NULL) return;
    Cell *c = (Cell *)gc_deref(h);
    if (c) { free(c->t); c->t = NULL; }
}
static void walker_finalizer(JSRuntimeHandle rt, GCValue v) { (void)rt; finalize(v, g_walker_class); }
static void iter_finalizer(JSRuntimeHandle rt, GCValue v) { (void)rt; finalize(v, g_iter_class); }
static void walker_mark(JSRuntimeHandle rt, GCValue v, JS_MarkFunc *m) { GCHandle h = JS_GetOpaqueHandle(v, g_walker_class); if (h != GC_HANDLE_NULL) m(rt, h); }
static void iter_mark(JSRuntimeHandle rt, GCValue v, JS_MarkFunc *m) { GCHandle h = JS_GetOpaqueHandle(v, g_iter_class); if (h != GC_HANDLE_NULL) m(rt, h); }
static JSClassDef g_walker_def = { .class_name = "TreeWalker", .finalizer = walker_finalizer, .gc_mark = walker_mark };
static JSClassDef g_iter_def = { .class_name = "NodeIterator", .finalizer = iter_finalizer, .gc_mark = iter_mark };

/* ---- the store ---- */

static uint32_t S(NodeId n) { return n && dom_valid(n) ? dom_slot(n) : 0; }
static NodeId N(uint32_t s) { return dom_id_of(s); }
#define PARENT(s) g_dom.parent[s]
#define FIRST(s) g_dom.first[s]
#define LAST(s) g_dom.last[s]
#define NEXT(s) g_dom.next[s]
#define PREV(s) g_dom.prev[s]

/* "filter" (DOM 6.1): ACCEPT, REJECT or SKIP (another number: none of
 * them); -1 with an exception pending. */
static int filter(JSContextHandle ctx, GCValue obj, Trav *t, uint32_t s) {
    if (t->active) {
        throw_dom_exception(ctx, "InvalidStateError", "The traversal is already filtering.");
        return -1;
    }
    int type = g_dom.kind[s];
    if (type < 1 || type > 32 || !(t->show & (1u << (type - 1)))) return SKIP;
    GCValue f = JS_GetPropertyStr(ctx, obj, "__filter");
    if (JS_IsNull(f) || JS_IsUndefined(f)) return ACCEPT;
    GCValue node = dom_js(ctx, N(s)), r;
    t->active = true;
    if (JS_IsFunction(ctx, f)) {
        r = JS_Call(ctx, f, JS_UNDEFINED, 1, &node);
    } else {
        GCValue m = JS_GetPropertyStr(ctx, f, "acceptNode");
        if (JS_IsException(m)) { t->active = false; return -1; }
        if (!JS_IsFunction(ctx, m)) { t->active = false; JS_ThrowTypeError(ctx, "acceptNode is not a function"); return -1; }
        r = JS_Call(ctx, m, f, 1, &node);
    }
    t->active = false;
    if (JS_IsException(r)) return -1;
    double d;
    if (JS_ToFloat64(ctx, &d, r)) return -1;
    return d == ACCEPT ? ACCEPT : d == REJECT ? REJECT : d == SKIP ? SKIP : 0;
}

#define FILTER(var, s) do { var = filter(ctx, this_val, t, s); if (var < 0) return JS_EXCEPTION; } while (0)
#define THIS(t) \
    JSClassID which_; \
    Trav *t = trav_of(ctx, this_val, &which_); \
    if (!t) return JS_ThrowTypeError(ctx, "Illegal invocation")

static GCValue set_cur(JSContextHandle ctx, Trav *t, uint32_t s) {
    t->cur = N(s);
    return dom_js(ctx, t->cur);
}

/* ---- TreeWalker ---- */

static GCValue js_parent_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS(t);
    uint32_t root = S(t->root), node = S(t->cur);
    while (node && node != root) {
        node = PARENT(node);
        if (node) {
            int r;
            FILTER(r, node);
            if (r == ACCEPT) return set_cur(ctx, t, node);
        }
    }
    return JS_NULL;
}

/* "traverse children" (magic 1: first, 0: last) */
static GCValue js_children(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int first) {
    (void)argc; (void)argv;
    THIS(t);
    uint32_t root = S(t->root), cur = S(t->cur);
    uint32_t node = cur ? (first ? FIRST(cur) : LAST(cur)) : 0;
    while (node) {
        int r;
        FILTER(r, node);
        if (r == ACCEPT) return set_cur(ctx, t, node);
        if (r == SKIP) {
            uint32_t child = first ? FIRST(node) : LAST(node);
            if (child) { node = child; continue; }
        }
        for (;;) {
            uint32_t sib = first ? NEXT(node) : PREV(node);
            if (sib) { node = sib; break; }
            uint32_t parent = PARENT(node);
            if (!parent || parent == root || parent == cur) return JS_NULL;
            node = parent;
        }
    }
    return JS_NULL;
}

/* "traverse siblings" (magic 1: next, 0: previous) */
static GCValue js_siblings(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int next) {
    (void)argc; (void)argv;
    THIS(t);
    uint32_t root = S(t->root), node = S(t->cur);
    if (!node || node == root) return JS_NULL;
    for (;;) {
        uint32_t sib = next ? NEXT(node) : PREV(node);
        while (sib) {
            node = sib;
            int r;
            FILTER(r, node);
            if (r == ACCEPT) return set_cur(ctx, t, node);
            sib = next ? FIRST(node) : LAST(node);
            if (r == REJECT || !sib) sib = next ? NEXT(node) : PREV(node);
        }
        node = PARENT(node);
        if (!node || node == root) return JS_NULL;
        int r;
        FILTER(r, node);
        if (r == ACCEPT) return JS_NULL;
    }
}

static GCValue js_previous_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS(t);
    uint32_t root = S(t->root), node = S(t->cur);
    while (node && node != root) {
        uint32_t sib = PREV(node);
        while (sib) {
            node = sib;
            int r;
            FILTER(r, node);
            while (r != REJECT && LAST(node)) { node = LAST(node); FILTER(r, node); }
            if (r == ACCEPT) return set_cur(ctx, t, node);
            sib = PREV(node);
        }
        if (node == root || !PARENT(node)) return JS_NULL;
        node = PARENT(node);
        int r;
        FILTER(r, node);
        if (r == ACCEPT) return set_cur(ctx, t, node);
    }
    return JS_NULL;
}

static GCValue js_next_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS(t);
    uint32_t root = S(t->root), node = S(t->cur);
    int r = ACCEPT;
    if (!node) return JS_NULL;
    for (;;) {
        while (r != REJECT && FIRST(node)) {
            node = FIRST(node);
            FILTER(r, node);
            if (r == ACCEPT) return set_cur(ctx, t, node);
        }
        uint32_t tmp = node, sib = 0;
        while (tmp) {
            if (tmp == root) return JS_NULL;
            sib = NEXT(tmp);
            if (sib) break;
            tmp = PARENT(tmp);
        }
        if (!sib) return JS_NULL;
        node = sib;
        FILTER(r, node);
        if (r == ACCEPT) return set_cur(ctx, t, node);
    }
}

static GCValue js_get_current(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS(t);
    return dom_js(ctx, t->cur);
}

static GCValue js_set_current(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS(t);
    NodeId n = argc && JS_IsObject(argv[0]) ? dom_node_of(argv[0]) : DOM_NULL;
    if (!n) return JS_ThrowTypeError(ctx, "Failed to set the 'currentNode' property on 'TreeWalker': The provided value is not of type 'Node'.");
    t->cur = n;
    return JS_UNDEFINED;
}

/* ---- NodeIterator ---- */

static uint32_t following(uint32_t node, uint32_t root) {
    if (FIRST(node)) return FIRST(node);
    for (; node && node != root; node = PARENT(node))
        if (NEXT(node)) return NEXT(node);
    return 0;
}

static uint32_t preceding(uint32_t node, uint32_t root) {
    if (node == root) return 0;
    uint32_t p = PREV(node);
    if (!p) return PARENT(node);
    while (LAST(p)) p = LAST(p);
    return p;
}

/* "traverse" (magic 1: next, 0: previous) */
static GCValue js_iterate(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int next) {
    (void)argc; (void)argv;
    THIS(t);
    uint32_t root = S(t->root), node = S(t->cur);
    bool before = t->before;
    if (!node) return JS_NULL;
    for (;;) {
        if (next) {
            if (!before) { node = following(node, root); if (!node) return JS_NULL; }
            else before = false;
        } else {
            if (before) { node = preceding(node, root); if (!node) return JS_NULL; }
            else before = true;
        }
        int r;
        FILTER(r, node);
        if (r == ACCEPT) break;
    }
    t->cur = N(node);
    t->before = before;
    return dom_js(ctx, t->cur);
}

static GCValue js_before_ref(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS(t);
    return JS_NewBool(ctx, t->before);
}

static GCValue js_detach(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

/* ---- shared attributes ---- */

static GCValue js_root(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS(t);
    return dom_js(ctx, t->root);
}

static GCValue js_what_to_show(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS(t);
    return JS_NewUint32(ctx, t->show);
}

static GCValue js_filter(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS(t);
    (void)t;
    GCValue f = JS_GetPropertyStr(ctx, this_val, "__filter");
    return JS_IsUndefined(f) ? JS_NULL : f;
}

/* document.createTreeWalker (magic 0) / createNodeIterator (magic 1) */
static GCValue js_create(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    (void)this_val;
    NodeId root = argc && JS_IsObject(argv[0]) ? dom_node_of(argv[0]) : DOM_NULL;
    if (!root) return JS_ThrowTypeError(ctx, "Failed to execute '%s' on 'Document': parameter 1 is not of type 'Node'.",
                                        magic ? "createNodeIterator" : "createTreeWalker");
    uint32_t show = 0xFFFFFFFFu;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToUint32(ctx, &show, argv[1])) return JS_EXCEPTION;
    Trav *t = calloc(1, sizeof *t);
    t->root = t->cur = root;
    t->show = show;
    t->before = true;
    JSClassID id = magic ? g_iter_class : g_walker_class;
    GCValue ctor = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), magic ? "NodeIterator" : "TreeWalker");
    GCValue proto = JS_IsObject(ctor) ? JS_GetPropertyStr(ctx, ctor, "prototype") : JS_UNDEFINED;
    GCValue obj = JS_IsObject(proto) ? JS_NewObjectProtoClass(ctx, proto, id) : JS_NewObjectClass(ctx, id);
    if (JS_IsException(obj)) { free(t); return obj; }
    GCHandle h = gc_allocz(sizeof(Cell), JS_GC_OBJ_TYPE_DATA);
    if (h == GC_HANDLE_NULL) { free(t); return JS_ThrowOutOfMemory(ctx); }
    ((Cell *)gc_deref(h))->t = t;
    JS_SetOpaqueHandle(obj, h);
    JS_DefinePropertyValueStr(ctx, obj, "__filter", argc > 2 && !JS_IsUndefined(argv[2]) ? argv[2] : JS_NULL, 0);
    return obj;
}

static GCValue js_illegal(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* ---- installing ---- */

static void getter(JSContextHandle ctx, GCValue p, const char *name, JSCFunction *get, JSCFunction *set) {
    JSAtom a = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, p, a, JS_NewCFunction(ctx, get, name, 0),
                            set ? JS_NewCFunction(ctx, set, name, 1) : JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
}

static void method(JSContextHandle ctx, GCValue p, const char *name, JSCFunctionMagic *fn, int magic) {
    JS_DefinePropertyValueStr(ctx, p, name, JS_NewCFunctionMagic(ctx, fn, name, 0, JS_CFUNC_generic_magic, magic),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
}

static void plain(JSContextHandle ctx, GCValue p, const char *name, JSCFunction *fn) {
    JS_DefinePropertyValueStr(ctx, p, name, JS_NewCFunction(ctx, fn, name, 0),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
}

static GCValue interface(JSContextHandle ctx, GCValue global, const char *name) {
    GCValue proto = JS_NewObject(ctx);
    GCValue ctor = JS_NewCFunction2(ctx, js_illegal, name, 0, JS_CFUNC_constructor_or_func, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_DefinePropertyValueStr(ctx, global, name, ctor, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    GCValue win = JS_GetPropertyStr(ctx, global, "window");
    if (JS_IsObject(win) && !JS_StrictEq(ctx, win, global))
        JS_DefinePropertyValueStr(ctx, win, name, ctor, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    GCValue sym = JS_GetPropertyStr(ctx, global, "Symbol");
    JSAtom tag = JS_ValueToAtom(ctx, JS_GetPropertyStr(ctx, sym, "toStringTag"));
    JS_DefinePropertyValue(ctx, proto, tag, JS_NewString(ctx, name), JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, tag);
    getter(ctx, proto, "root", js_root, NULL);
    getter(ctx, proto, "whatToShow", js_what_to_show, NULL);
    getter(ctx, proto, "filter", js_filter, NULL);
    return proto;
}

/* After platform_api.js (which defines NodeFilter): the interfaces, and
 * document's (and Document.prototype's) create methods. */
void traversal_install(JSContextHandle ctx, GCValue global) {
    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    if (!g_walker_class) JS_NewClassID(&g_walker_class);
    if (!g_iter_class) JS_NewClassID(&g_iter_class);
    if (!JS_IsRegisteredClass(rt, g_walker_class)) JS_NewClass(rt, g_walker_class, &g_walker_def);
    if (!JS_IsRegisteredClass(rt, g_iter_class)) JS_NewClass(rt, g_iter_class, &g_iter_def);

    GCValue tw = interface(ctx, global, "TreeWalker");
    getter(ctx, tw, "currentNode", js_get_current, js_set_current);
    plain(ctx, tw, "parentNode", js_parent_node);
    method(ctx, tw, "firstChild", js_children, 1);
    method(ctx, tw, "lastChild", js_children, 0);
    method(ctx, tw, "nextSibling", js_siblings, 1);
    method(ctx, tw, "previousSibling", js_siblings, 0);
    plain(ctx, tw, "previousNode", js_previous_node);
    plain(ctx, tw, "nextNode", js_next_node);

    GCValue ni = interface(ctx, global, "NodeIterator");
    getter(ctx, ni, "referenceNode", js_get_current, NULL);
    getter(ctx, ni, "pointerBeforeReferenceNode", js_before_ref, NULL);
    method(ctx, ni, "nextNode", js_iterate, 1);
    method(ctx, ni, "previousNode", js_iterate, 0);
    plain(ctx, ni, "detach", js_detach);

    GCValue doc_ctor = JS_GetPropertyStr(ctx, global, "Document");
    GCValue targets[2] = { JS_IsObject(doc_ctor) ? JS_GetPropertyStr(ctx, doc_ctor, "prototype") : JS_UNDEFINED,
                           JS_GetPropertyStr(ctx, global, "document") };
    for (int i = 0; i < 2; i++) {
        if (!JS_IsObject(targets[i])) continue;
        method(ctx, targets[i], "createTreeWalker", js_create, 0);
        method(ctx, targets[i], "createNodeIterator", js_create, 1);
    }
}

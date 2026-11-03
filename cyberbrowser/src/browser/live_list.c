/*
 * live_list.c - the NodeList and HTMLCollection objects (DOM 4.2.10):
 * childNodes, children, getElementsByTagName(NS), getElementsByClassName
 * (live) and querySelectorAll's and the like (static).
 *
 * A list is an object of one class over its source: a kind, a root and a
 * key.  list[i] and an HTMLCollection's list[name] are exotic properties;
 * length, item() and namedItem() are the prototypes'.  A live list keeps
 * its nodes as of the store versions it read them at (structure, and the
 * id/class one for a class list) and finds them again after a change, so
 * indexing it in a loop stays linear.  A static list keeps its nodes'
 * objects in an array.  childNodes and children are each node's one list
 * (a hidden property of it).
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_internal.h"
#include "dom_js.h"
#include "dom_store.h"
#include "dom_query.h"

extern NodeId g_dom_document;

typedef struct LiveList {
    uint8_t kind;               /* LL_* */
    bool collection;            /* HTMLCollection (named properties), else NodeList */
    bool any_ns;                /* LL_TAG_NS: any namespace */
    bool html_doc;              /* LL_TAG: the root's document was HTML when the list was made */
    NodeId root;
    uint32_t ns;                /* LL_TAG_NS */
    char *key;                  /* tag or class names; NULL for none */
    NodeId *ids;                /* the nodes, as of sv/cv */
    uint32_t n, cap;
    uint64_t sv, cv;
    bool read;                  /* ids read at least once */
} LiveList;

/* The object's GC cell: its list, and what it keeps alive (the root's
 * object, or a static list's array of nodes). */
typedef struct ListCell { LiveList *l; GCValue keep; } ListCell;

static JSClassID g_list_class;

static ListCell *cell_of(JSContextHandle ctx, GCValue obj) {
    GCHandle h = JS_IsObject(obj) ? JS_GetOpaqueHandle2(ctx, obj, g_list_class) : GC_HANDLE_NULL;
    return h != GC_HANDLE_NULL ? (ListCell *)gc_deref(h) : NULL;
}

static GCValue global_proto(JSContextHandle ctx, const char *name) {
    GCValue c = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), name);
    return JS_IsObject(c) ? JS_GetPropertyStr(ctx, c, "prototype") : JS_UNDEFINED;
}

static void list_finalizer(JSRuntimeHandle rt, GCValue v) {
    (void)rt;
    GCHandle h = JS_GetOpaqueHandle(v, g_list_class);
    ListCell *c = h != GC_HANDLE_NULL ? (ListCell *)gc_deref(h) : NULL;
    if (!c || !c->l) return;
    free(c->l->ids);
    free(c->l->key);
    free(c->l);
    c->l = NULL;
}

static void list_mark(JSRuntimeHandle rt, GCValue v, JS_MarkFunc *mark) {
    GCHandle h = JS_GetOpaqueHandle(v, g_list_class);
    if (h == GC_HANDLE_NULL) return;
    mark(rt, h);
    ListCell *c = (ListCell *)gc_deref(h);
    if (c) JS_MarkValue(rt, c->keep, mark);
}

/* ---- finding the nodes ---- */

static void push(LiveList *l, NodeId id) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->ids = realloc(l->ids, l->cap * sizeof *l->ids);
    }
    l->ids[l->n++] = id;
}

/* LL_TAG (qualified name; an HTML element in an HTML document matches it
 * lower-cased) and LL_TAG_NS (namespace and local name; "*" any). */
static void find_tag(LiveList *l) {
    NodeId root = l->root;
    bool html_doc = l->html_doc, all = !strcmp(l->key, "*");
    size_t nl = strlen(l->key);
    char lower_buf[128], *lower = nl < sizeof lower_buf ? lower_buf : malloc(nl + 1);
    for (size_t i = 0; i <= nl; i++) lower[i] = (char)(l->key[i] >= 'A' && l->key[i] <= 'Z' ? l->key[i] + 32 : l->key[i]);
    uint32_t r = dom_slot(root);
    for (uint32_t s = dom_next_in_subtree(r, r); s; s = dom_next_in_subtree(s, r)) {
        if (g_dom.kind[s] != DOM_K_ELEMENT) continue;
        NodeId e = dom_id_of(s);
        bool match;
        if (l->kind == LL_TAG) {
            if (all) {
                match = true;
            } else {
                const char *p = dom_prefix(e), *ln = dom_local_name(e);
                const char *want = html_doc && g_dom.ns[s] == DOM_NS_HTML ? lower : l->key;
                size_t pl = p ? strlen(p) : 0;
                match = p ? !strncmp(want, p, pl) && want[pl] == ':' && !strcmp(want + pl + 1, ln) : !strcmp(want, ln);
            }
        } else {
            match = (l->any_ns || g_dom.ns[s] == l->ns) && (all || !strcmp(l->key, dom_local_name(e)));
        }
        if (match) push(l, e);
    }
    if (lower != lower_buf) free(lower);
}

/* The list's nodes, found again if the store changed since. */
static void refresh(LiveList *l) {
    if (l->kind == LL_STATIC) return;
    uint64_t cv = l->kind == LL_CLASS ? g_dom.class_version : 0;
    if (l->read && l->sv == g_dom.structure_version && l->cv == cv) return;
    l->n = 0;
    l->sv = g_dom.structure_version;
    l->cv = cv;
    l->read = true;
    if (!dom_valid(l->root)) return;
    switch (l->kind) {
    case LL_CHILD_NODES:
        for (NodeId c = dom_first_child(l->root); c; c = dom_next_sibling(c)) push(l, c);
        break;
    case LL_CHILDREN:
        for (NodeId c = dom_first_child(l->root); c; c = dom_next_sibling(c))
            if (dom_kind(c) == DOM_K_ELEMENT) push(l, c);
        break;
    case LL_TAG: case LL_TAG_NS:
        find_tag(l);
        break;
    case LL_CLASS: {
        DomNodeList q = dom_query_by_class_in(l->root, l->key, strlen(l->key));
        for (uint32_t i = 0; i < q.n; i++) push(l, q.ids[i]);
        dom_node_list_free(&q);
        break;
    }
    }
}

/* (a static list's n: its array's length, set when made) */
static uint32_t list_length(JSContextHandle ctx, ListCell *c) {
    (void)ctx;
    refresh(c->l);
    return c->l->n;
}

static GCValue list_item(JSContextHandle ctx, ListCell *c, uint32_t i) {
    if (c->l->kind == LL_STATIC) return i < c->l->n ? JS_GetPropertyUint32(ctx, c->keep, i) : JS_NULL;
    refresh(c->l);
    return i < c->l->n ? dom_js(ctx, c->l->ids[i]) : JS_NULL;
}

/* The element of an HTMLCollection named key (DOM 4.2.10.2 namedItem: the
 * first whose id is key, or an HTML element's name); NULL for none. */
static NodeId named(JSContextHandle ctx, ListCell *c, const char *key) {
    if (!key[0]) return DOM_NULL;
    uint32_t n = list_length(ctx, c);
    for (uint32_t i = 0; i < n; i++) {
        NodeId e = c->l->kind == LL_STATIC ? dom_node_of(JS_GetPropertyUint32(ctx, c->keep, i)) : c->l->ids[i];
        if (dom_kind(e) != DOM_K_ELEMENT) continue;
        const char *id = dom_get_attr_str(e, "id");
        if (id && !strcmp(id, key)) return e;
        const char *nm = dom_ns(e) == DOM_NS_HTML ? dom_get_attr_str(e, "name") : NULL;
        if (nm && !strcmp(nm, key)) return e;
    }
    return DOM_NULL;
}

/* (a named property shows when nothing on the prototype chain has the name) */
static bool on_proto_chain(JSContextHandle ctx, GCValue obj, JSAtom prop) {
    GCValue p = JS_GetPrototype(ctx, obj);
    return JS_IsObject(p) && JS_HasProperty(ctx, p, prop) > 0;
}

/* ---- the exotic methods ---- */

static int js_list_get_own(JSContextHandle ctx, JSPropertyDescriptor *desc, GCValue obj, JSAtom prop) {
    ListCell *c = cell_of(ctx, obj);
    if (!c || !c->l) return 0;
    uint32_t i;
    GCValue v = JS_UNDEFINED;
    bool index = JS_AtomArrayIndex(ctx, prop, &i);
    if (index) {
        if (i >= list_length(ctx, c)) return 0;
        v = list_item(ctx, c, i);
    } else {
        if (!c->l->collection || on_proto_chain(ctx, obj, prop)) return 0;
        GCValue key = JS_AtomToValue(ctx, prop);
        if (!JS_IsString(key)) return 0;
        const char *k = JS_ToCString(ctx, key);
        if (!k) return -1;
        NodeId e = named(ctx, c, k);
        JS_FreeCString(ctx, k);
        if (!e) return 0;
        v = dom_js(ctx, e);
    }
    if (desc) {
        desc->flags = index ? JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE : JS_PROP_CONFIGURABLE;
        desc->value = v;
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
    }
    return 1;
}

/* The indices, then (an HTMLCollection's) the supported names: each
 * element's id, then an HTML element's name, each once. */
static int js_list_own_names(JSContextHandle ctx, JSPropertyEnum **ptab, uint32_t *plen, GCValue obj) {
    ListCell *c = cell_of(ctx, obj);
    uint32_t n = c && c->l ? list_length(ctx, c) : 0;
    uint32_t cap = n + (c && c->l && c->l->collection ? 2 * n : 0) + 1;
    GCHandle h = gc_allocz(sizeof(JSPropertyEnum) * cap, JS_GC_OBJ_TYPE_DATA);
    JSPropertyEnum *tab = h != GC_HANDLE_NULL ? (JSPropertyEnum *)gc_deref(h) : NULL;
    if (!tab) return -1;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) tab[k++].atom = JS_NewAtomUInt32(ctx, i);
    if (n && c->l->collection) {
        uint32_t first_name = k;
        for (uint32_t i = 0; i < n; i++) {
            NodeId e = c->l->kind == LL_STATIC ? dom_node_of(JS_GetPropertyUint32(ctx, c->keep, i)) : c->l->ids[i];
            if (dom_kind(e) != DOM_K_ELEMENT) continue;
            const char *names[2] = { dom_get_attr_str(e, "id"), dom_ns(e) == DOM_NS_HTML ? dom_get_attr_str(e, "name") : NULL };
            for (int j = 0; j < 2; j++) {
                if (!names[j] || !names[j][0]) continue;
                JSAtom a = JS_NewAtom(ctx, names[j]);
                bool dup = false;
                for (uint32_t q = first_name; q < k && !dup; q++) dup = tab[q].atom == a;
                uint32_t idx;
                if (dup || JS_AtomArrayIndex(ctx, a, &idx) || on_proto_chain(ctx, obj, a)) { JS_FreeAtom(ctx, a); continue; }
                tab[k++].atom = a;
            }
        }
    }
    *ptab = tab;
    *plen = k;
    return 0;
}

/* No indexed or named setter: an index, or a name a named property shows
 * by, can't be defined; anything else is an ordinary property. */
static int js_list_define(JSContextHandle ctx, GCValue obj, JSAtom prop, GCValue val, GCValue getter, GCValue setter,
                          int flags) {
    ListCell *c = cell_of(ctx, obj);
    uint32_t i;
    if (c && c->l) {
        if (JS_AtomArrayIndex(ctx, prop, &i)) return JS_RefuseProperty(ctx, flags, "Index property setter is not supported.");
        if (c->l->collection && js_list_get_own(ctx, NULL, obj, prop) > 0)
            return JS_RefuseProperty(ctx, flags, "Named property setter is not supported.");
    }
    return JS_DefineProperty(ctx, obj, prop, val, getter, setter, flags | JS_PROP_NO_EXOTIC);
}

/* (an own ordinary property was deleted already: these are the exotic ones) */
static int js_list_delete(JSContextHandle ctx, GCValue obj, JSAtom prop) {
    return js_list_get_own(ctx, NULL, obj, prop) > 0 ? 0 : 1;
}

static JSClassExoticMethods g_list_exotic = {
    .get_own_property = js_list_get_own,
    .get_own_property_names = js_list_own_names,
    .delete_property = js_list_delete,
    .define_own_property = js_list_define,
};

static JSClassDef g_list_def = {
    .class_name = "NodeList",
    .finalizer = list_finalizer,
    .gc_mark = list_mark,
    .exotic = &g_list_exotic,
};

/* ---- making them ---- */

static GCValue make(JSContextHandle ctx, LiveList *l, GCValue keep) {
    GCValue proto = global_proto(ctx, l->collection ? "HTMLCollection" : "NodeList");
    GCValue obj = JS_IsObject(proto) ? JS_NewObjectProtoClass(ctx, proto, g_list_class) : JS_NewObjectClass(ctx, g_list_class);
    GCHandle h = JS_IsException(obj) ? GC_HANDLE_NULL : gc_allocz(sizeof(ListCell), JS_GC_OBJ_TYPE_DATA);
    if (h == GC_HANDLE_NULL) {
        free(l->key);
        free(l);
        return JS_IsException(obj) ? obj : JS_ThrowOutOfMemory(ctx);
    }
    ListCell *c = (ListCell *)gc_deref(h);
    c->l = l;
    c->keep = keep;
    JS_SetOpaqueHandle(obj, h);
    return obj;
}

GCValue live_list_new(JSContextHandle ctx, int kind, NodeId root, const char *key, uint32_t ns, bool any_ns,
                      bool collection) {
    LiveList *l = calloc(1, sizeof *l);
    if (!l) return JS_ThrowOutOfMemory(ctx);
    l->kind = (uint8_t)kind;
    l->collection = collection;
    l->root = root;
    l->ns = ns;
    l->any_ns = any_ns;
    l->key = key ? strdup(key) : NULL;
    NodeId doc = dom_kind(root) == DOM_K_DOCUMENT ? root : (dom_owner(root) ? dom_owner(root) : g_dom_document);
    l->html_doc = dom_is_html_document(doc);
    return make(ctx, l, dom_js(ctx, root));
}

GCValue live_list_static(JSContextHandle ctx, const NodeId *ids, uint32_t n, bool collection) {
    GCValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < n; i++) JS_SetPropertyUint32(ctx, arr, i, dom_js(ctx, ids[i]));
    return live_list_of_array(ctx, arr, collection);
}

GCValue live_list_of_array(JSContextHandle ctx, GCValue arr, bool collection) {
    LiveList *l = calloc(1, sizeof *l);
    if (!l) return JS_ThrowOutOfMemory(ctx);
    l->kind = LL_STATIC;
    l->collection = collection;
    JS_ToUint32(ctx, &l->n, JS_GetPropertyStr(ctx, arr, "length"));
    return make(ctx, l, arr);
}

/* A node's one childNodes (LL_CHILD_NODES) or children (LL_CHILDREN) list. */
GCValue live_list_of_node(JSContextHandle ctx, GCValue node, int kind) {
    NodeId n = dom_node_of(node);
    if (!n) return JS_ThrowTypeError(ctx, "Illegal invocation");
    const char *key = kind == LL_CHILD_NODES ? "__cyber_childNodes" : "__cyber_children";
    GCValue have = JS_GetPropertyStr(ctx, node, key);
    if (JS_IsObject(have)) return have;
    GCValue l = live_list_new(ctx, kind, n, NULL, 0, false, kind == LL_CHILDREN);
    if (JS_IsObject(l)) JS_DefinePropertyValueStr(ctx, node, key, l, JS_PROP_CONFIGURABLE);
    return l;
}

/* ---- the prototypes' members ---- */

/* length: a list's; another object's (a page's array-like over the
 * prototype) is not a list. */
static GCValue js_length(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    ListCell *c = cell_of(ctx, this_val);
    if (!c || !c->l) return JS_ThrowTypeError(ctx, "Illegal invocation");
    return JS_NewUint32(ctx, list_length(ctx, c));
}

/* item(i): a list's i-th node; on an array-like (an array given the
 * prototype by script) its i-th element. */
static GCValue js_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'item': 1 argument required, but only 0 present.");
    uint32_t i = 0;
    if (JS_ToUint32(ctx, &i, argv[0])) return JS_EXCEPTION;
    ListCell *c = cell_of(ctx, this_val);
    if (c && c->l) return list_item(ctx, c, i);
    if (!JS_IsObject(this_val) || !JS_IsArray(ctx, this_val)) return JS_ThrowTypeError(ctx, "Illegal invocation");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, JS_GetPropertyStr(ctx, this_val, "length"));
    if (i >= n) return JS_NULL;
    GCValue v = JS_GetPropertyUint32(ctx, this_val, i);
    return JS_IsUndefined(v) ? JS_NULL : v;
}

static GCValue js_named_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'namedItem': 1 argument required, but only 0 present.");
    const char *k = JS_ToCString(ctx, argv[0]);
    if (!k) return JS_EXCEPTION;
    ListCell *c = cell_of(ctx, this_val);
    GCValue r = JS_NULL;
    if (c && c->l) {
        NodeId e = named(ctx, c, k);
        r = e ? dom_js(ctx, e) : JS_NULL;
    } else if (JS_IsObject(this_val) && JS_IsArray(ctx, this_val) && k[0]) {
        /* (an array-like: its elements by id or name) */
        uint32_t n = 0;
        JS_ToUint32(ctx, &n, JS_GetPropertyStr(ctx, this_val, "length"));
        for (uint32_t i = 0; i < n && JS_IsNull(r); i++) {
            NodeId e = dom_node_of(JS_GetPropertyUint32(ctx, this_val, i));
            const char *id = e ? dom_get_attr_str(e, "id") : NULL, *nm = e ? dom_get_attr_str(e, "name") : NULL;
            if ((id && !strcmp(id, k)) || (nm && !strcmp(nm, k))) r = dom_js(ctx, e);
        }
    } else if (!JS_IsObject(this_val) || !JS_IsArray(ctx, this_val)) {
        JS_FreeCString(ctx, k);
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    }
    JS_FreeCString(ctx, k);
    return r;
}

/* After platform_api.js (which defines NodeList and HTMLCollection): their
 * prototypes' length, item and namedItem. */
void live_list_install(JSContextHandle ctx, GCValue global) {
    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    if (!g_list_class) JS_NewClassID(&g_list_class);
    if (!JS_IsRegisteredClass(rt, g_list_class)) JS_NewClass(rt, g_list_class, &g_list_def);
    static const char *const names[2] = { "NodeList", "HTMLCollection" };
    for (int k = 0; k < 2; k++) {
        GCValue ctor = JS_GetPropertyStr(ctx, global, names[k]);
        GCValue p = JS_IsObject(ctor) ? JS_GetPropertyStr(ctx, ctor, "prototype") : JS_UNDEFINED;
        if (!JS_IsObject(p)) continue;
        JSAtom a = JS_NewAtom(ctx, "length");
        JS_DefinePropertyGetSet(ctx, p, a, JS_NewCFunction(ctx, js_length, "get length", 0), JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
        JS_DefinePropertyValueStr(ctx, p, "item", JS_NewCFunction(ctx, js_item, "item", 1), JS_PROP_C_W_E);
        if (k) JS_DefinePropertyValueStr(ctx, p, "namedItem", JS_NewCFunction(ctx, js_named_item, "namedItem", 1),
                                         JS_PROP_C_W_E);
    }
}

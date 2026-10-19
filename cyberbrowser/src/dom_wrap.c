/* Lazy wrappers and tree-level lifetime.  See dom_wrap.h. */
#include "dom_wrap.h"
#include "browser_api_impl_types.h"
#include "gc_value_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

JSClassID js_dom_tree_class_id = 0;

extern JSClassID js_video_class_id;
extern JSClassID js_shadow_root_class_id;

static DomWrapFactory g_factory;
static JSContextHandle g_ctx;          /* the engine's context (one per process) */
static uint32_t g_wrappers, g_tree_objects;

uint32_t dom_wrap_count(void) { return g_wrappers; }
uint32_t dom_tree_object_count(void) { return g_tree_objects; }

void dom_wrap_set_factory(DomWrapFactory f) { g_factory = f; }

/* ============================================================================
 * Tree objects
 * ============================================================================ */

typedef struct DomTreeObj {
    uint32_t rec;               /* DomTreeRec index; 0 once merged away */
    GCValue arr;                /* wrappers of the tree's nodes */
} DomTreeObj;
typedef GCHandle DomTreeObjHandle;

typedef struct { uint32_t *v; uint32_t n, cap; } SlotVec;
static void sv_push(SlotVec *sv, uint32_t s) {
    if (sv->n == sv->cap) { sv->cap = sv->cap ? sv->cap * 2 : 16; sv->v = realloc(sv->v, sv->cap * sizeof(uint32_t)); }
    sv->v[sv->n++] = s;
}

/* Wrapped slots of the subtree of `root` (including shadow roots and
 * template contents hanging off it). */
static void collect_wrapped(uint32_t root, SlotVec *out) {
    SlotVec stack = { 0 };
    sv_push(&stack, root);
    while (stack.n) {
        uint32_t r = stack.v[--stack.n];
        for (uint32_t s = r; s; s = dom_next_in_subtree(s, r)) {
            if (g_dom.wrapper[s]) sv_push(out, s);
            if (g_dom.shadow[s]) sv_push(&stack, g_dom.shadow[s]);
            if (g_dom.content[s]) sv_push(&stack, g_dom.content[s]);
        }
    }
    free(stack.v);
}

/* A tree object died: every wrapper of its tree died in the same cycle (each
 * references the tree object), so the tree is unreachable from script.  Free
 * it.  Runs on the JS thread from the collector's finalizer step, before the
 * dead objects are swept (handle finalizer: the engine does not call class
 * finalizers). */
static void tree_handle_finalizer(JSRuntimeHandle rt, GCHandle handle, void *payload) {
    (void)rt; (void)payload;
    g_tree_objects--;
    uint32_t rec = 0;
    GCHandle th = JS_GetOpaqueHandle(GC_MKHANDLE(JS_TAG_OBJECT, handle), js_dom_tree_class_id);
    DomTreeObj *t = th ? (DomTreeObj *)gc_deref(th) : NULL;
    if (t) { rec = t->rec; t->rec = 0; }
    if (!rec || rec >= g_dom.ntrees) return;
    DomTreeRec *r = &g_dom.trees[rec];
    if (r->obj != handle) return;                       /* record reused */
    NodeId root = r->root;
    r->obj = 0;
    if (dom_valid(root) && g_dom.tree[dom_slot(root)] == rec && !dom_parent(root) && !dom_host(root) &&
        dom_kind(root) != DOM_K_FREE) {
        /* The wrappers are dead too: drop their records' malloc'd tables and
         * the store's handles, then free the nodes. */
        SlotVec wrapped = { 0 };
        collect_wrapped(dom_slot(root), &wrapped);
        for (uint32_t i = 0; i < wrapped.n; i++) {
            uint32_t s = wrapped.v[i];
            uint32_t w = g_dom.wrapper[s];
            if (!w) continue;
            dom_wrap_release_tables(dom_wrap_of(GC_MKHANDLE(JS_TAG_OBJECT, w)));
            g_dom.wrapper[s] = 0;
            g_wrappers--;
        }
        free(wrapped.v);
        dom_free_subtree(root);
    }
    dom_tree_free(rec);
}

static void dom_tree_obj_mark(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func) {
    GCHandle h = JS_GetOpaqueHandle(val, js_dom_tree_class_id);
    if (h == GC_HANDLE_NULL) return;
    mark_func(rt, h);
    DomTreeObj *t = (DomTreeObj *)gc_deref(h);
    if (t) JS_MarkValue(rt, t->arr, mark_func);
}

static JSClassDef js_dom_tree_class_def = {
    .class_name = "DOMTree",
    .gc_mark = dom_tree_obj_mark,
};

void dom_wrap_init_runtime(JSRuntimeHandle rt) {
    JS_NewClassID(&js_dom_tree_class_id);
    JS_NewClass(rt, js_dom_tree_class_id, &js_dom_tree_class_def);
    dom_store_init();
}

static GCValue tree_obj_value(uint32_t rec) {
    uint32_t h = g_dom.trees[rec].obj;
    return h ? GC_MKHANDLE(JS_TAG_OBJECT, h) : JS_NULL;
}

static GCValue tree_obj_arr(GCValue tobj) {
    GCHandle h = JS_GetOpaqueHandle(tobj, js_dom_tree_class_id);
    DomTreeObj *t = h ? (DomTreeObj *)gc_deref(h) : NULL;
    return t ? t->arr : JS_NULL;
}

/* Create the tree object of record `rec`. */
static GCValue tree_obj_create(uint32_t rec) {
    JSContextHandle ctx = g_ctx;
    GCValue obj = JS_NewObjectClass(ctx, js_dom_tree_class_id);
    GCHandle h = gc_allocz(sizeof(DomTreeObj), JS_GC_OBJ_TYPE_DATA);
    DomTreeObj *t = (DomTreeObj *)gc_deref(h);
    t->rec = rec;
    GCValue arr = JS_NewArray(ctx);
    t = (DomTreeObj *)gc_deref(h);
    GC_SET(t->arr, arr);
    JS_SetOpaqueHandle(obj, h);
    g_dom.trees[rec].obj = GC_VALUE_GET_HANDLE(obj);
    g_dom.trees[rec].wrappers = 0;
    g_tree_objects++;
    gc_add_handle_finalizer(GC_VALUE_GET_HANDLE(obj), tree_handle_finalizer);
    return obj;
}

/* ============================================================================
 * Wrapper records
 * ============================================================================ */

DomWrapHandle dom_wrap_new(NodeId id) {
    GCHandle h = gc_allocz(sizeof(DomWrap), JS_GC_OBJ_TYPE_DATA);
    DomWrap *w = (DomWrap *)gc_deref(h);
    if (!w) return GC_HANDLE_NULL;
    w->id = id;
    w->tree = JS_NULL;
    w->style = JS_NULL;
    w->computed_style = GC_HANDLE_NULL;
    return h;
}

void dom_wrap_mark(JSRuntimeHandle rt, DomWrapHandle wd, JS_MarkFunc *mark_func) {
    if (wd == GC_HANDLE_NULL) return;
    mark_func(rt, wd);
    DomWrap *w = (DomWrap *)gc_deref(wd);
    if (!w) return;
    JS_MarkValue(rt, w->tree, mark_func);
    JS_MarkValue(rt, w->style, mark_func);
    if (w->computed_style != GC_HANDLE_NULL) {
        mark_func(rt, w->computed_style);
        CssComputedStyle *cs = (CssComputedStyle *)gc_deref(w->computed_style);
        if (cs && cs->properties) {
            LFHashTable *t = cs->properties;
            for (uint32_t i = 0; i < t->bucket_count; i++)
                if (t->buckets[i].state == LF_HASH_OCCUPIED && t->buckets[i].value != GC_HANDLE_NULL)
                    mark_func(rt, t->buckets[i].value);
        }
    }
}

void dom_wrap_release_tables(DomWrapHandle wd) {
    DomWrap *w = wd ? (DomWrap *)gc_deref(wd) : NULL;
    if (!w || w->computed_style == GC_HANDLE_NULL) return;
    CssComputedStyle *cs = (CssComputedStyle *)gc_deref(w->computed_style);
    if (cs && cs->properties) {
        lf_hash_destroy(cs->properties);
        cs->properties = NULL;
    }
}

void dom_wrap_finalize(DomWrapHandle wd, GCValue obj) {
    if (wd == GC_HANDLE_NULL) return;
    DomWrap *w = (DomWrap *)gc_deref(wd);
    if (!w) return;
    dom_wrap_release_tables(wd);
    NodeId id = w->id;
    if (dom_valid(id) && g_dom.wrapper[dom_slot(id)] == GC_VALUE_GET_HANDLE(obj)) {
        g_dom.wrapper[dom_slot(id)] = 0;
        g_wrappers--;
    }
}

DomWrapHandle dom_wrap_of(GCValue obj) {
    if (!JS_IsObject(obj)) return GC_HANDLE_NULL;
    GCHandle h = JS_GetOpaqueHandle(obj, js_dom_node_class_id);
    if (h != GC_HANDLE_NULL) return h;
    if (js_video_class_id) {
        GCHandle vh = JS_GetOpaqueHandle(obj, js_video_class_id);
        if (vh != GC_HANDLE_NULL) {
            HTMLVideoElement *v = (HTMLVideoElement *)gc_deref(vh);
            return v ? v->dom_node : GC_HANDLE_NULL;
        }
    }
    if (js_shadow_root_class_id) {
        GCHandle sh = JS_GetOpaqueHandle(obj, js_shadow_root_class_id);
        if (sh != GC_HANDLE_NULL) {
            ShadowRootData *sr = (ShadowRootData *)gc_deref(sh);
            return sr ? sr->dom_node : GC_HANDLE_NULL;
        }
    }
    return GC_HANDLE_NULL;
}

/* ---- foreign objects: handle -> node, removed when the object dies ---- */

typedef struct { uint32_t handle; NodeId id; } ForeignEnt;
static ForeignEnt *g_foreign;
static uint32_t g_foreign_cap, g_foreign_n;

static uint32_t fhash(uint32_t h) { h ^= h >> 16; h *= 0x7feb352dU; h ^= h >> 15; return h; }

static ForeignEnt *foreign_find(uint32_t handle, bool insert) {
    if (insert && (g_foreign_n + 1) * 2 > g_foreign_cap) {
        uint32_t cap = g_foreign_cap ? g_foreign_cap * 2 : 64;
        ForeignEnt *n = calloc(cap, sizeof(ForeignEnt));
        for (uint32_t i = 0; i < g_foreign_cap; i++) {
            if (!g_foreign[i].handle || g_foreign[i].handle == UINT32_MAX) continue;
            uint32_t j = fhash(g_foreign[i].handle) & (cap - 1);
            while (n[j].handle) j = (j + 1) & (cap - 1);
            n[j] = g_foreign[i];
        }
        free(g_foreign);
        g_foreign = n;
        g_foreign_cap = cap;
    }
    if (!g_foreign_cap) return NULL;
    uint32_t j = fhash(handle) & (g_foreign_cap - 1);
    ForeignEnt *tomb = NULL;
    for (uint32_t probe = 0; probe < g_foreign_cap; probe++) {
        ForeignEnt *e = &g_foreign[j];
        if (e->handle == handle) return e;
        if (e->handle == UINT32_MAX) { if (!tomb) tomb = e; }
        else if (!e->handle) {
            if (!insert) return NULL;
            e = tomb ? tomb : e;
            e->handle = handle;
            e->id = DOM_NULL;
            g_foreign_n++;
            return e;
        }
        j = (j + 1) & (g_foreign_cap - 1);
    }
    return NULL;
}

static void foreign_finalizer(JSRuntimeHandle rt, GCHandle handle, void *payload) {
    (void)rt; (void)payload;
    ForeignEnt *e = foreign_find(handle, false);
    if (!e) return;
    NodeId id = e->id;
    e->handle = UINT32_MAX;
    e->id = DOM_NULL;
    g_foreign_n--;
    if (dom_valid(id) && g_dom.wrapper[dom_slot(id)] == handle) {
        g_dom.wrapper[dom_slot(id)] = 0;
        g_wrappers--;
    }
}

NodeId dom_node_of(GCValue obj) {
    if (!JS_IsObject(obj)) return DOM_NULL;
    DomWrapHandle wd = dom_wrap_of(obj);
    if (wd != GC_HANDLE_NULL) {
        DomWrap *w = (DomWrap *)gc_deref(wd);
        NodeId id = w ? w->id : DOM_NULL;
        return dom_valid(id) ? id : DOM_NULL;
    }
    if (!g_foreign_n) return DOM_NULL;
    ForeignEnt *e = foreign_find(GC_VALUE_GET_HANDLE(obj), false);
    return e && dom_valid(e->id) ? e->id : DOM_NULL;
}

/* ---- tree membership of wrappers ---- */

static void wrapper_set_tree(GCValue obj, GCValue tobj) {
    DomWrapHandle wd = dom_wrap_of(obj);
    if (wd != GC_HANDLE_NULL) {
        SAFE_SET(wd, DomWrapHandle, tree, tobj);
    } else {
        /* Foreign object: keep the tree alive through a hidden property. */
        JSAtom a = JS_NewAtom(g_ctx, "__cyber_tree");
        JS_DefinePropertyValue(g_ctx, obj, a, tobj, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        JS_FreeAtom(g_ctx, a);
    }
}

/* Append the wrapper of slot `s` to tree record `rec`'s object. */
static void tree_add_wrapper(uint32_t rec, uint32_t s) {
    GCValue tobj = tree_obj_value(rec);
    if (JS_IsNull(tobj)) tobj = tree_obj_create(rec);
    GCValue arr = tree_obj_arr(tobj);
    GCValue w = GC_MKHANDLE(JS_TAG_OBJECT, g_dom.wrapper[s]);
    uint32_t idx = g_dom.trees[rec].wrappers++;
    JS_SetPropertyUint32(g_ctx, arr, idx, w);
    g_dom.wrap_idx[s] = idx;
    wrapper_set_tree(w, tobj);
}

/* Remove the wrapper of slot `s` from tree record `rec` (swap with last). */
static void tree_remove_wrapper(uint32_t rec, uint32_t s) {
    GCValue tobj = tree_obj_value(rec);
    if (JS_IsNull(tobj)) return;
    GCValue arr = tree_obj_arr(tobj);
    uint32_t n = g_dom.trees[rec].wrappers;
    uint32_t idx = g_dom.wrap_idx[s];
    if (!n || idx >= n) return;
    if (idx != n - 1) {
        GCValue last = JS_GetPropertyUint32(g_ctx, arr, n - 1);
        JS_SetPropertyUint32(g_ctx, arr, idx, last);
        NodeId lid = dom_node_of(last);
        if (dom_valid(lid)) g_dom.wrap_idx[dom_slot(lid)] = idx;
    }
    JS_SetPropertyStr(g_ctx, arr, "length", JS_NewInt32(g_ctx, (int32_t)(n - 1)));
    g_dom.trees[rec].wrappers = n - 1;
}

/* ============================================================================
 * Binding and lookup
 * ============================================================================ */

void dom_bind(JSContextHandle ctx, NodeId id, GCValue obj) {
    if (!dom_valid(id) || !JS_IsObject(obj)) return;
    g_ctx = ctx;
    uint32_t s = dom_slot(id);
    uint32_t h = GC_VALUE_GET_HANDLE(obj);
    if (g_dom.wrapper[s] == h) return;
    DomWrapHandle wd = dom_wrap_of(obj);
    if (wd != GC_HANDLE_NULL) {
        SAFE_SET(wd, DomWrapHandle, id, id);
    } else if (JS_GetClassID(obj) == js_dom_node_class_id) {
        wd = dom_wrap_new(id);
        JS_SetOpaqueHandle(obj, wd);
    } else {
        ForeignEnt *e = foreign_find(h, true);
        e->id = id;
        gc_set_handle_finalizer(h, foreign_finalizer);
    }
    if (!g_dom.wrapper[s]) g_wrappers++;
    g_dom.wrapper[s] = h;
    uint32_t rec = dom_tree_of(id);
    tree_add_wrapper(rec, s);
}

GCValue dom_js_existing(NodeId id) {
    if (!dom_valid(id)) return JS_NULL;
    uint32_t h = g_dom.wrapper[dom_slot(id)];
    return h ? GC_MKHANDLE(JS_TAG_OBJECT, h) : JS_NULL;
}

GCValue dom_js(JSContextHandle ctx, NodeId id) {
    if (!dom_valid(id)) return JS_NULL;
    uint32_t h = g_dom.wrapper[dom_slot(id)];
    if (h) return GC_MKHANDLE(JS_TAG_OBJECT, h);
    g_ctx = ctx;
    if (!g_factory) return JS_NULL;
    GCValue obj = g_factory(ctx, id);
    if (!dom_valid(id)) return JS_NULL;
    if (!g_dom.wrapper[dom_slot(id)] && JS_IsObject(obj)) dom_bind(ctx, id, obj);
    return obj;
}

NodeId dom_node_ensure(JSContextHandle ctx, GCValue obj, int kind) {
    NodeId id = dom_node_of(obj);
    if (id || !JS_IsObject(obj)) return id;
    uint32_t s = dom_alloc_slot(kind, DOM_NULL);
    if (kind == DOM_K_ELEMENT) g_dom.tag[s] = CSS_ATOM_EMPTY;
    id = dom_id_of(s);
    dom_bind(ctx, id, obj);
    return id;
}

/* ============================================================================
 * Structure
 * ============================================================================ */

static void mark_ce_desc(uint32_t s) {
    /* Propagate "contains a custom-element candidate" to the ancestors. */
    for (uint32_t p = g_dom.parent[s] ? g_dom.parent[s] : g_dom.host[s]; p;
         p = g_dom.parent[p] ? g_dom.parent[p] : g_dom.host[p]) {
        if (g_dom.flags[p] & DOMF_CE_DESC) break;
        g_dom.flags[p] |= DOMF_CE_DESC;
    }
}

bool dom_subtree_has_ce(NodeId id) {
    uint16_t f = dom_flags(id);
    return (f & (DOMF_CE_CANDIDATE | DOMF_CE_DESC)) != 0;
}

/* Merge the tree of root slot `cr` into the tree of root slot `pr`. */
static void merge_trees(uint32_t pr, uint32_t cr) {
    uint32_t rp = g_dom.tree[pr], rc = g_dom.tree[cr];
    g_dom.tree[cr] = 0;
    if (!rc) return;
    if (!rp) {
        g_dom.tree[pr] = rc;
        g_dom.trees[rc].root = dom_id_of(pr);
        return;
    }
    /* Both have records: move the smaller set of wrappers. */
    uint32_t big = rp, small = rc;
    if (g_dom.trees[rc].wrappers > g_dom.trees[rp].wrappers && g_dom.trees[rc].obj) { big = rc; small = rp; }
    uint32_t n = g_dom.trees[small].wrappers;
    if (n && g_dom.trees[small].obj) {
        GCValue sarr = tree_obj_arr(tree_obj_value(small));
        for (uint32_t i = 0; i < n; i++) {
            GCValue w = JS_GetPropertyUint32(g_ctx, sarr, i);
            NodeId wid = dom_node_of(w);
            if (!dom_valid(wid)) continue;
            tree_add_wrapper(big, dom_slot(wid));
        }
        JS_SetPropertyStr(g_ctx, sarr, "length", JS_NewInt32(g_ctx, 0));
    }
    g_dom.trees[small].wrappers = 0;
    g_dom.tree[pr] = big;
    g_dom.trees[big].root = dom_id_of(pr);
    /* The small record lives until its tree object is finalized (or now,
     * when it never had one). */
    g_dom.trees[small].root = DOM_NULL;
    if (!g_dom.trees[small].obj) dom_tree_free(small);
}

void dom_attach(NodeId parent, NodeId child, NodeId ref) {
    if (!dom_valid(parent) || !dom_valid(child)) return;
    uint32_t c = dom_slot(child);
    if (g_dom.parent[c]) dom_detach(child, false);
    uint32_t pr = dom_tree_root_slot(dom_slot(parent));
    dom_link_before(parent, child, ref);
    if (pr != c) merge_trees(pr, c);
    if (g_dom.flags[c] & (DOMF_CE_CANDIDATE | DOMF_CE_DESC)) mark_ce_desc(c);
    dom_mark_dirty(parent, DOMF_DIRTY_CHILDREN);
}

void dom_detach(NodeId child, bool free_unwrapped) {
    if (!dom_valid(child)) return;
    uint32_t c = dom_slot(child);
    if (!g_dom.parent[c]) return;
    uint32_t r = dom_tree_root_slot(c);
    uint32_t rec = g_dom.tree[r];
    dom_mark_dirty(dom_id_of(g_dom.parent[c]), DOMF_DIRTY_CHILDREN);
    dom_unlink(child);
    SlotVec wrapped = { 0 };
    if (rec && g_dom.trees[rec].wrappers) collect_wrapped(c, &wrapped);
    if (!wrapped.n) {
        if (free_unwrapped) dom_free_subtree(child);
        free(wrapped.v);
        return;
    }
    uint32_t nrec = dom_tree_new(child);
    for (uint32_t i = 0; i < wrapped.n; i++) {
        tree_remove_wrapper(rec, wrapped.v[i]);
        tree_add_wrapper(nrec, wrapped.v[i]);
    }
    free(wrapped.v);
}

void dom_free_if_unwrapped(NodeId root) {
    if (!dom_valid(root)) return;
    uint32_t r = dom_slot(root);
    if (g_dom.parent[r] || g_dom.host[r]) return;
    SlotVec wrapped = { 0 };
    collect_wrapped(r, &wrapped);
    bool none = wrapped.n == 0;
    free(wrapped.v);
    if (none) dom_free_subtree(root);
}

void dom_attach_shadow(NodeId host, NodeId shadow) {
    if (!dom_valid(host) || !dom_valid(shadow)) return;
    uint32_t h = dom_slot(host), sh = dom_slot(shadow);
    if (g_dom.parent[sh]) dom_detach(shadow, false);
    uint32_t pr = dom_tree_root_slot(h);
    g_dom.shadow[h] = sh;
    g_dom.host[sh] = h;
    g_dom.flags[sh] |= DOMF_SHADOW_ROOT;
    if (pr != sh) merge_trees(pr, sh);
    if (g_dom.flags[sh] & (DOMF_CE_CANDIDATE | DOMF_CE_DESC)) mark_ce_desc(sh);
    g_dom.structure_version++;
}

void dom_set_template_content(NodeId tmpl, NodeId frag) {
    if (!dom_valid(tmpl) || !dom_valid(frag)) return;
    uint32_t t = dom_slot(tmpl), f = dom_slot(frag);
    uint32_t pr = dom_tree_root_slot(t);
    g_dom.content[t] = f;
    g_dom.host[f] = t;
    g_dom.flags[f] |= DOMF_TEMPLATE_CONTENT;
    if (pr != f) merge_trees(pr, f);
}

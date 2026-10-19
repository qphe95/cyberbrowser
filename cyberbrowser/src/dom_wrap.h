/*
 * Lazy JS wrappers over the node store (PARALLEL_DOM.md §6).
 *
 *   - A node gets a JS object (its wrapper) the first time script touches it.
 *     The store records the wrapper's GC handle, so the same node always
 *     returns the same object, and the wrapper records the node's id.
 *   - Lifetime is per tree.  Each tree that has wrappers owns a tree object
 *     (a GC object holding an array of the tree's wrappers) and every wrapper
 *     references its tree object.  So all wrappers of a tree are reachable as
 *     soon as one is: a child's wrapper keeps its detached parent tree alive,
 *     expandos on connected nodes survive (the document's wrapper is reachable
 *     from `document`), and a detached tree whose wrappers are all
 *     unreachable is collected as a unit; the tree object's finalizer then
 *     frees the tree's slots.  This is tree-level wrapper tracing, built from
 *     ordinary GC references, so the concurrent collector needs no extra
 *     barriers: the store's arrays hold no GC references.
 *   - Attaching a subtree merges its tree into the parent's (the smaller set
 *     of wrappers moves); detaching moves the subtree's wrappers to a new
 *     tree, or frees the subtree at once when it has none.
 *
 * Wrapper kinds: objects of the DOM node class carry a DomWrap record as
 * their opaque data; video elements and native shadow roots keep one in
 * their own data; any other object a script passes as a node (polyfill
 * objects) is bound through a side table.
 */
#ifndef DOM_WRAP_H
#define DOM_WRAP_H

#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "dom_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DomWrap {
    NodeId id;
    GCValue tree;               /* the tree object (keeps the tree alive) */
    GCValue style;              /* CSSStyleDeclaration object, created lazily */
    GCHandle computed_style;    /* CssComputedStyle table (getComputedStyle) */
} DomWrap;
typedef GCHandle DomWrapHandle;
#define GC_STRUCT_OF_DomWrapHandle DomWrap
#define GC_STRUCT_OF_DomWrap DomWrap

extern JSClassID js_dom_node_class_id;
extern JSClassID js_dom_tree_class_id;

/* Register the tree class (once per runtime). */
void dom_wrap_init_runtime(JSRuntimeHandle rt);

/* The browser layer creates wrapper objects (prototype, per-kind setup) and
 * must bind them with dom_bind() before returning. */
typedef GCValue (*DomWrapFactory)(JSContextHandle ctx, NodeId id);
void dom_wrap_set_factory(DomWrapFactory f);

/* New DomWrap record (GC data) for `id`. */
DomWrapHandle dom_wrap_new(NodeId id);
/* Mark a DomWrap record and the references it holds. */
void dom_wrap_mark(JSRuntimeHandle rt, DomWrapHandle wd, JS_MarkFunc *mark_func);
/* Finalizer part for objects carrying a DomWrap record (class finalizers;
 * the collector frees whole trees through their tree objects' handle
 * finalizers). */
void dom_wrap_finalize(DomWrapHandle wd, GCValue obj);
/* Free the malloc'd tables a record owns (computed-style table). */
void dom_wrap_release_tables(DomWrapHandle wd);
/* The DomWrap record of an object (DOM class, video, shadow root), or 0. */
DomWrapHandle dom_wrap_of(GCValue obj);

/* Node of a wrapper (DOM_NULL when the object is not bound). */
NodeId dom_node_of(GCValue obj);
/* Wrapper of a node, created on first use; JS_NULL for DOM_NULL. */
GCValue dom_js(JSContextHandle ctx, NodeId id);
/* Wrapper of a node if it has one, else JS_NULL. */
GCValue dom_js_existing(NodeId id);
static inline bool dom_has_wrapper(NodeId id) {
    return dom_valid(id) && g_dom.wrapper[dom_slot(id)] != 0;
}
/* Make `obj` the wrapper of `id` (the node must not have one yet).  For DOM
 * class objects that have no record, one is created and attached. */
void dom_bind(JSContextHandle ctx, NodeId id, GCValue obj);
/* Node of `obj`, binding a new detached element to it when it has none
 * (scripts may hand any object to appendChild). */
NodeId dom_node_ensure(JSContextHandle ctx, GCValue obj, int kind);

/* ---- tree operations (structure + wrapper bookkeeping) ---- */
/* Insert detached `child` under `parent` before `ref` (DOM_NULL appends). */
void dom_attach(NodeId parent, NodeId child, NodeId ref);
/* Unlink `child` from its parent.  When the removed subtree has no wrapper
 * and `free_unwrapped` is set, it is freed at once. */
void dom_detach(NodeId child, bool free_unwrapped);
/* Free the detached subtree `root` when none of its nodes has a wrapper
 * (nothing can reach it any more). */
void dom_free_if_unwrapped(NodeId root);
/* Attach a shadow root fragment to `host`; it joins the host's tree. */
void dom_attach_shadow(NodeId host, NodeId shadow);
/* Give template element `tmpl` its content fragment (joins its tree). */
void dom_set_template_content(NodeId tmpl, NodeId frag);

/* Does the subtree of `id` (an element or its descendants) contain custom
 * element candidates?  Conservative: may say yes after they were removed. */
bool dom_subtree_has_ce(NodeId id);

/* Statistics (tests): wrappers alive, tree objects alive. */
uint32_t dom_wrap_count(void);
uint32_t dom_tree_object_count(void);

#ifdef __cplusplus
}
#endif

#endif /* DOM_WRAP_H */

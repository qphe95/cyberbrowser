/*
 * range_api.c - AbstractRange, Range and StaticRange (DOM 5).
 *
 * A range object holds a row of the boundary-point store (dom_range.h); its
 * attributes read the row, and a Range's row is live: the tree's mutations
 * move it.  Content edits (deleteContents, extractContents, insertNode, ...)
 * go through the nodes' own methods (replaceData, removeChild, appendChild,
 * cloneNode), which queue the mutation records and move live ranges.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_internal.h"
#include "dom_js.h"
#include "dom_range.h"
#include "dom_store.h"
#include "dom_wrap.h"
#include "layout_api.h"

extern NodeId g_dom_document;
bool dom_api_insert_valid(JSContextHandle ctx, NodeId parent, NodeId node, NodeId child);

/* ---- The objects: a cell holding the row ---- */

typedef struct RangeCell {
    int row;
} RangeCell;

static JSClassID js_range_class_id;

static int range_row(GCValue v) {
    GCHandle h = JS_IsObject(v) ? JS_GetOpaqueHandle(v, js_range_class_id) : GC_HANDLE_NULL;
    RangeCell *c = h ? (RangeCell *)gc_deref(h) : NULL;
    return c ? c->row : -1;
}

static void range_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    GCHandle h = JS_GetOpaqueHandle(val, js_range_class_id);
    RangeCell *c = h ? (RangeCell *)gc_deref(h) : NULL;
    if (!c) return;
    dom_range_free(c->row);
    c->row = -1;
}

/* (its cell, and the nodes it is in: they live while it does) */
static void range_mark(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func) {
    GCHandle h = JS_GetOpaqueHandle(val, js_range_class_id);
    if (h == GC_HANDLE_NULL) return;
    mark_func(rt, h);
    RangeCell *c = (RangeCell *)gc_deref(h);
    if (!c || c->row < 0) return;
    NodeId sn, en;
    uint32_t so, eo;
    dom_range_get(c->row, &sn, &so, &en, &eo);
    GCValue w[2] = { dom_js_existing(sn), dom_js_existing(en) };
    for (int i = 0; i < 2; i++)
        if (JS_IsObject(w[i])) mark_func(rt, GC_VALUE_GET_HANDLE(w[i]));
}

static JSClassDef js_range_class_def = {
    .class_name = "Range",
    .finalizer = range_finalizer,
    .gc_mark = range_mark,
};

/* A range object with prototype proto at (n, off): live (a Range) or not. */
static GCValue range_object(JSContextHandle ctx, GCValue proto, NodeId n, uint32_t off, bool live) {
    GCValue obj = JS_NewObjectProtoClass(ctx, proto, js_range_class_id);
    if (JS_IsException(obj)) return obj;
    GCHandle h = gc_allocz(sizeof(RangeCell), JS_GC_OBJ_TYPE_DATA);
    if (h == GC_HANDLE_NULL) return JS_ThrowOutOfMemory(ctx);
    int row = dom_range_new(n, off, live);
    if (row < 0) return JS_ThrowOutOfMemory(ctx);
    ((RangeCell *)gc_deref(h))->row = row;
    JS_SetOpaqueHandle(obj, h);
    return obj;
}

static GCValue global_proto(JSContextHandle ctx, const char *name) {
    GCValue c = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), name);
    return JS_IsObject(c) ? JS_GetPropertyStr(ctx, c, "prototype") : JS_UNDEFINED;
}

/* The document of the realm (a frame's, for its realm's). */
static NodeId realm_document(JSContextHandle ctx) {
    NodeId d = dom_node_of(JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "document"));
    return dom_kind(d) == DOM_K_DOCUMENT ? d : g_dom_document;
}

/* ---- Boundary points (DOM 5.2) ---- */

typedef struct { NodeId n; uint32_t o; } RangeBP;

static uint32_t rng_index(NodeId n) {
    uint32_t i = 0;
    for (NodeId x = dom_prev_sibling(n); x; x = dom_prev_sibling(x)) i++;
    return i;
}

/* A node's length: its data's in UTF-16 code units, 0 for a doctype, else
 * its child count. */
static uint32_t rng_length(NodeId n) {
    int k = dom_kind(n);
    if (dom_kind_is_chardata(k)) {
        uint32_t len = 0, u = 0;
        const unsigned char *t = (const unsigned char *)dom_text(n, &len);
        for (uint32_t i = 0; t && i < len; i++) {
            if ((t[i] & 0xC0) != 0x80) u++;
            if (t[i] >= 0xF0) u++;      /* (a supplementary character: two units) */
        }
        return u;
    }
    if (k == DOM_K_DOCTYPE) return 0;
    uint32_t c = 0;
    for (NodeId x = dom_first_child(n); x; x = dom_next_sibling(x)) c++;
    return c;
}

static NodeId rng_root(NodeId n) {
    while (dom_parent(n)) n = dom_parent(n);
    return n;
}

static bool rng_inclusive_ancestor(NodeId a, NodeId b) {
    for (NodeId x = b; x; x = dom_parent(x))
        if (x == a) return true;
    return false;
}

/* Tree order: is a before b (both in one tree, a != b)? */
static bool rng_before(NodeId a, NodeId b) {
    if (rng_inclusive_ancestor(a, b)) return true;
    if (rng_inclusive_ancestor(b, a)) return false;
    NodeId pa = a, pb = b;
    /* (the children of their common ancestor on each side, compared) */
    for (NodeId x = a; x; x = dom_parent(x)) {
        NodeId y = b;
        for (; y; y = dom_parent(y)) if (dom_parent(y) == dom_parent(x) && dom_parent(x)) break;
        if (y) { pa = x; pb = y; break; }
    }
    for (NodeId s = dom_next_sibling(pa); s; s = dom_next_sibling(s)) if (s == pb) return true;
    return false;
}

/* -1, 0, 1: a before, equal to, after b (DOM "position of a boundary point"). */
static int rng_cmp(RangeBP a, RangeBP b) {
    if (a.n == b.n) return a.o == b.o ? 0 : a.o < b.o ? -1 : 1;
    if (!rng_before(a.n, b.n)) return -rng_cmp(b, a);
    if (rng_inclusive_ancestor(a.n, b.n)) {
        NodeId c = b.n;
        while (dom_parent(c) != a.n) c = dom_parent(c);
        return rng_index(c) < a.o ? 1 : -1;
    }
    return -1;
}

static bool rng_contained(NodeId n, RangeBP s, RangeBP e) {
    RangeBP a = { n, 0 }, z = { n, rng_length(n) };
    return rng_root(n) == rng_root(s.n) && rng_cmp(s, a) < 0 && rng_cmp(z, e) < 0;
}

static bool rng_partial(NodeId n, RangeBP s, RangeBP e) {
    return rng_inclusive_ancestor(n, s.n) != rng_inclusive_ancestor(n, e.n);
}

/* ---- this range ---- */

#define THIS_RANGE(r)                                                        \
    int r = range_row(this_val);                                             \
    if (r < 0) return JS_ThrowTypeError(ctx, "Illegal invocation")

static void rng_get_row(int r, RangeBP *s, RangeBP *e) {
    dom_range_get(r, &s->n, &s->o, &e->n, &e->o);
}

static bool rng_get(GCValue range, RangeBP *s, RangeBP *e) {
    int r = range_row(range);
    if (r < 0) return false;
    rng_get_row(r, s, e);
    return s->n && e->n;
}

static void rng_set_collapsed(GCValue range, NodeId n, uint32_t o) {
    dom_range_set(range_row(range), n, o, n, o);
}

/* ---- Attr nodes as boundary points ----
 * An Attr is no store node: a range's point in one is at its stand-in, an
 * empty fragment of its own (its root, of length 0, as an Attr is), whose
 * object names the Attr (__cyber_attr) and which the Attr keeps (__a_range).
 * The range's attributes give the Attr back. */

static bool is_attr_obj(JSContextHandle ctx, GCValue v) {
    if (!JS_IsObject(v) || dom_node_of(v)) return false;
    JSAtom a = JS_NewAtom(ctx, "__a_name");
    int own = JS_GetOwnProperty(ctx, NULL, v, a);
    JS_FreeAtom(ctx, a);
    return own > 0;
}

static NodeId attr_stand_in(JSContextHandle ctx, GCValue attr) {
    GCValue have = JS_GetPropertyStr(ctx, attr, "__a_range");
    NodeId n = JS_IsObject(have) ? dom_node_of(have) : DOM_NULL;
    if (n) return n;
    GCValue od = JS_GetPropertyStr(ctx, attr, "ownerDocument");
    n = dom_create_fragment(JS_IsObject(od) ? dom_node_of(od) : DOM_NULL);
    GCValue w = dom_js(ctx, n);
    if (!JS_IsObject(w)) return DOM_NULL;
    JS_DefinePropertyValueStr(ctx, w, "__cyber_attr", attr, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, attr, "__a_range", w, JS_PROP_CONFIGURABLE);
    return n;
}

/* The Attr a node stands in for (undefined: none). */
static GCValue stand_in_attr(JSContextHandle ctx, NodeId n) {
    if (dom_kind(n) != DOM_K_FRAGMENT || dom_first_child(n)) return JS_UNDEFINED;
    GCValue w = dom_js(ctx, n);
    GCValue a = JS_IsObject(w) ? JS_GetPropertyStr(ctx, w, "__cyber_attr") : JS_UNDEFINED;
    return JS_IsObject(a) ? a : JS_UNDEFINED;
}

/* A boundary point's node as script sees it (an Attr for its stand-in). */
static GCValue point_js(JSContextHandle ctx, NodeId n) {
    GCValue a = stand_in_attr(ctx, n);
    return JS_IsUndefined(a) ? dom_js(ctx, n) : a;
}

/* A node argument: its store node (an Attr's stand-in), or a TypeError
 * thrown (0). */
static NodeId node_arg(JSContextHandle ctx, int argc, GCValue *argv, int i, const char *method) {
    NodeId n = argc > i && JS_IsObject(argv[i]) ? dom_node_of(argv[i]) : DOM_NULL;
    if (!n && argc > i && is_attr_obj(ctx, argv[i])) n = attr_stand_in(ctx, argv[i]);
    if (!n) JS_ThrowTypeError(ctx, "Failed to execute '%s' on 'Range': parameter %d is not of type 'Node'.", method, i + 1);
    return n;
}

/* An "unsigned long" offset argument. */
static bool offset_arg(JSContextHandle ctx, int argc, GCValue *argv, int i, uint32_t *out) {
    if (argc <= i) {
        JS_ThrowTypeError(ctx, "2 arguments required.");
        return false;
    }
    return JS_ToUint32(ctx, out, argv[i]) == 0;
}

/* DOM "set the start or end": checks, then (start) the end moves too when
 * the point is in another tree or after it, and the same for the end. */
static GCValue rng_set_point(JSContextHandle ctx, int r, NodeId n, uint32_t off, bool start) {
    if (dom_kind(n) == DOM_K_DOCTYPE) return throw_dom_exception(ctx, "InvalidNodeTypeError", "The node is a doctype.");
    if (off > rng_length(n)) return throw_dom_exception(ctx, "IndexSizeError", "The offset is greater than the node's length.");
    RangeBP s, e, bp = { n, off };
    rng_get_row(r, &s, &e);
    bool other = rng_root(n) != rng_root(s.n);
    if (start) {
        if (other || rng_cmp(bp, e) > 0) e = bp;
        s = bp;
    } else {
        if (other || rng_cmp(bp, s) < 0) s = bp;
        e = bp;
    }
    dom_range_set(r, s.n, s.o, e.n, e.o);
    return JS_UNDEFINED;
}

GCValue js_range_set_start(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_RANGE(r);
    NodeId n = node_arg(ctx, argc, argv, 0, "setStart");
    uint32_t off;
    if (!n || !offset_arg(ctx, argc, argv, 1, &off)) return JS_EXCEPTION;
    return rng_set_point(ctx, r, n, off, true);
}

GCValue js_range_set_end(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_RANGE(r);
    NodeId n = node_arg(ctx, argc, argv, 0, "setEnd");
    uint32_t off;
    if (!n || !offset_arg(ctx, argc, argv, 1, &off)) return JS_EXCEPTION;
    return rng_set_point(ctx, r, n, off, false);
}

/* setStartBefore (0), setStartAfter (1), setEndBefore (2), setEndAfter (3) */
static GCValue js_range_set_around(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    THIS_RANGE(r);
    NodeId n = node_arg(ctx, argc, argv, 0, "setStartBefore");
    if (!n) return JS_EXCEPTION;
    NodeId p = dom_parent(n);
    if (!p) return throw_dom_exception(ctx, "InvalidNodeTypeError", "The node has no parent.");
    return rng_set_point(ctx, r, p, rng_index(n) + (magic & 1), magic < 2);
}

GCValue js_range_select_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_RANGE(r);
    NodeId n = node_arg(ctx, argc, argv, 0, "selectNode");
    if (!n) return JS_EXCEPTION;
    NodeId p = dom_parent(n);
    if (!p) return throw_dom_exception(ctx, "InvalidNodeTypeError", "The node has no parent.");
    uint32_t i = rng_index(n);
    dom_range_set(r, p, i, p, i + 1);
    return JS_UNDEFINED;
}

GCValue js_range_select_node_contents(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_RANGE(r);
    NodeId n = node_arg(ctx, argc, argv, 0, "selectNodeContents");
    if (!n) return JS_EXCEPTION;
    if (dom_kind(n) == DOM_K_DOCTYPE) return throw_dom_exception(ctx, "InvalidNodeTypeError", "The node is a doctype.");
    dom_range_set(r, n, 0, n, rng_length(n));
    return JS_UNDEFINED;
}

GCValue js_range_collapse(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_RANGE(r);
    bool to_start = argc > 0 && JS_ToBool(ctx, argv[0]);
    RangeBP s, e;
    rng_get_row(r, &s, &e);
    if (to_start) dom_range_set(r, s.n, s.o, s.n, s.o);
    else dom_range_set(r, e.n, e.o, e.n, e.o);
    return JS_UNDEFINED;
}

GCValue js_range_clone_range(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS_RANGE(r);
    RangeBP s, e;
    rng_get_row(r, &s, &e);
    GCValue c = range_object(ctx, JS_GetPrototype(ctx, this_val), s.n, s.o, true);
    if (!JS_IsException(c)) dom_range_set(range_row(c), s.n, s.o, e.n, e.o);
    return c;
}

GCValue js_range_detach(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

/* compareBoundaryPoints(how, sourceRange) */
static GCValue js_range_compare_boundary_points(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_RANGE(r);
    if (argc < 2) return JS_ThrowTypeError(ctx, "Failed to execute 'compareBoundaryPoints' on 'Range': 2 arguments required.");
    uint32_t how32;
    if (JS_ToUint32(ctx, &how32, argv[0])) return JS_EXCEPTION;
    int src = range_row(argv[1]);
    if (src < 0 || !dom_range_is_live(src))
        return JS_ThrowTypeError(ctx, "Failed to execute 'compareBoundaryPoints' on 'Range': parameter 2 is not of type 'Range'.");
    uint16_t how = (uint16_t)how32;
    if (how > 3) return throw_dom_exception(ctx, "NotSupportedError", "The comparison method is not valid.");
    RangeBP s, e, ss, se;
    rng_get_row(r, &s, &e);
    rng_get_row(src, &ss, &se);
    if (rng_root(s.n) != rng_root(ss.n)) return throw_dom_exception(ctx, "WrongDocumentError", "The ranges are in different trees.");
    RangeBP a = how == 0 || how == 3 ? s : e, b = how == 0 || how == 1 ? ss : se;
    return JS_NewInt32(ctx, rng_cmp(a, b));
}

/* A point argument checked (DOM isPointInRange / comparePoint steps 2-3);
 * false with an exception thrown. */
static bool point_args(JSContextHandle ctx, int argc, GCValue *argv, const char *method, RangeBP *p) {
    p->n = node_arg(ctx, argc, argv, 0, method);
    if (!p->n || !offset_arg(ctx, argc, argv, 1, &p->o)) return false;
    return true;
}

static GCValue point_checks(JSContextHandle ctx, RangeBP p) {
    if (dom_kind(p.n) == DOM_K_DOCTYPE) return throw_dom_exception(ctx, "InvalidNodeTypeError", "The node is a doctype.");
    if (p.o > rng_length(p.n)) return throw_dom_exception(ctx, "IndexSizeError", "The offset is greater than the node's length.");
    return JS_UNDEFINED;
}

static GCValue js_range_is_point_in_range(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_RANGE(r);
    RangeBP p, s, e;
    if (!point_args(ctx, argc, argv, "isPointInRange", &p)) return JS_EXCEPTION;
    rng_get_row(r, &s, &e);
    if (rng_root(p.n) != rng_root(s.n)) return JS_FALSE;
    GCValue x = point_checks(ctx, p);
    if (JS_IsException(x)) return x;
    return JS_NewBool(ctx, rng_cmp(p, s) >= 0 && rng_cmp(p, e) <= 0);
}

static GCValue js_range_compare_point(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_RANGE(r);
    RangeBP p, s, e;
    if (!point_args(ctx, argc, argv, "comparePoint", &p)) return JS_EXCEPTION;
    rng_get_row(r, &s, &e);
    if (rng_root(p.n) != rng_root(s.n)) return throw_dom_exception(ctx, "WrongDocumentError", "The point is in another tree.");
    GCValue x = point_checks(ctx, p);
    if (JS_IsException(x)) return x;
    return JS_NewInt32(ctx, rng_cmp(p, s) < 0 ? -1 : rng_cmp(p, e) > 0 ? 1 : 0);
}

static GCValue js_range_intersects_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_RANGE(r);
    NodeId n = node_arg(ctx, argc, argv, 0, "intersectsNode");
    if (!n) return JS_EXCEPTION;
    RangeBP s, e;
    rng_get_row(r, &s, &e);
    if (rng_root(n) != rng_root(s.n)) return JS_FALSE;
    NodeId p = dom_parent(n);
    if (!p) return JS_TRUE;
    uint32_t i = rng_index(n);
    RangeBP a = { p, i }, b = { p, i + 1 };
    return JS_NewBool(ctx, rng_cmp(a, e) < 0 && rng_cmp(b, s) > 0);
}

/* ---- stringifier: the Text data in the range ---- */

static void str_add_units(char **buf, size_t *len, size_t *cap, NodeId n, uint32_t from, uint32_t to) {
    uint32_t bl = 0;
    const unsigned char *t = (const unsigned char *)dom_text(n, &bl);
    /* (code units [from, to) as bytes) */
    uint32_t u = 0, a = bl, b = bl;
    for (uint32_t i = 0; i <= bl; i++) {
        if (i == bl || (t[i] & 0xC0) != 0x80) {
            if (u == from && a == bl) a = i;
            if (u == to) { b = i; break; }
            if (i < bl) u += t[i] >= 0xF0 ? 2 : 1;
        }
    }
    if (a > b) a = b;
    size_t need = *len + (b - a) + 1;
    if (need > *cap) {
        *cap = need * 2;
        *buf = (char *)realloc(*buf, *cap);
    }
    memcpy(*buf + *len, t + a, b - a);
    *len += b - a;
}

static bool is_text(NodeId n) { return dom_kind(n) == DOM_K_TEXT || dom_kind(n) == DOM_K_CDATA; }

GCValue js_range_to_string(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS_RANGE(r);
    RangeBP s, e;
    rng_get_row(r, &s, &e);
    char *buf = NULL;
    size_t len = 0, cap = 0;
    if (s.n == e.n && is_text(s.n)) {
        str_add_units(&buf, &len, &cap, s.n, s.o, e.o);
    } else {
        if (is_text(s.n)) str_add_units(&buf, &len, &cap, s.n, s.o, rng_length(s.n));
        NodeId root = s.n;
        while (root && !rng_inclusive_ancestor(root, e.n)) root = dom_parent(root);
        for (uint32_t x = dom_slot(root); x; x = dom_next_in_subtree(x, dom_slot(root))) {
            NodeId id = dom_id_of(x);
            if (is_text(id) && rng_contained(id, s, e)) str_add_units(&buf, &len, &cap, id, 0, rng_length(id));
        }
        if (is_text(e.n)) str_add_units(&buf, &len, &cap, e.n, 0, e.o);
    }
    GCValue v = JS_NewStringLen(ctx, buf ? buf : "", len);
    free(buf);
    return v;
}

/* ---- Contents (DOM 5.5) ---- */

static GCValue rng_call(JSContextHandle ctx, NodeId n, const char *m, int argc, GCValue *argv) {
    GCValue o = dom_js(ctx, n);
    return JS_Call(ctx, JS_GetPropertyStr(ctx, o, m), o, argc, argv);
}

static bool rng_replace(JSContextHandle ctx, NodeId n, uint32_t off, uint32_t count) {
    GCValue a[3] = { JS_NewUint32(ctx, off), JS_NewUint32(ctx, count), JS_NewString(ctx, "") };
    return !JS_IsException(rng_call(ctx, n, "replaceData", 3, a));
}

/* A clone of chardata node n holding its data [off, off + count). */
static GCValue rng_clone_data(JSContextHandle ctx, NodeId n, uint32_t off, uint32_t count) {
    GCValue f = JS_FALSE;
    GCValue clone = rng_call(ctx, n, "cloneNode", 1, &f);
    if (JS_IsException(clone)) return clone;
    GCValue a[2] = { JS_NewUint32(ctx, off), JS_NewUint32(ctx, count) };
    GCValue sub = rng_call(ctx, n, "substringData", 2, a);
    if (JS_IsException(sub)) return sub;
    JS_SetPropertyStr(ctx, clone, "data", sub);
    return clone;
}

static bool rng_append(JSContextHandle ctx, GCValue parent, GCValue child) {
    return !JS_IsException(JS_Call(ctx, JS_GetPropertyStr(ctx, parent, "appendChild"), parent, 1, &child));
}

/* Where the range collapses to after its contents go (delete / extract
 * steps "new node" and "new offset"). */
static RangeBP rng_new_point(RangeBP s, RangeBP e) {
    if (rng_inclusive_ancestor(s.n, e.n)) return s;
    NodeId ref = s.n;
    while (dom_parent(ref) && !rng_inclusive_ancestor(dom_parent(ref), e.n)) ref = dom_parent(ref);
    RangeBP p = { dom_parent(ref), rng_index(ref) + 1 };
    return p;
}

GCValue js_range_delete_contents(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    RangeBP s, e;
    if (!rng_get(this_val, &s, &e) || (s.n == e.n && s.o == e.o)) return JS_UNDEFINED;
    if (s.n == e.n && dom_kind_is_chardata(dom_kind(s.n))) {
        if (!rng_replace(ctx, s.n, s.o, e.o - s.o)) return JS_EXCEPTION;
        return JS_UNDEFINED;
    }
    /* The contained nodes whose parent is not contained, in tree order. */
    NodeId common = s.n;
    while (common && !rng_inclusive_ancestor(common, e.n)) common = dom_parent(common);
    NodeId *rm = NULL;
    size_t nrm = 0, cap = 0;
    for (NodeId x = dom_first_child(common); x;) {
        if (rng_contained(x, s, e)) {
            if (nrm == cap) { cap = cap ? cap * 2 : 16; rm = (NodeId *)realloc(rm, cap * sizeof(NodeId)); }
            rm[nrm++] = x;
        } else if (dom_first_child(x)) {
            x = dom_first_child(x);
            continue;
        }
        while (x && !dom_next_sibling(x) && x != common) x = dom_parent(x);
        if (!x || x == common) break;
        x = dom_next_sibling(x);
    }
    RangeBP np = rng_new_point(s, e);
    bool ok = true;
    if (dom_kind_is_chardata(dom_kind(s.n))) ok = rng_replace(ctx, s.n, s.o, rng_length(s.n) - s.o);
    for (size_t i = 0; ok && i < nrm; i++) {
        NodeId p = dom_parent(rm[i]);
        if (!p) continue;
        GCValue c = dom_js(ctx, rm[i]);
        ok = !JS_IsException(rng_call(ctx, p, "removeChild", 1, &c));
    }
    free(rm);
    if (ok && dom_kind_is_chardata(dom_kind(e.n))) ok = rng_replace(ctx, e.n, 0, e.o);
    if (!ok) return JS_EXCEPTION;
    rng_set_collapsed(this_val, np.n, np.o);
    return JS_UNDEFINED;
}

/* DOM "extract" (move) or "clone the contents" (!move) of [s, e]. */
static GCValue rng_contents(JSContextHandle ctx, RangeBP s, RangeBP e, bool move, RangeBP *np_out) {
    NodeId doc = dom_kind(s.n) == DOM_K_DOCUMENT ? s.n : dom_owner(s.n);
    GCValue dj = dom_js(ctx, doc);
    GCValue frag = JS_Call(ctx, JS_GetPropertyStr(ctx, dj, "createDocumentFragment"), dj, 0, NULL);
    if (JS_IsException(frag)) return frag;
    if (np_out) *np_out = s;
    if (s.n == e.n && s.o == e.o) return frag;
    if (s.n == e.n && dom_kind_is_chardata(dom_kind(s.n))) {
        GCValue c = rng_clone_data(ctx, s.n, s.o, e.o - s.o);
        if (JS_IsException(c) || !rng_append(ctx, frag, c) || (move && !rng_replace(ctx, s.n, s.o, e.o - s.o)))
            return JS_EXCEPTION;
        return frag;
    }
    NodeId common = s.n;
    while (common && !rng_inclusive_ancestor(common, e.n)) common = dom_parent(common);
    NodeId first = DOM_NULL, last = DOM_NULL;
    NodeId *kids = NULL;
    size_t nk = 0, cap = 0;
    for (NodeId c = dom_first_child(common); c; c = dom_next_sibling(c)) {
        if (!rng_inclusive_ancestor(s.n, e.n) && !first && rng_partial(c, s, e)) first = c;
        if (!rng_inclusive_ancestor(e.n, s.n) && rng_partial(c, s, e)) last = c;
        if (rng_contained(c, s, e)) {
            if (dom_kind(c) == DOM_K_DOCTYPE) { free(kids); return throw_dom_exception(ctx, "HierarchyRequestError", "A doctype cannot be extracted."); }
            if (nk == cap) { cap = cap ? cap * 2 : 16; kids = (NodeId *)realloc(kids, cap * sizeof(NodeId)); }
            kids[nk++] = c;
        }
    }
    if (np_out) *np_out = rng_new_point(s, e);
    bool ok = true;
    GCValue f = JS_FALSE, t = JS_TRUE;
    if (first && dom_kind_is_chardata(dom_kind(first))) {
        GCValue c = rng_clone_data(ctx, s.n, s.o, rng_length(s.n) - s.o);
        ok = !JS_IsException(c) && rng_append(ctx, frag, c) && (!move || rng_replace(ctx, s.n, s.o, rng_length(s.n) - s.o));
    } else if (first) {
        GCValue clone = rng_call(ctx, first, "cloneNode", 1, &f);
        RangeBP se = { first, rng_length(first) };
        GCValue sub = JS_IsException(clone) ? clone : rng_contents(ctx, s, se, move, NULL);
        ok = !JS_IsException(sub) && rng_append(ctx, frag, clone) && rng_append(ctx, clone, sub);
    }
    for (size_t i = 0; ok && i < nk; i++) {
        GCValue c = move ? dom_js(ctx, kids[i]) : rng_call(ctx, kids[i], "cloneNode", 1, &t);
        ok = !JS_IsException(c) && rng_append(ctx, frag, c);
    }
    free(kids);
    if (ok && last && dom_kind_is_chardata(dom_kind(last))) {
        GCValue c = rng_clone_data(ctx, e.n, 0, e.o);
        ok = !JS_IsException(c) && rng_append(ctx, frag, c) && (!move || rng_replace(ctx, e.n, 0, e.o));
    } else if (ok && last) {
        GCValue clone = rng_call(ctx, last, "cloneNode", 1, &f);
        RangeBP ls = { last, 0 };
        GCValue sub = JS_IsException(clone) ? clone : rng_contents(ctx, ls, e, move, NULL);
        ok = !JS_IsException(sub) && rng_append(ctx, frag, clone) && rng_append(ctx, clone, sub);
    }
    return ok ? frag : JS_EXCEPTION;
}

GCValue js_range_extract_contents(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS_RANGE(r);
    RangeBP s, e, np;
    rng_get_row(r, &s, &e);
    GCValue frag = rng_contents(ctx, s, e, true, &np);
    if (JS_IsException(frag)) return frag;
    rng_set_collapsed(this_val, np.n, np.o);
    return frag;
}

static GCValue js_range_clone_contents(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS_RANGE(r);
    RangeBP s, e;
    rng_get_row(r, &s, &e);
    return rng_contents(ctx, s, e, false, NULL);
}

/* DOM "insert" into a range: at its start, splitting a Text there. */
static GCValue rng_insert(JSContextHandle ctx, GCValue range, GCValue node) {
    RangeBP s, e;
    if (!rng_get(range, &s, &e)) return JS_ThrowTypeError(ctx, "Illegal invocation");
    if (is_attr_obj(ctx, node) || !JS_IsUndefined(stand_in_attr(ctx, s.n)))
        return throw_dom_exception(ctx, "HierarchyRequestError", "An attribute cannot have or be a child.");
    NodeId n = JS_IsObject(node) ? dom_node_of(node) : DOM_NULL;
    if (!n) return JS_ThrowTypeError(ctx, "Failed to execute 'insertNode' on 'Range': parameter 1 is not of type 'Node'.");
    int sk = dom_kind(s.n);
    if (sk == DOM_K_PI || sk == DOM_K_COMMENT || (is_text(s.n) && !dom_parent(s.n)) || s.n == n)
        return throw_dom_exception(ctx, "HierarchyRequestError", "The node cannot be inserted there.");
    NodeId ref = DOM_NULL;
    if (is_text(s.n)) ref = s.n;
    else {
        ref = dom_first_child(s.n);
        for (uint32_t i = 0; ref && i < s.o; i++) ref = dom_next_sibling(ref);
    }
    NodeId parent = ref ? dom_parent(ref) : s.n;
    if (!dom_api_insert_valid(ctx, parent, n, ref)) return JS_EXCEPTION;
    GCValue pj = dom_js(ctx, parent);
    if (is_text(s.n)) {
        GCValue off = JS_NewUint32(ctx, s.o);
        GCValue split = rng_call(ctx, s.n, "splitText", 1, &off);
        if (JS_IsException(split)) return split;
        ref = dom_node_of(split);
    }
    if (ref == n) ref = dom_next_sibling(n);
    if (dom_parent(n)) {
        GCValue c = node;
        GCValue r = rng_call(ctx, dom_parent(n), "removeChild", 1, &c);
        if (JS_IsException(r)) return r;
    }
    uint32_t off = ref ? rng_index(ref) : rng_length(parent);
    off += dom_kind(n) == DOM_K_FRAGMENT ? rng_length(n) : 1;
    GCValue args[2] = { node, ref ? dom_js(ctx, ref) : JS_NULL };
    GCValue r = js_node_insertBefore_real(ctx, pj, 2, args);
    if (JS_IsException(r)) return r;
    rng_get(range, &s, &e);
    if (s.n == e.n && s.o == e.o) dom_range_set(range_row(range), s.n, s.o, parent, off);
    return JS_UNDEFINED;
}

GCValue js_range_insert_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_RANGE(r);
    (void)r;
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'insertNode' on 'Range': 1 argument required.");
    return rng_insert(ctx, this_val, argv[0]);
}

GCValue js_range_surround_contents(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_RANGE(r);
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'surroundContents' on 'Range': 1 argument required.");
    NodeId np = JS_IsObject(argv[0]) ? dom_node_of(argv[0]) : DOM_NULL;
    if (!np) return JS_ThrowTypeError(ctx, "Failed to execute 'surroundContents' on 'Range': parameter 1 is not of type 'Node'.");
    RangeBP s, e;
    rng_get_row(r, &s, &e);
    if (!JS_IsUndefined(stand_in_attr(ctx, s.n)))
        return throw_dom_exception(ctx, "HierarchyRequestError", "An attribute cannot have a child.");
    /* (a node other than a Text partly in it) */
    NodeId common = s.n;
    while (common && !rng_inclusive_ancestor(common, e.n)) common = dom_parent(common);
    for (NodeId x = s.n; x && x != common; x = dom_parent(x))
        if (!is_text(x) && rng_partial(x, s, e)) return throw_dom_exception(ctx, "InvalidStateError", "The range partially contains a node.");
    for (NodeId x = e.n; x && x != common; x = dom_parent(x))
        if (!is_text(x) && rng_partial(x, s, e)) return throw_dom_exception(ctx, "InvalidStateError", "The range partially contains a node.");
    int k = dom_kind(np);
    if (k == DOM_K_DOCUMENT || k == DOM_K_DOCTYPE || k == DOM_K_FRAGMENT)
        return throw_dom_exception(ctx, "InvalidNodeTypeError", "The node cannot surround the range's contents.");
    GCValue frag = js_range_extract_contents(ctx, this_val, 0, NULL);
    if (JS_IsException(frag)) return frag;
    if (dom_first_child(np)) {
        GCValue res = js_element_replaceChildren(ctx, argv[0], 0, NULL);
        if (JS_IsException(res)) return res;
    }
    GCValue res = rng_insert(ctx, this_val, argv[0]);
    if (JS_IsException(res)) return res;
    if (!rng_append(ctx, argv[0], frag)) return JS_EXCEPTION;
    return js_range_select_node(ctx, this_val, 1, argv);
}

/* ---- geometry (CSSOM View 6.): what the range selects, box by box ---- */

/* UTF-16 offset to the byte of a text node's data. */
static size_t rng_byte_of(NodeId n, uint32_t units) {
    uint32_t bl = 0, u = 0;
    const unsigned char *t = (const unsigned char *)dom_text(n, &bl);
    for (uint32_t i = 0; t && i < bl; i++) {
        if ((t[i] & 0xC0) == 0x80) continue;
        if (u >= units) return i;
        u += t[i] >= 0xF0 ? 2 : 1;
    }
    return bl;
}

enum { RNG_RECTS = 256 };

static void rng_rects_of(JSContextHandle ctx, NodeId n, RangeBP s, RangeBP e, double (*out)[4], int *nr) {
    if (*nr >= RNG_RECTS) return;
    if (is_text(n)) {
        RangeBP a = { n, 0 }, z = { n, rng_length(n) };
        RangeBP from = rng_cmp(s, a) > 0 ? s : a, to = rng_cmp(e, z) < 0 ? e : z;
        if (rng_cmp(from, to) > 0) return;
        if (from.n != n) from = a;
        if (to.n != n) to = z;
        *nr += layout_query_text_rects(ctx, n, rng_byte_of(n, from.o), rng_byte_of(n, to.o), out + *nr, RNG_RECTS - *nr);
        return;
    }
    if (dom_kind(n) == DOM_K_ELEMENT && rng_contained(n, s, e)) {
        *nr += layout_query_client_rects(ctx, dom_js(ctx, n), out + *nr, RNG_RECTS - *nr);
        /* (and what's in it: its text's own lines are what matter to a selection) */
        return;
    }
    for (NodeId c = dom_first_child(n); c; c = dom_next_sibling(c)) {
        RangeBP before = { n, rng_index(c) }, after = { n, rng_index(c) + 1 };
        if (rng_cmp(after, s) < 0 || rng_cmp(before, e) > 0) continue;
        rng_rects_of(ctx, c, s, e, out, nr);
    }
}

static int rng_rects(JSContextHandle ctx, GCValue this_val, double (*out)[4]) {
    RangeBP s, e;
    if (!rng_get(this_val, &s, &e)) return 0;
    NodeId common = s.n;
    while (common && !rng_inclusive_ancestor(common, e.n)) common = dom_parent(common);
    int n = 0;
    if (common) rng_rects_of(ctx, common, s, e, out, &n);
    return n;
}

static GCValue rng_rect_object(JSContextHandle ctx, const double *r) {
    GCValue ctor = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "DOMRect");
    GCValue args[4] = { JS_NewFloat64(ctx, r[0]), JS_NewFloat64(ctx, r[1]), JS_NewFloat64(ctx, r[2]), JS_NewFloat64(ctx, r[3]) };
    return JS_CallConstructor(ctx, ctor, 4, args);
}

GCValue js_range_get_bounding_client_rect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    static double r[RNG_RECTS][4];
    int n = rng_rects(ctx, this_val, r);
    /* (the union of those with an area; else the first) */
    double u[4] = { 0, 0, 0, 0 };
    bool any = false;
    if (n > 0) memcpy(u, r[0], sizeof u);
    for (int i = 0; i < n; i++) {
        if (r[i][2] <= 0 && r[i][3] <= 0) continue;
        if (!any) { memcpy(u, r[i], sizeof u); any = true; continue; }
        double x0 = u[0] < r[i][0] ? u[0] : r[i][0], y0 = u[1] < r[i][1] ? u[1] : r[i][1];
        double x1 = u[0] + u[2] > r[i][0] + r[i][2] ? u[0] + u[2] : r[i][0] + r[i][2];
        double y1 = u[1] + u[3] > r[i][1] + r[i][3] ? u[1] + u[3] : r[i][1] + r[i][3];
        u[0] = x0; u[1] = y0; u[2] = x1 - x0; u[3] = y1 - y0;
    }
    return rng_rect_object(ctx, u);
}

GCValue js_range_get_client_rects(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    static double r[RNG_RECTS][4];
    int n = rng_rects(ctx, this_val, r);
    GCValue list = JS_NewArray(ctx);
    for (int i = 0; i < n; i++) JS_SetPropertyUint32(ctx, list, (uint32_t)i, rng_rect_object(ctx, r[i]));
    GCValue item = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "__cyberRectListItem");
    if (JS_IsFunction(ctx, item)) JS_SetPropertyStr(ctx, list, "item", item);
    return list;
}

/* ---- AbstractRange's attributes, Range's commonAncestorContainer ---- */

static GCValue js_range_attr(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    (void)argc; (void)argv;
    THIS_RANGE(r);
    RangeBP s, e;
    rng_get_row(r, &s, &e);
    switch (magic) {
    case 0: return point_js(ctx, s.n);
    case 1: return JS_NewUint32(ctx, s.o);
    case 2: return point_js(ctx, e.n);
    case 3: return JS_NewUint32(ctx, e.o);
    case 4: return JS_NewBool(ctx, s.n == e.n && s.o == e.o);
    default: {
        NodeId c = s.n;
        while (c && !rng_inclusive_ancestor(c, e.n)) c = dom_parent(c);
        return point_js(ctx, c);
    }
    }
}

/* ---- Constructors ---- */

GCValue js_range_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    if (JS_IsUndefined(new_target)) return JS_ThrowTypeError(ctx, "Failed to construct 'Range': Please use the 'new' operator.");
    GCValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (!JS_IsObject(proto)) proto = global_proto(ctx, "Range");
    return range_object(ctx, proto, realm_document(ctx), 0, true);
}

/* document.createRange(): a Range at (the document, 0). */
GCValue js_document_create_range(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId d = dom_node_of(this_val);
    if (dom_kind(d) != DOM_K_DOCUMENT) d = realm_document(ctx);
    return range_object(ctx, global_proto(ctx, "Range"), d, 0, true);
}

/* new StaticRange({startContainer, startOffset, endContainer, endOffset}) */
static GCValue js_static_range_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "Failed to construct 'StaticRange': Please use the 'new' operator.");
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "Failed to construct 'StaticRange': 1 argument required.");
    static const char *const names[4] = { "startContainer", "startOffset", "endContainer", "endOffset" };
    GCValue v[4];
    for (int i = 0; i < 4; i++) {
        v[i] = JS_GetPropertyStr(ctx, argv[0], names[i]);
        if (JS_IsException(v[i])) return v[i];
        if (JS_IsUndefined(v[i]))
            return JS_ThrowTypeError(ctx, "Failed to construct 'StaticRange': required member %s is undefined.", names[i]);
    }
    NodeId sn = JS_IsObject(v[0]) ? dom_node_of(v[0]) : DOM_NULL, en = JS_IsObject(v[2]) ? dom_node_of(v[2]) : DOM_NULL;
    /* (an Attr is an object of nodeType 2 here) */
    for (int i = 0; i < 4; i += 2) {
        int32_t nt = 0;
        if (JS_IsObject(v[i]) && !dom_node_of(v[i]))
            JS_ToInt32(ctx, &nt, JS_GetPropertyStr(ctx, v[i], "nodeType"));
        if (nt == 2) return throw_dom_exception(ctx, "InvalidNodeTypeError", "The container is an Attr.");
    }
    if (!sn || !en) return JS_ThrowTypeError(ctx, "Failed to construct 'StaticRange': the containers must be Nodes.");
    if (dom_kind(sn) == DOM_K_DOCTYPE || dom_kind(en) == DOM_K_DOCTYPE)
        return throw_dom_exception(ctx, "InvalidNodeTypeError", "The container is a doctype.");
    uint32_t so, eo;
    if (JS_ToUint32(ctx, &so, v[1]) || JS_ToUint32(ctx, &eo, v[3])) return JS_EXCEPTION;
    GCValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (!JS_IsObject(proto)) proto = global_proto(ctx, "StaticRange");
    GCValue obj = range_object(ctx, proto, sn, so, false);
    if (!JS_IsException(obj)) dom_range_set(range_row(obj), sn, so, en, eo);
    return obj;
}

static GCValue illegal_ctor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

static void getter(JSContextHandle ctx, GCValue proto, const char *name, int magic) {
    JSAtom a = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunctionMagic(ctx, js_range_attr, name, 0, JS_CFUNC_generic_magic, magic),
                            JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
}

static GCValue interface(JSContextHandle ctx, GCValue global, const char *name, JSCFunction *ctor, int length,
                         GCValue parent) {
    GCValue proto = JS_NewObject(ctx);
    if (JS_IsObject(parent)) JS_SetPrototype(ctx, proto, parent);
    GCValue c = JS_NewCFunction2(ctx, ctor ? ctor : illegal_ctor, name, length, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, c, proto);
    JS_DefinePropertyValueStr(ctx, global, name, c, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    GCValue win = JS_GetPropertyStr(ctx, global, "window");
    if (JS_IsObject(win) && !JS_StrictEq(ctx, win, global))
        JS_DefinePropertyValueStr(ctx, win, name, c, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    GCValue sym = JS_GetPropertyStr(ctx, global, "Symbol");
    JSAtom tag = JS_ValueToAtom(ctx, JS_GetPropertyStr(ctx, sym, "toStringTag"));
    JS_DefinePropertyValue(ctx, proto, tag, JS_NewString(ctx, name), JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, tag);
    return proto;
}

static const JSCFunctionListEntry js_range_funcs[] = {
    JS_CFUNC_DEF("setStart", 2, js_range_set_start),
    JS_CFUNC_DEF("setEnd", 2, js_range_set_end),
    JS_CFUNC_MAGIC_DEF("setStartBefore", 1, js_range_set_around, 0),
    JS_CFUNC_MAGIC_DEF("setStartAfter", 1, js_range_set_around, 1),
    JS_CFUNC_MAGIC_DEF("setEndBefore", 1, js_range_set_around, 2),
    JS_CFUNC_MAGIC_DEF("setEndAfter", 1, js_range_set_around, 3),
    JS_CFUNC_DEF("collapse", 0, js_range_collapse),
    JS_CFUNC_DEF("selectNode", 1, js_range_select_node),
    JS_CFUNC_DEF("selectNodeContents", 1, js_range_select_node_contents),
    JS_CFUNC_DEF("compareBoundaryPoints", 2, js_range_compare_boundary_points),
    JS_CFUNC_DEF("deleteContents", 0, js_range_delete_contents),
    JS_CFUNC_DEF("extractContents", 0, js_range_extract_contents),
    JS_CFUNC_DEF("cloneContents", 0, js_range_clone_contents),
    JS_CFUNC_DEF("insertNode", 1, js_range_insert_node),
    JS_CFUNC_DEF("surroundContents", 1, js_range_surround_contents),
    JS_CFUNC_DEF("cloneRange", 0, js_range_clone_range),
    JS_CFUNC_DEF("detach", 0, js_range_detach),
    JS_CFUNC_DEF("isPointInRange", 2, js_range_is_point_in_range),
    JS_CFUNC_DEF("comparePoint", 2, js_range_compare_point),
    JS_CFUNC_DEF("intersectsNode", 1, js_range_intersects_node),
    JS_CFUNC_DEF("toString", 0, js_range_to_string),
    JS_CFUNC_DEF("getBoundingClientRect", 0, js_range_get_bounding_client_rect),
    JS_CFUNC_DEF("getClientRects", 0, js_range_get_client_rects),
};

/* AbstractRange, Range (with its constants) and StaticRange on the global. */
void range_api_install(JSContextHandle ctx, GCValue global) {
    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    if (!js_range_class_id) JS_NewClassID(&js_range_class_id);
    if (!JS_IsRegisteredClass(rt, js_range_class_id)) JS_NewClass(rt, js_range_class_id, &js_range_class_def);
    GCValue ar = interface(ctx, global, "AbstractRange", NULL, 0, JS_UNDEFINED);
    getter(ctx, ar, "startContainer", 0);
    getter(ctx, ar, "startOffset", 1);
    getter(ctx, ar, "endContainer", 2);
    getter(ctx, ar, "endOffset", 3);
    getter(ctx, ar, "collapsed", 4);
    GCValue range = interface(ctx, global, "Range", (JSCFunction *)js_range_constructor, 0, ar);
    getter(ctx, range, "commonAncestorContainer", 5);
    JS_SetPropertyFunctionList(ctx, range, js_range_funcs, (int)(sizeof js_range_funcs / sizeof js_range_funcs[0]));
    GCValue ctor = JS_GetPropertyStr(ctx, range, "constructor");
    static const char *const k[] = { "START_TO_START", "START_TO_END", "END_TO_END", "END_TO_START" };
    for (int i = 0; i < 4; i++) {
        JS_DefinePropertyValueStr(ctx, range, k[i], JS_NewInt32(ctx, i), JS_PROP_ENUMERABLE);
        JS_DefinePropertyValueStr(ctx, ctor, k[i], JS_NewInt32(ctx, i), JS_PROP_ENUMERABLE);
    }
    interface(ctx, global, "StaticRange", (JSCFunction *)js_static_range_constructor, 1, ar);
}

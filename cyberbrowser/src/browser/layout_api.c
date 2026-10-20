/*
 * Layout metrics for scripts (CSSOM View): offsetWidth/Height/Top/Left,
 * offsetParent, clientWidth/Height/Top/Left, scrollWidth/Height,
 * scrollTop/Left and the box behind getBoundingClientRect.
 *
 * Like a browser's forced reflow, the first query after the document changed
 * lays it out again; later queries reuse that layout.  "Changed" is the
 * render snapshot's own test (the serialized document hashes differently),
 * so inline style writes (el.style.width = ...) count, which the store's
 * version counters do not see.  The layout runs at window.innerWidth x
 * innerHeight, the viewport matchMedia evaluates against.
 *
 * Elements map to boxes through the snapshot's store ids, so an element the
 * snapshot does not contain (detached, or in a tree that is not rendered)
 * has no box and reports zeros, as display:none elements do in browsers.
 * Scrolling is not modelled: scrollTop/Left read 0 and ignore writes.
 */
#include "layout_api.h"

#include "cyber_env.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "css_layout.h"
#include "dom_store.h"
#include "dom_wrap.h"
#include "html_dom.h"
#include "platform.h"
#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"

/* The cached forced layout. */
static LayoutContext g_lq_layout;
static HtmlDocument *g_lq_doc;
static bool g_lq_valid;
static uint64_t g_lq_key;
static double g_lq_vw, g_lq_vh;
static bool g_lq_running;
/* Store slot -> layout box index (+1; 0 = no box), and the NodeId that
 * owned the slot when the layout ran (a reused slot is a different node). */
static int *g_lq_box_of_slot;
static NodeId *g_lq_id_of_slot;
static uint32_t g_lq_slot_cap;

static uint64_t g_lq_queries, g_lq_layouts, g_lq_layout_ms;

void layout_api_stats(uint64_t *queries, uint64_t *layouts, uint64_t *layout_ms)
{
    if (queries) *queries = g_lq_queries;
    if (layouts) *layouts = g_lq_layouts;
    if (layout_ms) *layout_ms = g_lq_layout_ms;
}

static void lq_free(void)
{
    css_layout_tree_free(&g_lq_layout);   /* also a failed run's partial tree */
    if (g_lq_doc) html_document_free(g_lq_doc);
    g_lq_doc = NULL;
    g_lq_valid = false;
    if (g_lq_box_of_slot) memset(g_lq_box_of_slot, 0, g_lq_slot_cap * sizeof(int));
}

void layout_api_invalidate(void)
{
    lq_free();
    g_lq_key = 0;
}

static double window_number(JSContextHandle ctx, const char *name, double fallback)
{
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue v = JS_GetPropertyStr(ctx, global, name);
    double d = fallback;
    if (JS_IsNumber(v)) JS_ToFloat64(ctx, &d, v);
    return d > 0.0 ? d : fallback;
}

static void lq_map_slots(void)
{
    LayoutContext *lc = &g_lq_layout;
    for (int i = 0; i < lc->tree.count; i++) {
        HtmlNode *hn = (HtmlNode *)po_array_payload(&g_lq_doc->array, lc->tree.nodes[i].dom_node_idx);
        if (!hn || hn->type != HTML_NODE_ELEMENT || !hn->store_id) continue;
        uint32_t slot = dom_slot(hn->store_id);
        if (slot >= g_lq_slot_cap) {
            uint32_t cap = g_lq_slot_cap ? g_lq_slot_cap : 4096;
            while (cap <= slot) cap *= 2;
            int *b = (int *)realloc(g_lq_box_of_slot, cap * sizeof(int));
            NodeId *n = (NodeId *)realloc(g_lq_id_of_slot, cap * sizeof(NodeId));
            if (!b || !n) { free(b ? b : g_lq_box_of_slot); free(n ? n : g_lq_id_of_slot);
                            g_lq_box_of_slot = NULL; g_lq_id_of_slot = NULL; g_lq_slot_cap = 0; return; }
            memset(b + g_lq_slot_cap, 0, (cap - g_lq_slot_cap) * sizeof(int));
            memset(n + g_lq_slot_cap, 0, (cap - g_lq_slot_cap) * sizeof(NodeId));
            g_lq_box_of_slot = b;
            g_lq_id_of_slot = n;
            g_lq_slot_cap = cap;
        }
        g_lq_box_of_slot[slot] = i + 1;
        g_lq_id_of_slot[slot] = hn->store_id;
    }
}

/* Bring the cached layout up to date with the document; false when there is
 * nothing to lay out. */
static bool lq_ensure(JSContextHandle ctx)
{
    if (g_lq_running) return g_lq_valid;   /* a query from inside layout */
    /* CYBER_LAYOUT_QUERY=0: no forced layouts (every element reports no box). */
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = cyber_getenv("CYBER_LAYOUT_QUERY");
        enabled = !(e && e[0] == '0');
    }
    if (!enabled) return false;
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue js_doc = JS_GetPropertyStr(ctx, global, "document");
    if (!JS_IsObject(js_doc)) return false;
    double vw = window_number(ctx, "innerWidth", 1024.0);
    double vh = window_number(ctx, "innerHeight", 768.0);
    if (vw != g_lq_vw || vh != g_lq_vh) { g_lq_key = 0; g_lq_vw = vw; g_lq_vh = vh; }

    bool unchanged = false;
    uint64_t key = g_lq_valid ? g_lq_key : 0;
    HtmlDocument *doc = html_document_from_js_dom_if_changed(ctx, js_doc, &key, &unchanged);
    if (unchanged && g_lq_valid) return true;
    if (!doc) return g_lq_valid;

    g_lq_running = true;
    unsigned long long t0 = platform_get_time_ms();
    lq_free();
    g_lq_doc = doc;
    g_lq_layout.js_ctx = ctx;
    g_lq_valid = css_layout_run(&g_lq_layout, doc, NULL, vw, vh);
    if (g_lq_valid) {
        g_lq_key = key;
        lq_map_slots();
    } else {
        lq_free();
        g_lq_key = 0;
    }
    g_lq_layouts++;
    g_lq_layout_ms += platform_get_time_ms() - t0;
    g_lq_running = false;
    return g_lq_valid;
}

/* Layout box index of a store element, or -1. */
static int lq_box_index(NodeId id)
{
    if (!id || dom_kind(id) != DOM_K_ELEMENT) return -1;
    uint32_t slot = dom_slot(id);
    if (slot >= g_lq_slot_cap || g_lq_id_of_slot[slot] != id) return -1;
    return g_lq_box_of_slot[slot] - 1;
}

/* A box is rendered when neither it nor an ancestor is display:none. */
static bool lq_rendered(int idx)
{
    for (int i = idx; i >= 0; i = g_lq_layout.tree.nodes[i].parent_idx)
        if (g_lq_layout.boxes[i].display == CSS_DISPLAY_NONE) return false;
    return idx >= 0;
}

/* Rendered box index for a JS element (laying out if needed), or -1. */
static int lq_query(JSContextHandle ctx, GCValue element)
{
    g_lq_queries++;
    NodeId id = dom_node_of(element);
    if (!id || dom_kind(id) != DOM_K_ELEMENT) return -1;
    if (!lq_ensure(ctx)) return -1;
    int idx = lq_box_index(id);
    return lq_rendered(idx) ? idx : -1;
}

bool layout_query_element_box(JSContextHandle ctx, GCValue element, LayoutQueryBox *out)
{
    memset(out, 0, sizeof(*out));
    int idx = lq_query(ctx, element);
    if (idx < 0) return false;
    const LayoutBox *b = &g_lq_layout.boxes[idx];
    out->x = b->x;
    out->y = b->y;
    out->width = b->width;
    out->height = b->height;
    out->border_top = b->border_top;
    out->border_right = b->border_right;
    out->border_bottom = b->border_bottom;
    out->border_left = b->border_left;
    out->inline_level = b->display == CSS_DISPLAY_INLINE;
    return true;
}

static bool is_tag(NodeId id, const char *tag)
{
    return id && dom_kind(id) == DOM_K_ELEMENT && strcasecmp(dom_tag_name(id), tag) == 0;
}

static bool is_root_element(NodeId id)
{
    NodeId p = dom_parent(id);
    return p && dom_kind(p) == DOM_K_DOCUMENT;
}

/* offsetParent (CSSOM View §7): the nearest ancestor that is positioned, or a
 * td/th/table when the element itself is static, or body; none for the root,
 * body, fixed-position or unrendered elements. */
static NodeId lq_offset_parent(NodeId id, int idx)
{
    if (idx < 0 || is_root_element(id) || is_tag(id, "body")) return DOM_NULL;
    const LayoutBox *b = &g_lq_layout.boxes[idx];
    if (b->position == CSS_POSITION_FIXED) return DOM_NULL;
    bool self_static = b->position == CSS_POSITION_STATIC;
    for (NodeId a = dom_parent(id); a && dom_kind(a) == DOM_K_ELEMENT; a = dom_parent(a)) {
        if (is_tag(a, "body")) return a;
        int ai = lq_box_index(a);
        if (ai >= 0 && g_lq_layout.boxes[ai].position != CSS_POSITION_STATIC) return a;
        if (self_static && (is_tag(a, "td") || is_tag(a, "th") || is_tag(a, "table"))) return a;
    }
    return DOM_NULL;
}

static GCValue lq_int(JSContextHandle ctx, double v)
{
    if (!isfinite(v)) v = 0.0;
    return JS_NewInt32(ctx, (int32_t)lround(v));
}

enum { M_OFFSET_W, M_OFFSET_H, M_OFFSET_T, M_OFFSET_L,
       M_CLIENT_W, M_CLIENT_H, M_CLIENT_T, M_CLIENT_L,
       M_SCROLL_W, M_SCROLL_H };

/* Right / bottom extent of the rendered subtree of box idx (page coords). */
static void lq_extent(int idx, double *right, double *bottom)
{
    const LayoutNodeRef *nodes = g_lq_layout.tree.nodes;
    for (int c = nodes[idx].first_child_idx; c >= 0; c = nodes[c].next_sibling_idx) {
        const LayoutBox *cb = &g_lq_layout.boxes[c];
        if (cb->display == CSS_DISPLAY_NONE) continue;
        if (cb->x + cb->width > *right) *right = cb->x + cb->width;
        if (cb->y + cb->height > *bottom) *bottom = cb->y + cb->height;
        lq_extent(c, right, bottom);
    }
}

static GCValue lq_metric(JSContextHandle ctx, GCValue this_val, int which)
{
    NodeId id = dom_node_of(this_val);
    int idx = lq_query(ctx, this_val);
    bool root = id && is_root_element(id);
    /* The root element's client box is the viewport. */
    if (root && (which == M_CLIENT_W || which == M_CLIENT_H)) {
        if (!lq_ensure(ctx)) return JS_NewInt32(ctx, 0);
        return lq_int(ctx, which == M_CLIENT_W ? g_lq_vw : g_lq_vh);
    }
    if (idx < 0) return JS_NewInt32(ctx, 0);
    const LayoutBox *b = &g_lq_layout.boxes[idx];
    bool inl = b->display == CSS_DISPLAY_INLINE;
    switch (which) {
    case M_OFFSET_W: return lq_int(ctx, b->width);
    case M_OFFSET_H: return lq_int(ctx, b->height);
    case M_OFFSET_T:
    case M_OFFSET_L: {
        NodeId op = lq_offset_parent(id, idx);
        double v = which == M_OFFSET_T ? b->y : b->x;
        int pi = op ? lq_box_index(op) : -1;
        /* Relative to the offset parent's padding edge; to the initial
         * containing block when it is body or there is none. */
        if (pi >= 0 && !is_tag(op, "body")) {
            const LayoutBox *pb = &g_lq_layout.boxes[pi];
            v -= which == M_OFFSET_T ? pb->y + pb->border_top : pb->x + pb->border_left;
        }
        return lq_int(ctx, v);
    }
    case M_CLIENT_W: return lq_int(ctx, inl ? 0 : b->width - b->border_left - b->border_right);
    case M_CLIENT_H: return lq_int(ctx, inl ? 0 : b->height - b->border_top - b->border_bottom);
    case M_CLIENT_T: return lq_int(ctx, inl ? 0 : b->border_top);
    case M_CLIENT_L: return lq_int(ctx, inl ? 0 : b->border_left);
    case M_SCROLL_W:
    case M_SCROLL_H: {
        if (inl) return JS_NewInt32(ctx, 0);
        double left = b->x + b->border_left, top = b->y + b->border_top;
        double cw = root ? g_lq_vw : b->width - b->border_left - b->border_right;
        double ch = root ? g_lq_vh : b->height - b->border_top - b->border_bottom;
        double right = left + cw, bottom = top + ch;
        lq_extent(idx, &right, &bottom);
        return lq_int(ctx, which == M_SCROLL_W ? right - left : bottom - top);
    }
    }
    return JS_NewInt32(ctx, 0);
}

#define LQ_GETTER(fn, which) \
    static GCValue fn(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) \
    { (void)argc; (void)argv; return lq_metric(ctx, this_val, which); }
LQ_GETTER(js_offset_width, M_OFFSET_W)
LQ_GETTER(js_offset_height, M_OFFSET_H)
LQ_GETTER(js_offset_top, M_OFFSET_T)
LQ_GETTER(js_offset_left, M_OFFSET_L)
LQ_GETTER(js_client_width, M_CLIENT_W)
LQ_GETTER(js_client_height, M_CLIENT_H)
LQ_GETTER(js_client_top, M_CLIENT_T)
LQ_GETTER(js_client_left, M_CLIENT_L)
LQ_GETTER(js_scroll_width, M_SCROLL_W)
LQ_GETTER(js_scroll_height, M_SCROLL_H)

static GCValue js_offset_parent(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)argc; (void)argv;
    NodeId id = dom_node_of(this_val);
    NodeId op = lq_offset_parent(id, lq_query(ctx, this_val));
    return op ? dom_js(ctx, op) : JS_NULL;
}

static GCValue js_scroll_offset(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, 0);
}

static GCValue js_scroll_offset_set(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static void define_accessor(JSContextHandle ctx, GCValue proto, const char *name,
                            JSCFunction *get, JSCFunction *set)
{
    char gname[64], sname[64];
    snprintf(gname, sizeof(gname), "get %s", name);
    snprintf(sname, sizeof(sname), "set %s", name);
    JSAtom atom = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, proto, atom, JS_NewCFunction(ctx, get, gname, 0),
                            set ? JS_NewCFunction(ctx, set, sname, 1) : JS_UNDEFINED,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, atom);
}

void layout_api_install(JSContextHandle ctx, GCValue element_proto)
{
    if (!JS_IsObject(element_proto)) return;
    define_accessor(ctx, element_proto, "offsetWidth", js_offset_width, NULL);
    define_accessor(ctx, element_proto, "offsetHeight", js_offset_height, NULL);
    define_accessor(ctx, element_proto, "offsetTop", js_offset_top, NULL);
    define_accessor(ctx, element_proto, "offsetLeft", js_offset_left, NULL);
    define_accessor(ctx, element_proto, "offsetParent", js_offset_parent, NULL);
    define_accessor(ctx, element_proto, "clientWidth", js_client_width, NULL);
    define_accessor(ctx, element_proto, "clientHeight", js_client_height, NULL);
    define_accessor(ctx, element_proto, "clientTop", js_client_top, NULL);
    define_accessor(ctx, element_proto, "clientLeft", js_client_left, NULL);
    define_accessor(ctx, element_proto, "scrollWidth", js_scroll_width, NULL);
    define_accessor(ctx, element_proto, "scrollHeight", js_scroll_height, NULL);
    define_accessor(ctx, element_proto, "scrollTop", js_scroll_offset, js_scroll_offset_set);
    define_accessor(ctx, element_proto, "scrollLeft", js_scroll_offset, js_scroll_offset_set);
}

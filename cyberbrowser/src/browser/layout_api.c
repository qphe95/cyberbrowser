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
 * Like a browser, the layout is kept across changes that cannot alter the
 * boxes a script asks about, and laid out again only for a query whose
 * answer a change may have altered.  Each change the mirror recorded since
 * it (html_mirror_changes) either alters nothing (an attribute no selector
 * names and layout does not read, a paint-only inline style, anything in a
 * display:none subtree that no :has() rule above could see), only moves a
 * subtree (an inline offset or transform change), or has an anchor: the box
 * whose subtree, ancestors' sizes and followers it may alter, bounded by the
 * out-of-flow box or size boundary (a box its content cannot resize) it is
 * inside, if any.  A query reads the cached boxes when its box lies outside
 * every bound, or before an anchor, or holds one, and nothing on its way up
 * places or sizes it by what the change can alter (lq_anchor_independent);
 * otherwise it lays out again, as does a change that can reach anywhere (a
 * :has() rule that may restyle beyond its subtree aside), a sheet edit,
 * adopted sheets or an image load (dom_request_relayout).
 * CYBER_LQ_SKIP=0 turns this off; CYBER_LQ_ANCHOR=0 lays out again at any
 * change past a subtree; CYBER_LQ_SKIP_VERIFY=1 lays out anyway and
 * compares (every box the changes cannot reach, and each answer).
 *
 * Elements map to boxes through the snapshot's store ids, so an element the
 * snapshot does not contain (detached, or in a tree that is not rendered)
 * has no box and reports zeros, as display:none elements do in browsers.
 * Scrolling is not modelled: scrollTop/Left read 0 and ignore writes.
 */
#include "cyber_profile.h"
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
#include "det_clock.h"
#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "gc_platform.h"
#include "browser_api_impl.h"

/* The cached forced layout. */
static LayoutContext g_lq_layout;
static HtmlDocument *g_lq_doc;
static bool g_lq_valid;
static uint64_t g_lq_key;
static double g_lq_vw, g_lq_vh;
static bool g_lq_running;
/* The next layout styles display:none subtrees too (a computed-style query
 * reached an element inside one); layouts that did. */
static bool g_lq_style_all;
static uint64_t g_lq_style_all_layouts;
/* Store slot -> layout box index (+1; 0 = no box), and the NodeId that
 * owned the slot when the layout ran (a reused slot is a different node). */
static int *g_lq_box_of_slot;
static NodeId *g_lq_id_of_slot;
static uint32_t g_lq_slot_cap;

static uint64_t g_lq_queries, g_lq_layouts;
static uint64_t g_lq_layout_us, g_lq_layout_cpu_us;   /* wall and this thread's CPU */
/* Mirror change-log position and relayout requests at the cached layout;
 * layouts kept because nothing since could change a box. */
static uint64_t g_lq_change_pos, g_lq_relayouts, g_lq_kept, g_lq_region_layouts;
static uint64_t g_lq_change_pos0;   /* the change log position of the cached layout (CYBER_LQ_LOG) */

/* Changes past a subtree, as their anchors (layout indices; see the top of
 * the file and lq_anchor_independent). */
#define LQ_ANCHOR_MAX 64
static int g_lq_anchor[LQ_ANCHOR_MAX];
/* Per anchor: the region it stays in (-1: none), and whether it may restyle
 * through :has(). */
static int g_lq_anchor_bound[LQ_ANCHOR_MAX];
static bool g_lq_anchor_has[LQ_ANCHOR_MAX];
/* Per anchor: its own style holds (the change is inside it), and, for an
 * attribute change, its later siblings may restyle. */
static bool g_lq_anchor_kept[LQ_ANCHOR_MAX];
static bool g_lq_anchor_sib[LQ_ANCHOR_MAX];
/* Per anchor: its own style was restyled on demand (lq_anchor_style_kept);
 * queries that found it as before. */
static bool g_lq_anchor_tried[LQ_ANCHOR_MAX];
static uint64_t g_lq_ondemand_kept;
static int g_lq_nanchor;
/* Per layout index: preorder position and one past its subtree's last. */
static int *g_lq_pos, *g_lq_end;
static int g_lq_pos_cap;
static bool g_lq_pos_ok;           /* g_lq_pos / g_lq_end describe the cached layout */
/* Per layout index: what the anchors need of the node (LQ_KIND_*). */
enum { LQ_KIND_TEXT = 1, LQ_KIND_ROW = 2, LQ_KIND_CELL = 4 };
static unsigned char *g_lq_kind;
/* Per layout index: its store node, and the last independence answer per
 * ask (bit 2*ask: known, 2*ask+1: independent) with the anchor set it was
 * for. */
static NodeId *g_lq_ids;
static unsigned char *g_lq_ind;
static uint32_t *g_lq_ind_ver;
static uint32_t g_lq_anchor_ver = 1;
static uint64_t g_lq_anchor_answers;   /* queries answered past anchors */

/* Cheap change detection.  Serializing the document to see whether it changed
 * costs tens of milliseconds on a large page, and scripts query layout
 * thousands of times between mutations.  The store's version counters, the
 * write epoch of watched (style) objects and the layout-request counter
 * (CSSOM sheet edits) cover nearly every change; what they miss (properties
 * that live only on JS objects) is caught by a full check at least every
 * LQ_FULL_CHECK_MS. */
enum { LQ_CHEAP_N = 5 };
#define LQ_FULL_CHECK_MS 1000
static uint64_t g_lq_cheap[LQ_CHEAP_N];
static bool g_lq_cheap_ok;
static unsigned long long g_lq_full_at;
static uint64_t g_lq_cheap_hits;

static void lq_cheap_key(uint64_t k[LQ_CHEAP_N])
{
    k[0] = g_dom.structure_version;
    k[1] = g_dom.attr_version;
    k[2] = g_dom.text_version;
    k[3] = JS_watched_write_epoch;
    k[4] = g_dom_layout_requests;
}

void layout_api_stats(uint64_t *queries, uint64_t *layouts, uint64_t *layout_ms)
{
    if (queries) *queries = g_lq_queries;
    if (layouts) *layouts = g_lq_layouts;
    if (layout_ms) *layout_ms = g_lq_layout_us / 1000;
}

uint64_t layout_api_kept(void)
{
    return g_lq_kept;
}

uint64_t layout_api_region_layouts(void)
{
    return g_lq_region_layouts;
}

static void lq_free_keeping(bool keep_tree)
{
    /* (css_layout_run keeps the tree when it can) */
    if (!keep_tree) css_layout_tree_free(&g_lq_layout);   /* also a failed run's partial tree */
    if (g_lq_doc) html_document_free(g_lq_doc);
    g_lq_doc = NULL;
    g_lq_valid = false;
    if (g_lq_box_of_slot) memset(g_lq_box_of_slot, 0, g_lq_slot_cap * sizeof(int));
}

static void lq_free(void)
{
    lq_free_keeping(false);
}

void layout_api_invalidate(void)
{
    lq_free();
    g_lq_key = 0;
    g_lq_cheap_ok = false;
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
    g_lq_pos_ok = false;
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
    /* Document order, for the anchors. */
    int n = lc->tree.count;
    if (g_lq_pos_cap < n) {
        int *p = (int *)realloc(g_lq_pos, (size_t)n * sizeof(int));
        int *e = p ? (int *)realloc(g_lq_end, (size_t)n * sizeof(int)) : NULL;
        unsigned char *kd = e ? (unsigned char *)realloc(g_lq_kind, (size_t)n) : NULL;
        NodeId *ids = kd ? (NodeId *)realloc(g_lq_ids, (size_t)n * sizeof(NodeId)) : NULL;
        unsigned char *ind = ids ? (unsigned char *)realloc(g_lq_ind, (size_t)n) : NULL;
        uint32_t *iv = ind ? (uint32_t *)realloc(g_lq_ind_ver, (size_t)n * sizeof(uint32_t)) : NULL;
        if (p) g_lq_pos = p;
        if (e) g_lq_end = e;
        if (kd) g_lq_kind = kd;
        if (ids) g_lq_ids = ids;
        if (ind) g_lq_ind = ind;
        if (iv) g_lq_ind_ver = iv;
        if (!iv) { g_lq_pos_cap = 0; return; }
        g_lq_pos_cap = n;
    }
    for (int k = 0; k < n; k++) g_lq_pos[lc->tree.preorder[k]] = k;
    for (int i = 0; i < n; i++) {
        HtmlNode *hn = (HtmlNode *)po_array_payload(&g_lq_doc->array, lc->tree.nodes[i].dom_node_idx);
        g_lq_kind[i] = !hn ? 0 : hn->type == HTML_NODE_TEXT ? LQ_KIND_TEXT :
                       hn->type == HTML_NODE_ELEMENT && hn->tag_name && strcasecmp(hn->tag_name, "tr") == 0 ? LQ_KIND_ROW : 0;
        if (hn && hn->type == HTML_NODE_ELEMENT && hn->tag_name &&
            (strcasecmp(hn->tag_name, "td") == 0 || strcasecmp(hn->tag_name, "th") == 0))
            g_lq_kind[i] |= LQ_KIND_CELL;
        g_lq_ids[i] = hn ? hn->store_id : 0;
        g_lq_ind_ver[i] = 0;
    }
    for (int k = n - 1; k >= 0; k--) {
        int i = lc->tree.preorder[k];
        int last = k;
        for (int c = lc->tree.nodes[i].first_child_idx; c >= 0; c = lc->tree.nodes[c].next_sibling_idx)
            if (g_lq_end[c] - 1 > last) last = g_lq_end[c] - 1;
        g_lq_end[i] = last + 1;
    }
    g_lq_pos_ok = true;
}

/* The snapshot was just checked (or rebuilt) against the live document. */
static void lq_note_full_check(unsigned long long now)
{
    lq_cheap_key(g_lq_cheap);
    g_lq_cheap_ok = true;
    g_lq_full_at = now;
}

/* Bring the cached layout up to date with the document; false when there is
 * nothing to lay out. */
static NodeId g_lq_who;   /* element whose query is being answered (CYBER_LQ_LOG) */
/* CYBER_LQ_LOG=1: why each forced layout runs; 2: also each changed region. */
static int g_lq_log = -1;

/* Where store element `id` stands in the cached layout: 2 inside a
 * display:none subtree (an ancestor's box is display:none), 1 its own box is
 * display:none, 0 rendered, -1 not known (no box, nor one above it that is
 * hidden). */
static int lq_box_index(NodeId id);
static int lq_hidden(NodeId id)
{
    const LayoutNodeRef *nodes = g_lq_layout.tree.nodes;
    int idx = lq_box_index(id);
    if (idx >= 0) {
        for (int a = nodes[idx].parent_idx; a >= 0; a = nodes[a].parent_idx)
            if (g_lq_layout.boxes[a].s->display == CSS_DISPLAY_NONE) return 2;
        return g_lq_layout.boxes[idx].s->display == CSS_DISPLAY_NONE ? 1 : 0;
    }
    /* Not in the layout (added since): what holds it. */
    for (NodeId a = dom_parent(id); a && dom_kind(a) == DOM_K_ELEMENT; a = dom_parent(a)) {
        int ai = lq_box_index(a);
        if (ai < 0) continue;
        for (int b = ai; b >= 0; b = nodes[b].parent_idx)
            if (g_lq_layout.boxes[b].s->display == CSS_DISPLAY_NONE) return 2;
        return -1;
    }
    return -1;
}

static const HtmlAttribute *lq_attr_find(const HtmlAttribute *list, const char *name)
{
    for (const HtmlAttribute *a = list; a; a = a->next)
        if (a->name == name || strcmp(a->name, name) == 0) return a;
    return NULL;
}

/* Parts of the cached layout that changes since it may have altered, as
 * layout indices of subtree roots.  A query about an element in one (or
 * about the scroll extent of an ancestor of one) lays out again; any other
 * reads the cached boxes, which a new layout would compute the same. */
#define LQ_DIRTY_MAX 64
static int g_lq_dirty[LQ_DIRTY_MAX];
static bool g_lq_dirty_move[LQ_DIRTY_MAX];   /* only moved: sizes inside hold */
static int g_lq_ndirty;
/* What the query being answered reads of its element's box. */
typedef enum {
    LQ_ASK_POS,       /* position (offsetTop/Left, getBoundingClientRect): the default */
    LQ_ASK_SIZE,      /* size and borders (offsetWidth/Height, client*) */
    LQ_ASK_EXTENT,    /* scroll extent: its descendants' boxes too */
    LQ_ASK_STYLE,     /* computed style, display, position (font size, offsetParent) */
} LqAsk;
static LqAsk g_lq_ask;
/* Per layout index: 2 inside a changed region, 1 inside one that only
 * moved (built when the regions change). */
static unsigned char *g_lq_mark;
static int g_lq_mark_cap;
static bool g_lq_mark_stale;

static bool lq_is_ancestor_or_self(int a, int d)
{
    for (int x = d; x >= 0; x = g_lq_layout.tree.nodes[x].parent_idx)
        if (x == a) return true;
    return false;
}

/* A region at `root`: every box inside may differ, or (move) only their
 * positions do.  False when the region list is full. */
static bool lq_dirty_add(int root, bool move)
{
    for (int i = 0; i < g_lq_ndirty; i++)
        if ((!g_lq_dirty_move[i] || move) && lq_is_ancestor_or_self(g_lq_dirty[i], root)) return true;
    int w = 0;
    for (int i = 0; i < g_lq_ndirty; i++) {
        if ((g_lq_dirty_move[i] || !move) && lq_is_ancestor_or_self(root, g_lq_dirty[i])) continue;
        g_lq_dirty_move[w] = g_lq_dirty_move[i];
        g_lq_dirty[w++] = g_lq_dirty[i];
    }
    g_lq_ndirty = w;
    if (g_lq_ndirty == LQ_DIRTY_MAX) return false;
    g_lq_dirty_move[g_lq_ndirty] = move;
    g_lq_dirty[g_lq_ndirty++] = root;
    g_lq_mark_stale = true;
    return true;
}

/* Mark every box inside a region (stackless pre-order walk of each). */
static bool lq_marks(void)
{
    int n = g_lq_layout.tree.count;
    if (!g_lq_mark_stale && g_lq_mark_cap >= n) return true;
    if (g_lq_mark_cap < n) {
        unsigned char *m = (unsigned char *)realloc(g_lq_mark, (size_t)n);
        if (!m) return false;
        g_lq_mark = m;
        g_lq_mark_cap = n;
    }
    memset(g_lq_mark, 0, (size_t)n);
    const LayoutNodeRef *nodes = g_lq_layout.tree.nodes;
    for (int i = 0; i < g_lq_ndirty; i++) {
        int root = g_lq_dirty[i];
        unsigned char v = g_lq_dirty_move[i] ? 1 : 2;
        int x = root;
        while (x >= 0) {
            if (g_lq_mark[x] < v) g_lq_mark[x] = v;
            if (nodes[x].first_child_idx >= 0) { x = nodes[x].first_child_idx; continue; }
            while (x != root && nodes[x].next_sibling_idx < 0) x = nodes[x].parent_idx;
            x = x == root ? -1 : nodes[x].next_sibling_idx;
        }
    }
    g_lq_mark_stale = false;
    return true;
}

/* Element `q` (cached box `qi`, -1 none) inside a region, by where it
 * was laid out or where it is now (it may have moved into one): 2 a
 * changed one, 1 one that only moved, 0 none. */
static int lq_in_region(NodeId q, int qi)
{
    if (!g_lq_ndirty) return 0;
    if (!lq_marks()) return 2;
    int m = qi >= 0 ? g_lq_mark[qi] : 0;
    for (NodeId a = dom_parent(q); a && dom_kind(a) == DOM_K_ELEMENT && m < 2; a = dom_parent(a)) {
        int ai = lq_box_index(a);
        if (ai >= 0 && g_lq_mark[ai] > m) m = g_lq_mark[ai];
    }
    return m;
}

/* Is box `idx`'s width (horizontal) or height fixed by its own style, not
 * by its insets? */
static bool lq_size_definite(const LayoutBox *b, bool horizontal)
{
    return horizontal ? (b->s->width_set || b->s->width_percent > 0.0 || b->s->width_keyword)
                      : (b->s->height_set || b->s->height_percent > 0.0);
}

static bool lq_out_of_flow(int idx)
{
    const LayoutBox *b = &g_lq_layout.boxes[idx];
    return b->s->position == CSS_POSITION_ABSOLUTE || b->s->position == CSS_POSITION_FIXED;
}

/* The nearest out-of-flow box at (self) or above `idx`, or -1.  Nothing
 * outside such a box depends on its size or on what it contains: flow,
 * intrinsic sizes and shrink-to-fit all skip it. */
static int lq_out_of_flow_root(int idx, bool self)
{
    const LayoutNodeRef *nodes = g_lq_layout.tree.nodes;
    for (int a = self ? idx : nodes[idx].parent_idx; a >= 0; a = nodes[a].parent_idx)
        if (lq_out_of_flow(a)) return a;
    return -1;
}

/* The box of element `id`, or (*inside set) the box of the nearest store
 * ancestor that has one; -1 for neither. */
static int lq_box_or_holder(NodeId id, bool *inside)
{
    *inside = false;
    int idx = lq_box_index(id);
    if (idx >= 0) return idx;
    *inside = true;
    for (NodeId a = dom_parent(id); a && dom_kind(a) == DOM_K_ELEMENT; a = dom_parent(a)) {
        int ai = lq_box_index(a);
        if (ai >= 0) return ai;
    }
    return -1;
}

/* What attribute changes `was` -> `now` reach: *other when an attribute
 * other than style matters to some selector or to layout, *self when one
 * (or the inline style) can change how the element itself is laid out; the
 * furthest reach of the inline style change (css_layout_inline_style_reach). */
static void lq_attrs_reach(const HtmlAttribute *was, const HtmlAttribute *now, bool *other, bool *self,
                           int *style_reach, unsigned *sides, char *why, size_t cap, const char *tag)
{
    *other = false;
    *self = false;
    *style_reach = 0;
    *sides = 0;
    const HtmlAttribute *c0 = lq_attr_find(was, "class"), *c1 = lq_attr_find(now, "class");
    const HtmlAttribute *i0 = lq_attr_find(was, "id"), *i1 = lq_attr_find(now, "id");
    LayoutAttrOwner owner = { { c0 ? c0->value : NULL, c1 ? c1->value : NULL },
                              { i0 ? i0->value : NULL, i1 ? i1->value : NULL } };
    for (int pass = 0; pass < 2; pass++) {
        for (const HtmlAttribute *a = pass ? was : now; a; a = a->next) {
            const HtmlAttribute *o = lq_attr_find(pass ? now : was, a->name);
            if (pass && o) continue;                       /* seen in the first pass */
            const char *ov = pass ? a->value : (o ? o->value : NULL);
            const char *nv = pass ? NULL : a->value;
            if (ov && nv && strcmp(ov, nv) == 0) continue;
            if (strcmp(a->name, "style") == 0) {
                unsigned s = 0;
                int r = css_layout_inline_style_reach(&g_lq_layout, &owner, ov, nv, &s);
                *sides |= s;
                if (r > 0) *self = true;
                if (r > *style_reach) {
                    *style_reach = r;
                    if (!*other) snprintf(why, cap, "<%s> style [%.50s -> %.50s]", tag, ov ? ov : "", nv ? nv : "");
                }
            } else if (css_layout_attr_change_matters(&g_lq_layout, &owner, a->name, ov, nv)) {
                if (!*other) snprintf(why, cap, "<%s> %s [%.40s -> %.40s]", tag, a->name, ov ? ov : "", nv ? nv : "");
                *other = true;
                if (!*self && css_layout_attr_change_restyles_self(&g_lq_layout, &owner, a->name, ov, nv)) {
                    *self = true;
                    if (g_lq_log > 2)
                        fprintf(stderr, "[LQ] restyles itself: <%s> %s [%s] -> [%s]\n", tag, a->name, ov ? ov : "",
                                nv ? nv : "");
                }
            }
        }
    }
}

static bool lq_has_reach(int i, bool self);

/* What change `c` can alter in the cached layout: -2 nothing, -1 more than a
 * subtree, else the layout index of the subtree it stays in (*move: where
 * boxes only move).  *anchor is the box whose subtree, ancestors and
 * followers it can alter (lq_anchor_independent; -1 for anything), *from
 * the lowest box whose own style it cannot change (where a size boundary
 * around it may start), *has whether it may restyle more through :has(),
 * and *kept whether the anchor's own style holds. */
static int lq_change_reach(const HtmlMirrorChange *c, char *why, size_t cap, bool *move, int *anchor, int *from,
                           bool *has_out, bool *kept)
{
    *move = false;
    *anchor = -1;
    *from = -1;
    *has_out = false;
    *kept = false;
    const char *tag = c->id && dom_valid(c->id) ? dom_tag_name(c->id) : "?";
    if (c->kind == HTML_MIRROR_SHEETS) { snprintf(why, cap, "stylesheets (<%s>)", tag); return -1; }
    int st = lq_hidden(c->id);
    bool inside;
    int idx = lq_box_or_holder(c->id, &inside);
    /* What :has() can restyle: from an attribute change, the ancestors (and
     * their earlier siblings); from changed children (or a new element's
     * attributes), the box holding them too. */
    bool has = idx < 0 ? css_layout_has_any(&g_lq_layout)
                       : lq_has_reach(idx, c->kind == HTML_MIRROR_CHILDREN || inside);
    *has_out = has;
    /* Inside a display:none subtree (or under its own display:none box,
     * unless :empty flips with it), only :has() could see a change. */
    bool hidden = st == 2 || (st == 1 && c->kind == HTML_MIRROR_CHILDREN &&
                              (!c->empty_changed || !css_layout_uses_empty(&g_lq_layout)));
    if (hidden && !has) return -2;
    if (c->kind == HTML_MIRROR_ATTRS) {
        bool other, self;
        int reach;
        unsigned sides;
        lq_attrs_reach(c->old_attrs, c->new_attrs, &other, &self, &reach, &sides, why, cap, tag);
        if (!other && reach == 0) return -2;
        if (idx < 0) return -1;
        /* An attribute can restyle earlier siblings through :nth-last-child(of S). */
        *anchor = !inside && css_layout_uses_last_of(&g_lq_layout) ? g_lq_layout.tree.nodes[idx].parent_idx : idx;
        *kept = inside || !self || *anchor != idx;
        *from = *anchor == idx && !*kept ? g_lq_layout.tree.nodes[idx].parent_idx : *anchor;
        /* :has() may restyle what holds it or comes before it: no region. */
        if (has) return -1;
        /* Its inline style alone: offsets move just its subtree, and an
         * out-of-flow box keeps its size changes to itself.  Only a box
         * whose size the changed insets cannot set (an out-of-flow one with
         * auto width between left and right) keeps its size. */
        if (!inside && !other && (reach == 1 || (reach == 2 && lq_out_of_flow(idx)))) {
            const LayoutBox *b = &g_lq_layout.boxes[idx];
            unsigned ps = b->s->positioned_sides | sides;
            bool oof = lq_out_of_flow(idx);
            *move = reach == 1 &&
                    !(oof && (sides & (LAYOUT_SIDE_LEFT | LAYOUT_SIDE_RIGHT)) &&
                      (ps & LAYOUT_SIDE_LEFT) && (ps & LAYOUT_SIDE_RIGHT) && !lq_size_definite(b, true)) &&
                    !(oof && (sides & (LAYOUT_SIDE_TOP | LAYOUT_SIDE_BOTTOM)) &&
                      (ps & LAYOUT_SIDE_TOP) && (ps & LAYOUT_SIDE_BOTTOM) && !lq_size_definite(b, false));
            *anchor = idx;
            return idx;
        }
        /* An element strictly inside an out-of-flow box: its style, its
         * later siblings' and its descendants' all stay inside that box. */
        return lq_out_of_flow_root(idx, inside);
    }
    snprintf(why, cap, "<%s> children%s", tag, st == 2 || st == 1 ? " (hidden, :has)" : "");
    if (idx < 0) return -1;
    *anchor = idx;
    *kept = !(c->empty_changed && !inside && css_layout_uses_empty(&g_lq_layout));
    *from = *kept ? idx : g_lq_layout.tree.nodes[idx].parent_idx;
    if (has) return -1;
    /* Children of an out-of-flow box, or of anything inside one, reflow it
     * alone (unless :empty changes the box itself). */
    int a = lq_out_of_flow_root(idx, true);
    if (a == idx && !inside && c->empty_changed && css_layout_uses_empty(&g_lq_layout)) a = -1;
    return a;
}

/* ---- Anchors ----
 *
 * A change at anchor c can alter c's subtree, its ancestors' sizes and the
 * boxes after it, and nothing else, as layout places boxes in document order:
 * a block container stacks its children by the ones before, and its width
 * comes from its container.  A box wholly before c keeps its geometry when
 * nothing on its way up breaks that: the lowest box holding both (L), and
 * every box above, must be as wide and placed where they were (their width
 * not from their content, their position not from their own size or from
 * what follows), and nothing between the query and L may read the height of
 * a box that grows.  The rules follow this layout engine: flex rows place an
 * item by its siblings' main sizes, which come from their content unless
 * their basis and minimum are set; table rows size cells by their content;
 * column flex containers distribute their height. */

static bool lq_anchor_is_ancestor_or_self(int a, int d)
{
    return g_lq_pos[a] <= g_lq_pos[d] && g_lq_pos[d] < g_lq_end[a];
}

/* Note a change at anchor `a` that stays inside `bound`'s subtree (-1:
 * anywhere), with its flags (g_lq_anchor_has, _kept, _sib); false when the
 * list is full.  An anchor below another with the same bound adds nothing,
 * and two at one box keep the worse of their flags. */
static bool lq_anchor_add(int a, int bound, bool has, bool kept, bool sib)
{
    for (int i = 0; i < g_lq_nanchor; i++) {
        if (g_lq_anchor_bound[i] != bound) continue;
        if (g_lq_anchor[i] == a) {
            bool h = g_lq_anchor_has[i] || has, k = g_lq_anchor_kept[i] && kept, s = g_lq_anchor_sib[i] || sib;
            if (h != g_lq_anchor_has[i] || k != g_lq_anchor_kept[i] || s != g_lq_anchor_sib[i]) {
                g_lq_anchor_has[i] = h;
                g_lq_anchor_kept[i] = k;
                g_lq_anchor_sib[i] = s;
                g_lq_anchor_tried[i] = false;
                g_lq_anchor_ver++;
            }
            return true;
        }
        if (g_lq_anchor_has[i] >= has && lq_anchor_is_ancestor_or_self(g_lq_anchor[i], a)) return true;
    }
    int w = 0;
    for (int i = 0; i < g_lq_nanchor; i++) {
        if (g_lq_anchor_bound[i] == bound && has >= g_lq_anchor_has[i] &&
            lq_anchor_is_ancestor_or_self(a, g_lq_anchor[i])) continue;
        g_lq_anchor[w] = g_lq_anchor[i];
        g_lq_anchor_bound[w] = g_lq_anchor_bound[i];
        g_lq_anchor_has[w] = g_lq_anchor_has[i];
        g_lq_anchor_kept[w] = g_lq_anchor_kept[i];
        g_lq_anchor_tried[w] = g_lq_anchor_tried[i];
        g_lq_anchor_sib[w++] = g_lq_anchor_sib[i];
    }
    g_lq_nanchor = w;
    if (g_lq_nanchor == LQ_ANCHOR_MAX) return false;
    g_lq_anchor[g_lq_nanchor] = a;
    g_lq_anchor_bound[g_lq_nanchor] = bound;
    g_lq_anchor_has[g_lq_nanchor] = has;
    g_lq_anchor_kept[g_lq_nanchor] = kept;
    g_lq_anchor_tried[g_lq_nanchor] = false;
    g_lq_anchor_sib[g_lq_nanchor++] = sib;
    g_lq_anchor_ver++;
    return true;
}

static const LayoutBox *lq_box(int i) { return &g_lq_layout.boxes[i]; }
static int lq_parent(int i) { return g_lq_layout.tree.nodes[i].parent_idx; }

static bool lq_oof(const LayoutBox *b)
{
    return b->s->position == CSS_POSITION_ABSOLUTE || b->s->position == CSS_POSITION_FIXED;
}

/* Why the last independence test said no (CYBER_LQ_LOG=2). */
static const char *g_lq_no;
#define LQ_NO(why) do { g_lq_no = (why); return false; } while (0)

/* Does box b's geometry read the height of its containing block (out of
 * flow) or of its parent (in flow)? */
static bool lq_reads_cb_height(const LayoutBox *b)
{
    if (lq_oof(b) && (b->s->positioned_sides & LAYOUT_SIDE_BOTTOM)) return true;
    if (b->s->position != CSS_POSITION_STATIC && (b->s->inset_pct_bits & (1 | 4))) return true;   /* top, bottom % */
    return b->s->height_percent > 0.0 || (b->s->minmax_pct_bits & (4 | 8));               /* height, min/max-height % */
}

/* Block-level in block flow (not an atomic inline). */
static bool lq_block_level(const LayoutBox *b)
{
    return (b->s->display == CSS_DISPLAY_BLOCK || b->s->display == CSS_DISPLAY_FLEX || b->s->display == CSS_DISPLAY_GRID ||
            b->s->display == CSS_DISPLAY_LIST_ITEM || b->s->display == CSS_DISPLAY_OTHER) && !b->s->inline_level;
}

/* A box whose height its content cannot change. */
static bool lq_height_fixed(const LayoutBox *b)
{
    return b->s->height_set && b->s->height_percent <= 0.0;
}

/* The box an out-of-flow box b (layout index i) is positioned against, or
 * -1 for the viewport. */
static int lq_containing_block(int i)
{
    const LayoutBox *b = lq_box(i);
    if (b->s->position == CSS_POSITION_FIXED) return -1;
    int p = lq_parent(i);
    if (b->s->position == CSS_POSITION_ABSOLUTE)
        while (p >= 0 && lq_box(p)->s->position == CSS_POSITION_STATIC) p = lq_parent(p);
    return p;
}

/* Does box i's height stay what it was whatever its content: set, or a
 * percentage of (or, out of flow between top and bottom, the whole of) a
 * height that stays?  Heights outside `bound` (-1: none) do not change. */
static bool lq_height_free(int i, int bound)
{
    for (;;) {
        const LayoutBox *b = lq_box(i);
        if (lq_height_fixed(b) && !(b->s->minmax_pct_bits & (4 | 8))) return true;
        bool stretched = lq_oof(b) && !b->s->height_set && (b->s->positioned_sides & LAYOUT_SIDE_TOP) &&
                         (b->s->positioned_sides & LAYOUT_SIDE_BOTTOM);
        if (!(b->s->height_percent > 0.0 || stretched)) return false;
        int ref = lq_oof(b) ? lq_containing_block(i) : lq_parent(i);
        if (ref < 0) return true;                           /* the viewport */
        if (bound >= 0 && !lq_anchor_is_ancestor_or_self(bound, ref)) return true;
        i = ref;
    }
}

/* A box that lays its children out as a block (stacked by the ones before). */
static bool lq_block_container(int i)
{
    const LayoutBox *b = lq_box(i);
    return b->s->display != CSS_DISPLAY_INLINE && b->s->display != CSS_DISPLAY_FLEX &&
           b->s->display != CSS_DISPLAY_NONE && !(g_lq_kind[i] & LQ_KIND_ROW);
}

/* A flex row item whose main size its content cannot change: a set basis
 * (or width) and a minimum that is not its min-content width. */
static bool lq_flex_main_free(int i)
{
    const LayoutBox *b = lq_box(i);
    if (g_lq_kind[i] & LQ_KIND_TEXT) return false;
    bool w_def = (b->s->width_set || b->s->width_percent > 0.0) && !b->s->width_keyword;
    if (!(b->s->flex_basis_percent > 0.0 || b->s->flex_basis >= 0.0 || w_def)) return false;
    return b->min_width > 0.0 || (b->s->min_size_set & 1) || b->s->overflow != 0;
}

/* Is the width of box i, which holds the change, independent of its content? */
static bool lq_width_free(int i)
{
    const LayoutBox *b = lq_box(i);
    int p = lq_parent(i);
    if (p < 0) return true;                       /* the root: the viewport */
    if (b->s->width_keyword) LQ_NO("width keyword");
    bool definite = b->s->width_set || b->s->width_percent > 0.0;
    if (b->s->display == CSS_DISPLAY_INLINE) LQ_NO("inline");
    if (b->s->display == CSS_DISPLAY_INLINE_BLOCK || b->s->inline_level || b->s->float_side) {
        if (!definite) LQ_NO("shrink-to-fit width");
        return true;
    }
    if (lq_oof(b)) {
        if (!definite && !((b->s->positioned_sides & LAYOUT_SIDE_LEFT) && (b->s->positioned_sides & LAYOUT_SIDE_RIGHT)))
            LQ_NO("shrink-to-fit out-of-flow width");
        return true;
    }
    const LayoutBox *pb = lq_box(p);
    if (pb->s->display == CSS_DISPLAY_FLEX) {
        bool row = pb->s->flex_direction == CSS_FLEX_DIRECTION_ROW || pb->s->flex_direction == CSS_FLEX_DIRECTION_ROW_REVERSE;
        if (row) {
            if (!lq_flex_main_free(i)) LQ_NO("flex item sized by content");
            return true;
        }
        if (definite) return true;
        int align = b->s->align_self ? b->s->align_self - 1 : (int)pb->s->align_items;
        if (align != CSS_ALIGN_STRETCH || (b->s->margin_auto & (2 | 8))) LQ_NO("column item not stretched");
        return true;
    }
    if (g_lq_kind[p] & LQ_KIND_ROW) LQ_NO("table cell");
    return true;                                     /* a block in flow fills its container */
}

/* Does box p place its child a where it was, whatever a's own size and
 * the content after a (and, `changes`, a's own content)? */
static bool lq_place_free(int p, int a, bool changes)
{
    const LayoutBox *A = lq_box(a), *P = lq_box(p);
    if (A->s->has_translate && (A->s->translate_xpct != 0.0 || A->s->translate_ypct != 0.0)) LQ_NO("percentage translate");
    if (A->s->position == CSS_POSITION_FIXED) {
        if ((A->s->positioned_sides & LAYOUT_SIDE_BOTTOM) && !(A->s->positioned_sides & LAYOUT_SIDE_TOP))
            LQ_NO("fixed at the bottom");
        return true;
    }
    /* Absolute: against an ancestor whose height may change (its x needs
     * the width, which lq_width_free covers). */
    if (A->s->position == CSS_POSITION_ABSOLUTE) {
        if (lq_reads_cb_height(A)) LQ_NO("absolute by its containing block's height");
        return true;
    }
    if (A->s->position != CSS_POSITION_STATIC && (A->s->inset_pct_bits & (1 | 4))) LQ_NO("relative percentage offset");
    if (P->s->display == CSS_DISPLAY_FLEX) {
        if (P->s->flex_direction != CSS_FLEX_DIRECTION_ROW && P->s->flex_direction != CSS_FLEX_DIRECTION_ROW_REVERSE)
            LQ_NO("column flex container");
        if (P->s->flex_wrap != CSS_FLEX_WRAP_NOWRAP) LQ_NO("wrapping flex container");
        int align = A->s->align_self ? A->s->align_self - 1 : (int)P->s->align_items;
        if ((align != CSS_ALIGN_STRETCH && align != CSS_ALIGN_FLEX_START) || (A->s->margin_auto & (1 | 4)))
            LQ_NO("flex item aligned by the line");
        /* Main sizes (and so positions) come from the items' bases: those
         * after a (and a's, when its content changes) must not come from
         * their content. */
        const LayoutNodeRef *nodes = g_lq_layout.tree.nodes;
        for (int s = changes ? a : nodes[a].next_sibling_idx; s >= 0; s = nodes[s].next_sibling_idx) {
            const LayoutBox *S = lq_box(s);
            if (S->s->display == CSS_DISPLAY_NONE || lq_oof(S)) continue;
            if ((g_lq_kind[s] & LQ_KIND_TEXT) && s != a) continue;   /* its text changes only with p's children */
            if (!lq_flex_main_free(s)) LQ_NO("flex sibling sized by content");
        }
        return true;
    }
    if (!lq_block_container(p)) LQ_NO("inline or table row container");
    if (!lq_block_level(A) && !A->s->float_side) LQ_NO("inline-level");
    return true;
}

/* Box a and every box above it up to `bound` (-1: the root), which hold the
 * change: as wide as before and where they were. */
static bool lq_chain_free(int a, int bound)
{
    for (; a >= 0; a = lq_parent(a)) {
        if (!lq_width_free(a)) return false;
        int p = lq_parent(a);
        if (p >= 0 && !lq_place_free(p, a, true)) return false;
        if (a == bound) break;
    }
    return true;
}

/* Does box i start a block formatting context (layout_establishes_bfc):
 * floats inside it stay inside, and no margin collapses through its edges? */
static bool lq_bfc(int i)
{
    const LayoutBox *b = lq_box(i);
    int p = lq_parent(i);
    if (p < 0 || b->s->float_side || b->s->overflow != 0 || lq_oof(b)) return true;
    if (b->s->display == CSS_DISPLAY_INLINE_BLOCK || (b->s->inline_level && b->s->display != CSS_DISPLAY_INLINE)) return true;
    CssDisplay pd = lq_box(p)->s->display;
    return pd == CSS_DISPLAY_FLEX || pd == CSS_DISPLAY_GRID || (g_lq_kind[i] & LQ_KIND_CELL);
}

/* Is box i a size boundary: one whose box its content cannot change (as
 * wide and as high whatever is inside, and nothing inside reaching out)?
 * A change inside it then alters nothing outside it. */
static bool lq_size_boundary(int i)
{
    const LayoutBox *b = lq_box(i);
    if (b->s->display == CSS_DISPLAY_NONE || b->s->display == CSS_DISPLAY_INLINE) return false;
    return lq_bfc(i) && lq_height_free(i, -1) && lq_width_free(i);
}

/* Can a :has() rule restyle box i (its element) when something below it, or
 * after it among its siblings, changes? */
static bool lq_has_subject(int i)
{
    if (g_lq_kind[i] & LQ_KIND_TEXT) return false;
    return !g_lq_layout.has_flags || (g_lq_layout.has_flags[i] & LAYOUT_HAS_SUBJECT);
}

/* Can a change at box i (below it, or with `self` to i itself) restyle,
 * through :has(), anything outside i's subtree: an ancestor (or i) that
 * may be a subject, an earlier sibling of one of them (sibling-relative
 * arguments), or a target of a rule whose :has() compound names nothing?
 * (A box whose own attributes changed since the layout has a region or
 * anchor of its own.) */
static bool lq_has_reach(int i, bool self)
{
    if (!css_layout_has_any(&g_lq_layout)) return false;
    if (!g_lq_layout.has_flags || g_lq_layout.has_target_first < g_lq_layout.tree.count) return true;
    bool sib = css_layout_has_sibling_relative(&g_lq_layout);
    const LayoutNodeRef *nodes = g_lq_layout.tree.nodes;
    for (int a = i; a >= 0; a = nodes[a].parent_idx) {
        if ((a != i || self) && lq_has_subject(a)) return true;
        for (int s = sib ? nodes[a].prev_sibling_idx : -1; s >= 0; s = nodes[s].prev_sibling_idx)
            if (lq_has_subject(s)) return true;
    }
    return false;
}

/* What a change at or below box `top` (an ancestor of the query box qi, or
 * a box after it) can restyle through :has() without touching qi: nothing
 * from `top` up, nothing before qi (the earlier siblings of those boxes, for
 * sibling-relative arguments, apart from those after sq: L's child holding
 * qi, when top is L), nor (":has(x) [dialog]") an element up to qi the
 * rule's rightmost compound names. */
static bool lq_has_free(int qi, int top, int sq, int sc)
{
    const LayoutNodeRef *nodes = g_lq_layout.tree.nodes;
    if (g_lq_layout.has_target_first < g_lq_end[qi]) LQ_NO(":has() target before");
    for (int a = top; a >= 0; a = lq_parent(a))
        if (lq_has_subject(a)) LQ_NO(":has() subject above");
    if (!css_layout_has_sibling_relative(&g_lq_layout)) return true;
    for (int s = sc >= 0 ? nodes[sc].prev_sibling_idx : -1; s >= 0; s = nodes[s].prev_sibling_idx)
        if (g_lq_pos[s] <= g_lq_pos[sq] && lq_has_subject(s)) LQ_NO(":has() sibling subject");
    for (int a = top; a >= 0; a = lq_parent(a))
        for (int s = nodes[a].prev_sibling_idx; s >= 0; s = nodes[s].prev_sibling_idx)
            if (lq_has_subject(s)) LQ_NO(":has() sibling subject above");
    return true;
}

static bool lq_anchor_style_kept(int i);

/* Query box qi wholly before anchor i, inside `bound`. */
static bool lq_before_independent(int qi, int i, int bound)
{
    int ci = g_lq_anchor[i];
    bool has = g_lq_anchor_has[i];
    int L = lq_parent(qi);
    while (L >= 0 && !lq_anchor_is_ancestor_or_self(L, ci)) L = lq_parent(L);
    if (L < 0) LQ_NO("no common ancestor");
    int sq = qi, sc = ci;                                   /* L's children holding each */
    while (lq_parent(sq) != L) sq = lq_parent(sq);
    while (lq_parent(sc) != L) sc = lq_parent(sc);
    if (has && !lq_has_free(qi, L, sq, sc)) return false;
    /* L and above: as wide as before and where they were. */
    if (!lq_chain_free(L, bound)) return false;
    /* L places sq as before.  An anchor that is a flex item may change its
     * own basis when restyled (and restyle the items after it). */
    const LayoutBox *Lb = lq_box(L);
    if (Lb->s->display == CSS_DISPLAY_FLEX && sc == ci && (g_lq_anchor_sib[i] || !lq_anchor_style_kept(i)))
        LQ_NO("restyled flex item");
    if (!lq_place_free(L, sq, false)) return false;
    const LayoutBox *Sq = lq_box(sq);
    bool sq_grows = false;                                  /* stretched to a line that may grow */
    if (Lb->s->display == CSS_DISPLAY_FLEX && !lq_oof(Sq) && !lq_height_fixed(Sq)) {
        int align = Sq->s->align_self ? Sq->s->align_self - 1 : (int)Lb->s->align_items;
        sq_grows = align == CSS_ALIGN_STRETCH;
    }
    if (sq_grows && (qi == sq || !lq_block_container(sq))) LQ_NO("stretched by the line");
    /* Below L: nothing on the way down reads a height that may change. */
    for (int b = qi; b != sq; b = lq_parent(b)) {
        const LayoutBox *B = lq_box(b);
        if (B->s->position == CSS_POSITION_FIXED) continue;
        int ref = lq_oof(B) ? lq_containing_block(b) : lq_parent(b);
        if (ref < 0) continue;                              /* the viewport */
        bool grows = (g_lq_pos[ref] <= g_lq_pos[L] || (ref == sq && sq_grows)) && !lq_height_fixed(lq_box(ref)) &&
                     (bound < 0 || lq_anchor_is_ancestor_or_self(bound, ref));
        if (grows && lq_reads_cb_height(B)) LQ_NO("reads a height that grows");
    }
    /* A scroll extent reads the boxes below too. */
    if (g_lq_ask == LQ_ASK_EXTENT) {
        for (int k = g_lq_pos[qi]; k < g_lq_end[qi]; k++) {
            const LayoutBox *B = lq_box(g_lq_layout.tree.preorder[k]);
            if (lq_oof(B) && lq_reads_cb_height(B)) LQ_NO("extent reads a height that grows");
        }
    }
    return true;
}

/* Query box qi holding the anchor ci (strictly), inside `bound`: its size
 * comes from no content, it and its ancestors are placed as before, and
 * :has() restyles nothing from qi up or before it. */
static bool lq_contains_independent(int qi, int ci, int bound, bool has)
{
    (void)ci;
    if (g_lq_ask == LQ_ASK_EXTENT) LQ_NO("extent holds the change");
    if (has && !lq_has_free(qi, qi, qi, -1)) return false;
    if (!lq_height_free(qi, bound)) LQ_NO("height from content");
    return lq_chain_free(qi, bound);
}

/* Does anchor i's own style hold: noted with its changes, or, restyled on
 * demand, its element computes the style it had in the cached layout (its
 * changes then restyle only what is below or after it)?  The answer stands
 * until another change to it merges in (lq_anchor_add).  CYBER_LQ_ONDEMAND=0:
 * not restyled. */
static bool lq_anchor_style_kept(int i)
{
    static int env = -1;
    if (env < 0) { const char *e = cyber_getenv("CYBER_LQ_ONDEMAND"); env = !(e && e[0] == '0'); }
    if (g_lq_anchor_kept[i]) return true;
    if (g_lq_anchor_tried[i] || !env) return false;
    g_lq_anchor_tried[i] = true;
    int ci = g_lq_anchor[i];
    int dom = html_mirror_index_of(g_lq_doc, g_lq_ids[ci]);
    if (dom < 0 || !css_layout_style_unchanged(&g_lq_layout, ci, dom)) return false;
    g_lq_anchor_kept[i] = true;
    g_lq_ondemand_kept++;   /* (answers given before stay right, only cautious) */
    return true;
}

/* Can query box qi keep its geometry through the change at anchor i? */
static bool lq_anchor_independent(int qi, int i)
{
    int ci = g_lq_anchor[i], bound = g_lq_anchor_bound[i];
    if (bound >= 0 && !lq_anchor_is_ancestor_or_self(bound, qi)) {
        /* Outside its region; an extent covers the boxes below, the region too. */
        if (g_lq_ask == LQ_ASK_EXTENT && lq_anchor_is_ancestor_or_self(qi, bound)) LQ_NO("extent holds the region");
        return true;
    }
    if (g_lq_end[qi] <= g_lq_pos[ci]) return lq_before_independent(qi, i, bound);
    if (qi != ci && lq_anchor_is_ancestor_or_self(qi, ci)) return lq_contains_independent(qi, ci, bound, g_lq_anchor_has[i]);
    if (qi == ci && lq_anchor_style_kept(i)) {
        /* Its own style holds: as for a change inside it, but an attribute
         * may restyle its later siblings, which a flex container sizes it by. */
        int p = lq_parent(qi);
        if (g_lq_anchor_sib[i] && p >= 0 && lq_box(p)->s->display == CSS_DISPLAY_FLEX) LQ_NO("restyled flex siblings");
        return lq_contains_independent(qi, ci, bound, g_lq_anchor_has[i]);
    }
    LQ_NO("after or at the change");
}

static bool lq_who_affected_at(int qi);

/* The box holding element `id`, which is not in the cached layout, when
 * that box is hidden (it or an ancestor display:none), else -1. */
static int lq_hidden_holder(NodeId id)
{
    bool inside;
    int h = id ? lq_box_or_holder(id, &inside) : -1;
    if (h < 0 || !inside) return -1;
    for (int a = h; a >= 0; a = g_lq_layout.tree.nodes[a].parent_idx)
        if (g_lq_layout.boxes[a].s->display == CSS_DISPLAY_NONE) return h;
    return -1;
}

/* Does the query being answered read a box a change may have altered? */
static bool lq_who_affected(void)
{
    if (!g_lq_ndirty && !g_lq_nanchor) return false;
    int qi = g_lq_who ? lq_box_index(g_lq_who) : -1;
    LqAsk ask = g_lq_ask;
    if (qi < 0) {
        /* Left out of the layout inside a display:none subtree (or put in one
         * since): unrendered while the box holding it stays hidden, so it is
         * asked about as that box's style. */
        qi = lq_hidden_holder(g_lq_who);
        if (qi < 0) return true;
        g_lq_ask = LQ_ASK_STYLE;
    }
    bool affected = lq_who_affected_at(qi);
    g_lq_ask = ask;
    return affected;
}

static bool lq_who_affected_at(int qi)
{
    if (g_lq_ndirty) {
        int m = lq_in_region(g_lq_who, qi);
        /* Where boxes only moved, sizes, extents and styles hold. */
        if (m == 2 || (m == 1 && g_lq_ask == LQ_ASK_POS)) return true;
        /* An extent covers the boxes below: a region under it moves them. */
        for (int i = 0; g_lq_ask == LQ_ASK_EXTENT && i < g_lq_ndirty; i++)
            if (g_lq_dirty[i] != qi && lq_is_ancestor_or_self(qi, g_lq_dirty[i])) return true;
    }
    if (g_lq_nanchor) {
        /* The same query against the same anchors: the same answer. */
        unsigned bit = 1u << (2 * (unsigned)g_lq_ask);
        if (g_lq_ind_ver[qi] != g_lq_anchor_ver) { g_lq_ind_ver[qi] = g_lq_anchor_ver; g_lq_ind[qi] = 0; }
        if (!(g_lq_ind[qi] & bit)) {
            bool ind = true;
            for (int i = 0; i < g_lq_nanchor && ind; i++) {
                ind = lq_anchor_independent(qi, i);
                if (!ind && g_lq_log > 1) {
                    NodeId an = g_lq_ids[g_lq_anchor[i]];
                    bool el = an && dom_valid(an) && dom_kind(an) == DOM_K_ELEMENT;
                    const char *ac = el ? dom_get_attr_str(an, "class") : NULL;
                    int bd = g_lq_anchor_bound[i];
                    NodeId bn = bd >= 0 ? g_lq_ids[bd] : 0;
                    bool bel = bn && dom_valid(bn) && dom_kind(bn) == DOM_K_ELEMENT;
                    const char *bc = bel ? dom_get_attr_str(bn, "class") : NULL;
                    const char *qc = dom_get_attr_str(g_lq_who, "class");
                    fprintf(stderr, "[LQ] not independent of anchor <%s.%.40s> (region <%s.%.30s>) for <%s.%.40s> (ask %d): %s\n",
                            el ? dom_tag_name(an) : "#text", ac ? ac : "", bel ? dom_tag_name(bn) : "-", bc ? bc : "",
                            dom_tag_name(g_lq_who), qc ? qc : "", (int)g_lq_ask, g_lq_no ? g_lq_no : "?");
                }
            }
            g_lq_ind[qi] |= (unsigned char)(bit | (ind ? bit << 1 : 0));
        }
        if (!(g_lq_ind[qi] & (bit << 1))) return true;
        g_lq_anchor_answers++;
    }
    return false;
}

/* Add the regions of every change since the cached layout; false when one
 * can reach anywhere (`why` says which). */
static bool lq_note_changes(HtmlDocument *doc, char *why, size_t cap)
{
    snprintf(why, cap, "?");
    if (g_dom_relayout_requests != g_lq_relayouts) { snprintf(why, cap, "relayout request"); return false; }
    const HtmlMirrorChange *ch = NULL;
    size_t n = 0;
    if (!html_mirror_changes(doc, g_lq_change_pos, &ch, &n)) { snprintf(why, cap, "change log reset"); return false; }
    if (n == 0) { snprintf(why, cap, "unrecorded change"); return false; }
    static int anchors_env = -1;
    if (anchors_env < 0) { const char *e = cyber_getenv("CYBER_LQ_ANCHOR"); anchors_env = !(e && e[0] == '0'); }
    bool anchors = anchors_env && g_lq_pos_ok;
    for (size_t i = 0; i < n; i++) {
        bool move, has, kept;
        int anchor, from;
        int r = lq_change_reach(&ch[i], why, cap, &move, &anchor, &from, &has, &kept);
        if (r == -2) continue;
        /* The lowest size boundary holding it (inside its region) bounds it
         * tighter: nothing outside one changes. */
        if (anchors && !has && !move && anchor >= 0 && r != -2) {
            for (int a = from; a >= 0 && (r < 0 || lq_anchor_is_ancestor_or_self(r, a)); a = lq_parent(a)) {
                if (a == r) break;
                if (lq_size_boundary(a)) { r = a; break; }
            }
        }
        if (g_lq_log > 1) {
            int at = r >= 0 ? r : anchor;
            NodeId id = at >= 0 && g_lq_pos_ok ? g_lq_ids[at] : 0;
            bool el = id && dom_valid(id) && dom_kind(id) == DOM_K_ELEMENT;
            const char *cl = el ? dom_get_attr_str(id, "class") : NULL;
            fprintf(stderr, "[LQ] %s <%s.%.40s>%s%s from %s\n", r >= 0 ? "region" : anchor >= 0 ? "anchor" : "anywhere",
                    el ? dom_tag_name(id) : "?", cl ? cl : "", move ? " (moved)" : "", has ? " (:has)" : "", why);
        }
        if (r == -1 && (anchor < 0 || !anchors)) return false;
        if (r >= 0 && (move || !anchors || anchor < 0)) {
            if (!lq_dirty_add(r, move)) { snprintf(why, cap, "too many changed regions"); return false; }
            continue;
        }
        /* A change inside region r (an out-of-flow box) alters nothing
         * outside it: an anchor bounded by it. */
        bool sib = ch[i].kind == HTML_MIRROR_ATTRS && anchor >= 0 && g_lq_ids[anchor] == ch[i].id;
        if (!lq_anchor_add(anchor, r >= 0 ? r : -1, has, kept, sib)) { snprintf(why, cap, "too many anchors"); return false; }
    }
    return true;
}

/* CYBER_LQ_SKIP_VERIFY=1: the kept layout against a new one, outside the
 * changed regions (the boxes scripts read). */
static void lq_verify_kept(HtmlDocument *doc, double vw, double vh)
{
    static uint64_t checks, failing;
    LayoutContext fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.js_ctx = g_lq_layout.js_ctx;
    fresh.cache_readonly = true;   /* (the kept layout's styles stay cached) */
    bool was = g_lq_running;
    g_lq_running = true;
    bool ok = css_layout_run(&fresh, doc, NULL, vw, vh);
    g_lq_running = was;
    int diffs = 0, compared = 0;
    for (int i = 0; ok && i < fresh.tree.count; i++) {
        HtmlNode *hn = (HtmlNode *)po_array_payload(&doc->array, fresh.tree.nodes[i].dom_node_idx);
        if (!hn || hn->type != HTML_NODE_ELEMENT || !hn->store_id) continue;
        int k = lq_box_index(hn->store_id);
        int region = lq_in_region(hn->store_id, k);
        if (region == 2) continue;
        /* Boxes an anchor may alter (all those it may not are checked). */
        bool past = false;
        for (int a = 0; a < g_lq_nanchor && !past; a++) past = k < 0 || !lq_anchor_independent(k, a);
        if (past) continue;
        bool shown = true;
        for (int a = i; a >= 0; a = fresh.tree.nodes[a].parent_idx)
            if (fresh.boxes[a].s->display == CSS_DISPLAY_NONE) { shown = false; break; }
        bool kshown = k >= 0 && lq_hidden(hn->store_id) == 0;
        const LayoutBox *f = &fresh.boxes[i];
        const LayoutBox *o = k >= 0 ? &g_lq_layout.boxes[k] : NULL;
        compared++;
        bool differ = shown != kshown;
        if (!differ && shown)
            differ = (!region && (fabs(f->x - o->x) > 0.01 || fabs(f->y - o->y) > 0.01)) || fabs(f->width - o->width) > 0.01 ||
                     fabs(f->height - o->height) > 0.01 || f->s->display != o->s->display || f->s->position != o->s->position ||
                     fabs(f->s->font_size - o->s->font_size) > 0.01;
        if (!differ) continue;
        if (diffs++ < 3) {
            const char *idv = dom_get_attr_str(hn->store_id, "id");
            const char *cls = dom_get_attr_str(hn->store_id, "class");
            fprintf(stderr, "[lq-verify] kept layout differs at <%s#%s.%.40s> %u: %s %.1f,%.1f %.1fx%.1f, new %s %.1f,%.1f %.1fx%.1f\n",
                    hn->tag_name, idv ? idv : "", cls ? cls : "", hn->store_id, kshown ? "shown" : "hidden",
                    o ? o->x : 0, o ? o->y : 0, o ? o->width : 0, o ? o->height : 0,
                    shown ? "shown" : "hidden", f->x, f->y, f->width, f->height);
        }
    }
    css_layout_tree_free(&fresh);
    checks++;
    if (diffs) failing++;
    fprintf(stderr, "[lq-verify] kept layout %llu (%d regions): %d of %d boxes differ (%llu of %llu checks differ)\n",
            (unsigned long long)checks, g_lq_ndirty, diffs, compared, (unsigned long long)failing,
            (unsigned long long)checks);
}

static bool lq_rendered(int idx);

/* CYBER_LQ_SKIP_VERIFY=1: the box a query is answered with from the kept
 * layout against a new layout's. */
static void lq_verify_asked(HtmlDocument *doc, double vw, double vh)
{
    static uint64_t checks, failing;
    if (!g_lq_who || !doc || (!g_lq_nanchor && !g_lq_ndirty)) return;
    LayoutContext fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.js_ctx = g_lq_layout.js_ctx;
    fresh.cache_readonly = true;   /* (the kept layout's styles stay cached) */
    bool was = g_lq_running;
    g_lq_running = true;
    bool ok = css_layout_run(&fresh, doc, NULL, vw, vh);
    g_lq_running = was;
    int fi = -1;
    for (int i = 0; ok && i < fresh.tree.count && fi < 0; i++) {
        HtmlNode *hn = (HtmlNode *)po_array_payload(&doc->array, fresh.tree.nodes[i].dom_node_idx);
        if (hn && hn->type == HTML_NODE_ELEMENT && hn->store_id == g_lq_who) fi = i;
    }
    bool fshown = fi >= 0;
    for (int a = fi; a >= 0; a = fresh.tree.nodes[a].parent_idx)
        if (fresh.boxes[a].s->display == CSS_DISPLAY_NONE) { fshown = false; break; }
    int ki = lq_box_index(g_lq_who);
    bool kshown = ki >= 0 && lq_rendered(ki);
    const LayoutBox *f = fi >= 0 ? &fresh.boxes[fi] : NULL, *k = ki >= 0 ? &g_lq_layout.boxes[ki] : NULL;
    bool differ = ok && (fshown != kshown ||
                         (fshown && (fabs(f->x - k->x) > 0.01 || fabs(f->y - k->y) > 0.01 ||
                                     fabs(f->width - k->width) > 0.01 || fabs(f->height - k->height) > 0.01 ||
                                     fabs(f->s->font_size - k->s->font_size) > 0.01)));
    checks++;
    if (differ) {
        failing++;
        const char *idv = dom_get_attr_str(g_lq_who, "id");
        const char *cls = dom_get_attr_str(g_lq_who, "class");
        fprintf(stderr, "[lq-verify] asked <%s#%s.%.40s> (ask %d): kept %s %.1f,%.1f %.1fx%.1f, new %s %.1f,%.1f %.1fx%.1f; "
                "%d anchors, %d regions (%llu of %llu differ)\n",
                dom_tag_name(g_lq_who), idv ? idv : "", cls ? cls : "", (int)g_lq_ask,
                kshown ? "shown" : "hidden", k ? k->x : 0, k ? k->y : 0, k ? k->width : 0, k ? k->height : 0,
                fshown ? "shown" : "hidden", f ? f->x : 0, f ? f->y : 0, f ? f->width : 0, f ? f->height : 0,
                g_lq_nanchor, g_lq_ndirty, (unsigned long long)failing, (unsigned long long)checks);
    }
    css_layout_tree_free(&fresh);
}

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
    if (vw != g_lq_vw || vh != g_lq_vh) {
        g_lq_key = 0; g_lq_vw = vw; g_lq_vh = vh; g_lq_cheap_ok = false;
    }
    static int skip_env = -1, skip_verify = -1;
    if (g_lq_log < 0) { const char *e = cyber_getenv("CYBER_LQ_LOG"); g_lq_log = e ? (atoi(e) > 0 ? atoi(e) : 1) : 0; }
    if (skip_env < 0) { const char *e = cyber_getenv("CYBER_LQ_SKIP"); skip_env = !(e && e[0] == '0'); }
    if (skip_verify < 0) skip_verify = cyber_getenv("CYBER_LQ_SKIP_VERIFY") ? 1 : 0;
    char why[200] = "first layout";

    uint64_t cheap[LQ_CHEAP_N];
    lq_cheap_key(cheap);
    unsigned long long now = (unsigned long long)page_clock_now_ms();
    bool full = false;
    uint64_t key = g_lq_valid ? g_lq_key : 0;
    HtmlDocument *doc = NULL;
    if (g_lq_valid && g_lq_cheap_ok && now - g_lq_full_at < LQ_FULL_CHECK_MS &&
        memcmp(cheap, g_lq_cheap, sizeof(cheap)) == 0) {
        g_lq_cheap_hits++;
        if (!lq_who_affected()) {
            if (skip_verify) lq_verify_asked(g_lq_doc, vw, vh);
            return true;
        }
    } else {
        bool unchanged = false;
        /* Between full checks the snapshot copies the subtrees nothing marked
         * dirty; a full one at least every LQ_FULL_CHECK_MS also re-reads the
         * state only JS objects hold. CYBER_SNAPSHOT_INC=0 always takes full ones. */
        static int inc_env = -1;
        if (inc_env < 0) {
            const char *e = cyber_getenv("CYBER_SNAPSHOT_INC");
            inc_env = !(e && e[0] == '0');
        }
        full = !inc_env || !g_lq_cheap_ok || now - g_lq_full_at >= LQ_FULL_CHECK_MS;
        {
            CP_ACC("forced layout: serialize+snapshot");
            doc = full ? html_document_from_js_dom_if_changed(ctx, js_doc, &key, &unchanged)
                       : html_document_from_js_dom_incremental(ctx, js_doc, &key, &unchanged);
        }
        if (unchanged && g_lq_valid) {
            if (full) lq_note_full_check(now);
            else lq_cheap_key(g_lq_cheap);
            if (!lq_who_affected()) {
                if (skip_verify) lq_verify_asked(g_lq_doc, vw, vh);
                return true;
            }
            doc = NULL;
        } else if (!doc) {
            return g_lq_valid;
        } else if (skip_env && g_lq_valid && doc == g_lq_doc) {
            bool bounded;
            {
                CP_ACC("forced layout: change check");
                bounded = lq_note_changes(doc, why, sizeof(why));
            }
            g_lq_change_pos = html_mirror_change_seq();
            if (bounded) {
                if (skip_verify) lq_verify_kept(doc, vw, vh);
                g_lq_key = key;
                if (full) lq_note_full_check(now);
                else lq_cheap_key(g_lq_cheap);
                if (!lq_who_affected()) {
                    if (skip_verify) lq_verify_asked(doc, vw, vh);
                    html_document_free(doc);   /* the snapshot's reference: the layout holds its own */
                    g_lq_kept++;
                    return true;
                }
                html_document_free(doc);
                doc = NULL;
            }
        } else if (g_lq_valid) {
            snprintf(why, sizeof(why), doc != g_lq_doc ? "new document" : "CYBER_LQ_SKIP=0");
        }
    }
    if (!doc) {
        /* The cached document is current, but the query reads a box a
         * change may have altered: lay it out again. */
        int qi = g_lq_who ? lq_box_index(g_lq_who) : -1;
        int r0 = g_lq_ndirty ? g_lq_dirty[0] : -1;
        HtmlNode *rn = r0 >= 0 ? (HtmlNode *)po_array_payload(&g_lq_doc->array, g_lq_layout.tree.nodes[r0].dom_node_idx) : NULL;
        const char *rcls = rn && rn->store_id ? dom_get_attr_str(rn->store_id, "class") : NULL;
        snprintf(why, sizeof(why), "asked inside a changed region or past an anchor (%d, %d anchors; ask %d, in %d; first <%s.%.40s>%s)",
                 g_lq_ndirty, g_lq_nanchor, (int)g_lq_ask, lq_in_region(g_lq_who, qi), rn ? rn->tag_name : "?",
                 rcls ? rcls : "", g_lq_ndirty && g_lq_dirty_move[0] ? " moved" : "");
        doc = g_lq_doc;
        doc->refs++;
        g_lq_region_layouts++;
    }

    g_lq_running = true;
    if (g_lq_log && g_lq_valid && doc == g_lq_doc) {
        /* what changed since the cached layout, by kind */
        const HtmlMirrorChange *ch = NULL;
        size_t nch = 0;
        unsigned k[4] = { 0 };
        if (html_mirror_changes(doc, g_lq_change_pos0, &ch, &nch))
            for (size_t i = 0; i < nch; i++) if (ch[i].kind < 4) k[ch[i].kind]++;
        fprintf(stderr, "[LQ] changes since the last layout: %u attrs, %u children, %u sheets\n", k[1], k[2], k[3]);
    }
    if (g_lq_log) {
        const char *tn = g_lq_who ? css_atom_str(g_dom.tag[dom_slot(g_lq_who)]) : "?";
        const char *idv = g_lq_who ? dom_get_attr_str(g_lq_who, "id") : NULL;
        const char *cls = g_lq_who ? dom_get_attr_str(g_lq_who, "class") : NULL;
        fprintf(stderr, "[LQ] forced layout %llu: %s#%s.%.80s (%s)\n", (unsigned long long)g_lq_layouts + 1,
                tn ? tn : "?", idv ? idv : "", cls ? cls : "", why);
        fflush(stderr);
    }
    uint64_t t0 = gc_os_now_ns() / 1000, c0 = platform_thread_cpu_us();
    /* CYBER_LQ_LOG: whether the layout changed the box it was run for (when
     * not, a precise enough test of the changes could have kept it). */
    LayoutBox asked_was;
    bool asked_had = false;
    if (g_lq_log && g_lq_valid && g_lq_who) {
        int oi = lq_box_index(g_lq_who);
        if (oi >= 0 && lq_rendered(oi)) { asked_was = g_lq_layout.boxes[oi]; asked_had = true; }
    }
    lq_free_keeping(doc == g_lq_doc);
    g_lq_doc = doc;
    g_lq_layout.js_ctx = ctx;
    g_lq_layout.style_all = g_lq_style_all;
    /* Forced layouts run often with little to do: on this thread (waking
     * the pool for them cost more than it saved).  CYBER_LQ_PAR=1: in
     * parallel. */
    static int par_env = -1;
    if (par_env < 0) par_env = cyber_getenv("CYBER_LQ_PAR") && cyber_getenv("CYBER_LQ_PAR")[0] == '1';
    g_lq_layout.serial = !par_env;
    if (g_lq_style_all) g_lq_style_all_layouts++;
    g_lq_style_all = false;
    {
        CP_ACC("forced layout: layout");
        g_lq_valid = css_layout_run(&g_lq_layout, doc, NULL, vw, vh);
    }
    g_lq_ndirty = 0;
    g_lq_nanchor = 0;
    g_lq_anchor_ver++;
    g_lq_mark_stale = true;
    if (g_lq_valid) {
        g_lq_key = key;
        if (doc) {
            if (full) lq_note_full_check(now);
            else lq_cheap_key(g_lq_cheap);
        }
        lq_map_slots();
        if (asked_had) {
            int ni = lq_box_index(g_lq_who);
            const LayoutBox *nb = ni >= 0 && lq_rendered(ni) ? &g_lq_layout.boxes[ni] : NULL;
            bool same = nb && fabs(nb->x - asked_was.x) < 0.01 && fabs(nb->y - asked_was.y) < 0.01 &&
                        fabs(nb->width - asked_was.width) < 0.01 && fabs(nb->height - asked_was.height) < 0.01;
            fprintf(stderr, "[LQ] asked box %s\n", same ? "same" : "changed");
        }
        g_lq_change_pos = html_mirror_change_seq();
        g_lq_relayouts = g_dom_relayout_requests;
        g_lq_change_pos0 = g_lq_change_pos;
    } else {
        lq_free();
        g_lq_key = 0;
    }
    g_lq_layouts++;
    uint64_t took = gc_os_now_ns() / 1000 - t0;
    g_lq_layout_us += took;
    g_lq_layout_cpu_us += platform_thread_cpu_us() - c0;
    if (g_lq_log)
        fprintf(stderr, "[LQ] forced layout %llu took %llu ms\n", (unsigned long long)g_lq_layouts,
                (unsigned long long)(took / 1000));
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
        if (g_lq_layout.boxes[i].s->display == CSS_DISPLAY_NONE) return false;
    return idx >= 0;
}

/* Rendered box index for a JS element (laying out if needed), or -1. */
static int lq_query(JSContextHandle ctx, GCValue element)
{
    g_lq_queries++;
    NodeId id = dom_node_of(element);
    if (!id || dom_kind(id) != DOM_K_ELEMENT) return -1;
    g_lq_who = id;
    bool ok = lq_ensure(ctx);
    g_lq_who = 0;
    if (!ok) return -1;
    int idx = lq_box_index(id);
    return lq_rendered(idx) ? idx : -1;
}

bool layout_query_font_size(JSContextHandle ctx, GCValue element, double *px)
{
    *px = 0.0;
    NodeId id = dom_node_of(element);
    g_lq_who = id;
    g_lq_ask = LQ_ASK_STYLE;
    bool ok = id && lq_ensure(ctx);
    g_lq_ask = LQ_ASK_POS;
    g_lq_who = 0;
    if (!ok) return false;
    int idx = lq_box_index(id);
    if ((idx >= 0 && (g_lq_layout.boxes[idx].flags & LAYOUT_STYLE_SKIPPED)) || (idx < 0 && lq_hidden_holder(id) >= 0)) {
        /* Inside a display:none subtree, which layouts do not style (or leave
         * out): lay out again styling everything. */
        layout_api_invalidate();
        g_lq_style_all = true;
        g_lq_who = id;
        g_lq_ask = LQ_ASK_STYLE;
        ok = lq_ensure(ctx);
        g_lq_ask = LQ_ASK_POS;
        g_lq_who = 0;
        if (!ok) return false;
        idx = lq_box_index(id);
    }
    if (idx < 0 || g_lq_layout.boxes[idx].s->font_size <= 0.0) return false;
    *px = g_lq_layout.boxes[idx].s->font_size;
    return true;
}

bool layout_query_cascade(JSContextHandle ctx, GCValue element, LayoutCascadeFn fn, void *ud, LayoutQueryUsed *used)
{
    memset(used, 0, sizeof(*used));
    double fs = 0;
    /* (styles the element, laying out again with everything styled when it
     * is inside a display:none subtree) */
    if (!layout_query_font_size(ctx, element, &fs)) return false;
    NodeId id = dom_node_of(element);
    int idx = lq_box_index(id);
    if (idx < 0) return false;
    const LayoutBox *b = &g_lq_layout.boxes[idx];
    used->font_size = b->s->font_size;
    static const char *const roles[] = { NULL, "table", "table-row-group", "table-row", "table-cell", "table-caption", "table-column" };
    switch (b->s->display) {
    case CSS_DISPLAY_BLOCK: used->display = "block"; break;
    case CSS_DISPLAY_INLINE_BLOCK: used->display = "inline-block"; break;
    case CSS_DISPLAY_FLEX: used->display = "flex"; break;
    case CSS_DISPLAY_GRID: used->display = "grid"; break;
    case CSS_DISPLAY_NONE: used->display = "none"; break;
    case CSS_DISPLAY_LIST_ITEM: used->display = "list-item"; break;
    case CSS_DISPLAY_OTHER:
        used->display = b->s->table_role > 0 && b->s->table_role < 7 ? roles[b->s->table_role] : "inline";
        break;
    default: used->display = "inline"; break;
    }
    used->rendered = lq_rendered(idx) && !(b->flags & LAYOUT_STYLE_SKIPPED) && b->s->display != CSS_DISPLAY_NONE;
    if (used->rendered) {
        used->content_width = b->content_width;
        used->content_height = b->content_height;
        double m[4] = { b->margin_top, b->margin_right, b->margin_bottom, b->margin_left };
        double p[4] = { b->padding_top, b->padding_right, b->padding_bottom, b->padding_left };
        double r[4] = { b->s->border_top, b->s->border_right, b->s->border_bottom, b->s->border_left };
        memcpy(used->margin, m, sizeof m);
        memcpy(used->padding, p, sizeof p);
        memcpy(used->border, r, sizeof r);
    }
    return css_layout_element_cascade(&g_lq_layout, idx, fn, ud);
}

char *layout_query_custom(JSContextHandle ctx, GCValue element, const char *name)
{
    double fs = 0;
    if (!layout_query_font_size(ctx, element, &fs)) return NULL;   /* (styles it) */
    int idx = lq_box_index(dom_node_of(element));
    const char *v = idx >= 0 ? css_layout_element_custom(&g_lq_layout, idx, name) : NULL;
    return v ? strdup(v) : NULL;
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
    out->border_top = b->s->border_top;
    out->border_right = b->s->border_right;
    out->border_bottom = b->s->border_bottom;
    out->border_left = b->s->border_left;
    out->inline_level = b->s->display == CSS_DISPLAY_INLINE;
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
    if (b->s->position == CSS_POSITION_FIXED) return DOM_NULL;
    bool self_static = b->s->position == CSS_POSITION_STATIC;
    for (NodeId a = dom_parent(id); a && dom_kind(a) == DOM_K_ELEMENT; a = dom_parent(a)) {
        if (is_tag(a, "body")) return a;
        int ai = lq_box_index(a);
        if (ai >= 0 && g_lq_layout.boxes[ai].s->position != CSS_POSITION_STATIC) return a;
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
        if (cb->s->display == CSS_DISPLAY_NONE) continue;
        if (cb->x + cb->width > *right) *right = cb->x + cb->width;
        if (cb->y + cb->height > *bottom) *bottom = cb->y + cb->height;
        lq_extent(c, right, bottom);
    }
}

static GCValue lq_metric(JSContextHandle ctx, GCValue this_val, int which)
{
    NodeId id = dom_node_of(this_val);
    g_lq_ask = which == M_SCROLL_W || which == M_SCROLL_H ? LQ_ASK_EXTENT :
               which == M_OFFSET_T || which == M_OFFSET_L ? LQ_ASK_POS : LQ_ASK_SIZE;
    int idx = lq_query(ctx, this_val);
    g_lq_ask = LQ_ASK_POS;
    bool root = id && is_root_element(id);
    /* The root element's client box is the viewport. */
    if (root && (which == M_CLIENT_W || which == M_CLIENT_H)) {
        if (!g_lq_valid) return JS_NewInt32(ctx, 0);
        return lq_int(ctx, which == M_CLIENT_W ? g_lq_vw : g_lq_vh);
    }
    if (idx < 0) return JS_NewInt32(ctx, 0);
    const LayoutBox *b = &g_lq_layout.boxes[idx];
    bool inl = b->s->display == CSS_DISPLAY_INLINE;
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
            v -= which == M_OFFSET_T ? pb->y + pb->s->border_top : pb->x + pb->s->border_left;
        }
        return lq_int(ctx, v);
    }
    case M_CLIENT_W: return lq_int(ctx, inl ? 0 : b->width - b->s->border_left - b->s->border_right);
    case M_CLIENT_H: return lq_int(ctx, inl ? 0 : b->height - b->s->border_top - b->s->border_bottom);
    case M_CLIENT_T: return lq_int(ctx, inl ? 0 : b->s->border_top);
    case M_CLIENT_L: return lq_int(ctx, inl ? 0 : b->s->border_left);
    case M_SCROLL_W:
    case M_SCROLL_H: {
        if (inl) return JS_NewInt32(ctx, 0);
        double left = b->x + b->s->border_left, top = b->y + b->s->border_top;
        double cw = root ? g_lq_vw : b->width - b->s->border_left - b->s->border_right;
        double ch = root ? g_lq_vh : b->height - b->s->border_top - b->s->border_bottom;
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
    g_lq_ask = LQ_ASK_STYLE;
    int idx = lq_query(ctx, this_val);
    g_lq_ask = LQ_ASK_POS;
    NodeId op = lq_offset_parent(id, idx);
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

uint64_t layout_api_cheap_hits(void)
{
    return g_lq_cheap_hits;
}

uint64_t layout_api_style_all_layouts(void)
{
    return g_lq_style_all_layouts;
}

uint64_t layout_api_cpu_ms(void)
{
    return g_lq_layout_cpu_us / 1000;
}

uint64_t layout_api_ondemand_kept(void)
{
    return g_lq_ondemand_kept;
}

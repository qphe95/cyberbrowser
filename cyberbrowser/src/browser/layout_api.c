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
 * boxes a script asks about.  Each change the mirror recorded since it
 * (html_mirror_changes) either alters nothing (an attribute no selector
 * names and layout does not read, a paint-only inline style, anything in a
 * display:none subtree that no :has() rule above could see), stays inside
 * one subtree (an inline offset or transform change moves only its box; an
 * out-of-flow box keeps changes to its size or to anything inside it to
 * itself), or can reach anywhere.  Queries outside the changed subtrees
 * read the cached boxes; one inside lays out again, as does a change that
 * can reach anywhere, a sheet edit, adopted sheets or an image load
 * (dom_request_relayout).
 * CYBER_LQ_SKIP=0 turns this off; CYBER_LQ_SKIP_VERIFY=1 lays out anyway
 * and compares.
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
/* Mirror change-log position and relayout requests at the cached layout;
 * layouts kept because nothing since could change a box. */
static uint64_t g_lq_change_pos, g_lq_relayouts, g_lq_kept, g_lq_region_layouts;

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
    if (layout_ms) *layout_ms = g_lq_layout_ms;
}

uint64_t layout_api_kept(void)
{
    return g_lq_kept;
}

uint64_t layout_api_region_layouts(void)
{
    return g_lq_region_layouts;
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
            if (g_lq_layout.boxes[a].display == CSS_DISPLAY_NONE) return 2;
        return g_lq_layout.boxes[idx].display == CSS_DISPLAY_NONE ? 1 : 0;
    }
    /* Not in the layout (added since): what holds it. */
    for (NodeId a = dom_parent(id); a && dom_kind(a) == DOM_K_ELEMENT; a = dom_parent(a)) {
        int ai = lq_box_index(a);
        if (ai < 0) continue;
        for (int b = ai; b >= 0; b = nodes[b].parent_idx)
            if (g_lq_layout.boxes[b].display == CSS_DISPLAY_NONE) return 2;
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
    return horizontal ? (b->width_set || b->width_percent > 0.0 || b->width_keyword)
                      : (b->height_set || b->height_percent > 0.0);
}

static bool lq_out_of_flow(int idx)
{
    const LayoutBox *b = &g_lq_layout.boxes[idx];
    return b->position == CSS_POSITION_ABSOLUTE || b->position == CSS_POSITION_FIXED;
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
 * other than style matters to some selector or to layout; the furthest
 * reach of the inline style change (css_layout_inline_style_reach). */
static void lq_attrs_reach(const HtmlAttribute *was, const HtmlAttribute *now, bool *other, int *style_reach,
                           unsigned *sides, char *why, size_t cap, const char *tag)
{
    *other = false;
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
                if (r > *style_reach) {
                    *style_reach = r;
                    if (!*other) snprintf(why, cap, "<%s> style [%.50s -> %.50s]", tag, ov ? ov : "", nv ? nv : "");
                }
            } else if (css_layout_attr_change_matters(&g_lq_layout, &owner, a->name, ov, nv)) {
                if (!*other) snprintf(why, cap, "<%s> %s [%.40s -> %.40s]", tag, a->name, ov ? ov : "", nv ? nv : "");
                *other = true;
            }
        }
    }
}

/* What change `c` can alter in the cached layout: -2 nothing, -1 anything
 * (lay out again), else the layout index of the subtree it stays in
 * (*move: where boxes only move). */
static int lq_change_reach(const HtmlMirrorChange *c, char *why, size_t cap, bool *move)
{
    *move = false;
    const char *tag = c->id && dom_valid(c->id) ? dom_tag_name(c->id) : "?";
    if (c->kind == HTML_MIRROR_SHEETS) { snprintf(why, cap, "stylesheets (<%s>)", tag); return -1; }
    int st = lq_hidden(c->id);
    bool has = css_layout_has_may_depend(&g_lq_layout, c->id);
    /* Inside a display:none subtree (or under its own display:none box,
     * unless :empty flips with it), only :has() could see a change. */
    bool hidden = st == 2 || (st == 1 && c->kind == HTML_MIRROR_CHILDREN &&
                              (!c->empty_changed || !css_layout_uses_empty(&g_lq_layout)));
    if (hidden && !has) return -2;
    bool inside;
    int idx = lq_box_or_holder(c->id, &inside);
    if (c->kind == HTML_MIRROR_ATTRS) {
        bool other;
        int reach;
        unsigned sides;
        lq_attrs_reach(c->old_attrs, c->new_attrs, &other, &reach, &sides, why, cap, tag);
        if (!other && reach == 0) return -2;
        if (has || idx < 0) return -1;
        /* Its inline style alone: offsets move just its subtree, and an
         * out-of-flow box keeps its size changes to itself.  Only a box
         * whose size the changed insets cannot set (an out-of-flow one with
         * auto width between left and right) keeps its size. */
        if (!inside && !other && (reach == 1 || (reach == 2 && lq_out_of_flow(idx)))) {
            const LayoutBox *b = &g_lq_layout.boxes[idx];
            unsigned ps = b->positioned_sides | sides;
            bool oof = lq_out_of_flow(idx);
            *move = reach == 1 &&
                    !(oof && (sides & (LAYOUT_SIDE_LEFT | LAYOUT_SIDE_RIGHT)) &&
                      (ps & LAYOUT_SIDE_LEFT) && (ps & LAYOUT_SIDE_RIGHT) && !lq_size_definite(b, true)) &&
                    !(oof && (sides & (LAYOUT_SIDE_TOP | LAYOUT_SIDE_BOTTOM)) &&
                      (ps & LAYOUT_SIDE_TOP) && (ps & LAYOUT_SIDE_BOTTOM) && !lq_size_definite(b, false));
            return idx;
        }
        /* An element strictly inside an out-of-flow box: its style, its
         * later siblings' and its descendants' all stay inside that box. */
        int a = lq_out_of_flow_root(idx, inside);
        return a >= 0 ? a : -1;
    }
    snprintf(why, cap, "<%s> children%s", tag, st == 2 || st == 1 ? " (hidden, :has)" : "");
    if (has || idx < 0) return -1;
    /* Children of an out-of-flow box, or of anything inside one, reflow it
     * alone (unless :empty changes the box itself). */
    int a = lq_out_of_flow_root(idx, true);
    if (a == idx && !inside && c->empty_changed && css_layout_uses_empty(&g_lq_layout)) return -1;
    return a;
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
    for (size_t i = 0; i < n; i++) {
        bool move;
        int r = lq_change_reach(&ch[i], why, cap, &move);
        if (r == -1) return false;
        if (r >= 0 && g_lq_log > 1) {
            HtmlNode *rn = (HtmlNode *)po_array_payload(&doc->array, g_lq_layout.tree.nodes[r].dom_node_idx);
            const char *rc = rn && rn->store_id ? dom_get_attr_str(rn->store_id, "class") : NULL;
            fprintf(stderr, "[LQ] region <%s.%.40s>%s from %s\n", rn ? rn->tag_name : "?", rc ? rc : "",
                    move ? " (moved)" : "", why);
        }
        if (r >= 0 && !lq_dirty_add(r, move)) { snprintf(why, cap, "too many changed regions"); return false; }
    }
    return true;
}

/* Does the query being answered read a box a change may have altered? */
static bool lq_who_affected(void)
{
    if (!g_lq_ndirty) return false;
    int qi = g_lq_who ? lq_box_index(g_lq_who) : -1;
    if (qi < 0) return true;
    int m = lq_in_region(g_lq_who, qi);
    /* Where boxes only moved, sizes, extents and styles hold. */
    if (m == 2 || (m == 1 && g_lq_ask == LQ_ASK_POS)) return true;
    /* An extent covers the boxes below: a region under it moves them. */
    for (int i = 0; g_lq_ask == LQ_ASK_EXTENT && i < g_lq_ndirty; i++)
        if (g_lq_dirty[i] != qi && lq_is_ancestor_or_self(qi, g_lq_dirty[i])) return true;
    return false;
}

/* CYBER_LQ_SKIP_VERIFY=1: the kept layout against a new one, outside the
 * changed regions (the boxes scripts read). */
static void lq_verify_kept(HtmlDocument *doc, double vw, double vh)
{
    static uint64_t checks, failing;
    LayoutContext fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.js_ctx = g_lq_layout.js_ctx;
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
        bool shown = true;
        for (int a = i; a >= 0; a = fresh.tree.nodes[a].parent_idx)
            if (fresh.boxes[a].display == CSS_DISPLAY_NONE) { shown = false; break; }
        bool kshown = k >= 0 && lq_hidden(hn->store_id) == 0;
        const LayoutBox *f = &fresh.boxes[i];
        const LayoutBox *o = k >= 0 ? &g_lq_layout.boxes[k] : NULL;
        compared++;
        bool differ = shown != kshown;
        if (!differ && shown)
            differ = (!region && (fabs(f->x - o->x) > 0.01 || fabs(f->y - o->y) > 0.01)) || fabs(f->width - o->width) > 0.01 ||
                     fabs(f->height - o->height) > 0.01 || f->display != o->display || f->position != o->position ||
                     fabs(f->font_size - o->font_size) > 0.01;
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
    unsigned long long now = platform_get_time_ms();
    bool full = false;
    uint64_t key = g_lq_valid ? g_lq_key : 0;
    HtmlDocument *doc = NULL;
    if (g_lq_valid && g_lq_cheap_ok && now - g_lq_full_at < LQ_FULL_CHECK_MS &&
        memcmp(cheap, g_lq_cheap, sizeof(cheap)) == 0) {
        g_lq_cheap_hits++;
        if (!lq_who_affected()) return true;
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
            if (!lq_who_affected()) return true;
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
        snprintf(why, sizeof(why), "asked inside a changed region (%d; ask %d, in %d; first <%s.%.40s>%s)",
                 g_lq_ndirty, (int)g_lq_ask, lq_in_region(g_lq_who, qi), rn ? rn->tag_name : "?",
                 rcls ? rcls : "", g_lq_ndirty && g_lq_dirty_move[0] ? " moved" : "");
        doc = g_lq_doc;
        doc->refs++;
        g_lq_region_layouts++;
    }

    g_lq_running = true;
    if (g_lq_log) {
        const char *tn = g_lq_who ? css_atom_str(g_dom.tag[dom_slot(g_lq_who)]) : "?";
        const char *idv = g_lq_who ? dom_get_attr_str(g_lq_who, "id") : NULL;
        const char *cls = g_lq_who ? dom_get_attr_str(g_lq_who, "class") : NULL;
        fprintf(stderr, "[LQ] forced layout %llu: %s#%s.%.80s (%s)\n", (unsigned long long)g_lq_layouts + 1,
                tn ? tn : "?", idv ? idv : "", cls ? cls : "", why);
        fflush(stderr);
    }
    unsigned long long t0 = platform_get_time_ms();
    lq_free();
    g_lq_doc = doc;
    g_lq_layout.js_ctx = ctx;
    {
        CP_ACC("forced layout: layout");
        g_lq_valid = css_layout_run(&g_lq_layout, doc, NULL, vw, vh);
    }
    g_lq_ndirty = 0;
    g_lq_mark_stale = true;
    if (g_lq_valid) {
        g_lq_key = key;
        if (doc) {
            if (full) lq_note_full_check(now);
            else lq_cheap_key(g_lq_cheap);
        }
        lq_map_slots();
        g_lq_change_pos = html_mirror_change_seq();
        g_lq_relayouts = g_dom_relayout_requests;
    } else {
        lq_free();
        g_lq_key = 0;
    }
    g_lq_layouts++;
    g_lq_layout_ms += platform_get_time_ms() - t0;
    if (g_lq_log)
        fprintf(stderr, "[LQ] forced layout %llu took %llu ms\n", (unsigned long long)g_lq_layouts,
                (unsigned long long)(platform_get_time_ms() - t0));
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
    if (idx < 0 || g_lq_layout.boxes[idx].font_size <= 0.0) return false;
    *px = g_lq_layout.boxes[idx].font_size;
    return true;
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

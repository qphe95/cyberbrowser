/*
 * CSS Layout Engine
 *
 * Parallel CSS layout as described in PARALLEL_CSS_LAYOUT.md.
 *
 * Two passes over a flat layout tree built from an HtmlDocument:
 *   - Top-down (pre-order): inherited / containing-block values.
 *   - Bottom-up (post-order): auto heights, content sizes, scroll extents.
 *
 * Both passes are dispatched to the GC thread pool and synchronize with
 * fine-grained spin-waits on per-node atomic flags/counters.
 */

#ifndef CSS_LAYOUT_H
#define CSS_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include "html_dom.h"
#include "css_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bit flags for LayoutBox.flags */
#define LAYOUT_NEEDS_LAYOUT 0x01
#define LAYOUT_HAS_LAYOUT   0x02

/* Layout values for a single DOM node. */
typedef struct LayoutBox {
    double x, y;
    double width, height;
    double margin_top, margin_right, margin_bottom, margin_left;
    double padding_top, padding_right, padding_bottom, padding_left;
    double border_top, border_right, border_bottom, border_left;
    double content_width, content_height;
    double baseline;

    /* Resolved colors (RGBA, 0..1). */
    double color_r, color_g, color_b, color_a;
    double background_color_r, background_color_g, background_color_b, background_color_a;

    uint32_t flags;
    uint32_t _pad;
} LayoutBox;

/* Flat node reference used by both passes. */
typedef struct LayoutNodeRef {
    int dom_node_idx;       /* index into HtmlDocument's PreOrderCompactionArray */
    int parent_idx;         /* index in layout array, or -1 for root */
    int first_child_idx;
    int next_sibling_idx;
    int child_count;
} LayoutNodeRef;

/* Flat layout tree and its synchronization state. */
typedef struct LayoutTree {
    LayoutNodeRef *nodes;
    int count;
    int root_idx;

    int *preorder;          /* indices in parent-before-children order */
    int *postorder;         /* indices in children-before-parent order */
} LayoutTree;

typedef struct LayoutNodeState {
    volatile uint32_t top_down_done;     /* 0 = pending, 1 = done */
    volatile uint32_t children_remaining;
    volatile uint32_t bottom_up_done;    /* 0 = pending, 1 = done */
} LayoutNodeState;

typedef struct LayoutContext {
    HtmlDocument *doc;
    LayoutTree tree;
    LayoutNodeState *states;
    LayoutBox *boxes;       /* boxes[i] corresponds to tree.nodes[i] */

    /* Viewport / initial containing block dimensions. */
    double viewport_width;
    double viewport_height;
} LayoutContext;

/* Build a flat layout tree from a parsed HTML document. */
bool css_layout_tree_build(LayoutContext *ctx, HtmlDocument *doc);

/* Free resources held by a layout tree. */
void css_layout_tree_free(LayoutContext *ctx);

/* Run both layout passes in parallel. */
bool css_layout_document(LayoutContext *ctx, CssStylesheet *sheet);

/* Convenience: allocate/resolve/free in one call. */
bool css_layout_run(LayoutContext *ctx, HtmlDocument *doc, CssStylesheet *sheet,
                    double viewport_width, double viewport_height);

/* Access the resolved LayoutBox for a DOM node index, or NULL if not found. */
LayoutBox* css_layout_box_for_node(LayoutContext *ctx, int dom_node_idx);

#ifdef __cplusplus
}
#endif

#endif /* CSS_LAYOUT_H */

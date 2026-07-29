/*
 * CSS Layout Engine
 *
 * Parallel CSS layout as described in PARALLEL_CSS_LAYOUT.md.
 *
 * Two passes over a flat layout tree built from an HtmlDocument:
 *   - Top-down (pre-order): inherited / containing-block values, plus block
 *     sibling stacking and inline line-flow.  Each node spin-waits on both its
 *     parent and its previous sibling, forming a dependency chain that gives a
 *     valid topological order for parallel execution.
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
#include "quickjs_gc_unified.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bit flags for LayoutBox.flags */
#define LAYOUT_NEEDS_LAYOUT 0x01
#define LAYOUT_HAS_LAYOUT   0x02

/* CSS display value used for visibility decisions. */
typedef enum {
    CSS_DISPLAY_INLINE,
    CSS_DISPLAY_BLOCK,
    CSS_DISPLAY_INLINE_BLOCK,
    CSS_DISPLAY_FLEX,
    CSS_DISPLAY_GRID,
    CSS_DISPLAY_NONE,
    CSS_DISPLAY_LIST_ITEM,
    CSS_DISPLAY_OTHER
} CssDisplay;

/* CSS visibility value. */
typedef enum {
    CSS_VISIBILITY_VISIBLE,
    CSS_VISIBILITY_HIDDEN,
    CSS_VISIBILITY_COLLAPSE
} CssVisibility;

/* CSS flex-direction value. */
typedef enum {
    CSS_FLEX_DIRECTION_ROW,
    CSS_FLEX_DIRECTION_ROW_REVERSE,
    CSS_FLEX_DIRECTION_COLUMN,
    CSS_FLEX_DIRECTION_COLUMN_REVERSE
} CssFlexDirection;

/* CSS flex-wrap value. */
typedef enum {
    CSS_FLEX_WRAP_NOWRAP,
    CSS_FLEX_WRAP_WRAP,
    CSS_FLEX_WRAP_WRAP_REVERSE
} CssFlexWrap;

/* CSS justify-content value. */
typedef enum {
    CSS_JUSTIFY_FLEX_START,
    CSS_JUSTIFY_FLEX_END,
    CSS_JUSTIFY_CENTER,
    CSS_JUSTIFY_SPACE_BETWEEN,
    CSS_JUSTIFY_SPACE_AROUND,
    CSS_JUSTIFY_SPACE_EVENLY
} CssJustifyContent;

/* CSS align-items value. */
typedef enum {
    CSS_ALIGN_STRETCH,
    CSS_ALIGN_FLEX_START,
    CSS_ALIGN_FLEX_END,
    CSS_ALIGN_CENTER
} CssAlignItems;

/* CSS position value. */
typedef enum {
    CSS_POSITION_STATIC,
    CSS_POSITION_RELATIVE,
    CSS_POSITION_ABSOLUTE,
    CSS_POSITION_FIXED,
    CSS_POSITION_STICKY
} CssPosition;

/* CSS box-sizing value. */
typedef enum {
    CSS_BOX_SIZING_CONTENT_BOX,
    CSS_BOX_SIZING_BORDER_BOX
} CssBoxSizing;

/* CSS text-align value (justify is treated as left). */
typedef enum {
    CSS_TEXT_ALIGN_LEFT,
    CSS_TEXT_ALIGN_CENTER,
    CSS_TEXT_ALIGN_RIGHT
} CssTextAlign;

/* Layout values for a single DOM node. */
typedef struct LayoutBox {
    double x, y;
    double width, height;
    /* Parsed CSS width/height values before box-sizing conversion.  width/height
     * above always store the final border-box totals. */
    double css_width, css_height;
    /* Set when width/height were explicitly assigned (even to 0): an explicit
     * zero is a real size, not "auto". */
    unsigned char width_set, height_set;
    /* Set when visibility was explicitly assigned (for inheritance). */
    unsigned char visibility_set;
    double margin_top, margin_right, margin_bottom, margin_left;
    double padding_top, padding_right, padding_bottom, padding_left;
    double border_top, border_right, border_bottom, border_left;
    /* Border color (single slot; most real-world borders are uniform).
     * Defaults to the text color (currentColor) when unset. */
    double border_color_r, border_color_g, border_color_b, border_color_a;
    unsigned char border_color_set;
    double content_width, content_height;
    double baseline;

    /* Resolved colors (RGBA, 0..1). */
    double color_r, color_g, color_b, color_a;
    double background_color_r, background_color_g, background_color_b, background_color_a;
    /* Set when a rule/inline style explicitly assigned `color`; used to
     * decide inheritance from the parent box. */
    unsigned char color_set;
    /* Set when font-size was explicitly assigned (for inheritance). */
    unsigned char font_size_set;

    /* Text alignment for inline content (inherited like color). */
    CssTextAlign text_align;
    unsigned char text_align_set;

    /* CSS float/clear.  float_side: 0=none, 1=left, 2=right.
     * clear: 0=none, 1=left, 2=right, 3=both. */
    unsigned char float_side;
    unsigned char clear;

    /* overflow/overflow-x/overflow-y: 0=visible, 1=hidden, 2=auto, 3=scroll.
     * Any value other than visible makes the box establish a new block
     * formatting context (CSS 2.1 §9.4.1), which blocks margin collapsing
     * with in-flow children (§8.3.1) and stops empty-box through-collapse. */
    unsigned char overflow;

    /* vertical-align: 0=baseline, 1=middle, 2=top, 3=bottom.
     * Table cells (td/th) default to middle per the HTML UA stylesheet;
     * everything else defaults to baseline.  Only middle/top/bottom are
     * honoured for table-cell content alignment at the moment. */
    unsigned char vertical_align;

    /* Resolved typography. */
    double font_size;
    char   font_family[64];
    /* Font weight 100..900 (400 = normal, 700 = bold) and italic flag. */
    int    font_weight;
    unsigned char font_italic;
    unsigned char font_weight_set;
    unsigned char font_italic_set;
    /* text-decoration bitmask: 1 = underline, 2 = line-through, 4 = overline. */
    unsigned char text_decoration;
    unsigned char text_decoration_set;
    /* Resolved line-height in px; <= 0 means "normal" (1.5 x font-size). */
    double line_height;
    unsigned char line_height_set;
    /* Unitless line-height (<number>) inherits as a RATIO, not as the
     * computed pixel value (CSS 2.1 §10.8.1): > 0 here means line_height
     * must be recomputed as ratio x own font-size once that is final. */
    double line_height_ratio;

    /* em-unit margins/paddings resolve against the element's FINAL computed
     * font-size, which is only known after the pass-3 font-size fixup.
     * Declarations in em are therefore recorded as ratios here (in side
     * order top/right/bottom/left) and re-resolved in pass 3; the pixel
     * fields above always hold a provisional value in the meantime.
     * em_deferred bits: 0-3 = margin T/R/B/L, 4-7 = padding T/R/B/L. */
    double margin_em[4];
    double padding_em[4];
    unsigned char em_deferred;

    /* list-style for display:list-item markers. */
    unsigned char list_style_type;   /* 0=none, 1=disc, 2=circle, 3=square, 4=decimal */
    unsigned char list_style_type_set;

    /* Background image URL, if any. */
    char   background_image_url[256];

    CssDisplay display;
    CssVisibility visibility;
    CssPosition position;
    CssBoxSizing box_sizing;
    CssFlexDirection flex_direction;
    CssFlexWrap flex_wrap;
    CssJustifyContent justify_content;
    CssAlignItems align_items;

    double flex_basis;
    double flex_basis_percent; /* >0 when flex-basis was a percentage */
    double flex_grow;
    double flex_shrink;

    /* Word-wrap state for text boxes whose run wraps across lines.
     * wrap_cont_w > 0 marks the box as wrapped: display emission wraps the
     * text with first line width wrap_first_w, continuation lines starting at
     * wrap_cont_x with width wrap_cont_w.  When a float below the run start
     * stops intruding mid-wrap, wrap_cont2_line (>1) is the 1-based line
     * number from which continuation lines instead use wrap_cont2_x /
     * wrap_cont2_w (e.g. full width once the text passes the float). */
    double wrap_first_w;
    double wrap_cont_w;
    double wrap_cont_x;
    double wrap_cont2_x;
    double wrap_cont2_w;
    int    wrap_cont2_line;

    double min_width, max_width;
    double min_height, max_height;
    double top, left, right, bottom;
    double width_percent;   /* >0 if width was specified as a percentage. */
    double height_percent;
    double gap_row, gap_col;

    /* Aspect ratio derived from percentage padding-top/bottom when height
     * is otherwise auto (common thumbnail placeholder technique). */
    double aspect_ratio;

    /* Temporary line-flow state used during the top-down pass.  Each parent
     * box tracks the current line position; children update it sequentially
     * through the previous-sibling dependency chain. */
    double line_x;
    double line_y_offset;

    uint32_t flags;
    /* Bitmask of which top/left/right/bottom values were explicitly set. */
#define LAYOUT_SIDE_LEFT   0x01
#define LAYOUT_SIDE_RIGHT  0x02
#define LAYOUT_SIDE_TOP    0x04
#define LAYOUT_SIDE_BOTTOM 0x08
    uint32_t positioned_sides;

    /* >0 when font-size was specified relative to the parent's font-size
     * (percentage or em).  The ratio is applied against the parent's final
     * font-size in a serial preorder pass after the parallel style apply,
     * because the parallel pass cannot safely read the parent's computed
     * font-size. */
    double font_size_ratio;
} LayoutBox;

/* Flat node reference used by both passes. */
typedef struct LayoutNodeRef {
    int dom_node_idx;       /* index into HtmlDocument's PreOrderCompactionArray */
    int parent_idx;         /* index in layout array, or -1 for root */
    int first_child_idx;
    int next_sibling_idx;
    int prev_sibling_idx;   /* previous sibling in layout array, or -1 for first child */
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

/* Forward declaration for custom properties storage. */
typedef struct CssCustomProps CssCustomProps;

/* A placed float, used to compute line insets while flowing inline content.
 * side: 1=left, 2=right. */
typedef struct {
    double left, right, bottom;
    unsigned char side;
} FloatRec;

typedef struct LayoutContext {
    HtmlDocument *doc;
    LayoutTree tree;
    LayoutNodeState *states;
    LayoutBox *boxes;       /* boxes[i] corresponds to tree.nodes[i] */

    /* Viewport / initial containing block dimensions. */
    double viewport_width;
    double viewport_height;

    /* Base URL used to resolve relative image / stylesheet URLs. */
    char base_url[512];

    /* Per-node custom property (CSS variable) map. */
    CssCustomProps *custom_props;

    /* Floats placed so far during the flow, in page coordinates.  Inline
     * lines shrink around them while they are vertically active; they also
     * serve clear: left/right/both. */
    FloatRec *float_stack;
    int float_count;
    int float_cap;

    /* Optional JS context for collecting constructed/adopted stylesheets. */
    JSContextHandle js_ctx;
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

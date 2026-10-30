/*
 * CSS Layout Engine (PARALLEL_STYLE_LAYOUT.md).
 *
 * css_layout_run() builds a flat layout tree from an HtmlDocument, then:
 *   - style: stylesheets are parsed once per distinct text (in parallel, in
 *     chunks for large sheets) and compiled into a cached CssStyleSet;
 *     elements are styled top-down by range jobs on the job system, with the
 *     compiled bucketed matcher, an ancestor Bloom filter and a style-sharing
 *     cache;
 *   - layout: one recursive pass; flex items and table cells, which their
 *     container places before laying out, fork into jobs when no float can
 *     cross between them.
 * Workers never touch the GC heap or QuickJS.  The result is identical for
 * any thread count.  Runs on the engine thread, synchronously.
 */

#ifndef CSS_LAYOUT_H
#define CSS_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include "html_dom.h"
#include "css_parser.h"
#include "quickjs_gc_unified.h"
#include "frame_arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bit flags for LayoutBox.flags */
#define LAYOUT_NEEDS_LAYOUT 0x01
#define LAYOUT_HAS_LAYOUT   0x02
/* Inside a display:none subtree, so not styled (LayoutContext.style_all
 * off): a display:none placeholder. */
#define LAYOUT_STYLE_SKIPPED 0x04

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
/* The computed style of a box: what the cascade produced, shared by the
 * boxes whose styles are equal (LayoutBox.s).  The layout passes only read
 * it; what they compute is in LayoutBox. */
typedef struct LayoutStyle {    /* Parsed CSS width/height values before box-sizing conversion.  width/height
     * above always store the final border-box totals. */
    double css_width, css_height;
    /* Set when width/height were explicitly assigned (even to 0): an explicit
     * zero is a real size, not "auto". */
    unsigned char width_set, height_set;
    /* Set when visibility was explicitly assigned (for inheritance). */
    unsigned char visibility_set;
    double border_top, border_right, border_bottom, border_left;
    /* Border color (single slot; most real-world borders are uniform).
     * Defaults to the text color (currentColor) when unset. */
    double border_color_r, border_color_g, border_color_b, border_color_a;
    unsigned char border_color_set;

    /* Resolved colors (RGBA, 0..1). */
    double color_r, color_g, color_b, color_a;
    double background_color_r, background_color_g, background_color_b, background_color_a;
    /* Set when a rule/inline style explicitly assigned `color`; used to
     * decide inheritance from the parent box. */
    unsigned char color_set;
    /* SVG fill (inherited): 0 initial (black), 1 currentColor, 2 the
     * fill_* color, 3 none.  Passed to inline <svg> painting. */
    unsigned char fill_kind, fill_set;
    double fill_r, fill_g, fill_b, fill_a;
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
    /* First font-family name, interned (lives for the process; "" unset). */
    const char *font_family;
    /* Font weight 100..900 (400 = normal, 700 = bold) and italic flag. */
    int    font_weight;
    unsigned char font_italic;
    /* Font-registry slot for the family / weight / style (resolved after
     * the cascade, before layout; -1 = not resolved). */
    int    font_slot;
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

    /* Specified margins, paddings, min/max sizes and insets in px (their
     * percentage parts are below): what the box's used values start from
     * when it is laid out (layout_box_start). */
    double margin_top, margin_right, margin_bottom, margin_left;
    double padding_top, padding_right, padding_bottom, padding_left;
    double min_width, max_width, min_height, max_height;
    double top, left, right, bottom;
    /* Left unstyled inside a display:none subtree (only display is set;
     * the box gets LAYOUT_STYLE_SKIPPED). */
    unsigned char skipped;

    /* em-unit margins/paddings resolve against the element's FINAL computed
     * font-size, which is only known after the pass-3 font-size fixup.
     * Declarations in em are therefore recorded as ratios here (in side
     * order top/right/bottom/left) and re-resolved in pass 3; the pixel
     * fields above always hold a provisional value in the meantime.
     * em_deferred bits: 0-3 = margin T/R/B/L, 4-7 = padding T/R/B/L. */
    double margin_em[4];
    double padding_em[4];
    unsigned char em_deferred;

    /* Margins and paddings with a percentage part (50%, calc(10px + 5%))
     * resolve against the containing block WIDTH at layout time (CSS 2.1
     * §8.3/§8.4, also for top/bottom): side value = base px + pct x width.
     * Side order top/right/bottom/left.  pct_sides bits: 0-3 margin
     * T/R/B/L, 4-7 padding T/R/B/L. */
    double margin_pct[4], margin_base[4];
    double padding_pct[4], padding_base[4];
    unsigned char pct_sides;

    /* list-style for display:list-item markers. */
    unsigned char list_style_type;   /* 0=none, 1=disc, 2=circle, 3=square, 4=decimal */
    unsigned char list_style_type_set;

    /* Background image URL, interned (lives for the process; "" none). */
    const char *background_image_url;

    /* mask-image: an absolute image URL (mask_kind 1) or the text of a
     * linear-gradient(...) (mask_kind 2), interned (lives for the process);
     * NULL / 0 = no mask.  mask-size: kind 0 auto, 1 contain, 2 cover,
     * 3 explicit (mask_size_w / _h in px, or fractions of the box when the
     * matching mask_size_pct bit is set; a negative height means auto).
     * mask-position: x / y in px, or fractions when mask_pos_pct bits are set.
     * mask_no_repeat: mask-repeat: no-repeat. */
    const char *mask_image;
    unsigned char mask_kind;
    unsigned char mask_size_kind;
    unsigned char mask_size_pct;
    unsigned char mask_pos_pct;
    unsigned char mask_no_repeat;
    double mask_size_w, mask_size_h;
    double mask_pos_x, mask_pos_y;

    /* border-radius per corner (top-left, top-right, bottom-right,
     * bottom-left): px, or a fraction of the box when the corner's bit in
     * radius_pct is set (elliptical radii use their horizontal value). */
    float radius[4];
    unsigned char radius_pct;

    /* box-shadow: the first outer (non-inset) shadow, if any. */
    unsigned char shadow_set;
    float shadow_x, shadow_y, shadow_blur, shadow_spread;
    float shadow_r, shadow_g, shadow_b, shadow_a;

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
    double flex_basis_pct_px;  /* px addend of a calc() basis with a % part */
    double flex_grow;
    double flex_shrink;
    /* min/max sizes with a percentage part, resolved at layout time against
     * the containing block (widths: its width; heights: its height, or none
     * when that is indefinite).  Order min-w, max-w, min-h, max-h; bit i of
     * minmax_pct_bits set when entry i has a percentage part. */
    double minmax_pct[4], minmax_base[4];
    unsigned char minmax_pct_bits;
    /* >0 if width/height had a percentage part; *_pct_px is the px addend
     * of a calc() mix (calc(100% - 20px) = 100% and -20px). */
    double width_percent, width_pct_px;
    double height_percent, height_pct_px;
    double gap_row, gap_col;

    /* Preferred aspect ratio as height / width: from the aspect-ratio
     * property (aspect_ratio_prop set; it sizes the box box-sizing names),
     * or from a ::before{padding-top:N%} placeholder, which sizes the content
     * box since pseudo boxes are not built. */
    double aspect_ratio;
    unsigned char aspect_ratio_prop;
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

    /* Outer display is inline but the box is laid out as an atomic inline
     * (inline-flex / inline-grid / inline-table; inline-block uses
     * CSS_DISPLAY_INLINE_BLOCK).  Cleared by blockification. */
    unsigned char inline_level;
    /* An inline element laid out as a block because it holds block-level
     * children (block-in-inline): percentages in it resolve against the
     * block around it.  Set by blockification. */
    unsigned char block_in_inline;
    /* display: table-caption (sized at min-content in a shrink-to-fit table). */
    unsigned char table_caption;
    /* Margins specified as `auto`: 1=top 2=right 4=bottom 8=left. */
    unsigned char margin_auto;
    /* align-self: 0 = auto (use the container's align-items), otherwise
     * CssAlignItems + 1. */
    unsigned char align_self;
    /* CSS `order` for flex items. */
    int order;
    /* min-width / min-height given explicitly (bit 0 / bit 1), so that an
     * explicit 0 disables the flex automatic minimum size. */
    unsigned char min_size_set;
    /* Per-side border bookkeeping (bits 1=left 2=right 4=top 8=bottom):
     * a side with border-style none/hidden has a used width of 0, and a
     * styled side without an explicit width is `medium` (3px). */
    unsigned char border_styled;
    unsigned char border_width_set;
    /* transform: translation part only (px plus fraction of own size). */
    double translate_x, translate_y;
    double translate_xpct, translate_ypct;
    unsigned char has_translate;
    /* width: min-content (1) / max-content (2) / fit-content (3). */
    unsigned char width_keyword;
    /* Percentage insets (top, right, bottom, left): bit set => the
     * corresponding inset_pct ratio applies against the containing block. */
    double inset_pct[4];
    unsigned char inset_pct_bits;
    /* white-space: 0 normal, 1 nowrap, 2 pre, 3 pre-wrap, 4 pre-line. */
    unsigned char white_space;
    unsigned char white_space_set;
    /* opacity (when opacity_set) and the effective opacity including all
     * ancestors, computed after layout for painting. */
    double opacity;
    unsigned char opacity_set;
    /* display:-webkit-box (legacy), -webkit-box-orient:vertical, and
     * -webkit-line-clamp: a vertical legacy box lays out as a block whose
     * height is capped at line_clamp lines. */
    unsigned char webkit_box;
    unsigned char box_orient_vertical;
    unsigned char line_clamp;
    /* text-overflow: ellipsis (for clipped single-line text). */
    unsigned char text_overflow_ellipsis;
    /* object-fit: 0 fill, 1 contain, 2 cover, 3 none, 4 scale-down. */
    unsigned char object_fit;

    /* CSS Grid container: the track lists and areas as specified
     * (interned, NULL = none), parsed at layout time against the final
     * font size.  grid_flow: bit 0 column, bit 1 dense. */
    const char *grid_cols, *grid_rows, *grid_areas, *grid_auto_cols, *grid_auto_rows;
    unsigned char grid_flow;
    /* Grid item placement: grid-row-start/-end, grid-column-start/-end as
     * specified (interned, NULL = auto). */
    const char *grid_rs, *grid_re, *grid_cs, *grid_ce;
    /* justify-items / justify-self: 0 = normal/auto, else CssAlignItems + 1. */
    unsigned char justify_items, justify_self;
    /* Table part (from the tag or display: table-*): 0 none, 1 table,
     * 2 row group, 3 row, 4 cell, 5 caption, 6 column (ignored).
     * border-spacing (when border_spacing_set; else the cellspacing
     * attribute or 2px) and border-collapse: collapse (no spacing). */
    unsigned char table_role;
    unsigned char border_spacing_set, border_collapse;
    double border_spacing_h, border_spacing_v;
} LayoutStyle;

/* A box of the layout tree: its computed style (shared, read-only once the
 * style pass is over) and the geometry the layout passes compute.  The
 * margins, paddings, min/max sizes and insets here are the used values,
 * which start from the style's specified ones (layout_box_start). */
typedef struct LayoutBox {
    LayoutStyle *s;
    double x, y;
    double width, height;
    double margin_top, margin_right, margin_bottom, margin_left;
    double padding_top, padding_right, padding_bottom, padding_left;
    double content_width, content_height;
    double baseline;

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

    uint32_t flags;
    /* Set by layout_resolve_used_sizes when a percentage height could not
     * resolve (indefinite containing block): the used height is auto. */
    unsigned char height_pct_unresolved;
    /* Text node whose collapsible leading space was dropped at a line start. */
    unsigned char text_lead_skip;
    double opacity_eff;
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
    LayoutBox *boxes;       /* boxes[i] corresponds to tree.nodes[i] */
    LayoutStyle *styles;    /* the first block of style slots (slot, below) */

    /* Viewport / initial containing block dimensions. */
    double viewport_width;
    double viewport_height;

    /* Base URL used to resolve relative image / stylesheet URLs. */
    char base_url[512];

    /* Per-node custom property (CSS variable) map; immutable, shared with
     * the parent when the node sets none (copy-on-write). */
    const CssCustomProps **custom_props;

    /* Allocations that live as long as this layout (variable maps, ...). */
    FrameArena arena;

    /* Floats placed so far during the flow, in page coordinates.  Inline
     * lines shrink around them while they are vertically active; they also
     * serve clear: left/right/both. */
    FloatRec *float_stack;
    int float_count;
    int float_cap;

    /* Optional JS context for collecting constructed/adopted stylesheets. */
    JSContextHandle js_ctx;

    /* Optional path for a tab-separated box dump after layout (NULL = none;
     * CSS_DUMP=<path> in the environment overrides). */
    const char *dump_path;

    /* Memoized intrinsic widths (2 per box: min, max; < 0 = unknown). */
    double *intrinsic_cache;

    /* Per text box: the text with white space collapsed (white-space
     * normal/nowrap/pre-line), or NULL to use the DOM text as is. */
    char **text_norm;

    /* Per document node (index): its text with generated ::after content
     * realized, or NULL.  Layout never writes the document itself (it may
     * outlive this layout and be laid out again). */
    char **text_gen;
    int text_gen_cap;

    /* Generation of the cached style set this layout used (0: none). */
    uint64_t style_gen;

    /* Style the descendants of display:none elements too (input, kept by
     * css_layout_run).  Off, nothing renders or lays them out, so they get
     * placeholder boxes (LAYOUT_STYLE_SKIPPED), and how many there were is
     * style_skipped. */
    bool style_all;
    int style_skipped;
    /* Per layout index: 1 for a display:none element whose descendants the
     * tree leaves out (css_layout_run guesses which from the style cache and
     * builds again when one turns out shown: prune_failed); how many. */
    unsigned char *pruned;
    int pruned_n;
    bool prune_failed;
    uint64_t prune_has_gen;
    /* The style cache pass this layout stored (0: none): while it is the
     * last, the cache holds this layout's styles (css_layout_style_unchanged). */
    uint64_t style_pass;

    /* Style and lay out on the calling thread (input, kept by css_layout_run):
     * for layouts run often with little to do, which waking the pool costs
     * more than it saves. */
    bool serial;
    /* Read the style cache but leave it as it is (input, kept by
     * css_layout_run): layouts that check another one. */
    bool cache_readonly;

    /* Preorder position of the first element a :has() rule whose compound
     * names nothing restyles when any ancestor's :has() flips (":has(x)
     * [dialog]"): tree.count for none, 0 when not known. */
    int has_target_first;
    /* Per layout index, for the style set's :has() rules (NULL: not known,
     * when any element may be): LAYOUT_HAS_SUBJECT when one may restyle the
     * element as its subject (a change below it, or after it among its
     * siblings, can flip it). */
    unsigned char *has_flags;
    /* The tree a successful css_layout_run left, and the mirror change log
     * position it was built for: the next run on the same mirror document
     * keeps it when no element's children changed since. */
    bool tree_keep;
    uint64_t tree_seq;
    /* This run kept the tree, and the store ids of the elements whose
     * attributes, and whose children, changed since the last (from the
     * mirror's change log). */
    bool tree_kept;
    uint32_t *attr_ids;
    int attr_n, attr_cap;
    uint32_t *child_ids;
    int child_n, child_cap;
    /* Each box's own style slot (boxes[i].s is it, or a text box's shared
     * style), from a pool that a tree rebuilt after children changed takes
     * over with the slots of the boxes it keeps (css_layout.c); styles is
     * the pool's first block (styles_n slots). */
    LayoutStyle **slot;
    struct LayoutSlots *slot_pool;
    int styles_n;
    /* Per layout index: the node's store id, and whether its box's style is
     * the one the last style pass gave it (a box new to the tree: no). */
    uint32_t *ids;
    unsigned char *style_ok;
    /* Per layout index, set by a style pass on the kept tree: the box's
     * style is the one it had (its slot, or a text box's style, left as it
     * was).  And the record of the last layout's calls that this one reuses
     * clean subtrees from (css_layout.c LayoutFlow), with this layout's
     * index -> the record's (NULL: the same; a tree rebuilt after children
     * changed maps them). */
    unsigned char *style_left;
    struct LayoutFlow *flow;
    int *flow_old;
    /* DOM index -> layout index (-1: not in the tree), dom_map_n of them. */
    int *dom_map;
    int dom_map_n;
    /* What the last style cache store recorded per layout index, for a
     * style pass on the kept tree to start from (css_layout.c): the
     * element's key, its entry (index + 1, 0 none) and the entry version
     * its style slot holds (0: not known), valid while the cache's entry
     * order is sc_order and its global key sc_global. */
    uint64_t *sc_key;
    int *sc_entry;
    uint32_t *slot_ver;
    uint64_t sc_order, sc_global;
    /* Per layout index, 8 each: the :has() checks of the element's own
     * features (css_layout.c has_memo_at; -1 not known) and whether it is
     * an element, for the :has() key sets of generation has_bits_gen. */
    signed char *has_bits;
    uint64_t has_bits_gen;
} LayoutContext;

#define LAYOUT_HAS_SUBJECT 1

/* What a change can affect, for the style set layout `ctx` used (each
 * answers true when that is not known):
 *   css_layout_attr_change_matters: could changing attribute `name` of an
 *     element from `old_v` to `new_v` (NULL: absent) change any element's
 *     computed style, or what layout reads of the element?  No when no
 *     selector names the attribute (or the class tokens or ids that
 *     changed) and layout does not read it.
 *   css_layout_uses_empty: does some selector use :empty?
 *   css_layout_uses_last_of: does some selector use :nth-last-child(An+B of
 *     S), so that an attribute change can restyle earlier siblings? */
/* The element an attribute change is on: its class and id attributes before
 * and after (NULL: absent), which decide what selectors it can match. */
typedef struct LayoutAttrOwner {
    const char *cls[2];
    const char *id[2];
} LayoutAttrOwner;
bool css_layout_attr_change_matters(const LayoutContext *ctx, const LayoutAttrOwner *owner, const char *name,
                                    const char *old_v, const char *new_v);
/* The same for the element's own computed style (a selector's rightmost
 * compound names the attribute, the class tokens or ids that changed, or
 * layout reads it): no means only other elements (descendants, later
 * siblings) may restyle. */
bool css_layout_attr_change_restyles_self(const LayoutContext *ctx, const LayoutAttrOwner *owner, const char *name,
                                          const char *old_v, const char *new_v);
bool css_layout_uses_empty(const LayoutContext *ctx);
bool css_layout_uses_last_of(const LayoutContext *ctx);
/* :has() and what a change can restyle through it (true when not known):
 *   css_layout_has_any: some rule uses :has();
 *   css_layout_has_sibling_relative: some :has() argument starts with + or ~
 *     (a change can restyle earlier siblings of it and of its ancestors).
 * Which elements a :has() rule may restyle is in LayoutContext.has_flags
 * and has_target_first. */
/* Would the element at layout index `idx` of the kept layout `ctx`, as the
 * document now is (its node at `dom_now` in ctx->doc), compute the style it
 * had in `ctx`?  Its cascade alone runs (on demand); false when not known
 * (a style pass ran since, or its parent changed). */
bool css_layout_style_unchanged(LayoutContext *ctx, int idx, int dom_now);
bool css_layout_has_any(const LayoutContext *ctx);
bool css_layout_has_sibling_relative(const LayoutContext *ctx);
/* How far replacing inline style `a` by `b` on an element can reach:
 *   0 nothing but painting;
 *   1 the element's offsets (left/top/right/bottom/inset, transform,
 *     translate): it and its subtree move, nothing else does (*sides gets
 *     the LAYOUT_SIDE_* bits of the insets that changed, and 0x10 for a
 *     transform);
 *   2 the element's own box and contents (sizes, margins, padding, borders,
 *     fonts, text, overflow, ...): for an out-of-flow box, its subtree only;
 *   3 further (display, position, float, custom properties, anything else,
 *     or a selector that tests the style attribute). */
int css_layout_inline_style_reach(const LayoutContext *ctx, const LayoutAttrOwner *owner, const char *a,
                                  const char *b, unsigned *sides);

/* A DOM text node's text as this layout sees it (generated content
 * included). */
const char *css_layout_node_text(const LayoutContext *ctx, const HtmlNode *node);

/* The text a text box lays out and paints (white space processed). */
const char *css_layout_box_text(const LayoutContext *ctx, int idx, const char *raw);

/* Build a flat layout tree from a parsed HTML document. */
bool css_layout_tree_build(LayoutContext *ctx, HtmlDocument *doc);

/* Free resources held by a layout tree. */
void css_layout_tree_free(LayoutContext *ctx);

/* Style and lay out the tree (parallel inside, synchronous to the caller). */
bool css_layout_document(LayoutContext *ctx, CssStylesheet *sheet);

/* Convenience: allocate/resolve/free in one call. */
bool css_layout_run(LayoutContext *ctx, HtmlDocument *doc, CssStylesheet *sheet,
                    double viewport_width, double viewport_height);
/* Layouts that kept the previous tree, and that built one (tests,
 * benchmarks). */
void css_layout_tree_stats(uint64_t *kept, uint64_t *built);

/* The declarations that apply to box idx's element, in cascade order (the
 * last one for a property wins): the normal stylesheet declarations, the
 * inline style, then the !important ones of each; var() resolved against
 * the element's custom properties (not reported).  Uses the style set of ctx's last layout; false when it is gone
 * or idx is not an element. */
typedef void (*CssCascadeFn)(void *ud, const char *prop, const char *value, bool important);
bool css_layout_element_cascade(LayoutContext *ctx, int idx, CssCascadeFn fn, void *ud);
/* The computed value of custom property name (--x) on box idx's element
 * from ctx's last layout, or NULL. */
const char *css_layout_element_custom(LayoutContext *ctx, int idx, const char *name);
/* Layouts on kept trees reuse clean subtrees from the last layout's calls:
 * forget them (an image or a font changed the measurements), and how many
 * subtrees (of how many boxes) were reused. */
void css_layout_retained_invalidate(void);
void css_layout_flow_stats(uint64_t *subtrees, uint64_t *boxes);

/* Test hook: elements whose computed style the last style pass took from
 * the incremental restyle cache, and those it recomputed (both 0 when the
 * pass did not use the cache). */
void css_layout_restyle_stats(unsigned *reused, unsigned *recomputed);
/* Test hook: of the recomputed elements, those that matched exactly as for
 * their cached style and took it (the matched-properties cache). */
unsigned css_layout_restyle_matched_same(void);

/* Test hook: parse a stylesheet with the chunked parallel parser (chunk =
 * bytes per chunk, 0 = default).  Equal to css_stylesheet_parse's result. */
CssStylesheet *css_layout_parse_sheet_parallel(const char *text, size_t len, size_t chunk);

/* Fetch the stylesheet at absolute `url` into the process-wide sheet cache
 * that layout reads <link> sheets from (no-op when cached).  True when the
 * sheet is available.  Engine thread. */
bool css_layout_fetch_stylesheet_url(const char *url);
/* Is the sheet at `url` (absolute) already fetched? */
bool css_layout_stylesheet_cached(const char *url);
/* Start downloading the sheet a <link rel=stylesheet href=...> of the page
 * names, in the background (dom_fetch's pool), so the first layout finds
 * it fetched instead of downloading every sheet in turn. */
void css_layout_prefetch_stylesheet(const char *href);
/* Take in the prefetched sheets whose downloads finished (without waiting),
 * which also starts the downloads of the web fonts they declare. */
void css_layout_install_prefetched_sheets(void);

/* Access the resolved LayoutBox for a DOM node index, or NULL if not found. */
LayoutBox* css_layout_box_for_node(LayoutContext *ctx, int dom_node_idx);

/* Layout benchmark (CYBER_LAYOUT_BENCH): while recording, each layout's
 * time per phase is kept; the report prints each phase's median and mean
 * over the layouts recorded (and resets). */
void css_layout_bench_record(bool on);
void css_layout_bench_report(const char *label);

#ifdef __cplusplus
}
#endif

#endif /* CSS_LAYOUT_H */

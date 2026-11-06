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

/* border-*-style values (LayoutStyle.border_style). */
typedef enum {
    CSS_BORDER_NONE, CSS_BORDER_HIDDEN, CSS_BORDER_SOLID, CSS_BORDER_DASHED, CSS_BORDER_DOTTED,
    CSS_BORDER_DOUBLE, CSS_BORDER_GROOVE, CSS_BORDER_RIDGE, CSS_BORDER_INSET, CSS_BORDER_OUTSET
} CssBorderStyle;

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
    /* A clip: rect() of no area hid the box (clip_vis*: what visibility was
     * before, for a later clip declaration that does not). */
    unsigned char clip_hid, clip_vis, clip_vis_set;
    double border_top, border_right, border_bottom, border_left;
    /* Per side, indexed as border_styled's bits (0 left, 1 right, 2 top,
     * 3 bottom): the color as packed RGBA (0xRRGGBBAA; bit k of
     * border_color_set: given, else currentColor) and the style
     * (CssBorderStyle). */
    uint32_t border_rgba[4];
    unsigned char border_color_set;
    unsigned char border_style[4];
    /* The border as the cascade gave it (used widths, styles, colors): in a
     * collapsing table the fields above are the resolved ones, written from
     * these by layout_collapse_borders each layout. */
    float border_spec_w[4];
    /* Computed border widths (border_styled bit order): the length even
     * where the side has no style (its used width is 0), which is what a
     * child's border-width: inherit takes. */
    float border_cw[4];
    uint32_t border_spec_rgba[4];
    unsigned char border_spec_style[4], border_spec_cset;
    unsigned char border_coll;   /* the fields above written by the collapse pass (the copy stands) */
    /* A collapsed table's cell along its outside: how far past its box a
     * side's border is painted (the table's share of that edge; the table
     * itself paints none).  Indexed as border_style. */
    float border_out[4];

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
    /* How text_align was given: 0 start (as initially), 1 left / right /
     * center (as it is), 2 end; start and end follow direction (1: rtl,
     * inherited). */
    unsigned char text_align_how, direction, direction_set;
    unsigned char text_justify;   /* text-align: justify (its last lines as start) */
    /* unicode-bidi (not inherited): 0 normal, 1 embed, 2 isolate,
     * 3 bidi-override, 4 isolate-override, 5 plaintext */
    unsigned char unicode_bidi;
    unsigned char table_layout_fixed;   /* table-layout: fixed */
    unsigned char table_group;          /* a row group: 0 body, 1 header, 2 footer */
    unsigned char flow_root;            /* display: flow-root (a block establishing a formatting context) */

    /* CSS float/clear.  float_side: 0=none, 1=left, 2=right.
     * clear: 0=none, 1=left, 2=right, 3=both. */
    unsigned char float_side;
    unsigned char clear;

    /* overflow/overflow-x/overflow-y: 0=visible, 1=hidden, 2=auto, 3=scroll.
     * Any value other than visible makes the box establish a new block
     * formatting context (CSS 2.1 §9.4.1), which blocks margin collapsing
     * with in-flow children (§8.3.1) and stops empty-box through-collapse. */
    unsigned char overflow;
    /* overflow: clip (overflow is 1), and how far out it clips. */
    unsigned char overflow_clip;
    /* What makes it a containing block for absolute and fixed descendants without
     * being positioned: a bit per property (contain, transform, filter...). */
    unsigned short abs_cb;
    unsigned char height_stretch;   /* height: stretch */
    unsigned char minmax_stretch;   /* min/max width/height: stretch (bits as minmax_pct_bits) */
    /* writing-mode (0 horizontal-tb, 1 vertical-rl, 2 vertical-lr, 3 sideways-rl, 4 sideways-lr). */
    unsigned char writing_mode, writing_mode_set;
    unsigned char wm_swapped;       /* now in the axes of a vertical flow being laid out */
    unsigned char wm_flipped;       /* and of one going right to left */
    /* line-break: 0 auto, 1 loose, 2 normal, 3 strict, 4 anywhere; word-break:
     * 0 normal, 1 break-all, 2 keep-all (both inherited). */
    unsigned char line_break, word_break, line_break_set, word_break_set;
    /* Anchor positioning: anchor-name, position-anchor and position-area as
     * written; insets and sizes that use anchor() or anchor-size() (top, right,
     * bottom, left, width, height); self-alignment anchor-center (1 justify, 2 align). */
    const char *anchor_name, *position_anchor, *position_area;
    const char *try_list;           /* position-try-fallbacks, as written */
    const char *js_text, *as_text;  /* justify-self and align-self, as written */
    unsigned char try_pa[4], try_pa_n;  /* flips position-area goes through: 0 x, 1 y, 2 across */
    unsigned char try_order;        /* position-try-order: 0 normal, 1 most-width, 2 most-height, 3 block, 4 inline */
    const char *anchor_expr[6];
    unsigned char self_anchor;
    unsigned char anchor_fail;      /* of those insets, the ones with no value: bit k */
    /* zoom: its own (0: none), and what the box's lengths were last scaled by (0: 1). */
    float zoom_own, zoom;
    /* overflow-anchor: none. */
    unsigned char overflow_anchor_none;
    /* scroll-marker-group: 1 before, 2 after. */
    unsigned char scroll_marker_group;
    float overflow_clip_margin;

    /* vertical-align: 0=baseline, 1=middle, 2=top, 3=bottom, 4=sub,
     * 5=super, 6=text-top, 7=text-bottom, 8=a length (va_len px) or
     * percentage (va_pct of the line-height), raising the box.  Table cells
     * (td/th) default to middle per the HTML UA stylesheet; for a cell's
     * content only middle/top/bottom are honoured at the moment. */
    unsigned char vertical_align;
    float va_len, va_pct;

    /* Resolved typography. */
    double font_size;
    /* First font-family name, interned (lives for the process; "" unset). */
    const char *font_family;
    /* Font weight 100..900 (400 = normal, 700 = bold) and italic flag. */
    int    font_weight;
    unsigned char font_italic;
    unsigned char font_is_oblique;   /* font_italic from an oblique, of this angle */
    float font_oblique_deg;
    float font_stretch;              /* percent */
    unsigned char font_stretch_set;
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
    /* An em or percentage line-height of this element (> 0: that many of
     * its own final font-size, computed then; it inherits as pixels). */
    double line_height_em;

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
    /* CSS_FEAT_* of the declarations that applied (css_note_property). */
    unsigned char render_features;

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
    /* list-style-image (inherited): its absolute URL, "" none */
    const char *list_image;
    unsigned char list_image_set;
    /* letter-spacing / word-spacing in px (0: normal) and text-transform
     * (0 none, 1 uppercase, 2 lowercase, 3 capitalize); inherited. */
    float letter_spacing, word_spacing;
    unsigned char letter_spacing_set, word_spacing_set;
    unsigned char text_transform, text_transform_set;
    /* text-indent: px plus a fraction of the containing block's width;
     * inherited. */
    float text_indent, text_indent_pct;
    unsigned char text_indent_set;
    /* counter-reset / -increment / -set as written (interned; NULL: none)
     * and quotes (inherited when not quotes_set; NULL: the UA's). */
    const char *counter_reset, *counter_increment, *counter_set;
    const char *quotes;
    unsigned char quotes_set;

    /* Background image URL, interned (lives for the process; "" none). */
    const char *background_image_url;
    const char *shape_image_url;    /* shape-outside: url() */
    /* Its placement: no-repeat per axis (repeat otherwise); position as a
     * fraction of the free room plus a length per axis; size kind 0 auto,
     * 1 cover, 2 contain, 3 given (bg_size_w/h: px, or a fraction of the
     * area when bg_size_pct has bit 0 / 1; < 0 auto); attached to the
     * viewport (fixed). */
    unsigned char bg_no_repeat_x, bg_no_repeat_y;
    double bg_pos_xf, bg_pos_xpx, bg_pos_yf, bg_pos_ypx;
    unsigned char bg_size_kind, bg_size_pct, bg_fixed;
    double bg_size_w, bg_size_h;

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
    CssJustifyContent align_content;
    /* not normal or stretch, so nothing stretches to fill: 1 justify-content, 2 align-content */
    unsigned char content_set;
    /* (calc-size() on a keyword: the expression over what the keyword comes to) */
    const char *csz_w, *csz_h;
    /* (content keywords: min-width's and max-width's, 1 min 2 max 3 fit; on height the
     * content is what it comes to) */
    unsigned char mm_kw_w[2], h_kw, mm_kw_h;
    /* (contain: size and inline-size: 1 inline, 2 block; and the sizes it then has) */
    unsigned char contain_size;
/* (size containment from contain, content-visibility, or container-type: 1 inline axis, 2 block axis) */
#define CSS_CONTAIN_SIZE_OF(s) (((s)->table_role >= 2 && (s)->table_role != 5) ? 0 :     ((s)->contain_size | ((s)->container_type == 2 ? 3 : (s)->container_type == 1 ? 1 : 0)))
    float cis_w, cis_h;     /* contain-intrinsic-width and -height, px; 0 none */
    CssAlignItems align_items;

    double flex_basis;
    double flex_basis_percent; /* >0 when flex-basis was a percentage */
    unsigned char fb_kw;       /* by content: 1 min-content, 2 max-content, 3 fit-content, 4 content */
    const char *fb_csz;        /* with calc-size(): what it comes to, of size */
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
    double font_size_add;   /* (px, with the ratio: calc(1em + 2px)) */

    /* Outer display is inline but the box is laid out as an atomic inline
     * (inline-flex / inline-grid / inline-table; inline-block uses
     * CSS_DISPLAY_INLINE_BLOCK).  Cleared by blockification. */
    unsigned char inline_level;
    /* Absolutely positioned (or fixed) and inline-level before
     * blockification: its static position is where it would be on its line
     * (CSS 2.1 section 10.3.7), not below it. */
    unsigned char abs_inline;
    /* Block-in-inline, set by blockification: 2 for an inline element
     * with block-level children, split around them (its parent's flow lays
     * out the inline content between them as pieces of it, the blocks as
     * its own children: LayoutContext.pieces); 1 for one laid out as a
     * block instead (one holding such an inline), where percentages
     * resolve against the block around it. */
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
    /* container-type (0 normal, 1 inline-size, 2 size) and container-name */
    unsigned char container_type;
    const char *container_name;
    /* transform: translation part only (px plus fraction of own size). */
    double translate_x, translate_y;
    double translate_xpct, translate_ypct;
    unsigned char has_translate;
    /* (more than a translation: as written; the box itself stays where it's laid out) */
    const char *tf_text, *tf_origin, *tf_rotate, *tf_scale;
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
    /* transition-* and animation-* (css_anim.h's specification; interned,
     * NULL: all initial). */
    const char *anim_spec;
    /* z-index (when z_index_set; auto otherwise). */
    int z_index;
    unsigned char z_index_set;
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
    /* The alignment keywords the enums above fold together (LAYOUT_ALIGN_KW_*,
     * with 0x80: safe), for what they mean in each kind of container. */
    unsigned char jc_kw, ac_kw, ai_kw, as_kw, ji_kw, js_kw;
    /* Table part (from the tag or display: table-*): 0 none, 1 table,
     * 2 row group, 3 row, 4 cell, 5 caption, 6 column (ignored).
     * border-spacing (when border_spacing_set; else the cellspacing
     * attribute or 2px) and border-collapse: collapse (no spacing). */
    unsigned char table_role;
    unsigned char border_spacing_set, border_collapse, border_collapse_set;   /* (border-collapse: inherited) */
    unsigned char empty_cells_hide, empty_cells_set;   /* (empty-cells: inherited) */
    double border_spacing_h, border_spacing_v;
    /* caption-side as given: 0 not (inherited: css_layout_caption_bottom), 1 top, 2 bottom */
    unsigned char caption_side;
} LayoutStyle;

struct LayoutContext;
/* Does caption box idx go below its table (caption-side, inherited)? */
bool css_layout_caption_bottom(const struct LayoutContext *ctx, int idx);
/* A list item's marker baseline (its first line's, else its strut's). */
double css_layout_marker_baseline(struct LayoutContext *ctx, int idx);

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

    double min_width, max_width;
    double min_height, max_height;
    double top, left, right, bottom;

    uint32_t flags;
    /* Set by layout_resolve_used_sizes when a percentage height could not
     * resolve (indefinite containing block): the used height is auto. */
    unsigned char height_pct_unresolved;
    /* An absolutely positioned box's static position, from the line it is
     * on (inline layout on items), relative to its parent's box; used and
     * cleared by the absolute positioning pass. */
    unsigned char has_static;
    double static_dx, static_dy;
    double opacity_eff;
} LayoutBox;

/* A piece of an inline split around its block-level children
 * (block_in_inline 2): its border box, placed relative to its first child
 * (anchor; -1: to the inline's own box) so it moves with it, and which of
 * the inline's left (1) and right (2) edges it has. */
typedef struct LayoutPiece {
    int anchor;
    double dx, dy, w, h;
    unsigned char sides;
} LayoutPiece;

typedef struct LayoutPieceList {
    LayoutPiece *p;
    int n, cap;
} LayoutPieceList;

/* One line's part of a text box in a paragraph that needs bidi
 * (BIDI_DESIGN.md): the bytes [start, end) of the text it paints
 * (css_layout_text_paint), drawn from dx with its em box's top at dy (both
 * from the box's x and y, so they move with it), w wide; level its
 * embedding level (odd: right to left).  Painted instead of the wrap the
 * box's geometry gives. */
typedef struct LayoutTextFrag {
    uint32_t start, end;
    double dx, dy, w;
    unsigned char level;
    unsigned char ellipsis;   /* an ellipsis after it (line-clamp, text-overflow) */
    unsigned char fl;         /* on a ::first-line whose color it takes (fl_rgba) */
    uint32_t fl_rgba;
} LayoutTextFrag;

typedef struct LayoutTextFragList {
    LayoutTextFrag *f;
    int n;
} LayoutTextFragList;

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
    double left, right, top, bottom;   /* (margin box) */
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

    /* Per box, the pieces of a split inline (block_in_inline 2); NULL when
     * the tree has none. */
    LayoutPieceList *pieces;
    int pieces_n;

    /* Per box, a text box's fragments when its paragraph needs bidi
     * (n 0: painted from its geometry); NULL until one does. */
    LayoutTextFragList *text_frags;
    int text_frags_n;

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

    /* Pseudo-element boxes (::before / ::after): after the style pass, the
     * boxes its elements' styles call for are added after the document's
     * nodes (base_count of them), each a pseudo element node with one text
     * node, linked in as its element's first or last child.  Their document
     * nodes are the layout's own (pseudo_nodes), at document indices from
     * LAYOUT_PSEUDO_DOM0 on.  pseudo_rec: what each element's style said
     * (const struct PseudoRec *, per document-built node, this pass). */
    int base_count;
    struct HtmlNode *pseudo_nodes;
    int pseudo_nodes_n;
    const void **pseudo_rec;
    /* What each pseudo box (pair k: boxes base + 2k and its text) was made
     * from; and, between a kept tree's layouts, the boxes it had (parked:
     * their slots and nodes, by element and side) for the next to take back
     * when nothing they depend on changed (style set parked_gen). */
    struct LayoutPseudoPair *pseudo_pairs;
    struct LayoutParkedPseudo *parked;
    int parked_n;
    uint64_t parked_gen;
    /* Anonymous table boxes (CSS 2.1 section 17.2.1): added after the
     * pseudo boxes, around the children they wrap; their document nodes
     * are the layout's own (anon_nodes, at LAYOUT_ANON_DOM0 on).  Dropped
     * with the pseudo boxes. */
    struct HtmlNode *anon_nodes;
    int anon_nodes_n;
    /* The ::first-line color a block's first line takes from an ancestor
     * whose first line it is (fl_in set: fl_in_rgba), from the flow laying
     * it out to its own (layout_block_flow_inner). */
    unsigned char fl_in;
    uint32_t fl_in_rgba;
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

/* The document node of layout box idx: one of the document's, or (a pseudo
 * element's box and its text) one of the layout's own; NULL for none. */
#define LAYOUT_PSEUDO_DOM0 0x40000000
#define LAYOUT_ANON_DOM0 0x50000000
struct HtmlNode *css_layout_box_node(const LayoutContext *ctx, int idx);

/* Scrolling.  Right / bottom extent of box idx's rendered subtree (page
 * coordinates; they grow only); whether it is a scroll container (html and
 * body are the viewport's); its offset, clamped to that extent, when it is
 * scrolled; and what its scrolled ancestors move it by (fixed: it is on the
 * viewport, whatever that has scrolled). */
void css_layout_extent(const LayoutContext *ctx, int idx, double *right, double *bottom);
void css_layout_extent_start(const LayoutContext *ctx, int idx, double *left, double *top);
/* The elements of the documents laid out whose style has a CSS_FEAT_* bit, in
 * tree order (each document's as of its last layout), and how many layouts
 * there have been. */
void css_layout_render_list(const uint32_t **els, const unsigned char **bits, uint32_t *n);
extern uint64_t g_layout_runs;
bool css_layout_box_scrolls(const LayoutContext *ctx, int idx);
bool css_layout_box_clips(const LayoutContext *ctx, int idx);
/* A pseudo-element's declarations (ps: 1 ::before, 2 ::after, 6 ::scroll-marker,
 * 7 its current ones, 8 ::scroll-marker-group), in cascade order; their count. */
int css_layout_pseudo_decls(const LayoutContext *ctx, int idx, int ps, const CssDeclaration *const **decls);
/* The scroll pseudo boxes (::scroll-marker and -group, ::scroll-button): how
 * many pair slots to look at, and for one its box, owner box and side. */
int css_layout_pseudo_pair_count(const LayoutContext *ctx);
bool css_layout_scroll_pseudo(const LayoutContext *ctx, int pair, int *box, int *owner, int *side);
/* box is such a pseudo box's: its owner box, else -1. */
int css_layout_scroll_pseudo_owner(const LayoutContext *ctx, int box);
extern volatile int g_css_scroll_pseudos;
/* Some style has said zoom. */
extern volatile int g_layout_zoom_any;
void css_layout_box_max(const LayoutContext *ctx, int idx, double *x, double *y);
bool css_layout_box_offset(const LayoutContext *ctx, int idx, float *x, float *y);
void css_layout_box_scrolled(const LayoutContext *ctx, int idx, double *dx, double *dy, bool *fixed);
/* What idx's transforms and its ancestors' (up to, not with, stop) make of points
 * in laid-out coordinates: an affine m[6] (a b c d e f); false when they make nothing of them. */
bool css_layout_box_map(const LayoutContext *ctx, int idx, int stop, double m[6]);
/* A container query (a @container prelude) for the DOM node dom; self: dom may be the container. */
bool css_layout_container_match(const LayoutContext *ctx, int dom, bool self, const char *query);
/* A rect's bounds under m. */
void css_layout_map_bounds(const double m[6], double *x, double *y, double *w, double *h);

/* Box idx's values as the transitions store's before-change style. */
void css_layout_anim_seed(const LayoutContext *ctx, int idx, uint32_t el);

/* The rectangles of the bytes [a, b) of text box idx's data, one per line. */
int css_layout_text_rects(const LayoutContext *ctx, int idx, size_t a, size_t b, double (*out)[4], int max);

/* The text a text box lays out and paints (white space processed). */
const char *css_layout_box_text(const LayoutContext *ctx, int idx, const char *raw);

struct TextShaper;

/* What text box idx is painted with: its font, scale (font-size / 16),
 * line advance, and text (white space processed, a leading space layout
 * dropped left out).  False when it has no text. */
bool css_layout_text_paint(const LayoutContext *ctx, int idx, struct TextShaper **font, float *scale,
                           float *line_adv, const char **text);

/* Build a flat layout tree from a parsed HTML document. */
bool css_layout_tree_build(LayoutContext *ctx, HtmlDocument *doc);
/* A new document in this process: drop what is cached per element. */
void css_layout_forget_document(void);

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
bool css_layout_highlight_cascade(LayoutContext *ctx, int dom, const char *pseudo, CssCascadeFn fn, void *ud);
bool css_layout_element_cascade(LayoutContext *ctx, int idx, CssCascadeFn fn, void *ud);
/* The computed value of custom property name (--x) on box idx's element
 * from ctx's last layout, or NULL. */
const char *css_layout_element_custom(LayoutContext *ctx, int idx, const char *name);
/* The same for an element of the mirror document (dom index) that has no
 * styled box (inside a display:none subtree): var() resolves against the
 * custom properties of box holder (its nearest styled ancestor). */
bool css_layout_dom_cascade(LayoutContext *ctx, int dom, int holder, CssCascadeFn fn, void *ud);
/* The user agent's display of an element with tag (lower case). */
const char *css_layout_tag_display(const char *tag);
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
/* Whether a prefetched stylesheet is still downloading. */
bool css_layout_stylesheets_pending(void);
/* Take in the prefetched sheets whose downloads finished (without waiting),
 * which also starts the downloads of the web fonts they declare. */
void css_layout_install_prefetched_sheets(void);

/* Box idx's client rects (CSSOM View getClientRects), up to max of them
 * into out as x, y, width, height: an inline box one per line it is on (its
 * pieces, else its text's fragments grouped by line, its start edge on the
 * first and its end edge on the last), any other box its border box.  The
 * number of them. */
int css_layout_client_rects(const LayoutContext *ctx, int idx, double (*out)[4], int max);

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

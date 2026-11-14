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

/* (a corner's vertical radius in px, of a border box h high: its own, else its horizontal one) */
#define LAYOUT_RADIUS_Y(s, c, h) fmax(0.0, ((s)->radius_v_set >> (c) & 1) ? (((s)->radius_vpct >> (c) & 1) ? (s)->radius_v[c] * (h) + (s)->radius_vadd[c] : (s)->radius_v[c]) \
                                  : (((s)->radius_pct >> (c) & 1) ? (s)->radius[c] * (h) + (s)->radius_add[c] : (s)->radius[c]))
/* (a corner's horizontal radius in px, of a border box w wide) */
#define LAYOUT_RADIUS_X(s, c, w) fmax(0.0, ((s)->radius_pct >> (c) & 1) ? (s)->radius[c] * (w) + (s)->radius_add[c] : (s)->radius[c])

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
    unsigned char is_th;
    unsigned char is_fieldset;      /* (a fieldset: no internal table display of its own) */
    unsigned char ovf_visible_given;
    unsigned char ovf_moved;
    unsigned char ovf_from_body;   /* (a root's overflow that is the body's, moved: undone before moving again) */
    const char *color_cc_expr;   /* (color with currentColor in a function: worked out with the inherited color) */   /* (a body's overflow moved to the viewport: the value + 1, its clip bit 8) */   /* (overflow: visible said: a canvas drawing past its box shown) */
    /* (position-visibility: bit 0 always, 1 anchors-visible, 2 no-overflow; 0 as initially, anchors-visible;
     * pv_anchor its anchor box + 1 (0 none); pv_hidden: it overflowed, no-overflow hides it) */
    unsigned char pos_vis, pv_hidden;
    int pv_anchor;
    int anc_used;   /* (an anchor it was placed against + 1: it moves with that anchor's scrolling) */
    const char *bg_cc_expr;   /* (background-color with currentColor inside a function: worked out with the box's color) */   /* (an HTML th: centred while the text-align it inherits is start) */
    /* How text_align was given: 0 start (as initially), 1 left / right /
     * center (as it is), 2 end; start and end follow direction (1: rtl,
     * inherited). */
    unsigned char text_align_how, direction, direction_set;
    unsigned char fl_brk;   /* (a ::first-line box: the line ends after it) */
    unsigned char ta_match;   /* (text-align: match-parent: the parent's, start and end as the parent's direction has them) */
    /* text-align-last: 0 auto, 1 start, 2 end, 3 left, 4 right, 5 center, 6 justify (inherited) */
    unsigned char tal, tal_set;
    unsigned char tga;   /* (text-group-align: 0 none, 1 start, 2 end, 3 left, 4 right, 5 center) */
    unsigned char tj_none, tj_set;   /* text-justify: none (inherited) */
    unsigned char tj_char;           /* (text-justify: 1 inter-character or distribute, 2 inter-word; inherited) */
    /* offset-path ray(): op_kind 1, its angle (deg), size (0 closest-side,
     * 1 closest-corner, 2 farthest-side, 3 farthest-corner, 4 sides) and
     * contain; offset-distance (px, or a fraction of the path when
     * op_dist_pct); offset-rotate (op_rot_auto: the path's direction plus
     * op_rot degrees, else op_rot) */
    unsigned char op_kind, op_size, op_contain, op_dist_pct, op_rot_auto;
    float op_angle, op_dist, op_rot, op_dist_px;   /* (op_dist_px: a length beside a percentage) */
    const char *op_path;             /* (op_kind 2: path()'s SVG path data; 3 a basic shape; 4 url()'s id) */
    unsigned char op_anchor;         /* (offset-anchor given: op_a* its place in the box) */
    unsigned char op_at, op_box;     /* (ray(): its "at" position given, in op_r*; the path's box: 0 border, 1 padding, 2 content) */
    float op_rxp, op_rxl, op_ryp, op_ryl;
    unsigned char op_posk;           /* (offset-position: 0 normal (the containing block's centre), 1 auto, 2 op_p*) */
    float op_pxp, op_pxl, op_pyp, op_pyl;
    float op_axp, op_axl, op_ayp, op_ayl;   /* (its x and y: a fraction of the size and a length) */
    /* filter: up to 8 functions in order (filt_t: 1 grayscale, 2 sepia,
     * 3 saturate, 4 hue-rotate (deg), 5 invert, 6 opacity, 7 brightness,
     * 8 contrast, 9 blur (px), 10 drop-shadow (one: its offsets, blur,
     * color in filt_ds)) */
    unsigned char filt_url;   /* (filter has a url(): the whole list goes through svg_filter.c) */
    unsigned char filt_n, filt_t[8];
    const char *filt_text;          /* (the filter as written, for animations; NULL none) */
    unsigned char bf_n, bf_t[8];    /* backdrop-filter: as filter's (no drop-shadow) */
    const char *bf_text;            /* (as written, for animations) */
    float bf_v[8];
    float filt_v[8], filt_ds[7];
    unsigned char blend;             /* mix-blend-mode: 0 normal, 1 multiply .. 15 luminosity (CSS order), 16 plus-lighter */
    float il_size;                   /* initial-letter: its size in lines (0 normal) and sink */
    int il_sink;
    unsigned char text_justify;   /* text-align: justify (its last lines as start) */
    /* unicode-bidi (not inherited): 0 normal, 1 embed, 2 isolate,
     * 3 bidi-override, 4 isolate-override, 5 plaintext */
    unsigned char unicode_bidi;
    unsigned char table_layout_fixed;   /* table-layout: fixed */
    unsigned char table_group;          /* a row group: 0 body, 1 header, 2 footer */
    unsigned char flow_root;            /* display: flow-root (a block establishing a formatting context) */
    unsigned char flow_orth;            /* (flow_root only for crossing its parent's flow) */
    unsigned char custom_layout;        /* display: layout(): no multi-column layout */
    unsigned char clip_on, clip_auto;   /* (clip: rect(): on; its auto sides, bits top right bottom left) */
    float clip_v[4];                    /* (its top, right, bottom, left offsets from the border box's corner) */

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
    unsigned char ovf_clip_axes;   /* (overflow clip on its own: bit 1 the x axis, 2 the y; 0 both) */
    /* What makes it a containing block for absolute and fixed descendants without
     * being positioned: a bit per property (contain, transform, filter...). */
    unsigned short abs_cb;         /* (1024: will-change: position, for absolute descendants only) */
    unsigned char wc_sc;           /* (will-change names what makes a stacking context: 1 always, 2 z-index: when positioned or a flex / grid item) */
    unsigned char height_stretch;   /* height: stretch */
    unsigned char stretch_drop;     /* (margins a block-axis stretch leaves on: 1 top, 2 right, 4 bottom, 8 left) */
    unsigned char minmax_content;   /* (min/max width/height given as min-content, max-content or fit-content: bits as minmax_stretch) */
    unsigned char minmax_stretch;   /* min/max width/height: stretch (bits as minmax_pct_bits) */
    /* writing-mode (0 horizontal-tb, 1 vertical-rl, 2 vertical-lr, 3 sideways-rl, 4 sideways-lr). */
    unsigned char writing_mode, writing_mode_set;
    unsigned char wm_own, dir_own, wm_own_set;   /* (a table's row, row group or column: its computed ones, what its children inherit; it flows as the table) */
    unsigned char wm_swapped;       /* now in the axes of a vertical flow being laid out */
    unsigned char wm_flipped;       /* and of one going right to left */
    /* line-break: 0 auto, 1 loose, 2 normal, 3 strict, 4 anywhere; word-break:
     * 0 normal, 1 break-all, 2 keep-all (both inherited). */
    unsigned char line_break, word_break, line_break_set, word_break_set;
    /* Anchor positioning: anchor-name, position-anchor and position-area as
     * written; insets and sizes that use anchor() or anchor-size() (top, right,
     * bottom, left, width, height); self-alignment anchor-center (1 justify, 2 align). */
    const char *anchor_name, *position_anchor, *position_area;
    const char *anchor_scope;          /* anchor-scope: "all" or the names its subtree keeps to itself (NULL none) */
    const char *try_list;           /* position-try-fallbacks, as written */
    const char *js_text, *as_text;  /* justify-self and align-self, as written */
    unsigned char try_pa[4], try_pa_n;  /* flips position-area goes through: 0 x, 1 y, 2 across */
    unsigned char try_order;        /* position-try-order: 0 normal, 1 most-width, 2 most-height, 3 block, 4 inline */
    const char *anchor_expr[14];    /* insets (top right bottom left), width, height, margins (6-9, the same order), min-width, min-height, max-width, max-height */
    unsigned char self_anchor;
    unsigned char anchor_fail;      /* of those insets, the ones with no value: bit k */
    /* zoom: its own (0: none), and what the box's lengths were last scaled by (0: 1). */
    float zoom_own, zoom;
    /* overflow-anchor: none. */
    unsigned char overflow_anchor_none;
    /* scroll-marker-group: 1 before, 2 after. */
    unsigned char scroll_marker_group;
    float overflow_clip_margin;
    unsigned char ocm_inh;   /* (overflow-clip-margin: inherit) */
    unsigned char ocm_box;   /* (its visual box: 0 padding-box, 1 border-box, 2 content-box) */
    unsigned char to_inh;    /* (text-overflow: inherit) */

    /* vertical-align: 0=baseline, 1=middle, 2=top, 3=bottom, 4=sub,
     * 5=super, 6=text-top, 7=text-bottom, 8=a length (va_len px) or
     * percentage (va_pct of the line-height), raising the box.  Table cells
     * (td/th) default to middle per the HTML UA stylesheet; for a cell's
     * content only middle/top/bottom are honoured at the moment. */
    unsigned char vertical_align;
    float va_len, va_pct;
    float il_push;           /* (a raised initial letter: its block's content pushed down by this, as top padding) */
    float il_push_l, il_push_r;   /* (the same across a vertical block: left or right padding) */
    float bs_len, bs_pct;     /* (baseline-shift: a length and a percentage of the line-height, raising it) */
    unsigned char bs_kw;      /* (baseline-shift sub 4, super 5; 0 a length) */

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
    unsigned char fw_rel;           /* (font-weight: 1 bolder, 2 lighter, of the parent's) */
    unsigned char font_italic_set;
    /* text-decoration bitmask: 1 = underline, 2 = line-through, 4 = overline. */
    unsigned char text_decoration;
    unsigned char text_decoration_set;
    /* (text-decoration's other longhands, the decorating box's: color
     * (td_cset 0: currentcolor), style 0 solid 1 double 2 dotted 3 dashed
     * 4 wavy, thickness td_tk 0 auto 1 from-font 2 px 3 em (td_t), inset at
     * the start and end (td_in_auto: auto)) */
    uint32_t td_rgba;
    unsigned char td_cset, td_style, td_tk, td_in_auto, td_in_pct;   /* (td_in_pct: bit k, td_in[k] a percentage) */
    float td_in_cpct[2];   /* (a calc() inset's percentage part, a fraction) */
    float td_t, td_te, td_in[2];   /* (thickness: td_t px + td_te em) */
    /* (text-underline-offset, inherited: ul_k 0 auto 2 px 3 em (ul_v);
     * text-underline-position: under) */
    unsigned char ul_k, ul_set, ul_under, ul_pos_set, ul_right;   /* (ul_right: text-underline-position: right, in vertical text the over side) */
    float ul_v, ul_e;              /* (offset: ul_v px + ul_e em) */
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
    /* list-style-type when not one of those (list_style_type 5): a counter
     * style's name, a symbols() function or a "string"; inherited */
    const char *list_style;
    unsigned char list_inside, list_inside_set;   /* list-style-position: inside */
    /* letter-spacing / word-spacing in px (0: normal) and text-transform
     * (0 none, 1 uppercase, 2 lowercase, 3 capitalize); inherited. */
    float letter_spacing, word_spacing;
    /* (letter-spacing as a percentage of the font size: inherited as one) */
    float ls_pct;
    float ws_pct, ws_px;            /* (word-spacing: a fraction of the space's width, and a length, when ws_pct_set) */
    unsigned char ws_pct_set;
    unsigned char ls_pct_set;
    /* (tab-size, inherited: spaces, or px when tab_len; tab_set when given) */
    float tab_size;
    unsigned char tab_len, tab_set;
    /* (text-decoration-skip-spaces, inherited: bit 1 start, 2 end, 4 all; tds_set when given, else start end) */
    unsigned char tds_bits, tds_set;
    /* (image-orientation: none, inherited; img_orient_set when given) */
    unsigned char img_orient_none, img_orient_set;
    unsigned char img_anim, img_anim_set;   /* (image-animation: 0 normal, 1 running, 2 paused, 3 stopped) */
    /* (font-size-adjust, inherited: the aspect value (0 none) and its metric,
     * 0 ex-height, 1 cap-height, 2 ch-width, 3 ic; fsa_set when given;
     * fs_unadj the font size before the adjustment, 0 not yet made) */
    float fsa;
    unsigned char fsa_metric, fsa_set;
    double fs_unadj;
    unsigned char fs_vunit;   /* (font-size in vi 1 or vb 2: fs_vval of them, by its own writing mode) */
    float fs_vval;
    /* (font-variant-* and font-feature-settings, inherited, as written:
     * position, caps, numeric, east-asian, alternates, ligatures, settings;
     * fv_set's bit k when given here; ot_tags the OpenType features they ask for) */
    const char *fv[8];   /* (and [7] font-kerning: normal, none, NULL auto) */
    unsigned char fv_set;
    /* (text-decoration-skip-ink, inherited: 0 auto, 1 none, 2 all; tdsi_set when given) */
    unsigned char tdsi, tdsi_set;
    unsigned char ruby_ovh_none, ruby_ovh_set, ruby_ovh_sp;   /* (ruby_ovh_sp: spaces, over the spaces beside only) */
    unsigned char ruby_ovh_pad_;   /* (ruby-overhang: none, inherited; auto lets an annotation over adjacent text) */
    /* (text-shadow, inherited: up to four, first on top; tsh_cur's bit k: its
     * color currentColor; tsh_set when given) */
    unsigned char tsh_n, tsh_cur, tsh_set;
    float tsh_x[4], tsh_y[4], tsh_b[4];
    uint32_t tsh_rgba[4];
    uint32_t ot_tags[16];
    unsigned char ot_n;
    unsigned char letter_spacing_set, word_spacing_set;
    unsigned char text_transform, text_transform_set;
    unsigned char wst, wst_set;   /* (word-space-transform: 1 space, 2 ideographic-space) */
    /* text-indent: px plus a fraction of the containing block's width;
     * inherited. */
    float text_indent, text_indent_pct;
    unsigned char text_indent_set;
    unsigned char cs_dark;   /* (color-scheme, inherited: the dark one used) */
    unsigned char ti_hang;   /* (text-indent: 1 hanging: the lines after the first indented, not it; 2 each-line: after a forced break as the first) */
    unsigned char trim_off, trim_set;   /* (text-spacing-trim: space-all, inherited) */
    unsigned char autospace, autospace_set;   /* (text-autospace, inherited: 1 ideograph-alpha, 2 ideograph-numeric) */
    unsigned char hang, hang_set;
    /* (outline: its style (a CssBorderStyle, 0 none; auto as none), width
     * (px, -1: medium), offset, color (outline_cset 0: currentcolor)) */
    unsigned char outline_style, outline_cset, outline_wset;
    unsigned char outline_auto;     /* (outline-style: auto, drawn solid) */
    unsigned char outline_inh;      /* (inherit asked of: 1 style, 2 width, 4 color, 8 offset; 16 offset: inset) */
    float outline_w, outline_off;
    uint32_t outline_rgba;
    unsigned char bg_current;       /* (background-color: currentcolor, the element's color once known) */
    unsigned char owrap, owrap_set; /* (overflow-wrap, inherited: 1 break-word, 2 anywhere) */
    const char *hyph_char;          /* (hyphenate-character, inherited: the string, NULL auto) */
    unsigned char hyph_char_set;
    unsigned char frame_sizing;     /* (frame-sizing: 1 content-height, 2 content-width) */
    unsigned char text_fit, text_fit_per;   /* (text-fit: 1 grow, 2 shrink; per: 0 consistent, 1 per-line, 2 per-line-all) */
    float text_fit_max;             /* (its <percentage>: the most it scales, 0 no limit) */
    const char *font_palette;       /* (font-palette, inherited: the text, NULL normal) */
    unsigned char font_palette_set;
    unsigned char tcy;    /* (text-combine-upright: all, in a vertical flow: a horizontal cell 1em square) */
    unsigned char tw_balance, tw_balance_set;   /* (text-wrap-style: balance, inherited) */
    float mm_fc_px[2], mm_fc_pct[2];   /* (min-/max-width: fit-content(L)) */
    unsigned char mm_fc_set;           /* (bit 0 min, bit 1 max) */
    unsigned char hyphens, hyphens_set;   /* (0 manual, 1 none, 2 auto; inherited) */
    signed char hyl_word, hyl_before, hyl_after, hyl_set;   /* (hyphenate-limit-chars, inherited: -1 auto) */
    unsigned char orphans, widows, ow_set;   /* (inherited; 0: the initial 2; ow_set bit 1 orphans, 2 widows) */
    unsigned char text_orient, text_orient_set;   /* (text-orientation, inherited: 0 mixed, 1 upright, 2 sideways) */
    unsigned char disp_contents;    /* (display: contents: laid out as an inline with no box of its own) */
    unsigned char run_in;           /* (display: run-in; run_in_merge: it goes into the next block's first line) */
    unsigned char run_in_merge;
    unsigned char ruby_int;         /* (display: ruby-base, ruby-text and their containers) */
    unsigned char contents_pd;      /* (of such a box: the display of the box its children are in, a CssDisplay) */   /* (hanging-punctuation, inherited: 1 first, 2 last, 4 force-end, 8 allow-end) */
    float contents_bg[4];   /* (display: contents: the background color it computed, which it does not paint, for inherit) */
    /* counter-reset / -increment / -set as written (interned; NULL: none)
     * and quotes (inherited when not quotes_set; NULL: the UA's). */
    const char *counter_reset, *counter_increment, *counter_set;
    const char *quotes;
    unsigned char quotes_set;

    /* Background image URL, interned (lives for the process; "" none). */
    const char *background_image_url;
    float bg_res;                 /* (image-set(): the chosen image's resolution, its dppx; 0 as 1) */
    float content_res;            /* (content: image-set(): the same for the image replacing the element) */
    const char *shape_image_url;    /* shape-outside: url() */
    float shape_threshold;          /* shape-image-threshold */
    const char *shape_grad;         /* shape-outside: a gradient's text (NULL none) */
    const char *bg_grad;            /* background-image: the first layer's gradient (NULL none) */
    /* (the background properties as written, interned: the shorthand, then image, position,
     * size, repeat, origin, clip, attachment set after it; bg_nl: how many layers) */
    const char *bg_raw[8];
    unsigned char bg_nl;
    const char *shape_outside;      /* shape-outside: a basic shape or a box, as written (NULL none) */
    double shape_margin;
    float shape_margin_pct;   /* (a percentage: of the containing block's inline size, at layout) */
    /* Its placement: no-repeat per axis (repeat otherwise); position as a
     * fraction of the free room plus a length per axis; size kind 0 auto,
     * 1 cover, 2 contain, 3 given (bg_size_w/h: px, or a fraction of the
     * area when bg_size_pct has bit 0 / 1; < 0 auto); attached to the
     * viewport (fixed). */
    unsigned char bg_no_repeat_x, bg_no_repeat_y;
    unsigned char bg_space, bg_round;   /* (background-repeat space / round: bit 1 across, 2 down) */
    unsigned char bg_clip, bg_origin;
    unsigned char bg_clip_col;   /* (the last layer's clip, the color's: 1 + as bg_clip; 0 as bg_clip) */   /* (clip: 0 border-box, 1 padding-box, 2 content-box; origin: 0 padding-box, 1 border-box, 2 content-box) */
    /* border-image: bi_src the image (NULL none); slices top right bottom
     * left (bit k of bi_slice_pct: a percent) and fill; widths by bi_w_kind
     * (0 a multiple of the border width, 1 px, 2 percent, 3 auto); outsets
     * (bit k of bi_out_num: a multiple); repeat across and down (0 stretch,
     * 1 repeat, 2 round, 3 space) */
    const char *bi_src, *bi_grad;   /* (bi_grad: a gradient source's text) */
    float bi_slice[4], bi_w[4], bi_out[4];
    unsigned char bi_slice_pct, bi_fill, bi_w_kind[4], bi_out_num, bi_rep[2], bi_init;
    double bg_pos_xf, bg_pos_xpx, bg_pos_yf, bg_pos_ypx;
    float bg_pos_xfn, bg_pos_yfn;   /* (the fractions where the basis is negative, bg_pos_neg bits 1 x, 2 y) */
    unsigned char bg_pos_neg;
    unsigned char bg_size_kind, bg_size_pct, bg_fixed;
    double bg_size_w, bg_size_h;
    double bg_size_cpx[2];
    const char *bg_size_raw[2];   /* (a calc() not linear in its percentage, as written: bits 16 / 32 of bg_size_pct) */   /* (a calc() size's length part, with its fraction in bg_size_w/h: bits 4 / 8 of bg_size_pct) */

    /* mask-image: an absolute image URL (mask_kind 1) or the text of a
     * linear-gradient(...) (mask_kind 2), interned (lives for the process);
     * NULL / 0 = no mask.  mask-size: kind 0 auto, 1 contain, 2 cover,
     * 3 explicit (mask_size_w / _h in px, or fractions of the box when the
     * matching mask_size_pct bit is set; a negative height means auto).
     * mask-position: x / y in px, or fractions when mask_pos_pct bits are set.
     * mask_no_repeat: mask-repeat: no-repeat. */
    const char *mask_image;
    /* (the declared text, each layer's: image, mode, repeat, position, clip, origin, size, composite; [8] the shorthand) */
    const char *mk_text[9];
    const char *clip_path;     /* (clip-path as written; NULL none) */
    const char *border_shape;  /* (border-shape as written; NULL none) */
    uint32_t tfill_rgba;            /* (-webkit-text-fill-color: tfill_set 1 a color of its own, else the text's color; inherited) */
    unsigned char tfill_set, tfill_given;
    const char *content_img;   /* (content: url() on an element: the absolute URL of the image replacing it; NULL none) */
    /* (ruby: ruby_role 1 container, 2 base, 3 text, 4 base container, 5 text container, 6 a column's box;
     * an annotation's column box + 1 (ruby_col), its level, the next annotation of the same column + 1;
     * a column's first annotation + 1; ruby_sv: the display, inline_level and position box generation
     * changed, bit 0 set; ruby_align as declared: 1 start, 2 center, 3 space-between, 4 space-around) */
    int ruby_col, ruby_next, ruby_first, ruby_sv;
    unsigned char ruby_role, ruby_lvl, ruby_align;
    int repl_disp;             /* (inside an element content: url() replaces: its display before it was hidden, plus 1; 0 not hidden) */
    unsigned char mask_kind;
    unsigned char mask_size_kind;
    unsigned char mask_size_pct;
    unsigned char mask_pos_pct;
    unsigned char mask_no_repeat;
    /* (mask-origin and mask-clip, the first layer's: 0 border-box, 1 padding-box,
     * 2 content-box, 3 margin-box / no-clip) */
    unsigned char mask_origin, mask_clip;
    double mask_size_w, mask_size_h;
    double mask_pos_x, mask_pos_y;
    double mask_pos_xpx, mask_pos_ypx;   /* (an offset from an edge: px added to the fraction) */

    /* border-radius per corner (top-left, top-right, bottom-right,
     * bottom-left): px, or a fraction of the box when the corner's bit in
     * radius_pct is set (elliptical radii use their horizontal value). */
    float radius[4];
    unsigned char radius_pct;
    float radius_add[4], radius_vadd[4];   /* (calc(): px with the fraction) */
    /* (elliptical corners: the vertical radius, where radius_v_set's bit
     * says one was given, a fraction of the height when radius_vpct's is) */
    float radius_v[4];
    unsigned char radius_v_set, radius_vpct;
    /* (corner-shape per corner: superellipse(K), -16 notch .. 16 square,
     * where corner_shaped's bit is set; else round, K = 1) */
    float corner_k[4];
    unsigned char corner_shaped;

    /* box-shadow: the first outer (non-inset) shadow, if any. */
    unsigned char shadow_set;
    /* (box-shadow as written; how many shadows; several or an inset one: painted item by item; this one inset) */
    const char *shadow_raw;
    unsigned char shadow_n, shadow_multi, shadow_inset, shadow_cc;   /* (cc: no colour given, the element's) */
    float shadow_x, shadow_y, shadow_blur, shadow_spread;
    float shadow_r, shadow_g, shadow_b, shadow_a;

    CssDisplay display;
    CssVisibility visibility;
    CssPosition position;
    CssBoxSizing box_sizing;
    CssFlexDirection flex_direction;
    CssFlexWrap flex_wrap;
    unsigned char flex_balance;   /* (flex-wrap: balance) */
    CssJustifyContent justify_content;
    CssJustifyContent align_content;
    /* not normal or stretch, so nothing stretches to fill: 1 justify-content, 2 align-content */
    unsigned char content_set;
    /* (calc-size() on a keyword: the expression over what the keyword comes to) */
    const char *csz_w, *csz_h;
    const char *csz_minw, *csz_minh;   /* (min-width/-height: calc-size(auto, ...): the expression over the automatic minimum) */
    /* (content keywords: min-width's and max-width's, 1 min 2 max 3 fit; on height the
     * content is what it comes to) */
    unsigned char mm_kw_w[2], h_kw, mm_kw_h;
    /* (contain: size and inline-size: 1 inline, 2 block; and the sizes it then has) */
    unsigned char contain_size;
    unsigned char contain_any;
    unsigned char contain_layout;
    unsigned char contain_style;    /* (contain: style, content or strict: counters and quotes scoped to it) */   /* (contain: layout, content or strict: its descendants' overflow its ink only) */
    unsigned char contain_paint;    /* (contain: paint, content or strict: clipped to the padding box) */   /* (contain other than none: blocks the body's propagation) */
    /* text-emphasis (inherited): the mark ("" none; te_shape a shape keyword
     * whose mark depends on the writing mode: 1 filled, 2 open, 0 a string),
     * its color (te_color_set: else currentColor) and side (te_under) */
    char te_mark[16];
    unsigned char te_set, te_color_set, te_under, te_pos_set, te_shape;
    unsigned char te_left;   /* (vertical text: the marks on the left, else the right) */
    uint32_t te_rgba;
/* (size containment from contain, content-visibility, or container-type: 1 inline axis, 2 block axis) */
#define CSS_CONTAIN_SIZE_OF(s) ((((s)->table_role >= 2 && (s)->table_role != 5) || (s)->ruby_int) ? 0 :     ((s)->contain_size | ((s)->container_type == 2 ? 3 : (s)->container_type == 1 ? 1 : 0)))
    float cis_w, cis_h;     /* contain-intrinsic-width and -height, px; 0 none */
    unsigned char cis_auto; /* auto before them: the last remembered size first (1 width, 2 height) */
    unsigned char cis_given; /* (not none: 1 width, 2 height) */
    unsigned char cv_hidden; /* content-visibility: hidden (its contents skipped) */
    unsigned char margin_trim; /* margin-trim: 1 block-start, 2 inline-start, 4 block-end, 8 inline-end */
    unsigned char baseline_src; /* baseline-source: 0 auto, 1 first, 2 last */
    CssAlignItems align_items;
    unsigned char ai_stretch;   /* (align-items: stretch itself, not normal) */
    unsigned char self_normal;  /* (bit 1: align-self normal, 2: justify-self normal, not auto) */
    unsigned char flex_line_count;
    unsigned char ruby_pos;          /* (ruby-position as declared here: 0 none, 1 over, 2 under; inherited by lookup) */   /* (flex-line-count: balanced lines, at least this many; 0/1 none) */

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
    const char *w_mm;   /* (width: min()/max()/clamp() over percentages, as written; NULL none) */
    double height_percent, height_pct_px;
    double gap_row, gap_col;
    unsigned char gap_row_given;      /* row-gap other than normal */
    unsigned char gap_col_given;      /* column-gap other than normal (multicol's normal is 1em) */
    /* Multi-column layout (CSS Multicol 1): column-count (0 auto), column-width
     * (column_width_set: not auto), column-fill (0 balance, 1 auto) */
    int column_count;
    double column_width;
    unsigned char column_width_set, column_fill, column_span_all;
    double column_height;              /* (column-height; 0 auto) */
    unsigned char column_wrap;         /* (0 auto, 1 nowrap, 2 wrap) */
    char page_name[24];                /* (page: its name, "" auto) */
    double block_step;                 /* (block-step-size) */
    unsigned char appearance_none;     /* (appearance: none) */
    unsigned char accent_set;          /* (accent-color: a color) */
    float accent[3];
    /* column-rule (column_rule_style a CssBorderStyle; color currentColor
     * unless column_rule_color_set) */
    float column_rule_width;
    float contents_mp[8];   /* (display: contents: the margins and padding it computed, for a child's inherit) */
    unsigned char column_rule_style, column_rule_width_set, column_rule_color_set;
    uint32_t column_rule_rgba;
    /* Gap decorations (CSS Gaps 1), [0] the column rules, [1] the row rules:
     * each list's values (n 0: the initial one) with its repeat(auto, ...)
     * run at [as, as + an); break (0 normal, 1 none, 2 intersection);
     * visibility (0 normal, 1 all, 2 around, 3 between); insets cap-start,
     * cap-end, junction-start, junction-end (px plus a fraction of the
     * crossing gap; oj overlap-join); gr_any: some rule has a style. */
    struct {
        uint32_t rgba[6];
        unsigned char cset[6], style[6];
        float w[6];
        unsigned char nc, cas, can, ns, sas, san, nw, was, wan;
        unsigned char brk, vis;
        struct { float px, pct; unsigned char oj; } inset[4];
    } gr[2];
    unsigned char gr_overlap, gr_any;  /* (overlap 1: column-over-row) */
    float gap_row_pct, gap_col_pct;   /* gaps of a percentage (fractions): of the container's own content box, at layout */

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
    /* (math-depth: md_kind 0 inherited, 1 a number, 2 add(), 3 auto-add; math_depth the computed one;
     * math-style compact (mc_set: set here); fs_math: font-size: math) */
    int md_val, math_depth;
    /* (caret-shape: 0 auto, 1 bar, 2 block, 3 underscore; caret-color as written, NULL auto) */
    unsigned char caret_shape, caret_shape_set, caret_color_set;
    unsigned char inl_li;   /* (display: inline list-item: an inline box, a list item; 2: inline flow-root list-item, an inline-block one) */
    const char *caret_color;
    unsigned char md_kind, math_compact, mc_set, fs_math;
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
    /* (of that, the transform's own and the translate property's: the two add up) */
    double tft_x, tft_y, tft_xpct, tft_ypct, tpr_x, tpr_y, tpr_xpct, tpr_ypct;
    /* (more than a translation: as written; the box itself stays where it's laid out) */
    const char *tf_text, *tf_origin, *tf_rotate, *tf_scale;
    double persp;                 /* (perspective: its length, 0 none) */
    const char *persp_origin;     /* (perspective-origin as written, NULL the centre) */
    unsigned char t3d_preserve, backface_hidden;   /* (transform-style: preserve-3d; backface-visibility: hidden) */
    const char *tf_raw;     /* (transform as written, translations too: for SVG elements) */
    unsigned char tb_trim, tb_over, tb_under;
    unsigned char tb_edge_set;        /* (text-box-edge its own: else inherited) */
    float h_lh;             /* (height in lh: of the line height the element comes to; 0 none) */
    unsigned char bdb_clone;   /* (box-decoration-break: clone) */
    unsigned char brk_before, brk_after, brk_inside;   /* (break-*: 0 auto, 1 a forced break, 2 avoid) */   /* (text-box-trim: 1 start, 2 end; text-box-edge over 0 text 1 cap 2 ex, under 0 text 1 alphabetic) */
    unsigned char tf_box;   /* (transform-box: 0 view-box, 1 fill-box, 2 stroke-box, 3 content-box, 4 border-box) */
    /* width: min-content (1) / max-content (2) / fit-content (3). */
    unsigned char width_keyword;
    unsigned char fc_set;             /* (width: fit-content(<length-percentage>): its px and fraction of the containing block) */
    float fc_px, fc_pct;
    /* Percentage insets (top, right, bottom, left): bit set => the
     * corresponding inset_pct ratio applies against the containing block. */
    double inset_pct[4];
    unsigned char inset_pct_bits;
    /* white-space: 0 normal, 1 nowrap, 2 pre, 3 pre-wrap, 4 pre-line. */
    unsigned char white_space;
    unsigned char white_space_set;
    unsigned char break_spaces;   /* white-space: break-spaces (white_space 3, and spaces that take room and wrap) */
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
    /* line-clamp (CSS Overflow 4) given, which clamps any block container
     * (-webkit-line-clamp only a -webkit-box); line_clamp_auto: as many
     * lines as its height or max-height has room for */
    unsigned char line_clamp_std, line_clamp_auto;
    unsigned char cont_discard;   /* (continue: discard: no fragments past its last, a multicol's columns past its count) */
    unsigned char lc_no_ell;    /* (line-clamp: N no-ellipsis) */
    unsigned char lc_cont;      /* (continue: collapse set it) */
    const char *lc_ell;         /* (line-clamp: N "string": the block ellipsis; NULL: U+2026) */
    float max_h_lh;                   /* max-height as <n>lh (0: not): line-clamp: auto counts by it */
    /* text-overflow: ellipsis (for clipped single-line text). */
    unsigned char text_overflow_ellipsis;
    const char *to_str;   /* (text-overflow: <string>: it in place of the ellipsis) */
    /* object-fit: 0 fill, 1 contain, 2 cover, 3 none, 4 scale-down. */
    unsigned char object_fit;
    /* display: grid-lanes (CSS Grid 3): lanes_dir 0 column lanes (items
     * stack down), 1 row lanes; lanes_fill_rev, lanes_track_rev */
    unsigned char grid_lanes, lanes_dir, lanes_dir_set, lanes_fill_rev, lanes_track_rev, fit_tol_set;
    const char *fvs;                  /* (font-variation-settings: "tag=value;..." interned, NULL normal) */
    unsigned char fvs_set;
    unsigned char fos_none, fos_set;  /* (font-optical-sizing: none) */
    double height_pct_neg;            /* (a height calc()'s negative percentage, height_percent then a marker) */
    double fit_tol;                   /* fit-tolerance in px (fit_tol_set: not normal, 1em) */
    /* object-position (obj_pos_set: given, else 50% 50%): fractions of the
     * room left and pixels, by axis, as background-position's */
    float obj_xf, obj_xpx, obj_yf, obj_ypx;
    unsigned char obj_pos_set;
    /* object-view-box: ovb_kind 0 none, 1 inset(t r b l), 2 rect(t r b l),
     * 3 xywh(x y w h); its four values, px or (bit k of ovb_pct) percents */
    unsigned char ovb_kind, ovb_pct;
    float ovb[4];

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
    unsigned char isolate;      /* (isolation: isolate: a stacking context) */
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
const char *css_layout_marker_text(const struct LayoutContext *ctx, int idx);
/* The color list item idx's ::marker gives (rgba 0..1); false: none of its own. */
bool css_layout_marker_color(const struct LayoutContext *ctx, int idx, float rgba[4]);
bool css_layout_marker_has_content(const struct LayoutContext *ctx, int idx);
/* An inline box's content area (its metric font's ascent + descent, rounded) and its line advance. */
double css_layout_content_area(const struct LayoutContext *ctx, int i, double *adv);
int css_layout_box_lang(const struct LayoutContext *ctx, int i);
bool css_layout_te_under(const struct LayoutContext *ctx, int i);
const char *css_layout_block_ellipsis(const struct LayoutContext *ctx, int i);
const char *css_layout_line_ellipsis(const struct LayoutContext *ctx, int i);
void css_layout_marker_spacing(const struct LayoutContext *ctx, int idx, double fs, float *ls, float *ws);
double css_layout_hl_length(const struct LayoutContext *ctx, int box, const char *v, double fs);
/* The font list item idx's ::marker asks for: size (px), weight, italic, family; each left as given when not. */
void css_layout_marker_font(const struct LayoutContext *ctx, int idx, double *fs, int *weight, int *italic, const char **family);
double css_layout_marker_baseline(struct LayoutContext *ctx, int idx);
/* (the first text in item idx: its em box top as drawn and its font size; NAN when none) */
double css_layout_marker_text_top(struct LayoutContext *ctx, int idx, double *font_size);
/* An outside marker's tab-size in spaces (its ::marker's, else the item's). */
int css_layout_marker_tab(const struct LayoutContext *ctx, int idx);

/* A box of the layout tree: its computed style (shared, read-only once the
 * style pass is over) and the geometry the layout passes compute.  The
 * margins, paddings, min/max sizes and insets here are the used values,
 * which start from the style's specified ones (layout_box_start). */
typedef struct LayoutBox {
    LayoutStyle *s;
    double x, y;
    double width, height;
    /* A multi-column container's columns (mc_n 0: not one): its content laid
     * out as one column mc_w wide, painted in columns mc_h high, mc_gap apart */
    int mc_n, mc_used;          /* (mc_used: the columns its content takes) */
    float mc_grow;              /* (how much the column push grew it, inside a flex line) */
    float mc_w, mc_gap, mc_h;
    unsigned char mc_nested;
    double bs_added;            /* (block-step-size: what was added to the end margin) */
    unsigned char mc_vert;      /* (its flow across: 1 block axis along +x, 2 along -x; its children's mc_src, mc_dst from its content start) */
    unsigned char mc_hang;      /* (in columns: 1 can't break and passes its column's end, whole there; 2 a box around such a one) */
    float mc_cut;               /* (2: how much of it went with what hangs, painted past the column's end) */
    int rep_h, rep_f;           /* (a table in columns: its header and footer groups repeated at its breaks, -1 none) */
    unsigned char clr_empty;    /* (an empty box placed with clearance: the margins collapsed through it stay in its parent) */
    unsigned char bdb_n;        /* (box-decoration-break: clone, in columns: the breaks in it, each its decorations again) */    /* (its columns in rows, one in each column of the multicol container around it) */
    float mc_rgap;              /* (column-wrap: rows of mc_n columns, this far apart; < 0 none) */
    /* A multi-column container's child: its run of columns (between spanners)
     * starts at mc_src in the one column it was laid out in and is painted from
     * mc_dst, columns mc_seg_h high (mc_seg_h 0: not set); mc_span: a spanner,
     * laid out across the container and painted where it is */
    float mc_src, mc_dst, mc_seg_h;
    float rel_dx, rel_dy;       /* (its own relative offset and translate, applied after the columns are cut) */
    unsigned char mc_span, mc_seg_last;
    /* (a container with spanners deeper in it: its runs by where they are in the
     * one column, ctx->mc_segs[mc_seg0 .. mc_seg0 + mc_nseg)) */
    int mc_seg0, mc_nseg;
    double margin_top, margin_right, margin_bottom, margin_left;
    double padding_top, padding_right, padding_bottom, padding_left;
    double content_width, content_height;
    short grid_r, grid_rs;   /* (a grid item: its first row and span as last placed; span 0 not placed) */
    double baseline;

    double min_width, max_width;
    double min_height, max_height;
    double top, left, right, bottom;

    uint32_t flags;
    /* Set by layout_resolve_used_sizes when a percentage height could not
     * resolve (indefinite containing block): the used height is auto. */
    unsigned char height_pct_unresolved;
    float strut_cross;          /* (a collapsed flex item: its line's cross size from the layout with it shown) */
    unsigned char pct_indef;    /* (a column flex item whose flexed height stays indefinite: no percentage base) */
    unsigned char flex_def_h;   /* (a flex item whose height flex layout made definite) */
    unsigned char flex_given;   /* (a table flex item in its layout: 1 its width, 2 its height what flex gave it) */
    /* An absolutely positioned box's static position, from the line it is
     * on (inline layout on items), relative to its parent's box; used and
     * cleared by the absolute positioning pass. */
    unsigned char has_static;
    unsigned char clamp_gone;   /* (after a line-clamp's last line: not painted) */
    unsigned char clamp_keep;   /* (a box of a height of its own before the clamp point: what overflows it stays) */
    int run_in_prev;            /* (a block a run-in goes into: its index + 1) */
    unsigned char clamp_clip;   /* (a line-clamp container that clamped: what is past clamp_y, its clamp point, not painted) */
    float clamp_y;
    float marker_dx;   /* (an outside marker moved in by floats: its line's start past them) */
    double static_dx, static_dy;
    double opacity_eff;
    double ell_start;           /* (right to left: how far before the content edge the furthest would have begun; negative, 0 none) */
    double ell_end;             /* (text-overflow cut its lines: where the furthest would have ended, for its scroll range; 0 none) */
} LayoutBox;
struct LayoutContext;
bool css_layout_shadow_item(const struct LayoutContext *ctx, const LayoutBox *box, int k, LayoutStyle *out);
bool css_layout_bg_layer(const struct LayoutContext *ctx, const LayoutBox *box, int k, LayoutStyle *out);

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
    float ell_by;             /* (its line's baseline, the block's, below its own: where the ellipsis sits) */
    unsigned char ell_dir;    /* (its paragraph's direction where plaintext gives it one: 1 rtl, 2 ltr; 0 the block's) */
    unsigned char ell_abs;    /* (ell_top set: the ellipsis's em box top from the box's y, as the block's own text there would have it) */
    double ell_top;
    float ell_dx;            /* (the edges of inline boxes closing after it: the ellipsis past them) */
    unsigned char fl;         /* on a ::first-line whose color it takes (fl_rgba) */
    uint32_t fl_rgba;
    float jls;                /* (text-justify: inter-character: the room added after each character) */
} LayoutTextFrag;

typedef struct LayoutTextFragList {
    LayoutTextFrag *f;
    int n;
} LayoutTextFragList;

/* (a block's line boxes, top and height, as its inline layout placed them:
 * kept in a multi-column container for its breaks) */
/* (a run of columns: the one column's [src, end) in columns h high from dst) */
typedef struct LayoutMcSeg {
    float src, end, h, dst;
    int used;   /* (the columns with content) */
    float rgap; /* (a row of columns below the run before, this far below it; < 0: not one) */
} LayoutMcSeg;

typedef struct LayoutLineList {
    float *t, *h;
    int n, cap;
    double base;    /* (the block's y when they were recorded: it may have moved since) */
} LayoutLineList;

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
    unsigned char il;                  /* (an initial letter: a float on its side after it goes below it) */
    /* shape-outside (CSS Shapes 1), what lines wrap around: 0 none (the
     * margin box), 1 an ellipse (s[0..3]: cx, cy, rx, ry), 2 a rounded
     * rectangle (s[0..3]: x0, y0, x1, y1; rad: tl, tr, br, bl, x and y), 3 a
     * polygon (npts points); shape-margin already in */
    unsigned char shape, npts;
    float s[4], rad[4][2], margin;
    float pts[32][2];
    const float *rows;                 /* (shape 4, an image's: each row's left and right, rows from s[1]; NAN none) */
    int nrows;
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
    /* (per box, its line boxes, in a multi-column container only; NULL: none) */
    LayoutMcSeg *mc_segs;
    int mc_segs_n, mc_segs_cap;
    LayoutLineList *lines;
    int lines_n;

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
    /* An outside marker's text, by box (layout_counters; NULL: none) */
    char **marker_text;
    int marker_text_n;
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
/* (::marker { content: none } on list item idx: no marker of any kind) */
bool css_layout_marker_none(const LayoutContext *ctx, int idx);
/* (the last character of the text before text box idx in its paragraph, through inline boxes; 0 none) */
uint32_t css_layout_text_prev_cp(const LayoutContext *ctx, int idx);
/* (the code point before byte a of t; 0 at its start) */
uint32_t css_layout_cp_before(const char *t, uint32_t a);
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
/* (offset-path path(): the point offset-distance along it and the turn offset-rotate gives there) */
bool css_layout_motion_point(const LayoutStyle *s, double *px, double *py, double *rot_deg);
/* (the same with what is in multicol container mc about its piece in column
 * col: the strip's top, a column's height) */
bool css_layout_box_map_col(const LayoutContext *ctx, int idx, int mc, double top, double H, int col, double m[6]);
/* A container query (a @container prelude) for the DOM node dom; self: dom may be the container. */
bool css_layout_container_match(const LayoutContext *ctx, int dom, bool self, const char *query);
/* A rect's bounds under m. */
void css_layout_map_bounds(const double m[6], double *x, double *y, double *w, double *h);

/* Box idx's values as the transitions store's before-change style. */
void css_layout_anim_seed(const LayoutContext *ctx, int idx, uint32_t el);

/* The rectangles of the bytes [a, b) of text box idx's data, one per line. */
/* Gap decorations' geometry (CSS Gaps 1), from the content box's corner: a
 * container's gaps (axis 0 between columns, its rules down; 1 between rows),
 * gi the gap's place in its axis' value lists, pos and w its middle and
 * width across, its gap segments along it [at, at + n) and at its two ends
 * the crossing gap's width (0: a cap) and index; each segment where it runs,
 * occ its sides with items (bit 0 start, 1 end), disc an item over the gap,
 * and the junction after it: jt the crossing gaps there (0 none, 1 one
 * side, 2 both), jw its width, ci its index.  kind 0 grid, 1 flex. */
typedef struct { float s, e, jw; int ci; unsigned char occ, disc, jt; } CssGapSeg;
typedef struct { unsigned char axis; int gi, nn; float pos, w, jw0, jw1; int ci0, ci1, at, n; } CssGapLine;   /* (nn: the gaps its lists run over, 0 its axis' all) */
int css_layout_gap_lines(const LayoutContext *ctx, int idx, unsigned char *kind, const CssGapLine **lines, const CssGapSeg **segs);
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
/* Print: each of the n pages' names into g_css_print.pname. */
void css_layout_print_names(LayoutContext *ctx, int n);

/* Style and lay out the tree (parallel inside, synchronous to the caller). */
bool css_layout_document(LayoutContext *ctx, CssStylesheet *sheet);

/* Convenience: allocate/resolve/free in one call. */
/* (a frame's document just laid out in ctx, the iframe its NodeId: its content size noted when it opts in to responsive sizing) */
void css_layout_frame_content_note(uint32_t iframe_node, LayoutContext *ctx);
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
/* (css_hl_marks: the cascade above calls fn with prop "\x01" before each
 * element's declarations, root first) */
extern __thread bool css_hl_marks;
/* object-view-box: the view box of an image of natural size W x H; false: none. */
bool css_layout_view_box(const LayoutStyle *s, double W, double H, double *x, double *y, double *w, double *h);
/* A custom property's computed value on box idx; NULL: none. */
const char *css_layout_custom_prop(const LayoutContext *ctx, int idx, const char *name);
bool css_layout_element_cascade(LayoutContext *ctx, int idx, CssCascadeFn fn, void *ud);
/* The computed value of custom property name (--x) on box idx's element
 * from ctx's last layout, or NULL. */
const char *css_layout_element_custom(LayoutContext *ctx, int idx, const char *name);
/* The same for an element of the mirror document (dom index) that has no
 * styled box (inside a display:none subtree): var() resolves against the
 * custom properties of box holder (its nearest styled ancestor). */
bool css_layout_dom_cascade(LayoutContext *ctx, int dom, int holder, CssCascadeFn fn, void *ud);
bool css_layout_parse_color_text(const char *text, double *rgba);
/* (does this box's style use what only a 3D transform chain can do?) */
bool css_layout_style_3d(const LayoutStyle *s);
/* (how near the eye a box's centre is within its 3D rendering context) */
double css_layout_box_depth3d(const LayoutContext *ctx, int idx);
bool css_layout_style_keeps3d(const LayoutStyle *s);
/* (a box's 2D map through the 3D chain of its ancestors, flattened at its place; *hidden: it
 * or what it is flattened into faces away with backface-visibility: hidden, or is behind the eye) */
bool css_layout_box_map3d(const LayoutContext *ctx, int idx, double m[6], bool *hidden);
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

/* (a list item: display list-item, or an inline-block one) */
#define CSS_IS_LI(s) ((s)->display == CSS_DISPLAY_LIST_ITEM || ((s)->inl_li == 2 && (s)->display == CSS_DISPLAY_INLINE_BLOCK))
#endif /* CSS_LAYOUT_H */

/*
 * CSS Layout Engine - Implementation
 *
 * Style and layout pipeline described in PARALLEL_STYLE_LAYOUT.md.
 */

#include "cyber_env.h"
#include "css_layout.h"
#include "css_parser.h"
#include "browser_api_impl.h"
#include "platform.h"
#include "url_utils.h"
#include "quickjs_gc_unified.h"
#include "http_download.h"
#include "html_dom.h"
#include "dom_wrap.h"
#include "text_shaper.h"
#include "display_list.h"
#include "image_cache.h"
#include "css_style.h"
#include "frame_arena.h"
#include "job_system.h"
#include "gc_platform.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#include <unistd.h>
#endif

/* Debug switches, read once per layout (never inside per-node code). */
enum {
    LDBG_MEASURE = 1, LDBG_FLOAT = 2, LDBG_INLINE = 4, LDBG_FLEX = 8
};
static unsigned g_layout_debug;

/* Parallel layout (PARALLEL_STYLE_LAYOUT.md §9): flex items and table cells
 * are positioned before their subtrees are laid out, so sibling subtrees are
 * independent units — unless a float inside one could intrude into another
 * through the shared float stack.  Such containers stay serial. */
typedef struct LayoutPar {
    bool enabled;
    int min_nodes;              /* smallest item group worth forking */
    uint8_t *has_float;         /* per layout index: subtree contains a float */
    int *subtree;               /* per layout index: subtree size */
    _Atomic uint32_t regions, units, serial_regions;
} LayoutPar;
static LayoutPar g_lpar;

/* CSS_LOG=1: per-stage times on stderr. */
static int g_css_log = -1;
static double layout_now_ms(void) { return (double)gc_os_now_ns() / 1e6; }
#define STAGE_BEGIN() double _st_t = layout_now_ms()
#define STAGE(name) do { if (g_css_log > 0) { double _n = layout_now_ms(); \
    fprintf(stderr, "[css] %-22s %8.3f ms\n", name, _n - _st_t); _st_t = _n; } } while (0)

#define LOG_TAG "css_layout"
#define LOG_INFO(...) platform_log(LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)
#define LOG_WARN(...) platform_log(LOG_LEVEL_WARN, LOG_TAG, __VA_ARGS__)
#define LOG_ERROR(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)

/* Storage for CSS custom properties (variables) on a single layout node. */
typedef struct CssCustomProp {
    char *name;
    char *value;
} CssCustomProp;

typedef struct CssCustomProps {
    CssCustomProp *props;
    int count;
    int capacity;
} CssCustomProps;


static inline HtmlNode* layout_node_dom(LayoutContext *ctx, int dom_idx)
{
    return (HtmlNode*)po_array_payload(&ctx->doc->array, dom_idx);
}

static const char* layout_node_attribute(HtmlNode *node, const char *name)
{
    if (!node || node->type != HTML_NODE_ELEMENT || !name) return NULL;
    for (HtmlAttribute *a = node->attributes; a; a = a->next) {
        if (strcasecmp(a->name, name) == 0) return a->value;
    }
    return NULL;
}

static inline LayoutNodeRef* layout_node_ref(LayoutContext *ctx, int idx)
{
    return &ctx->tree.nodes[idx];
}

static inline LayoutBox* layout_box(LayoutContext *ctx, int idx)
{
    return &ctx->boxes[idx];
}


/* Forward declarations for display/visibility helpers used during tree build. */
static CssDisplay layout_default_display(const char *tag_name);
static char* layout_resolve_url(const char *base_url, const char *href);

/* ============================================================================
 * Layout tree construction
 * ============================================================================ */

static int layout_count_active_nodes(LayoutContext *ctx)
{
    return (int)po_array_active_count(&ctx->doc->array);
}

/* Map active DOM node indices to dense layout indices. */
static bool layout_build_index_map(LayoutContext *ctx, int **out_map)
{
    int dom_count = (int)po_array_count(&ctx->doc->array);
    int *map = (int*)malloc(dom_count * sizeof(int));
    if (!map) return false;
    for (int i = 0; i < dom_count; i++) map[i] = -1;

    int layout_idx = 0;
    for (int i = 0; i < dom_count; i++) {
        if (!po_array_is_active(&ctx->doc->array, i)) continue;
        map[i] = layout_idx++;
    }
    *out_map = map;
    return true;
}

static bool layout_build_nodes(LayoutContext *ctx, const int *map)
{
    int dom_count = (int)po_array_count(&ctx->doc->array);
    int active = layout_count_active_nodes(ctx);

    ctx->tree.nodes = (LayoutNodeRef*)calloc(active, sizeof(LayoutNodeRef));
    ctx->tree.count = active;
    ctx->boxes = (LayoutBox*)calloc(active, sizeof(LayoutBox));
    if (!ctx->tree.nodes || !ctx->boxes) return false;

    for (int i = 0; i < dom_count; i++) {
        int idx = map[i];
        if (idx < 0) continue;

        PreOrderCompactionArrayNode *hdr = po_array_node(&ctx->doc->array, i);
        LayoutNodeRef *node = &ctx->tree.nodes[idx];
        node->dom_node_idx = i;
        node->parent_idx = (hdr->parent >= 0) ? map[hdr->parent] : -1;
        node->first_child_idx = (hdr->first_child >= 0) ? map[hdr->first_child] : -1;
        node->next_sibling_idx = (hdr->next_sibling >= 0) ? map[hdr->next_sibling] : -1;

        /* Find the previous active sibling, skipping tombstones. */
        int prev_dom = hdr->prev_sibling;
        while (prev_dom >= 0 && map[prev_dom] < 0) {
            PreOrderCompactionArrayNode *prev_hdr = po_array_node(&ctx->doc->array, prev_dom);
            prev_dom = prev_hdr ? prev_hdr->prev_sibling : -1;
        }
        node->prev_sibling_idx = (prev_dom >= 0) ? map[prev_dom] : -1;

        int child_count = 0;
        for (int c = hdr->first_child; c >= 0; c = po_array_next_sibling(&ctx->doc->array, c)) {
            if (po_array_is_active(&ctx->doc->array, c)) child_count++;
        }
        node->child_count = child_count;
    }

    /* Root is the first active node with no parent. */
    ctx->tree.root_idx = -1;
    for (int i = 0; i < active; i++) {
        if (ctx->tree.nodes[i].parent_idx < 0) {
            ctx->tree.root_idx = i;
            break;
        }
    }
    if (ctx->tree.root_idx < 0 && active > 0) ctx->tree.root_idx = 0;

    /* Initialize default display/visibility for each box. */
    for (int i = 0; i < active; i++) {
        LayoutBox *box = &ctx->boxes[i];
        HtmlNode *node = layout_node_dom(ctx, ctx->tree.nodes[i].dom_node_idx);
        if (node && node->type == HTML_NODE_ELEMENT) {
            box->display = layout_default_display(node->tag_name);
        } else {
            box->display = CSS_DISPLAY_INLINE;
        }
        box->visibility = CSS_VISIBILITY_VISIBLE;
        box->position = CSS_POSITION_STATIC;
        box->overflow = 0;
        /* HTML UA stylesheet: table cells are middle-aligned by default. */
        if (node && node->type == HTML_NODE_ELEMENT &&
            (strcasecmp(node->tag_name, "td") == 0 ||
             strcasecmp(node->tag_name, "th") == 0))
            box->vertical_align = 1;
        else
            box->vertical_align = 0;
        box->box_sizing = CSS_BOX_SIZING_CONTENT_BOX;
        box->flex_direction = CSS_FLEX_DIRECTION_ROW;
        box->flex_wrap = CSS_FLEX_WRAP_NOWRAP;
        box->justify_content = CSS_JUSTIFY_FLEX_START;
        box->align_items = CSS_ALIGN_STRETCH;
        box->font_size = 16.0;
        box->font_size_ratio = 0.0;
        box->font_family[0] = '\0';
        box->font_weight = 400;
        box->font_italic = 0;
        box->font_weight_set = 0;
        box->font_italic_set = 0;
        box->text_decoration = 0;
        box->text_decoration_set = 0;
        box->line_height = 0.0;
        box->line_height_set = 0;
        box->list_style_type = 0;
        box->list_style_type_set = 0;
        box->border_color_r = 0.0;
        box->border_color_g = 0.0;
        box->border_color_b = 0.0;
        box->border_color_a = 1.0;
        box->border_color_set = 0;
        box->background_image_url[0] = '\0';
        box->color_r = 0.0;
        box->color_g = 0.0;
        box->color_b = 0.0;
        box->color_a = 1.0;
        box->color_set = 0;
        box->fill_kind = 0;
        box->fill_set = 0;
        box->font_size_set = 0;
        box->width_set = 0;
        box->height_set = 0;
        box->visibility_set = 0;
        box->text_align = CSS_TEXT_ALIGN_LEFT;
        box->text_align_set = 0;
        box->flex_basis = -1.0;
        box->flex_basis_percent = 0.0;
        box->flex_grow = 0.0;
        box->wrap_first_w = 0.0;
        box->wrap_cont_w = 0.0;
        box->wrap_cont_x = 0.0;
        box->wrap_cont2_x = 0.0;
        box->wrap_cont2_w = 0.0;
        box->wrap_cont2_line = 0;
        box->margin_em[0] = box->margin_em[1] = box->margin_em[2] = box->margin_em[3] = 0.0;
        box->padding_em[0] = box->padding_em[1] = box->padding_em[2] = box->padding_em[3] = 0.0;
        box->em_deferred = 0;
        box->pct_sides = 0;
        box->minmax_pct_bits = 0;
        box->width_pct_px = 0.0;
        box->height_pct_px = 0.0;
        box->flex_basis_pct_px = 0.0;
        box->flex_shrink = 1.0;
        box->min_width = 0.0;
        box->max_width = 0.0;
        box->min_height = 0.0;
        box->max_height = 0.0;
        box->top = 0.0;
        box->left = 0.0;
        box->right = 0.0;
        box->bottom = 0.0;
        box->positioned_sides = 0;
        box->width_percent = 0.0;
        box->height_percent = 0.0;
        box->gap_row = 0.0;
        box->gap_col = 0.0;
        box->aspect_ratio = 0.0;
        box->aspect_ratio_prop = 0;
    }

    return true;
}

static void layout_build_preorder_recursive(LayoutContext *ctx, int idx, int *preorder, int *pos)
{
    preorder[(*pos)++] = idx;
    for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        layout_build_preorder_recursive(ctx, c, preorder, pos);
    }
}

static void layout_build_postorder_recursive(LayoutContext *ctx, int idx, int *postorder, int *pos)
{
    for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        layout_build_postorder_recursive(ctx, c, postorder, pos);
    }
    postorder[(*pos)++] = idx;
}

static bool layout_build_orders(LayoutContext *ctx)
{
    int n = ctx->tree.count;
    ctx->tree.preorder = (int*)malloc(n * sizeof(int));
    ctx->tree.postorder = (int*)malloc(n * sizeof(int));
    if (!ctx->tree.preorder || !ctx->tree.postorder) return false;

    if (n == 0) return true;

    int pre_pos = 0, post_pos = 0;
    layout_build_preorder_recursive(ctx, ctx->tree.root_idx, ctx->tree.preorder, &pre_pos);
    layout_build_postorder_recursive(ctx, ctx->tree.root_idx, ctx->tree.postorder, &post_pos);

    /* Handle disconnected roots (should not happen for a single document). */
    for (int i = 0; i < n; i++) {
        if (ctx->tree.nodes[i].parent_idx < 0 && i != ctx->tree.root_idx) {
            layout_build_preorder_recursive(ctx, i, ctx->tree.preorder, &pre_pos);
            layout_build_postorder_recursive(ctx, i, ctx->tree.postorder, &post_pos);
        }
    }
    return true;
}

bool css_layout_tree_build(LayoutContext *ctx, HtmlDocument *doc)
{
    if (!ctx || !doc) return false;
    memset(ctx, 0, sizeof(*ctx));
    ctx->doc = doc;

    int *map = NULL;
    if (!layout_build_index_map(ctx, &map)) return false;

    bool ok = layout_build_nodes(ctx, map);
    if (ok) ok = layout_build_orders(ctx);

    if (ok) {
        ctx->custom_props = (const CssCustomProps**)calloc((size_t)ctx->tree.count + 1, sizeof(CssCustomProps*));
        if (!ctx->custom_props) ok = false;
    }

    if (ok) {
        ctx->float_cap = 64;
        ctx->float_stack = (FloatRec*)calloc((size_t)ctx->float_cap, sizeof(FloatRec));
        if (!ctx->float_stack) ok = false;
    }

    free(map);
    return ok;
}

void css_layout_tree_free(LayoutContext *ctx)
{
    if (!ctx) return;
    free((void *)ctx->custom_props);
    frame_arena_reset(&ctx->arena);
    free(ctx->float_stack);
    free(ctx->tree.nodes);
    free(ctx->tree.preorder);
    free(ctx->tree.postorder);
    free(ctx->boxes);
    free(ctx->intrinsic_cache);
    if (ctx->text_norm) {
        for (int i = 0; i < ctx->tree.count; i++) free(ctx->text_norm[i]);
        free(ctx->text_norm);
    }
    memset(ctx, 0, sizeof(*ctx));
}

/* ============================================================================
 * Property resolution helpers
 * ============================================================================ */

static bool css_is_inherited(const char *prop)
{
    static const char *inherited[] = {
        "color", "font-size", "font-family", "font-weight", "font-style", "line-height",
        "text-align", "visibility", "cursor", "letter-spacing", "word-spacing",
        "white-space", "direction", NULL
    };
    for (int i = 0; inherited[i]; i++) {
        if (strcasecmp(prop, inherited[i]) == 0) return true;
    }
    return false;
}

static double css_parse_length(const char *value, double parent_value, double viewport_value, double font_size);
/* Per-layout unit context: viewport size for vw/vh/vmin/vmax and the root
 * element's font size for rem (set when the root box is styled). */
static double g_unit_vw = 1024.0, g_unit_vh = 768.0, g_unit_rem = 16.0;
static double css_unit_factor(const char *unit, double parent_value, double font_size, bool *known)
{
    *known = true;
    if (strcasecmp(unit, "%") == 0) return parent_value / 100.0;
    if (strcasecmp(unit, "vw") == 0) return g_unit_vw / 100.0;
    if (strcasecmp(unit, "vh") == 0) return g_unit_vh / 100.0;
    if (strcasecmp(unit, "vmin") == 0) return (g_unit_vw < g_unit_vh ? g_unit_vw : g_unit_vh) / 100.0;
    if (strcasecmp(unit, "vmax") == 0) return (g_unit_vw > g_unit_vh ? g_unit_vw : g_unit_vh) / 100.0;
    if (strcasecmp(unit, "em") == 0) return font_size;
    if (strcasecmp(unit, "rem") == 0) return g_unit_rem;
    if (strcasecmp(unit, "pt") == 0) return 96.0 / 72.0;
    if (strcasecmp(unit, "pc") == 0) return 16.0;
    if (strcasecmp(unit, "in") == 0) return 96.0;
    if (strcasecmp(unit, "cm") == 0) return 96.0 / 2.54;
    if (strcasecmp(unit, "mm") == 0) return 96.0 / 25.4;
    if (strcasecmp(unit, "px") == 0) return 1.0;
    *known = false;
    return 1.0;
}

/* A length as px + pct x basis: percentages stay symbolic until the basis
 * (usually the containing block width) is known.  pct is a fraction
 * (50% -> 0.5). */
typedef struct { double px, pct; } CssLen;

/* Minimal calc() expression evaluator. Supports +, -, *, /, parentheses and
 * length/percentage operands; a product or quotient needs one side to be a
 * plain number.  Returns 0 for unsupported expressions. */
typedef struct {
    const char *s;
    size_t len;
    size_t pos;
    double font_size;
} CalcParser;

static void calc_skip_space(CalcParser *p)
{
    while (p->pos < p->len && isspace((unsigned char)p->s[p->pos])) p->pos++;
}

static CssLen calc_parse_expr(CalcParser *p);

static CssLen calc_parse_primary(CalcParser *p)
{
    CssLen zero = { 0.0, 0.0 };
    calc_skip_space(p);
    if (p->pos >= p->len) return zero;

    /* Nested calc( and bare parentheses. */
    if (p->len - p->pos > 5 && strncasecmp(p->s + p->pos, "calc(", 5) == 0) p->pos += 4;
    if (p->s[p->pos] == '(') {
        p->pos++;
        CssLen v = calc_parse_expr(p);
        calc_skip_space(p);
        if (p->pos < p->len && p->s[p->pos] == ')') p->pos++;
        return v;
    }

    const char *start = p->s + p->pos;
    char *end = NULL;
    double num = strtod(start, &end);
    if (end == start) return zero;
    p->pos = (size_t)(end - p->s);

    if (p->pos < p->len && p->s[p->pos] == '%') {
        p->pos++;
        CssLen v = { 0.0, num / 100.0 };
        return v;
    }
    if (p->pos < p->len && isalpha((unsigned char)p->s[p->pos])) {
        const char *unit_start = p->s + p->pos;
        while (p->pos < p->len && isalpha((unsigned char)p->s[p->pos])) p->pos++;
        size_t unit_len = (size_t)(p->s + p->pos - unit_start);
        char unit[8] = {0};
        if (unit_len < sizeof(unit)) {
            memcpy(unit, unit_start, unit_len);
            for (size_t i = 0; i < unit_len; i++) unit[i] = (char)tolower((unsigned char)unit[i]);
        }
        bool known;
        double f = css_unit_factor(unit, 0.0, p->font_size, &known);
        /* px or unknown unit: treat number as px */
        CssLen v = { num * f, 0.0 };
        return v;
    }
    CssLen v = { num, 0.0 }; /* unitless number */
    return v;
}

static CssLen calc_parse_term(CalcParser *p)
{
    CssLen lhs = calc_parse_primary(p);
    for (;;) {
        calc_skip_space(p);
        if (p->pos >= p->len) break;
        char op = p->s[p->pos];
        if (op != '*' && op != '/') break;
        p->pos++;
        CssLen rhs = calc_parse_primary(p);
        if (op == '*') {
            /* number * length or length * number */
            double k = rhs.pct == 0.0 ? rhs.px : lhs.px;
            CssLen l = rhs.pct == 0.0 ? lhs : rhs;
            lhs.px = l.px * k;
            lhs.pct = l.pct * k;
        } else if (rhs.px != 0.0) {
            lhs.px /= rhs.px;
            lhs.pct /= rhs.px;
        }
    }
    return lhs;
}

static CssLen calc_parse_expr(CalcParser *p)
{
    calc_skip_space(p);
    CssLen lhs = calc_parse_term(p);
    for (;;) {
        calc_skip_space(p);
        if (p->pos >= p->len) break;
        char op = p->s[p->pos];
        if (op != '+' && op != '-') break;
        p->pos++;
        CssLen rhs = calc_parse_term(p);
        double sign = op == '+' ? 1.0 : -1.0;
        lhs.px += sign * rhs.px;
        lhs.pct += sign * rhs.pct;
    }
    return lhs;
}

/* A length value (number+unit, percentage or calc()) as px + pct.  Unitless
 * numbers and unknown units are px. */
static CssLen css_parse_len(const char *value, double font_size)
{
    CssLen out = { 0.0, 0.0 };
    if (!value || !*value) return out;
    while (*value && isspace((unsigned char)*value)) value++;
    if (strncasecmp(value, "calc(", 5) == 0) {
        CalcParser p = {0};
        p.s = value + 5;
        p.len = strlen(p.s);
        p.font_size = font_size;
        return calc_parse_expr(&p);
    }
    char *end = NULL;
    double num = strtod(value, &end);
    if (end == value) return out;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end == '%') { out.pct = num / 100.0; return out; }
    if (*end == '\0') { out.px = num; return out; }
    bool known;
    out.px = num * css_unit_factor(end, 0.0, font_size, &known);
    return out;
}

/* A length resolved now, percentages against parent_value. */
static double css_parse_length(const char *value, double parent_value, double viewport_value, double font_size)
{
    (void)viewport_value;
    CssLen l = css_parse_len(value, font_size);
    return l.px + l.pct * parent_value;
}

static bool css_parse_color(const char *value, double *r, double *g, double *b, double *a)
{
    *r = *g = *b = 0.0; *a = 1.0;
    if (!value || !*value) return false;

    /* Skip leading whitespace. */
    while (*value && isspace((unsigned char)*value)) value++;
    if (!*value) return false;

    if (value[0] == '#') {
        int rr = 0, gg = 0, bb = 0;
        if (sscanf(value + 1, "%02x%02x%02x", &rr, &gg, &bb) == 3) {
            *r = rr / 255.0; *g = gg / 255.0; *b = bb / 255.0;
            return true;
        }
        /* 3-digit hex: #fff */
        if (sscanf(value + 1, "%1x%1x%1x", &rr, &gg, &bb) == 3) {
            *r = (rr * 17) / 255.0; *g = (gg * 17) / 255.0; *b = (bb * 17) / 255.0;
            return true;
        }
        return false;
    }

    /* rgb() / rgba() */
    if (strncasecmp(value, "rgb", 3) == 0) {
        int rr = 0, gg = 0, bb = 0;
        double aa = 1.0;
        const char *p = value + 3;
        if (*p == 'a') p++;
        if (*p == '(') {
            p++;
            /* Parse r,g,b and optional a */
            rr = (int)strtol(p, (char**)&p, 10);
            while (*p && (*p == ',' || *p == ' ' || *p == '/')) p++;
            gg = (int)strtol(p, (char**)&p, 10);
            while (*p && (*p == ',' || *p == ' ' || *p == '/')) p++;
            bb = (int)strtol(p, (char**)&p, 10);
            while (*p && (*p == ',' || *p == ' ' || *p == '/')) p++;
            if (*p && *p != ')') {
                aa = strtod(p, (char**)&p);
            }
            *r = rr / 255.0; *g = gg / 255.0; *b = bb / 255.0; *a = aa;
            return true;
        }
        return false;
    }

    /* hsl() / hsla() */
    if (strncasecmp(value, "hsl", 3) == 0) {
        double h, s, l;
        double aa = 1.0;
        const char *p = value + 3;
        if (*p == 'a') p++;
        if (*p == '(') {
            p++;
            h = strtod(p, (char**)&p);
            while (*p && (*p == ',' || *p == ' ' || *p == '/')) p++;
            s = strtod(p, (char**)&p);
            while (*p && (*p == ',' || *p == ' ' || *p == '/')) p++;
            l = strtod(p, (char**)&p);
            while (*p && (*p == ',' || *p == ' ' || *p == '/')) p++;
            if (*p && *p != ')') {
                aa = strtod(p, (char**)&p);
            }
            /* Convert HSL to RGB */
            s /= 100.0; l /= 100.0;
            double c = (1.0 - fabs(2.0 * l - 1.0)) * s;
            double hp = h / 60.0;
            double x = c * (1.0 - fabs(fmod(hp, 2.0) - 1.0));
            double r1 = 0, g1 = 0, b1 = 0;
            if (hp < 1) { r1 = c; g1 = x; b1 = 0; }
            else if (hp < 2) { r1 = x; g1 = c; b1 = 0; }
            else if (hp < 3) { r1 = 0; g1 = c; b1 = x; }
            else if (hp < 4) { r1 = 0; g1 = x; b1 = c; }
            else if (hp < 5) { r1 = x; g1 = 0; b1 = c; }
            else { r1 = c; g1 = 0; b1 = x; }
            double m = l - c / 2.0;
            *r = r1 + m; *g = g1 + m; *b = b1 + m; *a = aa;
            return true;
        }
        return false;
    }

    /* Named colors - common subset. */
    struct { const char *name; double r, g, b; } names[] = {
        {"black", 0,0,0}, {"white", 1,1,1}, {"red", 1,0,0},
        {"green", 0,0.5,0}, {"blue", 0,0,1}, {"yellow", 1,1,0},
        {"cyan", 0,1,1}, {"magenta", 1,0,1}, {"gray", 0.5,0.5,0.5},
        {"grey", 0.5,0.5,0.5}, {"transparent", 0,0,0},
        {NULL, 0,0,0}
    };
    for (int i = 0; names[i].name; i++) {
        if (strcasecmp(value, names[i].name) == 0) {
            *r = names[i].r; *g = names[i].g; *b = names[i].b;
            if (strcasecmp(value, "transparent") == 0) *a = 0.0;
            return true;
        }
    }
    return false;
}

/* If tok is exactly "<number>em" (sign allowed), return 1 and the numeric
 * ratio.  em units on margin/padding must resolve against the element's
 * FINAL computed font-size, which is not known while declarations are
 * still being applied, so the ratio is recorded and re-resolved in pass 3. */
static int css_token_is_em(const char *tok, double *ratio)
{
    if (!tok || !ratio) return 0;
    while (*tok && isspace((unsigned char)*tok)) tok++;
    char *end = NULL;
    double v = strtod(tok, &end);
    if (end == tok) return 0;
    while (*end && isspace((unsigned char)*end)) end++;
    if (end[0] != 'e' && end[0] != 'E') return 0;
    if (end[1] != 'm' && end[1] != 'M') return 0;
    const char *rest = end + 2;
    while (*rest && isspace((unsigned char)*rest)) rest++;
    if (*rest != '\0') return 0;
    *ratio = v;
    return 1;
}

/* Applies a 1-4 value box shorthand to the four side outputs.  Returns a
 * bitmask (bit 0=top, 1=right, 2=bottom, 3=left) of sides whose token was
 * em-dimensioned; when em_out is non-NULL it receives those sides' ratios
 * (side order top/right/bottom/left).  Pixel outputs always receive a
 * provisional value resolved against the current font-size. */
/* Split a space-separated value list into at most `max` tokens, keeping
 * parenthesized groups (calc(9 / 16 * 100%)) whole.  Returns the count. */
static int css_split_values(const char *value, char out[][128], int max)
{
    int n = 0;
    const char *p = value ? value : "";
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        size_t len = 0;
        int depth = 0;
        while (*p && (depth > 0 || (*p != ' ' && *p != '\t' && *p != '\n'))) {
            if (*p == '(') depth++;
            else if (*p == ')' && depth > 0) depth--;
            if (len < 127) out[n][len++] = *p;
            p++;
        }
        out[n][len] = '\0';
        n++;
    }
    return n;
}

/* Expand a 1-4 value side list to top/right/bottom/left token indexes. */
static void css_side_indexes(int count, int idx[4])
{
    static const int map[5][4] = { {0,0,0,0}, {0,0,0,0}, {0,1,0,1}, {0,1,2,1}, {0,1,2,3} };
    for (int i = 0; i < 4; i++) idx[i] = map[count > 4 ? 4 : count][i];
}

/* Record which margin (padding=false) or padding sides have a percentage
 * part, for resolution against the containing block width at layout time.
 * `side` is 0..3 for a longhand, -1 for the shorthand.  The side fields get
 * the px part meanwhile. */
static void layout_note_side_pcts(LayoutBox *box, const char *value, bool padding, int side)
{
    double *fields[4] = {
        padding ? &box->padding_top : &box->margin_top,
        padding ? &box->padding_right : &box->margin_right,
        padding ? &box->padding_bottom : &box->margin_bottom,
        padding ? &box->padding_left : &box->margin_left,
    };
    double *pct = padding ? box->padding_pct : box->margin_pct;
    double *base = padding ? box->padding_base : box->margin_base;
    unsigned shift = padding ? 4u : 0u;
    CssLen len4[4];
    int first = side, last = side;
    if (side < 0) {
        char toks[4][128];
        int count = css_split_values(value, toks, 4);
        if (count == 0) return;
        int idx[4];
        css_side_indexes(count, idx);
        for (int i = 0; i < 4; i++) len4[i] = css_parse_len(toks[idx[i]], box->font_size);
        first = 0; last = 3;
    } else {
        len4[side] = css_parse_len(value, box->font_size);
    }
    for (int i = first; i <= last; i++) {
        unsigned bit = 1u << (shift + (unsigned)i);
        if (len4[i].pct != 0.0) {
            pct[i] = len4[i].pct;
            base[i] = len4[i].px;
            *fields[i] = len4[i].px;
            box->pct_sides |= (unsigned char)bit;
        } else {
            box->pct_sides &= (unsigned char)~bit;
        }
    }
}

/* Margins and paddings against the containing block width (CSS 2.1 §8.3,
 * §8.4: percentages refer to the width, also for top and bottom). */
static void layout_resolve_side_pcts(LayoutBox *box, double cb_width)
{
    if (!box->pct_sides) return;
    double *fields[8] = {
        &box->margin_top, &box->margin_right, &box->margin_bottom, &box->margin_left,
        &box->padding_top, &box->padding_right, &box->padding_bottom, &box->padding_left,
    };
    if (cb_width < 0.0) cb_width = 0.0;
    for (int i = 0; i < 8; i++) {
        if (!(box->pct_sides & (1u << i))) continue;
        double v = i < 4 ? box->margin_base[i] + box->margin_pct[i] * cb_width
                         : box->padding_base[i - 4] + box->padding_pct[i - 4] * cb_width;
        /* Negative padding is invalid. */
        if (i >= 4 && v < 0.0) v = 0.0;
        *fields[i] = v;
    }
}

/* min-width/max-width/min-height/max-height (which 0..3): returns the px
 * part, recording a percentage part for layout_resolve_minmax_pcts(). */
static double layout_note_minmax(LayoutBox *box, int which, const char *value)
{
    CssLen l = css_parse_len(value, box->font_size);
    unsigned char bit = (unsigned char)(1u << which);
    if (l.pct != 0.0) {
        box->minmax_pct[which] = l.pct;
        box->minmax_base[which] = l.px;
        box->minmax_pct_bits |= bit;
    } else {
        box->minmax_pct_bits &= (unsigned char)~bit;
    }
    return l.px;
}

/* Percentage min/max sizes: widths against the containing block width,
 * heights against its height; with an indefinite height they mean none. */
static void layout_resolve_minmax_pcts(LayoutBox *box, double cb_width, double cb_height)
{
    if (!box->minmax_pct_bits) return;
    double *fields[4] = { &box->min_width, &box->max_width, &box->min_height, &box->max_height };
    for (int i = 0; i < 4; i++) {
        if (!(box->minmax_pct_bits & (1u << i))) continue;
        double basis = i < 2 ? cb_width : cb_height;
        double v = basis > 0.0 ? box->minmax_base[i] + box->minmax_pct[i] * basis : 0.0;
        *fields[i] = v > 0.0 ? v : 0.0;
    }
}

static unsigned layout_apply_shorthand_sides(LayoutBox *box, const char *value,
                                             double parent_width, double viewport_width,
                                             double *top, double *right, double *bottom, double *left,
                                             double em_out[4])
{
    double values[4];
    double ems[4] = {0.0, 0.0, 0.0, 0.0};
    unsigned em_flags = 0;
    char toks[4][128];
    int count = css_split_values(value, toks, 4);
    for (int i = 0; i < count; i++) {
        values[i] = css_parse_length(toks[i], parent_width, viewport_width, box->font_size);
        double r;
        if (css_token_is_em(toks[i], &r)) {
            ems[i] = r;
            em_flags |= (1u << i);
        }
    }
    if (count == 0) return 0;

    double v4[4];
    double e4[4] = {0.0, 0.0, 0.0, 0.0};
    unsigned f4 = 0;
    if (count == 1) {
        v4[0] = v4[1] = v4[2] = v4[3] = values[0];
        e4[0] = e4[1] = e4[2] = e4[3] = ems[0];
        if (em_flags & 1u) f4 = 0xF;
    } else if (count == 2) {
        v4[0] = v4[2] = values[0];
        v4[1] = v4[3] = values[1];
        e4[0] = e4[2] = ems[0];
        e4[1] = e4[3] = ems[1];
        if (em_flags & 1u) f4 |= 0x5; /* T, B */
        if (em_flags & 2u) f4 |= 0xA; /* R, L */
    } else if (count == 3) {
        v4[0] = values[0];
        v4[1] = v4[3] = values[1];
        v4[2] = values[2];
        e4[0] = ems[0];
        e4[1] = e4[3] = ems[1];
        e4[2] = ems[2];
        if (em_flags & 1u) f4 |= 0x1;
        if (em_flags & 2u) f4 |= 0xA;
        if (em_flags & 4u) f4 |= 0x4;
    } else {
        v4[0] = values[0]; v4[1] = values[1];
        v4[2] = values[2]; v4[3] = values[3];
        e4[0] = ems[0]; e4[1] = ems[1]; e4[2] = ems[2]; e4[3] = ems[3];
        f4 = em_flags;
    }
    *top = v4[0]; *right = v4[1]; *bottom = v4[2]; *left = v4[3];
    if (em_out) {
        em_out[0] = e4[0]; em_out[1] = e4[1];
        em_out[2] = e4[2]; em_out[3] = e4[3];
    }
    return f4;
}

static CssDisplay layout_default_display(const char *tag_name) {
    if (!tag_name || !tag_name[0]) return CSS_DISPLAY_BLOCK;
    /* Non-rendered metadata tags should not generate boxes. */
    static const char *none_tags[] = {
        "head", "script", "style", "link", "meta", "title",
        "base", "template", "noscript", NULL
    };
    for (int i = 0; none_tags[i]; i++) {
        if (strcasecmp(tag_name, none_tags[i]) == 0) return CSS_DISPLAY_NONE;
    }
    /* Common replaced/phrasing elements default to inline. */
    static const char *inline_tags[] = {
        "span", "a", "em", "strong", "b", "i", "u", "s", "small",
        "img", "br", "wbr", "code", "sub", "sup", "label",
        "iframe", "canvas", NULL
    };
    /* Form controls are inline-block in the HTML UA stylesheet. */
    if (strcasecmp(tag_name, "input") == 0 || strcasecmp(tag_name, "button") == 0 ||
        strcasecmp(tag_name, "textarea") == 0 || strcasecmp(tag_name, "select") == 0)
        return CSS_DISPLAY_INLINE_BLOCK;
    for (int i = 0; inline_tags[i]; i++) {
        if (strcasecmp(tag_name, inline_tags[i]) == 0) return CSS_DISPLAY_INLINE;
    }
    if (strcasecmp(tag_name, "li") == 0) return CSS_DISPLAY_LIST_ITEM;
    /* Elements the HTML UA stylesheet makes block-level (tables are laid out
     * as blocks by this engine).  Everything else, including custom and
     * unknown elements, is inline by default. */
    static const char *block_tags[] = {
        "html", "body", "div", "p", "h1", "h2", "h3", "h4", "h5", "h6",
        "ul", "ol", "dl", "dt", "dd", "menu", "dir", "section", "article",
        "aside", "nav", "header", "footer", "main", "address", "blockquote",
        "figure", "figcaption", "form", "fieldset", "legend", "hr", "pre",
        "listing", "xmp", "plaintext", "center", "details", "summary",
        "dialog", "search", "hgroup", "optgroup", "frameset", "frame",
        "table", "caption", "colgroup", "col", "thead", "tbody", "tfoot",
        "tr", "td", "th", NULL
    };
    for (int i = 0; block_tags[i]; i++) {
        if (strcasecmp(tag_name, block_tags[i]) == 0) return CSS_DISPLAY_BLOCK;
    }
    return CSS_DISPLAY_INLINE;
}

static bool layout_display_is_block_level(CssDisplay d)
{
    return d == CSS_DISPLAY_BLOCK || d == CSS_DISPLAY_FLEX || d == CSS_DISPLAY_GRID ||
           d == CSS_DISPLAY_LIST_ITEM || d == CSS_DISPLAY_OTHER;
}

/* Blockification (CSS Display §2.7) and block-in-inline: flex/grid items,
 * absolutely positioned and floated boxes compute to block-level; an inline
 * element containing in-flow block-level children is laid out as a block
 * (the anonymous block boxes a browser creates around them span the same
 * area).  Runs bottom-up so a promotion propagates to inline ancestors. */
static void layout_blockify(LayoutContext *ctx)
{
    const int *post = ctx->tree.postorder;
    if (!post) return;
    for (int k = 0; k < ctx->tree.count; k++) {
        int i = post[k];
        LayoutBox *box = layout_box(ctx, i);
        if (box->display != CSS_DISPLAY_INLINE) continue;
        HtmlNode *node = layout_node_dom(ctx, ctx->tree.nodes[i].dom_node_idx);
        if (!node || node->type != HTML_NODE_ELEMENT) continue;
        bool promote = box->position == CSS_POSITION_ABSOLUTE || box->position == CSS_POSITION_FIXED ||
                       box->float_side != 0;
        int p = ctx->tree.nodes[i].parent_idx;
        if (!promote && p >= 0) {
            CssDisplay pd = layout_box(ctx, p)->display;
            if (pd == CSS_DISPLAY_FLEX || pd == CSS_DISPLAY_GRID) promote = true;
        }
        for (int c = ctx->tree.nodes[i].first_child_idx; !promote && c >= 0;
             c = ctx->tree.nodes[c].next_sibling_idx) {
            LayoutBox *cb = layout_box(ctx, c);
            if (layout_display_is_block_level(cb->display) && !cb->inline_level &&
                cb->position != CSS_POSITION_ABSOLUTE && cb->position != CSS_POSITION_FIXED &&
                cb->float_side == 0)
                promote = true;
        }
        if (promote) box->display = CSS_DISPLAY_BLOCK;
    }
}

static CssPosition css_parse_position(const char *value) {
    if (!value) return CSS_POSITION_STATIC;
    if (strcasecmp(value, "static") == 0) return CSS_POSITION_STATIC;
    if (strcasecmp(value, "relative") == 0) return CSS_POSITION_RELATIVE;
    if (strcasecmp(value, "absolute") == 0) return CSS_POSITION_ABSOLUTE;
    if (strcasecmp(value, "fixed") == 0) return CSS_POSITION_FIXED;
    if (strcasecmp(value, "sticky") == 0) return CSS_POSITION_STICKY;
    return CSS_POSITION_STATIC;
}

static CssBoxSizing css_parse_box_sizing(const char *value) {
    if (!value) return CSS_BOX_SIZING_CONTENT_BOX;
    if (strcasecmp(value, "border-box") == 0) return CSS_BOX_SIZING_BORDER_BOX;
    return CSS_BOX_SIZING_CONTENT_BOX;
}

static CssDisplay css_parse_display(const char *value) {
    if (!value) return CSS_DISPLAY_OTHER;
    if (strcasecmp(value, "none") == 0) return CSS_DISPLAY_NONE;
    if (strcasecmp(value, "block") == 0) return CSS_DISPLAY_BLOCK;
    if (strcasecmp(value, "inline") == 0) return CSS_DISPLAY_INLINE;
    if (strcasecmp(value, "inline-block") == 0) return CSS_DISPLAY_INLINE_BLOCK;
    if (strcasecmp(value, "list-item") == 0) return CSS_DISPLAY_LIST_ITEM;
    if (strcasecmp(value, "flex") == 0 ||
        strcasecmp(value, "inline-flex") == 0 ||
        strcasecmp(value, "-webkit-flex") == 0 ||
        strcasecmp(value, "-webkit-inline-flex") == 0 ||
        strcasecmp(value, "-webkit-box") == 0 ||
        strcasecmp(value, "-webkit-inline-box") == 0 ||
        strcasecmp(value, "flexbox") == 0 ||
        strcasecmp(value, "inline-flexbox") == 0) return CSS_DISPLAY_FLEX;
    if (strcasecmp(value, "grid") == 0 ||
        strcasecmp(value, "inline-grid") == 0) return CSS_DISPLAY_GRID;
    return CSS_DISPLAY_OTHER;
}

/* inline-flex / inline-grid / inline-table: atomic inline-level boxes whose
 * inner display css_parse_display already returned. */
static bool css_display_is_inline_level(const char *value) {
    if (!value) return false;
    return strcasecmp(value, "inline-flex") == 0 ||
           strcasecmp(value, "-webkit-inline-flex") == 0 ||
           strcasecmp(value, "-webkit-inline-box") == 0 ||
           strcasecmp(value, "inline-flexbox") == 0 ||
           strcasecmp(value, "inline-grid") == 0 ||
           strcasecmp(value, "inline-table") == 0;
}

/* Which sides of a margin shorthand are `auto` (bits 1=T 2=R 4=B 8=L). */
static unsigned char css_margin_auto_mask(const char *value) {
    if (!value) return 0;
    int is_auto[4] = {0, 0, 0, 0};
    int count = 0;
    const char *p = value;
    while (*p && count < 4) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *st = p;
        int depth = 0;
        while (*p && (depth > 0 || (*p != ' ' && *p != '\t'))) {
            if (*p == '(') depth++;
            else if (*p == ')' && depth > 0) depth--;
            p++;
        }
        is_auto[count++] = (p - st == 4 && strncasecmp(st, "auto", 4) == 0);
    }
    if (count == 0) return 0;
    int t, r, b, l;
    if (count == 1) t = r = b = l = is_auto[0];
    else if (count == 2) { t = b = is_auto[0]; r = l = is_auto[1]; }
    else if (count == 3) { t = is_auto[0]; r = l = is_auto[1]; b = is_auto[2]; }
    else { t = is_auto[0]; r = is_auto[1]; b = is_auto[2]; l = is_auto[3]; }
    return (unsigned char)((t ? 1 : 0) | (r ? 2 : 0) | (b ? 4 : 0) | (l ? 8 : 0));
}

static CssFlexDirection css_parse_flex_direction(const char *value) {
    if (!value) return CSS_FLEX_DIRECTION_ROW;
    if (strcasecmp(value, "row-reverse") == 0) return CSS_FLEX_DIRECTION_ROW_REVERSE;
    if (strcasecmp(value, "column") == 0) return CSS_FLEX_DIRECTION_COLUMN;
    if (strcasecmp(value, "column-reverse") == 0) return CSS_FLEX_DIRECTION_COLUMN_REVERSE;
    return CSS_FLEX_DIRECTION_ROW;
}

static bool css_value_is_percent(const char *value) {
    if (!value) return false;
    size_t len = strlen(value);
    return len > 1 && value[len - 1] == '%';
}

static double css_parse_percent_ratio(const char *value) {
    if (!css_value_is_percent(value)) return 0.0;
    char *end = NULL;
    double num = strtod(value, &end);
    if (end == value) return 0.0;
    return num / 100.0;
}

static CssVisibility css_parse_visibility(const char *value) {
    if (!value) return CSS_VISIBILITY_VISIBLE;
    if (strcasecmp(value, "hidden") == 0) return CSS_VISIBILITY_HIDDEN;
    if (strcasecmp(value, "collapse") == 0) return CSS_VISIBILITY_COLLAPSE;
    return CSS_VISIBILITY_VISIBLE;
}

static CssFlexWrap css_parse_flex_wrap(const char *value) {
    if (!value) return CSS_FLEX_WRAP_NOWRAP;
    if (strcasecmp(value, "wrap") == 0) return CSS_FLEX_WRAP_WRAP;
    if (strcasecmp(value, "wrap-reverse") == 0) return CSS_FLEX_WRAP_WRAP_REVERSE;
    return CSS_FLEX_WRAP_NOWRAP;
}

static CssJustifyContent css_parse_justify_content(const char *value) {
    if (!value) return CSS_JUSTIFY_FLEX_START;
    if (strcasecmp(value, "flex-start") == 0 || strcasecmp(value, "start") == 0) return CSS_JUSTIFY_FLEX_START;
    if (strcasecmp(value, "flex-end") == 0 || strcasecmp(value, "end") == 0) return CSS_JUSTIFY_FLEX_END;
    if (strcasecmp(value, "center") == 0) return CSS_JUSTIFY_CENTER;
    if (strcasecmp(value, "space-between") == 0) return CSS_JUSTIFY_SPACE_BETWEEN;
    if (strcasecmp(value, "space-around") == 0) return CSS_JUSTIFY_SPACE_AROUND;
    if (strcasecmp(value, "space-evenly") == 0) return CSS_JUSTIFY_SPACE_EVENLY;
    return CSS_JUSTIFY_FLEX_START;
}

static CssAlignItems css_parse_align_items(const char *value) {
    if (!value) return CSS_ALIGN_STRETCH;
    if (strcasecmp(value, "stretch") == 0) return CSS_ALIGN_STRETCH;
    if (strcasecmp(value, "flex-start") == 0 || strcasecmp(value, "start") == 0 ||
        strcasecmp(value, "self-start") == 0 || strstr(value, "baseline")) return CSS_ALIGN_FLEX_START;
    if (strcasecmp(value, "flex-end") == 0 || strcasecmp(value, "end") == 0 ||
        strcasecmp(value, "self-end") == 0) return CSS_ALIGN_FLEX_END;
    if (strcasecmp(value, "center") == 0) return CSS_ALIGN_CENTER;
    return CSS_ALIGN_STRETCH;
}

static bool css_parse_auto_length(const char *value, double parent_value, double viewport_value, double font_size, double *out)
{
    if (!value || !*value) return false;
    if (strcasecmp(value, "auto") == 0) return false;
    *out = css_parse_length(value, parent_value, viewport_value, font_size);
    return true;
}

static void css_parse_shorthand_1_or_2(const char *value, double parent_value, double viewport_value,
                                       double font_size, double *out1, double *out2)
{
    *out1 = *out2 = 0.0;
    if (!value || !*value) return;
    char *copy = strdup(value);
    char *save = NULL;
    char *tok = strtok_r(copy, " \t", &save);
    if (tok) {
        *out1 = css_parse_length(tok, parent_value, viewport_value, font_size);
        tok = strtok_r(NULL, " \t", &save);
        if (tok) {
            *out2 = css_parse_length(tok, parent_value, viewport_value, font_size);
        } else {
            *out2 = *out1;
        }
    }
    free(copy);
}

/* Parse the flex shorthand: none | [ <'flex-grow'> <'flex-shrink'>? || <'flex-basis'> ] */
/* A flex-basis value: auto, a length, a percentage or a calc() mix.  The
 * percentage part resolves against the flex container at layout time. */
static void css_parse_flex_basis(const char *tok, double *basis, double *basis_percent,
                                 double *basis_pct_px, double font_size)
{
    *basis_percent = 0.0;
    *basis_pct_px = 0.0;
    if (strcasecmp(tok, "auto") == 0 || strcasecmp(tok, "content") == 0) { *basis = -1.0; return; }
    CssLen l = css_parse_len(tok, font_size);
    if (l.pct > 0.0) {
        *basis_percent = l.pct;
        *basis_pct_px = l.px;
        *basis = -1.0;
    } else {
        *basis = l.px;
    }
}

static void css_parse_flex_shorthand(const char *value, double *grow, double *shrink, double *basis,
                                     double *basis_percent, double *basis_pct_px, double font_size)
{
    *grow = 1.0; *shrink = 1.0; *basis = 0.0; *basis_percent = 0.0; *basis_pct_px = 0.0;
    if (!value || !*value) return;
    if (strcasecmp(value, "none") == 0) { *grow = 0.0; *shrink = 0.0; *basis = -1.0; return; }
    char toks[3][128];
    int n = css_split_values(value, toks, 3);
    int numbers = 0;   /* grow, then shrink: plain numbers only */
    for (int i = 0; i < n; i++) {
        char *end = NULL;
        double num = strtod(toks[i], &end);
        bool plain_number = end != toks[i] && *end == '\0';
        if (plain_number && numbers < 2) {
            if (numbers == 0) *grow = num;
            else *shrink = num;
            numbers++;
        } else {
            css_parse_flex_basis(toks[i], basis, basis_percent, basis_pct_px, font_size);
        }
    }
}

/* Block-level formatting context: these generate a block box that stacks
 * vertically with siblings.  Inline-block is sized like a block but flows
 * on a line with other inline content. */
static bool layout_is_block_flow(CssDisplay display);
/* Is the used height auto (content-sized)? */
static inline bool layout_height_auto(const LayoutBox *b) {
    return !b->height_set || b->height_pct_unresolved;
}
/* Block-level box taking part in block flow (not an atomic inline). */
static bool layout_box_is_block_level(const LayoutBox *box) {
    return layout_is_block_flow(box->display) && !box->inline_level;
}
static bool layout_box_is_atomic_inline(const LayoutBox *box) {
    return box->display == CSS_DISPLAY_INLINE_BLOCK || box->inline_level;
}

static bool layout_is_block_flow(CssDisplay display) {
    /* Treat unknown/legacy display values as block-like so that custom elements
     * and future display types do not collapse into inline 80x20 placeholders. */
    return display == CSS_DISPLAY_BLOCK ||
           display == CSS_DISPLAY_FLEX ||
           display == CSS_DISPLAY_GRID ||
           display == CSS_DISPLAY_LIST_ITEM ||
           display == CSS_DISPLAY_OTHER;
}

static bool css_parse_url_value(const char *value, char *out, size_t out_size)
{
    out[0] = '\0';
    if (!value || !*value) return false;
    while (*value && isspace((unsigned char)*value)) value++;
    if (strncasecmp(value, "url(", 4) != 0) {
        if (strcasecmp(value, "none") == 0) return true;
        return false;
    }
    value += 4;
    while (*value && isspace((unsigned char)*value)) value++;
    char quote = 0;
    if (*value == '"' || *value == '\'') { quote = *value; value++; }
    const char *end = value;
    if (quote) {
        while (*end && *end != quote) end++;
    } else {
        while (*end && *end != ')' && !isspace((unsigned char)*end)) end++;
    }
    size_t len = (size_t)(end - value);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, value, len);
    out[len] = '\0';
    return true;
}

static bool css_border_style_keyword(const char *t)
{
    static const char *kw[] = { "none", "hidden", "solid", "dashed", "dotted", "double",
                                "groove", "ridge", "inset", "outset", NULL };
    for (int i = 0; kw[i]; i++) if (strcasecmp(t, kw[i]) == 0) return true;
    return false;
}

static bool css_border_style_visible(const char *t)
{
    return css_border_style_keyword(t) && strcasecmp(t, "none") != 0 && strcasecmp(t, "hidden") != 0;
}

static double css_border_width_value(const char *v, double pw, double vw, double fs)
{
    if (strcasecmp(v, "thin") == 0) return 1.0;
    if (strcasecmp(v, "medium") == 0) return 3.0;
    if (strcasecmp(v, "thick") == 0) return 5.0;
    return css_parse_length(v, pw, vw, fs);
}

/* transform: keep the translation (translate/translateX/translateY/
 * translate3d/matrix); other functions are ignored. */
static void css_parse_transform(LayoutBox *box, const char *v, double pw, double vw)
{
    box->translate_x = box->translate_y = 0.0;
    box->translate_xpct = box->translate_ypct = 0.0;
    box->has_translate = 0;
    const char *p = v;
    while (p && *p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        const char *name = p;
        while (*p && *p != '(' && !isspace((unsigned char)*p)) p++;
        size_t nl = (size_t)(p - name);
        if (*p != '(') break;
        p++;
        const char *args = p;
        int depth = 1;
        while (*p && depth > 0) { if (*p == '(') depth++; else if (*p == ')') depth--; if (depth) p++; }
        size_t al = (size_t)(p - args);
        if (*p == ')') p++;
        char buf[256];
        if (al >= sizeof(buf)) continue;
        memcpy(buf, args, al);
        buf[al] = '\0';
        char *parts[6]; int np = 0;
        char *save = NULL;
        for (char *t = strtok_r(buf, ",", &save); t && np < 6; t = strtok_r(NULL, ",", &save)) {
            while (*t && isspace((unsigned char)*t)) t++;
            char *e = t + strlen(t);
            while (e > t && isspace((unsigned char)e[-1])) *--e = '\0';
            parts[np++] = t;
        }
        #define TR_ADD(str, px, pct) do { \
            if (css_value_is_percent(str)) (pct) += css_parse_percent_ratio(str); \
            else (px) += css_parse_length(str, pw, vw, box->font_size); } while (0)
        if ((nl == 9 && strncasecmp(name, "translate", 9) == 0) ||
            (nl == 11 && strncasecmp(name, "translate3d", 11) == 0)) {
            if (np >= 1) TR_ADD(parts[0], box->translate_x, box->translate_xpct);
            if (np >= 2) TR_ADD(parts[1], box->translate_y, box->translate_ypct);
        } else if (nl == 10 && strncasecmp(name, "translateX", 10) == 0) {
            if (np >= 1) TR_ADD(parts[0], box->translate_x, box->translate_xpct);
        } else if (nl == 10 && strncasecmp(name, "translateY", 10) == 0) {
            if (np >= 1) TR_ADD(parts[0], box->translate_y, box->translate_ypct);
        } else if (nl == 6 && strncasecmp(name, "matrix", 6) == 0 && np == 6) {
            box->translate_x += strtod(parts[4], NULL);
            box->translate_y += strtod(parts[5], NULL);
        }
        #undef TR_ADD
    }
    box->has_translate = (box->translate_x != 0.0 || box->translate_y != 0.0 ||
                          box->translate_xpct != 0.0 || box->translate_ypct != 0.0);
}

/* top/right/bottom/left (side 0..3): auto clears the side; percentages
 * resolve later against the containing block. */
static void layout_set_inset(LayoutBox *box, int side, const char *value,
                             double pw, double vw)
{
    static const uint32_t bits[4] = { LAYOUT_SIDE_TOP, LAYOUT_SIDE_RIGHT, LAYOUT_SIDE_BOTTOM, LAYOUT_SIDE_LEFT };
    double *dst[4] = { &box->top, &box->right, &box->bottom, &box->left };
    box->inset_pct_bits &= (unsigned char)~(1u << side);
    if (strcasecmp(value, "auto") == 0) {
        *dst[side] = 0.0;
        box->positioned_sides &= ~bits[side];
        return;
    }
    if (css_value_is_percent(value)) {
        box->inset_pct[side] = css_parse_percent_ratio(value);
        box->inset_pct_bits |= (unsigned char)(1u << side);
        *dst[side] = 0.0;
    } else {
        *dst[side] = css_parse_length(value, pw, vw, box->font_size);
    }
    box->positioned_sides |= bits[side];
}

/* Resolve the used insets against a containing block. */
static void layout_resolve_insets(LayoutBox *box, double cb_w, double cb_h)
{
    if (box->inset_pct_bits & 1) box->top = box->inset_pct[0] * cb_h;
    if (box->inset_pct_bits & 2) box->right = box->inset_pct[1] * cb_w;
    if (box->inset_pct_bits & 4) box->bottom = box->inset_pct[2] * cb_h;
    if (box->inset_pct_bits & 8) box->left = box->inset_pct[3] * cb_w;
}

static void layout_apply_declaration(LayoutBox *box, const CssDeclaration *decl,
                                     double parent_width, double viewport_width,
                                     const char *base_url)
{
    const char *prop = decl->property;
    const char *value = decl->value;
    if (!prop || !value) return;

    if (strcasecmp(prop, "display") == 0) {
        box->display = css_parse_display(value);
        box->inline_level = css_display_is_inline_level(value) ? 1 : 0;
        box->webkit_box = (strcasecmp(value, "-webkit-box") == 0 || strcasecmp(value, "box") == 0) ? 1 :
                          strcasecmp(value, "-webkit-inline-box") == 0 ? 2 : 0;
        if (strcasecmp(value, "inline-table") == 0) box->display = CSS_DISPLAY_BLOCK;
    } else if (strcasecmp(prop, "visibility") == 0) {
        box->visibility = css_parse_visibility(value);
        box->visibility_set = 1;
    } else if (strcasecmp(prop, "clip") == 0) {
        /* The "visually hidden" pattern: clip:rect() with ~zero area
         * (e.g. clip:rect(1px,1px,1px,1px)).  Hide the box; descendants
         * inherit visibility. */
        const char *p = strstr(value, "rect(");
        if (p) {
            double v[4] = {0, 0, 0, 0};
            int n = 0;
            p += 5;
            while (*p && n < 4) {
                while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
                char *end = NULL;
                double num = strtod(p, &end);
                if (end == p) break;
                v[n++] = num;
                p = end;
                while (*p && (isalpha((unsigned char)*p) || *p == '%')) p++; /* unit */
            }
            if (n == 4 && (v[2] - v[0]) <= 1.0 && (v[3] - v[1]) <= 1.0) {
                box->visibility = CSS_VISIBILITY_HIDDEN;
                box->visibility_set = 1;
            }
        }
    } else if (strcasecmp(prop, "opacity") == 0) {
        char *endp = NULL;
        double op = strtod(value, &endp);
        if (endp != value) {            /* invalid values are ignored */
            if (*endp == '%') op /= 100.0;
            if (op < 0.0) op = 0.0;
            if (op > 1.0) op = 1.0;
            box->opacity = op;
            box->opacity_set = 1;
        }
    } else if (strcasecmp(prop, "position") == 0) {
        box->position = css_parse_position(value);
    } else if (strcasecmp(prop, "box-sizing") == 0) {
        box->box_sizing = css_parse_box_sizing(value);
    } else if (strcasecmp(prop, "width") == 0) {
        box->width_keyword = 0;
        if (strstr(value, "min-content")) box->width_keyword = 1;
        else if (strstr(value, "max-content")) box->width_keyword = 2;
        else if (strstr(value, "fit-content")) box->width_keyword = 3;
        if (box->width_keyword) {
            box->width_set = 0;
            box->width_percent = 0.0;
            box->css_width = 0.0;
        } else {
            /* A percentage part (50%, calc(100% - 20px)) resolves against
             * the containing block at layout time. */
            CssLen l = css_parse_len(value, box->font_size);
            box->width_percent = l.pct > 0.0 ? l.pct : 0.0;
            box->width_pct_px = l.pct > 0.0 ? l.px : 0.0;
            box->css_width = l.pct > 0.0 ? 0.0 : l.px;
        }
        if (!box->width_keyword) box->width_set = (strcasecmp(value, "auto") != 0);
    } else if (strcasecmp(prop, "height") == 0) {
        if (strstr(value, "-content")) {
            box->height_set = 0;
            box->height_percent = 0.0;
            box->css_height = 0.0;
            return;
        }
        {
            CssLen l = css_parse_len(value, box->font_size);
            box->height_percent = l.pct > 0.0 ? l.pct : 0.0;
            box->height_pct_px = l.pct > 0.0 ? l.px : 0.0;
            box->css_height = l.pct > 0.0 ? 0.0 : l.px;
        }
        box->height_set = (strcasecmp(value, "auto") != 0);
    } else if (strcasecmp(prop, "min-width") == 0) {
        box->min_width = layout_note_minmax(box, 0, value);
        if (strcasecmp(value, "auto") == 0) box->min_size_set &= (unsigned char)~1;
        else box->min_size_set |= 1;
    } else if (strcasecmp(prop, "max-width") == 0) {
        box->max_width = layout_note_minmax(box, 1, value);
    } else if (strcasecmp(prop, "min-height") == 0) {
        box->min_height = layout_note_minmax(box, 2, value);
        if (strcasecmp(value, "auto") == 0) box->min_size_set &= (unsigned char)~2;
        else box->min_size_set |= 2;
    } else if (strcasecmp(prop, "max-height") == 0) {
        box->max_height = layout_note_minmax(box, 3, value);
    } else if (strcasecmp(prop, "top") == 0) {
        layout_set_inset(box, 0, value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "right") == 0) {
        layout_set_inset(box, 1, value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "bottom") == 0) {
        layout_set_inset(box, 2, value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "left") == 0) {
        layout_set_inset(box, 3, value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "inset") == 0) {
        char vals[4][64]; int n = 0;
        const char *p = value;
        while (*p && n < 4) {
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;
            size_t tl = 0;
            while (p[tl] && !isspace((unsigned char)p[tl])) tl++;
            size_t cl = tl < 63 ? tl : 63;
            memcpy(vals[n], p, cl); vals[n][cl] = '\0';
            n++; p += tl;
        }
        if (n > 0) {
            int map[4][4] = { {0,0,0,0}, {0,1,0,1}, {0,1,2,1}, {0,1,2,3} };
            for (int side = 0; side < 4; side++)
                layout_set_inset(box, side, vals[map[n - 1][side]], parent_width, viewport_width);
        }
    } else if (strcasecmp(prop, "margin") == 0) {
        double em4[4];
        unsigned fl = layout_apply_shorthand_sides(box, value, parent_width, viewport_width,
                                                   &box->margin_top, &box->margin_right,
                                                   &box->margin_bottom, &box->margin_left, em4);
        layout_note_side_pcts(box, value, false, -1);
        box->em_deferred &= 0xF0; /* shorthand resets all four margin sides */
        box->margin_auto = css_margin_auto_mask(value);
        for (int i = 0; i < 4; i++) {
            if (fl & (1u << i)) {
                box->margin_em[i] = em4[i];
                box->em_deferred |= (unsigned char)(1u << i);
            }
        }
    } else if (strcasecmp(prop, "margin-left") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->margin_em[3] = r; box->em_deferred |= 0x08; }
        else box->em_deferred &= (unsigned char)~0x08;
        box->margin_left = css_parse_length(value, parent_width, viewport_width, box->font_size);
        layout_note_side_pcts(box, value, false, 3);
        if (strcasecmp(value, "auto") == 0) box->margin_auto |= 8;
        else box->margin_auto &= (unsigned char)~8;
    } else if (strcasecmp(prop, "margin-right") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->margin_em[1] = r; box->em_deferred |= 0x02; }
        else box->em_deferred &= (unsigned char)~0x02;
        box->margin_right = css_parse_length(value, parent_width, viewport_width, box->font_size);
        layout_note_side_pcts(box, value, false, 1);
        if (strcasecmp(value, "auto") == 0) box->margin_auto |= 2;
        else box->margin_auto &= (unsigned char)~2;
    } else if (strcasecmp(prop, "margin-top") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->margin_em[0] = r; box->em_deferred |= 0x01; }
        else box->em_deferred &= (unsigned char)~0x01;
        box->margin_top = css_parse_length(value, parent_width, viewport_width, box->font_size);
        layout_note_side_pcts(box, value, false, 0);
        if (strcasecmp(value, "auto") == 0) box->margin_auto |= 1;
        else box->margin_auto &= (unsigned char)~1;
    } else if (strcasecmp(prop, "margin-bottom") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->margin_em[2] = r; box->em_deferred |= 0x04; }
        else box->em_deferred &= (unsigned char)~0x04;
        box->margin_bottom = css_parse_length(value, parent_width, viewport_width, box->font_size);
        layout_note_side_pcts(box, value, false, 2);
        if (strcasecmp(value, "auto") == 0) box->margin_auto |= 4;
        else box->margin_auto &= (unsigned char)~4;
    } else if (strcasecmp(prop, "aspect-ratio") == 0) {
        /* auto | <ratio> | auto && <ratio>; <ratio> = <number> [ / <number> ].
         * The "auto" fallback for replaced content is not modelled. */
        double w = 0.0, h = 1.0;
        const char *p = value;
        while (*p && !(isdigit((unsigned char)*p) || *p == '.')) p++;
        if (*p) {
            char *end = NULL;
            w = strtod(p, &end);
            while (end && (*end == ' ' || *end == '\t')) end++;
            if (end && *end == '/') {
                h = strtod(end + 1, NULL);
            }
        }
        box->aspect_ratio = (w > 0.0 && h > 0.0) ? h / w : 0.0;
        box->aspect_ratio_prop = box->aspect_ratio > 0.0;
    } else if (strcasecmp(prop, "padding") == 0) {
        double em4[4];
        unsigned fl = layout_apply_shorthand_sides(box, value, parent_width, viewport_width,
                                                   &box->padding_top, &box->padding_right,
                                                   &box->padding_bottom, &box->padding_left, em4);
        layout_note_side_pcts(box, value, true, -1);
        box->em_deferred &= 0x0F; /* shorthand resets all four padding sides */
        for (int i = 0; i < 4; i++) {
            if (fl & (1u << i)) {
                box->padding_em[i] = em4[i];
                box->em_deferred |= (unsigned char)(0x10u << i);
            }
        }
    } else if (strcasecmp(prop, "padding-left") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->padding_em[3] = r; box->em_deferred |= 0x80; }
        else box->em_deferred &= (unsigned char)~0x80;
        box->padding_left = css_parse_length(value, parent_width, viewport_width, box->font_size);
        layout_note_side_pcts(box, value, true, 3);
    } else if (strcasecmp(prop, "padding-right") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->padding_em[1] = r; box->em_deferred |= 0x20; }
        else box->em_deferred &= (unsigned char)~0x20;
        box->padding_right = css_parse_length(value, parent_width, viewport_width, box->font_size);
        layout_note_side_pcts(box, value, true, 1);
    } else if (strcasecmp(prop, "padding-top") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->padding_em[0] = r; box->em_deferred |= 0x10; }
        else box->em_deferred &= (unsigned char)~0x10;
        box->padding_top = css_parse_length(value, parent_width, viewport_width, box->font_size);
        layout_note_side_pcts(box, value, true, 0);
    } else if (strcasecmp(prop, "padding-bottom") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->padding_em[2] = r; box->em_deferred |= 0x40; }
        else box->em_deferred &= (unsigned char)~0x40;
        box->padding_bottom = css_parse_length(value, parent_width, viewport_width, box->font_size);
        layout_note_side_pcts(box, value, true, 2);
    } else if (strcasecmp(prop, "border-left-width") == 0) {
        box->border_left = css_border_width_value(value, parent_width, viewport_width, box->font_size);
        box->border_width_set |= 1;
    } else if (strcasecmp(prop, "border-left-style") == 0) {
        if (css_border_style_visible(value)) box->border_styled |= 1;
        else box->border_styled &= (unsigned char)~1;
    } else if (strcasecmp(prop, "border-right-width") == 0) {
        box->border_right = css_border_width_value(value, parent_width, viewport_width, box->font_size);
        box->border_width_set |= 2;
    } else if (strcasecmp(prop, "border-right-style") == 0) {
        if (css_border_style_visible(value)) box->border_styled |= 2;
        else box->border_styled &= (unsigned char)~2;
    } else if (strcasecmp(prop, "border-top-width") == 0) {
        box->border_top = css_border_width_value(value, parent_width, viewport_width, box->font_size);
        box->border_width_set |= 4;
    } else if (strcasecmp(prop, "border-top-style") == 0) {
        if (css_border_style_visible(value)) box->border_styled |= 4;
        else box->border_styled &= (unsigned char)~4;
    } else if (strcasecmp(prop, "border-bottom-width") == 0) {
        box->border_bottom = css_border_width_value(value, parent_width, viewport_width, box->font_size);
        box->border_width_set |= 8;
    } else if (strcasecmp(prop, "border-bottom-style") == 0) {
        if (css_border_style_visible(value)) box->border_styled |= 8;
        else box->border_styled &= (unsigned char)~8;
    } else if (strcasecmp(prop, "flex-direction") == 0) {
        box->flex_direction = css_parse_flex_direction(value);
    } else if (strcasecmp(prop, "flex-wrap") == 0) {
        box->flex_wrap = css_parse_flex_wrap(value);
    } else if (strcasecmp(prop, "justify-content") == 0) {
        box->justify_content = css_parse_justify_content(value);
    } else if (strcasecmp(prop, "align-items") == 0) {
        box->align_items = css_parse_align_items(value);
    } else if (strcasecmp(prop, "transform") == 0 || strcasecmp(prop, "-webkit-transform") == 0) {
        css_parse_transform(box, value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "white-space") == 0 || strcasecmp(prop, "white-space-collapse") == 0) {
        unsigned char ws = 0;
        if (strcasecmp(value, "nowrap") == 0) ws = 1;
        else if (strcasecmp(value, "pre") == 0) ws = 2;
        else if (strcasecmp(value, "pre-wrap") == 0 || strcasecmp(value, "break-spaces") == 0 ||
                 strcasecmp(value, "preserve") == 0) ws = 3;
        else if (strcasecmp(value, "pre-line") == 0 || strcasecmp(value, "preserve-breaks") == 0) ws = 4;
        box->white_space = ws;
        box->white_space_set = 1;
    } else if (strcasecmp(prop, "-webkit-box-orient") == 0 || strcasecmp(prop, "box-orient") == 0) {
        box->box_orient_vertical = (strcasecmp(value, "vertical") == 0 ||
                                    strcasecmp(value, "block-axis") == 0) ? 1 : 0;
    } else if (strcasecmp(prop, "-webkit-line-clamp") == 0 || strcasecmp(prop, "line-clamp") == 0) {
        int n = atoi(value);
        box->line_clamp = (unsigned char)(n > 0 && n < 255 ? n : 0);
    } else if (strcasecmp(prop, "object-fit") == 0) {
        box->object_fit = strcasecmp(value, "contain") == 0 ? 1 :
                          strcasecmp(value, "cover") == 0 ? 2 :
                          strcasecmp(value, "none") == 0 ? 3 :
                          strcasecmp(value, "scale-down") == 0 ? 4 : 0;
    } else if (strcasecmp(prop, "align-self") == 0) {
        box->align_self = (strcasecmp(value, "auto") == 0 || strcasecmp(value, "normal") == 0)
            ? 0 : (unsigned char)(css_parse_align_items(value) + 1);
    } else if (strcasecmp(prop, "order") == 0) {
        box->order = atoi(value);
    } else if (strcasecmp(prop, "flex-basis") == 0) {
        css_parse_flex_basis(value, &box->flex_basis, &box->flex_basis_percent,
                             &box->flex_basis_pct_px, box->font_size);
    } else if (strcasecmp(prop, "flex-grow") == 0) {
        box->flex_grow = strtod(value, NULL);
    } else if (strcasecmp(prop, "flex-shrink") == 0) {
        box->flex_shrink = strtod(value, NULL);
    } else if (strcasecmp(prop, "flex") == 0) {
        css_parse_flex_shorthand(value, &box->flex_grow, &box->flex_shrink, &box->flex_basis,
                                 &box->flex_basis_percent, &box->flex_basis_pct_px, box->font_size);
    } else if (strcasecmp(prop, "gap") == 0) {
        css_parse_shorthand_1_or_2(value, parent_width, viewport_width,
                                   box->font_size, &box->gap_row, &box->gap_col);
    } else if (strcasecmp(prop, "row-gap") == 0) {
        box->gap_row = css_parse_length(value, parent_width, viewport_width, box->font_size);
    } else if (strcasecmp(prop, "column-gap") == 0) {
        box->gap_col = css_parse_length(value, parent_width, viewport_width, box->font_size);
    } else if (strcasecmp(prop, "fill") == 0) {
        while (isspace((unsigned char)*value)) value++;
        if (strncasecmp(value, "currentcolor", 12) == 0) { box->fill_kind = 1; box->fill_set = 1; }
        else if (strncasecmp(value, "none", 4) == 0) { box->fill_kind = 3; box->fill_set = 1; }
        else if (css_parse_color(value, &box->fill_r, &box->fill_g, &box->fill_b, &box->fill_a)) {
            box->fill_kind = 2; box->fill_set = 1;
        }
    } else if (strcasecmp(prop, "color") == 0) {
        if (css_parse_color(value, &box->color_r, &box->color_g, &box->color_b, &box->color_a))
            box->color_set = 1;
    } else if (strcasecmp(prop, "text-align") == 0) {
        if (strncasecmp(value, "center", 6) == 0) box->text_align = CSS_TEXT_ALIGN_CENTER;
        else if (strncasecmp(value, "right", 5) == 0) box->text_align = CSS_TEXT_ALIGN_RIGHT;
        else box->text_align = CSS_TEXT_ALIGN_LEFT; /* left/start/justify */
        box->text_align_set = 1;
    } else if (strcasecmp(prop, "float") == 0) {
        if (strcasecmp(value, "left") == 0) box->float_side = 1;
        else if (strcasecmp(value, "right") == 0) box->float_side = 2;
        else box->float_side = 0;
    } else if (strcasecmp(prop, "clear") == 0) {
        if (strcasecmp(value, "left") == 0) box->clear = 1;
        else if (strcasecmp(value, "right") == 0) box->clear = 2;
        else if (strcasecmp(value, "both") == 0) box->clear = 3;
        else box->clear = 0;
    } else if (strcasecmp(prop, "overflow") == 0 ||
               strcasecmp(prop, "overflow-x") == 0 ||
               strcasecmp(prop, "overflow-y") == 0) {
        /* Any non-visible overflow establishes a new block formatting
         * context.  The axis distinction does not matter for margin
         * collapsing / float containment, so a single slot suffices. */
        if (strcasecmp(value, "visible") == 0) {
            /* For shorthand semantics: overflow:visible on one axis paired
             * with a non-visible value on the other computes to auto, but
             * tracking per-axis is overkill here. */
            if (strcasecmp(prop, "overflow") == 0) box->overflow = 0;
        }
        else if (strcasecmp(value, "hidden") == 0) box->overflow = 1;
        else if (strcasecmp(value, "auto") == 0) box->overflow = 2;
        else if (strcasecmp(value, "scroll") == 0) box->overflow = 3;
        else if (strcasecmp(value, "clip") == 0) box->overflow = 1;
    } else if (strcasecmp(prop, "vertical-align") == 0) {
        if (strcasecmp(value, "middle") == 0) box->vertical_align = 1;
        else if (strcasecmp(value, "top") == 0) box->vertical_align = 2;
        else if (strcasecmp(value, "bottom") == 0) box->vertical_align = 3;
        else box->vertical_align = 0;   /* baseline/sub/super/lengths: treat as baseline */
    } else if (strcasecmp(prop, "background-color") == 0) {
        double cr, cg, cb, ca;
        if (css_parse_color(value, &cr, &cg, &cb, &ca)) {
            box->background_color_r = cr; box->background_color_g = cg;
            box->background_color_b = cb; box->background_color_a = ca;
        }
    } else if (strcasecmp(prop, "background") == 0) {
        /* Shorthand: pull out the color and/or url() parts we support and
         * ignore the rest (position/repeat/size/attachment/origin).  Parse
         * colors into temporaries: css_parse_color clobbers on failure. */
        const char *p = value;
        char token[1024];
        /* The shorthand resets omitted longhands: transparent, no image. */
        box->background_color_r = box->background_color_g = box->background_color_b = 0.0;
        box->background_color_a = 0.0;
        box->background_image_url[0] = '\0';
        while (*p) {
            while (*p && (isspace((unsigned char)*p) || *p == '/')) p++;
            if (!*p) break;
            size_t tl = 0;
            int depth = 0;
            while (p[tl] && (depth > 0 || (!isspace((unsigned char)p[tl]) && p[tl] != '/'))) {
                if (p[tl] == '(') depth++;
                else if (p[tl] == ')' && depth > 0) depth--;
                tl++;
            }
            if (tl >= sizeof(token)) tl = sizeof(token) - 1;
            memcpy(token, p, tl);
            token[tl] = '\0';
            p += tl;
            if (strncasecmp(token, "url(", 4) == 0) {
                char url[1024];
                if (css_parse_url_value(token, url, sizeof(url)) && url[0]) {
                    char *abs = layout_resolve_url(base_url, url);
                    if (abs) {
                        snprintf(box->background_image_url, sizeof(box->background_image_url), "%s", abs);
                        free(abs);
                    }
                }
            } else {
                double cr, cg, cb, ca;
                if (css_parse_color(token, &cr, &cg, &cb, &ca)) {
                    box->background_color_r = cr; box->background_color_g = cg;
                    box->background_color_b = cb; box->background_color_a = ca;
                }
            }
        }
    } else if (strcasecmp(prop, "background-image") == 0) {
        char url[1024];
        if (css_parse_url_value(value, url, sizeof(url)) && url[0]) {
            char *abs = layout_resolve_url(base_url, url);
            if (abs) {
                snprintf(box->background_image_url, sizeof(box->background_image_url), "%s", abs);
                free(abs);
            }
        } else {
            box->background_image_url[0] = '\0';
        }
    } else if (strcasecmp(prop, "font-size") == 0) {
        /* Absolute-size keywords (CSS2 table for 16px medium). */
        static const struct { const char *kw; double px; } kw_sizes[] = {
            { "xx-small", 9.0 }, { "x-small", 10.0 }, { "small", 13.33 },
            { "medium", 16.0 }, { "large", 18.0 }, { "x-large", 24.0 },
            { "xx-large", 32.0 }, { NULL, 0.0 }
        };
        bool kw_done = false;
        for (int k = 0; kw_sizes[k].kw; k++) {
            if (strcasecmp(value, kw_sizes[k].kw) == 0) {
                box->font_size = kw_sizes[k].px;
                box->font_size_ratio = 0.0;
                box->font_size_set = 1;
                kw_done = true;
                break;
            }
        }
        if (kw_done) {
            /* done */
        } else if (strcasecmp(value, "smaller") == 0) {
            box->font_size_ratio = 0.833;
            box->font_size = 16.0 * 0.833; /* provisional */
            box->font_size_set = 1;
        } else if (strcasecmp(value, "larger") == 0) {
            box->font_size_ratio = 1.2;
            box->font_size = 16.0 * 1.2; /* provisional */
            box->font_size_set = 1;
        } else if (strcasecmp(value, "inherit") == 0) {
            /* Clear the flag so pass-3 inheritance fills the value in. */
            box->font_size_set = 0;
            box->font_size_ratio = 0.0;
        } else if (strcasecmp(value, "initial") == 0) {
            box->font_size = 16.0;
            box->font_size_ratio = 0.0;
            box->font_size_set = 1;
        /* Percentage and em font-sizes are relative to the PARENT's computed
         * font-size, not to the containing-block width.  Record the ratio and
         * resolve it in a serial preorder pass after the (parallel) apply. */
        } else if (css_value_is_percent(value)) {
            box->font_size_ratio = css_parse_percent_ratio(value);
            box->font_size = 16.0 * box->font_size_ratio; /* provisional */
            box->font_size_set = 1;
        } else {
            const char *p = value;
            while (*p && isspace((unsigned char)*p)) p++;
            char *end = NULL;
            double num = strtod(p, &end);
            if (end != p && strcasecmp(end, "em") == 0) {
                box->font_size_ratio = num;
                box->font_size = 16.0 * num; /* provisional */
            } else {
                box->font_size_ratio = 0.0;
                box->font_size = css_parse_length(value, parent_width, viewport_width, box->font_size);
            }
            if (box->font_size <= 0.0) box->font_size = 16.0;
            box->font_size_set = 1;
        }
    } else if (strcasecmp(prop, "font-family") == 0) {
        /* Keep only the first comma-separated family name, stripped of quotes and whitespace. */
        const char *p = value;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '"' || *p == '\'') p++;
        size_t len = 0;
        while (p[len] && p[len] != ',' && p[len] != '"' && p[len] != '\'' && !isspace((unsigned char)p[len])) len++;
        if (len >= sizeof(box->font_family)) len = sizeof(box->font_family) - 1;
        memcpy(box->font_family, p, len);
        box->font_family[len] = '\0';
    } else if (strcasecmp(prop, "font-weight") == 0) {
        int w = 400;
        if (strncasecmp(value, "bold", 4) == 0) w = 700;
        else if (strncasecmp(value, "normal", 6) == 0) w = 400;
        else if (strncasecmp(value, "bolder", 6) == 0) w = 700;
        else if (strncasecmp(value, "lighter", 7) == 0) w = 400;
        else {
            int n = atoi(value);
            if (n >= 100 && n <= 900) w = n;
        }
        box->font_weight = w;
        box->font_weight_set = 1;
    } else if (strcasecmp(prop, "font-style") == 0) {
        box->font_italic = (strncasecmp(value, "italic", 6) == 0 ||
                            strncasecmp(value, "oblique", 7) == 0) ? 1 : 0;
        box->font_italic_set = 1;
    } else if (strcasecmp(prop, "text-decoration") == 0 ||
               strcasecmp(prop, "text-decoration-line") == 0) {
        /* Token scan: underline / line-through / overline; "none" clears. */
        unsigned char deco = 0;
        if (strstr(value, "underline")) deco |= 1;
        if (strstr(value, "line-through")) deco |= 2;
        if (strstr(value, "overline")) deco |= 4;
        box->text_decoration = deco;
        box->text_decoration_set = 1;
    } else if (strcasecmp(prop, "line-height") == 0) {
        if (strncasecmp(value, "normal", 6) == 0) {
            box->line_height = 0.0; /* <=0 => normal (font metrics at layout) */
            box->line_height_ratio = 0.0;
        } else {
            char *end = NULL;
            double num = strtod(value, &end);
            if (end != value) {
                while (*end && isspace((unsigned char)*end)) end++;
                if (*end == '\0') {
                    /* Unitless multiplier of the element's own font-size:
                     * keep the ratio for inheritance/recompute. */
                    box->line_height = num * box->font_size;
                    box->line_height_ratio = num;
                } else if (css_value_is_percent(value)) {
                    box->line_height = css_parse_percent_ratio(value) * box->font_size;
                } else {
                    box->line_height = css_parse_length(value, parent_width, viewport_width,
                                                        box->font_size);
                }
            }
        }
        box->line_height_set = 1;
    } else if (strcasecmp(prop, "list-style-type") == 0 ||
               strcasecmp(prop, "list-style") == 0) {
        unsigned char t = 0;
        if (strstr(value, "disc")) t = 1;
        else if (strstr(value, "circle")) t = 2;
        else if (strstr(value, "square")) t = 3;
        else if (strstr(value, "decimal")) t = 4;
        else if (strstr(value, "none")) t = 0;
        box->list_style_type = t;
        box->list_style_type_set = 1;
    } else if (strcasecmp(prop, "border") == 0 ||
               strcasecmp(prop, "border-left") == 0 ||
               strcasecmp(prop, "border-right") == 0 ||
               strcasecmp(prop, "border-top") == 0 ||
               strcasecmp(prop, "border-bottom") == 0) {
        /* Shorthand: scan tokens for a width (number+unit) and a color.
         * Style keywords (solid/dashed/...) are accepted and ignored. */
        int sides = 0x0F;
        if (strstr(prop, "left")) sides = 0x01;
        else if (strstr(prop, "right")) sides = 0x02;
        else if (strstr(prop, "top")) sides = 0x04;
        else if (strstr(prop, "bottom")) sides = 0x08;
        const char *p = value;
        char token[256];
        /* The shorthand resets omitted parts: style none, width medium. */
        box->border_styled &= (unsigned char)~sides;
        box->border_width_set &= (unsigned char)~sides;
        while (*p) {
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;
            size_t tl = 0;
            while (p[tl] && !isspace((unsigned char)p[tl])) tl++;
            if (tl >= sizeof(token)) tl = sizeof(token) - 1;
            memcpy(token, p, tl);
            token[tl] = '\0';
            p += tl;
            if (css_border_style_keyword(token)) {
                if (css_border_style_visible(token)) box->border_styled |= (unsigned char)sides;
                continue;
            }
            double w = css_parse_length(token, parent_width, viewport_width, box->font_size);
            if (w >= 0.0 && (isdigit((unsigned char)token[0]) || token[0] == '.')) {
                box->border_width_set |= (unsigned char)sides;
                if (sides & 0x01) box->border_left = w;
                if (sides & 0x02) box->border_right = w;
                if (sides & 0x04) box->border_top = w;
                if (sides & 0x08) box->border_bottom = w;
                continue;
            }
            /* Border-width keywords. */
            double kw = 0.0;
            if (strcasecmp(token, "thin") == 0) kw = 1.0;
            else if (strcasecmp(token, "medium") == 0) kw = 3.0;
            else if (strcasecmp(token, "thick") == 0) kw = 5.0;
            if (kw > 0.0) {
                box->border_width_set |= (unsigned char)sides;
                if (sides & 0x01) box->border_left = kw;
                if (sides & 0x02) box->border_right = kw;
                if (sides & 0x04) box->border_top = kw;
                if (sides & 0x08) box->border_bottom = kw;
                continue;
            }
            double cr, cg, cb, ca;
            if (css_parse_color(token, &cr, &cg, &cb, &ca)) {
                box->border_color_r = cr; box->border_color_g = cg;
                box->border_color_b = cb; box->border_color_a = ca;
                box->border_color_set = 1;
            }
        }
    } else if (strcasecmp(prop, "border-width") == 0) {
        layout_apply_shorthand_sides(box, value, parent_width, viewport_width,
                                     &box->border_top, &box->border_right,
                                     &box->border_bottom, &box->border_left, NULL);
        box->border_width_set = 0x0F;
    } else if (strcasecmp(prop, "border-style") == 0) {
        /* 1-4 values: top, right, bottom, left. */
        int vis[4] = {0, 0, 0, 0}, n = 0;
        const char *p = value;
        char token[64];
        while (*p && n < 4) {
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;
            size_t tl = 0;
            while (p[tl] && !isspace((unsigned char)p[tl])) tl++;
            size_t cl = tl < sizeof(token) ? tl : sizeof(token) - 1;
            memcpy(token, p, cl);
            token[cl] = '\0';
            p += tl;
            vis[n++] = css_border_style_visible(token);
        }
        if (n > 0) {
            int t, r, b, l;
            if (n == 1) t = r = b = l = vis[0];
            else if (n == 2) { t = b = vis[0]; r = l = vis[1]; }
            else if (n == 3) { t = vis[0]; r = l = vis[1]; b = vis[2]; }
            else { t = vis[0]; r = vis[1]; b = vis[2]; l = vis[3]; }
            box->border_styled = (unsigned char)((l ? 1 : 0) | (r ? 2 : 0) | (t ? 4 : 0) | (b ? 8 : 0));
        }
    } else if (strcasecmp(prop, "border-color") == 0 ||
               strcasecmp(prop, "border-left-color") == 0 ||
               strcasecmp(prop, "border-right-color") == 0 ||
               strcasecmp(prop, "border-top-color") == 0 ||
               strcasecmp(prop, "border-bottom-color") == 0) {
        /* Single border color slot: first color token wins per declaration. */
        double cr, cg, cb, ca;
        if (css_parse_color(value, &cr, &cg, &cb, &ca)) {
            box->border_color_r = cr; box->border_color_g = cg;
            box->border_color_b = cb; box->border_color_a = ca;
            box->border_color_set = 1;
        }
    }
}

static void layout_apply_inline_style(LayoutBox *box, HtmlNode *node,
                                      double parent_width, double viewport_width,
                                      const char *base_url)
{
    for (HtmlAttribute *attr = node->attributes; attr; attr = attr->next) {
        if (strcasecmp(attr->name, "style") != 0) continue;
        int count = 0;
        CssDeclaration *decls = css_parse_inline_style(attr->value, &count);
        for (int i = 0; i < count; i++) {
            layout_apply_declaration(box, &decls[i], parent_width, viewport_width, base_url);
        }
        css_declarations_free(decls, count);
    }
}

/* ============================================================================
 * CSS custom properties (variables)
 * ============================================================================ */

/* Custom-property maps are immutable once a node's style is computed: a node
 * that sets no custom property shares its parent's map (copy-on-write). */
static const char* css_custom_props_get(const CssCustomProps *props, const char *name)
{
    if (!props || !name) return NULL;
    for (int i = 0; i < props->count; i++) {
        if (strcmp(props->props[i].name, name) == 0) return props->props[i].value;
    }
    return NULL;
}

/* New map holding `base`'s entries with room for `extra` more. */
static CssCustomProps *css_custom_props_fork(FrameArena *a, const CssCustomProps *base, int extra)
{
    int n = base ? base->count : 0;
    CssCustomProps *m = (CssCustomProps *)frame_arena_alloc(a, sizeof(CssCustomProps));
    m->props = (CssCustomProp *)frame_arena_alloc(a, (size_t)(n + extra) * sizeof(CssCustomProp));
    if (n) memcpy(m->props, base->props, (size_t)n * sizeof(CssCustomProp));
    m->count = n;
    m->capacity = n + extra;
    return m;
}

/* Replace or append (legacy css_custom_props_set semantics). */
static void css_custom_props_put(FrameArena *a, CssCustomProps *m, const char *name, const char *value)
{
    for (int i = 0; i < m->count; i++) {
        if (strcmp(m->props[i].name, name) == 0) {
            m->props[i].value = frame_arena_strdup(a, value);
            return;
        }
    }
    if (m->count >= m->capacity) return;   /* capacity = number of custom declarations */
    m->props[m->count].name = frame_arena_strdup(a, name);
    m->props[m->count].value = frame_arena_strdup(a, value);
    m->count++;
}

/* Resolve var(--name, fallback) references in a CSS value.  Looks up custom
 * properties in the supplied map and falls back to the comma-separated fallback
 * if the variable is not defined.  Returns a newly allocated string or NULL if
 * no var() references were found. */
/* Set when a var() had neither a value nor a fallback: the declaration is
 * invalid at computed-value time (the property behaves as `unset`). */
static __thread bool t_var_failed;

static char* css_var_resolve(const char *value, const CssCustomProps *props)
{
    if (!value || !strchr(value, '(')) return NULL;
    const char *p = value;
    const char *var = strstr(p, "var(");
    if (!var) return NULL;

    char *out = (char*)malloc(strlen(value) + 1);
    if (!out) return NULL;
    size_t out_len = 0;

    while (*p) {
        var = strstr(p, "var(");
        if (!var) {
            size_t rest = strlen(p);
            memcpy(out + out_len, p, rest);
            out_len += rest;
            break;
        }

        /* Copy literal prefix. */
        size_t prefix = (size_t)(var - p);
        if (out_len + prefix >= strlen(value) + 1) break;
        memcpy(out + out_len, p, prefix);
        out_len += prefix;

        /* Parse var(...) contents. */
        p = var + 4;
        while (*p && isspace((unsigned char)*p)) p++;
        const char *name_start = p;
        while (*p && *p != ',' && *p != ')') p++;
        const char *name_end = p;
        while (name_end > name_start && isspace((unsigned char)name_end[-1])) name_end--;

        const char *fallback = NULL;
        const char *fallback_end = NULL;
        if (*p == ',') {
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
            fallback = p;
            int depth = 1;
            while (*p && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                if (depth > 0) p++;
            }
            fallback_end = p;
            while (fallback_end > fallback && isspace((unsigned char)fallback_end[-1])) fallback_end--;
            /* Consume the closing ')' so it is not emitted as literal text
             * after the substituted value. */
            if (*p == ')') p++;
        } else if (*p == ')') {
            p++;
        }

        size_t name_len = (size_t)(name_end - name_start);
        char *name = (char*)malloc(name_len + 1);
        if (!name) break;
        memcpy(name, name_start, name_len);
        name[name_len] = '\0';

        const char *resolved = css_custom_props_get(props, name);
        free(name);

        if (resolved && resolved[0]) {
            size_t rlen = strlen(resolved);
            char *grown = (char*)realloc(out, out_len + rlen + strlen(p) + 1);
            if (!grown) break;
            out = grown;
            memcpy(out + out_len, resolved, rlen);
            out_len += rlen;
        } else if (fallback && fallback_end > fallback) {
            /* The fallback may itself contain var() references. */
            size_t flen0 = (size_t)(fallback_end - fallback);
            char *fb = (char*)malloc(flen0 + 1);
            if (!fb) break;
            memcpy(fb, fallback, flen0);
            fb[flen0] = '\0';
            char *fr = css_var_resolve(fb, props);
            const char *use = fr ? fr : fb;
            size_t flen = strlen(use);
            char *grown = (char*)realloc(out, out_len + flen + strlen(p) + 1);
            if (!grown) { free(fr); free(fb); break; }
            out = grown;
            memcpy(out + out_len, use, flen);
            out_len += flen;
            free(fr);
            free(fb);
        } else {
            t_var_failed = true;
        }
    }

    out[out_len] = '\0';
    return out;
}

/* ============================================================================
 * Stylesheet collection and parallel application
 * ============================================================================ */

/* Stylesheets are collected as text, parsed once per distinct text (cache
 * keyed by content and shadow scope), parsed in parallel on a miss (large
 * sheets in chunks), and compiled into a CssStyleSet that is cached per
 * (sheets, viewport).  All of this runs on the engine thread except the
 * parse jobs, which only build new sheets. */

typedef struct SheetEntry {
    uint64_t hash;
    size_t len;
    char *text;
    char *host;                 /* shadow scope (NULL = document) */
    CssStylesheet *sheet;       /* may have zero rules */
    int refs;                   /* style-set cache entries using it */
    uint64_t last_use;
} SheetEntry;

#define SHEET_CACHE_MAX 128
static SheetEntry *g_sheet_cache[SHEET_CACHE_MAX];
static int g_sheet_cache_n;
static uint64_t g_layout_clock;

typedef struct SheetItem {
    const char *text;           /* points at own_text or at document text */
    size_t len;
    char *own_text;
    char *host;
    CssStylesheet *owned;       /* a sheet the caller handed in */
    SheetEntry *entry;          /* cache entry once resolved */
    CssStylesheet *parsed;      /* parse result when the cache could not keep it */
} SheetItem;

typedef struct LayoutStyleSheetList {
    CssStylesheet **sheets;     /* final, in cascade order */
    SheetEntry **entries;       /* parallel to sheets (NULL for uncached) */
    int count;
    int capacity;
    SheetItem *items;
    int nitems, cap_items;
    bool all_cached;
} LayoutStyleSheetList;

static uint64_t sheet_hash(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ull; }
    return h;
}

static SheetItem *layout_sheet_item_new(LayoutStyleSheetList *list)
{
    if (list->nitems == list->cap_items) {
        list->cap_items = list->cap_items ? list->cap_items * 2 : 8;
        list->items = (SheetItem *)realloc(list->items, (size_t)list->cap_items * sizeof(SheetItem));
    }
    SheetItem *it = &list->items[list->nitems++];
    memset(it, 0, sizeof(*it));
    return it;
}

/* Queue stylesheet text (copied) for parsing, optionally scoped to a host. */
static void layout_sheet_list_add_text(LayoutStyleSheetList *list, const char *text, size_t len,
                                       const char *host)
{
    if (!text || len == 0) return;
    SheetItem *it = layout_sheet_item_new(list);
    it->own_text = (char *)malloc(len + 1);
    memcpy(it->own_text, text, len);
    it->own_text[len] = '\0';
    it->text = it->own_text;
    it->len = len;
    it->host = host ? strdup(host) : NULL;
}

/* A sheet owned by the caller-supplied list (freed with the list). */
static bool layout_sheet_list_add(LayoutStyleSheetList *list, CssStylesheet *sheet) {
    if (!sheet) return false;
    SheetItem *it = layout_sheet_item_new(list);
    it->owned = sheet;
    return true;
}

static void layout_sheet_list_free(LayoutStyleSheetList *list) {
    if (!list) return;
    for (int i = 0; i < list->nitems; i++) {
        SheetItem *it = &list->items[i];
        if (it->owned) css_stylesheet_free(it->owned);
        if (it->parsed) css_stylesheet_free(it->parsed);
        free(it->own_text);
        free(it->host);
    }
    free(list->items);
    free(list->sheets);
    free(list->entries);
    memset(list, 0, sizeof(*list));
}

static SheetEntry *sheet_cache_find(uint64_t h, const char *text, size_t len, const char *host)
{
    for (int i = 0; i < g_sheet_cache_n; i++) {
        SheetEntry *e = g_sheet_cache[i];
        if (e->hash != h || e->len != len) continue;
        if ((e->host == NULL) != (host == NULL)) continue;
        if (host && strcmp(e->host, host) != 0) continue;
        if (memcmp(e->text, text, len) != 0) continue;
        return e;
    }
    return NULL;
}

/* Insert a parsed sheet; returns NULL when the cache is full of sheets in
 * use (the caller then owns the sheet for this layout only). */
static SheetEntry *sheet_cache_insert(uint64_t h, const char *text, size_t len, const char *host,
                                      CssStylesheet *sheet)
{
    if (g_sheet_cache_n == SHEET_CACHE_MAX) {
        int victim = -1;
        for (int i = 0; i < g_sheet_cache_n; i++) {
            SheetEntry *e = g_sheet_cache[i];
            if (e->refs > 0 || e->last_use == g_layout_clock) continue;
            if (victim < 0 || e->last_use < g_sheet_cache[victim]->last_use) victim = i;
        }
        if (victim < 0) return NULL;
        SheetEntry *v = g_sheet_cache[victim];
        if (v->sheet) css_stylesheet_free(v->sheet);
        free(v->text);
        free(v->host);
        free(v);
        g_sheet_cache[victim] = g_sheet_cache[--g_sheet_cache_n];
    }
    SheetEntry *e = (SheetEntry *)calloc(1, sizeof(SheetEntry));
    e->hash = h;
    e->len = len;
    e->text = (char *)malloc(len + 1);
    memcpy(e->text, text, len);
    e->text[len] = '\0';
    e->host = host ? strdup(host) : NULL;
    e->sheet = sheet;
    e->last_use = g_layout_clock;
    g_sheet_cache[g_sheet_cache_n++] = e;
    return e;
}

/* ---- Parallel parsing (PARALLEL_STYLE_LAYOUT.md §6) ----
 * A sheet larger than SHEET_CHUNK bytes is cut at candidate top-level rule
 * boundaries; chunks parse in parallel with css_stylesheet_parse_range, and
 * the continuation validates that each chunk starts exactly where the
 * previous one stopped (re-parsing a chunk from the true position when a
 * guess was wrong), so the result equals a single serial parse. */

#define SHEET_CHUNK_DEFAULT (32u * 1024u)
static size_t g_sheet_chunk = SHEET_CHUNK_DEFAULT;   /* CSS_SHEET_CHUNK=<bytes> */

typedef struct ParseTask {
    const char *text;
    size_t len;
    const char *host;
    int nchunks;
    size_t *start;              /* nchunks + 1 (last = len) */
    size_t *end;
    CssStylesheet **chunk;
    CssStylesheet *result;
} ParseTask;

/* Candidate boundaries: just after a '}' that closes depth 0, outside
 * comments and strings, roughly every SHEET_CHUNK bytes. */
static int sheet_split(const char *s, size_t n, size_t *starts, int max_chunks)
{
    int k = 0;
    starts[k++] = 0;
    size_t next = g_sheet_chunk;
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i < n && k < max_chunks; i++) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && i + 1 < n) i++;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            const char *e = strstr(s + i + 2, "*/");
            if (!e || (size_t)(e - s) >= n) break;
            i = (size_t)(e - s) + 1;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '{') depth++;
        else if (c == '}') {
            if (depth > 0) depth--;
            if (depth == 0 && i + 1 >= next && i + 1 < n) {
                starts[k++] = i + 1;
                next = i + 1 + g_sheet_chunk;
            }
        }
    }
    return k;
}

static void parse_chunk_job(Job *j)
{
    ParseTask *t = (ParseTask *)j->ctx;
    int k = (int)j->a;
    t->chunk[k] = css_stylesheet_parse_range(t->text, t->len, t->start[k], t->start[k + 1], &t->end[k]);
}

static void parse_merge_cont(Job *j)
{
    ParseTask *t = (ParseTask *)j->ctx;
    size_t pos = 0;
    int total = 0;
    for (int k = 0; k < t->nchunks; k++) {
        if (t->start[k] != pos) {
            /* Mis-guessed boundary: parse this range from the true position. */
            if (t->chunk[k]) css_stylesheet_free(t->chunk[k]);
            size_t stop = t->start[k + 1] > pos ? t->start[k + 1] : pos;
            t->chunk[k] = css_stylesheet_parse_range(t->text, t->len, pos, stop, &t->end[k]);
        }
        pos = t->end[k];
        if (t->chunk[k]) total += t->chunk[k]->rule_count;
    }
    CssStylesheet *sheet = (CssStylesheet *)calloc(1, sizeof(CssStylesheet));
    sheet->rules = (CssRule *)malloc((size_t)(total ? total : 1) * sizeof(CssRule));
    sheet->rule_capacity = total ? total : 1;
    for (int k = 0; k < t->nchunks; k++) {
        CssStylesheet *c = t->chunk[k];
        if (!c) continue;
        if (c->rule_count)
            memcpy(sheet->rules + sheet->rule_count, c->rules, (size_t)c->rule_count * sizeof(CssRule));
        sheet->rule_count += c->rule_count;
        free(c->rules);
        free(c);
    }
    if (t->host) css_scope_stylesheet(sheet, t->host);
    t->result = sheet;
}

static void parse_sheet_job(Job *j)
{
    ParseTask *t = (ParseTask *)j->ctx;
    if (t->nchunks <= 1) {
        size_t end = 0;
        t->result = css_stylesheet_parse_range(t->text, t->len, 0, t->len, &end);
        if (t->result && t->host) css_scope_stylesheet(t->result, t->host);
        return;
    }
    for (int k = 0; k < t->nchunks; k++) job_spawn(job_new(parse_chunk_job, j, t, (uint32_t)k, 0));
    job_set_continuation(j, parse_merge_cont);
}

typedef struct ParseBatch { ParseTask *tasks; int n; } ParseBatch;

/* Test hook: parse one sheet through the chunked parallel parser with the
 * given chunk size (0 = default). */
CssStylesheet *css_layout_parse_sheet_parallel(const char *text, size_t len, size_t chunk)
{
    size_t saved = g_sheet_chunk;
    g_sheet_chunk = chunk ? chunk : SHEET_CHUNK_DEFAULT;
    ParseTask t;
    memset(&t, 0, sizeof(t));
    t.text = text;
    t.len = len;
    int maxc = (int)(len / g_sheet_chunk) + 2;
    t.start = (size_t *)malloc(((size_t)maxc + 1) * sizeof(size_t));
    t.nchunks = sheet_split(text, len, t.start, maxc);
    t.start[t.nchunks] = len;
    t.end = (size_t *)calloc((size_t)t.nchunks + 1, sizeof(size_t));
    t.chunk = (CssStylesheet **)calloc((size_t)t.nchunks + 1, sizeof(CssStylesheet *));
    job_graph_run(job_new(parse_sheet_job, NULL, &t, 0, 0));
    free(t.start);
    free(t.end);
    free(t.chunk);
    g_sheet_chunk = saved;
    return t.result;
}

static void parse_batch_job(Job *j)
{
    ParseBatch *b = (ParseBatch *)j->ctx;
    for (int i = 0; i < b->n; i++) job_spawn(job_new(parse_sheet_job, j, &b->tasks[i], 0, 0));
}

#define STYLESET_CACHE_MAX_FWD 4
static void styleset_cache_drop(int i);

/* Resolve queued texts to sheets (cache, then parallel parse of misses) and
 * build list->sheets in cascade order.  Empty sheets are dropped, as the
 * legacy collector dropped sheets that parsed to nothing. */
static void layout_sheet_list_resolve(LayoutStyleSheetList *list)
{
    g_layout_clock++;
    {
        const char *e = getenv("CSS_SHEET_CHUNK");
        g_sheet_chunk = (e && atoi(e) > 0) ? (size_t)atoi(e) : SHEET_CHUNK_DEFAULT;
        e = getenv("CSS_SHEET_CACHE");
        if (e && strcmp(e, "0") == 0) {
            /* Testing: forget every cached sheet and style set first. */
            for (int i = 0; i < STYLESET_CACHE_MAX_FWD; i++) styleset_cache_drop(i);
            for (int i = 0; i < g_sheet_cache_n; i++) {
                SheetEntry *v = g_sheet_cache[i];
                if (v->sheet) css_stylesheet_free(v->sheet);
                free(v->text); free(v->host); free(v);
            }
            g_sheet_cache_n = 0;
        }
    }
    ParseTask *tasks = (ParseTask *)calloc((size_t)list->nitems + 1, sizeof(ParseTask));
    int *task_item = (int *)malloc(((size_t)list->nitems + 1) * sizeof(int));
    uint64_t *hashes = (uint64_t *)calloc((size_t)list->nitems + 1, sizeof(uint64_t));
    int ntasks = 0;
    for (int i = 0; i < list->nitems; i++) {
        SheetItem *it = &list->items[i];
        if (it->owned) continue;
        hashes[i] = sheet_hash(it->text, it->len);
        SheetEntry *e = sheet_cache_find(hashes[i], it->text, it->len, it->host);
        if (e) { e->last_use = g_layout_clock; it->entry = e; continue; }
        /* The same text twice in one document parses once. */
        bool dup = false;
        for (int t = 0; t < ntasks && !dup; t++) {
            SheetItem *o = &list->items[task_item[t]];
            if (hashes[task_item[t]] == hashes[i] && o->len == it->len &&
                ((o->host == NULL) == (it->host == NULL)) && (!o->host || strcmp(o->host, it->host) == 0) &&
                memcmp(o->text, it->text, it->len) == 0) dup = true;
        }
        if (dup) continue;
        ParseTask *t = &tasks[ntasks];
        t->text = it->text;
        t->len = it->len;
        t->host = it->host;
        int maxc = (int)(it->len / g_sheet_chunk) + 2;
        t->start = (size_t *)malloc(((size_t)maxc + 1) * sizeof(size_t));
        t->nchunks = sheet_split(it->text, it->len, t->start, maxc);
        t->start[t->nchunks] = it->len;
        t->end = (size_t *)calloc((size_t)t->nchunks + 1, sizeof(size_t));
        t->chunk = (CssStylesheet **)calloc((size_t)t->nchunks + 1, sizeof(CssStylesheet *));
        task_item[ntasks++] = i;
    }
    if (ntasks > 0) {
        ParseBatch b = { tasks, ntasks };
        job_graph_run(job_new(parse_batch_job, NULL, &b, 0, 0));
    }
    for (int t = 0; t < ntasks; t++) {
        SheetItem *it = &list->items[task_item[t]];
        CssStylesheet *sheet = tasks[t].result;
        if (sheet) {
            it->entry = sheet_cache_insert(hashes[task_item[t]], it->text, it->len, it->host, sheet);
            if (!it->entry) it->parsed = sheet;
        }
        free(tasks[t].start);
        free(tasks[t].end);
        free(tasks[t].chunk);
    }
    /* Items that duplicated a parsed text pick up its entry now. */
    list->all_cached = true;
    list->sheets = (CssStylesheet **)malloc(((size_t)list->nitems + 1) * sizeof(CssStylesheet *));
    list->entries = (SheetEntry **)malloc(((size_t)list->nitems + 1) * sizeof(SheetEntry *));
    list->count = 0;
    for (int i = 0; i < list->nitems; i++) {
        SheetItem *it = &list->items[i];
        CssStylesheet *sh = NULL;
        SheetEntry *e = NULL;
        if (it->owned) {
            sh = it->owned;
        } else {
            if (!it->entry && !it->parsed)
                it->entry = sheet_cache_find(hashes[i], it->text, it->len, it->host);
            if (it->entry) { e = it->entry; sh = e->sheet; e->last_use = g_layout_clock; }
            else if (it->parsed) sh = it->parsed;
            else {
                /* Duplicate of a text whose parse the cache could not keep. */
                for (int k = 0; k < i && !sh; k++) {
                    SheetItem *o = &list->items[k];
                    if (o->parsed && hashes[k] == hashes[i] && o->len == it->len &&
                        memcmp(o->text, it->text, it->len) == 0 &&
                        ((o->host == NULL) == (it->host == NULL)) &&
                        (!o->host || strcmp(o->host, it->host) == 0))
                        sh = o->parsed;
                }
            }
        }
        if (!sh || sh->rule_count == 0) continue;
        if (!e) list->all_cached = false;
        list->sheets[list->count] = sh;
        list->entries[list->count] = e;
        list->count++;
    }
    free(tasks);
    free(task_item);
    free(hashes);
}

/* ---- Compiled style-set cache: (sheets, viewport) -> CssStyleSet ---- */

typedef struct StyleSetEntry {
    int n;
    SheetEntry **sheets;
    double vw, vh;
    CssStyleSet *set;
    uint64_t last_use;
} StyleSetEntry;

#define STYLESET_CACHE_MAX 4
static StyleSetEntry g_styleset_cache[STYLESET_CACHE_MAX];

static void styleset_cache_drop(int i)
{
    StyleSetEntry *c = &g_styleset_cache[i];
    if (!c->set) return;
    for (int k = 0; k < c->n; k++) c->sheets[k]->refs--;
    css_styleset_free(c->set);
    free(c->sheets);
    memset(c, 0, sizeof(*c));
}

/* Returns the compiled set for the list; *owned is true when the caller must
 * free it (lists with uncached sheets are compiled per layout). */
static CssStyleSet *layout_styleset_get(LayoutStyleSheetList *list, double vw, double vh, bool *owned)
{
    *owned = false;
    if (!list->all_cached) {
        *owned = true;
        return css_styleset_build(list->sheets, list->count, vw, vh);
    }
    for (int i = 0; i < STYLESET_CACHE_MAX; i++) {
        StyleSetEntry *c = &g_styleset_cache[i];
        if (!c->set || c->n != list->count || c->vw != vw || c->vh != vh) continue;
        if (memcmp(c->sheets, list->entries, (size_t)list->count * sizeof(SheetEntry *)) != 0) continue;
        c->last_use = g_layout_clock;
        return c->set;
    }
    int victim = 0;
    for (int i = 0; i < STYLESET_CACHE_MAX; i++) {
        if (!g_styleset_cache[i].set) { victim = i; break; }
        if (g_styleset_cache[i].last_use < g_styleset_cache[victim].last_use) victim = i;
    }
    styleset_cache_drop(victim);
    StyleSetEntry *c = &g_styleset_cache[victim];
    c->n = list->count;
    c->sheets = (SheetEntry **)malloc(((size_t)list->count + 1) * sizeof(SheetEntry *));
    memcpy(c->sheets, list->entries, (size_t)list->count * sizeof(SheetEntry *));
    for (int k = 0; k < c->n; k++) c->sheets[k]->refs++;
    c->vw = vw;
    c->vh = vh;
    c->set = css_styleset_build(list->sheets, list->count, vw, vh);
    c->last_use = g_layout_clock;
    return c->set;
}

static char* layout_resolve_url(const char *base_url, const char *href) {
    if (!href || !href[0]) return NULL;
    if (url_has_scheme(href)) return strdup(href);
    if (strncmp(href, "//", 2) == 0) {
        char buf[2048];
        snprintf(buf, sizeof(buf), "https:%s", href);
        return strdup(buf);
    }
    if (href[0] == '/') {
        const char *base = base_url && base_url[0] ? base_url : "https://localhost";
        char buf[2048];
        if (base[strlen(base) - 1] == '/') {
            snprintf(buf, sizeof(buf), "%s%s", base, href + 1);
        } else {
            snprintf(buf, sizeof(buf), "%s%s", base, href);
        }
        return strdup(buf);
    }
    const char *base = base_url && base_url[0] ? base_url : "https://localhost/";
    char buf[2048];
    if (base[strlen(base) - 1] == '/') {
        snprintf(buf, sizeof(buf), "%s%s", base, href);
    } else {
        const char *last_slash = strrchr(base, '/');
        if (last_slash) {
            size_t base_len = (size_t)(last_slash - base) + 1;
            snprintf(buf, sizeof(buf), "%.*s%s", (int)base_len, base, href);
        } else {
            snprintf(buf, sizeof(buf), "%s/%s", base, href);
        }
    }
    return strdup(buf);
}

/* Fetched stylesheet texts, by URL, for the life of the process: a layout
 * (hit test, re-render) never re-downloads a sheet. */
typedef struct UrlSheet { char *url; char *text; size_t len; } UrlSheet;
#define URL_SHEET_MAX 64
static UrlSheet g_url_sheets[URL_SHEET_MAX];
static int g_url_sheet_n, g_url_sheet_next;

/* The cached sheet for `url`, fetching it on first use (NULL on failure). */
static const UrlSheet *url_sheet_get(const char *url) {
    for (int i = 0; i < g_url_sheet_n; i++)
        if (strcmp(g_url_sheets[i].url, url) == 0) return &g_url_sheets[i];

    HttpBuffer buffer = {0};
    char err[256] = {0};
    bool ok = http_get_to_memory(url, &buffer, err, sizeof(err));
    const UrlSheet *out = NULL;
    if (ok && buffer.data && buffer.size > 0) {
        LOG_INFO("Fetched stylesheet (%zu bytes)", buffer.size);
        UrlSheet *u = &g_url_sheets[g_url_sheet_next];
        if (g_url_sheet_n == URL_SHEET_MAX) { free(u->url); free(u->text); }
        else g_url_sheet_n++;
        g_url_sheet_next = (g_url_sheet_next + 1) % URL_SHEET_MAX;
        u->url = strdup(url);
        u->text = (char *)malloc(buffer.size + 1);
        memcpy(u->text, buffer.data, buffer.size);
        u->text[buffer.size] = '\0';
        u->len = buffer.size;
        out = u;
    } else {
        LOG_WARN("Failed to fetch stylesheet %.80s: %s", url, err[0] ? err : "unknown");
    }
    if (buffer.data) free(buffer.data);
    return out;
}

bool css_layout_fetch_stylesheet_url(const char *url) {
    return url && url[0] && url_sheet_get(url) != NULL;
}

static void layout_fetch_stylesheet(LayoutStyleSheetList *list, const char *base_url, const char *href) {
    char *url = layout_resolve_url(base_url, href);
    if (!url) return;
    const UrlSheet *u = url_sheet_get(url);
    if (u) layout_sheet_list_add_text(list, u->text, u->len, NULL);
    free(url);
}

/* Walk up the native parent chain to find the nearest custom-element host.
 * This lets us scope shadow-root <style> sheets correctly. */
static const char* layout_find_host_tag(LayoutContext *ctx, int dom_idx) {
    int p = po_array_parent(&ctx->doc->array, dom_idx);
    while (p >= 0) {
        HtmlNode *node = (HtmlNode*)po_array_payload(&ctx->doc->array, p);
        if (node && node->type == HTML_NODE_ELEMENT && node->tag_name && strchr(node->tag_name, '-')) {
            return node->tag_name;
        }
        p = po_array_parent(&ctx->doc->array, p);
    }
    return NULL;
}

static void layout_collect_stylesheets_recursive(LayoutContext *ctx, int layout_idx,
                                                  LayoutStyleSheetList *list,
                                                  const char *base_url) {
    if (layout_idx < 0 || layout_idx >= ctx->tree.count) return;
    int dom_idx = ctx->tree.nodes[layout_idx].dom_node_idx;
    HtmlNode *node = layout_node_dom(ctx, dom_idx);
    if (!node || node->type != HTML_NODE_ELEMENT) goto next;

    if (strcasecmp(node->tag_name, "style") == 0 && node->text_content && node->text_content[0]) {
        /* If this <style> lives inside a stamped shadow root, scope :host,
         * ::slotted, etc. to the host custom element. */
        layout_sheet_list_add_text(list, node->text_content, strlen(node->text_content),
                                   layout_find_host_tag(ctx, dom_idx));
    } else if (strcasecmp(node->tag_name, "link") == 0) {
        const char *rel = NULL;
        const char *href = NULL;
        for (HtmlAttribute *a = node->attributes; a; a = a->next) {
            if (strcasecmp(a->name, "rel") == 0) rel = a->value;
            else if (strcasecmp(a->name, "href") == 0) href = a->value;
        }
        if (rel && href && strcasecmp(rel, "stylesheet") == 0) {
            layout_fetch_stylesheet(list, base_url, href);
        }
    }

next:
    for (int c = ctx->tree.nodes[layout_idx].first_child_idx; c >= 0;
         c = ctx->tree.nodes[c].next_sibling_idx) {
        layout_collect_stylesheets_recursive(ctx, c, list, base_url);
    }
}

static void layout_collect_js_sheet_array(JSContextHandle ctx, GCValue arr,
                                            LayoutStyleSheetList *list)
{
    if (!JS_IsArray(ctx, arr)) return;
    GCValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, len_val);
    for (uint32_t i = 0; i < n; i++) {
        GCValue sheet = JS_GetPropertyUint32(ctx, arr, i);
        GCValue text = JS_GetPropertyStr(ctx, sheet, "cssText");
        const char *s = JS_ToCString(ctx, text);
        if (s && s[0]) layout_sheet_list_add_text(list, s, strlen(s), NULL);
    }
}

static void layout_collect_constructed_stylesheets(LayoutContext *ctx,
                                                    LayoutStyleSheetList *list)
{
    if (!SAFE_VALID(ctx->js_ctx) ||
        gc_handle_get_type(ctx->js_ctx) != JS_GC_OBJ_TYPE_JS_CONTEXT) return;
    JSContextHandle js = ctx->js_ctx;
    GCValue global = JS_GetGlobalObject(js);
    GCValue doc = JS_GetPropertyStr(js, global, "document");
    if (!JS_IsObject(doc)) return;

    /* Document-level adopted stylesheets. */
    GCValue adopted = JS_GetPropertyStr(js, doc, "__adoptedStyleSheets");
    layout_collect_js_sheet_array(js, adopted, list);

    /* Shadow-root adopted stylesheets.  Each sheet is scoped to its host by
     * prefixing every selector with a unique class assigned to the host. */
    const char *script =
        "(function(){"
        "  var out=[];"
        "  var counter = (window.__cyber_scope_counter || 0);"
        "  var roots=window.__cyber_adopting_roots||[];"
        "  for(var i=0;i<roots.length;i++){"
        "    var sr=roots[i], host=sr.host;"
        "    if(sr && host && host.isConnected){"
        "      var sheets=sr.__adoptedStyleSheets;"
        "      if(sheets && sheets.length){"
        "        var sid=host.getAttribute('__cyber_scope_id');"
        "        if(!sid){ sid='cs'+(++counter); }"
        "        var scopeClass='cyber-scope-'+sid;"
        "        for(var j=0;j<sheets.length;j++){"
        "          var css=sheets[j].cssText||'';"
        "          if(!css) continue;"
        "          var scoped=css.replace(/([^{};@][^{};]*)\\{/g, function(m, sel){"
        "            if(sel.trim().charAt(0)==='@') return m;"
        "            var parts=sel.split(',');"
        "            for(var k=0;k<parts.length;k++){"
        "              var s=parts[k].trim();"
        "              if(!s) continue;"
        "              if(s.indexOf(':host')===0) s='.'+scopeClass+s.substring(5);"
        "              else s='.'+scopeClass+' '+s;"
        "              parts[k]=s;"
        "            }"
        "            return parts.join(', ')+'{';"
        "          });"
        "          out.push(scoped);"
        "        }"
        "      }"
        "    }"
        "  }"
        "  window.__cyber_scope_counter=counter;"
        "  return out;"
        "})()";
    GCValue scoped_css = JS_Eval(js, script, strlen(script),
                                 "<constructed-css>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsArray(js, scoped_css)) {
        GCValue len_val = JS_GetPropertyStr(js, scoped_css, "length");
        uint32_t m = 0;
        JS_ToUint32(js, &m, len_val);
        for (uint32_t j = 0; j < m; j++) {
            GCValue text = JS_GetPropertyUint32(js, scoped_css, j);
            const char *s = JS_ToCString(js, text);
            if (s && s[0]) layout_sheet_list_add_text(list, s, strlen(s), NULL);
        }
    }
}

/* User-agent stylesheet approximating the HTML5 rendering defaults.
 * All selectors are kept at single-element specificity (0,0,1) so that any
 * author rule — even a bare element selector — wins by specificity or source
 * order (the UA sheet is always first in the sheet list). */
static const char *g_ua_stylesheet_css =
    "head, meta, title, link, style, script, base, template, noscript { display: none; }\n"
    "html { font-family: serif; }\n"
    "body { margin: 8px; }\n"
    "p { margin: 1em 0; }\n"
    "h1 { display: block; font-size: 2em; font-weight: bold; margin: 0.67em 0; }\n"
    "h2 { display: block; font-size: 1.5em; font-weight: bold; margin: 0.83em 0; }\n"
    "h3 { display: block; font-size: 1.17em; font-weight: bold; margin: 1em 0; }\n"
    "h4 { display: block; font-size: 1em; font-weight: bold; margin: 1.33em 0; }\n"
    "h5 { display: block; font-size: 0.83em; font-weight: bold; margin: 1.67em 0; }\n"
    "h6 { display: block; font-size: 0.67em; font-weight: bold; margin: 2.33em 0; }\n"
    "b { font-weight: bold; }\n"
    "strong { font-weight: bold; }\n"
    "th { font-weight: bold; }\n"
    "i { font-style: italic; }\n"
    "em { font-style: italic; }\n"
    "cite { font-style: italic; }\n"
    "dfn { font-style: italic; }\n"
    "var { font-style: italic; }\n"
    "address { font-style: italic; }\n"
    "u { text-decoration: underline; }\n"
    "ins { text-decoration: underline; }\n"
    "s { text-decoration: line-through; }\n"
    "del { text-decoration: line-through; }\n"
    "strike { text-decoration: line-through; }\n"
    "a { color: #0000EE; text-decoration: underline; }\n"
    "code { font-family: monospace; }\n"
    "kbd { font-family: monospace; }\n"
    "samp { font-family: monospace; }\n"
    "tt { font-family: monospace; }\n"
    "pre { display: block; font-family: monospace; margin: 1em 0; white-space: pre; }\n"
    "textarea { white-space: pre-wrap; }\n"
    "nobr { white-space: nowrap; }\n"
    "small { font-size: 0.833em; }\n"
    "sub { font-size: 0.833em; }\n"
    "sup { font-size: 0.833em; }\n"
    "big { font-size: 1.17em; }\n"
    "ul { display: block; margin: 1em 0; padding-left: 40px; list-style-type: disc; }\n"
    "ol { display: block; margin: 1em 0; padding-left: 40px; list-style-type: decimal; }\n"
    "li { display: list-item; }\n"
    "dl { display: block; margin: 1em 0; }\n"
    "dd { display: block; margin-left: 40px; }\n"
    "blockquote { display: block; margin: 1em 40px; }\n"
    "figure { display: block; margin: 1em 40px; }\n"
    "hr { display: block; margin: 0.5em 0; border-top: 1px solid #808080; }\n"
    "center { text-align: center; }\n"
    "caption { text-align: center; }\n"
    "mark { background-color: #ffff00; color: #000000; }\n"
    "button, input, select, textarea { font-size: 13.333px; line-height: normal; }\n"
    "button { padding: 1px 6px; border: 2px outset #767676; background-color: #efefef; box-sizing: border-box; text-align: center; }\n"
    "input { padding: 1px 2px; border: 2px inset #767676; }\n"
    "textarea { padding: 2px; border: 1px solid #767676; }\n"
    "select { border: 1px solid #767676; box-sizing: border-box; }\n";

static bool layout_collect_document_stylesheets(LayoutContext *ctx, LayoutStyleSheetList *list,
                                                 const char *base_url) {
    if (!ctx || !list || ctx->tree.count == 0) return false;

    /* Inject the UA stylesheet first so metadata/head/template elements are
     * hidden even when the hardcoded fast-path is bypassed. */
    layout_sheet_list_add_text(list, g_ua_stylesheet_css, strlen(g_ua_stylesheet_css), NULL);

    int root_dom_idx = ctx->tree.nodes[ctx->tree.root_idx].dom_node_idx;
    layout_collect_stylesheets_recursive(ctx, root_dom_idx, list, base_url);
    layout_collect_constructed_stylesheets(ctx, list);
    return true;
}

/* True when the value's var() references the property being set: per spec
 * that makes the declaration invalid (the property is left unset). */
static bool css_var_is_self_reference(const char *prop, const char *val)
{
    if (!prop || !val) return false;
    size_t plen = strlen(prop);
    const char *p = val;
    while ((p = strstr(p, "var(")) != NULL) {
        p += 4;
        while (*p && isspace((unsigned char)*p)) p++;
        if (strncasecmp(p, prop, plen) == 0) {
            char next = p[plen];
            if (next == ',' || next == ')' || isspace((unsigned char)next)) return true;
        }
    }
    return false;
}

static bool layout_text_is_whitespace(const char *s);

/* Decode a CSS `content` value consisting solely of string literals into a
 * UTF-8 buffer.  Handles CSS hex escapes (up to 6 digits + optional trailing
 * whitespace terminator); U+00A0 decodes to a regular space (same rendered
 * width, avoids missing-glyph risk in the shaper).  Returns false for
 * anything beyond plain strings (counter()/attr()/url()/open-quote/...),
 * which callers treat as "no usable textual content".  *is_empty is set when
 * the value explicitly produces no text (none/normal/empty string) so that
 * cascade suppression by later rules still works. */
static bool css_content_decode_string(const char *value, char *out, size_t outsz,
                                      bool *is_empty)
{
    size_t o = 0;
    out[0] = '\0';
    if (is_empty) *is_empty = false;
    if (!value) return false;
    const char *p = value;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncasecmp(p, "none", 4) == 0 || strncasecmp(p, "normal", 6) == 0) {
        if (is_empty) *is_empty = true;
        return true;
    }
    bool any_string = false;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p != '"' && *p != '\'') return false;  /* counter()/attr()/... */
        char quote = *p++;
        any_string = true;
        while (*p && *p != quote) {
            if (*p == '\\') {
                p++;
                if (*p == '\n' || *p == '\f') { p++; continue; }
                if (*p == '\r') { p++; if (*p == '\n') p++; continue; }
                unsigned cp;
                if (isxdigit((unsigned char)*p)) {
                    cp = 0;
                    int nd = 0;
                    while (nd < 6 && isxdigit((unsigned char)*p)) {
                        unsigned d = isdigit((unsigned char)*p)
                            ? (unsigned)(*p - '0')
                            : (unsigned)(tolower((unsigned char)*p) - 'a' + 10);
                        cp = cp * 16 + d;
                        p++; nd++;
                    }
                    if (isspace((unsigned char)*p)) p++;  /* escape terminator */
                    if (cp == 0xA0) cp = 0x20;  /* nbsp -> space */
                    /* encode cp as UTF-8 */
                    if (cp < 0x80) {
                        if (o + 1 >= outsz) goto trunc;
                        out[o++] = (char)cp;
                    } else if (cp < 0x800) {
                        if (o + 2 >= outsz) goto trunc;
                        out[o++] = (char)(0xC0 | (cp >> 6));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        if (o + 3 >= outsz) goto trunc;
                        out[o++] = (char)(0xE0 | (cp >> 12));
                        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        if (o + 4 >= outsz) goto trunc;
                        out[o++] = (char)(0xF0 | (cp >> 18));
                        out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    }
                } else {
                    if (o + 1 >= outsz) goto trunc;
                    out[o++] = *p++;
                }
            } else {
                if (o + 1 >= outsz) goto trunc;
                out[o++] = *p++;  /* stylesheet text is already UTF-8 */
            }
        }
        if (*p == quote) p++;
    }
trunc:
    out[o < outsz ? o : outsz - 1] = '\0';
    if (!any_string && is_empty) *is_empty = true;
    return true;
}

/* Deepest-last non-whitespace text descendant of a DOM node, or -1. */
static int layout_last_text_descendant(HtmlDocument *doc, int idx)
{
    int result = -1;
    for (int c = po_array_first_child(&doc->array, idx); c >= 0;
         c = po_array_next_sibling(&doc->array, c)) {
        int sub = layout_last_text_descendant(doc, c);
        if (sub >= 0) result = sub;
        HtmlNode *n = (HtmlNode *)po_array_payload(&doc->array, c);
        if (n && n->type == HTML_NODE_TEXT && n->text_content &&
            !layout_text_is_whitespace(n->text_content))
            result = c;
    }
    return result;
}

/* Realize an ::after generated-content string by injecting it into the DOM
 * text adjacent to the element.  Preferred site: a whitespace-only text node
 * immediately after the element — replacing it keeps the generated text
 * outside nested <a> elements (matching the pseudo box's own formatting
 * context) and preserves the collapsed-space geometry the whitespace would
 * have produced.  Fallback: append to the element's last text descendant
 * (minified markup without inter-element whitespace). */
static void layout_inject_generated_after(LayoutContext *ctx, int idx,
                                          const char *content)
{
    if (!ctx->doc || !content || !content[0]) return;
    int dom_idx = ctx->tree.nodes[idx].dom_node_idx;
    int sib = po_array_next_sibling(&ctx->doc->array, dom_idx);
    if (sib >= 0) {
        HtmlNode *sn = (HtmlNode *)po_array_payload(&ctx->doc->array, sib);
        if (sn && sn->type == HTML_NODE_TEXT && sn->text_content &&
            layout_text_is_whitespace(sn->text_content)) {
            size_t cl = strlen(content);
            char *nt = (char *)malloc(cl + 1);
            if (!nt) return;
            memcpy(nt, content, cl + 1);
            free(sn->text_content);
            sn->text_content = nt;
            sn->text_len = cl;
            return;
        }
    }
    int last = layout_last_text_descendant(ctx->doc, dom_idx);
    if (last >= 0) {
        HtmlNode *tn = (HtmlNode *)po_array_payload(&ctx->doc->array, last);
        if (tn && tn->text_content) {
            size_t tl = strlen(tn->text_content), cl = strlen(content);
            /* Idempotency guard: skip when the content is already there. */
            if (tl >= cl && strcmp(tn->text_content + tl - cl, content) == 0)
                return;
            char *nt = (char *)malloc(tl + cl + 1);
            if (!nt) return;
            memcpy(nt, tn->text_content, tl);
            memcpy(nt + tl, content, cl + 1);
            free(tn->text_content);
            tn->text_content = nt;
            tn->text_len = tl + cl;
        }
    }
}

/* ============================================================================
 * Style pass (PARALLEL_STYLE_LAYOUT.md §7)
 *
 * One visit per node in preorder: the parent's style is final when a node is
 * visited, so custom properties, the cascade, inheritance, em/percentage
 * font-size resolution and blockification all happen in the same visit.
 * Selectors are matched once per element through the compiled style set
 * (buckets + ancestor Bloom filter).  Generated ::after content is recorded
 * and realized after the pass, so the pass never writes the document.
 * ============================================================================ */

typedef struct StyleGenAfter {
    int pos;                    /* preorder position (merge order) */
    int idx;                    /* layout index */
    char text[256];
} StyleGenAfter;

/* Per-thread state: matching scratch, generated content found, and the arena
 * for variable maps (absorbed into the layout's arena after the pass). */
/* Style-sharing cache entry: a recently styled element of this thread. */
#define STYLE_SHARE_SLOTS 32
typedef struct StyleShareEntry {
    int idx, dom;
    int parent_cls;             /* equivalence class of the parent's style */
    CssAtom tag;
    uint64_t *bits;             /* revalidation results (computed on first use) */
    bool bits_ready;
    bool has_after;
    char after[256];
} StyleShareEntry;

typedef struct StyleThread {
    CssStyleScratch *scratch;
    StyleGenAfter *gen;
    int gen_n, gen_cap;
    FrameArena arena;
    StyleShareEntry share[STYLE_SHARE_SLOTS];
    int share_n, share_next;
    uint64_t *bits;             /* revalidation results of the current element */
    uint64_t shared, share_misses;
} StyleThread;

typedef struct StylePass {
    LayoutContext *ctx;
    const CssStyleSet *set;
    const CssStyleDoc *sd;
    bool em_defer;
    int grain;
    int *pre_end;               /* per preorder position: one past its subtree */
    int *cls;                   /* per layout index: style equivalence class */
    bool share;                 /* style-sharing cache enabled */
    int reval_words;
    StyleThread *threads;       /* job_thread_count() + 1 (last: outside the pool) */
    int nthreads;
} StylePass;

static StyleThread *style_thread(StylePass *P)
{
    int i = job_thread_index();
    if (i < 0 || i >= P->nthreads) i = P->nthreads;
    StyleThread *T = &P->threads[i];
    if (!T->scratch) {
        T->scratch = css_style_scratch_new(P->set);
        size_t words = (size_t)(P->reval_words ? P->reval_words : 1);
        T->bits = (uint64_t *)frame_arena_calloc(&T->arena, words * sizeof(uint64_t));
        for (int k = 0; k < STYLE_SHARE_SLOTS; k++)
            T->share[k].bits = (uint64_t *)frame_arena_calloc(&T->arena, words * sizeof(uint64_t));
    }
    return T;
}

typedef struct InlineBlock { CssDeclaration *decls; int count; } InlineBlock;

/* Winning ::after `content` string for an element (legacy cascade: highest
 * important, then specificity of the first matching branch, then order). */
static bool style_after_content(StylePass *P, StyleThread *T, int dom, char out[256],
                                double *pseudo_aspect)
{
    const CssAfterMatch *am = NULL;
    int n = css_style_match_after(P->set, P->sd, dom, T->scratch, &am);
    bool after_set = false;
    int best_important = -1, best_spec = -1, best_order = -1;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        const CssRule *rule = css_styleset_rule(P->set, am[i].rule);
        /* Pseudo-element aspect boxes (e.g. .thumb::before{padding-top:56.25%})
         * size the real element, since pseudo boxes are not generated. */
        for (int d = 0; d < rule->declaration_count; d++) {
            const CssDeclaration *decl = &rule->declarations[d];
            if (!decl->property || !decl->value || strcasecmp(decl->property, "padding-top") != 0) continue;
            if (!css_value_is_percent(decl->value)) continue;
            double ratio = css_parse_percent_ratio(decl->value);
            if (ratio > *pseudo_aspect) *pseudo_aspect = ratio;
        }
        if (am[i].is_before) continue;
        int branch_spec = am[i].specificity;
        for (int d = 0; d < rule->declaration_count; d++) {
            const CssDeclaration *decl = &rule->declarations[d];
            if (!decl->property || strcasecmp(decl->property, "content") != 0) continue;
            int order = css_styleset_rule_order(P->set, am[i].rule) + d;
            int imp = decl->important ? 1 : 0;
            if (imp < best_important) continue;
            if (imp == best_important && branch_spec < best_spec) continue;
            if (imp == best_important && branch_spec == best_spec && order <= best_order) continue;
            char decoded[256];
            bool is_empty = false;
            if (!css_content_decode_string(decl->value, decoded, sizeof(decoded), &is_empty))
                continue;   /* counters/url()/etc.: unsupported */
            best_important = imp;
            best_spec = branch_spec;
            best_order = order;
            after_set = true;
            if (is_empty) out[0] = '\0';
            else { strncpy(out, decoded, 255); out[255] = '\0'; }
        }
    }
    return after_set;
}

/* A declaration invalid at computed-value time acts as `unset`: inherited
 * properties inherit (clear the "set" marker), others keep the value an
 * earlier declaration gave (approximation of the initial value). */
static void layout_unset_property(LayoutBox *box, const char *prop)
{
    if (!prop) return;
    if (strcasecmp(prop, "font-size") == 0) { box->font_size_set = 0; box->font_size_ratio = 0.0; }
    else if (strcasecmp(prop, "color") == 0) box->color_set = 0;
    else if (strcasecmp(prop, "fill") == 0) box->fill_set = 0;
    else if (strcasecmp(prop, "line-height") == 0) { box->line_height_set = 0; box->line_height = 0.0; box->line_height_ratio = 0.0; }
    else if (strcasecmp(prop, "font-weight") == 0) box->font_weight_set = 0;
    else if (strcasecmp(prop, "font-style") == 0) box->font_italic_set = 0;
    else if (strcasecmp(prop, "font-family") == 0) box->font_family[0] = '\0';
    else if (strcasecmp(prop, "text-align") == 0) box->text_align_set = 0;
    else if (strcasecmp(prop, "white-space") == 0) box->white_space_set = 0;
    else if (strcasecmp(prop, "visibility") == 0) box->visibility_set = 0;
    else if (strcasecmp(prop, "display") == 0) { /* initial: inline */ box->display = CSS_DISPLAY_INLINE; box->inline_level = 0; }
}

/* CSS-wide keywords (css-cascade §7.3: inherit, initial, unset, revert,
 * revert-layer), handled before the per-property parsers, which would read
 * "inherit" as a length (height:inherit became an explicit 0px height).
 * Returns true when the declaration was a CSS-wide keyword. */
static bool layout_apply_wide_keyword(LayoutBox *box, const LayoutBox *parent,
                                      const char *prop, const char *value,
                                      double viewport_width, const char *base_url)
{
    while (*value == ' ' || *value == '\t') value++;
    bool inherit = strcasecmp(value, "inherit") == 0;
    bool initial = strcasecmp(value, "initial") == 0;
    bool unset = strcasecmp(value, "unset") == 0 || strcasecmp(value, "revert") == 0 ||
                 strcasecmp(value, "revert-layer") == 0;
    if (!inherit && !initial && !unset) return false;
    /* font-size's parser handles its keywords (initial = medium). */
    if (strcasecmp(prop, "font-size") == 0) return false;
    bool inherited_prop = css_is_inherited(prop);

    /* Inherited properties: inheritance fills the value in (initial is
     * approximated the same way). */
    if (inherited_prop) {
        layout_unset_property(box, prop);
        return true;
    }

    /* Non-inherited, inherit: the parent's computed value, for the box
     * properties pages use it on. */
    if (inherit && parent) {
        if (strcasecmp(prop, "height") == 0) {
            box->height_set = parent->height_set; box->css_height = parent->css_height;
            box->height_percent = parent->height_percent; box->height_pct_px = parent->height_pct_px;
            return true;
        }
        if (strcasecmp(prop, "width") == 0) {
            box->width_set = parent->width_set; box->css_width = parent->css_width;
            box->width_percent = parent->width_percent; box->width_pct_px = parent->width_pct_px;
            box->width_keyword = parent->width_keyword;
            return true;
        }
        static const struct { const char *name; int which; } mm[] = {
            { "min-width", 0 }, { "max-width", 1 }, { "min-height", 2 }, { "max-height", 3 }
        };
        for (size_t i = 0; i < sizeof(mm) / sizeof(mm[0]); i++) {
            if (strcasecmp(prop, mm[i].name) != 0) continue;
            double *dst[4] = { &box->min_width, &box->max_width, &box->min_height, &box->max_height };
            const double *src[4] = { &parent->min_width, &parent->max_width, &parent->min_height, &parent->max_height };
            *dst[mm[i].which] = *src[mm[i].which];
            unsigned char bit = (unsigned char)(1u << mm[i].which);
            box->minmax_pct_bits = (unsigned char)((box->minmax_pct_bits & ~bit) | (parent->minmax_pct_bits & bit));
            box->minmax_pct[mm[i].which] = parent->minmax_pct[mm[i].which];
            box->minmax_base[mm[i].which] = parent->minmax_base[mm[i].which];
            return true;
        }
        if (strcasecmp(prop, "flex-direction") == 0) { box->flex_direction = parent->flex_direction; return true; }
        if (strcasecmp(prop, "box-sizing") == 0) { box->box_sizing = parent->box_sizing; return true; }
        if (strcasecmp(prop, "overflow") == 0) { box->overflow = parent->overflow; return true; }
    }

    /* initial / unset (and inherit we cannot model): the initial value. */
    static const struct { const char *prop, *value; } initial_values[] = {
        { "width", "auto" }, { "height", "auto" },
        { "min-width", "auto" }, { "min-height", "auto" }, { "max-width", "none" }, { "max-height", "none" },
        { "margin", "0" }, { "margin-top", "0" }, { "margin-right", "0" }, { "margin-bottom", "0" }, { "margin-left", "0" },
        { "padding", "0" }, { "padding-top", "0" }, { "padding-right", "0" }, { "padding-bottom", "0" }, { "padding-left", "0" },
        { "top", "auto" }, { "right", "auto" }, { "bottom", "auto" }, { "left", "auto" },
        { "position", "static" }, { "float", "none" }, { "clear", "none" },
        { "flex-direction", "row" }, { "flex-wrap", "nowrap" }, { "flex-grow", "0" }, { "flex-shrink", "1" },
        { "flex-basis", "auto" }, { "flex", "0 1 auto" }, { "box-sizing", "content-box" },
        { "overflow", "visible" }, { "overflow-x", "visible" }, { "overflow-y", "visible" },
        { "aspect-ratio", "auto" }, { "opacity", "1" }, { "background-color", "transparent" },
        { "z-index", "auto" }, { "order", "0" }, { "gap", "0" }, { "row-gap", "0" }, { "column-gap", "0" },
    };
    if (strcasecmp(prop, "display") == 0) {
        box->display = CSS_DISPLAY_INLINE;
        box->inline_level = 1;
        return true;
    }
    for (size_t i = 0; i < sizeof(initial_values) / sizeof(initial_values[0]); i++) {
        if (strcasecmp(prop, initial_values[i].prop) != 0) continue;
        CssDeclaration d = { 0 };
        d.property = (char *)initial_values[i].prop;
        d.value = (char *)initial_values[i].value;
        layout_apply_declaration(box, &d, viewport_width, viewport_width, base_url);
        return true;
    }
    /* Not modelled: ignore the declaration rather than parse the keyword. */
    return true;
}

static void style_node(StylePass *P, StyleThread *T, int pos, int idx)
{
    LayoutContext *ctx = P->ctx;
    LayoutBox *box = layout_box(ctx, idx);
    int p = ctx->tree.nodes[idx].parent_idx;
    LayoutBox *parent = (p >= 0) ? layout_box(ctx, p) : NULL;
    int dom = ctx->tree.nodes[idx].dom_node_idx;
    HtmlNode *node = layout_node_dom(ctx, dom);

    /* Custom properties inherit; the map is shared until this node sets one. */
    const CssCustomProps *pv = (p >= 0) ? ctx->custom_props[p] : NULL;
    ctx->custom_props[idx] = pv;

    P->cls[idx] = idx;
    bool share_ok = false;
    bool bits_ready = false;
    int parent_cls = (p >= 0) ? P->cls[p] : -1;
    if (node && node->type == HTML_NODE_ELEMENT) {
        css_style_bloom_enter(T->scratch, P->sd, po_array_parent(&ctx->doc->array, dom));

        /* Style sharing: reuse the style of an element with equal features,
         * an equivalent parent style and equal revalidation results. */
        if (P->share && css_style_shareable(P->sd, dom)) {
            share_ok = true;
            for (HtmlAttribute *attr = node->attributes; attr; attr = attr->next)
                if (strcasecmp(attr->name, "style") == 0) { share_ok = false; break; }
        }
        if (share_ok) {
            CssAtom tag = css_style_tag(P->sd, dom);
            for (int k = 0; k < T->share_n; k++) {
                StyleShareEntry *e = &T->share[k];
                if (e->parent_cls != parent_cls || e->tag != tag) continue;
                if (!css_style_features_equal(P->sd, e->dom, dom)) continue;
                /* Revalidation results are computed only when a candidate
                 * passed the cheap tests. */
                if (P->reval_words && !bits_ready) {
                    css_style_reval_bits(P->set, P->sd, dom, T->bits);
                    bits_ready = true;
                }
                if (P->reval_words && !e->bits_ready) {
                    css_style_reval_bits(P->set, P->sd, e->dom, e->bits);
                    e->bits_ready = true;
                }
                if (P->reval_words &&
                    memcmp(e->bits, T->bits, (size_t)P->reval_words * sizeof(uint64_t)) != 0) continue;
                *box = *layout_box(ctx, e->idx);
                ctx->custom_props[idx] = ctx->custom_props[e->idx];
                P->cls[idx] = P->cls[e->idx];
                if (e->has_after) {
                    if (T->gen_n == T->gen_cap) {
                        T->gen_cap = T->gen_cap ? T->gen_cap * 2 : 16;
                        T->gen = (StyleGenAfter *)realloc(T->gen, (size_t)T->gen_cap * sizeof(StyleGenAfter));
                    }
                    T->gen[T->gen_n].pos = pos;
                    T->gen[T->gen_n].idx = idx;
                    memcpy(T->gen[T->gen_n].text, e->after, sizeof(e->after));
                    T->gen_n++;
                }
                T->shared++;
                css_style_bloom_push(T->scratch, P->sd, dom);
                return;
            }
            T->share_misses++;
        }
    }
    char share_after[256];
    bool share_has_after = false;
    if (node && node->type == HTML_NODE_ELEMENT) {
        const CssAppliedDecl *applied = NULL;
        int count = css_style_match(P->set, P->sd, dom, T->scratch, &applied);

        /* Inline style blocks (every style attribute, in order). */
        InlineBlock inl[4];
        int ninl = 0;
        int n_custom = 0;
        for (HtmlAttribute *attr = node->attributes; attr && ninl < 4; attr = attr->next) {
            if (strcasecmp(attr->name, "style") != 0) continue;
            inl[ninl].decls = css_parse_inline_style(attr->value, &inl[ninl].count);
            for (int i = 0; i < inl[ninl].count; i++) {
                const char *prop = inl[ninl].decls[i].property;
                if (prop && prop[0] == '-' && prop[1] == '-') n_custom++;
            }
            ninl++;
        }
        for (int d = 0; d < count; d++) {
            const char *prop = applied[d].decl->property;
            if (prop && prop[0] == '-' && prop[1] == '-') n_custom++;
        }

        /* Custom properties: stylesheet declarations in cascade order, then
         * inline ones; each value resolves against the map built so far. */
        if (n_custom > 0) {
            CssCustomProps *m = css_custom_props_fork(&T->arena, pv, n_custom);
            for (int d = 0; d < count; d++) {
                const char *prop = applied[d].decl->property;
                const char *val = applied[d].decl->value;
                if (!(prop && prop[0] == '-' && prop[1] == '-')) continue;
                if (css_var_is_self_reference(prop, val)) continue; /* invalid: unset */
                char *resolved = css_var_resolve(val, m);
                css_custom_props_put(&T->arena, m, prop, resolved ? resolved : val);
                free(resolved);
            }
            for (int b = 0; b < ninl; b++) {
                for (int i = 0; i < inl[b].count; i++) {
                    const char *prop = inl[b].decls[i].property;
                    if (!(prop && prop[0] == '-' && prop[1] == '-')) continue;
                    if (css_var_is_self_reference(prop, inl[b].decls[i].value)) continue;
                    char *resolved = css_var_resolve(inl[b].decls[i].value, m);
                    css_custom_props_put(&T->arena, m, prop, resolved ? resolved : inl[b].decls[i].value);
                    free(resolved);
                }
            }
            ctx->custom_props[idx] = m;
        }
        const CssCustomProps *props = ctx->custom_props[idx];

        /* Pseudo-element aspect ratios (e.g. .thumbnail:before{padding-top:56.25%})
         * are applied to the real element since pseudo boxes are not rendered. */
        for (int d = 0; d < count; d++) {
            if (!css_style_rule_has_aspect_base(P->set, applied[d].rule)) continue;
            const char *prop = applied[d].decl->property;
            if (!prop || strcasecmp(prop, "padding-top") != 0) continue;
            if (!css_style_aspect_base_matches(P->set, P->sd, applied[d].rule, dom)) continue;
            double ratio = css_parse_percent_ratio(applied[d].decl->value);
            if (ratio > box->aspect_ratio) box->aspect_ratio = ratio;
        }

        char after_content[256];
        double pseudo_aspect = 0.0;
        bool after_set = style_after_content(P, T, dom, after_content, &pseudo_aspect);

        for (int d = 0; d < count; d++) {
            const char *prop = applied[d].decl->property;
            if (prop && prop[0] == '-' && prop[1] == '-') continue;
            t_var_failed = false;
            char *resolved = css_var_resolve(applied[d].decl->value, props);
            CssDeclaration decl = *applied[d].decl;
            decl.value = resolved ? resolved : applied[d].decl->value;
            if (t_var_failed) layout_unset_property(box, decl.property);
            else if (!layout_apply_wide_keyword(box, parent, decl.property, decl.value,
                                                ctx->viewport_width, ctx->base_url))
                layout_apply_declaration(box, &decl, ctx->viewport_width, ctx->viewport_width,
                                          ctx->base_url);
            free(resolved);
        }
        /* Inline styles override stylesheet rules. */
        for (int b = 0; b < ninl; b++) {
            for (int i = 0; i < inl[b].count; i++) {
                const char *prop = inl[b].decls[i].property;
                if (prop && prop[0] == '-' && prop[1] == '-') continue;
                t_var_failed = false;
                char *resolved = css_var_resolve(inl[b].decls[i].value, props);
                CssDeclaration decl = inl[b].decls[i];
                decl.value = resolved ? resolved : inl[b].decls[i].value;
                if (t_var_failed) layout_unset_property(box, decl.property);
                else if (!layout_apply_wide_keyword(box, parent, decl.property, decl.value,
                                                    ctx->viewport_width, ctx->base_url))
                    layout_apply_declaration(box, &decl, ctx->viewport_width, ctx->viewport_width,
                                              ctx->base_url);
                free(resolved);
            }
            css_declarations_free(inl[b].decls, inl[b].count);
        }
        if (pseudo_aspect > box->aspect_ratio) box->aspect_ratio = pseudo_aspect;

        /* UA rule: metadata elements are always display:none. */
        if (layout_default_display(node->tag_name) == CSS_DISPLAY_NONE) {
            box->display = CSS_DISPLAY_NONE;
        }

        if (after_set && after_content[0] != '\0') {
            share_has_after = true;
            memcpy(share_after, after_content, sizeof(share_after));
            if (T->gen_n == T->gen_cap) {
                T->gen_cap = T->gen_cap ? T->gen_cap * 2 : 16;
                T->gen = (StyleGenAfter *)realloc(T->gen, (size_t)T->gen_cap * sizeof(StyleGenAfter));
            }
            T->gen[T->gen_n].pos = pos;
            T->gen[T->gen_n].idx = idx;
            memcpy(T->gen[T->gen_n].text, after_content, sizeof(after_content));
            T->gen_n++;
        }
    }
    css_style_bloom_push(T->scratch, P->sd, dom);

    /* Inheritance and parent-relative values: the parent is final. */
    if (!box->font_size_set && box->font_size_ratio <= 0.0 && parent) {
        box->font_size = parent->font_size;
    }
    if (box->font_size_ratio > 0.0) {
        double base = parent ? parent->font_size : 16.0;
        if (base <= 0.0) base = 16.0;
        box->font_size = base * box->font_size_ratio;
    }
    if (!parent && box->font_size > 0.0) g_unit_rem = box->font_size;   /* root: rem base */
    if (!box->fill_set && parent) {
        box->fill_kind = parent->fill_kind;
        box->fill_r = parent->fill_r;
        box->fill_g = parent->fill_g;
        box->fill_b = parent->fill_b;
        box->fill_a = parent->fill_a;
    }
    if (!box->color_set && parent) {
        box->color_r = parent->color_r;
        box->color_g = parent->color_g;
        box->color_b = parent->color_b;
        box->color_a = parent->color_a;
    }
    if (!box->text_align_set && parent) box->text_align = parent->text_align;
    if (!box->visibility_set && parent) box->visibility = parent->visibility;
    /* Typography inheritance: font-family, weight, style, line-height and
     * list-style-type are inherited CSS properties.  text-decoration is
     * technically not inherited, but it visually propagates to inline
     * descendants; inheriting-when-unset approximates that well. */
    if (parent) {
        if (!box->font_family[0]) {
            snprintf(box->font_family, sizeof(box->font_family), "%s", parent->font_family);
        }
        if (!box->font_weight_set) box->font_weight = parent->font_weight;
        if (!box->font_italic_set) box->font_italic = parent->font_italic;
        if (!box->text_decoration_set) box->text_decoration = parent->text_decoration;
        if (!box->line_height_set) {
            box->line_height = parent->line_height;
            box->line_height_ratio = parent->line_height_ratio;
        }
        if (!box->list_style_type_set) box->list_style_type = parent->list_style_type;
        if (!box->white_space_set) box->white_space = parent->white_space;
    }
    /* Unitless line-height inherits as a ratio: recompute against the
     * element's own (now final) font-size. */
    if (box->line_height_ratio > 0.0) {
        double fs = box->font_size > 0.0 ? box->font_size : 16.0;
        box->line_height = box->line_height_ratio * fs;
    }
    /* em-unit margins/paddings resolve against the element's own FINAL
     * font-size (CSS 2.1 §8.3/§8.4). */
    if (P->em_defer && box->em_deferred) {
        double fs = box->font_size > 0.0 ? box->font_size : 16.0;
        if (box->em_deferred & 0x01) box->margin_top     = box->margin_em[0] * fs;
        if (box->em_deferred & 0x02) box->margin_right   = box->margin_em[1] * fs;
        if (box->em_deferred & 0x04) box->margin_bottom  = box->margin_em[2] * fs;
        if (box->em_deferred & 0x08) box->margin_left    = box->margin_em[3] * fs;
        if (box->em_deferred & 0x10) box->padding_top    = box->padding_em[0] * fs;
        if (box->em_deferred & 0x20) box->padding_right  = box->padding_em[1] * fs;
        if (box->em_deferred & 0x40) box->padding_bottom = box->padding_em[2] * fs;
        if (box->em_deferred & 0x80) box->padding_left   = box->padding_em[3] * fs;
    }

    /* A vertical legacy -webkit-box (the line-clamp idiom) is a block. */
    if (box->webkit_box && box->display == CSS_DISPLAY_FLEX &&
        (box->box_orient_vertical || box->line_clamp)) {
        box->display = box->webkit_box == 2 ? CSS_DISPLAY_INLINE_BLOCK : CSS_DISPLAY_BLOCK;
        box->inline_level = 0;
    }

    /* Used border widths: unstyled sides have none; styled sides without
     * an explicit width are medium. */
    if (node && node->type == HTML_NODE_ELEMENT) {
        double *bw[4] = { &box->border_left, &box->border_right, &box->border_top, &box->border_bottom };
        for (int k = 0; k < 4; k++) {
            unsigned char bit = (unsigned char)(1u << k);
            if (!(box->border_styled & bit)) *bw[k] = 0.0;
            else if (!(box->border_width_set & bit)) *bw[k] = 3.0;
        }
    }

    /* Blockification (CSS 2.1 §9.7, Flexbox §4): floats, absolutely
     * positioned boxes and flex/grid items have inline-level display values
     * blockified.  The parent's display is final. */
    if (box->display == CSS_DISPLAY_INLINE || box->display == CSS_DISPLAY_INLINE_BLOCK ||
        box->inline_level) {
        bool container_item = parent && (parent->display == CSS_DISPLAY_FLEX ||
                                         parent->display == CSS_DISPLAY_GRID);
        if (container_item || box->float_side != 0 ||
            box->position == CSS_POSITION_ABSOLUTE || box->position == CSS_POSITION_FIXED) {
            if (box->display == CSS_DISPLAY_INLINE || box->display == CSS_DISPLAY_INLINE_BLOCK)
                box->display = CSS_DISPLAY_BLOCK;
            box->inline_level = 0;
        }
    }

    if (share_ok) {
        StyleShareEntry *e = &T->share[T->share_next];
        T->share_next = (T->share_next + 1) % STYLE_SHARE_SLOTS;
        if (T->share_n < STYLE_SHARE_SLOTS) T->share_n++;
        e->idx = idx;
        e->dom = dom;
        e->parent_cls = parent_cls;
        e->tag = css_style_tag(P->sd, dom);
        e->bits_ready = bits_ready;
        if (P->reval_words && bits_ready)
            memcpy(e->bits, T->bits, (size_t)P->reval_words * sizeof(uint64_t));
        e->has_after = share_has_after;
        if (share_has_after) memcpy(e->after, share_after, sizeof(e->after));
    }
}

/* Style the subtree at preorder position `pos`; child subtrees larger than
 * the grain become jobs of their own (their parent is already styled). */
static void style_subtree(StylePass *P, StyleThread *T, Job *self, int pos)
{
    const int *pre = P->ctx->tree.preorder;
    style_node(P, T, pos, pre[pos]);
    int end = P->pre_end[pos];
    for (int c = pos + 1; c < end; c = P->pre_end[c]) {
        if (P->pre_end[c] - c > P->grain) {
            job_spawn(job_new(self->fn, self, P, (uint32_t)c, 0));
        } else {
            style_subtree(P, T, self, c);
        }
    }
}

static void style_job(Job *j)
{
    StylePass *P = (StylePass *)j->ctx;
    style_subtree(P, style_thread(P), j, (int)j->a);
}

static void style_root_job(Job *j)
{
    StylePass *P = (StylePass *)j->ctx;
    int n = P->ctx->tree.count;
    for (int pos = 0; pos < n; pos = P->pre_end[pos])
        job_spawn(job_new(style_job, j, P, (uint32_t)pos, 0));
}

static int style_gen_cmp(const void *a, const void *b)
{
    return ((const StyleGenAfter *)a)->pos - ((const StyleGenAfter *)b)->pos;
}

static void layout_apply_stylesheet(LayoutContext *ctx, CssStylesheet *sheet)
{
    STAGE_BEGIN();
    LayoutStyleSheetList list = {0};
    layout_collect_document_stylesheets(ctx, &list, ctx->base_url);
    if (sheet) layout_sheet_list_add(&list, sheet);
    STAGE("collect sheets");
    layout_sheet_list_resolve(&list);
    STAGE("parse sheets (cached)");

    StylePass P;
    memset(&P, 0, sizeof(P));
    P.ctx = ctx;
    P.em_defer = cyber_getenv("CYBER_DISABLE_EM_DEFER") == NULL;
    const char *genv = getenv("CSS_GRAIN");
    P.grain = (genv && atoi(genv) > 0) ? atoi(genv) : 128;
    bool set_owned = false;
    P.set = layout_styleset_get(&list, ctx->viewport_width, ctx->viewport_height, &set_owned);
    STAGE("compile style set (cached)");
    P.sd = css_styledoc_build(ctx->doc);
    STAGE("element features");
    P.nthreads = job_thread_count();
    {
        const char *sh = getenv("CSS_SHARE");
        int nreval = css_style_reval_count(P.set);
        /* Revalidation costs one chain test per positional selector per
         * element; past a few hundred it costs more than it saves. */
        P.share = !(sh && strcmp(sh, "0") == 0) && nreval <= 512;
        P.reval_words = (nreval + 63) / 64;
    }
    P.cls = (int *)malloc(((size_t)ctx->tree.count + 1) * sizeof(int));
    P.threads = (StyleThread *)calloc((size_t)P.nthreads + 1, sizeof(StyleThread));

    /* Subtree extents in preorder (a subtree is a contiguous range). */
    int n = ctx->tree.count;
    P.pre_end = (int *)malloc(((size_t)n + 1) * sizeof(int));
    int *size = (int *)malloc(((size_t)n + 1) * sizeof(int));
    for (int i = 0; i < n; i++) size[i] = 1;
    for (int pos = n - 1; pos >= 0; pos--) {
        int idx = ctx->tree.preorder[pos];
        int par = ctx->tree.nodes[idx].parent_idx;
        if (par >= 0) size[par] += size[idx];
    }
    for (int pos = 0; pos < n; pos++) P.pre_end[pos] = pos + size[ctx->tree.preorder[pos]];
    free(size);

    STAGE("subtree extents");
    job_graph_run(job_new(style_root_job, NULL, &P, 0, 0));
    STAGE("style (parallel)");

    /* Merge: generated ::after strings in document order, arenas, stats. */
    int gen_total = 0;
    for (int t = 0; t <= P.nthreads; t++) gen_total += P.threads[t].gen_n;
    StyleGenAfter *gen = (StyleGenAfter *)malloc(((size_t)gen_total + 1) * sizeof(StyleGenAfter));
    int g = 0;
    CssMatchStats st = {0};
    uint64_t shared = 0, share_misses = 0;
    for (int t = 0; t <= P.nthreads; t++) {
        StyleThread *T = &P.threads[t];
        if (T->gen_n) memcpy(gen + g, T->gen, (size_t)T->gen_n * sizeof(StyleGenAfter));
        g += T->gen_n;
        free(T->gen);
        shared += T->shared;
        share_misses += T->share_misses;
        frame_arena_absorb(&ctx->arena, &T->arena);
        if (T->scratch) {
            const CssMatchStats *ts = css_style_scratch_stats(T->scratch);
            st.nodes += ts->nodes; st.candidates += ts->candidates;
            st.bloom_rejects += ts->bloom_rejects; st.full_matches += ts->full_matches;
            css_style_scratch_free(T->scratch);
        }
    }
    qsort(gen, (size_t)gen_total, sizeof(StyleGenAfter), style_gen_cmp);
    /* Realize the winning ::after strings (e.g. .hlist li::after " · "). */
    for (int i = 0; i < gen_total; i++) layout_inject_generated_after(ctx, gen[i].idx, gen[i].text);
    free(gen);
    layout_blockify(ctx);
    STAGE("merge");

    uint64_t jobs = 0, steals = 0;
    job_last_stats(&jobs, &steals);
    int chains = 0, universal = 0;
    css_styleset_stats(P.set, &chains, &universal);
    if (g_css_log > 0) fprintf(stderr, "[css] Style: %d sheets, %d rules, %d selectors (%d universal); %llu elements, "
             "%llu candidates, %llu Bloom rejects, %llu full tests; %llu shared (%llu misses, %d reval); "
             "%d threads, %llu jobs, %llu steals\n",
             list.count, css_styleset_rule_count(P.set), chains, universal,
             (unsigned long long)st.nodes, (unsigned long long)st.candidates,
             (unsigned long long)st.bloom_rejects, (unsigned long long)st.full_matches,
             (unsigned long long)shared, (unsigned long long)share_misses, css_style_reval_count(P.set),
             P.nthreads, (unsigned long long)jobs, (unsigned long long)steals);
    LOG_INFO("Style: %d sheets, %d rules, %d threads", list.count, css_styleset_rule_count(P.set), P.nthreads);

    free(P.threads);
    free(P.pre_end);
    free(P.cls);
    css_styledoc_free((CssStyleDoc *)P.sd);
    if (set_owned) css_styleset_free((CssStyleSet *)P.set);
    layout_sheet_list_free(&list);
    STAGE("free");
}

/* ============================================================================
 * Flex layout support
 * ============================================================================ */

static double layout_clamp_size(double value, double min_v, double max_v)
{
    if (min_v > 0 && value < min_v) value = min_v;
    if (max_v > 0 && value > max_v) value = max_v;
    return value;
}

static void layout_update_content_sizes(LayoutBox *box)
{
    box->content_width = box->width - box->padding_left - box->padding_right
                         - box->border_left - box->border_right;
    if (box->content_width < 0) box->content_width = 0;
    if (box->height > 0) {
        box->content_height = box->height - box->padding_top - box->padding_bottom
                              - box->border_top - box->border_bottom;
        if (box->content_height < 0) box->content_height = 0;
    }
}

static double layout_horizontal_border_padding(const LayoutBox *box)
{
    return box->padding_left + box->padding_right +
           box->border_left + box->border_right;
}

static double layout_vertical_border_padding(const LayoutBox *box)
{
    return box->padding_top + box->padding_bottom +
           box->border_top + box->border_bottom;
}

/* Convert a content-box dimension to the total border-box dimension. */
static double layout_content_to_total_width(const LayoutBox *box, double content_width)
{
    return content_width + layout_horizontal_border_padding(box);
}

static double layout_content_to_total_height(const LayoutBox *box, double content_height)
{
    return content_height + layout_vertical_border_padding(box);
}

/* Convert a total border-box dimension to the content-box dimension. */
static double layout_total_to_content_width(const LayoutBox *box, double total_width)
{
    double bp = layout_horizontal_border_padding(box);
    return total_width > bp ? total_width - bp : 0.0;
}

static double layout_total_to_content_height(const LayoutBox *box, double total_height)
{
    double bp = layout_vertical_border_padding(box);
    return total_height > bp ? total_height - bp : 0.0;
}

/* Convert a content-box flex constraint (flex-basis, min-width, max-width) into
 * the total border-box main/cross size used by the flex algorithm.  Values <= 0
 * mean "no constraint" (or a zero flex-basis) and are left as-is. */
static double layout_flex_main_total(const LayoutBox *box, double value, bool is_row)
{
    if (value < 0.0) return value;
    if (box->box_sizing == CSS_BOX_SIZING_CONTENT_BOX) {
        double inset = is_row ? layout_horizontal_border_padding(box)
                              : layout_vertical_border_padding(box);
        return value + inset;
    }
    return value;
}

static double layout_flex_cross_total(const LayoutBox *box, double value, bool is_row)
{
    if (value < 0.0) return value;
    if (box->box_sizing == CSS_BOX_SIZING_CONTENT_BOX) {
        double inset = is_row ? layout_vertical_border_padding(box)
                              : layout_horizontal_border_padding(box);
        return value + inset;
    }
    return value;
}


/* Resolve width/height into the authoritative border-box size stored in
 * box->width / box->height.  content_width / content_height are then derived
 * by subtracting padding and border.  This function must handle both
 * box-sizing models correctly:
 *
 *   content-box: the CSS width/height specifies the content box; padding and
 *                border are added outside.
 *   border-box:  the CSS width/height specifies the full border box.
 *
 * min-width/max-width/min-height/max-height apply to the same box as the
 * width/height property, i.e. content-box for content-box sizing and
 * border-box for border-box sizing.
 */
static LayoutContext *g_layout_ctx;   /* the layout in progress (keyword sizes) */

static bool layout_is_img(const HtmlNode *node)
{
    return node && node->type == HTML_NODE_ELEMENT && node->tag_name &&
           strcasecmp(node->tag_name, "img") == 0;
}

/* Natural size of an <img>: from the decoded image header (the image cache
 * starts the fetch on first use and notifies when the size is known); the
 * width/height attributes stand in while it is unknown.  Thread-safe. */
static bool layout_img_natural(const LayoutContext *ctx, HtmlNode *node, double *nw, double *nh)
{
    *nw = *nh = 0.0;
    ImageCache *ic = display_list_get_image_cache();
    if (ic) {
        char *url = display_list_image_source(ctx ? ctx->base_url : "", node, 0.0f);
        if (url) {
            int w = 0, h = 0;
            bool ok = image_cache_natural_size(ic, url, &w, &h);
            free(url);
            if (ok) { *nw = w; *nh = h; return true; }
        }
    }
    return false;
}

static double layout_attr_px(HtmlNode *node, const char *name)
{
    const char *v = layout_node_attribute(node, name);
    if (!v || !v[0] || strchr(v, '%')) return 0.0;
    double d = atof(v);
    return d > 0.0 ? d : 0.0;
}

/* Used content size of an <img> (CSS 2.1 §10.3.2 / §10.6.2): definite CSS
 * sizes win (the width/height attributes act as CSS sizes when CSS says
 * auto); a missing dimension follows the natural aspect ratio; with both
 * auto the natural size is used.  Unknown natural size (still loading):
 * 0x0 unless sized by CSS/attributes. */
static void layout_img_size(const LayoutContext *ctx, HtmlNode *node, double cw_in, bool w_auto,
                            double ch_in, bool h_auto, double *cw_out, double *ch_out)
{
    double nw, nh;
    bool known = layout_img_natural(ctx, node, &nw, &nh);
    double aw = layout_attr_px(node, "width"), ah = layout_attr_px(node, "height");
    double cw = cw_in, ch = ch_in;
    if (w_auto && aw > 0.0) { cw = aw; w_auto = false; }
    if (h_auto && ah > 0.0) { ch = ah; h_auto = false; }
    double ratio = known && nh > 0.0 ? nw / nh : (aw > 0.0 && ah > 0.0 ? aw / ah : 0.0);
    if (w_auto && h_auto) {
        cw = known ? nw : 0.0;
        ch = known ? nh : 0.0;
    } else if (w_auto) {
        cw = ratio > 0.0 ? ch * ratio : (known ? nw : 0.0);
    } else if (h_auto) {
        ch = ratio > 0.0 ? cw / ratio : (known ? nh : 0.0);
    }
    *cw_out = cw;
    *ch_out = ch;
}
static double layout_intrinsic_width(LayoutContext *ctx, int idx, bool want_max);

static void layout_resolve_used_sizes(LayoutBox *box, HtmlNode *node,
                                      double parent_content_width,
                                      double parent_content_height)
{
    /* Throughout the engine box->width / box->height are the total border-box
     * sizes, and content_width / content_height are derived from them.  This
     * function converts CSS-used values (which may be content-box or border-box
     * depending on box-sizing) into those totals. */

    /* Percentage margins/paddings first: the width below depends on them. */
    layout_resolve_side_pcts(box, parent_content_width);
    layout_resolve_minmax_pcts(box, parent_content_width, parent_content_height);

    /* ---------- width ---------- */
    bool width_auto = true;
    double used_content_width = 0.0; /* valid when !width_auto */
    double used_total_width = 0.0;   /* valid when !width_auto */

    if (box->width_percent > 0.0) {
        if (box->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
            used_total_width = parent_content_width * box->width_percent + box->width_pct_px;
        } else {
            used_content_width = parent_content_width * box->width_percent + box->width_pct_px;
        }
        width_auto = false;
    } else if (box->width_set) {
        /* Explicit width, possibly zero (an explicit 0 is a real size). */
        if (box->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
            used_total_width = box->css_width;
        } else {
            used_content_width = box->css_width;
        }
        width_auto = false;
    }

    if (width_auto && box->width_keyword && g_layout_ctx &&
        box >= g_layout_ctx->boxes && box < g_layout_ctx->boxes + g_layout_ctx->tree.count) {
        int bidx = (int)(box - g_layout_ctx->boxes);
        double mx = layout_intrinsic_width(g_layout_ctx, bidx, true);
        if (box->width_keyword == 2) used_total_width = mx;
        else {
            double mn = layout_intrinsic_width(g_layout_ctx, bidx, false);
            if (box->width_keyword == 1) used_total_width = mn;
            else {
                double avail = parent_content_width - box->margin_left - box->margin_right;
                used_total_width = mx < avail ? mx : (mn > avail ? mn : avail);
            }
        }
        width_auto = false;
        /* used_total_width is already a border-box size */
        if (box->box_sizing == CSS_BOX_SIZING_CONTENT_BOX)
            used_content_width = used_total_width - layout_horizontal_border_padding(box);
    } else if (width_auto && layout_is_img(node)) {
        /* Replaced element: natural size / aspect ratio, not the
         * containing block width. */
        double ch_spec = 0.0;
        bool h_auto = true;
        if (box->height_set && !(box->height_percent > 0.0)) {
            ch_spec = box->box_sizing == CSS_BOX_SIZING_BORDER_BOX
                ? box->css_height - layout_vertical_border_padding(box) : box->css_height;
            h_auto = false;
        } else if (box->height_percent > 0.0 && parent_content_height > 0.0) {
            ch_spec = parent_content_height * box->height_percent + box->height_pct_px;
            if (box->box_sizing == CSS_BOX_SIZING_BORDER_BOX) ch_spec -= layout_vertical_border_padding(box);
            h_auto = false;
        }
        double cw = 0.0, ch = 0.0;
        layout_img_size(g_layout_ctx, node, 0.0, true, ch_spec, h_auto, &cw, &ch);
        used_content_width = cw;
        used_total_width = layout_content_to_total_width(box, cw);
        width_auto = false;
        if (box->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
            /* the content-box branch below converts; keep totals consistent */
        }
    } else if (width_auto) {
        if (box->display == CSS_DISPLAY_INLINE) {
            /* Inline boxes shrink-to-fit: leave the width to the inline flow,
             * which measures the text runs (an empty inline stays 0 wide). */
            used_total_width = 0.0;
        } else {
            /* Auto width fills the containing block content width; that is the
             * total border-box width before margins are applied. */
            used_total_width = parent_content_width - box->margin_left - box->margin_right;
            if (used_total_width < 0.0) used_total_width = 0.0;
        }
    } else if (box->box_sizing == CSS_BOX_SIZING_CONTENT_BOX) {
        used_total_width = layout_content_to_total_width(box, used_content_width);
    }

    /* min/max-width constrains the same box that width specified. */
    if (box->min_width > 0.0 || box->max_width > 0.0) {
        if (box->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
            used_total_width = layout_clamp_size(used_total_width,
                                                  box->min_width, box->max_width);
        } else {
            /* For auto width the content width derives from the total just
             * computed above; clamping the (never assigned) used_content_width
             * would collapse the box to zero. */
            double cw = width_auto
                ? used_total_width - layout_horizontal_border_padding(box)
                : used_content_width;
            cw = layout_clamp_size(cw, box->min_width, box->max_width);
            used_total_width = layout_content_to_total_width(box, cw);
        }
    }
    box->width = used_total_width;

    /* ---------- height ---------- */
    bool height_auto = true;
    double used_content_height = 0.0;
    double used_total_height = 0.0;

    if (box->height_percent > 0.0) {
        /* Percentage height resolves against the containing block height.
         * When that height is indefinite (depends on content), the
         * percentage computes to auto (CSS 2.1 §10.5); the box is flagged so
         * auto-height logic treats it as height:auto for this layout, and a
         * later layout against a definite height (flex stretch) resolves it. */
        box->height_pct_unresolved = 0;
        if (parent_content_height > 0.0) {
            if (box->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
                used_total_height = parent_content_height * box->height_percent + box->height_pct_px;
            } else {
                used_content_height = parent_content_height * box->height_percent + box->height_pct_px;
            }
            height_auto = false;
        } else {
            box->height_pct_unresolved = 1;
        }
    }
    if (height_auto && box->height_set && !(box->height_percent > 0.0)) {
        /* Explicit height, possibly zero. */
        if (box->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
            used_total_height = box->css_height;
        } else {
            used_content_height = box->css_height;
        }
        height_auto = false;
    } else if (height_auto && box->aspect_ratio > 0.0) {
        /* The aspect-ratio property relates the box box-sizing names (the
         * border box for border-box); a ::before placeholder ratio relates
         * the content box. */
        if (box->aspect_ratio_prop && box->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
            used_total_height = box->width * box->aspect_ratio;
        } else {
            double content_width = layout_total_to_content_width(box, box->width);
            used_content_height = content_width * box->aspect_ratio;
        }
        height_auto = false;
    }

    if (height_auto && layout_is_img(node)) {
        /* Replaced element: height from the used width and aspect ratio. */
        double cw = box->width - layout_horizontal_border_padding(box);
        if (cw < 0.0) cw = 0.0;
        double w2 = 0.0, ch = 0.0;
        layout_img_size(g_layout_ctx, node, cw, false, 0.0, true, &w2, &ch);
        box->height = layout_content_to_total_height(box, ch);
    } else if (height_auto) {
        /* Height stays 0; it will be resolved from children in the bottom-up pass. */
        box->height = 0.0;
    } else if (box->box_sizing == CSS_BOX_SIZING_CONTENT_BOX) {
        box->height = layout_content_to_total_height(box, used_content_height);
    } else {
        /* border-box: used_total_height already holds the total for explicit
         * and percentage sizes; aspect-ratio produced a content height that
         * must still be converted to a total. */
        if (box->aspect_ratio > 0.0 && !box->aspect_ratio_prop &&
            !(box->height_percent > 0.0) && box->css_height == 0.0) {
            box->height = layout_content_to_total_height(box, used_content_height);
        } else {
            box->height = used_total_height;
        }
    }

    /* min/max-height constrains the same box that height specified.  An
     * auto height stays 0 here (resolved from content by the formatting
     * context, which applies the clamp then). */
    if (!height_auto && (box->min_height > 0.0 || box->max_height > 0.0)) {
        if (box->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
            box->height = layout_clamp_size(box->height,
                                            box->min_height, box->max_height);
        } else {
            double ch = layout_total_to_content_height(box, box->height);
            ch = layout_clamp_size(ch, box->min_height, box->max_height);
            box->height = layout_content_to_total_height(box, ch);
        }
    }

    layout_update_content_sizes(box);
}

/* ============================================================================
 * Serial layout core
 *
 * Replaces the earlier parallel top-down/bottom-up + spin-wait model with a
 * single recursive depth-first pass.  For each container a formatting context
 * (block or flex) positions and sizes its in-flow children, recursing so that
 * every child's border box is final before the parent computes its own
 * content height.  This makes height resolution (auto height = extent of
 * children; percentage height against the parent's *resolved* height) fall
 * out naturally and correctly.
 *
 * The function set is file-local; the public contract (LayoutBox fields read
 * by display_list/dom_api/main) is unchanged.
 * ============================================================================ */

/* Per-child cursor carried down a sibling chain for normal block flow.  It is
 * a stack value in layout_flow_children, never stored on the box, which keeps
 * flow positioning free of the previous-sibling dependency that the old
 * parent->line_y_offset model suffered from. */
typedef struct {
    double x;          /* current content-left for this line          */
    double y;          /* current vertical cursor (top of next child) */
    double line_top;   /* top of the current inline line              */
    double line_box;   /* tallest item on the current inline line     */
} FlowCursor;

/* ---------------------------------------------------------------------------
 * Inline text measurement helpers.  When the default font is available,
 * inline children are measured with real glyph metrics so text runs share
 * lines like a browser; otherwise the coarse fallbacks are used.
 * ------------------------------------------------------------------------- */

/* Per-line vertical advance for inline content at a given font size. */
static double layout_line_advance(double font_size)
{
    return font_size * 1.5;
}

static int layout_font_slot(const LayoutBox *box);

/* Height of an inline box's content area (ascent + descent of its font,
 * rounded like browsers do); getBoundingClientRect of a non-replaced inline
 * element covers this, centered in the line-height (half-leading). */
static double layout_content_area_height(const LayoutBox *box)
{
    double fs = box && box->font_size > 0.0 ? box->font_size : 16.0;
    TextShaper *font = display_list_get_font(layout_font_slot(box));
    if (!font) font = display_list_get_default_font();
    float a, d, g;
    if (font && text_shaper_vmetrics(font, &a, &d, &g)) return roundf(a * (float)fs) + roundf(d * (float)fs);
    return fs * 1.15;
}

/* Line advance honoring an explicit CSS line-height; `normal` uses the
 * font's own ascent + descent + line gap, as browsers do. */
static double layout_line_advance_box(const LayoutBox *box)
{
    if (box && box->line_height > 0.0) return box->line_height;
    double fs = box && box->font_size > 0.0 ? box->font_size : 16.0;
    TextShaper *font = display_list_get_font(layout_font_slot(box));
    if (!font) font = display_list_get_default_font();
    float lh = font ? text_shaper_normal_line_height(font, (float)fs) : 0.0f;
    if (lh > 0.5f * fs && lh < 3.0f * fs) return lh;
    return layout_line_advance(fs);
}

/* Resolve the font slot for a box's typography (sans/serif/mono x bold x italic). */
static int layout_font_slot(const LayoutBox *box)
{
    if (!box) return 0;
    return display_list_resolve_font_slot(box->font_family, box->font_weight,
                                          box->font_italic ? 1 : 0);
}

static bool layout_text_is_whitespace(const char *s)
{
    if (!s) return true;
    for (; *s; s++) {
        if (!isspace((unsigned char)*s)) return false;
    }
    return true;
}

/* Measure text at a given font size with the box's resolved font. */
static bool layout_measure_text_styled(const LayoutBox *box, const char *text,
                                       double font_size,
                                       double *out_w, double *out_h)
{
    TextShaper *font = display_list_get_font(layout_font_slot(box));
    if (!font) font = display_list_get_default_font();
    if (!font || !text || !text[0]) return false;
    float mw = 0.0f, mh = 0.0f;
    if (!text_shaper_measure(font, text, &mw, &mh)) return false;
    float scale = (float)(font_size / 16.0);
    if (out_w) *out_w = (double)mw * scale;
    if (out_h) *out_h = (double)mh * scale;
    if (g_layout_debug & LDBG_MEASURE) {
        fprintf(stderr, "[MEAS] fs=%.2f slot=%d mw=%.4f out=%.4f text=%.30s\n",
                font_size, layout_font_slot(box), mw, out_w ? *out_w : -1.0, text);
    }
    return true;
}

/* Concatenate descendant text of a DOM node (used to size inline elements). */
static void layout_concat_dom_text(HtmlDocument *doc, HtmlNode *node,
                                   char *buf, size_t bufsz, size_t *len)
{
    for (HtmlNode *c = html_node_first_child(doc, node); c && *len + 1 < bufsz;
         c = html_node_next_sibling(doc, c)) {
        if (c->type == HTML_NODE_TEXT && c->text_content) {
            size_t tl = strlen(c->text_content);
            if (*len + tl >= bufsz) tl = bufsz - 1 - *len;
            memcpy(buf + *len, c->text_content, tl);
            *len += tl;
            buf[*len] = '\0';
        } else if (c->type == HTML_NODE_ELEMENT) {
            layout_concat_dom_text(doc, c, buf, bufsz, len);
        }
    }
}

static void layout_node_serial(LayoutContext *ctx, int idx);

/* Shift a box and all of its descendants by (dx,dy).  Fixed boxes are skipped
 * (they position against the viewport). */
static void layout_offset_subtree(LayoutContext *ctx, int idx, double dx, double dy)
{
    if (dx == 0.0 && dy == 0.0) return;
    LayoutNodeRef *node = layout_node_ref(ctx, idx);
    for (int c = node->first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        LayoutBox *child = layout_box(ctx, c);
        if (child->position == CSS_POSITION_FIXED) continue;
        child->x += dx;
        child->y += dy;
        layout_offset_subtree(ctx, c, dx, dy);
    }
}

/* Is this box in normal flow (participates in its parent's flow)? */
static inline bool layout_is_in_flow(const LayoutBox *box)
{
    if (box->display == CSS_DISPLAY_NONE) return false;
    if (box->position == CSS_POSITION_ABSOLUTE ||
        box->position == CSS_POSITION_FIXED) return false;
    return true;
}
/* ============================================================================
 * Serial layout core — flow positioning, flex, height resolution
 * ============================================================================ */

/* Collect the in-flow children of a container into a dense array.  Returns a
 * malloc'd int array (caller frees) or NULL if there are none. */
static int* layout_collect_flow_children(LayoutContext *ctx, int idx, int *out_count)
{
    *out_count = 0;
    LayoutNodeRef *node = layout_node_ref(ctx, idx);
    int cap = node->child_count > 0 ? node->child_count : 8;
    int *kids = (int*)malloc((size_t)cap * sizeof(int));
    if (!kids) return NULL;
    int n = 0;
    for (int c = node->first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        LayoutBox *child = layout_box(ctx, c);
        if (!layout_is_in_flow(child)) continue;
        if (n < cap) kids[n++] = c;
    }
    if (n == 0) { free(kids); return NULL; }
    *out_count = n;
    return kids;
}

/* Shift the inline runs of one finished line for text-align center/right.
 * `members` are the layout-tree indices of the inline children placed on the
 * line.  Lines containing a word-wrapped text box are left alone: their slack
 * is ~0 by construction, and the wrapped box cannot be partially shifted. */
static void layout_align_line(LayoutContext *ctx, const int *members, int count,
                              double content_left, double avail_width, CssTextAlign align)
{
    if (align == CSS_TEXT_ALIGN_LEFT || count <= 0) return;
    for (int i = 0; i < count; i++) {
        if (layout_box(ctx, members[i])->wrap_cont_w > 0.0) return;
    }
    LayoutBox *last = layout_box(ctx, members[count - 1]);
    double used = (last->x + last->width) - content_left;
    double slack = avail_width - used;
    if (slack <= 0.5) return;
    double dx = (align == CSS_TEXT_ALIGN_CENTER) ? slack * 0.5 : slack;
    for (int i = 0; i < count; i++) {
        LayoutBox *b = layout_box(ctx, members[i]);
        b->x += dx;
        layout_offset_subtree(ctx, members[i], dx, 0.0);
    }
}

/* Record a placed float on the context stack (grows on demand). */
static void layout_float_push(LayoutContext *ctx, const FloatRec *rec)
{
    if (!ctx->float_stack) return;
    if (ctx->float_count >= ctx->float_cap) {
        int new_cap = ctx->float_cap ? ctx->float_cap * 2 : 64;
        FloatRec *ns = (FloatRec*)realloc(ctx->float_stack, (size_t)new_cap * sizeof(FloatRec));
        if (!ns) return;
        ctx->float_stack = ns;
        ctx->float_cap = new_cap;
    }
    ctx->float_stack[ctx->float_count++] = *rec;
}

/* Total intrusion of active floats into the line at vertical position y.
 * A float only intrudes when it horizontally overlaps the line's content
 * range [content_left, content_right] — a float in another column must not
 * shrink this column's lines. */
static void layout_float_insets(const FloatRec *floats, int n, double y,
                                double content_left, double content_right,
                                double *out_left_in, double *out_right_in)
{
    double li = 0.0, ri = 0.0;
    for (int i = 0; i < n; i++) {
        if (y < floats[i].bottom - 0.001) {
            if (floats[i].side == 1) {
                if (floats[i].right > content_left && floats[i].left < content_right) {
                    double v = floats[i].right - content_left;
                    if (v > li) li = v;
                }
            } else {
                if (floats[i].left < content_right && floats[i].right > content_left) {
                    double v = content_right - floats[i].left;
                    if (v > ri) ri = v;
                }
            }
        }
    }
    *out_left_in = li;
    *out_right_in = ri;
}

/* Effective line start/width at vertical position y given placed floats. */
static void layout_flow_line_geometry(const FloatRec *floats, int n, double y,
                                      double content_left, double avail_width,
                                      double *line_left, double *line_avail)
{
    double li = 0.0, ri = 0.0;
    layout_float_insets(floats, n, y, content_left, content_left + avail_width, &li, &ri);
    *line_left = content_left + li;
    *line_avail = avail_width - li - ri;
    if (*line_avail < 0.0) *line_avail = 0.0;
}

/* Replaced form/embedded elements keep a default object size even when they
 * have no text content (unlike empty phrasing elements, which shrink to 0). */
static bool layout_is_replaced_form_element(const char *tag)
{
    static const char *tags[] = {
        "input", "button", "select", "textarea", "iframe",
        "canvas", "object", "audio", "video", "embed", NULL
    };
    if (!tag) return false;
    for (int i = 0; tags[i]; i++) {
        if (strcasecmp(tag, tags[i]) == 0) return true;
    }
    return false;
}

/* Used size of an inline <svg>, a replaced element: CSS width/height (px, or
 * a percentage of the parent's content box when that is definite), else its
 * width/height attributes, else what the viewBox ratio gives from the other
 * dimension, else 300x150 (CSS replaced-element defaults). */
static void layout_svg_size(LayoutContext *ctx, int c, HtmlNode *dom, double *out_w, double *out_h)
{
    LayoutBox *b = layout_box(ctx, c);
    int p = ctx->tree.nodes[c].parent_idx;
    const LayoutBox *pb = p >= 0 ? layout_box(ctx, p) : NULL;
    double pcw = pb ? pb->width - layout_horizontal_border_padding(pb) : 0.0;
    double pch = pb && !layout_height_auto(pb) ? pb->height - layout_vertical_border_padding(pb) : 0.0;
    double w = -1.0, h = -1.0;
    if (b->width_percent > 0.0) { if (pcw > 0.0) w = pcw * b->width_percent / 100.0 + b->width_pct_px; }
    else if (b->width_set) w = b->css_width;
    if (b->height_percent > 0.0) { if (pch > 0.0) h = pch * b->height_percent / 100.0 + b->height_pct_px; }
    else if (b->height_set) h = b->css_height;
    const char *wa = layout_node_attribute(dom, "width");
    const char *ha = layout_node_attribute(dom, "height");
    if (w < 0.0 && wa && wa[0] && !strchr(wa, '%')) w = atof(wa);
    if (h < 0.0 && ha && ha[0] && !strchr(ha, '%')) h = atof(ha);
    double ratio = 0.0;   /* width / height */
    const char *vb = layout_node_attribute(dom, "viewbox");
    if (vb) {
        double v[4];
        if (sscanf(vb, "%lf%*[ ,]%lf%*[ ,]%lf%*[ ,]%lf", &v[0], &v[1], &v[2], &v[3]) == 4 && v[3] > 0.0)
            ratio = v[2] / v[3];
    }
    if (w < 0.0) w = (h >= 0.0 && ratio > 0.0) ? h * ratio : 300.0;
    if (h < 0.0) h = ratio > 0.0 ? w / ratio : 150.0;
    *out_w = w;
    *out_h = h;
}

/* Find the first <img> descendant and return its width/height attributes.
 * Used to size inline elements (like <a><img></a>) that wrap an image. */
static bool layout_first_img_size(HtmlDocument *doc, HtmlNode *node,
                                  double *out_w, double *out_h)
{
    for (HtmlNode *c = html_node_first_child(doc, node); c;
         c = html_node_next_sibling(doc, c)) {
        if (c->type != HTML_NODE_ELEMENT) continue;
        if (strcasecmp(c->tag_name, "img") == 0) {
            double w = 0.0, h = 0.0;
            layout_img_size(g_layout_ctx, c, 0.0, true, 0.0, true, &w, &h);
            if (w > 0.0) {
                *out_w = w;
                *out_h = h > 0.0 ? h : w;
                return true;
            }
        }
        if (layout_first_img_size(doc, c, out_w, out_h)) return true;
    }
    return false;
}

/* ---- Margin collapsing (CSS 2.1 §8.3.1) ----
 * layout_eff_margin_top/bottom return the collapse-aware vertical margin a
 * block-level box presents to its parent's block flow:
 * - a box's top margin collapses with its first in-flow block child's top
 *   margin when the box has no top border/padding;
 * - symmetrically at the bottom when the box's height is auto (not set);
 * - a box with no significant in-flow children is empty: its top and bottom
 *   margins collapse together into a single max();
 * - an inline-level first/last child forms a line box, which blocks the
 *   collapse.  Whitespace-only text is not significant. */
static void layout_sig_children(LayoutContext *ctx, int idx, int *out_first, int *out_last)
{
    *out_first = *out_last = -1;
    for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
         c = ctx->tree.nodes[c].next_sibling_idx) {
        LayoutBox *ch = layout_box(ctx, c);
        if (!layout_is_in_flow(ch) || ch->float_side != 0) continue;
        HtmlNode *dn = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
        if (dn && dn->type == HTML_NODE_TEXT &&
            layout_text_is_whitespace(dn->text_content)) continue;
        if (*out_first < 0) *out_first = c;
        *out_last = c;
    }
}

/* A childless box's own top and bottom margins are adjoining only when
 * nothing separates them: no height, min-height, padding or border. */
static bool layout_box_collapses_through(const LayoutBox *box)
{
    if (box->height_set && box->css_height > 0.0) return false;
    if (box->height_percent > 0.0 || box->min_height > 0.0 || box->aspect_ratio > 0.0) return false;
    if (box->padding_top > 0.0 || box->padding_bottom > 0.0) return false;
    if (box->border_top > 0.0 || box->border_bottom > 0.0) return false;
    return true;
}

static double layout_eff_margin_top(LayoutContext *ctx, int idx, int depth)
{
    LayoutBox *box = layout_box(ctx, idx);
    double mt = box->margin_top, mb = box->margin_bottom;
    if (depth > 8) return mt;
    int f, l;
    layout_sig_children(ctx, idx, &f, &l);
    if (f < 0) {
        /* Empty box: margins collapse through only when the box does NOT
         * establish a new block formatting context and has no height,
         * padding or border separating them (CSS 2.1 §8.3.1). */
        if (box->overflow != 0 || !layout_box_collapses_through(box)) return mt;
        return mt > mb ? mt : mb;
    }
    if (box->overflow != 0) return mt;    /* BFC: no collapse with children */
    /* Flex/grid containers establish a flex/grid formatting context: their
     * margins never collapse with their items' margins. */
    if (box->display == CSS_DISPLAY_FLEX || box->display == CSS_DISPLAY_GRID) return mt;
    if (box->padding_top > 0.0 || box->border_top > 0.0) return mt;
    if (!layout_box_is_block_level(layout_box(ctx, f))) return mt;
    double c = layout_eff_margin_top(ctx, f, depth + 1);
    return mt > c ? mt : c;
}

static double layout_eff_margin_bottom(LayoutContext *ctx, int idx, int depth)
{
    LayoutBox *box = layout_box(ctx, idx);
    double mt = box->margin_top, mb = box->margin_bottom;
    if (depth > 8) return mb;
    int f, l;
    layout_sig_children(ctx, idx, &f, &l);
    if (f < 0) {
        if (box->overflow != 0 || !layout_box_collapses_through(box)) return mb;
        return mt > mb ? mt : mb;
    }
    if (box->overflow != 0) return mb;    /* BFC: no collapse with children */
    if (box->display == CSS_DISPLAY_FLEX || box->display == CSS_DISPLAY_GRID) return mb;
    if (box->padding_bottom > 0.0 || box->border_bottom > 0.0) return mb;
    if (!layout_height_auto(box)) return mb;     /* explicit height blocks */
    if (!layout_box_is_block_level(layout_box(ctx, l))) return mb;
    double c = layout_eff_margin_bottom(ctx, l, depth + 1);
    return mb > c ? mb : c;
}

/* Position in-flow children in a block formatting context and recurse into
 * each.  The box itself is already positioned and width-resolved by the
 * caller; this resolves each child's width/height/position and, after
 * recursing, the parent's auto height. */
/* ---- Intrinsic sizes (CSS Sizing 3 §5) ----
 * Min-content and max-content border-box widths, excluding margins.  Used
 * for shrink-to-fit (atomic inlines, floats, absolutely positioned boxes)
 * and for flex items' content-based sizes.  Memoized per layout run in
 * ctx->intrinsic_cache (two slots per box, < 0 = not computed). */

static double layout_text_intrinsic(const LayoutBox *box, const char *text, bool want_max)
{
    double fs = box->font_size > 0.0 ? box->font_size : 16.0;
    char buf[4096];
    size_t n = 0;
    double best = 0.0;
    const char *p = text;
    /* Preserved white space (pre, and pre-wrap's max-content): each line
     * is measured as written, spaces included. */
    if (box->white_space == 2 || (box->white_space == 3 && want_max)) {
        while (*p) {
            const char *st = p;
            while (*p && *p != '\n') p++;
            size_t ll = (size_t)(p - st);
            if (ll >= sizeof(buf)) ll = sizeof(buf) - 1;
            memcpy(buf, st, ll);
            buf[ll] = '\0';
            double w = 0.0;
            if (ll > 0 && layout_measure_text_styled(box, buf, fs, &w, NULL) && w > best) best = w;
            if (*p) p++;
        }
        return best;
    }
    /* Collapse runs of white space; for min-content measure each word. */
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *st = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        size_t wl = (size_t)(p - st);
        if (want_max) {
            if (n > 0 && n + 1 < sizeof(buf)) buf[n++] = ' ';
            if (n + wl >= sizeof(buf)) wl = sizeof(buf) - 1 - n;
            memcpy(buf + n, st, wl);
            n += wl;
        } else {
            char word[512];
            if (wl >= sizeof(word)) wl = sizeof(word) - 1;
            memcpy(word, st, wl);
            word[wl] = '\0';
            double w = 0.0;
            if (layout_measure_text_styled(box, word, fs, &w, NULL) && w > best) best = w;
        }
    }
    if (want_max) {
        buf[n] = '\0';
        if (n == 0) return 0.0;
        double w = 0.0;
        if (layout_measure_text_styled(box, buf, fs, &w, NULL)) best = w;
    }
    return best;
}

static double layout_intrinsic_width(LayoutContext *ctx, int idx, bool want_max);

const char *css_layout_box_text(const LayoutContext *ctx, int idx, const char *raw)
{
    if (ctx && ctx->text_norm && idx >= 0 && idx < ctx->tree.count && ctx->text_norm[idx])
        return ctx->text_norm[idx];
    return raw;
}

/* Margin-box contribution of a child (auto margins count as zero). */
static double layout_intrinsic_contribution(LayoutContext *ctx, int c, bool want_max)
{
    LayoutBox *b = layout_box(ctx, c);
    double w = layout_intrinsic_width(ctx, c, want_max);
    if (!(b->margin_auto & 8)) w += b->margin_left;
    if (!(b->margin_auto & 2)) w += b->margin_right;
    return w < 0.0 ? 0.0 : w;
}

static double layout_intrinsic_compute(LayoutContext *ctx, int idx, bool want_max)
{
    LayoutBox *box = layout_box(ctx, idx);
    if (box->display == CSS_DISPLAY_NONE) return 0.0;
    HtmlNode *dom = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    if (dom && dom->type == HTML_NODE_TEXT)
        return layout_text_intrinsic(box, dom->text_content ? dom->text_content : "",
                                     want_max || box->white_space == 1 || box->white_space == 2);
    if (!dom || dom->type != HTML_NODE_ELEMENT) return 0.0;

    double hpb = layout_horizontal_border_padding(box);
    /* Compressible replaced elements (form controls, images, ...) with a
     * percentage width contribute nothing to min-content (Sizing §5.2.2). */
    if (!want_max && box->width_percent > 0.0 && dom->tag_name &&
        (strcasecmp(dom->tag_name, "input") == 0 || strcasecmp(dom->tag_name, "textarea") == 0 ||
         strcasecmp(dom->tag_name, "select") == 0 || strcasecmp(dom->tag_name, "img") == 0 ||
         strcasecmp(dom->tag_name, "video") == 0 || strcasecmp(dom->tag_name, "iframe") == 0 ||
         strcasecmp(dom->tag_name, "canvas") == 0 || strcasecmp(dom->tag_name, "svg") == 0))
        return hpb;
    /* A definite (non-percentage) width is both intrinsic sizes. */
    if (box->width_set && box->width_percent <= 0.0) {
        double t = box->box_sizing == CSS_BOX_SIZING_BORDER_BOX ? box->css_width
                                                                : box->css_width + hpb;
        return t < 0.0 ? 0.0 : t;
    }

    double content = 0.0;
    const char *tag = dom->tag_name ? dom->tag_name : "";
    if (strcasecmp(tag, "img") == 0) {
        double ch_spec = 0.0;
        bool h_auto = !(box->height_set && !(box->height_percent > 0.0));
        if (!h_auto) ch_spec = box->box_sizing == CSS_BOX_SIZING_BORDER_BOX
            ? box->css_height - layout_vertical_border_padding(box) : box->css_height;
        double ch = 0.0;
        layout_img_size(ctx, dom, 0.0, true, ch_spec, h_auto, &content, &ch);
    } else if (strcasecmp(tag, "input") == 0) {
        const char *ty = layout_node_attribute(dom, "type");
        if (ty && (strcasecmp(ty, "checkbox") == 0 || strcasecmp(ty, "radio") == 0)) content = 13.0;
        else if (ty && strcasecmp(ty, "hidden") == 0) content = 0.0;
        else content = 177.0;
    } else if (strcasecmp(tag, "textarea") == 0) {
        content = 176.0;
    } else if (strcasecmp(tag, "svg") == 0 || strcasecmp(tag, "canvas") == 0 ||
               strcasecmp(tag, "video") == 0 || strcasecmp(tag, "iframe") == 0) {
        const char *wa = layout_node_attribute(dom, "width");
        content = (wa && wa[0]) ? atof(wa)
                                : (strcasecmp(tag, "svg") == 0 ? 300.0 :
                                   strcasecmp(tag, "canvas") == 0 ? 300.0 :
                                   strcasecmp(tag, "iframe") == 0 ? 300.0 : 300.0);
    } else if (box->display == CSS_DISPLAY_FLEX) {
        bool is_row = box->flex_direction == CSS_FLEX_DIRECTION_ROW ||
                      box->flex_direction == CSS_FLEX_DIRECTION_ROW_REVERSE;
        bool wraps = box->flex_wrap != CSS_FLEX_WRAP_NOWRAP;
        int n = 0;
        for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
             c = ctx->tree.nodes[c].next_sibling_idx) {
            LayoutBox *cb = layout_box(ctx, c);
            if (!layout_is_in_flow(cb)) continue;
            HtmlNode *cn = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
            if (cn && cn->type == HTML_NODE_TEXT && layout_text_is_whitespace(cn->text_content))
                continue;
            double w = layout_intrinsic_contribution(ctx, c, want_max);
            if (is_row && (want_max || !wraps)) content += w;
            else if (w > content) content = w;
            n++;
        }
        if (is_row && (want_max || !wraps) && n > 1) content += (n - 1) * box->gap_col;
    } else {
        /* Block / inline formatting: inline-level children accumulate along
         * a line (max-content) while block-level children stand alone. */
        double line = 0.0, pending_space = 0.0;
        for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
             c = ctx->tree.nodes[c].next_sibling_idx) {
            LayoutBox *cb = layout_box(ctx, c);
            if (cb->display == CSS_DISPLAY_NONE) continue;
            if (cb->position == CSS_POSITION_ABSOLUTE || cb->position == CSS_POSITION_FIXED) continue;
            HtmlNode *cn = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
            if (cn && cn->type == HTML_NODE_ELEMENT && cn->tag_name &&
                strcasecmp(cn->tag_name, "br") == 0) {
                if (line > content) content = line;
                line = 0.0;
                pending_space = 0.0;
                continue;
            }
            if (cn && cn->type == HTML_NODE_TEXT && layout_text_is_whitespace(cn->text_content)) {
                /* Collapsible space: counts only between inline content. */
                if (want_max && line > 0.0) {
                    double sw = 0.0;
                    double fs = cb->font_size > 0.0 ? cb->font_size : 16.0;
                    if (!layout_measure_text_styled(cb, " ", fs, &sw, NULL)) sw = fs * 0.25;
                    pending_space = sw;
                }
                continue;
            }
            double w = layout_intrinsic_contribution(ctx, c, want_max);
            if (layout_box_is_block_level(cb) && cb->float_side == 0) {
                if (line > content) content = line;
                line = 0.0;
                pending_space = 0.0;
                if (w > content) content = w;
            } else if (want_max) {
                line += pending_space + w;
                pending_space = 0.0;
            } else if (w > content) {
                content = w;
            }
        }
        if (line > content) content = line;
    }

    double total = content + hpb;
    if (box->min_width > 0.0 || box->max_width > 0.0) {
        if (box->box_sizing == CSS_BOX_SIZING_BORDER_BOX)
            total = layout_clamp_size(total, box->min_width, box->max_width);
        else
            total = layout_clamp_size(content, box->min_width, box->max_width) + hpb;
    }
    return total < 0.0 ? 0.0 : total;
}

static double layout_intrinsic_width(LayoutContext *ctx, int idx, bool want_max)
{
    double *cache = ctx->intrinsic_cache;
    if (cache) {
        double v = cache[2 * (size_t)idx + (want_max ? 1 : 0)];
        if (v >= 0.0) return v;
    }
    double v = layout_intrinsic_compute(ctx, idx, want_max);
    if (cache) cache[2 * (size_t)idx + (want_max ? 1 : 0)] = v;
    return v;
}

/* Shrink-to-fit border-box width (CSS 2.1 §10.3.5):
 * min(max(min-content, available), max-content). */
static double layout_shrink_to_fit(LayoutContext *ctx, int idx, double avail)
{
    double mx = layout_intrinsic_width(ctx, idx, true);
    if (mx <= avail) return mx;
    double mn = layout_intrinsic_width(ctx, idx, false);
    return mn > avail ? mn : avail;
}

/* Size an atomic inline (inline-block / inline-flex) before placing it on a
 * line: definite widths come from layout_resolve_used_sizes, auto widths
 * shrink to fit the line; auto height is resolved by its own layout. */
static void layout_size_atomic_inline(LayoutContext *ctx, int c, double avail)
{
    LayoutBox *child = layout_box(ctx, c);
    if (!(child->width_set || child->width_percent > 0.0 || child->width_keyword)) {
        double a = avail - child->margin_left - child->margin_right;
        if (a < 0.0) a = 0.0;
        child->width = layout_shrink_to_fit(ctx, c, a);
    }
    if (layout_height_auto(child)) {
        child->height = 0.0;
        /* Text-entry controls have no content boxes to size them: one line
         * (two for textarea) plus their padding and border. */
        HtmlNode *dn = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
        if (dn && dn->type == HTML_NODE_ELEMENT && dn->tag_name &&
            (strcasecmp(dn->tag_name, "input") == 0 || strcasecmp(dn->tag_name, "textarea") == 0 ||
             strcasecmp(dn->tag_name, "select") == 0)) {
            const char *ty = layout_node_attribute(dn, "type");
            if (!(ty && strcasecmp(ty, "hidden") == 0)) {
                double lines = strcasecmp(dn->tag_name, "textarea") == 0 ? 2.0 : 1.0;
                double fs = child->font_size > 0.0 ? child->font_size : 16.0;
                double lh = layout_line_advance_box(child);
                (void)fs;
                child->height = lines * lh + layout_vertical_border_padding(child);
            }
        }
    }
    layout_update_content_sizes(child);
}

/* First float of the block formatting context being laid out on this thread
 * (floats only affect line boxes in their own BFC). */
static __thread int t_float_base = 0;

static bool layout_establishes_bfc(LayoutContext *ctx, int idx)
{
    LayoutBox *b = layout_box(ctx, idx);
    if (idx == ctx->tree.root_idx || b->float_side != 0 || b->overflow != 0 ||
        b->position == CSS_POSITION_ABSOLUTE || b->position == CSS_POSITION_FIXED ||
        layout_box_is_atomic_inline(b))
        return true;
    int p = ctx->tree.nodes[idx].parent_idx;
    if (p >= 0) {
        CssDisplay pd = layout_box(ctx, p)->display;
        if (pd == CSS_DISPLAY_FLEX || pd == CSS_DISPLAY_GRID) return true;
    }
    HtmlNode *dn = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    if (dn && dn->type == HTML_NODE_ELEMENT && dn->tag_name &&
        (strcasecmp(dn->tag_name, "td") == 0 || strcasecmp(dn->tag_name, "th") == 0))
        return true;
    return false;
}

static void layout_block_flow_inner(LayoutContext *ctx, int idx);

static void layout_block_flow(LayoutContext *ctx, int idx)
{
    if (!layout_establishes_bfc(ctx, idx)) {
        layout_block_flow_inner(ctx, idx);
        return;
    }
    int saved_base = t_float_base, saved_count = ctx->float_count;
    t_float_base = ctx->float_count;
    layout_block_flow_inner(ctx, idx);
    ctx->float_count = saved_count;
    t_float_base = saved_base;
}

static void layout_block_flow_inner(LayoutContext *ctx, int idx)
{
    LayoutBox *box = layout_box(ctx, idx);
    double content_left = box->x + box->padding_left + box->border_left;
    double content_top  = box->y + box->padding_top + box->border_top;
    double avail_width  = box->content_width;

    int nkids = 0;
    int *kids = layout_collect_flow_children(ctx, idx, &nkids);

    FlowCursor cur = {0};
    cur.x = content_left;
    cur.y = content_top;
    cur.line_top = content_top;
    cur.line_box = 0.0;

    /* Margin collapsing: cur.y is the anchor (bottom of the last non-empty
     * in-flow content, margins excluded); pending_mb is the collapsed bottom
     * margin waiting to be resolved against the next block child's top
     * margin.  any_placed becomes true once any line box or non-empty block
     * has been placed (used for the first-child top-margin collapse rule). */
    double pending_mb = 0.0;
    bool any_placed = false;

    /* text-align: indices of the inline runs on the current line; the line is
     * shifted when it finishes (wrap, block child, or end of container). */
    int *line_members = (nkids > 0) ? (int*)malloc((size_t)nkids * sizeof(int)) : NULL;
    int line_member_count = 0;

    /* Floats placed by this container's flow; inline lines shrink around them. */
    FloatRec floats[32];
    int nfloats = 0;

    for (int i = 0; i < nkids; i++) {
        int c = kids[i];
        LayoutBox *child = layout_box(ctx, c);

        /* Resolve used sizes against this container's content box. */
        layout_resolve_used_sizes(child, layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx),
                                  avail_width, box->content_height);

        if (child->float_side != 0 && (layout_is_block_flow(child->display) ||
                                       child->display == CSS_DISPLAY_INLINE_BLOCK)) {
            /* Floated block: finish the pending inline line, then place at the
             * current cursor on its side; the vertical cursor does not
             * advance (following content flows around it). */
            if (line_member_count > 0) {
                layout_align_line(ctx, line_members, line_member_count,
                                  content_left, avail_width, box->text_align);
                line_member_count = 0;
            }
            cur.y += cur.line_box;
            cur.line_box = 0.0;
            cur.x = content_left;

            /* Lay out the float's subtree at the full available width; the
             * used (shrink-to-fit) width comes from the widest in-flow child
             * extent (e.g. a fixed-width thumb frame inside). */
            child->x = content_left + child->margin_left;
            child->y = cur.y + child->margin_top;
            layout_update_content_sizes(child);
            layout_node_serial(ctx, c);

            double used = 0.0;
            for (int k = ctx->tree.nodes[c].first_child_idx; k >= 0;
                 k = ctx->tree.nodes[k].next_sibling_idx) {
                LayoutBox *kc = layout_box(ctx, k);
                if (!layout_is_in_flow(kc)) continue;
                double ext = kc->x + kc->width + kc->margin_right
                           - (child->x + child->padding_left + child->border_left);
                if (ext > used) used = ext;
            }
            if (used > 0.0 && used < child->width) {
                child->width = used + child->padding_left + child->padding_right
                              + child->border_left + child->border_right;
                layout_update_content_sizes(child);
            }

            double content_right = content_left + avail_width;
            double new_x = (child->float_side == 1)
                ? content_left + child->margin_left
                : content_right - child->margin_right - child->width;
            double dx = new_x - child->x;
            if (dx != 0.0) {
                child->x = new_x;
                layout_offset_subtree(ctx, c, dx, 0.0);
            }

            if (nfloats < (int)(sizeof(floats) / sizeof(floats[0]))) {
                FloatRec rec;
                rec.left = child->x - child->margin_left;
                rec.right = child->x + child->width + child->margin_right;
                rec.bottom = child->y + child->height + child->margin_bottom;
                rec.side = child->float_side;
                floats[nfloats++] = rec;
                /* Also publish on the shared stack so descendant block flows
                 * (siblings' inline content) shrink around this float. */
                layout_float_push(ctx, &rec);
                if (g_layout_debug & LDBG_FLOAT) {
                    HtmlNode *dn = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
                    fprintf(stderr, "[FLOAT] idx=%d side=%d left=%.0f right=%.0f bottom=%.0f (tag=%s class=%.40s)\n",
                            c, rec.side, rec.left, rec.right, rec.bottom,
                            dn ? dn->tag_name : "?", dn ? layout_node_attribute(dn, "class") : "");
                }
            }
            continue;
        }

        if (layout_box_is_block_level(child)) {
            /* Block child: finish the pending inline line, then stack. */
            if (line_member_count > 0) {
                layout_align_line(ctx, line_members, line_member_count,
                                  content_left, avail_width, box->text_align);
                line_member_count = 0;
            }
            if (cur.line_box > 0.0) {
                cur.y += cur.line_box;      /* drop any pending inline-line height */
                cur.line_box = 0.0;
                any_placed = true;
                pending_mb = 0.0;           /* line boxes block margin collapse */
            }
            cur.x = content_left;

            /* clear: advance past the matching floats before stacking. */
            bool cleared = false;
            if (child->clear) {
                for (int fi = t_float_base; fi < ctx->float_count; fi++) {
                    bool match = (child->clear == 3) ||
                                 (child->clear == 1 && ctx->float_stack[fi].side == 1) ||
                                 (child->clear == 2 && ctx->float_stack[fi].side == 2);
                    if (match && ctx->float_stack[fi].bottom > cur.y) {
                        cur.y = ctx->float_stack[fi].bottom;
                        cleared = true;
                    }
                }
            }

            /* Collapsed top gap: sibling margins resolve to max(); the first
             * in-flow child's top margin collapses out through the parent
             * when the parent has no top border/padding and the parent does
             * not establish a new block formatting context (§8.3.1). */
            double eff_mt = layout_eff_margin_top(ctx, c, 0);
            double gap;
            if (cleared) {
                gap = eff_mt;
                pending_mb = 0.0;
            } else if (!any_placed && box->overflow == 0 &&
                       box->padding_top <= 0.0 && box->border_top <= 0.0 &&
                       idx != ctx->tree.root_idx &&
                       box->display != CSS_DISPLAY_FLEX && box->display != CSS_DISPLAY_GRID &&
                       box->position != CSS_POSITION_ABSOLUTE && box->position != CSS_POSITION_FIXED) {
                /* Collapses out through this box (its parent applied it via
                 * layout_eff_margin_top).  The root element, and boxes that
                 * establish a new formatting context (absolutely positioned
                 * boxes), keep their children's margins inside (§8.3.1). */
                gap = 0.0;
            } else {
                gap = pending_mb > eff_mt ? pending_mb : eff_mt;
            }

            child->x = content_left + child->margin_left;
            if ((child->margin_auto & 0xA) && (child->width_set || child->width_percent > 0.0 || child->width_keyword ||
                                               child->max_width > 0.0)) {
                /* Auto horizontal margins absorb the free space (§10.3.3). */
                double ml = (child->margin_auto & 8) ? 0.0 : child->margin_left;
                double mr = (child->margin_auto & 2) ? 0.0 : child->margin_right;
                double free_w = avail_width - child->width - ml - mr;
                if (free_w > 0.0) {
                    if ((child->margin_auto & 0xA) == 0xA) child->x = content_left + free_w / 2.0;
                    else if (child->margin_auto & 8) child->x = content_left + free_w;
                }
            }
            child->y = cur.y + gap;
            layout_update_content_sizes(child);

            /* Recurse so the child's subtree (and thus its auto height) is final
             * before we advance the cursor past it. */
            layout_node_serial(ctx, c);

            double eff_mb = layout_eff_margin_bottom(ctx, c, 0);
            if (child->height <= 0.0 && layout_height_auto(child) && child->overflow == 0 &&
                child->padding_top <= 0.0 && child->padding_bottom <= 0.0 &&
                child->border_top <= 0.0 && child->border_bottom <= 0.0) {
                /* Empty block: top and bottom margins collapse through into a
                 * single margin; the anchor does not move.  (A box that
                 * establishes a new BFC, e.g. overflow:hidden, never
                 * collapses through, §8.3.1.) */
                double m = eff_mt > eff_mb ? eff_mt : eff_mb;
                pending_mb = pending_mb > m ? pending_mb : m;
            } else {
                cur.y = child->y + child->height;
                pending_mb = eff_mb;
                any_placed = true;
            }
        } else {
            /* Inline-level child: measure with real glyph metrics so runs
             * share lines like a browser; long text runs wrap across lines.
             * Line geometry accounts for floats intruding at this height. */
            double line_left = content_left, line_avail = avail_width;
            layout_flow_line_geometry(ctx->float_stack + t_float_base, ctx->float_count - t_float_base, cur.y,
                                      content_left, avail_width,
                                      &line_left, &line_avail);
            /* The pen must never start left of the float-inset line edge:
             * without this clamp the first line of a container (and any run
             * right after a block boundary) is emitted at content_left and
             * overlaps a left float. */
            if (cur.x < line_left) cur.x = line_left;
            HtmlNode *dom = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
            if (pending_mb > 0.0 &&
                !(dom && dom->type == HTML_NODE_TEXT && layout_text_is_whitespace(dom->text_content))) {
                /* A line box after a block: the block's bottom margin no
                 * longer collapses with anything. */
                cur.y += pending_mb;
                pending_mb = 0.0;
                any_placed = true;
            }
            if (layout_box_is_atomic_inline(child) && dom && dom->type == HTML_NODE_ELEMENT &&
                strcasecmp(dom->tag_name, "img") != 0) {
                /* Atomic inline (inline-block / inline-flex): shrink-to-fit
                 * width, laid out as its own formatting context, then placed
                 * on the line as one unbreakable box. */
                layout_size_atomic_inline(ctx, c, line_avail);
                double aspan = child->margin_left + child->width + child->margin_right;
                if (cur.x + aspan > line_left + line_avail + 0.01 && cur.x > line_left + 0.001) {
                    if (line_member_count > 0) {
                        layout_align_line(ctx, line_members, line_member_count,
                                          line_left, line_avail, box->text_align);
                        line_member_count = 0;
                    }
                    cur.y += cur.line_box;
                    cur.line_box = 0.0;
                    cur.x = line_left;
                }
                child->x = cur.x + child->margin_left;
                child->y = cur.y + child->margin_top;
                layout_update_content_sizes(child);
                layout_node_serial(ctx, c);
                cur.x += aspan;
                double ah = child->margin_top + child->height + child->margin_bottom;
                if (ah > cur.line_box) cur.line_box = ah;
                if (line_members) line_members[line_member_count++] = c;
                continue;
            }
            double fs = child->font_size > 0.0 ? child->font_size : 16.0;
            float scale = (float)(fs / 16.0);
            double line_adv = layout_line_advance_box(child);
            const char *text = NULL;
            char concat[8192];
            bool is_text_run = false;
            bool is_whitespace_run = false;
            bool is_img = false;
            bool is_br = false;
            child->text_lead_skip = 0;
            if (dom && dom->type == HTML_NODE_TEXT) {
                if (!layout_text_is_whitespace(dom->text_content)) {
                    text = css_layout_box_text(ctx, c, dom->text_content);
                    if (text[0] == ' ' && cur.x <= line_left + 0.001 && child->white_space != 2 &&
                        child->white_space != 3) {
                        /* A collapsible space at the start of a line is removed. */
                        text++;
                        child->text_lead_skip = 1;
                    }
                    is_text_run = true;
                } else {
                    is_whitespace_run = true;
                }
            } else if (dom && dom->type == HTML_NODE_ELEMENT) {
                if (strcasecmp(dom->tag_name, "img") == 0) {
                    is_img = true;
                } else if (strcasecmp(dom->tag_name, "br") == 0) {
                    is_br = true;
                } else {
                    size_t cl = 0;
                    concat[0] = '\0';
                    layout_concat_dom_text(ctx->doc, dom, concat, sizeof(concat), &cl);
                    if (cl > 0 && !layout_text_is_whitespace(concat)) {
                        text = concat;
                    } else if (cl > 0 && child->display == CSS_DISPLAY_INLINE &&
                               layout_horizontal_border_padding(child) <= 0.0) {
                        /* <span> </span> between inline content (YouTube's
                         * hashtag separators) is a collapsible space. */
                        HtmlNode *fc = html_node_first_child(ctx->doc, dom);
                        if (fc && fc->type == HTML_NODE_TEXT && !html_node_next_sibling(ctx->doc, fc))
                            is_whitespace_run = true;
                    }
                }
            }

            double tw = 0.0;
            if (is_whitespace_run) {
                /* Whitespace-only run: between inline content it collapses
                 * to a single space; at block boundaries (line start) it
                 * disappears entirely.  Handle it fully here so the generic
                 * fallbacks cannot resurrect it as an 80x24 box. */
                if (cur.x > line_left + 0.001) {
                    double sw = 0.0;
                    if (!layout_measure_text_styled(child, " ", fs, &sw, NULL)) sw = fs * 0.3;
                    child->width = sw;
                    child->height = line_adv;
                } else {
                    child->width = 0.0;
                    child->height = 0.0;
                }
            } else if (is_img) {
                /* Replaced element: layout_resolve_used_sizes already sized
                 * it from CSS, attributes and the natural size / ratio. */
            } else if (text && layout_measure_text_styled(child, text, fs, &tw, NULL)) {
                child->width = tw;
                child->height = line_adv;
                if (!is_text_run && child->display == CSS_DISPLAY_INLINE) {
                    /* Inline element: its max-content width also counts
                     * atomic inline descendants and its own padding. */
                    double iw = layout_intrinsic_width(ctx, c, true);
                    if (iw > tw + 0.5) {
                        double cap = line_avail - child->margin_left - child->margin_right;
                        if (iw <= cap || layout_horizontal_border_padding(child) > 0.0) {
                            child->width = iw;
                            text = NULL;   /* not a plain text run: no wrapping */
                        }
                    }
                }
            } else if (!is_text_run || text == NULL) {
                /* Element with no text content (e.g. <a><img></a>): size it
                 * from a replaced <img> descendant when present, so captions
                 * and following content don't overlap the image. */
                double iw = 0.0, ih = 0.0;
                bool from_img = dom && layout_first_img_size(ctx->doc, dom, &iw, &ih);
                if (from_img) {
                    child->width = iw;
                    child->height = ih;
                } else if (dom && dom->type == HTML_NODE_ELEMENT && dom->tag_name &&
                           strcasecmp(dom->tag_name, "svg") == 0) {
                    layout_svg_size(ctx, c, dom, &child->width, &child->height);
                } else if (dom && layout_is_replaced_form_element(dom->tag_name)) {
                    if (child->width <= 0.0) child->width = 80.0;
                    if (child->height <= 0.0) child->height = 20.0;
                } else {
                    /* Inline element without text: size from atomic inline
                     * descendants (icons in links); otherwise empty (anchor
                     * span, empty <a>, ...) shrinks to zero. */
                    double iw = (child->display == CSS_DISPLAY_INLINE)
                        ? layout_intrinsic_width(ctx, c, true) : 0.0;
                    if (iw > 0.0) {
                        child->width = iw;
                        child->height = line_adv;
                    } else {
                        if (!child->width_set) child->width = 0.0;
                        if (layout_height_auto(child)) child->height = 0.0;
                    }
                }
            }
            layout_update_content_sizes(child);

            /* Vertical margins on non-replaced inline boxes have no effect
             * (CSS 2.1 §8.4): they neither shift the box on its line nor
             * inflate the line box.  Replaced elements (img, form controls)
             * and inline-blocks keep their vertical margins. */
            bool nonreplaced_inline =
                child->display == CSS_DISPLAY_INLINE && !is_img &&
                !(dom && dom->type == HTML_NODE_ELEMENT &&
                  layout_is_replaced_form_element(dom->tag_name));
            double vmt = nonreplaced_inline ? 0.0 : child->margin_top;
            double vmb = nonreplaced_inline ? 0.0 : child->margin_bottom;

            if (is_br) {
                /* <br>: forced line break — finish the current line (its box
                 * height defaults to one line when empty) and start a new one. */
                if (line_member_count > 0) {
                    layout_align_line(ctx, line_members, line_member_count,
                                      line_left, line_avail, box->text_align);
                    line_member_count = 0;
                }
                double adv = cur.line_box > 0.0 ? cur.line_box : line_adv;
                child->x = cur.x;
                child->y = cur.y;
                child->width = 0.0;
                child->height = line_adv;
                layout_update_content_sizes(child);
                cur.y += adv;
                cur.line_box = 0.0;
                cur.x = line_left;
                layout_node_serial(ctx, c);
                continue;
            }

            double span = child->margin_left + child->width + child->margin_right;
            double line_right = line_left + line_avail;
            double remaining = line_right - cur.x;
            /* A trailing collapsible space hangs past the line end. */
            double trail = 0.0;
            if (text && is_text_run && text[0] && text[strlen(text) - 1] == ' ') {
                if (!layout_measure_text_styled(child, " ", fs, &trail, NULL)) trail = 0.0;
            }
            bool nowrap = child->white_space == 1 || child->white_space == 2;
            if (g_layout_debug & LDBG_INLINE) {
                HtmlNode *dn = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
                fprintf(stderr, "[INL] idx=%d span=%.1f cur.x=%.1f line=[%.1f..%.1f] y=%.1f tag=%s type=%d\n",
                        c, span, cur.x, line_left, line_right, cur.y,
                        dn ? dn->tag_name : "?", dn ? (int)dn->type : -1);
            }

            /* Text run that doesn't fit on the current line: wrap it across
             * lines, starting on the current one when it has useful room.
             * Inline elements whose content is (concatenated) text — links,
             * <b>, <span> — wrap the same way; the wrap geometry is later
             * propagated to their text descendants for glyph emission. */
            if (text && !nowrap && span - trail > remaining + 0.01 && span > fs * 2.0) {
                double first_w = remaining;
                if (cur.x <= line_left + 0.001) {
                    /* Already at the line start: use the full line. */
                    cur.line_box = 0.0;
                    cur.x = line_left;
                    first_w = line_avail;
                } else {
                    /* Split the run at a word boundary only when its first
                     * word (with any leading spaces) fits in the remaining
                     * sliver; otherwise push the whole run to the next
                     * line.  Browsers split whenever a word fits.  A first
                     * word that is itself too wide may still start the line
                     * when a hyphen prefix of it fits (UAX #14 break-after-
                     * hyphen), because the shaper will split it there. */
                    const char *we = text;
                    while (*we == ' ' || *we == '\t' || *we == '\n') we++;
                    while (*we && *we != ' ' && *we != '\t' && *we != '\n') we++;
                    char first_word[256];
                    size_t fwlen = (size_t)(we - text);
                    if (fwlen >= sizeof(first_word)) fwlen = sizeof(first_word) - 1;
                    memcpy(first_word, text, fwlen);
                    first_word[fwlen] = '\0';
                    double fww = 0.0, fwh = 0.0;
                    bool fw_ok = layout_measure_text_styled(child, first_word,
                                                            fs, &fww, &fwh);
                    if (fw_ok && fww > first_w + 0.001) {
                        /* Try hyphen prefixes (longest first that fits). */
                        char *last_hyphen = NULL;
                        for (char *hp = first_word; *hp; hp++) {
                            if (*hp == '-' && hp[1] &&
                                ((hp[1] >= 'a' && hp[1] <= 'z') ||
                                 (hp[1] >= 'A' && hp[1] <= 'Z') ||
                                 (unsigned char)hp[1] >= 0x80))
                                last_hyphen = hp;
                        }
                        while (last_hyphen) {
                            char saved = last_hyphen[1];
                            last_hyphen[1] = '\0';
                            double pw = 0.0;
                            bool ok = layout_measure_text_styled(child, first_word,
                                                                 fs, &pw, NULL);
                            if (ok && pw <= first_w + 0.001) { fw_ok = true; fww = pw; break; }
                            last_hyphen[1] = saved;
                            /* find an earlier hyphen */
                            char *prev = NULL;
                            for (char *hp = first_word; hp < last_hyphen; hp++) {
                                if (*hp == '-' && hp[1] &&
                                    ((hp[1] >= 'a' && hp[1] <= 'z') ||
                                     (hp[1] >= 'A' && hp[1] <= 'Z') ||
                                     (unsigned char)hp[1] >= 0x80))
                                    prev = hp;
                            }
                            last_hyphen = prev;
                        }
                    }
                    if (!fw_ok || fww > first_w + 0.001) {
                        /* First word doesn't fit: finish the current line
                         * first (alignment applies). */
                        if (line_member_count > 0) {
                            layout_align_line(ctx, line_members, line_member_count,
                                              line_left, line_avail, box->text_align);
                            line_member_count = 0;
                        }
                        cur.y += cur.line_box;
                        cur.line_box = 0.0;
                        cur.x = line_left;
                        first_w = line_avail;
                    }
                }
                if (span <= first_w) {
                    /* Fits on the (possibly new) current line unwrapped. */
                    child->x = cur.x + child->margin_left;
                    child->y = cur.y + vmt;
                    cur.x += span;
                    double h = vmt + child->height + vmb;
                    if (h > cur.line_box) cur.line_box = h;
                } else {
                    TextShaper *font = display_list_get_font(layout_font_slot(child));
                    if (!font) font = display_list_get_default_font();
                    /* When a float stops intruding partway through this
                     * wrapped run, continuation lines below it change
                     * geometry (typically widening to full width).  Find the
                     * nearest float bottom below the run start and the line
                     * geometry just under it; the shaper switches to it from
                     * the first line whose top clears that bottom. */
                    int cont2_line = 0;
                    double cont2_x = 0.0, cont2_w = 0.0;
                    double next_fb = 1e30;
                    for (int fi = t_float_base; fi < ctx->float_count; fi++) {
                        double fb = ctx->float_stack[fi].bottom;
                        if (fb > cur.y + 0.001 && fb < next_fb) next_fb = fb;
                    }
                    if (next_fb < 1e29) {
                        double g2l = content_left, g2a = avail_width;
                        layout_flow_line_geometry(ctx->float_stack + t_float_base,
                                                  ctx->float_count - t_float_base,
                                                  next_fb + 0.01,
                                                  content_left, avail_width,
                                                  &g2l, &g2a);
                        if (g2l != line_left || g2a != line_avail) {
                            int k = (int)ceil((next_fb - 0.001 - cur.y)
                                              / line_adv) + 1;
                            if (k < 2) k = 2;
                            cont2_line = k;
                            cont2_x = g2l;
                            cont2_w = g2a;
                        }
                    }
                    TsWrapResult wr;
                    if (font && text_shaper_wrap_measure(font, text,
                            (float)(cur.x + child->margin_left), (float)line_left,
                            (float)first_w, (float)line_avail, scale, (float)line_adv,
                            cont2_line, (float)cont2_x, (float)cont2_w, &wr)) {
                        child->x = cur.x + child->margin_left;
                        child->y = cur.y + vmt;
                        child->width = wr.max_width;
                        child->height = wr.height;
                        child->wrap_first_w = first_w;
                        child->wrap_cont_w = line_avail;
                        child->wrap_cont_x = line_left;
                        child->wrap_cont2_x = cont2_x;
                        child->wrap_cont2_w = cont2_w;
                        child->wrap_cont2_line = cont2_line;
                        layout_update_content_sizes(child);
                        /* Continue after the wrapped text: cursor sits at the
                         * end of its last line so following runs share it. */
                        cur.y += vmt + wr.height - wr.line_advance;
                        cur.x = wr.last_end_x + child->margin_right;
                        cur.line_box = wr.line_advance + vmb;
                    } else {
                        child->x = cur.x + child->margin_left;
                        child->y = cur.y + vmt;
                        cur.x += span;
                    }
                }
                if (line_members) line_members[line_member_count++] = c;
                layout_node_serial(ctx, c);
                continue;
            }

            if (is_whitespace_run && cur.x + span > line_right) {
                /* Collapsible white space at the end of a line hangs: it
                 * neither wraps nor opens a new line box. */
                child->width = 0.0;
                child->height = 0.0;
                layout_update_content_sizes(child);
                span = 0.0;
            }
            if (cur.x + span - trail > line_right + 0.01 && cur.x > line_left + 0.001) {
                /* Finish the current line (alignment applies), then wrap. */
                if (line_member_count > 0) {
                    layout_align_line(ctx, line_members, line_member_count,
                                      line_left, line_avail, box->text_align);
                    line_member_count = 0;
                }
                cur.y += cur.line_box;
                cur.line_box = 0.0;
                cur.x = line_left;
            }
            child->x = cur.x + child->margin_left;
            child->y = cur.y + vmt;
            cur.x += span;
            if (line_members) line_members[line_member_count++] = c;
            layout_node_serial(ctx, c);
            if (child->display == CSS_DISPLAY_INLINE && dom && dom->type == HTML_NODE_ELEMENT) {
                /* Atomic inline descendants can be taller than one line. */
                double bottom = child->y + child->height;
                for (int k = ctx->tree.nodes[c].first_child_idx; k >= 0;
                     k = ctx->tree.nodes[k].next_sibling_idx) {
                    LayoutBox *kb = layout_box(ctx, k);
                    if (!layout_is_in_flow(kb)) continue;
                    double kbot = kb->y + kb->height +
                                  (layout_box_is_atomic_inline(kb) ? kb->margin_bottom : 0.0);
                    if (kbot > bottom) bottom = kbot;
                }
                bottom += child->padding_bottom + child->border_bottom;
                /* The inline box keeps its own (font) height; the line box
                 * grows around taller atomic content. */
                if (bottom - cur.y > cur.line_box) cur.line_box = bottom - cur.y;
            }
            double h = vmt + child->height + vmb;
            if (h > cur.line_box) cur.line_box = h;
            if (child->display == CSS_DISPLAY_INLINE && dom && dom->type == HTML_NODE_ELEMENT &&
                !is_img && !is_br && child->wrap_cont_w <= 0.0 &&
                fabs(child->height - line_adv) < 0.01) {
                /* Visual box of a one-line inline element: its content area
                 * plus padding/border, centered by the half-leading. */
                double ca = layout_content_area_height(child);
                double vpb = layout_vertical_border_padding(child);
                child->y += (line_adv - ca) / 2.0 - child->padding_top - child->border_top;
                child->height = ca + vpb;
                layout_update_content_sizes(child);
            }
        }
    }

    /* Finish the trailing inline line (alignment applies). */
    if (line_member_count > 0) {
        double line_left = content_left, line_avail = avail_width;
        layout_flow_line_geometry(ctx->float_stack + t_float_base, ctx->float_count - t_float_base, cur.y,
                                  content_left, avail_width,
                                  &line_left, &line_avail);
        layout_align_line(ctx, line_members, line_member_count,
                          line_left, line_avail, box->text_align);
        line_member_count = 0;
    }
    free(line_members);

    cur.y += cur.line_box;  /* finish trailing inline line */

    free(kids);

    /* Resolve auto height from the extent of the children (not when height
     * was explicitly assigned, even to zero).  cur.y is the anchor: the
     * bottom of the last non-empty in-flow content.  The last child's
     * collapsed bottom margin extends the box only when it cannot collapse
     * out through the parent (i.e. the parent has bottom border/padding). */
    if (box->height <= 0.0 && layout_height_auto(box)) {
        double max_bottom = cur.y;
        {
            HtmlNode *imn = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
            if (layout_is_img(imn)) {
                double w2 = 0.0, ch = 0.0;
                layout_img_size(ctx, imn, box->content_width, false, 0.0, true, &w2, &ch);
                if (content_top + ch > max_bottom) max_bottom = content_top + ch;
            }
        }
        if (box->aspect_ratio > 0.0) {
            /* ::before { padding-top: N% } aspect boxes: the pseudo block
             * occupies width x ratio at the top of the content box.  The
             * aspect-ratio property with border-box sizes the border box. */
            double ch = box->content_width * box->aspect_ratio;
            if (box->aspect_ratio_prop && box->box_sizing == CSS_BOX_SIZING_BORDER_BOX)
                ch = box->width * box->aspect_ratio - layout_vertical_border_padding(box);
            double b = content_top + ch;
            if (b > max_bottom) max_bottom = b;
        }
        {
            /* Text-entry controls: one line of text (two for textarea). */
            HtmlNode *bn = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
            if (bn && bn->type == HTML_NODE_ELEMENT && bn->tag_name &&
                (strcasecmp(bn->tag_name, "input") == 0 || strcasecmp(bn->tag_name, "textarea") == 0 ||
                 strcasecmp(bn->tag_name, "select") == 0)) {
                const char *ty = layout_node_attribute(bn, "type");
                if (!(ty && strcasecmp(ty, "hidden") == 0)) {
                    double lines = strcasecmp(bn->tag_name, "textarea") == 0 ? 2.0 : 1.0;
                    double b = content_top + lines * layout_line_advance_box(box);
                    if (b > max_bottom) max_bottom = b;
                }
            }
        }
        if (box->padding_bottom > 0.0 || box->border_bottom > 0.0)
            max_bottom += pending_mb;
        /* Children that overflowed their line (e.g. tall replaced inline
         * content) can extend below the anchor; take them into account. */
        for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
             c = ctx->tree.nodes[c].next_sibling_idx) {
            LayoutBox *child = layout_box(ctx, c);
            if (!layout_is_in_flow(child)) continue;
            double bottom = child->y + child->height;
            if (bottom > max_bottom) max_bottom = bottom;
        }
        /* Floats are out of flow but the container still grows around them
         * (clearfix-free pages like Wikipedia rely on this visually). */
        for (int fi = 0; fi < nfloats; fi++) {
            if (floats[fi].bottom > max_bottom) max_bottom = floats[fi].bottom;
        }
        if (box->line_clamp && box->webkit_box) {
            double cap = content_top + box->line_clamp * layout_line_advance_box(box);
            if (max_bottom > cap) max_bottom = cap;
        }
        double total = max_bottom - box->y + box->padding_bottom + box->border_bottom;
        if (total < 0.0) total = 0.0;
        /* min/max-height clamps (content-box aware). */
        if (box->min_height > 0.0 || box->max_height > 0.0) {
            if (box->box_sizing == CSS_BOX_SIZING_BORDER_BOX)
                total = layout_clamp_size(total, box->min_height, box->max_height);
            else {
                double ch = layout_total_to_content_height(box, total);
                ch = layout_clamp_size(ch, box->min_height, box->max_height);
                total = layout_content_to_total_height(box, ch);
            }
        }
        box->height = total;
        layout_update_content_sizes(box);
    } else {
        /* <button> centers its content vertically in a taller box. */
        HtmlNode *bn = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
        if (bn && bn->type == HTML_NODE_ELEMENT && bn->tag_name &&
            strcasecmp(bn->tag_name, "button") == 0) {
            double used = cur.y - content_top;
            double room = box->content_height - used;
            if (used > 0.0 && room > 0.5) {
                for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
                     c = ctx->tree.nodes[c].next_sibling_idx) {
                    LayoutBox *cb = layout_box(ctx, c);
                    if (!layout_is_in_flow(cb)) continue;
                    cb->y += room / 2.0;
                    layout_offset_subtree(ctx, c, 0.0, room / 2.0);
                }
            }
        }
    }
}

/* ---- Units: subtrees of already-placed boxes ---- */

typedef struct LayoutUnitsJob {
    LayoutContext *ctx;
    const int *idx;
} LayoutUnitsJob;

static void layout_unit_job(Job *j)
{
    LayoutUnitsJob *u = (LayoutUnitsJob *)j->ctx;
    for (uint32_t i = j->a; i < j->b; i++) layout_node_serial(u->ctx, u->idx[i]);
}

/* Lay out the subtrees of `n` boxes that their container has already placed
 * and sized.  Equivalent to calling layout_node_serial on each in order: the
 * subtrees touch only their own boxes, and with no float in any of them the
 * shared float stack is only read. */
static void layout_units(LayoutContext *ctx, const int *idx, int n)
{
    bool par = g_lpar.enabled && n >= 2;
    int total = 0;
    if (par) {
        for (int i = 0; i < n; i++) {
            if (g_lpar.has_float[idx[i]]) { par = false; break; }
            total += g_lpar.subtree[idx[i]];
        }
        if (par && total < g_lpar.min_nodes) par = false;
        if (!par && total >= g_lpar.min_nodes) atomic_fetch_add(&g_lpar.serial_regions, 1);
    }
    if (!par) {
        for (int i = 0; i < n; i++) layout_node_serial(ctx, idx[i]);
        return;
    }
    atomic_fetch_add(&g_lpar.regions, 1);
    atomic_fetch_add(&g_lpar.units, (uint32_t)n);
    LayoutUnitsJob u = { ctx, idx };
    Job *group = job_group_new();
    /* One job per item; tiny neighbours are batched up to the grain. */
    int i = 0;
    while (i < n) {
        int j = i, sz = 0;
        do { sz += g_lpar.subtree[idx[j]]; j++; } while (j < n && sz < g_lpar.min_nodes / 2);
        job_group_spawn(group, layout_unit_job, &u, (uint32_t)i, (uint32_t)j);
        i = j;
    }
    job_group_wait(group);
}

/* ---- Flex layout (CSS Flexbox §9) ----
 * 1. flex base sizes (flex-basis, definite size, or content size: max-content
 *    width for rows, laid-out height for columns) and automatic minimums;
 * 2. line breaking, then the resolve-flexible-lengths loop with freezing;
 * 3. main-axis placement (auto margins, justify-content);
 * 4. item layout at the final main size gives hypothetical cross sizes;
 * 5. line cross sizes, container cross size, align-self/align-items with
 *    stretch (re-layout only when a box's contents depend on its height). */

typedef struct {
    int idx;
    bool is_text;
    double grow, shrink;
    double base, hypo, target;
    double min_main, max_main;          /* border-box; max <= 0 = none */
    double min_cross, max_cross;
    double mm_s, mm_e, cm_s, cm_e;      /* margins (auto counts as 0) */
    unsigned char auto_ms, auto_me, auto_cs, auto_ce;
    bool frozen;
    bool cross_def;                     /* explicit cross size */
    bool measured;                      /* laid out at its current size */
    double text_w, text_h;
    double main_pos;                    /* border-box main start */
    double cross;                       /* border-box cross size */
    int align;
    int order, seq;
} FlexItem;

typedef struct {
    int start, count;
    double cross_size;
    double cross_pos;
} FlexLineSeg;

static int flex_item_order_cmp(const void *a, const void *b)
{
    const FlexItem *x = (const FlexItem *)a, *y = (const FlexItem *)b;
    if (x->order != y->order) return x->order < y->order ? -1 : 1;
    return x->seq - y->seq;
}

/* Does changing this box's used height after layout move its contents? */
static bool layout_height_sensitive(LayoutContext *ctx, int idx)
{
    LayoutBox *b = layout_box(ctx, idx);
    if (b->display == CSS_DISPLAY_FLEX || b->display == CSS_DISPLAY_GRID) return true;
    HtmlNode *dn = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    if (dn && dn->type == HTML_NODE_ELEMENT && dn->tag_name &&
        strcasecmp(dn->tag_name, "button") == 0) return true;   /* centers its content */
    for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
         c = ctx->tree.nodes[c].next_sibling_idx) {
        LayoutBox *cb = layout_box(ctx, c);
        if (cb->display == CSS_DISPLAY_NONE) continue;
        if (cb->height_percent > 0.0) return true;
    }
    return false;
}

/* Move an already laid out box (and its subtree) to a new border-box origin. */
static void layout_move_box(LayoutContext *ctx, int idx, double x, double y)
{
    LayoutBox *b = layout_box(ctx, idx);
    double dx = x - b->x, dy = y - b->y;
    b->x = x;
    b->y = y;
    layout_offset_subtree(ctx, idx, dx, dy);
}

static double flex_clamp(double v, double mn, double mx)
{
    if (mx > 0.0 && v > mx) v = mx;
    if (v < mn) v = mn;
    return v;
}

static void layout_flex_container(LayoutContext *ctx, int idx)
{
    LayoutBox *ct = layout_box(ctx, idx);
    bool is_row = (ct->flex_direction == CSS_FLEX_DIRECTION_ROW ||
                   ct->flex_direction == CSS_FLEX_DIRECTION_ROW_REVERSE);
    bool reverse = (ct->flex_direction == CSS_FLEX_DIRECTION_ROW_REVERSE ||
                    ct->flex_direction == CSS_FLEX_DIRECTION_COLUMN_REVERSE);
    bool do_wrap = ct->flex_wrap != CSS_FLEX_WRAP_NOWRAP;
    bool wrap_rev = ct->flex_wrap == CSS_FLEX_WRAP_WRAP_REVERSE;

    double content_x = ct->x + ct->padding_left + ct->border_left;
    double content_y = ct->y + ct->padding_top + ct->border_top;
    double cw = ct->content_width;
    bool height_def = !layout_height_auto(ct) || ct->height > 0.0;
    double chh = height_def ? ct->content_height : 0.0;
    bool main_def = is_row ? true : height_def;
    bool cross_def = is_row ? height_def : true;
    double avail_main = is_row ? cw : chh;
    double gap_main = is_row ? ct->gap_col : ct->gap_row;
    double gap_cross = is_row ? ct->gap_row : ct->gap_col;
    double main_start = is_row ? content_x : content_y;
    double cross_start = is_row ? content_y : content_x;

    /* Container min/max on each axis, as content-box sizes. */
    double cmin_h = ct->min_height, cmax_h = ct->max_height;
    if (ct->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
        double v = layout_vertical_border_padding(ct);
        cmin_h = cmin_h > 0.0 ? (cmin_h - v > 0.0 ? cmin_h - v : 0.0) : 0.0;
        cmax_h = cmax_h > 0.0 ? (cmax_h - v > 0.0 ? cmax_h - v : 0.0) : 0.0;
    }

    int nkids = 0;
    int *kids = layout_collect_flow_children(ctx, idx, &nkids);
    /* Whitespace-only text between items creates no flex item (§4). */
    if (nkids > 0) {
        int w = 0;
        for (int r = 0; r < nkids; r++) {
            HtmlNode *kn = layout_node_dom(ctx, ctx->tree.nodes[kids[r]].dom_node_idx);
            if (kn && kn->type == HTML_NODE_TEXT && layout_text_is_whitespace(kn->text_content))
                continue;
            kids[w++] = kids[r];
        }
        nkids = w;
    }
    FlexItem *items = nkids > 0 ? (FlexItem *)calloc((size_t)nkids, sizeof(FlexItem)) : NULL;
    if (nkids > 0 && !items) { free(kids); return; }

    /* ---- 1. base sizes ---- */
    int *measure = nkids > 0 ? (int *)malloc((size_t)nkids * sizeof(int)) : NULL;
    int nmeasure = 0;
    for (int i = 0; i < nkids; i++) {
        int c = kids[i];
        LayoutBox *child = layout_box(ctx, c);
        HtmlNode *dom = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
        FlexItem *it = &items[i];
        it->idx = c;
        it->seq = i;
        it->order = child->order;
        it->grow = child->flex_grow;
        it->shrink = child->flex_shrink;
        it->align = child->align_self ? child->align_self - 1 : (int)ct->align_items;

        if (dom && dom->type == HTML_NODE_TEXT) {
            it->is_text = true;
            it->text_w = layout_intrinsic_width(ctx, c, true);
            it->text_h = layout_line_advance_box(child);
            it->base = is_row ? it->text_w : it->text_h;
            it->min_main = is_row ? layout_intrinsic_width(ctx, c, false) : it->text_h;
            it->hypo = it->base;
            it->cross_def = true;
            it->cross = is_row ? it->text_h : it->text_w;
            continue;
        }

        layout_resolve_used_sizes(child, dom, cw, chh);
        bool w_def = child->width_set || child->width_percent > 0.0 || child->width_keyword;
        bool h_def = !layout_height_auto(child);
        if (!w_def) child->width = 0.0;
        if (!h_def) child->height = 0.0;
        layout_update_content_sizes(child);

        unsigned char ma = child->margin_auto;
        if (is_row) {
            it->auto_ms = ma & 8; it->auto_me = ma & 2; it->auto_cs = ma & 1; it->auto_ce = ma & 4;
            it->mm_s = it->auto_ms ? 0.0 : child->margin_left;
            it->mm_e = it->auto_me ? 0.0 : child->margin_right;
            it->cm_s = it->auto_cs ? 0.0 : child->margin_top;
            it->cm_e = it->auto_ce ? 0.0 : child->margin_bottom;
        } else {
            it->auto_ms = ma & 1; it->auto_me = ma & 4; it->auto_cs = ma & 8; it->auto_ce = ma & 2;
            it->mm_s = it->auto_ms ? 0.0 : child->margin_top;
            it->mm_e = it->auto_me ? 0.0 : child->margin_bottom;
            it->cm_s = it->auto_cs ? 0.0 : child->margin_left;
            it->cm_e = it->auto_ce ? 0.0 : child->margin_right;
        }
        it->cross_def = is_row ? h_def : w_def;
        bool main_def_item = is_row ? w_def : h_def;

        /* Cross-axis min/max (border-box). */
        if (is_row) {
            it->min_cross = child->min_height > 0.0 ? layout_flex_cross_total(child, child->min_height, true) : 0.0;
            it->max_cross = child->max_height > 0.0 ? layout_flex_cross_total(child, child->max_height, true) : 0.0;
        } else {
            it->min_cross = child->min_width > 0.0 ? layout_flex_cross_total(child, child->min_width, false) : 0.0;
            it->max_cross = child->max_width > 0.0 ? layout_flex_cross_total(child, child->max_width, false) : 0.0;
        }

        /* Column items: the cross size (width) is known before layout. */
        if (!is_row && !w_def) {
            double room = cw - it->cm_s - it->cm_e;
            if (room < 0.0) room = 0.0;
            if (it->align == CSS_ALIGN_STRETCH && !it->auto_cs && !it->auto_ce)
                child->width = flex_clamp(room, it->min_cross, it->max_cross);
            else
                child->width = flex_clamp(layout_shrink_to_fit(ctx, c, room), it->min_cross, it->max_cross);
            layout_update_content_sizes(child);
        }

        double basis = -1.0;
        if (child->flex_basis_percent > 0.0) {
            if (main_def) basis = layout_flex_main_total(child, avail_main * child->flex_basis_percent + child->flex_basis_pct_px, is_row);
        } else if (child->flex_basis >= 0.0) {
            basis = layout_flex_main_total(child, child->flex_basis, is_row);
        }
        if (basis < 0.0 && main_def_item) basis = is_row ? child->width : child->height;
        if (basis < 0.0) {
            if (is_row) {
                basis = layout_intrinsic_width(ctx, c, true);
            } else {
                /* Content height: lay the item out at its cross size. */
                child->x = content_x + it->cm_s;
                child->y = content_y + it->mm_s;
                child->height = 0.0;
                layout_update_content_sizes(child);
                measure[nmeasure++] = i;
                it->measured = true;
                basis = -2.0;   /* filled in after the measuring layout */
            }
        }
        it->base = basis;

        if (is_row) {
            it->min_main = child->min_width > 0.0 ? layout_flex_main_total(child, child->min_width, true) : -1.0;
            it->max_main = child->max_width > 0.0 ? layout_flex_main_total(child, child->max_width, true) : 0.0;
        } else {
            it->min_main = child->min_height > 0.0 ? layout_flex_main_total(child, child->min_height, false) : -1.0;
            it->max_main = child->max_height > 0.0 ? layout_flex_main_total(child, child->max_height, false) : 0.0;
        }
        if (it->min_main < 0.0 && (child->min_size_set & (is_row ? 1 : 2))) it->min_main = 0.0;
        if (it->min_main < 0.0) {
            /* Automatic minimum size (§4.5): the content size for items that
             * are not scroll containers, capped by a definite size. */
            it->min_main = 0.0;
            if (child->overflow == 0 && is_row) {
                double mc = layout_intrinsic_width(ctx, c, false);
                if (w_def && child->width < mc) mc = child->width;
                if (it->max_main > 0.0 && mc > it->max_main) mc = it->max_main;
                it->min_main = mc;
            }
        }
    }
    if (nmeasure > 0) {
        int *list = (int *)malloc((size_t)nmeasure * sizeof(int));
        for (int k = 0; k < nmeasure; k++) list[k] = items[measure[k]].idx;
        layout_units(ctx, list, nmeasure);
        free(list);
        for (int k = 0; k < nmeasure; k++) {
            FlexItem *it = &items[measure[k]];
            LayoutBox *child = layout_box(ctx, it->idx);
            it->base = child->height;
            if (child->overflow == 0 && !(child->min_size_set & 2) && child->min_height <= 0.0) {
                double mc = child->height;
                if (it->max_main > 0.0 && mc > it->max_main) mc = it->max_main;
                it->min_main = mc;
            }
        }
    }
    for (int i = 0; i < nkids; i++) {
        FlexItem *it = &items[i];
        if (!it->is_text) it->hypo = flex_clamp(it->base, it->min_main, it->max_main);
    }

    /* order: stable sort (measurement indices are no longer needed). */
    if (nkids > 1) qsort(items, (size_t)nkids, sizeof(FlexItem), flex_item_order_cmp);

    /* ---- 2. lines ---- */
    FlexLineSeg *lines = (FlexLineSeg *)calloc((size_t)nkids + 1, sizeof(FlexLineSeg));
    int nlines = 0;
    {
        int start = 0;
        double used = 0.0;
        for (int i = 0; i < nkids; i++) {
            double outer = items[i].hypo + items[i].mm_s + items[i].mm_e;
            double add = (i > start ? gap_main : 0.0) + outer;
            if (do_wrap && main_def && i > start && used + add > avail_main + 0.01) {
                lines[nlines].start = start;
                lines[nlines].count = i - start;
                nlines++;
                start = i;
                used = outer;
            } else {
                used += add;
            }
        }
        if (nkids > 0) {
            lines[nlines].start = start;
            lines[nlines].count = nkids - start;
            nlines++;
        }
    }

    /* Indefinite main size (column, auto height): the content size,
     * clamped by min/max-height so flexible items can fill a min-height. */
    if (!main_def) {
        double longest = 0.0;
        for (int li = 0; li < nlines; li++) {
            double sum = 0.0;
            for (int j = 0; j < lines[li].count; j++) {
                FlexItem *it = &items[lines[li].start + j];
                sum += it->hypo + it->mm_s + it->mm_e;
            }
            if (lines[li].count > 1) sum += (lines[li].count - 1) * gap_main;
            if (sum > longest) longest = sum;
        }
        avail_main = flex_clamp(longest, cmin_h, cmax_h);
    }

    /* ---- resolve flexible lengths (§9.7) ---- */
    for (int li = 0; li < nlines; li++) {
        FlexItem *L = &items[lines[li].start];
        int n = lines[li].count;
        double outer_hypo = 0.0, margins = 0.0;
        for (int j = 0; j < n; j++) {
            outer_hypo += L[j].hypo + L[j].mm_s + L[j].mm_e;
            margins += L[j].mm_s + L[j].mm_e;
        }
        double gaps = n > 1 ? (n - 1) * gap_main : 0.0;
        bool grow = avail_main - outer_hypo - gaps > 0.0;
        for (int j = 0; j < n; j++) {
            FlexItem *it = &L[j];
            double f = grow ? it->grow : it->shrink;
            it->frozen = f <= 0.0 || (grow && it->base > it->hypo) || (!grow && it->base < it->hypo);
            it->target = it->frozen ? it->hypo : it->base;
        }
        double initial_free = 0.0;
        {
            double used = margins + gaps;
            for (int j = 0; j < n; j++) used += L[j].frozen ? L[j].target : L[j].base;
            initial_free = avail_main - used;
        }
        for (int iter = 0; iter <= n; iter++) {
            double used = margins + gaps, sum_f = 0.0, sum_scaled = 0.0;
            int unfrozen = 0;
            for (int j = 0; j < n; j++) {
                FlexItem *it = &L[j];
                if (it->frozen) { used += it->target; continue; }
                used += it->base;
                unfrozen++;
                sum_f += grow ? it->grow : it->shrink;
                sum_scaled += it->shrink * it->base;
            }
            if (unfrozen == 0) break;
            double free_space = avail_main - used;
            if (sum_f < 1.0) {
                double v = initial_free * sum_f;
                if (fabs(v) < fabs(free_space)) free_space = v;
            }
            for (int j = 0; j < n; j++) {
                FlexItem *it = &L[j];
                if (it->frozen) continue;
                if (grow) {
                    it->target = it->base + (sum_f > 0.0 ? free_space * it->grow / sum_f : 0.0);
                } else if (sum_scaled > 0.0) {
                    it->target = it->base + free_space * (it->shrink * it->base) / sum_scaled;
                } else {
                    it->target = it->base;
                }
            }
            double violation = 0.0;
            for (int j = 0; j < n; j++) {
                FlexItem *it = &L[j];
                if (it->frozen) continue;
                double cl = flex_clamp(it->target, it->min_main, it->max_main);
                if (cl < 0.0) cl = 0.0;
                violation += cl - it->target;
            }
            for (int j = 0; j < n; j++) {
                FlexItem *it = &L[j];
                if (it->frozen) continue;
                double cl = flex_clamp(it->target, it->min_main, it->max_main);
                if (cl < 0.0) cl = 0.0;
                bool freeze = violation == 0.0 ||
                              (violation > 0.0 && cl > it->target) ||
                              (violation < 0.0 && cl < it->target);
                if (freeze) { it->target = cl; it->frozen = true; }
            }
        }
        for (int j = 0; j < n; j++) if (L[j].target < 0.0) L[j].target = 0.0;
    }

    /* ---- 3. main-axis placement ---- */
    for (int li = 0; li < nlines; li++) {
        FlexItem *L = &items[lines[li].start];
        int n = lines[li].count;
        double used = n > 1 ? (n - 1) * gap_main : 0.0;
        int nauto = 0;
        for (int j = 0; j < n; j++) {
            used += L[j].target + L[j].mm_s + L[j].mm_e;
            nauto += (L[j].auto_ms ? 1 : 0) + (L[j].auto_me ? 1 : 0);
        }
        double free_space = avail_main - used;
        double pos = 0.0, extra = 0.0, auto_m = 0.0;
        if (nauto > 0) {
            if (free_space > 0.0) auto_m = free_space / nauto;
        } else {
            switch (ct->justify_content) {
            case CSS_JUSTIFY_CENTER: pos = free_space / 2.0; break;
            case CSS_JUSTIFY_FLEX_END: pos = free_space; break;
            case CSS_JUSTIFY_SPACE_BETWEEN:
                if (free_space > 0.0 && n > 1) extra = free_space / (n - 1);
                break;
            case CSS_JUSTIFY_SPACE_AROUND:
                if (free_space > 0.0) { extra = free_space / n; pos = extra / 2.0; }
                else pos = free_space / 2.0;
                break;
            case CSS_JUSTIFY_SPACE_EVENLY:
                if (free_space > 0.0) { extra = free_space / (n + 1); pos = extra; }
                else pos = free_space / 2.0;
                break;
            default: break;
            }
        }
        for (int j = 0; j < n; j++) {
            FlexItem *it = &L[j];
            pos += it->mm_s + (it->auto_ms ? auto_m : 0.0);
            double p = pos;
            if (reverse) p = avail_main - pos - it->target;
            it->main_pos = main_start + p;
            pos += it->target + it->mm_e + (it->auto_me ? auto_m : 0.0) + gap_main + extra;
        }
    }

    /* ---- 4. item layout at the final main size ---- */
    int *todo = nkids > 0 ? (int *)malloc((size_t)nkids * sizeof(int)) : NULL;
    int ntodo = 0;
    for (int i = 0; i < nkids; i++) {
        FlexItem *it = &items[i];
        LayoutBox *child = layout_box(ctx, it->idx);
        if (it->is_text) {
            if (is_row) { child->width = it->target; child->height = it->text_h; }
            else { child->height = it->target; child->width = it->text_w < cw ? it->text_w : cw; }
            it->cross = is_row ? child->height : child->width;
            continue;
        }
        double x = is_row ? it->main_pos : cross_start + it->cm_s;
        double y = is_row ? cross_start + it->cm_s : it->main_pos;
        if (is_row) {
            child->width = it->target;
            layout_update_content_sizes(child);
            child->x = x; child->y = y;
            if (!it->cross_def) child->height = 0.0;
            layout_update_content_sizes(child);
            todo[ntodo++] = it->idx;
        } else {
            if (it->measured && fabs(child->height - it->target) < 0.01) {
                layout_move_box(ctx, it->idx, x, y);
            } else {
                child->height = it->target;
                child->x = x; child->y = y;
                layout_update_content_sizes(child);
                todo[ntodo++] = it->idx;
            }
        }
    }
    if (ntodo > 0) layout_units(ctx, todo, ntodo);
    for (int i = 0; i < nkids; i++) {
        FlexItem *it = &items[i];
        if (it->is_text) continue;
        LayoutBox *child = layout_box(ctx, it->idx);
        if (is_row) {
            /* An item's own layout may not change its definite main size. */
            child->width = it->target;
            if (child->height < 0.0) child->height = 0.0;
            child->height = flex_clamp(child->height, it->min_cross, it->max_cross);
            it->cross = child->height;
        } else {
            child->height = it->target;
            it->cross = child->width;
        }
        layout_update_content_sizes(child);
    }

    /* ---- 5. cross sizes and alignment ---- */
    double inner_cross = is_row ? (cross_def ? chh : 0.0) : cw;
    double total_cross = 0.0;
    for (int li = 0; li < nlines; li++) {
        double m = 0.0;
        for (int j = 0; j < lines[li].count; j++) {
            FlexItem *it = &items[lines[li].start + j];
            double o = it->cross + it->cm_s + it->cm_e;
            if (o > m) m = o;
        }
        lines[li].cross_size = m;
        total_cross += m;
    }
    if (nlines > 1) total_cross += (nlines - 1) * gap_cross;
    if (!cross_def) {
        /* Auto cross size (row container, auto height). */
        inner_cross = flex_clamp(total_cross, cmin_h, cmax_h);
    }
    if (!do_wrap && nlines == 1) {
        lines[0].cross_size = inner_cross;
    } else if (nlines > 0 && inner_cross > total_cross) {
        /* align-content: normal/stretch distributes the extra space. */
        double extra = (inner_cross - total_cross) / nlines;
        for (int li = 0; li < nlines; li++) lines[li].cross_size += extra;
    }
    {
        double cpos = 0.0;
        for (int li = 0; li < nlines; li++) {
            int l = wrap_rev ? nlines - 1 - li : li;
            lines[l].cross_pos = cpos;
            cpos += lines[l].cross_size + gap_cross;
        }
    }

    int *relayout = todo;   /* reuse */
    int nrelayout = 0;
    for (int li = 0; li < nlines; li++) {
        for (int j = 0; j < lines[li].count; j++) {
            FlexItem *it = &items[lines[li].start + j];
            LayoutBox *child = layout_box(ctx, it->idx);
            double lc = lines[li].cross_size;
            double size = it->cross;
            double off = 0.0;
            double room = lc - it->cm_s - it->cm_e;
            bool stretched = false;
            if (it->auto_cs || it->auto_ce) {
                double fr = room - size;
                if (fr > 0.0) off = (it->auto_cs && it->auto_ce) ? fr / 2.0 : (it->auto_cs ? fr : 0.0);
            } else {
                switch (it->align) {
                case CSS_ALIGN_STRETCH:
                    if (!it->cross_def && !it->is_text) {
                        double ns = flex_clamp(room < 0.0 ? 0.0 : room, it->min_cross, it->max_cross);
                        if (fabs(ns - size) > 0.01) { size = ns; stretched = true; }
                    }
                    break;
                case CSS_ALIGN_CENTER: off = (room - size) / 2.0; break;
                case CSS_ALIGN_FLEX_END: off = room - size; break;
                default: break;
                }
            }
            if (wrap_rev && it->align != CSS_ALIGN_STRETCH && !(it->auto_cs || it->auto_ce))
                off = room - size - off;
            double cpos = cross_start + lines[li].cross_pos + it->cm_s + off;
            double x = is_row ? it->main_pos : cpos;
            double y = is_row ? cpos : it->main_pos;
            if (it->is_text) {
                child->x = x; child->y = y;
                continue;
            }
            layout_move_box(ctx, it->idx, x, y);
            if (stretched) {
                if (is_row) child->height = size; else child->width = size;
                layout_update_content_sizes(child);
                if (!is_row || layout_height_sensitive(ctx, it->idx)) relayout[nrelayout++] = it->idx;
            }
        }
    }
    if (nrelayout > 0) {
        layout_units(ctx, relayout, nrelayout);
        /* Re-layout must keep the stretched sizes. */
        for (int li = 0; li < nlines; li++) {
            for (int j = 0; j < lines[li].count; j++) {
                FlexItem *it = &items[lines[li].start + j];
                if (it->is_text) continue;
                LayoutBox *child = layout_box(ctx, it->idx);
                if (is_row) child->width = it->target; else child->height = it->target;
                layout_update_content_sizes(child);
            }
        }
    }

    /* ---- container auto size ---- */
    if (is_row && !cross_def) {
        ct->height = inner_cross + layout_vertical_border_padding(ct);
        layout_update_content_sizes(ct);
    } else if (!is_row && !main_def) {
        ct->height = avail_main + layout_vertical_border_padding(ct);
        layout_update_content_sizes(ct);
    }

    free(todo);
    free(measure);
    free(lines);
    free(items);
    free(kids);
}

/* Position and size one box's subtree, given the box itself is already placed
 * (x/y) and width-resolved by its parent.  Dispatches to the appropriate
 * formatting context, then the auto-height is resolved from children. */
/* Minimal table-row layout: a <tr> places its <td>/<th> children side by
 * side across the row's content width.  Cells whose subtree is dominated by
 * a replaced element (an image) take that content's width (plus their own
 * padding/border); the remaining width is shared equally among the other
 * cells.  The row height is the tallest cell and all cells stretch to it,
 * matching table-row box semantics for backgrounds and borders. */
static void layout_shift_subtree_y(LayoutContext *ctx, int idx, double dy)
{
    for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
         c = ctx->tree.nodes[c].next_sibling_idx) {
        LayoutBox *child = layout_box(ctx, c);
        child->y += dy;
        layout_shift_subtree_y(ctx, c, dy);
    }
}

static void layout_table_row(LayoutContext *ctx, int idx)
{
    LayoutBox *row = layout_box(ctx, idx);
    int nkids = 0;
    int *kids = layout_collect_flow_children(ctx, idx, &nkids);

    /* Keep only real cell elements (td/th); whitespace text between cells
     * generates no box in the row. */
    int cells_count = 0;
    for (int i = 0; i < nkids; i++) {
        HtmlNode *dom = layout_node_dom(ctx, ctx->tree.nodes[kids[i]].dom_node_idx);
        if (!dom || dom->type != HTML_NODE_ELEMENT) continue;
        if (strcasecmp(dom->tag_name, "td") != 0 && strcasecmp(dom->tag_name, "th") != 0) continue;
        kids[cells_count++] = kids[i];
    }
    if (cells_count == 0) {
        free(kids);
        layout_block_flow(ctx, idx);
        return;
    }

    double inner_left = row->x + row->padding_left + row->border_left;
    double inner_top  = row->y + row->padding_top + row->border_top;
    double inner_w    = row->content_width;

    /* Pass 1: assign cell widths — replaced-content cells size to content,
     * text cells share what remains. */
    double *widths = (double*)calloc((size_t)cells_count, sizeof(double));
    double fixed_sum = 0.0;
    int flex_count = 0;
    for (int i = 0; i < cells_count; i++) {
        int c = kids[i];
        LayoutBox *cell = layout_box(ctx, c);
        HtmlNode *dom = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
        double hp = cell->padding_left + cell->padding_right
                  + cell->border_left + cell->border_right;
        double w = 0.0;
        if (cell->width_percent > 0.0) {
            w = inner_w * cell->width_percent + cell->width_pct_px;
        } else if (cell->width_set && cell->css_width > 0.0) {
            w = cell->css_width;
            if (cell->box_sizing == CSS_BOX_SIZING_CONTENT_BOX) w += hp;
        } else if (dom) {
            double iw = 0.0, ih = 0.0;
            if (layout_first_img_size(ctx->doc, dom, &iw, &ih) && iw > 0.0) {
                w = iw + hp;
            }
        }
        if (w > 0.0) {
            widths[i] = w;
            fixed_sum += w;
        } else {
            flex_count++;
        }
    }
    double remain = inner_w - fixed_sum;
    if (remain < 0.0) remain = 0.0;
    double flex_w = (flex_count > 0) ? (remain / flex_count) : 0.0;

    /* Pass 2: place the cells left to right and lay out their subtrees. */
    double cx = inner_left;
    double row_h = 0.0;
    for (int i = 0; i < cells_count; i++) {
        int c = kids[i];
        LayoutBox *cell = layout_box(ctx, c);
        double w = (widths[i] > 0.0) ? widths[i] : flex_w;
        cell->x = cx;
        cell->y = inner_top;
        cell->width = w;
        layout_update_content_sizes(cell);
        cx += w;
    }
    /* Cells are placed; their subtrees are independent units. */
    layout_units(ctx, kids, cells_count);
    for (int i = 0; i < cells_count; i++) {
        LayoutBox *cell = layout_box(ctx, kids[i]);
        if (cell->height > row_h) row_h = cell->height;
    }

    /* Stretch every cell to the row height so backgrounds/borders cover the
     * full row.  Cell content obeys vertical-align: td/th default to middle
     * (HTML UA stylesheet), so shorter content is centered vertically within
     * the row; top/baseline keep it at the top, bottom pushes it down. */
    for (int i = 0; i < cells_count; i++) {
        LayoutBox *cell = layout_box(ctx, kids[i]);
        double free_h = row_h - cell->height;
        if (free_h > 0.0) {
            double dy = 0.0;
            if (cell->vertical_align == 1) dy = free_h * 0.5;
            else if (cell->vertical_align == 3) dy = free_h;
            if (dy != 0.0) layout_shift_subtree_y(ctx, kids[i], dy);
        }
        if (cell->height < row_h) {
            cell->height = row_h;
            layout_update_content_sizes(cell);
        }
    }
    row->height = row_h + row->padding_top + row->padding_bottom
                + row->border_top + row->border_bottom;
    layout_update_content_sizes(row);
    free(widths);
    free(kids);
}

static void layout_node_serial(LayoutContext *ctx, int idx)
{
    LayoutBox *box = layout_box(ctx, idx);

    if (box->display == CSS_DISPLAY_NONE) return;
    if (box->display == CSS_DISPLAY_INLINE) {
        /* Inline boxes don't establish a block formatting context, but their
         * inline children still need positions: flow them left-to-right,
         * measuring text with real glyph metrics when the font is available
         * so positions match the parent's measured width. */
        double cx = box->x + box->padding_left + box->border_left;
        double cy = box->y + box->padding_top + box->border_top;
        /* If this inline box was word-wrapped by the block-flow inline branch
         * and it has exactly one significant child (the common <a>text</a>
         * case, possibly nested like <b><a>text</a></b>), propagate the wrap
         * geometry down so the text leaf emits wrapped glyphs. */
        int wrap_target = -1;
        if (box->wrap_cont_w > 0.0) {
            int sig_count = 0;
            for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
                 c = ctx->tree.nodes[c].next_sibling_idx) {
                LayoutBox *child = layout_box(ctx, c);
                if (child->display == CSS_DISPLAY_NONE) continue;
                HtmlNode *dn = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
                if (dn && dn->type == HTML_NODE_TEXT &&
                    layout_text_is_whitespace(dn->text_content)) continue;
                sig_count++;
                wrap_target = c;
            }
            if (sig_count != 1) wrap_target = -1;
        }
        for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
             c = ctx->tree.nodes[c].next_sibling_idx) {
            LayoutBox *child = layout_box(ctx, c);
            if (child->display == CSS_DISPLAY_NONE) continue;
            HtmlNode *dom = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
            layout_resolve_used_sizes(child, dom, box->content_width, box->content_height);
            if (c == wrap_target) {
                child->x = box->x;
                child->y = box->y;
                child->width = box->width;
                child->height = box->height;
                child->wrap_first_w = box->wrap_first_w;
                child->wrap_cont_w = box->wrap_cont_w;
                child->wrap_cont_x = box->wrap_cont_x;
                child->wrap_cont2_x = box->wrap_cont2_x;
                child->wrap_cont2_w = box->wrap_cont2_w;
                child->wrap_cont2_line = box->wrap_cont2_line;
                layout_update_content_sizes(child);
                layout_node_serial(ctx, c);
                continue;
            }
            if (layout_box_is_atomic_inline(child) && dom && dom->type == HTML_NODE_ELEMENT &&
                strcasecmp(dom->tag_name, "img") != 0) {
                layout_size_atomic_inline(ctx, c, 1e9);
                child->x = cx + child->margin_left;
                child->y = cy + child->margin_top;
                layout_update_content_sizes(child);
                layout_node_serial(ctx, c);
                cx += child->margin_left + child->width + child->margin_right;
                continue;
            }
            double fs = child->font_size > 0.0 ? child->font_size : 16.0;
            double line_adv = layout_line_advance_box(child);
            bool sized = false;
            if (dom && dom->type == HTML_NODE_TEXT) {
                if (layout_text_is_whitespace(dom->text_content)) {
                    /* Same contextual collapse as the block-flow inline branch:
                     * a space between inline content, nothing at the start. */
                    double content_left_i = box->x + box->padding_left + box->border_left;
                    if (cx > content_left_i + 0.001) {
                        double sw = 0.0;
                        if (!layout_measure_text_styled(child, " ", fs, &sw, NULL)) sw = fs * 0.3;
                        child->width = sw;
                    } else {
                        child->width = 0.0;
                    }
                    child->height = 0.0;
                    sized = true;
                } else {
                    double tw = 0.0;
                    if (layout_measure_text_styled(child, css_layout_box_text(ctx, c, dom->text_content),
                                                   fs, &tw, NULL)) {
                        child->width = tw;
                        child->height = line_adv;
                        sized = true;
                    }
                }
            }
            if (!sized) {
                /* Direct <img> children get their attribute size; other
                 * elements without text size from an <img> descendant. */
                double iw = 0.0, ih = 0.0;
                if (dom && dom->type == HTML_NODE_ELEMENT &&
                    strcasecmp(dom->tag_name, "img") == 0) {
                    /* Sized by layout_resolve_used_sizes (replaced element). */
                    sized = true;
                } else if (dom && layout_first_img_size(ctx->doc, dom, &iw, &ih)) {
                    child->width = iw;
                    child->height = ih;
                    sized = true;
                }
            }
            if (!sized && dom && dom->type == HTML_NODE_ELEMENT &&
                child->display == CSS_DISPLAY_INLINE) {
                /* Nested inline element: its max-content width (text, atomic
                 * inline descendants and its own padding). */
                double iw = layout_intrinsic_width(ctx, c, true);
                if (iw > 0.0) {
                    child->width = iw;
                    child->height = line_adv;
                    sized = true;
                }
            }
            if (!sized && dom && dom->type == HTML_NODE_ELEMENT) {
                /* Inline element with descendant text (<a>, <b>, <span>, ...):
                 * measure its real text width, mirroring the block-flow
                 * inline branch, instead of falling back to a fake size that
                 * pushes following siblings out of the box (overlap). */
                char concat[8192];
                size_t cl = 0;
                concat[0] = '\0';
                layout_concat_dom_text(ctx->doc, dom, concat, sizeof(concat), &cl);
                if (cl > 0 && !layout_text_is_whitespace(concat)) {
                    double tw = 0.0;
                    if (layout_measure_text_styled(child, concat, fs, &tw, NULL)) {
                        child->width = tw;
                        child->height = line_adv;
                        sized = true;
                    }
                }
            }
            if (!sized) {
                /* Empty inline element with no text/img content and no
                 * explicit size: shrink to zero (mirrors the block-flow
                 * inline branch) instead of resurrecting a fake 80x20 box
                 * that displaces following content. Replaced form elements
                 * keep a small default size. */
                if (dom && layout_is_replaced_form_element(dom->tag_name)) {
                    if (child->width <= 0.0) child->width = 80.0;
                    if (child->height <= 0.0) child->height = 20.0;
                } else {
                    if (child->width <= 0.0) child->width = 0.0;
                    if (child->height <= 0.0) child->height = 0.0;
                }
            }
            child->x = cx;
            child->y = cy;
            layout_update_content_sizes(child);
            cx += child->width;
            layout_node_serial(ctx, c);
        }
        return;
    }

    if (box->display == CSS_DISPLAY_FLEX) {
        layout_flex_container(ctx, idx);
        return;
    }

    /* Table rows get a (minimal) horizontal cell layout. */
    HtmlNode *rowdom = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    if (rowdom && rowdom->type == HTML_NODE_ELEMENT &&
        strcasecmp(rowdom->tag_name, "tr") == 0) {
        layout_table_row(ctx, idx);
        return;
    }

    /* Block (and grid/other treated as block). */
    layout_block_flow(ctx, idx);
}

/* Position the root box at the viewport origin and size it to the viewport. */
static void layout_root(LayoutContext *ctx)
{
    int root = ctx->tree.root_idx;
    if (root < 0) return;
    LayoutBox *box = layout_box(ctx, root);
    box->x = 0.0;
    box->y = 0.0;
    box->width = ctx->viewport_width;
    box->height = ctx->viewport_height;
    layout_update_content_sizes(box);
}

/* ============================================================================
 * Layout debugging dump
 * ============================================================================ */

static void layout_dump_boxes(LayoutContext *ctx, const char *path)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "idx\ttag\tid\tclass\tdisplay\tx\ty\twidth\theight\tmargin_t\tmargin_r\tmargin_b\tmargin_l\tpadding_t\tpadding_r\tpadding_b\tpadding_l\tborder_t\tborder_r\tborder_b\tborder_l\tflex_grow\tflex_shrink\tposition\tbox_sizing\tbg\tvisibility\tfont_size\ttext\n");
    for (int i = 0; i < ctx->tree.count; i++) {
        LayoutBox *b = &ctx->boxes[i];
        HtmlNode *node = layout_node_dom(ctx, ctx->tree.nodes[i].dom_node_idx);
        const char *tag = (node && node->type == HTML_NODE_ELEMENT) ? node->tag_name : "#text";
        const char *id = layout_node_attribute(node, "id");
        const char *cls = layout_node_attribute(node, "class");
        char id_buf[64] = {0};
        char cls_buf[128] = {0};
        if (id) {
            strncpy(id_buf, id, sizeof(id_buf) - 1);
            for (size_t k = 0; k < strlen(id_buf); k++) if (id_buf[k] == '\t' || id_buf[k] == '\n') id_buf[k] = ' ';
        }
        if (cls) {
            strncpy(cls_buf, cls, sizeof(cls_buf) - 1);
            for (size_t k = 0; k < strlen(cls_buf); k++) if (cls_buf[k] == '\t' || cls_buf[k] == '\n') cls_buf[k] = ' ';
        }
        const char *dname = "block";
        switch (b->display) {
            case CSS_DISPLAY_BLOCK: dname = "block"; break;
            case CSS_DISPLAY_INLINE: dname = "inline"; break;
            case CSS_DISPLAY_INLINE_BLOCK: dname = "inline-block"; break;
            case CSS_DISPLAY_FLEX: dname = "flex"; break;
            case CSS_DISPLAY_GRID: dname = "grid"; break;
            case CSS_DISPLAY_NONE: dname = "none"; break;
            default: dname = "other"; break;
        }
        const char *pname = "static";
        switch (b->position) {
            case CSS_POSITION_RELATIVE: pname = "relative"; break;
            case CSS_POSITION_ABSOLUTE: pname = "absolute"; break;
            case CSS_POSITION_FIXED:    pname = "fixed"; break;
            case CSS_POSITION_STICKY:   pname = "sticky"; break;
            default: break;
        }
        const char *bsname = "content-box";
        if (b->box_sizing == CSS_BOX_SIZING_BORDER_BOX) bsname = "border-box";
        char bg_buf[32] = "-";
        if (b->background_color_a > 0.0) {
            snprintf(bg_buf, sizeof(bg_buf), "#%02x%02x%02x/a%.2f",
                     (int)(b->background_color_r * 255.0),
                     (int)(b->background_color_g * 255.0),
                     (int)(b->background_color_b * 255.0),
                     b->background_color_a);
        }
        const char *vname = (b->visibility == CSS_VISIBILITY_HIDDEN) ? "hidden" : "visible";
        char txt_buf[48] = "";
        if (node && node->type == HTML_NODE_TEXT && node->text_content) {
            strncpy(txt_buf, node->text_content, sizeof(txt_buf) - 1);
            for (size_t k = 0; k < strlen(txt_buf); k++)
                if (txt_buf[k] == '\t' || txt_buf[k] == '\n' || txt_buf[k] == '\r') txt_buf[k] = ' ';
        }
        fprintf(fp, "%d\t%s\t%s\t%s\t%s\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%s\t%s\t%s\t%s\t%.2f\t%s\n",
                i, tag, id_buf, cls_buf, dname,
                b->x, b->y, b->width, b->height,
                b->margin_top, b->margin_right, b->margin_bottom, b->margin_left,
                b->padding_top, b->padding_right, b->padding_bottom, b->padding_left,
                b->border_top, b->border_right, b->border_bottom, b->border_left,
                b->flex_grow, b->flex_shrink, pname, bsname, bg_buf, vname,
                b->font_size, txt_buf);
    }
    fclose(fp);
}

/* ============================================================================
 * Absolute positioning post-pass
 * ============================================================================ */

/* Find the nearest ancestor that establishes a containing block for an
 * absolutely positioned box.  In our subset that is the nearest ancestor with
 * a non-static position (fixed uses the viewport, so it is handled separately
 * in the top-down pass). */
static int layout_positioned_ancestor(LayoutContext *ctx, int idx)
{
    int p = ctx->tree.nodes[idx].parent_idx;
    while (p >= 0) {
        CssPosition pos = layout_box(ctx, p)->position;
        if (pos == CSS_POSITION_RELATIVE ||
            pos == CSS_POSITION_ABSOLUTE ||
            pos == CSS_POSITION_FIXED ||
            pos == CSS_POSITION_STICKY) {
            return p;
        }
        p = ctx->tree.nodes[p].parent_idx;
    }
    return -1;
}

/* Position an absolutely positioned box against its containing block.
 * This runs after the normal flow passes so containing-block sizes are final. */
/* Static position (§10.3.7): where the box's margin edge would have been
 * in normal flow, derived from the final positions of its in-flow siblings. */
static void layout_static_position(LayoutContext *ctx, int idx, double *sx, double *sy)
{
    int p = ctx->tree.nodes[idx].parent_idx;
    if (p < 0) { *sx = 0.0; *sy = 0.0; return; }
    LayoutBox *P = layout_box(ctx, p);
    *sx = P->x + P->border_left + P->padding_left;
    *sy = P->y + P->border_top + P->padding_top;
    if (P->display == CSS_DISPLAY_FLEX || P->display == CSS_DISPLAY_GRID) return;
    for (int k = ctx->tree.nodes[idx].prev_sibling_idx; k >= 0; k = ctx->tree.nodes[k].prev_sibling_idx) {
        LayoutBox *kb = layout_box(ctx, k);
        if (!layout_is_in_flow(kb) || kb->float_side != 0) continue;
        HtmlNode *kn = layout_node_dom(ctx, ctx->tree.nodes[k].dom_node_idx);
        if (kn && kn->type == HTML_NODE_TEXT && layout_text_is_whitespace(kn->text_content)) continue;
        if (layout_box_is_block_level(kb)) {
            *sy = kb->y + kb->height + kb->margin_bottom;
        } else {
            *sx = kb->x + kb->width + kb->margin_right;
            *sy = kb->y;
        }
        return;
    }
}

static void layout_position_absolute_box(LayoutContext *ctx, int idx)
{
    LayoutBox *box = layout_box(ctx, idx);
    double static_x, static_y;
    layout_static_position(ctx, idx, &static_x, &static_y);

    /* The containing block is the padding box of the nearest positioned
     * ancestor; fixed boxes use the viewport (scroll position 0). */
    double cb_x, cb_y, cb_w, cb_h;
    int anc = box->position == CSS_POSITION_FIXED ? -1 : layout_positioned_ancestor(ctx, idx);
    if (anc >= 0) {
        LayoutBox *a = layout_box(ctx, anc);
        cb_x = a->x + a->border_left;
        cb_y = a->y + a->border_top;
        cb_w = a->width - a->border_left - a->border_right;
        cb_h = a->height - a->border_top - a->border_bottom;
        if (cb_w < 0.0) cb_w = 0.0;
        if (cb_h < 0.0) cb_h = 0.0;
    } else {
        cb_x = 0.0;
        cb_y = 0.0;
        cb_w = ctx->viewport_width;
        cb_h = ctx->viewport_height;
    }

    bool has_left = (box->positioned_sides & LAYOUT_SIDE_LEFT) != 0;
    bool has_right = (box->positioned_sides & LAYOUT_SIDE_RIGHT) != 0;
    bool has_top = (box->positioned_sides & LAYOUT_SIDE_TOP) != 0;
    bool has_bottom = (box->positioned_sides & LAYOUT_SIDE_BOTTOM) != 0;
    layout_resolve_insets(box, cb_w, cb_h);

    layout_resolve_used_sizes(box, layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx),
                              cb_w, cb_h);
    bool width_auto = !(box->width_set || box->width_percent > 0.0 || box->width_keyword);
    double ml = (box->margin_auto & 8) ? 0.0 : box->margin_left;
    double mr = (box->margin_auto & 2) ? 0.0 : box->margin_right;
    double mt = (box->margin_auto & 1) ? 0.0 : box->margin_top;
    double mb = (box->margin_auto & 4) ? 0.0 : box->margin_bottom;

    /* ---- horizontal (§10.3.7) ---- */
    if (width_auto) {
        if (has_left && has_right) {
            box->width = cb_w - box->left - box->right - ml - mr;
        } else {
            double avail = cb_w - (has_left ? box->left : 0.0) - (has_right ? box->right : 0.0) - ml - mr;
            box->width = layout_shrink_to_fit(ctx, idx, avail < 0.0 ? 0.0 : avail);
        }
        if (box->max_width > 0.0 || box->min_width > 0.0) {
            if (box->box_sizing == CSS_BOX_SIZING_BORDER_BOX)
                box->width = layout_clamp_size(box->width, box->min_width, box->max_width);
            else
                box->width = layout_content_to_total_width(box,
                    layout_clamp_size(layout_total_to_content_width(box, box->width),
                                      box->min_width, box->max_width));
        }
        if (box->width < 0.0) box->width = 0.0;
        layout_update_content_sizes(box);
    }
    if (has_left && has_right && !width_auto && (box->margin_auto & 0xA)) {
        double free_w = cb_w - box->left - box->right - box->width - ml - mr;
        if ((box->margin_auto & 0xA) == 0xA) { ml += free_w / 2.0; }
        else if (box->margin_auto & 8) ml += free_w;
    }
    if (has_left) box->x = cb_x + box->left + ml;
    else if (has_right) box->x = cb_x + cb_w - box->right - mr - box->width;
    else box->x = static_x + ml;

    /* ---- vertical (§10.6.4) ---- */
    bool height_auto = layout_height_auto(box) && !(box->aspect_ratio > 0.0 && box->height > 0.0);
    if (height_auto && has_top && has_bottom) {
        double h = cb_h - box->top - box->bottom - mt - mb;
        box->height = h < 0.0 ? 0.0 : h;
        layout_update_content_sizes(box);
        height_auto = false;
    }
    if (has_top) box->y = cb_y + box->top + mt;
    else box->y = static_y + mt;

    /* Lay out the subtree at its final x (absolute/fixed boxes were skipped
     * by the normal flow pass); auto heights come out of this. */
    layout_node_serial(ctx, idx);

    if (!has_top) {
        double y = box->y;
        if (has_bottom) y = cb_y + cb_h - box->bottom - mb - box->height;
        if (y != box->y) layout_move_box(ctx, idx, box->x, y);
    } else if (has_bottom && !height_auto && (box->margin_auto & 5)) {
        double free_h = cb_h - box->top - box->bottom - box->height - mt - mb;
        double dy = ((box->margin_auto & 5) == 5) ? free_h / 2.0 : ((box->margin_auto & 1) ? free_h : 0.0);
        if (dy != 0.0) layout_move_box(ctx, idx, box->x, box->y + dy);
    }
}

static void layout_position_absolute_subtree(LayoutContext *ctx, int idx)
{
    LayoutBox *box = layout_box(ctx, idx);
    if (box->display == CSS_DISPLAY_NONE) return;
    if (box->position == CSS_POSITION_ABSOLUTE || box->position == CSS_POSITION_FIXED) {
        layout_position_absolute_box(ctx, idx);
    }
    for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
         c = ctx->tree.nodes[c].next_sibling_idx) {
        layout_position_absolute_subtree(ctx, c);
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

/* After layout, write resolved geometry and used values back into the JS
 * computed-style table so getComputedStyle can return real values. */
static void layout_export_computed_geometry(LayoutContext *ctx)
{
    if (!SAFE_VALID(ctx->js_ctx) ||
        gc_handle_get_type(ctx->js_ctx) != JS_GC_OBJ_TYPE_JS_CONTEXT) return;
    JSContextHandle js = ctx->js_ctx;
    char buf[128];

    for (int i = 0; i < ctx->tree.count; i++) {
        HtmlNode *node = layout_node_dom(ctx, ctx->tree.nodes[i].dom_node_idx);
        if (!node || node->type != HTML_NODE_ELEMENT || !node->has_js_object) continue;

        GCValue element = node->js_object;
        DomWrapHandle dom_node = dom_wrap_of(element);
        if (dom_node == GC_HANDLE_NULL) continue;

        LayoutBox *box = &ctx->boxes[i];
#define SET_VAL(prop, val_str) do { \
    JSAtom atom = JS_NewAtom(js, (prop)); \
    if (atom != JS_ATOM_NULL) { \
        css_computed_set_property(js, dom_node, atom, (val_str)); \
        JS_FreeAtom(js, atom); \
    } \
} while(0)
#define SET_PX(prop, val) do { \
    snprintf(buf, sizeof(buf), "%.2fpx", (val)); \
    SET_VAL(prop, buf); \
} while(0)

        SET_PX("width", box->width);
        SET_PX("height", box->height);
        SET_PX("margin-top", box->margin_top);
        SET_PX("margin-right", box->margin_right);
        SET_PX("margin-bottom", box->margin_bottom);
        SET_PX("margin-left", box->margin_left);
        SET_PX("padding-top", box->padding_top);
        SET_PX("padding-right", box->padding_right);
        SET_PX("padding-bottom", box->padding_bottom);
        SET_PX("padding-left", box->padding_left);
        SET_PX("top", box->y);
        SET_PX("left", box->x);
        SET_PX("font-size", box->font_size);

        const char *display_str = "block";
        switch (box->display) {
            case CSS_DISPLAY_NONE: display_str = "none"; break;
            case CSS_DISPLAY_INLINE: display_str = "inline"; break;
            case CSS_DISPLAY_INLINE_BLOCK: display_str = "inline-block"; break;
            case CSS_DISPLAY_FLEX: display_str = "flex"; break;
            case CSS_DISPLAY_GRID: display_str = "grid"; break;
            default: break;
        }
        SET_VAL("display", display_str);

        const char *visibility_str = "visible";
        if (box->visibility == CSS_VISIBILITY_HIDDEN) visibility_str = "hidden";
        else if (box->visibility == CSS_VISIBILITY_COLLAPSE) visibility_str = "collapse";
        SET_VAL("visibility", visibility_str);

        const char *position_str = "static";
        switch (box->position) {
            case CSS_POSITION_RELATIVE: position_str = "relative"; break;
            case CSS_POSITION_ABSOLUTE: position_str = "absolute"; break;
            case CSS_POSITION_FIXED: position_str = "fixed"; break;
            case CSS_POSITION_STICKY: position_str = "sticky"; break;
            default: break;
        }
        SET_VAL("position", position_str);

        snprintf(buf, sizeof(buf), "rgba(%d, %d, %d, %.3f)",
                 (int)(box->color_r * 255.0 + 0.5),
                 (int)(box->color_g * 255.0 + 0.5),
                 (int)(box->color_b * 255.0 + 0.5),
                 box->color_a);
        SET_VAL("color", buf);

        snprintf(buf, sizeof(buf), "rgba(%d, %d, %d, %.3f)",
                 (int)(box->background_color_r * 255.0 + 0.5),
                 (int)(box->background_color_g * 255.0 + 0.5),
                 (int)(box->background_color_b * 255.0 + 0.5),
                 box->background_color_a);
        SET_VAL("background-color", buf);

        /* CSS custom properties are stored separately; mirror them too. */
        if (ctx->custom_props) {
            const CssCustomProps *cp = ctx->custom_props[i];
            for (int j = 0; cp && j < cp->count; j++) {
                SET_VAL(cp->props[j].name, cp->props[j].value);
            }
        }

#undef SET_PX
#undef SET_VAL
    }
}

static void layout_flow_job(Job *j)
{
    LayoutContext *ctx = (LayoutContext *)j->ctx;
    if (ctx->tree.root_idx >= 0) layout_node_serial(ctx, ctx->tree.root_idx);
    if (ctx->tree.count > 0) layout_position_absolute_subtree(ctx, ctx->tree.root_idx);
}

bool css_layout_document(LayoutContext *ctx, CssStylesheet *sheet)
{
    if (!ctx || ctx->tree.count == 0) return false;

    if (g_css_log < 0) { const char *e = getenv("CSS_LOG"); g_css_log = e ? atoi(e) : 0; }
    STAGE_BEGIN();
    g_layout_debug = (cyber_getenv("CYBER_DEBUG_MEASURE") ? LDBG_MEASURE : 0) |
                     (cyber_getenv("CYBER_DEBUG_FLOAT") ? LDBG_FLOAT : 0) |
                     (cyber_getenv("CYBER_DEBUG_INLINE") ? LDBG_INLINE : 0) |
                     (cyber_getenv("CYBER_DEBUG_FLEX") ? LDBG_FLEX : 0);

    /* Cascade: match selectors, resolve custom properties, apply declarations. */
    STAGE("setup");
    layout_apply_stylesheet(ctx, sheet);
    _st_t = layout_now_ms();

    /* Size and position the root box to the viewport. */
    layout_root(ctx);

    g_layout_ctx = ctx;
    /* White-space processing (CSS Text §4.1.1): collapse runs of spaces,
     * tabs and newlines to one space unless white-space preserves them. */
    if (!ctx->text_norm) ctx->text_norm = (char **)calloc((size_t)ctx->tree.count + 1, sizeof(char *));
    if (ctx->text_norm) {
        for (int i = 0; i < ctx->tree.count; i++) {
            free(ctx->text_norm[i]);
            ctx->text_norm[i] = NULL;
            HtmlNode *tn = layout_node_dom(ctx, ctx->tree.nodes[i].dom_node_idx);
            if (!tn || tn->type != HTML_NODE_TEXT || !tn->text_content) continue;
            unsigned char ws = ctx->boxes[i].white_space;
            if (ws == 2 || ws == 3) continue;
            const char *t = tn->text_content;
            bool needs = false;
            for (const char *q = t; *q; q++) {
                if (*q == '\n' || *q == '\t' || *q == '\r' || *q == '\f' || (*q == ' ' && q[1] == ' ')) {
                    needs = true;
                    break;
                }
            }
            if (!needs) continue;
            size_t L = strlen(t);
            char *o = (char *)malloc(L + 1);
            if (!o) continue;
            size_t k = 0;
            bool in_space = false;
            for (const char *q = t; *q; q++) {
                char ch = *q;
                if (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r' || ch == '\f') {
                    if (!in_space) o[k++] = ' ';
                    in_space = true;
                } else {
                    o[k++] = ch;
                    in_space = false;
                }
            }
            o[k] = '\0';
            ctx->text_norm[i] = o;
        }
    }
    free(ctx->intrinsic_cache);
    ctx->intrinsic_cache = (double *)malloc(((size_t)ctx->tree.count + 1) * 2 * sizeof(double));
    if (ctx->intrinsic_cache)
        for (size_t i = 0; i < ((size_t)ctx->tree.count + 1) * 2; i++) ctx->intrinsic_cache[i] = -1.0;

    /* Parallel-layout bookkeeping: subtree sizes and float presence. */
    {
        int n = ctx->tree.count;
        const char *pe = getenv("CSS_LAYOUT_PAR");
        const char *ge = getenv("CSS_LAYOUT_GRAIN");
        g_lpar.enabled = job_thread_count() > 1 && !(pe && strcmp(pe, "0") == 0);
        g_lpar.min_nodes = (ge && atoi(ge) > 0) ? atoi(ge) : 64;
        atomic_store(&g_lpar.regions, 0);
        atomic_store(&g_lpar.units, 0);
        atomic_store(&g_lpar.serial_regions, 0);
        g_lpar.has_float = (uint8_t *)calloc((size_t)n + 1, 1);
        g_lpar.subtree = (int *)malloc(((size_t)n + 1) * sizeof(int));
        for (int i = 0; i < n; i++) g_lpar.subtree[i] = 1;
        for (int pos = n - 1; pos >= 0; pos--) {
            int i = ctx->tree.preorder[pos];
            if (ctx->boxes[i].float_side != 0) g_lpar.has_float[i] = 1;
            int par = ctx->tree.nodes[i].parent_idx;
            if (par >= 0) {
                g_lpar.subtree[par] += g_lpar.subtree[i];
                g_lpar.has_float[par] |= g_lpar.has_float[i];
            }
        }
    }
    STAGE("layout setup");

    /* One recursive pass: each container positions and sizes its in-flow
     * children, recurses, then resolves its auto height.  Flex items and
     * table cells fork into jobs (layout_units).  Then absolutely/fixed
     * positioned boxes are placed against their final containing blocks. */
    job_graph_run(job_new(layout_flow_job, NULL, ctx, 0, 0));
    /* Translations (transform) shift boxes and their subtrees after layout;
     * preorder so nested translations accumulate. */
    for (int k = 0; k < ctx->tree.count; k++) {
        int i = ctx->tree.preorder[k];
        LayoutBox *b = &ctx->boxes[i];
        int par = ctx->tree.nodes[i].parent_idx;
        b->opacity_eff = (par >= 0 ? ctx->boxes[par].opacity_eff : 1.0) *
                         (b->opacity_set ? b->opacity : 1.0);
        if (b->display == CSS_DISPLAY_NONE) continue;
        double dx = 0.0, dy = 0.0;
        if ((b->position == CSS_POSITION_RELATIVE || b->position == CSS_POSITION_STICKY) &&
            b->positioned_sides) {
            /* Relative offsets (§9.4.3) against the parent's content box;
             * sticky boxes are treated as relative at scroll position 0. */
            int par = ctx->tree.nodes[i].parent_idx;
            double pw = par >= 0 ? ctx->boxes[par].content_width : ctx->viewport_width;
            double ph = par >= 0 ? ctx->boxes[par].content_height : ctx->viewport_height;
            layout_resolve_insets(b, pw, ph);
            if (b->position == CSS_POSITION_RELATIVE) {
                if (b->positioned_sides & LAYOUT_SIDE_LEFT) dx = b->left;
                else if (b->positioned_sides & LAYOUT_SIDE_RIGHT) dx = -b->right;
                if (b->positioned_sides & LAYOUT_SIDE_TOP) dy = b->top;
                else if (b->positioned_sides & LAYOUT_SIDE_BOTTOM) dy = -b->bottom;
            }
        }
        if (b->has_translate) {
            dx += b->translate_x + b->translate_xpct * b->width;
            dy += b->translate_y + b->translate_ypct * b->height;
        }
        if (dx == 0.0 && dy == 0.0) continue;
        b->x += dx;
        b->y += dy;
        layout_offset_subtree(ctx, i, dx, dy);
    }
    STAGE("flow + absolute layout");
    if (g_css_log > 0)
        fprintf(stderr, "[css] Layout: %u parallel regions (%u units), %u serial regions with floats\n",
                atomic_load(&g_lpar.regions), atomic_load(&g_lpar.units),
                atomic_load(&g_lpar.serial_regions));
    free(g_lpar.has_float);
    free(g_lpar.subtree);
    g_lpar.has_float = NULL;
    g_lpar.subtree = NULL;

    for (int i = 0; i < ctx->tree.count; i++) {
        ctx->boxes[i].flags |= LAYOUT_HAS_LAYOUT;
    }

    layout_export_computed_geometry(ctx);
    STAGE("export computed");

    /* The box dump is written only when asked for: by the caller
     * (ctx->dump_path) or by CSS_DUMP=<path>. */
    {
        const char *env = getenv("CSS_DUMP");
        const char *path = (env && env[0]) ? env : ctx->dump_path;
        if (path && path[0]) layout_dump_boxes(ctx, path);
    }
    STAGE("dump");
    return true;
}

bool css_layout_run(LayoutContext *ctx, HtmlDocument *doc, CssStylesheet *sheet,
                    double viewport_width, double viewport_height)
{
    double _run_t0 = layout_now_ms();
    JSContextHandle saved_js_ctx = ctx->js_ctx;
    const char *saved_dump_path = ctx->dump_path;
    if (!css_layout_tree_build(ctx, doc)) return false;
    GC_SET(ctx->js_ctx, saved_js_ctx);
    ctx->dump_path = saved_dump_path;
    ctx->viewport_width = viewport_width;
    ctx->viewport_height = viewport_height;
    g_unit_vw = viewport_width > 0.0 ? viewport_width : 1024.0;
    g_unit_vh = viewport_height > 0.0 ? viewport_height : 768.0;
    g_unit_rem = 16.0;
    /* Base URL for resolving relative stylesheet/image URLs: derive the origin
     * (scheme + host + "/") from the page's start URL.  Fall back to the
     * historical default when no start URL has been set. */
    {
        extern const char *g_cyber_start_url;
        const char *src = (g_cyber_start_url && g_cyber_start_url[0])
                          ? g_cyber_start_url : "https://www.youtube.com/";
        const char *scheme = strstr(src, "://");
        if (scheme) {
            const char *host = scheme + 3;
            const char *slash = strchr(host, '/');
            size_t origin_len = slash ? (size_t)(slash - src) + 1 : strlen(src);
            if (origin_len >= sizeof(ctx->base_url)) origin_len = sizeof(ctx->base_url) - 1;
            gc_memcpy(ctx->base_url, src, origin_len);
            ctx->base_url[origin_len] = '\0';
            if (!slash && origin_len + 1 < sizeof(ctx->base_url)) {
                ctx->base_url[origin_len] = '/';
                ctx->base_url[origin_len + 1] = '\0';
            }
        } else {
            strncpy(ctx->base_url, src, sizeof(ctx->base_url) - 1);
            ctx->base_url[sizeof(ctx->base_url) - 1] = '\0';
        }
    }
    if (g_css_log < 0) { const char *e = getenv("CSS_LOG"); g_css_log = e ? atoi(e) : 0; }
    if (g_css_log > 0) fprintf(stderr, "[css] tree build + setup  %8.3f ms\n", layout_now_ms() - _run_t0);
    bool ok = css_layout_document(ctx, sheet);
    if (!ok) css_layout_tree_free(ctx);
    return ok;
}

LayoutBox* css_layout_box_for_node(LayoutContext *ctx, int dom_node_idx)
{
    if (!ctx) return NULL;
    for (int i = 0; i < ctx->tree.count; i++) {
        if (ctx->tree.nodes[i].dom_node_idx == dom_node_idx) {
            return &ctx->boxes[i];
        }
    }
    return NULL;
}

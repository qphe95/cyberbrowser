/*
 * CSS Layout Engine - Implementation
 *
 * Parallel top-down / bottom-up layout as described in PARALLEL_CSS_LAYOUT.md.
 */

#include "css_layout.h"
#include "css_parser.h"
#include "browser_api_impl.h"
#include "platform.h"
#include "url_utils.h"
#include "quickjs_gc_unified.h"
#include "http_download.h"
#include "html_dom.h"
#include "text_shaper.h"
#include "display_list.h"

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

static void css_custom_props_clear(CssCustomProps *props);

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

static inline LayoutNodeState* layout_state(LayoutContext *ctx, int idx)
{
    return &ctx->states[idx];
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
    ctx->states = (LayoutNodeState*)calloc(active, sizeof(LayoutNodeState));
    if (!ctx->tree.nodes || !ctx->boxes || !ctx->states) return false;

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
        ctx->custom_props = (CssCustomProps*)calloc(ctx->tree.count, sizeof(CssCustomProps));
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
    if (ctx->custom_props) {
        for (int i = 0; i < ctx->tree.count; i++) {
            css_custom_props_clear(&ctx->custom_props[i]);
        }
        free(ctx->custom_props);
    }
    free(ctx->float_stack);
    free(ctx->tree.nodes);
    free(ctx->tree.preorder);
    free(ctx->tree.postorder);
    free(ctx->boxes);
    free(ctx->states);
    memset(ctx, 0, sizeof(*ctx));
}

/* ============================================================================
 * Property resolution helpers
 * ============================================================================ */

static bool css_is_inherited(const char *prop)
{
    static const char *inherited[] = {
        "color", "font-size", "font-family", "line-height", "text-align",
        "visibility", "cursor", "letter-spacing", "word-spacing",
        "white-space", "direction", NULL
    };
    for (int i = 0; inherited[i]; i++) {
        if (strcasecmp(prop, inherited[i]) == 0) return true;
    }
    return false;
}

static double css_parse_length(const char *value, double parent_value, double viewport_value, double font_size);

/* Minimal calc() expression evaluator. Supports +, -, *, /, parentheses and
 * length/percentage operands.  Returns 0 for unsupported expressions. */
typedef struct {
    const char *s;
    size_t len;
    size_t pos;
    double parent_value;
    double viewport_value;
    double font_size;
} CalcParser;

static void calc_skip_space(CalcParser *p)
{
    while (p->pos < p->len && isspace((unsigned char)p->s[p->pos])) p->pos++;
}

static double calc_parse_expr(CalcParser *p);

static double calc_parse_primary(CalcParser *p)
{
    calc_skip_space(p);
    if (p->pos >= p->len) return 0.0;

    if (p->s[p->pos] == '(') {
        p->pos++;
        double v = calc_parse_expr(p);
        calc_skip_space(p);
        if (p->pos < p->len && p->s[p->pos] == ')') p->pos++;
        return v;
    }

    const char *start = p->s + p->pos;
    char *end = NULL;
    double num = strtod(start, &end);
    if (end == start) return 0.0;
    p->pos = (size_t)(end - p->s);
    calc_skip_space(p);

    if (p->pos < p->len && isalpha((unsigned char)p->s[p->pos])) {
        const char *unit_start = p->s + p->pos;
        while (p->pos < p->len && isalpha((unsigned char)p->s[p->pos])) p->pos++;
        size_t unit_len = (size_t)(p->s + p->pos - unit_start);
        char unit[8] = {0};
        if (unit_len < sizeof(unit)) {
            memcpy(unit, unit_start, unit_len);
            for (size_t i = 0; i < unit_len; i++) unit[i] = (char)tolower((unsigned char)unit[i]);
        }
        if (strcmp(unit, "%") == 0) return num * p->parent_value / 100.0;
        if (strcmp(unit, "vw") == 0) return num * p->viewport_value / 100.0;
        if (strcmp(unit, "vh") == 0) return num * p->viewport_value / 100.0;
        if (strcmp(unit, "em") == 0) return num * p->font_size;
        if (strcmp(unit, "rem") == 0) return num * 16.0;
        /* px or unknown unit: treat number as px */
        return num;
    }
    return num; /* unitless */
}

static double calc_parse_term(CalcParser *p)
{
    double lhs = calc_parse_primary(p);
    for (;;) {
        calc_skip_space(p);
        if (p->pos >= p->len) break;
        char op = p->s[p->pos];
        if (op != '*' && op != '/') break;
        p->pos++;
        double rhs = calc_parse_primary(p);
        if (op == '*') lhs *= rhs;
        else if (rhs != 0.0) lhs /= rhs;
    }
    return lhs;
}

static double calc_parse_expr(CalcParser *p)
{
    calc_skip_space(p);
    double lhs = calc_parse_term(p);
    for (;;) {
        calc_skip_space(p);
        if (p->pos >= p->len) break;
        char op = p->s[p->pos];
        if (op != '+' && op != '-') break;
        p->pos++;
        double rhs = calc_parse_term(p);
        if (op == '+') lhs += rhs;
        else lhs -= rhs;
    }
    return lhs;
}

static double css_parse_calc(const char *value, double parent_value, double viewport_value, double font_size)
{
    if (!value) return 0.0;
    while (*value && isspace((unsigned char)*value)) value++;
    if (strncasecmp(value, "calc(", 5) != 0) return 0.0;
    value += 5;
    CalcParser p = {0};
    p.s = value;
    p.len = strlen(value);
    p.parent_value = parent_value;
    p.viewport_value = viewport_value;
    p.font_size = font_size;
    double result = calc_parse_expr(&p);
    calc_skip_space(&p);
    if (p.pos < p.len && p.s[p.pos] == ')') p.pos++;
    return result;
}

static double css_parse_length(const char *value, double parent_value, double viewport_value, double font_size)
{
    if (!value || !*value) return 0.0;
    while (*value && isspace((unsigned char)*value)) value++;
    if (strncasecmp(value, "calc(", 5) == 0) {
        return css_parse_calc(value, parent_value, viewport_value, font_size);
    }
    char *end = NULL;
    double num = strtod(value, &end);
    if (end == value) return 0.0;

    while (*end && isspace((unsigned char)*end)) end++;
    if (strcasecmp(end, "px") == 0 || *end == '\0') return num;
    if (strcasecmp(end, "%") == 0) return num * parent_value / 100.0;
    if (strcasecmp(end, "vw") == 0) return num * viewport_value / 100.0;
    if (strcasecmp(end, "vh") == 0) return num * viewport_value / 100.0;
    if (strcasecmp(end, "em") == 0) return num * font_size;
    if (strcasecmp(end, "rem") == 0) return num * 16.0; /* root em fallback */
    if (strcasecmp(end, "pt") == 0) return num * 96.0 / 72.0;
    if (strcasecmp(end, "pc") == 0) return num * 16.0;
    return num; /* treat unitless as px */
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
static unsigned layout_apply_shorthand_sides(LayoutBox *box, const char *value,
                                             double parent_width, double viewport_width,
                                             double *top, double *right, double *bottom, double *left,
                                             double em_out[4])
{
    double values[4];
    double ems[4] = {0.0, 0.0, 0.0, 0.0};
    unsigned em_flags = 0;
    char *copy = strdup(value);
    char *save = NULL;
    char *tok = strtok_r(copy, " \t", &save);
    int count = 0;
    while (tok && count < 4) {
        values[count] = css_parse_length(tok, parent_width, viewport_width, box->font_size);
        double r;
        if (css_token_is_em(tok, &r)) {
            ems[count] = r;
            em_flags |= (1u << count);
        }
        count++;
        tok = strtok_r(NULL, " \t", &save);
    }
    free(copy);
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
        "img", "br", "wbr", "code", "pre", "sub", "sup", "label",
        "input", "button", "textarea", "select", "iframe", "canvas", NULL
    };
    for (int i = 0; inline_tags[i]; i++) {
        if (strcasecmp(tag_name, inline_tags[i]) == 0) return CSS_DISPLAY_INLINE;
    }
    if (strcasecmp(tag_name, "li") == 0) return CSS_DISPLAY_LIST_ITEM;
    return CSS_DISPLAY_BLOCK;
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
    if (strcasecmp(value, "flex-start") == 0 || strcasecmp(value, "start") == 0) return CSS_ALIGN_FLEX_START;
    if (strcasecmp(value, "flex-end") == 0 || strcasecmp(value, "end") == 0) return CSS_ALIGN_FLEX_END;
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
static void css_parse_flex_shorthand(const char *value, double *grow, double *shrink, double *basis,
                                     double *basis_percent, double font_size)
{
    *grow = 1.0; *shrink = 1.0; *basis = 0.0; *basis_percent = 0.0;
    if (!value || !*value) return;
    if (strcasecmp(value, "none") == 0) { *grow = 0.0; *shrink = 0.0; *basis = -1.0; return; }
    char *copy = strdup(value);
    char *save = NULL;
    char *tok = strtok_r(copy, " \t", &save);
    int idx = 0;
    while (tok) {
        bool is_basis_token = false;
        if (idx == 0) {
            char *end = NULL;
            double num = strtod(tok, &end);
            if (end != tok) { *grow = num; }
            else is_basis_token = true;
        } else if (idx == 1) {
            char *end = NULL;
            double num = strtod(tok, &end);
            if (end != tok) { *shrink = num; }
            else is_basis_token = true;
        } else {
            is_basis_token = true;
        }
        if (is_basis_token) {
            if (css_value_is_percent(tok)) {
                *basis_percent = css_parse_percent_ratio(tok);
                *basis = -1.0;
            } else {
                *basis = css_parse_length(tok, 0, 0, font_size);
            }
        }
        idx++;
        tok = strtok_r(NULL, " \t", &save);
    }
    free(copy);
}

/* Block-level formatting context: these generate a block box that stacks
 * vertically with siblings.  Inline-block is sized like a block but flows
 * on a line with other inline content. */
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

static void layout_apply_declaration(LayoutBox *box, const CssDeclaration *decl,
                                     double parent_width, double viewport_width,
                                     const char *base_url)
{
    const char *prop = decl->property;
    const char *value = decl->value;
    if (!prop || !value) return;

    if (strcasecmp(prop, "display") == 0) {
        box->display = css_parse_display(value);
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
        /* opacity:0 renders nothing — treat like hidden for the subtree. */
        if (strtod(value, NULL) <= 0.0) {
            box->visibility = CSS_VISIBILITY_HIDDEN;
            box->visibility_set = 1;
        }
    } else if (strcasecmp(prop, "position") == 0) {
        box->position = css_parse_position(value);
    } else if (strcasecmp(prop, "box-sizing") == 0) {
        box->box_sizing = css_parse_box_sizing(value);
    } else if (strcasecmp(prop, "width") == 0) {
        if (css_value_is_percent(value)) {
            box->width_percent = css_parse_percent_ratio(value);
            box->css_width = 0.0;
        } else {
            box->css_width = css_parse_length(value, parent_width, viewport_width, box->font_size);
            box->width_percent = 0.0;
        }
        box->width_set = (strcasecmp(value, "auto") != 0);
    } else if (strcasecmp(prop, "height") == 0) {
        if (css_value_is_percent(value)) {
            box->height_percent = css_parse_percent_ratio(value);
            box->css_height = 0.0;
        } else {
            box->css_height = css_parse_length(value, parent_width, viewport_width, box->font_size);
            box->height_percent = 0.0;
        }
        box->height_set = (strcasecmp(value, "auto") != 0);
    } else if (strcasecmp(prop, "min-width") == 0) {
        box->min_width = css_parse_length(value, parent_width, viewport_width, box->font_size);
    } else if (strcasecmp(prop, "max-width") == 0) {
        box->max_width = css_parse_length(value, parent_width, viewport_width, box->font_size);
    } else if (strcasecmp(prop, "min-height") == 0) {
        box->min_height = css_parse_length(value, parent_width, viewport_width, box->font_size);
    } else if (strcasecmp(prop, "max-height") == 0) {
        box->max_height = css_parse_length(value, parent_width, viewport_width, box->font_size);
    } else if (strcasecmp(prop, "top") == 0) {
        box->top = css_parse_length(value, parent_width, viewport_width, box->font_size);
        box->positioned_sides |= LAYOUT_SIDE_TOP;
    } else if (strcasecmp(prop, "left") == 0) {
        box->left = css_parse_length(value, parent_width, viewport_width, box->font_size);
        box->positioned_sides |= LAYOUT_SIDE_LEFT;
    } else if (strcasecmp(prop, "right") == 0) {
        box->right = css_parse_length(value, parent_width, viewport_width, box->font_size);
        box->positioned_sides |= LAYOUT_SIDE_RIGHT;
    } else if (strcasecmp(prop, "bottom") == 0) {
        box->bottom = css_parse_length(value, parent_width, viewport_width, box->font_size);
        box->positioned_sides |= LAYOUT_SIDE_BOTTOM;
    } else if (strcasecmp(prop, "margin") == 0) {
        double em4[4];
        unsigned fl = layout_apply_shorthand_sides(box, value, parent_width, viewport_width,
                                                   &box->margin_top, &box->margin_right,
                                                   &box->margin_bottom, &box->margin_left, em4);
        box->em_deferred &= 0xF0; /* shorthand resets all four margin sides */
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
    } else if (strcasecmp(prop, "margin-right") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->margin_em[1] = r; box->em_deferred |= 0x02; }
        else box->em_deferred &= (unsigned char)~0x02;
        box->margin_right = css_parse_length(value, parent_width, viewport_width, box->font_size);
    } else if (strcasecmp(prop, "margin-top") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->margin_em[0] = r; box->em_deferred |= 0x01; }
        else box->em_deferred &= (unsigned char)~0x01;
        box->margin_top = css_parse_length(value, parent_width, viewport_width, box->font_size);
    } else if (strcasecmp(prop, "margin-bottom") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->margin_em[2] = r; box->em_deferred |= 0x04; }
        else box->em_deferred &= (unsigned char)~0x04;
        box->margin_bottom = css_parse_length(value, parent_width, viewport_width, box->font_size);
    } else if (strcasecmp(prop, "padding") == 0) {
        double em4[4];
        unsigned fl = layout_apply_shorthand_sides(box, value, parent_width, viewport_width,
                                                   &box->padding_top, &box->padding_right,
                                                   &box->padding_bottom, &box->padding_left, em4);
        box->em_deferred &= 0x0F; /* shorthand resets all four padding sides */
        for (int i = 0; i < 4; i++) {
            if (fl & (1u << i)) {
                box->padding_em[i] = em4[i];
                box->em_deferred |= (unsigned char)(0x10u << i);
            }
        }
        box->aspect_ratio = css_parse_percent_ratio(value);
    } else if (strcasecmp(prop, "padding-left") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->padding_em[3] = r; box->em_deferred |= 0x80; }
        else box->em_deferred &= (unsigned char)~0x80;
        box->padding_left = css_parse_length(value, parent_width, viewport_width, box->font_size);
    } else if (strcasecmp(prop, "padding-right") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->padding_em[1] = r; box->em_deferred |= 0x20; }
        else box->em_deferred &= (unsigned char)~0x20;
        box->padding_right = css_parse_length(value, parent_width, viewport_width, box->font_size);
    } else if (strcasecmp(prop, "padding-top") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->padding_em[0] = r; box->em_deferred |= 0x10; }
        else box->em_deferred &= (unsigned char)~0x10;
        box->padding_top = css_parse_length(value, parent_width, viewport_width, box->font_size);
        box->aspect_ratio = css_parse_percent_ratio(value);
    } else if (strcasecmp(prop, "padding-bottom") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->padding_em[2] = r; box->em_deferred |= 0x40; }
        else box->em_deferred &= (unsigned char)~0x40;
        box->padding_bottom = css_parse_length(value, parent_width, viewport_width, box->font_size);
        box->aspect_ratio = css_parse_percent_ratio(value);
    } else if (strcasecmp(prop, "border-left-width") == 0) {
        box->border_left = css_parse_length(value, parent_width, viewport_width, box->font_size);
    } else if (strcasecmp(prop, "border-right-width") == 0) {
        box->border_right = css_parse_length(value, parent_width, viewport_width, box->font_size);
    } else if (strcasecmp(prop, "border-top-width") == 0) {
        box->border_top = css_parse_length(value, parent_width, viewport_width, box->font_size);
    } else if (strcasecmp(prop, "border-bottom-width") == 0) {
        box->border_bottom = css_parse_length(value, parent_width, viewport_width, box->font_size);
    } else if (strcasecmp(prop, "flex-direction") == 0) {
        box->flex_direction = css_parse_flex_direction(value);
    } else if (strcasecmp(prop, "flex-wrap") == 0) {
        box->flex_wrap = css_parse_flex_wrap(value);
    } else if (strcasecmp(prop, "justify-content") == 0) {
        box->justify_content = css_parse_justify_content(value);
    } else if (strcasecmp(prop, "align-items") == 0) {
        box->align_items = css_parse_align_items(value);
    } else if (strcasecmp(prop, "flex-basis") == 0) {
        if (strcasecmp(value, "auto") == 0) {
            box->flex_basis = -1.0;
            box->flex_basis_percent = 0.0;
        } else if (css_value_is_percent(value)) {
            box->flex_basis_percent = css_parse_percent_ratio(value);
            box->flex_basis = -1.0;
        } else {
            box->flex_basis = css_parse_length(value, parent_width, viewport_width, box->font_size);
            box->flex_basis_percent = 0.0;
        }
    } else if (strcasecmp(prop, "flex-grow") == 0) {
        box->flex_grow = strtod(value, NULL);
    } else if (strcasecmp(prop, "flex-shrink") == 0) {
        box->flex_shrink = strtod(value, NULL);
    } else if (strcasecmp(prop, "flex") == 0) {
        css_parse_flex_shorthand(value, &box->flex_grow, &box->flex_shrink, &box->flex_basis,
                                 &box->flex_basis_percent, box->font_size);
    } else if (strcasecmp(prop, "gap") == 0) {
        css_parse_shorthand_1_or_2(value, parent_width, viewport_width,
                                   box->font_size, &box->gap_row, &box->gap_col);
    } else if (strcasecmp(prop, "row-gap") == 0) {
        box->gap_row = css_parse_length(value, parent_width, viewport_width, box->font_size);
    } else if (strcasecmp(prop, "column-gap") == 0) {
        box->gap_col = css_parse_length(value, parent_width, viewport_width, box->font_size);
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
        while (*p) {
            while (*p && (isspace((unsigned char)*p) || *p == '/')) p++;
            if (!*p) break;
            size_t tl = 0;
            while (p[tl] && !isspace((unsigned char)p[tl]) && p[tl] != '/') tl++;
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
            box->line_height = 0.0; /* <=0 => normal (fs x 1.5 at layout) */
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
        while (*p) {
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;
            size_t tl = 0;
            while (p[tl] && !isspace((unsigned char)p[tl])) tl++;
            if (tl >= sizeof(token)) tl = sizeof(token) - 1;
            memcpy(token, p, tl);
            token[tl] = '\0';
            p += tl;
            double w = css_parse_length(token, parent_width, viewport_width, box->font_size);
            if (w > 0.0 && (isdigit((unsigned char)token[0]) || token[0] == '.')) {
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

static bool css_custom_props_inherit(CssCustomProps *dst, const CssCustomProps *src)
{
    if (!src || src->count == 0) return true;
    if (dst->capacity < src->count) {
        CssCustomProp *new_props = (CssCustomProp*)realloc(dst->props,
                                                            src->count * sizeof(CssCustomProp));
        if (!new_props) return false;
        dst->props = new_props;
        dst->capacity = src->count;
    }
    for (int i = 0; i < src->count; i++) {
        dst->props[i].name = strdup(src->props[i].name);
        dst->props[i].value = strdup(src->props[i].value);
        if (!dst->props[i].name || !dst->props[i].value) {
            for (int j = 0; j <= i; j++) {
                free(dst->props[j].name);
                free(dst->props[j].value);
            }
            return false;
        }
    }
    dst->count = src->count;
    return true;
}

static void css_custom_props_clear(CssCustomProps *props)
{
    if (!props) return;
    for (int i = 0; i < props->count; i++) {
        free(props->props[i].name);
        free(props->props[i].value);
    }
    free(props->props);
    props->props = NULL;
    props->count = 0;
    props->capacity = 0;
}

static const char* css_custom_props_get(const CssCustomProps *props, const char *name)
{
    if (!props || !name) return NULL;
    for (int i = 0; i < props->count; i++) {
        if (strcmp(props->props[i].name, name) == 0) return props->props[i].value;
    }
    return NULL;
}

static bool css_custom_props_set(CssCustomProps *props, const char *name, const char *value)
{
    if (!props || !name || !value) return false;
    for (int i = 0; i < props->count; i++) {
        if (strcmp(props->props[i].name, name) == 0) {
            char *v = strdup(value);
            if (!v) return false;
            free(props->props[i].value);
            props->props[i].value = v;
            return true;
        }
    }
    if (props->count >= props->capacity) {
        int new_cap = props->capacity ? props->capacity * 2 : 4;
        CssCustomProp *new_props = (CssCustomProp*)realloc(props->props,
                                                            new_cap * sizeof(CssCustomProp));
        if (!new_props) return false;
        props->props = new_props;
        props->capacity = new_cap;
    }
    props->props[props->count].name = strdup(name);
    props->props[props->count].value = strdup(value);
    if (!props->props[props->count].name || !props->props[props->count].value) {
        free(props->props[props->count].name);
        free(props->props[props->count].value);
        return false;
    }
    props->count++;
    return true;
}

/* Resolve var(--name, fallback) references in a CSS value.  Looks up custom
 * properties in the supplied map and falls back to the comma-separated fallback
 * if the variable is not defined.  Returns a newly allocated string or NULL if
 * no var() references were found. */
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
            size_t flen = (size_t)(fallback_end - fallback);
            char *grown = (char*)realloc(out, out_len + flen + strlen(p) + 1);
            if (!grown) break;
            out = grown;
            memcpy(out + out_len, fallback, flen);
            out_len += flen;
        }
    }

    out[out_len] = '\0';
    return out;
}

/* ============================================================================
 * Stylesheet collection and parallel application
 * ============================================================================ */

typedef struct LayoutStyleSheetList {
    CssStylesheet **sheets;
    int count;
    int capacity;
} LayoutStyleSheetList;

static bool layout_sheet_list_add(LayoutStyleSheetList *list, CssStylesheet *sheet) {
    if (!sheet) return false;
    if (list->count >= list->capacity) {
        int new_cap = list->capacity ? list->capacity * 2 : 4;
        CssStylesheet **new_sheets = (CssStylesheet**)realloc(list->sheets,
                                                               new_cap * sizeof(CssStylesheet*));
        if (!new_sheets) { css_stylesheet_free(sheet); return false; }
        list->sheets = new_sheets;
        list->capacity = new_cap;
    }
    list->sheets[list->count++] = sheet;
    return true;
}

static void layout_sheet_list_free(LayoutStyleSheetList *list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) css_stylesheet_free(list->sheets[i]);
    free(list->sheets);
    list->sheets = NULL;
    list->count = list->capacity = 0;
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

static CssStylesheet* layout_fetch_stylesheet(const char *base_url, const char *href) {
    char *url = layout_resolve_url(base_url, href);
    if (!url) return NULL;

    HttpBuffer buffer = {0};
    char err[256] = {0};
    bool ok = http_get_to_memory(url, &buffer, err, sizeof(err));
    CssStylesheet *sheet = NULL;
    if (ok && buffer.data && buffer.size > 0) {
        LOG_INFO("Fetched stylesheet (%zu bytes)", buffer.size);
        sheet = css_stylesheet_parse(buffer.data, buffer.size);
    } else {
        LOG_WARN("Failed to fetch stylesheet %.80s: %s", url, err[0] ? err : "unknown");
    }
    free(url);
    if (buffer.data) free(buffer.data);
    return sheet;
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
        CssStylesheet *sheet = css_stylesheet_parse(node->text_content, strlen(node->text_content));
        if (sheet) {
            /* If this <style> lives inside a stamped shadow root, scope :host,
             * ::slotted, etc. to the host custom element. */
            const char *host_tag = layout_find_host_tag(ctx, dom_idx);
            if (host_tag) css_scope_stylesheet(sheet, host_tag);
            LOG_INFO("Parsed inline <style> stylesheet with %d rules", sheet->rule_count);
            layout_sheet_list_add(list, sheet);
        }
    } else if (strcasecmp(node->tag_name, "link") == 0) {
        const char *rel = NULL;
        const char *href = NULL;
        for (HtmlAttribute *a = node->attributes; a; a = a->next) {
            if (strcasecmp(a->name, "rel") == 0) rel = a->value;
            else if (strcasecmp(a->name, "href") == 0) href = a->value;
        }
        if (rel && href && strcasecmp(rel, "stylesheet") == 0) {
            CssStylesheet *sheet = layout_fetch_stylesheet(base_url, href);
            if (sheet) layout_sheet_list_add(list, sheet);
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
        if (s && s[0]) {
            CssStylesheet *parsed = css_stylesheet_parse(s, strlen(s));
            if (parsed) {
                LOG_INFO("Parsed constructed CSSStyleSheet with %d rules", parsed->rule_count);
                layout_sheet_list_add(list, parsed);
            }
        }
    }
}

static void layout_collect_constructed_stylesheets(LayoutContext *ctx,
                                                    LayoutStyleSheetList *list)
{
    if (!ctx->js_ctx.valid()) return;
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
        "  var all=document.querySelectorAll('*');"
        "  for(var i=0;i<all.length;i++){"
        "    var host=all[i];"
        "    var sr=host.shadowRoot;"
        "    if(sr){"
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
            if (s && s[0]) {
                CssStylesheet *parsed = css_stylesheet_parse(s, strlen(s));
                if (parsed) layout_sheet_list_add(list, parsed);
            }
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
    "pre { display: block; font-family: monospace; margin: 1em 0; }\n"
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
    "mark { background-color: #ffff00; color: #000000; }\n";

static bool layout_collect_document_stylesheets(LayoutContext *ctx, LayoutStyleSheetList *list,
                                                 const char *base_url) {
    if (!ctx || !list || ctx->tree.count == 0) return false;

    /* Inject the UA stylesheet first so metadata/head/template elements are
     * hidden even when the hardcoded fast-path is bypassed. */
    CssStylesheet *ua = css_stylesheet_parse(g_ua_stylesheet_css, strlen(g_ua_stylesheet_css));
    if (ua) layout_sheet_list_add(list, ua);

    int root_dom_idx = ctx->tree.nodes[ctx->tree.root_idx].dom_node_idx;
    layout_collect_stylesheets_recursive(ctx, root_dom_idx, list, base_url);
    layout_collect_constructed_stylesheets(ctx, list);
    return true;
}

/* Collect matching declarations for one node and return them sorted by cascade.
 * Caller must free *out_applied. */
static bool layout_collect_matched_declarations(LayoutContext *ctx, int idx,
                                                 LayoutStyleSheetList *list,
                                                 CssAppliedDecl **out_applied,
                                                 int *out_count)
{
    *out_applied = NULL;
    *out_count = 0;
    HtmlNode *node = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    if (!node || node->type != HTML_NODE_ELEMENT) return false;

    int cap = 64;
    int count = 0;
    CssAppliedDecl *applied = (CssAppliedDecl*)malloc(cap * sizeof(CssAppliedDecl));
    if (!applied) return false;

    for (int s = 0; s < list->count; s++) {
        CssStylesheet *sheet = list->sheets[s];
        for (int r = 0; r < sheet->rule_count; r++) {
            CssRule *rule = &sheet->rules[r];
            if (!rule->selector_text || !rule->selector_text[0]) continue;
            /* Check the (cheap) media query before the selector match so that
             * responsive rules outside the current viewport are skipped early. */
            if (!css_rule_media_matches(rule, ctx->viewport_width, ctx->viewport_height)) continue;
            if (!css_rule_matches(rule, ctx->doc, node)) continue;
            int spec = rule->specificity;
            if (spec == 0) spec = css_specificity_from_selector_text_matching(
                                      rule->selector_text, ctx->doc, node);

            for (int d = 0; d < rule->declaration_count; d++) {
                if (count >= cap) {
                    int new_cap = cap * 2;
                    CssAppliedDecl *new_app = (CssAppliedDecl*)realloc(applied,
                                                                        new_cap * sizeof(CssAppliedDecl));
                    if (!new_app) break;
                    applied = new_app;
                    cap = new_cap;
                }
                applied[count].decl = &rule->declarations[d];
                applied[count].specificity = spec;
                applied[count].order = s * 1000000 + r * 1000 + d;
                applied[count].important = rule->declarations[d].important;
                count++;
            }
        }
    }

    if (count > 0) {
        qsort(applied, (size_t)count, sizeof(CssAppliedDecl), css_applied_decl_compare);
    }
    *out_applied = applied;
    *out_count = count;
    return true;
}

/* Apply only custom property declarations to a node.  Inheritance from the
 * parent must already have been copied into ctx->custom_props[idx]. */
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

static void layout_apply_stylesheet_node_custom_props(LayoutContext *ctx, int idx,
                                                       LayoutStyleSheetList *list)
{
    HtmlNode *node = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    if (!node || node->type != HTML_NODE_ELEMENT) return;
    CssCustomProps *props = &ctx->custom_props[idx];

    CssAppliedDecl *applied = NULL;
    int count = 0;
    if (list && list->count > 0) {
        layout_collect_matched_declarations(ctx, idx, list, &applied, &count);
    }

    if (applied) {
        for (int d = 0; d < count; d++) {
            const char *prop = applied[d].decl->property;
            const char *val = applied[d].decl->value;
            if (prop && prop[0] == '-' && prop[1] == '-') {
                if (css_var_is_self_reference(prop, val)) continue; /* invalid: unset */
                /* Custom property values may reference other variables; resolve
                 * them against the parent's inherited map for this node. */
                char *resolved = css_var_resolve(val, props);
                css_custom_props_set(props, prop, resolved ? resolved : val);
                free(resolved);
            }
        }
        free(applied);
    }

    /* Inline custom properties override stylesheet rules. */
    for (HtmlAttribute *attr = node->attributes; attr; attr = attr->next) {
        if (strcasecmp(attr->name, "style") != 0) continue;
        int ic = 0;
        CssDeclaration *idecls = css_parse_inline_style(attr->value, &ic);
        for (int i = 0; i < ic; i++) {
            const char *prop = idecls[i].property;
            if (prop && prop[0] == '-' && prop[1] == '-') {
                if (css_var_is_self_reference(prop, idecls[i].value)) continue;
                char *resolved = css_var_resolve(idecls[i].value, props);
                css_custom_props_set(props, prop, resolved ? resolved : idecls[i].value);
                free(resolved);
            }
        }
        css_declarations_free(idecls, ic);
    }
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

/* Test one comma-separated selector branch: does it end in ::pseudo_kw
 * (legacy :pseudo_kw accepted) and does its base selector match the node? */
static bool layout_pseudo_branch_matches(const char *branch, size_t blen,
                                         const char *pseudo_kw,
                                         HtmlDocument *doc, HtmlNode *node)
{
    char buf[512];
    if (blen == 0 || blen >= sizeof(buf)) return false;
    memcpy(buf, branch, blen);
    buf[blen] = '\0';
    char pat2[32];
    snprintf(pat2, sizeof(pat2), "::%s", pseudo_kw);
    char *pseudo = strstr(buf, pat2);
    size_t kwlen = strlen(pat2);
    if (!pseudo) {
        snprintf(pat2, sizeof(pat2), ":%s", pseudo_kw);
        pseudo = strstr(buf, pat2);
        kwlen = strlen(pat2);
    }
    if (!pseudo) return false;
    char *tail = pseudo + kwlen;
    while (*tail && isspace((unsigned char)*tail)) tail++;
    if (*tail) return false;  /* pseudo-element must terminate the branch */
    *pseudo = '\0';
    size_t l = strlen(buf);
    while (l > 0 && isspace((unsigned char)buf[l - 1])) buf[--l] = '\0';
    char *base = buf;
    while (*base && isspace((unsigned char)*base)) base++;
    if (!*base) return false;
    return css_selector_matches(base, doc, node);
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

/* Collect matching declarations for one node, sort by cascade, and apply. */
static void layout_apply_stylesheet_node(LayoutContext *ctx, int idx,
                                          LayoutStyleSheetList *list)
{
    LayoutBox *box = layout_box(ctx, idx);
    HtmlNode *node = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    if (!node || node->type != HTML_NODE_ELEMENT) return;

    CssAppliedDecl *applied = NULL;
    int count = 0;
    if (!list || list->count == 0 ||
        !layout_collect_matched_declarations(ctx, idx, list, &applied, &count)) {
        applied = NULL;
        count = 0;
    }

    /* Pseudo-element aspect ratios (e.g. .thumbnail:before{padding-top:56.25%})
     * are applied to the real element since we don't render pseudo nodes. */
    if (applied) {
        for (int d = 0; d < count; d++) {
            CssRule *rule = NULL;
            /* Reconstruct rule pointer from declaration to find selector. */
            for (int s = 0; s < list->count && !rule; s++) {
                CssStylesheet *sheet = list->sheets[s];
                for (int r = 0; r < sheet->rule_count; r++) {
                    if (applied[d].decl >= &sheet->rules[r].declarations[0] &&
                        applied[d].decl < &sheet->rules[r].declarations[sheet->rules[r].declaration_count]) {
                        rule = &sheet->rules[r];
                        break;
                    }
                }
            }
            if (rule && rule->selector_text) {
                const char *sel = rule->selector_text;
                const char *pseudo = strstr(sel, ":before");
                if (!pseudo) pseudo = strstr(sel, ":after");
                if (pseudo) {
                    size_t base_len = (size_t)(pseudo - sel);
                    while (base_len > 0 && isspace((unsigned char)sel[base_len - 1])) base_len--;
                    size_t start = 0;
                    while (start < base_len && isspace((unsigned char)sel[start])) start++;
                    if (base_len > start) {
                        char *base = (char*)malloc(base_len - start + 1);
                        if (base) {
                            memcpy(base, sel + start, base_len - start);
                            base[base_len - start] = '\0';
                            if (css_selector_matches(base, ctx->doc, node)) {
                                const char *prop = applied[d].decl->property;
                                const char *val = applied[d].decl->value;
                                if (prop && strcasecmp(prop, "padding-top") == 0) {
                                    double ratio = css_parse_percent_ratio(val);
                                    if (ratio > box->aspect_ratio) box->aspect_ratio = ratio;
                                }
                            }
                            free(base);
                        }
                    }
                }
            }
        }
    }

    /* Generated content: gather ::after string content in cascade order
     * (applied[] is sorted ascending, so the last matching declaration wins,
     * including explicit content:none/"" suppression).  The winning string
     * is injected into the DOM text after the full style pass below. */
    char after_content[256];
    bool after_set = false;
    after_content[0] = '\0';
    if (applied) {
        for (int d = 0; d < count; d++) {
            const char *prop = applied[d].decl->property;
            if (!prop || strcasecmp(prop, "content") != 0) continue;
            CssRule *rule = NULL;
            for (int s = 0; s < list->count && !rule; s++) {
                CssStylesheet *sheet = list->sheets[s];
                for (int r = 0; r < sheet->rule_count; r++) {
                    if (applied[d].decl >= &sheet->rules[r].declarations[0] &&
                        applied[d].decl < &sheet->rules[r].declarations[sheet->rules[r].declaration_count]) {
                        rule = &sheet->rules[r];
                        break;
                    }
                }
            }
            if (!rule || !rule->selector_text) continue;
            /* Only branches ending in ::after contribute, and only when their
             * base selector matches this node (the rule-level match may have
             * come from a different comma branch). */
            bool branch_matched = false;
            const char *bp = rule->selector_text;
            while (*bp && !branch_matched) {
                const char *comma = strchr(bp, ',');
                size_t blen = comma ? (size_t)(comma - bp) : strlen(bp);
                while (blen > 0 && isspace((unsigned char)*bp)) { bp++; blen--; }
                while (blen > 0 && isspace((unsigned char)bp[blen - 1])) blen--;
                if (blen > 0 &&
                    layout_pseudo_branch_matches(bp, blen, "after", ctx->doc, node))
                    branch_matched = true;
                if (!comma) break;
                bp = comma + 1;
            }
            if (!branch_matched) continue;
            char decoded[256];
            bool is_empty = false;
            if (!css_content_decode_string(applied[d].decl->value, decoded,
                                           sizeof(decoded), &is_empty))
                continue;  /* counters/url()/etc.: unsupported, leave as-is */
            after_set = true;
            if (is_empty) {
                after_content[0] = '\0';
            } else {
                strncpy(after_content, decoded, sizeof(after_content) - 1);
                after_content[sizeof(after_content) - 1] = '\0';
            }
        }
    }

    const CssCustomProps *props = &ctx->custom_props[idx];

    if (applied) {
        bool dbg = getenv("CYBER_DEBUG_STYLE") != NULL;
        bool is_h2 = false;
        if (dbg && node->type == HTML_NODE_ELEMENT) {
            const char *cls = NULL;
            for (HtmlAttribute *a = node->attributes; a; a = a->next)
                if (strcasecmp(a->name, "class") == 0) { cls = a->value; break; }
            is_h2 = cls && strstr(cls, "mp-h2") != NULL;
        }
        for (int d = 0; d < count; d++) {
            const char *prop = applied[d].decl->property;
            if (prop && prop[0] == '-' && prop[1] == '-') continue; /* custom props handled separately */
            if (is_h2 && (strncasecmp(prop, "margin", 6) == 0 ||
                          strncasecmp(prop, "padding", 7) == 0 ||
                          strcasecmp(prop, "font-size") == 0)) {
                fprintf(stderr, "[STYLE] %s: %s  (pre fs=%.2f, spec=%d, ord=%d)\n",
                        prop, applied[d].decl->value, box->font_size,
                        applied[d].specificity, applied[d].order);
            }
            char *resolved = css_var_resolve(applied[d].decl->value, props);
            CssDeclaration decl = *applied[d].decl;
            decl.value = resolved ? resolved : applied[d].decl->value;
            layout_apply_declaration(box, &decl,
                                     ctx->viewport_width, ctx->viewport_width,
                                     ctx->base_url);
            free(resolved);
        }
        free(applied);
    }

    /* Inline styles override stylesheet rules. */
    for (HtmlAttribute *attr = node->attributes; attr; attr = attr->next) {
        if (strcasecmp(attr->name, "style") != 0) continue;
        int ic = 0;
        CssDeclaration *idecls = css_parse_inline_style(attr->value, &ic);
        for (int i = 0; i < ic; i++) {
            const char *prop = idecls[i].property;
            if (prop && prop[0] == '-' && prop[1] == '-') continue;
            char *resolved = css_var_resolve(idecls[i].value, props);
            CssDeclaration decl = idecls[i];
            decl.value = resolved ? resolved : idecls[i].value;
            layout_apply_declaration(box, &decl,
                                     ctx->viewport_width, ctx->viewport_width,
                                     ctx->base_url);
            free(resolved);
        }
        css_declarations_free(idecls, ic);
    }

    /* UA rule: metadata/head/style/script/link/meta/title/base/template/noscript
     * are always display:none, regardless of any page CSS. */
    if (layout_default_display(node->tag_name) == CSS_DISPLAY_NONE) {
        box->display = CSS_DISPLAY_NONE;
    }

    /* Realize the winning ::after string (e.g. .hlist li::after " · "). */
    if (after_set && after_content[0] != '\0')
        layout_inject_generated_after(ctx, idx, after_content);
}

typedef struct StyleApplyChunk {
    LayoutContext *ctx;
    LayoutStyleSheetList *list;
    int start;
    int end;
} StyleApplyChunk;

static void layout_apply_stylesheet_job(void *arg)
{
    StyleApplyChunk *chunk = (StyleApplyChunk*)arg;
    for (int i = chunk->start; i < chunk->end; i++) {
        layout_apply_stylesheet_node(chunk->ctx, i, chunk->list);
    }
    free(chunk);
}

static bool layout_apply_stylesheets_parallel(LayoutContext *ctx, LayoutStyleSheetList *list)
{
    int n = ctx->tree.count;
    if (n == 0) return true;

    uint32_t thread_count = gc_thread_pool_get_thread_count();
    if (thread_count < 1) thread_count = 1;
    int num_jobs = (int)thread_count;
    if (num_jobs > n) num_jobs = n;

    int chunk = n / num_jobs;
    int remainder = n % num_jobs;
    int start = 0;
    for (int j = 0; j < num_jobs; j++) {
        int end = start + chunk + (j < remainder ? 1 : 0);
        if (end <= start) continue;
        StyleApplyChunk *c = (StyleApplyChunk*)malloc(sizeof(StyleApplyChunk));
        if (!c) return false;
        c->ctx = ctx;
        c->list = list;
        c->start = start;
        c->end = end;
        if (!gc_thread_pool_submit_job(layout_apply_stylesheet_job, c)) {
            free(c);
            return false;
        }
        start = end;
    }

    gc_thread_pool_wait_empty();
    return true;
}

static void layout_apply_custom_props_inherit(LayoutContext *ctx, int idx)
{
    int parent = ctx->tree.nodes[idx].parent_idx;
    if (parent >= 0) {
        css_custom_props_inherit(&ctx->custom_props[idx], &ctx->custom_props[parent]);
    }
}

static void layout_apply_stylesheet(LayoutContext *ctx, CssStylesheet *sheet)
{
    LayoutStyleSheetList list = {0};
    layout_collect_document_stylesheets(ctx, &list, ctx->base_url);
    if (sheet) layout_sheet_list_add(&list, sheet);

    /* Pass 1: collect custom properties in preorder so inheritance works. */
    if (ctx->tree.count > 0) {
        LOG_INFO("Applying custom properties from %d stylesheet(s) to %d nodes",
                 list.count, ctx->tree.count);
        for (int i = 0; i < ctx->tree.count; i++) {
            int idx = ctx->tree.preorder[i];
            layout_apply_custom_props_inherit(ctx, idx);
            layout_apply_stylesheet_node_custom_props(ctx, idx, &list);
        }
        int total_vars = 0;
        for (int i = 0; i < ctx->tree.count; i++) {
            total_vars += ctx->custom_props[i].count;
        }
        LOG_INFO("Collected %d custom property entries across %d nodes", total_vars, ctx->tree.count);
    }

    /* Pass 2: apply normal declarations in parallel, resolving var() references. */
    LOG_INFO("Applying %d stylesheet(s) to %d layout nodes in parallel", list.count, ctx->tree.count);
    layout_apply_stylesheets_parallel(ctx, &list);

    /* Pass 3 (serial, preorder): resolve parent-relative font-size ratios now
     * that every box's own font-size is final, and inherit `color` /
     * `text-align` / `visibility` / `font-size` from the parent box when the
     * node did not set them explicitly.  Preorder traversal guarantees
     * parents are processed before their children. */
    for (int i = 0; i < ctx->tree.count; i++) {
        int idx = ctx->tree.preorder[i];
        LayoutBox *box = layout_box(ctx, idx);
        int p = ctx->tree.nodes[idx].parent_idx;
        LayoutBox *parent = (p >= 0) ? layout_box(ctx, p) : NULL;
        /* Inherit font-size when the node did not set its own. */
        if (!box->font_size_set && box->font_size_ratio <= 0.0 && parent) {
            box->font_size = parent->font_size;
        }
        if (box->font_size_ratio > 0.0) {
            double base = parent ? parent->font_size : 16.0;
            if (base <= 0.0) base = 16.0;
            box->font_size = base * box->font_size_ratio;
        }
        if (!box->color_set && parent) {
            box->color_r = parent->color_r;
            box->color_g = parent->color_g;
            box->color_b = parent->color_b;
            box->color_a = parent->color_a;
        }
        if (!box->text_align_set && parent) {
            box->text_align = parent->text_align;
        }
        if (!box->visibility_set && parent) {
            box->visibility = parent->visibility;
        }
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
        }
        /* Unitless line-height inherits as a ratio: recompute against the
         * element's own (now final) font-size. */
        if (box->line_height_ratio > 0.0) {
            double fs = box->font_size > 0.0 ? box->font_size : 16.0;
            box->line_height = box->line_height_ratio * fs;
        }
        /* em-unit margins/paddings resolve against the element's own FINAL
         * font-size (CSS 2.1 §8.3/§8.4): re-resolve flagged sides now that
         * font-size inheritance and ratios have settled. */
        if (getenv("CYBER_DISABLE_EM_DEFER") == NULL && box->em_deferred) {
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
    }

    /* Blockification (CSS 2.1 §9.7, Flexbox §4): boxes that float, are
     * absolutely positioned, or are children of a flex/grid container have
     * inline-level display values blockified (inline / inline-block become
     * block).  Preorder traversal: parent display values are already final. */
    for (int i = 0; i < ctx->tree.count; i++) {
        int idx = ctx->tree.preorder[i];
        LayoutBox *box = layout_box(ctx, idx);
        if (box->display != CSS_DISPLAY_INLINE &&
            box->display != CSS_DISPLAY_INLINE_BLOCK) continue;
        int p = ctx->tree.nodes[idx].parent_idx;
        LayoutBox *parent = (p >= 0) ? layout_box(ctx, p) : NULL;
        bool container_item = parent && (parent->display == CSS_DISPLAY_FLEX ||
                                         parent->display == CSS_DISPLAY_GRID);
        if (container_item || box->float_side != 0 ||
            box->position == CSS_POSITION_ABSOLUTE || box->position == CSS_POSITION_FIXED) {
            box->display = CSS_DISPLAY_BLOCK;
        }
    }
    layout_sheet_list_free(&list);
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
static void layout_resolve_used_sizes(LayoutBox *box, HtmlNode *node,
                                      double parent_content_width,
                                      double parent_content_height)
{
    /* Throughout the engine box->width / box->height are the total border-box
     * sizes, and content_width / content_height are derived from them.  This
     * function converts CSS-used values (which may be content-box or border-box
     * depending on box-sizing) into those totals. */

    /* ---------- width ---------- */
    bool width_auto = true;
    double used_content_width = 0.0; /* valid when !width_auto */
    double used_total_width = 0.0;   /* valid when !width_auto */

    if (box->width_percent > 0.0) {
        if (box->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
            used_total_width = parent_content_width * box->width_percent;
        } else {
            used_content_width = parent_content_width * box->width_percent;
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

    if (width_auto) {
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
         * percentage computes to auto (CSS 2.1 §10.5) — clear the markers
         * so downstream auto-height logic (e.g. flex cross sizing) treats
         * the box as height:auto. */
        if (parent_content_height > 0.0) {
            if (box->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
                used_total_height = parent_content_height * box->height_percent;
            } else {
                used_content_height = parent_content_height * box->height_percent;
            }
            height_auto = false;
        } else {
            box->height_percent = 0.0;
            box->height_set = 0;
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
        /* Aspect ratio relates the content box dimensions. */
        double content_width = layout_total_to_content_width(box, box->width);
        used_content_height = content_width * box->aspect_ratio;
        height_auto = false;
    }

    if (height_auto) {
        /* Height stays 0; it will be resolved from children in the bottom-up pass. */
        box->height = 0.0;
    } else if (box->box_sizing == CSS_BOX_SIZING_CONTENT_BOX) {
        box->height = layout_content_to_total_height(box, used_content_height);
    } else {
        /* border-box: used_total_height already holds the total for explicit
         * and percentage sizes; aspect-ratio produced a content height that
         * must still be converted to a total. */
        if (box->aspect_ratio > 0.0 &&
            !(box->height_percent > 0.0) && box->css_height == 0.0) {
            box->height = layout_content_to_total_height(box, used_content_height);
        } else {
            box->height = used_total_height;
        }
    }

    /* min/max-height constrains the same box that height specified. */
    if (box->min_height > 0.0 || box->max_height > 0.0) {
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

/* Line advance honoring an explicit CSS line-height (<=0 means normal). */
static double layout_line_advance_box(const LayoutBox *box)
{
    if (box && box->line_height > 0.0) return box->line_height;
    return layout_line_advance(box ? box->font_size : 16.0);
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
    if (getenv("CYBER_DEBUG_MEASURE")) {
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

/* Find the first <img> descendant and return its width/height attributes.
 * Used to size inline elements (like <a><img></a>) that wrap an image. */
static bool layout_first_img_size(HtmlDocument *doc, HtmlNode *node,
                                  double *out_w, double *out_h)
{
    for (HtmlNode *c = html_node_first_child(doc, node); c;
         c = html_node_next_sibling(doc, c)) {
        if (c->type != HTML_NODE_ELEMENT) continue;
        if (strcasecmp(c->tag_name, "img") == 0) {
            const char *wa = layout_node_attribute(c, "width");
            const char *ha = layout_node_attribute(c, "height");
            double w = (wa && wa[0]) ? atof(wa) : 0.0;
            double h = (ha && ha[0]) ? atof(ha) : 0.0;
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

static double layout_eff_margin_top(LayoutContext *ctx, int idx, int depth)
{
    LayoutBox *box = layout_box(ctx, idx);
    double mt = box->margin_top, mb = box->margin_bottom;
    if (depth > 8) return mt;
    int f, l;
    layout_sig_children(ctx, idx, &f, &l);
    if (f < 0) {
        /* Empty box: margins collapse through only when the box does NOT
         * establish a new block formatting context (CSS 2.1 §8.3.1). */
        if (box->overflow != 0) return mt;
        return mt > mb ? mt : mb;
    }
    if (box->overflow != 0) return mt;    /* BFC: no collapse with children */
    /* Flex/grid containers establish a flex/grid formatting context: their
     * margins never collapse with their items' margins. */
    if (box->display == CSS_DISPLAY_FLEX || box->display == CSS_DISPLAY_GRID) return mt;
    if (box->padding_top > 0.0 || box->border_top > 0.0) return mt;
    if (!layout_is_block_flow(layout_box(ctx, f)->display)) return mt;
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
        if (box->overflow != 0) return mb;
        return mt > mb ? mt : mb;
    }
    if (box->overflow != 0) return mb;    /* BFC: no collapse with children */
    if (box->display == CSS_DISPLAY_FLEX || box->display == CSS_DISPLAY_GRID) return mb;
    if (box->padding_bottom > 0.0 || box->border_bottom > 0.0) return mb;
    if (box->height_set) return mb;               /* explicit height blocks */
    if (!layout_is_block_flow(layout_box(ctx, l)->display)) return mb;
    double c = layout_eff_margin_bottom(ctx, l, depth + 1);
    return mb > c ? mb : c;
}

/* Position in-flow children in a block formatting context and recurse into
 * each.  The box itself is already positioned and width-resolved by the
 * caller; this resolves each child's width/height/position and, after
 * recursing, the parent's auto height. */
static void layout_block_flow(LayoutContext *ctx, int idx)
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

        if (layout_is_block_flow(child->display) && child->float_side != 0) {
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
                if (getenv("CYBER_DEBUG_FLOAT")) {
                    HtmlNode *dn = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
                    fprintf(stderr, "[FLOAT] idx=%d side=%d left=%.0f right=%.0f bottom=%.0f (tag=%s class=%.40s)\n",
                            c, rec.side, rec.left, rec.right, rec.bottom,
                            dn ? dn->tag_name : "?", dn ? layout_node_attribute(dn, "class") : "");
                }
            }
            continue;
        }

        if (layout_is_block_flow(child->display)) {
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
                for (int fi = 0; fi < ctx->float_count; fi++) {
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
                       box->padding_top <= 0.0 && box->border_top <= 0.0) {
                gap = 0.0;
            } else {
                gap = pending_mb > eff_mt ? pending_mb : eff_mt;
            }

            child->x = content_left + child->margin_left;
            child->y = cur.y + gap;
            layout_update_content_sizes(child);

            /* Recurse so the child's subtree (and thus its auto height) is final
             * before we advance the cursor past it. */
            layout_node_serial(ctx, c);

            double eff_mb = layout_eff_margin_bottom(ctx, c, 0);
            if (child->height <= 0.0 && !child->height_set && child->overflow == 0 &&
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
            layout_flow_line_geometry(ctx->float_stack, ctx->float_count, cur.y,
                                      content_left, avail_width,
                                      &line_left, &line_avail);
            /* The pen must never start left of the float-inset line edge:
             * without this clamp the first line of a container (and any run
             * right after a block boundary) is emitted at content_left and
             * overlaps a left float. */
            if (cur.x < line_left) cur.x = line_left;
            HtmlNode *dom = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
            double fs = child->font_size > 0.0 ? child->font_size : 16.0;
            float scale = (float)(fs / 16.0);
            double line_adv = layout_line_advance_box(child);
            const char *text = NULL;
            char concat[8192];
            bool is_text_run = false;
            bool is_whitespace_run = false;
            bool is_img = false;
            bool is_br = false;
            if (dom && dom->type == HTML_NODE_TEXT) {
                if (!layout_text_is_whitespace(dom->text_content)) {
                    text = dom->text_content;
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
                    if (cl > 0 && !layout_text_is_whitespace(concat)) text = concat;
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
                /* Replaced element: honor width/height attributes when
                 * present; the exact intrinsic size is filled in later. */
                const char *wa = layout_node_attribute(dom, "width");
                const char *ha = layout_node_attribute(dom, "height");
                if (wa && wa[0]) child->width = atof(wa);
                if (ha && ha[0]) child->height = atof(ha);
                if (child->width <= 0.0) child->width = 32.0;
                if (child->height <= 0.0) child->height = child->width;
            } else if (text && layout_measure_text_styled(child, text, fs, &tw, NULL)) {
                child->width = tw;
                child->height = line_adv;
            } else if (!is_text_run || text == NULL) {
                /* Element with no text content (e.g. <a><img></a>): size it
                 * from a replaced <img> descendant when present, so captions
                 * and following content don't overlap the image. */
                double iw = 0.0, ih = 0.0;
                bool from_img = dom && layout_first_img_size(ctx->doc, dom, &iw, &ih);
                if (from_img) {
                    child->width = iw;
                    child->height = ih;
                } else if (dom && layout_is_replaced_form_element(dom->tag_name)) {
                    if (child->width <= 0.0) child->width = 80.0;
                    if (child->height <= 0.0) child->height = 20.0;
                } else {
                    /* Empty inline element (anchor span, empty <a>, ...):
                     * shrink to zero instead of a fake box. */
                    if (!child->width_set) child->width = 0.0;
                    if (!child->height_set) child->height = 0.0;
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
            if (getenv("CYBER_DEBUG_INLINE")) {
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
            if (text && span > remaining && span > fs * 2.0) {
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
                     * line.  Browsers split whenever a word fits. */
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
                    for (int fi = 0; fi < ctx->float_count; fi++) {
                        double fb = ctx->float_stack[fi].bottom;
                        if (fb > cur.y + 0.001 && fb < next_fb) next_fb = fb;
                    }
                    if (next_fb < 1e29) {
                        double g2l = content_left, g2a = avail_width;
                        layout_flow_line_geometry(ctx->float_stack,
                                                  ctx->float_count,
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

            if (cur.x + span > line_right && cur.x > line_left + 0.001) {
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
            double h = vmt + child->height + vmb;
            if (h > cur.line_box) cur.line_box = h;

            if (line_members) line_members[line_member_count++] = c;
            layout_node_serial(ctx, c);
        }
    }

    /* Finish the trailing inline line (alignment applies). */
    if (line_member_count > 0) {
        double line_left = content_left, line_avail = avail_width;
        layout_flow_line_geometry(ctx->float_stack, ctx->float_count, cur.y,
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
    if (box->height <= 0.0 && !box->height_set) {
        double max_bottom = cur.y;
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
    }
}

/* ---- Flex layout (single-pass, serial) ---- */

typedef struct {
    int idx;
    double flex_grow, flex_shrink;
    double main_size;   /* border-box basis (flex-basis or css size), pre-grow */
    double cross_size;  /* border-box cross basis */
    double min_main, max_main;
    double main_margin_start, main_margin_end;
    double cross_margin_start, cross_margin_end;
    double final_main, final_cross;
    double final_x, final_y;
} FlexItem;

typedef struct {
    int start, count;
    double cross_size;
} FlexLineSeg;

static void layout_flex_container(LayoutContext *ctx, int idx)
{
    LayoutBox *container = layout_box(ctx, idx);
    LayoutNodeRef *node = layout_node_ref(ctx, idx);
    bool is_row = (container->flex_direction == CSS_FLEX_DIRECTION_ROW ||
                   container->flex_direction == CSS_FLEX_DIRECTION_ROW_REVERSE);
    bool reverse = (container->flex_direction == CSS_FLEX_DIRECTION_ROW_REVERSE ||
                    container->flex_direction == CSS_FLEX_DIRECTION_COLUMN_REVERSE);
    bool do_wrap = (container->flex_wrap == CSS_FLEX_WRAP_WRAP ||
                    container->flex_wrap == CSS_FLEX_WRAP_WRAP_REVERSE);

    int nkids = 0;
    int *kids = layout_collect_flow_children(ctx, idx, &nkids);
    /* Whitespace-only text between flex items creates no flex item (CSS Flexbox
     * §4: a text run that is entirely white space is not rendered).  Without
     * this filter those nodes would each get an auto basis and eat the free
     * space meant for the real items. */
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
    if (nkids == 0) { free(kids); return; }

    FlexItem *items = (FlexItem*)calloc((size_t)nkids, sizeof(FlexItem));
    if (!items) { free(kids); return; }

    double avail_main, avail_cross;
    if (is_row) {
        avail_main = container->content_width;        avail_cross = container->content_height;
    } else {
        avail_main = container->content_height;
        avail_cross = container->content_width;
    }
    bool main_definite = avail_main > 0.0;

    /* Phase 1: measure items, compute main/cross bases. */
    for (int i = 0; i < nkids; i++) {
        int c = kids[i];
        LayoutBox *child = layout_box(ctx, c);
        FlexItem *it = &items[i];
        it->idx = c;
        it->flex_grow = child->flex_grow;
        it->flex_shrink = child->flex_shrink > 0.0 ? child->flex_shrink : 1.0;

        /* Percentage cross/main sizes resolve against the container's
         * content box; when that size is indefinite (auto, still 0 here)
         * the percentage computes to auto (CSS 2.1 §10.5).  Clear the
         * markers so the item sizes from its content below. */
        if (is_row) {
            if (child->height_percent > 0.0 && container->content_height <= 0.0) {
                child->height_percent = 0.0;
                child->height_set = 0;
                child->height = 0.0;
            }
        } else {
            if (child->width_percent > 0.0 && container->content_width <= 0.0) {
                child->width_percent = 0.0;
                child->width_set = 0;
                child->width = 0.0;
            }
        }
        /* Preliminary main size from flex-basis / explicit size / auto.  A
         * percentage flex-basis resolves against the container's main size;
         * so does a percentage width/height used as the automatic basis. */
        double basis = -1.0;
        if (child->flex_basis_percent > 0.0) basis = avail_main * child->flex_basis_percent;
        else if (child->flex_basis >= 0.0) basis = child->flex_basis;
        if (basis < 0.0) {
            if (is_row) {
                if (child->width_percent > 0.0) basis = avail_main * child->width_percent;
                else if (child->css_width > 0.0) basis = child->width;
            } else {
                if (child->height_percent > 0.0) basis = avail_main * child->height_percent;
                else if (child->css_height > 0.0) basis = child->height;
            }
        }
        if (basis < 0.0) {
            /* basis:auto — content size.  Measure the item's text with real
             * glyph metrics so content-sized items (tabs, buttons) fit. */
            HtmlNode *fdom = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
            if (fdom) {
                char fbuf[2048];
                size_t fl = 0;
                fbuf[0] = '\0';
                layout_concat_dom_text(ctx->doc, fdom, fbuf, sizeof(fbuf), &fl);
                if (fl > 0) {
                    double fs2 = child->font_size > 0.0 ? child->font_size : 16.0;
                    double tw = 0.0;
                    if (layout_measure_text_styled(child, fbuf, fs2, &tw, NULL)) basis = tw;
                }
            }
        }
        /* Convert content-box basis to border-box. */
        if (basis >= 0.0) basis = layout_flex_main_total(child, basis, is_row);

        if (is_row) {
            it->main_margin_start = child->margin_left;
            it->main_margin_end = child->margin_right;
            it->cross_margin_start = child->margin_top;
            it->cross_margin_end = child->margin_bottom;
            /* A 0 min/max means "no constraint" and must not pick up the
             * padding/border inset from layout_flex_main_total. */
            it->min_main = child->min_width > 0.0 ? layout_flex_main_total(child, child->min_width, is_row) : 0.0;
            it->max_main = child->max_width > 0.0 ? layout_flex_main_total(child, child->max_width, is_row) : 0.0;
            it->cross_size = child->height;
        } else {
            it->main_margin_start = child->margin_top;
            it->main_margin_end = child->margin_bottom;
            it->cross_margin_start = child->margin_left;
            it->cross_margin_end = child->margin_right;
            it->min_main = child->min_height > 0.0 ? layout_flex_main_total(child, child->min_height, is_row) : 0.0;
            it->max_main = child->max_height > 0.0 ? layout_flex_main_total(child, child->max_height, is_row) : 0.0;
            it->cross_size = child->width;
        }
        it->main_size = basis < 0.0 ? 0.0 : basis;
        /* If no explicit main size and basis auto, fall back to content sizing:
         * give it the container main size so it can shrink, or a small default. */
        if (basis < 0.0 && it->main_size == 0.0) {
            it->main_size = main_definite ? avail_main * 0.25 : 40.0;
        }
        /* Clamp initial basis to min/max. */
        it->main_size = layout_clamp_size(it->main_size, it->min_main, it->max_main);
    }

    /* Phase 2: line breaking. */
    FlexLineSeg *lines = (FlexLineSeg*)calloc((size_t)(nkids + 1), sizeof(FlexLineSeg));
    int line_count = 0;
    {
        int start = 0;
        double used = 0.0;
        for (int i = 0; i < nkids; i++) {
            double item_main = items[i].main_size + items[i].main_margin_start + items[i].main_margin_end;
            if (do_wrap && main_definite && line_count > 0 && start < i &&
                used + item_main > avail_main) {
                lines[line_count].start = start;
                lines[line_count].count = i - start;
                lines[line_count].cross_size = 0.0;
                line_count++;
                start = i;
                used = 0.0;
            }
            used += item_main;
        }
        lines[line_count].start = start;
        lines[line_count].count = nkids - start;
        lines[line_count].cross_size = 0.0;
        line_count++;
    }

    /* Phase 3: main-axis distribution + cross sizing per line. */
    for (int li = 0; li < line_count; li++) {
        FlexLineSeg *line = &lines[li];
        double sum_basis = 0.0, sum_grow = 0.0, sum_wshrink = 0.0, sum_margin = 0.0;
        for (int j = 0; j < line->count; j++) {
            FlexItem *it = &items[line->start + j];
            sum_basis += it->main_size;
            sum_grow += it->flex_grow;
            sum_wshrink += it->flex_shrink * (it->main_size > 0.0 ? it->main_size : 1.0);
            sum_margin += it->main_margin_start + it->main_margin_end;
        }
        double gaps = (line->count > 1) ? (line->count - 1) * container->gap_col : 0.0;
        double free = main_definite ? (avail_main - sum_basis - sum_margin - gaps) : 0.0;

        for (int j = 0; j < line->count; j++) {
            FlexItem *it = &items[line->start + j];
            double main = it->main_size;
            if (main_definite && free > 0.0 && sum_grow > 0.0) {
                main += free * (it->flex_grow / sum_grow);
            } else if (main_definite && free < 0.0 && sum_wshrink > 0.0) {
                double weighted = it->flex_shrink * (it->main_size > 0.0 ? it->main_size : 1.0);
                main += free * (weighted / sum_wshrink);
            }
            main = layout_clamp_size(main, it->min_main, it->max_main);
            if (main < 0.0) main = 0.0;
            it->final_main = main;
            if (getenv("CYBER_DEBUG_FLEX")) {
                LayoutBox *cb = layout_box(ctx, it->idx);
                fprintf(stderr, "[FLEX] idx=%d basis=%.1f grow=%.1f free=%.1f avail=%.1f -> final_main=%.1f (tag=%s)\n",
                        it->idx, it->main_size, it->flex_grow, free, avail_main, main,
                        layout_node_dom(ctx, ctx->tree.nodes[it->idx].dom_node_idx) ?
                        layout_node_dom(ctx, ctx->tree.nodes[it->idx].dom_node_idx)->tag_name : "?");
                (void)cb;
            }

            /* Cross size: use basis, stretch decided in positioning phase. */
            it->final_cross = it->cross_size;
            if (it->final_cross <= 0.0) it->final_cross = is_row ? 20.0 : 80.0;

            /* The line's cross size is the largest item MARGIN box (Flexbox
             * §9.4): cross margins count toward the line and container. */
            double item_cross_total = it->final_cross + it->cross_margin_start + it->cross_margin_end;
            if (item_cross_total > line->cross_size)
                line->cross_size = item_cross_total;
        }
    }

    /* Phase 4: container cross size (if auto) and per-line cross start. */
    double total_cross = 0.0;
    for (int li = 0; li < line_count; li++) total_cross += lines[li].cross_size;
    if (line_count > 1) total_cross += (line_count - 1) * container->gap_row;

    bool cross_auto;
    if (is_row) cross_auto = (container->height <= 0.0 && !container->height_set);
    else        cross_auto = (container->width <= 0.0 && !container->width_set);
    double cross_avail;
    if (cross_auto) {
        cross_avail = total_cross;
        if (is_row) {
            container->height = total_cross + container->padding_top + container->padding_bottom
                                + container->border_top + container->border_bottom;
        } else {
            container->width = total_cross + container->padding_left + container->padding_right
                               + container->border_left + container->border_right;
        }
        layout_update_content_sizes(container);
    } else {
        cross_avail = is_row ? container->content_height : container->content_width;
    }

    /* Phase 5: position items. */
    double content_main_start = is_row
        ? (container->x + container->padding_left + container->border_left)
        : (container->y + container->padding_top + container->border_top);
    double content_cross_start = is_row
        ? (container->y + container->padding_top + container->border_top)
        : (container->x + container->padding_left + container->border_left);

    double cross_cursor = content_cross_start;
    for (int li = 0; li < line_count; li++) {
        FlexLineSeg *line = &lines[li];
        double line_cross_start = cross_cursor;
        double line_cross = lines[li].cross_size;

        /* justify-content: main start offset + extra gap. */
        double line_main_used = 0.0;
        for (int j = 0; j < line->count; j++) {
            FlexItem *it = &items[line->start + j];
            line_main_used += it->final_main + it->main_margin_start + it->main_margin_end;
        }
        double line_gaps = (line->count > 1) ? (line->count - 1) * container->gap_col : 0.0;
        double line_free = main_definite ? (avail_main - line_main_used - line_gaps) : 0.0;
        double extra_gap = 0.0, main_pos = content_main_start;
        if (main_definite && line_free > 0.0) {
            switch (container->justify_content) {
                case CSS_JUSTIFY_CENTER:
                    main_pos += line_free / 2.0; break;
                case CSS_JUSTIFY_FLEX_END:
                    main_pos += line_free; break;
                case CSS_JUSTIFY_SPACE_BETWEEN:
                    extra_gap = (line->count > 1) ? line_free / (line->count - 1) : 0.0; break;
                case CSS_JUSTIFY_SPACE_AROUND:
                    extra_gap = (line->count > 0) ? line_free / line->count : 0.0;
                    main_pos += extra_gap / 2.0; break;
                case CSS_JUSTIFY_SPACE_EVENLY:
                    extra_gap = line_free / (line->count + 1);
                    main_pos += extra_gap; break;
                default: break; /* flex-start */
            }
        }

        for (int jstep = 0; jstep < line->count; jstep++) {
            int j = reverse ? (line->start + line->count - 1 - jstep) : (line->start + jstep);
            FlexItem *it = &items[j];
            LayoutBox *child = layout_box(ctx, it->idx);

            /* Cross placement (align-items).  line_cross is the margin-box
             * line cross size, so margins are subtracted before aligning and
             * added back as the final offset.  Items whose cross size is not
             * definite (content-sized placeholder) align to flex-start: their
             * real cross extent is unknown until the subtree is laid out, and
             * the container's auto cross post-pass grows around them. */
            bool cross_is_definite = (it->cross_size > 0.0);
            double item_cross = it->final_cross;
            double cross_offset;
            if (!cross_is_definite && container->align_items != CSS_ALIGN_STRETCH) {
                cross_offset = 0.0;
            } else switch (container->align_items) {
                case CSS_ALIGN_STRETCH:
                    item_cross = line_cross - it->cross_margin_start - it->cross_margin_end;
                    if (item_cross < 0.0) item_cross = 0.0;
                    cross_offset = 0.0;
                    break;
                case CSS_ALIGN_CENTER:
                    cross_offset = (line_cross - item_cross
                                    - it->cross_margin_start - it->cross_margin_end) / 2.0;
                    if (cross_offset < 0.0) cross_offset = 0.0;
                    break;
                case CSS_ALIGN_FLEX_END:
                    cross_offset = line_cross - item_cross - it->cross_margin_end;
                    if (cross_offset < 0.0) cross_offset = 0.0;
                    break;
                default:
                    cross_offset = 0.0; /* flex-start */
                    break;
            }
            cross_offset += it->cross_margin_start;

            /* Write final main/cross back to the box's width/height.  When the
             * item's cross size is auto (no definite cross basis), leave it
             * at 0 so the subtree's own flow resolves its auto height/width
             * from its children. */
            if (is_row) {
                child->width = it->final_main;
                if (cross_is_definite) child->height = item_cross;
                child->x = main_pos + it->main_margin_start;
                child->y = line_cross_start + cross_offset;
            } else {
                child->height = it->final_main;
                if (cross_is_definite) child->width = item_cross;
                child->y = main_pos + it->main_margin_start;
                child->x = line_cross_start + cross_offset;
            }
            layout_update_content_sizes(child);

            /* Recurse into the item so its subtree is laid out with the final
             * main/cross size, then it does not need a second pass. */
            layout_node_serial(ctx, it->idx);

            main_pos += it->final_main + it->main_margin_start + it->main_margin_end + container->gap_col + extra_gap;
        }

        cross_cursor += line_cross + container->gap_row;
    }

    /* Derive the container's auto cross size from the items' actual extents:
     * their subtrees have final sizes only after the recursion above, so the
     * pre-layout line cross sizes were just estimates. */
    if (is_row && cross_auto) {
        double max_bottom = content_cross_start;
        for (int i = 0; i < nkids; i++) {
            LayoutBox *child = layout_box(ctx, items[i].idx);
            double bottom = child->y + child->height + child->margin_bottom;
            if (bottom > max_bottom) max_bottom = bottom;
        }
        container->height = max_bottom - container->y + container->padding_bottom + container->border_bottom;
        layout_update_content_sizes(container);
    } else if (!is_row && cross_auto) {
        double max_right = content_cross_start;
        for (int i = 0; i < nkids; i++) {
            LayoutBox *child = layout_box(ctx, items[i].idx);
            double right = child->x + child->width + child->margin_right;
            if (right > max_right) max_right = right;
        }
        container->width = max_right - container->x + container->padding_right + container->border_right;
        layout_update_content_sizes(container);
    }

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
            w = inner_w * cell->width_percent;
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
        layout_node_serial(ctx, c);
        if (cell->height > row_h) row_h = cell->height;
        cx += w;
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
                    if (layout_measure_text_styled(child, dom->text_content, fs, &tw, NULL)) {
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
                    const char *wa = layout_node_attribute(dom, "width");
                    const char *ha = layout_node_attribute(dom, "height");
                    if (wa && wa[0]) child->width = atof(wa);
                    if (ha && ha[0]) child->height = atof(ha);
                    if (child->width > 0.0) {
                        if (child->height <= 0.0) child->height = child->width;
                        sized = true;
                    }
                } else if (dom && layout_first_img_size(ctx->doc, dom, &iw, &ih)) {
                    child->width = iw;
                    child->height = ih;
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
            pos == CSS_POSITION_STICKY) {
            return p;
        }
        p = ctx->tree.nodes[p].parent_idx;
    }
    return -1;
}

/* Position an absolutely positioned box against its containing block.
 * This runs after the normal flow passes so containing-block sizes are final. */
static void layout_position_absolute_box(LayoutContext *ctx, int idx)
{
    LayoutBox *box = layout_box(ctx, idx);

    double cb_x, cb_y, cb_w, cb_h;
    int anc = layout_positioned_ancestor(ctx, idx);
    if (anc >= 0) {
        LayoutBox *a = layout_box(ctx, anc);
        cb_x = a->x + a->padding_left + a->border_left;
        cb_y = a->y + a->padding_top + a->border_top;
        cb_w = a->content_width;
        cb_h = a->content_height;
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
    double old_x = box->x;
    double old_y = box->y;

    /* Resolve the box's own used sizes against the containing block, then fill
     * auto width from the available CB width (or left+right constraints). */
    layout_resolve_used_sizes(box, layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx),
                              cb_w, cb_h);
    if (box->width == 0.0 && has_left && has_right) {
        double w = cb_w - box->left - box->right
                   - box->margin_left - box->margin_right;
        if (w < 0.0) w = 0.0;
        box->width = w;
        layout_update_content_sizes(box);
    } else if (box->width == 0.0) {
        /* Auto width with no opposing offsets: fill the containing block. */
        box->width = cb_w - box->margin_left - box->margin_right;
        if (box->width < 0.0) box->width = 0.0;
        layout_update_content_sizes(box);
    }
    if (box->height == 0.0 && has_top && has_bottom) {
        double h = cb_h - box->top - box->bottom
                   - box->margin_top - box->margin_bottom;
        if (h < 0.0) h = 0.0;
        box->height = h;
        layout_update_content_sizes(box);
    }

    /* Compute x offset. */
    if (has_left) {
        box->x = cb_x + box->left + box->margin_left;
    } else if (has_right) {
        box->x = cb_x + cb_w - box->right - box->margin_right - box->width;
    } else {
        box->x = cb_x + box->margin_left;
    }

    /* Compute y offset. */
    if (has_top) {
        box->y = cb_y + box->top + box->margin_top;
    } else if (has_bottom) {
        box->y = cb_y + cb_h - box->bottom - box->margin_bottom - box->height;
    } else {
        box->y = cb_y + box->margin_top;
    }

    /* Lay out the absolute box's subtree now that it has a final position and
     * size; absolute/fixed boxes were skipped by the normal flow pass. */
    layout_node_serial(ctx, idx);

    /* Offset descendants so the absolute box acts as a containing block. */
    layout_offset_subtree(ctx, idx, box->x - old_x, box->y - old_y);
}

static void layout_position_absolute_subtree(LayoutContext *ctx, int idx)
{
    LayoutBox *box = layout_box(ctx, idx);
    if (box->position == CSS_POSITION_ABSOLUTE) {
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
    if (!ctx->js_ctx.valid()) return;
    JSContextHandle js = ctx->js_ctx;
    char buf[128];

    for (int i = 0; i < ctx->tree.count; i++) {
        HtmlNode *node = layout_node_dom(ctx, ctx->tree.nodes[i].dom_node_idx);
        if (!node || node->type != HTML_NODE_ELEMENT || !node->has_js_object) continue;

        GCValue element = node->js_object;
        DOMNodeHandle dom_node = DOMNodeHandle::from_object(element);
        if (!dom_node.valid()) continue;

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
            CssCustomProps *cp = &ctx->custom_props[i];
            for (int j = 0; j < cp->count; j++) {
                SET_VAL(cp->props[j].name, cp->props[j].value);
            }
        }

#undef SET_PX
#undef SET_VAL
    }
}

bool css_layout_document(LayoutContext *ctx, CssStylesheet *sheet)
{
    if (!ctx || ctx->tree.count == 0) return false;

    /* Cascade: match selectors, resolve custom properties, apply declarations. */
    layout_apply_stylesheet(ctx, sheet);

    /* Size and position the root box to the viewport. */
    layout_root(ctx);

    /* Single recursive serial pass: for each container, position and size its
     * in-flow children, recurse, then resolve auto/percentage height from the
     * now-final children.  No per-node atomics or thread-pool dispatch. */
    if (ctx->tree.root_idx >= 0) {
        layout_node_serial(ctx, ctx->tree.root_idx);
    }

    /* Position absolutely/fixed positioned boxes against their containing
     * blocks now that all sizes are final. */
    if (ctx->tree.count > 0) {
        layout_position_absolute_subtree(ctx, ctx->tree.root_idx);
    }

    for (int i = 0; i < ctx->tree.count; i++) {
        ctx->boxes[i].flags |= LAYOUT_HAS_LAYOUT;
    }

    layout_export_computed_geometry(ctx);

    layout_dump_boxes(ctx, "layout_dump.txt");
    return true;
}

bool css_layout_run(LayoutContext *ctx, HtmlDocument *doc, CssStylesheet *sheet,
                    double viewport_width, double viewport_height)
{
    JSContextHandle saved_js_ctx = ctx->js_ctx;
    if (!css_layout_tree_build(ctx, doc)) return false;
    ctx->js_ctx = saved_js_ctx;
    ctx->viewport_width = viewport_width;
    ctx->viewport_height = viewport_height;
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
            memcpy(ctx->base_url, src, origin_len);
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

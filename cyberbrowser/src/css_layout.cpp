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
        box->box_sizing = CSS_BOX_SIZING_CONTENT_BOX;
        box->flex_direction = CSS_FLEX_DIRECTION_ROW;
        box->flex_wrap = CSS_FLEX_WRAP_NOWRAP;
        box->justify_content = CSS_JUSTIFY_FLEX_START;
        box->align_items = CSS_ALIGN_STRETCH;
        box->font_size = 16.0;
        box->font_size_ratio = 0.0;
        box->font_family[0] = '\0';
        box->background_image_url[0] = '\0';
        box->color_r = 0.0;
        box->color_g = 0.0;
        box->color_b = 0.0;
        box->color_a = 1.0;
        box->flex_basis = -1.0;
        box->flex_grow = 0.0;
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

static double css_parse_length(const char *value, double parent_value, double viewport_value);

/* Minimal calc() expression evaluator. Supports +, -, *, /, parentheses and
 * length/percentage operands.  Returns 0 for unsupported expressions. */
typedef struct {
    const char *s;
    size_t len;
    size_t pos;
    double parent_value;
    double viewport_value;
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
        if (strcmp(unit, "em") == 0) return num * p->parent_value;
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

static double css_parse_calc(const char *value, double parent_value, double viewport_value)
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
    double result = calc_parse_expr(&p);
    calc_skip_space(&p);
    if (p.pos < p.len && p.s[p.pos] == ')') p.pos++;
    return result;
}

static double css_parse_length(const char *value, double parent_value, double viewport_value)
{
    if (!value || !*value) return 0.0;
    while (*value && isspace((unsigned char)*value)) value++;
    if (strncasecmp(value, "calc(", 5) == 0) {
        return css_parse_calc(value, parent_value, viewport_value);
    }
    char *end = NULL;
    double num = strtod(value, &end);
    if (end == value) return 0.0;

    while (*end && isspace((unsigned char)*end)) end++;
    if (strcasecmp(end, "px") == 0 || *end == '\0') return num;
    if (strcasecmp(end, "%") == 0) return num * parent_value / 100.0;
    if (strcasecmp(end, "vw") == 0) return num * viewport_value / 100.0;
    if (strcasecmp(end, "vh") == 0) return num * viewport_value / 100.0;
    if (strcasecmp(end, "em") == 0) return num * parent_value;
    if (strcasecmp(end, "rem") == 0) return num * 16.0; /* root em fallback */
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

static void layout_apply_shorthand_sides(LayoutBox *box, const char *value,
                                          double parent_width, double viewport_width,
                                          double *top, double *right, double *bottom, double *left)
{
    double values[4];
    char *copy = strdup(value);
    char *save = NULL;
    char *tok = strtok_r(copy, " \t", &save);
    int count = 0;
    while (tok && count < 4) {
        values[count++] = css_parse_length(tok, parent_width, viewport_width);
        tok = strtok_r(NULL, " \t", &save);
    }
    free(copy);

    if (count == 1) {
        *top = *right = *bottom = *left = values[0];
    } else if (count == 2) {
        *top = *bottom = values[0];
        *right = *left = values[1];
    } else if (count == 3) {
        *top = values[0];
        *right = *left = values[1];
        *bottom = values[2];
    } else if (count >= 4) {
        *top = values[0];
        *right = values[1];
        *bottom = values[2];
        *left = values[3];
    }
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

static bool css_parse_auto_length(const char *value, double parent_value, double viewport_value, double *out)
{
    if (!value || !*value) return false;
    if (strcasecmp(value, "auto") == 0) return false;
    *out = css_parse_length(value, parent_value, viewport_value);
    return true;
}

static void css_parse_shorthand_1_or_2(const char *value, double parent_value, double viewport_value,
                                       double *out1, double *out2)
{
    *out1 = *out2 = 0.0;
    if (!value || !*value) return;
    char *copy = strdup(value);
    char *save = NULL;
    char *tok = strtok_r(copy, " \t", &save);
    if (tok) {
        *out1 = css_parse_length(tok, parent_value, viewport_value);
        tok = strtok_r(NULL, " \t", &save);
        if (tok) {
            *out2 = css_parse_length(tok, parent_value, viewport_value);
        } else {
            *out2 = *out1;
        }
    }
    free(copy);
}

/* Parse the flex shorthand: none | [ <'flex-grow'> <'flex-shrink'>? || <'flex-basis'> ] */
static void css_parse_flex_shorthand(const char *value, double *grow, double *shrink, double *basis)
{
    *grow = 1.0; *shrink = 1.0; *basis = 0.0;
    if (!value || !*value) return;
    if (strcasecmp(value, "none") == 0) { *grow = 0.0; *shrink = 0.0; *basis = -1.0; return; }
    char *copy = strdup(value);
    char *save = NULL;
    char *tok = strtok_r(copy, " \t", &save);
    int idx = 0;
    while (tok) {
        if (idx == 0) {
            char *end = NULL;
            double num = strtod(tok, &end);
            if (end != tok) { *grow = num; }
            else { *basis = css_parse_length(tok, 0, 0); }
        } else if (idx == 1) {
            char *end = NULL;
            double num = strtod(tok, &end);
            if (end != tok) { *shrink = num; }
            else { *basis = css_parse_length(tok, 0, 0); }
        } else {
            *basis = css_parse_length(tok, 0, 0);
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
    } else if (strcasecmp(prop, "position") == 0) {
        box->position = css_parse_position(value);
    } else if (strcasecmp(prop, "box-sizing") == 0) {
        box->box_sizing = css_parse_box_sizing(value);
    } else if (strcasecmp(prop, "width") == 0) {
        if (css_value_is_percent(value)) {
            box->width_percent = css_parse_percent_ratio(value);
            box->css_width = 0.0;
        } else {
            box->css_width = css_parse_length(value, parent_width, viewport_width);
            box->width_percent = 0.0;
        }
    } else if (strcasecmp(prop, "height") == 0) {
        if (css_value_is_percent(value)) {
            box->height_percent = css_parse_percent_ratio(value);
            box->css_height = 0.0;
        } else {
            box->css_height = css_parse_length(value, parent_width, viewport_width);
            box->height_percent = 0.0;
        }
    } else if (strcasecmp(prop, "min-width") == 0) {
        box->min_width = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "max-width") == 0) {
        box->max_width = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "min-height") == 0) {
        box->min_height = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "max-height") == 0) {
        box->max_height = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "top") == 0) {
        box->top = css_parse_length(value, parent_width, viewport_width);
        box->positioned_sides |= LAYOUT_SIDE_TOP;
    } else if (strcasecmp(prop, "left") == 0) {
        box->left = css_parse_length(value, parent_width, viewport_width);
        box->positioned_sides |= LAYOUT_SIDE_LEFT;
    } else if (strcasecmp(prop, "right") == 0) {
        box->right = css_parse_length(value, parent_width, viewport_width);
        box->positioned_sides |= LAYOUT_SIDE_RIGHT;
    } else if (strcasecmp(prop, "bottom") == 0) {
        box->bottom = css_parse_length(value, parent_width, viewport_width);
        box->positioned_sides |= LAYOUT_SIDE_BOTTOM;
    } else if (strcasecmp(prop, "margin") == 0) {
        layout_apply_shorthand_sides(box, value, parent_width, viewport_width,
                                     &box->margin_top, &box->margin_right,
                                     &box->margin_bottom, &box->margin_left);
    } else if (strcasecmp(prop, "margin-left") == 0) {
        box->margin_left = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "margin-right") == 0) {
        box->margin_right = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "margin-top") == 0) {
        box->margin_top = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "margin-bottom") == 0) {
        box->margin_bottom = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "padding") == 0) {
        layout_apply_shorthand_sides(box, value, parent_width, viewport_width,
                                     &box->padding_top, &box->padding_right,
                                     &box->padding_bottom, &box->padding_left);
        box->aspect_ratio = css_parse_percent_ratio(value);
    } else if (strcasecmp(prop, "padding-left") == 0) {
        box->padding_left = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "padding-right") == 0) {
        box->padding_right = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "padding-top") == 0) {
        box->padding_top = css_parse_length(value, parent_width, viewport_width);
        box->aspect_ratio = css_parse_percent_ratio(value);
    } else if (strcasecmp(prop, "padding-bottom") == 0) {
        box->padding_bottom = css_parse_length(value, parent_width, viewport_width);
        box->aspect_ratio = css_parse_percent_ratio(value);
    } else if (strcasecmp(prop, "border-left-width") == 0) {
        box->border_left = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "border-right-width") == 0) {
        box->border_right = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "border-top-width") == 0) {
        box->border_top = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "border-bottom-width") == 0) {
        box->border_bottom = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "flex-direction") == 0) {
        box->flex_direction = css_parse_flex_direction(value);
    } else if (strcasecmp(prop, "flex-wrap") == 0) {
        box->flex_wrap = css_parse_flex_wrap(value);
    } else if (strcasecmp(prop, "justify-content") == 0) {
        box->justify_content = css_parse_justify_content(value);
    } else if (strcasecmp(prop, "align-items") == 0) {
        box->align_items = css_parse_align_items(value);
    } else if (strcasecmp(prop, "flex-basis") == 0) {
        if (strcasecmp(value, "auto") == 0) box->flex_basis = -1.0;
        else box->flex_basis = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "flex-grow") == 0) {
        box->flex_grow = strtod(value, NULL);
    } else if (strcasecmp(prop, "flex-shrink") == 0) {
        box->flex_shrink = strtod(value, NULL);
    } else if (strcasecmp(prop, "flex") == 0) {
        css_parse_flex_shorthand(value, &box->flex_grow, &box->flex_shrink, &box->flex_basis);
    } else if (strcasecmp(prop, "gap") == 0) {
        css_parse_shorthand_1_or_2(value, parent_width, viewport_width,
                                   &box->gap_row, &box->gap_col);
    } else if (strcasecmp(prop, "row-gap") == 0) {
        box->gap_row = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "column-gap") == 0) {
        box->gap_col = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "color") == 0) {
        css_parse_color(value, &box->color_r, &box->color_g, &box->color_b, &box->color_a);
    } else if (strcasecmp(prop, "background-color") == 0) {
        css_parse_color(value, &box->background_color_r, &box->background_color_g,
                        &box->background_color_b, &box->background_color_a);
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
        /* Percentage and em font-sizes are relative to the PARENT's computed
         * font-size, not to the containing-block width.  Record the ratio and
         * resolve it in a serial preorder pass after the (parallel) apply. */
        if (css_value_is_percent(value)) {
            box->font_size_ratio = css_parse_percent_ratio(value);
            box->font_size = 16.0 * box->font_size_ratio; /* provisional */
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
                box->font_size = css_parse_length(value, parent_width, viewport_width);
            }
        }
        if (box->font_size <= 0.0) box->font_size = 16.0;
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

static const char *g_ua_stylesheet_css =
    "head, meta, title, link, style, script, base, template, noscript { display: none; }\n";

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
            if (!css_rule_media_matches(rule, ctx->viewport_width)) continue;
            if (!css_rule_matches(rule, ctx->doc, node)) continue;
            int spec = rule->specificity;
            if (spec == 0) spec = css_specificity_from_selector_text(rule->selector_text);

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
                char *resolved = css_var_resolve(idecls[i].value, props);
                css_custom_props_set(props, prop, resolved ? resolved : idecls[i].value);
                free(resolved);
            }
        }
        css_declarations_free(idecls, ic);
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

    const CssCustomProps *props = &ctx->custom_props[idx];

    if (applied) {
        for (int d = 0; d < count; d++) {
            const char *prop = applied[d].decl->property;
            if (prop && prop[0] == '-' && prop[1] == '-') continue; /* custom props handled separately */
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
     * that every box's own font-size is final. */
    for (int i = 0; i < ctx->tree.count; i++) {
        int idx = ctx->tree.preorder[i];
        LayoutBox *box = layout_box(ctx, idx);
        if (box->font_size_ratio > 0.0) {
            int p = ctx->tree.nodes[idx].parent_idx;
            double base = (p >= 0) ? layout_box(ctx, p)->font_size : 16.0;
            if (base <= 0.0) base = 16.0;
            box->font_size = base * box->font_size_ratio;
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
    } else if (box->css_width > 0.0) {
        if (box->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
            used_total_width = box->css_width;
        } else {
            used_content_width = box->css_width;
        }
        width_auto = false;
    }

    if (width_auto) {
        /* Auto width fills the containing block content width; that is the
         * total border-box width before margins are applied. */
        used_total_width = parent_content_width - box->margin_left - box->margin_right;
        if (used_total_width < 0.0) used_total_width = 0.0;
    } else if (box->box_sizing == CSS_BOX_SIZING_CONTENT_BOX) {
        used_total_width = layout_content_to_total_width(box, used_content_width);
    }

    /* min/max-width constrains the same box that width specified. */
    if (box->min_width > 0.0 || box->max_width > 0.0) {
        if (box->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
            used_total_width = layout_clamp_size(used_total_width,
                                                  box->min_width, box->max_width);
        } else {
            double cw = layout_clamp_size(used_content_width,
                                          box->min_width, box->max_width);
            used_total_width = layout_content_to_total_width(box, cw);
        }
    }
    box->width = used_total_width;

    /* ---------- height ---------- */
    bool height_auto = true;
    double used_content_height = 0.0;
    double used_total_height = 0.0;

    if (box->height_percent > 0.0) {
        /* Percentage height resolves against the containing block height. */
        if (box->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
            used_total_height = parent_content_height * box->height_percent;
        } else {
            used_content_height = parent_content_height * box->height_percent;
        }
        height_auto = false;
    } else if (box->css_height > 0.0) {
        if (box->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
            used_total_height = box->css_height;
        } else {
            used_content_height = box->css_height;
        }
        height_auto = false;
    } else if (box->aspect_ratio > 0.0) {
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

    for (int i = 0; i < nkids; i++) {
        int c = kids[i];
        LayoutBox *child = layout_box(ctx, c);

        /* Resolve used sizes against this container's content box. */
        layout_resolve_used_sizes(child, layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx),
                                  avail_width, box->content_height);

        if (layout_is_block_flow(child->display)) {
            /* Block child: break to a new line and stack vertically. */
            cur.y += cur.line_box;          /* drop any pending inline-line height */
            cur.line_box = 0.0;
            cur.x = content_left;

            child->x = content_left + child->margin_left;
            child->y = cur.y + child->margin_top;
            layout_update_content_sizes(child);

            /* Recurse so the child's subtree (and thus its auto height) is final
             * before we advance the cursor past it. */
            layout_node_serial(ctx, c);

            cur.y = child->y + child->height + child->margin_bottom;
        } else {
            /* Inline-level child: flow onto the current line, wrap if needed.
             * Real text shaping happens later in the display list; here we only
             * need a plausible box so children of inline-block descendants and
             * line height advance correctly. */
            if (child->width <= 0.0) child->width = child->font_size * 5.0;
            if (child->height <= 0.0) child->height = child->font_size * 1.25;
            if (child->width <= 0.0) child->width = 80.0;
            if (child->height <= 0.0) child->height = 20.0;
            layout_update_content_sizes(child);

            double span = child->margin_left + child->width + child->margin_right;
            if (cur.x + span > content_left + avail_width && cur.x > content_left) {
                cur.y += cur.line_box;
                cur.line_box = 0.0;
                cur.x = content_left;
            }
            child->x = cur.x + child->margin_left;
            child->y = cur.y + child->margin_top;
            cur.x += span;
            double h = child->margin_top + child->height + child->margin_bottom;
            if (h > cur.line_box) cur.line_box = h;

            layout_node_serial(ctx, c);
        }
    }

    cur.y += cur.line_box;  /* finish trailing inline line */

    free(kids);

    /* Resolve auto height from the extent of the children. */
    if (box->height <= 0.0) {
        double max_bottom = content_top;
        for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
             c = ctx->tree.nodes[c].next_sibling_idx) {
            LayoutBox *child = layout_box(ctx, c);
            if (!layout_is_in_flow(child)) continue;
            double bottom = child->y + child->height + child->margin_bottom;
            if (bottom > max_bottom) max_bottom = bottom;
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
    if (nkids == 0) { free(kids); return; }

    FlexItem *items = (FlexItem*)calloc((size_t)nkids, sizeof(FlexItem));
    if (!items) { free(kids); return; }

    double avail_main, avail_cross;
    if (is_row) {
        avail_main = container->content_width;
        avail_cross = container->content_height;
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

        /* Preliminary main size from flex-basis / explicit size / auto. */
        double basis = -1.0;
        if (child->flex_basis >= 0.0) basis = child->flex_basis;
        if (basis < 0.0) {
            if (is_row) basis = (child->width_percent > 0.0 || child->css_width > 0.0)
                                ? child->width : 0.0;
            else        basis = (child->height_percent > 0.0 || child->css_height > 0.0)
                                ? child->height : 0.0;
        }
        /* Convert content-box basis to border-box. */
        if (basis >= 0.0) basis = layout_flex_main_total(child, basis, is_row);

        if (is_row) {
            it->main_margin_start = child->margin_left;
            it->main_margin_end = child->margin_right;
            it->cross_margin_start = child->margin_top;
            it->cross_margin_end = child->margin_bottom;
            it->min_main = layout_flex_main_total(child, child->min_width, is_row);
            it->max_main = layout_flex_main_total(child, child->max_width, is_row);
            it->cross_size = child->height;
        } else {
            it->main_margin_start = child->margin_top;
            it->main_margin_end = child->margin_bottom;
            it->cross_margin_start = child->margin_left;
            it->cross_margin_end = child->margin_right;
            it->min_main = layout_flex_main_total(child, child->min_height, is_row);
            it->max_main = layout_flex_main_total(child, child->max_height, is_row);
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

            /* Cross size: use basis, stretch decided in positioning phase. */
            it->final_cross = it->cross_size;
            if (it->final_cross <= 0.0) it->final_cross = is_row ? 20.0 : 80.0;

            if (it->final_cross > line->cross_size)
                line->cross_size = it->final_cross;
        }
    }

    /* Phase 4: container cross size (if auto) and per-line cross start. */
    double total_cross = 0.0;
    for (int li = 0; li < line_count; li++) total_cross += lines[li].cross_size;
    if (line_count > 1) total_cross += (line_count - 1) * container->gap_row;

    bool cross_auto;
    if (is_row) cross_auto = (container->height <= 0.0);
    else        cross_auto = (container->width <= 0.0);
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

            /* Cross placement (align-items). */
            double item_cross = it->final_cross;
            if (container->align_items == CSS_ALIGN_STRETCH) {
                item_cross = line_cross;
            }
            double cross_offset;
            switch (container->align_items) {
                case CSS_ALIGN_CENTER:
                    cross_offset = (line_cross - item_cross) / 2.0; break;
                case CSS_ALIGN_FLEX_END:
                    cross_offset = line_cross - item_cross; break;
                default:
                    cross_offset = 0.0; break; /* stretch/flex-start */
            }
            cross_offset += it->cross_margin_start;

            /* Write final main/cross back to the box's width/height. */
            if (is_row) {
                child->width = it->final_main;
                child->height = item_cross;
                child->x = main_pos + it->main_margin_start;
                child->y = line_cross_start + cross_offset;
            } else {
                child->height = it->final_main;
                child->width = item_cross;
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

    /* If auto height was not set above (row + content height), derive from lines. */
    if (is_row && container->height <= 0.0) {
        container->height = cross_cursor - container->y + container->padding_bottom + container->border_bottom;
        layout_update_content_sizes(container);
    } else if (!is_row && container->width <= 0.0) {
        container->width = cross_cursor - container->x + container->padding_right + container->border_right;
        layout_update_content_sizes(container);
    }

    free(lines);
    free(items);
    free(kids);
}

/* Position and size one box's subtree, given the box itself is already placed
 * (x/y) and width-resolved by its parent.  Dispatches to the appropriate
 * formatting context, then the auto-height is resolved from children. */
static void layout_node_serial(LayoutContext *ctx, int idx)
{
    LayoutBox *box = layout_box(ctx, idx);

    if (box->display == CSS_DISPLAY_NONE) return;
    if (box->display == CSS_DISPLAY_INLINE) {
        /* Inline boxes don't establish a block formatting context for their
         * block children; recurse minimally so inline-block descendants still
         * get laid out. */
        for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
             c = ctx->tree.nodes[c].next_sibling_idx) {
            LayoutBox *child = layout_box(ctx, c);
            if (child->display == CSS_DISPLAY_NONE) continue;
            layout_node_serial(ctx, c);
        }
        return;
    }

    if (box->display == CSS_DISPLAY_FLEX) {
        layout_flex_container(ctx, idx);
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
    fprintf(fp, "idx\ttag\tid\tclass\tdisplay\tx\ty\twidth\theight\tmargin_t\tmargin_r\tmargin_b\tmargin_l\tpadding_t\tpadding_r\tpadding_b\tpadding_l\tborder_t\tborder_r\tborder_b\tborder_l\tflex_grow\tflex_shrink\tposition\tbox_sizing\tbg\tvisibility\n");
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
        fprintf(fp, "%d\t%s\t%s\t%s\t%s\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%s\t%s\t%s\t%s\n",
                i, tag, id_buf, cls_buf, dname,
                b->x, b->y, b->width, b->height,
                b->margin_top, b->margin_right, b->margin_bottom, b->margin_left,
                b->padding_top, b->padding_right, b->padding_bottom, b->padding_left,
                b->border_top, b->border_right, b->border_bottom, b->border_left,
                b->flex_grow, b->flex_shrink, pname, bsname, bg_buf, vname);
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
    const char *default_base = "https://www.youtube.com/";
    strncpy(ctx->base_url, default_base, sizeof(ctx->base_url) - 1);
    ctx->base_url[sizeof(ctx->base_url) - 1] = '\0';
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

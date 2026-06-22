/*
 * CSS Layout Engine - Implementation
 *
 * Parallel top-down / bottom-up layout as described in PARALLEL_CSS_LAYOUT.md.
 */

#include "css_layout.h"
#include "platform.h"
#include "quickjs_gc_unified.h"
#include "http_download.h"

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

static inline void layout_thread_yield(void) {
#ifdef _WIN32
    Sleep(0);
#else
    sched_yield();
#endif
}

#define LOG_TAG "css_layout"
#define LOG_INFO(...) platform_log(LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)
#define LOG_WARN(...) platform_log(LOG_LEVEL_WARN, LOG_TAG, __VA_ARGS__)
#define LOG_ERROR(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)

/* Thread-pool job argument for a chunk of node indices. */
typedef struct LayoutChunk {
    LayoutContext *ctx;
    const int *order;
    int start;
    int end;
} LayoutChunk;

static inline HtmlNode* layout_node_dom(LayoutContext *ctx, int dom_idx)
{
    return (HtmlNode*)po_array_payload(&ctx->doc->array, dom_idx);
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
        box->flex_direction = CSS_FLEX_DIRECTION_ROW;
        box->flex_wrap = CSS_FLEX_WRAP_NOWRAP;
        box->justify_content = CSS_JUSTIFY_FLEX_START;
        box->align_items = CSS_ALIGN_STRETCH;
        box->font_size = 16.0;
        box->font_family[0] = '\0';
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

    free(map);
    return ok;
}

void css_layout_tree_free(LayoutContext *ctx)
{
    if (!ctx) return;
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

static double css_parse_length(const char *value, double parent_value, double viewport_value)
{
    if (!value || !*value) return 0.0;
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

    if (value[0] == '#') {
        int rr = 0, gg = 0, bb = 0;
        if (sscanf(value + 1, "%02x%02x%02x", &rr, &gg, &bb) == 3) {
            *r = rr / 255.0; *g = gg / 255.0; *b = bb / 255.0;
            return true;
        }
    }

    /* Named colors - tiny subset. */
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
    return display == CSS_DISPLAY_BLOCK ||
           display == CSS_DISPLAY_FLEX ||
           display == CSS_DISPLAY_GRID;
}

static void layout_apply_declaration(LayoutBox *box, const CssDeclaration *decl,
                                     double parent_width, double viewport_width)
{
    const char *prop = decl->property;
    const char *value = decl->value;
    if (!prop || !value) return;

    if (strcasecmp(prop, "display") == 0) {
        box->display = css_parse_display(value);
    } else if (strcasecmp(prop, "visibility") == 0) {
        box->visibility = css_parse_visibility(value);
    } else if (strcasecmp(prop, "width") == 0) {
        box->width = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "height") == 0) {
        box->height = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "min-width") == 0) {
        box->min_width = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "max-width") == 0) {
        box->max_width = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "min-height") == 0) {
        box->min_height = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "max-height") == 0) {
        box->max_height = css_parse_length(value, parent_width, viewport_width);
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
    } else if (strcasecmp(prop, "font-size") == 0) {
        box->font_size = css_parse_length(value, parent_width, viewport_width);
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
                                      double parent_width, double viewport_width)
{
    for (HtmlAttribute *attr = node->attributes; attr; attr = attr->next) {
        if (strcasecmp(attr->name, "style") != 0) continue;
        int count = 0;
        CssDeclaration *decls = css_parse_inline_style(attr->value, &count);
        for (int i = 0; i < count; i++) {
            layout_apply_declaration(box, &decls[i], parent_width, viewport_width);
        }
        css_declarations_free(decls, count);
    }
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
    if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) return strdup(href);
    if (strncmp(href, "//", 2) == 0) {
        char buf[2048];
        snprintf(buf, sizeof(buf), "https:%s", href);
        return strdup(buf);
    }
    if (href[0] == '/') {
        const char *base = base_url && base_url[0] ? base_url : "https://www.youtube.com";
        char buf[2048];
        snprintf(buf, sizeof(buf), "%s%s", base, href);
        return strdup(buf);
    }
    const char *base = base_url && base_url[0] ? base_url : "https://www.youtube.com/";
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
    LOG_INFO("Fetching stylesheet: %.80s", url);

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

static void layout_collect_stylesheets_recursive(LayoutContext *ctx, int layout_idx,
                                                  LayoutStyleSheetList *list,
                                                  const char *base_url) {
    if (layout_idx < 0 || layout_idx >= ctx->tree.count) return;
    HtmlNode *node = layout_node_dom(ctx, ctx->tree.nodes[layout_idx].dom_node_idx);
    if (!node || node->type != HTML_NODE_ELEMENT) goto next;

    if (strcasecmp(node->tag_name, "style") == 0 && node->text_content && node->text_content[0]) {
        CssStylesheet *sheet = css_stylesheet_parse(node->text_content, strlen(node->text_content));
        if (sheet) {
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

static bool layout_collect_document_stylesheets(LayoutContext *ctx, LayoutStyleSheetList *list,
                                                 const char *base_url) {
    if (!ctx || !list || ctx->tree.count == 0) return false;
    int root_dom_idx = ctx->tree.nodes[ctx->tree.root_idx].dom_node_idx;
    layout_collect_stylesheets_recursive(ctx, root_dom_idx, list, base_url);
    return true;
}

/* Collect matching declarations for one node, sort by cascade, and apply. */
static void layout_apply_stylesheet_node(LayoutContext *ctx, int idx,
                                          LayoutStyleSheetList *list)
{
    LayoutBox *box = layout_box(ctx, idx);
    HtmlNode *node = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    if (!node || node->type != HTML_NODE_ELEMENT) return;

    int cap = 64;
    int count = 0;
    CssAppliedDecl *applied = (CssAppliedDecl*)malloc(cap * sizeof(CssAppliedDecl));
    if (!applied) return;

    for (int s = 0; s < list->count; s++) {
        CssStylesheet *sheet = list->sheets[s];
        for (int r = 0; r < sheet->rule_count; r++) {
            CssRule *rule = &sheet->rules[r];
            if (!rule->selector_text || !rule->selector_text[0]) continue;
            if (!css_selector_matches(rule->selector_text, ctx->doc, node)) continue;
            if (!css_rule_media_matches(rule, ctx->viewport_width)) continue;
            int spec = rule->specificity;
            if (spec == 0) spec = css_specificity_from_selector_text(rule->selector_text);

            /* Pseudo-element aspect ratios (e.g. .thumbnail:before{padding-top:56.25%})
             * are applied to the real element since we don't render pseudo nodes. */
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
                            for (int d = 0; d < rule->declaration_count; d++) {
                                const char *prop = rule->declarations[d].property;
                                const char *val = rule->declarations[d].value;
                                if (prop && strcasecmp(prop, "padding-top") == 0) {
                                    double ratio = css_parse_percent_ratio(val);
                                    if (ratio > box->aspect_ratio) box->aspect_ratio = ratio;
                                }
                            }
                        }
                        free(base);
                    }
                }
            }

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
                count++;
            }
        }
    }

    if (count > 0) {
        qsort(applied, (size_t)count, sizeof(CssAppliedDecl), css_applied_decl_compare);
        for (int d = 0; d < count; d++) {
            layout_apply_declaration(box, applied[d].decl,
                                     ctx->viewport_width, ctx->viewport_width);
        }
    }
    free(applied);

    /* Inline styles override stylesheet rules. */
    layout_apply_inline_style(box, node, ctx->viewport_width, ctx->viewport_width);
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
    if (n == 0 || list->count == 0) return true;

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

static void layout_apply_stylesheet(LayoutContext *ctx, CssStylesheet *sheet)
{
    LayoutStyleSheetList list = {0};
    const char *base_url = "https://www.youtube.com/";
    layout_collect_document_stylesheets(ctx, &list, base_url);
    if (sheet) layout_sheet_list_add(&list, sheet);

    if (list.count == 0) {
        /* No stylesheets to apply, but inline styles still need to be resolved. */
        for (int i = 0; i < ctx->tree.count; i++) {
            LayoutBox *box = layout_box(ctx, i);
            HtmlNode *node = layout_node_dom(ctx, ctx->tree.nodes[i].dom_node_idx);
            if (node && node->type == HTML_NODE_ELEMENT) {
                layout_apply_inline_style(box, node, ctx->viewport_width, ctx->viewport_width);
            }
        }
    } else {
        LOG_INFO("Applying %d stylesheet(s) to %d layout nodes in parallel", list.count, ctx->tree.count);
        layout_apply_stylesheets_parallel(ctx, &list);
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

static void layout_resolve_used_sizes(LayoutBox *box, double parent_content_width)
{
    if (box->width == 0) {
        box->width = parent_content_width - box->margin_left - box->margin_right;
        if (box->width < 0) box->width = 0;
    }
    box->width = layout_clamp_size(box->width, box->min_width, box->max_width);

    if (box->height == 0 && box->aspect_ratio > 0) {
        box->height = box->width * box->aspect_ratio;
    }
    if (box->height > 0) {
        box->height = layout_clamp_size(box->height, box->min_height, box->max_height);
    }

    layout_update_content_sizes(box);
}

typedef struct {
    int idx;
    double flex_grow;
    double flex_shrink;
    double main_size;
    double cross_size;
    double main_margin_start, main_margin_end;
    double cross_margin_start, cross_margin_end;
    double min_main, max_main;
    double min_cross, max_cross;
} FlexItemData;

typedef struct {
    int start, count;
    double main_used;
    double cross_size;
} FlexLine;

static void layout_flex_container(LayoutContext *ctx, int idx);
static void layout_offset_children(LayoutContext *ctx, int idx, double dx, double dy);

/* Recursively resolve a subtree under a flex item (or block wrapper) so that
 * its content-based height/width is known before the flex container distributes
 * space.  This is a simple serial pass used only for the flex item itself; the
 * normal parallel top-down/bottom-up passes will skip nodes already marked done. */
static void layout_resolve_subtree(LayoutContext *ctx, int idx, double avail_width)
{
    LayoutBox *box = layout_box(ctx, idx);
    LayoutNodeRef *node = layout_node_ref(ctx, idx);

    if (box->display == CSS_DISPLAY_INLINE) {
        /* Inline boxes are not laid out as blocks; leave them zero-sized so
         * the display list ignores them. */
        atomic_store_u32(&layout_state(ctx, idx)->top_down_done, 1);
        return;
    }

    if (box->display == CSS_DISPLAY_FLEX) {
        if (box->width == 0) {
            box->width = avail_width - box->margin_left - box->margin_right;
            if (box->width < 0) box->width = 0;
        }
        box->width = layout_clamp_size(box->width, box->min_width, box->max_width);
        box->x = 0.0;
        box->y = 0.0;
        layout_update_content_sizes(box);
        atomic_store_u32(&layout_state(ctx, idx)->top_down_done, 1);
        layout_flex_container(ctx, idx);
    } else {
        layout_resolve_used_sizes(box, avail_width);
        box->x = 0.0;
        box->y = 0.0;
        atomic_store_u32(&layout_state(ctx, idx)->top_down_done, 1);

        double content_left = box->padding_left + box->border_left;
        double content_top  = box->padding_top + box->border_top;
        double y_offset = 0.0;

        for (int c = node->first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
            LayoutBox *child = layout_box(ctx, c);
            if (child->display == CSS_DISPLAY_NONE) continue;
            if (child->display == CSS_DISPLAY_INLINE) continue;
            if (child->visibility == CSS_VISIBILITY_HIDDEN) continue;
            layout_resolve_subtree(ctx, c, box->content_width);
            double nx = content_left + child->margin_left;
            double ny = content_top + y_offset + child->margin_top;
            layout_offset_children(ctx, c, nx, ny);
            child->x = nx;
            child->y = ny;
            y_offset = (ny + child->height + child->margin_bottom) - content_top;
        }

        if (box->height == 0) {
            box->height = y_offset + box->padding_top + box->padding_bottom
                          + box->border_top + box->border_bottom;
            layout_update_content_sizes(box);
        }
    }
}

static void layout_offset_children(LayoutContext *ctx, int idx, double dx, double dy)
{
    if (dx == 0.0 && dy == 0.0) return;
    LayoutNodeRef *node = layout_node_ref(ctx, idx);
    for (int c = node->first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        LayoutBox *child = layout_box(ctx, c);
        child->x += dx;
        child->y += dy;
        layout_offset_children(ctx, c, dx, dy);
    }
}

static void layout_flex_container(LayoutContext *ctx, int idx)
{
    LayoutBox *container = layout_box(ctx, idx);
    LayoutNodeRef *node = layout_node_ref(ctx, idx);
    if (node->child_count <= 0) return;

    bool is_row = container->flex_direction == CSS_FLEX_DIRECTION_ROW ||
                  container->flex_direction == CSS_FLEX_DIRECTION_ROW_REVERSE;
    bool reverse = container->flex_direction == CSS_FLEX_DIRECTION_ROW_REVERSE ||
                   container->flex_direction == CSS_FLEX_DIRECTION_COLUMN_REVERSE;
    bool wrap = container->flex_wrap == CSS_FLEX_WRAP_WRAP;
    double gap_main = is_row ? container->gap_col : container->gap_row;
    double gap_cross = is_row ? container->gap_row : container->gap_col;

    double content_left = container->x + container->padding_left + container->border_left;
    double content_top  = container->y + container->padding_top + container->border_top;
    double content_w = container->content_width;
    double content_h = container->content_height;
    if (content_w < 0) content_w = 0;
    if (content_h < 0) content_h = 0;

    double avail_main = is_row ? content_w : content_h;
    double container_cross = is_row ? content_h : content_w;
    bool main_definite = is_row ? (container->width > 0) : (container->height > 0);

    int *children = (int*)malloc(node->child_count * sizeof(int));
    if (!children) return;
    int n = 0;
    for (int c = node->first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        LayoutBox *child = layout_box(ctx, c);
        if (child->display == CSS_DISPLAY_NONE) continue;
        if (child->visibility == CSS_VISIBILITY_HIDDEN) continue;
        children[n++] = c;
    }
    if (n == 0) { free(children); return; }

    /* Phase 0: resolve each child's intrinsic size.  Flex children get their
     * main-axis size from flex-basis/width/height; block children are laid out
     * serially so their content height is known. */
    for (int i = 0; i < n; i++) {
        int c = children[i];
        LayoutBox *child = layout_box(ctx, c);
        double min_main = is_row ? child->min_width : child->min_height;
        double max_main = is_row ? child->max_width : child->max_height;
        double prelim_main = 0.0;
        if (child->flex_basis >= 0.0) {
            prelim_main = child->flex_basis;
        } else if (is_row) {
            if (child->width > 0.0) prelim_main = child->width;
        } else {
            if (child->height > 0.0) prelim_main = child->height;
        }
        prelim_main = layout_clamp_size(prelim_main, min_main, max_main);
        double pass_width = is_row ? prelim_main : content_w;
        if (pass_width < 0.0) pass_width = 0.0;
        layout_resolve_subtree(ctx, c, pass_width);
    }

    FlexItemData *items = (FlexItemData*)calloc(n, sizeof(FlexItemData));
    if (!items) { free(children); return; }

    for (int i = 0; i < n; i++) {
        int c = children[i];
        LayoutBox *child = layout_box(ctx, c);
        FlexItemData *it = &items[i];
        it->idx = c;
        it->flex_grow = child->flex_grow;
        it->flex_shrink = child->flex_shrink;

        if (is_row) {
            it->main_margin_start = child->margin_left;
            it->main_margin_end   = child->margin_right;
            it->cross_margin_start = child->margin_top;
            it->cross_margin_end   = child->margin_bottom;
            it->min_main = child->min_width;  it->max_main = child->max_width;
            it->min_cross = child->min_height; it->max_cross = child->max_height;
        } else {
            it->main_margin_start = child->margin_top;
            it->main_margin_end   = child->margin_bottom;
            it->cross_margin_start = child->margin_left;
            it->cross_margin_end   = child->margin_right;
            it->min_main = child->min_height; it->max_main = child->max_height;
            it->min_cross = child->min_width;  it->max_cross = child->max_width;
        }

        double main = is_row ? child->width : child->height;
        double cross = is_row ? child->height : child->width;
        main = layout_clamp_size(main, it->min_main, it->max_main);
        cross = layout_clamp_size(cross, it->min_cross, it->max_cross);
        it->main_size = main;
        it->cross_size = cross;
    }

    FlexLine *lines = (FlexLine*)malloc(n * sizeof(FlexLine));
    if (!lines) { free(children); free(items); return; }
    int line_count = 0;
    int line_start = 0;
    double line_main = 0.0;
    for (int i = 0; i <= n; i++) {
        if (i == n) {
            lines[line_count].start = line_start;
            lines[line_count].count = i - line_start;
            lines[line_count].main_used = 0.0;
            lines[line_count].cross_size = 0.0;
            line_count++;
            break;
        }
        double item_total = items[i].main_size + items[i].main_margin_start + items[i].main_margin_end;
        double extra = item_total + (line_main > 0.0 ? gap_main : 0.0);
        if (wrap && main_definite && line_main > 0.0 &&
            line_main + extra > avail_main + 1e-6 && i > line_start) {
            lines[line_count].start = line_start;
            lines[line_count].count = i - line_start;
            lines[line_count].main_used = 0.0;
            lines[line_count].cross_size = 0.0;
            line_count++;
            line_start = i;
            line_main = item_total;
        } else {
            line_main += extra;
        }
    }

    for (int li = 0; li < line_count; li++) {
        FlexLine *line = &lines[li];
        int start = line->start;
        int count = line->count;
        double sum_basis = 0.0;
        double sum_grow = 0.0;
        double sum_weighted_shrink = 0.0;
        for (int j = start; j < start + count; j++) {
            sum_basis += items[j].main_size + items[j].main_margin_start + items[j].main_margin_end;
            sum_grow += items[j].flex_grow;
            sum_weighted_shrink += items[j].flex_shrink * items[j].main_size;
        }
        double gaps = (count > 1) ? (count - 1) * gap_main : 0.0;
        double total = sum_basis + gaps;
        double free_main = avail_main - total;

        if (main_definite) {
            if (free_main > 1e-6 && sum_grow > 0.0) {
                for (int j = start; j < start + count; j++) {
                    items[j].main_size += free_main * items[j].flex_grow / sum_grow;
                }
            } else if (free_main < -1e-6 && sum_weighted_shrink > 0.0) {
                double to_shrink = -free_main;
                for (int j = start; j < start + count; j++) {
                    double share = (items[j].flex_shrink * items[j].main_size) / sum_weighted_shrink;
                    items[j].main_size -= to_shrink * share;
                }
            }
        }

        double line_cross = 0.0;
        for (int j = start; j < start + count; j++) {
            items[j].main_size = layout_clamp_size(items[j].main_size,
                                                    items[j].min_main, items[j].max_main);
            double cross = items[j].cross_size;
            if (container->align_items == CSS_ALIGN_STRETCH && container_cross > 0.0 && cross == 0.0) {
                cross = container_cross - items[j].cross_margin_start - items[j].cross_margin_end;
            }
            cross = layout_clamp_size(cross, items[j].min_cross, items[j].max_cross);
            items[j].cross_size = cross;
            double cross_total = cross + items[j].cross_margin_start + items[j].cross_margin_end;
            if (cross_total > line_cross) line_cross = cross_total;
        }
        if (line_cross <= 0.0) line_cross = is_row ? 20.0 : 80.0;
        line->cross_size = line_cross;

        double used = 0.0;
        for (int j = start; j < start + count; j++) {
            used += items[j].main_size + items[j].main_margin_start + items[j].main_margin_end;
        }
        used += gaps;
        line->main_used = used;
    }

    double cross_offset = 0.0;
    for (int li = 0; li < line_count; li++) {
        FlexLine *line = &lines[li];
        int start = line->start;
        int count = line->count;
        double free_main = main_definite ? (avail_main - line->main_used) : 0.0;
        if (free_main < 0.0) free_main = 0.0;

        double main_pos = 0.0;
        double extra_gap = 0.0;
        double initial_gap = 0.0;
        switch (container->justify_content) {
            case CSS_JUSTIFY_FLEX_START: main_pos = 0.0; break;
            case CSS_JUSTIFY_FLEX_END:   main_pos = free_main; break;
            case CSS_JUSTIFY_CENTER:     main_pos = free_main / 2.0; break;
            case CSS_JUSTIFY_SPACE_BETWEEN:
                extra_gap = (count > 1) ? free_main / (count - 1) : 0.0;
                break;
            case CSS_JUSTIFY_SPACE_AROUND:
                extra_gap = (count > 0) ? free_main / count : 0.0;
                initial_gap = extra_gap / 2.0;
                break;
            case CSS_JUSTIFY_SPACE_EVENLY:
                extra_gap = (count > 0) ? free_main / (count + 1) : 0.0;
                initial_gap = extra_gap;
                break;
        }
        main_pos += initial_gap;

        int first = reverse ? start + count - 1 : start;
        int dir = reverse ? -1 : 1;
        for (int k = 0; k < count; k++) {
            int j = first + k * dir;
            FlexItemData *it = &items[j];
            int c = it->idx;
            LayoutBox *child = layout_box(ctx, c);

            double main_offset = main_pos + it->main_margin_start;
            double cross_offset_in_line = 0.0;
            switch (container->align_items) {
                case CSS_ALIGN_FLEX_END:
                    cross_offset_in_line = line->cross_size - it->cross_size - it->cross_margin_end;
                    break;
                case CSS_ALIGN_CENTER:
                    cross_offset_in_line = (line->cross_size - it->cross_size -
                                            it->cross_margin_start - it->cross_margin_end) / 2.0
                                            + it->cross_margin_start;
                    break;
                case CSS_ALIGN_STRETCH:
                    cross_offset_in_line = it->cross_margin_start;
                    {
                        double stretched = line->cross_size - it->cross_margin_start - it->cross_margin_end;
                        stretched = layout_clamp_size(stretched, it->min_cross, it->max_cross);
                        if (stretched > it->cross_size) it->cross_size = stretched;
                    }
                    break;
                case CSS_ALIGN_FLEX_START:
                default:
                    cross_offset_in_line = it->cross_margin_start;
                    break;
            }

            double old_x = child->x;
            double old_y = child->y;
            if (is_row) {
                child->x = content_left + main_offset;
                child->y = content_top + cross_offset + cross_offset_in_line;
                child->width = it->main_size;
                child->height = it->cross_size;
            } else {
                child->x = content_left + cross_offset + cross_offset_in_line;
                child->y = content_top + main_offset;
                child->width = it->cross_size;
                child->height = it->main_size;
            }
            layout_offset_children(ctx, c, child->x - old_x, child->y - old_y);

            layout_update_content_sizes(child);
            atomic_store_u32(&layout_state(ctx, c)->top_down_done, 1);

            main_pos += it->main_size + it->main_margin_start + it->main_margin_end + gap_main + extra_gap;
        }
        cross_offset += line->cross_size + gap_cross;
    }

    /* If the container's cross size is still auto, size it to its content. */
    if (is_row && container->height == 0.0) {
        container->height = cross_offset + container->padding_top + container->padding_bottom
                            + container->border_top + container->border_bottom;
        layout_update_content_sizes(container);
    } else if (!is_row && container->width == 0.0) {
        container->width = cross_offset + container->padding_left + container->padding_right
                           + container->border_left + container->border_right;
        layout_update_content_sizes(container);
    }

    free(lines);
    free(items);
    free(children);
}

/* ============================================================================
 * Top-down pass
 * ============================================================================ */

static void layout_top_down_node(LayoutContext *ctx, int idx)
{
    LayoutNodeRef *node = layout_node_ref(ctx, idx);
    LayoutNodeState *state = layout_state(ctx, idx);
    LayoutBox *box = layout_box(ctx, idx);

    if (atomic_load_u32(&state->top_down_done) != 0) return;

    if (node->parent_idx >= 0) {
        /* Wait for parent. */
        while (atomic_load_u32(&ctx->states[node->parent_idx].top_down_done) == 0) {
            layout_thread_yield();
        }
        LayoutBox *parent = layout_box(ctx, node->parent_idx);

        /* Inherit color if not explicitly set (simple heuristic). */
        if (box->color_r == 0 && box->color_g == 0 && box->color_b == 0 && box->color_a == 0) {
            box->color_r = parent->color_r;
            box->color_g = parent->color_g;
            box->color_b = parent->color_b;
            box->color_a = parent->color_a;
        }

        /* Inherit font properties. */
        if (box->font_size <= 0.0 || (box->font_size == 16.0 && parent->font_size != 16.0)) {
            box->font_size = parent->font_size;
        }
        if (box->font_family[0] == '\0') {
            memcpy(box->font_family, parent->font_family, sizeof(box->font_family));
        }

        if (parent->display == CSS_DISPLAY_FLEX) {
            /* The flex container has already positioned and sized this item. */
            layout_update_content_sizes(box);
        } else {
            /* Normal flow: resolve width/height, then stack or flow. */
            layout_resolve_used_sizes(box, parent->content_width);

            /* Wait for previous sibling before touching parent's line state. */
            if (node->prev_sibling_idx >= 0) {
                while (atomic_load_u32(&ctx->states[node->prev_sibling_idx].top_down_done) == 0) {
                    layout_thread_yield();
                }
            }

            double content_left = parent->x + parent->padding_left + parent->border_left;
            double content_top  = parent->y + parent->padding_top + parent->border_top;
            double avail_width  = parent->content_width;

            if (node->prev_sibling_idx < 0) {
                parent->line_x = content_left;
                parent->line_y_offset = 0.0;
                parent->line_height = 0.0;
            }

            if (box->display == CSS_DISPLAY_NONE) {
                /* No box generated; leave line state untouched. */
            } else if (layout_is_block_flow(box->display)) {
                /* Block-level boxes start a new line and stack vertically. */
                parent->line_y_offset += parent->line_height;
                parent->line_height = 0.0;
                parent->line_x = content_left;

                box->x = content_left + box->margin_left;
                box->y = content_top + parent->line_y_offset + box->margin_top;

                parent->line_y_offset = (box->y + box->height + box->margin_bottom) - content_top;
            } else {
                /* Inline / inline-block boxes flow on lines and wrap when needed. */
                if (box->width == 0.0) box->width = 80.0;
                if (box->height == 0.0) box->height = 20.0;
                layout_update_content_sizes(box);

                double child_span = box->margin_left + box->width + box->margin_right;

                /* Wrap to next line if we would overflow and aren't at line start. */
                if (parent->line_x + child_span > content_left + avail_width
                    && parent->line_x > content_left) {
                    parent->line_y_offset += parent->line_height;
                    parent->line_height = 0.0;
                    parent->line_x = content_left;
                }

                box->x = parent->line_x + box->margin_left;
                box->y = content_top + parent->line_y_offset + box->margin_top;

                parent->line_x += child_span;
                double h = box->margin_top + box->height + box->margin_bottom;
                if (h > parent->line_height) parent->line_height = h;
            }
        }
    } else {
        /* Root forms the initial containing block. */
        box->x = 0;
        box->y = 0;
        if (box->width == 0) box->width = ctx->viewport_width;
        if (box->height == 0) box->height = ctx->viewport_height;
        layout_resolve_used_sizes(box, box->width);
    }

    if (box->display == CSS_DISPLAY_FLEX) {
        layout_flex_container(ctx, idx);
    }

    atomic_store_u32(&state->top_down_done, 1);
}

static void layout_top_down_job(void *arg)
{
    LayoutChunk *chunk = (LayoutChunk*)arg;
    for (int i = chunk->start; i < chunk->end; i++) {
        layout_top_down_node(chunk->ctx, chunk->order[i]);
    }
    free(chunk);
}

/* ============================================================================
 * Bottom-up pass
 * ============================================================================ */

static void layout_bottom_up_node(LayoutContext *ctx, int idx)
{
    LayoutNodeRef *node = layout_node_ref(ctx, idx);
    LayoutNodeState *state = layout_state(ctx, idx);
    LayoutBox *box = layout_box(ctx, idx);

    /* Wait for all children. */
    while (atomic_load_u32(&state->children_remaining) > 0) {
        layout_thread_yield();
    }

    /* Compute auto height from children.  Because the top-down pass already
     * stacked siblings and flowed inline boxes on lines, the parent's height
     * is determined by the bottom-most child. */
    if (box->height == 0) {
        double content_top = box->y + box->padding_top + box->border_top;
        double max_bottom = content_top;
        for (int c = node->first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
            LayoutBox *child = layout_box(ctx, c);
            if (child->display == CSS_DISPLAY_NONE) continue;
            double child_bottom = child->y + child->height + child->margin_bottom;
            if (child_bottom > max_bottom) max_bottom = child_bottom;
        }
        box->content_height = max_bottom - content_top;
        if (box->content_height < 0) box->content_height = 0;
        box->height = box->content_height + box->padding_top + box->padding_bottom
                      + box->border_top + box->border_bottom;
    } else {
        box->content_height = box->height - box->padding_top - box->padding_bottom
                              - box->border_top - box->border_bottom;
        if (box->content_height < 0) box->content_height = 0;
    }

    box->baseline = box->height;
    atomic_store_u32(&state->bottom_up_done, 1);

    if (node->parent_idx >= 0) {
        atomic_fetch_sub_u32(&ctx->states[node->parent_idx].children_remaining, 1);
    }
}

static void layout_bottom_up_job(void *arg)
{
    LayoutChunk *chunk = (LayoutChunk*)arg;
    for (int i = chunk->start; i < chunk->end; i++) {
        layout_bottom_up_node(chunk->ctx, chunk->order[i]);
    }
    free(chunk);
}

/* ============================================================================
 * Chunk dispatch
 * ============================================================================ */

static bool layout_dispatch_chunks(LayoutContext *ctx, const int *order, int count,
                                   void (*job_func)(void*))
{
    int chunk_count = 4; /* TODO: query thread pool size */
    if (chunk_count < 1) chunk_count = 1;
    if (chunk_count > count) chunk_count = count;
    if (count == 0) return true;

    int per_chunk = count / chunk_count;
    int remainder = count % chunk_count;

    int start = 0;
    for (int i = 0; i < chunk_count; i++) {
        int end = start + per_chunk + (i < remainder ? 1 : 0);
        if (end <= start) continue;

        LayoutChunk *chunk = (LayoutChunk*)malloc(sizeof(LayoutChunk));
        if (!chunk) return false;
        chunk->ctx = ctx;
        chunk->order = order;
        chunk->start = start;
        chunk->end = end;

        if (!gc_thread_pool_submit_job(job_func, chunk)) {
            free(chunk);
            return false;
        }
        start = end;
    }

    gc_thread_pool_wait_empty();
    return true;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

bool css_layout_document(LayoutContext *ctx, CssStylesheet *sheet)
{
    if (!ctx || ctx->tree.count == 0) return false;

    layout_apply_stylesheet(ctx, sheet);

    /* Initialize synchronization state. */
    for (int i = 0; i < ctx->tree.count; i++) {
        atomic_store_u32(&ctx->states[i].top_down_done, 0);
        atomic_store_u32(&ctx->states[i].bottom_up_done, 0);
        atomic_store_u32(&ctx->states[i].children_remaining, ctx->tree.nodes[i].child_count);
    }

    /* Top-down pass. */
    if (!layout_dispatch_chunks(ctx, ctx->tree.preorder, ctx->tree.count, layout_top_down_job)) {
        return false;
    }

    /* Bottom-up pass. */
    if (!layout_dispatch_chunks(ctx, ctx->tree.postorder, ctx->tree.count, layout_bottom_up_job)) {
        return false;
    }

    for (int i = 0; i < ctx->tree.count; i++) {
        ctx->boxes[i].flags |= LAYOUT_HAS_LAYOUT;
    }
    return true;
}

bool css_layout_run(LayoutContext *ctx, HtmlDocument *doc, CssStylesheet *sheet,
                    double viewport_width, double viewport_height)
{
    if (!css_layout_tree_build(ctx, doc)) return false;
    ctx->viewport_width = viewport_width;
    ctx->viewport_height = viewport_height;
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

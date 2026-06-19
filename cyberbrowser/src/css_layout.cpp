/*
 * CSS Layout Engine - Implementation
 *
 * Parallel top-down / bottom-up layout as described in PARALLEL_CSS_LAYOUT.md.
 */

#include "css_layout.h"
#include "platform.h"
#include "quickjs_gc_unified.h"

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

static void layout_apply_declaration(LayoutBox *box, const CssDeclaration *decl,
                                     double parent_width, double viewport_width)
{
    const char *prop = decl->property;
    const char *value = decl->value;
    if (!prop || !value) return;

    if (strcasecmp(prop, "width") == 0) {
        box->width = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "height") == 0) {
        box->height = css_parse_length(value, parent_width, viewport_width);
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
    } else if (strcasecmp(prop, "padding-left") == 0) {
        box->padding_left = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "padding-right") == 0) {
        box->padding_right = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "padding-top") == 0) {
        box->padding_top = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "padding-bottom") == 0) {
        box->padding_bottom = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "border-left-width") == 0) {
        box->border_left = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "border-right-width") == 0) {
        box->border_right = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "border-top-width") == 0) {
        box->border_top = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "border-bottom-width") == 0) {
        box->border_bottom = css_parse_length(value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "color") == 0) {
        css_parse_color(value, &box->color_r, &box->color_g, &box->color_b, &box->color_a);
    } else if (strcasecmp(prop, "background-color") == 0) {
        css_parse_color(value, &box->background_color_r, &box->background_color_g,
                        &box->background_color_b, &box->background_color_a);
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

/* Apply a stylesheet to all nodes before layout (stores used values in boxes). */
static void layout_apply_stylesheet(LayoutContext *ctx, CssStylesheet *sheet)
{
    for (int i = 0; i < ctx->tree.count; i++) {
        LayoutBox *box = layout_box(ctx, i);
        HtmlNode *node = layout_node_dom(ctx, ctx->tree.nodes[i].dom_node_idx);
        if (!node || node->type != HTML_NODE_ELEMENT) continue;

        if (sheet) {
            for (int r = 0; r < sheet->rule_count; r++) {
                CssRule *rule = &sheet->rules[r];
                /* Minimal matching: tag name only for now. */
                if (strcasecmp(rule->selector_text, node->tag_name) == 0) {
                    for (int d = 0; d < rule->declaration_count; d++) {
                        layout_apply_declaration(box, &rule->declarations[d],
                                                 ctx->viewport_width, ctx->viewport_width);
                    }
                }
            }
        }

        /* Inline styles have higher precedence. */
        layout_apply_inline_style(box, node, ctx->viewport_width, ctx->viewport_width);
    }
}

/* ============================================================================
 * Top-down pass
 * ============================================================================ */

static void layout_top_down_node(LayoutContext *ctx, int idx)
{
    LayoutNodeRef *node = layout_node_ref(ctx, idx);
    LayoutNodeState *state = layout_state(ctx, idx);
    LayoutBox *box = layout_box(ctx, idx);

    /* Wait for parent. */
    if (node->parent_idx >= 0) {
        while (atomic_load_u32(&ctx->states[node->parent_idx].top_down_done) == 0) {
            layout_thread_yield();
        }
        LayoutBox *parent = layout_box(ctx, node->parent_idx);
        box->x = parent->x + parent->padding_left + box->margin_left;
        box->y = parent->y + parent->padding_top + box->margin_top;

        /* Inherit color if not explicitly set (simple heuristic). */
        if (box->color_r == 0 && box->color_g == 0 && box->color_b == 0 && box->color_a == 0) {
            box->color_r = parent->color_r;
            box->color_g = parent->color_g;
            box->color_b = parent->color_b;
            box->color_a = parent->color_a;
        }
    } else {
        box->x = 0;
        box->y = 0;
        if (box->width == 0) box->width = ctx->viewport_width;
        if (box->height == 0) box->height = ctx->viewport_height;
    }

    /* Default width fills containing block if not set. */
    if (box->width == 0 && node->parent_idx >= 0) {
        LayoutBox *parent = layout_box(ctx, node->parent_idx);
        box->width = parent->content_width - box->margin_left - box->margin_right;
        if (box->width < 0) box->width = 0;
    }

    box->content_width = box->width - box->padding_left - box->padding_right
                         - box->border_left - box->border_right;
    if (box->content_width < 0) box->content_width = 0;

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

    /* Compute auto height from children. */
    if (box->height == 0) {
        double child_height = 0;
        for (int c = node->first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
            LayoutBox *child = layout_box(ctx, c);
            double h = child->margin_top + child->height + child->margin_bottom;
            child_height += h;
        }
        box->content_height = child_height;
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

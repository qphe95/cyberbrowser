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
    if (strcasecmp(value, "flex") == 0) return CSS_DISPLAY_FLEX;
    if (strcasecmp(value, "grid") == 0) return CSS_DISPLAY_GRID;
    return CSS_DISPLAY_OTHER;
}

static CssVisibility css_parse_visibility(const char *value) {
    if (!value) return CSS_VISIBILITY_VISIBLE;
    if (strcasecmp(value, "hidden") == 0) return CSS_VISIBILITY_HIDDEN;
    if (strcasecmp(value, "collapse") == 0) return CSS_VISIBILITY_COLLAPSE;
    return CSS_VISIBILITY_VISIBLE;
}

static bool layout_is_block_like(CssDisplay display) {
    return display == CSS_DISPLAY_BLOCK ||
           display == CSS_DISPLAY_FLEX ||
           display == CSS_DISPLAY_GRID ||
           display == CSS_DISPLAY_INLINE_BLOCK;
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

    LOG_INFO("Applying %d stylesheet(s) to %d layout nodes in parallel", list.count, ctx->tree.count);
    layout_apply_stylesheets_parallel(ctx, &list);
    layout_sheet_list_free(&list);
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
 * Block-flow sibling stacking
 *
 * The parallel top-down pass gives every child the same y coordinate, so we
 * run a single-threaded post-pass that walks the tree in pre-order and stacks
 * block-level siblings vertically.  Inline/inline-block children are flowed on
 * lines and wrap to the next line when they exceed the containing-block width.
 * ============================================================================ */

static void layout_stack_node(LayoutContext *ctx, int idx)
{
    LayoutNodeRef *node = layout_node_ref(ctx, idx);
    LayoutBox *box = layout_box(ctx, idx);

    double y_offset = box->padding_top + box->border_top;
    double line_height = 0.0;
    double line_x = box->padding_left + box->border_left;
    double avail_width = box->width - box->padding_left - box->padding_right
                         - box->border_left - box->border_right;
    if (avail_width < 0.0) avail_width = 0.0;

    for (int c = node->first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        LayoutBox *child = layout_box(ctx, c);
        if (child->display == CSS_DISPLAY_NONE) continue;

        if (layout_is_block_like(child->display)) {
            /* Finish any current inline line before a block. */
            y_offset += line_height;
            line_height = 0.0;
            line_x = box->padding_left + box->border_left;

            /* Block children fill the available width if not explicitly set. */
            if (child->width == 0.0) {
                child->width = avail_width - child->margin_left - child->margin_right;
                if (child->width < 0.0) child->width = 0.0;
                child->content_width = child->width - child->padding_left - child->padding_right
                                       - child->border_left - child->border_right;
                if (child->content_width < 0.0) child->content_width = 0.0;
            }

            child->x = box->x + box->padding_left + box->border_left + child->margin_left;
            child->y = box->y + y_offset + child->margin_top;

            layout_stack_node(ctx, c);

            y_offset += child->margin_top + child->height + child->margin_bottom;
        } else {
            /* Inline/inline-block children are flowed on lines. */
            if (child->width == 0.0) child->width = 80.0;
            if (child->height == 0.0) child->height = 20.0;
            child->content_width = child->width - child->padding_left - child->padding_right
                                   - child->border_left - child->border_right;
            if (child->content_width < 0.0) child->content_width = 0.0;
            child->content_height = child->height - child->padding_top - child->padding_bottom
                                    - child->border_top - child->border_bottom;
            if (child->content_height < 0.0) child->content_height = 0.0;

            double child_span = child->margin_left + child->width + child->margin_right;
            if (line_x + child_span > box->padding_left + box->border_left + avail_width
                && line_x > box->padding_left + box->border_left) {
                y_offset += line_height;
                line_height = 0.0;
                line_x = box->padding_left + box->border_left;
            }

            child->x = box->x + line_x + child->margin_left;
            child->y = box->y + y_offset + child->margin_top;

            layout_stack_node(ctx, c);

            line_x += child_span;
            double h = child->margin_top + child->height + child->margin_bottom;
            if (h > line_height) line_height = h;
        }
    }
    y_offset += line_height;

    /* Recompute auto height from the stacked children if the box had no
     * explicit height.  We detect auto height by checking whether the current
     * height equals the bottom-up estimate of child margins + padding/border.
     * This keeps explicit sizes from CSS intact while fixing up containers. */
    if (box->height == 0.0 && node->first_child_idx >= 0) {
        box->content_height = y_offset - box->padding_top - box->border_top;
        if (box->content_height < 0.0) box->content_height = 0.0;
        box->height = box->content_height + box->padding_top + box->padding_bottom
                      + box->border_top + box->border_bottom;
    }
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

    /* Post-pass: stack siblings so block children do not overlap vertically. */
    if (ctx->tree.root_idx >= 0) {
        layout_stack_node(ctx, ctx->tree.root_idx);
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

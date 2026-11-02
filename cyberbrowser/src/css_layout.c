/*
 * CSS Layout Engine - Implementation
 *
 * Style and layout pipeline described in PARALLEL_STYLE_LAYOUT.md.
 */

#include "cyber_env.h"
#include "cyber_profile.h"
#include "css_layout.h"
#include "css_parser.h"
#include "css_props.h"
#include "bidi.h"
#include "browser_api_impl.h"
#include "platform.h"
#include "url_utils.h"
#include "quickjs_gc_unified.h"
#include "http_download.h"
#include "html_dom.h"
#include "dom_fetch.h"
#include "dom_store.h"
#include "dom_wrap.h"
#include "text_shaper.h"
#include "display_list.h"
#include "font_registry.h"
#include "image_cache.h"
#include "css_style.h"
#include "frame_arena.h"
#include "job_system.h"
#include "gc_platform.h"
#include "url_whatwg.h"
#include "css_charset.h"

#include <pthread.h>
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
/* Layout benchmark: per-layout phase times while recording. */
#define LPH_MAX 40
static struct { const char *name; double *v; int n, cap; double cur; } g_lph[LPH_MAX];
static int g_lph_n;
static bool g_lph_on;
static void lph_note(const char *name, double ms)
{
    int i;
    for (i = 0; i < g_lph_n; i++) if (g_lph[i].name == name || strcmp(g_lph[i].name, name) == 0) break;
    if (i == g_lph_n) {
        if (g_lph_n == LPH_MAX) return;
        g_lph[g_lph_n++].name = name;
    }
    g_lph[i].cur += ms;
}
/* The end of one layout: its phase times become samples. */
static void lph_flush(double total_ms)
{
    lph_note("(total)", total_ms);
    for (int i = 0; i < g_lph_n; i++) {
        if (g_lph[i].n == g_lph[i].cap) {
            int nc = g_lph[i].cap ? g_lph[i].cap * 2 : 256;
            double *nv = (double *)realloc(g_lph[i].v, (size_t)nc * sizeof(double));
            if (!nv) continue;
            g_lph[i].v = nv;
            g_lph[i].cap = nc;
        }
        g_lph[i].v[g_lph[i].n++] = g_lph[i].cur;
        g_lph[i].cur = 0.0;
    }
}
static int lph_cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
void css_layout_bench_record(bool on)
{
    g_lph_on = on;
}
void css_layout_bench_report(const char *label)
{
    int layouts = 0;
    for (int i = 0; i < g_lph_n; i++) if (g_lph[i].n > layouts) layouts = g_lph[i].n;
    fprintf(stderr, "[layout-bench] %s: %d layouts\n", label, layouts);
    for (int i = 0; i < g_lph_n; i++) {
        /* (a phase first seen after some layouts: the earlier ones had none) */
        int n = g_lph[i].n;
        if (!n) continue;
        double sum = 0.0;
        for (int k = 0; k < n; k++) sum += g_lph[i].v[k];
        qsort(g_lph[i].v, (size_t)n, sizeof(double), lph_cmp);
        fprintf(stderr, "[layout-bench] %s: %-28s median %8.3f ms  mean %8.3f ms\n", label, g_lph[i].name,
                layouts > n ? 0.0 : g_lph[i].v[n / 2], sum / (layouts ? layouts : 1));
    }
    for (int i = 0; i < g_lph_n; i++) {
        free(g_lph[i].v);
        g_lph[i].v = NULL;
        g_lph[i].n = g_lph[i].cap = 0;
        g_lph[i].cur = 0.0;
    }
    g_lph_n = 0;
}

#define STAGE_BEGIN() double _st_t = layout_now_ms()
#define STAGE(name) do { if (g_css_log > 0 || g_lph_on) { double _n = layout_now_ms(); \
    if (g_lph_on) lph_note(name, _n - _st_t); \
    if (g_css_log > 0) fprintf(stderr, "[css] %-22s %8.3f ms\n", name, _n - _st_t); \
    _st_t = _n; } } while (0)

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
    /* Open-addressing index over props (position + 1), index_cap a power of
     * two at least twice capacity; NULL for maps without one. */
    uint32_t *index;
    uint32_t index_cap;
} CssCustomProps;


static inline HtmlNode* layout_node_dom(LayoutContext *ctx, int dom_idx)
{
    if (dom_idx >= LAYOUT_ANON_DOM0)   /* (an anonymous table box) */
        return dom_idx - LAYOUT_ANON_DOM0 < ctx->anon_nodes_n ? &ctx->anon_nodes[dom_idx - LAYOUT_ANON_DOM0] : NULL;
    if (dom_idx >= LAYOUT_PSEUDO_DOM0)   /* (a pseudo element's box or its text) */
        return dom_idx - LAYOUT_PSEUDO_DOM0 < ctx->pseudo_nodes_n ? &ctx->pseudo_nodes[dom_idx - LAYOUT_PSEUDO_DOM0] : NULL;
    return (HtmlNode*)po_array_payload(&ctx->doc->array, dom_idx);
}

HtmlNode *css_layout_box_node(const LayoutContext *ctx, int idx)
{
    if (!ctx || idx < 0 || idx >= ctx->tree.count || !ctx->doc) return NULL;
    return layout_node_dom((LayoutContext *)ctx, ctx->tree.nodes[idx].dom_node_idx);
}

static const char* layout_node_attribute(HtmlNode *node, const char *name)
{
    if (!node || node->type != HTML_NODE_ELEMENT || !name) return NULL;
    for (HtmlAttribute *a = node->attributes; a; a = a->next) {
        if (strcasecmp(a->name, name) == 0) return a->value;
    }
    return NULL;
}

const char *css_layout_node_text(const LayoutContext *ctx, const HtmlNode *node)
{
    if (!node) return NULL;
    if (ctx && ctx->text_gen && ctx->doc) {
        int i = po_array_index_from_payload(&ctx->doc->array, node);
        if (i >= 0 && i < ctx->text_gen_cap && ctx->text_gen[i]) return ctx->text_gen[i];
    }
    return node->text_content;
}

static inline LayoutNodeRef* layout_node_ref(LayoutContext *ctx, int idx)
{
    return &ctx->tree.nodes[idx];
}

/* Style st into box dst, which keeps its own style slot (a box never
 * shares the style it is styled into).  The style pass leaves geometry
 * alone: layout starts each box from its style (layout_box_start). */
static inline void layout_style_set(LayoutBox *dst, const LayoutStyle *st)
{
    if (dst->s != st) *dst->s = *st;
}

/* Whether box b has style st. */
static inline bool layout_style_same(const LayoutBox *b, const LayoutStyle *st)
{
    return memcmp(b->s, st, sizeof(LayoutStyle)) == 0;
}

static inline LayoutBox* layout_box(LayoutContext *ctx, int idx)
{
    return &ctx->boxes[idx];
}


/* Forward declarations for display/visibility helpers used during tree build. */
static CssDisplay layout_default_display(const char *tag_name);
static unsigned char layout_default_table_role(const char *tag_name);
static char* layout_resolve_url(const char *base_url, const char *href);
static bool style_is_font_size_prop(const char *prop);

/* ============================================================================
 * Layout tree construction
 * ============================================================================ */

static bool layout_prune_root(HtmlDocument *doc, int i);
/* Generation of the style set the last restyle's :has() rules (g_has) are
 * for. */
static uint64_t g_has_gen;

/* Map active DOM node indices to dense layout indices (*count of them).
 * With `prune`, the descendants of display:none elements are left out
 * (layout_prune_root), the elements marked in *pruned (layout indices). */
static bool layout_build_index_map(LayoutContext *ctx, int **out_map, int *count, bool prune,
                                   unsigned char **pruned)
{
    PreOrderCompactionArray *a = &ctx->doc->array;
    int dom_count = (int)po_array_count(a);
    int *map = (int*)malloc(((size_t)dom_count + 1) * sizeof(int));
    if (!map) return false;
    for (int i = 0; i < dom_count; i++) map[i] = -1;
    *pruned = NULL;
    /* Leaving out needs parents before children (the mirror keeps its
     * array in pre-order). */
    unsigned char *out = NULL;   /* per DOM index: 1 left out, 2 a root whose children are */
    /* (the headers read in place: these loops visit every DOM node) */
#define DOM_HDR(i) ((const PreOrderCompactionArrayNode *)(a->buffer + (size_t)(i) * a->node_size))
    for (int i = 0; prune && i < dom_count; i++) {
        if (DOM_HDR(i)->state != PO_ARRAY_NODE_ACTIVE) continue;
        if (DOM_HDR(i)->parent >= i) { prune = false; break; }
    }
    if (prune) out = (unsigned char *)calloc((size_t)dom_count + 1, 1);

    int layout_idx = 0, roots = 0;
    for (int i = 0; i < dom_count; i++) {
        const PreOrderCompactionArrayNode *h = DOM_HDR(i);
        if (h->state != PO_ARRAY_NODE_ACTIVE) continue;
        if (out) {
            int p = h->parent;
            if (p >= 0 && out[p]) { out[i] = 1; continue; }
            if (((const HtmlNode *)h)->hidden_hint != 2 && layout_prune_root(ctx->doc, i)) { out[i] = 2; roots++; }
        }
        map[i] = layout_idx++;
    }
#undef DOM_HDR
    if (out && roots) {
        *pruned = (unsigned char *)calloc((size_t)layout_idx + 1, 1);
        for (int i = 0; *pruned && i < dom_count; i++)
            if (out[i] == 2) (*pruned)[map[i]] = 1;
        if (!*pruned) {   /* cannot note them: in full */
            free(out);
            free(map);
            return layout_build_index_map(ctx, out_map, count, false, pruned);
        }
    }
    free(out);
    *out_map = map;
    *count = layout_idx;
    return true;
}

/* Recycled buffers: one layout's tree and boxes (12 MB on a large page)
 * become the next one's instead of going back to the OS and faulting in
 * again.  The style pass writes every box before it is read. */
typedef struct LayoutSpare { void *p; size_t cap; } LayoutSpare;
static LayoutSpare g_spare_nodes, g_spare_boxes, g_spare_styles;

static void *layout_spare_take(LayoutSpare *sp, size_t need)
{
    void *p;
    if (sp->p && sp->cap >= need) {
        p = sp->p;
        sp->p = NULL;
        sp->cap = 0;
    } else {
        p = malloc(need ? need : 1);
    }
    /* CYBER_LAYOUT_POISON=1: garbage in, so a read before the write shows. */
    static int poison = -1;
    if (poison < 0) poison = cyber_getenv("CYBER_LAYOUT_POISON") ? 1 : 0;
    if (poison && p) memset(p, 0xA5, need);
    return p;
}

static void layout_spare_give(LayoutSpare *sp, void *p, size_t cap)
{
    if (!p) return;
    if (sp->p && sp->cap >= cap) { free(p); return; }
    free(sp->p);
    sp->p = p;
    sp->cap = cap;
}

/* Style slots beyond a tree's first block (LayoutContext.styles): blocks
 * added for the boxes a kept tree gained, and the slots of the boxes it
 * lost, for reuse. */
typedef struct LayoutSlots {
    LayoutStyle **blocks;
    int nblocks, blocks_cap;
    LayoutStyle **free;
    int nfree, free_cap;
} LayoutSlots;

static LayoutStyle *layout_slot_take(LayoutContext *ctx)
{
    LayoutSlots *p = ctx->slot_pool;
    if (!p) {
        p = ctx->slot_pool = (LayoutSlots *)calloc(1, sizeof(LayoutSlots));
        if (!p) return NULL;
    }
    if (p->nfree == 0) {
        enum { N = 256 };
        LayoutStyle *b = (LayoutStyle *)malloc(N * sizeof(LayoutStyle));
        if (!b) return NULL;
        if (p->nblocks == p->blocks_cap) {
            int nc = p->blocks_cap ? 2 * p->blocks_cap : 8;
            LayoutStyle **nb = (LayoutStyle **)realloc(p->blocks, (size_t)nc * sizeof(*nb));
            if (!nb) { free(b); return NULL; }
            p->blocks = nb;
            p->blocks_cap = nc;
        }
        if (p->free_cap < p->nfree + N) {
            int nc = p->free_cap ? 2 * p->free_cap : 2 * N;
            while (nc < p->nfree + N) nc *= 2;
            LayoutStyle **nf = (LayoutStyle **)realloc(p->free, (size_t)nc * sizeof(*nf));
            if (!nf) { free(b); return NULL; }
            p->free = nf;
            p->free_cap = nc;
        }
        p->blocks[p->nblocks++] = b;
        for (int i = N - 1; i >= 0; i--) p->free[p->nfree++] = &b[i];
    }
    return p->free[--p->nfree];
}

static void layout_slot_give(LayoutContext *ctx, LayoutStyle *slot)
{
    LayoutSlots *p = ctx->slot_pool;
    if (!p) {
        p = ctx->slot_pool = (LayoutSlots *)calloc(1, sizeof(LayoutSlots));
        if (!p) return;   /* (the block it is in lives as long as the tree) */
    }
    if (p->nfree == p->free_cap) {
        int nc = p->free_cap ? 2 * p->free_cap : 256;
        LayoutStyle **nf = (LayoutStyle **)realloc(p->free, (size_t)nc * sizeof(*nf));
        if (!nf) return;
        p->free = nf;
        p->free_cap = nc;
    }
    p->free[p->nfree++] = slot;
}

static void layout_slots_free(LayoutSlots *p)
{
    if (!p) return;
    for (int i = 0; i < p->nblocks; i++) free(p->blocks[i]);
    free(p->blocks);
    free(p->free);
    free(p);
}

struct LayoutFlow;
static void layout_flow_free(struct LayoutFlow *F);

/* (carry: the slots are left to layout_tree_carry) */
static bool layout_build_nodes(LayoutContext *ctx, const int *map, int active, bool carry)
{
    int dom_count = (int)po_array_count(&ctx->doc->array);

    ctx->tree.nodes = (LayoutNodeRef*)layout_spare_take(&g_spare_nodes, (size_t)active * sizeof(LayoutNodeRef));
    ctx->tree.count = active;
    ctx->boxes = (LayoutBox*)layout_spare_take(&g_spare_boxes, (size_t)active * sizeof(LayoutBox));
    ctx->ids = (uint32_t *)malloc(((size_t)active + 1) * sizeof(uint32_t));
    if (!ctx->tree.nodes || !ctx->boxes || !ctx->ids) return false;
    if (!carry) {
        ctx->styles = (LayoutStyle*)layout_spare_take(&g_spare_styles, (size_t)active * sizeof(LayoutStyle));
        ctx->styles_n = active;
        ctx->slot = (LayoutStyle **)malloc(((size_t)active + 1) * sizeof(LayoutStyle *));
        if (!ctx->styles || !ctx->slot) return false;
        for (int i = 0; i < active; i++) ctx->boxes[i].s = ctx->slot[i] = &ctx->styles[i];
    }

    for (int i = 0; i < dom_count; i++) {
        int idx = map[i];
        if (idx < 0) continue;

        PreOrderCompactionArrayNode *hdr = po_array_node(&ctx->doc->array, i);
        LayoutNodeRef *node = &ctx->tree.nodes[idx];
        node->dom_node_idx = i;
        ctx->ids[idx] = ((const HtmlNode *)hdr)->store_id;
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
            if (map[c] >= 0) child_count++;   /* (none when left out) */
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

/* The defaults every box starts from. */
static const LayoutBox *layout_box_template(void)
{
    static LayoutBox tmpl;
    static LayoutStyle tmpl_style;
    static int tmpl_ready;
    if (!tmpl_ready) {
        LayoutBox *box = &tmpl;
        memset(box, 0, sizeof(*box));
        memset(&tmpl_style, 0, sizeof(tmpl_style));
        box->s = &tmpl_style;
        box->s->visibility = CSS_VISIBILITY_VISIBLE;
        box->s->position = CSS_POSITION_STATIC;
        box->s->overflow = 0;
        box->s->box_sizing = CSS_BOX_SIZING_CONTENT_BOX;
        box->s->flex_direction = CSS_FLEX_DIRECTION_ROW;
        box->s->flex_wrap = CSS_FLEX_WRAP_NOWRAP;
        box->s->justify_content = CSS_JUSTIFY_FLEX_START;
        box->s->align_items = CSS_ALIGN_STRETCH;
        box->s->font_size = 16.0;
        box->s->font_size_ratio = 0.0;
        box->s->font_family = "";
        box->s->font_weight = 400;
        box->s->font_italic = 0;
        box->s->font_weight_set = 0;
        box->s->font_italic_set = 0;
        box->s->text_decoration = 0;
        box->s->text_decoration_set = 0;
        box->s->line_height = 0.0;
        box->s->line_height_set = 0;
        box->s->list_style_type = 0;
        box->s->list_style_type_set = 0;
        box->s->border_color_set = 0;
        box->s->background_image_url = "";
        box->s->color_r = 0.0;
        box->s->color_g = 0.0;
        box->s->color_b = 0.0;
        box->s->color_a = 1.0;
        box->s->color_set = 0;
        box->s->fill_kind = 0;
        box->s->fill_set = 0;
        box->s->font_size_set = 0;
        box->s->width_set = 0;
        box->s->height_set = 0;
        box->s->visibility_set = 0;
        box->s->text_align = CSS_TEXT_ALIGN_LEFT;
        box->s->text_align_set = 0;
        box->s->flex_basis = -1.0;
        box->s->flex_basis_percent = 0.0;
        box->s->flex_grow = 0.0;
        box->wrap_first_w = 0.0;
        box->wrap_cont_w = 0.0;
        box->wrap_cont_x = 0.0;
        box->wrap_cont2_x = 0.0;
        box->wrap_cont2_w = 0.0;
        box->wrap_cont2_line = 0;
        box->s->font_slot = -1;
        box->s->margin_em[0] = box->s->margin_em[1] = box->s->margin_em[2] = box->s->margin_em[3] = 0.0;
        box->s->padding_em[0] = box->s->padding_em[1] = box->s->padding_em[2] = box->s->padding_em[3] = 0.0;
        box->s->em_deferred = 0;
        box->s->pct_sides = 0;
        box->s->minmax_pct_bits = 0;
        box->s->width_pct_px = 0.0;
        box->s->height_pct_px = 0.0;
        box->s->flex_basis_pct_px = 0.0;
        box->s->flex_shrink = 1.0;
        box->s->min_width = 0.0;
        box->s->max_width = -1.0;     /* (none) */
        box->s->min_height = 0.0;
        box->s->max_height = -1.0;
        box->s->top = 0.0;
        box->s->left = 0.0;
        box->s->right = 0.0;
        box->s->bottom = 0.0;
        box->s->positioned_sides = 0;
        box->s->width_percent = 0.0;
        box->s->height_percent = 0.0;
        box->s->gap_row = 0.0;
        box->s->gap_col = 0.0;
        box->s->aspect_ratio = 0.0;
        box->s->aspect_ratio_prop = 0;
        tmpl_ready = 1;
    }
    return &tmpl;
}

/* A box before its style: the defaults plus its tag's display.  The style
 * pass sets each box from this (or copies a styled one), so a tree build
 * need not initialize the boxes, and their buffer can be recycled. */
static void layout_box_init(LayoutContext *ctx, int idx)
{
    LayoutBox *box = &ctx->boxes[idx];
    layout_style_set(box, layout_box_template()->s);
    HtmlNode *node = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    if (node && node->type == HTML_NODE_ELEMENT) {
        box->s->display = layout_default_display(node->tag_name);
        box->s->table_role = layout_default_table_role(node->tag_name);
        if (box->s->table_role == 2)
            box->s->table_group = !strcasecmp(node->tag_name, "thead") ? 1 : !strcasecmp(node->tag_name, "tfoot") ? 2 : 0;
        /* HTML UA stylesheet: table cells are middle-aligned by default. */
        if (box->s->table_role == 4) box->s->vertical_align = 1;
    } else {
        box->s->display = CSS_DISPLAY_INLINE;
    }
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

static bool layout_tree_build_ex(LayoutContext *ctx, HtmlDocument *doc, bool prune, bool carry)
{
    if (!ctx || !doc) return false;
    memset(ctx, 0, sizeof(*ctx));
    ctx->doc = doc;

    int *map = NULL;
    int count = 0;
    unsigned char *pruned = NULL;
    double tb0 = layout_now_ms();
    if (!layout_build_index_map(ctx, &map, &count, prune, &pruned)) return false;
    double tb1 = layout_now_ms();
    ctx->pruned = pruned;
    for (int i = 0; pruned && i < count; i++) ctx->pruned_n += pruned[i];
    ctx->prune_has_gen = g_has_gen;

    bool ok = layout_build_nodes(ctx, map, count, carry);
    double tb2 = layout_now_ms();
    if (ok) ok = layout_build_orders(ctx);
    if (g_css_log > 0)
        fprintf(stderr, "[css] tree: index map %8.3f ms\n[css] tree: nodes %8.3f ms\n"
                "[css] tree: orders %8.3f ms\n[css] tree: doc nodes %d, boxes %d\n",
                tb1 - tb0, tb2 - tb1, layout_now_ms() - tb2, (int)po_array_count(&doc->array), count);

    if (ok) {
        ctx->custom_props = (const CssCustomProps**)calloc((size_t)ctx->tree.count + 1, sizeof(CssCustomProps*));
        if (!ctx->custom_props) ok = false;
    }

    if (ok) {
        ctx->float_cap = 64;
        ctx->float_stack = (FloatRec*)calloc((size_t)ctx->float_cap, sizeof(FloatRec));
        if (!ctx->float_stack) ok = false;
    }

    ctx->dom_map = map;   /* (a kept tree's style pass finds changed elements with it) */
    ctx->dom_map_n = (int)po_array_count(&doc->array);
    return ok;
}

static bool layout_tree_build(LayoutContext *ctx, HtmlDocument *doc, bool prune)
{
    return layout_tree_build_ex(ctx, doc, prune, false);
}

bool css_layout_tree_build(LayoutContext *ctx, HtmlDocument *doc)
{
    return layout_tree_build(ctx, doc, false);
}

/* What pseudo box pair k was made from. */
typedef struct LayoutPseudoPair {
    int owner;
    unsigned char side;
    unsigned char no_park;      /* (::first-letter, or a box whose text it split) */
    unsigned char dynamic;      /* its content has counters or quotes (layout_counters) */
    int n;
    const CssDeclaration *const *decls;
    const char *content;
} LayoutPseudoPair;

/* A pseudo box of the last layout, kept for the next: its element (store
 * id) and side, its index then, what it was made from, its and its text's
 * style slots and nodes (the text's string moves with it). */
typedef struct LayoutParkedPseudo {
    uint32_t owner_id;
    unsigned char side, used;
    int old_pe, n;
    const CssDeclaration *const *decls;
    const char *content;
    LayoutStyle *slot_e, *slot_t;
    HtmlNode e, t;
} LayoutParkedPseudo;

static void layout_text_frags_free(LayoutContext *ctx);

void css_layout_tree_free(LayoutContext *ctx)
{
    if (!ctx) return;
    free((void *)ctx->custom_props);
    frame_arena_reset(&ctx->arena);
    free(ctx->float_stack);
    layout_spare_give(&g_spare_nodes, ctx->tree.nodes, (size_t)ctx->tree.count * sizeof(LayoutNodeRef));
    free(ctx->tree.preorder);
    free(ctx->tree.postorder);
    layout_spare_give(&g_spare_boxes, ctx->boxes, (size_t)ctx->tree.count * sizeof(LayoutBox));
    layout_spare_give(&g_spare_styles, ctx->styles, (size_t)ctx->styles_n * sizeof(LayoutStyle));
    layout_slots_free(ctx->slot_pool);
    free(ctx->slot);
    free(ctx->ids);
    free(ctx->style_ok);
    free(ctx->child_ids);
    free(ctx->intrinsic_cache);
    free(ctx->has_flags);
    free(ctx->pruned);
    free(ctx->dom_map);
    free(ctx->attr_ids);
    free(ctx->sc_key);
    free(ctx->sc_entry);
    free(ctx->slot_ver);
    free(ctx->has_bits);
    free(ctx->style_left);
    layout_flow_free(ctx->flow);
    free(ctx->flow_old);
    if (ctx->text_norm) {
        for (int i = 0; i < ctx->tree.count; i++) free(ctx->text_norm[i]);
        free(ctx->text_norm);
    }
    if (ctx->text_gen) {
        for (int i = 0; i < ctx->text_gen_cap; i++) free(ctx->text_gen[i]);
        free(ctx->text_gen);
    }
    for (int i = 0; ctx->pieces && i < ctx->pieces_n; i++) free(ctx->pieces[i].p);
    free(ctx->pieces);
    layout_text_frags_free(ctx);
    for (int k = 0; k < ctx->pseudo_nodes_n; k++) free(ctx->pseudo_nodes[k].text_content);
    free(ctx->pseudo_nodes);
    free(ctx->anon_nodes);
    free(ctx->pseudo_rec);
    free(ctx->pseudo_pairs);
    for (int j = 0; j < ctx->parked_n; j++) {
        free((void *)ctx->parked[j].decls);
        if (!ctx->parked[j].used) free(ctx->parked[j].t.text_content);
    }
    free(ctx->parked);
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
        "white-space", "direction", "letter-spacing", "word-spacing", "text-transform", "text-indent", NULL
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
/* The element whose declarations are being applied (layout_apply_declaration):
 * its font gives ex and ch. */
static __thread const LayoutBox *t_unit_box;

/* ex (the font's x-height) or ch (the advance of "0") in ems, for the
 * element t_unit_box with its font as styled so far; 0.5 without one. */
static double unit_font_ratio(bool ch)
{
    const LayoutBox *b = t_unit_box;
    if (!b || !b->s->font_family || !b->s->font_family[0]) return 0.5;
    TextShaper *f = font_registry_slot(font_registry_resolve(b->s->font_family, b->s->font_weight,
                                                             b->s->font_italic != 0));
    float r = f ? (ch ? text_shaper_zero_advance(f) : text_shaper_x_height(f)) : 0.0f;
    return r > 0.0f ? (double)r : 0.5;
}

static double css_unit_factor(const char *unit, double parent_value, double font_size, bool *known)
{
    *known = true;
    if (strcasecmp(unit, "ex") == 0) return font_size * unit_font_ratio(false);
    if (strcasecmp(unit, "ch") == 0) return font_size * unit_font_ratio(true);
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

/* Comparing px + pct operands needs a percentage basis the value parser
 * does not have; the viewport width stands in for the containing block. */
static double g_calc_pct_hint = 1024.0;   /* viewport width of the running layout */
static double calc_pct_hint(void) { return g_calc_pct_hint; }
static double calc_cmp_value(CssLen v) { return v.px + v.pct * calc_pct_hint(); }

/* min() / max() / clamp() / round() / mod() / rem() / abs() (CSS Values 4).
 * Unknown functions evaluate to 0, like other unsupported expressions. */
static CssLen calc_parse_function(CalcParser *p)
{
    CssLen zero = { 0.0, 0.0 };
    size_t ns = p->pos;
    while (p->pos < p->len && (isalpha((unsigned char)p->s[p->pos]) || p->s[p->pos] == '-')) p->pos++;
    size_t nl = p->pos - ns;
    char name[8] = {0};
    if (nl >= sizeof(name) || p->pos >= p->len || p->s[p->pos] != '(') return zero;
    for (size_t i = 0; i < nl; i++) name[i] = (char)tolower((unsigned char)p->s[ns + i]);
    p->pos++;

    /* round()'s optional rounding strategy comes first. */
    int strategy = 0;   /* 0 nearest, 1 up, 2 down, 3 to-zero */
    if (strcmp(name, "round") == 0) {
        calc_skip_space(p);
        static const char *const strat[] = { "nearest", "up", "down", "to-zero" };
        for (int k = 0; k < 4; k++) {
            size_t sl = strlen(strat[k]);
            if (p->len - p->pos > sl && strncasecmp(p->s + p->pos, strat[k], sl) == 0 &&
                !isalpha((unsigned char)p->s[p->pos + sl]) && p->s[p->pos + sl] != '-') {
                strategy = k;
                p->pos += sl;
                calc_skip_space(p);
                if (p->pos < p->len && p->s[p->pos] == ',') p->pos++;
                break;
            }
        }
    }

    CssLen args[8];
    int n = 0;
    for (;;) {
        CssLen v = calc_parse_expr(p);
        if (n < 8) args[n++] = v;
        calc_skip_space(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        break;
    }
    if (p->pos < p->len && p->s[p->pos] == ')') p->pos++;
    if (n == 0) return zero;

    if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0) {
        bool want_min = name[1] == 'i';
        CssLen best = args[0];
        for (int i = 1; i < n; i++) {
            double a = calc_cmp_value(args[i]), b = calc_cmp_value(best);
            if (want_min ? a < b : a > b) best = args[i];
        }
        return best;
    }
    if (strcmp(name, "clamp") == 0) {
        if (n < 3) return args[0];
        CssLen v = args[1];
        if (calc_cmp_value(v) > calc_cmp_value(args[2])) v = args[2];
        if (calc_cmp_value(v) < calc_cmp_value(args[0])) v = args[0];
        return v;
    }
    if (strcmp(name, "abs") == 0) {
        if (calc_cmp_value(args[0]) < 0.0) { args[0].px = -args[0].px; args[0].pct = -args[0].pct; }
        return args[0];
    }
    if (strcmp(name, "round") == 0 || strcmp(name, "mod") == 0 || strcmp(name, "rem") == 0) {
        double a = calc_cmp_value(args[0]);
        double b = n > 1 ? calc_cmp_value(args[1]) : 1.0;
        CssLen out = { 0.0, 0.0 };
        if (b == 0.0) return out;
        double q = a / b, r;
        if (name[0] == 'r' && name[1] == 'o') {
            r = strategy == 1 ? ceil(q) : strategy == 2 ? floor(q) :
                strategy == 3 ? trunc(q) : floor(q + 0.5);
            out.px = r * b;
        } else if (name[0] == 'm') {
            out.px = a - b * floor(q);     /* mod: sign of the divisor */
        } else {
            out.px = a - b * trunc(q);     /* rem: sign of the dividend */
        }
        return out;
    }
    return zero;
}

static CssLen calc_parse_primary(CalcParser *p)
{
    CssLen zero = { 0.0, 0.0 };
    calc_skip_space(p);
    if (p->pos >= p->len) return zero;

    /* Nested calc( and bare parentheses. */
    if (p->len - p->pos > 5 && strncasecmp(p->s + p->pos, "calc(", 5) == 0) p->pos += 4;
    if (isalpha((unsigned char)p->s[p->pos])) return calc_parse_function(p);
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
    if (isalpha((unsigned char)*value) && strchr(value, '(')) {
        CalcParser p = {0};
        p.s = value;
        p.len = strlen(p.s);
        p.font_size = font_size;
        return calc_parse_function(&p);
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

    size_t vlen = strlen(value);
    while (vlen && isspace((unsigned char)value[vlen - 1])) vlen--;
    if (value[0] == '#' && (vlen == 4 || vlen == 7)) {
        /* the common hex forms without a tokenizer */
        int d[6];
        for (size_t i = 1; i < vlen; i++) {
            char c = (char)tolower((unsigned char)value[i]);
            if (!isxdigit((unsigned char)c)) return false;
            d[i - 1] = isdigit((unsigned char)c) ? c - '0' : c - 'a' + 10;
        }
        if (vlen == 4) { *r = d[0] * 17 / 255.0; *g = d[1] * 17 / 255.0; *b = d[2] * 17 / 255.0; }
        else { *r = (d[0] * 16 + d[1]) / 255.0; *g = (d[2] * 16 + d[3]) / 255.0; *b = (d[4] * 16 + d[5]) / 255.0; }
        return true;
    }
    if (strncasecmp(value, "rgb", 3) == 0) {
        /* rgb(N, N, N) / rgba(N, N, N, A) with plain numbers, without a
         * tokenizer; anything else (percentages, none, calc) falls through */
        const char *p = value + 3 + (tolower((unsigned char)value[3]) == 'a');
        double v[4] = { 0, 0, 0, 1 };
        int n = 0;
        if (*p++ == '(') {
            for (; n < 4; n++) {
                while (*p == ' ') p++;
                char *e;
                v[n] = strtod(p, &e);
                if (e == p || !(isdigit((unsigned char)e[-1]) || e[-1] == '.')) break;
                p = e;
                while (*p == ' ') p++;
                if (*p == ',') { p++; continue; }
                if (*p == ')' && (n == 2 || n == 3)) {
                    for (int k = 0; k < 3; k++) v[k] = v[k] < 0 ? 0 : v[k] > 255 ? 255 : v[k];
                    *r = v[0] / 255; *g = v[1] / 255; *b = v[2] / 255;
                    *a = v[3] < 0 ? 0 : v[3] > 1 ? 1 : v[3];
                    return true;
                }
                break;
            }
        }
    }
    if (value[0] == '#' || memchr(value, '(', vlen)) {
        /* every other syntax (rgb() hsl() hwb() lab() lch() oklab() oklch()
         * color() color-mix() light-dark(), relative colors) */
        double c[4];
        if (!css_color_parse(value, c)) return false;
        *r = c[0] / 255; *g = c[1] / 255; *b = c[2] / 255; *a = c[3];
        return true;
    }

    /* Named colors (CSS Color 4 section 6.1), and transparent. */
    static const struct { const char *name; uint32_t rgb; } names[] = {
        {"aliceblue", 0xf0f8ff}, {"antiquewhite", 0xfaebd7}, {"aqua", 0x00ffff}, {"aquamarine", 0x7fffd4},
        {"azure", 0xf0ffff}, {"beige", 0xf5f5dc}, {"bisque", 0xffe4c4}, {"black", 0x000000},
        {"blanchedalmond", 0xffebcd}, {"blue", 0x0000ff}, {"blueviolet", 0x8a2be2}, {"brown", 0xa52a2a},
        {"burlywood", 0xdeb887}, {"cadetblue", 0x5f9ea0}, {"chartreuse", 0x7fff00}, {"chocolate", 0xd2691e},
        {"coral", 0xff7f50}, {"cornflowerblue", 0x6495ed}, {"cornsilk", 0xfff8dc}, {"crimson", 0xdc143c},
        {"cyan", 0x00ffff}, {"darkblue", 0x00008b}, {"darkcyan", 0x008b8b}, {"darkgoldenrod", 0xb8860b},
        {"darkgray", 0xa9a9a9}, {"darkgreen", 0x006400}, {"darkgrey", 0xa9a9a9}, {"darkkhaki", 0xbdb76b},
        {"darkmagenta", 0x8b008b}, {"darkolivegreen", 0x556b2f}, {"darkorange", 0xff8c00}, {"darkorchid", 0x9932cc},
        {"darkred", 0x8b0000}, {"darksalmon", 0xe9967a}, {"darkseagreen", 0x8fbc8f}, {"darkslateblue", 0x483d8b},
        {"darkslategray", 0x2f4f4f}, {"darkslategrey", 0x2f4f4f}, {"darkturquoise", 0x00ced1}, {"darkviolet", 0x9400d3},
        {"deeppink", 0xff1493}, {"deepskyblue", 0x00bfff}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
        {"dodgerblue", 0x1e90ff}, {"firebrick", 0xb22222}, {"floralwhite", 0xfffaf0}, {"forestgreen", 0x228b22},
        {"fuchsia", 0xff00ff}, {"gainsboro", 0xdcdcdc}, {"ghostwhite", 0xf8f8ff}, {"gold", 0xffd700},
        {"goldenrod", 0xdaa520}, {"gray", 0x808080}, {"green", 0x008000}, {"greenyellow", 0xadff2f},
        {"grey", 0x808080}, {"honeydew", 0xf0fff0}, {"hotpink", 0xff69b4}, {"indianred", 0xcd5c5c},
        {"indigo", 0x4b0082}, {"ivory", 0xfffff0}, {"khaki", 0xf0e68c}, {"lavender", 0xe6e6fa},
        {"lavenderblush", 0xfff0f5}, {"lawngreen", 0x7cfc00}, {"lemonchiffon", 0xfffacd}, {"lightblue", 0xadd8e6},
        {"lightcoral", 0xf08080}, {"lightcyan", 0xe0ffff}, {"lightgoldenrodyellow", 0xfafad2}, {"lightgray", 0xd3d3d3},
        {"lightgreen", 0x90ee90}, {"lightgrey", 0xd3d3d3}, {"lightpink", 0xffb6c1}, {"lightsalmon", 0xffa07a},
        {"lightseagreen", 0x20b2aa}, {"lightskyblue", 0x87cefa}, {"lightslategray", 0x778899},
        {"lightslategrey", 0x778899}, {"lightsteelblue", 0xb0c4de}, {"lightyellow", 0xffffe0}, {"lime", 0x00ff00},
        {"limegreen", 0x32cd32}, {"linen", 0xfaf0e6}, {"magenta", 0xff00ff}, {"maroon", 0x800000},
        {"mediumaquamarine", 0x66cdaa}, {"mediumblue", 0x0000cd}, {"mediumorchid", 0xba55d3},
        {"mediumpurple", 0x9370db}, {"mediumseagreen", 0x3cb371}, {"mediumslateblue", 0x7b68ee},
        {"mediumspringgreen", 0x00fa9a}, {"mediumturquoise", 0x48d1cc}, {"mediumvioletred", 0xc71585},
        {"midnightblue", 0x191970}, {"mintcream", 0xf5fffa}, {"mistyrose", 0xffe4e1}, {"moccasin", 0xffe4b5},
        {"navajowhite", 0xffdead}, {"navy", 0x000080}, {"oldlace", 0xfdf5e6}, {"olive", 0x808000},
        {"olivedrab", 0x6b8e23}, {"orange", 0xffa500}, {"orangered", 0xff4500}, {"orchid", 0xda70d6},
        {"palegoldenrod", 0xeee8aa}, {"palegreen", 0x98fb98}, {"paleturquoise", 0xafeeee},
        {"palevioletred", 0xdb7093}, {"papayawhip", 0xffefd5}, {"peachpuff", 0xffdab9}, {"peru", 0xcd853f},
        {"pink", 0xffc0cb}, {"plum", 0xdda0dd}, {"powderblue", 0xb0e0e6}, {"purple", 0x800080},
        {"rebeccapurple", 0x663399}, {"red", 0xff0000}, {"rosybrown", 0xbc8f8f}, {"royalblue", 0x4169e1},
        {"saddlebrown", 0x8b4513}, {"salmon", 0xfa8072}, {"sandybrown", 0xf4a460}, {"seagreen", 0x2e8b57},
        {"seashell", 0xfff5ee}, {"sienna", 0xa0522d}, {"silver", 0xc0c0c0}, {"skyblue", 0x87ceeb},
        {"slateblue", 0x6a5acd}, {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xfffafa},
        {"springgreen", 0x00ff7f}, {"steelblue", 0x4682b4}, {"tan", 0xd2b48c}, {"teal", 0x008080},
        {"thistle", 0xd8bfd8}, {"tomato", 0xff6347}, {"turquoise", 0x40e0d0}, {"violet", 0xee82ee},
        {"wheat", 0xf5deb3}, {"white", 0xffffff}, {"whitesmoke", 0xf5f5f5}, {"yellow", 0xffff00},
        {"yellowgreen", 0x9acd32}, {"transparent", 0x000000},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcasecmp(value, names[i].name) == 0) {
            *r = ((names[i].rgb >> 16) & 0xff) / 255.0;
            *g = ((names[i].rgb >> 8) & 0xff) / 255.0;
            *b = (names[i].rgb & 0xff) / 255.0;
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
        padding ? &box->s->padding_top : &box->s->margin_top,
        padding ? &box->s->padding_right : &box->s->margin_right,
        padding ? &box->s->padding_bottom : &box->s->margin_bottom,
        padding ? &box->s->padding_left : &box->s->margin_left,
    };
    double *pct = padding ? box->s->padding_pct : box->s->margin_pct;
    double *base = padding ? box->s->padding_base : box->s->margin_base;
    unsigned shift = padding ? 4u : 0u;
    CssLen len4[4];
    int first = side, last = side;
    if (side < 0) {
        char toks[4][128];
        int count = css_split_values(value, toks, 4);
        if (count == 0) return;
        int idx[4];
        css_side_indexes(count, idx);
        for (int i = 0; i < 4; i++) len4[i] = css_parse_len(toks[idx[i]], box->s->font_size);
        first = 0; last = 3;
    } else {
        len4[side] = css_parse_len(value, box->s->font_size);
    }
    for (int i = first; i <= last; i++) {
        unsigned bit = 1u << (shift + (unsigned)i);
        if (len4[i].pct != 0.0) {
            pct[i] = len4[i].pct;
            base[i] = len4[i].px;
            *fields[i] = len4[i].px;
            box->s->pct_sides |= (unsigned char)bit;
        } else {
            box->s->pct_sides &= (unsigned char)~bit;
        }
    }
}

/* A box before it is laid out: no geometry yet, and its style's specified
 * margins, paddings, min/max sizes and insets as the used ones (layout
 * resolves their percentage parts).  A box left unstyled gets only its
 * flag: whatever reads it checks that first. */
static void layout_box_start(LayoutBox *box)
{
    const LayoutStyle *s = box->s;
    if (s->skipped) { box->flags = LAYOUT_STYLE_SKIPPED; return; }
    memset((char *)box + sizeof(LayoutStyle *), 0, sizeof(LayoutBox) - sizeof(LayoutStyle *));
    box->margin_top = s->margin_top;
    box->margin_right = s->margin_right;
    box->margin_bottom = s->margin_bottom;
    box->margin_left = s->margin_left;
    box->padding_top = s->padding_top;
    box->padding_right = s->padding_right;
    box->padding_bottom = s->padding_bottom;
    box->padding_left = s->padding_left;
    box->min_width = s->min_width;
    box->max_width = s->max_width;
    box->min_height = s->min_height;
    box->max_height = s->max_height;
    box->top = s->top;
    box->left = s->left;
    box->right = s->right;
    box->bottom = s->bottom;
}

/* Margins and paddings against the containing block width (CSS 2.1 §8.3,
 * §8.4: percentages refer to the width, also for top and bottom). */
static void layout_resolve_side_pcts(LayoutBox *box, double cb_width)
{
    if (!box->s->pct_sides) return;
    double *fields[8] = {
        &box->margin_top, &box->margin_right, &box->margin_bottom, &box->margin_left,
        &box->padding_top, &box->padding_right, &box->padding_bottom, &box->padding_left,
    };
    if (cb_width < 0.0) cb_width = 0.0;
    for (int i = 0; i < 8; i++) {
        if (!(box->s->pct_sides & (1u << i))) continue;
        double v = i < 4 ? box->s->margin_base[i] + box->s->margin_pct[i] * cb_width
                         : box->s->padding_base[i - 4] + box->s->padding_pct[i - 4] * cb_width;
        /* Negative padding is invalid. */
        if (i >= 4 && v < 0.0) v = 0.0;
        *fields[i] = v;
    }
}

/* min-width/max-width/min-height/max-height (which 0..3): returns the px
 * part, recording a percentage part for layout_resolve_minmax_pcts().  A
 * max of none is -1; a negative value is invalid, leaving the size as it was. */
static double layout_note_minmax(LayoutBox *box, int which, const char *value)
{
    double *cur[4] = { &box->s->min_width, &box->s->max_width, &box->s->min_height, &box->s->max_height };
    unsigned char bit = (unsigned char)(1u << which);
    if ((which & 1) && strcasecmp(value, "none") == 0) {
        box->s->minmax_pct_bits &= (unsigned char)~bit;
        return -1.0;
    }
    CssLen l = css_parse_len(value, box->s->font_size);
    if (l.pct < 0.0 || (l.pct == 0.0 && l.px < 0.0)) return *cur[which];
    if (l.pct != 0.0) {
        box->s->minmax_pct[which] = l.pct;
        box->s->minmax_base[which] = l.px;
        box->s->minmax_pct_bits |= bit;
    } else {
        box->s->minmax_pct_bits &= (unsigned char)~bit;
    }
    return l.px;
}

/* Percentage min/max sizes: widths against the containing block width,
 * heights against its height; with an indefinite height they mean none. */
static void layout_resolve_minmax_pcts(LayoutBox *box, double cb_width, double cb_height)
{
    if (!box->s->minmax_pct_bits) return;
    double *fields[4] = { &box->min_width, &box->max_width, &box->min_height, &box->max_height };
    for (int i = 0; i < 4; i++) {
        if (!(box->s->minmax_pct_bits & (1u << i))) continue;
        double basis = i < 2 ? cb_width : cb_height;
        if (i == 3 && basis <= 0.0) { *fields[i] = -1.0; continue; }
        double v = basis > 0.0 ? box->s->minmax_base[i] + box->s->minmax_pct[i] * basis : 0.0;
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
        values[i] = css_parse_length(toks[i], parent_width, viewport_width, box->s->font_size);
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
    /* Sorted by name (binary search; it runs for every styled element):
     *   none: non-rendered metadata tags generate no boxes;
     *   inline-block: form controls (HTML UA stylesheet);
     *   inline: common replaced/phrasing elements;
     *   block: what the HTML UA stylesheet makes block-level (tables are
     *     laid out as blocks by this engine).
     * Everything else, including custom and unknown elements, is inline. */
#define N CSS_DISPLAY_NONE
#define B CSS_DISPLAY_BLOCK
#define I CSS_DISPLAY_INLINE
#define IB CSS_DISPLAY_INLINE_BLOCK
    static const struct { const char *tag; CssDisplay d; } tags[] = {
        {"a", I}, {"address", B}, {"article", B}, {"aside", B}, {"b", I}, {"base", N},
        {"blockquote", B}, {"body", B}, {"br", I}, {"button", IB}, {"canvas", I}, {"caption", B},
        {"center", B}, {"code", I}, {"col", B}, {"colgroup", B}, {"dd", B}, {"details", B},
        {"dialog", B}, {"dir", B}, {"div", B}, {"dl", B}, {"dt", B}, {"em", I},
        {"fieldset", B}, {"figcaption", B}, {"figure", B}, {"footer", B}, {"form", B},
        {"frame", B}, {"frameset", B}, {"h1", B}, {"h2", B}, {"h3", B}, {"h4", B},
        {"h5", B}, {"h6", B}, {"head", N}, {"header", B}, {"hgroup", B}, {"hr", B},
        {"html", B}, {"i", I}, {"iframe", I}, {"img", I}, {"input", IB}, {"label", I},
        {"legend", B}, {"li", CSS_DISPLAY_LIST_ITEM}, {"link", N}, {"listing", B}, {"main", B},
        {"menu", B}, {"meta", N}, {"nav", B}, {"noscript", N}, {"ol", B}, {"optgroup", B},
        {"p", B}, {"plaintext", B}, {"pre", B}, {"s", I}, {"script", N}, {"search", B},
        {"section", B}, {"select", IB}, {"small", I}, {"span", I}, {"strong", I}, {"style", N},
        {"sub", I}, {"summary", B}, {"sup", I}, {"table", B}, {"tbody", B}, {"td", B},
        {"template", N}, {"textarea", IB}, {"tfoot", B}, {"th", B}, {"thead", B}, {"title", N},
        {"tr", B}, {"u", I}, {"ul", B}, {"wbr", I}, {"xmp", B},
    };
#undef N
#undef B
#undef I
#undef IB
    int lo = 0, hi = (int)(sizeof(tags) / sizeof(tags[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcasecmp(tag_name, tags[mid].tag);
        if (c == 0) return tags[mid].d;
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return CSS_DISPLAY_INLINE;
}

/* The table part an HTML element is by the UA stylesheet (LayoutStyle.table_role). */
static unsigned char layout_default_table_role(const char *tag_name)
{
    if (!tag_name || tag_name[0] != 't') {
        if (tag_name && strcasecmp(tag_name, "caption") == 0) return 5;
        if (tag_name && (strcasecmp(tag_name, "col") == 0 || strcasecmp(tag_name, "colgroup") == 0)) return 6;
        return 0;
    }
    if (strcasecmp(tag_name, "table") == 0) return 1;
    if (strcasecmp(tag_name, "tbody") == 0 || strcasecmp(tag_name, "thead") == 0 ||
        strcasecmp(tag_name, "tfoot") == 0) return 2;
    if (strcasecmp(tag_name, "tr") == 0) return 3;
    if (strcasecmp(tag_name, "td") == 0 || strcasecmp(tag_name, "th") == 0) return 4;
    return 0;
}

/* The table part a display value makes a box. */
static unsigned char css_display_table_role(const char *value)
{
    if (strncasecmp(value, "table", 5) != 0 && strcasecmp(value, "inline-table") != 0) return 0;
    if (strcasecmp(value, "table") == 0 || strcasecmp(value, "inline-table") == 0) return 1;
    if (strcasecmp(value, "table-row-group") == 0 || strcasecmp(value, "table-header-group") == 0 ||
        strcasecmp(value, "table-footer-group") == 0) return 2;
    if (strcasecmp(value, "table-row") == 0) return 3;
    if (strcasecmp(value, "table-cell") == 0) return 4;
    if (strcasecmp(value, "table-caption") == 0) return 5;
    if (strncasecmp(value, "table-column", 12) == 0) return 6;
    return 0;
}

static bool layout_display_is_block_level(CssDisplay d)
{
    return d == CSS_DISPLAY_BLOCK || d == CSS_DISPLAY_FLEX || d == CSS_DISPLAY_GRID ||
           d == CSS_DISPLAY_LIST_ITEM || d == CSS_DISPLAY_OTHER;
}

/* Blockification (CSS Display §2.7) and block-in-inline: flex/grid items,
 * absolutely positioned and floated boxes compute to block-level.  An
 * inline element containing in-flow block-level children is split around
 * them (block_in_inline 2: its parent's flow lays it out in pieces); one
 * containing such an inline is laid out as a block instead (the anonymous
 * block boxes a browser creates around its blocks span the same area).
 * Runs bottom-up so a promotion propagates to inline ancestors. */
static void layout_blockify(LayoutContext *ctx)
{
    const int *post = ctx->tree.postorder;
    for (int i = 0; ctx->pieces && i < ctx->pieces_n; i++) free(ctx->pieces[i].p);
    free(ctx->pieces);
    layout_text_frags_free(ctx);
    ctx->pieces = NULL;
    ctx->pieces_n = 0;
    if (!post) return;
    bool split = false;
    for (int k = 0; k < ctx->tree.count; k++) {
        int i = post[k];
        LayoutBox *box = layout_box(ctx, i);
        if (box->s->display != CSS_DISPLAY_INLINE) continue;
        HtmlNode *node = layout_node_dom(ctx, ctx->tree.nodes[i].dom_node_idx);
        if (!node || node->type != HTML_NODE_ELEMENT) continue;
        if (box->s->block_in_inline == 2) box->s->block_in_inline = 0;   /* (a kept style) */
        bool promote = box->s->position == CSS_POSITION_ABSOLUTE || box->s->position == CSS_POSITION_FIXED ||
                       box->s->float_side != 0;
        int p = ctx->tree.nodes[i].parent_idx;
        if (!promote && p >= 0) {
            CssDisplay pd = layout_box(ctx, p)->s->display;
            if (pd == CSS_DISPLAY_FLEX || pd == CSS_DISPLAY_GRID) promote = true;
        }
        bool blocks = false, nested = false, br = false;
        for (int c = ctx->tree.nodes[i].first_child_idx; !promote && c >= 0;
             c = ctx->tree.nodes[c].next_sibling_idx) {
            LayoutBox *cb = layout_box(ctx, c);
            if (cb->s->display == CSS_DISPLAY_INLINE && cb->s->block_in_inline == 2) nested = true;
            if (layout_display_is_block_level(cb->s->display) && !cb->s->inline_level &&
                cb->s->position != CSS_POSITION_ABSOLUTE && cb->s->position != CSS_POSITION_FIXED &&
                cb->s->float_side == 0)
                blocks = true;
            if (cb->s->display == CSS_DISPLAY_INLINE) {
                HtmlNode *cn = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
                if (cn && cn->type == HTML_NODE_ELEMENT && strcasecmp(cn->tag_name, "br") == 0) br = true;
            }
        }
        /* (a line break in an inline whose parent is a block: laid out in
         * pieces too, one per line, so its children break where the <br>
         * is and its edges go on its first and last lines) */
        if (br && !nested && !blocks && !promote && p >= 0 && layout_box(ctx, p)->s->display != CSS_DISPLAY_INLINE) {
            box->s->block_in_inline = 2;
            split = true;
        }
        if (nested || blocks) {
            box->s->block_in_inline = nested ? 1 : 2;
            if (nested) promote = true;
            else split = true;
        }
        if (promote) box->s->display = CSS_DISPLAY_BLOCK;
    }
    if (split) {
        ctx->pieces = (LayoutPieceList *)calloc((size_t)ctx->tree.count, sizeof(LayoutPieceList));
        if (ctx->pieces) ctx->pieces_n = ctx->tree.count;
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
    return !b->s->height_set || b->height_pct_unresolved;
}
/* Block-level box taking part in block flow (not an atomic inline). */
static bool layout_box_is_block_level(const LayoutBox *box) {
    return layout_is_block_flow(box->s->display) && !box->s->inline_level;
}
static bool layout_box_is_atomic_inline(const LayoutBox *box) {
    return box->s->display == CSS_DISPLAY_INLINE_BLOCK || box->s->inline_level;
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

/* A border style keyword's CssBorderStyle, -1 for none of them. */
static int css_border_style_code(const char *t)
{
    static const char *kw[] = { "none", "hidden", "solid", "dashed", "dotted", "double",
                                "groove", "ridge", "inset", "outset", NULL };
    while (isspace((unsigned char)*t)) t++;
    size_t n = strlen(t);
    while (n && isspace((unsigned char)t[n - 1])) n--;
    for (int i = 0; kw[i]; i++)
        if (strlen(kw[i]) == n && strncasecmp(t, kw[i], n) == 0) return i;
    return -1;
}

static uint32_t css_pack_rgba(double r, double g, double b, double a)
{
    double c[4] = { r, g, b, a };
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        double x = c[i] < 0.0 ? 0.0 : c[i] > 1.0 ? 1.0 : c[i];
        v = (v << 8) | (uint32_t)lround(x * 255.0);
    }
    return v;
}

/* The next whitespace-separated value of *p (parentheses kept whole) into
 * tok; false at the end. */
static bool css_next_value(const char **p, char *tok, size_t cap)
{
    const char *q = *p;
    while (*q && isspace((unsigned char)*q)) q++;
    if (!*q) { *p = q; return false; }
    size_t n = 0;
    int depth = 0;
    while (*q && (depth > 0 || !isspace((unsigned char)*q))) {
        if (*q == '(') depth++;
        else if (*q == ')' && depth > 0) depth--;
        if (n + 1 < cap) tok[n++] = *q;
        q++;
    }
    tok[n] = 0;
    *p = q;
    return true;
}

/* Sides (border_styled bits) set to a style code. */
static void border_set_style(LayoutBox *box, int sides, int code)
{
    for (int k = 0; k < 4; k++) {
        if (!(sides & (1 << k))) continue;
        box->s->border_style[k] = (unsigned char)code;
        if (code >= CSS_BORDER_SOLID) box->s->border_styled |= (unsigned char)(1 << k);
        else box->s->border_styled &= (unsigned char)~(1 << k);
    }
}

/* Sides set to a color value (currentColor: not set); false when it is
 * none. */
static bool border_set_color(LayoutBox *box, int sides, const char *v)
{
    while (isspace((unsigned char)*v)) v++;
    bool current = strncasecmp(v, "currentcolor", 12) == 0;
    double cr, cg, cb, ca;
    if (!current && !css_parse_color(v, &cr, &cg, &cb, &ca)) return false;
    uint32_t rgba = current ? 0 : css_pack_rgba(cr, cg, cb, ca);
    for (int k = 0; k < 4; k++) {
        if (!(sides & (1 << k))) continue;
        box->s->border_rgba[k] = rgba;
        if (current) box->s->border_color_set &= (unsigned char)~(1 << k);
        else box->s->border_color_set |= (unsigned char)(1 << k);
    }
    return true;
}

/* The sides (border_styled bits) a 1-4 value list gives each of its values
 * to, in CSS order top, right, bottom, left: for value i of n. */
static int border_list_sides(int n, int i)
{
    static const int sides[4][4] = {
        { 0x0F, 0, 0, 0 },          /* all */
        { 0x0C, 0x03, 0, 0 },       /* top+bottom, left+right */
        { 0x04, 0x03, 0x08, 0 },    /* top, left+right, bottom */
        { 0x04, 0x02, 0x08, 0x01 }, /* top, right, bottom, left */
    };
    return n >= 1 && n <= 4 && i < n ? sides[n - 1][i] : 0;
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
    box->s->translate_x = box->s->translate_y = 0.0;
    box->s->translate_xpct = box->s->translate_ypct = 0.0;
    box->s->has_translate = 0;
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
            else (px) += css_parse_length(str, pw, vw, box->s->font_size); } while (0)
        if ((nl == 9 && strncasecmp(name, "translate", 9) == 0) ||
            (nl == 11 && strncasecmp(name, "translate3d", 11) == 0)) {
            if (np >= 1) TR_ADD(parts[0], box->s->translate_x, box->s->translate_xpct);
            if (np >= 2) TR_ADD(parts[1], box->s->translate_y, box->s->translate_ypct);
        } else if (nl == 10 && strncasecmp(name, "translateX", 10) == 0) {
            if (np >= 1) TR_ADD(parts[0], box->s->translate_x, box->s->translate_xpct);
        } else if (nl == 10 && strncasecmp(name, "translateY", 10) == 0) {
            if (np >= 1) TR_ADD(parts[0], box->s->translate_y, box->s->translate_ypct);
        } else if (nl == 6 && strncasecmp(name, "matrix", 6) == 0 && np == 6) {
            box->s->translate_x += strtod(parts[4], NULL);
            box->s->translate_y += strtod(parts[5], NULL);
        }
        #undef TR_ADD
    }
    box->s->has_translate = (box->s->translate_x != 0.0 || box->s->translate_y != 0.0 ||
                          box->s->translate_xpct != 0.0 || box->s->translate_ypct != 0.0);
}

/* top/right/bottom/left (side 0..3): auto clears the side; percentages
 * resolve later against the containing block. */
static void layout_set_inset(LayoutBox *box, int side, const char *value,
                             double pw, double vw)
{
    static const uint32_t bits[4] = { LAYOUT_SIDE_TOP, LAYOUT_SIDE_RIGHT, LAYOUT_SIDE_BOTTOM, LAYOUT_SIDE_LEFT };
    double *dst[4] = { &box->s->top, &box->s->right, &box->s->bottom, &box->s->left };
    box->s->inset_pct_bits &= (unsigned char)~(1u << side);
    if (strcasecmp(value, "auto") == 0) {
        *dst[side] = 0.0;
        box->s->positioned_sides &= ~bits[side];
        return;
    }
    if (css_value_is_percent(value)) {
        box->s->inset_pct[side] = css_parse_percent_ratio(value);
        box->s->inset_pct_bits |= (unsigned char)(1u << side);
        *dst[side] = 0.0;
    } else {
        *dst[side] = css_parse_length(value, pw, vw, box->s->font_size);
    }
    box->s->positioned_sides |= bits[side];
}

/* Resolve the used insets against a containing block. */
static void layout_resolve_insets(LayoutBox *box, double cb_w, double cb_h)
{
    if (box->s->inset_pct_bits & 1) box->top = box->s->inset_pct[0] * cb_h;
    if (box->s->inset_pct_bits & 2) box->right = box->s->inset_pct[1] * cb_w;
    if (box->s->inset_pct_bits & 4) box->bottom = box->s->inset_pct[2] * cb_h;
    if (box->s->inset_pct_bits & 8) box->left = box->s->inset_pct[3] * cb_w;
}

/* Process-lifetime string interning for values boxes point at (mask
 * images): style application runs on many threads. */
static const char *layout_intern(const char *s, size_t n)
{
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static char **tab;
    static size_t cap, count;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ull; }
    pthread_mutex_lock(&mu);
    if (count * 2 >= cap) {
        size_t nc = cap ? cap * 2 : 1024;
        char **nt = (char **)calloc(nc, sizeof(char *));
        if (!nt) { pthread_mutex_unlock(&mu); return NULL; }
        for (size_t i = 0; i < cap; i++) {
            if (!tab[i]) continue;
            uint64_t hh = 1469598103934665603ull;
            for (const char *q = tab[i]; *q; q++) { hh ^= (unsigned char)*q; hh *= 1099511628211ull; }
            size_t j = (size_t)hh & (nc - 1);
            while (nt[j]) j = (j + 1) & (nc - 1);
            nt[j] = tab[i];
        }
        free(tab);
        tab = nt;
        cap = nc;
    }
    size_t j = (size_t)h & (cap - 1);
    while (tab[j]) {
        if (strlen(tab[j]) == n && memcmp(tab[j], s, n) == 0) { pthread_mutex_unlock(&mu); return tab[j]; }
        j = (j + 1) & (cap - 1);
    }
    char *c = (char *)malloc(n + 1);
    if (c) {
        memcpy(c, s, n);
        c[n] = '\0';
        tab[j] = c;
        count++;
    }
    pthread_mutex_unlock(&mu);
    return c;
}

/* The same, with "" (never NULL) for an empty string or out of memory. */
static const char *layout_intern_or_empty(const char *s, size_t n)
{
    const char *r = n ? layout_intern(s, n) : NULL;
    return r ? r : "";
}

/* mask-image / the mask shorthand's image: url() or linear-gradient(). */
static void layout_parse_mask_image(LayoutBox *box, const char *value, const char *base_url)
{
    box->s->mask_image = NULL;
    box->s->mask_kind = 0;
    const char *g = strstr(value, "linear-gradient(");
    const char *u = strstr(value, "url(");
    if (g && (!u || g < u)) {
        int depth = 0;
        const char *e = g;
        for (; *e; e++) {
            if (*e == '(') depth++;
            else if (*e == ')' && --depth == 0) { e++; break; }
        }
        box->s->mask_image = layout_intern(g, (size_t)(e - g));
        box->s->mask_kind = box->s->mask_image ? 2 : 0;
    } else if (u) {
        char url[2048];
        if (css_parse_url_value(u, url, sizeof(url)) && url[0]) {
            char *abs = layout_resolve_url(base_url, url);
            if (abs) {
                box->s->mask_image = layout_intern(abs, strlen(abs));
                box->s->mask_kind = box->s->mask_image ? 1 : 0;
                free(abs);
            }
        }
    }
}

/* One mask-size / mask-position component: px, or a fraction with *pct. */
static bool layout_mask_len(const char *tok, double font_size, double *out, bool *pct)
{
    CssLen l = css_parse_len(tok, font_size);
    if (l.pct != 0.0 && l.px == 0.0) { *out = l.pct; *pct = true; return true; }
    *out = l.px;
    *pct = false;
    return true;
}

static void layout_parse_mask_size(LayoutBox *box, const char *value)
{
    while (*value && isspace((unsigned char)*value)) value++;
    box->s->mask_size_pct = 0;
    if (strncasecmp(value, "contain", 7) == 0) { box->s->mask_size_kind = 1; return; }
    if (strncasecmp(value, "cover", 5) == 0) { box->s->mask_size_kind = 2; return; }
    if (strncasecmp(value, "auto", 4) == 0 && !strchr(value, ' ')) { box->s->mask_size_kind = 0; return; }
    /* One or two lengths (calc() included), separated at top level. */
    char a[256] = { 0 }, b[256] = { 0 };
    int depth = 0;
    size_t k = 0;
    const char *p = value;
    for (; *p; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        else if (depth == 0 && isspace((unsigned char)*p)) break;
        if (k + 1 < sizeof(a)) a[k++] = *p;
    }
    while (*p && isspace((unsigned char)*p)) p++;
    snprintf(b, sizeof(b), "%s", p);
    bool pw = false, ph = false;
    box->s->mask_size_kind = 3;
    layout_mask_len(a, box->s->font_size, &box->s->mask_size_w, &pw);
    if (b[0] && strncasecmp(b, "auto", 4) != 0) layout_mask_len(b, box->s->font_size, &box->s->mask_size_h, &ph);
    else box->s->mask_size_h = -1.0;
    box->s->mask_size_pct = (unsigned char)((pw ? 1 : 0) | (ph ? 2 : 0));
}

static void layout_parse_mask_position(LayoutBox *box, const char *value)
{
    char tok[2][128] = { { 0 }, { 0 } };
    int n = 0;
    const char *p = value;
    while (*p && n < 2) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        size_t k = 0;
        int depth = 0;
        while (*p && (depth > 0 || !isspace((unsigned char)*p))) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            if (k + 1 < sizeof(tok[0])) tok[n][k++] = *p;
            p++;
        }
        n++;
    }
    if (n == 1) snprintf(tok[1], sizeof(tok[1]), "center");
    double v[2] = { 0, 0 };
    bool pct[2] = { true, true };
    for (int i = 0; i < 2; i++) {
        const char *t = tok[i];
        if (strcasecmp(t, "center") == 0) v[i] = 0.5;
        else if (strcasecmp(t, "left") == 0 || strcasecmp(t, "top") == 0) v[i] = 0.0;
        else if (strcasecmp(t, "right") == 0 || strcasecmp(t, "bottom") == 0) v[i] = 1.0;
        else layout_mask_len(t, box->s->font_size, &v[i], &pct[i]);
    }
    /* "top left" style pairs name y first. */
    if (strcasecmp(tok[0], "top") == 0 || strcasecmp(tok[0], "bottom") == 0 ||
        strcasecmp(tok[1], "left") == 0 || strcasecmp(tok[1], "right") == 0) {
        double tv = v[0]; v[0] = v[1]; v[1] = tv;
        bool tp = pct[0]; pct[0] = pct[1]; pct[1] = tp;
    }
    box->s->mask_pos_x = v[0];
    box->s->mask_pos_y = v[1];
    box->s->mask_pos_pct = (unsigned char)((pct[0] ? 1 : 0) | (pct[1] ? 2 : 0));
}

/* The text of value up to its first top-level '/' (out of parentheses and
 * strings) into a (trimmed, interned) and the rest into b; *has_b says
 * whether there was a '/'. */
static void css_split_slash(const char *value, const char **a, const char **b, bool *has_b)
{
    int depth = 0;
    char q = 0;
    const char *p = value, *slash = NULL;
    for (; *p; p++) {
        if (q) { if (*p == q) q = 0; continue; }
        if (*p == '"' || *p == '\'') q = *p;
        else if (*p == '(' || *p == '[') depth++;
        else if ((*p == ')' || *p == ']') && depth > 0) depth--;
        else if (*p == '/' && depth == 0) { slash = p; break; }
    }
    const char *ae = slash ? slash : p;
    const char *as = value;
    while (as < ae && isspace((unsigned char)*as)) as++;
    while (ae > as && isspace((unsigned char)ae[-1])) ae--;
    *a = ae > as ? layout_intern(as, (size_t)(ae - as)) : NULL;
    *has_b = slash != NULL;
    *b = NULL;
    if (slash) {
        const char *bs = slash + 1, *be = bs + strlen(bs);
        while (bs < be && isspace((unsigned char)*bs)) bs++;
        while (be > bs && isspace((unsigned char)be[-1])) be--;
        *b = be > bs ? layout_intern(bs, (size_t)(be - bs)) : NULL;
    }
}

/* A track list or placement as stored: NULL for none / auto. */
static const char *css_grid_value(const char *v, const char *none)
{
    if (!v) return NULL;
    while (isspace((unsigned char)*v)) v++;
    if (!*v || strcasecmp(v, none) == 0) return NULL;
    return layout_intern(v, strlen(v));
}

/* One grid line placement as the -start/-end pair of grid-row / grid-column
 * / grid-area: a lone custom ident also names the end line (Grid §8.4). */
static bool css_grid_is_ident(const char *v)
{
    if (!v || !*v || isdigit((unsigned char)*v) || *v == '-' || *v == '+') return false;
    if (strncasecmp(v, "span", 4) == 0 && (v[4] == '\0' || isspace((unsigned char)v[4]))) return false;
    if (strcasecmp(v, "auto") == 0) return false;
    for (const char *p = v; *p; p++) if (isspace((unsigned char)*p)) return false;
    return true;
}

/* grid-template (also grid's template form): rows with optional area
 * strings, '/', columns.  Strings go to areas, the sizes after them to rows. */
static void css_apply_grid_template(LayoutBox *box, const char *value)
{
    const char *a, *b;
    bool has_b;
    css_split_slash(value, &a, &b, &has_b);
    if (!has_b) {
        box->s->grid_rows = box->s->grid_cols = box->s->grid_areas = NULL;
        return;
    }
    box->s->grid_cols = css_grid_value(b, "none");
    if (!a || (!strchr(a, '"') && !strchr(a, '\''))) {
        box->s->grid_rows = css_grid_value(a, "none");
        box->s->grid_areas = NULL;
        return;
    }
    char areas[1024], rows[1024];
    size_t an = 0, rn = 0;
    const char *p = a;
    bool pending = false;   /* a string with no size after it yet */
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '"' || *p == '\'') {
            if (pending && rn + 6 < sizeof(rows)) rn += (size_t)snprintf(rows + rn, sizeof(rows) - rn, "auto ");
            char q = *p++;
            const char *s = p;
            while (*p && *p != q) p++;
            if (an + (size_t)(p - s) + 4 < sizeof(areas)) {
                areas[an++] = '"';
                memcpy(areas + an, s, (size_t)(p - s));
                an += (size_t)(p - s);
                areas[an++] = '"';
                areas[an++] = ' ';
            }
            if (*p) p++;
            pending = true;
            continue;
        }
        const char *s = p;
        int depth = 0;
        while (*p && (depth > 0 || !isspace((unsigned char)*p))) {
            if (*p == '(' || *p == '[') depth++;
            else if (*p == ')' || *p == ']') depth--;
            p++;
        }
        if (rn + (size_t)(p - s) + 2 < sizeof(rows)) {
            memcpy(rows + rn, s, (size_t)(p - s));
            rn += (size_t)(p - s);
            rows[rn++] = ' ';
        }
        if (*s != '[') pending = false;
    }
    if (pending && rn + 6 < sizeof(rows)) rn += (size_t)snprintf(rows + rn, sizeof(rows) - rn, "auto ");
    box->s->grid_areas = an ? layout_intern(areas, an - 1) : NULL;
    box->s->grid_rows = rn ? layout_intern(rows, rn - 1) : NULL;
}

static unsigned char css_parse_self_align(const char *v)
{
    while (isspace((unsigned char)*v)) v++;
    if (strncasecmp(v, "auto", 4) == 0 || strncasecmp(v, "normal", 6) == 0 || strncasecmp(v, "legacy", 6) == 0)
        return 0;
    if (strncasecmp(v, "left", 4) == 0) return (unsigned char)(CSS_ALIGN_FLEX_START + 1);
    if (strncasecmp(v, "right", 5) == 0) return (unsigned char)(CSS_ALIGN_FLEX_END + 1);
    char w[32];
    size_t n = 0;
    while (v[n] && !isspace((unsigned char)v[n]) && n + 1 < sizeof(w)) { w[n] = v[n]; n++; }
    w[n] = '\0';
    if (strcasecmp(w, "safe") == 0 || strcasecmp(w, "unsafe") == 0) return css_parse_self_align(v + n);
    return (unsigned char)(css_parse_align_items(w) + 1);
}

/* Grid, box-alignment shorthands and table spacing properties. */
static void layout_apply_grid_declaration(LayoutBox *box, const char *prop, const char *value,
                                          double parent_width, double viewport_width)
{
    LayoutStyle *s = box->s;
    const char *a, *b;
    bool has_b;
    if (strcasecmp(prop, "grid-template-columns") == 0) s->grid_cols = css_grid_value(value, "none");
    else if (strcasecmp(prop, "grid-template-rows") == 0) s->grid_rows = css_grid_value(value, "none");
    else if (strcasecmp(prop, "grid-template-areas") == 0) s->grid_areas = css_grid_value(value, "none");
    else if (strcasecmp(prop, "grid-auto-columns") == 0) s->grid_auto_cols = css_grid_value(value, "auto");
    else if (strcasecmp(prop, "grid-auto-rows") == 0) s->grid_auto_rows = css_grid_value(value, "auto");
    else if (strcasecmp(prop, "grid-auto-flow") == 0) {
        s->grid_flow = (unsigned char)((strstr(value, "column") ? 1 : 0) | (strstr(value, "dense") ? 2 : 0));
    } else if (strcasecmp(prop, "grid-template") == 0) {
        css_apply_grid_template(box, value);
    } else if (strcasecmp(prop, "grid") == 0) {
        s->grid_auto_cols = s->grid_auto_rows = NULL;
        s->grid_flow = 0;
        const char *af = strstr(value, "auto-flow");
        if (!af) {
            css_apply_grid_template(box, value);
            return;
        }
        css_split_slash(value, &a, &b, &has_b);
        bool rows_side = a && strstr(a, "auto-flow");   /* auto-flow [dense] <auto-rows> / <cols> */
        const char *flow = rows_side ? a : b, *tmpl = rows_side ? b : a;
        const char *t = flow ? flow : "";
        char rest[512];
        size_t rn = 0;
        while (*t) {
            while (isspace((unsigned char)*t)) t++;
            const char *w = t;
            while (*t && !isspace((unsigned char)*t)) t++;
            size_t wl = (size_t)(t - w);
            if ((wl == 9 && strncasecmp(w, "auto-flow", 9) == 0)) continue;
            if (wl == 5 && strncasecmp(w, "dense", 5) == 0) { s->grid_flow |= 2; continue; }
            if (wl && rn + wl + 2 < sizeof(rest)) { memcpy(rest + rn, w, wl); rn += wl; rest[rn++] = ' '; }
        }
        const char *autos = rn ? layout_intern(rest, rn - 1) : NULL;
        s->grid_areas = NULL;
        if (rows_side) {
            s->grid_rows = NULL;
            s->grid_auto_rows = autos;
            s->grid_cols = css_grid_value(tmpl, "none");
        } else {
            s->grid_flow |= 1;
            s->grid_cols = NULL;
            s->grid_auto_cols = autos;
            s->grid_rows = css_grid_value(tmpl, "none");
        }
    } else if (strcasecmp(prop, "grid-row-start") == 0) s->grid_rs = css_grid_value(value, "auto");
    else if (strcasecmp(prop, "grid-row-end") == 0) s->grid_re = css_grid_value(value, "auto");
    else if (strcasecmp(prop, "grid-column-start") == 0) s->grid_cs = css_grid_value(value, "auto");
    else if (strcasecmp(prop, "grid-column-end") == 0) s->grid_ce = css_grid_value(value, "auto");
    else if (strcasecmp(prop, "grid-row") == 0 || strcasecmp(prop, "grid-column") == 0) {
        css_split_slash(value, &a, &b, &has_b);
        const char *st = css_grid_value(a, "auto");
        const char *en = has_b ? css_grid_value(b, "auto") : (css_grid_is_ident(st) ? st : NULL);
        if (prop[5] == 'r') { s->grid_rs = st; s->grid_re = en; }
        else { s->grid_cs = st; s->grid_ce = en; }
    } else if (strcasecmp(prop, "grid-area") == 0) {
        const char *v[4] = { NULL, NULL, NULL, NULL };
        const char *rest = value;
        int n = 0;
        while (rest && n < 4) {
            css_split_slash(rest, &a, &b, &has_b);
            v[n++] = css_grid_value(a, "auto");
            rest = has_b ? (b ? b : "") : NULL;
        }
        /* Omitted values: a custom ident repeats, anything else is auto. */
        if (n < 2) v[1] = css_grid_is_ident(v[0]) ? v[0] : NULL;
        if (n < 3) v[2] = css_grid_is_ident(v[0]) ? v[0] : NULL;
        if (n < 4) v[3] = css_grid_is_ident(v[1]) ? v[1] : NULL;
        s->grid_rs = v[0]; s->grid_cs = v[1]; s->grid_re = v[2]; s->grid_ce = v[3];
    } else if (strcasecmp(prop, "grid-gap") == 0 || strcasecmp(prop, "grid-row-gap") == 0 ||
               strcasecmp(prop, "grid-column-gap") == 0) {
        double *r = &s->gap_row, *c = &s->gap_col;
        if (prop[5] == 'g') css_parse_shorthand_1_or_2(value, parent_width, viewport_width, s->font_size, r, c);
        else if (prop[5] == 'r') *r = css_parse_length(value, parent_width, viewport_width, s->font_size);
        else *c = css_parse_length(value, parent_width, viewport_width, s->font_size);
    } else if (strcasecmp(prop, "justify-items") == 0) s->justify_items = css_parse_self_align(value);
    else if (strcasecmp(prop, "justify-self") == 0) s->justify_self = css_parse_self_align(value);
    else if (strcasecmp(prop, "place-items") == 0 || strcasecmp(prop, "place-self") == 0 ||
             strcasecmp(prop, "place-content") == 0) {
        char first[64];
        size_t n = 0;
        while (isspace((unsigned char)*value)) value++;
        while (value[n] && !isspace((unsigned char)value[n]) && n + 1 < sizeof(first)) { first[n] = value[n]; n++; }
        first[n] = '\0';
        const char *second = value + n;
        while (isspace((unsigned char)*second)) second++;
        if (!*second) second = first;
        if (prop[6] == 'i') {
            s->align_items = css_parse_align_items(first);
            s->justify_items = css_parse_self_align(second);
        } else if (prop[6] == 's') {
            s->align_self = (strcasecmp(first, "auto") == 0 || strcasecmp(first, "normal") == 0)
                ? 0 : (unsigned char)(css_parse_align_items(first) + 1);
            s->justify_self = css_parse_self_align(second);
        } else {
            s->justify_content = css_parse_justify_content(second);
        }
    } else if (strcasecmp(prop, "border-spacing") == 0) {
        css_parse_shorthand_1_or_2(value, parent_width, viewport_width, s->font_size,
                                   &s->border_spacing_h, &s->border_spacing_v);
        s->border_spacing_set = 1;
    } else if (strcasecmp(prop, "border-collapse") == 0) {
        s->border_collapse = strcasecmp(value, "collapse") == 0 ? 1 : 0;
        s->border_collapse_set = 1;
    }
}

static void layout_unset_property(LayoutBox *box, const char *prop);

/* A counter-reset / -increment / -set list: identifiers (not none or a
 * CSS-wide keyword), each with an optional integer. */
static bool layout_counter_list_valid(const char *v, size_t n)
{
    size_t i = 0;
    int names = 0;
    while (i < n) {
        while (i < n && isspace((unsigned char)v[i])) i++;
        if (i >= n) break;
        size_t b = i;
        bool num = isdigit((unsigned char)v[i]) ||
                   ((v[i] == '+' || v[i] == '-') && i + 1 < n && isdigit((unsigned char)v[i + 1]));
        if (!num && (isalpha((unsigned char)v[i]) || v[i] == '_' || v[i] == '-' || (unsigned char)v[i] >= 0x80)) {
            while (i < n && (isalnum((unsigned char)v[i]) || v[i] == '_' || v[i] == '-' || (unsigned char)v[i] >= 0x80)) i++;
            size_t l = i - b;
            static const char *const bad[] = { "none", "inherit", "initial", "unset", "default", NULL };
            for (int k = 0; bad[k]; k++)
                if (l == strlen(bad[k]) && !strncasecmp(v + b, bad[k], l)) return false;
            names++;
            continue;
        }
        if (num) {
            if (!names) return false;   /* (an integer follows a name) */
            if (v[i] == '+' || v[i] == '-') i++;
            if (i >= n || !isdigit((unsigned char)v[i])) return false;
            while (i < n && isdigit((unsigned char)v[i])) i++;
            if (i < n && !isspace((unsigned char)v[i])) return false;
            continue;
        }
        return false;
    }
    return names > 0;
}

/* A negative number among a value's top-level tokens (not inside a
 * function such as calc(), which clamps instead). */
static bool css_value_has_negative(const char *v)
{
    int depth = 0;
    bool start = true;
    for (; *v; v++) {
        char ch = *v;
        if (ch == '(') depth++;
        else if (ch == ')') { if (depth > 0) depth--; }
        else if (depth == 0 && start && ch == '-' && (isdigit((unsigned char)v[1]) || v[1] == '.') &&
                 strtod(v, NULL) < 0.0)   /* (-0 is zero) */
            return true;
        start = isspace((unsigned char)ch) || ch == '/' || ch == ',';
    }
    return false;
}

/* Properties a negative length or number is invalid for (the declaration
 * is then ignored, CSS 2.1 §4.2). */
static bool css_prop_non_negative(const char *p)
{
    static const char *names[] = {
        "padding", "padding-top", "padding-right", "padding-bottom", "padding-left",
        "width", "height", "min-width", "min-height", "max-width", "max-height",
        "border-width", "border-top-width", "border-right-width", "border-bottom-width", "border-left-width",
        "font-size", "line-height", NULL
    };
    for (int i = 0; names[i]; i++) if (strcasecmp(p, names[i]) == 0) return true;
    return false;
}

/* ---- background placement (repeat, position, size, attachment) ---- */
static void bg_reset_placement(LayoutStyle *s)
{
    s->bg_no_repeat_x = s->bg_no_repeat_y = 0;
    s->bg_pos_xf = s->bg_pos_yf = 0.0;
    s->bg_pos_xpx = s->bg_pos_ypx = 0.0;
    s->bg_size_kind = 0;
    s->bg_size_pct = 0;
    s->bg_size_w = s->bg_size_h = -1.0;
    s->bg_fixed = 0;
}

/* A repeat keyword (repeat-x / repeat-y set both axes; a single repeat,
 * no-repeat, space or round is for both unless a second follows: *axis
 * counts the ones seen). */
static bool bg_repeat_token(LayoutStyle *s, const char *t, int *axis)
{
    if (strcasecmp(t, "repeat-x") == 0) { s->bg_no_repeat_x = 0; s->bg_no_repeat_y = 1; *axis = 2; return true; }
    if (strcasecmp(t, "repeat-y") == 0) { s->bg_no_repeat_x = 1; s->bg_no_repeat_y = 0; *axis = 2; return true; }
    int v = strcasecmp(t, "no-repeat") == 0 ? 1 :
            (strcasecmp(t, "repeat") == 0 || strcasecmp(t, "space") == 0 || strcasecmp(t, "round") == 0) ? 0 : -1;
    if (v < 0) return false;
    if (*axis == 0) { s->bg_no_repeat_x = s->bg_no_repeat_y = (unsigned char)v; *axis = 1; }
    else if (*axis == 1) { s->bg_no_repeat_y = (unsigned char)v; *axis = 2; }
    return true;
}

static bool bg_pos_keyword(const char *t)
{
    return !strcasecmp(t, "left") || !strcasecmp(t, "right") || !strcasecmp(t, "top") ||
           !strcasecmp(t, "bottom") || !strcasecmp(t, "center");
}

static bool bg_length_token(const char *t)
{
    return (isdigit((unsigned char)t[0]) || t[0] == '-' || t[0] == '+' || t[0] == '.') ||
           strncasecmp(t, "calc(", 5) == 0;
}

/* A length or percentage token: fraction (*f) or pixels (*px). */
static void bg_value(const char *t, double area_base, double vw, double fs, double *f, double *px)
{
    size_t n = strlen(t);
    *f = 0.0;
    *px = 0.0;
    if (n && t[n - 1] == '%') *f = atof(t) / 100.0;
    else *px = css_parse_length(t, area_base, vw, fs);
}

/* background-position from its tokens (1 to 4). */
static void bg_set_position(LayoutStyle *s, char toks[][64], int n, double vw, double fs)
{
    double xf = 0.5, xpx = 0.0, yf = 0.5, ypx = 0.0;
    if (n == 1) {
        const char *a = toks[0];
        if (!strcasecmp(a, "top")) yf = 0.0;
        else if (!strcasecmp(a, "bottom")) yf = 1.0;
        else if (!strcasecmp(a, "left")) xf = 0.0;
        else if (!strcasecmp(a, "right")) xf = 1.0;
        else if (strcasecmp(a, "center") != 0) bg_value(a, 0.0, vw, fs, &xf, &xpx);
    } else if (n == 2) {
        const char *a = toks[0], *b = toks[1];
        bool swap = !strcasecmp(a, "top") || !strcasecmp(a, "bottom") || !strcasecmp(b, "left") || !strcasecmp(b, "right");
        if (swap) { const char *t = a; a = b; b = t; }
        if (!strcasecmp(a, "left")) xf = 0.0;
        else if (!strcasecmp(a, "right")) xf = 1.0;
        else if (!strcasecmp(a, "center")) xf = 0.5;
        else bg_value(a, 0.0, vw, fs, &xf, &xpx);
        if (!strcasecmp(b, "top")) yf = 0.0;
        else if (!strcasecmp(b, "bottom")) yf = 1.0;
        else if (!strcasecmp(b, "center")) yf = 0.5;
        else bg_value(b, 0.0, vw, fs, &yf, &ypx);
    } else {
        /* Edge keywords with offsets: right 10px bottom 20% ... */
        for (int i = 0; i < n; i++) {
            const char *k = toks[i];
            double of = 0.0, opx = 0.0;
            bool off = i + 1 < n && !bg_pos_keyword(toks[i + 1]);
            if (off) bg_value(toks[i + 1], 0.0, vw, fs, &of, &opx);
            if (!strcasecmp(k, "left")) { xf = of; xpx = opx; }
            else if (!strcasecmp(k, "right")) { xf = 1.0 - of; xpx = -opx; }
            else if (!strcasecmp(k, "top")) { yf = of; ypx = opx; }
            else if (!strcasecmp(k, "bottom")) { yf = 1.0 - of; ypx = -opx; }
            if (off) i++;
        }
    }
    s->bg_pos_xf = xf; s->bg_pos_xpx = xpx;
    s->bg_pos_yf = yf; s->bg_pos_ypx = ypx;
}

/* background-size from its tokens (1 or 2). */
static void bg_set_size(LayoutStyle *s, char toks[][64], int n, double vw, double fs)
{
    s->bg_size_pct = 0;
    s->bg_size_w = s->bg_size_h = -1.0;
    if (n >= 1 && !strcasecmp(toks[0], "cover")) { s->bg_size_kind = 1; return; }
    if (n >= 1 && !strcasecmp(toks[0], "contain")) { s->bg_size_kind = 2; return; }
    s->bg_size_kind = 3;
    for (int i = 0; i < 2; i++) {
        const char *t = i < n ? toks[i] : "auto";
        if (!strcasecmp(t, "auto")) continue;
        double f, px;
        bg_value(t, 0.0, vw, fs, &f, &px);
        size_t l = strlen(t);
        if (l && t[l - 1] == '%') { s->bg_size_pct |= (unsigned char)(1u << i); px = f; }
        if (i == 0) s->bg_size_w = px; else s->bg_size_h = px;
    }
    if (s->bg_size_w < 0.0 && s->bg_size_h < 0.0) s->bg_size_kind = 0;
}

/* Split a value into its top-level tokens (at most max, 64 chars each);
 * a '/' is a token of its own. */
static int bg_tokens(const char *p, char toks[][64], int max)
{
    int n = 0;
    while (*p && n < max) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '/' || *p == ',') { toks[n][0] = *p; toks[n][1] = 0; n++; p++; continue; }
        size_t tl = 0;
        int depth = 0;
        while (p[tl] && (depth > 0 || (!isspace((unsigned char)p[tl]) && p[tl] != '/' && p[tl] != ','))) {
            if (p[tl] == '(') depth++;
            else if (p[tl] == ')' && depth > 0) depth--;
            tl++;
        }
        size_t cl = tl < 63 ? tl : 63;
        memcpy(toks[n], p, cl);
        toks[n][cl] = 0;
        n++;
        p += tl;
    }
    return n;
}

/* Whether decl's value is one layout ignores (css_value_invalid), known
 * once per declaration: the rule's declarations are shared and read-only
 * but for this cache. */
static bool layout_decl_invalid(const CssDeclaration *decl)
{
    unsigned char inv = __atomic_load_n(&decl->invalid, __ATOMIC_RELAXED);
    if (!inv) {
        inv = css_value_invalid(decl->property, decl->value) ? 2 : 1;
        __atomic_store_n((unsigned char *)&decl->invalid, inv, __ATOMIC_RELAXED);
    }
    return inv == 2;
}

/* A copy of a rule's declaration to apply, its value maybe substituted
 * (var()): its validity from the original's cache unless substituted. */
static void layout_decl_copy_validity(CssDeclaration *copy, const CssDeclaration *orig, bool substituted)
{
    copy->invalid = substituted ? 0 : layout_decl_invalid(orig) ? 2 : 1;
}

static void layout_apply_declaration(LayoutBox *box, const CssDeclaration *decl,
                                     double parent_width, double viewport_width,
                                     const char *base_url)
{
    const char *prop = decl->property;
    const char *value = decl->value;
    if (!prop || !value) return;
    t_unit_box = box;
    if (css_prop_non_negative(prop) && css_value_has_negative(value)) return;
    /* (an invalid value is ignored: the declaration before it stays) */
    if (layout_decl_invalid(decl)) return;

    if (strcasecmp(prop, "font") == 0) {
        /* The shorthand as its longhands (each reset when not given). */
        const CssPropDef *p = css_prop_find("font", 4);
        const char **names;
        char **vals = NULL;
        int n = p ? css_prop_longhands(p, &names) : 0;
        if (n <= 0 || css_prop_expand(p, value, &vals) != n) return;
        for (int i = 0; i < n; i++) {
            if (!vals[i][0]) {
                /* (a var(): pending substitution) */
            } else if (strcasecmp(names[i], "font-size") != 0 && css_is_inherited(names[i]) &&
                       (!strcasecmp(vals[i], "inherit") || !strcasecmp(vals[i], "initial") ||
                        !strcasecmp(vals[i], "unset") || !strncasecmp(vals[i], "revert", 6))) {
                layout_unset_property(box, names[i]);   /* as layout_apply_wide_keyword */
            } else {
                CssDeclaration d = { (char *)names[i], vals[i], decl->important };
                layout_apply_declaration(box, &d, parent_width, viewport_width, base_url);
            }
            free(vals[i]);
        }
        free(vals);
        return;
    }
    if (strcasecmp(prop, "display") == 0) {
        box->s->display = css_parse_display(value);
        box->s->inline_level = css_display_is_inline_level(value) ? 1 : 0;
        box->s->table_caption = strcasecmp(value, "table-caption") == 0 ? 1 : 0;
        box->s->webkit_box = (strcasecmp(value, "-webkit-box") == 0 || strcasecmp(value, "box") == 0) ? 1 :
                          strcasecmp(value, "-webkit-inline-box") == 0 ? 2 : 0;
        if (strcasecmp(value, "inline-table") == 0) box->s->display = CSS_DISPLAY_BLOCK;
        box->s->flow_root = strcasecmp(value, "flow-root") == 0;
        if (box->s->flow_root) box->s->display = CSS_DISPLAY_BLOCK;
        box->s->table_role = css_display_table_role(value);
        box->s->table_group = !strcasecmp(value, "table-header-group") ? 1 : !strcasecmp(value, "table-footer-group") ? 2 : 0;
    } else if (strncasecmp(prop, "grid", 4) == 0 || strcasecmp(prop, "justify-items") == 0 ||
               strcasecmp(prop, "justify-self") == 0 || strncasecmp(prop, "place-", 6) == 0 ||
               strncasecmp(prop, "border-spacing", 14) == 0 || strcasecmp(prop, "border-collapse") == 0) {
        layout_apply_grid_declaration(box, prop, value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "visibility") == 0) {
        box->s->visibility = css_parse_visibility(value);
        box->s->visibility_set = 1;
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
                box->s->visibility = CSS_VISIBILITY_HIDDEN;
                box->s->visibility_set = 1;
            }
        }
    } else if (strcasecmp(prop, "opacity") == 0) {
        char *endp = NULL;
        double op = strtod(value, &endp);
        if (endp != value) {            /* invalid values are ignored */
            if (*endp == '%') op /= 100.0;
            if (op < 0.0) op = 0.0;
            if (op > 1.0) op = 1.0;
            box->s->opacity = op;
            box->s->opacity_set = 1;
        }
    } else if (strcasecmp(prop, "position") == 0) {
        box->s->position = css_parse_position(value);
    } else if (strcasecmp(prop, "z-index") == 0) {
        /* auto, or an integer (anything else is invalid: ignored) */
        while (isspace((unsigned char)*value)) value++;
        char *end = NULL;
        long z = strtol(value, &end, 10);
        if (strncasecmp(value, "auto", 4) == 0) {
            box->s->z_index_set = 0;
            box->s->z_index = 0;
        } else if (end != value) {
            while (end && isspace((unsigned char)*end)) end++;
            if (end && (*end == '\0' || *end == '!')) {
                box->s->z_index_set = 1;
                box->s->z_index = (int)(z > 1000000000L ? 1000000000L : z < -1000000000L ? -1000000000L : z);
            }
        }
    } else if (strcasecmp(prop, "box-sizing") == 0) {
        box->s->box_sizing = css_parse_box_sizing(value);
    } else if (strcasecmp(prop, "width") == 0) {
        box->s->width_keyword = 0;
        if (strstr(value, "min-content")) box->s->width_keyword = 1;
        else if (strstr(value, "max-content")) box->s->width_keyword = 2;
        else if (strstr(value, "fit-content")) box->s->width_keyword = 3;
        if (box->s->width_keyword) {
            box->s->width_set = 0;
            box->s->width_percent = 0.0;
            box->s->css_width = 0.0;
        } else {
            /* A percentage part (50%, calc(100% - 20px)) resolves against
             * the containing block at layout time. */
            CssLen l = css_parse_len(value, box->s->font_size);
            box->s->width_percent = l.pct > 0.0 ? l.pct : 0.0;
            box->s->width_pct_px = l.pct > 0.0 ? l.px : 0.0;
            box->s->css_width = l.pct > 0.0 ? 0.0 : l.px;
        }
        if (!box->s->width_keyword) box->s->width_set = (strcasecmp(value, "auto") != 0);
    } else if (strcasecmp(prop, "height") == 0) {
        if (strstr(value, "-content")) {
            box->s->height_set = 0;
            box->s->height_percent = 0.0;
            box->s->css_height = 0.0;
            return;
        }
        {
            CssLen l = css_parse_len(value, box->s->font_size);
            box->s->height_percent = l.pct > 0.0 ? l.pct : 0.0;
            box->s->height_pct_px = l.pct > 0.0 ? l.px : 0.0;
            box->s->css_height = l.pct > 0.0 ? 0.0 : l.px;
        }
        box->s->height_set = (strcasecmp(value, "auto") != 0);
    } else if (strcasecmp(prop, "min-width") == 0) {
        box->s->min_width = layout_note_minmax(box, 0, value);
        if (strcasecmp(value, "auto") == 0) box->s->min_size_set &= (unsigned char)~1;
        else box->s->min_size_set |= 1;
    } else if (strcasecmp(prop, "max-width") == 0) {
        box->s->max_width = layout_note_minmax(box, 1, value);
    } else if (strcasecmp(prop, "min-height") == 0) {
        box->s->min_height = layout_note_minmax(box, 2, value);
        if (strcasecmp(value, "auto") == 0) box->s->min_size_set &= (unsigned char)~2;
        else box->s->min_size_set |= 2;
    } else if (strcasecmp(prop, "max-height") == 0) {
        box->s->max_height = layout_note_minmax(box, 3, value);
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
                                                   &box->s->margin_top, &box->s->margin_right,
                                                   &box->s->margin_bottom, &box->s->margin_left, em4);
        layout_note_side_pcts(box, value, false, -1);
        box->s->em_deferred &= 0xF0; /* shorthand resets all four margin sides */
        box->s->margin_auto = css_margin_auto_mask(value);
        for (int i = 0; i < 4; i++) {
            if (fl & (1u << i)) {
                box->s->margin_em[i] = em4[i];
                box->s->em_deferred |= (unsigned char)(1u << i);
            }
        }
    } else if (strcasecmp(prop, "margin-left") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->s->margin_em[3] = r; box->s->em_deferred |= 0x08; }
        else box->s->em_deferred &= (unsigned char)~0x08;
        box->s->margin_left = css_parse_length(value, parent_width, viewport_width, box->s->font_size);
        layout_note_side_pcts(box, value, false, 3);
        if (strcasecmp(value, "auto") == 0) box->s->margin_auto |= 8;
        else box->s->margin_auto &= (unsigned char)~8;
    } else if (strcasecmp(prop, "margin-right") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->s->margin_em[1] = r; box->s->em_deferred |= 0x02; }
        else box->s->em_deferred &= (unsigned char)~0x02;
        box->s->margin_right = css_parse_length(value, parent_width, viewport_width, box->s->font_size);
        layout_note_side_pcts(box, value, false, 1);
        if (strcasecmp(value, "auto") == 0) box->s->margin_auto |= 2;
        else box->s->margin_auto &= (unsigned char)~2;
    } else if (strcasecmp(prop, "margin-top") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->s->margin_em[0] = r; box->s->em_deferred |= 0x01; }
        else box->s->em_deferred &= (unsigned char)~0x01;
        box->s->margin_top = css_parse_length(value, parent_width, viewport_width, box->s->font_size);
        layout_note_side_pcts(box, value, false, 0);
        if (strcasecmp(value, "auto") == 0) box->s->margin_auto |= 1;
        else box->s->margin_auto &= (unsigned char)~1;
    } else if (strcasecmp(prop, "margin-bottom") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->s->margin_em[2] = r; box->s->em_deferred |= 0x04; }
        else box->s->em_deferred &= (unsigned char)~0x04;
        box->s->margin_bottom = css_parse_length(value, parent_width, viewport_width, box->s->font_size);
        layout_note_side_pcts(box, value, false, 2);
        if (strcasecmp(value, "auto") == 0) box->s->margin_auto |= 4;
        else box->s->margin_auto &= (unsigned char)~4;
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
        box->s->aspect_ratio = (w > 0.0 && h > 0.0) ? h / w : 0.0;
        box->s->aspect_ratio_prop = box->s->aspect_ratio > 0.0;
    } else if (strcasecmp(prop, "padding") == 0) {
        double em4[4];
        unsigned fl = layout_apply_shorthand_sides(box, value, parent_width, viewport_width,
                                                   &box->s->padding_top, &box->s->padding_right,
                                                   &box->s->padding_bottom, &box->s->padding_left, em4);
        layout_note_side_pcts(box, value, true, -1);
        box->s->em_deferred &= 0x0F; /* shorthand resets all four padding sides */
        for (int i = 0; i < 4; i++) {
            if (fl & (1u << i)) {
                box->s->padding_em[i] = em4[i];
                box->s->em_deferred |= (unsigned char)(0x10u << i);
            }
        }
    } else if (strcasecmp(prop, "padding-left") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->s->padding_em[3] = r; box->s->em_deferred |= 0x80; }
        else box->s->em_deferred &= (unsigned char)~0x80;
        box->s->padding_left = css_parse_length(value, parent_width, viewport_width, box->s->font_size);
        layout_note_side_pcts(box, value, true, 3);
    } else if (strcasecmp(prop, "padding-right") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->s->padding_em[1] = r; box->s->em_deferred |= 0x20; }
        else box->s->em_deferred &= (unsigned char)~0x20;
        box->s->padding_right = css_parse_length(value, parent_width, viewport_width, box->s->font_size);
        layout_note_side_pcts(box, value, true, 1);
    } else if (strcasecmp(prop, "padding-top") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->s->padding_em[0] = r; box->s->em_deferred |= 0x10; }
        else box->s->em_deferred &= (unsigned char)~0x10;
        box->s->padding_top = css_parse_length(value, parent_width, viewport_width, box->s->font_size);
        layout_note_side_pcts(box, value, true, 0);
    } else if (strcasecmp(prop, "padding-bottom") == 0) {
        double r;
        if (css_token_is_em(value, &r)) { box->s->padding_em[2] = r; box->s->em_deferred |= 0x40; }
        else box->s->em_deferred &= (unsigned char)~0x40;
        box->s->padding_bottom = css_parse_length(value, parent_width, viewport_width, box->s->font_size);
        layout_note_side_pcts(box, value, true, 2);
    } else if (strcasecmp(prop, "border-left-width") == 0) {
        box->s->border_left = css_border_width_value(value, parent_width, viewport_width, box->s->font_size);
        box->s->border_width_set |= 1;
    } else if (strcasecmp(prop, "border-left-style") == 0) {
        int code = css_border_style_code(value);
        if (code >= 0) border_set_style(box, 1, code);
    } else if (strcasecmp(prop, "border-right-width") == 0) {
        box->s->border_right = css_border_width_value(value, parent_width, viewport_width, box->s->font_size);
        box->s->border_width_set |= 2;
    } else if (strcasecmp(prop, "border-right-style") == 0) {
        int code = css_border_style_code(value);
        if (code >= 0) border_set_style(box, 2, code);
    } else if (strcasecmp(prop, "border-top-width") == 0) {
        box->s->border_top = css_border_width_value(value, parent_width, viewport_width, box->s->font_size);
        box->s->border_width_set |= 4;
    } else if (strcasecmp(prop, "border-top-style") == 0) {
        int code = css_border_style_code(value);
        if (code >= 0) border_set_style(box, 4, code);
    } else if (strcasecmp(prop, "border-bottom-width") == 0) {
        box->s->border_bottom = css_border_width_value(value, parent_width, viewport_width, box->s->font_size);
        box->s->border_width_set |= 8;
    } else if (strcasecmp(prop, "border-bottom-style") == 0) {
        int code = css_border_style_code(value);
        if (code >= 0) border_set_style(box, 8, code);
    } else if (strcasecmp(prop, "flex-direction") == 0) {
        box->s->flex_direction = css_parse_flex_direction(value);
    } else if (strcasecmp(prop, "flex-wrap") == 0) {
        box->s->flex_wrap = css_parse_flex_wrap(value);
    } else if (strcasecmp(prop, "justify-content") == 0) {
        box->s->justify_content = css_parse_justify_content(value);
    } else if (strcasecmp(prop, "align-items") == 0) {
        box->s->align_items = css_parse_align_items(value);
    } else if (strcasecmp(prop, "transform") == 0 || strcasecmp(prop, "-webkit-transform") == 0) {
        css_parse_transform(box, value, parent_width, viewport_width);
    } else if (strcasecmp(prop, "white-space") == 0 || strcasecmp(prop, "white-space-collapse") == 0) {
        unsigned char ws = 0;
        if (strcasecmp(value, "nowrap") == 0) ws = 1;
        else if (strcasecmp(value, "pre") == 0) ws = 2;
        else if (strcasecmp(value, "pre-wrap") == 0 || strcasecmp(value, "break-spaces") == 0 ||
                 strcasecmp(value, "preserve") == 0) ws = 3;
        else if (strcasecmp(value, "pre-line") == 0 || strcasecmp(value, "preserve-breaks") == 0) ws = 4;
        box->s->white_space = ws;
        box->s->white_space_set = 1;
    } else if (strcasecmp(prop, "-webkit-box-orient") == 0 || strcasecmp(prop, "box-orient") == 0) {
        box->s->box_orient_vertical = (strcasecmp(value, "vertical") == 0 ||
                                    strcasecmp(value, "block-axis") == 0) ? 1 : 0;
    } else if (strcasecmp(prop, "-webkit-line-clamp") == 0 || strcasecmp(prop, "line-clamp") == 0) {
        int n = atoi(value);
        box->s->line_clamp = (unsigned char)(n > 0 && n < 255 ? n : 0);
    } else if (strcasecmp(prop, "text-overflow") == 0) {
        box->s->text_overflow_ellipsis = strstr(value, "ellipsis") != NULL;
    } else if (strcasecmp(prop, "object-fit") == 0) {
        box->s->object_fit = strcasecmp(value, "contain") == 0 ? 1 :
                          strcasecmp(value, "cover") == 0 ? 2 :
                          strcasecmp(value, "none") == 0 ? 3 :
                          strcasecmp(value, "scale-down") == 0 ? 4 : 0;
    } else if (strcasecmp(prop, "align-self") == 0) {
        box->s->align_self = (strcasecmp(value, "auto") == 0 || strcasecmp(value, "normal") == 0)
            ? 0 : (unsigned char)(css_parse_align_items(value) + 1);
    } else if (strcasecmp(prop, "order") == 0) {
        box->s->order = atoi(value);
    } else if (strcasecmp(prop, "flex-basis") == 0) {
        css_parse_flex_basis(value, &box->s->flex_basis, &box->s->flex_basis_percent,
                             &box->s->flex_basis_pct_px, box->s->font_size);
    } else if (strcasecmp(prop, "flex-grow") == 0) {
        box->s->flex_grow = strtod(value, NULL);
    } else if (strcasecmp(prop, "flex-shrink") == 0) {
        box->s->flex_shrink = strtod(value, NULL);
    } else if (strcasecmp(prop, "flex") == 0) {
        css_parse_flex_shorthand(value, &box->s->flex_grow, &box->s->flex_shrink, &box->s->flex_basis,
                                 &box->s->flex_basis_percent, &box->s->flex_basis_pct_px, box->s->font_size);
    } else if (strcasecmp(prop, "gap") == 0) {
        css_parse_shorthand_1_or_2(value, parent_width, viewport_width,
                                   box->s->font_size, &box->s->gap_row, &box->s->gap_col);
    } else if (strcasecmp(prop, "row-gap") == 0) {
        box->s->gap_row = css_parse_length(value, parent_width, viewport_width, box->s->font_size);
    } else if (strcasecmp(prop, "column-gap") == 0) {
        box->s->gap_col = css_parse_length(value, parent_width, viewport_width, box->s->font_size);
    } else if (strcasecmp(prop, "fill") == 0) {
        while (isspace((unsigned char)*value)) value++;
        if (strncasecmp(value, "currentcolor", 12) == 0) { box->s->fill_kind = 1; box->s->fill_set = 1; }
        else if (strncasecmp(value, "none", 4) == 0) { box->s->fill_kind = 3; box->s->fill_set = 1; }
        else if (css_parse_color(value, &box->s->fill_r, &box->s->fill_g, &box->s->fill_b, &box->s->fill_a)) {
            box->s->fill_kind = 2; box->s->fill_set = 1;
        }
    } else if (strcasecmp(prop, "color") == 0) {
        if (css_parse_color(value, &box->s->color_r, &box->s->color_g, &box->s->color_b, &box->s->color_a))
            box->s->color_set = 1;
    } else if (strcasecmp(prop, "text-align") == 0) {
        box->s->text_align_how = 1;
        if (strncasecmp(value, "center", 6) == 0) box->s->text_align = CSS_TEXT_ALIGN_CENTER;
        else if (strncasecmp(value, "right", 5) == 0) box->s->text_align = CSS_TEXT_ALIGN_RIGHT;
        else if (strncasecmp(value, "left", 4) == 0) box->s->text_align = CSS_TEXT_ALIGN_LEFT;
        else if (strncasecmp(value, "end", 3) == 0) box->s->text_align_how = 2;
        else box->s->text_align_how = 0;   /* (start, justify) */
        box->s->text_align_set = 1;
    } else if (strcasecmp(prop, "table-layout") == 0) {
        box->s->table_layout_fixed = strcasecmp(value, "fixed") == 0;
    } else if (strcasecmp(prop, "direction") == 0) {
        if (!strcasecmp(value, "rtl")) { box->s->direction = 1; box->s->direction_set = 1; }
        else if (!strcasecmp(value, "ltr")) { box->s->direction = 0; box->s->direction_set = 1; }
    } else if (strcasecmp(prop, "unicode-bidi") == 0) {
        static const char *const kw[] = { "normal", "embed", "isolate", "bidi-override", "isolate-override",
                                          "plaintext" };
        for (int i = 0; i < 6; i++)
            if (!strcasecmp(value, kw[i])) box->s->unicode_bidi = (unsigned char)i;
    } else if (strcasecmp(prop, "float") == 0) {
        if (strcasecmp(value, "left") == 0) box->s->float_side = 1;
        else if (strcasecmp(value, "right") == 0) box->s->float_side = 2;
        else box->s->float_side = 0;
    } else if (strcasecmp(prop, "clear") == 0) {
        if (strcasecmp(value, "left") == 0) box->s->clear = 1;
        else if (strcasecmp(value, "right") == 0) box->s->clear = 2;
        else if (strcasecmp(value, "both") == 0) box->s->clear = 3;
        else box->s->clear = 0;
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
            if (strcasecmp(prop, "overflow") == 0) box->s->overflow = 0;
        }
        else if (strcasecmp(value, "hidden") == 0) box->s->overflow = 1;
        else if (strcasecmp(value, "auto") == 0) box->s->overflow = 2;
        else if (strcasecmp(value, "scroll") == 0) box->s->overflow = 3;
        else if (strcasecmp(value, "clip") == 0) box->s->overflow = 1;
    } else if (strcasecmp(prop, "vertical-align") == 0) {
        if (strcasecmp(value, "middle") == 0) box->s->vertical_align = 1;
        else if (strcasecmp(value, "top") == 0) box->s->vertical_align = 2;
        else if (strcasecmp(value, "bottom") == 0) box->s->vertical_align = 3;
        else box->s->vertical_align = 0;   /* baseline/sub/super/lengths: treat as baseline */
    } else if (strcasecmp(prop, "background-color") == 0) {
        double cr, cg, cb, ca;
        if (css_parse_color(value, &cr, &cg, &cb, &ca)) {
            box->s->background_color_r = cr; box->s->background_color_g = cg;
            box->s->background_color_b = cb; box->s->background_color_a = ca;
        }
    } else if (strcasecmp(prop, "background") == 0) {
        /* Shorthand: pull out the color and/or url() parts we support and
         * ignore the rest (position/repeat/size/attachment/origin).  Parse
         * colors into temporaries: css_parse_color clobbers on failure. */
        const char *p = value;
        char token[1024];
        /* The shorthand resets omitted longhands: transparent, no image. */
        box->s->background_color_r = box->s->background_color_g = box->s->background_color_b = 0.0;
        box->s->background_color_a = 0.0;
        box->s->background_image_url = "";
        bg_reset_placement(box->s);
        {
            /* (repeat, attachment, position and / size, from the first
             * layer: the colour and url() parts are read below) */
            char toks[16][64];
            int n = bg_tokens(value, toks, 16), axis = 0, np = 0, ns = 0;
            char pos[4][64], size[2][64];
            bool after_slash = false;
            for (int i = 0; i < n; i++) {
                const char *t = toks[i];
                if (t[0] == ',') break;
                if (t[0] == '/') { after_slash = true; continue; }
                if (after_slash) {
                    if (ns < 2 && (bg_length_token(t) || !strcasecmp(t, "auto") || !strcasecmp(t, "cover") ||
                                   !strcasecmp(t, "contain"))) { strcpy(size[ns++], t); continue; }
                    after_slash = false;
                }
                if (bg_repeat_token(box->s, t, &axis)) continue;
                if (!strcasecmp(t, "fixed")) { box->s->bg_fixed = 1; continue; }
                if (bg_pos_keyword(t) || (bg_length_token(t) && !css_parse_color(t, &(double){0}, &(double){0}, &(double){0}, &(double){0}))) {
                    if (np < 4) strcpy(pos[np++], t);
                }
            }
            if (np) bg_set_position(box->s, pos, np, viewport_width, box->s->font_size);
            if (ns) bg_set_size(box->s, size, ns, viewport_width, box->s->font_size);
        }
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
                        box->s->background_image_url = layout_intern_or_empty(abs, strlen(abs));
                        free(abs);
                    }
                }
            } else {
                double cr, cg, cb, ca;
                if (css_parse_color(token, &cr, &cg, &cb, &ca)) {
                    box->s->background_color_r = cr; box->s->background_color_g = cg;
                    box->s->background_color_b = cb; box->s->background_color_a = ca;
                }
            }
        }
    } else if (strcasecmp(prop, "background-repeat") == 0) {
        char toks[4][64];
        int n = bg_tokens(value, toks, 4), axis = 0;
        box->s->bg_no_repeat_x = box->s->bg_no_repeat_y = 0;
        for (int i = 0; i < n && toks[i][0] != ','; i++) bg_repeat_token(box->s, toks[i], &axis);
    } else if (strcasecmp(prop, "background-position") == 0) {
        char toks[8][64];
        int n = bg_tokens(value, toks, 8), m = 0;
        while (m < n && toks[m][0] != ',') m++;
        if (m) bg_set_position(box->s, toks, m > 4 ? 4 : m, viewport_width, box->s->font_size);
    } else if (strcasecmp(prop, "background-position-x") == 0 || strcasecmp(prop, "background-position-y") == 0) {
        bool x = prop[20] == 'x' || prop[20] == 'X';
        double f = 0.0, px = 0.0;
        while (isspace((unsigned char)*value)) value++;
        if (!strncasecmp(value, "left", 4) || !strncasecmp(value, "top", 3)) f = 0.0;
        else if (!strncasecmp(value, "right", 5) || !strncasecmp(value, "bottom", 6)) f = 1.0;
        else if (!strncasecmp(value, "center", 6)) f = 0.5;
        else bg_value(value, 0.0, viewport_width, box->s->font_size, &f, &px);
        if (x) { box->s->bg_pos_xf = f; box->s->bg_pos_xpx = px; }
        else { box->s->bg_pos_yf = f; box->s->bg_pos_ypx = px; }
    } else if (strcasecmp(prop, "background-size") == 0) {
        char toks[4][64];
        int n = bg_tokens(value, toks, 4), m = 0;
        while (m < n && toks[m][0] != ',') m++;
        if (m) bg_set_size(box->s, toks, m > 2 ? 2 : m, viewport_width, box->s->font_size);
    } else if (strcasecmp(prop, "background-attachment") == 0) {
        box->s->bg_fixed = strncasecmp(value, "fixed", 5) == 0;
    } else if (strcasecmp(prop, "background-image") == 0) {
        char url[1024];
        if (css_parse_url_value(value, url, sizeof(url)) && url[0]) {
            char *abs = layout_resolve_url(base_url, url);
            if (abs) {
                box->s->background_image_url = layout_intern_or_empty(abs, strlen(abs));
                free(abs);
            }
        } else {
            box->s->background_image_url = "";
        }
    } else if (strcasecmp(prop, "mask-image") == 0 || strcasecmp(prop, "-webkit-mask-image") == 0 ||
               strcasecmp(prop, "mask") == 0 || strcasecmp(prop, "-webkit-mask") == 0) {
        bool shorthand = strcasecmp(prop, "mask") == 0 || strcasecmp(prop, "-webkit-mask") == 0;
        layout_parse_mask_image(box, value, base_url);
        if (shorthand) {
            /* The shorthand's other parts: repeat keywords and position. */
            box->s->mask_no_repeat = strstr(value, "no-repeat") != NULL;
            if (strstr(value, "center")) {
                box->s->mask_pos_x = box->s->mask_pos_y = 0.5;
                box->s->mask_pos_pct = 3;
            }
        }
    } else if (strcasecmp(prop, "box-shadow") == 0 || strcasecmp(prop, "-webkit-box-shadow") == 0) {
        /* Comma-separated shadows; the first outer one is kept:
         * [inset] <x> <y> [<blur> [<spread>]] [<color>] in any order. */
        box->s->shadow_set = 0;
        const char *p = value;
        while (*p && !box->s->shadow_set) {
            const char *st = p;
            int depth = 0;
            while (*p && (depth > 0 || *p != ',')) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                p++;
            }
            char one[512];
            size_t ol = (size_t)(p - st);
            if (ol >= sizeof(one)) ol = sizeof(one) - 1;
            memcpy(one, st, ol);
            one[ol] = '\0';
            if (*p == ',') p++;
            if (strstr(one, "inset") || strncasecmp(one, "none", 4) == 0) continue;
            double len[4] = { 0, 0, 0, 0 };
            int nlen = 0;
            double cr = 0, cg = 0, cb = 0, ca = 1;
            bool color_set = false;
            const char *q = one;
            while (*q) {
                while (*q && isspace((unsigned char)*q)) q++;
                if (!*q) break;
                char tok[256];
                size_t k = 0;
                depth = 0;
                while (*q && (depth > 0 || !isspace((unsigned char)*q))) {
                    if (*q == '(') depth++;
                    else if (*q == ')') depth--;
                    if (k + 1 < sizeof(tok)) tok[k++] = *q;
                    q++;
                }
                tok[k] = '\0';
                double r_, g_, b_, a_;
                if ((isdigit((unsigned char)tok[0]) || tok[0] == '-' || tok[0] == '.' ||
                     strncasecmp(tok, "calc(", 5) == 0) && nlen < 4) {
                    len[nlen++] = css_parse_len(tok, box->s->font_size).px;
                } else if (css_parse_color(tok, &r_, &g_, &b_, &a_)) {
                    cr = r_; cg = g_; cb = b_; ca = a_;
                    color_set = true;
                }
            }
            if (nlen < 2) continue;
            if (!color_set) { cr = box->s->color_r; cg = box->s->color_g; cb = box->s->color_b; ca = box->s->color_a; }
            if (ca <= 0.0) continue;
            box->s->shadow_set = 1;
            box->s->shadow_x = (float)len[0];
            box->s->shadow_y = (float)len[1];
            box->s->shadow_blur = (float)(len[2] > 0 ? len[2] : 0);
            box->s->shadow_spread = (float)len[3];
            box->s->shadow_r = (float)cr; box->s->shadow_g = (float)cg; box->s->shadow_b = (float)cb; box->s->shadow_a = (float)ca;
        }
    } else if (strcasecmp(prop, "border-radius") == 0 || strcasecmp(prop, "-webkit-border-radius") == 0) {
        /* 1-4 values (horizontal radii; "/ vertical" part ignored). */
        char tok[4][128];
        int n = 0;
        const char *p = value;
        while (*p && n < 4) {
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p || *p == '/') break;
            size_t k = 0;
            int depth = 0;
            while (*p && (depth > 0 || (!isspace((unsigned char)*p) && *p != '/'))) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                if (k + 1 < sizeof(tok[0])) tok[n][k++] = *p;
                p++;
            }
            tok[n][k] = '\0';
            n++;
        }
        static const int map[5][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 1, 0, 1 },
                                       { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };
        box->s->radius_pct = 0;
        for (int c = 0; c < 4; c++) {
            box->s->radius[c] = 0.0f;
            if (n == 0) continue;
            double v;
            bool pct;
            layout_mask_len(tok[map[n][c]], box->s->font_size, &v, &pct);
            box->s->radius[c] = (float)(v > 0.0 ? v : 0.0);
            if (pct) box->s->radius_pct |= (unsigned char)(1 << c);
        }
    } else if (strcasecmp(prop, "border-top-left-radius") == 0 || strcasecmp(prop, "border-top-right-radius") == 0 ||
               strcasecmp(prop, "border-bottom-right-radius") == 0 ||
               strcasecmp(prop, "border-bottom-left-radius") == 0) {
        int c = strcasecmp(prop, "border-top-left-radius") == 0 ? 0 :
                strcasecmp(prop, "border-top-right-radius") == 0 ? 1 :
                strcasecmp(prop, "border-bottom-right-radius") == 0 ? 2 : 3;
        char first[128];
        size_t k = 0;
        const char *p = value;
        while (*p && isspace((unsigned char)*p)) p++;
        int depth = 0;
        while (*p && (depth > 0 || !isspace((unsigned char)*p)) && k + 1 < sizeof(first)) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            first[k++] = *p++;
        }
        first[k] = '\0';
        double v;
        bool pct;
        layout_mask_len(first, box->s->font_size, &v, &pct);
        box->s->radius[c] = (float)(v > 0.0 ? v : 0.0);
        if (pct) box->s->radius_pct |= (unsigned char)(1 << c);
        else box->s->radius_pct &= (unsigned char)~(1 << c);
    } else if (strcasecmp(prop, "mask-size") == 0 || strcasecmp(prop, "-webkit-mask-size") == 0) {
        layout_parse_mask_size(box, value);
    } else if (strcasecmp(prop, "mask-position") == 0 || strcasecmp(prop, "-webkit-mask-position") == 0) {
        layout_parse_mask_position(box, value);
    } else if (strcasecmp(prop, "mask-repeat") == 0 || strcasecmp(prop, "-webkit-mask-repeat") == 0) {
        box->s->mask_no_repeat = strstr(value, "no-repeat") != NULL;
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
                box->s->font_size = kw_sizes[k].px;
                box->s->font_size_ratio = 0.0;
                box->s->font_size_set = 1;
                kw_done = true;
                break;
            }
        }
        if (kw_done) {
            /* done */
        } else if (strcasecmp(value, "smaller") == 0) {
            box->s->font_size_ratio = 0.833;
            box->s->font_size = 16.0 * 0.833; /* provisional */
            box->s->font_size_set = 1;
        } else if (strcasecmp(value, "larger") == 0) {
            box->s->font_size_ratio = 1.2;
            box->s->font_size = 16.0 * 1.2; /* provisional */
            box->s->font_size_set = 1;
        } else if (strcasecmp(value, "inherit") == 0) {
            /* Clear the flag so pass-3 inheritance fills the value in. */
            box->s->font_size_set = 0;
            box->s->font_size_ratio = 0.0;
        } else if (strcasecmp(value, "initial") == 0) {
            box->s->font_size = 16.0;
            box->s->font_size_ratio = 0.0;
            box->s->font_size_set = 1;
        /* Percentage and em font-sizes are relative to the PARENT's computed
         * font-size, not to the containing-block width.  Record the ratio and
         * resolve it in a serial preorder pass after the (parallel) apply. */
        } else if (css_value_is_percent(value)) {
            box->s->font_size_ratio = css_parse_percent_ratio(value);
            box->s->font_size = 16.0 * box->s->font_size_ratio; /* provisional */
            box->s->font_size_set = 1;
        } else {
            const char *p = value;
            while (*p && isspace((unsigned char)*p)) p++;
            char *end = NULL;
            double num = strtod(p, &end);
            if (end != p && strcasecmp(end, "em") == 0) {
                box->s->font_size_ratio = num;
                box->s->font_size = 16.0 * num; /* provisional */
            } else {
                box->s->font_size_ratio = 0.0;
                box->s->font_size = css_parse_length(value, parent_width, viewport_width, box->s->font_size);
            }
            /* (a size of 0 is a tiny one: 0 stands for none elsewhere) */
            if (box->s->font_size <= 0.0) box->s->font_size = end != p && num == 0.0 ? 0.01 : 16.0;
            box->s->font_size_set = 1;
        }
    } else if (strcasecmp(prop, "font-family") == 0) {
        /* The whole list: font_registry_resolve takes the first family it
         * has, so one it lacks falls through to the next (a generic last). */
        const char *p = value;
        while (*p && isspace((unsigned char)*p)) p++;
        size_t len = strlen(p);
        while (len && isspace((unsigned char)p[len - 1])) len--;
        box->s->font_family = layout_intern_or_empty(p, len);
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
        box->s->font_weight = w;
        box->s->font_weight_set = 1;
    } else if (strcasecmp(prop, "font-style") == 0) {
        box->s->font_italic = (strncasecmp(value, "italic", 6) == 0 ||
                            strncasecmp(value, "oblique", 7) == 0) ? 1 : 0;
        box->s->font_italic_set = 1;
    } else if (strcasecmp(prop, "text-decoration") == 0 ||
               strcasecmp(prop, "text-decoration-line") == 0) {
        /* Token scan: underline / line-through / overline; "none" clears. */
        unsigned char deco = 0;
        if (strstr(value, "underline")) deco |= 1;
        if (strstr(value, "line-through")) deco |= 2;
        if (strstr(value, "overline")) deco |= 4;
        box->s->text_decoration = deco;
        box->s->text_decoration_set = 1;
    } else if (strcasecmp(prop, "line-height") == 0) {
        box->s->line_height_em = 0.0;
        double em = 0.0;
        if (strncasecmp(value, "normal", 6) == 0) {
            box->s->line_height = 0.0; /* <=0 => normal (font metrics at layout) */
            box->s->line_height_ratio = 0.0;
        } else {
            char *end = NULL;
            double num = strtod(value, &end);
            if (end != value) {
                while (*end && isspace((unsigned char)*end)) end++;
                if (*end == '\0') {
                    /* Unitless multiplier of the element's own font-size:
                     * keep the ratio for inheritance/recompute. */
                    box->s->line_height = num * box->s->font_size;
                    box->s->line_height_ratio = num;
                } else if (css_value_is_percent(value)) {
                    box->s->line_height = css_parse_percent_ratio(value) * box->s->font_size;
                    box->s->line_height_ratio = 0.0;
                    box->s->line_height_em = css_parse_percent_ratio(value);
                } else if (css_token_is_em(value, &em)) {
                    box->s->line_height = em * box->s->font_size;
                    box->s->line_height_ratio = 0.0;
                    box->s->line_height_em = em;
                } else {
                    box->s->line_height_ratio = 0.0;
                    box->s->line_height = css_parse_length(value, parent_width, viewport_width,
                                                        box->s->font_size);
                }
                /* (0 means normal here: a zero line height is a tiny one) */
                if (box->s->line_height <= 0.0) box->s->line_height = 1e-6;
            }
        }
        box->s->line_height_set = 1;
    } else if (strcasecmp(prop, "counter-reset") == 0 || strcasecmp(prop, "counter-increment") == 0 ||
               strcasecmp(prop, "counter-set") == 0) {
        const char *v = value;
        while (isspace((unsigned char)*v)) v++;
        size_t vl = strlen(v);
        while (vl && isspace((unsigned char)v[vl - 1])) vl--;
        const char **dst = prop[8] == 'r' ? &box->s->counter_reset : prop[8] == 'i' ? &box->s->counter_increment
                                                                                   : &box->s->counter_set;
        if (vl == 4 && !strncasecmp(v, "none", 4)) *dst = NULL;
        else if (layout_counter_list_valid(v, vl)) *dst = layout_intern(v, vl);
    } else if (strcasecmp(prop, "letter-spacing") == 0 || strcasecmp(prop, "word-spacing") == 0) {
        bool letter = prop[0] == 'l' || prop[0] == 'L';
        const char *v = value;
        while (isspace((unsigned char)*v)) v++;
        float px = 0.0f;
        bool ok = true;
        if (strncasecmp(v, "normal", 6) != 0) {
            char *end = NULL;
            double num = strtod(v, &end);
            if (end == v) ok = false;
            else if (num != 0.0 && !isalpha((unsigned char)*end)) ok = false;   /* (a length, not a number) */
            else px = (float)css_parse_length(v, parent_width, viewport_width, box->s->font_size);
        }
        if (ok && letter) { box->s->letter_spacing = px; box->s->letter_spacing_set = 1; }
        else if (ok) { box->s->word_spacing = px; box->s->word_spacing_set = 1; }
    } else if (strcasecmp(prop, "text-indent") == 0) {
        const char *v = value;
        while (isspace((unsigned char)*v)) v++;
        char *end = NULL;
        double num = strtod(v, &end);
        if (end != v) {
            while (*end && isspace((unsigned char)*end)) end++;
            if (*end == '%') {
                box->s->text_indent = 0.0f;
                box->s->text_indent_pct = (float)(num / 100.0);
                box->s->text_indent_set = 1;
            } else if (num == 0.0 || isalpha((unsigned char)*end)) {
                box->s->text_indent = (float)css_parse_length(v, parent_width, viewport_width, box->s->font_size);
                box->s->text_indent_pct = 0.0f;
                box->s->text_indent_set = 1;
            }
        }
    } else if (strcasecmp(prop, "text-transform") == 0) {
        const char *v = value;
        while (isspace((unsigned char)*v)) v++;
        int t = -1;
        if (!strcasecmp(v, "none")) t = 0;
        else if (!strcasecmp(v, "uppercase")) t = 1;
        else if (!strcasecmp(v, "lowercase")) t = 2;
        else if (!strcasecmp(v, "capitalize")) t = 3;
        if (t >= 0) { box->s->text_transform = (unsigned char)t; box->s->text_transform_set = 1; }
    } else if (strcasecmp(prop, "quotes") == 0) {
        const char *v = value;
        while (isspace((unsigned char)*v)) v++;
        size_t vl = strlen(v);
        while (vl && isspace((unsigned char)v[vl - 1])) vl--;
        box->s->quotes = vl == 4 && !strncasecmp(v, "none", 4) ? "" : layout_intern(v, vl);
        box->s->quotes_set = 1;
    } else if (strcasecmp(prop, "list-style-type") == 0 ||
               strcasecmp(prop, "list-style") == 0) {
        unsigned char t = 0;
        if (strstr(value, "disc")) t = 1;
        else if (strstr(value, "circle")) t = 2;
        else if (strstr(value, "square")) t = 3;
        else if (strstr(value, "decimal")) t = 4;
        else if (strstr(value, "none")) t = 0;
        box->s->list_style_type = t;
        box->s->list_style_type_set = 1;
    } else if (strcasecmp(prop, "border") == 0 ||
               strcasecmp(prop, "border-left") == 0 ||
               strcasecmp(prop, "border-right") == 0 ||
               strcasecmp(prop, "border-top") == 0 ||
               strcasecmp(prop, "border-bottom") == 0) {
        /* Shorthand: a width, a style and a color, in any order; the parts
         * left out are reset (width medium, style none, currentColor). */
        int sides = 0x0F;
        if (strstr(prop, "left")) sides = 0x01;
        else if (strstr(prop, "right")) sides = 0x02;
        else if (strstr(prop, "top")) sides = 0x04;
        else if (strstr(prop, "bottom")) sides = 0x08;
        const char *p = value;
        char token[256];
        int style = CSS_BORDER_NONE, nparts = 0;
        double w = -1.0;
        const char *color = NULL;
        char color_buf[256];
        bool ok = true;
        while (ok && css_next_value(&p, token, sizeof(token))) {
            nparts++;
            int code = css_border_style_code(token);
            if (code >= 0) { style = code; continue; }
            double kw = !strcasecmp(token, "thin") ? 1.0 : !strcasecmp(token, "medium") ? 3.0
                      : !strcasecmp(token, "thick") ? 5.0 : -1.0;
            if (kw >= 0.0) { w = kw; continue; }
            if (isdigit((unsigned char)token[0]) || token[0] == '.' ||
                ((token[0] == '+' || token[0] == '-') && (isdigit((unsigned char)token[1]) || token[1] == '.'))) {
                double v = css_parse_length(token, parent_width, viewport_width, box->s->font_size);
                if (v < 0.0) ok = false;
                else w = v;
                continue;
            }
            snprintf(color_buf, sizeof(color_buf), "%s", token);
            color = color_buf;
        }
        if (ok && nparts > 0) {
            double cr, cg, cb, ca;
            if (color && strncasecmp(color, "currentcolor", 12) != 0 && !css_parse_color(color, &cr, &cg, &cb, &ca))
                ok = false;
        }
        if (ok && nparts > 0) {
            if (w >= 0.0) box->s->border_width_set |= (unsigned char)sides;
            else box->s->border_width_set &= (unsigned char)~sides;
            if (sides & 0x01) box->s->border_left = w >= 0.0 ? w : 3.0;
            if (sides & 0x02) box->s->border_right = w >= 0.0 ? w : 3.0;
            if (sides & 0x04) box->s->border_top = w >= 0.0 ? w : 3.0;
            if (sides & 0x08) box->s->border_bottom = w >= 0.0 ? w : 3.0;
            border_set_style(box, sides, style);
            border_set_color(box, sides, color ? color : "currentcolor");
        }
    } else if (strcasecmp(prop, "border-width") == 0) {
        layout_apply_shorthand_sides(box, value, parent_width, viewport_width,
                                     &box->s->border_top, &box->s->border_right,
                                     &box->s->border_bottom, &box->s->border_left, NULL);
        box->s->border_width_set = 0x0F;
    } else if (strcasecmp(prop, "border-style") == 0) {
        /* 1-4 values: top, right, bottom, left. */
        int codes[4], n = 0;
        const char *p = value;
        char token[64];
        bool ok = true;
        while (css_next_value(&p, token, sizeof(token))) {
            int code = n < 4 ? css_border_style_code(token) : -1;
            if (code < 0) { ok = false; break; }
            codes[n++] = code;
        }
        for (int i = 0; ok && i < n; i++) border_set_style(box, border_list_sides(n, i), codes[i]);
    } else if (strcasecmp(prop, "border-color") == 0 ||
               strcasecmp(prop, "border-left-color") == 0 ||
               strcasecmp(prop, "border-right-color") == 0 ||
               strcasecmp(prop, "border-top-color") == 0 ||
               strcasecmp(prop, "border-bottom-color") == 0) {
        if (strcasecmp(prop, "border-color") == 0) {
            char vals[4][128];
            int n = 0;
            const char *p = value;
            char token[128];
            bool ok = true;
            while (css_next_value(&p, token, sizeof(token))) {
                if (n == 4) { ok = false; break; }
                snprintf(vals[n++], sizeof(vals[0]), "%s", token);
            }
            /* (all valid, or none applies) */
            for (int i = 0; ok && i < n; i++) {
                double cr, cg, cb, ca;
                if (strncasecmp(vals[i], "currentcolor", 12) != 0 && !css_parse_color(vals[i], &cr, &cg, &cb, &ca)) ok = false;
            }
            for (int i = 0; ok && i < n; i++) border_set_color(box, border_list_sides(n, i), vals[i]);
        } else {
            int side = strstr(prop, "left") ? 1 : strstr(prop, "right") ? 2 : strstr(prop, "top") ? 4 : 8;
            border_set_color(box, side, value);
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
static uint32_t cp_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

/* Position of `name` in m, or -1. */
static int cp_find(const CssCustomProps *m, const char *name)
{
    if (!m->index) {
        for (int i = 0; i < m->count; i++)
            if (strcmp(m->props[i].name, name) == 0) return i;
        return -1;
    }
    uint32_t mask = m->index_cap - 1, h = cp_hash(name) & mask;
    for (;;) {
        uint32_t v = m->index[h];
        if (!v) return -1;
        if (strcmp(m->props[v - 1].name, name) == 0) return (int)v - 1;
        h = (h + 1) & mask;
    }
}

static void cp_index_insert(CssCustomProps *m, int pos)
{
    uint32_t mask = m->index_cap - 1, h = cp_hash(m->props[pos].name) & mask;
    while (m->index[h]) h = (h + 1) & mask;
    m->index[h] = (uint32_t)pos + 1;
}

/* Give m an index sized for its capacity (maps of a few entries scan). */
static void cp_index_build(FrameArena *a, CssCustomProps *m, const CssCustomProps *base)
{
    m->index = NULL;
    m->index_cap = 0;
    if (m->capacity < 16) return;
    uint32_t cap = 32;
    while (cap < 2u * (uint32_t)m->capacity) cap *= 2;
    m->index = (uint32_t *)frame_arena_alloc(a, cap * sizeof(uint32_t));
    m->index_cap = cap;
    if (base && base->index && base->index_cap == cap && base->count == m->count) {
        memcpy(m->index, base->index, cap * sizeof(uint32_t));   /* same entries, same slots */
        return;
    }
    memset(m->index, 0, cap * sizeof(uint32_t));
    for (int i = 0; i < m->count; i++) cp_index_insert(m, i);
}

static const char* css_custom_props_get(const CssCustomProps *props, const char *name)
{
    if (!props || !name) return NULL;
    int i = cp_find(props, name);
    return i >= 0 ? props->props[i].value : NULL;
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
    cp_index_build(a, m, base);
    return m;
}

/* Replace or append (legacy css_custom_props_set semantics). */
static void css_custom_props_put(FrameArena *a, CssCustomProps *m, const char *name, const char *value)
{
    int i = cp_find(m, name);
    if (i >= 0) {
        m->props[i].value = frame_arena_strdup(a, value);
        return;
    }
    if (m->count >= m->capacity) return;   /* capacity = number of custom declarations */
    m->props[m->count].name = frame_arena_strdup(a, name);
    m->props[m->count].value = frame_arena_strdup(a, value);
    if (m->index) cp_index_insert(m, m->count);
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
    uint64_t src;               /* fetch id of a URL sheet's text (0 = none) */
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
    uint64_t hash;              /* precomputed sheet_hash(text) when has_hash */
    bool has_hash;
    uint64_t src;               /* URL sheet fetch id: same id, same text */
    bool ua;                    /* the user agent's sheet */
} SheetItem;

typedef struct LayoutStyleSheetList {
    CssStylesheet **sheets;     /* final, in cascade order */
    SheetEntry **entries;       /* parallel to sheets (NULL for uncached) */
    int count;
    int capacity;
    SheetItem *items;
    int nitems, cap_items;
    bool all_cached;
    bool incomplete;            /* a linked sheet could not be fetched (try again next layout) */
} LayoutStyleSheetList;

/* Sheet texts run to megabytes and are hashed on every layout: eight bytes
 * a step (only compared within the process; hits are confirmed by memcmp). */
static uint64_t sheet_hash(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull ^ (n * 0x9E3779B97F4A7C15ull);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, s + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ull; }
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

static SheetEntry *sheet_cache_find(uint64_t h, const char *text, size_t len, const char *host, uint64_t src)
{
    for (int i = 0; i < g_sheet_cache_n; i++) {
        SheetEntry *e = g_sheet_cache[i];
        if (e->hash != h || e->len != len) continue;
        if ((e->host == NULL) != (host == NULL)) continue;
        if (host && strcmp(e->host, host) != 0) continue;
        /* A URL sheet's text is the same for one fetch: no compare. */
        if (!(src && e->src == src) && memcmp(e->text, text, len) != 0) continue;
        if (src && !e->src) e->src = src;
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
        hashes[i] = it->has_hash ? it->hash : sheet_hash(it->text, it->len);
        SheetEntry *e = sheet_cache_find(hashes[i], it->text, it->len, it->host, it->src);
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
        if (sheet && it->ua) sheet->user_agent = true;
        if (sheet) {
            it->entry = sheet_cache_insert(hashes[task_item[t]], it->text, it->len, it->host, sheet);
            if (!it->entry) it->parsed = sheet;
            else it->entry->src = it->src;
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
                it->entry = sheet_cache_find(hashes[i], it->text, it->len, it->host, it->src);
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
    uint64_t gen;               /* build number: identifies this set for the style cache */
    /* Built by extending the set of generation base_gen (base_nrules
     * rules) with more sheets; 0 when compiled from scratch. */
    uint64_t base_gen;
    int base_nrules;
} StyleSetEntry;
static uint64_t g_styleset_gen;

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
static CssStyleSet *layout_styleset_get(LayoutStyleSheetList *list, double vw, double vh, bool *owned,
                                       uint64_t *gen)
{
    *owned = false;
    *gen = 0;
    if (!list->all_cached) {
        *owned = true;
        return css_styleset_build(list->sheets, list->count, vw, vh);
    }
    for (int i = 0; i < STYLESET_CACHE_MAX; i++) {
        StyleSetEntry *c = &g_styleset_cache[i];
        if (!c->set || c->n != list->count || c->vw != vw || c->vh != vh) continue;
        if (memcmp(c->sheets, list->entries, (size_t)list->count * sizeof(SheetEntry *)) != 0) continue;
        c->last_use = g_layout_clock;
        *gen = c->gen;
        return c->set;
    }
    int victim = 0;
    for (int i = 0; i < STYLESET_CACHE_MAX; i++) {
        if (!g_styleset_cache[i].set) { victim = i; break; }
        if (g_styleset_cache[i].last_use < g_styleset_cache[victim].last_use) victim = i;
    }
    if (g_css_log > 1) {
        /* What changed against the most recently used set. */
        int last = -1;
        for (int i = 0; i < STYLESET_CACHE_MAX; i++)
            if (g_styleset_cache[i].set && (last < 0 || g_styleset_cache[i].last_use > g_styleset_cache[last].last_use)) last = i;
        if (last >= 0) {
            StyleSetEntry *o = &g_styleset_cache[last];
            int same = 0;
            for (int k = 0; k < list->count; k++)
                for (int m = 0; m < o->n; m++)
                    if (o->sheets[m] == list->entries[k]) { same++; break; }
            fprintf(stderr, "[css] style set miss: %d sheets (was %d), %d kept, viewport %s\n",
                    list->count, o->n, same, o->vw != vw || o->vh != vh ? "changed" : "same");
            for (int k = 0; k < list->count; k++) {
                bool found = false;
                for (int m = 0; m < o->n && !found; m++) found = o->sheets[m] == list->entries[k];
                if (!found) fprintf(stderr, "[css]   new sheet %d: %zu bytes%s%s: %.80s\n", k, list->entries[k]->len,
                                    list->entries[k]->host ? " in " : "", list->entries[k]->host ? list->entries[k]->host : "",
                                    list->entries[k]->text ? list->entries[k]->text : "");
            }
        }
    }
    /* Pages add sheets as their components load: extend the cached set
     * whose sheets begin the list (the longest such) rather than compile
     * every rule again.  CSS_EXTEND=0: always from scratch. */
    static int extend_env = -1, verify_env = -1;
    if (extend_env < 0) { const char *e = getenv("CSS_EXTEND"); extend_env = !(e && strcmp(e, "0") == 0); }
    if (verify_env < 0) verify_env = getenv("CSS_EXTEND_VERIFY") ? 1 : 0;
    int base = -1;
    for (int i = 0; extend_env && i < STYLESET_CACHE_MAX; i++) {
        StyleSetEntry *c = &g_styleset_cache[i];
        if (!c->set || c->n > list->count || c->vw != vw || c->vh != vh) continue;
        if (memcmp(c->sheets, list->entries, (size_t)c->n * sizeof(SheetEntry *)) != 0) continue;
        if (base < 0 || c->n > g_styleset_cache[base].n) base = i;
    }
    CssStyleSet *set;
    uint64_t base_gen = 0;
    int base_nrules = 0;
    if (base >= 0) {
        const CssStyleSet *from = g_styleset_cache[base].set;
        set = css_styleset_extend(from, list->sheets, list->count, vw, vh);
        base_gen = g_styleset_cache[base].gen;
        base_nrules = css_styleset_rule_count(from);
        if (g_css_log > 0)
            fprintf(stderr, "[css] style set extended: %d sheets on %d (%d rules on %d)\n", list->count,
                    g_styleset_cache[base].n, css_styleset_rule_count(set), base_nrules);
        if (verify_env) {
            CssStyleSet *full = css_styleset_build(list->sheets, list->count, vw, vh);
            int c1, u1, c2, u2;
            css_styleset_stats(set, &c1, &u1);
            css_styleset_stats(full, &c2, &u2);
            bool same = c1 == c2 && u1 == u2 && css_styleset_rule_count(set) == css_styleset_rule_count(full) &&
                        css_style_reval_count(set) == css_style_reval_count(full);
            for (int r = 0; same && r < css_styleset_rule_count(set); r++)
                same = css_styleset_rule(set, r) == css_styleset_rule(full, r) &&
                       css_styleset_rule_order(set, r) == css_styleset_rule_order(full, r);
            fprintf(stderr, "[css] style set extension %s a full build\n", same ? "matches" : "DIFFERS FROM");
            css_styleset_free(full);
        }
    } else {
        set = css_styleset_build(list->sheets, list->count, vw, vh);
    }
    /* The base was extended into this set: a page only adds sheets, so it
     * is not asked for again.  It goes rather than the least recently
     * used (it holds as many compiled rules, nearly). */
    if (base >= 0) victim = base;
    /* Evicted only now: the base may be the least recently used. */
    styleset_cache_drop(victim);
    StyleSetEntry *c = &g_styleset_cache[victim];
    c->n = list->count;
    c->sheets = (SheetEntry **)malloc(((size_t)list->count + 1) * sizeof(SheetEntry *));
    memcpy(c->sheets, list->entries, (size_t)list->count * sizeof(SheetEntry *));
    for (int k = 0; k < c->n; k++) c->sheets[k]->refs++;
    c->vw = vw;
    c->vh = vh;
    c->set = set;
    c->last_use = g_layout_clock;
    c->gen = ++g_styleset_gen;
    c->base_gen = base_gen;
    c->base_nrules = base_nrules;
    *gen = c->gen;
    return c->set;
}

/* The generation a style set of generation `gen` extends, and the rules it
 * kept from it (false: compiled from scratch, or no longer cached). */
static bool styleset_base_of(uint64_t gen, uint64_t *base_gen, int *base_nrules)
{
    for (int i = 0; i < STYLESET_CACHE_MAX; i++) {
        const StyleSetEntry *c = &g_styleset_cache[i];
        if (c->set && c->gen == gen && c->base_gen) {
            *base_gen = c->base_gen;
            *base_nrules = c->base_nrules;
            return true;
        }
    }
    return false;
}

static char* layout_resolve_url(const char *base_url, const char *href) {
    if (!href || !href[0]) return NULL;
    if (url_has_scheme(href)) return strdup(href);
    if (base_url && url_has_scheme(base_url)) {
        char *abs = wurl_resolve(base_url, href);
        if (abs) return abs;
    }
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
typedef struct UrlSheet { char *url; char *text; size_t len; uint64_t hash; uint64_t id; const char *enc; } UrlSheet;
static uint64_t sheet_hash(const char *s, size_t n);
#define URL_SHEET_MAX 64
static UrlSheet g_url_sheets[URL_SHEET_MAX];
static int g_url_sheet_n, g_url_sheet_next;

/* The cached sheet for `url`, fetching it on first use (NULL on failure):
 * its text as UTF-8, decoded from its bytes' encoding (fallback_label: the
 * referrer's, when nothing in the response says). */
static const UrlSheet *url_sheet_get_ex(const char *url, const char *fallback_label) {
    for (int i = 0; i < g_url_sheet_n; i++)
        if (strcmp(g_url_sheets[i].url, url) == 0) return &g_url_sheets[i];

    /* A prefetched download (waiting for it if still in flight), else a
     * download now. */
    HttpBuffer buffer = {0};
    char err[256] = {0};
    bool ok = dom_fetch_script(url, &buffer, err, sizeof(err));
    const UrlSheet *out = NULL;
    if (ok && buffer.data && buffer.size > 0) {
        LOG_INFO("Fetched stylesheet (%zu bytes)", buffer.size);
        UrlSheet *u = &g_url_sheets[g_url_sheet_next];
        if (g_url_sheet_n == URL_SHEET_MAX) { free(u->url); free(u->text); }
        else g_url_sheet_n++;
        g_url_sheet_next = (g_url_sheet_next + 1) % URL_SHEET_MAX;
        u->url = strdup(url);
        /* (a response that is not CSS applies nothing) */
        u->text = css_sheet_type_ok(buffer.headers, buffer.headers_size)
                  ? css_sheet_decode(buffer.data, buffer.size, buffer.headers, buffer.headers_size, fallback_label,
                                     &u->len, &u->enc)
                  : NULL;
        if (!u->text) { u->text = strdup(""); u->len = 0; }
        u->hash = sheet_hash(u->text, u->len);
        static uint64_t s_fetches;
        u->id = ++s_fetches;
        /* Its @font-face rules, once per fetch (relative to the sheet). */
        font_registry_scan_css(u->text, u->len, url);
        out = u;
    } else {
        LOG_WARN("Failed to fetch stylesheet %.80s: %s", url, err[0] ? err : "unknown");
    }
    http_free_buffer(&buffer);
    return out;
}

static const UrlSheet *url_sheet_get(const char *url) {
    return url_sheet_get_ex(url, NULL);
}

bool css_layout_fetch_stylesheet_url(const char *url) {
    return url && url[0] && url_sheet_get(url) != NULL;
}

bool css_layout_stylesheet_cached(const char *url) {
    for (int i = 0; url && i < g_url_sheet_n; i++)
        if (strcmp(g_url_sheets[i].url, url) == 0) return true;
    return false;
}

/* The base layout resolves sheet and image URLs against: the page's URL
 * (its origin when the URL does not fit). */
static void layout_origin_base(char *out, size_t cap)
{
    extern const char *g_cyber_start_url;
    const char *src = (g_cyber_start_url && g_cyber_start_url[0]) ? g_cyber_start_url : "https://www.youtube.com/";
    if (strlen(src) < cap) {
        memcpy(out, src, strlen(src) + 1);
        return;
    }
    const char *scheme = strstr(src, "://");
    if (scheme) {
        const char *host = scheme + 3;
        const char *slash = strchr(host, '/');
        size_t origin_len = slash ? (size_t)(slash - src) + 1 : strlen(src);
        if (origin_len >= cap) origin_len = cap - 1;
        memcpy(out, src, origin_len);
        out[origin_len] = '\0';
        if (!slash && origin_len + 1 < cap) {
            out[origin_len] = '/';
            out[origin_len + 1] = '\0';
        }
    } else {
        strncpy(out, src, cap - 1);
        out[cap - 1] = '\0';
    }
}

/* The location of the layout's realm, as its base URL (a frame's
 * document resolves against its own page). */
static void layout_realm_base(LayoutContext *ctx)
{
    if (!SAFE_VALID(ctx->js_ctx) || gc_handle_get_type(ctx->js_ctx) != JS_GC_OBJ_TYPE_JS_CONTEXT) return;
    GCValue loc = JS_GetPropertyStr(ctx->js_ctx, JS_GetGlobalObject(ctx->js_ctx), "location");
    GCValue href = JS_IsObject(loc) ? JS_GetPropertyStr(ctx->js_ctx, loc, "href") : JS_UNDEFINED;
    if (!JS_IsString(href)) return;
    const char *s = JS_ToCString(ctx->js_ctx, href);
    if (s && url_has_scheme(s) && strncmp(s, "about:", 6) != 0 && strlen(s) < sizeof(ctx->base_url))
        snprintf(ctx->base_url, sizeof(ctx->base_url), "%s", s);
}

/* Prefetched sheets not taken into the cache yet. */
#define SHEET_PREFETCH_MAX 64
static char *g_sheet_prefetch[SHEET_PREFETCH_MAX];
static int g_sheet_prefetch_n;

void css_layout_prefetch_stylesheet(const char *href)
{
    char base[512];
    layout_origin_base(base, sizeof(base));
    char *url = layout_resolve_url(base, href);
    if (!url) return;
    bool known = false;
    for (int i = 0; i < g_url_sheet_n && !known; i++) known = strcmp(g_url_sheets[i].url, url) == 0;
    for (int i = 0; i < g_sheet_prefetch_n && !known; i++) known = strcmp(g_sheet_prefetch[i], url) == 0;
    if (!known && url_is_network_url(url) && g_sheet_prefetch_n < SHEET_PREFETCH_MAX) {
        dom_fetch_prefetch_first(url);
        g_sheet_prefetch[g_sheet_prefetch_n++] = url;
        return;
    }
    free(url);
}

void css_layout_install_prefetched_sheets(void)
{
    int w = 0;
    for (int i = 0; i < g_sheet_prefetch_n; i++) {
        char *url = g_sheet_prefetch[i];
        if (dom_fetch_ready(url)) {
            url_sheet_get(url);
            free(url);
        } else {
            g_sheet_prefetch[w++] = url;
        }
    }
    g_sheet_prefetch_n = w;
}

/* A prefetched stylesheet still downloading (render-blocking: the first
 * paint waits for the page's sheets, as browsers' does). */
bool css_layout_stylesheets_pending(void)
{
    for (int i = 0; i < g_sheet_prefetch_n; i++)
        if (dom_fetch_in_flight(g_sheet_prefetch[i])) return true;
    return false;
}

/* Queue a fetched sheet's text (borrowed while the cache has room). */
static void layout_sheet_add_url(LayoutStyleSheetList *list, const UrlSheet *u)
{
    if (g_url_sheet_n < URL_SHEET_MAX) {
        /* The cache cannot evict while it has room: borrow its text and
         * hash instead of copying and rehashing megabytes per layout. */
        SheetItem *it = layout_sheet_item_new(list);
        it->text = u->text;
        it->len = u->len;
        it->hash = u->hash;
        it->has_hash = true;
        it->src = u->id;
    } else {
        layout_sheet_list_add_text(list, u->text, u->len, NULL);
    }
}

/* The @import rules a sheet starts with (after any @charset): each sheet
 * whose media list matches, fetched relative to base (enc: the importing
 * sheet's encoding, for an imported sheet that names none), queued with its
 * own imports before it; all before the importing sheet's rules. */
static void layout_sheet_imports(LayoutStyleSheetList *list, const char *text, size_t len, const char *base,
                                 const char *enc, int depth)
{
    if (!text || depth > 8) return;
    const char *p = text, *end = text + len;
    for (;;) {
        for (;;) {
            while (p < end && isspace((unsigned char)*p)) p++;
            if (end - p >= 2 && p[0] == '/' && p[1] == '*') {
                const char *c = p + 2;
                while (c + 1 < end && !(c[0] == '*' && c[1] == '/')) c++;
                p = c + 1 < end ? c + 2 : end;
                continue;
            }
            if (end - p >= 4 && !memcmp(p, "<!--", 4)) { p += 4; continue; }
            if (end - p >= 3 && !memcmp(p, "-->", 3)) { p += 3; continue; }
            if (end - p >= 9 && !memcmp(p, "<![CDATA[", 9)) { p += 9; continue; }
            if (end - p >= 3 && !memcmp(p, "]]>", 3)) { p += 3; continue; }
            break;
        }
        if (p >= end) break;
        bool charset = end - p >= 8 && !strncasecmp(p, "@charset", 8);
        if (!charset && !(end - p >= 8 && !strncasecmp(p, "@import", 7) &&
                          (isspace((unsigned char)p[7]) || p[7] == '"' || p[7] == '\''))) {
            /* (an invalid rule or an unknown at-rule before it: passed over) */
            bool counts;
            size_t e = css_sheet_item_skip(text, len, (size_t)(p - text), &counts);
            if (counts || e <= (size_t)(p - text)) break;
            p = text + e;
            continue;
        }
        p += charset ? 8 : 7;
        while (p < end && isspace((unsigned char)*p)) p++;
        char href[1024];
        href[0] = 0;
        if (!charset) {
            const char *a = NULL, *b = NULL;
            if (end - p >= 4 && !strncasecmp(p, "url(", 4)) {
                a = p + 4;
                while (a < end && isspace((unsigned char)*a)) a++;
                if (a < end && (*a == '"' || *a == '\'')) {
                    char q = *a++;
                    b = a;
                    while (b < end && *b != q) b++;
                    p = b < end ? b + 1 : end;
                    while (p < end && *p != ')') p++;
                } else {
                    b = a;
                    while (b < end && *b != ')' && !isspace((unsigned char)*b)) b++;
                    p = b;
                    while (p < end && *p != ')') p++;
                }
                if (p < end) p++;
            } else if (p < end && (*p == '"' || *p == '\'')) {
                char q = *p++;
                a = b = p;
                while (b < end && *b != q) b++;
                p = b < end ? b + 1 : end;
            }
            if (a && b > a && (size_t)(b - a) < sizeof(href)) {
                memcpy(href, a, (size_t)(b - a));
                href[b - a] = 0;
            }
        }
        /* The media list: to the ';'. */
        const char *ms = p;
        int paren = 0;
        while (p < end && !(*p == ';' && paren == 0)) {
            if (*p == '(') paren++;
            else if (*p == ')' && paren > 0) paren--;
            else if (*p == '{') return;   /* (a block: not a statement) */
            p++;
        }
        const char *me = p;
        if (p < end) p++;
        if (charset || !href[0]) continue;
        while (ms < me && isspace((unsigned char)*ms)) ms++;
        while (me > ms && isspace((unsigned char)me[-1])) me--;
        if (me > ms) {
            char media[512];
            size_t ml = (size_t)(me - ms) < sizeof(media) - 1 ? (size_t)(me - ms) : sizeof(media) - 1;
            memcpy(media, ms, ml);
            media[ml] = 0;
            if (!css_media_query_matches(media, g_unit_vw, g_unit_vh)) continue;
        }
        char *url = layout_resolve_url(base, href);
        if (!url) continue;
        const UrlSheet *u = url_sheet_get_ex(url, enc);
        if (u) {
            layout_sheet_imports(list, u->text, u->len, url, u->enc, depth + 1);
            layout_sheet_add_url(list, u);
        }
        free(url);
    }
}

static void layout_fetch_stylesheet(LayoutStyleSheetList *list, const char *base_url, const char *href,
                                    const char *charset) {
    char *url = layout_resolve_url(base_url, href);
    if (!url) return;
    const UrlSheet *u = url_sheet_get_ex(url, charset);
    if (u) layout_sheet_imports(list, u->text, u->len, url, u->enc, 0);
    if (u && g_url_sheet_n < URL_SHEET_MAX) {
        /* The cache cannot evict while it has room: borrow its text and
         * hash instead of copying and rehashing megabytes per layout. */
        SheetItem *it = layout_sheet_item_new(list);
        it->text = u->text;
        it->len = u->len;
        it->hash = u->hash;
        it->has_hash = true;
        it->src = u->id;
    } else if (u) {
        layout_sheet_list_add_text(list, u->text, u->len, NULL);
    } else {
        list->incomplete = true;
    }
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

/* A <style> or <link> whose sheet scripts changed through the CSSOM
 * (cssom.js): the sheet's serialization (malloc'd); NULL otherwise. */
static char *layout_cssom_sheet_text(LayoutContext *ctx, HtmlNode *node) {
    if (!node->store_id || !SAFE_VALID(ctx->js_ctx) ||
        gc_handle_get_type(ctx->js_ctx) != JS_GC_OBJ_TYPE_JS_CONTEXT) return NULL;
    JSContextHandle js = ctx->js_ctx;
    GCValue el = dom_js_existing(node->store_id);
    if (!JS_IsObject(el)) return NULL;
    GCValue sh = JS_GetPropertyStr(js, el, "__cyberSheet");
    if (!JS_IsObject(sh) || !JS_ToBool(js, JS_GetPropertyStr(js, sh, "__cyberModified"))) return NULL;
    GCValue fn = JS_GetPropertyStr(js, sh, "__cyberText");
    if (!JS_IsFunction(js, fn)) return NULL;
    GCValue t = JS_Call(js, fn, sh, 0, NULL);
    if (JS_IsException(t)) { JS_GetException(js); return NULL; }
    const char *s = JS_ToCString(js, t);
    return strdup(s ? s : "");
}

/* The sheets of the document subtree at `dom_idx` (the document, not the
 * layout tree: that may leave display:none subtrees out). */
static void layout_collect_stylesheets_recursive(LayoutContext *ctx, int dom_idx,
                                                  LayoutStyleSheetList *list,
                                                  const char *base_url) {
    PreOrderCompactionArray *da = &ctx->doc->array;
    if (dom_idx < 0 || !po_array_is_active(da, dom_idx)) return;
    HtmlNode *node = layout_node_dom(ctx, dom_idx);
    if (!node || node->type != HTML_NODE_ELEMENT) goto next;

    char *cssom = (strcasecmp(node->tag_name, "style") == 0 || strcasecmp(node->tag_name, "link") == 0)
                   ? layout_cssom_sheet_text(ctx, node) : NULL;
    if (cssom) {
        const char *host = layout_find_host_tag(ctx, dom_idx);
        layout_sheet_list_add_text(list, cssom, strlen(cssom), host);
        font_registry_scan_css(cssom, strlen(cssom), base_url);
        free(cssom);
    } else if (strcasecmp(node->tag_name, "style") == 0 && node->text_content && node->text_content[0]) {
        /* If this <style> lives inside a stamped shadow root, scope :host,
         * ::slotted, etc. to the host custom element.  The text is borrowed:
         * the document outlives the sheet list. */
        size_t len = node->text_len ? strnlen(node->text_content, node->text_len) : strlen(node->text_content);
        const char *host = layout_find_host_tag(ctx, dom_idx);
        if (!host) layout_sheet_imports(list, node->text_content, len, base_url, NULL, 0);
        SheetItem *it = layout_sheet_item_new(list);
        it->text = node->text_content;
        it->len = len;
        it->host = host ? strdup(host) : NULL;
        font_registry_scan_css(node->text_content, len, base_url);
    } else if (strcasecmp(node->tag_name, "link") == 0) {
        const char *rel = NULL;
        const char *href = NULL;
        const char *charset = NULL;
        for (HtmlAttribute *a = node->attributes; a; a = a->next) {
            if (strcasecmp(a->name, "rel") == 0) rel = a->value;
            else if (strcasecmp(a->name, "href") == 0) href = a->value;
            else if (strcasecmp(a->name, "charset") == 0) charset = a->value;
        }
        if (rel && href && strcasecmp(rel, "stylesheet") == 0) {
            layout_fetch_stylesheet(list, base_url, href, charset);
        }
    }

next:
    for (int c = po_array_first_child(da, dom_idx); c >= 0; c = po_array_next_sibling(da, c))
        layout_collect_stylesheets_recursive(ctx, c, list, base_url);
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

/* A shadow root's adopted sheet scoped to its host: every style rule's
 * selectors prefixed with the host's scope class (":host" becomes the
 * class itself, ":host(x)" the class and x); at-rule preludes are kept, rules inside @media, @supports,
 * @container and @layer are scoped, and the contents of @keyframes,
 * @font-face and @page blocks and of style rules (nested rules included)
 * are copied as they are.  (Braces inside strings and comments are not
 * told apart.) */
static char *layout_scope_sheet(const char *css, const char *scope_class) {
    size_t n = strlen(css), cap = n * 2 + 64, o = 0, cl = strlen(scope_class);
    char *out = malloc(cap);
    bool opaque[64];            /* per open block: its contents copied as they are */
    int depth = 0;
    size_t seg = 0;             /* the text since the last { } or ; */
#define OUT(p, len) do { size_t l_ = (len); if (o + l_ + 1 > cap) { while (o + l_ + 1 > cap) cap *= 2; out = realloc(out, cap); } memcpy(out + o, (p), l_); o += l_; } while (0)
#define OPEN(op) do { if (depth < 64) opaque[depth] = (op); depth++; } while (0)
    for (size_t i = 0; i < n; i++) {
        char c = css[i];
        if (c != '{' && c != '}' && c != ';') continue;
        bool in_opaque = depth > 0 && opaque[depth - 1 < 64 ? depth - 1 : 63];
        if (c != '{' || in_opaque) {
            OUT(css + seg, i + 1 - seg);
            seg = i + 1;
            if (c == '}') { if (depth > 0) depth--; }
            else if (c == '{') OPEN(true);
            continue;
        }
        /* a prelude: [seg, i) */
        size_t b = seg, e = i;
        while (b < e && isspace((unsigned char)css[b])) b++;
        while (e > b && isspace((unsigned char)css[e - 1])) e--;
        OUT(css + seg, b - seg);
        if (b == e || css[b] == '@') {
            OUT(css + b, i + 1 - b);
            OPEN(b < e && (!strncasecmp(css + b, "@keyframes", 10) || !strncasecmp(css + b, "@-webkit-keyframes", 18) ||
                           !strncasecmp(css + b, "@font-face", 10) || !strncasecmp(css + b, "@page", 5)));
            seg = i + 1;
            continue;
        }
        /* the selector list, each selector prefixed */
        bool first = true;
        for (size_t p = b; p <= e; ) {
            size_t q = p;
            int paren = 0;
            while (q < e && (css[q] != ',' || paren)) { if (css[q] == '(') paren++; else if (css[q] == ')') paren--; q++; }
            size_t sb = p, se = q;
            while (sb < se && isspace((unsigned char)css[sb])) sb++;
            while (se > sb && isspace((unsigned char)css[se - 1])) se--;
            if (se > sb) {
                if (!first) OUT(", ", 2);
                first = false;
                OUT(".", 1);
                OUT(scope_class, cl);
                if (se - sb >= 6 && !strncmp(css + sb, ":host(", 6)) {
                    /* :host(x) y: the host matching x */
                    size_t k = sb + 6;
                    int d = 1;
                    while (k < se && d) { if (css[k] == '(') d++; else if (css[k] == ')') d--; if (d) k++; }
                    OUT(css + sb + 6, k - sb - 6);
                    if (k < se) OUT(css + k + 1, se - k - 1);
                } else if (se - sb >= 5 && !strncmp(css + sb, ":host", 5)) OUT(css + sb + 5, se - sb - 5);
                else { OUT(" ", 1); OUT(css + sb, se - sb); }
            }
            p = q + 1;
        }
        OUT("{", 1);
        OPEN(true);             /* (a style rule's block) */
        seg = i + 1;
    }
    OUT(css + seg, n - seg);
#undef OUT
#undef OPEN
    out[o] = '\0';
    return out;
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

    /* Shadow-root adopted stylesheets, each scoped to its host by its scope
     * class (shadow_dom_api.c gives the host __cyber_scope_id). */
    GCValue roots = JS_GetPropertyStr(js, global, "__cyber_adopting_roots");
    if (!JS_IsArray(js, roots)) return;
    uint32_t nr = 0;
    JS_ToUint32(js, &nr, JS_GetPropertyStr(js, roots, "length"));
    for (uint32_t i = 0; i < nr; i++) {
        GCValue sr = JS_GetPropertyUint32(js, roots, i);
        GCValue host = JS_IsObject(sr) ? JS_GetPropertyStr(js, sr, "host") : JS_UNDEFINED;
        NodeId hn = JS_IsObject(host) ? dom_node_of(host) : DOM_NULL;
        if (!hn || !JS_ToBool(js, JS_GetPropertyStr(js, host, "isConnected"))) continue;
        const char *sid = dom_get_attr_str(hn, "__cyber_scope_id");
        if (!sid) continue;
        char scope_class[96];
        snprintf(scope_class, sizeof scope_class, "cyber-scope-%s", sid);
        GCValue sheets = JS_GetPropertyStr(js, sr, "__adoptedStyleSheets");
        uint32_t ns = 0;
        if (!JS_IsObject(sheets)) continue;
        JS_ToUint32(js, &ns, JS_GetPropertyStr(js, sheets, "length"));
        for (uint32_t j = 0; j < ns; j++) {
            GCValue text = JS_GetPropertyStr(js, JS_GetPropertyUint32(js, sheets, j), "cssText");
            const char *css = JS_IsString(text) ? JS_ToCString(js, text) : NULL;
            if (css && css[0]) {
                char *scoped = layout_scope_sheet(css, scope_class);
                layout_sheet_list_add_text(list, scoped, strlen(scoped), NULL);
                font_registry_scan_css(scoped, strlen(scoped), ctx->base_url);
                free(scoped);
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
    "pre { display: block; font-family: monospace; margin: 1em 0; white-space: pre; }\n"
    "textarea { white-space: pre-wrap; }\n"
    /* (HTML's bidirectional text rules; dir=auto is a hint, from the text) */
    "bdi, output, [dir] { unicode-bidi: isolate; }\n"
    "bdo, bdo[dir] { unicode-bidi: isolate-override; }\n"
    "[dir=ltr i] { direction: ltr; }\n"
    "[dir=rtl i] { direction: rtl; }\n"
    "textarea[dir=auto i], pre[dir=auto i] { unicode-bidi: plaintext; }\n"
    "nobr { white-space: nowrap; }\n"
    "small { font-size: 0.833em; }\n"
    "sub { font-size: 0.833em; }\n"
    "sup { font-size: 0.833em; }\n"
    "big { font-size: 1.17em; }\n"
    "ul { display: block; margin: 1em 0; padding-left: 40px; list-style-type: disc; }\n"
    "ol { display: block; margin: 1em 0; padding-left: 40px; list-style-type: decimal; }\n"
    "ol, ul, menu { counter-reset: list-item; }\n"
    "li { display: list-item; }\n"
    "dl { display: block; margin: 1em 0; }\n"
    "dd { display: block; margin-left: 40px; }\n"
    "blockquote { display: block; margin: 1em 40px; }\n"
    "figure { display: block; margin: 1em 40px; }\n"
    "hr { display: block; margin: 0.5em 0; border-top: 1px solid #808080; }\n"
    "center { text-align: center; }\n"
    "caption { text-align: center; }\n"
    "table { box-sizing: border-box; }\n"
    "mark { background-color: #ffff00; color: #000000; }\n"
    "button, input, select, textarea { font-size: 13.333px; line-height: normal; }\n"
    /* (solid, not the outset / inset of HTML's sheet: controls are drawn
     * flat, as browsers' native appearance draws them, not as 3D borders) */
    "button { padding: 1px 6px; border: 2px solid #767676; background-color: #efefef; box-sizing: border-box; text-align: center; }\n"
    "input { padding: 1px 2px; border: 2px solid #767676; }\n"
    "textarea { padding: 2px; border: 1px solid #767676; }\n"
    "select { border: 1px solid #767676; box-sizing: border-box; }\n";

static bool layout_collect_document_stylesheets(LayoutContext *ctx, LayoutStyleSheetList *list,
                                                 const char *base_url) {
    if (!ctx || !list || ctx->tree.count == 0) return false;

    /* Inject the UA stylesheet first so metadata/head/template elements are
     * hidden even when the hardcoded fast-path is bypassed. */
    layout_sheet_list_add_text(list, g_ua_stylesheet_css, strlen(g_ua_stylesheet_css), NULL);
    list->items[list->nitems - 1].ua = true;

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
static bool layout_pseudo_allowed(const HtmlNode *n);

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

/* What an element's ::before (side 0), ::after (side 1) and ::first-letter
 * (side 2: content "" when any rule matched) styles say: the
 * value of the winning content declaration (NULL: no box, as for none or
 * normal) and the other declarations of the rules that matched, in cascade
 * order (importance, specificity, order).  Both point into the style set's
 * rules.  Built in a style thread's arena; a cache entry's copy is in the
 * cache's (owned). */
typedef struct PseudoSide {
    const char *content;
    int n;
    const CssDeclaration *const *decls;
} PseudoSide;
typedef struct PseudoRec {
    PseudoSide side[3];
    bool owned;
} PseudoRec;

static const PseudoRec *pseudo_rec_own(const PseudoRec *r);
static const PseudoRec *pseudo_rec_copy(const PseudoRec *r);

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
    const PseudoRec *pseudo;
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

/* A context feature of an element: the key, and for attributes the value. */
typedef struct StyleCtxItem { uint64_t k, v; } StyleCtxItem;

typedef struct StyleCtxBuf {
    StyleCtxItem *items;
    int n, cap;
    bool fail;
} StyleCtxBuf;

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
    /* Incremental restyle (see StyleCache): per layout index, the cache entry
     * of the same element (or NULL), whether its key still matches, whether
     * it must be recomputed regardless, and whether its computed style ended
     * up different from the cached one (children then recompute). */
    bool inc;
    bool verify;
    /* On a kept tree, from the last store's notes (LayoutContext.sc_entry):
     * prepare recomputes only the elements whose attributes changed (and
     * those without a note), and the pass leaves alone the style slots that
     * hold their entry's style (kept[idx] set), and the text boxes under
     * them. */
    bool retain;
    unsigned char *kept;
    _Atomic uint32_t kept_n;
    const struct StyleCacheEntry **cached;
    unsigned char *key_ok;
    unsigned char *force;
    unsigned char *out_changed;
    uint64_t *entry_key;        /* per layout index: the element's key (0 = not an element) */
    /* Per layout index, for elements whose key changed: their context
     * features (style_ctx_items) in cx at cx_off (cx_cnt -1: not computed)
     * and the hash of the rest. */
    StyleCtxBuf cx;
    int *cx_off, *cx_cnt;
    uint64_t *cx_struct;
    _Atomic uint32_t reused, recomputed, verify_mismatches, verify_reusable;
    /* Matched-properties cache: per layout index, a hash of what the cascade
     * of a recomputed element read (0: none); and how many recomputed
     * elements matched as before and took the cached box. */
    uint64_t *match_hash;
    _Atomic uint32_t matched_same;
    _Atomic uint32_t why[5];    /* CSS_LOG=2: recompute reasons (no entry, key, ancestor/sibling, :has, parent) */
    /* Per layout index: the elements in its subtree that cannot take their
     * cached style as it is (NULL: not counted, as without the cache). */
    int *dirty_n;
    _Atomic uint32_t copied;     /* elements style_copy_subtree took */
    /* Descendants of display:none elements are not styled (skip_hidden):
     * per layout index, 1 for those whose cached entry still holds, 2 for
     * the others (their entries go stale); 0 styled. */
    bool skip_hidden;
    unsigned char *skipped;
    _Atomic uint32_t skipped_n;
    /* Per parent box: the style its text children share (computed by the
     * first of them, published with a CAS; NULL until then). */
    _Atomic(LayoutStyle *) *text_styles;
    bool text_style_check;          /* CYBER_TEXT_STYLE_CHECK=1 */
    _Atomic uint32_t text_style_mismatches;
} StylePass;

/* ---- Incremental restyle ----
 *
 * Forced layouts (scripts reading geometry) restyle the whole document, yet
 * between two of them scripts usually touch a handful of elements.  The
 * style cache keeps every element's post-cascade box from the last style
 * pass, keyed by its node-store id, together with a key over what selector
 * matching can see of the element itself: tag, attributes (inline styles
 * included), parent, position among element siblings, its neighbours and
 * its child count.  An element reuses its cached box when the style set,
 * viewport and base URL are the same, its key is unchanged, no ancestor's
 * key changed (descendant selectors), no earlier sibling's key changed
 * (sibling selectors), nothing in its subtree changed when the style set
 * uses :has(), and its parent's computed style is unchanged (inheritance).
 * A recomputed element whose result equals the cached one lets its children
 * keep theirs.  Custom-property maps referenced by cached boxes are copied
 * into the cache's own arena with interned strings. */

typedef struct StyleCacheEntry {
    NodeId id;
    uint64_t key;
    /* What other elements' selectors see of it (style_ctx_items). */
    uint64_t ctx_struct;
    const struct StyleCtxItem *ctx_items;  /* in the cache arena */
    uint32_t ctx_n;
    const CssCustomProps *props;
    uint64_t match_hash;        /* what its cascade read (StylePass.match_hash), 0 = unknown */
    /* Key and context features are current but box, props and after are
     * not: the element was left unstyled (inside display:none) when its
     * style may have changed. */
    unsigned char stale;
    /* Its descendants were left out of a tree (a display:none element):
     * their entries may be stale, so they are restyled when it is next built
     * with them. */
    unsigned char pruned_below;
    unsigned char has_after;
    char after[256];
    const PseudoRec *pseudo;    /* its ::before / ::after (NULL: none) */
    uint32_t ver;               /* changes whenever style does (style slots note it) */
    LayoutStyle style;
} StyleCacheEntry;

typedef struct StyleCache {
    uint64_t global;            /* style set generation, viewport, base URL (0 = empty) */
    bool has_has;               /* the style set uses :has() */
    StyleCacheEntry *e;
    int n, cap;
    int *index;                 /* open addressing: id -> entry + 1 */
    int index_cap;
    FrameArena arena;           /* custom-property maps of cached entries */
    FrameArena retired;         /* the previous arena, kept one generation */
    uint64_t passes;            /* style passes stored (LayoutContext.style_pass) */
    uint64_t order;             /* changes when entries move (LayoutContext.sc_entry) */
} StyleCache;

static StyleCache g_sc;
static uint32_t g_sc_ver;      /* StyleCacheEntry.ver */

/* A new document in this process (a same-site navigation): its elements
 * may have the store ids the last one's had, so nothing cached for an
 * element is reused. */
void css_layout_forget_document(void)
{
    g_sc.global = 0;
}

static const StyleCacheEntry *style_cache_find(NodeId id)
{
    if (!id || !g_sc.index_cap) return NULL;
    uint32_t h = (id * 2654435761u) & (uint32_t)(g_sc.index_cap - 1);
    for (;;) {
        int v = g_sc.index[h];
        if (!v) return NULL;
        if (g_sc.e[v - 1].id == id) return &g_sc.e[v - 1];
        h = (h + 1) & (uint32_t)(g_sc.index_cap - 1);
    }
}

/* Mix n bytes into hash h, a word at a time (restyle keys hash a few
 * hundred thousand words per forced layout; a multiply per byte showed
 * up in profiles).  Only a key within this process: not stable across
 * builds. */
static uint64_t style_mix(uint64_t h, const void *p, size_t n)
{
    const unsigned char *s = (const unsigned char *)p;
    while (n >= 8) {
        uint64_t w;
        memcpy(&w, s, 8);
        h ^= w * 0x9E3779B97F4A7C15ull;
        h = ((h << 27) | (h >> 37)) * 0xBF58476D1CE4E5B9ull;
        s += 8;
        n -= 8;
    }
    for (; n; n--, s++) { h ^= *s; h *= 1099511628211ull; }
    return h;
}

static bool style_props_equal(const CssCustomProps *a, const CssCustomProps *b)
{
    if (a == b) return true;
    if (!a || !b || a->count != b->count) return false;
    for (int i = 0; i < a->count; i++) {
        if (strcmp(a->props[i].name, b->props[i].name) != 0) return false;
        if (strcmp(a->props[i].value ? a->props[i].value : "", b->props[i].value ? b->props[i].value : "") != 0)
            return false;
    }
    return true;
}

/* What :has() rules can match as their subject: the tags, ids and classes
 * of the compound selector carrying :has(...), or for a compound that names
 * none (".list :has(> x)"), those of the compound before it, which then must
 * match an ancestor; universal when neither names anything. Each key notes
 * whether its compound has more selector after it (a flip then changes
 * descendants' matching too, so they recompute with it). Sibling-relative
 * arguments (:has(+ x), :has(~ x)) are noted for the caller. */
typedef struct HasKeySet {
    uint64_t *keys;             /* hashes of 't' tag, '#' id, '.' class */
    unsigned char *deep;
    int n, cap;
} HasKeySet;

/* Everything one compound requires (tag, id, classes): an element matches
 * only with all of them. */
#define HAS_REQ_MAX 6
typedef struct HasReq {
    uint64_t keys[HAS_REQ_MAX];
    int n;
    unsigned char deep;         /* more selector after the :has() compound */
} HasReq;
typedef struct HasReqList {
    HasReq *v;
    int n, cap;
} HasReqList;

typedef struct HasSubjects {
    uint64_t gen;               /* style set generation the key sets describe */
    bool any;                   /* some rule uses :has() */
    bool universal, universal_deep;
    /* A :has() compound naming nothing is the subject (or a rule could not
     * be recorded): any element may restyle itself. */
    bool universal_self;
    bool sibling_relative;
    HasKeySet self, anc;
    /* The same compounds with all their requirements (self_req parallel to
     * subjects, anc_req to the ancestors they need). */
    HasReqList self_req, anc_req;
    /* Rules whose :has() compound names nothing and has more selector after
     * it (":has(x) [dialog]"): any element may be the subject, but only
     * elements matching the rightmost compound can change.  One feature of
     * each such compound ('t' tag, '#' id, '.' class, '[' attribute name);
     * those elements are recomputed after any change (universal_deep, the
     * whole document, when a compound names nothing). */
    HasKeySet deep_targets;
} HasSubjects;

static uint64_t has_key(char kind, const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    h = style_mix(h, &kind, 1);
    for (size_t i = 0; i < n; i++) {
        char c = (char)tolower((unsigned char)s[i]);
        h = style_mix(h, &c, 1);
    }
    return h;
}

static void has_add(HasSubjects *hs, HasKeySet *ks, uint64_t k, bool deep)
{
    for (int i = 0; i < ks->n; i++)
        if (ks->keys[i] == k) { if (deep) ks->deep[i] = 1; return; }
    if (ks->n == ks->cap) {
        int nc = ks->cap ? ks->cap * 2 : 32;
        uint64_t *nk = (uint64_t *)realloc(ks->keys, (size_t)nc * sizeof(uint64_t));
        if (nk) ks->keys = nk;
        unsigned char *nd = (unsigned char *)realloc(ks->deep, (size_t)nc);
        if (nd) ks->deep = nd;
        if (!nk || !nd) { hs->universal = hs->universal_deep = hs->universal_self = true; return; }
        ks->cap = nc;
    }
    ks->keys[ks->n] = k;
    ks->deep[ks->n++] = deep;
}

static int has_lookup(const HasKeySet *ks, uint64_t k)
{
    for (int i = 0; i < ks->n; i++) if (ks->keys[i] == k) return ks->deep[i] ? 2 : 1;
    return 0;
}

static void has_free(HasSubjects *hs)
{
    free(hs->self.keys);
    free(hs->self.deep);
    free(hs->anc.keys);
    free(hs->anc.deep);
    free(hs->self_req.v);
    free(hs->anc_req.v);
    free(hs->deep_targets.keys);
    free(hs->deep_targets.deep);
    memset(hs, 0, sizeof(*hs));
}

static const char *has_compound_start(const char *sel, const char *end);

/* One feature every element matching the rightmost compound of the complex
 * selector containing sel[at] carries (see HasSubjects.deep_targets); false
 * when that compound names none, or sel[at] is inside parentheses. */
static bool has_rightmost_key(const char *sel, const char *at, uint64_t *key)
{
    int d = 0;
    for (const char *q = sel; q < at; q++) {
        if (*q == '(' || *q == '[') d++;
        else if (*q == ')' || *q == ']') d--;
    }
    if (d != 0) return false;
    const char *end = at;
    for (d = 0; *end; end++) {
        if (*end == '(' || *end == '[') d++;
        else if (*end == ')' || *end == ']') d--;
        else if (d == 0 && (*end == ',' || *end == '{')) break;
    }
    while (end > at && end[-1] == ' ') end--;
    const char *st = has_compound_start(sel, end);
    for (const char *q = st; q < end; q++) if (*q == '\\') return false;
    const char *q = st;
    if (q < end && (isalpha((unsigned char)*q) || *q == '-' || *q == '_')) {
        const char *e = q;
        while (e < end && (isalnum((unsigned char)*e) || *e == '-' || *e == '_')) e++;
        *key = has_key('t', q, (size_t)(e - q));
        return true;
    }
    uint64_t attr = 0;
    for (d = 0; q < end; q++) {
        if (*q == '(') { d++; continue; }
        if (*q == ')') { d--; continue; }
        if (d) continue;
        if (*q == '.' || *q == '#') {
            char kind = *q;
            const char *e = q + 1;
            while (e < end && (isalnum((unsigned char)*e) || *e == '-' || *e == '_')) e++;
            if (e > q + 1) { *key = has_key(kind, q + 1, (size_t)(e - q - 1)); return true; }
        } else if (*q == '[' && !attr) {
            const char *n0 = q + 1, *e = n0;
            while (e < end && (isalnum((unsigned char)*e) || *e == '-' || *e == '_')) e++;
            if (e > n0) attr = has_key('[', n0, (size_t)(e - n0));
            while (q < end && *q != ']') q++;
        }
    }
    if (attr) { *key = attr; return true; }
    return false;
}

/* Does the element carry one of the deep_targets features? */
static bool has_match_target(const HasKeySet *ks, const HtmlNode *hn)
{
    if (hn->tag_name && has_lookup(ks, has_key('t', hn->tag_name, strlen(hn->tag_name)))) return true;
    for (HtmlAttribute *a = hn->attributes; a; a = a->next) {
        if (!a->name) continue;
        if (has_lookup(ks, has_key('[', a->name, strlen(a->name)))) return true;
        const char *v = a->value ? a->value : "";
        if (strcmp(a->name, "id") == 0) {
            if (v[0] && has_lookup(ks, has_key('#', v, strlen(v)))) return true;
        } else if (strcmp(a->name, "class") == 0) {
            for (const char *c = v; *c;) {
                while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r' || *c == '\f') c++;
                const char *e = c;
                while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r' && *e != '\f') e++;
                if (e > c && has_lookup(ks, has_key('.', c, (size_t)(e - c)))) return true;
                c = e;
            }
        }
    }
    return false;
}

/* Start of the compound selector ending at `end`. */
static const char *has_compound_start(const char *sel, const char *end)
{
    const char *st = end;
    int depth = 0;
    while (st > sel) {
        char c = st[-1];
        if (c == ')' || c == ']') depth++;
        else if (c == '(' || c == '[') { if (depth == 0) break; depth--; }
        else if (depth == 0 && (c == ' ' || c == '>' || c == '+' || c == '~' || c == ',')) break;
        st--;
    }
    return st;
}

/* Adds the tag, ids and classes of compound [st, end) to `ks`; false when
 * it names none (or has escapes this does not decode). */
/* Adds one feature every element matching compound [st, end) carries (its
 * id, else its first class, else its tag) to `ks`; false when it names
 * none (or has escapes this does not decode). */
static bool has_compound_keys(HasSubjects *hs, HasKeySet *ks, const char *st, const char *end, bool deep)
{
    for (const char *q = st; q < end; q++) if (*q == '\\') return false;
    uint64_t tag = 0, cls = 0, id = 0, attr = 0;
    HasReq req = { {0}, 0, 0 };
    const char *q = st;
    if (q < end && (isalpha((unsigned char)*q) || *q == '-' || *q == '_')) {
        const char *e = q;
        while (e < end && (isalnum((unsigned char)*e) || *e == '-' || *e == '_')) e++;
        tag = has_key('t', q, (size_t)(e - q));
        if (req.n < HAS_REQ_MAX) req.keys[req.n++] = tag;
        q = e;
    }
    while (q < end) {
        if (*q == '[' || *q == '(') {
            /* Attribute values and pseudo-class arguments say nothing
             * about the subject's own class or id; an attribute's name
             * is a feature it must carry. */
            if (*q == '[' && !attr) {
                const char *n0 = q + 1, *e = n0;
                while (e < end && (isalnum((unsigned char)*e) || *e == '-' || *e == '_')) e++;
                if (e > n0) {
                    attr = has_key('[', n0, (size_t)(e - n0));
                    if (req.n < HAS_REQ_MAX) req.keys[req.n++] = attr;
                }
            }
            int d = 0;
            for (; q < end; q++) {
                if (*q == '[' || *q == '(') d++;
                else if ((*q == ']' || *q == ')') && --d == 0) break;
            }
            q++;
        } else if (*q == '.' || *q == '#') {
            char kind = *q++;
            const char *e = q;
            while (e < end && (isalnum((unsigned char)*e) || *e == '-' || *e == '_')) e++;
            if (e > q) {
                uint64_t k = has_key(kind, q, (size_t)(e - q));
                if (kind == '#' && !id) id = k;
                if (kind == '.' && !cls) cls = k;
                if (req.n < HAS_REQ_MAX) req.keys[req.n++] = k;
            }
            q = e;
        } else {
            q++;
        }
    }
    /* One feature the compound requires stands for it in the key sets the
     * restyle uses; req keeps them all (has_reqs_node). */
    uint64_t k = id ? id : cls ? cls : tag ? tag : attr;
    if (!k) return false;
    has_add(hs, ks, k, deep);
    HasReqList *rl = ks == &hs->anc ? &hs->anc_req : &hs->self_req;
    if (rl->n == rl->cap) {
        int nc = rl->cap ? rl->cap * 2 : 16;
        HasReq *nv = (HasReq *)realloc(rl->v, (size_t)nc * sizeof(HasReq));
        if (!nv) { hs->universal = hs->universal_self = true; return true; }   /* cannot record: assume anything */
        rl->v = nv;
        rl->cap = nc;
    }
    req.deep = deep;
    rl->v[rl->n++] = req;
    return true;
}

/* Pseudo-classes that never match here (dynamic states a static render
 * does not have; css_apply_pseudo gives them never_match). */
static bool has_pseudo_never_matches(const char *name, size_t nl)
{
    static const char *const names[] = {
        "visited", "hover", "active", "focus", "focus-visible", "focus-within", "target", "target-within",
        "checked", "disabled", "indeterminate", "read-write", "autofill", "playing", "paused", "current",
        "past", "future",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strlen(names[i]) == nl && strncasecmp(names[i], name, nl) == 0) return true;
    return false;
}

/* Can the :has() argument [arg, end) ever match?  Not when every one of its
 * alternatives needs a pseudo-class that never matches (outside :not()):
 * ytd-app[masthead-hidden]:not(:has(#masthead-container:focus-within)) is
 * then a constant, whatever the DOM under the subject does. */
static bool has_argument_can_match(const char *arg, const char *end)
{
    bool alt_dead = false;
    int depth = 0;
    int not_depth[32];
    int nnot = 0;
    for (const char *q = arg; q < end; q++) {
        char c = *q;
        if (c == '"' || c == '\'') {
            char qc = c;
            for (q++; q < end && *q != qc; q++) if (*q == '\\' && q + 1 < end) q++;
            continue;
        }
        if (c == '(') { depth++; continue; }
        if (c == ')') {
            if (nnot && not_depth[nnot - 1] == depth) nnot--;
            depth--;
            continue;
        }
        if (c == ',' && depth == 0) {
            if (!alt_dead) return true;
            alt_dead = false;
            continue;
        }
        if (c == ':') {
            const char *n = q + 1;
            if (n < end && *n == ':') continue;          /* a pseudo-element */
            const char *e = n;
            while (e < end && (isalnum((unsigned char)*e) || *e == '-' || *e == '_')) e++;
            size_t nl = (size_t)(e - n);
            if (nl == 3 && strncasecmp(n, "not", 3) == 0 && e < end && *e == '(') {
                if (nnot < 32) not_depth[nnot++] = depth + 1;
            } else if (!nnot && has_pseudo_never_matches(n, nl)) {
                alt_dead = true;
            }
            q = e - 1;
        }
    }
    return !alt_dead;
}

static void has_scan_selector(HasSubjects *hs, const char *sel)
{
    for (const char *p = strstr(sel, ":has("); p; p = strstr(p + 5, ":has(")) {
        {
            /* The argument's extent; a constant :has() has no subjects. */
            const char *e = p + 5;
            int d = 1;
            for (; *e && d > 0; e++) {
                if (*e == '(') d++;
                else if (*e == ')') d--;
            }
            if (!has_argument_can_match(p + 5, d == 0 ? e - 1 : e)) continue;
        }
        hs->any = true;
        const char *arg = p + 5;
        while (*arg == ' ') arg++;
        if (*arg == '+' || *arg == '~') hs->sibling_relative = true;
        /* After the argument: a combinator before the selector ends means
         * the :has() compound is not the rightmost one. */
        bool deep = false;
        {
            const char *e = p + 4;
            int d = 0;
            for (; *e; e++) {
                if (*e == '(') d++;
                else if (*e == ')' && --d == 0) { e++; break; }
            }
            int outer = 0;
            for (; *e; e++) {
                char c = *e;
                if (c == '(') d++;
                else if (c == ')') { if (d > 0) d--; else outer++; }
                else if (c == ',' && d == 0 && outer == 0) break;
                else if (d == 0 && (c == '>' || c == '+' || c == '~')) { deep = true; break; }
                else if (d == 0 && c == ' ') {
                    const char *t = e;
                    while (*t == ' ') t++;
                    if (*t && *t != ',' && *t != ')' && *t != '{') { deep = true; break; }
                }
            }
        }
        /* The compound carrying :has(, walking out of :not( / :is( /
         * :where( when :has() is all their argument starts with. */
        const char *end = p;
        const char *st = has_compound_start(sel, end);
        bool named = has_compound_keys(hs, &hs->self, st, end, deep);
        while (!named && st > sel && st[-1] == '(') {
            const char *name = st - 1;
            static const char *const outs[] = { ":not", ":is", ":where", ":matches", ":-webkit-any" };
            const char *colon = NULL;
            for (size_t i = 0; i < sizeof(outs) / sizeof(outs[0]); i++) {
                size_t l = strlen(outs[i]);
                if ((size_t)(name - sel) >= l && strncasecmp(name - l, outs[i], l) == 0) { colon = name - l; break; }
            }
            if (!colon) break;
            end = colon;
            st = has_compound_start(sel, end);
            named = has_compound_keys(hs, &hs->self, st, end, deep);
        }
        if (!named && st > sel) {
            /* ".list :has(...)", ".list > :has(...)": an ancestor of the
             * subject must match the compound before. */
            const char *c = st;
            while (c > sel && c[-1] == ' ') c--;
            if (c > sel && c[-1] == '>') { c--; while (c > sel && c[-1] == ' ') c--; }
            if (c < st && c > sel && c[-1] != ',' && c[-1] != '+' && c[-1] != '~' && c[-1] != '(') {
                const char *ast = has_compound_start(sel, c);
                named = has_compound_keys(hs, &hs->anc, ast, c, deep);
            }
        }
        if (!named) {
            uint64_t target;
            bool targeted = deep && has_rightmost_key(sel, p, &target);
            if (g_css_log > 1)
                fprintf(stderr, "[css] :has() universal%s: %.100s\n",
                        !deep ? "" : targeted ? " deep (rightmost compound targeted)" : " deep", st);
            hs->universal = true;
            if (!deep) hs->universal_self = true;
            else if (targeted) has_add(hs, &hs->deep_targets, target, false);
            else hs->universal_deep = true;
        }
    }
}

static int has_match_keys(const HasKeySet *ks, const HtmlNode *hn)
{
    int r = 0;
    if (hn->tag_name) r = has_lookup(ks, has_key('t', hn->tag_name, strlen(hn->tag_name)));
    const char *id = layout_node_attribute((HtmlNode *)hn, "id");
    if (r < 2 && id && id[0]) { int m = has_lookup(ks, has_key('#', id, strlen(id))); if (m > r) r = m; }
    const char *cls = layout_node_attribute((HtmlNode *)hn, "class");
    for (const char *c = cls; c && *c && r < 2;) {
        while (*c == ' ' || *c == '\t' || *c == '\n') c++;
        const char *e = c;
        while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
        if (e > c) { int m = has_lookup(ks, has_key('.', c, (size_t)(e - c))); if (m > r) r = m; }
        c = e;
    }
    for (HtmlAttribute *a = hn->attributes; a && r < 2; a = a->next) {
        if (!a->name) continue;
        int m = has_lookup(ks, has_key('[', a->name, strlen(a->name)));
        if (m > r) r = m;
    }
    return r;
}

/* Does element hn carry every feature of one of rl's compounds (the full
 * test has_match_keys approximates with one feature each)?  2 when that
 * compound has more selector after it, else 1. */
static int has_reqs_node(const HasReqList *rl, const HtmlNode *hn)
{
    if (!rl->n) return 0;
    uint64_t have[64];
    int nh = 0;
    bool overflow = false;
    if (hn->tag_name) have[nh++] = has_key('t', hn->tag_name, strlen(hn->tag_name));
    for (HtmlAttribute *a = hn->attributes; a; a = a->next) {
        if (!a->name) continue;
        if (nh < 64) have[nh++] = has_key('[', a->name, strlen(a->name));
        else overflow = true;
        const char *v = a->value ? a->value : "";
        if (strcmp(a->name, "id") == 0) {
            if (v[0]) { if (nh < 64) have[nh++] = has_key('#', v, strlen(v)); else overflow = true; }
        } else if (strcmp(a->name, "class") == 0) {
            for (const char *c = v; *c;) {
                while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r' || *c == '\f') c++;
                const char *e = c;
                while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r' && *e != '\f') e++;
                if (e > c) { if (nh < 64) have[nh++] = has_key('.', c, (size_t)(e - c)); else overflow = true; }
                c = e;
            }
        }
    }
    if (overflow) return 2;
    int r = 0;
    for (int i = 0; i < rl->n && r < 2; i++) {
        const HasReq *q = &rl->v[i];
        bool all = true;
        for (int k = 0; k < q->n && all; k++) {
            bool found = false;
            for (int h = 0; h < nh && !found; h++) found = have[h] == q->keys[k];
            all = found;
        }
        if (all) r = q->deep ? 2 : 1;
    }
    return r;
}

/* Could layout node `idx` be the subject of some :has() rule? 0: no, 1: yes,
 * 2: yes, and descendants' matching depends on it. */
/* The :has() checks of an element's own features (0: has_match_keys on
 * hs->self, 1: on hs->anc, 2: has_match_target on hs->deep_targets, 3 and
 * 4: has_reqs_node on hs->self_req and hs->anc_req),
 * remembered on the node: forced layouts run them on most elements, and
 * they hash every class name each time.  Valid while the element's
 * attributes (attr_hash, computed by style_cache_prepare) and the key sets
 * are the same. */
static int has_memo_compute(const HasSubjects *hs, const HtmlNode *hn, int which)
{
    switch (which) {
    case 0: return has_match_keys(&hs->self, hn);
    case 1: return has_match_keys(&hs->anc, hn);
    case 2: return has_match_target(&hs->deep_targets, hn);
    case 3: return has_reqs_node(&hs->self_req, hn);
    default: return has_reqs_node(&hs->anc_req, hn);
    }
}

static int has_memo(const HasSubjects *hs, HtmlNode *hn, int which)
{
    uint64_t stamp = (hn->attr_hash ^ (hs->gen * 0x9E3779B97F4A7C15ull)) | 1;
    if (!hn->attr_hash) {   /* not hashed this pass: no memo */
        return has_memo_compute(hs, hn, which);
    }
    if (hn->has_memo_stamp != stamp) {
        hn->has_memo_stamp = stamp;
        for (int i = 0; i < 5; i++) hn->has_memo[i] = -1;
    }
    if (hn->has_memo[which] < 0)
        hn->has_memo[which] = (int8_t)has_memo_compute(hs, hn, which);
    return hn->has_memo[which];
}

/* has_memo for layout node idx, from ctx->has_bits when known there (a
 * kept tree's elements keep them; those whose attributes changed are
 * forgotten by layout_tree_keep): 0 for a node that is not an element. */
static int has_memo_at(LayoutContext *ctx, const HasSubjects *hs, int idx, int which)
{
    signed char *b = ctx->has_bits && ctx->has_bits_gen == hs->gen ? ctx->has_bits + (size_t)idx * 8 : NULL;
    if (b && b[5] == 0) return 0;
    if (b && b[which] >= 0) return b[which];
    HtmlNode *hn = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    bool elem = hn && hn->type == HTML_NODE_ELEMENT;
    int v = elem ? has_memo(hs, hn, which) : 0;
    if (b) { b[5] = elem; b[which] = (signed char)v; }
    return v;
}

static bool has_is_element(LayoutContext *ctx, int idx)
{
    signed char *b = ctx->has_bits ? ctx->has_bits + (size_t)idx * 8 : NULL;
    if (b && b[5] >= 0) return b[5] != 0;
    HtmlNode *hn = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    bool elem = hn && hn->type == HTML_NODE_ELEMENT;
    if (b) b[5] = elem;
    return elem;
}

/* ctx->has_bits for the key sets hs: all forgotten when they are not the
 * ones they were for. */
static void has_bits_ready(LayoutContext *ctx, const HasSubjects *hs)
{
    if (!ctx->has_bits) {
        ctx->has_bits = (signed char *)malloc(((size_t)ctx->tree.count + 1) * 8);
        if (!ctx->has_bits) return;
        ctx->has_bits_gen = hs->gen + 1;
    }
    if (ctx->has_bits_gen != hs->gen) {
        memset(ctx->has_bits, -1, ((size_t)ctx->tree.count + 1) * 8);
        ctx->has_bits_gen = hs->gen;
    }
}

/* The :has() rules of the last restyle's style set (layout_apply_stylesheet),
 * and its generation (g_has_gen, declared above). */
static HasSubjects g_has;

/* ---- Leaving hidden subtrees out ----
 *
 * What a display:none element holds is neither styled nor laid out
 * (style_skip_descendants), so a tree build may leave it out, and every
 * pass over the tree skips it.  Whether an element stays display:none is
 * known only once styled: the build guesses from the style cache, and
 * css_layout_run builds again in full when a guess turns out shown
 * (prune_failed).  The cache entry of an element whose descendants were
 * left out notes it (pruned_below), and they are restyled when it is next
 * built with them, whatever their keys say.  The restyle's :has() forcing
 * does not see changes in a left-out subtree, so an element is left out
 * only when no :has() rule can flip because of one: no subject at or above
 * it (nor, for sibling-relative arguments, before those among their
 * siblings), and no element a rule whose :has() compound names nothing
 * restyles in the tree.
 * CSS_PRUNE=0: build in full. */
static bool layout_prune_root(HtmlDocument *doc, int i)
{
    HtmlNode *hn = (HtmlNode *)po_array_payload(&doc->array, i);
    if (!hn || hn->type != HTML_NODE_ELEMENT || !hn->store_id || hn->array_node.first_child < 0) return false;
    if (hn->hidden_hint == 2) return false;   /* (a valid entry it has is not display:none) */
    const StyleCacheEntry *e = style_cache_find(hn->store_id);
    if (!e || e->stale || e->style.display != CSS_DISPLAY_NONE || !g_sc.global) return false;
    if (!g_has.any) return true;
    /* (Rules whose :has() compound names nothing restyle their targets
     * anywhere: css_layout_document builds again in full when the tree has
     * one.) */
    if (g_has.universal_self || g_has.universal_deep) return false;
    for (int a = i; a >= 0; a = po_array_parent(&doc->array, a)) {
        HtmlNode *an = (HtmlNode *)po_array_payload(&doc->array, a);
        if (!an || an->type != HTML_NODE_ELEMENT) continue;
        /* (An ancestor carrying an ancestor compound's features makes what is
         * below it subjects.) */
        if ((g_has.self_req.n && has_memo(&g_has, an, 3)) || (a != i && g_has.anc_req.n && has_memo(&g_has, an, 4)))
            return false;
        for (int s = g_has.sibling_relative ? po_array_prev_sibling(&doc->array, a) : -1; s >= 0;
             s = po_array_prev_sibling(&doc->array, s)) {
            HtmlNode *sn = (HtmlNode *)po_array_payload(&doc->array, s);
            if (sn && sn->type == HTML_NODE_ELEMENT && g_has.self_req.n && has_memo(&g_has, sn, 3)) return false;
        }
    }
    return true;
}

static int has_may_match(const HasSubjects *hs, LayoutContext *ctx, int idx)
{
    if (hs->universal_deep) return 2;
    if (ctx->tree.nodes[idx].dom_node_idx < 0) return 0;
    /* (A universal :has() compound with more selector after it changes the
     * elements the rightmost compound names, not itself: deep_targets.) */
    int r = hs->universal_self ? 1 : 0;
    if (hs->self_req.n) { int m = has_memo_at(ctx, hs, idx, 3); if (m > r) r = m; }
    if (hs->anc_req.n && r < 2) {
        for (int a = ctx->tree.nodes[idx].parent_idx; a >= 0 && r < 2; a = ctx->tree.nodes[a].parent_idx) {
            int m = has_memo_at(ctx, hs, a, 4);
            if (m > r) r = m;
        }
    }
    return r;
}

/* What a compound selector that is not the rightmost one (".a > .b", the .a)
 * can observe about an element: other elements' matching depends on these
 * features only, so a change elsewhere in an element's attributes needs no
 * recompute of its subtree or later siblings. Features inside :has() are
 * left to the :has() forcing.
 *
 * Each such feature also records what a change of it can affect (an
 * invalidation set): the descendants matching the rightmost compound of the
 * selectors it appears in (one tag, id, class or attribute name of that
 * compound, which every match must carry), the whole subtree when that
 * compound names nothing or the feature sits inside :is() / :not(), and
 * later siblings when a sibling combinator follows it. */
typedef struct StyleKeySet {
    uint64_t *slot;             /* 0 = empty */
    uint32_t cap, n;
} StyleKeySet;

typedef struct StyleInval {
    bool whole;                 /* the element's whole subtree */
    bool sibling;               /* later siblings and their subtrees */
    uint64_t *targets;          /* has_key of descendants' features */
    int n, cap;
} StyleInval;

typedef struct StyleCtxFeatures {
    bool all;                   /* something this does not model: every attribute */
    bool all_tags;
    bool sibling;               /* '+' or '~' somewhere */
    /* Structure seen: position, being first, the previous sibling, being
     * last, position from the end, having no children. */
    bool s_pos, s_first, s_prev, s_last, s_fromend, s_empty;
    StyleKeySet keys;           /* has_key: 't' tag, '#' id, '.' class, '[' attribute name */
    /* Invalidation sets by key (open addressing, `inval_key` 0 = empty). */
    uint64_t *inval_key;
    int *inval_idx;
    uint32_t inval_cap, inval_n;
    StyleInval *inval;
    /* While scanning: what the compound being scanned invalidates. */
    uint64_t cur_target;        /* 0: whole subtree */
    bool cur_sibling;
    int nested;
} StyleCtxFeatures;

static void keyset_add(StyleKeySet *s, uint64_t k, bool *fail)
{
    if (!k) k = 1;
    if (2 * (s->n + 1) > s->cap) {
        uint32_t nc = s->cap ? s->cap * 2 : 256;
        uint64_t *ns = (uint64_t *)calloc(nc, sizeof(uint64_t));
        if (!ns) { *fail = true; return; }
        for (uint32_t i = 0; i < s->cap; i++) {
            uint64_t v = s->slot[i];
            if (!v) continue;
            uint32_t h = (uint32_t)(v >> 7) & (nc - 1);
            while (ns[h]) h = (h + 1) & (nc - 1);
            ns[h] = v;
        }
        free(s->slot);
        s->slot = ns;
        s->cap = nc;
    }
    uint32_t h = (uint32_t)(k >> 7) & (s->cap - 1);
    while (s->slot[h]) {
        if (s->slot[h] == k) return;
        h = (h + 1) & (s->cap - 1);
    }
    s->slot[h] = k;
    s->n++;
}

static bool keyset_has(const StyleKeySet *s, uint64_t k)
{
    if (!s->cap) return false;
    if (!k) k = 1;
    uint32_t h = (uint32_t)(k >> 7) & (s->cap - 1);
    while (s->slot[h]) {
        if (s->slot[h] == k) return true;
        h = (h + 1) & (s->cap - 1);
    }
    return false;
}

static void keyset_clear(StyleKeySet *s)
{
    if (s->n) memset(s->slot, 0, (size_t)s->cap * sizeof(uint64_t));
    s->n = 0;
}

/* ---- What attribute changes can affect ----
 *
 * Selector matching reads an element's attributes only through the
 * classes, ids and attribute names its selectors spell out (and :link,
 * href); every other pseudo-class is structural or never matches.  So a
 * change of an attribute no selector names, outside what layout reads
 * itself, changes no computed style and no geometry.  The names come from a
 * lexical scan of every selector (inside :not(), :is(), :has(), :host() and
 * ::slotted() arguments too), kept per style-set generation. */
typedef struct StyleMentions {
    uint64_t gen;               /* style set generation (0 = empty slot) */
    StyleKeySet keys;           /* has_key('.' class, '#' id, '[' attribute) */
    /* Attribute tests by the compound they sit in: one that names a class
     * or id (the guard) can only match elements carrying it, so for others
     * the attribute is not tested.  attr_any: tested in some unguarded
     * compound; attr_named / attr_guarded: tested under some guard /
     * attr_guard_key(attribute, guard). */
    StyleKeySet attr_any, attr_named, attr_guarded;
    HasSubjects has;
    bool empty;                 /* some selector uses :empty */
    /* some selector uses :nth-last-child(An+B of S): an attribute change can
     * restyle earlier siblings */
    bool last_of;
    /* Features named in the rightmost compound (or in its parentheses) of
     * some rule that sets a layout property: what can change how an
     * element's own box is laid out.  subj_all: not recorded (every
     * mentioned feature counts). */
    StyleKeySet subj;
    bool subj_all;
    bool unknown;               /* escapes or allocation failure: everything matters */
} StyleMentions;

#define STYLE_MENTIONS_SLOTS 4
static StyleMentions g_mentions[STYLE_MENTIONS_SLOTS];
static int g_mentions_next;

static bool mention_ident(unsigned char c)
{
    return isalnum(c) || c == '-' || c == '_' || c >= 0x80;
}

/* An identifier at p with CSS escapes decoded (.md\:flex is the class
 * "md:flex") into buf; returns its end.  *len > cap: too long to keep. */
static const char *mention_read_ident(const char *p, char *buf, size_t cap, size_t *len)
{
    size_t n = 0;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' && p[1] && p[1] != '\n') {
            p++;
            if (isxdigit((unsigned char)*p)) {
                unsigned cp = 0;
                for (int k = 0; k < 6 && isxdigit((unsigned char)*p); k++, p++)
                    cp = cp * 16 + (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
                if (*p == ' ' || *p == '\t' || *p == '\n') p++;
                char u[4];
                int ul;
                if (cp < 0x80) { u[0] = (char)cp; ul = 1; }
                else if (cp < 0x800) { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); ul = 2; }
                else if (cp < 0x10000) { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                         u[2] = (char)(0x80 | (cp & 0x3F)); ul = 3; }
                else { u[0] = (char)(0xF0 | (cp >> 18)); u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                       u[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (char)(0x80 | (cp & 0x3F)); ul = 4; }
                for (int k = 0; k < ul; k++, n++) if (n < cap) buf[n] = u[k];
            } else {
                if (n < cap) buf[n] = *p;
                n++;
                p++;
            }
            continue;
        }
        if (!mention_ident(c)) break;
        if (n < cap) buf[n] = (char)c;
        n++;
        p++;
    }
    *len = n;
    return p;
}

static void mentions_add(StyleMentions *m, char kind, const char *buf, size_t n, size_t cap, bool *fail)
{
    if (n > cap) { m->unknown = true; return; }
    if (n) keyset_add(&m->keys, has_key(kind, buf, n), fail);
}

static uint64_t attr_guard_key(uint64_t attr, uint64_t guard)
{
    return style_mix(attr, &guard, sizeof(guard));
}

static bool style_prop_paint_only(const char *p);

/* Does rule r set a property that can move or resize a box (anything but
 * paint; custom properties count, as other rules may lay out by them)? */
static bool rule_sets_layout(const CssRule *r)
{
    for (int i = 0; i < r->declaration_count; i++) {
        const char *p = r->declarations[i].property;
        if (p && !style_prop_paint_only(p)) return true;
    }
    return false;
}

/* `layout`: the rule sets layout properties (its rightmost compounds'
 * features go in m->subj). */
static void mentions_scan(StyleMentions *m, const char *s, bool layout)
{
    bool fail = false;
    char buf[512];
    size_t n;
    const char *p = s;
    /* The compound being read: its first class or id, its attribute tests. */
    uint64_t guard = 0;
    uint64_t pend[16];
    int npend = 0;
#define MENTION_END_COMPOUND() do { \
        for (int i_ = 0; i_ < npend; i_++) { \
            if (guard) { \
                keyset_add(&m->attr_named, pend[i_], &fail); \
                keyset_add(&m->attr_guarded, attr_guard_key(pend[i_], guard), &fail); \
            } else { \
                keyset_add(&m->attr_any, pend[i_], &fail); \
            } \
        } \
        npend = 0; \
        guard = 0; \
    } while (0)
#define MENTION_ATTR(k) do { \
        uint64_t k_ = (k); \
        if (npend < 16) pend[npend++] = k_; \
        else keyset_add(&m->attr_any, k_, &fail); \
    } while (0)
    /* The rightmost compound of each complex selector: the features since
     * the last combinator outside parentheses (white space there is one when
     * a compound follows). */
    uint64_t subj[128];
    int nsubj = 0, depth = 0;
    bool ws = false;
#define MENTION_SUBJ(k) do { if (nsubj < 128) subj[nsubj++] = (k); else m->subj_all = true; } while (0)
#define MENTION_SUBJ_FLUSH() do { \
        for (int i_ = 0; layout && i_ < nsubj; i_++) keyset_add(&m->subj, subj[i_], &fail); \
        nsubj = 0; \
    } while (0)
    while (*p) {
        char c = *p;
        if (depth == 0) {
            bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
            bool comb = c == '>' || c == '+' || c == '~';
            if (!space && !comb && ws && c != ',' && c != ')') nsubj = 0;
            ws = space || (ws && comb);
            if (comb) nsubj = 0;
            if (c == ',') MENTION_SUBJ_FLUSH();
        }
        if (c == '(') depth++;
        else if (c == ')' && depth > 0) depth--;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '>' || c == '+' || c == '~' ||
            c == ',' || c == '(' || c == ')') {
            MENTION_END_COMPOUND();
            p++;
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            p++;
            while (*p && *p != q) p += (*p == '\\' && p[1]) ? 2 : 1;
            if (*p) p++;
            continue;
        }
        if (c == '.' || c == '#') {
            p = mention_read_ident(p + 1, buf, sizeof(buf), &n);
            mentions_add(m, c, buf, n, sizeof(buf), &fail);
            if (n && n <= sizeof(buf)) MENTION_SUBJ(has_key(c, buf, n));
            if (!guard && n && n <= sizeof(buf)) guard = has_key(c, buf, n);
            continue;
        }
        if (c == '[') {
            p++;
            while (*p == ' ' || *p == '\t' || *p == '\n') p++;
            p = mention_read_ident(p, buf, sizeof(buf), &n);
            if (*p == '|' && p[1] != '=') p = mention_read_ident(p + 1, buf, sizeof(buf), &n);
            mentions_add(m, '[', buf, n, sizeof(buf), &fail);
            if (n && n <= sizeof(buf)) MENTION_ATTR(has_key('[', buf, n));
            if (n && n <= sizeof(buf)) MENTION_SUBJ(has_key('[', buf, n));
            /* The rest of the test: operator and (quoted) value. */
            while (*p && *p != ']') {
                if (*p == '"' || *p == '\'') {
                    char q = *p++;
                    while (*p && *p != q) p += (*p == '\\' && p[1]) ? 2 : 1;
                    if (*p) p++;
                } else {
                    p += (*p == '\\' && p[1]) ? 2 : 1;
                }
            }
            if (*p) p++;
            continue;
        }
        if (c == '\\') {      /* an escape in a tag name: tags are not tracked */
            p = mention_read_ident(p, buf, sizeof(buf), &n);
            if (p == s || n == 0) p++;
            continue;
        }
        if (c == ':') {
            while (*p == ':') p++;
            const char *st = p;
            while (*p && mention_ident((unsigned char)*p)) p++;
            size_t nl = (size_t)(p - st);
            if (nl == 4 && strncasecmp(st, "link", 4) == 0) {
                keyset_add(&m->keys, has_key('[', "href", 4), &fail);
                MENTION_ATTR(has_key('[', "href", 4));
                MENTION_SUBJ(has_key('[', "href", 4));
            }
            if (nl == 5 && strncasecmp(st, "empty", 5) == 0) m->empty = true;
            if (nl == 14 && strncasecmp(st, "nth-last-child", 14) == 0 && *p == '(') {
                const char *e = strchr(p, ')');
                for (const char *o = p; e && o + 4 <= e; o++)
                    if (strncasecmp(o, " of ", 4) == 0) { m->last_of = true; break; }
            }
            continue;
        }
        p++;
    }
    MENTION_END_COMPOUND();
    MENTION_SUBJ_FLUSH();
#undef MENTION_END_COMPOUND
#undef MENTION_ATTR
#undef MENTION_SUBJ
#undef MENTION_SUBJ_FLUSH
    if (fail) m->unknown = true;
}

static void mentions_free(StyleMentions *m)
{
    free(m->keys.slot);
    free(m->attr_any.slot);
    free(m->attr_named.slot);
    free(m->attr_guarded.slot);
    free(m->subj.slot);
    has_free(&m->has);
    memset(m, 0, sizeof(*m));
}

static void mentions_build(const CssStyleSet *set, uint64_t gen)
{
    for (int i = 0; i < STYLE_MENTIONS_SLOTS; i++)
        if (g_mentions[i].gen == gen) return;
    /* A set extending one we have the mentions of: add its new rules to
     * those (a superset; a lookup by the old generation then finds nothing,
     * which callers treat as "everything matters"). */
    uint64_t base_gen;
    int base_nrules;
    if (styleset_base_of(gen, &base_gen, &base_nrules)) {
        for (int i = 0; i < STYLE_MENTIONS_SLOTS; i++) {
            StyleMentions *m = &g_mentions[i];
            if (m->gen != base_gen) continue;
            for (int r = base_nrules, nr = css_styleset_rule_count(set); r < nr; r++) {
                const CssRule *rule = css_styleset_rule(set, r);
                if (!rule || !rule->selector_text) continue;
                mentions_scan(m, rule->selector_text, rule_sets_layout(rule));
                has_scan_selector(&m->has, rule->selector_text);
            }
            m->gen = gen;
            return;
        }
    }
    StyleMentions *m = &g_mentions[g_mentions_next];
    g_mentions_next = (g_mentions_next + 1) % STYLE_MENTIONS_SLOTS;
    mentions_free(m);
    for (int r = 0, nr = css_styleset_rule_count(set); r < nr; r++) {
        const CssRule *rule = css_styleset_rule(set, r);
        if (!rule || !rule->selector_text) continue;
        mentions_scan(m, rule->selector_text, rule_sets_layout(rule));
        has_scan_selector(&m->has, rule->selector_text);
    }
    m->gen = gen;
    if (g_css_log > 0)
        fprintf(stderr, "[css] mentions: %u keys%s%s%s\n", m->keys.n, m->unknown ? ", unknown" : "",
                m->has.any ? ", :has()" : "", m->empty ? ", :empty" : "");
}

static const StyleMentions *mentions_of(const LayoutContext *ctx)
{
    if (!ctx || !ctx->style_gen) return NULL;
    for (int i = 0; i < STYLE_MENTIONS_SLOTS; i++)
        if (g_mentions[i].gen == ctx->style_gen) return g_mentions[i].unknown ? NULL : &g_mentions[i];
    return NULL;
}

/* Attributes layout reads itself (sizes, images, inline style, sheets). */
static bool layout_reads_attr(const char *name)
{
    static const char *const names[] = {
        "width", "height", "viewbox", "type", "src", "srcset", "rel", "href", "media",
        "hidden", "colspan", "rowspan", "span", "dir", "lang", "slot", "name", "size", "cols", "rows",
        /* presentational hints */
        "bgcolor", "align", "valign", "nowrap", "cellpadding", "cellspacing", "border", "color", "face",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcasecmp(name, names[i]) == 0) return true;
    return false;
}

static bool class_has_token(const char *list, const char *t, size_t n)
{
    for (const char *c = list ? list : ""; *c;) {
        while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r' || *c == '\f') c++;
        const char *e = c;
        while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r' && *e != '\f') e++;
        if ((size_t)(e - c) == n && n && strncasecmp(c, t, n) == 0) return true;
        c = e;
    }
    return false;
}

/* A class token in `a` but not in `b` that some selector names? */
static bool class_tokens_mentioned(const StyleMentions *m, const char *a, const char *b)
{
    for (const char *c = a ? a : ""; *c;) {
        while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r' || *c == '\f') c++;
        const char *e = c;
        while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r' && *e != '\f') e++;
        size_t n = (size_t)(e - c);
        if (n && !class_has_token(b, c, n) && keyset_has(&m->keys, has_key('.', c, n))) return true;
        c = e;
    }
    return false;
}

/* Properties that only change how boxes paint, never where they are or
 * how big (transforms do move boxes here; custom properties may feed any
 * other property through var()). */
static bool style_prop_paint_only(const char *p)
{
    static const char *const exact[] = {
        "color", "opacity", "visibility", "z-index", "cursor", "pointer-events", "user-select",
        "-webkit-user-select", "fill", "fill-opacity", "stroke", "stroke-width", "stroke-opacity",
        "stroke-dasharray", "stroke-dashoffset", "box-shadow", "text-shadow", "filter", "backdrop-filter",
        "-webkit-backdrop-filter", "mix-blend-mode", "isolation", "will-change", "caret-color",
        "accent-color", "border-color", "border-top-color", "border-right-color", "border-bottom-color",
        "border-left-color", "clip-path", "-webkit-tap-highlight-color", "touch-action", "outline",
        "outline-color", "outline-style", "outline-width", "outline-offset", "image-rendering",
        "text-decoration-color", "text-decoration-style", "text-decoration-thickness",
        "text-underline-offset", "scroll-behavior", "overscroll-behavior",
    };
    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++)
        if (strcasecmp(p, exact[i]) == 0) return true;
    static const char *const prefix[] = {
        "background", "transition", "animation", "mask", "-webkit-mask", "scrollbar-",
    };
    for (size_t i = 0; i < sizeof(prefix) / sizeof(prefix[0]); i++)
        if (strncasecmp(p, prefix[i], strlen(prefix[i])) == 0) return true;
    return false;
}

/* The value an inline declaration list gives `prop` (the last, an
 * !important one over others), or NULL. */
static const char *inline_decl_value(const CssDeclaration *d, int n, const char *prop)
{
    const char *v = NULL;
    bool imp = false;
    for (int i = 0; i < n; i++) {
        if (!d[i].property || strcasecmp(d[i].property, prop) != 0) continue;
        if (imp && !d[i].important) continue;
        v = d[i].value ? d[i].value : "";
        imp = d[i].important;
    }
    return v;
}

/* Offsets: moving the box (and its subtree) without reflowing anything. */
static bool style_prop_offset(const char *p)
{
    static const char *const names[] = {
        "left", "top", "right", "bottom", "inset", "inset-block", "inset-inline", "inset-block-start",
        "inset-block-end", "inset-inline-start", "inset-inline-end", "transform", "translate",
        "transform-origin", "rotate", "scale",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcasecmp(p, names[i]) == 0) return true;
    return false;
}

/* The box itself and what it contains: an out-of-flow box keeps these to
 * its own subtree. */
static bool style_prop_box_local(const char *p)
{
    static const char *const exact[] = {
        "width", "height", "min-width", "min-height", "max-width", "max-height", "box-sizing",
        "line-height", "letter-spacing", "word-spacing", "text-align", "text-indent", "text-transform",
        "white-space", "word-break", "overflow-wrap", "word-wrap", "vertical-align", "aspect-ratio",
        "object-fit", "object-position", "text-overflow", "-webkit-line-clamp", "-webkit-box-orient",
        "direction", "tab-size", "hyphens", "gap", "row-gap", "column-gap", "order",
    };
    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++)
        if (strcasecmp(p, exact[i]) == 0) return true;
    static const char *const prefix[] = {
        "margin", "padding", "border", "font", "overflow", "flex", "justify-", "align-", "place-",
        "grid", "list-style", "text-decoration", "block-size", "inline-size", "min-block", "min-inline",
        "max-block", "max-inline",
    };
    for (size_t i = 0; i < sizeof(prefix) / sizeof(prefix[0]); i++)
        if (strncasecmp(p, prefix[i], strlen(prefix[i])) == 0) return true;
    return false;
}

/* Which side (LAYOUT_SIDE_*) an inset property sets; every side for the
 * shorthands, 0x10 for the transform properties. */
static unsigned style_offset_sides(const char *p)
{
    if (strcasecmp(p, "left") == 0) return LAYOUT_SIDE_LEFT;
    if (strcasecmp(p, "right") == 0) return LAYOUT_SIDE_RIGHT;
    if (strcasecmp(p, "top") == 0) return LAYOUT_SIDE_TOP;
    if (strcasecmp(p, "bottom") == 0) return LAYOUT_SIDE_BOTTOM;
    if (strncasecmp(p, "inset", 5) == 0) return LAYOUT_SIDE_LEFT | LAYOUT_SIDE_RIGHT | LAYOUT_SIDE_TOP | LAYOUT_SIDE_BOTTOM;
    return 0x10;
}

/* The furthest reach (css_layout_inline_style_reach) of the declarations
 * that differ between inline styles `a` and `b`. */
static int inline_style_reach(const char *a, const char *b, unsigned *sides)
{
    if (sides) *sides = 0;
    int na = 0, nb = 0;
    CssDeclaration *da = css_parse_inline_style(a ? a : "", &na);
    CssDeclaration *db = css_parse_inline_style(b ? b : "", &nb);
    int reach = 0;
    for (int side = 0; side < 2 && reach < 3; side++) {
        const CssDeclaration *d = side ? db : da;
        int n = side ? nb : na;
        for (int i = 0; i < n && reach < 3; i++) {
            const char *prop = d[i].property;
            if (!prop) continue;
            const char *va = inline_decl_value(da, na, prop), *vb = inline_decl_value(db, nb, prop);
            if (va && vb && strcmp(va, vb) == 0) continue;
            int r = style_prop_paint_only(prop) ? 0 : style_prop_offset(prop) ? 1 : style_prop_box_local(prop) ? 2 : 3;
            if (r == 1 && sides) *sides |= style_offset_sides(prop);
            if (r > reach) reach = r;
        }
    }
    css_declarations_free(da, na);
    css_declarations_free(db, nb);
    return reach;
}

/* Does some selector test attribute `name` in a compound the element
 * could match (by its classes and id, before or after)? */
static bool attr_tested(const StyleMentions *m, const LayoutAttrOwner *o, const char *name)
{
    uint64_t k = has_key('[', name, strlen(name));
    if (keyset_has(&m->attr_any, k)) return true;
    if (!keyset_has(&m->attr_named, k)) return false;
    if (!o) return true;
    for (int side = 0; side < 2; side++) {
        const char *id = o->id[side];
        if (id && id[0] && keyset_has(&m->attr_guarded, attr_guard_key(k, has_key('#', id, strlen(id))))) return true;
        for (const char *c = o->cls[side] ? o->cls[side] : ""; *c;) {
            while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r' || *c == '\f') c++;
            const char *e = c;
            while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r' && *e != '\f') e++;
            if (e > c && keyset_has(&m->attr_guarded, attr_guard_key(k, has_key('.', c, (size_t)(e - c))))) return true;
            c = e;
        }
    }
    return false;
}

int css_layout_inline_style_reach(const LayoutContext *ctx, const LayoutAttrOwner *owner, const char *a,
                                  const char *b, unsigned *sides)
{
    if (sides) *sides = 0;
    const StyleMentions *m = mentions_of(ctx);
    if (!m || attr_tested(m, owner, "style")) return 3;
    return inline_style_reach(a, b, sides);
}

/* Could replacing inline style `a` by `b` move or resize a box? */
static bool inline_style_change_matters(const char *a, const char *b)
{
    return inline_style_reach(a, b, NULL) > 0;
}

bool css_layout_attr_change_matters(const LayoutContext *ctx, const LayoutAttrOwner *owner, const char *name,
                                    const char *old_v, const char *new_v)
{
    const StyleMentions *m = mentions_of(ctx);
    if (!m || !name) return true;
    if (strcasecmp(name, "style") == 0)
        return attr_tested(m, owner, "style") || inline_style_change_matters(old_v, new_v);
    if (layout_reads_attr(name)) return true;
    if (attr_tested(m, owner, name)) return true;
    if (strcasecmp(name, "class") == 0)
        return class_tokens_mentioned(m, old_v, new_v) || class_tokens_mentioned(m, new_v, old_v);
    if (strcasecmp(name, "id") == 0) {
        if (old_v && old_v[0] && keyset_has(&m->keys, has_key('#', old_v, strlen(old_v)))) return true;
        if (new_v && new_v[0] && keyset_has(&m->keys, has_key('#', new_v, strlen(new_v)))) return true;
    }
    return false;
}

/* A class token in `a` but not in `b` that some rightmost compound names? */
static bool class_tokens_in(const StyleKeySet *s, const char *a, const char *b)
{
    for (const char *c = a ? a : ""; *c;) {
        while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r' || *c == '\f') c++;
        const char *e = c;
        while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r' && *e != '\f') e++;
        size_t n = (size_t)(e - c);
        if (n && !class_has_token(b, c, n) && keyset_has(s, has_key('.', c, n))) return true;
        c = e;
    }
    return false;
}

bool css_layout_attr_change_restyles_self(const LayoutContext *ctx, const LayoutAttrOwner *owner, const char *name,
                                          const char *old_v, const char *new_v)
{
    const StyleMentions *m = mentions_of(ctx);
    if (!m || m->unknown || m->subj_all || !name) return css_layout_attr_change_matters(ctx, owner, name, old_v, new_v);
    if (!css_layout_attr_change_matters(ctx, owner, name, old_v, new_v)) return false;
    if (strcasecmp(name, "style") == 0 || layout_reads_attr(name)) return true;
    if (keyset_has(&m->subj, has_key('[', name, strlen(name)))) return true;
    if (strcasecmp(name, "class") == 0)
        return class_tokens_in(&m->subj, old_v, new_v) || class_tokens_in(&m->subj, new_v, old_v);
    if (strcasecmp(name, "id") == 0) {
        if (old_v && old_v[0] && keyset_has(&m->subj, has_key('#', old_v, strlen(old_v)))) return true;
        if (new_v && new_v[0] && keyset_has(&m->subj, has_key('#', new_v, strlen(new_v)))) return true;
    }
    return false;
}

bool css_layout_has_any(const LayoutContext *ctx)
{
    const StyleMentions *m = mentions_of(ctx);
    return !m || m->unknown || m->has.any;
}

bool css_layout_has_sibling_relative(const LayoutContext *ctx)
{
    const StyleMentions *m = mentions_of(ctx);
    return !m || m->unknown || m->has.sibling_relative;
}

bool css_layout_uses_empty(const LayoutContext *ctx)
{
    const StyleMentions *m = mentions_of(ctx);
    return !m || m->empty;
}

bool css_layout_uses_last_of(const LayoutContext *ctx)
{
    const StyleMentions *m = mentions_of(ctx);
    return !m || m->unknown || m->last_of;
}

static void ctx_features_free(StyleCtxFeatures *f)
{
    free(f->keys.slot);
    for (uint32_t i = 0; i < f->inval_n; i++) free(f->inval[i].targets);
    free(f->inval);
    free(f->inval_key);
    free(f->inval_idx);
    memset(f, 0, sizeof(*f));
}

static const StyleInval *ctx_inval_find(const StyleCtxFeatures *f, uint64_t k)
{
    if (!f->inval_cap) return NULL;
    if (!k) k = 1;
    uint32_t h = (uint32_t)(k >> 7) & (f->inval_cap - 1);
    while (f->inval_key[h]) {
        if (f->inval_key[h] == k) return &f->inval[f->inval_idx[h]];
        h = (h + 1) & (f->inval_cap - 1);
    }
    return NULL;
}

static StyleInval *ctx_inval_get(StyleCtxFeatures *f, uint64_t k)
{
    if (!k) k = 1;
    StyleInval *found = (StyleInval *)ctx_inval_find(f, k);
    if (found) return found;
    if (2 * (f->inval_n + 1) > f->inval_cap) {
        uint32_t nc = f->inval_cap ? f->inval_cap * 2 : 1024;
        uint64_t *nk = (uint64_t *)calloc(nc, sizeof(uint64_t));
        int *ni = (int *)calloc(nc, sizeof(int));
        StyleInval *nv = (StyleInval *)realloc(f->inval, (size_t)nc * sizeof(StyleInval));
        if (!nk || !ni || !nv) { free(nk); free(ni); if (nv) f->inval = nv; return NULL; }
        f->inval = nv;
        for (uint32_t i = 0; i < f->inval_cap; i++) {
            if (!f->inval_key[i]) continue;
            uint32_t h = (uint32_t)(f->inval_key[i] >> 7) & (nc - 1);
            while (nk[h]) h = (h + 1) & (nc - 1);
            nk[h] = f->inval_key[i];
            ni[h] = f->inval_idx[i];
        }
        free(f->inval_key);
        free(f->inval_idx);
        f->inval_key = nk;
        f->inval_idx = ni;
        f->inval_cap = nc;
    }
    uint32_t h = (uint32_t)(k >> 7) & (f->inval_cap - 1);
    while (f->inval_key[h]) h = (h + 1) & (f->inval_cap - 1);
    f->inval_key[h] = k;
    f->inval_idx[h] = (int)f->inval_n;
    StyleInval *v = &f->inval[f->inval_n++];
    memset(v, 0, sizeof(*v));
    return v;
}

static void ctx_add(StyleCtxFeatures *f, char kind, const char *s, size_t n)
{
    bool fail = false;
    uint64_t k = has_key(kind, s, n);
    keyset_add(&f->keys, k, &fail);
    StyleInval *v = fail ? NULL : ctx_inval_get(f, k);
    if (!v) { f->all = true; return; }
    if (f->nested) { v->whole = v->sibling = true; return; }
    if (f->cur_sibling) v->sibling = true;
    else if (!f->cur_target) v->whole = true;
    else if (!v->whole) {
        for (int i = 0; i < v->n; i++) if (v->targets[i] == f->cur_target) return;
        if (v->n == v->cap) {
            int nc = v->cap ? v->cap * 2 : 4;
            uint64_t *nt = (uint64_t *)realloc(v->targets, (size_t)nc * sizeof(uint64_t));
            if (!nt) { v->whole = true; return; }
            v->targets = nt;
            v->cap = nc;
        }
        v->targets[v->n++] = f->cur_target;
    }
}

static bool ctx_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '-' || c == '_' || (unsigned char)c >= 0x80;
}

/* End of the bracket or paren group opening at `s` (quotes respected). */
static const char *ctx_skip_group(const char *s, const char *e)
{
    if (s < e && (*s == '"' || *s == '\'')) {
        char q = *s++;
        for (; s < e; s++) {
            if (*s == '\\' && s + 1 < e) s++;
            else if (*s == q) return s + 1;
        }
        return e;
    }
    int d = 0;
    char q = 0;
    for (; s < e; s++) {
        char c = *s;
        if (q) { if (c == '\\' && s + 1 < e) s++; else if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') q = c;
        else if (c == '(' || c == '[') d++;
        else if ((c == ')' || c == ']') && --d == 0) return s + 1;
    }
    return e;
}

/* The attribute name of "[name...]" starting at `q`. */
static const char *ctx_attr_name(const char *q, const char *e, size_t *len)
{
    const char *t = q + 1;
    while (t < e && *t == ' ') t++;
    const char *ns = t;
    while (t < e && (ctx_ident_char(*t) || *t == '|' || *t == '*')) t++;
    /* [ns|attr]: the name after the bar. */
    const char *bar = ns;
    for (const char *b = ns; b < t; b++) if (*b == '|' && b + 1 < t && b[1] != '=') bar = b + 1;
    *len = (size_t)(t - bar);
    return bar;
}

/* One feature every element matching compound [s, e) carries (id, then
 * class, attribute name, tag), or 0. */
static uint64_t ctx_compound_target(const char *s, const char *e)
{
    uint64_t tag = 0, cls = 0, attr = 0;
    const char *q = s;
    for (const char *b = s; b < e; b++) if (*b == '\\') return 0;
    if (q < e && ctx_ident_char(*q)) {
        const char *t = q;
        while (t < e && ctx_ident_char(*t)) t++;
        tag = has_key('t', q, (size_t)(t - q));
        q = t;
    }
    while (q < e) {
        char c = *q;
        if (c == '.' || c == '#') {
            const char *t = ++q;
            while (t < e && ctx_ident_char(*t)) t++;
            if (t > q) {
                if (c == '#') return has_key('#', q, (size_t)(t - q));
                if (!cls) cls = has_key('.', q, (size_t)(t - q));
            }
            q = t;
        } else if (c == '[') {
            size_t nl;
            const char *name = ctx_attr_name(q, e, &nl);
            if (nl && !attr) attr = has_key('[', name, nl);
            q = ctx_skip_group(q, e);
        } else if (c == '(' || c == '"' || c == '\'') {
            q = ctx_skip_group(q, e);
        } else {
            q++;
        }
    }
    return cls ? cls : attr ? attr : tag;
}

static void ctx_scan_list(StyleCtxFeatures *f, const char *s, const char *e, bool ctx);

static void ctx_scan_compound(StyleCtxFeatures *f, const char *s, const char *e, bool ctx)
{
    const char *q = s;
    if (q < e && ctx_ident_char(*q)) {
        const char *t = q;
        while (t < e && ctx_ident_char(*t)) t++;
        if (ctx) ctx_add(f, 't', q, (size_t)(t - q));
        q = t;
    }
    while (q < e) {
        char c = *q;
        if (c == '\\' || c == '&') { if (ctx) f->all = true; q++; continue; }
        if (c == '.' || c == '#') {
            const char *t = ++q;
            while (t < e && ctx_ident_char(*t)) t++;
            if (ctx && t > q) ctx_add(f, c, q, (size_t)(t - q));
            q = t;
        } else if (c == '[') {
            size_t nl;
            const char *name = ctx_attr_name(q, e, &nl);
            if (ctx && nl) ctx_add(f, '[', name, nl);
            q = ctx_skip_group(q, e);
        } else if (c == ':') {
            bool element = q + 1 < e && q[1] == ':';
            q += element ? 2 : 1;
            const char *name = q;
            while (q < e && ctx_ident_char(*q)) q++;
            size_t nl = (size_t)(q - name);
            const char *as = NULL, *ae = NULL;
            if (q < e && *q == '(') {
                const char *g = ctx_skip_group(q, e);
                as = q + 1;
                ae = g > as ? g - 1 : as;
                q = g;
            }
            if (element) continue;
#define PN(lit) (nl == sizeof(lit) - 1 && strncasecmp(name, lit, nl) == 0)
            if (PN("not") || PN("is") || PN("where") || PN("matches") || PN("-webkit-any") || PN("-moz-any")) {
                if (as) { f->nested++; ctx_scan_list(f, as, ae, ctx); f->nested--; }
                continue;
            }
            if (PN("has")) continue;
            if (PN("nth-child") || PN("nth-last-child")) {
                /* :nth-child(An+B of S) */
                for (const char *o = as; o && o + 4 <= ae; o++)
                    if (strncasecmp(o, " of ", 4) == 0) { f->nested++; ctx_scan_list(f, o + 4, ae, ctx); f->nested--; break; }
            }
            if (!ctx) continue;
            if (PN("first-child")) f->s_first = true;
            else if (PN("nth-child")) f->s_pos = true;
            else if (PN("last-child")) f->s_last = true;
            else if (PN("only-child")) f->s_first = f->s_last = true;
            else if (PN("nth-last-child")) f->s_fromend = true;
            else if (PN("first-of-type") || PN("nth-of-type")) f->s_pos = f->all_tags = true;
            else if (PN("last-of-type") || PN("nth-last-of-type")) f->s_fromend = f->all_tags = true;
            else if (PN("only-of-type")) f->s_pos = f->s_fromend = f->all_tags = true;
            else if (PN("empty")) f->s_empty = true;
            else if (PN("checked")) ctx_add(f, '[', "checked", 7);
            else if (PN("disabled") || PN("enabled")) ctx_add(f, '[', "disabled", 8);
            else if (PN("required") || PN("optional")) ctx_add(f, '[', "required", 8);
            else if (PN("read-only") || PN("read-write")) {
                ctx_add(f, '[', "readonly", 8);
                ctx_add(f, '[', "contenteditable", 15);
                ctx_add(f, '[', "disabled", 8);
            }
            else if (PN("placeholder-shown")) { ctx_add(f, '[', "placeholder", 11); ctx_add(f, '[', "value", 5); }
            else if (PN("link") || PN("any-link") || PN("-webkit-any-link")) ctx_add(f, '[', "href", 4);
            else if (PN("lang")) ctx_add(f, '[', "lang", 4);
            else if (PN("dir")) ctx_add(f, '[', "dir", 3);
            else if (PN("open") || PN("closed")) ctx_add(f, '[', "open", 4);
            else if (PN("hover") || PN("active") || PN("focus") || PN("focus-within") || PN("focus-visible") ||
                     PN("visited") || PN("target") || PN("target-within") || PN("scope") || PN("root") ||
                     PN("defined") || PN("host") || PN("fullscreen") || PN("-webkit-full-screen") ||
                     PN("autofill") || PN("-webkit-autofill") || PN("popover-open") || PN("modal") ||
                     PN("playing") || PN("paused") || PN("picture-in-picture"))
                ;   /* not modelled from the DOM (as for the element itself) */
            else
                f->all = true;
#undef PN
        } else {
            q++;
        }
    }
}

#define CTX_MAX_COMPOUNDS 64

/* One complex selector: every compound followed by a combinator is context,
 * invalidating the rightmost compound's elements below it (or, before a
 * sibling combinator, later siblings). */
static void ctx_scan_complex(StyleCtxFeatures *f, const char *s, const char *e, bool ctx)
{
    const char *cs[CTX_MAX_COMPOUNDS], *ce[CTX_MAX_COMPOUNDS];
    bool sib_after[CTX_MAX_COMPOUNDS];
    int nc = 0;
    bool overflow = false;
    while (s < e && (*s == ' ' || *s == '\n' || *s == '\t')) s++;
    /* Relative selectors (in :is() arguments too) may lead with a combinator. */
    const char *start = s;
    const char *q = s;
    while (q < e) {
        char c = *q;
        if (c == '(' || c == '[' || c == '"' || c == '\'') { q = ctx_skip_group(q, e); continue; }
        if (c == ' ' || c == '>' || c == '+' || c == '~' || c == '\n' || c == '\t') {
            const char *end = q;
            bool sib = false;
            while (q < e && (*q == ' ' || *q == '>' || *q == '+' || *q == '~' || *q == '\n' || *q == '\t')) {
                if (*q == '+') { f->sibling = true; f->s_prev = true; sib = true; }
                if (*q == '~') { f->sibling = true; f->s_pos = true; sib = true; }
                q++;
            }
            if (end > start) {
                if (nc < CTX_MAX_COMPOUNDS) { cs[nc] = start; ce[nc] = end; sib_after[nc] = sib; nc++; }
                else overflow = true;
            }
            start = q;
            continue;
        }
        q++;
    }
    if (q > start) {
        if (nc < CTX_MAX_COMPOUNDS) { cs[nc] = start; ce[nc] = q; sib_after[nc] = false; nc++; }
        else overflow = true;
    }
    if (!nc) return;
    uint64_t target = overflow ? 0 : ctx_compound_target(cs[nc - 1], ce[nc - 1]);
    if (overflow) f->nested++;
    for (int i = 0; i < nc; i++) {
        bool context = i < nc - 1;
        f->cur_target = target;
        f->cur_sibling = context && sib_after[i];
        ctx_scan_compound(f, cs[i], ce[i], ctx || context);
    }
    if (overflow) f->nested--;
}

static void ctx_scan_list(StyleCtxFeatures *f, const char *s, const char *e, bool ctx)
{
    const char *cs = s;
    const char *q = s;
    while (q < e) {
        char c = *q;
        if (c == '(' || c == '[' || c == '"' || c == '\'') { q = ctx_skip_group(q, e); continue; }
        if (c == ',') { ctx_scan_complex(f, cs, q, ctx); cs = ++q; continue; }
        q++;
    }
    if (q > cs) ctx_scan_complex(f, cs, q, ctx);
}

static void ctx_buf_push(StyleCtxBuf *b, uint64_t k, uint64_t v)
{
    if (b->n == b->cap) {
        int nc = b->cap ? b->cap * 2 : 4096;
        StyleCtxItem *ni = (StyleCtxItem *)realloc(b->items, (size_t)nc * sizeof(StyleCtxItem));
        if (!ni) { b->fail = true; return; }
        b->items = ni;
        b->cap = nc;
    }
    b->items[b->n].k = k;
    b->items[b->n].v = v;
    b->n++;
}

static int ctx_item_cmp(const StyleCtxItem *a, const StyleCtxItem *b)
{
    if (a->k != b->k) return a->k < b->k ? -1 : 1;
    if (a->v != b->v) return a->v < b->v ? -1 : 1;
    return 0;
}

/* The context features of an element, appended to `b` sorted and unique;
 * returns the hash of the rest (structure as in its key: parent id,
 * position, element siblings, previous and next ids, children). The parent
 * always counts: a moved element has new ancestors. Tags are left out, as
 * an element keeps its tag. */
static uint64_t style_ctx_items(const StyleCtxFeatures *f, const HtmlNode *hn, const uint64_t s[6],
                                StyleCtxBuf *b)
{
    uint64_t h = 1469598103934665603ull;
    h = style_mix(h, &s[0], sizeof(s[0]));
    if (f->all) {
        if (hn->tag_name) h = style_mix(h, hn->tag_name, strlen(hn->tag_name) + 1);
        for (HtmlAttribute *a = hn->attributes; a; a = a->next) {
            if (a->name) h = style_mix(h, a->name, strlen(a->name) + 1);
            if (a->value) h = style_mix(h, a->value, strlen(a->value) + 1);
        }
        return style_mix(h, s, 6 * sizeof(s[0]));
    }
    int first = b->n;
    for (HtmlAttribute *a = hn->attributes; a; a = a->next) {
        if (!a->name) continue;
        const char *v = a->value ? a->value : "";
        size_t nl = strlen(a->name);
        uint64_t ak = has_key('[', a->name, nl);
        if (keyset_has(&f->keys, ak)) {
            uint64_t vh = style_mix(1469598103934665603ull, v, strlen(v) + 1);
            ctx_buf_push(b, ak, vh ? vh : 1);
        }
        if (strcmp(a->name, "id") == 0) {
            if (v[0]) {
                uint64_t k = has_key('#', v, strlen(v));
                if (keyset_has(&f->keys, k)) ctx_buf_push(b, k, 0);
            }
        } else if (strcmp(a->name, "class") == 0) {
            for (const char *c = v; *c;) {
                while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r' || *c == '\f') c++;
                const char *t = c;
                while (*t && *t != ' ' && *t != '\t' && *t != '\n' && *t != '\r' && *t != '\f') t++;
                if (t > c) {
                    uint64_t k = has_key('.', c, (size_t)(t - c));
                    if (keyset_has(&f->keys, k)) ctx_buf_push(b, k, 0);
                }
                c = t;
            }
        }
    }
    /* Sort (few items) and drop duplicate classes. */
    StyleCtxItem *it = b->items + first;
    int m = b->fail ? 0 : b->n - first;
    for (int i = 1; i < m; i++) {
        StyleCtxItem x = it[i];
        int j = i - 1;
        while (j >= 0 && ctx_item_cmp(&it[j], &x) > 0) { it[j + 1] = it[j]; j--; }
        it[j + 1] = x;
    }
    int u = 0;
    for (int i = 0; i < m; i++) if (!u || ctx_item_cmp(&it[u - 1], &it[i]) != 0) it[u++] = it[i];
    if (!b->fail) b->n = first + u;
    uint64_t v[6] = {
        f->s_pos ? s[1] : 0,
        f->s_first ? (s[1] == 0) : 0,
        f->s_prev ? s[3] : 0,
        f->s_last ? (s[4] == 0) : 0,
        f->s_fromend ? s[2] - s[1] : 0,
        f->s_empty ? (s[5] == 0) : 0,
    };
    h = style_mix(h, v, sizeof(v));
    return h;
}

/* Does element `hn` carry one of the features in `t`? */
static bool ctx_targets_match(const StyleKeySet *t, const HtmlNode *hn)
{
    if (hn->tag_name && keyset_has(t, has_key('t', hn->tag_name, strlen(hn->tag_name)))) return true;
    for (HtmlAttribute *a = hn->attributes; a; a = a->next) {
        if (!a->name) continue;
        if (keyset_has(t, has_key('[', a->name, strlen(a->name)))) return true;
        const char *v = a->value ? a->value : "";
        if (strcmp(a->name, "id") == 0) {
            if (v[0] && keyset_has(t, has_key('#', v, strlen(v)))) return true;
        } else if (strcmp(a->name, "class") == 0) {
            for (const char *c = v; *c;) {
                while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r' || *c == '\f') c++;
                const char *e = c;
                while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r' && *e != '\f') e++;
                if (e > c && keyset_has(t, has_key('.', c, (size_t)(e - c)))) return true;
                c = e;
            }
        }
    }
    return false;
}

/* The structure part of element c's key (style_cache_prepare): its parent,
 * its position among the element children (key nonzero), their count, its
 * element neighbours and its own child count. */
static void style_struct_of(LayoutContext *ctx, const uint64_t *key, int c, uint64_t s[6])
{
    int idx = ctx->tree.nodes[c].parent_idx;
    int elems = 0, pos = 0;
    NodeId prev = 0, next = 0, pid = 0;
    HtmlNode *pn = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    if (pn) pid = pn->store_id;
    bool after = false;
    for (int d = ctx->tree.nodes[idx].first_child_idx; d >= 0; d = ctx->tree.nodes[d].next_sibling_idx) {
        if (!key[d]) continue;
        elems++;
        if (d == c) { after = true; continue; }
        HtmlNode *dn = layout_node_dom(ctx, ctx->tree.nodes[d].dom_node_idx);
        if (!after) { pos++; prev = dn ? dn->store_id : 0; }
        else if (!next) next = dn ? dn->store_id : 0;
    }
    int ckids = 0;
    for (int g = ctx->tree.nodes[c].first_child_idx; g >= 0; g = ctx->tree.nodes[g].next_sibling_idx) ckids++;
    if (ctx->pruned && ctx->pruned[c]) {
        PreOrderCompactionArray *da = &ctx->doc->array;
        for (int g = po_array_first_child(da, ctx->tree.nodes[c].dom_node_idx); g >= 0; g = po_array_next_sibling(da, g))
            if (po_array_is_active(da, g)) ckids++;
    }
    s[0] = (uint64_t)pid; s[1] = (uint64_t)pos; s[2] = (uint64_t)elems;
    s[3] = (uint64_t)prev; s[4] = (uint64_t)next; s[5] = (uint64_t)ckids;
}

/* Element keys and the reuse flags for every layout node (serial, before
 * the parallel style pass). */
static void style_cache_prepare(StylePass *P, uint64_t global, const HasSubjects *has,
                                const StyleCtxFeatures *cf)
{
    LayoutContext *ctx = P->ctx;
    int n = ctx->tree.count;
    P->cached = (const StyleCacheEntry **)calloc((size_t)n + 1, sizeof(*P->cached));
    P->key_ok = (unsigned char *)calloc((size_t)n + 1, 1);
    P->force = (unsigned char *)calloc((size_t)n + 1, 1);
    g_sc.has_has = has && has->any;
    P->out_changed = (unsigned char *)calloc((size_t)n + 1, 1);
    P->match_hash = (uint64_t *)calloc((size_t)n + 1, sizeof(uint64_t));
    P->cx_off = (int *)calloc((size_t)n + 1, sizeof(int));
    P->cx_cnt = (int *)malloc(((size_t)n + 1) * sizeof(int));
    P->cx_struct = (uint64_t *)calloc((size_t)n + 1, sizeof(uint64_t));
    uint64_t *key = (uint64_t *)calloc((size_t)n + 1, sizeof(uint64_t));
    uint64_t (*S)[6] = (uint64_t (*)[6])calloc((size_t)n + 1, sizeof(*S));
    unsigned char *changed = (unsigned char *)calloc((size_t)n + 1, 1);
    unsigned char *whole = (unsigned char *)calloc((size_t)n + 1, 1);   /* subtree */
    unsigned char *sibch = (unsigned char *)calloc((size_t)n + 1, 1);   /* later siblings */
    int *part = (int *)malloc(((size_t)n + 1) * sizeof(int));          /* partial invalidation */
    int *depth = (int *)calloc((size_t)n + 1, sizeof(int));
    int *at = (int *)calloc((size_t)n + 1, sizeof(int));                /* preorder position */
    P->entry_key = key;
    if (!P->cached || !P->key_ok || !P->force || !P->out_changed || !P->cx_off || !P->cx_cnt ||
        !P->cx_struct || !key || !S || !changed || !whole || !sibch || !part || !depth || !at) {
        P->inc = false;
        free(S); free(changed); free(whole); free(sibch); free(part); free(depth); free(at);
        return;
    }
    for (int i = 0; i <= n; i++) P->cx_cnt[i] = -1;
    bool usable = g_sc.global == global && g_sc.n > 0;
    /* On a kept tree, the elements whose attributes changed and those the
     * last store did not note (redo[idx]); the others' keys and entries are
     * as noted (no element's structure changed). */
    unsigned char *redo = NULL;
    if (P->retain) {
        redo = (unsigned char *)calloc((size_t)n + 1, 1);
        if (!redo || !usable) P->retain = false;
    }
    if (P->retain) {
        for (int idx = 0; idx < n; idx++) redo[idx] = ctx->sc_entry[idx] == 0;
        for (int k = 0; k < ctx->attr_n; k++) {
            int d = html_mirror_index_of(ctx->doc, ctx->attr_ids[k]);
            int li = d >= 0 && d < ctx->dom_map_n ? ctx->dom_map[d] : -1;
            if (li >= 0) redo[li] = 1;
        }
        /* (an element whose children changed: their structure, and its own
         * child count) */
        for (int k = 0; k < ctx->child_n; k++) {
            int d = html_mirror_index_of(ctx->doc, ctx->child_ids[k]);
            int li = d >= 0 && d < ctx->dom_map_n ? ctx->dom_map[d] : -1;
            if (li < 0) continue;
            redo[li] = 1;
            for (int c = ctx->tree.nodes[li].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) redo[c] = 1;
        }
        for (int idx = 0; idx < n; idx++) if (!redo[idx]) key[idx] = ctx->sc_key[idx];
    }

    /* Keys: own tag and attributes (kept on the node until its attribute
     * list changes), then structure from the parent's view. */
    for (int idx = 0; idx < n; idx++) {
        if (redo && !redo[idx]) continue;
        HtmlNode *hn = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
        if (!hn || hn->type != HTML_NODE_ELEMENT) continue;
        uint64_t h = hn->attr_hash;
        if (!h) {
            h = 1469598103934665603ull;
            if (hn->tag_name) h = style_mix(h, hn->tag_name, strlen(hn->tag_name) + 1);
            for (HtmlAttribute *a = hn->attributes; a; a = a->next) {
                if (a->name) h = style_mix(h, a->name, strlen(a->name) + 1);
                if (a->value) h = style_mix(h, a->value, strlen(a->value) + 1);
            }
            if (!h) h = 1;
            hn->attr_hash = h;
        }
        key[idx] = h;
    }
    for (int idx = 0; idx < n; idx++) {
        if (redo) {
            /* (only parents of elements redone; and their structure only) */
            bool any = false;
            for (int c = ctx->tree.nodes[idx].first_child_idx; !any && c >= 0; c = ctx->tree.nodes[c].next_sibling_idx)
                any = redo[c] && key[c];
            if (!any) continue;
        }
        int elems = 0;
        for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx)
            if (key[c]) elems++;
        int pos = 0;
        NodeId prev = 0;
        NodeId pid = 0;
        HtmlNode *pn = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
        if (pn) pid = pn->store_id;
        for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
            if (!key[c]) continue;
            NodeId next = 0;
            for (int d = ctx->tree.nodes[c].next_sibling_idx; d >= 0; d = ctx->tree.nodes[d].next_sibling_idx) {
                if (!key[d]) continue;
                HtmlNode *dn = layout_node_dom(ctx, ctx->tree.nodes[d].dom_node_idx);
                next = dn ? dn->store_id : 0;
                break;
            }
            uint64_t *s = S[c];
            s[0] = (uint64_t)pid; s[1] = (uint64_t)pos; s[2] = (uint64_t)elems;
            s[3] = (uint64_t)prev; s[4] = (uint64_t)next;
            HtmlNode *cn = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
            int ckids = 0;
            for (int g = ctx->tree.nodes[c].first_child_idx; g >= 0; g = ctx->tree.nodes[g].next_sibling_idx) ckids++;
            if (ctx->pruned && ctx->pruned[c]) {   /* its children left out: the document's */
                PreOrderCompactionArray *da = &ctx->doc->array;
                for (int g = po_array_first_child(da, ctx->tree.nodes[c].dom_node_idx); g >= 0; g = po_array_next_sibling(da, g))
                    if (po_array_is_active(da, g)) ckids++;
            }
            s[5] = (uint64_t)ckids;
            if (!redo || redo[c]) key[c] = style_mix(key[c], s, 6 * sizeof(uint64_t));
            prev = cn ? cn->store_id : 0;
            pos++;
        }
    }
    /* Cache lookup; context features of every element that changed, and
     * what of them changed (the root has no parent in the layout tree and
     * keeps an all-zero structure). */
    StyleCtxBuf *b = &P->cx;
    b->n = 0;
    b->fail = false;
    int nparts = 0;
    for (int idx = 0; idx < n; idx++) {
        part[idx] = -1;
        if (!key[idx]) continue;
        if (redo && !redo[idx]) {
            const StyleCacheEntry *e = &g_sc.e[ctx->sc_entry[idx] - 1];
            P->cached[idx] = e;
            P->key_ok[idx] = e->key == key[idx];
            changed[idx] = !P->key_ok[idx];
            if (P->key_ok[idx]) continue;
            /* (an entry left stale: its context features as below; its
             * structure was not computed above) */
            redo[idx] = 1;
            int p = ctx->tree.nodes[idx].parent_idx;
            if (p >= 0) style_struct_of(ctx, key, idx, S[idx]);
        }
        HtmlNode *hn = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
        const StyleCacheEntry *e = (usable && hn) ? style_cache_find(hn->store_id) : NULL;
        P->cached[idx] = e;
        P->key_ok[idx] = e && e->key == key[idx];
        changed[idx] = !P->key_ok[idx];
        if (P->key_ok[idx] || !hn) continue;
        P->cx_off[idx] = b->n;
        P->cx_struct[idx] = style_ctx_items(cf, hn, S[idx], b);
        if (b->fail) { P->inc = false; break; }
        P->cx_cnt[idx] = b->n - P->cx_off[idx];
        if (!e || e->ctx_struct != P->cx_struct[idx]) {
            whole[idx] = 1;
            sibch[idx] = 1;
            continue;
        }
        /* Merge the sorted old and new items: keys on one side only. */
        const StyleCtxItem *o = e->ctx_items, *w = b->items + P->cx_off[idx];
        int on = (int)e->ctx_n, wn = P->cx_cnt[idx], i = 0, j = 0;
        bool any_part = false;
        while (i < on || j < wn) {
            int c = i >= on ? 1 : j >= wn ? -1 : ctx_item_cmp(&o[i], &w[j]);
            if (c == 0) { i++; j++; continue; }
            uint64_t k = c < 0 ? o[i++].k : w[j++].k;
            const StyleInval *v = ctx_inval_find(cf, k);
            if (!v || v->whole) whole[idx] = 1;
            if (!v || v->sibling) sibch[idx] = 1;
            if (v && v->n) any_part = true;
        }
        if (any_part && !whole[idx]) { part[idx] = nparts++; }
    }
    free(redo);
    if (!P->inc) {
        free(S); free(changed); free(whole); free(sibch); free(part); free(depth); free(at);
        return;
    }
    /* Forcing (2: recompute this element and its whole subtree): later
     * siblings of an element whose sibling-visible features changed, then
     * down from whole-invalidated or forced ancestors. A change no context
     * compound sees recomputes the element alone, and its children follow
     * only if its result changed (out_changed). A change some selectors
     * see recomputes (1: self only) the descendants matching those
     * selectors' rightmost compounds. With :has(), a change anywhere below
     * recomputes the element itself (1) or, when the :has() compound has
     * more selector after it, its subtree (2). */
    const int *pre = ctx->tree.preorder;
    for (int idx = 0; idx < n && cf->sibling; idx++) {
        bool seen = false;
        for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
            if (!key[c]) continue;
            if (seen) P->force[c] = 2;
            if (sibch[c]) seen = true;
        }
    }
    for (int pos = 0; pos < n; pos++) {
        int idx = pre[pos];
        int p = ctx->tree.nodes[idx].parent_idx;
        at[idx] = pos;
        depth[idx] = p >= 0 ? depth[p] + 1 : 0;
        if (p >= 0 && (whole[p] || P->force[p] == 2 ||
                       (P->cached[p] && P->cached[p]->pruned_below && !(ctx->pruned && ctx->pruned[p])))) P->force[idx] = 2;
    }
    int npartial = 0;
    if (nparts) {
        StyleKeySet t = {0};
        for (int idx = 0; idx < n; idx++) {
            if (part[idx] < 0 || P->force[idx] == 2) continue;
            const StyleCacheEntry *e = P->cached[idx];
            const StyleCtxItem *o = e->ctx_items, *w = b->items + P->cx_off[idx];
            int on = (int)e->ctx_n, wn = P->cx_cnt[idx], i = 0, j = 0;
            bool fail = false;
            keyset_clear(&t);
            while (i < on || j < wn) {
                int c = i >= on ? 1 : j >= wn ? -1 : ctx_item_cmp(&o[i], &w[j]);
                if (c == 0) { i++; j++; continue; }
                const StyleInval *v = ctx_inval_find(cf, c < 0 ? o[i++].k : w[j++].k);
                for (int k = 0; v && k < v->n; k++) keyset_add(&t, v->targets[k], &fail);
            }
            for (int pos = at[idx] + 1; pos < n; pos++) {
                int d = pre[pos];
                if (depth[d] <= depth[idx]) break;
                if (fail) { P->force[d] = 2; continue; }
                if (P->force[d] || !key[d]) continue;
                HtmlNode *dn = layout_node_dom(ctx, ctx->tree.nodes[d].dom_node_idx);
                if (dn && ctx_targets_match(&t, dn)) { P->force[d] = 1; npartial++; }
            }
        }
        free(t.slot);
    }
    if (has && has->any) {
        unsigned char *below = (unsigned char *)calloc((size_t)n + 1, 1);
        if (below) {
            for (int pos = n - 1; pos >= 0; pos--) {
                int idx = pre[pos];
                int p = ctx->tree.nodes[idx].parent_idx;
                if (p >= 0 && (below[idx] || changed[idx])) below[p] = 1;
            }
            /* :has(+ x) / :has(~ x): a change in or under a later sibling
             * can flip an earlier sibling too. */
            if (has->sibling_relative) {
                for (int idx = 0; idx < n; idx++) {
                    int last = -1;
                    for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx)
                        if (key[c] && (below[c] || changed[c])) last = c;
                    /* (No changed child: no earlier sibling to flip.  Without
                     * this every child of every element was marked.) */
                    if (last < 0) continue;
                    for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0 && c != last; c = ctx->tree.nodes[c].next_sibling_idx)
                        if (key[c]) below[c] = 1;
                }
            }
            /* Only elements some :has() rule could be about. */
            bool any_change = false;
            for (int idx = 0; idx < n; idx++) {
                if (changed[idx] && key[idx]) any_change = true;
                if (!below[idx] || P->force[idx] == 2 || !key[idx]) continue;
                int m = has_may_match(has, ctx, idx);
                if (m > P->force[idx]) P->force[idx] = (unsigned char)m;
            }
            /* Universal :has() subjects with more selector after them: a
             * change anywhere can flip some ancestor, so the elements the
             * rightmost compounds can match are recomputed (their children
             * follow if their style changed). */
            if (any_change && has->deep_targets.n) {
                for (int idx = 0; idx < n; idx++) {
                    if (P->force[idx] || !key[idx]) continue;
                    if (has_memo_at(ctx, has, idx, 2)) P->force[idx] = 1;
                }
            }
            free(below);
            /* Subtrees under elements :has() forced whole. */
            for (int pos = 0; pos < n; pos++) {
                int idx = pre[pos];
                int p = ctx->tree.nodes[idx].parent_idx;
                if (p >= 0 && P->force[p] == 2) P->force[idx] = 2;
            }
        }
    }
    if (g_css_log > 1) {
        int nch = 0, nw = 0, nf = 0, nfc = 0, n1 = 0;
        for (int idx = 0; idx < n; idx++) {
            if (!key[idx]) continue;
            nch += changed[idx];
            nw += whole[idx];
            nf += P->force[idx] == 2;
            nfc += P->force[idx] == 2 && P->cached[idx];
            n1 += P->force[idx] == 1;
        }
        fprintf(stderr, "[css] restyle prepare: %d changed, %d invalidate subtree, %d partial (%d matched), "
                "%d subtree-forced (%d cached), %d self-forced\n", nch, nw, nparts, npartial, nf, nfc, n1);
    }
    free(S); free(changed); free(whole); free(sibch); free(part); free(depth); free(at);
}

/* StylePass.dirty_n from what style_cache_prepare found.  With skip_hidden,
 * elements below a display:none element that keeps its cached style (so
 * stays display:none) are not styled, and do not count. */
static void style_count_dirty(StylePass *P)
{
    LayoutContext *ctx = P->ctx;
    int n = ctx->tree.count;
    const int *pre = ctx->tree.preorder;
    P->dirty_n = (int *)calloc((size_t)n + 1, sizeof(int));
    unsigned char *under = P->skip_hidden ? (unsigned char *)calloc((size_t)n + 1, 1) : NULL;
    if (!P->dirty_n) { free(under); return; }
    for (int pos = 0; under && pos < n; pos++) {
        int idx = pre[pos];
        int p = ctx->tree.nodes[idx].parent_idx;
        if (p < 0) continue;
        const StyleCacheEntry *pe = P->cached[p];
        under[idx] = under[p] || (pe && !pe->stale && P->key_ok[p] && !P->force[p] &&
                                  pe->style.display == CSS_DISPLAY_NONE);
    }
    for (int pos = n - 1; pos >= 0; pos--) {
        int idx = pre[pos];
        const StyleCacheEntry *ce = P->cached[idx];
        if (P->entry_key[idx] && !(under && under[idx]) &&
            !(ce && !ce->stale && P->key_ok[idx] && !P->force[idx]))
            P->dirty_n[idx]++;
        int p = ctx->tree.nodes[idx].parent_idx;
        if (p >= 0) P->dirty_n[p] += P->dirty_n[idx];
    }
    free(under);
}

typedef struct { const void *from; const CssCustomProps *to; } StylePropsRemap;

/* `m` as a map the cache owns: maps of the previous cache stay as they are,
 * others are copied once (interned strings) into the cache arena. */
static const CssCustomProps *style_cache_own(const CssCustomProps *m, StylePropsRemap *tab, int cap)
{
    if (!m) return NULL;
    uint32_t h = (uint32_t)(((uintptr_t)m >> 4) * 2654435761u) & (uint32_t)(cap - 1);
    for (;;) {
        if (!tab[h].from) break;
        if (tab[h].from == m) return tab[h].to;
        h = (h + 1) & (uint32_t)(cap - 1);
    }
    CssCustomProps *c = (CssCustomProps *)frame_arena_alloc(&g_sc.arena, sizeof(CssCustomProps));
    c->count = m->count;
    c->capacity = m->count;
    c->props = (CssCustomProp *)frame_arena_alloc(&g_sc.arena, ((size_t)m->count + 1) * sizeof(CssCustomProp));
    for (int i = 0; i < m->count; i++) {
        const char *nm = m->props[i].name ? m->props[i].name : "";
        const char *vl = m->props[i].value ? m->props[i].value : "";
        c->props[i].name = (char *)layout_intern(nm, strlen(nm));
        c->props[i].value = (char *)layout_intern(vl, strlen(vl));
    }
    cp_index_build(&g_sc.arena, c, NULL);
    tab[h].from = m;
    tab[h].to = c;
    return c;
}

/* Remember this pass's post-cascade boxes for the next one. */
/* The context features of entry e: computed this pass, else those of
 * `old` (copied into the current arena). */
static void style_cache_set_ctx(StyleCacheEntry *e, StylePass *P, int idx, const StyleCacheEntry *old)
{
    const StyleCtxItem *src = NULL;
    uint32_t cnt = 0;
    if (P->cx_cnt && P->cx_cnt[idx] >= 0) {
        src = P->cx.items + P->cx_off[idx];
        cnt = (uint32_t)P->cx_cnt[idx];
        e->ctx_struct = P->cx_struct[idx];
    } else if (old) {
        src = old->ctx_items;
        cnt = old->ctx_n;
        e->ctx_struct = old->ctx_struct;
    } else {
        e->ctx_struct = 0;
    }
    StyleCtxItem *dst = NULL;
    if (cnt) {
        dst = (StyleCtxItem *)frame_arena_alloc(&g_sc.arena, (size_t)cnt * sizeof(StyleCtxItem));
        if (dst) memcpy(dst, src, (size_t)cnt * sizeof(StyleCtxItem));
        else { cnt = 0; e->ctx_struct = ~(uint64_t)0; }   /* never matches: next pass invalidates */
    }
    e->ctx_items = dst;
    e->ctx_n = cnt;
}

static uint32_t style_cache_next_ver(void)
{
    if (++g_sc_ver == 0) g_sc_ver = 1;
    return g_sc_ver;
}

/* What the store left for element idx, for the next style pass on the
 * kept tree (LayoutContext.sc_entry): its entry (index k, -1 none), its
 * key, and whether its style slot holds the entry's style (the slot of an
 * element left unstyled does not). */
static void style_slot_note(StylePass *P, int idx, int k, const StyleCacheEntry *e, bool holds)
{
    LayoutContext *ctx = P->ctx;
    if (!ctx->sc_entry) return;
    ctx->sc_entry[idx] = k + 1;
    ctx->sc_key[idx] = P->entry_key ? P->entry_key[idx] : 0;
    ctx->slot_ver[idx] = holds && e ? e->ver : 0;
}

static void style_cache_set(StyleCacheEntry *e, StylePass *P, int idx, const char *after,
                            StylePropsRemap *tab, int cap)
{
    LayoutContext *ctx = P->ctx;
    e->key = P->entry_key ? P->entry_key[idx] : 0;
    if (P->cx_cnt && P->cx_cnt[idx] >= 0) style_cache_set_ctx(e, P, idx, NULL);
    e->props = style_cache_own(ctx->custom_props[idx], tab, cap);
    e->match_hash = P->match_hash ? P->match_hash[idx] : 0;
    e->stale = 0;
    e->pruned_below = ctx->pruned && ctx->pruned[idx];
    e->has_after = after != NULL;
    if (after) memcpy(e->after, after, sizeof(e->after));
    else e->after[0] = '\0';
    e->pseudo = pseudo_rec_own(ctx->pseudo_rec[idx]);
    e->style = *ctx->boxes[idx].s;
    e->ver = style_cache_next_ver();
    HtmlNode *hn = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    if (hn) hn->hidden_hint = e->style.display == CSS_DISPLAY_NONE ? 1 : 2;
}

/* Entry e of an element left unstyled whose style may have changed: what
 * the next pass compares (key, context features) is brought up to date, so
 * it does not count as changed again; its style is unknown. */
static void style_cache_set_stale(StyleCacheEntry *e, StylePass *P, int idx, const StyleCacheEntry *old)
{
    e->key = P->entry_key ? P->entry_key[idx] : 0;
    if ((P->cx_cnt && P->cx_cnt[idx] >= 0) || !old) style_cache_set_ctx(e, P, idx, NULL);
    else if (old != e) style_cache_set_ctx(e, P, idx, old);
    e->props = NULL;
    e->match_hash = 0;
    e->stale = 1;
    e->pruned_below = old ? old->pruned_below : 0;
    e->has_after = 0;
    e->after[0] = '\0';
    e->pseudo = NULL;
    e->ver = style_cache_next_ver();
}

/* In-place form of style_cache_store for an unchanged global key: entries of
 * reused elements (and of recomputed ones with the same key and result) stay
 * as they are; false when out of memory (the caller rebuilds). */
static bool style_cache_update(StylePass *P, const StyleGenAfter *gen, int gen_total, int fresh)
{
    LayoutContext *ctx = P->ctx;
    int n = ctx->tree.count;
    int need = g_sc.n + fresh;
    if (need + 1 > g_sc.cap) {
        int nc = g_sc.cap ? g_sc.cap : 64;
        while (nc < need + 1) nc *= 2;
        StyleCacheEntry *ne = (StyleCacheEntry *)realloc(g_sc.e, (size_t)nc * sizeof(StyleCacheEntry));
        if (!ne) return false;
        /* P->cached points into the old array. */
        for (int idx = 0; idx < n; idx++)
            if (P->cached[idx]) P->cached[idx] = ne + (P->cached[idx] - g_sc.e);
        g_sc.e = ne;
        g_sc.cap = nc;
    }
    if (2 * (need + 1) > g_sc.index_cap) {
        int icap = g_sc.index_cap ? g_sc.index_cap : 64;
        while (icap < 2 * (need + 1)) icap *= 2;
        int *index = (int *)calloc((size_t)icap, sizeof(int));
        if (!index) return false;
        for (int k = 0; k < g_sc.n; k++) {
            uint32_t h = (g_sc.e[k].id * 2654435761u) & (uint32_t)(icap - 1);
            while (index[h]) h = (h + 1) & (uint32_t)(icap - 1);
            index[h] = k + 1;
        }
        free(g_sc.index);
        g_sc.index = index;
        g_sc.index_cap = icap;
    }
    const char **after = (const char **)calloc((size_t)n + 1, sizeof(char *));
    if (!after) return false;
    for (int i = 0; i < gen_total; i++) after[gen[i].idx] = gen[i].text;
    /* Remap table for maps not owned yet; owned ones map to themselves (only
     * those of written entries are looked up, so seed lazily from them). */
    int cap = 64;
    while (cap < 4 * (fresh + 64)) cap *= 2;
    StylePropsRemap *tab = NULL;
    int written = 0;
    for (int idx = 0; idx < n; idx++) {
        /* A slot left as it was: its entry, key and note are as they were
         * (what its cascade read too: the entry's). */
        if (P->kept && P->kept[idx]) continue;
        HtmlNode *hn = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
        if (!hn || hn->type != HTML_NODE_ELEMENT || !hn->store_id) continue;
        StyleCacheEntry *e = (StyleCacheEntry *)P->cached[idx];
        /* Not styled: an entry that still holds stays as it is. */
        if (P->skipped && P->skipped[idx] == 1) { style_slot_note(P, idx, e ? (int)(e - g_sc.e) : -1, e, false); continue; }
        if (P->skipped && P->skipped[idx] == 2) {
            if (e && e->stale && P->key_ok[idx]) { style_slot_note(P, idx, (int)(e - g_sc.e), e, false); continue; }
            const StyleCacheEntry *old = e;
            if (!e) {
                e = &g_sc.e[g_sc.n];
                e->id = hn->store_id;
                uint32_t h = (e->id * 2654435761u) & (uint32_t)(g_sc.index_cap - 1);
                while (g_sc.index[h]) h = (h + 1) & (uint32_t)(g_sc.index_cap - 1);
                g_sc.index[h] = ++g_sc.n;
            }
            style_cache_set_stale(e, P, idx, old);
            style_slot_note(P, idx, (int)(e - g_sc.e), e, false);
            continue;
        }
        if (e && P->key_ok[idx] && !P->out_changed[idx] &&
            e->has_after == (after[idx] != NULL) &&
            (!after[idx] || strncmp(e->after, after[idx], sizeof(e->after)) == 0)) {
            /* Same result: record what its cascade read this time (and
             * what its pseudo-elements' did). */
            if (P->match_hash) e->match_hash = P->match_hash[idx];
            if (e->pseudo != ctx->pseudo_rec[idx]) e->pseudo = pseudo_rec_own(ctx->pseudo_rec[idx]);
            e->pruned_below = ctx->pruned && ctx->pruned[idx];
            style_slot_note(P, idx, (int)(e - g_sc.e), e, true);
            continue;
        }
        if (!e) {
            e = &g_sc.e[g_sc.n];
            e->id = hn->store_id;
            uint32_t h = (e->id * 2654435761u) & (uint32_t)(g_sc.index_cap - 1);
            while (g_sc.index[h]) h = (h + 1) & (uint32_t)(g_sc.index_cap - 1);
            g_sc.index[h] = ++g_sc.n;
        }
        if (!tab) {
            /* Every map this pass copied from the cache is already owned. */
            int total = 0;
            for (int i = 0; i < n; i++) if (ctx->custom_props[i]) total++;
            while (cap < 4 * (total + 1)) cap *= 2;
            tab = (StylePropsRemap *)calloc((size_t)cap, sizeof(*tab));
            if (!tab) { free(after); g_sc.global = 0; return true; }
            for (int i = 0; i < n; i++) {
                const StyleCacheEntry *ce = P->cached[i];
                if (!ce || !ce->props || ctx->custom_props[i] != ce->props) continue;
                const CssCustomProps *m = ce->props;
                uint32_t h = (uint32_t)(((uintptr_t)m >> 4) * 2654435761u) & (uint32_t)(cap - 1);
                while (tab[h].from && tab[h].from != m) h = (h + 1) & (uint32_t)(cap - 1);
                tab[h].from = m;
                tab[h].to = m;
            }
        }
        style_cache_set(e, P, idx, after[idx], tab, cap);
        style_slot_note(P, idx, (int)(e - g_sc.e), e, true);
        written++;
    }
    (void)written;
    free(tab);
    free(after);
    return true;
}

static unsigned g_restyle_reused, g_restyle_recomputed, g_restyle_matched_same;

void css_layout_restyle_stats(unsigned *reused, unsigned *recomputed)
{
    if (reused) *reused = g_restyle_reused;
    if (recomputed) *recomputed = g_restyle_recomputed;
}

unsigned css_layout_restyle_matched_same(void) { return g_restyle_matched_same; }

static void style_cache_store(StylePass *P, uint64_t global, const StyleGenAfter *gen, int gen_total)
{
    LayoutContext *ctx = P->ctx;
    P->ctx->style_pass = ++g_sc.passes;
    int n = ctx->tree.count;
    ctx->sc_order = 0;   /* (the notes are good once the store is done) */
    if (!ctx->sc_entry) {
        ctx->sc_key = (uint64_t *)calloc((size_t)n + 1, sizeof(uint64_t));
        ctx->sc_entry = (int *)calloc((size_t)n + 1, sizeof(int));
        ctx->slot_ver = (uint32_t *)calloc((size_t)n + 1, sizeof(uint32_t));
        if (!ctx->sc_key || !ctx->sc_entry || !ctx->slot_ver) {
            free(ctx->sc_key); free(ctx->sc_entry); free(ctx->slot_ver);
            ctx->sc_key = NULL; ctx->sc_entry = NULL; ctx->slot_ver = NULL;
        }
    }
    bool keep = g_sc.global == global && g_sc.arena.bytes < ((size_t)32 << 20);
    if (!keep) {
        frame_arena_reset(&g_sc.retired);
        g_sc.retired = g_sc.arena;
        memset(&g_sc.arena, 0, sizeof(g_sc.arena));
    }
    int count = 0, fresh = 0;
    for (int idx = 0; idx < n; idx++) {
        if (P->kept && P->kept[idx]) { count++; continue; }   /* (an element with an entry) */
        HtmlNode *hn = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
        if (!hn || hn->type != HTML_NODE_ELEMENT || !hn->store_id) continue;
        count++;
        if (!P->cached[idx]) fresh++;
    }
    /* Same cache, few stale entries: update the entries of recomputed
     * elements in place and append new ones. */
    if (keep && g_sc.n > 0 && g_sc.n + fresh <= 2 * count + 256 &&
        style_cache_update(P, gen, gen_total, fresh)) {
        if (g_sc.global == global) { ctx->sc_order = g_sc.order; ctx->sc_global = global; }
        return;
    }
    int cap = 64;
    while (cap < 4 * (count + g_sc.n + 1)) cap *= 2;
    StylePropsRemap *tab = (StylePropsRemap *)calloc((size_t)cap, sizeof(*tab));
    const char **after = (const char **)calloc((size_t)n + 1, sizeof(char *));
    StyleCacheEntry *ne = (StyleCacheEntry *)malloc(((size_t)count + 1) * sizeof(StyleCacheEntry));
    int icap = 64;
    while (icap < 2 * (count + 1)) icap *= 2;
    int *index = (int *)calloc((size_t)icap, sizeof(int));
    if (!tab || !after || !ne || !index) {
        free(tab); free(after); free(ne); free(index);
        g_sc.global = 0;
        return;
    }
    /* Maps of the previous cache are already owned (their arena lives on). */
    if (keep) {
        for (int i = 0; i < g_sc.n; i++) {
            const CssCustomProps *m = g_sc.e[i].props;
            if (!m) continue;
            uint32_t h = (uint32_t)(((uintptr_t)m >> 4) * 2654435761u) & (uint32_t)(cap - 1);
            while (tab[h].from && tab[h].from != m) h = (h + 1) & (uint32_t)(cap - 1);
            tab[h].from = m;
            tab[h].to = m;
        }
    }
    for (int i = 0; i < gen_total; i++) after[gen[i].idx] = gen[i].text;
    int k = 0;
    for (int idx = 0; idx < n; idx++) {
        HtmlNode *hn = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
        if (!hn || hn->type != HTML_NODE_ELEMENT || !hn->store_id) continue;
        StyleCacheEntry *e = &ne[k];
        e->id = hn->store_id;
        const StyleCacheEntry *old = P->cached[idx];
        if (P->skipped && P->skipped[idx] == 2) {
            style_cache_set_stale(e, P, idx, old);
            style_slot_note(P, idx, k, e, false);
        } else if (P->skipped && P->skipped[idx] == 1) {
            /* Not styled: its entry still holds (and carries over). */
            e->key = old->key;
            e->stale = 0;
            e->pruned_below = old->pruned_below;
            style_cache_set_ctx(e, P, idx, old);
            e->props = style_cache_own(old->props, tab, cap);
            e->match_hash = old->match_hash;
            e->has_after = old->has_after;
            memcpy(e->after, old->after, sizeof(e->after));
            e->pseudo = pseudo_rec_copy(old->pseudo);
            e->style = old->style;
            e->ver = old->ver;
            hn->hidden_hint = e->style.display == CSS_DISPLAY_NONE ? 1 : 2;
            style_slot_note(P, idx, k, e, false);
        } else {
            e->key = P->entry_key ? P->entry_key[idx] : 0;
            style_cache_set_ctx(e, P, idx, old);
            e->props = style_cache_own(ctx->custom_props[idx], tab, cap);
            e->match_hash = P->match_hash ? P->match_hash[idx] : 0;
            e->stale = 0;
            e->pruned_below = ctx->pruned && ctx->pruned[idx];
            e->has_after = after[idx] != NULL;
            if (after[idx]) memcpy(e->after, after[idx], sizeof(e->after));
            else e->after[0] = '\0';
            e->pseudo = pseudo_rec_copy((const PseudoRec *)ctx->pseudo_rec[idx]);
            /* (a slot left as it was holds its entry's style as blockified
             * and given fonts after the last pass: the entry carries over) */
            if (P->kept && P->kept[idx]) {
                e->style = old->style;
                e->ver = old->ver;
            } else {
                e->style = *ctx->boxes[idx].s;
                e->ver = style_cache_next_ver();
            }
            hn->hidden_hint = e->style.display == CSS_DISPLAY_NONE ? 1 : 2;
            style_slot_note(P, idx, k, e, true);
        }
        uint32_t h = (e->id * 2654435761u) & (uint32_t)(icap - 1);
        while (index[h]) h = (h + 1) & (uint32_t)(icap - 1);
        index[h] = k + 1;
        k++;
    }
    free(tab);
    free(after);
    free(g_sc.e);
    free(g_sc.index);
    g_sc.e = ne;
    g_sc.n = k;
    g_sc.order++;   /* (entries moved: the kept trees' notes are stale) */
    ctx->sc_order = g_sc.order;
    ctx->sc_global = global;
    g_sc.cap = count + 1;
    g_sc.index = index;
    g_sc.index_cap = icap;
    g_sc.global = global;
}

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
typedef struct { const CssDeclaration *d; int imp, spec, order; } PseudoDecl;

/* Count a function's comma-separated arguments (between its parentheses). */
static int css_fn_args(const char *open, const char *close)
{
    int n = 0, depth = 0;
    bool any = false;
    for (const char *p = open + 1; p < close; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        else if (*p == ',' && depth == 0) { n++; any = false; continue; }
        if (!isspace((unsigned char)*p)) any = true;
    }
    return any || n ? n + 1 : 0;
}

/* Is a content value valid (CSS 2.1 §12.2): none, normal, or a list of
 * strings, url(), counter(name[, style]), counters(name, string[, style]),
 * attr(name) and the quote keywords? */
/* counter(name[, style]) / counters(name, string[, style]) between the
 * parentheses open..close: a name, then the string (counters), then an
 * identifier. */
static bool css_counter_args_ok(const char *open, const char *close, bool counters)
{
    const char *a = open + 1;
    for (int k = 0; a < close; k++) {
        while (a < close && isspace((unsigned char)*a)) a++;
        bool str = *a == '"' || *a == '\'';
        if (str) {
            char q = *a++;
            while (a < close && *a != q) a += *a == '\\' && a + 1 < close ? 2 : 1;
            if (a < close) a++;
        } else {
            const char *b = a;
            while (a < close && (isalnum((unsigned char)*a) || *a == '-' || *a == '_' || (unsigned char)*a >= 0x80)) a++;
            if (a == b) return false;
        }
        bool want_str = counters && k == 1;
        if (str != want_str) return false;
        while (a < close && isspace((unsigned char)*a)) a++;
        if (a < close && *a != ',') return false;
        a++;
    }
    return true;
}

static bool css_content_value_valid(const char *v)
{
    while (isspace((unsigned char)*v)) v++;
    if (!strncasecmp(v, "none", 4) || !strncasecmp(v, "normal", 6)) return true;
    int items = 0;
    while (*v) {
        while (isspace((unsigned char)*v)) v++;
        if (!*v || *v == '!') break;
        if (*v == '"' || *v == '\'') {
            char q = *v++;
            while (*v && *v != q) v += *v == '\\' && v[1] ? 2 : 1;
            if (*v != q) return false;
            v++;
            items++;
            continue;
        }
        const char *w = v;
        while (*v && (isalnum((unsigned char)*v) || *v == '-' || *v == '_')) v++;
        size_t wl = (size_t)(v - w);
        if (!wl) return false;
        if (*v == '(') {
            const char *open = v;
            int depth = 0;
            for (; *v; v++) {
                if (*v == '(') depth++;
                else if (*v == ')' && --depth == 0) break;
            }
            if (*v != ')') return false;
            int args = css_fn_args(open, v);
            v++;
            if (wl == 7 && !strncasecmp(w, "counter", 7)) {
                if (args < 1 || args > 2 || !css_counter_args_ok(open, v - 1, false)) return false;
            } else if (wl == 8 && !strncasecmp(w, "counters", 8)) {
                if (args < 2 || args > 3 || !css_counter_args_ok(open, v - 1, true)) return false;
            }
            else if (wl == 4 && !strncasecmp(w, "attr", 4)) { if (args != 1) return false; }
            else if (!(wl == 3 && !strncasecmp(w, "url", 3))) return false;
        } else if (!((wl == 10 && !strncasecmp(w, "open-quote", 10)) || (wl == 11 && !strncasecmp(w, "close-quote", 11)) ||
                     (wl == 13 && !strncasecmp(w, "no-open-quote", 13)) || (wl == 14 && !strncasecmp(w, "no-close-quote", 14)))) {
            return false;
        }
        items++;
    }
    return items > 0;
}

static int pseudo_decl_cmp(const void *a, const void *b)
{
    const PseudoDecl *x = (const PseudoDecl *)a, *y = (const PseudoDecl *)b;
    if (x->imp != y->imp) return x->imp - y->imp;
    if (x->spec != y->spec) return x->spec < y->spec ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

/* The element's PseudoRec from its ::before / ::after matches (NULL when
 * neither makes a box), in T's arena. */
static const PseudoRec *style_pseudo_rec(StylePass *P, StyleThread *T, const CssAfterMatch *am, int n)
{
    if (n <= 0) return NULL;
    PseudoDecl stack[64];
    PseudoDecl *all = n * 8 <= 64 ? stack : NULL;
    int cap = all ? 64 : 0, cnt = 0;
    PseudoRec *rec = NULL;
    for (int side = 0; side < 3; side++) {
        const char *content = NULL;
        int bi = -1, bs = -1, bo = -1;
        bool any = false;
        cnt = 0;
        for (int i = 0; i < n; i++) {
            if ((am[i].is_before == 1 ? 0 : am[i].is_before == 2 ? 2 : 1) != side) continue;
            any = true;
            const CssRule *rule = css_styleset_rule(P->set, am[i].rule);
            for (int d = 0; d < rule->declaration_count; d++) {
                const CssDeclaration *decl = &rule->declarations[d];
                if (!decl->property || !decl->value) continue;
                int imp = decl->important ? 1 : 0, order = css_styleset_rule_order(P->set, am[i].rule) + d;
                if (strcasecmp(decl->property, "content") == 0) {
                    if (!css_content_value_valid(decl->value)) continue;   /* (invalid: ignored) */
                    if (imp < bi || (imp == bi && am[i].specificity < bs) ||
                        (imp == bi && am[i].specificity == bs && order <= bo)) continue;
                    bi = imp; bs = am[i].specificity; bo = order;
                    content = decl->value;
                    continue;
                }
                if (cnt == cap) {
                    int nc = cap ? cap * 2 : 64;
                    PseudoDecl *na = (PseudoDecl *)malloc((size_t)nc * sizeof(PseudoDecl));
                    if (!na) break;
                    if (cnt) memcpy(na, all, (size_t)cnt * sizeof(PseudoDecl));
                    if (all != stack) free(all);
                    all = na;
                    cap = nc;
                }
                all[cnt++] = (PseudoDecl){ decl, imp, am[i].specificity, order };
            }
        }
        if (side == 2) content = any ? "" : NULL;   /* (::first-letter: a box when a rule matched) */
        else if (content) {
            const char *v = content;
            while (isspace((unsigned char)*v)) v++;
            if (!strncasecmp(v, "none", 4) || !strncasecmp(v, "normal", 6)) content = NULL;
        }
        if (!content) continue;
        if (!rec) {
            rec = (PseudoRec *)frame_arena_alloc(&T->arena, sizeof(PseudoRec));
            if (!rec) break;
            memset(rec, 0, sizeof(*rec));
        }
        qsort(all, (size_t)cnt, sizeof(PseudoDecl), pseudo_decl_cmp);
        const CssDeclaration **ds = cnt ? (const CssDeclaration **)frame_arena_alloc(&T->arena, (size_t)cnt * sizeof(*ds)) : NULL;
        for (int k = 0; ds && k < cnt; k++) ds[k] = all[k].d;
        rec->side[side].content = content;
        rec->side[side].n = ds ? cnt : 0;
        rec->side[side].decls = ds;
    }
    if (all != stack) free(all);
    return rec;
}

/* A PseudoRec the style cache can keep: its own copy, in its arena (copy:
 * even of one it owns, which may be in the arena it retired). */
static const PseudoRec *pseudo_rec_copy(const PseudoRec *r)
{
    if (!r) return NULL;
    PseudoRec *c = (PseudoRec *)frame_arena_alloc(&g_sc.arena, sizeof(PseudoRec));
    if (!c) return NULL;
    *c = *r;
    c->owned = true;
    for (int side = 0; side < 3; side++) {
        if (!r->side[side].n) continue;
        size_t b = (size_t)r->side[side].n * sizeof(CssDeclaration *);
        const CssDeclaration **ds = (const CssDeclaration **)frame_arena_alloc(&g_sc.arena, b);
        if (!ds) { c->side[side].n = 0; c->side[side].decls = NULL; continue; }
        memcpy(ds, r->side[side].decls, b);
        c->side[side].decls = ds;
    }
    return c;
}

/* The same, keeping one it owns (an in-place update: same arena). */
static const PseudoRec *pseudo_rec_own(const PseudoRec *r)
{
    return r && r->owned ? r : pseudo_rec_copy(r);
}

static bool style_after_content(StylePass *P, StyleThread *T, int dom, char out[256],
                                const PseudoRec **rec)
{
    const CssAfterMatch *am = NULL;
    int n = css_style_match_after(P->set, P->sd, dom, T->scratch, &am);
    *rec = style_pseudo_rec(P, T, am, n);
    bool after_set = false;
    int best_important = -1, best_spec = -1, best_order = -1;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        const CssRule *rule = css_styleset_rule(P->set, am[i].rule);
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
    if (strcasecmp(prop, "font-size") == 0) { box->s->font_size_set = 0; box->s->font_size_ratio = 0.0; }
    else if (strcasecmp(prop, "color") == 0) box->s->color_set = 0;
    else if (strcasecmp(prop, "fill") == 0) box->s->fill_set = 0;
    else if (strcasecmp(prop, "letter-spacing") == 0) { box->s->letter_spacing_set = 0; box->s->letter_spacing = 0.0f; }
    else if (strcasecmp(prop, "word-spacing") == 0) { box->s->word_spacing_set = 0; box->s->word_spacing = 0.0f; }
    else if (strcasecmp(prop, "text-transform") == 0) { box->s->text_transform_set = 0; box->s->text_transform = 0; }
    else if (strcasecmp(prop, "direction") == 0) { box->s->direction_set = 0; box->s->direction = 0; }
    else if (strcasecmp(prop, "text-indent") == 0) { box->s->text_indent_set = 0; box->s->text_indent = box->s->text_indent_pct = 0.0f; }
    else if (strcasecmp(prop, "line-height") == 0) { box->s->line_height_set = 0; box->s->line_height = 0.0; box->s->line_height_ratio = 0.0; box->s->line_height_em = 0.0; }
    else if (strcasecmp(prop, "font-weight") == 0) box->s->font_weight_set = 0;
    else if (strcasecmp(prop, "font-style") == 0) box->s->font_italic_set = 0;
    else if (strcasecmp(prop, "font-family") == 0) box->s->font_family = "";
    else if (strcasecmp(prop, "text-align") == 0) box->s->text_align_set = 0;
    else if (strcasecmp(prop, "white-space") == 0) box->s->white_space_set = 0;
    else if (strcasecmp(prop, "visibility") == 0) box->s->visibility_set = 0;
    else if (strcasecmp(prop, "display") == 0) { /* initial: inline */ box->s->display = CSS_DISPLAY_INLINE; box->s->inline_level = 0; box->s->table_caption = 0; }
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
    /* (the font shorthand gives each longhand the keyword itself) */
    if (strcasecmp(prop, "font") == 0) return false;
    bool inherited_prop = css_is_inherited(prop);
    if (!strncasecmp(prop, "border", 6) && (prop[6] == 0 || prop[6] == '-')) {
        /* Which sides (border_styled bits) and parts (1 width, 2 style, 4
         * color) the property is. */
        int sides = 0x0F, parts = 7;
        const char *q = prop + 6;
        static const struct { const char *name; int bit; } sn[] = {
            { "-left", 1 }, { "-right", 2 }, { "-top", 4 }, { "-bottom", 8 } };
        for (int i = 0; i < 4; i++) {
            size_t l = strlen(sn[i].name);
            if (!strncasecmp(q, sn[i].name, l) && (q[l] == 0 || q[l] == '-')) { sides = sn[i].bit; q += l; break; }
        }
        if (*q == 0) parts = 7;
        else if (!strcasecmp(q, "-width")) parts = 1;
        else if (!strcasecmp(q, "-style")) parts = 2;
        else if (!strcasecmp(q, "-color")) parts = 4;
        else parts = 0;   /* (border-radius, border-collapse, border-spacing, ...) */
        if (parts) {
            double *bw[4] = { &box->s->border_left, &box->s->border_right, &box->s->border_top, &box->s->border_bottom };
            const double *pw[4] = { parent ? &parent->s->border_left : NULL, parent ? &parent->s->border_right : NULL,
                                    parent ? &parent->s->border_top : NULL, parent ? &parent->s->border_bottom : NULL };
            for (int k = 0; k < 4; k++) {
                unsigned char bit = (unsigned char)(1 << k);
                if (!(sides & bit)) continue;
                bool from_parent = inherit && parent;
                if (parts & 1) {
                    *bw[k] = from_parent ? *pw[k] : 3.0;
                    if (from_parent) box->s->border_width_set |= bit;
                    else box->s->border_width_set &= (unsigned char)~bit;
                }
                if (parts & 2) border_set_style(box, bit, from_parent ? parent->s->border_style[k] : CSS_BORDER_NONE);
                if (parts & 4) {
                    box->s->border_rgba[k] = from_parent ? parent->s->border_rgba[k] : 0;
                    if (from_parent && (parent->s->border_color_set & bit)) box->s->border_color_set |= bit;
                    else box->s->border_color_set &= (unsigned char)~bit;
                }
            }
            return true;
        }
    }
    if (!strcasecmp(prop, "counter-reset") || !strcasecmp(prop, "counter-increment") ||
        !strcasecmp(prop, "counter-set")) {
        const char **dst = prop[8] == 'r' ? &box->s->counter_reset : prop[8] == 'i' ? &box->s->counter_increment
                                                                                   : &box->s->counter_set;
        const char *src = !parent ? NULL : prop[8] == 'r' ? parent->s->counter_reset
                                  : prop[8] == 'i' ? parent->s->counter_increment : parent->s->counter_set;
        *dst = inherit ? src : NULL;
        return true;
    }
    if (!strcasecmp(prop, "quotes")) {
        /* (inherited: initial is the UA's) */
        box->s->quotes = NULL;
        box->s->quotes_set = initial;
        return true;
    }

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
            box->s->height_set = parent->s->height_set; box->s->css_height = parent->s->css_height;
            box->s->height_percent = parent->s->height_percent; box->s->height_pct_px = parent->s->height_pct_px;
            return true;
        }
        if (strcasecmp(prop, "width") == 0) {
            box->s->width_set = parent->s->width_set; box->s->css_width = parent->s->css_width;
            box->s->width_percent = parent->s->width_percent; box->s->width_pct_px = parent->s->width_pct_px;
            box->s->width_keyword = parent->s->width_keyword;
            return true;
        }
        static const struct { const char *name; int which; } mm[] = {
            { "min-width", 0 }, { "max-width", 1 }, { "min-height", 2 }, { "max-height", 3 }
        };
        for (size_t i = 0; i < sizeof(mm) / sizeof(mm[0]); i++) {
            if (strcasecmp(prop, mm[i].name) != 0) continue;
            double *dst[4] = { &box->s->min_width, &box->s->max_width, &box->s->min_height, &box->s->max_height };
            const double *src[4] = { &parent->s->min_width, &parent->s->max_width, &parent->s->min_height, &parent->s->max_height };
            *dst[mm[i].which] = *src[mm[i].which];
            unsigned char bit = (unsigned char)(1u << mm[i].which);
            box->s->minmax_pct_bits = (unsigned char)((box->s->minmax_pct_bits & ~bit) | (parent->s->minmax_pct_bits & bit));
            box->s->minmax_pct[mm[i].which] = parent->s->minmax_pct[mm[i].which];
            box->s->minmax_base[mm[i].which] = parent->s->minmax_base[mm[i].which];
            return true;
        }
        if (strcasecmp(prop, "flex-direction") == 0) { box->s->flex_direction = parent->s->flex_direction; return true; }
        if (strcasecmp(prop, "float") == 0) { box->s->float_side = parent->s->float_side; return true; }
        if (strcasecmp(prop, "display") == 0) {
            box->s->display = parent->s->display;
            box->s->inline_level = parent->s->inline_level;
            box->s->table_role = parent->s->table_role;
            box->s->table_caption = parent->s->table_caption;
            return true;
        }
        if (strcasecmp(prop, "box-sizing") == 0) { box->s->box_sizing = parent->s->box_sizing; return true; }
        if (strcasecmp(prop, "overflow") == 0) { box->s->overflow = parent->s->overflow; return true; }
        /* A margin or padding side: the parent's computed value (a
         * percentage stays one, of this box's containing block). */
        static const char *const sides[4] = { "top", "right", "bottom", "left" };
        bool is_margin = !strncasecmp(prop, "margin-", 7), is_padding = !strncasecmp(prop, "padding-", 8);
        for (int sd = 0; (is_margin || is_padding) && sd < 4; sd++) {
            if (strcasecmp(prop + (is_margin ? 7 : 8), sides[sd]) != 0) continue;
            double *dst = is_margin ? &box->s->margin_top : &box->s->padding_top;
            const double *src = is_margin ? &parent->s->margin_top : &parent->s->padding_top;
            dst[sd] = src[sd];
            unsigned char pb = (unsigned char)(1u << (sd + (is_margin ? 0 : 4)));
            box->s->em_deferred &= (unsigned char)~pb;
            box->s->pct_sides = (unsigned char)((box->s->pct_sides & ~pb) | (parent->s->pct_sides & pb));
            if (is_margin) {
                box->s->margin_pct[sd] = parent->s->margin_pct[sd];
                box->s->margin_base[sd] = parent->s->margin_base[sd];
                unsigned char ab = (unsigned char)(1u << sd);
                box->s->margin_auto = (unsigned char)((box->s->margin_auto & ~ab) | (parent->s->margin_auto & ab));
            } else {
                box->s->padding_pct[sd] = parent->s->padding_pct[sd];
                box->s->padding_base[sd] = parent->s->padding_base[sd];
            }
            return true;
        }
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
        { "z-index", "auto" }, { "unicode-bidi", "normal" }, { "order", "0" }, { "gap", "0" }, { "row-gap", "0" }, { "column-gap", "0" },
    };
    if (strcasecmp(prop, "display") == 0) {
        box->s->display = CSS_DISPLAY_INLINE;
        box->s->inline_level = 1;
        return true;
    }
    for (size_t i = 0; i < sizeof(initial_values) / sizeof(initial_values[0]); i++) {
        if (strcasecmp(prop, initial_values[i].prop) != 0) continue;
        CssDeclaration d = { 0 };
        d.property = (char *)initial_values[i].prop;
        d.value = (char *)initial_values[i].value;
        d.invalid = 1;   /* (known valid) */
        layout_apply_declaration(box, &d, viewport_width, viewport_width, base_url);
        return true;
    }
    /* Not modelled: ignore the declaration rather than parse the keyword. */
    return true;
}

/* After computing an element's style: does it differ from the cached one
 * (its children then recompute)?  CYBER_STYLE_VERIFY=1 also reports
 * elements whose reuse would have been wrong. */
static void style_note_result(StylePass *P, int idx, const LayoutBox *box, const CssCustomProps *props,
                              const StyleCacheEntry *ce, bool would_reuse, const HtmlNode *node)
{
    if (!P->inc) return;
    bool same = ce && !ce->stale && layout_style_same(box, &ce->style) && style_props_equal(props, ce->props);
    P->out_changed[idx] = !same;
    atomic_fetch_add(&P->recomputed, 1);
    if (P->verify && would_reuse) atomic_fetch_add(&P->verify_reusable, 1);
    if (P->verify && would_reuse && !same) {
        uint32_t k = atomic_fetch_add(&P->verify_mismatches, 1);
        if (k < 20) {
            size_t off = 0;
            const unsigned char *a = (const unsigned char *)box->s, *b = (const unsigned char *)&ce->style;
            while (off < sizeof(*box->s) && a[off] == b[off]) off++;
            fprintf(stderr, "[style-verify] reuse would differ: <%s id=%s class=%.60s> first byte %zu%s\n",
                    node && node->tag_name ? node->tag_name : "?",
                    node ? (layout_node_attribute((HtmlNode *)node, "id") ? layout_node_attribute((HtmlNode *)node, "id") : "") : "",
                    node ? (layout_node_attribute((HtmlNode *)node, "class") ? layout_node_attribute((HtmlNode *)node, "class") : "") : "",
                    off, off >= sizeof(*box->s) ? " (geometry or custom properties)" : " of the style");
        }
    }
}

static void style_finish_box(StylePass *P, LayoutBox *box, const LayoutBox *parent, bool element);

/* The first strong character's direction in the text under DOM node dom
 * (1 rtl, 0 ltr, -1 none), leaving out bdi, script, style and textarea
 * elements and those with a dir attribute (HTML's dir=auto). */
static int style_text_direction(LayoutContext *ctx, int dom)
{
    PreOrderCompactionArray *da = &ctx->doc->array;
    for (int c = po_array_first_child(da, dom); c >= 0; c = po_array_next_sibling(da, c)) {
        HtmlNode *n = layout_node_dom(ctx, c);
        if (!n) continue;
        if (n->type == HTML_NODE_TEXT && n->text_content) {
            const unsigned char *p = (const unsigned char *)n->text_content, *e = p + n->text_len;
            while (p < e) {
                uint32_t cp = *p++;
                int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
                if (more) cp &= 0x3F >> more;
                for (; more && p < e; more--) cp = (cp << 6) | (*p++ & 0x3F);
                BidiClass k = bidi_class(cp);
                if (k == BIDI_L) return 0;
                if (k == BIDI_R || k == BIDI_AL) return 1;
            }
        } else if (n->type == HTML_NODE_ELEMENT) {
            const char *t = n->tag_name ? n->tag_name : "";
            if (!strcmp(t, "bdi") || !strcmp(t, "script") || !strcmp(t, "style") || !strcmp(t, "textarea") ||
                layout_node_attribute(n, "dir"))
                continue;
            int r = style_text_direction(ctx, c);
            if (r >= 0) return r;
        }
    }
    return -1;
}

/* An element with dir=auto, or a bdi without dir: its direction from its
 * text (1 rtl, 0 ltr, also when it has no strong character); else -1. */
static int style_auto_direction(LayoutContext *ctx, int dom, HtmlNode *node)
{
    const char *d = layout_node_attribute(node, "dir");
    if (d ? strcasecmp(d, "auto") != 0 : !(node->tag_name && !strcmp(node->tag_name, "bdi"))) return -1;
    return style_text_direction(ctx, dom) == 1 ? 1 : 0;
}

static void hint_apply(LayoutContext *ctx, LayoutBox *box, const char *prop, const char *value)
{
    CssDeclaration d = { (char *)prop, (char *)value, false };
    layout_apply_declaration(box, &d, ctx->viewport_width, ctx->viewport_width, ctx->base_url);
}

/* A dimension attribute (width="85%", height="10") as a CSS length. */
static bool hint_dimension(const char *v, char *out, size_t cap)
{
    if (!v) return false;
    while (isspace((unsigned char)*v)) v++;
    char *end = NULL;
    double n = strtod(v, &end);
    if (end == v || n < 0.0) return false;
    snprintf(out, cap, "%g%s", n, *end == '%' ? "%" : "px");
    return true;
}

/* HTML presentational hints (HTML §15.3): attributes that map to CSS
 * below every author rule (bgcolor, width/height, align, valign, nowrap,
 * cellpadding / cellspacing).  The style cache keys cover the attributes;
 * a table's cellpadding is read from the cell's nearest table. */
static void style_presentational_hints(LayoutContext *ctx, LayoutBox *box, int idx, HtmlNode *node)
{
    const char *tag = node->tag_name ? node->tag_name : "";
    unsigned char role = layout_default_table_role(tag);
    bool body = strcasecmp(tag, "body") == 0;
    /* (embedded content: width and height map to the CSS properties) */
    bool embedded = !strcasecmp(tag, "img") || !strcasecmp(tag, "iframe") || !strcasecmp(tag, "video") ||
                    !strcasecmp(tag, "embed") || !strcasecmp(tag, "object");
    if (!role && !body && !embedded && strcasecmp(tag, "font") != 0 && strcasecmp(tag, "div") != 0 &&
        strcasecmp(tag, "p") != 0 && strncasecmp(tag, "h", 1) != 0 && strcasecmp(tag, "hr") != 0)
        return;
    char buf[64];
    const char *v;
    if (embedded) {
        if (hint_dimension(layout_node_attribute(node, "width"), buf, sizeof(buf))) hint_apply(ctx, box, "width", buf);
        if (hint_dimension(layout_node_attribute(node, "height"), buf, sizeof(buf))) hint_apply(ctx, box, "height", buf);
        return;
    }
    if ((role || body) && (v = layout_node_attribute(node, "bgcolor")) && *v)
        hint_apply(ctx, box, "background-color", v);
    if (strcasecmp(tag, "font") == 0) {
        if ((v = layout_node_attribute(node, "color")) && *v) hint_apply(ctx, box, "color", v);
        if ((v = layout_node_attribute(node, "face")) && *v) hint_apply(ctx, box, "font-family", v);
        return;
    }
    if (role == 1 || role == 4 || role == 6 || strcasecmp(tag, "hr") == 0) {
        if (hint_dimension(layout_node_attribute(node, "width"), buf, sizeof(buf))) hint_apply(ctx, box, "width", buf);
    }
    if (role == 1 || role == 3 || role == 4) {
        if (hint_dimension(layout_node_attribute(node, "height"), buf, sizeof(buf))) hint_apply(ctx, box, "height", buf);
    }
    v = layout_node_attribute(node, "align");
    if (role == 1) {
        /* Quirks-mode tables don't inherit text-align (the pages that put
         * them in <center> are legacy ones). */
        for (int p = ctx->tree.nodes[idx].parent_idx, hops = 0; p >= 0 && hops < 8;
             p = ctx->tree.nodes[p].parent_idx, hops++) {
            HtmlNode *pn = layout_node_dom(ctx, ctx->tree.nodes[p].dom_node_idx);
            if (pn && pn->type == HTML_NODE_ELEMENT && pn->tag_name && strcasecmp(pn->tag_name, "center") == 0) {
                hint_apply(ctx, box, "text-align", "left");
                break;
            }
        }
        if (v && strcasecmp(v, "center") == 0) {
            hint_apply(ctx, box, "margin-left", "auto");
            hint_apply(ctx, box, "margin-right", "auto");
        } else if (v && (strcasecmp(v, "left") == 0 || strcasecmp(v, "right") == 0)) {
            hint_apply(ctx, box, "float", v);
        }
        if (hint_dimension(layout_node_attribute(node, "cellspacing"), buf, sizeof(buf)))
            hint_apply(ctx, box, "border-spacing", buf);
        if (hint_dimension(layout_node_attribute(node, "border"), buf, sizeof(buf)) && strcmp(buf, "0px") != 0) {
            hint_apply(ctx, box, "border-style", "outset");
            hint_apply(ctx, box, "border-width", buf);
            hint_apply(ctx, box, "border-color", "gray");
        }
        return;
    }
    if (v && *v && (role >= 2 || strcasecmp(tag, "div") == 0 || strcasecmp(tag, "p") == 0 ||
                    (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && !tag[2]))) {
        if (strcasecmp(v, "center") == 0 || strcasecmp(v, "middle") == 0) hint_apply(ctx, box, "text-align", "center");
        else if (strcasecmp(v, "left") == 0 || strcasecmp(v, "right") == 0 || strcasecmp(v, "justify") == 0)
            hint_apply(ctx, box, "text-align", v);
    } else if (role == 4 && tag[1] == 'h') {
        hint_apply(ctx, box, "text-align", "center");
    }
    if (role >= 2 && role <= 4 && (v = layout_node_attribute(node, "valign")) && *v) {
        if (strcasecmp(v, "top") == 0 || strcasecmp(v, "bottom") == 0 || strcasecmp(v, "middle") == 0 ||
            strcasecmp(v, "baseline") == 0)
            hint_apply(ctx, box, "vertical-align", v);
    }
    if (role != 4) return;
    if (layout_node_attribute(node, "nowrap")) hint_apply(ctx, box, "white-space", "nowrap");
    /* Cell padding: the table's cellpadding, else the UA's 1px. */
    const char *pad = "1px";
    for (int p = ctx->tree.nodes[idx].parent_idx, hops = 0; p >= 0 && hops < 4;
         p = ctx->tree.nodes[p].parent_idx, hops++) {
        HtmlNode *pn = layout_node_dom(ctx, ctx->tree.nodes[p].dom_node_idx);
        if (!pn || pn->type != HTML_NODE_ELEMENT || !pn->tag_name) continue;
        if (strcasecmp(pn->tag_name, "table") != 0) continue;
        if (hint_dimension(layout_node_attribute(pn, "cellpadding"), buf, sizeof(buf))) pad = buf;
        break;
    }
    hint_apply(ctx, box, "padding", pad);
}

static void style_node(StylePass *P, StyleThread *T, int pos, int idx)
{
    LayoutContext *ctx = P->ctx;
    LayoutBox *box = layout_box(ctx, idx);
    int p = ctx->tree.nodes[idx].parent_idx;
    LayoutBox *parent = (p >= 0) ? layout_box(ctx, p) : NULL;
    int dom = ctx->tree.nodes[idx].dom_node_idx;
    HtmlNode *node = layout_node_dom(ctx, dom);
    box->s = ctx->slot[idx];   /* (a kept tree's text box may hold a shared style) */

    /* Custom properties inherit; the map is shared until this node sets one. */
    const CssCustomProps *pv = (p >= 0) ? ctx->custom_props[p] : NULL;
    ctx->custom_props[idx] = pv;

    P->cls[idx] = idx;
    bool share_ok = false;
    bool bits_ready = false;
    int parent_cls = (p >= 0) ? P->cls[p] : -1;
    const StyleCacheEntry *ce = P->inc ? P->cached[idx] : NULL;
    bool can_reuse = ce && !ce->stale && P->key_ok[idx] && !P->force[idx] && (p < 0 || !P->out_changed[p]);
    bool mpc_would = false;   /* CYBER_STYLE_VERIFY: the matched-properties cache would take ce */
    if (P->inc && !can_reuse && g_css_log > 1 && node && node->type == HTML_NODE_ELEMENT) {
        int why = !ce ? 0 : !P->key_ok[idx] ? 1 : P->force[idx] == 2 ? 2 : P->force[idx] == 1 ? 3 : 4;
        atomic_fetch_add(&P->why[why], 1);
    }
    if (node && node->type == HTML_NODE_ELEMENT) {
        css_style_bloom_enter(T->scratch, P->sd, po_array_parent(&ctx->doc->array, dom));

        /* Incremental restyle: the cached box is this element's style. */
        if (can_reuse && !P->verify) {
            if (P->retain && ctx->slot_ver[idx] == ce->ver) { P->kept[idx] = 1; atomic_fetch_add(&P->kept_n, 1); }
            else layout_style_set(box, &ce->style);
            ctx->custom_props[idx] = ce->props;
            P->match_hash[idx] = ce->match_hash;
            ctx->pseudo_rec[idx] = ce->pseudo;
            if (!parent && box->s->font_size > 0.0) g_unit_rem = box->s->font_size;
            if (ce->has_after) {
                if (T->gen_n == T->gen_cap) {
                    T->gen_cap = T->gen_cap ? T->gen_cap * 2 : 16;
                    T->gen = (StyleGenAfter *)realloc(T->gen, (size_t)T->gen_cap * sizeof(StyleGenAfter));
                }
                T->gen[T->gen_n].pos = pos;
                T->gen[T->gen_n].idx = idx;
                memcpy(T->gen[T->gen_n].text, ce->after, sizeof(ce->after));
                T->gen_n++;
            }
            P->out_changed[idx] = 0;
            atomic_fetch_add(&P->reused, 1);
            css_style_bloom_push(T->scratch, P->sd, dom);
            return;
        }

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
                /* (a slot left as it was is past the last pass's blockification: its entry) */
                layout_style_set(box, P->kept && P->kept[e->idx] ? &P->cached[e->idx]->style : layout_box(ctx, e->idx)->s);
                ctx->custom_props[idx] = ctx->custom_props[e->idx];
                ctx->pseudo_rec[idx] = e->pseudo;
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
                style_note_result(P, idx, box, ctx->custom_props[idx], ce, can_reuse, node);
                return;
            }
            T->share_misses++;
        }
    }
    /* A text node: the style of its parent's text children, computed once
     * per parent (it is a function of the parent's final style alone). */
    if (!(node && node->type == HTML_NODE_ELEMENT) && parent && P->text_styles) {
        LayoutStyle *ts = atomic_load_explicit(&P->text_styles[p], memory_order_acquire);
        if (!ts || P->text_style_check) {
            layout_box_init(ctx, idx);
            style_finish_box(P, box, parent, false);
            if (ts) {
                if (memcmp(ts, box->s, sizeof(LayoutStyle)) != 0)
                    atomic_fetch_add(&P->text_style_mismatches, 1);
            } else {
                LayoutStyle *mine = (LayoutStyle *)frame_arena_alloc(&T->arena, sizeof(LayoutStyle));
                if (mine) {
                    LayoutStyle *expected = NULL;
                    *mine = *box->s;
                    if (atomic_compare_exchange_strong_explicit(&P->text_styles[p], &expected, mine,
                                                                memory_order_acq_rel, memory_order_acquire))
                        ts = mine;
                    else
                        ts = expected;
                }
            }
            if (ts) box->s = ts;
        } else {
            box->s = ts;   /* the shared style */
        }
        css_style_bloom_push(T->scratch, P->sd, dom);
        if (P->inc) P->out_changed[idx] = 1;
        return;
    }
    /* Computed from the defaults (a recycled box holds the last layout's). */
    layout_box_init(ctx, idx);
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

        char after_content[256];
        double pseudo_aspect = 0.0;
        const PseudoRec *prec = NULL;
        bool after_set = style_after_content(P, T, dom, after_content, &prec);
        ctx->pseudo_rec[idx] = prec;

        /* (dir=auto: the direction its text gives, part of the cache key) */
        int dir_auto = style_auto_direction(ctx, dom, node);

        /* Matched-properties cache: the cascade below is a function of the
         * declarations matched, the inline styles, the ::after content, the
         * aspect ratios found above, the root font size and the parent's
         * computed style.  When all are as they were for the cached box (the
         * parent's style and its own key unchanged), that box is the result.  Invalidation
         * recomputes many elements that match exactly as before. */
        if (P->match_hash) {
            uint64_t h = 1469598103934665603ull;
            h = style_mix(h, &count, sizeof(count));
            for (int d = 0; d < count; d++) {
                h = style_mix(h, &applied[d].decl, sizeof(applied[d].decl));
                h = style_mix(h, &applied[d].rule, sizeof(applied[d].rule));
            }
            for (int b = 0; b < ninl; b++)
                for (int i = 0; i < inl[b].count; i++) {
                    const CssDeclaration *dl = &inl[b].decls[i];
                    if (dl->property) h = style_mix(h, dl->property, strlen(dl->property) + 1);
                    if (dl->value) h = style_mix(h, dl->value, strlen(dl->value) + 1);
                    h = style_mix(h, &dl->important, sizeof(dl->important));
                }
            h = style_mix(h, &ninl, sizeof(ninl));
            h = style_mix(h, &after_set, sizeof(after_set));
            if (after_set) h = style_mix(h, after_content, strnlen(after_content, sizeof(after_content)) + 1);
            h = style_mix(h, &pseudo_aspect, sizeof(pseudo_aspect));
            h = style_mix(h, &dir_auto, sizeof(dir_auto));
            h = style_mix(h, &box->s->aspect_ratio, sizeof(box->s->aspect_ratio));
            h = style_mix(h, &g_unit_rem, sizeof(g_unit_rem));
            if (h == 0) h = 1;
            P->match_hash[idx] = h;
            /* The key covers the attributes, which presentational hints read.
             * (Below a parent whose entry was stored with its children left
             * out, theirs are older than its: "unchanged" says nothing.) */
            bool take = ce && !ce->stale && ce->match_hash == h && P->key_ok[idx] &&
                        (p < 0 || (!P->out_changed[p] && !(P->cached[p] && P->cached[p]->pruned_below)));
            if (take && P->verify) mpc_would = true;   /* checked against the full result */
            if (take && !P->verify) {
                for (int b = 0; b < ninl; b++) css_declarations_free(inl[b].decls, inl[b].count);
                layout_style_set(box, &ce->style);
                ctx->custom_props[idx] = ce->props;
                if (!parent && box->s->font_size > 0.0) g_unit_rem = box->s->font_size;
                if (after_set && after_content[0] != '\0') {
                    if (T->gen_n == T->gen_cap) {
                        T->gen_cap = T->gen_cap ? T->gen_cap * 2 : 16;
                        T->gen = (StyleGenAfter *)realloc(T->gen, (size_t)T->gen_cap * sizeof(StyleGenAfter));
                    }
                    T->gen[T->gen_n].pos = pos;
                    T->gen[T->gen_n].idx = idx;
                    memcpy(T->gen[T->gen_n].text, after_content, sizeof(after_content));
                    T->gen_n++;
                }
                P->out_changed[idx] = 0;
                atomic_fetch_add(&P->recomputed, 1);
                atomic_fetch_add(&P->matched_same, 1);
                css_style_bloom_push(T->scratch, P->sd, dom);
                return;
            }
        }

        style_presentational_hints(ctx, box, idx, node);
        if (dir_auto >= 0) hint_apply(ctx, box, "direction", dir_auto ? "rtl" : "ltr");
        /* font-size (and the font shorthand) first, the element's final
         * size worked out, so em lengths in any declaration resolve against
         * it (as the computed font-size is what they mean); then all, in
         * order. */
        for (int phase = 0; phase < 2; phase++) {
        for (int d = 0; d < count; d++) {
            const char *prop = applied[d].decl->property;
            if (prop && prop[0] == '-' && prop[1] == '-') continue;
            if (phase == 0 && !style_is_font_size_prop(prop)) continue;
            t_var_failed = false;
            char *resolved = css_var_resolve(applied[d].decl->value, props);
            CssDeclaration decl = *applied[d].decl;
            decl.value = resolved ? resolved : applied[d].decl->value;
            layout_decl_copy_validity(&decl, applied[d].decl, resolved != NULL);
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
                if (phase == 0 && !style_is_font_size_prop(prop)) continue;
                t_var_failed = false;
                char *resolved = css_var_resolve(inl[b].decls[i].value, props);
                CssDeclaration decl = inl[b].decls[i];
                decl.value = resolved ? resolved : inl[b].decls[i].value;
                if (resolved) decl.invalid = 0;
                if (t_var_failed) layout_unset_property(box, decl.property);
                else if (!layout_apply_wide_keyword(box, parent, decl.property, decl.value,
                                                    ctx->viewport_width, ctx->base_url))
                    layout_apply_declaration(box, &decl, ctx->viewport_width, ctx->viewport_width,
                                              ctx->base_url);
                free(resolved);
            }
        }
        if (phase == 0) {
            if (box->s->font_size_ratio > 0.0) {
                double base = parent ? parent->s->font_size : 16.0;
                box->s->font_size = (base > 0.0 ? base : 16.0) * box->s->font_size_ratio;
            } else if (!box->s->font_size_set && parent) {
                box->s->font_size = parent->s->font_size;
            }
        }
        }
        for (int b = 0; b < ninl; b++) css_declarations_free(inl[b].decls, inl[b].count);
        if (pseudo_aspect > box->s->aspect_ratio) box->s->aspect_ratio = pseudo_aspect;

        /* UA rule: metadata elements are always display:none. */
        if (layout_default_display(node->tag_name) == CSS_DISPLAY_NONE) {
            box->s->display = CSS_DISPLAY_NONE;
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
    style_finish_box(P, box, parent, node && node->type == HTML_NODE_ELEMENT);

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
        e->pseudo = ctx->pseudo_rec[idx];
        if (share_has_after) memcpy(e->after, share_after, sizeof(e->after));
    }
    if (node && node->type == HTML_NODE_ELEMENT)
        style_note_result(P, idx, box, ctx->custom_props[idx], ce, can_reuse || mpc_would, node);
    else if (P->inc)
        P->out_changed[idx] = 1;
}

/* What the cascade leaves to the parent's final style: inheritance,
 * parent-relative sizes, em lengths against the final font size, used
 * border widths and blockification. */
static void style_finish_box_em(bool em_defer, LayoutBox *box, const LayoutBox *parent, bool element);
static void style_finish_box(StylePass *P, LayoutBox *box, const LayoutBox *parent, bool element)
{
    style_finish_box_em(P->em_defer, box, parent, element);
}

/* font-size or the font shorthand (what sets the font size). */
static bool style_is_font_size_prop(const char *prop)
{
    return prop && (prop[0] == 'f' || prop[0] == 'F') &&
           (!strcasecmp(prop, "font") || !strcasecmp(prop, "font-size"));
}

static void style_finish_box_em(bool em_defer, LayoutBox *box, const LayoutBox *parent, bool element)
{
    /* Inheritance and parent-relative values: the parent is final. */
    if (!box->s->font_size_set && box->s->font_size_ratio <= 0.0 && parent) {
        box->s->font_size = parent->s->font_size;
    }
    if (box->s->font_size_ratio > 0.0) {
        double base = parent ? parent->s->font_size : 16.0;
        if (base <= 0.0) base = 16.0;
        box->s->font_size = base * box->s->font_size_ratio;
    }
    if (!parent && box->s->font_size > 0.0) g_unit_rem = box->s->font_size;   /* root: rem base */
    if (!box->s->fill_set && parent) {
        box->s->fill_kind = parent->s->fill_kind;
        box->s->fill_r = parent->s->fill_r;
        box->s->fill_g = parent->s->fill_g;
        box->s->fill_b = parent->s->fill_b;
        box->s->fill_a = parent->s->fill_a;
    }
    if (!box->s->color_set && parent) {
        box->s->color_r = parent->s->color_r;
        box->s->color_g = parent->s->color_g;
        box->s->color_b = parent->s->color_b;
        box->s->color_a = parent->s->color_a;
    }
    if (!box->s->text_align_set && parent) {
        box->s->text_align = parent->s->text_align;
        box->s->text_align_how = parent->s->text_align_how;
    }
    if (!box->s->direction_set && parent) box->s->direction = parent->s->direction;
    if (box->s->text_align_how != 1)
        box->s->text_align = (box->s->text_align_how == 2) != (box->s->direction == 1) ? CSS_TEXT_ALIGN_RIGHT
                                                                                       : CSS_TEXT_ALIGN_LEFT;
    if (!box->s->visibility_set && parent) box->s->visibility = parent->s->visibility;
    /* Typography inheritance: font-family, weight, style, line-height and
     * list-style-type are inherited CSS properties.  text-decoration is
     * technically not inherited, but it visually propagates to inline
     * descendants; inheriting-when-unset approximates that well. */
    if (parent) {
        if (!box->s->font_family[0]) box->s->font_family = parent->s->font_family;
        if (!box->s->font_weight_set) box->s->font_weight = parent->s->font_weight;
        if (!box->s->font_italic_set) box->s->font_italic = parent->s->font_italic;
        if (!box->s->text_decoration_set) box->s->text_decoration = parent->s->text_decoration;
        if (!box->s->line_height_set) {
            box->s->line_height = parent->s->line_height;
            box->s->line_height_ratio = parent->s->line_height_ratio;
        }
        if (!box->s->list_style_type_set) box->s->list_style_type = parent->s->list_style_type;
        if (!box->s->quotes_set) box->s->quotes = parent->s->quotes;
        if (!box->s->letter_spacing_set) box->s->letter_spacing = parent->s->letter_spacing;
        if (!box->s->word_spacing_set) box->s->word_spacing = parent->s->word_spacing;
        if (!box->s->text_transform_set) box->s->text_transform = parent->s->text_transform;
        if (!box->s->border_collapse_set) box->s->border_collapse = parent->s->border_collapse;
        if (!box->s->text_indent_set) {
            box->s->text_indent = parent->s->text_indent;
            box->s->text_indent_pct = parent->s->text_indent_pct;
        }
        if (!box->s->white_space_set) box->s->white_space = parent->s->white_space;
    }
    /* Unitless line-height inherits as a ratio: recompute against the
     * element's own (now final) font-size. */
    if (box->s->line_height_ratio > 0.0) {
        double fs = box->s->font_size > 0.0 ? box->s->font_size : 16.0;
        box->s->line_height = box->s->line_height_ratio * fs;
    }
    /* Table-internal boxes have no margins; rows, row groups and columns no
     * padding, nor borders but in the collapsing border model (CSS 2.1
     * sections 8.3, 8.4, 17.6.1). */
    if (box->s->display == CSS_DISPLAY_OTHER && box->s->table_role >= 2 && box->s->table_role <= 6 &&
        box->s->table_role != 5) {
        box->s->margin_top = box->s->margin_right = box->s->margin_bottom = box->s->margin_left = 0.0;
        box->s->margin_auto = 0;
        box->s->em_deferred &= 0xF0;
        box->s->pct_sides &= 0xF0;
        for (int sd = 0; sd < 4; sd++) box->s->margin_pct[sd] = box->s->margin_base[sd] = 0.0;
        if (box->s->table_role != 4) {
            box->s->padding_top = box->s->padding_right = box->s->padding_bottom = box->s->padding_left = 0.0;
            box->s->em_deferred &= 0x0F;
            box->s->pct_sides &= 0x0F;
            for (int sd = 0; sd < 4; sd++) box->s->padding_pct[sd] = box->s->padding_base[sd] = 0.0;
            if (!box->s->border_collapse)
                box->s->border_top = box->s->border_right = box->s->border_bottom = box->s->border_left = 0.0;
        }
    }
    /* overflow applies to block containers: not to table rows, row groups
     * or columns (CSS 2.1 section 11.1.1). */
    if (box->s->overflow && box->s->display == CSS_DISPLAY_OTHER &&
        (box->s->table_role == 2 || box->s->table_role == 3 || box->s->table_role == 6))
        box->s->overflow = 0;
    /* An em / percentage line-height: of the element's final font-size. */
    if (box->s->line_height_set && box->s->line_height_em > 0.0) {
        box->s->line_height = box->s->line_height_em * box->s->font_size;
        if (box->s->line_height <= 0.0) box->s->line_height = 1e-6;
    }
    /* em-unit margins/paddings resolve against the element's own FINAL
     * font-size (CSS 2.1 §8.3/§8.4). */
    if (em_defer && box->s->em_deferred) {
        double fs = box->s->font_size > 0.0 ? box->s->font_size : 16.0;
        if (box->s->em_deferred & 0x01) box->s->margin_top     = box->s->margin_em[0] * fs;
        if (box->s->em_deferred & 0x02) box->s->margin_right   = box->s->margin_em[1] * fs;
        if (box->s->em_deferred & 0x04) box->s->margin_bottom  = box->s->margin_em[2] * fs;
        if (box->s->em_deferred & 0x08) box->s->margin_left    = box->s->margin_em[3] * fs;
        if (box->s->em_deferred & 0x10) box->s->padding_top    = box->s->padding_em[0] * fs;
        if (box->s->em_deferred & 0x20) box->s->padding_right  = box->s->padding_em[1] * fs;
        if (box->s->em_deferred & 0x40) box->s->padding_bottom = box->s->padding_em[2] * fs;
        if (box->s->em_deferred & 0x80) box->s->padding_left   = box->s->padding_em[3] * fs;
    }

    /* A vertical legacy -webkit-box (the line-clamp idiom) is a block. */
    if (box->s->webkit_box && box->s->display == CSS_DISPLAY_FLEX &&
        (box->s->box_orient_vertical || box->s->line_clamp)) {
        box->s->display = box->s->webkit_box == 2 ? CSS_DISPLAY_INLINE_BLOCK : CSS_DISPLAY_BLOCK;
        box->s->inline_level = 0;
    }

    /* Used border widths: unstyled sides have none; styled sides without
     * an explicit width are medium. */
    if (element) {
        double *bw[4] = { &box->s->border_left, &box->s->border_right, &box->s->border_top, &box->s->border_bottom };
        for (int k = 0; k < 4; k++) {
            unsigned char bit = (unsigned char)(1u << k);
            if (!(box->s->border_styled & bit)) *bw[k] = 0.0;
            else if (!(box->s->border_width_set & bit)) *bw[k] = 3.0;
            if (box->s->border_coll) continue;   /* (a style finished again: its copy stands) */
            box->s->border_spec_w[k] = (float)*bw[k];
            box->s->border_spec_style[k] = box->s->border_style[k];
            box->s->border_spec_rgba[k] = box->s->border_rgba[k];
        }
        if (!box->s->border_coll) box->s->border_spec_cset = box->s->border_color_set;
    }

    /* Blockification (CSS 2.1 §9.7, Flexbox §4): floats, absolutely
     * positioned boxes and flex/grid items have inline-level display values
     * blockified.  The parent's display is final. */
    if (box->s->display == CSS_DISPLAY_INLINE || box->s->display == CSS_DISPLAY_INLINE_BLOCK ||
        box->s->inline_level) {
        bool container_item = parent && (parent->s->display == CSS_DISPLAY_FLEX ||
                                         parent->s->display == CSS_DISPLAY_GRID);
        if (container_item || box->s->float_side != 0 ||
            box->s->position == CSS_POSITION_ABSOLUTE || box->s->position == CSS_POSITION_FIXED) {
            box->s->abs_inline = !container_item && box->s->float_side == 0;
            if (box->s->display == CSS_DISPLAY_INLINE || box->s->display == CSS_DISPLAY_INLINE_BLOCK)
                box->s->display = CSS_DISPLAY_BLOCK;
            box->s->inline_level = 0;
        }
    }
}

static void style_skip_descendants(StylePass *P, int pos, bool clean);

/* A subtree in which no element needs restyling, under a parent whose style
 * is as cached: every element takes its cached style as it is and text
 * boxes inherit.  Nothing is matched, so the ancestor filter is left alone. */
static void style_copy_subtree(StylePass *P, StyleThread *T, int pos)
{
    LayoutContext *ctx = P->ctx;
    const int *pre = ctx->tree.preorder;
    int end = P->pre_end[pos];
    uint32_t n = 0;
    for (int q = pos; q < end; q++) {
        int idx = pre[q];
        int p = ctx->tree.nodes[idx].parent_idx;
        LayoutBox *box = layout_box(ctx, idx);
        const StyleCacheEntry *ce = P->cached[idx];
        P->cls[idx] = idx;
        if (ce) {
            /* An element (one without an entry would need restyling). */
            if (P->retain && ctx->slot_ver[idx] == ce->ver) { P->kept[idx] = 1; P->kept_n++; }
            else layout_style_set(box, &ce->style);
            ctx->custom_props[idx] = ce->props;
            P->match_hash[idx] = ce->match_hash;
            ctx->pseudo_rec[idx] = ce->pseudo;
            if (ce->has_after) {
                if (T->gen_n == T->gen_cap) {
                    T->gen_cap = T->gen_cap ? T->gen_cap * 2 : 16;
                    T->gen = (StyleGenAfter *)realloc(T->gen, (size_t)T->gen_cap * sizeof(StyleGenAfter));
                }
                T->gen[T->gen_n].pos = q;
                T->gen[T->gen_n].idx = idx;
                memcpy(T->gen[T->gen_n].text, ce->after, sizeof(ce->after));
                T->gen_n++;
            }
            P->out_changed[idx] = 0;
            n++;
            /* Still display:none: what is below is not styled (and may be
             * stale or changed: style_count_dirty did not count it). */
            if (P->skip_hidden && box->s->display == CSS_DISPLAY_NONE && q + 1 < P->pre_end[q]) {
                style_skip_descendants(P, q, true);
                q = P->pre_end[q] - 1;
            }
        } else {
            ctx->custom_props[idx] = p >= 0 ? ctx->custom_props[p] : NULL;
            /* A text box under an element whose slot was left: its style as
             * it is. */
            if (!(P->retain && p >= 0 && P->kept[p] && ctx->style_ok && ctx->style_ok[idx])) {
                box->s = ctx->slot[idx];
                layout_box_init(ctx, idx);
                style_finish_box(P, box, p >= 0 ? layout_box(ctx, p) : NULL, false);
            } else {
                P->kept[idx] = 1;
            }
            P->out_changed[idx] = 1;
        }
    }
    atomic_fetch_add(&P->reused, n);
    atomic_fetch_add(&P->copied, n);
}

/* The descendants of the display:none element at preorder position `pos`
 * (whose style is as cached when `clean`): nothing renders or lays them
 * out, so they get display:none placeholders instead of styles.  Elements
 * that would have taken their cached style keep their entries; the others'
 * go stale (StylePass.skipped). */
static void style_skip_descendants(StylePass *P, int pos, bool clean)
{
    LayoutContext *ctx = P->ctx;
    const int *pre = ctx->tree.preorder;
    int top = pre[pos];
    int end = P->pre_end[pos];
    for (int q = pos + 1; q < end; q++) {
        int idx = pre[q];
        int p = ctx->tree.nodes[idx].parent_idx;
        bool parent_clean = p == top ? clean : P->skipped[p] == 1;
        bool keep = parent_clean && P->inc && P->cached[idx] && !P->cached[idx]->stale && P->key_ok[idx] && !P->force[idx];
        P->skipped[idx] = keep ? 1 : 2;
        /* Only these two fields: whatever reads a box past them checks
         * display or LAYOUT_STYLE_SKIPPED (s->skipped) first (a full box would be a
         * kilobyte per hidden node per layout). */
        LayoutBox *box = layout_box(ctx, idx);
        box->s->display = CSS_DISPLAY_NONE;
        box->s->skipped = 1;
        ctx->custom_props[idx] = ctx->custom_props[p];
        P->cls[idx] = idx;
        if (P->inc) P->out_changed[idx] = 1;
    }
    atomic_fetch_add(&P->skipped_n, (uint32_t)(end - pos - 1));
}

static void style_job(Job *j);

/* Style the subtree at preorder position `pos`; child subtrees with more
 * work than the grain become jobs of their own (their parent is already
 * styled).  With the restyle cache, work is the elements to restyle, and a
 * child subtree without any (under a parent styled as before) is copied. */
static void style_subtree(StylePass *P, StyleThread *T, Job *self, int pos)
{
    const int *pre = P->ctx->tree.preorder;
    int idx = pre[pos];
    style_node(P, T, pos, idx);
    int end = P->pre_end[pos];
    if (P->skip_hidden && layout_box(P->ctx, idx)->s->display == CSS_DISPLAY_NONE) {
        if (pos + 1 < end) style_skip_descendants(P, pos, P->inc && !P->out_changed[idx]);
        return;
    }
    bool by_dirty = P->dirty_n && !P->out_changed[idx];
    for (int c = pos + 1; c < end; c = P->pre_end[c]) {
        int work = P->pre_end[c] - c;
        if (by_dirty) {
            work = P->dirty_n[pre[c]];
            if (work == 0) { style_copy_subtree(P, T, c); continue; }
        }
        if (work > P->grain) {
            job_spawn(job_new(style_job, self, P, (uint32_t)c, 0));
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

/* Top-level subtrees with little to restyle run here, without waking the
 * pool. */
static void style_root_job(Job *j)
{
    StylePass *P = (StylePass *)j->ctx;
    int n = P->ctx->tree.count;
    const int *pre = P->ctx->tree.preorder;
    for (int pos = 0; pos < n; pos = P->pre_end[pos]) {
        if ((P->dirty_n && P->dirty_n[pre[pos]] <= P->grain) || P->grain == INT32_MAX)
            style_subtree(P, style_thread(P), j, pos);
        else
            job_spawn(job_new(style_job, j, P, (uint32_t)pos, 0));
    }
}

static const CssStyleSet *styleset_cached(uint64_t gen, double vw, double vh);

/* ---- One element restyled on demand ----
 *
 * Would the element at layout index `idx` of the kept layout `ctx`, as the
 * document now is (its node at `dom_now` there), compute the style it had
 * then?  Its cascade runs against the current document with its parent's
 * style as cached, and the result is compared with its own cache entry,
 * which is its style in `ctx` while no style pass has run since
 * (ctx->style_pass).  False when not known. */
bool css_layout_style_unchanged(LayoutContext *ctx, int idx, int dom_now)
{
    if (!ctx || !ctx->style_pass || ctx->style_pass != g_sc.passes || !g_sc.global || idx < 0 ||
        idx >= ctx->tree.count || !ctx->doc)
        return false;
    HtmlNode *hn = (HtmlNode *)po_array_payload(&ctx->doc->array, dom_now);
    if (!hn || hn->type != HTML_NODE_ELEMENT || !hn->store_id) return false;
    const StyleCacheEntry *ce = style_cache_find(hn->store_id);
    if (!ce || ce->stale) return false;
    int p = ctx->tree.nodes[idx].parent_idx;
    const StyleCacheEntry *pe = NULL;
    if (p >= 0) {
        /* The same parent as then, styled as cached. */
        int pd = po_array_parent(&ctx->doc->array, dom_now);
        HtmlNode *pn = pd >= 0 ? (HtmlNode *)po_array_payload(&ctx->doc->array, pd) : NULL;
        HtmlNode *was = layout_node_dom(ctx, ctx->tree.nodes[p].dom_node_idx);
        if (!pn || !pn->store_id || !was || was->store_id != pn->store_id) return false;
        pe = style_cache_find(pn->store_id);
        if (!pe || pe->stale) return false;
    }
    const CssStyleSet *set = styleset_cached(ctx->style_gen, ctx->viewport_width, ctx->viewport_height);
    if (!set) return false;

    StylePass P;
    memset(&P, 0, sizeof(P));
    P.ctx = ctx;
    P.set = set;
    P.sd = css_styledoc_get(ctx->doc);
    P.em_defer = cyber_getenv("CYBER_DISABLE_EM_DEFER") == NULL;
    P.cls = (int *)calloc((size_t)ctx->tree.count + 1, sizeof(int));
    P.threads = (StyleThread *)calloc(1, sizeof(StyleThread));
    if (!P.sd || !P.cls || !P.threads) { free(P.cls); free(P.threads); return false; }
    for (int i = 0; i < ctx->tree.count; i++) P.cls[i] = i;
    /* Styled into the kept layout's slots, then put back. */
    LayoutBox *box = &ctx->boxes[idx];
    LayoutStyle kept_style = *box->s, parent_kept_style;
    const CssCustomProps *kept_props = ctx->custom_props[idx], *parent_props = NULL;
    int kept_dom = ctx->tree.nodes[idx].dom_node_idx;
    if (p >= 0) {
        parent_kept_style = *ctx->boxes[p].s;
        parent_props = ctx->custom_props[p];
        layout_style_set(&ctx->boxes[p], &pe->style);
        ctx->custom_props[p] = pe->props;
    }
    ctx->tree.nodes[idx].dom_node_idx = dom_now;
    double rem = g_unit_rem;
    StyleThread *T = style_thread(&P);
    style_node(&P, T, 0, idx);
    bool after = T->gen_n > 0;
    bool same = layout_style_same(box, &ce->style) && style_props_equal(ctx->custom_props[idx], ce->props) &&
                after == (ce->has_after != 0) &&
                (!after || strncmp(T->gen[0].text, ce->after, sizeof(ce->after)) == 0);
    g_unit_rem = rem;
    ctx->tree.nodes[idx].dom_node_idx = kept_dom;
    layout_style_set(box, &kept_style);
    ctx->custom_props[idx] = kept_props;
    if (p >= 0) {
        layout_style_set(&ctx->boxes[p], &parent_kept_style);
        ctx->custom_props[p] = parent_props;
    }
    free(T->gen);
    if (T->scratch) css_style_scratch_free(T->scratch);
    frame_arena_reset(&T->arena);
    free(P.threads);
    free(P.cls);
    return same;
}

static int style_gen_cmp(const void *a, const void *b)
{
    return ((const StyleGenAfter *)a)->pos - ((const StyleGenAfter *)b)->pos;
}

/* Mix an adopted-sheet array (its sheets' identities) into h. */
static uint64_t layout_mix_sheet_array(JSContextHandle js, GCValue arr, uint64_t h)
{
    if (!JS_IsArray(js, arr)) return style_mix(h, "-", 1);
    uint32_t n = 0;
    JS_ToUint32(js, &n, JS_GetPropertyStr(js, arr, "length"));
    h = style_mix(h, &n, sizeof(n));
    for (uint32_t i = 0; i < n; i++) {
        GCValue s = JS_GetPropertyUint32(js, arr, i);
        h = style_mix(h, &s, sizeof(s));
    }
    return h;
}

/* What decides the adopted sheets layout_collect_constructed_stylesheets
 * collects, besides their text (whose edits request a relayout): which
 * sheets the document and each registered shadow root adopt, and whether
 * each root's host is connected. */
static uint64_t layout_adopted_fingerprint(LayoutContext *ctx)
{
    if (!SAFE_VALID(ctx->js_ctx) || gc_handle_get_type(ctx->js_ctx) != JS_GC_OBJ_TYPE_JS_CONTEXT) return 0;
    JSContextHandle js = ctx->js_ctx;
    uint64_t h = 1469598103934665603ull;
    GCValue global = JS_GetGlobalObject(js);
    GCValue doc = JS_GetPropertyStr(js, global, "document");
    if (JS_IsObject(doc)) h = layout_mix_sheet_array(js, JS_GetPropertyStr(js, doc, "__adoptedStyleSheets"), h);
    GCValue roots = JS_GetPropertyStr(js, global, "__cyber_adopting_roots");
    if (JS_IsArray(js, roots)) {
        uint32_t n = 0;
        JS_ToUint32(js, &n, JS_GetPropertyStr(js, roots, "length"));
        for (uint32_t i = 0; i < n; i++) {
            GCValue root = JS_GetPropertyUint32(js, roots, i);
            GCValue host = JS_GetPropertyStr(js, root, "host");
            unsigned char connected = JS_IsObject(host) && JS_ToBool(js, JS_GetPropertyStr(js, host, "isConnected"));
            h = style_mix(h, &host, sizeof(host));
            h = style_mix(h, &connected, 1);
            h = layout_mix_sheet_array(js, JS_GetPropertyStr(js, root, "__adoptedStyleSheets"), h);
        }
    }
    return h;
}

/* Everything the sheets a layout of ctx->doc collects come from: its style
 * and link elements (doc->sheet_gen), CSSOM edits and adopted-sheet
 * assignments (g_dom_relayout_requests), the adopted sheets themselves and
 * the base URL.  0 when not known (a document edited untracked). */
static uint64_t layout_sheets_key(LayoutContext *ctx)
{
    HtmlDocument *doc = ctx->doc;
    if (!doc || !doc->sheets_tracked || !doc->serial) return 0;
    uint64_t v[4] = { doc->serial, doc->sheet_gen, g_dom_relayout_requests, layout_adopted_fingerprint(ctx) };
    uint64_t h = style_mix(1469598103934665603ull, v, sizeof(v));
    h = style_mix(h, ctx->base_url, strlen(ctx->base_url));
    return h ? h : 1;
}

/* The style set the last layout with these sheets used (layouts in a row
 * collect, hash and look up the same megabytes of sheets otherwise).
 * CSS_SHEETS_MEMO=0: always collect. */
static struct {
    uint64_t key, set_gen;
    double vw, vh;
    int nsheets;
} g_sheets_memo;

static const CssStyleSet *styleset_cached(uint64_t gen, double vw, double vh)
{
    for (int i = 0; i < STYLESET_CACHE_MAX; i++) {
        StyleSetEntry *c = &g_styleset_cache[i];
        if (!c->set || c->gen != gen || c->vw != vw || c->vh != vh) continue;
        c->last_use = g_layout_clock;
        return c->set;
    }
    return NULL;
}

static void layout_restyle_all(LayoutContext *ctx);

/* ---- Pseudo-element boxes (::before, ::after) ----
 * The style pass notes what each element's ::before and ::after styles say
 * (ctx->pseudo_rec); then layout_pseudo_attach adds a box for each one that
 * has content: a pseudo element node (tag "::before" / "::after") holding
 * one text node, after the document-built nodes (base_count of them), linked
 * in as the element's first or last child, styled from the element's final
 * style and its pseudo rules.  Their document nodes are the layout's own
 * (pseudo_nodes).  A kept tree drops them (layout_pseudo_detach) before its
 * next style pass. */

static void style_finish_box_em(bool em_defer, LayoutBox *box, const LayoutBox *parent, bool element);

/* Unicode punctuation (general category P*), generated from Unicode data. */
static const uint32_t k_punct_ranges[][2] = {
    { 0x21, 0x23 }, { 0x25, 0x2A }, { 0x2C, 0x2F }, { 0x3A, 0x3B },
    { 0x3F, 0x40 }, { 0x5B, 0x5D }, { 0x5F, 0x5F }, { 0x7B, 0x7B },
    { 0x7D, 0x7D }, { 0xA1, 0xA1 }, { 0xA7, 0xA7 }, { 0xAB, 0xAB },
    { 0xB6, 0xB7 }, { 0xBB, 0xBB }, { 0xBF, 0xBF }, { 0x37E, 0x37E },
    { 0x387, 0x387 }, { 0x55A, 0x55F }, { 0x589, 0x58A }, { 0x5BE, 0x5BE },
    { 0x5C0, 0x5C0 }, { 0x5C3, 0x5C3 }, { 0x5C6, 0x5C6 }, { 0x5F3, 0x5F4 },
    { 0x609, 0x60A }, { 0x60C, 0x60D }, { 0x61B, 0x61B }, { 0x61D, 0x61F },
    { 0x66A, 0x66D }, { 0x6D4, 0x6D4 }, { 0x700, 0x70D }, { 0x7F7, 0x7F9 },
    { 0x830, 0x83E }, { 0x85E, 0x85E }, { 0x964, 0x965 }, { 0x970, 0x970 },
    { 0x9FD, 0x9FD }, { 0xA76, 0xA76 }, { 0xAF0, 0xAF0 }, { 0xC77, 0xC77 },
    { 0xC84, 0xC84 }, { 0xDF4, 0xDF4 }, { 0xE4F, 0xE4F }, { 0xE5A, 0xE5B },
    { 0xF04, 0xF12 }, { 0xF14, 0xF14 }, { 0xF3A, 0xF3D }, { 0xF85, 0xF85 },
    { 0xFD0, 0xFD4 }, { 0xFD9, 0xFDA }, { 0x104A, 0x104F }, { 0x10FB, 0x10FB },
    { 0x1360, 0x1368 }, { 0x1400, 0x1400 }, { 0x166E, 0x166E }, { 0x169B, 0x169C },
    { 0x16EB, 0x16ED }, { 0x1735, 0x1736 }, { 0x17D4, 0x17D6 }, { 0x17D8, 0x17DA },
    { 0x1800, 0x180A }, { 0x1944, 0x1945 }, { 0x1A1E, 0x1A1F }, { 0x1AA0, 0x1AA6 },
    { 0x1AA8, 0x1AAD }, { 0x1B4E, 0x1B4F }, { 0x1B5A, 0x1B60 }, { 0x1B7D, 0x1B7F },
    { 0x1BFC, 0x1BFF }, { 0x1C3B, 0x1C3F }, { 0x1C7E, 0x1C7F }, { 0x1CC0, 0x1CC7 },
    { 0x1CD3, 0x1CD3 }, { 0x2010, 0x2027 }, { 0x2030, 0x2043 }, { 0x2045, 0x2051 },
    { 0x2053, 0x205E }, { 0x207D, 0x207E }, { 0x208D, 0x208E }, { 0x2308, 0x230B },
    { 0x2329, 0x232A }, { 0x2768, 0x2775 }, { 0x27C5, 0x27C6 }, { 0x27E6, 0x27EF },
    { 0x2983, 0x2998 }, { 0x29D8, 0x29DB }, { 0x29FC, 0x29FD }, { 0x2CF9, 0x2CFC },
    { 0x2CFE, 0x2CFF }, { 0x2D70, 0x2D70 }, { 0x2E00, 0x2E2E }, { 0x2E30, 0x2E4F },
    { 0x2E52, 0x2E5D }, { 0x3001, 0x3003 }, { 0x3008, 0x3011 }, { 0x3014, 0x301F },
    { 0x3030, 0x3030 }, { 0x303D, 0x303D }, { 0x30A0, 0x30A0 }, { 0x30FB, 0x30FB },
    { 0xA4FE, 0xA4FF }, { 0xA60D, 0xA60F }, { 0xA673, 0xA673 }, { 0xA67E, 0xA67E },
    { 0xA6F2, 0xA6F7 }, { 0xA874, 0xA877 }, { 0xA8CE, 0xA8CF }, { 0xA8F8, 0xA8FA },
    { 0xA8FC, 0xA8FC }, { 0xA92E, 0xA92F }, { 0xA95F, 0xA95F }, { 0xA9C1, 0xA9CD },
    { 0xA9DE, 0xA9DF }, { 0xAA5C, 0xAA5F }, { 0xAADE, 0xAADF }, { 0xAAF0, 0xAAF1 },
    { 0xABEB, 0xABEB }, { 0xFD3E, 0xFD3F }, { 0xFE10, 0xFE19 }, { 0xFE30, 0xFE52 },
    { 0xFE54, 0xFE61 }, { 0xFE63, 0xFE63 }, { 0xFE68, 0xFE68 }, { 0xFE6A, 0xFE6B },
    { 0xFF01, 0xFF03 }, { 0xFF05, 0xFF0A }, { 0xFF0C, 0xFF0F }, { 0xFF1A, 0xFF1B },
    { 0xFF1F, 0xFF20 }, { 0xFF3B, 0xFF3D }, { 0xFF3F, 0xFF3F }, { 0xFF5B, 0xFF5B },
    { 0xFF5D, 0xFF5D }, { 0xFF5F, 0xFF65 }, { 0x10100, 0x10102 }, { 0x1039F, 0x1039F },
    { 0x103D0, 0x103D0 }, { 0x1056F, 0x1056F }, { 0x10857, 0x10857 }, { 0x1091F, 0x1091F },
    { 0x1093F, 0x1093F }, { 0x10A50, 0x10A58 }, { 0x10A7F, 0x10A7F }, { 0x10AF0, 0x10AF6 },
    { 0x10B39, 0x10B3F }, { 0x10B99, 0x10B9C }, { 0x10D6E, 0x10D6E }, { 0x10EAD, 0x10EAD },
    { 0x10ED0, 0x10ED0 }, { 0x10F55, 0x10F59 }, { 0x10F86, 0x10F89 }, { 0x11047, 0x1104D },
    { 0x110BB, 0x110BC }, { 0x110BE, 0x110C1 }, { 0x11140, 0x11143 }, { 0x11174, 0x11175 },
    { 0x111C5, 0x111C8 }, { 0x111CD, 0x111CD }, { 0x111DB, 0x111DB }, { 0x111DD, 0x111DF },
    { 0x11238, 0x1123D }, { 0x112A9, 0x112A9 }, { 0x113D4, 0x113D5 }, { 0x113D7, 0x113D8 },
    { 0x1144B, 0x1144F }, { 0x1145A, 0x1145B }, { 0x1145D, 0x1145D }, { 0x114C6, 0x114C6 },
    { 0x115C1, 0x115D7 }, { 0x11641, 0x11643 }, { 0x11660, 0x1166C }, { 0x116B9, 0x116B9 },
    { 0x1173C, 0x1173E }, { 0x1183B, 0x1183B }, { 0x11944, 0x11946 }, { 0x119E2, 0x119E2 },
    { 0x11A3F, 0x11A46 }, { 0x11A9A, 0x11A9C }, { 0x11A9E, 0x11AA2 }, { 0x11B00, 0x11B09 },
    { 0x11BE1, 0x11BE1 }, { 0x11C41, 0x11C45 }, { 0x11C70, 0x11C71 }, { 0x11EF7, 0x11EF8 },
    { 0x11F43, 0x11F4F }, { 0x11FFF, 0x11FFF }, { 0x12470, 0x12474 }, { 0x12FF1, 0x12FF2 },
    { 0x16A6E, 0x16A6F }, { 0x16AF5, 0x16AF5 }, { 0x16B37, 0x16B3B }, { 0x16B44, 0x16B44 },
    { 0x16D6D, 0x16D6F }, { 0x16E97, 0x16E9A }, { 0x16FE2, 0x16FE2 }, { 0x1BC9F, 0x1BC9F },
    { 0x1DA87, 0x1DA8B }, { 0x1E5FF, 0x1E5FF }, { 0x1E95E, 0x1E95F },
};

static bool layout_is_punct(uint32_t cp)
{
    int lo = 0, hi = (int)(sizeof(k_punct_ranges) / sizeof(k_punct_ranges[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < k_punct_ranges[mid][0]) hi = mid - 1;
        else if (cp > k_punct_ranges[mid][1]) lo = mid + 1;
        else return true;
    }
    return false;
}

static uint32_t layout_utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = s[0];
    int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (n > 1) c &= 0x3F >> (n - 1);
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { n = i; break; }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *p += n;
    return c;
}

/* Replace document node dom_idx's text for this layout (copied). */
static void layout_set_node_text(LayoutContext *ctx, int dom_idx, const char *text, size_t len)
{
    if (!ctx->doc || dom_idx < 0) return;
    if (!ctx->text_gen) {
        int cap = (int)po_array_count(&ctx->doc->array);
        ctx->text_gen = (char **)calloc((size_t)cap + 1, sizeof(char *));
        if (!ctx->text_gen) return;
        ctx->text_gen_cap = cap;
    }
    if (dom_idx >= ctx->text_gen_cap) return;
    char *t = (char *)malloc(len + 1);
    if (!t) return;
    memcpy(t, text, len);
    t[len] = '\0';
    free(ctx->text_gen[dom_idx]);
    ctx->text_gen[dom_idx] = t;
}

static bool layout_text_is_whitespace(const char *s);
static bool layout_pseudo_allowed(const HtmlNode *n);

/* The text box an element's ::first-letter takes its letter from: the first
 * in-flow text with more than white space, through inline and block
 * descendants (-1: none, as when a replaced element or an inline-block
 * comes first). */
static int layout_first_letter_text(LayoutContext *ctx, int idx, int depth)
{
    if (depth > 32) return -1;
    for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        const LayoutBox *b = &ctx->boxes[c];
        if (b->s->display == CSS_DISPLAY_NONE || b->s->float_side != 0 ||
            b->s->position == CSS_POSITION_ABSOLUTE || b->s->position == CSS_POSITION_FIXED)
            continue;
        HtmlNode *n = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
        if (!n) continue;
        if (n->type == HTML_NODE_TEXT) {
            const char *t = css_layout_node_text(ctx, n);
            if (t && !layout_text_is_whitespace(t)) return c;
            continue;
        }
        if (n->type != HTML_NODE_ELEMENT) continue;
        if (!layout_pseudo_allowed(n) || b->s->display == CSS_DISPLAY_INLINE_BLOCK || b->s->inline_level)
            return -1;
        int r = layout_first_letter_text(ctx, c, depth + 1);
        if (r != -1) return r;
    }
    return -1;
}

/* Where the first letter of text t is: [*from, *to) (leading white space,
 * then punctuation, a letter and the punctuation after it); false for none. */
static bool layout_first_letter_span(const char *t, size_t *from, size_t *to)
{
    const char *p = t;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f') p++;
    *from = (size_t)(p - t);
    const char *q = p;
    bool letter = false;
    while (*q) {
        const char *r = q;
        uint32_t cp = layout_utf8_next(&r);
        if (layout_is_punct(cp)) { q = r; continue; }
        if (cp == ' ' || cp == '\t' || cp == '\n') break;
        q = r;
        letter = true;
        break;
    }
    if (!letter) return false;
    while (*q) {
        const char *r = q;
        uint32_t cp = layout_utf8_next(&r);
        if (!layout_is_punct(cp)) break;
        q = r;
    }
    *to = (size_t)(q - t);
    return true;
}

/* No ::before / ::after boxes: replaced and void elements. */
static bool layout_pseudo_allowed(const HtmlNode *n)
{
    static const char *const none[] = {
        "img", "input", "select", "textarea", "iframe", "video", "audio", "canvas", "embed", "object",
        "br", "wbr", "svg", "math", "meter", "progress", NULL
    };
    if (!n || n->type != HTML_NODE_ELEMENT || !n->tag_name) return false;
    for (int i = 0; none[i]; i++)
        if (strcasecmp(n->tag_name, none[i]) == 0) return false;
    return true;
}

/* A content value's text: strings, attr() of the element, quotes (counters
 * and images give none yet). */
static void layout_pseudo_text(const char *v, const HtmlNode *owner, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = 0;
    while (*v) {
        while (isspace((unsigned char)*v)) v++;
        if (!*v) break;
        const char *piece = NULL;
        char buf[1024];
        size_t tl = 0;
        if (*v == '"' || *v == '\'') {
            char q = *v;
            tl = 1;
            while (v[tl] && v[tl] != q) tl += v[tl] == '\\' && v[tl + 1] ? 2 : 1;
            if (v[tl] == q) tl++;
            if (tl < sizeof(buf)) {
                char tok[1024];
                memcpy(tok, v, tl);
                tok[tl] = 0;
                bool empty = false;
                if (css_content_decode_string(tok, buf, sizeof(buf), &empty)) piece = buf;
            }
        } else {
            int depth = 0;
            while (v[tl] && (depth > 0 || !isspace((unsigned char)v[tl]))) {
                if (v[tl] == '(') depth++;
                else if (v[tl] == ')' && depth > 0) depth--;
                tl++;
            }
            if (!strncasecmp(v, "attr(", 5)) {
                char name[128];
                size_t nl = 0;
                const char *a = v + 5;
                while (isspace((unsigned char)*a)) a++;
                while (*a && *a != ')' && !isspace((unsigned char)*a) && nl + 1 < sizeof(name)) name[nl++] = *a++;
                name[nl] = 0;
                /* (attribute names match ignoring case in an HTML document only) */
                bool html = !owner->store_id || dom_is_html_document(dom_owner(owner->store_id));
                for (const HtmlAttribute *at = owner->attributes; at && !piece; at = at->next)
                    if (at->name && (html ? !strcasecmp(at->name, name) : !strcmp(at->name, name))) piece = at->value;
            } else if (!strncasecmp(v, "open-quote", tl) && tl == 10) {
                piece = "\xE2\x80\x9C";
            } else if (!strncasecmp(v, "close-quote", tl) && tl == 11) {
                piece = "\xE2\x80\x9D";
            }
        }
        if (piece) {
            size_t pl = strlen(piece);
            if (o + pl >= cap) pl = cap - 1 - o;
            memcpy(out + o, piece, pl);
            o += pl;
            out[o] = 0;
        }
        v += tl ? tl : 1;
    }
}

/* The style of pseudo box pe (side 0 ::before, 1 ::after) of element owner:
 * its rules' declarations over the defaults, then what the element's final
 * style gives it. */
static void layout_pseudo_style(LayoutContext *ctx, int pe, int owner, const PseudoSide *ps, bool em_defer)
{
    LayoutBox *box = &ctx->boxes[pe], *ob = &ctx->boxes[owner];
    layout_box_init(ctx, pe);
    const CssCustomProps *props = ctx->custom_props[owner];
    for (int d = 0; d < ps->n; d++) {
        const CssDeclaration *src = ps->decls[d];
        const char *prop = src->property;
        if (!prop || (prop[0] == '-' && prop[1] == '-')) continue;
        t_var_failed = false;
        char *resolved = css_var_resolve(src->value, props);
        CssDeclaration decl = *src;
        decl.value = resolved ? resolved : src->value;
        layout_decl_copy_validity(&decl, src, resolved != NULL);
        if (t_var_failed) layout_unset_property(box, decl.property);
        else if (!layout_apply_wide_keyword(box, ob, decl.property, decl.value, ctx->viewport_width, ctx->base_url))
            layout_apply_declaration(box, &decl, ctx->viewport_width, ctx->viewport_width, ctx->base_url);
        free(resolved);
    }
    style_finish_box_em(em_defer, box, ob, true);
    /* (a column or column group renders no content) */
    if (box->s->table_role == 6) box->s->display = CSS_DISPLAY_NONE;
}

/* Grow a per-box array to n entries (+1), new ones zero (fill: their byte). */
static bool layout_grow(void **p, int old_n, int n, size_t each, int fill)
{
    if (!*p) return true;
    void *np = realloc(*p, ((size_t)n + 1) * each);
    if (!np) return false;
    memset((char *)np + (size_t)old_n * each, fill, ((size_t)(n - old_n) + 1) * each);
    *p = np;
    return true;
}

/* The per-box arrays grown from old_n to n boxes (the new ones cleared). */
static bool layout_grow_box_arrays(LayoutContext *ctx, int old_n, int n)
{
    return layout_grow((void **)&ctx->slot, old_n, n, sizeof(LayoutStyle *), 0) &&
           layout_grow((void **)&ctx->ids, old_n, n, sizeof(uint32_t), 0) &&
           layout_grow((void **)&ctx->custom_props, old_n, n, sizeof(CssCustomProps *), 0) &&
           layout_grow((void **)&ctx->style_ok, old_n, n, 1, 0) &&
           layout_grow((void **)&ctx->style_left, old_n, n, 1, 0) &&
           layout_grow((void **)&ctx->pruned, old_n, n, 1, 0) &&
           layout_grow((void **)&ctx->has_flags, old_n, n, 1, 0) &&
           layout_grow((void **)&ctx->sc_key, old_n, n, sizeof(uint64_t), 0) &&
           layout_grow((void **)&ctx->sc_entry, old_n, n, sizeof(int), 0) &&
           layout_grow((void **)&ctx->slot_ver, old_n, n, sizeof(uint32_t), 0) &&
           layout_grow((void **)&ctx->has_bits, old_n, n, 8, 0xFF) &&
           layout_grow((void **)&ctx->flow_old, old_n, n, sizeof(int), 0xFF);
}

static void layout_drop_text_caches(LayoutContext *ctx)
{
    if (ctx->text_norm) {
        for (int i = 0; i < ctx->tree.count; i++) free(ctx->text_norm[i]);
        free(ctx->text_norm);
        ctx->text_norm = NULL;
    }
    free(ctx->intrinsic_cache);
    ctx->intrinsic_cache = NULL;
    for (int i = 0; ctx->pieces && i < ctx->pieces_n; i++) free(ctx->pieces[i].p);
    free(ctx->pieces);
    layout_text_frags_free(ctx);
    ctx->pieces = NULL;
    ctx->pieces_n = 0;
}

static bool layout_build_orders(LayoutContext *ctx);

static void layout_text_frags_free(LayoutContext *ctx)
{
    for (int i = 0; ctx->text_frags && i < ctx->text_frags_n; i++) free(ctx->text_frags[i].f);
    free(ctx->text_frags);
    ctx->text_frags = NULL;
    ctx->text_frags_n = 0;
}

static void layout_rebuild_orders(LayoutContext *ctx)
{
    free(ctx->tree.preorder);
    free(ctx->tree.postorder);
    ctx->tree.preorder = ctx->tree.postorder = NULL;
    layout_build_orders(ctx);
}


/* The parked boxes nobody took: their slots and strings given back. */
static void layout_parked_release(LayoutContext *ctx)
{
    for (int j = 0; j < ctx->parked_n; j++) {
        LayoutParkedPseudo *p = &ctx->parked[j];
        free((void *)p->decls);
        if (p->used) continue;
        layout_slot_give(ctx, p->slot_e);
        layout_slot_give(ctx, p->slot_t);
        free(p->t.text_content);
    }
    free(ctx->parked);
    ctx->parked = NULL;
    ctx->parked_n = 0;
}

/* Back to the document-built tree: the pseudo boxes out of their elements'
 * children, parked for the next layout (park) or their slots given back. */
/* Link the document-built boxes among c and its following siblings under
 * owner (an added box: the document-built boxes in it, an anonymous
 * table box's children). */
static void layout_detach_keep(LayoutNodeRef *nodes, int base, int owner, int c, int *prev, int *cnt)
{
    while (c >= 0) {
        int next = nodes[c].next_sibling_idx;
        if (c < base) {
            nodes[c].parent_idx = owner;
            nodes[c].prev_sibling_idx = *prev;
            nodes[c].next_sibling_idx = -1;
            if (*prev < 0) nodes[owner].first_child_idx = c;
            else nodes[*prev].next_sibling_idx = c;
            *prev = c;
            ++*cnt;
        } else {
            layout_detach_keep(nodes, base, owner, nodes[c].first_child_idx, prev, cnt);
        }
        c = next;
    }
}

static void layout_pseudo_detach_ex(LayoutContext *ctx, bool park)
{
    layout_parked_release(ctx);
    int base = ctx->base_count, n = ctx->tree.count;
    if (base <= 0 || n <= base) return;
    LayoutNodeRef *nodes = ctx->tree.nodes;
    for (int i = base; i < n; i++) {
        int owner = nodes[i].parent_idx;
        if (owner < 0 || owner >= base) continue;   /* (a pseudo box's text) */
        int prev = -1, cnt = 0;
        int c = nodes[owner].first_child_idx;
        nodes[owner].first_child_idx = -1;
        layout_detach_keep(nodes, base, owner, c, &prev, &cnt);
        nodes[owner].child_count = cnt;
    }
    layout_drop_text_caches(ctx);
    for (int i = base + ctx->pseudo_nodes_n; i < n; i++) layout_slot_give(ctx, ctx->slot[i]);
    free(ctx->anon_nodes);
    ctx->anon_nodes = NULL;
    ctx->anon_nodes_n = 0;
    n = base + ctx->pseudo_nodes_n;
    int pairs = ctx->pseudo_nodes_n / 2;
    if (park && ctx->style_gen && ctx->pseudo_pairs && ctx->ids && pairs > 0 &&
        (ctx->parked = (LayoutParkedPseudo *)calloc((size_t)pairs, sizeof(LayoutParkedPseudo)))) {
        for (int j = 0; j < pairs; j++) {
            const LayoutPseudoPair *pp = &ctx->pseudo_pairs[j];
            LayoutParkedPseudo *p = &ctx->parked[j];
            int pe = base + 2 * j;
            if (pp->no_park) {
                /* (not to be taken back: its slots and string go now) */
                layout_slot_give(ctx, ctx->slot[pe]);
                layout_slot_give(ctx, ctx->slot[pe + 1]);
                free(ctx->pseudo_nodes[2 * j + 1].text_content);
                p->used = 1;
                continue;
            }
            p->owner_id = ctx->ids[pp->owner];
            p->side = pp->side;
            p->old_pe = pe;
            p->n = pp->n;
            if (pp->n) {
                const CssDeclaration **ds = (const CssDeclaration **)malloc((size_t)pp->n * sizeof(*ds));
                if (ds) memcpy(ds, pp->decls, (size_t)pp->n * sizeof(*ds));
                else p->n = -1;   /* (matches nothing) */
                p->decls = ds;
            }
            p->content = pp->content;
            p->slot_e = ctx->slot[pe];
            p->slot_t = ctx->slot[pe + 1];
            p->e = ctx->pseudo_nodes[2 * j];
            p->t = ctx->pseudo_nodes[2 * j + 1];
        }
        ctx->parked_n = pairs;
        ctx->parked_gen = ctx->style_gen;
    } else {
        for (int i = base; i < n; i++) layout_slot_give(ctx, ctx->slot[i]);
        for (int k = 0; k < ctx->pseudo_nodes_n; k++) free(ctx->pseudo_nodes[k].text_content);
    }
    free(ctx->pseudo_nodes);
    ctx->pseudo_nodes = NULL;
    ctx->pseudo_nodes_n = 0;
    free(ctx->pseudo_pairs);
    ctx->pseudo_pairs = NULL;
    ctx->tree.count = base;
    layout_rebuild_orders(ctx);
}

static void layout_pseudo_detach(LayoutContext *ctx)
{
    layout_pseudo_detach_ex(ctx, true);
}

/* The parked box of element store id id on side side (-1: none). */
static int layout_parked_find(const LayoutContext *ctx, const int *tab, int cap, uint32_t id, int side)
{
    if (!tab || !id) return -1;
    uint32_t h = ((id * 2u + (uint32_t)side) * 2654435761u) & (uint32_t)(cap - 1);
    for (int v; (v = tab[h]) != 0; h = (h + 1) & (uint32_t)(cap - 1)) {
        const LayoutParkedPseudo *p = &ctx->parked[v - 1];
        if (p->owner_id == id && p->side == side) return v - 1;
    }
    return -1;
}

static void layout_pseudo_attach(LayoutContext *ctx, bool em_defer)
{
    int base = ctx->tree.count;
    ctx->base_count = base;
    if (!ctx->pseudo_rec) { layout_parked_release(ctx); return; }
    int m = 0;
    for (int i = 0; i < base; i++) {
        const PseudoRec *r = (const PseudoRec *)ctx->pseudo_rec[i];
        if (!r) continue;
        const LayoutBox *b = &ctx->boxes[i];
        if (b->s->display == CSS_DISPLAY_NONE || b->s->skipped) continue;
        if (!layout_pseudo_allowed(layout_node_dom(ctx, ctx->tree.nodes[i].dom_node_idx))) continue;
        m += (r->side[0].content != NULL) + (r->side[1].content != NULL) + (r->side[2].content != NULL);
    }
    if (m == 0) { layout_parked_release(ctx); return; }
    int n = base + 2 * m;
    LayoutNodeRef *nn = (LayoutNodeRef *)realloc(ctx->tree.nodes, (size_t)n * sizeof(LayoutNodeRef));
    if (!nn) return;
    ctx->tree.nodes = nn;
    LayoutBox *nb = (LayoutBox *)realloc(ctx->boxes, (size_t)n * sizeof(LayoutBox));
    if (!nb) return;
    ctx->boxes = nb;
    HtmlNode *hn = (HtmlNode *)calloc((size_t)2 * m, sizeof(HtmlNode));
    LayoutPseudoPair *pairs = (LayoutPseudoPair *)calloc((size_t)m, sizeof(LayoutPseudoPair));
    if (!hn || !pairs) { free(hn); free(pairs); layout_parked_release(ctx); return; }
    if (!layout_grow_box_arrays(ctx, base, n)) {
        free(hn);
        free(pairs);
        layout_parked_release(ctx);
        return;
    }
    /* The boxes of the last layout this one may take back (same style set),
     * by element and side. */
    int ptab_cap = 0, *ptab = NULL;
    if (ctx->parked_n && ctx->parked_gen == ctx->style_gen && ctx->style_left) {
        ptab_cap = 16;
        while (ptab_cap < 2 * ctx->parked_n) ptab_cap *= 2;
        ptab = (int *)calloc((size_t)ptab_cap, sizeof(int));
        for (int j = 0; ptab && j < ctx->parked_n; j++) {
            const LayoutParkedPseudo *p = &ctx->parked[j];
            if (!p->owner_id) continue;
            uint32_t h = ((p->owner_id * 2u + p->side) * 2654435761u) & (uint32_t)(ptab_cap - 1);
            while (ptab[h]) h = (h + 1) & (uint32_t)(ptab_cap - 1);
            ptab[h] = j + 1;
        }
    }
    int reused = 0;
    layout_drop_text_caches(ctx);
    ctx->pseudo_nodes = hn;
    ctx->pseudo_nodes_n = 2 * m;
    free(ctx->pseudo_pairs);
    ctx->pseudo_pairs = pairs;
    int k = 0;
    for (int owner = 0; owner < base; owner++) {
        const PseudoRec *r = (const PseudoRec *)ctx->pseudo_rec[owner];
        if (!r) continue;
        const LayoutBox *b = &ctx->boxes[owner];
        HtmlNode *on = layout_node_dom(ctx, ctx->tree.nodes[owner].dom_node_idx);
        if (b->s->display == CSS_DISPLAY_NONE || b->s->skipped || !layout_pseudo_allowed(on)) continue;
        for (int side = 0; side < 3; side++) {
            const PseudoSide *ps = &r->side[side];
            if (!ps->content) continue;
            int pe = base + k, pt = pe + 1;
            if (side == 2) {
                /* ::first-letter (block containers): a box with the first
                 * letter of the first text, before that text, which goes on
                 * with the rest. */
                CssDisplay od = b->s->display;
                if (od != CSS_DISPLAY_BLOCK && od != CSS_DISPLAY_LIST_ITEM && od != CSS_DISPLAY_INLINE_BLOCK &&
                    !(od == CSS_DISPLAY_OTHER && (b->s->table_role == 4 || b->s->table_role == 5)))
                    continue;
                int t = layout_first_letter_text(ctx, owner, 0);
                if (t < 0) continue;
                HtmlNode *tn = layout_node_dom(ctx, ctx->tree.nodes[t].dom_node_idx);
                const char *tt = tn ? css_layout_node_text(ctx, tn) : NULL;
                size_t from = 0, to = 0;
                if (!tt || !layout_first_letter_span(tt, &from, &to)) continue;
                int par = ctx->tree.nodes[t].parent_idx;
                pairs[k / 2] = (LayoutPseudoPair){ owner, 2, 1, 0, ps->n, ps->decls, ps->content };
                HtmlNode *he = &hn[k], *ht = &hn[k + 1];
                he->array_node.parent = he->array_node.first_child = he->array_node.next_sibling = he->array_node.prev_sibling = -1;
                ht->array_node = he->array_node;
                he->type = HTML_NODE_ELEMENT;
                he->tag_name = "::first-letter";
                ht->type = HTML_NODE_TEXT;
                ht->tag_name = "";
                ht->text_content = (char *)malloc(to - from + 1);
                if (ht->text_content) { memcpy(ht->text_content, tt + from, to - from); ht->text_content[to - from] = 0; }
                ht->text_len = ht->text_content ? to - from : 0;
                /* (the rest: a document text's for this layout, a pseudo box's its own) */
                const char *rest = tt + to;
                int td = ctx->tree.nodes[t].dom_node_idx;
                if (td >= LAYOUT_PSEUDO_DOM0) {
                    char *nt = strdup(rest);
                    if (nt) { free(tn->text_content); tn->text_content = nt; tn->text_len = strlen(nt); }
                    if (t >= base && t - base < 2 * (k / 2 + 1)) pairs[(t - base) / 2].no_park = 1;
                } else {
                    layout_set_node_text(ctx, td, rest, strlen(rest));
                }
                LayoutNodeRef *E = &ctx->tree.nodes[pe], *T = &ctx->tree.nodes[pt], *TN = &ctx->tree.nodes[t];
                E->dom_node_idx = LAYOUT_PSEUDO_DOM0 + k;
                T->dom_node_idx = LAYOUT_PSEUDO_DOM0 + k + 1;
                E->parent_idx = par;
                E->first_child_idx = pt;
                E->child_count = 1;
                T->parent_idx = pe;
                T->first_child_idx = T->next_sibling_idx = T->prev_sibling_idx = -1;
                T->child_count = 0;
                E->prev_sibling_idx = TN->prev_sibling_idx;
                E->next_sibling_idx = t;
                if (TN->prev_sibling_idx >= 0) ctx->tree.nodes[TN->prev_sibling_idx].next_sibling_idx = pe;
                else ctx->tree.nodes[par].first_child_idx = pe;
                TN->prev_sibling_idx = pe;
                ctx->tree.nodes[par].child_count++;
                for (int x = pe; x <= pt; x++) {
                    ctx->slot[x] = layout_slot_take(ctx);
                    memset(&ctx->boxes[x], 0, sizeof(LayoutBox));
                    ctx->boxes[x].s = ctx->slot[x];
                    ctx->custom_props[x] = ctx->custom_props[par];
                }
                ctx->tree.count = pt + 1;
                layout_pseudo_style(ctx, pe, par, ps, em_defer);
                /* (inline, or a float) */
                if (ctx->boxes[pe].s->float_side == 0) {
                    ctx->boxes[pe].s->display = CSS_DISPLAY_INLINE;
                    ctx->boxes[pe].s->inline_level = 0;
                }
                ctx->boxes[pe].s->position = CSS_POSITION_STATIC;
                layout_box_init(ctx, pt);
                style_finish_box_em(em_defer, &ctx->boxes[pt], &ctx->boxes[pe], false);
                k += 2;
                continue;
            }
            HtmlNode *he = &hn[k], *ht = &hn[k + 1];
            bool dyn = strstr(ps->content, "counter") || strstr(ps->content, "quote");
            pairs[k / 2] = (LayoutPseudoPair){ owner, (unsigned char)side, dyn, dyn, ps->n, ps->decls, ps->content };
            /* The last layout's box, when what it depends on is as it was:
             * its element's style slot left as it was (its attributes too,
             * which attr() reads) and the same rules. */
            int j = ptab && !dyn && ctx->style_left[owner] ? layout_parked_find(ctx, ptab, ptab_cap, ctx->ids[owner], side) : -1;
            LayoutParkedPseudo *pk = j >= 0 ? &ctx->parked[j] : NULL;
            if (pk && (pk->used || pk->content != ps->content || pk->n != ps->n ||
                       (ps->n && memcmp(pk->decls, ps->decls, (size_t)ps->n * sizeof(*ps->decls)) != 0)))
                pk = NULL;
            if (pk) {
                pk->used = 1;
                *he = pk->e;
                *ht = pk->t;
            } else {
                char text[1024];
                layout_pseudo_text(ps->content, on, text, sizeof(text));
                he->array_node.parent = he->array_node.first_child = he->array_node.next_sibling = he->array_node.prev_sibling = -1;
                ht->array_node = he->array_node;
                he->type = HTML_NODE_ELEMENT;
                he->tag_name = side ? "::after" : "::before";
                ht->type = HTML_NODE_TEXT;
                ht->tag_name = "";
                ht->text_content = strdup(text);
                ht->text_len = ht->text_content ? strlen(ht->text_content) : 0;
            }
            LayoutNodeRef *E = &ctx->tree.nodes[pe], *T = &ctx->tree.nodes[pt], *O = &ctx->tree.nodes[owner];
            E->dom_node_idx = LAYOUT_PSEUDO_DOM0 + k;
            T->dom_node_idx = LAYOUT_PSEUDO_DOM0 + k + 1;
            E->parent_idx = owner;
            E->first_child_idx = pt;
            E->child_count = 1;
            T->parent_idx = pe;
            T->first_child_idx = T->next_sibling_idx = T->prev_sibling_idx = -1;
            T->child_count = 0;
            if (side == 0) {
                E->prev_sibling_idx = -1;
                E->next_sibling_idx = O->first_child_idx;
                if (O->first_child_idx >= 0) ctx->tree.nodes[O->first_child_idx].prev_sibling_idx = pe;
                O->first_child_idx = pe;
            } else {
                int last = -1;
                for (int c = O->first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) last = c;
                E->prev_sibling_idx = last;
                E->next_sibling_idx = -1;
                if (last < 0) O->first_child_idx = pe;
                else ctx->tree.nodes[last].next_sibling_idx = pe;
            }
            O->child_count++;
            for (int x = pe; x <= pt; x++) {
                ctx->slot[x] = pk ? (x == pe ? pk->slot_e : pk->slot_t) : layout_slot_take(ctx);
                memset(&ctx->boxes[x], 0, sizeof(LayoutBox));
                ctx->boxes[x].s = ctx->slot[x];
                ctx->custom_props[x] = ctx->custom_props[owner];
            }
            ctx->tree.count = pt + 1;   /* (layout_box_init reads the node) */
            if (pk) {
                /* Its styles as they were.  The flow pass may reuse its
                 * layout where it can find its record: the same index, or
                 * (a tree built again) what flow_old maps it to. */
                reused++;
                if (ctx->flow_old) {
                    ctx->flow_old[pe] = pk->old_pe;
                    ctx->flow_old[pt] = pk->old_pe + 1;
                    ctx->style_left[pe] = ctx->style_left[pt] = 1;
                } else {
                    ctx->style_left[pe] = ctx->style_left[pt] = pe == pk->old_pe;
                }
            } else {
                layout_pseudo_style(ctx, pe, owner, ps, em_defer);
                layout_box_init(ctx, pt);
                style_finish_box_em(em_defer, &ctx->boxes[pt], &ctx->boxes[pe], false);
            }
            k += 2;
        }
    }
    /* (the first :has() target is a preorder position: the same box's in the
     * new order, and "none" stays past the end) */
    int target = ctx->has_target_first >= 0 && ctx->has_target_first < base && ctx->tree.preorder
                 ? ctx->tree.preorder[ctx->has_target_first] : -1;
    free(ptab);
    layout_parked_release(ctx);
    n = base + k;
    ctx->pseudo_nodes_n = k;
    if (g_css_log > 0) fprintf(stderr, "[css] pseudo boxes: %d (%d as they were)\n", m, reused);
    ctx->tree.count = n;
    layout_rebuild_orders(ctx);
    ctx->has_target_first = n;
    for (int k = 0; target >= 0 && ctx->tree.preorder && k < n; k++)
        if (ctx->tree.preorder[k] == target) { ctx->has_target_first = k; break; }
}

/* ---- Counters and quotes (CSS 2.1 sections 12.3, 12.4) ----
 * One walk in document order over the boxes (pseudo boxes included; not
 * into display:none) with the counters in scope on a stack: a reset makes
 * a counter for the element, its descendants and its following siblings
 * (a sibling's reset of the same name replacing it), set and increment
 * change the innermost (made at the element, at 0, when there is none).
 * The text of pseudo boxes whose content has counter() / counters() /
 * quotes is made here, quotes by their nesting across the document. */
typedef struct { char name[64]; int value, level; } LayoutCounter;
typedef struct { LayoutCounter *v; int n, cap; int quote_depth; } LayoutCounters;

static LayoutCounter *ctr_find(LayoutCounters *c, const char *name)
{
    for (int i = c->n - 1; i >= 0; i--)
        if (!strcmp(c->v[i].name, name)) return &c->v[i];
    return NULL;
}

static LayoutCounter *ctr_push(LayoutCounters *c, const char *name, int value, int level)
{
    for (int i = c->n - 1; i >= 0 && c->v[i].level == level; i--)
        if (!strcmp(c->v[i].name, name)) { c->v[i].value = value; return &c->v[i]; }
    if (c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 32;
        LayoutCounter *nv = (LayoutCounter *)realloc(c->v, (size_t)nc * sizeof(*nv));
        if (!nv) return NULL;
        c->v = nv;
        c->cap = nc;
    }
    LayoutCounter *e = &c->v[c->n++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->value = value;
    e->level = level;
    return e;
}

/* Apply a counter list (kind 0 reset, 1 set, 2 increment). */
static void ctr_apply(LayoutCounters *c, const char *list, int kind, int level)
{
    const char *p = list;
    while (p && *p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char name[64];
        size_t nl = 0;
        while (*p && !isspace((unsigned char)*p) && nl + 1 < sizeof(name)) name[nl++] = *p++;
        while (*p && !isspace((unsigned char)*p)) p++;
        name[nl] = 0;
        while (isspace((unsigned char)*p)) p++;
        int num = kind == 2 ? 1 : 0;
        if (*p == '+' || *p == '-' || isdigit((unsigned char)*p)) {
            char *end = NULL;
            long l = strtol(p, &end, 10);
            num = (int)(l > 1000000000L ? 1000000000L : l < -1000000000L ? -1000000000L : l);
            p = end;
        }
        if (kind == 0) { ctr_push(c, name, num, level); continue; }
        LayoutCounter *e = ctr_find(c, name);
        if (!e) e = ctr_push(c, name, 0, level);
        if (!e) continue;
        if (kind == 1) e->value = num;
        else e->value += num;
    }
}

/* A counter value in a counter style (CSS 2.1 list-style-type values). */
static void ctr_format(int v, const char *style, char *out, size_t cap)
{
    out[0] = 0;
    if (!style || !*style || !strcasecmp(style, "decimal")) { snprintf(out, cap, "%d", v); return; }
    if (!strcasecmp(style, "none")) return;
    if (!strcasecmp(style, "disc")) { snprintf(out, cap, "\xE2\x80\xA2"); return; }
    if (!strcasecmp(style, "circle")) { snprintf(out, cap, "\xE2\x97\xA6"); return; }
    if (!strcasecmp(style, "square")) { snprintf(out, cap, "\xE2\x96\xAA"); return; }
    if (!strcasecmp(style, "decimal-leading-zero")) {
        if (v >= 0 && v < 10) snprintf(out, cap, "0%d", v);
        else if (v < 0 && v > -10) snprintf(out, cap, "-0%d", -v);
        else snprintf(out, cap, "%d", v);
        return;
    }
    bool upper = !strncasecmp(style, "upper-", 6);
    if (!strcasecmp(style, "lower-roman") || !strcasecmp(style, "upper-roman")) {
        if (v < 1 || v > 3999) { snprintf(out, cap, "%d", v); return; }
        static const int val[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
        static const char *const sym[] = { "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i" };
        size_t o = 0;
        for (int i = 0; i < 13; i++)
            while (v >= val[i] && o + 3 < cap) {
                for (const char *q = sym[i]; *q; q++) out[o++] = upper ? (char)toupper((unsigned char)*q) : *q;
                v -= val[i];
            }
        out[o] = 0;
        return;
    }
    if (!strcasecmp(style, "lower-alpha") || !strcasecmp(style, "lower-latin") ||
        !strcasecmp(style, "upper-alpha") || !strcasecmp(style, "upper-latin")) {
        if (v < 1) { snprintf(out, cap, "%d", v); return; }
        char tmp[16];
        int t = 0;
        while (v > 0 && t < 15) { v--; tmp[t++] = (char)((upper ? 'A' : 'a') + v % 26); v /= 26; }
        size_t o = 0;
        while (t > 0 && o + 1 < cap) out[o++] = tmp[--t];
        out[o] = 0;
        return;
    }
    if (!strcasecmp(style, "lower-greek")) {
        static const char *const g[] = { "α", "β", "γ", "δ", "ε", "ζ", "η", "θ", "ι", "κ", "λ", "μ",
                                         "ν", "ξ", "ο", "π", "ρ", "σ", "τ", "υ", "φ", "χ", "ψ", "ω" };
        if (v < 1) { snprintf(out, cap, "%d", v); return; }
        int digits[16], t = 0;
        while (v > 0 && t < 16) { v--; digits[t++] = v % 24; v /= 24; }
        size_t o = 0;
        while (t > 0) {
            const char *q = g[digits[--t]];
            size_t l = strlen(q);
            if (o + l >= cap) break;
            memcpy(out + o, q, l);
            o += l;
        }
        out[o] = 0;
        return;
    }
    /* Additive systems. */
    static const struct { int v; const char *s; } armenian[] = {
        { 9000, "Ք" }, { 8000, "Փ" }, { 7000, "Ւ" }, { 6000, "Ց" }, { 5000, "Ր" }, { 4000, "Տ" }, { 3000, "Վ" },
        { 2000, "Ս" }, { 1000, "Ռ" }, { 900, "Ջ" }, { 800, "Պ" }, { 700, "Չ" }, { 600, "Ո" }, { 500, "Շ" },
        { 400, "Ն" }, { 300, "Յ" }, { 200, "Մ" }, { 100, "Ճ" }, { 90, "Ղ" }, { 80, "Ձ" }, { 70, "Հ" },
        { 60, "Կ" }, { 50, "Ծ" }, { 40, "Խ" }, { 30, "Լ" }, { 20, "Ի" }, { 10, "Ժ" }, { 9, "Թ" }, { 8, "Ը" },
        { 7, "Է" }, { 6, "Զ" }, { 5, "Ե" }, { 4, "Դ" }, { 3, "Գ" }, { 2, "Բ" }, { 1, "Ա" }, { 0, NULL }
    };
    static const struct { int v; const char *s; } georgian[] = {
        { 10000, "ჵ" }, { 9000, "ჰ" }, { 8000, "ჯ" }, { 7000, "ჴ" }, { 6000, "ხ" }, { 5000, "ჭ" }, { 4000, "წ" },
        { 3000, "ძ" }, { 2000, "ც" }, { 1000, "ჩ" }, { 900, "შ" }, { 800, "ყ" }, { 700, "ღ" }, { 600, "ქ" },
        { 500, "ფ" }, { 400, "ჳ" }, { 300, "ტ" }, { 200, "ს" }, { 100, "რ" }, { 90, "ჟ" }, { 80, "პ" },
        { 70, "ო" }, { 60, "ჲ" }, { 50, "ნ" }, { 40, "მ" }, { 30, "ლ" }, { 20, "კ" }, { 10, "ი" }, { 9, "თ" },
        { 8, "ჱ" }, { 7, "ზ" }, { 6, "ვ" }, { 5, "ე" }, { 4, "დ" }, { 3, "გ" }, { 2, "ბ" }, { 1, "ა" }, { 0, NULL }
    };
    bool arm = !strcasecmp(style, "armenian") || !strcasecmp(style, "upper-armenian");
    bool geo = !strcasecmp(style, "georgian");
    if ((arm && v >= 1 && v <= 9999) || (geo && v >= 1 && v <= 19999)) {
        size_t o = 0;
        for (int i = 0; arm ? armenian[i].s != NULL : georgian[i].s != NULL; i++) {
            int w = arm ? armenian[i].v : georgian[i].v;
            const char *q = arm ? armenian[i].s : georgian[i].s;
            while (v >= w) {
                size_t l = strlen(q);
                if (o + l >= cap) break;
                memcpy(out + o, q, l);
                o += l;
                v -= w;
            }
        }
        out[o] = 0;
        return;
    }
    snprintf(out, cap, "%d", v);
}

/* The i-th string of a quotes value (0 open 1 close of pair p), else the
 * UA's (curly double, then single). */
static bool ctr_quote(const char *quotes, int pair, int close, char *out, size_t cap)
{
    out[0] = 0;
    if (!quotes) {
        static const char *const ua[] = { "\xE2\x80\x9C", "\xE2\x80\x9D", "\xE2\x80\x98", "\xE2\x80\x99" };
        int p = pair > 1 ? 1 : pair;
        snprintf(out, cap, "%s", ua[p * 2 + close]);
        return true;
    }
    if (!*quotes) return false;   /* (none) */
    /* The strings, in order: the last pair serves deeper levels. */
    char strs[16][64];
    int ns = 0;
    const char *v = quotes;
    while (*v && ns < 16) {
        while (isspace((unsigned char)*v)) v++;
        if (*v != '"' && *v != '\'') break;
        char q = *v;
        size_t tl = 1;
        while (v[tl] && v[tl] != q) tl += v[tl] == '\\' && v[tl + 1] ? 2 : 1;
        if (v[tl] == q) tl++;
        char tok[256];
        if (tl >= sizeof(tok)) break;
        memcpy(tok, v, tl);
        tok[tl] = 0;
        bool empty = false;
        if (!css_content_decode_string(tok, strs[ns], sizeof(strs[ns]), &empty)) strs[ns][0] = 0;
        ns++;
        v += tl;
    }
    if (ns < 2) return false;
    int np = ns / 2, p = pair < np ? pair : np - 1;
    snprintf(out, cap, "%s", strs[p * 2 + close]);
    return true;
}

/* A content value's text with counters and quotes (cs). */
static void layout_content_text(const char *v, const HtmlNode *owner, const char *quotes, LayoutCounters *cs,
                                int level, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = 0;
    while (*v) {
        while (isspace((unsigned char)*v)) v++;
        if (!*v) break;
        char piece[1024];
        piece[0] = 0;
        size_t tl = 0;
        if (*v == '"' || *v == '\'') {
            char q = *v;
            tl = 1;
            while (v[tl] && v[tl] != q) tl += v[tl] == '\\' && v[tl + 1] ? 2 : 1;
            if (v[tl] == q) tl++;
            char tok[1024];
            if (tl < sizeof(tok)) {
                memcpy(tok, v, tl);
                tok[tl] = 0;
                bool empty = false;
                if (!css_content_decode_string(tok, piece, sizeof(piece), &empty)) piece[0] = 0;
            }
        } else {
            int depth = 0;
            while (v[tl] && (depth > 0 || !isspace((unsigned char)v[tl]))) {
                if (v[tl] == '(') depth++;
                else if (v[tl] == ')' && depth > 0) depth--;
                tl++;
            }
            bool counters = !strncasecmp(v, "counters(", 9);
            if (counters || !strncasecmp(v, "counter(", 8)) {
                /* name [, separator] [, style] */
                const char *a = v + (counters ? 9 : 8), *end = v + tl;
                char args[3][256];
                int na = 0;
                while (a < end && na < 3) {
                    while (a < end && isspace((unsigned char)*a)) a++;
                    const char *b = a;
                    if (*a == '"' || *a == '\'') {
                        char q = *a++;
                        while (a < end && *a != q) a += *a == '\\' && a + 1 < end ? 2 : 1;
                        if (a < end) a++;
                    } else {
                        while (a < end && *a != ',' && *a != ')') a++;
                    }
                    size_t l = (size_t)(a - b);
                    while (l && isspace((unsigned char)b[l - 1])) l--;
                    if (l >= sizeof(args[na])) l = sizeof(args[na]) - 1;
                    memcpy(args[na], b, l);
                    args[na][l] = 0;
                    na++;
                    while (a < end && *a != ',' && *a != ')') a++;
                    if (a < end && *a == ',') a++;
                    else break;
                }
                const char *name = na > 0 ? args[0] : "";
                char sep[256] = "";
                const char *style = NULL;
                if (counters && na > 1) {
                    bool empty = false;
                    if (!css_content_decode_string(args[1], sep, sizeof(sep), &empty)) sep[0] = 0;
                    if (na > 2) style = args[2];
                } else if (!counters && na > 1) {
                    style = args[1];
                }
                if (cs && !ctr_find(cs, name)) ctr_push(cs, name, 0, level);
                size_t po = 0;
                for (int i = 0; cs && i < cs->n; i++) {
                    if (strcmp(cs->v[i].name, name)) continue;
                    if (!counters) {
                        /* (only the innermost) */
                        LayoutCounter *e = ctr_find(cs, name);
                        ctr_format(e->value, style, piece, sizeof(piece));
                        po = strlen(piece);
                        break;
                    }
                    char num[128];
                    ctr_format(cs->v[i].value, style, num, sizeof(num));
                    int w = snprintf(piece + po, sizeof(piece) - po, "%s%s", po ? sep : "", num);
                    if (w > 0) po += (size_t)w;
                    if (po >= sizeof(piece)) { po = sizeof(piece) - 1; break; }
                }
                piece[po] = 0;
            } else if (!strncasecmp(v, "attr(", 5)) {
                char name[128];
                size_t nl = 0;
                const char *a = v + 5;
                while (isspace((unsigned char)*a)) a++;
                while (*a && *a != ')' && !isspace((unsigned char)*a) && nl + 1 < sizeof(name)) name[nl++] = *a++;
                name[nl] = 0;
                bool html = !owner->store_id || dom_is_html_document(dom_owner(owner->store_id));
                for (const HtmlAttribute *at = owner->attributes; at; at = at->next)
                    if (at->name && (html ? !strcasecmp(at->name, name) : !strcmp(at->name, name))) {
                        snprintf(piece, sizeof(piece), "%s", at->value ? at->value : "");
                        break;
                    }
            } else if (tl == 10 && !strncasecmp(v, "open-quote", 10)) {
                if (cs) {
                    ctr_quote(quotes, cs->quote_depth, 0, piece, sizeof(piece));
                    cs->quote_depth++;
                } else {
                    ctr_quote(quotes, 0, 0, piece, sizeof(piece));
                }
            } else if (tl == 11 && !strncasecmp(v, "close-quote", 11)) {
                if (cs) {
                    if (cs->quote_depth > 0) {
                        cs->quote_depth--;
                        ctr_quote(quotes, cs->quote_depth, 1, piece, sizeof(piece));
                    }
                } else {
                    ctr_quote(quotes, 0, 1, piece, sizeof(piece));
                }
            } else if (tl == 13 && !strncasecmp(v, "no-open-quote", 13)) {
                if (cs) cs->quote_depth++;
            } else if (tl == 14 && !strncasecmp(v, "no-close-quote", 14)) {
                if (cs && cs->quote_depth > 0) cs->quote_depth--;
            }
        }
        size_t pl = strlen(piece);
        if (o + pl >= cap) pl = cap - 1 - o;
        memcpy(out + o, piece, pl);
        o += pl;
        out[o] = 0;
        v += tl ? tl : 1;
    }
}

static void layout_counters_walk(LayoutContext *ctx, int node, int level, LayoutCounters *cs, int depth)
{
    if (depth > 400) return;
    int base = ctx->base_count;
    for (int c = ctx->tree.nodes[node].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        const LayoutBox *b = &ctx->boxes[c];
        if (b->s->display == CSS_DISPLAY_NONE) continue;
        if (b->s->counter_reset) ctr_apply(cs, b->s->counter_reset, 0, level);
        if (b->s->counter_set) ctr_apply(cs, b->s->counter_set, 1, level);
        if (b->s->counter_increment) ctr_apply(cs, b->s->counter_increment, 2, level);
        if (b->s->display == CSS_DISPLAY_LIST_ITEM &&
            !(b->s->counter_increment && strstr(b->s->counter_increment, "list-item")) &&
            !(b->s->counter_set && strstr(b->s->counter_set, "list-item")))
            ctr_apply(cs, "list-item", 2, level);
        if (c >= base && base > 0 && ctx->pseudo_pairs && ((c - base) & 1) == 0) {
            const LayoutPseudoPair *pp = &ctx->pseudo_pairs[(c - base) / 2];
            if (pp->dynamic) {
                HtmlNode *tn = &ctx->pseudo_nodes[c - base + 1];
                HtmlNode *on = layout_node_dom(ctx, ctx->tree.nodes[pp->owner].dom_node_idx);
                char text[1024];
                layout_content_text(pp->content, on, b->s->quotes, cs, level + 1, text, sizeof(text));
                char *nt = strdup(text);
                if (nt) {
                    free(tn->text_content);
                    tn->text_content = nt;
                    tn->text_len = strlen(nt);
                }
            }
        }
        layout_counters_walk(ctx, c, level + 1, cs, depth + 1);
    }
    while (cs->n > 0 && cs->v[cs->n - 1].level >= level) cs->n--;
}

/* Counters and quotes over the tree (only when some pseudo box needs them). */
static void layout_counters(LayoutContext *ctx)
{
    int pairs = ctx->pseudo_nodes_n / 2;
    bool any = false;
    for (int j = 0; j < pairs && !any; j++) any = ctx->pseudo_pairs && ctx->pseudo_pairs[j].dynamic;
    if (!any || ctx->tree.root_idx < 0) return;
    LayoutCounters cs = { 0 };
    const LayoutBox *rb = &ctx->boxes[ctx->tree.root_idx];
    if (rb->s->counter_reset) ctr_apply(&cs, rb->s->counter_reset, 0, 0);
    if (rb->s->counter_set) ctr_apply(&cs, rb->s->counter_set, 1, 0);
    if (rb->s->counter_increment) ctr_apply(&cs, rb->s->counter_increment, 2, 0);
    layout_counters_walk(ctx, ctx->tree.root_idx, 1, &cs, 0);
    free(cs.v);
}

/* ---- text-transform ----
 * After the pseudo boxes: each text box's text (a pseudo box's too) with
 * its transform applied, as the layout's own text for it (text_gen).
 * capitalize upper-cases the first letter or digit of each word, a word
 * running on across inline boxes. */
#include "case_tables.h"

static const char *case_map(const CaseMap *t, int n, uint32_t cp)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (t[mid].cp == cp) return t[mid].to;
        if (t[mid].cp < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

static size_t utf8_put(char *o, uint32_t cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* A letter or digit, as far as words go (not white space, punctuation or
 * a symbol in ASCII or Latin-1). */
static bool tt_wordy(uint32_t cp)
{
    if (cp < 0x80) return isalnum((int)cp) != 0;
    if (cp < 0xC0) return cp == 0xAA || cp == 0xB5 || cp == 0xBA;
    if (cp == 0xD7 || cp == 0xF7) return false;
    return !layout_is_punct(cp) && cp != 0x3000 && !(cp >= 0x2000 && cp <= 0x206F);
}

/* s with transform t (1 upper, 2 lower, 3 capitalize; *in_word: capitalize
 * is inside a word), malloc'd. */
static char *layout_text_transform(const char *s, int t, bool *in_word)
{
    size_t n = strlen(s);
    char *out = (char *)malloc(n * 5 + 16);
    if (!out) return NULL;
    size_t o = 0;
    const char *p = s;
    while (*p) {
        const char *at = p;
        uint32_t cp = layout_utf8_next(&p);
        if (p == at) { p++; continue; }
        const char *to = NULL;
        if (t == 1) to = case_map(k_case_upper, (int)(sizeof(k_case_upper) / sizeof(k_case_upper[0])), cp);
        else if (t == 2) to = case_map(k_case_lower, (int)(sizeof(k_case_lower) / sizeof(k_case_lower[0])), cp);
        else if (t == 3) {
            bool wordy = tt_wordy(cp);
            if (wordy && !*in_word) {
                /* (titlecase: the digraphs' own, else the uppercase with
                 * any further letters of it lower case) */
                if (cp >= 0x1C4 && cp <= 0x1CC) { o += utf8_put(out + o, cp <= 0x1C6 ? 0x1C5 : cp <= 0x1C9 ? 0x1C8 : 0x1CB); *in_word = true; continue; }
                if (cp >= 0x1F1 && cp <= 0x1F3) { o += utf8_put(out + o, 0x1F2); *in_word = true; continue; }
                to = case_map(k_case_upper, (int)(sizeof(k_case_upper) / sizeof(k_case_upper[0])), cp);
                if (to && to[0] && (unsigned char)to[0] < 0x80 && to[1]) {
                    out[o++] = to[0];
                    for (const char *q = to + 1; *q; q++) out[o++] = (char)tolower((unsigned char)*q);
                    *in_word = true;
                    continue;
                }
                *in_word = true;
            } else if (!(cp == '\'' || cp == 0x2019)) {
                /* (an apostrophe stays inside its word) */
                *in_word = wordy;
            }
        }
        if (to) {
            size_t l = strlen(to);
            memcpy(out + o, to, l);
            o += l;
        } else {
            memcpy(out + o, at, (size_t)(p - at));
            o += (size_t)(p - at);
        }
    }
    out[o] = 0;
    return out;
}

static void layout_collapse_borders(LayoutContext *ctx);

static void layout_text_transforms(LayoutContext *ctx)
{
    int n = ctx->tree.count;
    bool any = false;
    for (int i = 0; i < n && !any; i++) any = ctx->boxes[i].s->text_transform != 0;
    if (!any || !ctx->tree.preorder) return;
    bool in_word = false, changed = false;
    for (int k = 0; k < n; k++) {
        int i = ctx->tree.preorder[k];
        const LayoutBox *b = &ctx->boxes[i];
        int d = ctx->tree.nodes[i].dom_node_idx;
        HtmlNode *hn = layout_node_dom(ctx, d);
        if (!hn) continue;
        if (hn->type == HTML_NODE_ELEMENT) {
            if (b->s->display != CSS_DISPLAY_INLINE) in_word = false;
            continue;
        }
        if (hn->type != HTML_NODE_TEXT) continue;
        const char *text = css_layout_node_text(ctx, hn);
        if (!text || !text[0]) continue;
        int t = b->s->text_transform;
        if (!t) {
            /* (where the text leaves off, for a capitalized word after it) */
            size_t l = strlen(text);
            const char *q = text + l;
            while (q > text && ((unsigned char)q[-1] & 0xC0) == 0x80) q--;
            if (q > text) q--;
            while (q > text && ((unsigned char)*q & 0xC0) == 0x80) q--;
            const char *qq = q;
            in_word = tt_wordy(layout_utf8_next(&qq));
            continue;
        }
        char *nt = layout_text_transform(text, t, &in_word);
        if (!nt) continue;
        if (strcmp(nt, text) == 0) { free(nt); continue; }
        if (d >= LAYOUT_PSEUDO_DOM0) {
            free(hn->text_content);
            hn->text_content = nt;
            hn->text_len = strlen(nt);
        } else {
            layout_set_node_text(ctx, d, nt, strlen(nt));
            free(nt);
        }
        changed = true;
    }
    if (changed) layout_drop_text_caches(ctx);
}

/* ---- Anonymous table boxes (CSS 2.1 section 17.2.1) ----
 * After the pseudo boxes, top down: a table's (or row group's) run of
 * children that are not rows, row groups, captions or columns goes in an
 * anonymous row; a row's run of non-cells in an anonymous cell; anywhere
 * else a run of table parts goes in an anonymous table (inline-table in an
 * inline).  White space that would collapse, out-of-flow and display:none
 * children pass between a run's members without ending it. */
static const char k_anon_table[] = "::table", k_anon_row[] = "::row", k_anon_cell[] = "::cell";

static bool table_is_table(const LayoutBox *b);
static bool layout_is_table_part(const LayoutBox *b);

/* How a box's children are fixed: 0 not, 1 a table's, 2 a row group's,
 * 3 a row's, 4 any other box's. */
static int anon_parent_kind(const LayoutContext *ctx, int p)
{
    const LayoutBox *b = &ctx->boxes[p];
    if (b->s->display == CSS_DISPLAY_NONE || b->s->skipped || b->s->display == CSS_DISPLAY_FLEX ||
        b->s->display == CSS_DISPLAY_GRID)
        return 0;
    HtmlNode *n = layout_node_dom((LayoutContext *)ctx, ctx->tree.nodes[p].dom_node_idx);
    if (!n || n->type != HTML_NODE_ELEMENT) return 0;
    if (b->s->table_role == 1) return table_is_table(b) || b->s->inline_level ? 1 : 0;
    if (b->s->table_role == 2) return 2;
    if (b->s->table_role == 3) return 3;
    if (b->s->table_role == 6) return 0;   /* (a column group: its columns) */
    return 4;
}

/* Child c for a kind of parent: 0 passes (white space and the like), 1 in
 * a run, 2 ends one. */
static int anon_child_class(LayoutContext *ctx, int c, int kind)
{
    HtmlNode *n = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
    const LayoutBox *b = &ctx->boxes[c];
    if (!n) return 0;
    if (n->type == HTML_NODE_TEXT) {
        if (b->s->white_space == 2 || b->s->white_space == 3) return kind == 4 ? 2 : 1;
        if (layout_text_is_whitespace(css_layout_node_text(ctx, n))) return 0;
        return kind == 4 ? 2 : 1;
    }
    if (n->type != HTML_NODE_ELEMENT) return 0;
    if (b->s->display == CSS_DISPLAY_NONE || b->s->position == CSS_POSITION_ABSOLUTE ||
        b->s->position == CSS_POSITION_FIXED)
        return 0;
    int role = layout_is_table_part(b) ? b->s->table_role : 0;
    if (kind == 1) return role == 2 || role == 3 || role == 5 || role == 6 ? 2 : 1;
    if (kind == 2) return role == 3 || role == 6 ? 2 : 1;
    if (kind == 3) return role == 4 ? 2 : 1;
    return role >= 2 && role <= 6 ? 1 : 2;
}

typedef struct { int cap, anon_cap; bool em_defer; } AnonFix;

/* A new box wrapping children first..last of p (role: the table part it
 * is); -1 when out of memory. */
static int anon_wrap(LayoutContext *ctx, AnonFix *F, int p, int first, int last, int role, bool inline_table)
{
    int w = ctx->tree.count;
    if (w + 1 >= F->cap) {
        int nc = w + w / 4 + 64;
        LayoutNodeRef *nn = (LayoutNodeRef *)realloc(ctx->tree.nodes, (size_t)(nc + 1) * sizeof(LayoutNodeRef));
        if (!nn) return -1;
        ctx->tree.nodes = nn;
        LayoutBox *nb = (LayoutBox *)realloc(ctx->boxes, (size_t)(nc + 1) * sizeof(LayoutBox));
        if (!nb) return -1;
        ctx->boxes = nb;
        if (!layout_grow_box_arrays(ctx, w, nc)) return -1;
        F->cap = nc;
    }
    if (ctx->anon_nodes_n == F->anon_cap) {
        int nc = F->anon_cap ? F->anon_cap * 2 : 16;
        HtmlNode *na = (HtmlNode *)realloc(ctx->anon_nodes, (size_t)nc * sizeof(HtmlNode));
        if (!na) return -1;
        ctx->anon_nodes = na;
        F->anon_cap = nc;
    }
    int a = ctx->anon_nodes_n++;
    HtmlNode *hn = &ctx->anon_nodes[a];
    memset(hn, 0, sizeof(*hn));
    hn->array_node.parent = hn->array_node.first_child = hn->array_node.next_sibling = hn->array_node.prev_sibling = -1;
    hn->type = HTML_NODE_ELEMENT;
    hn->tag_name = role == 1 ? k_anon_table : role == 3 ? k_anon_row : k_anon_cell;
    LayoutNodeRef *N = ctx->tree.nodes, *W = &N[w];
    W->dom_node_idx = LAYOUT_ANON_DOM0 + a;
    W->parent_idx = p;
    W->prev_sibling_idx = N[first].prev_sibling_idx;
    W->next_sibling_idx = N[last].next_sibling_idx;
    if (W->prev_sibling_idx >= 0) N[W->prev_sibling_idx].next_sibling_idx = w;
    else N[p].first_child_idx = w;
    if (W->next_sibling_idx >= 0) N[W->next_sibling_idx].prev_sibling_idx = w;
    W->first_child_idx = first;
    N[first].prev_sibling_idx = -1;
    N[last].next_sibling_idx = -1;
    int cnt = 0;
    for (int c = first; c >= 0; c = N[c].next_sibling_idx) { N[c].parent_idx = w; cnt++; }
    W->child_count = cnt;
    N[p].child_count -= cnt - 1;
    ctx->tree.count = w + 1;
    ctx->slot[w] = layout_slot_take(ctx);
    memset(&ctx->boxes[w], 0, sizeof(LayoutBox));
    ctx->boxes[w].s = ctx->slot[w];
    ctx->custom_props[w] = ctx->custom_props[p];
    layout_box_init(ctx, w);
    LayoutStyle *st = ctx->boxes[w].s;
    st->display = inline_table ? CSS_DISPLAY_BLOCK : CSS_DISPLAY_OTHER;
    st->inline_level = inline_table ? 1 : 0;
    st->table_role = (unsigned char)role;
    style_finish_box_em(F->em_defer, &ctx->boxes[w], &ctx->boxes[p], true);
    return w;
}

static bool anon_fix(LayoutContext *ctx, AnonFix *F, int p, int depth)
{
    if (depth > 400 || ctx->boxes[p].s->display == CSS_DISPLAY_NONE) return true;
    int kind = anon_parent_kind(ctx, p);
    for (int c = kind ? ctx->tree.nodes[p].first_child_idx : -1; c >= 0;) {
        if (anon_child_class(ctx, c, kind) != 1) { c = ctx->tree.nodes[c].next_sibling_idx; continue; }
        int last = c;
        for (int k = ctx->tree.nodes[c].next_sibling_idx; k >= 0; k = ctx->tree.nodes[k].next_sibling_idx) {
            int cl = anon_child_class(ctx, k, kind);
            if (cl == 2) break;
            if (cl == 1) last = k;
        }
        int role = kind == 3 ? 4 : kind == 4 ? 1 : 3;
        bool inl = kind == 4 && ctx->boxes[p].s->display == CSS_DISPLAY_INLINE;
        int w = anon_wrap(ctx, F, p, c, last, role, inl);
        if (w < 0) return false;
        c = ctx->tree.nodes[w].next_sibling_idx;
    }
    for (int c = ctx->tree.nodes[p].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx)
        if (ctx->tree.nodes[c].first_child_idx >= 0 && !anon_fix(ctx, F, c, depth + 1)) return false;
    return true;
}

static void layout_anon_tables(LayoutContext *ctx, bool em_defer)
{
    int n = ctx->tree.count;
    bool any = false;
    for (int i = 0; i < n && !any; i++) any = ctx->boxes[i].s->table_role != 0;
    if (!any || ctx->tree.root_idx < 0) return;
    int target = ctx->has_target_first >= 0 && ctx->has_target_first < n && ctx->tree.preorder
                 ? ctx->tree.preorder[ctx->has_target_first] : -1;
    layout_drop_text_caches(ctx);
    AnonFix F = { n + 1, 0, em_defer };
    anon_fix(ctx, &F, ctx->tree.root_idx, 0);
    if (ctx->tree.count == n) return;
    if (g_css_log > 0) fprintf(stderr, "[css] anonymous table boxes: %d\n", ctx->tree.count - n);
    layout_rebuild_orders(ctx);
    n = ctx->tree.count;
    ctx->has_target_first = n;
    for (int k = 0; target >= 0 && ctx->tree.preorder && k < n; k++)
        if (ctx->tree.preorder[k] == target) { ctx->has_target_first = k; break; }
}

static void layout_apply_stylesheet(LayoutContext *ctx, CssStylesheet *sheet)
{
    STAGE_BEGIN();
    static int memo_env = -1;
    if (memo_env < 0) { const char *e = getenv("CSS_SHEETS_MEMO"); memo_env = !(e && strcmp(e, "0") == 0); }
    uint64_t skey = sheet || !memo_env ? 0 : layout_sheets_key(ctx);
    const CssStyleSet *memo_set = NULL;
    if (skey && skey == g_sheets_memo.key)
        memo_set = styleset_cached(g_sheets_memo.set_gen, ctx->viewport_width, ctx->viewport_height);
    LayoutStyleSheetList list = {0};
    if (!memo_set) {
        layout_collect_document_stylesheets(ctx, &list, ctx->base_url);
        if (sheet) layout_sheet_list_add(&list, sheet);
    }
    STAGE("collect sheets");
    if (g_css_log > 1) {
        size_t bytes = 0;
        for (int i = 0; i < list.nitems; i++) bytes += list.items[i].len;
        fprintf(stderr, "[css] sheets: %d items, %zu bytes%s\n", list.nitems, bytes, memo_set ? " (as before)" : "");
    }
    if (!memo_set) layout_sheet_list_resolve(&list);
    STAGE("parse sheets (cached)");

    StylePass P;
    memset(&P, 0, sizeof(P));
    P.ctx = ctx;
    P.em_defer = cyber_getenv("CYBER_DISABLE_EM_DEFER") == NULL;
    const char *genv = getenv("CSS_GRAIN");
    P.grain = (genv && atoi(genv) > 0) ? atoi(genv) : 128;
    if (ctx->serial) P.grain = INT32_MAX;
    bool set_owned = false;
    uint64_t set_gen = 0;
    if (memo_set) {
        P.set = memo_set;
        set_gen = g_sheets_memo.set_gen;
        list.count = g_sheets_memo.nsheets;
    } else {
        P.set = layout_styleset_get(&list, ctx->viewport_width, ctx->viewport_height, &set_owned, &set_gen);
        g_sheets_memo.key = 0;
        if (skey && set_gen && !set_owned && !list.incomplete) {
            g_sheets_memo.key = skey;
            g_sheets_memo.set_gen = set_gen;
            g_sheets_memo.vw = ctx->viewport_width;
            g_sheets_memo.vh = ctx->viewport_height;
            g_sheets_memo.nsheets = list.count;
        }
    }
    ctx->style_gen = set_gen;
    STAGE("compile style set (cached)");
    P.sd = css_styledoc_get(ctx->doc);
    if (g_css_log > 1) {
        CssStyleDocStats ss;
        css_styledoc_stats(&ss);
        fprintf(stderr, "[css] features: %llu reused, %llu updated (%llu elements), %llu built (%llu elements copied)\n",
                (unsigned long long)ss.reused, (unsigned long long)ss.updated,
                (unsigned long long)ss.elements_redone, (unsigned long long)ss.built,
                (unsigned long long)ss.elements_copied);
    }
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

    /* Incremental restyle against the previous pass (CSS_INC=0: off). */
    uint64_t sc_global = 0;
    {
        static int inc_env = -1, verify_env = -1;
        if (inc_env < 0) { const char *e = getenv("CSS_INC"); inc_env = !(e && strcmp(e, "0") == 0); }
        if (verify_env < 0) verify_env = cyber_getenv("CYBER_STYLE_VERIFY") ? 1 : 0;
        if (inc_env && set_gen) {
            sc_global = 1469598103934665603ull;
            sc_global = style_mix(sc_global, &set_gen, sizeof(set_gen));
            sc_global = style_mix(sc_global, &ctx->viewport_width, sizeof(ctx->viewport_width));
            sc_global = style_mix(sc_global, &ctx->viewport_height, sizeof(ctx->viewport_height));
            sc_global = style_mix(sc_global, ctx->base_url, strlen(ctx->base_url));
            sc_global = style_mix(sc_global, &P.em_defer, sizeof(P.em_defer));
            if (sc_global == 0) sc_global = 1;
            static StyleCtxFeatures cf;
            mentions_build(P.set, set_gen);
            if (g_has_gen != set_gen) {
                /* An extension of the set these describe: scan its new
                 * rules only. */
                uint64_t base_gen = 0;
                int first = 0;
                if (!(g_has_gen && styleset_base_of(set_gen, &base_gen, &first) && base_gen == g_has_gen)) {
                    has_free(&g_has);
                    ctx_features_free(&cf);
                    first = 0;
                }
                for (int r = first, nr = css_styleset_rule_count(P.set); r < nr; r++) {
                    const CssRule *rule = css_styleset_rule(P.set, r);
                    if (!rule || !rule->selector_text) continue;
                    has_scan_selector(&g_has, rule->selector_text);
                    ctx_scan_list(&cf, rule->selector_text, rule->selector_text + strlen(rule->selector_text), false);
                }
                if (g_css_log > 0)
                    fprintf(stderr, "[css] context features: %u keys%s%s%s%s%s%s%s%s%s\n", cf.keys.n,
                            cf.all ? ", all attributes" : "", cf.all_tags ? ", all tags" : "",
                            cf.sibling ? ", sibling combinators" : "", cf.s_pos ? ", position" : "",
                            cf.s_first ? ", first" : "", cf.s_prev ? ", previous" : "", cf.s_last ? ", last" : "",
                            cf.s_fromend ? ", from end" : "", cf.s_empty ? ", empty" : "");
                g_has_gen = set_gen;
                g_has.gen = set_gen;
                if (g_css_log > 0 && g_has.any)
                    fprintf(stderr, "[css] :has() subjects: %d keys, %d ancestor keys%s%s%s\n", g_has.self.n, g_has.anc.n,
                            g_has.universal ? ", universal" : "", g_has.universal_deep ? " (deep)" : "",
                            g_has.sibling_relative ? ", sibling-relative" : "");
            }
            P.inc = true;
            P.verify = verify_env;
            /* A kept tree whose notes are current (no other store moved the
             * entries since), and an arena not grown too large: start from
             * them.  CYBER_STYLE_RETAIN=0: restyle kept trees in full. */
            static int retain_env = -1;
            if (retain_env < 0) {
                const char *e = cyber_getenv("CYBER_STYLE_RETAIN");
                retain_env = !(e && e[0] == '0');
            }
            P.retain = retain_env && ctx->tree_kept && !verify_env && !ctx->style_all && ctx->sc_entry &&
                       ctx->dom_map && g_sc.order && ctx->sc_order == g_sc.order && ctx->sc_global == sc_global &&
                       ctx->arena.bytes < ((size_t)16 << 20);
            if (P.retain) {
                P.kept = (unsigned char *)calloc((size_t)n + 1, 1);
                if (!P.kept) P.retain = false;
            }
            has_bits_ready(ctx, &g_has);
            style_cache_prepare(&P, sc_global, &g_has, &cf);
            /* The first element a :has() rule whose compound names nothing
             * restyles (layout_api's anchors read it). */
            ctx->has_target_first = n;
            if (g_has.any && (g_has.universal_deep || g_has.deep_targets.n)) {
                int k = 0;
                for (; !g_has.universal_deep && k < n; k++)
                    if (has_memo_at(ctx, &g_has, ctx->tree.preorder[k], 2)) break;
                ctx->has_target_first = k;
            }
            /* And which elements a :has() rule may restyle as its subject
             * (has_may_match's test: its own features, or an ancestor's). */
            free(ctx->has_flags);
            ctx->has_flags = NULL;
            if (!g_has.universal_self && !g_has.universal_deep)
                ctx->has_flags = (unsigned char *)calloc((size_t)n + 1, 1);
            for (int k = 0; ctx->has_flags && g_has.any && k < n; k++) {
                int idx = ctx->tree.preorder[k];
                int p = ctx->tree.nodes[idx].parent_idx;
                if (!has_is_element(ctx, idx)) continue;
                /* Bit 2: an ancestor carries an ancestor-compound feature. */
                unsigned char below = p >= 0 ? (unsigned char)(ctx->has_flags[p] & 2) : 0;
                if (p >= 0 && g_has.anc_req.n && has_memo_at(ctx, &g_has, p, 4)) below = 2;
                bool subject = below || (g_has.self_req.n && has_memo_at(ctx, &g_has, idx, 3));
                ctx->has_flags[idx] = (unsigned char)(below | (subject ? LAYOUT_HAS_SUBJECT : 0));
            }
            STAGE("restyle prepare");
        } else if (verify_env) {
            fprintf(stderr, "[css] restyle: full (%s)\n", !inc_env ? "CSS_INC=0" : "style set not cached");
        }
    }
    /* A kept tree restyled in full starts its slots over. */
    if (ctx->tree_kept && !P.retain) layout_restyle_all(ctx);
    /* CYBER_STYLE_HIDDEN=1: style display:none subtrees as well. */
    {
        static int hidden_env = -1;
        if (hidden_env < 0) hidden_env = cyber_getenv("CYBER_STYLE_HIDDEN") ? 1 : 0;
        P.skip_hidden = !ctx->style_all && !hidden_env && !P.verify;
        if (P.skip_hidden) P.skipped = (unsigned char *)calloc((size_t)n + 1, 1);
        if (!P.skipped) P.skip_hidden = false;
    }
    if (P.inc && !P.verify) style_count_dirty(&P);
    {
        static int share_text = -1, check_text = -1;
        if (share_text < 0) share_text = cyber_getenv("CYBER_TEXT_STYLE_SHARE") ? atoi(cyber_getenv("CYBER_TEXT_STYLE_SHARE")) != 0 : 1;
        if (check_text < 0) check_text = cyber_getenv("CYBER_TEXT_STYLE_CHECK") ? 1 : 0;
        if (share_text)
            P.text_styles = (_Atomic(LayoutStyle *) *)calloc((size_t)n + 1, sizeof(*P.text_styles));
        P.text_style_check = check_text != 0;
    }
    g_restyle_reused = g_restyle_recomputed = g_restyle_matched_same = 0;
    free(ctx->pseudo_rec);
    ctx->pseudo_rec = (const void **)calloc((size_t)n + 1, sizeof(void *));
    if (!ctx->pseudo_rec) return;
    job_graph_run(job_new(style_root_job, NULL, &P, 0, 0));
    ctx->style_skipped = (int)atomic_load(&P.skipped_n);
    if (!ctx->style_ok) ctx->style_ok = (unsigned char *)malloc((size_t)n + 1);
    if (ctx->style_ok) memset(ctx->style_ok, 1, (size_t)n + 1);
    if (P.text_style_check && atomic_load(&P.text_style_mismatches))
        fprintf(stderr, "[text-style-check] %u text boxes differ from their shared style\n",
                (unsigned)atomic_load(&P.text_style_mismatches));
    free((void *)P.text_styles);
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
    /* The cache keeps the boxes as the cascade left them (before generated
     * content and blockification below). */
    /* (a pass that stores nothing leaves the notes behind) */
    if (!P.inc || !P.entry_key || ctx->cache_readonly) ctx->sc_order = 0;
    if (P.inc && P.entry_key) {
        if (!ctx->cache_readonly) style_cache_store(&P, sc_global, gen, gen_total);
        g_restyle_reused = atomic_load(&P.reused);
        g_restyle_recomputed = atomic_load(&P.recomputed);
        g_restyle_matched_same = atomic_load(&P.matched_same);
        STAGE("restyle store");
        if (g_css_log > 1)
            fprintf(stderr, "[css] restyle why: noentry %u key %u forced %u has %u parent %u\n",
                    (unsigned)P.why[0], (unsigned)P.why[1], (unsigned)P.why[2], (unsigned)P.why[3], (unsigned)P.why[4]);
        if (g_css_log > 0 && P.retain)
            fprintf(stderr, "[css] restyle on the kept tree: %u style slots left as they were\n", (unsigned)atomic_load(&P.kept_n));
        if (g_css_log > 0 || P.verify)
            fprintf(stderr, "[css] restyle: %u reused (%u copied, %d to restyle), %u recomputed (%u matched as before), %u not styled (hidden)%s",
                    (unsigned)atomic_load(&P.reused), (unsigned)atomic_load(&P.copied),
                    P.dirty_n && n ? P.dirty_n[ctx->tree.root_idx] : -1, (unsigned)atomic_load(&P.recomputed),
                    (unsigned)atomic_load(&P.matched_same), (unsigned)atomic_load(&P.skipped_n),
                    P.verify ? "" : "\n");
        if (P.verify)
            fprintf(stderr, ", %u reusable, %u reuse mismatches\n", (unsigned)atomic_load(&P.verify_reusable),
                    (unsigned)atomic_load(&P.verify_mismatches));
    } else if (!P.inc) {
        g_sc.global = 0;
    }
    free(P.dirty_n);
    free(P.skipped);
    /* (which styles it left as they were: the flow pass reuses what it can
     * of their boxes) */
    free(ctx->style_left);
    ctx->style_left = P.retain ? P.kept : NULL;
    if (!P.retain) free(P.kept);
    free(P.cached);
    free(P.key_ok);
    free(P.force);
    free(P.out_changed);
    free(P.match_hash);
    free(P.entry_key);
    free(P.cx.items);
    free(P.cx_off);
    free(P.cx_cnt);
    free(P.cx_struct);
    free(gen);
    /* The ::before / ::after boxes the styles call for, then (they are
     * children like any other) blockification. */
    STAGE("merge");
    layout_pseudo_attach(ctx, P.em_defer);
    layout_counters(ctx);
    layout_text_transforms(ctx);
    layout_anon_tables(ctx, P.em_defer);
    layout_collapse_borders(ctx);
    STAGE("pseudo boxes");
    layout_blockify(ctx);
    STAGE("blockify");

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
    if (set_owned) css_styleset_free((CssStyleSet *)P.set);
    layout_sheet_list_free(&list);
    STAGE("free");
}

/* ============================================================================
 * Flex layout support
 * ============================================================================ */

static double layout_clamp_size(double value, double min_v, double max_v)
{
    /* (the min wins over a smaller max, CSS 2.1 §10.4) */
    if (max_v >= 0 && value > max_v) value = max_v;
    if (min_v > 0 && value < min_v) value = min_v;
    return value;
}

static void layout_update_content_sizes(LayoutBox *box)
{
    box->content_width = box->width - box->padding_left - box->padding_right
                         - box->s->border_left - box->s->border_right;
    if (box->content_width < 0) box->content_width = 0;
    if (box->height > 0) {
        box->content_height = box->height - box->padding_top - box->padding_bottom
                              - box->s->border_top - box->s->border_bottom;
        if (box->content_height < 0) box->content_height = 0;
    }
}

static double layout_horizontal_border_padding(const LayoutBox *box)
{
    return box->padding_left + box->padding_right +
           box->s->border_left + box->s->border_right;
}

static double layout_vertical_border_padding(const LayoutBox *box)
{
    return box->padding_top + box->padding_bottom +
           box->s->border_top + box->s->border_bottom;
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
    if (box->s->box_sizing == CSS_BOX_SIZING_CONTENT_BOX) {
        double inset = is_row ? layout_horizontal_border_padding(box)
                              : layout_vertical_border_padding(box);
        return value + inset;
    }
    return value;
}

static double layout_flex_cross_total(const LayoutBox *box, double value, bool is_row)
{
    if (value < 0.0) return value;
    if (box->s->box_sizing == CSS_BOX_SIZING_CONTENT_BOX) {
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

static void layout_svg_size_in(const LayoutBox *b, HtmlNode *dom, double pcw, double pch,
                               double *out_w, double *out_h);
/* Replaced elements layout_svg_size sizes: <svg>, <canvas>, <iframe>. */
static bool layout_is_svg_box(const HtmlNode *node)
{
    return node && node->type == HTML_NODE_ELEMENT && node->tag_name &&
           (strcasecmp(node->tag_name, "svg") == 0 || strcasecmp(node->tag_name, "canvas") == 0 ||
            strcasecmp(node->tag_name, "iframe") == 0);
}

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

    if (box->s->width_percent > 0.0) {
        if (box->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
            used_total_width = parent_content_width * box->s->width_percent + box->s->width_pct_px;
        } else {
            used_content_width = parent_content_width * box->s->width_percent + box->s->width_pct_px;
        }
        width_auto = false;
    } else if (box->s->width_set) {
        /* Explicit width, possibly zero (an explicit 0 is a real size). */
        if (box->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
            used_total_width = box->s->css_width;
        } else {
            used_content_width = box->s->css_width;
        }
        width_auto = false;
    }

    if (width_auto && box->s->width_keyword && g_layout_ctx &&
        box >= g_layout_ctx->boxes && box < g_layout_ctx->boxes + g_layout_ctx->tree.count) {
        int bidx = (int)(box - g_layout_ctx->boxes);
        double mx = layout_intrinsic_width(g_layout_ctx, bidx, true);
        if (box->s->width_keyword == 2) used_total_width = mx;
        else {
            double mn = layout_intrinsic_width(g_layout_ctx, bidx, false);
            if (box->s->width_keyword == 1) used_total_width = mn;
            else {
                double avail = parent_content_width - box->margin_left - box->margin_right;
                used_total_width = mx < avail ? mx : (mn > avail ? mn : avail);
            }
        }
        width_auto = false;
        /* used_total_width is already a border-box size */
        if (box->s->box_sizing == CSS_BOX_SIZING_CONTENT_BOX)
            used_content_width = used_total_width - layout_horizontal_border_padding(box);
    } else if (width_auto && layout_is_svg_box(node)) {
        /* <svg> / <canvas>: replaced, the same. */
        double cw = 0.0, ch = 0.0;
        layout_svg_size_in(box, node, parent_content_width, parent_content_height, &cw, &ch);
        used_content_width = cw;
        used_total_width = layout_content_to_total_width(box, cw);
        width_auto = false;
    } else if (width_auto && layout_is_img(node)) {
        /* Replaced element: natural size / aspect ratio, not the
         * containing block width. */
        double ch_spec = 0.0;
        bool h_auto = true;
        if (box->s->height_set && !(box->s->height_percent > 0.0)) {
            ch_spec = box->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX
                ? box->s->css_height - layout_vertical_border_padding(box) : box->s->css_height;
            h_auto = false;
        } else if (box->s->height_percent > 0.0 && parent_content_height > 0.0) {
            ch_spec = parent_content_height * box->s->height_percent + box->s->height_pct_px;
            if (box->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX) ch_spec -= layout_vertical_border_padding(box);
            h_auto = false;
        }
        /* The width follows the height min/max-height leave (§10.4). */
        if (!h_auto && (box->min_height > 0.0 || box->max_height >= 0.0)) {
            if (box->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
                double vbp = layout_vertical_border_padding(box);
                ch_spec = layout_clamp_size(ch_spec + vbp, box->min_height, box->max_height) - vbp;
            } else {
                ch_spec = layout_clamp_size(ch_spec, box->min_height, box->max_height);
            }
        }
        double cw = 0.0, ch = 0.0;
        layout_img_size(g_layout_ctx, node, 0.0, true, ch_spec, h_auto, &cw, &ch);
        used_content_width = cw;
        used_total_width = layout_content_to_total_width(box, cw);
        width_auto = false;
        if (box->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
            /* the content-box branch below converts; keep totals consistent */
        }
    } else if (width_auto) {
        if (box->s->display == CSS_DISPLAY_INLINE) {
            /* Inline boxes shrink-to-fit: leave the width to the inline flow,
             * which measures the text runs (an empty inline stays 0 wide). */
            used_total_width = 0.0;
        } else {
            /* Auto width fills the containing block content width; that is the
             * total border-box width before margins are applied. */
            used_total_width = parent_content_width - box->margin_left - box->margin_right;
            /* (at least its padding and border: the content width can't be
             * negative, §10.3.3) */
            double hbp = layout_horizontal_border_padding(box);
            if (used_total_width < hbp) used_total_width = hbp;
        }
    } else if (box->s->box_sizing == CSS_BOX_SIZING_CONTENT_BOX) {
        used_total_width = layout_content_to_total_width(box, used_content_width);
    }

    /* min/max-width constrains the same box that width specified. */
    if (box->min_width > 0.0 || box->max_width >= 0.0) {
        if (box->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
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

    if (box->s->height_percent > 0.0) {
        /* Percentage height resolves against the containing block height.
         * When that height is indefinite (depends on content), the
         * percentage computes to auto (CSS 2.1 §10.5); the box is flagged so
         * auto-height logic treats it as height:auto for this layout, and a
         * later layout against a definite height (flex stretch) resolves it. */
        box->height_pct_unresolved = 0;
        if (parent_content_height > 0.0) {
            if (box->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
                used_total_height = parent_content_height * box->s->height_percent + box->s->height_pct_px;
            } else {
                used_content_height = parent_content_height * box->s->height_percent + box->s->height_pct_px;
            }
            height_auto = false;
        } else {
            box->height_pct_unresolved = 1;
        }
    }
    if (height_auto && box->s->height_set && !(box->s->height_percent > 0.0)) {
        /* Explicit height, possibly zero. */
        if (box->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
            used_total_height = box->s->css_height;
        } else {
            used_content_height = box->s->css_height;
        }
        height_auto = false;
    } else if (height_auto && box->s->aspect_ratio > 0.0) {
        /* The aspect-ratio property relates the box box-sizing names (the
         * border box for border-box); a ::before placeholder ratio relates
         * the content box. */
        if (box->s->aspect_ratio_prop && box->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
            used_total_height = box->width * box->s->aspect_ratio;
        } else {
            double content_width = layout_total_to_content_width(box, box->width);
            used_content_height = content_width * box->s->aspect_ratio;
        }
        height_auto = false;
    }

    if (height_auto && layout_is_svg_box(node)) {
        double cw = 0.0, ch = 0.0;
        layout_svg_size_in(box, node, parent_content_width, parent_content_height, &cw, &ch);
        box->height = layout_content_to_total_height(box, ch);
    } else if (height_auto && layout_is_img(node)) {
        /* Replaced element: height from the used width and aspect ratio. */
        double cw = box->width - layout_horizontal_border_padding(box);
        if (cw < 0.0) cw = 0.0;
        double w2 = 0.0, ch = 0.0;
        layout_img_size(g_layout_ctx, node, cw, false, 0.0, true, &w2, &ch);
        box->height = layout_content_to_total_height(box, ch);
    } else if (height_auto) {
        /* Height stays 0; it will be resolved from children in the bottom-up pass. */
        box->height = 0.0;
    } else if (box->s->box_sizing == CSS_BOX_SIZING_CONTENT_BOX) {
        box->height = layout_content_to_total_height(box, used_content_height);
    } else {
        /* border-box: used_total_height already holds the total for explicit
         * and percentage sizes; aspect-ratio produced a content height that
         * must still be converted to a total. */
        if (box->s->aspect_ratio > 0.0 && !box->s->aspect_ratio_prop &&
            !(box->s->height_percent > 0.0) && box->s->css_height == 0.0) {
            box->height = layout_content_to_total_height(box, used_content_height);
        } else {
            box->height = used_total_height;
        }
    }

    /* min/max-height constrains the same box that height specified.  An
     * auto height stays 0 here (resolved from content by the formatting
     * context, which applies the clamp then). */
    if (!height_auto && (box->min_height > 0.0 || box->max_height >= 0.0)) {
        if (box->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
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
    double fs = box && box->s->font_size > 0.0 ? box->s->font_size : 16.0;
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
    if (box && box->s->line_height > 0.0) return box->s->line_height;
    double fs = box && box->s->font_size > 0.0 ? box->s->font_size : 16.0;
    TextShaper *font = display_list_get_font(layout_font_slot(box));
    if (!font) font = display_list_get_default_font();
    float lh = font ? text_shaper_normal_line_height(font, (float)fs) : 0.0f;
    if (lh > 0.5f * fs && lh < 3.0f * fs) return lh;
    return layout_line_advance(fs);
}

/* Resolve the font slot for a box's typography (sans/serif/mono x bold x italic). */
static int layout_font_slot(const LayoutBox *box)
{
    if (!box) return font_registry_default_slot();
    if (box->s->font_slot >= 0) return box->s->font_slot;
    return font_registry_resolve(box->s->font_family, box->s->font_weight, box->s->font_italic != 0);
}

static void layout_resolve_fonts_pass(LayoutContext *ctx)
{
    const LayoutBox *last = NULL;
    for (int i = 0; i < ctx->tree.count; i++) {
        LayoutBox *b = &ctx->boxes[i];
        if (b->flags & LAYOUT_STYLE_SKIPPED) continue;   /* a placeholder: no font */
        if (last && b->s->font_weight == last->s->font_weight && b->s->font_italic == last->s->font_italic &&
            strcmp(b->s->font_family, last->s->font_family) == 0) {
            b->s->font_slot = last->s->font_slot;
            continue;
        }
        b->s->font_slot = font_registry_resolve(b->s->font_family, b->s->font_weight, b->s->font_italic != 0);
        last = b;
    }
}

/* Resolve every box's font; web fonts first seen here are fetched (in
 * parallel) and the pass reruns so the boxes pick them up. */
static void layout_resolve_fonts(LayoutContext *ctx)
{
    CP_ACC("layout: resolve fonts");
    layout_resolve_fonts_pass(ctx);
    if (font_registry_load_pending() > 0) {
        layout_resolve_fonts_pass(ctx);
        css_layout_retained_invalidate();   /* (text measures differently) */
    }
}

static bool layout_text_is_whitespace(const char *s)
{
    if (!s) return true;
    for (; *s; s++) {
        if (!isspace((unsigned char)*s)) return false;
    }
    return true;
}

/* Collapsible white space (space, tab, segment break). */
static bool layout_is_space_char(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
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
    float scale = (float)(font_size / 16.0);
    text_shaper_set_spacing(box->s->letter_spacing, box->s->word_spacing, scale);
    bool measured = text_shaper_measure(font, text, &mw, &mh);
    text_shaper_set_spacing(0.0f, 0.0f, 1.0f);
    if (!measured) return false;
    if (out_w) *out_w = (double)mw * scale;
    if (out_h) *out_h = (double)mh * scale;
    if (g_layout_debug & LDBG_MEASURE) {
        fprintf(stderr, "[MEAS] fs=%.2f slot=%d mw=%.4f out=%.4f text=%.30s\n",
                font_size, layout_font_slot(box), mw, out_w ? *out_w : -1.0, text);
    }
    return true;
}

static inline bool layout_is_in_flow(const LayoutBox *box);

/* Concatenate the text in a box (used to size inline elements): its
 * descendants in the flow (not floated, positioned or display:none ones). */
static void layout_concat_box_text(LayoutContext *ctx, int idx, char *buf, size_t bufsz, size_t *len)
{
    for (int k = ctx->tree.nodes[idx].first_child_idx; k >= 0 && *len + 1 < bufsz;
         k = ctx->tree.nodes[k].next_sibling_idx) {
        const LayoutBox *kb = layout_box(ctx, k);
        if (!layout_is_in_flow(kb) || kb->s->float_side != 0) continue;
        HtmlNode *c = layout_node_dom(ctx, ctx->tree.nodes[k].dom_node_idx);
        if (!c) continue;
        const char *ct = c->type == HTML_NODE_TEXT ? css_layout_node_text(ctx, c) : NULL;
        if (ct) {
            size_t tl = strlen(ct);
            if (*len + tl >= bufsz) tl = bufsz - 1 - *len;
            memcpy(buf + *len, ct, tl);
            *len += tl;
            buf[*len] = '\0';
        } else if (c->type == HTML_NODE_ELEMENT) {
            layout_concat_box_text(ctx, k, buf, bufsz, len);
        }
    }
}

static void layout_node_serial(LayoutContext *ctx, int idx);
static void layout_node_serial_impl(LayoutContext *ctx, int idx);

/* Shift a box and all of its descendants by (dx,dy).  Fixed boxes are skipped
 * (they position against the viewport). */
static void layout_offset_subtree(LayoutContext *ctx, int idx, double dx, double dy)
{
    if (dx == 0.0 && dy == 0.0) return;
    LayoutNodeRef *node = layout_node_ref(ctx, idx);
    for (int c = node->first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        LayoutBox *child = layout_box(ctx, c);
        if (child->s->position == CSS_POSITION_FIXED) continue;
        child->x += dx;
        child->y += dy;
        /* Wrapped text runs keep absolute continuation-line origins. */
        if (child->wrap_cont_w > 0.0) {
            child->wrap_cont_x += dx;
            if (child->wrap_cont2_line > 1) child->wrap_cont2_x += dx;
        }
        layout_offset_subtree(ctx, c, dx, dy);
    }
}

/* Is this box in normal flow (participates in its parent's flow)? */
static inline bool layout_is_in_flow(const LayoutBox *box)
{
    if (box->s->display == CSS_DISPLAY_NONE) return false;
    if (box->s->position == CSS_POSITION_ABSOLUTE ||
        box->s->position == CSS_POSITION_FIXED) return false;
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

/* The width of the collapsible white space a line ends with: it hangs (CSS
 * Text 3 section 4.1.3), so alignment leaves it out.  From the last
 * members back while they are text; white-space pre and pre-wrap keep
 * theirs. */
static double layout_line_hanging_space(LayoutContext *ctx, const int *members, int count)
{
    double hang = 0.0;
    for (int i = count - 1; i >= 0; i--) {
        int m = members[i];
        HtmlNode *n = css_layout_box_node(ctx, m);
        if (!n || n->type != HTML_NODE_TEXT) break;
        const LayoutBox *b = layout_box(ctx, m);
        if (b->s->white_space == 2 || b->s->white_space == 3) break;
        TextShaper *font;
        float scale, adv;
        const char *t;
        if (!css_layout_text_paint(ctx, m, &font, &scale, &adv, &t)) break;
        size_t len = strlen(t), k = len;
        while (k > 0 && (t[k - 1] == ' ' || t[k - 1] == '\t' || t[k - 1] == '\n')) k--;
        if (k == len) break;
        if (k == 0) {
            hang += b->width;
            continue;
        }
        text_shaper_set_spacing(b->s->letter_spacing, b->s->word_spacing, scale);
        hang += text_shaper_span_width(font, t, k, len, scale);
        text_shaper_set_spacing(0.0f, 0.0f, 1.0f);
        break;
    }
    return hang;
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
    double used = (last->x + last->width) - content_left - layout_line_hanging_space(ctx, members, count);
    /* (a split inline's piece that ends the line with its right edge: that
     * edge's room is on the line too) */
    int p = ctx->tree.nodes[members[count - 1]].parent_idx;
    if (p >= 0 && ctx->pieces && p < ctx->pieces_n && ctx->pieces[p].n > 0) {
        const LayoutBox *pb = layout_box(ctx, p);
        double rpad = pb->padding_right + pb->s->border_right, end = content_left + used + rpad;
        for (int q = 0; q < ctx->pieces[p].n; q++) {
            const LayoutPiece *pc = &ctx->pieces[p].p[q];
            if (!(pc->sides & 2)) continue;
            const LayoutBox *ab = pc->anchor >= 0 ? layout_box(ctx, pc->anchor) : pb;
            if (fabs(ab->x + pc->dx + pc->w - end) < 1.0) {
                used += rpad + pb->margin_right;
                break;
            }
        }
    }
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
        if (y < floats[i].bottom - 0.001 && y >= floats[i].top - 0.001) {
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
 * dimension, else 300x150 (CSS replaced-element defaults).  A <canvas> the
 * same, its natural size and ratio its bitmap's: the width and height
 * attributes, 300x150 by default (HTML 4.12.5). */
static void layout_svg_size_in(const LayoutBox *b, HtmlNode *dom, double pcw, double pch,
                               double *out_w, double *out_h);
static void layout_svg_size(LayoutContext *ctx, int c, HtmlNode *dom, double *out_w, double *out_h)
{
    int p = ctx->tree.nodes[c].parent_idx;
    const LayoutBox *pb = p >= 0 ? layout_box(ctx, p) : NULL;
    double pcw = pb ? pb->width - layout_horizontal_border_padding(pb) : 0.0;
    double pch = pb && !layout_height_auto(pb) ? pb->height - layout_vertical_border_padding(pb) : 0.0;
    layout_svg_size_in(layout_box(ctx, c), dom, pcw, pch, out_w, out_h);
}

/* The same against a given containing block (pch 0: an auto height).  An
 * <svg>'s width/height attributes are its CSS width/height (SVG 2 §8.2), so a
 * percentage one resolves like a CSS percentage. */
static void layout_svg_size_in(const LayoutBox *b, HtmlNode *dom, double pcw, double pch,
                               double *out_w, double *out_h)
{
    double w = -1.0, h = -1.0;
    if (b->s->width_percent > 0.0) { if (pcw > 0.0) w = pcw * b->s->width_percent + b->s->width_pct_px; }
    else if (b->s->width_set) w = b->s->css_width;
    if (b->s->height_percent > 0.0) { if (pch > 0.0) h = pch * b->s->height_percent + b->s->height_pct_px; }
    else if (b->s->height_set) h = b->s->css_height;
    const char *wa = layout_node_attribute(dom, "width");
    const char *ha = layout_node_attribute(dom, "height");
    bool canvas = dom->tag_name && strcasecmp(dom->tag_name, "canvas") == 0;
    if (w < 0.0 && wa && wa[0]) {
        if (!strchr(wa, '%')) w = atof(wa);
        else if (!canvas && pcw > 0.0) w = pcw * atof(wa) / 100.0;
    }
    if (h < 0.0 && ha && ha[0]) {
        if (!strchr(ha, '%')) h = atof(ha);
        else if (!canvas && pch > 0.0) h = pch * atof(ha) / 100.0;
    }
    double ratio = 0.0;   /* width / height */
    if (canvas) {
        double aw = wa && wa[0] ? atof(wa) : 300.0, ah = ha && ha[0] ? atof(ha) : 150.0;
        if (aw < 0.0) aw = 300.0;
        if (ah < 0.0) ah = 150.0;
        ratio = aw > 0.0 && ah > 0.0 ? aw / ah : 0.0;
        if (w < 0.0) w = h >= 0.0 && ratio > 0.0 ? h * ratio : aw;
        if (h < 0.0) h = ratio > 0.0 ? w / ratio : ah;
        *out_w = w;
        *out_h = h;
        return;
    }
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
static bool layout_inline_range_empty(LayoutContext *ctx, int first, int last);

static void layout_sig_children(LayoutContext *ctx, int idx, int *out_first, int *out_last)
{
    *out_first = *out_last = -1;
    for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
         c = ctx->tree.nodes[c].next_sibling_idx) {
        LayoutBox *ch = layout_box(ctx, c);
        if (!layout_is_in_flow(ch) || ch->s->float_side != 0) continue;
        if (ch->s->display == CSS_DISPLAY_INLINE && ch->s->block_in_inline == 2) {
            /* A split inline: its children, between its edges (which make
             * line boxes when they take room). */
            const LayoutStyle *s = ch->s;
            bool l = s->margin_left != 0.0 || s->padding_left != 0.0 || s->border_left != 0.0;
            bool r = s->margin_right != 0.0 || s->padding_right != 0.0 || s->border_right != 0.0;
            if (l) {
                if (*out_first < 0) *out_first = c;
                *out_last = c;
            }
            for (int k = ctx->tree.nodes[c].first_child_idx; k >= 0; k = ctx->tree.nodes[k].next_sibling_idx) {
                LayoutBox *kb = layout_box(ctx, k);
                if (!layout_is_in_flow(kb) || kb->s->float_side != 0) continue;
                if (!layout_box_is_block_level(kb) && layout_inline_range_empty(ctx, k, k)) continue;
                if (*out_first < 0) *out_first = k;
                *out_last = k;
            }
            if (r) *out_last = c;
            continue;
        }
        HtmlNode *dn = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
        if (dn && dn->type == HTML_NODE_TEXT &&
            layout_text_is_whitespace(css_layout_node_text(ctx, dn))) continue;
        if (*out_first < 0) *out_first = c;
        *out_last = c;
    }
}

/* A childless box's own top and bottom margins are adjoining only when
 * nothing separates them: no height, min-height, padding or border. */
static bool layout_box_collapses_through(const LayoutBox *box)
{
    if (box->s->height_set && box->s->css_height > 0.0) return false;
    if (box->s->height_percent > 0.0 || box->min_height > 0.0 || box->s->aspect_ratio > 0.0) return false;
    if (box->padding_top > 0.0 || box->padding_bottom > 0.0) return false;
    if (box->s->border_top > 0.0 || box->s->border_bottom > 0.0) return false;
    return true;
}

/* Two adjoining margins as one (§8.3.1): the larger when both are positive,
 * the more negative when both are negative, else their sum. */
static double layout_margin_collapse(double a, double b)
{
    if (a >= 0.0 && b >= 0.0) return a > b ? a : b;
    if (a <= 0.0 && b <= 0.0) return a < b ? a : b;
    return a + b;
}

static bool layout_establishes_bfc(LayoutContext *ctx, int idx);

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
        if (box->s->overflow != 0 || !layout_box_collapses_through(box)) return mt;
        return layout_margin_collapse(mt, mb);
    }
    /* A box establishing a formatting context (overflow, flex, grid, a
     * table cell, ...): no collapse with its children. */
    if (layout_establishes_bfc(ctx, idx) && idx != ctx->tree.root_idx) return mt;
    if (box->padding_top > 0.0 || box->s->border_top > 0.0) return mt;
    if (!layout_box_is_block_level(layout_box(ctx, f))) return mt;
    double c = layout_eff_margin_top(ctx, f, depth + 1);
    return layout_margin_collapse(mt, c);
}

static double layout_eff_margin_bottom(LayoutContext *ctx, int idx, int depth)
{
    LayoutBox *box = layout_box(ctx, idx);
    double mt = box->margin_top, mb = box->margin_bottom;
    if (depth > 8) return mb;
    int f, l;
    layout_sig_children(ctx, idx, &f, &l);
    if (f < 0) {
        if (box->s->overflow != 0 || !layout_box_collapses_through(box)) return mb;
        return layout_margin_collapse(mt, mb);
    }
    if (layout_establishes_bfc(ctx, idx) && idx != ctx->tree.root_idx) return mb;   /* (as above) */
    if (box->padding_bottom > 0.0 || box->s->border_bottom > 0.0) return mb;
    if (!layout_height_auto(box)) return mb;     /* explicit height blocks */
    if (!layout_box_is_block_level(layout_box(ctx, l))) return mb;
    double c = layout_eff_margin_bottom(ctx, l, depth + 1);
    return layout_margin_collapse(mb, c);
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
    double fs = box->s->font_size > 0.0 ? box->s->font_size : 16.0;
    char buf[4096];
    size_t n = 0;
    double best = 0.0;
    const char *p = text;
    /* Preserved white space (pre, and pre-wrap's max-content): each line
     * is measured as written, spaces included. */
    if (box->s->white_space == 2 || (box->s->white_space == 3 && want_max)) {
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
    if (!(b->s->margin_auto & 8)) w += b->margin_left;
    if (!(b->s->margin_auto & 2)) w += b->margin_right;
    return w < 0.0 ? 0.0 : w;
}

static double layout_grid_intrinsic(LayoutContext *ctx, int idx, bool want_max);
static double layout_table_intrinsic(LayoutContext *ctx, int idx, bool want_max);
static bool table_is_table(const LayoutBox *b);
static bool table_skip_child(LayoutContext *ctx, int c);
static void layout_table_box(LayoutContext *ctx, int idx, LayoutBox *tb);

/* A table part (row group, row, cell, column, caption) in the flow. */
static bool layout_is_table_part(const LayoutBox *b)
{
    return b->s->table_role >= 2 && b->s->table_role <= 6 && b->s->display != CSS_DISPLAY_NONE &&
           b->s->float_side == 0 && b->s->position != CSS_POSITION_ABSOLUTE && b->s->position != CSS_POSITION_FIXED;
}

static bool ifc_eligible(LayoutContext *ctx, int idx);
static double ifc_intrinsic(LayoutContext *ctx, int idx, bool want_max);

static double layout_intrinsic_compute(LayoutContext *ctx, int idx, bool want_max)
{
    LayoutBox *box = layout_box(ctx, idx);
    if (box->s->display == CSS_DISPLAY_NONE) return 0.0;
    HtmlNode *dom = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    if (dom && dom->type == HTML_NODE_TEXT)
        return layout_text_intrinsic(box, css_layout_node_text(ctx, dom) ? css_layout_node_text(ctx, dom) : "",
                                     want_max || box->s->white_space == 1 || box->s->white_space == 2);
    if (!dom || dom->type != HTML_NODE_ELEMENT) return 0.0;

    double hpb = layout_horizontal_border_padding(box);
    /* Compressible replaced elements (form controls, images, ...) with a
     * percentage width contribute nothing to min-content (Sizing §5.2.2). */
    if (!want_max && box->s->width_percent > 0.0 && dom->tag_name &&
        (strcasecmp(dom->tag_name, "input") == 0 || strcasecmp(dom->tag_name, "textarea") == 0 ||
         strcasecmp(dom->tag_name, "select") == 0 || strcasecmp(dom->tag_name, "img") == 0 ||
         strcasecmp(dom->tag_name, "video") == 0 || strcasecmp(dom->tag_name, "iframe") == 0 ||
         strcasecmp(dom->tag_name, "canvas") == 0 || strcasecmp(dom->tag_name, "svg") == 0))
        return hpb;
    /* A definite (non-percentage) width is both intrinsic sizes (an inline
     * that isn't replaced has none, §10.3.1). */
    if (box->s->width_set && box->s->width_percent <= 0.0 &&
        !(box->s->display == CSS_DISPLAY_INLINE && !layout_is_img(dom) && !layout_is_svg_box(dom) &&
          !layout_is_replaced_form_element(dom->tag_name))) {
        double t = box->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX ? box->s->css_width
                                                                : box->s->css_width + hpb;
        return t < 0.0 ? 0.0 : t;
    }

    double content = 0.0;
    const char *tag = dom->tag_name ? dom->tag_name : "";
    if (strcasecmp(tag, "img") == 0) {
        double ch_spec = 0.0;
        bool h_auto = !(box->s->height_set && !(box->s->height_percent > 0.0));
        if (!h_auto) ch_spec = box->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX
            ? box->s->css_height - layout_vertical_border_padding(box) : box->s->css_height;
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
    } else if (box->s->display == CSS_DISPLAY_FLEX) {
        bool is_row = box->s->flex_direction == CSS_FLEX_DIRECTION_ROW ||
                      box->s->flex_direction == CSS_FLEX_DIRECTION_ROW_REVERSE;
        bool wraps = box->s->flex_wrap != CSS_FLEX_WRAP_NOWRAP;
        int n = 0;
        for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
             c = ctx->tree.nodes[c].next_sibling_idx) {
            LayoutBox *cb = layout_box(ctx, c);
            if (!layout_is_in_flow(cb)) continue;
            HtmlNode *cn = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
            if (cn && cn->type == HTML_NODE_TEXT && layout_text_is_whitespace(css_layout_node_text(ctx, cn)))
                continue;
            double w = layout_intrinsic_contribution(ctx, c, want_max);
            if (is_row && (want_max || !wraps)) content += w;
            else if (w > content) content = w;
            n++;
        }
        if (is_row && (want_max || !wraps) && n > 1) content += (n - 1) * box->s->gap_col;
    } else if (box->s->display == CSS_DISPLAY_GRID) {
        content = layout_grid_intrinsic(ctx, idx, want_max);
    } else if (table_is_table(box) && (content = layout_table_intrinsic(ctx, idx, want_max)) >= 0.0) {
        /* (a table without rows or cells is sized as a block) */
    } else if (ifc_eligible(ctx, idx)) {
        /* (inline content laid out on items: its widths from the same words) */
        content = ifc_intrinsic(ctx, idx, want_max);
    } else {
        content = 0.0;
        /* Block / inline formatting: inline-level children accumulate along
         * a line (max-content) while block-level children stand alone. */
        double line = 0.0, pending_space = 0.0;
        /* nowrap / pre: the inline content is one unbreakable line, so
         * min-content equals max-content. */
        bool one_line = want_max || box->s->white_space == 1 || box->s->white_space == 2;
        for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
             c = ctx->tree.nodes[c].next_sibling_idx) {
            LayoutBox *cb = layout_box(ctx, c);
            if (cb->s->display == CSS_DISPLAY_NONE) continue;
            if (cb->s->position == CSS_POSITION_ABSOLUTE || cb->s->position == CSS_POSITION_FIXED) continue;
            HtmlNode *cn = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
            if (cn && cn->type == HTML_NODE_ELEMENT && cn->tag_name &&
                strcasecmp(cn->tag_name, "br") == 0) {
                if (line > content) content = line;
                line = 0.0;
                pending_space = 0.0;
                continue;
            }
            if (cn && cn->type == HTML_NODE_TEXT && layout_text_is_whitespace(css_layout_node_text(ctx, cn))) {
                /* Collapsible space: counts only between inline content. */
                if (one_line && line > 0.0) {
                    double sw = 0.0;
                    double fs = cb->s->font_size > 0.0 ? cb->s->font_size : 16.0;
                    if (!layout_measure_text_styled(cb, " ", fs, &sw, NULL)) sw = fs * 0.25;
                    pending_space = sw;
                }
                continue;
            }
            /* A table caption never widens its table past min-content. */
            double w = layout_intrinsic_contribution(ctx, c, want_max && !cb->s->table_caption);
            if (layout_box_is_block_level(cb) && cb->s->float_side == 0) {
                if (line > content) content = line;
                line = 0.0;
                pending_space = 0.0;
                if (w > content) content = w;
            } else if (one_line) {
                line += pending_space + w;
                pending_space = 0.0;
            } else if (w > content) {
                content = w;
            }
        }
        if (line > content) content = line;
    }

    double total = content + hpb;
    if (box->min_width > 0.0 || box->max_width >= 0.0) {
        if (box->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX)
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
    HtmlNode *rd = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
    if (layout_is_svg_box(rd)) {
        /* Replaced: its own size, not shrink-to-fit (§10.3.10, §10.6.6). */
        double sw = 0.0, sh = 0.0;
        layout_svg_size(ctx, c, rd, &sw, &sh);
        if (!(child->s->width_set || child->s->width_percent > 0.0 || child->s->width_keyword))
            child->width = sw + layout_horizontal_border_padding(child);
        if (layout_height_auto(child)) child->height = sh + layout_vertical_border_padding(child);
        layout_update_content_sizes(child);
        return;
    }
    if (!(child->s->width_set || child->s->width_percent > 0.0 || child->s->width_keyword)) {
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
                double fs = child->s->font_size > 0.0 ? child->s->font_size : 16.0;
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
    if (idx == ctx->tree.root_idx || b->s->float_side != 0 || b->s->overflow != 0 ||
        b->s->position == CSS_POSITION_ABSOLUTE || b->s->position == CSS_POSITION_FIXED ||
        layout_box_is_atomic_inline(b))
        return true;
    int p = ctx->tree.nodes[idx].parent_idx;
    if (p >= 0) {
        CssDisplay pd = layout_box(ctx, p)->s->display;
        if (pd == CSS_DISPLAY_FLEX || pd == CSS_DISPLAY_GRID) return true;
    }
    /* (flow-root, flex and grid containers, tables, table cells and
     * captions) */
    if (b->s->flow_root || b->s->display == CSS_DISPLAY_FLEX || b->s->display == CSS_DISPLAY_GRID) return true;
    if (b->s->table_role == 1 || b->s->table_role == 4 || b->s->table_role == 5) return true;
    HtmlNode *dn = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    if (dn && dn->type == HTML_NODE_ELEMENT && dn->tag_name &&
        (strcasecmp(dn->tag_name, "td") == 0 || strcasecmp(dn->tag_name, "th") == 0))
        return true;
    return false;
}

/* The height that percentage heights of idx's children resolve against: its
 * content height, or, when idx is an inline laid out as a block
 * (block-in-inline: YouTube's ytw-player-with-controls), that of the
 * nearest ancestor that isn't one, the containing block a browser uses. */
static double layout_pct_base_height(LayoutContext *ctx, int idx)
{
    for (int i = idx, hops = 0; i >= 0 && hops < 8; i = ctx->tree.nodes[i].parent_idx, hops++) {
        LayoutBox *b = layout_box(ctx, i);
        if (!b->s->block_in_inline || !layout_height_auto(b)) return b->content_height;
    }
    return 0.0;
}

/* Is the inline content of the siblings first..last (first -1: none)
 * empty: only collapsible white space, floats, and inline elements with
 * nothing else and no horizontal margin, border or padding?  A piece of a split
 * inline holding nothing more makes no line box (CSS 2.1 §9.4.2). */
static bool layout_inline_range_empty(LayoutContext *ctx, int first, int last)
{
    for (int c = first; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        LayoutBox *b = layout_box(ctx, c);
        if (b->s->display != CSS_DISPLAY_NONE && b->s->position != CSS_POSITION_ABSOLUTE &&
            b->s->position != CSS_POSITION_FIXED && b->s->float_side == 0) {
            HtmlNode *d = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
            if (!d) return false;
            if (d->type == HTML_NODE_TEXT) {
                const char *t = css_layout_node_text(ctx, d);
                if ((t && !layout_text_is_whitespace(t)) || (b->s->white_space >= 2 && b->s->white_space <= 4 && t && t[0]))
                    return false;
            } else if (d->type == HTML_NODE_ELEMENT) {
                const LayoutStyle *s = b->s;
                if (s->display != CSS_DISPLAY_INLINE || !d->tag_name ||
                    strcasecmp(d->tag_name, "img") == 0 || strcasecmp(d->tag_name, "br") == 0 ||
                    layout_is_replaced_form_element(d->tag_name) || strcasecmp(d->tag_name, "svg") == 0 ||
                    strcasecmp(d->tag_name, "canvas") == 0)
                    return false;
                if (s->margin_left != 0.0 || s->margin_right != 0.0 || s->padding_left != 0.0 ||
                    s->padding_right != 0.0 || s->border_left != 0.0 || s->border_right != 0.0)
                    return false;
                if (!layout_inline_range_empty(ctx, ctx->tree.nodes[c].first_child_idx, -1)) return false;
            }
        }
        if (c == last) break;
    }
    return true;
}

/* A piece of a split inline in a block flow: the inline (el), the children
 * it has (first..last; first -1: none), which of its edges, whether it
 * makes a line box, and where its line started. */
typedef struct FlowPiece {
    int el, first, last;
    unsigned char sides;
    bool shown;
    double start_x, start_y;
} FlowPiece;

static void layout_flow_noreuse_up(LayoutContext *ctx, int idx);

static bool layout_is_br(LayoutContext *ctx, int idx)
{
    HtmlNode *n = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    return n && n->type == HTML_NODE_ELEMENT && n->tag_name && strcasecmp(n->tag_name, "br") == 0;
}

/* The start of a piece in the flow: a line box (after a block, its bottom
 * margin no longer collapses), and the room its left edge takes.  True
 * when it starts its line. */
static bool layout_piece_start(LayoutContext *ctx, FlowPiece *fp, FlowCursor *cur, double *pending_mb,
                               bool *any_placed, double content_left, double avail_width)
{
    if (!fp->shown) {
        /* (no line box: its white space and empty inlines are nowhere; its
         * floats are items of the flow) */
        for (int k = fp->first; k >= 0; k = ctx->tree.nodes[k].next_sibling_idx) {
            LayoutBox *kb = layout_box(ctx, k);
            if (kb->s->float_side != 0) {
                if (k == fp->last) break;
                continue;
            }
            kb->x = cur->x;
            kb->y = cur->y;
            kb->width = kb->height = 0.0;
            layout_update_content_sizes(kb);
            if (k == fp->last) break;
        }
        return false;
    }
    LayoutBox *eb = layout_box(ctx, fp->el);
    if (*pending_mb != 0.0) {
        cur->y += *pending_mb;
        *pending_mb = 0.0;
        *any_placed = true;
    }
    double line_left = content_left, line_avail = avail_width;
    layout_flow_line_geometry(ctx->float_stack + t_float_base, ctx->float_count - t_float_base, cur->y,
                              content_left, avail_width, &line_left, &line_avail);
    if (cur->x < line_left) cur->x = line_left;
    fp->start_x = cur->x;
    fp->start_y = cur->y;
    if (fp->sides & 1) cur->x += eb->margin_left + eb->s->border_left + eb->padding_left;
    return fp->start_x <= line_left + 0.001;
}

/* The end of a piece: the room its right edge takes, and its border boxes,
 * one per line it is on, noted against their first child (which line
 * alignment and later moves carry), the inline's own box when it has
 * none.  line_ends: nothing follows it on its line (a trailing space of
 * its text hangs there, outside it). */
static void layout_piece_end(LayoutContext *ctx, FlowPiece *fp, FlowCursor *cur, bool line_ends)
{
    if (!fp->shown) return;
    int el = fp->el;
    LayoutBox *eb = layout_box(ctx, el);
    if (fp->sides & 2) cur->x += eb->padding_right + eb->s->border_right + eb->margin_right;
    double adv = layout_line_advance_box(eb);
    if (adv > cur->line_box) cur->line_box = adv;
    double ca = layout_content_area_height(eb);
    double lpad = eb->padding_left + eb->s->border_left, rpad = eb->padding_right + eb->s->border_right;
    LayoutPieceList *pl = &ctx->pieces[el];
    /* (lines: runs of children with the same top) */
    int k = fp->first, nlines = 0;
    for (;;) {
        int anchor = -1, end = -1;
        double x0 = 1e30, x1 = -1e30, ly = 0.0;
        for (; k >= 0; k = k == fp->last ? -1 : ctx->tree.nodes[k].next_sibling_idx) {
            const LayoutBox *kb = layout_box(ctx, k);
            if (!layout_is_in_flow(kb) || (kb->width <= 0.0 && kb->height <= 0.0)) continue;
            if (anchor >= 0 && fabs(kb->y - ly) > 0.5) break;
            if (anchor < 0) { anchor = k; ly = kb->y; }
            if (kb->x < x0) x0 = kb->x;
            if (kb->x + kb->width > x1) { x1 = kb->x + kb->width; end = k; }
        }
        if (k < 0 && line_ends && end >= 0) {
            const LayoutBox *eb2 = layout_box(ctx, end);
            HtmlNode *tn = layout_node_dom(ctx, ctx->tree.nodes[end].dom_node_idx);
            const char *t = tn && tn->type == HTML_NODE_TEXT ? css_layout_box_text(ctx, end, css_layout_node_text(ctx, tn)) : NULL;
            size_t tl = t ? strlen(t) : 0;
            double sw = 0.0;
            double fs = eb2->s->font_size > 0.0 ? eb2->s->font_size : 16.0;
            if (tl > 0 && t[tl - 1] == ' ' && eb2->s->white_space != 2 && eb2->s->white_space != 3 &&
                eb2->wrap_cont_w <= 0.0 && layout_measure_text_styled(eb2, " ", fs, &sw, NULL))
                x1 -= sw;
        }
        bool first_line = nlines == 0, last_line = k < 0;
        /* (the line's top: where the piece started, or a text box on it; an
         * inline box on it is placed in the line by its own leading) */
        if (first_line) {
            ly = fp->start_y;
        } else {
            for (int t = anchor; t >= 0; t = ctx->tree.nodes[t].first_child_idx) {
                HtmlNode *tn = layout_node_dom(ctx, ctx->tree.nodes[t].dom_node_idx);
                if (tn && tn->type == HTML_NODE_TEXT) { ly = layout_box(ctx, t)->y; break; }
            }
        }
        if (anchor < 0) {
            if (nlines > 0) break;
            /* (nothing in it: its edges alone) */
            x0 = x1 = fp->start_x + ((fp->sides & 1) ? eb->margin_left + lpad : 0.0);
            ly = fp->start_y;
        }
        LayoutPiece pc;
        /* (its start edge on its first line, its end edge on its last: left
         * and right, or right and left in a right-to-left inline) */
        bool rtl = eb->s->direction == 1;
        pc.sides = (unsigned char)(((fp->sides & 1) && (rtl ? last_line : first_line) ? 1 : 0) |
                                   ((fp->sides & 2) && (rtl ? first_line : last_line) ? 2 : 0));
        double bx = x0 - ((pc.sides & 1) ? lpad : 0.0);
        double bw = x1 + ((pc.sides & 2) ? rpad : 0.0) - bx;
        double by = ly + (adv - ca) / 2.0 - eb->padding_top - eb->s->border_top;
        double bh = ca + eb->padding_top + eb->padding_bottom + eb->s->border_top + eb->s->border_bottom;
        const LayoutBox *ab = anchor >= 0 ? layout_box(ctx, anchor) : NULL;
        pc.anchor = anchor;
        pc.dx = bx - (ab ? ab->x : 0.0);
        pc.dy = by - (ab ? ab->y : 0.0);
        pc.w = bw;
        pc.h = bh;
        if (pl->n == pl->cap) {
            int nc = pl->cap ? 2 * pl->cap : 4;
            LayoutPiece *np = (LayoutPiece *)realloc(pl->p, (size_t)nc * sizeof(LayoutPiece));
            if (!np) return;
            pl->p = np;
            pl->cap = nc;
        }
        pl->p[pl->n++] = pc;
        nlines++;
        if (k < 0) break;
    }
}

/* Place float c of a block flow whose content box starts at content_left,
 * avail_width wide (CSS 2.1 section 9.5.1): laid out at the full width (an
 * auto width shrinking to fit), its top no higher than top, below what it
 * clears and no higher than an earlier float, as far to its side as the
 * floats there leave room, else lower down; noted in floats (cap of them)
 * and on the float stack, so lines and later blocks shrink around it. */
static void layout_place_float(LayoutContext *ctx, int c, double content_left, double avail_width, double top,
                               FloatRec *floats, int *nfloats, int cap)
{
    LayoutBox *child = layout_box(ctx, c);
    /* Lay out the float's subtree at the full available width; the
     * used (shrink-to-fit) width comes from the widest in-flow child
     * extent (e.g. a fixed-width thumb frame inside). */
    child->x = content_left + child->margin_left;
    /* (below the margin of the block before it, which still collapses
     * with what follows) */
    child->y = top + child->margin_top;
    if (!(child->s->width_set || child->s->width_percent > 0.0 || child->s->width_keyword)) {
        /* An auto-width float shrinks to fit (§10.3.5) before its
         * layout: a caption wraps instead of widening a floated
         * table, auto grid tracks don't stretch, and an empty float
         * is only its padding and border. */
        double a = avail_width - child->margin_left - child->margin_right;
        if (a < 0.0) a = 0.0;
        child->width = layout_shrink_to_fit(ctx, c, a);
    }
    layout_update_content_sizes(child);
    layout_node_serial(ctx, c);

    double used = 0.0;
    for (int k = ctx->tree.nodes[c].first_child_idx; k >= 0;
         k = ctx->tree.nodes[k].next_sibling_idx) {
        LayoutBox *kc = layout_box(ctx, k);
        if (!layout_is_in_flow(kc)) continue;
        double ext = kc->x + kc->width + kc->margin_right
                   - (child->x + child->padding_left + child->s->border_left);
        if (ext > used) used = ext;
    }
    bool fw_auto = !(child->s->width_set || child->s->width_percent > 0.0 || child->s->width_keyword);
    if (fw_auto && used > 0.0 && used < child->width) {
        child->width = used + child->padding_left + child->padding_right
                      + child->s->border_left + child->s->border_right;
        layout_update_content_sizes(child);
    }

    /* Where it goes (§9.5.1): below what it clears and no higher than
     * an earlier float; as far to its side as the floats already
     * there leave room, else lower down, past them. */
    double content_right = content_left + avail_width;
    double fy = child->y - child->margin_top;
    const FloatRec *fl = ctx->float_stack + t_float_base;
    int nf = ctx->float_count - t_float_base;
    for (int fi = 0; fi < nf; fi++) {
        bool match = child->s->clear == 3 || (child->s->clear && child->s->clear == fl[fi].side);
        if (match && fl[fi].bottom > fy) fy = fl[fi].bottom;
        if (fl[fi].top > fy) fy = fl[fi].top;
    }
    double mw = child->margin_left + child->width + child->margin_right;
    double mh = child->margin_top + child->height + child->margin_bottom;
    double fl_left = content_left, fl_right = content_right;
    for (int guard = 0; guard < 64; guard++) {
        fl_left = content_left;
        fl_right = content_right;
        double next = -1.0;
        for (int fi = 0; fi < nf; fi++) {
            if (fl[fi].bottom <= fy + 0.001 || fl[fi].top >= fy + (mh > 0.0 ? mh : 0.001) - 0.001) continue;
            if (fl[fi].right <= content_left || fl[fi].left >= content_right) continue;
            if (fl[fi].side == 1 && fl[fi].right > fl_left) fl_left = fl[fi].right;
            if (fl[fi].side == 2 && fl[fi].left < fl_right) fl_right = fl[fi].left;
            if (next < 0.0 || fl[fi].bottom < next) next = fl[fi].bottom;
        }
        if (mw <= fl_right - fl_left + 0.01 || next < 0.0) break;
        fy = next;
    }
    double new_x = (child->s->float_side == 1)
        ? fl_left + child->margin_left
        : fl_right - child->margin_right - child->width;
    double dx = new_x - child->x, dy = fy + child->margin_top - child->y;
    if (dx != 0.0 || dy != 0.0) {
        child->x = new_x;
        child->y += dy;
        layout_offset_subtree(ctx, c, dx, dy);
    }

    if (*nfloats < cap) {
        FloatRec rec;
        rec.left = child->x - child->margin_left;
        rec.right = child->x + child->width + child->margin_right;
        rec.top = child->y - child->margin_top;
        rec.bottom = child->y + child->height + child->margin_bottom;
        rec.side = child->s->float_side;
        floats[(*nfloats)++] = rec;
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

/* In rtl, the first line's indent is at its right end: aligned short of it. */
#define RTL_FIRST_CUT (box->s->direction == 1 && !block_seen && cur.y == content_top ? indent0 : 0.0)
/* ---- Bidi paragraphs (BIDI_DESIGN.md) ----
 * A block's inline content is a paragraph; one that needs bidi has its
 * text boxes painted from per-line fragments.  CYBER_BIDI_ALL=1 gives every
 * paragraph fragments (to check they paint as the boxes' geometry does). */

static int g_bidi_all = -1;

/* Text that may need the bidi algorithm: a right-to-left letter or digit
 * block (U+0590-08FF, U+FB1D-FDFF, U+FE70-FEFF, U+10800-10FFF,
 * U+1E800-1EFFF) or a bidi control (ALM, LRM, RLM, U+202A-202E,
 * U+2066-2069), by their UTF-8 bytes. */
static bool layout_text_has_bidi(const char *t, size_t n)
{
    const unsigned char *p = (const unsigned char *)t, *e = p + n;
    for (; p < e; p++) {
        unsigned char c = *p;
        if (c < 0xD6) continue;
        if (c <= 0xDF) {
            if (c > 0xD6 || (p + 1 < e && p[1] >= 0x90)) return true;   /* (U+0590-07FF) */
        } else if (c == 0xE0) {
            if (p + 1 < e && p[1] >= 0xA0 && p[1] <= 0xA3) return true;  /* (U+0800-08FF) */
        } else if (c == 0xE2 && p + 2 < e) {
            if (p[1] == 0x80 && (p[2] == 0x8E || p[2] == 0x8F || (p[2] >= 0xAA && p[2] <= 0xAE))) return true;
            if (p[1] == 0x81 && p[2] >= 0xA6 && p[2] <= 0xA9) return true;
        } else if (c == 0xEF && p + 1 < e) {
            if ((p[1] >= 0xAC && p[1] <= 0xB7) || (p[1] >= 0xB9 && p[1] <= 0xBB)) return true;   /* (FB00-FDFF, FE40-FEFF) */
        } else if (c == 0xF0 && p + 1 < e) {
            if (p[1] == 0x90 && p + 2 < e && p[2] >= 0xA0) return true;   /* (U+10800-10FFF) */
            if (p[1] == 0x9E && p + 2 < e && p[2] >= 0xA0) return true;   /* (U+1E800-1EFFF) */
        }
    }
    return false;
}

/* The paragraph of block idx: each text box of its inline content (into
 * inline elements, not into atomic inlines or blocks), to fn; whether
 * anything in it needs bidi when fn is NULL. */
static bool layout_paragraph_walk(LayoutContext *ctx, int idx, void (*fn)(LayoutContext *, int))
{
    for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        LayoutBox *cb = layout_box(ctx, c);
        if (!layout_is_in_flow(cb) || cb->s->display == CSS_DISPLAY_NONE) continue;
        HtmlNode *n = css_layout_box_node(ctx, c);
        if (n && n->type == HTML_NODE_TEXT) {
            if (fn) {
                fn(ctx, c);
            } else {
                const char *t = css_layout_box_text(ctx, c, css_layout_node_text(ctx, n));
                if (t && layout_text_has_bidi(t, strlen(t))) return true;
            }
            continue;
        }
        if (cb->s->display != CSS_DISPLAY_INLINE) continue;
        if (!fn && (cb->s->direction == 1 || cb->s->unicode_bidi != 0)) return true;
        if (layout_paragraph_walk(ctx, c, fn) && !fn) return true;
    }
    return false;
}

/* Text box idx's fragments, one per line it paints (from its geometry, as
 * painting it from that would). */
static void layout_text_frags_build(LayoutContext *ctx, int idx)
{
    LayoutTextFragList *fl = &ctx->text_frags[idx];
    int had = fl->f ? fl->n : 0;   /* (its array holds at least this many) */
    fl->n = 0;
    const LayoutBox *b = layout_box(ctx, idx);
    TextShaper *font = NULL;
    float scale = 1.0f, adv = 0.0f;
    const char *text;
    TsLine stack[16], *ls = stack;
    int n;
    if (b->wrap_cont_w <= 0.0 && b->s->line_height > 0.0) {
        /* (one line with a set line height: no font needed, the text as
         * css_layout_text_paint gives it) */
        HtmlNode *tn = css_layout_box_node(ctx, idx);
        const char *raw = tn ? css_layout_node_text(ctx, tn) : NULL;
        text = raw ? css_layout_box_text(ctx, idx, raw) : NULL;
        if (!text) return;
        if (b->text_lead_skip && text[0] == ' ') text++;
        adv = (float)b->s->line_height;
    } else if (!css_layout_text_paint(ctx, idx, &font, &scale, &adv, &text)) {
        return;
    }
    if (b->wrap_cont_w > 0.0) {
        text_shaper_set_spacing(b->s->letter_spacing, b->s->word_spacing, scale);
        n = text_shaper_wrap_lines(font, text, (float)b->x, (float)b->y, (float)b->wrap_cont_x,
                                   (float)b->wrap_first_w, (float)b->wrap_cont_w, scale, adv, b->wrap_cont2_line,
                                   (float)b->wrap_cont2_x, (float)b->wrap_cont2_w, ls, 16);
        if (n > 16 && (ls = (TsLine *)malloc((size_t)n * sizeof(TsLine))) != NULL)
            n = text_shaper_wrap_lines(font, text, (float)b->x, (float)b->y, (float)b->wrap_cont_x,
                                       (float)b->wrap_first_w, (float)b->wrap_cont_w, scale, adv,
                                       b->wrap_cont2_line, (float)b->wrap_cont2_x, (float)b->wrap_cont2_w, ls, n);
        text_shaper_set_spacing(0.0f, 0.0f, 1.0f);
        if (!ls) return;
    } else {
        /* (one line: the em box centered in the line; a pre run with
         * newlines is painted as before) */
        size_t len = strlen(text);
        if (memchr(text, '\n', len)) return;
        double fs = b->s->font_size > 0.0 ? b->s->font_size : 16.0;
        stack[0].start = 0;
        stack[0].end = (uint32_t)len;
        stack[0].x = (float)b->x;
        stack[0].y = (float)(b->y + (adv - fs) * 0.5);
        stack[0].width = (float)b->width;
        n = 1;
    }
    if (n > had) {
        LayoutTextFrag *g = (LayoutTextFrag *)realloc(fl->f, (size_t)n * sizeof(LayoutTextFrag));
        if (!g) n = 0;
        else fl->f = g;
    }
    if (fl->f) {
        for (int i = 0; i < n; i++) {
            fl->f[i].start = ls[i].start;
            fl->f[i].end = ls[i].end;
            fl->f[i].dx = (double)ls[i].x - b->x;
            fl->f[i].dy = (double)ls[i].y - b->y;
            fl->f[i].w = ls[i].width;
            fl->f[i].level = 0;
        }
        fl->n = n;
    }
    if (ls != stack) free(ls);
}

/* The bidi pass's working memory: a per-thread bump allocator, rewound at
 * the start of each paragraph and kept between them (one block once it has
 * grown to the largest paragraph's need).  Each allocation has a 16-byte
 * header with its size, so the last one can grow in place; freeing is a
 * no-op. */
typedef struct BumpChunk {
    struct BumpChunk *prev;
    size_t cap, used, last;   /* (last: the offset of the newest allocation's header) */
    size_t pad;
} BumpChunk;

static _Thread_local BumpChunk *t_bump;

static void bump_reset(void)
{
    if (t_bump && t_bump->prev) {
        size_t total = 0;
        for (BumpChunk *c = t_bump; c;) {
            BumpChunk *p = c->prev;
            total += c->cap;
            free(c);
            c = p;
        }
        t_bump = (BumpChunk *)malloc(sizeof(BumpChunk) + total);
        if (!t_bump) return;
        t_bump->prev = NULL;
        t_bump->cap = total;
    }
    if (t_bump) t_bump->used = t_bump->last = 0;
}

static void *bump_alloc(size_t n)
{
    size_t need = ((n + 15) & ~(size_t)15) + 16;
    BumpChunk *c = t_bump;
    if (!c || c->used + need > c->cap) {
        size_t cap = c ? c->cap * 2 : 65536;
        while (cap < need) cap *= 2;
        BumpChunk *nc = (BumpChunk *)malloc(sizeof(BumpChunk) + cap);
        if (!nc) return NULL;
        nc->prev = c;
        nc->cap = cap;
        nc->used = nc->last = 0;
        t_bump = c = nc;
    }
    unsigned char *base = (unsigned char *)(c + 1);
    *(size_t *)(base + c->used) = n;
    c->last = c->used;
    c->used += need;
    return base + c->last + 16;
}

static void *bump_calloc(size_t a, size_t b)
{
    void *p = bump_alloc(a * b);
    if (p) memset(p, 0, a * b);
    return p;
}

static void *bump_realloc(void *p, size_t n)
{
    if (!p) return bump_alloc(n);
    unsigned char *h = (unsigned char *)p - 16;
    size_t old = *(size_t *)h;
    BumpChunk *c = t_bump;
    unsigned char *base = (unsigned char *)(c + 1);
    if (h == base + c->last) {
        size_t need = ((n + 15) & ~(size_t)15) + 16;
        if (c->last + need <= c->cap) {
            *(size_t *)h = n;
            c->used = c->last + need;
            return p;
        }
    }
    void *q = bump_alloc(n);
    if (q) memcpy(q, p, old < n ? old : n);
    return q;
}

static inline void bump_free(void *p) { (void)p; }

/* Resolved levels by paragraph text: a paragraph laid out again with the
 * same code points and base direction takes its levels from here instead
 * of running UAX #9 again.  Shared by the layout threads (a paragraph is
 * laid out by whichever thread takes its subtree, which changes from one
 * layout to the next), direct-mapped by a hash of the text (the text itself
 * compared on a hit), a lock per stripe of slots; nothing more is kept once
 * it holds 16 MB, an entry replaced frees its room. */
typedef struct {
    uint64_t h;
    int n, base, para;
    uint32_t *cp;
    uint8_t *lv;
} BidiLevelsEntry;

#define BIDI_LEVELS_SLOTS 8192
#define BIDI_LEVELS_STRIPES 64
static BidiLevelsEntry g_lvc[BIDI_LEVELS_SLOTS];
static pthread_mutex_t g_lvc_mu[BIDI_LEVELS_STRIPES] = { [0 ... BIDI_LEVELS_STRIPES - 1] = PTHREAD_MUTEX_INITIALIZER };
static _Atomic size_t g_lvc_bytes;

static uint64_t bidi_text_hash(const uint32_t *cp, int n, int base)
{
    uint64_t h = 1469598103934665603ull ^ (uint64_t)(base + 2);
    for (int i = 0; i < n; i++) h = (h ^ cp[i]) * 1099511628211ull;
    return h;
}

/* The levels of P's text at base (-1: from its text), into lv: from the
 * cache, else resolved and kept.  The paragraph level, or -1. */
static int bidi_levels_cached(const uint32_t *cp, int n, int base, uint8_t *lv)
{
    uint64_t h = bidi_text_hash(cp, n, base);
    unsigned slot = (unsigned)(h >> 20) & (BIDI_LEVELS_SLOTS - 1);
    BidiLevelsEntry *e = &g_lvc[slot];
    pthread_mutex_t *mu = &g_lvc_mu[slot & (BIDI_LEVELS_STRIPES - 1)];
    pthread_mutex_lock(mu);
    if (e->cp && e->h == h && e->n == n && e->base == base && memcmp(e->cp, cp, (size_t)n * sizeof(uint32_t)) == 0) {
        memcpy(lv, e->lv, (size_t)n);
        int para = e->para;
        pthread_mutex_unlock(mu);
        return para;
    }
    pthread_mutex_unlock(mu);
    int para = bidi_resolve(cp, n, base, lv);
    if (para < 0) return para;
    size_t sz = (size_t)n * (sizeof(uint32_t) + 1);
    pthread_mutex_lock(mu);
    size_t old = e->cp ? (size_t)e->n * (sizeof(uint32_t) + 1) : 0;
    if (atomic_load(&g_lvc_bytes) - old + sz <= ((size_t)16 << 20)) {
        uint32_t *mem = (uint32_t *)realloc(e->cp, sz);
        if (mem) {
            e->cp = mem;
            e->lv = (uint8_t *)(mem + n);
            memcpy(e->cp, cp, (size_t)n * sizeof(uint32_t));
            memcpy(e->lv, lv, (size_t)n);
            e->h = h;
            e->n = n;
            e->base = base;
            e->para = para;
            atomic_fetch_add(&g_lvc_bytes, sz);
            atomic_fetch_sub(&g_lvc_bytes, old);
        }
    }
    pthread_mutex_unlock(mu);
    return para;
}

/* ---- The paragraph's text, levels, and its lines in visual order ----
 * The paragraph is its inline content as code points in logical order: the
 * text boxes' painted text, U+FFFC for an atomic inline, U+2029 for a block
 * between, and around an inline box with unicode-bidi the controls it
 * stands for (CSS Writing Modes 3 section 2.4.2).  UAX #9 gives each a
 * level; a line is the fragments and atomic inlines the flow put on it, cut
 * into runs of one level, which L2 puts in visual order from the line's
 * start. */

typedef struct {
    uint32_t *cp;     /* code points */
    int *box;         /* the text box or atomic inline of each (-1: a control) */
    uint32_t *byte;   /* its byte offset in the box's painted text */
    int n, cap;
} BidiPara;

static bool bidi_para_put(BidiPara *P, uint32_t cp, int box, uint32_t byte)
{
    if (P->n == P->cap) {
        int nc = P->cap ? P->cap * 2 : 256;
        uint32_t *a = (uint32_t *)bump_realloc(P->cp, (size_t)nc * sizeof(uint32_t));
        if (!a) return false;
        P->cp = a;
        int *b = (int *)bump_realloc(P->box, (size_t)nc * sizeof(int));
        if (!b) return false;
        P->box = b;
        uint32_t *c = (uint32_t *)bump_realloc(P->byte, (size_t)nc * sizeof(uint32_t));
        if (!c) return false;
        P->byte = c;
        P->cap = nc;
    }
    P->cp[P->n] = cp;
    P->box[P->n] = box;
    P->byte[P->n] = byte;
    P->n++;
    return true;
}

/* Room in P for more code points. */
static bool bidi_para_reserve(BidiPara *P, int more)
{
    if (P->n + more <= P->cap) return true;
    int nc = P->cap ? P->cap : 256;
    while (nc < P->n + more) nc *= 2;
    uint32_t *a = (uint32_t *)bump_realloc(P->cp, (size_t)nc * sizeof(uint32_t));
    int *b = a ? (int *)bump_realloc(P->box, (size_t)nc * sizeof(int)) : NULL;
    uint32_t *c = b ? (uint32_t *)bump_realloc(P->byte, (size_t)nc * sizeof(uint32_t)) : NULL;
    if (a) P->cp = a;
    if (b) P->box = b;
    if (!c) return false;
    P->byte = c;
    P->cap = nc;
    return true;
}

/* The next code point of p (UTF-8, as the shaper decodes it). */
static uint32_t bidi_utf8_next(const unsigned char **pp, const unsigned char *e)
{
    const unsigned char *p = *pp;
    uint32_t c = *p++;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (more) c &= 0x3Fu >> more;
    for (; more && p < e; more--) c = (c << 6) | (*p++ & 0x3F);
    *pp = p;
    return c;
}

static bool layout_is_replaced_tag(const char *t)
{
    static const char *const tags[] = { "img", "input", "video", "canvas", "iframe", "svg", "object", "embed",
                                        "select", "textarea", "button", "audio", "meter", "progress", NULL };
    for (int i = 0; tags[i]; i++)
        if (!strcmp(t, tags[i])) return true;
    return false;
}

/* The controls an inline box's unicode-bidi stands for, opening (or closing). */
static int bidi_controls(const LayoutBox *b, bool open, uint32_t out[2])
{
    bool r = b->s->direction == 1;
    switch (b->s->unicode_bidi) {
    case 1: out[0] = open ? (r ? 0x202B : 0x202A) : 0x202C; return 1;
    case 2: out[0] = open ? (r ? 0x2067 : 0x2066) : 0x2069; return 1;
    case 3: out[0] = open ? (r ? 0x202E : 0x202D) : 0x202C; return 1;
    case 4:
        if (open) { out[0] = r ? 0x2067 : 0x2066; out[1] = r ? 0x202E : 0x202D; }
        else { out[0] = 0x202C; out[1] = 0x2069; }
        return 2;
    case 5: out[0] = open ? 0x2068 : 0x2069; return 1;
    default: return 0;
    }
}

/* Block idx's inline content in logical order into P; items, the text
 * boxes and atomic inlines met, in order. */
static void bidi_para_collect(LayoutContext *ctx, int idx, BidiPara *P, int *items, int *item_cp, int *nitems)
{
    for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        LayoutBox *cb = layout_box(ctx, c);
        if (!layout_is_in_flow(cb) || cb->s->display == CSS_DISPLAY_NONE) continue;
        HtmlNode *n = css_layout_box_node(ctx, c);
        if (n && n->type == HTML_NODE_TEXT) {
            /* (the text it paints, as css_layout_text_paint gives it) */
            const char *raw = css_layout_node_text(ctx, n);
            if (!raw) continue;
            const char *t = css_layout_box_text(ctx, c, raw);
            if (!t) continue;
            if (cb->text_lead_skip && t[0] == ' ') t++;
            const unsigned char *p = (const unsigned char *)t, *e = p + strlen(t);
            item_cp[*nitems] = P->n;
            if (!bidi_para_reserve(P, (int)(e - p))) continue;
            while (p < e) {
                uint32_t off = (uint32_t)(p - (const unsigned char *)t);
                uint32_t cp = *p < 0x80 ? *p++ : bidi_utf8_next(&p, e);
                P->cp[P->n] = cp;
                P->box[P->n] = c;
                P->byte[P->n] = off;
                P->n++;
            }
            items[(*nitems)++] = c;
            continue;
        }
        bool inline_box = cb->s->display == CSS_DISPLAY_INLINE &&
                          !(n && n->type == HTML_NODE_ELEMENT && n->tag_name && layout_is_replaced_tag(n->tag_name));
        if (inline_box && n && n->type == HTML_NODE_ELEMENT && n->tag_name && !strcmp(n->tag_name, "br")) {
            bidi_para_put(P, 0x2029, -1, 0);   /* (a forced break ends a paragraph) */
        } else if (inline_box) {
            uint32_t ctl[2];
            int k = bidi_controls(cb, true, ctl);
            for (int i = 0; i < k; i++) bidi_para_put(P, ctl[i], -1, 0);
            bidi_para_collect(ctx, c, P, items, item_cp, nitems);
            k = bidi_controls(cb, false, ctl);
            for (int i = 0; i < k; i++) bidi_para_put(P, ctl[i], -1, 0);
        } else if (cb->s->inline_level || cb->s->display != CSS_DISPLAY_BLOCK) {
            item_cp[*nitems] = P->n;
            bidi_para_put(P, 0xFFFC, c, 0);
            items[(*nitems)++] = c;
        } else {
            bidi_para_put(P, 0x2029, -1, 0);
        }
    }
}

/* One run of a line: a text fragment's bytes at one level, or an atomic
 * inline (frag -1). */
typedef struct {
    int box, frag;
    uint32_t start, end;
    double x0, w, gap;   /* where the flow put it, its width, the space before it */
    int cp0;             /* its first code point in the paragraph */
    int item;            /* its item (-1: a control) */
    unsigned char hang;  /* white space at the line's end: no room */
    double lead, trail;  /* the edges (margin, border, padding) of the inline boxes it opens / closes */
    unsigned char level;
} BidiRun;

/* A paragraph's inline boxes (its items' ancestors below the block), found
 * by box index in a small open-addressed table: which items they begin and
 * end at, and their content's extent before and after reordering, on one
 * line (line -1: not one whole piece on one line).  And for its pieces
 * after reordering: its first and last line, its leftmost and rightmost
 * run on the line being placed, the piece open there (where it starts, which
 * edge it has), and the top of its text on its first line and on this one. */
typedef struct {
    int box, first, last, line;
    double o0, o1, n0, n1;
    int lfirst, llast, lm_line, lm, rm, ty_line, np;
    bool open, has_base, keep;
    unsigned char psides;
    double ps, ty, base;
} BidiAnc;

/* A piece of a paragraph's inline box (entry a), in page coordinates. */
typedef struct {
    int a;
    LayoutPiece pc;
} BidiPieceRec;



typedef struct {
    BidiAnc *a;
    int n, cap, mask;
    int *slot;
} BidiAncs;

static bool bidi_ancs_init(BidiAncs *A, int cap)
{
    int size = 16;
    while (size < cap * 2) size *= 2;
    A->a = (BidiAnc *)bump_alloc((size_t)cap * sizeof(BidiAnc));
    A->slot = (int *)bump_alloc((size_t)size * sizeof(int));
    A->n = 0;
    A->cap = cap;
    A->mask = size - 1;
    if (!A->a || !A->slot) return false;
    for (int i = 0; i < size; i++) A->slot[i] = -1;
    return true;
}

/* Box's entry (added when add), or -1. */
static int bidi_anc(BidiAncs *A, int box, bool add)
{
    for (unsigned h = ((unsigned)box * 2654435761u) & (unsigned)A->mask;; h = (h + 1) & (unsigned)A->mask) {
        int k = A->slot[h];
        if (k >= 0 && A->a[k].box == box) return k;
        if (k >= 0) continue;
        if (!add || A->n == A->cap) return -1;
        BidiAnc *e = &A->a[A->n];
        memset(e, 0, sizeof(*e));
        e->box = box;
        e->first = e->last = -1;
        e->line = -2;
        e->lfirst = e->llast = e->lm_line = e->ty_line = -1;
        A->slot[h] = A->n;
        return A->n++;
    }
}

/* An upper bound on the items and inline boxes of block idx's paragraph:
 * its children, and theirs inside inline boxes (not inside blocks or atomic
 * inlines, as bidi_para_collect goes). */
static int bidi_count_items(LayoutContext *ctx, int idx)
{
    int n = 0;
    for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        n++;
        if (ctx->tree.nodes[c].first_child_idx >= 0 && layout_box(ctx, c)->s->display == CSS_DISPLAY_INLINE)
            n += bidi_count_items(ctx, c);
    }
    return n;
}

/* The runs of block idx's paragraph reordered line by line: text fragments
 * cut at level changes and placed in visual order, atomic inlines moved;
 * a line holding wrapped text aligned here (the flow leaves those). */
static void layout_bidi_reorder(LayoutContext *ctx, int idx)
{
    LayoutBox *blk = layout_box(ctx, idx);
    bump_reset();
    BidiPara P = { 0 };
    int maxitems = bidi_count_items(ctx, idx) + 1;
    int *items = (int *)bump_alloc((size_t)maxitems * sizeof(int));
    int *first_cp = (int *)bump_alloc((size_t)maxitems * sizeof(int));
    if (!items || !first_cp) { bump_free(items); bump_free(first_cp); return; }
    int nitems = 0;
    /* (an override or plaintext on the block itself: around all of it) */
    int nopen = blk->s->unicode_bidi == 3 || blk->s->unicode_bidi == 4 ? 1 : 0;
    if (nopen) bidi_para_put(&P, blk->s->direction == 1 ? 0x202E : 0x202D, -1, 0);
    bidi_para_collect(ctx, idx, &P, items, first_cp, &nitems);
    if (nopen) bidi_para_put(&P, 0x202C, -1, 0);
    if (P.n == 0 || nitems == 0) goto out;
    /* (nothing right to left in it: every level even, nothing reversed) */
    if (blk->s->direction != 1) {
        bool rtl = false;
        for (int i = 0; i < P.n && !rtl; i++) {
            uint32_t c = P.cp[i];
            if (c < 0x590) continue;
            int k = bidi_class(c);
            rtl = k == BIDI_R || k == BIDI_AL || k == BIDI_AN || k == BIDI_RLE || k == BIDI_RLO || k == BIDI_RLI ||
                  k == BIDI_FSI;
        }
        if (!rtl) goto out;
    }
    uint8_t *lv = (uint8_t *)bump_alloc((size_t)P.n);
    if (!lv) goto out;
    int para = bidi_levels_cached(P.cp, P.n, blk->s->unicode_bidi == 5 ? -1 : blk->s->direction, lv);
    if (para < 0) { bump_free(lv); goto out; }
    /* (no odd level: nothing is reversed, every line stays as the flow put it) */
    {
        bool odd = false;
        for (int i = 0; i < P.n && !odd; i++) odd = lv[i] & 1;
        if (!odd) { bump_free(lv); goto out; }
    }
    /* (reordered: its text boxes get their fragments, and it is laid out
     * again at the next pass, not reused) */
    layout_flow_noreuse_up(ctx, idx);
    for (int i = 0; i < nitems; i++) {
        HtmlNode *tn = css_layout_box_node(ctx, items[i]);
        if (tn && tn->type == HTML_NODE_TEXT) layout_text_frags_build(ctx, items[i]);
    }

    /* The lines: in logical order, a fragment after a box's first, or an
     * item the flow put left of the one before, starts one. */
    int cap = 64, nr = 0;
    BidiRun *runs = (BidiRun *)bump_alloc((size_t)cap * sizeof(BidiRun));
    if (!runs) { bump_free(lv); goto out; }
    /* Each item's lead and trail: the left edges of the inline boxes it is
     * the first content of, the right edges of those it is the last of. */
    double *lead = (double *)bump_calloc((size_t)nitems, sizeof(double)), *trail = (double *)bump_calloc((size_t)nitems, sizeof(double));
    BidiAncs A = { 0 };
    if (!lead || !trail || !bidi_ancs_init(&A, maxitems)) {
        bump_free(lead); bump_free(trail); bump_free(A.a); bump_free(A.slot); bump_free(lv); bump_free(runs); goto out;
    }
    /* (each item's inline boxes, innermost first: chain[ch0[i], ch0[i + 1])) */
    int *ch0 = (int *)bump_alloc(((size_t)nitems + 1) * sizeof(int));
    int chcap = 64, chn = 0;
    int *chain_all = (int *)bump_alloc((size_t)chcap * sizeof(int));
    if (!ch0 || !chain_all) { bump_free(lv); goto out; }
    for (int i = 0; i < nitems; i++) {
        ch0[i] = chn;
        for (int p = ctx->tree.nodes[items[i]].parent_idx; p >= 0 && p != idx; p = ctx->tree.nodes[p].parent_idx) {
            int a = bidi_anc(&A, p, true);
            if (a < 0) continue;
            if (A.a[a].first < 0) A.a[a].first = i;
            A.a[a].last = i;
            if (chn == chcap) {
                chcap *= 2;
                chain_all = (int *)bump_realloc(chain_all, (size_t)chcap * sizeof(int));
                if (!chain_all) { bump_free(lv); goto out; }
            }
            chain_all[chn++] = a;
        }
    }
    ch0[nitems] = chn;
    for (int k = 0; k < A.n; k++) {
        const LayoutBox *pb = layout_box(ctx, A.a[k].box);
        lead[A.a[k].first] += pb->margin_left + pb->s->border_left + pb->padding_left;
        trail[A.a[k].last] += pb->margin_right + pb->s->border_right + pb->padding_right;
    }

    int line = -1;
    double prev_end = -1e30, prev_trail = 0.0, prev_top = -1e30;
    int line_cap = 64;
    int *line_start = (int *)bump_alloc((size_t)line_cap * sizeof(int));   /* first run of each line */
    int *line_cp0 = (int *)bump_alloc((size_t)line_cap * sizeof(int)), *line_cp1 = (int *)bump_alloc((size_t)line_cap * sizeof(int));
    double *line_x = (double *)bump_alloc((size_t)line_cap * sizeof(double));   /* where its first run with content starts (1e30: none) */
    if (!line_start || !line_cp0 || !line_cp1 || !line_x) {
        bump_free(lv); bump_free(runs); bump_free(line_start); bump_free(line_cp0); bump_free(line_cp1); bump_free(line_x); goto out;
    }
    for (int i = 0; i < nitems; i++) {
        int b = items[i];
        LayoutBox *bb = layout_box(ctx, b);
        const LayoutTextFragList *fl = ctx->text_frags && b < ctx->text_frags_n ? &ctx->text_frags[b] : NULL;
        HtmlNode *in = css_layout_box_node(ctx, b);
        bool text = in && in->type == HTML_NODE_TEXT;
        int nf = text ? (fl ? fl->n : 0) : 1;
        int cpos = first_cp[i];   /* (where the next fragment's code points are searched from) */
        for (int q = 0; q < nf; q++) {
            double x0 = text ? bb->x + fl->f[q].dx : bb->x - bb->margin_left;
            double w = text ? fl->f[q].w : bb->margin_left + bb->width + bb->margin_right;
            /* (a new line: below the item before and back to the left; the
             * flow overlaps boxes where it collapses a space between them) */
            double top = text ? bb->y + fl->f[q].dy : bb->y - bb->margin_top;
            /* (an empty run, a collapsed space: where the flow left it says
             * nothing; it neither starts a line nor places one) */
            bool empty = text && w <= 0.001;
            if (line < 0 || (text && q > 0) || (!empty && x0 < prev_end - 0.5 && top > prev_top + 1.0)) {
                if (++line == line_cap) {
                    line_cap *= 2;
                    line_start = (int *)bump_realloc(line_start, (size_t)line_cap * sizeof(int));
                    line_cp0 = (int *)bump_realloc(line_cp0, (size_t)line_cap * sizeof(int));
                    line_cp1 = (int *)bump_realloc(line_cp1, (size_t)line_cap * sizeof(int));
                    line_x = (double *)bump_realloc(line_x, (size_t)line_cap * sizeof(double));
                    if (!line_start || !line_cp0 || !line_cp1 || !line_x) { line = -1; break; }
                }
                line_start[line] = nr;
                line_cp0[line] = -1;
                line_x[line] = 1e30;
            }
            if (nr == cap) {
                cap *= 2;
                BidiRun *nrs = (BidiRun *)bump_realloc(runs, (size_t)cap * sizeof(BidiRun));
                if (!nrs) break;
                runs = nrs;
            }
            BidiRun *r = &runs[nr++];
            r->item = i;
            r->box = b;
            r->frag = text ? q : -1;
            r->start = text ? fl->f[q].start : 0;
            r->end = text ? fl->f[q].end : 0;
            r->x0 = x0;
            r->w = w;
            r->lead = q == 0 ? lead[i] : 0.0;
            r->trail = q == nf - 1 ? trail[i] : 0.0;
            if (empty) {
                r->gap = 0.0;
            } else if (line_x[line] >= 1e29) {
                r->gap = 0.0;
                line_x[line] = x0 - r->lead;
            } else {
                r->gap = x0 - prev_end - prev_trail - r->lead;   /* (negative: the flow overlapped a collapsed space) */
            }
            if (!empty) {
                prev_end = x0 + w;
                prev_trail = r->trail;
                prev_top = top;
            }
            /* (its code points) */
            int c0 = cpos, c1 = c0;
            if (text) {
                while (c0 < P.n && P.box[c0] == b && P.byte[c0] < r->start) c0++;
                c1 = c0;
                while (c1 < P.n && P.box[c1] == b && P.byte[c1] < r->end) c1++;
                cpos = c1;
            } else {
                c1 = c0 + 1;
            }
            r->cp0 = c0;
            if (line_cp0[line] < 0) line_cp0[line] = c0;
            line_cp1[line] = c1;
        }
        if (line < 0) break;
    }
    int nlines = line + 1;
    /* (each inline box's first and last line) */
    for (int L = 0; L < nlines; L++) {
        int r1 = L + 1 < nlines ? line_start[L + 1] : nr;
        for (int k = line_start[L]; k < r1; k++)
            for (int q = ch0[runs[k].item]; q < ch0[runs[k].item + 1]; q++) {
                int a = chain_all[q];
                if (A.a[a].lfirst < 0) A.a[a].lfirst = L;
                A.a[a].llast = L;
            }
    }
    BidiPieceRec *bp = NULL;
    int nbp = 0, bpcap = 0;

    /* L1 per line, then each run cut where the level changes, and where the
     * white space at the line's end starts: that hangs (takes no room, as
     * the flow's trailing spaces do not in a browser). */
    int *line_hang = (int *)bump_alloc(((size_t)nlines + 1) * sizeof(int));
    if (!line_hang) { bump_free(lv); bump_free(runs); bump_free(line_start); bump_free(line_cp0); bump_free(line_cp1); goto out; }
    for (int L = 0; L < nlines; L++) {
        line_hang[L] = line_cp1[L];
        if (line_cp0[L] < 0 || line_cp1[L] <= line_cp0[L]) continue;
        bidi_line_levels(P.cp, line_cp0[L], line_cp1[L], para, lv);
        for (int c = line_cp1[L] - 1; c >= line_cp0[L]; c--) {
            uint32_t ch = P.cp[c];
            int k = bidi_class(ch);
            if (!(ch == ' ' || ch == '\t' || ch == '\n' || k == BIDI_BN || P.box[c] < 0)) break;
            line_hang[L] = c;
        }
    }

    /* (per item, its fragments in visual order) */
    LayoutTextFragList *nf_lists = (LayoutTextFragList *)bump_calloc((size_t)nitems, sizeof(LayoutTextFragList));
    BidiRun *cut = NULL;
    int ncut = 0, cutcap = 0;
    uint8_t *clv = NULL;
    int *order = NULL;
    for (int L = 0; L < nlines && nf_lists; L++) {
        int r0 = line_start[L], r1 = L + 1 < nlines ? line_start[L + 1] : nr;
        ncut = 0;
        bool wrapped = false;
        int wbox = -1, wfrag = -1;
        int ctl_c = line_cp0[L] >= 0 ? line_cp0[L] : 0;   /* (the controls not yet passed) */
        for (int k = r0; k < r1; k++) {
            BidiRun r = runs[k];
            if (ncut + 64 >= cutcap) {
                cutcap = cutcap ? cutcap * 2 : 128;
                while (ncut + 64 >= cutcap) cutcap *= 2;
                cut = (BidiRun *)bump_realloc(cut, (size_t)cutcap * sizeof(BidiRun));
                if (!cut) break;
            }
            /* (an isolate's or embedding's controls before it: zero-width
             * runs at their levels, which keep runs apart when reordered) */
            for (; ctl_c < r.cp0; ctl_c++) {
                if (P.box[ctl_c] >= 0) continue;
                if (ncut + 1 >= cutcap) {
                    cutcap *= 2;
                    cut = (BidiRun *)bump_realloc(cut, (size_t)cutcap * sizeof(BidiRun));
                    if (!cut) break;
                }
                BidiRun cr;
                memset(&cr, 0, sizeof(cr));
                cr.box = -1;
                cr.item = -1;
                cr.frag = -2;
                cr.cp0 = ctl_c;
                cr.level = lv[ctl_c];
                cr.hang = ctl_c >= line_hang[L];
                cut[ncut++] = cr;
            }
            if (!cut) break;
            if (ctl_c <= r.cp0) ctl_c = r.cp0 + 1;
            while (ctl_c < P.n && P.box[ctl_c] == r.box) ctl_c++;
            if (r.frag < 0) {
                r.level = lv[r.cp0];
                r.hang = 0;
                cut[ncut++] = r;
                continue;
            }
            LayoutBox *bb = layout_box(ctx, r.box);
            if (bb->wrap_cont_w > 0.0 && !wrapped) { wrapped = true; wbox = r.box; wfrag = r.frag; }
            /* (the run's code points, by level) */
            int c = r.cp0;
            uint32_t seg = r.start;
            int level = c < P.n && P.box[c] == r.box ? lv[c] : para;
            bool hang = c >= line_hang[L];
            bool first_piece = true;
            for (;;) {
                bool end = c >= P.n || P.box[c] != r.box || P.byte[c] >= r.end;
                if (end || lv[c] != level || (c >= line_hang[L]) != hang) {
                    uint32_t segend = end ? r.end : P.byte[c];
                    if (segend > seg) {
                        if (ncut + 1 >= cutcap) {
                            cutcap *= 2;
                            cut = (BidiRun *)bump_realloc(cut, (size_t)cutcap * sizeof(BidiRun));
                            if (!cut) break;
                        }
                        BidiRun pc = r;
                        pc.start = seg;
                        pc.end = segend;
                        pc.level = (unsigned char)level;
                        pc.gap = first_piece ? r.gap : 0.0;
                        pc.lead = first_piece ? r.lead : 0.0;
                        pc.trail = 0.0;
                        pc.hang = hang;
                        first_piece = false;
                        cut[ncut++] = pc;
                    }
                    if (end) break;
                    seg = P.byte[c];
                    level = lv[c];
                    hang = c >= line_hang[L];
                }
                c++;
            }
            if (!cut) break;
            if (!first_piece) cut[ncut - 1].trail = r.trail;
        }
        if (!cut) break;
        /* (a run that is the whole fragment keeps its width; a fragment cut
         * in pieces has all but its longest measured, and that one is the
         * rest of the fragment's width) */
        for (int k = 0; k < ncut;) {
            BidiRun *pc = &cut[k];
            int k1 = k + 1;
            if (pc->frag < 0) { k = k1; continue; }
            const LayoutTextFrag *fr = &ctx->text_frags[pc->box].f[pc->frag];
            while (k1 < ncut && cut[k1].box == pc->box && cut[k1].frag == pc->frag) k1++;
            if (k1 - k < 2) { k = k1; continue; }
            TextShaper *font;
            float scale, adv;
            const char *t;
            if (!css_layout_text_paint(ctx, pc->box, &font, &scale, &adv, &t)) { k = k1; continue; }
            int longest = k;
            for (int q = k + 1; q < k1; q++)
                if (cut[q].end - cut[q].start > cut[longest].end - cut[longest].start) longest = q;
            const LayoutBox *bb = layout_box(ctx, pc->box);
            text_shaper_set_spacing(bb->s->letter_spacing, bb->s->word_spacing, scale);
            double rest = fr->w;
            for (int q = k; q < k1; q++) {
                if (q == longest) continue;
                cut[q].w = text_shaper_span_width(font, t, cut[q].start, cut[q].end, scale);
                rest -= cut[q].w;
            }
            text_shaper_set_spacing(0.0f, 0.0f, 1.0f);
            cut[longest].w = rest > 0.0 ? rest : 0.0;
            k = k1;
        }
        double hang_w = 0.0;
        for (int k = 0; k < ncut; k++)
            if (cut[k].hang) {
                hang_w += cut[k].w;
                cut[k].w = 0.0;
            }
        clv = (uint8_t *)bump_realloc(clv, (size_t)ncut + 1);
        order = (int *)bump_realloc(order, ((size_t)ncut + 1) * sizeof(int));
        if (!clv || !order) break;
        for (int k = 0; k < ncut; k++) clv[k] = cut[k].level;
        bidi_reorder(clv, ncut, order);
        double x = line_x[L] < 1e29 ? line_x[L] : runs[r0].x0 - runs[r0].lead, used = 0.0;
        for (int k = 0; k < ncut; k++) used += cut[k].gap + cut[k].lead + cut[k].w + cut[k].trail;
        /* (a line with wrapped text: aligned here, against the wrap's line) */
        if (wrapped && blk->s->text_align != CSS_TEXT_ALIGN_LEFT) {
            const LayoutBox *wb = layout_box(ctx, wbox);
            double right;
            if (wfrag == 0) right = wb->x + wb->wrap_first_w;
            else if (wb->wrap_cont2_line > 1 && wfrag + 1 >= wb->wrap_cont2_line) right = wb->wrap_cont2_x + wb->wrap_cont2_w;
            else right = wb->wrap_cont_x + wb->wrap_cont_w;
            double slack = right - x - used;
            if (slack > 0.5) x += blk->s->text_align == CSS_TEXT_ALIGN_CENTER ? slack * 0.5 : slack;
        }
        (void)hang_w;   /* (the flow aligned the other lines without it) */
        /* (each inline box's leftmost and rightmost run on the line) */
        for (int k = 0; k < ncut; k++) {
            const BidiRun *pc = &cut[order[k]];
            if (pc->frag == -2 || pc->hang) continue;
            for (int q = ch0[pc->item]; q < ch0[pc->item + 1]; q++) {
                BidiAnc *e = &A.a[chain_all[q]];
                if (e->lm_line != L) {
                    e->lm_line = L;
                    e->lm = k;
                    e->open = false;
                }
                e->rm = k;
            }
        }
        /* Placed left to right: an inline box's pieces are its stretches of
         * visually adjacent runs; its left edge is on the leftmost run of its
         * first line and its right edge on the rightmost of its last (the
         * right edge first, the left last, when it is right to left: CSS 2.1
         * section 8.6), opened outermost first and closed innermost first. */
        for (int k = 0; k < ncut; k++) {
            BidiRun *pc = &cut[order[k]];
            if (pc->frag == -2) continue;
            x += pc->gap;
            const int *chain = chain_all + ch0[pc->item];
            int nch = pc->hang ? 0 : ch0[pc->item + 1] - ch0[pc->item];
            double ty = pc->frag >= 0 ? layout_box(ctx, pc->box)->y + ctx->text_frags[pc->box].f[pc->frag].dy : 0.0;
            for (int c = nch - 1; c >= 0; c--) {
                BidiAnc *e = &A.a[chain[c]];
                if (pc->frag >= 0 && e->ty_line != L) {
                    e->ty_line = L;
                    e->ty = ty;
                    if (L == e->lfirst && !e->has_base) {
                        e->has_base = true;
                        e->base = ty;
                    }
                }
                if (e->open) continue;
                const LayoutBox *eb = layout_box(ctx, e->box);
                bool left = k == e->lm && L == (eb->s->direction == 1 ? e->llast : e->lfirst);
                if (left) x += eb->margin_left;
                e->open = true;
                e->ps = x;
                e->psides = left ? 1 : 0;
                if (left) x += eb->s->border_left + eb->padding_left;
            }
            if (pc->frag < 0) {
                double dx = x - pc->x0;
                if (fabs(dx) > 0.001) {
                    LayoutBox *ab = layout_box(ctx, pc->box);
                    ab->x += dx;
                    layout_offset_subtree(ctx, pc->box, dx, 0.0);
                }
            } else {
                const LayoutTextFrag *fr = &ctx->text_frags[pc->box].f[pc->frag];
                LayoutTextFragList *dst = &nf_lists[pc->item];
                LayoutTextFrag *nfr = (LayoutTextFrag *)bump_realloc(dst->f, ((size_t)dst->n + 1) * sizeof(LayoutTextFrag));
                if (nfr) {
                    dst->f = nfr;
                    LayoutTextFrag *o = &dst->f[dst->n++];
                    *o = *fr;
                    o->start = pc->start;
                    o->end = pc->end;
                    o->dx = x - layout_box(ctx, pc->box)->x;
                    o->w = pc->w;
                    o->level = pc->level;
                }
            }
            if (!pc->hang) {
                bool whole = pc->frag < 0 || (pc->start == ctx->text_frags[pc->box].f[pc->frag].start &&
                                              pc->end == ctx->text_frags[pc->box].f[pc->frag].end);
                for (int q = ch0[pc->item]; q < ch0[pc->item + 1]; q++) {
                    BidiAnc *e = &A.a[chain_all[q]];
                    if (e->line == -2) {
                        e->line = L;
                        e->o0 = e->n0 = 1e30;
                        e->o1 = e->n1 = -1e30;
                    } else if (e->line != L) {
                        e->line = -1;
                    }
                    if (!whole) e->line = -1;
                    double ox = pc->x0;
                    if (ox < e->o0) e->o0 = ox;
                    if (ox + pc->w > e->o1) e->o1 = ox + pc->w;
                    if (x < e->n0) e->n0 = x;
                    if (x + pc->w > e->n1) e->n1 = x + pc->w;
                }
            }
            x += pc->w;
            if (nch == 0) continue;
            int nx = k + 1;
            while (nx < ncut && (cut[order[nx]].frag == -2 || cut[order[nx]].hang)) nx++;
            const int *nchain = nx < ncut ? chain_all + ch0[cut[order[nx]].item] : NULL;
            int nnch = nx < ncut ? ch0[cut[order[nx]].item + 1] - ch0[cut[order[nx]].item] : 0;
            for (int c = 0; c < nch; c++) {
                BidiAnc *e = &A.a[chain[c]];
                bool next_in = false;
                for (int q = 0; q < nnch && !next_in; q++) next_in = nchain[q] == chain[c];
                if (next_in) continue;
                const LayoutBox *eb = layout_box(ctx, e->box);
                bool right = k == e->rm && L == (eb->s->direction == 1 ? e->lfirst : e->llast);
                if (right) x += eb->padding_right + eb->s->border_right;
                if (nbp == bpcap) {
                    bpcap = bpcap ? bpcap * 2 : 32;
                    BidiPieceRec *nb = (BidiPieceRec *)bump_realloc(bp, (size_t)bpcap * sizeof(BidiPieceRec));
                    if (!nb) { bpcap = nbp; e->open = false; continue; }
                    bp = nb;
                }
                BidiPieceRec *rec = &bp[nbp++];
                rec->a = chain[c];
                rec->pc.anchor = -1;
                rec->pc.sides = (unsigned char)(e->psides | (right ? 2 : 0));
                rec->pc.dx = e->ps;
                rec->pc.w = x - e->ps;
                if (e->lfirst == e->llast) {
                    rec->pc.dy = eb->y;
                    rec->pc.h = eb->height;
                } else {
                    /* (a line's piece: as tall as one line of it, as far down
                     * from its first line's as its text) */
                    double ca = layout_content_area_height(eb);
                    rec->pc.dy = eb->y + (e->ty_line == L && e->has_base ? e->ty - e->base : 0.0);
                    rec->pc.h = ca + eb->padding_top + eb->padding_bottom + eb->s->border_top + eb->s->border_bottom;
                }
                e->np++;
                if (right) x += eb->margin_right;
                e->open = false;
            }
        }
    }
    for (int k = 0; k < A.n; k++) {
        const BidiAnc *e = &A.a[k];
        if (e->line < 0 || fabs((e->n1 - e->n0) - (e->o1 - e->o0)) > 0.5) continue;
        double d = e->n0 - e->o0;
        if (fabs(d) > 0.001) layout_box(ctx, e->box)->x += d;
    }
    /* (an inline box in more than one piece, or on more than one line, is
     * painted as its pieces; one split around blocks has its own) */
    for (int k = 0; k < A.n; k++) {
        BidiAnc *e = &A.a[k];
        e->keep = !((e->np < 2 && e->lfirst == e->llast) || layout_box(ctx, e->box)->s->block_in_inline == 2);
    }
    for (int q = 0; q < nbp; q++) {
        BidiAnc *e = &A.a[bp[q].a];
        const LayoutBox *eb = layout_box(ctx, e->box);
        if (!e->keep) continue;
        if (!ctx->pieces) {
            ctx->pieces = (LayoutPieceList *)calloc((size_t)ctx->tree.count, sizeof(LayoutPieceList));
            if (!ctx->pieces) break;
            ctx->pieces_n = ctx->tree.count;
        }
        if (e->box >= ctx->pieces_n) continue;
        LayoutPieceList *pl = &ctx->pieces[e->box];
        if (e->np >= 0) {   /* (its first piece here: the list starts over) */
            pl->n = 0;
            e->np = -1;
        }
        if (pl->n == pl->cap) {
            int nc = pl->cap ? 2 * pl->cap : 4;
            LayoutPiece *np = (LayoutPiece *)realloc(pl->p, (size_t)nc * sizeof(LayoutPiece));
            if (!np) continue;
            pl->p = np;
            pl->cap = nc;
        }
        LayoutPiece pc = bp[q].pc;
        pc.dx -= eb->x;
        pc.dy -= eb->y;
        pl->p[pl->n++] = pc;
    }
    bump_free(bp);
    bump_free(A.a);
    bump_free(A.slot);
    if (nf_lists) {
        for (int k = 0; k < nitems; k++) {
            if (!nf_lists[k].f) continue;
            LayoutTextFragList *own = &ctx->text_frags[items[k]];
            if (nf_lists[k].n > own->n) {
                LayoutTextFrag *g = (LayoutTextFrag *)realloc(own->f, (size_t)nf_lists[k].n * sizeof(LayoutTextFrag));
                if (!g) continue;
                own->f = g;
            }
            memcpy(own->f, nf_lists[k].f, (size_t)nf_lists[k].n * sizeof(LayoutTextFrag));
            own->n = nf_lists[k].n;
        }
        bump_free(nf_lists);
    }
    bump_free(cut);
    bump_free(clv);
    bump_free(order);
    bump_free(runs);
    bump_free(line_start);
    bump_free(line_cp0);
    bump_free(line_cp1);
    bump_free(line_x);
    bump_free(line_hang);
    bump_free(lead);
    bump_free(trail);
    bump_free(lv);
out:
    bump_free(items);
    bump_free(first_cp);
    bump_free(P.cp);
    bump_free(P.box);
    bump_free(P.byte);
}

/* Block idx's paragraph, its lines placed: when it needs bidi and has
 * something reordered, its text boxes' fragments in visual order (and it is
 * laid out again at the next pass, not reused, as a split inline's block
 * is); left as the flow placed it otherwise.  CYBER_BIDI_ALL=1: fragments
 * for every paragraph. */
static void layout_bidi_paragraph(LayoutContext *ctx, int idx)
{
    if (g_bidi_all < 0) g_bidi_all = getenv("CYBER_BIDI_ALL") != NULL;
    bool needs = layout_box(ctx, idx)->s->direction == 1 || layout_box(ctx, idx)->s->unicode_bidi != 0 ||
                 layout_paragraph_walk(ctx, idx, NULL);
    if (!g_bidi_all && !needs) return;
    if (!ctx->text_frags) {
        ctx->text_frags = (LayoutTextFragList *)calloc((size_t)ctx->tree.count, sizeof(LayoutTextFragList));
        if (!ctx->text_frags) return;
        ctx->text_frags_n = ctx->tree.count;
    }
    if (needs) {
        layout_bidi_reorder(ctx, idx);   /* (fragments only where it reorders) */
    } else {
        layout_flow_noreuse_up(ctx, idx);
        layout_paragraph_walk(ctx, idx, layout_text_frags_build);
    }
}

bool css_layout_text_paint(const LayoutContext *ctx, int idx, TextShaper **font, float *scale, float *line_adv,
                           const char **text)
{
    const LayoutBox *b = &ctx->boxes[idx];
    HtmlNode *n = css_layout_box_node(ctx, idx);
    const char *raw = n ? css_layout_node_text(ctx, n) : NULL;
    if (!raw) return false;
    const char *t = css_layout_box_text(ctx, idx, raw);
    if (!t) return false;
    if (b->text_lead_skip && t[0] == ' ') t++;
    double fs = b->s->font_size > 0.0 ? b->s->font_size : 16.0;
    TextShaper *f = display_list_get_font(layout_font_slot(b));
    if (!f) f = display_list_get_default_font();
    if (!f) return false;
    float nlh = text_shaper_normal_line_height(f, (float)fs);
    if (!(nlh > 0.5f * fs && nlh < 3.0f * fs)) nlh = (float)(fs * 1.5);
    *font = f;
    *scale = (float)(fs / 16.0);
    *line_adv = b->s->line_height > 0.0 ? (float)b->s->line_height : nlh;
    *text = t;
    return true;
}

/* ---- Inline formatting on items (INLINE_LAYOUT_DESIGN.md, step 3) ----
 * A block whose content is only inline is laid out from a list of tokens
 * (each text box cut into words and collapsible spaces, white space
 * collapsed across boxes as CSS Text 3 section 4.1.1 does), broken into
 * lines greedily at spaces, each line placed from its start (the float-free
 * room at its top) and aligned; each text box gets its fragments per line
 * (painted from them) and an outline around them, each inline element the
 * outline of its content (and pieces per line when it is on more than one).
 * On by default (CYBER_IFC=0: off); while it covers only part of what the flow does, a block it
 * cannot lay out (ifc_eligible) takes the flow's per-child path. */
static int g_ifc = -1;
static bool layout_has_gen(LayoutContext *ctx, int i);

typedef struct {
    int box;              /* text box; the element of an edge; the box a mark or break stands for */
    uint32_t start, end;  /* bytes in the text box's painted text */
    double w, x;
    unsigned char kind;   /* IFC_WORD, IFC_SPACE, ... */
    unsigned char drop;   /* a space removed at a line's start */
    unsigned char nobrk;  /* a space that is not a break opportunity (nowrap) */
    unsigned char kept;   /* a space white-space keeps (pre-wrap): not removed at a line's ends */
    unsigned char level;  /* its bidi level (a paragraph that needs bidi) */
    int line;
    int cp0, cp1;         /* its code points in the paragraph's (when gathered) */
} IfcTok;

enum {
    IFC_WORD,     /* a piece of a word (a word across boxes is several) */
    IFC_SPACE,    /* a collapsible space: the break opportunity */
    IFC_MARK,     /* where a box with nothing kept is */
    IFC_OPEN,     /* an inline element's start edge: margin, border, padding */
    IFC_CLOSE,    /* its end edge */
    IFC_BR,       /* a forced line break */
    IFC_ATOM,     /* an atomic inline or image: its margin box, laid out already */
    IFC_FLOAT,    /* a float: placed where its line allows (no width on the line) */
    IFC_CTL,      /* a bidi control unicode-bidi stands for: no width, keeps runs apart when reordered */
    IFC_ABS       /* an absolutely positioned box: no width, where its static position is */
};

typedef struct {
    int box, tok0, tok1;  /* an inline element and its tokens (its edges included) */
} IfcEl;

typedef struct {
    IfcTok *t;
    int n, cap;
    IfcEl *el;
    int nel, elcap;
    bool space_before;    /* the last text was a collapsible space (or nothing yet) */
    double x0, y0;        /* where atomic inlines are laid out before their line is known */
    /* The paragraph's code points, gathered when it needs bidi (as
     * bidi_para_collect gathers them): the text boxes' text, U+FFFC per
     * atomic inline, U+2029 per line break element, the controls
     * unicode-bidi stands for; each with its byte in its text box. */
    bool gather;
    uint32_t *cp, *cbyte;
    int ncp, cpcap;
    const char *bt;       /* the text box being cut into tokens (NULL: none) */
    uint32_t bcur;        /* its bytes gathered so far */
} IfcItems;

static bool ifc_cp_reserve(IfcItems *I, int more)
{
    if (I->ncp + more <= I->cpcap) return true;
    int nc = I->cpcap ? I->cpcap : 256;
    while (nc < I->ncp + more) nc *= 2;
    uint32_t *a = (uint32_t *)realloc(I->cp, (size_t)nc * sizeof(uint32_t));
    if (a) I->cp = a;
    uint32_t *b = a ? (uint32_t *)realloc(I->cbyte, (size_t)nc * sizeof(uint32_t)) : NULL;
    if (b) I->cbyte = b;
    if (!a || !b) {
        I->gather = false;   /* (out of memory: no bidi for it) */
        return false;
    }
    I->cpcap = nc;
    return true;
}

static void ifc_cp_put(IfcItems *I, uint32_t cp, uint32_t byte)
{
    if (!I->gather || !ifc_cp_reserve(I, 1)) return;
    I->cp[I->ncp] = cp;
    I->cbyte[I->ncp++] = byte;
}

/* The text box's code points up to byte to; the count gathered. */
static int ifc_cp_upto(IfcItems *I, uint32_t to)
{
    if (!I->gather || !I->bt) return I->ncp;
    const unsigned char *t = (const unsigned char *)I->bt;
    while (I->bcur < to && I->gather) {
        const unsigned char *p = t + I->bcur;
        uint32_t off = I->bcur;
        uint32_t c = *p < 0x80 ? *p++ : bidi_utf8_next(&p, t + to);
        I->bcur = (uint32_t)(p - t);
        ifc_cp_put(I, c, off);
    }
    return I->ncp;
}

static bool ifc_put(IfcItems *I, int box, uint32_t a, uint32_t b, double w, int kind)
{
    if (I->n == I->cap) {
        int nc = I->cap ? I->cap * 2 : 64;
        IfcTok *t = (IfcTok *)realloc(I->t, (size_t)nc * sizeof(IfcTok));
        if (!t) return false;
        I->t = t;
        I->cap = nc;
    }
    IfcTok *k = &I->t[I->n++];
    memset(k, 0, sizeof(*k));
    k->box = box;
    k->start = a;
    k->end = b;
    k->w = w;
    k->x = I->x0;   /* (where the content starts, for one on no line) */
    k->kind = (unsigned char)kind;
    if (I->gather) {
        k->cp0 = ifc_cp_upto(I, a);
        k->cp1 = ifc_cp_upto(I, b);
    }
    return true;
}

static double ifc_width(LayoutContext *ctx, int box, const char *t, uint32_t a, uint32_t b)
{
    const LayoutBox *bb = layout_box(ctx, box);
    TextShaper *font = display_list_get_font(layout_font_slot(bb));
    if (!font) font = display_list_get_default_font();
    if (!font || b <= a) return 0.0;
    double fs = bb->s->font_size > 0.0 ? bb->s->font_size : 16.0;
    float scale = (float)(fs / 16.0);
    text_shaper_set_spacing(bb->s->letter_spacing, bb->s->word_spacing, scale);
    double w = text_shaper_span_width(font, t, a, b, scale);
    text_shaper_set_spacing(0.0f, 0.0f, 1.0f);
    return w;
}

static bool ifc_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static bool ifc_is_br(HtmlNode *n)
{
    return n && n->type == HTML_NODE_ELEMENT && n->tag_name && !strcasecmp(n->tag_name, "br");
}

static bool ifc_is_img(HtmlNode *n)
{
    return n && n->type == HTML_NODE_ELEMENT && n->tag_name && !strcasecmp(n->tag_name, "img");
}

/* A box this path places whole on a line: an atomic inline (inline-block,
 * inline-flex, inline-grid, inline-table, a control) or an inline image. */
static bool ifc_is_atom(const LayoutBox *b, HtmlNode *n)
{
    return n && n->type == HTML_NODE_ELEMENT &&
           (layout_box_is_atomic_inline(b) || (ifc_is_img(n) && b->s->display == CSS_DISPLAY_INLINE));
}

/* Whether box p's in-flow children, and theirs inside inline elements, are
 * what this path lays out: text in any white-space, inline elements (and
 * ::before / ::after), line breaks, atomic inlines and images, and (top: the
 * block's own) floats, in any direction, absolutely positioned boxes at any
 * depth (left out: placed after, from their static position); no
 * ::first-letter box (its own sizing, the flow's for now), no float inside an
 * inline element. */
static bool ifc_is_float(const LayoutBox *b)
{
    return b->s->float_side != 0 && b->s->position != CSS_POSITION_ABSOLUTE && b->s->position != CSS_POSITION_FIXED &&
           (layout_is_block_flow(b->s->display) || b->s->display == CSS_DISPLAY_INLINE_BLOCK);
}

static bool ifc_inline_ok(LayoutContext *ctx, int p, bool top)
{
    for (int c = ctx->tree.nodes[p].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        LayoutBox *cb = layout_box(ctx, c);
        if (cb->s->display == CSS_DISPLAY_NONE) continue;
        if (!layout_is_in_flow(cb)) continue;   /* (absolutely positioned: placed after, from its static position) */
        if (top && ifc_is_float(cb)) continue;   /* (the block's own floats; floats deeper in, for now, not) */
        HtmlNode *n = css_layout_box_node(ctx, c);
        if (!n) return false;
        if (ifc_is_atom(cb, n)) continue;   /* (what it holds is its own) */
        if (n->type == HTML_NODE_TEXT) continue;
        if (n->type != HTML_NODE_ELEMENT || cb->s->display != CSS_DISPLAY_INLINE || cb->s->block_in_inline == 1) return false;
        if (ifc_is_br(n)) continue;
        if (!n->tag_name || (n->tag_name[0] == ':' && strcmp(n->tag_name, "::before") && strcmp(n->tag_name, "::after")) ||
            layout_is_replaced_tag(n->tag_name) ||
            !strcasecmp(n->tag_name, "img") || !strcasecmp(n->tag_name, "wbr") || layout_is_svg_box(n))
            return false;
        if (!ifc_inline_ok(ctx, c, false)) return false;
    }
    return true;
}

static bool ifc_eligible(LayoutContext *ctx, int idx)
{
    if (g_ifc < 0) {
        const char *e = getenv("CYBER_IFC");
        g_ifc = !(e && e[0] == '0');   /* (CYBER_IFC=0: the flow's per-child path for all) */
    }
    if (!g_ifc) return false;
    const LayoutBox *b = layout_box(ctx, idx);
    if (b->s->line_clamp || b->s->text_overflow_ellipsis) return false;
    if (ctx->tree.nodes[idx].first_child_idx < 0) return false;
    return ifc_inline_ok(ctx, idx, true);
}

/* The tokens of p's inline content into I. */
static void ifc_collect(LayoutContext *ctx, int p, IfcItems *I, double avail_width, double pct_h)
{
    for (int c = ctx->tree.nodes[p].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        LayoutBox *cb = layout_box(ctx, c);
        if (cb->s->display == CSS_DISPLAY_NONE) continue;
        if (!layout_is_in_flow(cb)) {
            /* (out of the line, the spaces around it collapsing together:
             * where it is, for its static position) */
            cb->has_static = 0;
            ifc_put(I, c, 0, 0, 0.0, IFC_ABS);
            continue;
        }
        HtmlNode *n = css_layout_box_node(ctx, c);
        layout_resolve_used_sizes(cb, layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx), avail_width, pct_h);
        if (ifc_is_float(cb)) {
            /* (out of the line: the spaces around it collapse together) */
            ifc_put(I, c, 0, 0, 0.0, IFC_FLOAT);
            continue;
        }
        cb->text_lead_skip = 0;
        cb->wrap_first_w = cb->wrap_cont_w = cb->wrap_cont_x = cb->wrap_cont2_x = cb->wrap_cont2_w = 0.0;
        cb->wrap_cont2_line = 0;
        int n0 = I->n;
        if (n->type == HTML_NODE_TEXT) {
            const char *raw = css_layout_node_text(ctx, n);
            const char *t = raw ? css_layout_box_text(ctx, c, raw) : NULL;
            uint32_t len = t ? (uint32_t)strlen(t) : 0;
            double sw = -1.0;
            /* (white-space: pre and pre-wrap keep spaces; they, and pre-line,
             * keep newlines as forced breaks; nowrap and pre do not wrap) */
            unsigned char ws = cb->s->white_space;
            bool keep_sp = ws == 2 || ws == 3, keep_nl = ws >= 2, wrap = ws != 1 && ws != 2;
            if (I->gather && ifc_cp_reserve(I, (int)len)) {
                I->bt = t;
                I->bcur = 0;
            }
            for (uint32_t i = 0; i < len;) {
                uint32_t j = i;
                if (keep_nl && t[i] == '\n') {
                    ifc_put(I, c, i, i + 1, 0.0, IFC_BR);
                    I->space_before = true;
                    i++;
                    continue;
                }
                if (ifc_space(t[i])) {
                    while (j < len && ifc_space(t[j]) && !(keep_nl && t[j] == '\n')) j++;
                    if (keep_sp) {
                        double w = ifc_width(ctx, c, t, i, j);
                        if (!wrap) {
                            ifc_put(I, c, i, j, w, IFC_WORD);   /* (pre: part of the line, no break) */
                        } else if (ifc_put(I, c, i, j, w, IFC_SPACE)) {
                            I->t[I->n - 1].kept = 1;
                        }
                        I->space_before = false;
                    } else if (!I->space_before) {
                        if (sw < 0.0) sw = ifc_width(ctx, c, " ", 0, 1);
                        if (ifc_put(I, c, i, j, sw, IFC_SPACE)) I->t[I->n - 1].nobrk = !wrap;
                        I->space_before = true;
                    } else if (wrap && I->n > 0 && I->t[I->n - 1].kind == IFC_SPACE) {
                        /* (a space of wrapping text collapsed into a nowrap one:
                         * the break opportunity is there still) */
                        I->t[I->n - 1].nobrk = 0;
                    }
                } else {
                    while (j < len && !ifc_space(t[j])) j++;
                    ifc_put(I, c, i, j, ifc_width(ctx, c, t, i, j), IFC_WORD);
                    I->space_before = false;
                }
                i = j;
            }
            ifc_cp_upto(I, len);
            if (I->n == n0) ifc_put(I, c, 0, 0, 0.0, IFC_MARK);   /* (nothing of it kept: where it is) */
            I->bt = NULL;
        } else if (ifc_is_br(n)) {
            ifc_cp_put(I, 0x2029, 0);   /* (a forced break ends a bidi paragraph) */
            ifc_put(I, c, 0, 0, 0.0, IFC_BR);
            if (I->gather) I->t[I->n - 1].cp0 = I->ncp - 1;
            I->space_before = true;
        } else if (ifc_is_atom(cb, n)) {
            /* (laid out here, at the content start: placed on its line later,
             * moved with what it holds) */
            if (!ifc_is_img(n)) layout_size_atomic_inline(ctx, c, avail_width);
            cb->x = I->x0 + cb->margin_left;
            cb->y = I->y0 + cb->margin_top;
            layout_update_content_sizes(cb);
            layout_node_serial(ctx, c);
            ifc_cp_put(I, 0xFFFC, 0);
            ifc_put(I, c, 0, 0, cb->margin_left + cb->width + cb->margin_right, IFC_ATOM);
            unsigned char pws = layout_box(ctx, p)->s->white_space;   /* (no break around it in nowrap or pre) */
            I->t[I->n - 1].nobrk = pws == 1 || pws == 2;
            if (I->gather) I->t[I->n - 1].cp0 = I->ncp - 1;
            I->space_before = false;
        } else {
            /* (an inline split around a line break by layout_blockify: this
             * path lays it out, its pieces are this path's) */
            if (cb->s->block_in_inline == 2) cb->s->block_in_inline = 0;
            if (ctx->pieces && c < ctx->pieces_n) ctx->pieces[c].n = 0;
            int e = I->nel;
            if (I->nel == I->elcap) {
                int nc = I->elcap ? I->elcap * 2 : 16;
                IfcEl *ne = (IfcEl *)realloc(I->el, (size_t)nc * sizeof(IfcEl));
                if (!ne) continue;
                I->el = ne;
                I->elcap = nc;
            }
            I->nel++;
            I->el[e].box = c;
            I->el[e].tok0 = I->n;
            /* (its start edge is its right one when it is right to left) */
            double lw = cb->margin_left + cb->s->border_left + cb->padding_left;
            double rw = cb->padding_right + cb->s->border_right + cb->margin_right;
            if (cb->s->direction == 1) {
                double sw = lw;
                lw = rw;
                rw = sw;
            }
            if (lw != 0.0) ifc_put(I, c, 0, 0, lw, IFC_OPEN);
            /* (the controls its unicode-bidi stands for: tokens of no width
             * at their place in the text, which keep runs apart) */
            uint32_t ctl[2];
            int nctl = I->gather ? bidi_controls(cb, true, ctl) : 0;
            for (int q = 0; q < nctl; q++) {
                ifc_cp_put(I, ctl[q], 0);
                if (ifc_put(I, -1, 0, 0, 0.0, IFC_CTL)) I->t[I->n - 1].cp0 = I->ncp - 1;
            }
            int inner = I->n;
            ifc_collect(ctx, c, I, avail_width, pct_h);
            if (I->n == inner && lw == 0.0 && rw == 0.0) ifc_put(I, c, 0, 0, 0.0, IFC_MARK);
            if (rw != 0.0) ifc_put(I, c, 0, 0, rw, IFC_CLOSE);
            I->el[e].tok1 = I->n;
            nctl = I->gather ? bidi_controls(cb, false, ctl) : 0;
            for (int q = 0; q < nctl; q++) {
                ifc_cp_put(I, ctl[q], 0);
                if (ifc_put(I, -1, 0, 0, 0.0, IFC_CTL)) I->t[I->n - 1].cp0 = I->ncp - 1;
            }
        }
    }
}

/* Intrinsic widths over p's inline content, as the line breaker sees it:
 * max-content the widest line between forced breaks (no spaces at its
 * ends), min-content the widest run without a break opportunity (a word
 * with the edges joined to it, an atomic inline). */
typedef struct {
    bool want_max, space_before;
    double line, pending, run, best;
    double run_sp;   /* (a nowrap space in the run: counted once content follows) */
    bool line_has;   /* (something on the line: a space after it counts) */
} IfcIntr;

static void ifc_intr_content(IfcIntr *S, double w)
{
    if (S->line_has) S->line += S->pending;
    S->pending = 0.0;
    S->line += w;
    S->line_has = true;
}

static void ifc_intr_break(IfcIntr *S)
{
    if (S->run > S->best) S->best = S->run;
    S->run = S->run_sp = 0.0;
}

static void ifc_intr_run(IfcIntr *S, double w)
{
    S->run += S->run_sp + w;
    S->run_sp = 0.0;
}

static void ifc_intr_walk(LayoutContext *ctx, int p, IfcIntr *S)
{
    for (int c = ctx->tree.nodes[p].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        LayoutBox *cb = layout_box(ctx, c);
        if (!layout_is_in_flow(cb)) continue;
        HtmlNode *n = css_layout_box_node(ctx, c);
        if (ifc_is_float(cb)) {
            double w = layout_intrinsic_contribution(ctx, c, S->want_max);
            if (S->want_max) S->line += w;
            else if (w > S->best) S->best = w;
            continue;
        }
        if (n->type == HTML_NODE_TEXT) {
            const char *raw = css_layout_node_text(ctx, n);
            const char *t = raw ? css_layout_box_text(ctx, c, raw) : NULL;
            uint32_t len = t ? (uint32_t)strlen(t) : 0;
            unsigned char ws = cb->s->white_space;
            bool keep_sp = ws == 2 || ws == 3, keep_nl = ws >= 2, wrap = ws != 1 && ws != 2;
            for (uint32_t i = 0; i < len;) {
                uint32_t j = i;
                if (keep_nl && t[i] == '\n') {
                    if (S->line > S->best && S->want_max) S->best = S->line;
                    S->line = S->pending = 0.0;
                    S->line_has = false;
                    S->space_before = true;
                    ifc_intr_break(S);
                    i++;
                    continue;
                }
                if (ifc_space(t[i])) {
                    while (j < len && ifc_space(t[j]) && !(keep_nl && t[j] == '\n')) j++;
                    if (keep_sp) {
                        double w = ifc_width(ctx, c, t, i, j);
                        if (!wrap) {
                            ifc_intr_content(S, w);
                            ifc_intr_run(S, w);
                        } else {
                            S->pending += w;
                            ifc_intr_break(S);
                        }
                        S->space_before = false;
                    } else if (!S->space_before) {
                        double w = ifc_width(ctx, c, " ", 0, 1);
                        if (wrap) {
                            S->pending = w;
                            ifc_intr_break(S);
                        } else {
                            S->pending = w;
                            S->run_sp = w;
                        }
                        S->space_before = true;
                    }
                } else {
                    while (j < len && !ifc_space(t[j])) j++;
                    double w = ifc_width(ctx, c, t, i, j);
                    ifc_intr_content(S, w);
                    ifc_intr_run(S, w);
                    S->space_before = false;
                }
                i = j;
            }
        } else if (ifc_is_br(n)) {
            if (S->line > S->best && S->want_max) S->best = S->line;
            S->line = S->pending = 0.0;
            S->line_has = false;
            S->space_before = true;
            ifc_intr_break(S);
        } else if (ifc_is_atom(cb, n)) {
            double w = layout_intrinsic_contribution(ctx, c, S->want_max);
            unsigned char pws = layout_box(ctx, p)->s->white_space;
            ifc_intr_content(S, w);
            if (pws == 1 || pws == 2) {
                ifc_intr_run(S, w);   /* (nowrap or pre: part of the run around it) */
            } else {
                ifc_intr_break(S);
                if (w > S->best && !S->want_max) S->best = w;
            }
            S->space_before = false;
        } else {
            double lw = cb->margin_left + cb->s->border_left + cb->padding_left;
            double rw = cb->padding_right + cb->s->border_right + cb->margin_right;
            if (lw != 0.0) { ifc_intr_content(S, lw); ifc_intr_run(S, lw); }
            ifc_intr_walk(ctx, c, S);
            if (rw != 0.0) { S->line += rw; S->line_has = true; S->run += rw; }
        }
    }
}

static double ifc_intrinsic(LayoutContext *ctx, int idx, bool want_max)
{
    IfcIntr S;
    memset(&S, 0, sizeof(S));
    S.want_max = want_max;
    S.space_before = true;
    ifc_intr_walk(ctx, idx, &S);
    if (want_max) return S.line > S.best ? S.line : S.best;
    ifc_intr_break(&S);
    return S.best;
}

/* The height a token's line takes from it, as the flow's line box takes it
 * from the item it is in (the block's child that holds it): the item's line
 * advance, and how far the item's own children reach (a text child its line
 * advance, an element child the bottom of its box, centred on the line by
 * the half-leading). */
static double ifc_tok_adv(LayoutContext *ctx, int idx, int box)
{
    int item = box, below = -1;
    for (int p = ctx->tree.nodes[box].parent_idx; p >= 0 && p != idx; p = ctx->tree.nodes[p].parent_idx) {
        below = item;
        item = p;
    }
    double a = layout_line_advance_box(layout_box(ctx, item));
    if (below >= 0) {
        const LayoutBox *bb = layout_box(ctx, below);
        HtmlNode *n = css_layout_box_node(ctx, below);
        double q = n && n->type == HTML_NODE_TEXT ? layout_line_advance_box(bb)
                                                  : (layout_line_advance_box(bb) + layout_content_area_height(bb)) / 2.0 +
                                                    bb->padding_bottom + bb->s->border_bottom;
        if (q > a) a = q;
    }
    return a;
}

/* Line L's height: from its words and the spaces it keeps; a line with
 * only a line break takes the break's (an empty line). */
static double ifc_line_height(LayoutContext *ctx, int idx, const IfcItems *I, int t0, int t1)
{
    double h = 0.0, br = 0.0;
    for (int k = t0; k < t1; k++) {
        const IfcTok *t = &I->t[k];
        if (t->kind == IFC_BR) {
            double a = ifc_tok_adv(ctx, idx, t->box);
            if (a > br) br = a;
        }
        if (t->kind == IFC_ATOM) {
            const LayoutBox *ab = layout_box(ctx, t->box);
            double a = ab->margin_top + ab->height + ab->margin_bottom;
            if (a > h) h = a;
            continue;
        }
        if (!(t->kind == IFC_WORD || (t->kind == IFC_SPACE && !t->drop))) continue;
        double a = ifc_tok_adv(ctx, idx, t->box);
        if (a > h) h = a;
    }
    return h > 0.0 ? h : br;
}

/* Where token k's box edge is: an open edge's border start, a close edge's
 * border end, else the token's own extent. */
static double ifc_tok_left(LayoutContext *ctx, const IfcTok *t)
{
    return t->kind == IFC_OPEN ? t->x + layout_box(ctx, t->box)->margin_left : t->x;
}

static double ifc_tok_right(LayoutContext *ctx, const IfcTok *t)
{
    return t->kind == IFC_CLOSE ? t->x + t->w - layout_box(ctx, t->box)->margin_right : t->x + t->w;
}

/* The room a line from y, h tall, has beside the floats of this formatting
 * context that overlap any of it (a point at y when h is 0). */
static void ifc_band_geometry(LayoutContext *ctx, double y, double h, double content_left, double avail_width,
                              double *lx, double *la)
{
    const FloatRec *fl = ctx->float_stack + t_float_base;
    int n = ctx->float_count - t_float_base;
    if (h <= 0.0) {
        layout_flow_line_geometry(fl, n, y, content_left, avail_width, lx, la);
        return;
    }
    double content_right = content_left + avail_width, li = 0.0, ri = 0.0;
    for (int i = 0; i < n; i++) {
        if (!(fl[i].top < y + h - 0.001 && fl[i].bottom > y + 0.001)) continue;
        if (fl[i].right <= content_left || fl[i].left >= content_right) continue;
        if (fl[i].side == 1 && fl[i].right - content_left > li) li = fl[i].right - content_left;
        if (fl[i].side != 1 && content_right - fl[i].left > ri) ri = content_right - fl[i].left;
    }
    *lx = content_left + li;
    *la = avail_width - li - ri;
    if (*la < 0.0) *la = 0.0;
}

/* Bidi levels on I's tokens when block idx's paragraph needs bidi: UAX #9
 * over its text as the bidi post-pass gathers it (bidi_para_collect), a
 * word cut where its level changes, the controls unicode-bidi stands for as
 * tokens of their own (in the word's place in the text, so they keep runs
 * apart when a line is reordered).  Whether its lines are finished in visual
 * order (something right to left in it), its level in *para. */
static bool ifc_bidi_levels(LayoutContext *ctx, int idx, IfcItems *I, int *para)
{
    LayoutBox *blk = layout_box(ctx, idx);
    if (!I->gather || I->ncp == 0) return false;
    bool rtl_el = false;   /* (an inline element right to left: its edges swap, levels or not) */
    for (int e = 0; e < I->nel && !rtl_el; e++) rtl_el = layout_box(ctx, I->el[e].box)->s->direction == 1;
    if (blk->s->direction != 1 && !rtl_el) {
        bool rtl = false;
        for (int i = 0; i < I->ncp && !rtl; i++) {
            uint32_t c = I->cp[i];
            if (c < 0x590) continue;
            int k = bidi_class(c);
            rtl = k == BIDI_R || k == BIDI_AL || k == BIDI_AN || k == BIDI_RLE || k == BIDI_RLO || k == BIDI_RLI ||
                  k == BIDI_FSI;
        }
        if (!rtl) return false;
    }
    uint8_t *lv = (uint8_t *)malloc((size_t)I->ncp);
    int *nmap = NULL;
    IfcItems J = { 0 };
    bool ok = false;
    if (!lv) return false;
    *para = bidi_levels_cached(I->cp, I->ncp, blk->s->unicode_bidi == 5 ? -1 : blk->s->direction, lv);
    if (*para < 0) goto out;
    if (!rtl_el && !(*para & 1)) {   /* (a right-to-left paragraph: its indent at the right) */
        bool odd = false;
        for (int i = 0; i < I->ncp && !odd; i++) odd = lv[i] & 1;
        if (!odd) goto out;
    }
    /* (each token its first code point's level; a word whose level changes
     * cut there, the tokens then copied) */
    int ncut = 0;
    for (int q = 0; q < I->n; q++) {
        IfcTok *t = &I->t[q];
        bool own = t->kind == IFC_WORD || t->kind == IFC_SPACE || t->kind == IFC_BR || t->kind == IFC_ATOM || t->kind == IFC_CTL;
        t->level = (unsigned char)(own && t->cp0 < I->ncp ? lv[t->cp0] : *para);
        if (t->kind == IFC_WORD)
            for (int k = t->cp0 + 1; k < t->cp1; k++)
                if (lv[k] != lv[t->cp0]) { ncut++; break; }
    }
    if (ncut) {
        nmap = (int *)malloc(((size_t)I->n + 1) * sizeof(int));
        if (!nmap) goto out;
        for (int q = 0; q < I->n; q++) {
            IfcTok t = I->t[q];
            nmap[q] = J.n;
            bool cut = false;
            if (t.kind == IFC_WORD)
                for (int k = t.cp0 + 1; k < t.cp1 && !cut; k++) cut = lv[k] != lv[t.cp0];
            if (cut) {
                const char *tx = css_layout_box_text(ctx, t.box, css_layout_node_text(ctx, css_layout_box_node(ctx, t.box)));
                int k0 = t.cp0;
                for (int k = t.cp0 + 1; k <= t.cp1; k++) {
                    if (k < t.cp1 && lv[k] == lv[k0]) continue;
                    uint32_t b0 = I->cbyte[k0], b1 = k < t.cp1 ? I->cbyte[k] : t.end;
                    if (!ifc_put(&J, 0, 0, 0, 0.0, 0)) goto out;
                    IfcTok *p = &J.t[J.n - 1];
                    *p = t;
                    p->start = b0;
                    p->end = b1;
                    p->cp0 = k0;
                    p->cp1 = k;
                    p->w = ifc_width(ctx, t.box, tx, b0, b1);
                    p->level = lv[k0];
                    k0 = k;
                }
            } else {
                if (!ifc_put(&J, 0, 0, 0, 0.0, 0)) goto out;
                J.t[J.n - 1] = t;
            }
        }
        nmap[I->n] = J.n;
        for (int e = 0; e < I->nel; e++) {
            I->el[e].tok0 = nmap[I->el[e].tok0];
            I->el[e].tok1 = nmap[I->el[e].tok1];
        }
        free(I->t);
        I->t = J.t;
        I->n = J.n;
        I->cap = J.cap;
        J.t = NULL;
    }
    ok = true;
out:
    free(J.t);
    free(nmap);
    free(lv);
    return ok;
}

/* A paragraph's lines in visual order: per token its innermost inline
 * element, per element its parent element and its state on the line being
 * placed (whether it has something there, its leftmost and rightmost unit,
 * the piece open), and the pieces placed, in page coordinates. */
typedef struct {
    int par, uline, lm_line, lm, rm, stamp, head, tail;   /* (head, tail: its pieces) */
    bool open;
    unsigned char sides;
    double ps;
} IfcBidiEl;

typedef struct {
    int e, line;
    double a, z;   /* its border box's left and right */
    unsigned char sides;
    int next;      /* its element's next piece */
} IfcBidiPiece;

typedef struct {
    int para, sc;
    int *tel;
    IfcBidiEl *el;
    int *u, *order;
    uint8_t *ulv;
    IfcBidiPiece *pc;
    int npc, pccap;
} IfcBidi;

static bool ifc_bidi_init(IfcBidi *B, const IfcItems *I)
{
    B->tel = (int *)malloc(((size_t)I->n + 1) * sizeof(int));
    B->u = (int *)malloc(((size_t)I->n + 1) * sizeof(int));
    B->order = (int *)malloc(((size_t)I->n + 1) * sizeof(int));
    B->ulv = (uint8_t *)malloc((size_t)I->n + 1);
    B->el = (IfcBidiEl *)calloc((size_t)I->nel + 1, sizeof(IfcBidiEl));
    if (!B->tel || !B->u || !B->order || !B->ulv || !B->el) return false;
    for (int q = 0; q < I->n; q++) B->tel[q] = -1;
    /* (elements in the order they open: each inside the ones before whose
     * tokens hold its own) */
    for (int e = 0; e < I->nel; e++) {
        const IfcEl *el = &I->el[e];
        B->el[e].par = el->tok0 < el->tok1 ? B->tel[el->tok0] : -1;
        B->el[e].uline = B->el[e].lm_line = B->el[e].head = B->el[e].tail = -1;
        for (int q = el->tok0; q < el->tok1; q++) B->tel[q] = e;
    }
    return true;
}

static void ifc_bidi_free(IfcBidi *B)
{
    free(B->tel);
    free(B->u);
    free(B->order);
    free(B->ulv);
    free(B->el);
    free(B->pc);
}

/* Whether element e's start (open) or end edge token is in tokens [t0, t1). */
static bool ifc_edge_on(const IfcItems *I, int e, bool open, int t0, int t1)
{
    const IfcEl *el = &I->el[e];
    int q = open ? el->tok0 : el->tok1 - 1;
    return q >= t0 && q < t1 && q >= el->tok0 && q < el->tok1 && I->t[q].kind == (open ? IFC_OPEN : IFC_CLOSE) &&
           I->t[q].box == el->box;
}

/* Line L, tokens [t0, t1), placed in visual order from x: its units (words,
 * spaces kept, atomic inlines, breaks, marks, controls, and an edge of an
 * element with nothing else on the line) at their levels, white space at
 * its end back to the paragraph's (L1), reordered (L2); each inline
 * element's pieces its stretches of adjacent units, its left edge on its
 * leftmost unit of the line its left edge is on (its start edge's line when
 * it is left to right, its end edge's when right to left) and its right
 * edge likewise (CSS 2.1 section 8.6), opened outermost first and closed
 * innermost first.  Returns where the line ends. */
static double ifc_bidi_line(LayoutContext *ctx, IfcItems *I, IfcBidi *B, int t0, int t1, int L, double x)
{
#define IFC_UNIT(t) ((t)->kind == IFC_WORD || ((t)->kind == IFC_SPACE && !(t)->drop) || (t)->kind == IFC_ATOM || \
                     (t)->kind == IFC_MARK || (t)->kind == IFC_BR || (t)->kind == IFC_CTL || (t)->kind == IFC_ABS)
    for (int q = t0; q < t1; q++)
        if (IFC_UNIT(&I->t[q]))
            for (int e = B->tel[q]; e >= 0; e = B->el[e].par) B->el[e].uline = L;
    int nu = 0;
    for (int q = t0; q < t1; q++) {
        const IfcTok *t = &I->t[q];
        bool unit = IFC_UNIT(t);
        if (!unit && (t->kind == IFC_OPEN || t->kind == IFC_CLOSE) && B->tel[q] >= 0 && B->el[B->tel[q]].uline != L) {
            /* (an edge of an element with nothing else here: once) */
            unit = true;
            B->el[B->tel[q]].uline = L;
        }
        if (unit) B->u[nu++] = q;
    }
#undef IFC_UNIT
    if (nu == 0) return x;
    for (int k = 0; k < nu; k++) {
        const IfcTok *t = &I->t[B->u[k]];
        bool own = t->kind == IFC_WORD || t->kind == IFC_SPACE || t->kind == IFC_ATOM || t->kind == IFC_CTL;
        B->ulv[k] = own ? t->level : t->kind == IFC_BR ? (uint8_t)B->para : k > 0 ? B->ulv[k - 1] : (uint8_t)B->para;
    }
    for (int k = nu - 1; k >= 0; k--) {
        int kind = I->t[B->u[k]].kind;
        if (kind == IFC_WORD || kind == IFC_ATOM) break;
        B->ulv[k] = (uint8_t)B->para;
    }
    bidi_reorder(B->ulv, nu, B->order);
    for (int k = 0; k < nu; k++)
        for (int e = B->tel[B->u[B->order[k]]]; e >= 0; e = B->el[e].par) {
            IfcBidiEl *s = &B->el[e];
            if (s->lm_line != L) {
                s->lm_line = L;
                s->lm = k;
                s->open = false;
            }
            s->rm = k;
        }
    for (int k = 0; k < nu; k++) {
        int q = B->u[B->order[k]];
        IfcTok *t = &I->t[q];
        int ch[64], nch = 0;
        for (int e = B->tel[q]; e >= 0 && nch < 64; e = B->el[e].par) ch[nch++] = e;
        for (int j = nch - 1; j >= 0; j--) {
            IfcBidiEl *s = &B->el[ch[j]];
            if (s->open) continue;
            const LayoutBox *eb = layout_box(ctx, I->el[ch[j]].box);
            bool left = k == s->lm && ifc_edge_on(I, ch[j], eb->s->direction != 1, t0, t1);
            if (left) x += eb->margin_left;
            s->open = true;
            s->ps = x;
            s->sides = left ? 1 : 0;
            if (left) x += eb->s->border_left + eb->padding_left;
        }
        t->x = x;
        if (t->kind == IFC_WORD || t->kind == IFC_SPACE || t->kind == IFC_ATOM) x += t->w;
        B->sc++;
        if (k + 1 < nu)
            for (int e = B->tel[B->u[B->order[k + 1]]]; e >= 0; e = B->el[e].par) B->el[e].stamp = B->sc;
        for (int j = 0; j < nch; j++) {
            IfcBidiEl *s = &B->el[ch[j]];
            if (s->stamp == B->sc) continue;
            const LayoutBox *eb = layout_box(ctx, I->el[ch[j]].box);
            bool right = k == s->rm && ifc_edge_on(I, ch[j], eb->s->direction == 1, t0, t1);
            if (right) x += eb->padding_right + eb->s->border_right;
            if (B->npc == B->pccap) {
                int nc = B->pccap ? B->pccap * 2 : 16;
                IfcBidiPiece *np = (IfcBidiPiece *)realloc(B->pc, (size_t)nc * sizeof(IfcBidiPiece));
                if (np) {
                    B->pc = np;
                    B->pccap = nc;
                }
            }
            if (B->npc < B->pccap) {
                IfcBidiPiece *p = &B->pc[B->npc++];
                p->e = ch[j];
                p->line = L;
                p->a = s->ps;
                p->z = x;
                p->sides = (unsigned char)(s->sides | (right ? 2 : 0));
                p->next = -1;
                if (s->tail >= 0) B->pc[s->tail].next = B->npc - 1;
                else s->head = B->npc - 1;
                s->tail = B->npc - 1;
            }
            if (right) x += eb->margin_right;
            s->open = false;
        }
    }
    return x;
}

/* Lay out block idx's inline content on items (when ifc_eligible): its
 * lines from cur->y, the cursor left at the last line's top with its height
 * in line_box, as the flow's per-child path leaves it. */
static void ifc_layout(LayoutContext *ctx, int idx, double content_left, double avail_width, double indent0,
                       FlowCursor *cur, bool *any_placed, FloatRec *floats, int *nfloats, int fcap)
{
    LayoutBox *blk = layout_box(ctx, idx);
    IfcItems I = { 0 };
    I.space_before = true;
    I.gather = blk->s->direction == 1 || blk->s->unicode_bidi != 0 || layout_paragraph_walk(ctx, idx, NULL);
    /* (an override on the block itself: around all of it) */
    bool blk_ovr = I.gather && (blk->s->unicode_bidi == 3 || blk->s->unicode_bidi == 4);
    if (blk_ovr) ifc_cp_put(&I, blk->s->direction == 1 ? 0x202E : 0x202D, 0);
    I.x0 = content_left;
    I.y0 = cur->y;
    ifc_collect(ctx, idx, &I, avail_width, layout_pct_base_height(ctx, idx));
    if (blk_ovr) ifc_cp_put(&I, 0x202C, 0);
    IfcBidi B;
    memset(&B, 0, sizeof(B));
    bool bidi = ifc_bidi_levels(ctx, idx, &I, &B.para) && ifc_bidi_init(&B, &I);
    layout_flow_noreuse_up(ctx, idx);   /* (its fragments are not in the flow record yet) */
    if (!ctx->text_frags) {
        ctx->text_frags = (LayoutTextFragList *)calloc((size_t)ctx->tree.count, sizeof(LayoutTextFragList));
        ctx->text_frags_n = ctx->text_frags ? ctx->tree.count : 0;
    }
    bool any = false;   /* (anything that makes a line: a word, an edge, a break) */
    for (int k = 0; k < I.n && !any; k++)
        any = I.t[k].kind != IFC_SPACE && I.t[k].kind != IFC_MARK && I.t[k].kind != IFC_FLOAT && I.t[k].kind != IFC_CTL &&
              I.t[k].kind != IFC_ABS;
    /* (no line: its floats where the content starts) */
    for (int k = 0; !any && k < I.n; k++)
        if (I.t[k].kind == IFC_FLOAT) layout_place_float(ctx, I.t[k].box, content_left, avail_width, cur->y, floats, nfloats, fcap);
    int *deferred = (int *)malloc(((size_t)I.n + 1) * sizeof(int));
    int ndef = 0;
    int cap_lines = 16, nlines = 0;
    double *ltop = (double *)malloc((size_t)cap_lines * sizeof(double)), *lh = (double *)malloc((size_t)cap_lines * sizeof(double));
    double y = cur->y, last_x = content_left;
    int i = 0;
    double band_h = 0.0;   /* (a line tried again: the height of the floats' band it must clear) */
    while (any && ltop && lh && deferred && i < I.n) {
        double lx = content_left, la = avail_width;
        ifc_band_geometry(ctx, y, band_h, content_left, avail_width, &lx, &la);
        double x = lx + (nlines == 0 ? indent0 : 0.0);
        int t0 = i;
        double restart = -1.0;
        bool word = false;   /* (a word on it: spaces after it are kept, a group that does not fit breaks) */
        while (i < I.n) {
            IfcTok *t = &I.t[i];
            if (t->kind == IFC_FLOAT && t->drop) { i++; continue; }   /* (placed, the line started over) */
            if (t->kind == IFC_FLOAT && ndef > 0) {
                /* (after one that waits for the line's end: in order, not above it) */
                t->drop = 1;
                deferred[ndef++] = t->box;
                i++;
                continue;
            }
            if (t->kind == IFC_FLOAT) {
                t->drop = 1;
                /* A float at this line's top when it fits beside what the line
                 * holds (CSS 2.1 section 9.5.1), the line then starting past a
                 * left one; else after the line. */
                int fc0 = ctx->float_count, nf0 = *nfloats;
                layout_place_float(ctx, t->box, content_left, avail_width, y, floats, nfloats, fcap);
                double lx2 = content_left, la2 = avail_width;
                layout_flow_line_geometry(ctx->float_stack + t_float_base, ctx->float_count - t_float_base, y,
                                          content_left, avail_width, &lx2, &la2);
                double used = x - lx;
                if (word && used > la2 + 0.01 && ctx->float_count > fc0) {
                    ctx->float_count = fc0;
                    *nfloats = nf0;
                    deferred[ndef++] = t->box;
                } else if (lx2 != lx) {
                    for (int k = t0; k < i; k++) I.t[k].x += lx2 - lx;
                    x += lx2 - lx;
                }
                lx = lx2;
                la = la2;
                i++;
                continue;
            }
            if (t->kind == IFC_BR) {
                t->x = x;
                t->line = nlines;
                i++;
                break;
            }
            if (t->kind == IFC_SPACE && (!word || !t->nobrk)) {
                t->x = x;
                t->line = nlines;
                if (!word && !t->kept) t->drop = 1;   /* (at the line's start: removed) */
                else x += t->w;
                i++;
                /* (an element that ends after the space ends on this line) */
                while (i < I.n && (I.t[i].kind == IFC_CLOSE || I.t[i].kind == IFC_MARK)) {
                    I.t[i].x = x;
                    I.t[i].line = nlines;
                    x += I.t[i].w;
                    i++;
                }
                continue;
            }
            /* a group: up to the next space or break */
            int j = i;
            double ww = 0.0;
            bool has_word = false, has_atom = false, atom_brk = false;
            while (j < I.n && !(I.t[j].kind == IFC_SPACE && !I.t[j].nobrk) && I.t[j].kind != IFC_BR && I.t[j].kind != IFC_FLOAT) {
                /* (an atomic inline: a break opportunity before and after it,
                 * where the text around it wraps) */
                if (I.t[j].kind == IFC_ATOM && !I.t[j].nobrk && (has_word || has_atom)) break;
                if (atom_brk && I.t[j].kind != IFC_CLOSE) break;
                ww += I.t[j].w;
                has_word = has_word || I.t[j].kind == IFC_WORD;
                has_atom = has_atom || I.t[j].kind == IFC_ATOM;
                if (I.t[j].kind == IFC_ATOM) atom_brk = !I.t[j].nobrk;
                else if (I.t[j].kind != IFC_CLOSE) atom_brk = false;
                j++;
            }
            has_word = has_word || has_atom;
            if (word && x + ww > lx + la + 0.01) break;
            /* (a line that floats narrow too much for its first content goes
             * down past them, CSS 2.1 section 9.5) */
            if (!word && x + ww > lx + la + 0.01 && la < avail_width - 0.01) {
                double next = -1.0;
                for (int fi = t_float_base; fi < ctx->float_count; fi++) {
                    double fb = ctx->float_stack[fi].bottom;
                    if (fb > y + 0.01 && (next < 0.0 || fb < next)) next = fb;
                }
                if (next > 0.0) {
                    restart = next;
                    break;
                }
            }
            for (int k = i; k < j; k++) {
                I.t[k].x = x;
                I.t[k].line = nlines;
                x += I.t[k].w;
            }
            word = word || has_word;
            i = j;
        }
        if (restart > 0.0) {
            /* (the line again, lower down: its spaces kept or dropped anew) */
            for (int k = t0; k < i; k++)
                if (I.t[k].kind != IFC_FLOAT) I.t[k].drop = 0;
            i = t0;
            y = restart;
            band_h = 0.0;
            continue;
        }
        /* (the line as tall as it is: the floats beside any of it narrow it;
         * once, built again in that room) */
        if (band_h <= 0.0) {
            double h = ifc_line_height(ctx, idx, &I, t0, i), blx, bla;
            ifc_band_geometry(ctx, y, h, content_left, avail_width, &blx, &bla);
            if (h > 0.0 && (blx > lx + 0.01 || blx + bla < lx + la - 0.01)) {
                for (int k = t0; k < i; k++)
                    if (I.t[k].kind != IFC_FLOAT) I.t[k].drop = 0;
                i = t0;
                band_h = h;
                continue;
            }
        }
        band_h = 0.0;
        /* (spaces at the line's end are removed, the inline boxes that close
         * after them too: their edges move back to the last word) */
        {
            int f0 = i;
            while (f0 > t0 && (I.t[f0 - 1].kind == IFC_SPACE || I.t[f0 - 1].kind == IFC_CLOSE || I.t[f0 - 1].kind == IFC_MARK ||
                               I.t[f0 - 1].kind == IFC_CTL || I.t[f0 - 1].kind == IFC_ABS))
                f0--;
            if (f0 < i) {
                double xx = I.t[f0].x;
                for (int k = f0; k < i; k++) {
                    IfcTok *t = &I.t[k];
                    t->x = xx;
                    if (t->kind == IFC_SPACE && !t->kept) t->drop = 1;
                    else xx += t->w;
                }
                x = xx;
            }
        }
        if (bidi) {
            /* (in visual order, the first line's indent at the paragraph's
             * start side) */
            bool rl = B.para & 1;
            int pc0 = B.npc;
            double end = ifc_bidi_line(ctx, &I, &B, t0, i, nlines, lx + (nlines == 0 && !rl ? indent0 : 0.0));
            double slack = lx + la - (nlines == 0 && rl ? indent0 : 0.0) - end;
            if (blk->s->text_align != CSS_TEXT_ALIGN_LEFT && slack > 0.5) {
                double dx = blk->s->text_align == CSS_TEXT_ALIGN_CENTER ? slack * 0.5 : slack;
                for (int k = t0; k < i; k++) I.t[k].x += dx;
                for (int k = pc0; k < B.npc; k++) {
                    B.pc[k].a += dx;
                    B.pc[k].z += dx;
                }
                end += dx;
            }
            x = end;
        } else {
            /* (the line's end: its last word or edge, trailing spaces hang) */
            double end = lx + (nlines == 0 ? indent0 : 0.0);
            for (int k = i - 1; k >= t0; k--)
                if (I.t[k].kind == IFC_WORD || I.t[k].kind == IFC_OPEN || I.t[k].kind == IFC_CLOSE || I.t[k].kind == IFC_ATOM) {
                    end = I.t[k].x + I.t[k].w;
                    break;
                }
            double slack = lx + la - end;
            if (blk->s->text_align != CSS_TEXT_ALIGN_LEFT && slack > 0.5) {
                double dx = blk->s->text_align == CSS_TEXT_ALIGN_CENTER ? slack * 0.5 : slack;
                for (int k = t0; k < i; k++) I.t[k].x += dx;
                x += dx;
            }
        }
        if (nlines == cap_lines) {
            cap_lines *= 2;
            double *a = (double *)realloc(ltop, (size_t)cap_lines * sizeof(double));
            double *b = a ? (double *)realloc(lh, (size_t)cap_lines * sizeof(double)) : NULL;
            if (a) ltop = a;
            if (!b) break;
            lh = b;
        }
        ltop[nlines] = y;
        lh[nlines] = ifc_line_height(ctx, idx, &I, t0, i);
        y += lh[nlines];
        nlines++;
        last_x = x;
        /* (floats that did not fit beside it: below it) */
        for (int q = 0; q < ndef; q++) layout_place_float(ctx, deferred[q], content_left, avail_width, y, floats, nfloats, fcap);
        ndef = 0;
    }
    for (int q = 0; deferred && q < ndef; q++) layout_place_float(ctx, deferred[q], content_left, avail_width, y, floats, nfloats, fcap);
    free(deferred);
    if (!ltop || !lh) nlines = 0;
    /* Text boxes: fragments per line (consecutive tokens of one box on one
     * line), the box around them; a break, a mark, where it is. */
    for (int k = 0; k < I.n;) {
        if (I.t[k].kind == IFC_CTL) { k++; continue; }
        int b = I.t[k].box;
        int k1 = k;
        while (k1 < I.n && I.t[k1].box == b && I.t[k1].kind != IFC_OPEN && I.t[k1].kind != IFC_CLOSE) k1++;
        if (k1 == k) { k++; continue; }   /* (an edge: its element's, below) */
        if (I.t[k].kind == IFC_FLOAT || I.t[k].kind == IFC_ABS) { k = k1; continue; }   /* (placed already; placed after) */
        LayoutBox *bb = layout_box(ctx, b);
        HtmlNode *tn = css_layout_box_node(ctx, b);
        int L = nlines > 0 ? (I.t[k].line < nlines ? I.t[k].line : nlines - 1) : -1;
        double top = L >= 0 ? ltop[L] : cur->y;
        if (I.t[k].kind == IFC_ATOM) {
            /* (its margin box at the line's top, as the flow places it) */
            double dx = I.t[k].x + bb->margin_left - bb->x, dy = top + bb->margin_top - bb->y;
            if (dx != 0.0 || dy != 0.0) {
                bb->x += dx;
                bb->y += dy;
                layout_offset_subtree(ctx, b, dx, dy);
            }
            layout_update_content_sizes(bb);
            k = k1;
            continue;
        }
        if (I.t[k].kind == IFC_BR && ifc_is_br(tn)) {
            bb->x = I.t[k].x;
            bb->y = top;
            bb->width = 0.0;
            bb->height = layout_line_advance_box(bb);
        } else if (tn && tn->type == HTML_NODE_TEXT && nlines > 0) {
            TextShaper *font;
            float scale, adv;
            const char *t;
            bool painted = css_layout_text_paint(ctx, b, &font, &scale, &adv, &t);
            double fs = bb->s->font_size > 0.0 ? bb->s->font_size : 16.0;
            double tadv = layout_line_advance_box(bb);
            int kk0 = -1, kk1 = -1;
            for (int q = k; q < k1; q++)
                if (I.t[q].kind == IFC_WORD || (I.t[q].kind == IFC_SPACE && !I.t[q].drop)) { if (kk0 < 0) kk0 = q; kk1 = q; }
            LayoutTextFragList *tf = ctx->text_frags && b < ctx->text_frags_n ? &ctx->text_frags[b] : NULL;
            if (kk0 < 0) {
                bb->x = I.t[k].x;
                bb->y = top;
                bb->width = bb->height = 0.0;
                if (tf) tf->n = 0;
            } else {
                int L0 = I.t[kk0].line, L1 = I.t[kk1].line;
                double x0 = 1e30, x1 = -1e30;
                for (int q = kk0; q <= kk1; q++) {
                    if (I.t[q].drop || (I.t[q].kind != IFC_WORD && I.t[q].kind != IFC_SPACE)) continue;
                    if (I.t[q].x < x0) x0 = I.t[q].x;
                    if (I.t[q].x + I.t[q].w > x1) x1 = I.t[q].x + I.t[q].w;
                }
                bb->x = x0;
                bb->y = ltop[L0];
                bb->width = x1 - x0;
                bb->height = L0 == L1 ? tadv : ltop[L1] + tadv - ltop[L0];
                if (tf && painted) {
                    int nf = kk1 - kk0 + 1;
                    LayoutTextFrag *g = (LayoutTextFrag *)realloc(tf->f, (size_t)nf * sizeof(LayoutTextFrag));
                    if (g) {
                        tf->f = g;
                        tf->n = 0;
                        for (int q = kk0; q <= kk1; q++) {
                            if (I.t[q].drop || (I.t[q].kind != IFC_WORD && I.t[q].kind != IFC_SPACE)) continue;
                            LayoutTextFrag *fr = tf->n > 0 ? &tf->f[tf->n - 1] : NULL;
                            const IfcTok *tq = &I.t[q], *tp = q > 0 ? &I.t[q - 1] : tq;
                            /* (in visual order: one level, the token beside the
                             * one before on the side its level reads from) */
                            bool apart = bidi && fr && (tq->level != tp->level ||
                                                        fabs((tq->level & 1) ? tq->x + tq->w - tp->x : tp->x + tp->w - tq->x) > 0.01);
                            if (!fr || tq->line != tp->line || tp->drop || apart ||
                                (tp->kind != IFC_WORD && tp->kind != IFC_SPACE)) {
                                fr = &tf->f[tf->n++];
                                fr->start = tq->start;
                                fr->dx = tq->x - bb->x;
                                fr->dy = ltop[tq->line] + (adv - fs) * 0.5 - bb->y;
                                fr->level = bidi ? tq->level : 0;
                                fr->w = 0.0;
                            }
                            fr->end = tq->end;
                            if (tq->x - bb->x < fr->dx) {
                                fr->w += fr->dx - (tq->x - bb->x);   /* (right to left: it grows leftward) */
                                fr->dx = tq->x - bb->x;
                            } else {
                                fr->w = tq->x + tq->w - (bb->x + fr->dx);
                            }
                        }
                    }
                }
            }
        } else {
            bb->x = I.t[k].x;
            bb->y = top;
            bb->width = bb->height = 0.0;
        }
        layout_update_content_sizes(bb);
        k = k1;
    }
    /* Inline elements: around their content and edges (one line: the
     * content area with padding and border, centred by the half-leading, as
     * the flow's one-line inline boxes are); on several lines, with a
     * background or border, a piece per line, the start edge on the line it
     * opens on and the end edge on the line it closes on. */
    for (int e = 0; e < I.nel; e++) {
        IfcEl *el = &I.el[e];
        LayoutBox *eb = layout_box(ctx, el->box);
        double x0 = 1e30, x1 = -1e30, atom_h = 0.0;
        int L0 = -1, L1 = -1;
        bool has_text = false;
        for (int q = el->tok0; q < el->tok1; q++) {
            const IfcTok *t = &I.t[q];
            if (t->drop || t->kind == IFC_CTL || t->kind == IFC_ABS) continue;
            if (t->kind == IFC_WORD || t->kind == IFC_SPACE) has_text = true;
            if (t->kind == IFC_ATOM) {
                const LayoutBox *ab = layout_box(ctx, t->box);
                if (ab->margin_top + ab->height + ab->margin_bottom > atom_h) atom_h = ab->margin_top + ab->height + ab->margin_bottom;
            }
            /* (a space that ends a line hangs outside) */
            if (t->kind == IFC_SPACE && (q + 1 >= I.n || I.t[q + 1].line != t->line)) continue;
            double a = ifc_tok_left(ctx, t), z = ifc_tok_right(ctx, t);
            if (a < x0) x0 = a;
            if (z > x1) x1 = z;
            if (t->kind == IFC_MARK) continue;
            if (L0 < 0) L0 = t->line;
            L1 = t->line;
        }
        /* (in visual order: around its pieces) */
        int npc = 0;
        for (int k = bidi ? B.el[e].head : -1; k >= 0; k = B.pc[k].next) {
            if (npc++ == 0) {
                x0 = 1e30;
                x1 = -1e30;
                L0 = B.pc[k].line;
            }
            if (B.pc[k].a < x0) x0 = B.pc[k].a;
            if (B.pc[k].z > x1) x1 = B.pc[k].z;
            L1 = B.pc[k].line;
        }
        if (x1 < x0) {
            int q = el->tok0 < I.n ? el->tok0 : I.n - 1;
            x0 = x1 = q >= 0 ? I.t[q].x : content_left;
        }
        double adv = layout_line_advance_box(eb);
        double ca = layout_content_area_height(eb);
        double vpb = layout_vertical_border_padding(eb);
        eb->x = x0;
        eb->width = x1 - x0;
        if (L0 < 0 || nlines == 0) {
            int q = el->tok0 < I.n ? el->tok0 : -1;
            eb->y = q >= 0 && nlines > 0 ? ltop[I.t[q].line < nlines ? I.t[q].line : nlines - 1] : cur->y;
            eb->height = 0.0;
        } else if (L0 == L1 && !has_text && atom_h > 0.0) {
            /* (only atomic inlines: around them, as the flow sizes it from them) */
            eb->y = ltop[L0];
            eb->height = atom_h;
        } else if (L0 == L1 && npc <= 1) {
            eb->y = ltop[L0] + (adv - ca) / 2.0 - eb->padding_top - eb->s->border_top;
            eb->height = ca + vpb;
        } else {
            if (L0 == L1) {
                eb->y = ltop[L0] + (adv - ca) / 2.0 - eb->padding_top - eb->s->border_top;
                eb->height = ca + vpb;
            } else {
                eb->y = ltop[L0];
                eb->height = ltop[L1] + adv - ltop[L0];
            }
            bool deco = eb->s->background_color_a > 0.0f || eb->s->border_top > 0.0 || eb->s->border_bottom > 0.0 ||
                        eb->s->border_left > 0.0 || eb->s->border_right > 0.0 || eb->s->background_image_url[0];
            if (deco && !ctx->pieces) {
                ctx->pieces = (LayoutPieceList *)calloc((size_t)ctx->tree.count, sizeof(LayoutPieceList));
                ctx->pieces_n = ctx->pieces ? ctx->tree.count : 0;
            }
            LayoutPieceList *pl = deco && ctx->pieces && el->box < ctx->pieces_n ? &ctx->pieces[el->box] : NULL;
            for (int k = bidi && pl ? B.el[e].head : -1; k >= 0; k = B.pc[k].next) {
                if (pl->n == pl->cap) {
                    int nc = pl->cap ? pl->cap * 2 : 4;
                    LayoutPiece *np = (LayoutPiece *)realloc(pl->p, (size_t)nc * sizeof(LayoutPiece));
                    if (!np) break;
                    pl->p = np;
                    pl->cap = nc;
                }
                LayoutPiece *pc = &pl->p[pl->n++];
                pc->anchor = -1;
                pc->sides = B.pc[k].sides;
                pc->dx = B.pc[k].a - eb->x;
                pc->dy = ltop[B.pc[k].line] + (adv - ca) / 2.0 - eb->padding_top - eb->s->border_top - eb->y;
                pc->w = B.pc[k].z - B.pc[k].a;
                pc->h = ca + vpb;
            }
            for (int L = L0; pl && !bidi && L <= L1; L++) {
                double a = 1e30, z = -1e30;
                unsigned char sides = 0;
                for (int q = el->tok0; q < el->tok1; q++) {
                    const IfcTok *t = &I.t[q];
                    if (t->line != L || t->drop || t->kind == IFC_MARK) continue;
                    if (t->kind == IFC_SPACE && (q + 1 >= I.n || I.t[q + 1].line != L)) continue;
                    if (t->kind == IFC_OPEN) sides |= 1;
                    if (t->kind == IFC_CLOSE) sides |= 2;
                    double l = ifc_tok_left(ctx, t), r = ifc_tok_right(ctx, t);
                    if (l < a) a = l;
                    if (r > z) z = r;
                }
                if (z < a) continue;
                if (pl->n == pl->cap) {
                    int nc = pl->cap ? pl->cap * 2 : 4;
                    LayoutPiece *np = (LayoutPiece *)realloc(pl->p, (size_t)nc * sizeof(LayoutPiece));
                    if (!np) break;
                    pl->p = np;
                    pl->cap = nc;
                }
                LayoutPiece *pc = &pl->p[pl->n++];
                pc->anchor = -1;
                pc->sides = sides;
                pc->dx = a - eb->x;
                pc->dy = ltop[L] + (adv - ca) / 2.0 - eb->padding_top - eb->s->border_top - eb->y;
                pc->w = z - a;
                pc->h = ca + vpb;
            }
        }
        layout_update_content_sizes(eb);
    }
    /* Absolutely positioned boxes: their static position (CSS 2.1 section
     * 10.3.7), relative to their parent, for the positioning pass: one
     * inline-level before blockification where it is on its line, at the
     * line's top; a block-level one at the content's left, below its line
     * when something comes before it there. */
    for (int k = 0; k < I.n; k++) {
        if (I.t[k].kind != IFC_ABS) continue;
        LayoutBox *ab = layout_box(ctx, I.t[k].box);
        int p = ctx->tree.nodes[I.t[k].box].parent_idx;
        if (p < 0) continue;
        double sx = content_left, sy = cur->y;
        if (nlines > 0) {
            int L = I.t[k].line < nlines ? I.t[k].line : nlines - 1;
            sy = ltop[L];
            if (ab->s->abs_inline) {
                sx = I.t[k].x;
            } else {
                for (int q = k - 1; q >= 0 && I.t[q].line == L; q--)
                    if (I.t[q].kind == IFC_WORD || I.t[q].kind == IFC_ATOM || (I.t[q].kind == IFC_SPACE && !I.t[q].drop)) {
                        sy = ltop[L] + lh[L];
                        break;
                    }
            }
        }
        const LayoutBox *pb = layout_box(ctx, p);
        ab->static_dx = sx - pb->x;
        ab->static_dy = sy - pb->y;
        ab->has_static = 1;
    }
    if (nlines > 0) {
        cur->y = ltop[nlines - 1];
        cur->line_box = lh[nlines - 1];
        cur->x = last_x;
        *any_placed = true;
    }
    free(ltop);
    free(lh);
    free(I.t);
    free(I.el);
    free(I.cp);
    free(I.cbyte);
    ifc_bidi_free(&B);
}

static void layout_block_flow_inner(LayoutContext *ctx, int idx)
{
    LayoutBox *box = layout_box(ctx, idx);
    double content_left = box->x + box->padding_left + box->s->border_left;
    double content_top  = box->y + box->padding_top + box->s->border_top;
    double avail_width  = box->content_width;
    HtmlNode *self_dom = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    bool in_center = self_dom && self_dom->type == HTML_NODE_ELEMENT && self_dom->tag_name &&
                     strcasecmp(self_dom->tag_name, "center") == 0;

    int nkids = 0;
    int *kids = layout_collect_flow_children(ctx, idx, &nkids);

    /* Inlines split around their block-level children (block_in_inline 2):
     * their children are items of this flow, the blocks laid out as blocks
     * and each stretch of inline content between them as a piece of the
     * inline, between a start and an end item (piece_of 2k and 2k + 1 for
     * piece k) where its edges take room and its border boxes are noted.
     * piece_of[i]: -1 for any other item. */
    /* (inline content on items, CYBER_IFC: no pieces set up, the
     * per-child loop not run) */
    bool ifc_done = ifc_eligible(ctx, idx);
    FlowPiece *fpieces = NULL;
    int *piece_of = NULL, nfp = 0, extra = 0;
    for (int i = 0; !ifc_done && ctx->pieces && i < nkids; i++) {
        LayoutBox *kb = layout_box(ctx, kids[i]);
        if (kb->s->block_in_inline == 2 && kb->s->display == CSS_DISPLAY_INLINE)
            extra += 4 * ctx->tree.nodes[kids[i]].child_count + 4;
    }
    if (extra > 0) {
        int cap = nkids + extra;
        int *items = (int *)malloc((size_t)cap * sizeof(int));
        piece_of = (int *)malloc((size_t)cap * sizeof(int));
        fpieces = (FlowPiece *)calloc((size_t)extra, sizeof(FlowPiece));
        if (!items || !piece_of || !fpieces) {
            free(items);
            free(piece_of);
            free(fpieces);
            piece_of = NULL;
            fpieces = NULL;
        } else {
            int n = 0;
            for (int i = 0; i < nkids; i++) {
                int el = kids[i];
                LayoutBox *eb = layout_box(ctx, el);
                if (!(eb->s->block_in_inline == 2 && eb->s->display == CSS_DISPLAY_INLINE)) {
                    items[n] = el;
                    piece_of[n++] = -1;
                    continue;
                }
                ctx->pieces[el].n = 0;
                layout_resolve_used_sizes(eb, layout_node_dom(ctx, ctx->tree.nodes[el].dom_node_idx),
                                          avail_width, layout_pct_base_height(ctx, idx));
                bool ldeco = eb->margin_left != 0.0 || eb->padding_left != 0.0 || eb->s->border_left != 0.0;
                bool rdeco = eb->margin_right != 0.0 || eb->padding_right != 0.0 || eb->s->border_right != 0.0;
                bool first_piece = true;
                bool rtl = eb->s->direction == 1;
                for (int k = ctx->tree.nodes[el].first_child_idx;;) {
                    /* (a piece: up to a block or a line break) */
                    int b = k, last = -1;
                    while (b >= 0) {
                        LayoutBox *bb = layout_box(ctx, b);
                        if (layout_is_in_flow(bb) && layout_box_is_block_level(bb) && bb->s->float_side == 0) break;
                        if (layout_is_br(ctx, b)) break;
                        last = b;
                        b = ctx->tree.nodes[b].next_sibling_idx;
                    }
                    FlowPiece *fp = &fpieces[nfp];
                    fp->el = el;
                    fp->first = last >= 0 ? k : -1;
                    fp->last = last;
                    /* (the start edge on the first piece, the end edge on the
                     * last: the right edge first when right to left) */
                    bool first_p = first_piece, last_p = b < 0;
                    fp->sides = (unsigned char)(((rtl ? last_p : first_p) ? 1 : 0) | ((rtl ? first_p : last_p) ? 2 : 0));
                    fp->shown = !layout_inline_range_empty(ctx, fp->first, fp->last) ||
                                ((fp->sides & 1) && ldeco) || ((fp->sides & 2) && rdeco);
                    items[n] = el;
                    piece_of[n++] = 2 * nfp;
                    for (int q = fp->first; q >= 0; q = ctx->tree.nodes[q].next_sibling_idx) {
                        LayoutBox *qb = layout_box(ctx, q);
                        if (layout_is_in_flow(qb) && (fp->shown || qb->s->float_side != 0)) {
                            items[n] = q;
                            piece_of[n++] = -1;
                        }
                        if (q == fp->last) break;
                    }
                    items[n] = el;
                    piece_of[n++] = 2 * nfp + 1;
                    nfp++;
                    if (b < 0) break;
                    items[n] = b;
                    piece_of[n++] = -1;
                    first_piece = false;
                    k = ctx->tree.nodes[b].next_sibling_idx;
                }
            }
            free(kids);
            kids = items;
            nkids = n;
        }
    }
    /* (laid out again each time: its pieces are not in the flow record) */
    if (fpieces) layout_flow_noreuse_up(ctx, idx);
    /* Where a piece's left edge opened its line: white space after it is
     * still at the line's start. */
    double lead_x = -1e30, lead_y = -1e30;

    /* text-indent: the first line (before any block, and while the pen is
     * on the container's first line) starts this far in. */
    double indent0 = box->s->text_indent + box->s->text_indent_pct * avail_width;
    bool block_seen = false;

    FlowCursor cur = {0};
    cur.x = content_left + indent0;
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
    int *line_members = (nkids > 0) ? (int*)malloc((size_t)(nkids + extra) * sizeof(int)) : NULL;
    int line_member_count = 0;

    /* Floats placed by this container's flow; inline lines shrink around them. */
    FloatRec floats[32];
    int nfloats = 0;

    if (ifc_done)
        ifc_layout(ctx, idx, content_left, avail_width, indent0, &cur, &any_placed, floats, &nfloats,
                   (int)(sizeof(floats) / sizeof(floats[0])));

    for (int i = 0; !ifc_done && i < nkids; i++) {
        int c = kids[i];
        LayoutBox *child = layout_box(ctx, c);
        if (piece_of && piece_of[i] >= 0) {
            FlowPiece *fp = &fpieces[piece_of[i] / 2];
            if (piece_of[i] & 1) {
                bool line_ends = i + 1 >= nkids ||
                                 (piece_of[i + 1] < 0 && layout_box_is_block_level(layout_box(ctx, kids[i + 1])) &&
                                  layout_box(ctx, kids[i + 1])->s->float_side == 0) ||
                                 (piece_of[i + 1] < 0 && layout_is_br(ctx, kids[i + 1]));
                layout_piece_end(ctx, fp, &cur, line_ends);
            } else if (layout_piece_start(ctx, fp, &cur, &pending_mb, &any_placed, content_left, avail_width)) {
                lead_x = cur.x;
                lead_y = cur.y;
            }
            continue;
        }


        /* Resolve used sizes against this container's content box. */
        layout_resolve_used_sizes(child, layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx),
                                  avail_width, layout_pct_base_height(ctx, idx));

        if (child->s->float_side != 0 && (layout_is_block_flow(child->s->display) ||
                                       child->s->display == CSS_DISPLAY_INLINE_BLOCK)) {
            /* Floated block: finish the pending inline line, then place at the
             * current cursor on its side; the vertical cursor does not
             * advance (following content flows around it). */
            if (line_member_count > 0) {
                layout_align_line(ctx, line_members, line_member_count,
                                  content_left, avail_width - RTL_FIRST_CUT, box->s->text_align);
                line_member_count = 0;
            }
            cur.y += cur.line_box;
            cur.line_box = 0.0;
            cur.x = content_left;

            layout_place_float(ctx, c, content_left, avail_width, cur.y + pending_mb, floats, &nfloats,
                               (int)(sizeof(floats) / sizeof(floats[0])));
            continue;
        }

        if (layout_box_is_block_level(child)) {
            /* Block child: finish the pending inline line, then stack. */
            if (line_member_count > 0) {
                layout_align_line(ctx, line_members, line_member_count,
                                  content_left, avail_width - RTL_FIRST_CUT, box->s->text_align);
                line_member_count = 0;
            }
            if (cur.line_box > 0.0) {
                cur.y += cur.line_box;      /* drop any pending inline-line height */
                cur.line_box = 0.0;
                any_placed = true;
                pending_mb = 0.0;           /* line boxes block margin collapse */
            }
            cur.x = content_left;
            block_seen = true;

            /* clear: advance past the matching floats before stacking. */
            bool cleared = false;
            if (child->s->clear) {
                for (int fi = t_float_base; fi < ctx->float_count; fi++) {
                    bool match = (child->s->clear == 3) ||
                                 (child->s->clear == 1 && ctx->float_stack[fi].side == 1) ||
                                 (child->s->clear == 2 && ctx->float_stack[fi].side == 2);
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
            } else if (!any_placed && box->s->overflow == 0 &&
                       box->padding_top <= 0.0 && box->s->border_top <= 0.0 &&
                       idx != ctx->tree.root_idx &&
                       box->s->display != CSS_DISPLAY_FLEX && box->s->display != CSS_DISPLAY_GRID &&
                       box->s->position != CSS_POSITION_ABSOLUTE && box->s->position != CSS_POSITION_FIXED &&
                       !box->s->flow_root &&
                       box->s->table_role != 1 && box->s->table_role != 4 && box->s->table_role != 5) {
                /* Collapses out through this box (its parent applied it via
                 * layout_eff_margin_top).  The root element, and boxes that
                 * establish a new formatting context (absolutely positioned
                 * boxes), keep their children's margins inside (§8.3.1). */
                gap = 0.0;
            } else {
                gap = layout_margin_collapse(pending_mb, eff_mt);
            }

            child->x = content_left + child->margin_left;
            /* <center> centers its block children (-webkit-center). */
            unsigned char mauto = child->s->margin_auto;
            if (in_center && !(mauto & 0xA) && child->s->float_side == 0) mauto |= 0xA;
            if ((mauto & 0xA) && (child->s->width_set || child->s->width_percent > 0.0 || child->s->width_keyword ||
                                  child->max_width >= 0.0)) {
                /* Auto horizontal margins absorb the free space (§10.3.3). */
                double ml = (mauto & 8) ? 0.0 : child->margin_left;
                double mr = (mauto & 2) ? 0.0 : child->margin_right;
                double free_w = avail_width - child->width - ml - mr;
                if (free_w > 0.0) {
                    if ((mauto & 0xA) == 0xA) child->x = content_left + free_w / 2.0;
                    else if (mauto & 8) child->x = content_left + free_w;
                }
            } else if (box->s->direction == 1 && child->s->float_side == 0 &&
                       (child->s->width_set || child->s->width_percent > 0.0 || child->s->width_keyword ||
                        child->max_width >= 0.0)) {
                /* Over-constrained in an rtl container: margin-left is the
                 * one that gives (§10.3.3), the box against the right. */
                child->x = content_left + avail_width - child->margin_right - child->width;
            }
            child->y = cur.y + gap;
            /* A box that establishes a block formatting context (overflow
             * other than visible, a table, flex or grid, flow-root) does not
             * overlap the floats of this one (CSS 2.1 section 9.5): it goes
             * beside them, an auto width narrowed to the room there, or
             * down past them to where it fits. */
            int nfl = ctx->float_count - t_float_base;
            if (nfl > 0 && child->s->float_side == 0 &&
                (child->s->overflow != 0 || child->s->flow_root || table_is_table(child) ||
                 child->s->display == CSS_DISPLAY_FLEX || child->s->display == CSS_DISPLAY_GRID)) {
                const FloatRec *fl = ctx->float_stack + t_float_base;
                bool sized = child->s->width_set || child->s->width_percent > 0.0 || child->s->width_keyword;
                bool auto_w = !sized && !table_is_table(child);
                /* (an auto-width table fits where its min-content width does,
                 * then shrinks to the room) */
                double need = auto_w ? 0.0 : sized ? child->width : layout_intrinsic_width(ctx, c, false);
                double y = child->y, lx = content_left, la = avail_width;
                double content_right = content_left + avail_width;
                /* (a definite height: beside every float over all of it) */
                double bh = child->s->height_set && child->s->height_percent <= 0.0 ? child->height : 0.0;
                for (int guard = 0; guard < 64; guard++) {
                    ifc_band_geometry(ctx, y, bh, content_left, avail_width, &lx, &la);
                    /* (a float edge only where a float intrudes: else the margin, a negative one too) */
                    double left = lx > content_left + 0.01 && lx > content_left + child->margin_left ? lx : content_left + child->margin_left;
                    double right = lx + la < content_right - 0.01 && lx + la < content_right - child->margin_right ? lx + la : content_right - child->margin_right;
                    double room = right - left;
                    if (la >= avail_width - 0.01 || (auto_w ? room > 0.0 : need <= room + 0.01)) break;
                    double next = -1.0;
                    for (int fi = 0; fi < nfl; fi++)
                        if (fl[fi].bottom > y + 0.01 && (next < 0.0 || fl[fi].bottom < next)) next = fl[fi].bottom;
                    if (next < 0.0) break;
                    y = next;
                }
                if (la < avail_width - 0.01) {
                    /* (a float edge only where a float intrudes: else the margin, a negative one too) */
                    double left = lx > content_left + 0.01 && lx > content_left + child->margin_left ? lx : content_left + child->margin_left;
                    double right = lx + la < content_right - 0.01 && lx + la < content_right - child->margin_right ? lx + la : content_right - child->margin_right;
                    child->x = left;
                    if ((auto_w || table_is_table(child)) && right - left < child->width)
                        child->width = right - left > 0.0 ? right - left : 0.0;
                }
                child->y = y;
            }
            layout_update_content_sizes(child);

            /* Recurse so the child's subtree (and thus its auto height) is final
             * before we advance the cursor past it. */
            layout_node_serial(ctx, c);

            double eff_mb = layout_eff_margin_bottom(ctx, c, 0);
            if (child->height <= 0.0 && layout_height_auto(child) && child->s->overflow == 0 &&
                child->padding_top <= 0.0 && child->padding_bottom <= 0.0 &&
                child->s->border_top <= 0.0 && child->s->border_bottom <= 0.0) {
                /* Empty block: top and bottom margins collapse through into a
                 * single margin; the anchor does not move.  (A box that
                 * establishes a new BFC, e.g. overflow:hidden, never
                 * collapses through, §8.3.1.) */
                pending_mb = layout_margin_collapse(pending_mb, layout_margin_collapse(eff_mt, eff_mb));
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
            double line_start = line_left + (!block_seen && cur.y == content_top ? indent0 : 0.0);
            if (cur.x < line_start) cur.x = line_start;
            HtmlNode *dom = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
            if (pending_mb != 0.0 &&
                !(dom && dom->type == HTML_NODE_TEXT && layout_text_is_whitespace(css_layout_node_text(ctx, dom)))) {
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
                /* No soft wrap opportunity between atomics under nowrap / pre. */
                bool line_nowrap = box->s->white_space == 1 || box->s->white_space == 2;
                if (!line_nowrap && cur.x + aspan > line_left + line_avail + 0.01 && cur.x > line_start + 0.001) {
                    if (line_member_count > 0) {
                        layout_align_line(ctx, line_members, line_member_count,
                                          line_left, line_avail - RTL_FIRST_CUT, box->s->text_align);
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
            double fs = child->s->font_size > 0.0 ? child->s->font_size : 16.0;
            float scale = (float)(fs / 16.0);
            double line_adv = layout_line_advance_box(child);
            const char *text = NULL;
            char concat[8192];
            bool is_text_run = false;
            bool is_whitespace_run = false;
            bool is_img = false;
            bool is_br = false;
            child->text_lead_skip = 0;
            /* Preserved segment breaks (pre, pre-wrap, pre-line) force line
             * breaks: the run goes through the wrap path, whose shaper
             * honours '\n'. */
            bool keeps_nl = child->s->white_space >= 2 && child->s->white_space <= 4;
            if (dom && dom->type == HTML_NODE_TEXT) {
                /* (white space only: content where pre / pre-wrap keep its
                 * spaces, or a kept segment break) */
                const char *raw = css_layout_node_text(ctx, dom);
                bool kept_spaces = (child->s->white_space == 2 || child->s->white_space == 3) && raw &&
                                   strpbrk(raw, " \t");
                if (!layout_text_is_whitespace(raw) || kept_spaces || (keeps_nl && raw && strchr(raw, '\n'))) {
                    text = css_layout_box_text(ctx, c, css_layout_node_text(ctx, dom));
                    bool at_start = cur.x <= line_start + 0.001 || (cur.x <= lead_x + 0.001 && cur.y == lead_y);
                    if (text[0] == ' ' && at_start && child->s->white_space != 2 &&
                        child->s->white_space != 3) {
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
                    layout_concat_box_text(ctx, c, concat, sizeof(concat), &cl);
                    if (cl > 0 && !layout_text_is_whitespace(concat)) {
                        text = concat;
                        if (layout_is_space_char(text[0]) &&
                            child->s->white_space != 2 && child->s->white_space != 3 &&
                            (cur.x <= line_start + 0.001 || (cur.x <= lead_x + 0.001 && cur.y == lead_y))) {
                            /* At the start of its line: its leading collapsible
                             * space is removed (from its first text too). */
                            while (layout_is_space_char(*text)) text++;
                            child->text_lead_skip = 1;
                        }
                    } else if (cl > 0 && child->s->display == CSS_DISPLAY_INLINE &&
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
                if (cur.x > line_start + 0.001 && !(cur.x <= lead_x + 0.001 && cur.y == lead_y)) {
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
                /* With preserved segment breaks the text's width is its
                 * longest line, while the intrinsic width below lays the
                 * children out on one line: a break between two children
                 * (YouTube's description runs, "text\n\n" then "more") made it
                 * larger and turned the forced breaks off. */
                bool forced_breaks = keeps_nl && strchr(text, '\n');
                if (!is_text_run && child->s->display == CSS_DISPLAY_INLINE && !forced_breaks) {
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
                } else if (layout_is_svg_box(dom)) {
                    layout_svg_size(ctx, c, dom, &child->width, &child->height);
                    child->width += layout_horizontal_border_padding(child);
                    child->height += layout_vertical_border_padding(child);
                } else if (dom && layout_is_replaced_form_element(dom->tag_name)) {
                    if (child->width <= 0.0) child->width = 80.0;
                    if (child->height <= 0.0) child->height = 20.0;
                } else {
                    /* Inline element without text: size from atomic inline
                     * descendants (icons in links); otherwise empty (anchor
                     * span, empty <a>, ...) shrinks to zero. */
                    double iw = (child->s->display == CSS_DISPLAY_INLINE)
                        ? layout_intrinsic_width(ctx, c, true) : 0.0;
                    if (iw > 0.0) {
                        child->width = iw;
                        child->height = line_adv;
                    } else {
                        /* (width and height don't apply to an inline, §10.3.1, §10.6.1) */
                        bool inl = child->s->display == CSS_DISPLAY_INLINE;
                        if (inl) child->width = layout_horizontal_border_padding(child);
                        else if (!child->s->width_set) child->width = 0.0;
                        if (inl || layout_height_auto(child)) child->height = 0.0;
                    }
                }
            }
            layout_update_content_sizes(child);

            /* Vertical margins on non-replaced inline boxes have no effect
             * (CSS 2.1 §8.4): they neither shift the box on its line nor
             * inflate the line box.  Replaced elements (img, form controls)
             * and inline-blocks keep their vertical margins. */
            bool nonreplaced_inline =
                child->s->display == CSS_DISPLAY_INLINE && !is_img &&
                !(dom && dom->type == HTML_NODE_ELEMENT &&
                  layout_is_replaced_form_element(dom->tag_name));
            double vmt = nonreplaced_inline ? 0.0 : child->margin_top;
            double vmb = nonreplaced_inline ? 0.0 : child->margin_bottom;

            if (is_br) {
                /* <br>: forced line break — finish the current line (its box
                 * height defaults to one line when empty) and start a new one. */
                if (line_member_count > 0) {
                    layout_align_line(ctx, line_members, line_member_count,
                                      line_left, line_avail - RTL_FIRST_CUT, box->s->text_align);
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
            bool nowrap = child->s->white_space == 1 || child->s->white_space == 2;
            bool hard_nl = keeps_nl && text && strchr(text, '\n') &&
                           (is_text_run || child->s->display == CSS_DISPLAY_INLINE);
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
            if (hard_nl || (text && !nowrap && span - trail > remaining + 0.01 && span > fs * 2.0)) {
                double first_w = remaining;
                /* pre: lines end only at newlines. */
                double wrap_avail = hard_nl && nowrap ? 1e6 : line_avail;
                if (hard_nl && nowrap) first_w = 1e6;
                if (cur.x <= line_start + 0.001) {
                    /* Already at the line start: use the full line. */
                    cur.line_box = 0.0;
                    cur.x = line_start;
                    first_w = wrap_avail - (line_start - line_left);
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
                                              line_left, line_avail - RTL_FIRST_CUT, box->s->text_align);
                            line_member_count = 0;
                        }
                        cur.y += cur.line_box;
                        cur.line_box = 0.0;
                        cur.x = line_left;
                        first_w = wrap_avail;
                    }
                }
                if (!hard_nl && span <= first_w) {
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
                    text_shaper_set_spacing(child->s->letter_spacing, child->s->word_spacing, scale);
                    bool wrapped = font && text_shaper_wrap_measure(font, text,
                            (float)(cur.x + child->margin_left), (float)line_left,
                            (float)first_w, (float)wrap_avail, scale, (float)line_adv,
                            cont2_line, (float)cont2_x, (float)cont2_w, &wr);
                    text_shaper_set_spacing(0.0f, 0.0f, 1.0f);
                    if (wrapped) {
                        child->x = cur.x + child->margin_left;
                        child->y = cur.y + vmt;
                        child->width = wr.max_width;
                        child->height = wr.height;
                        child->wrap_first_w = first_w;
                        child->wrap_cont_w = wrap_avail;
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
            if (!(box->s->white_space == 1 || box->s->white_space == 2) &&
                cur.x + span - trail > line_right + 0.01 && cur.x > line_start + 0.001) {
                /* Finish the current line (alignment applies), then wrap. */
                if (line_member_count > 0) {
                    layout_align_line(ctx, line_members, line_member_count,
                                      line_left, line_avail - RTL_FIRST_CUT, box->s->text_align);
                    line_member_count = 0;
                }
                cur.y += cur.line_box;
                cur.line_box = 0.0;
                cur.x = line_left;
            }
            if (is_img && !(box->s->white_space == 1 || box->s->white_space == 2)) {
                /* An image too wide for a line that floats shorten, first on
                 * it, goes down past them till it fits (§9.5). */
                const FloatRec *fl = ctx->float_stack + t_float_base;
                int nf = ctx->float_count - t_float_base;
                for (int guard = 0; guard < 64; guard++) {
                    layout_flow_line_geometry(fl, nf, cur.y, content_left, avail_width, &line_left, &line_avail);
                    if (cur.x < line_left) cur.x = line_left;
                    if (cur.x > line_left + 0.001 || span <= line_avail + 0.01 || line_avail >= avail_width - 0.01)
                        break;
                    double next = -1.0;
                    for (int f = 0; f < nf; f++)
                        if (fl[f].bottom > cur.y + 0.001 && (next < 0.0 || fl[f].bottom < next)) next = fl[f].bottom;
                    if (next < 0.0) break;
                    cur.y = next;
                    cur.x = 0.0;
                }
            }
            child->x = cur.x + child->margin_left;
            child->y = cur.y + vmt;
            cur.x += span;
            if (line_members) line_members[line_member_count++] = c;
            layout_node_serial(ctx, c);
            if (child->s->display == CSS_DISPLAY_INLINE && dom && dom->type == HTML_NODE_ELEMENT) {
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
                /* The inline box keeps its own (font) height; the line box
                 * grows around taller atomic content. */
                if (bottom - cur.y > cur.line_box) cur.line_box = bottom - cur.y;
            }
            double h = vmt + child->height + vmb;
            if (h > cur.line_box) cur.line_box = h;
            if (child->s->display == CSS_DISPLAY_INLINE && dom && dom->type == HTML_NODE_ELEMENT &&
                !is_img && !is_br && child->wrap_cont_w <= 0.0 &&
                fabs(child->height - line_adv) < 0.01) {
                /* Visual box of a one-line inline element: its content area
                 * plus padding/border, centered by the half-leading. */
                double ca = layout_content_area_height(child);
                double vpb = layout_vertical_border_padding(child);
                child->y += (line_adv - ca) / 2.0 - child->padding_top - child->s->border_top;
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
                          line_left, line_avail - RTL_FIRST_CUT, box->s->text_align);
        line_member_count = 0;
    }
    free(line_members);

    cur.y += cur.line_box;  /* finish trailing inline line */

    /* A split inline's own box: around its pieces and blocks (what scripts
     * measure); pieces with no child to follow are placed against it. */
    for (int k = 0; k < nfp; k++) {
        int el = fpieces[k].el;
        if (k + 1 < nfp && fpieces[k + 1].el == el) continue;
        LayoutPieceList *pl = &ctx->pieces[el];
        LayoutBox *eb = layout_box(ctx, el);
        double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
        for (int q = 0; q < pl->n; q++) {
            const LayoutPiece *pc = &pl->p[q];
            if (pc->anchor < 0) continue;
            const LayoutBox *ab = layout_box(ctx, pc->anchor);
            double px = ab->x + pc->dx, py = ab->y + pc->dy;
            if (px < x0) x0 = px;
            if (py < y0) y0 = py;
            if (px + pc->w > x1) x1 = px + pc->w;
            if (py + pc->h > y1) y1 = py + pc->h;
        }
        for (int b = ctx->tree.nodes[el].first_child_idx; b >= 0; b = ctx->tree.nodes[b].next_sibling_idx) {
            const LayoutBox *bb = layout_box(ctx, b);
            if (!layout_is_in_flow(bb) || !layout_box_is_block_level(bb) || bb->s->float_side != 0) continue;
            if (bb->x < x0) x0 = bb->x;
            if (bb->y < y0) y0 = bb->y;
            if (bb->x + bb->width > x1) x1 = bb->x + bb->width;
            if (bb->y + bb->height > y1) y1 = bb->y + bb->height;
        }
        for (int q = 0; q < pl->n; q++) {
            const LayoutPiece *pc = &pl->p[q];
            if (pc->anchor >= 0) continue;
            if (pc->dx < x0) x0 = pc->dx;
            if (pc->dy < y0) y0 = pc->dy;
            if (pc->dx + pc->w > x1) x1 = pc->dx + pc->w;
            if (pc->dy + pc->h > y1) y1 = pc->dy + pc->h;
        }
        if (x1 < x0) continue;
        eb->x = x0;
        eb->y = y0;
        eb->width = x1 - x0;
        eb->height = y1 - y0;
        layout_update_content_sizes(eb);
        for (int q = 0; q < pl->n; q++) {
            if (pl->p[q].anchor >= 0) continue;
            pl->p[q].dx -= x0;
            pl->p[q].dy -= y0;
        }
    }
    free(fpieces);
    free(piece_of);

    free(kids);
    if (!ifc_done) layout_bidi_paragraph(ctx, idx);   /* (on items: its lines placed in visual order already) */

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
        if (box->s->aspect_ratio > 0.0) {
            /* ::before { padding-top: N% } aspect boxes: the pseudo block
             * occupies width x ratio at the top of the content box.  The
             * aspect-ratio property with border-box sizes the border box. */
            double ch = box->content_width * box->s->aspect_ratio;
            if (box->s->aspect_ratio_prop && box->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX)
                ch = box->width * box->s->aspect_ratio - layout_vertical_border_padding(box);
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
        if (box->padding_bottom > 0.0 || box->s->border_bottom > 0.0 || layout_establishes_bfc(ctx, idx))
            max_bottom += pending_mb;
        /* Children that overflowed their line (e.g. tall replaced inline
         * content) can extend below the anchor; take them into account. */
        for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
             c = ctx->tree.nodes[c].next_sibling_idx) {
            LayoutBox *child = layout_box(ctx, c);
            if (!layout_is_in_flow(child)) continue;
            /* (blocks are in cur.y already, after their margins, which may
             * be negative) */
            if (layout_box_is_block_level(child) || child->height <= 0.0) continue;
            double bottom = child->y + child->height;
            if (bottom > max_bottom) max_bottom = bottom;
        }
        /* Floats are out of flow but the container still grows around them
         * (clearfix-free pages like Wikipedia rely on this visually). */
        for (int fi = 0; fi < nfloats; fi++) {
            if (floats[fi].bottom > max_bottom) max_bottom = floats[fi].bottom;
        }
        if (box->s->line_clamp && box->s->webkit_box) {
            double cap = content_top + box->s->line_clamp * layout_line_advance_box(box);
            if (max_bottom > cap) max_bottom = cap;
        }
        if (max_bottom < content_top) max_bottom = content_top;   /* (no negative content height) */
        double total = max_bottom - box->y + box->padding_bottom + box->s->border_bottom;
        if (total < 0.0) total = 0.0;
        /* min/max-height clamps (content-box aware). */
        if (box->min_height > 0.0 || box->max_height >= 0.0) {
            if (box->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX)
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
#undef RTL_FIRST_CUT

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
    if (b->s->display == CSS_DISPLAY_FLEX || b->s->display == CSS_DISPLAY_GRID) return true;
    HtmlNode *dn = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    if (dn && dn->type == HTML_NODE_ELEMENT && dn->tag_name &&
        strcasecmp(dn->tag_name, "button") == 0) return true;   /* centers its content */
    for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
         c = ctx->tree.nodes[c].next_sibling_idx) {
        LayoutBox *cb = layout_box(ctx, c);
        if (cb->s->display == CSS_DISPLAY_NONE) continue;
        if (cb->s->height_percent > 0.0) return true;
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
    bool is_row = (ct->s->flex_direction == CSS_FLEX_DIRECTION_ROW ||
                   ct->s->flex_direction == CSS_FLEX_DIRECTION_ROW_REVERSE);
    bool reverse = (ct->s->flex_direction == CSS_FLEX_DIRECTION_ROW_REVERSE ||
                    ct->s->flex_direction == CSS_FLEX_DIRECTION_COLUMN_REVERSE);
    bool do_wrap = ct->s->flex_wrap != CSS_FLEX_WRAP_NOWRAP;
    bool wrap_rev = ct->s->flex_wrap == CSS_FLEX_WRAP_WRAP_REVERSE;

    double content_x = ct->x + ct->padding_left + ct->s->border_left;
    double content_y = ct->y + ct->padding_top + ct->s->border_top;
    double cw = ct->content_width;
    bool height_def = !layout_height_auto(ct) || ct->height > 0.0;
    double chh = height_def ? ct->content_height : 0.0;
    bool main_def = is_row ? true : height_def;
    bool cross_def = is_row ? height_def : true;
    double avail_main = is_row ? cw : chh;
    double gap_main = is_row ? ct->s->gap_col : ct->s->gap_row;
    double gap_cross = is_row ? ct->s->gap_row : ct->s->gap_col;
    double main_start = is_row ? content_x : content_y;
    double cross_start = is_row ? content_y : content_x;

    /* Container min/max on each axis, as content-box sizes. */
    double cmin_h = ct->min_height, cmax_h = ct->max_height;
    if (ct->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
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
            if (kn && kn->type == HTML_NODE_TEXT && layout_text_is_whitespace(css_layout_node_text(ctx, kn)))
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
        it->order = child->s->order;
        it->grow = child->s->flex_grow;
        it->shrink = child->s->flex_shrink;
        it->align = child->s->align_self ? child->s->align_self - 1 : (int)ct->s->align_items;

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
        bool w_def = child->s->width_set || child->s->width_percent > 0.0 || child->s->width_keyword;
        bool h_def = !layout_height_auto(child);
        if (!w_def) child->width = 0.0;
        if (!h_def) child->height = 0.0;
        layout_update_content_sizes(child);

        unsigned char ma = child->s->margin_auto;
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
        if (child->s->flex_basis_percent > 0.0) {
            if (main_def) basis = layout_flex_main_total(child, avail_main * child->s->flex_basis_percent + child->s->flex_basis_pct_px, is_row);
        } else if (child->s->flex_basis >= 0.0) {
            basis = layout_flex_main_total(child, child->s->flex_basis, is_row);
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
        if (it->min_main < 0.0 && (child->s->min_size_set & (is_row ? 1 : 2))) it->min_main = 0.0;
        if (it->min_main < 0.0) {
            /* Automatic minimum size (§4.5): the content size for items that
             * are not scroll containers, capped by a definite size. */
            it->min_main = 0.0;
            if (child->s->overflow == 0 && is_row) {
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
            if (child->s->overflow == 0 && !(child->s->min_size_set & 2) && child->min_height <= 0.0) {
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
            switch (ct->s->justify_content) {
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

/* ---- Grid layout (CSS Grid §7-§12) ----
 * 1. the track lists (repeat(), auto-fill/-fit, minmax(), fit-content(),
 *    line names) and grid-template-areas, parsed against the final font size;
 * 2. item placement: definite lines, spans, areas and named lines, then
 *    auto-placement (row or column flow, sparse or dense) with implicit
 *    tracks sized by grid-auto-rows/-columns;
 * 3. column sizing from the items' min/max-content contributions (§12.5:
 *    intrinsic sizes, spanning items, maximize, flexible tracks, stretched
 *    auto tracks), items laid out at their column widths, then row sizing
 *    from their heights;
 * 4. alignment: justify-/align-self (stretch by default) and
 *    justify-content over the columns. */

enum { GT_LEN, GT_FR, GT_AUTO, GT_MINC, GT_MAXC, GT_FIT };

typedef struct {
    unsigned char kmin, kmax;          /* GT_*: min and max sizing functions */
    double min_px, min_pct;            /* GT_LEN min */
    double max_px, max_pct;            /* GT_LEN max; flex factor for GT_FR; limit for GT_FIT */
} GridTrackDef;

#define GRID_MAX_TRACKS 512
#define GRID_MAX_NAMES 64
#define GRID_MAX_AREAS 64

typedef struct { const char *name; size_t len; int line; } GridLineName;

typedef struct {
    GridTrackDef t[GRID_MAX_TRACKS];
    int n;
    GridLineName names[GRID_MAX_NAMES];
    int nnames;
    int fit_start, fit_count;          /* repeat(auto-fit, ...) tracks: empty ones collapse */
} GridTrackList;

typedef struct { const char *name; size_t len; int r0, r1, c0, c1; } GridArea;

/* Next whitespace-separated token of a track list, keeping (...) and [...]
 * together.  Returns false at the end. */
static bool grid_token(const char **p, const char **s, size_t *n)
{
    const char *q = *p;
    while (*q && (isspace((unsigned char)*q) || *q == ',')) q++;
    if (!*q) { *p = q; return false; }
    const char *st = q;
    int depth = 0;
    while (*q && (depth > 0 || (!isspace((unsigned char)*q) && *q != ','))) {
        if (*q == '(' || *q == '[') depth++;
        else if ((*q == ')' || *q == ']') && depth > 0) depth--;
        q++;
    }
    *s = st;
    *n = (size_t)(q - st);
    *p = q;
    return true;
}

static bool grid_breadth(const char *s, size_t n, double fs, unsigned char *k, double *px, double *pct)
{
    char buf[128];
    if (n == 0 || n >= sizeof(buf)) return false;
    memcpy(buf, s, n);
    buf[n] = '\0';
    *px = *pct = 0.0;
    if (strcasecmp(buf, "auto") == 0) { *k = GT_AUTO; return true; }
    if (strcasecmp(buf, "min-content") == 0) { *k = GT_MINC; return true; }
    if (strcasecmp(buf, "max-content") == 0) { *k = GT_MAXC; return true; }
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end != buf && strcasecmp(end, "fr") == 0) { *k = GT_FR; *px = v < 0.0 ? 0.0 : v; return true; }
    if (end == buf && !strchr(buf, '(')) return false;
    CssLen l = css_parse_len(buf, fs);
    *k = GT_LEN;
    *px = l.px;
    *pct = l.pct;
    return true;
}

static bool grid_track_size(const char *s, size_t n, double fs, GridTrackDef *t)
{
    memset(t, 0, sizeof(*t));
    if (n > 7 && strncasecmp(s, "minmax(", 7) == 0 && s[n - 1] == ')') {
        const char *in = s + 7, *e = s + n - 1, *comma = in;
        int depth = 0;
        for (; comma < e; comma++) {
            if (*comma == '(') depth++;
            else if (*comma == ')') depth--;
            else if (*comma == ',' && depth == 0) break;
        }
        if (comma >= e) return false;
        const char *a = in, *ae = comma, *b = comma + 1, *be = e;
        while (a < ae && isspace((unsigned char)*a)) a++;
        while (ae > a && isspace((unsigned char)ae[-1])) ae--;
        while (b < be && isspace((unsigned char)*b)) b++;
        while (be > b && isspace((unsigned char)be[-1])) be--;
        if (!grid_breadth(a, (size_t)(ae - a), fs, &t->kmin, &t->min_px, &t->min_pct)) return false;
        if (!grid_breadth(b, (size_t)(be - b), fs, &t->kmax, &t->max_px, &t->max_pct)) return false;
        if (t->kmin == GT_FR) { t->kmin = GT_AUTO; t->min_px = 0.0; }
        return true;
    }
    if (n > 12 && strncasecmp(s, "fit-content(", 12) == 0 && s[n - 1] == ')') {
        unsigned char k;
        if (!grid_breadth(s + 12, n - 13, fs, &k, &t->max_px, &t->max_pct) || k != GT_LEN) return false;
        t->kmin = GT_AUTO;
        t->kmax = GT_FIT;
        return true;
    }
    unsigned char k;
    double px, pct;
    if (!grid_breadth(s, n, fs, &k, &px, &pct)) return false;
    if (k == GT_FR) { t->kmin = GT_AUTO; t->kmax = GT_FR; t->max_px = px; return true; }
    t->kmin = t->kmax = k;
    t->min_px = t->max_px = px;
    t->min_pct = t->max_pct = pct;
    return true;
}

/* A track's definite size for counting auto repetitions (§7.2.3.2). */
static double grid_track_fixed(const GridTrackDef *t, double avail)
{
    if (t->kmax == GT_LEN) return t->max_px + t->max_pct * (avail > 0.0 ? avail : 0.0);
    if (t->kmin == GT_LEN) return t->min_px + t->min_pct * (avail > 0.0 ? avail : 0.0);
    return 0.0;
}

static void grid_add_names(GridTrackList *L, const char *s, size_t n, int line)
{
    const char *p = s + 1, *e = s + n - (s[n - 1] == ']' ? 1 : 0);
    while (p < e) {
        while (p < e && isspace((unsigned char)*p)) p++;
        const char *w = p;
        while (p < e && !isspace((unsigned char)*p)) p++;
        if (p > w && L->nnames < GRID_MAX_NAMES) {
            L->names[L->nnames].name = w;
            L->names[L->nnames].len = (size_t)(p - w);
            L->names[L->nnames].line = line;
            L->nnames++;
        }
    }
}

/* One track list into L.  avail (< 0: indefinite) sizes auto repetitions. */
static void grid_parse_tracks(const char *src, double fs, double avail, double gap, GridTrackList *L)
{
    L->n = 0;
    L->nnames = 0;
    L->fit_start = L->fit_count = 0;
    if (!src) return;
    /* The space the tracks outside an auto repetition take. */
    double other = 0.0;
    int nother = 0;
    const char *p = src, *s;
    size_t n;
    while (grid_token(&p, &s, &n)) {
        if (*s == '[' || (n > 7 && strncasecmp(s, "repeat(", 7) == 0)) continue;
        GridTrackDef t;
        if (grid_track_size(s, n, fs, &t)) { other += grid_track_fixed(&t, avail); nother++; }
    }
    p = src;
    while (grid_token(&p, &s, &n) && L->n < GRID_MAX_TRACKS) {
        if (*s == '[') { grid_add_names(L, s, n, L->n); continue; }
        if (n > 8 && strncasecmp(s, "repeat(", 7) == 0 && s[n - 1] == ')') {
            const char *in = s + 7, *e = s + n - 1, *comma = in;
            while (comma < e && *comma != ',') comma++;
            if (comma >= e) continue;
            char cnt[32];
            size_t cl = 0;
            for (const char *c = in; c < comma && cl + 1 < sizeof(cnt); c++)
                if (!isspace((unsigned char)*c)) cnt[cl++] = *c;
            cnt[cl] = '\0';
            /* The repeated list (no nested repeat()). */
            GridTrackDef rep[64];
            struct { const char *s; size_t n; int at; } rnames[16];
            int nrep = 0, nrn = 0;
            char inner[1024];
            size_t il = (size_t)(e - comma - 1);
            if (il >= sizeof(inner)) il = sizeof(inner) - 1;
            memcpy(inner, comma + 1, il);
            inner[il] = '\0';
            const char *ip = inner, *is;
            size_t in_n;
            while (grid_token(&ip, &is, &in_n)) {
                if (*is == '[') {
                    if (nrn < 16) { rnames[nrn].s = is; rnames[nrn].n = in_n; rnames[nrn].at = nrep; nrn++; }
                    continue;
                }
                if (nrep < 64 && grid_track_size(is, in_n, fs, &rep[nrep])) nrep++;
            }
            if (nrep == 0) continue;
            int count;
            bool fit = strcasecmp(cnt, "auto-fit") == 0;
            if (fit || strcasecmp(cnt, "auto-fill") == 0) {
                double per = 0.0;
                for (int i = 0; i < nrep; i++) per += grid_track_fixed(&rep[i], avail);
                per += gap * nrep;
                count = 1;
                if (avail > 0.0 && per > 0.0) {
                    double room = avail - other - gap * nother + gap;
                    count = (int)floor((room + 1e-6) / per);
                    if (count < 1) count = 1;
                }
                if (fit) { L->fit_start = L->n; L->fit_count = count * nrep; }
            } else {
                count = atoi(cnt);
                if (count < 1) count = 1;
            }
            for (int r = 0; r < count && L->n < GRID_MAX_TRACKS; r++) {
                int base = L->n;
                for (int i = 0; i < nrep && L->n < GRID_MAX_TRACKS; i++) L->t[L->n++] = rep[i];
                for (int k = 0; k < nrn; k++) {
                    /* (inner points into a stack buffer: intern the names) */
                    const char *nm = layout_intern(rnames[k].s, rnames[k].n);
                    if (nm) grid_add_names(L, nm, rnames[k].n, base + rnames[k].at);
                }
            }
            if (fit && L->fit_count > L->n - L->fit_start) L->fit_count = L->n - L->fit_start;
            continue;
        }
        if (grid_track_size(s, n, fs, &L->t[L->n])) L->n++;
    }
}

/* grid-template-areas: each named area's rectangle (0-based lines). */
static int grid_parse_areas(const char *src, GridArea *areas, int max, int *rows, int *cols)
{
    int na = 0, r = 0;
    *rows = *cols = 0;
    if (!src) return 0;
    const char *p = src;
    while (*p) {
        while (*p && *p != '"' && *p != '\'') p++;
        if (!*p) break;
        char q = *p++;
        int c = 0;
        while (*p && *p != q) {
            while (*p && *p != q && isspace((unsigned char)*p)) p++;
            if (!*p || *p == q) break;
            const char *w = p;
            if (*p == '.') {
                while (*p == '.') p++;
            } else {
                while (*p && *p != q && !isspace((unsigned char)*p) && *p != '.') p++;
                size_t len = (size_t)(p - w);
                int a = 0;
                while (a < na && !(areas[a].len == len && memcmp(areas[a].name, w, len) == 0)) a++;
                if (a == na && na < max) {
                    areas[na].name = w; areas[na].len = len;
                    areas[na].r0 = r; areas[na].r1 = r + 1;
                    areas[na].c0 = c; areas[na].c1 = c + 1;
                    na++;
                } else if (a < na) {
                    if (r + 1 > areas[a].r1) areas[a].r1 = r + 1;
                    if (c + 1 > areas[a].c1) areas[a].c1 = c + 1;
                    if (c < areas[a].c0) areas[a].c0 = c;
                }
            }
            c++;
        }
        if (*p) p++;
        if (c > *cols) *cols = c;
        r++;
    }
    *rows = r;
    return na;
}

typedef struct {
    GridTrackList cols, rows;
    GridArea areas[GRID_MAX_AREAS];
    int nareas, area_rows, area_cols;
} GridDefs;

/* A named line: an area's edge, name-start / name-end, or an explicit name. */
static int grid_named_line(const GridDefs *D, bool rows, const char *nm, size_t len, bool end)
{
    for (int a = 0; a < D->nareas; a++)
        if (D->areas[a].len == len && memcmp(D->areas[a].name, nm, len) == 0)
            return rows ? (end ? D->areas[a].r1 : D->areas[a].r0) : (end ? D->areas[a].c1 : D->areas[a].c0);
    const GridTrackList *L = rows ? &D->rows : &D->cols;
    for (int i = 0; i < L->nnames; i++)
        if (L->names[i].len == len && memcmp(L->names[i].name, nm, len) == 0) return L->names[i].line;
    /* name-start / name-end of an area */
    if (len > 6 && (memcmp(nm + len - 6, "-start", 6) == 0 || (len > 4 && memcmp(nm + len - 4, "-end", 4) == 0))) {
        bool is_end = memcmp(nm + len - 4, "-end", 4) == 0;
        size_t bl = len - (is_end ? 4 : 6);
        for (int a = 0; a < D->nareas; a++)
            if (D->areas[a].len == bl && memcmp(D->areas[a].name, nm, bl) == 0)
                return rows ? (is_end ? D->areas[a].r1 : D->areas[a].r0) : (is_end ? D->areas[a].c1 : D->areas[a].c0);
    }
    return -1;
}

/* One placement property: *line (0-based) when definite, else *span
 * (>= 1) from "span N"; both unset for auto. */
static void grid_line_value(const GridDefs *D, bool rows, const char *v, bool end, int explicit_lines,
                            int *line, int *span)
{
    *line = -1;
    *span = 0;
    if (!v) return;
    while (isspace((unsigned char)*v)) v++;
    if (strncasecmp(v, "span", 4) == 0 && (isspace((unsigned char)v[4]) || !v[4])) {
        int n = atoi(v + 4);
        *span = n > 0 ? n : 1;
        return;
    }
    char *e = NULL;
    long n = strtol(v, &e, 10);
    if (e != v) {
        if (n > 0) *line = (int)n - 1;
        else if (n < 0) *line = explicit_lines + (int)n;
        if (*line < 0) *line = 0;
        return;
    }
    size_t len = 0;
    while (v[len] && !isspace((unsigned char)v[len])) len++;
    int l = grid_named_line(D, rows, v, len, end);
    if (l >= 0) *line = l;
}

/* An item's start track and span on one axis; false when it is auto. */
static bool grid_resolve_axis(const GridDefs *D, bool rows, const char *sv, const char *ev, int explicit_tracks,
                              int *start, int *span)
{
    int sl, ss, el, es;
    grid_line_value(D, rows, sv, false, explicit_tracks + 1, &sl, &ss);
    grid_line_value(D, rows, ev, true, explicit_tracks + 1, &el, &es);
    if (sl >= 0 && el >= 0) {
        if (el < sl) { int t = sl; sl = el; el = t; }
        *start = sl;
        *span = el > sl ? el - sl : 1;
        return true;
    }
    if (sl >= 0) { *start = sl; *span = es > 0 ? es : 1; return true; }
    if (el >= 0) {
        int sp = ss > 0 ? ss : 1;
        *start = el - sp < 0 ? 0 : el - sp;
        *span = el - *start > 0 ? el - *start : 1;
        return true;
    }
    *span = ss > 0 ? ss : (es > 0 ? es : 1);
    return false;
}

typedef struct {
    int idx;
    bool is_text;
    int r, c, rs, cs;                  /* start track and span on each axis */
    double min_w, max_w;               /* column contributions (margin box) */
    double h;                          /* margin-box height at its column width */
    int order, seq;
} GridItem;

typedef struct {
    unsigned char kmin, kmax;
    double minv, limit, fr;            /* fixed minimum; fixed maximum / fit-content limit; flex factor */
    double base, growth;               /* growth < 0: infinite */
    bool collapsed;
} GridSize;

static int grid_item_order_cmp(const void *a, const void *b)
{
    const GridItem *x = (const GridItem *)a, *y = (const GridItem *)b;
    if (x->order != y->order) return x->order < y->order ? -1 : 1;
    return x->seq - y->seq;
}

static bool grid_occupied(const unsigned char *occ, int M, int maj, int majspan, int mn, int mnspan, int nmaj)
{
    for (int a = maj; a < maj + majspan; a++) {
        if (a >= nmaj) return false;
        for (int b = mn; b < mn + mnspan; b++) if (occ[(size_t)a * M + b]) return true;
    }
    return false;
}

/* Auto-placement (§8.5).  The major axis is rows for row flow (it grows),
 * the minor one has M tracks. */
static int grid_place_items(GridItem *it, int n, const bool *rdef, const bool *cdef, bool column_flow,
                            bool dense, int ncols_explicit, int nrows_explicit, int *out_cols)
{
    int M = column_flow ? nrows_explicit : ncols_explicit;
    int nmaj = column_flow ? ncols_explicit : nrows_explicit;
    for (int i = 0; i < n; i++) {
        bool mdef = column_flow ? rdef[i] : cdef[i];
        int mn = column_flow ? it[i].r : it[i].c, mns = column_flow ? it[i].rs : it[i].cs;
        if (mdef && mn + mns > M) M = mn + mns;
        if (!mdef && mns > M) M = mns;
        bool jdef = column_flow ? cdef[i] : rdef[i];
        int mj = column_flow ? it[i].c : it[i].r, mjs = column_flow ? it[i].cs : it[i].rs;
        if (jdef && mj + mjs > nmaj) nmaj = mj + mjs;
    }
    if (M < 1) M = 1;
    int cap = nmaj + n * 2 + 8;
    int span_max = 1;
    for (int i = 0; i < n; i++) {
        int mjs = column_flow ? it[i].cs : it[i].rs;
        if (mjs > span_max) span_max = mjs;
    }
    cap += span_max * n;
    if (cap > 100000) cap = 100000;
    unsigned char *occ = (unsigned char *)calloc((size_t)cap * (size_t)M, 1);
    if (!occ) { *out_cols = M; return nmaj; }
#define MAJ(i) (column_flow ? it[i].c : it[i].r)
#define MAJS(i) (column_flow ? it[i].cs : it[i].rs)
#define MIN_(i) (column_flow ? it[i].r : it[i].c)
#define MINS(i) (column_flow ? it[i].rs : it[i].cs)
#define SETPOS(i, mj, mn) do { if (column_flow) { it[i].c = (mj); it[i].r = (mn); } \
                               else { it[i].r = (mj); it[i].c = (mn); } } while (0)
    int used = 0;
    /* 1. both axes definite */
    for (int i = 0; i < n; i++) {
        if (!(rdef[i] && cdef[i])) continue;
        for (int a = MAJ(i); a < MAJ(i) + MAJS(i) && a < cap; a++)
            for (int b = MIN_(i); b < MIN_(i) + MINS(i) && b < M; b++) occ[(size_t)a * M + b] = 1;
        if (MAJ(i) + MAJS(i) > used) used = MAJ(i) + MAJS(i);
    }
    /* 2. major axis definite only */
    for (int i = 0; i < n; i++) {
        bool jdef = column_flow ? cdef[i] : rdef[i], mdef = column_flow ? rdef[i] : cdef[i];
        if (!jdef || mdef) continue;
        int mj = MAJ(i), mjs = MAJS(i), mns = MINS(i), at = 0;
        while (at + mns <= M && grid_occupied(occ, M, mj, mjs, at, mns, cap)) at++;
        if (at + mns > M) at = 0;
        SETPOS(i, mj, at);
        for (int a = mj; a < mj + mjs && a < cap; a++)
            for (int b = at; b < at + mns && b < M; b++) occ[(size_t)a * M + b] = 1;
        if (mj + mjs > used) used = mj + mjs;
    }
    /* 3. the rest, in order */
    int cr = 0, cc = 0;
    for (int i = 0; i < n; i++) {
        bool jdef = column_flow ? cdef[i] : rdef[i], mdef = column_flow ? rdef[i] : cdef[i];
        if (jdef) continue;
        int mjs = MAJS(i), mns = MINS(i);
        if (dense) { cr = 0; cc = 0; }
        if (mdef) {
            int mn = MIN_(i);
            if (!dense && mn < cc) cr++;
            while (cr + mjs <= cap && grid_occupied(occ, M, cr, mjs, mn, mns, cap)) cr++;
            if (cr + mjs > cap) cr = cap - mjs;
            SETPOS(i, cr, mn);
            cc = mn + mns;
        } else {
            for (;;) {
                if (cc + mns > M) { cr++; cc = 0; }
                if (cr + mjs > cap) { cr = cap - mjs; break; }
                if (!grid_occupied(occ, M, cr, mjs, cc, mns, cap)) break;
                cc++;
            }
            SETPOS(i, cr, cc);
            cc += mns;
        }
        for (int a = MAJ(i); a < MAJ(i) + mjs && a < cap; a++)
            for (int b = MIN_(i); b < MIN_(i) + mns && b < M; b++) occ[(size_t)a * M + b] = 1;
        if (MAJ(i) + mjs > used) used = MAJ(i) + mjs;
    }
#undef MAJ
#undef MAJS
#undef MIN_
#undef MINS
#undef SETPOS
    free(occ);
    *out_cols = M;
    return used > nmaj ? used : nmaj;
}

/* Track sizing (§12.3-§12.7) on one axis.  avail < 0: indefinite.  mode 0
 * lays out, 1 / 2 size under a min- / max-content constraint (the
 * container's intrinsic widths).  Contributions: columns use min_w / max_w,
 * rows h. */
static void grid_size_tracks(GridSize *t, int n, double avail, double gap, const GridItem *it, int nit,
                             bool rows, int mode, bool stretch)
{
    bool def = avail >= 0.0 && mode == 0;
    for (int i = 0; i < n; i++) {
        GridSize *g = &t[i];
        if (g->collapsed) { g->base = g->growth = 0.0; continue; }
        g->base = g->kmin == GT_LEN ? g->minv : 0.0;
        g->growth = -1.0;
    }
    /* Span-1 items. */
    for (int k = 0; k < nit; k++) {
        int s = rows ? it[k].r : it[k].c, sp = rows ? it[k].rs : it[k].cs;
        if (sp != 1 || s >= n) continue;
        GridSize *g = &t[s];
        if (g->collapsed) continue;
        double cmin = rows ? it[k].h : it[k].min_w, cmax = rows ? it[k].h : it[k].max_w;
        if (mode == 1) cmax = cmin;
        if (g->kmin == GT_AUTO || g->kmin == GT_MINC) { if (cmin > g->base) g->base = cmin; }
        else if (g->kmin == GT_MAXC) { if (cmax > g->base) g->base = cmax; }
        if (g->kmax == GT_MINC) { if (cmin > g->growth) g->growth = cmin; }
        else if (g->kmax == GT_MAXC || g->kmax == GT_AUTO) { if (cmax > g->growth) g->growth = cmax; }
        else if (g->kmax == GT_FIT) {
            double v = cmax < g->limit ? cmax : g->limit;
            if (v > g->growth) g->growth = v;
        } else if (g->kmax == GT_FR) {
            if (cmax / (g->fr > 1.0 ? g->fr : 1.0) > g->growth) g->growth = cmax / (g->fr > 1.0 ? g->fr : 1.0);
        }
    }
    for (int i = 0; i < n; i++) {
        GridSize *g = &t[i];
        if (g->kmax == GT_FR) continue;
        if (g->kmax == GT_LEN) g->growth = g->limit;
        else if (g->kmax == GT_FIT && g->growth > g->limit) g->growth = g->limit;
        if (g->growth < 0.0) g->growth = g->base;
        if (g->growth < g->base) g->growth = g->base;
    }
    /* Spanning items: grow the intrinsic tracks they cross. */
    for (int k = 0; k < nit; k++) {
        int s = rows ? it[k].r : it[k].c, sp = rows ? it[k].rs : it[k].cs;
        if (sp < 2 || s >= n) continue;
        int e = s + sp > n ? n : s + sp;
        double cmin = rows ? it[k].h : it[k].min_w, cmax = rows ? it[k].h : it[k].max_w;
        if (mode == 1) cmax = cmin;
        double sum_b = gap * (e - s - 1), sum_g = sum_b;
        int nint = 0, nflex = 0, nmax = 0;
        for (int i = s; i < e; i++) {
            sum_b += t[i].base;
            sum_g += t[i].kmax == GT_FR ? t[i].base : t[i].growth;
            if (t[i].kmax == GT_FR) nflex++;
            else {
                if (t[i].kmin == GT_AUTO || t[i].kmin == GT_MINC || t[i].kmin == GT_MAXC) nint++;
                if (t[i].kmax != GT_LEN) nmax++;
            }
        }
        if (nflex > 0) {
            double extra = cmin - sum_b;
            if (extra > 0.0)
                for (int i = s; i < e; i++)
                    if (t[i].kmax == GT_FR && t[i].kmin != GT_LEN) t[i].base += extra / nflex;
            continue;
        }
        double extra = cmin - sum_b;
        if (extra > 0.0 && nint > 0)
            for (int i = s; i < e; i++)
                if (t[i].kmin == GT_AUTO || t[i].kmin == GT_MINC || t[i].kmin == GT_MAXC) {
                    t[i].base += extra / nint;
                    if (t[i].growth < t[i].base) t[i].growth = t[i].base;
                }
        extra = cmax - sum_g;
        if (extra > 0.0 && nmax > 0)
            for (int i = s; i < e; i++)
                if (t[i].kmax != GT_LEN && t[i].kmax != GT_FR) t[i].growth += extra / nmax;
    }
    /* Maximize tracks: free space up to their growth limits. */
    double gaps = 0.0;
    int nlive = 0;
    for (int i = 0; i < n; i++) if (!t[i].collapsed) nlive++;
    gaps = nlive > 1 ? gap * (nlive - 1) : 0.0;
    if (def) {
        double used = gaps;
        for (int i = 0; i < n; i++) used += t[i].base;
        double free_space = avail - used;
        for (int iter = 0; iter < 8 && free_space > 0.01; iter++) {
            int m = 0;
            for (int i = 0; i < n; i++) if (t[i].kmax != GT_FR && t[i].growth > t[i].base + 0.001) m++;
            if (m == 0) break;
            double share = free_space / m;
            for (int i = 0; i < n; i++) {
                if (t[i].kmax == GT_FR || t[i].growth <= t[i].base + 0.001) continue;
                double add = t[i].growth - t[i].base < share ? t[i].growth - t[i].base : share;
                t[i].base += add;
                free_space -= add;
            }
        }
    } else if (mode != 1) {
        for (int i = 0; i < n; i++) if (t[i].kmax != GT_FR) t[i].base = t[i].growth;
    }
    /* Flexible tracks (§12.7). */
    double sum_fr = 0.0;
    for (int i = 0; i < n; i++) if (t[i].kmax == GT_FR && !t[i].collapsed) sum_fr += t[i].fr;
    if (sum_fr > 0.0) {
        double fr_size = 0.0;
        if (def) {
            double leftover = avail - gaps;
            for (int i = 0; i < n; i++) if (t[i].kmax != GT_FR) leftover -= t[i].base;
            bool *inflex = (bool *)calloc((size_t)n, sizeof(bool));
            double sf = sum_fr, left = leftover;
            for (int iter = 0; iter <= n; iter++) {
                fr_size = left / (sf > 1.0 ? sf : 1.0);
                bool again = false;
                for (int i = 0; i < n; i++) {
                    if (t[i].kmax != GT_FR || t[i].collapsed || (inflex && inflex[i])) continue;
                    if (t[i].base > fr_size * t[i].fr) {
                        if (inflex) inflex[i] = true;
                        left -= t[i].base;
                        sf -= t[i].fr;
                        again = true;
                    }
                }
                if (!again || !inflex) break;
            }
            free(inflex);
        } else {
            /* Indefinite: the largest base (or max-content) per fr. */
            for (int i = 0; i < n; i++) {
                if (t[i].kmax != GT_FR || t[i].collapsed) continue;
                double f = t[i].fr > 1.0 ? t[i].fr : 1.0;
                double v = t[i].base / f;
                if (t[i].growth > v) v = t[i].growth;   /* (span-1 contributions per fr) */
                if (v > fr_size) fr_size = v;
            }
        }
        if (fr_size < 0.0) fr_size = 0.0;
        for (int i = 0; i < n; i++)
            if (t[i].kmax == GT_FR && !t[i].collapsed && fr_size * t[i].fr > t[i].base) t[i].base = fr_size * t[i].fr;
    }
    /* Stretch auto tracks (justify/align-content: normal). */
    if (def && stretch) {
        double used = gaps;
        int m = 0;
        for (int i = 0; i < n; i++) { used += t[i].base; if (t[i].kmax == GT_AUTO && !t[i].collapsed) m++; }
        if (avail - used > 0.01 && m > 0)
            for (int i = 0; i < n; i++)
                if (t[i].kmax == GT_AUTO && !t[i].collapsed) t[i].base += (avail - used) / m;
    }
}

/* Track definitions for n tracks: the explicit list, then the auto list. */
static void grid_init_sizes(GridSize *t, int n, const GridTrackList *L, const GridTrackList *A, double avail)
{
    for (int i = 0; i < n; i++) {
        GridTrackDef d;
        if (i < L->n) d = L->t[i];
        else if (A->n > 0) d = A->t[(i - L->n) % A->n];
        else { memset(&d, 0, sizeof(d)); d.kmin = d.kmax = GT_AUTO; }
        /* Percentages of an indefinite size behave as auto. */
        if (d.kmin == GT_LEN && d.min_pct != 0.0 && avail < 0.0) d.kmin = GT_AUTO;
        if (d.kmax == GT_LEN && d.max_pct != 0.0 && avail < 0.0) d.kmax = GT_AUTO;
        t[i].kmin = d.kmin;
        t[i].kmax = d.kmax;
        t[i].fr = d.kmax == GT_FR ? d.max_px : 0.0;
        t[i].collapsed = false;
        double a = avail > 0.0 ? avail : 0.0;
        t[i].minv = d.kmin == GT_LEN ? d.min_px + d.min_pct * a : 0.0;
        t[i].limit = (d.kmax == GT_LEN || d.kmax == GT_FIT) ? d.max_px + d.max_pct * a : 0.0;
        if (d.kmax == GT_LEN && t[i].limit < t[i].minv) t[i].limit = t[i].minv;
    }
}

typedef struct {
    LayoutContext *ctx;
    int idx;
    GridDefs *D;
    GridItem *items;
    int nitems;
    int ncols, nrows;
    GridSize *cols, *rows;
} GridState;

static void grid_state_free(GridState *G)
{
    free(G->D);
    free(G->items);
    free(G->cols);
    free(G->rows);
}

/* Parse the container's templates, collect and place its items.  cw / ch
 * < 0: indefinite. */
static bool grid_setup(LayoutContext *ctx, int idx, double cw, double ch, GridState *G)
{
    LayoutBox *ct = layout_box(ctx, idx);
    memset(G, 0, sizeof(*G));
    G->ctx = ctx;
    G->idx = idx;
    double fs = ct->s->font_size > 0.0 ? ct->s->font_size : 16.0;
    GridDefs *D = (GridDefs *)calloc(1, sizeof(GridDefs));
    if (!D) return false;
    G->D = D;
    grid_parse_tracks(ct->s->grid_cols, fs, cw, ct->s->gap_col, &D->cols);
    grid_parse_tracks(ct->s->grid_rows, fs, ch, ct->s->gap_row, &D->rows);
    D->nareas = grid_parse_areas(ct->s->grid_areas, D->areas, GRID_MAX_AREAS, &D->area_rows, &D->area_cols);
    int ecols = D->cols.n > D->area_cols ? D->cols.n : D->area_cols;
    int erows = D->rows.n > D->area_rows ? D->rows.n : D->area_rows;

    int nkids = 0;
    int *kids = layout_collect_flow_children(ctx, idx, &nkids);
    G->items = nkids > 0 ? (GridItem *)calloc((size_t)nkids, sizeof(GridItem)) : NULL;
    bool *rdef = nkids > 0 ? (bool *)calloc((size_t)nkids * 2, sizeof(bool)) : NULL;
    if (nkids > 0 && (!G->items || !rdef)) { free(kids); free(rdef); return false; }
    bool *cdef = rdef ? rdef + nkids : NULL;
    int n = 0;
    for (int i = 0; i < nkids; i++) {
        HtmlNode *kn = layout_node_dom(ctx, ctx->tree.nodes[kids[i]].dom_node_idx);
        if (kn && kn->type == HTML_NODE_TEXT && layout_text_is_whitespace(css_layout_node_text(ctx, kn))) continue;
        GridItem *it = &G->items[n];
        LayoutBox *cb = layout_box(ctx, kids[i]);
        it->idx = kids[i];
        it->is_text = kn && kn->type == HTML_NODE_TEXT;
        it->order = it->is_text ? 0 : cb->s->order;
        it->seq = n;
        n++;
    }
    free(kids);
    if (n > 1) qsort(G->items, (size_t)n, sizeof(GridItem), grid_item_order_cmp);
    for (int i = 0; i < n; i++) {
        GridItem *it = &G->items[i];
        const LayoutStyle *s = layout_box(ctx, it->idx)->s;
        if (it->is_text) { it->rs = it->cs = 1; continue; }
        rdef[i] = grid_resolve_axis(D, true, s->grid_rs, s->grid_re, erows, &it->r, &it->rs);
        cdef[i] = grid_resolve_axis(D, false, s->grid_cs, s->grid_ce, ecols, &it->c, &it->cs);
        if (it->rs > GRID_MAX_TRACKS) it->rs = GRID_MAX_TRACKS;
        if (it->cs > GRID_MAX_TRACKS) it->cs = GRID_MAX_TRACKS;
    }
    bool column_flow = ct->s->grid_flow & 1;
    int minor = 0;
    int major = grid_place_items(G->items, n, rdef, cdef, column_flow, (ct->s->grid_flow & 2) != 0,
                                 ecols, erows, &minor);
    free(rdef);
    G->nitems = n;
    G->ncols = column_flow ? major : minor;
    G->nrows = column_flow ? minor : major;
    if (G->ncols < ecols) G->ncols = ecols;
    if (G->nrows < erows) G->nrows = erows;
    if (G->ncols > GRID_MAX_TRACKS * 4) G->ncols = GRID_MAX_TRACKS * 4;
    if (G->nrows > 100000) G->nrows = 100000;
    if (G->ncols < 1) G->ncols = 1;
    if (G->nrows < 1) G->nrows = 1;
    G->cols = (GridSize *)calloc((size_t)G->ncols, sizeof(GridSize));
    G->rows = (GridSize *)calloc((size_t)G->nrows, sizeof(GridSize));
    if (!G->cols || !G->rows) return false;
    GridTrackList *ac = (GridTrackList *)malloc(sizeof(GridTrackList));
    if (!ac) return false;
    grid_parse_tracks(ct->s->grid_auto_cols, fs, cw, 0.0, ac);
    grid_init_sizes(G->cols, G->ncols, &D->cols, ac, cw);
    grid_parse_tracks(ct->s->grid_auto_rows, fs, ch, 0.0, ac);
    grid_init_sizes(G->rows, G->nrows, &D->rows, ac, ch);
    free(ac);
    /* auto-fit: repeated tracks no item occupies collapse. */
    for (int axis = 0; axis < 2; axis++) {
        const GridTrackList *L = axis ? &D->rows : &D->cols;
        GridSize *t = axis ? G->rows : G->cols;
        for (int k = L->fit_start; k < L->fit_start + L->fit_count; k++) {
            bool used = false;
            for (int i = 0; i < n && !used; i++) {
                int s = axis ? G->items[i].r : G->items[i].c, sp = axis ? G->items[i].rs : G->items[i].cs;
                used = k >= s && k < s + sp;
            }
            if (!used) t[k].collapsed = true;
        }
    }
    for (int i = 0; i < n; i++) {
        GridItem *it = &G->items[i];
        if (it->c >= G->ncols) it->c = G->ncols - 1;
        if (it->c + it->cs > G->ncols) it->cs = G->ncols - it->c;
        if (it->r >= G->nrows) it->r = G->nrows - 1;
        if (it->r + it->rs > G->nrows) it->rs = G->nrows - it->r;
    }
    return true;
}

static void grid_column_contributions(GridState *G)
{
    for (int i = 0; i < G->nitems; i++) {
        GridItem *it = &G->items[i];
        it->min_w = layout_intrinsic_contribution(G->ctx, it->idx, false);
        it->max_w = layout_intrinsic_contribution(G->ctx, it->idx, true);
    }
}

static double grid_tracks_total(const GridSize *t, int n, double gap)
{
    double sum = 0.0;
    int live = 0;
    for (int i = 0; i < n; i++) if (!t[i].collapsed) { sum += t[i].base; live++; }
    return sum + (live > 1 ? gap * (live - 1) : 0.0);
}

/* The grid container's min- / max-content content width. */
static double layout_grid_intrinsic(LayoutContext *ctx, int idx, bool want_max)
{
    GridState G;
    double w = 0.0;
    if (grid_setup(ctx, idx, -1.0, -1.0, &G)) {
        grid_column_contributions(&G);
        grid_size_tracks(G.cols, G.ncols, -1.0, layout_box(ctx, idx)->s->gap_col, G.items, G.nitems,
                         false, want_max ? 2 : 1, false);
        w = grid_tracks_total(G.cols, G.ncols, layout_box(ctx, idx)->s->gap_col);
    }
    grid_state_free(&G);
    return w;
}

/* Track start offsets under justify-/align-content. */
static void grid_track_positions(const GridSize *t, int n, double gap, double avail, CssJustifyContent jc,
                                 double *pos)
{
    double total = grid_tracks_total(t, n, gap);
    double free_space = avail >= 0.0 ? avail - total : 0.0;
    int live = 0;
    for (int i = 0; i < n; i++) if (!t[i].collapsed) live++;
    double p = 0.0, extra = 0.0;
    switch (jc) {
    case CSS_JUSTIFY_CENTER: p = free_space / 2.0; break;
    case CSS_JUSTIFY_FLEX_END: p = free_space; break;
    case CSS_JUSTIFY_SPACE_BETWEEN: if (free_space > 0.0 && live > 1) extra = free_space / (live - 1); break;
    case CSS_JUSTIFY_SPACE_AROUND: if (free_space > 0.0 && live) { extra = free_space / live; p = extra / 2.0; } break;
    case CSS_JUSTIFY_SPACE_EVENLY: if (free_space > 0.0) { extra = free_space / (live + 1); p = extra; } break;
    default: break;
    }
    bool first = true;
    for (int i = 0; i < n; i++) {
        if (t[i].collapsed) { pos[i] = p; continue; }
        if (!first) p += gap + extra;
        pos[i] = p;
        p += t[i].base;
        first = false;
    }
    pos[n] = p;
}

static void layout_grid_container(LayoutContext *ctx, int idx)
{
    LayoutBox *ct = layout_box(ctx, idx);
    double content_x = ct->x + ct->padding_left + ct->s->border_left;
    double content_y = ct->y + ct->padding_top + ct->s->border_top;
    double cw = ct->content_width;
    bool height_def = !layout_height_auto(ct) || ct->height > 0.0;
    double chh = height_def ? ct->content_height : -1.0;
    double gap_c = ct->s->gap_col, gap_r = ct->s->gap_row;

    GridState G;
    if (!grid_setup(ctx, idx, cw, chh, &G)) { grid_state_free(&G); layout_block_flow(ctx, idx); return; }
    grid_column_contributions(&G);
    grid_size_tracks(G.cols, G.ncols, cw, gap_c, G.items, G.nitems, false, 0,
                     ct->s->justify_content == CSS_JUSTIFY_FLEX_START);
    double *cpos = (double *)malloc((size_t)(G.ncols + 1) * sizeof(double));
    double *rpos = (double *)malloc((size_t)(G.nrows + 1) * sizeof(double));
    int *todo = G.nitems > 0 ? (int *)malloc((size_t)G.nitems * sizeof(int)) : NULL;
    if (!cpos || !rpos || (G.nitems > 0 && !todo)) {
        free(cpos); free(rpos); free(todo); grid_state_free(&G);
        return;
    }
    grid_track_positions(G.cols, G.ncols, gap_c, cw, ct->s->justify_content, cpos);

    /* Items at their column widths; heights by their own layout. */
    int ntodo = 0;
    for (int i = 0; i < G.nitems; i++) {
        GridItem *it = &G.items[i];
        LayoutBox *child = layout_box(ctx, it->idx);
        double ax = cpos[it->c];
        /* (the gap after the last spanned track is not part of the area) */
        double aw = cpos[it->c + it->cs - 1] + G.cols[it->c + it->cs - 1].base - ax;
        if (aw < 0.0) aw = 0.0;
        if (it->is_text) {
            child->x = content_x + ax;
            child->y = content_y;
            child->width = it->max_w < aw ? it->max_w : aw;
            child->height = layout_line_advance_box(child);
            it->h = child->height;
            continue;
        }
        HtmlNode *dom = layout_node_dom(ctx, ctx->tree.nodes[it->idx].dom_node_idx);
        layout_resolve_used_sizes(child, dom, aw, chh > 0.0 && it->rs == 1 && G.rows[it->r].kmin == GT_LEN &&
                                  G.rows[it->r].kmax == GT_LEN ? G.rows[it->r].limit : 0.0);
        bool w_def = child->s->width_set || child->s->width_percent > 0.0 || child->s->width_keyword ||
                     layout_is_img(dom);
        unsigned char ma = child->s->margin_auto;
        double ml = (ma & 8) ? 0.0 : child->margin_left, mr = (ma & 2) ? 0.0 : child->margin_right;
        double room = aw - ml - mr;
        if (room < 0.0) room = 0.0;
        int js = child->s->justify_self ? child->s->justify_self - 1
               : (ct->s->justify_items ? ct->s->justify_items - 1 : CSS_ALIGN_STRETCH);
        if (!w_def) {
            if (js == CSS_ALIGN_STRETCH && !(ma & 0xA)) {
                double w = room;
                if (child->max_width >= 0.0) {
                    double mx = child->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX ? child->max_width
                              : child->max_width + layout_horizontal_border_padding(child);
                    if (w > mx) w = mx;
                }
                child->width = w;
            } else {
                child->width = layout_shrink_to_fit(ctx, it->idx, room);
            }
        }
        double free_w = room - child->width;
        double off = 0.0;
        if ((ma & 0xA) == 0xA) off = free_w > 0.0 ? free_w / 2.0 : 0.0;
        else if (ma & 8) off = free_w > 0.0 ? free_w : 0.0;
        else if (!(ma & 2)) {
            if (js == CSS_ALIGN_CENTER) off = free_w / 2.0;
            else if (js == CSS_ALIGN_FLEX_END) off = free_w;
        }
        child->x = content_x + ax + ml + off;
        child->y = content_y + child->margin_top;
        layout_update_content_sizes(child);
        todo[ntodo++] = it->idx;
    }
    if (ntodo > 0) layout_units(ctx, todo, ntodo);
    for (int i = 0; i < G.nitems; i++) {
        GridItem *it = &G.items[i];
        if (it->is_text) continue;
        LayoutBox *child = layout_box(ctx, it->idx);
        double mt = (child->s->margin_auto & 1) ? 0.0 : child->margin_top;
        double mb = (child->s->margin_auto & 4) ? 0.0 : child->margin_bottom;
        it->h = child->height + mt + mb;
    }

    grid_size_tracks(G.rows, G.nrows, chh, gap_r, G.items, G.nitems, true, 0, true);
    grid_track_positions(G.rows, G.nrows, gap_r, chh, CSS_JUSTIFY_FLEX_START, rpos);

    /* Block-axis alignment; stretched items whose contents depend on their
     * height are laid out again. */
    int nre = 0;
    for (int i = 0; i < G.nitems; i++) {
        GridItem *it = &G.items[i];
        LayoutBox *child = layout_box(ctx, it->idx);
        double ay = rpos[it->r];
        double ah = rpos[it->r + it->rs - 1] + G.rows[it->r + it->rs - 1].base - ay;
        if (ah < 0.0) ah = 0.0;
        if (it->is_text) { child->y = content_y + ay; continue; }
        unsigned char ma = child->s->margin_auto;
        double mt = (ma & 1) ? 0.0 : child->margin_top, mb = (ma & 4) ? 0.0 : child->margin_bottom;
        double room = ah - mt - mb;
        int as = child->s->align_self ? child->s->align_self - 1 : (int)ct->s->align_items;
        HtmlNode *dom = layout_node_dom(ctx, ctx->tree.nodes[it->idx].dom_node_idx);
        bool h_auto = layout_height_auto(child) && !layout_is_img(dom);
        double off = 0.0;
        if (as == CSS_ALIGN_STRETCH && h_auto && !(ma & 5)) {
            double h = room;
            if (child->max_height >= 0.0 && h > child->max_height) h = child->max_height;
            if (h > child->height + 0.01) {
                child->height = h;
                layout_update_content_sizes(child);
                if (layout_height_sensitive(ctx, it->idx)) todo[nre++] = it->idx;
            }
        } else {
            double free_h = room - child->height;
            if ((ma & 5) == 5) off = free_h > 0.0 ? free_h / 2.0 : 0.0;
            else if (ma & 1) off = free_h > 0.0 ? free_h : 0.0;
            else if (!(ma & 4)) {
                if (as == CSS_ALIGN_CENTER) off = free_h / 2.0;
                else if (as == CSS_ALIGN_FLEX_END) off = free_h;
            }
        }
        layout_move_box(ctx, it->idx, child->x, content_y + ay + mt + off);
    }
    if (nre > 0) {
        double *keep = (double *)malloc((size_t)nre * sizeof(double));
        for (int k = 0; k < nre && keep; k++) keep[k] = layout_box(ctx, todo[k])->height;
        layout_units(ctx, todo, nre);
        for (int k = 0; k < nre && keep; k++) {
            LayoutBox *child = layout_box(ctx, todo[k]);
            child->height = keep[k];
            layout_update_content_sizes(child);
        }
        free(keep);
    }

    if (!height_def) {
        double h = grid_tracks_total(G.rows, G.nrows, gap_r);
        double mn = ct->min_height, mx = ct->max_height;
        if (ct->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
            double v = layout_vertical_border_padding(ct);
            mn = mn > 0.0 ? (mn - v > 0.0 ? mn - v : 0.0) : 0.0;
            mx = mx > 0.0 ? (mx - v > 0.0 ? mx - v : 0.0) : 0.0;
        }
        h = flex_clamp(h, mn, mx);
        ct->height = h + layout_vertical_border_padding(ct);
        layout_update_content_sizes(ct);
    }
    free(cpos);
    free(rpos);
    free(todo);
    grid_state_free(&G);
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

    double inner_left = row->x + row->padding_left + row->s->border_left;
    double inner_top  = row->y + row->padding_top + row->s->border_top;
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
                  + cell->s->border_left + cell->s->border_right;
        double w = 0.0;
        if (cell->s->width_percent > 0.0) {
            w = inner_w * cell->s->width_percent + cell->s->width_pct_px;
        } else if (cell->s->width_set && cell->s->css_width > 0.0) {
            w = cell->s->css_width;
            if (cell->s->box_sizing == CSS_BOX_SIZING_CONTENT_BOX) w += hp;
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
            if (cell->s->vertical_align == 1) dy = free_h * 0.5;
            else if (cell->s->vertical_align == 3) dy = free_h;
            if (dy != 0.0) layout_shift_subtree_y(ctx, kids[i], dy);
        }
        if (cell->height < row_h) {
            cell->height = row_h;
            layout_update_content_sizes(cell);
        }
    }
    row->height = row_h + row->padding_top + row->padding_bottom
                + row->s->border_top + row->s->border_bottom;
    layout_update_content_sizes(row);
    free(widths);
    free(kids);
}

/* ---- Table layout (CSS 2.1 §17, automatic table layout) ----
 * The table's rows (directly, or in row groups; cells directly in the table
 * or a group form an anonymous row) become a grid of cells with colspan /
 * rowspan.  Column min/max widths come from the cells' min-/max-content
 * widths (a fixed width is a column's preferred width, a percentage a share
 * of the table); the table takes its specified width or shrinks to fit its
 * container; columns get their minimums and share the rest by their maximums.
 * Cells are laid out at their column widths, rows take the tallest cell,
 * cells stretch to their rows with vertical-align moving their content. */

typedef struct {
    int idx;
    int row, col, rs, cs;              /* cs < 0: every column (anonymous cell) */
    double minw, maxw, pct;
    bool fixed;
} TableCell;

typedef struct {
    int idx;                           /* -1: anonymous */
    int group;                         /* index into groups, -1: none */
    double y, h;
} TableRow;

typedef struct {
    TableCell *cells;
    int ncells, cap_cells;
    TableRow *rows;
    int nrows, cap_rows;
    int groups[64];
    int ngroups;
    int captions[8];
    int ncaptions;
    double colspec[64];                /* the columns' (and column groups') widths; -1: auto */
    double colpct[64];                 /* their percentage widths (fractions; 0: none) */
    int colbox[64], colgroup[64];      /* their column and column group boxes (-1: none) */
    int ncolspec;
    int ncols;
    double *cmin, *cmax, *cpct;
    unsigned char *cfixed;
    double hs, vs;                     /* border-spacing */
} TableGrid;

static void table_free(TableGrid *T)
{
    free(T->cells);
    free(T->rows);
    free(T->cmin);
    free(T->cmax);
    free(T->cpct);
    free(T->cfixed);
}

static bool table_is_table(const LayoutBox *b)
{
    return b->s->table_role == 1 && b->s->display != CSS_DISPLAY_FLEX && b->s->display != CSS_DISPLAY_GRID &&
           b->s->display != CSS_DISPLAY_NONE && b->s->display != CSS_DISPLAY_INLINE;
}

static bool table_skip_child(LayoutContext *ctx, int c)
{
    HtmlNode *kn = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
    if (!kn) return true;
    if (kn->type == HTML_NODE_TEXT) return layout_text_is_whitespace(css_layout_node_text(ctx, kn));
    return kn->type != HTML_NODE_ELEMENT;
}

static int table_add_row(TableGrid *T, int idx, int group)
{
    if (T->nrows == T->cap_rows) {
        int nc = T->cap_rows ? T->cap_rows * 2 : 16;
        TableRow *nr = (TableRow *)realloc(T->rows, (size_t)nc * sizeof(TableRow));
        if (!nr) return -1;
        T->rows = nr;
        T->cap_rows = nc;
    }
    TableRow *r = &T->rows[T->nrows];
    memset(r, 0, sizeof(*r));
    r->idx = idx;
    r->group = group;
    return T->nrows++;
}

static void table_add_cell(TableGrid *T, int idx, int row, int cs, int rs)
{
    if (T->ncells == T->cap_cells) {
        int nc = T->cap_cells ? T->cap_cells * 2 : 32;
        TableCell *ncl = (TableCell *)realloc(T->cells, (size_t)nc * sizeof(TableCell));
        if (!ncl) return;
        T->cells = ncl;
        T->cap_cells = nc;
    }
    TableCell *c = &T->cells[T->ncells++];
    memset(c, 0, sizeof(*c));
    c->idx = idx;
    c->row = row;
    c->cs = cs;
    c->rs = rs;
}

static int table_span_attr(LayoutContext *ctx, int c, const char *name)
{
    HtmlNode *dn = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
    const char *v = dn ? layout_node_attribute(dn, name) : NULL;
    if (!v) return 1;
    int n = atoi(v);
    if (strcmp(name, "rowspan") == 0) return n < 0 ? 1 : (n > 65534 ? 65534 : n);   /* 0: to the end */
    return n < 1 ? 1 : (n > 1000 ? 1000 : n);
}

/* The width a column or column group box gives its columns, -1 for auto. */
static double table_column_width(LayoutContext *ctx, int c)
{
    LayoutBox *b = layout_box(ctx, c);
    bool set = b->s->width_set && b->s->width_percent <= 0.0 && b->s->css_width >= 0.0;
    /* (min-width alone: the column's least width) */
    if (!set) return b->s->min_width > 0.0 ? b->s->min_width : -1.0;
    /* (min- and max-width apply to columns: max first, then min wins) */
    double w = b->s->css_width;
    if (b->s->max_width >= 0.0 && w > b->s->max_width) w = b->s->max_width;
    if (b->s->min_width > w) w = b->s->min_width;
    return w;
}

/* Column box c (a table-column or table-column-group): the columns it makes,
 * each with its width, noted in T->colspec.  A group with columns in it is
 * theirs; one without spans its span attribute's count. */
static void table_note_columns(LayoutContext *ctx, TableGrid *T, int c)
{
    bool inner = false;
    for (int k = ctx->tree.nodes[c].first_child_idx; k >= 0; k = ctx->tree.nodes[k].next_sibling_idx) {
        LayoutBox *kb = layout_box(ctx, k);
        if (kb->s->display == CSS_DISPLAY_NONE || kb->s->table_role != 6) continue;
        inner = true;
        double w = table_column_width(ctx, k);
        double pct = layout_box(ctx, k)->s->width_percent;
        for (int n = table_span_attr(ctx, k, "span"); n > 0 && T->ncolspec < 64; n--) {
            T->colbox[T->ncolspec] = k;
            T->colgroup[T->ncolspec] = c;
            T->colpct[T->ncolspec] = pct;
            T->colspec[T->ncolspec++] = w;
        }
    }
    if (inner) return;
    double w = table_column_width(ctx, c);
    double pct = layout_box(ctx, c)->s->width_percent;
    for (int n = table_span_attr(ctx, c, "span"); n > 0 && T->ncolspec < 64; n--) {
        T->colbox[T->ncolspec] = c;
        T->colgroup[T->ncolspec] = -1;
        T->colpct[T->ncolspec] = pct;
        T->colspec[T->ncolspec++] = w;
    }
}

/* The cells of row r: the row box's children. */
static void table_row_cells(LayoutContext *ctx, TableGrid *T, int row_box, int r)
{
    for (int c = ctx->tree.nodes[row_box].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        if (!layout_is_in_flow(layout_box(ctx, c)) || table_skip_child(ctx, c)) continue;
        table_add_cell(T, c, r, table_span_attr(ctx, c, "colspan"), table_span_attr(ctx, c, "rowspan"));
    }
}

/* Rows and cells of a table or row group's children. */
static bool table_collect_children(LayoutContext *ctx, TableGrid *T, int parent, int group, bool *real)
{
    /* A table's first header group goes first and its first footer group
     * last (CSS 2.1 section 17.2), whatever their place among its children. */
    int head = -1, foot = -1;
    for (int c = group < 0 ? ctx->tree.nodes[parent].first_child_idx : -1; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        const LayoutBox *cb = layout_box(ctx, c);
        if (cb->s->display == CSS_DISPLAY_NONE || cb->s->table_role != 2 || !layout_is_in_flow(cb)) continue;
        if (cb->s->table_group == 1 && head < 0) head = c;
        else if (cb->s->table_group == 2 && foot < 0) foot = c;
    }
    for (int pass = head >= 0 || foot >= 0 ? 0 : 1; pass < (foot >= 0 ? 3 : 2); pass++) {
    int anon = -1;   /* the anonymous row open for loose cells */
    for (int c = ctx->tree.nodes[parent].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
        if (pass == 0 ? c != head : pass == 2 ? c != foot : (c == head || c == foot)) continue;
        LayoutBox *cb = layout_box(ctx, c);
        if (!layout_is_in_flow(cb) || table_skip_child(ctx, c)) continue;
        unsigned char role = cb->s->display == CSS_DISPLAY_NONE ? 0 : cb->s->table_role;
        if (role == 5 && group < 0) {
            if (T->ncaptions < 8) T->captions[T->ncaptions++] = c;
            anon = -1;
        } else if (role == 6) {
            if (group < 0) table_note_columns(ctx, T, c);
            continue;
        } else if (role == 2 && group < 0) {
            int g = T->ngroups < 64 ? T->ngroups++ : -1;
            if (g >= 0) T->groups[g] = c;
            table_collect_children(ctx, T, c, g, real);
            anon = -1;
        } else if (role == 3) {
            int r = table_add_row(T, c, group);
            if (r >= 0) table_row_cells(ctx, T, c, r);
            *real = true;
            anon = -1;
        } else if (role == 4) {
            if (anon < 0) anon = table_add_row(T, -1, group);
            if (anon >= 0) table_add_cell(T, c, anon, table_span_attr(ctx, c, "colspan"),
                                          table_span_attr(ctx, c, "rowspan"));
            *real = true;
        } else {
            /* Other content: a row of one cell across the table. */
            int r = table_add_row(T, -1, group);
            if (r >= 0) table_add_cell(T, c, r, -1, 1);
            anon = -1;
        }
    }
    }
    return true;
}

static bool table_place(LayoutContext *ctx, int idx, const LayoutBox *tb, TableGrid *T);

/* ---- The collapsing border model (CSS 2.1 section 17.6.2) ----
 * After the style pass, for each table with border-collapse: collapse,
 * every edge of its grid takes the border that wins among the cells,
 * rows, row groups, columns, column groups and the table along it
 * (17.6.2.1): hidden over all, then the wider, then by style, then cell
 * over row over row group over column over column group over table, then
 * the one left of or above the other.  A cell's border on a side is then
 * half its edge's (in that edge's style and color), the table's half its
 * outer edges', and rows, row groups and columns have none of their own;
 * the table has no padding.  Written from the cascade's border
 * (border_spec_*) each time. */
typedef struct { float w; unsigned char style, prio; uint32_t rgba; } CollEdge;

static int coll_rank(int st)
{
    switch (st) {
    case CSS_BORDER_DOUBLE: return 8;
    case CSS_BORDER_SOLID: return 7;
    case CSS_BORDER_DASHED: return 6;
    case CSS_BORDER_DOTTED: return 5;
    case CSS_BORDER_RIDGE: return 4;
    case CSS_BORDER_OUTSET: return 3;
    case CSS_BORDER_GROOVE: return 2;
    case CSS_BORDER_INSET: return 1;
    default: return 0;
    }
}

/* Box b's side k (0 left, 1 right, 2 top, 3 bottom) offered to edge e at
 * priority prio. */
static void coll_offer(CollEdge *e, const LayoutBox *b, int k, int prio)
{
    if (!b || e->style == CSS_BORDER_HIDDEN) return;
    int st = b->s->border_spec_style[k];
    float w = b->s->border_spec_w[k];
    if (st == CSS_BORDER_HIDDEN) {
        e->style = CSS_BORDER_HIDDEN;
        e->w = 0.0f;
        return;
    }
    if (st <= CSS_BORDER_HIDDEN || w <= 0.0f) return;
    bool wins = e->style == CSS_BORDER_NONE || w > e->w ||
                (w == e->w && (coll_rank(st) > coll_rank(e->style) || (st == e->style && prio > e->prio)));
    if (!wins) return;
    e->w = w;
    e->style = (unsigned char)st;
    e->prio = (unsigned char)prio;
    if (b->s->border_spec_cset & (1 << k)) {
        e->rgba = b->s->border_spec_rgba[k];
    } else {
        double c[4] = { b->s->color_r, b->s->color_g, b->s->color_b, b->s->color_a };
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) v = (v << 8) | (uint32_t)lround((c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i]) * 255.0);
        e->rgba = v;
    }
}

/* Box b's side k as its share of edge e: the box before the edge (left of
 * or above it) floor(w / 2), the one after it the rest, so the two add up
 * to the whole in whole pixels. */
static void coll_set_side(LayoutBox *b, int k, const CollEdge *e, bool before)
{
    double *bw[4] = { &b->s->border_left, &b->s->border_right, &b->s->border_top, &b->s->border_bottom };
    unsigned char bit = (unsigned char)(1 << k);
    b->s->border_coll = 1;
    b->s->border_out[k] = 0.0f;
    bool shown = e && e->style >= CSS_BORDER_SOLID && e->w > 0.0f;
    float half = shown ? floorf(e->w * 0.5f) : 0.0f;
    *bw[k] = shown ? (before ? half : e->w - half) : 0.0;
    b->s->border_style[k] = shown ? e->style : CSS_BORDER_NONE;
    b->s->border_rgba[k] = shown ? e->rgba : 0;
    if (shown) { b->s->border_styled |= bit; b->s->border_color_set |= bit; }
    else { b->s->border_styled &= (unsigned char)~bit; b->s->border_color_set &= (unsigned char)~bit; }
}

/* What of a box's style the collapse pass writes, as one number. */
static uint64_t coll_sig(const LayoutBox *b)
{
    const LayoutStyle *st = b->s;
    uint64_t h = 1469598103934665603ull;
    const void *parts[] = { &st->border_top, &st->border_right, &st->border_bottom, &st->border_left,
                            &st->padding_top, &st->padding_right, &st->padding_bottom, &st->padding_left };
    for (int i = 0; i < 8; i++) {
        uint64_t v;
        memcpy(&v, parts[i], sizeof(v));
        h = (h ^ v) * 1099511628211ull;
    }
    for (int k = 0; k < 4; k++) {
        uint32_t o;
        memcpy(&o, &st->border_out[k], sizeof(o));
        h = (h ^ ((uint64_t)st->border_rgba[k] << 8 | st->border_style[k]) ^ ((uint64_t)o << 40)) * 1099511628211ull;
    }
    return (h ^ st->border_color_set) * 1099511628211ull;
}

/* The boxes of a table the pass writes, with their styles' signatures
 * before; those it changed lose their last layout (style_left). */
typedef struct { int *idx; uint64_t *sig; int n, cap; } CollTouched;

static void coll_note(LayoutContext *ctx, CollTouched *t, int i)
{
    if (i < 0) return;
    if (t->n == t->cap) {
        int nc = t->cap ? t->cap * 2 : 32;
        int *ni = (int *)realloc(t->idx, (size_t)nc * sizeof(int));
        if (!ni) return;
        t->idx = ni;
        uint64_t *ns = (uint64_t *)realloc(t->sig, (size_t)nc * sizeof(uint64_t));
        if (!ns) return;
        t->sig = ns;
        t->cap = nc;
    }
    t->idx[t->n] = i;
    t->sig[t->n++] = coll_sig(layout_box(ctx, i));
}

static void layout_collapse_table(LayoutContext *ctx, int idx)
{
    LayoutBox *tb = &ctx->boxes[idx];
    TableGrid T;
    if (!table_place(ctx, idx, tb, &T) || T.ncols <= 0 || T.nrows <= 0) { table_free(&T); return; }
    CollTouched touched = { NULL, NULL, 0, 0 };
    coll_note(ctx, &touched, idx);
    for (int i = 0; i < T.ncells; i++) coll_note(ctx, &touched, T.cells[i].idx);
    for (int r = 0; r < T.nrows; r++) coll_note(ctx, &touched, T.rows[r].idx);
    for (int g = 0; g < T.ngroups; g++) coll_note(ctx, &touched, T.groups[g]);
    for (int k = 0; k < T.ncolspec; k++) { coll_note(ctx, &touched, T.colbox[k]); coll_note(ctx, &touched, T.colgroup[k]); }
    int R = T.nrows, C = T.ncols;
    /* H[r][c]: the edge above row r in column c (r = R: the bottom);
     * V[r][c]: the edge left of column c in row r (c = C: the right). */
    CollEdge *H = (CollEdge *)calloc((size_t)(R + 1) * (size_t)C, sizeof(CollEdge));
    CollEdge *V = (CollEdge *)calloc((size_t)R * (size_t)(C + 1), sizeof(CollEdge));
    if (!H || !V) { free(H); free(V); free(touched.idx); free(touched.sig); table_free(&T); return; }
#define HE(r, c) (&H[(size_t)(r) * (size_t)C + (size_t)(c)])
#define VE(r, c) (&V[(size_t)(r) * (size_t)(C + 1) + (size_t)(c)])
    for (int i = 0; i < T.ncells; i++) {
        const TableCell *cl = &T.cells[i];
        const LayoutBox *cb = layout_box(ctx, cl->idx);
        int r1 = cl->row + cl->rs, c1 = cl->col + cl->cs;
        for (int c = cl->col; c < c1 && c < C; c++) { coll_offer(HE(cl->row, c), cb, 2, 5); coll_offer(HE(r1, c), cb, 3, 5); }
        for (int r = cl->row; r < r1 && r < R; r++) { coll_offer(VE(r, cl->col), cb, 0, 5); coll_offer(VE(r, c1), cb, 1, 5); }
    }
    for (int r = 0; r < R; r++) {
        if (T.rows[r].idx < 0) continue;
        const LayoutBox *rb = layout_box(ctx, T.rows[r].idx);
        for (int c = 0; c < C; c++) { coll_offer(HE(r, c), rb, 2, 4); coll_offer(HE(r + 1, c), rb, 3, 4); }
        coll_offer(VE(r, 0), rb, 0, 4);
        coll_offer(VE(r, C), rb, 1, 4);
    }
    for (int g = 0; g < T.ngroups; g++) {
        int first = -1, last = -1;
        for (int r = 0; r < R; r++) if (T.rows[r].group == g) { if (first < 0) first = r; last = r; }
        if (first < 0) continue;
        const LayoutBox *gb = layout_box(ctx, T.groups[g]);
        for (int c = 0; c < C; c++) { coll_offer(HE(first, c), gb, 2, 3); coll_offer(HE(last + 1, c), gb, 3, 3); }
        for (int r = first; r <= last; r++) { coll_offer(VE(r, 0), gb, 0, 3); coll_offer(VE(r, C), gb, 1, 3); }
    }
    for (int pass = 0; pass < 2; pass++) {
        /* (columns, then column groups: a group's edges are its first and
         * last columns') */
        for (int k = 0; k < T.ncolspec && k < C; k++) {
            int b = pass == 0 ? T.colbox[k] : T.colgroup[k];
            if (b < 0 || (pass == 1 && k > 0 && T.colgroup[k - 1] == b)) continue;
            int k1 = k;
            if (pass == 1) while (k1 + 1 < T.ncolspec && k1 + 1 < C && T.colgroup[k1 + 1] == b) k1++;
            const LayoutBox *xb = layout_box(ctx, b);
            int prio = pass == 0 ? 2 : 1;
            for (int r = 0; r < R; r++) { coll_offer(VE(r, k), xb, 0, prio); coll_offer(VE(r, k1 + 1), xb, 1, prio); }
            for (int c = k; c <= k1; c++) { coll_offer(HE(0, c), xb, 2, prio); coll_offer(HE(R, c), xb, 3, prio); }
        }
    }
    for (int c = 0; c < C; c++) { coll_offer(HE(0, c), tb, 2, 0); coll_offer(HE(R, c), tb, 3, 0); }
    for (int r = 0; r < R; r++) { coll_offer(VE(r, 0), tb, 0, 0); coll_offer(VE(r, C), tb, 1, 0); }

    /* The cells: half of each of their edges. */
    for (int i = 0; i < T.ncells; i++) {
        const TableCell *cl = &T.cells[i];
        LayoutBox *cb = layout_box(ctx, cl->idx);
        int c1 = cl->col + cl->cs < C ? cl->col + cl->cs : C, r1 = cl->row + cl->rs < R ? cl->row + cl->rs : R;
        coll_set_side(cb, 0, VE(cl->row, cl->col), false);
        coll_set_side(cb, 1, VE(cl->row, c1), true);
        coll_set_side(cb, 2, HE(cl->row, cl->col), false);
        coll_set_side(cb, 3, HE(r1, cl->col), true);
    }
    /* Cells along the outside paint their edges whole: the table's share
     * too, past their boxes. */
    for (int i = 0; i < T.ncells; i++) {
        const TableCell *cl = &T.cells[i];
        LayoutBox *cb = layout_box(ctx, cl->idx);
        int c1 = cl->col + cl->cs, r1 = cl->row + cl->rs;
        const CollEdge *e;
        if (cl->col == 0 && (e = VE(cl->row, 0))->style >= CSS_BORDER_SOLID) cb->s->border_out[0] = floorf(e->w * 0.5f);
        if (c1 >= C && (e = VE(cl->row, C))->style >= CSS_BORDER_SOLID) cb->s->border_out[1] = e->w - floorf(e->w * 0.5f);
        if (cl->row == 0 && (e = HE(0, cl->col))->style >= CSS_BORDER_SOLID) cb->s->border_out[2] = floorf(e->w * 0.5f);
        if (r1 >= R && (e = HE(R, cl->col))->style >= CSS_BORDER_SOLID) cb->s->border_out[3] = e->w - floorf(e->w * 0.5f);
    }
    /* Rows, groups and columns: none of their own. */
    for (int r = 0; r < R; r++)
        if (T.rows[r].idx >= 0) for (int k = 0; k < 4; k++) coll_set_side(layout_box(ctx, T.rows[r].idx), k, NULL, false);
    for (int g = 0; g < T.ngroups; g++) for (int k = 0; k < 4; k++) coll_set_side(layout_box(ctx, T.groups[g]), k, NULL, false);
    for (int k = 0; k < T.ncolspec; k++) {
        if (T.colbox[k] >= 0) for (int j = 0; j < 4; j++) coll_set_side(layout_box(ctx, T.colbox[k]), j, NULL, false);
        if (T.colgroup[k] >= 0) for (int j = 0; j < 4; j++) coll_set_side(layout_box(ctx, T.colgroup[k]), j, NULL, false);
    }
    /* The table: half its outer edges (the widest along the top and
     * bottom, the first row's at the sides), no padding. */
    const CollEdge *top = HE(0, 0), *bot = HE(R, 0);
    for (int c = 1; c < C; c++) {
        if (HE(0, c)->w > top->w) top = HE(0, c);
        if (HE(R, c)->w > bot->w) bot = HE(R, c);
    }
    coll_set_side(tb, 0, VE(0, 0), true);
    coll_set_side(tb, 1, VE(0, C), false);
    coll_set_side(tb, 2, top, true);
    coll_set_side(tb, 3, bot, false);
    /* (its border: room in the layout, painted by the cells) */
    for (int k = 0; k < 4; k++) tb->s->border_style[k] = CSS_BORDER_NONE;
    tb->s->padding_top = tb->s->padding_right = tb->s->padding_bottom = tb->s->padding_left = 0.0;
    tb->s->em_deferred &= 0x0F;
    tb->s->pct_sides &= 0x0F;
    for (int sd = 0; sd < 4; sd++) tb->s->padding_pct[sd] = tb->s->padding_base[sd] = 0.0;
#undef HE
#undef VE
    for (int i = 0; ctx->style_left && i < touched.n; i++)
        if (coll_sig(layout_box(ctx, touched.idx[i])) != touched.sig[i]) ctx->style_left[touched.idx[i]] = 0;
    free(touched.idx);
    free(touched.sig);
    free(H);
    free(V);
    table_free(&T);
}

static void layout_collapse_borders(LayoutContext *ctx)
{
    int n = 0;
    for (int i = 0; i < ctx->tree.count; i++) {
        const LayoutBox *b = &ctx->boxes[i];
        /* (the style's skipped, not the box's flag: that is the last
         * layout's until this one starts the box) */
        if (!b->s->border_collapse || b->s->table_role != 1 || b->s->skipped || !table_is_table(b))
            continue;
        layout_collapse_table(ctx, i);
        n++;
    }
    if (g_css_log > 0 && n) fprintf(stderr, "[css] collapsing-border tables: %d\n", n);
}

static bool table_grow_cols(TableGrid *T, int n)
{
    if (n <= T->ncols) return true;
    double *a = (double *)realloc(T->cmin, (size_t)n * sizeof(double));
    if (!a) return false;
    T->cmin = a;
    a = (double *)realloc(T->cmax, (size_t)n * sizeof(double));
    if (!a) return false;
    T->cmax = a;
    a = (double *)realloc(T->cpct, (size_t)n * sizeof(double));
    if (!a) return false;
    T->cpct = a;
    unsigned char *f = (unsigned char *)realloc(T->cfixed, (size_t)n);
    if (!f) return false;
    T->cfixed = f;
    for (int i = T->ncols; i < n; i++) { T->cmin[i] = T->cmax[i] = T->cpct[i] = 0.0; T->cfixed[i] = 0; }
    T->ncols = n;
    return true;
}

/* Collect the table, place its cells (colspan / rowspan) and compute the
 * column min / max widths.  False when it has no rows or cells. */
static bool table_place(LayoutContext *ctx, int idx, const LayoutBox *tb, TableGrid *T)
{
    memset(T, 0, sizeof(*T));
    bool real = false;
    table_collect_children(ctx, T, idx, -1, &real);
    if (!real) return false;
    if (tb->s->border_collapse) T->hs = T->vs = 0.0;
    else if (tb->s->border_spacing_set) { T->hs = tb->s->border_spacing_h; T->vs = tb->s->border_spacing_v; }
    else {
        /* (the UA style sheet's 2px is for <table>; the initial value is 0) */
        HtmlNode *node = tb == layout_box(ctx, idx) ? layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx) : NULL;
        bool html_table = node && node->type == HTML_NODE_ELEMENT && node->tag_name &&
                          strcasecmp(node->tag_name, "table") == 0;
        T->hs = T->vs = html_table ? 2.0 : 0.0;
    }

    /* Placement: a column cursor per row skipping slots taken by rowspans. */
    int *busy = NULL;   /* rows still covered per column */
    int nbusy = 0;
    int ci = 0;
    for (int r = 0; r < T->nrows; r++) {
        for (int k = 0; k < nbusy; k++) if (busy[k] > 0) busy[k]--;
        int col = 0;
        for (; ci < T->ncells && T->cells[ci].row == r; ci++) {
            TableCell *c = &T->cells[ci];
            while (col < nbusy && busy[col] > 0) col++;
            c->col = col;
            int cs = c->cs < 0 ? 1 : c->cs;
            int rs = c->rs == 0 ? T->nrows - r : c->rs;
            if (rs > T->nrows - r) rs = T->nrows - r;
            if (rs < 1) rs = 1;
            c->rs = rs;
            if (col + cs > nbusy) {
                int *nb = (int *)realloc(busy, (size_t)(col + cs) * sizeof(int));
                if (!nb) break;
                for (int k = nbusy; k < col + cs; k++) nb[k] = 0;
                busy = nb;
                nbusy = col + cs;
            }
            for (int k = col; k < col + cs; k++) busy[k] = rs;
            col += cs;
        }
    }
    free(busy);
    /* (columns make the grid wide however few cells there are) */
    int ngrid = nbusy > T->ncolspec ? nbusy : T->ncolspec;
    if (!table_grow_cols(T, ngrid > 0 ? ngrid : 1)) return false;
    for (int i = 0; i < T->ncells; i++) {
        TableCell *c = &T->cells[i];
        if (c->cs < 0) c->cs = T->ncols - c->col;
        if (c->col + c->cs > T->ncols) c->cs = T->ncols - c->col;
        if (c->cs < 1) c->cs = 1;
    }

    return true;
}

/* Collect the table, place its cells (colspan / rowspan) and compute the
 * column min / max widths.  False when it has no rows or cells. */
static bool table_build(LayoutContext *ctx, int idx, const LayoutBox *tb, TableGrid *T)
{
    if (!table_place(ctx, idx, tb, T)) return false;
    /* A column's width is a fixed width for it (§17.5.2). */
    for (int k = 0; k < T->ncolspec && k < T->ncols; k++)
        if (T->colspec[k] >= 0.0) { T->cfixed[k] = 1; T->cmin[k] = T->cmax[k] = T->colspec[k]; }
    /* Column widths from single-column cells, then the spanning ones. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < T->ncells; i++) {
            TableCell *c = &T->cells[i];
            if ((pass == 0) != (c->cs == 1)) continue;
            LayoutBox *cb = layout_box(ctx, c->idx);
            c->minw = layout_intrinsic_width(ctx, c->idx, false);
            c->maxw = layout_intrinsic_width(ctx, c->idx, true);
            if (cb->s->width_percent > 0.0) {
                c->pct = cb->s->width_percent;
            } else if (cb->s->width_set) {
                c->fixed = true;
                if (c->maxw < c->minw) c->maxw = c->minw;
            }
            if (c->maxw < c->minw) c->maxw = c->minw;
            if (c->cs == 1) {
                int k = c->col;
                if (c->minw > T->cmin[k]) T->cmin[k] = c->minw;
                if (c->fixed && !T->cfixed[k]) { T->cfixed[k] = 1; T->cmax[k] = c->maxw; }
                else if (c->fixed || !T->cfixed[k]) { if (c->maxw > T->cmax[k]) T->cmax[k] = c->maxw; }
                if (c->pct > T->cpct[k]) T->cpct[k] = c->pct;
                if (T->cmax[k] < T->cmin[k]) T->cmax[k] = T->cmin[k];
                continue;
            }
            double smin = T->hs * (c->cs - 1), smax = smin, wsum = 0.0;
            for (int k = c->col; k < c->col + c->cs; k++) { smin += T->cmin[k]; smax += T->cmax[k]; wsum += T->cmax[k]; }
            double dmin = c->minw - smin, dmax = c->maxw - smax;
            for (int k = c->col; k < c->col + c->cs; k++) {
                double share = wsum > 0.0 ? T->cmax[k] / wsum : 1.0 / c->cs;
                if (dmin > 0.0) T->cmin[k] += dmin * share;
                if (dmax > 0.0) T->cmax[k] += dmax * share;
                if (T->cmax[k] < T->cmin[k]) T->cmax[k] = T->cmin[k];
            }
        }
    }
    return true;
}

static double table_spacing(const TableGrid *T)
{
    return T->hs * (T->ncols + 1);
}

/* table-layout: fixed (CSS 2.1 section 17.5.2.1) in a table W wide: each
 * column as wide as its column element says, else as the first row's cell
 * in it says (a spanning cell's width split evenly), the others sharing
 * what is left equally; the cells' content plays no part. */
static void table_fixed_columns(LayoutContext *ctx, TableGrid *T, double W)
{
    int n = T->ncols;
    double *w = (double *)malloc((size_t)(n > 0 ? n : 1) * sizeof(double));
    if (!w) return;
    /* (a percentage: of the width less the spacing) */
    double inner = W - table_spacing(T);
    for (int k = 0; k < n; k++)
        w[k] = k >= T->ncolspec ? -1.0 : T->colpct[k] > 0.0 ? T->colpct[k] * (inner > 0.0 ? inner : 0.0)
             : T->colspec[k] >= 0.0 ? T->colspec[k] : -1.0;
    for (int i = 0; i < T->ncells && T->cells[i].row == 0; i++) {
        const TableCell *c = &T->cells[i];
        const LayoutBox *cb = layout_box(ctx, c->idx);
        double cw;
        if (cb->s->width_percent > 0.0) cw = cb->s->width_percent * W;
        else if (cb->s->width_set && cb->s->css_width >= 0.0) cw = cb->s->css_width;
        else continue;
        if (cb->s->box_sizing != CSS_BOX_SIZING_BORDER_BOX)
            cw += cb->s->padding_left + cb->s->padding_right + cb->s->border_left + cb->s->border_right;
        int cs = c->cs > 0 ? c->cs : 1;
        double per = (cw - T->hs * (cs - 1)) / cs;
        for (int k = c->col; k < c->col + cs && k < n; k++)
            if (w[k] < 0.0) w[k] = per > 0.0 ? per : 0.0;
    }
    double used = table_spacing(T);
    int rest = 0;
    for (int k = 0; k < n; k++) {
        if (w[k] >= 0.0) used += w[k];
        else rest++;
    }
    double share = rest && W > used ? (W - used) / rest : 0.0;
    for (int k = 0; k < n; k++) {
        T->cmin[k] = T->cmax[k] = w[k] >= 0.0 ? w[k] : share;
        T->cfixed[k] = 1;
        T->cpct[k] = 0.0;
    }
    free(w);
}

/* The table's min- / max-content content width; -1 when it has no rows or
 * cells. */
static double layout_table_intrinsic(LayoutContext *ctx, int idx, bool want_max)
{
    TableGrid T;
    double w = -1.0;
    if (table_build(ctx, idx, layout_box(ctx, idx), &T)) {
        w = table_spacing(&T);
        for (int k = 0; k < T.ncols; k++) w += want_max ? T.cmax[k] : T.cmin[k];
        for (int i = 0; i < T.ncaptions; i++) {
            double cw = layout_intrinsic_contribution(ctx, T.captions[i], false);
            if (cw > w) w = cw;
        }
    }
    table_free(&T);
    return w;
}

/* Column widths for a table content width W (§17.5.2.2, as browsers do it). */
static void table_distribute(const TableGrid *T, double W, double *w)
{
    double avail = W - table_spacing(T);
    if (avail < 0.0) avail = 0.0;
    double used = 0.0, mins = 0.0, maxs = 0.0;
    for (int k = 0; k < T->ncols; k++) {
        if (T->cpct[k] > 0.0) {
            w[k] = T->cpct[k] * avail;
            if (w[k] < T->cmin[k]) w[k] = T->cmin[k];
            used += w[k];
        } else {
            mins += T->cmin[k];
            maxs += T->cmax[k];
        }
    }
    double R = avail - used;
    int nauto = 0, nfixed = 0;
    double auto_max = 0.0, fixed_max = 0.0;
    for (int k = 0; k < T->ncols; k++) {
        if (T->cpct[k] > 0.0) continue;
        if (R <= mins) w[k] = T->cmin[k];
        else if (R <= maxs && maxs > mins) w[k] = T->cmin[k] + (T->cmax[k] - T->cmin[k]) * (R - mins) / (maxs - mins);
        else w[k] = T->cmax[k];
        if (T->cfixed[k]) { nfixed++; fixed_max += T->cmax[k]; }
        else { nauto++; auto_max += T->cmax[k]; }
    }
    double extra = R - maxs;
    if (extra <= 0.01) return;
    /* Space beyond every maximum: auto columns first, by their maximums. */
    for (int k = 0; k < T->ncols; k++) {
        if (T->cpct[k] > 0.0) continue;
        if (nauto > 0) {
            if (T->cfixed[k]) continue;
            w[k] += auto_max > 0.0 ? extra * T->cmax[k] / auto_max : extra / nauto;
        } else if (nfixed > 0) {
            w[k] += fixed_max > 0.0 ? extra * T->cmax[k] / fixed_max : extra / nfixed;
        }
    }
    if (nauto == 0 && nfixed == 0) {
        int np = 0;
        for (int k = 0; k < T->ncols; k++) if (T->cpct[k] > 0.0) np++;
        for (int k = 0; k < T->ncols && np > 0; k++) if (T->cpct[k] > 0.0) w[k] += extra / np;
    }
}

/* A column box over columns [first, first + n) of the grid, from the top of
 * the first row to the bottom of the last; past the grid's columns, none. */
static void table_place_column(LayoutBox *b, const double *colx, int ncols, int first, int n, double hs,
                               double top, double bottom)
{
    if (n <= 0 || first >= ncols) {
        b->width = b->height = 0.0;
    } else {
        int last = first + n < ncols ? first + n : ncols;
        b->x = colx[first];
        b->y = top;
        b->width = colx[last] - colx[first] - hs;
        if (b->width < 0.0) b->width = 0.0;
        b->height = bottom - top;
    }
    layout_update_content_sizes(b);
}

static void layout_table_box(LayoutContext *ctx, int idx, LayoutBox *tb);

static void layout_table(LayoutContext *ctx, int idx)
{
    layout_table_box(ctx, idx, layout_box(ctx, idx));
}

/* Table box tb (idx's own) of idx's children. */
static void layout_table_box(LayoutContext *ctx, int idx, LayoutBox *tb)
{
    TableGrid T;
    bool anon = tb != layout_box(ctx, idx);
    bool w_auto = !(tb->s->width_set || tb->s->width_percent > 0.0 || tb->s->width_keyword);
    double hpb = layout_horizontal_border_padding(tb);
    if (!table_build(ctx, idx, tb, &T)) {
        if (anon) { table_free(&T); tb->height = 0.0; return; }
        /* No rows or cells (display: table around blocks): a block that
         * shrinks to fit. */
        table_free(&T);
        if (w_auto) {
            double w = layout_shrink_to_fit(ctx, idx, tb->width);
            if (w < tb->width) { tb->width = w; layout_update_content_sizes(tb); }
        }
        layout_block_flow(ctx, idx);
        return;
    }
    if (tb->s->table_layout_fixed && !w_auto) table_fixed_columns(ctx, &T, tb->width - hpb);
    double minsum = table_spacing(&T), maxsum = minsum;
    for (int k = 0; k < T.ncols; k++) { minsum += T.cmin[k]; maxsum += T.cmax[k]; }
    double avail = tb->width - hpb;
    double W = w_auto ? (maxsum < avail ? maxsum : avail) : avail;
    if (W < minsum) W = minsum;
    if (fabs(W + hpb - tb->width) > 0.01) {
        /* Auto margins (or a <center> parent) center the narrowed table. */
        double old_w = tb->width;
        tb->width = W + hpb;
        int p = anon ? -1 : ctx->tree.nodes[idx].parent_idx;
        HtmlNode *pd = p >= 0 ? layout_node_dom(ctx, ctx->tree.nodes[p].dom_node_idx) : NULL;
        bool center_parent = pd && pd->type == HTML_NODE_ELEMENT && pd->tag_name &&
                             strcasecmp(pd->tag_name, "center") == 0;
        if (w_auto && tb->s->float_side == 0) {
            if ((tb->s->margin_auto & 0xA) == 0xA || center_parent) tb->x += (old_w - tb->width) / 2.0;
            else if (tb->s->margin_auto & 8) tb->x += old_w - tb->width;
        }
        layout_update_content_sizes(tb);
    }
    double content_x = tb->x + tb->padding_left + tb->s->border_left;
    double content_y = tb->y + tb->padding_top + tb->s->border_top;
    double cw = tb->content_width;

    double *colw = (double *)calloc((size_t)T.ncols + 1, sizeof(double));
    double *colx = (double *)calloc((size_t)T.ncols + 1, sizeof(double));
    int *list = (int *)malloc(((size_t)T.ncells + 8) * sizeof(int));
    if (!colw || !colx || !list) { free(colw); free(colx); free(list); table_free(&T); return; }
    table_distribute(&T, cw, colw);
    {
        double x = content_x + T.hs;
        for (int k = 0; k < T.ncols; k++) { colx[k] = x; x += colw[k] + T.hs; }
        colx[T.ncols] = x;
    }

    /* Captions above the rows. */
    double y = content_y;
    for (int i = 0; i < T.ncaptions; i++) {
        int c = T.captions[i];
        LayoutBox *cap = layout_box(ctx, c);
        layout_resolve_used_sizes(cap, layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx), cw, 0.0);
        cap->x = content_x + cap->margin_left;
        cap->y = y + cap->margin_top;
        layout_update_content_sizes(cap);
        layout_node_serial(ctx, c);
        y = cap->y + cap->height + cap->margin_bottom;
    }

    /* Cells at their column widths. */
    for (int i = 0; i < T.ncells; i++) {
        TableCell *c = &T.cells[i];
        LayoutBox *cell = layout_box(ctx, c->idx);
        HtmlNode *dom = layout_node_dom(ctx, ctx->tree.nodes[c->idx].dom_node_idx);
        double w = colx[c->col + c->cs] - colx[c->col] - T.hs;
        layout_resolve_used_sizes(cell, dom, cw, 0.0);
        cell->margin_left = cell->margin_right = cell->margin_top = cell->margin_bottom = 0.0;
        cell->x = colx[c->col];
        cell->y = y;
        cell->width = w < 0.0 ? 0.0 : w;
        layout_update_content_sizes(cell);
        list[i] = c->idx;
    }
    layout_units(ctx, list, T.ncells);

    /* Row heights: the tallest single-row cell, the row's own height, then
     * rowspans add what they still need to their last row. */
    for (int r = 0; r < T.nrows; r++) {
        TableRow *row = &T.rows[r];
        row->h = 0.0;
        if (row->idx >= 0) {
            LayoutBox *rb = layout_box(ctx, row->idx);
            if (rb->s->height_set && rb->s->height_percent <= 0.0) row->h = rb->s->css_height;
        }
    }
    for (int i = 0; i < T.ncells; i++) {
        TableCell *c = &T.cells[i];
        if (c->rs != 1) continue;
        double h = layout_box(ctx, c->idx)->height;
        if (h > T.rows[c->row].h) T.rows[c->row].h = h;
    }
    for (int i = 0; i < T.ncells; i++) {
        TableCell *c = &T.cells[i];
        if (c->rs == 1) continue;
        double have = T.vs * (c->rs - 1);
        for (int r = c->row; r < c->row + c->rs; r++) have += T.rows[r].h;
        double need = layout_box(ctx, c->idx)->height - have;
        if (need > 0.0) T.rows[c->row + c->rs - 1].h += need;
    }
    if (!layout_height_auto(tb) && T.nrows > 0) {
        /* A table taller than its rows: they share the rest, by their heights
         * (evenly when none has any). */
        double sum = 0.0;
        for (int r = 0; r < T.nrows; r++) sum += T.rows[r].h;
        double extra = tb->height - layout_vertical_border_padding(tb) - (y - content_y) - T.vs * (T.nrows + 1) - sum;
        for (int r = 0; extra > 0.0 && r < T.nrows; r++)
            T.rows[r].h += sum > 0.0 ? extra * T.rows[r].h / sum : extra / T.nrows;
    }
    y += T.vs;
    for (int r = 0; r < T.nrows; r++) {
        T.rows[r].y = y;
        y += T.rows[r].h + T.vs;
    }
    double rows_end = y;

    /* Cells into their rows; vertical-align moves their content. */
    for (int i = 0; i < T.ncells; i++) {
        TableCell *c = &T.cells[i];
        LayoutBox *cell = layout_box(ctx, c->idx);
        double h = T.vs * (c->rs - 1);
        for (int r = c->row; r < c->row + c->rs; r++) h += T.rows[r].h;
        layout_move_box(ctx, c->idx, cell->x, T.rows[c->row].y);
        double free_h = h - cell->height;
        if (free_h > 0.0) {
            double dy = cell->s->vertical_align == 1 ? free_h * 0.5 : cell->s->vertical_align == 3 ? free_h : 0.0;
            if (dy != 0.0) layout_shift_subtree_y(ctx, c->idx, dy);
            cell->height = h;
            layout_update_content_sizes(cell);
        }
    }
    /* Row and row-group boxes span their cells (backgrounds, hit testing). */
    double row_x = content_x + T.hs, row_w = cw - 2.0 * T.hs;
    if (row_w < 0.0) row_w = 0.0;
    for (int r = 0; r < T.nrows; r++) {
        if (T.rows[r].idx < 0) continue;
        LayoutBox *rb = layout_box(ctx, T.rows[r].idx);
        rb->x = row_x;
        rb->y = T.rows[r].y;
        rb->width = row_w;
        rb->height = T.rows[r].h;
        layout_update_content_sizes(rb);
    }
    for (int g = 0; g < T.ngroups; g++) {
        LayoutBox *gb = layout_box(ctx, T.groups[g]);
        double top = -1.0, bottom = 0.0;
        for (int r = 0; r < T.nrows; r++) {
            if (T.rows[r].group != g) continue;
            if (top < 0.0) top = T.rows[r].y;
            bottom = T.rows[r].y + T.rows[r].h;
        }
        gb->x = row_x;
        gb->y = top < 0.0 ? content_y : top;
        gb->width = row_w;
        gb->height = top < 0.0 ? 0.0 : bottom - top;
        layout_update_content_sizes(gb);
    }
    /* Column and column-group boxes span their columns' cells (their
     * backgrounds paint under the rows', §17.5.1), counted as
     * table_note_columns counts them. */
    if (T.nrows > 0) {
        double top = T.rows[0].y, bottom = T.rows[T.nrows - 1].y + T.rows[T.nrows - 1].h;
        int col = 0;
        for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
            LayoutBox *cb = layout_box(ctx, c);
            if (cb->s->display == CSS_DISPLAY_NONE || cb->s->table_role != 6 || !layout_is_in_flow(cb) ||
                table_skip_child(ctx, c))
                continue;
            int first = col;
            bool inner = false;
            for (int k = ctx->tree.nodes[c].first_child_idx; k >= 0; k = ctx->tree.nodes[k].next_sibling_idx) {
                LayoutBox *kb = layout_box(ctx, k);
                if (kb->s->display == CSS_DISPLAY_NONE || kb->s->table_role != 6) continue;
                inner = true;
                int n = table_span_attr(ctx, k, "span");
                table_place_column(kb, colx, T.ncols, col, n, T.hs, top, bottom);
                col += n;
            }
            if (!inner) col += table_span_attr(ctx, c, "span");
            table_place_column(cb, colx, T.ncols, first, col - first, T.hs, top, bottom);
        }
    }

    double content_h = rows_end - content_y;
    double total = content_h + layout_vertical_border_padding(tb);
    if (layout_height_auto(tb) || tb->height < total) {
        tb->height = total;
        layout_update_content_sizes(tb);
    }
    free(colw);
    free(colx);
    free(list);
    table_free(&T);
}

/* Children of an inline box that the parent flow wrapped across lines (or
 * broke at preserved newlines) and that has several significant children
 * (YouTube's attributed strings: text spans and link spans in one pre-wrap
 * host).  Each run continues where the previous one ended: its first line
 * is the rest of the current line, continuation lines use the box's wrap
 * geometry. */
static void layout_inline_wrapped_children(LayoutContext *ctx, int idx)
{
    LayoutBox *box = layout_box(ctx, idx);
    double cont_x = box->wrap_cont_x, cont_w = box->wrap_cont_w;
    double cx = box->x + box->padding_left + box->s->border_left;
    double cy = box->y;   /* (an inline's vertical padding and border take no room) */
    double line_start = cx;
    double line_right = box->x + box->wrap_first_w;
    for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
         c = ctx->tree.nodes[c].next_sibling_idx) {
        LayoutBox *child = layout_box(ctx, c);
        if (child->s->display == CSS_DISPLAY_NONE) continue;
        HtmlNode *dom = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
        layout_resolve_used_sizes(child, dom, box->content_width, box->content_height);
        double fs = child->s->font_size > 0.0 ? child->s->font_size : 16.0;
        double line_adv = layout_line_advance_box(child);
        bool keeps_nl = child->s->white_space >= 2 && child->s->white_space <= 4;
        child->wrap_cont_w = 0.0;
        child->wrap_cont2_line = 0;

        const char *text = NULL;
        char concat[8192];
        if (dom && dom->type == HTML_NODE_TEXT) {
            text = css_layout_box_text(ctx, c, css_layout_node_text(ctx, dom) ? css_layout_node_text(ctx, dom) : "");
        } else if (dom && dom->type == HTML_NODE_ELEMENT && child->s->display == CSS_DISPLAY_INLINE &&
                   strcasecmp(dom->tag_name, "img") != 0) {
            size_t cl = 0;
            concat[0] = '\0';
            layout_concat_box_text(ctx, c, concat, sizeof(concat), &cl);
            if (cl > 0) text = concat;
        }
        bool has_nl = text && keeps_nl && strchr(text, '\n');
        if (text && layout_text_is_whitespace(text) && !has_nl) {
            /* Collapsible space: one space between content, none at a line start. */
            double sw = 0.0;
            if (cx > line_start + 0.001 && !keeps_nl) {
                if (!layout_measure_text_styled(child, " ", fs, &sw, NULL)) sw = fs * 0.3;
            } else if (keeps_nl && !layout_measure_text_styled(child, text, fs, &sw, NULL)) {
                sw = 0.0;
            }
            child->x = cx;
            child->y = cy;
            child->width = sw;
            child->height = sw > 0.0 ? line_adv : 0.0;
            layout_update_content_sizes(child);
            cx += sw;
            layout_node_serial(ctx, c);
            continue;
        }
        if (text) {
            double tw = 0.0;
            layout_measure_text_styled(child, text, fs, &tw, NULL);
            TextShaper *font = display_list_get_font(layout_font_slot(child));
            if (!font) font = display_list_get_default_font();
            TsWrapResult wr;
            text_shaper_set_spacing(child->s->letter_spacing, child->s->word_spacing, (float)(fs / 16.0));
            bool wrapped = font && (has_nl || cx + tw > line_right + 0.01) &&
                text_shaper_wrap_measure(font, text, (float)cx, (float)cont_x,
                                         (float)(line_right - cx), (float)cont_w,
                                         (float)(fs / 16.0), (float)line_adv, 0, 0.0f, 0.0f, &wr);
            text_shaper_set_spacing(0.0f, 0.0f, 1.0f);
            if (wrapped) {
                child->x = cx;
                child->y = cy;
                child->width = wr.max_width;
                child->height = wr.height;
                child->wrap_first_w = line_right - cx;
                child->wrap_cont_w = cont_w;
                child->wrap_cont_x = cont_x;
                layout_update_content_sizes(child);
                layout_node_serial(ctx, c);
                if (wr.height > wr.line_advance + 0.01) {
                    cy += wr.height - wr.line_advance;
                    line_start = cont_x;
                    line_right = cont_x + cont_w;
                }
                cx = wr.last_end_x;
                continue;
            }
            child->x = cx;
            child->y = cy;
            child->width = tw;
            child->height = line_adv;
            layout_update_content_sizes(child);
            cx += tw;
            layout_node_serial(ctx, c);
            continue;
        }
        /* Atomic inline, image, or empty element: place it, breaking first
         * when it overflows a line that already has content. */
        if (layout_box_is_atomic_inline(child) && dom && dom->type == HTML_NODE_ELEMENT &&
            strcasecmp(dom->tag_name, "img") != 0)
            layout_size_atomic_inline(ctx, c, cont_w);
        double w = child->margin_left + child->width + child->margin_right;
        if (cx + w > line_right + 0.01 && cx > line_start + 0.001) {
            cy += line_adv;
            cx = line_start = cont_x;
            line_right = cont_x + cont_w;
        }
        child->x = cx + child->margin_left;
        child->y = cy + child->margin_top;
        layout_update_content_sizes(child);
        layout_node_serial(ctx, c);
        cx += w;
    }
}

/* ---- Incremental flow ----
 *
 * On a kept tree, each call of the flow and absolute-positioning passes is
 * recorded: the box's inputs (its geometry as its container set it, but
 * where: flow_entry_hash), its geometry when the call returned, and every
 * box's geometry after each pass.  The next layout takes a call's result
 * from that record when the box's subtree is clean (the same boxes in the
 * same order, styles left as they were, no text or children changed), the
 * inputs hash the same and no float is involved: the subtree's boxes get
 * their recorded geometry, shifted by how far the box moved.  Valid while
 * the viewport is the same and no image or font changed the measurements
 * (css_layout_retained_invalidate).  CYBER_FLOW_REUSE=0: lay out in full. */
typedef struct LayoutFlowBuf {
    int n, cap;
    uint64_t *entry;            /* inputs at its last call (0: none) */
    LayoutBox *exit;            /* geometry when that call returned */
    LayoutBox *snap1, *snap2;   /* every box after the flow pass, after absolute positioning */
    uint8_t *pass;              /* 1: laid out in the flow pass, 2: in absolute positioning */
    uint8_t *ok;                /* may be a reuse root (bit 0: in the flow pass, bit 1: absolute,
                                   bit 2: the containing blocks of what is absolutely
                                   positioned below are inside) */
    uint8_t *noreuse;           /* laid out with floats placed */
    uint8_t *gen;               /* its text had generated content */
    uint8_t *copied;            /* its ok and cb are the record's (reused in the absolute pass) */
    uint64_t *img;              /* an image's natural size as layout saw it (0: none, or unknown) */
    int *cb;                    /* how far below it the highest containing block of what is
                                   absolutely positioned below is (< 0: above it; FLOW_NO_CB) */
    int *first_child, *next_sibling;
} LayoutFlowBuf;

#define FLOW_NO_CB (1 << 20)

typedef struct LayoutFlow {
    LayoutFlowBuf rec, cur;     /* the last layout's record, this one's */
    bool rec_valid;
    uint64_t epoch;
    double vw, vh;
    unsigned char pass;         /* calls recorded in: 1 flow, 2 absolute positioning (0: none) */
    bool reuse;                 /* rec is good for this layout */
    uint8_t *clean;             /* per index of this layout: its subtree is as recorded */
    uint8_t *skip;              /* reused in the absolute pass: its subtree is final */
    int *depth;
    int clean_cap;
} LayoutFlow;

static uint64_t g_flow_epoch = 1;
static _Atomic uint64_t g_flow_hits, g_flow_hit_boxes;

void css_layout_retained_invalidate(void)
{
    g_flow_epoch++;
}

void css_layout_flow_stats(uint64_t *subtrees, uint64_t *boxes)
{
    if (subtrees) *subtrees = atomic_load(&g_flow_hits);
    if (boxes) *boxes = atomic_load(&g_flow_hit_boxes);
}

static void flow_buf_free(LayoutFlowBuf *b)
{
    free(b->entry); free(b->exit); free(b->snap1); free(b->snap2);
    free(b->pass); free(b->ok); free(b->noreuse); free(b->gen); free(b->copied); free(b->cb);
    free(b->img); free(b->first_child); free(b->next_sibling);
    memset(b, 0, sizeof(*b));
}

static bool flow_buf_ensure(LayoutFlowBuf *b, int n)
{
    if (b->cap >= n && b->entry) return true;
    flow_buf_free(b);
    n += n / 4 + 16;   /* (a tree that grows a little keeps its buffers) */
    size_t m = (size_t)n + 1;
    b->entry = (uint64_t *)malloc(m * sizeof(uint64_t));
    b->exit = (LayoutBox *)malloc(m * sizeof(LayoutBox));
    b->snap1 = (LayoutBox *)malloc(m * sizeof(LayoutBox));
    b->snap2 = (LayoutBox *)malloc(m * sizeof(LayoutBox));
    b->pass = (uint8_t *)malloc(m);
    b->ok = (uint8_t *)malloc(m);
    b->noreuse = (uint8_t *)malloc(m);
    b->gen = (uint8_t *)malloc(m);
    b->copied = (uint8_t *)malloc(m);
    b->cb = (int *)malloc(m * sizeof(int));
    b->img = (uint64_t *)malloc(m * sizeof(uint64_t));
    b->first_child = (int *)malloc(m * sizeof(int));
    b->next_sibling = (int *)malloc(m * sizeof(int));
    if (!b->entry || !b->exit || !b->snap1 || !b->snap2 || !b->pass || !b->ok || !b->noreuse || !b->gen ||
        !b->copied || !b->cb || !b->img || !b->first_child || !b->next_sibling) {
        flow_buf_free(b);
        return false;
    }
    b->cap = n;
    return true;
}

static void layout_flow_free(LayoutFlow *F)
{
    if (!F) return;
    flow_buf_free(&F->rec);
    flow_buf_free(&F->cur);
    free(F->clean);
    free(F->skip);
    free(F->depth);
    free(F);
}

/* What a container sets before laying out a box: its geometry, but where. */
static uint64_t flow_entry_hash(const LayoutBox *b)
{
    const size_t off = offsetof(LayoutBox, width);
    const unsigned char *p = (const unsigned char *)b + off;
    size_t n = sizeof(LayoutBox) - off, i = 0;
    /* four independent lanes (the multiplies overlap), folded at the end */
    uint64_t h = 1469598103934665603ull, h1 = 0x243F6A8885A308D3ull, h2 = 0x13198A2E03707344ull, h3 = 0xA4093822299F31D0ull;
    for (; i + 32 <= n; i += 32) {
        uint64_t w0, w1, w2, w3;
        memcpy(&w0, p + i, 8); memcpy(&w1, p + i + 8, 8); memcpy(&w2, p + i + 16, 8); memcpy(&w3, p + i + 24, 8);
        h = (h ^ w0) * 0x9E3779B97F4A7C15ull;
        h1 = (h1 ^ w1) * 0x9E3779B97F4A7C15ull;
        h2 = (h2 ^ w2) * 0x9E3779B97F4A7C15ull;
        h3 = (h3 ^ w3) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29; h1 ^= h1 >> 29; h2 ^= h2 >> 29; h3 ^= h3 >> 29;
    }
    h = (h ^ (h1 * 0xC2B2AE3D27D4EB4Full) ^ (h2 * 0x165667B19E3779F9ull) ^ (h3 * 0x27D4EB2F165667C5ull)) * 0x9E3779B97F4A7C15ull;
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h ? h : 1;
}

static bool layout_has_gen(LayoutContext *ctx, int i)
{
    int d = ctx->tree.nodes[i].dom_node_idx;
    return ctx->text_gen && d >= 0 && d < ctx->text_gen_cap && ctx->text_gen[d];
}

/* Before the flow pass (styles final): this layout's record, and which
 * subtrees are as recorded. */
static void layout_flow_begin(LayoutContext *ctx)
{
    static int env = -1;
    if (env < 0) {
        const char *e = cyber_getenv("CYBER_FLOW_REUSE");
        env = !(e && e[0] == '0');
    }
    int n = ctx->tree.count;
    /* (only on kept trees: a context that builds every time never reuses) */
    if (!env || !ctx->tree_kept || n <= 0) return;
    LayoutFlow *F = ctx->flow;
    if (!F) F = ctx->flow = (LayoutFlow *)calloc(1, sizeof(LayoutFlow));
    if (!F || !flow_buf_ensure(&F->cur, n)) return;
    if (F->clean_cap < n) {
        free(F->clean); free(F->skip); free(F->depth);
        F->clean = (uint8_t *)malloc((size_t)n + 1);
        F->skip = (uint8_t *)malloc((size_t)n + 1);
        F->depth = (int *)malloc(((size_t)n + 1) * sizeof(int));
        F->clean_cap = F->clean && F->skip && F->depth ? n : 0;
        if (!F->clean_cap) return;
    }
    memset(F->cur.entry, 0, (size_t)n * sizeof(uint64_t));
    memset(F->cur.pass, 0, (size_t)n);
    memset(F->cur.noreuse, 0, (size_t)n);
    memset(F->cur.copied, 0, (size_t)n);
    memset(F->skip, 0, (size_t)n);
    F->cur.n = n;
    F->pass = 1;
    /* Images size boxes from outside the document: one whose natural size
     * became known (or changed) since the record is not as recorded. */
    for (int i = 0; i < n; i++) {
        HtmlNode *dn = layout_node_dom(ctx, ctx->tree.nodes[i].dom_node_idx);
        uint64_t sig = 0;
        double nw, nh;
        if (layout_is_img(dn) && layout_img_natural(ctx, dn, &nw, &nh))
            sig = ((uint64_t)(uint32_t)nw << 32 | (uint32_t)nh) + 1;
        F->cur.img[i] = sig;
    }
    F->reuse = F->rec_valid && F->epoch == g_flow_epoch && F->vw == ctx->viewport_width &&
               F->vh == ctx->viewport_height && ctx->style_left && (ctx->flow_old || F->rec.n == n);
    if (!F->reuse) return;
    const LayoutFlowBuf *R = &F->rec;
    uint8_t *clean = F->clean;
    memset(clean, 1, (size_t)n);
    for (int i = 0; i < n; i++) {
        int oi = ctx->flow_old ? ctx->flow_old[i] : i;
        if (oi >= 0 && R->img[oi] != F->cur.img[i]) clean[i] = 0;
    }
    /* The elements whose children changed, and those children (their text
     * may have). */
    for (int k = 0; k < ctx->child_n; k++) {
        int d = html_mirror_index_of(ctx->doc, ctx->child_ids[k]);
        int li = d >= 0 && d < ctx->dom_map_n ? ctx->dom_map[d] : -1;
        if (li < 0) continue;
        clean[li] = 0;
        for (int c = ctx->tree.nodes[li].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) clean[c] = 0;
    }
    /* Children before parents. */
    for (int k = n - 1; k >= 0; k--) {
        int i = ctx->tree.preorder[k];
        int oi = ctx->flow_old ? ctx->flow_old[i] : i;
        bool c = clean[i] && oi >= 0 && ctx->style_left[i] && !layout_has_gen(ctx, i) && !R->gen[oi];
        int oc = c ? R->first_child[oi] : -1;
        for (int ch = ctx->tree.nodes[i].first_child_idx; c && ch >= 0; ch = ctx->tree.nodes[ch].next_sibling_idx) {
            int och = ctx->flow_old ? ctx->flow_old[ch] : ch;
            if (oc < 0 || och != oc || !clean[ch]) c = false;
            else oc = R->next_sibling[oc];
        }
        if (c && oc >= 0) c = false;
        clean[i] = c;
    }
}

static int layout_positioned_ancestor(LayoutContext *ctx, int idx);

static bool flow_parent_is_row(LayoutContext *ctx, int i)
{
    int p = ctx->tree.nodes[i].parent_idx;
    if (p < 0) return false;
    HtmlNode *pn = layout_node_dom(ctx, ctx->tree.nodes[p].dom_node_idx);
    return pn && pn->type == HTML_NODE_ELEMENT && pn->tag_name && strcasecmp(pn->tag_name, "tr") == 0;
}

/* A box and its subtree from the record: the box's geometry when its call
 * returned, its subtree's after the pass, shifted by how far it moved (as
 * layout_offset_subtree moves boxes). */
static void flow_shift(LayoutBox *b, double dx, double dy)
{
    b->x += dx;
    b->y += dy;
    if (b->wrap_cont_w > 0.0) {
        b->wrap_cont_x += dx;
        if (b->wrap_cont2_line > 1) b->wrap_cont2_x += dx;
    }
}

/* (mx, my: how far the root moved after its call in the recorded layout;
 * a box the layout never positioned, still at the origin layout_box_start
 * gave it, moved that much and no more) */
static void flow_reuse_children(LayoutContext *ctx, LayoutFlow *F, const LayoutBox *snap,
                                int i, int o, double dx, double dy, double mx, double my)
{
    const LayoutFlowBuf *R = &F->rec;
    LayoutFlowBuf *C = &F->cur;
    int oc = R->first_child[o];
    for (int c = ctx->tree.nodes[i].first_child_idx; c >= 0 && oc >= 0;
         c = ctx->tree.nodes[c].next_sibling_idx, oc = R->next_sibling[oc]) {
        LayoutBox *b = &ctx->boxes[c];
        LayoutStyle *s = b->s;
        double px = b->x, py = b->y;
        bool unplaced = snap[oc].x == mx && snap[oc].y == my && px == 0.0 && py == 0.0;
        *b = snap[oc];
        b->s = s;
        if (unplaced) { b->x = px; b->y = py; }
        else flow_shift(b, dx, dy);
        C->entry[c] = R->entry[oc];
        C->exit[c] = R->exit[oc];
        flow_shift(&C->exit[c], dx, dy);
        C->pass[c] = R->pass[oc];
        C->noreuse[c] = R->noreuse[oc];
        if (F->pass == 2) { C->ok[c] = R->ok[oc]; C->cb[c] = R->cb[oc]; C->copied[c] = 1; }
        flow_reuse_children(ctx, F, snap, c, oc, dx, dy, mx, my);
    }
}

/* Box idx and its ancestors are not reused at the next flow pass. */
static void layout_flow_noreuse_up(LayoutContext *ctx, int idx)
{
    LayoutFlow *F = ctx->flow;
    if (!F || !F->pass) return;
    for (int i = idx; i >= 0; i = ctx->tree.nodes[i].parent_idx) F->cur.noreuse[i] = 1;
}

static bool flow_try(LayoutContext *ctx, LayoutFlow *F, int idx, uint64_t entry)
{
    if (!F->clean[idx] || ctx->float_count != 0) return false;
    const LayoutStyle *st = ctx->boxes[idx].s;
    /* An inline box is part of its container's lines (they move it, not
     * its contents, after its call); a leaf has nothing to reuse; an
     * absolutely positioned box is sized and placed before its call (in
     * the absolute pass, against its containing block) and may be moved
     * after it, as any box; a fixed one is not, against the viewport. */
    if (st->display == CSS_DISPLAY_INLINE || ctx->tree.nodes[idx].first_child_idx < 0) return false;
    if (st->position == CSS_POSITION_FIXED) return false;
    if (flow_parent_is_row(ctx, idx)) return false;
    const LayoutFlowBuf *R = &F->rec;
    int o = ctx->flow_old ? ctx->flow_old[idx] : idx;
    unsigned char pass = F->pass;
    /* (in the absolute pass, what is absolutely positioned below is placed
     * against containing blocks inside: the record has it placed) */
    uint8_t need = pass == 2 ? 6 : 1;
    if (o < 0 || R->pass[o] != pass || (R->ok[o] & need) != need || R->entry[o] != entry) return false;
    const LayoutBox *snap = pass == 2 ? R->snap2 : R->snap1;
    LayoutBox *r = &ctx->boxes[idx];
    double x = r->x, y = r->y;
    double dx = x - snap[o].x, dy = y - snap[o].y;
    double mx = snap[o].x - R->exit[o].x, my = snap[o].y - R->exit[o].y;
    LayoutStyle *s = r->s;
    *r = R->exit[o];
    r->s = s;
    r->x = x;
    r->y = y;
    F->cur.entry[idx] = entry;
    F->cur.exit[idx] = *r;
    F->cur.pass[idx] = pass;
    if (pass == 2) {
        F->cur.ok[idx] = R->ok[o];
        F->cur.cb[idx] = R->cb[o];
        F->cur.copied[idx] = 1;
        F->skip[idx] = 1;   /* (layout_position_absolute_subtree: its subtree is final) */
    }
    flow_reuse_children(ctx, F, snap, idx, o, dx, dy, mx, my);
    atomic_fetch_add(&g_flow_hits, 1);
    atomic_fetch_add(&g_flow_hit_boxes, (uint64_t)g_lpar.subtree[idx]);
    return true;
}

/* After each pass: every box's geometry (before translations). */
static void layout_flow_snap(LayoutContext *ctx, bool abs)
{
    LayoutFlow *F = ctx->flow;
    if (!F || !F->pass) return;
    memcpy(abs ? F->cur.snap2 : F->cur.snap1, ctx->boxes, (size_t)ctx->tree.count * sizeof(LayoutBox));
}

/* An empty or hidden box c outside its parent p in snapshot g, moved from
 * the origin it started at: a position some measuring call wrote, which
 * does not follow the parent. */
static bool flow_stray(const LayoutBox *g, int p, int c, CssDisplay display)
{
    const LayoutBox *b = &g[c], *pb = &g[p];
    if (display != CSS_DISPLAY_NONE && (b->width != 0.0 || b->height != 0.0)) return false;
    if (b->x == 0.0 && b->y == 0.0) return false;
    return b->x < pb->x || b->x > pb->x + pb->width || b->y < pb->y || b->y > pb->y + pb->height;
}

/* After the layout: its record replaces the last one.  A reuse root: no
 * float, fixed box or float stack at its entry inside (bit 0); in the
 * absolute pass also no absolutely positioned box below it (bit 1: they are
 * placed after its call); nor an empty or hidden box left outside its
 * parent (flow_stray). */
static void layout_flow_end(LayoutContext *ctx)
{
    LayoutFlow *F = ctx->flow;
    free(ctx->flow_old);
    ctx->flow_old = NULL;
    if (!F || !F->pass) return;
    F->pass = 0;
    int n = ctx->tree.count;
    LayoutFlowBuf *C = &F->cur;
    for (int i = 0; i < n; i++) {
        C->first_child[i] = ctx->tree.nodes[i].first_child_idx;
        C->next_sibling[i] = ctx->tree.nodes[i].next_sibling_idx;
        C->gen[i] = layout_has_gen(ctx, i);
    }
    int *depth = F->depth;
    for (int k = 0; k < n; k++) {
        int i = ctx->tree.preorder[k], p = ctx->tree.nodes[i].parent_idx;
        depth[i] = p >= 0 ? depth[p] + 1 : 0;
    }
    for (int k = n - 1; k >= 0; k--) {
        int i = ctx->tree.preorder[k];
        if (C->copied[i]) continue;
        const LayoutBox *b = &ctx->boxes[i];
        /* (left unstyled inside display:none: nothing lays it out, and its
         * style slot holds only display) */
        if (b->flags & LAYOUT_STYLE_SKIPPED) { C->ok[i] = 7; C->cb[i] = FLOW_NO_CB; continue; }
        bool ok = !C->noreuse[i] && b->s->float_side == 0;
        bool ok_abs = ok;
        int cb = FLOW_NO_CB;
        for (int c = ctx->tree.nodes[i].first_child_idx; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) {
            if (ctx->boxes[c].flags & LAYOUT_STYLE_SKIPPED) continue;
            const LayoutStyle *cs = ctx->boxes[c].s;
            bool fixed = cs->position == CSS_POSITION_FIXED;
            ok = ok && (C->ok[c] & 1) && !fixed && !flow_stray(C->snap1, i, c, cs->display);
            ok_abs = ok_abs && (C->ok[c] & 2) && !fixed && !flow_stray(C->snap2, i, c, cs->display);
            if (C->cb[c] != FLOW_NO_CB && C->cb[c] + 1 < cb) cb = C->cb[c] + 1;
            if (cs->position == CSS_POSITION_ABSOLUTE) {
                int a = layout_positioned_ancestor(ctx, c);
                int v = a >= 0 ? depth[a] - depth[i] : -FLOW_NO_CB;
                if (v < cb) cb = v;
            }
        }
        C->cb[i] = cb;
        C->ok[i] = (uint8_t)((ok ? 1 : 0) | (ok_abs ? 2 : 0) | (cb >= 0 ? 4 : 0));
    }
    LayoutFlowBuf t = F->rec;
    F->rec = F->cur;
    F->cur = t;
    F->rec_valid = true;
    F->epoch = g_flow_epoch;
    F->vw = ctx->viewport_width;
    F->vh = ctx->viewport_height;
}

static void layout_node_serial(LayoutContext *ctx, int idx)
{
    LayoutFlow *F = ctx->flow;
    if (F && F->pass) {
        uint64_t e = flow_entry_hash(&ctx->boxes[idx]);
        if (F->reuse && flow_try(ctx, F, idx, e)) return;
        if (ctx->float_count != 0) F->cur.noreuse[idx] = 1;
        layout_node_serial_impl(ctx, idx);
        F->cur.pass[idx] = F->pass;
        F->cur.entry[idx] = e;
        F->cur.exit[idx] = ctx->boxes[idx];
        return;
    }
    layout_node_serial_impl(ctx, idx);
}

static void layout_node_serial_impl(LayoutContext *ctx, int idx)
{
    LayoutBox *box = layout_box(ctx, idx);

    if (box->s->display == CSS_DISPLAY_NONE) return;
    if (box->s->display == CSS_DISPLAY_INLINE) {
        /* Inline boxes don't establish a block formatting context, but their
         * inline children still need positions: flow them left-to-right,
         * measuring text with real glyph metrics when the font is available
         * so positions match the parent's measured width. */
        double cx = box->x + box->padding_left + box->s->border_left;
        double cy = box->y;   /* (its vertical padding and border take no room on the line) */
        /* (it starts its line: its first text has no leading space) */
        bool lead = box->text_lead_skip != 0;
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
                if (child->s->display == CSS_DISPLAY_NONE) continue;
                HtmlNode *dn = layout_node_dom(ctx, ctx->tree.nodes[c].dom_node_idx);
                if (dn && dn->type == HTML_NODE_TEXT &&
                    layout_text_is_whitespace(css_layout_node_text(ctx, dn))) continue;
                sig_count++;
                wrap_target = c;
            }
            if (sig_count != 1) wrap_target = -1;
            if (sig_count > 1) {
                layout_inline_wrapped_children(ctx, idx);
                return;
            }
        }
        for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
             c = ctx->tree.nodes[c].next_sibling_idx) {
            LayoutBox *child = layout_box(ctx, c);
            if (child->s->display == CSS_DISPLAY_NONE) continue;
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
            double fs = child->s->font_size > 0.0 ? child->s->font_size : 16.0;
            double line_adv = layout_line_advance_box(child);
            bool sized = false;
            if (dom && dom->type == HTML_NODE_TEXT) {
                if (layout_text_is_whitespace(css_layout_node_text(ctx, dom))) {
                    /* Same contextual collapse as the block-flow inline branch:
                     * a space between inline content, nothing at the start. */
                    double content_left_i = box->x + box->padding_left + box->s->border_left;
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
                    const char *tt = css_layout_box_text(ctx, c, css_layout_node_text(ctx, dom));
                    child->text_lead_skip = 0;
                    if (lead && tt[0] == ' ' && child->s->white_space != 2 && child->s->white_space != 3) {
                        tt++;
                        child->text_lead_skip = 1;
                    }
                    lead = false;
                    if (layout_measure_text_styled(child, tt, fs, &tw, NULL)) {
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
            if (!sized && dom && dom->type == HTML_NODE_ELEMENT && dom->tag_name &&
                strcasecmp(dom->tag_name, "canvas") == 0) {
                layout_svg_size(ctx, c, dom, &child->width, &child->height);
                sized = true;
            }
            if (dom && dom->type == HTML_NODE_ELEMENT && child->s->display == CSS_DISPLAY_INLINE) {
                child->text_lead_skip = lead;
                lead = false;
            }
            if (!sized && dom && dom->type == HTML_NODE_ELEMENT &&
                child->s->display == CSS_DISPLAY_INLINE) {
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
                layout_concat_box_text(ctx, c, concat, sizeof(concat), &cl);
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
            if (child->s->display == CSS_DISPLAY_INLINE && dom && dom->type == HTML_NODE_ELEMENT &&
                !layout_is_img(dom) && child->wrap_cont_w <= 0.0 && fabs(child->height - line_adv) < 0.01) {
                /* Its visual box, as the block flow gives a line's inlines:
                 * the content area plus padding and border. */
                double ca = layout_content_area_height(child);
                child->y += (line_adv - ca) / 2.0 - child->padding_top - child->s->border_top;
                child->height = ca + layout_vertical_border_padding(child);
                layout_update_content_sizes(child);
            }
        }
        return;
    }

    if (box->s->display == CSS_DISPLAY_FLEX) {
        layout_flex_container(ctx, idx);
        return;
    }
    if (box->s->display == CSS_DISPLAY_GRID) {
        layout_grid_container(ctx, idx);
        return;
    }
    if (table_is_table(box)) {
        layout_table(ctx, idx);
        return;
    }

    /* Rows outside a table get a (minimal) horizontal cell layout. */
    HtmlNode *rowdom = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    if (rowdom && rowdom->type == HTML_NODE_ELEMENT &&
        strcasecmp(rowdom->tag_name, "tr") == 0) {
        layout_table_row(ctx, idx);
        return;
    }

    /* Block (and grid/other treated as block). */
    layout_block_flow(ctx, idx);
}

/* The root box in the initial containing block (the viewport): its margins
 * apply, its width fills what they leave, its height is its own or (auto)
 * its content's, found by the flow (§10.1, §10.6.3). */
static void layout_root(LayoutContext *ctx)
{
    int root = ctx->tree.root_idx;
    if (root < 0) return;
    LayoutBox *box = layout_box(ctx, root);
    layout_resolve_used_sizes(box, layout_node_dom(ctx, ctx->tree.nodes[root].dom_node_idx),
                              ctx->viewport_width, ctx->viewport_height);
    box->x = box->margin_left;
    box->y = box->margin_top;
    if (layout_height_auto(box)) box->height = 0.0;
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
        if (b->flags & LAYOUT_STYLE_SKIPPED) { fprintf(fp, "%d\t%s\t\t\tnone (not styled)\n", i, tag); continue; }
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
        switch (b->s->display) {
            case CSS_DISPLAY_BLOCK: dname = "block"; break;
            case CSS_DISPLAY_INLINE: dname = "inline"; break;
            case CSS_DISPLAY_INLINE_BLOCK: dname = "inline-block"; break;
            case CSS_DISPLAY_FLEX: dname = "flex"; break;
            case CSS_DISPLAY_GRID: dname = "grid"; break;
            case CSS_DISPLAY_NONE: dname = "none"; break;
            default: dname = "other"; break;
        }
        const char *pname = "static";
        switch (b->s->position) {
            case CSS_POSITION_RELATIVE: pname = "relative"; break;
            case CSS_POSITION_ABSOLUTE: pname = "absolute"; break;
            case CSS_POSITION_FIXED:    pname = "fixed"; break;
            case CSS_POSITION_STICKY:   pname = "sticky"; break;
            default: break;
        }
        const char *bsname = "content-box";
        if (b->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX) bsname = "border-box";
        char bg_buf[32] = "-";
        if (b->s->background_color_a > 0.0) {
            snprintf(bg_buf, sizeof(bg_buf), "#%02x%02x%02x/a%.2f",
                     (int)(b->s->background_color_r * 255.0),
                     (int)(b->s->background_color_g * 255.0),
                     (int)(b->s->background_color_b * 255.0),
                     b->s->background_color_a);
        }
        const char *vname = (b->s->visibility == CSS_VISIBILITY_HIDDEN) ? "hidden" : "visible";
        char txt_buf[48] = "";
        if (node && node->type == HTML_NODE_TEXT && css_layout_node_text(ctx, node)) {
            strncpy(txt_buf, css_layout_node_text(ctx, node), sizeof(txt_buf) - 1);
            for (size_t k = 0; k < strlen(txt_buf); k++)
                if (txt_buf[k] == '\t' || txt_buf[k] == '\n' || txt_buf[k] == '\r') txt_buf[k] = ' ';
        }
        fprintf(fp, "%d\t%s\t%s\t%s\t%s\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%s\t%s\t%s\t%s\t%.2f\t%s\n",
                i, tag, id_buf, cls_buf, dname,
                b->x, b->y, b->width, b->height,
                b->margin_top, b->margin_right, b->margin_bottom, b->margin_left,
                b->padding_top, b->padding_right, b->padding_bottom, b->padding_left,
                b->s->border_top, b->s->border_right, b->s->border_bottom, b->s->border_left,
                b->s->flex_grow, b->s->flex_shrink, pname, bsname, bg_buf, vname,
                b->s->font_size, txt_buf);
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
        CssPosition pos = layout_box(ctx, p)->s->position;
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
    LayoutBox *B = layout_box(ctx, idx);
    if (B->has_static) {
        /* (from the line it is on: inline layout on items) */
        *sx = P->x + B->static_dx;
        *sy = P->y + B->static_dy;
        return;
    }
    *sx = P->x + P->s->border_left + P->padding_left;
    *sy = P->y + P->s->border_top + P->padding_top;
    if (P->s->display == CSS_DISPLAY_FLEX || P->s->display == CSS_DISPLAY_GRID) return;
    for (int k = ctx->tree.nodes[idx].prev_sibling_idx; k >= 0; k = ctx->tree.nodes[k].prev_sibling_idx) {
        LayoutBox *kb = layout_box(ctx, k);
        if (!layout_is_in_flow(kb) || kb->s->float_side != 0) continue;
        HtmlNode *kn = layout_node_dom(ctx, ctx->tree.nodes[k].dom_node_idx);
        if (kn && kn->type == HTML_NODE_TEXT && layout_text_is_whitespace(css_layout_node_text(ctx, kn))) continue;
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
    box->has_static = 0;

    /* The containing block is the padding box of the nearest positioned
     * ancestor; fixed boxes use the viewport (scroll position 0). */
    double cb_x, cb_y, cb_w, cb_h;
    int anc = box->s->position == CSS_POSITION_FIXED ? -1 : layout_positioned_ancestor(ctx, idx);
    if (anc >= 0) {
        LayoutBox *a = layout_box(ctx, anc);
        cb_x = a->x + a->s->border_left;
        cb_y = a->y + a->s->border_top;
        cb_w = a->width - a->s->border_left - a->s->border_right;
        cb_h = a->height - a->s->border_top - a->s->border_bottom;
        if (cb_w < 0.0) cb_w = 0.0;
        if (cb_h < 0.0) cb_h = 0.0;
    } else {
        cb_x = 0.0;
        cb_y = 0.0;
        cb_w = ctx->viewport_width;
        cb_h = ctx->viewport_height;
    }

    bool has_left = (box->s->positioned_sides & LAYOUT_SIDE_LEFT) != 0;
    bool has_right = (box->s->positioned_sides & LAYOUT_SIDE_RIGHT) != 0;
    bool has_top = (box->s->positioned_sides & LAYOUT_SIDE_TOP) != 0;
    bool has_bottom = (box->s->positioned_sides & LAYOUT_SIDE_BOTTOM) != 0;
    layout_resolve_insets(box, cb_w, cb_h);

    HtmlNode *adom = layout_node_dom(ctx, ctx->tree.nodes[idx].dom_node_idx);
    layout_resolve_used_sizes(box, adom, cb_w, cb_h);
    bool width_auto = !(box->s->width_set || box->s->width_percent > 0.0 || box->s->width_keyword);
    if (layout_is_svg_box(adom)) {
        /* A replaced element: its used size from its own, not shrink-to-fit
         * (§10.3.8, §10.6.5), percentages against this containing block. */
        double sw = 0.0, sh = 0.0;
        layout_svg_size_in(box, adom, cb_w, cb_h,
                           &sw, &sh);
        if (width_auto) box->width = sw + layout_horizontal_border_padding(box);
        if (layout_height_auto(box)) box->height = sh + layout_vertical_border_padding(box);
        layout_update_content_sizes(box);
        width_auto = false;
    }
    double ml = (box->s->margin_auto & 8) ? 0.0 : box->margin_left;
    double mr = (box->s->margin_auto & 2) ? 0.0 : box->margin_right;
    double mt = (box->s->margin_auto & 1) ? 0.0 : box->margin_top;
    double mb = (box->s->margin_auto & 4) ? 0.0 : box->margin_bottom;

    /* ---- horizontal (§10.3.7) ---- */
    if (width_auto) {
        if (has_left && has_right) {
            box->width = cb_w - box->left - box->right - ml - mr;
        } else {
            double avail = cb_w - (has_left ? box->left : 0.0) - (has_right ? box->right : 0.0) - ml - mr;
            box->width = layout_shrink_to_fit(ctx, idx, avail < 0.0 ? 0.0 : avail);
        }
        if (box->max_width >= 0.0 || box->min_width > 0.0) {
            if (box->s->box_sizing == CSS_BOX_SIZING_BORDER_BOX)
                box->width = layout_clamp_size(box->width, box->min_width, box->max_width);
            else
                box->width = layout_content_to_total_width(box,
                    layout_clamp_size(layout_total_to_content_width(box, box->width),
                                      box->min_width, box->max_width));
        }
        if (box->width < 0.0) box->width = 0.0;
        layout_update_content_sizes(box);
    }
    if (has_left && has_right && !width_auto && (box->s->margin_auto & 0xA)) {
        double free_w = cb_w - box->left - box->right - box->width - ml - mr;
        if ((box->s->margin_auto & 0xA) == 0xA) { ml += free_w / 2.0; }
        else if (box->s->margin_auto & 8) ml += free_w;
    }
    /* (rtl: over-constrained, left is the one ignored; neither set, the
     * static position is against the parent's right content edge) */
    bool cb_rtl = (anc >= 0 ? layout_box(ctx, anc) : layout_box(ctx, ctx->tree.root_idx))->s->direction == 1;
    int sp = ctx->tree.nodes[idx].parent_idx;
    const LayoutBox *spb = sp >= 0 ? layout_box(ctx, sp) : NULL;
    if (has_left && !(has_right && cb_rtl && !(box->s->margin_auto & 0xA))) box->x = cb_x + box->left + ml;
    else if (has_right) box->x = cb_x + cb_w - box->right - mr - box->width;
    else if (spb && spb->s->direction == 1)
        box->x = spb->x + spb->s->border_left + spb->padding_left + spb->content_width - mr - box->width;
    else box->x = static_x + ml;

    /* ---- vertical (§10.6.4) ---- */
    bool height_auto = layout_height_auto(box) && !(box->s->aspect_ratio > 0.0 && box->height > 0.0);
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
    } else if (has_bottom && !height_auto && (box->s->margin_auto & 5)) {
        double free_h = cb_h - box->top - box->bottom - box->height - mt - mb;
        double dy = ((box->s->margin_auto & 5) == 5) ? free_h / 2.0 : ((box->s->margin_auto & 1) ? free_h : 0.0);
        if (dy != 0.0) layout_move_box(ctx, idx, box->x, box->y + dy);
    }
}

static void layout_position_absolute_subtree(LayoutContext *ctx, int idx)
{
    LayoutBox *box = layout_box(ctx, idx);
    if (box->s->display == CSS_DISPLAY_NONE) return;
    /* (reused in this pass: what is absolutely positioned in it is placed) */
    if (ctx->flow && ctx->flow->pass == 2 && ctx->flow->skip[idx]) return;
    if (box->s->position == CSS_POSITION_ABSOLUTE || box->s->position == CSS_POSITION_FIXED) {
        layout_position_absolute_box(ctx, idx);
        if (ctx->flow && ctx->flow->pass == 2 && ctx->flow->skip[idx]) return;   /* (its call was reused) */
    }
    for (int c = ctx->tree.nodes[idx].first_child_idx; c >= 0;
         c = ctx->tree.nodes[c].next_sibling_idx) {
        layout_position_absolute_subtree(ctx, c);
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */
static void layout_flow_job(Job *j)
{
    LayoutContext *ctx = (LayoutContext *)j->ctx;
    if (ctx->tree.root_idx >= 0) layout_node_serial(ctx, ctx->tree.root_idx);
    layout_flow_snap(ctx, false);
    if (ctx->flow && ctx->flow->pass) ctx->flow->pass = 2;
    if (ctx->tree.count > 0) layout_position_absolute_subtree(ctx, ctx->tree.root_idx);
    layout_flow_snap(ctx, true);
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
    if (ctx->viewport_width > 0.0) g_calc_pct_hint = ctx->viewport_width;
    layout_apply_stylesheet(ctx, sheet);
    /* A display:none guess that came out shown, or :has() rules other than
     * those the guesses were checked against: css_layout_run builds again. */
    if (ctx->pruned_n) {
        bool failed_has = ctx->prune_has_gen != g_has_gen || ctx->has_target_first < ctx->tree.count;
        bool failed = failed_has;
        for (int i = 0; !failed && i < ctx->tree.count; i++)
            failed = ctx->pruned[i] && ctx->boxes[i].s->display != CSS_DISPLAY_NONE;
        if (g_css_log > 0)
            fprintf(stderr, "[css] %d subtrees left out%s\n", ctx->pruned_n,
                    failed_has ? ", :has() rules changed: again" : failed ? ", one shown: again" : "");
        if (failed) { ctx->prune_failed = true; return false; }
    }
    _st_t = layout_now_ms();

    /* The used values every box starts from. */
    for (int i = 0; i < ctx->tree.count; i++) layout_box_start(&ctx->boxes[i]);

    /* Fonts: each box's family / weight / style to a registry slot, after
     * fetching the web fonts the page asks for (once; cached afterwards). */
    layout_resolve_fonts(ctx);

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
            if (!tn || tn->type != HTML_NODE_TEXT || !css_layout_node_text(ctx, tn)) continue;
            if (ctx->boxes[i].flags & LAYOUT_STYLE_SKIPPED) continue;
            unsigned char ws = ctx->boxes[i].s->white_space;
            if (ws == 2 || ws == 3) continue;
            const char *t = css_layout_node_text(ctx, tn);
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
                if (ch == '\n' && ws == 4) {
                    /* pre-line keeps segment breaks; spaces around them go. */
                    if (k > 0 && o[k - 1] == ' ') k--;
                    o[k++] = '\n';
                    in_space = true;
                } else if (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r' || ch == '\f') {
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
        g_lpar.enabled = job_thread_count() > 1 && !(pe && strcmp(pe, "0") == 0) && !ctx->serial;
        g_lpar.min_nodes = (ge && atoi(ge) > 0) ? atoi(ge) : 64;
        atomic_store(&g_lpar.regions, 0);
        atomic_store(&g_lpar.units, 0);
        atomic_store(&g_lpar.serial_regions, 0);
        g_lpar.has_float = (uint8_t *)calloc((size_t)n + 1, 1);
        g_lpar.subtree = (int *)malloc(((size_t)n + 1) * sizeof(int));
        for (int i = 0; i < n; i++) g_lpar.subtree[i] = 1;
        for (int pos = n - 1; pos >= 0; pos--) {
            int i = ctx->tree.preorder[pos];
            if (!(ctx->boxes[i].flags & LAYOUT_STYLE_SKIPPED) && ctx->boxes[i].s->float_side != 0) g_lpar.has_float[i] = 1;
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
    layout_flow_begin(ctx);
    job_graph_run(job_new(layout_flow_job, NULL, ctx, 0, 0));
    layout_flow_end(ctx);
    /* Translations (transform) shift boxes and their subtrees after layout;
     * preorder so nested translations accumulate. */
    for (int k = 0; k < ctx->tree.count; k++) {
        int i = ctx->tree.preorder[k];
        LayoutBox *b = &ctx->boxes[i];
        int par = ctx->tree.nodes[i].parent_idx;
        if (b->flags & LAYOUT_STYLE_SKIPPED) { b->opacity_eff = 0.0; continue; }
        b->opacity_eff = (par >= 0 ? ctx->boxes[par].opacity_eff : 1.0) *
                         (b->s->opacity_set ? b->s->opacity : 1.0);
        if (b->s->display == CSS_DISPLAY_NONE) continue;
        double dx = 0.0, dy = 0.0;
        if ((b->s->position == CSS_POSITION_RELATIVE || b->s->position == CSS_POSITION_STICKY) &&
            b->s->positioned_sides) {
            /* Relative offsets (§9.4.3) against the parent's content box;
             * sticky boxes are treated as relative at scroll position 0. */
            int par = ctx->tree.nodes[i].parent_idx;
            double pw = par >= 0 ? ctx->boxes[par].content_width : ctx->viewport_width;
            double ph = par >= 0 ? ctx->boxes[par].content_height : ctx->viewport_height;
            layout_resolve_insets(b, pw, ph);
            if (b->s->position == CSS_POSITION_RELATIVE) {
                if (b->s->positioned_sides & LAYOUT_SIDE_LEFT) dx = b->left;
                else if (b->s->positioned_sides & LAYOUT_SIDE_RIGHT) dx = -b->right;
                if (b->s->positioned_sides & LAYOUT_SIDE_TOP) dy = b->top;
                else if (b->s->positioned_sides & LAYOUT_SIDE_BOTTOM) dy = -b->bottom;
            }
        }
        if (b->s->has_translate) {
            dx += b->s->translate_x + b->s->translate_xpct * b->width;
            dy += b->s->translate_y + b->s->translate_ypct * b->height;
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

    STAGE("layout flags");

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

/* The declarations that apply to box idx's element (css_layout.h). */
static bool cascade_at(LayoutContext *ctx, int dom, const CssCustomProps *props, CssCascadeFn fn, void *ud);

bool css_layout_element_cascade(LayoutContext *ctx, int idx, CssCascadeFn fn, void *ud)
{
    if (!ctx || idx < 0 || idx >= ctx->tree.count || !ctx->doc) return false;
    return cascade_at(ctx, ctx->tree.nodes[idx].dom_node_idx, ctx->custom_props ? ctx->custom_props[idx] : NULL, fn, ud);
}

bool css_layout_dom_cascade(LayoutContext *ctx, int dom, int holder, CssCascadeFn fn, void *ud)
{
    if (!ctx || !ctx->doc || dom < 0) return false;
    const CssCustomProps *props = ctx->custom_props && holder >= 0 && holder < ctx->tree.count ? ctx->custom_props[holder] : NULL;
    return cascade_at(ctx, dom, props, fn, ud);
}

const char *css_layout_tag_display(const char *tag)
{
    switch (layout_default_display(tag ? tag : "")) {
    case CSS_DISPLAY_BLOCK: return "block";
    case CSS_DISPLAY_INLINE_BLOCK: return "inline-block";
    case CSS_DISPLAY_FLEX: return "flex";
    case CSS_DISPLAY_GRID: return "grid";
    case CSS_DISPLAY_NONE: return "none";
    case CSS_DISPLAY_LIST_ITEM: return "list-item";
    default: return "inline";
    }
}

static bool cascade_at(LayoutContext *ctx, int dom, const CssCustomProps *props, CssCascadeFn fn, void *ud)
{
    const CssStyleSet *set = styleset_cached(ctx->style_gen, ctx->viewport_width, ctx->viewport_height);
    if (!set) return false;
    HtmlNode *node = layout_node_dom(ctx, dom);
    if (!node || node->type != HTML_NODE_ELEMENT) return false;
    const CssStyleDoc *sd = css_styledoc_get(ctx->doc);
    CssStyleScratch *t = css_style_scratch_new(set);
    if (!t) return false;
    css_style_bloom_enter(t, sd, po_array_parent(&ctx->doc->array, dom));
    const CssAppliedDecl *applied = NULL;
    int count = css_style_match(set, sd, dom, t, &applied);
    InlineBlock inl[4];
    int ninl = 0;
    for (HtmlAttribute *attr = node->attributes; attr && ninl < 4; attr = attr->next) {
        if (strcasecmp(attr->name, "style") != 0) continue;
        inl[ninl].decls = css_parse_inline_style(attr->value, &inl[ninl].count);
        ninl++;
    }
    /* normal sheet declarations, inline, !important sheet, !important inline */
    for (int pass = 0; pass < 2; pass++) {
        bool imp = pass == 1;
        for (int d = 0; d < count; d++) {
            const CssDeclaration *dl = applied[d].decl;
            if (applied[d].important != imp || !dl->property || !dl->value || (dl->property[0] == '-' && dl->property[1] == '-')) continue;
            char *r = strstr(dl->value, "var(") ? css_var_resolve(dl->value, props) : NULL;
            fn(ud, dl->property, r ? r : dl->value, imp);
            free(r);
        }
        for (int b = 0; b < ninl; b++)
            for (int i = 0; i < inl[b].count; i++) {
                const CssDeclaration *dl = &inl[b].decls[i];
                if (dl->important != imp || !dl->property || !dl->value || (dl->property[0] == '-' && dl->property[1] == '-')) continue;
                char *r = strstr(dl->value, "var(") ? css_var_resolve(dl->value, props) : NULL;
                fn(ud, dl->property, r ? r : dl->value, imp);
                free(r);
            }
    }
    for (int b = 0; b < ninl; b++) css_declarations_free(inl[b].decls, inl[b].count);
    css_style_scratch_free(t);
    return true;
}

/* The computed value of custom property name on box idx's element. */
const char *css_layout_element_custom(LayoutContext *ctx, int idx, const char *name)
{
    if (!ctx || idx < 0 || idx >= ctx->tree.count || !ctx->custom_props) return NULL;
    return css_custom_props_get(ctx->custom_props[idx], name);
}

static uint64_t g_tree_kept, g_tree_built;

void css_layout_tree_stats(uint64_t *kept, uint64_t *built)
{
    if (kept) *kept = g_tree_kept;
    if (built) *built = g_tree_built;
}

/* The tree the last run on ctx left, for this run on doc: kept when it is
 * the same mirror document and no element's children changed since (its
 * nodes and their indices are then as they were; attribute changes only
 * restyle).  Everything else a tree build sets up starts over.  Elements
 * it left out stay out: a guess that comes out shown builds again
 * (prune_failed).  CYBER_LAYOUT_KEEP=0: build every time. */
/* A style pass over the whole kept tree: its style slots, custom
 * properties and their arena start over, as on a tree built afresh. */
static void layout_restyle_all(LayoutContext *ctx)
{
    for (int i = 0; i < ctx->tree.count; i++) ctx->boxes[i].s = ctx->slot[i];
    memset((void *)ctx->custom_props, 0, ((size_t)ctx->tree.count + 1) * sizeof(*ctx->custom_props));
    frame_arena_reset(&ctx->arena);
    if (ctx->slot_ver) memset(ctx->slot_ver, 0, (size_t)ctx->tree.count * sizeof(uint32_t));
}

/* The kept tree ctx built again for doc after elements' children changed:
 * the boxes it still has (the same store ids) keep their style slots (and
 * the styles in them, and the shared text styles they point to, in the
 * arena that moves over too), custom properties and the last store's
 * notes; the ones it lost give their slots back, and new ones take slots
 * with no note.  False (and ctx freed) when it cannot. */
static bool layout_tree_carry(LayoutContext *ctx, HtmlDocument *doc, bool prune)
{
    LayoutContext old = *ctx;
    int on = old.tree.count;
    bool ok = layout_tree_build_ex(ctx, doc, prune, true) && old.slot && old.ids;
    int n = ok ? ctx->tree.count : 0;
    uint32_t cap = 64;
    while (cap < 2u * (uint32_t)on + 2) cap <<= 1;
    int *tab = ok ? (int *)malloc(cap * sizeof(int)) : NULL;
    unsigned char *used = ok ? (unsigned char *)calloc((size_t)on + 1, 1) : NULL;
    if (ok) {
        ctx->slot = (LayoutStyle **)malloc(((size_t)n + 1) * sizeof(LayoutStyle *));
        ctx->style_ok = (unsigned char *)calloc((size_t)n + 1, 1);
        if (old.sc_entry) {
            ctx->sc_key = (uint64_t *)calloc((size_t)n + 1, sizeof(uint64_t));
            ctx->sc_entry = (int *)calloc((size_t)n + 1, sizeof(int));
            ctx->slot_ver = (uint32_t *)calloc((size_t)n + 1, sizeof(uint32_t));
        }
        if (old.has_bits) {
            ctx->has_bits = (signed char *)malloc(((size_t)n + 1) * 8);
            if (ctx->has_bits) memset(ctx->has_bits, -1, ((size_t)n + 1) * 8);
        }
        ok = tab && used && ctx->slot && ctx->style_ok && (!old.sc_entry || (ctx->sc_key && ctx->sc_entry && ctx->slot_ver)) &&
             (!old.has_bits || ctx->has_bits);
    }
    if (!ok) {
        free(tab);
        free(used);
        css_layout_tree_free(ctx);
        css_layout_tree_free(&old);
        return false;
    }
    /* What moves over whole. */
    ctx->slot_pool = old.slot_pool;          old.slot_pool = NULL;
    ctx->styles = old.styles;                old.styles = NULL;
    ctx->styles_n = old.styles_n;
    ctx->arena = old.arena;                  memset(&old.arena, 0, sizeof(old.arena));
    ctx->attr_ids = old.attr_ids;            old.attr_ids = NULL;
    ctx->attr_n = old.attr_n;
    ctx->attr_cap = old.attr_cap;
    ctx->child_ids = old.child_ids;          old.child_ids = NULL;
    ctx->child_n = old.child_n;
    ctx->child_cap = old.child_cap;
    ctx->sc_order = old.sc_order;
    ctx->sc_global = old.sc_global;
    ctx->has_bits_gen = old.has_bits_gen;
    ctx->flow = old.flow;                    old.flow = NULL;
    ctx->parked = old.parked;                old.parked = NULL;   /* (their slots are the pool's) */
    ctx->parked_n = old.parked_n;            old.parked_n = 0;
    ctx->parked_gen = old.parked_gen;
    ctx->flow_old = ctx->flow ? (int *)malloc(((size_t)n + 1) * sizeof(int)) : NULL;
    if (ctx->flow && !ctx->flow_old) { layout_flow_free(ctx->flow); ctx->flow = NULL; }
    /* Store id -> old index. */
    memset(tab, -1, cap * sizeof(int));
    for (int o = 0; o < on; o++) {
        uint32_t id = old.ids[o];
        if (!id) continue;
        uint32_t h = (id * 2654435761u) & (cap - 1);
        while (tab[h] >= 0 && old.ids[tab[h]] != id) h = (h + 1) & (cap - 1);
        if (tab[h] < 0) tab[h] = o;
    }
    for (int i = 0; i < n; i++) {
        uint32_t id = ctx->ids[i];
        int o = -1;
        if (id) {
            uint32_t h = (id * 2654435761u) & (cap - 1);
            while (tab[h] >= 0 && old.ids[tab[h]] != id) h = (h + 1) & (cap - 1);
            o = tab[h];
        }
        if (ctx->flow_old) ctx->flow_old[i] = o >= 0 && !used[o] ? o : -1;
        if (o >= 0 && !used[o]) {
            used[o] = 1;
            ctx->slot[i] = old.slot[o];
            ctx->boxes[i].s = old.boxes[o].s;
            ctx->custom_props[i] = old.custom_props[o];
            ctx->style_ok[i] = old.style_ok ? old.style_ok[o] : 0;
            if (ctx->sc_entry) {
                ctx->sc_key[i] = old.sc_key[o];
                ctx->sc_entry[i] = old.sc_entry[o];
                ctx->slot_ver[i] = old.slot_ver[o];
            }
            if (ctx->has_bits) memcpy(ctx->has_bits + (size_t)i * 8, old.has_bits + (size_t)o * 8, 8);
        } else {
            ctx->slot[i] = layout_slot_take(ctx);
            if (!ctx->slot[i]) {
                for (int j = i; j < n; j++) ctx->slot[j] = NULL;
                ok = false;
                break;
            }
            ctx->boxes[i].s = ctx->slot[i];
        }
    }
    for (int o = 0; ok && o < on; o++)
        if (!used[o]) layout_slot_give(ctx, old.slot[o]);
    free(tab);
    free(used);
    css_layout_tree_free(&old);
    if (!ok) { css_layout_tree_free(ctx); return false; }
    return true;
}

static bool layout_tree_keep(LayoutContext *ctx, HtmlDocument *doc, bool prune)
{
    static int keep_env = -1;
    if (keep_env < 0) {
        const char *e = cyber_getenv("CYBER_LAYOUT_KEEP");
        keep_env = !(e && e[0] == '0');
    }
    if (!ctx->tree_keep) return false;
    ctx->tree_keep = false;
    const HtmlMirrorChange *ch = NULL;
    size_t nch = 0;
    bool keep = keep_env && ctx->doc == doc && ctx->tree.count > 0 && (prune || !ctx->pruned_n) &&
                html_mirror_changes(doc, ctx->tree_seq, &ch, &nch);
    if (!keep) { css_layout_tree_free(ctx); return false; }
    /* (its pseudo-element boxes come again after the style pass) */
    layout_pseudo_detach(ctx);

    /* The elements whose attributes, and whose children, changed: where a
     * style pass on the kept tree starts (layout_apply_stylesheet). */
    ctx->attr_n = ctx->child_n = 0;
    bool children = false;
    for (size_t k = 0; k < nch; k++) {
        if (ch[k].kind == HTML_MIRROR_CHILDREN) children = true;
        if ((ch[k].kind != HTML_MIRROR_ATTRS && ch[k].kind != HTML_MIRROR_CHILDREN) || !ch[k].id) continue;
        bool attrs = ch[k].kind == HTML_MIRROR_ATTRS;
        uint32_t **ids = attrs ? &ctx->attr_ids : &ctx->child_ids;
        int *nn = attrs ? &ctx->attr_n : &ctx->child_n, *cap = attrs ? &ctx->attr_cap : &ctx->child_cap;
        if (*nn == *cap) {
            int nc = *cap ? 2 * *cap : 64;
            uint32_t *na = (uint32_t *)realloc(*ids, (size_t)nc * sizeof(uint32_t));
            if (!na) { css_layout_tree_free(ctx); return false; }
            *ids = na;
            *cap = nc;
        }
        (*ids)[(*nn)++] = ch[k].id;
    }
    /* Children changed: the tree is built again, keeping what its boxes
     * carry over. */
    if (children && !layout_tree_carry(ctx, doc, prune)) return false;
    /* (the :has() checks of the elements whose attributes changed are of
     * their old attributes) */
    for (int k = 0; ctx->has_bits && k < ctx->attr_n; k++) {
        int d = html_mirror_index_of(doc, ctx->attr_ids[k]);
        int li = d >= 0 && d < ctx->dom_map_n ? ctx->dom_map[d] : -1;
        if (li >= 0) memset(ctx->has_bits + (size_t)li * 8, -1, 8);
    }
    /* (The style slots, custom properties and the arena they live in stay
     * for the style pass: layout_restyle_all starts them over when it
     * restyles everything.) */
    ctx->tree_kept = true;
    ctx->float_count = 0;
    free(ctx->intrinsic_cache);
    ctx->intrinsic_cache = NULL;
    free(ctx->has_flags);
    ctx->has_flags = NULL;
    if (ctx->text_gen) {
        for (int i = 0; i < ctx->text_gen_cap; i++) free(ctx->text_gen[i]);
        free(ctx->text_gen);
        ctx->text_gen = NULL;
        ctx->text_gen_cap = 0;
    }
    ctx->style_gen = 0;
    ctx->style_pass = 0;
    ctx->style_skipped = 0;
    ctx->prune_failed = false;
    ctx->has_target_first = 0;
    return true;
}

/* The same geometry, but for rounding (a reused position is the recorded
 * one plus a shift). */
static bool layout_geom_close(const LayoutBox *a, const LayoutBox *b)
{
#define GEOM_NEAR(f) (a->f == b->f || fabs(a->f - b->f) <= 1e-6 * (1.0 + fabs(a->f)))
    return GEOM_NEAR(x) && GEOM_NEAR(y) && GEOM_NEAR(width) && GEOM_NEAR(height) &&
           GEOM_NEAR(margin_top) && GEOM_NEAR(margin_right) && GEOM_NEAR(margin_bottom) && GEOM_NEAR(margin_left) &&
           GEOM_NEAR(padding_top) && GEOM_NEAR(padding_right) && GEOM_NEAR(padding_bottom) && GEOM_NEAR(padding_left) &&
           GEOM_NEAR(content_width) && GEOM_NEAR(content_height) && GEOM_NEAR(baseline) &&
           GEOM_NEAR(wrap_first_w) && GEOM_NEAR(wrap_cont_w) && GEOM_NEAR(wrap_cont_x) && GEOM_NEAR(wrap_cont2_x) &&
           GEOM_NEAR(wrap_cont2_w) && a->wrap_cont2_line == b->wrap_cont2_line &&
           GEOM_NEAR(min_width) && GEOM_NEAR(max_width) && GEOM_NEAR(min_height) && GEOM_NEAR(max_height) &&
           GEOM_NEAR(top) && GEOM_NEAR(left) && GEOM_NEAR(right) && GEOM_NEAR(bottom) &&
           a->flags == b->flags && a->height_pct_unresolved == b->height_pct_unresolved &&
           a->text_lead_skip == b->text_lead_skip && GEOM_NEAR(opacity_eff);
#undef GEOM_NEAR
}

/* CYBER_LAYOUT_KEEP_VERIFY=1: after each layout on a kept tree, the same
 * layout on a tree built afresh (storing nothing in the style cache), and
 * every box compared. */
static void layout_keep_verify(LayoutContext *ctx, HtmlDocument *doc, CssStylesheet *sheet)
{
    static uint64_t layouts, bad_layouts;
    LayoutContext fresh;
    memset(&fresh, 0, sizeof(fresh));
    GC_SET(fresh.js_ctx, ctx->js_ctx);
    fresh.style_all = ctx->style_all;
    fresh.serial = ctx->serial;
    fresh.cache_readonly = true;
    uint64_t kept = g_tree_kept, built = g_tree_built;
    bool ok = css_layout_run(&fresh, doc, sheet, ctx->viewport_width, ctx->viewport_height);
    g_tree_kept = kept;
    g_tree_built = built;
    layouts++;
    int bad = 0, first = -1;
    /* (by document node: the two trees may leave out different hidden
     * subtrees; a box either leaves unstyled is not compared) */
    int dn_count = (int)po_array_count(&doc->array);
    int *at = ok ? (int *)malloc(((size_t)dn_count + 1) * sizeof(int)) : NULL;
    if (!at) bad = -1;
    else {
        for (int i = 0; i < dn_count; i++) at[i] = -1;
        for (int i = 0; i < fresh.tree.count; i++) {
            int d = fresh.tree.nodes[i].dom_node_idx;
            if (d >= 0 && d < dn_count) at[d] = i;
        }
        const size_t g0 = sizeof(LayoutStyle *);
        for (int i = 0; i < ctx->tree.count; i++) {
            int d = ctx->tree.nodes[i].dom_node_idx, j = d >= 0 && d < dn_count ? at[d] : -1;
            if (d >= LAYOUT_PSEUDO_DOM0) {
                /* A pseudo box: the fresh tree's of the same element and kind
                 * (its text: under that one). */
                int pe = i, txt = 0;
                HtmlNode *pn = layout_node_dom(ctx, d);
                if (pn && pn->type == HTML_NODE_TEXT) { pe = ctx->tree.nodes[i].parent_idx; txt = 1; }
                HtmlNode *en = layout_node_dom(ctx, ctx->tree.nodes[pe].dom_node_idx);
                int od = ctx->tree.nodes[ctx->tree.nodes[pe].parent_idx].dom_node_idx;
                int jo = od >= 0 && od < dn_count ? at[od] : -1;
                j = -1;
                for (int c = jo >= 0 ? fresh.tree.nodes[jo].first_child_idx : -1; c >= 0 && en; c = fresh.tree.nodes[c].next_sibling_idx) {
                    HtmlNode *cn = layout_node_dom(&fresh, fresh.tree.nodes[c].dom_node_idx);
                    if (fresh.tree.nodes[c].dom_node_idx >= LAYOUT_PSEUDO_DOM0 && cn && cn->tag_name == en->tag_name) {
                        j = txt ? fresh.tree.nodes[c].first_child_idx : c;
                        break;
                    }
                }
            }
            const LayoutBox *a = &ctx->boxes[i], *b = j >= 0 ? &fresh.boxes[j] : NULL;
            if ((a->flags & LAYOUT_STYLE_SKIPPED) || !b || (b->flags & LAYOUT_STYLE_SKIPPED)) continue;
            bool same_style = memcmp(a->s, b->s, sizeof(LayoutStyle)) == 0;
            bool same = same_style && (memcmp((const char *)a + g0, (const char *)b + g0, sizeof(LayoutBox) - g0) == 0 ||
                                       layout_geom_close(a, b));
            if (!same && bad++ == 0) {
                first = i;
                fprintf(stderr, "[keep-verify] box %d: %s; x %.2f/%.2f y %.2f/%.2f w %.2f/%.2f h %.2f/%.2f\n", i,
                        same_style ? "geometry" : "style", a->x, b->x, a->y, b->y, a->width, b->width, a->height, b->height);
            }
        }
        free(at);
    }
    if (bad) {
        bad_layouts++;
        HtmlNode *dn = first >= 0 ? layout_node_dom(ctx, ctx->tree.nodes[first].dom_node_idx) : NULL;
        fprintf(stderr, "[keep-verify] layout %llu: %d boxes differ (%d vs %d boxes; first %d <%s>)\n",
                (unsigned long long)layouts, bad, ctx->tree.count, fresh.tree.count, first,
                dn && dn->tag_name ? dn->tag_name : "?");
    }
    fprintf(stderr, "[keep-verify] %llu layouts on kept trees, %llu differ\n", (unsigned long long)layouts,
            (unsigned long long)bad_layouts);
    css_layout_tree_free(&fresh);
}

bool css_layout_run(LayoutContext *ctx, HtmlDocument *doc, CssStylesheet *sheet,
                    double viewport_width, double viewport_height)
{
    uint64_t seq = html_mirror_change_seq();
    bool kept_tree = false;
    double _run_t0 = layout_now_ms(), lph_tree_prev = 0.0;
    JSContextHandle saved_js_ctx = ctx->js_ctx;
    const char *saved_dump_path = ctx->dump_path;
    bool saved_style_all = ctx->style_all, saved_serial = ctx->serial, saved_readonly = ctx->cache_readonly;
    /* CSS_PRUNE=0: build trees in full. */
    static int prune_env = -1;
    if (prune_env < 0) { const char *e = getenv("CSS_PRUNE"); prune_env = !(e && strcmp(e, "0") == 0); }
    bool prune = prune_env && !saved_style_all;
    int prune_retries = 0;
again:
    kept_tree = layout_tree_keep(ctx, doc, prune);
    if (kept_tree) g_tree_kept++;
    else if (layout_tree_build(ctx, doc, prune)) g_tree_built++;
    else return false;
    GC_SET(ctx->js_ctx, saved_js_ctx);
    ctx->dump_path = saved_dump_path;
    ctx->style_all = saved_style_all;
    ctx->serial = saved_serial;
    ctx->cache_readonly = saved_readonly;
    ctx->viewport_width = viewport_width;
    ctx->viewport_height = viewport_height;
    g_unit_vw = viewport_width > 0.0 ? viewport_width : 1024.0;
    g_unit_vh = viewport_height > 0.0 ? viewport_height : 768.0;
    g_unit_rem = 16.0;
    /* Base URL for resolving relative stylesheet/image URLs: the URL of the
     * document's realm (a frame's page has its own), else the start URL. */
    layout_origin_base(ctx->base_url, sizeof(ctx->base_url));
    layout_realm_base(ctx);
    if (g_css_log < 0) { const char *e = getenv("CSS_LOG"); g_css_log = e ? atoi(e) : 0; }
    if (g_css_log > 0) fprintf(stderr, "[css] tree build + setup  %8.3f ms\n", layout_now_ms() - _run_t0);
    if (g_lph_on) lph_note("tree build + setup", layout_now_ms() - _run_t0 - lph_tree_prev);
    bool ok = css_layout_document(ctx, sheet);
    if (!ok && ctx->prune_failed && prune) {
        /* The attempt brought what the guesses depend on up to date: a guess
         * that came out shown is in the style cache now, and the :has()
         * rules are the new ones.  Built again, the other hidden subtrees
         * are still left out (and checked again); after two such tries, or
         * at once when the attempt stored nothing (cache_readonly), in full. */
        css_layout_tree_free(ctx);
        if (saved_readonly || ++prune_retries > 2) prune = false;
        /* (the retry's tree build is timed from here) */
        lph_tree_prev = layout_now_ms() - _run_t0;
        goto again;
    }
    if (!ok) css_layout_tree_free(ctx);
    else {
        ctx->tree_keep = true;
        ctx->tree_seq = seq;
        static int verify_env = -1;
        if (verify_env < 0) verify_env = cyber_getenv("CYBER_LAYOUT_KEEP_VERIFY") ? 1 : 0;
        if (verify_env && kept_tree) layout_keep_verify(ctx, doc, sheet);
    }
    if (g_lph_on) lph_flush(layout_now_ms() - _run_t0);
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

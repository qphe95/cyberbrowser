/*
 * Display List - Implementation
 */

#include "display_list.h"
#include "platform.h"
#include "url_utils.h"
#include "text_shaper.h"
#include "font_registry.h"
#include "url_whatwg.h"
#include "image_cache.h"
#include "svg_render.h"
#include "browser_api_impl_types.h"

/* The frame a <video> shows now (browser/media_source_api.c). */
bool media_video_frame_for_node(uint32_t node, const uint8_t **rgba, int *w, int *h,
                                uint64_t *frame_id);

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <math.h>

#define LOG_TAG "display_list"
#define LOG_ERROR(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)

void display_list_init(DisplayList *dl)
{
    if (!dl) return;
    dl->cmds = NULL;
    dl->count = 0;
    dl->capacity = 0;
    dl->masks = NULL;
    dl->mask_count = dl->mask_capacity = 0;
}

void display_list_free(DisplayList *dl)
{
    if (!dl) return;
    free(dl->cmds);
    dl->cmds = NULL;
    dl->count = 0;
    dl->capacity = 0;
    free(dl->masks);
    dl->masks = NULL;
    dl->mask_count = dl->mask_capacity = 0;
}

bool display_list_reserve(DisplayList *dl, int extra)
{
    if (!dl) return false;
    int needed = dl->count + extra;
    if (needed <= dl->capacity) return true;

    int new_capacity = dl->capacity ? dl->capacity * 2 : 64;
    while (new_capacity < needed) new_capacity *= 2;

    DisplayListCmd *new_cmds = (DisplayListCmd*)realloc(dl->cmds, new_capacity * sizeof(DisplayListCmd));
    if (!new_cmds) {
        LOG_ERROR("Out of memory growing display list");
        return false;
    }
    dl->cmds = new_cmds;
    dl->capacity = new_capacity;
    return true;
}

bool display_list_add_rect(DisplayList *dl, float x, float y, float w, float h,
                           float r, float g, float b, float a)
{
    if (!display_list_reserve(dl, 1)) return false;
    DisplayListCmd *cmd = &dl->cmds[dl->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = DL_RECT;
    cmd->x = x; cmd->y = y; cmd->w = w; cmd->h = h;
    cmd->r = r; cmd->g = g; cmd->b = b; cmd->a = a;
    return true;
}

bool display_list_add_border(DisplayList *dl, float x, float y, float w, float h,
                             float thickness, float r, float g, float b, float a)
{
    if (!display_list_reserve(dl, 1)) return false;
    DisplayListCmd *cmd = &dl->cmds[dl->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = DL_BORDER;
    cmd->x = x; cmd->y = y; cmd->w = w; cmd->h = h;
    cmd->r = r; cmd->g = g; cmd->b = b; cmd->a = a;
    cmd->u.border.thickness = thickness;
    return true;
}

bool display_list_add_glyph(DisplayList *dl, float x, float y, float w, float h,
                            float u0, float v0, float u1, float v1,
                            uint32_t glyph_index,
                            float r, float g, float b, float a)
{
    if (!display_list_reserve(dl, 1)) return false;
    DisplayListCmd *cmd = &dl->cmds[dl->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = DL_GLYPH;
    cmd->x = x; cmd->y = y; cmd->w = w; cmd->h = h;
    cmd->r = r; cmd->g = g; cmd->b = b; cmd->a = a;
    cmd->u.glyph.u0 = u0; cmd->u.glyph.v0 = v0;
    cmd->u.glyph.u1 = u1; cmd->u.glyph.v1 = v1;
    cmd->u.glyph.glyph_index = glyph_index;
    return true;
}

bool display_list_add_image(DisplayList *dl, float x, float y, float w, float h,
                            int image_handle,
                            float u0, float v0, float u1, float v1)
{
    if (!display_list_reserve(dl, 1)) return false;
    DisplayListCmd *cmd = &dl->cmds[dl->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = DL_IMAGE;
    cmd->x = x; cmd->y = y; cmd->w = w; cmd->h = h;
    cmd->u.image.image_handle = image_handle;
    cmd->u.image.u0 = u0; cmd->u.image.v0 = v0;
    cmd->u.image.u1 = u1; cmd->u.image.v1 = v1;
    return true;
}

static ImageCache *g_image_cache = NULL;

static void on_async_image_loaded(const char *url, void *user_data);

void display_list_set_image_cache(ImageCache *cache)
{
    g_image_cache = cache;
    /* Any image becoming sized (layout) or decoded (paint) re-renders. */
    if (cache) image_cache_set_notify(cache, on_async_image_loaded, NULL);
}

ImageCache *display_list_get_image_cache(void)
{
    return g_image_cache;
}

bool display_list_set_default_font(const char *ttf_path, float size_pixels)
{
    (void)size_pixels;
    return ttf_path && font_registry_set_default_file(ttf_path);
}

struct TextShaper *display_list_get_default_font(void)
{
    return font_registry_slot(font_registry_default_slot());
}

struct TextShaper *display_list_get_font(int slot)
{
    TextShaper *s = font_registry_slot(slot);
    return s ? s : display_list_get_default_font();
}

int display_list_resolve_font_slot(const char *font_family, int font_weight,
                                   int font_italic)
{
    return font_registry_resolve(font_family, font_weight, font_italic != 0);
}

static bool text_is_whitespace(const char *s)
{
    for (; *s; s++) {
        if (!isspace((unsigned char)*s)) return false;
    }
    return true;
}

static bool node_has_class(HtmlNode *node, const char *needle)
{
    if (!node || node->type != HTML_NODE_ELEMENT || !needle) return false;
    size_t needle_len = strlen(needle);
    for (HtmlAttribute *a = node->attributes; a; a = a->next) {
        if (strcasecmp(a->name, "class") == 0 && a->value[0]) {
            const char *p = a->value;
            size_t len = strlen(p);
            for (size_t i = 0; i < len; ) {
                while (i < len && isspace((unsigned char)p[i])) i++;
                size_t start = i;
                while (i < len && !isspace((unsigned char)p[i])) i++;
                if (i - start == needle_len && strncasecmp(p + start, needle, needle_len) == 0) return true;
            }
        }
    }
    return false;
}

static bool node_class_contains_any(HtmlNode *node, const char **needles)
{
    if (!node || node->type != HTML_NODE_ELEMENT || !needles) return false;
    for (HtmlAttribute *a = node->attributes; a; a = a->next) {
        if (strcasecmp(a->name, "class") != 0 || !a->value[0]) continue;
        const char *cls = a->value;
        for (const char **n = needles; *n; n++) {
            if (strstr(cls, *n) != NULL) return true;
        }
    }
    return false;
}

static bool node_or_ancestor_has_class(LayoutContext *ctx, int node_idx,
                                        const char *needle)
{
    if (node_idx < 0 || node_idx >= ctx->tree.count) return false;
    int idx = node_idx;
    while (idx >= 0) {
        HtmlNode *node = (HtmlNode*)po_array_payload(&ctx->doc->array,
                                                      ctx->tree.nodes[idx].dom_node_idx);
        if (node_has_class(node, needle)) return true;
        idx = ctx->tree.nodes[idx].parent_idx;
    }
    return false;
}

static bool node_has_hidden_class(LayoutContext *ctx, int node_idx)
{
    /* Treat any node inside a hidden subtree as hidden. Some page skeletons
     * carry class "hidden" on their root while descendants do not, so we must
     * walk up the layout tree. */
    if (node_or_ancestor_has_class(ctx, node_idx, "hidden")) return true;
    return false;
}

/* True when the node or any ancestor is display:none or visibility:hidden.
 * Emissions that bypass the size filters (text glyphs, <img>, background
 * images) must check this so hidden subtrees (scripts, off-screen menus,
 * head content) do not leak onto the page at the origin. */
static bool box_or_ancestor_hidden(LayoutContext *ctx, int node_idx)
{
    int idx = node_idx;
    while (idx >= 0) {
        LayoutBox *b = &ctx->boxes[idx];
        if (b->display == CSS_DISPLAY_NONE || b->visibility == CSS_VISIBILITY_HIDDEN) return true;
        idx = ctx->tree.nodes[idx].parent_idx;
    }
    return false;
}

static const char* node_attribute_value(HtmlNode *node, const char *name)
{
    if (!node || node->type != HTML_NODE_ELEMENT) return NULL;
    for (HtmlAttribute *a = node->attributes; a; a = a->next) {
        if (strcasecmp(a->name, name) == 0) return a->value;
    }
    return NULL;
}

/* Pick the best candidate from an <img srcset> for the box's CSS width.
 * Width descriptors (300w): smallest candidate >= css width, else the
 * largest.  Density descriptors (1.5x): smallest >= 1x, else the largest.
 * Returns a malloc'd URL string (caller frees) or NULL. */
static char *srcset_pick(const char *srcset, float css_width)
{
    if (!srcset || !srcset[0]) return NULL;
    char *copy = strdup(srcset);
    if (!copy) return NULL;

    char *best_w_ge = NULL;  double best_w_ge_v = 0.0;  /* smallest w >= target */
    char *best_any = NULL;   double best_any_v = 0.0;   /* largest overall */
    char *best_x_ge = NULL;  double best_x_ge_v = 0.0;  /* smallest x >= 1 */
    char *best_x = NULL;     double best_x_v = 0.0;     /* largest x */

    char *save = NULL;
    for (char *cand = strtok_r(copy, ",", &save); cand; cand = strtok_r(NULL, ",", &save)) {
        while (isspace((unsigned char)*cand)) cand++;
        if (!cand[0]) continue;
        /* Split URL from descriptor at the first space. */
        char *desc = NULL;
        for (char *q = cand; *q; q++) {
            if (isspace((unsigned char)*q)) { *q = '\0'; desc = q + 1; break; }
        }
        double w = -1.0, x = 1.0;
        if (desc) {
            while (isspace((unsigned char)*desc)) desc++;
            char *end = NULL;
            double v = strtod(desc, &end);
            if (end && end != desc) {
                if (*end == 'w') { w = v; x = -1.0; }
                else if (*end == 'x') x = v;
            }
        }
        if (w > 0.0) {
            if (css_width > 0.0f && w >= css_width &&
                (best_w_ge == NULL || w < best_w_ge_v)) {
                free(best_w_ge); best_w_ge = strdup(cand); best_w_ge_v = w;
            }
            if (best_any == NULL || w > best_any_v) {
                free(best_any); best_any = strdup(cand); best_any_v = w;
            }
        } else {
            if (x >= 1.0 && (best_x_ge == NULL || x < best_x_ge_v)) {
                free(best_x_ge); best_x_ge = strdup(cand); best_x_ge_v = x;
            }
            if (best_x == NULL || x > best_x_v) {
                free(best_x); best_x = strdup(cand); best_x_v = x;
            }
        }
    }

    char *out = NULL;
    if (best_w_ge || best_any) out = best_w_ge ? best_w_ge : best_any;
    else out = best_x_ge ? best_x_ge : best_x;
    if (best_w_ge && out != best_w_ge) free(best_w_ge);
    if (best_any && out != best_any) free(best_any);
    if (best_x_ge && out != best_x_ge) free(best_x_ge);
    if (best_x && out != best_x) free(best_x);
    free(copy);
    return out;
}

static char* dl_resolve_url(const char *base_url, const char *href)
{
    if (!href || !href[0]) return NULL;
    if (url_has_scheme(href))
        return strdup(href);
    if (strncmp(href, "//", 2) == 0) {
        char buf[2048];
        snprintf(buf, sizeof(buf), "https:%s", href);
        return strdup(buf);
    }
    if (href[0] == '/')
        return strdup(href);
    if (!base_url || !base_url[0])
        return strdup(href);

    const char *base = base_url;
    if (href[0] == '/') {
        char buf[2048];
        if (base[strlen(base) - 1] == '/') {
            snprintf(buf, sizeof(buf), "%s%s", base, href + 1);
        } else {
            snprintf(buf, sizeof(buf), "%s%s", base, href);
        }
        return strdup(buf);
    }
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

static void emit_image(DisplayList *dl, float x, float y, float w, float h, int handle)
{
    if (handle < 0) return;
    int img_w = 0, img_h = 0, ch = 0;
    uint8_t *pix = NULL;
    if (!image_cache_get(g_image_cache, handle, &img_w, &img_h, &ch, &pix)) return;
    if (w <= 0.0f) w = (float)img_w;
    if (h <= 0.0f) h = (float)img_h;
    if (w <= 0.0f || h <= 0.0f) return;
    display_list_add_image(dl, x, y, w, h, handle, 0.0f, 0.0f, 1.0f, 1.0f);
}

static void on_async_image_loaded(const char *url, void *user_data)
{
    (void)url;
    (void)user_data;
    dom_request_relayout();
}

/* Paint an <img> (or other replaced image) into its content box once its
 * pixels are decoded; the decode is requested at the painted size.  While
 * loading nothing is painted (like browsers); the cache notification
 * triggers a re-render when the pixels arrive.  object-fit maps the image
 * into the box: fill (stretch), contain, cover (crop), none, scale-down. */
static void emit_image_async(DisplayList *dl, const LayoutBox *box, const char *url, float op)
{
    if (!g_image_cache || !url || !url[0]) return;
    float cx = (float)(box->x + box->border_left + box->padding_left);
    float cy = (float)(box->y + box->border_top + box->padding_top);
    float cw = (float)(box->width - box->border_left - box->border_right - box->padding_left - box->padding_right);
    float ch = (float)(box->height - box->border_top - box->border_bottom - box->padding_top - box->padding_bottom);
    if (cw <= 0.0f || ch <= 0.0f) return;
    int handle = image_cache_request(g_image_cache, url, (int)ceilf(cw), (int)ceilf(ch));
    if (handle < 0) return;
    int img_w = 0, img_h = 0, chn = 0;
    uint8_t *pix = NULL;
    if (!image_cache_get(g_image_cache, handle, &img_w, &img_h, &chn, &pix)) return;
    if (img_w <= 0 || img_h <= 0) return;
    int nw = img_w, nh = img_h;
    image_cache_get_natural(g_image_cache, handle, &nw, &nh);
    if (nw <= 0 || nh <= 0) { nw = img_w; nh = img_h; }

    float dx = cx, dy = cy, dw = cw, dh = ch;
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    int fit = box->object_fit;
    if (fit == 4) {   /* scale-down: none or contain, whichever is smaller */
        fit = ((float)nw <= cw && (float)nh <= ch) ? 3 : 1;
    }
    if (fit == 1 || fit == 3) {
        float s = fit == 1 ? fminf(cw / (float)nw, ch / (float)nh) : 1.0f;
        dw = (float)nw * s;
        dh = (float)nh * s;
        dx = cx + (cw - dw) * 0.5f;     /* object-position: 50% 50% */
        dy = cy + (ch - dh) * 0.5f;
        if (fit == 3 && (dw > cw || dh > ch)) {
            /* none, larger than the box: crop to the box. */
            float vx0 = fmaxf(dx, cx), vy0 = fmaxf(dy, cy);
            float vx1 = fminf(dx + dw, cx + cw), vy1 = fminf(dy + dh, cy + ch);
            u0 = (vx0 - dx) / dw; u1 = (vx1 - dx) / dw;
            v0 = (vy0 - dy) / dh; v1 = (vy1 - dy) / dh;
            dx = vx0; dy = vy0; dw = vx1 - vx0; dh = vy1 - vy0;
        }
    } else if (fit == 2) {
        /* cover: scale to fill, crop the overflow symmetrically. */
        float s = fmaxf(cw / (float)nw, ch / (float)nh);
        float sw = (float)nw * s, sh = (float)nh * s;
        u0 = ((sw - cw) * 0.5f) / sw;
        u1 = 1.0f - u0;
        v0 = ((sh - ch) * 0.5f) / sh;
        v1 = 1.0f - v0;
    }
    if (dw <= 0.0f || dh <= 0.0f) return;
    int from = dl->count;
    display_list_add_image(dl, dx, dy, dw, dh, handle, u0, v0, u1, v1);
    if (op < 0.999f && dl->count > from) dl->cmds[from].a = op;
}

/* ---- inline <svg> ---- */

typedef struct { char *s; size_t n, cap; } SvgBuf;

static void sb_put(SvgBuf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->n + n + 1) nc *= 2;
        char *t = (char *)realloc(b->s, nc);
        if (!t) return;
        b->s = t;
        b->cap = nc;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
}
static void sb_puts(SvgBuf *b, const char *s) { sb_put(b, s, strlen(s)); }

static void sb_escaped(SvgBuf *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
        case '&': sb_puts(b, "&amp;"); break;
        case '<': sb_puts(b, "&lt;"); break;
        case '>': sb_puts(b, "&gt;"); break;
        case '"': sb_puts(b, "&quot;"); break;
        default: sb_put(b, s + i, 1);
        }
    }
}

/* SVG names are case-sensitive; the HTML parser lower-cased them. */
static const char *svg_case(const char *lower)
{
    static const char *const names[] = {
        "viewBox", "preserveAspectRatio", "gradientUnits", "gradientTransform", "spreadMethod",
        "patternUnits", "patternContentUnits", "patternTransform", "clipPathUnits", "maskUnits",
        "maskContentUnits", "markerUnits", "markerWidth", "markerHeight", "refX", "refY",
        "stdDeviation", "textLength", "lengthAdjust", "startOffset", "pathLength",
        "linearGradient", "radialGradient", "clipPath", "foreignObject", "textPath",
        "feGaussianBlur", "feOffset", "feBlend", "feColorMatrix", "feComposite", "feFlood",
        "feMerge", "feMergeNode", NULL
    };
    for (int i = 0; names[i]; i++)
        if (strcasecmp(lower, names[i]) == 0) return names[i];
    return lower;
}

static void svg_serialize(PreOrderCompactionArray *arr, int idx, SvgBuf *b, bool root,
                          int w, int h, const char *root_style)
{
    HtmlNode *n = (HtmlNode *)po_array_payload(arr, idx);
    if (!n) return;
    if (n->type == HTML_NODE_TEXT) {
        if (n->text_content) sb_escaped(b, n->text_content, n->text_len);
        return;
    }
    if (n->type != HTML_NODE_ELEMENT || !n->tag_name) return;
    const char *tag = svg_case(n->tag_name);
    sb_puts(b, "<");
    sb_puts(b, tag);
    bool has_xmlns = false;
    for (HtmlAttribute *a = n->attributes; a; a = a->next) {
        if (!a->name) continue;
        if (root && (strcasecmp(a->name, "width") == 0 || strcasecmp(a->name, "height") == 0 ||
                     strcasecmp(a->name, "style") == 0)) continue;
        if (strcasecmp(a->name, "xmlns") == 0) has_xmlns = true;
        sb_puts(b, " ");
        sb_puts(b, svg_case(a->name));
        sb_puts(b, "=\"");
        if (a->value) sb_escaped(b, a->value, strlen(a->value));
        sb_puts(b, "\"");
    }
    if (root) {
        char t[64];
        if (!has_xmlns) sb_puts(b, " xmlns=\"http://www.w3.org/2000/svg\"");
        snprintf(t, sizeof(t), " width=\"%d\" height=\"%d\"", w, h);
        sb_puts(b, t);
        if (root_style && root_style[0]) {
            sb_puts(b, " style=\"");
            sb_escaped(b, root_style, strlen(root_style));
            sb_puts(b, "\"");
        }
    }
    sb_puts(b, ">");
    for (int c = po_array_first_child(arr, idx); c >= 0; c = po_array_next_sibling(arr, c))
        svg_serialize(arr, c, b, false, 0, 0, NULL);
    sb_puts(b, "</");
    sb_puts(b, tag);
    sb_puts(b, ">");
}

static uint64_t svg_hash(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ull; }
    return h;
}

static uint8_t to_byte(double v)
{
    v = v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
    return (uint8_t)lround(v * 255.0);
}

/* Paint an outermost inline <svg> into its content box: its subtree is
 * serialized as a standalone document (with the CSS fill it inherits, and
 * the text color for currentColor), rasterized at the painted size, and
 * kept in the image cache under a key naming the markup, size and colors. */
static void emit_inline_svg(LayoutContext *ctx, DisplayList *dl, const LayoutBox *box, int dom_idx, float op)
{
    int cw = (int)lround(box->width - box->border_left - box->border_right - box->padding_left - box->padding_right);
    int ch = (int)lround(box->height - box->border_top - box->border_bottom - box->padding_top - box->padding_bottom);
    if (cw <= 0 || ch <= 0 || cw > 4096 || ch > 4096) return;
    char style[96] = "";
    if (box->fill_kind == 1) snprintf(style, sizeof(style), "fill:currentColor");
    else if (box->fill_kind == 3) snprintf(style, sizeof(style), "fill:none");
    else if (box->fill_kind == 2)
        snprintf(style, sizeof(style), "fill:#%02x%02x%02x;fill-opacity:%.3f", to_byte(box->fill_r),
                 to_byte(box->fill_g), to_byte(box->fill_b), box->fill_a);
    uint32_t color = (uint32_t)to_byte(box->color_r) << 24 | (uint32_t)to_byte(box->color_g) << 16 |
                     (uint32_t)to_byte(box->color_b) << 8 | to_byte(box->color_a);

    SvgBuf b = { 0 };
    svg_serialize(&ctx->doc->array, dom_idx, &b, true, cw, ch, style);
    if (!b.s) return;
    char key[96];
    snprintf(key, sizeof(key), "cyber-svg:%016llx:%08x", (unsigned long long)svg_hash(b.s, b.n), color);
    if (image_cache_status(g_image_cache, key) != 1) {
        uint8_t *px = NULL;
        int pw = 0, ph = 0;
        if (svg_render_rgba(b.s, b.n, cw, ch, color, &px, &pw, &ph)) {
            image_cache_put_rgba(g_image_cache, key, pw, ph, px);
            free(px);
        }
    }
    free(b.s);
    LayoutBox fill_box = *box;
    fill_box.object_fit = 0;
    emit_image_async(dl, &fill_box, key, op);
}

static bool inside_svg(PreOrderCompactionArray *arr, int dom_idx)
{
    for (int p = po_array_parent(arr, dom_idx); p >= 0; p = po_array_parent(arr, p)) {
        HtmlNode *n = (HtmlNode *)po_array_payload(arr, p);
        if (n && n->type == HTML_NODE_ELEMENT && n->tag_name && strcasecmp(n->tag_name, "svg") == 0) return true;
    }
    return false;
}

char *display_list_image_source(const char *base_url, HtmlNode *node, float css_width)
{
    if (!node || node->type != HTML_NODE_ELEMENT) return NULL;
    const char *src = node_attribute_value(node, "src");
    /* srcset, when present, overrides src with the candidate that best
     * matches the laid-out width (or the 1x candidate). */
    const char *srcset = node_attribute_value(node, "srcset");
    char *picked = srcset ? srcset_pick(srcset, css_width) : NULL;
    if (picked) src = picked;
    char *url = NULL;
    if (src && src[0]) {
        /* Resolve against the document URL (relative paths need its
         * directory, not just the origin layout carries). */
        extern const char *g_cyber_start_url;
        const char *doc_url = (g_cyber_start_url && g_cyber_start_url[0]) ? g_cyber_start_url : base_url;
        url = (doc_url && doc_url[0]) ? wurl_resolve(doc_url, src) : NULL;
        if (!url) url = dl_resolve_url(base_url, src);
    }
    free(picked);
    return url;
}

char *display_list_pick_srcset(const char *srcset, float css_width)
{
    return srcset_pick(srcset, css_width);
}

/* Emit underline / line-through rects for the glyph commands emitted since
 * `from_cmd`.  Glyphs are grouped into lines by y proximity so wrapped runs
 * get one decoration segment per line. */
static void emit_text_decorations(DisplayList *dl, int from_cmd, double fs,
                                  unsigned char deco, float r, float g, float b, float a)
{
    if (!deco || from_cmd >= dl->count) return;
    float tol = (float)(fs * 0.5);
    if (tol < 1.0f) tol = 1.0f;
    /* Collect line extents first (add_rect may realloc dl->cmds). */
    typedef struct { float y, x0, x1; } DecoLine;
    int cap = 16, n = 0;
    DecoLine *lines = (DecoLine *)malloc((size_t)cap * sizeof(DecoLine));
    if (!lines) return;
    for (int i = from_cmd; i < dl->count; i++) {
        DisplayListCmd *c = &dl->cmds[i];
        if (c->type != DL_GLYPH) continue;
        float ly = c->y;
        int found = -1;
        for (int k = 0; k < n; k++) {
            if (fabsf(lines[k].y - ly) <= tol) { found = k; break; }
        }
        if (found < 0) {
            if (n >= cap) {
                cap *= 2;
                DecoLine *nl = (DecoLine *)realloc(lines, (size_t)cap * sizeof(DecoLine));
                if (!nl) { free(lines); return; }
                lines = nl;
            }
            found = n++;
            lines[found].y = ly;
            lines[found].x0 = c->x;
            lines[found].x1 = c->x + c->w;
        } else {
            if (c->x < lines[found].x0) lines[found].x0 = c->x;
            if (c->x + c->w > lines[found].x1) lines[found].x1 = c->x + c->w;
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        float yoff, thick;
        if (pass == 0) {
            if (!(deco & 1)) continue; /* underline */
            yoff = (float)(fs * 0.92);
        } else {
            if (!(deco & 2)) continue; /* line-through */
            yoff = (float)(fs * 0.32);
        }
        thick = (float)(fs / 14.0);
        if (thick < 1.0f) thick = 1.0f;
        for (int k = 0; k < n; k++) {
            float w = lines[k].x1 - lines[k].x0;
            if (w <= 0.0f) continue;
            display_list_add_rect(dl, lines[k].x0, lines[k].y + yoff, w, thick,
                                  r, g, b, a);
        }
    }
    free(lines);
}

/* Return true if the document is in a dark theme.
 * This is used to make default-black text visible on dark backgrounds. */
static bool document_is_dark_mode(LayoutContext *ctx)
{
    if (!ctx || !ctx->doc) return false;
    for (size_t i = 0; i < ctx->doc->array.count; i++) {
        HtmlNode *node = (HtmlNode *)po_array_payload(&ctx->doc->array, i);
        if (!node || node->type != HTML_NODE_ELEMENT) continue;
        if (strcasecmp(node->tag_name, "html") == 0 ||
            strcasecmp(node->tag_name, "body") == 0) {
            /* Only the exact `dark` attribute marks YouTube's dark theme.  The
             * legacy `darker-dark-theme` attribute is present on the light
             * theme as well (deprecated), so it must not be used here. */
            if (node_attribute_value(node, "dark") != NULL) return true;
            static const char *dark_needles[] = {"dark-mode", "theme-dark", NULL};
            if (node_class_contains_any(node, dark_needles)) return true;
        }
    }
    return false;
}

/* Clip the commands [from, dl->count) to rect c (x0,y0,x1,y1): quads are
 * cropped (texture coordinates follow), fully clipped ones collapse. */
static void dl_clip_range(DisplayList *dl, int from, const float c[4])
{
    for (int k = from; k < dl->count; k++) {
        DisplayListCmd *cmd = &dl->cmds[k];
        float x0 = cmd->x, y0 = cmd->y, x1 = cmd->x + cmd->w, y1 = cmd->y + cmd->h;
        if (x0 >= c[0] && y0 >= c[1] && x1 <= c[2] && y1 <= c[3]) continue;
        float nx0 = x0 > c[0] ? x0 : c[0], ny0 = y0 > c[1] ? y0 : c[1];
        float nx1 = x1 < c[2] ? x1 : c[2], ny1 = y1 < c[3] ? y1 : c[3];
        if (nx1 <= nx0 || ny1 <= ny0) {
            cmd->w = 0.0f; cmd->h = 0.0f; cmd->a = 0.0f;
            if (cmd->type == DL_BORDER) { cmd->u.border.w = 0.0f; cmd->u.border.h = 0.0f; }
            continue;
        }
        float *u0 = NULL, *v0 = NULL, *u1 = NULL, *v1 = NULL;
        if (cmd->type == DL_GLYPH) { u0 = &cmd->u.glyph.u0; v0 = &cmd->u.glyph.v0; u1 = &cmd->u.glyph.u1; v1 = &cmd->u.glyph.v1; }
        else if (cmd->type == DL_IMAGE) { u0 = &cmd->u.image.u0; v0 = &cmd->u.image.v0; u1 = &cmd->u.image.u1; v1 = &cmd->u.image.v1; }
        else if (cmd->type == DL_BORDER) continue;   /* outlines: keep whole */
        if (u0 && cmd->w > 0.0f && cmd->h > 0.0f) {
            float du = *u1 - *u0, dv = *v1 - *v0;
            float nu0 = *u0 + du * (nx0 - x0) / cmd->w, nu1 = *u0 + du * (nx1 - x0) / cmd->w;
            float nv0 = *v0 + dv * (ny0 - y0) / cmd->h, nv1 = *v0 + dv * (ny1 - y0) / cmd->h;
            *u0 = nu0; *u1 = nu1; *v0 = nv0; *v1 = nv1;
        }
        cmd->x = nx0; cmd->y = ny0; cmd->w = nx1 - nx0; cmd->h = ny1 - ny0;
    }
}

/* ---- CSS masks ---- */

static int dl_add_mask(DisplayList *dl, const DisplayMask *m)
{
    if (dl->mask_count == dl->mask_capacity) {
        int nc = dl->mask_capacity ? dl->mask_capacity * 2 : 16;
        DisplayMask *nm = (DisplayMask *)realloc(dl->masks, (size_t)nc * sizeof(*nm));
        if (!nm) return -1;
        dl->masks = nm;
        dl->mask_capacity = nc;
    }
    dl->masks[dl->mask_count] = *m;
    return dl->mask_count++;
}

static bool dl_add_mask_cmd(DisplayList *dl, DisplayListCmdType type, int mask_index, const DisplayMask *m)
{
    if (!display_list_reserve(dl, 1)) return false;
    DisplayListCmd *cmd = &dl->cmds[dl->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = type;
    cmd->x = m->x; cmd->y = m->y; cmd->w = m->w; cmd->h = m->h;
    cmd->a = 1.0f;
    cmd->u.mask_index = mask_index;
    return true;
}

/* Case-insensitive substring test over the first n bytes of s. */
static bool memmem_ci(const char *s, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; i++)
        if (strncasecmp(s + i, needle, m) == 0) return true;
    return false;
}

/* Alpha of a CSS color token (masks use only alpha). */
static float mask_color_alpha(const char *s, size_t n)
{
    while (n && isspace((unsigned char)*s)) { s++; n--; }
    if (n >= 11 && strncasecmp(s, "transparent", 11) == 0) return 0.0f;
    if (n >= 5 && strncasecmp(s, "rgba(", 5) == 0) {
        const char *last = NULL;
        for (const char *q = s; q < s + n; q++) if (*q == ',' || *q == '/') last = q;
        if (last) {
            float a = (float)atof(last + 1);
            for (const char *q = last + 1; q < s + n && *q != ')'; q++) if (*q == '%') { a /= 100.0f; break; }
            return a;
        }
        return 1.0f;
    }
    if (n >= 4 && strncasecmp(s, "rgb(", 4) == 0) {
        for (const char *q = s; q < s + n; q++) if (*q == '/') return (float)atof(q + 1);
        return 1.0f;
    }
    if (s[0] == '#') {
        size_t hl = 0;
        while (hl + 1 < n && isxdigit((unsigned char)s[hl + 1])) hl++;
        unsigned v = 0;
        if (hl == 8) { sscanf(s + 7, "%2x", &v); return (float)v / 255.0f; }
        if (hl == 4) { sscanf(s + 4, "%1x", &v); return (float)v / 15.0f; }
    }
    return 1.0f;
}

/* linear-gradient(<angle> | to <side>, <color> [<pos>], ...) into m's angle
 * and alpha stops (px positions over the gradient line of a w x h box). */
static bool parse_mask_gradient(const char *g, DisplayMask *m, float w, float h)
{
    const char *p = strchr(g, '(');
    if (!p) return false;
    p++;
    m->angle_deg = 180.0f;
    m->nstops = 0;
    float pos[DL_MASK_STOPS];
    bool has_pos[DL_MASK_STOPS];
    bool first = true;
    while (*p && *p != ')' && m->nstops < DL_MASK_STOPS) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        const char *st = p;
        int depth = 0;
        while (*p && (depth > 0 || (*p != ',' && *p != ')'))) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            p++;
        }
        size_t n = (size_t)(p - st);
        while (n && isspace((unsigned char)st[n - 1])) n--;
        if (!n) continue;
        if (first) {
            first = false;
            if (n > 3 && strncasecmp(st, "to ", 3) == 0) {
                bool r = memmem_ci(st, n, "right"), l = memmem_ci(st, n, "left");
                bool t = memmem_ci(st, n, "top"), b = memmem_ci(st, n, "bottom");
                float ax = r ? 1.0f : l ? -1.0f : 0.0f, ay = b ? 1.0f : t ? -1.0f : 0.0f;
                m->angle_deg = (float)(atan2(ax, -ay) * 180.0 / M_PI);
                if ((r || l) && (t || b) && w > 0 && h > 0)   /* corner: perpendicular to the diagonal */
                    m->angle_deg = (float)(atan2(ax * h, -ay * w) * 180.0 / M_PI);
                continue;
            }
            char *end = NULL;
            double v = strtod(st, &end);
            if (end && end > st && end < st + n) {
                if (strncasecmp(end, "deg", 3) == 0) { m->angle_deg = (float)v; continue; }
                if (strncasecmp(end, "turn", 4) == 0) { m->angle_deg = (float)(v * 360.0); continue; }
                if (strncasecmp(end, "rad", 3) == 0) { m->angle_deg = (float)(v * 180.0 / M_PI); continue; }
            }
        }
        /* color [position]: the position is the last space-separated token
         * outside parentheses. */
        const char *cut = NULL;
        depth = 0;
        for (const char *q = st; q < st + n; q++) {
            if (*q == '(') depth++;
            else if (*q == ')') depth--;
            else if (depth == 0 && isspace((unsigned char)*q)) cut = q;
        }
        int k = m->nstops++;
        has_pos[k] = false;
        pos[k] = 0.0f;
        m->stop_alpha[k] = mask_color_alpha(st, cut ? (size_t)(cut - st) : n);
        if (cut) {
            char tok[64];
            size_t tl = (size_t)(st + n - (cut + 1));
            if (tl >= sizeof(tok)) tl = sizeof(tok) - 1;
            memcpy(tok, cut + 1, tl);
            tok[tl] = '\0';
            char *end = NULL;
            double v = strtod(tok, &end);
            if (end != tok) {
                has_pos[k] = true;
                double rad = m->angle_deg * M_PI / 180.0;
                double line = fabs(w * sin(rad)) + fabs(h * cos(rad));
                pos[k] = (float)(*end == '%' ? v / 100.0 : (line > 0 ? v / line : 0.0));
            }
        }
    }
    if (m->nstops < 1) return false;
    /* Missing positions: first 0, last 1, others spread between neighbours;
     * positions never decrease. */
    if (!has_pos[0]) { pos[0] = 0.0f; has_pos[0] = true; }
    if (!has_pos[m->nstops - 1]) { pos[m->nstops - 1] = 1.0f; has_pos[m->nstops - 1] = true; }
    for (int i = 1; i < m->nstops; i++) {
        if (has_pos[i]) { if (pos[i] < pos[i - 1]) pos[i] = pos[i - 1]; continue; }
        int j = i;
        while (!has_pos[j]) j++;
        for (int k = i; k < j; k++) pos[k] = pos[i - 1] + (pos[j] - pos[i - 1]) * (float)(k - i + 1) / (float)(j - i + 1);
        i = j - 1;
    }
    memcpy(m->stop_pos, pos, sizeof(float) * (size_t)m->nstops);
    return true;
}

/* The mask for a box with mask-image, or false when it has none usable. */
static bool box_mask(const LayoutBox *box, DisplayMask *m)
{
    memset(m, 0, sizeof(*m));
    m->x = (float)box->x; m->y = (float)box->y;
    m->w = (float)box->width; m->h = (float)box->height;
    m->image_handle = -1;
    if (box->mask_kind == 2 && box->mask_image) {
        m->kind = 2;
        return parse_mask_gradient(box->mask_image, m, m->w, m->h);
    }
    if (box->mask_kind != 1 || !box->mask_image || !g_image_cache) return false;
    m->kind = 1;
    m->repeat = !box->mask_no_repeat;
    /* Size: auto = natural size (the box until the image is known). */
    float iw = m->w, ih = m->h;
    int nw = 0, nh = 0;
    int probe = image_cache_request(g_image_cache, box->mask_image, 0, 0);
    bool natural = probe >= 0 && image_cache_get_natural(g_image_cache, probe, &nw, &nh) && nw > 0 && nh > 0;
    if (box->mask_size_kind == 0 && natural) { iw = (float)nw; ih = (float)nh; }
    else if ((box->mask_size_kind == 1 || box->mask_size_kind == 2) && natural) {
        float sx = m->w / (float)nw, sy = m->h / (float)nh;
        float s = box->mask_size_kind == 1 ? (sx < sy ? sx : sy) : (sx > sy ? sx : sy);
        iw = (float)nw * s; ih = (float)nh * s;
    } else if (box->mask_size_kind == 3) {
        iw = (float)((box->mask_size_pct & 1) ? box->mask_size_w * m->w : box->mask_size_w);
        if (box->mask_size_h >= 0.0)
            ih = (float)((box->mask_size_pct & 2) ? box->mask_size_h * m->h : box->mask_size_h);
        else
            ih = natural ? iw * (float)nh / (float)nw : iw;
    }
    if (iw < 1.0f || ih < 1.0f) return false;
    m->iw = iw; m->ih = ih;
    m->ix = m->x + (float)((box->mask_pos_pct & 1) ? box->mask_pos_x * (m->w - iw) : box->mask_pos_x);
    m->iy = m->y + (float)((box->mask_pos_pct & 2) ? box->mask_pos_y * (m->h - ih) : box->mask_pos_y);
    m->image_handle = image_cache_request(g_image_cache, box->mask_image, (int)ceilf(iw), (int)ceilf(ih));
    return m->image_handle >= 0;
}

/* border-radius as a rounded-rectangle mask over the border box; false for
 * square corners.  Overlapping radii scale down together (CSS Backgrounds
 * §5.5). */
static bool box_round_mask(const LayoutBox *box, DisplayMask *m)
{
    if (box->radius[0] <= 0.0f && box->radius[1] <= 0.0f && box->radius[2] <= 0.0f && box->radius[3] <= 0.0f)
        return false;
    memset(m, 0, sizeof(*m));
    m->kind = 3;
    m->image_handle = -1;
    m->x = (float)box->x; m->y = (float)box->y;
    m->w = (float)box->width; m->h = (float)box->height;
    if (m->w <= 0.0f || m->h <= 0.0f) return false;
    for (int c = 0; c < 4; c++) {
        bool pct = (box->radius_pct >> c) & 1;
        m->rx[c] = pct ? box->radius[c] * m->w : box->radius[c];
        m->ry[c] = pct ? box->radius[c] * m->h : box->radius[c];
    }
    float f = 1.0f;
    float sums[4] = { m->rx[0] + m->rx[1], m->ry[1] + m->ry[2], m->rx[2] + m->rx[3], m->ry[3] + m->ry[0] };
    float sides[4] = { m->w, m->h, m->w, m->h };
    for (int k = 0; k < 4; k++) if (sums[k] > sides[k] && sums[k] > 0.0f && sides[k] / sums[k] < f) f = sides[k] / sums[k];
    for (int c = 0; c < 4; c++) { m->rx[c] *= f; m->ry[c] *= f; }
    return true;
}

static bool is_replaced_tag(const HtmlNode *n)
{
    if (!n || n->type != HTML_NODE_ELEMENT || !n->tag_name) return false;
    return strcasecmp(n->tag_name, "img") == 0 || strcasecmp(n->tag_name, "video") == 0 ||
           strcasecmp(n->tag_name, "svg") == 0 || strcasecmp(n->tag_name, "canvas") == 0 ||
           strcasecmp(n->tag_name, "iframe") == 0;
}

/* One past the last layout index in box i's subtree (preorder numbering). */
static int subtree_end(const LayoutContext *ctx, int i)
{
    int e = i;
    for (;;) {
        int c = ctx->tree.nodes[e].first_child_idx, last = -1;
        for (; c >= 0; c = ctx->tree.nodes[c].next_sibling_idx) last = c;
        if (last < 0) break;
        e = last;
    }
    return e + 1;
}

static bool build_display_list_impl(LayoutContext *ctx, DisplayList *dl, float (*clip)[4]);

bool css_layout_build_display_list(LayoutContext *ctx, DisplayList *dl)
{
    if (!ctx || !dl) return false;
    display_list_init(dl);
    /* overflow != visible clips descendants to the padding box. */
    int n = ctx->tree.count;
    float (*clip)[4] = (float (*)[4])malloc(((size_t)n + 1) * sizeof(*clip));
    if (clip) {
        for (int i = 0; i < n; i++) {
            int p = ctx->tree.nodes[i].parent_idx;
            LayoutBox *b = &ctx->boxes[i];
            float c[4] = { -1e30f, -1e30f, 1e30f, 1e30f };
            if (p >= 0 && p < i && b->position != CSS_POSITION_FIXED) {
                memcpy(c, clip[p], sizeof(c));
                LayoutBox *pb = &ctx->boxes[p];
                /* overflow on <html>/<body> applies to the viewport. */
                HtmlNode *pn = ctx->doc ? (HtmlNode *)po_array_payload(&ctx->doc->array,
                                              ctx->tree.nodes[p].dom_node_idx) : NULL;
                bool viewport_box = pn && pn->type == HTML_NODE_ELEMENT && pn->tag_name &&
                    (strcasecmp(pn->tag_name, "html") == 0 || strcasecmp(pn->tag_name, "body") == 0);
                if (pb->overflow != 0 && pb->display != CSS_DISPLAY_NONE && !viewport_box) {
                    if (getenv("CYBER_DEBUG_CLIP") && ctx->tree.nodes[i].prev_sibling_idx < 0)
                        fprintf(stderr, "[CLIP] box %d by parent %d: %.0f,%.0f %.0fx%.0f\n", i, p,
                                pb->x, pb->y, pb->width, pb->height);
                    float px0 = (float)(pb->x + pb->border_left), py0 = (float)(pb->y + pb->border_top);
                    float px1 = (float)(pb->x + pb->width - pb->border_right);
                    float py1 = (float)(pb->y + pb->height - pb->border_bottom);
                    if (px0 > c[0]) c[0] = px0;
                    if (py0 > c[1]) c[1] = py0;
                    if (px1 < c[2]) c[2] = px1;
                    if (py1 < c[3]) c[3] = py1;
                }
            }
            memcpy(clip[i], c, sizeof(c));
        }
    }
    bool ok = build_display_list_impl(ctx, dl, clip);
    free(clip);
    return ok;
}

static bool build_display_list_impl(LayoutContext *ctx, DisplayList *dl, float (*clip)[4])
{
    bool dark_mode = document_is_dark_mode(ctx);
    TextShaper *default_font = display_list_get_default_font();
    int pending_box = -1, pending_from = 0;
    /* Open masks: each covers its box's subtree, [i, end). */
    int mask_end[64], mask_idx[64], nmask = 0;

    for (int i = 0; i < ctx->tree.count; i++) {
        if (pending_box >= 0 && clip) dl_clip_range(dl, pending_from, clip[pending_box]);
        while (nmask > 0 && mask_end[nmask - 1] <= i) {
            nmask--;
            dl_add_mask_cmd(dl, DL_MASK_END, mask_idx[nmask], &dl->masks[mask_idx[nmask]]);
        }
        pending_box = i;
        pending_from = dl->count;
        LayoutBox *box = &ctx->boxes[i];
        if (!(box->flags & LAYOUT_HAS_LAYOUT)) continue;
        if (box->display == CSS_DISPLAY_NONE) continue;
        if (box->visibility == CSS_VISIBILITY_HIDDEN) continue;
        /* Effective opacity (product over ancestors) scales every paint. */
        float op = (float)box->opacity_eff;
        if (op < 0.004f) continue;
        if (node_has_hidden_class(ctx, i)) continue;
        if (box_or_ancestor_hidden(ctx, i)) continue;

        HtmlNode *node = NULL;
        if (ctx->doc) {
            node = (HtmlNode *)po_array_payload(&ctx->doc->array,
                                                ctx->tree.nodes[i].dom_node_idx);
        }

        /* mask-image: everything the box and its subtree paint goes through
         * the mask (closed above once the subtree is done). */
        if (box->mask_kind && nmask < 64) {
            DisplayMask m;
            if (box_mask(box, &m)) {
                int mi = dl_add_mask(dl, &m);
                if (mi >= 0 && dl_add_mask_cmd(dl, DL_MASK_BEGIN, mi, &m)) {
                    mask_end[nmask] = subtree_end(ctx, i);
                    mask_idx[nmask] = mi;
                    nmask++;
                }
            }
        }
        /* box-shadow (outer): below the box, outside its rounded rectangle. */
        if (box->shadow_set && box->width > 0.0 && box->height > 0.0) {
            DisplayMask sm, rr;
            memset(&sm, 0, sizeof(sm));
            sm.kind = 5;
            sm.image_handle = -1;
            float sp = box->shadow_spread;
            sm.x = (float)box->x + box->shadow_x - sp;
            sm.y = (float)box->y + box->shadow_y - sp;
            sm.w = (float)box->width + 2.0f * sp;
            sm.h = (float)box->height + 2.0f * sp;
            sm.blur = box->shadow_blur;
            sm.ix = (float)box->x; sm.iy = (float)box->y;
            sm.iw = (float)box->width; sm.ih = (float)box->height;
            if (box_round_mask(box, &rr)) {
                for (int c = 0; c < 4; c++) {
                    sm.ex_rx[c] = rr.rx[c]; sm.ex_ry[c] = rr.ry[c];
                    sm.rx[c] = rr.rx[c] > 0.0f ? rr.rx[c] + sp : 0.0f;
                    sm.ry[c] = rr.ry[c] > 0.0f ? rr.ry[c] + sp : 0.0f;
                    if (sm.rx[c] < 0.0f) sm.rx[c] = 0.0f;
                    if (sm.ry[c] < 0.0f) sm.ry[c] = 0.0f;
                }
            }
            if (sm.w > 0.0f && sm.h > 0.0f) {
                int mi = dl_add_mask(dl, &sm);
                if (mi >= 0 && display_list_reserve(dl, 1)) {
                    DisplayListCmd *cmd = &dl->cmds[dl->count++];
                    memset(cmd, 0, sizeof(*cmd));
                    cmd->type = DL_SHADOW;
                    float pad = sm.blur;
                    cmd->x = sm.x - pad; cmd->y = sm.y - pad;
                    cmd->w = sm.w + 2.0f * pad; cmd->h = sm.h + 2.0f * pad;
                    cmd->r = box->shadow_r; cmd->g = box->shadow_g; cmd->b = box->shadow_b;
                    cmd->a = box->shadow_a * op;
                    cmd->u.mask_index = mi;
                }
            }
        }
        /* border-radius: rounds the box's own background and border, and
         * clips the whole subtree when the box clips (overflow) or is a
         * replaced element (rounded thumbnails, avatars). */
        if (nmask < 64) {
            DisplayMask rm;
            if (box_round_mask(box, &rm)) {
                int mi = dl_add_mask(dl, &rm);
                if (mi >= 0 && dl_add_mask_cmd(dl, DL_MASK_BEGIN, mi, &rm)) {
                    mask_end[nmask] = (box->overflow != 0 || is_replaced_tag(node)) ? subtree_end(ctx, i) : i + 1;
                    mask_idx[nmask] = mi;
                    nmask++;
                }
            }
        }

        /* Backgrounds paint first: under the borders, list marker and the
         * box's content. */
        if (box->width >= 2.0f && box->height >= 2.0f && box->width * box->height >= 6.0f) {
            if (box->background_color_a > 0.0f &&
                !display_list_add_rect(dl, (float)box->x, (float)box->y,
                                       (float)box->width, (float)box->height,
                                       (float)box->background_color_r, (float)box->background_color_g,
                                       (float)box->background_color_b, (float)box->background_color_a * op))
                return false;
            /* Background image, stretched to the padding box. */
            if (g_image_cache && box->background_image_url[0]) {
                int handle = image_cache_request(g_image_cache, box->background_image_url,
                                                 (int)ceil(box->width), (int)ceil(box->height));
                emit_image(dl, (float)box->x, (float)box->y,
                           (float)box->width, (float)box->height, handle);
            }
        }

        /* Replaced images: <img src="..."> */
        if (node && node->type == HTML_NODE_ELEMENT &&
            strcasecmp(node->tag_name, "img") == 0 && g_image_cache) {
            char *url = display_list_image_source(ctx->base_url, node, (float)box->width);
            if (url) {
                emit_image_async(dl, box, url, op);
                free(url);
            }
            continue;
        }

        /* Inline <svg>: the outermost one paints its whole subtree. */
        if (node && node->type == HTML_NODE_ELEMENT && node->tag_name &&
            strcasecmp(node->tag_name, "svg") == 0 && g_image_cache && ctx->doc) {
            int di = ctx->tree.nodes[i].dom_node_idx;
            if (!inside_svg(&ctx->doc->array, di)) emit_inline_svg(ctx, dl, box, di, op);
            continue;
        }

        /* <video>: the frame its player shows now, letterboxed (object-fit
         * defaults to contain for video). */
        if (node && node->type == HTML_NODE_ELEMENT && node->store_id &&
            strcasecmp(node->tag_name, "video") == 0 && g_image_cache) {
            const uint8_t *px = NULL;
            int vw = 0, vh = 0;
            uint64_t frame_id = 0;
            if (media_video_frame_for_node(node->store_id, &px, &vw, &vh, &frame_id)) {
                char key[48];
                snprintf(key, sizeof(key), "cyber-video-frame:%u", node->store_id);
                image_cache_put_rgba(g_image_cache, key, vw, vh, px);
                LayoutBox vbox = *box;
                if (vbox.object_fit == 0) vbox.object_fit = 1;
                emit_image_async(dl, &vbox, key, op);
            }
            continue;
        }

        /* Emit glyphs for text nodes when a default font is available.
         * Text nodes often have no measured box size in this layout engine,
         * so emit them before the visibility/size filters.
         * Skip nodes whose box has collapsed to the viewport root; they have
         * not been positioned and would all overlap at the origin. */
        if (default_font && ctx->doc) {
            if (node && node->type == HTML_NODE_TEXT &&
                css_layout_node_text(ctx, node) && css_layout_node_text(ctx, node)[0] &&
                !text_is_whitespace(css_layout_node_text(ctx, node))) {
                bool root_sized = (box->x == 0.0 && box->y == 0.0 &&
                                   box->width == ctx->viewport_width &&
                                   box->height == ctx->viewport_height);
                if (!root_sized) {
                    float tr = (float)box->color_r;
                    float tg = (float)box->color_g;
                    float tb = (float)box->color_b;
                    float ta = (float)box->color_a;
                    if (ta <= 0.0f) ta = 1.0f;
                    ta *= op;
                    /* In dark mode, default-black text is invisible on dark
                     * backgrounds.  Render it white so labels are readable. */
                    if (dark_mode && tr < 0.1f && tg < 0.1f && tb < 0.1f) {
                        tr = 1.0f; tg = 1.0f; tb = 1.0f;
                    }
                    /* Scale glyph metrics for the box's font size; the shaper
                     * itself was loaded at 16px. */
                    double fs = box->font_size > 0.0 ? box->font_size : 16.0;
                    float scale = (float)(fs / 16.0);
                    /* Pick the font variant matching the box's typography
                     * (sans/serif/mono x bold x italic). */
                    int fslot = box->font_slot >= 0 ? box->font_slot : display_list_resolve_font_slot(
                        box->font_family, box->font_weight, box->font_italic ? 1 : 0);
                    TextShaper *font = display_list_get_font(fslot);
                    if (!font) font = default_font;
                    int from_cmd = dl->count;
                    /* The white-space processed text the layout measured. */
                    const char *run_text = css_layout_box_text(ctx, i, css_layout_node_text(ctx, node));
                    if (box->text_lead_skip && run_text[0] == ' ') run_text++;
                    /* -webkit-line-clamp / text-overflow: ellipsis on a near
                     * ancestor cut the run and end it with an ellipsis. */
                    char *clamped = NULL;
                    for (int a = ctx->tree.nodes[i].parent_idx, depth = 0; a >= 0 && depth < 6;
                         a = ctx->tree.nodes[a].parent_idx, depth++) {
                        const LayoutBox *ab = &ctx->boxes[a];
                        if (ab->line_clamp && ab->webkit_box) {
                            double top = ab->y + ab->border_top + ab->padding_top;
                            float nlh0 = text_shaper_normal_line_height(font, (float)fs);
                            double adv = box->line_height > 0.0 ? box->line_height
                                       : (nlh0 > 0.5f * fs && nlh0 < 3.0f * fs ? nlh0 : fs * 1.5);
                            int before = adv > 0.0 ? (int)floor((box->y - top) / adv + 0.01) : 0;
                            int left = (int)ab->line_clamp - (before > 0 ? before : 0);
                            if (left >= 1 && box->wrap_cont_w > 0.0)
                                clamped = text_shaper_truncate_lines(font, run_text, (float)box->x,
                                                                     (float)box->wrap_cont_x, (float)box->wrap_first_w,
                                                                     (float)box->wrap_cont_w, scale, left);
                            break;
                        }
                        if (ab->text_overflow_ellipsis && ab->overflow != 0 &&
                            (ab->white_space == 1 || ab->white_space == 2)) {
                            double right = ab->x + ab->width - ab->border_right - ab->padding_right;
                            if (box->wrap_cont_w <= 0.0 && right > box->x)
                                clamped = text_shaper_truncate_width(font, run_text, scale, (float)(right - box->x));
                            break;
                        }
                    }
                    if (clamped) run_text = clamped;
                    if (box->wrap_cont_w > 0.0) {
                        /* Wrapped run: shape across lines using the wrap
                         * geometry recorded by the layout. */
                        float nlh = text_shaper_normal_line_height(font, (float)fs);
                        if (!(nlh > 0.5f * fs && nlh < 3.0f * fs)) nlh = (float)(fs * 1.5);
                        float line_adv = box->line_height > 0.0
                            ? (float)box->line_height : nlh;
                        if (!text_shaper_wrap_shape(font,
                                                    run_text,
                                                    (float)box->x, (float)box->y,
                                                    (float)box->wrap_cont_x,
                                                    (float)box->wrap_first_w,
                                                    (float)box->wrap_cont_w,
                                                    scale, line_adv,
                                                    box->wrap_cont2_line,
                                                    (float)box->wrap_cont2_x,
                                                    (float)box->wrap_cont2_w,
                                                    tr, tg, tb, ta,
                                                    dl, NULL)) {
                            return false;
                        }
                    } else {
                        /* CSS strut model: center the em box in the line box;
                         * the shaper adds the em ascent on top of this y. */
                        float nlh = text_shaper_normal_line_height(font, (float)fs);
                        if (!(nlh > 0.5f * fs && nlh < 3.0f * fs)) nlh = (float)(fs * 1.5);
                        double lh = box->line_height > 0.0 ? box->line_height : nlh;
                        double half_leading = (lh - fs) * 0.5;
                        if (half_leading < 0.0) half_leading = 0.0;
                        if (!text_shaper_shape_to_display_list_scaled(font,
                                                           run_text,
                                                           (float)box->x,
                                                           (float)(box->y + half_leading),
                                                           scale,
                                                           tr, tg, tb, ta,
                                                           dl)) {
                            return false;
                        }
                    }
                    emit_text_decorations(dl, from_cmd, fs, box->text_decoration,
                                          tr, tg, tb, ta);
                    free(clamped);
                }
            }
        }

        /* display:list-item marker (bullet / ordinal) in the gutter to the
         * left of the item's first line. */
        if (default_font && node && node->type == HTML_NODE_ELEMENT &&
            box->display == CSS_DISPLAY_LIST_ITEM && box->list_style_type != 0) {
            char marker[24];
            if (box->list_style_type == 4) {
                /* decimal: ordinal from preceding <li> element siblings. */
                int ordinal = 1;
                int dom_idx = ctx->tree.nodes[i].dom_node_idx;
                for (int s = po_array_prev_sibling(&ctx->doc->array, dom_idx);
                     s >= 0;
                     s = po_array_prev_sibling(&ctx->doc->array, s)) {
                    HtmlNode *sn = (HtmlNode *)po_array_payload(&ctx->doc->array, s);
                    if (sn && sn->type == HTML_NODE_ELEMENT &&
                        strcasecmp(sn->tag_name, "li") == 0) ordinal++;
                }
                snprintf(marker, sizeof(marker), "%d.", ordinal);
            } else if (box->list_style_type == 2) {
                snprintf(marker, sizeof(marker), "\xE2\x97\xA6"); /* U+25E6 white bullet */
            } else if (box->list_style_type == 3) {
                snprintf(marker, sizeof(marker), "\xE2\x96\xAA"); /* U+25AA black small square */
            } else {
                snprintf(marker, sizeof(marker), "\xE2\x80\xA2"); /* U+2022 bullet */
            }
            double mfs = box->font_size > 0.0 ? box->font_size : 16.0;
            float mscale = (float)(mfs / 16.0);
            int mfslot = box->font_slot >= 0 ? box->font_slot : display_list_resolve_font_slot(
                box->font_family, box->font_weight, box->font_italic ? 1 : 0);
            TextShaper *mfont = display_list_get_font(mfslot);
            if (!mfont) mfont = default_font;
            float mw = 0.0f, mh = 0.0f;
            text_shaper_measure(mfont, marker, &mw, &mh);
            double mx = box->x - (double)mw * mscale - mfs * 0.4;
            if (mx < 0.0) mx = 0.0;
            text_shaper_shape_to_display_list_scaled(mfont, marker,
                    (float)mx, (float)box->y, mscale,
                    (float)box->color_r, (float)box->color_g,
                    (float)box->color_b, (float)box->color_a * op, dl);
        }

        /* Borders, painted as four filled edge rects so per-side widths work.
         * The color defaults to currentColor when no border-color was given.
         * Emitted before the small-box filter: a thin box (e.g. <hr>) still
         * needs its border painted. */
        if (box->border_top > 0.0 || box->border_right > 0.0 ||
            box->border_bottom > 0.0 || box->border_left > 0.0) {
            float br_r = (float)(box->border_color_set ? box->border_color_r : box->color_r);
            float br_g = (float)(box->border_color_set ? box->border_color_g : box->color_g);
            float br_b = (float)(box->border_color_set ? box->border_color_b : box->color_b);
            float br_a = (float)(box->border_color_set ? box->border_color_a : box->color_a) * op;
            float bx = (float)box->x, by = (float)box->y;
            float bw = (float)box->width, bh = (float)box->height;
            DisplayMask ring;
            if (box_round_mask(box, &ring)) {
                /* Rounded: the border is the ring between the outer and the
                 * inset rounded rectangles. */
                ring.kind = 4;
                ring.border[0] = (float)box->border_top;
                ring.border[1] = (float)box->border_right;
                ring.border[2] = (float)box->border_bottom;
                ring.border[3] = (float)box->border_left;
                int mi = dl_add_mask(dl, &ring);
                if (mi >= 0 && dl_add_mask_cmd(dl, DL_MASK_BEGIN, mi, &ring)) {
                    display_list_add_rect(dl, bx, by, bw, bh, br_r, br_g, br_b, br_a);
                    dl_add_mask_cmd(dl, DL_MASK_END, mi, &ring);
                }
            } else {
            if (box->border_top > 0.0) {
                display_list_add_rect(dl, bx, by, bw, (float)box->border_top,
                                      br_r, br_g, br_b, br_a);
            }
            if (box->border_bottom > 0.0) {
                display_list_add_rect(dl, bx, by + bh - (float)box->border_bottom,
                                      bw, (float)box->border_bottom,
                                      br_r, br_g, br_b, br_a);
            }
            double mid_h = box->height - box->border_top - box->border_bottom;
            if (mid_h < 0.0) mid_h = 0.0;
            if (box->border_left > 0.0) {
                display_list_add_rect(dl, bx, by + (float)box->border_top,
                                      (float)box->border_left, (float)mid_h,
                                      br_r, br_g, br_b, br_a);
            }
            if (box->border_right > 0.0) {
                display_list_add_rect(dl, bx + bw - (float)box->border_right,
                                      by + (float)box->border_top,
                                      (float)box->border_right, (float)mid_h,
                                      br_r, br_g, br_b, br_a);
            }
            }
        }

        if (box->width < 2.0f || box->height < 2.0f) continue;
        if (box->width * box->height < 6.0f) continue;

        /* Placeholder fills for content shells injected without visible CSS in
         * our engine.  This makes skeleton thumbnails, avatars, and text shells
         * visible instead of transparent.  Colors are chosen to contrast with
         * dark backgrounds.  Only applies when the cascade did not provide a
         * background color — otherwise it would paint over the real (light)
         * skeleton color from the page's CSS.
         * YouTube-only: the class needles are YouTube skeleton classes, and
         * substring matching would otherwise misfire on classes like
         * MediaWiki's "vector-search-box-show-thumbnail". */
        /* Retired: the layout engine now renders YouTube's real content
         * (CYBER_YT_PLACEHOLDERS=1 re-enables the skeleton fill). */
        static int placeholders_env = -1;
        if (placeholders_env < 0) placeholders_env = getenv("CYBER_YT_PLACEHOLDERS") ? 1 : 0;
        bool placeholders_enabled = placeholders_env && ctx->base_url &&
                                    strstr(ctx->base_url, "youtube.") != NULL;
        if (placeholders_enabled && !box->background_image_url[0] && box->background_color_a <= 0.0f) {
            static const char *thumbnail_needles[] = {
                "rich-thumbnail", "video-thumbnail", "thumbnail", NULL
            };
            static const char *avatar_needles[] = {
                "channel-avatar", "avatar", NULL
            };
            static const char *title_needles[] = {
                "rich-video-title", "video-title", "text-shell", NULL
            };
            static const char *meta_needles[] = {
                "rich-video-meta", "video-meta", NULL
            };
            static const char *details_needles[] = {
                "details-text-shell", NULL
            };
            static const char *skeleton_needles[] = {
                "skeleton-bg-color", "video-skeleton",
                "skeleton-light-border-bottom", NULL
            };
            static const char *chip_needles[] = {
                "home-chips", "home-chips-ghost", NULL
            };
            static const char *guide_needles[] = {
                "guide-skeleton", "guide-ghost", "guide-ghost-icon",
                "guide-ghost-text", NULL
            };
            float pr = 0.0f, pg = 0.0f, pb = 0.0f, pa = 0.0f;
            if (node_class_contains_any(node, thumbnail_needles)) {
                pr = 0.25f; pg = 0.25f; pb = 0.32f; pa = 1.0f; /* slate thumbnail */
            } else if (node_class_contains_any(node, avatar_needles)) {
                pr = 0.50f; pg = 0.50f; pb = 0.50f; pa = 1.0f; /* #808080 */
            } else if (node_class_contains_any(node, title_needles)) {
                pr = 0.35f; pg = 0.35f; pb = 0.35f; pa = 1.0f; /* #595959 */
            } else if (node_class_contains_any(node, meta_needles)) {
                pr = 0.28f; pg = 0.28f; pb = 0.28f; pa = 1.0f; /* #474747 */
            } else if (node_class_contains_any(node, details_needles)) {
                pr = 0.22f; pg = 0.22f; pb = 0.22f; pa = 1.0f; /* #383838 */
            } else if (node_class_contains_any(node, skeleton_needles)) {
                pr = 0.40f; pg = 0.40f; pb = 0.40f; pa = 1.0f; /* #666666 */
            } else if (node_class_contains_any(node, chip_needles)) {
                pr = 0.45f; pg = 0.45f; pb = 0.45f; pa = 1.0f; /* chip pill */
            } else if (node_class_contains_any(node, guide_needles)) {
                pr = 0.18f; pg = 0.18f; pb = 0.18f; pa = 1.0f; /* guide sidebar */
            }
            if (pa > 0.0f) {
                if (!display_list_add_rect(dl,
                                           (float)box->x, (float)box->y,
                                           (float)box->width, (float)box->height,
                                           pr, pg, pb, pa)) {
                    return false;
                }
            }
        }

    }
    if (pending_box >= 0 && clip) dl_clip_range(dl, pending_from, clip[pending_box]);
    while (nmask > 0) {
        nmask--;
        dl_add_mask_cmd(dl, DL_MASK_END, mask_idx[nmask], &dl->masks[mask_idx[nmask]]);
    }

    return true;
}

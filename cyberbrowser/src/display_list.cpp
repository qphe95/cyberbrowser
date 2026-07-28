/*
 * Display List - Implementation
 */

#include "display_list.h"
#include "platform.h"
#include "url_utils.h"
#include "text_shaper.h"
#include "image_cache.h"
#include "browser_api_impl_types.h"

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
}

void display_list_free(DisplayList *dl)
{
    if (!dl) return;
    free(dl->cmds);
    dl->cmds = NULL;
    dl->count = 0;
    dl->capacity = 0;
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

/* Global default font used when the display list encounters text nodes. */
static TextShaper *g_default_font = NULL;
static ImageCache *g_image_cache = NULL;

/* Font table: 3 families x 4 variants.  Slot 0 aliases g_default_font. */
static TextShaper *g_font_slots[DL_FONT_SLOTS] = {0};

void display_list_set_image_cache(ImageCache *cache)
{
    g_image_cache = cache;
}

ImageCache *display_list_get_image_cache(void)
{
    return g_image_cache;
}

bool display_list_set_default_font(const char *ttf_path, float size_pixels)
{
    if (g_default_font) {
        text_shaper_destroy(g_default_font);
        g_default_font = NULL;
    }
    g_font_slots[0] = NULL;
    if (!ttf_path) return true;
    g_default_font = text_shaper_create(ttf_path, size_pixels);
    g_font_slots[0] = g_default_font;
    return g_default_font != NULL;
}

struct TextShaper *display_list_get_default_font(void)
{
    return g_default_font;
}

bool display_list_set_font(int slot, const char *ttf_path, float size_pixels)
{
    if (slot < 0 || slot >= DL_FONT_SLOTS) return false;
    if (slot == 0) return display_list_set_default_font(ttf_path, size_pixels);
    if (g_font_slots[slot]) {
        text_shaper_destroy(g_font_slots[slot]);
        g_font_slots[slot] = NULL;
    }
    if (!ttf_path) return true;
    g_font_slots[slot] = text_shaper_create(ttf_path, size_pixels);
    return g_font_slots[slot] != NULL;
}

struct TextShaper *display_list_get_font(int slot)
{
    if (slot < 0 || slot >= DL_FONT_SLOTS) return g_default_font;
    TextShaper *s = g_font_slots[slot];
    return s ? s : g_default_font;
}

int display_list_resolve_font_slot(const char *font_family, int font_weight,
                                   int font_italic)
{
    int base = DL_FONT_SANS;
    if (font_family && font_family[0]) {
        char buf[96];
        size_t n = strlen(font_family);
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        for (size_t i = 0; i < n; i++)
            buf[i] = (char)tolower((unsigned char)font_family[i]);
        buf[n] = '\0';
        if (strstr(buf, "mono") || strstr(buf, "courier") ||
            strstr(buf, "consolas") || strstr(buf, "menlo")) {
            base = DL_FONT_MONO;
        } else if ((strstr(buf, "serif") && !strstr(buf, "sans")) ||
                   strstr(buf, "times") || strstr(buf, "georgia")) {
            base = DL_FONT_SERIF;
        }
    }
    int slot = base + (font_weight >= 600 ? 1 : 0) + (font_italic ? 2 : 0);
    /* Fall back to whatever variant of the family is actually loaded. */
    if (!g_font_slots[slot]) slot = base;
    if (!g_font_slots[slot]) slot = 0;
    return slot;
}

void display_list_stamp_font_slot(DisplayList *dl, int from_cmd, int slot)
{
    if (!dl || slot <= 0) return;
    for (int i = from_cmd; i < dl->count; i++) {
        if (dl->cmds[i].type == DL_GLYPH)
            dl->cmds[i].u.glyph.font_slot = (uint8_t)slot;
    }
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
    dom_request_layout();
}

static void emit_image_async(DisplayList *dl, float x, float y, float w, float h, const char *url)
{
    if (!g_image_cache || !url || !url[0]) return;
    int handle = image_cache_load_async(g_image_cache, url, on_async_image_loaded, NULL);
    if (handle < 0) return;

    int img_w = 0, img_h = 0, ch = 0;
    uint8_t *pix = NULL;
    if (image_cache_get(g_image_cache, handle, &img_w, &img_h, &ch, &pix)) {
        /* Image already available (local or cached): emit normally. */
        if (w <= 0.0f) w = (float)img_w;
        if (h <= 0.0f) h = (float)img_h;
        if (w > 0.0f && h > 0.0f) {
            display_list_add_image(dl, x, y, w, h, handle, 0.0f, 0.0f, 1.0f, 1.0f);
        }
    } else {
        /* Still loading: draw a placeholder border so the layout box is visible. */
        float pw = (w > 0.0f) ? w : 32.0f;
        float ph = (h > 0.0f) ? h : 32.0f;
        display_list_add_border(dl, x, y, pw, ph, 1.0f, 0.6f, 0.6f, 0.6f, 1.0f);
    }
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

bool css_layout_build_display_list(LayoutContext *ctx, DisplayList *dl)
{
    if (!ctx || !dl) return false;
    display_list_init(dl);
    bool dark_mode = document_is_dark_mode(ctx);

    for (int i = 0; i < ctx->tree.count; i++) {
        LayoutBox *box = &ctx->boxes[i];
        if (!(box->flags & LAYOUT_HAS_LAYOUT)) continue;
        if (box->display == CSS_DISPLAY_NONE) continue;
        if (box->visibility == CSS_VISIBILITY_HIDDEN) continue;
        if (node_has_hidden_class(ctx, i)) continue;
        if (box_or_ancestor_hidden(ctx, i)) continue;

        HtmlNode *node = NULL;
        if (ctx->doc) {
            node = (HtmlNode *)po_array_payload(&ctx->doc->array,
                                                ctx->tree.nodes[i].dom_node_idx);
        }

        /* Replaced images: <img src="..."> */
        if (node && node->type == HTML_NODE_ELEMENT &&
            strcasecmp(node->tag_name, "img") == 0 && g_image_cache) {
            const char *src = node_attribute_value(node, "src");
            /* srcset, when present, overrides src with the candidate that
             * best matches the laid-out box width. */
            const char *srcset = node_attribute_value(node, "srcset");
            char *picked = srcset ? srcset_pick(srcset, (float)box->width) : NULL;
            if (picked) src = picked;
            if (src && src[0]) {
                char *url = dl_resolve_url(ctx->base_url, src);
                if (url) {
                    float w = (float)box->width;
                    float h = (float)box->height;
                    emit_image_async(dl, (float)box->x, (float)box->y, w, h, url);
                    free(url);
                }
            }
            free(picked);
            continue;
        }

        /* Emit glyphs for text nodes when a default font is available.
         * Text nodes often have no measured box size in this layout engine,
         * so emit them before the visibility/size filters.
         * Skip nodes whose box has collapsed to the viewport root; they have
         * not been positioned and would all overlap at the origin. */
        if (g_default_font && ctx->doc) {
            if (node && node->type == HTML_NODE_TEXT &&
                node->text_content && node->text_len > 0 &&
                !text_is_whitespace(node->text_content)) {
                bool root_sized = (box->x == 0.0 && box->y == 0.0 &&
                                   box->width == ctx->viewport_width &&
                                   box->height == ctx->viewport_height);
                if (!root_sized) {
                    float tr = (float)box->color_r;
                    float tg = (float)box->color_g;
                    float tb = (float)box->color_b;
                    float ta = (float)box->color_a;
                    if (ta <= 0.0f) ta = 1.0f;
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
                    int fslot = display_list_resolve_font_slot(
                        box->font_family, box->font_weight, box->font_italic ? 1 : 0);
                    TextShaper *font = display_list_get_font(fslot);
                    if (!font) font = g_default_font;
                    int from_cmd = dl->count;
                    if (box->wrap_cont_w > 0.0) {
                        /* Wrapped run: shape across lines using the wrap
                         * geometry recorded by the layout. */
                        float line_adv = box->line_height > 0.0
                            ? (float)box->line_height : (float)(fs * 1.5);
                        if (!text_shaper_wrap_shape(font,
                                                    node->text_content,
                                                    (float)box->x, (float)box->y,
                                                    (float)box->wrap_cont_x,
                                                    (float)box->wrap_first_w,
                                                    (float)box->wrap_cont_w,
                                                    scale, line_adv,
                                                    tr, tg, tb, ta,
                                                    dl, NULL)) {
                            return false;
                        }
                    } else {
                        /* CSS strut model: center the em box in the line box;
                         * the shaper adds the em ascent on top of this y. */
                        double lh = box->line_height > 0.0 ? box->line_height
                                                           : fs * 1.5;
                        double half_leading = (lh - fs) * 0.5;
                        if (half_leading < 0.0) half_leading = 0.0;
                        if (!text_shaper_shape_to_display_list_scaled(font,
                                                           node->text_content,
                                                           (float)box->x,
                                                           (float)(box->y + half_leading),
                                                           scale,
                                                           tr, tg, tb, ta,
                                                           dl)) {
                            return false;
                        }
                    }
                    display_list_stamp_font_slot(dl, from_cmd, fslot);
                    emit_text_decorations(dl, from_cmd, fs, box->text_decoration,
                                          tr, tg, tb, ta);
                }
            }
        }

        /* display:list-item marker (bullet / ordinal) in the gutter to the
         * left of the item's first line. */
        if (g_default_font && node && node->type == HTML_NODE_ELEMENT &&
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
            int mfslot = display_list_resolve_font_slot(
                box->font_family, box->font_weight, box->font_italic ? 1 : 0);
            TextShaper *mfont = display_list_get_font(mfslot);
            if (!mfont) mfont = g_default_font;
            float mw = 0.0f, mh = 0.0f;
            text_shaper_measure(mfont, marker, &mw, &mh);
            double mx = box->x - (double)mw * mscale - mfs * 0.4;
            if (mx < 0.0) mx = 0.0;
            int from_cmd = dl->count;
            if (text_shaper_shape_to_display_list_scaled(mfont, marker,
                    (float)mx, (float)box->y, mscale,
                    (float)box->color_r, (float)box->color_g,
                    (float)box->color_b, (float)box->color_a, dl)) {
                display_list_stamp_font_slot(dl, from_cmd, mfslot);
            }
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
            float br_a = (float)(box->border_color_set ? box->border_color_a : box->color_a);
            float bx = (float)box->x, by = (float)box->y;
            float bw = (float)box->width, bh = (float)box->height;
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

        if (box->width < 2.0f || box->height < 2.0f) continue;
        if (box->width * box->height < 6.0f) continue;

        /* Background rectangle when a color is explicitly set. */
        if (box->background_color_a > 0.0f) {
            if (!display_list_add_rect(dl,
                                       (float)box->x, (float)box->y,
                                       (float)box->width, (float)box->height,
                                       (float)box->background_color_r,
                                       (float)box->background_color_g,
                                       (float)box->background_color_b,
                                       (float)box->background_color_a)) {
                return false;
            }
        }

        /* Background image, stretched to the padding box. */
        if (g_image_cache && box->background_image_url[0]) {
            int handle = image_cache_load(g_image_cache, box->background_image_url);
            emit_image(dl, (float)box->x, (float)box->y,
                       (float)box->width, (float)box->height, handle);
        }

        /* Placeholder fills for content shells injected without visible CSS in
         * our engine.  This makes skeleton thumbnails, avatars, and text shells
         * visible instead of transparent.  Colors are chosen to contrast with
         * dark backgrounds.  Only applies when the cascade did not provide a
         * background color — otherwise it would paint over the real (light)
         * skeleton color from the page's CSS.
         * YouTube-only: the class needles are YouTube skeleton classes, and
         * substring matching would otherwise misfire on classes like
         * MediaWiki's "vector-search-box-show-thumbnail". */
        bool placeholders_enabled = ctx->base_url && strstr(ctx->base_url, "youtube.") != NULL;
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

        /* Explicit border. */
        if (box->border_top > 0 || box->border_right > 0 ||
            box->border_bottom > 0 || box->border_left > 0) {
            float thickness = (float)(box->border_top + box->border_right +
                                      box->border_bottom + box->border_left) / 4.0f;
            if (thickness <= 0) thickness = 1.0f;
            if (!display_list_add_border(dl,
                                         (float)box->x, (float)box->y,
                                         (float)box->width, (float)box->height,
                                         thickness,
                                         (float)box->color_r,
                                         (float)box->color_g,
                                         (float)box->color_b,
                                         (float)box->color_a)) {
                return false;
            }
        }

    }

    return true;
}

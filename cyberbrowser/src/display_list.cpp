/*
 * Display List - Implementation
 */

#include "display_list.h"
#include "platform.h"
#include "text_shaper.h"
#include "image_cache.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

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
    if (!ttf_path) return true;
    g_default_font = text_shaper_create(ttf_path, size_pixels);
    return g_default_font != NULL;
}

struct TextShaper *display_list_get_default_font(void)
{
    return g_default_font;
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
        if (strcasecmp(a->name, "class") == 0 && a->value) {
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

static bool node_has_hidden_class(LayoutContext *ctx, int node_idx)
{
    if (node_idx < 0 || node_idx >= ctx->tree.count) return false;
    HtmlNode *node = (HtmlNode*)po_array_payload(&ctx->doc->array,
                                                  ctx->tree.nodes[node_idx].dom_node_idx);
    return node_has_class(node, "hidden");
}

static const char* node_attribute_value(HtmlNode *node, const char *name)
{
    if (!node || node->type != HTML_NODE_ELEMENT) return NULL;
    for (HtmlAttribute *a = node->attributes; a; a = a->next) {
        if (strcasecmp(a->name, name) == 0) return a->value;
    }
    return NULL;
}

static bool is_absolute_local_path(const char *href)
{
    if (href[0] == '/') return true;
    const char *colon = strchr(href, ':');
    if (colon && strncmp(colon, "://", 3) != 0) return true;
    return false;
}

static char* dl_resolve_url(const char *base_url, const char *href)
{
    if (!href || !href[0]) return NULL;
    if (strncasecmp(href, "http://", 7) == 0 || strncasecmp(href, "https://", 8) == 0)
        return strdup(href);
    if (strncmp(href, "//", 2) == 0) {
        char buf[2048];
        snprintf(buf, sizeof(buf), "https:%s", href);
        return strdup(buf);
    }
    if (is_absolute_local_path(href))
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

bool css_layout_build_display_list(LayoutContext *ctx, DisplayList *dl)
{
    if (!ctx || !dl) return false;
    display_list_init(dl);

    for (int i = 0; i < ctx->tree.count; i++) {
        LayoutBox *box = &ctx->boxes[i];
        if (!(box->flags & LAYOUT_HAS_LAYOUT)) continue;
        if (box->display == CSS_DISPLAY_NONE) continue;
        if (box->visibility == CSS_VISIBILITY_HIDDEN) continue;
        if (node_has_hidden_class(ctx, i)) continue;

        HtmlNode *node = NULL;
        if (ctx->doc) {
            node = (HtmlNode *)po_array_payload(&ctx->doc->array,
                                                ctx->tree.nodes[i].dom_node_idx);
        }

        /* Replaced images: <img src="..."> */
        if (node && node->type == HTML_NODE_ELEMENT &&
            strcasecmp(node->tag_name, "img") == 0 && g_image_cache) {
            const char *src = node_attribute_value(node, "src");
            if (src && src[0]) {
                char *url = dl_resolve_url(ctx->base_url, src);
                if (url) {
                    int handle = image_cache_load(g_image_cache, url);
                    if (handle >= 0) {
                        float w = (float)box->width;
                        float h = (float)box->height;
                        emit_image(dl, (float)box->x, (float)box->y, w, h, handle);
                    }
                    free(url);
                }
            }
            continue;
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
        } else if (!node_has_class(node, "yt-card")) {
            /* Every visible element gets at least a wireframe outline so the
             * layout structure is observable even without explicit styles. */
            if (!display_list_add_border(dl,
                                         (float)box->x, (float)box->y,
                                         (float)box->width, (float)box->height,
                                         1.0f,
                                         0.75f, 0.75f, 0.75f, 1.0f)) {
                return false;
            }
        }

        /* Emit glyphs for text nodes when a default font is available. */
        if (g_default_font && ctx->doc) {
            if (node && node->type == HTML_NODE_TEXT &&
                node->text_content && node->text_len > 0 &&
                !text_is_whitespace(node->text_content)) {
                if (!text_shaper_shape_to_display_list(g_default_font,
                                                       node->text_content,
                                                       (float)box->x, (float)box->y,
                                                       (float)box->color_r,
                                                       (float)box->color_g,
                                                       (float)box->color_b,
                                                       (float)box->color_a,
                                                       dl)) {
                    return false;
                }
            }
        }
    }

    return true;
}

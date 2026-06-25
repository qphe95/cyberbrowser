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
    /* Treat any node inside a hidden subtree as hidden.  YouTube's static
     * watch-page skeleton carries class "hidden" on its root, but its
     * descendants do not, so we must walk up the layout tree. */
    if (node_or_ancestor_has_class(ctx, node_idx, "hidden")) return true;
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

/* Return true if the document is in a dark theme (e.g. YouTube dark mode).
 * This is used to make default-black text visible on dark backgrounds. */
static bool document_is_dark_mode(LayoutContext *ctx)
{
    if (!ctx || !ctx->doc) return false;
    for (size_t i = 0; i < ctx->doc->array.count; i++) {
        HtmlNode *node = (HtmlNode *)po_array_payload(&ctx->doc->array, i);
        if (!node || node->type != HTML_NODE_ELEMENT) continue;
        if (strcasecmp(node->tag_name, "html") == 0 ||
            strcasecmp(node->tag_name, "body") == 0) {
            if (node_attribute_value(node, "darker-dark-theme") != NULL) return true;
            if (node_attribute_value(node, "dark") != NULL) return true;
            static const char *dark_needles[] = {"dark", "darker-dark-theme", NULL};
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
                    float w = (float)box->width;
                    float h = (float)box->height;
                    emit_image_async(dl, (float)box->x, (float)box->y, w, h, url);
                    free(url);
                }
            }
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
                    if (!text_shaper_shape_to_display_list(g_default_font,
                                                           node->text_content,
                                                           (float)box->x, (float)box->y,
                                                           tr, tg, tb, ta,
                                                           dl)) {
                        return false;
                    }
                }
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

        /* Placeholder fills for content shells that YouTube injects without
         * visible CSS in our engine.  This makes skeleton thumbnails, avatars,
         * and text shells visible instead of transparent.  Colors are chosen
         * to contrast with YouTube's dark (#0f0f0f) background.
         * These intentionally override any near-black CSS background so the
         * skeleton shapes remain visible. */
        if (!box->background_image_url[0]) {
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

/*
 * Display List - Simple intermediate representation for CSS layout output.
 *
 * The layout engine produces a list of primitive commands (rectangles, borders,
 * glyphs) that a renderer can consume.  This decouples layout from any
 * specific rendering backend (Vulkan, software, etc.).
 */

#ifndef DISPLAY_LIST_H
#define DISPLAY_LIST_H

#include <stdbool.h>
#include <stddef.h>
#include "css_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DL_RECT,
    DL_BORDER,
    DL_GLYPH,
    DL_IMAGE,
    /* Commands between a MASK_BEGIN and its MASK_END are composited through
     * the mask (DisplayList.masks[u.mask_index]). */
    DL_MASK_BEGIN,
    DL_MASK_END,
    /* box-shadow: a blurred rounded rectangle (DisplayList.masks[u.mask_index],
     * kind 5) in the command's color, not drawn under the box itself. */
    DL_SHADOW,
} DisplayListCmdType;

#define DL_MASK_STOPS 8

/* A CSS mask over a box: a linear gradient (alpha along a line through the
 * box) or an image whose alpha masks, placed at (ix, iy, iw, ih) and
 * optionally repeated. */
typedef struct {
    float x, y, w, h;          /* the masked box (its border box) */
    int kind;                  /* 1 image, 2 linear gradient, 3 rounded rectangle,
                                  4 rounded border ring (3 minus its inset by border[]) */
    int image_handle;
    float ix, iy, iw, ih;
    int repeat;
    float angle_deg;           /* CSS gradient angle: 0 = to top, 90 = to right */
    int nstops;
    float stop_pos[DL_MASK_STOPS];     /* along the gradient line, 0..1 */
    float stop_alpha[DL_MASK_STOPS];
    float rx[4], ry[4];        /* rounded rectangle: corner radii (tl, tr, br, bl) */
    float border[4];           /* ring: border widths (top, right, bottom, left) */
    float blur;                /* shadow: blur radius (2 sigma) */
    float ex_rx[4], ex_ry[4];  /* shadow: radii of the box it is cut out of (ix..ih) */
} DisplayMask;

typedef struct {
    float x, y, w, h;
    float thickness;
} DisplayBorder;

typedef struct {
    float u0, v0, u1, v1;
    uint32_t glyph_index;
    uint8_t font_slot;   /* index into the display-list font table */
} DisplayGlyph;

typedef struct {
    int image_handle;
    float u0, v0, u1, v1;
} DisplayImage;

typedef struct {
    DisplayListCmdType type;
    float x, y, w, h;
    float r, g, b, a;
    union {
        DisplayBorder border;
        DisplayGlyph glyph;
        DisplayImage image;
        int mask_index;
    } u;
} DisplayListCmd;

typedef struct {
    DisplayListCmd *cmds;
    int count;
    int capacity;
    DisplayMask *masks;
    int mask_count, mask_capacity;
} DisplayList;

void display_list_init(DisplayList *dl);
void display_list_free(DisplayList *dl);
bool display_list_reserve(DisplayList *dl, int extra);
bool display_list_add_rect(DisplayList *dl, float x, float y, float w, float h,
                           float r, float g, float b, float a);
bool display_list_add_border(DisplayList *dl, float x, float y, float w, float h,
                             float thickness, float r, float g, float b, float a);
bool display_list_add_glyph(DisplayList *dl, float x, float y, float w, float h,
                            float u0, float v0, float u1, float v1,
                            uint32_t glyph_index,
                            float r, float g, float b, float a);
bool display_list_add_image(DisplayList *dl, float x, float y, float w, float h,
                            int image_handle,
                            float u0, float v0, float u1, float v1);

typedef struct ImageCache ImageCache;

/* Set the image cache used to resolve background images and <img> elements. */
void display_list_set_image_cache(ImageCache *cache);
ImageCache *display_list_get_image_cache(void);

/* Resolved URL an <img> paints (srcset candidate for css_width, else src);
 * malloc'd, NULL when the element has none.  Shared by layout (natural
 * size) and paint so both use the same image. */
struct HtmlNode;
char *display_list_image_source(const char *base_url, struct HtmlNode *node, float css_width);

/* The srcset candidate (malloc'd URL text) for a layout width; NULL if none. */
char *display_list_pick_srcset(const char *srcset, float css_width);

/* Set a TTF font to use when rendering text nodes. Pass NULL to unload. */
bool display_list_set_default_font(const char *ttf_path, float size_pixels);
struct TextShaper;
struct TextShaper *display_list_get_default_font(void);

/* Fonts live in the font registry (font_registry.h); slots index it. */
struct TextShaper *display_list_get_font(int slot);

/* Resolve a CSS font stack + weight + style to a font-table slot. */
int display_list_resolve_font_slot(const char *font_family, int font_weight,
                                   int font_italic);

/* Build a display list from a resolved LayoutContext. */
bool css_layout_build_display_list(LayoutContext *ctx, DisplayList *dl);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_LIST_H */

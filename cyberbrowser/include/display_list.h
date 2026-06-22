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
} DisplayListCmdType;

typedef struct {
    float x, y, w, h;
    float thickness;
} DisplayBorder;

typedef struct {
    float u0, v0, u1, v1;
    uint32_t glyph_index;
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
    } u;
} DisplayListCmd;

typedef struct {
    DisplayListCmd *cmds;
    int count;
    int capacity;
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

/* Set a TTF font to use when rendering text nodes. Pass NULL to unload. */
bool display_list_set_default_font(const char *ttf_path, float size_pixels);
struct TextShaper;
struct TextShaper *display_list_get_default_font(void);

/* Build a display list from a resolved LayoutContext. */
bool css_layout_build_display_list(LayoutContext *ctx, DisplayList *dl);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_LIST_H */

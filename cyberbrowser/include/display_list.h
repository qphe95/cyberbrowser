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

/* Font table: families of 4 slots (regular, bold, italic, bold-italic). */
#define DL_FONT_SANS   0
#define DL_FONT_SERIF  4
#define DL_FONT_MONO   8
#define DL_FONT_SLOTS  12

/* Load a TTF into a font-table slot (replaces any previous occupant). */
bool display_list_set_font(int slot, const char *ttf_path, float size_pixels);
struct TextShaper *display_list_get_font(int slot);

/* Resolve a CSS font stack + weight + style to a font-table slot. */
int display_list_resolve_font_slot(const char *font_family, int font_weight,
                                   int font_italic);

/* Stamp every glyph command emitted since `from_cmd` with `slot` so the
 * rasterizer can pick the right atlas. */
void display_list_stamp_font_slot(DisplayList *dl, int from_cmd, int slot);

/* Build a display list from a resolved LayoutContext. */
bool css_layout_build_display_list(LayoutContext *ctx, DisplayList *dl);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_LIST_H */

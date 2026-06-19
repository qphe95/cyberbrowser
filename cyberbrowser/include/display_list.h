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
    DisplayListCmdType type;
    float x, y, w, h;
    float r, g, b, a;
    union {
        DisplayBorder border;
        DisplayGlyph glyph;
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
                            float r, float g, float b, float a);

/* Build a display list from a resolved LayoutContext. */
bool css_layout_build_display_list(LayoutContext *ctx, DisplayList *dl);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_LIST_H */

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
    /* A 2D transform for the commands after it (to the next one): u.xf, an
     * affine a b c d e f taking their coordinates to the page's; the
     * identity ends it.  x, y, w, h unused (0). */
    DL_XFORM,
} DisplayListCmdType;

#define DL_MASK_STOPS 8

/* A CSS mask over a box: a linear gradient (alpha along a line through the
 * box) or an image whose alpha masks, placed at (ix, iy, iw, ih) and
 * optionally repeated. */
typedef struct {
    float x, y, w, h;          /* the masked box (its border box) */
    int kind;                  /* 1 image, 2 linear gradient, 3 rounded rectangle,
                                  4 rounded border ring (3 minus its inset by border[]),
                                  6 border side: (x, y, w, h) is the side's strip of a box
                                  whose widths are border[], the side angle_deg (0 top,
                                  1 right, 2 bottom, 3 left), covered from stop_pos[0] to
                                  stop_pos[1] of its width (from the outer edge) and cut at
                                  the corners by the diagonals to the inner corners: at its
                                  first end (going clockwise) when stop_alpha[0] is 1, its
                                  second when stop_alpha[1] is (else it fills the corner);
                                  8 mix-blend-mode: what the subtree paints, apart, blended over
                                  what was there by mode angle_deg (the region ix..ih);
                                  9 filter: what the subtree paints, apart, through filter
                                  functions (kinds stop_pos * 32, amounts rx then ry; a
                                  drop-shadow's offsets ix, iy, blur, color border 0..255) */
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
        float xf[6];
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
/* A 2D canvas's solid rectangles (canvas_api.js through __cyberCanvasRect),
 * in its bitmap's pixels: clear removes those it covers. */
void display_list_canvas_rect(uint32_t node, float x, float y, float w, float h, uint32_t rgba, int clear);
/* (and an image drawn whole into [x, x + w) x [y, y + h)) */
void display_list_canvas_image(uint32_t node, const char *url, float x, float y, float w, float h);
/* (a new document: its node ids the last one's again, every canvas's drawing gone) */
void display_list_canvas_reset(void);
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
/* Whether node is an <object> showing an image (its type image/..., or its
 * data an image file): laid out and painted as an <img> of its data. */
bool display_list_object_image(const struct HtmlNode *node);
char *display_list_image_source(const char *base_url, struct HtmlNode *node, float css_width);
/* (image-orientation: none: url as the cache's unturned variant, when the
 * image is of the page's own origin; url freed or returned) */
char *display_list_noexif(char *url, const char *base_url);

/* The srcset candidate (malloc'd URL text) for a layout width; NULL if none. */
char *display_list_pick_srcset(const char *srcset, float css_width);

/* The srcset of the <source> an <img> (store id) in a <picture> takes (HTML
 * 4.8.4.3 "update the source set": the first before it with a srcset whose
 * media matches the viewport and whose type is an image's); NULL when it is
 * not in a <picture> or none does. */
const char *display_list_picture_srcset(uint32_t img);
/* The viewport media queries of <source> are evaluated against (the last
 * layout's). */
void display_list_set_media_viewport(double vw, double vh);

/* Set a TTF font to use when rendering text nodes. Pass NULL to unload. */
bool display_list_set_default_font(const char *ttf_path, float size_pixels);
struct TextShaper;
struct TextShaper *display_list_get_default_font(void);

/* Fonts live in the font registry (font_registry.h); slots index it. */
struct TextShaper *display_list_get_font(int slot);

/* Resolve a CSS font stack + weight + style to a font-table slot. */
/* Sets the text shaper's font-family chain (the list's other fonts, for a
 * character the first lacks) for a box's font-family; 0 when it has one font. */
/* Where scrolling pushes sticky box a now (no page scroll). */
void display_list_sticky_shift(struct LayoutContext *ctx, int a, float *ox, float *oy);
int display_list_set_font_chain(const char *font_family, int font_weight, int font_italic);
int display_list_resolve_font_slot(const char *font_family, int font_weight,
                                   int font_italic);

/* Custom highlights (CSS.highlights) from script: nh highlights, each its
 * name and priority, in registry order; nseg segments of 4 words each:
 * highlight index, text node (NodeId), UTF-16 start and end. */
void display_list_highlights_set(const char *const *names, const int *prios, int nh, const uint32_t *seg, int nseg);

/* Build a display list from a resolved LayoutContext. */
bool css_layout_build_display_list(LayoutContext *ctx, DisplayList *dl);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_LIST_H */

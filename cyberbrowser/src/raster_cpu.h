#ifndef RASTER_CPU_H
#define RASTER_CPU_H

/* The CPU rasterizer: paints a display list into pixels. */

#include <stdbool.h>
#include <stdint.h>
#include "display_list.h"

/* Where the rasterizer finds what glyph and image commands refer to: a font
 * page of the glyph atlas (8-bit coverage, w x h; a glyph command's
 * font_slot), and an image handle's decoded pixels (w x h x ch, top-down).
 * The page engine's come from the text shaper's atlas and the image cache;
 * the GPU process's from what each tab uploaded.  Either may return false (not there: nothing is drawn). */
typedef struct RasterResources {
    bool (*atlas)(void *user, int slot, const uint8_t **pixels, int *w, int *h);
    bool (*image)(void *user, int handle, const uint8_t **pixels, int *w, int *h, int *ch);
    void *user;
} RasterResources;

/* Paint `dl` at width x height on a white background into `rgb`
 * (width * height * 3 bytes, packed RGB rows, top-down), with the font
 * shaper's glyph atlas and the image cache's images; a frame of the glyph
 * atlas (text_shaper_frame_sent). */
void raster_display_list(const DisplayList *dl, uint8_t *rgb, int width, int height);

/* The same, with resources from `res`. */
void raster_display_list_with(const DisplayList *dl, const RasterResources *res, uint8_t *rgb, int width,
                              int height);

/* Paint `dl` at width x height on a white background and write it to
 * `path` as a JPEG (quality 95). */
bool render_display_list_to_jpg(const DisplayList *dl, const char *path, int width, int height);

#endif

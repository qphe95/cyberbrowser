#ifndef RASTER_CPU_H
#define RASTER_CPU_H

/* The CPU rasterizer: paints a display list into pixels. */

#include <stdbool.h>
#include <stdint.h>
#include "display_list.h"

/* Paint `dl` at width x height on a white background into `rgb`
 * (width * height * 3 bytes, packed RGB rows, top-down). */
void raster_display_list(const DisplayList *dl, uint8_t *rgb, int width, int height);

/* Paint `dl` at width x height on a white background and write it to
 * `path` as a JPEG (quality 95). */
bool render_display_list_to_jpg(const DisplayList *dl, const char *path, int width, int height);

#endif

#ifndef RASTER_CPU_H
#define RASTER_CPU_H

/* The CPU rasterizer: paints a display list into pixels. */

#include <stdbool.h>
#include "display_list.h"

/* Paint `dl` at width x height on a white background and write it to
 * `path` as a JPEG (quality 95). */
bool render_display_list_to_jpg(const DisplayList *dl, const char *path, int width, int height);

#endif

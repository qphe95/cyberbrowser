#ifndef SVG_RENDER_H
#define SVG_RENDER_H

/* SVG rasterization (PlutoSVG, src/third_party/plutosvg) for <img src=.svg>
 * and inline <svg> elements. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Intrinsic size of an SVG document (its width/height, else the viewBox);
 * false when it cannot be parsed. */
bool svg_intrinsic_size(const char *data, size_t len, float *w, float *h);

/* Rasterize an SVG document into non-premultiplied RGBA, w x h pixels
 * (*out is malloc'ed, w*h*4 bytes).  `current_color` (0xRRGGBBAA) resolves
 * CSS currentColor.  w or h <= 0 uses the intrinsic size. */
bool svg_render_rgba(const char *data, size_t len, int w, int h, uint32_t current_color,
                     uint8_t **out, int *out_w, int *out_h);

#endif

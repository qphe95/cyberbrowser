#ifndef TEXT_SHAPER_H
#define TEXT_SHAPER_H

#include <stdbool.h>
#include <stdint.h>
#include "display_list.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque text-shaping context.  Implemented with stb_truetype. */
typedef struct TextShaper TextShaper;

/* Create/destroy a shaper.  `size_pixels` is the em height used for rasterization. */
TextShaper *text_shaper_create(const char *ttf_path, float size_pixels);
void        text_shaper_destroy(TextShaper *shaper);

/* Raw atlas access for GPU upload. */
const uint8_t *text_shaper_atlas_pixels(const TextShaper *shaper);
int            text_shaper_atlas_width(const TextShaper *shaper);
int            text_shaper_atlas_height(const TextShaper *shaper);

/* Measure a UTF-8 string in pixels. */
bool text_shaper_measure(const TextShaper *shaper, const char *utf8,
                         float *out_width, float *out_height);

/* Result of a word-wrap walk.  All values are in pixels, already scaled. */
typedef struct {
    int   lines;         /* number of lines produced (>= 1 when text non-empty) */
    float height;        /* total height consumed: lines * line_advance */
    float last_end_x;    /* absolute x where the last line ends */
    float max_width;     /* widest line width */
    float line_advance;  /* per-line vertical advance used */
} TsWrapResult;

/* Simulate word wrap of `utf8` starting at x (first line limited to
 * `first_width`, continuation lines start at `cont_x` with `cont_width`).
 * `scale` multiplies all glyph metrics (for font sizes other than the
 * shaper's loaded size); `line_advance` is the per-line vertical advance in
 * pixels (already scaled by the caller).  All output x values are absolute. */
bool text_shaper_wrap_measure(const TextShaper *shaper, const char *utf8,
                              float x, float cont_x,
                              float first_width, float cont_width,
                              float scale, float line_advance,
                              TsWrapResult *out);

/* Append DL_GLYPH commands for `utf8`, word-wrapped.  (x, y) is the top-left
 * origin of the first line; continuation lines start at `cont_x`. */
bool text_shaper_wrap_shape(const TextShaper *shaper, const char *utf8,
                            float x, float y, float cont_x,
                            float first_width, float cont_width,
                            float scale, float line_advance,
                            float r, float g, float b, float a,
                            DisplayList *dl, TsWrapResult *out);

/* Append DL_GLYPH commands for a UTF-8 string to a display list, with all
 * metrics multiplied by `scale` (for font sizes other than the loaded size).
 * (x, y) is the top-left origin of the text line. */
bool text_shaper_shape_to_display_list_scaled(const TextShaper *shaper,
                                       const char *utf8,
                                       float x, float y, float scale,
                                       float r, float g, float b, float a,
                                       DisplayList *dl);

/* Append DL_GLYPH commands for a UTF-8 string to a display list.
 * (x, y) is the top-left origin of the text line. */
bool text_shaper_shape_to_display_list(const TextShaper *shaper,
                                       const char *utf8,
                                       float x, float y,
                                       float r, float g, float b, float a,
                                       DisplayList *dl);

#ifdef __cplusplus
}
#endif

#endif /* TEXT_SHAPER_H */

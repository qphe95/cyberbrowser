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

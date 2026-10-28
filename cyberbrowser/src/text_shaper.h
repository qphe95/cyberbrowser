#ifndef TEXT_SHAPER_H
#define TEXT_SHAPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "display_list.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque text-shaping context.  Implemented with stb_truetype. */
typedef struct TextShaper TextShaper;

/* Create/destroy a shaper.  `size_pixels` is the em height used for rasterization. */
TextShaper *text_shaper_create(const char *ttf_path, float size_pixels);
/* The same from sfnt bytes in memory (font `font_index` of a collection);
 * takes ownership of `data` (freed on failure too). */
TextShaper *text_shaper_create_from_memory(uint8_t *data, size_t size, int font_index, float size_pixels);
/* The same with synthetic bold: glyph bitmaps widened by `embolden` pixels
 * (at the loaded size) and advances grown to match. */
TextShaper *text_shaper_create_from_memory_ex(uint8_t *data, size_t size, int font_index, float size_pixels,
                                              int embolden);
/* The same from a read-only view of a font file (MapViewOfFile on Windows),
 * unmapped when the shaper goes (and on failure): the file's pages are
 * shared, not the process's private memory. */
TextShaper *text_shaper_create_from_mapped(uint8_t *view, size_t size, int font_index, float size_pixels);
/* Does the font carry variation axes (fvar)?  Only its default instance is
 * drawn. */
bool text_shaper_is_variable(const TextShaper *shaper);
/* Font-registry slot whose atlas holds this shaper's glyphs (-1 = none). */
void text_shaper_set_slot(TextShaper *shaper, int slot);
int  text_shaper_slot(const TextShaper *shaper);
/* Does the font itself (no fallback) map this codepoint? */
bool text_shaper_has_glyph(const TextShaper *shaper, uint32_t codepoint);
void        text_shaper_destroy(TextShaper *shaper);

/* Raw atlas access for GPU upload. */
const uint8_t *text_shaper_atlas_pixels(const TextShaper *shaper);
int            text_shaper_atlas_width(const TextShaper *shaper);
int            text_shaper_atlas_height(const TextShaper *shaper);
/* Rows 0 .. used - 1 hold every glyph drawn so far. */
int            text_shaper_atlas_used_rows(const TextShaper *shaper);
/* The rows [y0, y1) drawn into since the last call (false: none): what a
 * copy of the atlas elsewhere (the GPU process's) needs again. */
bool           text_shaper_atlas_take_dirty(TextShaper *shaper, int *y0, int *y1);

/* Vertical metrics as fractions of the em (descent positive). */
bool text_shaper_vmetrics(const TextShaper *shaper, float *ascent, float *descent,
                          float *line_gap);

/* CSS `line-height: normal` in pixels at `font_size` (ascent + descent +
 * line gap, each rounded to whole pixels as browsers do), or 0 if unknown. */
float text_shaper_normal_line_height(const TextShaper *shaper, float font_size);

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
 * pixels (already scaled by the caller).  All output x values are absolute.
 * When `cont2_from_line` > 1, continuation lines whose 1-based line number
 * reaches it start at `cont2_x` with `cont2_width` instead — used when a
 * float stops intruding partway through the wrapped run. */
bool text_shaper_wrap_measure(const TextShaper *shaper, const char *utf8,
                              float x, float cont_x,
                              float first_width, float cont_width,
                              float scale, float line_advance,
                              int cont2_from_line, float cont2_x,
                              float cont2_width,
                              TsWrapResult *out);

/* Append DL_GLYPH commands for `utf8`, word-wrapped.  (x, y) is the top-left
 * origin of the first line; continuation lines start at `cont_x`, switching
 * to `cont2_x`/`cont2_width` from line `cont2_from_line` on (when > 1). */
/* Text cut to at most max_lines wrapped lines (same geometry as
 * text_shaper_wrap_measure), the last ending in an ellipsis, as a malloc'd
 * string; NULL when it already fits. */
char *text_shaper_truncate_lines(const TextShaper *shaper, const char *utf8, float x, float cont_x,
                                 float first_width, float cont_width, float scale, int max_lines);

/* One line of text cut to `width` with an ellipsis (malloc'd); NULL when it
 * fits. */
char *text_shaper_truncate_width(const TextShaper *shaper, const char *utf8, float scale, float width);

bool text_shaper_wrap_shape(const TextShaper *shaper, const char *utf8,
                            float x, float y, float cont_x,
                            float first_width, float cont_width,
                            float scale, float line_advance,
                            int cont2_from_line, float cont2_x,
                            float cont2_width,
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

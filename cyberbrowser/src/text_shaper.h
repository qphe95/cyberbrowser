#ifndef TEXT_SHAPER_H
#define TEXT_SHAPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "display_list.h"
#include "glyph_atlas.h"

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
/* Does the font itself (no fallback) map this codepoint? */
bool text_shaper_has_glyph(const TextShaper *shaper, uint32_t codepoint);
void        text_shaper_destroy(TextShaper *shaper);

/* Glyphs are drawn from strikes (a font at one size, in quarter pixels)
 * into one glyph atlas for all fonts (glyph_atlas.h); a glyph command's
 * font_slot is its atlas page.  The atlas, NULL before the first glyph: */
struct GlyphAtlas *text_shaper_glyph_atlas(void);
/* Where codepoint's glyph at size_px is in the atlas, drawn there first if
 * need be; false when it has no bitmap or there is no room (tests). */
bool text_shaper_glyph_rect(const TextShaper *shaper, float size_px, uint32_t codepoint, GaRect *out);
/* A display list on screen uses its glyphs in the current frame (one sent
 * again names them only by atlas place). */
void text_shaper_frame_uses(const DisplayList *dl);
/* `dl` went to the screen (or was rasterized): the frame it makes is the
 * last sent, whose glyphs are not evicted while it can be shown. */
void text_shaper_frame_sent(const DisplayList *dl);

/* The height of "x" and the advance of "0" as fractions of the em (CSS's
 * ex and ch units); 0 when the font has no such glyph. */
float text_shaper_x_height(const TextShaper *shaper);
float text_shaper_cap_height(const TextShaper *shaper);
float text_shaper_zero_advance(const TextShaper *shaper);
/* (the same glyph's vertical advance, upright in vertical text: of the em, 1 without vertical metrics) */
float text_shaper_zero_vadvance(const TextShaper *shaper);
/* (codepoint cp's advance, of the em, vertical or not; -1 without a glyph) */
float text_shaper_cp_advance(const TextShaper *s, uint32_t cp, bool vert);
bool text_shaper_is_mark(uint32_t cp);   /* (a combining mark: in its base's cluster) */
/* (text-orientation: upright in vertical text, for what's measured and drawn next: 1 on) */
void text_shaper_set_upright(int on);
/* The underline's top below the baseline and its thickness, the strikeout's
 * top above it and its thickness, as fractions of the em (post, OS/2). */
bool text_shaper_deco_metrics(const TextShaper *shaper, float *ul_pos, float *ul_th, float *st_pos, float *st_th);

/* Vertical metrics as fractions of the em (descent positive). */
/* (@font-face metric overrides: fractions of the em, a negative one kept as it is) */
void text_shaper_override_vmetrics(TextShaper *shaper, float ascent, float descent, float line_gap);
bool text_shaper_vmetrics(const TextShaper *shaper, float *ascent, float *descent,
                          float *line_gap);

/* CSS `line-height: normal` in pixels at `font_size` (ascent + descent +
 * line gap, each rounded to whole pixels as browsers do), or 0 if unknown. */
float text_shaper_normal_line_height(const TextShaper *shaper, float font_size);

/* Measure a UTF-8 string in pixels. */
/* Letter and word spacing (CSS letter-spacing / word-spacing, in pixels of
 * text drawn at scale) added to every advance this thread measures or draws
 * after the call, until it is set again (0, 0: none). */
void text_shaper_set_spacing(float letter_px, float word_px, float scale);
/* OpenType features (4-byte tags, as 'smcp' = 0x736d6370) the text measured
 * and drawn next takes: their single and alternate substitutions; n = 0 none. */
void text_shaper_set_features(const uint32_t *tags, int n);
/* (the palette color glyphs use on this thread: kind 0 normal, 1 light, 2 dark, 3 palette index; n overrides) */
void text_shaper_set_palette(int kind, int index, const int *over_index, const float (*over_rgba)[4], int n);
/* The other fonts of the element's font-family list, tried in order for a
 * character the font lacks before the registry's fallbacks (n 0: none). */
void text_shaper_set_chain(TextShaper *const *fonts, int n);
/* text-autospace for what's measured and drawn next: 1 ideograph-alpha, 2 ideograph-numeric. */
void text_shaper_set_autospace(int mode);
/* (the character before what's measured or drawn next, for text-autospace) */
void text_shaper_set_autospace_prev(uint32_t cp);

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

/* One line of a run as text_shaper_wrap_shape draws it: the bytes
 * [start, end) of the text, drawn from x (the pen at the line's start)
 * with the em box's top at y (the line's top plus the half-leading),
 * advancing width. */
typedef struct { uint32_t start, end; float x, y, width; } TsLine;

/* The lines of utf8 wrapped as text_shaper_wrap_measure wraps it (same
 * arguments), into lines[0, cap): how many there are (when more than cap,
 * the first cap are stored). */
int text_shaper_wrap_lines(const TextShaper *shaper, const char *utf8,
                           float x, float y, float cont_x,
                           float first_width, float cont_width,
                           float scale, float line_advance,
                           int cont2_from_line, float cont2_x,
                           float cont2_width,
                           TsLine *lines, int cap);

/* DL_GLYPH commands for the bytes [start, end) of utf8 on one line: the pen
 * from x, the em box's top at y (a TsLine's x and y, made absolute).  rtl:
 * the code points drawn last first, mirrored (a right-to-left run). */
/* The advance of the bytes [start, end) of utf8 on one line (the spacing
 * text_shaper_set_spacing set included). */
float text_shaper_span_width(const TextShaper *shaper, const char *utf8, size_t start, size_t end, float scale);

bool text_shaper_shape_span(const TextShaper *shaper, const char *utf8, size_t start, size_t end,
                            float x, float y, float scale, bool rtl,
                            float r, float g, float b, float a, DisplayList *dl);

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

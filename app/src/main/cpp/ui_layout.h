#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_GLYPH_W 8
#define FONT_GLYPH_H 8
#define FONT_COLS 16
#define FONT_ROWS 6
#define FONT_ATLAS_W (FONT_COLS * FONT_GLYPH_W)
#define FONT_ATLAS_H (FONT_ROWS * FONT_GLYPH_H)

typedef struct Vertex {
    float pos[2];
    float uv[2];
    float color[3];
} Vertex;

extern const uint8_t font8x8_basic[128][8];

void build_font_atlas(uint8_t *atlas);

float string_width(const char *text, float glyphW);

void append_glyph(Vertex *vertices, uint32_t *count, float x, float y,
                  float w, float h, float u0, float v0, float u1, float v1,
                  float screenW, float screenH, bool mirrorX,
                  float r, float g, float b);

float draw_string(Vertex *vertices, uint32_t *count, uint32_t capacity,
                  const char *text, float x, float y,
                  float glyphW, float glyphH,
                  float screenW, float screenH, bool mirrorX,
                  float r, float g, float b);

float draw_wrapped_text(Vertex *vertices, uint32_t *count, uint32_t capacity,
                        const char *text, float x, float y,
                        float glyphW, float glyphH, float maxW,
                        float screenW, float screenH, bool mirrorX,
                        float r, float g, float b);

void append_rect(Vertex *vertices, uint32_t *count,
                 float x, float y, float w, float h,
                 float screenW, float screenH, bool mirrorX,
                 float r, float g, float b);

void append_border(Vertex *vertices, uint32_t *count,
                   float x, float y, float w, float h, float thick,
                   float screenW, float screenH, bool mirrorX,
                   float r, float g, float b);

void append_hline(Vertex *vertices, uint32_t *count,
                  float x, float y, float w, float thick,
                  float screenW, float screenH, bool mirrorX,
                  float r, float g, float b);

void append_vline(Vertex *vertices, uint32_t *count,
                  float x, float y, float h, float thick,
                  float screenW, float screenH, bool mirrorX,
                  float r, float g, float b);

/* Generate vertices for a single line of test text at a fixed position.
   Returns the number of vertices written. */
uint32_t generate_test_text(Vertex *vertices, uint32_t capacity,
                            const char *text, float x, float y,
                            float densityScale, float screenW, float screenH,
                            float r, float g, float b);

#ifdef __cplusplus
}
#endif

#endif /* UI_LAYOUT_H */

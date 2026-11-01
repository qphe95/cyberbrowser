#ifndef GLYPH_ATLAS_H
#define GLYPH_ATLAS_H

/* One glyph atlas for every font and size a tab draws (FONT_PIPELINE_DESIGN.md):
 * A8 pages of size x size pixels, shelf allocation, a dirty rectangle per page
 * for upload, and a frame stamp per entry.  An allocation that finds no room
 * evicts whole shelves none of whose entries were used since before the last
 * frame sent (least recently used first), then adds a page while under the
 * budget; past that it fails and the caller draws something else.
 *
 * Entries are named by a GaRef; an evicted entry's ref stops resolving.  Not
 * thread-safe: glyphs are drawn on one thread. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GLYPH_ATLAS_MAX_PAGES 16

typedef struct GlyphAtlas GlyphAtlas;

typedef struct {
    int32_t  id;     /* -1: none */
    uint32_t gen;
} GaRef;

typedef struct {
    int page;
    int x, y, w, h;
} GaRect;

/* `budget` pages at most (1 .. GLYPH_ATLAS_MAX_PAGES). */
GlyphAtlas *glyph_atlas_create(int page_size, int budget);
void        glyph_atlas_destroy(GlyphAtlas *a);

/* Room for a w x h bitmap (with empty texels all around it), cleared,
 * marked dirty and stamped with the current frame; false when there is none
 * within the budget. */
bool glyph_atlas_alloc(GlyphAtlas *a, int w, int h, GaRef *ref, GaRect *out);
/* Where a live entry is, stamped with the current frame; false once it was
 * evicted. */
bool glyph_atlas_get(GlyphAtlas *a, GaRef ref, GaRect *out);

/* The shelf holding row y of a page counts as used by the current frame (a
 * display list sent again names its glyphs by page and position only). */
void glyph_atlas_touch(GlyphAtlas *a, int page, int y);
/* The current frame went to the screen: what it used stays until a later
 * frame went too.  Starts the next frame. */
void     glyph_atlas_frame_sent(GlyphAtlas *a);
uint32_t glyph_atlas_frame(const GlyphAtlas *a);

int      glyph_atlas_page_size(const GlyphAtlas *a);
int      glyph_atlas_page_count(const GlyphAtlas *a);
uint8_t *glyph_atlas_page_pixels(const GlyphAtlas *a, int page);
/* Rows 0 .. used - 1 hold everything allocated in the page. */
int      glyph_atlas_page_used_rows(const GlyphAtlas *a, int page);
/* The rectangle [x0, x1) x [y0, y1) written since the last call (false:
 * nothing): what a copy of the page elsewhere needs again. */
bool     glyph_atlas_take_dirty(GlyphAtlas *a, int page, int *x0, int *y0, int *x1, int *y1);

/* Shelves evicted so far (tests, logs). */
uint64_t glyph_atlas_evictions(const GlyphAtlas *a);

#ifdef __cplusplus
}
#endif

#endif /* GLYPH_ATLAS_H */

/* The glyph atlas (src/glyph_atlas.c) and the text shaper's strikes drawing
 * into it: allocation, strike lookup, eviction order. */
#include "test_runner.h"
#include "glyph_atlas.h"
#include "text_shaper.h"
#include "font_registry.h"
#include "display_list.h"
#include "raster_cpu.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

void run_glyph_atlas_tests(void);

static bool overlaps(const GaRect *a, const GaRect *b)
{
    /* With the pixel of gutter right and below each. */
    return a->page == b->page && a->x < b->x + b->w + 1 && b->x < a->x + a->w + 1 && a->y < b->y + b->h + 1 &&
           b->y < a->y + a->h + 1;
}

TEST(test_glyph_atlas_alloc_packs_without_overlap) {
    GlyphAtlas *a = glyph_atlas_create(256, 2);
    ASSERT_TRUE(a != NULL);
    ASSERT_TRUE(glyph_atlas_page_count(a) == 0);
    GaRef refs[400];
    GaRect rects[400];
    int n = 0;
    for (int i = 0; i < 400; i++) {
        int w = 3 + (i * 7) % 20, h = 4 + (i * 5) % 17;
        if (!glyph_atlas_alloc(a, w, h, &refs[n], &rects[n])) break;
        ASSERT_TRUE(rects[n].w == w && rects[n].h == h);
        ASSERT_TRUE(rects[n].x >= 1 && rects[n].x + w < 256 && rects[n].y >= 1 && rects[n].y + h < 256);
        n++;
    }
    ASSERT_TRUE(n == 400);
    ASSERT_TRUE(glyph_atlas_page_count(a) >= 1 && glyph_atlas_page_count(a) <= 2);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) ASSERT_TRUE(!overlaps(&rects[i], &rects[j]));
    /* Every entry still where it was; dirty covers what was drawn. */
    for (int i = 0; i < n; i++) {
        GaRect r;
        ASSERT_TRUE(glyph_atlas_get(a, refs[i], &r));
        ASSERT_TRUE(memcmp(&r, &rects[i], sizeof(r)) == 0);
    }
    int x0, y0, x1, y1;
    ASSERT_TRUE(glyph_atlas_take_dirty(a, 0, &x0, &y0, &x1, &y1));
    ASSERT_TRUE(x0 == 1 && y0 == 1 && y1 <= glyph_atlas_page_used_rows(a, 0));
    ASSERT_TRUE(!glyph_atlas_take_dirty(a, 0, &x0, &y0, &x1, &y1));
    /* Glyphs of one height share shelves: 40 of 10 x 10 in a fresh page
     * take 2 shelves of 12 rows (below row 0). */
    GlyphAtlas *b = glyph_atlas_create(256, 1);
    GaRef r;
    GaRect rc;
    for (int i = 0; i < 40; i++) ASSERT_TRUE(glyph_atlas_alloc(b, 10, 10, &r, &rc));
    ASSERT_TRUE(glyph_atlas_page_used_rows(b, 0) == 25);
    /* Too big for a page: no. */
    ASSERT_TRUE(!glyph_atlas_alloc(b, 255, 10, &r, &rc));
    ASSERT_TRUE(!glyph_atlas_alloc(b, 0, 10, &r, &rc));
    glyph_atlas_destroy(b);
    glyph_atlas_destroy(a);
    return true;
}

TEST(test_glyph_atlas_budget_without_frames) {
    /* Nothing was ever sent: nothing may be evicted, so the budget ends it. */
    GlyphAtlas *a = glyph_atlas_create(65, 2);
    GaRef r;
    GaRect rc;
    int n = 0;
    while (n < 100 && glyph_atlas_alloc(a, 62, 7, &r, &rc)) n++;
    ASSERT_TRUE(n == 16);                         /* 8 shelves of 8 rows a page */
    ASSERT_TRUE(glyph_atlas_page_count(a) == 2);
    ASSERT_TRUE(glyph_atlas_evictions(a) == 0);
    glyph_atlas_destroy(a);
    return true;
}

TEST(test_glyph_atlas_eviction_order) {
    /* One page of 4 shelves (8 rows, below row 0), one full-width entry each. */
    GlyphAtlas *a = glyph_atlas_create(33, 1);
    GaRef ra, rb, rc, rd, rx;
    GaRect g;
    ASSERT_TRUE(glyph_atlas_alloc(a, 30, 7, &ra, &g));
    ASSERT_TRUE(glyph_atlas_alloc(a, 30, 7, &rb, &g));
    ASSERT_TRUE(glyph_atlas_alloc(a, 30, 7, &rc, &g));
    ASSERT_TRUE(glyph_atlas_alloc(a, 30, 7, &rd, &g));
    glyph_atlas_frame_sent(a);                    /* frame 1: a b c d */
    ASSERT_TRUE(glyph_atlas_get(a, rc, &g));
    glyph_atlas_frame_sent(a);                    /* frame 2: c */
    ASSERT_TRUE(glyph_atlas_get(a, ra, &g));
    glyph_atlas_frame_sent(a);                    /* frame 3: a */
    ASSERT_TRUE(glyph_atlas_get(a, rd, &g));
    glyph_atlas_frame_sent(a);                    /* frame 4 (the last sent): d */
    /* Least recently used first: b (1), c (2), a (3); d was in the last
     * frame sent and stays. */
    ASSERT_TRUE(glyph_atlas_alloc(a, 30, 7, &rx, &g));
    ASSERT_TRUE(!glyph_atlas_get(a, rb, &g) && glyph_atlas_get(a, rc, &g) == true);
    glyph_atlas_frame_sent(a);                    /* frame 5: x c */
    ASSERT_TRUE(glyph_atlas_get(a, rx, &g) && glyph_atlas_get(a, rc, &g));
    /* now stamps: a 3, d 4, x c 6 (current); last sent 5 */
    ASSERT_TRUE(glyph_atlas_alloc(a, 30, 7, &rb, &g));
    ASSERT_TRUE(!glyph_atlas_get(a, ra, &g));
    ASSERT_TRUE(glyph_atlas_alloc(a, 30, 7, &ra, &g));
    ASSERT_TRUE(!glyph_atlas_get(a, rd, &g));
    /* Everything left is the current frame's: no room. */
    ASSERT_TRUE(!glyph_atlas_alloc(a, 30, 7, &rd, &g));
    ASSERT_TRUE(glyph_atlas_get(a, rx, &g) && glyph_atlas_get(a, rc, &g) && glyph_atlas_get(a, ra, &g) &&
                glyph_atlas_get(a, rb, &g));
    ASSERT_TRUE(glyph_atlas_evictions(a) == 3);
    glyph_atlas_destroy(a);
    return true;
}

TEST(test_glyph_atlas_never_evicts_last_sent_frame) {
    GlyphAtlas *a = glyph_atlas_create(33, 1);
    GaRef r[4], x;
    GaRect g;
    for (int i = 0; i < 4; i++) ASSERT_TRUE(glyph_atlas_alloc(a, 30, 7, &r[i], &g));
    glyph_atlas_frame_sent(a);
    /* The frame just sent used all four: none goes, though this frame uses
     * none of them. */
    ASSERT_TRUE(!glyph_atlas_alloc(a, 30, 7, &x, &g));
    for (int i = 0; i < 4; i++) ASSERT_TRUE(glyph_atlas_get(a, r[i], &g));
    glyph_atlas_frame_sent(a);                    /* frame 2 used all four again */
    /* A display list sent again names glyphs by place: touching the rows of
     * r[1] and r[2] keeps them, the others may go. */
    glyph_atlas_touch(a, 0, 9);
    glyph_atlas_touch(a, 0, 21);
    glyph_atlas_frame_sent(a);                    /* frame 3: r1 r2 */
    ASSERT_TRUE(glyph_atlas_alloc(a, 30, 7, &x, &g));
    ASSERT_TRUE(glyph_atlas_alloc(a, 30, 7, &x, &g));
    ASSERT_TRUE(!glyph_atlas_alloc(a, 30, 7, &x, &g));
    ASSERT_TRUE(!glyph_atlas_get(a, r[0], &g) && !glyph_atlas_get(a, r[3], &g));
    ASSERT_TRUE(glyph_atlas_get(a, r[1], &g) && glyph_atlas_get(a, r[2], &g));
    glyph_atlas_destroy(a);
    return true;
}

TEST(test_glyph_atlas_eviction_merges_shelves) {
    GlyphAtlas *a = glyph_atlas_create(33, 1);
    GaRef r[4], tall;
    GaRect g;
    for (int i = 0; i < 4; i++) ASSERT_TRUE(glyph_atlas_alloc(a, 30, 7, &r[i], &g));
    glyph_atlas_frame_sent(a);
    ASSERT_TRUE(glyph_atlas_get(a, r[2], &g) && glyph_atlas_get(a, r[3], &g));
    glyph_atlas_frame_sent(a);
    glyph_atlas_frame_sent(a);                    /* r0 r1 at 1, r2 r3 at 2; last sent 3 */
    int x0, y0, x1, y1;
    ASSERT_TRUE(glyph_atlas_take_dirty(a, 0, &x0, &y0, &x1, &y1));   /* (the first upload) */
    ASSERT_TRUE(glyph_atlas_get(a, r[2], &g) && glyph_atlas_get(a, r[3], &g));
    /* 15 rows need 16: the two old shelves together. */
    ASSERT_TRUE(glyph_atlas_alloc(a, 20, 15, &tall, &g));
    ASSERT_TRUE(g.y == 1 && g.h == 15);
    ASSERT_TRUE(!glyph_atlas_get(a, r[0], &g) && !glyph_atlas_get(a, r[1], &g));
    ASSERT_TRUE(glyph_atlas_get(a, r[2], &g) && glyph_atlas_get(a, r[3], &g));
    /* An evicted shelf is cleared (and sent again). */
    ASSERT_TRUE(glyph_atlas_take_dirty(a, 0, &x0, &y0, &x1, &y1));
    ASSERT_TRUE(y0 == 1 && y1 == 17);
    glyph_atlas_destroy(a);
    return true;
}

/* ---- strikes ---- */

static TextShaper *default_font(void)
{
    return font_registry_slot(font_registry_default_slot());
}

TEST(test_strike_lookup) {
    TextShaper *f = default_font();
    ASSERT_TRUE(f != NULL);
    GaRect a1, a2, b, c, d;
    ASSERT_TRUE(text_shaper_glyph_rect(f, 16.0f, 'H', &a1));
    ASSERT_TRUE(text_shaper_glyph_rect(f, 16.0f, 'H', &a2));
    ASSERT_TRUE(memcmp(&a1, &a2, sizeof(a1)) == 0);           /* found, not drawn again */
    ASSERT_TRUE(text_shaper_glyph_rect(f, 32.0f, 'H', &b));
    ASSERT_TRUE(!(b.page == a1.page && b.x == a1.x && b.y == a1.y));
    ASSERT_TRUE(b.h > a1.h * 3 / 2);                           /* drawn at its own size */
    /* Sizes are kept in quarter pixels: 16.05 is 16's strike, 16.25 is not. */
    ASSERT_TRUE(text_shaper_glyph_rect(f, 16.05f, 'H', &c));
    ASSERT_TRUE(memcmp(&c, &a1, sizeof(c)) == 0);
    ASSERT_TRUE(text_shaper_glyph_rect(f, 16.25f, 'H', &d));
    ASSERT_TRUE(!(d.page == a1.page && d.x == a1.x && d.y == a1.y));
    /* A space has no bitmap. */
    ASSERT_TRUE(!text_shaper_glyph_rect(f, 16.0f, ' ', &d));
    /* No cap on sizes (it was 16 per font). */
    for (int i = 0; i < 64; i++) ASSERT_TRUE(text_shaper_glyph_rect(f, 40.0f + (float)i * 0.25f, 'e', &d));
    return true;
}

static uint64_t image_hash(const uint8_t *px, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= px[i]; h *= 1099511628211ull; }
    return h;
}

#define STRESS_SIZES 200
#define STRESS_W 900
#define STRESS_H 140

/* Text at `px` (the shaper's size times a scale) painted alone. */
static uint64_t paint_size(TextShaper *f, float px, uint8_t *rgb)
{
    DisplayList dl;
    display_list_init(&dl);
    text_shaper_shape_to_display_list_scaled(f, "Hamburgefonstiv", 2.0f, 2.0f, px / 16.0f, 0, 0, 0, 1, &dl);
    raster_display_list(&dl, rgb, STRESS_W, STRESS_H);   /* (a frame: text_shaper_frame_sent) */
    display_list_free(&dl);
    return image_hash(rgb, (size_t)STRESS_W * STRESS_H * 3);
}

TEST(test_strikes_many_sizes_draw_the_same) {
    TextShaper *f = default_font();
    ASSERT_TRUE(f != NULL);
    uint8_t *rgb = (uint8_t *)malloc((size_t)STRESS_W * STRESS_H * 3);
    uint64_t *first = (uint64_t *)calloc(STRESS_SIZES, sizeof(uint64_t));
    ASSERT_TRUE(rgb && first);
    /* 200 sizes, each in a frame of its own, twice: the second time, after
     * all the others (and the evictions they forced), each size draws
     * exactly as it did the first time. */
    for (int i = 0; i < STRESS_SIZES; i++) first[i] = paint_size(f, 5.0f + (float)i * 0.5f, rgb);
    uint64_t ev = glyph_atlas_evictions(text_shaper_glyph_atlas());
    for (int i = STRESS_SIZES - 1; i >= 0; i--) {
        uint64_t again = paint_size(f, 5.0f + (float)i * 0.5f, rgb);
        if (again != first[i]) printf("    size %.1f draws differently\n", 5.0f + (float)i * 0.5f);
        ASSERT_TRUE(again == first[i]);
    }
    ASSERT_TRUE(glyph_atlas_page_count(text_shaper_glyph_atlas()) <= 4);
    ASSERT_TRUE(glyph_atlas_evictions(text_shaper_glyph_atlas()) > ev);   /* the budget was reached */
    /* All 200 in one frame is more than the budget holds: the text still
     * draws (scaled glyphs for what does not fit), and the next frame on
     * its own draws crisp again. */
    DisplayList dl;
    display_list_init(&dl);
    for (int i = 0; i < STRESS_SIZES; i++)
        text_shaper_shape_to_display_list_scaled(f, "Hamburgefonstiv", 2.0f, 2.0f, (5.0f + (float)i * 0.5f) / 16.0f,
                                                 0, 0, 0, 1, &dl);
    int glyphs = 0;
    for (int i = 0; i < dl.count; i++) glyphs += dl.cmds[i].type == DL_GLYPH && dl.cmds[i].w > 0;
    ASSERT_TRUE(glyphs >= STRESS_SIZES * 14);
    raster_display_list(&dl, rgb, STRESS_W, STRESS_H);
    display_list_free(&dl);
    paint_size(f, 60.0f, rgb);                        /* (in the last frame sent: no eviction yet) */
    for (int i = 0; i < STRESS_SIZES; i += 37)
        ASSERT_TRUE(paint_size(f, 5.0f + (float)i * 0.5f, rgb) == first[i]);
    free(first);
    free(rgb);
    return true;
}

void run_glyph_atlas_tests(void) {
    printf("\n--- Glyph atlas tests ---\n");
    RUN_TEST(test_glyph_atlas_alloc_packs_without_overlap);
    RUN_TEST(test_glyph_atlas_budget_without_frames);
    RUN_TEST(test_glyph_atlas_eviction_order);
    RUN_TEST(test_glyph_atlas_never_evicts_last_sent_frame);
    RUN_TEST(test_glyph_atlas_eviction_merges_shelves);
    RUN_TEST(test_strike_lookup);
    RUN_TEST(test_strikes_many_sizes_draw_the_same);
}

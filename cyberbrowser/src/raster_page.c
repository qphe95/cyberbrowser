/*
 * raster_page.c - the CPU rasterizer with the page engine's resources
 * (raster_cpu.h): the text shaper's glyph atlas and the image cache's images.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "raster_cpu.h"
#include "display_list.h"
#include "text_shaper.h"
#include "image_cache.h"
#include "stb_image_write.h"

typedef struct {
    uint8_t r, g, b;
} RGB;

/* The page engine's resources: the glyph atlas's pages and the image cache. */
static bool page_atlas(void *user, int page, const uint8_t **pixels, int *w, int *h) {
    (void)user;
    GlyphAtlas *a = text_shaper_glyph_atlas();
    *pixels = glyph_atlas_page_pixels(a, page);
    *w = *h = glyph_atlas_page_size(a);
    return *pixels != NULL;
}

static bool cache_image(void *user, int handle, const uint8_t **pixels, int *w, int *h, int *ch) {
    (void)user;
    ImageCache *cache = display_list_get_image_cache();
    uint8_t *px = NULL;
    if (!cache || !image_cache_get(cache, handle, w, h, ch, &px)) return false;
    *pixels = px;
    return true;
}

void raster_display_list(const DisplayList *dl, uint8_t *rgb, int img_width, int img_height) {
    RasterResources res = { page_atlas, cache_image, NULL };
    raster_display_list_with(dl, &res, rgb, img_width, img_height);
    text_shaper_frame_sent(dl);
}

bool render_display_list_to_jpg(const DisplayList *dl, const char *path,
                                int img_width, int img_height) {
    RGB *pixels = (RGB *)calloc((size_t)img_width * (size_t)img_height, sizeof(RGB));
    if (!pixels) {
        printf("FATAL: out of memory allocating %dx%d image buffer\n", img_width, img_height);
        return false;
    }
    raster_display_list(dl, (uint8_t *)pixels, img_width, img_height);
    /* stbi_write_jpg expects interleaved RGB. */
    int ok = stbi_write_jpg(path, img_width, img_height, 3, pixels, 95);
    free(pixels);
    return ok != 0;
}

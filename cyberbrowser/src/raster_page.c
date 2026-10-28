/*
 * raster_page.c - the CPU rasterizer with the page engine's resources
 * (raster_cpu.h): the font registry's atlases and the image cache's images.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "raster_cpu.h"
#include "display_list.h"
#include "font_registry.h"
#include "text_shaper.h"
#include "image_cache.h"
#include "stb_image_write.h"

typedef struct {
    uint8_t r, g, b;
} RGB;

/* The page engine's resources: the font registry and the image cache. */
static bool registry_atlas(void *user, int slot, const uint8_t **pixels, int *w, int *h) {
    (void)user;
    TextShaper *f = slot >= 0 && slot < FONT_SLOTS_MAX ? font_registry_slot(slot) : NULL;
    if (!f) return false;
    *pixels = text_shaper_atlas_pixels(f);
    *w = text_shaper_atlas_width(f);
    *h = text_shaper_atlas_height(f);
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
    RasterResources res = { registry_atlas, cache_image, NULL };
    raster_display_list_with(dl, &res, rgb, img_width, img_height);
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

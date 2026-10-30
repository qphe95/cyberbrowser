/* lib_gpu.c - models of what cb_gpu.exe links outside M9 (SANDBOX_DESIGN.md
 * 8.7.1): the CPU rasterizer (raster_cpu.c) and the Vulkan backend
 * (gpu_vk.c).  TRUSTED, like the Windows contracts: each states what the
 * code needs from the shim (valid buffers of the stated sizes, as
 * preconditions Eva checks at the calls) and does what the shim may
 * observe of it (calls the shim's resource callbacks and reads the pixels
 * they return, writes the buffers it is given, allocates its targets and
 * textures), and nothing about how it draws. */
#include <stdlib.h>
#include <windows.h>
#include "gpu_backend.h"
#include "__fc_builtin.h"

/* Reads the pixels a callback hands out: w * h * channels bytes. */
static void read_pixels(const uint8_t *px, int w, int h, int ch) {
    if (!px || w <= 0 || h <= 0) return;
    uint8_t first = px[0], last = px[(size_t)w * h * ch - 1];
    (void)first;
    (void)last;
}

/* Asks the shim for any atlas and image the list names (each slot or id
 * the display list carries, as the rasterizer does), reads them, and
 * writes rgb (width * height * 3 bytes). */
/*@ requires dl: \valid_read(dl) && 0 <= dl->count && 0 <= dl->mask_count;
    requires cmds: dl->count > 0 ==> \valid_read(dl->cmds + (0 .. dl->count - 1));
    requires masks: dl->mask_count > 0 ==> \valid_read(dl->masks + (0 .. dl->mask_count - 1));
    requires res: \valid_read(res);
    requires size: 0 < width && 0 < height;
    requires rgb: \valid(rgb + (0 .. (size_t)width * height * 3 - 1)); */
void raster_display_list_with(const DisplayList *dl, const RasterResources *res, uint8_t *rgb, int width,
                              int height) {
    for (int i = 0; i < dl->count; i++) {
        const DisplayListCmd *c = &dl->cmds[i];
        const uint8_t *px = NULL;
        int w = 0, h = 0, ch = 0;
        if (c->type == DL_GLYPH && res->atlas(res->user, c->u.glyph.font_slot, &px, &w, &h)) read_pixels(px, w, h, 1);
        if (c->type == DL_IMAGE && res->image(res->user, c->u.image.image_handle, &px, &w, &h, &ch))
            read_pixels(px, w, h, ch);
    }
    for (int i = 0; i < dl->mask_count; i++) {
        const uint8_t *px = NULL;
        int w = 0, h = 0, ch = 0;
        if (dl->masks[i].image_handle >= 0 && res->image(res->user, dl->masks[i].image_handle, &px, &w, &h, &ch))
            read_pixels(px, w, h, ch);
    }
    rgb[0] = (uint8_t)Frama_C_interval(0, 255);
    rgb[(size_t)width * height * 3 - 1] = (uint8_t)Frama_C_interval(0, 255);
}

/* ---- the Vulkan backend --------------------------------------------------------- */

typedef struct FcTexture { bool rgba; uint32_t w, h; } FcTexture;
typedef struct FcTarget { uint32_t w, h; } FcTarget;

/*@ requires size: 0 < w <= 16384 && 0 < h <= 16384; */
static GpuTexture *vk_texture_new(bool rgba, uint32_t w, uint32_t h) {
    FcTexture *t = (FcTexture *)malloc(sizeof(FcTexture));
    if (t && Frama_C_nondet(0, 1)) { free(t); t = NULL; }     /* no device memory */
    if (!t) return NULL;
    t->rgba = rgba;
    t->w = w;
    t->h = h;
    return (GpuTexture *)(void *)t;
}

/* Rows y0 .. y0 + rows - 1 of the texture, from pixels (rows of its width,
 * 4 or 1 bytes a pixel). */
/*@ requires tex: \valid_read((FcTexture *)(void *)t);
    requires rows: y0 + (uint64_t)rows <= ((FcTexture *)(void *)t)->h;
    requires pixels: rows > 0 ==> \valid_read(pixels + (0 .. (size_t)rows * ((FcTexture *)(void *)t)->w *
                                                             (((FcTexture *)(void *)t)->rgba ? 4 : 1) - 1)); */
static void vk_texture_update(GpuTexture *t, uint32_t y0, uint32_t rows, const uint8_t *pixels) {
    FcTexture *x = (FcTexture *)(void *)t;
    (void)y0;
    if (rows) read_pixels(pixels, (int)x->w, (int)rows, x->rgba ? 4 : 1);
}

/*@ requires tex: t == \null || \freeable(t); */
static void vk_texture_free(GpuTexture *t) { free(t); }

/* Makes or resizes *target to w x h; draws the list with the textures
 * (and res for what has none). */
/*@ requires target: \valid(target);
    requires size: 0 < w && 0 < h;
    requires dl: \valid_read(dl);
    requires textures: \valid_read(atlases) && \valid_read(images);
    requires res: \valid_read(res); */
static bool vk_render(GpuTarget **target, uint32_t w, uint32_t h, const DisplayList *dl, GpuTexture *const *atlases,
                      GpuTexture *const *images, const RasterResources *res) {
    (void)atlases;
    (void)images;
    (void)res;
    (void)dl;
    FcTarget *t = (FcTarget *)(void *)*target;
    if (!t || t->w != w || t->h != h) {
        free(t);
        *target = NULL;
        t = (FcTarget *)malloc(sizeof(FcTarget));
        if (!t) return false;
        t->w = w;
        t->h = h;
        *target = (GpuTarget *)(void *)t;
    }
    return Frama_C_nondet(0, 1);
}

/*@ requires t: t == \null || \freeable(t); */
static void vk_target_free(GpuTarget *t) { free(t); }

/*@ requires t: \valid_read((FcTarget *)(void *)t); */
static bool vk_present(HWND hwnd, uint32_t w, uint32_t h, GpuTarget *t, int32_t src_y) {
    (void)hwnd; (void)w; (void)h; (void)t; (void)src_y;
    return Frama_C_nondet(0, 1);
}

/* The target's pixels into out: its w * h * 4 bytes. */
/*@ requires t: \valid_read((FcTarget *)(void *)t);
    requires out: \valid(out + (0 .. (size_t)((FcTarget *)(void *)t)->w * ((FcTarget *)(void *)t)->h * 4 - 1));
    requires wh: \valid(w) && \valid(h); */
static bool vk_readback(GpuTarget *t, uint8_t *out, uint32_t *w, uint32_t *h) {
    FcTarget *x = (FcTarget *)(void *)t;
    size_t n = (size_t)x->w * x->h * 4;
    if (n) {
        out[0] = 0;
        out[n - 1] = 0;
    }
    *w = x->w;
    *h = x->h;
    return Frama_C_nondet(0, 1);
}

static void vk_window_gone(HWND hwnd) { (void)hwnd; }

static const GpuBackend g_vk = {
    "vulkan", vk_texture_new, vk_texture_update, vk_texture_free, vk_render, vk_target_free, vk_present,
    vk_readback, vk_window_gone,
};

/* NULL when there is no usable Vulkan. */
const GpuBackend *gpu_vk_backend(void) {
    return Frama_C_nondet(0, 1) ? &g_vk : NULL;
}

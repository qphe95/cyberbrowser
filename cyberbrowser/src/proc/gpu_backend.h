#ifndef GPU_BACKEND_H
#define GPU_BACKEND_H

/* The GPU process's rasterizers (SANDBOX_DESIGN.md section 5.3): Vulkan
 * (gpu_vk.c), or the CPU rasterizer when there is no usable GPU
 * (gpu_main.c).  Both draw a tab's decoded display list into that tab's own
 * target, and only compositing puts a target in a window. */

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>
#include "display_list.h"
#include "raster_cpu.h"

typedef struct GpuTarget GpuTarget;         /* a tab's rendered content */
typedef struct GpuTexture GpuTexture;       /* an atlas (8-bit) or an image (RGBA) */

typedef struct GpuBackend {
    const char *name;
    /* Textures for a tab's resources; NULL if the backend draws from the
     * CPU copies (RasterResources) instead. */
    GpuTexture *(*texture_new)(bool rgba, uint32_t w, uint32_t h);
    void (*texture_update)(GpuTexture *t, uint32_t y0, uint32_t rows, const uint8_t *pixels);
    void (*texture_free)(GpuTexture *t);
    /* Draw dl into *target (made or resized to w x h): textures[] by font
     * slot and images[] by image id, res for what has no texture. */
    bool (*render)(GpuTarget **target, uint32_t w, uint32_t h, const DisplayList *dl,
                   GpuTexture *const *atlases, GpuTexture *const *images, const RasterResources *res);
    void (*target_free)(GpuTarget *t);
    /* Show target rows [src_y, src_y + h) in window hwnd (its client area:
     * w x h), white where the target has none. */
    bool (*present)(HWND hwnd, uint32_t w, uint32_t h, GpuTarget *t, int32_t src_y);
    /* The target's pixels, BGRX top-down, into out (its w * h * 4 bytes). */
    bool (*readback)(GpuTarget *t, uint8_t *out, uint32_t *w, uint32_t *h);
    /* Window hwnd shows nothing any more: drop what presenting to it took. */
    void (*window_gone)(HWND hwnd);
} GpuBackend;

/* The Vulkan backend, or NULL if Vulkan can't be used here. */
const GpuBackend *gpu_vk_backend(void);

#endif

#ifndef WEBGPU_BRIDGE_H
#define WEBGPU_BRIDGE_H

/* How the engine reaches a web-GPU process (SANDBOX_DESIGN.md section 5.7):
 * the tab gives this (its gl channel, asked of the coordinator on first
 * use).  Without it WebGL and WebGPU are unavailable. */

#include <stdbool.h>
#include <stdint.h>

typedef struct WebgpuBridge {
    /* The channel, asked for once: false if none can be had. */
    bool (*connect)(void);
    bool (*send)(uint16_t type, const void *a, uint32_t alen, const void *b, uint32_t blen);
    /* The next message (its body valid until the next call); false if the
     * channel broke or nothing came for a while. */
    bool (*recv)(uint16_t *type, const uint8_t **body, uint32_t *len);
} WebgpuBridge;

void webgpu_bridge_set(const WebgpuBridge *b);

/* A canvas presented a frame since last asked (the page needs painting). */
bool webgpu_canvas_dirty_take(void);
/* Whether a canvas has a frame to draw ("cyber-canvas:<node>" in the image cache). */
bool webgpu_canvas_has_frame(uint32_t node);

#endif

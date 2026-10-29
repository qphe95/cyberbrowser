#ifndef WEBGPU_WGPU_H
#define WEBGPU_WGPU_H

/* WebGPU in the web-GPU process (SANDBOX_DESIGN.md section 5.7), on
 * wgpu-native: one tab's adapter, devices and objects (a WgpuClient).
 *
 * A tab's calls come as records (webgpu_wire.h) in TW_GPU_RUN.  Objects are
 * named by ids the tab gives out, and only ever looked up in that tab's
 * table; descriptors come as value trees, parsed with limits and then read
 * field by field into wgpu's structs.  wgpu validates every call, as its
 * WebGPU implementation does in Firefox.  What can wait (mapping a buffer,
 * finished work) is waited for here, so every call is answered in order. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef struct WgpuClient WgpuClient;

/* A tab's WebGPU, with wgpu_native.dll from dir (loaded once). */
WgpuClient *wgpu_client_new(const wchar_t *dir);
void wgpu_client_free(WgpuClient *c);

/* Run records (len bytes), appending results to *out.  False if one was
 * malformed (the rest are dropped). */
bool wgpu_run(WgpuClient *c, const uint8_t *recs, uint32_t len, uint8_t **out, uint32_t *out_len, uint32_t *out_cap);

/* A configured canvas's current texture, RGBA top-down, into *px (grown). */
void wgpu_present(WgpuClient *c, uint32_t canvas, uint8_t **px, size_t *cap, uint32_t *w, uint32_t *h);
void wgpu_resize(WgpuClient *c, uint32_t canvas, uint32_t w, uint32_t h);

/* Callbacks are waited for inside wgpu_run; nothing is left pending. */
static inline void wgpu_poll(WgpuClient *c) { (void)c; }
static inline bool wgpu_pending(WgpuClient *c) { (void)c; return false; }

#endif

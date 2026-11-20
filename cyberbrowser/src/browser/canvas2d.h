/* canvas2d.h - 2D canvas bitmaps (canvas2d.c). */
#ifndef CANVAS2D_H
#define CANVAS2D_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ids of OffscreenCanvas / ImageBitmap bitmaps start here (below: <canvas> NodeIds) */
#define C2D_OFF_BASE 0x100000000ull

/* (a <canvas> whose 2D context has a bitmap) */
bool canvas2d_has(uint32_t id);
/* Its image cache key in key, the bitmap put there when it changed; false: none. */
bool canvas2d_paint_key(uint32_t id, char *key, size_t cap);
/* (drawElementImage of el on canvas: where, in its bitmap's pixels (x, y, w, h; w, h <= 0 its own size), and the bitmap's size) */
bool canvas2d_elem_draw(uint32_t canvas, uint32_t el, float out[4], int *bw, int *bh);
bool canvas2d_elem_src(uint32_t canvas, uint32_t el, float out[4]);
bool canvas2d_elem_fx(uint32_t canvas, uint32_t el, unsigned char *n, unsigned char t[8], float v[8], float ds[7]);
int canvas2d_elem_op(uint32_t canvas, uint32_t el);
/* (every bitmap dropped: a new document) */
void canvas2d_reset_all(void);

#ifdef QUICKJS_H
void canvas2d_api_install(JSContextHandle ctx);
#endif
#endif

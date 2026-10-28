#ifndef GPU_WIRE_H
#define GPU_WIRE_H

/* Display lists as they cross from a tab to the GPU process (SANDBOX_DESIGN.md
 * section 5.2): fixed-size records of integers, no pointers, no floats.  The
 * tab encodes its DisplayList (gpu_encode.c); the GPU process decodes the
 * private copy it received with the verified decoder (gpu_decode.c), which
 * checks every record against the sending tab's own resources.
 *
 * A TG_FRAME message is a GpuFrame, then ncmds WireCmd, then nmasks
 * WireMask.  Lengths are in 1/256 px (WIRE_UNIT), colors and gradient stops
 * in 1/65535, texture coordinates in 1/2^24 of the texture. */

#include <stdint.h>

#define WIRE_UNIT 256               /* 1 px */
#define WIRE_UV_ONE (1u << 24)      /* texture coordinate 1.0 */
#define WIRE_MASK_STOPS 8

/* Command types: DisplayListCmdType's values (display_list.h). */
enum {
    WIRE_RECT = 0,
    WIRE_BORDER,
    WIRE_GLYPH,
    WIRE_IMAGE,
    WIRE_MASK_BEGIN,
    WIRE_MASK_END,
    WIRE_SHADOW,
    WIRE_NTYPES
};

typedef struct GpuFrame {
    uint32_t frame;                 /* the tab's frame number */
    uint32_t width, height;         /* the viewport, px */
    uint32_t ncmds, nmasks;
} GpuFrame;

typedef struct WireCmd {
    uint32_t type;                  /* WIRE_* */
    int32_t x, y, w, h;             /* 1/256 px */
    uint16_t r, g, b, a;            /* 0..65535 */
    int32_t thickness;              /* WIRE_BORDER: 1/256 px */
    uint32_t ref;                   /* GLYPH: font slot; IMAGE: image id; MASK_BEGIN, SHADOW: mask index */
    uint32_t u0, v0, u1, v1;        /* GLYPH, IMAGE: texture coordinates, 0..WIRE_UV_ONE */
} WireCmd;

typedef struct WireMask {
    uint32_t kind;                  /* 1 image, 2 linear gradient, 3 rounded rect, 4 ring, 5 shadow */
    uint32_t image;                 /* kind 1: image id */
    int32_t x, y, w, h;             /* the masked box, 1/256 px */
    int32_t ix, iy, iw, ih;         /* the image's place; a shadow's caster */
    uint32_t repeat;
    int32_t angle;                  /* 1/256 degree */
    uint32_t nstops;
    uint16_t stop_pos[WIRE_MASK_STOPS], stop_alpha[WIRE_MASK_STOPS];   /* 0..65535 */
    int32_t rx[4], ry[4];           /* 1/256 px */
    int32_t border[4];
    int32_t blur;
    int32_t ex_rx[4], ex_ry[4];
} WireMask;

#endif

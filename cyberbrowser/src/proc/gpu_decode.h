#ifndef GPU_DECODE_H
#define GPU_DECODE_H

/* The GPU process's verified decoder (SANDBOX_DESIGN.md sections 5.3, 8.3):
 * turns a TG_FRAME message (gpu_wire.h), already copied out of the channel,
 * into records the rasterizers can trust.  Pure C, no library calls, bounded
 * loops: proved with Frama-C/WP (tools/prove.sh) free of undefined behavior
 * for any bytes (P1), bounded (P7), and staying in its client's namespace
 * (P5):
 *   - every glyph and image names a font slot or image the sending tab
 *     uploaded, and every mask an image it uploaded or none; records that
 *     don't resolve are dropped;
 *   - every length lies in [-GPU_COORD_MAX, GPU_COORD_MAX] (sizes in
 *     [0, GPU_COORD_MAX]), every texture coordinate in [0, 1];
 *   - mask brackets are balanced and at most GPU_MAX_DEPTH deep; a mask
 *     command names a mask of the right kind. */

#include <stdbool.h>
#include <stdint.h>
#include "gpu_wire.h"

#define GPU_FONT_SLOTS 255
#define GPU_MAX_IMAGES 16384
#define GPU_MAX_CMDS (1u << 20)
#define GPU_MAX_MASKS (1u << 16)
#define GPU_MAX_DEPTH 64
#define GPU_COORD_MAX (1 << 28)     /* 2^20 px */
#define GPU_MAX_W 4096
#define GPU_MAX_H 4096
#define GPU_NO_IMAGE 0xffffffffu

#define WIRE_FRAME_BYTES 40u
#define WIRE_CMD_BYTES 52u
#define WIRE_MASK_BYTES 168u

/* What one tab has uploaded: its namespace. */
typedef struct GpuClientRes {
    uint8_t font[GPU_FONT_SLOTS];       /* 1: that slot's glyph atlas is here */
    uint8_t image[GPU_MAX_IMAGES];      /* 1: that image is here */
} GpuClientRes;

/*@ predicate coord_ok(integer v) = -GPU_COORD_MAX <= v <= GPU_COORD_MAX;
    predicate size_ok(integer v) = 0 <= v <= GPU_COORD_MAX;

    predicate mask_ok{L}(WireMask *m, GpuClientRes *res) =
        m->kind <= 5 &&
        (m->kind == 1 ==> m->image == GPU_NO_IMAGE || (m->image < GPU_MAX_IMAGES && res->image[m->image] != 0)) &&
        1 <= m->nstops <= WIRE_MASK_STOPS &&
        coord_ok(m->x) && coord_ok(m->y) && size_ok(m->w) && size_ok(m->h) &&
        coord_ok(m->ix) && coord_ok(m->iy) && size_ok(m->iw) && size_ok(m->ih) &&
        coord_ok(m->angle) && size_ok(m->blur) &&
        (\forall integer k; 0 <= k < 4 ==>
            size_ok(m->rx[k]) && size_ok(m->ry[k]) && size_ok(m->border[k]) &&
            size_ok(m->ex_rx[k]) && size_ok(m->ex_ry[k]));

    // P5: a record the rasterizers may draw.
    predicate cmd_ok{L}(WireCmd *c, GpuClientRes *res, WireMask *masks, integer nmasks) =
        c->type < WIRE_NTYPES &&
        coord_ok(c->x) && coord_ok(c->y) && size_ok(c->w) && size_ok(c->h) && size_ok(c->thickness) &&
        c->u0 <= WIRE_UV_ONE && c->v0 <= WIRE_UV_ONE && c->u1 <= WIRE_UV_ONE && c->v1 <= WIRE_UV_ONE &&
        (c->type == WIRE_GLYPH ==> c->ref < GPU_FONT_SLOTS && res->font[c->ref] != 0) &&
        (c->type == WIRE_IMAGE ==> c->ref < GPU_MAX_IMAGES && res->image[c->ref] != 0) &&
        (c->type == WIRE_MASK_BEGIN ==> c->ref < nmasks && 1 <= masks[c->ref].kind <= 4) &&
        (c->type == WIRE_SHADOW ==> c->ref < nmasks && masks[c->ref].kind == 5);

    // What a record does to the mask nesting.
    logic integer delta(integer type) = type == WIRE_MASK_BEGIN ? 1 : type == WIRE_MASK_END ? -1 : 0;
 */

/* Decode msg (len bytes) from a client with resources res into *frame,
 * cmds (cmd_cap records, at least the message's count + GPU_MAX_DEPTH) and
 * masks (mask_cap); levels[i] is the mask nesting after cmds[i].  False if
 * the message is malformed as a whole (its lengths don't add up, or it is
 * larger than the caps). */
/*@ requires \valid_read(msg + (0 .. len - 1)) && \valid_read(res);
    requires \valid(frame) && \valid(ncmds) && \valid(nmasks);
    requires \valid(cmds + (0 .. cmd_cap - 1)) && \valid(levels + (0 .. cmd_cap - 1));
    requires \valid(masks + (0 .. mask_cap - 1));
    requires \separated(msg + (0 .. len - 1), res, frame, ncmds, nmasks, cmds + (0 .. cmd_cap - 1),
                        levels + (0 .. cmd_cap - 1), masks + (0 .. mask_cap - 1));
    requires \separated(res, frame, ncmds, nmasks, cmds + (0 .. cmd_cap - 1), levels + (0 .. cmd_cap - 1),
                        masks + (0 .. mask_cap - 1));
    requires \separated(frame, ncmds, nmasks, cmds + (0 .. cmd_cap - 1), levels + (0 .. cmd_cap - 1),
                        masks + (0 .. mask_cap - 1));
    assigns *frame, *ncmds, *nmasks, cmds[0 .. cmd_cap - 1], levels[0 .. cmd_cap - 1], masks[0 .. mask_cap - 1];
    ensures \result ==> 1 <= frame->width <= GPU_MAX_W && 1 <= frame->height <= GPU_MAX_H;
    ensures \result ==> *nmasks <= mask_cap && *ncmds <= cmd_cap;
    ensures \result ==> \forall integer i; 0 <= i < *nmasks ==> mask_ok(&masks[i], res);
    ensures \result ==> \forall integer i; 0 <= i < *ncmds ==> cmd_ok(&cmds[i], res, masks, *nmasks);
    // Mask brackets balanced, at most GPU_MAX_DEPTH deep.
    ensures \result ==> \forall integer i; 0 <= i < *ncmds ==> levels[i] <= GPU_MAX_DEPTH;
    ensures \result && *ncmds > 0 ==> levels[0] == delta(cmds[0].type);
    ensures \result ==> \forall integer i; 0 < i < *ncmds ==> levels[i] == levels[i - 1] + delta(cmds[i].type);
    ensures \result && *ncmds > 0 ==> levels[*ncmds - 1] == 0;
 */
bool gpu_decode(const uint8_t *msg, uint32_t len, const GpuClientRes *res, GpuFrame *frame, WireCmd *cmds,
                uint8_t *levels, uint32_t cmd_cap, uint32_t *ncmds, WireMask *masks, uint32_t mask_cap, uint32_t *nmasks);

#endif

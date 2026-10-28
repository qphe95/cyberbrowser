/* gpu_decode.c - the GPU process's verified decoder (gpu_decode.h).  No
 * library calls, no allocation, bounded loops: proved with Frama-C/WP
 * (tools/prove.sh).  Integers are read byte by byte, little-endian, with
 * arithmetic the provers handle (no shifts or masks). */
#include "gpu_decode.h"

/*@ requires \valid_read(p + (0 .. 1));
    assigns \nothing;
    ensures 0 <= \result <= 65535;
 */
static uint32_t rd16(const uint8_t *p) {
    return (uint32_t)p[0] + (uint32_t)p[1] * 256u;
}

/*@ requires \valid_read(p + (0 .. 3));
    assigns \nothing;
 */
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] + (uint32_t)p[1] * 0x100u + (uint32_t)p[2] * 0x10000u + (uint32_t)p[3] * 0x1000000u;
}

/* A signed 32-bit length clamped into [lo, hi]. */
/*@ requires \valid_read(p + (0 .. 3)) && lo <= hi;
    assigns \nothing;
    ensures lo <= \result <= hi;
 */
static int32_t rd_clamp(const uint8_t *p, int32_t lo, int32_t hi) {
    int64_t v = (int64_t)rd32(p);
    if (v >= 0x80000000ll) v -= 0x100000000ll;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return (int32_t)v;
}

/*@ requires \valid_read(p + (0 .. 3));
    assigns \nothing;
    ensures coord_ok(\result);
 */
static int32_t rd_coord(const uint8_t *p) {
    return rd_clamp(p, -GPU_COORD_MAX, GPU_COORD_MAX);
}

/*@ requires \valid_read(p + (0 .. 3));
    assigns \nothing;
    ensures size_ok(\result);
 */
static int32_t rd_size(const uint8_t *p) {
    return rd_clamp(p, 0, GPU_COORD_MAX);
}

/*@ requires \valid_read(p + (0 .. 3));
    assigns \nothing;
    ensures \result <= WIRE_UV_ONE;
 */
static uint32_t rd_uv(const uint8_t *p) {
    uint32_t v = rd32(p);
    return v > WIRE_UV_ONE ? WIRE_UV_ONE : v;
}

/* One mask record (WIRE_MASK_BYTES at p): out of range kinds become 0
 * (none: commands naming it are dropped), an image not uploaded becomes
 * GPU_NO_IMAGE (transparent). */
/*@ requires \valid_read(p + (0 .. WIRE_MASK_BYTES - 1)) && \valid_read(res) && \valid(m);
    requires \separated(m, p + (0 .. WIRE_MASK_BYTES - 1), res);
    assigns *m;
    ensures mask_ok(m, res);
 */
static void decode_mask(const uint8_t *p, const GpuClientRes *res, WireMask *m) {
    uint32_t kind = rd32(p);
    m->kind = kind >= 1 && kind <= 5 ? kind : 0;
    uint32_t image = rd32(p + 4);
    m->image = m->kind == 1 && image < GPU_MAX_IMAGES && res->image[image] ? image : GPU_NO_IMAGE;
    m->x = rd_coord(p + 8);
    m->y = rd_coord(p + 12);
    m->w = rd_size(p + 16);
    m->h = rd_size(p + 20);
    m->ix = rd_coord(p + 24);
    m->iy = rd_coord(p + 28);
    m->iw = rd_size(p + 32);
    m->ih = rd_size(p + 36);
    m->repeat = rd32(p + 40) ? 1 : 0;
    m->angle = rd_coord(p + 44);
    uint32_t nstops = rd32(p + 48);
    m->nstops = nstops < 1 ? 1 : nstops > WIRE_MASK_STOPS ? WIRE_MASK_STOPS : nstops;
    /*@ loop invariant 0 <= k <= WIRE_MASK_STOPS;
        loop assigns k, m->stop_pos[0 .. WIRE_MASK_STOPS - 1], m->stop_alpha[0 .. WIRE_MASK_STOPS - 1];
        loop variant WIRE_MASK_STOPS - k;
     */
    for (uint32_t k = 0; k < WIRE_MASK_STOPS; k++) {
        m->stop_pos[k] = (uint16_t)rd16(p + 52 + 2 * k);
        m->stop_alpha[k] = (uint16_t)rd16(p + 68 + 2 * k);
    }
    /*@ loop invariant 0 <= k <= 4;
        loop invariant \forall integer j; 0 <= j < k ==>
            size_ok(m->rx[j]) && size_ok(m->ry[j]) && size_ok(m->border[j]) &&
            size_ok(m->ex_rx[j]) && size_ok(m->ex_ry[j]);
        loop assigns k, m->rx[0 .. 3], m->ry[0 .. 3], m->border[0 .. 3], m->ex_rx[0 .. 3], m->ex_ry[0 .. 3];
        loop variant 4 - k;
     */
    for (uint32_t k = 0; k < 4; k++) {
        m->rx[k] = rd_size(p + 84 + 4 * k);
        m->ry[k] = rd_size(p + 100 + 4 * k);
        m->border[k] = rd_size(p + 116 + 4 * k);
        m->ex_rx[k] = rd_size(p + 136 + 4 * k);
        m->ex_ry[k] = rd_size(p + 152 + 4 * k);
    }
    m->blur = rd_size(p + 132);
}

/* One command record (WIRE_CMD_BYTES at p), its numbers clamped; whether
 * its references resolve is gpu_decode's to check. */
/*@ requires \valid_read(p + (0 .. WIRE_CMD_BYTES - 1));
    assigns \nothing;
    ensures coord_ok(\result.x) && coord_ok(\result.y) && size_ok(\result.w) && size_ok(\result.h);
    ensures size_ok(\result.thickness);
    ensures \result.u0 <= WIRE_UV_ONE && \result.v0 <= WIRE_UV_ONE;
    ensures \result.u1 <= WIRE_UV_ONE && \result.v1 <= WIRE_UV_ONE;
 */
static WireCmd decode_cmd(const uint8_t *p) {
    WireCmd c;
    c.type = rd32(p);
    c.x = rd_coord(p + 4);
    c.y = rd_coord(p + 8);
    c.w = rd_size(p + 12);
    c.h = rd_size(p + 16);
    c.r = (uint16_t)rd16(p + 20);
    c.g = (uint16_t)rd16(p + 22);
    c.b = (uint16_t)rd16(p + 24);
    c.a = (uint16_t)rd16(p + 26);
    c.thickness = rd_size(p + 28);
    c.ref = rd32(p + 32);
    c.u0 = rd_uv(p + 36);
    c.v0 = rd_uv(p + 40);
    c.u1 = rd_uv(p + 44);
    c.v1 = rd_uv(p + 48);
    return c;
}

/* Does c's reference resolve in the client's namespace (and its masks). */
/*@ requires \valid_read(res) && \valid_read(masks + (0 .. nmasks - 1));
    assigns \nothing;
    ensures \result ==> type < WIRE_NTYPES;
    ensures \result && type == WIRE_GLYPH ==> ref < GPU_FONT_SLOTS && res->font[ref] != 0;
    ensures \result && type == WIRE_IMAGE ==> ref < GPU_MAX_IMAGES && res->image[ref] != 0;
    ensures \result && type == WIRE_MASK_BEGIN ==> ref < nmasks && 1 <= masks[ref].kind <= 4;
    ensures \result && type == WIRE_SHADOW ==> ref < nmasks && masks[ref].kind == 5;
 */
static bool resolves(uint32_t type, uint32_t ref, const GpuClientRes *res, const WireMask *masks, uint32_t nmasks) {
    switch (type) {
    case WIRE_RECT:
    case WIRE_BORDER:
    case WIRE_MASK_END:
        return true;
    case WIRE_GLYPH:
        return ref < GPU_FONT_SLOTS && res->font[ref];
    case WIRE_IMAGE:
        return ref < GPU_MAX_IMAGES && res->image[ref];
    case WIRE_MASK_BEGIN:
        return ref < nmasks && masks[ref].kind >= 1 && masks[ref].kind <= 4;
    case WIRE_SHADOW:
        return ref < nmasks && masks[ref].kind == 5;
    default:
        return false;
    }
}

#define IN_STACK 256                /* input nesting tracked; deeper brackets are dropped */

bool gpu_decode(const uint8_t *msg, uint32_t len, const GpuClientRes *res, GpuFrame *frame, WireCmd *cmds,
                uint8_t *levels, uint32_t cmd_cap, uint32_t *ncmds, WireMask *masks, uint32_t mask_cap,
                uint32_t *nmasks) {
    if (len < WIRE_FRAME_BYTES) return false;
    uint32_t in_cmds = rd32(msg + 12), in_masks = rd32(msg + 16);
    if (in_cmds > GPU_MAX_CMDS || in_masks > GPU_MAX_MASKS || in_masks > mask_cap ||
        cmd_cap < GPU_MAX_DEPTH || in_cmds > cmd_cap - GPU_MAX_DEPTH)
        return false;
    if ((uint64_t)WIRE_FRAME_BYTES + (uint64_t)in_cmds * WIRE_CMD_BYTES + (uint64_t)in_masks * WIRE_MASK_BYTES != len)
        return false;
    frame->frame = rd32(msg);
    uint32_t w = rd32(msg + 4), h = rd32(msg + 8);
    frame->width = w < 1 ? 1 : w > GPU_MAX_W ? GPU_MAX_W : w;
    frame->height = h < 1 ? 1 : h > GPU_MAX_H ? GPU_MAX_H : h;
    frame->ncmds = in_cmds;
    frame->nmasks = in_masks;

    /* The masks first: commands refer to them by index. */
    const uint8_t *mp = msg + WIRE_FRAME_BYTES + in_cmds * WIRE_CMD_BYTES;
    /*@ loop invariant 0 <= i <= in_masks;
        loop invariant \forall integer k; 0 <= k < i ==> mask_ok(&masks[k], res);
        loop assigns i, masks[0 .. mask_cap - 1];
        loop variant in_masks - i;
     */
    for (uint32_t i = 0; i < in_masks; i++) decode_mask(mp + i * WIRE_MASK_BYTES, res, &masks[i]);
    *nmasks = in_masks;

    /* The commands, dropping what doesn't resolve.  Each mask bracket the
     * input opens is kept (if its mask is good and the output is less than
     * GPU_MAX_DEPTH deep) or dropped together with its close. */
    uint8_t kept[IN_STACK];
    uint32_t sp = 0, over = 0, n = 0, depth = 0;
    /*@ loop invariant 0 <= i <= in_cmds;
        loop invariant 0 <= n <= i;
        loop invariant 0 <= depth <= GPU_MAX_DEPTH && 0 <= sp <= IN_STACK;
        loop invariant n == 0 ==> depth == 0;
        loop invariant n > 0 ==> levels[n - 1] == depth;
        loop invariant \forall integer k; 0 <= k < n ==> levels[k] <= GPU_MAX_DEPTH;
        loop invariant n > 0 ==> levels[0] == delta(cmds[0].type);
        loop invariant \forall integer k; 0 < k < n ==> levels[k] == levels[k - 1] + delta(cmds[k].type);
        loop invariant \forall integer k; 0 <= k < n ==> cmd_ok(&cmds[k], res, masks, in_masks);
        loop invariant \forall integer k; 0 <= k < in_masks ==> mask_ok{LoopEntry}(&masks[k], res);
        loop assigns i, n, sp, over, depth, kept[0 .. IN_STACK - 1], cmds[0 .. cmd_cap - 1],
                     levels[0 .. cmd_cap - 1];
        loop variant in_cmds - i;
     */
    for (uint32_t i = 0; i < in_cmds; i++) {
        WireCmd c = decode_cmd(msg + WIRE_FRAME_BYTES + i * WIRE_CMD_BYTES);
        if (c.type == WIRE_MASK_BEGIN) {
            if (sp == IN_STACK) {
                over++;                         /* too deep to track: dropped */
                continue;
            }
            bool keep = depth < GPU_MAX_DEPTH && resolves(c.type, c.ref, res, masks, in_masks);
            kept[sp++] = keep ? 1 : 0;
            if (!keep) continue;
            depth++;
        } else if (c.type == WIRE_MASK_END) {
            if (over > 0) {
                over--;
                continue;
            }
            if (sp == 0) continue;              /* nothing open */
            if (!kept[--sp] || depth == 0) continue;
            depth--;
        } else if (!resolves(c.type, c.ref, res, masks, in_masks)) {
            continue;
        }
        cmds[n] = c;
        levels[n] = (uint8_t)depth;
        n++;
    }
    /* Close what is still open. */
    WireCmd end;
    end.type = WIRE_MASK_END;
    end.x = end.y = end.w = end.h = end.thickness = 0;
    end.r = end.g = end.b = end.a = 0;
    end.ref = 0;
    end.u0 = end.v0 = end.u1 = end.v1 = 0;
    /*@ loop invariant 0 <= depth <= GPU_MAX_DEPTH && n + depth <= cmd_cap;
        loop invariant n == 0 ==> depth == 0;
        loop invariant n > 0 ==> levels[n - 1] == depth;
        loop invariant \forall integer k; 0 <= k < n ==> levels[k] <= GPU_MAX_DEPTH;
        loop invariant n > 0 ==> levels[0] == delta(cmds[0].type);
        loop invariant \forall integer k; 0 < k < n ==> levels[k] == levels[k - 1] + delta(cmds[k].type);
        loop invariant \forall integer k; 0 <= k < n ==> cmd_ok(&cmds[k], res, masks, in_masks);
        loop assigns n, depth, cmds[0 .. cmd_cap - 1], levels[0 .. cmd_cap - 1];
        loop variant depth;
     */
    while (depth > 0) {
        depth--;
        cmds[n] = end;
        levels[n] = (uint8_t)depth;
        n++;
    }
    *ncmds = n;
    return true;
}

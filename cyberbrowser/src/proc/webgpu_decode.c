/* webgpu_decode.c - the decoding of a tab's WebGPU records (webgpu_decode.h).
 * No library calls, no recursion, bounded loops: proved with Frama-C/WP
 * (tools/prove.sh webgpu).  Integers are read byte by byte, little-endian,
 * with arithmetic the provers handle (no shifts or masks). */
#include "webgpu_decode.h"

/*@ requires \valid_read(p + (0 .. 3));
    assigns \nothing;
 */
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] + (uint32_t)p[1] * 0x100u + (uint32_t)p[2] * 0x10000u + (uint32_t)p[3] * 0x1000000u;
}

/*@ requires \valid_read(p + (0 .. 7));
    assigns \nothing;
 */
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) + (uint64_t)rd32(p + 4) * 0x100000000ull;
}

/* ---- value trees ------------------------------------------------------------------------ */

/* A node added to the tree, with no children or siblings yet. */
/*@ requires \valid(t) && tree_ok(t, len) && t->n < WIRE_MAX_NODES;
    requires val_ok(v, len) && v.first == -1 && v.next == -1;
    assigns t->n, t->nodes[t->n];
    ensures \result == \old(t->n) && t->n == \old(t->n) + 1;
    ensures tree_ok(t, len);
 */
static int32_t add(WireTree *t, WireNode v, uint32_t len) {
    (void)len;
    int32_t i = t->n;
    t->nodes[i] = v;
    t->n = i + 1;
    return i;
}

/* Node i as node p's first child. */
/*@ requires \valid(t) && tree_ok(t, len) && 0 <= p < i < t->n;
    assigns t->nodes[p].first;
    ensures tree_ok(t, len) && t->n == \old(t->n);
 */
static void link_first(WireTree *t, int32_t p, int32_t i, uint32_t len) {
    (void)len;
    t->nodes[p].first = i;
}

/* Node i as node p's next sibling. */
/*@ requires \valid(t) && tree_ok(t, len) && 0 <= p < i < t->n;
    assigns t->nodes[p].next;
    ensures tree_ok(t, len) && t->n == \old(t->n);
 */
static void link_next(WireTree *t, int32_t p, int32_t i, uint32_t len) {
    (void)len;
    t->nodes[p].next = i;
}

/* Iterative, with the open arrays and objects on a stack of its own: a
 * frame per container being filled (its node, its last child so far, how
 * many children are still to come, whether they have keys).  A node's
 * depth is how many frames are open when it is read. */
int32_t wire_parse(const uint8_t *in, uint32_t len, uint32_t *pos, WireTree *t) {
    int32_t par[WIRE_STACK], last[WIRE_STACK];
    uint32_t left[WIRE_STACK];
    bool keyed[WIRE_STACK];
    uint32_t sp = 0, at = *pos;
    int32_t root = -1;
    /*@ ghost int32_t n0 = t->n; */
    /*@ loop invariant 0 <= sp <= WIRE_STACK;
        loop invariant at <= len;
        loop invariant n0 <= t->n <= WIRE_MAX_NODES;
        loop invariant tree_ok(t, len);
        loop invariant sp == 0 ==> t->n == n0 && root == -1;
        loop invariant sp > 0 ==> t->n > n0 && root == n0;
        loop invariant \forall integer k; 0 <= k < sp ==>
            n0 <= par[k] < t->n && (last[k] == -1 || par[k] < last[k] < t->n);
        loop assigns sp, at, root, t->n, t->nodes[0 .. WIRE_MAX_NODES - 1],
                     par[0 .. WIRE_STACK - 1], last[0 .. WIRE_STACK - 1], left[0 .. WIRE_STACK - 1],
                     keyed[0 .. WIRE_STACK - 1];
        loop variant 2 * (WIRE_MAX_NODES - t->n) + sp;
     */
    for (;;) {
        if (sp > 0 && left[sp - 1] == 0) {          /* a container filled */
            sp--;
            if (sp == 0) break;
            continue;
        }
        /* A value; in an object, after its key. */
        uint32_t key_off = 0, key_len = 0;
        if (sp > 0 && keyed[sp - 1]) {
            if (len - at < 4) goto bad;
            key_len = rd32(in + at);
            at += 4;
            if (key_len > WIRE_MAX_KEY || key_len > len - at) goto bad;
            key_off = at;
            at += key_len;
        }
        if (sp > WIRE_MAX_DEPTH || at >= len || t->n >= WIRE_MAX_NODES) goto bad;
        WireNode v;
        v.tag = in[at];
        at++;
        v.n = 0;
        v.bits = 0;
        v.off = 0;
        v.key_off = key_off;
        v.key_len = key_len;
        v.first = -1;
        v.next = -1;
        switch (v.tag) {
        case V_NULL: case V_FALSE: case V_TRUE: break;
        case V_NUMBER:
            if (len - at < 8) goto bad;
            v.bits = rd64(in + at);
            at += 8;
            break;
        case V_ID:
            if (len - at < 4) goto bad;
            v.n = rd32(in + at);
            at += 4;
            break;
        case V_STRING: case V_BYTES:
            if (len - at < 4) goto bad;
            v.n = rd32(in + at);
            at += 4;
            if (v.n > len - at || v.n > WIRE_MAX_STRING) goto bad;
            v.off = at;
            at += v.n;
            break;
        case V_ARRAY: case V_OBJECT:
            if (len - at < 4) goto bad;
            v.n = rd32(in + at);
            at += 4;
            if (v.n > WIRE_MAX_NODES) goto bad;
            break;
        default:
            goto bad;
        }
        int32_t i = add(t, v, len);
        if (sp > 0) {                               /* a child of the container open */
            if (last[sp - 1] < 0) link_first(t, par[sp - 1], i, len);
            else link_next(t, last[sp - 1], i, len);
            last[sp - 1] = i;
            left[sp - 1]--;
        } else {
            root = i;
        }
        if ((v.tag == V_ARRAY || v.tag == V_OBJECT) && v.n > 0) {
            par[sp] = i;
            last[sp] = -1;
            left[sp] = v.n;
            keyed[sp] = v.tag == V_OBJECT;
            sp++;
        } else if (sp == 0) {
            break;
        }
    }
    *pos = at;
    return root;
bad:
    *pos = at;
    return -1;
}

int wire_next(const uint8_t *recs, uint32_t len, uint32_t at, uint32_t *op, uint32_t *off, uint32_t *n) {
    if (at == len) return 0;
    if (len - at < 8) return -1;
    uint32_t k = rd32(recs + at + 4);
    if (k > len - at - 8) return -1;
    *op = rd32(recs + at);
    *off = at + 8;
    *n = k;
    return 1;
}

/* ---- a client's objects --------------------------------------------------------------- */

WgpuObj *wgpu_obj(WgpuObj *objs, uint32_t id, uint32_t kind) {
    if (id == 0 || id >= WGPU_MAX_IDS || objs[id].kind != kind || !objs[id].h || objs[id].lost) return 0;
    uint32_t d = objs[id].device;
    if (kind != O_ADAPTER && d != 0 && d < WGPU_MAX_IDS && objs[d].lost) return 0;
    return &objs[id];
}

bool wgpu_id_free(const WgpuObj *objs, uint32_t id) {
    return id > 0 && id < WGPU_MAX_IDS && objs[id].kind == O_NONE;
}

/* ---- numbers and ranges --------------------------------------------------------------- */

#define SIGN 0x8000000000000000ull
#define P52 0x10000000000000ull             /* 2^52: an f64's mantissa field */

bool wire_whole(uint64_t bits, uint64_t *v) {
    if (bits == SIGN) { *v = 0; return true; }      /* -0 */
    if (bits >= SIGN) return false;                 /* negative */
    uint64_t e = bits / P52, m = bits % P52;        /* the biased exponent, 0 .. 2047 */
    if (e == 2047) return false;                    /* infinite, NaN */
    if (e < 1023) { *v = 0; return true; }          /* below 1 */
    if (e >= 1023 + 53) return false;               /* 2^53 or more */
    /* (2^52 + m) / 2^(52 - (e - 1023)), truncated. */
    uint64_t x = P52 + m;
    /*@ loop invariant 1023 <= k <= 1023 + 52;
        loop invariant x < 2 * P52;
        loop assigns k, x;
        loop variant 1023 + 52 - k;
     */
    for (uint64_t k = e; k < 1023 + 52; k++) x /= 2;
    *v = x;
    return true;
}

bool wgpu_map_ok(uint64_t off, uint64_t size, uint64_t buf_size) {
    return off % 8 == 0 && size % 4 == 0 && size <= WGPU_MAP_MAX && off <= buf_size && size <= buf_size - off;
}

bool wgpu_write_ok(uint64_t off, uint32_t len, uint64_t map_off, uint64_t map_size) {
    return len != 0 && off % 8 == 0 && len % 4 == 0 && off >= map_off && off - map_off <= map_size &&
           len <= map_size - (off - map_off);
}

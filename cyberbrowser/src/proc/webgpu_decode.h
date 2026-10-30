#ifndef WEBGPU_DECODE_H
#define WEBGPU_DECODE_H

/* The web-GPU process's decoding of a tab's WebGPU records (SANDBOX_DESIGN.md
 * sections 5.7, 8.7.3): the records' framing, the value trees
 * (webgpu_wire.h), the object ids, and the ranges checked before bytes are
 * copied into mapped memory.  Pure C, no library calls, no recursion,
 * bounded loops: proved with Frama-C/WP (tools/prove.sh webgpu) free of
 * undefined behavior for any bytes (P1) and bounded (P7), and:
 *   - a tree has at most WIRE_MAX_NODES nodes and WIRE_MAX_DEPTH levels;
 *     every string, byte string and key it returns lies inside the input,
 *     and every child and sibling index names a later node of the tree (so
 *     walking it stays inside it, and ends);
 *   - an id names an object of this client's table, of the kind asked for,
 *     live (made, and its device not destroyed), or nothing;
 *   - an offset and size from the page reach a mapping only inside it. */

#include <stdbool.h>
#include <stdint.h>
#include "webgpu_wire.h"

#define WIRE_MAX_KEY 256                /* an object's key */
#define WIRE_STACK (WIRE_MAX_DEPTH + 1) /* open arrays and objects */

/* A value of a tree.  Where the input's bytes are: offsets into it. */
typedef struct WireNode {
    uint8_t tag;                    /* V_* */
    uint32_t n;                     /* array/object: children; string/bytes: length; id: the id */
    uint64_t bits;                  /* number: the f64's bits */
    uint32_t off;                   /* string/bytes: where its bytes start */
    uint32_t key_off, key_len;      /* a member of an object: its key */
    int32_t first, next;            /* children, siblings: node indices, -1 none */
} WireNode;

typedef struct WireTree {
    WireNode nodes[WIRE_MAX_NODES];
    int32_t n;
} WireTree;

/*@ // A node's own values: its tag, and its bytes and key inside the input.
    predicate val_ok(WireNode v, integer len) =
        v.tag <= V_BYTES &&
        v.key_len <= WIRE_MAX_KEY && v.key_off + v.key_len <= len &&
        ((v.tag == V_STRING || v.tag == V_BYTES) ==> v.n <= WIRE_MAX_STRING && v.off + v.n <= len) &&
        ((v.tag == V_ARRAY || v.tag == V_OBJECT) ==> v.n <= WIRE_MAX_NODES);

    // Node i's child and sibling: none, or a later node of the n there are.
    predicate links_ok(WireNode v, integer i, integer n) =
        (v.first == -1 || i < v.first < n) && (v.next == -1 || i < v.next < n);

    lemma links_more: \forall WireNode v, integer i, n, m; links_ok(v, i, n) && n <= m ==> links_ok(v, i, m);

    predicate tree_ok{L}(WireTree *t, integer len) =
        0 <= t->n <= WIRE_MAX_NODES &&
        \forall integer i; 0 <= i < t->n ==> val_ok(t->nodes[i], len) && links_ok(t->nodes[i], i, t->n);
 */

/* One value of in[0 .. len) at *pos (and what it holds), added to t after
 * the nodes it has; *pos moves past it.  Its node index, or -1 if it is
 * malformed or breaks a limit (t then holds what was read before). */
/*@ requires \valid_read(in + (0 .. len - 1)) && \valid(t) && \valid(pos);
    requires \separated(t, pos, in + (0 .. len - 1));
    requires \separated(pos, in + (0 .. len - 1));
    requires *pos <= len && tree_ok(t, len);
    assigns *t, *pos;
    ensures tree_ok(t, len) && *pos <= len && \old(t->n) <= t->n;
    ensures \result == -1 || (\result == \old(t->n) && \result < t->n);
 */
int32_t wire_parse(const uint8_t *in, uint32_t len, uint32_t *pos, WireTree *t);

/* The record at `at` of a batch of len bytes ([u32 op][u32 n][n bytes]): 1
 * (its op, and its bytes at recs[*off .. *off + *n)), 0 at the end, -1 if
 * the rest is cut short. */
/*@ requires \valid_read(recs + (0 .. len - 1)) && \valid(op) && \valid(off) && \valid(n);
    requires \separated(op, off, n, recs + (0 .. len - 1));
    requires at <= len;
    assigns *op, *off, *n;
    ensures \result == 0 || \result == 1 || \result == -1;
    ensures \result == 0 <==> at == len;
    ensures \result == 1 ==> *off == at + 8 && *n <= len - *off;
 */
int wire_next(const uint8_t *recs, uint32_t len, uint32_t at, uint32_t *op, uint32_t *off, uint32_t *n);

/* ---- a client's objects --------------------------------------------------------------- */

enum { O_NONE = 0, O_ADAPTER, O_DEVICE, O_QUEUE, O_BUFFER, O_TEXTURE, O_VIEW, O_SAMPLER, O_MODULE, O_BGL, O_PL, O_BG,
       O_RPIPE, O_CPIPE, O_ENCODER, O_RPASS, O_CPASS, O_CMDBUF };

#define WGPU_MAX_IDS 65536

/* A buffer's or texture's state, shared by what uses it (webgpu_wgpu.c). */
typedef struct WgpuRes WgpuRes;

/* One id of a client's table. */
typedef struct WgpuObj {
    uint8_t kind;                   /* O_*, O_NONE: free */
    void *h;                        /* wgpu's */
    uint32_t device;                /* the device it was made by */
    uint32_t scopes;                /* a device's error scopes pushed (wgpu-native aborts on a pop of none) */
    bool lost;                      /* a device destroyed: nothing of it reaches wgpu again (much of what
                                     * wgpu-native does on a lost device aborts) */
    bool ended;                     /* a pass ended */
    bool bad;                       /* uses more than can be tracked: never submitted */
    uint32_t parent;                /* a pass's encoder */
    WgpuRes *res;                   /* a buffer's or a texture's (a view's: its texture's) */
    WgpuRes **uses;                 /* what a bind group, an encoder or a command buffer uses */
    uint32_t nuses, cap_uses;
    uint64_t size, map_off, map_size;   /* a buffer's size, and the range mapped */
} WgpuObj;

/*@ predicate live{L}(WgpuObj *objs, integer id, integer kind) =
        0 < id < WGPU_MAX_IDS && objs[id].kind == kind && objs[id].h != \null && !objs[id].lost &&
        (kind == O_ADAPTER || objs[id].device == 0 || objs[id].device >= WGPU_MAX_IDS ||
         !objs[objs[id].device].lost);
 */

/* The object id names in a client's table objs (WGPU_MAX_IDS entries), if
 * it is of that kind and live; else NULL. */
/*@ requires \valid(objs + (0 .. WGPU_MAX_IDS - 1));
    assigns \nothing;
    ensures live(objs, id, kind) ==> \result == objs + id;
    ensures !live(objs, id, kind) ==> \result == \null;
 */
WgpuObj *wgpu_obj(WgpuObj *objs, uint32_t id, uint32_t kind);

/* Whether id is one a new object may take (in range, and free). */
/*@ requires \valid_read(objs + (0 .. WGPU_MAX_IDS - 1));
    assigns \nothing;
    ensures \result <==> 0 < id < WGPU_MAX_IDS && objs[id].kind == O_NONE;
 */
bool wgpu_id_free(const WgpuObj *objs, uint32_t id);

/* ---- numbers and ranges --------------------------------------------------------------- */

#define WIRE_WHOLE_MAX 9007199254740992ull  /* 2^53: past it, an f64 skips whole numbers */
#define WGPU_MAP_MAX (256u << 20)            /* the most a mapping may read back at once */

/* An f64 (its bits, as the tab sent it) as a whole number below 2^53,
 * truncated toward zero as WebIDL's conversion to an integer is: false if
 * it is negative (but -0), not finite, or too large. */
/*@ requires \valid(v);
    assigns *v;
    ensures \result ==> *v < WIRE_WHOLE_MAX;
 */
bool wire_whole(uint64_t bits, uint64_t *v);

/* Whether mapAsync may map [off, off + size) of a buffer of buf_size bytes:
 * off a multiple of 8, size of 4, at most WGPU_MAP_MAX, inside the buffer. */
/*@ assigns \nothing;
    ensures \result <==> off % 8 == 0 && size % 4 == 0 && size <= WGPU_MAP_MAX && off + size <= buf_size;
 */
bool wgpu_map_ok(uint64_t off, uint64_t size, uint64_t buf_size);

/* Whether len bytes written at off land inside the range mapped,
 * [map_off, map_off + map_size): off a multiple of 8, len of 4, not 0. */
/*@ assigns \nothing;
    ensures \result <==> len != 0 && off % 8 == 0 && len % 4 == 0 &&
        map_off <= off && off + len <= map_off + map_size;
 */
bool wgpu_write_ok(uint64_t off, uint32_t len, uint64_t map_off, uint64_t map_size);

#endif

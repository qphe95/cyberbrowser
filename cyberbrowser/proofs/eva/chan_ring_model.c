/* chan_ring_model.c - chan_ring.c's contracts (chan_ring.h), as code Eva
 * runs in place of the proved functions.
 *
 * chan_ring_put and chan_ring_take are proved against their contracts by
 * WP (tools/prove.sh chan: P1, P2).  Here Eva only needs what the contracts
 * promise, for any ring contents: each body below does no more than its
 * contract allows (it writes only what the assigns clause names, and any
 * values the postconditions permit), so whatever Eva concludes about
 * chan.c from it holds for the real functions too.  A body, not the
 * contract alone, because Eva can then keep "a message was taken" and "its
 * length is at most CHAN_MSG_MAX" together (-eva-split-return); and the
 * contracts have no \from clauses, which Eva needs.
 *
 * Eva checks the contracts' preconditions at each call in chan.c. */
#include "chan_ring.h"
#include "__fc_builtin.h"

int chan_ring_put(uint8_t *ring, uint32_t size, uint64_t head, uint64_t tail, uint16_t type,
                  const uint8_t *payload, uint32_t len, uint64_t *new_tail) {
    (void)type;
    if (len > CHAN_MSG_MAX) return -1;
    if (len) {
        /* The payload is read (it is copied into the ring). */
        uint8_t first = payload[0], last = payload[len - 1];
        (void)first;
        (void)last;
    }
    int r = Frama_C_interval(-1, 1);
    if (r == 1) {
        uint64_t t = tail + CHAN_HDR_BYTES + ((len + 7) / 8) * 8;
        if ((uint64_t)(t - head) > size) return 0;  /* no room: the contract allows no other 1 */
        /* The ring's bytes (shared: already unknown) are left as they are. */
        *new_tail = t;
    }
    return r;
}

int chan_ring_take(const uint8_t *ring, uint32_t size, uint64_t head, uint64_t tail, ChanMsg *m,
                   uint64_t *new_head) {
    (void)ring;
    int r = Frama_C_interval(-1, 1);
    if (r == 1) {
        uint32_t len = Frama_C_unsigned_int_interval(0, CHAN_MSG_MAX);
        uint64_t h = head + CHAN_HDR_BYTES + ((len + 7) / 8) * 8;
        if ((uint64_t)(h - head) > (uint64_t)(tail - head) || (uint64_t)(tail - head) > size) return 0;
        m->type = (uint16_t)Frama_C_interval(0, 0xffff);
        m->len = len;
        Frama_C_make_unknown((char *)m->data, CHAN_MSG_MAX);
        *new_head = h;
    }
    return r;
}

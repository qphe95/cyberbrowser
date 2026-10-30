#ifndef CHAN_RING_H
#define CHAN_RING_H

/* The rings of libchan (chan.h), without Win32: putting a message into a
 * ring and taking one out, given the head and tail counters the caller read
 * from shared memory once.  Pure C, no library calls, bounded loops: proved
 * in M4 (SANDBOX_DESIGN.md section 8.3, P1 and P2) whatever the ring's bytes
 * and the counters hold.
 *
 * A message is an 8-byte header (payload length, type) and its payload,
 * padded to 8 bytes.  Head and tail are byte counters that only grow; a
 * position in the ring is the counter modulo the ring's size, a power of
 * two. */

#include <stdint.h>

#define CHAN_MSG_MAX 16384          /* largest payload */
#define CHAN_HDR_BYTES 8u
#define CHAN_RING_MIN (2 * (CHAN_MSG_MAX + CHAN_HDR_BYTES))
#define CHAN_RING_MAX (1u << 28)

typedef struct ChanMsg {
    uint16_t type;
    uint32_t len;
    uint8_t data[CHAN_MSG_MAX];     /* a private copy */
} ChanMsg;

/*@ predicate ring_size_ok(integer size) =
        CHAN_RING_MIN <= size <= CHAN_RING_MAX && (size & (size - 1)) == 0;
 */

#define CHAN_MAGIC 0x6e616863u      /* "chan" */

/* Whether a channel's shared header, as read once by chan_attach (the peer
 * can rewrite it), describes a channel this side may map: the magic, and a
 * ring size that chan_ring_put and chan_ring_take accept. */
/*@ assigns \nothing;
    ensures \result <==> magic == CHAN_MAGIC && ring_size_ok(ring_bytes);
 */
int chan_ring_header_ok(uint32_t magic, uint32_t ring_bytes);

/* Put a message at tail of a ring whose consumer is at head: 1 put (and
 * *new_tail is past it), 0 no room yet, -1 the counters are impossible (the
 * peer corrupted them). */
/*@ requires ring_size_ok(size);
    requires \valid(ring + (0 .. size - 1));
    requires len <= CHAN_MSG_MAX ==> \valid_read(payload + (0 .. len - 1));
    requires \valid(new_tail);
    requires \separated(ring + (0 .. size - 1), new_tail);
    requires len <= CHAN_MSG_MAX ==> \separated(ring + (0 .. size - 1), payload + (0 .. len - 1));
    assigns ring[0 .. size - 1], *new_tail;
    ensures -1 <= \result <= 1;
    ensures len > CHAN_MSG_MAX ==> \result == -1;
    ensures \result == 1 ==> *new_tail == (uint64_t)(tail + CHAN_HDR_BYTES + ((len + 7) / 8) * 8);
    ensures \result == 1 ==> (uint64_t)(*new_tail - head) <= size;
 */
int chan_ring_put(uint8_t *ring, uint32_t size, uint64_t head, uint64_t tail, uint16_t type,
                  const uint8_t *payload, uint32_t len, uint64_t *new_tail);

/* Take the message at head of a ring whose producer is at tail: 1 took it
 * into m (and *new_head is past it), 0 none waiting, -1 the counters or the
 * header are impossible. */
/*@ requires ring_size_ok(size);
    requires \valid_read(ring + (0 .. size - 1));
    requires \valid(m) && \valid(new_head);
    requires \separated(ring + (0 .. size - 1), m, new_head);
    requires \separated(m, new_head);
    assigns m->type, m->len, m->data[0 .. CHAN_MSG_MAX - 1], *new_head;
    ensures -1 <= \result <= 1;
    ensures \result == 1 ==> m->len <= CHAN_MSG_MAX;
    ensures \result == 1 ==> *new_head == (uint64_t)(head + CHAN_HDR_BYTES + ((m->len + 7) / 8) * 8);
    ensures \result == 1 ==> (uint64_t)(*new_head - head) <= (uint64_t)(tail - head) <= size;
 */
int chan_ring_take(const uint8_t *ring, uint32_t size, uint64_t head, uint64_t tail, ChanMsg *m,
                   uint64_t *new_head);

#endif

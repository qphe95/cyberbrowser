/* chan_ring.c - libchan's rings (chan_ring.h).  No library calls, bounded
 * loops: proved with Frama-C/WP (tools/prove.sh). */
#include "chan_ring.h"

/*@ logic integer pad8(integer n) = ((n + 7) / 8) * 8; */

/*@ requires n <= CHAN_MSG_MAX;
    assigns \nothing;
    ensures \result == pad8(n);
 */
static uint32_t pad8(uint32_t n) { return (n + 7u) / 8u * 8u; }

/* The position of counter pos in a ring of size bytes. */
/*@ requires ring_size_ok(size);
    assigns \nothing;
    ensures 0 <= \result < size;
 */
static uint32_t at_of(uint64_t pos, uint32_t size) {
    return (uint32_t)(pos % size);
}

/* Copy n bytes into the ring at counter pos, wrapping: two plain copies,
 * which the compiler turns into memcpy. */
/*@ requires ring_size_ok(size) && n <= size;
    requires \valid(ring + (0 .. size - 1));
    requires \valid_read(src + (0 .. n - 1));
    requires \separated(ring + (0 .. size - 1), src + (0 .. n - 1));
    assigns ring[0 .. size - 1];
 */
static void ring_write(uint8_t *ring, uint32_t size, uint64_t pos, const uint8_t *src, uint32_t n) {
    uint32_t at = at_of(pos, size), first = size - at < n ? size - at : n;
    /*@ loop invariant 0 <= i <= first;
        loop assigns i, ring[at .. size - 1];
        loop variant first - i;
     */
    for (uint32_t i = 0; i < first; i++) ring[at + i] = src[i];
    /*@ loop invariant first <= i <= n;
        loop assigns i, ring[0 .. n - first - 1];
        loop variant n - i;
     */
    for (uint32_t i = first; i < n; i++) ring[i - first] = src[i];
}

/* Copy n bytes out of the ring at counter pos, wrapping. */
/*@ requires ring_size_ok(size) && n <= size;
    requires \valid_read(ring + (0 .. size - 1));
    requires \valid(dst + (0 .. n - 1));
    requires \separated(ring + (0 .. size - 1), dst + (0 .. n - 1));
    assigns dst[0 .. n - 1];
 */
static void ring_read(const uint8_t *ring, uint32_t size, uint64_t pos, uint8_t *dst, uint32_t n) {
    uint32_t at = at_of(pos, size), first = size - at < n ? size - at : n;
    /*@ loop invariant 0 <= i <= first;
        loop assigns i, dst[0 .. first - 1];
        loop variant first - i;
     */
    for (uint32_t i = 0; i < first; i++) dst[i] = ring[at + i];
    /*@ loop invariant first <= i <= n;
        loop assigns i, dst[first .. n - 1];
        loop variant n - i;
     */
    for (uint32_t i = first; i < n; i++) dst[i] = ring[i - first];
}

int chan_ring_header_ok(uint32_t magic, uint32_t ring_bytes) {
    return magic == CHAN_MAGIC && ring_bytes >= CHAN_RING_MIN && ring_bytes <= CHAN_RING_MAX &&
           (ring_bytes & (ring_bytes - 1)) == 0;
}

int chan_ring_put(uint8_t *ring, uint32_t size, uint64_t head, uint64_t tail, uint16_t type,
                  const uint8_t *payload, uint32_t len, uint64_t *new_tail) {
    if (len > CHAN_MSG_MAX) return -1;
    uint64_t used = tail - head;
    if (used > size) return -1;
    uint32_t need = CHAN_HDR_BYTES + pad8(len);
    if (size - used < need) return 0;
    uint8_t hdr[CHAN_HDR_BYTES];
    hdr[0] = (uint8_t)len;
    hdr[1] = (uint8_t)(len >> 8);
    hdr[2] = (uint8_t)(len >> 16);
    hdr[3] = (uint8_t)(len >> 24);
    hdr[4] = (uint8_t)type;
    hdr[5] = (uint8_t)(type >> 8);
    hdr[6] = 0;
    hdr[7] = 0;
    ring_write(ring, size, tail, hdr, CHAN_HDR_BYTES);
    ring_write(ring, size, tail + CHAN_HDR_BYTES, payload, len);
    *new_tail = tail + need;
    return 1;
}

int chan_ring_take(const uint8_t *ring, uint32_t size, uint64_t head, uint64_t tail, ChanMsg *m,
                   uint64_t *new_head) {
    uint64_t avail = tail - head;
    if (avail == 0) return 0;
    if (avail > size || avail < CHAN_HDR_BYTES) return -1;
    uint8_t hdr[CHAN_HDR_BYTES];
    ring_read(ring, size, head, hdr, CHAN_HDR_BYTES);
    uint32_t len = (uint32_t)hdr[0] | (uint32_t)hdr[1] << 8 | (uint32_t)hdr[2] << 16 | (uint32_t)hdr[3] << 24;
    if (len > CHAN_MSG_MAX || CHAN_HDR_BYTES + pad8(len) > avail) return -1;
    ring_read(ring, size, head + CHAN_HDR_BYTES, m->data, len);
    m->type = (uint16_t)((uint32_t)hdr[4] | (uint32_t)hdr[5] << 8);
    m->len = len;
    *new_head = head + CHAN_HDR_BYTES + pad8(len);
    return 1;
}

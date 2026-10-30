/* big_core.c - assembling bigmsg.h's messages (big_core.h).  No library
 * calls, bounded loops: proved with Frama-C/WP (tools/prove.sh). */
#include "big_core.h"

uint32_t big_core_limit(uint32_t limit) {
    return limit == 0 || limit > BIG_MAX ? BIG_MAX : limit;
}

int big_core_part(BigAsm *s, uint32_t limit, uint16_t ptype, const uint8_t *part, uint32_t len, uint8_t *buf,
                  uint32_t cap, uint32_t *need) {
    uint32_t lim = big_core_limit(limit);
    uint32_t off = 0, total = s->total, have = s->have;
    if (ptype != BIG_MORE) {
        if (s->busy || len < BIG_HDR) return BIG_REFUSED;
        /* Little-endian, as sums the provers relate to le32 directly: at most
         * 0xffffffff, so no wraparound. */
        total = part[0] + part[1] * 0x100u + part[2] * 0x10000u + part[3] * 0x1000000u;
        if (total > lim || len - BIG_HDR > total) return BIG_REFUSED;
        have = 0;
        off = BIG_HDR;
    } else if (!s->busy || have > total || total > lim) {
        return BIG_REFUSED;
    }
    uint32_t n = len - off;
    if (n > total - have) return BIG_REFUSED;
    /* At most total + 1 <= lim + 1 <= BIG_MAX + 1: no wraparound. */
    *need = have + n + 1;
    if (cap < *need) return BIG_GROW;
    /*@ loop invariant 0 <= i <= n;
        loop invariant \forall integer j; 0 <= j < i ==> buf[have + j] == part[off + j];
        loop assigns i, buf[have .. have + n - 1];
        loop variant n - i;
     */
    for (uint32_t i = 0; i < n; i++) buf[have + i] = part[off + i];
    have += n;
    if (ptype != BIG_MORE) s->type = ptype;
    s->total = total;
    s->have = have;
    if (have == total) {
        buf[total] = 0;
        s->busy = 0;
        return BIG_DONE;
    }
    s->busy = 1;
    return BIG_PART;
}

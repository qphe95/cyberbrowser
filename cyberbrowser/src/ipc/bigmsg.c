/* bigmsg.c - messages of any size over libchan (bigmsg.h). */
#include "bigmsg.h"

#include <stdlib.h>
#include <string.h>

bool big_send(Chan *c, uint16_t type, const void *a, uint32_t alen, const void *b, uint32_t blen,
              const void *d, uint32_t dlen, DWORD timeout_ms) {
    uint64_t total = (uint64_t)alen + blen + dlen;
    if (total > BIG_MAX || type == BIG_MORE) return false;
    const uint8_t *piece[3] = { (const uint8_t *)a, (const uint8_t *)b, (const uint8_t *)d };
    uint32_t plen[3] = { a ? alen : 0, b ? blen : 0, d ? dlen : 0 };
    static __thread uint8_t part[CHAN_MSG_MAX];
    uint32_t t32 = (uint32_t)total;
    memcpy(part, &t32, 4);
    uint32_t used = 4;
    uint16_t ptype = type;
    for (int k = 0; k < 3; k++) {
        uint32_t off = 0;
        while (off < plen[k]) {
            uint32_t n = plen[k] - off;
            if (n > CHAN_MSG_MAX - used) n = CHAN_MSG_MAX - used;
            memcpy(part + used, piece[k] + off, n);
            used += n;
            off += n;
            if (used == CHAN_MSG_MAX) {
                if (!chan_send(c, ptype, part, used, timeout_ms)) return false;
                ptype = BIG_MORE;
                used = 0;
            }
        }
    }
    /* The last part (for an empty message, the first). */
    if (used || ptype != BIG_MORE) return chan_send(c, ptype, part, used, timeout_ms);
    return true;
}

int big_recv(Chan *c, BigRx *rx) {
    for (;;) {
        int r = chan_recv(c, &rx->part);
        if (r <= 0) return r;
        BigAsm s = { rx->type, rx->total, rx->have, rx->busy };
        uint32_t need = 0;
        int got = big_core_part(&s, rx->limit, rx->part.type, rx->part.data, rx->part.len, rx->buf, rx->cap, &need);
        if (got == BIG_GROW) {
            /* The buffer grows with what arrives, not with what the sender
             * claims: a peer that announces 256 MB and stops costs us little.
             * need is at most the limit plus one (big_core.h). */
            uint32_t most = big_core_limit(rx->limit) + 1, cap = rx->cap ? rx->cap : 4096;
            if (cap > most) cap = most;
            while (cap < need) cap = cap > most / 2 ? most : cap * 2;
            uint8_t *nb = (uint8_t *)realloc(rx->buf, cap);
            if (!nb) return -1;
            rx->buf = nb;
            rx->cap = cap;
            got = big_core_part(&s, rx->limit, rx->part.type, rx->part.data, rx->part.len, rx->buf, rx->cap, &need);
        }
        if (got < 0 || got == BIG_GROW) return -1;
        rx->type = s.type;
        rx->total = s.total;
        rx->have = s.have;
        rx->busy = s.busy != 0;
        if (got == BIG_DONE) return 1;
    }
}

void big_free(BigRx *rx) {
    free(rx->buf);
    rx->buf = NULL;
    rx->cap = 0;
    rx->busy = false;
}

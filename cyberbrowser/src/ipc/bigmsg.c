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
        const uint8_t *p = rx->part.data;
        uint32_t n = rx->part.len;
        if (rx->part.type != BIG_MORE) {
            if (rx->busy || n < 4) return -1;
            uint32_t total;
            memcpy(&total, p, 4);
            if (total > BIG_MAX || n - 4 > total) return -1;
            free(rx->buf);
            rx->buf = (uint8_t *)malloc((size_t)total + 1);
            if (!rx->buf) return -1;
            rx->type = rx->part.type;
            rx->total = total;
            rx->have = 0;
            rx->busy = true;
            p += 4;
            n -= 4;
        } else if (!rx->busy) {
            return -1;
        }
        if (n > rx->total - rx->have) return -1;
        memcpy(rx->buf + rx->have, p, n);
        rx->have += n;
        if (rx->have == rx->total) {
            rx->buf[rx->total] = 0;
            rx->busy = false;
            return 1;
        }
    }
}

void big_free(BigRx *rx) {
    free(rx->buf);
    rx->buf = NULL;
    rx->busy = false;
}

#ifndef BIGMSG_H
#define BIGMSG_H

/* Messages of any size over a libchan channel: the first part has the
 * message's type and its total size, the rest have type BIG_MORE.  A sender
 * on a channel shared by several threads holds its lock for the whole
 * message, so the parts of one message arrive together and the receiver
 * assembles one message at a time. */

#include "chan.h"

#define BIG_MORE 0xffff
#define BIG_MAX (256u << 20)        /* largest message */

/* Send the concatenation of up to three pieces (any may be NULL / 0) as one
 * message.  timeout_ms bounds each part's wait for room. */
bool big_send(Chan *c, uint16_t type, const void *a, uint32_t alen, const void *b, uint32_t blen,
              const void *d, uint32_t dlen, DWORD timeout_ms);

typedef struct BigRx {
    uint16_t type;
    uint8_t *buf;                   /* the whole message, NUL-terminated after `total` */
    uint32_t total, have;
    bool busy;                      /* a message is being assembled */
    ChanMsg part;
} BigRx;

/* Take parts from the channel: 1 when a whole message is in rx (until the
 * next call), 0 none complete yet, -1 the channel or the message is broken. */
int big_recv(Chan *c, BigRx *rx);
void big_free(BigRx *rx);

#endif

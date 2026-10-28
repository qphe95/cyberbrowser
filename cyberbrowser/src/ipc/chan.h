#ifndef CHAN_H
#define CHAN_H

/* libchan: a channel between two processes (SANDBOX_DESIGN.md section 4.2).
 *
 * One shared section holds a ring per direction; each message is an 8-byte
 * header (payload length, type) and its payload, padded to 8 bytes.  Head
 * and tail are byte counters that only grow.  A side that is about to sleep
 * sets its `waiting` flag and the other side signals its event only then, so
 * a busy channel moves messages with no system call.
 *
 * The peer is untrusted: it can rewrite the shared memory at any time.  Every
 * index and length is read once, checked, and a message is copied out before
 * it is returned; a corrupt ring marks the channel broken. */

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

#include "chan_ring.h"

typedef struct ChanShared ChanShared;

typedef struct Chan {
    HANDLE section;
    HANDLE ev[2];                   /* ev[s] wakes side s */
    int side;                       /* 0 or 1: side 0 sends on ring 0 */
    uint32_t ring_bytes;            /* a power of two */
    ChanShared *sh;
    uint8_t *data[2];               /* ring data, per direction */
    bool broken;                    /* the peer corrupted the ring */
} Chan;

/* Create a channel and be its side 0.  ring_bytes: a power of two, at least
 * 2 * (CHAN_MSG_MAX + 8). */
bool chan_create(Chan *c, uint32_t ring_bytes);
/* Use a channel created by another process (handles valid here). */
bool chan_attach(Chan *c, HANDLE section, HANDLE ev0, HANDLE ev1, int side);
void chan_close(Chan *c);

/* Send a message; waits (yielding) while the ring is full, up to timeout_ms.
 * False if it didn't fit in time, the payload is too large, or the channel
 * is broken. */
bool chan_send(Chan *c, uint16_t type, const void *payload, uint32_t len, DWORD timeout_ms);
/* Take the next message: 1 got one, 0 none waiting, -1 broken. */
int chan_recv(Chan *c, ChanMsg *m);

/* Before sleeping on chan_event(): announce it.  False if a message is
 * already waiting (don't sleep).  chan_disarm after waking. */
bool chan_arm(Chan *c);
void chan_disarm(Chan *c);
HANDLE chan_event(Chan *c);
/* Wait up to ms for a message (arm, wait, disarm).  True if one may be waiting. */
bool chan_wait(Chan *c, DWORD ms);

/* Make the channel's handles inheritable (for a child created with them). */
void chan_inheritable(Chan *c, bool on);

#endif

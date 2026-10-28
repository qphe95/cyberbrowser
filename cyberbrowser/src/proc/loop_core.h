#ifndef LOOP_CORE_H
#define LOOP_CORE_H

/* The loopback relay's decisions (SANDBOX_DESIGN.md section 6.4), as a pure
 * state machine like the coordinator's: loop_step takes the state and one
 * event and fills a list of actions.  The shim (loop_main.c) turns the
 * network process's messages and its sockets' readiness into events and
 * performs the actions with Winsock.
 *
 * The network process, in an AppContainer that can't reach loopback, asks
 * the relay to connect to a port on 127.0.0.1 or ::1 and passes bytes both
 * ways; the relay doesn't read them.  Flow control keeps the relay's buffers
 * fixed: the network process sends the next NL_DATA of a connection only
 * after LN_SENT for the last one, and the relay reads from a socket only while
 * the bytes it has passed on and the network process hasn't credited back
 * (NL_CREDIT) stay under LOOP_WINDOW.
 *
 * Proved (M4, section 8.3): P1, P7, and P8: every connection the relay makes
 * is to a loopback address, and the bytes it forwards are those it was given,
 * unchanged. */

#include <stdbool.h>
#include <stdint.h>
#include "proto.h"

#define LOOP_MAX_CONNS 60           /* the shim waits on them all at once */
#define LOOP_WINDOW (256u * 1024)   /* uncredited bytes to the network process, per connection */

typedef enum { LS_FREE = 0, LS_CONNECTING, LS_OPEN } LoopConnState;

typedef struct LoopConn {
    LoopConnState state;
    uint32_t id;                    /* the network process's id for it */
    uint32_t uncredited;            /* bytes passed to the network process, not yet credited */
    bool sending;                   /* an NL_DATA is being written to the socket */
} LoopConn;

typedef struct LoopState {
    LoopConn c[LOOP_MAX_CONNS];
} LoopState;

typedef enum {
    LE_NET_MESSAGE,                 /* type, data, len: a message from the network process */
    LE_CONNECTED,                   /* slot, ok: LA_CONNECT finished */
    LE_SOCK_DATA,                   /* slot, data, len (1..LOOP_DATA_MAX): read from the socket */
    LE_SOCK_SENT,                   /* slot: LA_SOCK_SEND's bytes are all written */
    LE_SOCK_CLOSED,                 /* slot: the socket ended (closed, reset, failed) */
} LoopEventKind;

typedef struct LoopEvent {
    LoopEventKind kind;
    int slot;
    bool ok;
    uint16_t type;
    const uint8_t *data;
    uint32_t len;
} LoopEvent;

typedef enum {
    LA_CONNECT,                     /* slot, family (LOOP_V4 or LOOP_V6), port */
    LA_SOCK_SEND,                   /* slot, data (len bytes) */
    LA_SOCK_CLOSE,                  /* slot: close the socket, the slot is free */
    LA_NET_SEND,                    /* type, data (len bytes): to the network process */
} LoopActionKind;

typedef struct LoopAction {
    LoopActionKind kind;
    int slot;
    uint32_t family;
    uint16_t port;
    uint16_t type;
    uint32_t len;
    uint8_t data[4 + LOOP_DATA_MAX];
} LoopAction;

#define LOOP_MAX_ACTIONS 2

typedef struct LoopActions {
    int n;
    LoopAction a[LOOP_MAX_ACTIONS];
} LoopActions;

/* What M4 proves of loop_step (SANDBOX_DESIGN.md section 8.3), in ACSL. */
/*@ logic integer u32le{L}(uint8_t *p) = p[0] + p[1] * 0x100 + p[2] * 0x10000 + p[3] * 0x1000000;

    predicate lstate_ok(integer st) = st == LS_FREE || st == LS_CONNECTING || st == LS_OPEN;

    predicate loop_ok{L}(LoopState *s) =
        \forall integer i; 0 <= i < LOOP_MAX_CONNS ==>
            lstate_ok(s->c[i].state) && s->c[i].uncredited <= LOOP_WINDOW;

    // A connection id names one connection at a time.
    predicate ids_unique{L}(LoopState *s) =
        \forall integer i, j; 0 <= i < j < LOOP_MAX_CONNS &&
            s->c[i].state != LS_FREE && s->c[j].state != LS_FREE ==> s->c[i].id != s->c[j].id;

    // P8: every connection is to a loopback address (the shim makes
    // LOOP_V4 127.0.0.1 and LOOP_V6 ::1, and nothing else); every socket
    // action names a slot; every message fits.
    predicate act_ok{L}(LoopAction *a) =
        (a->kind == LA_CONNECT ==> (a->family == LOOP_V4 || a->family == LOOP_V6) && 1 <= a->port) &&
        (a->kind != LA_NET_SEND ==> 0 <= a->slot < LOOP_MAX_CONNS) &&
        (a->kind == LA_SOCK_SEND ==> 1 <= a->len <= LOOP_DATA_MAX) &&
        (a->kind == LA_NET_SEND ==> 4 <= a->len <= 4 + LOOP_DATA_MAX);

    // P8: bytes written to a socket are those of an NL_DATA message, unchanged.
    predicate sock_bytes_from_net{L}(LoopEvent *e, LoopAction *a) =
        a->kind == LA_SOCK_SEND ==>
            e->kind == LE_NET_MESSAGE && e->type == NL_DATA && a->len == e->len - 4 &&
            \forall integer j; 0 <= j < a->len ==> a->data[j] == e->data[4 + j];

    predicate no_ln_data{L}(LoopAction *a) = a->kind == LA_NET_SEND ==> a->type != LN_DATA;
 */

/*@ requires \valid(s);
    assigns s->c[0 .. LOOP_MAX_CONNS - 1];
    ensures loop_ok(s) && ids_unique(s);
 */
void loop_init(LoopState *s);

/*@ requires \valid(s) && \valid(out) && \valid_read(e) && \separated(s, out, e);
    requires (e->kind == LE_NET_MESSAGE || e->kind == LE_SOCK_DATA) ==>
        \valid_read(e->data + (0 .. e->len - 1)) && \separated(s, out, e, e->data + (0 .. e->len - 1));
    requires loop_ok(s) && ids_unique(s);
    assigns *s, *out;
    ensures loop_ok(s) && ids_unique(s);
    ensures 0 <= out->n <= LOOP_MAX_ACTIONS;
    ensures \forall integer k; 0 <= k < out->n ==> act_ok(&out->a[k]) && sock_bytes_from_net(e, &out->a[k]);
    // P8: the bytes go to the connection the network process named, an open one.
    ensures \forall integer k; 0 <= k < out->n && out->a[k].kind == LA_SOCK_SEND ==>
        \let i = out->a[k].slot; \at(s->c[i].state, Old) == LS_OPEN && \at(s->c[i].id, Old) == u32le(e->data);
    // P8: bytes for the network process (LN_DATA) are those read from that
    // connection's socket, unchanged, under that connection's id.
    ensures \forall integer k; 0 <= k < out->n && out->a[k].kind == LA_NET_SEND && out->a[k].type == LN_DATA ==>
        e->kind == LE_SOCK_DATA && out->a[k].len == 4 + e->len &&
        \at(s->c[e->slot].state, Old) == LS_OPEN && u32le(&out->a[k].data[0]) == \at(s->c[e->slot].id, Old) &&
        \forall integer j; 0 <= j < e->len ==> out->a[k].data[4 + j] == e->data[j];
 */
void loop_step(LoopState *s, const LoopEvent *e, LoopActions *out);

/* Should the shim read from slot's socket: the connection is open and the
 * network process has room for another LOOP_DATA_MAX bytes. */
/*@ requires \valid_read(s);
    assigns \nothing;
    ensures \result ==> 0 <= slot < LOOP_MAX_CONNS && s->c[slot].state == LS_OPEN &&
        s->c[slot].uncredited <= LOOP_WINDOW - LOOP_DATA_MAX;
 */
bool loop_want_read(const LoopState *s, int slot);

#endif

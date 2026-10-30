/* loop_core_model.c - loop_core.c's contracts (loop_core.h), as code Eva
 * runs in place of the proved functions (see chan_ring_model.c for why
 * that is sound: WP proves loop_core.c meets these contracts, P1, P7, P8).
 * Each body does what its postconditions allow, nothing narrower: any
 * state that keeps loop_ok, and any actions that act_ok allows (a slot for
 * every socket action, a loopback family and a port for a connect, a length
 * for every message), with any bytes.  Eva checks the preconditions at the
 * shim's calls (loop_main.c). */
#include "loop_core.h"
#include "__fc_builtin.h"

static void any_state(LoopState *s) {
    for (int i = 0; i < LOOP_MAX_CONNS; i++) {
        s->c[i].state = (LoopConnState)Frama_C_interval(LS_FREE, LS_OPEN);
        s->c[i].id = Frama_C_unsigned_int_interval(0, 0xffffffffu);
        s->c[i].uncredited = Frama_C_unsigned_int_interval(0, LOOP_WINDOW);
        s->c[i].sending = Frama_C_nondet(0, 1);
    }
}

void loop_init(LoopState *s) {
    for (int i = 0; i < LOOP_MAX_CONNS; i++) {
        s->c[i].state = LS_FREE;
        s->c[i].id = 0;
        s->c[i].uncredited = 0;
        s->c[i].sending = false;
    }
}

void loop_step(LoopState *s, const LoopEvent *e, LoopActions *out) {
    if ((e->kind == LE_NET_MESSAGE || e->kind == LE_SOCK_DATA) && e->len) {
        uint8_t first = e->data[0], last = e->data[e->len - 1];
        (void)first;
        (void)last;
    }
    any_state(s);
    out->n = Frama_C_interval(0, LOOP_MAX_ACTIONS);
    for (int k = 0; k < LOOP_MAX_ACTIONS; k++) {
        LoopAction *a = &out->a[k];
        a->kind = (LoopActionKind)Frama_C_interval(LA_CONNECT, LA_NET_SEND);
        a->slot = a->kind == LA_NET_SEND ? Frama_C_interval(-2147483647 - 1, 2147483647)
                                         : Frama_C_interval(0, LOOP_MAX_CONNS - 1);
        a->family = Frama_C_nondet(0, 1) ? LOOP_V4 : LOOP_V6;
        a->port = (uint16_t)Frama_C_interval(1, 65535);
        a->type = (uint16_t)Frama_C_interval(0, 0xffff);
        a->len = a->kind == LA_SOCK_SEND ? Frama_C_unsigned_int_interval(1, LOOP_DATA_MAX)
               : a->kind == LA_NET_SEND ? Frama_C_unsigned_int_interval(4, 4 + LOOP_DATA_MAX)
                                        : Frama_C_unsigned_int_interval(0, 0xffffffffu);
        Frama_C_make_unknown((char *)a->data, sizeof(a->data));
    }
}

bool loop_want_read(const LoopState *s, int slot) {
    if (slot < 0 || slot >= LOOP_MAX_CONNS || s->c[slot].state != LS_OPEN) return false;
    return Frama_C_nondet(0, 1);
}

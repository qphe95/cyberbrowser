/* loop_core.c - the loopback relay's decisions (loop_core.h).  No I/O, no
 * library calls, no allocation, bounded loops: proved with Frama-C/WP
 * (tools/prove.sh). */
#include "loop_core.h"

/*@ requires \valid_read(p + (0 .. 3));
    assigns \nothing;
    ensures \result == u32le(p);
 */
static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] + (uint32_t)p[1] * 0x100u + (uint32_t)p[2] * 0x10000u + (uint32_t)p[3] * 0x1000000u;
}

/*@ requires \valid(p + (0 .. 3));
    assigns p[0 .. 3];
    ensures u32le(p) == v;
 */
static void write_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v % 0x100u);
    p[1] = (uint8_t)(v / 0x100u % 0x100u);
    p[2] = (uint8_t)(v / 0x10000u % 0x100u);
    p[3] = (uint8_t)(v / 0x1000000u);
}

/*@ requires \valid(out) && 0 <= out->n <= LOOP_MAX_ACTIONS;
    assigns out->n, out->a[out->n];
    ensures \old(out->n) < LOOP_MAX_ACTIONS ==>
        \result == &out->a[\old(out->n)] && out->n == \old(out->n) + 1 &&
        \result->kind == kind && \result->slot == slot && \result->type == 0 && \result->len == 0 &&
        \result->family == 0;
    ensures \old(out->n) >= LOOP_MAX_ACTIONS ==> \result == \null && out->n == \old(out->n);
 */
static LoopAction *add(LoopActions *out, LoopActionKind kind, int slot) {
    if (out->n >= LOOP_MAX_ACTIONS) return 0;
    LoopAction *a = &out->a[out->n++];
    a->kind = kind;
    a->slot = slot;
    a->family = 0;
    a->port = 0;
    a->type = 0;
    a->len = 0;
    return a;
}

/* To the network process: type, the id, then n bytes from p. */
/*@ requires \valid(out) && 0 <= out->n <= LOOP_MAX_ACTIONS;
    requires n <= LOOP_DATA_MAX && \valid_read(p + (0 .. n - 1));
    requires \separated(out, p + (0 .. n - 1));
    assigns out->n, out->a[out->n];
    ensures \old(out->n) <= out->n <= \old(out->n) + 1;
    ensures out->n == \old(out->n) + 1 ==>
        \let a = &out->a[\old(out->n)];
        a->kind == LA_NET_SEND && a->type == type && a->len == 4 + n && u32le(&a->data[0]) == id &&
        \forall integer i; 0 <= i < n ==> a->data[4 + i] == p[i];
 */
static void net_send(LoopActions *out, uint16_t type, uint32_t id, const uint8_t *p, uint32_t n) {
    LoopAction *a = add(out, LA_NET_SEND, -1);
    if (!a) return;
    a->type = type;
    a->len = 4 + n;
    write_u32(a->data, id);
    /*@ loop invariant 0 <= i <= n;
        loop invariant u32le(&a->data[0]) == id;
        loop invariant \forall integer k; 0 <= k < i ==> a->data[4 + k] == p[k];
        loop assigns i, a->data[4 .. 4 + n - 1];
        loop variant n - i;
     */
    for (uint32_t i = 0; i < n; i++) a->data[4 + i] = p[i];
}

/*@ requires \valid(out) && 0 <= out->n <= LOOP_MAX_ACTIONS;
    requires type != LN_DATA;
    assigns out->n, out->a[out->n];
    ensures \old(out->n) <= out->n <= \old(out->n) + 1;
    ensures out->n == \old(out->n) + 1 ==>
        out->a[\old(out->n)].kind == LA_NET_SEND && out->a[\old(out->n)].type == type &&
        out->a[\old(out->n)].len == 4;
 */
static void net_send_id(LoopActions *out, uint16_t type, uint32_t id) {
    uint8_t none[1] = { 0 };
    net_send(out, type, id, none, 0);
}

/*@ requires \valid(out) && 0 <= out->n <= LOOP_MAX_ACTIONS;
    assigns out->n, out->a[out->n];
    ensures \old(out->n) <= out->n <= \old(out->n) + 1;
    ensures out->n == \old(out->n) + 1 ==>
        out->a[\old(out->n)].kind == LA_NET_SEND && out->a[\old(out->n)].type == LN_CONNECTED &&
        out->a[\old(out->n)].len == 8;
 */
static void net_send_connected(LoopActions *out, uint32_t id, bool ok) {
    uint8_t v[4];
    write_u32(v, ok ? 1 : 0);
    net_send(out, LN_CONNECTED, id, v, 4);
}

/*@ requires \valid_read(s);
    assigns \nothing;
    ensures -1 <= \result < LOOP_MAX_CONNS;
    ensures \result >= 0 ==> s->c[\result].state != LS_FREE && s->c[\result].id == id;
    ensures \result < 0 ==> \forall integer i; 0 <= i < LOOP_MAX_CONNS && s->c[i].state != LS_FREE ==>
        s->c[i].id != id;
 */
static int slot_of_id(const LoopState *s, uint32_t id) {
    /*@ loop invariant 0 <= i <= LOOP_MAX_CONNS;
        loop invariant \forall integer k; 0 <= k < i && s->c[k].state != LS_FREE ==> s->c[k].id != id;
        loop assigns i;
        loop variant LOOP_MAX_CONNS - i;
     */
    for (int i = 0; i < LOOP_MAX_CONNS; i++)
        if (s->c[i].state != LS_FREE && s->c[i].id == id) return i;
    return -1;
}

/*@ requires \valid(s) && 0 <= i < LOOP_MAX_CONNS && loop_ok(s) && ids_unique(s);
    assigns s->c[i];
    ensures s->c[i].state == LS_FREE;
    ensures loop_ok(s) && ids_unique(s);
 */
static void free_slot(LoopState *s, int i) {
    s->c[i].state = LS_FREE;
    s->c[i].id = 0;
    s->c[i].uncredited = 0;
    s->c[i].sending = false;
}

void loop_init(LoopState *s) {
    /*@ loop invariant 0 <= i <= LOOP_MAX_CONNS;
        loop invariant \forall integer k; 0 <= k < i ==> s->c[k].state == LS_FREE && s->c[k].uncredited == 0;
        loop assigns i, s->c[0 .. LOOP_MAX_CONNS - 1];
        loop variant LOOP_MAX_CONNS - i;
     */
    for (int i = 0; i < LOOP_MAX_CONNS; i++) {
        s->c[i].state = LS_FREE;
        s->c[i].id = 0;
        s->c[i].uncredited = 0;
        s->c[i].sending = false;
    }
}

bool loop_want_read(const LoopState *s, int slot) {
    if (slot < 0 || slot >= LOOP_MAX_CONNS) return false;
    const LoopConn *c = &s->c[slot];
    return c->state == LS_OPEN && c->uncredited <= LOOP_WINDOW - LOOP_DATA_MAX;
}

/*@ requires \valid(s) && 0 <= i < LOOP_MAX_CONNS && loop_ok(s) && ids_unique(s);
    requires s->c[i].state == LS_FREE;
    requires \forall integer j; 0 <= j < LOOP_MAX_CONNS && s->c[j].state != LS_FREE ==> s->c[j].id != id;
    assigns s->c[i];
    ensures s->c[i].state == LS_CONNECTING && s->c[i].id == id;
    ensures loop_ok(s) && ids_unique(s);
 */
static void use_slot(LoopState *s, int i, uint32_t id) {
    s->c[i].state = LS_CONNECTING;
    s->c[i].id = id;
    s->c[i].uncredited = 0;
    s->c[i].sending = false;
}

/* NL_CONNECT: a connection to port of 127.0.0.1 or ::1, nothing else. */
/*@ requires \valid(s) && \valid(out) && \valid_read(m + (0 .. 11)) && \separated(s, out, m + (0 .. 11));
    requires loop_ok(s) && ids_unique(s) && out->n == 0;
    assigns s->c[0 .. LOOP_MAX_CONNS - 1], out->n, out->a[0];
    ensures loop_ok(s) && ids_unique(s);
    ensures 0 <= out->n <= 1;
    ensures out->n == 1 ==>
        (out->a[0].kind == LA_CONNECT && 0 <= out->a[0].slot < LOOP_MAX_CONNS &&
         (out->a[0].family == LOOP_V4 || out->a[0].family == LOOP_V6) && 1 <= out->a[0].port) ||
        (out->a[0].kind == LA_NET_SEND && out->a[0].type == LN_CONNECTED && out->a[0].len == 8);
 */
static void on_connect(LoopState *s, LoopActions *out, const uint8_t *m) {
    uint32_t id = read_u32(m), family = read_u32(m + 4), port = read_u32(m + 8);
    int free = -1;
    /*@ loop invariant 0 <= k <= LOOP_MAX_CONNS;
        loop invariant -1 <= free < LOOP_MAX_CONNS;
        loop invariant free >= 0 ==> s->c[free].state == LS_FREE;
        loop assigns k, free;
        loop variant LOOP_MAX_CONNS - k;
     */
    for (int k = 0; k < LOOP_MAX_CONNS && free < 0; k++)
        if (s->c[k].state == LS_FREE) free = k;
    if (slot_of_id(s, id) >= 0 || free < 0 || (family != LOOP_V4 && family != LOOP_V6) || port == 0 ||
        port > 65535) {
        net_send_connected(out, id, false);
        return;
    }
    use_slot(s, free, id);
    LoopAction *a = add(out, LA_CONNECT, free);
    a->family = family;
    a->port = (uint16_t)port;
}

/* NL_DATA: the bytes after the id, to that connection's socket, unchanged. */
/*@ requires \valid(s) && \valid(out) && 4 <= len && \valid_read(m + (0 .. len - 1));
    requires \separated(s, out, m + (0 .. len - 1));
    requires loop_ok(s) && ids_unique(s) && out->n == 0;
    requires 0 <= i < LOOP_MAX_CONNS && s->c[i].state != LS_FREE && s->c[i].id == u32le(m);
    assigns s->c[i], out->n, out->a[0 .. 1];
    ensures loop_ok(s) && ids_unique(s);
    ensures 0 <= out->n <= 2;
    ensures \forall integer k; 0 <= k < out->n ==>
        (out->a[k].kind == LA_SOCK_SEND && out->a[k].slot == i && \old(s->c[i].state) == LS_OPEN &&
         \old(s->c[i].id) == u32le(m) && 1 <= out->a[k].len <= LOOP_DATA_MAX &&
         out->a[k].len == len - 4 && \forall integer j; 0 <= j < len - 4 ==> out->a[k].data[j] == m[4 + j]) ||
        (out->a[k].kind == LA_SOCK_CLOSE && out->a[k].slot == i) ||
        (out->a[k].kind == LA_NET_SEND && out->a[k].type == LN_CLOSED && out->a[k].len == 4);
 */
static void on_data(LoopState *s, LoopActions *out, int i, const uint8_t *m, uint32_t len) {
    uint32_t n = len - 4;
    if (s->c[i].state != LS_OPEN || s->c[i].sending || n == 0 || n > LOOP_DATA_MAX) {
        /* Out of turn: the network process broke the protocol, so the
         * connection ends. */
        uint32_t id = s->c[i].id;
        free_slot(s, i);
        add(out, LA_SOCK_CLOSE, i);
        net_send_id(out, LN_CLOSED, id);
        return;
    }
    LoopAction *a = add(out, LA_SOCK_SEND, i);
    a->len = n;
    /*@ loop invariant 0 <= k <= n;
        loop invariant \forall integer j; 0 <= j < k ==> a->data[j] == m[4 + j];
        loop assigns k, a->data[0 .. n - 1];
        loop variant n - k;
     */
    for (uint32_t k = 0; k < n; k++) a->data[k] = m[4 + k];
    s->c[i].sending = true;
}

/*@ requires \valid(s) && \valid(out) && \valid_read(e) && e->kind == LE_NET_MESSAGE;
    requires \valid_read(e->data + (0 .. e->len - 1));
    requires \separated(s, out, e, e->data + (0 .. e->len - 1));
    requires loop_ok(s) && ids_unique(s) && out->n == 0;
    assigns s->c[0 .. LOOP_MAX_CONNS - 1], out->n, out->a[0 .. 1];
    ensures loop_ok(s) && ids_unique(s);
    ensures 0 <= out->n <= 2;
    ensures \forall integer k; 0 <= k < out->n ==>
        act_ok(&out->a[k]) && sock_bytes_from_net(e, &out->a[k]) && no_ln_data(&out->a[k]);
    ensures \forall integer k; 0 <= k < out->n && out->a[k].kind == LA_SOCK_SEND ==>
        \let i = out->a[k].slot; \at(s->c[i].state, Old) == LS_OPEN && \at(s->c[i].id, Old) == u32le(e->data);
 */
static void net_message(LoopState *s, const LoopEvent *e, LoopActions *out) {
    if (e->len < 4) return;
    uint32_t id = read_u32(e->data);
    int i = slot_of_id(s, id);
    switch (e->type) {
    case NL_CONNECT:
        if (e->len != 12) return;
        on_connect(s, out, e->data);
        return;
    case NL_DATA:
        if (i < 0) return;
        on_data(s, out, i, e->data, e->len);
        return;
    case NL_CREDIT: {
        if (i < 0 || e->len != 8) return;
        uint32_t bytes = read_u32(e->data + 4);
        s->c[i].uncredited -= bytes < s->c[i].uncredited ? bytes : s->c[i].uncredited;
        return;
    }
    case NL_CLOSE:
        if (i < 0 || e->len != 4) return;
        free_slot(s, i);
        add(out, LA_SOCK_CLOSE, i);
        return;
    default:
        return;
    }
}

void loop_step(LoopState *s, const LoopEvent *e, LoopActions *out) {
    out->n = 0;
    if (e->kind == LE_NET_MESSAGE) {
        net_message(s, e, out);
        return;
    }
    if (e->slot < 0 || e->slot >= LOOP_MAX_CONNS) return;
    int i = e->slot;
    uint32_t id = s->c[i].id;
    switch (e->kind) {
    case LE_CONNECTED:
        if (s->c[i].state != LS_CONNECTING) return;
        if (e->ok) {
            s->c[i].state = LS_OPEN;
            net_send_connected(out, id, true);
        } else {
            free_slot(s, i);
            add(out, LA_SOCK_CLOSE, i);
            net_send_connected(out, id, false);
        }
        return;
    case LE_SOCK_DATA:
        if (s->c[i].state != LS_OPEN || e->len == 0 || e->len > LOOP_DATA_MAX ||
            s->c[i].uncredited > LOOP_WINDOW - e->len)
            return;
        s->c[i].uncredited += e->len;
        net_send(out, LN_DATA, id, e->data, e->len);
        return;
    case LE_SOCK_SENT:
        if (s->c[i].state != LS_OPEN || !s->c[i].sending) return;
        s->c[i].sending = false;
        net_send_id(out, LN_SENT, id);
        return;
    case LE_SOCK_CLOSED: {
        if (s->c[i].state == LS_FREE) return;
        bool connecting = s->c[i].state == LS_CONNECTING;
        free_slot(s, i);
        add(out, LA_SOCK_CLOSE, i);
        if (connecting) net_send_connected(out, id, false);
        else net_send_id(out, LN_CLOSED, id);
        return;
    }
    default:
        return;
    }
}

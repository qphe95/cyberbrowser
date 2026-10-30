/* loop_main.c - cb_loop.exe: the loopback relay (SANDBOX_DESIGN.md section
 * 6.4), the Winsock shim around loop_core.c.
 *
 * Started by the coordinator with its control channel:
 *   cb_loop.exe --chan S E0 E1
 *
 * The coordinator grants it one channel, to the network process (GRANT_NET).
 * The network process runs in an AppContainer, which can't connect to
 * loopback; it asks the relay to connect to a port of 127.0.0.1 or ::1 and
 * passes bytes through it.  The core decides; this file waits on the channels
 * and the sockets, turns what happens into events and performs the actions.
 * It reads from a socket only while the core wants it (loop_want_read), and
 * writes one NL_DATA at a time, so its buffers are fixed. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "chan.h"
#include "proto.h"
#include "spawn.h"
#include "loop_core.h"

typedef struct Sock {
    SOCKET s;
    WSAEVENT ev;
    bool readable;                  /* FD_READ or FD_CLOSE seen, recv not yet refused */
    bool eof;                       /* FD_CLOSE seen */
    uint8_t out[LOOP_DATA_MAX];     /* the NL_DATA being written */
    uint32_t out_len, out_off;
} Sock;

static Chan g_ctl, g_net;
static bool g_have_net;
static LoopState g_state;
static LoopActions g_actions;
static Sock g_socks[LOOP_MAX_CONNS];

static void step(const LoopEvent *e);

/* Events that performing an action led to, stepped after it (step). */
static bool g_stepping;
static LoopEvent g_follow[LOOP_MAX_ACTIONS];
static int g_nfollow;

static void sock_close(int i) {
    Sock *k = &g_socks[i];
    if (k->s != INVALID_SOCKET) closesocket(k->s);
    if (k->ev != WSA_INVALID_EVENT) WSACloseEvent(k->ev);
    k->s = INVALID_SOCKET;
    k->ev = WSA_INVALID_EVENT;
    k->readable = k->eof = false;
    k->out_len = k->out_off = 0;
}

/* A socket's event: stepped now, or, while an action is being performed,
 * queued to be stepped after it. */
static void event_slot(LoopEventKind kind, int slot, bool ok) {
    LoopEvent e;
    memset(&e, 0, sizeof(e));
    e.kind = kind;
    e.slot = slot;
    e.ok = ok;
    if (!g_stepping) {
        step(&e);
        return;
    }
    if (g_nfollow == LOOP_MAX_ACTIONS) {
        /* Can't happen: loop_step's contract (see step). */
        fprintf(stderr, "[loop] more events than loop_step allows\n");
        abort();
    }
    g_follow[g_nfollow++] = e;
}

/* Connect slot i to the loopback address of family (the only addresses
 * there are here) and port, without blocking: FD_CONNECT finishes it. */
static bool sock_connect(int i, uint32_t family, uint16_t port) {
    Sock *k = &g_socks[i];
    struct sockaddr_storage a;
    int alen;
    memset(&a, 0, sizeof(a));
    if (family == LOOP_V4) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)&a;
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        v4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        alen = sizeof(*v4);
    } else if (family == LOOP_V6) {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&a;
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        v6->sin6_addr = in6addr_loopback;
        alen = sizeof(*v6);
    } else {
        return false;
    }
    k->s = socket(a.ss_family, SOCK_STREAM, IPPROTO_TCP);
    k->ev = WSACreateEvent();
    if (k->s == INVALID_SOCKET || k->ev == WSA_INVALID_EVENT ||
        WSAEventSelect(k->s, k->ev, FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE) != 0)
        return false;
    BOOL nodelay = TRUE;
    setsockopt(k->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
    if (connect(k->s, (struct sockaddr *)&a, alen) == 0) return true;
    return WSAGetLastError() == WSAEWOULDBLOCK;
}

/* Write what is left of slot i's NL_DATA; LE_SOCK_SENT once it's all out. */
static void sock_flush(int i) {
    Sock *k = &g_socks[i];
    while (k->out_off < k->out_len) {
        int n = send(k->s, (const char *)k->out + k->out_off, (int)(k->out_len - k->out_off), 0);
        if (n > 0) {
            k->out_off += (uint32_t)n;
            continue;
        }
        if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) return;     /* FD_WRITE comes back */
        event_slot(LE_SOCK_CLOSED, i, false);
        return;
    }
    if (k->out_len) {
        k->out_len = k->out_off = 0;
        event_slot(LE_SOCK_SENT, i, true);
    }
}

/* Read from slot i while the core wants it. */
static void sock_pump(int i) {
    static uint8_t buf[LOOP_DATA_MAX];
    Sock *k = &g_socks[i];
    while (k->s != INVALID_SOCKET && k->readable && loop_want_read(&g_state, i)) {
        int n = recv(k->s, (char *)buf, sizeof(buf), 0);
        if (n > 0) {
            LoopEvent e;
            memset(&e, 0, sizeof(e));
            e.kind = LE_SOCK_DATA;
            e.slot = i;
            e.data = buf;
            e.len = (uint32_t)n;
            step(&e);
            continue;
        }
        if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK && !k->eof) {
            k->readable = false;                /* FD_READ comes back */
            return;
        }
        event_slot(LE_SOCK_CLOSED, i, false);   /* the end, a reset, or an error */
        return;
    }
}

static void perform(const LoopAction *a) {
    switch (a->kind) {
    case LA_CONNECT:
        sock_close(a->slot);
        if (!sock_connect(a->slot, a->family, a->port)) event_slot(LE_CONNECTED, a->slot, false);
        break;
    case LA_SOCK_SEND: {
        Sock *k = &g_socks[a->slot];
        memcpy(k->out, a->data, a->len);
        k->out_len = a->len;
        k->out_off = 0;
        sock_flush(a->slot);
        break;
    }
    case LA_SOCK_CLOSE:
        sock_close(a->slot);
        break;
    case LA_NET_SEND:
        if (g_have_net && !chan_send(&g_net, a->type, a->data, a->len, 30000))
            fprintf(stderr, "[loop] the network process stopped reading\n");
        break;
    }
}

/* Step the core and perform its actions.  An action can lead to another
 * event: LA_CONNECT to LE_CONNECTED (a connect that failed at once), and
 * LA_SOCK_SEND to LE_SOCK_SENT or LE_SOCK_CLOSED; at most one each, which
 * event_slot queues.  They are stepped after that action, before this
 * step's next (the order M4's recursion had).  loop_step's contract says
 * such an event's actions are only LA_SOCK_CLOSE and LA_NET_SEND, which lead
 * to none: so every event is stepped, nothing goes deeper than one level,
 * and the queue never holds more than one. */
static void step(const LoopEvent *e) {
    g_stepping = true;
    loop_step(&g_state, e, &g_actions);
    LoopActions acts = g_actions;
    for (int i = 0; i < acts.n; i++) {
        g_nfollow = 0;
        perform(&acts.a[i]);
        for (int k = 0; k < g_nfollow; k++) {
            LoopEvent f = g_follow[k];
            loop_step(&g_state, &f, &g_actions);
            LoopActions more = g_actions;
            for (int j = 0; j < more.n; j++) perform(&more.a[j]);
        }
    }
    g_nfollow = 0;
    g_stepping = false;
}

static void grant(const CsGrant *g) {
    HANDLE s = (HANDLE)(uintptr_t)g->chan_section, e0 = (HANDLE)(uintptr_t)g->chan_ev0,
           e1 = (HANDLE)(uintptr_t)g->chan_ev1;
    if (g->kind != GRANT_NET || g_have_net || !chan_attach(&g_net, s, e0, e1, 1)) {
        CloseHandle(s); CloseHandle(e0); CloseHandle(e1);
        return;
    }
    g_have_net = true;
    fprintf(stderr, "[loop] serving the network process\n");
}

int main(int argc, char **argv) {
    if (argc < 5 || strcmp(argv[1], "--chan") ||
        !chan_attach(&g_ctl, spawn_handle_from_arg(argv[2]), spawn_handle_from_arg(argv[3]),
                     spawn_handle_from_arg(argv[4]), 1)) {
        fprintf(stderr, "cb_loop: bad arguments\n");
        return 1;
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "cb_loop: no Winsock\n"); return 1; }
    loop_init(&g_state);
    for (int i = 0; i < LOOP_MAX_CONNS; i++) {
        g_socks[i].s = INVALID_SOCKET;
        g_socks[i].ev = WSA_INVALID_EVENT;
    }
    static ChanMsg m;
    for (;;) {
        int r;
        while ((r = chan_recv(&g_ctl, &m)) > 0) {
            if (m.type == CS_GRANT && m.len == sizeof(CsGrant)) {
                CsGrant g;
                memcpy(&g, m.data, sizeof(g));
                grant(&g);
            } else if (m.type == CS_QUIT) {
                return 0;
            }
        }
        if (r < 0) return 1;
        while (g_have_net && (r = chan_recv(&g_net, &m)) > 0) {
            LoopEvent e;
            memset(&e, 0, sizeof(e));
            e.kind = LE_NET_MESSAGE;
            e.type = m.type;
            e.data = m.data;
            e.len = m.len;
            step(&e);
        }
        if (g_have_net && r < 0) {
            fprintf(stderr, "[loop] the network process broke its channel\n");
            return 1;
        }
        for (int i = 0; i < LOOP_MAX_CONNS; i++) sock_pump(i);
        fflush(stderr);

        HANDLE waits[2 + LOOP_MAX_CONNS];
        int slot_of[2 + LOOP_MAX_CONNS];
        int n = 0;
        bool ready = !chan_arm(&g_ctl);
        waits[n] = chan_event(&g_ctl); slot_of[n++] = -1;
        if (g_have_net) {
            if (!chan_arm(&g_net)) ready = true;
            waits[n] = chan_event(&g_net); slot_of[n++] = -1;
        }
        for (int i = 0; i < LOOP_MAX_CONNS; i++)
            if (g_socks[i].ev != WSA_INVALID_EVENT) { waits[n] = g_socks[i].ev; slot_of[n++] = i; }
        DWORD w = ready ? WAIT_TIMEOUT : WaitForMultipleObjects((DWORD)n, waits, FALSE, INFINITE);
        chan_disarm(&g_ctl);
        if (g_have_net) chan_disarm(&g_net);
        if (w >= WAIT_OBJECT_0 + (DWORD)n) continue;
        /* Every socket whose event is set, not only the first. */
        for (int j = (int)(w - WAIT_OBJECT_0); j < n; j++) {
            int i = slot_of[j];
            if (i < 0 || g_socks[i].s == INVALID_SOCKET) continue;
            WSANETWORKEVENTS ne;
            if (WSAEnumNetworkEvents(g_socks[i].s, g_socks[i].ev, &ne) != 0) continue;
            if (ne.lNetworkEvents & FD_CONNECT) {
                event_slot(LE_CONNECTED, i, ne.iErrorCode[FD_CONNECT_BIT] == 0);
                if (g_socks[i].s == INVALID_SOCKET) continue;
            }
            if (ne.lNetworkEvents & FD_WRITE) sock_flush(i);
            if (ne.lNetworkEvents & FD_READ) g_socks[i].readable = true;
            if (ne.lNetworkEvents & FD_CLOSE) g_socks[i].readable = g_socks[i].eof = true;
        }
    }
}

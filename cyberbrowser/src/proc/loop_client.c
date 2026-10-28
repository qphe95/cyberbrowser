/* loop_client.c - the network process's side of the loopback relay
 * (SANDBOX_DESIGN.md section 6.4; the relay is loop_main.c).
 *
 * The network process runs in an AppContainer, which can't reach loopback.
 * Once the coordinator grants it a channel to the relay, loop_client_start
 * routes loopback hosts through it (tls_set_loopback): each connection is an
 * id, and its bytes travel as NL_DATA and LN_DATA messages.  A reader thread
 * files what arrives by id; the engine's threads wait on their connections.
 *
 * Flow control mirrors loop_core.h: an NL_DATA only after LN_SENT for the
 * last one, and NL_CREDIT as bytes are consumed. */
#include "loop_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "chan.h"
#include "proto.h"
#include "net_loopback.h"

#define CREDIT_EVERY (64u * 1024)

typedef struct LConn {
    uint32_t id;
    int state;                      /* 0 connecting, 1 open, -1 refused */
    bool sending;                   /* an NL_DATA awaits its LN_SENT */
    bool eof;                       /* LN_CLOSED: no more bytes after these */
    uint8_t *buf;                   /* bytes received, from off to len */
    size_t off, len, cap;
    uint32_t uncredited;            /* consumed, not yet credited back */
    struct LConn *next;
} LConn;

static Chan g_ch;                   /* side 0 */
static CRITICAL_SECTION g_lock;     /* the connections */
static CRITICAL_SECTION g_send;     /* the channel's sending side */
static CONDITION_VARIABLE g_cv;     /* any connection changed */
static LConn *g_conns;
static uint32_t g_next_id = 1;
static volatile LONG g_broken;

static LConn *find(uint32_t id) {
    for (LConn *c = g_conns; c; c = c->next)
        if (c->id == id) return c;
    return NULL;
}

static bool send_msg(uint16_t type, const void *a, uint32_t alen, const void *b, uint32_t blen) {
    static uint8_t msg[CHAN_MSG_MAX];
    if (alen + blen > CHAN_MSG_MAX) return false;
    EnterCriticalSection(&g_send);
    memcpy(msg, a, alen);
    if (blen) memcpy(msg + alen, b, blen);
    bool ok = !g_broken && chan_send(&g_ch, type, msg, alen + blen, 30000);
    LeaveCriticalSection(&g_send);
    return ok;
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static DWORD WINAPI reader(void *arg) {
    (void)arg;
    static ChanMsg m;
    for (;;) {
        int r;
        while ((r = chan_recv(&g_ch, &m)) > 0) {
            if (m.len < 4) continue;
            EnterCriticalSection(&g_lock);
            LConn *c = find(read_u32(m.data));
            if (c) {
                if (m.type == LN_CONNECTED && m.len == 8) {
                    c->state = read_u32(m.data + 4) ? 1 : -1;
                } else if (m.type == LN_DATA && m.len > 4) {
                    size_t n = m.len - 4;
                    if (c->off == c->len) c->off = c->len = 0;
                    if (c->len + n > c->cap) {
                        size_t cap = c->cap ? c->cap : 65536;
                        while (cap < c->len + n) cap *= 2;
                        uint8_t *nb = (uint8_t *)realloc(c->buf, cap);
                        if (nb) { c->buf = nb; c->cap = cap; }
                    }
                    if (c->len + n <= c->cap) {
                        memcpy(c->buf + c->len, m.data + 4, n);
                        c->len += n;
                    } else {
                        c->eof = true;      /* out of memory: end it */
                    }
                } else if (m.type == LN_SENT) {
                    c->sending = false;
                } else if (m.type == LN_CLOSED) {
                    c->eof = true;
                    if (c->state == 0) c->state = -1;
                }
                WakeAllConditionVariable(&g_cv);
            }
            LeaveCriticalSection(&g_lock);
        }
        if (r < 0) {
            fprintf(stderr, "[net] the loopback relay broke its channel\n");
            EnterCriticalSection(&g_lock);
            g_broken = 1;
            for (LConn *c = g_conns; c; c = c->next) {
                c->eof = true;
                if (c->state == 0) c->state = -1;
            }
            WakeAllConditionVariable(&g_cv);
            LeaveCriticalSection(&g_lock);
            return 0;
        }
        chan_wait(&g_ch, 1000);
    }
}

/* Wait (holding g_lock) until cond holds or timeout_ms passes. */
#define WAIT_UNTIL(cond, timeout_ms)                                                   \
    do {                                                                               \
        DWORD start_ = GetTickCount();                                                 \
        while (!(cond)) {                                                              \
            DWORD el_ = GetTickCount() - start_;                                       \
            if (el_ >= (timeout_ms)) break;                                            \
            SleepConditionVariableCS(&g_cv, &g_lock, (timeout_ms) - el_);              \
        }                                                                              \
    } while (0)

static int try_connect(uint32_t family, uint32_t port) {
    LConn *c = (LConn *)calloc(1, sizeof(LConn));
    if (!c) return -1;
    EnterCriticalSection(&g_lock);
    c->id = g_next_id++;
    if (!g_next_id) g_next_id = 1;
    c->next = g_conns;
    g_conns = c;
    LeaveCriticalSection(&g_lock);
    NlConnect q = { c->id, family, port };
    bool sent = send_msg(NL_CONNECT, &q, sizeof(q), NULL, 0);
    EnterCriticalSection(&g_lock);
    if (sent) WAIT_UNTIL(c->state != 0, 30000);
    int id = c->state == 1 ? (int)c->id : -1;
    LeaveCriticalSection(&g_lock);
    if (id < 0) loop_client_close_id(c->id);
    return id;
}

static int lb_connect(const char *host, const char *port) {
    char *end = NULL;
    unsigned long p = strtoul(port, &end, 10);
    if (!end || *end || p == 0 || p > 65535) return -1;
    if (!strcmp(host, "::1") || !strcmp(host, "[::1]")) return try_connect(LOOP_V6, (uint32_t)p);
    if (!strcmp(host, "127.0.0.1")) return try_connect(LOOP_V4, (uint32_t)p);
    /* localhost: IPv4 first, as most local servers listen there. */
    int id = try_connect(LOOP_V4, (uint32_t)p);
    return id > 0 ? id : try_connect(LOOP_V6, (uint32_t)p);
}

static int lb_send(int id, const unsigned char *buf, size_t len, unsigned timeout_ms) {
    if (len == 0) return 0;
    size_t n = len < LOOP_DATA_MAX ? len : LOOP_DATA_MAX;
    EnterCriticalSection(&g_lock);
    LConn *c = find((uint32_t)id);
    if (c) WAIT_UNTIL(!c->sending || c->eof, timeout_ms);
    bool ok = c && !c->sending && !c->eof;
    if (ok) c->sending = true;
    LeaveCriticalSection(&g_lock);
    if (!ok) return -1;
    uint32_t hdr = (uint32_t)id;
    if (!send_msg(NL_DATA, &hdr, 4, buf, (uint32_t)n)) return -1;
    return (int)n;
}

static int lb_recv(int id, unsigned char *buf, size_t len, unsigned timeout_ms) {
    EnterCriticalSection(&g_lock);
    LConn *c = find((uint32_t)id);
    if (c) WAIT_UNTIL(c->off < c->len || c->eof, timeout_ms);
    if (!c || (c->off == c->len && !c->eof)) {
        LeaveCriticalSection(&g_lock);
        return -1;                                  /* timed out */
    }
    size_t n = c->len - c->off < len ? c->len - c->off : len;
    memcpy(buf, c->buf + c->off, n);
    c->off += n;
    c->uncredited += (uint32_t)n;
    uint32_t credit = 0;
    if (c->uncredited >= CREDIT_EVERY || (c->off == c->len && c->uncredited)) {
        credit = c->uncredited;
        c->uncredited = 0;
    }
    LeaveCriticalSection(&g_lock);
    if (credit) {
        NlCredit q = { (uint32_t)id, credit };
        send_msg(NL_CREDIT, &q, sizeof(q), NULL, 0);
    }
    return (int)n;                                  /* 0 at the end */
}

static bool lb_readable(int id) {
    EnterCriticalSection(&g_lock);
    LConn *c = find((uint32_t)id);
    bool r = !c || c->off < c->len || c->eof;
    LeaveCriticalSection(&g_lock);
    return r;
}

void loop_client_close_id(int id) {
    EnterCriticalSection(&g_lock);
    LConn **pp = &g_conns;
    while (*pp && (*pp)->id != (uint32_t)id) pp = &(*pp)->next;
    LConn *c = *pp;
    if (c) *pp = c->next;
    bool tell = c && c->state == 1 && !c->eof;
    LeaveCriticalSection(&g_lock);
    if (!c) return;
    if (tell) {
        uint32_t hdr = (uint32_t)id;
        send_msg(NL_CLOSE, &hdr, 4, NULL, 0);
    }
    free(c->buf);
    free(c);
}

static void lb_close(int id) { loop_client_close_id(id); }

static const TlsLoopback g_lb = { lb_connect, lb_send, lb_recv, lb_readable, lb_close };

bool loop_client_start(HANDLE section, HANDLE ev0, HANDLE ev1) {
    static bool started;
    if (started || !chan_attach(&g_ch, section, ev0, ev1, 0)) return false;
    started = true;
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_send);
    InitializeConditionVariable(&g_cv);
    HANDLE t = CreateThread(NULL, 0, reader, NULL, 0, NULL);
    if (!t) return false;
    CloseHandle(t);
    tls_set_loopback(&g_lb);
    fprintf(stderr, "[net] loopback hosts go through the relay\n");
    return true;
}

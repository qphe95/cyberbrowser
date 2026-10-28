/* net_main.c - cb_net.exe: the network process (SANDBOX_DESIGN.md section 2.4).
 *
 * Started by the coordinator with its control channel:
 *   cb_net.exe --chan S E0 E1
 *
 * The coordinator grants it a channel per tab, badged with the tab's site,
 * and one to the storage service.  It makes the tabs' requests (DNS, TCP,
 * TLS, HTTP: http_download.c), each with the cookie jar of the tab's site, so
 * a tab only ever sends and sees its own site's cookies, and HttpOnly ones
 * never reach it.  Jars are loaded from and saved to each site's `cookies`
 * file through the storage service.  CYBER_REPLAY and CYBER_RECORD work here,
 * as in the single-process browser. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "chan.h"
#include "bigmsg.h"
#include "proto.h"
#include "spawn.h"

#include "platform.h"
#include "http_download.h"
#include "loop_client.h"

#define MAX_CLIENTS 64
#define WORKERS 16

/* A site's cookie jar.  Opaque tabs (about:blank) get a jar of their own
 * that is never saved. */
typedef struct Site {
    char label[PROTO_SITE_MAX];
    HttpCookieJar *jar;
    uint64_t saved_version;
    struct Site *next;
} Site;

typedef struct Client {
    uint32_t tab;
    HttpCookieJar *jar;
    Chan chan;                      /* side 1: the network process */
    CRITICAL_SECTION send_lock;     /* workers answer concurrently */
    volatile LONG refs;             /* the table's, the reader's and each job's */
    volatile LONG dead;
    HANDLE reader;
} Client;

typedef struct Job {
    Client *client;
    uint8_t *msg;                   /* a TN_REQUEST, whole */
    uint32_t len;
    struct Job *next;
} Job;

static Chan g_ctl;                  /* side 1 */
static Chan g_store;                /* side 0 (the storage service serves side 1) */
static bool g_have_store;
static BigRx g_store_rx;
static uint32_t g_store_id;
static Site *g_sites;
static Client *g_clients[MAX_CLIENTS];

static CRITICAL_SECTION g_jobs_lock;
static CONDITION_VARIABLE g_jobs_cv;
static Job *g_jobs_head, *g_jobs_tail;

static void client_release(Client *c) {
    if (InterlockedDecrement(&c->refs) == 0) {
        chan_close(&c->chan);
        DeleteCriticalSection(&c->send_lock);
        free(c);
    }
}

/* ---- the storage service (main thread only) ------------------------------- */

static bool store_call(uint16_t type, const char *site, const char *path, const void *data, uint32_t dlen,
                       uint8_t **out, uint32_t *out_len) {
    if (!g_have_store) return false;
    uint8_t head[sizeof(StoreRequest) + PROTO_SITE_MAX + 64];
    StoreRequest r = { ++g_store_id, (uint32_t)strlen(site), (uint32_t)strlen(path), dlen };
    if (r.site_len >= PROTO_SITE_MAX || r.path_len >= 64) return false;
    memcpy(head, &r, sizeof(r));
    memcpy(head + sizeof(r), site, r.site_len);
    memcpy(head + sizeof(r) + r.site_len, path, r.path_len);
    if (!big_send(&g_store, type, head, (uint32_t)(sizeof(r) + r.site_len + r.path_len), data, dlen, NULL, 0, 5000))
        return false;
    for (;;) {
        int got = big_recv(&g_store, &g_store_rx);
        if (got < 0) { g_have_store = false; return false; }
        if (got == 0) { if (!chan_wait(&g_store, 10000)) return false; continue; }
        if (g_store_rx.type != ST_RESULT || g_store_rx.total < sizeof(StoreResult)) continue;
        StoreResult res;
        memcpy(&res, g_store_rx.buf, sizeof(res));
        if (res.id != r.id) continue;
        if (res.data_len != g_store_rx.total - sizeof(res)) return false;
        if (out) {
            *out = g_store_rx.buf + sizeof(res);
            *out_len = res.data_len;
        }
        return res.ok != 0;
    }
}

static HttpCookieJar *site_jar(const char *label) {
    for (Site *s = g_sites; s; s = s->next)
        if (!strcmp(s->label, label)) return s->jar;
    Site *s = (Site *)calloc(1, sizeof(Site));
    snprintf(s->label, sizeof(s->label), "%s", label);
    s->jar = http_cookie_jar_new();
    uint8_t *data;
    uint32_t len;
    if (store_call(TS_READ, label, "cookies", NULL, 0, &data, &len))
        http_cookie_jar_load(s->jar, (const char *)data, len);
    s->saved_version = http_cookie_jar_version(s->jar);
    s->next = g_sites;
    g_sites = s;
    fprintf(stderr, "[net] cookie jar for %s loaded\n", label);
    return s->jar;
}

static void save_jars(void) {
    for (Site *s = g_sites; s; s = s->next) {
        uint64_t v = http_cookie_jar_version(s->jar);
        if (v == s->saved_version) continue;
        size_t len = 0;
        char *text = http_cookie_jar_save(s->jar, &len);
        if (text && store_call(TS_WRITE, s->label, "cookies", text, (uint32_t)len, NULL, NULL))
            s->saved_version = v;
        free(text);
    }
}

/* ---- serving a tab ------------------------------------------------------------- */

static void respond(Client *c, uint32_t id, bool ok, int status, const HttpBuffer *b, const char *err) {
    NetResponse r = { id, ok, status, ok && b->headers ? (uint32_t)b->headers_size : 0,
                      ok ? 0 : (uint32_t)strlen(err), ok && b->data ? (uint32_t)b->size : 0 };
    EnterCriticalSection(&c->send_lock);
    if (!c->dead)
        big_send(&c->chan, NT_RESPONSE, &r, sizeof(r), ok ? b->headers : err, ok ? r.headers_len : r.error_len,
                 ok ? b->data : NULL, r.body_len, 30000);
    LeaveCriticalSection(&c->send_lock);
}

/* A TN_REQUEST, on a worker thread. */
static void run_request(Client *c, const uint8_t *m, uint32_t len) {
    NetRequest q;
    if (len < sizeof(q)) return;
    memcpy(&q, m, sizeof(q));
    uint64_t need = (uint64_t)sizeof(q) + q.method_len + q.url_len + q.headers_len + q.body_len;
    const char *method = (const char *)m + sizeof(q);
    const char *url = method + q.method_len;
    const char *hdrs = url + q.url_len;
    const char *body = hdrs + q.headers_len;
    if (need != len || q.method_len == 0 || q.url_len == 0 || q.nheaders > 64 ||
        method[q.method_len - 1] || url[q.url_len - 1] || (q.headers_len && hdrs[q.headers_len - 1])) {
        respond(c, q.id, false, 0, NULL, "malformed request");
        return;
    }
    const char *headers[64];
    size_t nh = 0;
    for (const char *h = hdrs; h < hdrs + q.headers_len && nh < q.nheaders; h += strlen(h) + 1) headers[nh++] = h;
    http_cookie_jar_use(c->jar);
    HttpBuffer b = { 0 };
    int status = 0;
    char err[512] = "";
    bool ok = http_request_to_memory(url, method, q.body_len ? body : NULL, q.body_len, headers, nh,
                                     &b, &status, err, sizeof(err));
    http_cookie_jar_use(NULL);
    respond(c, q.id, ok, status, &b, err[0] ? err : "request failed");
    http_free_buffer(&b);
}

static DWORD WINAPI worker(void *arg) {
    (void)arg;
    for (;;) {
        EnterCriticalSection(&g_jobs_lock);
        while (!g_jobs_head) SleepConditionVariableCS(&g_jobs_cv, &g_jobs_lock, INFINITE);
        Job *j = g_jobs_head;
        g_jobs_head = j->next;
        if (!g_jobs_head) g_jobs_tail = NULL;
        LeaveCriticalSection(&g_jobs_lock);
        if (!j->client->dead) run_request(j->client, j->msg, j->len);
        client_release(j->client);
        free(j->msg);
        free(j);
    }
    return 0;
}

static void cookie_get(Client *c, const uint8_t *m, uint32_t len) {
    NetCookieGet g;
    if (len <= sizeof(g)) return;
    memcpy(&g, m, sizeof(g));
    const char *url = (const char *)m + sizeof(g);      /* big_recv NUL-terminates */
    http_cookie_jar_use(c->jar);
    const char *s = http_cookie_get_for_document(url);
    HttpBuffer b = { (char *)(s ? s : ""), s ? strlen(s) : 0, NULL, 0 };
    respond(c, g.id, true, 200, &b, NULL);
    http_cookie_jar_use(NULL);
}

static void cookie_set(Client *c, const uint8_t *m, uint32_t len) {
    const char *url = (const char *)m;
    size_t ulen = strnlen(url, len);
    if (ulen >= len) return;
    http_cookie_jar_use(c->jar);
    http_cookie_set_for_document(url, url + ulen + 1);
    http_cookie_jar_use(NULL);
}

/* A tab's channel: requests go to the workers, cookies are answered here. */
static DWORD WINAPI reader(void *arg) {
    Client *c = (Client *)arg;
    BigRx rx = { 0 };
    while (!c->dead) {
        int r = big_recv(&c->chan, &rx);
        if (r < 0) {
            fprintf(stderr, "[net] tab %u: channel broken\n", c->tab);
            break;
        }
        if (r == 0) { chan_wait(&c->chan, 1000); continue; }
        if (rx.type == TN_REQUEST) {
            Job *j = (Job *)malloc(sizeof(Job));
            j->client = c;
            j->len = rx.total;
            j->msg = rx.buf;                /* taken over */
            j->next = NULL;
            rx.buf = NULL;
            rx.cap = 0;
            InterlockedIncrement(&c->refs);
            EnterCriticalSection(&g_jobs_lock);
            if (g_jobs_tail) g_jobs_tail->next = j; else g_jobs_head = j;
            g_jobs_tail = j;
            WakeConditionVariable(&g_jobs_cv);
            LeaveCriticalSection(&g_jobs_lock);
        } else if (rx.type == TN_COOKIE_GET) {
            cookie_get(c, rx.buf, rx.total);
        } else if (rx.type == TN_COOKIE_SET) {
            cookie_set(c, rx.buf, rx.total);
        }
    }
    big_free(&rx);
    client_release(c);
    return 0;
}

/* ---- the coordinator's grants ---------------------------------------------------- */

static void grant(const CsGrant *g) {
    HANDLE s = (HANDLE)(uintptr_t)g->chan_section, e0 = (HANDLE)(uintptr_t)g->chan_ev0,
           e1 = (HANDLE)(uintptr_t)g->chan_ev1;
    if (g->kind == GRANT_STORE) {
        g_have_store = chan_attach(&g_store, s, e0, e1, 0);
        return;
    }
    if (g->kind == GRANT_LOOP) {
        if (!loop_client_start(s, e0, e1)) { CloseHandle(s); CloseHandle(e0); CloseHandle(e1); }
        return;
    }
    if (g->kind != GRANT_TAB || g->site[PROTO_SITE_MAX - 1]) return;
    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS && slot < 0; i++)
        if (!g_clients[i]) slot = i;
    Client *c = (Client *)calloc(1, sizeof(Client));
    if (slot < 0 || !c || !chan_attach(&c->chan, s, e0, e1, 1)) {
        free(c);
        CloseHandle(s); CloseHandle(e0); CloseHandle(e1);
        return;
    }
    c->tab = g->client;
    c->jar = g->site[0] ? site_jar(g->site) : http_cookie_jar_new();
    InitializeCriticalSection(&c->send_lock);
    c->refs = 2;                        /* the table's and the reader's */
    g_clients[slot] = c;
    c->reader = CreateThread(NULL, 0, reader, c, 0, NULL);
    fprintf(stderr, "[net] serving tab %u (%s)\n", c->tab, g->site[0] ? g->site : "opaque");
}

static void revoke(uint32_t tab) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = g_clients[i];
        if (!c || c->tab != tab) continue;
        c->dead = 1;
        SetEvent(chan_event(&c->chan));     /* wake its reader */
        if (c->reader) CloseHandle(c->reader);
        g_clients[i] = NULL;
        client_release(c);
        fprintf(stderr, "[net] stopped serving tab %u\n", tab);
    }
}

int main(int argc, char **argv) {
    if (argc < 5 || strcmp(argv[1], "--chan") ||
        !chan_attach(&g_ctl, spawn_handle_from_arg(argv[2]), spawn_handle_from_arg(argv[3]),
                     spawn_handle_from_arg(argv[4]), 1)) {
        fprintf(stderr, "cb_net: bad arguments\n");
        return 1;
    }
    if (!platform_init() || !platform_http_init()) { fprintf(stderr, "cb_net: no network\n"); return 1; }
    InitializeCriticalSection(&g_jobs_lock);
    InitializeConditionVariable(&g_jobs_cv);
    for (int i = 0; i < WORKERS; i++) CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));

    static ChanMsg m;
    DWORD last_save = GetTickCount();
    for (;;) {
        int r;
        while ((r = chan_recv(&g_ctl, &m)) > 0) {
            if (m.type == CS_GRANT && m.len == sizeof(CsGrant)) {
                CsGrant g;
                memcpy(&g, m.data, sizeof(g));
                grant(&g);
            } else if (m.type == CS_REVOKE && m.len == sizeof(CsRevoke)) {
                CsRevoke v;
                memcpy(&v, m.data, sizeof(v));
                revoke(v.client);
            } else if (m.type == CS_QUIT) {
                r = -1;
                break;
            }
        }
        if (r < 0) break;
        if (GetTickCount() - last_save >= 2000) {
            save_jars();
            last_save = GetTickCount();
        }
        fflush(stderr);
        chan_wait(&g_ctl, 2000);
    }
    save_jars();
    return 0;
}

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
 * as in the single-process browser.
 *
 * What a tab sends is read by net_decode.c, and every decision is
 * net_policy.c's (both proved); this file extracts what they look at (sites,
 * origins, header values) and keeps the books.  Each request says what it is
 * for and which document asks (NetRequest's mode and initiator: the tab's
 * word).  The network process then enforces:
 *   - P10: the page's own document is of the tab's site.  A navigation to
 *     another site, or a redirect to one (not followed), is answered
 *     NET_ELSEWHERE with that URL, and the tab asks the UI to go there, in a
 *     tab of that site.
 *   - Private Network Access: a tab's address space is that of the
 *     connection its document came over (until then, its site's host's);
 *     its other requests connect only to addresses no more private
 *     (tls_set_space_limit, net_space_allows).  Its own documents are exempt.
 *   - ORB: a no-cors response from another origin reaches the tab only if
 *     orb_verdict allows it (images, media, scripts, styles, ...).
 *   - CORS: a cors response from another origin only with an
 *     Access-Control-Allow-Origin naming the initiator (or *; not * and
 *     with Access-Control-Allow-Credentials: true when the jar's cookies
 *     went with it).
 * Frame documents pass both, as they must while frames share their
 * embedder's process (SANDBOX_DESIGN.md section 9).  An initiator counts
 * only if it is of the tab's site or an origin the tab loaded a frame
 * document from; else the request is taken as the page's.  Every reply
 * carries the URL the request ended at, after redirects.
 *
 * One tab can't hold up the others: its requests wait in a bounded queue
 * (refused beyond it), it runs at most CLIENT_RUNNING_MAX of the workers at
 * once, and its replies go out through a thread of its own, so a tab that
 * doesn't read them holds only that thread; past CLIENT_OUT_MAX replies (or
 * CLIENT_OUT_BYTES_MAX bytes) unsent, or a part not taken in
 * SEND_TIMEOUT_MS, it is served no more.
 *
 * CYBER_PNA_PUBLIC_PORTS=P,Q (tests): loopback addresses on those ports
 * count as public. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "chan.h"
#include "bigmsg.h"
#include "proto.h"
#include "spawn.h"

#include "platform.h"
#include "http_download.h"
#include "loop_client.h"
#include "net_decode.h"
#include "net_cache.h"
#include "net_policy.h"
#include "site.h"
#include "tls_space.h"
#include "url_whatwg.h"

_Static_assert(HTTP_MODE_NO_CORS == NET_MODE_NO_CORS && HTTP_MODE_CORS == NET_MODE_CORS &&
               HTTP_MODE_NAVIGATE == NET_MODE_NAVIGATE && HTTP_MODE_FRAME == NET_MODE_FRAME &&
               NET_MODE_FRAME == NET_MODE_MAX, "net_policy.c's modes are http_download.h's");
_Static_assert(sizeof(NetRequest) == NET_REQUEST_FIXED, "net_decode.c reads NetRequest's fields by offset");
_Static_assert(NET_INITIATOR_MAX == NET_ORIGIN_MAX, "an initiator fits a frame origin");

#define MAX_CLIENTS 64
#define WORKERS 32
#define CLIENT_RUNNING_MAX 16               /* workers one tab may hold at once */
#define CLIENT_QUEUED_MAX 256               /* requests one tab may have waiting for a worker */
#define CLIENT_QUEUED_BYTES_MAX (256u << 20)
#define QUEUED_MAX 2048                     /* all tabs' */
#define CLIENT_OUT_MAX 256                  /* replies waiting to go to one tab */
#define CLIENT_OUT_BYTES_MAX (512ull << 20)
#define SEND_TIMEOUT_MS 30000               /* for each part of a reply */

/* A site's cookie jar.  Opaque tabs (about:blank) get a jar of their own
 * that is never saved. */
typedef struct Site {
    char label[PROTO_SITE_MAX];
    HttpCookieJar *jar;
    uint64_t saved_version;
    struct Site *next;
} Site;

/* A reply waiting to be sent. */
typedef struct Out {
    uint16_t type;                  /* NT_RESPONSE, or NT_PART (head: the id; b.data: the bytes) */
    uint64_t size;                  /* what it counts against the queue's bytes */
    uint8_t *head;                  /* NetResponse + url */
    uint32_t head_len;
    char *err;                      /* a failure's */
    HttpBuffer b;                   /* a success's headers and body, taken over */
    struct Out *next;
} Out;

typedef struct Client {
    uint32_t tab;
    char site[PROTO_SITE_MAX];          /* its badge ("" = opaque) */
    volatile LONG space;                /* NET_SPACE_*: where its document came from */
    SRWLOCK frames_lock;
    NetFrames frames;                   /* origins it loaded frame documents from */
    HttpCookieJar *jar;
    Chan chan;                      /* side 1: the network process */
    volatile LONG refs;             /* the table's, the reader's, the sender's and each job's */
    volatile LONG dead;             /* revoked, or served no more */
    HANDLE reader, sender;
    uint32_t queued, running;       /* its jobs (under g_jobs_lock) */
    uint64_t queued_bytes;
    CRITICAL_SECTION out_lock;      /* its replies, for the sender */
    CONDITION_VARIABLE out_cv;
    Out *out_head, *out_tail;
    uint32_t out_n;
    uint64_t out_bytes;
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
static Job *g_jobs_head, **g_jobs_tailp = &g_jobs_head;
static uint32_t g_queued;

static void out_free(Out *o) {
    free(o->head);
    free(o->err);
    http_free_buffer(&o->b);
    free(o);
}

static void client_release(Client *c) {
    if (InterlockedDecrement(&c->refs) == 0) {
        chan_close(&c->chan);
        DeleteCriticalSection(&c->out_lock);
        free(c);
    }
}

/* ---- the storage service (main thread only) ------------------------------- */

/* A storage request in site's folder, from any thread (one at a time, under
 * g_store_lock); what a read returns is a copy for the caller (*out,
 * malloc'd, NUL-terminated). */
static CRITICAL_SECTION g_store_lock;

static bool store_call_locked(uint16_t type, const char *site, const char *path, const void *data, uint32_t dlen,
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
        if (out && res.ok) {
            *out = (uint8_t *)malloc((size_t)res.data_len + 1);
            if (!*out) return false;
            memcpy(*out, g_store_rx.buf + sizeof(res), res.data_len);
            (*out)[res.data_len] = 0;
            *out_len = res.data_len;
        }
        return res.ok != 0;
    }
}

static bool store_call(uint16_t type, const char *site, const char *path, const void *data, uint32_t dlen,
                       uint8_t **out, uint32_t *out_len) {
    EnterCriticalSection(&g_store_lock);
    bool ok = store_call_locked(type, site, path, data, dlen, out, out_len);
    LeaveCriticalSection(&g_store_lock);
    return ok;
}

/* The HTTP cache's files, in a site's folder (net_cache.h). */
static bool cache_read(const char *site, const char *path, uint8_t **data, uint32_t *len) {
    return store_call(TS_READ, site, path, NULL, 0, data, len);
}

static void cache_write(const char *site, const char *path, const void *data, uint32_t len) {
    store_call(TS_WRITE, site, path, data, len, NULL, NULL);
}

static const NetCacheIo g_cache_io = { cache_read, cache_write };

static HttpCookieJar *site_jar(const char *label) {
    for (Site *s = g_sites; s; s = s->next)
        if (!strcmp(s->label, label)) return s->jar;
    Site *s = (Site *)calloc(1, sizeof(Site));
    if (!s) return NULL;
    snprintf(s->label, sizeof(s->label), "%s", label);
    s->jar = http_cookie_jar_new();
    if (!s->jar) {
        free(s);
        return NULL;
    }
    uint8_t *data = NULL;
    uint32_t len;
    if (store_call(TS_READ, label, "cookies", NULL, 0, &data, &len))
        http_cookie_jar_load(s->jar, (const char *)data, len);
    free(data);
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

/* ---- replies ------------------------------------------------------------------- */

/* Stop serving c: its reader and sender end, its jobs are skipped.  (The
 * coordinator's revocation releases the table's reference later.) */
static void client_stop(Client *c, const char *why) {
    if (InterlockedExchange(&c->dead, 1) == 0 && why)
        fprintf(stderr, "[net] tab %u: %s; it is served no more\n", c->tab, why);
    SetEvent(chan_event(&c->chan));     /* wake its reader */
    EnterCriticalSection(&c->out_lock);
    WakeAllConditionVariable(&c->out_cv);
    LeaveCriticalSection(&c->out_lock);
}

static void queue_out(Client *c, Out *o);

/* Queue a reply for c's sender.  ok: NET_OK (b's headers and body are taken
 * over), NET_FAILED (err), NET_ELSEWHERE (url).  url: where the request
 * ended (NULL: none). */
static void reply(Client *c, uint32_t id, uint32_t ok, int status, const char *url, HttpBuffer *b, const char *err) {
    Out *o = (Out *)calloc(1, sizeof(Out));
    size_t ulen = url ? strlen(url) : 0;
    if (o) o->head = (uint8_t *)malloc(sizeof(NetResponse) + ulen);
    if (o && ok != NET_OK) o->err = strdup(err ? err : "request failed");
    if (!o || !o->head || (ok != NET_OK && !o->err)) {
        if (o) out_free(o);
        if (b) http_free_buffer(b);
        client_stop(c, "out of memory for a reply");
        return;
    }
    if (ok == NET_OK && b) {
        o->b = *b;
        memset(b, 0, sizeof(*b));
    }
    NetResponse r = { id, ok, status, ok == NET_OK && o->b.headers ? (uint32_t)o->b.headers_size : 0,
                      ok != NET_OK ? (uint32_t)strlen(o->err) : 0,
                      ok == NET_OK && o->b.data ? (uint32_t)o->b.size : 0, (uint32_t)ulen };
    memcpy(o->head, &r, sizeof(r));
    if (ulen) memcpy(o->head + sizeof(r), url, ulen);
    o->head_len = (uint32_t)(sizeof(r) + ulen);
    o->type = NT_RESPONSE;
    o->size = (uint64_t)o->head_len + r.headers_len + r.error_len + r.body_len;
    queue_out(c, o);
}

/* Queue o for c's sender, within the queue's limits (else c is served no
 * more). */
static void queue_out(Client *c, Out *o) {
    uint64_t size = o->size;
    bool counted = o->type == NT_RESPONSE;      /* (parts count in bytes only: a page comes in many) */
    EnterCriticalSection(&c->out_lock);
    bool full = (counted && c->out_n >= CLIENT_OUT_MAX) || c->out_bytes + size > CLIENT_OUT_BYTES_MAX;
    bool queued = !c->dead && !full;
    if (queued) {
        if (c->out_tail) c->out_tail->next = o; else c->out_head = o;
        c->out_tail = o;
        if (counted) c->out_n++;
        c->out_bytes += size;
        WakeAllConditionVariable(&c->out_cv);
    }
    LeaveCriticalSection(&c->out_lock);
    if (!queued) {
        out_free(o);
        if (full) client_stop(c, "too many replies it hasn't taken");
    }
}

/* A piece of a navigation's body, for c to parse (and paint) as it comes:
 * queued ahead of the request's reply (NT_PART). */
typedef struct PartTo {
    Client *c;
    uint32_t id;
    int verdict;                        /* P10 on the URL the body is from: -1 not yet asked */
} PartTo;

static int doc_verdict(const Client *c, int mode, const char *url);

static void part(void *user, const char *data, size_t len) {
    PartTo *t = (PartTo *)user;
    if (!len || len > CHAN_MSG_MAX * 64 || t->c->dead) return;
    /* Every hop was filtered (same_site_only); the body's own URL is asked
     * again before any of it goes. */
    if (t->verdict < 0) {
        const char *at = http_last_url();
        t->verdict = at[0] ? doc_verdict(t->c, HTTP_MODE_NAVIGATE, at) : NET_DOC_ELSEWHERE;
    }
    if (t->verdict != NET_DOC_OK) return;
    const char *at = http_last_url();
    uint32_t ulen = (uint32_t)strnlen(at, PROTO_URL_MAX);
    Out *o = (Out *)calloc(1, sizeof(Out));
    if (o) o->head = (uint8_t *)malloc(8 + ulen);
    if (o && o->head) o->b.data = (char *)malloc(len);
    if (!o || !o->head || !o->b.data) {
        if (o) out_free(o);
        return;                         /* (the reply still has it all) */
    }
    memcpy(o->head, &t->id, 4);
    memcpy(o->head + 4, &ulen, 4);
    memcpy(o->head + 8, at, ulen);
    o->head_len = 8 + ulen;
    memcpy(o->b.data, data, len);
    o->b.size = len;
    o->type = NT_PART;
    o->size = o->head_len + (uint64_t)len;
    queue_out(t->c, o);
}

/* A tab's replies, in order.  A tab that doesn't take them holds only this
 * thread. */
static DWORD WINAPI sender(void *arg) {
    Client *c = (Client *)arg;
    for (;;) {
        EnterCriticalSection(&c->out_lock);
        while (!c->out_head && !c->dead) SleepConditionVariableCS(&c->out_cv, &c->out_lock, INFINITE);
        Out *o = c->dead ? NULL : c->out_head;
        if (o) {
            c->out_head = o->next;
            if (!c->out_head) c->out_tail = NULL;
            if (o->type == NT_RESPONSE) c->out_n--;
            c->out_bytes -= o->size;
        }
        LeaveCriticalSection(&c->out_lock);
        if (!o) break;
        bool sent;
        if (o->type == NT_PART) {
            sent = big_send(&c->chan, NT_PART, o->head, o->head_len, o->b.data, (uint32_t)o->b.size, NULL, 0,
                            SEND_TIMEOUT_MS);
        } else {
            NetResponse r;
            memcpy(&r, o->head, sizeof(r));
            bool ok = r.ok == NET_OK;
            sent = big_send(&c->chan, NT_RESPONSE, o->head, o->head_len, ok ? o->b.headers : o->err,
                            ok ? r.headers_len : r.error_len, ok ? o->b.data : NULL, r.body_len, SEND_TIMEOUT_MS);
        }
        out_free(o);
        if (!sent) client_stop(c, "it doesn't take its replies");
    }
    /* What is left is never sent. */
    EnterCriticalSection(&c->out_lock);
    for (Out *o = c->out_head, *n; o; o = n) {
        n = o->next;
        out_free(o);
    }
    c->out_head = c->out_tail = NULL;
    c->out_n = 0;
    c->out_bytes = 0;
    LeaveCriticalSection(&c->out_lock);
    client_release(c);
    return 0;
}

/* ---- what a request may reach and read ------------------------------------------ */

static int g_public_ports[8], g_npublic_ports;

/* An address's space (net_policy.c), for tls_client's connects. */
static int space_of(const struct sockaddr *a) {
    int space = NET_SPACE_PUBLIC;
    unsigned port = 0;
    if (a->sa_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)a;
        space = net_space_v4((const uint8_t *)&v4->sin_addr);
        port = ntohs(v4->sin_port);
    } else if (a->sa_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)a;
        space = net_space_v6((const uint8_t *)&v6->sin6_addr);
        port = ntohs(v6->sin6_port);
    }
    for (int i = 0; i < g_npublic_ports && space == NET_SPACE_LOCAL; i++)
        if ((unsigned)g_public_ports[i] == port) space = NET_SPACE_PUBLIC;
    return space;
}

/* The space a site's host names by itself (localhost, an IP): until its
 * document has come, a tab is taken to be there. */
static int site_space(const char *label) {
    const char *host = strchr(label, '+');
    if (!host) return NET_SPACE_PUBLIC;
    host++;
    size_t n = strlen(host);
    if (!strcmp(host, "localhost") || (n > 10 && !strcmp(host + n - 10, ".localhost"))) return NET_SPACE_LOCAL;
    struct in_addr a4;
    if (inet_pton(AF_INET, host, &a4) == 1) return net_space_v4((const uint8_t *)&a4);
    return NET_SPACE_PUBLIC;
}

/* The site of url into out[SITE_MAX]; "" if it has none. */
static void url_site(const char *url, char *out) {
    char scheme[8] = "", host[SITE_HOST_MAX + 1] = "";
    out[0] = 0;
    if (!site_url_host((const uint8_t *)url, (uint32_t)strlen(url), scheme, sizeof(scheme), host, sizeof(host)) ||
        !site_of(scheme, host, out))
        out[0] = 0;
}

/* The origin of url, malloc'd ("null" if it has none; NULL out of memory). */
static char *url_origin(const char *url) {
    WUrl u;
    if (wurl_parse(url, strlen(url), NULL, &u) != 0) return strdup("null");
    char *o = wurl_origin(&u);
    wurl_free(&u);
    return o ? o : strdup("null");
}

/* A header's value in a raw header block, into out (trimmed); false if
 * it isn't there. */
static bool header_value(const char *hdrs, size_t len, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl + 1 < len; ) {
        size_t e = i;
        while (e < len && hdrs[e] != '\n') e++;
        if (e - i > nl && !_strnicmp(hdrs + i, name, nl) && hdrs[i + nl] == ':') {
            size_t v = i + nl + 1;
            while (v < e && (hdrs[v] == ' ' || hdrs[v] == '\t')) v++;
            size_t ve = e;
            while (ve > v && (hdrs[ve - 1] == '\r' || hdrs[ve - 1] == ' ' || hdrs[ve - 1] == '\t')) ve--;
            snprintf(out, cap, "%.*s", (int)(ve - v), hdrs + v);
            return true;
        }
        i = e + 1;
    }
    return false;
}

#define U8(s) ((const uint8_t *)(s))

/* P10 (net_doc_verdict) for a request of mode for url, from c. */
static int doc_verdict(const Client *c, int mode, const char *url) {
    char site[SITE_MAX];
    url_site(url, site);
    return net_doc_verdict(mode, U8(c->site), (uint32_t)strlen(c->site), U8(site), (uint32_t)strlen(site));
}

/* A redirect of a navigation is followed only within the tab's site; the
 * first one out is kept, to send the tab there. */
typedef struct Redirects {
    const Client *c;
    char elsewhere[2048];
} Redirects;

static bool same_site_only(const char *next, void *user) {
    Redirects *r = (Redirects *)user;
    if (doc_verdict(r->c, NET_MODE_NAVIGATE, next) == NET_DOC_OK) return true;
    snprintf(r->elsewhere, sizeof(r->elsewhere), "%s", next);
    return false;
}

static void frame_loaded(Client *c, const char *url) {
    char *o = url_origin(url);
    if (!o) return;
    AcquireSRWLockExclusive(&c->frames_lock);
    net_frames_add(&c->frames, U8(o), (uint32_t)strnlen(o, NET_ORIGIN_MAX + 1));
    ReleaseSRWLockExclusive(&c->frames_lock);
    free(o);
}

/* Whether the tab may read this response (net_may_read); why if not. */
static bool may_read(Client *c, int mode, const char *initiator, const char *final_url, bool credentialed,
                     int status, const HttpBuffer *b, char *why, size_t why_len) {
    char init_site[SITE_MAX] = "", final_site[SITE_MAX], acao_site[SITE_MAX] = "";
    char acao[512] = "", acac[64] = "", mime[512] = "", xcto[64] = "";
    if (initiator[0]) url_site(initiator, init_site);
    url_site(final_url, final_site);
    char *final_origin = url_origin(final_url);
    if (!final_origin) {
        snprintf(why, why_len, "out of memory");
        return false;
    }
    const char *h = b->headers ? b->headers : "";
    size_t hl = b->headers ? b->headers_size : 0;
    bool have_acao = header_value(h, hl, "access-control-allow-origin", acao, sizeof(acao));
    if (have_acao) url_site(acao, acao_site);
    header_value(h, hl, "access-control-allow-credentials", acac, sizeof(acac));
    header_value(h, hl, "content-type", mime, sizeof(mime));
    header_value(h, hl, "x-content-type-options", xcto, sizeof(xcto));
    NetRead r = {
        mode, U8(c->site), (uint32_t)strlen(c->site), U8(initiator), (uint32_t)strlen(initiator),
        U8(init_site), (uint32_t)strlen(init_site), &c->frames,
        U8(final_origin), (uint32_t)strlen(final_origin), U8(final_site), (uint32_t)strlen(final_site),
        credentialed, have_acao, U8(acao), (uint32_t)strlen(acao), U8(acao_site), (uint32_t)strlen(acao_site),
        U8(acac), (uint32_t)strlen(acac), U8(mime), (uint32_t)strlen(mime), !_stricmp(xcto, "nosniff"), status,
        U8(b->data ? b->data : ""), b->data ? (uint32_t)(b->size < 0xffffffffu ? b->size : 0xffffffffu) : 0,
    };
    AcquireSRWLockShared(&c->frames_lock);
    int v = net_may_read(&r);
    ReleaseSRWLockShared(&c->frames_lock);
    free(final_origin);
    switch (v) {
    case NET_READ_OK: return true;
    case NET_READ_NO_ACAO: snprintf(why, why_len, "CORS: %s has no Access-Control-Allow-Origin", final_url); break;
    case NET_READ_ACAO: snprintf(why, why_len, "CORS: %s allows %s only", final_url, acao); break;
    case NET_READ_CREDENTIALS:
        snprintf(why, why_len, "CORS: %s is not for reading with credentials (Access-Control-Allow-Origin %s, "
                 "-Credentials %s)", final_url, acao, acac[0] ? acac : "none");
        break;
    case NET_READ_ORB:
        snprintf(why, why_len, "ORB: %s (%s) is not for another origin to read", final_url, mime[0] ? mime : "no type");
        break;
    default: snprintf(why, why_len, "no such mode"); break;
    }
    return false;
}

/* ---- serving a tab ------------------------------------------------------------- */

/* A TN_REQUEST, on a worker thread. */
static void run_request(Client *c, const uint8_t *m, uint32_t len) {
    NetReq q;
    if (!net_decode_request(m, len, &q)) {
        uint32_t id = 0;
        if (len >= 4) memcpy(&id, m, 4);
        reply(c, id, NET_FAILED, 0, NULL, NULL, "malformed request");
        return;
    }
    /* (Each string is NUL-terminated in m: net_decode_request.) */
    const char *method = (const char *)m + q.method, *url = (const char *)m + q.url;
    const char *initiator = (const char *)m + q.initiator;
    const char *headers[NET_HEADERS_MAX + 2];       /* (+2: the cache's validators) */
    for (uint32_t i = 0; i < q.nheaders; i++) headers[i] = (const char *)m + q.header[i];
    uint32_t nheaders = q.nheaders;
    /* The jar's cookies go with it unless it brings its own Cookie header
     * (empty: none), or it is a preflight (http_download.c). */
    bool credentialed = _stricmp(method, "OPTIONS") != 0;
    for (uint32_t i = 0; i < q.nheaders; i++)
        if (!_strnicmp(headers[i], "Cookie:", 7)) credentialed = false;
    /* P10: the page's document is of the tab's site; else it goes there in
     * a tab of its own. */
    bool doc = q.mode == HTTP_MODE_NAVIGATE;
    if (doc && doc_verdict(c, (int)q.mode, url) != NET_DOC_OK) {
        fprintf(stderr, "[net] tab %u: %s is another site's: elsewhere\n", c->tab, url);
        reply(c, q.id, NET_ELSEWHERE, 0, url, NULL, "another site's document");
        return;
    }
    /* Its document goes anywhere; everything else no more private than
     * where the document came from. */
    Redirects rd = { c, "" };
    if (doc) http_set_redirect_filter(same_site_only, &rd);
    tls_set_space_limit(doc ? -1 : (int)c->space);
    tls_reset_last_space();
    http_cookie_jar_use(c->jar);
    HttpBuffer b = { 0 };
    int status = 0;
    char err[512] = "";
    /* The HTTP cache (net_cache.h), in the tab's site's folder: a fresh entry
     * is the response; a stale one is asked about again (its validators). */
    bool cacheable = c->site[0] && net_cache_request_ok(method, headers, q.nheaders);
    NetCached cached = { 0 };
    char inm[300], ims[100];
    bool ok;
    if (cacheable && net_cache_lookup(c->site, url, &cached) && cached.fresh) {
        b = cached.b;
        memset(&cached.b, 0, sizeof(cached.b));
        status = cached.status;
        ok = true;
    } else {
        if (cached.found && cached.etag[0]) {
            snprintf(inm, sizeof(inm), "If-None-Match: %s", cached.etag);
            headers[nheaders++] = inm;
        }
        if (cached.found && cached.last_modified[0]) {
            snprintf(ims, sizeof(ims), "If-Modified-Since: %s", cached.last_modified);
            headers[nheaders++] = ims;
        }
        /* A document streams to the tab as it comes (NT_PART), to be parsed
         * and painted before it has all arrived. */
        PartTo to = { c, q.id, -1 };
        ok = http_request_to_memory_observed(url, method, q.body_len ? (const char *)m + q.body : NULL, q.body_len,
                                             headers, nheaders, &b, &status, err, sizeof(err),
                                             doc ? part : NULL, &to);
    }
    http_cookie_jar_use(NULL);
    tls_set_space_limit(-1);
    http_set_redirect_filter(NULL, NULL);
    char final_url[2048];
    /* (From the cache: no request was made, and where it ended is url.) */
    /* (http_download reports a 304 as a failure: here it is the answer.) */
    bool from_cache = cacheable && cached.found && (cached.fresh || status == 304);
    snprintf(final_url, sizeof(final_url), "%s", !from_cache && http_last_url()[0] ? http_last_url() : url);
    if (cacheable && cached.found && !cached.fresh && status == 304) {
        /* Not modified: the stored response (kept fresh again if it may be). */
        net_cache_revalidated(c->site, url, &b, &cached);
        http_free_buffer(&b);
        b = cached.b;
        memset(&cached.b, 0, sizeof(cached.b));
        status = cached.status;
        ok = true;
        err[0] = 0;
    } else if (ok && cacheable) {
        if (status == 200 && !cached.fresh && !strcmp(final_url, url)) {
            net_cache_store(c->site, url, status, &b);
        }
    }
    net_cached_free(&cached);
    if (!ok && rd.elsewhere[0]) {
        fprintf(stderr, "[net] tab %u: %s redirects to %s, another site's: elsewhere\n", c->tab, url, rd.elsewhere);
        reply(c, q.id, NET_ELSEWHERE, 0, rd.elsewhere, NULL, "redirected to another site");
        http_free_buffer(&b);
        return;
    }
    /* (The filter saw every hop; P10 is checked on where it ended all the same.) */
    if (ok && doc && doc_verdict(c, (int)q.mode, final_url) != NET_DOC_OK) {
        fprintf(stderr, "[net] tab %u: %s ended at %s, another site's: elsewhere\n", c->tab, url, final_url);
        reply(c, q.id, NET_ELSEWHERE, 0, final_url, NULL, "another site's document");
        http_free_buffer(&b);
        return;
    }
    if (ok && doc && tls_last_space() >= 0) InterlockedExchange(&c->space, tls_last_space());
    if (ok && q.mode == HTTP_MODE_FRAME) frame_loaded(c, final_url);
    char why[2600];
    if (ok && !may_read(c, (int)q.mode, initiator, final_url, credentialed, status, &b, why, sizeof(why))) {
        fprintf(stderr, "[net] tab %u: %s\n", c->tab, why);
        ok = false;
        snprintf(err, sizeof(err), "%.500s", why);
    }
    reply(c, q.id, ok ? NET_OK : NET_FAILED, status, ok ? final_url : NULL, &b, err[0] ? err : "request failed");
    http_free_buffer(&b);
}

/* The first job whose tab isn't at CLIENT_RUNNING_MAX (under g_jobs_lock). */
static Job **next_job(void) {
    Job **pp = &g_jobs_head;
    while (*pp && (*pp)->client->running >= CLIENT_RUNNING_MAX) pp = &(*pp)->next;
    return *pp ? pp : NULL;
}

static DWORD WINAPI worker(void *arg) {
    (void)arg;
    for (;;) {
        EnterCriticalSection(&g_jobs_lock);
        Job **pp;
        while (!(pp = next_job())) SleepConditionVariableCS(&g_jobs_cv, &g_jobs_lock, INFINITE);
        Job *j = *pp;
        *pp = j->next;
        if (g_jobs_tailp == &j->next) g_jobs_tailp = pp;
        Client *c = j->client;
        c->queued--;
        c->queued_bytes -= j->len;
        g_queued--;
        c->running++;
        LeaveCriticalSection(&g_jobs_lock);
        if (!c->dead) run_request(c, j->msg, j->len);
        EnterCriticalSection(&g_jobs_lock);
        c->running--;
        if (c->queued) WakeAllConditionVariable(&g_jobs_cv);   /* its next may go now */
        LeaveCriticalSection(&g_jobs_lock);
        client_release(c);
        free(j->msg);
        free(j);
    }
    return 0;
}

/* A TN_REQUEST, for the workers (rx's buffer taken over), or refused when
 * the queue is full. */
static void enqueue(Client *c, BigRx *rx) {
    Job *j = (Job *)malloc(sizeof(Job));
    EnterCriticalSection(&g_jobs_lock);
    bool room = j && c->queued < CLIENT_QUEUED_MAX && g_queued < QUEUED_MAX &&
                c->queued_bytes + rx->total <= CLIENT_QUEUED_BYTES_MAX;
    if (room) {
        j->client = c;
        j->len = rx->total;
        j->msg = rx->buf;
        j->next = NULL;
        rx->buf = NULL;
        rx->cap = 0;
        InterlockedIncrement(&c->refs);
        *g_jobs_tailp = j;
        g_jobs_tailp = &j->next;
        c->queued++;
        c->queued_bytes += j->len;
        g_queued++;
        WakeConditionVariable(&g_jobs_cv);
    }
    LeaveCriticalSection(&g_jobs_lock);
    if (!room) {
        free(j);
        uint32_t id = 0;
        if (rx->total >= 4) memcpy(&id, rx->buf, 4);
        reply(c, id, NET_FAILED, 0, NULL, NULL, "too many requests");
    }
}

static void cookie_get(Client *c, const uint8_t *m, uint32_t len) {
    uint32_t id, ulen;
    if (!net_decode_cookie_get(m, len, &id, &ulen)) return;
    char *url = (char *)malloc((size_t)ulen + 1);
    if (!url) return;
    memcpy(url, m + 4, ulen);
    url[ulen] = 0;
    http_cookie_jar_use(c->jar);
    const char *s = http_cookie_get_for_document(url);
    HttpBuffer b = { strdup(s ? s : ""), s ? strlen(s) : 0, NULL, 0 };
    http_cookie_jar_use(NULL);
    free(url);
    if (b.data) reply(c, id, NET_OK, 200, NULL, &b, NULL);
    else reply(c, id, NET_FAILED, 0, NULL, NULL, "out of memory");
}

static void cookie_set(Client *c, const uint8_t *m, uint32_t len) {
    uint32_t ulen, llen;
    if (!net_decode_cookie_set(m, len, &ulen, &llen)) return;
    char *line = (char *)malloc((size_t)llen + 1);
    if (!line) return;
    memcpy(line, m + ulen + 1, llen);
    line[llen] = 0;
    http_cookie_jar_use(c->jar);
    http_cookie_set_for_document((const char *)m, line);       /* (the URL ends with its NUL) */
    http_cookie_jar_use(NULL);
    free(line);
}

/* A tab's channel: requests go to the workers, cookies are answered here. */
static DWORD WINAPI reader(void *arg) {
    Client *c = (Client *)arg;
    BigRx rx = { 0 };
    rx.limit = BIG_MAX / 2 + (4u << 20);   /* a tab sends bodies up to BIG_MAX / 2 (tab_main.c), plus 4 MB of URL and headers */
    while (!c->dead) {
        int r = big_recv(&c->chan, &rx);
        if (r < 0) {
            client_stop(c, "channel broken");
            break;
        }
        if (r == 0) { chan_wait(&c->chan, 1000); continue; }
        if (rx.type == TN_REQUEST) enqueue(c, &rx);
        else if (rx.type == TN_COOKIE_GET) cookie_get(c, rx.buf, rx.total);
        else if (rx.type == TN_COOKIE_SET) cookie_set(c, rx.buf, rx.total);
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
        /* The HTTP cache keeps its entries there (CYBER_HTTP_CACHE=0: none). */
        const char *hc = getenv("CYBER_HTTP_CACHE");
        if (g_have_store && !(hc && hc[0] == '0')) net_cache_init(&g_cache_io);
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
    snprintf(c->site, sizeof(c->site), "%s", g->site);
    c->space = g->site[0] ? site_space(g->site) : NET_SPACE_PUBLIC;
    c->jar = g->site[0] ? site_jar(g->site) : http_cookie_jar_new();
    if (!c->jar) {                      /* out of memory: not served */
        chan_close(&c->chan);
        free(c);
        return;
    }
    InitializeSRWLock(&c->frames_lock);
    InitializeCriticalSection(&c->out_lock);
    InitializeConditionVariable(&c->out_cv);
    c->refs = 3;                        /* the table's, the reader's and the sender's */
    g_clients[slot] = c;
    c->sender = CreateThread(NULL, 0, sender, c, 0, NULL);
    if (!c->sender) {
        c->dead = 1;
        client_release(c);              /* (the sender's) */
    }
    c->reader = CreateThread(NULL, 0, reader, c, 0, NULL);
    if (!c->reader) {
        client_stop(c, "no thread to read it");
        client_release(c);              /* (the reader's) */
    }
    fprintf(stderr, "[net] serving tab %u (%s)\n", c->tab, g->site[0] ? g->site : "opaque");
}

static void revoke(uint32_t tab) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = g_clients[i];
        if (!c || c->tab != tab) continue;
        client_stop(c, NULL);           /* wakes its reader and sender */
        if (c->reader) CloseHandle(c->reader);
        if (c->sender) CloseHandle(c->sender);
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
    const char *pp = getenv("CYBER_PNA_PUBLIC_PORTS");
    for (const char *p = pp; p && *p && g_npublic_ports < 8; ) {
        g_public_ports[g_npublic_ports++] = atoi(p);
        p = strchr(p, ',');
        if (p) p++;
    }
    if (g_npublic_ports) fprintf(stderr, "[net] CYBER_PNA_PUBLIC_PORTS=%s\n", pp);
    tls_set_space_policy(space_of);
    InitializeCriticalSection(&g_jobs_lock);
    InitializeCriticalSection(&g_store_lock);
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

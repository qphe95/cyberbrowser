/* Script fetch pool.  See dom_fetch.h. */
#include "dom_fetch.h"
#include "cyber_env.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

typedef struct FetchEntry {
    char *url;
    HttpBuffer buf;
    char err[256];
    bool ok;
    int state;                  /* 0 queued, 1 running, 2 done, 3 claimed */
    struct FetchEntry *next;    /* all entries */
    struct FetchEntry *qnext;   /* work queue */
} FetchEntry;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_done = PTHREAD_COND_INITIALIZER;
static FetchEntry *g_all;
static FetchEntry *g_qhead, *g_qtail;
static int g_threads = -1;      /* -1: pool not started */
static DomFetchFn g_fetcher = http_get_to_memory;
static DomFetchStats g_stats;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

void dom_fetch_set_fetcher(DomFetchFn fn) {
    pthread_mutex_lock(&g_mu);
    g_fetcher = fn ? fn : http_get_to_memory;
    pthread_mutex_unlock(&g_mu);
}

static void *fetch_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        while (!g_qhead) pthread_cond_wait(&g_work, &g_mu);
        FetchEntry *e = g_qhead;
        g_qhead = e->qnext;
        if (!g_qhead) g_qtail = NULL;
        e->state = 1;
        DomFetchFn fn = g_fetcher;
        pthread_mutex_unlock(&g_mu);
        HttpBuffer b = { 0 };
        char err[256] = { 0 };
        bool ok = fn(e->url, &b, err, sizeof(err));
        pthread_mutex_lock(&g_mu);
        e->buf = b;
        e->ok = ok;
        memcpy(e->err, err, sizeof(err));
        e->state = 2;
        g_stats.completed++;
        pthread_cond_broadcast(&g_done);
    }
    return NULL;
}

/* Start the pool (called with g_mu held). */
static void pool_start(void) {
    if (g_threads >= 0) return;
    int n = 6;
    const char *v = cyber_getenv("CYBER_FETCH_THREADS");
    if (v) n = atoi(v);
    if (n < 0) n = 0;
    if (n > 32) n = 32;
    g_threads = 0;
    for (int i = 0; i < n; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, fetch_worker, NULL) != 0) break;
        pthread_detach(t);
        g_threads++;
    }
}

static FetchEntry *find_entry(const char *url) {
    for (FetchEntry *e = g_all; e; e = e->next)
        if (e->state != 3 && strcmp(e->url, url) == 0) return e;
    return NULL;
}

void dom_fetch_prefetch(const char *url) {
    if (!url || !url[0]) return;
    pthread_mutex_lock(&g_mu);
    pool_start();
    if (g_threads > 0 && !find_entry(url)) {
        FetchEntry *e = calloc(1, sizeof(FetchEntry));
        e->url = strdup(url);
        e->next = g_all;
        g_all = e;
        if (g_qtail) g_qtail->qnext = e; else g_qhead = e;
        g_qtail = e;
        g_stats.queued++;
        pthread_cond_signal(&g_work);
    }
    pthread_mutex_unlock(&g_mu);
}

bool dom_fetch_script(const char *url, HttpBuffer *out, char *err, size_t err_len) {
    memset(out, 0, sizeof(*out));
    if (!url) return false;
    pthread_mutex_lock(&g_mu);
    FetchEntry *e = find_entry(url);
    if (!e) {
        DomFetchFn fn = g_fetcher;
        g_stats.sync_fetches++;
        pthread_mutex_unlock(&g_mu);
        return fn(url, out, err, err_len);
    }
    if (e->state < 2) {
        double t0 = now_ms();
        g_stats.waits++;
        while (e->state < 2) pthread_cond_wait(&g_done, &g_mu);
        g_stats.wait_ms += now_ms() - t0;
    } else {
        g_stats.hits++;
    }
    *out = e->buf;
    memset(&e->buf, 0, sizeof(e->buf));
    if (err && err_len) snprintf(err, err_len, "%s", e->err);
    bool ok = e->ok;
    e->state = 3;
    pthread_mutex_unlock(&g_mu);
    return ok;
}

void dom_fetch_stats(DomFetchStats *out) {
    pthread_mutex_lock(&g_mu);
    *out = g_stats;
    pthread_mutex_unlock(&g_mu);
}

void dom_fetch_reset(void) {
    pthread_mutex_lock(&g_mu);
    for (FetchEntry *e = g_all; e; e = e->next)
        while (e->state < 2) pthread_cond_wait(&g_done, &g_mu);
    FetchEntry *e = g_all;
    while (e) {
        FetchEntry *n = e->next;
        if (e->state != 3) http_free_buffer(&e->buf);
        free(e->url);
        free(e);
        e = n;
    }
    g_all = NULL;
    g_qhead = g_qtail = NULL;
    memset(&g_stats, 0, sizeof(g_stats));
    pthread_mutex_unlock(&g_mu);
}

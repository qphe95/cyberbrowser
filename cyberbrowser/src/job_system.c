/*
 * Job system: Chase–Lev deques, dependency counters, helping waits.
 * See job_system.h.
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "job_system.h"
#include "gc_platform.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define JOB_MAX_THREADS 64
#define DEQUE_CAP 4096u            /* power of two */

/* ---------------------------------------------------------------------------
 * Chase–Lev deque (Lê, Pop, Cohen, Zappa Nardelli 2013), fixed capacity.
 * ------------------------------------------------------------------------- */

typedef struct Deque {
    _Alignas(64) _Atomic int64_t top;
    _Alignas(64) _Atomic int64_t bottom;
    _Atomic(Job *) buf[DEQUE_CAP];
} Deque;

static bool deque_push(Deque *d, Job *j)
{
    int64_t b = atomic_load_explicit(&d->bottom, memory_order_relaxed);
    int64_t t = atomic_load_explicit(&d->top, memory_order_acquire);
    if (b - t >= (int64_t)DEQUE_CAP) return false;
    atomic_store_explicit(&d->buf[b & (DEQUE_CAP - 1)], j, memory_order_relaxed);
    /* Release on bottom publishes the job (and everything written before
     * it) to a thief that acquires bottom. */
    atomic_store_explicit(&d->bottom, b + 1, memory_order_release);
    return true;
}

static Job *deque_pop(Deque *d)
{
    int64_t b = atomic_load_explicit(&d->bottom, memory_order_relaxed) - 1;
    atomic_store_explicit(&d->bottom, b, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    int64_t t = atomic_load_explicit(&d->top, memory_order_relaxed);
    Job *x = NULL;
    if (t <= b) {
        x = atomic_load_explicit(&d->buf[b & (DEQUE_CAP - 1)], memory_order_relaxed);
        if (t == b) {
            /* Last element: race against thieves. */
            if (!atomic_compare_exchange_strong_explicit(&d->top, &t, t + 1,
                                                         memory_order_seq_cst,
                                                         memory_order_relaxed))
                x = NULL;
            atomic_store_explicit(&d->bottom, b + 1, memory_order_relaxed);
        }
    } else {
        atomic_store_explicit(&d->bottom, b + 1, memory_order_relaxed);
    }
    return x;
}

/* Returns a job, NULL when empty, or (Job *)1 when a race was lost. */
#define STEAL_RETRY ((Job *)1)
static Job *deque_steal(Deque *d)
{
    int64_t t = atomic_load_explicit(&d->top, memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    int64_t b = atomic_load_explicit(&d->bottom, memory_order_acquire);
    if (t >= b) return NULL;
    Job *x = atomic_load_explicit(&d->buf[t & (DEQUE_CAP - 1)], memory_order_acquire);
    if (!atomic_compare_exchange_strong_explicit(&d->top, &t, t + 1,
                                                 memory_order_seq_cst,
                                                 memory_order_relaxed))
        return STEAL_RETRY;
    return x;
}

/* ---------------------------------------------------------------------------
 * Threads
 * ------------------------------------------------------------------------- */

typedef struct JThread {
    Deque dq;
    FrameArena arena;
    uint64_t jobs, steals;
    uint32_t rng;
    int started;
    GCThread os;
} JThread;

static JThread *g_thr;                 /* [0] = graph owner, [1..n-1] workers */
static int g_nthreads;
static _Atomic uint32_t g_init;        /* 0 none, 2 ready */
static _Atomic uint32_t g_wake_seq;
static _Atomic uint32_t g_sleepers;
static _Atomic uint32_t g_shutdown;
static _Atomic uint32_t g_owner_busy;
static bool g_random_steal;
static _Thread_local int tl_index = -1;
static _Thread_local FrameArena tl_inline_arena;   /* threads outside the pool */

static uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

static int cpu_count(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : (int)n;
#endif
}

static void pool_init(void);
int job_thread_count(void) { pool_init(); return g_nthreads ? g_nthreads : 1; }
int job_thread_index(void) { return tl_index; }

FrameArena *job_thread_arena(void)
{
    if (tl_index >= 0 && g_thr) return &g_thr[tl_index].arena;
    return &tl_inline_arena;
}

static void run_job(JThread *self, Job *j);

static Job *find_work(int idx)
{
    JThread *self = &g_thr[idx];
    Job *j = deque_pop(&self->dq);
    if (j) return j;
    int n = g_nthreads;
    if (n <= 1) return NULL;
    int start = (int)(xorshift(&self->rng) % (uint32_t)n);
    for (int k = 0; k < n; k++) {
        int v = (start + k) % n;
        if (v == idx) continue;
        Job *s;
        int tries = 0;
        do { s = deque_steal(&g_thr[v].dq); } while (s == STEAL_RETRY && ++tries < 8);
        if (s && s != STEAL_RETRY) { self->steals++; return s; }
    }
    return NULL;
}

static void wake_workers(void)
{
    if (atomic_load_explicit(&g_sleepers, memory_order_seq_cst) == 0) return;
    atomic_fetch_add_explicit(&g_wake_seq, 1, memory_order_seq_cst);
    gc_os_wake_all((volatile uint32_t *)&g_wake_seq);
}

static void worker_main(void *arg)
{
    int idx = (int)(intptr_t)arg;
    tl_index = idx;
    JThread *self = &g_thr[idx];
    int idle = 0;
    while (!atomic_load_explicit(&g_shutdown, memory_order_acquire)) {
        Job *j = find_work(idx);
        if (j) { run_job(self, j); idle = 0; continue; }
        if (++idle < 128) {
            if ((idle & 15) == 0) gc_os_yield();
            continue;
        }
        uint32_t seq = atomic_load_explicit(&g_wake_seq, memory_order_seq_cst);
        atomic_fetch_add_explicit(&g_sleepers, 1, memory_order_seq_cst);
        j = find_work(idx);   /* re-check after announcing: no lost wake-ups */
        if (j) {
            atomic_fetch_sub_explicit(&g_sleepers, 1, memory_order_seq_cst);
            run_job(self, j);
            idle = 0;
            continue;
        }
        /* Woken by job_spawn; the timeout is only a safety net, kept long
         * so idle workers cost nothing while the browser runs scripts. */
        gc_os_wait((volatile uint32_t *)&g_wake_seq, seq, 1000);
        atomic_fetch_sub_explicit(&g_sleepers, 1, memory_order_seq_cst);
        idle = 0;
    }
}

static void pool_shutdown(void)
{
    atomic_store(&g_shutdown, 1);
    atomic_fetch_add(&g_wake_seq, 1);
    gc_os_wake_all((volatile uint32_t *)&g_wake_seq);
    for (int i = 1; i < g_nthreads; i++) if (g_thr[i].started) gc_os_thread_join(&g_thr[i].os);
}

/* Started by the first graph owner (the engine thread), so it never races. */
static void pool_init(void)
{
    if (atomic_load_explicit(&g_init, memory_order_acquire) == 2) return;
    int n = cpu_count();
    const char *env = getenv("CSS_THREADS");
    if (env && env[0]) n = atoi(env);
    if (n < 1) n = 1;
    if (n > JOB_MAX_THREADS) n = JOB_MAX_THREADS;
    const char *st = getenv("CSS_STEAL");
    g_random_steal = st && strcmp(st, "random") == 0;
    g_thr = calloc((size_t)n, sizeof(JThread));
    for (int i = 0; i < n; i++) g_thr[i].rng = 0x12345u * (uint32_t)(i + 1);
    /* The count is fixed before any worker starts (workers read it).  A
     * worker that fails to start leaves an empty deque nobody pushes to. */
    g_nthreads = n;
    int started = 1;
    for (int i = 1; i < n; i++) {
        if (gc_os_thread_start(&g_thr[i].os, worker_main, (void *)(intptr_t)i)) {
            g_thr[i].started = 1;
            started++;
        }
    }
    if (started == 1) g_nthreads = 1;
    if (started > 1) atexit(pool_shutdown);
    atomic_store_explicit(&g_init, 2, memory_order_release);
}

/* ---------------------------------------------------------------------------
 * Jobs
 * ------------------------------------------------------------------------- */

Job *job_new(JobFn fn, Job *parent, void *ctx, uint32_t a, uint32_t b)
{
    Job *j = frame_arena_alloc(job_thread_arena(), sizeof(Job));
    j->fn = fn;
    j->cont = NULL;
    j->parent = parent;
    atomic_store_explicit(&j->pending, 1, memory_order_relaxed);
    atomic_store_explicit(&j->done, 0, memory_order_relaxed);
    j->ctx = ctx;
    j->a = a;
    j->b = b;
    return j;
}

/* Count down `j`; the thread that reaches zero runs the continuation and
 * then carries on to the parent. */
static void job_finish(Job *j)
{
    for (;;) {
        if (atomic_fetch_sub_explicit(&j->pending, 1, memory_order_acq_rel) != 1) return;
        if (j->cont) {
            JobFn c = j->cont;
            j->cont = NULL;
            atomic_store_explicit(&j->pending, 1, memory_order_relaxed);
            c(j);
            continue;   /* drop the self reference taken for the continuation */
        }
        Job *p = j->parent;
        if (!p) {
            atomic_store_explicit(&j->done, 1, memory_order_release);
            gc_os_wake_all((volatile uint32_t *)&j->done);
            return;
        }
        j = p;
    }
}

static void run_job(JThread *self, Job *j)
{
    if (self) {
        self->jobs++;
        if (g_random_steal && (xorshift(&self->rng) & 7) == 0) gc_os_yield();
    }
    j->fn(j);
    job_finish(j);
}

void job_spawn(Job *j)
{
    if (j->parent) atomic_fetch_add_explicit(&j->parent->pending, 1, memory_order_relaxed);
    int idx = tl_index;
    if (idx < 0 || !g_thr || g_nthreads <= 1 || !deque_push(&g_thr[idx].dq, j)) {
        /* No pool, or the deque is full: run it now (depth-first). */
        run_job(idx >= 0 && g_thr ? &g_thr[idx] : NULL, j);
        return;
    }
    atomic_thread_fence(memory_order_seq_cst);
    wake_workers();
}

static uint64_t g_last_jobs, g_last_steals;

/* Run jobs until `root` is done. */
static void help_until_done(int idx, Job *root)
{
    JThread *self = &g_thr[idx];
    int idle = 0;
    while (!atomic_load_explicit(&root->done, memory_order_acquire)) {
        Job *j = find_work(idx);
        if (j) { run_job(self, j); idle = 0; continue; }
        if (++idle < 64) { if ((idle & 7) == 0) gc_os_yield(); continue; }
        gc_os_wait((volatile uint32_t *)&root->done, 0, 1);
    }
}

void job_last_stats(uint64_t *jobs, uint64_t *steals)
{
    if (jobs) *jobs = g_last_jobs;
    if (steals) *steals = g_last_steals;
}

Job *job_group_new(void)
{
    return job_new(NULL, NULL, NULL, 0, 0);
}

void job_group_spawn(Job *group, JobFn fn, void *ctx, uint32_t a, uint32_t b)
{
    job_spawn(job_new(fn, group, ctx, a, b));
}

void job_group_wait(Job *group)
{
    job_finish(group);                  /* drop the group's self reference */
    if (tl_index >= 0 && g_thr) help_until_done(tl_index, group);
    /* Outside the pool children ran inline, so the group is already done. */
}

void job_graph_run(Job *root)
{
    pool_init();
    if (tl_index >= 0) {
        /* Nested graph from inside a job: help until it is done. */
        run_job(&g_thr[tl_index], root);
        help_until_done(tl_index, root);
        return;
    }
    uint32_t expected = 0;
    if (!atomic_compare_exchange_strong(&g_owner_busy, &expected, 1)) {
        /* Another thread owns the pool: run this graph inline, depth-first
         * (spawn executes children immediately on threads outside the pool). */
        run_job(NULL, root);
        frame_arena_reset(&tl_inline_arena);
        return;
    }
    tl_index = 0;
    for (int i = 0; i < g_nthreads; i++) g_thr[i].jobs = g_thr[i].steals = 0;
    run_job(&g_thr[0], root);
    help_until_done(0, root);
    uint64_t jobs = 0, steals = 0;
    for (int i = 0; i < g_nthreads; i++) { jobs += g_thr[i].jobs; steals += g_thr[i].steals; }
    g_last_jobs = jobs;
    g_last_steals = steals;
    /* Every job of the graph has finished: the arenas can be recycled. */
    for (int i = 0; i < g_nthreads; i++) frame_arena_reset(&g_thr[i].arena);
    tl_index = -1;
    atomic_store(&g_owner_busy, 0);
}

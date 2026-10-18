/*
 * General-purpose worker pool (used by parallel CSS layout).  Lock-free:
 * jobs go through a bounded multi-producer multi-consumer queue (Vyukov's
 * sequence-numbered ring); idle workers sleep on an address.  The API keeps
 * its historical gc_thread_pool_* names.
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "quickjs_gc_unified.h"
#include "gc_platform.h"
#include "cyber_profile.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define POOL_CAPACITY 4096u   /* power of two */
#define POOL_MAX_THREADS 64

typedef struct PoolCell {
    _Atomic uint64_t seq;
    GCThreadPoolJobFunc func;
    void *arg;
} PoolCell;

static PoolCell g_cells[POOL_CAPACITY];
static _Atomic uint64_t g_enqueue_pos;
static _Atomic uint64_t g_dequeue_pos;
static _Atomic uint32_t g_pending;        /* submitted and not yet finished */
static _Atomic uint32_t g_wake_seq;       /* bumped on every submit */
static _Atomic uint32_t g_shutdown;
static _Atomic uint32_t g_init_state;     /* 0 = no, 1 = in progress, 2 = ready */
static GCThread g_threads[POOL_MAX_THREADS];
static uint32_t g_thread_count;

static uint32_t processor_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : (uint32_t)n;
#endif
}

static bool pool_push(GCThreadPoolJobFunc func, void *arg) {
    uint64_t pos = atomic_load_explicit(&g_enqueue_pos, memory_order_relaxed);
    for (;;) {
        PoolCell *c = &g_cells[pos & (POOL_CAPACITY - 1)];
        uint64_t seq = atomic_load_explicit(&c->seq, memory_order_acquire);
        int64_t dif = (int64_t)seq - (int64_t)pos;
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&g_enqueue_pos, &pos, pos + 1,
                                                      memory_order_relaxed, memory_order_relaxed)) {
                c->func = func;
                c->arg = arg;
                atomic_store_explicit(&c->seq, pos + 1, memory_order_release);
                return true;
            }
        } else if (dif < 0) {
            return false;  /* full */
        } else {
            pos = atomic_load_explicit(&g_enqueue_pos, memory_order_relaxed);
        }
    }
}

static bool pool_pop(GCThreadPoolJobFunc *func, void **arg) {
    uint64_t pos = atomic_load_explicit(&g_dequeue_pos, memory_order_relaxed);
    for (;;) {
        PoolCell *c = &g_cells[pos & (POOL_CAPACITY - 1)];
        uint64_t seq = atomic_load_explicit(&c->seq, memory_order_acquire);
        int64_t dif = (int64_t)seq - (int64_t)(pos + 1);
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&g_dequeue_pos, &pos, pos + 1,
                                                      memory_order_relaxed, memory_order_relaxed)) {
                *func = c->func;
                *arg = c->arg;
                atomic_store_explicit(&c->seq, pos + POOL_CAPACITY, memory_order_release);
                return true;
            }
        } else if (dif < 0) {
            return false;  /* empty */
        } else {
            pos = atomic_load_explicit(&g_dequeue_pos, memory_order_relaxed);
        }
    }
}

static void run_job(GCThreadPoolJobFunc func, void *arg) {
    CP_BEGIN_CAT("pool-job", "pool");
    func(arg);
    CP_END("pool-job");
    if (atomic_fetch_sub_explicit(&g_pending, 1, memory_order_acq_rel) == 1)
        gc_os_wake_all((volatile uint32_t *)&g_pending);
}

static void pool_worker(void *unused) {
    (void)unused;
    while (!atomic_load_explicit(&g_shutdown, memory_order_acquire)) {
        GCThreadPoolJobFunc func;
        void *arg;
        uint32_t seen = atomic_load_explicit(&g_wake_seq, memory_order_acquire);
        if (pool_pop(&func, &arg)) {
            run_job(func, arg);
            continue;
        }
        gc_os_wait((volatile uint32_t *)&g_wake_seq, seen, 100);
    }
}

static void pool_shutdown(void) {
    atomic_store(&g_shutdown, 1);
    atomic_fetch_add(&g_wake_seq, 1);
    gc_os_wake_all((volatile uint32_t *)&g_wake_seq);
    for (uint32_t i = 0; i < g_thread_count; i++)
        gc_os_thread_join(&g_threads[i]);
    g_thread_count = 0;
}

static bool pool_init(void) {
    /* The pool is started on first use by the thread that owns the engine
     * (gc_init starts it too), so initialization never races. */
    if (atomic_load_explicit(&g_init_state, memory_order_acquire) == 2) return true;
    for (uint32_t i = 0; i < POOL_CAPACITY; i++)
        atomic_store_explicit(&g_cells[i].seq, i, memory_order_relaxed);
    uint32_t n = processor_count();
    if (n < 2) n = 2;
    if (n > POOL_MAX_THREADS) n = POOL_MAX_THREADS;
    for (uint32_t i = 0; i < n; i++) {
        if (!gc_os_thread_start(&g_threads[i], pool_worker, NULL)) break;
        g_thread_count++;
    }
    atexit(pool_shutdown);
    atomic_store_explicit(&g_init_state, 2, memory_order_release);
    return g_thread_count > 0;
}

uint32_t gc_thread_pool_get_thread_count(void) {
    pool_init();
    return g_thread_count;
}

bool gc_thread_pool_submit_job(GCThreadPoolJobFunc func, void *arg) {
    if (!func || !pool_init()) return false;
    atomic_fetch_add_explicit(&g_pending, 1, memory_order_acq_rel);
    if (!pool_push(func, arg)) {
        atomic_fetch_sub_explicit(&g_pending, 1, memory_order_acq_rel);
        return false;
    }
    atomic_fetch_add_explicit(&g_wake_seq, 1, memory_order_release);
    gc_os_wake_all((volatile uint32_t *)&g_wake_seq);
    return true;
}

bool gc_thread_pool_submit_test_job(GCThreadPoolJobFunc func, void *arg) {
    return gc_thread_pool_submit_job(func, arg);
}

/* The caller runs queued jobs itself while it waits, so waiting never
 * stalls the pool. */
void gc_thread_pool_wait_empty(void) {
    for (;;) {
        uint32_t pending = atomic_load_explicit(&g_pending, memory_order_acquire);
        if (pending == 0) return;
        GCThreadPoolJobFunc func;
        void *arg;
        if (pool_pop(&func, &arg)) {
            run_job(func, arg);
            continue;
        }
        gc_os_wait((volatile uint32_t *)&g_pending, pending, 1);
    }
}

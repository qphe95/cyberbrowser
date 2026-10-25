/*
 * Concurrent collection: the GC thread, boundary steps, dirty tracking,
 * concurrent sweeping and evacuation, the nursery, the freed-handle ring
 * and verification.  Design: CONCURRENT_GC.md.
 *
 * Threads
 *   The JS thread runs the engine.  One GC thread runs collection cycles.
 *   They meet only at boundary steps: the GC thread posts a request; the JS
 *   thread runs it at its next poll (gc_poll at function entry and every
 *   interpreter loop iteration) or tick (gc_tick between event-loop tasks).
 *   A boundary step has a time budget; a step that does not fit declines
 *   and the GC thread does more work before asking again.  Nothing waits on
 *   a lock.  The GC thread sleeps on an address while it waits for an
 *   answer; the JS thread never waits for the GC thread, except in gc_run(),
 *   the explicit stop-the-world collection.
 *
 * Major cycle (whole heap)
 *   mark start (boundary)   flip mark parity, record TAMS, snapshot roots
 *   concurrent mark         trace; rescan dirty cards in rounds
 *   remark (boundary)       roots again, last dirty cards, finish tracing,
 *                           weak step.  Declines if over budget.
 *   concurrent sweep        free handles of unmarked objects (ring to JS)
 *   concurrent evacuation   copy live objects out of sparse regions into
 *                           the back table (CAS)
 *   flip (boundary)         reject copies the JS thread changed or pinned,
 *                           swap the handle tables
 *   release                 bring the new back table up to date, free the
 *                           emptied regions
 *
 * Minor cycle (nursery)
 *   The same steps, restricted to the nursery regions that existed at
 *   minor start.  Roots include the remembered set: the cards written since
 *   the previous minor start.  Every surviving object is promoted (copied to
 *   old space), or promoted in place when its copy is rejected or its region
 *   is mostly live.
 *
 * Dirty tracking
 *   The engine's write helpers set one byte per 4 KB card.  (Built with
 *   GC_SOFTWARE_DIRTY=0 on Windows, the heap is reserved with
 *   MEM_WRITE_WATCH and GetWriteWatch reports written pages instead.)
 *   Either is pulled through dirty_pull(), which also records every card in
 *   the nursery's remembered set.  Frames are written without the helpers
 *   while they are on the call chain (a root); a frame that leaves it and
 *   stays reachable has its slots flagged then.  QJS_GC_BARRIER_CHECK=1
 *   (Windows) compares the cards with the OS write watch to find stores
 *   that bypass the helpers.
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
#include <stdatomic.h>

#include "quickjs.h"
#include "quickjs_gc_unified.h"
#include "quickjs-internal.h"
#include "gc_platform.h"
#include "gc_core.h"
#include "gc_collector.h"

/* ============================================================================
 * Small vectors
 * ============================================================================ */

typedef struct U32Vec {
    uint32_t *items;
    uint32_t n, cap;
} U32Vec;

static void u32_push(U32Vec *v, uint32_t x) {
    if (v->n == v->cap) {
        uint32_t cap = v->cap ? v->cap * 2 : 1024;
        uint32_t *n = (uint32_t *)realloc(v->items, cap * sizeof(uint32_t));
        if (!n) {
            fprintf(stderr, "[GC] out of memory\n");
            abort();
        }
        v->items = n;
        v->cap = cap;
    }
    v->items[v->n++] = x;
}

static void u32_free(U32Vec *v) {
    free(v->items);
    memset(v, 0, sizeof(*v));
}

/* ============================================================================
 * JS-thread side list: the reference log
 * ============================================================================ */

/* gc_note_ref: references stored in off-heap memory (no dirty flag). */
static U32Vec g_ref_log;
static uint32_t g_ref_log_major_start;

void gc_note_ref(GCHandle h) {
    if (h != GC_HANDLE_NULL && g_core.config.background)
        u32_push(&g_ref_log, h);
}

void gc_col_mark_ref_log(JSRuntimeHandle rt, uint32_t from) {
    for (uint32_t i = from; i < g_ref_log.n; i++)
        gc_col_mark_cb(rt, g_ref_log.items[i]);
}

/* ============================================================================
 * Collector state shared by the two threads
 * ============================================================================ */

enum {
    REQ_NONE = 0,
    REQ_MARK_START,
    REQ_REMARK,
    REQ_WEAK_APPLY,
};

enum {
    ACK_DONE = 1,
    ACK_DECLINED,
    ACK_ABORTED,
};

enum {
    TRIG_MINOR = 1,
    TRIG_MAJOR = 2,
};

typedef struct GCConc {
    /* Request / answer protocol.  Each field has one writer. */
    _Atomic uint32_t request;     /* GC: REQ_* posted; JS: cleared when answered */
    _Atomic uint32_t req_seq;     /* GC */
    _Atomic uint32_t req_kind;    /* GC: cycle kind of the request */
    _Atomic uint64_t req_budget_ns; /* GC: budget of the request (0 = none) */
    _Atomic uint32_t ack;         /* JS: ACK_* */
    _Atomic uint32_t ack_seq;     /* JS: req_seq of the last answered request */

    _Atomic uint32_t trigger;     /* JS: TRIG_* wanted */
    _Atomic uint32_t wake;        /* futex word the GC thread sleeps on */
    _Atomic uint32_t abort;       /* JS: abandon the cycle */
    _Atomic uint32_t idle;        /* GC: 1 when no cycle is running */
    _Atomic uint32_t stop;        /* JS: thread shutdown */
    _Atomic uint64_t tick;        /* JS: boundaries passed */

    bool running;                 /* JS: the thread exists */
    bool in_boundary;             /* JS: a boundary step is on the stack */
    bool force_full;              /* JS: run gc_run at the next poll */
    GCThread thread;

    /* Allocation pacing (JS thread). */
    size_t young_bytes;           /* since the last minor start */
    size_t old_bytes;             /* since the last major start */

    /* Work handed between the threads with the request/answer protocol. */
    U32Vec pending_cards;         /* pulled but not yet rescanned */

    /* Weak processing results (GC thread -> weak apply step). */
    U32Vec weak_dead[3];          /* WeakMap/WeakSet states, WeakRefs, FinalizationRegistries */
    void *weak_shapes;            /* the shape table the GC thread cleaned */
    bool weak_finalizers;         /* some registered finalizer's object died */

    /* Remembered set: one bit per card written since the last minor start
     * (acc), and the set the running minor cycle consumes (cur). */
    uint64_t *remset_acc;
    uint64_t *remset_cur;
    uint32_t card_count;
} GCConc;

static GCConc g_conc;

/* Debug (QJS_GC_STATS=1): what the collector costs the JS thread, printed at
 * exit: boundary steps by kind, full collections, waits for the GC thread,
 * and how long the GC thread waits for the JS thread to reach a poll. */
static int g_gstats = -1;
static struct {
    uint64_t n, ns, max_ns, declined;        /* JS thread: the step itself */
    uint64_t wait_ns, wait_max_ns;           /* GC thread: post to answer, less the step */
} g_gstep[4];
static _Atomic uint64_t g_gstep_last_ns;     /* JS thread -> GC thread: the step just answered */
static uint64_t g_gwait_idle_ns, g_gwait_idle_n;
static _Atomic uint64_t g_gthread_cpu_ns;    /* GC thread: its CPU time after its last cycle */
static uint64_t g_gcycle_n[2], g_gcycle_ns[2];
static uint64_t g_gjs_cpu0_ns;

static void gstats_dump(void) {
    static const char *names[4] = { "none", "mark start", "remark", "weak apply" };
    uint64_t js_cpu = gc_os_thread_cpu_ns() - g_gjs_cpu0_ns, total = 0;
    fprintf(stderr, "[GC-STATS] JS thread CPU %.1f ms; GC thread CPU %.1f ms\n",
            js_cpu / 1e6, atomic_load(&g_gthread_cpu_ns) / 1e6);
    for (int k = 1; k < 4; k++) {
        total += g_gstep[k].ns;
        fprintf(stderr, "[GC-STATS] %-10s: %llu steps, %.2f ms total, %.2f ms max, %llu declined; "
                "GC thread waited %.1f ms for them (%.2f ms max)\n", names[k],
                (unsigned long long)g_gstep[k].n, g_gstep[k].ns / 1e6, g_gstep[k].max_ns / 1e6,
                (unsigned long long)g_gstep[k].declined, g_gstep[k].wait_ns / 1e6, g_gstep[k].wait_max_ns / 1e6);
    }
    fprintf(stderr, "[GC-STATS] full collections: %llu, %.2f ms (%.2f ms of it waiting for the GC thread), %.2f ms max\n",
            (unsigned long long)g_core.stats.full_collections, g_core.stats.full_ns_total / 1e6 + g_core.stats.full_wait_ns_total / 1e6,
            g_core.stats.full_wait_ns_total / 1e6, g_core.stats.full_ns_max / 1e6);
    fprintf(stderr, "[GC-STATS] gc_wait_idle: %llu calls, %.2f ms\n", (unsigned long long)g_gwait_idle_n, g_gwait_idle_ns / 1e6);
    total += g_core.stats.full_ns_total + g_core.stats.full_wait_ns_total + g_gwait_idle_ns;
    fprintf(stderr, "[GC-STATS] cycles: %llu minor (%.1f ms), %llu major (%.1f ms) on the GC thread\n",
            (unsigned long long)g_gcycle_n[0], g_gcycle_ns[0] / 1e6, (unsigned long long)g_gcycle_n[1], g_gcycle_ns[1] / 1e6);
    fprintf(stderr, "[GC-STATS] JS thread total in GC steps and waits: %.2f ms (%.1f%% of its CPU)\n",
            total / 1e6, js_cpu ? 100.0 * total / js_cpu : 0.0);
    fflush(stderr);
}

static inline bool gstats_on(void) {
    if (__builtin_expect(g_gstats < 0, 0)) {
        g_gstats = getenv("QJS_GC_STATS") != NULL;
        if (g_gstats) { g_gjs_cpu0_ns = gc_os_thread_cpu_ns(); atexit(gstats_dump); }
    }
    return g_gstats;
}

static inline bool aborted(void) {
    return atomic_load_explicit(&g_conc.abort, memory_order_acquire) ||
           atomic_load_explicit(&g_conc.stop, memory_order_acquire);
}

/* The poll flag tells the JS thread to look for work at its next poll.
 * The GC thread sets it when it posts a request; the JS thread sets it for
 * a forced collection.  Only the JS thread clears it, when it answers a
 * request (the GC thread is then waiting, so nothing new can be pending)
 * or when the GC thread is known to be idle. */
static void request_poll(void) {
    atomic_store_explicit(&g_gc_poll_flag, 1, memory_order_release);
}

/* ============================================================================
 * Dirty tracking
 * ============================================================================ */

static inline uintptr_t heap_used_end(void) {
    return (uintptr_t)g_gc.heap_base + ((uintptr_t)atomic_load(&g_core.region_hwm) << GC_REGION_SHIFT);
}

static inline void remset_note(uint32_t card) {
    if (g_conc.remset_acc)
        g_conc.remset_acc[card >> 6] |= (uint64_t)1 << (card & 63);
}

typedef struct PullCtx {
    U32Vec *out;
} PullCtx;

#if !GC_SOFTWARE_DIRTY
static void pull_page_cb(void *page, void *opaque) {
    PullCtx *c = (PullCtx *)opaque;
    uint32_t card = (uint32_t)(((uintptr_t)page - (uintptr_t)g_gc.heap_base) >> GC_CARD_SHIFT);
    remset_note(card);
    if (c->out) u32_push(c->out, card);
}
#endif

#if GC_SOFTWARE_DIRTY && defined(_WIN32)
/* ============================================================================
 * Barrier checker (QJS_GC_BARRIER_CHECK=1, software-dirty builds on Windows)
 * ============================================================================
 * The OS write watch sees every store to the heap; the software cards see only
 * stores made through the write helpers.  At each pull, a page the OS saw
 * written but whose card is clean had a store that bypassed the helpers.  The
 * page is compared with a shadow copy from the previous pull, and changed
 * words that hold references are counted by object type and offset.  Objects
 * whose header changed since the shadow (new or moved objects: initialising
 * stores) and frames (written without helpers by design) are skipped. */
static int g_bcheck = -1;
static uint8_t *g_bshadow;            /* mirror of the heap reserve */
static uint8_t *g_bshadow_committed;  /* one byte per 64 KB chunk */
static uint64_t g_bcheck_pages, g_bcheck_flagged_pages, g_bcheck_words;
static uint64_t g_bcheck_by_type[32];

#define BCHECK_KEYS 8192
static struct { uint32_t key, count; GCHandle example; uint16_t class_id, type; uint32_t off; } g_bkeys[BCHECK_KEYS];

static void bcheck_note(uint32_t type, uint32_t off, GCHandle obj, uint16_t class_id) {
    /* By allocation site when the history records them, else by (type, offset). */
    uint32_t site = g_handle_history && obj < gc_handle_limit() ? g_handle_history[obj].site : 0;
    uint32_t key = site ? 0x80000000u | (type >= 32 ? 0x40000000u : 0) | site : (type << 16) | (off < 0xffff ? off : 0xffff);
    uint32_t i = (key * 2654435761u) % BCHECK_KEYS;
    for (uint32_t k = 0; k < BCHECK_KEYS; k++, i = (i + 1) % BCHECK_KEYS) {
        if (g_bkeys[i].count && g_bkeys[i].key == key) { g_bkeys[i].count++; return; }
        if (!g_bkeys[i].count) {
            g_bkeys[i].key = key; g_bkeys[i].count = 1;
            g_bkeys[i].example = obj; g_bkeys[i].class_id = class_id;
            g_bkeys[i].type = (uint16_t)type; g_bkeys[i].off = off;
            /* Now as well as at exit: next to what was running. */
            fprintf(stderr, "[BARRIER-CHECK] new site: type %u offset %u, object %u (site %u)\n",
                    type, off, (unsigned)obj, site);
            return;
        }
    }
}

static void bcheck_report(void) {
    uint32_t order[BCHECK_KEYS], n = 0;
    for (uint32_t i = 0; i < BCHECK_KEYS; i++) if (g_bkeys[i].count) order[n++] = i;
    for (uint32_t a = 0; a < n; a++)            /* small n: selection by count */
        for (uint32_t b = a + 1; b < n; b++)
            if (g_bkeys[order[b]].count > g_bkeys[order[a]].count) { uint32_t t = order[a]; order[a] = order[b]; order[b] = t; }
    fprintf(stderr, "[BARRIER-CHECK] %llu pages checked, %llu with unbarriered reference stores, %llu words, %u (type, offset) sites\n",
            (unsigned long long)g_bcheck_pages, (unsigned long long)g_bcheck_flagged_pages,
            (unsigned long long)g_bcheck_words, n);
    for (int t = 0; t < 32; t++)
        if (g_bcheck_by_type[t])
            fprintf(stderr, "[BARRIER-CHECK] type %d: %llu words\n", t, (unsigned long long)g_bcheck_by_type[t]);
    for (uint32_t a = 0; a < n && a < 60; a++) {
        uint32_t i = order[a];
        if (g_bkeys[i].key & 0x80000000u) {
            uint64_t rvas[8];
            int nf = gc_debug_site_frames(g_bkeys[i].key & 0x3fffffffu, rvas, 8);
            fprintf(stderr, "[BARRIER-CHECK] site %u: %u words, type %u (e.g. object %u offset %u, class %u), allocated at",
                    g_bkeys[i].key & 0x3fffffffu, g_bkeys[i].count, g_bkeys[i].type, (unsigned)g_bkeys[i].example,
                    g_bkeys[i].off, g_bkeys[i].class_id);
            for (int f = 0; f < nf; f++) fprintf(stderr, " 0x%llx", (unsigned long long)rvas[f]);
            fputc('\n', stderr);
            continue;
        }
        fprintf(stderr, "[BARRIER-CHECK] type %u offset %u%s: %u words (e.g. object %u, class %u)\n",
                g_bkeys[i].key >> 16, g_bkeys[i].key & 0xffff, (g_bkeys[i].key & 0xffff) == 0xffff ? "+" : "",
                g_bkeys[i].count, (unsigned)g_bkeys[i].example, g_bkeys[i].class_id);
    }
    fflush(stderr);
}

static bool bcheck_enabled(void) {
    if (g_bcheck < 0) {
        g_bcheck = getenv("QJS_GC_BARRIER_CHECK") != NULL;
        if (g_bcheck) {
            g_bshadow = (uint8_t *)gc_os_reserve(g_gc.heap_reserved, false);
            g_bshadow_committed = (uint8_t *)calloc(g_gc.heap_reserved >> 16, 1);
            if (!g_bshadow || !g_bshadow_committed) g_bcheck = 0;
            else atexit(bcheck_report);
        }
    }
    return g_bcheck > 0;
}

static bool bcheck_is_ref_handle(uint32_t w) {
    if (w == GC_HANDLE_NULL || w >= atomic_load_explicit(&g_gc.handle_limit, memory_order_acquire)) return false;
    if (!gc_handle_alive(w)) return false;
    /* Atoms and other permanent objects are never collected: no barrier needed. */
    GCRegion *r = gc_core_region_of(gc_deref(w));
    return !(r && r->permanent);
}

/* Page `page` (4 KB) was written with its card clean: report reference
 * words that changed outside new objects, then refresh its shadow. */
static void bcheck_page(uintptr_t page, bool card_clean) {
    if (gc_core_page_is_off((const void *)page)) return;   /* given back: nothing to read */
    size_t off = page - (uintptr_t)g_gc.heap_base;
    size_t chunk = off >> 16;
    uint8_t *shadow = g_bshadow + off;
    if (!g_bshadow_committed[chunk]) {
        if (!gc_os_commit(g_bshadow + (chunk << 16), (size_t)1 << 16)) return;
        g_bshadow_committed[chunk] = 1;
        memcpy(shadow, (void *)page, GC_CARD_SIZE);   /* first sight: nothing to compare */
        return;
    }
    GCRegion *r = gc_core_region_of((void *)page);
    uint32_t st = r ? atomic_load_explicit(&r->state, memory_order_acquire) : 0;
    if (card_clean && r && gc_core_region_in_use(st) && !r->permanent) {
        g_bcheck_pages++;
        uintptr_t top = atomic_load_explicit(&r->top, memory_order_acquire);
        uintptr_t end = page + GC_CARD_SIZE < top ? page + GC_CARD_SIZE : top;
        GCHeader *hdr = gc_core_object_containing((const void *)page);
        uintptr_t p = hdr ? (uintptr_t)hdr : gc_core_next_header(r, page, end);
        bool flagged = false;
        while (p < end) {
            GCHeader *h = (GCHeader *)p;
            size_t total = gc_header_total_size(h);
            if (!total) { p = gc_core_skip_free(r, p, end); continue; }
            uint32_t type = gc_header_type(h);
            /* The header's shadow: if it differs the object is new (or was
             * copied here) since the last pull, and its stores initialise it. */
            bool same_header = p >= page && memcmp(shadow + (p - page), (void *)p, GC_HEADER_SIZE) == 0;
            bool header_before_page = p < page;
            /* A new object counts too when a cycle that does not trace it is
             * running (allocated after mark start): its first stores need the
             * helpers then, like any other.  Reported as type + 32. */
            bool fresh = !same_header && !header_before_page &&
                         atomic_load_explicit(&g_core.cycle_active, memory_order_acquire) && !gc_col_in_cycle(r, h);
            bool skip = type == JS_GC_OBJ_TYPE_STACK_FRAME || type == JS_GC_OBJ_TYPE_ASYNC_FUNCTION ||
                        type == JS_GC_OBJ_TYPE_JS_STRING || type == JS_GC_OBJ_TYPE_JS_STRING_ROPE ||
                        type == JS_GC_OBJ_TYPE_JS_BIGINT || type == JS_GC_OBJ_TYPE_BYTES ||
                        /* shapes: hash table and property entries hold atoms and indexes;
                         * proto_handle is set only through its helper */
                        type == JS_GC_OBJ_TYPE_SHAPE ||
                        !gc_core_header_is_current(h) || (!same_header && !header_before_page && !fresh);
            if (!skip) {
                uintptr_t payload = p + GC_HEADER_SIZE;
                uintptr_t a = payload > page ? payload : page;
                uintptr_t b = p + total < end ? p + total : end;
                if (type == JS_GC_OBJ_TYPE_FUNCTION_BYTECODE) {
                    extern size_t JS_GCBytecodeRefEnd(GCHandle b);
                    uintptr_t ref_end = payload + JS_GCBytecodeRefEnd(h->handle);
                    if (b > ref_end) b = ref_end;
                }
                uint16_t class_id = type == JS_GC_OBJ_TYPE_JS_OBJECT ? *(uint16_t *)payload : 0;
                for (a = (a + 3) & ~(uintptr_t)3; a + 4 <= b; a += 4) {
                    uint32_t cur = *(uint32_t *)a, old = fresh ? 0 : *(uint32_t *)(shadow + (a - page));
                    if (cur == old || !bcheck_is_ref_handle(cur)) continue;
                    /* A GCValue payload (8-aligned) counts only with a reference
                     * tag: the word after it (8-byte values) or the 8 bytes after
                     * it (16-byte ones). */
#ifdef QJS_VALUE8
                    if (((a - payload) & 7) == 0 && a + 8 <= b) {
                        uint32_t tag = *(uint32_t *)(a + 4);
#else
                    if (((a - payload) & 7) == 0 && a + 16 <= b) {
                        uint32_t tag = (uint32_t)*(int64_t *)(a + 8);
#endif
                        if (!((unsigned)(int32_t)tag >= (unsigned)JS_TAG_FIRST) && tag != 0) continue;
                    }
                    bcheck_note(fresh ? type + 32 : type, (uint32_t)(a - payload), h->handle, class_id);
                    g_bcheck_by_type[type & 31]++;
                    g_bcheck_words++;
                    flagged = true;
                }
            }
            p += total;
        }
        if (flagged) g_bcheck_flagged_pages++;
    }
    memcpy(shadow, (void *)page, GC_CARD_SIZE);
}

typedef struct { uintptr_t *pages; uint32_t n, cap; } BCheckPages;
static void bcheck_collect_cb(void *page, void *opaque) {
    BCheckPages *v = (BCheckPages *)opaque;
    if (v->n == v->cap) {
        uint32_t cap = v->cap ? v->cap * 2 : 1024;
        uintptr_t *np = (uintptr_t *)realloc(v->pages, cap * sizeof(uintptr_t));
        if (!np) return;
        v->pages = np; v->cap = cap;
    }
    v->pages[v->n++] = (uintptr_t)page;
}

/* Cards the pull in progress found dirty (one byte per card). */
static uint8_t *g_bpulled;

/* After the card loop of a pull: pull the OS write watch for the same range.
 * A page counts as unbarriered only if its card was clean in this pull and
 * is still clean, so a store that lands between the two (its card set after
 * the card loop) is not mistaken for one. */
static void bcheck_range(uintptr_t lo, uintptr_t hi) {
    static BCheckPages v;
    v.n = 0;
    if (!gc_os_write_watch_pull((void *)lo, hi - lo, bcheck_collect_cb, &v)) return;
    for (uint32_t i = 0; i < v.n; i++) {
        uint32_t card = (uint32_t)((v.pages[i] - (uintptr_t)g_gc.heap_base) >> GC_CARD_SHIFT);
        bool clean = !g_bpulled[card] &&
                     atomic_load_explicit((_Atomic uint8_t *)&g_gc.cards[card], memory_order_acquire) == 0;
        bcheck_page(v.pages[i], clean);
    }
}
#endif

/* Read and reset the dirty state of [lo, hi).  Every dirty card goes into
 * the remembered set and, if `out` is given, into out.  One caller at a
 * time: the GC thread, or the JS thread in a boundary step. */
static void dirty_pull_range(uintptr_t lo, uintptr_t hi, U32Vec *out) {
    if (hi <= lo) return;
#if GC_SOFTWARE_DIRTY
    uint32_t c0 = (uint32_t)((lo - (uintptr_t)g_gc.heap_base) >> GC_CARD_SHIFT);
    uint32_t c1 = (uint32_t)((hi - (uintptr_t)g_gc.heap_base + GC_CARD_SIZE - 1) >> GC_CARD_SHIFT);
    uint8_t *cards = g_gc.cards;
#if defined(_WIN32)
    bool bcheck = bcheck_enabled();
    static U32Vec pulled;
    if (bcheck) {
        if (!g_bpulled) g_bpulled = (uint8_t *)calloc(g_gc.heap_reserved >> GC_CARD_SHIFT, 1);
        pulled.n = 0;
    }
#endif
    uint32_t c = c0;
    while (c < c1) {
        if ((c & 7) == 0 && c + 8 <= c1) {
            uint64_t w;
            memcpy(&w, &cards[c], 8);
            atomic_signal_fence(memory_order_acquire);
            if (!w) { c += 8; continue; }
        }
        if (atomic_load_explicit((_Atomic uint8_t *)&cards[c], memory_order_relaxed) &&
            atomic_exchange_explicit((_Atomic uint8_t *)&cards[c], 0, memory_order_acq_rel)) {
            remset_note(c);
            if (out) u32_push(out, c);
#if defined(_WIN32)
            if (bcheck && g_bpulled) { g_bpulled[c] = 1; u32_push(&pulled, c); }
#endif
        }
        c++;
    }
#if defined(_WIN32)
    if (bcheck && g_bpulled) {
        bcheck_range(lo, hi);
        for (uint32_t i = 0; i < pulled.n; i++) g_bpulled[pulled.items[i]] = 0;
    }
#endif
#else
    PullCtx ctx = { out };
    if (!gc_os_write_watch_pull((void *)lo, hi - lo, pull_page_cb, &ctx)) {
        /* The OS refused: treat the whole range as written. */
        for (uintptr_t a = lo; a < hi; a += GC_CARD_SIZE) {
            uint32_t card = (uint32_t)((a - (uintptr_t)g_gc.heap_base) >> GC_CARD_SHIFT);
            remset_note(card);
            if (out) u32_push(out, card);
        }
    }
#endif
}

static void dirty_pull_all(U32Vec *out) {
    dirty_pull_range((uintptr_t)g_gc.heap_base, heap_used_end(), out);
}

/* ============================================================================
 * Request / answer
 * ============================================================================ */

static uint32_t g_next_kind;   /* GC thread: kind of the cycle being run */

/* GC thread: post a boundary request and sleep until the JS thread answers
 * it.  Returns ACK_*. */
static uint32_t post_request(uint32_t req, uint64_t budget_ns) {
    if (g_core.log_level > 2) fprintf(stderr, "[GC] post request %u\n", req);
    uint32_t seq = atomic_load_explicit(&g_conc.req_seq, memory_order_relaxed) + 1;
    atomic_store_explicit(&g_conc.req_seq, seq, memory_order_relaxed);
    atomic_store_explicit(&g_conc.req_kind, g_next_kind, memory_order_relaxed);
    atomic_store_explicit(&g_conc.req_budget_ns, budget_ns, memory_order_relaxed);
    atomic_store_explicit(&g_conc.request, req, memory_order_release);
    atomic_store_explicit(&g_gc_poll_flag, 1, memory_order_release);
    uint64_t t_post = g_gstats > 0 ? gc_os_now_ns() : 0;
    for (;;) {
        uint32_t a = atomic_load_explicit(&g_conc.ack_seq, memory_order_acquire);
        if (a == seq) {
            if (t_post && req < 4) {
                uint64_t dt = gc_os_now_ns() - t_post, step = atomic_load(&g_gstep_last_ns);
                uint64_t w = dt > step ? dt - step : 0;
                g_gstep[req].wait_ns += w;
                if (w > g_gstep[req].wait_max_ns) g_gstep[req].wait_max_ns = w;
            }
            return atomic_load_explicit(&g_conc.ack, memory_order_acquire);
        }
        if (atomic_load_explicit(&g_conc.stop, memory_order_acquire)) return ACK_ABORTED;
        gc_os_wait((volatile uint32_t *)&g_conc.ack_seq, a, 20);
    }
}

/* JS thread: answer the current request. */
static void answer(uint32_t ack) {
    uint32_t seq = atomic_load_explicit(&g_conc.req_seq, memory_order_relaxed);
    atomic_store_explicit(&g_conc.request, REQ_NONE, memory_order_relaxed);
    atomic_store_explicit(&g_conc.ack, ack, memory_order_relaxed);
    /* Clear the poll flag before the answer is visible: a request posted
     * after the GC thread reads it sets the flag again, after this store. */
    atomic_store_explicit(&g_gc_poll_flag, g_conc.force_full, memory_order_relaxed);
    atomic_store_explicit(&g_conc.ack_seq, seq, memory_order_release);
    gc_os_wake_all((volatile uint32_t *)&g_conc.ack_seq);
}

/* ============================================================================
 * Boundary steps (JS thread)
 * ============================================================================ */

static inline bool over_budget(uint64_t t0, uint64_t budget_ns) {
    return budget_ns && gc_os_now_ns() - t0 > budget_ns;
}

/* Mark start: flip parity, record TAMS, snapshot the roots. */
static void step_mark_start(int kind) {
    JSRuntimeHandle rt = g_gc.rt;
    /* The GC thread holds no pointer into retired registry storage now. */
    gc_core_free_retired();
    if (g_core.trace_handle) gc_core_trace(0, kind == GC_CYCLE_MINOR ? "--- minor mark start" : "--- major mark start");
    /* Holes keep being filled during the cycle.  What is allocated in them
     * lies below TAMS, so their lines are flagged as new: the rest of the
     * holes the contexts are in now, and every hole taken until the sweep is
     * done.  The last cycle's flags go first. */
    gc_core_clear_new_lines();
    atomic_store_explicit(&g_core.cycle_active, true, memory_order_release);
    gc_core_flag_context_holes();
    if (kind == GC_CYCLE_MINOR) {
        /* The regions allocated into since the last minor cycle hold its
         * young objects and are the ones it sweeps.  A region a context is
         * still filling gets young objects for the next one too. */
        uint32_t hwm = atomic_load(&g_core.region_hwm);
        for (uint32_t i = 0; i < hwm; i++) {
            GCRegion *r = &g_core.regions[i];
            r->collecting = r->has_young && gc_core_region_in_use(atomic_load(&r->state));
            r->has_young = r->alloc_owner != GC_OWNER_NONE;
        }
        g_conc.young_bytes = 0;
    } else {
        g_conc.old_bytes = 0;
    }
    /* The remembered set so far goes to this cycle: a minor rescans it; a
     * major traces every object it could lead to, so the next minor needs
     * only the cards written from now on.  retire_remset() drops it when
     * the cycle completes and merges it back if it does not. */
    uint64_t *t = g_conc.remset_cur;
    g_conc.remset_cur = g_conc.remset_acc;
    g_conc.remset_acc = t;   /* cleared by the GC thread after the last cycle */
    uint64_t ta = gc_os_now_ns();
    gc_col_begin_cycle(kind, false);
    uint64_t tb = gc_os_now_ns();
    gc_col_enumerate_roots(rt, false);
    if (g_core.log_level > 3)
        fprintf(stderr, "[GC]   mark start: setup %.1f us, roots %.1f us\n", (tb - ta) / 1e3, (gc_os_now_ns() - tb) / 1e3);
    if (kind == GC_CYCLE_MINOR) {
        gc_col_mark_ref_log(rt, 0);
        g_ref_log.n = 0;
    } else {
        g_ref_log_major_start = g_ref_log.n;
    }
}

/* Remark: roots again, the last dirty cards, finish tracing, then the weak
 * step.  Declines (leaving the work to the GC thread) if over budget. */
static uint32_t step_remark(int kind, uint64_t t0, uint64_t budget_ns) {
    JSRuntimeHandle rt = g_gc.rt;
    uint64_t ta = gc_os_now_ns();
    gc_col_enumerate_roots(rt, false);
    gc_col_mark_ref_log(rt, kind == GC_CYCLE_MINOR ? 0 : g_ref_log_major_start);
    uint64_t tb = gc_os_now_ns();
    dirty_pull_all(&g_conc.pending_cards);
    uint32_t ncards = g_conc.pending_cards.n;
    uint64_t tc = gc_os_now_ns();
    for (;;) {
        if (!gc_col_drain(rt, 256)) {
            if (over_budget(t0, budget_ns)) return ACK_DECLINED;
            continue;
        }
        if (g_conc.pending_cards.n == 0) break;
        uint32_t card = g_conc.pending_cards.items[--g_conc.pending_cards.n];
        gc_col_rescan_card(rt, card);
        g_core.stats.dirty_cards_rescanned++;
        if ((g_conc.pending_cards.n & 63) == 0 && over_budget(t0, budget_ns))
            return ACK_DECLINED;
    }
    /* Marking is complete. */
    if (g_core.trace_handle) gc_core_trace(0, "--- remark done");
    if (g_core.config.verify) gc_col_verify_impl(rt);
    if (g_core.log_level > 3)
        fprintf(stderr, "[GC]   remark: roots %.1f us, pull %.1f us (%u cards), rescan+trace %.1f us\n",
                (tb - ta) / 1e3, (tc - tb) / 1e3, ncards, (gc_os_now_ns() - tc) / 1e3);
    /* Open the weak window: from now until the weak apply step,
     * gc_object_is_dead() answers for this cycle, so weak lookups (shape
     * hash, WeakRef.deref) cannot hand out an object the cycle condemned
     * while the GC thread works out what died.  Handle reuse stays
     * deferred: the GC thread reads weak containers through handles. */
    atomic_store_explicit((_Atomic bool *)&g_gc.weak_step_active, true, memory_order_release);
    if (kind == GC_CYCLE_MAJOR && !g_core.nursery_enabled) g_ref_log.n = 0;
    return ACK_DONE;
}

/* Weak apply: fix the weak containers the GC thread flagged, run the
 * finalizers of condemned objects, close the weak window. */
static void close_weak_window(void) {
    atomic_store_explicit((_Atomic bool *)&g_gc.weak_step_active, false, memory_order_release);
}

static uint32_t step_weak_apply(void) {
    JSRuntimeHandle rt = g_gc.rt;
    const GCHandle *lists[3];
    uint32_t counts[3];
    for (int k = 0; k < 3; k++) {
        lists[k] = g_conc.weak_dead[k].items;
        counts[k] = g_conc.weak_dead[k].n;
    }
    if (rt != GC_HANDLE_NULL) JS_GCWeakApply(rt, g_conc.weak_shapes, lists, counts);
    gc_col_finalize_dead(rt, g_conc.weak_finalizers);
    close_weak_window();
    return ACK_DONE;
}

/* Run the pending boundary step, if any. */
static void run_boundary(uintptr_t stack_lo) {
    if (g_conc.in_boundary) return;
    if (g_conc.force_full) {
        g_conc.force_full = false;
        gc_run();   /* (timed in gc_run) */
        return;
    }
    uint32_t req = atomic_load_explicit(&g_conc.request, memory_order_acquire);
    if (req == REQ_NONE) return;
    g_conc.in_boundary = true;
    uint64_t t0 = gc_os_now_ns();
    uint64_t budget = atomic_load_explicit(&g_conc.req_budget_ns, memory_order_relaxed);
    int kind = (int)atomic_load_explicit(&g_conc.req_kind, memory_order_relaxed);
    gc_col_capture_stack(stack_lo);
    uint32_t ack = ACK_DONE;
    switch (req) {
    case REQ_MARK_START:
        step_mark_start(kind);
        break;
    case REQ_REMARK:
        ack = step_remark(kind, t0, budget);
        break;
    case REQ_WEAK_APPLY:
        ack = step_weak_apply();
        break;
    }
    uint64_t dt = gc_os_now_ns() - t0;
    if (g_core.log_level > 2)
        fprintf(stderr, "[GC] boundary step %u (kind %d): %.1f us%s\n", req, kind, dt / 1e3,
                ack == ACK_DECLINED ? " (declined)" : "");
    g_core.stats.boundary_steps++;
    g_core.stats.boundary_ns_total += dt;
    if (dt > g_core.stats.boundary_ns_max) g_core.stats.boundary_ns_max = dt;
    if (ack == ACK_DECLINED) g_core.stats.boundary_declines++;
    if (gstats_on() && req < 4) {
        g_gstep[req].n++;
        g_gstep[req].ns += dt;
        if (dt > g_gstep[req].max_ns) g_gstep[req].max_ns = dt;
        if (ack == ACK_DECLINED) g_gstep[req].declined++;
        atomic_store(&g_gstep_last_ns, dt);
    }
    atomic_fetch_add_explicit(&g_conc.tick, 1, memory_order_relaxed);
    g_conc.in_boundary = false;
    answer(ack);
}

#if defined(__GNUC__) || defined(__clang__)
#define GC_NOINLINE __attribute__((noinline))
#else
#define GC_NOINLINE
#endif

/* Spill callee-saved registers into this frame, so the stack scan of the
 * boundary step sees values the callers keep only in registers. */
GC_NOINLINE static void boundary_with_stack(void) {
    jmp_buf regs;
    setjmp(regs);
    volatile uintptr_t here = (uintptr_t)&regs;
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : : "r"(&regs) : "memory");
#endif
    uintptr_t lo = here < (uintptr_t)&here ? here : (uintptr_t)&here;
    run_boundary(lo);
}

GC_NOINLINE void gc_poll_slow(void) {
    if (!g_gc.initialized) return;
    boundary_with_stack();
}

GC_NOINLINE void gc_tick(void) {
    if (!g_gc.initialized) return;
    atomic_fetch_add_explicit(&g_conc.tick, 1, memory_order_relaxed);
    if (atomic_load_explicit(&g_gc_poll_flag, memory_order_acquire))
        boundary_with_stack();
}

/* ============================================================================
 * Allocation pacing (JS thread)
 * ============================================================================ */

static void trigger(uint32_t what) {
    if ((atomic_load_explicit(&g_conc.trigger, memory_order_relaxed) & what) == what) return;
    uint32_t old = atomic_fetch_or_explicit(&g_conc.trigger, what, memory_order_acq_rel);
    if ((old & what) != what) {
        atomic_fetch_add_explicit(&g_conc.wake, 1, memory_order_release);
        gc_os_wake_all((volatile uint32_t *)&g_conc.wake);
    }
}

void gc_core_note_allocation(size_t bytes, bool young) {
    if (!g_conc.running) return;
    if (young) {
        g_conc.young_bytes += bytes;
        if (g_conc.young_bytes >= g_core.config.nursery_size) trigger(TRIG_MINOR);
    } else {
        g_conc.old_bytes += bytes;
        if (g_conc.old_bytes >= g_core.config.major_trigger) trigger(TRIG_MAJOR);
        /* The collector is not keeping up: collect synchronously at the
         * next poll instead of letting the heap grow without bound. */
        if (g_conc.old_bytes >= 8 * g_core.config.major_trigger && !g_conc.force_full) {
            g_conc.force_full = true;
            request_poll();
        }
    }
}

void gc_request_cycle(bool major) {
    if (!g_conc.running) return;
    trigger(major ? TRIG_MAJOR : TRIG_MINOR);
}

bool gc_is_background(void) { return g_conc.running; }

bool gc_is_idle(void) {
    if (!g_conc.running) return true;
    return atomic_load_explicit(&g_conc.idle, memory_order_acquire) &&
           !atomic_load_explicit(&g_conc.trigger, memory_order_acquire);
}

bool gc_wait_idle(uint32_t timeout_ms) {
    uint64_t t0 = gc_os_now_ns(), end = t0 + (uint64_t)timeout_ms * 1000000ull;
    bool idle = true;
    while (!gc_is_idle()) {
        gc_tick();
        if (gc_os_now_ns() > end) { idle = false; break; }
        if (!atomic_load_explicit(&g_gc_poll_flag, memory_order_acquire)) gc_os_yield();
    }
    if (gstats_on()) { g_gwait_idle_n++; g_gwait_idle_ns += gc_os_now_ns() - t0; }
    return idle;
}

/* ============================================================================
 * GC thread: marking
 * ============================================================================ */

/* Clear the mark bitmaps of the parity the next cycle uses. */
static void prepare_bitmaps(int kind) {
    if (kind == GC_CYCLE_MINOR) return;   /* it keeps the bitmap in use */
    uint32_t next = g_col.parity ^ 1;
    uint32_t hwm = atomic_load(&g_core.region_hwm);
    for (uint32_t i = 0; i < hwm; i++) {
        GCRegion *r = &g_core.regions[i];
        uint32_t st = atomic_load_explicit(&r->state, memory_order_acquire);
        if (!gc_core_region_in_use(st)) continue;
        memset(r->mark_bits[next], 0, (GC_REGION_SIZE / GC_GRANULE) / 8);
    }
}

/* Trace and rescan dirty cards until a round finds little to do. */
static bool concurrent_mark(JSRuntimeHandle rt) {
    for (int round = 0; ; round++) {
        while (!gc_col_drain(rt, 512))
            if (aborted()) return false;
        while (g_conc.pending_cards.n) {
            uint32_t card = g_conc.pending_cards.items[--g_conc.pending_cards.n];
            gc_col_rescan_card(rt, card);
            g_core.stats.dirty_cards_rescanned++;
            if ((g_conc.pending_cards.n & 255) == 0) {
                if (aborted()) return false;
                while (!gc_col_drain(rt, 512))
                    if (aborted()) return false;
            }
        }
        while (!gc_col_drain(rt, 512))
            if (aborted()) return false;
        if (round >= 16) return true;
        dirty_pull_all(&g_conc.pending_cards);
        if (g_core.log_level > 1)
            fprintf(stderr, "[GC]   round %d: %u dirty cards\n", round, g_conc.pending_cards.n);
        if (g_conc.pending_cards.n < 64) {
            /* Final pass: rescan these here, then pull once more just before
             * remark is posted, so the JS thread only sees the cards written
             * in the moments since. */
            while (g_conc.pending_cards.n) {
                gc_col_rescan_card(rt, g_conc.pending_cards.items[--g_conc.pending_cards.n]);
                g_core.stats.dirty_cards_rescanned++;
            }
            while (!gc_col_drain(rt, 512))
                if (aborted()) return false;
            dirty_pull_all(&g_conc.pending_cards);
            while (g_conc.pending_cards.n) {
                gc_col_rescan_card(rt, g_conc.pending_cards.items[--g_conc.pending_cards.n]);
                g_core.stats.dirty_cards_rescanned++;
            }
            while (!gc_col_drain(rt, 512))
                if (aborted()) return false;
            return true;
        }
    }
}

/* Feed the remembered set of a minor cycle to the marker. */
static void queue_remset(void) {
    uint64_t *bits = g_conc.remset_cur;
    uint32_t words = (g_conc.card_count + 63) / 64;
    uint32_t used = (uint32_t)((heap_used_end() - (uintptr_t)g_gc.heap_base) >> GC_CARD_SHIFT);
    uint32_t used_words = (used + 63) / 64;
    if (used_words > words) used_words = words;
    for (uint32_t w = 0; w < used_words; w++) {
        uint64_t v = bits[w];
        if (!v) continue;
        while (v) {
            uint32_t b = (uint32_t)__builtin_ctzll(v);
            v &= v - 1;
            uint32_t card = w * 64 + b;
            u32_push(&g_conc.pending_cards, card);
        }
    }
    if (g_core.log_level > 1) {
        /* What the remembered set holds: cards with an old (marked) object
         * on them, and how many bytes of old objects they cover. */
        uint32_t with_old = 0, young_only = 0;
        uint64_t old_bytes = 0;
        for (uint32_t i = 0; i < g_conc.pending_cards.n; i++) {
            uint32_t card = g_conc.pending_cards.items[i];
            uintptr_t lo = (uintptr_t)g_gc.heap_base + ((uintptr_t)card << GC_CARD_SHIFT), hi = lo + GC_CARD_SIZE;
            GCRegion *r = gc_core_region_of((void *)lo);
            if (!r || !gc_core_region_in_use(atomic_load(&r->state))) continue;
            uintptr_t top = atomic_load(&r->top);
            if (hi > top) hi = top;
            GCHeader *hdr = gc_core_object_containing((const void *)lo);
            uintptr_t p = hdr ? (uintptr_t)hdr : gc_core_next_header(r, lo, hi);
            uint64_t ob = 0;
            while (p < hi) {
                GCHeader *h = (GCHeader *)p;
                size_t total = gc_header_total_size(h);
                if (!total) { p = gc_core_skip_free(r, p, hi); continue; }
                if (gc_col_is_marked(r, h)) {
                    uintptr_t a = p < lo ? lo : p, e = p + total < hi ? p + total : hi;
                    ob += e - a;
                }
                p += total;
            }
            if (ob) { with_old++; old_bytes += ob; } else young_only++;
        }
        fprintf(stderr, "[GC]   remset: %u cards, %u with old objects (%.1f MB of them), %u young only\n",
                g_conc.pending_cards.n, with_old, old_bytes / 1048576.0, young_only);
    }
}

/* Remark, retrying with a larger budget after each decline. */
static bool remark(JSRuntimeHandle rt) {
    uint64_t budget = (uint64_t)g_core.config.boundary_budget_us * 1000ull;
    for (int attempt = 0; ; attempt++) {
        uint64_t b = attempt >= 8 ? 0 : budget << attempt;
        uint32_t ack = post_request(REQ_REMARK, b);
        if (ack == ACK_DONE) return true;
        if (ack == ACK_ABORTED || aborted()) return false;
        /* Declined: the JS thread left its worklist and cards to us. */
        if (!concurrent_mark(rt)) return false;
    }
}

/* ============================================================================
 * GC thread: sweeping
 * ============================================================================ */

/* Major cycles also run when the heap has grown by its live size (at least
 * major_trigger) since the last one: without moving, garbage in old regions
 * only turns into holes at major cycles. */
static size_t g_live_after_major, g_committed_after_major;   /* GC thread */
static size_t g_swept_live;                                    /* GC thread: the last sweep's live bytes */

static size_t committed_bytes(void) {
    return (size_t)atomic_load(&g_core.regions_in_use) * GC_REGION_SIZE -
           (size_t)atomic_load(&g_core.pages_decommitted) * GC_OS_PAGE_SIZE;
}

static void sweep_concurrent(int kind) {
    uint32_t hwm = atomic_load(&g_core.region_hwm);
    uint32_t n_released = 0, n_offered = 0, n_kept = 0;
    size_t hole_bytes = 0, live_bytes = 0;
    for (uint32_t i = 0; i < hwm; i++) {
        GCRegion *r = &g_core.regions[i];
        uint32_t st = atomic_load_explicit(&r->state, memory_order_acquire);
        if (!gc_core_region_in_use(st) || r->permanent) continue;
        /* A minor cycle sweeps where young objects may be. */
        if (kind == GC_CYCLE_MINOR && !r->collecting) continue;
        /* Hold the region so no context claims it meanwhile.  One a context
         * is filling holes of is swept all the same (skipping it would leave
         * its dead objects current after their referents are freed): what
         * the context allocates is in flagged lines, outside the cycle.  It
         * is neither formatted nor released. */
        uint32_t rs = atomic_load_explicit(&r->recycle_state, memory_order_acquire);
        if (rs == GC_RECYCLE_OWNED ||
            !atomic_compare_exchange_strong(&r->recycle_state, &rs, GC_RECYCLE_SWEEPING)) {
            live_bytes += gc_col_sweep_region(r, r->tams, false);
            n_kept++;
            continue;
        }
        uintptr_t top = atomic_load_explicit(&r->top, memory_order_acquire);
        size_t live = gc_col_sweep_region(r, r->tams, false);
        /* Release a region that is empty and that nothing was allocated in
         * since mark start.  The JS thread only claims FREE regions, so an
         * in-use region it is not filling cannot become its own again. */
        if (live == 0 && top == r->tams && r->alloc_owner == GC_OWNER_NONE && !r->pinned) {
            atomic_store_explicit(&r->recycle_state, GC_RECYCLE_IDLE, memory_order_release);
            gc_core_region_release(r);
            n_released++;
            continue;
        }
        /* Old regions nothing is filling: turn their free lines into holes
         * for the recycling allocation contexts. */
        live_bytes += live;
        size_t hb;
        bool offered = false;
        if (g_core.holes && st == GC_RS_OLD && r->alloc_owner == GC_OWNER_NONE && !r->pinned &&
            (hb = gc_core_format_holes(r))) {
            hole_bytes += hb;
            n_offered++;
            atomic_store_explicit(&r->recycle_state, GC_RECYCLE_AVAILABLE, memory_order_release);
            gc_core_offer_recyclable(r);
            offered = true;
        }
        if (!offered) {
            atomic_store_explicit(&r->recycle_state, GC_RECYCLE_IDLE, memory_order_release);
            n_kept++;
            if (g_core.log_level > 1)
                fprintf(stderr, "[GC]     kept region %u: state %u, top %s tams, owner %u, pinned %d, live %zu of %zu\n",
                        i, st, top == r->tams ? "at" : "past", r->alloc_owner, r->pinned, live,
                        (size_t)(top - (uintptr_t)gc_core_region_start(r)));
        }
    }
    if (g_core.log_level)
        fprintf(stderr, "[GC]   %s sweep: %u regions released, %u offered (%.1f MB in holes), %u kept; %.1f MB live\n",
                kind == GC_CYCLE_MINOR ? "minor" : "major", n_released, n_offered, hole_bytes / 1048576.0, n_kept, live_bytes / 1048576.0);
    g_swept_live = live_bytes;
    atomic_store_explicit(&g_core.cycle_active, false, memory_order_release);
}

/* ============================================================================
 * GC thread: cycles
 * ============================================================================ */

/* After marking: find what died among the weak structures (GC thread).
 * The arrays read here are JS-owned; their storage is never freed while a
 * cycle may read it (gc_core_retire), and entries are published before the
 * count. */
static void weak_compute(JSRuntimeHandle rt) {
    JSHandleArray *arrays[3] = { &g_gc.weakmap_handles, &g_gc.weakref_handles, &g_gc.finrec_handles };
    g_conc.weak_shapes = rt != GC_HANDLE_NULL ? JS_GCWeakCleanShapes(rt) : NULL;
    for (int k = 0; k < 3; k++) {
        g_conc.weak_dead[k].n = 0;
        uint32_t n = atomic_load_explicit((_Atomic uint32_t *)&arrays[k]->count, memory_order_acquire);
        GCHandle *hs = atomic_load_explicit((_Atomic(GCHandle *) *)&arrays[k]->handles, memory_order_acquire);
        for (uint32_t i = 0; i < n; i++) {
            GCHandle h = atomic_load_explicit((_Atomic uint32_t *)&hs[i], memory_order_relaxed);
            if (gc_handle_array_entry_is_valid(h) && JS_GCWeakContainerHasDead(h, k))
                u32_push(&g_conc.weak_dead[k], h);
        }
    }
    g_conc.weak_finalizers = false;
    uint32_t n = atomic_load_explicit((_Atomic uint32_t *)&g_core.finalizers.count, memory_order_acquire);
    GCRegistryEntry *items = atomic_load_explicit((_Atomic(GCRegistryEntry *) *)&g_core.finalizers.items,
                                                  memory_order_acquire);
    for (uint32_t i = 0; i < n && !g_conc.weak_finalizers; i++)
        if (gc_object_is_dead(items[i].handle)) g_conc.weak_finalizers = true;
}

/* The remembered set a cycle took at mark start: cleared when the cycle
 * completed (what it covered is old now), merged back otherwise. */
static void retire_remset(bool completed) {
    uint32_t used = (uint32_t)((heap_used_end() - (uintptr_t)g_gc.heap_base) >> GC_CARD_SHIFT);
    uint32_t words = (used + 63) / 64;
    for (uint32_t w = 0; w < words; w++) {
        if (!completed) g_conc.remset_acc[w] |= g_conc.remset_cur[w];
        g_conc.remset_cur[w] = 0;
    }
}

static void end_concurrent_cycle(void) {
    /* An abandoned cycle never reaches its sweep. */
    atomic_store_explicit(&g_core.cycle_active, false, memory_order_release);
    gc_col_end_cycle();
    g_conc.pending_cards.n = 0;
}

static size_t g_promoted_since_major;   /* GC thread */

#define GC_LOG(...) do { if (g_core.log_level) fprintf(stderr, __VA_ARGS__); } while (0)

static void run_cycle(int kind) {
    JSRuntimeHandle rt = g_gc.rt;
    bool marked = false, started = false;
    uint64_t t_start = gc_os_now_ns(), t_mark, t_remark, t_sweep;
    uint64_t cards0 = g_core.stats.dirty_cards_rescanned;
    g_next_kind = (uint32_t)kind;
    prepare_bitmaps(kind);
    /* Discard the writes made so far (the tracer reads the objects as they
     * are now), recording them in the remembered set.  Writes between this
     * pull and mark start are pulled, and rescanned, during the cycle. */
    dirty_pull_all(NULL);
    if (post_request(REQ_MARK_START, 0) != ACK_DONE) goto abandon;
    started = true;
    if (kind == GC_CYCLE_MINOR) queue_remset();
    if (!concurrent_mark(rt)) goto abandon;
    t_mark = gc_os_now_ns();
    if (!remark(rt)) goto abandon;
    t_remark = gc_os_now_ns();
    marked = true;
    weak_compute(rt);
    if (post_request(REQ_WEAK_APPLY, 0) != ACK_DONE) goto finish;
    sweep_concurrent(kind);
    t_sweep = gc_os_now_ns();
    if (aborted()) goto finish;
    if (kind == GC_CYCLE_MINOR) {
        /* Nothing moves: the survivors, now marked, are old where they are. */
        size_t promoted = atomic_load_explicit(&g_col.promoted, memory_order_relaxed);
        g_core.stats.bytes_promoted += promoted;
        g_promoted_since_major += promoted;
        g_core.stats.minor_cycles++;
    } else {
        g_promoted_since_major = 0;
        g_live_after_major = g_swept_live;
        g_committed_after_major = committed_bytes();
        g_core.stats.major_cycles++;
    }
finish:
    retire_remset(!aborted());
    end_concurrent_cycle();
    if (g_gstats > 0) {
        int m = kind == GC_CYCLE_MINOR ? 0 : 1;
        g_gcycle_n[m]++;
        g_gcycle_ns[m] += gc_os_now_ns() - t_start;
        atomic_store(&g_gthread_cpu_ns, gc_os_thread_cpu_ns());
    }
    GC_LOG("[GC] %s cycle: mark %.2f ms, remark %.2f ms, sweep %.2f ms, evacuate %.2f ms; "
           "cards %llu, heap %u regions, %.0f MB committed\n",
           kind == GC_CYCLE_MINOR ? "minor" : "major",
           (t_mark - t_start) / 1e6, (t_remark - t_mark) / 1e6, (t_sweep - t_remark) / 1e6,
           (gc_os_now_ns() - t_sweep) / 1e6,
           (unsigned long long)(g_core.stats.dirty_cards_rescanned - cards0),
           atomic_load(&g_core.regions_in_use),
           (atomic_load(&g_core.regions_in_use) * (double)GC_REGION_SIZE -
            atomic_load(&g_core.pages_decommitted) * (double)GC_OS_PAGE_SIZE) / 1048576.0);
    return;
abandon:
    /* Marking was not finished: nothing has been freed or moved.  The JS
     * thread recycles deferred frees when it sees the abort. */
    (void)marked;
    retire_remset(false);
    /* An abandoned major cycle's bitmap is partial: go back to the one the
     * marks of old objects stick in.  (The JS thread waits for this.) */
    if (kind != GC_CYCLE_MINOR && started) g_col.parity ^= 1;
    g_core.stats.aborted_cycles++;
    end_concurrent_cycle();
}

static void gc_thread_main(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t seen = atomic_load_explicit(&g_conc.wake, memory_order_acquire);
        if (atomic_load_explicit(&g_conc.stop, memory_order_acquire)) break;
        uint32_t t = 0;
        if (g_gc.rt != GC_HANDLE_NULL) {
            /* Look busy before consuming the trigger, so gc_is_idle() never
             * sees "idle and nothing pending" while a cycle is starting. */
            if (atomic_load_explicit(&g_conc.trigger, memory_order_acquire) || g_core.config.stress)
                atomic_store_explicit(&g_conc.idle, 0, memory_order_seq_cst);
            t = atomic_exchange_explicit(&g_conc.trigger, 0, memory_order_acq_rel);
            if (!t && g_core.config.stress) t = TRIG_MINOR | TRIG_MAJOR;
            if (g_promoted_since_major >= g_core.config.major_trigger) t |= TRIG_MAJOR;
            size_t growth = g_live_after_major > g_core.config.major_trigger ? g_live_after_major : g_core.config.major_trigger;
            if (committed_bytes() >= g_committed_after_major + growth) t |= TRIG_MAJOR;
        }
        if (!t || atomic_load_explicit(&g_conc.abort, memory_order_acquire)) {
            if (t) atomic_fetch_or(&g_conc.trigger, t);
            atomic_store_explicit(&g_conc.idle, 1, memory_order_release);
            gc_os_wake_all((volatile uint32_t *)&g_conc.idle);
            gc_os_wait((volatile uint32_t *)&g_conc.wake, seen, 50);
            continue;
        }
        atomic_store_explicit(&g_conc.idle, 0, memory_order_release);
        if (g_core.nursery_enabled && (t & TRIG_MINOR)) {
            run_cycle(GC_CYCLE_MINOR);
            /* The remembered set the cycle consumed is clear again. */
        }
        if (!aborted() && (t & TRIG_MAJOR))
            run_cycle(GC_CYCLE_MAJOR);
        if (!g_core.nursery_enabled && (t & TRIG_MINOR) && !(t & TRIG_MAJOR) && !aborted())
            run_cycle(GC_CYCLE_MAJOR);
        if (g_core.config.stress) gc_os_yield();
    }
    atomic_store_explicit(&g_conc.idle, 1, memory_order_release);
    gc_os_wake_all((volatile uint32_t *)&g_conc.idle);
}

void gc_core_start_thread(void) {
    if (g_conc.running || !g_core.config.background) return;
    g_conc.card_count = (uint32_t)(GC_HEAP_RESERVE >> GC_CARD_SHIFT);
    size_t words = (g_conc.card_count + 63) / 64;
    if (!g_conc.remset_acc) g_conc.remset_acc = (uint64_t *)calloc(words, sizeof(uint64_t));
    if (!g_conc.remset_cur) g_conc.remset_cur = (uint64_t *)calloc(words, sizeof(uint64_t));
    if (!g_conc.remset_acc || !g_conc.remset_cur) return;
    g_core.nursery_enabled = g_core.config.nursery;
    atomic_store(&g_conc.stop, 0);
    atomic_store(&g_conc.abort, 0);
    atomic_store(&g_conc.idle, 1);
    (void)gstats_on();   /* read by the GC thread from its start */
    if (!gc_os_thread_start(&g_conc.thread, gc_thread_main, NULL)) {
        g_core.nursery_enabled = false;
        return;
    }
    g_conc.running = true;
}

/* Wait (JS thread) until the GC thread has abandoned or finished its cycle.
 * Only gc_run() and shutdown wait; boundary steps never do. */
static void abort_and_wait(void) {
    atomic_store_explicit(&g_conc.abort, 1, memory_order_release);
    atomic_fetch_add_explicit(&g_conc.wake, 1, memory_order_release);
    gc_os_wake_all((volatile uint32_t *)&g_conc.wake);
    for (;;) {
        if (atomic_load_explicit(&g_conc.request, memory_order_acquire) != REQ_NONE)
            answer(ACK_ABORTED);
        if (atomic_load_explicit(&g_conc.idle, memory_order_acquire)) break;
        gc_os_wait((volatile uint32_t *)&g_conc.idle, 0, 5);
    }
    atomic_store_explicit(&g_conc.abort, 0, memory_order_release);
    close_weak_window();
    /* The GC thread is idle: nothing can be pending. */
    atomic_store_explicit(&g_gc_poll_flag, g_conc.force_full, memory_order_release);
}

void gc_core_stop_thread(void) {
    if (!g_conc.running) return;
    atomic_store_explicit(&g_conc.stop, 1, memory_order_release);
    abort_and_wait();
    atomic_fetch_add(&g_conc.wake, 1);
    gc_os_wake_all((volatile uint32_t *)&g_conc.wake);
    gc_os_thread_join(&g_conc.thread);
    g_conc.running = false;
    g_core.nursery_enabled = false;
    atomic_store(&g_conc.stop, 0);
}

/* gc_run() first stops whatever the GC thread is doing. */
void gc_col_finish_concurrent_cycle(uintptr_t stack_lo) {
    (void)stack_lo;
    if (!g_conc.running) return;
    if (atomic_load_explicit(&g_conc.idle, memory_order_acquire) &&
        atomic_load_explicit(&g_conc.request, memory_order_acquire) == REQ_NONE)
        return;
    abort_and_wait();
}

/* After a stop-the-world collection the remembered set starts over: every
 * card written so far was either swept or is still dirty. */
void gc_core_after_full_collection(void) {
    g_conc.young_bytes = 0;
    g_conc.old_bytes = 0;
}

void gc_core_free_internal(void) {
    free(g_col.work.items);
    free(g_col.pins);
    memset(&g_col, 0, sizeof(g_col));
    u32_free(&g_ref_log);
    u32_free(&g_conc.pending_cards);
    for (int k = 0; k < 3; k++) u32_free(&g_conc.weak_dead[k]);
    free(g_conc.remset_acc);
    free(g_conc.remset_cur);
    memset(&g_conc, 0, sizeof(g_conc));
    g_ref_log_major_start = 0;
}

/* ============================================================================
 * Verification: re-mark from scratch and compare with the cycle's marks
 * ============================================================================ */

static uint64_t *v_bits;
static GCHandle *v_stack;
static uint32_t v_n, v_cap;
static GCHandle v_parent;
/* Raw data objects reached from the stack scan or engine roots, visited
 * after everything else so that an owner reaching them is seen first (the
 * data-object scan below needs the owner). */
static GCHandle *v_deferred;
static uint32_t v_ndeferred, v_cap_deferred;

static bool is_explicit_root(GCHandle h) {
    for (uint32_t i = 0; i < g_core.roots.count; i++)
        if (g_core.roots.items[i].handle == h) return true;
    return false;
}

static void verify_cb(JSRuntimeHandle rt, GCHandle h) {
    (void)rt;
    GCHeader *hdr = gc_col_handle_header(h);
    if (!hdr) return;
    uint64_t m = (uint64_t)1 << (h & 63);
    if (v_bits[h >> 6] & m) return;
    if (v_parent == GC_HANDLE_NULL && gc_header_type(hdr) == JS_GC_OBJ_TYPE_DATA && !is_explicit_root(h)) {
        if (v_ndeferred == v_cap_deferred) {
            v_cap_deferred = v_cap_deferred ? v_cap_deferred * 2 : 1024;
            v_deferred = (GCHandle *)realloc(v_deferred, v_cap_deferred * sizeof(GCHandle));
        }
        v_deferred[v_ndeferred++] = h;
        return;
    }
    v_bits[h >> 6] |= m;
    if (v_n == v_cap) {
        v_cap = v_cap ? v_cap * 2 : 4096;
        v_stack = (GCHandle *)realloc(v_stack, v_cap * 2 * sizeof(GCHandle));
    }
    v_stack[2 * v_n] = h;
    v_stack[2 * v_n + 1] = v_parent;
    v_n++;
}

/* Raw data objects (JS_GC_OBJ_TYPE_DATA) have no traced children: whoever
 * owns one marks what it holds.  Scan each reachable one for slots that
 * reference live objects this cycle left unmarked, which a missing mark
 * function would leave to be swept.  "value" slots are GCValues (a handle
 * with a reference tag); "handle" slots (QJS_GC_VERIFY=2) are bare 32-bit
 * words that happen to be the handle of such an object, often a coincidence
 * (a for-in iterator's property list was a real one).  Reported once per
 * owner class, size, offset and kind. */
#define DSCAN_KEYS 256
static struct { int ptype, pclass; size_t size; long off; int kind; uint32_t n; GCHandle first, child; } g_dscan[DSCAN_KEYS];

static void dscan_note(GCHandle parent, GCHeader *dhdr, long off, int kind, GCHandle child) {
    GCHeader *ph = gc_col_handle_header(parent);
    int ptype = ph ? (int)gc_header_type(ph) : -1;
    int pclass = ph && ptype == JS_GC_OBJ_TYPE_JS_OBJECT ? *(const uint16_t *)gc_header_payload(ph) : -1;
    size_t size = gc_header_payload_size(dhdr);
    for (int i = 0; i < DSCAN_KEYS; i++) {
        if (g_dscan[i].n && g_dscan[i].ptype == ptype && g_dscan[i].pclass == pclass &&
            g_dscan[i].size == size && g_dscan[i].off == off && g_dscan[i].kind == kind) {
            g_dscan[i].n++;
            return;
        }
        if (!g_dscan[i].n) {
            g_dscan[i].ptype = ptype; g_dscan[i].pclass = pclass; g_dscan[i].size = size;
            g_dscan[i].off = off; g_dscan[i].kind = kind; g_dscan[i].n = 1;
            g_dscan[i].first = gc_handle_of_header(dhdr); g_dscan[i].child = child;
            GCHeader *ch = gc_col_handle_header(child);
            fprintf(stderr, "[GC verify] data object %u (size %zu, owner %u type %d class %d) holds an unmarked %s "
                    "at offset %ld: handle %u type %d\n",
                    g_dscan[i].first, size, parent, ptype, pclass, kind ? "handle" : "value", off,
                    child, ch ? (int)gc_header_type(ch) : -1);
            return;
        }
    }
}

static bool dscan_unmarked(GCHandle c) {
    if (c == GC_HANDLE_NULL || !gc_handle_live(c)) return false;
    GCHeader *ch = gc_col_handle_header(c);
    if (!ch) return false;
    GCRegion *cr = gc_core_region_of(ch);
    return gc_col_in_cycle(cr, ch) && !gc_col_is_marked(cr, ch);
}

static void dscan_data_object(GCHandle h, GCHandle parent, GCHeader *hdr) {
    /* Reached only from a root.  The conservative stack scan finds raw
     * objects (an array's value storage, say) that nothing traces through,
     * dead or not; only explicit roots must hold marked values. */
    if (parent == GC_HANDLE_NULL && !is_explicit_root(h)) return;
    GCHeader *ph = gc_col_handle_header(parent);
    if (ph && gc_header_type(ph) == JS_GC_OBJ_TYPE_JS_OBJECT) {
        /* Weak containers hold unmarked targets by design. */
        extern bool JS_GCClassIsWeak(int class_id);
        if (JS_GCClassIsWeak(*(const uint16_t *)gc_header_payload(ph))) return;
    }
    /* Bare handles only with QJS_GC_VERIFY=2: C structs hold pointers whose
     * halves, and bytecode bytes, often look like handles. */
    bool bytes = g_core.config.verify < 2 ||
                 (ph && gc_header_type(ph) == JS_GC_OBJ_TYPE_FUNCTION_BYTECODE);
    const uint8_t *p = (const uint8_t *)gc_header_payload(hdr);
    size_t n = gc_header_payload_size(hdr);
    if (ph) {
        extern size_t JS_GCDataLiveBytes(GCHandle owner, GCHandle data);
        size_t live = JS_GCDataLiveBytes(parent, h);
        if (live < n) n = live;
    }
#ifdef QJS_VALUE8
    /* A value: the handle in the low word, a reference tag in the high one. */
    for (size_t k = 0; k + 8 <= n; k += 8) {
        int32_t tag = *(const int32_t *)(p + k + 4);
#else
    for (size_t k = 0; k + 16 <= n; k += 8) {
        int64_t tag = *(const int64_t *)(p + k + 8);
#endif
        if (tag < JS_TAG_FIRST || tag >= 0) continue;
        GCHandle c = *(const uint32_t *)(p + k);
        if (dscan_unmarked(c)) dscan_note(parent, hdr, (long)k, 0, c);
    }
    /* Bare handles: words outside anything that looks like a GCValue (an
     * int or the halves of a double in a value array would match by
     * chance). */
    for (size_t k = 0; !bytes && k + 4 <= n; k += 4) {
#ifdef QJS_VALUE8
        if (k % 8 == 0 && k + 8 <= n) {
            int32_t tag = *(const int32_t *)(p + k + 4);
            if (tag >= JS_TAG_FIRST && tag < JS_TAG_FLOAT64) { k += 4; continue; }
        }
#else
        if (k % 8 == 0 && k + 16 <= n) {
            int64_t tag = *(const int64_t *)(p + k + 8);
            if (tag >= JS_TAG_FIRST && tag <= JS_TAG_FLOAT64) { k += 12; continue; }
        }
#endif
        GCHandle c = *(const uint32_t *)(p + k);
        if (c != h && dscan_unmarked(c)) dscan_note(parent, hdr, (long)k, 1, c);
    }
}

/* Re-trace everything reachable now (JS thread, marking just finished) and
 * report objects the cycle left unmarked.  In a minor cycle the trace goes
 * through old objects too, so it also checks the remembered set. */
void gc_col_verify_impl(JSRuntimeHandle rt) {
    uint32_t limit = gc_handle_limit();
    v_bits = (uint64_t *)calloc((limit + 63) / 64 + 1, sizeof(uint64_t));
    if (!v_bits) return;
    v_n = 0;
    v_parent = GC_HANDLE_NULL;
    gc_col_enumerate_roots_cb(rt, verify_cb);
    for (uint32_t i = 0; i < g_ref_log.n; i++) verify_cb(rt, g_ref_log.items[i]);
    uint32_t failures = 0;
    while (v_n > 0 || v_ndeferred > 0) {
        if (v_n == 0) {
            /* Raw data objects no owner reached: check them as roots. */
            uint32_t nd = v_ndeferred;
            v_ndeferred = 0;
            for (uint32_t i = 0; i < nd; i++) {
                GCHandle d = v_deferred[i];
                uint64_t m = (uint64_t)1 << (d & 63);
                if (v_bits[d >> 6] & m) continue;
                v_bits[d >> 6] |= m;
                if (v_n == v_cap) {
                    v_cap = v_cap ? v_cap * 2 : 4096;
                    v_stack = (GCHandle *)realloc(v_stack, v_cap * 2 * sizeof(GCHandle));
                }
                v_stack[2 * v_n] = d;
                v_stack[2 * v_n + 1] = GC_HANDLE_NULL;
                v_n++;
            }
            if (v_n == 0) break;
        }
        v_n--;
        GCHandle h = v_stack[2 * v_n];
        GCHandle parent = v_stack[2 * v_n + 1];
        GCHeader *hdr = gc_col_handle_header(h);
        if (!hdr) continue;
        GCRegion *r = gc_core_region_of(hdr);
        if (gc_col_in_cycle(r, hdr) && !gc_col_is_marked(r, hdr)) {
            if (failures < 20) {
                GCHeader *ph = gc_col_handle_header(parent);
                long off = -1;
                int card_dirty = -1;
                if (ph) {
                    /* Where in the parent the reference sits, and whether
                     * that card is still flagged. */
                    const uint8_t *pp = (const uint8_t *)gc_header_payload(ph);
                    size_t n = gc_header_payload_size(ph);
                    for (size_t k = 0; k + 4 <= n; k += 4)
                        if (*(const uint32_t *)(pp + k) == h) { off = (long)k; break; }
#if GC_SOFTWARE_DIRTY
                    if (off >= 0) {
                        uintptr_t a = (uintptr_t)pp + (uintptr_t)off - (uintptr_t)g_gc.heap_base;
                        card_dirty = g_gc.cards[a >> GC_CARD_SHIFT];
                    }
#endif
                }
                int pclass = ph && gc_header_type(ph) == JS_GC_OBJ_TYPE_JS_OBJECT
                             ? *(const uint16_t *)gc_header_payload(ph) : -1;
                GCRegion *pr = ph ? gc_core_region_of(ph) : NULL;
                fprintf(stderr, "[GC verify] reachable but unmarked: handle %u type %d size %zu"
                        " (referenced by handle %u type %d class %d at offset %ld, card flag %d, parent region %s)\n",
                        h, (int)gc_header_type(hdr), gc_header_payload_size(hdr),
                        parent, ph ? (int)gc_header_type(ph) : -1, pclass, off, card_dirty,
                        ph ? (pr->collecting ? "collecting" :
                              pr->space == GC_RS_NURSERY ? "nursery" : "old") : "-");
                if (ph)
                    fprintf(stderr, "[GC verify]   cycle %s; parent region %u recycle %u owner %u, parent %s tams, "
                            "in cycle %d, marked %d; child region %u %s; cycle active %d\n",
                            g_col.kind == GC_CYCLE_MINOR ? "minor" : g_col.kind == GC_CYCLE_MAJOR ? "major" : "full",
                            (unsigned)(pr - g_core.regions), atomic_load(&pr->recycle_state), pr->alloc_owner,
                            (uintptr_t)ph < pr->tams ? "below" : "above",
                            gc_col_in_cycle(pr, ph), gc_col_is_marked(pr, ph),
                            (unsigned)(r - g_core.regions), r->collecting ? "collecting" : r->space == GC_RS_NURSERY ? "nursery" : "old",
                            (int)atomic_load(&g_core.cycle_active));
                if (g_handle_history) {
                    gc_debug_report_stale_handle(h, "[GC verify]   child");
                    if (ph) gc_debug_report_stale_handle(parent, "[GC verify]   parent");
                }
            }
            failures++;
            if (g_core.trace_handle && failures <= 3) {
                fprintf(stderr, "[GC verify] history of %u:\n", h);
                gc_core_trace_dump(h);
                fprintf(stderr, "[GC verify] history of parent %u:\n", parent);
                gc_core_trace_dump(parent);
            }
        }
        if (gc_header_type(hdr) == JS_GC_OBJ_TYPE_DATA) dscan_data_object(h, parent, hdr);
        v_parent = h;
        mark_children(rt, h, verify_cb);
    }
    v_parent = GC_HANDLE_NULL;
    v_ndeferred = 0;
    free(v_bits);
    v_bits = NULL;
    if (failures) {
        fprintf(stderr, "[GC verify] %u reachable objects were not marked\n", failures);
        g_core.stats.verify_failures += failures;
    }
}

/* ============================================================================
 * Heap consistency check (tests and debugging)
 * ============================================================================ */

static uint32_t g_check_problems;

static void check_object(GCHeader *hdr, void *opaque) {
    GCRegion *r = (GCRegion *)opaque;
    size_t total = gc_header_total_size(hdr);
    if (total < GC_GRANULE || ((uintptr_t)hdr & 15) != GC_HEADER_SIZE) {
        g_check_problems++;
        return;
    }
    /* Found from its first and last bytes and from its payload. */
    if (gc_core_object_containing(hdr) != hdr ||
        gc_core_object_containing((uint8_t *)hdr + total - 1) != hdr ||
        gc_core_object_containing(gc_header_payload(hdr)) != hdr) {
        if (g_check_problems < 5)
            fprintf(stderr, "[GC check] lookup misses object %p (size %zu, region %u, space %u) from %p/%p/%p: %p %p %p\n",
                    (void *)hdr, total, (unsigned)(r - g_core.regions), r->space, (void *)hdr,
                    (void *)((uint8_t *)hdr + total - 1), gc_header_payload(hdr),
                    (void *)gc_core_object_containing(hdr), (void *)gc_core_object_containing((uint8_t *)hdr + total - 1),
                    (void *)gc_core_object_containing(gc_header_payload(hdr)));
        g_check_problems++;
    }
    /* A current object's header names its own handle. */
    if (gc_core_header_is_current(hdr) && hdr->handle != gc_handle_of_header(hdr)) g_check_problems++;
}

/* Returns the number of inconsistencies found: object headers that are
 * misaligned or not found from their own addresses, and live handles that
 * are not the header of an object.  Waits for the collector to be idle. */
/* QJS_GC_CENSUS=1: at exit, after a full collection, the live objects by
 * type (and JS objects by class, data objects by size): what fills the
 * heap. */
static uint64_t census_refs[32 + 256], census_root_refs;
static int census_parent;
static void census_cb(JSRuntimeHandle rt, GCHandle h) {
    (void)rt;
    void *p = h ? gc_deref(h) : NULL;
    GCHeader *hdr = p ? gc_core_object_containing(p) : NULL;
    if (!hdr || gc_header_type(hdr) != JS_GC_OBJ_TYPE_STACK_FRAME) return;
    if (census_parent < 0) census_root_refs++;
    else census_refs[census_parent]++;
}

void gc_census_dump(void) {
    gc_run();
    gc_wait_idle(20000);
    uint64_t tn[32] = {0}, tb[32] = {0}, cn[256] = {0}, cb[256] = {0};
    uint64_t dn[8] = {0}, db[8] = {0};   /* DATA by size class: <64, <256, <1K, <4K, <16K, <64K, <256K, more */
    uint64_t total = 0, count = 0;
    for (GCHandle h = gc_next_object(0); h; h = gc_next_object(h)) {
        void *p = gc_deref(h);
        GCHeader *hdr = gc_core_object_containing(p);
        if (!hdr) continue;
        uint32_t t = gc_header_type(hdr) & 31;
        size_t sz = gc_header_total_size(hdr);
        tn[t]++; tb[t] += sz; total += sz; count++;
        if (t == JS_GC_OBJ_TYPE_JS_OBJECT) {
            uint16_t cls = *(const uint16_t *)gc_header_payload(hdr);
            cn[cls & 255]++; cb[cls & 255] += sz;
        } else if (t == JS_GC_OBJ_TYPE_DATA) {
            int k = sz < 64 ? 0 : sz < 256 ? 1 : sz < 1024 ? 2 : sz < 4096 ? 3 : sz < 16384 ? 4 : sz < 65536 ? 5 : sz < 262144 ? 6 : 7;
            dn[k]++; db[k] += sz;
        }
    }
    fprintf(stderr, "[census] %llu objects, %.1f MB live\n", (unsigned long long)count, total / 1048576.0);
    for (int t = 0; t < 32; t++)
        if (tn[t]) fprintf(stderr, "[census] type %2d: %9llu objects %9.1f MB\n", t, (unsigned long long)tn[t], tb[t] / 1048576.0);
    for (int c = 0; c < 256; c++)
        if (cb[c] > (1u << 20)) fprintf(stderr, "[census]   class %3d: %9llu objects %9.1f MB\n", c, (unsigned long long)cn[c], cb[c] / 1048576.0);
    static const char *dsz[] = { "<64", "<256", "<1K", "<4K", "<16K", "<64K", "<256K", ">=256K" };
    for (int k = 0; k < 8; k++)
        if (dn[k]) fprintf(stderr, "[census]   data %-7s %9llu objects %9.1f MB\n", dsz[k], (unsigned long long)dn[k], db[k] / 1048576.0);

    /* Who references the stack frames: parents by type (and class). */
    JSRuntimeHandle rt = g_gc.rt;
    memset(census_refs, 0, sizeof census_refs);
    census_parent = -1;
    gc_col_enumerate_roots_cb(rt, census_cb);
    for (GCHandle h = gc_next_object(0); h; h = gc_next_object(h)) {
        GCHeader *hdr = gc_core_object_containing(gc_deref(h));
        if (!hdr) continue;
        uint32_t t = gc_header_type(hdr) & 31;
        census_parent = t == JS_GC_OBJ_TYPE_JS_OBJECT ? 32 + (*(const uint16_t *)gc_header_payload(hdr) & 255) : (int)t;
        mark_children(rt, h, census_cb);
    }
    fprintf(stderr, "[census] stack-frame references: roots %llu\n", (unsigned long long)census_root_refs);
    for (int k = 0; k < 32 + 256; k++)
        if (census_refs[k])
            fprintf(stderr, "[census]   from %s %3d: %llu\n", k < 32 ? "type " : "class", k < 32 ? k : k - 32,
                    (unsigned long long)census_refs[k]);
    fflush(stderr);
}

uint32_t gc_test_heap_check(void) {
    /* A sweep formats holes as it goes: walk the heap between cycles. */
    gc_wait_idle(20000);
    g_check_problems = 0;
    uint32_t hwm = atomic_load(&g_core.region_hwm);
    for (uint32_t i = 0; i < hwm; i++) {
        GCRegion *r = &g_core.regions[i];
        uint32_t st = atomic_load(&r->state);
        if (!gc_core_region_in_use(st)) continue;
        gc_core_walk_region(r, (uintptr_t)gc_core_region_start(r) + GC_HEADER_SIZE,
                            atomic_load(&r->top), check_object, r);
    }
    for (GCHandle h = gc_next_object(0); h; h = gc_next_object(h)) {
        void *p = gc_deref(h);
        GCHeader *hdr = gc_core_object_containing(p);
        if (!hdr || gc_header_payload(hdr) != p || hdr->handle != h) g_check_problems++;
    }
    return g_check_problems;
}

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
 *   Windows: the heap is reserved with MEM_WRITE_WATCH and GetWriteWatch
 *   reports written pages.  Elsewhere the engine's write helpers set one
 *   byte per 4 KB card.  Both are pulled through dirty_pull(), which also
 *   records every card in the nursery's remembered set.  The software flags
 *   cover reference stores only, so on those platforms a copy is validated
 *   at the flip by comparing it with the original.
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
 * Freed-handle ring: GC thread -> JS thread
 * ============================================================================ */

static GCRing g_freed_ring;

static GCRingBlock *ring_block_new(void) {
    GCRingBlock *b = (GCRingBlock *)calloc(1, sizeof(GCRingBlock));
    if (!b) {
        fprintf(stderr, "[GC] out of memory (ring)\n");
        abort();
    }
    return b;
}

/* Producer side (one thread at a time: the GC thread, or the JS thread in
 * a stop-the-world collection). */
void gc_col_freed_ring_push(GCHandle h) {
    GCRing *q = &g_freed_ring;
    if (!q->tail) {
        q->tail = ring_block_new();
        q->head = q->tail;
    }
    GCRingBlock *b = q->tail;
    uint32_t n = atomic_load_explicit(&b->count, memory_order_relaxed);
    if (n == GC_RING_BLOCK) {
        GCRingBlock *nb = ring_block_new();
        atomic_store_explicit(&b->next, nb, memory_order_release);
        q->tail = nb;
        b = nb;
        n = 0;
    }
    b->items[n] = h;
    atomic_store_explicit(&b->count, n + 1, memory_order_release);
}

/* Consumer side (JS thread).  The head block is kept (the producer may
 * still be filling it). */
void gc_core_drain_freed_ring(void) {
    GCRing *q = &g_freed_ring;
    GCRingBlock *b = q->head;
    while (b) {
        uint32_t n = atomic_load_explicit(&b->count, memory_order_acquire);
        while (b->consumed < n)
            gc_core_hcache_push(b->items[b->consumed++]);
        if (n < GC_RING_BLOCK) break;
        GCRingBlock *next = atomic_load_explicit(&b->next, memory_order_acquire);
        if (!next) break;
        q->head = next;
        free(b);
        b = next;
    }
}

/* ============================================================================
 * JS-thread side lists: deferred frees and the reference log
 * ============================================================================ */

/* Handles the JS thread freed while a cycle was marking.  Reusing one then
 * could hand the marker an object of another type behind a handle it read
 * earlier; they are recycled when marking ends. */
static U32Vec g_deferred_frees;

/* gc_note_ref: references stored in off-heap memory (no dirty flag). */
static U32Vec g_ref_log;
static uint32_t g_ref_log_major_start;

void gc_core_free_handle_from_js(GCHandle h) {
    if (g_core.defer_frees) u32_push(&g_deferred_frees, h);
    else gc_core_hcache_push(h);
}

static void release_deferred_frees(void) {
    for (uint32_t i = 0; i < g_deferred_frees.n; i++)
        gc_core_hcache_push(g_deferred_frees.items[i]);
    g_deferred_frees.n = 0;
}

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
    REQ_FLIP,
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

typedef struct GCMoved {
    GCHandle h;
    uint32_t total;     /* bytes including the header */
    uint8_t *from;      /* payload of the original */
    uint8_t *to;        /* payload of the copy */
} GCMoved;

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

    _Atomic uint32_t evacuating;  /* GC: copies exist that the back table may hold */
    bool running;                 /* JS: the thread exists */
    bool in_boundary;             /* JS: a boundary step is on the stack */
    bool force_full;              /* JS: run gc_run at the next poll */
    GCThread thread;

    /* Allocation pacing (JS thread). */
    size_t young_bytes;           /* since the last minor start */
    size_t old_bytes;             /* since the last major start */

    /* Work handed between the threads with the request/answer protocol. */
    U32Vec pending_cards;         /* pulled but not yet rescanned */
    GCMoved *moved;
    uint32_t moved_n, moved_cap;
    uint32_t cset_first, cset_count;   /* regions being evacuated (list below) */
    U32Vec cset;

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
static struct { uint32_t key, count; GCHandle example; uint16_t class_id; } g_bkeys[BCHECK_KEYS];

static void bcheck_note(uint32_t type, uint32_t off, GCHandle obj, uint16_t class_id) {
    uint32_t key = (type << 16) | (off < 0xffff ? off : 0xffff);
    uint32_t i = (key * 2654435761u) % BCHECK_KEYS;
    for (uint32_t k = 0; k < BCHECK_KEYS; k++, i = (i + 1) % BCHECK_KEYS) {
        if (g_bkeys[i].count && g_bkeys[i].key == key) { g_bkeys[i].count++; return; }
        if (!g_bkeys[i].count) {
            g_bkeys[i].key = key; g_bkeys[i].count = 1;
            g_bkeys[i].example = obj; g_bkeys[i].class_id = class_id;
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
    if (w == GC_HANDLE_NULL || w >= atomic_load_explicit(&g_gc.high_water, memory_order_acquire)) return false;
    void **f = atomic_load_explicit(&g_gc.front, memory_order_acquire);
    return atomic_load_explicit((_Atomic(void *) *)&f[w], memory_order_acquire) != NULL;
}

/* Page `page` (4 KB) was written with its card clean: report reference
 * words that changed outside new objects, then refresh its shadow. */
static void bcheck_page(uintptr_t page, bool card_clean) {
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
            bool skip = type == JS_GC_OBJ_TYPE_STACK_FRAME || type == JS_GC_OBJ_TYPE_ASYNC_FUNCTION ||
                        type == JS_GC_OBJ_TYPE_JS_STRING || type == JS_GC_OBJ_TYPE_JS_STRING_ROPE ||
                        type == JS_GC_OBJ_TYPE_JS_BIGINT || type == JS_GC_OBJ_TYPE_BYTES ||
                        /* shapes: hash table and property entries hold atoms and indexes;
                         * proto_handle is set only through its helper */
                        type == JS_GC_OBJ_TYPE_SHAPE ||
                        !gc_core_header_is_current(h) || (!same_header && !header_before_page);
            if (!skip) {
                uintptr_t payload = p + GC_HEADER_SIZE;
                uintptr_t a = payload > page ? payload : page;
                uintptr_t b = p + total < end ? p + total : end;
                uint16_t class_id = type == JS_GC_OBJ_TYPE_JS_OBJECT ? *(uint16_t *)payload : 0;
                for (a = (a + 3) & ~(uintptr_t)3; a + 4 <= b; a += 4) {
                    uint32_t cur = *(uint32_t *)a, old = *(uint32_t *)(shadow + (a - page));
                    if (cur == old || !bcheck_is_ref_handle(cur)) continue;
                    /* A GCValue payload (8-aligned) counts only with a reference tag. */
                    if (((a - payload) & 7) == 0 && a + 8 <= b) {
                        uint32_t tag = *(uint32_t *)(a + 4);
                        if (!((unsigned)(int32_t)tag >= (unsigned)JS_TAG_FIRST) && tag != 0) continue;
                    }
                    bcheck_note(type, (uint32_t)(a - payload), h->handle, class_id);
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
    for (;;) {
        uint32_t a = atomic_load_explicit(&g_conc.ack_seq, memory_order_acquire);
        if (a == seq)
            return atomic_load_explicit(&g_conc.ack, memory_order_acquire);
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
    if (kind == GC_CYCLE_MINOR) {
        /* The nursery that exists now is the one this cycle collects; new
         * young objects go to fresh regions. */
        gc_core_alloc_ctx_retire(&g_core.js_young);
        uint32_t hwm = atomic_load(&g_core.region_hwm);
        for (uint32_t i = 0; i < hwm; i++) {
            GCRegion *r = &g_core.regions[i];
            if (atomic_load(&r->state) == GC_RS_NURSERY) r->collecting = true;
        }
        uint64_t *t = g_conc.remset_cur;
        g_conc.remset_cur = g_conc.remset_acc;
        g_conc.remset_acc = t;   /* cleared by the GC thread after the last cycle */
        g_conc.young_bytes = 0;
    } else {
        g_conc.old_bytes = 0;
    }
    uint64_t ta = gc_os_now_ns();
    gc_col_begin_cycle(kind, false);
    g_core.defer_frees = true;
    /* No hole allocation until this cycle's sweep is done (see
     * gc_core.holes_paused); the GC thread has retired gc_copy already. */
    if (kind != GC_CYCLE_MINOR) {
        atomic_store_explicit(&g_core.holes_paused, true, memory_order_release);
        if (g_core.js_old.in_hole) gc_core_alloc_ctx_retire(&g_core.js_old);
    }
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

static inline bool ranges_overlap_dirty(const uint8_t *dirty, uintptr_t a, uintptr_t b) {
    uint32_t c0 = (uint32_t)((a - (uintptr_t)g_gc.heap_base) >> GC_CARD_SHIFT);
    uint32_t c1 = (uint32_t)((b - 1 - (uintptr_t)g_gc.heap_base) >> GC_CARD_SHIFT);
    for (uint32_t c = c0; c <= c1; c++)
        if (dirty[c >> 3] & (1u << (c & 7))) return true;
    return false;
}

#if !GC_SOFTWARE_DIRTY
static uint8_t *g_flip_dirty;        /* card bitmap for the flip (Windows) */
static size_t g_flip_dirty_bytes;
#endif

/* Flip: reject copies that changed since they were made, or whose
 * originals are pinned, then swap the handle tables. */
static uint32_t step_flip(uint64_t t0, uint64_t budget_ns) {
    JSRuntimeHandle rt = g_gc.rt;
    gc_col_pin_running_code(rt);
#if !GC_SOFTWARE_DIRTY
    /* Pages of the evacuated regions written since their objects were
     * copied (the copier reset each region's write watch first). */
    U32Vec cards = { 0 };
    for (uint32_t i = 0; i < g_conc.cset.n; i++) {
        uintptr_t lo = (uintptr_t)gc_core_region_start(&g_core.regions[g_conc.cset.items[i]]);
        dirty_pull_range(lo, lo + GC_REGION_SIZE, &cards);
    }
    size_t need = (g_conc.card_count + 7) / 8;
    if (g_flip_dirty_bytes < need) {
        free(g_flip_dirty);
        g_flip_dirty = (uint8_t *)calloc(need, 1);
        g_flip_dirty_bytes = need;
    }
    for (uint32_t i = 0; i < cards.n; i++)
        g_flip_dirty[cards.items[i] >> 3] |= (uint8_t)(1u << (cards.items[i] & 7));
#endif
    void **front = atomic_load_explicit(&g_gc.front, memory_order_acquire);
    void **back = gc_core_back();
    uint32_t rejected = 0;
    for (uint32_t i = 0; i < g_conc.moved_n; i++) {
        GCMoved *m = &g_conc.moved[i];
        if (front[m->h] != m->from || back[m->h] != m->to) continue;   /* the JS thread moved on */
        GCRegion *src = gc_core_region_of(m->from);
        bool reject = gc_col_is_pinned((const GCHeader *)(m->from - GC_HEADER_SIZE));
#if GC_SOFTWARE_DIRTY
        /* Software flags only cover reference stores: compare the bytes. */
        if (!reject && memcmp(m->from - GC_HEADER_SIZE, m->to - GC_HEADER_SIZE, m->total) != 0)
            reject = true;
#else
        if (!reject && ranges_overlap_dirty(g_flip_dirty, (uintptr_t)m->from - GC_HEADER_SIZE,
                                            (uintptr_t)m->from - GC_HEADER_SIZE + m->total))
            reject = true;
#endif
        if (!reject && g_core.config.reject == GC_REJECT_ALL) reject = true;
        if (!reject && g_core.config.reject == GC_REJECT_HALF && (i & 1)) reject = true;
        if (reject) {
            GC_TRACE(m->h, "copy rejected at flip");
            atomic_store_explicit((_Atomic(void *) *)&back[m->h], m->from, memory_order_release);
            src->keep = true;
            rejected++;
        }
        if ((i & 255) == 255 && over_budget(t0, budget_ns)) {
#if !GC_SOFTWARE_DIRTY
            /* The write-watch state was consumed: keep it for the retry by
             * rejecting everything not yet checked. */
            for (uint32_t k = i + 1; k < g_conc.moved_n; k++) {
                GCMoved *x = &g_conc.moved[k];
                if (front[x->h] == x->from && back[x->h] == x->to &&
                    ranges_overlap_dirty(g_flip_dirty, (uintptr_t)x->from - GC_HEADER_SIZE,
                                         (uintptr_t)x->from - GC_HEADER_SIZE + x->total)) {
                    atomic_store_explicit((_Atomic(void *) *)&back[x->h], x->from, memory_order_release);
                    gc_core_region_of(x->from)->keep = true;
                }
            }
            for (uint32_t k = 0; k < cards.n; k++)
                g_flip_dirty[cards.items[k] >> 3] = 0;
            u32_free(&cards);
#endif
            g_core.stats.copies_rejected += rejected;
            return ACK_DECLINED;
        }
    }
#if !GC_SOFTWARE_DIRTY
    for (uint32_t k = 0; k < cards.n; k++)
        g_flip_dirty[cards.items[k] >> 3] = 0;
    u32_free(&cards);
#endif
    g_core.stats.copies_rejected += rejected;
    atomic_store_explicit(&g_gc.front, back, memory_order_release);
    if (g_core.trace_handle) gc_core_trace(0, "--- flip");
    return ACK_DONE;
}

/* Weak apply: fix the weak containers the GC thread flagged, run the
 * finalizers of condemned objects, close the weak window. */
static void close_weak_window(void) {
    atomic_store_explicit((_Atomic bool *)&g_gc.weak_step_active, false, memory_order_release);
    g_core.defer_frees = false;
    release_deferred_frees();
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
        gc_run();
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
    case REQ_FLIP:
        ack = step_flip(t0, budget);
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
    gc_core_drain_freed_ring();
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
    uint64_t end = gc_os_now_ns() + (uint64_t)timeout_ms * 1000000ull;
    while (!gc_is_idle()) {
        gc_tick();
        if (gc_os_now_ns() > end) return false;
        if (!atomic_load_explicit(&g_gc_poll_flag, memory_order_acquire)) gc_os_yield();
    }
    gc_core_drain_freed_ring();
    return true;
}

/* ============================================================================
 * GC thread: marking
 * ============================================================================ */

/* Clear the mark bitmaps of the parity the next cycle uses. */
static void prepare_bitmaps(int kind) {
    uint32_t next = g_col.parity ^ 1;
    uint32_t hwm = atomic_load(&g_core.region_hwm);
    for (uint32_t i = 0; i < hwm; i++) {
        GCRegion *r = &g_core.regions[i];
        uint32_t st = atomic_load_explicit(&r->state, memory_order_acquire);
        if (!gc_core_region_in_use(st)) continue;
        if (kind == GC_CYCLE_MINOR && st != GC_RS_NURSERY) continue;
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
            /* Cards of the nursery being collected hold no old-to-young
             * references: its live objects are traced anyway. */
            GCRegion *r = gc_core_region_of(g_gc.heap_base + ((uintptr_t)card << GC_CARD_SHIFT));
            if (r && r->collecting) continue;
            u32_push(&g_conc.pending_cards, card);
        }
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

static void sweep_concurrent(int kind) {
    uint32_t hwm = atomic_load(&g_core.region_hwm);
    uint32_t n_released = 0, n_offered = 0, n_kept = 0;
    size_t hole_bytes = 0, live_bytes = 0;
    for (uint32_t i = 0; i < hwm; i++) {
        GCRegion *r = &g_core.regions[i];
        uint32_t st = atomic_load_explicit(&r->state, memory_order_acquire);
        if (!gc_core_region_in_use(st) || r->permanent) continue;
        if (kind == GC_CYCLE_MINOR) {
            if (!r->collecting) continue;
            gc_col_sweep_region(r, atomic_load(&r->top), false);
            continue;
        }
        if (r->collecting) continue;
        /* Hold the region so no context claims it meanwhile.  None is
         * filling its holes (they are paused), but a region a context still
         * owns could only be skipped: its dead objects would stay current
         * after their referents are freed. */
        uint32_t rs = atomic_load_explicit(&r->recycle_state, memory_order_acquire);
        if (rs == GC_RECYCLE_OWNED ||
            !atomic_compare_exchange_strong(&r->recycle_state, &rs, GC_RECYCLE_SWEEPING))
            continue;
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
         * for the recycling allocation contexts, unless the region is about
         * to be evacuated. */
        live_bytes += live;
        size_t hb;
        bool offered = false;
        if (g_core.holes && st == GC_RS_OLD && top == r->tams && r->alloc_owner == GC_OWNER_NONE &&
            !r->pinned && !gc_core_region_evacuates(r, top) && (hb = gc_core_format_holes(r))) {
            hole_bytes += hb;
            n_offered++;
            atomic_store_explicit(&r->recycle_state, GC_RECYCLE_AVAILABLE, memory_order_release);
            gc_core_offer_recyclable(r);
            offered = true;
        }
        if (!offered) {
            atomic_store_explicit(&r->recycle_state, GC_RECYCLE_IDLE, memory_order_release);
            n_kept++;
        }
    }
    if (kind != GC_CYCLE_MINOR && g_core.log_level)
        fprintf(stderr, "[GC]   major sweep: %u regions released, %u offered (%.1f MB in holes), %u kept; %.1f MB live\n",
                n_released, n_offered, hole_bytes / 1048576.0, n_kept, live_bytes / 1048576.0);
    if (kind != GC_CYCLE_MINOR)
        atomic_store_explicit(&g_core.holes_paused, false, memory_order_release);
}

/* ============================================================================
 * GC thread: evacuation
 * ============================================================================ */

static void moved_push(GCHandle h, uint32_t total, uint8_t *from, uint8_t *to) {
    if (g_conc.moved_n == g_conc.moved_cap) {
        uint32_t cap = g_conc.moved_cap ? g_conc.moved_cap * 2 : 4096;
        GCMoved *n = (GCMoved *)realloc(g_conc.moved, cap * sizeof(GCMoved));
        if (!n) {
            fprintf(stderr, "[GC] out of memory (moved list)\n");
            abort();
        }
        g_conc.moved = n;
        g_conc.moved_cap = cap;
    }
    GCMoved *m = &g_conc.moved[g_conc.moved_n++];
    m->h = h;
    m->total = total;
    m->from = from;
    m->to = to;
}

/* Copy one object into the back table.  The JS thread keeps using the
 * original until the flip; a CAS that fails means it changed the entry. */
static void copy_object_concurrent(GCHeader *hdr, void *opaque) {
    (void)opaque;
    if (!gc_core_header_is_current(hdr)) return;
    /* Never moved; finish_flip keeps the region because it is current. */
    if (gc_header_type(hdr) == JS_GC_OBJ_TYPE_BYTES) return;
    size_t total = gc_header_total_size(hdr);
    GCHeader *dst = gc_core_alloc_object_memory(&g_core.gc_copy, total - GC_HEADER_SIZE,
                                                gc_header_type(hdr));
    if (!dst) return;
    memcpy(dst, hdr, total);
    uint8_t *from = (uint8_t *)gc_header_payload(hdr);
    uint8_t *to = (uint8_t *)gc_header_payload(dst);
    gc_dirty_range(to, total - GC_HEADER_SIZE);
    void **back = gc_core_back();
    void *expect = from;
    if (atomic_compare_exchange_strong_explicit((_Atomic(void *) *)&back[dst->handle], &expect, to,
                                                memory_order_acq_rel, memory_order_acquire)) {
        moved_push(dst->handle, (uint32_t)total, from, to);
        GC_TRACE(dst->handle, "copied");
        g_core.stats.objects_copied++;
    }
}

static void copy_region(GCRegion *r, uintptr_t limit) {
    atomic_store_explicit(&g_conc.evacuating, 1, memory_order_release);
#if !GC_SOFTWARE_DIRTY
    /* Reset the region's write watch before copying: a write after this
     * point shows at the flip and rejects the copy. */
    uintptr_t lo = (uintptr_t)gc_core_region_start(r);
    dirty_pull_range(lo, lo + GC_REGION_SIZE, NULL);
#endif
    uintptr_t start = (uintptr_t)gc_core_region_start(r) + GC_HEADER_SIZE;
    gc_core_walk_region(r, start, limit, copy_object_concurrent, NULL);
}

/* Undo the back-table entries of an abandoned evacuation. */
static void unwind_moves(void) {
    void **back = gc_core_back();
    for (uint32_t i = 0; i < g_conc.moved_n; i++) {
        GCMoved *m = &g_conc.moved[i];
        void *expect = m->to;
        atomic_compare_exchange_strong((_Atomic(void *) *)&back[m->h], &expect, (void *)m->from);
    }
    g_conc.moved_n = 0;
    for (uint32_t i = 0; i < g_conc.cset.n; i++) {
        g_core.regions[g_conc.cset.items[i]].keep = false;
        g_core.regions[g_conc.cset.items[i]].in_cset = false;
    }
    g_conc.cset.n = 0;
    atomic_store_explicit(&g_conc.evacuating, 0, memory_order_release);
}

static void any_current_cb(GCHeader *hdr, void *opaque) {
    if (gc_core_header_is_current(hdr)) *(bool *)opaque = true;
}

/* After a flip: bring the new back table up to date, then free the
 * regions everything left. */
static void finish_flip(int kind) {
    void **front = atomic_load_explicit(&g_gc.front, memory_order_acquire);
    void **back = gc_core_back();
    for (uint32_t i = 0; i < g_conc.moved_n; i++) {
        GCMoved *m = &g_conc.moved[i];
        if (front[m->h] != m->to) continue;   /* rejected, or changed by the JS thread */
        void *expect = m->from;
        atomic_compare_exchange_strong((_Atomic(void *) *)&back[m->h], &expect, (void *)m->to);
        if (kind == GC_CYCLE_MINOR) g_core.stats.bytes_promoted += m->total;
    }
    g_conc.moved_n = 0;
    for (uint32_t i = 0; i < g_conc.cset.n; i++) {
        GCRegion *r = &g_core.regions[g_conc.cset.items[i]];
        bool live = r->keep;
        if (!live) {
            /* Belt and braces: anything still current stays. */
            uintptr_t start = (uintptr_t)gc_core_region_start(r) + GC_HEADER_SIZE;
            gc_core_walk_region(r, start, atomic_load(&r->top), any_current_cb, &live);
        }
        if (live) {
            if (kind == GC_CYCLE_MINOR) {
                /* Promoted in place. */
                r->space = GC_RS_OLD;
                atomic_store_explicit(&r->state, GC_RS_OLD, memory_order_release);
                g_core.stats.regions_promoted_in_place++;
            }
            r->keep = false;
            r->in_cset = false;
            r->collecting = false;
            continue;
        }
        gc_core_region_release(r);
    }
    g_conc.cset.n = 0;
    atomic_store_explicit(&g_conc.evacuating, 0, memory_order_release);
}

/* Flip, retrying with a larger budget after each decline. */
static bool flip(void) {
    uint64_t budget = (uint64_t)g_core.config.boundary_budget_us * 1000ull;
    for (int attempt = 0; ; attempt++) {
        uint64_t b = attempt >= 8 ? 0 : budget << attempt;
        uint32_t ack = post_request(REQ_FLIP, b);
        if (ack == ACK_DONE) return true;
        if (ack == ACK_ABORTED || aborted()) return false;
    }
}

/* Largest live size one flip validates (the JS thread compares copies with
 * their originals on platforms without OS dirty tracking). */
#if GC_SOFTWARE_DIRTY
#define EVAC_BATCH_BYTES ((size_t)512 << 10)
#else
#define EVAC_BATCH_BYTES ((size_t)16 << 20)
#endif

/* Major cycle: evacuate sparse old regions, one batch per flip. */
static bool evacuate_major(void) {
    uint32_t hwm = atomic_load(&g_core.region_hwm);
    uint32_t next = 0;
    while (next < hwm) {
        size_t batch = 0;
        g_conc.cset.n = 0;
        for (; next < hwm && batch < EVAC_BATCH_BYTES; next++) {
            GCRegion *r = &g_core.regions[next];
            if (atomic_load_explicit(&r->state, memory_order_acquire) != GC_RS_OLD) continue;
            if (r->alloc_owner != GC_OWNER_NONE || r->pinned || r->permanent) continue;
            uintptr_t top = atomic_load_explicit(&r->top, memory_order_acquire);
            if (top != r->tams) continue;   /* allocated in during the cycle */
            if (!gc_core_region_evacuates(r, top) && g_core.config.stress < 2) continue;
            if (r->live_bytes == 0) continue;
            /* Its holes were offered: a context may be filling them. */
            if (atomic_load_explicit(&r->recycle_state, memory_order_acquire) != GC_RECYCLE_IDLE) continue;
            r->in_cset = true;
            u32_push(&g_conc.cset, next);
            batch += r->live_bytes;
        }
        if (!g_conc.cset.n) break;
        for (uint32_t i = 0; i < g_conc.cset.n; i++) {
            GCRegion *r = &g_core.regions[g_conc.cset.items[i]];
            copy_region(r, r->tams);
            if (aborted()) {
                unwind_moves();
                return false;
            }
        }
        /* The copy region stays the GC thread's across batches and cycles
         * (its top is published with every allocation). */
        if (!flip()) {
            unwind_moves();
            return false;
        }
        finish_flip(GC_CYCLE_MAJOR);
    }
    return true;
}

/* Minor cycle: promote every survivor of the collected nursery.  Sparse
 * regions are copied out (compacting them); dense ones, and everything past
 * one flip's validation budget, are promoted in place. */
static bool evacuate_minor(void) {
    uint32_t hwm = atomic_load(&g_core.region_hwm);
    size_t copied = 0;
    g_conc.cset.n = 0;
    for (uint32_t i = 0; i < hwm; i++) {
        GCRegion *r = &g_core.regions[i];
        if (!r->collecting || atomic_load(&r->state) != GC_RS_NURSERY) continue;
        u32_push(&g_conc.cset, i);
        size_t used = atomic_load(&r->top) - (uintptr_t)gc_core_region_start(r);
        bool dense = r->live_bytes * 2 > used && g_core.config.stress < 2;
        if (dense || copied + r->live_bytes > EVAC_BATCH_BYTES) {
            r->keep = true;
            continue;
        }
        copied += r->live_bytes;
        if (r->live_bytes) copy_region(r, atomic_load(&r->top));
        if (aborted()) {
            unwind_moves();
            return false;
        }
    }
    if (g_conc.moved_n && !flip()) {
        unwind_moves();
        return false;
    }
    finish_flip(GC_CYCLE_MINOR);
    return true;
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

/* The remembered set a minor cycle used: cleared when the cycle completed
 * (every young object it covered was promoted), merged back otherwise. */
static void retire_remset(bool completed) {
    uint32_t used = (uint32_t)((heap_used_end() - (uintptr_t)g_gc.heap_base) >> GC_CARD_SHIFT);
    uint32_t words = (used + 63) / 64;
    for (uint32_t w = 0; w < words; w++) {
        if (!completed) g_conc.remset_acc[w] |= g_conc.remset_cur[w];
        g_conc.remset_cur[w] = 0;
    }
}

static void end_concurrent_cycle(void) {
    /* An abandoned major cycle never reaches its sweep. */
    atomic_store_explicit(&g_core.holes_paused, false, memory_order_release);
    gc_col_end_cycle();
    g_conc.pending_cards.n = 0;
}

static size_t g_promoted_since_major;   /* GC thread */

#define GC_LOG(...) do { if (g_core.log_level) fprintf(stderr, __VA_ARGS__); } while (0)

static void run_cycle(int kind) {
    JSRuntimeHandle rt = g_gc.rt;
    bool marked = false;
    uint64_t t_start = gc_os_now_ns(), t_mark, t_remark, t_sweep;
    uint64_t cards0 = g_core.stats.dirty_cards_rescanned;
    g_next_kind = (uint32_t)kind;
    prepare_bitmaps(kind);
    /* A major cycle sweeps every region: give up the holes the copier is
     * filling (the JS thread does the same for its own at mark start). */
    if (kind != GC_CYCLE_MINOR && g_core.gc_copy.in_hole) gc_core_alloc_ctx_retire(&g_core.gc_copy);
    /* Discard the writes made so far (the tracer reads the objects as they
     * are now), recording them in the remembered set.  Writes between this
     * pull and mark start are pulled, and rescanned, during the cycle. */
    dirty_pull_all(NULL);
    if (post_request(REQ_MARK_START, 0) != ACK_DONE) goto abandon;
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
        uint64_t before = g_core.stats.bytes_promoted;
        if (!evacuate_minor()) goto finish;
        g_promoted_since_major += g_core.stats.bytes_promoted - before;
        g_core.stats.minor_cycles++;
    } else {
        if (!evacuate_major()) goto finish;
        g_promoted_since_major = 0;
        g_core.stats.major_cycles++;
    }
finish:
    if (kind == GC_CYCLE_MINOR) retire_remset(!aborted());
    end_concurrent_cycle();
    GC_LOG("[GC] %s cycle: mark %.2f ms, remark %.2f ms, sweep %.2f ms, evacuate %.2f ms; "
           "cards %llu, heap %u regions\n",
           kind == GC_CYCLE_MINOR ? "minor" : "major",
           (t_mark - t_start) / 1e6, (t_remark - t_mark) / 1e6, (t_sweep - t_remark) / 1e6,
           (gc_os_now_ns() - t_sweep) / 1e6,
           (unsigned long long)(g_core.stats.dirty_cards_rescanned - cards0),
           atomic_load(&g_core.regions_in_use));
    return;
abandon:
    /* Marking was not finished: nothing has been freed or moved.  The JS
     * thread recycles deferred frees when it sees the abort. */
    (void)marked;
    if (kind == GC_CYCLE_MINOR) retire_remset(false);
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
    gc_core_drain_freed_ring();
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
    GCRingBlock *b = g_freed_ring.head;
    while (b) {
        GCRingBlock *n = atomic_load(&b->next);
        free(b);
        b = n;
    }
    g_freed_ring.head = g_freed_ring.tail = NULL;
    free(g_col.work.items);
    free(g_col.pins);
    memset(&g_col, 0, sizeof(g_col));
    u32_free(&g_deferred_frees);
    u32_free(&g_ref_log);
    u32_free(&g_conc.pending_cards);
    for (int k = 0; k < 3; k++) u32_free(&g_conc.weak_dead[k]);
    u32_free(&g_conc.cset);
    free(g_conc.moved);
    free(g_conc.remset_acc);
    free(g_conc.remset_cur);
#if !GC_SOFTWARE_DIRTY
    free(g_flip_dirty);
    g_flip_dirty = NULL;
    g_flip_dirty_bytes = 0;
#endif
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

static void verify_cb(JSRuntimeHandle rt, GCHandle h) {
    (void)rt;
    if (!gc_col_handle_header(h)) return;
    uint64_t m = (uint64_t)1 << (h & 63);
    if (v_bits[h >> 6] & m) return;
    v_bits[h >> 6] |= m;
    if (v_n == v_cap) {
        v_cap = v_cap ? v_cap * 2 : 4096;
        v_stack = (GCHandle *)realloc(v_stack, v_cap * 2 * sizeof(GCHandle));
    }
    v_stack[2 * v_n] = h;
    v_stack[2 * v_n + 1] = v_parent;
    v_n++;
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
    while (v_n > 0) {
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
                            "in cycle %d, marked %d; child region %u %s; holes paused %d\n",
                            g_col.kind == GC_CYCLE_MINOR ? "minor" : g_col.kind == GC_CYCLE_MAJOR ? "major" : "full",
                            (unsigned)(pr - g_core.regions), atomic_load(&pr->recycle_state), pr->alloc_owner,
                            (uintptr_t)ph < pr->tams ? "below" : "above",
                            gc_col_in_cycle(pr, ph), gc_col_is_marked(pr, ph),
                            (unsigned)(r - g_core.regions), r->collecting ? "collecting" : r->space == GC_RS_NURSERY ? "nursery" : "old",
                            (int)atomic_load(&g_core.holes_paused));
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
        v_parent = h;
        mark_children(rt, h, verify_cb);
    }
    v_parent = GC_HANDLE_NULL;
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
    /* The back table holds the same address, or (while an evacuation is in
     * flight) the address of a copy of this object. */
    if (gc_core_header_is_current(hdr)) {
        void *b = atomic_load_explicit((_Atomic(void *) *)&gc_core_back()[hdr->handle], memory_order_acquire);
        if (b != gc_header_payload(hdr)) {
            GCHeader *bh = b ? gc_core_object_containing(b) : NULL;
            if (!bh || gc_header_payload(bh) != b || bh->handle != hdr->handle) g_check_problems++;
        }
    }
}

/* Returns the number of inconsistencies found: object headers that are
 * misaligned or not in the start bitmap, current objects whose two
 * handle-table entries disagree, and handle entries that point outside an
 * object.  Call when the collector is idle. */
uint32_t gc_test_heap_check(void) {
    g_check_problems = 0;
    uint32_t hwm = atomic_load(&g_core.region_hwm);
    for (uint32_t i = 0; i < hwm; i++) {
        GCRegion *r = &g_core.regions[i];
        uint32_t st = atomic_load(&r->state);
        if (!gc_core_region_in_use(st)) continue;
        gc_core_walk_region(r, (uintptr_t)gc_core_region_start(r) + GC_HEADER_SIZE,
                            atomic_load(&r->top), check_object, r);
    }
    uint32_t limit = gc_handle_limit();
    void **f = atomic_load(&g_gc.front);
    for (uint32_t h = 1; h < limit; h++) {
        void *p = f[h];
        if (!p) continue;
        GCHeader *hdr = gc_core_object_containing(p);
        if (!hdr || gc_header_payload(hdr) != p || hdr->handle != h) g_check_problems++;
    }
    return g_check_problems;
}

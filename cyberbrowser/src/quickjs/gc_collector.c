/*
 * The collector: marking, roots, dirty-card rescans, weak processing,
 * sweeping and the synchronous full collection.  Design: CONCURRENT_GC.md.
 *
 * This file holds what every kind of collection shares:
 *   - the mark worklist and the rule for "is this object part of the cycle",
 *   - root enumeration, including the conservative scan of the JS thread's
 *     C stack and the pins it produces,
 *   - the conservative rescan of a dirty card,
 *   - the weak step (engine weak tables, finalizers),
 *   - sweeping,
 *   - the stop-the-world full collection behind gc_run().
 * The GC thread, boundary steps, concurrent evacuation and the nursery are
 * in gc_concurrent.c.
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

_Atomic uint32_t g_gc_poll_flag;
GCCollector g_col;

/* ============================================================================
 * Configuration and statistics
 * ============================================================================ */

void gc_core_config_defaults(void) {
    GCConfig *c = &g_core.config;
    c->background = true;
    c->nursery = true;
    c->boundary_budget_us = 500;
    c->nursery_size = 4u << 20;
    c->major_trigger = 32u << 20;
    c->verify = getenv("QJS_GC_VERIFY") != NULL;
    c->stress = 0;
    const char *bg = getenv("QJS_GC_BACKGROUND");
    if (bg) c->background = atoi(bg) != 0;
    const char *ns = getenv("QJS_GC_NURSERY");
    if (ns) c->nursery = atoi(ns) != 0;
    const char *st = getenv("QJS_GC_STRESS");
    if (st) c->stress = (uint32_t)atoi(st);
    const char *bu = getenv("QJS_GC_BUDGET_US");
    if (bu) c->boundary_budget_us = (uint32_t)atoi(bu);
    const char *nz = getenv("QJS_GC_NURSERY_KB");
    if (nz) c->nursery_size = (size_t)atoi(nz) << 10;
    const char *mt = getenv("QJS_GC_MAJOR_KB");
    if (mt) c->major_trigger = (size_t)atoi(mt) << 10;
    const char *rj = getenv("QJS_GC_REJECT");
    c->reject = GC_REJECT_NORMAL;
    if (rj && !strcmp(rj, "all")) c->reject = GC_REJECT_ALL;
    if (rj && !strcmp(rj, "half")) c->reject = GC_REJECT_HALF;
}

void gc_get_config(GCConfig *cfg) { *cfg = g_core.config; }

void gc_set_config(const GCConfig *cfg) {
    gc_col_finish_concurrent_cycle(0);
    bool was_bg = g_core.config.background;
    g_core.config = *cfg;
    g_core.nursery_enabled = cfg->background && cfg->nursery;
    if (!was_bg && cfg->background) gc_core_start_thread();
    if (was_bg && !cfg->background) gc_core_stop_thread();
}

void gc_get_stats(GCStats *s) {
    *s = g_core.stats;
    s->regions_in_use = atomic_load(&g_core.regions_in_use);
    s->heap_committed = (size_t)atomic_load(&g_core.regions_committed) * GC_REGION_SIZE;
    size_t used = 0;
    uint32_t hwm = atomic_load(&g_core.region_hwm);
    for (uint32_t i = 0; i < hwm; i++) {
        GCRegion *r = &g_core.regions[i];
        uint32_t st = atomic_load(&r->state);
        if (st == GC_RS_HUMONGOUS_CONT) { used += GC_REGION_SIZE; continue; }
        if (!gc_core_region_in_use(st)) continue;
        used += atomic_load(&r->top) - (uintptr_t)gc_core_region_start(r);
    }
    s->heap_used = used;
    s->handles_in_use = gc_handle_limit() - 1 - g_core.hcache_n;
}

/* ============================================================================
 * Worklist (owned by whichever thread runs the current phase)
 * ============================================================================ */

static void worklist_push(GCWorklist *w, GCHandle h) {
    if (w->n == w->cap) {
        uint32_t cap = w->cap ? w->cap * 2 : 4096;
        GCHandle *n = (GCHandle *)realloc(w->items, cap * sizeof(GCHandle));
        if (!n) {
            fprintf(stderr, "[GC] out of memory growing the mark worklist\n");
            abort();
        }
        w->items = n;
        w->cap = cap;
    }
    w->items[w->n++] = h;
}

/* ============================================================================
 * Cycle membership and marking
 * ============================================================================ */

/* True if the collection in progress decides this object's fate.  Objects
 * outside the cycle are live by definition: allocated during the cycle
 * (above TAMS, or in a hole taken since mark start).  In a minor cycle the old objects are in it too, but they
 * are marked already (sticky mark bits), so only young ones get traced or
 * freed. */
static inline bool in_cycle(const GCRegion *r, const GCHeader *hdr) {
    if (r->permanent) return false;   /* atoms */
    return g_col.kind != GC_CYCLE_NONE && (uintptr_t)hdr < r->tams && !gc_core_in_new_line(r, hdr);
}

static inline bool is_marked(const GCRegion *r, const GCHeader *hdr) {
    uint32_t g = gc_core_granule_of(r, hdr);
    const uint64_t *bits = r->mark_bits[g_col.parity];
    return (atomic_load_explicit((_Atomic uint64_t *)&bits[g >> 6], memory_order_acquire) >> (g & 63)) & 1;
}

/* Set the mark bit; true if it was clear. */
static inline bool set_mark(GCRegion *r, const GCHeader *hdr) {
    uint32_t g = gc_core_granule_of(r, hdr);
    _Atomic uint64_t *w = (_Atomic uint64_t *)&r->mark_bits[g_col.parity][g >> 6];
    uint64_t m = (uint64_t)1 << (g & 63);
    if (atomic_load_explicit(w, memory_order_relaxed) & m) return false;
    return !(atomic_fetch_or_explicit(w, m, memory_order_acq_rel) & m);
}

/* Region holding a header: humongous objects start in their head region. */
static inline GCRegion *header_region(const GCHeader *hdr) {
    return gc_core_region_of(hdr);
}

/* Read a handle's address from the front table, with bounds checks that
 * hold even if the handle came from a torn, stale or conservative read. */
static inline GCHeader *handle_header(GCHandle h) {
    if (h == GC_HANDLE_NULL || h >= atomic_load_explicit(&g_gc.high_water, memory_order_acquire))
        return NULL;
    void **f = atomic_load_explicit(&g_gc.front, memory_order_acquire);
    void *p = atomic_load_explicit((_Atomic(void *) *)&f[h], memory_order_acquire);
    if (!p || ((uintptr_t)p & 15)) return NULL;
    GCHeader *hdr = gc_header(p);
    if (!gc_core_region_of(hdr)) return NULL;
    return hdr;
}

/* True if the object is unreachable according to the cycle that just
 * finished marking. */
static bool header_condemned(const GCHeader *hdr) {
    GCRegion *r = header_region(hdr);
    if (!r) return false;
    if (!in_cycle(r, hdr)) return false;
    return !is_marked(r, hdr);
}

bool gc_object_is_dead(GCHandle handle) {
    if (!g_gc.weak_step_active) return false;
    if (handle == GC_HANDLE_NULL) return true;
    GCHeader *hdr = handle_header(handle);
    if (!hdr) return true;
    return header_condemned(hdr);
}

/* Mark callback handed to the engine's mark_children(). */
static void mark_cb(JSRuntimeHandle rt, GCHandle h) {
    (void)rt;
    GCHeader *hdr = handle_header(h);
    if (!hdr) return;
    GCRegion *r = header_region(hdr);
    if (!in_cycle(r, hdr)) return;
    if (set_mark(r, hdr)) {
        GC_TRACE(h, "marked");
        if (g_col.kind == GC_CYCLE_MINOR)
            atomic_fetch_add_explicit(&g_col.promoted, gc_header_total_size(hdr), memory_order_relaxed);
        worklist_push(&g_col.work, h);
    }
}

typedef struct TraceEvent {
    uint64_t seq;
    const char *what;
    GCHandle h;
    int16_t kind, type;
    uint8_t gc_thread, young, collecting, marked;
    void *addr;
} TraceEvent;
#define TRACE_RING (1u << 22)
static TraceEvent *g_trace;
static _Atomic uint64_t g_trace_seq;

void gc_core_trace(GCHandle h, const char *what) {
    if (!g_trace) {
        g_trace = (TraceEvent *)calloc(TRACE_RING, sizeof(TraceEvent));
        if (!g_trace) return;
    }
    uint64_t seq = atomic_fetch_add(&g_trace_seq, 1);
    TraceEvent *e = &g_trace[seq & (TRACE_RING - 1)];
    void *p = gc_deref(h);
    GCHeader *hdr = p ? gc_header(p) : NULL;
    GCRegion *r = hdr ? gc_core_region_of(hdr) : NULL;
    e->h = h;
    e->what = what;
    e->kind = (int16_t)g_col.kind;
    e->type = hdr ? (int16_t)gc_header_type(hdr) : -1;
    e->gc_thread = gc_os_thread_id() != g_core.js_thread_id;
    e->young = r && hdr && !is_marked(r, hdr);
    e->collecting = r && r->collecting;
    e->marked = r && hdr && in_cycle(r, hdr) ? (is_marked(r, hdr) ? 2 : 1) : 0;
    e->addr = p;
    atomic_store_explicit((_Atomic uint64_t *)&e->seq, seq + 1, memory_order_release);
}

void gc_core_trace_dump(GCHandle h) {
    static const char *kinds[] = { "none", "full", "major", "minor" };
    if (!g_trace) return;
    uint64_t end = atomic_load(&g_trace_seq);
    uint64_t start = end > TRACE_RING ? end - TRACE_RING : 0;
    for (uint64_t s = start; s < end; s++) {
        TraceEvent *e = &g_trace[s & (TRACE_RING - 1)];
        if (e->seq != s + 1) continue;
        if (e->h != h && !(e->h == 0 && e->what)) continue;
        fprintf(stderr, "  [%llu] %u %-34s cycle %-5s %s type %d %s%s%s %p\n", (unsigned long long)s, e->h,
                e->what, kinds[e->kind & 3], e->gc_thread ? "GC" : "JS", e->type,
                e->young ? "young" : "old", e->collecting ? " collecting" : "",
                e->marked == 2 ? " marked" : e->marked == 1 ? " unmarked" : "", e->addr);
    }
}

/* Scan an object's payload conservatively: every 32-bit word that is a
 * handle marks it. */
static void scan_words(JSRuntimeHandle rt, const GCHeader *hdr) {
    const uint8_t *p = (const uint8_t *)hdr + GC_HEADER_SIZE;
    const uint8_t *end = (const uint8_t *)hdr + gc_header_total_size(hdr);
    uint32_t limit = atomic_load_explicit(&g_gc.high_water, memory_order_acquire);
    for (; p + 4 <= end; p += 4) {
        uint32_t w = atomic_load_explicit((_Atomic uint32_t *)p, memory_order_relaxed);
        if (w != GC_HANDLE_NULL && w < limit)
            mark_cb(rt, (GCHandle)w);
    }
}

/* Trace until the worklist is empty, or until `limit` objects were traced
 * (0 = no limit).  Returns true if the worklist is empty.
 *
 * DATA objects are buffers whose contents only their owner's mark function
 * understands.  A minor cycle does not trace old owners, so a young buffer
 * held by an old object (an old object's young property array, say) is
 * scanned conservatively instead. */
static bool drain_worklist(JSRuntimeHandle rt, uint32_t limit) {
    uint32_t n = 0;
    while (g_col.work.n > 0) {
        if (limit && n++ >= limit) return false;
        GCHandle h = g_col.work.items[--g_col.work.n];
        GCHeader *hdr = handle_header(h);
        if (!hdr) continue;
        JSGCObjectTypeEnum t = gc_header_type(hdr);
        if (g_col.kind == GC_CYCLE_MINOR && (t == JS_GC_OBJ_TYPE_DATA || t == JS_GC_OBJ_TYPE_AUTO))
            scan_words(rt, hdr);
        else
            mark_children(rt, h, mark_cb);
    }
    return true;
}

/* ============================================================================
 * Dirty-card rescan
 * ============================================================================ */

/* Types whose payload holds no references. */
static inline bool type_is_leaf(JSGCObjectTypeEnum t) {
    return t == JS_GC_OBJ_TYPE_JS_STRING || t == JS_GC_OBJ_TYPE_JS_BIGINT ||
           t == JS_GC_OBJ_TYPE_BYTES;
}

/* A card was written since it was last looked at.  Every live object on it
 * may have gained references the marker has not seen.  The words of those
 * objects that lie on the card are scanned conservatively: a 32-bit word
 * equal to a handle marks that handle.  This needs no knowledge of object
 * layouts (references may sit in engine structs, value arrays or opaque
 * browser data) and works the same for OS dirty pages and software cards;
 * the price is that an integer that looks like a handle can keep an object
 * alive until the next cycle.
 *
 * Only live objects matter: marked ones, and ones outside the cycle.  An
 * unmarked object in the cycle is traced in full if it is ever marked. */
void gc_col_rescan_card(JSRuntimeHandle rt, uint32_t card) {
    uintptr_t lo = (uintptr_t)g_gc.heap_base + ((uintptr_t)card << GC_CARD_SHIFT);
    uintptr_t hi = lo + GC_CARD_SIZE;
    GCRegion *r = gc_core_region_of((void *)lo);
    if (!r) return;
    uint32_t st = atomic_load_explicit(&r->state, memory_order_acquire);
    if (st == GC_RS_HUMONGOUS_CONT) {
        r = &g_core.regions[r->humongous_head];
        st = atomic_load_explicit(&r->state, memory_order_acquire);
    }
    if (!gc_core_region_in_use(st)) return;
    uintptr_t top = atomic_load_explicit(&r->top, memory_order_acquire);
    if (hi > top) hi = top;
    if (lo >= hi) return;

    /* First object overlapping the card. */
    GCHeader *hdr = gc_core_object_containing((const void *)lo);
    uintptr_t p = hdr ? (uintptr_t)hdr : gc_core_next_header(r, lo, hi);
    while (p < hi) {
        GCHeader *h = (GCHeader *)p;
        size_t total = gc_header_total_size(h);
        if (!total) { p = gc_core_skip_free(r, p, hi); continue; }
        uintptr_t end = p + total;
        if (!type_is_leaf(gc_header_type(h)) && gc_core_header_is_current(h) &&
            (!in_cycle(r, h) || is_marked(r, h))) {
            uintptr_t a = p + GC_HEADER_SIZE;
            if (a < lo) a = lo;
            uintptr_t b = end < hi ? end : hi;
            uint32_t limit = atomic_load_explicit(&g_gc.high_water, memory_order_acquire);
            for (a = (a + 3) & ~(uintptr_t)3; a + 4 <= b; a += 4) {
                uint32_t w = atomic_load_explicit((_Atomic uint32_t *)a, memory_order_relaxed);
                if (w != GC_HANDLE_NULL && w < limit)
                    mark_cb(rt, (GCHandle)w);
            }
        }
        p = end;
    }
}

/* ============================================================================
 * Roots
 * ============================================================================ */

/* Stack range of the JS thread, captured by the boundary step or gc_run
 * that is on the stack right now. */
static inline void capture_stack_range(uintptr_t lo) {
    g_col.stack_lo = lo;
    g_col.stack_hi = gc_os_stack_base();
}

static int compare_uintptr(const void *a, const void *b) {
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return x < y ? -1 : x > y;
}

/* Pin an object: its region (stop-the-world compaction works by region)
 * and the object itself (the concurrent flip rejects just that object). */
static void pin_header(GCHeader *hdr) {
    GCRegion *r = header_region(hdr);
    if (r) r->pinned = true;
    if (g_col.pins_n == g_col.pins_cap) {
        uint32_t cap = g_col.pins_cap ? g_col.pins_cap * 2 : 1024;
        uintptr_t *n = (uintptr_t *)realloc(g_col.pins, cap * sizeof(uintptr_t));
        if (!n) return;   /* the region pin still holds */
        g_col.pins = n;
        g_col.pins_cap = cap;
    }
    g_col.pins[g_col.pins_n++] = (uintptr_t)hdr;
}

bool gc_col_is_pinned(const GCHeader *hdr) {
    uint32_t lo = 0, hi = g_col.pins_n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_col.pins[mid] < (uintptr_t)hdr) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_col.pins_n && g_col.pins[lo] == (uintptr_t)hdr;
}

/* Conservative scan of the JS thread's stack and spilled registers.  A
 * 32-bit word equal to a live handle, or a pointer into an object, keeps
 * the object alive.  Pointers also pin the object: a raw pointer is about
 * to be used, so the object must not move this cycle. */
static void scan_stack(JSRuntimeHandle rt, bool pin, JS_MarkFunc *cb) {
    uintptr_t lo = g_col.stack_lo & ~(uintptr_t)3;
    uintptr_t hi = g_col.stack_hi;
    if (!lo || hi <= lo || hi - lo > ((size_t)512 << 20)) return;
    uint32_t hwm_handles = atomic_load_explicit(&g_gc.high_water, memory_order_acquire);
    uintptr_t heap_lo = (uintptr_t)g_gc.heap_base;
    uintptr_t heap_hi = heap_lo + ((uintptr_t)atomic_load(&g_core.region_hwm) << GC_REGION_SHIFT);

    for (uintptr_t a = lo; a + sizeof(uint32_t) <= hi; a += sizeof(uint32_t)) {
        uint32_t w = *(const uint32_t *)a;
        if (w != GC_HANDLE_NULL && w < hwm_handles)
            cb(rt, (GCHandle)w);
    }
    for (uintptr_t a = (lo + 7) & ~(uintptr_t)7; a + sizeof(uintptr_t) <= hi; a += sizeof(uintptr_t)) {
        uintptr_t w = *(const uintptr_t *)a;
        if (w < heap_lo || w >= heap_hi) continue;
        /* Check the address and the byte before it: a pointer one past the
         * end of an object belongs to that object. */
        for (int d = 0; d < 2; d++) {
            GCHeader *hdr = gc_core_object_containing((const void *)(w - (uintptr_t)d));
            if (!hdr || !gc_core_header_is_current(hdr)) continue;
            cb(rt, hdr->handle);
            if (pin) pin_header(hdr);
        }
    }
}

static void pin_handle_cb(JSRuntimeHandle rt, GCHandle h) {
    (void)rt;
    GCHeader *hdr = handle_header(h);
    if (hdr) pin_header(hdr);
}

static void ignore_cb(JSRuntimeHandle rt, GCHandle h) { (void)rt; (void)h; }

/* Every root of the runtime.  With pin set, objects that running code
 * reaches through raw pointers are pinned.  JS thread only: the roots live
 * in structures the JS thread changes without dirty flags. */
static void enumerate_roots(JSRuntimeHandle rt, bool pin, JS_MarkFunc *cb) {
    /* Atoms are permanent (the engine refers to them by index from places
     * the collector cannot see) and live in permanent regions, which no
     * cycle includes: marking does not need to visit them.  The verifier
     * still does. */
    if (cb != mark_cb) {
        for (uint32_t i = 0; i < g_gc.atom_handles.count; i++) {
            GCHandle h = g_gc.atom_handles.handles[i];
            if (gc_handle_array_entry_is_valid(h)) cb(rt, h);
        }
    }
    for (uint32_t i = 0; i < g_core.contexts.count; i++) {
        cb(rt, g_core.contexts.items[i].handle);
        if (rt != GC_HANDLE_NULL)
            JS_GCMarkContextRoots(rt, g_core.contexts.items[i].handle, cb);
    }
    for (uint32_t i = 0; i < g_core.roots.count; i++)
        cb(rt, g_core.roots.items[i].handle);
    if (rt != GC_HANDLE_NULL) {
        cb(rt, rt);
        JS_GCEnumerateEngineRoots(rt, cb);
        JS_MarkActiveStackFrames(rt, cb);
        if (pin) JS_GCForEachActiveFrameObject(rt, pin_handle_cb);
    }
    scan_stack(rt, pin, cb);
}

/* Pin what running code holds raw pointers to (stack, active frames). */
static void pin_running_code(JSRuntimeHandle rt) {
    uint32_t hwm = atomic_load(&g_core.region_hwm);
    for (uint32_t i = 0; i < hwm; i++)
        g_core.regions[i].pinned = false;
    g_col.pins_n = 0;
    if (rt != GC_HANDLE_NULL) JS_GCForEachActiveFrameObject(rt, pin_handle_cb);
    scan_stack(rt, true, ignore_cb);
    qsort(g_col.pins, g_col.pins_n, sizeof(uintptr_t), compare_uintptr);
}

/* ============================================================================
 * Weak step and sweeping
 * ============================================================================ */

/* Drop weak references to objects this cycle condemned and run their
 * finalizers.  Runs on the JS thread; the engine's weak tables belong to
 * it. */
/* Run the finalizers of condemned objects and drop dead contexts (JS
 * thread, while gc_object_is_dead answers for the finished cycle). */
void gc_col_finalize_dead(JSRuntimeHandle rt, bool finalizers) {
    uint32_t j = 0;
    if (!finalizers) goto contexts;
    for (uint32_t i = 0; i < g_core.finalizers.count; i++) {
        GCRegistryEntry e = g_core.finalizers.items[i];
        if (gc_object_is_dead(e.handle)) {
            void *p = gc_deref(e.handle);
            if (e.fn && p) e.fn(rt, e.handle, p);
            continue;
        }
        g_core.finalizers.items[j++] = e;
    }
    g_core.finalizers.count = j;

contexts:
    j = 0;
    for (uint32_t i = 0; i < g_core.contexts.count; i++) {
        GCRegistryEntry e = g_core.contexts.items[i];
        if (gc_object_is_dead(e.handle) || !gc_deref(e.handle)) continue;
        g_core.contexts.items[j++] = e;
    }
    g_core.contexts.count = j;
}

static void weak_step(JSRuntimeHandle rt) {
    g_gc.weak_step_active = true;
    if (rt != GC_HANDLE_NULL) JS_GCWeakStep(rt);
    gc_col_finalize_dead(rt, true);
    g_gc.weak_step_active = false;
}

/* Free a condemned object's handle.  The CAS makes this safe against the
 * JS thread freeing it too (gc_free) or having moved on from it. */
static void free_dead_handle(GCHeader *hdr, bool now) {
    GCHandle h = hdr->handle;
    if (!gc_core_clear_handle(h, gc_header_payload(hdr))) return;
    GC_TRACE(h, now ? "freed by stop-the-world sweep" : "freed by concurrent sweep");
    if (g_handle_history) gc_core_note_freed(h, now ? GC_FREED_STW_SWEEP : GC_FREED_CONCURRENT_SWEEP);
    if (now) gc_core_hcache_push(h);
    else gc_col_freed_ring_push(h);
    g_core.stats.objects_freed++;
}

typedef struct SweepState {
    size_t live;
    bool now;
} SweepState;

static void sweep_object(GCHeader *hdr, void *opaque) {
    SweepState *s = (SweepState *)opaque;
    if (!gc_core_header_is_current(hdr)) return;
    if (header_condemned(hdr)) {
        free_dead_handle(hdr, s->now);
        return;
    }
    s->live += gc_header_total_size(hdr);
}

/* Sweep one region: free the handles of condemned objects below `limit`
 * and record the live bytes.  Returns the live bytes. */
size_t gc_col_sweep_region(GCRegion *r, uintptr_t limit, bool now) {
    SweepState s = { 0, now };
    uintptr_t start = (uintptr_t)gc_core_region_start(r) + GC_HEADER_SIZE;
    gc_core_walk_region(r, start, limit, sweep_object, &s);
    r->live_bytes = s.live;
    return s.live;
}

/* Stop-the-world sweep of the whole heap; releases regions that end up
 * empty. */
static void sweep_all_now(void) {
    uint32_t hwm = atomic_load(&g_core.region_hwm);
    for (uint32_t i = 0; i < hwm; i++) {
        GCRegion *r = &g_core.regions[i];
        uint32_t st = atomic_load(&r->state);
        if (!gc_core_region_in_use(st)) continue;
        if (r->permanent) continue;
        size_t live = gc_col_sweep_region(r, atomic_load(&r->top), true);
        if (live == 0 && r->alloc_owner == GC_OWNER_NONE && !r->pinned) {
            atomic_store(&r->recycle_state, GC_RECYCLE_IDLE);
            gc_core_region_release(r);
        } else if (g_core.holes && st == GC_RS_OLD && r->alloc_owner == GC_OWNER_NONE && !r->pinned &&
                   atomic_load(&r->recycle_state) != GC_RECYCLE_OWNED &&
                   gc_core_format_holes(r)) {
            atomic_store(&r->recycle_state, GC_RECYCLE_AVAILABLE);
            gc_core_offer_recyclable(r);
        }
    }
}

/* ============================================================================
 * Cycle setup
 * ============================================================================ */

/* Start a cycle on the JS thread.  `clear_bits`: clear the new parity's
 * mark bitmaps here (the synchronous collector); the concurrent collector
 * clears them on the GC thread beforehand. */
static void begin_cycle(int kind, bool clear_bits) {
    g_col.kind = kind;
    /* A minor cycle keeps the bitmap: what is marked is old. */
    if (kind != GC_CYCLE_MINOR) g_col.parity ^= 1;
    atomic_store_explicit(&g_col.promoted, 0, memory_order_relaxed);
    uint32_t hwm = atomic_load(&g_core.region_hwm);
    for (uint32_t i = 0; i < hwm; i++) {
        GCRegion *r = &g_core.regions[i];
        uint32_t st = atomic_load(&r->state);
        r->pinned = false;
        if (!gc_core_region_in_use(st)) continue;
        if (clear_bits)
            memset(r->mark_bits[g_col.parity], 0, (GC_REGION_SIZE / GC_GRANULE) / 8);
        r->tams = atomic_load(&r->top);
    }
}

static void end_cycle(void) {
    g_col.kind = GC_CYCLE_NONE;
    g_col.work.n = 0;
    uint32_t hwm = atomic_load(&g_core.region_hwm);
    for (uint32_t i = 0; i < hwm; i++) {
        GCRegion *r = &g_core.regions[i];
        r->pinned = false;
        r->collecting = false;
    }
}

/* ============================================================================
 * Synchronous full collection
 * ============================================================================ */

static void full_collection(uintptr_t stack_lo) {
    JSRuntimeHandle rt = g_gc.rt;
    uint64_t t0 = gc_os_now_ns();
    gc_core_alloc_ctx_retire(&g_core.js_young);
    gc_core_alloc_ctx_retire(&g_core.js_old);
    gc_core_alloc_ctx_retire(&g_core.gc_copy);

    if (g_core.trace_handle) gc_core_trace(0, "--- full collection");
    /* The full collection supersedes any concurrent cycle: nothing is being
     * allocated during it. */
    atomic_store(&g_core.cycle_active, false);
    gc_core_clear_new_lines();
    begin_cycle(GC_CYCLE_FULL, true);
    g_col.pins_n = 0;
    capture_stack_range(stack_lo);
    enumerate_roots(rt, true, mark_cb);
    gc_col_mark_ref_log(rt, 0);
    drain_worklist(rt, 0);
    if (g_core.config.verify) gc_col_verify_impl(rt);
    weak_step(rt);
    sweep_all_now();
    end_cycle();
    gc_core_after_full_collection();
    g_core.stats.full_collections++;
    uint64_t dt = gc_os_now_ns() - t0;
    g_core.stats.full_ns_total += dt;
    if (dt > g_core.stats.full_ns_max) g_core.stats.full_ns_max = dt;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void gc_run(void) {
    if (!g_gc.initialized) return;
    /* Spill callee-saved registers into this frame so the conservative
     * stack scan sees values the caller keeps only in registers. */
    jmp_buf regs;
    setjmp(regs);
    volatile uintptr_t here = (uintptr_t)&regs;
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : : "r"(&regs) : "memory");
#endif
    uintptr_t lo = here < (uintptr_t)&here ? here : (uintptr_t)&here;
    gc_col_finish_concurrent_cycle(lo);
    full_collection(lo);
}

/* ============================================================================
 * Exported helpers for gc_concurrent.c
 * ============================================================================ */

void gc_col_mark_cb(JSRuntimeHandle rt, GCHandle h) { mark_cb(rt, h); }
bool gc_col_drain(JSRuntimeHandle rt, uint32_t limit) { return drain_worklist(rt, limit); }
void gc_col_enumerate_roots(JSRuntimeHandle rt, bool pin) { enumerate_roots(rt, pin, mark_cb); }
void gc_col_enumerate_roots_cb(JSRuntimeHandle rt, JS_MarkFunc *cb) { enumerate_roots(rt, false, cb); }
void gc_col_capture_stack(uintptr_t lo) { capture_stack_range(lo); }
void gc_col_pin_running_code(JSRuntimeHandle rt) { pin_running_code(rt); }
void gc_col_weak_step(JSRuntimeHandle rt) { weak_step(rt); }
void gc_col_begin_cycle(int kind, bool clear_bits) { begin_cycle(kind, clear_bits); }
void gc_col_end_cycle(void) { end_cycle(); }
bool gc_col_header_condemned(const GCHeader *hdr) { return header_condemned(hdr); }
bool gc_col_is_marked(const GCRegion *r, const GCHeader *hdr) { return is_marked(r, hdr); }
bool gc_col_set_mark(GCRegion *r, const GCHeader *hdr) { return set_mark(r, hdr); }
bool gc_col_in_cycle(const GCRegion *r, const GCHeader *hdr) { return in_cycle(r, hdr); }
GCHeader *gc_col_handle_header(GCHandle h) { return handle_header(h); }
void gc_col_worklist_push(GCHandle h) { worklist_push(&g_col.work, h); }
void gc_col_sort_uintptr(uintptr_t *a, size_t n) { qsort(a, n, sizeof(uintptr_t), compare_uintptr); }

/* A copy of an object (gc_realloc, evacuation) under the same handle: its
 * referrers never wrote that handle again, so no card leads a minor cycle
 * to it.  It starts out old. */
void gc_col_mark_copy(GCHeader *hdr) { set_mark(header_region(hdr), hdr); }

/*
 * GC heap: regions, handle tables, allocation, registries.
 * Design: CONCURRENT_GC.md.  The collector itself is in gc_collector.c.
 *
 * Heap
 *   One reserved address range split into 1 MB regions.  Objects are bump
 *   allocated; each starts with an 8-byte header placed at 8 mod 16 so the
 *   payload is 16-byte aligned.  Regions keep side bitmaps: object starts
 *   and two mark bitmaps (cycle parity).
 *
 * Handles
 *   Two handle tables of the same size.  gc_deref reads the front table.
 *   The JS thread writes every entry change to both ("dual write"), front
 *   first.  The collector builds moved addresses in the back table and the
 *   JS thread swaps the tables at a tick boundary.
 *
 * Threads
 *   Only the JS thread allocates engine objects.  The GC thread allocates
 *   copies in its own regions.  Regions are claimed with CAS; nothing here
 *   takes a lock.
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

#include "quickjs.h"
#include "quickjs_gc_unified.h"
#include "quickjs-internal.h"
#include "gc_platform.h"
#include "gc_core.h"

GCState g_gc;
GCCore g_core;

/* ============================================================================
 * Bit helpers
 * ============================================================================ */

static inline size_t gc_align16(size_t n) { return (n + 15) & ~(size_t)15; }

/* Set a bit in a bitmap written by a single thread and read by others. */
static inline void bit_set_single_writer(uint64_t *bits, uint32_t i) {
    _Atomic uint64_t *w = (_Atomic uint64_t *)&bits[i >> 6];
    uint64_t v = atomic_load_explicit(w, memory_order_relaxed);
    atomic_store_explicit(w, v | ((uint64_t)1 << (i & 63)), memory_order_release);
}

/* Highest set bit with index <= i, or -1. */
static int64_t bit_find_prev(const uint64_t *bits, int64_t i) {
    if (i < 0) return -1;
    int64_t w = i >> 6;
    uint64_t v = atomic_load_explicit((_Atomic uint64_t *)&bits[w], memory_order_acquire);
    int sh = (int)(i & 63);
    if (sh != 63) v &= (((uint64_t)1 << (sh + 1)) - 1);
    for (;;) {
        if (v) return (w << 6) + (63 - __builtin_clzll(v));
        if (--w < 0) return -1;
        v = atomic_load_explicit((_Atomic uint64_t *)&bits[w], memory_order_acquire);
    }
}

/* ============================================================================
 * Handle tables
 * ============================================================================ */

#define HANDLE_COMMIT_CHUNK (64u * 1024u)   /* entries committed at a time */

void **gc_core_back_table(void) {
    void **f = atomic_load_explicit(&g_gc.front, memory_order_acquire);
    return f == g_gc.tables[0] ? g_gc.tables[1] : g_gc.tables[0];
}

static bool handle_tables_commit_to(uint32_t limit) {
    while (g_gc.committed_handles < limit) {
        uint32_t start = g_gc.committed_handles;
        size_t bytes = (size_t)HANDLE_COMMIT_CHUNK * sizeof(void *);
        if (start + HANDLE_COMMIT_CHUNK > GC_MAX_HANDLES) return false;
        for (int t = 0; t < 2; t++) {
            if (!gc_os_commit(&g_gc.tables[t][start], bytes)) return false;
        }
        g_gc.committed_handles = start + HANDLE_COMMIT_CHUNK;
    }
    return true;
}

/* Write an entry in both tables (JS thread).  Front first, then back: the
 * collector's back-table updates use CAS and rely on this order. */
void gc_core_handle_dual_write(GCHandle h, void *p) {
    void **f = atomic_load_explicit(&g_gc.front, memory_order_acquire);
    void **b = f == g_gc.tables[0] ? g_gc.tables[1] : g_gc.tables[0];
    atomic_store_explicit((_Atomic(void *) *)&f[h], p, memory_order_release);
    atomic_store_explicit((_Atomic(void *) *)&b[h], p, memory_order_release);
}

bool gc_core_clear_handle(GCHandle h, void *payload) {
    void **f = atomic_load_explicit(&g_gc.front, memory_order_acquire);
    void *expect = payload;
    if (!atomic_compare_exchange_strong_explicit((_Atomic(void *) *)&f[h], &expect, NULL,
                                                 memory_order_acq_rel, memory_order_acquire))
        return false;
    void **b = f == g_gc.tables[0] ? g_gc.tables[1] : g_gc.tables[0];
    /* The back entry normally equals the front one.  Between a flip and the
     * end of the back table's update it may still hold an older address;
     * either way nothing may keep pointing at a freed handle. */
    atomic_store_explicit((_Atomic(void *) *)&b[h], NULL, memory_order_release);
    return true;
}

void gc_core_hcache_push(GCHandle h) {
    if (g_core.hcache_n == g_core.hcache_cap) {
        uint32_t cap = g_core.hcache_cap ? g_core.hcache_cap * 2 : 4096;
        GCHandle *n = (GCHandle *)realloc(g_core.hcache, cap * sizeof(GCHandle));
        if (!n) return;  /* leak the handle rather than fail */
        g_core.hcache = n;
        g_core.hcache_cap = cap;
    }
    g_core.hcache[g_core.hcache_n++] = h;
}

static GCHandle handle_allocate(void) {
    if (g_core.hcache_n == 0)
        gc_core_drain_freed_ring();
    if (g_core.hcache_n > 0) {
        GCHandle r = g_core.hcache[--g_core.hcache_n];
        GC_TRACE(r, "reused by allocation");
        return r;
    }
    uint32_t h = atomic_load_explicit(&g_gc.high_water, memory_order_relaxed);
    if (h >= GC_MAX_HANDLES) {
        fprintf(stderr, "[GC] out of handles\n");
        abort();
    }
    if (h + 1 > g_gc.committed_handles && !handle_tables_commit_to(h + 1)) {
        fprintf(stderr, "[GC] cannot commit handle table\n");
        abort();
    }
    atomic_store_explicit(&g_gc.high_water, h + 1, memory_order_release);
    return h;
}

/* ============================================================================
 * Regions
 * ============================================================================ */

#define BITMAP_WORDS ((GC_REGION_SIZE / GC_GRANULE) / 64)

static bool region_commit_metadata(GCRegion *r) {
    if (!r->start_bits) {
        r->start_bits = (uint64_t *)calloc(BITMAP_WORDS, sizeof(uint64_t));
        r->mark_bits[0] = (uint64_t *)calloc(BITMAP_WORDS, sizeof(uint64_t));
        r->mark_bits[1] = (uint64_t *)calloc(BITMAP_WORDS, sizeof(uint64_t));
    }
    return r->start_bits && r->mark_bits[0] && r->mark_bits[1];
}

/* Bring a claimed region's memory and bitmaps to a clean state. */
static bool region_prepare(GCRegion *r, bool fresh) {
    uint8_t *base = gc_core_region_start(r);
    if (fresh || r->decommitted) {
        if (!gc_os_commit(base, GC_REGION_SIZE)) return false;
        r->decommitted = false;
        if (fresh) atomic_fetch_add(&g_core.regions_committed, 1);
    } else if (r->used_before && !r->zeroed) {
        memset(base, 0, GC_REGION_SIZE);
    }
    r->zeroed = false;
    if (!region_commit_metadata(r)) return false;
    memset(r->start_bits, 0, BITMAP_WORDS * sizeof(uint64_t));
    memset(r->mark_bits[0], 0, BITMAP_WORDS * sizeof(uint64_t));
    memset(r->mark_bits[1], 0, BITMAP_WORDS * sizeof(uint64_t));
    return true;
}

static void region_activate(GCRegion *r, uint32_t space, uint32_t head, uint32_t count) {
    uint8_t *base = gc_core_region_start(r);
    r->used_before = true;
    r->space = space;
    r->alloc_owner = GC_OWNER_NONE;
    r->live_bytes = 0;
    r->tams = (uintptr_t)base;
    r->safe_top = (uintptr_t)base + GC_HEADER_SIZE;
    r->scan_top = (uintptr_t)base + GC_HEADER_SIZE;
    r->in_cset = false;
    r->collecting = false;
    r->keep = false;
    r->pinned = false;
    r->permanent = false;
    r->humongous_head = head;
    r->humongous_count = count;
    atomic_store_explicit(&r->top, (uintptr_t)base + GC_HEADER_SIZE, memory_order_release);
    atomic_store_explicit(&r->state, space, memory_order_release);
    atomic_fetch_add(&g_core.regions_in_use, 1);
}

/* Claim a region for allocation.  Lock-free: CAS a FREE region, or take a
 * never-used one by bumping the high-water mark. */
GCRegion *gc_core_region_acquire(uint32_t space) {
    uint32_t hwm = atomic_load_explicit(&g_core.region_hwm, memory_order_acquire);
    uint32_t hint = atomic_load_explicit((_Atomic uint32_t *)&g_core.region_hint, memory_order_relaxed);
    if (hint >= hwm) hint = 0;
    for (uint32_t n = 0; n < hwm; n++) {
        uint32_t i = hint + n;
        if (i >= hwm) i -= hwm;
        GCRegion *r = &g_core.regions[i];
        if (atomic_load_explicit(&r->state, memory_order_relaxed) != GC_RS_FREE) continue;
        uint32_t expect = GC_RS_FREE;
        if (!atomic_compare_exchange_strong(&r->state, &expect, GC_RS_CLAIMED)) continue;
        atomic_store_explicit((_Atomic uint32_t *)&g_core.region_hint, i + 1, memory_order_relaxed);
        if (!region_prepare(r, false)) {
            atomic_store(&r->state, GC_RS_FREE);
            return NULL;
        }
        region_activate(r, space, i, 1);
        return r;
    }
    uint32_t i = atomic_fetch_add(&g_core.region_hwm, 1);
    if (i >= GC_MAX_REGIONS) {
        fprintf(stderr, "[GC] heap reservation exhausted\n");
        abort();
    }
    GCRegion *r = &g_core.regions[i];
    atomic_store(&r->state, GC_RS_CLAIMED);
    if (!region_prepare(r, true)) {
        fprintf(stderr, "[GC] cannot commit region\n");
        abort();
    }
    region_activate(r, space, i, 1);
    return r;
}

/* Claim n contiguous regions for a humongous object. */
static GCRegion *region_acquire_humongous(uint32_t n) {
    uint32_t hwm = atomic_load_explicit(&g_core.region_hwm, memory_order_acquire);
    for (uint32_t i = 0; i + n <= hwm; i++) {
        uint32_t k;
        for (k = 0; k < n; k++) {
            uint32_t expect = GC_RS_FREE;
            if (!atomic_compare_exchange_strong(&g_core.regions[i + k].state, &expect, GC_RS_CLAIMED))
                break;
        }
        if (k < n) {
            while (k > 0) {
                k--;
                atomic_store(&g_core.regions[i + k].state, GC_RS_FREE);
            }
            continue;
        }
        for (k = 0; k < n; k++) {
            if (!region_prepare(&g_core.regions[i + k], false)) {
                fprintf(stderr, "[GC] cannot commit region\n");
                abort();
            }
        }
        for (k = 0; k < n; k++)
            region_activate(&g_core.regions[i + k], k == 0 ? GC_RS_HUMONGOUS : GC_RS_HUMONGOUS_CONT,
                            i, k == 0 ? n : 0);
        return &g_core.regions[i];
    }
    uint32_t i = atomic_fetch_add(&g_core.region_hwm, n);
    if (i + n > GC_MAX_REGIONS) {
        fprintf(stderr, "[GC] heap reservation exhausted\n");
        abort();
    }
    for (uint32_t k = 0; k < n; k++) {
        GCRegion *r = &g_core.regions[i + k];
        atomic_store(&r->state, GC_RS_CLAIMED);
        if (!region_prepare(r, true)) {
            fprintf(stderr, "[GC] cannot commit region\n");
            abort();
        }
    }
    for (uint32_t k = 0; k < n; k++)
        region_activate(&g_core.regions[i + k], k == 0 ? GC_RS_HUMONGOUS : GC_RS_HUMONGOUS_CONT,
                        i, k == 0 ? n : 0);
    return &g_core.regions[i];
}

/* Return a region (or a whole humongous run) to the free pool. */
void gc_core_region_release(GCRegion *r) {
    uint32_t first = (uint32_t)(r - g_core.regions);
    uint32_t count = atomic_load(&r->state) == GC_RS_HUMONGOUS ? r->humongous_count : 1;
    for (uint32_t k = 0; k < count; k++) {
        GCRegion *x = &g_core.regions[first + k];
        x->in_cset = false;
        x->collecting = false;
        x->keep = false;
        x->pinned = false;
        x->permanent = false;
        x->live_bytes = 0;
        x->alloc_owner = GC_OWNER_NONE;
        if (g_core.poison) {
            gc_os_decommit(gc_core_region_start(x), GC_REGION_SIZE);
            x->decommitted = true;
        } else if (x->used_before) {
            /* Zero what was used now, on the thread that releases it (the
             * collector's, when concurrent), instead of the whole region
             * on the allocating thread when it is claimed again.  A
             * humongous continuation's top says nothing: all of it. */
            uint8_t *base = gc_core_region_start(x);
            uintptr_t top = atomic_load_explicit(&x->top, memory_order_acquire);
            size_t used = k == 0 && top > (uintptr_t)base && top <= (uintptr_t)base + GC_REGION_SIZE
                              ? (size_t)(top - (uintptr_t)base) : GC_REGION_SIZE;
            memset(base, 0, used);
            x->zeroed = true;
        }
        atomic_store_explicit(&x->state, GC_RS_FREE, memory_order_release);
        atomic_fetch_sub(&g_core.regions_in_use, 1);
    }
}

/* ============================================================================
 * Allocation
 * ============================================================================ */

void gc_core_alloc_ctx_retire(GCAllocCtx *a) {
    if (a->region) {
        atomic_store_explicit(&a->region->top, (uintptr_t)a->top, memory_order_release);
        a->region->alloc_owner = GC_OWNER_NONE;
    }
    a->region = NULL;
    a->top = a->end = NULL;
}

GCHeader *gc_core_alloc_object_memory(GCAllocCtx *a, size_t payload, JSGCObjectTypeEnum type) {
    size_t total = gc_align16(payload + GC_HEADER_SIZE);
    if (total < GC_GRANULE) total = GC_GRANULE;
    if (total > ((size_t)(UINT32_MAX >> GC_INFO_SIZE_SHIFT)) * GC_GRANULE) {
        fprintf(stderr, "[GC] object too large (%zu bytes)\n", payload);
        abort();
    }
    uint32_t info = (uint32_t)type | (uint32_t)((total / GC_GRANULE) << GC_INFO_SIZE_SHIFT);

    if (total >= GC_HUMONGOUS_MIN) {
        uint32_t n = (uint32_t)((total + GC_HEADER_SIZE + GC_REGION_SIZE - 1) >> GC_REGION_SHIFT);
        GCRegion *r = region_acquire_humongous(n);
        GCHeader *hdr = (GCHeader *)(gc_core_region_start(r) + GC_HEADER_SIZE);
        hdr->info = info;
        bit_set_single_writer(r->start_bits, gc_core_granule_of(r, hdr));
        atomic_store_explicit(&r->top, (uintptr_t)hdr + total, memory_order_release);
        return hdr;
    }

    if (!a->top || a->top + total > a->end) {
        gc_core_alloc_ctx_retire(a);
        GCRegion *r = gc_core_region_acquire(a->space);
        if (!r) return NULL;
        r->alloc_owner = a->owner;
        r->permanent = a->permanent;
        a->region = r;
        a->top = (uint8_t *)atomic_load_explicit(&r->top, memory_order_relaxed);
        a->end = gc_core_region_start(r) + GC_REGION_SIZE;
    }
    GCHeader *hdr = (GCHeader *)a->top;
    a->top += total;
    hdr->info = info;
    bit_set_single_writer(a->region->start_bits, gc_core_granule_of(a->region, hdr));
    /* Publish the new top so the collector can see the object. */
    atomic_store_explicit(&a->region->top, (uintptr_t)a->top, memory_order_release);
    return hdr;
}

/* Types that live long from birth go straight to old space. */
static bool type_starts_old(JSGCObjectTypeEnum type) {
    switch (type) {
    case JS_GC_OBJ_TYPE_JS_RUNTIME:
    case JS_GC_OBJ_TYPE_JS_CONTEXT:
    case JS_GC_OBJ_TYPE_SHAPE:
    case JS_GC_OBJ_TYPE_FUNCTION_BYTECODE:
    case JS_GC_OBJ_TYPE_MODULE:
    case JS_GC_OBJ_TYPE_FUNCTION_DEF:
    case JS_GC_OBJ_TYPE_BYTES:   /* never moves: keep it out of the nursery */
        return true;
    default:
        return false;
    }
}

static GCHandle gc_alloc_in(size_t size, JSGCObjectTypeEnum type, bool old, bool permanent) {
    if (!g_gc.initialized && !gc_init()) return GC_HANDLE_NULL;
    bool young = !old && !permanent && g_core.nursery_enabled;
    GCAllocCtx *a = permanent ? &g_core.js_perm : young ? &g_core.js_young : &g_core.js_old;
    GCHeader *hdr = gc_core_alloc_object_memory(a, size, type);
    if (!hdr) {
        fprintf(stderr, "[GC] out of memory allocating %zu bytes\n", size);
        abort();
    }
    GCHandle h = handle_allocate();
    hdr->handle = h;
    gc_core_handle_dual_write(h, gc_header_payload(hdr));
    GC_TRACE(h, type == JS_GC_OBJ_TYPE_STACK_FRAME ? "allocated (frame)" : "allocated");
    if (type == JS_GC_OBJ_TYPE_JS_CONTEXT)
        gc_core_registry_add(&g_core.contexts, h);
    gc_core_note_allocation(gc_header_total_size(hdr), young);
    return h;
}

GCHandle gc_alloc(size_t size, JSGCObjectTypeEnum gc_obj_type) {
    return gc_alloc_in(size, gc_obj_type, type_starts_old(gc_obj_type), false);
}

GCHandle gc_alloc_old(size_t size, JSGCObjectTypeEnum gc_obj_type) {
    return gc_alloc_in(size, gc_obj_type, true, false);
}

GCHandle gc_alloc_ex(size_t size, JSGCObjectTypeEnum gc_obj_type, GCHandleArrayType array_type) {
    /* Atoms are permanent: they go to regions no cycle looks at, so the
     * collector never has to mark them. */
    bool atom = array_type == GC_HANDLE_ARRAY_ATOM;
    GCHandle h = gc_alloc_in(size, gc_obj_type, atom || type_starts_old(gc_obj_type), atom);
    if (h == GC_HANDLE_NULL) return h;
    if (array_type == GC_HANDLE_ARRAY_ATOM)
        gc_handle_array_add(&g_gc.atom_handles, h);
    else if (array_type == GC_HANDLE_ARRAY_WEAKREF)
        gc_handle_array_add(&g_gc.weakref_handles, h);
    return h;
}

GCHandle gc_realloc(GCHandle handle, size_t new_size) {
    if (handle == GC_HANDLE_NULL)
        return gc_alloc(new_size, JS_GC_OBJ_TYPE_DATA);
    if (new_size == 0) return GC_HANDLE_NULL;
    void *old_ptr = gc_deref(handle);
    if (!old_ptr) return gc_alloc(new_size, JS_GC_OBJ_TYPE_DATA);
    GCHeader *old_hdr = gc_header(old_ptr);
    JSGCObjectTypeEnum type = gc_header_type(old_hdr);
    size_t old_size = gc_header_payload_size(old_hdr);
    GCRegion *orr = gc_core_region_of(old_hdr);
    bool young = g_core.nursery_enabled && orr && orr->space == GC_RS_NURSERY;
    GCAllocCtx *a = orr && orr->permanent ? &g_core.js_perm : young ? &g_core.js_young : &g_core.js_old;
    GCHeader *hdr = gc_core_alloc_object_memory(a, new_size, type);
    if (!hdr) {
        fprintf(stderr, "[GC] out of memory reallocating %zu bytes\n", new_size);
        abort();
    }
    void *new_ptr = gc_header_payload(hdr);
    size_t n = old_size < new_size ? old_size : new_size;
    memcpy(new_ptr, old_ptr, n);
    hdr->handle = handle;
    gc_core_handle_dual_write(handle, new_ptr);
    gc_dirty_range(new_ptr, n);
    gc_core_note_allocation(gc_header_total_size(hdr), young);
    return handle;
}

GCHandle gc_realloc2(GCHandle handle, size_t new_size, size_t *pslack) {
    GCHandle h = gc_realloc(handle, new_size);
    if (pslack && h != GC_HANDLE_NULL) {
        size_t usable = gc_usable_size(h);
        *pslack = usable > new_size ? usable - new_size : 0;
    }
    return h;
}

void gc_free(GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return;
    void *p = gc_deref(handle);
    if (!p) return;
    /* CAS: a concurrent sweep may be freeing the same (unreachable) handle. */
    if (gc_core_clear_handle(handle, p)) {
        GC_TRACE(handle, "gc_free");
        gc_core_free_handle_from_js(handle);
    }
}

/* ============================================================================
 * Queries
 * ============================================================================ */

JSGCObjectTypeEnum gc_handle_get_type(GCHandle handle) {
    return gc_handle_get_type_inline(handle);
}

bool (gc_handle_is_valid)(GCHandle handle) {
    return gc_deref(handle) != NULL;
}

bool gc_ptr_in_heap(void *ptr) {
    uintptr_t off = (uintptr_t)ptr - (uintptr_t)g_gc.heap_base;
    return g_gc.heap_base && off < g_gc.heap_reserved;
}

bool gc_ptr_is_valid(void *ptr) {
    return gc_ptr_to_handle(ptr) != GC_HANDLE_NULL;
}

/* The object whose memory (header or payload) contains p, current or not. */
GCHeader *gc_core_object_containing(const void *p) {
    GCRegion *r = gc_core_region_of(p);
    if (!r) return NULL;
    uint32_t st = atomic_load_explicit(&r->state, memory_order_acquire);
    if (st == GC_RS_HUMONGOUS_CONT) {
        r = &g_core.regions[r->humongous_head];
        st = atomic_load_explicit(&r->state, memory_order_acquire);
    }
    if (!gc_core_region_in_use(st)) return NULL;
    uint8_t *base = gc_core_region_start(r);
    uintptr_t top = atomic_load_explicit(&r->top, memory_order_acquire);
    if ((uintptr_t)p >= top || (uintptr_t)p < (uintptr_t)base + GC_HEADER_SIZE) return NULL;
    int64_t g = st == GC_RS_HUMONGOUS ? 0 : (int64_t)(((uintptr_t)p - (uintptr_t)base) >> 4);
    /* A header at granule k starts at base + 16k + 8; p may lie in the first
     * 8 bytes of granule g that still belong to the previous object. */
    if (st != GC_RS_HUMONGOUS && (((uintptr_t)p - (uintptr_t)base) & 15) < GC_HEADER_SIZE) g--;
    int64_t k = bit_find_prev(r->start_bits, g);
    if (k < 0) return NULL;
    GCHeader *hdr = (GCHeader *)(base + ((size_t)k << 4) + GC_HEADER_SIZE);
    size_t total = gc_header_total_size(hdr);
    if (!total || (uintptr_t)p >= (uintptr_t)hdr + total) return NULL;
    return hdr;
}

GCHandle gc_ptr_to_handle(void *ptr) {
    GCHeader *hdr = gc_core_object_containing(ptr);
    if (!hdr || !gc_core_header_is_current(hdr)) return GC_HANDLE_NULL;
    return hdr->handle;
}

void gc_dirty_range_slow(const void *p, size_t size) {
#if GC_SOFTWARE_DIRTY
    if (!size) return;
    uintptr_t off = (uintptr_t)p - (uintptr_t)g_gc.heap_base;
    if (off >= g_gc.heap_reserved) return;
    uintptr_t last = off + size - 1;
    for (uintptr_t c = off >> GC_CARD_SHIFT; c <= (last >> GC_CARD_SHIFT); c++)
        atomic_store_explicit((_Atomic uint8_t *)&g_gc.cards[c], 1, memory_order_release);
#else
    (void)p; (void)size;
#endif
}

/* ============================================================================
 * Registries: contexts, finalizers, roots (JS thread)
 * ============================================================================ */

/* ============================================================================
 * Retired storage
 * ============================================================================ */

static void **g_retired;
static uint32_t g_retired_n, g_retired_cap;

void gc_core_retire(void *p) {
    if (!p) return;
    if (g_retired_n == g_retired_cap) {
        uint32_t cap = g_retired_cap ? g_retired_cap * 2 : 64;
        void **n = (void **)realloc(g_retired, cap * sizeof(void *));
        if (!n) return;   /* leak rather than free under a reader */
        g_retired = n;
        g_retired_cap = cap;
    }
    g_retired[g_retired_n++] = p;
}

void gc_core_free_retired(void) {
    for (uint32_t i = 0; i < g_retired_n; i++) free(g_retired[i]);
    g_retired_n = 0;
}

/* Grow storage the GC thread may read: copy, publish, retire the old. */
static void *grow_shared(void *old, size_t old_bytes, size_t new_bytes) {
    void *n = calloc(1, new_bytes);
    if (!n) return NULL;
    if (old) memcpy(n, old, old_bytes);
    gc_core_retire(old);
    return n;
}

void gc_core_registry_add(GCRegistry *reg, GCHandle h) {
    if (reg->count == reg->cap) {
        uint32_t cap = reg->cap ? reg->cap * 2 : 16;
        GCRegistryEntry *n = (GCRegistryEntry *)grow_shared(reg->items, reg->cap * sizeof(*n),
                                                            cap * sizeof(*n));
        if (!n) return;
        atomic_store_explicit((_Atomic(GCRegistryEntry *) *)&reg->items, n, memory_order_release);
        reg->cap = cap;
    }
    reg->items[reg->count].handle = h;
    reg->items[reg->count].fn = NULL;
    atomic_store_explicit((_Atomic uint32_t *)&reg->count, reg->count + 1, memory_order_release);
}

void gc_set_handle_finalizer(GCHandle handle, GCFinalizerFunc *finalizer) {
    if (handle == GC_HANDLE_NULL) return;
    for (uint32_t i = 0; i < g_core.finalizers.count; i++) {
        if (g_core.finalizers.items[i].handle == handle) {
            g_core.finalizers.items[i].fn = finalizer;
            return;
        }
    }
    gc_core_registry_add(&g_core.finalizers, handle);
    if (g_core.finalizers.count)
        g_core.finalizers.items[g_core.finalizers.count - 1].fn = finalizer;
}

/* gc_set_handle_finalizer() for a handle known to have none (a new object):
 * no search of the registry. */
void gc_add_handle_finalizer(GCHandle handle, GCFinalizerFunc *finalizer) {
    if (handle == GC_HANDLE_NULL) return;
    gc_core_registry_add(&g_core.finalizers, handle);
    if (g_core.finalizers.count)
        g_core.finalizers.items[g_core.finalizers.count - 1].fn = finalizer;
}

GCFinalizerFunc *gc_get_handle_finalizer(GCHandle handle) {
    for (uint32_t i = 0; i < g_core.finalizers.count; i++)
        if (g_core.finalizers.items[i].handle == handle)
            return g_core.finalizers.items[i].fn;
    return NULL;
}

bool gc_add_root(GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return true;
    gc_core_registry_add(&g_core.roots, handle);
    return true;
}

void gc_remove_root(GCHandle handle) {
    for (uint32_t i = 0; i < g_core.roots.count; i++) {
        if (g_core.roots.items[i].handle == handle) {
            g_core.roots.items[i] = g_core.roots.items[--g_core.roots.count];
            return;
        }
    }
}

void gc_core_walk_region(GCRegion *r, uintptr_t from, uintptr_t to,
                         void (*fn)(GCHeader *hdr, void *opaque), void *opaque) {
    uint8_t *p = (uint8_t *)from;
    while ((uintptr_t)p < to) {
        GCHeader *hdr = (GCHeader *)p;
        size_t total = gc_header_total_size(hdr);
        if (!total) break;
        fn(hdr, opaque);
        p += total;
    }
}

typedef struct TypeWalk {
    JSGCObjectTypeEnum type;
    GCTypeIteratorFunc func;
    void *user_data;
} TypeWalk;

static void type_walk_cb(GCHeader *hdr, void *opaque) {
    TypeWalk *w = (TypeWalk *)opaque;
    if (gc_header_type(hdr) == w->type && gc_core_header_is_current(hdr))
        w->func(hdr->handle, w->user_data);
}

void gc_for_each_object_of_type(JSGCObjectTypeEnum type, GCTypeIteratorFunc func, void *user_data) {
    if (!func || !g_gc.initialized) return;
    if (type == JS_GC_OBJ_TYPE_JS_CONTEXT) {
        for (uint32_t i = 0; i < g_core.contexts.count; i++) {
            GCHandle h = g_core.contexts.items[i].handle;
            if (gc_deref(h) && gc_handle_get_type_inline(h) == JS_GC_OBJ_TYPE_JS_CONTEXT)
                func(h, user_data);
        }
        return;
    }
    TypeWalk w = { type, func, user_data };
    uint32_t hwm = atomic_load_explicit(&g_core.region_hwm, memory_order_acquire);
    for (uint32_t i = 0; i < hwm; i++) {
        GCRegion *r = &g_core.regions[i];
        uint32_t st = atomic_load_explicit(&r->state, memory_order_acquire);
        if (!gc_core_region_in_use(st)) continue;
        gc_core_walk_region(r, (uintptr_t)gc_core_region_start(r) + GC_HEADER_SIZE,
                            atomic_load_explicit(&r->top, memory_order_acquire), type_walk_cb, &w);
    }
}

static void count_cb(GCHandle h, void *d) { (void)h; (*(uint32_t *)d)++; }

uint32_t gc_count_objects_of_type(JSGCObjectTypeEnum type) {
    uint32_t n = 0;
    gc_for_each_object_of_type(type, count_cb, &n);
    return n;
}

/* ============================================================================
 * Handle arrays (engine registries)
 * ============================================================================ */

int gc_handle_array_init(JSHandleArray *arr, uint32_t capacity) {
    if (!arr) return -1;
    if (capacity == 0) capacity = 16;
    arr->handles = (GCHandle *)calloc(capacity, sizeof(GCHandle));
    if (!arr->handles) return -1;
    arr->count = 0;
    arr->capacity = capacity;
    return 0;
}

void gc_handle_array_free(JSHandleArray *arr) {
    if (!arr) return;
    free(arr->handles);
    arr->handles = NULL;
    arr->count = 0;
    arr->capacity = 0;
}

int gc_handle_array_add(JSHandleArray *arr, GCHandle handle) {
    return gc_handle_array_push_with_index(arr, handle, NULL);
}

int gc_handle_array_push_with_index(JSHandleArray *arr, GCHandle handle, uint32_t *out_index) {
    if (!arr || handle == GC_HANDLE_NULL) return -1;
    if (arr->count >= arr->capacity) {
        /* The GC thread may be reading the array (weak processing). */
        uint32_t cap = arr->capacity ? arr->capacity * 2 : 16;
        GCHandle *n = (GCHandle *)grow_shared(arr->handles, arr->capacity * sizeof(GCHandle),
                                              cap * sizeof(GCHandle));
        if (!n) return -1;
        atomic_store_explicit((_Atomic(GCHandle *) *)&arr->handles, n, memory_order_release);
        arr->capacity = cap;
    }
    uint32_t index = arr->count;
    arr->handles[index] = handle;
    atomic_store_explicit((_Atomic uint32_t *)&arr->count, index + 1, memory_order_release);
    if (out_index) *out_index = index;
    return 0;
}

void gc_handle_array_remove(JSHandleArray *arr, GCHandle handle) {
    if (!arr || handle == GC_HANDLE_NULL) return;
    for (uint32_t i = 0; i < arr->count; i++) {
        if (arr->handles[i] == handle) {
            arr->handles[i] = GC_HANDLE_FREED;
            return;
        }
    }
}

void gc_handle_array_compact(JSHandleArray *arr) {
    if (!arr) return;
    uint32_t j = 0;
    for (uint32_t i = 0; i < arr->count; i++) {
        GCHandle h = arr->handles[i];
        if (!gc_handle_array_entry_is_valid(h) || !gc_deref(h)) continue;
        arr->handles[j++] = h;
    }
    arr->count = j;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

bool gc_init(void) {
    if (g_gc.initialized) return true;
    memset(&g_core, 0, sizeof(g_core));

    const char *lv = getenv("QJS_GC_LOG");
    g_core.log_level = lv ? atoi(lv) : 0;
    g_core.poison = getenv("QJS_GC_POISON") != NULL;
    g_core.trace_handle = getenv("QJS_GC_TRACE") != NULL;

    /* Reserve one extra region so the base can be region-aligned. */
    size_t reserve = GC_HEAP_RESERVE + GC_REGION_SIZE;
    g_core.heap_reservation = gc_os_reserve(reserve, gc_os_has_write_watch());
    if (!g_core.heap_reservation) {
        fprintf(stderr, "[GC] cannot reserve %zu bytes of address space\n", reserve);
        return false;
    }
    uintptr_t b = (uintptr_t)g_core.heap_reservation;
    g_gc.heap_base = (uint8_t *)((b + GC_REGION_SIZE - 1) & ~((uintptr_t)GC_REGION_SIZE - 1));
    g_gc.heap_reserved = GC_HEAP_RESERVE;

    g_core.regions = (GCRegion *)calloc(GC_MAX_REGIONS, sizeof(GCRegion));
#if GC_SOFTWARE_DIRTY
    size_t card_bytes = GC_HEAP_RESERVE >> GC_CARD_SHIFT;
    g_gc.cards = (uint8_t *)gc_os_reserve(card_bytes, false);
    if (g_gc.cards && !gc_os_commit(g_gc.cards, card_bytes)) g_gc.cards = NULL;
#endif
    for (int t = 0; t < 2; t++)
        g_gc.tables[t] = (void **)gc_os_reserve((size_t)GC_MAX_HANDLES * sizeof(void *), false);
    if (!g_core.regions || !g_gc.tables[0] || !g_gc.tables[1]
#if GC_SOFTWARE_DIRTY
        || !g_gc.cards
#endif
        ) {
        fprintf(stderr, "[GC] initialization failed\n");
        return false;
    }
    atomic_store(&g_gc.front, g_gc.tables[0]);
    g_gc.committed_handles = 0;
    if (!handle_tables_commit_to(HANDLE_COMMIT_CHUNK)) return false;
    atomic_store(&g_gc.high_water, 1);  /* handle 0 is GC_HANDLE_NULL */

    g_core.js_young.space = GC_RS_NURSERY;
    g_core.js_young.owner = GC_OWNER_JS;
    g_core.js_old.space = GC_RS_OLD;
    g_core.js_old.owner = GC_OWNER_JS;
    g_core.js_perm.space = GC_RS_OLD;
    g_core.js_perm.owner = GC_OWNER_JS;
    g_core.js_perm.permanent = true;
    g_core.gc_copy.space = GC_RS_OLD;
    g_core.gc_copy.owner = GC_OWNER_GC;

    gc_handle_array_init(&g_gc.atom_handles, 1024);
    gc_handle_array_init(&g_gc.weakmap_handles, 64);
    gc_handle_array_init(&g_gc.weakref_handles, 64);
    gc_handle_array_init(&g_gc.finrec_handles, 64);

    g_core.js_thread_id = gc_os_thread_id();
    gc_core_config_defaults();
    g_gc.initialized = true;
    return true;
}

bool gc_is_initialized(void) { return g_gc.initialized; }

void gc_cleanup(void) {
    if (!g_gc.initialized) return;
    gc_core_stop_thread();
    gc_core_free_internal();
    gc_handle_array_free(&g_gc.atom_handles);
    gc_handle_array_free(&g_gc.weakmap_handles);
    gc_handle_array_free(&g_gc.weakref_handles);
    gc_handle_array_free(&g_gc.finrec_handles);
    for (uint32_t i = 0; i < GC_MAX_REGIONS; i++) {
        GCRegion *r = &g_core.regions[i];
        free(r->start_bits);
        free(r->mark_bits[0]);
        free(r->mark_bits[1]);
    }
    free(g_core.regions);
    free(g_core.hcache);
    gc_core_free_retired();
    free(g_retired);
    g_retired = NULL;
    g_retired_cap = 0;
    free(g_core.contexts.items);
    free(g_core.finalizers.items);
    free(g_core.roots.items);
    gc_os_release(g_core.heap_reservation, GC_HEAP_RESERVE + GC_REGION_SIZE);
    for (int t = 0; t < 2; t++)
        gc_os_release(g_gc.tables[t], (size_t)GC_MAX_HANDLES * sizeof(void *));
#if GC_SOFTWARE_DIRTY
    gc_os_release(g_gc.cards, GC_HEAP_RESERVE >> GC_CARD_SHIFT);
#endif
    memset(&g_core, 0, sizeof(g_core));
    memset(&g_gc, 0, sizeof(g_gc));
}

/* The GC thread runs while a runtime is attached. */
void gc_set_runtime(JSRuntimeHandle rt) {
    if (rt == GC_HANDLE_NULL) {
        gc_core_stop_thread();
        g_gc.rt = GC_HANDLE_NULL;
        return;
    }
    g_gc.rt = rt;
    gc_core_start_thread();
}

void gc_background_stop(void) { gc_core_stop_thread(); }
JSRuntimeHandle gc_get_runtime(void) { return (JSRuntimeHandle)g_gc.rt; }

size_t gc_used_bytes(void) {
    GCStats s;
    gc_get_stats(&s);
    return s.heap_used;
}

uint32_t gc_get_handle_count(void) {
    return gc_handle_limit();
}

/* ============================================================================
 * GC-safe linked lists
 * ============================================================================ */

static inline GCListHead *gc_list_head_from_handle(GCHandle handle, size_t offset) {
    if (handle == GC_HANDLE_NULL) return NULL;
    void *ptr = gc_deref(handle);
    return ptr ? (GCListHead *)((uint8_t *)ptr + offset) : NULL;
}

void gc_list_add_between(GCHandle new_node, GCHandle prev, GCHandle next, size_t link_offset) {
    GCListHead *new_link = gc_list_head_from_handle(new_node, link_offset);
    if (!new_link) return;
    new_link->prev = prev;
    new_link->next = next;
    gc_dirty_slot(new_link);
    if (prev != GC_HANDLE_NULL) {
        GCListHead *prev_link = gc_list_head_from_handle(prev, link_offset);
        if (prev_link) {
            prev_link->next = new_node;
            gc_dirty_slot(&prev_link->next);
        }
    }
    if (next != GC_HANDLE_NULL) {
        GCListHead *next_link = gc_list_head_from_handle(next, link_offset);
        if (next_link) {
            next_link->prev = new_node;
            gc_dirty_slot(&next_link->prev);
        }
    }
}

void gc_list_add(GCHandle new_node, struct GCListHead *head, size_t link_offset) {
    if (!head || new_node == GC_HANDLE_NULL) return;
    GCHandle first = head->next;
    gc_list_add_between(new_node, GC_HANDLE_NULL, first, link_offset);
    head->next = new_node;
    if (head->prev == GC_HANDLE_NULL)
        head->prev = new_node;
    gc_dirty_slot(head);
}

void gc_list_add_tail(GCHandle new_node, struct GCListHead *head, size_t link_offset) {
    if (!head || new_node == GC_HANDLE_NULL) return;
    GCHandle last = head->prev;
    gc_list_add_between(new_node, last, GC_HANDLE_NULL, link_offset);
    head->prev = new_node;
    if (head->next == GC_HANDLE_NULL)
        head->next = new_node;
    gc_dirty_slot(head);
}

void gc_list_del(GCHandle node, struct GCListHead *head, size_t link_offset) {
    if (!head || node == GC_HANDLE_NULL) return;
    GCListHead *link = gc_list_head_from_handle(node, link_offset);
    if (!link) return;
    GCHandle prev = link->prev;
    GCHandle next = link->next;
    if (prev != GC_HANDLE_NULL) {
        GCListHead *prev_link = gc_list_head_from_handle(prev, link_offset);
        if (prev_link) {
            prev_link->next = next;
            gc_dirty_slot(&prev_link->next);
        }
    } else {
        head->next = next;
    }
    if (next != GC_HANDLE_NULL) {
        GCListHead *next_link = gc_list_head_from_handle(next, link_offset);
        if (next_link) {
            next_link->prev = prev;
            gc_dirty_slot(&next_link->prev);
        }
    } else {
        head->prev = prev;
    }
    gc_dirty_slot(head);
    link->prev = GC_HANDLE_NULL;
    link->next = GC_HANDLE_NULL;
}

void gc_list_add_tail_in_container(GCHandle new_node, GCHandle container,
                                   size_t list_offset, size_t link_offset) {
    if (container == GC_HANDLE_NULL || new_node == GC_HANDLE_NULL) return;
    GCListHead *head = (GCListHead *)((uint8_t *)gc_deref(container) + list_offset);
    gc_list_add_tail(new_node, head, link_offset);
}

void gc_list_del_in_container(GCHandle node, GCHandle container,
                              size_t list_offset, size_t link_offset) {
    if (container == GC_HANDLE_NULL || node == GC_HANDLE_NULL) return;
    GCListHead *head = (GCListHead *)((uint8_t *)gc_deref(container) + list_offset);
    gc_list_del(node, head, link_offset);
}

/* ============================================================================
 * GCDataArray (see quickjs_types.h)
 * ============================================================================ */

bool gc_data_array_resize(GCDataArray *arr, size_t elem_size, int new_count)
{
    if (new_count <= arr->capacity) {
        /* Zero new elements when growing within capacity */
        if (new_count > arr->count) {
            uint8_t *p = (uint8_t *)GC_DATA_ARRAY_PTR(*arr, void);
            if (p) memset(p + elem_size * arr->count, 0, elem_size * (new_count - arr->count));
        }
        arr->count = new_count;
        return true;
    }
    int new_cap = new_count > arr->capacity * 3 / 2 ? new_count : arr->capacity * 3 / 2;
    if (new_cap < 4) new_cap = 4;

    GCHandle new_handle = gc_alloc(elem_size * new_cap, JS_GC_OBJ_TYPE_DATA);
    if (new_handle == GC_HANDLE_NULL) return false;
    uint8_t *new_data = (uint8_t *)gc_deref(new_handle);
    if (!new_data) return false;

    uint8_t *old_data = (uint8_t *)GC_DATA_ARRAY_PTR(*arr, void);
    if (old_data && arr->count > 0) {
        memcpy(new_data, old_data, elem_size * arr->count);
        gc_dirty_range(new_data, elem_size * arr->count);
    }

    arr->handle = new_handle;
    gc_dirty_slot(&arr->handle);
    arr->capacity = new_cap;
    arr->count = new_count;
    return true;
}

void *gc_data_array_prepare_push(GCDataArray *arr, size_t elem_size)
{
    if (!gc_data_array_resize(arr, elem_size, arr->count + 1))
        return NULL;
    uint8_t *p = (uint8_t *)GC_DATA_ARRAY_PTR(*arr, void);
    return p ? p + elem_size * (arr->count - 1) : NULL;
}

bool gc_data_array_push(GCDataArray *arr, size_t elem_size, const void *elem)
{
    void *slot = gc_data_array_prepare_push(arr, elem_size);
    if (!slot) return false;
    memcpy(slot, elem, elem_size);
    gc_dirty_range(slot, elem_size);
    return true;
}

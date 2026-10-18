/*
 * Internal state shared by the GC heap (quickjs_gc_unified.c) and the
 * collector (gc_collector.c).  Not part of the engine-facing API.
 */
#ifndef GC_CORE_H
#define GC_CORE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "quickjs_gc_unified.h"
#include "gc_platform.h"

/* Region states.  A region in use is NURSERY, OLD, HUMONGOUS (the first
 * region of a humongous object) or HUMONGOUS_CONT (the rest of it). */
enum {
    GC_RS_UNUSED = 0,      /* never committed */
    GC_RS_FREE,            /* committed, in the free pool */
    GC_RS_CLAIMED,         /* being set up by the thread that claimed it */
    GC_RS_NURSERY,
    GC_RS_OLD,
    GC_RS_HUMONGOUS,
    GC_RS_HUMONGOUS_CONT,
};

enum { GC_OWNER_NONE = 0, GC_OWNER_JS, GC_OWNER_GC };

typedef struct GCRegion {
    _Atomic uint32_t state;
    uint32_t space;            /* state while in use */
    uint32_t alloc_owner;      /* allocation context currently filling it */
    uint32_t humongous_head;   /* index of the first region of a humongous run */
    uint32_t humongous_count;  /* regions in the run (head only) */
    bool used_before;          /* memory must be zeroed before reuse */
    bool decommitted;          /* pages returned to the OS (poison mode) */
    bool in_cset;              /* being evacuated by a major cycle */
    bool collecting;           /* in the nursery set a minor cycle collects */
    bool keep;                 /* a move out of it was rejected: it stays in use */
    bool pinned;               /* holds an object pinned for this cycle */
    _Atomic uintptr_t top;     /* end of the last fully allocated object */
    uintptr_t tams;            /* top at mark start (major cycle) */
    uintptr_t safe_top;        /* top published at the last boundary */
    uintptr_t scan_top;        /* collector has scanned new objects up to here */
    size_t live_bytes;
    uint64_t *start_bits;      /* 1 bit per 16-byte granule: object header starts */
    uint64_t *mark_bits[2];    /* mark bitmaps, by cycle parity */
} GCRegion;

typedef struct GCAllocCtx {
    GCRegion *region;
    uint8_t *top;
    uint8_t *end;
    uint32_t space;
    uint32_t owner;
} GCAllocCtx;

typedef struct GCRegistryEntry {
    GCHandle handle;
    GCFinalizerFunc *fn;
} GCRegistryEntry;

typedef struct GCRegistry {
    GCRegistryEntry *items;
    uint32_t count;
    uint32_t cap;
} GCRegistry;

/* Single-producer single-consumer queue of handles, made of linked blocks.
 * The producer fills a block and publishes its count; the consumer frees
 * blocks it has drained.  Never blocks either side. */
#define GC_RING_BLOCK 4096
typedef struct GCRingBlock {
    _Atomic uint32_t count;              /* items published by the producer */
    uint32_t consumed;                   /* consumer-private */
    struct GCRingBlock *_Atomic next;
    GCHandle items[GC_RING_BLOCK];
} GCRingBlock;
typedef struct GCRing {
    GCRingBlock *head;   /* consumer side */
    GCRingBlock *tail;   /* producer side */
} GCRing;

typedef struct GCCore {
    GCRegion *regions;
    _Atomic uint32_t region_hwm;
    uint32_t region_hint;
    _Atomic uint32_t regions_in_use;
    _Atomic uint32_t regions_committed;
    void *heap_reservation;

    GCAllocCtx js_young;   /* JS thread: nursery */
    GCAllocCtx js_old;     /* JS thread: pretenured objects */
    GCAllocCtx gc_copy;    /* GC thread (or sync collector): copies */

    GCHandle *hcache;      /* JS thread free-handle cache */
    uint32_t hcache_n;
    uint32_t hcache_cap;

    GCRegistry contexts;
    GCRegistry finalizers;
    GCRegistry roots;

    int log_level;
    uint32_t trace_handle;   /* debug: log what happens to this handle */
    bool poison;
    bool nursery_enabled;
    bool defer_frees;      /* marking in progress: gc_free must not recycle handles */
    uint64_t js_thread_id;

    GCConfig config;
    GCStats stats;
} GCCore;

extern GCCore g_core;

/* Heap (quickjs_gc_unified.c) */
GCRegion *gc_core_region_acquire(uint32_t space);
void gc_core_handle_dual_write(GCHandle h, void *p);
void gc_core_hcache_push(GCHandle h);
void gc_core_walk_region(GCRegion *r, uintptr_t from, uintptr_t to,
                         void (*fn)(GCHeader *hdr, void *opaque), void *opaque);
GCHeader *gc_core_alloc_object_memory(GCAllocCtx *a, size_t payload, JSGCObjectTypeEnum type);
void gc_core_alloc_ctx_retire(GCAllocCtx *a);
void gc_core_region_release(GCRegion *r);
GCRegion *gc_core_init_humongous_run(uint32_t first, uint32_t n);
GCHeader *gc_core_object_containing(const void *p);
void **gc_core_back_table(void);
void gc_core_registry_add(GCRegistry *reg, GCHandle h);

static inline GCRegion *gc_core_region_of(const void *p) {
    uintptr_t off = (uintptr_t)p - (uintptr_t)g_gc.heap_base;
    if (off >= g_gc.heap_reserved) return 0;
    return &g_core.regions[off >> GC_REGION_SHIFT];
}

static inline uint8_t *gc_core_region_start(const GCRegion *r) {
    return g_gc.heap_base + ((size_t)(r - g_core.regions) << GC_REGION_SHIFT);
}

static inline uint32_t gc_core_granule_of(const GCRegion *r, const void *hdr) {
    return (uint32_t)(((const uint8_t *)hdr - gc_core_region_start(r)) >> 4);
}

static inline bool gc_core_region_in_use(uint32_t st) {
    return st == GC_RS_NURSERY || st == GC_RS_OLD || st == GC_RS_HUMONGOUS;
}

/* An object is current when the front table maps its handle to it.  Old
 * copies (after a move or a realloc) and freed objects are not current. */
static inline bool gc_core_header_is_current(const GCHeader *hdr) {
    GCHandle h = hdr->handle;
    if (h == GC_HANDLE_NULL || h >= atomic_load_explicit(&g_gc.high_water, memory_order_acquire))
        return false;
    void **f = atomic_load_explicit(&g_gc.front, memory_order_acquire);
    return atomic_load_explicit((_Atomic(void *) *)&f[h], memory_order_acquire)
           == (const uint8_t *)hdr + GC_HEADER_SIZE;
}

/* Debug trace (QJS_GC_TRACE=1): events per handle in an in-memory ring,
 * printed for the handles the verifier reports. */
void gc_core_trace(GCHandle h, const char *what);
void gc_core_trace_dump(GCHandle h);
#define GC_TRACE(h, what) do { if (g_core.trace_handle) gc_core_trace((h), (what)); } while (0)

/* The other table: the one the JS thread does not read. */
static inline void **gc_core_back(void) {
    void **f = atomic_load_explicit(&g_gc.front, memory_order_acquire);
    return f == g_gc.tables[0] ? g_gc.tables[1] : g_gc.tables[0];
}

/* Clear a handle's entries if they still point at payload (CAS, so a
 * concurrent change by the other thread wins).  True if this call freed it. */
bool gc_core_clear_handle(GCHandle h, void *payload);

/* Collector (gc_collector.c) */
void gc_core_drain_freed_ring(void);
void gc_core_free_handle_from_js(GCHandle h);
void gc_core_note_allocation(size_t bytes, bool young);
void gc_core_config_defaults(void);
void gc_core_start_thread(void);
void gc_core_stop_thread(void);
void gc_core_free_internal(void);

#endif /* GC_CORE_H */

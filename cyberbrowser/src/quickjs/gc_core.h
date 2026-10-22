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
    bool zeroed;               /* zeroed when released (no zeroing at claim) */
    bool decommitted;          /* pages returned to the OS (poison mode) */
    bool in_cset;              /* being evacuated by a major cycle */
    bool collecting;           /* in the nursery set a minor cycle collects */
    bool keep;                 /* a move out of it was rejected: it stays in use */
    bool permanent;            /* holds atoms: never in a cycle, never moved or freed */
    bool pinned;               /* holds an object pinned for this cycle */
    _Atomic uintptr_t top;     /* end of the last fully allocated object */
    uintptr_t tams;            /* top at mark start (major cycle) */
    uintptr_t safe_top;        /* top published at the last boundary */
    uintptr_t scan_top;        /* collector has scanned new objects up to here */
    size_t live_bytes;
    /* Per line: the granule offset of the first header that starts in it,
     * or GC_LINE_CONT when none does.  Finds objects from addresses. */
    uint8_t *line_first;
    uint64_t *mark_bits[2];    /* mark bitmaps, by cycle parity */
    /* Holes (runs of free lines below top) found by the last major sweep,
     * one bit per line; see gc_core_format_holes. */
    uint64_t *free_lines;
    _Atomic uint32_t recycle_state;   /* GC_RECYCLE_* */
    bool in_recyclable;               /* listed in g_core.recyclable (under its lock) */
} GCRegion;

/* Lines: 128-byte units of a region, starting GC_HEADER_SIZE into it so
 * that a line boundary is a valid header position. */
#define GC_LINE_SHIFT   7
#define GC_LINE_SIZE    ((uintptr_t)1 << GC_LINE_SHIFT)
#define GC_LINES        ((GC_REGION_SIZE - GC_HEADER_SIZE) >> GC_LINE_SHIFT)
#define GC_LINE_WORDS   ((GC_LINES + 63) / 64)
#define GC_LINE_CONT    0xFF
/* Where lines end: the last bytes of a region, less than a line, are unused. */
#define GC_LINES_END_OFFSET (GC_HEADER_SIZE + ((uintptr_t)GC_LINES << GC_LINE_SHIFT))

/* A zero header (size 0) starts free space: the rest of a run of free
 * lines, which a walk skips to the next line where a header starts. */

/* Who may touch a region's holes.  The sweeper moves IDLE or AVAILABLE to
 * SWEEPING and back; an allocation context moves AVAILABLE to OWNED and,
 * when done, to IDLE.  So a region is never swept and filled at once. */
enum { GC_RECYCLE_IDLE = 0, GC_RECYCLE_AVAILABLE, GC_RECYCLE_OWNED, GC_RECYCLE_SWEEPING };

typedef struct GCAllocCtx {
    GCRegion *region;
    uint8_t *top;
    uint8_t *end;
    uint8_t *line_end;         /* end of the line holding top: past it, line bytes are due */
    uint32_t space;
    uint32_t owner;
    bool permanent;            /* fills permanent regions */
    bool recycles;             /* may fill holes of old regions */
    bool in_hole;              /* top/end bound a hole of a recycled region */
    uint32_t next_line;        /* recycled region: where to look for the next hole */
    /* Recycling contexts: a fresh region for objects bigger than a line
     * that do not fit what is left of the current hole. */
    struct GCAllocCtx *overflow;
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
    GCAllocCtx js_perm;    /* JS thread: atoms (permanent) */
    GCAllocCtx gc_copy;    /* GC thread (or sync collector): copies */
    GCAllocCtx js_old_overflow, gc_copy_overflow;

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

    /* Hole reuse (QJS_GC_HOLES, on by default): old regions with holes, for
     * the recycling allocation contexts.  Paused from a major cycle's mark
     * start to the end of its sweep: the sweep must reach every region, and
     * one a context fills cannot be swept. */
    bool holes;
    atomic_flag recyclable_lock;
    uint32_t *recyclable;
    uint32_t recyclable_n, recyclable_cap;
    _Atomic bool holes_paused;

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
/* The first header at or after p, which lies in no object (see
 * gc_core_object_containing), or `limit`. */
uintptr_t gc_core_next_header(const GCRegion *r, uintptr_t p, uintptr_t limit);
void **gc_core_back_table(void);
void gc_core_registry_add(GCRegistry *reg, GCHandle h);
/* After a region's sweep (its condemned objects' handles freed): turn its
 * runs of free lines into walkable holes and offer it for recycling.
 * Returns the bytes in holes.  The caller holds the region SWEEPING. */
size_t gc_core_format_holes(GCRegion *r);
void gc_core_offer_recyclable(GCRegion *r);

static inline GCRegion *gc_core_region_of(const void *p) {
    uintptr_t off = (uintptr_t)p - (uintptr_t)g_gc.heap_base;
    if (off >= g_gc.heap_reserved) return 0;
    return &g_core.regions[off >> GC_REGION_SHIFT];
}

static inline uint8_t *gc_core_region_start(const GCRegion *r) {
    return g_gc.heap_base + ((size_t)(r - g_core.regions) << GC_REGION_SHIFT);
}

static inline uintptr_t gc_core_line_base(const GCRegion *r) {
    return (uintptr_t)gc_core_region_start(r) + GC_HEADER_SIZE;
}

/* p holds a zero header: the next header after the free space, or `limit`
 * if there is none before it. */
static inline uintptr_t gc_core_skip_free(const GCRegion *r, uintptr_t p, uintptr_t limit) {
    uintptr_t lb = gc_core_line_base(r);
    uint32_t l = (uint32_t)((p - lb) >> GC_LINE_SHIFT) + 1;
    while (l < GC_LINES && r->line_first[l] == GC_LINE_CONT) l++;
    if (l >= GC_LINES) return limit;
    uintptr_t q = lb + ((uintptr_t)l << GC_LINE_SHIFT) + ((uintptr_t)r->line_first[l] << 4);
    return q < limit ? q : limit;
}

/* Whether a swept region is sparse enough to copy its survivors out and
 * free it.  With hole reuse only the sparsest are (live under a quarter of
 * the used space): their holes would refill slowly while the region stays
 * committed.  The rest keep their objects in place and offer their holes. */
static inline bool gc_core_region_evacuates(const GCRegion *r, uintptr_t top) {
    size_t used = top - (uintptr_t)gc_core_region_start(r);
    return g_core.holes ? r->live_bytes * 4 <= used : r->live_bytes * 2 <= used;
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

/* Debug: each handle remembers when it was allocated and freed, and what
 * held it before it was reused (QJS_GC_HISTORY=1).  With QJS_GC_NO_REUSE=1
 * freed handles are never handed out again, so a stale handle dereferences
 * to NULL instead of naming a new object. */
typedef struct GCHandleHistory {
    uint32_t alloc_cycle, free_cycle;   /* cycles completed by then */
    uint16_t type;
    uint8_t freed_by;                   /* GC_FREED_* */
    uint8_t alloc_young;
    /* The previous object with this handle. */
    uint32_t prev_alloc_cycle, prev_free_cycle;
    uint16_t prev_type;
    uint8_t prev_freed_by, prev_young;
    uint32_t uses;                      /* objects that have had this handle */
} GCHandleHistory;
enum { GC_FREED_NOT = 0, GC_FREED_JS, GC_FREED_STW_SWEEP, GC_FREED_CONCURRENT_SWEEP };
extern GCHandleHistory *g_handle_history;   /* NULL unless QJS_GC_HISTORY or QJS_GC_NO_REUSE */
extern bool g_handle_no_reuse;
void gc_core_note_freed(GCHandle h, int how);

/* The other table: the one the JS thread does not read. */
static inline void **gc_core_back(void) {
    void **f = atomic_load_explicit(&g_gc.front, memory_order_acquire);
    return f == g_gc.tables[0] ? g_gc.tables[1] : g_gc.tables[0];
}

/* Clear a handle's entries if they still point at payload (CAS, so a
 * concurrent change by the other thread wins).  True if this call freed it. */
bool gc_core_clear_handle(GCHandle h, void *payload);

/* Storage the GC thread may be reading (registry arrays) is not freed when
 * it is replaced: it is retired, and freed at the next mark start, when the
 * GC thread holds no pointer into it.  JS thread only. */
void gc_core_retire(void *p);
void gc_core_free_retired(void);

/* Collector (gc_collector.c) */
void gc_core_drain_freed_ring(void);
void gc_core_free_handle_from_js(GCHandle h);
void gc_core_note_allocation(size_t bytes, bool young);
void gc_core_config_defaults(void);
void gc_core_start_thread(void);
void gc_core_stop_thread(void);
void gc_core_free_internal(void);

#endif /* GC_CORE_H */

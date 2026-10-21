/*
 * Garbage collector for QuickJS: concurrent, region based, handle indexed.
 *
 * Every GC object is reached through a 32-bit handle.  gc_deref() turns a
 * handle into an address with one load from the front handle table.  The
 * collector moves objects by building new addresses in a back table and
 * swapping the two tables at a tick boundary.  See CONCURRENT_GC.md.
 *
 * Object layout: an 8-byte header followed by the payload.  Headers sit at
 * 8 mod 16, so payloads are 16-byte aligned.
 */

#ifndef QUICKJS_GC_UNIFIED_H
#define QUICKJS_GC_UNIFIED_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdatomic.h>

#include "quickjs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Legacy atomic helpers (used by the job queue, hash tables and quickjs.c)
 * ============================================================================ */

#ifdef _WIN32
#include <intrin.h>
static inline uint32_t atomic_load_u32(volatile uint32_t *p) {
    uint32_t val = *p;
    _ReadWriteBarrier();
    return val;
}
static inline void atomic_store_u32(volatile uint32_t *p, uint32_t val) {
    _ReadWriteBarrier();
    *p = val;
    _ReadWriteBarrier();
}
static inline void *atomic_load_ptr(void *volatile *p) {
    void *val = *p;
    _ReadWriteBarrier();
    return val;
}
static inline void atomic_store_ptr(void *volatile *p, void *val) {
    _ReadWriteBarrier();
    *p = val;
    _ReadWriteBarrier();
}
static inline void *atomic_compare_exchange_ptr(void *volatile *p, void *expected, void *desired) {
    return _InterlockedCompareExchangePointer(p, desired, expected);
}
static inline size_t atomic_fetch_add_zu(volatile size_t *p, size_t val) {
    return _InterlockedExchangeAdd64((volatile __int64 *)p, val);
}
static inline uint32_t atomic_fetch_add_u32(volatile uint32_t *p, uint32_t val) {
    return _InterlockedExchangeAdd((volatile long *)p, val);
}
static inline uint32_t atomic_fetch_sub_u32(volatile uint32_t *p, uint32_t val) {
    return (uint32_t)_InterlockedExchangeAdd((volatile long *)p, -(long)val);
}
static inline uint32_t atomic_compare_exchange_u32(volatile uint32_t *p, uint32_t expected, uint32_t desired) {
    return (uint32_t)_InterlockedCompareExchange((volatile long *)p, (long)desired, (long)expected);
}
static inline uint64_t atomic_load_u64(volatile uint64_t *p) {
    uint64_t val = *p;
    _ReadWriteBarrier();
    return val;
}
static inline void atomic_store_u64(volatile uint64_t *p, uint64_t val) {
    _ReadWriteBarrier();
    *p = val;
    _ReadWriteBarrier();
}
static inline uint64_t atomic_fetch_add_u64(volatile uint64_t *p, uint64_t val) {
    return (uint64_t)_InterlockedExchangeAdd64((volatile __int64 *)p, (__int64)val);
}
static inline uint64_t atomic_compare_exchange_u64(volatile uint64_t *p, uint64_t expected, uint64_t desired) {
    return (uint64_t)_InterlockedCompareExchange64((volatile __int64 *)p, (__int64)desired, (__int64)expected);
}
#else
static inline uint32_t atomic_load_u32(volatile uint32_t *p) {
    uint32_t val = *p;
    __sync_synchronize();
    return val;
}
static inline void atomic_store_u32(volatile uint32_t *p, uint32_t val) {
    __sync_synchronize();
    *p = val;
    __sync_synchronize();
}
static inline void *atomic_load_ptr(void *volatile *p) {
    void *val = *p;
    __sync_synchronize();
    return val;
}
static inline void atomic_store_ptr(void *volatile *p, void *val) {
    __sync_synchronize();
    *p = val;
    __sync_synchronize();
}
static inline void *atomic_compare_exchange_ptr(void *volatile *p, void *expected, void *desired) {
    return __sync_val_compare_and_swap(p, expected, desired);
}
static inline size_t atomic_fetch_add_zu(volatile size_t *p, size_t val) {
    return __sync_fetch_and_add(p, val);
}
static inline uint32_t atomic_fetch_add_u32(volatile uint32_t *p, uint32_t val) {
    return __sync_fetch_and_add(p, val);
}
static inline uint32_t atomic_fetch_sub_u32(volatile uint32_t *p, uint32_t val) {
    return __sync_fetch_and_sub(p, val);
}
static inline uint32_t atomic_compare_exchange_u32(volatile uint32_t *p, uint32_t expected, uint32_t desired) {
    return __sync_val_compare_and_swap(p, expected, desired);
}
static inline uint64_t atomic_load_u64(volatile uint64_t *p) {
    uint64_t val = *p;
    __sync_synchronize();
    return val;
}
static inline void atomic_store_u64(volatile uint64_t *p, uint64_t val) {
    __sync_synchronize();
    *p = val;
    __sync_synchronize();
}
static inline uint64_t atomic_fetch_add_u64(volatile uint64_t *p, uint64_t val) {
    return __sync_fetch_and_add(p, val);
}
static inline uint64_t atomic_compare_exchange_u64(volatile uint64_t *p, uint64_t expected, uint64_t desired) {
    return __sync_val_compare_and_swap(p, expected, desired);
}
#endif

/* 128-bit atomics for GCValue-sized slots (lock-free property arrays).
 * addr must be 16-byte aligned. */
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
static inline void atomic_load_128(volatile uint64_t *addr, uint64_t *low, uint64_t *high) {
    uint64_t rax = 0, rdx = 0;
    __asm__ __volatile__ (
        "lock; cmpxchg16b %0"
        : "+m"(*(volatile __int128 *)addr), "+a"(rax), "+d"(rdx)
        : "b"((uint64_t)0), "c"((uint64_t)0)
        : "cc", "memory"
    );
    *low = rax;
    *high = rdx;
}
static inline bool atomic_compare_exchange_128(volatile uint64_t *addr,
                                               uint64_t *expected_low, uint64_t *expected_high,
                                               uint64_t desired_low, uint64_t desired_high) {
    uint64_t rax = *expected_low;
    uint64_t rdx = *expected_high;
    uint8_t success;
    __asm__ __volatile__ (
        "lock; cmpxchg16b %3\n\t"
        "sete %0"
        : "=q"(success), "+a"(rax), "+d"(rdx), "+m"(*(volatile __int128 *)addr)
        : "b"(desired_low), "c"(desired_high)
        : "cc", "memory"
    );
    if (!success) {
        *expected_low = rax;
        *expected_high = rdx;
    }
    return success;
}
static inline void atomic_store_128(volatile uint64_t *addr, uint64_t low, uint64_t high) {
    uint64_t expected_low = 0, expected_high = 0;
    atomic_load_128(addr, &expected_low, &expected_high);
    while (!atomic_compare_exchange_128(addr, &expected_low, &expected_high, low, high)) {
    }
}
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
static inline void atomic_load_128(volatile uint64_t *addr, uint64_t *low, uint64_t *high) {
    __int128 val = __atomic_load_n((volatile __int128 *)addr, __ATOMIC_SEQ_CST);
    *low = (uint64_t)val;
    *high = (uint64_t)(val >> 64);
}
static inline bool atomic_compare_exchange_128(volatile uint64_t *addr,
                                               uint64_t *expected_low, uint64_t *expected_high,
                                               uint64_t desired_low, uint64_t desired_high) {
    union { __int128 v; struct { uint64_t lo, hi; } s; } exp, des;
    exp.s.lo = *expected_low;
    exp.s.hi = *expected_high;
    des.s.lo = desired_low;
    des.s.hi = desired_high;
    bool ok = __atomic_compare_exchange_n((volatile __int128 *)addr, &exp.v, des.v,
                                          false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    if (!ok) {
        *expected_low = exp.s.lo;
        *expected_high = exp.s.hi;
    }
    return ok;
}
static inline void atomic_store_128(volatile uint64_t *addr, uint64_t low, uint64_t high) {
    union { __int128 v; struct { uint64_t lo, hi; } s; } des;
    des.s.lo = low;
    des.s.hi = high;
    __atomic_store_n((volatile __int128 *)addr, des.v, __ATOMIC_SEQ_CST);
}
#else
#error "128-bit atomics are only implemented for x86-64 and arm64"
#endif

/* ============================================================================
 * Heap geometry
 * ============================================================================ */

#define GC_GRANULE          16u
#define GC_HEADER_SIZE      8u
#define GC_REGION_SHIFT     20
#define GC_REGION_SIZE      ((size_t)1 << GC_REGION_SHIFT)        /* 1 MB */
#define GC_CARD_SHIFT       12
#define GC_CARD_SIZE        ((size_t)1 << GC_CARD_SHIFT)          /* 4 KB */
#define GC_HEAP_RESERVE     ((size_t)16 << 30)                    /* 16 GB of address space */
#define GC_MAX_REGIONS      (GC_HEAP_RESERVE >> GC_REGION_SHIFT)
#define GC_MAX_HANDLES      ((uint32_t)1 << 27)
/* Objects larger than this get their own run of regions and never move. */
#define GC_HUMONGOUS_MIN    (GC_REGION_SIZE / 4)

/* Software dirty flags: on Windows the OS tracks writes (GetWriteWatch), so
 * the write helpers do nothing.  Elsewhere they set a byte per 4 KB card. */
#ifndef GC_SOFTWARE_DIRTY
#ifdef _WIN32
#define GC_SOFTWARE_DIRTY 0
#else
#define GC_SOFTWARE_DIRTY 1
#endif
#endif

/* ============================================================================
 * Object header (8 bytes)
 * ============================================================================ */

typedef struct GCHeader {
    uint32_t handle;   /* the object's handle */
    uint32_t info;     /* bits 0-4: JSGCObjectTypeEnum; bits 5-31: total size / 16 */
} GCHeader;

#define GC_INFO_TYPE_MASK 31u
#define GC_INFO_SIZE_SHIFT 5

static inline JSGCObjectTypeEnum gc_header_type(const GCHeader *h) {
    return (JSGCObjectTypeEnum)(h->info & GC_INFO_TYPE_MASK);
}
/* Bytes from the header to the next header. */
static inline size_t gc_header_total_size(const GCHeader *h) {
    return (size_t)(h->info >> GC_INFO_SIZE_SHIFT) * GC_GRANULE;
}
static inline size_t gc_header_payload_size(const GCHeader *h) {
    return gc_header_total_size(h) - GC_HEADER_SIZE;
}
static inline GCHeader *gc_header(void *payload) {
    return payload ? (GCHeader *)((uint8_t *)payload - GC_HEADER_SIZE) : NULL;
}
static inline void *gc_header_payload(GCHeader *h) {
    return (uint8_t *)h + GC_HEADER_SIZE;
}

/* ============================================================================
 * Global state visible to inline fast paths
 * ============================================================================ */

typedef void GCFinalizerFunc(JSRuntimeHandle rt, GCHandle handle, void *payload);

/* Handle arrays (registries of weak containers and atoms), owned by the JS
 * thread. */
typedef struct JSHandleArray {
    GCHandle *handles;
    uint32_t count;
    uint32_t capacity;
} JSHandleArray;

typedef enum {
    GC_HANDLE_ARRAY_GC,
    GC_HANDLE_ARRAY_CONTEXT,
    GC_HANDLE_ARRAY_ATOM,
    GC_HANDLE_ARRAY_JOB,
    GC_HANDLE_ARRAY_WEAKREF,
} GCHandleArrayType;

typedef struct GCState {
    /* Handle tables: gc_deref reads front.  Both tables are reserved at
     * full size and never move. */
    void **_Atomic front;
    void **tables[2];
    _Atomic uint32_t high_water;      /* next never-used handle */
    uint32_t committed_handles;       /* handle-table entries committed */

    uint8_t *heap_base;               /* reserved heap range */
    size_t heap_reserved;
    uint8_t *cards;                   /* software dirty flags, one per card */

    /* Weak predicate: set while the weak step of a cycle runs. */
    bool weak_step_active;

    bool initialized;
    GCHandle rt;

    /* Registries kept by the engine (JS thread only). */
    JSHandleArray atom_handles;
    JSHandleArray weakmap_handles;
    JSHandleArray weakref_handles;
    JSHandleArray finrec_handles;
} GCState;

extern GCState g_gc;
/* Bumped by every gc_init(): handles kept outside the heap from an earlier
 * heap are stale when it differs. */
extern uint32_t g_gc_epoch;

/* Debug build option: report dereferences of dead handles (with
 * QJS_GC_NO_REUSE=1, which keeps freed handles dead). */
#ifndef GC_STALE_CHECK
#define GC_STALE_CHECK 0
#endif
#if GC_STALE_CHECK
void gc_debug_stale_deref(GCHandle handle, void *site);
#endif

static inline __attribute__((always_inline)) void *gc_deref(GCHandle handle) {
    if (handle == GC_HANDLE_NULL ||
        handle >= atomic_load_explicit(&g_gc.high_water, memory_order_relaxed))
        return NULL;
    void **table = atomic_load_explicit(&g_gc.front, memory_order_acquire);
#if GC_STALE_CHECK
    void *r = table[handle];
    if (__builtin_expect(!r, 0))
        gc_debug_stale_deref(handle, ({ __label__ site; site: &&site; }));
    return r;
#else
    return table[handle];
#endif
}

/* Record that the heap slot at p was written (software dirty flag).  Every
 * store of a handle or GCValue into a GC object goes through this. */
static inline void gc_dirty_slot(const void *p) {
#if GC_SOFTWARE_DIRTY
    uintptr_t off = (uintptr_t)p - (uintptr_t)g_gc.heap_base;
    if (off < g_gc.heap_reserved)
        atomic_store_explicit((_Atomic uint8_t *)&g_gc.cards[off >> GC_CARD_SHIFT], 1,
                              memory_order_release);
#else
    (void)p;
#endif
}

/* Mark every card of [p, p+size) dirty. */
void gc_dirty_range_slow(const void *p, size_t size);
static inline void gc_dirty_range(const void *p, size_t size) {
#if GC_SOFTWARE_DIRTY
    uintptr_t off = (uintptr_t)p - (uintptr_t)g_gc.heap_base;
    if (off >= g_gc.heap_reserved || size == 0) return;
    uintptr_t c0 = off >> GC_CARD_SHIFT, c1 = (off + size - 1) >> GC_CARD_SHIFT;
    if (c1 - c0 > 1) {
        gc_dirty_range_slow(p, size);
        return;
    }
    atomic_store_explicit((_Atomic uint8_t *)&g_gc.cards[c0], 1, memory_order_release);
    atomic_store_explicit((_Atomic uint8_t *)&g_gc.cards[c1], 1, memory_order_release);
#else
    (void)p; (void)size;
#endif
}

/* Store a reference into memory that may belong to a GC object, then set
 * the slot's dirty flag.  The value is computed before the destination
 * address, so a collection inside `value` cannot leave a stale address.
 * A statement, on every platform. */
#if GC_SOFTWARE_DIRTY
#define GC_SET(lvalue, value) \
    do { \
        __typeof__(lvalue) gc_set_v_ = (value); \
        __typeof__(lvalue) *gc_set_p_ = &(lvalue); \
        *gc_set_p_ = gc_set_v_; \
        gc_dirty_slot(gc_set_p_); \
    } while (0)
#else
#define GC_SET(lvalue, value) do { (lvalue) = (value); } while (0)
#endif

/* memcpy / memmove into memory that may belong to a GC object. */
static inline void *gc_memcpy(void *dst, const void *src, size_t n) {
    memcpy(dst, src, n);
    gc_dirty_range(dst, n);
    return dst;
}
static inline void *gc_memmove(void *dst, const void *src, size_t n) {
    memmove(dst, src, n);
    gc_dirty_range(dst, n);
    return dst;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

bool gc_init(void);
bool gc_is_initialized(void);
void gc_cleanup(void);
void gc_set_runtime(JSRuntimeHandle rt);
/* Stop the background collector (before tearing down the runtime). */
void gc_background_stop(void);
JSRuntimeHandle gc_get_runtime(void);

/* ============================================================================
 * Allocation
 * ============================================================================ */

GCHandle gc_alloc(size_t size, JSGCObjectTypeEnum gc_obj_type);
GCHandle gc_alloc_ex(size_t size, JSGCObjectTypeEnum gc_obj_type,
                     GCHandleArrayType array_type);
/* Allocate in old space even if the type would normally start young. */
GCHandle gc_alloc_old(size_t size, JSGCObjectTypeEnum gc_obj_type);
/* Move the object to a new allocation of new_size bytes; the handle stays. */
GCHandle gc_realloc(GCHandle handle, size_t new_size);
GCHandle gc_realloc2(GCHandle handle, size_t new_size, size_t *pslack);
/* Explicitly free an object the engine knows is unreachable. */
void gc_free(GCHandle handle);

static inline GCHandle gc_realloc_handle(GCHandle handle, size_t new_size) {
    if (handle == GC_HANDLE_NULL) return GC_HANDLE_NULL;
    return gc_realloc(handle, new_size);
}
static inline GCHandle gc_alloc_js_object(size_t size, JSGCObjectTypeEnum gc_obj_type) {
    return gc_alloc(size, gc_obj_type);
}

/* Memory returned by gc_alloc is always zeroed. */
static inline GCHandle gc_allocz(size_t size, JSGCObjectTypeEnum gc_obj_type) {
    return gc_alloc(size, gc_obj_type);
}
static inline GCHandle gc_allocz_ex(size_t size, JSGCObjectTypeEnum gc_obj_type,
                                    GCHandleArrayType array_type) {
    return gc_alloc_ex(size, gc_obj_type, array_type);
}

static inline GCHandle gc_strdup(const char *str) {
    size_t len = str ? strlen(str) : 0;
    GCHandle handle = gc_alloc(len + 1, JS_GC_OBJ_TYPE_DATA);
    if (handle != GC_HANDLE_NULL && str) {
        char *ptr = (char *)gc_deref(handle);
        if (ptr) memcpy(ptr, str, len + 1);
    }
    return handle;
}

static inline GCHandle gc_strndup(const char *s, size_t n) {
    size_t len = 0;
    if (s) while (len < n && s[len]) len++;
    GCHandle handle = gc_alloc(len + 1, JS_GC_OBJ_TYPE_DATA);
    if (handle != GC_HANDLE_NULL) {
        char *ptr = (char *)gc_deref(handle);
        if (ptr) {
            if (s) memcpy(ptr, s, len);
            ptr[len] = '\0';
        }
    }
    return handle;
}

#define GC_ALLOC_AND_INIT(type, init_code) ({ \
    GCHandle _gc_handle = gc_allocz(sizeof(type), JS_GC_OBJ_TYPE_DATA); \
    if (_gc_handle != GC_HANDLE_NULL) { \
        type *_gc_ptr = (type *)gc_deref(_gc_handle); \
        if (_gc_ptr) { init_code; } \
    } \
    _gc_handle; \
})

#define GC_ALLOC_AND_INIT_EX(type, extra_size, init_code) ({ \
    GCHandle _gc_handle = gc_allocz(sizeof(type) + (extra_size), JS_GC_OBJ_TYPE_DATA); \
    if (_gc_handle != GC_HANDLE_NULL) { \
        type *_gc_ptr = (type *)gc_deref(_gc_handle); \
        if (_gc_ptr) { init_code; } \
    } \
    _gc_handle; \
})

/* ============================================================================
 * Object queries
 * ============================================================================ */

static inline GCHeader *gc_header_from_handle(GCHandle handle) {
    return gc_header(gc_deref(handle));
}

static inline JSGCObjectTypeEnum gc_handle_get_type_inline(GCHandle handle) {
    GCHeader *hdr = gc_header_from_handle(handle);
    return hdr ? gc_header_type(hdr) : JS_GC_OBJ_TYPE_DATA;
}
JSGCObjectTypeEnum gc_handle_get_type(GCHandle handle);

/* Payload size in bytes (at least the size that was requested). */
static inline size_t gc_handle_get_size(GCHandle handle) {
    GCHeader *hdr = gc_header_from_handle(handle);
    return hdr ? gc_header_payload_size(hdr) : 0;
}
static inline size_t gc_usable_size(GCHandle handle) {
    return gc_handle_get_size(handle);
}

bool gc_handle_is_valid(GCHandle handle);
/* Inline at every use (SAFE_VALID runs on most object accesses); the
 * function stays for callers that take its address. */
#define gc_handle_is_valid(handle) (gc_deref(handle) != NULL)

/* Number of elem-sized slots that fit in the object behind h after offset
 * bytes.  The concurrent marker reads a count from one object and an array
 * from another while the JS thread may be growing both, so every loop bound
 * is clamped to the array's real size: a stale count can then only skip
 * slots (whose later stores set dirty flags), never read past the object. */
static inline uint32_t gc_slots(GCHandle h, size_t offset, size_t elem)
{
    size_t sz = gc_handle_get_size(h);
    return sz > offset ? (uint32_t)((sz - offset) / elem) : 0;
}

static inline int gc_clamp_count(int n, uint32_t cap)
{
    if (n < 0) return 0;
    return (uint32_t)n > cap ? (int)cap : n;
}

static inline bool gc_handle_is_freed(GCHandle handle) {
    return !gc_handle_is_valid(handle);
}

/* While the weak step of a collection runs, true for objects the cycle found
 * unreachable.  Weak containers (WeakMap, WeakRef, FinalizationRegistry,
 * the shape hash) use it to drop entries.  Always false outside the step. */
bool gc_object_is_dead(GCHandle handle);

/* Debug (QJS_GC_NO_REUSE): non-NULL when freed handles are kept dead; then
 * report what is known about dead handle h, used at 'where'. */
struct GCHandleHistory;
extern struct GCHandleHistory *g_handle_history;
void gc_debug_report_stale_handle(GCHandle h, const char *where);
/* Old name: "marked" means "not dead". */
static inline uint8_t gc_handle_get_mark(GCHandle handle) {
    return gc_object_is_dead(handle) ? 0 : 1;
}
static inline void gc_handle_set_mark(GCHandle handle, uint8_t mark) {
    (void)handle; (void)mark;
}

/* Number of handles ever allocated (exclusive upper bound). */
static inline uint32_t gc_handle_limit(void) {
    return atomic_load_explicit(&g_gc.high_water, memory_order_relaxed);
}

/* True if ptr points into an object in the GC heap. */
bool gc_ptr_is_valid(void *ptr);
bool gc_ptr_in_heap(void *ptr);
static inline bool gc_ptr_is_valid_handle(GCHandle handle) {
    return gc_deref(handle) != NULL;
}
/* Handle of the object containing ptr, or GC_HANDLE_NULL. */
GCHandle gc_ptr_to_handle(void *ptr);

void gc_set_handle_finalizer(GCHandle handle, GCFinalizerFunc *finalizer);
/* The same for a handle that has no finalizer yet (no registry search). */
void gc_add_handle_finalizer(GCHandle handle, GCFinalizerFunc *finalizer);
GCFinalizerFunc *gc_get_handle_finalizer(GCHandle handle);

/* Iterate the current objects of one type (slow: walks the heap, except
 * for contexts, which have a registry). */
typedef void (*GCTypeIteratorFunc)(GCHandle handle, void *user_data);
void gc_for_each_object_of_type(JSGCObjectTypeEnum type, GCTypeIteratorFunc func, void *user_data);
uint32_t gc_count_objects_of_type(JSGCObjectTypeEnum type);

/* ============================================================================
 * Roots
 * ============================================================================ */

bool gc_add_root(GCHandle handle);
void gc_remove_root(GCHandle handle);

/* ============================================================================
 * Collection
 * ============================================================================ */

/* Full stop-the-world collection on the calling (JS) thread, including
 * compaction.  Safe while JS or native code is running: objects that raw
 * pointers on the stack point into are pinned.  If a concurrent cycle is in
 * progress, it is finished first. */
void gc_run(void);

/* Tick boundary: call when no JS frames are active and the host holds no
 * raw pointers into the GC heap (between event-loop tasks, between jobs).
 * Runs at most one budgeted GC step on the calling thread. */
void gc_tick(void);

/* Interpreter poll: called at function entry and loop back-edges.  Cheap
 * unless the collector has a request pending. */
extern _Atomic uint32_t g_gc_poll_flag;
void gc_poll_slow(void);
static inline void gc_poll(void) {
    if (atomic_load_explicit(&g_gc_poll_flag, memory_order_relaxed))
        gc_poll_slow();
}

/* Configuration and statistics. */
#define GC_HAS_CONCURRENT 1
enum {
    GC_REJECT_NORMAL = 0,   /* reject moved objects only when needed */
    GC_REJECT_ALL,          /* testing: reject every move at the flip */
    GC_REJECT_HALF,         /* testing: reject every other move */
};
typedef struct GCConfig {
    bool background;              /* run concurrent cycles on a GC thread */
    bool nursery;                 /* young generation (with background) */
    uint32_t boundary_budget_us;  /* time budget of one boundary step */
    size_t nursery_size;          /* bytes allocated young before a minor cycle */
    size_t major_trigger;         /* old-space growth that starts a major cycle */
    bool verify;                  /* debug: verify every cycle against a full re-mark */
    uint32_t stress;              /* testing: 1 = run cycles back to back, 2 = also move dense regions */
    uint32_t reject;              /* testing: GC_REJECT_* */
} GCConfig;
void gc_get_config(GCConfig *cfg);
void gc_set_config(const GCConfig *cfg);

typedef struct GCStats {
    uint64_t minor_cycles;
    uint64_t major_cycles;
    uint64_t full_collections;
    uint64_t aborted_cycles;
    uint64_t boundary_steps;
    uint64_t boundary_declines;
    uint64_t boundary_ns_total;
    uint64_t boundary_ns_max;
    uint64_t full_ns_total;
    uint64_t full_ns_max;
    uint64_t dirty_cards_rescanned;
    uint64_t objects_freed;
    uint64_t objects_copied;
    uint64_t copies_rejected;
    uint64_t bytes_promoted;
    uint64_t regions_promoted_in_place;
    uint64_t verify_failures;
    size_t heap_committed;
    size_t heap_used;
    uint32_t regions_in_use;
    uint32_t handles_in_use;
} GCStats;
void gc_get_stats(GCStats *stats);

/* True when a background (concurrent) collector is running. */
bool gc_is_background(void);
/* True when no cycle is in progress. */
bool gc_is_idle(void);
/* Start a cycle if none is running (non-blocking).  Without a background
 * collector this does nothing. */
void gc_request_cycle(bool major);
/* Testing: run tick boundaries until the collector is idle (or timeout_ms
 * passes).  Returns true if it became idle. */
bool gc_wait_idle(uint32_t timeout_ms);

/* A reference to h was stored in memory the collector cannot see dirty
 * (a malloc'd table hanging off a GC object).  JS thread only. */
void gc_note_ref(GCHandle h);

size_t gc_used_bytes(void);
uint32_t gc_get_handle_count(void);
static inline uint32_t gc_get_handle_capacity(void) { return GC_MAX_HANDLES; }
static inline size_t gc_get_used_bytes(void) { return gc_used_bytes(); }

/* ============================================================================
 * Handle arrays
 * ============================================================================ */

/* Sentinel value for removed entries */
#define GC_HANDLE_FREED ((GCHandle)(uintptr_t)-1)

int gc_handle_array_init(JSHandleArray *arr, uint32_t capacity);
void gc_handle_array_free(JSHandleArray *arr);
int gc_handle_array_add(JSHandleArray *arr, GCHandle handle);
int gc_handle_array_push_with_index(JSHandleArray *arr, GCHandle handle, uint32_t *out_index);
void gc_handle_array_remove(JSHandleArray *arr, GCHandle handle);
void gc_handle_array_compact(JSHandleArray *arr);

static inline bool gc_handle_array_entry_is_valid(GCHandle handle) {
    return handle != GC_HANDLE_NULL && handle != GC_HANDLE_FREED;
}

/* ============================================================================
 * Write helpers
 * ============================================================================ */

/* Old barrier API, now a dirty-flag store on the written slot. */
static inline void gc_write_barrier_for_heap_slot(void *slot_ptr, GCHandle target) {
    (void)target;
    gc_dirty_slot(slot_ptr);
}
static inline void gc_write_barrier(GCHandle source, GCHandle target) {
    (void)target;
    gc_dirty_slot(gc_deref(source));
}
#define GC_WRITE_BARRIER(src, tgt) gc_write_barrier((src), (tgt))
#define GC_WRITE_BARRIER_HEAP_HANDLE(slot_ptr, target_handle) gc_dirty_slot(slot_ptr)
#define GC_WRITE_BARRIER_HEAP_VALUE(slot_ptr, val) gc_dirty_slot(slot_ptr)

/* ============================================================================
 * Thread pool (general purpose; used by parallel CSS layout)
 * ============================================================================ */

typedef void (*GCThreadPoolJobFunc)(void *arg);
uint32_t gc_thread_pool_get_thread_count(void);
void gc_thread_pool_wait_empty(void);
bool gc_thread_pool_submit_job(GCThreadPoolJobFunc func, void *arg);
bool gc_thread_pool_submit_test_job(GCThreadPoolJobFunc func, void *arg);

/* ============================================================================
 * GC-safe linked lists (handle based)
 * ============================================================================ */

typedef struct GCListHead {
    GCHandle next;  /* Handle to next node, GC_HANDLE_NULL if none */
    GCHandle prev;  /* Handle to previous node, GC_HANDLE_NULL if none */
} GCListHead;

#define GC_LIST_HEAD_INIT(name) { GC_HANDLE_NULL, GC_HANDLE_NULL }
#define GC_LIST_HEAD(name) \
    struct GCListHead name = GC_LIST_HEAD_INIT(name)

static inline void gc_list_init(struct GCListHead *list) {
    list->next = GC_HANDLE_NULL;
    list->prev = GC_HANDLE_NULL;
    gc_dirty_slot(list);
}

static inline void gc_list_init_in_container(GCHandle container, size_t list_offset) {
    if (container == GC_HANDLE_NULL) return;
    struct GCListHead *head = (struct GCListHead *)((uint8_t*)gc_deref(container) + list_offset);
    head->next = GC_HANDLE_NULL;
    head->prev = GC_HANDLE_NULL;
}

static inline bool gc_list_empty(const struct GCListHead *list) {
    return list->next == GC_HANDLE_NULL;
}

void gc_list_add_between(GCHandle new_node, GCHandle prev, GCHandle next, size_t link_offset);
void gc_list_add(GCHandle new_node, struct GCListHead *head, size_t link_offset);
void gc_list_add_tail(GCHandle new_node, struct GCListHead *head, size_t link_offset);
void gc_list_del(GCHandle node, struct GCListHead *head, size_t link_offset);
void gc_list_add_tail_in_container(GCHandle new_node, GCHandle container,
                                   size_t list_offset, size_t link_offset);
void gc_list_del_in_container(GCHandle node, GCHandle container,
                              size_t list_offset, size_t link_offset);

static inline GCHandle gc_list_first(struct GCListHead *head) {
    return head ? head->next : GC_HANDLE_NULL;
}

static inline GCHandle gc_list_first_from_container(GCHandle container, size_t list_offset) {
    if (container == GC_HANDLE_NULL) return GC_HANDLE_NULL;
    struct GCListHead *head = (struct GCListHead *)((uint8_t*)gc_deref(container) + list_offset);
    return head ? head->next : GC_HANDLE_NULL;
}

static inline GCHandle gc_list_last_from_container(GCHandle container, size_t list_offset) {
    if (container == GC_HANDLE_NULL) return GC_HANDLE_NULL;
    struct GCListHead *head = (struct GCListHead *)((uint8_t*)gc_deref(container) + list_offset);
    return head ? head->prev : GC_HANDLE_NULL;
}

static inline GCHandle gc_list_next(GCHandle node, size_t link_offset) {
    if (node == GC_HANDLE_NULL) return GC_HANDLE_NULL;
    struct GCListHead *link = (struct GCListHead *)((uint8_t*)gc_deref(node) + link_offset);
    return link ? link->next : GC_HANDLE_NULL;
}

static inline GCHandle gc_list_prev(GCHandle node, size_t link_offset) {
    if (node == GC_HANDLE_NULL) return GC_HANDLE_NULL;
    struct GCListHead *link = (struct GCListHead *)((uint8_t*)gc_deref(node) + link_offset);
    return link ? link->prev : GC_HANDLE_NULL;
}

static inline bool gc_list_empty_from_container(GCHandle container, size_t list_offset) {
    if (container == GC_HANDLE_NULL) return true;
    struct GCListHead *head = (struct GCListHead *)((uint8_t*)gc_deref(container) + list_offset);
    return !head || head->next == GC_HANDLE_NULL;
}

#define gc_list_for_each(pos, head_ptr, link_offset) \
    for (GCHandle pos = ((head_ptr) && (head_ptr)->next != GC_HANDLE_NULL) ? (head_ptr)->next : GC_HANDLE_NULL; \
         pos != GC_HANDLE_NULL; \
         pos = gc_list_next(pos, link_offset))

#define gc_list_for_each_safe(pos, n, head_ptr, link_offset) \
    for (GCHandle pos = ((head_ptr) && (head_ptr)->next != GC_HANDLE_NULL) ? (head_ptr)->next : GC_HANDLE_NULL, \
             n = pos != GC_HANDLE_NULL ? gc_list_next(pos, link_offset) : GC_HANDLE_NULL; \
         pos != GC_HANDLE_NULL; \
         pos = n, n = gc_list_next(n, link_offset))

#define gc_list_for_each_from_container(pos, container, list_offset, link_offset) \
    for (GCHandle pos = gc_list_first_from_container(container, list_offset); \
         pos != GC_HANDLE_NULL; \
         pos = gc_list_next(pos, link_offset))

#define gc_list_for_each_safe_from_container(pos, n, container, list_offset, link_offset) \
    for (GCHandle pos = gc_list_first_from_container(container, list_offset), \
             n = gc_list_next(gc_list_first_from_container(container, list_offset), link_offset); \
         pos != GC_HANDLE_NULL; \
         pos = n, n = gc_list_next(n, link_offset))

#define gc_list_entry(handle, type, member) \
    ((handle) == GC_HANDLE_NULL ? NULL : (type *)((uint8_t*)gc_deref(handle) - offsetof(type, member)))

/* ============================================================================
 * Lock-free property-array API (implemented in quickjs.c)
 * ============================================================================ */
bool js_object_prealloc_properties(JSContextHandle ctx, GCHandle obj_handle, uint32_t min_count);
uint32_t js_object_prop_version_load(GCHandle obj_handle);
bool js_object_set_property_value_atomic(JSContextHandle ctx, GCHandle obj_handle,
                                         uint32_t prop_idx, GCValue val);
bool js_object_cas_property_value_atomic(JSContextHandle ctx, GCHandle obj_handle,
                                         uint32_t prop_idx, GCValue expected,
                                         GCValue desired, GCValue *out_actual);
GCValue js_object_get_property_value_atomic(JSContextHandle ctx, GCHandle obj_handle,
                                            uint32_t prop_idx);

#ifdef __cplusplus
}
#endif

#endif /* QUICKJS_GC_UNIFIED_H */

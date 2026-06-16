/*
 * Unified GC Allocator for QuickJS - CLEAN INTERFACE
 * 
 * All allocations are GC-managed objects accessed through handles.
 * No pinning, no raw memory, no manual management.
 */

#ifndef QUICKJS_GC_UNIFIED_H
#define QUICKJS_GC_UNIFIED_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Include core types (GCHandle, GCValue, JSContextHandle, JSRuntimeHandle, etc.) */
#include "quickjs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GC_HEAP_SIZE (4ULL * 1024 * 1024 * 1024)
#define GC_INITIAL_HANDLES 100000
#define GC_DEFAULT_THRESHOLD (256 * 1024)

/* 
 * Canary values for detecting heap corruption
 */
#define GC_CANARY_PREFIX 0xDEADBEEFCAFEBAB0ULL
#define GC_CANARY_SUFFIX 0xB0B1B2B3B4B5B6B7ULL
#define GC_CANARY_CORRUPTED 0x0BAD0BAD0BAD0BADULL

typedef enum {
    GC_CANARY_OK = 0,
    GC_CANARY_CORRUPTED_PREFIX,
    GC_CANARY_CORRUPTED_SUFFIX,
    GC_CANARY_INVALID_HANDLE,
    GC_CANARY_NULL_POINTER,
} GCCanaryStatus;

GCCanaryStatus gc_validate_canaries(GCHandle handle);
GCCanaryStatus gc_validate_canaries_ptr(void *ptr);
int gc_validate_all_canaries(bool verbose);
const char *gc_canary_status_string(GCCanaryStatus status);
void gc_print_corruption_info(GCHandle handle, GCCanaryStatus status);
void gc_check_canaries_or_abort(GCHandle handle, const char *location);
GCCanaryStatus gc_validate_shape_canaries(uint32_t shape_handle);
void gc_diagnose_corruption_context(GCHandle handle);
void gc_dump_heap_for_analysis(const char *filename);

/*
 * GC Finalizer function type - now uses JSRuntimeHandle from quickjs_types.h
 */
typedef void GCFinalizerFunc(JSRuntimeHandle rt, GCHandle handle, void *user_ptr);

void gc_set_handle_finalizer(GCHandle handle, GCFinalizerFunc *finalizer);
GCFinalizerFunc *gc_get_handle_finalizer(GCHandle handle);

typedef enum {
    GC_HANDLE_ARRAY_GC,
    GC_HANDLE_ARRAY_CONTEXT,
    GC_HANDLE_ARRAY_ATOM,
    GC_HANDLE_ARRAY_JOB,
    GC_HANDLE_ARRAY_WEAKREF,
} GCHandleArrayType;

typedef struct JSHandleArray {
    GCHandle *handles;
    uint32_t count;
    uint32_t capacity;
} JSHandleArray;

/*
 * GCHeader - 64-byte aligned header for all GC-managed objects.
 */
typedef struct GCHeader {
    unsigned int gc_obj_type : 4;
    unsigned int mark : 1;
    uint8_t flags;
    uint8_t pad[7];
    
    struct {
        void *next;
        void *prev;
    } link;
    
    uint32_t handle;
    uint32_t size;
    
    GCFinalizerFunc *finalizer;
    
    uint64_t reserved1;
    uint64_t reserved2;
} GCHeader;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(GCHeader) == 64, "GCHeader must be exactly 64 bytes");
#endif

bool gc_init(void);
bool gc_is_initialized(void);
void gc_cleanup(void);
void gc_set_runtime(JSRuntimeHandle rt);
#ifdef __cplusplus
extern "C++" JSRuntimeHandle gc_get_runtime(void);
#else
JSRuntimeHandle gc_get_runtime(void);
#endif

GCHandle gc_alloc(size_t size, JSGCObjectTypeEnum gc_obj_type);
GCHandle gc_alloc_ex(size_t size, JSGCObjectTypeEnum gc_obj_type,
                     GCHandleArrayType array_type);
GCHandle gc_realloc(GCHandle handle, size_t new_size);
GCHandle gc_realloc2(GCHandle handle, size_t new_size, size_t *pslack);

/* Forward declaration for gc_deref - defined in quickjs.cpp or gc implementation */
void *gc_deref(uint32_t handle);

/* Handle-based GC header accessors - these are safe across GC compaction */
static inline GCHeader *gc_header_from_handle(GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return NULL;
    void *ptr = gc_deref(handle);
    if (!ptr) return NULL;
    return (GCHeader*)((uint8_t*)ptr - sizeof(GCHeader));
}

static inline JSGCObjectTypeEnum gc_handle_get_type_inline(GCHandle handle) {
    GCHeader *hdr = gc_header_from_handle(handle);
    return hdr ? (JSGCObjectTypeEnum)hdr->gc_obj_type : JS_GC_OBJ_TYPE_DATA;
}

static inline uint8_t gc_handle_get_mark(GCHandle handle) {
    GCHeader *hdr = gc_header_from_handle(handle);
    return hdr ? hdr->mark : 0;
}

static inline void gc_handle_set_mark(GCHandle handle, uint8_t mark) {
    GCHeader *hdr = gc_header_from_handle(handle);
    if (hdr) hdr->mark = mark;
}

static inline size_t gc_handle_get_size(GCHandle handle) {
    GCHeader *hdr = gc_header_from_handle(handle);
    return hdr ? hdr->size : 0;
}

static inline BOOL gc_handle_is_freed(GCHandle handle) {
    return gc_handle_get_size(handle) == 0;
}

/* Reallocate memory by handle - safe wrapper that avoids exposing pointers */
static inline GCHandle gc_realloc_handle(GCHandle handle, size_t new_size) {
    if (handle == GC_HANDLE_NULL) return GC_HANDLE_NULL;
    return gc_realloc(handle, new_size);
}

/* Legacy pointer-based header access - prefer handle-based accessors */
static inline GCHeader *gc_header(void *user_ptr) {
    if (!user_ptr) return NULL;
    return (GCHeader*)((uint8_t*)user_ptr - sizeof(GCHeader));
}

/* gc_allocz - allocate zeroed memory
 * CRITICAL: Explicitly zero-initializes because bump_alloc does NOT zero memory.
 * Many QuickJS data structures rely on zero-initialization. */
static inline GCHandle gc_allocz(size_t size, JSGCObjectTypeEnum gc_obj_type) {
    GCHandle handle = gc_alloc(size, gc_obj_type);
    if (handle != GC_HANDLE_NULL) {
        void *ptr = gc_deref(handle);
        if (ptr) memset(ptr, 0, size);
    }
    return handle;
}

static inline GCHandle gc_allocz_ex(size_t size, JSGCObjectTypeEnum gc_obj_type,
                                     GCHandleArrayType array_type) {
    GCHandle handle = gc_alloc_ex(size, gc_obj_type, array_type);
    if (handle != GC_HANDLE_NULL) {
        void *ptr = gc_deref(handle);
        if (ptr) memset(ptr, 0, size);
    }
    return handle;
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
    size_t len = s ? strnlen(s, n) : 0;
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

static inline size_t gc_usable_size(GCHandle handle) {
    void *ptr = gc_deref(handle);
    if (!ptr) return 0;
    GCHeader *hdr = gc_header(ptr);
    return hdr->size > sizeof(GCHeader) ? hdr->size - sizeof(GCHeader) : 0;
}

/*
 * ============================================================================
 * GC-Safe Allocation with Initialization
 * ============================================================================
 * 
 * These macros combine allocation and initialization in a single operation,
 * ensuring that the temporary pointer is never stored and only used for
 * immediate initialization.
 * 
 * Usage:
 *   GCHandle handle = GC_ALLOC_AND_INIT(JSMapState, {
 *       p->record_count = 0;
 *       p->is_weak = is_weak;
 *   });
 * 
 * The pointer 'p' is only valid within the init block and must not be stored.
 */

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

/*
 * GC_WITH_HANDLE - Execute code with a typed handle wrapper
 * 
 * Usage:
 *   GCHandle handle = ...;
 *   GC_WITH_HANDLE(JSMapState, handle, ms, {
 *       ms->set_record_count(0);
 *   });
 */
#define GC_WITH_HANDLE(type, handle, var_name, code) do { \
    type##Handle var_name(handle); \
    if (var_name.valid()) { code; } \
} while(0)

bool gc_ptr_is_valid(void *ptr);

/* Validate that a handle points to a valid object in the GC heap */
static inline bool gc_ptr_is_valid_handle(GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return false;
    void *ptr = gc_deref(handle);
    return gc_ptr_is_valid(ptr);
}

static inline GCHandle gc_alloc_js_object(size_t size, JSGCObjectTypeEnum gc_obj_type) {
    return gc_alloc(size, gc_obj_type);
}

static inline GCHandle gc_alloc_js_object_ex(size_t size, JSGCObjectTypeEnum gc_obj_type, 
                                              GCHandleArrayType array_type) {
    return gc_alloc_ex(size, gc_obj_type, array_type);
}

JSGCObjectTypeEnum gc_handle_get_type(GCHandle handle);

void gc_run(void);
void gc_reset(void);
void gc_reset_full(void);

size_t gc_used_bytes(void);
size_t gc_available_bytes(void);
size_t gc_total_bytes(void);

bool gc_add_root(GCHandle handle);
void gc_remove_root(GCHandle handle);

/* Sentinel value to mark freed handle slots (used by gc_sweep_atoms) */
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

/* Type buckets for fast iteration by object type
 * Each bucket contains handles to all live objects of that type */
typedef struct GCTypeBucket {
    GCHandle *handles;      /* Dynamic array of handles */
    uint32_t count;         /* Number of valid handles */
    uint32_t capacity;      /* Allocated capacity */
    uint32_t version;       /* Incremented on modification for safe iteration */
} GCTypeBucket;

/* Object types that have buckets for iteration */
typedef enum {
    GC_BUCKET_JSOBJECT = 0,
    GC_BUCKET_FUNCTION_BYTECODE,
    GC_BUCKET_SHAPE,
    GC_BUCKET_VAR_REF,
    GC_BUCKET_ASYNC_FUNCTION,
    GC_BUCKET_JS_CONTEXT,
    GC_BUCKET_JS_RUNTIME,
    GC_BUCKET_STACK_FRAME,
    GC_BUCKET_DATA,
    GC_BUCKET_JS_STRING,
    GC_BUCKET_JS_STRING_ROPE,
    GC_BUCKET_JS_BIGINT,
    GC_BUCKET_MODULE,
    GC_BUCKET_ARRAY_BUFFER,
    GC_BUCKET_TYPED_ARRAY,
    GC_BUCKET_WEAKREF_HEADER,
    GC_BUCKET_COUNT
} GCObjectBucketType;

/* Map JSGCObjectTypeEnum to bucket type */
static inline GCObjectBucketType gc_type_to_bucket(JSGCObjectTypeEnum type) {
    switch (type) {
        case JS_GC_OBJ_TYPE_JS_OBJECT: return GC_BUCKET_JSOBJECT;
        case JS_GC_OBJ_TYPE_FUNCTION_BYTECODE: return GC_BUCKET_FUNCTION_BYTECODE;
        case JS_GC_OBJ_TYPE_SHAPE: return GC_BUCKET_SHAPE;
        case JS_GC_OBJ_TYPE_VAR_REF: return GC_BUCKET_VAR_REF;
        case JS_GC_OBJ_TYPE_ASYNC_FUNCTION: return GC_BUCKET_ASYNC_FUNCTION;
        case JS_GC_OBJ_TYPE_JS_CONTEXT: return GC_BUCKET_JS_CONTEXT;
        case JS_GC_OBJ_TYPE_JS_RUNTIME: return GC_BUCKET_JS_RUNTIME;
        case JS_GC_OBJ_TYPE_STACK_FRAME: return GC_BUCKET_STACK_FRAME;
        case JS_GC_OBJ_TYPE_AUTO: return GC_BUCKET_DATA;
        case JS_GC_OBJ_TYPE_DATA: return GC_BUCKET_DATA;
        case JS_GC_OBJ_TYPE_JS_STRING: return GC_BUCKET_JS_STRING;
        case JS_GC_OBJ_TYPE_JS_STRING_ROPE: return GC_BUCKET_JS_STRING_ROPE;
        case JS_GC_OBJ_TYPE_JS_BIGINT: return GC_BUCKET_JS_BIGINT;
        case JS_GC_OBJ_TYPE_MODULE: return GC_BUCKET_MODULE;
        default: return GC_BUCKET_DATA;
    }
}

/* Type bucket iteration functions */
void gc_type_bucket_init(GCTypeBucket *bucket);
void gc_type_bucket_free(GCTypeBucket *bucket);
int gc_type_bucket_add(GCTypeBucket *bucket, GCHandle handle);
void gc_type_bucket_remove(GCTypeBucket *bucket, GCHandle handle);
void gc_type_bucket_compact(GCTypeBucket *bucket);

/* Iterate all objects of a given type */
typedef void (*GCTypeIteratorFunc)(GCHandle handle, void *user_data);
void gc_for_each_object_of_type(JSGCObjectTypeEnum type, GCTypeIteratorFunc func, void *user_data);

/* Get count of objects of a given type */
uint32_t gc_count_objects_of_type(JSGCObjectTypeEnum type);

/* Safe iteration with handle validation */
typedef struct GCTypeIterator {
    GCTypeBucket *bucket;
    uint32_t index;
    uint32_t version;  /* Detect modifications during iteration */
} GCTypeIterator;

GCTypeIterator gc_type_iterator_begin(JSGCObjectTypeEnum type);
bool gc_type_iterator_valid(GCTypeIterator *it);
void gc_type_iterator_next(GCTypeIterator *it);
GCHandle gc_type_iterator_handle(GCTypeIterator *it);

/* ============================================================================
 * MULTITHREADED GC - Double-Buffered Design (Lock-Free)
 * ============================================================================
 *
 * Two GCBuffer structs, each with handles[] (pointer table) and storage (object heap).
 * active_handle_table: atomic void** pointer to current active table.
 *
 * Handles are plain indices (no encoding).
 * gc_deref(handle): atomic_load(active_handle_table)[handle] — one atomic load + array index.
 *
 * During compaction: main thread allocates into post-compaction buffer, writes handle
 * to BOTH tables. GC cycle: Mark → set compaction_target → Compact (update active table
 * in-place) → atomic swap active_handle_table.
 *
 * No forwarding map, no handle remapping — tables kept consistent during compaction.
 * 
 * Lock-free allocation:
 * - Handle allocation: atomic increment of handle_count to reserve slot
 * - Memory allocation: atomic increment of bump_offset to reserve space
 * - Compaction sorts live objects by handle before moving (allocations may be out-of-order)
 * ============================================================================ */

typedef enum {
    GC_PHASE_IDLE = 0,
    GC_PHASE_MARKING,
    GC_PHASE_COMPACTING,
    GC_PHASE_SWAPPING,
} GCPhase;

/* Per-buffer state: object storage + handle pointer table */
typedef struct GCBuffer {
    uint8_t *storage;           /* Bump-allocated object heap */
    size_t storage_size;        /* Total size of storage */
    size_t bump_offset;         /* Current allocation offset (atomic access) */
    
    void **handles;             /* Handle-to-pointer table */
    uint32_t handle_count;      /* Next handle index to allocate (atomic access) */
    uint32_t handle_capacity;   /* Allocated capacity of handles[] */
} GCBuffer;

typedef struct GCState {
    /* Double buffers: [0] and [1] alternate as active/compact-target */
    GCBuffer buffers[2];
    
    /* Atomic pointer to the currently active handle table */
    void *volatile active_handle_table;   /* Points to one of buffers[0].handles or buffers[1].handles */
    
    /* Atomic GC phase machine state */
    uint32_t volatile gc_phase;           /* GCPhase enum, atomic */
    uint32_t volatile compaction_target;  /* Which buffer (0 or 1) GC is compacting INTO, atomic */
    
    /* Handle free list (shared between both buffers - handles are indices) */
    uint32_t *free_list;
    uint32_t free_count;
    uint32_t free_capacity;
    
    /* Root set */
    struct {
        GCHandle *roots;
        uint32_t count;
        uint32_t capacity;
    } root_set;
    
    /* Type buckets for fast iteration by object type */
    GCTypeBucket type_buckets[GC_BUCKET_COUNT];
    
    /* Typed handle arrays for special cases */
    JSHandleArray atom_handles;
    JSHandleArray weakmap_handles;
    JSHandleArray weakref_handles;
    JSHandleArray finrec_handles;
    
    /* Backward-compatible handle access shim (points to active buffer) */
    struct {
        void **ptrs;
        uint32_t count;
        uint32_t capacity;
        uint32_t *free_list;
        uint32_t free_count;
        uint32_t free_capacity;
    } handles;
    
    size_t bytes_allocated;
    size_t gc_threshold;
    GCHandle rt;  /* Runtime handle for GC operations */
    bool initialized;
} GCState;

extern GCState g_gc;

/* Backward-compatible macros for code that accesses g_gc.handles directly */
#define g_gc_handles_ptrs (g_gc.active_handle_table)

/* Derive active buffer index from active_handle_table pointer comparison */
static inline uint32_t gc_active_buffer_index(void) {
    return (g_gc.active_handle_table == g_gc.buffers[0].handles) ? 0 : 1;
}
#define g_gc_handles_count (g_gc.buffers[gc_active_buffer_index()].handle_count)

/* ============================================================================
 * GC Thread Control
 * ============================================================================ */

/* Start the background GC thread. Called once during init. */
bool gc_thread_start(void);

/* Stop the background GC thread. Called during cleanup. */
void gc_thread_stop(void);

/* Request a background GC cycle. Non-blocking; returns immediately. */
void gc_request_background(void);

/* Wait for any in-progress background GC to complete. Blocking. */
void gc_wait_for_completion(void);

/* Check if a background GC is currently running. */
bool gc_is_background_running(void);

/* ============================================================================
 * GC-Safe Linked Lists (GCListHead)
 * ============================================================================
 * 
 * These are doubly-linked lists that use GCHandles instead of raw pointers,
 * making them safe to use with the compacting garbage collector.
 * 
 * Usage:
 *   struct MyNode {
 *       GCListHead link;
 *       GCHandle self_handle;
 *       // other fields...
 *   };
 *   
 *   struct MyContainer {
 *       GCListHead list_head;  // Initialize with GC_LIST_HEAD_INIT
 *   };
 */

/* GC-safe list head/node structure using handles */
typedef struct GCListHead {
    GCHandle next;  /* Handle to next node, GC_HANDLE_NULL if none */
    GCHandle prev;  /* Handle to previous node, GC_HANDLE_NULL if none */
} GCListHead;

/* Static initializer for an empty list */
#define GC_LIST_HEAD_INIT(name) { GC_HANDLE_NULL, GC_HANDLE_NULL }
#define GC_LIST_HEAD(name) \
    struct GCListHead name = GC_LIST_HEAD_INIT(name)

/* Initialize a list head */
static inline void gc_list_init(struct GCListHead *list) {
    list->next = GC_HANDLE_NULL;
    list->prev = GC_HANDLE_NULL;
}

/* Initialize a list head in a container (handle-based - GC-safe) */
static inline void gc_list_init_in_container(GCHandle container, size_t list_offset) {
    if (container == GC_HANDLE_NULL) return;
    struct GCListHead *head = (struct GCListHead *)((uint8_t*)gc_deref(container) + list_offset);
    head->next = GC_HANDLE_NULL;
    head->prev = GC_HANDLE_NULL;
}

/* Check if list is empty */
static inline bool gc_list_empty(const struct GCListHead *list) {
    return list->next == GC_HANDLE_NULL;
}

/* Add a node between two existing nodes (internal) */
void gc_list_add_between(GCHandle new_node,
                         GCHandle prev, GCHandle next,
                         size_t link_offset);

/* Add node at the head of the list (pointer-based - use with caution) */
void gc_list_add(GCHandle new_node, struct GCListHead *head,
                 size_t link_offset);

/* Add node at the tail of the list (pointer-based - use with caution) */
void gc_list_add_tail(GCHandle new_node, struct GCListHead *head,
                      size_t link_offset);

/* Delete a node from the list (pointer-based - use with caution) */
void gc_list_del(GCHandle node, struct GCListHead *head,
                 size_t link_offset);

/* Add node at the tail of a list in a container (handle-based - GC-safe) */
void gc_list_add_tail_in_container(GCHandle new_node, GCHandle container,
                                   size_t list_offset, size_t link_offset);

/* Delete a node from a list (raw pointer version) */
void gc_list_del(GCHandle node, struct GCListHead *head,
                 size_t link_offset);

/* Delete a node from a list in a container (handle-based - GC-safe) */
void gc_list_del_in_container(GCHandle node, GCHandle container,
                              size_t list_offset, size_t link_offset);

/* Get the first node from a list head pointer */
static inline GCHandle gc_list_first(struct GCListHead *head) {
    return head ? head->next : GC_HANDLE_NULL;
}

/* Get the first node from a container handle and list offset */
static inline GCHandle gc_list_first_from_container(GCHandle container, size_t list_offset) {
    if (container == GC_HANDLE_NULL) return GC_HANDLE_NULL;
    struct GCListHead *head = (struct GCListHead *)((uint8_t*)gc_deref(container) + list_offset);
    return head ? head->next : GC_HANDLE_NULL;
}

/* Get the last node from a container handle and list offset */
static inline GCHandle gc_list_last_from_container(GCHandle container, size_t list_offset) {
    if (container == GC_HANDLE_NULL) return GC_HANDLE_NULL;
    struct GCListHead *head = (struct GCListHead *)((uint8_t*)gc_deref(container) + list_offset);
    return head ? head->prev : GC_HANDLE_NULL;
}

/* Get next node */
static inline GCHandle gc_list_next(GCHandle node, size_t link_offset) {
    if (node == GC_HANDLE_NULL) return GC_HANDLE_NULL;
    struct GCListHead *link = (struct GCListHead *)((uint8_t*)gc_deref(node) + link_offset);
    return link ? link->next : GC_HANDLE_NULL;
}

/* Get previous node */
static inline GCHandle gc_list_prev(GCHandle node, size_t link_offset) {
    if (node == GC_HANDLE_NULL) return GC_HANDLE_NULL;
    struct GCListHead *link = (struct GCListHead *)((uint8_t*)gc_deref(node) + link_offset);
    return link ? link->prev : GC_HANDLE_NULL;
}

/* Check if list is empty from container handle */
static inline bool gc_list_empty_from_container(GCHandle container, size_t list_offset) {
    if (container == GC_HANDLE_NULL) return true;
    struct GCListHead *head = (struct GCListHead *)((uint8_t*)gc_deref(container) + list_offset);
    return !head || head->next == GC_HANDLE_NULL;
}

/* Iterator macros that work with raw list head pointers */
#define gc_list_for_each(pos, head_ptr, link_offset) \
    for (GCHandle pos = ((head_ptr) && (head_ptr)->next != GC_HANDLE_NULL) ? (head_ptr)->next : GC_HANDLE_NULL; \
         pos != GC_HANDLE_NULL; \
         pos = gc_list_next(pos, link_offset))

#define gc_list_for_each_safe(pos, n, head_ptr, link_offset) \
    for (GCHandle pos = ((head_ptr) && (head_ptr)->next != GC_HANDLE_NULL) ? (head_ptr)->next : GC_HANDLE_NULL, \
             n = pos != GC_HANDLE_NULL ? gc_list_next(pos, link_offset) : GC_HANDLE_NULL; \
         pos != GC_HANDLE_NULL; \
         pos = n, n = gc_list_next(n, link_offset))

/* Iterator macros that work with container handles */
#define gc_list_for_each_from_container(pos, container, list_offset, link_offset) \
    for (GCHandle pos = gc_list_first_from_container(container, list_offset); \
         pos != GC_HANDLE_NULL; \
         pos = gc_list_next(pos, link_offset))

#define gc_list_for_each_safe_from_container(pos, n, container, list_offset, link_offset) \
    for (GCHandle pos = gc_list_first_from_container(container, list_offset), \
             n = gc_list_next(gc_list_first_from_container(container, list_offset), link_offset); \
         pos != GC_HANDLE_NULL; \
         pos = n, n = gc_list_next(n, link_offset))

/* Get the struct containing this list entry */
#define gc_list_entry(handle, type, member) \
    ((handle) == GC_HANDLE_NULL ? NULL : (type *)((uint8_t*)gc_deref(handle) - offsetof(type, member)))

#ifdef __cplusplus
}
#endif

#endif /* QUICKJS_GC_UNIFIED_H */

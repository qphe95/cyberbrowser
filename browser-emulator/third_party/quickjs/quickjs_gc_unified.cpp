#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#ifdef BE_PLATFORM_ANDROID
#include <android/log.h>
#endif
#include "quickjs.h"
#include "quickjs_gc_unified.h"
#include "quickjs-internal.h"

/* 
 * Debug logging for GC - controlled via QJS_DEBUG environment variable
 * Set QJS_DEBUG=1 for info level, QJS_DEBUG=2 for verbose debug
 */
static inline int qjs_gc_debug_level(void) {
    static int level = -1;
    if (level < 0) {
        const char *env = getenv("QJS_DEBUG");
        if (env) {
            level = atoi(env);
            if (level == 0 && env[0] != '0') level = 1;
        } else {
            level = 0;
        }
    }
    return level;
}

#define GC_LOGD(...) do { if (qjs_gc_debug_level() >= 2) { fprintf(stderr, "[D/GC] " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } } while(0)
#define GC_LOGI(...) do { if (qjs_gc_debug_level() >= 1) { fprintf(stderr, "[I/GC] " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } } while(0)
#define GC_LOGW(...) do { if (qjs_gc_debug_level() >= 1) { fprintf(stderr, "[W/GC] " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } } while(0)
#define GC_LOGE(...) do { fprintf(stderr, "[E/GC] " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while(0)

/* External C function declarations */
extern "C" void js_quickjs_reset_class_ids(void);
extern "C" void browser_api_impl_reset(void);

/* Forward declarations for QuickJS GC bridge functions (defined in quickjs.cpp) */
extern "C" {
    /* mark_children - recursively marks all children of a GC object */
    typedef void JS_MarkFunc(JSRuntimeHandle rt, GCHandle handle);
    void mark_children(JSRuntimeHandle rt, GCHandle handle, JS_MarkFunc *mark_func);
    
    /* JS_MarkValue - marks a JSValue if it contains GC references
     * Note: GCValue is defined in quickjs_types.h */
    void JS_MarkValue(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func);
    
    /* JS_MarkContext - marks all roots in a JSContext */
    void JS_MarkContext(JSRuntimeHandle rt, uint32_t ctx_handle, JS_MarkFunc *mark_func);
    
    /* Weak reference handling */
    void gc_remove_weak_objects(JSRuntimeHandle rt);
    
    /* Atom sweeping */
    void gc_sweep_atoms(JSRuntimeHandle rt);
    
    /* Shape hash cleanup */
    void gc_cleanup_shape_hash_table(JSRuntimeHandle rt);
    
    /* Stack frame marking */
    typedef struct JSStackFrame JSStackFrame;
    JSStackFrame *JSRuntime_get_current_stack_frame(JSRuntimeHandle rt);
    
    /* Atom marking helpers */
    uint32_t JSRuntime_get_permanent_atom_count(JSRuntimeHandle rt);
    uint32_t JSRuntime_get_atom_hash_size(JSRuntimeHandle rt);
    uint32_t *JSRuntime_get_atom_hash(JSRuntimeHandle rt);
    uint32_t JSString_get_hash_next(uint32_t atom_idx);
    void JSString_set_hash_next(uint32_t atom_idx, uint32_t next);
    
    /* Shape hash helpers */
    uint32_t JSRuntime_get_shape_hash_size(JSRuntimeHandle rt);
    uint32_t JSRuntime_get_shape_hash_count(JSRuntimeHandle rt);
    void JSRuntime_set_shape_hash_count(JSRuntimeHandle rt, uint32_t count);
    uint32_t *JSRuntime_get_shape_hash(JSRuntimeHandle rt);
    int JSShape_is_hashed(uint32_t shape_handle);
    void JSShape_set_is_hashed(uint32_t shape_handle, int val);
    
    /* Job queue iteration - returns handles directly */
    uint32_t JSRuntime_job_queue_count(JSRuntimeHandle rt);
    uint32_t JSRuntime_job_queue_get_handle(JSRuntimeHandle rt, int index);
    uint32_t JSJobEntry_get_realm_handle(uint32_t job_handle);
    int JSJobEntry_get_argc(uint32_t job_handle);
    GCValue JSJobEntry_get_argv(uint32_t job_handle, int index);
    
    /* Handle arrays in runtime */
    uint32_t JSRuntime_get_shape_hash_handle(JSRuntimeHandle rt);
    uint32_t JSRuntime_get_atom_array_handle(JSRuntimeHandle rt);
    GCValue JSRuntime_get_current_exception(JSRuntimeHandle rt);
    
    /* Stack frame helpers */
    GCValue JSStackFrame_get_cur_func(JSStackFrame *sf);
    JSStackFrame *JSStackFrame_get_prev(JSStackFrame *sf);
    
    /* Module iteration */
    typedef void (*ContextIteratorFunc)(uint32_t ctx_handle, void *user_data);
    void JSRuntime_for_each_context(JSRuntimeHandle rt, ContextIteratorFunc func, void *user_data);
}

#define ALIGN16(size) (((size) + 15) & ~15)
#define MIN_OBJECT_SIZE (sizeof(GCHeader) + 16)  /* 64 + 16 = 80 bytes minimum */

/* 
 * Calculate total allocation size from header.
 * Layout: [8-byte prefix] [64-byte GCHeader] [user data (aligned)] [8-byte suffix]
 */
static inline size_t gc_alloc_total_size(GCHeader *hdr) {
    if (!hdr) return MIN_OBJECT_SIZE;
    /* Mask out FREED flag (bit 31) when calculating size */
    uint32_t user_size = hdr->size & 0x7FFFFFFF;
    return 8 + sizeof(GCHeader) + ALIGN16(user_size) + 8;
}

GCState g_gc = {0};

static void gc_run_internal(void);
static void gc_maybe_run(void);

/* Forward declarations for canary functions (defined later) */
static inline uint64_t *gc_canary_prefix_ptr(GCHeader *hdr);
static inline uint64_t *gc_canary_suffix_ptr(GCHeader *hdr);
static inline void gc_set_canaries(GCHeader *hdr);
static inline void gc_corrupt_canaries(GCHeader *hdr);
static GCCanaryStatus gc_validate_canaries_hdr(GCHeader *hdr, void **out_ptr);

bool gc_init(void) {
    if (g_gc.initialized) return true;
    
    g_gc.heap = (uint8_t*)malloc(GC_HEAP_SIZE);
    if (!g_gc.heap) return false;
    
    g_gc.heap_size = GC_HEAP_SIZE;
    g_gc.bump.base = g_gc.heap;
    g_gc.bump.offset = 0;
    g_gc.bump.capacity = GC_HEAP_SIZE;
    
    /* Allocate handle table separately with malloc for dynamic growth */
    g_gc.handles.ptrs = (void**)malloc(GC_INITIAL_HANDLES * sizeof(void*));
    if (!g_gc.handles.ptrs) {
        free(g_gc.heap);
        g_gc.heap = NULL;
        return false;
    }
    memset(g_gc.handles.ptrs, 0, GC_INITIAL_HANDLES * sizeof(void*));
    g_gc.handles.capacity = GC_INITIAL_HANDLES;
    g_gc.handles.count = 1;
    g_gc.handles.free_list = (uint32_t*)malloc(GC_INITIAL_HANDLES * sizeof(uint32_t));
    if (!g_gc.handles.free_list) {
        free(g_gc.handles.ptrs);
        g_gc.handles.ptrs = NULL;
        free(g_gc.heap);
        g_gc.heap = NULL;
        return false;
    }
    g_gc.handles.free_count = 0;
    g_gc.handles.free_capacity = GC_INITIAL_HANDLES;
    
    /* Initialize root set with dynamic allocation */
    g_gc.root_set.capacity = 256;
    g_gc.root_set.roots = (GCHandle*)malloc(g_gc.root_set.capacity * sizeof(GCHandle));
    if (!g_gc.root_set.roots) {
        free(g_gc.handles.ptrs);
        g_gc.handles.ptrs = NULL;
        free(g_gc.heap);
        g_gc.heap = NULL;
        return false;
    }
    g_gc.root_set.count = 0;
    g_gc.bytes_allocated = 0;
    g_gc.gc_threshold = GC_DEFAULT_THRESHOLD;
    g_gc.rt = GC_HANDLE_NULL;
    
    /* Initialize typed handle arrays (atoms and weakrefs) */
    gc_handle_array_init(&g_gc.weakmap_handles, 100);
    gc_handle_array_init(&g_gc.weakref_handles, 100);
    gc_handle_array_init(&g_gc.finrec_handles, 100);
    gc_handle_array_init(&g_gc.atom_handles, 1000);
    
    /* Initialize type buckets */
    for (int i = 0; i < GC_BUCKET_COUNT; i++) {
        gc_type_bucket_init(&g_gc.type_buckets[i]);
    }
    
    g_gc.initialized = true;
    return true;
}

bool gc_is_initialized(void) {
    return g_gc.initialized;
}

void gc_cleanup(void) {
    /* Free typed handle arrays (atoms and weakrefs) */
    gc_handle_array_free(&g_gc.weakmap_handles);
    gc_handle_array_free(&g_gc.weakref_handles);
    gc_handle_array_free(&g_gc.finrec_handles);
    gc_handle_array_free(&g_gc.atom_handles);
    
    /* Free type buckets */
    for (int i = 0; i < GC_BUCKET_COUNT; i++) {
        gc_type_bucket_free(&g_gc.type_buckets[i]);
    }
    
    /* Free root set array */
    if (g_gc.root_set.roots) {
        free(g_gc.root_set.roots);
        g_gc.root_set.roots = NULL;
    }
    
    /* Free handle table */
    if (g_gc.handles.ptrs) {
        free(g_gc.handles.ptrs);
        g_gc.handles.ptrs = NULL;
    }
    if (g_gc.handles.free_list) {
        free(g_gc.handles.free_list);
        g_gc.handles.free_list = NULL;
    }
    
    if (g_gc.heap) {
        free(g_gc.heap);
        g_gc.heap = NULL;
    }
    memset(&g_gc, 0, sizeof(g_gc));
}

void gc_set_runtime(JSRuntimeHandle rt) {
    g_gc.rt = rt.handle();
}

JSRuntimeHandle gc_get_runtime(void) {
    return JSRuntimeHandle(g_gc.rt);
}

void gc_set_handle_finalizer(GCHandle handle, GCFinalizerFunc *finalizer) {
    if (handle == GC_HANDLE_NULL || handle >= g_gc.handles.count) return;
    
    void *ptr = g_gc.handles.ptrs[handle];
    if (!ptr) return;
    
    GCHeader *hdr = gc_header(ptr);
    if (!hdr) return;
    
    hdr->finalizer = finalizer;
}

GCFinalizerFunc *gc_get_handle_finalizer(GCHandle handle) {
    if (handle == GC_HANDLE_NULL || handle >= g_gc.handles.count) return NULL;
    
    void *ptr = g_gc.handles.ptrs[handle];
    if (!ptr) return NULL;
    
    GCHeader *hdr = gc_header(ptr);
    if (!hdr) return NULL;
    
    return hdr->finalizer;
}

/* ============================================================================
 * HANDLE ARRAY MANAGEMENT
 * ============================================================================ */

int gc_handle_array_init(JSHandleArray *arr, uint32_t capacity) {
    if (!arr) return -1;
    arr->handles = (GCHandle*)malloc(capacity * sizeof(GCHandle));
    if (!arr->handles) return -1;
    memset(arr->handles, 0, capacity * sizeof(GCHandle));
    arr->count = 0;
    arr->capacity = capacity;
    return 0;
}

void gc_handle_array_free(JSHandleArray *arr) {
    if (!arr) return;
    if (arr->handles) {
        free(arr->handles);
        arr->handles = NULL;
    }
    arr->count = 0;
    arr->capacity = 0;
}

int gc_handle_array_add(JSHandleArray *arr, GCHandle handle) {
    if (!arr || handle == GC_HANDLE_NULL) return -1;
    if (arr->count >= arr->capacity) {
        /* Grow array */
        uint32_t new_capacity = arr->capacity * 2;
        GCHandle *new_handles = (GCHandle*)realloc(arr->handles, new_capacity * sizeof(GCHandle));
        if (!new_handles) return -1;
        arr->handles = new_handles;
        arr->capacity = new_capacity;
    }
    arr->handles[arr->count++] = handle;
    return 0;
}

/*
 * gc_handle_array_push_with_index - Add a handle and return the index where it was stored.
 * Similar to gc_handle_array_add but returns the array index via out_index.
 * Returns 0 on success, -1 on failure.
 */
int gc_handle_array_push_with_index(JSHandleArray *arr, GCHandle handle, uint32_t *out_index) {
    if (!arr || handle == GC_HANDLE_NULL) return -1;
    
    /* Grow array if full */
    if (arr->count >= arr->capacity) {
        uint32_t new_capacity = arr->capacity * 2;
        if (new_capacity < arr->capacity) {
            /* Overflow check - would exceed uint32_t max */
            GC_LOGE("gc_handle_array_push_with_index: capacity overflow, cannot grow");
            return -1;
        }
        GC_LOGI("gc_handle_array_push_with_index: growing array from %u to %u", arr->capacity, new_capacity);
        GCHandle *new_handles = (GCHandle*)realloc(arr->handles, new_capacity * sizeof(GCHandle));
        if (!new_handles) {
            GC_LOGE("gc_handle_array_push_with_index: realloc failed, out of memory");
            return -1;
        }
        /* Zero-initialize new slots */
        memset(new_handles + arr->capacity, 0, (new_capacity - arr->capacity) * sizeof(GCHandle));
        arr->handles = new_handles;
        arr->capacity = new_capacity;
        GC_LOGI("gc_handle_array_push_with_index: array grown to capacity=%u", arr->capacity);
    }
    
    uint32_t index = arr->count++;  /* 0-based array index */
    arr->handles[index] = handle;   /* Store unified GC handle */
    
    GC_LOGI("gc_handle_array_push_with_index: arr=%p arr->handles=%p added handle=%u (index=%u)", 
            (void*)arr, (void*)arr->handles, handle, index);
    
    if (out_index) {
        *out_index = index;
    }
    return 0;
}

void gc_handle_array_remove(JSHandleArray *arr, GCHandle handle) {
    if (!arr || handle == GC_HANDLE_NULL) return;
    for (uint32_t i = 0; i < arr->count; i++) {
        if (arr->handles[i] == handle) {
            /* Mark as freed (compaction will remove it) */
            arr->handles[i] = GC_HANDLE_FREED;
            return;
        }
    }
}

void gc_handle_array_compact(JSHandleArray *arr) {
    if (!arr) return;
    uint32_t j = 0;
    for (uint32_t i = 0; i < arr->count; i++) {
        GCHandle handle = arr->handles[i];
        if (handle == GC_HANDLE_NULL || handle == GC_HANDLE_FREED) {
            continue;  /* Skip freed/invalid entries */
        }
        /* Check if object still exists (size != 0) */
        void *ptr = gc_deref(handle);
        if (ptr) {
            GCHeader *hdr = gc_header(ptr);
            if (hdr && hdr->size != 0) {
                arr->handles[j++] = handle;
            }
        }
    }
    arr->count = j;
}

/* ============================================================================
 * TYPE BUCKET MANAGEMENT
 * 
 * Type buckets allow fast iteration over all objects of a specific type.
 * Each bucket is a dynamic array of handles that is automatically maintained
 * during allocation and GC.
 * ============================================================================ */

void gc_type_bucket_init(GCTypeBucket *bucket) {
    if (!bucket) return;
    bucket->handles = NULL;
    bucket->count = 0;
    bucket->capacity = 0;
    bucket->version = 0;
}

void gc_type_bucket_free(GCTypeBucket *bucket) {
    if (!bucket) return;
    if (bucket->handles) {
        free(bucket->handles);
        bucket->handles = NULL;
    }
    bucket->count = 0;
    bucket->capacity = 0;
    bucket->version = 0;
}

int gc_type_bucket_add(GCTypeBucket *bucket, GCHandle handle) {
    if (!bucket || handle == GC_HANDLE_NULL) return -1;
    
    /* Grow if needed */
    if (bucket->count >= bucket->capacity) {
        uint32_t new_capacity = bucket->capacity == 0 ? 64 : bucket->capacity * 2;
        GCHandle *new_handles = (GCHandle*)realloc(bucket->handles, 
                                                    new_capacity * sizeof(GCHandle));
        if (!new_handles) return -1;
        bucket->handles = new_handles;
        bucket->capacity = new_capacity;
    }
    
    bucket->handles[bucket->count++] = handle;
    bucket->version++;
    return 0;
}

void gc_type_bucket_remove(GCTypeBucket *bucket, GCHandle handle) {
    if (!bucket || handle == GC_HANDLE_NULL) return;
    
    for (uint32_t i = 0; i < bucket->count; i++) {
        if (bucket->handles[i] == handle) {
            /* Mark as removed (compaction will clean up) */
            bucket->handles[i] = (GCHandle)(uintptr_t)-1;
            bucket->version++;
            return;
        }
    }
}

void gc_type_bucket_compact(GCTypeBucket *bucket) {
    if (!bucket) return;
    
    uint32_t j = 0;
    for (uint32_t i = 0; i < bucket->count; i++) {
        GCHandle handle = bucket->handles[i];
        if (handle == GC_HANDLE_NULL || handle == (GCHandle)(uintptr_t)-1) {
            continue;  /* Skip freed/invalid entries */
        }
        /* Check if object still exists (size != 0 in header) */
        void *ptr = gc_deref(handle);
        if (ptr) {
            GCHeader *hdr = gc_header(ptr);
            if (hdr && hdr->size != 0) {
                bucket->handles[j++] = handle;
            }
        }
    }
    bucket->count = j;
    bucket->version++;
}

void gc_for_each_object_of_type(JSGCObjectTypeEnum type, GCTypeIteratorFunc func, void *user_data) {
    if (!func) return;
    
    GCObjectBucketType bucket_type = gc_type_to_bucket(type);
    GCTypeBucket *bucket = &g_gc.type_buckets[bucket_type];
    
    /* Compact first to remove dead entries */
    gc_type_bucket_compact(bucket);
    
    /* Iterate over valid handles */
    for (uint32_t i = 0; i < bucket->count; i++) {
        GCHandle handle = bucket->handles[i];
        if (handle != GC_HANDLE_NULL && handle != (GCHandle)(uintptr_t)-1) {
            func(handle, user_data);
        }
    }
}

uint32_t gc_count_objects_of_type(JSGCObjectTypeEnum type) {
    GCObjectBucketType bucket_type = gc_type_to_bucket(type);
    GCTypeBucket *bucket = &g_gc.type_buckets[bucket_type];
    gc_type_bucket_compact(bucket);
    return bucket->count;
}

GCTypeIterator gc_type_iterator_begin(JSGCObjectTypeEnum type) {
    GCTypeIterator it = {0};
    GCObjectBucketType bucket_type = gc_type_to_bucket(type);
    it.bucket = &g_gc.type_buckets[bucket_type];
    it.index = 0;
    it.version = it.bucket->version;
    
    /* Compact and skip to first valid entry */
    gc_type_bucket_compact(it.bucket);
    while (it.index < it.bucket->count) {
        GCHandle handle = it.bucket->handles[it.index];
        if (handle != GC_HANDLE_NULL && handle != (GCHandle)(uintptr_t)-1) {
            break;
        }
        it.index++;
    }
    
    return it;
}

bool gc_type_iterator_valid(GCTypeIterator *it) {
    if (!it || !it->bucket) return false;
    /* Check if bucket was modified during iteration */
    if (it->version != it->bucket->version) return false;
    return it->index < it->bucket->count;
}

void gc_type_iterator_next(GCTypeIterator *it) {
    if (!it || !it->bucket) return;
    
    it->index++;
    /* Skip invalid entries */
    while (it->index < it->bucket->count) {
        GCHandle handle = it->bucket->handles[it->index];
        if (handle != GC_HANDLE_NULL && handle != (GCHandle)(uintptr_t)-1) {
            break;
        }
        it->index++;
    }
}

GCHandle gc_type_iterator_handle(GCTypeIterator *it) {
    if (!it || !it->bucket || it->index >= it->bucket->count) {
        return GC_HANDLE_NULL;
    }
    return it->bucket->handles[it->index];
}

/* 
 * CANARY-ENABLED BUMP ALLOCATOR
 * 
 * Layout: [8-byte prefix] [64-byte GCHeader] [user data] [8-byte suffix]
 * Total overhead: 16 bytes per allocation
 */
static void *bump_alloc(size_t size) {
    /* Add space for prefix and suffix canaries */
    size_t user_size = ALIGN16(size);
    size_t total_size = 8 + sizeof(GCHeader) + user_size + 8; /* canaries + header + data */
    
    /* Simple bump allocation (no threading, no CAS needed) */
    size_t old_offset = g_gc.bump.offset;
    /* CRITICAL: Ensure offset is 16-byte aligned before allocation */
    size_t aligned_offset = ALIGN16(old_offset);
    /* Prefix canary starts 8 bytes before the aligned position */
    size_t alloc_start = aligned_offset + 8;
    size_t new_offset = aligned_offset + total_size;
    if (new_offset > g_gc.heap_size) return NULL;
    
    /* Update bump pointer (single-threaded, no CAS needed) */
    g_gc.bump.offset = new_offset;
    
    /* Set up canaries and header */
    uint8_t *prefix_ptr = g_gc.heap + aligned_offset;
    uint8_t *hdr_ptr = g_gc.heap + alloc_start;
    GCHeader *hdr = (GCHeader*)hdr_ptr;
    uint8_t *user_ptr = hdr_ptr + sizeof(GCHeader);
    uint8_t *suffix_ptr = user_ptr + user_size;
    
    /* Set prefix canary */
    *(uint64_t*)prefix_ptr = GC_CANARY_PREFIX;
    
    /* Initialize header */
    hdr->gc_obj_type = 0;
    hdr->mark = 0;
    hdr->flags = 0;
    memset(hdr->pad, 0, sizeof(hdr->pad));
    hdr->link.next = NULL;
    hdr->link.prev = NULL;
    hdr->handle = GC_HANDLE_NULL;
    hdr->size = user_size;  /* Store USER size, not total size */
    hdr->flags = 0;
    memset(hdr->pad, 0, sizeof(hdr->pad));
    hdr->finalizer = NULL;  /* No finalizer by default */
    hdr->reserved1 = 0;
    hdr->reserved2 = 0;
    
    /* Clear user data */
    memset(user_ptr, 0, user_size);
    
    /* Set suffix canary */
    *(uint64_t*)suffix_ptr = GC_CANARY_SUFFIX;
    
    return user_ptr;
}

/* Grow the handle table when needed */
static bool grow_handle_table(void) {
    uint32_t new_capacity = g_gc.handles.capacity * 2;
    if (new_capacity < 1000) new_capacity = 1000;
    
    void **new_ptrs = (void**)realloc(g_gc.handles.ptrs, new_capacity * sizeof(void*));
    if (!new_ptrs) {
        fprintf(stderr, "[FATAL] Failed to grow handle table to %u entries\n", new_capacity);
        return false;
    }
    
    /* Zero out the new slots */
    memset(&new_ptrs[g_gc.handles.capacity], 0, (new_capacity - g_gc.handles.capacity) * sizeof(void*));
    
    g_gc.handles.ptrs = new_ptrs;
    g_gc.handles.capacity = new_capacity;
    GC_LOGI("Grew handle table to %u entries", new_capacity);
    return true;
}

static inline void push_free_handle(GCHandle handle) {
    if (g_gc.handles.free_count >= g_gc.handles.free_capacity) {
        uint32_t new_cap = g_gc.handles.free_capacity * 2;
        if (new_cap < 1000) new_cap = 1000;
        uint32_t *new_list = (uint32_t*)realloc(g_gc.handles.free_list, new_cap * sizeof(uint32_t));
        if (new_list) {
            g_gc.handles.free_list = new_list;
            g_gc.handles.free_capacity = new_cap;
        }
    }
    if (g_gc.handles.free_count < g_gc.handles.free_capacity) {
        g_gc.handles.free_list[g_gc.handles.free_count++] = handle;
    }
}

static GCHandle allocate_handle(void *ptr) {
    if (!ptr) return GC_HANDLE_NULL;
    
    /* Fast path: reuse a handle from the free list */
    if (g_gc.handles.free_count > 0) {
        GCHandle handle = g_gc.handles.free_list[--g_gc.handles.free_count];
        g_gc.handles.ptrs[handle] = ptr;
        return handle;
    }
    
    /* Need to allocate a new slot */
    if (g_gc.handles.count >= g_gc.handles.capacity) {
        /* Try to grow the handle table */
        if (!grow_handle_table()) {
            fprintf(stderr, "[FATAL] Out of handles (count=%u, capacity=%u) - grow failed\n", 
                    g_gc.handles.count, g_gc.handles.capacity);
            return GC_HANDLE_NULL;
        }
        GC_LOGI("Grew handle table, now capacity=%u", g_gc.handles.capacity);
    }
    
    GCHandle handle = g_gc.handles.count++;
    g_gc.handles.ptrs[handle] = ptr;
    return handle;
}

bool gc_ptr_is_valid(void *ptr) {
    if (!ptr) return false;
    if (!g_gc.initialized) return false;
    /* Check if pointer is within heap range */
    if ((uint8_t*)ptr < g_gc.heap || (uint8_t*)ptr >= g_gc.heap + g_gc.heap_size)
        return false;
    return true;
}

GCHandle gc_alloc(size_t size, JSGCObjectTypeEnum gc_obj_type) {
    return gc_alloc_ex(size, gc_obj_type, GC_HANDLE_ARRAY_GC);
}

GCHandle gc_alloc_ex(size_t size, JSGCObjectTypeEnum gc_obj_type,
                     GCHandleArrayType array_type) {
    if (!g_gc.initialized) return GC_HANDLE_NULL;
    
    void *ptr = bump_alloc(size);
    if (!ptr) {
        fprintf(stderr, "[FATAL] bump_alloc failed: out of memory (requested %zu bytes, heap size %zu, used %zu)\n",
                size, g_gc.heap_size, (size_t)g_gc.bump.offset);
        fprintf(stderr, "[FATAL] GC would have been triggered, but this is disabled to prevent handle corruption.\n");
        fflush(stderr);
        abort();
    }
    
    GCHeader *hdr = gc_header(ptr);
    hdr->gc_obj_type = gc_obj_type;
    
    GCHandle handle = allocate_handle(ptr);
    if (handle == GC_HANDLE_NULL) {
        hdr->size = 0;
        return GC_HANDLE_NULL;
    }
    hdr->handle = handle;
    g_gc.bytes_allocated += hdr->size;
    
    /* Auto-add to typed handle array based on array_type (weakrefs and atoms only) */
    JSHandleArray *target_array = NULL;
    switch (array_type) {
        case GC_HANDLE_ARRAY_WEAKREF:
            target_array = &g_gc.weakref_handles;
            break;
        case GC_HANDLE_ARRAY_ATOM:
            target_array = &g_gc.atom_handles;
            break;
        case GC_HANDLE_ARRAY_CONTEXT:
        case GC_HANDLE_ARRAY_JOB:
            /* These are now handled via type buckets and job_queue */
            break;
        case GC_HANDLE_ARRAY_GC:
        default:
            /* No special array needed */
            break;
    }
    if (target_array) {
        gc_handle_array_add(target_array, handle);
    }
    
    /* Auto-add to type bucket for fast iteration */
    GCObjectBucketType bucket_type = gc_type_to_bucket(gc_obj_type);
    if (bucket_type < GC_BUCKET_COUNT) {
        gc_type_bucket_add(&g_gc.type_buckets[bucket_type], handle);
    }
    
    return handle;
}

/*
 * gc_realloc - Reallocate memory while preserving the original handle.
 * 
 * This function allocates new memory, copies data from the old location,
 * and updates the original handle to point to the new memory. The old
 * memory is marked as freed and will be reclaimed by the GC.
 * 
 * Returns the original handle (now pointing to new memory), or GC_HANDLE_NULL on failure.
 */
GCHandle gc_realloc(GCHandle handle, size_t new_size) {
    if (handle == GC_HANDLE_NULL) {
        return gc_alloc(new_size, JS_GC_OBJ_TYPE_DATA);
    }
    
    if (new_size == 0) return GC_HANDLE_NULL;
    
    void *old_ptr = gc_deref(handle);
    if (!old_ptr) return gc_alloc(new_size, JS_GC_OBJ_TYPE_DATA);
    
    GCHeader *old_hdr = gc_header(old_ptr);
    JSGCObjectTypeEnum old_type = (JSGCObjectTypeEnum)old_hdr->gc_obj_type;
    size_t old_user_size = old_hdr->size;
    
    old_hdr->size = old_user_size | 0x80000000;
    
    /* Allocate new memory (gets a temporary handle) */
    GCHandle temp_handle = gc_alloc(new_size, old_type);
    if (temp_handle == GC_HANDLE_NULL) return GC_HANDLE_NULL;
    
    void *new_ptr = gc_deref(temp_handle);
    size_t copy_size = old_user_size < new_size ? old_user_size : new_size;
    memcpy(new_ptr, old_ptr, copy_size);
    
    /* Update the new memory's header to point to the ORIGINAL handle */
    GCHeader *new_hdr = gc_header(new_ptr);
    new_hdr->handle = handle;
    
    /* Update original handle entry to point to new memory */
    g_gc.handles.ptrs[handle] = new_ptr;
    
    /* Clear the temporary handle slot to prevent handle aliasing.
     * The temp_handle was allocated by gc_alloc above, but we only need
     * the original handle to point to the new memory. Without clearing
     * the temp slot, we'd have two handles pointing to the same memory,
     * which confuses the GC sweep phase.
     */
    g_gc.handles.ptrs[temp_handle] = NULL;
    push_free_handle(temp_handle);
    
    return handle;  /* Original handle, now pointing to new memory */
}

GCHandle gc_realloc2(GCHandle handle, size_t new_size, size_t *pslack) {
    GCHandle new_handle = gc_realloc(handle, new_size);
    if (pslack && new_handle != GC_HANDLE_NULL) {
        size_t usable = gc_usable_size(new_handle);
        *pslack = (usable > new_size) ? (usable - new_size) : 0;
    }
    return new_handle;
}

void *gc_deref(GCHandle handle) {
    if (handle == GC_HANDLE_NULL || handle >= g_gc.handles.count) {
        return NULL;
    }
    return g_gc.handles.ptrs[handle];
}

bool gc_handle_is_valid(GCHandle handle) {
    if (handle == GC_HANDLE_NULL || handle >= g_gc.handles.count) {
        return false;
    }
    void *ptr = g_gc.handles.ptrs[handle];
    if (!ptr) return false;
    GCHeader *hdr = gc_header(ptr);
    return (hdr->size & 0x7FFFFFFF) > 0;
}

JSGCObjectTypeEnum gc_handle_get_type(GCHandle handle) {
    return gc_handle_get_type_inline(handle);
}

/* Forward declarations */
static void gc_mark_value(GCValue val);
static void gc_mark_object(JSObject *p);

/* Helper to mark any GC object by pointer */
static void gc_mark_ptr(void *ptr) {
    if (!ptr) return;
    GCHeader *hdr = gc_header(ptr);
    if (hdr->size == 0 || hdr->mark) return;
    
    hdr->mark = 1;
    
    /* For JSVarRef, mark its value and the referenced frame (to keep parent frames alive) */
    if (hdr->gc_obj_type == JS_GC_OBJ_TYPE_VAR_REF) {
        JSVarRefHandle var_ref = JSVarRefHandle(hdr->handle);
        gc_mark_value(var_ref.get_detached_value());
        /* Mark the frame handle to keep parent frames alive for closures */
        GCHandle frame_handle = var_ref.frame_handle();
        if (frame_handle != GC_HANDLE_NULL) {
            /* Mark by setting the mark bit directly on the header */
            void *frame_ptr = gc_deref(frame_handle);
            if (frame_ptr) {
                GCHeader *frame_hdr = gc_header(frame_ptr);
                if ((frame_hdr->size & 0x7FFFFFFF) > 0 && !frame_hdr->mark) {
                    frame_hdr->mark = 1;
                }
            }
        }
    }
    /* Add other types as needed */
}

static void gc_mark_value(GCValue val) {
    int tag = JS_VALUE_GET_TAG(val);
    GCHandle handle = GC_VALUE_GET_HANDLE(val);
    switch(tag) {
    case JS_TAG_OBJECT:
    case JS_TAG_SYMBOL:
    case JS_TAG_STRING:
        {
            /* Use handle for safe marking - dereference only when needed */
            if (handle != GC_HANDLE_NULL) {
                void *ptr = gc_deref(handle);
                if (ptr) {
                    GCHeader *hdr = gc_header(ptr);
                    if ((hdr->size & 0x7FFFFFFF) > 0 && !hdr->mark) {
                        hdr->mark = 1;
                        if (tag == JS_TAG_OBJECT) {
                            gc_mark_object((JSObject*)ptr);
                        }
                        else if (tag == JS_TAG_STRING && hdr->gc_obj_type == JS_GC_OBJ_TYPE_JS_STRING_ROPE) {
                            JSStringRopeHandle rope(handle);
                            /* Mark left and right components of the rope */
                            gc_mark_value(rope.left());
                            gc_mark_value(rope.right());
                        }
                    }
                }
            }
        }
        break;
    }
}

static void gc_mark_object(JSObject *p) {
    if (!p) return;

    /* Mark the shape */
    JSShapeHandle shape = JSShapeHandle(p->shape_handle);
    if (shape.valid()) {
        GCHeader *shape_hdr = gc_header(gc_deref(shape.handle()));
        if ((shape_hdr->size & 0x7FFFFFFF) > 0) shape_hdr->mark = 1;
        
        /* Mark the shape's prototype handle - CRITICAL: shapes reference
         * prototype objects that must be kept alive */
        if (shape.proto_handle() != GC_HANDLE_NULL) {
            void *proto = gc_deref(shape.proto_handle());
            if (proto) {
                GCHeader *proto_hdr = gc_header(proto);
                if ((proto_hdr->size & 0x7FFFFFFF) > 0) proto_hdr->mark = 1;
            }
        }
    }

    /* Mark the object's prototype value */
    if (!JS_IsUndefined(p->prototype) && !JS_IsNull(p->prototype)) {
        gc_mark_value(p->prototype);
    }

    /* Mark properties */
    JSProperty *obj_props = (JSProperty*)gc_deref(p->prop_handle);
    if (shape && obj_props) {
        /* Shape properties (with flags) come right after JSShape header */
        JSShapeProperty *shape_props = (JSShapeProperty *)((uint8_t *)gc_deref(shape.handle()) + sizeof(JSShape));
        
        for (uint32_t i = 0; i < shape.prop_count(); i++) {
            JSShapeProperty *prs = &shape_props[i];
            JSProperty *pr = &obj_props[i];
            
            if (prs->atom == JS_ATOM_NULL) continue;
            
            /* Check property type using shape property flags */
            uint32_t prop_flags = prs->flags & JS_PROP_TMASK;
            if (prop_flags) {
                if (prop_flags == JS_PROP_GETSET) {
                    if (pr->u.getset.getter_handle != GC_HANDLE_NULL)
                        gc_mark_value(GC_MKHANDLE(JS_TAG_OBJECT, pr->u.getset.getter_handle));
                    if (pr->u.getset.setter_handle != GC_HANDLE_NULL)
                        gc_mark_value(GC_MKHANDLE(JS_TAG_OBJECT, pr->u.getset.setter_handle));
                } else if (prop_flags == JS_PROP_VARREF) {
                    /* Mark the var_ref if handle is non-null.
                     * CRITICAL: Don't use 'if (var_ref)' here because that calls valid()
                     * which may return false if the var_ref was previously unmarked.
                     * We need to mark it to keep it alive during this GC cycle. */
                    if (pr->u.var_ref_handle != GC_HANDLE_NULL) {
                        gc_mark_ptr(gc_deref(pr->u.var_ref_handle));
                    }
                }
                /* JS_PROP_AUTOINIT handled separately if needed */
            } else {
                /* JS_PROP_NORMAL - mark the value */
                gc_mark_value(pr->u.value);
            }
        }
    }
}

/* ============================================================================
 * UNIFIED GC BRIDGE - Integration with QuickJS comprehensive marking
 * ============================================================================ */

/* Bridge mark function - sets mark bit and recursively marks children via mark_children */
static void gc_unified_mark_recursive(JSRuntimeHandle rt, GCHandle handle);

static void gc_unified_mark_func(JSRuntimeHandle rt, GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return;
    
    /* Check if already marked */
    GCHeader *hdr = gc_header_from_handle(handle);
    if (!hdr) return;
    if ((hdr->size & 0x7FFFFFFF) == 0) return;  /* Already freed */
    if (hdr->mark) return;        /* Already marked */
    
    /* Set mark bit */
    hdr->mark = 1;
    
    /* Recursively mark children through QuickJS's mark_children function */
    /* This handles all object types: JSObject, JSShape, JSVarRef, JSFunctionBytecode, etc. */
    gc_unified_mark_recursive(rt, handle);
}

static void gc_unified_mark_recursive(JSRuntimeHandle rt, GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return;
    
    /* Skip uninitialized objects */
    JSGCObjectTypeEnum gc_obj_type = gc_handle_get_type_inline(handle);
    (void)gc_obj_type; /* Type is validated in mark_children */
    
    /* Call mark_children to recursively mark all children */
    /* mark_children is defined in quickjs.cpp and handles all object types */
    mark_children(rt, handle, gc_unified_mark_func);
}

/* Clear all mark bits before marking phase */
static void gc_clear_marks(void) {
    for (uint32_t i = 1; i < g_gc.handles.count; i++) {
        void *user_ptr = g_gc.handles.ptrs[i];
        if (user_ptr) {
            GCHeader *hdr = gc_header(user_ptr);
            if ((hdr->size & 0x7FFFFFFF) > 0) {
                hdr->mark = 0;
            }
        }
    }
}

/* Context marking callback */
static void gc_mark_context_callback(uint32_t ctx_handle, void *user_data) {
    JSRuntimeHandle rt = *(JSRuntimeHandle*)user_data;
    if (ctx_handle == GC_HANDLE_NULL) return;
    
    /* Mark the context itself */
    GCHeader *hdr = gc_header_from_handle(ctx_handle);
    if (hdr && (hdr->size & 0x7FFFFFFF) > 0 && !hdr->mark) {
        hdr->mark = 1;
        /* Mark all children of this context via mark_children */
        gc_unified_mark_recursive(rt, ctx_handle);
    }
}

/* Comprehensive mark phase - replaces the old gc_mark() */
static void gc_mark_comprehensive(JSRuntimeHandle rt) {
    /* Phase 1: Clear all marks */
    gc_clear_marks();
    
    /* Phase 2: Mark permanent atoms (Tier 1 roots) */
    for (uint32_t i = 0; i < g_gc.atom_handles.count; i++) {
        GCHandle atom_handle = g_gc.atom_handles.handles[i];
        if (gc_handle_array_entry_is_valid(atom_handle)) {
            GCHeader *hdr = gc_header_from_handle(atom_handle);
            if (hdr && (hdr->size & 0x7FFFFFFF) > 0 && !hdr->mark) {
                hdr->mark = 1;
            }
        }
    }
    
    /* Phase 3: Mark all contexts and their roots */
    /* This marks global objects, prototypes, constructors, shapes, etc. */
    JSRuntime_for_each_context(rt, gc_mark_context_callback, &rt);
    
    /* Phase 4: Mark the runtime's current_exception */
    GCValue exc = JSRuntime_get_current_exception(rt);
    GCHandle exc_handle = GC_VALUE_GET_HANDLE(exc);
    if (exc_handle != GC_HANDLE_NULL) {
        gc_unified_mark_func(rt, exc_handle);
    }
    
    /* Phase 5: Mark jobs in the job list */
    int job_count = JSRuntime_job_queue_count(rt);
    for (int i = 0; i < job_count; i++) {
        uint32_t job_handle = JSRuntime_job_queue_get_handle(rt, i);
        if (job_handle == GC_HANDLE_NULL) continue;
        
        /* Mark the job's realm */
        uint32_t realm_handle = JSJobEntry_get_realm_handle(job_handle);
        if (realm_handle != GC_HANDLE_NULL) {
            gc_unified_mark_func(rt, realm_handle);
        }
        
        /* Mark job arguments */
        int argc = JSJobEntry_get_argc(job_handle);
        for (int j = 0; j < argc; j++) {
            GCValue arg = JSJobEntry_get_argv(job_handle, j);
            GCHandle arg_handle = GC_VALUE_GET_HANDLE(arg);
            if (arg_handle != GC_HANDLE_NULL) {
                gc_unified_mark_func(rt, arg_handle);
            }
        }
    }
    
    /* Phase 6: Mark root set objects (user-added roots) */
    for (uint32_t i = 0; i < g_gc.root_set.count; i++) {
        GCHandle h = g_gc.root_set.roots[i];
        gc_unified_mark_func(rt, h);
    }
    
    /* Phase 7: Mark runtime handle arrays */
    uint32_t shape_hash_handle = JSRuntime_get_shape_hash_handle(rt);
    if (shape_hash_handle != GC_HANDLE_NULL) {
        gc_unified_mark_func(rt, shape_hash_handle);
    }
    
    uint32_t atom_array_handle = JSRuntime_get_atom_array_handle(rt);
    if (atom_array_handle != GC_HANDLE_NULL) {
        gc_unified_mark_func(rt, atom_array_handle);
    }
    
    /* Phase 8: Mark objects on the JS stack (active function calls) */
    JSStackFrame *sf = JSRuntime_get_current_stack_frame(rt);
    while (sf != NULL) {
        /* Mark the function object in this stack frame */
        GCValue cur_func = JSStackFrame_get_cur_func(sf);
        GCHandle func_handle = GC_VALUE_GET_HANDLE(cur_func);
        if (func_handle != GC_HANDLE_NULL) {
            gc_unified_mark_func(rt, func_handle);
        }
        
        /* Move to previous frame */
        sf = JSStackFrame_get_prev(sf);
    }
    
    /* Phase 9: Mark atoms referenced by shapes (Tier 2) */
    for (uint32_t i = 1; i < g_gc.handles.count; i++) {
        GCHandle handle = (GCHandle)i;
        if (!gc_handle_array_entry_is_valid(handle)) continue;
        if (gc_handle_is_freed(handle)) continue;
        
        if (gc_handle_get_type_inline(handle) == JS_GC_OBJ_TYPE_SHAPE) {
            /* Mark the shape itself first */
            gc_unified_mark_func(rt, handle);
        }
    }
}

/* Legacy gc_mark() - now redirects to comprehensive marking */
static void gc_mark(void) {
    if (g_gc.rt == GC_HANDLE_NULL) {
        /* No runtime set, fall back to simple root marking */
        gc_clear_marks();
        for (uint32_t i = 0; i < g_gc.root_set.count; i++) {
            GCHandle h = g_gc.root_set.roots[i];
            if (h < g_gc.handles.count && g_gc.handles.ptrs[h]) {
                void *ptr = g_gc.handles.ptrs[h];
                GCHeader *hdr = gc_header(ptr);
                if ((hdr->size & 0x7FFFFFFF) > 0 && !hdr->mark) {
                    hdr->mark = 1;
                    JSGCObjectTypeEnum obj_type = (JSGCObjectTypeEnum)hdr->gc_obj_type;
                    if (obj_type == JS_GC_OBJ_TYPE_JS_OBJECT) {
                        gc_mark_object((JSObject*)ptr);
                    }
                }
            }
        }
        return;
    }
    
    /* Use comprehensive marking with QuickJS bridge */
    JSRuntimeHandle rt(g_gc.rt);
    gc_mark_comprehensive(rt);
}

/*
 * Forwarding-based GC compaction
 * 
 * Phase 1: Build forwarding table - compute new locations for live objects
 * Phase 2: Update internal pointers using forwarding table
 * Phase 3: Move objects to new locations
 * Phase 4: Update handle table
 */

/* Helper: Get user pointer from GCHeader */
static inline void *gc_header_to_ptr(GCHeader *hdr) {
    return (uint8_t*)hdr + sizeof(GCHeader);
}

/* Helper: Get GCHeader from heap offset */
static inline GCHeader *gc_offset_to_header(size_t offset) {
    if (offset == 0 || offset >= g_gc.bump.offset) return NULL;
    uint64_t *prefix = (uint64_t*)(g_gc.heap + offset);
    if (*prefix == GC_CANARY_PREFIX || *prefix == GC_CANARY_CORRUPTED) {
        return (GCHeader*)(g_gc.heap + offset + 8);
    }
    return (GCHeader*)(g_gc.heap + offset);
}

/* Helper: Check if a pointer points into the GC heap */
static inline bool gc_ptr_in_heap(void *ptr) {
    if (!ptr || !g_gc.heap) return false;
    uint8_t *p = (uint8_t*)ptr;
    return p >= g_gc.heap && p < g_gc.heap + g_gc.heap_size;
}

/* Helper: Get heap offset from pointer */
static inline size_t gc_ptr_to_offset(void *ptr) {
    if (!gc_ptr_in_heap(ptr)) return 0;
    return (uint8_t*)ptr - g_gc.heap;
}

/* Helper: Forward a pointer if it points to a moved object */
static inline void *gc_forward_ptr(void *ptr) {
    if (!gc_ptr_in_heap(ptr)) return ptr;
    
    /* Find the header for this pointer */
    GCHeader *hdr = gc_header(ptr);
    if (!hdr || hdr->size == 0) return ptr;
    
    /* Check if forwarding pointer is set (stored in reserved1) */
    size_t fwd_offset = hdr->reserved1;
    if (fwd_offset != 0 && fwd_offset != (size_t)-1) {
        GCHeader *fwd_hdr = gc_offset_to_header(fwd_offset);
        if (fwd_hdr) {
            return gc_header_to_ptr(fwd_hdr);
        }
    }
    return ptr;
}

static void gc_compact_build_forwarding_table(uint8_t **new_write_ptr) {
    /* Handle table is now allocated separately, start from heap beginning */
    uint8_t *read = g_gc.heap;
    uint8_t *write = read;
    size_t bump = g_gc.bump.offset;
    
    while ((size_t)(read - g_gc.heap) < bump) {
        uint64_t *prefix_ptr = (uint64_t*)read;
        GCHeader *hdr;
        
        if (*prefix_ptr == GC_CANARY_PREFIX || *prefix_ptr == GC_CANARY_CORRUPTED) {
            hdr = (GCHeader*)(read + 8);
        }
        
        uint32_t raw_size = hdr->size;
        int is_freed = (raw_size & 0x80000000) != 0;
        
        if (raw_size == 0) {
            read += MIN_OBJECT_SIZE;
            continue;
        }
        
        size_t total_size = gc_alloc_total_size(hdr);
        
        if (is_freed) {
            read += total_size;
            continue;
        }
        
        if (hdr->mark) {
            /* Store forwarding info: reserved1 = new offset */
            hdr->reserved1 = (uint64_t)(write - g_gc.heap);
            write += total_size;
        } else {
            /* Dead object - clear reserved1 */
            hdr->reserved1 = 0;
        }
        
        read += total_size;
    }
    
    *new_write_ptr = write;
}

static void gc_compact_update_pointers(void) {
    /* Handle table is now allocated separately, start from heap beginning */
    uint8_t *read = g_gc.heap;
    size_t bump = g_gc.bump.offset;
    
    while ((size_t)(read - g_gc.heap) < bump) {
        uint64_t *prefix_ptr = (uint64_t*)read;
        GCHeader *hdr;
        
        if (*prefix_ptr == GC_CANARY_PREFIX || *prefix_ptr == GC_CANARY_CORRUPTED) {
            hdr = (GCHeader*)(read + 8);
        }
        
        uint32_t raw_size = hdr->size;
        int is_freed = (raw_size & 0x80000000) != 0;
        
        if (raw_size == 0 || is_freed) {
            size_t total_size = gc_alloc_total_size(hdr);
            read += total_size;
            continue;
        }
        
        size_t total_size = gc_alloc_total_size(hdr);
        
        if (hdr->mark) {
            /* Update internal pointers based on object type */
            JSGCObjectTypeEnum obj_type = (JSGCObjectTypeEnum)hdr->gc_obj_type;
            void *user_ptr = gc_header_to_ptr(hdr);
            
            switch (obj_type) {
            case JS_GC_OBJ_TYPE_JS_OBJECT: {
                JSObject *p = (JSObject*)user_ptr;
                /* Update shape handle pointer if stored directly */
                /* Shape is accessed via handle, not direct pointer - no update needed */
                /* Update prototype value if it's a pointer */
                if (!JS_IsUndefined(p->prototype) && !JS_IsNull(p->prototype)) {
                    int tag = JS_VALUE_GET_TAG(p->prototype);
                    if (tag == JS_TAG_OBJECT || tag == JS_TAG_STRING || tag == JS_TAG_SYMBOL) {
                        /* Safe handle-based forwarding during compaction */
                        GCHandle old_handle = GC_VALUE_GET_HANDLE(p->prototype);
                        void *old_ptr = gc_deref(old_handle);
                        void *new_ptr = gc_forward_ptr(old_ptr);
                        if (new_ptr != old_ptr) {
                            /* Get handle from new pointer's GC header */
                            GCHeader *new_hdr = gc_header(new_ptr);
                            p->prototype = GC_MKHANDLE(tag, new_hdr->handle);
                        }
                    }
                }
                /* Property values are GCValue which use handles - no update needed */
                break;
            }
            case JS_GC_OBJ_TYPE_VAR_REF: {
                /* JSVarRef uses dynamic pointer resolution - no update needed */
                break;
            }
            case JS_GC_OBJ_TYPE_JS_STRING_ROPE: {
                /* Get handle directly from GC header */
                JSStringRopeHandle rope(hdr->handle);
                /* Rope components are GCValue - handled via handles */
                break;
            }
            /* Add other object types as needed */
            default:
                break;
            }
        }
        
        read += total_size;
    }
}

static void gc_compact_move_objects(uint8_t *new_end) {
    /* Handle table is now allocated separately, start from heap beginning */
    uint8_t *read = g_gc.heap;
    uint8_t *write = read;
    size_t bump = g_gc.bump.offset;
    JSRuntimeHandle rt(g_gc.rt);
    size_t new_bytes = 0;
    int corrupted_objects = 0;
    (void)corrupted_objects;
    
    while ((size_t)(read - g_gc.heap) < bump) {
        uint64_t *prefix_ptr = (uint64_t*)read;
        GCHeader *hdr;
        
        if (*prefix_ptr == GC_CANARY_PREFIX || *prefix_ptr == GC_CANARY_CORRUPTED) {
            hdr = (GCHeader*)(read + 8);
        }        

        uint32_t raw_size = hdr->size;
        int is_freed = (raw_size & 0x80000000) != 0;
        void *user_ptr = gc_header_to_ptr(hdr);
        
        if (raw_size == 0) {
            read += MIN_OBJECT_SIZE;
            continue;
        }
        
        size_t total_size = gc_alloc_total_size(hdr);
        
        if (is_freed) {
            read += total_size;
            continue;
        }
        
        GCCanaryStatus status = gc_validate_canaries_hdr(hdr, NULL);
        if (status != GC_CANARY_OK) {
            corrupted_objects++;
            read += total_size;
            continue;
        }
        
        if (hdr->mark) {
            /* Get target location from forwarding info */
            size_t target_offset = (size_t)hdr->reserved1;
            uint8_t *target = g_gc.heap + target_offset;
            
            if (read != target) {
                memmove(target, read, total_size);
            }
            
            /* Update handle table */
            GCHeader *new_hdr = (GCHeader*)(target + 8);
            if (new_hdr->handle < g_gc.handles.count) {
                g_gc.handles.ptrs[new_hdr->handle] = gc_header_to_ptr(new_hdr);
            }
            
            /* Clear forwarding info */
            new_hdr->reserved1 = 0;
            new_hdr->mark = 0;
            
            if (target == write) {
                write += total_size;
                new_bytes += total_size;
            }
        } else {
            /* Object is being freed */
            if (hdr->handle < g_gc.handles.count) {
                if (hdr->finalizer && status == GC_CANARY_OK) {
                    hdr->finalizer(rt, hdr->handle, user_ptr);
                }
                g_gc.handles.ptrs[hdr->handle] = NULL;
                push_free_handle(hdr->handle);
            }
            gc_corrupt_canaries(hdr);
        }
        
        read += total_size;
    }
    
    g_gc.bump.offset = new_end - g_gc.heap;
    g_gc.bytes_allocated = new_bytes;
}

static void gc_compact(void) {
    uint8_t *new_end;
    
    /* Phase 1: Build forwarding table */
    gc_compact_build_forwarding_table(&new_end);
    
    /* Phase 2: Update internal pointers */
    gc_compact_update_pointers();
    
    /* Phase 3: Move objects and update handle table */
    gc_compact_move_objects(new_end);
}

/* Sweep phase - free unmarked objects */
static void gc_sweep_unified(JSRuntimeHandle rt) {
    GC_LOGI("gc_sweep_unified: ENTER, handles.count=%u", g_gc.handles.count);
    
    for (uint32_t i = 1; i < g_gc.handles.count; i++) {
        GCHandle handle = (GCHandle)i;
        void *user_ptr = g_gc.handles.ptrs[i];
        
        if (!user_ptr) continue;
        
        /* Safety check: user_ptr must point to valid memory in GC heap */
        if (!gc_ptr_is_valid(user_ptr)) {
            GC_LOGW("gc_sweep_unified: entry %d user_ptr=%p not in GC heap, clearing", i, user_ptr);
            g_gc.handles.ptrs[i] = NULL;
            push_free_handle((GCHandle)i);
            continue;
        }
        
        GCHeader *hdr = gc_header(user_ptr);
        
        /* Check if already freed */
        if (hdr->size == 0) {
            g_gc.handles.ptrs[i] = NULL;
            push_free_handle((GCHandle)i);
            continue;
        }
        
        /* Check if marked */
        if (!hdr->mark) {
            /* Object is unreachable - call finalizer */
            if (hdr->finalizer) {
                hdr->finalizer(rt, handle, user_ptr);
            }
            /* Note: Type bucket removal skipped - buckets are unused and
             * removing during sweep is O(n^2) which causes hangs with
             * large numbers of objects. Compaction handles stale entries. */
            /* Mark as freed - set FREED flag but keep size for compaction */
            hdr->size |= 0x80000000;  /* Set FREED flag */
            gc_corrupt_canaries(hdr);
            g_gc.handles.ptrs[i] = NULL;
            push_free_handle((GCHandle)i);
        }
    }
    
    GC_LOGI("gc_sweep_unified: DONE");
}

static void gc_run_internal(void) {
    if (!g_gc.initialized) return;
    
    JSRuntimeHandle rt(g_gc.rt);
    bool has_runtime = (g_gc.rt != GC_HANDLE_NULL);
    
    GC_LOGI("gc_run_internal: ENTER has_runtime=%d", has_runtime);
    
    if (has_runtime) {
        /* Phase 0: Remove weak objects (WeakMap, WeakSet, WeakRef) */
        /* This must happen BEFORE marking so weak refs are properly cleared */
        GC_LOGI("gc_run_internal: Phase 0 - removing weak objects");
        gc_remove_weak_objects(rt);
    }
    
    /* Phase 1: Mark phase - mark all reachable objects */
    GC_LOGI("gc_run_internal: Phase 1 - marking");
    gc_mark();
    
    if (has_runtime) {
        /* Phase 2: Clean up shape hash table before compaction */
        GC_LOGI("gc_run_internal: Phase 2 - cleaning shape hash table");
        gc_cleanup_shape_hash_table(rt);
        
        /* Phase 3: Sweep phase - free unmarked objects */
        GC_LOGI("gc_run_internal: Phase 3 - sweeping");
        gc_sweep_unified(rt);
        
        /* Phase 4: Sweep atoms */
        GC_LOGI("gc_run_internal: Phase 4 - sweeping atoms");
        gc_sweep_atoms(rt);
    }
    
    /* Phase 5: Compact phase - move live objects and update handles */
    GC_LOGI("gc_run_internal: Phase 5 - compacting");
    gc_compact();
    
    /* Phase 6: Compact typed handle arrays */
    GC_LOGI("gc_run_internal: Phase 6 - compacting handle arrays");
    gc_handle_array_compact(&g_gc.weakref_handles);
    gc_handle_array_compact(&g_gc.finrec_handles);
    gc_handle_array_compact(&g_gc.atom_handles);
    
    GC_LOGI("gc_run_internal: DONE");
}

static void gc_maybe_run(void) {
    if (g_gc.bytes_allocated > g_gc.gc_threshold) {
        gc_run_internal();
    }
}

void gc_run(void) {
    gc_run_internal();
}

void gc_reset(void) {
    if (!g_gc.initialized) return;
    
    /* Handle table is now allocated separately, start from heap beginning */
    g_gc.bump.offset = 0;
    
    for (uint32_t i = 1; i < g_gc.handles.count; i++) {
        g_gc.handles.ptrs[i] = NULL;
    }
    g_gc.handles.count = 1;
    g_gc.handles.free_count = 0;
    
    /* Reset root set - keep the allocated array but reset count */
    g_gc.root_set.count = 0;
    
    g_gc.bytes_allocated = 0;
}

void gc_reset_full(void) {
    browser_api_impl_reset();
    js_quickjs_reset_class_ids();
    gc_cleanup();
    gc_init();
}

bool gc_add_root(GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return true;
    
    /* Resize root set if needed */
    if (g_gc.root_set.count >= g_gc.root_set.capacity) {
        uint32_t new_capacity = g_gc.root_set.capacity * 2;
        GCHandle *new_roots = (GCHandle*)realloc(g_gc.root_set.roots, 
                                                  new_capacity * sizeof(GCHandle));
        if (!new_roots) return false;  /* Failed to resize, can't add root */
        g_gc.root_set.roots = new_roots;
        g_gc.root_set.capacity = new_capacity;
    }
    
    g_gc.root_set.roots[g_gc.root_set.count++] = handle;
    return true;
}

void gc_remove_root(GCHandle handle) {
    for (uint32_t i = 0; i < g_gc.root_set.count; i++) {
        if (g_gc.root_set.roots[i] == handle) {
            g_gc.root_set.roots[i] = g_gc.root_set.roots[--g_gc.root_set.count];
            return;
        }
    }
}

size_t gc_used_bytes(void) {
    if (!g_gc.initialized) return 0;
    return g_gc.bump.offset;
}

size_t gc_available_bytes(void) {
    if (!g_gc.initialized) return 0;
    return g_gc.heap_size - g_gc.bump.offset;
}

size_t gc_total_bytes(void) {
    return g_gc.heap_size;
}

/* ============================================================================
 * CANARY VALIDATION FOR HEAP CORRUPTION DETECTION
 * ============================================================================
 * 
 * Canaries are placed before and after each allocation to detect buffer
 * overflows and memory corruption. The canary layout is:
 * 
 * [PREFIX CANARY: 8 bytes] [GCHeader: 64 bytes] [user data] [SUFFIX CANARY: 8 bytes]
 * 
 * Both canaries are checked at:
 * - Allocation time (to catch use-after-free)
 * - Reallocation time
 * - GC compaction time
 * - Explicit validation calls
 */

/* Get pointer to prefix canary (8 bytes before GCHeader) */
static inline uint64_t *gc_canary_prefix_ptr(GCHeader *hdr) {
    if (!hdr) return NULL;
    return (uint64_t*)((uint8_t*)hdr - 8);
}

/* Get pointer to suffix canary (after user data) */
static inline uint64_t *gc_canary_suffix_ptr(GCHeader *hdr) {
    if (!hdr) return NULL;
    /* With hdr->size = user_size:
     * suffix is at hdr + sizeof(GCHeader) + user_size
     * Note: mask out FREED flag (bit 31) if set
     */
    uint32_t user_size = hdr->size & 0x7FFFFFFF;
    uint8_t *suffix = (uint8_t*)hdr + sizeof(GCHeader) + user_size;
    return (uint64_t*)suffix;
}

/* Set canaries for a new allocation */
static inline void gc_set_canaries(GCHeader *hdr) {
    if (!hdr) return;
    uint64_t *prefix = gc_canary_prefix_ptr(hdr);
    uint64_t *suffix = gc_canary_suffix_ptr(hdr);
    if (prefix) *prefix = GC_CANARY_PREFIX;
    if (suffix) *suffix = GC_CANARY_SUFFIX;
}

/* Corrupt canaries (called when freeing to catch use-after-free) */
static inline void gc_corrupt_canaries(GCHeader *hdr) {
    if (!hdr) return;
    uint64_t *prefix = gc_canary_prefix_ptr(hdr);
    uint64_t *suffix = gc_canary_suffix_ptr(hdr);
    if (prefix) *prefix = GC_CANARY_CORRUPTED;
    if (suffix) *suffix = GC_CANARY_CORRUPTED;
}

/* Validate canaries for a raw GCHeader pointer */
static GCCanaryStatus gc_validate_canaries_hdr(GCHeader *hdr, void **out_ptr) {
    if (!hdr) return GC_CANARY_NULL_POINTER;
    
    /* Check if handle is valid */
    if (hdr->handle == GC_HANDLE_NULL || hdr->handle >= g_gc.handles.count) {
        return GC_CANARY_INVALID_HANDLE;
    }
    
    /* Check prefix canary */
    uint64_t *prefix = gc_canary_prefix_ptr(hdr);
    if (prefix && *prefix != GC_CANARY_PREFIX) {
        if (out_ptr) *out_ptr = (uint8_t*)hdr + sizeof(GCHeader);
        return GC_CANARY_CORRUPTED_PREFIX;
    }
    
    /* Check suffix canary */
    uint64_t *suffix = gc_canary_suffix_ptr(hdr);
    if (suffix && *suffix != GC_CANARY_SUFFIX) {
        if (out_ptr) *out_ptr = (uint8_t*)hdr + sizeof(GCHeader);
        return GC_CANARY_CORRUPTED_SUFFIX;
    }
    
    if (out_ptr) *out_ptr = (uint8_t*)hdr + sizeof(GCHeader);
    return GC_CANARY_OK;
}

/* Public API: Validate canaries for a handle */
GCCanaryStatus gc_validate_canaries(GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return GC_CANARY_INVALID_HANDLE;
    if (handle >= g_gc.handles.count) return GC_CANARY_INVALID_HANDLE;
    
    void *ptr = g_gc.handles.ptrs[handle];
    if (!ptr) return GC_CANARY_NULL_POINTER;
    
    GCHeader *hdr = gc_header(ptr);
    return gc_validate_canaries_hdr(hdr, NULL);
}

/* Public API: Validate canaries for a pointer */
GCCanaryStatus gc_validate_canaries_ptr(void *ptr) {
    if (!ptr) return GC_CANARY_NULL_POINTER;
    GCHeader *hdr = gc_header(ptr);
    return gc_validate_canaries_hdr(hdr, NULL);
}

/* Get string description of canary status */
const char *gc_canary_status_string(GCCanaryStatus status) {
    switch (status) {
        case GC_CANARY_OK: return "OK";
        case GC_CANARY_CORRUPTED_PREFIX: return "CORRUPTED_PREFIX (buffer underflow or header corruption)";
        case GC_CANARY_CORRUPTED_SUFFIX: return "CORRUPTED_SUFFIX (buffer overflow)";
        case GC_CANARY_INVALID_HANDLE: return "INVALID_HANDLE";
        case GC_CANARY_NULL_POINTER: return "NULL_POINTER";
        default: return "UNKNOWN";
    }
}

/* Get type name for debugging */
static const char *gc_obj_type_name(JSGCObjectTypeEnum type) {
    switch (type) {
        case JS_GC_OBJ_TYPE_JS_OBJECT: return "JSObject";
        case JS_GC_OBJ_TYPE_FUNCTION_BYTECODE: return "FunctionBytecode";
        case JS_GC_OBJ_TYPE_SHAPE: return "JSShape";
        case JS_GC_OBJ_TYPE_VAR_REF: return "JSVarRef";
        case JS_GC_OBJ_TYPE_ASYNC_FUNCTION: return "JSAsyncFunction";
        case JS_GC_OBJ_TYPE_JS_CONTEXT: return "JSContext";
        case JS_GC_OBJ_TYPE_JS_RUNTIME: return "JSRuntime";
        case JS_GC_OBJ_TYPE_MODULE: return "JSModule";
        /* case JS_GC_OBJ_TYPE_JOB_ENTRY: return "JobEntry"; */
        case JS_GC_OBJ_TYPE_JS_STRING: return "JSString";
        case JS_GC_OBJ_TYPE_JS_STRING_ROPE: return "JSStringRope";
        case JS_GC_OBJ_TYPE_JS_BIGINT: return "JSBigInt";
        case JS_GC_OBJ_TYPE_DATA: return "Data";
        default: return "Unknown";
    }
}

/* Print detailed corruption info */
void gc_print_corruption_info(GCHandle handle, GCCanaryStatus status) {
    fprintf(stderr, "\n");
    fprintf(stderr, "=================================================\n");
    fprintf(stderr, "GC CANARY CORRUPTION DETECTED!\n");
    fprintf(stderr, "=================================================\n");
    fprintf(stderr, "Handle: %u\n", handle);
    fprintf(stderr, "Status: %s\n", gc_canary_status_string(status));
    
    if (handle == GC_HANDLE_NULL || handle >= g_gc.handles.count) {
        fprintf(stderr, "Invalid handle, cannot print details\n");
        fprintf(stderr, "=================================================\n");
        return;
    }
    
    void *ptr = g_gc.handles.ptrs[handle];
    if (!ptr) {
        fprintf(stderr, "Pointer is NULL\n");
        fprintf(stderr, "=================================================\n");
        return;
    }
    
    GCHeader *hdr = gc_header(ptr);
    fprintf(stderr, "Object Type: %s (%d)\n", gc_obj_type_name((JSGCObjectTypeEnum)hdr->gc_obj_type), hdr->gc_obj_type);
    fprintf(stderr, "Object Size: %u bytes\n", hdr->size);
    fprintf(stderr, "Object Address: %p\n", ptr);
    fprintf(stderr, "Header Address: %p\n", (void*)hdr);
    
    /* Show canary values */
    uint64_t *prefix = gc_canary_prefix_ptr(hdr);
    uint64_t *suffix = gc_canary_suffix_ptr(hdr);
    
    if (prefix) {
        fprintf(stderr, "\nPrefix Canary (expected 0x%016llx):\n", (unsigned long long)GC_CANARY_PREFIX);
        fprintf(stderr, "  Actual: 0x%016llx\n", (unsigned long long)*prefix);
        fprintf(stderr, "  Offset: %p\n", (void*)prefix);
        
        /* Try to interpret as ASCII */
        char *ascii = (char*)prefix;
        fprintf(stderr, "  ASCII: ");
        for (int i = 7; i >= 0; i--) {
            unsigned char c = ascii[i];
            fprintf(stderr, "%c", (c >= 32 && c < 127) ? c : '.');
        }
        fprintf(stderr, "\n");
    }
    
    if (suffix) {
        fprintf(stderr, "\nSuffix Canary (expected 0x%016llx):\n", (unsigned long long)GC_CANARY_SUFFIX);
        fprintf(stderr, "  Actual: 0x%016llx\n", (unsigned long long)*suffix);
        fprintf(stderr, "  Offset: %p\n", (void*)suffix);
        
        /* Try to interpret as ASCII */
        char *ascii = (char*)suffix;
        fprintf(stderr, "  ASCII: ");
        for (int i = 7; i >= 0; i--) {
            unsigned char c = ascii[i];
            fprintf(stderr, "%c", (c >= 32 && c < 127) ? c : '.');
        }
        fprintf(stderr, "\n");
    }
    
    /* Show hex dump of surrounding memory for suffix corruption */
    if (status == GC_CANARY_CORRUPTED_SUFFIX && suffix) {
        fprintf(stderr, "\nMemory dump around corruption:\n");
        uint8_t *dump_start = (uint8_t*)suffix - 64;
        for (int i = 0; i < 80; i += 16) {
            fprintf(stderr, "  %p: ", (void*)(dump_start + i));
            for (int j = 0; j < 16; j++) {
                fprintf(stderr, "%02x ", dump_start[i + j]);
            }
            fprintf(stderr, " |");
            for (int j = 0; j < 16; j++) {
                unsigned char c = dump_start[i + j];
                fprintf(stderr, "%c", (c >= 32 && c < 127) ? c : '.');
            }
            fprintf(stderr, "|\n");
            if (i == 64) fprintf(stderr, "  --> SUFFIX CANARY HERE <--\n");
        }
    }
    
    fprintf(stderr, "=================================================\n");
    fprintf(stderr, "\n");
}

/* Validate all canaries in the heap */
int gc_validate_all_canaries(bool verbose) {
    if (!g_gc.initialized) return 0;
    
    int corrupted_count = 0;
    
    for (uint32_t i = 1; i < g_gc.handles.count; i++) {
        void *ptr = g_gc.handles.ptrs[i];
        if (!ptr) continue;
        
        GCHeader *hdr = gc_header(ptr);
        GCCanaryStatus status = gc_validate_canaries_hdr(hdr, NULL);
        
        if (status != GC_CANARY_OK) {
            corrupted_count++;
            if (verbose) {
                gc_print_corruption_info(i, status);
            }
        }
    }
    
    return corrupted_count;
}

/* Check canaries and abort if corrupted (for debugging) */
void gc_check_canaries_or_abort(GCHandle handle, const char *location) {
    GCCanaryStatus status = gc_validate_canaries(handle);
    if (status != GC_CANARY_OK) {
        fprintf(stderr, "\n*** CANARY CHECK FAILED at %s ***\n", location ? location : "unknown");
        gc_print_corruption_info(handle, status);
        abort();
    }
}

/* ============================================================================
 * DIAGNOSTIC FUNCTIONS FOR QUICKJS INTEGRATION
 * ============================================================================
 */

/* 
 * Validate a shape object by handle
 * This is called from quickjs.c when shape corruption is suspected
 */
GCCanaryStatus gc_validate_shape_canaries(uint32_t shape_handle) {
    if (shape_handle == GC_HANDLE_NULL) {
        return GC_CANARY_INVALID_HANDLE;
    }
    return gc_validate_canaries(shape_handle);
}

/*
 * Diagnose corruption around a specific object
 * This dumps information about neighboring objects to help identify
 * which object caused the overflow
 */
void gc_diagnose_corruption_context(GCHandle handle) {
    if (handle == GC_HANDLE_NULL || handle >= g_gc.handles.count) {
        fprintf(stderr, "[DIAGNOSE] Invalid handle %u\n", handle);
        return;
    }
    
    void *ptr = g_gc.handles.ptrs[handle];
    if (!ptr) {
        fprintf(stderr, "[DIAGNOSE] Handle %u has NULL pointer\n", handle);
        return;
    }
    
    GCHeader *hdr = gc_header(ptr);
    fprintf(stderr, "\n[DIAGNOSE] Corruption context for handle %u:\n", handle);
    fprintf(stderr, "  Object address: %p\n", ptr);
    fprintf(stderr, "  Header address: %p\n", (void*)hdr);
    fprintf(stderr, "  Object size: %u\n", hdr->size);
    fprintf(stderr, "  Object type: %s\n", gc_obj_type_name((JSGCObjectTypeEnum)hdr->gc_obj_type));
    
    /* Show nearby handles */
    fprintf(stderr, "\n  Nearby objects:\n");
    int start = (int)handle - 5;
    if (start < 1) start = 1;
    int end = (int)handle + 5;
    if (end > (int)g_gc.handles.count) end = (int)g_gc.handles.count;
    
    for (int i = start; i < end; i++) {
        void *near_ptr = g_gc.handles.ptrs[i];
        if (near_ptr) {
            GCHeader *near_hdr = gc_header(near_ptr);
            GCCanaryStatus status = gc_validate_canaries(i);
            const char *status_str = (status == GC_CANARY_OK) ? "OK" : "CORRUPTED";
            fprintf(stderr, "    Handle %d: %s (size=%u, type=%s) [%s]\n",
                    i, status_str, near_hdr->size, 
                    gc_obj_type_name((JSGCObjectTypeEnum)near_hdr->gc_obj_type),
                    (i == (int)handle) ? "<-- TARGET" : "");
        } else {
            fprintf(stderr, "    Handle %d: FREE\n", i);
        }
    }
    
    /* Validate all canaries to find all corruption */
    fprintf(stderr, "\n  Scanning all objects for corruption...\n");
    int total_corrupted = gc_validate_all_canaries(false);
    fprintf(stderr, "  Total corrupted objects: %d\n", total_corrupted);
    
    fprintf(stderr, "\n");
}

/*
 * Emergency heap dump for post-mortem analysis
 * Dumps all objects and their canary status to a file
 */
void gc_dump_heap_for_analysis(const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "[DUMP] Failed to open %s for writing\n", filename);
        return;
    }
    
    fprintf(f, "GC Heap Dump\n");
    fprintf(f, "============\n\n");
    fprintf(f, "Total handles: %u\n", g_gc.handles.count);
    fprintf(f, "Used bytes: %zu\n", gc_used_bytes());
    fprintf(f, "\n");
    
    for (uint32_t i = 1; i < g_gc.handles.count; i++) {
        void *ptr = g_gc.handles.ptrs[i];
        if (!ptr) {
            fprintf(f, "Handle %u: FREE\n", i);
            continue;
        }
        
        GCHeader *hdr = gc_header(ptr);
        GCCanaryStatus status = gc_validate_canaries(i);
        
        fprintf(f, "Handle %u:\n", i);
        fprintf(f, "  Address: %p\n", ptr);
        fprintf(f, "  Size: %u\n", hdr->size);
        fprintf(f, "  Type: %s (%d)\n", gc_obj_type_name((JSGCObjectTypeEnum)hdr->gc_obj_type), hdr->gc_obj_type);
        fprintf(f, "  Mark: %d\n", hdr->mark);
        fprintf(f, "  Canary Status: %s\n", gc_canary_status_string(status));
        
        if (status != GC_CANARY_OK) {
            uint64_t *prefix = gc_canary_prefix_ptr(hdr);
            uint64_t *suffix = gc_canary_suffix_ptr(hdr);
            if (prefix) {
                fprintf(f, "  Prefix: 0x%016llx (expected 0x%016llx)\n",
                        (unsigned long long)*prefix, (unsigned long long)GC_CANARY_PREFIX);
            }
            if (suffix) {
                fprintf(f, "  Suffix: 0x%016llx (expected 0x%016llx)\n",
                        (unsigned long long)*suffix, (unsigned long long)GC_CANARY_SUFFIX);
            }
        }
        fprintf(f, "\n");
    }
    
    fclose(f);
    fprintf(stderr, "[DUMP] Heap dump written to %s\n", filename);
}

/* ============================================================================
 * GC-Safe Linked List Implementations
 * ============================================================================
 * 
 * These functions implement doubly-linked lists using GCHandles instead of
 * raw pointers, making them safe to use with the compacting garbage collector.
 */

/* Helper to get GCListHead* from handle and offset */
static inline GCListHead* gc_list_head_from_handle(GCHandle handle, size_t offset) {
    if (handle == GC_HANDLE_NULL) return nullptr;
    void* ptr = gc_deref(handle);
    return ptr ? (GCListHead*)((uint8_t*)ptr + offset) : nullptr;
}

/* Add a node between two existing nodes */
void gc_list_add_between(GCHandle new_node,
                         GCHandle prev, GCHandle next,
                         size_t link_offset) {
    GCListHead *new_link = gc_list_head_from_handle(new_node, link_offset);
    if (!new_link) return;
    
    new_link->prev = prev;
    new_link->next = next;
    
    if (prev != GC_HANDLE_NULL) {
        GCListHead *prev_link = gc_list_head_from_handle(prev, link_offset);
        if (prev_link) prev_link->next = new_node;
    }
    
    if (next != GC_HANDLE_NULL) {
        GCListHead *next_link = gc_list_head_from_handle(next, link_offset);
        if (next_link) next_link->prev = new_node;
    }
}

/* Add node at the head of the list */
void gc_list_add(GCHandle new_node, struct GCListHead *head,
                 size_t link_offset) {
    if (!head || new_node == GC_HANDLE_NULL) return;
    
    GCHandle first = head->next;
    gc_list_add_between(new_node, GC_HANDLE_NULL, first, link_offset);
    
    head->next = new_node;
    if (head->prev == GC_HANDLE_NULL) {
        head->prev = new_node;
    }
}

/* Add node at the tail of the list */
void gc_list_add_tail(GCHandle new_node, struct GCListHead *head,
                      size_t link_offset) {
    if (!head || new_node == GC_HANDLE_NULL) return;
    
    GCHandle last = head->prev;
    gc_list_add_between(new_node, last, GC_HANDLE_NULL, link_offset);
    
    head->prev = new_node;
    if (head->next == GC_HANDLE_NULL) {
        head->next = new_node;
    }
}

/* Delete a node from the list */
void gc_list_del(GCHandle node, struct GCListHead *head,
                 size_t link_offset) {
    if (!head || node == GC_HANDLE_NULL) return;
    
    GCListHead *link = gc_list_head_from_handle(node, link_offset);
    if (!link) return;
    
    GCHandle prev = link->prev;
    GCHandle next = link->next;
    
    if (prev != GC_HANDLE_NULL) {
        GCListHead *prev_link = gc_list_head_from_handle(prev, link_offset);
        if (prev_link) prev_link->next = next;
    } else {
        /* This was the first node */
        head->next = next;
    }
    
    if (next != GC_HANDLE_NULL) {
        GCListHead *next_link = gc_list_head_from_handle(next, link_offset);
        if (next_link) next_link->prev = prev;
    } else {
        /* This was the last node */
        head->prev = prev;
    }
    
    /* Clear the node's links */
    link->prev = GC_HANDLE_NULL;
    link->next = GC_HANDLE_NULL;
}

/* Add node at the tail of a list in a container (handle-based - GC-safe) */
void gc_list_add_tail_in_container(GCHandle new_node, GCHandle container,
                                   size_t list_offset, size_t link_offset) {
    if (container == GC_HANDLE_NULL || new_node == GC_HANDLE_NULL) return;
    
    /* Get the list head pointer temporarily - safe as long as no GC happens here */
    GCListHead *head = (GCListHead *)((uint8_t*)gc_deref(container) + list_offset);
    if (!head) return;
    
    gc_list_add_tail(new_node, head, link_offset);
}

/* Delete a node from a list in a container (handle-based - GC-safe) */
void gc_list_del_in_container(GCHandle node, GCHandle container,
                              size_t list_offset, size_t link_offset) {
    if (container == GC_HANDLE_NULL || node == GC_HANDLE_NULL) return;
    
    /* Get the list head pointer temporarily - safe as long as no GC happens here */
    GCListHead *head = (GCListHead *)((uint8_t*)gc_deref(container) + list_offset);
    if (!head) return;
    
    gc_list_del(node, head, link_offset);
}

/*
 * QuickJS Handle Classes
 * 
 * These C++ classes provide GC-safe access to JSContext and JSRuntime.
 * They are separated into this header to be included after GCValue definitions.
 * 
 * ============================================================================
 * GC SAFETY MIGRATION NOTICE (Phase 1 in progress)
 * ============================================================================
 * 
 * This codebase is migrating away from raw pointer usage to handle-based APIs
 * to ensure safety with compacting garbage collection. Raw pointers can become
 * stale after GC compaction, while handles (GCHandle) remain valid.
 * 
 * MIGRATION STATUS:
 * - get_raw_ptr() methods have been REMOVED (Phase 2)
 * - Use handle-based accessor methods instead (e.g., record_count(), key())
 * - Methods marked __unsafe_internal_ptr() are for internal use only
 * 
 * SAFE PATTERNS:
 *   JSMapStateHandle s(s_handle);
 *   s.inc_record_count();  // Safe: uses fresh dereference
 *   
 *   JSMapRecordHandle mr(mr_handle);
 *   GCValue k = mr.key();  // Safe: uses fresh dereference
 * 
 * INTERNAL USE ONLY (for performance-critical paths):
 *   JSMapState* s = s.__unsafe_internal_ptr();
 *   s->record_count++;  // OK: pointer used immediately, not stored
 * 
 * For details, see the raw_ptr removal plan in project docs.
 * ============================================================================
 */

#ifndef QUICKJS_HANDLE_CLASSES_H
#define QUICKJS_HANDLE_CLASSES_H

#include <stddef.h>
#include <stdint.h>

/* Include internal header for all struct definitions required by handle classes */
#include "quickjs-internal.h"

/* Forward declarations - these are defined in quickjs_types.h or elsewhere */
extern "C" void *gc_deref(uint32_t handle);
extern "C" bool gc_handle_is_valid(GCHandle handle);

/* gc_deref_unsafe is defined as a macro in quickjs.h (just an alias for gc_deref).
 * If quickjs.h is not included, we define it here as well. */
#ifndef gc_deref_unsafe
#define gc_deref_unsafe(handle) gc_deref(handle)
#endif

/* ============================================================================
 * GCPin Template Class - Type-safe temporary GC pointer pinning
 * ============================================================================
 * 
 * GCPin provides a type-safe way to temporarily dereference a GCHandle
 * to get a raw pointer for C/C++ operations. The pointer is only valid
 * until the next GC point. This is safer than raw gc_deref() calls because
 * it clearly documents the scope of pointer validity.
 * 
 * USAGE:
 *   GCPin<uint32_t> hash_table(s->hash_table_handle());
 *   uint32_t* ptr = hash_table.ptr();  // Valid only immediately
 *   
 *   // Or for one-time use:
 *   some_func(GCPin<JSClass>(class_array_handle).ptr());
 */

template<typename T>
class GCPin {
private:
    GCHandle handle_;
    mutable T* ptr_;
    
public:
    /* Construct from GCHandle */
    explicit GCPin(GCHandle handle) : handle_(handle) {
        ptr_ = (handle_ != GC_HANDLE_NULL) ? (T*)gc_deref(handle_) : nullptr;
    }
    
    /* Get the pinned pointer - only valid until next GC point */
    T* ptr() const { return ptr_; }
    
    /* Get the pinned pointer (alias for ptr()) */
    T* get() const { return ptr_; }
    
    /* Dereference operators */
    T& operator*() const { return *ptr_; }
    T* operator->() const { return ptr_; }
    
    /* Array access */
    T& operator[](size_t index) const { return ptr_[index]; }
    
    /* Boolean conversion */
    explicit operator bool() const { return ptr_ != nullptr; }
    
    /* Get the underlying handle */
    GCHandle handle() const { return handle_; }
};

/* Specialization for void - no dereference operators */
template<>
class GCPin<void> {
private:
    GCHandle handle_;
    mutable void* ptr_;
    
public:
    /* Construct from GCHandle */
    explicit GCPin(GCHandle handle) : handle_(handle) {
        ptr_ = (handle_ != GC_HANDLE_NULL) ? gc_deref(handle_) : nullptr;
    }
    
    /* Get the pinned pointer - only valid until next GC point */
    void* ptr() const { return ptr_; }
    
    /* Get the pinned pointer (alias for ptr()) */
    void* get() const { return ptr_; }
    
    /* Boolean conversion */
    explicit operator bool() const { return ptr_ != nullptr; }
    
    /* Get the underlying handle */
    GCHandle handle() const { return handle_; }
};

class JSContextHandle {
private:
    GCHandle handle_;
    
    /* Get fresh pointer - called on every access */
    JSContext* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSContext*)gc_deref(handle_);
    }

public:
    /* Default constructor - null handle */
    JSContextHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit JSContextHandle(GCHandle handle) : handle_(handle) {}
    
    /* Copy constructor */
    JSContextHandle(const JSContextHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    JSContextHandle(JSContextHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    JSContextHandle& operator=(const JSContextHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    JSContextHandle& operator=(JSContextHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Get the underlying handle */
    GCHandle handle() const { return handle_; }
    
    /* Check if handle is valid */
    bool valid() const { 
        return handle_ != GC_HANDLE_NULL && gc_handle_is_valid(handle_);
    }
    
    /* Explicit bool conversion */
    explicit operator bool() const { return valid(); }
    
    /* =========================================================================
     * Property accessors - each dereferences handle to get fresh pointer
     * ========================================================================= */
    
    /* rt_handle access */
    GCHandle rt_handle() const {
        JSContext* p = get_ptr();
        return p ? p->rt_handle : GC_HANDLE_NULL;
    }
    
    void set_rt_handle(GCHandle rt) {
        JSContext* p = get_ptr();
        if (p) {
            p->rt_handle = rt;
            gc_write_barrier_for_heap_slot(&p->rt_handle, rt);
        }
    }
    
    /* binary_object_count access */
    uint16_t binary_object_count() const {
        JSContext* p = get_ptr();
        return p ? p->binary_object_count : 0;
    }
    
    void set_binary_object_count(uint16_t count) {
        JSContext* p = get_ptr();
        if (p) p->binary_object_count = count;
    }
    
    /* binary_object_size access */
    int binary_object_size() const {
        JSContext* p = get_ptr();
        return p ? p->binary_object_size : 0;
    }
    
    void set_binary_object_size(int size) {
        JSContext* p = get_ptr();
        if (p) p->binary_object_size = size;
    }
    
    /* array_shape_handle access */
    GCHandle array_shape_handle() const {
        JSContext* p = get_ptr();
        return p ? p->array_shape_handle : GC_HANDLE_NULL;
    }
    
    void set_array_shape_handle(GCHandle handle) {
        JSContext* p = get_ptr();
        if (p) {
            p->array_shape_handle = handle;
            gc_write_barrier_for_heap_slot(&p->array_shape_handle, handle);
        }
    }
    
    GCHandle& array_shape_handle_ref() {
        JSContext* p = get_ptr();
        return p->array_shape_handle;
    }
    
    /* arguments_shape_handle access */
    GCHandle arguments_shape_handle() const {
        JSContext* p = get_ptr();
        return p ? p->arguments_shape_handle : GC_HANDLE_NULL;
    }
    
    void set_arguments_shape_handle(GCHandle handle) {
        JSContext* p = get_ptr();
        if (p) {
            p->arguments_shape_handle = handle;
            gc_write_barrier_for_heap_slot(&p->arguments_shape_handle, handle);
        }
    }
    
    GCHandle& arguments_shape_handle_ref() {
        JSContext* p = get_ptr();
        return p->arguments_shape_handle;
    }
    
    /* mapped_arguments_shape_handle access */
    GCHandle mapped_arguments_shape_handle() const {
        JSContext* p = get_ptr();
        return p ? p->mapped_arguments_shape_handle : GC_HANDLE_NULL;
    }
    
    void set_mapped_arguments_shape_handle(GCHandle handle) {
        JSContext* p = get_ptr();
        if (p) {
            p->mapped_arguments_shape_handle = handle;
            gc_write_barrier_for_heap_slot(&p->mapped_arguments_shape_handle, handle);
        }
    }
    
    GCHandle& mapped_arguments_shape_handle_ref() {
        JSContext* p = get_ptr();
        return p->mapped_arguments_shape_handle;
    }
    
    /* regexp_shape_handle access */
    GCHandle regexp_shape_handle() const {
        JSContext* p = get_ptr();
        return p ? p->regexp_shape_handle : GC_HANDLE_NULL;
    }
    
    void set_regexp_shape_handle(GCHandle handle) {
        JSContext* p = get_ptr();
        if (p) {
            p->regexp_shape_handle = handle;
            gc_write_barrier_for_heap_slot(&p->regexp_shape_handle, handle);
        }
    }
    
    /* regexp_shape_handle access by reference (for functions that modify it) */
    GCHandle& regexp_shape_handle_ref() {
        JSContext* p = get_ptr();
        return p->regexp_shape_handle;
    }
    
    /* regexp_result_shape_handle access */
    GCHandle regexp_result_shape_handle() const {
        JSContext* p = get_ptr();
        return p ? p->regexp_result_shape_handle : GC_HANDLE_NULL;
    }
    
    void set_regexp_result_shape_handle(GCHandle handle) {
        JSContext* p = get_ptr();
        if (p) {
            p->regexp_result_shape_handle = handle;
            gc_write_barrier_for_heap_slot(&p->regexp_result_shape_handle, handle);
        }
    }
    
    /* regexp_result_shape_handle access by reference (for functions that modify it) */
    GCHandle& regexp_result_shape_handle_ref() {
        JSContext* p = get_ptr();
        return p->regexp_result_shape_handle;
    }
    
    /* class_proto_handle access */
    GCHandle class_proto_handle() const {
        JSContext* p = get_ptr();
        return p ? p->class_proto_handle : GC_HANDLE_NULL;
    }
    
    void set_class_proto_handle(GCHandle handle) {
        JSContext* p = get_ptr();
        if (p) {
            p->class_proto_handle = handle;
            gc_write_barrier_for_heap_slot(&p->class_proto_handle, handle);
        }
    }
    
    /* class_proto_ptr - returns raw pointer to GCValue array (use with caution) */
    GCValue* class_proto_ptr() const {
        JSContext* p = get_ptr();
        return p ? (GCValue*)gc_deref(p->class_proto_handle) : nullptr;
    }
    
    /* function_proto access */
    GCValue function_proto() const {
        JSContext* p = get_ptr();
        return p ? p->function_proto : GC_UNDEFINED;
    }
    
    void set_function_proto(const GCValue& val) {
        JSContext* p = get_ptr();
        if (p) {
            p->function_proto = val;
            gc_write_barrier_for_heap_slot(&p->function_proto, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* function_ctor access */
    GCValue function_ctor() const {
        JSContext* p = get_ptr();
        return p ? p->function_ctor : GC_UNDEFINED;
    }
    
    void set_function_ctor(const GCValue& val) {
        JSContext* p = get_ptr();
        if (p) {
            p->function_ctor = val;
            gc_write_barrier_for_heap_slot(&p->function_ctor, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* array_ctor access */
    GCValue array_ctor() const {
        JSContext* p = get_ptr();
        return p ? p->array_ctor : GC_UNDEFINED;
    }
    
    void set_array_ctor(const GCValue& val) {
        JSContext* p = get_ptr();
        if (p) {
            p->array_ctor = val;
            gc_write_barrier_for_heap_slot(&p->array_ctor, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* regexp_ctor access */
    GCValue regexp_ctor() const {
        JSContext* p = get_ptr();
        return p ? p->regexp_ctor : GC_UNDEFINED;
    }
    
    void set_regexp_ctor(const GCValue& val) {
        JSContext* p = get_ptr();
        if (p) {
            p->regexp_ctor = val;
            gc_write_barrier_for_heap_slot(&p->regexp_ctor, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* promise_ctor access */
    GCValue promise_ctor() const {
        JSContext* p = get_ptr();
        return p ? p->promise_ctor : GC_UNDEFINED;
    }
    
    void set_promise_ctor(const GCValue& val) {
        JSContext* p = get_ptr();
        if (p) {
            p->promise_ctor = val;
            gc_write_barrier_for_heap_slot(&p->promise_ctor, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* iterator_ctor access */
    GCValue iterator_ctor() const {
        JSContext* p = get_ptr();
        return p ? p->iterator_ctor : GC_UNDEFINED;
    }
    
    void set_iterator_ctor(const GCValue& val) {
        JSContext* p = get_ptr();
        if (p) {
            p->iterator_ctor = val;
            gc_write_barrier_for_heap_slot(&p->iterator_ctor, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* async_iterator_proto access */
    GCValue async_iterator_proto() const {
        JSContext* p = get_ptr();
        return p ? p->async_iterator_proto : GC_UNDEFINED;
    }
    
    void set_async_iterator_proto(const GCValue& val) {
        JSContext* p = get_ptr();
        if (p) {
            p->async_iterator_proto = val;
            gc_write_barrier_for_heap_slot(&p->async_iterator_proto, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* array_proto_values access */
    GCValue array_proto_values() const {
        JSContext* p = get_ptr();
        return p ? p->array_proto_values : GC_UNDEFINED;
    }
    
    void set_array_proto_values(const GCValue& val) {
        JSContext* p = get_ptr();
        if (p) {
            p->array_proto_values = val;
            gc_write_barrier_for_heap_slot(&p->array_proto_values, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* throw_type_error access */
    GCValue throw_type_error() const {
        JSContext* p = get_ptr();
        return p ? p->throw_type_error : GC_UNDEFINED;
    }
    
    void set_throw_type_error(const GCValue& val) {
        JSContext* p = get_ptr();
        if (p) {
            p->throw_type_error = val;
            gc_write_barrier_for_heap_slot(&p->throw_type_error, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* eval_obj access */
    GCValue eval_obj() const {
        JSContext* p = get_ptr();
        return p ? p->eval_obj : GC_UNDEFINED;
    }
    
    void set_eval_obj(const GCValue& val) {
        JSContext* p = get_ptr();
        if (p) {
            p->eval_obj = val;
            gc_write_barrier_for_heap_slot(&p->eval_obj, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* global_obj access */
    GCValue global_obj() const {
        JSContext* p = get_ptr();
        return p ? p->global_obj : GC_UNDEFINED;
    }
    
    void set_global_obj(const GCValue& val) {
        JSContext* p = get_ptr();
        if (p) {
            p->global_obj = val;
            gc_write_barrier_for_heap_slot(&p->global_obj, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* random_state access */
    uint64_t random_state() const {
        JSContext* p = get_ptr();
        return p ? p->random_state : 0;
    }
    
    void set_random_state(uint64_t state) {
        JSContext* p = get_ptr();
        if (p) p->random_state = state;
    }
    
    /* user_opaque access */
    void* user_opaque() const {
        JSContext* p = get_ptr();
        return p ? p->user_opaque : nullptr;
    }
    
    void set_user_opaque(void* opaque) {
        JSContext* p = get_ptr();
        if (p) p->user_opaque = opaque;
    }
    
    /* native_error_proto access - returns pointer to array */
    GCValue* native_error_proto() const {
        JSContext* p = get_ptr();
        return p ? p->native_error_proto : nullptr;
    }
    
    /* eval_internal function pointer access */
    GCValue (*eval_internal_func() const)(JSContextHandle ctx, GCValue this_obj,
                                           const char *input, size_t input_len,
                                           const char *filename, int flags, int scope_idx) {
        JSContext* p = get_ptr();
        return p ? p->eval_internal : nullptr;
    }
    
    void set_eval_internal_func(GCValue (*func)(JSContextHandle ctx, GCValue this_obj,
                                                 const char *input, size_t input_len,
                                                 const char *filename, int flags, int scope_idx)) {
        JSContext* p = get_ptr();
        if (p) p->eval_internal = func;
    }
    
    /* compile_regexp function pointer access */
    GCValue (*compile_regexp_func() const)(JSContextHandle ctx, GCValue pattern,
                                            GCValue flags) {
        JSContext* p = get_ptr();
        return p ? p->compile_regexp : nullptr;
    }
    
    void set_compile_regexp_func(GCValue (*func)(JSContextHandle ctx, GCValue pattern,
                                                  GCValue flags)) {
        JSContext* p = get_ptr();
        if (p) p->compile_regexp = func;
    }
    
    /* =========================================================================
     * Memory and initialization operations
     * ========================================================================= */
    
    /* Zero all memory in the context struct */
    void zero_memory() {
        JSContext* p = get_ptr();
        if (p) memset(p, 0, sizeof(JSContext));
    }
    
    /* Get reference to loaded_modules list */
    GCHandleList& loaded_modules() {
        JSContext* p = get_ptr();
        static GCHandleList dummy;
        return p ? p->loaded_modules : dummy;
    }
    
    /* =========================================================================
     * Conversion operators for interoperability
     * ========================================================================= */
    
    /* Convert to GCHandle for handle-based APIs */
    operator GCHandle() const {
        return handle_;
    }
    
    /* Comparison operators */
    bool operator==(const JSContextHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSContextHandle& other) const {
        return handle_ != other.handle_;
    }
    
    bool operator==(GCHandle h) const {
        return handle_ == h;
    }
    
    bool operator!=(GCHandle h) const {
        return handle_ != h;
    }
    
    bool operator==(decltype(nullptr)) const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    bool operator!=(decltype(nullptr)) const {
        return handle_ != GC_HANDLE_NULL;
    }
    
    /* =========================================================================
     * Native error prototype array access - for js_quickjs.cpp
     * ========================================================================= */
    
    /* Get native error proto at index - returns GCValue by value */
    GCValue get_native_error_proto(int index) const {
        JSContext* p = get_ptr();
        if (p && index >= 0 && index < JS_NATIVE_ERROR_COUNT) {
            return p->native_error_proto[index];
        }
        return GC_UNDEFINED;
    }
    
    /* Set native error proto at index */
    void set_native_error_proto(int index, const GCValue& val) {
        JSContext* p = get_ptr();
        if (p && index >= 0 && index < JS_NATIVE_ERROR_COUNT) {
            p->native_error_proto[index] = val;
        }
    }
    
    /* Get pointer to native_error_proto array for iteration */
    GCValue* native_error_proto_array() const {
        JSContext* p = get_ptr();
        return p ? p->native_error_proto : nullptr;
    }
};


class JSRuntimeHandle {
private:
    GCHandle handle_;
    
    /* Get fresh pointer - called on every access */
    JSRuntime* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSRuntime*)gc_deref(handle_);
    }

public:
    /* Default constructor - null handle */
    JSRuntimeHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit JSRuntimeHandle(GCHandle handle) : handle_(handle) {}
    
    /* Copy constructor */
    JSRuntimeHandle(const JSRuntimeHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    JSRuntimeHandle(JSRuntimeHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    JSRuntimeHandle& operator=(const JSRuntimeHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    JSRuntimeHandle& operator=(JSRuntimeHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Get the underlying handle */
    GCHandle handle() const { return handle_; }
    
    /* Check if handle is valid */
    bool valid() const { 
        return handle_ != GC_HANDLE_NULL && gc_handle_is_valid(handle_);
    }
    
    /* Explicit bool conversion */
    explicit operator bool() const { return valid(); }
    
    /* =========================================================================
     * Property accessors - each dereferences handle to get fresh pointer
     * ========================================================================= */
    
    /* malloc_state access */
    JSMallocState malloc_state() const {
        JSRuntime* p = get_ptr();
        return p ? p->malloc_state : JSMallocState{};
    }
    
    void set_malloc_state(const JSMallocState& state) {
        JSRuntime* p = get_ptr();
        if (p) p->malloc_state = state;
    }
    
    /* rt_info access */
    const char* rt_info() const {
        JSRuntime* p = get_ptr();
        return p ? p->rt_info : nullptr;
    }
    
    void set_rt_info(const char* info) {
        JSRuntime* p = get_ptr();
        if (p) p->rt_info = info;
    }
    
    /* atom_hash_size access */
    int atom_hash_size() const {
        JSRuntime* p = get_ptr();
        return p ? p->atom_hash_size : 0;
    }
    
    void set_atom_hash_size(int size) {
        JSRuntime* p = get_ptr();
        if (p) p->atom_hash_size = size;
    }
    
    /* atom_count access */
    int atom_count() const {
        JSRuntime* p = get_ptr();
        return p ? p->atom_count : 0;
    }
    
    void set_atom_count(int count) {
        JSRuntime* p = get_ptr();
        if (p) p->atom_count = count;
    }
    
    /* atom_size access */
    int atom_size() const {
        JSRuntime* p = get_ptr();
        return p ? p->atom_size : 0;
    }
    
    void set_atom_size(int size) {
        JSRuntime* p = get_ptr();
        if (p) p->atom_size = size;
    }
    
    /* atom_count_resize access */
    int atom_count_resize() const {
        JSRuntime* p = get_ptr();
        return p ? p->atom_count_resize : 0;
    }
    
    void set_atom_count_resize(int count) {
        JSRuntime* p = get_ptr();
        if (p) p->atom_count_resize = count;
    }
    
    /* atom_hash_handle access */
    GCHandle atom_hash_handle() const {
        JSRuntime* p = get_ptr();
        return p ? p->atom_hash_handle : GC_HANDLE_NULL;
    }
    
    void set_atom_hash_handle(GCHandle h) {
        JSRuntime* p = get_ptr();
        if (p) {
            p->atom_hash_handle = h;
            gc_write_barrier_for_heap_slot(&p->atom_hash_handle, h);
        }
    }
    
    /* atom_array_handle access */
    GCHandle atom_array_handle() const {
        JSRuntime* p = get_ptr();
        return p ? p->atom_array_handle : GC_HANDLE_NULL;
    }
    
    void set_atom_array_handle(GCHandle h) {
        JSRuntime* p = get_ptr();
        if (p) {
            p->atom_array_handle = h;
            gc_write_barrier_for_heap_slot(&p->atom_array_handle, h);
        }
    }
    
    /* atom_free_index access */
    int atom_free_index() const {
        JSRuntime* p = get_ptr();
        return p ? p->atom_free_index : 0;
    }
    
    void set_atom_free_index(int index) {
        JSRuntime* p = get_ptr();
        if (p) p->atom_free_index = index;
    }
    
    /* class_count access */
    int class_count() const {
        JSRuntime* p = get_ptr();
        return p ? p->class_count : 0;
    }
    
    void set_class_count(int count) {
        JSRuntime* p = get_ptr();
        if (p) p->class_count = count;
    }
    
    /* class_array_handle access */
    GCHandle class_array_handle() const {
        JSRuntime* p = get_ptr();
        return p ? p->class_array_handle : GC_HANDLE_NULL;
    }
    
    void set_class_array_handle(GCHandle h) {
        JSRuntime* p = get_ptr();
        if (p) {
            p->class_array_handle = h;
            gc_write_barrier_for_heap_slot(&p->class_array_handle, h);
        }
    }
    
    /* gc_phase access */
    JSGCPhaseEnum gc_phase() const {
        JSRuntime* p = get_ptr();
        return p ? p->gc_phase : JS_GC_PHASE_NONE;
    }
    
    void set_gc_phase(JSGCPhaseEnum phase) {
        JSRuntime* p = get_ptr();
        if (p) p->gc_phase = phase;
    }
    
    /* malloc_gc_threshold access */
    size_t malloc_gc_threshold() const {
        JSRuntime* p = get_ptr();
        return p ? p->malloc_gc_threshold : 0;
    }
    
    void set_malloc_gc_threshold(size_t threshold) {
        JSRuntime* p = get_ptr();
        if (p) p->malloc_gc_threshold = threshold;
    }
    
    /* stack_size access */
    uintptr_t stack_size() const {
        JSRuntime* p = get_ptr();
        return p ? p->stack_size : 0;
    }
    
    void set_stack_size(uintptr_t size) {
        JSRuntime* p = get_ptr();
        if (p) p->stack_size = size;
    }
    
    /* stack_top access */
    uintptr_t stack_top() const {
        JSRuntime* p = get_ptr();
        return p ? p->stack_top : 0;
    }
    
    void set_stack_top(uintptr_t top) {
        JSRuntime* p = get_ptr();
        if (p) p->stack_top = top;
    }
    
    /* stack_limit access */
    uintptr_t stack_limit() const {
        JSRuntime* p = get_ptr();
        return p ? p->stack_limit : 0;
    }
    
    void set_stack_limit(uintptr_t limit) {
        JSRuntime* p = get_ptr();
        if (p) p->stack_limit = limit;
    }
    
    /* current_exception access */
    GCValue current_exception() const {
        JSRuntime* p = get_ptr();
        return p ? p->current_exception : GC_UNDEFINED;
    }
    
    void set_current_exception(const GCValue& val) {
        JSRuntime* p = get_ptr();
        if (p) {
            p->current_exception = val;
            gc_write_barrier_for_heap_slot(&p->current_exception, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* current_exception_is_uncatchable access */
    BOOL current_exception_is_uncatchable() const {
        JSRuntime* p = get_ptr();
        return p ? p->current_exception_is_uncatchable : FALSE;
    }
    
    void set_current_exception_is_uncatchable(BOOL val) {
        JSRuntime* p = get_ptr();
        if (p) p->current_exception_is_uncatchable = val;
    }
    
    /* in_out_of_memory access */
    BOOL in_out_of_memory() const {
        JSRuntime* p = get_ptr();
        return p ? p->in_out_of_memory : FALSE;
    }
    
    void set_in_out_of_memory(BOOL val) {
        JSRuntime* p = get_ptr();
        if (p) p->in_out_of_memory = val;
    }
    
    /* permanent_atom_count access */
    uint32_t permanent_atom_count() const {
        JSRuntime* p = get_ptr();
        return p ? p->permanent_atom_count : 0;
    }
    
    void set_permanent_atom_count(uint32_t count) {
        JSRuntime* p = get_ptr();
        if (p) p->permanent_atom_count = count;
    }
    
    /* atom_gc_marks_handle access */
    GCHandle atom_gc_marks_handle() const {
        JSRuntime* p = get_ptr();
        return p ? p->atom_gc_marks_handle : GC_HANDLE_NULL;
    }
    
    void set_atom_gc_marks_handle(GCHandle h) {
        JSRuntime* p = get_ptr();
        if (p) {
            p->atom_gc_marks_handle = h;
            gc_write_barrier_for_heap_slot(&p->atom_gc_marks_handle, h);
        }
    }
    
    /* current_stack_frame access - returns dereferenced pointer */
    JSStackFrame* current_stack_frame() const {
        JSRuntime* p = get_ptr();
        if (!p || p->current_stack_frame_handle == GC_HANDLE_NULL) return nullptr;
        return (JSStackFrame*)gc_deref(p->current_stack_frame_handle);
    }
    
    GCHandle current_stack_frame_handle() const {
        JSRuntime* p = get_ptr();
        return p ? p->current_stack_frame_handle : GCHandle();
    }
    
    void set_current_stack_frame(JSStackFrame* frame) {
        JSRuntime* p = get_ptr();
        if (p) p->current_stack_frame_handle = frame ? frame->self_handle : GCHandle();
    }
    
    void set_current_stack_frame_handle(GCHandle handle) {
        JSRuntime* p = get_ptr();
        if (p) {
            p->current_stack_frame_handle = handle;
            gc_write_barrier_for_heap_slot(&p->current_stack_frame_handle, handle);
        }
    }
    
    /* host_promise_rejection_tracker access */
    JSHostPromiseRejectionTracker* host_promise_rejection_tracker() const {
        JSRuntime* p = get_ptr();
        return p ? p->host_promise_rejection_tracker : nullptr;
    }
    
    void set_host_promise_rejection_tracker(JSHostPromiseRejectionTracker* tracker) {
        JSRuntime* p = get_ptr();
        if (p) p->host_promise_rejection_tracker = tracker;
    }
    
    /* host_promise_rejection_tracker_opaque access */
    void* host_promise_rejection_tracker_opaque() const {
        JSRuntime* p = get_ptr();
        return p ? p->host_promise_rejection_tracker_opaque : nullptr;
    }
    
    void set_host_promise_rejection_tracker_opaque(void* opaque) {
        JSRuntime* p = get_ptr();
        if (p) p->host_promise_rejection_tracker_opaque = opaque;
    }
    
    /* module_normalize_func access */
    JSModuleNormalizeFunc* module_normalize_func() const {
        JSRuntime* p = get_ptr();
        return p ? p->module_normalize_func : nullptr;
    }
    
    void set_module_normalize_func(JSModuleNormalizeFunc* func) {
        JSRuntime* p = get_ptr();
        if (p) p->module_normalize_func = func;
    }
    
    /* module_loader_has_attr access */
    BOOL module_loader_has_attr() const {
        JSRuntime* p = get_ptr();
        return p ? p->module_loader_has_attr : FALSE;
    }
    
    void set_module_loader_has_attr(BOOL val) {
        JSRuntime* p = get_ptr();
        if (p) p->module_loader_has_attr = val;
    }
    
    /* module_loader_func access */
    JSModuleLoaderFunc* module_loader_func() const {
        JSRuntime* p = get_ptr();
        return p ? p->u.module_loader_func : nullptr;
    }
    
    void set_module_loader_func(JSModuleLoaderFunc* func) {
        JSRuntime* p = get_ptr();
        if (p) p->u.module_loader_func = func;
    }
    
    /* module_loader_func2 access */
    JSModuleLoaderFunc2* module_loader_func2() const {
        JSRuntime* p = get_ptr();
        return p ? p->u.module_loader_func2 : nullptr;
    }
    
    void set_module_loader_func2(JSModuleLoaderFunc2* func) {
        JSRuntime* p = get_ptr();
        if (p) p->u.module_loader_func2 = func;
    }
    
    /* module_check_attrs access */
    JSModuleCheckSupportedImportAttributes* module_check_attrs() const {
        JSRuntime* p = get_ptr();
        return p ? p->module_check_attrs : nullptr;
    }
    
    void set_module_check_attrs(JSModuleCheckSupportedImportAttributes* func) {
        JSRuntime* p = get_ptr();
        if (p) p->module_check_attrs = func;
    }
    
    /* module_loader_opaque access */
    void* module_loader_opaque() const {
        JSRuntime* p = get_ptr();
        return p ? p->module_loader_opaque : nullptr;
    }
    
    void set_module_loader_opaque(void* opaque) {
        JSRuntime* p = get_ptr();
        if (p) p->module_loader_opaque = opaque;
    }
    
    /* module_async_evaluation_next_timestamp access */
    int64_t module_async_evaluation_next_timestamp() const {
        JSRuntime* p = get_ptr();
        return p ? p->module_async_evaluation_next_timestamp : 0;
    }
    
    void set_module_async_evaluation_next_timestamp(int64_t ts) {
        JSRuntime* p = get_ptr();
        if (p) p->module_async_evaluation_next_timestamp = ts;
    }
    
    /* can_block access */
    BOOL can_block() const {
        JSRuntime* p = get_ptr();
        return p ? p->can_block : FALSE;
    }
    
    void set_can_block(BOOL val) {
        JSRuntime* p = get_ptr();
        if (p) p->can_block = val;
    }
    
    /* strip_flags access */
    uint8_t strip_flags() const {
        JSRuntime* p = get_ptr();
        return p ? p->strip_flags : 0;
    }
    
    void set_strip_flags(uint8_t flags) {
        JSRuntime* p = get_ptr();
        if (p) p->strip_flags = flags;
    }
    
    /* Lock-free shape hash table access.  The table pointer itself is owned by
       the runtime and is swapped only during resize; readers dereference the
       pointer without locks. */
    LFHashTable *shape_hash() const {
        JSRuntime* p = get_ptr();
        if (!p) return nullptr;
        return (LFHashTable *)atomic_load_ptr((void *volatile *)&p->shape_hash);
    }
    
    void set_shape_hash(LFHashTable *t) {
        JSRuntime* p = get_ptr();
        if (p) atomic_store_ptr((void *volatile *)&p->shape_hash, (void *)t);
    }
    
    /* Address of the shape_hash pointer (for CAS resize). */
    LFHashTable **shape_hash_ptr_addr() {
        JSRuntime* p = get_ptr();
        return p ? &p->shape_hash : nullptr;
    }
    
    LFHashTable *shape_hash_retired() const {
        JSRuntime* p = get_ptr();
        if (!p) return nullptr;
        return (LFHashTable *)atomic_load_ptr((void *volatile *)&p->shape_hash_retired);
    }
    
    void set_shape_hash_retired(LFHashTable *t) {
        JSRuntime* p = get_ptr();
        if (p) atomic_store_ptr((void *volatile *)&p->shape_hash_retired, (void *)t);
    }
    
    int shape_hash_bits() const {
        LFHashTable *t = shape_hash();
        return t ? (int)t->bucket_bits : 0;
    }
    
    int shape_hash_size() const {
        LFHashTable *t = shape_hash();
        return t ? (int)t->bucket_count : 0;
    }
    
    /* shape_hash_count access - stored atomically by insert/delete wrappers */
    int shape_hash_count() const {
        JSRuntime* p = get_ptr();
        return p ? (int)atomic_load_u32(&p->shape_hash_count) : 0;
    }
    
    void set_shape_hash_count(int count) {
        JSRuntime* p = get_ptr();
        if (p) atomic_store_u32(&p->shape_hash_count, (uint32_t)count);
    }
    
    void shape_hash_count_inc() {
        JSRuntime* p = get_ptr();
        if (p) atomic_fetch_add_u32(&p->shape_hash_count, 1);
    }
    
    void shape_hash_count_dec() {
        JSRuntime* p = get_ptr();
        if (p) atomic_fetch_sub_u32(&p->shape_hash_count, 1);
    }
    
    /* user_opaque access */
    void* user_opaque() const {
        JSRuntime* p = get_ptr();
        return p ? p->user_opaque : nullptr;
    }
    
    void set_user_opaque(void* opaque) {
        JSRuntime* p = get_ptr();
        if (p) p->user_opaque = opaque;
    }
    
    /* instruction_counter access */
    uint32_t instruction_counter() const {
        JSRuntime* p = get_ptr();
        return p ? p->instruction_counter : 0;
    }
    
    void set_instruction_counter(uint32_t counter) {
        JSRuntime* p = get_ptr();
        if (p) p->instruction_counter = counter;
    }
    
    /* Job queue access */
    GCHandleRingBuffer& job_queue() {
        JSRuntime* p = get_ptr();
        static GCHandleRingBuffer dummy;
        return p ? p->job_queue : dummy;
    }
    
    /* =========================================================================
     * Handle accessors that return pointer to dereferenced arrays
     * Use with caution - pointers are only valid until next GC point
     * ========================================================================= */
    
    /* atom_hash - returns pointer to uint32_t array */
    uint32_t* atom_hash_ptr() const {
        JSRuntime* p = get_ptr();
        return p ? (uint32_t*)gc_deref(p->atom_hash_handle) : nullptr;
    }
    
    /* atom_array - returns pointer to GCHandle array */
    GCHandle* atom_array_ptr() const {
        JSRuntime* p = get_ptr();
        return p ? (GCHandle*)gc_deref(p->atom_array_handle) : nullptr;
    }
    
    /* class_array - returns pointer to JSClass array (forward declared, defined in quickjs.cpp) */
    void* class_array_ptr() const {
        JSRuntime* p = get_ptr();
        return p ? gc_deref(p->class_array_handle) : nullptr;
    }
    
    /* atom_gc_marks - returns pointer to uint32_t array */
    uint32_t* atom_gc_marks_ptr() const {
        JSRuntime* p = get_ptr();
        return p ? (uint32_t*)gc_deref(p->atom_gc_marks_handle) : nullptr;
    }
    
    /* =========================================================================
     * JSMallocState field accessors (for accessing sub-fields)
     * ========================================================================= */
    
    /* malloc_limit access (inside malloc_state) */
    size_t malloc_limit() const {
        JSRuntime* p = get_ptr();
        return p ? p->malloc_state.malloc_limit : 0;
    }
    
    void set_malloc_limit(size_t limit) {
        JSRuntime* p = get_ptr();
        if (p) p->malloc_state.malloc_limit = limit;
    }
    
    /* malloc_count access (inside malloc_state) */
    size_t malloc_count() const {
        JSRuntime* p = get_ptr();
        return p ? p->malloc_state.malloc_count : 0;
    }
    
    void set_malloc_count(size_t count) {
        JSRuntime* p = get_ptr();
        if (p) p->malloc_state.malloc_count = count;
    }
    
    /* malloc_size access (inside malloc_state) */
    size_t malloc_size() const {
        JSRuntime* p = get_ptr();
        return p ? p->malloc_state.malloc_size : 0;
    }
    
    void set_malloc_size(size_t size) {
        JSRuntime* p = get_ptr();
        if (p) p->malloc_state.malloc_size = size;
    }
    
    /* =========================================================================
     * Memory operations
     * ========================================================================= */
    
    /* Zero all memory in the runtime struct */
    void zero_memory() {
        JSRuntime* p = get_ptr();
        if (p) memset(p, 0, sizeof(JSRuntime));
    }

    /* sab_funcs access */
    JSSharedArrayBufferFunctions sab_funcs() const {
        JSRuntime* p = get_ptr();
        return p ? p->sab_funcs : JSSharedArrayBufferFunctions{};
    }
    
    void set_sab_funcs(const JSSharedArrayBufferFunctions& funcs) {
        JSRuntime* p = get_ptr();
        if (p) p->sab_funcs = funcs;
    }
    
    /* =========================================================================
     * Conversion operators for interoperability
     * ========================================================================= */
    
    /* Convert to GCHandle for handle-based APIs */
    operator GCHandle() const {
        return handle_;
    }
    
    /* Comparison operators */
    bool operator==(const JSRuntimeHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSRuntimeHandle& other) const {
        return handle_ != other.handle_;
    }
    
    bool operator==(GCHandle h) const {
        return handle_ == h;
    }
    
    bool operator!=(GCHandle h) const {
        return handle_ != h;
    }
    
    bool operator==(decltype(nullptr)) const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    bool operator!=(decltype(nullptr)) const {
        return handle_ != GC_HANDLE_NULL;
    }
};


/* ============================================================================
 * C++ JSAsyncFrameHandle Class - Safe GC Handle Wrapper for Async/Generator Frames
 * ============================================================================
 * 
 * This class wraps a GCHandle to provide safe access to JSStackFrame instances.
 * The key feature is that it NEVER stores a raw JSStackFrame pointer - instead,
 * it dereferences the handle on every property access to get a fresh pointer.
 * This ensures that the pointer is always valid even after GC compaction.
 * 
 * USAGE:
 *   JSAsyncFrameHandle sf = JSAsyncFrameHandle(frame_handle);
 *   JSVarRefHandle* refs = sf.var_refs();  // Fresh dereference
 *   int mode = sf.js_mode();
 */

class JSAsyncFrameHandle {
private:
    GCHandle handle_;
    
    /* Get fresh pointer - called on every access */
    JSStackFrame* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSStackFrame*)gc_deref(handle_);
    }

public:
    /* Default constructor - null handle */
    JSAsyncFrameHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit JSAsyncFrameHandle(GCHandle handle) : handle_(handle) {}
    
    /* Copy constructor */
    JSAsyncFrameHandle(const JSAsyncFrameHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    JSAsyncFrameHandle(JSAsyncFrameHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    JSAsyncFrameHandle& operator=(const JSAsyncFrameHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    JSAsyncFrameHandle& operator=(JSAsyncFrameHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Get the underlying handle */
    GCHandle handle() const { return handle_; }
    
    /* Check if handle is valid */
    bool valid() const { 
        return handle_ != GC_HANDLE_NULL && gc_handle_is_valid(handle_);
    }
    
    /* Explicit bool conversion */
    explicit operator bool() const { return valid(); }
    
    /* =========================================================================
     * Property accessors - each dereferences handle to get fresh pointer
     * ========================================================================= */
    
    /* prev_frame access - uses handle for GC safety */
    GCHandle prev_frame_handle() const {
        JSStackFrame* p = get_ptr();
        return p ? p->prev_frame_handle : GCHandle();
    }
    
    void set_prev_frame_handle(GCHandle handle) {
        JSStackFrame* p = get_ptr();
        if (p) {
            p->prev_frame_handle = handle;
            gc_write_barrier_for_heap_slot(&p->prev_frame_handle, handle);
        }
    }
    
    /* cur_func access */
    GCValue cur_func() const {
        JSStackFrame* p = get_ptr();
        return p ? p->cur_func : GC_UNDEFINED;
    }
    
    void set_cur_func(const GCValue& val) {
        JSStackFrame* p = get_ptr();
        if (p) {
            p->cur_func = val;
            gc_write_barrier_for_heap_slot(&p->cur_func, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* arg_buf access - uses offset from frame base */
    uint32_t arg_buf_offset() const {
        JSStackFrame* p = get_ptr();
        return p ? p->arg_buf_offset : 0;
    }
    
    void set_arg_buf_offset(uint32_t offset) {
        JSStackFrame* p = get_ptr();
        if (p) p->arg_buf_offset = offset;
    }
    
    /* var_buf access - uses offset from frame base */
    uint32_t var_buf_offset() const {
        JSStackFrame* p = get_ptr();
        return p ? p->var_buf_offset : 0;
    }
    
    void set_var_buf_offset(uint32_t offset) {
        JSStackFrame* p = get_ptr();
        if (p) p->var_buf_offset = offset;
    }
    
    /* var_refs access - uses offset from frame base */
    uint32_t var_refs_offset() const {
        JSStackFrame* p = get_ptr();
        return p ? p->var_refs_offset : 0;
    }
    
    void set_var_refs_offset(uint32_t offset) {
        JSStackFrame* p = get_ptr();
        if (p) p->var_refs_offset = offset;
    }
    
    /* cur_pc access - uses offset from bytecode base */
    uint32_t pc_offset() const {
        JSStackFrame* p = get_ptr();
        return p ? p->pc_offset : 0;
    }
    
    void set_pc_offset(uint32_t offset) {
        JSStackFrame* p = get_ptr();
        if (p) p->pc_offset = offset;
    }
    
    /* arg_count access */
    int arg_count() const {
        JSStackFrame* p = get_ptr();
        return p ? p->arg_count : 0;
    }
    
    void set_arg_count(int count) {
        JSStackFrame* p = get_ptr();
        if (p) p->arg_count = count;
    }
    
    /* js_mode access */
    int js_mode() const {
        JSStackFrame* p = get_ptr();
        return p ? p->js_mode : 0;
    }
    
    void set_js_mode(int mode) {
        JSStackFrame* p = get_ptr();
        if (p) p->js_mode = mode;
    }
    
    /* sp_offset access - offset from var_buf */
    uint32_t sp_offset() const {
        JSStackFrame* p = get_ptr();
        return p ? p->sp_offset : 0;
    }
    
    void set_sp_offset(uint32_t offset) {
        JSStackFrame* p = get_ptr();
        if (p) p->sp_offset = offset;
    }
    
    /* Get raw pointer for direct field access (use with caution - don't store) */
    JSStackFrame* ptr() const {
        return get_ptr();
    }
    
    /* =========================================================================
     * Conversion operators for interoperability
     * ========================================================================= */
    
    /* Convert to GCHandle for handle-based APIs */
    operator GCHandle() const {
        return handle_;
    }
    
    /* Comparison operators */
    bool operator==(const JSAsyncFrameHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSAsyncFrameHandle& other) const {
        return handle_ != other.handle_;
    }
    
    bool operator==(GCHandle h) const {
        return handle_ == h;
    }
    
    bool operator!=(GCHandle h) const {
        return handle_ != h;
    }
    
    bool operator==(decltype(nullptr)) const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    bool operator!=(decltype(nullptr)) const {
        return handle_ != GC_HANDLE_NULL;
    }
};


/* ============================================================================
 * C++ JSBigIntHandle Class - Safe GC Handle Wrapper
 * ============================================================================
 * 
 * This class wraps a GCHandle to provide safe access to JSBigInt instances.
 * The key feature is that it NEVER stores a raw JSBigInt pointer - instead,
 * it dereferences the handle on every property access to get a fresh pointer.
 * This ensures that the pointer is always valid even after GC compaction.
 * 
 * USAGE:
 *   JSBigIntHandle bigint = JS_VALUE_GET_BIGINT_HANDLE(val);
 *   uint32_t len = bigint.len();  // Fresh dereference
 *   js_limb_t* tab = bigint.tab(); // Get pointer to limbs
 */

class JSBigIntHandle {
private:
    GCHandle handle_;
    
    /* Get fresh pointer - called on every access - PRIVATE to prevent external storage */
    JSBigInt* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        /* Detect if this is a raw pointer (high bit set on 64-bit) vs a real handle */
        if (handle_ > 0x10000000) {
            return (JSBigInt*)(uintptr_t)handle_;
        }
        return (JSBigInt*)gc_deref(handle_);
    }

public:
    /* Default constructor - null handle */
    JSBigIntHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit JSBigIntHandle(GCHandle handle) : handle_(handle) {}
    
    /* Note: No raw pointer constructor - use GCHandle constructor to avoid GC unsafety */
    
    /* Copy constructor */
    JSBigIntHandle(const JSBigIntHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    JSBigIntHandle(JSBigIntHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    JSBigIntHandle& operator=(const JSBigIntHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    JSBigIntHandle& operator=(JSBigIntHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Assignment from nullptr */
    JSBigIntHandle& operator=(decltype(nullptr)) {
        handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Get the underlying handle */
    GCHandle handle() const { return handle_; }
    
    /* Check if handle is valid */
    bool valid() const { 
        return handle_ != GC_HANDLE_NULL && gc_handle_is_valid(handle_);
    }
    
    /* Check if handle is null */
    bool is_null() const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    /* Explicit bool conversion */
    explicit operator bool() const { return valid(); }
    
    /* =====================================================================
     * BigInt Property Accessors - NEVER store pointers, always use handles
     * Each accessor dereferences handle to get fresh pointer
     * ===================================================================== */
    
    /* len getter - returns number of limbs */
    uint32_t len() const {
        JSBigInt* p = get_ptr();
        return p ? p->len : 0;
    }
    
    /* len setter */
    void set_len(uint32_t val) {
        JSBigInt* p = get_ptr();
        if (p) p->len = val;
    }
    
    /* =====================================================================
     * Limb Array Element Accessors - Safe indexed access without exposing pointers
     * ===================================================================== */
    
    /* Get limb at index - returns value, not pointer */
    js_limb_t get_limb(uint32_t index) const {
        JSBigInt* p = get_ptr();
        if (p && index < p->len) {
            return p->tab[index];
        }
        return 0;
    }
    
    /* Set limb at index */
    void set_limb(uint32_t index, js_limb_t val) {
        JSBigInt* p = get_ptr();
        if (p && index < p->len) {
            p->tab[index] = val;
        }
    }
    
    /* Get pointer to limb array - USE WITH CAUTION, never store this pointer */
    /* This is needed for low-level bigint operations that need raw array access */
    js_limb_t* tab() const {
        JSBigInt* p = get_ptr();
        return p ? p->tab : nullptr;
    }
    
    /* UNSAFE: Get raw pointer to JSBigInt data.
     * WARNING: This pointer can become STALE after ANY GC operation!
     * Only use this when you MUST pass raw pointers to js_bigint_* functions.
     * The caller is responsible for ensuring GC safety.
     * Prefer using the handle-based methods (add, mul, etc.) instead.
     */
    JSBigInt* unsafe_data() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        /* Detect if this is a raw pointer (high bit set on 64-bit) vs a real handle */
        if (handle_ > 0x10000000) {
            return (JSBigInt*)(uintptr_t)handle_;
        }
        return (JSBigInt*)gc_deref_unsafe(handle_);
    }
    
    /* =====================================================================
     * BigInt Operations - High-level operations that don't expose pointers
     * ===================================================================== */
    
    /* Check if BigInt represents zero */
    bool is_zero() const {
        JSBigInt* p = get_ptr();
        if (!p || p->len == 0) return true;
        /* Check if all limbs are zero */
        for (uint32_t i = 0; i < p->len; i++) {
            if (p->tab[i] != 0) return false;
        }
        return true;
    }
    
    /* Check if BigInt is negative (sign bit set on highest limb) */
    bool is_negative() const {
        JSBigInt* p = get_ptr();
        if (!p || p->len == 0) return false;
        js_limb_t high = p->tab[p->len - 1];
        return (js_slimb_t)high < 0;
    }
    
    /* Get sign: -1 for negative, 0 for zero, 1 for positive */
    int sign() const {
        if (is_zero()) return 0;
        return is_negative() ? -1 : 1;
    }
    
    /* Get the number of bits needed to represent this BigInt */
    uint32_t bit_length() const {
        JSBigInt* p = get_ptr();
        if (!p || p->len == 0) return 0;
        js_limb_t high = p->tab[p->len - 1];
        uint32_t bits = p->len * JS_LIMB_BITS;
        /* Adjust for leading zeros in high limb */
        if (high != 0) {
            bits -= JS_LIMB_BITS;
            while (high != 0) {
                bits++;
                high >>= 1;
            }
        }
        return bits;
    }
    
    /* Check if bit at position is set */
    bool test_bit(uint32_t pos) const {
        JSBigInt* p = get_ptr();
        if (!p) return false;
        uint32_t limb_idx = pos / JS_LIMB_BITS;
        if (limb_idx >= p->len) {
            /* Sign extend for positions beyond current length */
            return is_negative();
        }
        uint32_t bit_idx = pos % JS_LIMB_BITS;
        return (p->tab[limb_idx] >> bit_idx) & 1;
    }
    
    /* Set bit at position */
    void set_bit(uint32_t pos, bool val) {
        JSBigInt* p = get_ptr();
        if (!p) return;
        uint32_t limb_idx = pos / JS_LIMB_BITS;
        if (limb_idx >= p->len) return; /* Out of bounds */
        uint32_t bit_idx = pos % JS_LIMB_BITS;
        if (val) {
            p->tab[limb_idx] |= ((js_limb_t)1 << bit_idx);
        } else {
            p->tab[limb_idx] &= ~((js_limb_t)1 << bit_idx);
        }
    }
    
    /* Get number of limbs (same as len() but clearer in some contexts) */
    uint32_t num_limbs() const {
        return len();
    }
    
    /* Check if index is valid for accessing limbs */
    bool is_valid_index(uint32_t index) const {
        return index < len();
    }
    
    /* =====================================================================
     * Bulk Operations - Operations on ranges of limbs
     * ===================================================================== */
    
    /* Copy limbs from this BigInt to destination buffer */
    void copy_limbs_to(js_limb_t* dest, uint32_t count) const {
        JSBigInt* p = get_ptr();
        if (!p || !dest) return;
        uint32_t n = (count < p->len) ? count : p->len;
        for (uint32_t i = 0; i < n; i++) {
            dest[i] = p->tab[i];
        }
        /* Zero pad if dest is larger */
        for (uint32_t i = n; i < count; i++) {
            dest[i] = 0;
        }
    }
    
    /* Copy limbs from source buffer to this BigInt */
    void copy_limbs_from(const js_limb_t* src, uint32_t count) {
        JSBigInt* p = get_ptr();
        if (!p || !src) return;
        uint32_t n = (count < p->len) ? count : p->len;
        for (uint32_t i = 0; i < n; i++) {
            p->tab[i] = src[i];
        }
    }
    
    /* Clear all limbs to zero */
    void clear_limbs() {
        JSBigInt* p = get_ptr();
        if (!p) return;
        for (uint32_t i = 0; i < p->len; i++) {
            p->tab[i] = 0;
        }
    }
    
    /* Fill limbs with a specific value */
    void fill_limbs(js_limb_t val) {
        JSBigInt* p = get_ptr();
        if (!p) return;
        for (uint32_t i = 0; i < p->len; i++) {
            p->tab[i] = val;
        }
    }
    
    /* =====================================================================
     * Comparison Operations
     * ===================================================================== */
    
    /* Compare absolute values of two BigInts (ignoring sign) */
    int compare_abs(const JSBigIntHandle& other) const {
        uint32_t len1 = len();
        uint32_t len2 = other.len();
        if (len1 != len2) {
            return (len1 < len2) ? -1 : 1;
        }
        /* Compare from high limbs to low */
        for (int i = (int)len1 - 1; i >= 0; i--) {
            js_limb_t a = get_limb(i);
            js_limb_t b = other.get_limb(i);
            if (a != b) {
                return (a < b) ? -1 : 1;
            }
        }
        return 0;
    }
    
    /* =====================================================================
     * Arithmetic Operations - Handle-based wrappers for js_bigint_* functions
     * These methods accept JSBigIntHandle parameters and return handles,
     * avoiding the need for raw pointer access outside the handle class.
     * ===================================================================== */
    
    /* Get sign bit (0 or 1) - wrapper for js_bigint_sign */
    int sign_bit() const {
        JSBigInt* p = get_ptr();
        if (!p || p->len == 0) return 0;
        return p->tab[p->len - 1] >> (JS_LIMB_BITS - 1);
    }
    
    /* Addition: this + other (handles only) */
    JSBigIntHandle add(JSContextHandle ctx, const JSBigIntHandle& other) const;
    
    /* Subtraction: this - other (handles only) */
    JSBigIntHandle sub(JSContextHandle ctx, const JSBigIntHandle& other) const;
    
    /* Multiplication: this * other (handles only) */
    JSBigIntHandle mul(JSContextHandle ctx, const JSBigIntHandle& other) const;
    
    /* Division: this / other (handles only) */
    JSBigIntHandle div(JSContextHandle ctx, const JSBigIntHandle& other) const;
    
    /* Remainder: this % other (handles only) */
    JSBigIntHandle rem(JSContextHandle ctx, const JSBigIntHandle& other) const;
    
    /* Power: this ^ exponent (handles only) */
    JSBigIntHandle pow(JSContextHandle ctx, const JSBigIntHandle& exponent) const;
    
    /* Bitwise NOT: ~this */
    JSBigIntHandle bit_not(JSContextHandle ctx) const;
    
    /* Bitwise AND: this & other */
    JSBigIntHandle bit_and(JSContextHandle ctx, const JSBigIntHandle& other) const;
    
    /* Bitwise OR: this | other */
    JSBigIntHandle bit_or(JSContextHandle ctx, const JSBigIntHandle& other) const;
    
    /* Bitwise XOR: this ^ other */
    JSBigIntHandle bit_xor(JSContextHandle ctx, const JSBigIntHandle& other) const;
    
    /* Left shift: this << n */
    JSBigIntHandle shl(JSContextHandle ctx, unsigned int n) const;
    
    /* Right shift: this >> n */
    JSBigIntHandle shr(JSContextHandle ctx, unsigned int n) const;
    
    /* Negation: -this */
    JSBigIntHandle neg(JSContextHandle ctx) const;
    
    /* Comparison: returns -1, 0, or 1 */
    int cmp(JSContextHandle ctx, const JSBigIntHandle& other) const;
    
    /* Convert to double */
    double to_double(JSContextHandle ctx) const;
    
    /* =====================================================================
     * Conversion operators for interoperability
     * ===================================================================== */
    
    /* Convert to GCHandle for handle-based APIs */
    operator GCHandle() const {
        return handle_;
    }
    
    /* Comparison operators */
    bool operator==(const JSBigIntHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSBigIntHandle& other) const {
        return handle_ != other.handle_;
    }
    
    bool operator==(GCHandle h) const {
        return handle_ == h;
    }
    
    bool operator!=(GCHandle h) const {
        return handle_ != h;
    }
    
    bool operator==(decltype(nullptr)) const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    bool operator!=(decltype(nullptr)) const {
        return handle_ != GC_HANDLE_NULL;
    }
};


/* ============================================================================
 * C++ JSModuleDefHandle Class - Safe GC Handle Wrapper
 * ============================================================================
 * 
 * This class wraps a GCHandle to provide safe access to JSModuleDef instances.
 * The key feature is that it NEVER stores a raw JSModuleDef pointer - instead,
 * it dereferences the handle on every property access to get a fresh pointer.
 * This ensures that the pointer is always valid even after GC compaction.
 * 
 * USAGE:
 *   JSModuleDefHandle m = JS_VALUE_GET_MODULE_HANDLE(val);
 *   JSAtom name = m.module_name();  // Fresh dereference
 *   m.set_resolved(true);           // Dereferences handle to set value
 */

class JSModuleDefHandle {
private:
    GCHandle handle_;
    
    /* Get fresh pointer - called on every access */
    JSModuleDef* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSModuleDef*)gc_deref(handle_);
    }

public:
    /* Default constructor - null handle */
    JSModuleDefHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit JSModuleDefHandle(GCHandle handle) : handle_(handle) {}
    
    /* Note: No raw pointer constructor - use GCHandle constructor to avoid GC unsafety */
    
    /* Copy constructor */
    JSModuleDefHandle(const JSModuleDefHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    JSModuleDefHandle(JSModuleDefHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    JSModuleDefHandle& operator=(const JSModuleDefHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    JSModuleDefHandle& operator=(JSModuleDefHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Assignment from nullptr */
    JSModuleDefHandle& operator=(decltype(nullptr)) {
        handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Get the underlying handle */
    GCHandle handle() const { return handle_; }
    
    /* Check if handle is valid */
    bool valid() const { 
        return handle_ != GC_HANDLE_NULL && gc_handle_is_valid(handle_);
    }
    
    /* Check if handle is null */
    bool is_null() const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    /* Explicit bool conversion */
    explicit operator bool() const { return valid(); }
    
    /* =====================================================================
     * Property accessors - each dereferences handle to get fresh pointer
     * ===================================================================== */
    
    /* module_name access */
    JSAtom module_name() const {
        JSModuleDef* p = get_ptr();
        return p ? p->module_name : 0;
    }
    
    void set_module_name(JSAtom val) {
        JSModuleDef* p = get_ptr();
        if (p) p->module_name = val;
    }
    
    /* Note: modules are tracked via JSContext::loaded_modules, not intrusive list */
    
    /* module_ns access */
    GCValue module_ns() const {
        JSModuleDef* p = get_ptr();
        return p ? p->module_ns : GC_UNDEFINED;
    }
    
    void set_module_ns(const GCValue& val) {
        JSModuleDef* p = get_ptr();
        if (p) {
            p->module_ns = val;
            gc_write_barrier_for_heap_slot(&p->module_ns, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* func_obj access */
    GCValue func_obj() const {
        JSModuleDef* p = get_ptr();
        return p ? p->func_obj : GC_UNDEFINED;
    }
    
    void set_func_obj(const GCValue& val) {
        JSModuleDef* p = get_ptr();
        if (p) {
            p->func_obj = val;
            gc_write_barrier_for_heap_slot(&p->func_obj, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* init_func access */
    JSModuleInitFunc* init_func() const {
        JSModuleDef* p = get_ptr();
        return p ? p->init_func : nullptr;
    }
    
    void set_init_func(JSModuleInitFunc* val) {
        JSModuleDef* p = get_ptr();
        if (p) p->init_func = val;
    }
    
    /* has_tla access */
    BOOL has_tla() const {
        JSModuleDef* p = get_ptr();
        return p ? p->has_tla : FALSE;
    }
    
    void set_has_tla(BOOL val) {
        JSModuleDef* p = get_ptr();
        if (p) p->has_tla = val;
    }
    
    /* resolved access */
    BOOL resolved() const {
        JSModuleDef* p = get_ptr();
        return p ? p->resolved : FALSE;
    }
    
    void set_resolved(BOOL val) {
        JSModuleDef* p = get_ptr();
        if (p) p->resolved = val;
    }
    
    /* func_created access */
    BOOL func_created() const {
        JSModuleDef* p = get_ptr();
        return p ? p->func_created : FALSE;
    }
    
    void set_func_created(BOOL val) {
        JSModuleDef* p = get_ptr();
        if (p) p->func_created = val;
    }
    
    /* status access */
    JSModuleStatus status() const {
        JSModuleDef* p = get_ptr();
        return p ? p->status : JS_MODULE_STATUS_UNLINKED;
    }
    
    void set_status(JSModuleStatus val) {
        JSModuleDef* p = get_ptr();
        if (p) p->status = val;
    }
    
    /* dfs_index access */
    int dfs_index() const {
        JSModuleDef* p = get_ptr();
        return p ? p->dfs_index : 0;
    }
    
    void set_dfs_index(int val) {
        JSModuleDef* p = get_ptr();
        if (p) p->dfs_index = val;
    }
    
    /* dfs_ancestor_index access */
    int dfs_ancestor_index() const {
        JSModuleDef* p = get_ptr();
        return p ? p->dfs_ancestor_index : 0;
    }
    
    void set_dfs_ancestor_index(int val) {
        JSModuleDef* p = get_ptr();
        if (p) p->dfs_ancestor_index = val;
    }
    
    /* stack_prev_handle access */
    GCHandle stack_prev_handle() const {
        JSModuleDef* p = get_ptr();
        return p ? p->stack_prev_handle : GC_HANDLE_NULL;
    }
    
    void set_stack_prev_handle(GCHandle val) {
        JSModuleDef* p = get_ptr();
        if (p) {
            p->stack_prev_handle = val;
            gc_write_barrier_for_heap_slot(&p->stack_prev_handle, val);
        }
    }
    
    /* async_parent_modules_handle access */
    GCHandle async_parent_modules_handle() const {
        JSModuleDef* p = get_ptr();
        return p ? p->async_parent_modules_handle : GC_HANDLE_NULL;
    }
    
    void set_async_parent_modules_handle(GCHandle val) {
        JSModuleDef* p = get_ptr();
        if (p) {
            p->async_parent_modules_handle = val;
            gc_write_barrier_for_heap_slot(&p->async_parent_modules_handle, val);
        }
    }
    
    /* async_parent_modules_count access */
    int async_parent_modules_count() const {
        JSModuleDef* p = get_ptr();
        return p ? p->async_parent_modules_count : 0;
    }
    
    void set_async_parent_modules_count(int val) {
        JSModuleDef* p = get_ptr();
        if (p) p->async_parent_modules_count = val;
    }
    
    /* async_parent_modules_size access */
    int async_parent_modules_size() const {
        JSModuleDef* p = get_ptr();
        return p ? p->async_parent_modules_size : 0;
    }
    
    void set_async_parent_modules_size(int val) {
        JSModuleDef* p = get_ptr();
        if (p) p->async_parent_modules_size = val;
    }
    
    /* Pointer to async_parent_modules_count for js_resize_array */
    int* async_parent_modules_count_ptr() {
        JSModuleDef* p = get_ptr();
        return p ? &p->async_parent_modules_count : nullptr;
    }
    
    /* Pointer to async_parent_modules_size for js_resize_array */
    int* async_parent_modules_size_ptr() {
        JSModuleDef* p = get_ptr();
        return p ? &p->async_parent_modules_size : nullptr;
    }
    
    /* Pointer to async_parent_modules_handle pointer for js_resize_array */
    GCHandle** async_parent_modules_handle_ptr_ptr() {
        JSModuleDef* p = get_ptr();
        return p ? (GCHandle**)&p->async_parent_modules_handle : nullptr;
    }
    
    /* pending_async_dependencies access */
    int pending_async_dependencies() const {
        JSModuleDef* p = get_ptr();
        return p ? p->pending_async_dependencies : 0;
    }
    
    void set_pending_async_dependencies(int val) {
        JSModuleDef* p = get_ptr();
        if (p) p->pending_async_dependencies = val;
    }
    
    /* async_evaluation access */
    BOOL async_evaluation() const {
        JSModuleDef* p = get_ptr();
        return p ? p->async_evaluation : FALSE;
    }
    
    void set_async_evaluation(BOOL val) {
        JSModuleDef* p = get_ptr();
        if (p) p->async_evaluation = val;
    }
    
    /* async_evaluation_timestamp access */
    int64_t async_evaluation_timestamp() const {
        JSModuleDef* p = get_ptr();
        return p ? p->async_evaluation_timestamp : 0;
    }
    
    void set_async_evaluation_timestamp(int64_t val) {
        JSModuleDef* p = get_ptr();
        if (p) p->async_evaluation_timestamp = val;
    }
    
    /* cycle_root_handle access */
    GCHandle cycle_root_handle() const {
        JSModuleDef* p = get_ptr();
        return p ? p->cycle_root_handle : GC_HANDLE_NULL;
    }
    
    void set_cycle_root_handle(GCHandle val) {
        JSModuleDef* p = get_ptr();
        if (p) {
            p->cycle_root_handle = val;
            gc_write_barrier_for_heap_slot(&p->cycle_root_handle, val);
        }
    }
    
    /* promise access */
    GCValue promise() const {
        JSModuleDef* p = get_ptr();
        return p ? p->promise : GC_UNDEFINED;
    }
    
    void set_promise(const GCValue& val) {
        JSModuleDef* p = get_ptr();
        if (p) {
            p->promise = val;
            gc_write_barrier_for_heap_slot(&p->promise, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* eval_has_exception access */
    BOOL eval_has_exception() const {
        JSModuleDef* p = get_ptr();
        return p ? p->eval_has_exception : FALSE;
    }
    
    void set_eval_has_exception(BOOL val) {
        JSModuleDef* p = get_ptr();
        if (p) p->eval_has_exception = val;
    }
    
    /* eval_exception access */
    GCValue eval_exception() const {
        JSModuleDef* p = get_ptr();
        return p ? p->eval_exception : GC_UNDEFINED;
    }
    
    void set_eval_exception(const GCValue& val) {
        JSModuleDef* p = get_ptr();
        if (p) {
            p->eval_exception = val;
            gc_write_barrier_for_heap_slot(&p->eval_exception, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* meta_obj access */
    GCValue meta_obj() const {
        JSModuleDef* p = get_ptr();
        return p ? p->meta_obj : GC_UNDEFINED;
    }
    
    void set_meta_obj(const GCValue& val) {
        JSModuleDef* p = get_ptr();
        if (p) {
            p->meta_obj = val;
            gc_write_barrier_for_heap_slot(&p->meta_obj, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* private_value access */
    GCValue private_value() const {
        JSModuleDef* p = get_ptr();
        return p ? p->private_value : GC_UNDEFINED;
    }
    
    void set_private_value(const GCValue& val) {
        JSModuleDef* p = get_ptr();
        if (p) {
            p->private_value = val;
            gc_write_barrier_for_heap_slot(&p->private_value, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* Pointer to private_value for set_value() API */
    GCValue* private_value_ptr() {
        JSModuleDef* p = get_ptr();
        return p ? &p->private_value : nullptr;
    }
    
    /* =====================================================================
     * Array pointer accessors - for internal use with C arrays
     * These return raw pointers for array access patterns that can't
     * be easily converted to handle-based access
     * ===================================================================== */
    
    /* =====================================================================
     * GCDataArray accessors for module entry arrays
     * ===================================================================== */
    
    /* req_module_entries array access - returns GCDataArray reference */
    GCDataArray<JSReqModuleEntry>& req_module_entries() const {
        JSModuleDef* p = get_ptr();
        static GCDataArray<JSReqModuleEntry> empty;
        return p ? p->req_module_entries : empty;
    }
    
    int req_module_entries_count() const {
        JSModuleDef* p = get_ptr();
        return p ? p->req_module_entries.count() : 0;
    }
    
    /* export_entries array access - returns GCDataArray reference */
    GCDataArray<JSExportEntry>& export_entries() const {
        JSModuleDef* p = get_ptr();
        static GCDataArray<JSExportEntry> empty;
        return p ? p->export_entries : empty;
    }
    
    int export_entries_count() const {
        JSModuleDef* p = get_ptr();
        return p ? p->export_entries.count() : 0;
    }
    
    /* star_export_entries array access - returns GCDataArray reference */
    GCDataArray<JSStarExportEntry>& star_export_entries() const {
        JSModuleDef* p = get_ptr();
        static GCDataArray<JSStarExportEntry> empty;
        return p ? p->star_export_entries : empty;
    }
    
    int star_export_entries_count() const {
        JSModuleDef* p = get_ptr();
        return p ? p->star_export_entries.count() : 0;
    }
    
    /* import_entries array access - returns GCDataArray reference */
    GCDataArray<JSImportEntry>& import_entries() const {
        JSModuleDef* p = get_ptr();
        static GCDataArray<JSImportEntry> empty;
        return p ? p->import_entries : empty;
    }
    
    int import_entries_count() const {
        JSModuleDef* p = get_ptr();
        return p ? p->import_entries.count() : 0;
    }
    
    /* resolving_funcs access */
    GCValue resolving_func(int idx) const {
        JSModuleDef* p = get_ptr();
        if (p && idx >= 0 && idx < 2) return p->resolving_funcs[idx];
        return GC_UNDEFINED;
    }
    
    void set_resolving_func(int idx, const GCValue& val) {
        JSModuleDef* p = get_ptr();
        if (p && idx >= 0 && idx < 2) {
            p->resolving_funcs[idx] = val;
            gc_write_barrier_for_heap_slot(&p->resolving_funcs[idx], GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* =====================================================================
     * Conversion operators for interoperability
     * ===================================================================== */
    
    /* Convert to GCHandle for handle-based APIs */
    operator GCHandle() const {
        return handle_;
    }
    
    /* Comparison operators */
    bool operator==(const JSModuleDefHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSModuleDefHandle& other) const {
        return handle_ != other.handle_;
    }
    
    bool operator==(GCHandle h) const {
        return handle_ == h;
    }
    
    bool operator!=(GCHandle h) const {
        return handle_ != h;
    }
    
    bool operator==(decltype(nullptr)) const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    bool operator!=(decltype(nullptr)) const {
        return handle_ != GC_HANDLE_NULL;
    }
};


/* ============================================================================
 * C++ JSAsyncFunctionStateHandle Class - Safe GC Handle Wrapper
 * ============================================================================
 * 
 * This class wraps a GCHandle to provide safe access to JSAsyncFunctionState
 * instances. The key feature is that it NEVER stores a raw pointer - instead,
 * it dereferences the handle on every property access to get a fresh pointer.
 * This ensures that the pointer is always valid even after GC compaction.
 * 
 * USAGE:
 *   JSAsyncFunctionStateHandle s = JS_VALUE_GET_ASYNC_FUNC_HANDLE(val);
 *   int argc = s.argc();  // Fresh dereference
 *   s.set_is_completed(true);
 */

class JSAsyncFunctionStateHandle {
private:
    GCHandle handle_;
    
    /* Get fresh pointer - called on every access */
    JSAsyncFunctionState* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSAsyncFunctionState*)gc_deref(handle_);
    }

public:
    /* Default constructor - null handle */
    JSAsyncFunctionStateHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit JSAsyncFunctionStateHandle(GCHandle handle) : handle_(handle) {}
    
    /* Get raw pointer for GC marking purposes - INTERNAL USE ONLY */
    JSAsyncFunctionState* mark_ptr() const {
        return get_ptr();
    }
    
    /* Copy constructor */
    JSAsyncFunctionStateHandle(const JSAsyncFunctionStateHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    JSAsyncFunctionStateHandle(JSAsyncFunctionStateHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    JSAsyncFunctionStateHandle& operator=(const JSAsyncFunctionStateHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    JSAsyncFunctionStateHandle& operator=(JSAsyncFunctionStateHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Assignment from nullptr */
    JSAsyncFunctionStateHandle& operator=(decltype(nullptr)) {
        handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Get the underlying handle */
    GCHandle handle() const { return handle_; }
    
    /* Check if handle is valid */
    bool valid() const { 
        return handle_ != GC_HANDLE_NULL && gc_handle_is_valid(handle_);
    }
    
    /* Check if handle is null */
    bool is_null() const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    /* Explicit bool conversion */
    explicit operator bool() const { return valid(); }
    
    /* =====================================================================
     * Property accessors - each dereferences handle to get fresh pointer
     * ===================================================================== */
    
    /* this_val access */
    GCValue this_val() const {
        JSAsyncFunctionState* p = get_ptr();
        return p ? p->this_val : GC_UNDEFINED;
    }
    
    void set_this_val(const GCValue& val) {
        JSAsyncFunctionState* p = get_ptr();
        if (p) {
            p->this_val = val;
            gc_write_barrier_for_heap_slot(&p->this_val, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* argc access */
    int argc() const {
        JSAsyncFunctionState* p = get_ptr();
        return p ? p->argc : 0;
    }
    
    void set_argc(int val) {
        JSAsyncFunctionState* p = get_ptr();
        if (p) p->argc = val;
    }
    
    /* throw_flag access */
    BOOL throw_flag() const {
        JSAsyncFunctionState* p = get_ptr();
        return p ? p->throw_flag : FALSE;
    }
    
    void set_throw_flag(BOOL val) {
        JSAsyncFunctionState* p = get_ptr();
        if (p) p->throw_flag = val;
    }
    
    /* is_completed access */
    BOOL is_completed() const {
        JSAsyncFunctionState* p = get_ptr();
        return p ? p->is_completed : FALSE;
    }
    
    void set_is_completed(BOOL val) {
        JSAsyncFunctionState* p = get_ptr();
        if (p) p->is_completed = val;
    }
    
    /* resolving_funcs access */
    GCValue resolving_func(int idx) const {
        JSAsyncFunctionState* p = get_ptr();
        if (p && idx >= 0 && idx < 2) return p->resolving_funcs[idx];
        return GC_UNDEFINED;
    }
    
    void set_resolving_func(int idx, const GCValue& val) {
        JSAsyncFunctionState* p = get_ptr();
        if (p && idx >= 0 && idx < 2) {
            p->resolving_funcs[idx] = val;
            gc_write_barrier_for_heap_slot(&p->resolving_funcs[idx], GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* resolving_funcs array access - returns pointer to array for APIs that need it */
    GCValue* resolving_funcs_ptr() const {
        JSAsyncFunctionState* p = get_ptr();
        return p ? p->resolving_funcs : nullptr;
    }
    
    /* self_handle access - handle back to this async function state */
    GCHandle self_handle() const {
        JSAsyncFunctionState* p = get_ptr();
        return p ? p->self_handle : GC_HANDLE_NULL;
    }
    
    void set_self_handle(GCHandle h) {
        JSAsyncFunctionState* p = get_ptr();
        if (p) {
            p->self_handle = h;
            gc_write_barrier_for_heap_slot(&p->self_handle, h);
        }
    }
    
    /* frame access - returns pointer to JSStackFrame for low-level operations */
    /* USE WITH CAUTION - pointer becomes invalid after any operation that may trigger GC */
    JSStackFrame* frame_ptr() const {
        JSAsyncFunctionState* p = get_ptr();
        return p ? &p->frame : nullptr;
    }
    
    /* =====================================================================
     * Stack operations - safe wrappers for common patterns
     * ===================================================================== */
    
    /* Get the current stack pointer offset */
    uint32_t sp_offset() const {
        JSAsyncFunctionState* p = get_ptr();
        return p ? p->frame.sp_offset : 0;
    }
    
    /* Set the current stack pointer offset */
    void set_sp_offset(uint32_t offset) {
        JSAsyncFunctionState* p = get_ptr();
        if (p) p->frame.sp_offset = offset;
    }
    
    /* Get the stack top value - returns undefined if stack is empty */
    GCValue stack_top() const {
        JSAsyncFunctionState* p = get_ptr();
        if (p && p->frame.sp_offset > 0) {
            GCValue* var_buf = JS_SF_VAR_BUF(&p->frame);
            return var_buf[p->frame.sp_offset - 1];
        }
        return GC_UNDEFINED;
    }
    
    /* Set the stack top value */
    void set_stack_top(const GCValue& val) {
        JSAsyncFunctionState* p = get_ptr();
        if (p && p->frame.sp_offset > 0) {
            GCValue* var_buf = JS_SF_VAR_BUF(&p->frame);
            var_buf[p->frame.sp_offset - 1] = val;
            gc_write_barrier_for_heap_slot(&var_buf[p->frame.sp_offset - 1], GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* Pop a value from the stack */
    GCValue stack_pop() {
        JSAsyncFunctionState* p = get_ptr();
        if (p && p->frame.sp_offset > 0) {
            GCValue* var_buf = JS_SF_VAR_BUF(&p->frame);
            p->frame.sp_offset--;
            return var_buf[p->frame.sp_offset];
        }
        return GC_UNDEFINED;
    }
    
    /* Push a value onto the stack */
    void stack_push(const GCValue& val) {
        JSAsyncFunctionState* p = get_ptr();
        if (p) {
            GCValue* var_buf = JS_SF_VAR_BUF(&p->frame);
            var_buf[p->frame.sp_offset] = val;
            p->frame.sp_offset++;
        }
    }
    
    /* Clear the stack top value */
    void clear_stack_top() {
        JSAsyncFunctionState* p = get_ptr();
        if (p && p->frame.sp_offset > 0) {
            GCValue* var_buf = JS_SF_VAR_BUF(&p->frame);
            var_buf[p->frame.sp_offset - 1] = GC_UNDEFINED;
        }
    }
    
    /* =====================================================================
     * Frame field accessors
     * ===================================================================== */
    
    /* Get cur_func from frame */
    GCValue cur_func() const {
        JSAsyncFunctionState* p = get_ptr();
        return p ? p->frame.cur_func : GC_UNDEFINED;
    }
    
    /* Set cur_func in frame */
    void set_cur_func(const GCValue& val) {
        JSAsyncFunctionState* p = get_ptr();
        if (p) {
            p->frame.cur_func = val;
            gc_write_barrier_for_heap_slot(&p->frame.cur_func, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* Get arg_buf offset from frame */
    uint32_t arg_buf_offset() const {
        JSAsyncFunctionState* p = get_ptr();
        return p ? p->frame.arg_buf_offset : 0;
    }
    
    /* Set arg_buf offset in frame */
    void set_arg_buf_offset(uint32_t offset) {
        JSAsyncFunctionState* p = get_ptr();
        if (p) p->frame.arg_buf_offset = offset;
    }
    
    /* Get var_buf offset from frame */
    uint32_t var_buf_offset() const {
        JSAsyncFunctionState* p = get_ptr();
        return p ? p->frame.var_buf_offset : 0;
    }
    
    /* Set var_buf offset in frame */
    void set_var_buf_offset(uint32_t offset) {
        JSAsyncFunctionState* p = get_ptr();
        if (p) p->frame.var_buf_offset = offset;
    }
    
    /* Get var_refs offset from frame */
    uint32_t var_refs_offset() const {
        JSAsyncFunctionState* p = get_ptr();
        return p ? p->frame.var_refs_offset : 0;
    }
    
    /* Set var_refs offset in frame */
    void set_var_refs_offset(uint32_t offset) {
        JSAsyncFunctionState* p = get_ptr();
        if (p) p->frame.var_refs_offset = offset;
    }
    
    /* Get arg_count from frame */
    int arg_count() const {
        JSAsyncFunctionState* p = get_ptr();
        return p ? p->frame.arg_count : 0;
    }
    
    /* Set arg_count in frame */
    void set_arg_count(int count) {
        JSAsyncFunctionState* p = get_ptr();
        if (p) p->frame.arg_count = count;
    }
    
    /* Get js_mode from frame */
    int js_mode() const {
        JSAsyncFunctionState* p = get_ptr();
        return p ? p->frame.js_mode : 0;
    }
    
    /* Set js_mode in frame */
    void set_js_mode(int mode) {
        JSAsyncFunctionState* p = get_ptr();
        if (p) p->frame.js_mode = mode;
    }
    
    /* Get pc_offset from frame */
    uint32_t pc_offset() const {
        JSAsyncFunctionState* p = get_ptr();
        return p ? p->frame.pc_offset : 0;
    }
    
    /* Set pc_offset in frame */
    void set_pc_offset(uint32_t offset) {
        JSAsyncFunctionState* p = get_ptr();
        if (p) p->frame.pc_offset = offset;
    }
    
    /* Get prev_frame_handle from frame */
    GCHandle prev_frame_handle() const {
        JSAsyncFunctionState* p = get_ptr();
        return p ? p->frame.prev_frame_handle : GCHandle();
    }
    
    /* Set prev_frame_handle in frame */
    void set_prev_frame_handle(GCHandle handle) {
        JSAsyncFunctionState* p = get_ptr();
        if (p) {
            p->frame.prev_frame_handle = handle;
            gc_write_barrier_for_heap_slot(&p->frame.prev_frame_handle, handle);
        }
    }
    
    /* =====================================================================
     * Conversion operators for interoperability
     * ===================================================================== */
    
    /* Convert to GCHandle for handle-based APIs */
    operator GCHandle() const {
        return handle_;
    }
    
    /* Comparison operators */
    bool operator==(const JSAsyncFunctionStateHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSAsyncFunctionStateHandle& other) const {
        return handle_ != other.handle_;
    }
    
    bool operator==(GCHandle h) const {
        return handle_ == h;
    }
    
    bool operator!=(GCHandle h) const {
        return handle_ != h;
    }
    
    bool operator==(decltype(nullptr)) const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    bool operator!=(decltype(nullptr)) const {
        return handle_ != GC_HANDLE_NULL;
    }
};


/* ============================================================================
 * C++ JSJobEntryHandle Class - Safe GC Handle Wrapper
 * ============================================================================
 * 
 * This class wraps a GCHandle to provide safe access to JSJobEntry instances.
 * The key feature is that it NEVER stores a raw pointer - instead,
 * it dereferences the handle on every property access to get a fresh pointer.
 * This ensures that the pointer is always valid even after GC compaction.
 * 
 * USAGE:
 *   JSJobEntryHandle job = JSJobEntryHandle(job_handle);
 *   GCHandle realm = job.realm_handle();  // Fresh dereference
 *   int argc = job.argc();
 */

class JSJobEntryHandle {
private:
    GCHandle handle_;
    
    /* Get fresh pointer - called on every access */
    JSJobEntry* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSJobEntry*)gc_deref(handle_);
    }

public:
    /* Default constructor - null handle */
    JSJobEntryHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit JSJobEntryHandle(GCHandle handle) : handle_(handle) {}
    
    /* Note: No raw pointer constructor - use GCHandle constructor to avoid GC unsafety */
    
    /* Copy constructor */
    JSJobEntryHandle(const JSJobEntryHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    JSJobEntryHandle(JSJobEntryHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    JSJobEntryHandle& operator=(const JSJobEntryHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    JSJobEntryHandle& operator=(JSJobEntryHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Assignment from nullptr */
    JSJobEntryHandle& operator=(decltype(nullptr)) {
        handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Get the underlying handle */
    GCHandle handle() const { return handle_; }
    
    /* Check if handle is valid */
    bool valid() const { 
        return handle_ != GC_HANDLE_NULL && gc_handle_is_valid(handle_);
    }
    
    /* Check if handle is null */
    bool is_null() const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    /* Explicit bool conversion */
    explicit operator bool() const { return valid(); }
    
    /* =====================================================================
     * Property accessors - each dereferences handle to get fresh pointer
     * ===================================================================== */
    
    /* realm_handle access */
    GCHandle realm_handle() const {
        JSJobEntry* p = get_ptr();
        return p ? p->realm_handle : GC_HANDLE_NULL;
    }
    
    void set_realm_handle(GCHandle val) {
        JSJobEntry* p = get_ptr();
        if (p) {
            p->realm_handle = val;
            gc_write_barrier_for_heap_slot(&p->realm_handle, val);
        }
    }
    
    /* job_func access */
    JSJobFunc* job_func() const {
        JSJobEntry* p = get_ptr();
        return p ? p->job_func : nullptr;
    }
    
    void set_job_func(JSJobFunc* val) {
        JSJobEntry* p = get_ptr();
        if (p) p->job_func = val;
    }
    
    /* argc access */
    int argc() const {
        JSJobEntry* p = get_ptr();
        return p ? p->argc : 0;
    }
    
    void set_argc(int val) {
        JSJobEntry* p = get_ptr();
        if (p) p->argc = val;
    }
    
    /* argv access - returns pointer to argv array */
    GCValue* argv() const {
        JSJobEntry* p = get_ptr();
        return p ? p->argv : nullptr;
    }
    
    /* Get argv at specific index */
    GCValue argv_at(int idx) const {
        JSJobEntry* p = get_ptr();
        if (p && idx >= 0 && idx < p->argc) return p->argv[idx];
        return GC_UNDEFINED;
    }
    
    void set_argv_at(int idx, const GCValue& val) {
        JSJobEntry* p = get_ptr();
        if (p && idx >= 0 && idx < p->argc) {
            p->argv[idx] = val;
            gc_write_barrier_for_heap_slot(&p->argv[idx], GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* =====================================================================
     * Conversion operators for interoperability
     * ===================================================================== */
    
    /* Convert to GCHandle for handle-based APIs */
    operator GCHandle() const {
        return handle_;
    }
    
    /* Comparison operators */
    bool operator==(const JSJobEntryHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSJobEntryHandle& other) const {
        return handle_ != other.handle_;
    }
    
    bool operator==(GCHandle h) const {
        return handle_ == h;
    }
    
    bool operator!=(GCHandle h) const {
        return handle_ != h;
    }
    
    bool operator==(decltype(nullptr)) const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    bool operator!=(decltype(nullptr)) const {
        return handle_ != GC_HANDLE_NULL;
    }
};


/* ============================================================================
 * C++ JSFunctionBytecodeHandle Class - Safe GC Handle Wrapper
 * ============================================================================
 * 
 * This class wraps a GCHandle to provide safe access to JSFunctionBytecode
 * instances. The key feature is that it NEVER stores a raw pointer - instead,
 * it dereferences the handle on every property access to get a fresh pointer.
 * This ensures that the pointer is always valid even after GC compaction.
 * 
 * USAGE:
 *   JSFunctionBytecodeHandle b = JS_VALUE_GET_BYTECODE_HANDLE(val);
 *   uint8_t js_mode = b.js_mode();  // Fresh dereference
 *   b.set_arg_count(5);             // Dereferences handle to set value
 */

class JSFunctionBytecodeHandle {
private:
    GCHandle handle_;
    
    /* Get fresh pointer - called on every access */
    JSFunctionBytecode* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSFunctionBytecode*)gc_deref(handle_);
    }

public:
    /* Default constructor - null handle */
    JSFunctionBytecodeHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit JSFunctionBytecodeHandle(GCHandle handle) : handle_(handle) {}
    
    /* Note: No raw pointer constructor - use GCHandle constructor to avoid GC unsafety */
    
    /* Copy constructor */
    JSFunctionBytecodeHandle(const JSFunctionBytecodeHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    JSFunctionBytecodeHandle(JSFunctionBytecodeHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    JSFunctionBytecodeHandle& operator=(const JSFunctionBytecodeHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    JSFunctionBytecodeHandle& operator=(JSFunctionBytecodeHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Assignment from nullptr */
    JSFunctionBytecodeHandle& operator=(decltype(nullptr)) {
        handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Get the underlying handle */
    GCHandle handle() const { return handle_; }
    
    /* Check if handle is valid */
    bool valid() const { 
        return handle_ != GC_HANDLE_NULL && gc_handle_is_valid(handle_);
    }
    
    /* Check if handle is null */
    bool is_null() const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    /* Explicit bool conversion */
    explicit operator bool() const { return valid(); }
    
    /* =====================================================================
     * Property accessors - each dereferences handle to get fresh pointer
     * ===================================================================== */
    
    /* js_mode access */
    uint8_t js_mode() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->js_mode : 0;
    }
    
    void set_js_mode(uint8_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->js_mode = val;
    }
    
    /* has_prototype access */
    uint8_t has_prototype() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->has_prototype : 0;
    }
    
    void set_has_prototype(uint8_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->has_prototype = val;
    }
    
    /* has_simple_parameter_list access */
    uint8_t has_simple_parameter_list() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->has_simple_parameter_list : 0;
    }
    
    void set_has_simple_parameter_list(uint8_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->has_simple_parameter_list = val;
    }
    
    /* is_derived_class_constructor access */
    uint8_t is_derived_class_constructor() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->is_derived_class_constructor : 0;
    }
    
    void set_is_derived_class_constructor(uint8_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->is_derived_class_constructor = val;
    }
    
    /* need_home_object access */
    uint8_t need_home_object() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->need_home_object : 0;
    }
    
    void set_need_home_object(uint8_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->need_home_object = val;
    }
    
    /* func_kind access */
    uint8_t func_kind() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->func_kind : 0;
    }
    
    void set_func_kind(uint8_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->func_kind = val;
    }
    
    /* new_target_allowed access */
    uint8_t new_target_allowed() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->new_target_allowed : 0;
    }
    
    void set_new_target_allowed(uint8_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->new_target_allowed = val;
    }
    
    /* super_call_allowed access */
    uint8_t super_call_allowed() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->super_call_allowed : 0;
    }
    
    void set_super_call_allowed(uint8_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->super_call_allowed = val;
    }
    
    /* super_allowed access */
    uint8_t super_allowed() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->super_allowed : 0;
    }
    
    void set_super_allowed(uint8_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->super_allowed = val;
    }
    
    /* arguments_allowed access */
    uint8_t arguments_allowed() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->arguments_allowed : 0;
    }
    
    void set_arguments_allowed(uint8_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->arguments_allowed = val;
    }
    
    /* has_debug access */
    uint8_t has_debug() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->has_debug : 0;
    }
    
    void set_has_debug(uint8_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->has_debug = val;
    }
    
    /* read_only_bytecode access */
    uint8_t read_only_bytecode() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->read_only_bytecode : 0;
    }
    
    void set_read_only_bytecode(uint8_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->read_only_bytecode = val;
    }
    
    /* is_direct_or_indirect_eval access */
    uint8_t is_direct_or_indirect_eval() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->is_direct_or_indirect_eval : 0;
    }
    
    void set_is_direct_or_indirect_eval(uint8_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->is_direct_or_indirect_eval = val;
    }
    
    /* byte_code_handle access */
    GCHandle byte_code_handle() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->byte_code_handle : GC_HANDLE_NULL;
    }
    
    void set_byte_code_handle(GCHandle val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) {
            p->byte_code_handle = val;
            gc_write_barrier_for_heap_slot(&p->byte_code_handle, val);
        }
    }
    
    /* byte_code_offset access */
    uint32_t byte_code_offset() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->byte_code_offset : 0;
    }
    
    void set_byte_code_offset(uint32_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->byte_code_offset = val;
    }
    
    /* byte_code_buf access - fresh dereference from handle */
    uint8_t* byte_code_buf() const {
        JSFunctionBytecode* p = get_ptr();
        if (!p) return nullptr;
        if (p->byte_code_handle == GC_HANDLE_NULL) return nullptr;
        /* If self-handle (inline bytecode), add offset; otherwise use handle directly */
        if (p->byte_code_handle == handle_) {
            return (uint8_t*)gc_deref(p->byte_code_handle) + p->byte_code_offset;
        }
        return (uint8_t*)gc_deref(p->byte_code_handle);
    }
    
    /* Set byte_code_buf from raw pointer (legacy compatibility - does nothing) */
    void set_byte_code_buf(uint8_t* val) {
        /* No-op: pointer is derived from handle + offset */
        (void)val;
    }
    
    /* byte_code_len access */
    int byte_code_len() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->byte_code_len : 0;
    }
    
    void set_byte_code_len(int val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->byte_code_len = val;
    }
    
    /* func_name access */
    JSAtom func_name() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->func_name : 0;
    }
    
    void set_func_name(JSAtom val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->func_name = val;
    }
    
    /* vardefs_offset access */
    uint32_t vardefs_offset() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->vardefs_offset : 0;
    }
    
    void set_vardefs_offset(uint32_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->vardefs_offset = val;
    }
    
    /* closure_var_offset access */
    uint32_t closure_var_offset() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->closure_var_offset : 0;
    }
    
    void set_closure_var_offset(uint32_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->closure_var_offset = val;
    }
    
    /* arg_count access */
    uint16_t arg_count() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->arg_count : 0;
    }
    
    void set_arg_count(uint16_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->arg_count = val;
    }
    
    /* var_count access */
    uint16_t var_count() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->var_count : 0;
    }
    
    void set_var_count(uint16_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->var_count = val;
    }
    
    /* defined_arg_count access */
    uint16_t defined_arg_count() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->defined_arg_count : 0;
    }
    
    void set_defined_arg_count(uint16_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->defined_arg_count = val;
    }
    
    /* stack_size access */
    uint16_t stack_size() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->stack_size : 0;
    }
    
    void set_stack_size(uint16_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->stack_size = val;
    }
    
    /* var_ref_count access */
    uint16_t var_ref_count() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->var_ref_count : 0;
    }
    
    void set_var_ref_count(uint16_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->var_ref_count = val;
    }
    
    /* realm_handle access */
    GCHandle realm_handle() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->realm_handle : GC_HANDLE_NULL;
    }
    
    void set_realm_handle(GCHandle val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) {
            p->realm_handle = val;
            gc_write_barrier_for_heap_slot(&p->realm_handle, val);
        }
    }
    
    /* cpool_offset access */
    uint32_t cpool_offset() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->cpool_offset : 0;
    }
    
    void set_cpool_offset(uint32_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->cpool_offset = val;
    }
    
    /* cpool_count access */
    int cpool_count() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->cpool_count : 0;
    }
    
    void set_cpool_count(int val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->cpool_count = val;
    }
    
    /* closure_var_count access */
    int closure_var_count() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->closure_var_count : 0;
    }
    
    void set_closure_var_count(int val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->closure_var_count = val;
    }
    
    /* =====================================================================
     * Debug info accessors
     * ===================================================================== */
    
    /* debug.filename access */
    JSAtom debug_filename() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->debug.filename : 0;
    }
    
    void set_debug_filename(JSAtom val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->debug.filename = val;
    }
    
    /* debug.source_len access */
    int debug_source_len() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->debug.source_len : 0;
    }
    
    void set_debug_source_len(int val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->debug.source_len = val;
    }
    
    /* debug.pc2line_len access */
    int debug_pc2line_len() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->debug.pc2line_len : 0;
    }
    
    void set_debug_pc2line_len(int val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->debug.pc2line_len = val;
    }
    
    /* debug.pc2line_handle access */
    GCHandle debug_pc2line_handle() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->debug.pc2line_handle : GC_HANDLE_NULL;
    }
    
    void set_debug_pc2line_handle(GCHandle val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) {
            p->debug.pc2line_handle = val;
            gc_write_barrier_for_heap_slot(&p->debug.pc2line_handle, val);
        }
    }
    
    /* debug.pc2line_buf access - fresh dereference from handle */
    uint8_t* debug_pc2line_buf() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? (uint8_t*)gc_deref(p->debug.pc2line_handle) : nullptr;
    }
    
    /* debug.source_handle access */
    GCHandle debug_source_handle() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->debug.source_handle : GC_HANDLE_NULL;
    }
    
    void set_debug_source_handle(GCHandle val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) {
            p->debug.source_handle = val;
            gc_write_barrier_for_heap_slot(&p->debug.source_handle, val);
        }
    }
    
    /* debug.source access - fresh dereference from handle */
    char* debug_source() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? (char*)gc_deref(p->debug.source_handle) : nullptr;
    }
    
    /* =====================================================================
     * Array pointer accessors - for internal use with C arrays
     * These return raw pointers for array access patterns that can't
     * be easily converted to handle-based access
     * ===================================================================== */
    
    /* Get pointer to vardefs array (JSBytecodeVarDef) */
    void* vardefs_ptr() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? (uint8_t*)p + p->vardefs_offset : nullptr;
    }
    
    /* Get pointer to closure_var array (JSClosureVar) */
    void* closure_var_ptr() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? (uint8_t*)p + p->closure_var_offset : nullptr;
    }
    
    /* Get closure_var field at index - uses fresh dereference for GC safety
     * 
     * This method is used for runtime access to closure_var data in the interpreter.
     * It performs a fresh dereference via GCPin to ensure the pointer is valid
     * even if GC has compacted.
     */
    JSAtom closure_var_var_name(int index) const {
        JSFunctionBytecode* p = get_ptr();
        if (!p || index < 0 || index >= p->closure_var_count) return 0;  /* JS_ATOM_NULL = 0 */
        JSClosureVar* cv = (JSClosureVar*)((uint8_t*)p + p->closure_var_offset + index * sizeof(JSClosureVar));
        return cv->var_name;
    }
    
    uint8_t closure_var_closure_type(int index) const {
        JSFunctionBytecode* p = get_ptr();
        if (!p || index < 0 || index >= p->closure_var_count) return 0;
        JSClosureVar* cv = (JSClosureVar*)((uint8_t*)p + p->closure_var_offset + index * sizeof(JSClosureVar));
        return (uint8_t)cv->closure_type;
    }
    
    BOOL closure_var_is_lexical(int index) const {
        JSFunctionBytecode* p = get_ptr();
        if (!p || index < 0 || index >= p->closure_var_count) return FALSE;
        JSClosureVar* cv = (JSClosureVar*)((uint8_t*)p + p->closure_var_offset + index * sizeof(JSClosureVar));
        return cv->is_lexical;
    }
    
    BOOL closure_var_is_const(int index) const {
        JSFunctionBytecode* p = get_ptr();
        if (!p || index < 0 || index >= p->closure_var_count) return FALSE;
        JSClosureVar* cv = (JSClosureVar*)((uint8_t*)p + p->closure_var_offset + index * sizeof(JSClosureVar));
        return cv->is_const;
    }
    
    uint8_t closure_var_var_kind(int index) const {
        JSFunctionBytecode* p = get_ptr();
        if (!p || index < 0 || index >= p->closure_var_count) return 0;
        JSClosureVar* cv = (JSClosureVar*)((uint8_t*)p + p->closure_var_offset + index * sizeof(JSClosureVar));
        return cv->var_kind;
    }
    
    uint16_t closure_var_var_idx(int index) const {
        JSFunctionBytecode* p = get_ptr();
        if (!p || index < 0 || index >= p->closure_var_count) return 0;
        JSClosureVar* cv = (JSClosureVar*)((uint8_t*)p + p->closure_var_offset + index * sizeof(JSClosureVar));
        return cv->var_idx;
    }
    
    /* Get pointer to cpool array (GCValue) */
    GCValue* cpool_ptr() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? (GCValue*)((uint8_t*)p + p->cpool_offset) : nullptr;
    }
    
    /* Get cpool value at index - SAFE, performs fresh dereference */
    GCValue cpool_value(int index) const {
        JSFunctionBytecode* p = get_ptr();
        if (!p || index < 0 || index >= p->cpool_count) return GC_UNDEFINED;
        return ((GCValue*)((uint8_t*)p + p->cpool_offset))[index];
    }
    
    /* Set cpool value at index - SAFE, performs fresh dereference */
    void set_cpool_value(int index, const GCValue& val) const {
        JSFunctionBytecode* p = get_ptr();
        if (!p || index < 0 || index >= p->cpool_count) return;
        ((GCValue*)((uint8_t*)p + p->cpool_offset))[index] = val;
    }
    
    /* Get pointer to bytecode buffer at a given byte offset.
     * This is used for computing the actual bytecode buffer location
     * when the bytecode is stored inline after the struct.
     */
    uint8_t* bytecode_buf_at_offset(int byte_code_offset) const {
        JSFunctionBytecode* p = get_ptr();
        return p ? (uint8_t*)p + byte_code_offset : nullptr;
    }
    
    /* Set byte_code_handle to this object's handle (for inline bytecode) */
    void set_byte_code_handle_self() {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->byte_code_handle = handle_;
    }
    
    /* Set byte_code_buf to a computed offset location (inline bytecode) */
    void set_byte_code_buf_at_offset(int byte_code_offset) {
        JSFunctionBytecode* p = get_ptr();
        if (p) {
            p->byte_code_handle = handle_; /* Self-handle for inline bytecode */
        }
    }
    
    /* Get pointer to bytecode at stored offset from base (for inline bytecode) */
    uint8_t* byte_code_buf_at_offset_ptr() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? (uint8_t*)p + p->byte_code_offset : nullptr;
    }
    
    /* Zero out the entire struct memory.
     * USE WITH CAUTION - only valid after fresh allocation before any GC can occur.
     */
    void memset_zero(size_t size) {
        JSFunctionBytecode* p = get_ptr();
        if (p) memset(p, 0, size);
    }
    
    /* Copy data to the struct from a source buffer.
     * USE WITH CAUTION - only safe when no GC can occur.
     */
    void memcpy_from(const void* src, size_t size) {
        JSFunctionBytecode* p = get_ptr();
        if (p) memcpy(p, src, size);
    }
    
    /* Create a GCValue from this handle (for JS_MKPTR patterns) */
    GCValue to_gc_value(int tag) const {
        return GCValue(handle_, tag);
    }
    
    /* =====================================================================
     * Conversion operators for interoperability
     * ===================================================================== */
    
    /* Convert to GCHandle for handle-based APIs */
    operator GCHandle() const {
        return handle_;
    }
    
    /* Comparison operators */
    bool operator==(const JSFunctionBytecodeHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSFunctionBytecodeHandle& other) const {
        return handle_ != other.handle_;
    }
    
    bool operator==(GCHandle h) const {
        return handle_ == h;
    }
    
    bool operator!=(GCHandle h) const {
        return handle_ != h;
    }
    
    bool operator==(decltype(nullptr)) const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    bool operator!=(decltype(nullptr)) const {
        return handle_ != GC_HANDLE_NULL;
    }
    
    /* =====================================================================
     * Lazy function parsing accessors
     * ===================================================================== */
    
    /* func_parse_state access */
    uint8_t func_parse_state() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->func_parse_state : 0;
    }
    
    void set_func_parse_state(uint8_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->func_parse_state = val;
    }
    
    /* lazy_source access */
    const char* lazy_source() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->lazy_source : nullptr;
    }
    
    void set_lazy_source(const char* val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->lazy_source = val;
    }
    
    /* lazy_source_len access */
    uint32_t lazy_source_len() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->lazy_source_len : 0;
    }
    
    void set_lazy_source_len(uint32_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->lazy_source_len = val;
    }
    
    /* lazy_source_line access */
    uint32_t lazy_source_line() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->lazy_source_line : 0;
    }
    
    void set_lazy_source_line(uint32_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->lazy_source_line = val;
    }
    
    /* lazy_source_col access */
    uint32_t lazy_source_col() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->lazy_source_col : 0;
    }
    
    void set_lazy_source_col(uint32_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->lazy_source_col = val;
    }
    
    /* lazy_parent_scope access */
    GCHandle lazy_parent_scope() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->lazy_parent_scope : GC_HANDLE_NULL;
    }
    
    void set_lazy_parent_scope(GCHandle val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) {
            p->lazy_parent_scope = val;
            gc_write_barrier_for_heap_slot(&p->lazy_parent_scope, val);
        }
    }
    
    /* is_expr_body access */
    uint8_t is_expr_body() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->is_expr_body : 0;
    }
    
    void set_is_expr_body(uint8_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->is_expr_body = val;
    }
    
    /* lazy_arg_names_handle access */
    GCHandle lazy_arg_names_handle() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->lazy_arg_names_handle : GC_HANDLE_NULL;
    }
    
    void set_lazy_arg_names_handle(GCHandle val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) {
            p->lazy_arg_names_handle = val;
            gc_write_barrier_for_heap_slot(&p->lazy_arg_names_handle, val);
        }
    }
    
    /* lazy_arg_count access */
    uint16_t lazy_arg_count() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->lazy_arg_count : 0;
    }
    
    void set_lazy_arg_count(uint16_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->lazy_arg_count = val;
    }
    
    /* =====================================================================
     * Resume parser state accessors (Phase 1 implementation)
     * ===================================================================== */
    
    /* has_resume_parse_state access */
    uint8_t has_resume_parse_state() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? p->has_resume_parse_state : 0;
    }
    
    void set_has_resume_parse_state(uint8_t val) {
        JSFunctionBytecode* p = get_ptr();
        if (p) p->has_resume_parse_state = val;
    }
    
    /* resume_parse_state access - returns pointer to the state struct */
    const JSLazyParseState* resume_parse_state() const {
        JSFunctionBytecode* p = get_ptr();
        return p ? &p->resume_parse_state : nullptr;
    }
    
    JSLazyParseState* resume_parse_state() {
        JSFunctionBytecode* p = get_ptr();
        return p ? &p->resume_parse_state : nullptr;
    }
};


/* ============================================================================
 * JSArrayBufferHandle - Handle wrapper for JSArrayBuffer
 * ============================================================================ */
class JSArrayBufferHandle {
private:
    GCHandle handle_;
    
    JSArrayBuffer* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSArrayBuffer*)gc_deref(handle_);
    }

public:
    JSArrayBufferHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSArrayBufferHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    int byte_length() const {
        JSArrayBuffer* p = get_ptr();
        return p ? p->byte_length : 0;
    }
    
    void set_byte_length(int len) {
        JSArrayBuffer* p = get_ptr();
        if (p) p->byte_length = len;
    }
    
    int max_byte_length() const {
        JSArrayBuffer* p = get_ptr();
        return p ? p->max_byte_length : -1;
    }
    
    void set_max_byte_length(int len) {
        JSArrayBuffer* p = get_ptr();
        if (p) p->max_byte_length = len;
    }
    
    GCHandle data_handle() const {
        JSArrayBuffer* p = get_ptr();
        return p ? p->data_handle : GC_HANDLE_NULL;
    }
    
    void set_data_handle(GCHandle h) {
        JSArrayBuffer* p = get_ptr();
        if (p) {
            p->data_handle = h;
            gc_write_barrier_for_heap_slot(&p->data_handle, h);
        }
    }
    
    bool detached() const {
        JSArrayBuffer* p = get_ptr();
        return p ? p->detached : true;
    }
    
    void set_detached(bool d) {
        JSArrayBuffer* p = get_ptr();
        if (p) p->detached = d;
    }
    
    bool shared() const {
        JSArrayBuffer* p = get_ptr();
        return p ? p->shared : false;
    }
    
    bool is_resizable() const {
        JSArrayBuffer* p = get_ptr();
        return p ? p->max_byte_length >= 0 : false;
    }
    
    uint8_t* data() const {
        JSArrayBuffer* p = get_ptr();
        return p ? p->data : nullptr;
    }
    
    void set_data(uint8_t* d) {
        JSArrayBuffer* p = get_ptr();
        if (p) p->data = d;
    }
    
    /* Access typed_arrays list */
    GCHandleList& typed_arrays() const {
        JSArrayBuffer* p = get_ptr();
        static GCHandleList empty_list;
        return p ? p->typed_arrays : empty_list;
    }
    
    /* Get free_func */
    JSFreeArrayBufferDataFunc* free_func() const {
        JSArrayBuffer* p = get_ptr();
        return p ? p->free_func : nullptr;
    }
    
    /* Set free_func */
    void set_free_func(JSFreeArrayBufferDataFunc* func) {
        JSArrayBuffer* p = get_ptr();
        if (p) p->free_func = func;
    }
    
    /* Get opaque pointer */
    void* opaque() const {
        JSArrayBuffer* p = get_ptr();
        return p ? p->opaque : nullptr;
    }
    
    /* Set opaque pointer */
    void set_opaque(void* op) {
        JSArrayBuffer* p = get_ptr();
        if (p) p->opaque = op;
    }
    
    /* DEPRECATED: Direct pointer access - will be removed */
    JSArrayBuffer* ptr() const { return get_ptr(); }
};

/* ============================================================================
 * JSTypedArrayHandle - Handle wrapper for JSTypedArray
 * ============================================================================ */
class JSTypedArrayHandle {
private:
    GCHandle handle_;
    
    JSTypedArray* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSTypedArray*)gc_deref(handle_);
    }

public:
    JSTypedArrayHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSTypedArrayHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    GCHandle obj_handle() const {
        JSTypedArray* p = get_ptr();
        return p ? p->obj_handle : GC_HANDLE_NULL;
    }
    
    void set_obj_handle(GCHandle h) {
        JSTypedArray* p = get_ptr();
        if (p) {
            p->obj_handle = h;
            gc_write_barrier_for_heap_slot(&p->obj_handle, h);
        }
    }
    
    GCHandle buffer_handle() const {
        JSTypedArray* p = get_ptr();
        return p ? p->buffer_handle : GC_HANDLE_NULL;
    }
    
    void set_buffer_handle(GCHandle h) {
        JSTypedArray* p = get_ptr();
        if (p) {
            p->buffer_handle = h;
            gc_write_barrier_for_heap_slot(&p->buffer_handle, h);
        }
    }
    
    uint32_t offset() const {
        JSTypedArray* p = get_ptr();
        return p ? p->offset : 0;
    }
    
    void set_offset(uint32_t off) {
        JSTypedArray* p = get_ptr();
        if (p) p->offset = off;
    }
    
    uint32_t length() const {
        JSTypedArray* p = get_ptr();
        return p ? p->length : 0;
    }
    
    void set_length(uint32_t len) {
        JSTypedArray* p = get_ptr();
        if (p) p->length = len;
    }
    
    BOOL track_rab() const {
        JSTypedArray* p = get_ptr();
        return p ? p->track_rab : FALSE;
    }
    
    void set_track_rab(BOOL track) {
        JSTypedArray* p = get_ptr();
        if (p) p->track_rab = track;
    }
};

/* ============================================================================
 * JSMapStateHandle - Handle wrapper for JSMapState
 * ============================================================================ */
class JSMapStateHandle {
private:
    GCHandle handle_;
    
    JSMapState* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSMapState*)gc_deref(handle_);
    }

public:
    JSMapStateHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSMapStateHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    bool is_weak() const {
        JSMapState* p = get_ptr();
        return p ? p->is_weak : false;
    }
    
    void set_is_weak(bool weak) {
        JSMapState* p = get_ptr();
        if (p) p->is_weak = weak;
    }
    
    uint32_t record_count() const {
        JSMapState* p = get_ptr();
        return p ? p->record_count : 0;
    }
    
    void set_record_count(uint32_t count) {
        JSMapState* p = get_ptr();
        if (p) p->record_count = count;
    }
    
    GCHandle hash_table_handle() const {
        JSMapState* p = get_ptr();
        return p ? p->hash_table_handle : GC_HANDLE_NULL;
    }
    
    void set_hash_table_handle(GCHandle h) {
        JSMapState* p = get_ptr();
        if (p) {
            p->hash_table_handle = h;
            gc_write_barrier_for_heap_slot(&p->hash_table_handle, h);
        }
    }
    
    int hash_bits() const {
        JSMapState* p = get_ptr();
        return p ? p->hash_bits : 0;
    }
    
    void set_hash_bits(int bits) {
        JSMapState* p = get_ptr();
        if (p) p->hash_bits = bits;
    }
    
    uint32_t hash_size() const {
        JSMapState* p = get_ptr();
        return p ? p->hash_size : 0;
    }
    
    void set_hash_size(uint32_t size) {
        JSMapState* p = get_ptr();
        if (p) p->hash_size = size;
    }
    
    uint32_t record_count_threshold() const {
        JSMapState* p = get_ptr();
        return p ? p->record_count_threshold : 0;
    }
    
    void set_record_count_threshold(uint32_t threshold) {
        JSMapState* p = get_ptr();
        if (p) p->record_count_threshold = threshold;
    }
    
    /* Get the handle of the first record in the list (GC-safe) */
    GCHandle records_first() const {
        JSMapState* p = get_ptr();
        return p ? p->records.next : GC_HANDLE_NULL;
    }
    
    /* Check if records list is empty (GC-safe) */
    bool records_empty() const {
        JSMapState* p = get_ptr();
        return !p || p->records.next == GC_HANDLE_NULL;
    }
    
    /* Initialize records list - should be called after allocation */
    void init_records() const {
        JSMapState* p = get_ptr();
        if (p) gc_list_init(&p->records);
    }
    
    /* =========================================================================
     * Map/Set Operation Helpers - Use these instead of get_raw_ptr()
     * ========================================================================= */
    
    /* Increment record count */
    void inc_record_count() const {
        JSMapState* p = get_ptr();
        if (p) p->record_count++;
    }
    
    /* Decrement record count */
    void dec_record_count() const {
        JSMapState* p = get_ptr();
        if (p) p->record_count--;
    }
    
    /* Get the records list head for iteration (GC-safe) */
    GCHandle records_head() const {
        JSMapState* p = get_ptr();
        if (!p) return GC_HANDLE_NULL;
        return p->records.next;
    }
    
    /* Get hash table entry at index (GC-safe) */
    GCHandle hash_table_entry(uint32_t idx) const {
        JSMapState* p = get_ptr();
        if (!p || idx >= (uint32_t)(1U << p->hash_bits)) return GC_HANDLE_NULL;
        GCHandle* table = (GCHandle*)gc_deref(p->hash_table_handle);
        return table ? table[idx] : GC_HANDLE_NULL;
    }
    
    /* Set hash table entry at index */
    void set_hash_table_entry(uint32_t idx, GCHandle h) const {
        JSMapState* p = get_ptr();
        if (!p || idx >= (uint32_t)(1U << p->hash_bits)) return;
        GCHandle* table = (GCHandle*)gc_deref(p->hash_table_handle);
        if (table) {
            table[idx] = h;
            gc_write_barrier_for_heap_slot(&table[idx], h);
        }
    }
    
    /* Check if hash table needs resize */
    bool needs_resize() const {
        JSMapState* p = get_ptr();
        return p && p->record_count >= p->record_count_threshold;
    }
    
    /* =========================================================================
     * INTERNAL USE ONLY - NOT FOR GENERAL USE
     * These methods provide raw pointer access for internal refactoring.
     * They will be removed once all callers are migrated to handle-based APIs.
     * ========================================================================= */
    
    /* DEPRECATED: Use handle-based APIs instead. 
     * Returns raw pointer valid only immediately. Do not store. */
    JSMapState* __unsafe_internal_ptr() const {
        return get_ptr();
    }
    
    // get_raw_ptr() REMOVED in Phase 2 - use handle-based accessors instead
};

/* ============================================================================
 * JSPromiseDataHandle - Handle wrapper for JSPromiseData
 * ============================================================================ */
class JSPromiseDataHandle {
private:
    GCHandle handle_;
    
    JSPromiseData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSPromiseData*)gc_deref(handle_);
    }

public:
    JSPromiseDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSPromiseDataHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    int promise_state() const {
        JSPromiseData* p = get_ptr();
        return p ? p->promise_state : 0;
    }
    
    void set_promise_state(JSPromiseStateEnum state) {
        JSPromiseData* p = get_ptr();
        if (p) p->promise_state = state;
    }
    
    BOOL is_handled() const {
        JSPromiseData* p = get_ptr();
        return p ? p->is_handled : FALSE;
    }
    
    void set_is_handled(BOOL h) {
        JSPromiseData* p = get_ptr();
        if (p) p->is_handled = h;
    }
    
    GCValue promise_result() const {
        JSPromiseData* p = get_ptr();
        return p ? p->promise_result : GC_UNDEFINED;
    }
    
    void set_promise_result(GCValue v) {
        JSPromiseData* p = get_ptr();
        if (p) {
            p->promise_result = v;
            gc_write_barrier_for_heap_slot(&p->promise_result, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    GCValue* promise_result_ptr() const {
        JSPromiseData* p = get_ptr();
        return p ? &p->promise_result : nullptr;
    }
    
    /* Get the first reaction handle for the specified reaction type (0=fulfill, 1=reject) */
    GCHandle promise_reactions_first(int i) const {
        JSPromiseData* p = get_ptr();
        if (!p || i < 0 || i >= 2) return GC_HANDLE_NULL;
        return p->promise_reactions[i].next;
    }
    
    /* Initialize promise_reactions lists */
    void init_promise_reactions() const {
        JSPromiseData* p = get_ptr();
        if (p) {
            gc_list_init(&p->promise_reactions[0]);
            gc_list_init(&p->promise_reactions[1]);
        }
    }
};

/* ============================================================================
 * JSProxyDataHandle - Handle wrapper for JSProxyData
 * ============================================================================ */
class JSProxyDataHandle {
private:
    GCHandle handle_;
    
    JSProxyData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSProxyData*)gc_deref(handle_);
    }

public:
    JSProxyDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSProxyDataHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    GCValue target() const {
        JSProxyData* p = get_ptr();
        return p ? p->target : GC_UNDEFINED;
    }
    
    void set_target(GCValue t) {
        JSProxyData* p = get_ptr();
        if (p) {
            p->target = t;
            gc_write_barrier_for_heap_slot(&p->target, GC_VALUE_GET_HANDLE(t));
        }
    }
    
    GCValue handler() const {
        JSProxyData* p = get_ptr();
        return p ? p->handler : GC_UNDEFINED;
    }
    
    void set_handler(GCValue h) {
        JSProxyData* p = get_ptr();
        if (p) {
            p->handler = h;
            gc_write_barrier_for_heap_slot(&p->handler, GC_VALUE_GET_HANDLE(h));
        }
    }
    
    uint8_t is_func() const {
        JSProxyData* p = get_ptr();
        return p ? p->is_func : 0;
    }
    
    void set_is_func(uint8_t f) {
        JSProxyData* p = get_ptr();
        if (p) p->is_func = f;
    }
    
    uint8_t is_revoked() const {
        JSProxyData* p = get_ptr();
        return p ? p->is_revoked : 0;
    }
    
    void set_is_revoked(uint8_t r) {
        JSProxyData* p = get_ptr();
        if (p) p->is_revoked = r;
    }
};

/* ============================================================================
 * JSBoundFunctionHandle - Handle wrapper for JSBoundFunction
 * ============================================================================ */
class JSBoundFunctionHandle {
private:
    GCHandle handle_;
    
    JSBoundFunction* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSBoundFunction*)gc_deref(handle_);
    }

public:
    JSBoundFunctionHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSBoundFunctionHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    GCValue func_obj() const {
        JSBoundFunction* p = get_ptr();
        return p ? p->func_obj : GC_UNDEFINED;
    }
    
    void set_func_obj(GCValue v) {
        JSBoundFunction* p = get_ptr();
        if (p) {
            p->func_obj = v;
            gc_write_barrier_for_heap_slot(&p->func_obj, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    GCValue this_val() const {
        JSBoundFunction* p = get_ptr();
        return p ? p->this_val : GC_UNDEFINED;
    }
    
    void set_this_val(GCValue v) {
        JSBoundFunction* p = get_ptr();
        if (p) {
            p->this_val = v;
            gc_write_barrier_for_heap_slot(&p->this_val, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    int argc() const {
        JSBoundFunction* p = get_ptr();
        return p ? p->argc : 0;
    }
    
    GCValue argv(int idx) const {
        JSBoundFunction* p = get_ptr();
        if (!p || idx < 0 || idx >= p->argc) return GC_UNDEFINED;
        return p->argv[idx];
    }
    
    void set_argv(int idx, GCValue v) {
        JSBoundFunction* p = get_ptr();
        if (p && idx >= 0 && idx < p->argc) {
            p->argv[idx] = v;
            gc_write_barrier_for_heap_slot(&p->argv[idx], GC_VALUE_GET_HANDLE(v));
        }
    }
    
    /* Copy argv values to destination buffer.
     * Returns number of values copied (may be less than max_count if argc is smaller).
     * This is the SAFE alternative to argv_ptr() - never returns a raw pointer.
     */
    int copy_argv_to(GCValue* dest, int max_count) const {
        JSBoundFunction* p = get_ptr();
        if (!p || !dest || max_count <= 0) return 0;
        int count = (p->argc < max_count) ? p->argc : max_count;
        for (int i = 0; i < count; i++) {
            dest[i] = p->argv[i];
        }
        return count;
    }
    
    /* Get raw pointer - USE WITH CAUTION */
    JSBoundFunction* ptr() const {
        return get_ptr();
    }
};

/* ============================================================================
 * JSForInIteratorHandle - Handle wrapper for JSForInIterator
 * ============================================================================ */
class JSForInIteratorHandle {
private:
    GCHandle handle_;
    
    JSForInIterator* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSForInIterator*)gc_deref(handle_);
    }

public:
    JSForInIteratorHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSForInIteratorHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    GCValue obj() const {
        JSForInIterator* p = get_ptr();
        return p ? p->obj : GC_UNDEFINED;
    }
    
    void set_obj(GCValue v) {
        JSForInIterator* p = get_ptr();
        if (p) {
            p->obj = v;
            gc_write_barrier_for_heap_slot(&p->obj, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    uint32_t idx() const {
        JSForInIterator* p = get_ptr();
        return p ? p->idx : 0;
    }
    
    void set_idx(uint32_t i) {
        JSForInIterator* p = get_ptr();
        if (p) p->idx = i;
    }
    
    uint32_t atom_count() const {
        JSForInIterator* p = get_ptr();
        return p ? p->atom_count : 0;
    }
    
    void set_atom_count(uint32_t c) {
        JSForInIterator* p = get_ptr();
        if (p) p->atom_count = c;
    }
    
    uint8_t in_prototype_chain() const {
        JSForInIterator* p = get_ptr();
        return p ? p->in_prototype_chain : 0;
    }
    
    void set_in_prototype_chain(uint8_t v) {
        JSForInIterator* p = get_ptr();
        if (p) p->in_prototype_chain = v;
    }
    
    uint8_t is_array() const {
        JSForInIterator* p = get_ptr();
        return p ? p->is_array : 0;
    }
    
    void set_is_array(uint8_t v) {
        JSForInIterator* p = get_ptr();
        if (p) p->is_array = v;
    }
    
    GCHandle tab_atom_handle() const {
        JSForInIterator* p = get_ptr();
        return p ? p->tab_atom_handle : GC_HANDLE_NULL;
    }
    
    void set_tab_atom_handle(GCHandle h) {
        JSForInIterator* p = get_ptr();
        if (p) {
            p->tab_atom_handle = h;
            gc_write_barrier_for_heap_slot(&p->tab_atom_handle, h);
        }
    }
};

/* ============================================================================
 * JSMapRecordHandle - Handle wrapper for JSMapRecord
 * ============================================================================ */
class JSMapRecordHandle {
private:
    GCHandle handle_;
    
    JSMapRecord* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSMapRecord*)gc_deref(handle_);
    }

public:
    JSMapRecordHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSMapRecordHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    JSMapRecord* ptr() const { return get_ptr(); }
    
    int ref_count() const {
        JSMapRecord* p = get_ptr();
        return p ? p->ref_count : 0;
    }
    
    void set_ref_count(int rc) {
        JSMapRecord* p = get_ptr();
        if (p) p->ref_count = rc;
    }
    
    BOOL empty() const {
        JSMapRecord* p = get_ptr();
        return p ? p->empty : TRUE;
    }
    
    void set_empty(BOOL e) {
        JSMapRecord* p = get_ptr();
        if (p) p->empty = e;
    }
    
    GCHandle self_handle() const {
        JSMapRecord* p = get_ptr();
        return p ? p->self_handle : GC_HANDLE_NULL;
    }
    
    void set_self_handle(GCHandle h) {
        JSMapRecord* p = get_ptr();
        if (p) {
            p->self_handle = h;
            gc_write_barrier_for_heap_slot(&p->self_handle, h);
        }
    }
    
    GCHandle hash_next_handle() const {
        JSMapRecord* p = get_ptr();
        return p ? p->hash_next_handle : GC_HANDLE_NULL;
    }
    
    void set_hash_next_handle(GCHandle h) {
        JSMapRecord* p = get_ptr();
        if (p) {
            p->hash_next_handle = h;
            gc_write_barrier_for_heap_slot(&p->hash_next_handle, h);
        }
    }
    
    /* Get the next record in the linked list (for gc_list iteration) */
    GCHandle link_next_handle() const {
        JSMapRecord* p = get_ptr();
        if (!p) return GC_HANDLE_NULL;
        return p->link.next;
    }
    
    /* Get the previous record in the linked list */
    GCHandle link_prev_handle() const {
        JSMapRecord* p = get_ptr();
        if (!p) return GC_HANDLE_NULL;
        return p->link.prev;
    }
    
    GCValue key() const {
        JSMapRecord* p = get_ptr();
        return p ? p->key : GC_UNDEFINED;
    }
    
    void set_key(GCValue v) {
        JSMapRecord* p = get_ptr();
        if (p) {
            p->key = v;
            gc_write_barrier_for_heap_slot(&p->key, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    GCValue value() const {
        JSMapRecord* p = get_ptr();
        return p ? p->value : GC_UNDEFINED;
    }
    
    void set_value(GCValue v) {
        JSMapRecord* p = get_ptr();
        if (p) {
            p->value = v;
            gc_write_barrier_for_heap_slot(&p->value, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    /* Get self handle for gc_list operations */
    GCHandle link_handle() const {
        return handle_;
    }
    
    /* =========================================================================
     * Reference Counting Helpers
     * ========================================================================= */
    
    /* Increment reference count */
    void inc_ref() const {
        JSMapRecord* p = get_ptr();
        if (p) p->ref_count++;
    }
    
    /* Decrement reference count */
    void dec_ref() const {
        JSMapRecord* p = get_ptr();
        if (p) p->ref_count--;
    }
    
    /* Mark record as empty */
    void mark_empty() const {
        JSMapRecord* p = get_ptr();
        if (p) p->empty = TRUE;
    }
    
    /* =========================================================================
     * INTERNAL USE ONLY - NOT FOR GENERAL USE
     * These methods provide raw pointer access for internal refactoring.
     * They will be removed once all callers are migrated to handle-based APIs.
     * ========================================================================= */
    
    /* DEPRECATED: Use handle-based APIs instead.
     * Returns raw pointer valid only immediately. Do not store. */
    JSMapRecord* __unsafe_internal_ptr() const {
        return get_ptr();
    }
    
    // get_raw_ptr() REMOVED in Phase 2 - use handle-based accessors instead
};

/* ============================================================================
 * JSMapIteratorDataHandle - Handle wrapper for JSMapIteratorData
 * ============================================================================ */
class JSMapIteratorDataHandle {
private:
    GCHandle handle_;
    
    JSMapIteratorData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSMapIteratorData*)gc_deref(handle_);
    }

public:
    JSMapIteratorDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSMapIteratorDataHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    GCValue obj() const {
        JSMapIteratorData* p = get_ptr();
        return p ? p->obj : GC_UNDEFINED;
    }
    
    void set_obj(GCValue v) {
        JSMapIteratorData* p = get_ptr();
        if (p) {
            p->obj = v;
            gc_write_barrier_for_heap_slot(&p->obj, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    int kind() const {
        JSMapIteratorData* p = get_ptr();
        return p ? p->kind : 0;
    }
    
    void set_kind(int k) {
        JSMapIteratorData* p = get_ptr();
        if (p) p->kind = (JSIteratorKindEnum)k;
    }
    
    GCHandle cur_record_handle() const {
        JSMapIteratorData* p = get_ptr();
        return p ? p->cur_record_handle : GC_HANDLE_NULL;
    }
    
    void set_cur_record_handle(GCHandle h) {
        JSMapIteratorData* p = get_ptr();
        if (p) {
            p->cur_record_handle = h;
            gc_write_barrier_for_heap_slot(&p->cur_record_handle, h);
        }
    }
};

/* ============================================================================
 * JSArrayIteratorDataHandle - Handle wrapper for JSArrayIteratorData
 * ============================================================================ */
class JSArrayIteratorDataHandle {
private:
    GCHandle handle_;
    
    JSArrayIteratorData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSArrayIteratorData*)gc_deref(handle_);
    }

public:
    JSArrayIteratorDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSArrayIteratorDataHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    GCValue obj() const {
        JSArrayIteratorData* p = get_ptr();
        return p ? p->obj : GC_UNDEFINED;
    }
    
    void set_obj(GCValue v) {
        JSArrayIteratorData* p = get_ptr();
        if (p) {
            p->obj = v;
            gc_write_barrier_for_heap_slot(&p->obj, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    int kind() const {
        JSArrayIteratorData* p = get_ptr();
        return p ? p->kind : 0;
    }
    
    void set_kind(int k) {
        JSArrayIteratorData* p = get_ptr();
        if (p) p->kind = (JSIteratorKindEnum)k;
    }
    
    uint32_t idx() const {
        JSArrayIteratorData* p = get_ptr();
        return p ? p->idx : 0;
    }
    
    void set_idx(uint32_t i) {
        JSArrayIteratorData* p = get_ptr();
        if (p) p->idx = i;
    }
};

/* ============================================================================
 * JSIteratorHelperDataHandle - Handle wrapper for JSIteratorHelperData
 * ============================================================================ */
class JSIteratorHelperDataHandle {
private:
    GCHandle handle_;
    
    JSIteratorHelperData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSIteratorHelperData*)gc_deref(handle_);
    }

public:
    JSIteratorHelperDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSIteratorHelperDataHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    GCValue obj() const {
        JSIteratorHelperData* p = get_ptr();
        return p ? p->obj : GC_UNDEFINED;
    }
    
    void set_obj(GCValue v) {
        JSIteratorHelperData* p = get_ptr();
        if (p) {
            p->obj = v;
            gc_write_barrier_for_heap_slot(&p->obj, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    GCValue next() const {
        JSIteratorHelperData* p = get_ptr();
        return p ? p->next : GC_UNDEFINED;
    }
    
    void set_next(GCValue v) {
        JSIteratorHelperData* p = get_ptr();
        if (p) {
            p->next = v;
            gc_write_barrier_for_heap_slot(&p->next, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    GCValue func() const {
        JSIteratorHelperData* p = get_ptr();
        return p ? p->func : GC_UNDEFINED;
    }
    
    void set_func(GCValue v) {
        JSIteratorHelperData* p = get_ptr();
        if (p) {
            p->func = v;
            gc_write_barrier_for_heap_slot(&p->func, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    GCValue inner() const {
        JSIteratorHelperData* p = get_ptr();
        return p ? p->inner : GC_UNDEFINED;
    }
    
    void set_inner(GCValue v) {
        JSIteratorHelperData* p = get_ptr();
        if (p) {
            p->inner = v;
            gc_write_barrier_for_heap_slot(&p->inner, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    int64_t count() const {
        JSIteratorHelperData* p = get_ptr();
        return p ? p->count : 0;
    }
    
    void set_count(int64_t c) {
        JSIteratorHelperData* p = get_ptr();
        if (p) p->count = c;
    }
    
    int kind() const {
        JSIteratorHelperData* p = get_ptr();
        return p ? p->kind : 0;
    }
    
    void set_kind(int k) {
        JSIteratorHelperData* p = get_ptr();
        if (p) p->kind = (JSIteratorHelperKindEnum)k;
    }
    
    uint8_t executing() const {
        JSIteratorHelperData* p = get_ptr();
        return p ? p->executing : 0;
    }
    
    void set_executing(uint8_t e) {
        JSIteratorHelperData* p = get_ptr();
        if (p) p->executing = e;
    }
    
    uint8_t done() const {
        JSIteratorHelperData* p = get_ptr();
        return p ? p->done : 0;
    }
    
    void set_done(uint8_t d) {
        JSIteratorHelperData* p = get_ptr();
        if (p) p->done = d;
    }
};

/* ============================================================================
 * JSIteratorConcatDataHandle - Handle wrapper for JSIteratorConcatData
 * ============================================================================ */
class JSIteratorConcatDataHandle {
private:
    GCHandle handle_;
    
    JSIteratorConcatData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSIteratorConcatData*)gc_deref(handle_);
    }

public:
    JSIteratorConcatDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSIteratorConcatDataHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    int index() const {
        JSIteratorConcatData* p = get_ptr();
        return p ? p->index : 0;
    }
    
    void set_index(int i) {
        JSIteratorConcatData* p = get_ptr();
        if (p) p->index = i;
    }
    
    int count() const {
        JSIteratorConcatData* p = get_ptr();
        return p ? p->count : 0;
    }
    
    void set_count(int c) {
        JSIteratorConcatData* p = get_ptr();
        if (p) p->count = c;
    }
    
    BOOL running() const {
        JSIteratorConcatData* p = get_ptr();
        return p ? p->running : FALSE;
    }
    
    void set_running(BOOL r) {
        JSIteratorConcatData* p = get_ptr();
        if (p) p->running = r;
    }
    
    GCValue iter() const {
        JSIteratorConcatData* p = get_ptr();
        return p ? p->iter : GC_UNDEFINED;
    }
    
    void set_iter(GCValue v) {
        JSIteratorConcatData* p = get_ptr();
        if (p) {
            p->iter = v;
            gc_write_barrier_for_heap_slot(&p->iter, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    GCValue next() const {
        JSIteratorConcatData* p = get_ptr();
        return p ? p->next : GC_UNDEFINED;
    }
    
    void set_next(GCValue v) {
        JSIteratorConcatData* p = get_ptr();
        if (p) {
            p->next = v;
            gc_write_barrier_for_heap_slot(&p->next, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    /* Access to values[] array - returns pointer to first element */
    GCValue* values() const {
        JSIteratorConcatData* p = get_ptr();
        return p ? p->values : nullptr;
    }
};

/* ============================================================================
 * JSIteratorWrapDataHandle - Handle wrapper for JSIteratorWrapData
 * ============================================================================ */
class JSIteratorWrapDataHandle {
private:
    GCHandle handle_;
    
    JSIteratorWrapData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSIteratorWrapData*)gc_deref(handle_);
    }

public:
    JSIteratorWrapDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSIteratorWrapDataHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    GCValue wrapped_iter() const {
        JSIteratorWrapData* p = get_ptr();
        return p ? p->wrapped_iter : GC_UNDEFINED;
    }
    
    void set_wrapped_iter(GCValue v) {
        JSIteratorWrapData* p = get_ptr();
        if (p) {
            p->wrapped_iter = v;
            gc_write_barrier_for_heap_slot(&p->wrapped_iter, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    GCValue wrapped_next() const {
        JSIteratorWrapData* p = get_ptr();
        return p ? p->wrapped_next : GC_UNDEFINED;
    }
    
    void set_wrapped_next(GCValue v) {
        JSIteratorWrapData* p = get_ptr();
        if (p) {
            p->wrapped_next = v;
            gc_write_barrier_for_heap_slot(&p->wrapped_next, GC_VALUE_GET_HANDLE(v));
        }
    }
};

/* ============================================================================
 * JSRegExpStringIteratorDataHandle - Handle wrapper for JSRegExpStringIteratorData
 * ============================================================================ */
class JSRegExpStringIteratorDataHandle {
private:
    GCHandle handle_;
    
    JSRegExpStringIteratorData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSRegExpStringIteratorData*)gc_deref(handle_);
    }

public:
    JSRegExpStringIteratorDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSRegExpStringIteratorDataHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    GCValue iterating_regexp() const {
        JSRegExpStringIteratorData* p = get_ptr();
        return p ? p->iterating_regexp : GC_UNDEFINED;
    }
    
    void set_iterating_regexp(GCValue v) {
        JSRegExpStringIteratorData* p = get_ptr();
        if (p) {
            p->iterating_regexp = v;
            gc_write_barrier_for_heap_slot(&p->iterating_regexp, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    GCValue iterated_string() const {
        JSRegExpStringIteratorData* p = get_ptr();
        return p ? p->iterated_string : GC_UNDEFINED;
    }
    
    void set_iterated_string(GCValue v) {
        JSRegExpStringIteratorData* p = get_ptr();
        if (p) {
            p->iterated_string = v;
            gc_write_barrier_for_heap_slot(&p->iterated_string, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    BOOL global() const {
        JSRegExpStringIteratorData* p = get_ptr();
        return p ? p->global : FALSE;
    }
    
    void set_global(BOOL g) {
        JSRegExpStringIteratorData* p = get_ptr();
        if (p) p->global = g;
    }
    
    BOOL unicode() const {
        JSRegExpStringIteratorData* p = get_ptr();
        return p ? p->unicode : FALSE;
    }
    
    void set_unicode(BOOL u) {
        JSRegExpStringIteratorData* p = get_ptr();
        if (p) p->unicode = u;
    }
    
    BOOL done() const {
        JSRegExpStringIteratorData* p = get_ptr();
        return p ? p->done : FALSE;
    }
    
    void set_done(BOOL d) {
        JSRegExpStringIteratorData* p = get_ptr();
        if (p) p->done = d;
    }
};

/* Forward declaration for JSVarDefHandle - defined below */
class JSVarDefHandle;

/* ============================================================================
 * JSVarDefArrayHandle - Handle wrapper for JSVarDef arrays
 * ============================================================================
 * 
 * This class provides safe access to arrays of JSVarDef in GC-managed memory.
 * JSVarDef is used during bytecode generation to track variable definitions.
 * 
 * USAGE:
 *   JSVarDefArrayHandle vars(fd->vars_handle);
 *   JSVarDefHandle var = vars.at(idx);  // Get handle to specific element
 */

class JSVarDefArrayHandle {
private:
    GCHandle handle_;
    
    JSVarDef* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSVarDef*)GCPin<void>(handle_).ptr();
    }

public:
    JSVarDefArrayHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSVarDefArrayHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
    /* Element access - returns handle for safe field access */
    JSVarDefHandle at(int index) const;
};

/* ============================================================================
 * JSVarDefHandle - Handle wrapper for JSVarDef array elements
 * ============================================================================
 * 
 * This class provides safe access to JSVarDef elements in GC-managed arrays.
 * JSVarDef is used during bytecode generation to track variable definitions.
 * This wrapper allows safe indexed access and modification of vardef fields.
 * 
 * USAGE:
 *   JSVarDefHandle var = JSVarDefHandle(fd->vars_handle, idx);
 *   var.set_func_pool_idx(pool_idx);  // Safe field modification
 *   JSAtom name = var.var_name();     // Safe field access
 */

class JSVarDefHandle {
private:
    GCHandle array_handle_;
    int index_;
    
    /* Get fresh pointer to the specific element - called on every access */
    JSVarDef* get_ptr() const {
        if (array_handle_ == GC_HANDLE_NULL) return nullptr;
        JSVarDef* array = (JSVarDef*)gc_deref(array_handle_);
        return array ? &array[index_] : nullptr;
    }

public:
    /* Default constructor - null handle */
    JSVarDefHandle() : array_handle_(GC_HANDLE_NULL), index_(0) {}
    
    /* Construct from array handle and index */
    JSVarDefHandle(GCHandle array_handle, int index) 
        : array_handle_(array_handle), index_(index) {}
    
    /* Copy constructor */
    JSVarDefHandle(const JSVarDefHandle& other) 
        : array_handle_(other.array_handle_), index_(other.index_) {}
    
    /* Move constructor */
    JSVarDefHandle(JSVarDefHandle&& other) noexcept 
        : array_handle_(other.array_handle_), index_(other.index_) {
        other.array_handle_ = GC_HANDLE_NULL;
        other.index_ = 0;
    }
    
    /* Assignment operators */
    JSVarDefHandle& operator=(const JSVarDefHandle& other) {
        array_handle_ = other.array_handle_;
        index_ = other.index_;
        return *this;
    }
    
    JSVarDefHandle& operator=(JSVarDefHandle&& other) noexcept {
        array_handle_ = other.array_handle_;
        index_ = other.index_;
        other.array_handle_ = GC_HANDLE_NULL;
        other.index_ = 0;
        return *this;
    }
    
    /* Get the underlying array handle */
    GCHandle array_handle() const { return array_handle_; }
    
    /* Get the index */
    int index() const { return index_; }
    
    /* Set index for reuse */
    void set_index(int idx) { index_ = idx; }
    
    /* Check if handle is valid */
    bool valid() const { 
        return array_handle_ != GC_HANDLE_NULL && gc_handle_is_valid(array_handle_);
    }
    
    /* Explicit bool conversion */
    explicit operator bool() const { return valid(); }
    
    /* =====================================================================
     * Property accessors - each dereferences handle to get fresh pointer
     * ===================================================================== */
    
    /* var_name access */
    JSAtom var_name() const {
        JSVarDef* p = get_ptr();
        return p ? p->var_name : 0;
    }
    
    void set_var_name(JSAtom val) {
        JSVarDef* p = get_ptr();
        if (p) p->var_name = val;
    }
    
    /* scope_level access */
    int scope_level() const {
        JSVarDef* p = get_ptr();
        return p ? p->scope_level : 0;
    }
    
    void set_scope_level(int val) {
        JSVarDef* p = get_ptr();
        if (p) p->scope_level = val;
    }
    
    /* scope_next access */
    int scope_next() const {
        JSVarDef* p = get_ptr();
        return p ? p->scope_next : 0;
    }
    
    void set_scope_next(int val) {
        JSVarDef* p = get_ptr();
        if (p) p->scope_next = val;
    }
    
    /* is_const access */
    uint8_t is_const() const {
        JSVarDef* p = get_ptr();
        return p ? p->is_const : 0;
    }
    
    void set_is_const(uint8_t val) {
        JSVarDef* p = get_ptr();
        if (p) p->is_const = val;
    }
    
    /* is_lexical access */
    uint8_t is_lexical() const {
        JSVarDef* p = get_ptr();
        return p ? p->is_lexical : 0;
    }
    
    void set_is_lexical(uint8_t val) {
        JSVarDef* p = get_ptr();
        if (p) p->is_lexical = val;
    }
    
    /* is_captured access */
    uint8_t is_captured() const {
        JSVarDef* p = get_ptr();
        return p ? p->is_captured : 0;
    }
    
    void set_is_captured(uint8_t val) {
        JSVarDef* p = get_ptr();
        if (p) p->is_captured = val;
    }
    
    /* is_static_private access */
    uint8_t is_static_private() const {
        JSVarDef* p = get_ptr();
        return p ? p->is_static_private : 0;
    }
    
    void set_is_static_private(uint8_t val) {
        JSVarDef* p = get_ptr();
        if (p) p->is_static_private = val;
    }
    
    /* var_kind access */
    uint8_t var_kind() const {
        JSVarDef* p = get_ptr();
        return p ? p->var_kind : 0;
    }
    
    void set_var_kind(uint8_t val) {
        JSVarDef* p = get_ptr();
        if (p) p->var_kind = val;
    }
    
    /* var_ref_idx access */
    uint16_t var_ref_idx() const {
        JSVarDef* p = get_ptr();
        return p ? p->var_ref_idx : 0;
    }
    
    void set_var_ref_idx(uint16_t val) {
        JSVarDef* p = get_ptr();
        if (p) p->var_ref_idx = val;
    }
    
    /* func_pool_idx access */
    int func_pool_idx() const {
        JSVarDef* p = get_ptr();
        return p ? p->func_pool_idx : -1;
    }
    
    void set_func_pool_idx(int val) {
        JSVarDef* p = get_ptr();
        if (p) p->func_pool_idx = val;
    }
    
    /* =====================================================================
     * Conversion operators for interoperability
     * ===================================================================== */
    
    /* Comparison operators */
    bool operator==(const JSVarDefHandle& other) const {
        return array_handle_ == other.array_handle_ && index_ == other.index_;
    }
    
    bool operator!=(const JSVarDefHandle& other) const {
        return !(*this == other);
    }
};

/* Implementation of JSVarDefArrayHandle::at() - must be after JSVarDefHandle class */
inline JSVarDefHandle JSVarDefArrayHandle::at(int index) const {
    return JSVarDefHandle(handle_, index);
}

/* ============================================================================
 * JSAsyncFromSyncIteratorDataHandle - Handle wrapper for JSAsyncFromSyncIteratorData
 * ============================================================================ */
class JSAsyncFromSyncIteratorDataHandle {
private:
    GCHandle handle_;
    
    JSAsyncFromSyncIteratorData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSAsyncFromSyncIteratorData*)gc_deref(handle_);
    }

public:
    JSAsyncFromSyncIteratorDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSAsyncFromSyncIteratorDataHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    GCValue sync_iter() const {
        JSAsyncFromSyncIteratorData* p = get_ptr();
        return p ? p->sync_iter : GC_UNDEFINED;
    }
    
    void set_sync_iter(GCValue v) {
        JSAsyncFromSyncIteratorData* p = get_ptr();
        if (p) {
            p->sync_iter = v;
            gc_write_barrier_for_heap_slot(&p->sync_iter, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    GCValue next_method() const {
        JSAsyncFromSyncIteratorData* p = get_ptr();
        return p ? p->next_method : GC_UNDEFINED;
    }
    
    void set_next_method(GCValue v) {
        JSAsyncFromSyncIteratorData* p = get_ptr();
        if (p) {
            p->next_method = v;
            gc_write_barrier_for_heap_slot(&p->next_method, GC_VALUE_GET_HANDLE(v));
        }
    }
};

/* ============================================================================
 * JSPromiseFunctionDataHandle - Handle wrapper for JSPromiseFunctionData
 * ============================================================================ */
class JSPromiseFunctionDataHandle {
private:
    GCHandle handle_;
    
    JSPromiseFunctionData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSPromiseFunctionData*)gc_deref(handle_);
    }

public:
    JSPromiseFunctionDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSPromiseFunctionDataHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    GCValue promise() const {
        JSPromiseFunctionData* p = get_ptr();
        return p ? p->promise : GC_UNDEFINED;
    }
    
    void set_promise(GCValue v) {
        JSPromiseFunctionData* p = get_ptr();
        if (p) {
            p->promise = v;
            gc_write_barrier_for_heap_slot(&p->promise, GC_VALUE_GET_HANDLE(v));
        }
    }
    
    GCHandle presolved_handle() const {
        JSPromiseFunctionData* p = get_ptr();
        return p ? p->presolved_handle : GC_HANDLE_NULL;
    }
    
    void set_presolved_handle(GCHandle h) {
        JSPromiseFunctionData* p = get_ptr();
        if (p) {
            p->presolved_handle = h;
            gc_write_barrier_for_heap_slot(&p->presolved_handle, h);
        }
    }
};

/* ============================================================================
 * JSCFunctionDataRecordHandle - Handle wrapper for JSCFunctionDataRecord
 * ============================================================================ */
class JSCFunctionDataRecordHandle {
private:
    GCHandle handle_;
    
    JSCFunctionDataRecord* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSCFunctionDataRecord*)gc_deref(handle_);
    }

public:
    JSCFunctionDataRecordHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSCFunctionDataRecordHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    JSCFunctionData* func() const {
        JSCFunctionDataRecord* p = get_ptr();
        return p ? p->func : nullptr;
    }
    
    void set_func(JSCFunctionData* f) {
        JSCFunctionDataRecord* p = get_ptr();
        if (p) p->func = f;
    }
    
    uint8_t length() const {
        JSCFunctionDataRecord* p = get_ptr();
        return p ? p->length : 0;
    }
    
    void set_length(uint8_t l) {
        JSCFunctionDataRecord* p = get_ptr();
        if (p) p->length = l;
    }
    
    uint8_t data_len() const {
        JSCFunctionDataRecord* p = get_ptr();
        return p ? p->data_len : 0;
    }
    
    void set_data_len(uint8_t dl) {
        JSCFunctionDataRecord* p = get_ptr();
        if (p) p->data_len = dl;
    }
    
    uint16_t magic() const {
        JSCFunctionDataRecord* p = get_ptr();
        return p ? p->magic : 0;
    }
    
    void set_magic(uint16_t m) {
        JSCFunctionDataRecord* p = get_ptr();
        if (p) p->magic = m;
    }
    
    GCValue data(int idx) const {
        JSCFunctionDataRecord* p = get_ptr();
        if (!p || idx < 0 || idx >= p->data_len) return GC_UNDEFINED;
        return p->data[idx];
    }
    
    void set_data(int idx, GCValue v) {
        JSCFunctionDataRecord* p = get_ptr();
        if (p && idx >= 0 && idx < p->data_len) {
            p->data[idx] = v;
            gc_write_barrier_for_heap_slot(&p->data[idx], GC_VALUE_GET_HANDLE(v));
        }
    }
    
    /* Get raw pointer - INTERNAL USE ONLY.
     * This will be removed once all callers are migrated to handle-based APIs.
     * Do NOT store the returned pointer - only use immediately. */
    JSCFunctionDataRecord* ptr() const {
        return get_ptr();
    }
};

/* ============================================================================
 * JSGeneratorDataHandle - Handle wrapper for JSGeneratorData
 * ============================================================================ */
class JSGeneratorDataHandle {
private:
    GCHandle handle_;
    
    JSGeneratorData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSGeneratorData*)gc_deref(handle_);
    }

public:
    JSGeneratorDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSGeneratorDataHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    int state() const {
        JSGeneratorData* p = get_ptr();
        return p ? p->state : 0;
    }
    
    void set_state(int s) {
        JSGeneratorData* p = get_ptr();
        if (p) p->state = (JSGeneratorStateEnum)s;
    }
    
    GCHandle func_state_handle() const {
        JSGeneratorData* p = get_ptr();
        return p ? p->func_state_handle : GC_HANDLE_NULL;
    }
    
    void set_func_state_handle(GCHandle h) {
        JSGeneratorData* p = get_ptr();
        if (p) {
            p->func_state_handle = h;
            gc_write_barrier_for_heap_slot(&p->func_state_handle, h);
        }
    }
    
    /* Get raw pointer - USE WITH CAUTION */
    JSGeneratorData* ptr() const {
        return get_ptr();
    }
};

/* ============================================================================
 * JSAsyncGeneratorDataHandle - Handle wrapper for JSAsyncGeneratorData
 * ============================================================================ */
class JSAsyncGeneratorDataHandle {
private:
    GCHandle handle_;
    
    JSAsyncGeneratorData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSAsyncGeneratorData*)gc_deref(handle_);
    }

public:
    JSAsyncGeneratorDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSAsyncGeneratorDataHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    int state() const {
        JSAsyncGeneratorData* p = get_ptr();
        return p ? p->state : 0;
    }
    
    void set_state(int s) {
        JSAsyncGeneratorData* p = get_ptr();
        if (p) p->state = (JSAsyncGeneratorStateEnum)s;
    }
    
    GCHandle generator_handle() const {
        JSAsyncGeneratorData* p = get_ptr();
        return p ? p->generator_handle : GC_HANDLE_NULL;
    }
    
    void set_generator_handle(GCHandle h) {
        JSAsyncGeneratorData* p = get_ptr();
        if (p) {
            p->generator_handle = h;
            gc_write_barrier_for_heap_slot(&p->generator_handle, h);
        }
    }
    
    GCHandle func_state_handle() const {
        JSAsyncGeneratorData* p = get_ptr();
        return p ? p->func_state_handle : GC_HANDLE_NULL;
    }
    
    void set_func_state_handle(GCHandle h) {
        JSAsyncGeneratorData* p = get_ptr();
        if (p) {
            p->func_state_handle = h;
            gc_write_barrier_for_heap_slot(&p->func_state_handle, h);
        }
    }
    
    /* Get raw pointer - USE WITH CAUTION */
    JSAsyncGeneratorData* ptr() const {
        return get_ptr();
    }
    
    /* Get the first request in the queue */
    GCHandle queue_first() const {
        JSAsyncGeneratorData* p = get_ptr();
        return p ? p->queue.next : GC_HANDLE_NULL;
    }
    
    /* Check if queue is empty */
    bool queue_empty() const {
        JSAsyncGeneratorData* p = get_ptr();
        return !p || p->queue.next == GC_HANDLE_NULL;
    }
    
    /* Initialize the queue */
    void init_queue() const {
        JSAsyncGeneratorData* p = get_ptr();
        if (p) gc_list_init(&p->queue);
    }
};


/* ============================================================================
 * C++ JSFinalizationRegistryDataHandle Class - Safe GC Handle Wrapper
 * ============================================================================
 * 
 * This class wraps a GCHandle to provide safe access to JSFinalizationRegistryData
 * instances. The key feature is that it NEVER stores a raw pointer - instead,
 * it dereferences the handle on every property access to get a fresh pointer.
 * This ensures that the pointer is always valid even after GC compaction.
 * 
 * USAGE:
 *   JSFinalizationRegistryDataHandle frd(frd_handle);
 *   struct list_head* entries = frd.entries();  // Fresh dereference
 *   frd.set_realm_handle(ctx.handle());
 */

class JSFinalizationRegistryDataHandle {
private:
    GCHandle handle_;
    
    /* Get fresh pointer - called on every access */
    JSFinalizationRegistryData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSFinalizationRegistryData*)gc_deref(handle_);
    }

public:
    /* Default constructor - null handle */
    JSFinalizationRegistryDataHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit JSFinalizationRegistryDataHandle(GCHandle handle) : handle_(handle) {}
    
    /* Note: No raw pointer constructor - use GCHandle constructor to avoid GC unsafety */
    
    /* Copy constructor */
    JSFinalizationRegistryDataHandle(const JSFinalizationRegistryDataHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    JSFinalizationRegistryDataHandle(JSFinalizationRegistryDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    JSFinalizationRegistryDataHandle& operator=(const JSFinalizationRegistryDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    JSFinalizationRegistryDataHandle& operator=(JSFinalizationRegistryDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Assignment from nullptr */
    JSFinalizationRegistryDataHandle& operator=(decltype(nullptr)) {
        handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Get the underlying handle */
    GCHandle handle() const { return handle_; }
    
    /* Check if handle is valid */
    bool valid() const { 
        return handle_ != GC_HANDLE_NULL && gc_handle_is_valid(handle_);
    }
    
    /* Check if handle is null */
    bool is_null() const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    /* Explicit bool conversion */
    explicit operator bool() const { return valid(); }
    
    /* =====================================================================
     * Property accessors - each dereferences handle to get fresh pointer
     * ===================================================================== */
    
    /* Get the first entry in the list */
    GCHandle entries_first() const {
        JSFinalizationRegistryData* p = get_ptr();
        return p ? p->entries.next : GC_HANDLE_NULL;
    }
    
    /* Check if entries list is empty */
    bool entries_empty() const {
        JSFinalizationRegistryData* p = get_ptr();
        return !p || p->entries.next == GC_HANDLE_NULL;
    }
    
    /* Initialize entries list */
    void init_entries() const {
        JSFinalizationRegistryData* p = get_ptr();
        if (p) gc_list_init(&p->entries);
    }
    
    /* realm_handle access */
    GCHandle realm_handle() const {
        JSFinalizationRegistryData* p = get_ptr();
        return p ? p->realm_handle : GC_HANDLE_NULL;
    }
    
    void set_realm_handle(GCHandle val) {
        JSFinalizationRegistryData* p = get_ptr();
        if (p) {
            p->realm_handle = val;
            gc_write_barrier_for_heap_slot(&p->realm_handle, val);
        }
    }
    
    /* cb access */
    GCValue cb() const {
        JSFinalizationRegistryData* p = get_ptr();
        return p ? p->cb : GC_UNDEFINED;
    }
    
    void set_cb(const GCValue& val) {
        JSFinalizationRegistryData* p = get_ptr();
        if (p) {
            p->cb = val;
            gc_write_barrier_for_heap_slot(&p->cb, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* =====================================================================
     * Conversion operators for interoperability
     * ===================================================================== */
    
    /* Convert to GCHandle for handle-based APIs */
    operator GCHandle() const {
        return handle_;
    }
    
    /* Comparison operators */
    bool operator==(const JSFinalizationRegistryDataHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSFinalizationRegistryDataHandle& other) const {
        return handle_ != other.handle_;
    }
    
    bool operator==(GCHandle h) const {
        return handle_ == h;
    }
    
    bool operator!=(GCHandle h) const {
        return handle_ != h;
    }
    
    bool operator==(decltype(nullptr)) const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    bool operator!=(decltype(nullptr)) const {
        return handle_ != GC_HANDLE_NULL;
    }
    
    /* Get raw pointer - USE WITH CAUTION */
    JSFinalizationRegistryData* ptr() const {
        return get_ptr();
    }
};


/* ============================================================================
 * C++ JSWeakRefDataHandle Class - Safe GC Handle Wrapper
 * ============================================================================
 * 
 * This class wraps a GCHandle to provide safe access to JSWeakRefData instances.
 * The key feature is that it NEVER stores a raw pointer - instead,
 * it dereferences the handle on every property access to get a fresh pointer.
 * This ensures that the pointer is always valid even after GC compaction.
 * 
 * USAGE:
 *   JSWeakRefDataHandle wrd(wrd_handle);
 *   GCValue target = wrd.target();  // Fresh dereference
 *   wrd.set_target(new_target);     // Dereferences handle to set value
 */

class JSWeakRefDataHandle {
private:
    GCHandle handle_;
    
    /* Get fresh pointer - called on every access */
    JSWeakRefData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSWeakRefData*)gc_deref(handle_);
    }

public:
    /* Default constructor - null handle */
    JSWeakRefDataHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit JSWeakRefDataHandle(GCHandle handle) : handle_(handle) {}
    
    /* Note: No raw pointer constructor - use GCHandle constructor to avoid GC unsafety */
    
    /* Copy constructor */
    JSWeakRefDataHandle(const JSWeakRefDataHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    JSWeakRefDataHandle(JSWeakRefDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    JSWeakRefDataHandle& operator=(const JSWeakRefDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    JSWeakRefDataHandle& operator=(JSWeakRefDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Assignment from nullptr */
    JSWeakRefDataHandle& operator=(decltype(nullptr)) {
        handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Get the underlying handle */
    GCHandle handle() const { return handle_; }
    
    /* Check if handle is valid */
    bool valid() const { 
        return handle_ != GC_HANDLE_NULL && gc_handle_is_valid(handle_);
    }
    
    /* Check if handle is null */
    bool is_null() const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    /* Explicit bool conversion */
    explicit operator bool() const { return valid(); }
    
    /* =====================================================================
     * Property accessors - each dereferences handle to get fresh pointer
     * ===================================================================== */
    
    /* target access */
    GCValue target() const {
        JSWeakRefData* p = get_ptr();
        return p ? p->target : GC_UNDEFINED;
    }
    
    void set_target(const GCValue& val) {
        JSWeakRefData* p = get_ptr();
        if (p) {
            p->target = val;
            gc_write_barrier_for_heap_slot(&p->target, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* =====================================================================
     * Conversion operators for interoperability
     * ===================================================================== */
    
    /* Convert to GCHandle for handle-based APIs */
    operator GCHandle() const {
        return handle_;
    }
    
    /* Comparison operators */
    bool operator==(const JSWeakRefDataHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSWeakRefDataHandle& other) const {
        return handle_ != other.handle_;
    }
    
    bool operator==(GCHandle h) const {
        return handle_ == h;
    }
    
    bool operator!=(GCHandle h) const {
        return handle_ != h;
    }
    
    bool operator==(decltype(nullptr)) const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    bool operator!=(decltype(nullptr)) const {
        return handle_ != GC_HANDLE_NULL;
    }
    
    /* Get raw pointer - USE WITH CAUTION */
    JSWeakRefData* ptr() const {
        return get_ptr();
    }
};


/* ============================================================================
 * C++ JSPropertyEnumHandle Class - Safe GC Handle Wrapper for JSPropertyEnum arrays
 * ============================================================================
 * 
 * This class wraps a GCHandle to provide safe access to JSPropertyEnum arrays.
 * The key feature is that it NEVER stores a raw JSPropertyEnum pointer - instead,
 * it dereferences the handle on every access to get a fresh pointer.
 * This ensures that the pointer is always valid even after GC compaction.
 * 
 * USAGE:
 *   JSPropertyEnumHandle tab_atom(it.tab_atom_handle());
 *   JSAtom atom = tab_atom[i].atom;  // Fresh dereference with indexed access
 *   BOOL enumerable = tab_atom[i].is_enumerable;
 */

class JSPropertyEnumHandle {
private:
    GCHandle handle_;
    
    /* Get fresh pointer - called on every access */
    JSPropertyEnum* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSPropertyEnum*)gc_deref(handle_);
    }

public:
    /* Default constructor - null handle */
    JSPropertyEnumHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit JSPropertyEnumHandle(GCHandle handle) : handle_(handle) {}
    
    /* Copy constructor */
    JSPropertyEnumHandle(const JSPropertyEnumHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    JSPropertyEnumHandle(JSPropertyEnumHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    JSPropertyEnumHandle& operator=(const JSPropertyEnumHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    JSPropertyEnumHandle& operator=(JSPropertyEnumHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Assignment from nullptr */
    JSPropertyEnumHandle& operator=(decltype(nullptr)) {
        handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Get the underlying handle */
    GCHandle handle() const { return handle_; }
    
    /* Check if handle is valid */
    bool valid() const { 
        return handle_ != GC_HANDLE_NULL && gc_handle_is_valid(handle_);
    }
    
    /* Check if handle is null */
    bool is_null() const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    /* Explicit bool conversion */
    explicit operator bool() const { return valid(); }
    
    /* =====================================================================
     * Indexed element access - returns JSPropertyEnum by value
     * ===================================================================== */
    
    /* Get element at index - returns value, not pointer */
    JSPropertyEnum operator[](int index) const {
        JSPropertyEnum* p = get_ptr();
        if (p && index >= 0) {
            return p[index];
        }
        return JSPropertyEnum{};
    }
    
    /* Get atom at index */
    JSAtom get_atom(int index) const {
        JSPropertyEnum* p = get_ptr();
        if (p && index >= 0) {
            return p[index].atom;
        }
        return 0;
    }
    
    /* Get is_enumerable at index */
    BOOL get_is_enumerable(int index) const {
        JSPropertyEnum* p = get_ptr();
        if (p && index >= 0) {
            return p[index].is_enumerable;
        }
        return FALSE;
    }
    
    /* =====================================================================
     * Direct pointer access - USE WITH CAUTION
     * These return raw pointers for C-style array operations
     * ===================================================================== */
    
    /* Get pointer to array - USE WITH CAUTION, never store this pointer */
    JSPropertyEnum* ptr() const {
        return get_ptr();
    }
    
    /* =====================================================================
     * Conversion operators for interoperability
     * ===================================================================== */
    
    /* Convert to GCHandle for handle-based APIs */
    operator GCHandle() const {
        return handle_;
    }
    
    /* Comparison operators */
    bool operator==(const JSPropertyEnumHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSPropertyEnumHandle& other) const {
        return handle_ != other.handle_;
    }
    
    bool operator==(GCHandle h) const {
        return handle_ == h;
    }
    
    bool operator!=(GCHandle h) const {
        return handle_ != h;
    }
    
    bool operator==(decltype(nullptr)) const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    bool operator!=(decltype(nullptr)) const {
        return handle_ != GC_HANDLE_NULL;
    }
};


/* ============================================================================
 * C++ GCStringHandle Class - Safe GC-Managed String Data Access
 * ============================================================================
 * 
 * This class wraps a GCHandle for GC-managed string data (char arrays).
 * It provides safe access to string data with automatic cleanup.
 * 
 * USAGE:
 *   GCStringHandle str = GCStringHandle::alloc(size, type);
 *   char *data = str.data();  // Fresh dereference - valid until next GC point
 *   strcpy(data, "hello");
 *   
 *   // Or use with gc_strdup:
 *   GCStringHandle str = GCStringHandle::from_strdup("hello");
 */

class GCStringHandle {
private:
    GCHandle handle_;
    
    /* Private constructor - use factory methods */
    explicit GCStringHandle(GCHandle handle) : handle_(handle) {}

public:
    /* Default constructor - null handle */
    GCStringHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Copy constructor */
    GCStringHandle(const GCStringHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    GCStringHandle(GCStringHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    GCStringHandle& operator=(const GCStringHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    GCStringHandle& operator=(GCStringHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Get the underlying handle */
    GCHandle handle() const { return handle_; }
    
    /* Check if handle is valid */
    bool valid() const { 
        return handle_ != GC_HANDLE_NULL && gc_handle_is_valid(handle_);
    }
    
    /* Explicit bool conversion */
    explicit operator bool() const { return valid(); }
    
    /* =========================================================================
     * Factory methods for creating string handles
     * ========================================================================= */
    
    /* Allocate new GC-managed string buffer */
    static GCStringHandle alloc(size_t size, JSGCObjectTypeEnum type = JS_GC_OBJ_TYPE_DATA) {
        return GCStringHandle(gc_alloc(size, type));
    }
    
    /* Allocate zero-initialized string buffer */
    static GCStringHandle allocz(size_t size, JSGCObjectTypeEnum type = JS_GC_OBJ_TYPE_DATA) {
        return GCStringHandle(gc_allocz(size, type));
    }
    
    /* Create from strdup */
    static GCStringHandle from_strdup(const char *str) {
        return GCStringHandle(gc_strdup(str));
    }
    
    /* Create from strndup */
    static GCStringHandle from_strndup(const char *str, size_t n) {
        return GCStringHandle(gc_strndup(str, n));
    }
    
    /* Create from existing handle (for retrieving stored string handles) */
    static GCStringHandle from_handle(GCHandle handle) {
        return GCStringHandle(handle);
    }
    
    /* =========================================================================
     * Data access - fresh dereference on each call
     * ========================================================================= */
    
    /* Get fresh pointer to string data - only valid until next GC point */
    char* data() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (char*)gc_deref(handle_);
    }
    
    /* Convenience: dereference operator */
    char* operator->() const {
        return data();
    }
    
    /* Convenience: implicit conversion to char* (use with caution) */
    operator char*() const {
        return data();
    }
    
    /* =========================================================================
     * String operations
     * ========================================================================= */
    
    /* Get string length (uses strlen on dereferenced data) */
    size_t length() const {
        char *p = data();
        return p ? strlen(p) : 0;
    }
    
    /* Check if empty */
    bool empty() const {
        char *p = data();
        return !p || p[0] == '\0';
    }
    
    /* =========================================================================
     * Comparison operators
     * ========================================================================= */
    
    bool operator==(const GCStringHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const GCStringHandle& other) const {
        return handle_ != other.handle_;
    }
    
    bool operator==(GCHandle h) const {
        return handle_ == h;
    }
    
    bool operator!=(GCHandle h) const {
        return handle_ != h;
    }
    
    bool operator==(decltype(nullptr)) const {
        return handle_ == GC_HANDLE_NULL;
    }
    
    bool operator!=(decltype(nullptr)) const {
        return handle_ != GC_HANDLE_NULL;
    }
};

/* ============================================================================
 * JSPropertyArrayHandle - Safe handle wrapper for JSProperty arrays
 * 
 * This class wraps a GCHandle to a JSProperty array and provides safe
 * indexed access. Unlike raw pointers, this remains valid across GC compaction.
 * ============================================================================ */
class JSPropertyArrayHandle {
private:
    GCHandle handle_;
    
public:
    /* Default constructor */
    JSPropertyArrayHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit JSPropertyArrayHandle(GCHandle handle) : handle_(handle) {}
    
    /* Get the underlying handle */
    GCHandle handle() const { return handle_; }
    
    /* Check if valid */
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
    /* Get property at index - fresh dereference each time for GC safety */
    JSProperty* at(size_t index) const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        JSProperty* base = (JSProperty*)gc_deref(handle_);
        if (!base) return nullptr;
        return &base[index];
    }
    
    /* Array subscript operator - fresh dereference each time */
    JSProperty& operator[](size_t index) const {
        JSProperty* p = at(index);
        /* This should never happen if used correctly - 
         * handle must be valid and index in bounds */
        if (!p) {
            fprintf(stderr, "FATAL: JSPropertyArrayHandle null dereference at index %zu\n", index);
            abort();
        }
        return *p;
    }
    
    /* Get value at index - safe accessor */
    GCValue get_value(size_t index) const {
        JSProperty* p = at(index);
        return p ? p->u.value : GC_UNDEFINED;
    }
    
    /* Set value at index - safe accessor */
    void set_value(size_t index, const GCValue& val) const {
        JSProperty* p = at(index);
        if (p) {
            p->u.value = val;
            gc_write_barrier_for_heap_slot(&p->u.value, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* Get getter handle at index */
    GCHandle get_getter_handle(size_t index) const {
        JSProperty* p = at(index);
        return p ? p->u.getset.getter_handle : GC_HANDLE_NULL;
    }
    
    /* Set getter handle at index */
    void set_getter_handle(size_t index, GCHandle h) const {
        JSProperty* p = at(index);
        if (p) {
            p->u.getset.getter_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.getset.getter_handle, h);
        }
    }
    
    /* Get setter handle at index */
    GCHandle get_setter_handle(size_t index) const {
        JSProperty* p = at(index);
        return p ? p->u.getset.setter_handle : GC_HANDLE_NULL;
    }
    
    /* Set setter handle at index */
    void set_setter_handle(size_t index, GCHandle h) const {
        JSProperty* p = at(index);
        if (p) {
            p->u.getset.setter_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.getset.setter_handle, h);
        }
    }
    
    /* Get var_ref handle at index */
    GCHandle get_var_ref_handle(size_t index) const {
        JSProperty* p = at(index);
        return p ? p->u.var_ref_handle : GC_HANDLE_NULL;
    }
    
    /* Set var_ref handle at index */
    void set_var_ref_handle(size_t index, GCHandle h) const {
        JSProperty* p = at(index);
        if (p) {
            p->u.var_ref_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.var_ref_handle, h);
        }
    }
    
    /* Copy property from one index to another */
    void copy_property(size_t to_idx, size_t from_idx) const {
        JSProperty* to_p = at(to_idx);
        JSProperty* from_p = at(from_idx);
        if (to_p && from_p) *to_p = *from_p;
    }
};

/* ============================================================================
 * JSPropertyHandle - Handle wrapper for JSProperty
 * ============================================================================ */
class JSPropertyHandle {
private:
    GCHandle handle_;
    
    JSProperty* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSProperty*)gc_deref(handle_);
    }

public:
    JSPropertyHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSPropertyHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    /* value access (JS_PROP_NORMAL) */
    GCValue value() const {
        JSProperty* p = get_ptr();
        return p ? p->u.value : GC_UNDEFINED;
    }
    
    void set_value(const GCValue& val) {
        JSProperty* p = get_ptr();
        if (p) {
            p->u.value = val;
            gc_write_barrier_for_heap_slot(&p->u.value, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* getter_handle access (JS_PROP_GETSET) */
    GCHandle getter_handle() const {
        JSProperty* p = get_ptr();
        return p ? p->u.getset.getter_handle : GC_HANDLE_NULL;
    }
    
    void set_getter_handle(GCHandle h) {
        JSProperty* p = get_ptr();
        if (p) {
            p->u.getset.getter_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.getset.getter_handle, h);
        }
    }
    
    /* setter_handle access (JS_PROP_GETSET) */
    GCHandle setter_handle() const {
        JSProperty* p = get_ptr();
        return p ? p->u.getset.setter_handle : GC_HANDLE_NULL;
    }
    
    void set_setter_handle(GCHandle h) {
        JSProperty* p = get_ptr();
        if (p) {
            p->u.getset.setter_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.getset.setter_handle, h);
        }
    }
    
    /* var_ref_handle access (JS_PROP_VARREF) */
    GCHandle var_ref_handle() const {
        JSProperty* p = get_ptr();
        return p ? p->u.var_ref_handle : GC_HANDLE_NULL;
    }
    
    void set_var_ref_handle(GCHandle h) {
        JSProperty* p = get_ptr();
        if (p) {
            p->u.var_ref_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.var_ref_handle, h);
        }
    }
    
    /* realm_and_id access (JS_PROP_AUTOINIT) */
    uintptr_t realm_and_id() const {
        JSProperty* p = get_ptr();
        return p ? p->u.init.realm_and_id : 0;
    }
    
    void set_realm_and_id(uintptr_t val) {
        JSProperty* p = get_ptr();
        if (p) p->u.init.realm_and_id = val;
    }
    
    /* init opaque_handle access (JS_PROP_AUTOINIT) */
    GCHandle init_opaque_handle() const {
        JSProperty* p = get_ptr();
        return p ? p->u.init.opaque_handle : GC_HANDLE_NULL;
    }
    
    void set_init_opaque_handle(GCHandle h) {
        JSProperty* p = get_ptr();
        if (p) {
            p->u.init.opaque_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.init.opaque_handle, h);
        }
    }
    
    /* Direct pointer access - USE WITH CAUTION */
    JSProperty* ptr() const {
        return get_ptr();
    }
    
    /* Arrow operator for direct field access */
    JSProperty* operator->() const {
        return get_ptr();
    }
    
    /* Subscript operator for array access */
    JSProperty& operator[](size_t index) const {
        JSProperty* p = get_ptr();
        return p[index];
    }
    
    /* Conversion operators */
    operator GCHandle() const {
        return handle_;
    }
    
    bool operator==(const JSPropertyHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSPropertyHandle& other) const {
        return handle_ != other.handle_;
    }
};

/* ============================================================================
 * JSPromiseReactionDataHandle - Handle wrapper for JSPromiseReactionData
 * ============================================================================ */
class JSPromiseReactionDataHandle {
private:
    GCHandle handle_;
    
    JSPromiseReactionData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSPromiseReactionData*)gc_deref(handle_);
    }

public:
    JSPromiseReactionDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSPromiseReactionDataHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    /* resolving_funcs access */
    GCValue resolving_func(int idx) const {
        JSPromiseReactionData* p = get_ptr();
        if (p && idx >= 0 && idx < 2) return p->resolving_funcs[idx];
        return GC_UNDEFINED;
    }
    
    void set_resolving_func(int idx, const GCValue& val) {
        JSPromiseReactionData* p = get_ptr();
        if (p && idx >= 0 && idx < 2) {
            p->resolving_funcs[idx] = val;
            gc_write_barrier_for_heap_slot(&p->resolving_funcs[idx], GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* handler access */
    GCValue handler() const {
        JSPromiseReactionData* p = get_ptr();
        return p ? p->handler : GC_UNDEFINED;
    }
    
    void set_handler(const GCValue& val) {
        JSPromiseReactionData* p = get_ptr();
        if (p) {
            p->handler = val;
            gc_write_barrier_for_heap_slot(&p->handler, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* =========================================================================
     * INTERNAL USE ONLY - NOT FOR GENERAL USE
     * These methods provide raw pointer access for internal refactoring.
     * They will be removed once all callers are migrated to handle-based APIs.
     * ========================================================================= */
    
    /* DEPRECATED: Use handle-based APIs instead.
     * Returns raw pointer valid only immediately. Do not store. */
    JSPromiseReactionData* __unsafe_internal_ptr() const {
        return get_ptr();
    }
    
    /* TEMPORARY: For backward compatibility during migration.
     * This will be removed. Use handle-based accessors instead. */
    JSPromiseReactionData* ptr() const {
        return get_ptr();
    }
    
    /* TEMPORARY: For backward compatibility during migration. */
    JSPromiseReactionData* operator->() const {
        return get_ptr();
    }
    
    /* TEMPORARY: For backward compatibility during migration. */
    operator JSPromiseReactionData*() const {
        return get_ptr();
    }
    
    /* Conversion operators */
    operator GCHandle() const {
        return handle_;
    }
    
    bool operator==(const JSPromiseReactionDataHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSPromiseReactionDataHandle& other) const {
        return handle_ != other.handle_;
    }
};

/* ============================================================================
 * JSFinRecEntryHandle - Handle wrapper for JSFinRecEntry
 * ============================================================================ */
class JSFinRecEntryHandle {
private:
    GCHandle handle_;
    
    JSFinRecEntry* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSFinRecEntry*)gc_deref(handle_);
    }

public:
    JSFinRecEntryHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSFinRecEntryHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    /* target access */
    GCValue target() const {
        JSFinRecEntry* p = get_ptr();
        return p ? p->target : GC_UNDEFINED;
    }
    
    void set_target(const GCValue& val) {
        JSFinRecEntry* p = get_ptr();
        if (p) {
            p->target = val;
            gc_write_barrier_for_heap_slot(&p->target, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* held_val access */
    GCValue held_val() const {
        JSFinRecEntry* p = get_ptr();
        return p ? p->held_val : GC_UNDEFINED;
    }
    
    void set_held_val(const GCValue& val) {
        JSFinRecEntry* p = get_ptr();
        if (p) {
            p->held_val = val;
            gc_write_barrier_for_heap_slot(&p->held_val, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* token access */
    GCValue token() const {
        JSFinRecEntry* p = get_ptr();
        return p ? p->token : GC_UNDEFINED;
    }
    
    void set_token(const GCValue& val) {
        JSFinRecEntry* p = get_ptr();
        if (p) {
            p->token = val;
            gc_write_barrier_for_heap_slot(&p->token, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* Get self handle for gc_list operations */
    GCHandle link_handle() const {
        return handle_;
    }
    
    /* =========================================================================
     * INTERNAL USE ONLY - NOT FOR GENERAL USE
     * These methods provide raw pointer access for internal refactoring.
     * They will be removed once all callers are migrated to handle-based APIs.
     * ========================================================================= */
    
    /* DEPRECATED: Use handle-based APIs instead.
     * Returns raw pointer valid only immediately. Do not store. */
    JSFinRecEntry* __unsafe_internal_ptr() const {
        return get_ptr();
    }
    
    /* TEMPORARY: For backward compatibility during migration.
     * This will be removed. Use handle-based accessors instead. */
    JSFinRecEntry* ptr() const {
        return get_ptr();
    }
    
    /* TEMPORARY: For backward compatibility during migration. */
    JSFinRecEntry* operator->() const {
        return get_ptr();
    }
    
    /* TEMPORARY: For backward compatibility during migration. */
    operator JSFinRecEntry*() const {
        return get_ptr();
    }
    
    /* Conversion operators */
    operator GCHandle() const {
        return handle_;
    }
    
    bool operator==(const JSFinRecEntryHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSFinRecEntryHandle& other) const {
        return handle_ != other.handle_;
    }
};

/* ============================================================================
 * JSAsyncGeneratorRequestHandle - Handle wrapper for JSAsyncGeneratorRequest
 * ============================================================================ */
class JSAsyncGeneratorRequestHandle {
private:
    GCHandle handle_;
    
    JSAsyncGeneratorRequest* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSAsyncGeneratorRequest*)gc_deref(handle_);
    }

public:
    JSAsyncGeneratorRequestHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSAsyncGeneratorRequestHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    /* completion_type access */
    int completion_type() const {
        JSAsyncGeneratorRequest* p = get_ptr();
        return p ? p->completion_type : 0;
    }
    
    void set_completion_type(int val) {
        JSAsyncGeneratorRequest* p = get_ptr();
        if (p) p->completion_type = val;
    }
    
    /* result access */
    GCValue result() const {
        JSAsyncGeneratorRequest* p = get_ptr();
        return p ? p->result : GC_UNDEFINED;
    }
    
    void set_result(const GCValue& val) {
        JSAsyncGeneratorRequest* p = get_ptr();
        if (p) {
            p->result = val;
            gc_write_barrier_for_heap_slot(&p->result, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* promise access */
    GCValue promise() const {
        JSAsyncGeneratorRequest* p = get_ptr();
        return p ? p->promise : GC_UNDEFINED;
    }
    
    void set_promise(const GCValue& val) {
        JSAsyncGeneratorRequest* p = get_ptr();
        if (p) {
            p->promise = val;
            gc_write_barrier_for_heap_slot(&p->promise, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* resolving_funcs access */
    GCValue resolving_func(int idx) const {
        JSAsyncGeneratorRequest* p = get_ptr();
        if (p && idx >= 0 && idx < 2) return p->resolving_funcs[idx];
        return GC_UNDEFINED;
    }
    
    void set_resolving_func(int idx, const GCValue& val) {
        JSAsyncGeneratorRequest* p = get_ptr();
        if (p && idx >= 0 && idx < 2) {
            p->resolving_funcs[idx] = val;
            gc_write_barrier_for_heap_slot(&p->resolving_funcs[idx], GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* Direct pointer access - USE WITH CAUTION */
    JSAsyncGeneratorRequest* ptr() const {
        return get_ptr();
    }
    
    /* Arrow operator for direct field access */
    JSAsyncGeneratorRequest* operator->() const {
        return get_ptr();
    }
    
    /* Implicit conversion to pointer for array storage */
    operator JSAsyncGeneratorRequest*() const {
        return get_ptr();
    }
    
    /* Get self handle for gc_list operations */
    GCHandle link_handle() const {
        return handle_;
    }
    
    /* DEPRECATED: Use handle-based APIs instead.
     * Returns raw pointer valid only immediately. Do not store. */
    JSAsyncGeneratorRequest* __unsafe_internal_ptr() const {
        return get_ptr();
    }
};

/* ============================================================================
 * C++ JSPromiseFunctionDataResolvedHandle Class - Safe GC Handle Wrapper
 * ============================================================================
 * 
 * This class wraps a GCHandle to provide safe access to JSPromiseFunctionDataResolved instances.
 */

class JSPromiseFunctionDataResolvedHandle {
private:
    GCHandle handle_;
    
    JSPromiseFunctionDataResolved* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSPromiseFunctionDataResolved*)gc_deref(handle_);
    }

public:
    JSPromiseFunctionDataResolvedHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSPromiseFunctionDataResolvedHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL && gc_handle_is_valid(handle_); }
    explicit operator bool() const { return valid(); }
    
    JSPromiseFunctionDataResolved* ptr() const { return get_ptr(); }
    
    int ref_count() const {
        JSPromiseFunctionDataResolved* p = get_ptr();
        return p ? p->ref_count : 0;
    }
    
    void set_ref_count(int val) {
        JSPromiseFunctionDataResolved* p = get_ptr();
        if (p) p->ref_count = val;
    }
    
    BOOL already_resolved() const {
        JSPromiseFunctionDataResolved* p = get_ptr();
        return p ? p->already_resolved : FALSE;
    }
    
    void set_already_resolved(BOOL val) {
        JSPromiseFunctionDataResolved* p = get_ptr();
        if (p) p->already_resolved = val;
    }
    
    GCHandle self_handle() const {
        JSPromiseFunctionDataResolved* p = get_ptr();
        return p ? p->self_handle : GC_HANDLE_NULL;
    }
    
    void set_self_handle(GCHandle val) {
        JSPromiseFunctionDataResolved* p = get_ptr();
        if (p) {
            p->self_handle = val;
            gc_write_barrier_for_heap_slot(&p->self_handle, val);
        }
    }
    
    operator GCHandle() const { return handle_; }
    
    bool operator==(const JSPromiseFunctionDataResolvedHandle& other) const { return handle_ == other.handle_; }
    bool operator!=(const JSPromiseFunctionDataResolvedHandle& other) const { return handle_ != other.handle_; }
};


/* ============================================================================
 * C++ JSClassHandle Class - Safe indexed access to JSClass array
 * ============================================================================
 * 
 * This class provides safe indexed access to JSClass arrays stored in GC.
 */

class JSClassHandle {
private:
    GCHandle array_handle_;
    int index_;
    
    JSClass* get_ptr() const {
        if (array_handle_ == GC_HANDLE_NULL) return nullptr;
        JSClass* arr = (JSClass*)gc_deref(array_handle_);
        return arr ? &arr[index_] : nullptr;
    }

public:
    JSClassHandle() : array_handle_(GC_HANDLE_NULL), index_(0) {}
    JSClassHandle(GCHandle array_handle, int index) : array_handle_(array_handle), index_(index) {}
    
    GCHandle array_handle() const { return array_handle_; }
    int index() const { return index_; }
    bool valid() const { return array_handle_ != GC_HANDLE_NULL && gc_handle_is_valid(array_handle_); }
    explicit operator bool() const { return valid(); }
    
    /* Access class_name through the handle */
    JSAtom class_name() const {
        JSClass* p = get_ptr();
        return p ? p->class_name : 0;
    }
    
    JSClass* ptr() const { return get_ptr(); }
    
    /* Array access operator for indexed retrieval */
    JSClassHandle operator[](int idx) const {
        return JSClassHandle(array_handle_, idx);
    }
};

/* ============================================================================
 * JSObjectListEntryArrayHandle - Handle wrapper for JSObjectListEntry arrays
 * ============================================================================ */
class JSObjectListEntryArrayHandle {
private:
    GCHandle handle_;
    
    JSObjectListEntry* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSObjectListEntry*)gc_deref(handle_);
    }

public:
    JSObjectListEntryArrayHandle() : handle_(GC_HANDLE_NULL) {}
    explicit JSObjectListEntryArrayHandle(GCHandle handle) : handle_(handle) {}
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
    /* Array access - returns pointer to element at index */
    JSObjectListEntry* operator[](int index) const {
        JSObjectListEntry* p = get_ptr();
        return p ? &p[index] : nullptr;
    }
    
    /* Get raw pointer to array */
    JSObjectListEntry* ptr() const {
        return get_ptr();
    }
};


#endif /* QUICKJS_HANDLE_CLASSES_H */

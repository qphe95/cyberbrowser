/*
 * QuickJS handle helper functions
 * ============================================================================
 *
 * C static-inline replacements for the non-trivial methods of the old C++
 * handle wrapper classes (JSRuntimeHandle::atom_size(), JSObjectHandle::
 * shape_handle(), JSMapStateHandle::hash_table_entry(), ...).
 *
 * Plain field access uses the SAFE_GET / SAFE_SET family from
 * quickjs_handles.h. The helpers below exist only where the old accessor did
 * more than read or write one field: atomics, bounds checks, seqlock retry
 * loops, write barriers on computed slots, and so on. Each one takes the
 * object's GCHandle as its first argument and dereferences it on every call,
 * as the old class methods did, so a raw pointer is never kept across a GC
 * point.
 *
 * Naming: <prefix>_<old method name>, where the prefix identifies the handle
 * type (js_rt_ = JSRuntimeHandle, js_ctx_ = JSContextHandle, js_obj_ =
 * JSObjectHandle, js_bc_ = JSFunctionBytecodeHandle, ...). JSVarRefHandle
 * helpers (js_varref_*) live in quickjs-internal.h.
 * ============================================================================
 */

#ifndef QUICKJS_HANDLE_HELPERS_H
#define QUICKJS_HANDLE_HELPERS_H

#include "quickjs-internal.h"
#include "quickjs_handles.h"

/* ------------------------------------------------------------------------
 * GCStringHandle (char buffer)
 * ------------------------------------------------------------------------ */

static inline size_t gc_string_length(GCHandle handle)
{
    char *p = ((char *)gc_deref(handle));
    return p ? strlen(p) : 0;
}

/* ------------------------------------------------------------------------
 * JSArrayBufferHandle (struct JSArrayBuffer)
 * ------------------------------------------------------------------------ */

static inline uint8_t* js_abuf_data(GCHandle handle)
{
    JSArrayBuffer* p = GC_DEREF_H(handle, JSArrayBufferHandle);
    if (!p) return NULL;

    if (p->data_handle != GC_HANDLE_NULL)
        return (uint8_t *)gc_deref(p->data_handle);
    return p->data;
}

static inline bool js_abuf_is_resizable(GCHandle handle)
{
    JSArrayBuffer* p = GC_DEREF_H(handle, JSArrayBufferHandle);
    return p ? p->max_byte_length >= 0 : false;
}

static inline GCHandleList& js_abuf_typed_arrays(GCHandle handle)
{
    JSArrayBuffer* p = GC_DEREF_H(handle, JSArrayBufferHandle);
    static GCHandleList empty_list;
    return p ? p->typed_arrays : empty_list;
}

/* ------------------------------------------------------------------------
 * JSAsyncFunctionStateHandle (struct JSAsyncFunctionState)
 * ------------------------------------------------------------------------ */

static inline void js_async_func_state_clear_stack_top(GCHandle handle)
{
    JSAsyncFunctionState* p = GC_DEREF_H(handle, JSAsyncFunctionStateHandle);
    if (p && p->frame.sp_offset > 0) {
        GCValue* var_buf = JS_SF_VAR_BUF(&p->frame);
        var_buf[p->frame.sp_offset - 1] = GC_UNDEFINED;
    }
}

static inline JSStackFrame* js_async_func_state_frame_ptr(GCHandle handle)
{
    JSAsyncFunctionState* p = GC_DEREF_H(handle, JSAsyncFunctionStateHandle);
    return p ? &p->frame : NULL;
}

static inline GCValue js_async_func_state_resolving_func(GCHandle handle, int idx)
{
    JSAsyncFunctionState* p = GC_DEREF_H(handle, JSAsyncFunctionStateHandle);
    if (p && idx >= 0 && idx < 2) return p->resolving_funcs[idx];
    return GC_UNDEFINED;
}

static inline void js_async_func_state_set_resolving_func(GCHandle handle, int idx, GCValue val)
{
    JSAsyncFunctionState* p = GC_DEREF_H(handle, JSAsyncFunctionStateHandle);
    if (p && idx >= 0 && idx < 2) {
        p->resolving_funcs[idx] = val;
        gc_write_barrier_for_heap_slot(&p->resolving_funcs[idx], GC_VALUE_GET_HANDLE(val));
    }
}

static inline void js_async_func_state_set_stack_top(GCHandle handle, GCValue val)
{
    JSAsyncFunctionState* p = GC_DEREF_H(handle, JSAsyncFunctionStateHandle);
    if (p && p->frame.sp_offset > 0) {
        GCValue* var_buf = JS_SF_VAR_BUF(&p->frame);
        var_buf[p->frame.sp_offset - 1] = val;
        gc_write_barrier_for_heap_slot(&var_buf[p->frame.sp_offset - 1], GC_VALUE_GET_HANDLE(val));
    }
}

static inline void js_async_func_state_stack_push(GCHandle handle, GCValue val)
{
    JSAsyncFunctionState* p = GC_DEREF_H(handle, JSAsyncFunctionStateHandle);
    if (p) {
        GCValue* var_buf = JS_SF_VAR_BUF(&p->frame);
        var_buf[p->frame.sp_offset] = val;
        p->frame.sp_offset++;
    }
}

static inline GCValue js_async_func_state_stack_top(GCHandle handle)
{
    JSAsyncFunctionState* p = GC_DEREF_H(handle, JSAsyncFunctionStateHandle);
    if (p && p->frame.sp_offset > 0) {
        GCValue* var_buf = JS_SF_VAR_BUF(&p->frame);
        return var_buf[p->frame.sp_offset - 1];
    }
    return GC_UNDEFINED;
}

/* ------------------------------------------------------------------------
 * JSAsyncGeneratorDataHandle (struct JSAsyncGeneratorData)
 * ------------------------------------------------------------------------ */

static inline bool js_async_gen_queue_empty(GCHandle handle)
{
    JSAsyncGeneratorData* p = GC_DEREF_H(handle, JSAsyncGeneratorDataHandle);
    return !p || p->queue.next == GC_HANDLE_NULL;
}

/* ------------------------------------------------------------------------
 * JSAsyncGeneratorRequestHandle (struct JSAsyncGeneratorRequest)
 * ------------------------------------------------------------------------ */

static inline GCValue js_async_gen_req_resolving_func(GCHandle handle, int idx)
{
    JSAsyncGeneratorRequest* p = GC_DEREF_H(handle, JSAsyncGeneratorRequestHandle);
    if (p && idx >= 0 && idx < 2) return p->resolving_funcs[idx];
    return GC_UNDEFINED;
}

static inline void js_async_gen_req_set_resolving_func(GCHandle handle, int idx, GCValue val)
{
    JSAsyncGeneratorRequest* p = GC_DEREF_H(handle, JSAsyncGeneratorRequestHandle);
    if (p && idx >= 0 && idx < 2) {
        p->resolving_funcs[idx] = val;
        gc_write_barrier_for_heap_slot(&p->resolving_funcs[idx], GC_VALUE_GET_HANDLE(val));
    }
}

/* ------------------------------------------------------------------------
 * JSBigIntHandle (struct JSBigInt)
 * ------------------------------------------------------------------------ */

static inline js_limb_t js_bigint_h_get_limb(GCHandle handle, uint32_t index)
{
    JSBigInt* p = GC_DEREF_H(handle, JSBigIntHandle);
    if (p && index < p->len) {
        return p->tab[index];
    }
    return 0;
}

static inline JSBigInt* js_bigint_h_unsafe_data(GCHandle handle)
{
    if (handle == GC_HANDLE_NULL) return NULL;

    if (handle > 0x10000000) {
        return (JSBigInt*)(uintptr_t)handle;
    }
    return (JSBigInt*)gc_deref_unsafe(handle);
}

/* ------------------------------------------------------------------------
 * JSBoundFunctionHandle (struct JSBoundFunction)
 * ------------------------------------------------------------------------ */

static inline GCValue js_bound_func_argv(GCHandle handle, int idx)
{
    JSBoundFunction* p = GC_DEREF_H(handle, JSBoundFunctionHandle);
    if (!p || idx < 0 || idx >= p->argc) return GC_UNDEFINED;
    return p->argv[idx];
}

static inline int js_bound_func_copy_argv_to(GCHandle handle, GCValue* dest, int max_count)
{
    JSBoundFunction* p = GC_DEREF_H(handle, JSBoundFunctionHandle);
    if (!p || !dest || max_count <= 0) return 0;
    int count = (p->argc < max_count) ? p->argc : max_count;
    for (int i = 0; i < count; i++) {
        dest[i] = p->argv[i];
    }
    return count;
}

/* ------------------------------------------------------------------------
 * JSCFunctionDataRecordHandle (struct JSCFunctionDataRecord)
 * ------------------------------------------------------------------------ */

static inline GCValue js_cfunc_data_data(GCHandle handle, int idx)
{
    JSCFunctionDataRecord* p = GC_DEREF_H(handle, JSCFunctionDataRecordHandle);
    if (!p || idx < 0 || idx >= p->data_len) return GC_UNDEFINED;
    return p->data[idx];
}

static inline void js_cfunc_data_set_data(GCHandle handle, int idx, GCValue v)
{
    JSCFunctionDataRecord* p = GC_DEREF_H(handle, JSCFunctionDataRecordHandle);
    if (p && idx >= 0 && idx < p->data_len) {
        p->data[idx] = v;
        gc_write_barrier_for_heap_slot(&p->data[idx], GC_VALUE_GET_HANDLE(v));
    }
}

/* ------------------------------------------------------------------------
 * JSContextHandle (struct JSContext)
 * ------------------------------------------------------------------------ */

static inline void js_ctx_class_proto_lock_acquire(GCHandle handle)
{
    JSContext* p = GC_DEREF_H(handle, JSContextHandle);
    if (!p) return;
    while (atomic_compare_exchange_u32(&p->class_proto_lock, 0, 1) != 0) {

    }
}

static inline void js_ctx_class_proto_lock_release(GCHandle handle)
{
    JSContext* p = GC_DEREF_H(handle, JSContextHandle);
    if (p) atomic_store_u32(&p->class_proto_lock, 0);
}

static inline uint32_t js_ctx_class_proto_version_fetch_add(GCHandle handle, uint32_t delta)
{
    JSContext* p = GC_DEREF_H(handle, JSContextHandle);
    if (!p) return 0;
    return atomic_fetch_add_u32((volatile uint32_t *)&p->class_proto_version, delta);
}

static inline uint32_t js_ctx_class_proto_version(GCHandle handle)
{
    JSContext* p = GC_DEREF_H(handle, JSContextHandle);
    return p ? atomic_load_u32((volatile uint32_t *)&p->class_proto_version) : 0;
}

static inline GCHandleList& js_ctx_loaded_modules(GCHandle handle)
{
    JSContext* p = GC_DEREF_H(handle, JSContextHandle);
    static GCHandleList dummy;
    return p ? p->loaded_modules : dummy;
}

static inline void js_ctx_zero_memory(GCHandle handle)
{
    JSContext* p = GC_DEREF_H(handle, JSContextHandle);
    if (p) memset(p, 0, sizeof(JSContext));
}

/* ------------------------------------------------------------------------
 * JSFinalizationRegistryDataHandle (struct JSFinalizationRegistryData)
 * ------------------------------------------------------------------------ */

static inline void js_finreg_init_entries(GCHandle handle)
{
    JSFinalizationRegistryData* p = GC_DEREF_H(handle, JSFinalizationRegistryDataHandle);
    if (p) gc_list_init(&p->entries);
}

/* ------------------------------------------------------------------------
 * JSFunctionBytecodeHandle (struct JSFunctionBytecode)
 * ------------------------------------------------------------------------ */

static inline uint8_t* js_bc_byte_code_buf_at_offset_ptr(GCHandle handle)
{
    JSFunctionBytecode* p = GC_DEREF_H(handle, JSFunctionBytecodeHandle);
    return p ? (uint8_t*)p + p->byte_code_offset : NULL;
}

static inline uint8_t* js_bc_byte_code_buf(GCHandle handle)
{
    JSFunctionBytecode* p = GC_DEREF_H(handle, JSFunctionBytecodeHandle);
    if (!p) return NULL;
    if (p->byte_code_handle == GC_HANDLE_NULL) return NULL;

    if (p->byte_code_handle == handle) {
        return (uint8_t*)gc_deref(p->byte_code_handle) + p->byte_code_offset;
    }
    return (uint8_t*)gc_deref(p->byte_code_handle);
}

static inline uint8_t js_bc_closure_var_closure_type(GCHandle handle, int index)
{
    JSFunctionBytecode* p = GC_DEREF_H(handle, JSFunctionBytecodeHandle);
    if (!p || index < 0 || index >= p->closure_var_count) return 0;
    JSClosureVar* cv = (JSClosureVar*)((uint8_t*)p + p->closure_var_offset + index * sizeof(JSClosureVar));
    return (uint8_t)cv->closure_type;
}

static inline BOOL js_bc_closure_var_is_const(GCHandle handle, int index)
{
    JSFunctionBytecode* p = GC_DEREF_H(handle, JSFunctionBytecodeHandle);
    if (!p || index < 0 || index >= p->closure_var_count) return FALSE;
    JSClosureVar* cv = (JSClosureVar*)((uint8_t*)p + p->closure_var_offset + index * sizeof(JSClosureVar));
    return cv->is_const;
}

static inline BOOL js_bc_closure_var_is_lexical(GCHandle handle, int index)
{
    JSFunctionBytecode* p = GC_DEREF_H(handle, JSFunctionBytecodeHandle);
    if (!p || index < 0 || index >= p->closure_var_count) return FALSE;
    JSClosureVar* cv = (JSClosureVar*)((uint8_t*)p + p->closure_var_offset + index * sizeof(JSClosureVar));
    return cv->is_lexical;
}

static inline void* js_bc_closure_var_ptr(GCHandle handle)
{
    JSFunctionBytecode* p = GC_DEREF_H(handle, JSFunctionBytecodeHandle);
    return p ? (uint8_t*)p + p->closure_var_offset : NULL;
}

static inline uint16_t js_bc_closure_var_var_idx(GCHandle handle, int index)
{
    JSFunctionBytecode* p = GC_DEREF_H(handle, JSFunctionBytecodeHandle);
    if (!p || index < 0 || index >= p->closure_var_count) return 0;
    JSClosureVar* cv = (JSClosureVar*)((uint8_t*)p + p->closure_var_offset + index * sizeof(JSClosureVar));
    return cv->var_idx;
}

static inline uint8_t js_bc_closure_var_var_kind(GCHandle handle, int index)
{
    JSFunctionBytecode* p = GC_DEREF_H(handle, JSFunctionBytecodeHandle);
    if (!p || index < 0 || index >= p->closure_var_count) return 0;
    JSClosureVar* cv = (JSClosureVar*)((uint8_t*)p + p->closure_var_offset + index * sizeof(JSClosureVar));
    return cv->var_kind;
}

static inline JSAtom js_bc_closure_var_var_name(GCHandle handle, int index)
{
    JSFunctionBytecode* p = GC_DEREF_H(handle, JSFunctionBytecodeHandle);
    if (!p || index < 0 || index >= p->closure_var_count) return 0;  
    JSClosureVar* cv = (JSClosureVar*)((uint8_t*)p + p->closure_var_offset + index * sizeof(JSClosureVar));
    return cv->var_name;
}

static inline GCValue* js_bc_cpool_ptr(GCHandle handle)
{
    JSFunctionBytecode* p = GC_DEREF_H(handle, JSFunctionBytecodeHandle);
    return p ? (GCValue*)((uint8_t*)p + p->cpool_offset) : NULL;
}

static inline GCValue js_bc_cpool_value(GCHandle handle, int index)
{
    JSFunctionBytecode* p = GC_DEREF_H(handle, JSFunctionBytecodeHandle);
    if (!p || index < 0 || index >= p->cpool_count) return GC_UNDEFINED;
    return ((GCValue*)((uint8_t*)p + p->cpool_offset))[index];
}

static inline void js_bc_memcpy_from(GCHandle handle, const void* src, size_t size)
{
    JSFunctionBytecode* p = GC_DEREF_H(handle, JSFunctionBytecodeHandle);
    if (p) memcpy(p, src, size);
}

static inline void js_bc_memset_zero(GCHandle handle, size_t size)
{
    JSFunctionBytecode* p = GC_DEREF_H(handle, JSFunctionBytecodeHandle);
    if (p) memset(p, 0, size);
}

static inline JSLazyParseState* js_bc_resume_parse_state(GCHandle handle)
{
    JSFunctionBytecode* p = GC_DEREF_H(handle, JSFunctionBytecodeHandle);
    return p ? &p->resume_parse_state : NULL;
}

static inline void js_bc_set_cpool_value(GCHandle handle, int index, GCValue val)
{
    JSFunctionBytecode* p = GC_DEREF_H(handle, JSFunctionBytecodeHandle);
    if (!p || index < 0 || index >= p->cpool_count) return;
    ((GCValue*)((uint8_t*)p + p->cpool_offset))[index] = val;
}

static inline void* js_bc_vardefs_ptr(GCHandle handle)
{
    JSFunctionBytecode* p = GC_DEREF_H(handle, JSFunctionBytecodeHandle);
    return p ? (uint8_t*)p + p->vardefs_offset : NULL;
}

/* ------------------------------------------------------------------------
 * JSJobEntryHandle (struct JSJobEntry)
 * ------------------------------------------------------------------------ */

static inline GCValue js_job_argv_at(GCHandle handle, int idx)
{
    JSJobEntry* p = GC_DEREF_H(handle, JSJobEntryHandle);
    if (p && idx >= 0 && idx < p->argc) return p->argv[idx];
    return GC_UNDEFINED;
}

static inline void js_job_set_argv_at(GCHandle handle, int idx, GCValue val)
{
    JSJobEntry* p = GC_DEREF_H(handle, JSJobEntryHandle);
    if (p && idx >= 0 && idx < p->argc) {
        p->argv[idx] = val;
        gc_write_barrier_for_heap_slot(&p->argv[idx], GC_VALUE_GET_HANDLE(val));
    }
}

/* ------------------------------------------------------------------------
 * JSMapStateHandle (struct JSMapState)
 * ------------------------------------------------------------------------ */

static inline GCHandle js_map_state_hash_table_entry(GCHandle handle, uint32_t idx)
{
    JSMapState* p = GC_DEREF_H(handle, JSMapStateHandle);
    if (!p || idx >= (uint32_t)(1U << p->hash_bits)) return GC_HANDLE_NULL;
    GCHandle* table = (GCHandle*)gc_deref(p->hash_table_handle);
    return table ? table[idx] : GC_HANDLE_NULL;
}

static inline void js_map_state_init_records(GCHandle handle)
{
    JSMapState* p = GC_DEREF_H(handle, JSMapStateHandle);
    if (p) gc_list_init(&p->records);
}

static inline bool js_map_state_needs_resize(GCHandle handle)
{
    JSMapState* p = GC_DEREF_H(handle, JSMapStateHandle);
    return p && p->record_count >= p->record_count_threshold;
}

static inline void js_map_state_set_hash_table_entry(GCHandle handle, uint32_t idx, GCHandle h)
{
    JSMapState* p = GC_DEREF_H(handle, JSMapStateHandle);
    if (!p || idx >= (uint32_t)(1U << p->hash_bits)) return;
    GCHandle* table = (GCHandle*)gc_deref(p->hash_table_handle);
    if (table) {
        table[idx] = h;
        gc_write_barrier_for_heap_slot(&table[idx], h);
    }
}

/* ------------------------------------------------------------------------
 * JSModuleDefHandle (struct JSModuleDef)
 * ------------------------------------------------------------------------ */

static inline GCHandle** js_module_async_parent_modules_handle_ptr_ptr(GCHandle handle)
{
    JSModuleDef* p = GC_DEREF_H(handle, JSModuleDefHandle);
    return p ? (GCHandle**)&p->async_parent_modules_handle : NULL;
}

static inline int* js_module_async_parent_modules_size_ptr(GCHandle handle)
{
    JSModuleDef* p = GC_DEREF_H(handle, JSModuleDefHandle);
    return p ? &p->async_parent_modules_size : NULL;
}

static inline int js_module_export_entries_count(GCHandle handle)
{
    JSModuleDef* p = GC_DEREF_H(handle, JSModuleDefHandle);
    return p ? p->export_entries.count() : 0;
}

static inline GCDataArray<JSExportEntry>& js_module_export_entries(GCHandle handle)
{
    JSModuleDef* p = GC_DEREF_H(handle, JSModuleDefHandle);
    static GCDataArray<JSExportEntry> empty;
    return p ? p->export_entries : empty;
}

static inline int js_module_import_entries_count(GCHandle handle)
{
    JSModuleDef* p = GC_DEREF_H(handle, JSModuleDefHandle);
    return p ? p->import_entries.count() : 0;
}

static inline GCDataArray<JSImportEntry>& js_module_import_entries(GCHandle handle)
{
    JSModuleDef* p = GC_DEREF_H(handle, JSModuleDefHandle);
    static GCDataArray<JSImportEntry> empty;
    return p ? p->import_entries : empty;
}

static inline GCValue* js_module_private_value_ptr(GCHandle handle)
{
    JSModuleDef* p = GC_DEREF_H(handle, JSModuleDefHandle);
    return p ? &p->private_value : NULL;
}

static inline int js_module_req_module_entries_count(GCHandle handle)
{
    JSModuleDef* p = GC_DEREF_H(handle, JSModuleDefHandle);
    return p ? p->req_module_entries.count() : 0;
}

static inline GCDataArray<JSReqModuleEntry>& js_module_req_module_entries(GCHandle handle)
{
    JSModuleDef* p = GC_DEREF_H(handle, JSModuleDefHandle);
    static GCDataArray<JSReqModuleEntry> empty;
    return p ? p->req_module_entries : empty;
}

static inline GCValue js_module_resolving_func(GCHandle handle, int idx)
{
    JSModuleDef* p = GC_DEREF_H(handle, JSModuleDefHandle);
    if (p && idx >= 0 && idx < 2) return p->resolving_funcs[idx];
    return GC_UNDEFINED;
}

static inline void js_module_set_resolving_func(GCHandle handle, int idx, GCValue val)
{
    JSModuleDef* p = GC_DEREF_H(handle, JSModuleDefHandle);
    if (p && idx >= 0 && idx < 2) {
        p->resolving_funcs[idx] = val;
        gc_write_barrier_for_heap_slot(&p->resolving_funcs[idx], GC_VALUE_GET_HANDLE(val));
    }
}

static inline int js_module_star_export_entries_count(GCHandle handle)
{
    JSModuleDef* p = GC_DEREF_H(handle, JSModuleDefHandle);
    return p ? p->star_export_entries.count() : 0;
}

static inline GCDataArray<JSStarExportEntry>& js_module_star_export_entries(GCHandle handle)
{
    JSModuleDef* p = GC_DEREF_H(handle, JSModuleDefHandle);
    static GCDataArray<JSStarExportEntry> empty;
    return p ? p->star_export_entries : empty;
}

/* ------------------------------------------------------------------------
 * JSObjectHandle (struct JSObject)
 * ------------------------------------------------------------------------ */

static inline GCHandle js_obj_prop_handle(GCHandle handle)
{
    if (handle == GC_HANDLE_NULL) return GC_HANDLE_NULL;
    JSObject* ptr = (JSObject*)gc_deref(handle);
    if (!ptr) return GC_HANDLE_NULL;
    return atomic_load_u32((volatile uint32_t *)&ptr->prop_handle);
}

static inline uint32_t js_obj_prop_version_fetch_add(GCHandle handle, uint32_t delta)
{
    JSObject* p = GC_DEREF_H(handle, JSObjectHandle);
    if (p) {
        return atomic_fetch_add_u32((volatile uint32_t *)&p->prop_version, delta);
    }
    return 0;
}

static inline uint32_t js_obj_prop_version(GCHandle handle)
{
    if (handle == GC_HANDLE_NULL) return 0;
    JSObject* ptr = (JSObject*)gc_deref(handle);
    if (!ptr) return 0;
    return atomic_load_u32((volatile uint32_t *)&ptr->prop_version);
}

static inline void js_obj_set_prop_handle(GCHandle handle, GCHandle prop)
{
    JSObject* p = GC_DEREF_H(handle, JSObjectHandle);
    if (p) {
        atomic_store_u32((volatile uint32_t *)&p->prop_handle, prop);
        gc_write_barrier_for_heap_slot(&p->prop_handle, prop);
    }
}

static inline void js_obj_set_prop_version(GCHandle handle, uint32_t v)
{
    JSObject* p = GC_DEREF_H(handle, JSObjectHandle);
    if (p) {
        atomic_store_u32((volatile uint32_t *)&p->prop_version, v);
    }
}

static inline void js_obj_set_shape_handle(GCHandle handle, GCHandle shape)
{
    JSObject* p = GC_DEREF_H(handle, JSObjectHandle);
    if (p) {
        atomic_store_u32((volatile uint32_t *)&p->shape_handle, shape);
        gc_write_barrier_for_heap_slot(&p->shape_handle, shape);
    }
}

static inline GCHandle js_obj_shape_handle(GCHandle handle)
{
    if (handle == GC_HANDLE_NULL) return GC_HANDLE_NULL;
    JSObject* ptr = (JSObject*)gc_deref(handle);
    if (!ptr) return GC_HANDLE_NULL;
    return atomic_load_u32((volatile uint32_t *)&ptr->shape_handle);
}

/* ------------------------------------------------------------------------
 * JSPromiseDataHandle (struct JSPromiseData)
 * ------------------------------------------------------------------------ */

static inline void js_promise_data_init_promise_reactions(GCHandle handle)
{
    JSPromiseData* p = GC_DEREF_H(handle, JSPromiseDataHandle);
    if (p) {
        gc_list_init(&p->promise_reactions[0]);
        gc_list_init(&p->promise_reactions[1]);
    }
}

static inline GCValue* js_promise_data_promise_result_ptr(GCHandle handle)
{
    JSPromiseData* p = GC_DEREF_H(handle, JSPromiseDataHandle);
    return p ? &p->promise_result : NULL;
}

/* ------------------------------------------------------------------------
 * JSPromiseReactionDataHandle (struct JSPromiseReactionData)
 * ------------------------------------------------------------------------ */

static inline GCValue js_promise_reaction_resolving_func(GCHandle handle, int idx)
{
    JSPromiseReactionData* p = GC_DEREF_H(handle, JSPromiseReactionDataHandle);
    if (p && idx >= 0 && idx < 2) return p->resolving_funcs[idx];
    return GC_UNDEFINED;
}

static inline void js_promise_reaction_set_resolving_func(GCHandle handle, int idx, GCValue val)
{
    JSPromiseReactionData* p = GC_DEREF_H(handle, JSPromiseReactionDataHandle);
    if (p && idx >= 0 && idx < 2) {
        p->resolving_funcs[idx] = val;
        gc_write_barrier_for_heap_slot(&p->resolving_funcs[idx], GC_VALUE_GET_HANDLE(val));
    }
}

/* ------------------------------------------------------------------------
 * JSPropertyArrayHandle (struct JSProperty)
 * ------------------------------------------------------------------------ */

static inline JSProperty* js_prop_array_at(GCHandle handle, size_t index)
{
    if (handle == GC_HANDLE_NULL) return NULL;
    JSProperty* base = (JSProperty*)gc_deref(handle);
    if (!base) return NULL;
    return &base[index];
}

static inline GCHandle js_prop_array_get_getter_handle(GCHandle handle, size_t index)
{
    JSProperty* p = js_prop_array_at(handle, index);
    return p ? p->u.getset.getter_handle : GC_HANDLE_NULL;
}

static inline GCHandle js_prop_array_get_setter_handle(GCHandle handle, size_t index)
{
    JSProperty* p = js_prop_array_at(handle, index);
    return p ? p->u.getset.setter_handle : GC_HANDLE_NULL;
}

static inline GCValue js_prop_array_get_value(GCHandle handle, size_t index)
{
    JSProperty* p = js_prop_array_at(handle, index);
    return p ? p->u.value : GC_UNDEFINED;
}

static inline GCHandle js_prop_array_get_var_ref_handle(GCHandle handle, size_t index)
{
    JSProperty* p = js_prop_array_at(handle, index);
    return p ? p->u.var_ref_handle : GC_HANDLE_NULL;
}

static inline void js_prop_array_set_value(GCHandle handle, size_t index, GCValue val)
{
    JSProperty* p = js_prop_array_at(handle, index);
    if (p) {
        p->u.value = val;
        gc_write_barrier_for_heap_slot(&p->u.value, GC_VALUE_GET_HANDLE(val));
    }
}

/* ------------------------------------------------------------------------
 * JSPropertyEnumHandle (struct JSPropertyEnum)
 * ------------------------------------------------------------------------ */

static inline JSAtom js_prop_enum_get_atom(GCHandle handle, int index)
{
    JSPropertyEnum* p = GC_DEREF_H(handle, JSPropertyEnumHandle);
    if (p && index >= 0) {
        return p[index].atom;
    }
    return 0;
}

static inline BOOL js_prop_enum_get_is_enumerable(GCHandle handle, int index)
{
    JSPropertyEnum* p = GC_DEREF_H(handle, JSPropertyEnumHandle);
    if (p && index >= 0) {
        return p[index].is_enumerable;
    }
    return FALSE;
}

/* ------------------------------------------------------------------------
 * JSRuntimeHandle (struct JSRuntime)
 * ------------------------------------------------------------------------ */

static inline GCHandle js_rt_atom_array_handle(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (!p) return GC_HANDLE_NULL;
    return atomic_load_u32((volatile uint32_t *)&p->atom_array_handle);
}

static inline void js_rt_atom_count_inc(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) atomic_fetch_add_u32((volatile uint32_t *)&p->atom_count, 1);
}

static inline int js_rt_atom_count(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    return p ? (int)atomic_load_u32((volatile uint32_t *)&p->atom_count) : 0;
}

static inline uint32_t js_rt_atom_free_index_fetch_add(GCHandle handle, int delta)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (!p) return 0;
    return atomic_fetch_add_u32((volatile uint32_t *)&p->atom_free_index, (uint32_t)delta);
}

static inline int js_rt_atom_free_index(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    return p ? (int)atomic_load_u32((volatile uint32_t *)&p->atom_free_index) : 0;
}

static inline void js_rt_atom_hash_count_dec(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) atomic_fetch_sub_u32(&p->atom_hash_count, 1);
}

static inline void js_rt_atom_hash_count_inc(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) atomic_fetch_add_u32(&p->atom_hash_count, 1);
}

static inline int js_rt_atom_hash_count(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    return p ? (int)atomic_load_u32(&p->atom_hash_count) : 0;
}

static inline LFHashTable ** js_rt_atom_hash_ptr_addr(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    return p ? &p->atom_hash : NULL;
}

static inline LFHashTable * js_rt_atom_hash_retired(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (!p) return NULL;
    return (LFHashTable *)atomic_load_ptr((void *volatile *)&p->atom_hash_retired);
}

static inline LFHashTable * js_rt_atom_hash(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (!p) return NULL;
    return (LFHashTable *)atomic_load_ptr((void *volatile *)&p->atom_hash);
}

static inline int js_rt_atom_size(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    return p ? (int)atomic_load_u32((volatile uint32_t *)&p->atom_size) : 0;
}

static inline GCHandle js_rt_class_array_handle(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (!p) return GC_HANDLE_NULL;
    return atomic_load_u32((volatile uint32_t *)&p->class_array_handle);
}

static inline void js_rt_class_array_lock_acquire(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (!p) return;
    while (atomic_compare_exchange_u32(&p->class_array_lock, 0, 1) != 0) {

    }
}

static inline void js_rt_class_array_lock_release(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) atomic_store_u32(&p->class_array_lock, 0);
}

static inline uint32_t js_rt_class_array_version_fetch_add(GCHandle handle, uint32_t delta)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (!p) return 0;
    return atomic_fetch_add_u32((volatile uint32_t *)&p->class_array_version, delta);
}

static inline uint32_t js_rt_class_array_version(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    return p ? atomic_load_u32((volatile uint32_t *)&p->class_array_version) : 0;
}

static inline int js_rt_class_count(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    return p ? (int)atomic_load_u32((volatile uint32_t *)&p->class_count) : 0;
}

static inline GCHandle js_rt_compare_exchange_job_queue_handle(GCHandle handle, GCHandle expected, GCHandle desired)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (!p) return expected;
    return atomic_compare_exchange_u32((volatile uint32_t *)&p->job_queue_handle, expected, desired);
}

static inline JSStackFrame* js_rt_current_stack_frame(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (!p || p->current_stack_frame_handle == GC_HANDLE_NULL) return NULL;
    return (JSStackFrame*)gc_deref(p->current_stack_frame_handle);
}

static inline GCHandle js_rt_job_queue_handle(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    return p ? atomic_load_u32((volatile uint32_t *)&p->job_queue_handle) : GC_HANDLE_NULL;
}

static inline void js_rt_set_atom_array_handle(GCHandle handle, GCHandle h)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) {
        atomic_store_u32((volatile uint32_t *)&p->atom_array_handle, h);
        gc_write_barrier_for_heap_slot(&p->atom_array_handle, h);
    }
}

static inline void js_rt_set_atom_count(GCHandle handle, int count)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) atomic_store_u32((volatile uint32_t *)&p->atom_count, (uint32_t)count);
}

static inline void js_rt_set_atom_free_index(GCHandle handle, int index)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) atomic_store_u32((volatile uint32_t *)&p->atom_free_index, (uint32_t)index);
}

static inline void js_rt_set_atom_hash_count(GCHandle handle, int count)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) atomic_store_u32(&p->atom_hash_count, (uint32_t)count);
}

static inline void js_rt_set_atom_hash_retired(GCHandle handle, LFHashTable *t)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) atomic_store_ptr((void *volatile *)&p->atom_hash_retired, (void *)t);
}

static inline void js_rt_set_atom_hash(GCHandle handle, LFHashTable *t)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) atomic_store_ptr((void *volatile *)&p->atom_hash, (void *)t);
}

static inline void js_rt_set_atom_size(GCHandle handle, int size)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) atomic_store_u32((volatile uint32_t *)&p->atom_size, (uint32_t)size);
}

static inline void js_rt_set_class_array_handle(GCHandle handle, GCHandle h)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) {
        atomic_store_u32((volatile uint32_t *)&p->class_array_handle, h);
        gc_write_barrier_for_heap_slot(&p->class_array_handle, h);
    }
}

static inline void js_rt_set_class_array_lock(GCHandle handle, uint32_t val)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) atomic_store_u32(&p->class_array_lock, val);
}

static inline void js_rt_set_class_count(GCHandle handle, int count)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) atomic_store_u32((volatile uint32_t *)&p->class_count, (uint32_t)count);
}

static inline void js_rt_set_current_stack_frame(GCHandle handle, JSStackFrame* frame)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) p->current_stack_frame_handle = frame ? frame->self_handle : GC_HANDLE_NULL;
}

static inline void js_rt_set_job_queue_handle(GCHandle handle, GCHandle h)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) {
        atomic_store_u32((volatile uint32_t *)&p->job_queue_handle, h);
        gc_write_barrier_for_heap_slot(&p->job_queue_handle, h);
    }
}

static inline void js_rt_set_shape_hash_count(GCHandle handle, int count)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) atomic_store_u32(&p->shape_hash_count, (uint32_t)count);
}

static inline void js_rt_set_shape_hash_retired(GCHandle handle, LFHashTable *t)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) atomic_store_ptr((void *volatile *)&p->shape_hash_retired, (void *)t);
}

static inline void js_rt_set_shape_hash(GCHandle handle, LFHashTable *t)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) atomic_store_ptr((void *volatile *)&p->shape_hash, (void *)t);
}

static inline void js_rt_shape_hash_count_dec(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) atomic_fetch_sub_u32(&p->shape_hash_count, 1);
}

static inline void js_rt_shape_hash_count_inc(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) atomic_fetch_add_u32(&p->shape_hash_count, 1);
}

static inline int js_rt_shape_hash_count(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    return p ? (int)atomic_load_u32(&p->shape_hash_count) : 0;
}

static inline LFHashTable ** js_rt_shape_hash_ptr_addr(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    return p ? &p->shape_hash : NULL;
}

static inline LFHashTable * js_rt_shape_hash_retired(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (!p) return NULL;
    return (LFHashTable *)atomic_load_ptr((void *volatile *)&p->shape_hash_retired);
}

static inline LFHashTable * js_rt_shape_hash(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (!p) return NULL;
    return (LFHashTable *)atomic_load_ptr((void *volatile *)&p->shape_hash);
}

static inline int js_rt_shape_hash_size(GCHandle handle)
{
    LFHashTable *t = js_rt_shape_hash(handle);
    return t ? (int)t->bucket_count : 0;
}

static inline void js_rt_zero_memory(GCHandle handle)
{
    JSRuntime* p = GC_DEREF_H(handle, JSRuntimeHandle);
    if (p) memset(p, 0, sizeof(JSRuntime));
}

static inline GCHandle* js_rt_atom_array_ptr(GCHandle handle)
{
    GCHandle h = js_rt_atom_array_handle(handle);
    return h != GC_HANDLE_NULL ? (GCHandle*)gc_deref(h) : NULL;
}

static inline void* js_rt_class_array_ptr(GCHandle handle)
{
    for (;;) {
        uint32_t v0 = js_rt_class_array_version(handle);
        if ((v0 & 1) != 0) continue;
        GCHandle h = js_rt_class_array_handle(handle);
        void *ptr = h != GC_HANDLE_NULL ? gc_deref(h) : NULL;
        if (js_rt_class_array_version(handle) == v0)
            return ptr;
    }
}

/* ------------------------------------------------------------------------
 * JSShapeHandle (struct JSShape)
 * ------------------------------------------------------------------------ */

static inline GCHandle js_shape_proto_handle_atomic_load(GCHandle handle)
{
    JSShape* p = GC_DEREF_H(handle, JSShapeHandle);
    if (!p) return GC_HANDLE_NULL;
    for (;;) {
        uint32_t v0 = atomic_load_u32((volatile uint32_t *)&p->proto_version);
        if ((v0 & 1) != 0) continue;
        GCHandle h = atomic_load_u32((volatile uint32_t *)&p->proto_handle);
        if (atomic_load_u32((volatile uint32_t *)&p->proto_version) == v0)
            return h;
    }
}

static inline void js_shape_proto_handle_atomic_store(GCHandle handle, GCHandle val)
{
    JSShape* p = GC_DEREF_H(handle, JSShapeHandle);
    if (!p) return;
    atomic_fetch_add_u32((volatile uint32_t *)&p->proto_version, 1); 
    atomic_store_u32((volatile uint32_t *)&p->proto_handle, val);
    gc_write_barrier_for_heap_slot(&p->proto_handle, val);
    atomic_fetch_add_u32((volatile uint32_t *)&p->proto_version, 1); 
}

static inline void js_shape_set_proto_version(GCHandle handle, uint32_t val)
{
    JSShape* p = GC_DEREF_H(handle, JSShapeHandle);
    if (p) atomic_store_u32((volatile uint32_t *)&p->proto_version, val);
}

#endif /* QUICKJS_HANDLE_HELPERS_H */

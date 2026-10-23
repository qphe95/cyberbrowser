/*
 * QuickJS Internal Types for GC
 *
 * This header contains internal type definitions needed by
 * quickjs_gc_unified.c for garbage collection.
 * These definitions must match those in quickjs.c exactly.
 */
#ifndef QUICKJS_INTERNAL_H
#define QUICKJS_INTERNAL_H

#include "quickjs_types.h"
#include "quickjs_gc_unified.h"
#include "lockfree_hash_table.h"
#include "cutils.h"
#include "list.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct JSVarRef JSVarRef;
typedef struct JSProxyData JSProxyData;
typedef struct JSArrayBuffer JSArrayBuffer;
typedef struct JSTypedArray JSTypedArray;

/* Forward declaration for JSStackFrame */
struct JSStackFrame;

/* Forward declarations for inline functions defined in quickjs.h */
static inline JS_BOOL JS_IsUninitialized(GCValue v);
static inline JS_BOOL JS_IsObject(GCValue v);

/* Forward declarations for types defined in quickjs.h */
struct JSClassExoticMethods;
typedef struct JSPropertyEnum JSPropertyEnum;
typedef struct JSFunctionDef JSFunctionDef;

/*
 * JSVarRef - Variable reference structure
 * 
 * This structure references a JavaScript variable that may be:
 * 1. On the stack (in a JSStackFrame's var_buf or arg_buf)
 * 2. Detached (value stored internally in the 'value' field)
 * 
 * IMPORTANT: To support GC compaction, we don't store raw pointers to
 * stack-allocated GCValue. Instead, we store enough information to
 * compute the pointer dynamically:
 * - is_async_frame: distinguishes GC-managed frames (async/generators) from C-stack frames
 * - frame_ref: union containing either GCHandle (async) or raw pointer (sync)
 * - var_idx: index in var_buf or arg_buf
 * - is_arg: TRUE if in arg_buf, FALSE if in var_buf
 * 
 * Access to the value is ONLY through JSVarRefHandle methods - the raw pvalue()
 * pointer is private to prevent unsafe pointer storage.
 */
struct JSVarRef {
    /* Flags */
    uint8_t is_detached;      /* TRUE if variable is detached from stack */
    uint8_t is_lexical;       /* only used with global variables */
    uint8_t is_const;         /* only used with global variables */
    uint8_t is_async_frame;   /* TRUE if frame is GC-managed (async/generator) */
    
    /* Variable location (valid when !is_detached) */
    uint16_t var_idx;         /* index in var_buf or arg_buf */
    uint8_t is_arg;           /* TRUE if pointing to arg_buf, FALSE for var_buf */
    uint8_t _pad;             /* padding for alignment */
    
    /* 
     * Reference to the frame or the detached value.
     * When is_detached = TRUE: use 'value' field
     * When is_detached = FALSE && is_async_frame = TRUE: use 'frame_handle'
     * When is_detached = FALSE && is_async_frame = FALSE: use 'frame_ptr'
     */
    union {
        GCValue value;                    /* used when is_detached = TRUE */
        GCHandle frame_handle;            /* used when !is_detached && is_async_frame */
        JSStackFrame* frame_ptr;          /* used when !is_detached && !is_async_frame */
    } frame_ref;
    
    /* 
     * var_ref_idx is used when is_detached = FALSE to track position
     * in JSStackFrame.var_refs[] array. This allows cleanup when the
     * frame is destroyed.
     */
    uint16_t var_ref_idx;
};

#ifdef __cplusplus
}
#endif

/* ============================================================================
 * JSVarRefHandle - GC-safe access to JSVarRef
 * ============================================================================
 * JSVarRefHandle is a plain GCHandle typedef (see quickjs_types.h).  The old
 * C++ wrapper class dereferenced the handle on every access; the helpers below
 * keep that behaviour for the non-trivial accessors.  Plain fields are handled
 * with the SAFE_GET / SAFE_SET macros from quickjs_handles.h:
 *
 *   SAFE_GET(vr, JSVarRefHandle, is_detached)
 *   SAFE_SET(vr, JSVarRefHandle, is_lexical, 1)
 *   SAFE_SET_HANDLE(vr, JSVarRefHandle, frame_ref.frame_handle, fh)
 *   SAFE_GET(vr, JSVarRefHandle, frame_ref.frame_ptr)
 *
 * The raw value pointer must never be stored across a GC point.
 */
#define JS_VALUE_GET_VAR_REF_HANDLE(val) (GC_VALUE_GET_HANDLE(val))

static inline JSVarRef *js_varref_ptr(GCHandle handle)
{
    if (handle == GC_HANDLE_NULL)
        return NULL;
    return (JSVarRef *)gc_deref(handle);
}

/* Address of the variable's GCValue slot.  Valid only until the next GC. */
static inline GCValue *js_varref_pvalue(GCHandle handle)
{
    JSVarRef *p = js_varref_ptr(handle);
    JSStackFrame *sf;
    if (!p)
        return NULL;
    if (p->is_detached)
        return &p->frame_ref.value;
    if (p->is_async_frame)
        sf = js_frame_deref(p->frame_ref.frame_handle);
    else
        sf = p->frame_ref.frame_ptr;
    if (!sf)
        return NULL;
    if (p->is_arg) {
        GCValue *arg_buf = JS_SF_ARG_BUF(sf);
        return &arg_buf[p->var_idx];
    } else {
        GCValue *var_buf = JS_SF_VAR_BUF(sf);
        return &var_buf[p->var_idx];
    }
}

static inline GCValue js_varref_get_value(GCHandle handle)
{
    GCValue *pv = js_varref_pvalue(handle);
    return pv ? *pv : GC_UNDEFINED;
}

static inline void js_varref_set_value(GCHandle handle, GCValue val)
{
    GCValue *pv = js_varref_pvalue(handle);
    if (pv) {
        *pv = val;
        if (gc_ptr_in_heap(pv))
            gc_write_barrier_for_heap_slot(pv, GC_VALUE_GET_HANDLE(val));
    }
}

/* was get_value_ptr_for_set() */
static inline GCValue *js_varref_value_ptr(GCHandle handle)
{
    return js_varref_pvalue(handle);
}

static inline bool js_varref_is_value_uninitialized(GCHandle handle)
{
    GCValue *pv = js_varref_pvalue(handle);
    return pv ? JS_IsUninitialized(*pv) : false;
}

static inline bool js_varref_is_value_object(GCHandle handle)
{
    GCValue *pv = js_varref_pvalue(handle);
    return pv ? JS_IsObject(*pv) : false;
}

/* was copy_value_to(GCValue&) - C callers pass the destination by pointer */
static inline bool js_varref_copy_value_to(GCHandle handle, GCValue *dest)
{
    GCValue *pv = js_varref_pvalue(handle);
    if (pv) {
        GC_SET(*dest, *pv);
        return true;
    }
    return false;
}

static inline GCValue js_varref_get_detached_value(GCHandle handle)
{
    JSVarRef *p = js_varref_ptr(handle);
    return (p && p->is_detached) ? p->frame_ref.value : GC_UNDEFINED;
}

static inline void js_varref_set_detached_value(GCHandle handle, GCValue val)
{
    JSVarRef *p = js_varref_ptr(handle);
    if (p && p->is_detached) {
        p->frame_ref.value = val;
        gc_write_barrier_for_heap_slot(&p->frame_ref.value, GC_VALUE_GET_HANDLE(val));
    }
}

static inline void js_varref_detach(GCHandle handle)
{
    JSVarRef *p = js_varref_ptr(handle);
    GCValue *current;
    if (!p || p->is_detached)
        return;
    current = js_varref_pvalue(handle);
    if (current) {
        p->frame_ref.value = *current;
        gc_write_barrier_for_heap_slot(&p->frame_ref.value, GC_VALUE_GET_HANDLE(*current));
    } else {
        p->frame_ref.value = GC_UNDEFINED;
    }
    p->is_detached = TRUE;
}

static inline void js_varref_detach_with_value(GCHandle handle, GCValue val)
{
    JSVarRef *p = js_varref_ptr(handle);
    if (!p)
        return;
    p->frame_ref.value = val;
    gc_write_barrier_for_heap_slot(&p->frame_ref.value, GC_VALUE_GET_HANDLE(val));
    p->is_detached = TRUE;
}

static inline GCHandle js_varref_frame_handle(GCHandle handle)
{
    JSVarRef *p = js_varref_ptr(handle);
    if (!p || p->is_detached || !p->is_async_frame)
        return GC_HANDLE_NULL;
    return p->frame_ref.frame_handle;
}

static inline void js_varref_set_frame_handle(GCHandle handle, GCHandle val)
{
    JSVarRef *p = js_varref_ptr(handle);
    if (p && !p->is_detached) {
        p->frame_ref.frame_handle = val;
        gc_write_barrier_for_heap_slot(&p->frame_ref.frame_handle, val);
    }
}

static inline JSStackFrame *js_varref_frame_ptr(GCHandle handle)
{
    JSVarRef *p = js_varref_ptr(handle);
    if (!p || p->is_detached || p->is_async_frame)
        return NULL;
    return p->frame_ref.frame_ptr;
}

static inline void js_varref_set_frame_ptr(GCHandle handle, JSStackFrame *val)
{
    JSVarRef *p = js_varref_ptr(handle);
    if (p && !p->is_detached)
        p->frame_ref.frame_ptr = val;
}

/* Note that something outside the call chain refers to frame sf, so it is
 * not reused when its call returns.  Only for a real frame: some callers
 * pass a raw gc_deref() of an async function's state, which holds its frame
 * further in (see js_trigger_lazy_function), and a write there corrupts it. */
static inline bool js_frame_is_real(const JSStackFrame *sf)
{
    return sf && sf->self_handle != GC_HANDLE_NULL && js_frame_deref(sf->self_handle) == sf;
}

static inline void js_frame_mark_escaped(JSStackFrame *sf)
{
    if (js_frame_is_real(sf))
        sf->escaped |= JS_FRAME_PINNED;
}

/* A lazy closure's parent frame: its names are resolved when the closure is
 * first called, against this frame and, by name, the frames above it, so
 * all of them must outlive their calls. */
static inline void js_frame_pin_with_ancestors(JSStackFrame *sf)
{
    while (js_frame_is_real(sf) && !(sf->escaped & JS_FRAME_ANCESTORS)) {
        sf->escaped |= JS_FRAME_PINNED | JS_FRAME_ANCESTORS;
        sf = sf->prev_frame_handle != GC_HANDLE_NULL ? js_frame_deref(sf->prev_frame_handle) : NULL;
    }
}

static inline void js_varref_point_at(JSVarRef *p, JSStackFrame *ptr, uint8_t is_async, GCHandle frame_handle)
{
    p->is_async_frame = is_async;
    if (is_async)
        GC_SET(p->frame_ref.frame_handle, frame_handle);
    else
        p->frame_ref.frame_ptr = ptr;
}

/* A var_ref into frame `ptr` that is not in its var_refs array: nothing
 * detaches it, so the frame outlives its call.
 * (was set_frame_ref(ptr, async, frame_handle = GC_HANDLE_NULL)) */
static inline void js_varref_set_frame_ref(GCHandle handle, JSStackFrame *ptr,
                                           uint8_t is_async, GCHandle frame_handle)
{
    JSVarRef *p = js_varref_ptr(handle);
    if (!p)
        return;
    js_frame_mark_escaped(ptr);
    js_varref_point_at(p, ptr, is_async, frame_handle);
}

/* A var_ref the caller stores in frame `ptr`'s var_refs array: detached when
 * the call returns (js_frame_detach_var_refs), so the frame need not outlive
 * it. */
static inline void js_varref_set_frame_ref_in_array(GCHandle handle, JSStackFrame *ptr,
                                                    uint8_t is_async, GCHandle frame_handle)
{
    JSVarRef *p = js_varref_ptr(handle);
    if (!p)
        return;
    if (js_frame_is_real(ptr)) ptr->escaped |= JS_FRAME_VAR_REFS;
    js_varref_point_at(p, ptr, is_async, frame_handle);
}

/* was static from_array_handle() */
static inline JSVarRefHandle js_varref_from_array(GCHandle array_handle, size_t index)
{
    GCHandle *array;
    if (array_handle == GC_HANDLE_NULL)
        return GC_HANDLE_NULL;
    array = (GCHandle *)gc_deref(array_handle);
    if (!array)
        return GC_HANDLE_NULL;
    return array[index];
}

/* was static set_in_array_handle() */
static inline void js_varref_set_in_array(GCHandle array_handle, size_t index,
                                          JSVarRefHandle var_ref)
{
    GCHandle *array;
    if (array_handle == GC_HANDLE_NULL)
        return;
    array = (GCHandle *)gc_deref(array_handle);
    if (!array)
        return;
    array[index] = var_ref;
    gc_write_barrier_for_heap_slot(&array[index], var_ref);
}

/*
 * JSProxyData - Proxy data structure
 */
struct JSProxyData {
    GCValue target;
    GCValue handler;
    uint8_t is_func;
    uint8_t is_revoked;
};

/*
 * JSArrayBuffer - Array buffer structure
 */
struct JSArrayBuffer {
    int byte_length; /* 0 if detached */
    int max_byte_length; /* -1 if not resizable; >= byte_length otherwise */
    uint8_t detached;
    uint8_t shared; /* if shared, the array buffer cannot be detached */
    uint8_t *data; /* NULL if detached */
    GCHandle data_handle; /* handle to data buffer (GC_HANDLE_NULL if using external allocator) */
    /* Typed arrays referencing this buffer - list of handles for GC safety */
    GCHandleList typed_arrays;
    void *opaque;
    JSFreeArrayBufferDataFunc *free_func;
};

/*
 * JSTypedArray - Typed array structure
 */
struct JSTypedArray {
    /* Note: link to arraybuffer via JSArrayBuffer::typed_arrays handle array */
    GCHandle obj_handle; /* back pointer to the TypedArray/DataView object */
    GCHandle buffer_handle; /* based array buffer handle */
    uint32_t offset; /* byte offset in the array buffer */
    uint32_t length; /* byte length in the array buffer */
    BOOL track_rab; /* auto-track length of backing array buffer */
};

/*
 * ============================================================================
 * Extracted struct and enum definitions from quickjs.c
 * ============================================================================
 */

/* Enums */

typedef enum JSIteratorKindEnum {
    JS_ITERATOR_KIND_KEY,
    JS_ITERATOR_KIND_VALUE,
    JS_ITERATOR_KIND_KEY_AND_VALUE,
} JSIteratorKindEnum;

typedef enum JSGeneratorStateEnum {
    JS_GENERATOR_STATE_SUSPENDED_START,
    JS_GENERATOR_STATE_SUSPENDED_YIELD,
    JS_GENERATOR_STATE_SUSPENDED_YIELD_STAR,
    JS_GENERATOR_STATE_EXECUTING,
    JS_GENERATOR_STATE_COMPLETED,
} JSGeneratorStateEnum;

typedef enum JSAsyncGeneratorStateEnum {
    JS_ASYNC_GENERATOR_STATE_SUSPENDED_START,
    JS_ASYNC_GENERATOR_STATE_SUSPENDED_YIELD,
    JS_ASYNC_GENERATOR_STATE_SUSPENDED_YIELD_STAR,
    JS_ASYNC_GENERATOR_STATE_EXECUTING,
    JS_ASYNC_GENERATOR_STATE_AWAITING_RETURN,
    JS_ASYNC_GENERATOR_STATE_COMPLETED,
} JSAsyncGeneratorStateEnum;

typedef enum JSIteratorHelperKindEnum {
    JS_ITERATOR_HELPER_KIND_DROP,
    JS_ITERATOR_HELPER_KIND_EVERY,
    JS_ITERATOR_HELPER_KIND_FILTER,
    JS_ITERATOR_HELPER_KIND_FIND,
    JS_ITERATOR_HELPER_KIND_FLAT_MAP,
    JS_ITERATOR_HELPER_KIND_FOR_EACH,
    JS_ITERATOR_HELPER_KIND_MAP,
    JS_ITERATOR_HELPER_KIND_SOME,
    JS_ITERATOR_HELPER_KIND_TAKE,
} JSIteratorHelperKindEnum;

/* Structs */

typedef struct JSClass JSClass;
struct JSClass {
    uint32_t class_id; /* 0 means free entry */
    JSAtom class_name;
    JSClassFinalizer *finalizer;
    JSClassGCMark *gc_mark;
    JSClassCall *call;
    /* pointers for exotic behavior, can be NULL if none are present */
    const JSClassExoticMethods *exotic;
};

typedef struct JSObjectListEntry {
    GCHandle obj_handle; /* Handle to JSObjectHandle */
    uint32_t hash_next; /* -1 if no next entry */
} JSObjectListEntry;

typedef struct JSForInIterator {
    GCValue obj;
    uint32_t idx;
    uint32_t atom_count;
    uint8_t in_prototype_chain;
    uint8_t is_array;
    GCHandle tab_atom_handle; /* Handle to JSPropertyEnum array, GC_HANDLE_NULL if is_array = TRUE */
} JSForInIterator;

typedef struct JSArrayIteratorData {
    GCValue obj;
    JSIteratorKindEnum kind;
    uint32_t idx;
} JSArrayIteratorData;

typedef struct JSIteratorHelperData {
    GCValue obj;
    GCValue next;
    GCValue func; // predicate (filter) or mapper (flatMap, map)
    GCValue inner; // innerValue (flatMap)
    int64_t count; // limit (drop, take) or counter (filter, map, flatMap)
    JSIteratorHelperKindEnum kind : 8;
    uint8_t executing : 1;
    uint8_t done : 1;
} JSIteratorHelperData;

// note: deliberately doesn't use space-saving bit fields for
// |index|, |count| and |running| because tcc miscompiles them
typedef struct JSIteratorConcatData {
    int index, count;             // elements (not pairs!) in values[] array
    BOOL running;
    GCValue iter, next, values[]; // array of (object, method) pairs
} JSIteratorConcatData;

typedef struct JSIteratorWrapData {
    GCValue wrapped_iter;
    GCValue wrapped_next;
} JSIteratorWrapData;

typedef struct JSRegExpStringIteratorData {
    GCValue iterating_regexp;
    GCValue iterated_string;
    BOOL global;
    BOOL unicode;
    BOOL done;
} JSRegExpStringIteratorData;

/*
 * JSAsyncFromSyncIteratorData - Async-from-sync iterator data structure
 */
typedef struct JSAsyncFromSyncIteratorData {
    GCValue sync_iter;
    GCValue next_method;
} JSAsyncFromSyncIteratorData;

typedef struct JSMapRecord {
    int ref_count; /* used during enumeration to avoid freeing the record */
    BOOL empty : 8; /* TRUE if the record is deleted */
    GCListHead link;
    GCHandle self_handle; /* Handle to this record (set during allocation) */
    GCHandle hash_next_handle; /* Handle to next JSMapRecord in hash chain (GC_HANDLE_NULL if none) */
    GCValue key;
    GCValue value;
} JSMapRecord;

typedef struct JSMapState {
    BOOL is_weak; /* TRUE if WeakSet/WeakMap */
    GCListHead records; /* list of JSMapRecord.link */
    uint32_t record_count;
    GCHandle hash_table_handle; /* Handle to array of GCHandle (JSMapRecord handles) */
    int hash_bits;
    uint32_t hash_size; /* = 2 ^ hash_bits */
    uint32_t record_count_threshold; /* count at which a hash table
                                        resize is needed */
    /* NOTE: WeakMap/WeakSet use g_gc.weakmap_handles array, no header needed */
} JSMapState;

typedef struct JSMapIteratorData {
    GCValue obj;
    JSIteratorKindEnum kind;
    GCHandle cur_record_handle; /* Handle to current JSMapRecord, GC_HANDLE_NULL if none */
} JSMapIteratorData;

typedef struct JSWeakRefData {
    GCValue target;
    /* NOTE: Uses g_gc.weakref_handles array, no header needed */
} JSWeakRefData;

typedef struct JSFinalizationRegistryData {
    GCListHead entries; /* list of JSFinRecEntry.link */
    GCHandle realm_handle; /* Handle to JSContext realm */
    GCValue cb;
    /* NOTE: Uses g_gc.finrec_handles array, no header needed */
} JSFinalizationRegistryData;

typedef struct JSFinRecEntry {
    GCListHead link;
    GCValue target;
    GCValue held_val;
    GCValue token;
} JSFinRecEntry;

/* JSPromiseStateEnum is defined in quickjs_types.h */

typedef struct JSPromiseData {
    JSPromiseStateEnum promise_state;
    /* 0=fulfill, 1=reject, list of JSPromiseReactionData.link */
    GCListHead promise_reactions[2];
    BOOL is_handled; /* Note: only useful to debug */
    GCValue promise_result;
} JSPromiseData;

typedef struct JSPromiseFunctionData {
    GCValue promise;
    GCHandle presolved_handle; /* Handle to JSPromiseFunctionDataResolved, GC_HANDLE_NULL if not resolved */
} JSPromiseFunctionData;

typedef struct JSPromiseFunctionDataResolved {
    int ref_count;
    BOOL already_resolved;
    GCHandle self_handle; /* Handle to this struct (set during allocation) */
} JSPromiseFunctionDataResolved;

typedef struct JSPromiseReactionData {
    GCListHead link; /* not used in promise_reaction_job */
    GCValue resolving_funcs[2];
    GCValue handler;
} JSPromiseReactionData;

typedef struct JSAsyncGeneratorData {
    GCHandle generator_handle; /* back pointer to the async generator object */
    JSAsyncGeneratorStateEnum state;
    /* func_state_handle is GC_HANDLE_NULL in state AWAITING_RETURN and COMPLETED */
    GCHandle func_state_handle; /* Handle to JSAsyncFunctionStateHandle */
    GCListHead queue; /* list of JSAsyncGeneratorRequest.link */
} JSAsyncGeneratorData;

typedef struct JSAsyncGeneratorRequest {
    GCListHead link;
    /* completion */
    int completion_type; /* GEN_MAGIC_x */
    GCValue result;
    /* promise capability */
    GCValue promise;
    GCValue resolving_funcs[2];
} JSAsyncGeneratorRequest;

typedef struct JSGeneratorData {
    JSGeneratorStateEnum state;
    GCHandle func_state_handle; /* Handle to JSAsyncFunctionState, GC_HANDLE_NULL if completed */
} JSGeneratorData;

typedef struct JSCFunctionDataRecord {
    JSCFunctionData *func;
    uint8_t length;
    uint8_t data_len;
    uint16_t magic;
    GCValue data[0];
} JSCFunctionDataRecord;

typedef struct JSBoundFunction {
    GCValue func_obj;
    GCValue this_val;
    int argc;
    GCValue argv[0];
} JSBoundFunction;

/*
 * JSPropertyEnum - Property enumeration entry
 */
typedef struct JSPropertyEnum {
    JS_BOOL is_enumerable;
    JSAtom atom;
} JSPropertyEnum;

/*
 * JSProperty - Property descriptor structure
 */
struct JSProperty {
    union {
        GCValue value;      /* JS_PROP_NORMAL */
        struct {            /* JS_PROP_GETSET */
            GCHandle getter_handle; /* GC_HANDLE_NULL if undefined */
            GCHandle setter_handle; /* GC_HANDLE_NULL if undefined */
        } getset;
        GCHandle var_ref_handle;  /* JS_PROP_VARREF - handle to JSVarRef */
        struct {            /* JS_PROP_AUTOINIT */
            /* in order to use only 2 pointers, we compress the realm
               and the init function pointer */
            uintptr_t realm_and_id; /* realm and init_id (JS_AUTOINIT_ID_x)
                                       in the 2 low bits */
            GCHandle opaque_handle; /* Handle to opaque data, GC_HANDLE_NULL if none */
        } init;
    } u;
};

typedef struct JSVarDef {
    JSAtom var_name;
    /* index into fd->scopes of this variable lexical scope */
    int scope_level;
    /* - if scope_level = 0: scope in which the variable is defined
       - if scope_level != 0: index into fd->vars of the next
       variable in the same or enclosing lexical scope
    */
    int scope_next;
    uint8_t is_const : 1;
    uint8_t is_lexical : 1;
    uint8_t is_captured : 1; /* XXX: could remove and use a var_ref_idx value */
    uint8_t is_static_private : 1; /* only used during private class field parsing */
    uint8_t var_kind : 4; /* see JSVarKindEnum */
    /* if is_captured = TRUE, provides, the index of the corresponding
       JSVarRef on stack */
    uint16_t var_ref_idx;
    /* function pool index for lexical variables with var_kind =
       JS_VAR_FUNCTION_DECL/JS_VAR_NEW_FUNCTION_DECL or scope level of
       the definition of the 'var' variables (they have scope_level =
       0) */
    int func_pool_idx;
} JSVarDef;

/*
 * Note: JSAsyncFunctionState, JSJobEntry, JSModuleDef, and related types
 * are now defined in quickjs_types.h so they're available for the handle classes.
 */

/* Note: JSObject, JSShape, JSProperty, and JSShapeProperty are defined in quickjs.h */

#endif /* QUICKJS_INTERNAL_H */

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
 * C++ JSVarRefHandle Class - Safe GC Handle Wrapper
 * ============================================================================
 * 
 * This class wraps a GCHandle to provide safe access to JSVarRef instances.
 * The key feature is that it NEVER stores a raw JSVarRef pointer - instead,
 * it dereferences the handle on every property access to get a fresh pointer.
 * This ensures that the pointer is always valid even after GC compaction.
 * 
 * USAGE:
 *   JSVarRefHandle var_ref = JS_VALUE_GET_VAR_REF_HANDLE(val);
 *   GCValue val = var_ref.get_value();        // Read value safely
 *   var_ref.set_value(new_value);             // Write value safely
 *   bool init = var_ref.is_value_initialized(); // Check if initialized
 * 
 * IMPORTANT: Do NOT use JSVarRef* in new code. Always use JSVarRefHandle.
 * The raw pvalue() pointer is PRIVATE and should never be stored.
 */

class JSVarRefHandle {
private:
    GCHandle handle_;
    
    /* Get fresh pointer - called on every access */
    JSVarRef* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSVarRef*)gc_deref(handle_);
    }
    
    /* 
     * PRIVATE: Get pointer to the actual variable value
     * 
     * This dynamically computes the pointer based on the var_ref state.
     * DO NOT STORE this pointer - it may become invalid after GC!
     * Use get_value() and set_value() for safe access.
     */
    GCValue* pvalue() const {
        JSVarRef* p = get_ptr();
        if (!p) return nullptr;
        
        if (p->is_detached) {
            return &p->frame_ref.value;
        }
        
        JSStackFrame* sf;
        if (p->is_async_frame) {
            sf = (JSStackFrame*)gc_deref(p->frame_ref.frame_handle);
        } else {
            sf = p->frame_ref.frame_ptr;
        }
        
        if (!sf) return nullptr;
        
        if (p->is_arg) {
            GCValue* arg_buf = JS_SF_ARG_BUF(sf);
            return &arg_buf[p->var_idx];
        } else {
            GCValue* var_buf = JS_SF_VAR_BUF(sf);
            return &var_buf[p->var_idx];
        }
    }

public:
    /* Default constructor - null handle */
    JSVarRefHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit JSVarRefHandle(GCHandle handle) : handle_(handle) {}
    
    /* Copy constructor */
    JSVarRefHandle(const JSVarRefHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    JSVarRefHandle(JSVarRefHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    JSVarRefHandle& operator=(const JSVarRefHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    JSVarRefHandle& operator=(JSVarRefHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Assignment from nullptr */
    JSVarRefHandle& operator=(decltype(nullptr)) {
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
    
    /* =========================================================================
     * Property accessors - each dereferences handle to get fresh pointer
     * ========================================================================= */
    
    /* is_detached access */
    uint8_t is_detached() const {
        JSVarRef* p = get_ptr();
        return p ? p->is_detached : 0;
    }
    
    void set_is_detached(uint8_t val) {
        JSVarRef* p = get_ptr();
        if (p) p->is_detached = val;
    }
    
    /* is_lexical access */
    uint8_t is_lexical() const {
        JSVarRef* p = get_ptr();
        return p ? p->is_lexical : 0;
    }
    
    void set_is_lexical(uint8_t val) {
        JSVarRef* p = get_ptr();
        if (p) p->is_lexical = val;
    }
    
    /* is_const access */
    uint8_t is_const() const {
        JSVarRef* p = get_ptr();
        return p ? p->is_const : 0;
    }
    
    void set_is_const(uint8_t val) {
        JSVarRef* p = get_ptr();
        if (p) p->is_const = val;
    }
    
    /* is_async_frame access */
    uint8_t is_async_frame() const {
        JSVarRef* p = get_ptr();
        return p ? p->is_async_frame : 0;
    }
    
    void set_is_async_frame(uint8_t val) {
        JSVarRef* p = get_ptr();
        if (p) p->is_async_frame = val;
    }
    
    /* var_idx access (used when is_detached = FALSE) */
    uint16_t var_idx() const {
        JSVarRef* p = get_ptr();
        return p ? p->var_idx : 0;
    }
    
    void set_var_idx(uint16_t val) {
        JSVarRef* p = get_ptr();
        if (p) p->var_idx = val;
    }
    
    /* is_arg access (used when is_detached = FALSE) */
    uint8_t is_arg() const {
        JSVarRef* p = get_ptr();
        return p ? p->is_arg : 0;
    }
    
    void set_is_arg(uint8_t val) {
        JSVarRef* p = get_ptr();
        if (p) p->is_arg = val;
    }
    
    /* var_ref_idx access (used for cleanup in JSStackFrame.var_refs[]) */
    uint16_t var_ref_idx() const {
        JSVarRef* p = get_ptr();
        return p ? p->var_ref_idx : 0;
    }
    
    void set_var_ref_idx(uint16_t val) {
        JSVarRef* p = get_ptr();
        if (p) p->var_ref_idx = val;
    }
    
    /* =========================================================================
     * Frame reference accessors (used when is_detached = FALSE)
     * ========================================================================= */
    
    /* Get frame handle (for GC-managed frames) */
    GCHandle frame_handle() const {
        JSVarRef* p = get_ptr();
        if (!p || p->is_detached) return GC_HANDLE_NULL;
        return p->frame_ref.frame_handle;
    }
    
    void set_frame_handle(GCHandle val) {
        JSVarRef* p = get_ptr();
        if (p && !p->is_detached) p->frame_ref.frame_handle = val;
    }
    
    /* Get frame pointer (for sync frames) */
    JSStackFrame* frame_ptr() const {
        JSVarRef* p = get_ptr();
        if (!p || p->is_detached || p->is_async_frame) return nullptr;
        return p->frame_ref.frame_ptr;
    }
    
    void set_frame_ptr(JSStackFrame* val) {
        JSVarRef* p = get_ptr();
        if (p && !p->is_detached) p->frame_ref.frame_ptr = val;
    }
    
    /* Set frame reference (for use during initialization) */
    void set_frame_ref(JSStackFrame* ptr, uint8_t async, GCHandle frame_handle = GC_HANDLE_NULL) {
        JSVarRef* p = get_ptr();
        if (!p) return;
        
        p->is_async_frame = async;
        if (async) {
            p->frame_ref.frame_handle = frame_handle;
        } else {
            p->frame_ref.frame_ptr = ptr;
        }
    }
    
    /* =========================================================================
     * Safe Value Accessors - Use these instead of pvalue()
     * ========================================================================= */
    
    /* 
     * get_value() - Get the current value of the variable
     * 
     * This safely reads the value by computing the pointer dynamically.
     * Returns JS_UNDEFINED if the var_ref is invalid.
     */
    GCValue get_value() const {
        GCValue* pv = pvalue();
        return pv ? *pv : GC_UNDEFINED;
    }
    
    /*
     * set_value() - Set the value of the variable
     * 
     * This safely writes the value by computing the pointer dynamically.
     * Also handles proper value reference counting if needed.
     */
    void set_value(const GCValue& val) {
        GCValue* pv = pvalue();
        if (pv) *pv = val;
    }
    
    /*
     * get_value_ptr_for_set() - Get pointer for set_value() compatibility
     * 
     * This is for internal use with functions that take GCValue* but don't
     * store the pointer. The pointer must not be stored or used across GC points.
     * Returns nullptr if var_ref is invalid.
     */
    GCValue* get_value_ptr_for_set() const {
        return pvalue();
    }
    
    /*
     * is_value_uninitialized() - Check if the current value is uninitialized
     * 
     * This is a common check that avoids exposing the raw pointer.
     */
    bool is_value_uninitialized() const {
        GCValue* pv = pvalue();
        return pv ? JS_IsUninitialized(*pv) : false;
    }
    
    /*
     * is_value_object() - Check if the current value is an object
     */
    bool is_value_object() const {
        GCValue* pv = pvalue();
        return pv ? JS_IsObject(*pv) : false;
    }
    
    /*
     * copy_value_to() - Copy the value to an external GCValue
     * 
     * Useful when you need to store the value in a local variable.
     * Returns true if successful, false if var_ref is invalid.
     */
    bool copy_value_to(GCValue& dest) const {
        GCValue* pv = pvalue();
        if (pv) {
            dest = *pv;
            return true;
        }
        return false;
    }
    
    /* =========================================================================
     * Detached value accessors (for when is_detached = TRUE)
     * ========================================================================= */
    
    /* 
     * get_detached_value() - Get the detached value
     * Only valid when is_detached = TRUE.
     */
    GCValue get_detached_value() const {
        JSVarRef* p = get_ptr();
        return (p && p->is_detached) ? p->frame_ref.value : GC_UNDEFINED;
    }
    
    /*
     * set_detached_value() - Set the detached value
     * Only valid when is_detached = TRUE.
     */
    void set_detached_value(const GCValue& val) {
        JSVarRef* p = get_ptr();
        if (p && p->is_detached) p->frame_ref.value = val;
    }
    
    /* =========================================================================
     * Detachment operations
     * ========================================================================= */
    
    /* 
     * detach() - Detach the var_ref from its frame
     * 
     * This copies the current value from the frame into internal storage
     * and marks the var_ref as detached.
     */
    void detach() {
        JSVarRef* p = get_ptr();
        if (!p || p->is_detached) return;
        
        /* Copy value from frame to internal storage */
        GCValue* current = pvalue();
        if (current) {
            p->frame_ref.value = *current;
        } else {
            p->frame_ref.value = GC_UNDEFINED;
        }
        
        p->is_detached = TRUE;
    }
    
    /*
     * detach_with_value() - Detach with a specific value
     * 
     * Useful when you want to detach and set a new value at the same time.
     */
    void detach_with_value(const GCValue& val) {
        JSVarRef* p = get_ptr();
        if (!p) return;
        
        p->frame_ref.value = val;
        p->is_detached = TRUE;
    }
    
    /* =========================================================================
     * Static helpers for array access
     * ========================================================================= */
    
    /* Get a JSVarRefHandle from an array of handles at given index */
    static JSVarRefHandle from_array_handle(GCHandle array_handle, size_t index) {
        if (array_handle == GC_HANDLE_NULL) return JSVarRefHandle();
        GCHandle* array = (GCHandle*)gc_deref(array_handle);
        if (!array) return JSVarRefHandle();
        return JSVarRefHandle(array[index]);
    }
    
    /* Set a JSVarRefHandle in an array of handles at given index */
    static void set_in_array_handle(GCHandle array_handle, size_t index, JSVarRefHandle var_ref) {
        if (array_handle == GC_HANDLE_NULL) return;
        GCHandle* array = (GCHandle*)gc_deref(array_handle);
        if (!array) return;
        array[index] = var_ref.handle();
    }
    
    /* =========================================================================
     * Conversion operators for interoperability
     * ========================================================================= */
    
    /* Convert to GCHandle for handle-based APIs */
    operator GCHandle() const {
        return handle_;
    }
    
    /* Comparison operators */
    bool operator==(const JSVarRefHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSVarRefHandle& other) const {
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
 * Extracted struct and enum definitions from quickjs.cpp
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

/* JSPromiseStateEnum - Promise state enumeration */
typedef enum JSPromiseStateEnum {
    JS_PROMISE_PENDING,
    JS_PROMISE_FULFILLED,
    JS_PROMISE_REJECTED,
} JSPromiseStateEnum;

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

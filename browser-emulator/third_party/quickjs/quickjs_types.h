/*
 * QuickJS Type Definitions Header
 * 
 * This header consolidates type definitions shared between quickjs.h and quickjs_gc_unified.h
 * to prevent circular dependencies.
 */

#ifndef QUICKJS_TYPES_H
#define QUICKJS_TYPES_H

/* This header requires C++ compilation */
#ifndef __cplusplus
#error "quickjs_types.h requires C++ compilation"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "list.h"

/* ============================================================================
 * GC Handle Type (shared foundation)
 * ============================================================================ */

using GCHandle = uint32_t;

extern "C" {
/* External free function for malloc'd memory (defined in quickjs.cpp) */
void js_bc_free(void *ptr);

#define GC_HANDLE_NULL 0

/* ============================================================================
 * Forward declarations for internal structs
 * ============================================================================ */

struct JSRuntime;
struct JSContext;
struct JSString;
struct JSObject;
struct JSVarRef;
struct JSShape;
struct JSModuleDef;
struct JSFunctionBytecode;
struct JSJobEntry;
struct JSTypedArray;

} /* extern "C" */

/* ============================================================================
 * Forward declarations for C++ handle classes
 * ============================================================================ */

#ifdef __cplusplus

class JSContextHandle;
class JSRuntimeHandle;
class JSStringHandle;
class JSStringRopeHandle;
class JSVarRefHandle;
class JSBigIntHandle;
class JSModuleDefHandle;
class JSAsyncFunctionStateHandle;
class JSJobEntryHandle;
class JSFunctionBytecodeHandle;
class JSArrayBufferHandle;
class JSTypedArrayHandle;
class JSMapStateHandle;
class JSPromiseDataHandle;
class JSProxyDataHandle;
class JSBoundFunctionHandle;
class JSForInIteratorHandle;
class JSMapRecordHandle;
class JSMapIteratorDataHandle;
class JSArrayIteratorDataHandle;
class JSIteratorHelperDataHandle;
class JSIteratorConcatDataHandle;
class JSIteratorWrapDataHandle;
class JSRegExpStringIteratorDataHandle;
class JSPromiseFunctionDataHandle;
class JSPromiseFunctionDataResolvedHandle;
class JSCFunctionDataRecordHandle;
class JSGeneratorDataHandle;
class JSAsyncGeneratorDataHandle;

#endif /* __cplusplus */

/* Forward declaration for GCValue struct (defined below) */
struct GCValue;

#ifdef __cplusplus
extern "C" {
/* Forward declaration for gc_deref function */
void *gc_deref(uint32_t handle);
}
#endif

/* ============================================================================
 * Basic Types
 * ============================================================================ */

typedef uint32_t JSAtom;
typedef uint32_t JSClassID;

#ifndef BOOL
typedef int BOOL;
#define FALSE 0
#define TRUE 1
#endif

#ifndef JS_BOOL
#define JS_BOOL int
#endif

/* Value tagging system - reference types must have negative tags for handle-based storage */
enum {
    /* all tags with a reference count are negative */
    JS_TAG_FIRST       = -9, /* first negative tag */
    JS_TAG_BIG_INT     = -9,
    JS_TAG_SYMBOL      = -8,
    JS_TAG_STRING      = -7,
    JS_TAG_STRING_ROPE = -6,
    JS_TAG_MODULE      = -3, /* used internally */
    JS_TAG_FUNCTION_BYTECODE = -2, /* used internally */
    JS_TAG_OBJECT      = -1,

    JS_TAG_INT         = 0,
    JS_TAG_BOOL        = 1,
    JS_TAG_NULL        = 2,
    JS_TAG_UNDEFINED   = 3,
    JS_TAG_UNINITIALIZED = 4,
    JS_TAG_CATCH_OFFSET = 5,
    JS_TAG_EXCEPTION   = 6,
    JS_TAG_SHORT_BIG_INT = 7,
    JS_TAG_FLOAT64     = 8,
    /* any larger tag is FLOAT64 if JS_NAN_BOXING */
};

#define JS_FLOAT64_NAN_TAG  0x7ff80000

/* BigInt limb size - default to 64 bits on 64-bit platforms */
#ifndef JS_LIMB_BITS
#if defined(__SIZEOF_INT128__) && (INTPTR_MAX >= INT64_MAX)
#define JS_LIMB_BITS 64
#else
#define JS_LIMB_BITS 32
#endif
#endif

/* GC Phase Enumeration */
typedef enum JSGCPhaseEnum {
    JS_GC_PHASE_NONE,
    JS_GC_PHASE_MARK,
    JS_GC_PHASE_SWEEP,
    JS_GC_PHASE_DECREF,
    JS_GC_PHASE_REMOVE_CYCLES,
} JSGCPhaseEnum;

/* GC Object Type Enumeration */
typedef enum JSGCObjectTypeEnum {
    JS_GC_OBJ_TYPE_JS_OBJECT,
    JS_GC_OBJ_TYPE_FUNCTION_BYTECODE,
    JS_GC_OBJ_TYPE_SHAPE,
    JS_GC_OBJ_TYPE_VAR_REF,
    JS_GC_OBJ_TYPE_ASYNC_FUNCTION,
    JS_GC_OBJ_TYPE_JS_CONTEXT,
    JS_GC_OBJ_TYPE_JS_RUNTIME,
    JS_GC_OBJ_TYPE_STACK_FRAME,
    JS_GC_OBJ_TYPE_FUNCTION_DEF,
    JS_GC_OBJ_TYPE_AUTO,
    JS_GC_OBJ_TYPE_DATA,
    JS_GC_OBJ_TYPE_JS_STRING,
    JS_GC_OBJ_TYPE_JS_STRING_ROPE,
    JS_GC_OBJ_TYPE_JS_BIGINT,
    JS_GC_OBJ_TYPE_MODULE,
} JSGCObjectTypeEnum;

extern "C" {
/* Forward declaration for gc_alloc (defined in quickjs_gc_unified.cpp) */
GCHandle gc_alloc(size_t size, JSGCObjectTypeEnum gc_obj_type);
}

/* JSGCObjectTypeEnum must stay in sync with GCObjectBucketType in quickjs_gc_unified.h */
/* Make sure the values match */
#define JS_GC_OBJ_TYPE_JS_OBJECT_COMPAT JS_GC_OBJ_TYPE_JS_OBJECT

/* NOTE: JSWeakRefHeader removed - weak references now use typed handle arrays
 * in GCState (weakmap_handles, weakref_handles, finrec_handles) instead of
 * polymorphic headers. This eliminates type dispatch during GC sweep.
 */

/* bigint types */
#if JS_LIMB_BITS == 32

typedef int32_t js_slimb_t;
typedef uint32_t js_limb_t;
typedef int64_t js_sdlimb_t;
typedef uint64_t js_dlimb_t;

#define JS_LIMB_DIGITS 9

#else

typedef __int128 int128_t;
typedef unsigned __int128 uint128_t;
typedef int64_t js_slimb_t;
typedef uint64_t js_limb_t;
typedef int128_t js_sdlimb_t;
typedef uint128_t js_dlimb_t;

#define JS_LIMB_DIGITS 19

#endif

typedef struct JSBigInt {
    uint32_t len; /* number of limbs, >= 1 */
    js_limb_t tab[]; /* two's complement representation, always
                        normalized so that 'len' is the minimum
                        possible length >= 1 */
} JSBigInt;

/* this bigint structure can hold a 64 bit integer */
typedef struct {
    js_limb_t big_int_buf[sizeof(JSBigInt) / sizeof(js_limb_t)]; /* for JSBigInt */
    /* must come just after */
    js_limb_t tab[(64 + JS_LIMB_BITS - 1) / JS_LIMB_BITS];
} JSBigIntBuf;
    
/* AUTOINIT disabled - enum kept for compatibility but unused */
typedef enum {
    JS_AUTOINIT_ID_PROTOTYPE,
    JS_AUTOINIT_ID_MODULE_NS,
    JS_AUTOINIT_ID_PROP,
} JSAutoInitIDEnum;

/* Float64 union for bit manipulation */
typedef union JSFloat64Union {
    double d;
    uint64_t u64;
    uint32_t u32[2];
} JSFloat64Union;

/* Atom types */
enum {
    JS_ATOM_TYPE_STRING = 1,
    JS_ATOM_TYPE_GLOBAL_SYMBOL,
    JS_ATOM_TYPE_SYMBOL,
    JS_ATOM_TYPE_PRIVATE,
    JS_ATOM_TYPE_DEAD = 0xFF,  /* Atom is being freed, references are stale */
};

typedef enum {
    JS_ATOM_KIND_STRING,
    JS_ATOM_KIND_SYMBOL,
    JS_ATOM_KIND_PRIVATE,
} JSAtomKindEnum;

#define JS_ATOM_HASH_MASK  ((1 << 30) - 1)

/* ============================================================================
 * GCValue - unified value representation
 * ============================================================================ */

typedef union GCValueUnion {
    int32_t int32;
    double float64;
#if JS_SHORT_BIG_INT_BITS == 32
    int32_t short_big_int;
#else
    int64_t short_big_int;
#endif
    /* Handle storage for GC-managed reference types (tag < 0) */
    GCHandle handle;
} GCValueUnion;

typedef struct GCValue {
    GCValueUnion u;
    int64_t tag;
    
    /* Default constructor - explicitly initialize to undefined */
    GCValue() : tag(JS_TAG_UNDEFINED) { u.handle = GC_HANDLE_NULL; }
    
    /* Constructor from GCHandle */
    explicit GCValue(GCHandle h, int64_t t = JS_TAG_OBJECT) : tag(t) { u.handle = h; }
    
    /* Constructor for int32 */
    explicit GCValue(int32_t i, bool is_tag_int = true) : tag(is_tag_int ? JS_TAG_INT : JS_TAG_BOOL) { u.int32 = i; }
    
    /* Constructor for double */
    explicit GCValue(double d) : tag(JS_TAG_FLOAT64) { u.float64 = d; }
    
    /* Constructor for C-style initialization (GC_MKVAL macro compatibility) */
    GCValue(GCValueUnion union_val, int64_t t) : tag(t) { u = union_val; }
    
    /* Check value type */
    bool IsObject() const { return tag == JS_TAG_OBJECT; }
    bool IsInt32() const { return tag == JS_TAG_INT; }
    bool IsFloat64() const { return tag == JS_TAG_FLOAT64; }
    bool IsString() const { return tag == JS_TAG_STRING; }
    bool IsBool() const { return tag == JS_TAG_BOOL; }
    bool IsNull() const { return tag == JS_TAG_NULL; }
    bool IsUndefined() const { return tag == JS_TAG_UNDEFINED; }
    bool IsException() const { return tag == JS_TAG_EXCEPTION; }
    
    /* Getters */
    GCHandle GetHandle() const { return u.handle; }
    int32_t GetInt32() const { return u.int32; }
    double GetFloat64() const { return u.float64; }
    
    /* Check if handle is NULL */
    bool IsNullHandle() const { return u.handle == GC_HANDLE_NULL; }
} GCValue;

typedef const GCValue GCValueConst;

/* JSValueHandle is GCValue for value parameters in functions */
typedef GCValue JSValueHandle;

/* GCValue accessor macros */
#define GC_VALUE_GET_TAG(v) ((int)((v).tag))
#define GC_VALUE_GET_NORM_TAG(v) GC_VALUE_GET_TAG(v)
#define GC_VALUE_GET_INT(v) ((v).u.int32)
#define GC_VALUE_GET_BOOL(v) ((v).u.int32)
#define GC_VALUE_GET_FLOAT64(v) ((v).u.float64)
#define GC_VALUE_GET_HANDLE(v) ((v).u.handle)
#define JS_VALUE_GET_HANDLE(v) GC_VALUE_GET_HANDLE(v)
#define GC_VALUE_GET_SHORT_BIG_INT(v) GC_VALUE_GET_INT(v)

/* Special values - inline functions for C++ compatibility */
static inline GCValue GC_NULL_VAL() {
    GCValue v; v.tag = JS_TAG_NULL; v.u.handle = GC_HANDLE_NULL; return v;
}
static inline GCValue GC_UNDEFINED_VAL() {
    GCValue v; v.tag = JS_TAG_UNDEFINED; v.u.handle = GC_HANDLE_NULL; return v;
}
static inline GCValue GC_FALSE_VAL() {
    GCValue v; v.tag = JS_TAG_BOOL; v.u.int32 = 0; return v;
}
static inline GCValue GC_TRUE_VAL() {
    GCValue v; v.tag = JS_TAG_BOOL; v.u.int32 = 1; return v;
}
static inline GCValue GC_EXCEPTION_VAL() {
    GCValue v; v.tag = JS_TAG_EXCEPTION; v.u.handle = GC_HANDLE_NULL; return v;
}
static inline GCValue GC_UNINITIALIZED_VAL() {
    GCValue v; v.tag = JS_TAG_UNINITIALIZED; v.u.handle = GC_HANDLE_NULL; return v;
}

/* Backward compatibility macros */
#define GC_NULL       GC_NULL_VAL()
#define GC_UNDEFINED  GC_UNDEFINED_VAL()
#define GC_FALSE      GC_FALSE_VAL()
#define GC_TRUE       GC_TRUE_VAL()
#define GC_EXCEPTION  GC_EXCEPTION_VAL()
#define GC_UNINITIALIZED GC_UNINITIALIZED_VAL()

/* ============================================================================
 * GCValueHandle - Safe handle-based access to GCValue arrays
 * 
 * This class wraps a GCHandle to provide safe access to GCValue arrays.
 * It dereferences the handle on every access to ensure the pointer is valid.
 * NOTE: Defined AFTER GCValue struct so inline methods can use it.
 * ============================================================================ */
class GCValueHandle {
private:
    GCHandle handle_;
    
    /* Get fresh pointer - called on every access */
    GCValue* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (GCValue*)gc_deref(handle_);
    }

public:
    /* Default constructor - null handle */
    GCValueHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit GCValueHandle(GCHandle handle) : handle_(handle) {}
    
    /* Copy constructor */
    GCValueHandle(const GCValueHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    GCValueHandle(GCValueHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    GCValueHandle& operator=(const GCValueHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    GCValueHandle& operator=(GCHandle handle) {
        handle_ = handle;
        return *this;
    }
    
    /* Get the underlying handle */
    GCHandle handle() const { return handle_; }
    
    /* Check if handle is valid */
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    /* Explicit bool conversion */
    explicit operator bool() const { return valid(); }
    
    /* Array access - dereferences handle each time */
    GCValue& operator[](size_t index) {
        return get_ptr()[index];
    }
    
    const GCValue& operator[](size_t index) const {
        return get_ptr()[index];
    }
    
    /* Pointer-like access for iteration */
    GCValue* operator->() { return get_ptr(); }
    const GCValue* operator->() const { return get_ptr(); }
    
    /* Implicit conversion to GCValue* for compatibility */
    operator GCValue*() { return get_ptr(); }
    operator const GCValue*() const { return get_ptr(); }
};

/* ============================================================================
 * Error Codes
 * ============================================================================ */

typedef enum JSErrorEnum {
    JS_ERR_UNINITIALIZED = -1,
    JS_ERR_GENERIC = -2,
    JS_ERR_SYNTAX = -3,
    JS_ERR_REFERENCE = -4,
    JS_ERR_TYPE = -5,
    JS_ERR_RANGE = -6,
    JS_ERR_URI = -7,
    JS_ERR_INTERNAL = -8,
    JS_ERR_AGGREGATE = -9,
    JS_ERR_ALLOC = -10,
    JS_ERR_CTX_DESTROYED = -11,
    JS_ERR_VAR_REF = -12,
    JS_ERR_GLOBAL_VAR_NOT_FOUND = -13,
    JS_ERR_SHAPE_NOT_FOUND = -14,
    JS_ERR_ATOM_NOT_FOUND = -15,
    JS_ERR_ATOM_EXISTS = -16,
    JS_ERR_CLASS_DEF_NOT_FOUND = -17,
    JS_ERR_MODULE_NOT_FOUND = -18,
    JS_ERR_READ_ONLY = -19,
    JS_ERR_CANNOT_SET_PROPERTY = -20,
    JS_ERR_STACK_OVERFLOW = -21,
    JS_ERR_WORKER_NOT_RUNNING = -22,
    JS_ERR_PROMISE_NOT_HANDLED = -23,
    JS_ERR_ATOMIC_UPDATE = -24,
    JS_ERR_INVALID_CONVERSION = -25,
    JS_ERR_INVALID_FORMAT = -26,
    JS_ERR_INVALID_JSON = -27,
    JS_ERR_INVALID_URL = -28,
    JS_ERR_INVALID_STATE = -29,
    JS_ERR_INVALID_THIS = -30,
    JS_ERR_INVALID_ARGS = -31,
    JS_ERR_INVALID_ARITY = -32,
    JS_ERR_INVALID_CONTEXT = -33,
    JS_ERR_INVALID_RUNTIME = -34,
    JS_ERR_INVALID_THREAD = -35,
    JS_ERR_NO_SETTER = -36,
    JS_EVAL_ERROR = -37,
    JS_RANGE_ERROR = -38,
    JS_REFERENCE_ERROR = -39,
    JS_SYNTAX_ERROR = -40,
    JS_TYPE_ERROR = -41,
    JS_URI_ERROR = -42,
    JS_INTERNAL_ERROR = -43,
    JS_AGGREGATE_ERROR = -44,
} JSErrorEnum;

/* ============================================================================
 * Memory Management Structures
 * ============================================================================ */

typedef struct JSMallocState {
    size_t malloc_count;
    size_t malloc_size;
    size_t malloc_limit;
    size_t memory_used_size;
    void *opaqueref;
    void *(*malloc_f)(void *opaqueref, size_t size);
    void (*free_f)(void *opaqueref, void *ptr);
    void *(*realloc_f)(void *opaqueref, void *ptr, size_t size);
    void *(*calloc_f)(void *opaqueref, size_t count, size_t size);
    size_t (*malloc_usable_size_f)(const void *ptr);
    size_t (*memory_used_size_f)(void *opaqueref);
} JSMallocState;

typedef struct JSStackFrame {
    /* 
     * GC-SAFE FRAME STRUCTURE
     * All pointers to GC-managed objects use handles or offsets.
     * This ensures compaction safety.
     */
    
    /* Previous frame in call stack - GCHandle for GC safety */
    GCHandle prev_frame_handle;
    
    GCValue cur_func; /* current function, JS_UNDEFINED if the frame is detached */
    
    /* Bytecode handle for computing cur_pc (stored as raw handle to avoid include order issues) */
    GCHandle bytecode_handle;
    
    /* Offsets from frame base to buffers (within same GC allocation) */
    uint32_t arg_buf_offset;   /* offset to arguments */
    uint32_t var_buf_offset;   /* offset to variables */
    uint32_t var_refs_offset;  /* offset to var_refs array */
    uint32_t stack_buf_offset; /* offset to stack buffer end */
    
    /* PC as offset from bytecode base */
    uint32_t pc_offset;
    
    /* SP as offset from var_buf (0 = empty stack) */
    uint32_t sp_offset;
    
    int arg_count;
    int js_mode; /* not supported for C functions */
    int var_count;   /* number of variables */
    int stack_size;  /* max stack size */
    int var_ref_count; /* number of var_refs */
    
    /* Handle to this frame's GC allocation - needed for var_refs to reference */
    GCHandle self_handle;
} JSStackFrame;

/* ============================================================================
 * JSStackFrame Helper Macros (GC-safe accessors)
 * ============================================================================ */

/* Get pointer to frame base from handle */
#define JS_SF_BASE(sf_handle) ((JSStackFrame*)gc_deref(sf_handle))

/* Access buffers using offsets */
#define JS_SF_ARG_BUF(sf) ((GCValue*)((uint8_t*)(sf) + (sf)->arg_buf_offset))
#define JS_SF_VAR_BUF(sf) ((GCValue*)((uint8_t*)(sf) + (sf)->var_buf_offset))
/* var_refs is now an array of GCHandle (not JSVarRefHandle objects) for GC safety */
#define JS_SF_VAR_REFS(sf) ((GCHandle*)((uint8_t*)(sf) + (sf)->var_refs_offset))
#define JS_SF_STACK_BUF(sf) ((GCValue*)((uint8_t*)(sf) + (sf)->stack_buf_offset))

/* Access previous frame */
#define JS_SF_PREV(sf) ((sf)->prev_frame_handle != GC_HANDLE_NULL ? \
    (JSStackFrame*)gc_deref((sf)->prev_frame_handle) : NULL)

/* Forward declaration for bytecode buffer accessor (takes GCHandle) */
#ifdef __cplusplus
extern "C" {
#endif
const uint8_t *js_bytecode_get_buf_jsfc(GCHandle bc_handle);
#ifdef __cplusplus
}
#endif

/* Compute cur_pc from bytecode base + offset */
#define JS_SF_CUR_PC(sf) ((sf)->bytecode_handle != GC_HANDLE_NULL ? \
    (js_bytecode_get_buf_jsfc((sf)->bytecode_handle) + (sf)->pc_offset) : NULL)

/* Set pc_offset from absolute pc pointer */
#define JS_SF_SET_PC(sf, pc_ptr) do { \
    if ((sf)->bytecode_handle != GC_HANDLE_NULL) { \
        const uint8_t *base = js_bytecode_get_buf_jsfc((sf)->bytecode_handle); \
        (sf)->pc_offset = (uint32_t)((pc_ptr) - base); \
    } else { \
        (sf)->pc_offset = 0; \
    } \
} while(0)

/* Access cur_sp using offset */
#define JS_SF_CUR_SP(sf) ((sf)->sp_offset > 0 ? \
    JS_SF_VAR_BUF(sf) + (sf)->sp_offset : NULL)

/* Set sp_offset from absolute sp pointer */
#define JS_SF_SET_SP(sf, sp_ptr) do { \
    (sf)->sp_offset = (uint32_t)((sp_ptr) - JS_SF_VAR_BUF(sf)); \
} while(0)

/* SharedArrayBuffer functions */
typedef struct JSSharedArrayBufferFunctions {
    void *(*sab_alloc)(void *opaque, size_t size);
    void (*sab_free)(void *opaque, void *ptr);
    void (*sab_dup)(void *opaque, void *ptr);
    void *sab_opaque;
} JSSharedArrayBufferFunctions;

/* ============================================================================
 * Function Pointer Types
 * ============================================================================ */

/* C++ version using handle classes */
/* C function callable from JavaScript */
typedef GCValue JSCFunction(JSContextHandle ctx, JSValueHandle this_val, int argc, GCValue *argv);
typedef GCValue JSCFunctionMagic(JSContextHandle ctx, JSValueHandle this_val, int argc, GCValue *argv, int magic);
typedef GCValue JSCFunctionData(JSContextHandle ctx, JSValueHandle this_val, int argc, GCValue *argv, int magic, GCValue *func_data);

/* Promise rejection tracker */
typedef void JSHostPromiseRejectionTracker(JSContextHandle ctx, JSValueHandle promise,
                                          GCValue reason, BOOL is_handled, void *opaque);

/* Mark function for GC - receives GCHandle instead of pointer for safety */
typedef void JS_MarkFunc(JSRuntimeHandle rt, GCHandle handle);

/* Module loader functions */
typedef GCValue JSModuleNormalizeFunc(JSContextHandle ctx, const char *module_base_name, const char *module_name, void *opaque);
typedef JSModuleDefHandle JSModuleLoaderFunc(JSContextHandle ctx, const char *module_name, void *opaque);

/* Standard module loader with import assertions */
typedef JSModuleDefHandle JSModuleLoaderFunc2(JSContextHandle ctx, const char *module_name, const char *import_assertions_json, void *opaque);
typedef BOOL JSModuleCheckSupportedImportAttributes(JSContextHandle ctx, const char *import_attributes_json);

/* Array buffer free function */
typedef void JSFreeArrayBufferDataFunc(JSRuntimeHandle rt, void *opaque, void *ptr);

/* Job handler */
typedef GCValue JSJobFunc(JSContextHandle ctx, int argc, GCValue *argv);

/* C property getter/setter/iterator */
typedef GCValue JSGetterFunc(JSContextHandle ctx, JSValueHandle this_val);
typedef GCValue JSSetterFunc(JSContextHandle ctx, JSValueHandle this_val, GCValue val);
typedef GCValue JSIteratorFunc(JSContextHandle ctx, JSValueHandle this_val);
typedef GCValue JSIteratorNextFunc(JSContextHandle ctx, JSValueHandle this_val, JS_BOOL *pdone, JSValueHandle *pval);

/* Class finalizer, GC mark, and call */
typedef void JSClassFinalizer(JSRuntimeHandle rt, GCValue val);
typedef void JSClassGCMark(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func);
typedef GCValue JSClassCall(JSContextHandle ctx, GCValue func_obj, GCValue this_val,
                            int argc, GCValue *argv, int flags);

/* ============================================================================
 * Preprocessor Macros
 * ============================================================================ */

#ifndef JS_BOOL
#define JS_BOOL int
#endif

#ifndef JS_PTR64
#if INTPTR_MAX >= INT64_MAX
#define JS_PTR64
#endif
#endif

/* Force inline attribute */
#ifndef js_force_inline
#if defined(__GNUC__) || defined(__clang__)
#define js_force_inline inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define js_force_inline __forceinline
#else
#define js_force_inline inline
#endif
#endif

/* ============================================================================
 * Inline Helper Functions
 * ============================================================================ */

static inline GCValue GC_NewFloat64(double d)
{
    GCValue v;
    v.tag = JS_TAG_FLOAT64;
    v.u.float64 = d;
    return v;
}

static inline GCValue GC_NewInt32(int32_t val)
{
    GCValue v;
    v.tag = JS_TAG_INT;
    v.u.int32 = val;
    return v;
}

static inline GCValue GC_NewBool(JS_BOOL val)
{
    GCValue v;
    v.tag = JS_TAG_BOOL;
    v.u.int32 = val != 0;
    return v;
}

static inline GCValue GC_NewCatchOffset(int32_t val)
{
    GCValue v;
    v.tag = JS_TAG_CATCH_OFFSET;
    v.u.int32 = val;
    return v;
}

static inline GCValue GC_NewHandle(GCHandle h)
{
    GCValue v;
    v.tag = JS_TAG_OBJECT;
    v.u.handle = h;
    return v;
}

static inline GCValue GC_NewException()
{
    GCValue v;
    v.tag = JS_TAG_EXCEPTION;
    v.u.handle = GC_HANDLE_NULL;
    return v;
}

/* Forward declare GCHandleArray and GCDataArray templates for use in structs */
template<typename T> class GCHandleArray;
template<typename T> class GCDataArray;

/* Simple array of GCHandle values (for ordered lists of GC objects) */
class GCHandleList {
private:
    GCHandle *data_;
    int count_;
    int capacity_;

public:
    /* Default constructor - empty array */
    GCHandleList() : data_(nullptr), count_(0), capacity_(0) {}
    
    /* Destructor - DOES NOT free data by design */
    ~GCHandleList() = default;
    
    /* Access element at index */
    GCHandle& operator[](size_t index) { return data_[index]; }
    const GCHandle& operator[](size_t index) const { return data_[index]; }
    
    /* Get raw data pointer */
    GCHandle* data() { return data_; }
    const GCHandle* data() const { return data_; }
    
    /* Get element count */
    int count() const { return count_; }
    
    /* Get capacity */
    int capacity() const { return capacity_; }
    
    /* Check if array has data */
    bool valid() const { return data_ != nullptr; }
    explicit operator bool() const { return valid(); }
    bool empty() const { return count_ == 0; }
    
    /* Resize array, allocating new memory if needed */
    bool resize(int new_count) {
        if (new_count <= capacity_) {
            if (new_count > count_) {
                memset(data_ + count_, 0, sizeof(GCHandle) * (new_count - count_));
            }
            count_ = new_count;
            return true;
        }
        int new_cap = new_count > capacity_ * 3 / 2 ? new_count : capacity_ * 3 / 2;
        if (new_cap < 4) new_cap = 4;
        GCHandle *new_data = (GCHandle*)realloc(data_, sizeof(GCHandle) * new_cap);
        if (!new_data) return false;
        memset(new_data + count_, 0, sizeof(GCHandle) * (new_count - count_));
        data_ = new_data;
        capacity_ = new_cap;
        count_ = new_count;
        return true;
    }
    
    /* Add handle to end, growing array if needed */
    bool push(GCHandle handle) {
        if (!resize(count_ + 1))
            return false;
        data_[count_ - 1] = handle;
        return true;
    }
    
    /* Remove handle at index */
    void remove_at(int index) {
        if (index < 0 || index >= count_) return;
        for (int j = index; j < count_ - 1; j++) {
            data_[j] = data_[j + 1];
        }
        count_--;
    }
    
    /* Remove specific handle */
    void remove(GCHandle handle) {
        for (int i = 0; i < count_; i++) {
            if (data_[i] == handle) {
                remove_at(i);
                return;
            }
        }
    }
    
    /* Find handle index, returns -1 if not found */
    int find(GCHandle handle) const {
        for (int i = 0; i < count_; i++) {
            if (data_[i] == handle) return i;
        }
        return -1;
    }
    
    /* Clear count but keep allocated memory */
    void clear_count() { count_ = 0; }
    
    /* Free memory and reset */
    void clear_and_free() {
        if (data_) {
            js_bc_free(data_);
            data_ = nullptr;
        }
        count_ = 0;
        capacity_ = 0;
    }
    
    /* Iterator support */
    GCHandle* begin() { return data_; }
    GCHandle* end() { return data_ + count_; }
    const GCHandle* begin() const { return data_; }
    const GCHandle* end() const { return data_ + count_; }
};

/* ============================================================================
 * GCHandleRingBuffer - Circular buffer for efficient FIFO operations
 * ============================================================================
 * 
 * This class implements a ring buffer for O(1) push_back and pop_front operations.
 * When the buffer fills up, it grows by allocating new space and unwrapping the
 * ring buffer into the new linear space.
 */
class GCHandleRingBuffer {
private:
    GCHandle *data_;    /* Raw buffer */
    int head_;          /* Index of first element */
    int tail_;          /* Index where next element will be inserted */
    int count_;         /* Number of elements */
    int capacity_;      /* Total capacity */

    /* Advance head (for pop) */
    static inline int advance(int idx, int cap) { return (idx + 1) % cap; }
    
    /* Grow and unwrap the ring buffer into new linear space */
    bool grow_and_unwrap() {
        int new_cap = capacity_ < 4 ? 4 : capacity_ * 2;
        GCHandle *new_data = (GCHandle*)realloc(data_, sizeof(GCHandle) * new_cap);
        if (!new_data) return false;
        
        if (count_ > 0 && head_ >= tail_) {
            /* Buffer is wrapped: [head..capacity-1] [0..tail-1]
             * Unwrap to: [head..capacity-1] [0..count-(capacity-head)-1]
             * Move the wrapped portion to the end */
            int first_part = capacity_ - head_;
            int second_part = count_ - first_part;
            
            /* Move second part to after first part in new buffer */
            /* new_data already has first_part at [0..first_part-1] if head_==0,
             * but we need to shift to make room for second part */
            if (head_ != 0) {
                /* Shift first part to start of new buffer */
                for (int i = 0; i < first_part; i++) {
                    new_data[i] = new_data[head_ + i];
                }
            }
            /* Copy second part after first part */
            for (int i = 0; i < second_part; i++) {
                new_data[first_part + i] = data_[i];
            }
            head_ = 0;
            tail_ = count_;
        } else if (count_ > 0 && head_ != 0) {
            /* Not wrapped but head not at 0: shift to start */
            for (int i = 0; i < count_; i++) {
                new_data[i] = new_data[head_ + i];
            }
            head_ = 0;
            tail_ = count_;
        }
        /* Zero new space */
        if (new_cap > capacity_) {
            memset(new_data + tail_, 0, sizeof(GCHandle) * (new_cap - capacity_));
        }
        
        data_ = new_data;
        capacity_ = new_cap;
        return true;
    }

public:
    /* Default constructor - empty buffer */
    GCHandleRingBuffer() : data_(nullptr), head_(0), tail_(0), count_(0), capacity_(0) {}
    
    /* Destructor - DOES NOT free data by design (like GCHandleList) */
    ~GCHandleRingBuffer() = default;
    
    /* Get element count */
    int count() const { return count_; }
    
    /* Check if empty */
    bool empty() const { return count_ == 0; }
    
    /* Access element at logical index (0 = head/front) */
    GCHandle& operator[](size_t index) {
        return data_[(head_ + index) % capacity_];
    }
    const GCHandle& operator[](size_t index) const {
        return data_[(head_ + index) % capacity_];
    }
    
    /* Add handle to back (tail) - O(1) amortized */
    bool push_back(GCHandle handle) {
        if (count_ >= capacity_) {
            if (!grow_and_unwrap()) return false;
        }
        data_[tail_] = handle;
        tail_ = (tail_ + 1) % capacity_;
        count_++;
        return true;
    }
    
    /* Remove handle from front (head) - O(1) */
    void pop_front() {
        if (count_ == 0) return;
        data_[head_] = GC_HANDLE_NULL;  /* Clear for GC safety */
        head_ = (head_ + 1) % capacity_;
        count_--;
    }
    
    /* Get front element without removing */
    GCHandle front() const {
        return count_ > 0 ? data_[head_] : GC_HANDLE_NULL;
    }
    
    /* Get back element without removing */
    GCHandle back() const {
        if (count_ == 0) return GC_HANDLE_NULL;
        int back_idx = (tail_ - 1 + capacity_) % capacity_;
        return data_[back_idx];
    }
    
    /* Clear count but keep allocated memory */
    void clear_count() {
        if (data_ && count_ > 0) {
            /* Clear all elements for GC safety */
            for (int i = 0; i < count_; i++) {
                data_[(head_ + i) % capacity_] = GC_HANDLE_NULL;
            }
        }
        head_ = 0;
        tail_ = 0;
        count_ = 0;
    }
    
    /* Free memory and reset */
    void clear_and_free() {
        if (data_) {
            js_bc_free(data_);
            data_ = nullptr;
        }
        head_ = 0;
        tail_ = 0;
        count_ = 0;
        capacity_ = 0;
    }
};

/* ============================================================================
 * GCHandleArray<T> - Malloc'd array wrapper for structures containing GC refs
 * ============================================================================
 * 
 * IMPORTANT DESIGN NOTE:
 * This template wraps arrays that are allocated via malloc (not GC-managed) but
 * contain structures with GCHandle or GCValue fields. This is used for module
 * entry arrays (JSReqModuleEntry, JSExportEntry, etc.) where:
 * 
 * 1. The array itself is malloc'd for simplicity and performance
 * 2. The structures inside contain GCHandles that must be marked by GC
 * 3. The array is freed in the owner's finalizer (e.g., js_free_module_def)
 * 
 * GC MARKING REQUIREMENT:
 * The owner (e.g., JSModuleDef) MUST manually mark all GC references in these
 * arrays via its mark callback (e.g., js_mark_module_def). The GC does NOT
 * automatically scan malloc'd memory - only GC-managed memory.
 */
template<typename T>
class GCHandleArray {
private:
    T *data_;
    int count_;
    int capacity_;

public:
    /* Default constructor - empty array */
    GCHandleArray() : data_(nullptr), count_(0), capacity_(0) {}
    
    /* Destructor - DOES NOT free data by design */
    /* Caller must explicitly call free() or use clear() */
    ~GCHandleArray() = default;
    
    /* Access element at index */
    T& operator[](size_t index) { return data_[index]; }
    const T& operator[](size_t index) const { return data_[index]; }
    
    /* Get raw data pointer */
    T* data() { return data_; }
    const T* data() const { return data_; }
    
    /* Get element count */
    int count() const { return count_; }
    
    /* Get capacity */
    int capacity() const { return capacity_; }
    
    /* Check if array has data */
    bool valid() const { return data_ != nullptr; }
    explicit operator bool() const { return valid(); }
    
    /* Resize array, allocating new memory if needed */
    bool resize(int new_count) {
        if (new_count <= capacity_) {
            /* Zero new elements when growing within capacity */
            if (new_count > count_) {
                memset(data_ + count_, 0, sizeof(T) * (new_count - count_));
            }
            count_ = new_count;
            return true;
        }
        int new_cap = new_count > capacity_ * 3 / 2 ? new_count : capacity_ * 3 / 2;
        if (new_cap < 4) new_cap = 4;
        T *new_data = (T*)realloc(data_, sizeof(T) * new_cap);
        if (!new_data) return false;
        /* Zero the newly allocated elements (from old count to new count) */
        memset(new_data + count_, 0, sizeof(T) * (new_count - count_));
        data_ = new_data;
        capacity_ = new_cap;
        count_ = new_count;
        return true;
    }
    
    /* Add element, growing array if needed */
    bool push(const T& elem) {
        if (!resize(count_ + 1))
            return false;
        data_[count_ - 1] = elem;
        return true;
    }
    
    /* Get pointer to next slot for in-place construction */
    T* prepare_push() {
        if (!resize(count_ + 1))
            return nullptr;
        return &data_[count_ - 1];
    }
    
    /* Clear count but keep allocated memory */
    void clear_count() { count_ = 0; }
    
    /* Free memory and reset (named clear_and_free to avoid conflict with free() macro) */
    void clear_and_free() {
        if (data_) {
            js_bc_free(data_);
            data_ = nullptr;
        }
        count_ = 0;
        capacity_ = 0;
    }
    
    /* Reset with external allocation (used during deserialization) */
    void reset(T* data, int count, int capacity) {
        clear_and_free();
        data_ = data;
        count_ = count;
        capacity_ = capacity;
    }
    
    /* Iterator support */
    T* begin() { return data_; }
    T* end() { return data_ + count_; }
    const T* begin() const { return data_; }
    const T* end() const { return data_ + count_; }
};

/* ============================================================================
 * GCDataArray<T> - Unified GC heap array wrapper
 * ============================================================================
 * 
 * IMPORTANT DESIGN NOTE:
 * This template wraps arrays that are allocated on the unified GC heap (not
 * malloc'd). The array storage is accessed through a GCHandle, which is
 * dereferenced on every access. This ensures the array works correctly with
 * the compacting garbage collector - the handle remains valid even if the
 * GC moves the array in memory.
 * 
 * Usage in structs:
 *   GCDataArray<JSVarScope> scopes_handle;
 * 
 * The array data is GC-managed and will be freed by the GC when no longer
 * referenced. The struct containing the GCDataArray should be allocated
 * via gc_alloc() to ensure proper GC integration.
 */
template<typename T>
class GCDataArray {
private:
    GCHandle handle_;  /* Handle to GC-managed array storage */
    int count_;
    int capacity_;

public:
    /* Default constructor - empty array */
    GCDataArray() : handle_(GC_HANDLE_NULL), count_(0), capacity_(0) {}
    
    /* Destructor - DOES NOT free GC data by design */
    /* GC will reclaim the memory when the owning object is collected */
    ~GCDataArray() = default;
    
    /* Get the handle to the array storage */
    GCHandle handle() const { return handle_; }
    
    /* Get element count */
    int count() const { return count_; }
    
    /* Get capacity */
    int capacity() const { return capacity_; }
    
    /* Check if array has data */
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
    /* Comparison with GCHandle (for checking against GC_HANDLE_NULL) */
    bool operator==(GCHandle h) const { return handle_ == h; }
    bool operator!=(GCHandle h) const { return handle_ != h; }
    
    /* Assignment from GCHandle - for initialization with GC_HANDLE_NULL */
    GCDataArray& operator=(GCHandle h) {
        handle_ = h;
        count_ = 0;
        capacity_ = 0;
        return *this;
    }
    
    /* Dereference to get raw pointer - USE WITH CAUTION */
    /* The pointer is only valid until the next GC point */
    T* ptr() const { 
        return handle_ != GC_HANDLE_NULL ? (T*)gc_deref(handle_) : nullptr; 
    }
    
    /* Alias for ptr() - for compatibility with GCHandleArray API */
    T* data() { return ptr(); }
    const T* data() const { return ptr(); }
    
    /* Access element at index - dereferences handle each time for GC safety */
    T& operator[](size_t index) { 
        T* p = ptr();
        return p[index]; 
    }
    const T& operator[](size_t index) const { 
        T* p = ptr();
        return p[index]; 
    }
    
    /* Resize array, allocating new GC memory if needed */
    bool resize(int new_count) {
        if (new_count <= capacity_) {
            /* Zero new elements when growing within capacity */
            if (new_count > count_) {
                T* p = ptr();
                if (p) memset(p + count_, 0, sizeof(T) * (new_count - count_));
            }
            count_ = new_count;
            return true;
        }
        /* Need to grow - allocate new GC memory */
        int new_cap = new_count > capacity_ * 3 / 2 ? new_count : capacity_ * 3 / 2;
        if (new_cap < 4) new_cap = 4;
        
        /* Allocate new array on GC heap */
        GCHandle new_handle = gc_alloc(sizeof(T) * new_cap, JS_GC_OBJ_TYPE_DATA);
        if (new_handle == GC_HANDLE_NULL) return false;
        
        T* new_data = (T*)gc_deref(new_handle);
        if (!new_data) return false;
        
        /* Copy existing data */
        T* old_data = ptr();
        if (old_data && count_ > 0) {
            memcpy(new_data, old_data, sizeof(T) * count_);
        }
        
        /* Zero the newly allocated portion */
        memset(new_data + count_, 0, sizeof(T) * (new_count - count_));
        
        handle_ = new_handle;
        capacity_ = new_cap;
        count_ = new_count;
        return true;
    }
    
    /* Add element, growing array if needed */
    bool push(const T& elem) {
        if (!resize(count_ + 1))
            return false;
        T* p = ptr();
        if (p) p[count_ - 1] = elem;
        return true;
    }
    
    /* Get pointer to next slot for in-place construction */
    T* prepare_push() {
        if (!resize(count_ + 1))
            return nullptr;
        T* p = ptr();
        return p ? &p[count_ - 1] : nullptr;
    }
    
    /* Clear count but keep allocated memory */
    void clear_count() { count_ = 0; }
    
    /* Free memory and reset (named clear_and_free to avoid conflict with free() macro).
     * For GCDataArray, the memory is GC-managed so we just reset the handle.
     * The GC will reclaim the memory when no longer referenced. */
    void clear_and_free() {
        handle_ = GC_HANDLE_NULL;
        count_ = 0;
        capacity_ = 0;
    }
    
    /* Reset handle (used when the GC moves the array) */
    void set_handle(GCHandle h) { handle_ = h; }
    
    /* Iterator support - dereferences handle each time */
    class Iterator {
        GCDataArray* array_;
        int index_;
    public:
        Iterator(GCDataArray* arr, int idx) : array_(arr), index_(idx) {}
        T& operator*() { return (*array_)[index_]; }
        T* operator->() { 
            T* p = array_->ptr();
            return p ? &p[index_] : nullptr;
        }
        Iterator& operator++() { ++index_; return *this; }
        bool operator!=(const Iterator& other) const { return index_ != other.index_; }
    };
    
    Iterator begin() { return Iterator(this, 0); }
    Iterator end() { return Iterator(this, count_); }
};

/* Include atom cache header before JSContext definition */
#include "js_atom_cache.h"

/* ============================================================================
 * JSContext and JSRuntime struct definitions
 * ============================================================================ */

#define JS_NATIVE_ERROR_COUNT 8  /* EvalError, RangeError, ReferenceError, SyntaxError, TypeError, URIError, InternalError, AggregateError */

struct JSContext {
    GCHandle rt_handle;  /* Handle to JSRuntime - must use gc_deref to access */
    /* Note: contexts are tracked via GC type bucket, not intrusive list */

    uint16_t binary_object_count;
    int binary_object_size;
    
    GCHandle array_shape_handle;   /* handle to initial shape for Array objects */
    GCHandle arguments_shape_handle;  /* handle to shape for arguments objects */
    GCHandle mapped_arguments_shape_handle;  /* handle to shape for mapped arguments objects */
    GCHandle regexp_shape_handle;  /* handle to shape for regexp objects */
    GCHandle regexp_result_shape_handle;  /* handle to shape for regexp result objects */

    GCHandle class_proto_handle;  /* Handle to GCValue array */
    uint32_t class_proto_version; /* even = stable, odd = updating */
    uint32_t class_proto_lock;    /* spinlock for context proto array writes */
    GCValue function_proto;
    GCValue function_ctor;
    GCValue array_ctor;
    GCValue regexp_ctor;
    GCValue promise_ctor;
    GCValue native_error_proto[JS_NATIVE_ERROR_COUNT];
    GCValue iterator_ctor;
    GCValue async_iterator_proto;
    GCValue array_proto_values;
    GCValue throw_type_error;
    GCValue eval_obj;

    GCValue global_obj; /* global object - contains all globals including let/const */

    uint64_t random_state;

    /* Loaded modules - ordered list of handles (replaces list_head) */
    GCHandleList loaded_modules;

    /* if NULL, RegExp compilation is not supported */
    GCValue (*compile_regexp)(JSContextHandle ctx, GCValue pattern,
                              GCValue flags);
    /* if NULL, eval is not supported */
    GCValue (*eval_internal)(JSContextHandle ctx, GCValue this_obj,
                             const char *input, size_t input_len,
                             const char *filename, int flags, int scope_idx);
    void *user_opaque;
    
    /* Atom cache for faster identifier lookup during parsing */
    struct JSAtomCache atom_cache;
};

/* Forward declaration for the lock-free hash table used by the shape cache. */
typedef struct LFHashTable LFHashTable;

struct JSRuntime {
    JSMallocState malloc_state;
    const char *rt_info;

    /* Lock-free atom hash table: maps string hash to atom index for
       non-symbol atoms.  Symbol atoms are found via JSString.hash_next. */
    LFHashTable *atom_hash;
    LFHashTable *atom_hash_retired;
    uint32_t atom_hash_count; /* approximate number of non-symbol entries */

    int atom_count;   /* total number of atoms (treated atomically) */
    int atom_size;    /* allocated size of atom_array (treated atomically) */
    GCHandle atom_array_handle; /* Handle to GCHandle array - atomic load */
    int atom_free_index; /* 0 = none (treated atomically) */

    int class_count;    /* size of class_array (treated atomically) */
    GCHandle class_array_handle;  /* Handle to JSClass array (atomic load) */
    uint32_t class_array_lock;    /* spinlock for class registration */
    uint32_t class_array_version; /* even = stable, odd = updating */

    /* Job queue - ring buffer for O(1) push/pop operations */
    GCHandleRingBuffer job_queue;
    
    /* GC phase tracking */
    JSGCPhaseEnum gc_phase : 8;
    size_t malloc_gc_threshold;
    
    /* stack limitation */
    uintptr_t stack_size; /* in bytes, 0 if no limit */
    uintptr_t stack_top;
    uintptr_t stack_limit; /* lower stack limit */

    GCValue current_exception;
    /* true if the current exception cannot be catched */
    BOOL current_exception_is_uncatchable : 8;
    /* true if inside an out of memory error, to avoid recursing */
    BOOL in_out_of_memory : 8;
    
    /* 3-tier atom system: first permanent_atom_count atoms are never freed */
    uint32_t permanent_atom_count;
    GCHandle atom_gc_marks_handle;  /* Handle to mark bits array (GC_HANDLE_NULL if not needed) */

    GCHandle current_stack_frame_handle;  /* GC-safe handle to current stack frame */

    JSHostPromiseRejectionTracker *host_promise_rejection_tracker;
    void *host_promise_rejection_tracker_opaque;

    JSModuleNormalizeFunc *module_normalize_func;
    BOOL module_loader_has_attr;
    union {
        JSModuleLoaderFunc *module_loader_func;
        JSModuleLoaderFunc2 *module_loader_func2;
    } u;
    JSModuleCheckSupportedImportAttributes *module_check_attrs;
    void *module_loader_opaque;
    /* timestamp for internal use in module evaluation */
    int64_t module_async_evaluation_next_timestamp;

    BOOL can_block : 8; /* TRUE if Atomics.wait can block */
    /* used to allocate, free and clone SharedArrayBuffers */
    JSSharedArrayBufferFunctions sab_funcs;
    /* see JS_SetStripInfo() */
    uint8_t strip_flags;
    
    /* Lock-free shape hash table keyed by (proto_handle, property signature hash).
       Keys and values are GCHandle references to JSShape objects. */
    LFHashTable *shape_hash;
    /* Retired tables from lock-free resize.  Freed at runtime destruction after
       all readers are guaranteed gone. */
    LFHashTable *shape_hash_retired;
    uint32_t shape_hash_count; /* approximate atomic entry count */
    void *user_opaque;
    
    /* Instruction counter for GC triggering */
    uint32_t instruction_counter;
};

/* ============================================================================
 * Module Types - must be defined before handle classes
 * ============================================================================ */

/* Forward declarations for module types */
typedef struct JSReqModuleEntry JSReqModuleEntry;
typedef struct JSExportEntry JSExportEntry;
typedef struct JSStarExportEntry JSStarExportEntry;
typedef struct JSImportEntry JSImportEntry;

/* Module status enumeration */
typedef enum {
    JS_MODULE_STATUS_UNLINKED,
    JS_MODULE_STATUS_LINKING,
    JS_MODULE_STATUS_LINKED,
    JS_MODULE_STATUS_EVALUATING,
    JS_MODULE_STATUS_EVALUATING_ASYNC,
    JS_MODULE_STATUS_EVALUATED,
} JSModuleStatus;

/* Module initialization function type */
typedef int JSModuleInitFunc(JSContextHandle ctx, JSModuleDefHandle m);

typedef struct JSReqModuleEntry {
    JSAtom module_name;
    GCHandle module_handle; /* handle to module definition, GC_HANDLE_NULL before resolution */
    GCValue attributes; /* JS_UNDEFINED or an object contains the attributes as key/value */
} JSReqModuleEntry;

typedef enum JSExportTypeEnum {
    JS_EXPORT_TYPE_LOCAL,
    JS_EXPORT_TYPE_INDIRECT,
} JSExportTypeEnum;

typedef struct JSExportEntry {
    union {
        struct {
            int var_idx; /* closure variable index */
            GCHandle var_ref_handle; /* handle to variable reference, GC_HANDLE_NULL if not created */
        } local; /* for local export */
        int req_module_idx; /* module for indirect export */
    } u;
    JSExportTypeEnum export_type;
    JSAtom local_name; /* '*' if export ns from. not used for local export after compilation */
    JSAtom export_name; /* exported variable name */
} JSExportEntry;

typedef struct JSStarExportEntry {
    int req_module_idx; /* in req_module_entries */
} JSStarExportEntry;

typedef struct JSImportEntry {
    int var_idx; /* closure variable index */
    BOOL is_star; /* import_name = '*' is a valid import name, so need a flag */
    JSAtom import_name;
    int req_module_idx; /* in req_module_entries */
} JSImportEntry;

/* JSModuleDef structure - must be defined before JSModuleDefHandle */
struct JSModuleDef {
    JSAtom module_name;
    /* Note: modules are tracked via JSContext::loaded_modules, not intrusive list */

    /* 
     * Module entry arrays - IMPORTANT DESIGN NOTE:
     * These use GCDataArray which stores data in GC-managed memory.
     * This ensures the arrays are properly relocated during GC compaction.
     * The module's mark function must still manually mark all GC references.
     */
    GCDataArray<JSReqModuleEntry> req_module_entries;
    GCDataArray<JSExportEntry> export_entries;
    GCDataArray<JSStarExportEntry> star_export_entries;
    GCDataArray<JSImportEntry> import_entries;

    GCValue module_ns;
    GCValue func_obj; /* only used for JS modules */
    JSModuleInitFunc *init_func; /* only used for C modules */
    BOOL has_tla : 8; /* true if func_obj contains await */
    BOOL resolved : 8;
    BOOL func_created : 8;
    JSModuleStatus status : 8;
    /* temp use during js_module_link() & js_module_evaluate() */
    int dfs_index, dfs_ancestor_index;
    GCHandle stack_prev_handle; /* handle to previous module in stack during linking */
    /* temp use during js_module_evaluate() */
    GCHandle async_parent_modules_handle; /* handle to array of module handles */
    int async_parent_modules_count;
    int async_parent_modules_size;
    int pending_async_dependencies;
    BOOL async_evaluation; /* true: async_evaluation_timestamp corresponds to [[AsyncEvaluationOrder]] 
                              false: [[AsyncEvaluationOrder]] is UNSET or DONE */
    int64_t async_evaluation_timestamp;
    GCHandle cycle_root_handle; /* handle to cycle root module */
    GCValue promise; /* corresponds to spec field: capability */
    GCValue resolving_funcs[2]; /* corresponds to spec field: capability */

    /* true if evaluation yielded an exception. It is saved in
       eval_exception */
    BOOL eval_has_exception : 8;
    GCValue eval_exception;
    GCValue meta_obj; /* for import.meta */
    GCValue private_value; /* private value for C modules */
};

/* ============================================================================
 * Async Function and Job Types - must be defined before handle classes
 * ============================================================================ */

/* Job function type - must be defined before JSJobEntryHandle */
typedef GCValue JSJobFunc(JSContextHandle ctx, int argc, GCValue *argv);

typedef struct JSAsyncFunctionState {
    GCValue this_val; /* 'this' argument */
    int argc; /* number of function arguments */
    BOOL throw_flag; /* used to throw an exception in JS_CallInternal() */
    BOOL is_completed; /* TRUE if the function has returned. The stack
                          frame is no longer valid */
    GCValue resolving_funcs[2]; /* only used in JS async functions */
    GCHandle self_handle; /* handle to this async function state */
    JSStackFrame frame;
    /* arg_buf, var_buf, stack_buf and var_refs follow */
} JSAsyncFunctionState;

typedef struct JSJobEntry {
    /* Note: jobs are tracked via JSRuntime::job_queue, not intrusive list */
    GCHandle realm_handle; /* handle to JSContext realm */
    JSJobFunc *job_func;
    int argc;
    GCValue argv[0];
} JSJobEntry;

/* ============================================================================
 * Function Bytecode Types - must be defined before JSFunctionBytecodeHandle
 * ============================================================================ */

/* Lazy function parsing state */
typedef enum JSFunctionParseState {
    JS_FUNC_PARSED = 0,       /* Normal fully-parsed function */
    JS_FUNC_LAZY = 1,         /* Lazy function - body not yet parsed */
    JS_FUNC_PARSING = 2,      /* Currently being parsed (prevents re-entrancy) */
} JSFunctionParseState;

typedef enum JSFunctionKindEnum {
    JS_FUNC_NORMAL = 0,
    JS_FUNC_GENERATOR = (1 << 0),
    JS_FUNC_ASYNC = (1 << 1),
    JS_FUNC_ASYNC_GENERATOR = (JS_FUNC_GENERATOR | JS_FUNC_ASYNC),
} JSFunctionKindEnum;

typedef enum {
    /* XXX: add more variable kinds here instead of using bit fields */
    JS_VAR_NORMAL,
    JS_VAR_FUNCTION_DECL, /* lexical var with function declaration */
    JS_VAR_NEW_FUNCTION_DECL, /* lexical var with async/generator
                                 function declaration */
    JS_VAR_CATCH,
    JS_VAR_FUNCTION_NAME, /* function expression name */
    JS_VAR_PRIVATE_FIELD,
    JS_VAR_PRIVATE_METHOD,
    JS_VAR_PRIVATE_GETTER,
    JS_VAR_PRIVATE_SETTER, /* must come after JS_VAR_PRIVATE_GETTER */
    JS_VAR_PRIVATE_GETTER_SETTER, /* must come after JS_VAR_PRIVATE_SETTER */
    JS_VAR_GLOBAL_FUNCTION_DECL, /* global function definition, only in JSVarDef */
} JSVarKindEnum;

typedef enum {
    JS_CLOSURE_LOCAL, /* 'var_idx' is the index of a local variable in the parent function */
    JS_CLOSURE_ARG, /* 'var_idx' is the index of a argument variable in the parent function */
    JS_CLOSURE_REF, /* 'var_idx' is the index of a closure variable in the parent function */
    JS_CLOSURE_GLOBAL_REF, /* 'var_idx' in the index of a closure
                              variable in the parent function
                              referencing a global variable */
    JS_CLOSURE_GLOBAL_DECL, /* global variable declaration (eval code only) */
    JS_CLOSURE_GLOBAL, /* global variable (eval code only) */
    JS_CLOSURE_MODULE_DECL, /* definition of a module variable (eval code only) */
    JS_CLOSURE_MODULE_IMPORT, /* definition of a module import (eval code only) */ 
} JSClosureTypeEnum;

typedef struct JSClosureVar {
    uint8_t closure_type : 3;
    uint8_t is_lexical : 1; /* lexical variable */
    uint8_t is_const : 1; /* const variable (is_lexical = 1 if is_const = 1 */
    uint8_t var_kind : 4; /* see JSVarKindEnum */
    uint16_t var_idx; /* is_local = TRUE: index to a normal variable of the
                    parent function. otherwise: index to a closure
                    variable of the parent function */
    JSAtom var_name;
} JSClosureVar;

typedef struct JSBytecodeVarDef {
    JSAtom var_name;
    /* index into JSFunctionBytecode.vars of the next variable in the same or
       enclosing lexical scope
    */
    int scope_next; /* XXX: store on 16 bits */
    uint8_t is_const : 1;
    uint8_t is_lexical : 1;
    uint8_t is_captured : 1; /* XXX: could remove and use a var_ref_idx value */
    uint8_t has_scope: 1; /* true if JSVarDef.scope_level != 0 */
    uint8_t var_kind : 4; /* see JSVarKindEnum */
    /* If is_captured = TRUE, provides, the index of the corresponding
       JSVarRef on stack. It would be more compact to have a separate
       table with the corresponding inverted table but it requires
       more modifications in the code. */
    uint16_t var_ref_idx;
} JSBytecodeVarDef;

/* ============================================================================
 * Lazy Function Parsing State
 * Stores complete parser state for resuming parsing when a lazy function
 * is first called. This replaces the source-reconstruction approach.
 * ============================================================================ */
typedef struct JSLazyParseState {
    /* Source position - uses GC-managed source handle instead of raw pointer
     * This ensures the source stays alive as long as the lazy function exists
     */
    GCHandle source_handle;             /* Handle to GCString containing source */
    size_t source_offset;               /* Offset into source where function body starts */
    size_t source_len;                  /* Length of function body */
    size_t token_offset;                /* Offset of current token (relative to source_handle) */
    int token_val;                      /* Current token type */
    int line_num, col_num;              /* Position in source */
    
    /* Scope context */
    GCHandle parent_scope_handle;       /* Parent function scope */
    int parent_scope_level;             /* Parent's scope level at function definition point */
    GCHandle var_env_handle;            /* Variable environment */
    GCHandle lexical_env_handle;        /* Lexical environment */
    
    /* Function context */
    JSAtom func_name;                   /* Function name */
    uint8_t func_kind;                  /* JSFunctionKindEnum: normal/async/generator/etc */
    uint8_t func_type;                  /* JSParseFunctionEnum: statement/expr/arrow/method/etc */
    int arg_count;                      /* Number of arguments */
    uint8_t is_expr_body;               /* Arrow function expression body */
    
    /* Argument names for proper parameter resolution */
    GCHandle arg_names_handle;          /* Handle to array of JSAtom (parameter names) */
    
    /* Class context (for methods/constructors) */
    GCHandle class_handle;              /* Class object */
    GCHandle super_class_handle;        /* Super class */
    uint8_t is_static_method;           /* Static method flag */
    uint8_t is_derived_constructor;     /* Derived class constructor */
    
    /* Runtime context - parent frame handle for closure resolution */
    GCHandle parent_frame_handle;       /* Handle to parent stack frame at function definition time */
    
    /* Parser flags */
    uint8_t in_function_body;           /* Whether we're in function body */
    uint8_t in_class;                   /* Whether we're in a class */
    uint8_t in_strict_mode;             /* Strict mode flag */
    
} JSLazyParseState;

/* JSFunctionBytecode structure - must be defined before JSFunctionBytecodeHandle */
struct JSFunctionBytecode {
    uint8_t js_mode;
    uint8_t has_prototype : 1; /* true if a prototype field is necessary */
    uint8_t has_simple_parameter_list : 1;
    uint8_t is_derived_class_constructor : 1;
    /* true if home_object needs to be initialized */
    uint8_t need_home_object : 1;
    uint8_t func_kind : 2;
    uint8_t new_target_allowed : 1;
    uint8_t super_call_allowed : 1;
    uint8_t super_allowed : 1;
    uint8_t arguments_allowed : 1;
    uint8_t has_debug : 1;
    uint8_t read_only_bytecode : 1;
    uint8_t is_direct_or_indirect_eval : 1; /* used by JS_GetScriptOrModuleName() */
    /* XXX: 10 bits available */
    GCHandle byte_code_handle; /* Handle to bytecode buffer (GC-managed) */
    int byte_code_len;
    uint32_t byte_code_offset; /* offset from base of allocation to bytecode (for inline bytecode) */
    JSAtom func_name;
    uint32_t vardefs_offset; /* offset to JSBytecodeVarDef array (arguments + local variables) */
    uint32_t closure_var_offset; /* offset to JSClosureVar array (list of variables in the closure) */
    uint16_t arg_count;
    uint16_t var_count;
    uint16_t defined_arg_count; /* for length function property */
    uint16_t stack_size; /* maximum stack size */
    uint16_t var_ref_count; /* number of local variable references */
    GCHandle realm_handle; /* Handle to function realm context */
    uint32_t cpool_offset; /* offset to GCValue array (constant pool) */
    int cpool_count;
    int closure_var_count;
    struct {
        /* debug info, move to separate structure to save memory? */
        JSAtom filename;
        int source_len; 
        int pc2line_len;
        GCHandle pc2line_handle; /* Handle to pc2line buffer (GC-managed) */
        GCHandle source_handle; /* Handle to source string (GC-managed) */
    } debug;
    
    /* === Lazy function parsing fields === */
    uint8_t func_parse_state;  /* JSFunctionParseState: 0=PARSED, 1=LAZY, 2=PARSING */
    /* Source location for lazy parsing (valid when func_parse_state == JS_FUNC_LAZY) */
    const char *lazy_source;   /* Pointer to source code (in parent script memory) */
    uint32_t lazy_source_len;  /* Length of source code */
    uint32_t lazy_source_line; /* Starting line number */
    uint32_t lazy_source_col;  /* Starting column */
    GCHandle lazy_parent_scope; /* Handle to parent function definition for scope chain */
    uint8_t is_expr_body;      /* TRUE for arrow function with expression body (no braces) */
    /* Stored parameter names for lazy function reconstruction */
    GCHandle lazy_arg_names_handle; /* Handle to array of JSAtom (parameter names) */
    uint16_t lazy_arg_count;   /* Number of stored parameter names */
    
    /* === Resume parser state (Phase 1 implementation) === */
    /* Complete parser state for resuming parsing instead of source reconstruction */
    JSLazyParseState resume_parse_state;
    uint8_t has_resume_parse_state;  /* TRUE if resume_parse_state is valid */
};

/* ============================================================================
 * C++ Handle Class Wrappers
 * ============================================================================ */

/* Forward declarations of GC functions */
extern "C" void *gc_deref(uint32_t handle);
extern "C" bool gc_handle_is_valid(GCHandle handle);

#endif /* QUICKJS_TYPES_H */

/*
 * QuickJS Type Definitions Header
 * 
 * This header consolidates type definitions shared between quickjs.h and quickjs_gc_unified.h
 * to prevent circular dependencies.
 */

#ifndef QUICKJS_TYPES_H
#define QUICKJS_TYPES_H


#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "list.h"

/* ============================================================================
 * GC Handle Type (shared foundation)
 * ============================================================================ */

typedef uint32_t GCHandle;

#ifdef __cplusplus
extern "C" {
#endif
/* External free function for malloc'd memory (defined in quickjs.c) */
void js_bc_free(void *ptr);

#define GC_HANDLE_NULL 0

/* ============================================================================
 * Forward declarations for internal structs
 * ============================================================================ */

typedef struct JSRuntime JSRuntime;
typedef struct JSContext JSContext;
struct JSString;
struct JSObject;
struct JSVarRef;
struct JSShape;
struct JSModuleDef;
typedef struct JSFunctionBytecode JSFunctionBytecode;
struct JSJobEntry;
struct JSTypedArray;

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ============================================================================
 * GC handle types
 * ============================================================================
 * These used to be C++ wrapper classes that dereferenced a GCHandle on
 * every field access.  They are now plain handle typedefs; use the
 * SAFE_GET / SAFE_SET macros (quickjs_handles.h) to read/write fields.
 * ============================================================================ */

typedef GCHandle JSContextHandle;
typedef GCHandle JSRuntimeHandle;
typedef GCHandle JSStringHandle;
typedef GCHandle JSStringRopeHandle;
typedef GCHandle JSVarRefHandle;
typedef GCHandle JSBigIntHandle;
typedef GCHandle JSModuleDefHandle;
typedef GCHandle JSAsyncFunctionStateHandle;
typedef GCHandle JSJobEntryHandle;
typedef GCHandle JSFunctionBytecodeHandle;
typedef GCHandle JSArrayBufferHandle;
typedef GCHandle JSTypedArrayHandle;
typedef GCHandle JSMapStateHandle;
typedef GCHandle JSPromiseDataHandle;
typedef GCHandle JSProxyDataHandle;
typedef GCHandle JSBoundFunctionHandle;
typedef GCHandle JSForInIteratorHandle;
typedef GCHandle JSMapRecordHandle;
typedef GCHandle JSMapIteratorDataHandle;
typedef GCHandle JSArrayIteratorDataHandle;
typedef GCHandle JSIteratorHelperDataHandle;
typedef GCHandle JSIteratorConcatDataHandle;
typedef GCHandle JSIteratorWrapDataHandle;
typedef GCHandle JSRegExpStringIteratorDataHandle;
typedef GCHandle JSPromiseFunctionDataHandle;
typedef GCHandle JSPromiseFunctionDataResolvedHandle;
typedef GCHandle JSCFunctionDataRecordHandle;
typedef GCHandle JSGeneratorDataHandle;
typedef GCHandle JSAsyncGeneratorDataHandle;
typedef GCHandle JSAsyncFrameHandle;
typedef GCHandle JSVarDefHandle;
typedef struct JSVarDef *JSVarDefArrayHandle;   /* the compiler's, in plain memory */
typedef GCHandle JSAsyncFromSyncIteratorDataHandle;
typedef GCHandle JSFinalizationRegistryDataHandle;
typedef GCHandle JSWeakRefDataHandle;
typedef GCHandle JSPropertyEnumHandle;
typedef GCHandle GCStringHandle;
typedef GCHandle JSPropertyArrayHandle;
typedef GCHandle JSPropertyHandle;
typedef GCHandle JSPromiseReactionDataHandle;
typedef GCHandle JSFinRecEntryHandle;
typedef GCHandle JSAsyncGeneratorRequestHandle;
typedef GCHandle JSClassHandle;
typedef GCHandle JSObjectListEntryArrayHandle;
typedef GCHandle GCValueHandle;
typedef GCHandle JSShapeHandle;
typedef GCHandle JSObjectHandle;
typedef struct JSFunctionDef *JSFunctionDefHandle;  /* plain memory; private to quickjs.c */

/* Forward declaration for GCValue struct (defined below) */
struct GCValue;



/* ============================================================================
 * Basic Types
 * ============================================================================ */

typedef uint32_t JSAtom;
typedef uint32_t JSClassID;

typedef struct JSPropertyEnum JSPropertyEnum;
typedef struct JSClassExoticMethods JSClassExoticMethods;

/* JSPromiseStateEnum - Promise state enumeration */
typedef enum JSPromiseStateEnum {
    JS_PROMISE_PENDING,
    JS_PROMISE_FULFILLED,
    JS_PROMISE_REJECTED,
} JSPromiseStateEnum;

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
    JS_GC_OBJ_TYPE_JOB_ENTRY,   /* pending job: realm + arguments */
    JS_GC_OBJ_TYPE_BYTES,       /* raw bytes (ArrayBuffer storage, compiler and
                                   regexp buffers): no references, so stores into
                                   them need no write barrier */
} JSGCObjectTypeEnum;

#ifdef __cplusplus
extern "C" {
#endif
/* Forward declaration for gc_alloc (defined in quickjs_gc_unified.c) */
GCHandle gc_alloc(size_t size, JSGCObjectTypeEnum gc_obj_type);
#ifdef __cplusplus
}
#endif

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
    
} GCValue;

/* Constant initialiser for an undefined GCValue (usable for static storage).
 * Automatic variables can use GC_UNDEFINED instead. */
#define GC_UNDEFINED_INIT { { .handle = GC_HANDLE_NULL }, JS_TAG_UNDEFINED }

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

/* Special values - inline functions */
static inline GCValue GC_NULL_VAL(void) {
    GCValue v; v.tag = JS_TAG_NULL; v.u.handle = GC_HANDLE_NULL; return v;
}
static inline GCValue GC_UNDEFINED_VAL(void) {
    GCValue v; v.tag = JS_TAG_UNDEFINED; v.u.handle = GC_HANDLE_NULL; return v;
}
static inline GCValue GC_FALSE_VAL(void) {
    GCValue v; v.tag = JS_TAG_BOOL; v.u.int32 = 0; return v;
}
static inline GCValue GC_TRUE_VAL(void) {
    GCValue v; v.tag = JS_TAG_BOOL; v.u.int32 = 1; return v;
}
static inline GCValue GC_EXCEPTION_VAL(void) {
    GCValue v; v.tag = JS_TAG_EXCEPTION; v.u.handle = GC_HANDLE_NULL; return v;
}
static inline GCValue GC_UNINITIALIZED_VAL(void) {
    GCValue v; v.tag = JS_TAG_UNINITIALIZED; v.u.handle = GC_HANDLE_NULL; return v;
}

/* Backward compatibility macros */
#define GC_NULL       GC_NULL_VAL()
#define GC_UNDEFINED  GC_UNDEFINED_VAL()
#define GC_FALSE      GC_FALSE_VAL()
#define GC_TRUE       GC_TRUE_VAL()
#define GC_EXCEPTION  GC_EXCEPTION_VAL()
#define GC_UNINITIALIZED GC_UNINITIALIZED_VAL()

/* GCValueHandle is now a plain GCHandle typedef (see the handle types
 * block above).  Array element access goes through SAFE_AT. */

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

    /* var_refs grown after the frame was created (lazy parsing) live in a
     * separate array; GC_HANDLE_NULL when they are inline at var_refs_offset. */
    GCHandle var_refs_handle;

    /* What outside the call chain refers to the frame (JS_FRAME_*). */
    uint8_t escaped;
} JSStackFrame;

/* JSStackFrame.escaped.  PINNED: the frame outlives its call and is not
 * reused for the next one (a lazy closure's parent frame, which resolves
 * its names later, also through the frames above it; a var_ref outside
 * the frame's var_refs array; a suspended generator).  VAR_REFS: var_refs
 * in its array point into it; they are detached when the call returns,
 * after which the frame is free again.  ANCESTORS: the frames above it are
 * PINNED too. */
#define JS_FRAME_PINNED     1
#define JS_FRAME_VAR_REFS   2
#define JS_FRAME_ANCESTORS  4

/* ============================================================================
 * JSStackFrame Helper Macros (GC-safe accessors)
 * ============================================================================ */

/* Resolve a frame handle to its JSStackFrame.  A frame handle is either a
 * standalone JS_GC_OBJ_TYPE_STACK_FRAME allocation (the frame is the object)
 * or a generator/async function state (the frame is embedded in it). */
#ifdef __cplusplus
extern "C" {
#endif
struct JSStackFrame *js_frame_deref(GCHandle frame_handle);
#ifdef __cplusplus
}
#endif

/* Get pointer to frame base from handle */
#define JS_SF_BASE(sf_handle) js_frame_deref(sf_handle)

/* Access buffers using offsets */
#define JS_SF_ARG_BUF(sf) ((GCValue*)((uint8_t*)(sf) + (sf)->arg_buf_offset))
#define JS_SF_VAR_BUF(sf) ((GCValue*)((uint8_t*)(sf) + (sf)->var_buf_offset))
/* var_refs is now an array of GCHandle (not JSVarRefHandle objects) for GC safety */
#define JS_SF_VAR_REFS(sf) ((sf)->var_refs_handle != GC_HANDLE_NULL \
    ? (GCHandle *)gc_deref((sf)->var_refs_handle) \
    : (GCHandle *)((uint8_t *)(sf) + (sf)->var_refs_offset))
#define JS_SF_STACK_BUF(sf) ((GCValue*)((uint8_t*)(sf) + (sf)->stack_buf_offset))

/* Access previous frame */
#define JS_SF_PREV(sf) js_frame_deref((sf)->prev_frame_handle)

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

static inline GCValue GC_NewException(void)
{
    GCValue v;
    v.tag = JS_TAG_EXCEPTION;
    v.u.handle = GC_HANDLE_NULL;
    return v;
}

/* ============================================================================
 * GCHandleList - growable malloc'd array of GCHandle values
 * ============================================================================
 * Used for ordered lists of GC objects (loaded modules, typed arrays, child
 * function definitions, ...). The list does NOT free its storage on its own;
 * call gc_handle_list_clear_and_free() when the owner is finalized.
 * A zero-initialised GCHandleList is a valid empty list.
 */
typedef struct GCHandleList {
    GCHandle *data;
    int count;
    int capacity;
} GCHandleList;

/* Resize to new_count elements (new slots are zeroed). Returns false on OOM. */
static inline bool gc_handle_list_resize(GCHandleList *l, int new_count)
{
    if (new_count <= l->capacity) {
        if (new_count > l->count)
            memset(l->data + l->count, 0, sizeof(GCHandle) * (new_count - l->count));
        l->count = new_count;
        return true;
    }
    int new_cap = new_count > l->capacity * 3 / 2 ? new_count : l->capacity * 3 / 2;
    if (new_cap < 4) new_cap = 4;
    GCHandle *new_data = (GCHandle *)realloc(l->data, sizeof(GCHandle) * new_cap);
    if (!new_data) return false;
    memset(new_data + l->count, 0, sizeof(GCHandle) * (new_count - l->count));
    l->data = new_data;
    l->capacity = new_cap;
    l->count = new_count;
    return true;
}

/* Append a handle. Returns false on OOM. */
static inline bool gc_handle_list_push(GCHandleList *l, GCHandle handle)
{
    if (!gc_handle_list_resize(l, l->count + 1))
        return false;
    l->data[l->count - 1] = handle;  /* gc-no-dirty: malloc'd list */
    return true;
}

/* Remove the element at index (keeps order). */
static inline void gc_handle_list_remove_at(GCHandleList *l, int index)
{
    if (index < 0 || index >= l->count) return;
    for (int j = index; j < l->count - 1; j++)
        l->data[j] = l->data[j + 1];  /* gc-no-dirty: malloc'd list */
    l->count--;
}

/* Index of handle, or -1. */
static inline int gc_handle_list_find(const GCHandleList *l, GCHandle handle)
{
    for (int i = 0; i < l->count; i++) {
        if (l->data[i] == handle) return i;
    }
    return -1;
}

/* Remove the first occurrence of handle. */
static inline void gc_handle_list_remove(GCHandleList *l, GCHandle handle)
{
    gc_handle_list_remove_at(l, gc_handle_list_find(l, handle));
}

/* Free the storage and reset to empty. */
static inline void gc_handle_list_clear_and_free(GCHandleList *l)
{
    if (l->data) {
        js_bc_free(l->data);
        l->data = NULL;
    }
    l->count = 0;
    l->capacity = 0;
}

/* ============================================================================
 * JobBuffer - Flat array of pending JS job handles
 * ============================================================================
 *
 * Jobs are stored contiguously at indices [head, tail).  Indices below head
 * are dead space (already consumed).  Indices at or above tail are unused.
 * Buffers are linked into a short chain through `next`.  Each `next` pointer
 * holds a reference to the following buffer.  The JobQueue wrapper that points
 * to the chain is GC-managed.
 */
typedef struct JobBuffer {
    uint64_t                   capacity;      /* current size of jobs[] */
    volatile uint64_t          head;          /* next dequeue index */
    volatile uint64_t          tail;          /* next enqueue index */
    volatile uint32_t          refcount;      /* queue refs + in-flight readers/writers + next link */
    struct JobBuffer *volatile next;          /* next block in the chain, or NULL */
    GCHandle                   jobs[0];       /* flat array of capacity slots */
} JobBuffer;

/* ============================================================================
 * JobQueue - Lock-free MPMC job queue with chained blocks
 * ============================================================================
 *
 * The queue is a short linked list of large flat-array blocks.  Producers write
 * to tail_block; consumers read from head_block.  In steady state both pointers
 * refer to the same block.  When the tail block is full or half consumed, a new
 * larger block is allocated, linked after it, and published as tail_block.
 * Consumers drain head_block and then advance head_block to the next block.
 * An old block is freed once the queue and all in-flight readers have released
 * it.
 *
 * Jobs are never copied between blocks: each pending job lives in exactly one
 * block until it is consumed, so a job can never be executed twice.
 */
typedef struct JobQueue {
    JobBuffer *volatile head_block;  /* block consumers read from */
    JobBuffer *volatile tail_block;  /* block producers write to */
} JobQueue;

/* ============================================================================
 * GCDataArray - growable array stored on the unified GC heap
 * ============================================================================
 *
 * IMPORTANT DESIGN NOTE:
 * The array storage lives on the GC heap and is referred to by a GCHandle,
 * so it stays valid when the compacting GC moves it. Dereference the handle
 * at the point of use (GC_DATA_ARRAY_PTR / GC_DATA_ARRAY_AT); the resulting
 * pointer is only valid until the next GC point.
 *
 * The element type is not part of the struct, so the field declaration names
 * it in a comment and the accessors take it as a parameter:
 *
 *     GCDataArray scopes_handle;   // of JSVarScope
 *     JSVarScope *s = &GC_DATA_ARRAY_AT(fd->scopes_handle, JSVarScope, i);
 *     gc_data_array_resize(&fd->scopes_handle, sizeof(JSVarScope), n);
 *
 * The struct holding the GCDataArray should itself be GC allocated. The GC
 * reclaims the storage when it is no longer referenced. A zero-initialised
 * GCDataArray is a valid empty array.
 */
typedef struct GCDataArray {
    GCHandle handle;  /* handle to GC-managed array storage */
    int count;
    int capacity;
} GCDataArray;

/* Raw pointer to the storage (NULL if empty). Valid until the next GC point. */
#define GC_DATA_ARRAY_PTR(arr, T) \
    ((arr).handle != GC_HANDLE_NULL ? (T *)gc_deref((arr).handle) : (T *)NULL)
/* Element lvalue. */
#define GC_DATA_ARRAY_AT(arr, T, index) (GC_DATA_ARRAY_PTR(arr, T)[(index)])

/* Resize to new_count elements, reallocating on the GC heap when the capacity
 * is exceeded (new elements are zeroed). Returns false on OOM.
 * Defined in quickjs_gc_unified.c. */
bool gc_data_array_resize(GCDataArray *arr, size_t elem_size, int new_count);
bool gc_data_array_resize_bytes(GCDataArray *arr, size_t elem_size, int new_count);
/* Append one element slot and return a pointer to it (NULL on OOM). */
void *gc_data_array_prepare_push(GCDataArray *arr, size_t elem_size);
/* Append a copy of *elem. Returns false on OOM. */
bool gc_data_array_push(GCDataArray *arr, size_t elem_size, const void *elem);

/* Reset to empty. The GC reclaims the old storage. */
static inline void gc_data_array_clear_and_free(GCDataArray *arr)
{
    arr->handle = GC_HANDLE_NULL;
    arr->count = 0;
    arr->capacity = 0;
}

/* Include atom cache header before JSContext definition */
#include "js_atom_cache.h"

/* ============================================================================
 * JSContext and JSRuntime struct definitions
 * ============================================================================ */

#define JS_NATIVE_ERROR_COUNT 8  /* EvalError, RangeError, ReferenceError, SyntaxError, TypeError, URIError, InternalError, AggregateError */

struct JSContext {
    GCHandle rt_handle;  /* Handle to JSRuntime - must use gc_deref to access */
    uint32_t serial;     /* unique per context made (JS_GetContextSerial) */
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

/* Interrupt handler callback type. Return non-zero to abort execution. */
typedef int JSInterruptHandler(JSRuntimeHandle rt, void *opaque);

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

    /* Job queue - handle to lock-free compacting job queue (JobQueue) */
    GCHandle job_queue_handle;
    
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
    
    /* Instruction counter for interrupt / GC triggering */
    uint32_t instruction_counter;
    uint32_t instruction_counter_init;
    
    /* Optional interrupt handler: called when instruction_counter reaches 0.
     * If it returns non-zero, execution aborts with an exception. */
    JSInterruptHandler *interrupt_handler;
    void *interrupt_opaque;
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
    GCDataArray req_module_entries;   /* of JSReqModuleEntry */
    GCDataArray export_entries;   /* of JSExportEntry */
    GCDataArray star_export_entries;   /* of JSStarExportEntry */
    GCDataArray import_entries;   /* of JSImportEntry */

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

    /* The function definition's own flags, as computed by
     * js_parse_function_decl2 (arrow functions inherit new.target / super /
     * arguments permissions from their parent at that point). */
    uint8_t new_target_allowed;
    uint8_t super_call_allowed;
    uint8_t super_allowed;
    uint8_t arguments_allowed;
    uint8_t has_prototype;
    uint8_t has_home_object;
    uint8_t has_arguments_binding;
    uint8_t has_this_binding;
    uint8_t is_func_expr;

    /* Whole function text (for Function.prototype.toString), as an offset
     * into source_handle. */
    uint32_t func_source_offset;
    uint32_t func_source_len;

    /* Bytecode produced by the first resume; later closures created from
     * the same stub reuse it.  Its closure variables index the stub's
     * closure variables (see js_resume_lazy_parse). */
    GCHandle compiled_handle;

    /* length of the root source (gc_string_length() would be a strlen) */
    size_t source_total_len;

    /* The root source's structure index (js_struct_index.h), or NULL: it
     * lives as long as the runtime. */
    const struct JSStructIndex *struct_index;
    /* The body's compile unit (quickjs.c, LazyUnit), or NULL: plain memory. */
    struct LazyUnit *unit;
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
    /* Baseline JIT (quickjs_jit.c): hotness, and the compiled code (NULL:
     * none yet, JIT_CODE_FAILED: cannot be compiled).  Before debug, which
     * bytecode stripped of debug information leaves out.  A bytecode object
     * starts zeroed, so a set jit_code is always its own. */
    int32_t jit_budget;
    struct JitCode *jit_code;
    struct {
        /* debug info, move to separate structure to save memory? */
        JSAtom filename;
        int source_len; 
        uint32_t source_offset; /* offset of the function text in source_handle */
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

/* gc_deref and gc_handle_is_valid are declared in quickjs_gc_unified.h. */

#endif /* QUICKJS_TYPES_H */

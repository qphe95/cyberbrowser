/*
 * QuickJS Javascript Engine
 *
 * Copyright (c) 2017-2021 Fabrice Bellard
 * Copyright (c) 2017-2021 Charlie Gordon
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef QUICKJS_H
#define QUICKJS_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Include ALL core types first (GCHandle, GCValue, JSContextHandle, JSRuntimeHandle, structs, etc.) */
#include "quickjs_types.h"

/* Include unified GC for allocation functions */
#include "quickjs_gc_unified.h"

/* Include handle class definitions after GC functions are declared */
#ifdef __cplusplus
#include "quickjs_handle_classes.h"
#endif

/* Default version if not defined at compile time */
#ifndef CONFIG_VERSION
#define CONFIG_VERSION "unknown"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define js_likely(x)          __builtin_expect(!!(x), 1)
#define js_unlikely(x)        __builtin_expect(!!(x), 0)
#define js_force_inline       inline __attribute__((always_inline))
#define __js_printf_like(f, a)   __attribute__((format(printf, f, a)))
#else
#define js_likely(x)     (x)
#define js_unlikely(x)   (x)
#define js_force_inline  inline
#define __js_printf_like(a, b)
#endif

/* Note: JS_BOOL, JSRuntime, JSContext, JSClass, JSClassID, JSAtom, JS_TAG_* 
 * are all defined in quickjs_types.h */

#if INTPTR_MAX >= INT64_MAX
#define JS_PTR64
#define JS_PTR64_DEF(a) a
#else
#define JS_PTR64_DEF(a)
#endif

#ifndef JS_PTR64
#define JS_NAN_BOXING
#endif

#if defined(__SIZEOF_INT128__) && (INTPTR_MAX >= INT64_MAX)
#define JS_LIMB_BITS 64
#else
#define JS_LIMB_BITS 32
#endif

#define JS_SHORT_BIG_INT_BITS JS_LIMB_BITS
    
/* JS_TAG_* values are defined in quickjs_types.h */

/* Note: GCHeader is defined in quickjs_gc_unified.h */
struct GCHeader;

#define JS_FLOAT64_NAN NAN

#ifdef CONFIG_CHECK_JSVALUE
/* GCValue consistency checking mode */
typedef struct __GCValue *GCValue;
typedef const struct __GCValue *GCValueConst;

#define GC_VALUE_GET_TAG(v) (int)((uintptr_t)(v) & 0xf)
#define GC_VALUE_GET_NORM_TAG(v) GC_VALUE_GET_TAG(v)
#define GC_VALUE_GET_INT(v) (int)((intptr_t)(v) >> 4)
#define GC_VALUE_GET_BOOL(v) GC_VALUE_GET_INT(v)
#define GC_VALUE_GET_FLOAT64(v) (double)GC_VALUE_GET_INT(v)
#define GC_VALUE_GET_SHORT_BIG_INT(v) GC_VALUE_GET_INT(v)

/* GC_VALUE_GET_PTR for CONFIG_CHECK_JSVALUE - needs handle indirection */
static inline void *gc_value_get_ptr_check(GCValue v) {
    unsigned int tag = (unsigned int)((uintptr_t)(v) & 0xf);
    if (tag >= 0x7) {  /* Reference types */
        GCHandle handle = (GCHandle)((uintptr_t)(v) >> 4);
        if (handle == 0) {
            extern void __android_log_print(int prio, const char *tag, const char *fmt, ...);
            __android_log_print(6 /* ANDROID_LOG_ERROR */, "quickjs",
                "GC_VALUE_GET_PTR: handle=0 for v=%p tag=%u", (void*)v, tag);
            return NULL;
        }
        void *ptr = gc_deref(handle);
        if (!ptr) {
            extern void __android_log_print(int prio, const char *tag, const char *fmt, ...);
            __android_log_print(6 /* ANDROID_LOG_ERROR */, "quickjs",
                "GC_VALUE_GET_PTR: gc_deref returned NULL for handle=%u v=%p", handle, (void*)v);
        }
        return ptr;
    }
    return (void *)((intptr_t)(v) & ~0xf);
}
#define GC_VALUE_GET_PTR(v) gc_value_get_ptr_check(v)

#define GC_MKVAL(tag, val) (GCValue)(intptr_t)(((val) << 4) | (tag))

/* NOTE: GC_MKPTR and related functions removed - use GC_MKHANDLE with explicit handles instead */

#define GC_TAG_IS_FLOAT64(tag) ((unsigned)(tag) == JS_TAG_FLOAT64)

#define GC_NAN GC_MKVAL(JS_TAG_FLOAT64, 1)

static inline GCValue GC_NewFloat64ConfigCheck(double d)
{
    return GC_MKVAL(JS_TAG_FLOAT64, (int)d);
}

static inline JS_BOOL GC_VALUE_IS_NAN_CHECK(GCValue v)
{
    return 0;
}

static inline GCValue GC_NewShortBigIntConfigCheck(int32_t d)
{
    return GC_MKVAL(JS_TAG_SHORT_BIG_INT, d);
}

#elif defined(JS_NAN_BOXING)

/* NaN boxing implementation for GCValue */
typedef uint64_t GCValue;
typedef uint64_t GCValueConst;

#define GC_VALUE_GET_TAG(v) (int)((v) >> 32)
#define GC_VALUE_GET_NORM_TAG(v) GC_VALUE_GET_TAG(v)
#define GC_VALUE_GET_INT(v) (int)(v)
#define GC_VALUE_GET_BOOL(v) (int)(v)
#define GC_VALUE_GET_SHORT_BIG_INT(v) (int)(v)

/* For NaN boxing, we use the lower 32 bits to store handle for reference types */
static inline void *gc_value_get_ptr_nan(GCValue v) {
    int tag = (int)(v >> 32);
    GCHandle handle = (GCHandle)(v & 0xFFFFFFFF);
    if (tag < 0) {
        void *ptr = gc_deref(handle);
        if (!ptr) {
            extern void __android_log_print(int prio, const char *tag, const char *fmt, ...);
            __android_log_print(ANDROID_LOG_ERROR, "quickjs", "gc_value_get_ptr_nan: handle=%u returned NULL (v=0x%llx)", 
                               handle, (unsigned long long)v);
        }
        return ptr;
    }
    return (void *)(intptr_t)(v & 0xFFFFFFFF);
}
#define GC_VALUE_GET_PTR(v) gc_value_get_ptr_nan(v)

#define GC_MKVAL_NAN(tag, val) (((uint64_t)(tag) << 32) | (uint32_t)(val))

/* NOTE: GC_MKPTR_NAN and gc_mkptr_nan removed - use GC_MKHANDLE with explicit handles instead */

#define GC_FLOAT64_TAG_ADDEND (0x7ff80000 - JS_TAG_FIRST + 1) /* quiet NaN encoding */

static inline double GC_VALUE_GET_FLOAT64_NAN(GCValue v)
{
    union {
        GCValue v;
        double d;
    } u;
    u.v = v;
    u.v += (uint64_t)GC_FLOAT64_TAG_ADDEND << 32;
    return u.d;
}

#define GC_NAN_NAN (0x7ff8000000000000 - ((uint64_t)GC_FLOAT64_TAG_ADDEND << 32))

static inline GCValue GC_NewFloat64Nan(double d)
{
    union {
        double d;
        uint64_t u64;
    } u;
    GCValue v;
    u.d = d;
    if (js_unlikely((u.u64 & 0x7fffffffffffffff) > 0x7ff0000000000000))
        v = GC_NAN_NAN;
    else
        v = u.u64 - ((uint64_t)GC_FLOAT64_TAG_ADDEND << 32);
    return v;
}

/* same as GC_VALUE_GET_TAG, but return JS_TAG_FLOAT64 with NaN boxing */
static inline int GC_VALUE_GET_NORM_TAG_NAN(GCValue v)
{
    uint32_t tag;
    tag = GC_VALUE_GET_TAG(v);
    if (GC_TAG_IS_FLOAT64(tag))
        return JS_TAG_FLOAT64;
    else
        return tag;
}

static inline JS_BOOL GC_VALUE_IS_NAN_NAN(GCValue v)
{
    uint32_t tag;
    tag = GC_VALUE_GET_TAG(v);
    return tag == (GC_NAN_NAN >> 32);
}

static inline GCValue GC_NewShortBigIntNan(int32_t d)
{
    return GC_MKVAL_NAN(JS_TAG_SHORT_BIG_INT, d);
}

#else /* !JS_NAN_BOXING */

/* ============================================================================
 * GCValue - GC-safe value type using GCHandle
 * ============================================================================
 * 
 * GCValue is the replacement for the old GCValue which stored raw pointers.
 * The problem with raw pointers is that the GC can compact memory, moving
 * objects and invalidating pointers stored in C variables.
 * 
 * GCValue stores a GCHandle (an index into the handle table) for reference
 * types. The handle remains stable across GC compaction. The actual pointer
 * is only obtained when needed through gc_deref(), used immediately, and
 * never stored.
 * 
 * CRITICAL RULE: Never call gc_deref() and store the result in a variable.
 * Always use the GC_PROP_* macros which dereference and use in one operation.
 */

/* Note: GCValue, GCValueUnion, GCValueConst, and GC_VALUE_GET_* macros 
 * are defined in quickjs_types.h */

/* ============================================================================
 * GCHandle-based macros for GC-safe object access
 * ============================================================================
 * These macros work directly with GCHandles instead of pointers, ensuring
 * that object references remain valid across GC compaction.
 */

/* 
 * GC_OBJ_HANDLE - Get a field of type GCHandle from an object accessed via handle.
 * Usage: GCHandle proto_handle = GC_OBJ_HANDLE(obj_handle, JSObject, shape_handle);
 */
#define GC_OBJ_HANDLE(handle, type, field) ({ \
    void *_gc_ptr = gc_deref(handle); \
    (_gc_ptr != NULL) ? ((type *)_gc_ptr)->field : GC_HANDLE_NULL; \
})

/*
 * GC_OBJ_HANDLE_SET - Set a GCHandle field in an object accessed via handle.
 * Usage: GC_OBJ_HANDLE_SET(obj_handle, JSObject, shape_handle, new_shape_handle);
 */
#define GC_OBJ_HANDLE_SET(handle, type, field, value) do { \
    void *_gc_ptr = gc_deref(handle); \
    if (_gc_ptr != NULL) { \
        ((type *)_gc_ptr)->field = (value); \
    } \
} while(0)

/* ============================================================================
 * GC-safe field access macros for JSObject and related structures
 * ============================================================================
 */

/*
 * GC_PROP_GET_HANDLE - Get a GCHandle property from a GCValue object.
 * This macro accesses the property and returns the handle, not a pointer.
 */
#define GC_PROP_GET_HANDLE(ctx, obj, atom, field_type, field_name) ({ \
    GCHandle _gc_handle = GC_HANDLE_NULL; \
    int _gc_tag = GC_VALUE_GET_TAG(obj); \
    if (_gc_tag < 0) { \
        void *_gc_ptr = gc_deref((obj).u.handle); \
        if (_gc_ptr != NULL) { \
            field_type *_obj = (field_type *)_gc_ptr; \
            _gc_handle = _obj->field_name; \
        } \
    } \
    _gc_handle; \
})

/*
 * ============================================================================
 * GC-Safe Field Access Macros - NEVER store the result of gc_deref()
 * ============================================================================
 * 
 * These macros access fields of GC-managed objects through handles.
 * They ensure that pointers are never stored across potential GC points.
 * 
 * CRITICAL RULE: Never do this:
 *   JSObject *p = gc_deref(handle);  // WRONG - storing pointer!
 *   p->field = value;                // p may be invalid here!
 * 
 * Instead, use these macros which access fields through handles:
 *   GC_FIELD_SET(obj_handle, JSObject, field_handle, value_handle);
 */

/*
 * GC_FIELD_GET - Get a GCHandle field from a GC-managed object.
 * Note: This is a macro for generic field access. For specific fields,
 * use the accessor functions below (gc_obj_get_shape_handle, etc.)
 */
#ifdef _MSC_VER
#define GC_FIELD_GET(handle, type, field) \
    ([](GCHandle _h) -> GCHandle { \
        GCHandle _field_handle = GC_HANDLE_NULL; \
        void *_ptr = gc_deref(_h); \
        if (_ptr != NULL) { \
            _field_handle = ((type *)_ptr)->field; \
        } \
        return _field_handle; \
    })(handle)
#else
#define GC_FIELD_GET(handle, type, field) ({ \
    GCHandle _field_handle = GC_HANDLE_NULL; \
    void *_ptr = gc_deref(handle); \
    if (_ptr != NULL) { \
        _field_handle = ((type *)_ptr)->field; \
    } \
    _field_handle; \
})
#endif

/*
 * GC_FIELD_SET - Set a GCHandle field in a GC-managed object.
 */
#define GC_FIELD_SET(handle, type, field, value) do { \
    void *_ptr = gc_deref(handle); \
    if (_ptr != NULL) { \
        ((type *)_ptr)->field = (value); \
    } \
} while(0)

/*
 * Type-safe accessor functions for common JSObject fields.
 * These provide better type safety and debugging compared to macros.
 */
struct JSObject;
struct JSShape;

/* JSObject shape_handle access - defined in quickjs.c */
GCHandle gc_obj_get_shape_handle(GCHandle obj_handle);
void gc_obj_set_shape_handle(GCHandle obj_handle, GCHandle val);

/* JSObject prop_handle access - defined in quickjs.c */
GCHandle gc_obj_get_prop_handle(GCHandle obj_handle);
void gc_obj_set_prop_handle(GCHandle obj_handle, GCHandle val);

/* JSShape proto_handle access - defined in quickjs.c */
GCHandle gc_shape_get_proto_handle(GCHandle shape_handle);
void gc_shape_set_proto_handle(GCHandle shape_handle, GCHandle val);

/*
 * GC_FIELD_GET_PTR - Get a non-GC pointer field from a GC-managed object.
 * This is for data pointers (like byte_code_buf), not GC object pointers.
 * Usage: uint8_t *bytecode = GC_FIELD_GET_PTR(func_handle, JSFunctionBytecode, byte_code_buf);
 */
#define GC_FIELD_GET_PTR(handle, type, field, ptr_type) ({ \
    ptr_type *_field_ptr = NULL; \
    void *_ptr = gc_deref(handle); \
    if (_ptr != NULL) { \
        _field_ptr = ((type *)_ptr)->field; \
    } \
    _field_ptr; \
})

/*
 * GC_OBJ_DEREF - Immediately dereference a handle and call a function with the pointer.
 * This macro ensures the pointer is never stored.
 * Usage: GC_OBJ_DEREF(obj_handle, JSObject, js_object_method, ctx, arg1, arg2);
 */
#define GC_OBJ_DEREF(handle, type, func, ...) ({ \
    void *_ptr = gc_deref(handle); \
    int _result = -1; \
    if (_ptr != NULL) { \
        _result = func(__VA_ARGS__, (type *)_ptr); \
    } \
    _result; \
})

/*
 * GC_SHAPE_DEREF - Dereference a shape handle to get a temporary JSShape pointer.
 * WARNING: The pointer is only valid until the next GC point.
 * NEVER store this pointer. Only use it for immediate field access.
 * 
 * NOTE: For shapes, we store a handle to the JSShape pointer itself (not the
 * base allocation). This is because shapes have a hash table prefix, making
 * the offset from base allocation variable. The handle stores the sh pointer
 */

/*
 * GC_OBJ_GET_SHAPE - Get the shape handle from an object.
 * Usage: GCHandle shape_handle = GC_OBJ_GET_SHAPE(obj_handle);
 */
#define GC_OBJ_GET_SHAPE(obj_handle) GC_FIELD_GET(obj_handle, JSObject, shape_handle)

/*
 * GC_OBJ_GET_CLASS_ID - Get the class ID from an object handle.
 * Usage: uint16_t class_id = GC_OBJ_GET_CLASS_ID(obj_handle);
 */
#define GC_OBJ_GET_CLASS_ID(obj_handle) GC_FIELD_GET(obj_handle, JSObject, class_id)

/*
 * ============================================================================
 * ENHANCED GC-Safe Property Access Macros
 * ============================================================================
 * These macros provide direct field access without exposing gc_deref().
 * Use these instead of calling gc_deref() directly.
 */

/* ============================================================================
 * Scalar Field Access (uint32_t, int, uint16_t, uint8_t, etc.)
 * ============================================================================
 */

/* Get scalar field value (non-handle types) */
#define GC_HANDLE_GET_UINT32(handle, type, field) ({ \
    uint32_t _val = 0; \
    void *_ptr = gc_deref(handle); \
    if (_ptr != NULL) { \
        _val = ((type *)_ptr)->field; \
    } \
    _val; \
})

#define GC_HANDLE_GET_INT(handle, type, field) ({ \
    int _val = 0; \
    void *_ptr = gc_deref(handle); \
    if (_ptr != NULL) { \
        _val = ((type *)_ptr)->field; \
    } \
    _val; \
})

#define GC_HANDLE_GET_UINT16(handle, type, field) ({ \
    uint16_t _val = 0; \
    void *_ptr = gc_deref(handle); \
    if (_ptr != NULL) { \
        _val = ((type *)_ptr)->field; \
    } \
    _val; \
})

#define GC_HANDLE_GET_UINT8(handle, type, field) ({ \
    uint8_t _val = 0; \
    void *_ptr = gc_deref(handle); \
    if (_ptr != NULL) { \
        _val = ((type *)_ptr)->field; \
    } \
    _val; \
})

/* Set scalar field value */
#define GC_HANDLE_SET_UINT32(handle, type, field, value) do { \
    void *_ptr = gc_deref(handle); \
    if (_ptr != NULL) { \
        ((type *)_ptr)->field = (uint32_t)(value); \
    } \
} while(0)

#define GC_HANDLE_SET_INT(handle, type, field, value) do { \
    void *_ptr = gc_deref(handle); \
    if (_ptr != NULL) { \
        ((type *)_ptr)->field = (int)(value); \
    } \
} while(0)

#define GC_HANDLE_SET_UINT8(handle, type, field, value) do { \
    void *_ptr = gc_deref(handle); \
    if (_ptr != NULL) { \
        ((type *)_ptr)->field = (uint8_t)(value); \
    } \
} while(0)

/* ============================================================================
 * Raw Pointer Field Access (non-GC pointers like byte_code_buf, data buffers)
 * ============================================================================
 */
#define GC_FIELD_GET_RAW_PTR(handle, type, field, ptr_type) ({ \
    ptr_type *_field_ptr = NULL; \
    void *_ptr = gc_deref(handle); \
    if (_ptr != NULL) { \
        _field_ptr = (ptr_type)((type *)_ptr)->field; \
    } \
    _field_ptr; \
})

#define GC_FIELD_SET_RAW_PTR(handle, type, field, value) do { \
    void *_ptr = gc_deref(handle); \
    if (_ptr != NULL) { \
        ((type *)_ptr)->field = (value); \
    } \
} while(0)

/* ============================================================================
 * GCValue Field Access
 * ============================================================================
 */
#define GC_FIELD_GET_GCVALUE(handle, type, field) ({ \
    GCValue _val = GC_UNDEFINED; \
    void *_ptr = gc_deref(handle); \
    if (_ptr != NULL) { \
        _val = ((type *)_ptr)->field; \
    } \
    _val; \
})

#define GC_FIELD_SET_GCVALUE(handle, type, field, value) do { \
    void *_ptr = gc_deref(handle); \
    if (_ptr != NULL) { \
        ((type *)_ptr)->field = (value); \
    } \
} while(0)

/* ============================================================================
 * Property Array Access via Handles
 * ============================================================================
 */

/* Get JSProperty array element handle field */
#define GC_PROP_GET_HANDLE_AT(prop_handle, index, field) ({ \
    GCHandle _h = GC_HANDLE_NULL; \
    void *_ptr = gc_deref(prop_handle); \
    if (_ptr != NULL) { \
        JSProperty *_props = (JSProperty *)_ptr; \
        _h = _props[index].field; \
    } \
    _h; \
})

/* Set JSProperty array element handle field */
#define GC_PROP_SET_HANDLE_AT(prop_handle, index, field, value) do { \
    void *_ptr = gc_deref(prop_handle); \
    if (_ptr != NULL) { \
        JSProperty *_props = (JSProperty *)_ptr; \
        _props[index].field = (value); \
    } \
} while(0)

/* Get JSProperty array element GCValue */
#define GC_PROP_GET_GCVALUE_AT(prop_handle, index) ({ \
    GCValue _val = GC_UNDEFINED; \
    void *_ptr = gc_deref(prop_handle); \
    if (_ptr != NULL) { \
        JSProperty *_props = (JSProperty *)_ptr; \
        _val = _props[index].u.value; \
    } \
    _val; \
})

/* Set JSProperty array element GCValue */
#define GC_PROP_SET_GCVALUE_AT(prop_handle, index, value) do { \
    void *_ptr = gc_deref(prop_handle); \
    if (_ptr != NULL) { \
        JSProperty *_props = (JSProperty *)_ptr; \
        _props[index].u.value = (value); \
    } \
} while(0)

/* Get getter handle from property at index */
#define GC_PROP_GET_GETTER_AT(prop_handle, index) ({ \
    GCHandle _h = GC_HANDLE_NULL; \
    void *_ptr = gc_deref(prop_handle); \
    if (_ptr != NULL) { \
        JSProperty *_props = (JSProperty *)_ptr; \
        _h = _props[index].u.getset.getter_handle; \
    } \
    _h; \
})

/* Set getter handle in property at index */
#define GC_PROP_SET_GETTER_AT(prop_handle, index, h) do { \
    void *_ptr = gc_deref(prop_handle); \
    if (_ptr != NULL) { \
        JSProperty *_props = (JSProperty *)_ptr; \
        _props[index].u.getset.getter_handle = (h); \
    } \
} while(0)

/* Get setter handle from property at index */
#define GC_PROP_GET_SETTER_AT(prop_handle, index) ({ \
    GCHandle _h = GC_HANDLE_NULL; \
    void *_ptr = gc_deref(prop_handle); \
    if (_ptr != NULL) { \
        JSProperty *_props = (JSProperty *)_ptr; \
        _h = _props[index].u.getset.setter_handle; \
    } \
    _h; \
})

/* Set setter handle in property at index */
#define GC_PROP_SET_SETTER_AT(prop_handle, index, h) do { \
    void *_ptr = gc_deref(prop_handle); \
    if (_ptr != NULL) { \
        JSProperty *_props = (JSProperty *)_ptr; \
        _props[index].u.getset.setter_handle = (h); \
    } \
} while(0)

/* ============================================================================
 * Shape Property Access via Handles
 * ============================================================================
 */

/* Get JSShapeProperty field at index */
#define GC_SHAPE_PROP_GET_ATOM_AT(shape_handle, index) ({ \
    JSAtom _atom = JS_ATOM_NULL; \
    void *_ptr = gc_deref(shape_handle); \
    if (_ptr != NULL) { \
        JSShape *_sh = (JSShape *)_ptr; \
        JSShapeProperty *_props = (JSShapeProperty *)((uint8_t *)_sh + sizeof(JSShape)); \
        _atom = _props[index].atom; \
    } \
    _atom; \
})

#define GC_SHAPE_PROP_GET_FLAGS_AT(shape_handle, index) ({ \
    uint32_t _flags = 0; \
    void *_ptr = gc_deref(shape_handle); \
    if (_ptr != NULL) { \
        JSShape *_sh = (JSShape *)_ptr; \
        JSShapeProperty *_props = (JSShapeProperty *)((uint8_t *)_sh + sizeof(JSShape)); \
        _flags = _props[index].flags; \
    } \
    _flags; \
})

#define GC_SHAPE_PROP_GET_HASH_NEXT_AT(shape_handle, index) ({ \
    uint32_t _next = 0; \
    void *_ptr = gc_deref(shape_handle); \
    if (_ptr != NULL) { \
        JSShape *_sh = (JSShape *)_ptr; \
        JSShapeProperty *_props = (JSShapeProperty *)((uint8_t *)_sh + sizeof(JSShape)); \
        _next = _props[index].hash_next; \
    } \
    _next; \
})

/* ============================================================================
 * JSObject/JSShape Accessor Functions (replacing macros)
 * ============================================================================
 * Use these functions instead of macros for type safety and better debugging.
 */

/* JSObject boolean checks - defined in quickjs.c */
int gc_obj_is_exotic(GCHandle obj_handle);
int gc_obj_is_fast_array(GCHandle obj_handle);
int gc_obj_is_extensible(GCHandle obj_handle);
uint32_t gc_obj_get_weakref_count(GCHandle obj_handle);

/* JSShape accessors - defined in quickjs.c */
int gc_shape_get_prop_count(GCHandle shape_handle);
int gc_shape_is_hashed(GCHandle shape_handle);
void gc_shape_set_hashed(GCHandle shape_handle, int val);

/* ============================================================================
 * JSShape-specific Access Macros
 * ============================================================================
 */
#define GC_SHAPE_GET_PROTO_HANDLE(shape_handle) GC_FIELD_GET(shape_handle, JSShape, proto_handle)
#define GC_SHAPE_SET_PROTO_HANDLE(shape_handle, val) GC_FIELD_SET(shape_handle, JSShape, proto_handle, val)
#define GC_SHAPE_GET_HASH_NEXT_HANDLE(shape_handle) GC_FIELD_GET(shape_handle, JSShape, shape_hash_next_handle)
#define GC_SHAPE_SET_HASH_NEXT_HANDLE(shape_handle, val) GC_FIELD_SET(shape_handle, JSShape, shape_hash_next_handle, val)

/* ============================================================================
 * GC Dereference Function
 * ============================================================================
 * The gc_deref() function converts a GCHandle to a pointer.
 * Use the macros above instead of calling this directly.
 */
void *gc_deref(GCHandle handle);

/* gc_deref_unsafe: Intentional unsafe dereference for cases where we need raw pointers.
 * This is used for BigInt arithmetic operands where we pass pointers to C-style functions.
 * These cannot be removed as they bridge the handle-based GC with raw pointer operations. */
#define gc_deref_unsafe(handle) gc_deref(handle)

/* Type predicate macros */
#define GC_IS_OBJECT(v)       (GC_VALUE_GET_TAG(v) == JS_TAG_OBJECT)
#define GC_IS_NULL(v)         (GC_VALUE_GET_TAG(v) == JS_TAG_NULL)
#define GC_IS_UNDEFINED(v)    (GC_VALUE_GET_TAG(v) == JS_TAG_UNDEFINED)
#define GC_IS_BOOL(v)         (GC_VALUE_GET_TAG(v) == JS_TAG_BOOL)
#define GC_IS_INT(v)          (GC_VALUE_GET_TAG(v) == JS_TAG_INT)
#define GC_IS_FLOAT64(v)      (GC_VALUE_GET_TAG(v) == JS_TAG_FLOAT64)
#define GC_IS_STRING(v)       (GC_VALUE_GET_TAG(v) == JS_TAG_STRING || \
                               GC_VALUE_GET_TAG(v) == JS_TAG_STRING_ROPE)
#define GC_IS_SYMBOL(v)       (GC_VALUE_GET_TAG(v) == JS_TAG_SYMBOL)
#define GC_IS_BIG_INT(v)      (GC_VALUE_GET_TAG(v) == JS_TAG_BIG_INT || \
                               GC_VALUE_GET_TAG(v) == JS_TAG_SHORT_BIG_INT)
#define GC_IS_EXCEPTION(v)    (GC_VALUE_GET_TAG(v) == JS_TAG_EXCEPTION)

/* Reference types have negative tags */
#define GC_IS_REFERENCE(v)    (GC_VALUE_GET_TAG(v) < 0)

/* Create GCValue for primitive types */
#ifdef _MSC_VER
static inline GCValue GC_MKVAL_FUNC(int64_t tag, int32_t val) {
    GCValue v;
    v.u.int32 = val;
    v.tag = tag;
    return v;
}
#define GC_MKVAL(tag, val) GC_MKVAL_FUNC(tag, val)
#else
#define GC_MKVAL(tag, val) (GCValue){ (GCValueUnion){ .int32 = val }, tag }
#endif

/* Create GCValue from a handle for reference types */
#ifdef _MSC_VER
static inline GCValue GC_MKHANDLE_FUNC(int64_t tag, GCHandle handle_val) {
    GCValue v;
    v.u.handle = handle_val;
    v.tag = tag;
    return v;
}
#define GC_MKHANDLE(tag, handle_val) GC_MKHANDLE_FUNC(tag, handle_val)
#else
#define GC_MKHANDLE(tag, handle_val) (GCValue){ (GCValueUnion){ .handle = handle_val }, tag }
#endif

#define GC_TAG_IS_FLOAT64(tag) ((unsigned)(tag) == JS_TAG_FLOAT64)

/* Special values - use inline functions for C++ compatibility */
static inline GCValue GC_NAN_VAL() {
    GCValue v;
    v.tag = JS_TAG_FLOAT64;
    union { uint64_t u; double d; } nan = { 0x7FF8000000000000ULL };
    v.u.float64 = nan.d;
    return v;
}
#define GC_NAN GC_NAN_VAL()

/* Constructors for primitive types are defined in quickjs_types.h */

static inline JS_BOOL GC_VALUE_IS_NAN(GCValue v)
{
    union {
        double d;
        uint64_t u64;
    } u;
    if (v.tag != JS_TAG_FLOAT64)
        return 0;
    u.d = v.u.float64;
    return (u.u64 & 0x7fffffffffffffff) > 0x7ff0000000000000;
}

static inline GCValue GC_NewShortBigInt(int64_t d)
{
    GCValue v;
    v.tag = JS_TAG_SHORT_BIG_INT;
    v.u.short_big_int = d;
    return v;
}

#endif /* !JS_NAN_BOXING */

/* 
 * Note: JSErrorEnum, struct JSContext, struct JSRuntime, and the C++ classes
 * JSContextHandle and JSRuntimeHandle are all defined in quickjs_types.h
 * 
 * The C++ handle classes provide GC-safe access to JSContext and JSRuntime
 * through getter/setter methods. They never expose raw pointers directly.
 */

/* ============================================================================
 * Reference Counting Stubs for Mark-and-Sweep GC
 * ============================================================================
 * These are no-ops since we're using mark-and-sweep GC with shadow stack
 */
/* Note: With GC-based memory management, GCValue doesn't need reference counting.
   Simply assign/copy GCValue directly - the GC tracks object lifetime via reachability.
   No manual Dup/Free needed - the stable handle in GCValue is automatically valid. */

#define GC_VALUE_IS_BOTH_INT(v1, v2) ((GC_VALUE_GET_TAG(v1) | GC_VALUE_GET_TAG(v2)) == 0)
#define GC_VALUE_IS_BOTH_FLOAT(v1, v2) (GC_TAG_IS_FLOAT64(GC_VALUE_GET_TAG(v1)) && GC_TAG_IS_FLOAT64(GC_VALUE_GET_TAG(v2)))

#define GC_VALUE_HAS_REF_COUNT(v) ((unsigned)GC_VALUE_GET_TAG(v) >= (unsigned)JS_TAG_FIRST)

/* Special values - defined in quickjs_types.h */

/* Backward compatibility macros - map old JS_* names to GC_* names */
#define JS_NULL      GC_NULL
#define JS_UNDEFINED GC_UNDEFINED
#define JS_FALSE     GC_FALSE
#define JS_TRUE      GC_TRUE
#define JS_EXCEPTION GC_EXCEPTION
#define JS_UNINITIALIZED GC_UNINITIALIZED

/* Backward compatibility for value access macros */
#define JS_VALUE_GET_TAG(v)       GC_VALUE_GET_TAG(v)
#define JS_VALUE_GET_NORM_TAG(v)  GC_VALUE_GET_NORM_TAG(v)
#define JS_VALUE_GET_INT(v)       GC_VALUE_GET_INT(v)
#define JS_VALUE_GET_BOOL(v)      GC_VALUE_GET_BOOL(v)
#define JS_VALUE_GET_FLOAT64(v)   GC_VALUE_GET_FLOAT64(v)
#define JS_VALUE_HAS_REF_COUNT(v) GC_VALUE_HAS_REF_COUNT(v)
#define JS_VALUE_GET_SHORT_BIG_INT(v) GC_VALUE_GET_SHORT_BIG_INT(v)
#define JS_TAG_IS_FLOAT64(tag)    GC_TAG_IS_FLOAT64(tag)
#define JS_VALUE_IS_BOTH_INT(v1, v2) GC_VALUE_IS_BOTH_INT(v1, v2)
#define JS_VALUE_IS_BOTH_FLOAT(v1, v2) GC_VALUE_IS_BOTH_FLOAT(v1, v2)
#define JS_MKVAL(tag, val)        GC_MKVAL(tag, val)
#define JS_NAN                    GC_NAN

/* Additional backward compatibility functions */
static inline GCValue __JS_NewShortBigInt(JSContextHandle ctx, int64_t val)
{
    (void)ctx;
    return GC_NewShortBigInt(val);
}

/* NOTE: GC_MKPTR removed - use GC_MKHANDLE with explicit handles instead */

/* GC_MKHANDLE - Create a GCValue directly from a handle (for C++ handle-based API) */
#ifndef GC_MKHANDLE
#ifdef _MSC_VER
#define GC_MKHANDLE(tag, h) GC_MKHANDLE_FUNC(tag, h)
#else
#define GC_MKHANDLE(tag, h) (GCValue){ (GCValueUnion){ .handle = (h) }, (tag) }
#endif
#endif

/* GC_VALUE_GET_PTR - needs to be defined for non-NaN boxing case */
#ifndef GC_VALUE_GET_PTR
#define GC_VALUE_GET_PTR(v) ({ \
    void *_ptr = NULL; \
    if (GC_VALUE_GET_TAG(v) < 0) { \
        _ptr = gc_deref(GC_VALUE_GET_HANDLE(v)); \
    } else { \
        _ptr = (void *)((intptr_t)(v).u.int32 & ~0xf); \
    } \
    _ptr; \
})
#endif

/* ============================================================================
 * GCValue Property Access Macros
 * ============================================================================
 * 
 * These macros provide safe property access for GCValue/GCValue objects.
 * They dereference the handle and access the property in one operation,
 * ensuring the pointer is never stored across potential GC points.
 * 
 * RULE: Never store the result of GC_VALUE_GET_PTR(). Always use these
 * macros which get the pointer and use it immediately.
 */

/*
 * GC_PROP_GET_STR and GC_PROP_SET_STR are defined in gc_value_helpers.h
 * as static inline functions for type safety.
 */

/*
 * GC_PROP_GET_UINT32 - Get property by numeric index.
 * Uses handle directly without pointer round-trip for GC safety.
 */
#define GC_PROP_GET_UINT32(ctx, obj, idx) ({ \
    GCValue _gc_result = GC_UNDEFINED; \
    int _gc_tag = GC_VALUE_GET_TAG(obj); \
    if (_gc_tag < 0) { \
        /* Use the GCValue directly - it already contains the handle */ \
        _gc_result = JS_GetPropertyUint32((ctx), (obj), (idx)); \
    } \
    _gc_result; \
})

/*
 * GC_PROP_SET_UINT32 - Set property by numeric index.
 * Uses handle directly without pointer round-trip for GC safety.
 */
#define GC_PROP_SET_UINT32(ctx, obj, idx, val) ({ \
    int _gc_result = -1; \
    int _gc_tag = GC_VALUE_GET_TAG(obj); \
    if (_gc_tag < 0) { \
        /* Use the GCValue directly - it already contains the handle */ \
        _gc_result = JS_SetPropertyUint32((ctx), (obj), (idx), (val)); \
    } \
    _gc_result; \
})

/*
 * GC_IS_OBJECT - Check if GCValue is an object.
 */
#define GC_IS_OBJECT(v) (GC_VALUE_GET_TAG(v) == JS_TAG_OBJECT)

/*
 * GC_IS_NULL - Check if GCValue is null.
 */
#define GC_IS_NULL(v) (GC_VALUE_GET_TAG(v) == JS_TAG_NULL)

/*
 * GC_IS_UNDEFINED - Check if GCValue is undefined.
 */
#define GC_IS_UNDEFINED(v) (GC_VALUE_GET_TAG(v) == JS_TAG_UNDEFINED)

/*
 * GC_IS_STRING - Check if GCValue is a string.
 */
#define GC_IS_STRING(v) (GC_VALUE_GET_TAG(v) == JS_TAG_STRING || \
                         GC_VALUE_GET_TAG(v) == JS_TAG_STRING_ROPE)

/* ============================================================================
 * Backwards compatibility: JS property functions work with GCValue
 * ============================================================================
 */

/* flags for object properties */
#define JS_PROP_CONFIGURABLE  (1 << 0)
#define JS_PROP_WRITABLE      (1 << 1)
#define JS_PROP_ENUMERABLE    (1 << 2)
#define JS_PROP_C_W_E         (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE)
#define JS_PROP_LENGTH        (1 << 3) /* used internally in Arrays */
#define JS_PROP_TMASK         (3 << 4) /* mask for NORMAL, GETSET, VARREF, AUTOINIT */
#define JS_PROP_NORMAL         (0 << 4)
#define JS_PROP_GETSET         (1 << 4)
#define JS_PROP_VARREF         (2 << 4) /* used internally */
#define JS_PROP_AUTOINIT       (3 << 4) /* used internally */

/* flags for JS_DefineProperty */
#define JS_PROP_HAS_SHIFT        8
#define JS_PROP_HAS_CONFIGURABLE (1 << 8)
#define JS_PROP_HAS_WRITABLE     (1 << 9)
#define JS_PROP_HAS_ENUMERABLE   (1 << 10)
#define JS_PROP_HAS_GET          (1 << 11)
#define JS_PROP_HAS_SET          (1 << 12)
#define JS_PROP_HAS_VALUE        (1 << 13)

/* throw an exception if false would be returned
   (JS_DefineProperty/JS_SetProperty) */
#define JS_PROP_THROW            (1 << 14)
/* throw an exception if false would be returned in strict mode
   (JS_SetProperty) */
#define JS_PROP_THROW_STRICT     (1 << 15)

#define JS_PROP_NO_EXOTIC        (1 << 16) /* internal use */

#ifndef JS_DEFAULT_STACK_SIZE
#define JS_DEFAULT_STACK_SIZE (1024 * 1024)
#endif

/* JS_Eval() flags */
#define JS_EVAL_TYPE_GLOBAL   (0 << 0) /* global code (default) */
#define JS_EVAL_TYPE_MODULE   (1 << 0) /* module code */
#define JS_EVAL_TYPE_DIRECT   (2 << 0) /* direct call (internal use) */
#define JS_EVAL_TYPE_INDIRECT (3 << 0) /* indirect call (internal use) */
#define JS_EVAL_TYPE_MASK     (3 << 0)

#define JS_EVAL_FLAG_STRICT   (1 << 3) /* force 'strict' mode */
/* compile but do not run. The result is an object with a
   JS_TAG_FUNCTION_BYTECODE or JS_TAG_MODULE tag. It can be executed
   with JS_EvalFunction(). */
#define JS_EVAL_FLAG_COMPILE_ONLY (1 << 5)
/* don't include the stack frames before this eval in the Error() backtraces */
#define JS_EVAL_FLAG_BACKTRACE_BARRIER (1 << 6)
/* allow top-level await in normal script. JS_Eval() returns a
   promise. Only allowed with JS_EVAL_TYPE_GLOBAL */
#define JS_EVAL_FLAG_ASYNC (1 << 7)
/* disable lazy function parsing - parse all functions immediately */
#define JS_EVAL_FLAG_NO_LAZY (1 << 8)

/* JSCFunction typedefs are defined in quickjs_types.h */

/* Note: JSMallocState is defined in quickjs_types.h */

typedef struct GCHeader GCHeader;

/* 
 * Create a new JS runtime.
 * NOTE: gc_init() MUST be called before this function!
 * All memory comes from the unified GC allocator.
 */
JSRuntimeHandle JS_NewRuntime(void);
/* info lifetime must exceed that of rt */
void JS_SetRuntimeInfo(JSRuntimeHandle rt, const char *info);
void JS_SetMemoryLimit(JSRuntimeHandle rt, size_t limit);
void JS_SetGCThreshold(JSRuntimeHandle rt, size_t gc_threshold);
/* use 0 to disable maximum stack size check */
void JS_SetMaxStackSize(JSRuntimeHandle rt, size_t stack_size);
/* should be called when changing thread to update the stack top value
   used to check stack overflow. */
void JS_UpdateStackTop(JSRuntimeHandle rt);
void JS_FreeRuntime(JSRuntimeHandle rt);
void *JS_GetRuntimeOpaque(JSRuntimeHandle rt);
void JS_SetRuntimeOpaque(JSRuntimeHandle rt, void *opaque);
typedef void JS_MarkFunc(JSRuntimeHandle rt, GCHandle handle);
void JS_MarkValue(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func);
void JS_RunGC(JSRuntimeHandle rt);
JS_BOOL JS_IsLiveObject(JSRuntimeHandle rt, GCValue obj);

JSContextHandle JS_NewContext(JSRuntimeHandle rt);
void *JS_GetContextOpaque(JSContextHandle ctx);
void JS_SetContextOpaque(JSContextHandle ctx, void *opaque);
JSRuntimeHandle JS_GetRuntime(JSContextHandle ctx);
void JS_SetClassProto(JSContextHandle ctx, JSClassID class_id, GCValue obj);
GCValue JS_GetClassProto(JSContextHandle ctx, JSClassID class_id);

/* the following functions are used to select the intrinsic object to
   save memory */
JSContextHandle JS_NewContextRaw(JSRuntimeHandle rt);
int JS_AddIntrinsicBaseObjects(JSContextHandle ctx);
int JS_AddIntrinsicDate(JSContextHandle ctx);
int JS_AddIntrinsicEval(JSContextHandle ctx);
int JS_AddIntrinsicStringNormalize(JSContextHandle ctx);
void JS_AddIntrinsicRegExpCompiler(JSContextHandle ctx);
int JS_AddIntrinsicRegExp(JSContextHandle ctx);
int JS_AddIntrinsicJSON(JSContextHandle ctx);
int JS_AddIntrinsicProxy(JSContextHandle ctx);
int JS_AddIntrinsicMapSet(JSContextHandle ctx);
int JS_AddIntrinsicTypedArrays(JSContextHandle ctx);
int JS_AddIntrinsicPromise(JSContextHandle ctx);
int JS_AddIntrinsicWeakRef(JSContextHandle ctx);

GCValue js_string_codePointRange(JSContextHandle ctx, GCValue this_val,
                                 int argc, GCValue *argv);

/* GC allocation functions - use gc_alloc/gc_realloc directly
 * 
 * Pattern replacements:
 *   js_malloc(ctx, size)        -> gc_alloc(size, JS_GC_OBJ_TYPE_DATA)
 *   js_mallocz(ctx, size)       -> gc_allocz(size, JS_GC_OBJ_TYPE_DATA)
 *   js_realloc(ctx, h, size)    -> gc_realloc(h, size)
 *   js_realloc2(ctx, h, sz, sl) -> gc_realloc2(h, sz, &sl)
 *   js_strdup(ctx, str)         -> gc_strdup(str)
 *   js_strndup(ctx, s, n)       -> gc_strndup(s, n)
 *   js_malloc_usable_size(...)  -> gc_usable_size(handle)
 * 
 * Note: No free functions - GC automatically reclaims unreachable objects
 */

typedef struct JSMemoryUsage {
    int64_t malloc_size, malloc_limit, memory_used_size;
    int64_t malloc_count;
    int64_t memory_used_count;
    int64_t atom_count, atom_size;
    int64_t str_count, str_size;
    int64_t obj_count, obj_size;
    int64_t prop_count, prop_size;
    int64_t shape_count, shape_size;
    int64_t js_func_count, js_func_size, js_func_code_size;
    int64_t js_func_pc2line_count, js_func_pc2line_size;
    int64_t c_func_count, array_count;
    int64_t fast_array_count, fast_array_elements;
    int64_t binary_object_count, binary_object_size;
} JSMemoryUsage;

void JS_ComputeMemoryUsage(JSRuntimeHandle rt, JSMemoryUsage *s);
void JS_DumpMemoryUsage(FILE *fp, const JSMemoryUsage *s, JSRuntimeHandle rt);

/* atom support */
#define JS_ATOM_NULL 0

JSAtom JS_NewAtomLen(JSContextHandle ctx, const char *str, size_t len);
JSAtom JS_NewAtom(JSContextHandle ctx, const char *str);
JSAtom JS_NewAtomUInt32(JSContextHandle ctx, uint32_t n);
JSAtom JS_DupAtom(JSContextHandle ctx, JSAtom v);
void JS_FreeAtom(JSContextHandle ctx, JSAtom v);
void JS_FreeAtomRT(JSRuntimeHandle rt, JSAtom v);
GCValue JS_AtomToValue(JSContextHandle ctx, JSAtom atom);
GCValue JS_AtomToString(JSContextHandle ctx, JSAtom atom);
const char *JS_AtomToCStringLen(JSContextHandle ctx, size_t *plen, JSAtom atom);
static inline const char *JS_AtomToCString(JSContextHandle ctx, JSAtom atom)
{
    return JS_AtomToCStringLen(ctx, NULL, atom);
}
JSAtom JS_ValueToAtom(JSContextHandle ctx, GCValue val);

/* object class support */

/* JSPropertyEnum is now defined in quickjs-internal.h */

typedef struct JSPropertyDescriptor {
    int flags;
    GCValue value;
    GCValue getter;
    GCValue setter;
} JSPropertyDescriptor;

typedef struct JSClassExoticMethods {
    /* Return -1 if exception (can only happen in case of Proxy object),
       FALSE if the property does not exists, TRUE if it exists. If 1 is
       returned, the property descriptor 'desc' is filled if != NULL. */
    int (*get_own_property)(JSContextHandle ctx, JSPropertyDescriptor *desc,
                             GCValue obj, JSAtom prop);
    /* '*ptab' should hold the '*plen' property keys. Return 0 if OK,
       -1 if exception. The 'is_enumerable' field is ignored.
    */
    int (*get_own_property_names)(JSContextHandle ctx, JSPropertyEnum **ptab,
                                  uint32_t *plen,
                                  GCValue obj);
    /* return < 0 if exception, or TRUE/FALSE */
    int (*delete_property)(JSContextHandle ctx, GCValue obj, JSAtom prop);
    /* return < 0 if exception or TRUE/FALSE */
    int (*define_own_property)(JSContextHandle ctx, GCValue this_obj,
                               JSAtom prop, GCValue val,
                               GCValue getter, GCValue setter,
                               int flags);
    /* The following methods can be emulated with the previous ones,
       so they are usually not needed */
    /* return < 0 if exception or TRUE/FALSE */
    int (*has_property)(JSContextHandle ctx, GCValue obj, JSAtom atom);
    GCValue (*get_property)(JSContextHandle ctx, GCValue obj, JSAtom atom,
                            GCValue receiver);
    /* return < 0 if exception or TRUE/FALSE */
    int (*set_property)(JSContextHandle ctx, GCValue obj, JSAtom atom,
                        GCValue value, GCValue receiver, int flags);

    /* To get a consistent object behavior when get_prototype != NULL,
       get_property, set_property and set_prototype must be != NULL
       and the object must be created with a GC_NULL prototype. */
    GCValue (*get_prototype)(JSContextHandle ctx, GCValue obj);
    /* return < 0 if exception or TRUE/FALSE */
    int (*set_prototype)(JSContextHandle ctx, GCValue obj, GCValue proto_val);
    /* return < 0 if exception or TRUE/FALSE */
    int (*is_extensible)(JSContextHandle ctx, GCValue obj);
    /* return < 0 if exception or TRUE/FALSE */
    int (*prevent_extensions)(JSContextHandle ctx, GCValue obj);
} JSClassExoticMethods;

#define JS_CALL_FLAG_CONSTRUCTOR (1 << 0)

typedef struct JSClassDef {
    const char *class_name;
    JSClassFinalizer *finalizer;
    JSClassGCMark *gc_mark;
    /* if call != NULL, the object is a function. If (flags &
       JS_CALL_FLAG_CONSTRUCTOR) != 0, the function is called as a
       constructor. In this case, 'this_val' is new.target. A
       constructor call only happens if the object constructor bit is
       set (see JS_SetConstructorBit()). */
    JSClassCall *call;
    /* XXX: suppress this indirection ? It is here only to save memory
       because only a few classes need these methods */
    JSClassExoticMethods *exotic;
} JSClassDef;

#define JS_INVALID_CLASS_ID 0
JSClassID JS_NewClassID(JSClassID *pclass_id);
/* Returns the class ID if `v` is an object, otherwise returns JS_INVALID_CLASS_ID. */
JSClassID JS_GetClassID(GCValue v);
int JS_NewClass(JSRuntimeHandle rt, JSClassID class_id, const JSClassDef *class_def);
int JS_IsRegisteredClass(JSRuntimeHandle rt, JSClassID class_id);

/* value handling */

static js_force_inline GCValue JS_NewBool(JSContextHandle ctx, JS_BOOL val)
{
    return GC_MKVAL(JS_TAG_BOOL, (val != 0));
}

static js_force_inline GCValue JS_NewInt32(JSContextHandle ctx, int32_t val)
{
    return GC_MKVAL(JS_TAG_INT, val);
}

static js_force_inline GCValue JS_NewCatchOffset(JSContextHandle ctx, int32_t val)
{
    return GC_MKVAL(JS_TAG_CATCH_OFFSET, val);
}

/* Internal function to create a Float64 GCValue (ctx not used, for compatibility) */
static inline GCValue __JS_NewFloat64(JSContextHandle ctx, double d)
{
    (void)ctx;  /* ctx not used but kept for API compatibility */
    return GC_NewFloat64(d);
}

static js_force_inline GCValue JS_NewInt64(JSContextHandle ctx, int64_t val)
{
    GCValue v;
    if (val == (int32_t)val) {
        v = JS_NewInt32(ctx, val);
    } else {
        v = __JS_NewFloat64(ctx, val);
    }
    return v;
}

static js_force_inline GCValue JS_NewUint32(JSContextHandle ctx, uint32_t val)
{
    GCValue v;
    if (val <= 0x7fffffff) {
        v = JS_NewInt32(ctx, val);
    } else {
        v = __JS_NewFloat64(ctx, val);
    }
    return v;
}

GCValue JS_NewBigInt64(JSContextHandle ctx, int64_t v);
GCValue JS_NewBigUint64(JSContextHandle ctx, uint64_t v);

static js_force_inline GCValue JS_NewFloat64(JSContextHandle ctx, double d)
{
    int32_t val;
    union {
        double d;
        uint64_t u;
    } u, t;
    if (d >= INT32_MIN && d <= INT32_MAX) {
        u.d = d;
        val = (int32_t)d;
        t.d = val;
        /* -0 cannot be represented as integer, so we compare the bit
           representation */
        if (u.u == t.u)
            return GC_MKVAL(JS_TAG_INT, val);
    }
    return __JS_NewFloat64(ctx, d);
}

static inline JS_BOOL JS_IsNumber(GCValue v)
{
    int tag = GC_VALUE_GET_TAG(v);
    return tag == JS_TAG_INT || GC_TAG_IS_FLOAT64(tag);
}

static inline JS_BOOL JS_IsBigInt(GCValue v)
{
    int tag = GC_VALUE_GET_TAG(v);
    return tag == JS_TAG_BIG_INT || tag == JS_TAG_SHORT_BIG_INT;
}

static inline JS_BOOL JS_IsBool(GCValue v)
{
    return GC_VALUE_GET_TAG(v) == JS_TAG_BOOL;
}

static inline JS_BOOL JS_IsNull(GCValue v)
{
    return GC_VALUE_GET_TAG(v) == JS_TAG_NULL;
}

static inline JS_BOOL JS_IsUndefined(GCValue v)
{
    return GC_VALUE_GET_TAG(v) == JS_TAG_UNDEFINED;
}

static inline JS_BOOL JS_IsException(GCValue v)
{
    return js_unlikely(GC_VALUE_GET_TAG(v) == JS_TAG_EXCEPTION);
}

static inline JS_BOOL JS_IsUninitialized(GCValue v)
{
    return js_unlikely(GC_VALUE_GET_TAG(v) == JS_TAG_UNINITIALIZED);
}

static inline JS_BOOL JS_IsString(GCValue v)
{
    return GC_VALUE_GET_TAG(v) == JS_TAG_STRING ||
        GC_VALUE_GET_TAG(v) == JS_TAG_STRING_ROPE;
}

static inline JS_BOOL JS_IsSymbol(GCValue v)
{
    return GC_VALUE_GET_TAG(v) == JS_TAG_SYMBOL;
}

static inline JS_BOOL JS_IsObject(GCValue v)
{
    return GC_VALUE_GET_TAG(v) == JS_TAG_OBJECT;
}

GCValue JS_Throw(JSContextHandle ctx, GCValue obj);
void JS_SetUncatchableException(JSContextHandle ctx, JS_BOOL flag);
GCValue JS_GetException(JSContextHandle ctx);
JS_BOOL JS_HasException(JSContextHandle ctx);
JS_BOOL JS_IsError(JSContextHandle ctx, GCValue val);
GCValue JS_NewError(JSContextHandle ctx);
GCValue __js_printf_like(2, 3) JS_ThrowSyntaxError(JSContextHandle ctx, const char *fmt, ...);
GCValue __js_printf_like(2, 3) JS_ThrowTypeError(JSContextHandle ctx, const char *fmt, ...);
GCValue __js_printf_like(2, 3) JS_ThrowReferenceError(JSContextHandle ctx, const char *fmt, ...);
GCValue __js_printf_like(2, 3) JS_ThrowRangeError(JSContextHandle ctx, const char *fmt, ...);
GCValue __js_printf_like(2, 3) JS_ThrowInternalError(JSContextHandle ctx, const char *fmt, ...);
GCValue JS_ThrowOutOfMemory(JSContextHandle ctx);

/* Note: Reference counting functions (JS_FreeValue, JS_DupValue, etc.) removed.
   Using mark-and-sweep GC only. */

JS_BOOL JS_StrictEq(JSContextHandle ctx, GCValue op1, GCValue op2);
JS_BOOL JS_SameValue(JSContextHandle ctx, GCValue op1, GCValue op2);
JS_BOOL JS_SameValueZero(JSContextHandle ctx, GCValue op1, GCValue op2);

int JS_ToBool(JSContextHandle ctx, GCValue val); /* return -1 for GC_EXCEPTION */
int JS_ToInt32(JSContextHandle ctx, int32_t *pres, GCValue val);
static inline int JS_ToUint32(JSContextHandle ctx, uint32_t *pres, GCValue val)
{
    return JS_ToInt32(ctx, (int32_t*)pres, val);
}
int JS_ToInt64(JSContextHandle ctx, int64_t *pres, GCValue val);
int JS_ToIndex(JSContextHandle ctx, uint64_t *plen, GCValue val);
int JS_ToFloat64(JSContextHandle ctx, double *pres, GCValue val);
/* return an exception if 'val' is a Number */
int JS_ToBigInt64(JSContextHandle ctx, int64_t *pres, GCValue val);
/* same as JS_ToInt64() but allow BigInt */
int JS_ToInt64Ext(JSContextHandle ctx, int64_t *pres, GCValue val);

GCValue JS_NewStringLen(JSContextHandle ctx, const char *str1, size_t len1);
static inline GCValue JS_NewString(JSContextHandle ctx, const char *str)
{
    return JS_NewStringLen(ctx, str, strlen(str));
}
GCValue JS_NewAtomString(JSContextHandle ctx, const char *str);
GCValue JS_ToString(JSContextHandle ctx, GCValue val);
GCValue JS_ToPropertyKey(JSContextHandle ctx, GCValue val);
const char *JS_ToCStringLen2(JSContextHandle ctx, size_t *plen, GCValue val1, JS_BOOL cesu8);
static inline const char *JS_ToCStringLen(JSContextHandle ctx, size_t *plen, GCValue val1)
{
    return JS_ToCStringLen2(ctx, plen, val1, 0);
}
static inline const char *JS_ToCString(JSContextHandle ctx, GCValue val1)
{
    return JS_ToCStringLen2(ctx, NULL, val1, 0);
}

GCValue JS_NewObjectProtoClass(JSContextHandle ctx, GCValue proto, JSClassID class_id);
GCValue JS_NewObjectClass(JSContextHandle ctx, int class_id);
GCValue JS_NewObjectProto(JSContextHandle ctx, GCValue proto);
GCValue JS_NewObject(JSContextHandle ctx);

JS_BOOL JS_IsFunction(JSContextHandle ctx, GCValue val);
JS_BOOL JS_IsConstructor(JSContextHandle ctx, GCValue val);
JS_BOOL JS_SetConstructorBit(JSContextHandle ctx, GCValue func_obj, JS_BOOL val);

GCValue JS_NewArray(JSContextHandle ctx);
int JS_IsArray(JSContextHandle ctx, GCValue val);

GCValue JS_NewDate(JSContextHandle ctx, double epoch_ms);

GCValue JS_GetPropertyInternal(JSContextHandle ctx, GCValue obj,
                               JSAtom prop, GCValue receiver,
                               JS_BOOL throw_ref_error);
static js_force_inline GCValue JS_GetProperty(JSContextHandle ctx, GCValue this_obj,
                                              JSAtom prop)
{
    return JS_GetPropertyInternal(ctx, this_obj, prop, this_obj, 0);
}
GCValue JS_GetPropertyStr(JSContextHandle ctx, GCValue this_obj,
                          const char *prop);
GCValue JS_GetPropertyUint32(JSContextHandle ctx, GCValue this_obj,
                             uint32_t idx);

int JS_SetPropertyInternal(JSContextHandle ctx, GCValue obj,
                           JSAtom prop, GCValue val, GCValue this_obj,
                           int flags);
static inline int JS_SetProperty(JSContextHandle ctx, GCValue this_obj,
                                 JSAtom prop, GCValue val)
{
    return JS_SetPropertyInternal(ctx, this_obj, prop, val, this_obj, JS_PROP_THROW);
}
int JS_SetPropertyUint32(JSContextHandle ctx, GCValue this_obj,
                         uint32_t idx, GCValue val);
int JS_SetPropertyInt64(JSContextHandle ctx, GCValue this_obj,
                        int64_t idx, GCValue val);
int JS_SetPropertyStr(JSContextHandle ctx, GCValue this_obj,
                      const char *prop, GCValue val);
int JS_HasProperty(JSContextHandle ctx, GCValue this_obj, JSAtom prop);
int JS_IsExtensible(JSContextHandle ctx, GCValue obj);
int JS_PreventExtensions(JSContextHandle ctx, GCValue obj);
int JS_DeleteProperty(JSContextHandle ctx, GCValue obj, JSAtom prop, int flags);
int JS_SetPrototype(JSContextHandle ctx, GCValue obj, GCValue proto_val);
GCValue JS_GetPrototype(JSContextHandle ctx, GCValue val);

#define JS_GPN_STRING_MASK  (1 << 0)
#define JS_GPN_SYMBOL_MASK  (1 << 1)
#define JS_GPN_PRIVATE_MASK (1 << 2)
/* only include the enumerable properties */
#define JS_GPN_ENUM_ONLY    (1 << 4)
/* set theJSPropertyEnum.is_enumerable field */
#define JS_GPN_SET_ENUM     (1 << 5)

int JS_GetOwnPropertyNames(JSContextHandle ctx, JSPropertyEnum **ptab,
                           uint32_t *plen, GCValue obj, int flags);
void JS_FreePropertyEnum(JSContextHandle ctx, JSPropertyEnum *tab,
                         uint32_t len);
int JS_GetOwnProperty(JSContextHandle ctx, JSPropertyDescriptor *desc,
                      GCValue obj, JSAtom prop);

GCValue JS_Call(JSContextHandle ctx, GCValue func_obj, GCValue this_obj,
                int argc, GCValue *argv);
GCValue JS_Invoke(JSContextHandle ctx, GCValue this_val, JSAtom atom,
                  int argc, GCValue *argv);
GCValue JS_CallConstructor(JSContextHandle ctx, GCValue func_obj,
                           int argc, GCValue *argv);
GCValue JS_CallConstructor2(JSContextHandle ctx, GCValue func_obj,
                            GCValue new_target,
                            int argc, GCValue *argv);
JS_BOOL JS_DetectModule(const char *input, size_t input_len);
/* 'input' must be zero terminated i.e. input[input_len] = '\0'. */
GCValue JS_Eval(JSContextHandle ctx, const char *input, size_t input_len,
                const char *filename, int eval_flags);
/* same as JS_Eval() but with an explicit 'this_obj' parameter */
GCValue JS_EvalThis(JSContextHandle ctx, GCValue this_obj,
                    const char *input, size_t input_len,
                    const char *filename, int eval_flags);
GCValue JS_GetGlobalObject(JSContextHandle ctx);
int JS_IsInstanceOf(JSContextHandle ctx, GCValue val, GCValue obj);
int JS_DefineProperty(JSContextHandle ctx, GCValue this_obj,
                      JSAtom prop, GCValue val,
                      GCValue getter, GCValue setter, int flags);
int JS_DefinePropertyValue(JSContextHandle ctx, GCValue this_obj,
                           JSAtom prop, GCValue val, int flags);
int JS_DefinePropertyValueUint32(JSContextHandle ctx, GCValue this_obj,
                                 uint32_t idx, GCValue val, int flags);
int JS_DefinePropertyValueStr(JSContextHandle ctx, GCValue this_obj,
                              const char *prop, GCValue val, int flags);
int JS_DefinePropertyGetSet(JSContextHandle ctx, GCValue this_obj,
                            JSAtom prop, GCValue getter, GCValue setter,
                            int flags);
/* GC-safe handle-based opaque data API */
void JS_SetOpaqueHandle(GCValue obj, GCHandle opaque_handle);
GCHandle JS_GetOpaqueHandle(GCValue obj, JSClassID class_id);
GCHandle JS_GetOpaqueHandle2(JSContextHandle ctx, GCValue obj, JSClassID class_id);

/* 'buf' must be zero terminated i.e. buf[buf_len] = '\0'. */
GCValue JS_ParseJSON(JSContextHandle ctx, const char *buf, size_t buf_len,
                     const char *filename);
#define JS_PARSE_JSON_EXT (1 << 0) /* allow extended JSON */
GCValue JS_ParseJSON2(JSContextHandle ctx, const char *buf, size_t buf_len,
                      const char *filename, int flags);
GCValue JS_JSONStringify(JSContextHandle ctx, GCValue obj,
                         GCValue replacer, GCValue space0);

typedef void JSFreeArrayBufferDataFunc(JSRuntimeHandle rt, void *opaque, void *ptr);
GCValue JS_NewArrayBuffer(JSContextHandle ctx, uint8_t *buf, size_t len,
                          JSFreeArrayBufferDataFunc *free_func, void *opaque,
                          JS_BOOL is_shared);
GCValue JS_NewArrayBufferCopy(JSContextHandle ctx, const uint8_t *buf, size_t len);
void JS_DetachArrayBuffer(JSContextHandle ctx, GCValue obj);
uint8_t *JS_GetArrayBuffer(JSContextHandle ctx, size_t *psize, GCValue obj);

typedef enum JSTypedArrayEnum {
    JS_TYPED_ARRAY_UINT8C = 0,
    JS_TYPED_ARRAY_INT8,
    JS_TYPED_ARRAY_UINT8,
    JS_TYPED_ARRAY_INT16,
    JS_TYPED_ARRAY_UINT16,
    JS_TYPED_ARRAY_INT32,
    JS_TYPED_ARRAY_UINT32,
    JS_TYPED_ARRAY_BIG_INT64,
    JS_TYPED_ARRAY_BIG_UINT64,
    JS_TYPED_ARRAY_FLOAT16,
    JS_TYPED_ARRAY_FLOAT32,
    JS_TYPED_ARRAY_FLOAT64,
} JSTypedArrayEnum;

GCValue JS_NewTypedArray(JSContextHandle ctx, int argc, GCValue *argv,
                         JSTypedArrayEnum array_type);
GCValue JS_GetTypedArrayBuffer(JSContextHandle ctx, GCValue obj,
                               size_t *pbyte_offset,
                               size_t *pbyte_length,
                               size_t *pbytes_per_element);
/* Note: JSSharedArrayBufferFunctions is defined in quickjs_types.h */
void JS_SetSharedArrayBufferFunctions(JSRuntimeHandle rt,
                                      const JSSharedArrayBufferFunctions *sf);

/* JSPromiseStateEnum is now defined in quickjs-internal.h */

GCValue JS_NewPromiseCapability(JSContextHandle ctx, GCValue *resolving_funcs);
JSPromiseStateEnum JS_PromiseState(JSContextHandle ctx, GCValue promise);
GCValue JS_PromiseResult(JSContextHandle ctx, GCValue promise);

/* is_handled = TRUE means that the rejection is handled */
/* JSHostPromiseRejectionTracker defined in quickjs_types.h */
void JS_SetHostPromiseRejectionTracker(JSRuntimeHandle rt, JSHostPromiseRejectionTracker *cb, void *opaque);

/* if can_block is TRUE, Atomics.wait() can be used */
void JS_SetCanBlock(JSRuntimeHandle rt, JS_BOOL can_block);
/* select which debug info is stripped from the compiled code */
#define JS_STRIP_SOURCE (1 << 0) /* strip source code */
#define JS_STRIP_DEBUG  (1 << 1) /* strip all debug info including source code */
void JS_SetStripInfo(JSRuntimeHandle rt, int flags);
int JS_GetStripInfo(JSRuntimeHandle rt);

/* set the [IsHTMLDDA] internal slot */
void JS_SetIsHTMLDDA(JSContextHandle ctx, GCValue obj);

typedef struct JSModuleDef JSModuleDef;

/* return the module specifier (allocated with gc_alloc) or NULL if
   exception */
/* JSModuleNormalizeFunc defined in quickjs_types.h */
/* Note: JSModuleLoaderFunc, JSModuleLoaderFunc2, JSModuleCheckSupportedImportAttributes
 * are defined in quickjs_types.h */
                                                   
/* module_normalize = NULL is allowed and invokes the default module
   filename normalizer */
void JS_SetModuleLoaderFunc(JSRuntimeHandle rt,
                            JSModuleNormalizeFunc *module_normalize,
                            JSModuleLoaderFunc *module_loader, void *opaque);
/* same as JS_SetModuleLoaderFunc but with attributes. if
   module_check_attrs = NULL, no attribute checking is done. */
void JS_SetModuleLoaderFunc2(JSRuntimeHandle rt,
                             JSModuleNormalizeFunc *module_normalize,
                             JSModuleLoaderFunc2 *module_loader,
                             JSModuleCheckSupportedImportAttributes *module_check_attrs,
                             void *opaque);
/* return the import.meta object of a module */
GCValue JS_GetImportMeta(JSContextHandle ctx, JSModuleDefHandle m);
JSAtom JS_GetModuleName(JSContextHandle ctx, JSModuleDefHandle m);
GCValue JS_GetModuleNamespace(JSContextHandle ctx, JSModuleDefHandle m);

/* JS Job support */
int JS_EnqueueJob(JSContextHandle ctx, JSJobFunc *job_func, int argc, GCValue *argv);

JS_BOOL JS_IsJobPending(JSRuntimeHandle rt);
int JS_ExecutePendingJob(JSRuntimeHandle rt, JSContextHandle *pctx);

/* Object Writer/Reader (currently only used to handle precompiled code) */
#define JS_WRITE_OBJ_BYTECODE  (1 << 0) /* allow function/module */
#define JS_WRITE_OBJ_BSWAP     (1 << 1) /* byte swapped output */
#define JS_WRITE_OBJ_SAB       (1 << 2) /* allow SharedArrayBuffer */
#define JS_WRITE_OBJ_REFERENCE (1 << 3) /* allow object references to
                                           encode arbitrary object
                                           graph */
uint8_t *JS_WriteObject(JSContextHandle ctx, size_t *psize, GCValue obj,
                        int flags);
uint8_t *JS_WriteObject2(JSContextHandle ctx, size_t *psize, GCValue obj,
                         int flags, uint8_t ***psab_tab, size_t *psab_tab_len);

#define JS_READ_OBJ_BYTECODE  (1 << 0) /* allow function/module */
#define JS_READ_OBJ_ROM_DATA  (1 << 1) /* avoid duplicating 'buf' data */
#define JS_READ_OBJ_SAB       (1 << 2) /* allow SharedArrayBuffer */
#define JS_READ_OBJ_REFERENCE (1 << 3) /* allow object references */
GCValue JS_ReadObject(JSContextHandle ctx, const uint8_t *buf, size_t buf_len,
                      int flags);
/* instantiate and evaluate a bytecode function. Only used when
   reading a script or module with JS_ReadObject() */
GCValue JS_EvalFunction(JSContextHandle ctx, GCValue fun_obj);
/* load the dependencies of the module 'obj'. Useful when JS_ReadObject()
   returns a module. */
int JS_ResolveModule(JSContextHandle ctx, GCValue obj);

/* only exported for os.Worker() */
JSAtom JS_GetScriptOrModuleName(JSContextHandle ctx, int n_stack_levels);
/* only exported for os.Worker() */
GCValue JS_LoadModule(JSContextHandle ctx, const char *basename,
                      const char *filename);

/* C function definition */
typedef enum JSCFunctionEnum {  /* XXX: should rename for namespace isolation */
    JS_CFUNC_generic,
    JS_CFUNC_generic_magic,
    JS_CFUNC_constructor,
    JS_CFUNC_constructor_magic,
    JS_CFUNC_constructor_or_func,
    JS_CFUNC_constructor_or_func_magic,
    JS_CFUNC_f_f,
    JS_CFUNC_f_f_f,
    JS_CFUNC_getter,
    JS_CFUNC_setter,
    JS_CFUNC_getter_magic,
    JS_CFUNC_setter_magic,
    JS_CFUNC_iterator_next,
} JSCFunctionEnum;

typedef union JSCFunctionType {
    JSCFunction *generic;
    GCValue (*generic_magic)(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic);
    JSCFunction *constructor;
    GCValue (*constructor_magic)(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv, int magic);
    JSCFunction *constructor_or_func;
    double (*f_f)(double);
    double (*f_f_f)(double, double);
    GCValue (*getter)(JSContextHandle ctx, GCValue this_val);
    GCValue (*setter)(JSContextHandle ctx, GCValue this_val, GCValue val);
    GCValue (*getter_magic)(JSContextHandle ctx, GCValue this_val, int magic);
    GCValue (*setter_magic)(JSContextHandle ctx, GCValue this_val, GCValue val, int magic);
    GCValue (*iterator_next)(JSContextHandle ctx, GCValue this_val,
                             int argc, GCValue *argv, int *pdone, int magic);
} JSCFunctionType;

GCValue JS_NewCFunction2(JSContextHandle ctx, JSCFunction *func,
                         const char *name,
                         int length, JSCFunctionEnum cproto, int magic);
GCValue JS_NewCFunctionData(JSContextHandle ctx, JSCFunctionData *func,
                            int length, int magic, int data_len,
                            GCValue *data);

static inline GCValue JS_NewCFunction(JSContextHandle ctx, JSCFunction *func, const char *name,
                                      int length)
{
    return JS_NewCFunction2(ctx, func, name, length, JS_CFUNC_generic, 0);
}

static inline GCValue JS_NewCFunctionMagic(JSContextHandle ctx, JSCFunctionMagic *func,
                                           const char *name,
                                           int length, JSCFunctionEnum cproto, int magic)
{
    /* Used to squelch a -Wcast-function-type warning. */
    JSCFunctionType ft = { .generic_magic = func };
    return JS_NewCFunction2(ctx, ft.generic, name, length, cproto, magic);
}
int JS_SetConstructor(JSContextHandle ctx, GCValue func_obj,
                      GCValue proto);

/* C property definition */

typedef struct JSCFunctionListEntry {
    const char *name;
    uint8_t prop_flags;
    uint8_t def_type;
    int16_t magic;
    union {
        struct {
            uint8_t length; /* XXX: should move outside union */
            uint8_t cproto; /* XXX: should move outside union */
            JSCFunctionType cfunc;
        } func;
        struct {
            JSCFunctionType get;
            JSCFunctionType set;
        } getset;
        struct {
            const char *name;
            int base;
        } alias;
        struct {
            const struct JSCFunctionListEntry *tab;
            int len;
        } prop_list;
        const char *str;
        int32_t i32;
        int64_t i64;
        double f64;
    } u;
} JSCFunctionListEntry;

#define JS_DEF_CFUNC          0
#define JS_DEF_CGETSET        1
#define JS_DEF_CGETSET_MAGIC  2
#define JS_DEF_PROP_STRING    3
#define JS_DEF_PROP_INT32     4
#define JS_DEF_PROP_INT64     5
#define JS_DEF_PROP_DOUBLE    6
#define JS_DEF_PROP_UNDEFINED 7
#define JS_DEF_OBJECT         8
#define JS_DEF_ALIAS          9
#define JS_DEF_PROP_ATOM     10
#define JS_DEF_PROP_BOOL     11

/* Note: c++ does not like nested designators */
/* Using all-designated initializers for C++20 compatibility (GCC 16+) */
#define JS_CFUNC_DEF(name1, length1, func1) { .name = name1, .prop_flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, .def_type = JS_DEF_CFUNC, .magic = 0, .u = { .func = { .length = length1, .cproto = JS_CFUNC_generic, .cfunc = { .generic = func1 } } } }
#define JS_CFUNC_MAGIC_DEF(name1, length1, func1, magic1) { .name = name1, .prop_flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, .def_type = JS_DEF_CFUNC, .magic = magic1, .u = { .func = { .length = length1, .cproto = JS_CFUNC_generic_magic, .cfunc = { .generic_magic = func1 } } } }
#define JS_CFUNC_SPECIAL_DEF(name1, length1, cproto1, func1) { .name = name1, .prop_flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, .def_type = JS_DEF_CFUNC, .magic = 0, .u = { .func = { .length = length1, .cproto = JS_CFUNC_ ## cproto1, .cfunc = { .cproto1 = func1 } } } }
#define JS_ITERATOR_NEXT_DEF(name1, length1, func1, magic1) { .name = name1, .prop_flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, .def_type = JS_DEF_CFUNC, .magic = magic1, .u = { .func = { .length = length1, .cproto = JS_CFUNC_iterator_next, .cfunc = { .iterator_next = func1 } } } }
#define JS_CGETSET_DEF(name1, fgetter1, fsetter1) { .name = name1, .prop_flags = JS_PROP_CONFIGURABLE, .def_type = JS_DEF_CGETSET, .magic = 0, .u = { .getset = { .get = { .getter = fgetter1 }, .set = { .setter = fsetter1 } } } }
#define JS_CGETSET_MAGIC_DEF(name1, fgetter1, fsetter1, magic1) { .name = name1, .prop_flags = JS_PROP_CONFIGURABLE, .def_type = JS_DEF_CGETSET_MAGIC, .magic = magic1, .u = { .getset = { .get = { .getter_magic = fgetter1 }, .set = { .setter_magic = fsetter1 } } } }
#define JS_PROP_STRING_DEF(name1, cstr1, prop_flags1) { .name = name1, .prop_flags = prop_flags1, .def_type = JS_DEF_PROP_STRING, .magic = 0, .u = { .str = cstr1 } }
#define JS_PROP_INT32_DEF(name1, val1, prop_flags1) { .name = name1, .prop_flags = prop_flags1, .def_type = JS_DEF_PROP_INT32, .magic = 0, .u = { .i32 = val1 } }
#define JS_PROP_INT64_DEF(name1, val1, prop_flags1) { .name = name1, .prop_flags = prop_flags1, .def_type = JS_DEF_PROP_INT64, .magic = 0, .u = { .i64 = val1 } }
#define JS_PROP_DOUBLE_DEF(name1, val1, prop_flags1) { .name = name1, .prop_flags = prop_flags1, .def_type = JS_DEF_PROP_DOUBLE, .magic = 0, .u = { .f64 = val1 } }
#define JS_PROP_UNDEFINED_DEF(name1, prop_flags1) { .name = name1, .prop_flags = prop_flags1, .def_type = JS_DEF_PROP_UNDEFINED, .magic = 0, .u = { .i32 = 0 } }
#define JS_PROP_ATOM_DEF(name1, val1, prop_flags1) { .name = name1, .prop_flags = prop_flags1, .def_type = JS_DEF_PROP_ATOM, .magic = 0, .u = { .i32 = val1 } }
#define JS_PROP_BOOL_DEF(name1, val1, prop_flags1) { .name = name1, .prop_flags = prop_flags1, .def_type = JS_DEF_PROP_BOOL, .magic = 0, .u = { .i32 = val1 } }
#define JS_OBJECT_DEF(name1, tab1, len1, prop_flags1) { .name = name1, .prop_flags = prop_flags1, .def_type = JS_DEF_OBJECT, .magic = 0, .u = { .prop_list = { .tab = tab1, .len = len1 } } }
#define JS_ALIAS_DEF(name1, from1) { .name = name1, .prop_flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, .def_type = JS_DEF_ALIAS, .magic = 0, .u = { .alias = { .name = from1, .base = -1 } } }
#define JS_ALIAS_BASE_DEF(name1, from1, base1) { .name = name1, .prop_flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, .def_type = JS_DEF_ALIAS, .magic = 0, .u = { .alias = { .name = from1, .base = base1 } } }

int JS_SetPropertyFunctionList(JSContextHandle ctx, GCValue obj,
                               const JSCFunctionListEntry *tab,
                               int len);

/* C module definition */

typedef int JSModuleInitFunc(JSContextHandle ctx, JSModuleDefHandle m);

JSModuleDefHandle JS_NewCModule(JSContextHandle ctx, const char *name_str,
                           JSModuleInitFunc *func);
/* can only be called before the module is instantiated */
int JS_AddModuleExport(JSContextHandle ctx, JSModuleDefHandle m, const char *name_str);
int JS_AddModuleExportList(JSContextHandle ctx, JSModuleDefHandle m,
                           const JSCFunctionListEntry *tab, int len);
/* can only be called after the module is instantiated */
int JS_SetModuleExport(JSContextHandle ctx, JSModuleDefHandle m, const char *export_name,
                       GCValue val);
int JS_SetModuleExportList(JSContextHandle ctx, JSModuleDefHandle m,
                           const JSCFunctionListEntry *tab, int len);
/* associate a GCValue to a C module */
int JS_SetModulePrivateValue(JSContextHandle ctx, JSModuleDefHandle m, GCValue val);
GCValue JS_GetModulePrivateValue(JSContextHandle ctx, JSModuleDefHandle m);
                        
/* debug value output */

typedef struct {
    JS_BOOL show_hidden : 8; /* only show enumerable properties */
    JS_BOOL raw_dump : 8; /* avoid doing autoinit and avoid any malloc() call (for internal use) */
    uint32_t max_depth; /* recurse up to this depth, 0 = no limit */
    uint32_t max_string_length; /* print no more than this length for
                                   strings, 0 = no limit */
    uint32_t max_item_count; /*  print no more than this count for
                                 arrays or objects, 0 = no limit */
} JSPrintValueOptions;

typedef void JSPrintValueWrite(void *opaque, const char *buf, size_t len);

void JS_PrintValueSetDefaultOptions(JSPrintValueOptions *options);
void JS_PrintValueRT(JSRuntimeHandle rt, JSPrintValueWrite *write_func, void *write_opaque,
                     GCValue val, const JSPrintValueOptions *options);
void JS_PrintValue(JSContextHandle ctx, JSPrintValueWrite *write_func, void *write_opaque,
                   GCValue val, const JSPrintValueOptions *options);

#undef js_unlikely
#undef js_force_inline

/* ============================================================================
 * JSObject Structure Definition - Needed for C++ JSObjectHandle
 * ============================================================================
 * This is the full definition of JSObject needed by the C++ wrapper class.
 * The structure uses handles for all GC-managed references.
 */

/* Forward declarations */
typedef struct JSShape JSShape;
typedef struct JSProperty JSProperty;
typedef struct JSObject JSObject;
typedef struct JSRegExp JSRegExp;

/* JSProperty is now defined in quickjs-internal.h */

/*
 * JSShapeProperty - Property entry in shape's property table
 */
typedef struct JSShapeProperty {
    uint32_t hash_next : 26; /* 0 if last in list */
    uint32_t flags : 6;   /* JS_PROP_XXX */
    JSAtom atom; /* JS_ATOM_NULL = free property entry */
} JSShapeProperty;

/*
 * JSShape - Object shape/structure definition
 */
struct JSShape {
    /* hash table of size hash_mask + 1 before the start of the
       structure (see prop_hash_end()). */
    /* true if the shape is inserted in the shape hash table. If not,
       JSShape.hash is not valid */
    uint8_t is_hashed;
    uint8_t hash_size; /* hash table size (power of 2), stored for GC dereference */
    uint32_t hash; /* current hash value */
    uint32_t prop_hash_mask;
    int prop_size; /* allocated properties */
    int prop_count; /* include deleted properties */
    int deleted_prop_count;
    GCHandle proto_handle; /* Handle to prototype object (GC_HANDLE_NULL if none) */
    GCHandle handle; /* Handle to this shape */
    JSShapeProperty prop[]; /* prop_size elements - C99 flexible array member */
};

/*
 * JSRegExp - Regular expression structure
 */
struct JSRegExp {
    GCHandle pattern_handle; /* Handle to JSString pattern */
    GCHandle bytecode_handle; /* Handle to JSString bytecode (contains flags) */
};

/*
 * JSObject - JavaScript object structure
 *
 * This is the full definition of JSObject.
 * The union members are organized by class_id.
 */
struct JSObject {
    /* JS Object flags */
    uint16_t class_id;           /* see JS_CLASS_x */
    uint8_t is_std_array_prototype : 1;  /* TRUE if array prototype is "normal" */
    uint8_t extensible : 1;
    uint8_t free_mark : 1;       /* only used when freeing objects with cycles */
    uint8_t is_exotic : 1;       /* TRUE if object has exotic property handlers */
    uint8_t fast_array : 1;      /* TRUE if u.array is used */
    uint8_t is_constructor : 1;  /* TRUE if object is a constructor function */
    uint8_t has_immutable_prototype : 1; /* cannot modify the prototype */
    uint8_t tmp_mark : 1;        /* used in JS_WriteObjectRec() */
    uint8_t is_HTMLDDA : 1;      /* specific annex B IsHtmlDDA behavior */
    uint8_t _padding : 7;        /* unused padding to align weakref_count */
    /* count the number of weak references to this object. The object
       structure is freed only if weakref_count = 0 */
    uint32_t weakref_count;
    /* atomic version counter for lock-free property-array resizes.
       Odd = resize in progress, even = stable. */
    uint32_t prop_version;
    GCHandle shape_handle; /* Handle to shape (prototype and property names + flag) */
    GCHandle prop_handle; /* Handle to prop array */
    union {
        GCHandle opaque_handle; /* GC-safe handle to opaque data */
        GCHandle bound_function_handle; /* JS_CLASS_BOUND_FUNCTION - handle to JSBoundFunction */
        GCHandle c_function_data_record_handle; /* JS_CLASS_C_FUNCTION_DATA - handle to JSCFunctionDataRecord */
        GCHandle for_in_iterator_handle; /* JS_CLASS_FOR_IN_ITERATOR - handle to JSForInIterator */
        GCHandle array_buffer_handle; /* JS_CLASS_ARRAY_BUFFER, JS_CLASS_SHARED_ARRAY_BUFFER - handle to JSArrayBuffer */
        GCHandle typed_array_handle; /* JS_CLASS_UINT8C_ARRAY..JS_CLASS_DATAVIEW - handle to JSTypedArray */
        GCHandle map_state_handle;   /* JS_CLASS_MAP..JS_CLASS_WEAKSET - handle to JSMapState */
        GCHandle map_iterator_data_handle; /* JS_CLASS_MAP_ITERATOR, JS_CLASS_SET_ITERATOR - handle to JSMapIteratorData */
        GCHandle array_iterator_data_handle; /* JS_CLASS_ARRAY_ITERATOR, JS_CLASS_STRING_ITERATOR - handle to JSArrayIteratorData */
        GCHandle regexp_string_iterator_data_handle; /* JS_CLASS_REGEXP_STRING_ITERATOR - handle to JSRegExpStringIteratorData */
        GCHandle generator_data_handle; /* JS_CLASS_GENERATOR - handle to JSGeneratorData */
        GCHandle iterator_concat_data_handle; /* JS_CLASS_ITERATOR_CONCAT - handle to JSIteratorConcatData */
        GCHandle iterator_helper_data_handle; /* JS_CLASS_ITERATOR_HELPER - handle to JSIteratorHelperData */
        GCHandle iterator_wrap_data_handle; /* JS_CLASS_ITERATOR_WRAP - handle to JSIteratorWrapData */
        GCHandle proxy_data_handle; /* JS_CLASS_PROXY - handle to JSProxyData */
        GCHandle promise_data_handle; /* JS_CLASS_PROMISE - handle to JSPromiseData */
        GCHandle promise_function_data_handle; /* JS_CLASS_PROMISE_RESOLVE_FUNCTION, JS_CLASS_PROMISE_REJECT_FUNCTION - handle to JSPromiseFunctionData */
        GCHandle async_function_data_handle; /* JS_CLASS_ASYNC_FUNCTION_RESOLVE, JS_CLASS_ASYNC_FUNCTION_REJECT - handle to JSAsyncFunctionStateHandle /
        GCHandle async_from_sync_iterator_data_handle; /* JS_CLASS_ASYNC_FROM_SYNC_ITERATOR - handle to JSAsyncFromSyncIteratorData */
        GCHandle async_generator_data_handle; /* JS_CLASS_ASYNC_GENERATOR - handle to JSAsyncGeneratorData */
        struct { /* JS_CLASS_BYTECODE_FUNCTION: 12/24 bytes */
            /* also used by JS_CLASS_GENERATOR_FUNCTION, JS_CLASS_ASYNC_FUNCTION and JS_CLASS_ASYNC_GENERATOR_FUNCTION */
            GCHandle function_bytecode_handle; /* handle to JSFunctionBytecode */
            GCHandle var_refs_handle; /* handle to array of JSVarRef handles */
            GCHandle home_object_handle; /* for 'super' access (GC_HANDLE_NULL if none) */
            GCHandle parent_frame_handle; /* parent stack frame at closure creation time (for lazy functions) */
        } func;
        struct { /* JS_CLASS_C_FUNCTION: 12/20 bytes */
            GCHandle realm_handle; /* handle to JSContext realm */
            JSCFunctionType c_function;
            uint8_t length;
            uint8_t cproto;
            int16_t magic;
        } cfunc;
        /* array part for fast arrays and typed arrays */
        struct { /* JS_CLASS_ARRAY, JS_CLASS_ARGUMENTS, JS_CLASS_MAPPED_ARGUMENTS, JS_CLASS_UINT8C_ARRAY..JS_CLASS_FLOAT64_ARRAY */
            union {
                uint32_t size;          /* JS_CLASS_ARRAY */
                GCHandle typed_array_handle; /* JS_CLASS_UINT8C_ARRAY..JS_CLASS_FLOAT64_ARRAY - handle to JSTypedArray */
            } u1;
            union {
                GCHandle values_handle; /* handle to GCValue array, JS_CLASS_ARRAY, JS_CLASS_ARGUMENTS */
                GCHandle var_refs_handle; /* handle to array of JSVarRef handles, JS_CLASS_MAPPED_ARGUMENTS */
                void *ptr;              /* JS_CLASS_UINT8C_ARRAY..JS_CLASS_FLOAT64_ARRAY */
                int8_t *int8_ptr;       /* JS_CLASS_INT8_ARRAY */
                uint8_t *uint8_ptr;     /* JS_CLASS_UINT8_ARRAY, JS_CLASS_UINT8C_ARRAY */
                int16_t *int16_ptr;     /* JS_CLASS_INT16_ARRAY */
                uint16_t *uint16_ptr;   /* JS_CLASS_UINT16_ARRAY */
                int32_t *int32_ptr;     /* JS_CLASS_INT32_ARRAY */
                uint32_t *uint32_ptr;   /* JS_CLASS_UINT32_ARRAY */
                int64_t *int64_ptr;     /* JS_CLASS_INT64_ARRAY */
                uint64_t *uint64_ptr;   /* JS_CLASS_UINT64_ARRAY */
                uint16_t *fp16_ptr;     /* JS_CLASS_FLOAT16_ARRAY */
                float *float_ptr;       /* JS_CLASS_FLOAT32_ARRAY */
                double *double_ptr;     /* JS_CLASS_FLOAT64_ARRAY */
            } u;
            uint32_t count; /* <= 2^31-1. 0 for a detached typed array */
        } array;    /* 12/20 bytes */
        JSRegExp regexp;    /* JS_CLASS_REGEXP: 8/16 bytes */
        GCValue object_data;    /* for JS_SetObjectData(): 8/16/16 bytes */
        struct JSGlobalObject {
            GCValue uninitialized_vars; /* hidden object containing the list of uninitialized variables */
        } global_object;
    } u;
    GCValue prototype;
};

#ifdef __cplusplus
} /* extern "C" { */
#endif

/* ============================================================================
 * C++ JSShapeHandle Class - Safe GC Handle Wrapper for JSShape
 * ============================================================================
 * 
 * This class wraps a GCHandle to provide safe access to JSShape instances.
 * The key feature is that it NEVER stores a raw JSShape pointer - instead,
 * it dereferences the handle on every property access to get a fresh pointer.
 * This ensures that the pointer is always valid even after GC compaction.
 * 
 * USAGE:
 *   JSShapeHandle sh = JS_SHAPE_GET_HANDLE(shape_handle);
 *   sh.set_proto_handle(proto);   // Dereferences handle each time
 *   int prop_count = sh.prop_count();  // Fresh dereference
 * 
 * IMPORTANT: Do NOT use JSShape* in new code. Always use JSShapeHandle.
 */

class JSShapeHandle {
private:
    GCHandle handle_;
    
    /* Get fresh pointer - called on every access */
    JSShape* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSShape*)gc_deref(handle_);
    }

public:
    /* Default constructor - null handle */
    JSShapeHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit JSShapeHandle(GCHandle handle) : handle_(handle) {}
    
    /* Copy constructor */
    JSShapeHandle(const JSShapeHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    JSShapeHandle(JSShapeHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    JSShapeHandle& operator=(const JSShapeHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    JSShapeHandle& operator=(JSShapeHandle&& other) noexcept {
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
    
    /* is_hashed access */
    uint8_t is_hashed() const {
        JSShape* p = get_ptr();
        return p ? p->is_hashed : 0;
    }
    
    void set_is_hashed(uint8_t val) {
        JSShape* p = get_ptr();
        if (p) p->is_hashed = val;
    }
    
    /* hash_size access */
    uint8_t hash_size() const {
        JSShape* p = get_ptr();
        return p ? p->hash_size : 0;
    }
    
    void set_hash_size(uint8_t val) {
        JSShape* p = get_ptr();
        if (p) p->hash_size = val;
    }
    
    /* hash access */
    uint32_t hash() const {
        JSShape* p = get_ptr();
        return p ? p->hash : 0;
    }
    
    void set_hash(uint32_t val) {
        JSShape* p = get_ptr();
        if (p) p->hash = val;
    }
    
    /* prop_hash_mask access */
    uint32_t prop_hash_mask() const {
        JSShape* p = get_ptr();
        return p ? p->prop_hash_mask : 0;
    }
    
    void set_prop_hash_mask(uint32_t val) {
        JSShape* p = get_ptr();
        if (p) p->prop_hash_mask = val;
    }
    
    /* prop_size access */
    int prop_size() const {
        JSShape* p = get_ptr();
        return p ? p->prop_size : 0;
    }
    
    void set_prop_size(int val) {
        JSShape* p = get_ptr();
        if (p) p->prop_size = val;
    }
    
    /* prop_count access */
    int prop_count() const {
        JSShape* p = get_ptr();
        return p ? p->prop_count : 0;
    }
    
    void set_prop_count(int val) {
        JSShape* p = get_ptr();
        if (p) p->prop_count = val;
    }
    
    /* deleted_prop_count access */
    int deleted_prop_count() const {
        JSShape* p = get_ptr();
        return p ? p->deleted_prop_count : 0;
    }
    
    void set_deleted_prop_count(int val) {
        JSShape* p = get_ptr();
        if (p) p->deleted_prop_count = val;
    }
    
    /* proto_handle access */
    GCHandle proto_handle() const {
        JSShape* p = get_ptr();
        return p ? p->proto_handle : GC_HANDLE_NULL;
    }
    
    void set_proto_handle(GCHandle val) {
        JSShape* p = get_ptr();
        if (p) {
            p->proto_handle = val;
            gc_write_barrier_for_heap_slot(&p->proto_handle, val);
        }
    }
    
    /* handle access - returns the handle stored in the shape itself */
    GCHandle shape_handle() const {
        JSShape* p = get_ptr();
        return p ? p->handle : GC_HANDLE_NULL;
    }
    
    void set_shape_handle(GCHandle val) {
        JSShape* p = get_ptr();
        if (p) {
            p->handle = val;
            gc_write_barrier_for_heap_slot(&p->handle, val);
        }
    }
    
    /* =========================================================================
     * Access to prop array (flexible array member)
     * ========================================================================= */
    
    /* Get pointer to prop array - use with caution, pointer is temporary */
    JSShapeProperty* prop() const {
        JSShape* p = get_ptr();
        return p ? p->prop : nullptr;
    }
    
    /* Get a specific property at index */
    JSShapeProperty get_prop(int index) const {
        JSShape* p = get_ptr();
        if (p && index >= 0 && index < p->prop_size) {
            return p->prop[index];
        }
        return JSShapeProperty{};
    }
    
    /* Set a specific property at index */
    void set_prop(int index, const JSShapeProperty& prop_val) {
        JSShape* p = get_ptr();
        if (p && index >= 0 && index < p->prop_size) {
            p->prop[index] = prop_val;
        }
    }
    
    /* =========================================================================
     * Conversion operators for interoperability
     * ========================================================================= */
    
    /* Convert to GCHandle for handle-based APIs */
    operator GCHandle() const {
        return handle_;
    }
    
    /* Comparison operators */
    bool operator==(const JSShapeHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSShapeHandle& other) const {
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

/* Helper to create JSShapeHandle from GCHandle */
static inline JSShapeHandle JS_SHAPE_GET_HANDLE(GCHandle handle) {
    return JSShapeHandle(handle);
}

/* ============================================================================
 * JSString - JavaScript string structure
 * ============================================================================
 *
 * This is the full definition of JSString, placed here so that
 * JSStringHandle can inline its accessors for performance.
 */
struct JSString {
    /* IMPORTANT: Keep fixed-size fields separate from flexible array.
     * hash_next was moved here to avoid overlap with str8[0] */
    uint32_t hash_next; /* atom_index for JS_ATOM_TYPE_SYMBOL - MUST BE FIRST */
    uint32_t len : 31;
    uint32_t is_wide_char : 1; /* 0 = 8 bits, 1 = 16 bits characters */
    /* for JS_ATOM_TYPE_SYMBOL: hash = weakref_count, atom_type = 3,
       for JS_ATOM_TYPE_PRIVATE: hash = JS_ATOM_HASH_PRIVATE, atom_type = 3
       XXX: could change encoding to have one more bit in hash */
    uint32_t hash : 30;
    uint32_t atom_type : 2; /* != 0 if atom, JS_ATOM_TYPE_x */
    /* Note: strings are tracked via GC type bucket for DUMP_LEAKS, not intrusive list */
    union {
        uint8_t str8[0]; /* 8 bit strings will get an extra null terminator */
        uint16_t str16[0];
    } u;
};

/* ============================================================================
 * C++ JSStringHandle Class - Safe GC Handle Wrapper
 * ============================================================================
 * 
 * This class wraps a GCHandle to provide safe access to JSString instances.
 * The key feature is that it NEVER stores a raw JSString pointer - instead,
 * it dereferences the handle on every property access to get a fresh pointer.
 * This ensures that the pointer is always valid even after GC compaction.
 * 
 * USAGE:
 *   JSStringHandle str = JS_VALUE_GET_STRING_HANDLE(val);
 *   uint32_t len = str.len();           // Fresh dereference
 *   uint8_t is_wide = str.is_wide_char(); // Fresh dereference
 * 
 * IMPORTANT: Do NOT use JSString* in new code. Always use JSStringHandle.
 */

class JSStringHandle {
private:
    GCHandle handle_;
    
    /* Get fresh pointer - called on every access */
    JSString* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSString*)gc_deref(handle_);
    }

public:
    /* Default constructor - null handle */
    JSStringHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit JSStringHandle(GCHandle handle) : handle_(handle) {}
    
    /* Copy constructor */
    JSStringHandle(const JSStringHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    JSStringHandle(JSStringHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    JSStringHandle& operator=(const JSStringHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    JSStringHandle& operator=(JSStringHandle&& other) noexcept {
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
    
    /* Get raw pointer - USE WITH CAUTION (for C-style operations) */
    JSString* ptr() const {
        return get_ptr();
    }
    
    /* =========================================================================
     * Property accessors - each dereferences handle to get fresh pointer
     * ========================================================================= */
    
    /* hash_next access */
    uint32_t hash_next() const {
        JSString* p = get_ptr();
        return p ? p->hash_next : 0;
    }
    
    void set_hash_next(uint32_t val) {
        JSString* p = get_ptr();
        if (p) p->hash_next = val;
    }
    
    /* atom_index access - alias for hash_next, used for symbols */
    uint32_t atom_index() const {
        JSString* p = get_ptr();
        return p ? p->hash_next : 0;
    }
    
    void set_atom_index(uint32_t val) {
        JSString* p = get_ptr();
        if (p) p->hash_next = val;
    }
    
    /* len access */
    uint32_t len() const {
        JSString* p = get_ptr();
        return p ? p->len : 0;
    }
    
    void set_len(uint32_t val) {
        JSString* p = get_ptr();
        if (p) p->len = val;
    }
    
    /* is_wide_char access */
    uint8_t is_wide_char() const {
        JSString* p = get_ptr();
        return p ? p->is_wide_char : 0;
    }
    
    void set_is_wide_char(uint8_t val) {
        JSString* p = get_ptr();
        if (p) p->is_wide_char = val;
    }
    
    /* hash access */
    uint32_t hash() const {
        JSString* p = get_ptr();
        return p ? p->hash : 0;
    }
    
    void set_hash(uint32_t val) {
        JSString* p = get_ptr();
        if (p) p->hash = val;
    }
    
    /* atom_type access */
    uint8_t atom_type() const {
        JSString* p = get_ptr();
        return p ? p->atom_type : 0;
    }
    
    void set_atom_type(uint8_t val) {
        JSString* p = get_ptr();
        if (p) p->atom_type = val;
    }
    
    /* =========================================================================
     * String data accessors - for accessing string content safely
     * ========================================================================= */
    
    /* Get pointer to 8-bit string data - USE WITH CAUTION, prefer get_char() */
    uint8_t* str8() const {
        JSString* p = get_ptr();
        return p ? p->u.str8 : nullptr;
    }
    
    /* Get pointer to 16-bit string data - USE WITH CAUTION, prefer get_char() */
    uint16_t* str16() const {
        JSString* p = get_ptr();
        return p ? p->u.str16 : nullptr;
    }
    
    /* Get character at index - SAFE, dereferences fresh pointer */
    int get_char(int idx) const {
        JSString* p = get_ptr();
        if (!p || idx < 0 || (uint32_t)idx >= p->len) return 0;
        return p->is_wide_char ? p->u.str16[idx] : p->u.str8[idx];
    }
    
    /* Get character at index (const version) - SAFE */
    int char_at(int idx) const {
        return get_char(idx);
    }
    
#ifdef DUMP_LEAKS
    /* link access - returns pointer to list_head for list operations */
    struct list_head* link_ptr() {
        JSString* p = get_ptr();
        return p ? &p->link : nullptr;
    }
#endif
    
    /* =========================================================================
     * Conversion operators for interoperability
     * ========================================================================= */
    
    /* Convert to GCHandle for handle-based APIs */
    operator GCHandle() const {
        return handle_;
    }
    
    /* Comparison operators */
    bool operator==(const JSStringHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSStringHandle& other) const {
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
 * JSStringRope - Rope string structure for concatenated strings
 * 
 * Rope strings are used for efficient string concatenation. Instead of
 * copying string data immediately, ropes store left and right references
 * to other strings (which may also be ropes). The actual string data is
 * only flattened when needed.
 */
struct JSStringRope {
    uint32_t len;           /* Total length of the rope */
    uint8_t is_wide_char;   /* 0 = 8 bits, 1 = 16 bits */
    uint8_t depth;          /* Max depth of the rope tree */
    GCValue left;           /* Left string component (GC managed) */
    GCValue right;          /* Right string component (GC managed) */
};

/* ============================================================================
 * C++ JSStringRopeHandle Class - Safe GC Handle Wrapper
 * ============================================================================
 * 
 * This class wraps a GCHandle to provide safe access to JSStringRope instances.
 * The key feature is that it NEVER stores a raw JSStringRope pointer - instead,
 * it dereferences the handle on every property access to get a fresh pointer.
 * This ensures that the pointer is always valid even after GC compaction.
 * 
 * USAGE:
 *   JSStringRopeHandle rope = JS_VALUE_GET_STRING_ROPE_HANDLE(val);
 *   GCValue left = rope.left();   // Dereferences handle to get fresh pointer
 *   rope.set_depth(new_depth);    // Dereferences handle to set value
 */

class JSStringRopeHandle {
private:
    GCHandle handle_;
    
    /* Get fresh pointer - called on every access */
    JSStringRope* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSStringRope*)gc_deref(handle_);
    }

public:
    /* Default constructor - null handle */
    JSStringRopeHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit JSStringRopeHandle(GCHandle handle) : handle_(handle) {}
    
    /* Copy constructor */
    JSStringRopeHandle(const JSStringRopeHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    JSStringRopeHandle(JSStringRopeHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    JSStringRopeHandle& operator=(const JSStringRopeHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    JSStringRopeHandle& operator=(JSStringRopeHandle&& other) noexcept {
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
    
    /* len access */
    uint32_t len() const {
        JSStringRope* p = get_ptr();
        return p ? p->len : 0;
    }
    
    void set_len(uint32_t val) {
        JSStringRope* p = get_ptr();
        if (p) p->len = val;
    }
    
    /* is_wide_char access */
    uint8_t is_wide_char() const {
        JSStringRope* p = get_ptr();
        return p ? p->is_wide_char : 0;
    }
    
    void set_is_wide_char(uint8_t val) {
        JSStringRope* p = get_ptr();
        if (p) p->is_wide_char = val;
    }
    
    /* depth access */
    uint8_t depth() const {
        JSStringRope* p = get_ptr();
        return p ? p->depth : 0;
    }
    
    void set_depth(uint8_t val) {
        JSStringRope* p = get_ptr();
        if (p) p->depth = val;
    }
    
    /* left access */
    GCValue left() const {
        JSStringRope* p = get_ptr();
        return p ? p->left : GC_UNDEFINED;
    }
    
    void set_left(const GCValue& val) {
        JSStringRope* p = get_ptr();
        if (p) {
            p->left = val;
            gc_write_barrier_for_heap_slot(&p->left, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* right access */
    GCValue right() const {
        JSStringRope* p = get_ptr();
        return p ? p->right : GC_UNDEFINED;
    }
    
    void set_right(const GCValue& val) {
        JSStringRope* p = get_ptr();
        if (p) {
            p->right = val;
            gc_write_barrier_for_heap_slot(&p->right, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* =========================================================================
     * String data accessors - for accessing string content safely
     * ========================================================================= */
    
    /* Get pointer to 8-bit string data - USE WITH CAUTION, prefer get_char() */
    uint8_t* str8() const {
        JSStringRope* p = get_ptr();
        return p ? (uint8_t*)p : nullptr;
    }
    
    /* Get pointer to 16-bit string data - USE WITH CAUTION, prefer get_char() */
    uint16_t* str16() const {
        JSStringRope* p = get_ptr();
        return p ? (uint16_t*)p : nullptr;
    }
    
    /* Get character at index - SAFE, dereferences fresh pointer */
    int get_char(int idx) const {
        JSStringRope* p = get_ptr();
        if (!p || idx < 0 || (uint32_t)idx >= p->len) return 0;
        return p->is_wide_char ? ((uint16_t*)p)[idx] : ((uint8_t*)p)[idx];
    }
    
    /* Get character at index (const version) - SAFE */
    int char_at(int idx) const {
        return get_char(idx);
    }
    
    /* =========================================================================
     * Conversion operators for interoperability
     * ========================================================================= */
    
    /* Convert to GCHandle for handle-based APIs */
    operator GCHandle() const {
        return handle_;
    }
    
    /* Comparison operators */
    bool operator==(const JSStringRopeHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSStringRopeHandle& other) const {
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

/* Helper to create JSStringRopeHandle from JSValue (GCValue) */
static inline JSStringRopeHandle JS_VALUE_GET_STRING_ROPE_HANDLE(GCValue val) {
    if (GC_VALUE_GET_TAG(val) == JS_TAG_STRING_ROPE) {
        return JSStringRopeHandle(GC_VALUE_GET_HANDLE(val));
    }
    return JSStringRopeHandle();
}

/* Helper to convert JSStringRopeHandle to GCValue */
static inline GCValue JS_MKSTRING_ROPE(JSStringRopeHandle rope) {
    return GC_MKHANDLE(JS_TAG_STRING_ROPE, rope.handle());
}

/* Helper to create JSStringHandle from JSValue (GCValue) */
static inline JSStringHandle JS_VALUE_GET_STRING_HANDLE(GCValue val) {
    if (GC_VALUE_GET_TAG(val) == JS_TAG_STRING) {
        return JSStringHandle(GC_VALUE_GET_HANDLE(val));
    }
    return JSStringHandle();
}

/* Helper to convert JSStringHandle to GCValue */
static inline GCValue JS_MKSTRING(JSStringHandle str) {
    return GC_MKHANDLE(JS_TAG_STRING, str.handle());
}

/* ============================================================================
 * C++ JSObjectHandle Class - Safe GC Handle Wrapper
 * ============================================================================
 * 
 * This class wraps a GCHandle to provide safe access to JSObject instances.
 * The key feature is that it NEVER stores a raw JSObject pointer - instead,
 * it dereferences the handle on every property access to get a fresh pointer.
 * This ensures that the pointer is always valid even after GC compaction.
 * 
 * USAGE:
 *   JSObjectHandle obj = JS_VALUE_GET_OBJ_HANDLE(val);
 *   obj->shape_handle = new_shape_handle;  // Dereferences handle each time
 *   uint16_t class_id = obj.class_id;      // Fresh dereference
 * 
 * IMPORTANT: Do NOT use JSObject* in new code. Always use JSObjectHandle.
 */

class JSObjectHandle {
private:
    GCHandle handle_;
    
    /* Get fresh pointer - called on every access */
    JSObject* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (JSObject*)gc_deref(handle_);
    }

public:
    /* Default constructor - null handle */
    JSObjectHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit JSObjectHandle(GCHandle handle) : handle_(handle) {}
    
    /* Copy constructor */
    JSObjectHandle(const JSObjectHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    JSObjectHandle(JSObjectHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    JSObjectHandle& operator=(const JSObjectHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    JSObjectHandle& operator=(JSObjectHandle&& other) noexcept {
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
    
    /* class_id access */
    uint16_t class_id() const {
        JSObject* p = get_ptr();
        return p ? p->class_id : 0;
    }
    
    void set_class_id(uint16_t id) {
        JSObject* p = get_ptr();
        if (p) p->class_id = id;
    }
    
    /* shape_handle access (atomic load/store for thread-safe transitions) */
    GCHandle shape_handle() const {
        if (handle_ == GC_HANDLE_NULL) return GC_HANDLE_NULL;
        JSObject* ptr = (JSObject*)gc_deref(handle_);
        if (!ptr) return GC_HANDLE_NULL;
        return atomic_load_u32((volatile uint32_t *)&ptr->shape_handle);
    }
    
    void set_shape_handle(GCHandle shape) {
        JSObject* p = get_ptr();
        if (p) {
            atomic_store_u32((volatile uint32_t *)&p->shape_handle, shape);
            gc_write_barrier_for_heap_slot(&p->shape_handle, shape);
        }
    }
    
    /* prop_handle access (atomic load/store for lock-free resizes) */
    GCHandle prop_handle() const {
        if (handle_ == GC_HANDLE_NULL) return GC_HANDLE_NULL;
        JSObject* ptr = (JSObject*)gc_deref(handle_);
        if (!ptr) return GC_HANDLE_NULL;
        return atomic_load_u32((volatile uint32_t *)&ptr->prop_handle);
    }
    
    void set_prop_handle(GCHandle prop) {
        JSObject* p = get_ptr();
        if (p) {
            atomic_store_u32((volatile uint32_t *)&p->prop_handle, prop);
            gc_write_barrier_for_heap_slot(&p->prop_handle, prop);
        }
    }
    
    /* prop_version access (atomic, for lock-free property-array resizes) */
    uint32_t prop_version() const {
        if (handle_ == GC_HANDLE_NULL) return 0;
        JSObject* ptr = (JSObject*)gc_deref(handle_);
        if (!ptr) return 0;
        return atomic_load_u32((volatile uint32_t *)&ptr->prop_version);
    }
    
    void set_prop_version(uint32_t v) {
        JSObject* p = get_ptr();
        if (p) {
            atomic_store_u32((volatile uint32_t *)&p->prop_version, v);
        }
    }
    
    uint32_t prop_version_fetch_add(uint32_t delta) {
        JSObject* p = get_ptr();
        if (p) {
            return atomic_fetch_add_u32((volatile uint32_t *)&p->prop_version, delta);
        }
        return 0;
    }
    
    /* prop_ptr - returns raw pointer to JSProperty array (use with caution) */
    JSProperty* prop_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        JSObject* ptr = (JSObject*)gc_deref(handle_);
        if (!ptr) return nullptr;
        return (JSProperty*)gc_deref(ptr->prop_handle);
    }
    
    /* prototype access */
    GCValue prototype() const {
        JSObject* p = get_ptr();
        return p ? p->prototype : GC_UNDEFINED;
    }
    
    void set_prototype(const GCValue& proto) {
        JSObject* p = get_ptr();
        if (p) {
            p->prototype = proto;
            gc_write_barrier_for_heap_slot(&p->prototype, GC_VALUE_GET_HANDLE(proto));
        }
    }
    
    /* Boolean flag accessors */
    bool is_std_array_prototype() const {
        JSObject* p = get_ptr();
        return p ? p->is_std_array_prototype : false;
    }
    
    void set_is_std_array_prototype(bool val) {
        JSObject* p = get_ptr();
        if (p) p->is_std_array_prototype = val;
    }
    
    bool free_mark() const {
        JSObject* p = get_ptr();
        return p ? p->free_mark : false;
    }
    
    void set_free_mark(bool val) {
        JSObject* p = get_ptr();
        if (p) p->free_mark = val;
    }
    
    bool is_exotic() const {
        JSObject* p = get_ptr();
        return p ? p->is_exotic : false;
    }
    
    void set_is_exotic(bool val) {
        JSObject* p = get_ptr();
        if (p) p->is_exotic = val;
    }
    
    bool fast_array() const {
        JSObject* p = get_ptr();
        return p ? p->fast_array : false;
    }
    
    void set_fast_array(bool val) {
        JSObject* p = get_ptr();
        if (p) p->fast_array = val;
    }
    
    bool extensible() const {
        JSObject* p = get_ptr();
        return p ? p->extensible : false;
    }
    
    void set_extensible(bool val) {
        JSObject* p = get_ptr();
        if (p) p->extensible = val;
    }
    
    bool is_constructor() const {
        JSObject* p = get_ptr();
        return p ? p->is_constructor : false;
    }
    
    void set_is_constructor(bool val) {
        JSObject* p = get_ptr();
        if (p) p->is_constructor = val;
    }
    
    bool has_immutable_prototype() const {
        JSObject* p = get_ptr();
        return p ? p->has_immutable_prototype : false;
    }
    
    void set_has_immutable_prototype(bool val) {
        JSObject* p = get_ptr();
        if (p) p->has_immutable_prototype = val;
    }
    
    bool tmp_mark() const {
        JSObject* p = get_ptr();
        return p ? p->tmp_mark : false;
    }
    
    void set_tmp_mark(bool val) {
        JSObject* p = get_ptr();
        if (p) p->tmp_mark = val;
    }
    
    bool is_HTMLDDA() const {
        JSObject* p = get_ptr();
        return p ? p->is_HTMLDDA : false;
    }
    
    void set_is_HTMLDDA(bool val) {
        JSObject* p = get_ptr();
        if (p) p->is_HTMLDDA = val;
    }
    
    /* Union field access - u.opaque_handle (GC-safe handle to opaque data) */
    GCHandle opaque_handle() const {
        JSObject* p = get_ptr();
        return p ? p->u.opaque_handle : GC_HANDLE_NULL;
    }
    
    void set_opaque_handle(GCHandle handle) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.opaque_handle = handle;
            gc_write_barrier_for_heap_slot(&p->u.opaque_handle, handle);
        }
    }
    
    /* Union array field access - u.array.u.ptr */
    void* array_ptr() const {
        JSObject* p = get_ptr();
        return p ? p->u.array.u.ptr : nullptr;
    }
    
    void set_array_ptr(void* ptr) {
        JSObject* p = get_ptr();
        if (p) p->u.array.u.ptr = ptr;
    }
    
    /* Union array field access - u.array.u.values_handle */
    GCHandle array_values_handle() const {
        JSObject* p = get_ptr();
        return p ? p->u.array.u.values_handle : GC_HANDLE_NULL;
    }
    
    void set_array_values_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.array.u.values_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.array.u.values_handle, h);
        }
    }
    
    /* array_values_ptr - returns raw pointer to GCValue array (use with caution) */
    GCValue* array_values_ptr() const {
        JSObject* p = get_ptr();
        if (!p) return nullptr;
        return (GCValue*)gc_deref(p->u.array.u.values_handle);
    }
    
    /* Union array field access - u.array.count */
    uint32_t array_count() const {
        JSObject* p = get_ptr();
        return p ? p->u.array.count : 0;
    }
    
    void set_array_count(uint32_t count) {
        JSObject* p = get_ptr();
        if (p) p->u.array.count = count;
    }
    
    /* Union array field access - u.array.u1.size */
    uint32_t array_size() const {
        JSObject* p = get_ptr();
        return p ? p->u.array.u1.size : 0;
    }
    
    void set_array_size(uint32_t size) {
        JSObject* p = get_ptr();
        if (p) p->u.array.u1.size = size;
    }
    
    /* Union regexp field access - u.regexp.pattern_handle */
    GCHandle regexp_pattern_handle() const {
        JSObject* p = get_ptr();
        return p ? p->u.regexp.pattern_handle : GC_HANDLE_NULL;
    }
    
    void set_regexp_pattern_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.regexp.pattern_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.regexp.pattern_handle, h);
        }
    }
    
    /* Union regexp field access - u.regexp.bytecode_handle */
    GCHandle regexp_bytecode_handle() const {
        JSObject* p = get_ptr();
        return p ? p->u.regexp.bytecode_handle : GC_HANDLE_NULL;
    }
    
    void set_regexp_bytecode_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.regexp.bytecode_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.regexp.bytecode_handle, h);
        }
    }
    
    /* Union global_object field access - u.global_object.uninitialized_vars */
    GCValue global_object_uninitialized_vars() const {
        JSObject* p = get_ptr();
        return p ? p->u.global_object.uninitialized_vars : GC_UNDEFINED;
    }
    
    void set_global_object_uninitialized_vars(const GCValue& val) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.global_object.uninitialized_vars = val;
            gc_write_barrier_for_heap_slot(&p->u.global_object.uninitialized_vars, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* Union object_data field access */
    GCValue object_data() const {
        JSObject* p = get_ptr();
        return p ? p->u.object_data : GC_UNDEFINED;
    }
    
    void set_object_data(const GCValue& val) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.object_data = val;
            gc_write_barrier_for_heap_slot(&p->u.object_data, GC_VALUE_GET_HANDLE(val));
        }
    }
    
    /* Union array_iterator_data_handle access - returns typed handle */
    JSArrayIteratorDataHandle array_iterator_data_handle() const {
        JSObject* p = get_ptr();
        return JSArrayIteratorDataHandle(p ? p->u.array_iterator_data_handle : GC_HANDLE_NULL);
    }
    
    void set_array_iterator_data_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.array_iterator_data_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.array_iterator_data_handle, h);
        }
    }
    
    /* Union iterator_wrap_data_handle access - returns typed handle */
    JSIteratorWrapDataHandle iterator_wrap_data_handle() const {
        JSObject* p = get_ptr();
        return JSIteratorWrapDataHandle(p ? p->u.iterator_wrap_data_handle : GC_HANDLE_NULL);
    }
    
    void set_iterator_wrap_data_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.iterator_wrap_data_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.iterator_wrap_data_handle, h);
        }
    }
    
    /* Union iterator_concat_data_handle access - returns typed handle */
    JSIteratorConcatDataHandle iterator_concat_data_handle() const {
        JSObject* p = get_ptr();
        return JSIteratorConcatDataHandle(p ? p->u.iterator_concat_data_handle : GC_HANDLE_NULL);
    }
    
    void set_iterator_concat_data_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.iterator_concat_data_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.iterator_concat_data_handle, h);
        }
    }
    
    /* Union iterator_helper_data_handle access - returns typed handle */
    JSIteratorHelperDataHandle iterator_helper_data_handle() const {
        JSObject* p = get_ptr();
        return JSIteratorHelperDataHandle(p ? p->u.iterator_helper_data_handle : GC_HANDLE_NULL);
    }
    
    void set_iterator_helper_data_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.iterator_helper_data_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.iterator_helper_data_handle, h);
        }
    }
    
    /* Union regexp_string_iterator_data_handle access - returns typed handle */
    JSRegExpStringIteratorDataHandle regexp_string_iterator_data_handle() const {
        JSObject* p = get_ptr();
        return JSRegExpStringIteratorDataHandle(p ? p->u.regexp_string_iterator_data_handle : GC_HANDLE_NULL);
    }
    
    void set_regexp_string_iterator_data_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.regexp_string_iterator_data_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.regexp_string_iterator_data_handle, h);
        }
    }
    
    /* Union map_state_handle access - returns typed handle */
    JSMapStateHandle map_state_handle() const {
        JSObject* p = get_ptr();
        return JSMapStateHandle(p ? p->u.map_state_handle : GC_HANDLE_NULL);
    }
    
    void set_map_state_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.map_state_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.map_state_handle, h);
        }
    }
    
    /* Union map_iterator_data_handle access - returns typed handle */
    JSMapIteratorDataHandle map_iterator_data_handle() const {
        JSObject* p = get_ptr();
        return JSMapIteratorDataHandle(p ? p->u.map_iterator_data_handle : GC_HANDLE_NULL);
    }
    
    void set_map_iterator_data_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.map_iterator_data_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.map_iterator_data_handle, h);
        }
    }
    
    /* Union promise_function_data_handle access - returns typed handle */
    JSPromiseFunctionDataHandle promise_function_data_handle() const {
        JSObject* p = get_ptr();
        return JSPromiseFunctionDataHandle(p ? p->u.promise_function_data_handle : GC_HANDLE_NULL);
    }
    
    void set_promise_function_data_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.promise_function_data_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.promise_function_data_handle, h);
        }
    }
    
    /* Union func field access - u.func.function_bytecode_handle */
    GCHandle func_bytecode_handle() const {
        JSObject* p = get_ptr();
        return p ? p->u.func.function_bytecode_handle : GC_HANDLE_NULL;
    }
    
    void set_func_bytecode_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.func.function_bytecode_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.func.function_bytecode_handle, h);
        }
    }
    
    /* Union func field access - u.func.var_refs_handle */
    GCHandle func_var_refs_handle() const {
        JSObject* p = get_ptr();
        return p ? p->u.func.var_refs_handle : GC_HANDLE_NULL;
    }
    
    void set_func_var_refs_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.func.var_refs_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.func.var_refs_handle, h);
        }
    }
    
    /* Union func field access - u.func.home_object_handle */
    GCHandle func_home_object_handle() const {
        JSObject* p = get_ptr();
        return p ? p->u.func.home_object_handle : GC_HANDLE_NULL;
    }
    
    void set_func_home_object_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.func.home_object_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.func.home_object_handle, h);
        }
    }
    
    /* Union func field access - u.func.parent_frame_handle */
    GCHandle func_parent_frame_handle() const {
        JSObject* p = get_ptr();
        return p ? p->u.func.parent_frame_handle : GC_HANDLE_NULL;
    }
    
    void set_func_parent_frame_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.func.parent_frame_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.func.parent_frame_handle, h);
        }
    }
    
    /* Union bound_function_handle access - returns typed handle */
    JSBoundFunctionHandle bound_function_handle() const {
        JSObject* p = get_ptr();
        return JSBoundFunctionHandle(p ? p->u.bound_function_handle : GC_HANDLE_NULL);
    }
    
    void set_bound_function_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.bound_function_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.bound_function_handle, h);
        }
    }
    
    /* Union for_in_iterator_handle access - returns typed handle */
    JSForInIteratorHandle for_in_iterator_handle() const {
        JSObject* p = get_ptr();
        return JSForInIteratorHandle(p ? p->u.for_in_iterator_handle : GC_HANDLE_NULL);
    }
    
    void set_for_in_iterator_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.for_in_iterator_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.for_in_iterator_handle, h);
        }
    }
    
    /* Union array field access - u.array.u.var_refs_handle */
    GCHandle array_var_refs_handle() const {
        JSObject* p = get_ptr();
        return p ? p->u.array.u.var_refs_handle : GC_HANDLE_NULL;
    }
    
    void set_array_var_refs_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.array.u.var_refs_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.array.u.var_refs_handle, h);
        }
    }
    
    /* Union generator_data_handle access - returns typed handle */
    JSGeneratorDataHandle generator_data_handle() const {
        JSObject* p = get_ptr();
        return JSGeneratorDataHandle(p ? p->u.generator_data_handle : GC_HANDLE_NULL);
    }
    
    void set_generator_data_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.generator_data_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.generator_data_handle, h);
        }
    }
    
    /* Union async_generator_data_handle access - returns typed handle */
    JSAsyncGeneratorDataHandle async_generator_data_handle() const {
        JSObject* p = get_ptr();
        return JSAsyncGeneratorDataHandle(p ? p->u.async_generator_data_handle : GC_HANDLE_NULL);
    }
    
    void set_async_generator_data_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.async_generator_data_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.async_generator_data_handle, h);
        }
    }
    
    /* Union async_function_data_handle access */
    GCHandle async_function_data_handle() const {
        JSObject* p = get_ptr();
        return p ? p->u.async_function_data_handle : GC_HANDLE_NULL;
    }
    
    void set_async_function_data_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.async_function_data_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.async_function_data_handle, h);
        }
    }
    
    /* Union c_function_data_record_handle access - returns typed handle */
    JSCFunctionDataRecordHandle c_function_data_record_handle() const {
        JSObject* p = get_ptr();
        return JSCFunctionDataRecordHandle(p ? p->u.c_function_data_record_handle : GC_HANDLE_NULL);
    }
    
    void set_c_function_data_record_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.c_function_data_record_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.c_function_data_record_handle, h);
        }
    }
    
    /* Union array_buffer_handle access - returns typed handle */
    JSArrayBufferHandle array_buffer_handle() const {
        JSObject* p = get_ptr();
        return JSArrayBufferHandle(p ? p->u.array_buffer_handle : GC_HANDLE_NULL);
    }
    
    void set_array_buffer_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.array_buffer_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.array_buffer_handle, h);
        }
    }
    
    /* Union typed_array_handle access - returns typed handle */
    JSTypedArrayHandle typed_array_handle() const {
        JSObject* p = get_ptr();
        return JSTypedArrayHandle(p ? p->u.typed_array_handle : GC_HANDLE_NULL);
    }
    
    void set_typed_array_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.typed_array_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.typed_array_handle, h);
        }
    }
    
    /* Typed array pointer accessors */
    int8_t* array_int8_ptr() const {
        JSObject* p = get_ptr();
        return p ? p->u.array.u.int8_ptr : nullptr;
    }
    
    void set_array_int8_ptr(int8_t* ptr) {
        JSObject* p = get_ptr();
        if (p) p->u.array.u.int8_ptr = ptr;
    }
    
    uint8_t* array_uint8_ptr() const {
        JSObject* p = get_ptr();
        return p ? p->u.array.u.uint8_ptr : nullptr;
    }
    
    void set_array_uint8_ptr(uint8_t* ptr) {
        JSObject* p = get_ptr();
        if (p) p->u.array.u.uint8_ptr = ptr;
    }
    
    int16_t* array_int16_ptr() const {
        JSObject* p = get_ptr();
        return p ? p->u.array.u.int16_ptr : nullptr;
    }
    
    void set_array_int16_ptr(int16_t* ptr) {
        JSObject* p = get_ptr();
        if (p) p->u.array.u.int16_ptr = ptr;
    }
    
    uint16_t* array_uint16_ptr() const {
        JSObject* p = get_ptr();
        return p ? p->u.array.u.uint16_ptr : nullptr;
    }
    
    void set_array_uint16_ptr(uint16_t* ptr) {
        JSObject* p = get_ptr();
        if (p) p->u.array.u.uint16_ptr = ptr;
    }
    
    int32_t* array_int32_ptr() const {
        JSObject* p = get_ptr();
        return p ? p->u.array.u.int32_ptr : nullptr;
    }
    
    void set_array_int32_ptr(int32_t* ptr) {
        JSObject* p = get_ptr();
        if (p) p->u.array.u.int32_ptr = ptr;
    }
    
    uint32_t* array_uint32_ptr() const {
        JSObject* p = get_ptr();
        return p ? p->u.array.u.uint32_ptr : nullptr;
    }
    
    void set_array_uint32_ptr(uint32_t* ptr) {
        JSObject* p = get_ptr();
        if (p) p->u.array.u.uint32_ptr = ptr;
    }
    
    float* array_float_ptr() const {
        JSObject* p = get_ptr();
        return p ? p->u.array.u.float_ptr : nullptr;
    }
    
    void set_array_float_ptr(float* ptr) {
        JSObject* p = get_ptr();
        if (p) p->u.array.u.float_ptr = ptr;
    }
    
    double* array_double_ptr() const {
        JSObject* p = get_ptr();
        return p ? p->u.array.u.double_ptr : nullptr;
    }
    
    void set_array_double_ptr(double* ptr) {
        JSObject* p = get_ptr();
        if (p) p->u.array.u.double_ptr = ptr;
    }
    
    int64_t* array_int64_ptr() const {
        JSObject* p = get_ptr();
        return p ? p->u.array.u.int64_ptr : nullptr;
    }
    
    void set_array_int64_ptr(int64_t* ptr) {
        JSObject* p = get_ptr();
        if (p) p->u.array.u.int64_ptr = ptr;
    }
    
    uint64_t* array_uint64_ptr() const {
        JSObject* p = get_ptr();
        return p ? p->u.array.u.uint64_ptr : nullptr;
    }
    
    void set_array_uint64_ptr(uint64_t* ptr) {
        JSObject* p = get_ptr();
        if (p) p->u.array.u.uint64_ptr = ptr;
    }
    
    uint16_t* array_fp16_ptr() const {
        JSObject* p = get_ptr();
        return p ? p->u.array.u.fp16_ptr : nullptr;
    }
    
    void set_array_fp16_ptr(uint16_t* ptr) {
        JSObject* p = get_ptr();
        if (p) p->u.array.u.fp16_ptr = ptr;
    }
    
    /* Union cfunc field access - u.cfunc.realm_handle */
    GCHandle cfunc_realm_handle() const {
        JSObject* p = get_ptr();
        return p ? p->u.cfunc.realm_handle : GC_HANDLE_NULL;
    }
    
    void set_cfunc_realm_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.cfunc.realm_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.cfunc.realm_handle, h);
        }
    }
    
    /* Union cfunc field access - u.cfunc.c_function */
    JSCFunctionType cfunc_c_function() const {
        JSObject* p = get_ptr();
        return p ? p->u.cfunc.c_function : JSCFunctionType{};
    }
    
    void set_cfunc_c_function(JSCFunctionType func) {
        JSObject* p = get_ptr();
        if (p) p->u.cfunc.c_function = func;
    }
    
    /* Union cfunc field access - u.cfunc.length */
    uint8_t cfunc_length() const {
        JSObject* p = get_ptr();
        return p ? p->u.cfunc.length : 0;
    }
    
    void set_cfunc_length(uint8_t len) {
        JSObject* p = get_ptr();
        if (p) p->u.cfunc.length = len;
    }
    
    /* Union cfunc field access - u.cfunc.cproto */
    uint8_t cfunc_cproto() const {
        JSObject* p = get_ptr();
        return p ? p->u.cfunc.cproto : 0;
    }
    
    void set_cfunc_cproto(uint8_t proto) {
        JSObject* p = get_ptr();
        if (p) p->u.cfunc.cproto = proto;
    }
    
    /* Union cfunc field access - u.cfunc.magic */
    int16_t cfunc_magic() const {
        JSObject* p = get_ptr();
        return p ? p->u.cfunc.magic : 0;
    }
    
    void set_cfunc_magic(int16_t magic) {
        JSObject* p = get_ptr();
        if (p) p->u.cfunc.magic = magic;
    }
    
    /* weakref_count access */
    uint32_t weakref_count() const {
        JSObject* p = get_ptr();
        return p ? p->weakref_count : 0;
    }
    
    void set_weakref_count(uint32_t count) {
        JSObject* p = get_ptr();
        if (p) p->weakref_count = count;
    }
    
    /* Comparison operators */
    bool operator==(const JSObjectHandle& other) const {
        return handle_ == other.handle_;
    }
    
    bool operator!=(const JSObjectHandle& other) const {
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
    
    /* Union field access - u.proxy_data_handle for JS_CLASS_PROXY - returns typed handle */
    JSProxyDataHandle proxy_data_handle() const {
        JSObject* p = get_ptr();
        return JSProxyDataHandle(p ? p->u.proxy_data_handle : GC_HANDLE_NULL);
    }
    
    void set_proxy_data_handle(GCHandle h) {
        JSObject* p = get_ptr();
        if (p) {
            p->u.proxy_data_handle = h;
            gc_write_barrier_for_heap_slot(&p->u.proxy_data_handle, h);
        }
    }
};


/* Helper to create JSObjectHandle from JSValue (GCValue) */
static inline JSObjectHandle JS_VALUE_GET_OBJ_HANDLE(GCValue val) {
    if (GC_VALUE_GET_TAG(val) == JS_TAG_OBJECT) {
        return JSObjectHandle(GC_VALUE_GET_HANDLE(val));
    }
    return JSObjectHandle();
}

/* Helper to convert JSObjectHandle to GCValue */
static inline GCValue JS_MKOBJ(JSObjectHandle obj) {
    return GC_MKHANDLE(JS_TAG_OBJECT, obj.handle());
}

/* Helper to create JSModuleDefHandle from JSValue (GCValue) */
static inline JSModuleDefHandle JS_VALUE_GET_MODULE_HANDLE(GCValue val) {
    if (GC_VALUE_GET_TAG(val) == JS_TAG_MODULE) {
        return JSModuleDefHandle(GC_VALUE_GET_HANDLE(val));
    }
    return JSModuleDefHandle();
}

/* Helper to convert JSModuleDefHandle to GCValue */
static inline GCValue JS_MKMODULE(JSModuleDefHandle m) {
    return GC_MKHANDLE(JS_TAG_MODULE, m.handle());
}

/* Helper to create JSAsyncFunctionStateHandle from JSValue (GCValue) */
static inline JSAsyncFunctionStateHandle JS_VALUE_GET_ASYNC_FUNC_HANDLE(GCValue val) {
    /* Note: Async function state doesn't have a dedicated tag in JSValue,
     * it's accessed through internal structures. This helper assumes
     * the caller knows the value contains an async function state handle. */
    return JSAsyncFunctionStateHandle(GC_VALUE_GET_HANDLE(val));
}

/* Helper to create JSBigIntHandle from JSValue (GCValue) */
static inline JSBigIntHandle JS_VALUE_GET_BIGINT_HANDLE(GCValue val) {
    if (GC_VALUE_GET_TAG(val) == JS_TAG_BIG_INT) {
        return JSBigIntHandle(GC_VALUE_GET_HANDLE(val));
    }
    return JSBigIntHandle();
}

/* Helper to convert JSBigIntHandle to GCValue */
static inline GCValue JS_MKBIGINT(JSBigIntHandle bigint) {
    return GC_MKHANDLE(JS_TAG_BIG_INT, bigint.handle());
}

#endif /* QUICKJS_H */

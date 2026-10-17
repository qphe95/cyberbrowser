/*
 * QuickJS Safe Handle Macros
 * ============================================================================
 *
 * This header replaces the old C++ "handle wrapper classes"
 * (JSContextHandle, JSMapStateHandle, ... in quickjs_handle_classes.h and the
 * scattered *Handle classes in quickjs_types.h / quickjs-internal.h /
 * quickjs.h) with a macro based system that is C compatible.
 *
 * The GC is *relocating*: objects can move during compaction, so a raw
 * pointer obtained from a GCHandle must never be stored across a GC point.
 * A handle (GCHandle, a uint32_t index into the handle table) stays stable.
 *
 * Every macro below dereferences the handle at the point of use, so the
 * resulting pointer is only valid until the next GC point.
 *
 *     old C++ style:
 *     JSMapStateHandle s(s_handle);
 *     s.inc_record_count();
 *     s.set_record_count(0);
 *
 *     new C style:
 *     SAFE_SET(s_handle, JSMapStateHandle, record_count,
 *              SAFE_GET(s_handle, JSMapStateHandle, record_count) + 1);
 *
 * ----------------------------------------------------------------------------
 * MACRO FAMILY
 * ----------------------------------------------------------------------------
 *   GC_DEREF_H(handle, HType)          raw pointer (pinned, unsafe to store)
 *   SAFE_PTR(handle, HType)            alias of GC_DEREF_H (was .ptr()/__unsafe_internal_ptr())
 *   SAFE_GET(handle, HType, field)     read a field
 *   SAFE_GET_OR(handle, HType, field, dflt)
 *                                      read a field, or `dflt` when handle is NULL
 *   SAFE_SET(handle, HType, field, v)  write a plain (non-GC) field
 *   SAFE_SET_HANDLE(handle, HType, field, h)
 *                                      write a GCHandle field + write barrier
 *   SAFE_SET_VALUE(handle, HType, field, v)
 *                                      write a GCValue field + write barrier
 *   SAFE_REF(handle, HType, field)     lvalue reference to a field
 *   SAFE_AT(handle, HType, index)      array element (for array handles)
 *   SAFE_HANDLE(handle)                the handle itself (was .handle())
 *   SAFE_IS_NULL(handle)               handle == GC_HANDLE_NULL
 *   SAFE_VALID(handle)                 handle is in the live handle table
 *
 * `HType` is the *handle type name* (e.g. JSMapStateHandle). It is mapped to
 * the underlying struct via GC_STRUCT_OF_* below. For convenience the struct
 * names themselves (e.g. JSMapState) are also accepted.
 *
 * NOTE: SAFE_GET / SAFE_SET intentionally do NOT null-check, matching the
 * fast path of the old accessors. Use SAFE_GET_OR (or an explicit
 * SAFE_VALID check) when a NULL handle is possible.
 * ============================================================================
 */

#ifndef QUICKJS_HANDLES_H
#define QUICKJS_HANDLES_H

#include <stddef.h>
#include <stdint.h>

#include "quickjs_types.h"
#include "quickjs_gc_unified.h"

/* ============================================================================
 * Handle type -> struct type map
 * ============================================================================
 * The indirection through GC_STRUCT_OF lets us write the *handle* type in the
 * macros while still casting to the real struct. It also gives us a single
 * place to update when handle classes are finally renamed to plain structs.
 *
 *   #define GC_STRUCT_OF(JSContextHandle)  ->  GC_STRUCT_OF_JSContextHandle
 *                                          ->  JSContext
 */
#define GC_STRUCT_OF(HType) GC_STRUCT_OF_##HType

/* Handle class name -> underlying struct */
#define GC_STRUCT_OF_JSContextHandle                    JSContext
#define GC_STRUCT_OF_JSRuntimeHandle                    JSRuntime
#define GC_STRUCT_OF_JSAsyncFrameHandle                 JSStackFrame
#define GC_STRUCT_OF_JSBigIntHandle                     JSBigInt
#define GC_STRUCT_OF_JSModuleDefHandle                  JSModuleDef
#define GC_STRUCT_OF_JSAsyncFunctionStateHandle         JSAsyncFunctionState
#define GC_STRUCT_OF_JSJobEntryHandle                   JSJobEntry
#define GC_STRUCT_OF_JSFunctionBytecodeHandle           JSFunctionBytecode
#define GC_STRUCT_OF_JSArrayBufferHandle                JSArrayBuffer
#define GC_STRUCT_OF_JSTypedArrayHandle                 JSTypedArray
#define GC_STRUCT_OF_JSMapStateHandle                   JSMapState
#define GC_STRUCT_OF_JSPromiseDataHandle                JSPromiseData
#define GC_STRUCT_OF_JSProxyDataHandle                  JSProxyData
#define GC_STRUCT_OF_JSBoundFunctionHandle              JSBoundFunction
#define GC_STRUCT_OF_JSForInIteratorHandle              JSForInIterator
#define GC_STRUCT_OF_JSMapRecordHandle                  JSMapRecord
#define GC_STRUCT_OF_JSMapIteratorDataHandle            JSMapIteratorData
#define GC_STRUCT_OF_JSArrayIteratorDataHandle          JSArrayIteratorData
#define GC_STRUCT_OF_JSIteratorHelperDataHandle         JSIteratorHelperData
#define GC_STRUCT_OF_JSIteratorConcatDataHandle         JSIteratorConcatData
#define GC_STRUCT_OF_JSIteratorWrapDataHandle           JSIteratorWrapData
#define GC_STRUCT_OF_JSRegExpStringIteratorDataHandle   JSRegExpStringIteratorData
#define GC_STRUCT_OF_JSVarDefHandle                     JSVarDef
#define GC_STRUCT_OF_JSVarDefArrayHandle                JSVarDef
#define GC_STRUCT_OF_JSAsyncFromSyncIteratorDataHandle  JSAsyncFromSyncIteratorData
#define GC_STRUCT_OF_JSPromiseFunctionDataHandle        JSPromiseFunctionData
#define GC_STRUCT_OF_JSPromiseFunctionDataResolvedHandle JSPromiseFunctionDataResolved
#define GC_STRUCT_OF_JSCFunctionDataRecordHandle        JSCFunctionDataRecord
#define GC_STRUCT_OF_JSGeneratorDataHandle              JSGeneratorData
#define GC_STRUCT_OF_JSAsyncGeneratorDataHandle         JSAsyncGeneratorData
#define GC_STRUCT_OF_JSFinalizationRegistryDataHandle   JSFinalizationRegistryData
#define GC_STRUCT_OF_JSWeakRefDataHandle                JSWeakRefData
#define GC_STRUCT_OF_JSPropertyEnumHandle               JSPropertyEnum
#define GC_STRUCT_OF_GCStringHandle                     char
#define GC_STRUCT_OF_JSPropertyArrayHandle              JSProperty
#define GC_STRUCT_OF_JSPropertyHandle                   JSProperty
#define GC_STRUCT_OF_JSPromiseReactionDataHandle        JSPromiseReactionData
#define GC_STRUCT_OF_JSFinRecEntryHandle                JSFinRecEntry
#define GC_STRUCT_OF_JSAsyncGeneratorRequestHandle      JSAsyncGeneratorRequest
#define GC_STRUCT_OF_JSClassHandle                      JSClass
#define GC_STRUCT_OF_JSObjectListEntryArrayHandle       JSObjectListEntry
#define GC_STRUCT_OF_GCValueHandle                      GCValue
#define GC_STRUCT_OF_JSVarRefHandle                     JSVarRef
#define GC_STRUCT_OF_JSShapeHandle                      JSShape
#define GC_STRUCT_OF_JSStringHandle                     JSString
#define GC_STRUCT_OF_JSStringRopeHandle                 JSStringRope
#define GC_STRUCT_OF_JSObjectHandle                     JSObject
#define GC_STRUCT_OF_JSFunctionDefHandle                JSFunctionDef   /* defined in quickjs.cpp */
#define GC_STRUCT_OF_void                               void

/* Accept bare struct names too, so SAFE_GET(h, JSContext, rt_handle) works. */
#define GC_STRUCT_OF_JSContext                          JSContext
#define GC_STRUCT_OF_JSRuntime                          JSRuntime
#define GC_STRUCT_OF_JSStackFrame                       JSStackFrame
#define GC_STRUCT_OF_JSBigInt                           JSBigInt
#define GC_STRUCT_OF_JSModuleDef                        JSModuleDef
#define GC_STRUCT_OF_JSAsyncFunctionState               JSAsyncFunctionState
#define GC_STRUCT_OF_JSJobEntry                         JSJobEntry
#define GC_STRUCT_OF_JSFunctionBytecode                 JSFunctionBytecode
#define GC_STRUCT_OF_JSArrayBuffer                      JSArrayBuffer
#define GC_STRUCT_OF_JSTypedArray                       JSTypedArray
#define GC_STRUCT_OF_JSMapState                         JSMapState
#define GC_STRUCT_OF_JSPromiseData                      JSPromiseData
#define GC_STRUCT_OF_JSProxyData                        JSProxyData
#define GC_STRUCT_OF_JSBoundFunction                    JSBoundFunction
#define GC_STRUCT_OF_JSForInIterator                    JSForInIterator
#define GC_STRUCT_OF_JSMapRecord                        JSMapRecord
#define GC_STRUCT_OF_JSMapIteratorData                  JSMapIteratorData
#define GC_STRUCT_OF_JSArrayIteratorData                JSArrayIteratorData
#define GC_STRUCT_OF_JSIteratorHelperData               JSIteratorHelperData
#define GC_STRUCT_OF_JSIteratorConcatData               JSIteratorConcatData
#define GC_STRUCT_OF_JSIteratorWrapData                 JSIteratorWrapData
#define GC_STRUCT_OF_JSRegExpStringIteratorData         JSRegExpStringIteratorData
#define GC_STRUCT_OF_JSVarDef                           JSVarDef
#define GC_STRUCT_OF_JSAsyncFromSyncIteratorData        JSAsyncFromSyncIteratorData
#define GC_STRUCT_OF_JSPromiseFunctionData              JSPromiseFunctionData
#define GC_STRUCT_OF_JSPromiseFunctionDataResolved      JSPromiseFunctionDataResolved
#define GC_STRUCT_OF_JSCFunctionDataRecord              JSCFunctionDataRecord
#define GC_STRUCT_OF_JSGeneratorData                    JSGeneratorData
#define GC_STRUCT_OF_JSAsyncGeneratorData               JSAsyncGeneratorData
#define GC_STRUCT_OF_JSFinalizationRegistryData         JSFinalizationRegistryData
#define GC_STRUCT_OF_JSWeakRefData                      JSWeakRefData
#define GC_STRUCT_OF_JSPropertyEnum                     JSPropertyEnum
#define GC_STRUCT_OF_JSProperty                         JSProperty
#define GC_STRUCT_OF_JSPromiseReactionData              JSPromiseReactionData
#define GC_STRUCT_OF_JSFinRecEntry                      JSFinRecEntry
#define GC_STRUCT_OF_JSAsyncGeneratorRequest            JSAsyncGeneratorRequest
#define GC_STRUCT_OF_JSClass                            JSClass
#define GC_STRUCT_OF_JSObjectListEntry                  JSObjectListEntry
#define GC_STRUCT_OF_GCValue                            GCValue
#define GC_STRUCT_OF_JSVarRef                           JSVarRef
#define GC_STRUCT_OF_JSShape                            JSShape
#define GC_STRUCT_OF_JSString                           JSString
#define GC_STRUCT_OF_JSStringRope                       JSStringRope
#define GC_STRUCT_OF_JSObject                           JSObject
#define GC_STRUCT_OF_JSFunctionDef                      JSFunctionDef

/* ============================================================================
 * Core dereference
 * ============================================================================
 * Pin a handle to a raw pointer. Valid only until the next GC point; never
 * store the result in a variable that outlives the current statement.
 */
#define GC_DEREF_H(handle, HType) ((GC_STRUCT_OF(HType) *)(gc_deref(handle)))

/* Raw pointer escape hatch (was .ptr() / .__unsafe_internal_ptr()). */
#define SAFE_PTR(handle, HType) GC_DEREF_H(handle, HType)

/* ============================================================================
 * Field read / write
 * ============================================================================ */

/* Read a field. Not null-safe (matches the old fast path). */
#define SAFE_GET(handle, HType, field) (GC_DEREF_H(handle, HType)->field)

/* Declared type of a struct field, used so the SAFE_SET family can evaluate
 * the value *before* dereferencing the handle. The value expression may
 * allocate (and so move objects); dereferencing first would leave _p stale,
 * and SAFE_SET_HANDLE/VALUE would also evaluate `value` twice. The old
 * methods were safe here because C++ evaluates arguments before the call. */
#ifdef __cplusplus
#define GC_FIELD_TYPE(HType, field) decltype(((GC_STRUCT_OF(HType) *)0)->field)
#else
#define GC_FIELD_TYPE(HType, field) __typeof__(((GC_STRUCT_OF(HType) *)0)->field)
#endif

/* Write a plain (non-GC) field. */
#define SAFE_SET(handle, HType, field, value) \
    do { \
        GC_FIELD_TYPE(HType, field) _v = (value); \
        GC_STRUCT_OF(HType) *_p = GC_DEREF_H(handle, HType); \
        if (_p) _p->field = _v; \
    } while (0)

/* Lvalue reference to a field (e.g. to take its address). */
#define SAFE_REF(handle, HType, field) (GC_DEREF_H(handle, HType)->field)

/* Array element access for array-backed handles (JSVarDefArray, JSProperty, ...). */
#define SAFE_AT(handle, HType, index) (GC_DEREF_H(handle, HType)[(index)])

/* ============================================================================
 * GC pointer writes (with write barrier)
 * ============================================================================ */

/* Write a GCHandle field and run the heap-slot write barrier. */
#define SAFE_SET_HANDLE(handle, HType, field, value) \
    do { \
        GC_FIELD_TYPE(HType, field) _v = (value); \
        GC_STRUCT_OF(HType) *_p = GC_DEREF_H(handle, HType); \
        if (_p) { \
            _p->field = _v; \
            GC_WRITE_BARRIER_HEAP_HANDLE(&_p->field, _v); \
        } \
    } while (0)

/* Write a GCValue field and run the heap-slot write barrier. */
#define SAFE_SET_VALUE(handle, HType, field, value) \
    do { \
        GC_FIELD_TYPE(HType, field) _v = (value); \
        GC_STRUCT_OF(HType) *_p = GC_DEREF_H(handle, HType); \
        if (_p) { \
            _p->field = _v; \
            GC_WRITE_BARRIER_HEAP_VALUE(&_p->field, _v); \
        } \
    } while (0)

/* ============================================================================
 * Null-safe field read
 * ============================================================================
 * SAFE_GET_OR(handle, HType, field, dflt) yields `dflt` when the handle is
 * NULL, mirroring the `p ? p->field : dflt` behaviour of the old accessors.
 */
#if defined(__GNUC__) || defined(__clang__)
#define SAFE_GET_OR(handle, HType, field, dflt) \
    ({ \
        GC_STRUCT_OF(HType) *_p = GC_DEREF_H(handle, HType); \
        _p ? _p->field : (dflt); \
    })
#elif defined(__cplusplus)
#define SAFE_GET_OR(handle, HType, field, dflt) \
    ([&]() { \
        GC_STRUCT_OF(HType) *_p = GC_DEREF_H(handle, HType); \
        return _p ? _p->field : (dflt); \
    }())
#else
/* Fallback: no statement expressions available. */
#define SAFE_GET_OR(handle, HType, field, dflt) (GC_DEREF_H(handle, HType)->field)
#endif

/* ============================================================================
 * Handle identity / validity
 * ============================================================================ */

/* The handle itself (was wrapper.handle()). */
#define SAFE_HANDLE(handle) (handle)

#define SAFE_IS_NULL(handle) ((handle) == GC_HANDLE_NULL)
#define SAFE_VALID(handle) \
    ((handle) != GC_HANDLE_NULL && gc_handle_is_valid(handle))

#endif /* QUICKJS_HANDLES_H */

/*
 * GC Value Helpers - Helper utilities for working with GCValue
 * 
 * CRITICAL RULE: Never store the result of gc_deref(). Always use
 * handles directly instead of converting pointers back to handles.
 */

#ifndef GC_VALUE_HELPERS_H
#define GC_VALUE_HELPERS_H

#include "quickjs.h"
#include "quickjs_gc_unified.h"

#ifdef __cplusplus
extern "C" {
#endif


/* Check if GCValue represents a valid object */
static inline int gc_is_valid_object(GCValue v) {
    return GC_IS_OBJECT(v) && v.u.handle != GC_HANDLE_NULL;
}

/* Check if GCValue is a valid reference type */
static inline int gc_is_valid_reference(GCValue v) {
    return GC_IS_REFERENCE(v) && v.u.handle != GC_HANDLE_NULL;
}

/*
 * GC_PROP_GET_STR - Get string property from a GCValue object.
 * 
 * This function:
 * 1. Checks if the value is a reference type (tag < 0)
 * 2. Dereferences the handle to get the current pointer
 * 3. Immediately calls the property getter
 * 4. Does not store the pointer anywhere
 * 
 * IMPORTANT: The pointer obtained from gc_deref is used immediately and
 * never stored. This ensures GC safety.
 */
static inline GCValue GC_PROP_GET_STR(JSContextHandle ctx, GCValue obj, const char *prop) {
    GCValue result = GC_UNDEFINED;
    int tag = GC_VALUE_GET_TAG(obj);
    if (tag < 0 && obj.u.handle != GC_HANDLE_NULL) {
        /* Use handle directly instead of converting through pointer */
        GCValue wrapped = GC_MKHANDLE(tag, obj.u.handle);
        result = JS_GetPropertyStr(ctx, wrapped, prop);
    }
    return result;
}

/*
 * GC_PROP_SET_STR - Set string property on a GCValue object.
 */
static inline int GC_PROP_SET_STR(JSContextHandle ctx, GCValue obj, const char *prop, GCValue val) {
    int result = -1;
    int tag = GC_VALUE_GET_TAG(obj);
    if (tag < 0 && obj.u.handle != GC_HANDLE_NULL) {
        /* Use handle directly instead of converting through pointer */
        GCValue wrapped = GC_MKHANDLE(tag, obj.u.handle);
        result = JS_SetPropertyStr(ctx, wrapped, prop, val);
    }
    return result;
}

/* Safe property getter with null check */
static inline GCValue gc_get_prop_str_safe(JSContextHandle ctx, GCValue obj, const char *prop) {
    if (!gc_is_valid_reference(obj)) {
        return GC_UNDEFINED;
    }
    return GC_PROP_GET_STR(ctx, obj, prop);
}

/* Safe property setter with null check */
static inline int gc_set_prop_str_safe(JSContextHandle ctx, GCValue obj, const char *prop, GCValue val) {
    if (!gc_is_valid_reference(obj)) {
        return -1;
    }
    return GC_PROP_SET_STR(ctx, obj, prop, val);
}

#ifdef __cplusplus
}
#endif

#endif /* GC_VALUE_HELPERS_H */

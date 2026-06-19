#ifndef DEBUG_SHAPE_H
#define DEBUG_SHAPE_H

#include "quickjs.h"
#include <android/log.h>

// Debug function to check if a JSObject has valid shape
// Returns 1 if valid, 0 if invalid
static inline int check_object_shape(JSContextHandle ctx, GCValue obj, const char* name) {
    if (JS_IsException(obj) || !JS_IsObject(obj)) {
        __android_log_print(ANDROID_LOG_ERROR, "js_quickjs", "check_object_shape(%s): Not an object!", name);
        return 0;
    }
    
    // JSObject layout on ARM64:
    // offset 0: class_id (uint16) + weakref_count (uint32)  
    // offset 8: shape pointer (JSShape*)
    
    // Get the JSObject pointer from the GCValue using JS_VALUE_GET_PTR macro
    void* obj_ptr = JS_VALUE_GET_PTR(obj);
    
    // Read shape pointer at offset 8
    uintptr_t shape_ptr = *(uintptr_t*)((char*)obj_ptr + 8);
    
    __android_log_print(ANDROID_LOG_INFO, "js_quickjs", "check_object_shape(%s): obj=%p, shape=%p", 
        name, obj_ptr, (void*)shape_ptr);
    
    // Check if shape pointer looks valid
    if (shape_ptr == (uintptr_t)-1 || shape_ptr == 0 || shape_ptr < 0x1000) {
        __android_log_print(ANDROID_LOG_ERROR, "js_quickjs", "check_object_shape(%s): INVALID shape=%p!", 
            name, (void*)shape_ptr);
        return 0;
    }
    
    return 1;
}

#endif // DEBUG_SHAPE_H

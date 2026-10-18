/* Auto-generated split from browser_api_impl.cpp */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_types.h"
#include "browser_api_impl_handle_helpers.h"
#include "browser_api_impl_internal.h"
#include "html_dom.h"
#include "css_parser.h"
#include "gc_value_helpers.h"
#include "platform.h"

// Event Implementation
// ============================================================================

void js_event_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    (void)val;
}

void js_event_mark(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func) {
    GCHandle ev_handle = JS_GetOpaqueHandle(val, js_event_class_id);
    if (ev_handle == GC_HANDLE_NULL) return;
    mark_func(rt, ev_handle);
    EventData *ev = (EventData *)gc_deref(ev_handle);
    if (!ev) return;
    JS_MarkValue(rt, ev->target, mark_func);
    JS_MarkValue(rt, ev->currentTarget, mark_func);
    JS_MarkValue(rt, ev->path, mark_func);
}

JSClassDef js_event_class_def = {
    .class_name = "Event",
    .finalizer = js_event_finalizer,
    .gc_mark   = js_event_mark,
};

GCValue js_event_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    const char* type = "";
    if (argc > 0) {
        type = JS_ToCString(ctx, argv[0]);
        if (!type) type = "";
    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_event_class_id);
    if (JS_IsException(obj)) {
        return JS_EXCEPTION;
    }
    
    EventHandle ev = event_create(ctx, type);
    if (SAFE_IS_NULL(ev)) {
        return JS_ThrowInternalError(ctx, "Event creation failed");
    }
    
    // Parse event init object if provided
    if (argc > 1 && JS_IsObject(argv[1])) {
        GCValue bubbles_val = JS_GetPropertyStr(ctx, argv[1], "bubbles");
        GCValue cancelable_val = JS_GetPropertyStr(ctx, argv[1], "cancelable");
        GCValue composed_val = JS_GetPropertyStr(ctx, argv[1], "composed");
        
        SAFE_SET(ev, EventHandle, bubbles, JS_ToBool(ctx, bubbles_val));
        SAFE_SET(ev, EventHandle, cancelable, JS_ToBool(ctx, cancelable_val));
        SAFE_SET(ev, EventHandle, composed, JS_ToBool(ctx, composed_val));
    }
    
    JS_SetOpaqueHandle(obj, ev);
    return obj;
}

GCValue js_event_get_type(JSContextHandle ctx, GCValue this_val) {
    EventHandle ev = event_from_object_check(ctx, this_val);
    if (SAFE_IS_NULL(ev)) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return JS_NewString(ctx, SAFE_GET_OR(ev, EventHandle, type, ""));
}

GCValue js_event_get_bubbles(JSContextHandle ctx, GCValue this_val) {
    EventHandle ev = event_from_object_check(ctx, this_val);
    if (SAFE_IS_NULL(ev)) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return JS_NewBool(ctx, SAFE_GET_OR(ev, EventHandle, bubbles, 0));
}

GCValue js_event_get_cancelable(JSContextHandle ctx, GCValue this_val) {
    EventHandle ev = event_from_object_check(ctx, this_val);
    if (SAFE_IS_NULL(ev)) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return JS_NewBool(ctx, SAFE_GET_OR(ev, EventHandle, cancelable, 0));
}

GCValue js_event_get_composed(JSContextHandle ctx, GCValue this_val) {
    EventHandle ev = event_from_object_check(ctx, this_val);
    if (SAFE_IS_NULL(ev)) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return JS_NewBool(ctx, SAFE_GET_OR(ev, EventHandle, composed, 0));
}

GCValue js_event_get_defaultPrevented(JSContextHandle ctx, GCValue this_val) {
    EventHandle ev = event_from_object_check(ctx, this_val);
    if (SAFE_IS_NULL(ev)) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return JS_NewBool(ctx, SAFE_GET_OR(ev, EventHandle, defaultPrevented, 0));
}

GCValue js_event_get_target(JSContextHandle ctx, GCValue this_val) {
    EventHandle ev = event_from_object_check(ctx, this_val);
    GCValue actual = JS_NULL;
    GCValue current = JS_NULL;
    if (!SAFE_IS_NULL(ev)) {
        actual = SAFE_GET_OR(ev, EventHandle, target, JS_NULL);
        current = SAFE_GET_OR(ev, EventHandle, currentTarget, JS_NULL);
    } else {
        // Fallback for subclasses (CustomEvent, MouseEvent, FocusEvent) that do not
        // share the Event class id but still set the target/currentTarget properties.
        actual = JS_GetPropertyStr(ctx, this_val, "target");
        current = JS_GetPropertyStr(ctx, this_val, "currentTarget");
    }

    GCValue global = JS_GetGlobalObject(ctx);
    GCValue helper = JS_GetPropertyStr(ctx, global, "__cyber_eventRetarget");
    if (JS_IsFunction(ctx, helper)) {
        GCValue args[2] = { actual, current };
        return JS_Call(ctx, helper, global, 2, args);
    }
    return actual;
}

GCValue js_event_get_currentTarget(JSContextHandle ctx, GCValue this_val) {
    EventHandle ev = event_from_object_check(ctx, this_val);
    if (!SAFE_IS_NULL(ev)) return SAFE_GET_OR(ev, EventHandle, currentTarget, JS_NULL);
    return JS_GetPropertyStr(ctx, this_val, "currentTarget");
}

GCValue js_event_preventDefault(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    EventHandle ev = event_from_object_check(ctx, this_val);
    if (SAFE_IS_NULL(ev)) return JS_ThrowTypeError(ctx, "Invalid Event object");
    SAFE_SET(ev, EventHandle, defaultPrevented, 1);
    return JS_UNDEFINED;
}

GCValue js_event_stopPropagation(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    JS_SetPropertyStr(ctx, this_val, "cancelBubble", JS_TRUE);
    JS_SetPropertyStr(ctx, this_val, "__cyber_stopped", JS_TRUE);
    return JS_UNDEFINED;
}

GCValue js_event_stopImmediatePropagation(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    JS_SetPropertyStr(ctx, this_val, "cancelBubble", JS_TRUE);
    JS_SetPropertyStr(ctx, this_val, "__cyber_stopped", JS_TRUE);
    JS_SetPropertyStr(ctx, this_val, "__cyber_stopped_immediate", JS_TRUE);
    return JS_UNDEFINED;
}

GCValue js_event_get_eventPhase(JSContextHandle ctx, GCValue this_val) {
    EventHandle ev = event_from_object_check(ctx, this_val);
    if (SAFE_IS_NULL(ev)) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return JS_NewInt32(ctx, SAFE_GET_OR(ev, EventHandle, eventPhase, 0));
}

GCValue js_event_get_eventPhase_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_event_get_eventPhase(ctx, this_val);
}

GCValue js_event_composedPath(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue path = JS_NULL;
    EventHandle ev = event_from_object_check(ctx, this_val);
    if (!SAFE_IS_NULL(ev)) {
        path = SAFE_GET_OR(ev, EventHandle, path, JS_NULL);
    }
    if (!JS_IsArray(ctx, path)) {
        path = JS_GetPropertyStr(ctx, this_val, "__composedPath");
    }
    if (!JS_IsUndefined(path) && !JS_IsNull(path) && JS_IsArray(ctx, path)) {
        // Return a shallow copy so script mutation does not affect the event.
        GCValue copy = JS_NewArray(ctx);
        GCValue len_val = JS_GetPropertyStr(ctx, path, "length");
        uint32_t len = 0;
        JS_ToUint32(ctx, &len, len_val);
        for (uint32_t i = 0; i < len; i++) {
            GCValue item = JS_GetPropertyUint32(ctx, path, i);
            JS_SetPropertyUint32(ctx, copy, i, item);
        }
        return copy;
    }
    return JS_NewArray(ctx);
}

// Event getter wrapper functions (matching JSCFunction signature)
GCValue js_event_get_type_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_event_get_type(ctx, this_val);
}

GCValue js_event_get_bubbles_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_event_get_bubbles(ctx, this_val);
}

GCValue js_event_get_cancelable_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_event_get_cancelable(ctx, this_val);
}

GCValue js_event_get_composed_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_event_get_composed(ctx, this_val);
}

GCValue js_event_get_defaultPrevented_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_event_get_defaultPrevented(ctx, this_val);
}

GCValue js_event_get_target_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_event_get_target(ctx, this_val);
}

GCValue js_event_get_currentTarget_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_event_get_currentTarget(ctx, this_val);
}

// CustomEvent Implementation
void js_custom_event_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    (void)val;
}

void js_custom_event_mark(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func) {
    GCHandle ev_handle = JS_GetOpaqueHandle(val, js_custom_event_class_id);
    if (ev_handle == GC_HANDLE_NULL) return;
    mark_func(rt, ev_handle);
    CustomEventData *ev = (CustomEventData *)gc_deref(ev_handle);
    if (!ev) return;
    JS_MarkValue(rt, ev->base.target, mark_func);
    JS_MarkValue(rt, ev->base.currentTarget, mark_func);
    JS_MarkValue(rt, ev->base.path, mark_func);
    JS_MarkValue(rt, ev->detail, mark_func);
}

JSClassDef js_custom_event_class_def = {
    .class_name = "CustomEvent",
    .finalizer = js_custom_event_finalizer,
    .gc_mark   = js_custom_event_mark,
};

GCValue js_custom_event_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    const char* type = "";
    if (argc > 0) {
        type = JS_ToCString(ctx, argv[0]);
        if (!type) type = "";
    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_custom_event_class_id);
    if (JS_IsException(obj)) {
        return JS_EXCEPTION;
    }
    
    CustomEventHandle ev = custom_event_create(ctx, type);
    if (SAFE_IS_NULL(ev)) {
        return JS_ThrowInternalError(ctx, "CustomEvent creation failed");
    }
    
    // Parse event init object if provided
    if (argc > 1 && JS_IsObject(argv[1])) {
        GCValue bubbles_val = JS_GetPropertyStr(ctx, argv[1], "bubbles");
        GCValue cancelable_val = JS_GetPropertyStr(ctx, argv[1], "cancelable");
        GCValue composed_val = JS_GetPropertyStr(ctx, argv[1], "composed");
        GCValue detail_val = JS_GetPropertyStr(ctx, argv[1], "detail");
        
        SAFE_SET(ev, CustomEventHandle, base.bubbles, JS_ToBool(ctx, bubbles_val));
        SAFE_SET(ev, CustomEventHandle, base.cancelable, JS_ToBool(ctx, cancelable_val));
        SAFE_SET(ev, CustomEventHandle, base.composed, JS_ToBool(ctx, composed_val));
        SAFE_SET(ev, CustomEventHandle, detail, detail_val);
    }
    
    JS_SetOpaqueHandle(obj, ev);
    return obj;
}

GCValue js_custom_event_get_detail(JSContextHandle ctx, GCValue this_val) {
    CustomEventHandle ev = JS_GetOpaqueHandle2(ctx, this_val, js_custom_event_class_id);
    if (SAFE_IS_NULL(ev)) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return SAFE_GET_OR(ev, CustomEventHandle, detail, JS_NULL);
}

// MouseEvent Implementation
void js_mouse_event_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    (void)val;
}

void js_mouse_event_mark(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func) {
    GCHandle ev_handle = JS_GetOpaqueHandle(val, js_mouse_event_class_id);
    if (ev_handle == GC_HANDLE_NULL) return;
    mark_func(rt, ev_handle);
    MouseEventData *ev = (MouseEventData *)gc_deref(ev_handle);
    if (!ev) return;
    JS_MarkValue(rt, ev->base.target, mark_func);
    JS_MarkValue(rt, ev->base.currentTarget, mark_func);
    JS_MarkValue(rt, ev->base.path, mark_func);
}

JSClassDef js_mouse_event_class_def = {
    .class_name = "MouseEvent",
    .finalizer = js_mouse_event_finalizer,
    .gc_mark   = js_mouse_event_mark,
};

GCValue js_mouse_event_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    const char* type = "";
    if (argc > 0) {
        type = JS_ToCString(ctx, argv[0]);
        if (!type) type = "";
    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_mouse_event_class_id);
    if (JS_IsException(obj)) {
        return JS_EXCEPTION;
    }
    
    MouseEventHandle ev = mouse_event_create(ctx, type);
    if (SAFE_IS_NULL(ev)) {
        return JS_ThrowInternalError(ctx, "MouseEvent creation failed");
    }
    
    // Parse mouse event init object if provided
    if (argc > 1 && JS_IsObject(argv[1])) {
        GCValue bubbles_val = JS_GetPropertyStr(ctx, argv[1], "bubbles");
        GCValue cancelable_val = JS_GetPropertyStr(ctx, argv[1], "cancelable");
        GCValue clientX_val = JS_GetPropertyStr(ctx, argv[1], "clientX");
        GCValue clientY_val = JS_GetPropertyStr(ctx, argv[1], "clientY");
        GCValue screenX_val = JS_GetPropertyStr(ctx, argv[1], "screenX");
        GCValue screenY_val = JS_GetPropertyStr(ctx, argv[1], "screenY");
        GCValue button_val = JS_GetPropertyStr(ctx, argv[1], "button");
        GCValue buttons_val = JS_GetPropertyStr(ctx, argv[1], "buttons");
        GCValue ctrlKey_val = JS_GetPropertyStr(ctx, argv[1], "ctrlKey");
        GCValue shiftKey_val = JS_GetPropertyStr(ctx, argv[1], "shiftKey");
        GCValue altKey_val = JS_GetPropertyStr(ctx, argv[1], "altKey");
        GCValue metaKey_val = JS_GetPropertyStr(ctx, argv[1], "metaKey");
        
        SAFE_SET(ev, MouseEventHandle, base.bubbles, JS_ToBool(ctx, bubbles_val));
        SAFE_SET(ev, MouseEventHandle, base.cancelable, JS_ToBool(ctx, cancelable_val));
        
        double dval;
        if (!JS_IsException(clientX_val)) { JS_ToFloat64(ctx, &dval, clientX_val); SAFE_SET(ev, MouseEventHandle, clientX, dval); }
        if (!JS_IsException(clientY_val)) { JS_ToFloat64(ctx, &dval, clientY_val); SAFE_SET(ev, MouseEventHandle, clientY, dval); }
        
        int ival;
        if (!JS_IsException(button_val)) { JS_ToInt32(ctx, &ival, button_val); }
        if (!JS_IsException(ctrlKey_val)) { JS_ToBool(ctx, ctrlKey_val); }
    }
    
    JS_SetOpaqueHandle(obj, ev);
    return obj;
}

GCValue js_mouse_event_get_clientX(JSContextHandle ctx, GCValue this_val) {
    MouseEventHandle ev = JS_GetOpaqueHandle2(ctx, this_val, js_mouse_event_class_id);
    if (SAFE_IS_NULL(ev)) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return JS_NewFloat64(ctx, SAFE_GET_OR(ev, MouseEventHandle, clientX, 0));
}

GCValue js_mouse_event_get_clientY(JSContextHandle ctx, GCValue this_val) {
    MouseEventHandle ev = JS_GetOpaqueHandle2(ctx, this_val, js_mouse_event_class_id);
    if (SAFE_IS_NULL(ev)) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return JS_NewFloat64(ctx, SAFE_GET_OR(ev, MouseEventHandle, clientY, 0));
}

// Wrapper functions for property getters (matching JSCFunction signature)
GCValue js_custom_event_get_detail_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_custom_event_get_detail(ctx, this_val);
}

GCValue js_mouse_event_get_clientX_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_mouse_event_get_clientX(ctx, this_val);
}

GCValue js_mouse_event_get_clientY_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_mouse_event_get_clientY(ctx, this_val);
}

// FocusEvent Implementation
void js_focus_event_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    (void)val;
}

void js_focus_event_mark(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func) {
    GCHandle ev_handle = JS_GetOpaqueHandle(val, js_focus_event_class_id);
    if (ev_handle == GC_HANDLE_NULL) return;
    mark_func(rt, ev_handle);
    FocusEventData *ev = (FocusEventData *)gc_deref(ev_handle);
    if (!ev) return;
    JS_MarkValue(rt, ev->base.target, mark_func);
    JS_MarkValue(rt, ev->base.currentTarget, mark_func);
    JS_MarkValue(rt, ev->base.path, mark_func);
    JS_MarkValue(rt, ev->relatedTarget, mark_func);
}

JSClassDef js_focus_event_class_def = {
    .class_name = "FocusEvent",
    .finalizer = js_focus_event_finalizer,
    .gc_mark   = js_focus_event_mark,
};

// ServiceWorker class definitions (minimal - no finalizers needed for simple stubs)
JSClassDef js_service_worker_container_class_def = {
    .class_name = "ServiceWorkerContainer",
    .finalizer = NULL,
};

JSClassDef js_service_worker_registration_class_def = {
    .class_name = "ServiceWorkerRegistration",
    .finalizer = NULL,
};

JSClassDef js_service_worker_class_def = {
    .class_name = "ServiceWorker",
    .finalizer = NULL,
};

GCValue js_focus_event_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    const char* type = "";
    if (argc > 0) {
        type = JS_ToCString(ctx, argv[0]);
        if (!type) type = "";
    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_focus_event_class_id);
    if (JS_IsException(obj)) {
        return JS_EXCEPTION;
    }
    
    FocusEventHandle ev = focus_event_create(ctx, type);
    if (SAFE_IS_NULL(ev)) {
        return JS_ThrowInternalError(ctx, "FocusEvent creation failed");
    }
    
    // Parse focus event init object if provided
    if (argc > 1 && JS_IsObject(argv[1])) {
        GCValue bubbles_val = JS_GetPropertyStr(ctx, argv[1], "bubbles");
        GCValue cancelable_val = JS_GetPropertyStr(ctx, argv[1], "cancelable");
        GCValue relatedTarget_val = JS_GetPropertyStr(ctx, argv[1], "relatedTarget");
        
        SAFE_SET(ev, FocusEventHandle, base.bubbles, JS_ToBool(ctx, bubbles_val));
        SAFE_SET(ev, FocusEventHandle, base.cancelable, JS_ToBool(ctx, cancelable_val));
        SAFE_SET(ev, FocusEventHandle, relatedTarget, relatedTarget_val);
    }
    
    JS_SetOpaqueHandle(obj, ev);
    return obj;
}

GCValue js_focus_event_get_relatedTarget(JSContextHandle ctx, GCValue this_val) {
    FocusEventHandle ev = JS_GetOpaqueHandle2(ctx, this_val, js_focus_event_class_id);
    if (SAFE_IS_NULL(ev)) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return SAFE_GET_OR(ev, FocusEventHandle, relatedTarget, JS_NULL);
}
// ============================================================================
// EventTarget Implementation
// ============================================================================

GCValue js_event_target_addEventListener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Store event handlers in an array per event type to support multiple listeners
    if (argc < 2) return JS_UNDEFINED;
    
    const char *event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_UNDEFINED;
    
    // Use __listeners_{event} array to store all handlers
    char prop[128];
    snprintf(prop, sizeof(prop), "__listeners_%s", event);
    
    GCValue listeners = JS_GetPropertyStr(ctx, this_val, prop);
    if (JS_IsUndefined(listeners) || JS_IsNull(listeners) || !JS_IsArray(ctx, listeners)) {
        // Create new listeners array
        listeners = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, this_val, prop, listeners);
    }
    
    // Append handler to array
    int len = 0;
    GCValue len_val = JS_GetPropertyStr(ctx, listeners, "length");
    JS_ToInt32(ctx, &len, len_val);
    JS_SetPropertyUint32(ctx, listeners, len, argv[1]);
    
    return JS_UNDEFINED;
}

GCValue js_event_target_removeEventListener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    
    const char *event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_UNDEFINED;
    
    char prop[128];
    snprintf(prop, sizeof(prop), "__listeners_%s", event);
    
    GCValue listeners = JS_GetPropertyStr(ctx, this_val, prop);
    if (JS_IsUndefined(listeners) || JS_IsNull(listeners) || !JS_IsArray(ctx, listeners)) {
        return JS_UNDEFINED;
    }
    
    // Find and remove matching handler
    int len = 0;
    GCValue len_val = JS_GetPropertyStr(ctx, listeners, "length");
    JS_ToInt32(ctx, &len, len_val);
    
    for (int i = 0; i < len; i++) {
        GCValue handler = JS_GetPropertyUint32(ctx, listeners, i);
        if (JS_StrictEq(ctx, handler, argv[1])) {
            // Remove by setting to undefined (sparse array)
            JS_SetPropertyUint32(ctx, listeners, i, JS_UNDEFINED);
            break;
        }
    }
    
    return JS_UNDEFINED;
}

GCValue js_event_target_dispatchEvent(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_FALSE;
    
    // Get event type from the event object
    GCValue type_val = JS_GetPropertyStr(ctx, argv[0], "type");
    const char *type = JS_ToCString(ctx, type_val);
    if (!type) return JS_FALSE;
    
    // Set target if not already set — write both the JS property and the
    // internal EventData (the getters read from EventData).
    EventHandle ev0 = event_from_object_check(ctx, argv[0]);
    GCValue target = JS_GetPropertyStr(ctx, argv[0], "target");
    if (JS_IsNull(target) || JS_IsUndefined(target)) {
        JS_SetPropertyStr(ctx, argv[0], "target", this_val);
        if (!SAFE_IS_NULL(ev0)) SAFE_SET(ev0, EventHandle, target, this_val);
    }
    JS_SetPropertyStr(ctx, argv[0], "currentTarget", this_val);
    if (!SAFE_IS_NULL(ev0)) SAFE_SET(ev0, EventHandle, currentTarget, this_val);

    // Build the composed path: shadow-including ancestors of the target.
    GCValue path = JS_NewArray(ctx);
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue path_helper = JS_GetPropertyStr(ctx, global, "__cyber_eventComposedPath");
    if (JS_IsFunction(ctx, path_helper)) {
        GCValue arg = this_val;
        path = JS_Call(ctx, path_helper, global, 1, &arg);
    }
    EventHandle ev = event_from_object_check(ctx, argv[0]);
    if (!SAFE_IS_NULL(ev)) {
        SAFE_SET(ev, EventHandle, path, path);
    }
    // Also store on the object itself so subclasses (CustomEvent/MouseEvent/FocusEvent)
    // can expose a composedPath even though they use a separate internal handle.
    JS_SetPropertyStr(ctx, argv[0], "__composedPath", path);

    // 1. Call listeners from addEventListener: __listeners_{type}
    char prop[128];
    snprintf(prop, sizeof(prop), "__listeners_%s", type);
    GCValue listeners = JS_GetPropertyStr(ctx, this_val, prop);
    
    if (!JS_IsUndefined(listeners) && !JS_IsNull(listeners) && JS_IsArray(ctx, listeners)) {
        int len = 0;
        GCValue len_val = JS_GetPropertyStr(ctx, listeners, "length");
        JS_ToInt32(ctx, &len, len_val);
        
        for (int i = 0; i < len; i++) {
            GCValue handler = JS_GetPropertyUint32(ctx, listeners, i);
            if (JS_IsUndefined(handler) || JS_IsNull(handler)) continue;
            
            GCValue event_args[1] = { argv[0] };
            GCValue result = JS_Call(ctx, handler, this_val, 1, event_args);
            if (JS_IsException(result)) {
                GCValue exc = JS_GetException(ctx);
                (void)exc;
            }
        }
    }
    
    // 2. Also call inline handler: __on{type} (legacy single-handler storage)
    snprintf(prop, sizeof(prop), "__on%s", type);
    GCValue handler = JS_GetPropertyStr(ctx, this_val, prop);
    if (!JS_IsUndefined(handler) && !JS_IsNull(handler)) {
        GCValue event_args[1] = { argv[0] };
        GCValue result = JS_Call(ctx, handler, this_val, 1, event_args);
        if (JS_IsException(result)) {
            GCValue exc = JS_GetException(ctx);
            (void)exc;
        }
    }
    
    // 3. Call on{type} property (e.g., window.onload, document.onreadystatechange)
    if (type[0]) {
        char on_prop[128];
        snprintf(on_prop, sizeof(on_prop), "on%s", type);
        GCValue on_handler = JS_GetPropertyStr(ctx, this_val, on_prop);
        if (!JS_IsUndefined(on_handler) && !JS_IsNull(on_handler) && JS_IsFunction(ctx, on_handler)) {
            GCValue event_args[1] = { argv[0] };
            GCValue result = JS_Call(ctx, on_handler, this_val, 1, event_args);
            if (JS_IsException(result)) {
                GCValue exc = JS_GetException(ctx);
                (void)exc;
            }
        }
    }

    /* 4. Bubble phase: if the event bubbles, walk the composed path (which
     * already crosses shadow boundaries via host links) and invoke listeners
     * on each ancestor, updating currentTarget as we go.  Ancestor listeners
     * are how YouTube's delegated "attached"/"detached" lifecycle events and
     * general event-delegation reach handlers registered on hosts/document. */
    GCValue bubbles_val = JS_GetPropertyStr(ctx, argv[0], "bubbles");
    bool bubbles = JS_ToBool(ctx, bubbles_val);
    if (bubbles && JS_IsArray(ctx, path)) {
        GCValue plen_val = JS_GetPropertyStr(ctx, path, "length");
        uint32_t plen = 0;
        JS_ToUint32(ctx, &plen, plen_val);
        for (uint32_t pi = 1; pi < plen; pi++) {
            /* Honor stopPropagation()/cancelBubble between nodes. */
            GCValue cancel = JS_GetPropertyStr(ctx, argv[0], "cancelBubble");
            if (JS_ToBool(ctx, cancel)) break;
            GCValue stop = JS_GetPropertyStr(ctx, argv[0], "__cyber_stopped");
            if (JS_ToBool(ctx, stop)) break;

            GCValue node = JS_GetPropertyUint32(ctx, path, pi);
            if (JS_IsUndefined(node) || JS_IsNull(node) || !JS_IsObject(node)) continue;

            JS_SetPropertyStr(ctx, argv[0], "currentTarget", node);
            if (!SAFE_IS_NULL(ev0)) SAFE_SET(ev0, EventHandle, currentTarget, node);

            char lprop[128];
            snprintf(lprop, sizeof(lprop), "__listeners_%s", type);
            GCValue nlisteners = JS_GetPropertyStr(ctx, node, lprop);
            if (!JS_IsUndefined(nlisteners) && !JS_IsNull(nlisteners) && JS_IsArray(ctx, nlisteners)) {
                int nlen = 0;
                GCValue nlen_val = JS_GetPropertyStr(ctx, nlisteners, "length");
                JS_ToInt32(ctx, &nlen, nlen_val);
                for (int li = 0; li < nlen; li++) {
                    GCValue handler = JS_GetPropertyUint32(ctx, nlisteners, li);
                    if (JS_IsUndefined(handler) || JS_IsNull(handler) || !JS_IsFunction(ctx, handler)) continue;
                    GCValue event_args[1] = { argv[0] };
                    GCValue result = JS_Call(ctx, handler, node, 1, event_args);
                    if (JS_IsException(result)) {
                        GCValue exc = JS_GetException(ctx);
                        (void)exc;
                    }
                    /* stopImmediatePropagation aborts the remaining handlers. */
                    GCValue imm = JS_GetPropertyStr(ctx, argv[0], "__cyber_stopped_immediate");
                    if (JS_ToBool(ctx, imm)) break;
                }
            }

            /* inline __on{type} legacy storage on the ancestor */
            snprintf(lprop, sizeof(lprop), "__on%s", type);
            GCValue nhandler = JS_GetPropertyStr(ctx, node, lprop);
            if (!JS_IsUndefined(nhandler) && !JS_IsNull(nhandler) && JS_IsFunction(ctx, nhandler)) {
                GCValue event_args[1] = { argv[0] };
                GCValue result = JS_Call(ctx, nhandler, node, 1, event_args);
                if (JS_IsException(result)) {
                    GCValue exc = JS_GetException(ctx);
                    (void)exc;
                }
            }

            /* on{type} property on the ancestor (e.g., document.onload) */
            if (type[0]) {
                char on_prop[128];
                snprintf(on_prop, sizeof(on_prop), "on%s", type);
                GCValue on_h = JS_GetPropertyStr(ctx, node, on_prop);
                if (!JS_IsUndefined(on_h) && !JS_IsNull(on_h) && JS_IsFunction(ctx, on_h)) {
                    GCValue event_args[1] = { argv[0] };
                    GCValue result = JS_Call(ctx, on_h, node, 1, event_args);
                    if (JS_IsException(result)) {
                        GCValue exc = JS_GetException(ctx);
                        (void)exc;
                    }
                }
            }

            GCValue imm2 = JS_GetPropertyStr(ctx, argv[0], "__cyber_stopped_immediate");
            if (JS_ToBool(ctx, imm2)) break;
        }
        /* Restore currentTarget semantics: outside dispatch it reads as null. */
        JS_SetPropertyStr(ctx, argv[0], "currentTarget", JS_NULL);
        if (!SAFE_IS_NULL(ev0)) SAFE_SET(ev0, EventHandle, currentTarget, JS_NULL);
    }

    return JS_TRUE;
}


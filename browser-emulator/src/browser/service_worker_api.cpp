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
#include "browser_api_impl_handles.h"
#include "browser_api_impl_internal.h"
#include "html_dom.h"
#include "css_parser.h"
#include "gc_value_helpers.h"
#include "platform.h"

GCValue js_service_worker_register(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_service_worker_get_registration(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_service_worker_get_registrations(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_service_worker_add_event_listener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

// Execute a service worker script (simplified - runs in same context)
bool execute_service_worker_script(JSContextHandle ctx, const char *script_url, int worker_id) {
    // In a real implementation, this would load the script from the URL
    // For now, we just log it and return success
    LOG_INFO("ServiceWorker: Would execute script from %s (worker %d)", script_url, worker_id);
    
    // Dispatch 'install' and 'activate' events would happen here
    // For now, we just simulate that the worker activated
    
    return true;
}

// Create a ServiceWorkerRegistration object
GCValue create_service_worker_registration(JSContextHandle ctx, const char *script_url, const char *scope) {
    GCValue reg = JS_NewObjectClass(ctx, js_service_worker_registration_class_id);
    if (JS_IsException(reg)) return JS_EXCEPTION;
    
    // Allocate and initialize registration data
    GCHandle reg_handle = gc_allocz(sizeof(ServiceWorkerRegistrationData), JS_GC_OBJ_TYPE_DATA);
    if (reg_handle != GC_HANDLE_NULL) {
        ServiceWorkerRegistrationData *reg_data = (ServiceWorkerRegistrationData*)gc_deref(reg_handle);
        strncpy(reg_data->script_url, script_url, sizeof(reg_data->script_url) - 1);
        strncpy(reg_data->scope, scope, sizeof(reg_data->scope) - 1);
        reg_data->state = 3; // activated
        
        // Create ServiceWorker object for the active worker
        GCValue worker = JS_NewObjectClass(ctx, js_service_worker_class_id);
        GCHandle worker_handle = gc_allocz(sizeof(ServiceWorkerData), JS_GC_OBJ_TYPE_DATA);
        if (worker_handle != GC_HANDLE_NULL) {
            ServiceWorkerData *worker_data = (ServiceWorkerData*)gc_deref(worker_handle);
            strncpy(worker_data->script_url, script_url, sizeof(worker_data->script_url) - 1);
            strncpy(worker_data->state, "activated", sizeof(worker_data->state) - 1);
            worker_data->id = (int)(size_t)worker_handle; // Use handle as unique ID
            JS_SetOpaqueHandle(worker, worker_handle);
        }
        
        reg_data->installing = JS_NULL;
        reg_data->waiting = JS_NULL;
        reg_data->active = worker;
        reg_data->ctx = ctx;
        
        JS_SetOpaqueHandle(reg, reg_handle);
    }
    
    // Set registration properties
    JS_SetPropertyStr(ctx, reg, "scope", JS_NewString(ctx, scope));
    JS_SetPropertyStr(ctx, reg, "installing", JS_NULL);
    JS_SetPropertyStr(ctx, reg, "waiting", JS_NULL);
    JS_SetPropertyStr(ctx, reg, "active", JS_NULL); // Will be set below
    
    // Set update and unregister methods
    JS_SetPropertyStr(ctx, reg, "update",
        JS_NewCFunction(ctx, js_promise_resolve_undefined, "update", 0));
    JS_SetPropertyStr(ctx, reg, "unregister",
        JS_NewCFunction(ctx, js_true, "unregister", 0));
    
    return reg;
}

// ServiceWorkerContainer.register(scriptURL, options)
GCValue js_service_worker_register(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "requires at least 1 argument(s)");
    
    const char *script_url = JS_ToCString(ctx, argv[0]);
    if (!script_url) script_url = "";
    
    // Parse options for scope
    const char *scope = "/";
    if (argc > 1 && JS_IsObject(argv[1])) {
        GCValue scope_val = JS_GetPropertyStr(ctx, argv[1], "scope");
        if (JS_IsString(scope_val)) {
            scope = JS_ToCString(ctx, scope_val);
        }
    }
    
    // Create registration
    GCValue registration = create_service_worker_registration(ctx, script_url, scope);
    if (JS_IsException(registration)) return JS_EXCEPTION;
    
    // Get container data to store registration
    ServiceWorkerContainerData *swc = NULL;
    if (service_worker_handle != GC_HANDLE_NULL) {
        swc = (ServiceWorkerContainerData*)gc_deref(service_worker_handle);
    }
    
    if (swc) {
        // Add to registrations array
        GCValue regs = swc->registrations;
        if (JS_IsArray(ctx, regs)) {
            // Get current length and push new registration
            GCValue push = JS_GetPropertyStr(ctx, regs, "push");
            GCValue args[1] = { registration };
            JS_Call(ctx, push, regs, 1, args);
        }
    }
    
    // Execute the service worker script
    // Get worker ID from the worker object
    GCValue active_worker = JS_GetPropertyStr(ctx, registration, "active");
    int worker_id = 0;
    if (JS_IsObject(active_worker)) {
        GCHandle worker_handle = JS_GetOpaqueHandle(active_worker, js_service_worker_class_id);
        if (worker_handle != GC_HANDLE_NULL) {
            ServiceWorkerData *worker_data = (ServiceWorkerData*)gc_deref(worker_handle);
            if (worker_data) {
                worker_id = worker_data->id;
            }
        }
    }
    execute_service_worker_script(ctx, script_url, worker_id);
    
    return registration;
}

// ServiceWorkerContainer.getRegistration(clientURL)
GCValue js_service_worker_get_registration(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    
    if (service_worker_handle == GC_HANDLE_NULL) {
        return JS_NULL;
    }
    
    ServiceWorkerContainerData *swc = (ServiceWorkerContainerData*)gc_deref(service_worker_handle);
    if (!swc) return JS_NULL;
    
    // Return the first registration if any
    GCValue regs = swc->registrations;
    if (JS_IsArray(ctx, regs)) {
        GCValue length_val = JS_GetPropertyStr(ctx, regs, "length");
        int length = 0;
        JS_ToInt32(ctx, &length, length_val);
        if (length > 0) {
            return JS_GetPropertyUint32(ctx, regs, 0);
        }
    }
    
    return JS_NULL;
}

// ServiceWorkerContainer.getRegistrations()
GCValue js_service_worker_get_registrations(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    
    if (service_worker_handle == GC_HANDLE_NULL) {
        return JS_NewArray(ctx);
    }
    
    ServiceWorkerContainerData *swc = (ServiceWorkerContainerData*)gc_deref(service_worker_handle);
    if (!swc || !JS_IsArray(ctx, swc->registrations)) {
        return JS_NewArray(ctx);
    }
    
    return swc->registrations;
}

// ServiceWorkerContainer.addEventListener(type, handler)
GCValue js_service_worker_add_event_listener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_UNDEFINED;
    
    // Only handle 'message' events for now
    if (strcmp(type, "message") == 0) {
        if (service_worker_handle != GC_HANDLE_NULL) {
            ServiceWorkerContainerData *swc = (ServiceWorkerContainerData*)gc_deref(service_worker_handle);
            if (swc && JS_IsArray(ctx, swc->message_handlers)) {
                GCValue push = JS_GetPropertyStr(ctx, swc->message_handlers, "push");
                GCValue args[1] = { argv[1] }; // The handler function
                JS_Call(ctx, push, swc->message_handlers, 1, args);
            }
        }
    }
    
    return JS_UNDEFINED;
}

// Post a message to all service worker message handlers
void post_message_to_service_worker(JSContextHandle ctx, GCValue data) {
    if (service_worker_handle == GC_HANDLE_NULL) return;
    
    ServiceWorkerContainerData *swc = (ServiceWorkerContainerData*)gc_deref(service_worker_handle);
    if (!swc || !JS_IsArray(ctx, swc->message_handlers)) return;
    
    // Create MessageEvent-like object
    GCValue event = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, event, "data", data);
    JS_SetPropertyStr(ctx, event, "source", JS_NULL);
    JS_SetPropertyStr(ctx, event, "origin", JS_NewString(ctx, "https://www.youtube.com"));
    
    // Call each handler
    GCValue length_val = JS_GetPropertyStr(ctx, swc->message_handlers, "length");
    int length = 0;
    JS_ToInt32(ctx, &length, length_val);
    
    for (int i = 0; i < length; i++) {
        GCValue handler = JS_GetPropertyUint32(ctx, swc->message_handlers, i);
        if (JS_IsFunction(ctx, handler)) {
            GCValue args[1] = { event };
            JS_Call(ctx, handler, JS_UNDEFINED, 1, args);
        }
    }
}

/* ============================================================================
 * Geolocation API Implementation
 * ============================================================================ */

// GeolocationPosition constructor
GCValue js_geolocation_position_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)new_target; (void)argc; (void)argv;
    GCValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) return JS_EXCEPTION;
    
    // Create coords object
    GCValue coords = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, coords, "latitude", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, coords, "longitude", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, coords, "altitude", JS_NULL);
    JS_SetPropertyStr(ctx, coords, "accuracy", JS_NewFloat64(ctx, 100.0));
    JS_SetPropertyStr(ctx, coords, "altitudeAccuracy", JS_NULL);
    JS_SetPropertyStr(ctx, coords, "heading", JS_NULL);
    JS_SetPropertyStr(ctx, coords, "speed", JS_NULL);
    
    JS_SetPropertyStr(ctx, obj, "coords", coords);
    JS_SetPropertyStr(ctx, obj, "timestamp", JS_NewInt64(ctx, (int64_t)time(NULL) * 1000));
    
    return obj;
}

// GeolocationPositionError constructor
GCValue js_geolocation_position_error_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)new_target;
    GCValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) return JS_EXCEPTION;
    
    int code = 1; // Default to PERMISSION_DENIED
    const char *message = "User denied Geolocation";
    
    if (argc > 0 && JS_IsNumber(argv[0])) {
        JS_ToInt32(ctx, &code, argv[0]);
    }
    if (argc > 1 && JS_IsString(argv[1])) {
        message = JS_ToCString(ctx, argv[1]);
    }
    
    JS_SetPropertyStr(ctx, obj, "code", JS_NewInt32(ctx, code));
    JS_SetPropertyStr(ctx, obj, "message", JS_NewString(ctx, message));
    
    return obj;
}

// Geolocation getCurrentPosition

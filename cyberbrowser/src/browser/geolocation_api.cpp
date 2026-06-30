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

GCValue js_geolocation_get_current_position(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    
    // Create error callback for permission denied
    if (argc > 1 && JS_IsFunction(ctx, argv[1])) {
        // Create GeolocationPositionError
        GCValue error_ctor = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "GeolocationPositionError");
        GCValue error_args[2] = { JS_NewInt32(ctx, 1), JS_NewString(ctx, "User denied Geolocation") };
        GCValue error = JS_CallConstructor(ctx, error_ctor, 2, error_args);
        
        // Call error callback with the error
        GCValue args[1] = { error };
        JS_Call(ctx, argv[1], JS_UNDEFINED, 1, args);
    }
    
    return JS_UNDEFINED;
}

// Geolocation watchPosition
GCValue js_geolocation_watch_position(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    // Return watch ID 0
    return JS_NewInt32(ctx, 0);
}

// User-Agent Client Hints getHighEntropyValues
GCValue js_user_agent_data_get_high_entropy_values(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    GCValue result = JS_NewObject(ctx);
    if (JS_IsException(result)) return JS_EXCEPTION;
    
    JS_SetPropertyStr(ctx, result, "platform", JS_NewString(ctx, "Linux x86_64"));
    JS_SetPropertyStr(ctx, result, "platformVersion", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, result, "architecture", JS_NewString(ctx, "x86"));
    JS_SetPropertyStr(ctx, result, "model", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, result, "uaFullVersion", JS_NewString(ctx, "120.0.0.0"));
    
    return result;
}

// Battery API - getBattery() returns a Promise resolving to a mock battery object
GCValue js_navigator_get_battery(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    // Create a mock battery object
    GCValue battery = JS_NewObject(ctx);
    if (JS_IsException(battery)) return JS_EXCEPTION;
    
    JS_SetPropertyStr(ctx, battery, "charging", JS_TRUE);
    JS_SetPropertyStr(ctx, battery, "chargingTime", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, battery, "dischargingTime", JS_NewFloat64(ctx, INFINITY));
    JS_SetPropertyStr(ctx, battery, "level", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, battery, "addEventListener",
        JS_NewCFunction(ctx, js_undefined, "addEventListener", 2));
    JS_SetPropertyStr(ctx, battery, "removeEventListener",
        JS_NewCFunction(ctx, js_undefined, "removeEventListener", 2));
    
    // Wrap in a Promise (thenable) so code like navigator.getBattery?.().then(...)
    // works without throwing "undefined is not a function".
    GCValue resolving_funcs[2];
    GCValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) return battery; // fallback
    JS_Call(ctx, resolving_funcs[0], JS_UNDEFINED, 1, &battery);
    return promise;
}

// History pushState - stores the state object on history.state
GCValue js_history_push_state(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    // First argument is the state object to store
    if (argc > 0) {
        // Get the global object to access window.history
        GCValue global = JS_GetGlobalObject(ctx);
        GCValue history = JS_GetPropertyStr(ctx, global, "history");
        if (!JS_IsException(history)) {
            // Store the state on history.state
            JS_SetPropertyStr(ctx, history, "state", argv[0]);
        }
    }
    return JS_UNDEFINED;
}

// History replaceState - same as pushState for our stub
GCValue js_history_replace_state(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    // Same implementation as pushState for our stub
    if (argc > 0) {
        GCValue global = JS_GetGlobalObject(ctx);
        GCValue history = JS_GetPropertyStr(ctx, global, "history");
        if (!JS_IsException(history)) {
            JS_SetPropertyStr(ctx, history, "state", argv[0]);
        }
    }
    return JS_UNDEFINED;
}

// MediaMetadata constructor
GCValue js_media_metadata_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)new_target;
    GCValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) return JS_EXCEPTION;
    
    // Default values
    const char *title = "";
    const char *artist = "";
    const char *album = "";
    
    // Parse init object if provided
    if (argc > 0 && JS_IsObject(argv[0])) {
        GCValue title_val = JS_GetPropertyStr(ctx, argv[0], "title");
        GCValue artist_val = JS_GetPropertyStr(ctx, argv[0], "artist");
        GCValue album_val = JS_GetPropertyStr(ctx, argv[0], "album");
        GCValue artwork_val = JS_GetPropertyStr(ctx, argv[0], "artwork");
        
        if (JS_IsString(title_val)) {
            title = JS_ToCString(ctx, title_val);
        }
        if (JS_IsString(artist_val)) {
            artist = JS_ToCString(ctx, artist_val);
        }
        if (JS_IsString(album_val)) {
            album = JS_ToCString(ctx, album_val);
        }
        if (JS_IsArray(ctx, artwork_val)) {
            JS_SetPropertyStr(ctx, obj, "artwork", artwork_val);
        } else {
            JS_SetPropertyStr(ctx, obj, "artwork", JS_NewArray(ctx));
        }
    } else {
        JS_SetPropertyStr(ctx, obj, "artwork", JS_NewArray(ctx));
    }
    
    JS_SetPropertyStr(ctx, obj, "title", JS_NewString(ctx, title));
    JS_SetPropertyStr(ctx, obj, "artist", JS_NewString(ctx, artist));
    JS_SetPropertyStr(ctx, obj, "album", JS_NewString(ctx, album));
    
    return obj;
}

// getSelection() - returns a Selection-like object

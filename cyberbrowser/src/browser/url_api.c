/* Auto-generated split from browser_api_impl.c */
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
#include "url_utils.h"
#include "url_whatwg.h"

// URL.createObjectURL / revokeObjectURL.  URL, URLSearchParams and the
// URL attributes of <a>/<area> are in url_classes.c.
// ============================================================================

GCValue js_url_create_object_url(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "requires at least 1 argument(s)");

    GCValue obj = argv[0];
    static int blob_counter = 0;
    char blob_url[512];

    const char *origin = "";
    GCValue global_obj = JS_GetGlobalObject(ctx);
    GCValue location = JS_GetPropertyStr(ctx, global_obj, "location");
    if (!JS_IsUndefined(location) && !JS_IsNull(location)) {
        GCValue origin_val = JS_GetPropertyStr(ctx, location, "origin");
        if (!JS_IsUndefined(origin_val) && !JS_IsNull(origin_val)) {
            const char *s = JS_ToCString(ctx, origin_val);
            if (s) origin = s;
        }
    }

    bool is_media_source = false;
    MediaSourceDataHandle ms = JS_GetOpaqueHandle(obj, js_media_source_class_id);
    if (!SAFE_IS_NULL(ms)) {
        is_media_source = true;
    }

    if (origin && origin[0]) {
        snprintf(blob_url, sizeof(blob_url), "blob:%s/%s%d", origin,
                 is_media_source ? "ms-" : "", ++blob_counter);
    } else {
        snprintf(blob_url, sizeof(blob_url), "blob:cyberbrowser/%s%d",
                 is_media_source ? "ms-" : "", ++blob_counter);
    }

    GCValue registry = JS_GetPropertyStr(ctx, global_obj, "__blobRegistry");
    if (!JS_IsUndefined(registry) && !JS_IsNull(registry)) {
        JS_SetPropertyStr(ctx, registry, blob_url, obj);
    }

    if (is_media_source) {
        JS_SetPropertyStr(ctx, obj, "__blobUrl", JS_NewString(ctx, blob_url));
    }

    capture_url_debug(blob_url, is_media_source ? "url_create_object_url_ms" : "url_create_object_url_generic");
    return JS_NewString(ctx, blob_url);
}

// URL.revokeObjectURL()
GCValue js_url_revoke_object_url(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_UNDEFINED;
    GCValue global_obj = JS_GetGlobalObject(ctx);
    GCValue registry = JS_GetPropertyStr(ctx, global_obj, "__blobRegistry");
    if (!JS_IsUndefined(registry) && !JS_IsNull(registry)) {
        JSAtom url_atom = JS_NewAtom(ctx, url);
        JS_DeleteProperty(ctx, registry, url_atom, 0);
        JS_FreeAtom(ctx, url_atom);
    }
    return JS_UNDEFINED;
}


void url_api_install(JSContextHandle ctx, GCValue global)
{
    url_classes_install(ctx, global);
}

// ============================================================================
// Request/Response API (for fetch)
// ============================================================================

GCValue js_request_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "Request constructor requires at least 1 argument");
    }
    
    GCValue request_obj = JS_NewObject(ctx);
    
    // Handle Request object or URL string
    if (JS_IsObject(argv[0])) {
        // Copy from existing Request
        GCValue url_val = JS_GetPropertyStr(ctx, argv[0], "url");
        JS_SetPropertyStr(ctx, request_obj, "url", url_val);
        GCValue method_val = JS_GetPropertyStr(ctx, argv[0], "method");
        JS_SetPropertyStr(ctx, request_obj, "method", method_val);
    } else {
        // URL string
        const char *url_str = JS_ToCString(ctx, argv[0]);
        if (url_str) {
            JS_SetPropertyStr(ctx, request_obj, "url", JS_NewString(ctx, url_str));
            // Capture the URL
            capture_url_debug(url_str, "request_ctor");
        }
        JS_SetPropertyStr(ctx, request_obj, "method", JS_NewString(ctx, "GET"));
    }
    
    // Handle init options
    if (argc > 1 && JS_IsObject(argv[1])) {
        GCValue method_val = JS_GetPropertyStr(ctx, argv[1], "method");
        if (!JS_IsUndefined(method_val)) {
            JS_SetPropertyStr(ctx, request_obj, "method", method_val);
        }
        GCValue headers_val = JS_GetPropertyStr(ctx, argv[1], "headers");
        if (!JS_IsUndefined(headers_val)) {
            JS_SetPropertyStr(ctx, request_obj, "headers", headers_val);
        }
        GCValue body_val = JS_GetPropertyStr(ctx, argv[1], "body");
        if (!JS_IsUndefined(body_val)) {
            JS_SetPropertyStr(ctx, request_obj, "body", body_val);
        }
    }
    
    // Set default headers only if not already set
    GCValue existing_headers = JS_GetPropertyStr(ctx, request_obj, "headers");
    if (JS_IsUndefined(existing_headers) || JS_IsNull(existing_headers)) {
        JS_SetPropertyStr(ctx, request_obj, "headers", JS_NewObject(ctx));
    }
    
    return request_obj;
}

// Response constructor
GCValue js_response_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    GCValue response_obj = JS_NewObject(ctx);
    
    // Set status
    int status = 200;
    if (argc > 1) {
        JS_ToInt32(ctx, &status, argv[1]);
    }
    JS_SetPropertyStr(ctx, response_obj, "status", JS_NewInt32(ctx, status));
    JS_SetPropertyStr(ctx, response_obj, "ok", JS_NewBool(ctx, status >= 200 && status < 300));
    JS_SetPropertyStr(ctx, response_obj, "statusText", JS_NewString(ctx, "OK"));
    
    // Set body
    if (argc > 0) {
        JS_SetPropertyStr(ctx, response_obj, "body", argv[0]);
    }
    
    // Headers
    JS_SetPropertyStr(ctx, response_obj, "headers", JS_NewObject(ctx));
    
    return response_obj;
}

// Response.json() static method
GCValue js_response_json(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Return a promise that resolves to the JSON
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    GCValue promise = JS_CallConstructor(ctx, promise_ctor, 0, NULL);
    return promise;
}

// ============================================================================
// Navigator sendBeacon
// ============================================================================


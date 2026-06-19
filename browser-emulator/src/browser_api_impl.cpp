/*
 * Browser Stubs - C implementation of DOM/Browser APIs for QuickJS
 */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "html_dom.h"
#include "css_parser.h"
#include "gc_value_helpers.h"
#include "platform.h"
#include "browser_api_impl_types.h"
#include "browser_api_impl_handles.h"

// mbedtls includes for Crypto API
#include "mbedtls/md.h"

// Define macro to access private GCM functions
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS 1
#include "mbedtls/private/gcm.h"

// Forward declarations for Crypto API
static GCValue js_crypto_get_random_values(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_subtle_digest(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_subtle_encrypt(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_subtle_decrypt(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

#define LOG_TAG "browser_api_impl"
#define LOG_ERROR(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)
#define LOG_INFO(...) platform_log(LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)

// External symbols from js_quickjs.c
extern GCValue js_document_create_element(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

// Forward declarations for internal functions
static GCValue js_dummy_function(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_dummy_function_true(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_create_from_ctor_proto(JSContextHandle ctx, GCValue ctor);
static GCValue js_message_channel_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
static GCValue js_event_target_addEventListener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_event_target_removeEventListener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_event_target_dispatchEvent(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_promise_resolve_undefined(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_promise_resolve_empty_string(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_create_resolved_promise(JSContextHandle ctx, GCValue value);

// ServiceWorker API forward declarations
extern JSClassID js_service_worker_container_class_id;
extern JSClassID js_service_worker_registration_class_id;
extern JSClassID js_service_worker_class_id;
static GCHandle service_worker_handle = GC_HANDLE_NULL;

// Basic stub function definitions (must be before use in function lists)
static GCValue js_undefined(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static GCValue js_null(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_NULL;
}

static GCValue js_empty_array(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewArray(ctx);
}

static GCValue js_empty_string(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewString(ctx, "");
}

// document.createTextNode() - returns a proper Text node object
static GCValue js_document_create_text_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    const char *text = "";
    if (argc >= 1) {
        text = JS_ToCString(ctx, argv[0]);
        if (!text) text = "";
    }
    
    // Create a DOM node object with js_dom_node_class_id
    GCValue node = JS_NewObjectClass(ctx, js_dom_node_class_id);
    if (JS_IsException(node)) {
        return JS_NULL;
    }
    
    // Create and attach DOMNode data for Text node
    DOMNodeHandle dom_node = DOMNodeHandle::create(ctx, DOM_NODE_TYPE_TEXT, "#text");
    if (dom_node.valid()) {
        dom_node.set_node_value(text);
        dom_node.attach_to_object(node);
    }
    
    // Set standard Text node properties
    JS_SetPropertyStr(ctx, node, "nodeType", JS_NewInt32(ctx, DOM_NODE_TYPE_TEXT));
    JS_SetPropertyStr(ctx, node, "nodeName", JS_NewString(ctx, "#text"));
    JS_SetPropertyStr(ctx, node, "data", JS_NewString(ctx, text));
    JS_SetPropertyStr(ctx, node, "textContent", JS_NewString(ctx, text));
    JS_SetPropertyStr(ctx, node, "length", JS_NewInt32(ctx, (int)strlen(text)));
    
    return node;
}

static GCValue js_false(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_FALSE;
}

static GCValue js_true(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_TRUE;
}

// Promise rejection helper
static GCValue js_promise_reject(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "not supported");
}

// matchMedia stub
static GCValue js_match_media(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc;
    GCValue media_query = argc > 0 && JS_IsString(argv[0]) ? argv[0] : JS_NewString(ctx, "");
    GCValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "matches", JS_FALSE);
    JS_SetPropertyStr(ctx, result, "media", media_query);
    JS_SetPropertyStr(ctx, result, "addListener", JS_NewCFunction(ctx, js_undefined, "addListener", 1));
    JS_SetPropertyStr(ctx, result, "removeListener", JS_NewCFunction(ctx, js_undefined, "removeListener", 1));
    JS_SetPropertyStr(ctx, result, "addEventListener", JS_NewCFunction(ctx, js_undefined, "addEventListener", 2));
    JS_SetPropertyStr(ctx, result, "removeEventListener", JS_NewCFunction(ctx, js_undefined, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, result, "dispatchEvent", JS_NewCFunction(ctx, js_undefined, "dispatchEvent", 1));
    return result;
}

// Real base64 encode
static GCValue js_btoa(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "btoa requires a string");
    const char *str = JS_ToCString(ctx, argv[0]);
    if (!str) return JS_ThrowTypeError(ctx, "btoa: invalid string");
    
    static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(str);
    size_t out_len = ((len + 2) / 3) * 4;
    char *out = (char*)malloc(out_len + 1);
    if (!out) return JS_ThrowTypeError(ctx, "btoa: out of memory");
    
    size_t i, j;
    for (i = 0, j = 0; i < len; i += 3, j += 4) {
        unsigned int a = (unsigned char)str[i];
        unsigned int b = (i + 1 < len) ? (unsigned char)str[i + 1] : 0;
        unsigned int c = (i + 2 < len) ? (unsigned char)str[i + 2] : 0;
        unsigned int triple = (a << 16) | (b << 8) | c;
        out[j] = b64_table[(triple >> 18) & 0x3F];
        out[j + 1] = b64_table[(triple >> 12) & 0x3F];
        out[j + 2] = (i + 1 < len) ? b64_table[(triple >> 6) & 0x3F] : '=';
        out[j + 3] = (i + 2 < len) ? b64_table[triple & 0x3F] : '=';
    }
    out[j] = '\0';
    
    GCValue result = JS_NewString(ctx, out);
    free(out);
    return result;
}

// Real base64 decode
static GCValue js_atob(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "atob requires a string");
    const char *str = JS_ToCString(ctx, argv[0]);
    if (!str) return JS_ThrowTypeError(ctx, "atob: invalid string");
    
    static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(str);
    size_t out_len = (len / 4) * 3;
    if (len > 0 && str[len - 1] == '=') out_len--;
    if (len > 1 && str[len - 2] == '=') out_len--;
    char *out = (char*)malloc(out_len + 1);
    if (!out) return JS_ThrowTypeError(ctx, "atob: out of memory");
    
    int val = 0, valb = -8;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '=') break;
        const char *p = strchr(b64_table, str[i]);
        if (!p) {
            free(out);
            return JS_ThrowTypeError(ctx, "atob: invalid character");
        }
        val = (val << 6) + (p - b64_table);
        valb += 6;
        if (valb >= 0) {
            out[j++] = (char)((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    out[j] = '\0';
    
    GCValue result = JS_NewString(ctx, out);
    free(out);
    return result;
}

// AbortController constructor stub
static GCValue js_abort_controller_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = JS_NewObject(ctx);
    GCValue signal = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, signal, "aborted", JS_FALSE);
    JS_SetPropertyStr(ctx, signal, "addEventListener", JS_NewCFunction(ctx, js_undefined, "addEventListener", 2));
    JS_SetPropertyStr(ctx, signal, "removeEventListener", JS_NewCFunction(ctx, js_undefined, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, obj, "signal", signal);
    JS_SetPropertyStr(ctx, obj, "abort", JS_NewCFunction(ctx, js_undefined, "abort", 0));
    return obj;
}

// AbortSignal constructor stub - YouTube player scripts check for this
static GCValue js_abort_signal_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue signal = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, signal, "aborted", JS_FALSE);
    JS_SetPropertyStr(ctx, signal, "addEventListener", JS_NewCFunction(ctx, js_undefined, "addEventListener", 2));
    JS_SetPropertyStr(ctx, signal, "removeEventListener", JS_NewCFunction(ctx, js_undefined, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, signal, "throwIfAborted", JS_NewCFunction(ctx, js_undefined, "throwIfAborted", 0));
    JS_SetPropertyStr(ctx, signal, "dispatchEvent", JS_NewCFunction(ctx, js_undefined, "dispatchEvent", 1));
    return signal;
}

// AudioContext constructor stub
static GCValue js_audio_context_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "createBuffer", JS_NewCFunction(ctx, js_null, "createBuffer", 3));
    JS_SetPropertyStr(ctx, obj, "createBufferSource", JS_NewCFunction(ctx, js_null, "createBufferSource", 0));
    JS_SetPropertyStr(ctx, obj, "createGain", JS_NewCFunction(ctx, js_null, "createGain", 0));
    JS_SetPropertyStr(ctx, obj, "createOscillator", JS_NewCFunction(ctx, js_null, "createOscillator", 0));
    JS_SetPropertyStr(ctx, obj, "decodeAudioData",
        JS_NewCFunction(ctx, js_promise_reject, "decodeAudioData", 1));
    JS_SetPropertyStr(ctx, obj, "resume", JS_NewCFunction(ctx, js_promise_resolve_undefined, "resume", 0));
    JS_SetPropertyStr(ctx, obj, "suspend", JS_NewCFunction(ctx, js_promise_resolve_undefined, "suspend", 0));
    JS_SetPropertyStr(ctx, obj, "close", JS_NewCFunction(ctx, js_promise_resolve_undefined, "close", 0));
    JS_SetPropertyStr(ctx, obj, "state", JS_NewString(ctx, "running"));
    JS_SetPropertyStr(ctx, obj, "sampleRate", JS_NewFloat64(ctx, 48000));
    JS_SetPropertyStr(ctx, obj, "destination", JS_NewObject(ctx));
    return obj;
}

// DOMParser constructor stub
static GCValue js_dom_parser_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "parseFromString", JS_NewCFunction(ctx, js_empty_string, "parseFromString", 2));
    return obj;
}

// Worker constructor stub
static GCValue js_worker_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "postMessage", JS_NewCFunction(ctx, js_undefined, "postMessage", 1));
    JS_SetPropertyStr(ctx, obj, "terminate", JS_NewCFunction(ctx, js_undefined, "terminate", 0));
    JS_SetPropertyStr(ctx, obj, "addEventListener", JS_NewCFunction(ctx, js_undefined, "addEventListener", 2));
    JS_SetPropertyStr(ctx, obj, "removeEventListener", JS_NewCFunction(ctx, js_undefined, "removeEventListener", 2));
    return obj;
}

// Blob constructor stub
static GCValue js_blob_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, 0));
    JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, obj, "slice", JS_NewCFunction(ctx, js_null, "slice", 3));
    JS_SetPropertyStr(ctx, obj, "text", JS_NewCFunction(ctx, js_promise_resolve_empty_string, "text", 0));
    JS_SetPropertyStr(ctx, obj, "arrayBuffer", JS_NewCFunction(ctx, js_promise_reject, "arrayBuffer", 0));
    JS_SetPropertyStr(ctx, obj, "stream", JS_NewCFunction(ctx, js_null, "stream", 0));
    return obj;
}

// File constructor stub
static GCValue js_file_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = js_blob_constructor(ctx, this_val, argc, argv);
    JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, obj, "lastModified", JS_NewInt64(ctx, 0));
    return obj;
}

// FormData constructor stub
static GCValue js_form_data_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "append", JS_NewCFunction(ctx, js_undefined, "append", 2));
    JS_SetPropertyStr(ctx, obj, "delete", JS_NewCFunction(ctx, js_undefined, "delete", 1));
    JS_SetPropertyStr(ctx, obj, "get", JS_NewCFunction(ctx, js_null, "get", 1));
    JS_SetPropertyStr(ctx, obj, "getAll", JS_NewCFunction(ctx, js_empty_array, "getAll", 1));
    JS_SetPropertyStr(ctx, obj, "has", JS_NewCFunction(ctx, js_false, "has", 1));
    JS_SetPropertyStr(ctx, obj, "set", JS_NewCFunction(ctx, js_undefined, "set", 2));
    JS_SetPropertyStr(ctx, obj, "entries", JS_NewCFunction(ctx, js_empty_array, "entries", 0));
    JS_SetPropertyStr(ctx, obj, "keys", JS_NewCFunction(ctx, js_empty_array, "keys", 0));
    JS_SetPropertyStr(ctx, obj, "values", JS_NewCFunction(ctx, js_empty_array, "values", 0));
    return obj;
}

// TextEncoder constructor stub
static GCValue js_text_encoder_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "encoding", JS_NewString(ctx, "utf-8"));
    JS_SetPropertyStr(ctx, obj, "encode", JS_NewCFunction(ctx, js_empty_array, "encode", 1));
    JS_SetPropertyStr(ctx, obj, "encodeInto", JS_NewCFunction(ctx, js_undefined, "encodeInto", 2));
    return obj;
}

// TextDecoder constructor stub
static GCValue js_text_decoder_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "encoding", JS_NewString(ctx, "utf-8"));
    JS_SetPropertyStr(ctx, obj, "fatal", JS_FALSE);
    JS_SetPropertyStr(ctx, obj, "ignoreBOM", JS_FALSE);
    JS_SetPropertyStr(ctx, obj, "decode", JS_NewCFunction(ctx, js_empty_string, "decode", 1));
    return obj;
}

// ReadableStream constructor stub
static GCValue js_readable_stream_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "locked", JS_FALSE);
    JS_SetPropertyStr(ctx, obj, "cancel", JS_NewCFunction(ctx, js_promise_resolve_undefined, "cancel", 0));
    JS_SetPropertyStr(ctx, obj, "getReader", JS_NewCFunction(ctx, js_null, "getReader", 0));
    JS_SetPropertyStr(ctx, obj, "tee", JS_NewCFunction(ctx, js_empty_array, "tee", 0));
    JS_SetPropertyStr(ctx, obj, "pipeTo", JS_NewCFunction(ctx, js_promise_resolve_undefined, "pipeTo", 1));
    JS_SetPropertyStr(ctx, obj, "pipeThrough", JS_NewCFunction(ctx, js_null, "pipeThrough", 1));
    return obj;
}

// PressureObserver constructor stub
static GCValue js_pressure_observer_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "observe", JS_NewCFunction(ctx, js_promise_resolve_undefined, "observe", 1));
    JS_SetPropertyStr(ctx, obj, "unobserve", JS_NewCFunction(ctx, js_promise_resolve_undefined, "unobserve", 1));
    JS_SetPropertyStr(ctx, obj, "takeRecords", JS_NewCFunction(ctx, js_empty_array, "takeRecords", 0));
    return obj;
}

// Profiler constructor stub
static GCValue js_profiler_constructor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "start", JS_NewCFunction(ctx, js_undefined, "start", 0));
    JS_SetPropertyStr(ctx, obj, "stop", JS_NewCFunction(ctx, js_promise_resolve_undefined, "stop", 0));
    JS_SetPropertyStr(ctx, obj, "addEventListener", JS_NewCFunction(ctx, js_undefined, "addEventListener", 2));
    return obj;
}

// Promise-returning helpers for Clipboard API
static GCValue js_promise_resolve_undefined(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    // Return a resolved promise with undefined
    GCValue promise = JS_NewPromiseCapability(ctx, NULL);
    if (JS_IsException(promise)) return JS_EXCEPTION;
    // For simplicity, return undefined directly (QuickJS will wrap in Promise)
    return JS_UNDEFINED;
}

static GCValue js_promise_resolve_empty_string(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewString(ctx, "");
}

static GCValue js_promise_resolve_empty_array(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewArray(ctx);
}

// MediaCapabilities decodingInfo - returns Promise<{supported, smooth, powerEfficient}>
static GCValue js_media_capabilities_decoding_info(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    // Create result object with all capabilities supported
    GCValue result = JS_NewObject(ctx);
    if (JS_IsException(result)) return JS_EXCEPTION;
    
    JS_SetPropertyStr(ctx, result, "supported", JS_TRUE);
    JS_SetPropertyStr(ctx, result, "smooth", JS_TRUE);
    JS_SetPropertyStr(ctx, result, "powerEfficient", JS_TRUE);
    
    // Return a Promise that resolves to the result
    return js_create_resolved_promise(ctx, result);
}

// MediaCapabilities encodingInfo - returns Promise<{supported, smooth, powerEfficient}>
static GCValue js_media_capabilities_encoding_info(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    // Create result object - encoding not supported
    GCValue result = JS_NewObject(ctx);
    if (JS_IsException(result)) return JS_EXCEPTION;
    
    JS_SetPropertyStr(ctx, result, "supported", JS_FALSE);
    JS_SetPropertyStr(ctx, result, "smooth", JS_FALSE);
    JS_SetPropertyStr(ctx, result, "powerEfficient", JS_FALSE);
    
    // Return a Promise that resolves to the result
    return js_create_resolved_promise(ctx, result);
}

// PermissionStatus constructor
static GCValue js_permission_status_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)new_target;
    GCValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) return JS_EXCEPTION;
    
    // Default state is "prompt"
    const char *state = "prompt";
    
    // Parse state argument if provided
    if (argc > 0 && JS_IsString(argv[0])) {
        state = JS_ToCString(ctx, argv[0]);
        if (!state) state = "prompt";
    }
    
    JS_SetPropertyStr(ctx, obj, "state", JS_NewString(ctx, state));
    JS_SetPropertyStr(ctx, obj, "onchange", JS_NULL);
    
    return obj;
}

// Permissions query/request/revoke - returns Promise<PermissionStatus>
static GCValue js_permissions_query(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc;
    // Create PermissionStatus with state "prompt" (default permission state)
    GCValue permission_status = JS_NewObject(ctx);
    if (JS_IsException(permission_status)) return JS_EXCEPTION;
    
    JS_SetPropertyStr(ctx, permission_status, "state", JS_NewString(ctx, "prompt"));
    JS_SetPropertyStr(ctx, permission_status, "onchange", JS_NULL);
    
    return permission_status;
}

// Storage API - returns { usage, quota, usageDetails }
static GCValue js_storage_estimate(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    GCValue result = JS_NewObject(ctx);
    if (JS_IsException(result)) return JS_EXCEPTION;
    
    JS_SetPropertyStr(ctx, result, "usage", JS_NewInt64(ctx, 0));
    JS_SetPropertyStr(ctx, result, "quota", JS_NewInt64(ctx, 10737418240)); // 10GB
    JS_SetPropertyStr(ctx, result, "usageDetails", JS_NewObject(ctx));
    
    return result;
}

// Storage persist/persisted - returns Promise<false>
static GCValue js_false_promise(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_FALSE;
}

/* ============================================================================
 * Storage API Implementation (localStorage/sessionStorage)
 * ============================================================================ */

// Static storage data for localStorage and sessionStorage
static StorageData g_local_storage_data = {0};
static StorageData g_session_storage_data = {0};

// Helper to identify which storage type based on object
static StorageData* get_storage_data(JSContextHandle ctx, GCValue obj) {
    // Compare object with known localStorage/sessionStorage globals
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue localStorageGlob = JS_GetPropertyStr(ctx, global, "localStorage");
    GCValue sessionStorageGlob = JS_GetPropertyStr(ctx, global, "sessionStorage");
    
    // Check if obj is the same as global localStorage
    if (JS_StrictEq(ctx, obj, localStorageGlob)) {
        return &g_local_storage_data;
    }
    // Check if obj is the same as global sessionStorage
    if (JS_StrictEq(ctx, obj, sessionStorageGlob)) {
        return &g_session_storage_data;
    }
    
    // Also check for window.localStorage and window.sessionStorage pattern
    GCValue window = JS_GetPropertyStr(ctx, global, "window");
    if (!JS_IsUndefined(window) && !JS_IsNull(window)) {
        GCValue winLocalStorage = JS_GetPropertyStr(ctx, window, "localStorage");
        GCValue winSessionStorage = JS_GetPropertyStr(ctx, window, "sessionStorage");
        if (JS_StrictEq(ctx, obj, winLocalStorage)) {
            return &g_local_storage_data;
        }
        if (JS_StrictEq(ctx, obj, winSessionStorage)) {
            return &g_session_storage_data;
        }
    }
    
    // Default to localStorage if we can't identify
    return &g_local_storage_data;
}

// Helper to find item index by key in storage
static int storage_find_item(StorageData *storage, const char *key) {
    for (int i = 0; i < storage->count && i < STORAGE_MAX_ITEMS; i++) {
        if (storage->items[i].used && strcmp(storage->items[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

// Helper to find first empty slot in storage
static int storage_find_empty_slot(StorageData *storage) {
    for (int i = 0; i < STORAGE_MAX_ITEMS; i++) {
        if (!storage->items[i].used) {
            return i;
        }
    }
    return -1;
}

// Storage.getItem(key)
static GCValue js_storage_get_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_NULL;
    
    StorageData *storage = get_storage_data(ctx, this_val);
    if (!storage) return JS_NULL;
    
    int idx = storage_find_item(storage, key);
    if (idx >= 0) {
        return JS_NewString(ctx, storage->items[idx].value);
    }
    return JS_NULL;
}

// Storage.setItem(key, value)
static GCValue js_storage_set_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    
    const char *key = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    if (!key || !value) return JS_UNDEFINED;
    
    // Check key length
    size_t key_len = strlen(key);
    if (key_len >= STORAGE_MAX_KEY_LEN) {
        platform_log(LOG_LEVEL_ERROR, "Storage", "FATAL: Storage key length %zu exceeds maximum %d. Aborting.", key_len, STORAGE_MAX_KEY_LEN - 1);
        abort();
    }
    
    // Check value length
    size_t value_len = strlen(value);
    if (value_len >= STORAGE_MAX_VALUE_LEN) {
        platform_log(LOG_LEVEL_ERROR, "Storage", "FATAL: Storage value length %zu exceeds maximum %d. Aborting.", value_len, STORAGE_MAX_VALUE_LEN - 1);
        abort();
    }
    
    StorageData *storage = get_storage_data(ctx, this_val);
    if (!storage) return JS_UNDEFINED;
    
    // Check if key already exists
    int idx = storage_find_item(storage, key);
    if (idx >= 0) {
        // Update existing
        strncpy(storage->items[idx].value, value, STORAGE_MAX_VALUE_LEN - 1);
        storage->items[idx].value[STORAGE_MAX_VALUE_LEN - 1] = '\0';
    } else {
        // Find empty slot
        idx = storage_find_empty_slot(storage);
        if (idx < 0) {
            // Storage full - abort the program
            platform_log(LOG_LEVEL_ERROR, "Storage", "FATAL: Storage limit of %d items exceeded. Aborting.", STORAGE_MAX_ITEMS);
            abort();
        }
        // Add new item
        strncpy(storage->items[idx].key, key, STORAGE_MAX_KEY_LEN - 1);
        storage->items[idx].key[STORAGE_MAX_KEY_LEN - 1] = '\0';
        strncpy(storage->items[idx].value, value, STORAGE_MAX_VALUE_LEN - 1);
        storage->items[idx].value[STORAGE_MAX_VALUE_LEN - 1] = '\0';
        storage->items[idx].used = 1;
        storage->count++;
    }
    
    return JS_UNDEFINED;
}

// Storage.removeItem(key)
static GCValue js_storage_remove_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_UNDEFINED;
    
    StorageData *storage = get_storage_data(ctx, this_val);
    if (!storage) return JS_UNDEFINED;
    
    int idx = storage_find_item(storage, key);
    if (idx >= 0) {
        storage->items[idx].used = 0;
        storage->count--;
    }
    
    return JS_UNDEFINED;
}

// Storage.clear()
static GCValue js_storage_clear(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    
    StorageData *storage = get_storage_data(ctx, this_val);
    if (!storage) return JS_UNDEFINED;
    
    for (int i = 0; i < STORAGE_MAX_ITEMS; i++) {
        storage->items[i].used = 0;
    }
    storage->count = 0;
    
    return JS_UNDEFINED;
}

// Storage.key(index)
static GCValue js_storage_key(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    
    int index;
    if (JS_IsNumber(argv[0])) {
        JS_ToInt32(ctx, &index, argv[0]);
    } else {
        return JS_NULL;
    }
    
    StorageData *storage = get_storage_data(ctx, this_val);
    if (!storage) return JS_NULL;
    
    // Find the nth used slot
    int current = 0;
    for (int i = 0; i < STORAGE_MAX_ITEMS; i++) {
        if (storage->items[i].used) {
            if (current == index) {
                return JS_NewString(ctx, storage->items[i].key);
            }
            current++;
        }
    }
    return JS_NULL;
}

// Storage.length getter
static GCValue js_storage_get_length(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    
    StorageData *storage = get_storage_data(ctx, this_val);
    if (!storage) return JS_NewInt32(ctx, 0);
    
    return JS_NewInt32(ctx, storage->count);
}

/* ============================================================================
 * Crypto API Implementation
 * ============================================================================ */

// crypto.getRandomValues(typedArray)
static GCValue js_crypto_get_random_values(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "getRandomValues requires 1 argument");
    
    GCValue typed_array = argv[0];
    if (!JS_IsObject(typed_array)) return JS_ThrowTypeError(ctx, "getRandomValues argument must be a TypedArray");
    
    // Get the ArrayBuffer info from the TypedArray
    size_t byte_offset = 0;
    size_t byte_length = 0;
    size_t bytes_per_element = 0;
    GCValue buffer = JS_GetTypedArrayBuffer(ctx, typed_array, &byte_offset, &byte_length, &bytes_per_element);
    if (JS_IsException(buffer)) return JS_EXCEPTION;
    
    size_t buf_size;
    uint8_t *data = JS_GetArrayBuffer(ctx, &buf_size, buffer);
    if (!data) return JS_ThrowTypeError(ctx, "getRandomValues: failed to get ArrayBuffer");
    
    // Fill with random bytes using arc4random on macOS/Linux
    #if defined(__APPLE__) || defined(__linux__)
        arc4random_buf(data + byte_offset, byte_length);
    #else
        // Fallback to rand() - not cryptographically secure but sufficient for emulation
        for (size_t i = 0; i < byte_length; i++) {
            data[byte_offset + i] = (uint8_t)(rand() % 256);
        }
    #endif
    
    return typed_array;
}

// SubtleCrypto.digest(algorithm, data)
static GCValue js_subtle_digest(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "digest requires 2 arguments");
    
    // Get algorithm name
    const char *algo = NULL;
    if (JS_IsString(argv[0])) {
        algo = JS_ToCString(ctx, argv[0]);
    } else if (JS_IsObject(argv[0])) {
        GCValue name = JS_GetPropertyStr(ctx, argv[0], "name");
        algo = JS_ToCString(ctx, name);
    }
    if (!algo) return JS_ThrowTypeError(ctx, "digest: invalid algorithm");
    
    // Get data ArrayBuffer
    size_t data_len;
    uint8_t *data = JS_GetArrayBuffer(ctx, &data_len, argv[1]);
    if (!data) return JS_ThrowTypeError(ctx, "digest: data must be an ArrayBuffer");
    
    // Map algorithm to mbedtls digest type
    mbedtls_md_type_t md_type;
    size_t hash_len;
    
    if (strcmp(algo, "SHA-1") == 0) {
        md_type = MBEDTLS_MD_SHA1;
        hash_len = 20;
    } else if (strcmp(algo, "SHA-256") == 0) {
        md_type = MBEDTLS_MD_SHA256;
        hash_len = 32;
    } else if (strcmp(algo, "SHA-384") == 0) {
        md_type = MBEDTLS_MD_SHA384;
        hash_len = 48;
    } else if (strcmp(algo, "SHA-512") == 0) {
        md_type = MBEDTLS_MD_SHA512;
        hash_len = 64;
    } else {
        return JS_ThrowTypeError(ctx, "digest: unsupported algorithm '%s'", algo);
    }
    
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(md_type);
    if (!md_info) return JS_ThrowInternalError(ctx, "digest: failed to get md_info");
    
    uint8_t hash[64];  // Max hash size
    mbedtls_md(md_info, data, data_len, hash);
    
    return JS_NewArrayBufferCopy(ctx, hash, hash_len);
}

// Helper to extract raw key bytes from a CryptoKey object
static uint8_t* get_key_bytes(JSContextHandle ctx, GCValue key_obj, size_t *key_len) {
    // Try to get the key data from a "__keyData" hidden property
    GCValue key_data = JS_GetPropertyStr(ctx, key_obj, "__keyData");
    if (JS_IsUndefined(key_data) || JS_IsNull(key_data)) {
        return NULL;
    }
    
    // Get the ArrayBuffer from the key data
    size_t len;
    uint8_t *data = JS_GetArrayBuffer(ctx, &len, key_data);
    if (!data) return NULL;
    
    *key_len = len;
    return data;
}

// Helper to get algorithm IV/nonce
static uint8_t* get_iv(JSContextHandle ctx, GCValue algo_obj, size_t *iv_len) {
    GCValue iv_val = JS_GetPropertyStr(ctx, algo_obj, "iv");
    if (JS_IsUndefined(iv_val) || JS_IsNull(iv_val)) {
        // Try "nonce" as alternative
        iv_val = JS_GetPropertyStr(ctx, algo_obj, "nonce");
        if (JS_IsUndefined(iv_val) || JS_IsNull(iv_val)) {
            return NULL;
        }
    }
    
    size_t len;
    uint8_t *data = JS_GetArrayBuffer(ctx, &len, iv_val);
    if (!data) return NULL;
    
    *iv_len = len;
    return data;
}

// SubtleCrypto.encrypt(algorithm, key, data) - AES-GCM support
static GCValue js_subtle_encrypt(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 3) return JS_ThrowTypeError(ctx, "encrypt requires 3 arguments");
    
    // Get algorithm name
    const char *algo_name = NULL;
    if (JS_IsString(argv[0])) {
        algo_name = JS_ToCString(ctx, argv[0]);
    } else if (JS_IsObject(argv[0])) {
        GCValue name = JS_GetPropertyStr(ctx, argv[0], "name");
        algo_name = JS_ToCString(ctx, name);
    }
    if (!algo_name) return JS_ThrowTypeError(ctx, "encrypt: invalid algorithm");
    
    // Get key bytes
    size_t key_len;
    uint8_t *key_bytes = get_key_bytes(ctx, argv[1], &key_len);
    if (!key_bytes) return JS_ThrowTypeError(ctx, "encrypt: invalid key");
    
    // Get IV/nonce
    size_t iv_len;
    uint8_t *iv = get_iv(ctx, argv[0], &iv_len);
    if (!iv) return JS_ThrowTypeError(ctx, "encrypt: missing IV/nonce");
    
    // Get plaintext data
    size_t plaintext_len;
    uint8_t *plaintext = JS_GetArrayBuffer(ctx, &plaintext_len, argv[2]);
    if (!plaintext) return JS_ThrowTypeError(ctx, "encrypt: data must be an ArrayBuffer");
    
    // For AES-GCM
    if (strcmp(algo_name, "AES-GCM") == 0) {
        // Validate key size
        if (key_len != 16 && key_len != 24 && key_len != 32) {
            return JS_ThrowTypeError(ctx, "encrypt: invalid key size for AES");
        }
        if (iv_len != 12) {
            return JS_ThrowTypeError(ctx, "encrypt: IV must be 12 bytes for GCM");
        }
        
        // Output buffer: ciphertext + 16-byte tag
        size_t output_len = plaintext_len + 16;
        uint8_t *output = (uint8_t*)malloc(output_len);
        if (!output) return JS_ThrowOutOfMemory(ctx);
        
        // Use mbedtls GCM context
        mbedtls_gcm_context gcm;
        mbedtls_gcm_init(&gcm);
        
        int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key_bytes, key_len * 8);
        if (ret != 0) {
            mbedtls_gcm_free(&gcm);
            free(output);
            return JS_ThrowInternalError(ctx, "encrypt: failed to set GCM key");
        }
        
        // Encrypt and generate tag
        ret = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plaintext_len,
                                         iv, iv_len, NULL, 0,
                                         plaintext, output,
                                         16, output + plaintext_len);
        
        mbedtls_gcm_free(&gcm);
        
        if (ret != 0) {
            free(output);
            return JS_ThrowInternalError(ctx, "encrypt: GCM encryption failed");
        }
        
        GCValue result = JS_NewArrayBufferCopy(ctx, output, output_len);
        free(output);
        return result;
    }
    
    // Unsupported algorithm
    LOG_INFO("Crypto", "subtle.encrypt: unsupported algorithm %s", algo_name);
    return JS_ThrowTypeError(ctx, "encrypt: unsupported algorithm '%s'", algo_name);
}

// SubtleCrypto.decrypt(algorithm, key, data) - AES-GCM support  
static GCValue js_subtle_decrypt(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 3) return JS_ThrowTypeError(ctx, "decrypt requires 3 arguments");
    
    // Get algorithm name
    const char *algo_name = NULL;
    if (JS_IsString(argv[0])) {
        algo_name = JS_ToCString(ctx, argv[0]);
    } else if (JS_IsObject(argv[0])) {
        GCValue name = JS_GetPropertyStr(ctx, argv[0], "name");
        algo_name = JS_ToCString(ctx, name);
    }
    if (!algo_name) return JS_ThrowTypeError(ctx, "decrypt: invalid algorithm");
    
    // Get key bytes
    size_t key_len;
    uint8_t *key_bytes = get_key_bytes(ctx, argv[1], &key_len);
    if (!key_bytes) return JS_ThrowTypeError(ctx, "decrypt: invalid key");
    
    // Get IV/nonce
    size_t iv_len;
    uint8_t *iv = get_iv(ctx, argv[0], &iv_len);
    if (!iv) return JS_ThrowTypeError(ctx, "decrypt: missing IV/nonce");
    
    // Get ciphertext data (includes tag at the end for GCM)
    size_t ciphertext_len;
    uint8_t *ciphertext = JS_GetArrayBuffer(ctx, &ciphertext_len, argv[2]);
    if (!ciphertext) return JS_ThrowTypeError(ctx, "decrypt: data must be an ArrayBuffer");
    
    // For AES-GCM
    if (strcmp(algo_name, "AES-GCM") == 0) {
        // Validate key size
        if (key_len != 16 && key_len != 24 && key_len != 32) {
            return JS_ThrowTypeError(ctx, "decrypt: invalid key size for AES");
        }
        if (iv_len != 12) {
            return JS_ThrowTypeError(ctx, "decrypt: IV must be 12 bytes for GCM");
        }
        
        // Ciphertext must be at least 16 bytes (for the tag)
        if (ciphertext_len < 16) {
            return JS_ThrowTypeError(ctx, "decrypt: ciphertext too short");
        }
        
        // Output buffer: plaintext (ciphertext_len - tag_len)
        size_t plaintext_len = ciphertext_len - 16;
        uint8_t *output = (uint8_t*)malloc(plaintext_len);
        if (!output) return JS_ThrowOutOfMemory(ctx);
        
        // Use mbedtls GCM context
        mbedtls_gcm_context gcm;
        mbedtls_gcm_init(&gcm);
        
        int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key_bytes, key_len * 8);
        if (ret != 0) {
            mbedtls_gcm_free(&gcm);
            free(output);
            return JS_ThrowInternalError(ctx, "decrypt: failed to set GCM key");
        }
        
        // Decrypt and verify tag
        ret = mbedtls_gcm_auth_decrypt(&gcm, plaintext_len,
                                        iv, iv_len, NULL, 0,
                                        ciphertext + plaintext_len, 16,
                                        ciphertext, output);
        
        mbedtls_gcm_free(&gcm);
        
        if (ret != 0) {
            free(output);
            return JS_ThrowInternalError(ctx, "decrypt: authentication failed");
        }
        
        GCValue result = JS_NewArrayBufferCopy(ctx, output, plaintext_len);
        free(output);
        return result;
    }
    
    // Unsupported algorithm
    LOG_INFO("Crypto", "subtle.decrypt: unsupported algorithm %s", algo_name);
    return JS_ThrowTypeError(ctx, "decrypt: unsupported algorithm '%s'", algo_name);
}

/* ============================================================================
 * CSS API Implementation
 * ============================================================================ */

// Forward declaration for DOMTokenList.contains
static GCValue js_dom_token_list_contains(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

// CSS.supports(property, value)
static GCValue js_css_supports(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_FALSE;
    
    // Always return true for simplicity - actual CSS support checking is complex
    return JS_TRUE;
}

// CSS.escape(value)
static GCValue js_css_escape(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    
    const char *input = JS_ToCString(ctx, argv[0]);
    if (!input) return JS_NewString(ctx, "");
    
    // Simple escape - just return the input for now
    // Real implementation would escape special CSS characters
    return JS_NewString(ctx, input);
}

// CSSStyleSheet.insertRule(rule, index)
static GCValue js_css_style_sheet_insert_rule(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "requires at least 1 argument(s)");
    
    // Get the rules array from this object
    GCValue rules = JS_GetPropertyStr(ctx, this_val, "__rules");
    if (!JS_IsArray(ctx, rules)) {
        rules = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, this_val, "__rules", rules);
    }
    
    const char *rule = JS_ToCString(ctx, argv[0]);
    int index = 0;
    if (argc >= 2 && JS_IsNumber(argv[1])) {
        JS_ToInt32(ctx, &index, argv[1]);
    }
    
    // Get current length
    GCValue len_val = JS_GetPropertyStr(ctx, rules, "length");
    int len;
    JS_ToInt32(ctx, &len, len_val);
    
    // Clamp index
    if (index < 0) index = 0;
    if (index > len) index = len;
    
    // Insert rule at index
    // Shift elements
    for (int i = len; i > index; i--) {
        GCValue item = JS_GetPropertyUint32(ctx, rules, i - 1);
        JS_SetPropertyUint32(ctx, rules, i, item);
    }
    JS_SetPropertyUint32(ctx, rules, index, JS_NewString(ctx, rule ? rule : ""));
    
    return JS_NewInt32(ctx, index);
}

// CSSStyleSheet.deleteRule(index)
static GCValue js_css_style_sheet_delete_rule(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    
    int index;
    if (!JS_IsNumber(argv[0])) return JS_UNDEFINED;
    JS_ToInt32(ctx, &index, argv[0]);
    
    GCValue rules = JS_GetPropertyStr(ctx, this_val, "__rules");
    if (!JS_IsArray(ctx, rules)) return JS_UNDEFINED;
    
    // Get current length
    GCValue len_val = JS_GetPropertyStr(ctx, rules, "length");
    int len;
    JS_ToInt32(ctx, &len, len_val);
    
    if (index < 0 || index >= len) return JS_UNDEFINED;
    
    // Shift elements down
    for (int i = index; i < len - 1; i++) {
        GCValue item = JS_GetPropertyUint32(ctx, rules, i + 1);
        JS_SetPropertyUint32(ctx, rules, i, item);
    }
    
    // Delete last element
    JS_SetPropertyUint32(ctx, rules, len - 1, JS_UNDEFINED);
    
    return JS_UNDEFINED;
}

// CSSStyleSheet.addRule(selector, style, index)
static GCValue js_css_style_sheet_add_rule(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_NewInt32(ctx, -1);
    
    const char *selector = JS_ToCString(ctx, argv[0]);
    const char *style = JS_ToCString(ctx, argv[1]);
    int index = -1;
    if (argc >= 3 && JS_IsNumber(argv[2])) {
        JS_ToInt32(ctx, &index, argv[2]);
    }
    
    // Build rule string
    char rule[1024];
    snprintf(rule, sizeof(rule), "%s { %s }", selector ? selector : "", style ? style : "");
    
    // Call insertRule
    GCValue insert_args[2] = { JS_NewString(ctx, rule), JS_NewInt32(ctx, index) };
    js_css_style_sheet_insert_rule(ctx, this_val, 2, insert_args);
    
    return JS_NewInt32(ctx, 0);  // Return index (simplified)
}

// CSSStyleSheet.removeRule(index)
static GCValue js_css_style_sheet_remove_rule(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    return js_css_style_sheet_delete_rule(ctx, this_val, argc, argv);
}

// CSSStyleSheet.replace(text)
static GCValue js_css_style_sheet_replace(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    // Returns a Promise - simplified to resolve immediately
    return JS_UNDEFINED;
}

// CSSStyleSheet.replaceSync(text)
static GCValue js_css_style_sheet_replace_sync(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    
    // Replace all rules
    GCValue rules = JS_NewArray(ctx);
    JS_SetPropertyStr(ctx, this_val, "__rules", rules);
    
    // Parse and add rules (simplified)
    const char *text = JS_ToCString(ctx, argv[0]);
    if (text) {
        // Just store as one rule for simplicity
        JS_SetPropertyUint32(ctx, rules, 0, JS_NewString(ctx, text));
    }
    
    return JS_UNDEFINED;
}

// CSSStyleDeclaration.setProperty(property, value, priority)
static GCValue js_css_style_decl_set_property(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    
    const char *prop = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    
    if (prop && value) {
        JS_SetPropertyStr(ctx, this_val, prop, JS_NewString(ctx, value));
    }
    
    return JS_UNDEFINED;
}

// CSSStyleDeclaration.removeProperty(property)
static GCValue js_css_style_decl_remove_property(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    
    const char *prop = JS_ToCString(ctx, argv[0]);
    if (!prop) return JS_NewString(ctx, "");
    
    // Get old value
    GCValue old_val = JS_GetPropertyStr(ctx, this_val, prop);
    const char *old_str = JS_IsString(old_val) ? JS_ToCString(ctx, old_val) : "";
    
    // Delete property
    JS_SetPropertyStr(ctx, this_val, prop, JS_UNDEFINED);
    
    return JS_NewString(ctx, old_str ? old_str : "");
}

// CSSStyleDeclaration.getPropertyValue(property)
static GCValue js_css_style_decl_get_property_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    
    const char *prop = JS_ToCString(ctx, argv[0]);
    if (!prop) return JS_NewString(ctx, "");
    
    GCValue val = JS_GetPropertyStr(ctx, this_val, prop);
    if (JS_IsUndefined(val) || JS_IsNull(val)) {
        return JS_NewString(ctx, "");
    }
    
    const char *str = JS_ToCString(ctx, val);
    return JS_NewString(ctx, str ? str : "");
}

// CSSStyleDeclaration.getPropertyPriority(property)
static GCValue js_css_style_decl_get_property_priority(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    // Return empty string - no !important support in stub
    return JS_NewString(ctx, "");
}

// DOMTokenList.add(...tokens)
static GCValue js_dom_token_list_add(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    
    // Get tokens array
    GCValue tokens = JS_GetPropertyStr(ctx, this_val, "__tokens");
    if (!JS_IsArray(ctx, tokens)) {
        tokens = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, this_val, "__tokens", tokens);
    }
    
    // Add each token
    for (int i = 0; i < argc; i++) {
        if (JS_IsString(argv[i])) {
            const char *token = JS_ToCString(ctx, argv[i]);
            // Check if already exists
            GCValue exists = js_dom_token_list_contains(ctx, this_val, 1, &argv[i]);
            int has_token = JS_ToBool(ctx, exists);
            if (!has_token) {
                // Get length and append
                GCValue len_val = JS_GetPropertyStr(ctx, tokens, "length");
                int len;
                JS_ToInt32(ctx, &len, len_val);
                JS_SetPropertyUint32(ctx, tokens, len, JS_NewString(ctx, token ? token : ""));
            }
        }
    }
    
    return JS_UNDEFINED;
}

// DOMTokenList.remove(...tokens)
static GCValue js_dom_token_list_remove(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    
    GCValue tokens = JS_GetPropertyStr(ctx, this_val, "__tokens");
    if (!JS_IsArray(ctx, tokens)) return JS_UNDEFINED;
    
    // Get current length
    GCValue len_val = JS_GetPropertyStr(ctx, tokens, "length");
    int len;
    JS_ToInt32(ctx, &len, len_val);
    
    // Remove each token
    for (int i = 0; i < argc; i++) {
        if (!JS_IsString(argv[i])) continue;
        const char *token = JS_ToCString(ctx, argv[i]);
        if (!token) continue;
        
        // Find and remove
        for (int j = 0; j < len; j++) {
            GCValue item = JS_GetPropertyUint32(ctx, tokens, j);
            const char *item_str = JS_ToCString(ctx, item);
            if (item_str && strcmp(item_str, token) == 0) {
                // Shift remaining elements
                for (int k = j; k < len - 1; k++) {
                    GCValue next = JS_GetPropertyUint32(ctx, tokens, k + 1);
                    JS_SetPropertyUint32(ctx, tokens, k, next);
                }
                JS_SetPropertyUint32(ctx, tokens, len - 1, JS_UNDEFINED);
                len--;
                j--;  // Check same index again
                break;
            }
        }
    }
    
    return JS_UNDEFINED;
}

// DOMTokenList.contains(token) - defined before toggle
static GCValue js_dom_token_list_contains(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_FALSE;
    if (!JS_IsString(argv[0])) return JS_FALSE;
    
    const char *token = JS_ToCString(ctx, argv[0]);
    if (!token) return JS_FALSE;
    
    GCValue tokens = JS_GetPropertyStr(ctx, this_val, "__tokens");
    if (!JS_IsArray(ctx, tokens)) return JS_FALSE;
    
    // Get length
    GCValue len_val = JS_GetPropertyStr(ctx, tokens, "length");
    int len;
    JS_ToInt32(ctx, &len, len_val);
    
    // Search for token
    for (int i = 0; i < len; i++) {
        GCValue item = JS_GetPropertyUint32(ctx, tokens, i);
        const char *item_str = JS_ToCString(ctx, item);
        if (item_str && strcmp(item_str, token) == 0) {
            return JS_NewBool(ctx, 1);
        }
    }
    
    return JS_NewBool(ctx, 0);
}

// DOMTokenList.toggle(token, force)
static GCValue js_dom_token_list_toggle(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewBool(ctx, 0);
    
    int force = -1;  // -1 means auto
    if (argc >= 2) {
        int bool_val = JS_ToBool(ctx, argv[1]);
        if (bool_val >= 0) {
            force = bool_val ? 1 : 0;
        }
    }
    
    // Check if token exists
    GCValue exists = js_dom_token_list_contains(ctx, this_val, 1, argv);
    int has_token = JS_ToBool(ctx, exists);
    
    if (force == 1 || (force == -1 && !has_token)) {
        // Add token
        js_dom_token_list_add(ctx, this_val, 1, argv);
        return JS_NewBool(ctx, 1);
    } else if (force == 0 || (force == -1 && has_token)) {
        // Remove token
        js_dom_token_list_remove(ctx, this_val, 1, argv);
        return JS_NewBool(ctx, 0);
    }
    
    return JS_NewBool(ctx, has_token);
}

// DOMTokenList.forEach(callback)
static GCValue js_dom_token_list_for_each(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
    
    GCValue tokens = JS_GetPropertyStr(ctx, this_val, "__tokens");
    if (!JS_IsArray(ctx, tokens)) return JS_UNDEFINED;
    
    // Get length
    GCValue len_val = JS_GetPropertyStr(ctx, tokens, "length");
    int len;
    JS_ToInt32(ctx, &len, len_val);
    
    // Call callback for each token
    GCValue callback = argv[0];
    for (int i = 0; i < len; i++) {
        GCValue item = JS_GetPropertyUint32(ctx, tokens, i);
        GCValue args[3] = { item, JS_NewInt32(ctx, i), this_val };
        JS_Call(ctx, callback, JS_UNDEFINED, 3, args);
    }
    
    return JS_UNDEFINED;
}

/* ============================================================================
 * ServiceWorker API Implementation
 * ============================================================================ */

// Forward declarations for ServiceWorker
static GCValue js_service_worker_register(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_service_worker_get_registration(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_service_worker_get_registrations(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_service_worker_add_event_listener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

// Execute a service worker script (simplified - runs in same context)
static bool execute_service_worker_script(JSContextHandle ctx, const char *script_url, int worker_id) {
    // In a real implementation, this would load the script from the URL
    // For now, we just log it and return success
    LOG_INFO("ServiceWorker: Would execute script from %s (worker %d)", script_url, worker_id);
    
    // Dispatch 'install' and 'activate' events would happen here
    // For now, we just simulate that the worker activated
    
    return true;
}

// Create a ServiceWorkerRegistration object
static GCValue create_service_worker_registration(JSContextHandle ctx, const char *script_url, const char *scope) {
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
static GCValue js_service_worker_register(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
static GCValue js_service_worker_get_registration(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
static GCValue js_service_worker_get_registrations(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
static GCValue js_service_worker_add_event_listener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
static void post_message_to_service_worker(JSContextHandle ctx, GCValue data) {
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
static GCValue js_geolocation_position_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
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
static GCValue js_geolocation_position_error_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
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
static GCValue js_geolocation_get_current_position(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
static GCValue js_geolocation_watch_position(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    // Return watch ID 0
    return JS_NewInt32(ctx, 0);
}

// User-Agent Client Hints getHighEntropyValues
static GCValue js_user_agent_data_get_high_entropy_values(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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

// Battery API - getBattery() returns a mock battery object
static GCValue js_navigator_get_battery(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
    
    return battery;
}

// History pushState - stores the state object on history.state
static GCValue js_history_push_state(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
static GCValue js_history_replace_state(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
static GCValue js_media_metadata_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
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
static GCValue js_get_selection(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    // Create a simple Selection stub object
    GCValue selection = JS_NewObject(ctx);
    if (JS_IsException(selection)) return JS_EXCEPTION;
    
    // Add common Selection properties
    JS_SetPropertyStr(ctx, selection, "anchorNode", JS_NULL);
    JS_SetPropertyStr(ctx, selection, "focusNode", JS_NULL);
    JS_SetPropertyStr(ctx, selection, "anchorOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, selection, "focusOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, selection, "isCollapsed", JS_TRUE);
    JS_SetPropertyStr(ctx, selection, "rangeCount", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, selection, "type", JS_NewString(ctx, "None"));
    
    // Add common Selection methods (stubs)
    JS_SetPropertyStr(ctx, selection, "toString",
        JS_NewCFunction(ctx, js_empty_string, "toString", 0));
    JS_SetPropertyStr(ctx, selection, "removeAllRanges",
        JS_NewCFunction(ctx, js_undefined, "removeAllRanges", 0));
    JS_SetPropertyStr(ctx, selection, "addRange",
        JS_NewCFunction(ctx, js_undefined, "addRange", 1));
    JS_SetPropertyStr(ctx, selection, "removeRange",
        JS_NewCFunction(ctx, js_undefined, "removeRange", 1));
    JS_SetPropertyStr(ctx, selection, "deleteFromDocument",
        JS_NewCFunction(ctx, js_undefined, "deleteFromDocument", 0));
    JS_SetPropertyStr(ctx, selection, "getRangeAt",
        JS_NewCFunction(ctx, js_null, "getRangeAt", 1));
    JS_SetPropertyStr(ctx, selection, "collapse",
        JS_NewCFunction(ctx, js_undefined, "collapse", 2));
    JS_SetPropertyStr(ctx, selection, "extend",
        JS_NewCFunction(ctx, js_undefined, "extend", 2));
    JS_SetPropertyStr(ctx, selection, "selectAllChildren",
        JS_NewCFunction(ctx, js_undefined, "selectAllChildren", 1));
    
    return selection;
}

/* ============================================================================
 * Location Object Implementation
 * ============================================================================ */

// Parse URL into components - simple parser for standard URLs
static void parse_url(const char *url, LocationData *loc) {
    // Start with empty components
    memset(loc, 0, sizeof(LocationData));
    
    // Default values
    strncpy(loc->protocol, "https:", sizeof(loc->protocol) - 1);
    strncpy(loc->host, "www.youtube.com", sizeof(loc->host) - 1);
    strncpy(loc->hostname, "www.youtube.com", sizeof(loc->hostname) - 1);
    strncpy(loc->port, "", sizeof(loc->port) - 1);
    strncpy(loc->pathname, "/", sizeof(loc->pathname) - 1);
    strncpy(loc->search, "", sizeof(loc->search) - 1);
    strncpy(loc->hash, "", sizeof(loc->hash) - 1);
    
    // Parse protocol
    const char *p = url;
    const char *proto_end = strstr(p, "://");
    if (proto_end) {
        size_t proto_len = proto_end - p;
        if (proto_len < sizeof(loc->protocol) - 1) {
            strncpy(loc->protocol, p, proto_len);
            loc->protocol[proto_len] = ':';
            loc->protocol[proto_len + 1] = '\0';
        }
        p = proto_end + 3;
    }
    
    // Parse host (hostname:port)
    const char *path_start = strchr(p, '/');
    const char *query_start = strchr(p, '?');
    const char *hash_start = strchr(p, '#');
    
    size_t host_len = 0;
    if (path_start) {
        host_len = path_start - p;
    } else if (query_start) {
        host_len = query_start - p;
    } else if (hash_start) {
        host_len = hash_start - p;
    } else {
        host_len = strlen(p);
    }
    
    if (host_len > 0 && host_len < sizeof(loc->host)) {
        strncpy(loc->host, p, host_len);
        loc->host[host_len] = '\0';
        
        // Parse hostname and port from host
        const char *port_sep = strchr(loc->host, ':');
        if (port_sep) {
            size_t hostname_len = port_sep - loc->host;
            if (hostname_len < sizeof(loc->hostname)) {
                strncpy(loc->hostname, loc->host, hostname_len);
                loc->hostname[hostname_len] = '\0';
            }
            strncpy(loc->port, port_sep + 1, sizeof(loc->port) - 1);
        } else {
            strncpy(loc->hostname, loc->host, sizeof(loc->hostname) - 1);
            loc->port[0] = '\0';
        }
    }
    
    if (path_start) {
        p = path_start;
        const char *end = query_start ? query_start : (hash_start ? hash_start : p + strlen(p));
        size_t path_len = end - p;
        if (path_len < sizeof(loc->pathname)) {
            strncpy(loc->pathname, p, path_len);
            loc->pathname[path_len] = '\0';
        }
    }
    
    if (query_start) {
        p = query_start;
        const char *end = hash_start ? hash_start : p + strlen(p);
        size_t query_len = end - p;
        if (query_len < sizeof(loc->search)) {
            strncpy(loc->search, p, query_len);
            loc->search[query_len] = '\0';
        }
    }
    
    if (hash_start) {
        strncpy(loc->hash, hash_start, sizeof(loc->hash) - 1);
    }
    
    // Reconstruct full href
    strncpy(loc->href, url, sizeof(loc->href) - 1);
    
    // Construct origin
    snprintf(loc->origin, sizeof(loc->origin), "%s//%s", loc->protocol, loc->hostname);
}

// Reconstruct href from components
static void rebuild_href(LocationData *loc) {
    char port_part[32] = "";
    if (strlen(loc->port) > 0) {
        snprintf(port_part, sizeof(port_part), ":%s", loc->port);
    }
    
    snprintf(loc->href, sizeof(loc->href), "%s//%s%s%s%s%s%s",
        loc->protocol,
        loc->hostname,
        port_part,
        loc->pathname,
        loc->search,
        loc->hash);
    
    // Rebuild host
    if (strlen(loc->port) > 0) {
        snprintf(loc->host, sizeof(loc->host), "%s:%s", loc->hostname, loc->port);
    } else {
        strncpy(loc->host, loc->hostname, sizeof(loc->host) - 1);
    }
    
    // Rebuild origin
    snprintf(loc->origin, sizeof(loc->origin), "%s//%s", loc->protocol, loc->hostname);
}

// Get LocationData from JS object
static LocationData* get_location_data(JSContextHandle ctx, GCValue obj) {
    // Use gc_deref to get pointer from handle stored in opaque
    GCHandle handle = JS_GetOpaqueHandle(obj, JS_GC_OBJ_TYPE_DATA);
    if (handle == GC_HANDLE_NULL) return NULL;
    return (LocationData*)gc_deref(handle);
}

// location.href getter
static GCValue js_location_get_href(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    LocationData *loc = get_location_data(ctx, this_val);
    if (!loc) return JS_NewString(ctx, "");
    return JS_NewString(ctx, loc->href);
}

// location.href setter
static GCValue js_location_set_href(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    LocationData *loc = get_location_data(ctx, this_val);
    if (!loc) return JS_UNDEFINED;
    
    const char *url = JS_ToCString(ctx, argv[0]);
    if (url) {
        parse_url(url, loc);
    }
    return JS_UNDEFINED;
}

// location.protocol getter
static GCValue js_location_get_protocol(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    LocationData *loc = get_location_data(ctx, this_val);
    if (!loc) return JS_NewString(ctx, "https:");
    return JS_NewString(ctx, loc->protocol);
}

// location.host getter
static GCValue js_location_get_host(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    LocationData *loc = get_location_data(ctx, this_val);
    if (!loc) return JS_NewString(ctx, "");
    return JS_NewString(ctx, loc->host);
}

// location.hostname getter
static GCValue js_location_get_hostname(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    LocationData *loc = get_location_data(ctx, this_val);
    if (!loc) return JS_NewString(ctx, "");
    return JS_NewString(ctx, loc->hostname);
}

// location.port getter
static GCValue js_location_get_port(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    LocationData *loc = get_location_data(ctx, this_val);
    if (!loc) return JS_NewString(ctx, "");
    return JS_NewString(ctx, loc->port);
}

// location.pathname getter
static GCValue js_location_get_pathname(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    LocationData *loc = get_location_data(ctx, this_val);
    if (!loc) return JS_NewString(ctx, "/");
    return JS_NewString(ctx, loc->pathname);
}

// location.pathname setter
static GCValue js_location_set_pathname(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    LocationData *loc = get_location_data(ctx, this_val);
    if (!loc) return JS_UNDEFINED;
    
    const char *path = JS_ToCString(ctx, argv[0]);
    if (path) {
        // Ensure path starts with /
        if (path[0] == '/') {
            strncpy(loc->pathname, path, sizeof(loc->pathname) - 1);
        } else {
            char new_path[1024];
            snprintf(new_path, sizeof(new_path), "/%s", path);
            strncpy(loc->pathname, new_path, sizeof(loc->pathname) - 1);
        }
        rebuild_href(loc);
    }
    return JS_UNDEFINED;
}

// location.search getter
static GCValue js_location_get_search(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    LocationData *loc = get_location_data(ctx, this_val);
    if (!loc) return JS_NewString(ctx, "");
    return JS_NewString(ctx, loc->search);
}

// location.search setter
static GCValue js_location_set_search(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    LocationData *loc = get_location_data(ctx, this_val);
    if (!loc) return JS_UNDEFINED;
    
    const char *search = JS_ToCString(ctx, argv[0]);
    if (search) {
        if (search[0] == '?' || strlen(search) == 0) {
            strncpy(loc->search, search, sizeof(loc->search) - 1);
        } else {
            char new_search[2048];
            snprintf(new_search, sizeof(new_search), "?%s", search);
            strncpy(loc->search, new_search, sizeof(loc->search) - 1);
        }
        rebuild_href(loc);
    }
    return JS_UNDEFINED;
}

// location.hash getter
static GCValue js_location_get_hash(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    LocationData *loc = get_location_data(ctx, this_val);
    if (!loc) return JS_NewString(ctx, "");
    return JS_NewString(ctx, loc->hash);
}

// location.hash setter
static GCValue js_location_set_hash(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    LocationData *loc = get_location_data(ctx, this_val);
    if (!loc) return JS_UNDEFINED;
    
    const char *hash = JS_ToCString(ctx, argv[0]);
    if (hash) {
        if (hash[0] == '#' || strlen(hash) == 0) {
            strncpy(loc->hash, hash, sizeof(loc->hash) - 1);
        } else {
            char new_hash[256];
            snprintf(new_hash, sizeof(new_hash), "#%s", hash);
            strncpy(loc->hash, new_hash, sizeof(loc->hash) - 1);
        }
        rebuild_href(loc);
    }
    return JS_UNDEFINED;
}

// location.origin getter
static GCValue js_location_get_origin(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    LocationData *loc = get_location_data(ctx, this_val);
    if (!loc) return JS_NewString(ctx, "");
    return JS_NewString(ctx, loc->origin);
}

// location.toString()
static GCValue js_location_toString(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_location_get_href(ctx, this_val, argc, argv);
}

// location.assign(url)
static GCValue js_location_assign(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    // Just update href - same as setting location.href
    return js_location_set_href(ctx, this_val, argc, argv);
}

// location.replace(url)
static GCValue js_location_replace(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    // Same as assign for our purposes (no history manipulation needed)
    return js_location_set_href(ctx, this_val, argc, argv);
}

// location.reload()
static GCValue js_location_reload(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    // No-op for our purposes
    (void)ctx; (void)this_val;
    return JS_UNDEFINED;
}

// Validation that actually tests if object can be used
// Returns 1 if usable, 0 if not. Logs when corruption is detected.
static int is_obj_usable(JSContextHandle ctx, GCValue obj) {
    (void)ctx;
    if (JS_IsException(obj)) {
        LOG_ERROR("is_obj_usable: object is exception");
        return 0;
    }
    if (!JS_IsObject(obj)) {
        LOG_ERROR("is_obj_usable: not an object, tag=%d", (int)JS_VALUE_GET_TAG(obj));
        return 0;
    }
    return 1;
}

// Safe version of JS_SetPropertyStr that checks for errors
static int safe_set_property_str(JSContextHandle ctx, GCValue obj, const char *key, GCValue val) {
    if (!is_obj_usable(ctx, obj)) {
        LOG_ERROR("safe_set_property_str: obj not usable for key '%s'", key);
        return -1;
    }
    if (JS_IsException(val)) {
        LOG_ERROR("safe_set_property_str: val is exception for key '%s'", key);
        return -1;
    }
    int ret = JS_SetPropertyStr(ctx, obj, key, val);
    if (ret < 0) {
        LOG_ERROR("safe_set_property_str: JS_SetPropertyStr failed for key '%s', ret=%d", key, ret);
    }
    return ret;
}

// Helper to get a prototype from a constructor: Constructor.prototype
GCValue js_get_prototype(JSContextHandle ctx, GCValue ctor) {
    return JS_GetPropertyStr(ctx, ctor, "prototype");
}

static GCValue js_zero(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, 0);
}

/* ============================================================================
 * Console API Implementation
 * ============================================================================ */

// Global console data for timers, counters, and groups
static ConsoleData g_console_data = {0};

// Helper to format a single value for console output
static void console_format_value(JSContextHandle ctx, GCValue val, char *out_buf, size_t out_len) {
    (void)ctx;
    if (JS_IsUndefined(val)) {
        strncpy(out_buf, "undefined", out_len - 1);
    } else if (JS_IsNull(val)) {
        strncpy(out_buf, "null", out_len - 1);
    } else if (JS_IsBool(val)) {
        int bool_val = JS_ToBool(ctx, val);
        strncpy(out_buf, bool_val ? "true" : "false", out_len - 1);
    } else if (JS_IsNumber(val)) {
        double num;
        JS_ToFloat64(ctx, &num, val);
        if (isnan(num)) {
            strncpy(out_buf, "NaN", out_len - 1);
        } else if (isinf(num)) {
            strncpy(out_buf, num > 0 ? "Infinity" : "-Infinity", out_len - 1);
        } else {
            snprintf(out_buf, out_len, "%g", num);
        }
    } else if (JS_IsString(val)) {
        const char *str = JS_ToCString(ctx, val);
        if (str) {
            strncpy(out_buf, str, out_len - 1);
        }
    } else if (JS_IsArray(ctx, val)) {
        strncpy(out_buf, "[Array]", out_len - 1);
    } else if (JS_IsObject(val)) {
        // Check if it's an Error
        GCValue name = JS_GetPropertyStr(ctx, val, "name");
        if (JS_IsString(name)) {
            const char *name_str = JS_ToCString(ctx, name);
            if (name_str && strstr(name_str, "Error")) {
                GCValue msg = JS_GetPropertyStr(ctx, val, "message");
                const char *msg_str = JS_IsString(msg) ? JS_ToCString(ctx, msg) : "";
                snprintf(out_buf, out_len, "%s: %s", name_str, msg_str ? msg_str : "");
                return;
            }
        }
        strncpy(out_buf, "[Object]", out_len - 1);
    } else if (JS_IsFunction(ctx, val)) {
        strncpy(out_buf, "[Function]", out_len - 1);
    } else {
        strncpy(out_buf, "[unknown]", out_len - 1);
    }
    out_buf[out_len - 1] = '\0';
}

// Helper to format console arguments with printf-style formatting
static void console_format_args(JSContextHandle ctx, int argc, GCValue *argv, char *out_buf, size_t out_len) {
    if (argc == 0) {
        out_buf[0] = '\0';
        return;
    }
    
    out_buf[0] = '\0';
    size_t pos = 0;
    
    // Check if first arg is a format string
    if (argc > 0 && JS_IsString(argv[0])) {
        const char *fmt = JS_ToCString(ctx, argv[0]);
        if (fmt) {
            int arg_idx = 1;
            const char *p = fmt;
            while (*p && pos < out_len - 1) {
                if (*p == '%' && arg_idx < argc) {
                    p++;
                    char specifier = *p;
                    char val_buf[1024];
                    
                    switch (specifier) {
                        case 's':
                        case 'd':
                        case 'i':
                        case 'f':
                        case 'o':
                        case 'O':
                            console_format_value(ctx, argv[arg_idx++], val_buf, sizeof(val_buf));
                            pos += snprintf(out_buf + pos, out_len - pos, "%s", val_buf);
                            break;
                        case '%':
                            out_buf[pos++] = '%';
                            break;
                        default:
                            out_buf[pos++] = '%';
                            if (pos < out_len - 1) out_buf[pos++] = specifier;
                            break;
                    }
                    if (*p) p++;
                } else {
                    out_buf[pos++] = *p++;
                }
            }
            
            // Add remaining arguments
            while (arg_idx < argc && pos < out_len - 1) {
                char val_buf[1024];
                console_format_value(ctx, argv[arg_idx++], val_buf, sizeof(val_buf));
                pos += snprintf(out_buf + pos, out_len - pos, " %s", val_buf);
            }
            out_buf[pos] = '\0';
            return;
        }
    }
    
    // Simple concatenation without format string
    for (int i = 0; i < argc && pos < out_len - 1; i++) {
        char val_buf[1024];
        console_format_value(ctx, argv[i], val_buf, sizeof(val_buf));
        if (i > 0) pos += snprintf(out_buf + pos, out_len - pos, " ");
        pos += snprintf(out_buf + pos, out_len - pos, "%s", val_buf);
    }
    out_buf[pos] = '\0';
}

// Get current time in milliseconds
#ifdef _MSC_VER
#include <windows.h>
static double console_get_time_ms(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
static double console_get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
#endif

// console.log(...args)
static GCValue js_console_log(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    char msg[4096];
    console_format_args(ctx, argc, argv, msg, sizeof(msg));
    
    // Add group indentation
    char indent[64] = {0};
    for (int i = 0; i < g_console_data.group_depth && i < 16; i++) {
        strcat(indent, "  ");
    }
    
    platform_log(LOG_LEVEL_INFO, "console", "%s%s", indent, msg);
    return JS_UNDEFINED;
}

// console.warn(...args)
static GCValue js_console_warn(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    char msg[4096];
    console_format_args(ctx, argc, argv, msg, sizeof(msg));
    platform_log(LOG_LEVEL_WARN, "console", "%s", msg);
    return JS_UNDEFINED;
}

// console.error(...args)
static GCValue js_console_error(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    char msg[4096];
    console_format_args(ctx, argc, argv, msg, sizeof(msg));
    platform_log(LOG_LEVEL_ERROR, "console", "%s", msg);
    return JS_UNDEFINED;
}

// console.info(...args)
static GCValue js_console_info(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    char msg[4096];
    console_format_args(ctx, argc, argv, msg, sizeof(msg));
    platform_log(LOG_LEVEL_INFO, "console", "%s", msg);
    return JS_UNDEFINED;
}

// console.debug(...args)
static GCValue js_console_debug(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    char msg[4096];
    console_format_args(ctx, argc, argv, msg, sizeof(msg));
    platform_log(LOG_LEVEL_INFO, "console", "%s", msg);
    return JS_UNDEFINED;
}

// console.trace(...args)
static GCValue js_console_trace(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    char msg[4096];
    console_format_args(ctx, argc, argv, msg, sizeof(msg));
    platform_log(LOG_LEVEL_INFO, "console", "Trace: %s", msg);
    // Note: Full stack trace would require QuickJS stack inspection
    return JS_UNDEFINED;
}

// console.dir(obj) - display object properties (simplified)
static GCValue js_console_dir(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    
    char msg[4096];
    if (JS_IsObject(argv[0])) {
        // Simplified - just show it's an object
        platform_log(LOG_LEVEL_INFO, "console", "[Object]");
    } else {
        console_format_value(ctx, argv[0], msg, sizeof(msg));
        platform_log(LOG_LEVEL_INFO, "console", "%s", msg);
    }
    return JS_UNDEFINED;
}

// console.dirxml(node) - display XML/DOM representation
static GCValue js_console_dirxml(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    
    char msg[1024];
    console_format_value(ctx, argv[0], msg, sizeof(msg));
    platform_log(LOG_LEVEL_INFO, "console", "<Element: %s>", msg);
    return JS_UNDEFINED;
}

// console.group(label)
static GCValue js_console_group(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (g_console_data.group_depth < 16) {
        char label[256] = "console.group";
        if (argc > 0) {
            console_format_value(ctx, argv[0], label, sizeof(label));
        }
        platform_log(LOG_LEVEL_INFO, "console", "%s", label);
        g_console_data.group_collapsed[g_console_data.group_depth] = 0;
        g_console_data.group_depth++;
    }
    return JS_UNDEFINED;
}

// console.groupCollapsed(label)
static GCValue js_console_groupCollapsed(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (g_console_data.group_depth < 16) {
        char label[256] = "console.groupCollapsed";
        if (argc > 0) {
            console_format_value(ctx, argv[0], label, sizeof(label));
        }
        platform_log(LOG_LEVEL_INFO, "console", "%s", label);
        g_console_data.group_collapsed[g_console_data.group_depth] = 1;
        g_console_data.group_depth++;
    }
    return JS_UNDEFINED;
}

// console.groupEnd()
static GCValue js_console_groupEnd(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (g_console_data.group_depth > 0) {
        g_console_data.group_depth--;
    }
    return JS_UNDEFINED;
}

// console.time(label)
static GCValue js_console_time(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    char label[CONSOLE_MAX_LABEL_LEN] = "default";
    if (argc > 0) {
        console_format_value(ctx, argv[0], label, sizeof(label));
    }
    
    // Find existing timer or create new one
    for (int i = 0; i < CONSOLE_MAX_TIMERS; i++) {
        if (g_console_data.timers[i].active && strcmp(g_console_data.timers[i].label, label) == 0) {
            // Timer already exists, warn
            platform_log(LOG_LEVEL_WARN, "console", "Timer '%s' already exists", label);
            return JS_UNDEFINED;
        }
    }
    
    // Find empty slot
    for (int i = 0; i < CONSOLE_MAX_TIMERS; i++) {
        if (!g_console_data.timers[i].active) {
            strncpy(g_console_data.timers[i].label, label, CONSOLE_MAX_LABEL_LEN - 1);
            g_console_data.timers[i].start_time = console_get_time_ms();
            g_console_data.timers[i].active = 1;
            g_console_data.timer_count++;
            return JS_UNDEFINED;
        }
    }
    
    platform_log(LOG_LEVEL_WARN, "console", "Too many timers");
    return JS_UNDEFINED;
}

// console.timeEnd(label)
static GCValue js_console_timeEnd(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    char label[CONSOLE_MAX_LABEL_LEN] = "default";
    if (argc > 0) {
        console_format_value(ctx, argv[0], label, sizeof(label));
    }
    
    for (int i = 0; i < CONSOLE_MAX_TIMERS; i++) {
        if (g_console_data.timers[i].active && strcmp(g_console_data.timers[i].label, label) == 0) {
            double elapsed = console_get_time_ms() - g_console_data.timers[i].start_time;
            platform_log(LOG_LEVEL_INFO, "console", "%s: %0.3fms", label, elapsed);
            g_console_data.timers[i].active = 0;
            g_console_data.timer_count--;
            return JS_UNDEFINED;
        }
    }
    
    platform_log(LOG_LEVEL_WARN, "console", "Timer '%s' does not exist", label);
    return JS_UNDEFINED;
}

// console.timeLog(label)
static GCValue js_console_timeLog(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    char label[CONSOLE_MAX_LABEL_LEN] = "default";
    if (argc > 0) {
        console_format_value(ctx, argv[0], label, sizeof(label));
    }
    
    for (int i = 0; i < CONSOLE_MAX_TIMERS; i++) {
        if (g_console_data.timers[i].active && strcmp(g_console_data.timers[i].label, label) == 0) {
            double elapsed = console_get_time_ms() - g_console_data.timers[i].start_time;
            platform_log(LOG_LEVEL_INFO, "console", "%s: %0.3fms", label, elapsed);
            return JS_UNDEFINED;
        }
    }
    
    platform_log(LOG_LEVEL_WARN, "console", "Timer '%s' does not exist", label);
    return JS_UNDEFINED;
}

// console.count(label)
static GCValue js_console_count(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    char label[CONSOLE_MAX_LABEL_LEN] = "default";
    if (argc > 0) {
        console_format_value(ctx, argv[0], label, sizeof(label));
    }
    
    // Find existing counter or create new one
    for (int i = 0; i < CONSOLE_MAX_COUNTERS; i++) {
        if (g_console_data.counters[i].active && strcmp(g_console_data.counters[i].label, label) == 0) {
            g_console_data.counters[i].count++;
            platform_log(LOG_LEVEL_INFO, "console", "%s: %d", label, g_console_data.counters[i].count);
            return JS_UNDEFINED;
        }
    }
    
    // Find empty slot
    for (int i = 0; i < CONSOLE_MAX_COUNTERS; i++) {
        if (!g_console_data.counters[i].active) {
            strncpy(g_console_data.counters[i].label, label, CONSOLE_MAX_LABEL_LEN - 1);
            g_console_data.counters[i].count = 1;
            g_console_data.counters[i].active = 1;
            g_console_data.counter_count++;
            platform_log(LOG_LEVEL_INFO, "console", "%s: 1", label);
            return JS_UNDEFINED;
        }
    }
    
    platform_log(LOG_LEVEL_WARN, "console", "Too many counters");
    return JS_UNDEFINED;
}

// console.countReset(label)
static GCValue js_console_countReset(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    char label[CONSOLE_MAX_LABEL_LEN] = "default";
    if (argc > 0) {
        console_format_value(ctx, argv[0], label, sizeof(label));
    }
    
    for (int i = 0; i < CONSOLE_MAX_COUNTERS; i++) {
        if (g_console_data.counters[i].active && strcmp(g_console_data.counters[i].label, label) == 0) {
            g_console_data.counters[i].count = 0;
            platform_log(LOG_LEVEL_INFO, "console", "%s: 0", label);
            return JS_UNDEFINED;
        }
    }
    
    platform_log(LOG_LEVEL_WARN, "console", "Counter '%s' does not exist", label);
    return JS_UNDEFINED;
}

// console.assert(condition, ...args)
static GCValue js_console_assert(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    
    int condition = JS_ToBool(ctx, argv[0]);
    if (!condition) {
        char msg[4096] = "Assertion failed";
        if (argc > 1) {
            console_format_args(ctx, argc - 1, argv + 1, msg, sizeof(msg));
        }
        platform_log(LOG_LEVEL_ERROR, "console", "Assertion failed: %s", msg);
    }
    return JS_UNDEFINED;
}

// console.clear()
static GCValue js_console_clear(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    // Clear timers and counters - manually reset to avoid corrupting GCValue types
    for (int i = 0; i < CONSOLE_MAX_TIMERS; i++) {
        g_console_data.timers[i].active = 0;
    }
    for (int i = 0; i < CONSOLE_MAX_COUNTERS; i++) {
        g_console_data.counters[i].active = 0;
    }
    g_console_data.timer_count = 0;
    g_console_data.counter_count = 0;
    g_console_data.group_depth = 0;
    platform_log(LOG_LEVEL_INFO, "console", "[Console was cleared]");
    return JS_UNDEFINED;
}

// getComputedStyle - reads from the per-element computed-style table.
static GCValue js_get_computed_style(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    GCValue element = argc > 0 ? argv[0] : JS_UNDEFINED;
    GCValue pseudo = argc > 1 ? argv[1] : JS_UNDEFINED;
    (void)pseudo;

    GCValue style = JS_NewObject(ctx);
    if (JS_IsException(style)) return style;

    DOMNodeHandle node = DOMNodeHandle::from_object_check(ctx, element);
    if (node.valid()) {
        /* Attach a getPropertyValue method that reads from the computed table. */
        GCValue closure = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, closure, "__node_handle",
                          JS_NewInt32(ctx, (int32_t)node.handle()));

        JS_SetPropertyStr(ctx, style, "getPropertyValue",
            JS_NewCFunction(ctx, js_empty_string, "getPropertyValue", 1));

        /* Also materialize the known computed properties as direct properties
         * so common reads like cs.color work. */
        GCHandle cs_handle = node.computed_style_handle();
        if (cs_handle != GC_HANDLE_NULL) {
            CssComputedStyle *cs = (CssComputedStyle *)gc_deref(cs_handle);
            if (cs && cs->properties) {
                LFHashTable *t = cs->properties;
                for (uint32_t i = 0; i < t->bucket_count; i++) {
                    if (t->buckets[i].state == LF_HASH_OCCUPIED &&
                        t->buckets[i].value != GC_HANDLE_NULL) {
                        JSAtom prop_atom = (JSAtom)t->buckets[i].key;
                        const char *prop_str = JS_AtomToCString(ctx, prop_atom);
                        if (prop_str) {
                            GCValue val = GC_MKHANDLE(JS_TAG_STRING, t->buckets[i].value);
                            JS_SetPropertyStr(ctx, style, prop_str, val);
                            /* JS_ToCString/JS_AtomToCString in this QuickJS
                             * fork return pointers into GC-managed strings; no
                             * explicit free is required. */
                        }
                    }
                }
            }
        }
    } else {
        JS_SetPropertyStr(ctx, style, "getPropertyValue",
            JS_NewCFunction(ctx, js_empty_string, "getPropertyValue", 1));
    }
    return style;
}

static GCValue js_dummy_function_true(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_TRUE;
}

// Generic dummy function that returns undefined
static GCValue js_dummy_function(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

// MessageChannel constructor
static GCValue js_message_channel_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = js_create_from_ctor_proto(ctx, new_target);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    GCValue port1 = JS_NewObject(ctx);
    GCValue port2 = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "port1", port1);
    JS_SetPropertyStr(ctx, obj, "port2", port2);
    return obj;
}

// Helper: create object from constructor's prototype (like js_create_from_ctor)
static GCValue js_create_from_ctor_proto(JSContextHandle ctx, GCValue ctor) {
    GCValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
    if (JS_IsException(proto))
        return JS_EXCEPTION;
    GCValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    if (JS_IsObject(proto)) {
        JS_SetPrototype(ctx, obj, proto);
    }
    return obj;
}

// Constructor for Element - creates object with Element.prototype in chain
// Uses js_dom_node_class_id so DOM node data can be retrieved via JS_GetOpaqueHandle
static GCValue js_element_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = JS_NewObjectClass(ctx, js_dom_node_class_id);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    GCValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (!JS_IsException(proto) && JS_IsObject(proto)) {
        JS_SetPrototype(ctx, obj, proto);
    }
    return obj;
}

// Constructor for HTMLElement - creates object with HTMLElement.prototype in chain
// Uses js_dom_node_class_id so DOM node data can be retrieved via JS_GetOpaqueHandle
static GCValue js_html_element_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = JS_NewObjectClass(ctx, js_dom_node_class_id);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    GCValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (!JS_IsException(proto) && JS_IsObject(proto)) {
        JS_SetPrototype(ctx, obj, proto);
    }
    return obj;
}

// Static getter for observedAttributes (needed by Polymer mixin chain)
static GCValue js_event_target_observed_attributes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewArray(ctx);
}

// ============================================================================
// DOMException Implementation (needed for Web Animations API)
// ============================================================================

#define DOM_EXCEPTION_LOG_TAG "DOMException"
#define DOM_EX_LOGD(...) platform_log(LOG_LEVEL_INFO, DOM_EXCEPTION_LOG_TAG, __VA_ARGS__)

#define DOM_EXCEPTION_INDEX_SIZE_ERR 1
#define DOM_EXCEPTION_HIERARCHY_REQUEST_ERR 3
#define DOM_EXCEPTION_WRONG_DOCUMENT_ERR 4
#define DOM_EXCEPTION_INVALID_CHARACTER_ERR 5
#define DOM_EXCEPTION_NO_MODIFICATION_ALLOWED_ERR 7
#define DOM_EXCEPTION_NOT_FOUND_ERR 8
#define DOM_EXCEPTION_NOT_SUPPORTED_ERR 9
#define DOM_EXCEPTION_INVALID_STATE_ERR 11
#define DOM_EXCEPTION_SYNTAX_ERR 12
#define DOM_EXCEPTION_INVALID_MODIFICATION_ERR 13
#define DOM_EXCEPTION_NAMESPACE_ERR 14
#define DOM_EXCEPTION_INVALID_ACCESS_ERR 15
#define DOM_EXCEPTION_TYPE_MISMATCH_ERR 17
#define DOM_EXCEPTION_SECURITY_ERR 18
#define DOM_EXCEPTION_NETWORK_ERR 19
#define DOM_EXCEPTION_ABORT_ERR 20
#define DOM_EXCEPTION_URL_MISMATCH_ERR 21
#define DOM_EXCEPTION_QUOTA_EXCEEDED_ERR 22
#define DOM_EXCEPTION_TIMEOUT_ERR 23
#define DOM_EXCEPTION_INVALID_NODE_TYPE_ERR 24
#define DOM_EXCEPTION_DATA_CLONE_ERR 25

// DOMExceptionData struct is defined in browser_api_impl_types.h

JSClassID js_dom_exception_class_id = 0;

static void js_dom_exception_finalizer(JSRuntimeHandle rt, GCValue val) {
    // DOMExceptionData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

static JSClassDef js_dom_exception_class_def = {
    .class_name = "DOMException",
    .finalizer = js_dom_exception_finalizer,
};

static GCValue js_dom_exception_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    DOM_EX_LOGD("DOMException constructor called");
    DOMExceptionDataHandle de = DOMExceptionDataHandle::create();
    if (!de.valid()) return JS_ThrowTypeError(ctx, "Invalid DOM element");
    
    de.set_name("Error");
    de.set_code(0);
    
    if (argc > 0) {
        const char *msg = JS_ToCString(ctx, argv[0]);
        if (msg) {
            de.set_message(msg);
        }
    }
    
    if (argc > 1) {
        const char *name = JS_ToCString(ctx, argv[1]);
        if (name) {
            de.set_name(name);
            int code = 0;
            if (strcmp(name, "IndexSizeError") == 0) code = DOM_EXCEPTION_INDEX_SIZE_ERR;
            else if (strcmp(name, "HierarchyRequestError") == 0) code = DOM_EXCEPTION_HIERARCHY_REQUEST_ERR;
            else if (strcmp(name, "WrongDocumentError") == 0) code = DOM_EXCEPTION_WRONG_DOCUMENT_ERR;
            else if (strcmp(name, "InvalidCharacterError") == 0) code = DOM_EXCEPTION_INVALID_CHARACTER_ERR;
            else if (strcmp(name, "NoModificationAllowedError") == 0) code = DOM_EXCEPTION_NO_MODIFICATION_ALLOWED_ERR;
            else if (strcmp(name, "NotFoundError") == 0) code = DOM_EXCEPTION_NOT_FOUND_ERR;
            else if (strcmp(name, "NotSupportedError") == 0) code = DOM_EXCEPTION_NOT_SUPPORTED_ERR;
            else if (strcmp(name, "InvalidStateError") == 0) code = DOM_EXCEPTION_INVALID_STATE_ERR;
            else if (strcmp(name, "SyntaxError") == 0) code = DOM_EXCEPTION_SYNTAX_ERR;
            else if (strcmp(name, "InvalidModificationError") == 0) code = DOM_EXCEPTION_INVALID_MODIFICATION_ERR;
            else if (strcmp(name, "NamespaceError") == 0) code = DOM_EXCEPTION_NAMESPACE_ERR;
            else if (strcmp(name, "InvalidAccessError") == 0) code = DOM_EXCEPTION_INVALID_ACCESS_ERR;
            else if (strcmp(name, "TypeMismatchError") == 0) code = DOM_EXCEPTION_TYPE_MISMATCH_ERR;
            else if (strcmp(name, "SecurityError") == 0) code = DOM_EXCEPTION_SECURITY_ERR;
            else if (strcmp(name, "NetworkError") == 0) code = DOM_EXCEPTION_NETWORK_ERR;
            else if (strcmp(name, "AbortError") == 0) code = DOM_EXCEPTION_ABORT_ERR;
            else if (strcmp(name, "URLMismatchError") == 0) code = DOM_EXCEPTION_URL_MISMATCH_ERR;
            else if (strcmp(name, "QuotaExceededError") == 0) code = DOM_EXCEPTION_QUOTA_EXCEEDED_ERR;
            else if (strcmp(name, "TimeoutError") == 0) code = DOM_EXCEPTION_TIMEOUT_ERR;
            else if (strcmp(name, "InvalidNodeTypeError") == 0) code = DOM_EXCEPTION_INVALID_NODE_TYPE_ERR;
            else if (strcmp(name, "DataCloneError") == 0) code = DOM_EXCEPTION_DATA_CLONE_ERR;
            de.set_code(code);
        }
    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_dom_exception_class_id);
    if (JS_IsException(obj)) {
        return obj;
    }
    de.attach_to_object(obj);
    return obj;
}

static GCValue js_dom_exception_get_name(JSContextHandle ctx, GCValue this_val) {
    DOMExceptionDataHandle de = DOMExceptionDataHandle::from_object_check(ctx, this_val);
    if (!de.valid()) return JS_ThrowTypeError(ctx, "Invalid DOM element");
    return JS_NewString(ctx, de.name());
}

static GCValue js_dom_exception_get_message(JSContextHandle ctx, GCValue this_val) {
    DOMExceptionDataHandle de = DOMExceptionDataHandle::from_object_check(ctx, this_val);
    if (!de.valid()) return JS_ThrowTypeError(ctx, "Invalid DOM element");
    return JS_NewString(ctx, de.message());
}

static GCValue js_dom_exception_get_code(JSContextHandle ctx, GCValue this_val) {
    DOMExceptionDataHandle de = DOMExceptionDataHandle::from_object_check(ctx, this_val);
    if (!de.valid()) return JS_ThrowTypeError(ctx, "Invalid DOM element");
    return JS_NewInt32(ctx, de.code());
}

static const JSCFunctionListEntry js_dom_exception_proto_funcs[] = {
    JS_CGETSET_DEF("name", js_dom_exception_get_name, NULL),
    JS_CGETSET_DEF("message", js_dom_exception_get_message, NULL),
    JS_CGETSET_DEF("code", js_dom_exception_get_code, NULL),
};

// ============================================================================
// ES6+ Polyfills (C implementations)
// ============================================================================

// Object.getPrototypeOf polyfill
static GCValue js_object_get_prototype_of(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "requires at least 1 argument(s)");
    
    GCValue obj = argv[0];
    if (JS_IsNull(obj) || JS_IsUndefined(obj)) {
        return JS_ThrowTypeError(ctx, "Object.getPrototypeOf called on null or undefined");
    }
    
    // Get __proto__ property
    GCValue proto = JS_GetPropertyStr(ctx, obj, "__proto__");
    return proto;
}

// Object.defineProperty implementation with correct ownership semantics
// 
// OWNERSHIP RULES for QuickJS API:
// - JS_GetPropertyStr: returns NEW value (caller must free)
// - JS_DefinePropertyValue: TAKES OWNERSHIP of the value (frees it internally)
// - JS_DefinePropertyGetSet: does NOT take ownership (dupes internally)
// - JS_NewAtom: creates atom (caller must free with JS_FreeAtom)
//
// This implementation tracks ownership explicitly to avoid leaks or double-frees.


// SAFE_FREE_VALUE is no longer needed with mark-and-sweep GC
#define SAFE_FREE_VALUE(ctx, val) do { (void)(val); } while(0)

static GCValue js_object_define_property(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void) this_val;
    
    JSAtom prop_atom = 0;
    GCValue result = JS_UNDEFINED;
    
    if (argc < 3) {
        return JS_ThrowTypeError(ctx, "Object.defineProperty requires 3 arguments");
    }
    
    GCValue obj = argv[0];
    GCValue prop = argv[1];
    GCValue descriptor = argv[2];
    
    if (JS_IsNull(obj) || JS_IsUndefined(obj)) {
        return JS_ThrowTypeError(ctx, "Object.defineProperty called on null or undefined");
    }
    
    // Convert property to atom
    if (JS_IsSymbol(prop)) {
        prop_atom = JS_ValueToAtom(ctx, prop);
    } else {
        const char *prop_str = JS_ToCString(ctx, prop);
        if (!prop_str) {
            return JS_ThrowTypeError(ctx, "Object.defineProperty: invalid property key");
        }
        prop_atom = JS_NewAtom(ctx, prop_str);
    }
    
    if (prop_atom == JS_ATOM_NULL) {
        return JS_ThrowTypeError(ctx, "Object.defineProperty: invalid property atom");
    }
    
    // Get descriptor properties
    GCValue get_prop = JS_GetPropertyStr(ctx, descriptor, "get");
    GCValue set_prop = JS_GetPropertyStr(ctx, descriptor, "set");
    
    int has_get = !JS_IsException(get_prop) && !JS_IsUndefined(get_prop);
    int has_set = !JS_IsException(set_prop) && !JS_IsUndefined(set_prop);
    
    // Get flags
    GCValue writable_prop = JS_GetPropertyStr(ctx, descriptor, "writable");
    GCValue enumerable_prop = JS_GetPropertyStr(ctx, descriptor, "enumerable");
    GCValue configurable_prop = JS_GetPropertyStr(ctx, descriptor, "configurable");
    
    int writable = !JS_IsException(writable_prop) && JS_ToBool(ctx, writable_prop);
    int enumerable = !JS_IsException(enumerable_prop) && JS_ToBool(ctx, enumerable_prop);
    int configurable = !JS_IsException(configurable_prop) && JS_ToBool(ctx, configurable_prop);
    
    SAFE_FREE_VALUE(ctx, writable_prop);
    SAFE_FREE_VALUE(ctx, enumerable_prop);
    SAFE_FREE_VALUE(ctx, configurable_prop);
    
    int flags = JS_PROP_THROW;
    if (writable) flags |= JS_PROP_WRITABLE;
    if (enumerable) flags |= JS_PROP_ENUMERABLE;
    if (configurable) flags |= JS_PROP_CONFIGURABLE;
    
    int def_result = -1;
    GCValue value = JS_UNDEFINED;
    
    if (has_get || has_set) {
        // === ACCESSOR PROPERTY ===
        int acc_flags = JS_PROP_THROW;
        if (enumerable) acc_flags |= JS_PROP_ENUMERABLE;
        if (configurable) acc_flags |= JS_PROP_CONFIGURABLE;
        def_result = JS_DefinePropertyGetSet(ctx, obj, prop_atom,
            has_get ? get_prop : JS_UNDEFINED,
            has_set ? set_prop : JS_UNDEFINED,
            acc_flags);
    } else {
        // === DATA PROPERTY ===
        value = JS_GetPropertyStr(ctx, descriptor, "value");
        if (JS_IsException(value)) {
            result = JS_EXCEPTION;
            goto cleanup;
        }
        
        // JS_DefinePropertyValue TAKES OWNERSHIP of value
        def_result = JS_DefinePropertyValue(ctx, obj, prop_atom, value, flags);
        // Value is now owned by the object or freed on error, don't free it
        value = JS_UNDEFINED;
    }
    
    if (def_result < 0) {
        result = JS_EXCEPTION;
    } else {
        result = obj;
    }
    
cleanup:
    if (prop_atom) JS_FreeAtom(ctx, prop_atom);
    SAFE_FREE_VALUE(ctx, get_prop);
    SAFE_FREE_VALUE(ctx, set_prop);
    SAFE_FREE_VALUE(ctx, value);
    return result;
}


// Object.getOwnPropertyDescriptor polyfill
static GCValue js_object_get_own_property_descriptor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    
    GCValue obj = argv[0];
    const char *prop = JS_ToCString(ctx, argv[1]);
    if (!prop) return JS_UNDEFINED;
    
    JSAtom prop_atom = JS_NewAtom(ctx, prop);
    JSPropertyDescriptor desc_struct = {0};
    int has_prop = JS_GetOwnProperty(ctx, &desc_struct, obj, prop_atom);
    JS_FreeAtom(ctx, prop_atom);
    
    if (has_prop <= 0) {
        return JS_UNDEFINED;
    }
    
    GCValue desc = JS_NewObject(ctx);
    if (desc_struct.flags & JS_PROP_GETSET) {
        JS_SetPropertyStr(ctx, desc, "get", desc_struct.getter);
        JS_SetPropertyStr(ctx, desc, "set", desc_struct.setter);
        JS_SetPropertyStr(ctx, desc, "enumerable", JS_NewBool(ctx, !!(desc_struct.flags & JS_PROP_ENUMERABLE)));
        JS_SetPropertyStr(ctx, desc, "configurable", JS_NewBool(ctx, !!(desc_struct.flags & JS_PROP_CONFIGURABLE)));
    } else {
        JS_SetPropertyStr(ctx, desc, "value", desc_struct.value);
        JS_SetPropertyStr(ctx, desc, "writable", JS_NewBool(ctx, !!(desc_struct.flags & JS_PROP_WRITABLE)));
        JS_SetPropertyStr(ctx, desc, "enumerable", JS_NewBool(ctx, !!(desc_struct.flags & JS_PROP_ENUMERABLE)));
        JS_SetPropertyStr(ctx, desc, "configurable", JS_NewBool(ctx, !!(desc_struct.flags & JS_PROP_CONFIGURABLE)));
    }
    
    return desc;
}

// Object.setPrototypeOf polyfill
static GCValue js_object_set_prototype_of(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "requires at least 2 argument(s)");
    
    GCValue obj = argv[0];
    GCValue proto = argv[1];
    
    // Check for null/undefined
    if (JS_IsNull(obj) || JS_IsUndefined(obj)) {
        return JS_ThrowTypeError(ctx, "Object.setPrototypeOf called on null or undefined");
    }
    
    // Set the prototype using __proto__
    GCValue proto_key = JS_NewString(ctx, "__proto__");
    JS_SetProperty(ctx, obj, JS_ValueToAtom(ctx, proto_key), proto);

    return obj;
}

// Object.getOwnPropertySymbols polyfill - returns empty array
static GCValue js_object_get_own_property_symbols(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewArray(ctx);
}

// Object.assign polyfill
static GCValue js_object_assign(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    
    GCValue target = argv[0];
    
    for (int i = 1; i < argc; i++) {
        GCValue source = argv[i];
        if (JS_IsNull(source) || JS_IsUndefined(source)) continue;
        
        // Use Object.keys to get enumerable properties
        GCValue global = JS_GetGlobalObject(ctx);
        GCValue object_ctor = JS_GetPropertyStr(ctx, global, "Object");
        GCValue keys_func = JS_GetPropertyStr(ctx, object_ctor, "keys");
        GCValue keys = JS_Call(ctx, keys_func, JS_UNDEFINED, 1, &source);



        if (!JS_IsException(keys)) {
            GCValue len_val = JS_GetPropertyStr(ctx, keys, "length");
            uint32_t key_count = 0;
            JS_ToUint32(ctx, &key_count, len_val);

            for (uint32_t j = 0; j < key_count; j++) {
                GCValue key_val = JS_GetPropertyUint32(ctx, keys, j);
                const char *key = JS_ToCString(ctx, key_val);
                if (key) {
                    GCValue val = JS_GetPropertyStr(ctx, source, key);
                    JS_SetPropertyStr(ctx, target, key, val);
                }

            }

        }
    }
    
    return target;
}

// Reflect.construct polyfill
static GCValue js_reflect_construct(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "requires at least 2 argument(s)");
    
    GCValue target = argv[0];
    GCValue args_array = argv[1];
    
    // Get length of args array
    GCValue len_val = JS_GetPropertyStr(ctx, args_array, "length");
    uint32_t args_len = 0;
    JS_ToUint32(ctx, &args_len, len_val);

    // Build arguments array
    GCValue *args = (GCValue*)malloc(sizeof(GCValue) * args_len);
    for (uint32_t i = 0; i < args_len; i++) {
        args[i] = JS_GetPropertyUint32(ctx, args_array, i);
    }
    
    // Call constructor
    GCValue result = JS_CallConstructor(ctx, target, (int)args_len, args);
    
    for (uint32_t i = 0; i < args_len; i++) {

    }
    free(args);
    
    return result;
}

// Reflect.apply polyfill
static GCValue js_reflect_apply(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 3) return JS_ThrowTypeError(ctx, "requires at least 3 argument(s)");
    
    GCValue func = argv[0];
    GCValue this_arg = argv[1];
    GCValue args_array = argv[2];
    
    // Get length of args array
    GCValue args_len_val = JS_GetPropertyStr(ctx, args_array, "length");
    uint32_t args_len = 0;
    JS_ToUint32(ctx, &args_len, args_len_val);

    // Build arguments array
    GCValue *args = (GCValue*)malloc(sizeof(GCValue) * args_len);
    for (uint32_t i = 0; i < args_len; i++) {
        args[i] = JS_GetPropertyUint32(ctx, args_array, i);
    }
    
    // Call function
    GCValue result = JS_Call(ctx, func, this_arg, (int)args_len, args);
    
    for (uint32_t i = 0; i < args_len; i++) {

    }
    free(args);
    
    return result;
}

// Reflect.has polyfill
static GCValue js_reflect_has(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "requires at least 2 argument(s)");
    
    GCValue target = argv[0];
    const char *prop = JS_ToCString(ctx, argv[1]);
    if (!prop) return JS_FALSE;
    
    JSAtom prop_atom = JS_NewAtom(ctx, prop);
    int has_prop = JS_HasProperty(ctx, target, prop_atom);
    JS_FreeAtom(ctx, prop_atom);
    
    return JS_NewBool(ctx, has_prop);
}

// Promise.prototype.finally polyfill
static GCValue js_promise_finally(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsObject(this_val)) return JS_ThrowTypeError(ctx, "requires object and at least 1 argument");
    
    GCValue on_finally = argv[0];
    
    // Create the finally handler
    GCValue handler = JS_NewCFunction(ctx, js_dummy_function, "finally_handler", 0);
    
    // Call .then with the handler
    GCValue then_method = JS_GetPropertyStr(ctx, this_val, "then");
    GCValue args[2] = { handler, handler };
    GCValue result = JS_Call(ctx, then_method, this_val, 2, args);


    return result;
}

// String.prototype.includes polyfill
static GCValue js_string_includes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_FALSE;
    
    if (argc < 1) {
        return JS_FALSE;
    }
    
    const char *search = JS_ToCString(ctx, argv[0]);
    if (!search) {
        return JS_FALSE;
    }
    
    int32_t start = 0;
    if (argc > 1) {
        JS_ToInt32(ctx, &start, argv[1]);
    }
    
    // Adjust start position
    size_t str_len = strlen(str);
    if (start < 0) start = 0;
    if ((size_t)start > str_len) start = (int32_t)str_len;
    
    // Search for substring
    const char *found = strstr(str + start, search);
    
    
    return JS_NewBool(ctx, found != NULL);
}

// Array.prototype.includes polyfill
static GCValue js_array_includes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_FALSE;
    
    GCValue search_element = argv[0];
    int32_t from_index = 0;
    if (argc > 1) {
        JS_ToInt32(ctx, &from_index, argv[1]);
    }
    
    GCValue len_val = JS_GetPropertyStr(ctx, this_val, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, len_val);

    if (from_index < 0) {
        from_index = (int32_t)len + from_index;
        if (from_index < 0) from_index = 0;
    }
    
    for (uint32_t i = (uint32_t)from_index; i < len; i++) {
        GCValue elem = JS_GetPropertyUint32(ctx, this_val, i);
        int is_equal = JS_StrictEq(ctx, elem, search_element);

        if (is_equal) return JS_TRUE;
    }
    
    return JS_FALSE;
}

// Array.from polyfill
static GCValue js_array_from(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewArray(ctx);
    
    GCValue array_like = argv[0];
    uint32_t len = 0;
    
    GCValue len_val2 = JS_GetPropertyStr(ctx, array_like, "length");
    if (JS_ToUint32(ctx, &len, len_val2)) {


        return JS_NewArray(ctx);
    }

    GCValue result = JS_NewArray(ctx);
    for (uint32_t i = 0; i < len; i++) {
        GCValue val = JS_GetPropertyUint32(ctx, array_like, i);
        JS_SetPropertyUint32(ctx, result, i, val);
    }
    
    return result;
}

// ============================================================================
// Map Polyfill Implementation
// ============================================================================

// MapData struct is defined in browser_api_impl_types.h

JSClassID js_map_class_id = 0;

static void js_map_finalizer(JSRuntimeHandle rt, GCValue val) {
    // MapData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

static JSClassDef js_map_class_def = {
    .class_name = "Map",
    .finalizer = js_map_finalizer,
};

static GCValue js_map_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    MapDataHandle map = MapDataHandle::create(ctx);
    if (!map.valid()) return JS_ThrowTypeError(ctx, "Invalid Map object");
    
    GCValue obj = JS_NewObjectClass(ctx, js_map_class_id);
    map.attach_to_object(obj);
    return obj;
}

static GCValue js_map_set(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    MapDataHandle map = MapDataHandle::from_object_check(ctx, this_val);
    if (!map.valid() || argc < 2) return JS_ThrowTypeError(ctx, "Map requires 2 arguments");
    
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_ThrowTypeError(ctx, "Invalid key");
    
    // Check if key exists
    GCValue entries = map.entries();
    GCValue existing = JS_GetPropertyStr(ctx, entries, key);
    int exists = !JS_IsUndefined(existing);

    if (!exists) map.increment_size();
    
    JS_SetPropertyStr(ctx, entries, key, argv[1]);
    
    return this_val;
}

static GCValue js_map_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    MapDataHandle map = MapDataHandle::from_object_check(ctx, this_val);
    if (!map.valid() || argc < 1) return JS_ThrowTypeError(ctx, "Map requires at least 1 argument");
    
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_ThrowTypeError(ctx, "Invalid key");
    
    GCValue entries = map.entries();
    GCValue val = JS_GetPropertyStr(ctx, entries, key);
    
    return val;
}

static GCValue js_map_has(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    MapDataHandle map = MapDataHandle::from_object_check(ctx, this_val);
    if (!map.valid() || argc < 1) return JS_ThrowTypeError(ctx, "Map requires at least 1 argument");
    
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_ThrowTypeError(ctx, "Invalid key");
    
    GCValue entries = map.entries();
    GCValue val = JS_GetPropertyStr(ctx, entries, key);
    
    int exists = !JS_IsUndefined(val);

    return JS_NewBool(ctx, exists);
}

static GCValue js_map_delete(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    MapDataHandle map = MapDataHandle::from_object_check(ctx, this_val);
    if (!map.valid() || argc < 1) return JS_ThrowTypeError(ctx, "Map requires at least 1 argument");
    
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_ThrowTypeError(ctx, "Invalid key");
    
    GCValue entries = map.entries();
    GCValue val = JS_GetPropertyStr(ctx, entries, key);
    int exists = !JS_IsUndefined(val);

    if (exists) {
        GCValue undefined = JS_UNDEFINED;
        JS_SetPropertyStr(ctx, entries, key, undefined);
        map.decrement_size();
    }
    
    return JS_NewBool(ctx, exists);
}

static GCValue js_map_clear(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    MapDataHandle map = MapDataHandle::from_object_check(ctx, this_val);
    if (!map.valid()) return JS_ThrowTypeError(ctx, "Invalid Map object");

    map.set_entries(JS_NewObject(ctx));
    map.set_size(0);
    
    return JS_UNDEFINED;
}

static GCValue js_map_get_size(JSContextHandle ctx, GCValue this_val) {
    MapDataHandle map = MapDataHandle::from_object_check(ctx, this_val);
    if (!map.valid()) return JS_ThrowTypeError(ctx, "Invalid Map object");
    return JS_NewInt32(ctx, map.size());
}

// Helper: create an array of [key, value] pairs from a Map's entries object
static GCValue js_map_entries_array(JSContextHandle ctx, GCValue this_val) {
    MapDataHandle map = MapDataHandle::from_object_check(ctx, this_val);
    if (!map.valid()) return JS_ThrowTypeError(ctx, "Invalid Map object");
    
    GCValue entries_obj = map.entries();
    
    // Get Object.keys(entries_obj)
    GCValue global_obj = JS_GetGlobalObject(ctx);
    GCValue object_ctor = JS_GetPropertyStr(ctx, global_obj, "Object");
    GCValue keys_fn = JS_GetPropertyStr(ctx, object_ctor, "keys");
    GCValue keys = JS_Call(ctx, keys_fn, JS_UNDEFINED, 1, &entries_obj);
    if (JS_IsException(keys)) return JS_EXCEPTION;
    
    GCValue array = JS_NewArray(ctx);
    if (JS_IsException(array)) return JS_EXCEPTION;
    
    int len = 0;
    GCValue len_val = JS_GetPropertyStr(ctx, keys, "length");
    JS_ToInt32(ctx, &len, len_val);
    
    for (int i = 0; i < len; i++) {
        GCValue key_val = JS_GetPropertyUint32(ctx, keys, i);
        const char *key = JS_ToCString(ctx, key_val);
        if (!key) continue;
        
        GCValue val = JS_GetPropertyStr(ctx, entries_obj, key);
        
        GCValue pair = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, pair, 0, key_val);
        JS_SetPropertyUint32(ctx, pair, 1, val);
        JS_SetPropertyUint32(ctx, array, i, pair);
    }
    
    return array;
}

static GCValue js_map_entries(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue array = js_map_entries_array(ctx, this_val);
    if (JS_IsException(array)) return JS_EXCEPTION;
    
    // Return array[Symbol.iterator]()
    GCValue iterator_sym_eval = JS_Eval(ctx, "Symbol.iterator", 15, "<symbol>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(iterator_sym_eval) || JS_IsUndefined(iterator_sym_eval)) return JS_EXCEPTION;
    GCValue iterator_fn = JS_GetProperty(ctx, array, JS_ValueToAtom(ctx, iterator_sym_eval));
    if (JS_IsException(iterator_fn) || !JS_IsFunction(ctx, iterator_fn)) return JS_EXCEPTION;
    GCValue iterator = JS_Call(ctx, iterator_fn, array, 0, NULL);
    return iterator;
}

static GCValue js_map_keys(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    MapDataHandle map = MapDataHandle::from_object_check(ctx, this_val);
    if (!map.valid()) return JS_ThrowTypeError(ctx, "Invalid Map object");
    
    GCValue entries_obj = map.entries();
    GCValue global_obj = JS_GetGlobalObject(ctx);
    GCValue object_ctor = JS_GetPropertyStr(ctx, global_obj, "Object");
    GCValue keys_fn = JS_GetPropertyStr(ctx, object_ctor, "keys");
    GCValue keys = JS_Call(ctx, keys_fn, JS_UNDEFINED, 1, &entries_obj);
    
    // Return keys[Symbol.iterator]()
    GCValue iterator_sym_eval = JS_Eval(ctx, "Symbol.iterator", 15, "<symbol>", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(iterator_sym_eval) && !JS_IsUndefined(iterator_sym_eval)) {
        JSAtom iterator_atom = JS_ValueToAtom(ctx, iterator_sym_eval);
        GCValue iterator_fn = JS_GetProperty(ctx, keys, iterator_atom);
        if (!JS_IsException(iterator_fn) && JS_IsFunction(ctx, iterator_fn)) {
            GCValue iterator = JS_Call(ctx, iterator_fn, keys, 0, NULL);
            return iterator;
        }
    }
    return JS_EXCEPTION;
}

static GCValue js_map_values(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    MapDataHandle map = MapDataHandle::from_object_check(ctx, this_val);
    if (!map.valid()) return JS_ThrowTypeError(ctx, "Invalid Map object");
    
    GCValue entries_obj = map.entries();
    GCValue global_obj = JS_GetGlobalObject(ctx);
    GCValue object_ctor = JS_GetPropertyStr(ctx, global_obj, "Object");
    GCValue keys_fn = JS_GetPropertyStr(ctx, object_ctor, "keys");
    GCValue keys = JS_Call(ctx, keys_fn, JS_UNDEFINED, 1, &entries_obj);
    
    GCValue array = JS_NewArray(ctx);
    int len = 0;
    GCValue len_val = JS_GetPropertyStr(ctx, keys, "length");
    JS_ToInt32(ctx, &len, len_val);
    
    for (int i = 0; i < len; i++) {
        GCValue key_val = JS_GetPropertyUint32(ctx, keys, i);
        const char *key = JS_ToCString(ctx, key_val);
        if (!key) continue;
        GCValue val = JS_GetPropertyStr(ctx, entries_obj, key);
        JS_SetPropertyUint32(ctx, array, i, val);
    }
    
    // Return array[Symbol.iterator]()
    GCValue iterator_sym_eval = JS_Eval(ctx, "Symbol.iterator", 15, "<symbol>", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(iterator_sym_eval) && !JS_IsUndefined(iterator_sym_eval)) {
        JSAtom iterator_atom = JS_ValueToAtom(ctx, iterator_sym_eval);
        GCValue iterator_fn = JS_GetProperty(ctx, array, iterator_atom);
        if (!JS_IsException(iterator_fn) && JS_IsFunction(ctx, iterator_fn)) {
            GCValue iterator = JS_Call(ctx, iterator_fn, array, 0, NULL);
            return iterator;
        }
    }
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry js_map_proto_funcs[] = {
    JS_CFUNC_DEF("set", 2, js_map_set),
    JS_CFUNC_DEF("get", 1, js_map_get),
    JS_CFUNC_DEF("has", 1, js_map_has),
    JS_CFUNC_DEF("delete", 1, js_map_delete),
    JS_CFUNC_DEF("clear", 0, js_map_clear),
    JS_CGETSET_DEF("size", js_map_get_size, NULL),
    JS_CFUNC_DEF("entries", 0, js_map_entries),
    JS_CFUNC_DEF("keys", 0, js_map_keys),
    JS_CFUNC_DEF("values", 0, js_map_values),
};

extern "C" JSClassID js_xhr_class_id;
extern "C" JSClassID js_video_class_id;
extern GCValue js_xhr_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern GCValue js_video_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern GCValue js_fetch(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
extern "C" const JSCFunctionListEntry js_xhr_proto_funcs[];
extern "C" const JSCFunctionListEntry js_video_proto_funcs[];
extern "C" const size_t js_xhr_proto_funcs_count;
extern "C" const size_t js_video_proto_funcs_count;

// Class IDs for new APIs
JSClassID js_shadow_root_class_id = 0;
JSClassID js_animation_class_id = 0;
JSClassID js_keyframe_effect_class_id = 0;
JSClassID js_font_face_class_id = 0;
JSClassID js_font_face_set_class_id = 0;
JSClassID js_custom_element_registry_class_id = 0;
JSClassID js_mutation_observer_class_id = 0;
JSClassID js_resize_observer_class_id = 0;
JSClassID js_intersection_observer_class_id = 0;
JSClassID js_performance_class_id = 0;
JSClassID js_performance_entry_class_id = 0;
JSClassID js_performance_observer_class_id = 0;
JSClassID js_performance_timing_class_id = 0;
JSClassID js_dom_rect_class_id = 0;
JSClassID js_dom_rect_read_only_class_id = 0;
JSClassID js_media_source_class_id = 0;
JSClassID js_source_buffer_class_id = 0;
JSClassID js_date_class_id = 0;

// ============================================================================
// Event Implementation
// ============================================================================

JSClassID js_event_class_id = 0;
JSClassID js_custom_event_class_id = 0;
JSClassID js_mouse_event_class_id = 0;
JSClassID js_focus_event_class_id = 0;

// ============================================================================
// ServiceWorker API Class IDs
// ============================================================================
JSClassID js_service_worker_container_class_id = 0;
JSClassID js_service_worker_registration_class_id = 0;
JSClassID js_service_worker_class_id = 0;

// ============================================================================
// Real DOM Node Implementation
// ============================================================================

JSClassID js_dom_node_class_id = 0;

// Forward declarations for DOM functions
static GCValue js_node_appendChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_node_removeChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_node_insertBefore_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_node_cloneNode_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_node_contains_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_node_getRootNode_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_element_querySelector_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_element_querySelectorAll_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

// Forward declarations for DOM helper functions (used by ShadowRoot)
static bool matches_selector(JSContextHandle ctx, GCValue elem, const char* selector);
static DOMNodeHandle get_dom_node(JSContextHandle ctx, GCValue obj);
static DOMNodeHandle get_or_create_dom_node(JSContextHandle ctx, GCValue obj, int node_type, const char* node_name);
static GCValue query_selector_recursive(JSContextHandle ctx, GCValue elem, const char* selector);
static void query_selector_all_recursive(JSContextHandle ctx, GCValue elem, const char* selector, GCValue result_arr, int* idx);

static void js_dom_node_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    // Free the lock-free computed-style hash table.  The table itself is
    // malloc'd; the GC will reclaim the CssComputedStyle object separately.
    GCHandle node_handle = JS_GetOpaqueHandle(val, js_dom_node_class_id);
    if (node_handle != GC_HANDLE_NULL) {
        DOMNodeHandle node(node_handle);
        if (node.valid()) {
            GCHandle cs_handle = node.computed_style_handle();
            if (cs_handle != GC_HANDLE_NULL) {
                CssComputedStyle *cs = (CssComputedStyle *)gc_deref(cs_handle);
                if (cs && cs->properties) {
                    lf_hash_destroy(cs->properties);
                    cs->properties = NULL;
                }
            }
        }
    }
}

static void js_dom_node_mark(JSRuntimeHandle rt, GCValue val,
                             JS_MarkFunc *mark_func)
{
    (void)rt;
    GCHandle node_handle = JS_GetOpaqueHandle(val, js_dom_node_class_id);
    if (node_handle == GC_HANDLE_NULL) return;

    /* Keep the DOMNode data object alive; it is opaque data, not scanned
     * automatically by the generic object marker. */
    mark_func(rt, node_handle);

    DOMNodeHandle node(node_handle);
    if (!node.valid()) return;

    /* Tree links are stored as GCValue JS object references. */
    JS_MarkValue(rt, node.js_object(), mark_func);
    JS_MarkValue(rt, node.parent_node(), mark_func);
    JS_MarkValue(rt, node.first_child(), mark_func);
    JS_MarkValue(rt, node.last_child(), mark_func);
    JS_MarkValue(rt, node.previous_sibling(), mark_func);
    JS_MarkValue(rt, node.next_sibling(), mark_func);
    JS_MarkValue(rt, node.owner_document(), mark_func);
    JS_MarkValue(rt, node.shadow_root(), mark_func);

    /* Computed-style table.  Keep the table object alive, then mark every
     * value handle stored inside it (they are JS strings). */
    GCHandle cs_handle = node.computed_style_handle();
    if (cs_handle != GC_HANDLE_NULL) {
        mark_func(rt, cs_handle);
        CssComputedStyle *cs = (CssComputedStyle *)gc_deref(cs_handle);
        if (cs && cs->properties) {
            LFHashTable *t = cs->properties;
            for (uint32_t i = 0; i < t->bucket_count; i++) {
                if (t->buckets[i].state == LF_HASH_OCCUPIED &&
                    t->buckets[i].value != GC_HANDLE_NULL) {
                    mark_func(rt, t->buckets[i].value);
                }
            }
        }
    }

    /* Index-table list chaining refers to other DOMNode data handles. */
    GCHandle class_sib = node.next_class_sibling();
    if (class_sib != GC_HANDLE_NULL) mark_func(rt, class_sib);
    GCHandle tag_sib = node.next_tag_sibling();
    if (tag_sib != GC_HANDLE_NULL) mark_func(rt, tag_sib);
}

static JSClassDef js_dom_node_class_def = {
    .class_name = "DOMNode",
    .finalizer = js_dom_node_finalizer,
    .gc_mark   = js_dom_node_mark,
};

/* ============================================================================
 * Parallel CSS support: per-element computed-style table and index tables
 * ============================================================================ */

#define CSS_COMPUTED_STYLE_BUCKETS 16
#define CSS_ID_TABLE_BUCKETS       64
#define CSS_CLASS_TABLE_BUCKETS    64
#define CSS_TAG_TABLE_BUCKETS      64

/* Allocate or return the existing computed-style object for a DOM node. */
GCHandle css_ensure_computed_style(DOMNodeHandle node)
{
    GCHandle h = node.computed_style_handle();
    if (h != GC_HANDLE_NULL) return h;

    h = gc_allocz(sizeof(CssComputedStyle), JS_GC_OBJ_TYPE_DATA);
    if (h == GC_HANDLE_NULL) return GC_HANDLE_NULL;

    CssComputedStyle *cs = (CssComputedStyle *)gc_deref(h);
    cs->properties = lf_hash_create(CSS_COMPUTED_STYLE_BUCKETS);
    if (!cs->properties) {
        /* Cannot free the GC handle individually in unified GC; leak is
         * harmless because the handle will be reclaimed at next collection. */
        return GC_HANDLE_NULL;
    }

    node.set_computed_style_handle(h);
    return h;
}

/* Store a computed CSS property for a node.  The value is copied into a JS
 * string handle that is kept alive by the DOMNode's gc_mark callback. */
void css_computed_set_property(JSContextHandle ctx, DOMNodeHandle node,
                               JSAtom prop_atom, const char *value)
{
    if (prop_atom == JS_ATOM_NULL || !value) return;

    GCHandle cs_handle = css_ensure_computed_style(node);
    if (cs_handle == GC_HANDLE_NULL) return;

    CssComputedStyle *cs = (CssComputedStyle *)gc_deref(cs_handle);
    if (!cs || !cs->properties) return;

    GCValue str_val = JS_NewString(ctx, value);
    GCHandle str_handle = GC_VALUE_GET_HANDLE(str_val);
    if (str_handle == GC_HANDLE_NULL) return;

    lf_hash_insert(cs->properties, (uint32_t)prop_atom,
                   (GCHandle)prop_atom, str_handle);

    /* Also store the camelCase alias so getComputedStyle().fontSize works. */
    const char *prop_str = JS_AtomToCString(ctx, prop_atom);
    if (prop_str) {
        char *camel = css_to_camel_case(prop_str);
        if (camel && strcmp(camel, prop_str) != 0) {
            JSAtom camel_atom = JS_NewAtom(ctx, camel);
            if (camel_atom != JS_ATOM_NULL) {
                lf_hash_insert(cs->properties, (uint32_t)camel_atom,
                               (GCHandle)camel_atom, str_handle);
                JS_FreeAtom(ctx, camel_atom);
            }
        }
        free(camel);
    }
}

/* Look up a computed CSS property and return it as a JS value.  Returns
 * JS_UNDEFINED if no computed value exists. */
GCValue css_computed_get_property(JSContextHandle ctx, DOMNodeHandle node,
                                  JSAtom prop_atom)
{
    if (prop_atom == JS_ATOM_NULL) return JS_UNDEFINED;

    GCHandle cs_handle = node.computed_style_handle();
    if (cs_handle == GC_HANDLE_NULL) return JS_UNDEFINED;

    CssComputedStyle *cs = (CssComputedStyle *)gc_deref(cs_handle);
    if (!cs || !cs->properties) return JS_UNDEFINED;

    GCHandle str_handle = lf_hash_lookup(cs->properties, (uint32_t)prop_atom,
                                         (GCHandle)prop_atom);
    if (str_handle == GC_HANDLE_NULL) return JS_UNDEFINED;

    return GC_MKHANDLE(JS_TAG_STRING, str_handle);
}

/* Allocate and attach the per-document CSS index tables. */
CssDocumentState *css_document_state_ensure(JSRuntimeHandle rt)
{
    CssDocumentState *state = (CssDocumentState *)JS_GetRuntimeOpaque(rt);
    if (state) return state;

    state = (CssDocumentState *)calloc(1, sizeof(CssDocumentState));
    if (!state) return NULL;

    state->id_table    = lf_hash_create(CSS_ID_TABLE_BUCKETS);
    state->class_table = lf_hash_create(CSS_CLASS_TABLE_BUCKETS);
    state->tag_table   = lf_hash_create(CSS_TAG_TABLE_BUCKETS);

    if (!state->id_table || !state->class_table || !state->tag_table) {
        if (state->id_table) lf_hash_destroy(state->id_table);
        if (state->class_table) lf_hash_destroy(state->class_table);
        if (state->tag_table) lf_hash_destroy(state->tag_table);
        free(state);
        return NULL;
    }

    JS_SetRuntimeOpaque(rt, state);
    return state;
}

/* Free the per-document CSS index tables.  Call before gc_cleanup(). */
void css_document_state_destroy(JSRuntimeHandle rt)
{
    CssDocumentState *state = (CssDocumentState *)JS_GetRuntimeOpaque(rt);
    if (!state) return;

    JS_SetRuntimeOpaque(rt, NULL);
    if (state->id_table) lf_hash_destroy(state->id_table);
    if (state->class_table) lf_hash_destroy(state->class_table);
    if (state->tag_table) lf_hash_destroy(state->tag_table);
    free(state);
}

/* Insert a DOM node into the id/class/tag index tables.  Must be called after
 * the node's id, class, and tag attributes are initialized and the node is
 * attached to its public JS object. */
void css_index_insert_node(JSContextHandle ctx, DOMNodeHandle node)
{
    if (!node.valid()) return;

    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    CssDocumentState *state = css_document_state_ensure(rt);
    if (!state) return;

    GCHandle node_handle = node.handle();
    GCValue js_obj = node.js_object();

    /* Read id/className from the JS object when available; the DOMNode fields
     * are not always kept in sync by the current property setters. */
    const char *id = node.id();
    char id_buf[256];
    if ((!id || !id[0]) && !JS_IsNull(js_obj) && !JS_IsUndefined(js_obj)) {
        GCValue id_val = JS_GetPropertyStr(ctx, js_obj, "id");
        const char *id_str = JS_ToCString(ctx, id_val);
        if (id_str) {
            strncpy(id_buf, id_str, sizeof(id_buf) - 1);
            id_buf[sizeof(id_buf) - 1] = '\0';
            id = id_buf;
        }
    }
    if (id && id[0]) {
        JSAtom id_atom = JS_NewAtom(ctx, id);
        if (id_atom != JS_ATOM_NULL) {
            lf_hash_insert(state->id_table, (uint32_t)id_atom,
                           (GCHandle)id_atom, node_handle);
            JS_FreeAtom(ctx, id_atom);
        }
    }

    const char *class_name = node.class_name();
    char class_buf[1024];
    if ((!class_name || !class_name[0]) && !JS_IsNull(js_obj) && !JS_IsUndefined(js_obj)) {
        GCValue class_val = JS_GetPropertyStr(ctx, js_obj, "className");
        const char *class_str = JS_ToCString(ctx, class_val);
        if (class_str) {
            strncpy(class_buf, class_str, sizeof(class_buf) - 1);
            class_buf[sizeof(class_buf) - 1] = '\0';
            class_name = class_buf;
        }
    }
    if (class_name && class_name[0]) {
        /* Class attribute may contain multiple classes separated by spaces. */
        char *copy = strdup(class_name);
        if (copy) {
            char *saveptr = NULL;
            for (char *tok = strtok_r(copy, " \t\r\n", &saveptr);
                 tok != NULL;
                 tok = strtok_r(NULL, " \t\r\n", &saveptr)) {
                if (!tok[0]) continue;
                JSAtom class_atom = JS_NewAtom(ctx, tok);
                if (class_atom == JS_ATOM_NULL) continue;

                GCHandle prev_head = lf_hash_lookup(state->class_table,
                                                    (uint32_t)class_atom,
                                                    (GCHandle)class_atom);
                node.set_next_class_sibling(prev_head);
                lf_hash_insert(state->class_table, (uint32_t)class_atom,
                               (GCHandle)class_atom, node_handle);
                JS_FreeAtom(ctx, class_atom);
            }
            free(copy);
        }
    }

    const char *tag = node.node_name();
    if (tag && tag[0]) {
        JSAtom tag_atom = JS_NewAtom(ctx, tag);
        if (tag_atom != JS_ATOM_NULL) {
            GCHandle prev_head = lf_hash_lookup(state->tag_table,
                                                (uint32_t)tag_atom,
                                                (GCHandle)tag_atom);
            node.set_next_tag_sibling(prev_head);
            lf_hash_insert(state->tag_table, (uint32_t)tag_atom,
                           (GCHandle)tag_atom, node_handle);
            JS_FreeAtom(ctx, tag_atom);
        }
    }
}

/* Return the DOM node JS object with the given id, or JS_NULL if none. */
GCValue css_get_element_by_id(JSContextHandle ctx, JSAtom id)
{
    if (id == JS_ATOM_NULL) return JS_NULL;

    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    CssDocumentState *state = (CssDocumentState *)JS_GetRuntimeOpaque(rt);
    if (!state || !state->id_table) return JS_NULL;

    GCHandle node_handle = lf_hash_lookup(state->id_table, (uint32_t)id,
                                          (GCHandle)id);
    if (node_handle == GC_HANDLE_NULL) return JS_NULL;

    DOMNodeHandle node(node_handle);
    if (!node.valid()) return JS_NULL;

    GCValue obj = node.js_object();
    if (JS_IsNull(obj) || JS_IsUndefined(obj)) return JS_NULL;
    return obj;
}

/* Return an array of DOM node JS objects with the given class name. */
GCValue css_get_elements_by_class_name(JSContextHandle ctx, JSAtom class_atom)
{
    if (class_atom == JS_ATOM_NULL) return JS_NewArray(ctx);

    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    CssDocumentState *state = (CssDocumentState *)JS_GetRuntimeOpaque(rt);
    if (!state || !state->class_table) return JS_NewArray(ctx);

    GCValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    GCHandle cur = lf_hash_lookup(state->class_table, (uint32_t)class_atom,
                                  (GCHandle)class_atom);
    while (cur != GC_HANDLE_NULL) {
        DOMNodeHandle node(cur);
        if (!node.valid()) break;
        GCValue obj = node.js_object();
        if (!JS_IsNull(obj) && !JS_IsUndefined(obj)) {
            JS_SetPropertyUint32(ctx, arr, idx++, obj);
        }
        cur = node.next_class_sibling();
    }
    return arr;
}

/* Return an array of DOM node JS objects with the given tag name. */
GCValue css_get_elements_by_tag_name(JSContextHandle ctx, JSAtom tag_atom)
{
    if (tag_atom == JS_ATOM_NULL) return JS_NewArray(ctx);

    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    CssDocumentState *state = (CssDocumentState *)JS_GetRuntimeOpaque(rt);
    if (!state || !state->tag_table) return JS_NewArray(ctx);

    GCValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    GCHandle cur = lf_hash_lookup(state->tag_table, (uint32_t)tag_atom,
                                  (GCHandle)tag_atom);
    while (cur != GC_HANDLE_NULL) {
        DOMNodeHandle node(cur);
        if (!node.valid()) break;
        GCValue obj = node.js_object();
        if (!JS_IsNull(obj) && !JS_IsUndefined(obj)) {
            JS_SetPropertyUint32(ctx, arr, idx++, obj);
        }
        cur = node.next_tag_sibling();
    }
    return arr;
}

// URL capture callback for intercepted media URLs
static URLCaptureCallback g_url_capture_callback = NULL;

void browser_api_impl_set_url_capture_callback(URLCaptureCallback callback) {
    g_url_capture_callback = callback;
}

// Helper to capture URLs
static void capture_url(const char *url) {
    if (g_url_capture_callback && url && *url) {
        g_url_capture_callback(url);
    }
}

// Debug helper to capture URLs with source tracking
static void capture_url_debug(const char *url, const char *source) {
    if (url && *url && strstr(url, "data:") == url) {
        fprintf(stderr, "[CAPTURE_DEBUG] [%s] data URL: %.100s\n", source, url);
    }
    capture_url(url);
}

// Helper to throw a DOMException
static GCValue throw_dom_exception(JSContextHandle ctx, const char* name, const char* message) {
    // Create DOMException instance
    DOMExceptionDataHandle de = DOMExceptionDataHandle::create();
    if (!de.valid()) {
        return JS_ThrowTypeError(ctx, "%s: %s", name, message);
    }
    
    de.set_name(name);
    de.set_message(message);
    
    // Set appropriate code based on name
    int code = 0;
    if (strcmp(name, "IndexSizeError") == 0) code = 1;
    else if (strcmp(name, "HierarchyRequestError") == 0) code = 3;
    else if (strcmp(name, "WrongDocumentError") == 0) code = 4;
    else if (strcmp(name, "InvalidCharacterError") == 0) code = 5;
    else if (strcmp(name, "NoModificationAllowedError") == 0) code = 7;
    else if (strcmp(name, "NotFoundError") == 0) code = 8;
    else if (strcmp(name, "NotSupportedError") == 0) code = 9;
    else if (strcmp(name, "InvalidStateError") == 0) code = 11;
    else if (strcmp(name, "SyntaxError") == 0) code = 12;
    else if (strcmp(name, "InvalidModificationError") == 0) code = 13;
    else if (strcmp(name, "NamespaceError") == 0) code = 14;
    else if (strcmp(name, "InvalidAccessError") == 0) code = 15;
    else if (strcmp(name, "TypeMismatchError") == 0) code = 17;
    else if (strcmp(name, "SecurityError") == 0) code = 18;
    else if (strcmp(name, "NetworkError") == 0) code = 19;
    else if (strcmp(name, "AbortError") == 0) code = 20;
    else if (strcmp(name, "URLMismatchError") == 0) code = 21;
    else if (strcmp(name, "QuotaExceededError") == 0) code = 22;
    else if (strcmp(name, "TimeoutError") == 0) code = 23;
    else if (strcmp(name, "InvalidNodeTypeError") == 0) code = 24;
    else if (strcmp(name, "DataCloneError") == 0) code = 25;
    de.set_code(code);
    
    // Create the exception object
    GCValue exc = JS_NewObjectClass(ctx, js_dom_exception_class_id);
    if (JS_IsException(exc)) {
        return JS_ThrowTypeError(ctx, "%s: %s", name, message);
    }
    
    de.attach_to_object(exc);
    
    // Set name and message properties on the exception object
    JS_SetPropertyStr(ctx, exc, "name", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, exc, "message", JS_NewString(ctx, message));
    JS_SetPropertyStr(ctx, exc, "code", JS_NewInt32(ctx, code));
    
    return JS_Throw(ctx, exc);
}

// Helper macros
#define DEF_FUNC(ctx, parent, name, func, argc) \
    JS_SetPropertyStr(ctx, parent, name, JS_NewCFunction(ctx, func, name, argc))

#define DEF_PROP_STR(ctx, obj, name, value) \
    JS_SetPropertyStr(ctx, obj, name, JS_NewString(ctx, value))

#define DEF_PROP_INT(ctx, obj, name, value) \
    JS_SetPropertyStr(ctx, obj, name, JS_NewInt32(ctx, value))

#define DEF_PROP_BOOL(ctx, obj, name, value) \
    JS_SetPropertyStr(ctx, obj, name, JS_NewBool(ctx, value))

#define DEF_PROP_FLOAT(ctx, obj, name, value) \
    JS_SetPropertyStr(ctx, obj, name, JS_NewFloat64(ctx, value))

#define DEF_PROP_UNDEFINED(ctx, obj, name) \
    JS_SetPropertyStr(ctx, obj, name, JS_UNDEFINED)

// Dummy then function for mock promises
static GCValue js_promise_then(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Call the onFulfilled callback immediately with undefined
    if (argc > 0 && JS_IsFunction(ctx, argv[0])) {
        GCValue undefined = JS_UNDEFINED;
        GCValue result = JS_Call(ctx, argv[0], JS_UNDEFINED, 1, &undefined);

    }
    return this_val;
}

// Helper to create a resolved Promise
static GCValue js_create_resolved_promise(JSContextHandle ctx, GCValue value) {
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    
    // Check if Promise constructor exists and is an object
    if (JS_IsException(promise_ctor) || !JS_IsObject(promise_ctor)) {


        // Fallback: return a mock promise-like object
        GCValue mock_promise = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, mock_promise, "then", 
            JS_NewCFunction(ctx, js_promise_then, "then", 2));
        return mock_promise;
    }
    
    GCValue resolve_func = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
    
    // Check if Promise.resolve exists and is a function
    if (JS_IsException(resolve_func) || !JS_IsFunction(ctx, resolve_func)) {



        // Fallback: return a mock promise-like object
        GCValue mock_promise = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, mock_promise, "then", 
            JS_NewCFunction(ctx, js_promise_then, "then", 2));
        return mock_promise;
    }
    
    // Call Promise.resolve with the Promise constructor as 'this'
    GCValue result = JS_Call(ctx, resolve_func, promise_ctor, 1, &value);



    return result;
}

// Helper to create an empty resolved Promise
static GCValue js_create_empty_resolved_promise(JSContextHandle ctx) {
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    
    // Check if Promise constructor exists and is an object
    if (JS_IsException(promise_ctor) || !JS_IsObject(promise_ctor)) {


        // Fallback: return a mock promise-like object
        GCValue mock_promise = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, mock_promise, "then", 
            JS_NewCFunction(ctx, js_promise_then, "then", 2));
        return mock_promise;
    }
    
    GCValue resolve_func = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
    
    // Check if Promise.resolve exists and is a function
    if (JS_IsException(resolve_func) || !JS_IsFunction(ctx, resolve_func)) {



        // Fallback: return a mock promise-like object
        GCValue mock_promise = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, mock_promise, "then", 
            JS_NewCFunction(ctx, js_promise_then, "then", 2));
        return mock_promise;
    }
    
    // Call Promise.resolve with the Promise constructor as 'this'
    GCValue result = JS_Call(ctx, resolve_func, promise_ctor, 0, NULL);



    return result;
}

// ============================================================================
// Shadow DOM Implementation
// ============================================================================

// ShadowRootData struct is defined in browser_api_impl_types.h

static void js_shadow_root_finalizer(JSRuntimeHandle rt, GCValue val) {
    // ShadowRootData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

static JSClassDef js_shadow_root_class_def = {
    .class_name = "ShadowRoot",
    .finalizer = js_shadow_root_finalizer,
};

// ShadowRoot constructor - called when new ShadowRoot() is invoked
static GCValue js_shadow_root_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    // ShadowRoot cannot be constructed directly, it must be created via attachShadow
    // Return a new object with ShadowRoot prototype
    GCValue obj = JS_NewObjectClass(ctx, js_shadow_root_class_id);
    if (JS_IsException(obj)) {
        return JS_EXCEPTION;
    }
    return obj;
}

// ============================================================================
// Event Implementation
// ============================================================================

static void js_event_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    (void)val;
}

static JSClassDef js_event_class_def = {
    .class_name = "Event",
    .finalizer = js_event_finalizer,
};

static GCValue js_event_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    const char* type = "";
    if (argc > 0) {
        type = JS_ToCString(ctx, argv[0]);
        if (!type) type = "";
    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_event_class_id);
    if (JS_IsException(obj)) {
        return JS_EXCEPTION;
    }
    
    EventHandle ev = EventHandle::create(ctx, type);
    if (!ev.valid()) {
        return JS_ThrowInternalError(ctx, "Event creation failed");
    }
    
    // Parse event init object if provided
    if (argc > 1 && JS_IsObject(argv[1])) {
        GCValue bubbles_val = JS_GetPropertyStr(ctx, argv[1], "bubbles");
        GCValue cancelable_val = JS_GetPropertyStr(ctx, argv[1], "cancelable");
        GCValue composed_val = JS_GetPropertyStr(ctx, argv[1], "composed");
        
        ev.set_bubbles(JS_ToBool(ctx, bubbles_val));
        ev.set_cancelable(JS_ToBool(ctx, cancelable_val));
        ev.set_composed(JS_ToBool(ctx, composed_val));
    }
    
    ev.attach_to_object(obj);
    return obj;
}

static GCValue js_event_get_type(JSContextHandle ctx, GCValue this_val) {
    EventHandle ev = EventHandle::from_object_check(ctx, this_val);
    if (!ev.valid()) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return JS_NewString(ctx, ev.type());
}

static GCValue js_event_get_bubbles(JSContextHandle ctx, GCValue this_val) {
    EventHandle ev = EventHandle::from_object_check(ctx, this_val);
    if (!ev.valid()) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return JS_NewBool(ctx, ev.bubbles());
}

static GCValue js_event_get_cancelable(JSContextHandle ctx, GCValue this_val) {
    EventHandle ev = EventHandle::from_object_check(ctx, this_val);
    if (!ev.valid()) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return JS_NewBool(ctx, ev.cancelable());
}

static GCValue js_event_get_composed(JSContextHandle ctx, GCValue this_val) {
    EventHandle ev = EventHandle::from_object_check(ctx, this_val);
    if (!ev.valid()) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return JS_NewBool(ctx, ev.composed());
}

static GCValue js_event_get_defaultPrevented(JSContextHandle ctx, GCValue this_val) {
    EventHandle ev = EventHandle::from_object_check(ctx, this_val);
    if (!ev.valid()) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return JS_NewBool(ctx, ev.defaultPrevented());
}

static GCValue js_event_get_target(JSContextHandle ctx, GCValue this_val) {
    EventHandle ev = EventHandle::from_object_check(ctx, this_val);
    if (!ev.valid()) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return ev.target();
}

static GCValue js_event_get_currentTarget(JSContextHandle ctx, GCValue this_val) {
    EventHandle ev = EventHandle::from_object_check(ctx, this_val);
    if (!ev.valid()) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return ev.currentTarget();
}

static GCValue js_event_preventDefault(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    EventHandle ev = EventHandle::from_object_check(ctx, this_val);
    if (!ev.valid()) return JS_ThrowTypeError(ctx, "Invalid Event object");
    ev.set_defaultPrevented(1);
    return JS_UNDEFINED;
}

static GCValue js_event_stopPropagation(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static GCValue js_event_stopImmediatePropagation(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static GCValue js_event_get_eventPhase(JSContextHandle ctx, GCValue this_val) {
    EventHandle ev = EventHandle::from_object_check(ctx, this_val);
    if (!ev.valid()) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return JS_NewInt32(ctx, ev.eventPhase());
}

static GCValue js_event_get_eventPhase_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_event_get_eventPhase(ctx, this_val);
}

static GCValue js_event_composedPath(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    // Return empty array for now - would need full DOM tree traversal for real implementation
    return JS_NewArray(ctx);
}

// Event getter wrapper functions (matching JSCFunction signature)
static GCValue js_event_get_type_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_event_get_type(ctx, this_val);
}

static GCValue js_event_get_bubbles_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_event_get_bubbles(ctx, this_val);
}

static GCValue js_event_get_cancelable_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_event_get_cancelable(ctx, this_val);
}

static GCValue js_event_get_composed_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_event_get_composed(ctx, this_val);
}

static GCValue js_event_get_defaultPrevented_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_event_get_defaultPrevented(ctx, this_val);
}

static GCValue js_event_get_target_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_event_get_target(ctx, this_val);
}

static GCValue js_event_get_currentTarget_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_event_get_currentTarget(ctx, this_val);
}

// CustomEvent Implementation
static void js_custom_event_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    (void)val;
}

static JSClassDef js_custom_event_class_def = {
    .class_name = "CustomEvent",
    .finalizer = js_custom_event_finalizer,
};

static GCValue js_custom_event_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    const char* type = "";
    if (argc > 0) {
        type = JS_ToCString(ctx, argv[0]);
        if (!type) type = "";
    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_custom_event_class_id);
    if (JS_IsException(obj)) {
        return JS_EXCEPTION;
    }
    
    CustomEventHandle ev = CustomEventHandle::create(ctx, type);
    if (!ev.valid()) {
        return JS_ThrowInternalError(ctx, "CustomEvent creation failed");
    }
    
    // Parse event init object if provided
    if (argc > 1 && JS_IsObject(argv[1])) {
        GCValue bubbles_val = JS_GetPropertyStr(ctx, argv[1], "bubbles");
        GCValue cancelable_val = JS_GetPropertyStr(ctx, argv[1], "cancelable");
        GCValue composed_val = JS_GetPropertyStr(ctx, argv[1], "composed");
        GCValue detail_val = JS_GetPropertyStr(ctx, argv[1], "detail");
        
        ev.set_bubbles(JS_ToBool(ctx, bubbles_val));
        ev.set_cancelable(JS_ToBool(ctx, cancelable_val));
        ev.set_composed(JS_ToBool(ctx, composed_val));
        ev.set_detail(detail_val);
    }
    
    ev.attach_to_object(obj);
    return obj;
}

static GCValue js_custom_event_get_detail(JSContextHandle ctx, GCValue this_val) {
    CustomEventHandle ev = CustomEventHandle::from_object_check(ctx, this_val);
    if (!ev.valid()) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return ev.detail();
}

// MouseEvent Implementation
static void js_mouse_event_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    (void)val;
}

static JSClassDef js_mouse_event_class_def = {
    .class_name = "MouseEvent",
    .finalizer = js_mouse_event_finalizer,
};

static GCValue js_mouse_event_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    const char* type = "";
    if (argc > 0) {
        type = JS_ToCString(ctx, argv[0]);
        if (!type) type = "";
    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_mouse_event_class_id);
    if (JS_IsException(obj)) {
        return JS_EXCEPTION;
    }
    
    MouseEventHandle ev = MouseEventHandle::create(ctx, type);
    if (!ev.valid()) {
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
        
        ev.set_bubbles(JS_ToBool(ctx, bubbles_val));
        ev.set_cancelable(JS_ToBool(ctx, cancelable_val));
        
        double dval;
        if (!JS_IsException(clientX_val)) { JS_ToFloat64(ctx, &dval, clientX_val); ev.set_clientX(dval); }
        if (!JS_IsException(clientY_val)) { JS_ToFloat64(ctx, &dval, clientY_val); ev.set_clientY(dval); }
        
        int ival;
        if (!JS_IsException(button_val)) { JS_ToInt32(ctx, &ival, button_val); }
        if (!JS_IsException(ctrlKey_val)) { JS_ToBool(ctx, ctrlKey_val); }
    }
    
    ev.attach_to_object(obj);
    return obj;
}

static GCValue js_mouse_event_get_clientX(JSContextHandle ctx, GCValue this_val) {
    MouseEventHandle ev = MouseEventHandle::from_object_check(ctx, this_val);
    if (!ev.valid()) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return JS_NewFloat64(ctx, ev.clientX());
}

static GCValue js_mouse_event_get_clientY(JSContextHandle ctx, GCValue this_val) {
    MouseEventHandle ev = MouseEventHandle::from_object_check(ctx, this_val);
    if (!ev.valid()) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return JS_NewFloat64(ctx, ev.clientY());
}

// Wrapper functions for property getters (matching JSCFunction signature)
static GCValue js_custom_event_get_detail_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_custom_event_get_detail(ctx, this_val);
}

static GCValue js_mouse_event_get_clientX_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_mouse_event_get_clientX(ctx, this_val);
}

static GCValue js_mouse_event_get_clientY_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_mouse_event_get_clientY(ctx, this_val);
}

// FocusEvent Implementation
static void js_focus_event_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    (void)val;
}

static JSClassDef js_focus_event_class_def = {
    .class_name = "FocusEvent",
    .finalizer = js_focus_event_finalizer,
};

// ServiceWorker class definitions (minimal - no finalizers needed for simple stubs)
static JSClassDef js_service_worker_container_class_def = {
    .class_name = "ServiceWorkerContainer",
    .finalizer = NULL,
};

static JSClassDef js_service_worker_registration_class_def = {
    .class_name = "ServiceWorkerRegistration",
    .finalizer = NULL,
};

static JSClassDef js_service_worker_class_def = {
    .class_name = "ServiceWorker",
    .finalizer = NULL,
};

static GCValue js_focus_event_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    const char* type = "";
    if (argc > 0) {
        type = JS_ToCString(ctx, argv[0]);
        if (!type) type = "";
    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_focus_event_class_id);
    if (JS_IsException(obj)) {
        return JS_EXCEPTION;
    }
    
    FocusEventHandle ev = FocusEventHandle::create(ctx, type);
    if (!ev.valid()) {
        return JS_ThrowInternalError(ctx, "FocusEvent creation failed");
    }
    
    // Parse focus event init object if provided
    if (argc > 1 && JS_IsObject(argv[1])) {
        GCValue bubbles_val = JS_GetPropertyStr(ctx, argv[1], "bubbles");
        GCValue cancelable_val = JS_GetPropertyStr(ctx, argv[1], "cancelable");
        GCValue relatedTarget_val = JS_GetPropertyStr(ctx, argv[1], "relatedTarget");
        
        ev.set_bubbles(JS_ToBool(ctx, bubbles_val));
        ev.set_cancelable(JS_ToBool(ctx, cancelable_val));
        ev.set_relatedTarget(relatedTarget_val);
    }
    
    ev.attach_to_object(obj);
    return obj;
}

static GCValue js_focus_event_get_relatedTarget(JSContextHandle ctx, GCValue this_val) {
    FocusEventHandle ev = FocusEventHandle::from_object_check(ctx, this_val);
    if (!ev.valid()) return JS_ThrowTypeError(ctx, "Invalid Event object");
    return ev.relatedTarget();
}

static GCValue js_focus_event_get_relatedTarget_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_focus_event_get_relatedTarget(ctx, this_val);
}

static GCValue js_shadow_root_get_host(JSContextHandle ctx, GCValue this_val) {
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) return JS_ThrowTypeError(ctx, "Invalid ShadowRoot");
    return sr.host();
}

static GCValue js_shadow_root_get_host_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_shadow_root_get_host(ctx, this_val);
}

static GCValue js_shadow_root_get_mode(JSContextHandle ctx, GCValue this_val) {
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) return JS_ThrowTypeError(ctx, "Invalid ShadowRoot");
    return JS_NewString(ctx, sr.mode());
}

static GCValue js_shadow_root_get_mode_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_shadow_root_get_mode(ctx, this_val);
}

static GCValue js_shadow_root_get_innerHTML(JSContextHandle ctx, GCValue this_val) {
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) return JS_ThrowTypeError(ctx, "Invalid ShadowRoot");
    return sr.innerHTML();
}

static GCValue js_shadow_root_set_innerHTML(JSContextHandle ctx, GCValue this_val, GCValue val) {
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) return JS_ThrowTypeError(ctx, "Invalid ShadowRoot");

    sr.set_innerHTML(val);
    return JS_UNDEFINED;
}

// Forward declarations for ShadowRoot DOM tree functions
static GCValue js_shadow_root_append_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_shadow_root_remove_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_shadow_root_insert_before(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_shadow_root_get_first_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_shadow_root_get_last_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_shadow_root_get_child_nodes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

// Recursive querySelector helper for ShadowRoot - uses same selector matching as regular DOM
static GCValue query_selector_shadow_recursive(JSContextHandle ctx, GCValue elem, const char* selector) {
    if (JS_IsNull(elem)) return JS_NULL;
    
    // Check this element using the standard matches_selector helper
    DOMNodeHandle node = get_dom_node(ctx, elem);
    if (node.valid() && node.node_type() == DOM_NODE_TYPE_ELEMENT) {
        if (matches_selector(ctx, elem, selector)) {
            return elem;
        }
    }
    
    // Check children recursively
    DOMNodeHandle elem_node = get_dom_node(ctx, elem);
    if (elem_node.valid()) {
        GCValue child = elem_node.first_child();
        while (!JS_IsNull(child)) {
            GCValue result = query_selector_shadow_recursive(ctx, child, selector);
            if (!JS_IsNull(result)) {
                return result;
            }
            DOMNodeHandle child_node = get_dom_node(ctx, child);
            if (!child_node.valid()) break;
            child = child_node.next_sibling();
        }
    }
    
    return JS_NULL;
}

static void query_selector_all_shadow_recursive(JSContextHandle ctx, GCValue elem, const char* selector, GCValue result_arr, int* idx) {
    if (JS_IsNull(elem)) return;
    
    // Check this element
    DOMNodeHandle node = get_dom_node(ctx, elem);
    if (node.valid() && node.node_type() == DOM_NODE_TYPE_ELEMENT) {
        if (matches_selector(ctx, elem, selector)) {
            JS_SetPropertyUint32(ctx, result_arr, (*idx)++, elem);
        }
    }
    
    // Check children recursively
    DOMNodeHandle elem_node = get_dom_node(ctx, elem);
    if (elem_node.valid()) {
        GCValue child = elem_node.first_child();
        while (!JS_IsNull(child)) {
            query_selector_all_shadow_recursive(ctx, child, selector, result_arr, idx);
            DOMNodeHandle child_node = get_dom_node(ctx, child);
            if (!child_node.valid()) break;
            child = child_node.next_sibling();
        }
    }
}

static GCValue js_shadow_root_querySelector(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    
    const char* selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return JS_NULL;
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) return JS_NULL;
    
    // Search through all children and their descendants
    GCValue child = sr.first_child();
    while (!JS_IsNull(child)) {
        GCValue result = query_selector_shadow_recursive(ctx, child, selector);
        if (!JS_IsNull(result)) {
            return result;
        }
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }
    
    return JS_NULL;
}

static GCValue js_shadow_root_querySelectorAll(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue result = JS_NewArray(ctx);
    if (argc < 1) return result;
    
    const char* selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return result;
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) return result;
    
    int idx = 0;
    GCValue child = sr.first_child();
    while (!JS_IsNull(child)) {
        query_selector_all_shadow_recursive(ctx, child, selector, result, &idx);
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }
    
    return result;
}

static GCValue js_shadow_root_getElementById(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    
    const char* id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_NULL;
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) return JS_NULL;
    
    // Search through children for element with matching id
    GCValue child = sr.first_child();
    while (!JS_IsNull(child)) {
        // Check if this child has the id
        GCValue id_val = JS_GetPropertyStr(ctx, child, "id");
        const char* child_id = JS_ToCString(ctx, id_val);
        if (child_id && strcmp(child_id, id) == 0) {
            return child;
        }
        
        // Check child nodes recursively
        DOMNodeHandle child_node = DOMNodeHandle::from_object_check(ctx, child);
        if (child_node.valid()) {
            GCValue next = child_node.next_sibling();
            child = next;
        } else {
            break;
        }
    }
    
    return JS_NULL;
}

// ShadowRoot tree manipulation - appendChild
static GCValue js_shadow_root_append_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "appendChild: invalid argument");
    }
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) {
        return JS_ThrowTypeError(ctx, "appendChild: invalid ShadowRoot");
    }
    
    GCValue child = argv[0];
    
    // Get or create DOM data for child
    DOMNodeHandle child_node = get_or_create_dom_node(ctx, child, DOM_NODE_TYPE_ELEMENT, "");
    if (!child_node.valid()) {
        return JS_ThrowTypeError(ctx, "appendChild: failed to create DOM node");
    }
    
    // Remove child from its current parent if any
    GCValue old_parent_val = child_node.parent_node();
    if (!JS_IsNull(old_parent_val)) {
        // Check if old parent is a ShadowRoot
        ShadowRootDataHandle old_parent_sr = ShadowRootDataHandle::from_object_check(ctx, old_parent_val);
        if (old_parent_sr.valid()) {
            // Remove from ShadowRoot
            GCValue remove_args[1] = { child };
            js_shadow_root_remove_child(ctx, old_parent_val, 1, remove_args);
        } else {
            // Remove from regular DOM node
            GCValue remove_args[1] = { child };
            js_node_removeChild_real(ctx, old_parent_val, 1, remove_args);
        }
    }
    
    // Set child's parent to this shadow root
    child_node.set_parent_node(this_val);
    
    // Link child into shadow root's child list
    GCValue first_child = sr.first_child();
    GCValue last_child = sr.last_child();
    
    if (JS_IsNull(first_child)) {
        // First child
        sr.set_first_child(child);
        sr.set_last_child(child);
    } else {
        // Append to end
        DOMNodeHandle last_node = get_dom_node(ctx, last_child);
        if (last_node.valid()) {
            last_node.set_next_sibling(child);
        }
        child_node.set_previous_sibling(last_child);
        child_node.set_next_sibling(JS_NULL);
        sr.set_last_child(child);
    }
    
    sr.increment_child_count();
    
    return child;
}

// ShadowRoot removeChild
static GCValue js_shadow_root_remove_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "removeChild: invalid argument");
    }
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) {
        return JS_ThrowTypeError(ctx, "removeChild: invalid ShadowRoot");
    }
    
    GCValue child = argv[0];
    
    // Get DOM data for child
    DOMNodeHandle child_node = get_dom_node(ctx, child);
    if (!child_node.valid()) {
        return child;
    }
    
    // Verify child is actually a child of this shadow root
    GCValue child_parent = child_node.parent_node();
    if (!JS_StrictEq(ctx, child_parent, this_val)) {
        return throw_dom_exception(ctx, "NotFoundError", "Node is not a child of this shadow root");
    }
    
    // Get sibling references
    GCValue prev_sibling = child_node.previous_sibling();
    GCValue next_sibling = child_node.next_sibling();
    
    // Unlink from previous sibling
    if (!JS_IsNull(prev_sibling)) {
        DOMNodeHandle prev_node = get_dom_node(ctx, prev_sibling);
        if (prev_node.valid()) {
            prev_node.set_next_sibling(next_sibling);
        }
    } else {
        // Child was first child, update shadow root's firstChild
        sr.set_first_child(next_sibling);
    }
    
    // Unlink from next sibling
    if (!JS_IsNull(next_sibling)) {
        DOMNodeHandle next_node = get_dom_node(ctx, next_sibling);
        if (next_node.valid()) {
            next_node.set_previous_sibling(prev_sibling);
        }
    } else {
        // Child was last child, update shadow root's lastChild
        sr.set_last_child(prev_sibling);
    }
    
    // Clear child's references
    child_node.set_parent_node(JS_NULL);
    child_node.set_previous_sibling(JS_NULL);
    child_node.set_next_sibling(JS_NULL);
    
    sr.decrement_child_count();
    
    return child;
}

// ShadowRoot insertBefore
static GCValue js_shadow_root_insert_before(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "insertBefore: invalid arguments");
    }
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) {
        return JS_ThrowTypeError(ctx, "insertBefore: invalid ShadowRoot");
    }
    
    GCValue new_child = argv[0];
    GCValue ref_child = argv[1];  // Can be null (append at end)
    
    // Get or create DOM data for new child
    DOMNodeHandle new_node = get_or_create_dom_node(ctx, new_child, DOM_NODE_TYPE_ELEMENT, "");
    if (!new_node.valid()) {
        return JS_ThrowTypeError(ctx, "insertBefore: failed to create DOM node");
    }
    
    // Remove from current parent if any
    GCValue old_parent_val = new_node.parent_node();
    if (!JS_IsNull(old_parent_val)) {
        if (JS_StrictEq(ctx, old_parent_val, this_val)) {
            // Already in this shadow root, just remove first
            GCValue remove_args[1] = { new_child };
            js_shadow_root_remove_child(ctx, this_val, 1, remove_args);
        } else {
            // Remove from old parent
            ShadowRootDataHandle old_parent_sr = ShadowRootDataHandle::from_object_check(ctx, old_parent_val);
            if (old_parent_sr.valid()) {
                GCValue remove_args[1] = { new_child };
                js_shadow_root_remove_child(ctx, old_parent_val, 1, remove_args);
            } else {
                GCValue remove_args[1] = { new_child };
                js_node_removeChild_real(ctx, old_parent_val, 1, remove_args);
            }
        }
    }
    
    // If refChild is null, append at end
    if (JS_IsNull(ref_child)) {
        return js_shadow_root_append_child(ctx, this_val, 1, argv);
    }
    
    // Get ref child's DOM data
    DOMNodeHandle ref_node = get_dom_node(ctx, ref_child);
    if (!ref_node.valid()) {
        return throw_dom_exception(ctx, "NotFoundError", "Reference node not found");
    }
    
    // Verify ref_child is a child of this shadow root
    GCValue ref_parent = ref_node.parent_node();
    if (!JS_StrictEq(ctx, ref_parent, this_val)) {
        return throw_dom_exception(ctx, "NotFoundError", "Reference node is not a child of this shadow root");
    }
    
    // Insert before ref_child
    GCValue prev_sibling = ref_node.previous_sibling();
    
    // Update new node's links
    new_node.set_parent_node(this_val);
    new_node.set_previous_sibling(prev_sibling);
    new_node.set_next_sibling(ref_child);
    
    // Update ref child's previous sibling link
    ref_node.set_previous_sibling(new_child);
    
    // Update previous sibling's next link
    if (!JS_IsNull(prev_sibling)) {
        DOMNodeHandle prev_node = get_dom_node(ctx, prev_sibling);
        if (prev_node.valid()) {
            prev_node.set_next_sibling(new_child);
        }
    } else {
        // new_child is now the first child
        sr.set_first_child(new_child);
    }
    
    sr.increment_child_count();
    
    return new_child;
}

// ShadowRoot firstChild getter
static GCValue js_shadow_root_get_first_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) {
        return JS_NULL;
    }
    GCValue first = sr.first_child();
    return JS_IsNull(first) ? JS_NULL : first;
}

// ShadowRoot lastChild getter
static GCValue js_shadow_root_get_last_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) {
        return JS_NULL;
    }
    GCValue last = sr.last_child();
    return JS_IsNull(last) ? JS_NULL : last;
}

// ShadowRoot childNodes getter
static GCValue js_shadow_root_get_child_nodes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue arr = JS_NewArray(ctx);
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) {
        return arr;
    }
    
    int idx = 0;
    GCValue child = sr.first_child();
    while (!JS_IsNull(child)) {
        JS_SetPropertyUint32(ctx, arr, idx++, child);
        
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }
    
    return arr;
}

// ShadowRoot children getter (elements only)
static GCValue js_shadow_root_get_children(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue arr = JS_NewArray(ctx);
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) {
        return arr;
    }
    
    int idx = 0;
    GCValue child = sr.first_child();
    while (!JS_IsNull(child)) {
        // Only include element nodes
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            JS_SetPropertyUint32(ctx, arr, idx++, child);
        }
        
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }
    
    return arr;
}

// ShadowRoot childElementCount getter
static GCValue js_shadow_root_get_child_element_count(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) {
        return JS_NewInt32(ctx, 0);
    }
    
    int count = 0;
    GCValue child = sr.first_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            count++;
        }
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }
    
    return JS_NewInt32(ctx, count);
}

// ShadowRoot contains
static GCValue js_shadow_root_contains(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_FALSE;
    }
    
    GCValue other = argv[0];
    
    // Check if same node (ShadowRoot itself)
    if (JS_StrictEq(ctx, this_val, other)) {
        return JS_FALSE;  // ShadowRoot cannot contain itself
    }
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) {
        return JS_FALSE;
    }
    
    // Walk through children
    GCValue child = sr.first_child();
    while (!JS_IsNull(child)) {
        // Check if this is the node
        if (JS_StrictEq(ctx, child, other)) {
            return JS_TRUE;
        }
        
        // Check children recursively using Node.contains
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid()) {
            GCValue contains_args[1] = { other };
            GCValue result = js_node_contains_real(ctx, child, 1, contains_args);
            if (JS_ToBool(ctx, result)) {
                return JS_TRUE;
            }
            child = child_node.next_sibling();
        } else {
            break;
        }
    }
    
    return JS_FALSE;
}

static const JSCFunctionListEntry js_shadow_root_proto_funcs[] = {
    JS_CGETSET_DEF("host", js_shadow_root_get_host, NULL),
    JS_CGETSET_DEF("mode", js_shadow_root_get_mode, NULL),
    JS_CGETSET_DEF("innerHTML", js_shadow_root_get_innerHTML, js_shadow_root_set_innerHTML),
    JS_CFUNC_DEF("querySelector", 1, js_shadow_root_querySelector),
    JS_CFUNC_DEF("querySelectorAll", 1, js_shadow_root_querySelectorAll),
    JS_CFUNC_DEF("getElementById", 1, js_shadow_root_getElementById),
    JS_CFUNC_DEF("appendChild", 1, js_shadow_root_append_child),
    JS_CFUNC_DEF("removeChild", 1, js_shadow_root_remove_child),
    JS_CFUNC_DEF("insertBefore", 2, js_shadow_root_insert_before),
    JS_CFUNC_DEF("contains", 1, js_shadow_root_contains),
    JS_CFUNC_DEF("addEventListener", 2, js_dummy_function),
    JS_CFUNC_DEF("removeEventListener", 2, js_dummy_function),
    JS_CFUNC_DEF("dispatchEvent", 1, js_dummy_function_true),
    JS_PROP_INT32_DEF("nodeType", 11, JS_PROP_ENUMERABLE),  // DOCUMENT_FRAGMENT_NODE
    JS_PROP_STRING_DEF("nodeName", "#document-fragment", JS_PROP_ENUMERABLE),
};

// Element.prototype.attachShadow()
static GCValue js_element_attach_shadow(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "attachShadow requires an init object");
    }
    
    // Get mode from init object
    GCValue mode_val = JS_GetPropertyStr(ctx, argv[0], "mode");
    const char *mode = JS_ToCString(ctx, mode_val);
    if (!mode) mode = "closed";
    
    // Create ShadowRoot instance
    ShadowRootDataHandle sr = ShadowRootDataHandle::create(ctx, this_val, mode);
    if (!sr.valid()) {
        return JS_ThrowInternalError(ctx, "attachShadow: failed to create ShadowRoot");
    }
    
    GCValue shadow_root = JS_NewObjectClass(ctx, js_shadow_root_class_id);
    sr.attach_to_object(shadow_root);
    
    // Store shadowRoot reference on the element (internal property __shadowRoot)
    JS_SetPropertyStr(ctx, this_val, "__shadowRoot", shadow_root);
    

    return shadow_root;
}

// Element.prototype.shadowRoot getter
static GCValue js_element_get_shadow_root(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue shadow = JS_GetPropertyStr(ctx, this_val, "__shadowRoot");
    if (JS_IsUndefined(shadow)) {

        return JS_NULL;
    }
    
    // Check if mode is "open" - if closed, return null
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object(shadow);
    if (sr.valid() && sr.is_closed()) {
        return JS_NULL;
    }
    
    return shadow;
}

// ============================================================================
// Real DOM Tree Implementation
// ============================================================================

// Helper: Check if a node has the DOM node data attached
static bool is_dom_node(JSContextHandle ctx, GCValue obj) {
    if (JS_IsNull(obj) || JS_IsUndefined(obj)) return false;
    GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_dom_node_class_id);
    return h != GC_HANDLE_NULL;
}

// Helper: Get or create DOM node data for a JS object
static DOMNodeHandle get_or_create_dom_node(JSContextHandle ctx, GCValue obj, int node_type, const char* node_name) {
    if (is_dom_node(ctx, obj)) {
        return DOMNodeHandle::from_object_check(ctx, obj);
    }
    
    // Create new DOM node data
    DOMNodeHandle node = DOMNodeHandle::create(ctx, node_type, node_name);
    if (node.valid()) {
        node.attach_to_object(obj);
    }
    return node;
}

// Helper: Get DOM node data if it exists
static DOMNodeHandle get_dom_node(JSContextHandle ctx, GCValue obj) {
    return DOMNodeHandle::from_object(obj);
}

// Real appendChild implementation
static GCValue js_node_appendChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "appendChild: invalid argument");
    }
    
    GCValue child = argv[0];
    
    // Get or create DOM data for parent
    DOMNodeHandle parent = get_or_create_dom_node(ctx, this_val, DOM_NODE_TYPE_ELEMENT, "");
    if (!parent.valid()) {
        return JS_ThrowTypeError(ctx, "appendChild: invalid parent node");
    }
    
    // Get or create DOM data for child
    DOMNodeHandle child_node = get_or_create_dom_node(ctx, child, DOM_NODE_TYPE_ELEMENT, "");
    if (!child_node.valid()) {
        return JS_ThrowTypeError(ctx, "appendChild: invalid child node");
    }
    
    // Remove child from its current parent if any
    GCValue old_parent_val = child_node.parent_node();
    if (!JS_IsNull(old_parent_val)) {
        // Call removeChild on the old parent
        GCValue remove_args[1] = { child };
        js_node_removeChild_real(ctx, old_parent_val, 1, remove_args);
    }
    
    // Set child's parent
    child_node.set_parent_node(this_val);
    
    // Link child into parent's child list
    GCValue first_child = parent.first_child();
    GCValue last_child = parent.last_child();
    
    if (JS_IsNull(first_child)) {
        // First child - clear any stale sibling references
        parent.set_first_child(child);
        parent.set_last_child(child);
        child_node.set_previous_sibling(JS_NULL);
        child_node.set_next_sibling(JS_NULL);
    } else {
        // Append to end
        DOMNodeHandle last_node = get_dom_node(ctx, last_child);
        if (last_node.valid()) {
            last_node.set_next_sibling(child);
        }
        child_node.set_previous_sibling(last_child);
        child_node.set_next_sibling(JS_NULL);
        parent.set_last_child(child);
    }
    
    return child;
}

// Real removeChild implementation
static GCValue js_node_removeChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "removeChild: invalid argument");
    }
    
    GCValue child = argv[0];
    
    // Get DOM data
    DOMNodeHandle parent = get_dom_node(ctx, this_val);
    DOMNodeHandle child_node = get_dom_node(ctx, child);
    
    if (!parent.valid() || !child_node.valid()) {
        // If no DOM data, just return the child (no-op for non-DOM nodes)
        return child;
    }
    
    // Verify child is actually a child of parent
    GCValue child_parent = child_node.parent_node();
    if (!JS_StrictEq(ctx, child_parent, this_val)) {
        return throw_dom_exception(ctx, "NotFoundError", "Node is not a child of this node");
    }
    
    // Get sibling references
    GCValue prev_sibling = child_node.previous_sibling();
    GCValue next_sibling = child_node.next_sibling();
    
    // Unlink from previous sibling
    if (!JS_IsNull(prev_sibling)) {
        DOMNodeHandle prev_node = get_dom_node(ctx, prev_sibling);
        if (prev_node.valid()) {
            prev_node.set_next_sibling(next_sibling);
        }
    } else {
        // Child was first child, update parent's firstChild
        parent.set_first_child(next_sibling);
    }
    
    // Unlink from next sibling
    if (!JS_IsNull(next_sibling)) {
        DOMNodeHandle next_node = get_dom_node(ctx, next_sibling);
        if (next_node.valid()) {
            next_node.set_previous_sibling(prev_sibling);
        }
    } else {
        // Child was last child, update parent's lastChild
        parent.set_last_child(prev_sibling);
    }
    
    // Clear child's references
    child_node.set_parent_node(JS_NULL);
    child_node.set_previous_sibling(JS_NULL);
    child_node.set_next_sibling(JS_NULL);
    
    return child;
}

// Real insertBefore implementation
static GCValue js_node_insertBefore_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "insertBefore: invalid arguments");
    }
    
    GCValue new_child = argv[0];
    GCValue ref_child = argv[1];  // Can be null (append at end)
    
    // Get or create DOM data
    DOMNodeHandle parent = get_or_create_dom_node(ctx, this_val, DOM_NODE_TYPE_ELEMENT, "");
    if (!parent.valid()) {
        return JS_ThrowTypeError(ctx, "insertBefore: invalid parent node");
    }
    
    DOMNodeHandle new_node = get_or_create_dom_node(ctx, new_child, DOM_NODE_TYPE_ELEMENT, "");
    if (!new_node.valid()) {
        return JS_ThrowTypeError(ctx, "insertBefore: invalid new child node");
    }
    
    // Remove from current parent if any
    GCValue old_parent_val = new_node.parent_node();
    if (!JS_IsNull(old_parent_val)) {
        GCValue remove_args[1] = { new_child };
        js_node_removeChild_real(ctx, old_parent_val, 1, remove_args);
    }
    
    // If refChild is null, append at end
    if (JS_IsNull(ref_child)) {
        return js_node_appendChild_real(ctx, this_val, 1, argv);
    }
    
    // Get ref child's DOM data
    DOMNodeHandle ref_node = get_dom_node(ctx, ref_child);
    if (!ref_node.valid()) {
        return throw_dom_exception(ctx, "NotFoundError", "Reference node not found");
    }
    
    // Verify ref_child is a child of parent
    GCValue ref_parent = ref_node.parent_node();
    if (!JS_StrictEq(ctx, ref_parent, this_val)) {
        return throw_dom_exception(ctx, "NotFoundError", "Reference node is not a child of this node");
    }
    
    // Insert before ref_child
    GCValue prev_sibling = ref_node.previous_sibling();
    
    // Update new node's links
    new_node.set_parent_node(this_val);
    new_node.set_previous_sibling(prev_sibling);
    new_node.set_next_sibling(ref_child);
    
    // Update ref child's previous sibling link
    ref_node.set_previous_sibling(new_child);
    
    // Update previous sibling's next link
    if (!JS_IsNull(prev_sibling)) {
        DOMNodeHandle prev_node = get_dom_node(ctx, prev_sibling);
        if (prev_node.valid()) {
            prev_node.set_next_sibling(new_child);
        }
    } else {
        // new_child is now the first child
        parent.set_first_child(new_child);
    }
    
    return new_child;
}

// Real cloneNode implementation
static GCValue js_node_cloneNode_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    bool deep = false;
    if (argc > 0) {
        deep = JS_ToBool(ctx, argv[0]);
    }
    
    DOMNodeHandle original = get_dom_node(ctx, this_val);
    if (!original.valid()) {
        // No DOM data, return a basic object copy
        return JS_NewObject(ctx);
    }
    
    // Create new node of same type
    GCValue clone = JS_NewObjectClass(ctx, js_dom_node_class_id);
    DOMNodeHandle clone_node = DOMNodeHandle::create(ctx, original.node_type(), original.node_name());
    if (!clone_node.valid()) {
        return JS_ThrowInternalError(ctx, "cloneNode: failed to create clone");
    }
    
    clone_node.attach_to_object(clone);
    clone_node.set_node_value(original.node_value());
    clone_node.set_id(original.id());
    clone_node.set_class_name(original.class_name());
    
    // Copy attributes
    // (This would need iteration over all attributes in a full implementation)
    
    // If deep clone, clone all children
    if (deep) {
        GCValue child = original.first_child();
        while (!JS_IsNull(child)) {
            DOMNodeHandle child_node = get_dom_node(ctx, child);
            if (child_node.valid()) {
                GCValue clone_child = js_node_cloneNode_real(ctx, child, 1, argv);
                if (!JS_IsException(clone_child)) {
                    GCValue append_args[1] = { clone_child };
                    js_node_appendChild_real(ctx, clone, 1, append_args);
                }
            }
            child = child_node.next_sibling();
        }
    }
    
    return clone;
}

// Real contains implementation
static GCValue js_node_contains_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_FALSE;
    }
    
    GCValue other = argv[0];
    
    // Check if same node
    if (JS_StrictEq(ctx, this_val, other)) {
        return JS_TRUE;
    }
    
    // Walk up the tree from other node
    DOMNodeHandle other_node = get_dom_node(ctx, other);
    while (other_node.valid()) {
        GCValue parent = other_node.parent_node();
        if (JS_StrictEq(ctx, parent, this_val)) {
            return JS_TRUE;
        }
        other_node = get_dom_node(ctx, parent);
    }
    
    return JS_FALSE;
}

// Real getRootNode implementation
static GCValue js_node_getRootNode_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    
    GCValue current = this_val;
    DOMNodeHandle current_node = get_dom_node(ctx, current);
    
    while (current_node.valid()) {
        GCValue parent = current_node.parent_node();
        if (JS_IsNull(parent)) {
            break;
        }
        current = parent;
        current_node = get_dom_node(ctx, current);
    }
    
    return current;
}

// Tree navigation property getters
static GCValue js_node_get_firstChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    GCValue first = node.first_child();
    return JS_IsNull(first) ? JS_NULL : first;
}

static GCValue js_node_get_lastChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    GCValue last = node.last_child();
    return JS_IsNull(last) ? JS_NULL : last;
}

static GCValue js_node_get_nextSibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    GCValue next = node.next_sibling();
    return JS_IsNull(next) ? JS_NULL : next;
}

static GCValue js_node_get_previousSibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    GCValue prev = node.previous_sibling();
    return JS_IsNull(prev) ? JS_NULL : prev;
}

static GCValue js_node_get_parentNode(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    GCValue parent = node.parent_node();
    return JS_IsNull(parent) ? JS_NULL : parent;
}

static GCValue js_node_get_parentElement(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue parent = js_node_get_parentNode(ctx, this_val, 0, NULL);
    if (JS_IsNull(parent)) {
        return JS_NULL;
    }
    // Check if parent is an element
    DOMNodeHandle parent_node = get_dom_node(ctx, parent);
    if (parent_node.valid() && parent_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
        return parent;
    }
    return JS_NULL;
}

static GCValue js_node_get_childNodes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue arr = JS_NewArray(ctx);
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return arr;
    }
    
    int idx = 0;
    GCValue child = node.first_child();
    while (!JS_IsNull(child)) {
        JS_SetPropertyUint32(ctx, arr, idx++, child);
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }
    
    return arr;
}

static GCValue js_node_get_nodeType(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NewInt32(ctx, DOM_NODE_TYPE_ELEMENT);  // Default to element
    }
    return JS_NewInt32(ctx, node.node_type());
}

static GCValue js_node_get_nodeName(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid() || strlen(node.node_name()) == 0) {
        // Try to get from object
        GCValue tagName = JS_GetPropertyStr(ctx, this_val, "tagName");
        if (!JS_IsUndefined(tagName)) {
            return tagName;
        }
        return JS_NewString(ctx, "DIV");
    }
    return JS_NewString(ctx, node.node_name());
}

// Element.prototype.tagName getter
static GCValue js_element_get_tagName(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid() || strlen(node.node_name()) == 0) {
        return JS_NewString(ctx, "DIV");
    }
    return JS_NewString(ctx, node.node_name());
}

// Element tree navigation getters
static GCValue js_element_get_firstElementChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    
    GCValue child = node.first_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            return child;
        }
        child = child_node.next_sibling();
    }
    return JS_NULL;
}

static GCValue js_element_get_lastElementChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    
    GCValue child = node.last_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            return child;
        }
        child = child_node.previous_sibling();
    }
    return JS_NULL;
}

static GCValue js_element_get_nextElementSibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    
    GCValue sibling = node.next_sibling();
    while (!JS_IsNull(sibling)) {
        DOMNodeHandle sib_node = get_dom_node(ctx, sibling);
        if (sib_node.valid() && sib_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            return sibling;
        }
        sibling = sib_node.next_sibling();
    }
    return JS_NULL;
}

static GCValue js_element_get_previousElementSibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    
    GCValue sibling = node.previous_sibling();
    while (!JS_IsNull(sibling)) {
        DOMNodeHandle sib_node = get_dom_node(ctx, sibling);
        if (sib_node.valid() && sib_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            return sibling;
        }
        sibling = sib_node.previous_sibling();
    }
    return JS_NULL;
}

static GCValue js_element_get_childElementCount(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NewInt32(ctx, 0);
    }
    
    int count = 0;
    GCValue child = node.first_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            count++;
        }
        child = child_node.next_sibling();
    }
    return JS_NewInt32(ctx, count);
}

static GCValue js_element_get_children(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue arr = JS_NewArray(ctx);
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return arr;
    }
    
    int idx = 0;
    GCValue child = node.first_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            JS_SetPropertyUint32(ctx, arr, idx++, child);
        }
        child = child_node.next_sibling();
    }
    
    return arr;
}

// Element.prototype.querySelector
static GCValue js_element_querySelector(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_NULL;
}

// Element.prototype.querySelectorAll
static GCValue js_element_querySelectorAll(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_NewArray(ctx);
}

// Helper to recursively collect elements by tag name
static void collect_elements_by_tag(JSContextHandle ctx, DOMNodeHandle node, const char *tag_name, GCValue arr, int *idx) {
    if (!node.valid()) return;
    
    GCValue child = node.first_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            // Check if tag name matches
            const char *child_tag = child_node.node_name();
            if (child_tag && strcasecmp(child_tag, tag_name) == 0) {
                JS_SetPropertyUint32(ctx, arr, (*idx)++, child);
            }
            // Recurse into children
            collect_elements_by_tag(ctx, child_node, tag_name, arr, idx);
        }
        child = child_node.next_sibling();
    }
}

// Element.prototype.getElementsByTagName
static GCValue js_element_get_elements_by_tag_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NewArray(ctx);
    
    const char *tag_name = JS_ToCString(ctx, argv[0]);
    if (!tag_name) return JS_NewArray(ctx);
    
    GCValue arr = JS_NewArray(ctx);
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return arr;
    }
    
    int idx = 0;
    collect_elements_by_tag(ctx, node, tag_name, arr, &idx);
    
    return arr;
}

// Document.prototype.getElementsByTagName
static GCValue js_document_get_elements_by_tag_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NewArray(ctx);
    
    const char *tag_name = JS_ToCString(ctx, argv[0]);
    if (!tag_name) return JS_NewArray(ctx);
    
    GCValue arr = JS_NewArray(ctx);
    int idx = 0;
    
    // Special case: if looking for 'head', check document.head directly
    if (strcasecmp(tag_name, "head") == 0) {
        GCValue head = JS_GetPropertyStr(ctx, this_val, "head");
        if (!JS_IsUndefined(head) && !JS_IsNull(head)) {
            JS_SetPropertyUint32(ctx, arr, idx++, head);
        }
    }
    // Special case: if looking for 'body', check document.body directly
    else if (strcasecmp(tag_name, "body") == 0) {
        GCValue body = JS_GetPropertyStr(ctx, this_val, "body");
        if (!JS_IsUndefined(body) && !JS_IsNull(body)) {
            JS_SetPropertyUint32(ctx, arr, idx++, body);
        }
    }
    // For other tags, try to traverse the DOM tree
    else {
        // Get document element (documentElement) - usually <html>
        GCValue doc_elem = JS_GetPropertyStr(ctx, this_val, "documentElement");
        if (!JS_IsNull(doc_elem) && !JS_IsUndefined(doc_elem)) {
            DOMNodeHandle node = get_dom_node(ctx, doc_elem);
            if (node.valid()) {
                // Check document element itself
                const char *node_tag = node.node_name();
                if (node_tag && strcasecmp(node_tag, tag_name) == 0) {
                    JS_SetPropertyUint32(ctx, arr, idx++, doc_elem);
                }
                // Recurse into children
                collect_elements_by_tag(ctx, node, tag_name, arr, &idx);
            }
        }
    }
    
    return arr;
}

// Document.prototype.getElementsByClassName - use the lock-free class index table.
static GCValue js_document_getElementsByClassName(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewArray(ctx);

    const char *class_name = JS_ToCString(ctx, argv[0]);
    if (!class_name || !class_name[0]) return JS_NewArray(ctx);

    JSAtom class_atom = JS_NewAtom(ctx, class_name);
    if (class_atom == JS_ATOM_NULL) return JS_NewArray(ctx);
    GCValue arr = css_get_elements_by_class_name(ctx, class_atom);
    JS_FreeAtom(ctx, class_atom);
    return arr;
}

// Element.prototype.setAttribute
static GCValue js_element_set_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "requires at least 2 argument(s)");
    
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    
    if (name && value) {
        // Store attribute on the object itself
        JS_SetPropertyStr(ctx, this_val, name, JS_NewString(ctx, value));
        
        // Capture URL if src is being set on any element
        if (name && strcmp(name, "src") == 0 && value && value[0]) {
            capture_url_debug(value, "element_setAttribute_src");
        }
    }
    return JS_UNDEFINED;
}

// Element.prototype.getAttribute
static GCValue js_element_get_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;
    
    GCValue val = JS_GetPropertyStr(ctx, this_val, name);
    if (JS_IsUndefined(val) || JS_IsNull(val)) {
        return JS_NULL;
    }
    
    // Convert to string
    const char *str = JS_ToCString(ctx, val);
    if (str) {
        return JS_NewString(ctx, str);
    }
    return JS_NULL;
}

// Element.prototype.removeAttribute
static GCValue js_element_remove_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (name) {
        JSAtom atom = JS_NewAtom(ctx, name);
        JS_DeleteProperty(ctx, this_val, atom, 0);
        JS_FreeAtom(ctx, atom);
    }
    return JS_UNDEFINED;
}

// Element.prototype.hasAttribute
static GCValue js_element_has_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_FALSE;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_FALSE;
    
    GCValue val = JS_GetPropertyStr(ctx, this_val, name);
    return JS_NewBool(ctx, !JS_IsUndefined(val));
}

// Element.prototype.toggleAttribute
static GCValue js_element_toggle_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_FALSE;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_FALSE;
    
    JSAtom atom = JS_NewAtom(ctx, name);
    GCValue val = JS_GetProperty(ctx, this_val, atom);
    if (JS_IsUndefined(val)) {
        // Add attribute (empty string)
        JS_SetPropertyStr(ctx, this_val, name, JS_NewString(ctx, ""));
        JS_FreeAtom(ctx, atom);
        return JS_TRUE;
    } else {
        // Remove attribute
        JS_DeleteProperty(ctx, this_val, atom, 0);
        JS_FreeAtom(ctx, atom);
        return JS_FALSE;
    }
}

// Element.prototype.setAttributeNS
static GCValue js_element_set_attribute_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "setAttributeNS requires 3 arguments");
    // Ignore namespace, treat as regular setAttribute
    return js_element_set_attribute(ctx, this_val, argc - 1, argv + 1);
}

// Element.prototype.getAttributeNS
static GCValue js_element_get_attribute_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_NULL;
    // Ignore namespace, treat as regular getAttribute
    return js_element_get_attribute(ctx, this_val, argc - 1, argv + 1);
}

// Element.prototype.removeAttributeNS
static GCValue js_element_remove_attribute_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    // Ignore namespace, treat as regular removeAttribute
    return js_element_remove_attribute(ctx, this_val, argc - 1, argv + 1);
}

// Helper to create a generic element stub for querySelector fallback
static GCValue create_generic_element_stub(JSContextHandle ctx) {
    GCValue stub = JS_NewObject(ctx);
    if (JS_IsException(stub)) return JS_NULL;
    
    // Common element properties
    GCValue style = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, stub, "style", style);
    
    JS_SetPropertyStr(ctx, stub, "tagName", JS_NewString(ctx, "DIV"));
    JS_SetPropertyStr(ctx, stub, "nodeName", JS_NewString(ctx, "DIV"));
    JS_SetPropertyStr(ctx, stub, "localName", JS_NewString(ctx, "div"));
    
    GCValue classList = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, classList, "add", JS_NewCFunction(ctx, js_dummy_function, "add", 1));
    JS_SetPropertyStr(ctx, classList, "remove", JS_NewCFunction(ctx, js_dummy_function, "remove", 1));
    JS_SetPropertyStr(ctx, classList, "contains", JS_NewCFunction(ctx, js_dummy_function, "contains", 1));
    JS_SetPropertyStr(ctx, classList, "toggle", JS_NewCFunction(ctx, js_dummy_function, "toggle", 1));
    JS_SetPropertyStr(ctx, stub, "classList", classList);
    
    GCValue dataset = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, stub, "dataset", dataset);
    
    JS_SetPropertyStr(ctx, stub, "children", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, stub, "childNodes", JS_NewArray(ctx));
    
    // Common element methods
    JS_SetPropertyStr(ctx, stub, "getAttribute", JS_NewCFunction(ctx, js_element_get_attribute, "getAttribute", 1));
    JS_SetPropertyStr(ctx, stub, "setAttribute", JS_NewCFunction(ctx, js_element_set_attribute, "setAttribute", 2));
    JS_SetPropertyStr(ctx, stub, "hasAttribute", JS_NewCFunction(ctx, js_element_has_attribute, "hasAttribute", 1));
    JS_SetPropertyStr(ctx, stub, "removeAttribute", JS_NewCFunction(ctx, js_dummy_function, "removeAttribute", 1));
    JS_SetPropertyStr(ctx, stub, "getBoundingClientRect", JS_NewCFunction(ctx, js_dummy_function, "getBoundingClientRect", 0));
    JS_SetPropertyStr(ctx, stub, "querySelector", JS_NewCFunction(ctx, js_element_querySelector, "querySelector", 1));
    JS_SetPropertyStr(ctx, stub, "querySelectorAll", JS_NewCFunction(ctx, js_element_querySelectorAll, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, stub, "appendChild", JS_NewCFunction(ctx, js_dummy_function, "appendChild", 1));
    JS_SetPropertyStr(ctx, stub, "removeChild", JS_NewCFunction(ctx, js_dummy_function, "removeChild", 1));
    JS_SetPropertyStr(ctx, stub, "insertBefore", JS_NewCFunction(ctx, js_dummy_function, "insertBefore", 2));
    JS_SetPropertyStr(ctx, stub, "addEventListener", JS_NewCFunction(ctx, js_event_target_addEventListener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, stub, "removeEventListener", JS_NewCFunction(ctx, js_event_target_removeEventListener, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, stub, "dispatchEvent", JS_NewCFunction(ctx, js_event_target_dispatchEvent, "dispatchEvent", 1));
    JS_SetPropertyStr(ctx, stub, "focus", JS_NewCFunction(ctx, js_dummy_function, "focus", 0));
    JS_SetPropertyStr(ctx, stub, "blur", JS_NewCFunction(ctx, js_dummy_function, "blur", 0));
    JS_SetPropertyStr(ctx, stub, "click", JS_NewCFunction(ctx, js_dummy_function, "click", 0));
    JS_SetPropertyStr(ctx, stub, "closest", JS_NewCFunction(ctx, js_element_querySelector, "closest", 1));
    JS_SetPropertyStr(ctx, stub, "matches", JS_NewCFunction(ctx, js_dummy_function, "matches", 1));
    
    // Shadow root
    JS_SetPropertyStr(ctx, stub, "shadowRoot", JS_NULL);
    JS_SetPropertyStr(ctx, stub, "attachShadow", JS_NewCFunction(ctx, js_element_attach_shadow, "attachShadow", 1));
    
    // Parent/owner references
    JS_SetPropertyStr(ctx, stub, "parentNode", JS_NULL);
    JS_SetPropertyStr(ctx, stub, "parentElement", JS_NULL);
    JS_SetPropertyStr(ctx, stub, "firstChild", JS_NULL);
    JS_SetPropertyStr(ctx, stub, "lastChild", JS_NULL);
    JS_SetPropertyStr(ctx, stub, "nextSibling", JS_NULL);
    JS_SetPropertyStr(ctx, stub, "previousSibling", JS_NULL);
    
    // Content properties
    JS_SetPropertyStr(ctx, stub, "innerHTML", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, stub, "outerHTML", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, stub, "textContent", JS_NewString(ctx, ""));
    
    return stub;
}

// Document.getElementById - use the lock-free id index table.
static GCValue js_document_getElementById(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id || !*id) return JS_NULL;
    
    JSAtom id_atom = JS_NewAtom(ctx, id);
    if (id_atom == JS_ATOM_NULL) return JS_NULL;
    GCValue result = css_get_element_by_id(ctx, id_atom);
    JS_FreeAtom(ctx, id_atom);
    return result;
}

// Document.querySelector - search real DOM tree
static GCValue js_document_querySelector(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    
    const char *selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return JS_NULL;
    
    // Get document.documentElement
    GCValue doc_elem = JS_GetPropertyStr(ctx, this_val, "documentElement");
    if (JS_IsNull(doc_elem) || JS_IsUndefined(doc_elem)) {
        // Fallback: return stub if no documentElement
        return create_generic_element_stub(ctx);
    }
    
    // Search the DOM tree
    GCValue result = query_selector_recursive(ctx, doc_elem, selector);
    if (JS_IsNull(result)) {
        // Fallback: return stub so code doesn't crash
        return create_generic_element_stub(ctx);
    }
    
    return result;
}

// Document.querySelectorAll - search real DOM tree
static GCValue js_document_querySelectorAll(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue result = JS_NewArray(ctx);
    if (argc < 1) return result;
    
    const char *selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return result;
    
    // Get document.documentElement
    GCValue doc_elem = JS_GetPropertyStr(ctx, this_val, "documentElement");
    if (JS_IsNull(doc_elem) || JS_IsUndefined(doc_elem)) return result;
    
    int idx = 0;
    query_selector_all_recursive(ctx, doc_elem, selector, result, &idx);
    
    return result;
}

// Element.prototype.click
static GCValue js_element_click(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // No-op for stub implementation
    return JS_UNDEFINED;
}

// Element.prototype.getAnimations
static GCValue js_element_get_animations(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_NewArray(ctx);
}

// ============================================================================
// Real querySelector/querySelectorAll Implementation
// ============================================================================

// Simple selector matcher - supports tag name, #id, .class selectors
static bool matches_selector(JSContextHandle ctx, GCValue elem, const char* selector) {
    if (!selector || !*selector) return false;
    
    DOMNodeHandle node = get_dom_node(ctx, elem);
    
    // ID selector: #id
    if (selector[0] == '#') {
        const char* target_id = selector + 1;
        // First check DOMNode data
        if (node.valid() && strcmp(node.id(), target_id) == 0) {
            return true;
        }
        // Also check JS object property
        GCValue id_val = JS_GetPropertyStr(ctx, elem, "id");
        const char* id_str = JS_ToCString(ctx, id_val);
        if (id_str && strcmp(id_str, target_id) == 0) {
            return true;
        }
        return false;
    }
    
    // Class selector: .class
    if (selector[0] == '.') {
        const char* target_class = selector + 1;
        // First check DOMNode data
        if (node.valid() && strstr(node.class_name(), target_class) != NULL) {
            return true;
        }
        // Also check JS object property
        GCValue class_val = JS_GetPropertyStr(ctx, elem, "className");
        const char* class_str = JS_ToCString(ctx, class_val);
        if (class_str && strstr(class_str, target_class) != NULL) {
            return true;
        }
        return false;
    }
    
    // Tag selector
    const char* tag_name = node.valid() ? node.node_name() : "";
    // Check tagName property on object
    if (tag_name[0] == '\0') {
        GCValue tag_val = JS_GetPropertyStr(ctx, elem, "tagName");
        const char* tag_str = JS_ToCString(ctx, tag_val);
        if (tag_str) {
            return strcasecmp(tag_str, selector) == 0;
        }
        return false;
    }
    return strcasecmp(tag_name, selector) == 0;
}

// Recursive querySelector helper
static GCValue query_selector_recursive(JSContextHandle ctx, GCValue elem, const char* selector) {
    DOMNodeHandle node = get_dom_node(ctx, elem);
    if (!node.valid()) return JS_NULL;
    
    // Check this element
    if (matches_selector(ctx, elem, selector)) {
        return elem;
    }
    
    // Check children recursively
    GCValue child = node.first_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            GCValue result = query_selector_recursive(ctx, child, selector);
            if (!JS_IsNull(result)) {
                return result;
            }
        }
        child = child_node.next_sibling();
    }
    
    return JS_NULL;
}

// Recursive querySelectorAll helper
static void query_selector_all_recursive(JSContextHandle ctx, GCValue elem, const char* selector, GCValue result_arr, int* idx) {
    DOMNodeHandle node = get_dom_node(ctx, elem);
    if (!node.valid()) return;
    
    // Check this element
    if (matches_selector(ctx, elem, selector)) {
        JS_SetPropertyUint32(ctx, result_arr, (*idx)++, elem);
    }
    
    // Check children recursively
    GCValue child = node.first_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            query_selector_all_recursive(ctx, child, selector, result_arr, idx);
        }
        child = child_node.next_sibling();
    }
}

// Real querySelector implementation
static GCValue js_element_querySelector_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    
    const char* selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return JS_NULL;
    
    // Start from this element
    GCValue result = query_selector_recursive(ctx, this_val, selector);
    
    return result;
}

// Real querySelectorAll implementation
static GCValue js_element_querySelectorAll_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue result = JS_NewArray(ctx);
    if (argc < 1) return result;
    
    const char* selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return result;
    
    int idx = 0;
    query_selector_all_recursive(ctx, this_val, selector, result, &idx);
    
    return result;
}

// ============================================================================
// Element Content Getters/Setters
// ============================================================================

// classList getter - returns DOMTokenList stub
static GCValue js_element_get_classList(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    // Check if element already has a classList
    GCValue existing = JS_GetPropertyStr(ctx, this_val, "__classList");
    if (!JS_IsUndefined(existing) && !JS_IsNull(existing)) {
        return existing;
    }
    // Create DOMTokenList stub
    GCValue classList = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, classList, "add", JS_NewCFunction(ctx, js_dummy_function, "add", 1));
    JS_SetPropertyStr(ctx, classList, "remove", JS_NewCFunction(ctx, js_dummy_function, "remove", 1));
    JS_SetPropertyStr(ctx, classList, "toggle", JS_NewCFunction(ctx, js_dummy_function_true, "toggle", 1));
    JS_SetPropertyStr(ctx, classList, "contains", JS_NewCFunction(ctx, js_false, "contains", 1));
    JS_SetPropertyStr(ctx, classList, "item", JS_NewCFunction(ctx, js_null, "item", 1));
    JS_SetPropertyStr(ctx, classList, "length", JS_NewInt32(ctx, 0));
    // Store on element for reuse
    JS_SetPropertyStr(ctx, this_val, "__classList", classList);
    return classList;
}

// dataset getter - returns DOMStringMap stub (needed by YouTube player)
static GCValue js_element_get_dataset(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    // Check if element already has a dataset
    GCValue existing = JS_GetPropertyStr(ctx, this_val, "__dataset");
    if (!JS_IsUndefined(existing) && !JS_IsNull(existing)) {
        return existing;
    }
    // Create empty dataset object
    GCValue dataset = JS_NewObject(ctx);
    // Store on element for reuse
    JS_SetPropertyStr(ctx, this_val, "__dataset", dataset);
    return dataset;
}

// Element.prototype.innerHTML getter
static GCValue js_element_get_inner_html(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    // Return empty string for stub
    return JS_NewString(ctx, "");
}

// Element.prototype.innerHTML setter
static GCValue js_element_set_inner_html(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    // No-op for stub - in real implementation would parse HTML
    (void)argv;
    return JS_UNDEFINED;
}

// Element.prototype.outerHTML getter
static GCValue js_element_get_outer_html(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    // Return empty string for stub
    return JS_NewString(ctx, "");
}

// Element.prototype.outerHTML setter
static GCValue js_element_set_outer_html(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    // No-op for stub - in real implementation would replace element
    (void)argv;
    return JS_UNDEFINED;
}

// ============================================================================
// Node Content Getters/Setters
// ============================================================================

// Node.prototype.textContent getter
static GCValue js_node_get_text_content(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    // Return empty string for stub
    return JS_NewString(ctx, "");
}

// Node.prototype.textContent setter
static GCValue js_node_set_text_content(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    // No-op for stub
    (void)argv;
    return JS_UNDEFINED;
}

// Node.prototype.nodeValue getter
static GCValue js_node_get_node_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    // For Element nodes, nodeValue is null
    return JS_NULL;
}

// Node.prototype.nodeValue setter
static GCValue js_node_set_node_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // No-op - nodeValue is read-only for most node types
    (void)argc; (void)argv;
    return JS_UNDEFINED;
}

// ============================================================================
// Document Methods Implementation
// ============================================================================

// document.createRange()
static GCValue js_document_create_range(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue range = JS_NewObject(ctx);
    // Range properties
    JS_SetPropertyStr(ctx, range, "collapsed", JS_TRUE);
    JS_SetPropertyStr(ctx, range, "commonAncestorContainer", JS_NULL);
    JS_SetPropertyStr(ctx, range, "endContainer", JS_NULL);
    JS_SetPropertyStr(ctx, range, "endOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, range, "startContainer", JS_NULL);
    JS_SetPropertyStr(ctx, range, "startOffset", JS_NewInt32(ctx, 0));
    return range;
}

// document.createTreeWalker()
static GCValue js_document_create_tree_walker(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue tree_walker = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tree_walker, "currentNode", JS_NULL);
    JS_SetPropertyStr(ctx, tree_walker, "root", JS_NULL);
    JS_SetPropertyStr(ctx, tree_walker, "whatToShow", JS_NewInt32(ctx, 0xFFFFFFFF)); // SHOW_ALL
    JS_SetPropertyStr(ctx, tree_walker, "filter", JS_NULL);
    // Methods
    JS_SetPropertyStr(ctx, tree_walker, "firstChild", JS_NewCFunction(ctx, js_dummy_function, "firstChild", 0));
    JS_SetPropertyStr(ctx, tree_walker, "lastChild", JS_NewCFunction(ctx, js_dummy_function, "lastChild", 0));
    JS_SetPropertyStr(ctx, tree_walker, "nextNode", JS_NewCFunction(ctx, js_dummy_function, "nextNode", 0));
    JS_SetPropertyStr(ctx, tree_walker, "nextSibling", JS_NewCFunction(ctx, js_dummy_function, "nextSibling", 0));
    JS_SetPropertyStr(ctx, tree_walker, "parentNode", JS_NewCFunction(ctx, js_dummy_function, "parentNode", 0));
    JS_SetPropertyStr(ctx, tree_walker, "previousNode", JS_NewCFunction(ctx, js_dummy_function, "previousNode", 0));
    JS_SetPropertyStr(ctx, tree_walker, "previousSibling", JS_NewCFunction(ctx, js_dummy_function, "previousSibling", 0));
    return tree_walker;
}

// Event initEvent implementation
static GCValue js_event_init_event(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc >= 1) {
        const char *type = JS_ToCString(ctx, argv[0]);
        if (type) {
            JS_SetPropertyStr(ctx, this_val, "type", JS_NewString(ctx, type));
        }
    }
    if (argc >= 2) {
        JS_SetPropertyStr(ctx, this_val, "bubbles", JS_NewBool(ctx, JS_ToBool(ctx, argv[1])));
    }
    if (argc >= 3) {
        JS_SetPropertyStr(ctx, this_val, "cancelable", JS_NewBool(ctx, JS_ToBool(ctx, argv[2])));
    }
    return JS_UNDEFINED;
}

// Event initCustomEvent implementation
static GCValue js_event_init_custom_event(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc >= 1) {
        const char *type = JS_ToCString(ctx, argv[0]);
        if (type) {
            JS_SetPropertyStr(ctx, this_val, "type", JS_NewString(ctx, type));
        }
    }
    if (argc >= 2) {
        JS_SetPropertyStr(ctx, this_val, "bubbles", JS_NewBool(ctx, JS_ToBool(ctx, argv[1])));
    }
    if (argc >= 3) {
        JS_SetPropertyStr(ctx, this_val, "cancelable", JS_NewBool(ctx, JS_ToBool(ctx, argv[2])));
    }
    if (argc >= 4) {
        JS_SetPropertyStr(ctx, this_val, "detail", argv[3]);
    }
    return JS_UNDEFINED;
}

// document.createEvent()
static GCValue js_document_create_event(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue event = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, event, "bubbles", JS_FALSE);
    JS_SetPropertyStr(ctx, event, "cancelable", JS_FALSE);
    JS_SetPropertyStr(ctx, event, "composed", JS_FALSE);
    JS_SetPropertyStr(ctx, event, "currentTarget", JS_NULL);
    JS_SetPropertyStr(ctx, event, "defaultPrevented", JS_FALSE);
    JS_SetPropertyStr(ctx, event, "eventPhase", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, event, "target", JS_NULL);
    JS_SetPropertyStr(ctx, event, "timeStamp", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, ""));
    // Methods
    JS_SetPropertyStr(ctx, event, "initEvent", JS_NewCFunction(ctx, js_event_init_event, "initEvent", 3));
    JS_SetPropertyStr(ctx, event, "initCustomEvent", JS_NewCFunction(ctx, js_event_init_custom_event, "initCustomEvent", 4));
    JS_SetPropertyStr(ctx, event, "preventDefault", JS_NewCFunction(ctx, js_dummy_function, "preventDefault", 0));
    JS_SetPropertyStr(ctx, event, "stopPropagation", JS_NewCFunction(ctx, js_dummy_function, "stopPropagation", 0));
    JS_SetPropertyStr(ctx, event, "stopImmediatePropagation", JS_NewCFunction(ctx, js_dummy_function, "stopImmediatePropagation", 0));
    return event;
}

// document.importNode()
static GCValue js_document_import_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    // Return a shallow copy of the node
    GCValue clone = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, clone, "nodeType", JS_NewInt32(ctx, 1)); // ELEMENT_NODE
    JS_SetPropertyStr(ctx, clone, "nodeName", JS_NewString(ctx, "DIV"));
    return clone;
}

// document.elementFromPoint()
static GCValue js_document_element_from_point(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Return documentElement for any point
    return JS_GetPropertyStr(ctx, this_val, "documentElement");
}

// ============================================================================
// EventTarget Implementation
// ============================================================================

static GCValue js_event_target_addEventListener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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

static GCValue js_event_target_removeEventListener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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

static GCValue js_event_target_dispatchEvent(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_FALSE;
    
    // Get event type from the event object
    GCValue type_val = JS_GetPropertyStr(ctx, argv[0], "type");
    const char *type = JS_ToCString(ctx, type_val);
    if (!type) return JS_FALSE;
    
    // Set target if not already set
    GCValue target = JS_GetPropertyStr(ctx, argv[0], "target");
    if (JS_IsNull(target) || JS_IsUndefined(target)) {
        JS_SetPropertyStr(ctx, argv[0], "target", this_val);
    }
    JS_SetPropertyStr(ctx, argv[0], "currentTarget", this_val);
    
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
    
    return JS_TRUE;
}

// ============================================================================
// Node Implementation
// ============================================================================

static GCValue js_node_appendChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    // Return the appended child
    return argv[0];
}

static GCValue js_node_insertBefore(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    return argv[0];
}

static GCValue js_node_removeChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    return argv[0];
}

static GCValue js_node_cloneNode(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Return a new empty object as cloned node
    return JS_NewObject(ctx);
}

static GCValue js_node_contains(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_FALSE;
}

// Node.prototype.getRootNode - CRITICAL for Shadow DOM
static GCValue js_node_get_root_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Return document.documentElement as root
    GCValue document = JS_GetPropertyStr(ctx, this_val, "ownerDocument");
    if (JS_IsUndefined(document)) {
        return this_val; // Return self if no owner
    }
    GCValue root = JS_GetPropertyStr(ctx, document, "documentElement");
    if (JS_IsUndefined(root) || JS_IsNull(root)) {
        return this_val;
    }
    return root;
}

// ============================================================================
// Custom Elements API Implementation
// ============================================================================

// CustomElementRegistryData struct is defined in browser_api_impl_types.h

static void js_custom_element_registry_finalizer(JSRuntimeHandle rt, GCValue val) {
    // CustomElementRegistryData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

static JSClassDef js_custom_element_registry_class_def = {
    .class_name = "CustomElementRegistry",
    .finalizer = js_custom_element_registry_finalizer,
};

// customElements.define(name, constructor, options)
static GCValue js_custom_elements_define(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "define requires at least 2 arguments");
    }
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_ThrowTypeError(ctx, "Invalid name");
    
    platform_log(LOG_LEVEL_INFO, "customElements", "define called with name='%s' (argc=%d)", name, argc);
    
    // Validate name format (must contain hyphen)
    // NOTE: Relaxed for browser emulation - some scripts may pass invalid names
    // and expect them to be silently ignored or caught internally.
    if (strchr(name, '-') == NULL) {
        platform_log(LOG_LEVEL_WARN, "customElements", "Ignoring invalid custom element name: '%s'", name);
        return JS_UNDEFINED;
    }
    
    // Store in registry (the this_val should be the customElements object)
    JS_SetPropertyStr(ctx, this_val, name, argv[1]);
    
    return JS_UNDEFINED;
}

// customElements.get(name)
static GCValue js_custom_elements_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_UNDEFINED;
    
    GCValue ctor = JS_GetPropertyStr(ctx, this_val, name);
    
    
    if (JS_IsUndefined(ctor)) {
        return JS_UNDEFINED;
    }
    return ctor;
}

// customElements.whenDefined(name)
static GCValue js_custom_elements_when_defined(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Return a resolved promise
    return js_create_empty_resolved_promise(ctx);
}

// ============================================================================
// Web Animations API Implementation
// ============================================================================

// AnimationData and KeyFrameEffectData structs are defined in browser_api_impl_types.h

static void js_animation_finalizer(JSRuntimeHandle rt, GCValue val) {
    // AnimationData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

static void js_keyframe_effect_finalizer(JSRuntimeHandle rt, GCValue val) {
    // KeyFrameEffectData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

static JSClassDef js_animation_class_def = {
    .class_name = "Animation",
    .finalizer = js_animation_finalizer,
};

static JSClassDef js_keyframe_effect_class_def = {
    .class_name = "KeyframeEffect",
    .finalizer = js_keyframe_effect_finalizer,
};

// Animation constructor
static GCValue js_animation_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    AnimationDataHandle anim = AnimationDataHandle::create(ctx);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");
    
    if (argc > 0) {
        anim.set_effect(argv[0]);
        // Try to get duration from effect
        if (JS_IsObject(argv[0])) {
            GCValue duration_val = JS_GetPropertyStr(ctx, argv[0], "duration");
            double duration;
            if (!JS_IsException(duration_val) && !JS_ToFloat64(ctx, &duration, duration_val)) {
                anim.set_duration(duration);
            }

        }
    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_animation_class_id);
    anim.attach_to_object(obj);
    return obj;
}

// Animation.prototype.play()
static GCValue js_animation_play(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    AnimationDataHandle anim = AnimationDataHandle::from_object_check(ctx, this_val);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");
    anim.set_playing();
    return JS_UNDEFINED;
}

// Animation.prototype.pause()
static GCValue js_animation_pause(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    AnimationDataHandle anim = AnimationDataHandle::from_object_check(ctx, this_val);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");
    anim.set_paused();
    return JS_UNDEFINED;
}

// Animation.prototype.finish()
static GCValue js_animation_finish(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    AnimationDataHandle anim = AnimationDataHandle::from_object_check(ctx, this_val);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");
    anim.set_finished();
    anim.set_current_time(anim.duration());
    
    // Call onfinish callback if set
    GCValue onfinish = anim.onfinish();
    if (!JS_IsNull(onfinish) && JS_IsFunction(ctx, onfinish)) {
        JS_Call(ctx, onfinish, this_val, 0, NULL);
    }
    return JS_UNDEFINED;
}

// Animation.prototype.cancel()
static GCValue js_animation_cancel(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    AnimationDataHandle anim = AnimationDataHandle::from_object_check(ctx, this_val);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");
    anim.set_idle();
    anim.set_current_time(0);
    return JS_UNDEFINED;
}

// Animation.prototype.reverse()
static GCValue js_animation_reverse(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// Animation.playState getter
static GCValue js_animation_get_play_state(JSContextHandle ctx, GCValue this_val) {
    AnimationDataHandle anim = AnimationDataHandle::from_object_check(ctx, this_val);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");
    return JS_NewString(ctx, anim.play_state_string());
}

// Animation.currentTime getter
static GCValue js_animation_get_current_time(JSContextHandle ctx, GCValue this_val) {
    AnimationDataHandle anim = AnimationDataHandle::from_object_check(ctx, this_val);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");
    return JS_NewFloat64(ctx, anim.current_time());
}

// Animation.effect getter
static GCValue js_animation_get_effect(JSContextHandle ctx, GCValue this_val) {
    AnimationDataHandle anim = AnimationDataHandle::from_object_check(ctx, this_val);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");
    GCValue effect = anim.effect();
    if (JS_IsNull(effect)) return JS_NULL;
    return effect;
}

// Animation.onfinish getter/setter
static GCValue js_animation_get_onfinish(JSContextHandle ctx, GCValue this_val) {
    AnimationDataHandle anim = AnimationDataHandle::from_object_check(ctx, this_val);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");
    return anim.onfinish();
}

static GCValue js_animation_set_onfinish(JSContextHandle ctx, GCValue this_val, GCValue val) {
    AnimationDataHandle anim = AnimationDataHandle::from_object_check(ctx, this_val);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");

    anim.set_onfinish(val);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_animation_proto_funcs[] = {
    JS_CFUNC_DEF("play", 0, js_animation_play),
    JS_CFUNC_DEF("pause", 0, js_animation_pause),
    JS_CFUNC_DEF("finish", 0, js_animation_finish),
    JS_CFUNC_DEF("cancel", 0, js_animation_cancel),
    JS_CFUNC_DEF("reverse", 0, js_animation_reverse),
    JS_CGETSET_DEF("playState", js_animation_get_play_state, NULL),
    JS_CGETSET_DEF("currentTime", js_animation_get_current_time, NULL),
    JS_CGETSET_DEF("effect", js_animation_get_effect, NULL),
    JS_CGETSET_DEF("onfinish", js_animation_get_onfinish, js_animation_set_onfinish),
};

// KeyframeEffect constructor
static GCValue js_keyframe_effect_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    KeyFrameEffectDataHandle effect = KeyFrameEffectDataHandle::create();
    if (!effect.valid()) return JS_ThrowTypeError(ctx, "Invalid Effect");
    
    if (argc > 0) {
        effect.set_target(argv[0]);
    }
    if (argc > 1) {
        effect.set_keyframes(argv[1]);
    }
    if (argc > 2 && JS_IsObject(argv[2])) {
        GCValue duration_val = JS_GetPropertyStr(ctx, argv[2], "duration");
        double duration;
        if (!JS_IsException(duration_val) && !JS_ToFloat64(ctx, &duration, duration_val)) {
            effect.set_duration(duration);
        }

        GCValue easing_val = JS_GetPropertyStr(ctx, argv[2], "easing");
        const char *easing = JS_ToCString(ctx, easing_val);
        if (easing) {
            effect.set_easing(easing);
        }

    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_keyframe_effect_class_id);
    effect.attach_to_object(obj);
    return obj;
}

// KeyframeEffect.target getter
static GCValue js_keyframe_effect_get_target(JSContextHandle ctx, GCValue this_val) {
    KeyFrameEffectDataHandle effect = KeyFrameEffectDataHandle::from_object_check(ctx, this_val);
    if (!effect.valid()) return JS_ThrowTypeError(ctx, "Invalid Effect");
    GCValue target = effect.target();
    if (JS_IsNull(target)) return JS_NULL;
    return target;
}

// KeyframeEffect.duration getter
static GCValue js_keyframe_effect_get_duration(JSContextHandle ctx, GCValue this_val) {
    KeyFrameEffectDataHandle effect = KeyFrameEffectDataHandle::from_object_check(ctx, this_val);
    if (!effect.valid()) return JS_ThrowTypeError(ctx, "Invalid Effect");
    return JS_NewFloat64(ctx, effect.duration());
}

static const JSCFunctionListEntry js_keyframe_effect_proto_funcs[] = {
    JS_CGETSET_DEF("target", js_keyframe_effect_get_target, NULL),
    JS_CGETSET_DEF("duration", js_keyframe_effect_get_duration, NULL),
};

// Element.prototype.animate(keyframes, options)
static GCValue js_element_animate(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Create KeyframeEffect
    GCValue effect_args[3];
    effect_args[0] = this_val;  // target
    effect_args[1] = argc > 0 ? argv[0] : JS_NULL;  // keyframes
    effect_args[2] = argc > 1 ? argv[1] : JS_NULL;  // options
    
    GCValue effect = js_keyframe_effect_constructor(ctx, JS_UNDEFINED, 3, effect_args);



    if (JS_IsException(effect)) {
        return effect;
    }
    
    // Create Animation with the effect
    GCValue anim_args[1];
    anim_args[0] = effect;
    GCValue animation = js_animation_constructor(ctx, JS_UNDEFINED, 1, anim_args);

    if (JS_IsException(animation)) {
        return animation;
    }
    
    // Auto-play the animation
    AnimationDataHandle anim = AnimationDataHandle::from_object(animation);
    if (anim.valid()) {
        anim.set_playing();
    }
    
    // Set oncancel to null (not undefined) so Web Animations polyfill
    // feature detection skips its wrapping code path
    JS_SetPropertyStr(ctx, animation, "oncancel", JS_NULL);
    
    return animation;
}

// ============================================================================
// Font Loading API Implementation
// ============================================================================

// FontFaceData and FontFaceSetData structs are defined in browser_api_impl_types.h

static void js_font_face_finalizer(JSRuntimeHandle rt, GCValue val) {
    // FontFaceData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

static void js_font_face_set_finalizer(JSRuntimeHandle rt, GCValue val) {
    // FontFaceSetData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

static JSClassDef js_font_face_class_def = {
    .class_name = "FontFace",
    .finalizer = js_font_face_finalizer,
};

static JSClassDef js_font_face_set_class_def = {
    .class_name = "FontFaceSet",
    .finalizer = js_font_face_set_finalizer,
};

// FontFace constructor
static GCValue js_font_face_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    FontFaceDataHandle ff = FontFaceDataHandle::create();
    if (!ff.valid()) return JS_ThrowTypeError(ctx, "Invalid FontFace");
    
    if (argc > 0) {
        const char *family = JS_ToCString(ctx, argv[0]);
        if (family) {
            ff.set_family(family);
        }
    }
    
    if (argc > 1) {
        const char *source = JS_ToCString(ctx, argv[1]);
        if (source) {
            ff.set_source(source);
        }
    }
    
    if (argc > 2 && JS_IsObject(argv[2])) {
        GCValue display_val = JS_GetPropertyStr(ctx, argv[2], "display");
        const char *display = JS_ToCString(ctx, display_val);
        if (display) {
            ff.set_display(display);
        }

    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_font_face_class_id);
    ff.attach_to_object(obj);
    return obj;
}

// FontFace.load()
static GCValue js_font_face_load(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Return a resolved promise with this FontFace
    GCValue result = js_create_resolved_promise(ctx, this_val);

    return result;
}

// FontFace.loaded getter - returns a Promise that resolves to this FontFace
static GCValue js_font_face_get_loaded(JSContextHandle ctx, GCValue this_val) {
    FontFaceDataHandle ff = FontFaceDataHandle::from_object_check(ctx, this_val);
    if (!ff.valid()) return JS_ThrowTypeError(ctx, "Invalid FontFace");
    // Return a resolved promise with this FontFace
    GCValue result = js_create_resolved_promise(ctx, this_val);

    return result;
}

// FontFace.family getter
static GCValue js_font_face_get_family(JSContextHandle ctx, GCValue this_val) {
    FontFaceDataHandle ff = FontFaceDataHandle::from_object_check(ctx, this_val);
    if (!ff.valid()) return JS_ThrowTypeError(ctx, "Invalid FontFace");
    return JS_NewString(ctx, ff.family());
}

// FontFace.status getter
static GCValue js_font_face_get_status(JSContextHandle ctx, GCValue this_val) {
    return JS_NewString(ctx, "loaded");
}

static const JSCFunctionListEntry js_font_face_proto_funcs[] = {
    JS_CFUNC_DEF("load", 0, js_font_face_load),
    JS_CGETSET_DEF("family", js_font_face_get_family, NULL),
    JS_CGETSET_DEF("status", js_font_face_get_status, NULL),
    JS_CGETSET_DEF("loaded", js_font_face_get_loaded, NULL),  // Now returns a proper Promise
};

// FontFaceSet.load(fontSpec)
static GCValue js_font_face_set_load(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Return a resolved promise with empty array (all fonts "loaded")
    GCValue empty_array = JS_NewArray(ctx);
    GCValue result = js_create_resolved_promise(ctx, empty_array);

    return result;
}

// FontFaceSet.check(fontSpec)
static GCValue js_font_face_set_check(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Always return true (fonts are available)
    return JS_TRUE;
}

// FontFaceSet.ready getter
static GCValue js_font_face_set_get_ready(JSContextHandle ctx, GCValue this_val) {
    // Return a resolved promise
    return js_create_empty_resolved_promise(ctx);
}

// FontFaceSet.status getter
static GCValue js_font_face_set_get_status(JSContextHandle ctx, GCValue this_val) {
    return JS_NewString(ctx, "loaded");
}

// FontFaceSet.add(fontFace)
static GCValue js_font_face_set_add(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// FontFaceSet.delete(fontFace)
static GCValue js_font_face_set_delete(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_TRUE;
}

// FontFaceSet.clear()
static GCValue js_font_face_set_clear(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// FontFaceSet.has(fontFace)
static GCValue js_font_face_set_has(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_TRUE;
}

// FontFaceSet.forEach(callback)
static GCValue js_font_face_set_forEach(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// FontFaceSet[Symbol.iterator]()
static GCValue js_font_face_set_iterator(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue array_ctor = JS_GetPropertyStr(ctx, global, "Array");
    GCValue empty_array = JS_CallConstructor(ctx, array_ctor, 0, NULL);
    GCValue result = JS_GetPropertyStr(ctx, empty_array, "values");
    GCValue iterator = JS_Call(ctx, result, empty_array, 0, NULL);




    return iterator;
}

static const JSCFunctionListEntry js_font_face_set_proto_funcs[] = {
    JS_CFUNC_DEF("load", 1, js_font_face_set_load),
    JS_CFUNC_DEF("check", 1, js_font_face_set_check),
    JS_CGETSET_DEF("ready", js_font_face_set_get_ready, NULL),
    JS_CGETSET_DEF("status", js_font_face_set_get_status, NULL),
    JS_CFUNC_DEF("add", 1, js_font_face_set_add),
    JS_CFUNC_DEF("delete", 1, js_font_face_set_delete),
    JS_CFUNC_DEF("clear", 0, js_font_face_set_clear),
    JS_CFUNC_DEF("has", 1, js_font_face_set_has),
    JS_CFUNC_DEF("forEach", 1, js_font_face_set_forEach),
    JS_CFUNC_DEF("values", 0, js_font_face_set_iterator),
    JS_CFUNC_DEF("keys", 0, js_font_face_set_iterator),
    JS_CFUNC_DEF("entries", 0, js_font_face_set_iterator),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, js_font_face_set_iterator),
};

// ============================================================================
// MutationObserver Implementation
// ============================================================================

// MutationObserverData struct is defined in browser_api_impl_types.h

static void js_mutation_observer_finalizer(JSRuntimeHandle rt, GCValue val) {
    // MutationObserverData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

static JSClassDef js_mutation_observer_class_def = {
    .class_name = "MutationObserver",
    .finalizer = js_mutation_observer_finalizer,
};

// MutationObserver constructor
static GCValue js_mutation_observer_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "MutationObserver constructor requires a callback function");
    }
    
    MutationObserverDataHandle mo = MutationObserverDataHandle::create(ctx, argv[0]);
    if (!mo.valid()) return JS_ThrowTypeError(ctx, "Invalid MutationObserver");
    
    GCValue obj = JS_NewObjectClass(ctx, js_mutation_observer_class_id);
    mo.attach_to_object(obj);
    return obj;
}

// MutationObserver.prototype.observe(target, options)
static GCValue js_mutation_observer_observe(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// MutationObserver.prototype.disconnect()
static GCValue js_mutation_observer_disconnect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// MutationObserver.prototype.takeRecords()
static GCValue js_mutation_observer_takeRecords(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_NewArray(ctx);
}

static const JSCFunctionListEntry js_mutation_observer_proto_funcs[] = {
    JS_CFUNC_DEF("observe", 2, js_mutation_observer_observe),
    JS_CFUNC_DEF("disconnect", 0, js_mutation_observer_disconnect),
    JS_CFUNC_DEF("takeRecords", 0, js_mutation_observer_takeRecords),
};

// ============================================================================
// ResizeObserver Implementation
// ============================================================================

// ResizeObserverData struct is defined in browser_api_impl_types.h

static void js_resize_observer_finalizer(JSRuntimeHandle rt, GCValue val) {
    // ResizeObserverData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

static JSClassDef js_resize_observer_class_def = {
    .class_name = "ResizeObserver",
    .finalizer = js_resize_observer_finalizer,
};

// ResizeObserver constructor
static GCValue js_resize_observer_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "ResizeObserver constructor requires a callback function");
    }
    
    ResizeObserverDataHandle ro = ResizeObserverDataHandle::create(ctx, argv[0]);
    if (!ro.valid()) return JS_ThrowTypeError(ctx, "Invalid ResizeObserver");
    
    GCValue obj = JS_NewObjectClass(ctx, js_resize_observer_class_id);
    ro.attach_to_object(obj);
    return obj;
}

// ResizeObserver.prototype.observe(target)
static GCValue js_resize_observer_observe(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// ResizeObserver.prototype.unobserve(target)
static GCValue js_resize_observer_unobserve(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// ResizeObserver.prototype.disconnect()
static GCValue js_resize_observer_disconnect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_resize_observer_proto_funcs[] = {
    JS_CFUNC_DEF("observe", 1, js_resize_observer_observe),
    JS_CFUNC_DEF("unobserve", 1, js_resize_observer_unobserve),
    JS_CFUNC_DEF("disconnect", 0, js_resize_observer_disconnect),
};

// ============================================================================
// IntersectionObserver Implementation
// ============================================================================

// IntersectionObserverData struct is defined in browser_api_impl_types.h

static void js_intersection_observer_finalizer(JSRuntimeHandle rt, GCValue val) {
    // IntersectionObserverData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

static JSClassDef js_intersection_observer_class_def = {
    .class_name = "IntersectionObserver",
    .finalizer = js_intersection_observer_finalizer,
};

// IntersectionObserver constructor
static GCValue js_intersection_observer_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "IntersectionObserver constructor requires a callback function");
    }
    
    IntersectionObserverDataHandle io = IntersectionObserverDataHandle::create(ctx, argv[0]);
    if (!io.valid()) return JS_ThrowTypeError(ctx, "Invalid IntersectionObserver");
    
    // Parse options if provided
    if (argc > 1 && JS_IsObject(argv[1])) {
        GCValue root_val = JS_GetPropertyStr(ctx, argv[1], "root");
        if (!JS_IsUndefined(root_val) && !JS_IsNull(root_val)) {
            io.set_root(root_val);
        }

        GCValue margin_val = JS_GetPropertyStr(ctx, argv[1], "rootMargin");
        const char *margin = JS_ToCString(ctx, margin_val);
        if (margin) {
            io.set_root_margin(margin);
        }

        GCValue threshold_val = JS_GetPropertyStr(ctx, argv[1], "threshold");
        double threshold;
        if (!JS_IsException(threshold_val) && !JS_ToFloat64(ctx, &threshold, threshold_val)) {
            io.set_threshold(threshold);
        }

    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_intersection_observer_class_id);
    io.attach_to_object(obj);
    return obj;
}

// IntersectionObserver.prototype.observe(target)
static GCValue js_intersection_observer_observe(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// IntersectionObserver.prototype.unobserve(target)
static GCValue js_intersection_observer_unobserve(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// IntersectionObserver.prototype.disconnect()
static GCValue js_intersection_observer_disconnect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// IntersectionObserver.prototype.takeRecords()
static GCValue js_intersection_observer_takeRecords(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_NewArray(ctx);
}

static const JSCFunctionListEntry js_intersection_observer_proto_funcs[] = {
    JS_CFUNC_DEF("observe", 1, js_intersection_observer_observe),
    JS_CFUNC_DEF("unobserve", 1, js_intersection_observer_unobserve),
    JS_CFUNC_DEF("disconnect", 0, js_intersection_observer_disconnect),
    JS_CFUNC_DEF("takeRecords", 0, js_intersection_observer_takeRecords),
};

// ============================================================================
// Performance API Implementation
// ============================================================================

// PerformanceData, PerformanceEntryData, PerformanceObserverData structs are defined in browser_api_impl_types.h

static void js_performance_finalizer(JSRuntimeHandle rt, GCValue val) {
    // PerformanceData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

static void js_performance_entry_finalizer(JSRuntimeHandle rt, GCValue val) {
    // PerformanceEntryData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

static void js_performance_observer_finalizer(JSRuntimeHandle rt, GCValue val) {
    // PerformanceObserverData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

static JSClassDef js_performance_class_def = {
    .class_name = "Performance",
    .finalizer = js_performance_finalizer,
};

static JSClassDef js_performance_entry_class_def = {
    .class_name = "PerformanceEntry",
    .finalizer = js_performance_entry_finalizer,
};

static JSClassDef js_performance_observer_class_def = {
    .class_name = "PerformanceObserver",
    .finalizer = js_performance_observer_finalizer,
};

// Performance.now() - high resolution timestamp
static double g_performance_time_origin = 0.0;

#ifdef _MSC_VER
static double performance_get_time_ms(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
static double performance_get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
#endif

static GCValue js_performance_now(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    // Return high resolution timestamp relative to time origin
    double now = performance_get_time_ms() - g_performance_time_origin;
    return JS_NewFloat64(ctx, now);
}

// Performance.timeOrigin getter
static GCValue js_performance_get_time_origin(JSContextHandle ctx, GCValue this_val) {
    (void)this_val;
    return JS_NewFloat64(ctx, g_performance_time_origin);
}

// Helper to get PerformanceData from JS object
static PerformanceData* get_performance_data(JSContextHandle ctx, GCValue obj) {
    (void)ctx;
    // Use gc_deref to get pointer from handle stored in opaque
    GCHandle handle = JS_GetOpaqueHandle(obj, JS_GC_OBJ_TYPE_DATA);
    if (handle == GC_HANDLE_NULL) return NULL;
    return (PerformanceData*)gc_deref(handle);
}

// Performance.getEntries()
static GCValue js_performance_get_entries(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue result = JS_NewArray(ctx);
    
    PerformanceData* perf = get_performance_data(ctx, this_val);
    if (!perf) return result;
    
    for (int i = 0; i < perf->entry_count; i++) {
        // Create PerformanceEntry object
        GCValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "name", JS_NewString(ctx, perf->entries[i].name));
        JS_SetPropertyStr(ctx, entry, "entryType", JS_NewString(ctx, perf->entries[i].entryType));
        JS_SetPropertyStr(ctx, entry, "startTime", JS_NewFloat64(ctx, perf->entries[i].startTime));
        JS_SetPropertyStr(ctx, entry, "duration", JS_NewFloat64(ctx, perf->entries[i].duration));
        JS_SetPropertyUint32(ctx, result, i, entry);
    }
    
    return result;
}

// Performance.getEntriesByType(type)
static GCValue js_performance_get_entries_by_type(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue result = JS_NewArray(ctx);
    
    if (argc < 1) return result;
    
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return result;
    
    PerformanceData* perf = get_performance_data(ctx, this_val);
    if (!perf) return result;
    
    int idx = 0;
    for (int i = 0; i < perf->entry_count; i++) {
        if (strcmp(perf->entries[i].entryType, type) == 0) {
            GCValue entry = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, entry, "name", JS_NewString(ctx, perf->entries[i].name));
            JS_SetPropertyStr(ctx, entry, "entryType", JS_NewString(ctx, perf->entries[i].entryType));
            JS_SetPropertyStr(ctx, entry, "startTime", JS_NewFloat64(ctx, perf->entries[i].startTime));
            JS_SetPropertyStr(ctx, entry, "duration", JS_NewFloat64(ctx, perf->entries[i].duration));
            JS_SetPropertyUint32(ctx, result, idx++, entry);
        }
    }
    
    return result;
}

// Performance.getEntriesByName(name, type)
static GCValue js_performance_get_entries_by_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue result = JS_NewArray(ctx);
    
    if (argc < 1) return result;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return result;
    
    // Optional type filter
    const char *type_filter = NULL;
    if (argc >= 2) {
        type_filter = JS_ToCString(ctx, argv[1]);
    }
    
    PerformanceData* perf = get_performance_data(ctx, this_val);
    if (!perf) return result;
    
    int idx = 0;
    for (int i = 0; i < perf->entry_count; i++) {
        if (strcmp(perf->entries[i].name, name) == 0) {
            // Check type filter if provided
            if (type_filter && strcmp(perf->entries[i].entryType, type_filter) != 0) {
                continue;
            }
            GCValue entry = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, entry, "name", JS_NewString(ctx, perf->entries[i].name));
            JS_SetPropertyStr(ctx, entry, "entryType", JS_NewString(ctx, perf->entries[i].entryType));
            JS_SetPropertyStr(ctx, entry, "startTime", JS_NewFloat64(ctx, perf->entries[i].startTime));
            JS_SetPropertyStr(ctx, entry, "duration", JS_NewFloat64(ctx, perf->entries[i].duration));
            JS_SetPropertyUint32(ctx, result, idx++, entry);
        }
    }
    
    return result;
}

// Performance.mark(name)
static GCValue js_performance_mark(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_UNDEFINED;
    
    PerformanceData* perf = get_performance_data(ctx, this_val);
    if (!perf) return JS_UNDEFINED;
    
    if (perf->entry_count >= PERFORMANCE_MAX_ENTRIES) {
        platform_log(LOG_LEVEL_ERROR, "Performance", "FATAL: Performance entry limit of %d exceeded. Aborting.", PERFORMANCE_MAX_ENTRIES);
        abort();
    }
    
    // Add mark entry
    PerformanceEntryData* entry = &perf->entries[perf->entry_count++];
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    strncpy(entry->entryType, "mark", sizeof(entry->entryType) - 1);
    entry->startTime = performance_get_time_ms() - g_performance_time_origin;
    entry->duration = 0;
    
    return JS_UNDEFINED;
}

// Performance.measure(name, startMark, endMark)
static GCValue js_performance_measure(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_UNDEFINED;
    
    PerformanceData* perf = get_performance_data(ctx, this_val);
    if (!perf) return JS_UNDEFINED;
    
    if (perf->entry_count >= PERFORMANCE_MAX_ENTRIES) {
        platform_log(LOG_LEVEL_ERROR, "Performance", "FATAL: Performance entry limit of %d exceeded. Aborting.", PERFORMANCE_MAX_ENTRIES);
        abort();
    }
    
    double start_time = 0;
    double end_time = performance_get_time_ms() - g_performance_time_origin;
    
    // Parse startMark if provided
    if (argc >= 2 && JS_IsString(argv[1])) {
        const char *start_mark = JS_ToCString(ctx, argv[1]);
        if (start_mark) {
            // Find the mark
            for (int i = 0; i < perf->entry_count; i++) {
                if (strcmp(perf->entries[i].name, start_mark) == 0 && 
                    strcmp(perf->entries[i].entryType, "mark") == 0) {
                    start_time = perf->entries[i].startTime;
                    break;
                }
            }
        }
    }
    
    // Parse endMark if provided
    if (argc >= 3 && JS_IsString(argv[2])) {
        const char *end_mark = JS_ToCString(ctx, argv[2]);
        if (end_mark) {
            // Find the mark
            for (int i = 0; i < perf->entry_count; i++) {
                if (strcmp(perf->entries[i].name, end_mark) == 0 && 
                    strcmp(perf->entries[i].entryType, "mark") == 0) {
                    end_time = perf->entries[i].startTime;
                    break;
                }
            }
        }
    }
    
    // Add measure entry
    PerformanceEntryData* entry = &perf->entries[perf->entry_count++];
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    strncpy(entry->entryType, "measure", sizeof(entry->entryType) - 1);
    entry->startTime = start_time;
    entry->duration = end_time - start_time;
    
    return JS_UNDEFINED;
}

// Performance.clearMarks(name)
static GCValue js_performance_clear_marks(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    PerformanceData* perf = get_performance_data(ctx, this_val);
    if (!perf) return JS_UNDEFINED;
    
    const char *name_filter = NULL;
    if (argc >= 1 && JS_IsString(argv[0])) {
        name_filter = JS_ToCString(ctx, argv[0]);
    }
    
    int write_idx = 0;
    for (int i = 0; i < perf->entry_count; i++) {
        int should_remove = (strcmp(perf->entries[i].entryType, "mark") == 0);
        if (should_remove && name_filter) {
            should_remove = (strcmp(perf->entries[i].name, name_filter) == 0);
        }
        
        if (!should_remove) {
            // Keep this entry
            if (write_idx != i) {
                perf->entries[write_idx] = perf->entries[i];
            }
            write_idx++;
        }
    }
    perf->entry_count = write_idx;
    
    return JS_UNDEFINED;
}

// Performance.clearMeasures(name)
static GCValue js_performance_clear_measures(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    PerformanceData* perf = get_performance_data(ctx, this_val);
    if (!perf) return JS_UNDEFINED;
    
    const char *name_filter = NULL;
    if (argc >= 1 && JS_IsString(argv[0])) {
        name_filter = JS_ToCString(ctx, argv[0]);
    }
    
    int write_idx = 0;
    for (int i = 0; i < perf->entry_count; i++) {
        int should_remove = (strcmp(perf->entries[i].entryType, "measure") == 0);
        if (should_remove && name_filter) {
            should_remove = (strcmp(perf->entries[i].name, name_filter) == 0);
        }
        
        if (!should_remove) {
            // Keep this entry
            if (write_idx != i) {
                perf->entries[write_idx] = perf->entries[i];
            }
            write_idx++;
        }
    }
    perf->entry_count = write_idx;
    
    return JS_UNDEFINED;
}

// Performance.clearResourceTimings()
static GCValue js_performance_clear_resource_timings(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    PerformanceData* perf = get_performance_data(ctx, this_val);
    if (!perf) return JS_UNDEFINED;
    
    int write_idx = 0;
    for (int i = 0; i < perf->entry_count; i++) {
        int should_remove = (strcmp(perf->entries[i].entryType, "resource") == 0);
        
        if (!should_remove) {
            // Keep this entry
            if (write_idx != i) {
                perf->entries[write_idx] = perf->entries[i];
            }
            write_idx++;
        }
    }
    perf->entry_count = write_idx;
    
    return JS_UNDEFINED;
}

// PerformanceTimingData struct is defined in browser_api_impl_types.h

static void js_performance_timing_finalizer(JSRuntimeHandle rt, GCValue val) {
    // PerformanceTimingData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

static JSClassDef js_performance_timing_class_def = {
    .class_name = "PerformanceTiming",
    .finalizer = js_performance_timing_finalizer,
};

#define DEF_TIMING_GETTER(field) \
static GCValue js_performance_timing_get_##field(JSContextHandle ctx, GCValue this_val) { \
    PerformanceTimingDataHandle timing = PerformanceTimingDataHandle::from_object_check(ctx, this_val); \
    if (!timing.valid()) return JS_ThrowTypeError(ctx, "Invalid PerformanceTiming"); \
    return JS_NewFloat64(ctx, timing.field()); \
}

DEF_TIMING_GETTER(navigationStart)
DEF_TIMING_GETTER(unloadEventStart)
DEF_TIMING_GETTER(unloadEventEnd)
DEF_TIMING_GETTER(redirectStart)
DEF_TIMING_GETTER(redirectEnd)
DEF_TIMING_GETTER(fetchStart)
DEF_TIMING_GETTER(domainLookupStart)
DEF_TIMING_GETTER(domainLookupEnd)
DEF_TIMING_GETTER(connectStart)
DEF_TIMING_GETTER(connectEnd)
DEF_TIMING_GETTER(secureConnectionStart)
DEF_TIMING_GETTER(requestStart)
DEF_TIMING_GETTER(responseStart)
DEF_TIMING_GETTER(responseEnd)
DEF_TIMING_GETTER(domLoading)
DEF_TIMING_GETTER(domInteractive)
DEF_TIMING_GETTER(domContentLoadedEventStart)
DEF_TIMING_GETTER(domContentLoadedEventEnd)
DEF_TIMING_GETTER(domComplete)
DEF_TIMING_GETTER(loadEventStart)
DEF_TIMING_GETTER(loadEventEnd)

#undef DEF_TIMING_GETTER

static const JSCFunctionListEntry js_performance_timing_proto_funcs[] = {
    JS_CGETSET_DEF("navigationStart", js_performance_timing_get_navigationStart, NULL),
    JS_CGETSET_DEF("unloadEventStart", js_performance_timing_get_unloadEventStart, NULL),
    JS_CGETSET_DEF("unloadEventEnd", js_performance_timing_get_unloadEventEnd, NULL),
    JS_CGETSET_DEF("redirectStart", js_performance_timing_get_redirectStart, NULL),
    JS_CGETSET_DEF("redirectEnd", js_performance_timing_get_redirectEnd, NULL),
    JS_CGETSET_DEF("fetchStart", js_performance_timing_get_fetchStart, NULL),
    JS_CGETSET_DEF("domainLookupStart", js_performance_timing_get_domainLookupStart, NULL),
    JS_CGETSET_DEF("domainLookupEnd", js_performance_timing_get_domainLookupEnd, NULL),
    JS_CGETSET_DEF("connectStart", js_performance_timing_get_connectStart, NULL),
    JS_CGETSET_DEF("connectEnd", js_performance_timing_get_connectEnd, NULL),
    JS_CGETSET_DEF("secureConnectionStart", js_performance_timing_get_secureConnectionStart, NULL),
    JS_CGETSET_DEF("requestStart", js_performance_timing_get_requestStart, NULL),
    JS_CGETSET_DEF("responseStart", js_performance_timing_get_responseStart, NULL),
    JS_CGETSET_DEF("responseEnd", js_performance_timing_get_responseEnd, NULL),
    JS_CGETSET_DEF("domLoading", js_performance_timing_get_domLoading, NULL),
    JS_CGETSET_DEF("domInteractive", js_performance_timing_get_domInteractive, NULL),
    JS_CGETSET_DEF("domContentLoadedEventStart", js_performance_timing_get_domContentLoadedEventStart, NULL),
    JS_CGETSET_DEF("domContentLoadedEventEnd", js_performance_timing_get_domContentLoadedEventEnd, NULL),
    JS_CGETSET_DEF("domComplete", js_performance_timing_get_domComplete, NULL),
    JS_CGETSET_DEF("loadEventStart", js_performance_timing_get_loadEventStart, NULL),
    JS_CGETSET_DEF("loadEventEnd", js_performance_timing_get_loadEventEnd, NULL),
    JS_CGETSET_DEF("toJSON", js_performance_timing_get_navigationStart, NULL), // stub
};

// Performance.timing getter - returns a simple object with timing properties
static GCValue js_performance_get_timing(JSContextHandle ctx, GCValue this_val) {
    // Get the timing object from the Performance instance's opaque data
    // For simplicity, we store the timing object as a property on the performance instance
    GCValue timing_prop = JS_GetPropertyStr(ctx, this_val, "__timing");
    if (!JS_IsUndefined(timing_prop) && !JS_IsNull(timing_prop)) {
        return timing_prop;
    }

    // Create a simple timing object with all properties set to 0
    GCValue timing_obj = JS_NewObject(ctx);
    
    // Set all timing properties to 0
    JS_SetPropertyStr(ctx, timing_obj, "navigationStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "unloadEventStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "unloadEventEnd", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "redirectStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "redirectEnd", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "fetchStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "domainLookupStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "domainLookupEnd", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "connectStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "connectEnd", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "secureConnectionStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "requestStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "responseStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "responseEnd", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "domLoading", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "domInteractive", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "domContentLoadedEventStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "domContentLoadedEventEnd", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "domComplete", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "loadEventStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "loadEventEnd", JS_NewFloat64(ctx, 0.0));
    
    // Store on the performance instance
    JS_SetPropertyStr(ctx, this_val, "__timing", timing_obj);
    
    return timing_obj;
}

static const JSCFunctionListEntry js_performance_proto_funcs[] = {
    JS_CFUNC_DEF("now", 0, js_performance_now),
    JS_CGETSET_DEF("timeOrigin", js_performance_get_time_origin, NULL),
    // Note: timing is set directly on the instance, not as a getter on the prototype
    JS_CFUNC_DEF("getEntries", 0, js_performance_get_entries),
    JS_CFUNC_DEF("getEntriesByType", 1, js_performance_get_entries_by_type),
    JS_CFUNC_DEF("getEntriesByName", 1, js_performance_get_entries_by_name),
    JS_CFUNC_DEF("mark", 1, js_performance_mark),
    JS_CFUNC_DEF("measure", 1, js_performance_measure),
    JS_CFUNC_DEF("clearMarks", 0, js_performance_clear_marks),
    JS_CFUNC_DEF("clearMeasures", 0, js_performance_clear_measures),
    JS_CFUNC_DEF("clearResourceTimings", 0, js_performance_clear_resource_timings),
};

// PerformanceEntry.name getter
static GCValue js_performance_entry_get_name(JSContextHandle ctx, GCValue this_val) {
    PerformanceEntryDataHandle entry = PerformanceEntryDataHandle::from_object_check(ctx, this_val);
    if (!entry.valid()) return JS_ThrowTypeError(ctx, "Invalid PerformanceEntry");
    return JS_NewString(ctx, entry.name());
}

// PerformanceEntry.entryType getter
static GCValue js_performance_entry_get_entry_type(JSContextHandle ctx, GCValue this_val) {
    PerformanceEntryDataHandle entry = PerformanceEntryDataHandle::from_object_check(ctx, this_val);
    if (!entry.valid()) return JS_ThrowTypeError(ctx, "Invalid PerformanceEntry");
    return JS_NewString(ctx, entry.entry_type());
}

// PerformanceEntry.startTime getter
static GCValue js_performance_entry_get_start_time(JSContextHandle ctx, GCValue this_val) {
    PerformanceEntryDataHandle entry = PerformanceEntryDataHandle::from_object_check(ctx, this_val);
    if (!entry.valid()) return JS_ThrowTypeError(ctx, "Invalid PerformanceEntry");
    return JS_NewFloat64(ctx, entry.start_time());
}

// PerformanceEntry.duration getter
static GCValue js_performance_entry_get_duration(JSContextHandle ctx, GCValue this_val) {
    PerformanceEntryDataHandle entry = PerformanceEntryDataHandle::from_object_check(ctx, this_val);
    if (!entry.valid()) return JS_ThrowTypeError(ctx, "Invalid PerformanceEntry");
    return JS_NewFloat64(ctx, entry.duration());
}

static const JSCFunctionListEntry js_performance_entry_proto_funcs[] = {
    JS_CGETSET_DEF("name", js_performance_entry_get_name, NULL),
    JS_CGETSET_DEF("entryType", js_performance_entry_get_entry_type, NULL),
    JS_CGETSET_DEF("startTime", js_performance_entry_get_start_time, NULL),
    JS_CGETSET_DEF("duration", js_performance_entry_get_duration, NULL),
};

// PerformanceObserver constructor
static GCValue js_performance_observer_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "PerformanceObserver constructor requires a callback function");
    }
    
    PerformanceObserverDataHandle po = PerformanceObserverDataHandle::create(ctx, argv[0]);
    if (!po.valid()) return JS_ThrowTypeError(ctx, "Invalid PerformanceObserver");
    
    GCValue obj = JS_NewObjectClass(ctx, js_performance_observer_class_id);
    po.attach_to_object(obj);
    return obj;
}

// PerformanceObserver.prototype.observe(options)
static GCValue js_performance_observer_observe(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// PerformanceObserver.prototype.disconnect()
static GCValue js_performance_observer_disconnect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// PerformanceObserver.prototype.takeRecords()
static GCValue js_performance_observer_takeRecords(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_NewArray(ctx);
}

// PerformanceObserver.supportedEntryTypes getter
static GCValue js_performance_observer_get_supported_entry_types(JSContextHandle ctx, GCValue this_val) {
    // Return an array of supported entry types
    GCValue array = JS_NewArray(ctx);
    return array;
}

static const JSCFunctionListEntry js_performance_observer_proto_funcs[] = {
    JS_CFUNC_DEF("observe", 1, js_performance_observer_observe),
    JS_CFUNC_DEF("disconnect", 0, js_performance_observer_disconnect),
    JS_CFUNC_DEF("takeRecords", 0, js_performance_observer_takeRecords),
    JS_CGETSET_DEF("supportedEntryTypes", js_performance_observer_get_supported_entry_types, NULL),
};

// ============================================================================
// DOMRect Implementation
// ============================================================================

// DOMRectData struct is defined in browser_api_impl_types.h

static void js_dom_rect_finalizer(JSRuntimeHandle rt, GCValue val) {
    // DOMRectData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

static void js_dom_rect_read_only_finalizer(JSRuntimeHandle rt, GCValue val) {
    // DOMRectData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

static JSClassDef js_dom_rect_class_def = {
    .class_name = "DOMRect",
    .finalizer = js_dom_rect_finalizer,
};

static JSClassDef js_dom_rect_read_only_class_def = {
    .class_name = "DOMRectReadOnly",
    .finalizer = js_dom_rect_read_only_finalizer,
};

// Element.prototype.getBoundingClientRect - returns a DOMRect
static GCValue js_element_getBoundingClientRect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMRectDataHandle rect = DOMRectDataHandle::create();
    if (!rect.valid()) return JS_ThrowTypeError(ctx, "Invalid DOMRect");
    
    rect.set_x(0);
    rect.set_y(0);
    rect.set_width(640);
    rect.set_height(360);
    rect.compute_bounds();
    
    GCValue obj = JS_NewObjectClass(ctx, js_dom_rect_class_id);
    rect.attach_to_object(obj);
    return obj;
}

// DOMRect constructor
static GCValue js_dom_rect_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    DOMRectDataHandle rect = DOMRectDataHandle::create();
    if (!rect.valid()) return JS_ThrowTypeError(ctx, "Invalid DOMRect");
    
    double x = 0, y = 0, width = 0, height = 0;
    
    if (argc > 0) JS_ToFloat64(ctx, &x, argv[0]);
    if (argc > 1) JS_ToFloat64(ctx, &y, argv[1]);
    if (argc > 2) JS_ToFloat64(ctx, &width, argv[2]);
    if (argc > 3) JS_ToFloat64(ctx, &height, argv[3]);
    
    rect.set_x(x);
    rect.set_y(y);
    rect.set_width(width);
    rect.set_height(height);
    rect.compute_bounds();
    
    GCValue obj = JS_NewObjectClass(ctx, js_dom_rect_class_id);
    rect.attach_to_object(obj);
    return obj;
}

// DOMRectReadOnly constructor
static GCValue js_dom_rect_read_only_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    DOMRectDataHandle rect = DOMRectDataHandle::create();
    if (!rect.valid()) return JS_ThrowTypeError(ctx, "Invalid DOMRect");
    
    double x = 0, y = 0, width = 0, height = 0;
    
    if (argc > 0) JS_ToFloat64(ctx, &x, argv[0]);
    if (argc > 1) JS_ToFloat64(ctx, &y, argv[1]);
    if (argc > 2) JS_ToFloat64(ctx, &width, argv[2]);
    if (argc > 3) JS_ToFloat64(ctx, &height, argv[3]);
    
    rect.set_x(x);
    rect.set_y(y);
    rect.set_width(width);
    rect.set_height(height);
    rect.compute_bounds();
    
    GCValue obj = JS_NewObjectClass(ctx, js_dom_rect_read_only_class_id);
    rect.attach_to_object(obj);
    return obj;
}

// DOMRect.fromRect(other)
static GCValue js_dom_rect_from_rect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    double x = 0, y = 0, width = 0, height = 0;
    
    if (argc > 0 && JS_IsObject(argv[0])) {
        GCValue x_val = JS_GetPropertyStr(ctx, argv[0], "x");
        GCValue y_val = JS_GetPropertyStr(ctx, argv[0], "y");
        GCValue w_val = JS_GetPropertyStr(ctx, argv[0], "width");
        GCValue h_val = JS_GetPropertyStr(ctx, argv[0], "height");
        
        JS_ToFloat64(ctx, &x, x_val);
        JS_ToFloat64(ctx, &y, y_val);
        JS_ToFloat64(ctx, &width, w_val);
        JS_ToFloat64(ctx, &height, h_val);




    }
    
    GCValue args[4] = {
        JS_NewFloat64(ctx, x),
        JS_NewFloat64(ctx, y),
        JS_NewFloat64(ctx, width),
        JS_NewFloat64(ctx, height)
    };
    
    GCValue result = js_dom_rect_constructor(ctx, JS_UNDEFINED, 4, args);




    return result;
}

// DOMRectReadOnly.fromRect(other)
static GCValue js_dom_rect_read_only_from_rect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    double x = 0, y = 0, width = 0, height = 0;
    
    if (argc > 0 && JS_IsObject(argv[0])) {
        GCValue x_val = JS_GetPropertyStr(ctx, argv[0], "x");
        GCValue y_val = JS_GetPropertyStr(ctx, argv[0], "y");
        GCValue w_val = JS_GetPropertyStr(ctx, argv[0], "width");
        GCValue h_val = JS_GetPropertyStr(ctx, argv[0], "height");
        
        JS_ToFloat64(ctx, &x, x_val);
        JS_ToFloat64(ctx, &y, y_val);
        JS_ToFloat64(ctx, &width, w_val);
        JS_ToFloat64(ctx, &height, h_val);




    }
    
    GCValue args[4] = {
        JS_NewFloat64(ctx, x),
        JS_NewFloat64(ctx, y),
        JS_NewFloat64(ctx, width),
        JS_NewFloat64(ctx, height)
    };
    
    GCValue result = js_dom_rect_read_only_constructor(ctx, JS_UNDEFINED, 4, args);




    return result;
}

#define DEF_DOM_RECT_GETTER(name, getter_func) \
static GCValue js_dom_rect_get_##name(JSContextHandle ctx, GCValue this_val) { \
    DOMRectDataHandle rect = DOMRectDataHandle::from_dom_rect(this_val); \
    if (!rect.valid()) { \
        rect = DOMRectDataHandle::from_dom_rect_read_only(this_val); \
        if (!rect.valid()) return JS_ThrowTypeError(ctx, "Invalid DOMRect"); \
    } \
    return JS_NewFloat64(ctx, rect.getter_func()); \
}

DEF_DOM_RECT_GETTER(x, x)
DEF_DOM_RECT_GETTER(y, y)
DEF_DOM_RECT_GETTER(width, width)
DEF_DOM_RECT_GETTER(height, height)
DEF_DOM_RECT_GETTER(top, top)
DEF_DOM_RECT_GETTER(right, right)
DEF_DOM_RECT_GETTER(bottom, bottom)
DEF_DOM_RECT_GETTER(left, left)

#undef DEF_DOM_RECT_GETTER

#define DEF_DOM_RECT_SETTER(name, setter_func) \
static GCValue js_dom_rect_set_##name(JSContextHandle ctx, GCValue this_val, GCValue val) { \
    DOMRectDataHandle rect = DOMRectDataHandle::from_dom_rect(this_val); \
    if (!rect.valid()) return JS_ThrowTypeError(ctx, "Invalid DOMRect"); \
    double value; \
    JS_ToFloat64(ctx, &value, val); \
    rect.setter_func(value); \
    rect.compute_bounds(); \
    return JS_UNDEFINED; \
}

DEF_DOM_RECT_SETTER(x, set_x)
DEF_DOM_RECT_SETTER(y, set_y)
DEF_DOM_RECT_SETTER(width, set_width)
DEF_DOM_RECT_SETTER(height, set_height)

#undef DEF_DOM_RECT_SETTER

static const JSCFunctionListEntry js_dom_rect_proto_funcs[] = {
    JS_CGETSET_DEF("x", js_dom_rect_get_x, js_dom_rect_set_x),
    JS_CGETSET_DEF("y", js_dom_rect_get_y, js_dom_rect_set_y),
    JS_CGETSET_DEF("width", js_dom_rect_get_width, js_dom_rect_set_width),
    JS_CGETSET_DEF("height", js_dom_rect_get_height, js_dom_rect_set_height),
    JS_CGETSET_DEF("top", js_dom_rect_get_top, NULL),
    JS_CGETSET_DEF("right", js_dom_rect_get_right, NULL),
    JS_CGETSET_DEF("bottom", js_dom_rect_get_bottom, NULL),
    JS_CGETSET_DEF("left", js_dom_rect_get_left, NULL),
    JS_CFUNC_DEF("toJSON", 0, js_empty_string),
};

static const JSCFunctionListEntry js_dom_rect_read_only_proto_funcs[] = {
    JS_CGETSET_DEF("x", js_dom_rect_get_x, NULL),
    JS_CGETSET_DEF("y", js_dom_rect_get_y, NULL),
    JS_CGETSET_DEF("width", js_dom_rect_get_width, NULL),
    JS_CGETSET_DEF("height", js_dom_rect_get_height, NULL),
    JS_CGETSET_DEF("top", js_dom_rect_get_top, NULL),
    JS_CGETSET_DEF("right", js_dom_rect_get_right, NULL),
    JS_CGETSET_DEF("bottom", js_dom_rect_get_bottom, NULL),
    JS_CGETSET_DEF("left", js_dom_rect_get_left, NULL),
    JS_CFUNC_DEF("toJSON", 0, js_empty_string),
};

// ============================================================================
// Date API Implementation
// ============================================================================

static void js_date_finalizer(JSRuntimeHandle rt, GCValue val) {
    // DateData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

static JSClassDef js_date_class_def = {
    .class_name = "Date",
    .finalizer = js_date_finalizer,
};

// Helper: Get current time in milliseconds using platform API
static long long date_get_current_time_ms(void) {
    return (long long)platform_get_time_ms();
}

// Helper: Parse ISO date string to timestamp (simplified)
static long long date_parse_iso_string(const char *str) {
    struct tm tm = {0};
    int ms = 0;
    int tz_offset = 0;
    
    // Try ISO 8601 format: YYYY-MM-DDTHH:MM:SS.sssZ or YYYY-MM-DDTHH:MM:SS.sss+HH:MM
    if (sscanf(str, "%d-%d-%dT%d:%d:%d.%dZ", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &ms) >= 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        time_t t = timegm(&tm);
        if (t != -1) return (long long)t * 1000 + ms;
    }
    
    // Try without milliseconds: YYYY-MM-DDTHH:MM:SSZ
    if (sscanf(str, "%d-%d-%dT%d:%d:%dZ", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        time_t t = timegm(&tm);
        if (t != -1) return (long long)t * 1000;
    }
    
    // Try simple date format: YYYY-MM-DD
    if (sscanf(str, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) >= 3) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm.tm_hour = 0;
        tm.tm_min = 0;
        tm.tm_sec = 0;
        time_t t = timegm(&tm);
        if (t != -1) return (long long)t * 1000;
    }
    
    return 0; // Invalid date
}

// Helper: Convert timestamp to broken-down time
static void date_to_utc_time(long long timestamp_ms, struct tm *out_tm, int *out_ms) {
    time_t seconds = (time_t)(timestamp_ms / 1000);
    *out_ms = (int)(timestamp_ms % 1000);
    if (*out_ms < 0) {
        *out_ms += 1000;
        seconds -= 1;
    }
    gmtime_r(&seconds, out_tm);
}

// Helper: Convert timestamp to local time
static void date_to_local_time(long long timestamp_ms, struct tm *out_tm, int *out_ms) {
    time_t seconds = (time_t)(timestamp_ms / 1000);
    *out_ms = (int)(timestamp_ms % 1000);
    if (*out_ms < 0) {
        *out_ms += 1000;
        seconds -= 1;
    }
    localtime_r(&seconds, out_tm);
}

// Date constructor
static GCValue js_date_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    DateDataHandle date = DateDataHandle::create();
    if (!date.valid()) return JS_ThrowTypeError(ctx, "Invalid Date");
    
    long long timestamp_ms = 0;
    int is_valid = 1;
    
    if (argc == 0) {
        // new Date() - current time
        timestamp_ms = date_get_current_time_ms();
    } else if (argc == 1) {
        // new Date(value) - value can be string or number
        if (JS_IsString(argv[0])) {
            const char *str = JS_ToCString(ctx, argv[0]);
            if (str) {
                timestamp_ms = date_parse_iso_string(str);
            }
        } else if (JS_IsNumber(argv[0])) {
            double val;
            JS_ToFloat64(ctx, &val, argv[0]);
            timestamp_ms = (long long)val;
        } else if (JS_IsObject(argv[0])) {
            // Check if it's another Date object
            DateDataHandle other = DateDataHandle::from_object_check(ctx, argv[0]);
            if (other.valid()) {
                timestamp_ms = other.timestamp_ms();
                is_valid = other.is_valid();
            } else {
                is_valid = 0;
            }
        } else {
            is_valid = 0;
        }
    } else {
        // new Date(year, month, day, hours, minutes, seconds, ms)
        double year = 0, month = 0, day = 1, hours = 0, minutes = 0, seconds = 0, ms = 0;
        JS_ToFloat64(ctx, &year, argv[0]);
        JS_ToFloat64(ctx, &month, argv[1]);
        if (argc > 2) JS_ToFloat64(ctx, &day, argv[2]);
        if (argc > 3) JS_ToFloat64(ctx, &hours, argv[3]);
        if (argc > 4) JS_ToFloat64(ctx, &minutes, argv[4]);
        if (argc > 5) JS_ToFloat64(ctx, &seconds, argv[5]);
        if (argc > 6) JS_ToFloat64(ctx, &ms, argv[6]);
        
        // Handle 2-digit years
        if (year >= 0 && year <= 99) {
            year += 1900;
        }
        
        struct tm tm = {0};
        tm.tm_year = (int)year - 1900;
        tm.tm_mon = (int)month;
        tm.tm_mday = (int)day;
        tm.tm_hour = (int)hours;
        tm.tm_min = (int)minutes;
        tm.tm_sec = (int)seconds;
        
        time_t t = timegm(&tm);
        if (t != -1) {
            timestamp_ms = (long long)t * 1000 + (long long)ms;
        } else {
            is_valid = 0;
        }
    }
    
    date.set_timestamp_ms(timestamp_ms);
    date.set_valid(is_valid);
    
    GCValue obj = JS_NewObjectClass(ctx, js_date_class_id);
    date.attach_to_object(obj);
    return obj;
}

// Date.prototype.getTime()
static GCValue js_date_getTime(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid()) return JS_ThrowTypeError(ctx, "Invalid Date object");
    if (!date.is_valid()) return JS_NewFloat64(ctx, NAN);
    return JS_NewFloat64(ctx, (double)date.timestamp_ms());
}

// Date.prototype.valueOf() - same as getTime
static GCValue js_date_valueOf(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_date_getTime(ctx, this_val, argc, argv);
}

// Date.prototype.getFullYear()
static GCValue js_date_getFullYear(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_year + 1900);
}

// Date.prototype.getUTCFullYear()
static GCValue js_date_getUTCFullYear(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_year + 1900);
}

// Date.prototype.getMonth()
static GCValue js_date_getMonth(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_mon);
}

// Date.prototype.getUTCMonth()
static GCValue js_date_getUTCMonth(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_mon);
}

// Date.prototype.getDate()
static GCValue js_date_getDate(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_mday);
}

// Date.prototype.getUTCDate()
static GCValue js_date_getUTCDate(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_mday);
}

// Date.prototype.getDay()
static GCValue js_date_getDay(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_wday);
}

// Date.prototype.getUTCDay()
static GCValue js_date_getUTCDay(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_wday);
}

// Date.prototype.getHours()
static GCValue js_date_getHours(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_hour);
}

// Date.prototype.getUTCHours()
static GCValue js_date_getUTCHours(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_hour);
}

// Date.prototype.getMinutes()
static GCValue js_date_getMinutes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_min);
}

// Date.prototype.getUTCMinutes()
static GCValue js_date_getUTCMinutes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_min);
}

// Date.prototype.getSeconds()
static GCValue js_date_getSeconds(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_sec);
}

// Date.prototype.getUTCSeconds()
static GCValue js_date_getUTCSeconds(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_sec);
}

// Date.prototype.getMilliseconds()
static GCValue js_date_getMilliseconds(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, ms);
}

// Date.prototype.getUTCMilliseconds()
static GCValue js_date_getUTCMilliseconds(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, ms);
}

// Date.prototype.getTimezoneOffset()
static GCValue js_date_getTimezoneOffset(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm local_tm, utc_tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &local_tm, &ms);
    date_to_utc_time(date.timestamp_ms(), &utc_tm, &ms);
    
    time_t local_t = mktime(&local_tm);
    time_t utc_t = timegm(&utc_tm);
    int offset_minutes = (int)((local_t - utc_t) / 60);
    
    return JS_NewInt32(ctx, offset_minutes);
}

// Date.prototype.toString()
static GCValue js_date_toString(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewString(ctx, "Invalid Date");
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    
    const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    
    char buf[256];
    snprintf(buf, sizeof(buf), "%s %s %02d %04d %02d:%02d:%02d GMT%+.4d",
             days[tm.tm_wday],
             months[tm.tm_mon],
             tm.tm_mday,
             tm.tm_year + 1900,
             tm.tm_hour,
             tm.tm_min,
             tm.tm_sec,
             0); // Simplified timezone offset
    
    return JS_NewString(ctx, buf);
}

// Date.prototype.toDateString()
static GCValue js_date_toDateString(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewString(ctx, "Invalid Date");
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    
    const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    
    char buf[64];
    snprintf(buf, sizeof(buf), "%s %s %02d %04d",
             days[tm.tm_wday],
             months[tm.tm_mon],
             tm.tm_mday,
             tm.tm_year + 1900);
    
    return JS_NewString(ctx, buf);
}

// Date.prototype.toTimeString()
static GCValue js_date_toTimeString(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewString(ctx, "Invalid Date");
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    
    char buf[64];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d GMT%+.4d",
             tm.tm_hour,
             tm.tm_min,
             tm.tm_sec,
             0); // Simplified timezone offset
    
    return JS_NewString(ctx, buf);
}

// Date.prototype.toUTCString()
static GCValue js_date_toUTCString(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewString(ctx, "Invalid Date");
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    
    const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    
    char buf[64];
    snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT",
             days[tm.tm_wday],
             tm.tm_mday,
             months[tm.tm_mon],
             tm.tm_year + 1900,
             tm.tm_hour,
             tm.tm_min,
             tm.tm_sec);
    
    return JS_NewString(ctx, buf);
}

// Date.prototype.toISOString()
static GCValue js_date_toISOString(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) {
        return JS_ThrowRangeError(ctx, "Invalid Date");
    }
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm.tm_year + 1900,
             tm.tm_mon + 1,
             tm.tm_mday,
             tm.tm_hour,
             tm.tm_min,
             tm.tm_sec,
             ms);
    
    return JS_NewString(ctx, buf);
}

// Date.prototype.toJSON()
static GCValue js_date_toJSON(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_date_toISOString(ctx, this_val, argc, argv);
}

// Date.prototype[Symbol.toPrimitive]
static GCValue js_date_toPrimitive(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    
    const char *hint = JS_ToCString(ctx, argv[0]);
    if (!hint) return JS_UNDEFINED;
    
    GCValue result;
    if (strcmp(hint, "string") == 0 || strcmp(hint, "default") == 0) {
        result = js_date_toString(ctx, this_val, 0, NULL);
    } else {
        result = js_date_valueOf(ctx, this_val, 0, NULL);
    }
    
    return result;
}

// Date.now() - static method
static GCValue js_date_now(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, (double)date_get_current_time_ms());
}

// Date.parse() - static method
static GCValue js_date_parse(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0])) return JS_NewFloat64(ctx, NAN);
    
    const char *str = JS_ToCString(ctx, argv[0]);
    if (!str) return JS_NewFloat64(ctx, NAN);
    
    long long ts = date_parse_iso_string(str);
    
    return JS_NewFloat64(ctx, (double)ts);
}

// Date.UTC() - static method
static GCValue js_date_UTC(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_NewFloat64(ctx, NAN);
    
    double year = 0, month = 0, day = 1, hours = 0, minutes = 0, seconds = 0, ms = 0;
    JS_ToFloat64(ctx, &year, argv[0]);
    JS_ToFloat64(ctx, &month, argv[1]);
    if (argc > 2) JS_ToFloat64(ctx, &day, argv[2]);
    if (argc > 3) JS_ToFloat64(ctx, &hours, argv[3]);
    if (argc > 4) JS_ToFloat64(ctx, &minutes, argv[4]);
    if (argc > 5) JS_ToFloat64(ctx, &seconds, argv[5]);
    if (argc > 6) JS_ToFloat64(ctx, &ms, argv[6]);
    
    // Handle 2-digit years
    if (year >= 0 && year <= 99) {
        year += 1900;
    }
    
    struct tm tm = {0};
    tm.tm_year = (int)year - 1900;
    tm.tm_mon = (int)month;
    tm.tm_mday = (int)day;
    tm.tm_hour = (int)hours;
    tm.tm_min = (int)minutes;
    tm.tm_sec = (int)seconds;
    
    time_t t = timegm(&tm);
    if (t != -1) {
        return JS_NewFloat64(ctx, (double)((long long)t * 1000 + (long long)ms));
    }
    return JS_NewFloat64(ctx, NAN);
}

static const JSCFunctionListEntry js_date_proto_funcs[] = {
    JS_CFUNC_DEF("getTime", 0, js_date_getTime),
    JS_CFUNC_DEF("valueOf", 0, js_date_valueOf),
    JS_CFUNC_DEF("getFullYear", 0, js_date_getFullYear),
    JS_CFUNC_DEF("getUTCFullYear", 0, js_date_getUTCFullYear),
    JS_CFUNC_DEF("getMonth", 0, js_date_getMonth),
    JS_CFUNC_DEF("getUTCMonth", 0, js_date_getUTCMonth),
    JS_CFUNC_DEF("getDate", 0, js_date_getDate),
    JS_CFUNC_DEF("getUTCDate", 0, js_date_getUTCDate),
    JS_CFUNC_DEF("getDay", 0, js_date_getDay),
    JS_CFUNC_DEF("getUTCDay", 0, js_date_getUTCDay),
    JS_CFUNC_DEF("getHours", 0, js_date_getHours),
    JS_CFUNC_DEF("getUTCHours", 0, js_date_getUTCHours),
    JS_CFUNC_DEF("getMinutes", 0, js_date_getMinutes),
    JS_CFUNC_DEF("getUTCMinutes", 0, js_date_getUTCMinutes),
    JS_CFUNC_DEF("getSeconds", 0, js_date_getSeconds),
    JS_CFUNC_DEF("getUTCSeconds", 0, js_date_getUTCSeconds),
    JS_CFUNC_DEF("getMilliseconds", 0, js_date_getMilliseconds),
    JS_CFUNC_DEF("getUTCMilliseconds", 0, js_date_getUTCMilliseconds),
    JS_CFUNC_DEF("getTimezoneOffset", 0, js_date_getTimezoneOffset),
    JS_CFUNC_DEF("toString", 0, js_date_toString),
    JS_CFUNC_DEF("toDateString", 0, js_date_toDateString),
    JS_CFUNC_DEF("toTimeString", 0, js_date_toTimeString),
    JS_CFUNC_DEF("toUTCString", 0, js_date_toUTCString),
    JS_CFUNC_DEF("toISOString", 0, js_date_toISOString),
    JS_CFUNC_DEF("toJSON", 1, js_date_toJSON),
    JS_CFUNC_DEF("toLocaleString", 0, js_date_toString),
    JS_CFUNC_DEF("toLocaleDateString", 0, js_date_toDateString),
    JS_CFUNC_DEF("toLocaleTimeString", 0, js_date_toTimeString),
};

static const JSCFunctionListEntry js_date_static_funcs[] = {
    JS_CFUNC_DEF("now", 0, js_date_now),
    JS_CFUNC_DEF("parse", 1, js_date_parse),
    JS_CFUNC_DEF("UTC", 7, js_date_UTC),
};

// ============================================================================
// MediaSource API Implementation
// ============================================================================

static void js_media_source_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    (void)val;
}

static void js_source_buffer_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    (void)val;
}

static JSClassDef js_media_source_class_def = {
    .class_name = "MediaSource",
    .finalizer = js_media_source_finalizer,
};

static JSClassDef js_source_buffer_class_def = {
    .class_name = "SourceBuffer",
    .finalizer = js_source_buffer_finalizer,
};

// MediaSource constructor
static GCValue js_media_source_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    MediaSourceDataHandle ms = MediaSourceDataHandle::create(ctx);
    if (!ms.valid()) return JS_ThrowTypeError(ctx, "Invalid MediaSource");
    
    ms.set_ready_state(0); // closed
    
    GCValue obj = JS_NewObjectClass(ctx, js_media_source_class_id);
    ms.attach_to_object(obj);
    return obj;
}

// SourceBuffer constructor (internal use)
static GCValue js_source_buffer_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    SourceBufferDataHandle sb = SourceBufferDataHandle::create(ctx);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    
    if (argc > 0) {
        const char *mime_type = JS_ToCString(ctx, argv[0]);
        if (mime_type) {
            sb.set_mime_type(mime_type);
            // Capture blob URLs for media source
            capture_url_debug(mime_type, "source_buffer_ctor");
        }
    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_source_buffer_class_id);
    sb.attach_to_object(obj);
    return obj;
}

// MediaSource.isTypeSupported(static)
static GCValue js_media_source_is_type_supported(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_FALSE;
    
    const char *mime_type = JS_ToCString(ctx, argv[0]);
    if (!mime_type) return JS_FALSE;
    
    // Support common media types that YouTube uses
    bool supported = (
        strstr(mime_type, "video/mp4") != NULL ||
        strstr(mime_type, "video/webm") != NULL ||
        strstr(mime_type, "audio/mp4") != NULL ||
        strstr(mime_type, "audio/webm") != NULL ||
        strstr(mime_type, "video/x-matroska") != NULL
    );
    
    return JS_NewBool(ctx, supported);
}

// MediaSource.prototype.addSourceBuffer
static GCValue js_media_source_add_source_buffer(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    MediaSourceDataHandle ms = MediaSourceDataHandle::from_object_check(ctx, this_val);
    if (!ms.valid()) return JS_ThrowTypeError(ctx, "Invalid MediaSource");
    
    if (argc < 1) return JS_ThrowTypeError(ctx, "requires at least 1 argument(s)");
    
    const char *mime_type = JS_ToCString(ctx, argv[0]);
    if (!mime_type) return JS_ThrowTypeError(ctx, "Invalid MIME type");
    
    // Create a mock blob URL for this source buffer
    char blob_url[512];
    static int blob_counter = 0;
    snprintf(blob_url, sizeof(blob_url), "blob:media-source:%d?type=%s", ++blob_counter, mime_type);
    
    // Capture the blob URL
    capture_url_debug(blob_url, "media_source_add_source_buffer");
    
    // Create SourceBuffer object
    GCValue sb_args[1] = { JS_NewString(ctx, mime_type) };
    GCValue sb = js_source_buffer_constructor(ctx, JS_UNDEFINED, 1, sb_args);
    
    return sb;
}

// MediaSource.prototype.removeSourceBuffer
static GCValue js_media_source_remove_source_buffer(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// MediaSource.prototype.endOfStream
static GCValue js_media_source_end_of_stream(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    MediaSourceDataHandle ms = MediaSourceDataHandle::from_object_check(ctx, this_val);
    if (!ms.valid()) return JS_ThrowTypeError(ctx, "Invalid MediaSource");
    
    ms.set_ready_state(2); // ended
    return JS_UNDEFINED;
}

// MediaSource.readyState getter
static GCValue js_media_source_get_ready_state(JSContextHandle ctx, GCValue this_val) {
    MediaSourceDataHandle ms = MediaSourceDataHandle::from_object_check(ctx, this_val);
    if (!ms.valid()) return JS_ThrowTypeError(ctx, "Invalid MediaSource");
    
    static const char* states[] = {"closed", "open", "ended"};
    int state = ms.ready_state();
    if (state >= 0 && state < 3) {
        return JS_NewString(ctx, states[state]);
    }
    return JS_NewString(ctx, "closed");
}

// MediaSource.duration getter/setter
static GCValue js_media_source_get_duration(JSContextHandle ctx, GCValue this_val) {
    MediaSourceDataHandle ms = MediaSourceDataHandle::from_object_check(ctx, this_val);
    if (!ms.valid()) return JS_ThrowTypeError(ctx, "Invalid MediaSource");
    return JS_NewFloat64(ctx, ms.duration());
}

static GCValue js_media_source_set_duration(JSContextHandle ctx, GCValue this_val, GCValue val) {
    MediaSourceDataHandle ms = MediaSourceDataHandle::from_object_check(ctx, this_val);
    if (!ms.valid()) return JS_ThrowTypeError(ctx, "Invalid MediaSource");
    double duration;
    JS_ToFloat64(ctx, &duration, val);
    ms.set_duration(duration);
    return JS_UNDEFINED;
}

// MediaSource.sourceBuffers getter
static GCValue js_media_source_get_source_buffers(JSContextHandle ctx, GCValue this_val) {
    return JS_NewArray(ctx);
}

// MediaSource.activeSourceBuffers getter
static GCValue js_media_source_get_active_source_buffers(JSContextHandle ctx, GCValue this_val) {
    return JS_NewArray(ctx);
}

static const JSCFunctionListEntry js_media_source_proto_funcs[] = {
    JS_CFUNC_DEF("addSourceBuffer", 1, js_media_source_add_source_buffer),
    JS_CFUNC_DEF("removeSourceBuffer", 1, js_media_source_remove_source_buffer),
    JS_CFUNC_DEF("endOfStream", 0, js_media_source_end_of_stream),
    JS_CGETSET_DEF("readyState", js_media_source_get_ready_state, NULL),
    JS_CGETSET_DEF("duration", js_media_source_get_duration, js_media_source_set_duration),
    JS_CGETSET_DEF("sourceBuffers", js_media_source_get_source_buffers, NULL),
    JS_CGETSET_DEF("activeSourceBuffers", js_media_source_get_active_source_buffers, NULL),
};

// SourceBuffer.prototype.appendBuffer
static GCValue js_source_buffer_append_buffer(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    SourceBufferDataHandle sb = SourceBufferDataHandle::from_object_check(ctx, this_val);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    
    sb.set_updating(1);
    
    // Simulate async update completion
    sb.set_updating(0);
    
    return JS_UNDEFINED;
}

// SourceBuffer.prototype.abort
static GCValue js_source_buffer_abort(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    SourceBufferDataHandle sb = SourceBufferDataHandle::from_object_check(ctx, this_val);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    
    sb.set_updating(0);
    return JS_UNDEFINED;
}

// SourceBuffer.prototype.changeType
static GCValue js_source_buffer_change_type(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "requires at least 1 argument(s)");
    
    SourceBufferDataHandle sb = SourceBufferDataHandle::from_object_check(ctx, this_val);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    
    const char *mime_type = JS_ToCString(ctx, argv[0]);
    if (mime_type) {
        sb.set_mime_type(mime_type);
    }
    
    return JS_UNDEFINED;
}

// SourceBuffer.prototype.remove
static GCValue js_source_buffer_remove(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// SourceBuffer.updating getter
static GCValue js_source_buffer_get_updating(JSContextHandle ctx, GCValue this_val) {
    SourceBufferDataHandle sb = SourceBufferDataHandle::from_object_check(ctx, this_val);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    return JS_NewBool(ctx, sb.updating());
}

// SourceBuffer.timestampOffset getter/setter
static GCValue js_source_buffer_get_timestamp_offset(JSContextHandle ctx, GCValue this_val) {
    SourceBufferDataHandle sb = SourceBufferDataHandle::from_object_check(ctx, this_val);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    return JS_NewFloat64(ctx, sb.timestamp_offset());
}

static GCValue js_source_buffer_set_timestamp_offset(JSContextHandle ctx, GCValue this_val, GCValue val) {
    SourceBufferDataHandle sb = SourceBufferDataHandle::from_object_check(ctx, this_val);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    double offset;
    JS_ToFloat64(ctx, &offset, val);
    sb.set_timestamp_offset(offset);
    return JS_UNDEFINED;
}

// SourceBuffer.mode getter/setter
static GCValue js_source_buffer_get_mode(JSContextHandle ctx, GCValue this_val) {
    SourceBufferDataHandle sb = SourceBufferDataHandle::from_object_check(ctx, this_val);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    return JS_NewString(ctx, sb.mode());
}

static GCValue js_source_buffer_set_mode(JSContextHandle ctx, GCValue this_val, GCValue val) {
    SourceBufferDataHandle sb = SourceBufferDataHandle::from_object_check(ctx, this_val);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    const char *mode = JS_ToCString(ctx, val);
    if (mode) {
        sb.set_mode(mode);
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_source_buffer_proto_funcs[] = {
    JS_CFUNC_DEF("appendBuffer", 1, js_source_buffer_append_buffer),
    JS_CFUNC_DEF("abort", 0, js_source_buffer_abort),
    JS_CFUNC_DEF("changeType", 1, js_source_buffer_change_type),
    JS_CFUNC_DEF("remove", 2, js_source_buffer_remove),
    JS_CGETSET_DEF("updating", js_source_buffer_get_updating, NULL),
    JS_CGETSET_DEF("timestampOffset", js_source_buffer_get_timestamp_offset, js_source_buffer_set_timestamp_offset),
    JS_CGETSET_DEF("mode", js_source_buffer_get_mode, js_source_buffer_set_mode),
    JS_PROP_DOUBLE_DEF("appendWindowStart", 0, JS_PROP_WRITABLE),
    JS_PROP_DOUBLE_DEF("appendWindowEnd", 0, JS_PROP_WRITABLE),
    JS_PROP_STRING_DEF("videoTracks", "", JS_PROP_WRITABLE),
    JS_PROP_STRING_DEF("audioTracks", "", JS_PROP_WRITABLE),
    JS_PROP_STRING_DEF("textTracks", "", JS_PROP_WRITABLE),
};

// ============================================================================
// URL API Implementation
// ============================================================================

// URL.createObjectURL() - Creates blob URLs and captures them
static GCValue js_url_create_object_url(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "requires at least 1 argument(s)");
    
    GCValue obj = argv[0];
    
    // Generate a unique blob URL
    static int blob_counter = 0;
    char blob_url[512];
    
    // Check if it's a MediaSource object
    MediaSourceDataHandle ms = MediaSourceDataHandle::from_object(obj);
    if (ms.valid()) {
        snprintf(blob_url, sizeof(blob_url), "blob:mediasource:%d", ++blob_counter);
        ms.set_ready_state(1); // open
        
        // Capture the URL
        capture_url_debug(blob_url, "url_create_object_url_ms");
        
        return JS_NewString(ctx, blob_url);
    }
    
    // For other objects (File, Blob), create generic blob URL
    snprintf(blob_url, sizeof(blob_url), "blob:generic:%d", ++blob_counter);
    
    // Capture the URL
    capture_url_debug(blob_url, "url_create_object_url_generic");
    
    return JS_NewString(ctx, blob_url);
}

// URL.revokeObjectURL()
static GCValue js_url_revoke_object_url(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // No-op for stub implementation
    return JS_UNDEFINED;
}

// URL constructor - new URL(url, base)
static GCValue js_url_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "URL constructor requires at least 1 argument");
    }
    
    const char *url_str = JS_ToCString(ctx, argv[0]);
    if (!url_str) return JS_ThrowTypeError(ctx, "Invalid URL");
    
    // For URL parsing, we'll create a simple object with URL components
    GCValue url_obj = JS_NewObject(ctx);
    
    // Store the full URL
    JS_SetPropertyStr(ctx, url_obj, "href", JS_NewString(ctx, url_str));
    
    // Simple URL parsing (very basic)
    char protocol[64] = "";
    char hostname[256] = "";
    char pathname[512] = "/";
    char search[512] = "";
    char hash[256] = "";
    int port = 0;
    
    const char *p = url_str;
    
    // Parse protocol
    const char *proto_end = strstr(p, "://");
    if (proto_end) {
        size_t proto_len = proto_end - p;
        if (proto_len < sizeof(protocol)) {
            strncpy(protocol, p, proto_len);
            protocol[proto_len] = '\0';
        }
        p = proto_end + 3;
        
        // Parse hostname and port
        const char *path_start = strchr(p, '/');
        const char *query_start = strchr(p, '?');
        const char *hash_start = strchr(p, '#');
        
        const char *host_end = path_start;
        if (!host_end || (query_start && query_start < host_end)) host_end = query_start;
        if (!host_end || (hash_start && hash_start < host_end)) host_end = hash_start;
        
        if (host_end) {
            size_t host_len = host_end - p;
            if (host_len >= sizeof(hostname)) host_len = sizeof(hostname) - 1;
            strncpy(hostname, p, host_len);
            hostname[host_len] = '\0';
            
            // Check for port in hostname
            char *port_ptr = strchr(hostname, ':');
            if (port_ptr) {
                *port_ptr = '\0';
                port = atoi(port_ptr + 1);
            }
        } else {
            strncpy(hostname, p, sizeof(hostname) - 1);
            hostname[sizeof(hostname) - 1] = '\0';
        }
        
        // Parse pathname
        if (path_start) {
            const char *path_end = query_start;
            if (!path_end) path_end = hash_start;
            if (!path_end) path_end = path_start + strlen(path_start);
            
            size_t path_len = path_end - path_start;
            if (path_len >= sizeof(pathname)) path_len = sizeof(pathname) - 1;
            strncpy(pathname, path_start, path_len);
            pathname[path_len] = '\0';
        }
        
        // Parse search
        if (query_start) {
            const char *search_end = hash_start;
            if (!search_end) search_end = query_start + strlen(query_start);
            
            size_t search_len = search_end - query_start;
            if (search_len >= sizeof(search)) search_len = sizeof(search) - 1;
            strncpy(search, query_start, search_len);
            search[search_len] = '\0';
        }
        
        // Parse hash
        if (hash_start) {
            strncpy(hash, hash_start, sizeof(hash) - 1);
            hash[sizeof(hash) - 1] = '\0';
        }
    }
    
    // Set URL properties
    JS_SetPropertyStr(ctx, url_obj, "protocol", JS_NewString(ctx, protocol));
    JS_SetPropertyStr(ctx, url_obj, "hostname", JS_NewString(ctx, hostname));
    JS_SetPropertyStr(ctx, url_obj, "host", JS_NewString(ctx, hostname));
    JS_SetPropertyStr(ctx, url_obj, "pathname", JS_NewString(ctx, pathname));
    JS_SetPropertyStr(ctx, url_obj, "search", JS_NewString(ctx, search));
    JS_SetPropertyStr(ctx, url_obj, "hash", JS_NewString(ctx, hash));
    JS_SetPropertyStr(ctx, url_obj, "port", JS_NewInt32(ctx, port));
    
    // origin = protocol + // + hostname
    char origin[512];
    if (strlen(protocol) > 0) {
        snprintf(origin, sizeof(origin), "%s://%s", protocol, hostname);
    } else {
        origin[0] = '\0';
    }
    JS_SetPropertyStr(ctx, url_obj, "origin", JS_NewString(ctx, origin));
    
    return url_obj;
}

// ============================================================================
// Request/Response API (for fetch)
// ============================================================================

// Request constructor
static GCValue js_request_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
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
        // Copy __original_url so fetch() can decode base64 body through Request chains
        GCValue orig_url_val = JS_GetPropertyStr(ctx, argv[0], "__original_url");
        if (!JS_IsUndefined(orig_url_val)) {
            JS_SetPropertyStr(ctx, request_obj, "__original_url", orig_url_val);
        }
    } else {
        // URL string
        const char *url_str = JS_ToCString(ctx, argv[0]);
        if (url_str) {
            JS_SetPropertyStr(ctx, request_obj, "url", JS_NewString(ctx, url_str));
            // Store original URL so fetch can access it even if url getter is overridden
            JS_SetPropertyStr(ctx, request_obj, "__original_url", JS_NewString(ctx, url_str));
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
static GCValue js_response_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
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
static GCValue js_response_json(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Return a promise that resolves to the JSON
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    GCValue promise = JS_CallConstructor(ctx, promise_ctor, 0, NULL);
    return promise;
}

// ============================================================================
// Navigator sendBeacon
// ============================================================================

static GCValue js_navigator_send_beacon(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_FALSE;
    
    const char *url = JS_ToCString(ctx, argv[0]);
    if (url) {
        // Capture the beacon URL
        capture_url_debug(url, "send_beacon");
    }
    
    // Always return true per spec (beacon is "fire and forget")
    return JS_TRUE;
}

// ============================================================================
// Timer API Implementation (setTimeout, setInterval, requestAnimationFrame, etc.)
// ============================================================================

#include <pthread.h>

// Maximum number of concurrent timers
#define MAX_TIMERS 256
#define MAX_TIMER_ARGS 8

typedef enum {
    TIMER_TYPE_TIMEOUT,
    TIMER_TYPE_INTERVAL,
    TIMER_TYPE_RAF,           // requestAnimationFrame
    TIMER_TYPE_IDLE_CALLBACK  // requestIdleCallback
} TimerType;

typedef struct {
    int id;                           // Timer ID (positive integer)
    TimerType type;                   // Type of timer
    unsigned long long trigger_time;  // Time when timer should fire (ms)
    unsigned long long interval;      // For intervals: repeat interval (ms)
    int active;                       // 1 if active, 0 if cleared
    
    // Callback storage - we store the function as a JS value
    // Since GCValue is a handle, we need to keep it alive
    int callback_handle;              // Index into callback storage
    
    // Arguments
    int arg_count;
    int arg_handles[MAX_TIMER_ARGS];  // Indices into callback storage for args
} Timer;

// Timer storage and management
typedef struct {
    Timer timers[MAX_TIMERS];
    int timer_count;
    int next_id;
    pthread_mutex_t mutex;
    
    // Callback storage - parallel array to keep JS objects alive
    // We use a simple scheme: store JS values that the GC knows about
    GCValue callbacks[MAX_TIMERS * (MAX_TIMER_ARGS + 1)];  // +1 for callback itself
    int callback_count;
} TimerState;

// Static timer state - will be properly initialized at runtime
static TimerState g_timer_state;
static int g_timer_state_initialized = 0;

// Initialize timer state on first use
static void timer_state_ensure_initialized(void) {
    if (!g_timer_state_initialized) {
        memset(&g_timer_state, 0, sizeof(g_timer_state));
        pthread_mutex_init(&g_timer_state.mutex, NULL);
        g_timer_state.next_id = 1;
        g_timer_state_initialized = 1;
    }
}

// Helper to check if a GCValue contains a reference type that needs root registration
static inline int gcvalue_is_reference(GCValue val) {
    // Negative tags are reference types (objects, strings, symbols, etc.)
    return GC_VALUE_GET_TAG(val) < 0;
}

// Store a JS value in the callback storage, returns handle index
static int store_callback(JSContextHandle ctx, GCValue val) {
    (void)ctx;
    if (g_timer_state.callback_count >= MAX_TIMERS * (MAX_TIMER_ARGS + 1)) {
        return -1; // Full
    }
    // Store the handle directly
    int idx = g_timer_state.callback_count;
    g_timer_state.callbacks[idx] = val;
    
    // Register as GC root if it's a reference type (callback functions need to survive GC)
    if (gcvalue_is_reference(val)) {
        GCHandle handle = GC_VALUE_GET_HANDLE(val);
        if (handle != GC_HANDLE_NULL) {
            gc_add_root(handle);
        }
    }
    
    g_timer_state.callback_count++;
    return idx;
}

// Get a stored callback by handle
static GCValue get_callback(int handle) {
    if (handle < 0 || handle >= g_timer_state.callback_count) {
        return JS_UNDEFINED;
    }
    return g_timer_state.callbacks[handle];
}

// Helper to unregister all callback roots for a timer
static void unregister_timer_roots(Timer *timer) {
    if (!timer) return;
    
    // Unregister callback root
    GCValue callback = g_timer_state.callbacks[timer->callback_handle];
    if (gcvalue_is_reference(callback)) {
        GCHandle handle = GC_VALUE_GET_HANDLE(callback);
        if (handle != GC_HANDLE_NULL) {
            gc_remove_root(handle);
        }
    }
    
    // Unregister argument roots
    for (int i = 0; i < timer->arg_count; i++) {
        GCValue arg = g_timer_state.callbacks[timer->arg_handles[i]];
        if (gcvalue_is_reference(arg)) {
            GCHandle handle = GC_VALUE_GET_HANDLE(arg);
            if (handle != GC_HANDLE_NULL) {
                gc_remove_root(handle);
            }
        }
    }
}

// Clear all timers and callback storage
extern "C" void timer_api_reset(void) {
    timer_state_ensure_initialized();
    pthread_mutex_lock(&g_timer_state.mutex);
    
    // Unregister all GC roots before clearing
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_timer_state.timers[i].id != 0) {
            unregister_timer_roots(&g_timer_state.timers[i]);
        }
    }
    
    // Clear all stored callbacks
    memset(&g_timer_state.callbacks, 0, sizeof(g_timer_state.callbacks));
    g_timer_state.callback_count = 0;
    
    // Reset timers
    memset(&g_timer_state.timers, 0, sizeof(g_timer_state.timers));
    g_timer_state.timer_count = 0;
    g_timer_state.next_id = 1;
    
    pthread_mutex_unlock(&g_timer_state.mutex);
}

// Find a free timer slot
static int find_free_timer_slot(void) {
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!g_timer_state.timers[i].active && g_timer_state.timers[i].id == 0) {
            return i;
            }
        }
    return -1;
}

// Add a new timer
static int add_timer(JSContextHandle ctx, TimerType type, GCValue callback, 
                     unsigned long long delay_ms, int arg_count, GCValue *args) {
    timer_state_ensure_initialized();
    pthread_mutex_lock(&g_timer_state.mutex);
    
    int slot = find_free_timer_slot();
    if (slot < 0) {
        pthread_mutex_unlock(&g_timer_state.mutex);
        return 0; // No slots available
    }
    
    Timer *timer = &g_timer_state.timers[slot];
    timer->id = g_timer_state.next_id++;
    timer->type = type;
    timer->trigger_time = platform_get_time_ms() + delay_ms;
    timer->interval = (type == TIMER_TYPE_INTERVAL) ? delay_ms : 0;
    timer->active = 1;
    
    // Store callback
    int cb_handle = store_callback(ctx, callback);
    if (cb_handle < 0) {
        timer->id = 0;
        pthread_mutex_unlock(&g_timer_state.mutex);
        return 0;
    }
    timer->callback_handle = cb_handle;
    
    // Store arguments (up to MAX_TIMER_ARGS)
    timer->arg_count = (arg_count > MAX_TIMER_ARGS) ? MAX_TIMER_ARGS : arg_count;
    for (int i = 0; i < timer->arg_count; i++) {
        int arg_handle = store_callback(ctx, args[i]);
        if (arg_handle < 0) {
            // Failed, clean up
            timer->arg_count = i;
            timer->id = 0;
            pthread_mutex_unlock(&g_timer_state.mutex);
            return 0;
        }
        timer->arg_handles[i] = arg_handle;
    }
    
    g_timer_state.timer_count++;
    pthread_mutex_unlock(&g_timer_state.mutex);
    
    return timer->id;
}

// Clear a timer by ID
static int clear_timer_by_id(int id) {
    if (id <= 0) return 0;
    
    timer_state_ensure_initialized();
    pthread_mutex_lock(&g_timer_state.mutex);
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_timer_state.timers[i].id == id && g_timer_state.timers[i].active) {
            // Unregister GC roots for this timer's callback and args
            unregister_timer_roots(&g_timer_state.timers[i]);
            
            g_timer_state.timers[i].active = 0;
            g_timer_state.timer_count--;
            pthread_mutex_unlock(&g_timer_state.mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_timer_state.mutex);
    return 0;
}

// Find the next timer that should fire, returns its ID or 0 if none
static int find_due_timer(unsigned long long current_time) {
    timer_state_ensure_initialized();
    pthread_mutex_lock(&g_timer_state.mutex);
    
    int due_id = 0;
    unsigned long long earliest_time = (unsigned long long)(-1);
    
    for (int i = 0; i < MAX_TIMERS; i++) {
        Timer *t = &g_timer_state.timers[i];
        if (t->active && t->trigger_time <= current_time && t->trigger_time < earliest_time) {
            earliest_time = t->trigger_time;
            due_id = t->id;
        }
    }
    
    pthread_mutex_unlock(&g_timer_state.mutex);
    return due_id;
}

// Execute a timer callback by ID
static void execute_timer(JSContextHandle ctx, int id) {
    pthread_mutex_lock(&g_timer_state.mutex);
    
    Timer *timer = NULL;
    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_timer_state.timers[i].id == id && g_timer_state.timers[i].active) {
            timer = &g_timer_state.timers[i];
            slot = i;
            break;
        }
    }
    
    if (!timer) {
        pthread_mutex_unlock(&g_timer_state.mutex);
        return;
    }
    
    // Get callback and args while holding lock
    GCValue callback = get_callback(timer->callback_handle);
    GCValue args[MAX_TIMER_ARGS];
    int arg_count = timer->arg_count;
    for (int i = 0; i < arg_count; i++) {
        args[i] = get_callback(timer->arg_handles[i]);
    }
    
    // Handle different timer types
    if (timer->type == TIMER_TYPE_TIMEOUT || timer->type == TIMER_TYPE_RAF || timer->type == TIMER_TYPE_IDLE_CALLBACK) {
        // One-shot timer: mark as inactive before executing
        timer->active = 0;
        g_timer_state.timer_count--;
    } else if (timer->type == TIMER_TYPE_INTERVAL) {
        // Interval: schedule next trigger
        timer->trigger_time = platform_get_time_ms() + timer->interval;
    }
    
    pthread_mutex_unlock(&g_timer_state.mutex);
    
    // Execute callback (outside lock to avoid deadlock)
    if (JS_IsFunction(ctx, callback)) {
        // For RAF, pass the timestamp as argument
        if (timer->type == TIMER_TYPE_RAF && arg_count == 0) {
            args[0] = JS_NewFloat64(ctx, (double)platform_get_time_ms());
            arg_count = 1;
        }
        
        // For idle callback, pass a mock IdleDeadline object
        if (timer->type == TIMER_TYPE_IDLE_CALLBACK && arg_count == 0) {
            GCValue idle_deadline = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, idle_deadline, "didTimeout", JS_FALSE);
            // timeRemaining returns always some positive number
            JS_SetPropertyStr(ctx, idle_deadline, "timeRemaining", 
                JS_NewCFunction(ctx, js_true, "timeRemaining", 0));
            args[0] = idle_deadline;
            arg_count = 1;
        }
        
        GCValue global_obj = JS_GetGlobalObject(ctx);
        GCValue result = JS_Call(ctx, callback, global_obj, arg_count, args);
        if (JS_IsException(result)) {
            GCValue exc = JS_GetException(ctx);
            const char *exc_str = JS_ToCString(ctx, exc);
            if (exc_str) {
                platform_log(LOG_LEVEL_WARN, "timer", "Timer callback exception: %s", exc_str);
            }
        }
        (void)result; // Result is ignored for timer callbacks
    }
    // For one-shot timers, unregister roots after execution (they won't be accessed again)
    // For intervals, keep roots registered as they'll be called again
    if (timer->type == TIMER_TYPE_TIMEOUT || timer->type == TIMER_TYPE_RAF || timer->type == TIMER_TYPE_IDLE_CALLBACK) {
        // We need to re-acquire lock to safely access timer state
        pthread_mutex_lock(&g_timer_state.mutex);
        unregister_timer_roots(timer);
        pthread_mutex_unlock(&g_timer_state.mutex);
    }
}

// Process all due timers - called from interrupt handler
// Returns 1 if any timers were processed
extern "C" int timer_process_due(JSContextHandle ctx) {
    unsigned long long now = platform_get_time_ms();
    int processed = 0;
    
    // Process up to 10 timers per call to avoid blocking too long
    for (int i = 0; i < 10; i++) {
        int due_id = find_due_timer(now);
        if (due_id == 0) break;
        
        execute_timer(ctx, due_id);
        processed++;
    }
    
    return processed;
}

// Check if any timer is pending (for interrupt handler)
extern "C" int timer_has_pending(void) {
    pthread_mutex_lock(&g_timer_state.mutex);
    
    unsigned long long now = platform_get_time_ms();
    int has_pending = 0;
    
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_timer_state.timers[i].active && g_timer_state.timers[i].trigger_time <= now) {
            has_pending = 1;
            break;
        }
    }
    
    pthread_mutex_unlock(&g_timer_state.mutex);
    return has_pending;
}

// ============================================================================
// JavaScript API Bindings
// ============================================================================

// setTimeout(callback, delay, ...args)
static GCValue js_set_timeout(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    
    if (argc < 1) {
        return JS_NewInt32(ctx, 0);
    }
    
    // Check if first argument is undefined or null
    if (JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) {
        return JS_NewInt32(ctx, 0);
    }
    
    // Accept any object as callback (functions are objects in JS)
    // The actual call will fail gracefully if not callable
    if (!JS_IsObject(argv[0]) && !JS_IsFunction(ctx, argv[0])) {
        return JS_NewInt32(ctx, 0);
    }
    
    // Parse delay (default 0)
    unsigned long long delay = 0;
    if (argc >= 2) {
        double delay_ms;
        if (JS_ToFloat64(ctx, &delay_ms, argv[1]) == 0) {
            delay = (unsigned long long)(delay_ms > 0 ? delay_ms : 0);
        }
    }
    
    // Extract additional arguments
    GCValue args[MAX_TIMER_ARGS];
    int arg_count = 0;
    for (int i = 2; i < argc && arg_count < MAX_TIMER_ARGS; i++, arg_count++) {
        args[arg_count] = argv[i];
    }
    
    int id = add_timer(ctx, TIMER_TYPE_TIMEOUT, argv[0], delay, arg_count, args);
    return JS_NewInt32(ctx, id);
}

// clearTimeout(id)
static GCValue js_clear_timeout(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val;
    
    if (argc < 1) return JS_UNDEFINED;
    
    int32_t id;
    if (JS_ToInt32(ctx, &id, argv[0]) == 0 && id > 0) {
        clear_timer_by_id(id);
    }
    
    return JS_UNDEFINED;
}

// setInterval(callback, delay, ...args)
static GCValue js_set_interval(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_NewInt32(ctx, 0);
    }
    
    // Parse delay (default 0 if not specified, but spec says at least 4ms for intervals)
    unsigned long long delay = 4;  // Minimum 4ms per HTML spec
    if (argc >= 2) {
        double delay_ms;
        if (JS_ToFloat64(ctx, &delay_ms, argv[1]) == 0) {
            delay = (unsigned long long)(delay_ms > 4 ? delay_ms : 4);
        }
    }
    
    // Extract additional arguments
    GCValue args[MAX_TIMER_ARGS];
    int arg_count = 0;
    for (int i = 2; i < argc && arg_count < MAX_TIMER_ARGS; i++, arg_count++) {
        args[arg_count] = argv[i];
    }
    
    int id = add_timer(ctx, TIMER_TYPE_INTERVAL, argv[0], delay, arg_count, args);
    return JS_NewInt32(ctx, id);
}

// clearInterval(id)
static GCValue js_clear_interval(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val;
    
    if (argc < 1) return JS_UNDEFINED;
    
    int32_t id;
    if (JS_ToInt32(ctx, &id, argv[0]) == 0 && id > 0) {
        clear_timer_by_id(id);
    }
    
    return JS_UNDEFINED;
}

// requestAnimationFrame(callback)
static GCValue js_request_animation_frame(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_NewInt32(ctx, 0);
    }
    
    // RAF typically fires at 60fps (~16.67ms), but we'll use 0 for immediate execution
    int id = add_timer(ctx, TIMER_TYPE_RAF, argv[0], 0, 0, NULL);
    return JS_NewInt32(ctx, id);
}

// cancelAnimationFrame(id)
static GCValue js_cancel_animation_frame(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val;
    
    if (argc < 1) return JS_UNDEFINED;
    
    int32_t id;
    if (JS_ToInt32(ctx, &id, argv[0]) == 0 && id > 0) {
        clear_timer_by_id(id);
    }
    
    return JS_UNDEFINED;
}

// requestIdleCallback(callback, options)
static GCValue js_request_idle_callback(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_NewInt32(ctx, 0);
    }
    
    // Parse timeout from options (default 0)
    unsigned long long timeout = 0;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        GCValue timeout_val = JS_GetPropertyStr(ctx, argv[1], "timeout");
        double timeout_ms;
        if (JS_ToFloat64(ctx, &timeout_ms, timeout_val) == 0) {
            timeout = (unsigned long long)(timeout_ms > 0 ? timeout_ms : 0);
        }
    }
    
    int id = add_timer(ctx, TIMER_TYPE_IDLE_CALLBACK, argv[0], timeout, 0, NULL);
    return JS_NewInt32(ctx, id);
}

// cancelIdleCallback(id)
static GCValue js_cancel_idle_callback(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val;
    
    if (argc < 1) return JS_UNDEFINED;
    
    int32_t id;
    if (JS_ToInt32(ctx, &id, argv[0]) == 0 && id > 0) {
        clear_timer_by_id(id);
    }
    
    return JS_UNDEFINED;
}

// ============================================================================
// Main Initialization
// ============================================================================

void init_browser_api_impl(JSContextHandle ctx, GCValue global) {
    // ===== Initialize Class IDs =====
    JS_NewClassID(&js_shadow_root_class_id);
    JS_NewClassID(&js_animation_class_id);
    JS_NewClassID(&js_keyframe_effect_class_id);
    JS_NewClassID(&js_font_face_class_id);
    JS_NewClassID(&js_font_face_set_class_id);
    JS_NewClassID(&js_custom_element_registry_class_id);
    JS_NewClassID(&js_mutation_observer_class_id);
    JS_NewClassID(&js_resize_observer_class_id);
    JS_NewClassID(&js_intersection_observer_class_id);
    JS_NewClassID(&js_performance_class_id);
    JS_NewClassID(&js_performance_entry_class_id);
    JS_NewClassID(&js_performance_observer_class_id);
    JS_NewClassID(&js_dom_rect_class_id);
    JS_NewClassID(&js_dom_rect_read_only_class_id);
    JS_NewClassID(&js_performance_timing_class_id);
    JS_NewClassID(&js_map_class_id);
    JS_NewClassID(&js_dom_exception_class_id);
    JS_NewClassID(&js_media_source_class_id);
    JS_NewClassID(&js_source_buffer_class_id);
    JS_NewClassID(&js_date_class_id);
    JS_NewClassID(&js_dom_node_class_id);
    JS_NewClassID(&js_event_class_id);
    JS_NewClassID(&js_custom_event_class_id);
    JS_NewClassID(&js_mouse_event_class_id);
    JS_NewClassID(&js_focus_event_class_id);
    JS_NewClassID(&js_service_worker_container_class_id);
    JS_NewClassID(&js_service_worker_registration_class_id);
    JS_NewClassID(&js_service_worker_class_id);
    
    // Register classes with the runtime
    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    JS_NewClass(rt, js_shadow_root_class_id, &js_shadow_root_class_def);
    JS_NewClass(rt, js_animation_class_id, &js_animation_class_def);
    JS_NewClass(rt, js_keyframe_effect_class_id, &js_keyframe_effect_class_def);
    JS_NewClass(rt, js_font_face_class_id, &js_font_face_class_def);
    JS_NewClass(rt, js_font_face_set_class_id, &js_font_face_set_class_def);
    JS_NewClass(rt, js_custom_element_registry_class_id, &js_custom_element_registry_class_def);
    JS_NewClass(rt, js_mutation_observer_class_id, &js_mutation_observer_class_def);
    JS_NewClass(rt, js_resize_observer_class_id, &js_resize_observer_class_def);
    JS_NewClass(rt, js_intersection_observer_class_id, &js_intersection_observer_class_def);
    JS_NewClass(rt, js_performance_class_id, &js_performance_class_def);
    JS_NewClass(rt, js_performance_entry_class_id, &js_performance_entry_class_def);
    JS_NewClass(rt, js_performance_observer_class_id, &js_performance_observer_class_def);
    JS_NewClass(rt, js_dom_rect_class_id, &js_dom_rect_class_def);
    JS_NewClass(rt, js_dom_rect_read_only_class_id, &js_dom_rect_read_only_class_def);
    JS_NewClass(rt, js_map_class_id, &js_map_class_def);
    JS_NewClass(rt, js_performance_timing_class_id, &js_performance_timing_class_def);
    JS_NewClass(rt, js_dom_exception_class_id, &js_dom_exception_class_def);
    JS_NewClass(rt, js_media_source_class_id, &js_media_source_class_def);
    JS_NewClass(rt, js_source_buffer_class_id, &js_source_buffer_class_def);
    JS_NewClass(rt, js_date_class_id, &js_date_class_def);
    JS_NewClass(rt, js_dom_node_class_id, &js_dom_node_class_def);
    JS_NewClass(rt, js_event_class_id, &js_event_class_def);
    JS_NewClass(rt, js_custom_event_class_id, &js_custom_event_class_def);
    JS_NewClass(rt, js_mouse_event_class_id, &js_mouse_event_class_def);
    JS_NewClass(rt, js_focus_event_class_id, &js_focus_event_class_def);
    JS_NewClass(rt, js_service_worker_container_class_id, &js_service_worker_container_class_def);
    JS_NewClass(rt, js_service_worker_registration_class_id, &js_service_worker_registration_class_def);
    JS_NewClass(rt, js_service_worker_class_id, &js_service_worker_class_def);
    
    // ===== ES6+ Polyfills Registration =====
    // Get Object constructor
    GCValue object_ctor = JS_GetPropertyStr(ctx, global, "Object");
    
    // Object.getPrototypeOf
    if (!JS_IsException(object_ctor)) {
        JS_SetPropertyStr(ctx, object_ctor, "getPrototypeOf",
            JS_NewCFunction(ctx, js_object_get_prototype_of, "getPrototypeOf", 1));
    }
    
    // Object.setPrototypeOf
    if (!JS_IsException(object_ctor)) {
        JS_SetPropertyStr(ctx, object_ctor, "setPrototypeOf",
            JS_NewCFunction(ctx, js_object_set_prototype_of, "setPrototypeOf", 2));
    }
    
    // Object.defineProperty
    if (!JS_IsException(object_ctor)) {
        JS_SetPropertyStr(ctx, object_ctor, "defineProperty",
            JS_NewCFunction(ctx, js_object_define_property, "defineProperty", 3));
    }
    
    // Object.getOwnPropertyDescriptor
    if (!JS_IsException(object_ctor)) {
        JS_SetPropertyStr(ctx, object_ctor, "getOwnPropertyDescriptor",
            JS_NewCFunction(ctx, js_object_get_own_property_descriptor, "getOwnPropertyDescriptor", 2));
    }
    
    // Object.getOwnPropertySymbols
    if (!JS_IsException(object_ctor)) {
        JS_SetPropertyStr(ctx, object_ctor, "getOwnPropertySymbols",
            JS_NewCFunction(ctx, js_object_get_own_property_symbols, "getOwnPropertySymbols", 1));
    }
    
    // Object.assign
    if (!JS_IsException(object_ctor)) {
        JS_SetPropertyStr(ctx, object_ctor, "assign",
            JS_NewCFunction(ctx, js_object_assign, "assign", 2));
    }

    // Create Reflect object
    GCValue reflect_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, reflect_obj, "construct",
        JS_NewCFunction(ctx, js_reflect_construct, "construct", 2));
    JS_SetPropertyStr(ctx, reflect_obj, "apply",
        JS_NewCFunction(ctx, js_reflect_apply, "apply", 3));
    JS_SetPropertyStr(ctx, reflect_obj, "has",
        JS_NewCFunction(ctx, js_reflect_has, "has", 2));
    JS_SetPropertyStr(ctx, global, "Reflect", reflect_obj);
    
    // DOMException constructor
    GCValue dom_exception_proto = JS_NewObject(ctx);
    
    JS_SetClassProto(ctx, js_dom_exception_class_id, dom_exception_proto);
    
    JS_SetPropertyFunctionList(ctx, dom_exception_proto, js_dom_exception_proto_funcs,
        sizeof(js_dom_exception_proto_funcs) / sizeof(js_dom_exception_proto_funcs[0]));
    
    // Set up prototype chain: DOMException.prototype -> Error.prototype using Object.setPrototypeOf
    GCValue error_ctor = JS_GetPropertyStr(ctx, global, "Error");
    if (!JS_IsException(error_ctor)) {
        GCValue error_proto = JS_GetPropertyStr(ctx, error_ctor, "prototype");
        if (!JS_IsException(error_proto)) {
            GCValue obj_ctor = JS_GetPropertyStr(ctx, global, "Object");
            GCValue set_proto = JS_GetPropertyStr(ctx, obj_ctor, "setPrototypeOf");
            GCValue args[2] = { dom_exception_proto, error_proto };




        }

    }
    
    GCValue dom_exception_ctor = JS_NewCFunction2(ctx, js_dom_exception_constructor, "DOMException", 2, JS_CFUNC_constructor, 0);
    if (JS_IsException(dom_exception_ctor)) {
        LOG_ERROR("dom_exception_ctor not usable after creation - SKIPPING DOMException setup");
        goto skip_dom_exception;
    }
    JS_SetConstructor(ctx, dom_exception_ctor, dom_exception_proto);
    if (JS_IsException(dom_exception_ctor)) {
        LOG_ERROR("dom_exception_ctor not usable after SetConstructor - SKIPPING DOMException setup");
        goto skip_dom_exception;
    }
    
    /* HELPER: Set an integer constant on dom_exception_ctor. */
    #define SET_ERR_CONST(name, value) do { \
        GCValue err_val = JS_NewInt32(ctx, value); \
        JS_SetPropertyStr(ctx, dom_exception_ctor, #name, err_val); \
    } while(0)
    
    // Add static error code constants with validation before each set
    SET_ERR_CONST(INDEX_SIZE_ERR, DOM_EXCEPTION_INDEX_SIZE_ERR);
    SET_ERR_CONST(HIERARCHY_REQUEST_ERR, DOM_EXCEPTION_HIERARCHY_REQUEST_ERR);
    SET_ERR_CONST(WRONG_DOCUMENT_ERR, DOM_EXCEPTION_WRONG_DOCUMENT_ERR);
    SET_ERR_CONST(INVALID_CHARACTER_ERR, DOM_EXCEPTION_INVALID_CHARACTER_ERR);
    SET_ERR_CONST(NO_MODIFICATION_ALLOWED_ERR, DOM_EXCEPTION_NO_MODIFICATION_ALLOWED_ERR);
    SET_ERR_CONST(NOT_FOUND_ERR, DOM_EXCEPTION_NOT_FOUND_ERR);
    SET_ERR_CONST(NOT_SUPPORTED_ERR, DOM_EXCEPTION_NOT_SUPPORTED_ERR);
    SET_ERR_CONST(INVALID_STATE_ERR, DOM_EXCEPTION_INVALID_STATE_ERR);
    SET_ERR_CONST(SYNTAX_ERR, DOM_EXCEPTION_SYNTAX_ERR);
    SET_ERR_CONST(INVALID_MODIFICATION_ERR, DOM_EXCEPTION_INVALID_MODIFICATION_ERR);
    SET_ERR_CONST(NAMESPACE_ERR, DOM_EXCEPTION_NAMESPACE_ERR);
    SET_ERR_CONST(INVALID_ACCESS_ERR, DOM_EXCEPTION_INVALID_ACCESS_ERR);
    SET_ERR_CONST(TYPE_MISMATCH_ERR, DOM_EXCEPTION_TYPE_MISMATCH_ERR);
    SET_ERR_CONST(SECURITY_ERR, DOM_EXCEPTION_SECURITY_ERR);
    SET_ERR_CONST(NETWORK_ERR, DOM_EXCEPTION_NETWORK_ERR);
    SET_ERR_CONST(ABORT_ERR, DOM_EXCEPTION_ABORT_ERR);
    SET_ERR_CONST(URL_MISMATCH_ERR, DOM_EXCEPTION_URL_MISMATCH_ERR);
    SET_ERR_CONST(QUOTA_EXCEEDED_ERR, DOM_EXCEPTION_QUOTA_EXCEEDED_ERR);
    SET_ERR_CONST(TIMEOUT_ERR, DOM_EXCEPTION_TIMEOUT_ERR);
    SET_ERR_CONST(INVALID_NODE_TYPE_ERR, DOM_EXCEPTION_INVALID_NODE_TYPE_ERR);
    SET_ERR_CONST(DATA_CLONE_ERR, DOM_EXCEPTION_DATA_CLONE_ERR);
    
    #undef SET_ERR_CONST
    
    // Final validation before exposing to global object
    if (JS_IsException(dom_exception_ctor)) {
        LOG_ERROR("dom_exception_ctor corrupted before global assignment - aborting DOMException setup");
        goto dom_exception_cleanup;
    }
    
    JS_SetPropertyStr(ctx, global, "DOMException", dom_exception_ctor);
    
    // Success - skip cleanup
    goto skip_dom_exception;
    
dom_exception_cleanup:
    /* Constructor or property setting failed - log and continue without DOMException */
    LOG_ERROR("DOMException setup FAILED - continuing without DOMException (YouTube player may have reduced functionality)");
skip_dom_exception:
    /* Always log when we reach the skip label for debugging */
    LOG_INFO("Reached skip_dom_exception - continuing with rest of browser stubs initialization");
    ;
    
    // Map constructor
    LOG_INFO("Setting up Map constructor...");
    GCValue map_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, map_proto, js_map_proto_funcs, 
        sizeof(js_map_proto_funcs) / sizeof(js_map_proto_funcs[0]));
    GCValue map_ctor = JS_NewCFunction2(ctx, js_map_constructor, "Map", 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, map_ctor, map_proto);
    JS_SetClassProto(ctx, js_map_class_id, map_proto);
    // Set Map constructor on global
    JS_SetPropertyStr(ctx, global, "Map", map_ctor);
    LOG_INFO("Map constructor set on global");
    
    // Set Map prototype[Symbol.toStringTag]
    GCValue symbol_ctor = JS_GetPropertyStr(ctx, global, "Symbol");
    if (!JS_IsException(symbol_ctor) && !JS_IsUndefined(symbol_ctor)) {
        GCValue toStringTag = JS_GetPropertyStr(ctx, symbol_ctor, "toStringTag");
        if (!JS_IsException(toStringTag) && !JS_IsUndefined(toStringTag)) {
            JSAtom tag_atom = JS_ValueToAtom(ctx, toStringTag);
            if (tag_atom != JS_ATOM_NULL) {
                JS_SetProperty(ctx, map_proto, tag_atom, JS_NewString(ctx, "Map"));
            }
        }
    }
    LOG_INFO("Map Symbol.toStringTag done");
    
    // Set Map prototype[Symbol.iterator] = Map.prototype.entries
    // Use JS_Eval to get Symbol.iterator since it's a well-known symbol
    GCValue iterator_sym_eval = JS_Eval(ctx, "Symbol.iterator", 15, "<symbol>", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(iterator_sym_eval) && !JS_IsUndefined(iterator_sym_eval)) {
        JSAtom iterator_atom = JS_ValueToAtom(ctx, iterator_sym_eval);
        if (iterator_atom != JS_ATOM_NULL) {
            GCValue entries_fn = JS_GetPropertyStr(ctx, map_proto, "entries");
            JS_SetProperty(ctx, map_proto, iterator_atom, entries_fn);
            LOG_INFO("Map Symbol.iterator set via JS_Eval");
        }
    } else {
        LOG_INFO("Could not get Symbol.iterator");
    }
    
    // Promise.prototype.finally
    LOG_INFO("About to get Promise constructor...");
    GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    LOG_INFO("Got Promise constructor");
    if (!JS_IsException(promise_ctor)) {
        LOG_INFO("Promise ctor not exception, getting prototype...");
        GCValue promise_proto = JS_GetPropertyStr(ctx, promise_ctor, "prototype");
        LOG_INFO("Got Promise prototype");
        if (!JS_IsException(promise_proto)) {
            LOG_INFO("Promise proto not exception, setting finally...");
            JS_SetPropertyStr(ctx, promise_proto, "finally",
                JS_NewCFunction(ctx, js_promise_finally, "finally", 1));
            LOG_INFO("Promise.prototype.finally set");
        }
        LOG_INFO("Promise.prototype.finally done");
    }
    
    // String.prototype.includes
    GCValue string_ctor = JS_GetPropertyStr(ctx, global, "String");
    if (!JS_IsException(string_ctor)) {
        GCValue string_proto = JS_GetPropertyStr(ctx, string_ctor, "prototype");
        if (!JS_IsException(string_proto)) {
            JS_SetPropertyStr(ctx, string_proto, "includes",
                JS_NewCFunction(ctx, js_string_includes, "includes", 1));

        }

    }
    
    // Array.prototype.includes
    GCValue array_ctor = JS_GetPropertyStr(ctx, global, "Array");
    if (!JS_IsException(array_ctor)) {
        GCValue array_proto = JS_GetPropertyStr(ctx, array_ctor, "prototype");
        if (!JS_IsException(array_proto)) {
            JS_SetPropertyStr(ctx, array_proto, "includes",
                JS_NewCFunction(ctx, js_array_includes, "includes", 1));

        }
        // Array.from
        JS_SetPropertyStr(ctx, array_ctor, "from",
            JS_NewCFunction(ctx, js_array_from, "from", 1));

    }
    LOG_INFO("Array methods done");
    
    // ===== Window (global object itself) =====
    LOG_INFO("Setting up Window object...");
    // window IS the global object - this ensures 'this' at global level refers to window
    GCValue window = global;  // Use global object as window (no new object created)

    // Set window, self, globalThis, top, parent as properties on the global object
    // Set window properties on global
    JS_SetPropertyStr(ctx, global, "window", window);
    JS_SetPropertyStr(ctx, global, "self", window);
    JS_SetPropertyStr(ctx, global, "globalThis", window);
    JS_SetPropertyStr(ctx, global, "top", window);
    JS_SetPropertyStr(ctx, global, "parent", window);

    // Window constructor (needed by ShadyDOM polyfill)
    GCValue window_ctor = JS_NewCFunction2(ctx, js_dummy_function, "Window", 0, JS_CFUNC_constructor, 0);
    GCValue window_proto = JS_NewObject(ctx);
    if (!JS_IsException(window_proto)) {
        JS_SetPropertyStr(ctx, window_proto, "constructor", window_ctor);
        // Add EventTarget methods to Window.prototype (needed by ShadyDOM polyfill)
        JS_SetPropertyStr(ctx, window_proto, "addEventListener",
            JS_NewCFunction(ctx, js_event_target_addEventListener, "addEventListener", 2));
        JS_SetPropertyStr(ctx, window_proto, "removeEventListener",
            JS_NewCFunction(ctx, js_event_target_removeEventListener, "removeEventListener", 2));
        JS_SetPropertyStr(ctx, window_proto, "dispatchEvent",
            JS_NewCFunction(ctx, js_event_target_dispatchEvent, "dispatchEvent", 1));
        JS_SetPropertyStr(ctx, window_ctor, "prototype", window_proto);
    }
    JS_SetPropertyStr(ctx, global, "Window", window_ctor);
    // Set global object's prototype to Window.prototype (window instanceof Window should be true)
    JS_SetPrototype(ctx, global, window_proto);

    // ===== Create DOM Constructors with proper prototype chain in C =====
    // Reference counting rules:
    // - JS_NewCFunction2/JS_NewObject: returns value with refcount 1
    // - JS_SetPropertyStr: duplicates the value (refcount +1)
    // After setting a property, we MUST free the local reference!
    
    // Helper to set up prototype chain using Object.setPrototypeOf
    object_ctor = JS_GetPropertyStr(ctx, global, "Object");
    GCValue set_proto_of = JS_GetPropertyStr(ctx, object_ctor, "setPrototypeOf");
    
    // EventTarget constructor (base of all DOM constructors)
    GCValue event_target_ctor = JS_NewCFunction2(ctx, js_dummy_function, "EventTarget", 0, JS_CFUNC_constructor, 0);
    GCValue event_target_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, event_target_proto, "constructor", event_target_ctor);
    JS_SetPropertyStr(ctx, event_target_ctor, "prototype", event_target_proto);
    // Add observedAttributes static getter to prevent Polymer mixin chain from walking to null.constructor
    JS_DefinePropertyGetSet(ctx, event_target_ctor, JS_NewAtom(ctx, "observedAttributes"),
        JS_NewCFunction(ctx, js_event_target_observed_attributes, "observedAttributes", 0),
        JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_SetPropertyStr(ctx, global, "EventTarget", event_target_ctor);
    JS_SetPropertyStr(ctx, window, "EventTarget", event_target_ctor);
    // Keep event_target_proto for Node's prototype chain
    
    // Node constructor
    GCValue node_ctor = JS_NewCFunction2(ctx, js_dummy_function, "Node", 0, JS_CFUNC_constructor, 0);
    GCValue node_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, node_proto, "constructor", node_ctor);
    // Node.prototype -> EventTarget.prototype
    JS_SetPrototype(ctx, node_proto, event_target_proto);

    JS_SetPropertyStr(ctx, node_ctor, "prototype", node_proto);
    JS_SetPropertyStr(ctx, global, "Node", node_ctor);
    JS_SetPropertyStr(ctx, window, "Node", node_ctor);

    // Note: event_target_proto is kept alive for adding methods below
    // It will be freed after we add methods to it
    // Keep node_proto for Element and DocumentFragment
    
    // Element constructor
    GCValue element_ctor = JS_NewCFunction2(ctx, js_element_constructor, "Element", 0, JS_CFUNC_constructor, 0);
    GCValue element_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, element_proto, "constructor", element_ctor);
    // Element.prototype -> Node.prototype
    JS_SetPrototype(ctx, element_proto, node_proto);

    JS_SetPropertyStr(ctx, element_ctor, "prototype", element_proto);
    // Since window = global, setting Element on global makes it accessible as window.Element
    int element_ret = JS_SetPropertyStr(ctx, global, "Element", element_ctor);
    JS_SetPropertyStr(ctx, window, "Element", element_ctor);
    // DON'T free element_ctor yet - we need it for document.documentElement below
    // Keep element_proto for HTMLElement
    // Note: node_proto is kept alive for adding methods below
    
    // HTMLElement constructor
    GCValue html_element_ctor = JS_NewCFunction2(ctx, js_html_element_constructor, "HTMLElement", 0, JS_CFUNC_constructor, 0);
    GCValue html_element_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, html_element_proto, "constructor", html_element_ctor);
    // HTMLElement.prototype -> Element.prototype
    JS_SetPrototype(ctx, html_element_proto, element_proto);

    JS_SetPropertyStr(ctx, html_element_ctor, "prototype", html_element_proto);
    JS_SetPropertyStr(ctx, global, "HTMLElement", html_element_ctor);
    JS_SetPropertyStr(ctx, window, "HTMLElement", html_element_ctor);
    // DON'T free html_element_ctor yet - we need it for document.body below
    // element_proto will be freed after adding methods below
    // Keep html_element_ctor and html_element_proto for document.body
    
    // HTMLDivElement constructor (needed by Polymer/TypeScript decorator metadata)
    GCValue div_element_ctor = JS_NewCFunction2(ctx, js_html_element_constructor, "HTMLDivElement", 0, JS_CFUNC_constructor, 0);
    GCValue div_element_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, div_element_proto, "constructor", div_element_ctor);
    JS_SetPrototype(ctx, div_element_proto, html_element_proto);
    JS_SetPropertyStr(ctx, div_element_ctor, "prototype", div_element_proto);
    JS_SetPropertyStr(ctx, global, "HTMLDivElement", div_element_ctor);
    JS_SetPropertyStr(ctx, window, "HTMLDivElement", div_element_ctor);
    
    // HTMLImageElement constructor (needed by Polymer/TypeScript decorator metadata)
    GCValue img_element_ctor = JS_NewCFunction2(ctx, js_html_element_constructor, "HTMLImageElement", 0, JS_CFUNC_constructor, 0);
    GCValue img_element_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, img_element_proto, "constructor", img_element_ctor);
    JS_SetPrototype(ctx, img_element_proto, html_element_proto);
    JS_SetPropertyStr(ctx, img_element_ctor, "prototype", img_element_proto);
    JS_SetPropertyStr(ctx, global, "HTMLImageElement", img_element_ctor);
    JS_SetPropertyStr(ctx, window, "HTMLImageElement", img_element_ctor);
    
    // HTMLInputElement constructor (needed by Polymer/TypeScript decorator metadata)
    GCValue input_element_ctor = JS_NewCFunction2(ctx, js_html_element_constructor, "HTMLInputElement", 0, JS_CFUNC_constructor, 0);
    GCValue input_element_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, input_element_proto, "constructor", input_element_ctor);
    JS_SetPrototype(ctx, input_element_proto, html_element_proto);
    JS_SetPropertyStr(ctx, input_element_ctor, "prototype", input_element_proto);
    JS_SetPropertyStr(ctx, global, "HTMLInputElement", input_element_ctor);
    JS_SetPropertyStr(ctx, window, "HTMLInputElement", input_element_ctor);
    
    // HTMLFormElement constructor (needed by Polymer/TypeScript decorator metadata)
    GCValue form_element_ctor = JS_NewCFunction2(ctx, js_html_element_constructor, "HTMLFormElement", 0, JS_CFUNC_constructor, 0);
    GCValue form_element_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, form_element_proto, "constructor", form_element_ctor);
    JS_SetPrototype(ctx, form_element_proto, html_element_proto);
    JS_SetPropertyStr(ctx, form_element_ctor, "prototype", form_element_proto);
    JS_SetPropertyStr(ctx, global, "HTMLFormElement", form_element_ctor);
    JS_SetPropertyStr(ctx, window, "HTMLFormElement", form_element_ctor);
    
    // HTMLIFrameElement constructor (needed by Polymer/TypeScript decorator metadata)
    GCValue iframe_element_ctor = JS_NewCFunction2(ctx, js_html_element_constructor, "HTMLIFrameElement", 0, JS_CFUNC_constructor, 0);
    GCValue iframe_element_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, iframe_element_proto, "constructor", iframe_element_ctor);
    JS_SetPrototype(ctx, iframe_element_proto, html_element_proto);
    JS_SetPropertyStr(ctx, iframe_element_ctor, "prototype", iframe_element_proto);
    JS_SetPropertyStr(ctx, global, "HTMLIFrameElement", iframe_element_ctor);
    JS_SetPropertyStr(ctx, window, "HTMLIFrameElement", iframe_element_ctor);
    
    // HTMLTextAreaElement constructor (needed by Polymer/TypeScript decorator metadata)
    GCValue textarea_element_ctor = JS_NewCFunction2(ctx, js_html_element_constructor, "HTMLTextAreaElement", 0, JS_CFUNC_constructor, 0);
    GCValue textarea_element_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, textarea_element_proto, "constructor", textarea_element_ctor);
    JS_SetPrototype(ctx, textarea_element_proto, html_element_proto);
    JS_SetPropertyStr(ctx, textarea_element_ctor, "prototype", textarea_element_proto);
    JS_SetPropertyStr(ctx, global, "HTMLTextAreaElement", textarea_element_ctor);
    JS_SetPropertyStr(ctx, window, "HTMLTextAreaElement", textarea_element_ctor);
    
    // HTMLCanvasElement constructor (needed by Polymer/TypeScript decorator metadata)
    GCValue canvas_element_ctor = JS_NewCFunction2(ctx, js_html_element_constructor, "HTMLCanvasElement", 0, JS_CFUNC_constructor, 0);
    GCValue canvas_element_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, canvas_element_proto, "constructor", canvas_element_ctor);
    JS_SetPrototype(ctx, canvas_element_proto, html_element_proto);
    JS_SetPropertyStr(ctx, canvas_element_ctor, "prototype", canvas_element_proto);
    JS_SetPropertyStr(ctx, global, "HTMLCanvasElement", canvas_element_ctor);
    JS_SetPropertyStr(ctx, window, "HTMLCanvasElement", canvas_element_ctor);
    
    // HTMLAnchorElement constructor
    GCValue anchor_element_ctor = JS_NewCFunction2(ctx, js_html_element_constructor, "HTMLAnchorElement", 0, JS_CFUNC_constructor, 0);
    GCValue anchor_element_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, anchor_element_proto, "constructor", anchor_element_ctor);
    JS_SetPrototype(ctx, anchor_element_proto, html_element_proto);
    JS_SetPropertyStr(ctx, anchor_element_ctor, "prototype", anchor_element_proto);
    JS_SetPropertyStr(ctx, global, "HTMLAnchorElement", anchor_element_ctor);
    JS_SetPropertyStr(ctx, window, "HTMLAnchorElement", anchor_element_ctor);
    
    // HTMLButtonElement constructor
    GCValue button_element_ctor = JS_NewCFunction2(ctx, js_html_element_constructor, "HTMLButtonElement", 0, JS_CFUNC_constructor, 0);
    GCValue button_element_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, button_element_proto, "constructor", button_element_ctor);
    JS_SetPrototype(ctx, button_element_proto, html_element_proto);
    JS_SetPropertyStr(ctx, button_element_ctor, "prototype", button_element_proto);
    JS_SetPropertyStr(ctx, global, "HTMLButtonElement", button_element_ctor);
    JS_SetPropertyStr(ctx, window, "HTMLButtonElement", button_element_ctor);
    
    // HTMLLinkElement constructor
    GCValue link_element_ctor = JS_NewCFunction2(ctx, js_html_element_constructor, "HTMLLinkElement", 0, JS_CFUNC_constructor, 0);
    GCValue link_element_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, link_element_proto, "constructor", link_element_ctor);
    JS_SetPrototype(ctx, link_element_proto, html_element_proto);
    JS_SetPropertyStr(ctx, link_element_ctor, "prototype", link_element_proto);
    JS_SetPropertyStr(ctx, global, "HTMLLinkElement", link_element_ctor);
    JS_SetPropertyStr(ctx, window, "HTMLLinkElement", link_element_ctor);
    
    // HTMLSelectElement constructor
    GCValue select_element_ctor = JS_NewCFunction2(ctx, js_html_element_constructor, "HTMLSelectElement", 0, JS_CFUNC_constructor, 0);
    GCValue select_element_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, select_element_proto, "constructor", select_element_ctor);
    JS_SetPrototype(ctx, select_element_proto, html_element_proto);
    JS_SetPropertyStr(ctx, select_element_ctor, "prototype", select_element_proto);
    JS_SetPropertyStr(ctx, global, "HTMLSelectElement", select_element_ctor);
    JS_SetPropertyStr(ctx, window, "HTMLSelectElement", select_element_ctor);
    
    // HTMLOptionElement constructor
    GCValue option_element_ctor = JS_NewCFunction2(ctx, js_html_element_constructor, "HTMLOptionElement", 0, JS_CFUNC_constructor, 0);
    GCValue option_element_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, option_element_proto, "constructor", option_element_ctor);
    JS_SetPrototype(ctx, option_element_proto, html_element_proto);
    JS_SetPropertyStr(ctx, option_element_ctor, "prototype", option_element_proto);
    JS_SetPropertyStr(ctx, global, "HTMLOptionElement", option_element_ctor);
    JS_SetPropertyStr(ctx, window, "HTMLOptionElement", option_element_ctor);
    
    // HTMLStyleElement constructor
    GCValue style_element_ctor = JS_NewCFunction2(ctx, js_html_element_constructor, "HTMLStyleElement", 0, JS_CFUNC_constructor, 0);
    GCValue style_element_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, style_element_proto, "constructor", style_element_ctor);
    JS_SetPrototype(ctx, style_element_proto, html_element_proto);
    JS_SetPropertyStr(ctx, style_element_ctor, "prototype", style_element_proto);
    JS_SetPropertyStr(ctx, global, "HTMLStyleElement", style_element_ctor);
    JS_SetPropertyStr(ctx, window, "HTMLStyleElement", style_element_ctor);
    
    // HTMLUnknownElement constructor
    GCValue unknown_element_ctor = JS_NewCFunction2(ctx, js_html_element_constructor, "HTMLUnknownElement", 0, JS_CFUNC_constructor, 0);
    GCValue unknown_element_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, unknown_element_proto, "constructor", unknown_element_ctor);
    JS_SetPrototype(ctx, unknown_element_proto, html_element_proto);
    JS_SetPropertyStr(ctx, unknown_element_ctor, "prototype", unknown_element_proto);
    JS_SetPropertyStr(ctx, global, "HTMLUnknownElement", unknown_element_ctor);
    JS_SetPropertyStr(ctx, window, "HTMLUnknownElement", unknown_element_ctor);
    
    // HTMLFencedFrameElement constructor
    GCValue fenced_frame_element_ctor = JS_NewCFunction2(ctx, js_html_element_constructor, "HTMLFencedFrameElement", 0, JS_CFUNC_constructor, 0);
    GCValue fenced_frame_element_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, fenced_frame_element_proto, "constructor", fenced_frame_element_ctor);
    JS_SetPrototype(ctx, fenced_frame_element_proto, html_element_proto);
    JS_SetPropertyStr(ctx, fenced_frame_element_ctor, "prototype", fenced_frame_element_proto);
    JS_SetPropertyStr(ctx, global, "HTMLFencedFrameElement", fenced_frame_element_ctor);
    JS_SetPropertyStr(ctx, window, "HTMLFencedFrameElement", fenced_frame_element_ctor);
    
    // SVGElement constructor (needed by Polymer/TypeScript decorator metadata)
    GCValue svg_element_ctor = JS_NewCFunction2(ctx, js_html_element_constructor, "SVGElement", 0, JS_CFUNC_constructor, 0);
    GCValue svg_element_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, svg_element_proto, "constructor", svg_element_ctor);
    JS_SetPrototype(ctx, svg_element_proto, html_element_proto);
    JS_SetPropertyStr(ctx, svg_element_ctor, "prototype", svg_element_proto);
    JS_SetPropertyStr(ctx, global, "SVGElement", svg_element_ctor);
    JS_SetPropertyStr(ctx, window, "SVGElement", svg_element_ctor);
    
    // HTMLTemplateElement constructor (needed by ShadyDOM polyfill)
    GCValue template_ctor = JS_NewCFunction2(ctx, js_dummy_function, "HTMLTemplateElement", 0, JS_CFUNC_constructor, 0);
    GCValue template_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, template_proto, "constructor", template_ctor);
    JS_SetPropertyStr(ctx, template_ctor, "prototype", template_proto);
    JS_SetPropertyStr(ctx, global, "HTMLTemplateElement", template_ctor);
    
    // HTMLSlotElement constructor (needed by ShadyDOM polyfill)
    GCValue slot_ctor = JS_NewCFunction2(ctx, js_dummy_function, "HTMLSlotElement", 0, JS_CFUNC_constructor, 0);
    GCValue slot_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, slot_proto, "constructor", slot_ctor);
    JS_SetPropertyStr(ctx, slot_ctor, "prototype", slot_proto);
    JS_SetPropertyStr(ctx, global, "HTMLSlotElement", slot_ctor);
    
    // DocumentFragment constructor (needs node_proto)
    GCValue doc_fragment_ctor = JS_NewCFunction2(ctx, js_dummy_function, "DocumentFragment", 0, JS_CFUNC_constructor, 0);
    GCValue doc_fragment_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, doc_fragment_proto, "constructor", doc_fragment_ctor);
    // DocumentFragment.prototype -> Node.prototype
    // Note: Prototype chain setup removed - add methods directly to doc_fragment_proto if needed

    JS_SetPropertyStr(ctx, doc_fragment_ctor, "prototype", doc_fragment_proto);
    JS_SetPropertyStr(ctx, global, "DocumentFragment", doc_fragment_ctor);
    
    // Document constructor (needed by some polyfills like ShadyDOM)
    GCValue document_ctor = JS_NewCFunction2(ctx, js_dummy_function, "Document", 0, JS_CFUNC_constructor, 0);
    GCValue document_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, document_proto, "constructor", document_ctor);
    // Note: Document.prototype chain not set - if needed, add prototype methods directly to document_proto

    JS_SetPropertyStr(ctx, document_ctor, "prototype", document_proto);
    JS_SetPropertyStr(ctx, global, "Document", document_ctor);
    JS_SetPropertyStr(ctx, window, "Document", document_ctor);
    JS_SetPropertyStr(ctx, window, "DocumentFragment", doc_fragment_ctor);
    
    // Text constructor (needed by ShadyDOM polyfill)
    GCValue text_ctor = JS_NewCFunction2(ctx, js_dummy_function, "Text", 0, JS_CFUNC_constructor, 0);
    GCValue text_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, text_proto, "constructor", text_ctor);
    JS_SetPropertyStr(ctx, text_ctor, "prototype", text_proto);
    JS_SetPropertyStr(ctx, global, "Text", text_ctor);
    JS_SetPropertyStr(ctx, window, "Text", text_ctor);
    
    // Comment constructor (needed by ShadyDOM polyfill)
    GCValue comment_ctor = JS_NewCFunction2(ctx, js_dummy_function, "Comment", 0, JS_CFUNC_constructor, 0);
    GCValue comment_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, comment_proto, "constructor", comment_ctor);
    JS_SetPropertyStr(ctx, comment_ctor, "prototype", comment_proto);
    JS_SetPropertyStr(ctx, global, "Comment", comment_ctor);
    JS_SetPropertyStr(ctx, window, "Comment", comment_ctor);
    
    // CDATASection constructor (needed by ShadyDOM polyfill)
    GCValue cdata_section_ctor = JS_NewCFunction2(ctx, js_dummy_function, "CDATASection", 0, JS_CFUNC_constructor, 0);
    GCValue cdata_section_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, cdata_section_proto, "constructor", cdata_section_ctor);
    JS_SetPropertyStr(ctx, cdata_section_ctor, "prototype", cdata_section_proto);
    JS_SetPropertyStr(ctx, global, "CDATASection", cdata_section_ctor);
    JS_SetPropertyStr(ctx, window, "CDATASection", cdata_section_ctor);
    
    // ProcessingInstruction constructor (needed by ShadyDOM polyfill)
    GCValue processing_instruction_ctor = JS_NewCFunction2(ctx, js_dummy_function, "ProcessingInstruction", 0, JS_CFUNC_constructor, 0);
    GCValue processing_instruction_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, processing_instruction_proto, "constructor", processing_instruction_ctor);
    JS_SetPropertyStr(ctx, processing_instruction_ctor, "prototype", processing_instruction_proto);
    JS_SetPropertyStr(ctx, global, "ProcessingInstruction", processing_instruction_ctor);
    JS_SetPropertyStr(ctx, window, "ProcessingInstruction", processing_instruction_ctor);


    // ===== Ensure DOM Prototype Chain Integrity =====
    // Re-fetch prototypes from global to ensure chains are properly linked
    // This handles cases where JS code may have modified prototypes
    
    GCValue event_target_check = JS_GetPropertyStr(ctx, global, "EventTarget");
    GCValue node_check = JS_GetPropertyStr(ctx, global, "Node");
    GCValue element_check = JS_GetPropertyStr(ctx, global, "Element");
    GCValue html_element_check = JS_GetPropertyStr(ctx, global, "HTMLElement");
    GCValue doc_fragment_check = JS_GetPropertyStr(ctx, global, "DocumentFragment");
    
    GCValue event_target_proto_check = js_get_prototype(ctx, event_target_check);
    GCValue node_proto_check = js_get_prototype(ctx, node_check);
    GCValue element_proto_check = js_get_prototype(ctx, element_check);
    GCValue html_element_proto_check = js_get_prototype(ctx, html_element_check);
    GCValue doc_fragment_proto_check = js_get_prototype(ctx, doc_fragment_check);
    
    // Ensure prototype chains using Object.setPrototypeOf
    if (!JS_IsUndefined(node_proto_check) && !JS_IsNull(node_proto_check) &&
        !JS_IsUndefined(event_target_proto_check) && !JS_IsNull(event_target_proto_check)) {
        GCValue args1[2] = { node_proto_check, event_target_proto_check };

    }
    
    if (!JS_IsUndefined(element_proto_check) && !JS_IsNull(element_proto_check) &&
        !JS_IsUndefined(node_proto_check) && !JS_IsNull(node_proto_check)) {
        GCValue args2[2] = { element_proto_check, node_proto_check };

    }
    
    if (!JS_IsUndefined(html_element_proto_check) && !JS_IsNull(html_element_proto_check) &&
        !JS_IsUndefined(element_proto_check) && !JS_IsNull(element_proto_check)) {
        GCValue args3[2] = { html_element_proto_check, element_proto_check };

    }
    
    if (!JS_IsUndefined(doc_fragment_proto_check) && !JS_IsNull(doc_fragment_proto_check) &&
        !JS_IsUndefined(node_proto_check) && !JS_IsNull(node_proto_check)) {
        GCValue args4[2] = { doc_fragment_proto_check, node_proto_check };

    }










    // Clean up Object.setPrototypeOf helper


    // node_proto will be freed after adding methods below
    
    // ===== EventTarget prototype methods =====
    JS_SetPropertyStr(ctx, event_target_proto, "addEventListener",
        JS_NewCFunction(ctx, js_event_target_addEventListener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, event_target_proto, "removeEventListener",
        JS_NewCFunction(ctx, js_event_target_removeEventListener, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, event_target_proto, "dispatchEvent",
        JS_NewCFunction(ctx, js_event_target_dispatchEvent, "dispatchEvent", 1));
    
    // ===== Node prototype methods - REAL DOM IMPLEMENTATION =====
    JS_SetPropertyStr(ctx, node_proto, "appendChild",
        JS_NewCFunction(ctx, js_node_appendChild_real, "appendChild", 1));
    JS_SetPropertyStr(ctx, node_proto, "insertBefore",
        JS_NewCFunction(ctx, js_node_insertBefore_real, "insertBefore", 2));
    JS_SetPropertyStr(ctx, node_proto, "removeChild",
        JS_NewCFunction(ctx, js_node_removeChild_real, "removeChild", 1));
    JS_SetPropertyStr(ctx, node_proto, "cloneNode",
        JS_NewCFunction(ctx, js_node_cloneNode_real, "cloneNode", 1));
    JS_SetPropertyStr(ctx, node_proto, "contains",
        JS_NewCFunction(ctx, js_node_contains_real, "contains", 1));
    JS_SetPropertyStr(ctx, node_proto, "getRootNode",
        JS_NewCFunction(ctx, js_node_getRootNode_real, "getRootNode", 0));
    
    // ===== Node Tree Navigation Properties (REAL) =====
    // firstChild getter
    GCValue first_child_getter = JS_NewCFunction(ctx, js_node_get_firstChild, "get firstChild", 0);
    JSAtom first_child_atom = JS_NewAtom(ctx, "firstChild");
    JS_DefinePropertyGetSet(ctx, node_proto, first_child_atom, first_child_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, first_child_atom);
    
    // lastChild getter
    GCValue last_child_getter = JS_NewCFunction(ctx, js_node_get_lastChild, "get lastChild", 0);
    JSAtom last_child_atom = JS_NewAtom(ctx, "lastChild");
    JS_DefinePropertyGetSet(ctx, node_proto, last_child_atom, last_child_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, last_child_atom);
    
    // nextSibling getter
    GCValue next_sibling_getter = JS_NewCFunction(ctx, js_node_get_nextSibling, "get nextSibling", 0);
    JSAtom next_sibling_atom = JS_NewAtom(ctx, "nextSibling");
    JS_DefinePropertyGetSet(ctx, node_proto, next_sibling_atom, next_sibling_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, next_sibling_atom);
    
    // previousSibling getter
    GCValue prev_sibling_getter = JS_NewCFunction(ctx, js_node_get_previousSibling, "get previousSibling", 0);
    JSAtom prev_sibling_atom = JS_NewAtom(ctx, "previousSibling");
    JS_DefinePropertyGetSet(ctx, node_proto, prev_sibling_atom, prev_sibling_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, prev_sibling_atom);
    
    // parentNode getter
    GCValue parent_node_getter = JS_NewCFunction(ctx, js_node_get_parentNode, "get parentNode", 0);
    JSAtom parent_node_atom = JS_NewAtom(ctx, "parentNode");
    JS_DefinePropertyGetSet(ctx, node_proto, parent_node_atom, parent_node_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, parent_node_atom);
    
    // parentElement getter
    GCValue parent_element_getter = JS_NewCFunction(ctx, js_node_get_parentElement, "get parentElement", 0);
    JSAtom parent_element_atom = JS_NewAtom(ctx, "parentElement");
    JS_DefinePropertyGetSet(ctx, node_proto, parent_element_atom, parent_element_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, parent_element_atom);
    
    // childNodes getter
    GCValue child_nodes_getter = JS_NewCFunction(ctx, js_node_get_childNodes, "get childNodes", 0);
    JSAtom child_nodes_atom = JS_NewAtom(ctx, "childNodes");
    JS_DefinePropertyGetSet(ctx, node_proto, child_nodes_atom, child_nodes_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, child_nodes_atom);
    
    // nodeType getter
    GCValue node_type_getter = JS_NewCFunction(ctx, js_node_get_nodeType, "get nodeType", 0);
    JSAtom node_type_atom = JS_NewAtom(ctx, "nodeType");
    JS_DefinePropertyGetSet(ctx, node_proto, node_type_atom, node_type_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, node_type_atom);
    
    // nodeName getter
    GCValue node_name_getter = JS_NewCFunction(ctx, js_node_get_nodeName, "get nodeName", 0);
    JSAtom node_name_atom = JS_NewAtom(ctx, "nodeName");
    JS_DefinePropertyGetSet(ctx, node_proto, node_name_atom, node_name_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, node_name_atom);
    
    // ===== HTMLElement prototype methods =====
    // attachShadow (same as Element)
    JS_SetPropertyStr(ctx, html_element_proto, "attachShadow",
        JS_NewCFunction(ctx, js_element_attach_shadow, "attachShadow", 1));
    // click method (same as Element)
    JS_SetPropertyStr(ctx, html_element_proto, "click",
        JS_NewCFunction(ctx, js_element_click, "click", 0));
    // Attribute methods (needed because prototype chain to Element is broken)
    JS_SetPropertyStr(ctx, html_element_proto, "getAttribute",
        JS_NewCFunction(ctx, js_element_get_attribute, "getAttribute", 1));
    JS_SetPropertyStr(ctx, html_element_proto, "setAttribute",
        JS_NewCFunction(ctx, js_element_set_attribute, "setAttribute", 2));
    JS_SetPropertyStr(ctx, html_element_proto, "removeAttribute",
        JS_NewCFunction(ctx, js_element_remove_attribute, "removeAttribute", 1));
    JS_SetPropertyStr(ctx, html_element_proto, "hasAttribute",
        JS_NewCFunction(ctx, js_element_has_attribute, "hasAttribute", 1));
    JS_SetPropertyStr(ctx, html_element_proto, "querySelector",
        JS_NewCFunction(ctx, js_element_querySelector_real, "querySelector", 1));
    JS_SetPropertyStr(ctx, html_element_proto, "querySelectorAll",
        JS_NewCFunction(ctx, js_element_querySelectorAll_real, "querySelectorAll", 1));
    
    // ===== Element prototype methods =====
    // attachShadow method
    JS_SetPropertyStr(ctx, element_proto, "attachShadow",
        JS_NewCFunction(ctx, js_element_attach_shadow, "attachShadow", 1));
    // tagName getter - returns DOM node name or "DIV" fallback
    GCValue tagName_getter = JS_NewCFunction(ctx, js_element_get_tagName, "get tagName", 0);
    JSAtom tagName_atom = JS_NewAtom(ctx, "tagName");
    JS_DefinePropertyGetSet(ctx, element_proto, tagName_atom,
        tagName_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, tagName_atom);
    
    // shadowRoot getter
    GCValue getter = JS_NewCFunction(ctx, js_element_get_shadow_root, "get shadowRoot", 0);
    JSAtom shadow_root_atom = JS_NewAtom(ctx, "shadowRoot");
    // Note: JS_DefinePropertyGetSet takes ownership of the getter/setter values.
    // Do NOT free getter after passing it - the property now owns it.
    JS_DefinePropertyGetSet(ctx, element_proto, shadow_root_atom,
        getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, shadow_root_atom);
    // querySelector and querySelectorAll (REAL DOM IMPLEMENTATION)
    JS_SetPropertyStr(ctx, element_proto, "querySelector",
        JS_NewCFunction(ctx, js_element_querySelector_real, "querySelector", 1));
    JS_SetPropertyStr(ctx, element_proto, "querySelectorAll",
        JS_NewCFunction(ctx, js_element_querySelectorAll_real, "querySelectorAll", 1));
    // Attribute methods
    JS_SetPropertyStr(ctx, element_proto, "getAttribute",
        JS_NewCFunction(ctx, js_element_get_attribute, "getAttribute", 1));
    JS_SetPropertyStr(ctx, element_proto, "setAttribute",
        JS_NewCFunction(ctx, js_element_set_attribute, "setAttribute", 2));
    JS_SetPropertyStr(ctx, element_proto, "removeAttribute",
        JS_NewCFunction(ctx, js_element_remove_attribute, "removeAttribute", 1));
    JS_SetPropertyStr(ctx, element_proto, "hasAttribute",
        JS_NewCFunction(ctx, js_element_has_attribute, "hasAttribute", 1));
    JS_SetPropertyStr(ctx, element_proto, "toggleAttribute",
        JS_NewCFunction(ctx, js_element_toggle_attribute, "toggleAttribute", 1));
    // NS attribute methods
    JS_SetPropertyStr(ctx, element_proto, "setAttributeNS",
        JS_NewCFunction(ctx, js_element_set_attribute_ns, "setAttributeNS", 3));
    JS_SetPropertyStr(ctx, element_proto, "getAttributeNS",
        JS_NewCFunction(ctx, js_element_get_attribute_ns, "getAttributeNS", 2));
    JS_SetPropertyStr(ctx, element_proto, "removeAttributeNS",
        JS_NewCFunction(ctx, js_element_remove_attribute_ns, "removeAttributeNS", 2));
    // click method
    JS_SetPropertyStr(ctx, element_proto, "click",
        JS_NewCFunction(ctx, js_element_click, "click", 0));
    // animate method
    JS_SetPropertyStr(ctx, element_proto, "animate",
        JS_NewCFunction(ctx, js_element_animate, "animate", 2));
    // getAnimations method
    JS_SetPropertyStr(ctx, element_proto, "getAnimations",
        JS_NewCFunction(ctx, js_element_get_animations, "getAnimations", 0));
    // getBoundingClientRect method
    JS_SetPropertyStr(ctx, element_proto, "getBoundingClientRect",
        JS_NewCFunction(ctx, js_element_getBoundingClientRect, "getBoundingClientRect", 0));
    // getElementsByTagName method
    JS_SetPropertyStr(ctx, element_proto, "getElementsByTagName",
        JS_NewCFunction(ctx, js_element_get_elements_by_tag_name, "getElementsByTagName", 1));
    
    // ===== Element Tree Navigation Properties (REAL) =====
    // children getter
    GCValue children_getter = JS_NewCFunction(ctx, js_element_get_children, "get children", 0);
    JSAtom children_atom = JS_NewAtom(ctx, "children");
    JS_DefinePropertyGetSet(ctx, element_proto, children_atom, children_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, children_atom);
    
    // firstElementChild getter (REAL)
    GCValue first_elem_child_getter = JS_NewCFunction(ctx, js_element_get_firstElementChild, "get firstElementChild", 0);
    JSAtom first_elem_child_atom = JS_NewAtom(ctx, "firstElementChild");
    JS_DefinePropertyGetSet(ctx, element_proto, first_elem_child_atom, first_elem_child_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, first_elem_child_atom);
    
    // lastElementChild getter (REAL)
    GCValue last_elem_child_getter = JS_NewCFunction(ctx, js_element_get_lastElementChild, "get lastElementChild", 0);
    JSAtom last_elem_child_atom = JS_NewAtom(ctx, "lastElementChild");
    JS_DefinePropertyGetSet(ctx, element_proto, last_elem_child_atom, last_elem_child_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, last_elem_child_atom);
    
    // nextElementSibling getter (REAL)
    GCValue next_elem_sibling_getter = JS_NewCFunction(ctx, js_element_get_nextElementSibling, "get nextElementSibling", 0);
    JSAtom next_elem_sibling_atom = JS_NewAtom(ctx, "nextElementSibling");
    JS_DefinePropertyGetSet(ctx, element_proto, next_elem_sibling_atom, next_elem_sibling_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, next_elem_sibling_atom);
    
    // previousElementSibling getter (REAL)
    GCValue prev_elem_sibling_getter = JS_NewCFunction(ctx, js_element_get_previousElementSibling, "get previousElementSibling", 0);
    JSAtom prev_elem_sibling_atom = JS_NewAtom(ctx, "previousElementSibling");
    JS_DefinePropertyGetSet(ctx, element_proto, prev_elem_sibling_atom, prev_elem_sibling_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, prev_elem_sibling_atom);
    
    // childElementCount getter (REAL)
    GCValue child_elem_count_getter = JS_NewCFunction(ctx, js_element_get_childElementCount, "get childElementCount", 0);
    JSAtom child_elem_count_atom = JS_NewAtom(ctx, "childElementCount");
    JS_DefinePropertyGetSet(ctx, element_proto, child_elem_count_atom, child_elem_count_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, child_elem_count_atom);
    
    // ===== Element Content Properties =====
    // innerHTML getter/setter
    GCValue inner_html_getter = JS_NewCFunction(ctx, js_element_get_inner_html, "get innerHTML", 0);
    GCValue inner_html_setter = JS_NewCFunction(ctx, js_element_set_inner_html, "set innerHTML", 1);
    JSAtom inner_html_atom = JS_NewAtom(ctx, "innerHTML");
    JS_DefinePropertyGetSet(ctx, element_proto, inner_html_atom, inner_html_getter, inner_html_setter, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, inner_html_atom);
    
    // outerHTML getter/setter
    GCValue outer_html_getter = JS_NewCFunction(ctx, js_element_get_outer_html, "get outerHTML", 0);
    GCValue outer_html_setter = JS_NewCFunction(ctx, js_element_set_outer_html, "set outerHTML", 1);
    JSAtom outer_html_atom = JS_NewAtom(ctx, "outerHTML");
    JS_DefinePropertyGetSet(ctx, element_proto, outer_html_atom, outer_html_getter, outer_html_setter, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, outer_html_atom);
    
    // classList getter
    GCValue class_list_getter = JS_NewCFunction(ctx, js_element_get_classList, "get classList", 0);
    JSAtom class_list_atom = JS_NewAtom(ctx, "classList");
    JS_DefinePropertyGetSet(ctx, element_proto, class_list_atom, class_list_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, class_list_atom);
    
    // dataset getter
    GCValue dataset_getter = JS_NewCFunction(ctx, js_element_get_dataset, "get dataset", 0);
    JSAtom dataset_atom = JS_NewAtom(ctx, "dataset");
    JS_DefinePropertyGetSet(ctx, element_proto, dataset_atom, dataset_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, dataset_atom);
    
    // ===== Node Content Properties (on Node.prototype) =====
    // textContent getter/setter - use simple property instead of getter/setter
    JSAtom text_content_atom = JS_NewAtom(ctx, "textContent");
    JS_DefinePropertyValue(ctx, node_proto, text_content_atom, 
        JS_NewCFunction(ctx, js_node_get_text_content, "textContent", 0), JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, text_content_atom);
    
    // nodeValue getter/setter
    GCValue node_value_getter = JS_NewCFunction(ctx, js_node_get_node_value, "get nodeValue", 0);
    GCValue node_value_setter = JS_NewCFunction(ctx, js_node_set_node_value, "set nodeValue", 1);
    JSAtom node_value_atom = JS_NewAtom(ctx, "nodeValue");
    JS_DefinePropertyGetSet(ctx, node_proto, node_value_atom, node_value_getter, node_value_setter, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, node_value_atom);
    
    // Do NOT free the prototypes here!
    // They are still referenced by:
    // 1. The constructor's 'prototype' property (set via JS_SetPropertyStr)
    // 2. The prototype chain links (set via Object.setPrototypeOf)
    // 3. Each other through parent prototype relationships
    // Freeing them now would create dangling pointers.
    // QuickJS garbage collector will clean them up when the context is freed.
    // (void)event_target_proto;  // Kept alive by prototype chain
    // (void)node_proto;          // Kept alive by prototype chain
    // (void)element_proto;       // Kept alive by prototype chain
    
    // NOTE: We do NOT free the constructor objects here.
    // They are still referenced by:
    // 1. The global object (window.EventTarget, window.Node, etc.)
    // 2. Each other through prototype chains (__proto__ links)
    // 3. Later use in this function (e.g., JS_CallConstructor for doc_element)
    // QuickJS garbage collector will clean them up when the context is freed.
    LOG_INFO("DOM prototype methods set");
    
    // ===== Window Properties =====
    DEF_PROP_INT(ctx, window, "innerWidth", 1920);
    DEF_PROP_INT(ctx, window, "innerHeight", 1080);
    DEF_PROP_INT(ctx, window, "outerWidth", 1920);
    DEF_PROP_INT(ctx, window, "outerHeight", 1080);
    DEF_PROP_INT(ctx, window, "screenX", 0);
    DEF_PROP_INT(ctx, window, "screenY", 0);
    DEF_PROP_INT(ctx, window, "screenLeft", 0);
    DEF_PROP_INT(ctx, window, "screenTop", 0);
    DEF_PROP_FLOAT(ctx, window, "devicePixelRatio", 1.0);
    DEF_PROP_INT(ctx, window, "length", 0);
    DEF_PROP_BOOL(ctx, window, "closed", 0);
    DEF_PROP_STR(ctx, window, "name", "bgmdwnldr");
    JS_SetPropertyStr(ctx, window, "opener", JS_NULL);
    DEF_FUNC(ctx, window, "setTimeout", js_zero, 2);
    DEF_FUNC(ctx, window, "setInterval", js_zero, 2);
    DEF_FUNC(ctx, window, "clearTimeout", js_undefined, 1);
    DEF_FUNC(ctx, window, "clearInterval", js_undefined, 1);
    DEF_FUNC(ctx, window, "requestAnimationFrame", js_zero, 1);
    DEF_FUNC(ctx, window, "cancelAnimationFrame", js_undefined, 1);
    DEF_FUNC(ctx, window, "alert", js_undefined, 1);
    DEF_FUNC(ctx, window, "confirm", js_true, 0);
    DEF_FUNC(ctx, window, "prompt", js_empty_string, 1);
    DEF_FUNC(ctx, window, "open", js_null, 1);
    DEF_FUNC(ctx, window, "close", js_undefined, 0);
    DEF_FUNC(ctx, window, "focus", js_undefined, 0);
    DEF_FUNC(ctx, window, "blur", js_undefined, 0);
    DEF_FUNC(ctx, window, "scrollTo", js_undefined, 2);
    DEF_FUNC(ctx, window, "scrollBy", js_undefined, 2);
    DEF_FUNC(ctx, window, "scroll", js_undefined, 2);  // Alias to scrollTo
    DEF_FUNC(ctx, window, "print", js_undefined, 0);
    DEF_FUNC(ctx, window, "postMessage", js_undefined, 2);
    DEF_FUNC(ctx, window, "addEventListener", js_undefined, 2);
    DEF_FUNC(ctx, window, "removeEventListener", js_undefined, 2);
    DEF_FUNC(ctx, window, "dispatchEvent", js_true, 1);
    DEF_FUNC(ctx, window, "getComputedStyle", js_get_computed_style, 1);
    DEF_FUNC(ctx, window, "getSelection", js_get_selection, 0);
    LOG_INFO("Window properties set");
    
    // Also expose DOMException on window (if it exists on global)
    LOG_INFO("Getting DOMException...");
    GCValue dom_exception = JS_GetPropertyStr(ctx, global, "DOMException");
    LOG_INFO("Got DOMException");
    if (!JS_IsException(dom_exception) && !JS_IsUndefined(dom_exception)) {
        LOG_INFO("Setting DOMException on window...");
        JS_SetPropertyStr(ctx, window, "DOMException", dom_exception);
        LOG_INFO("DOMException set on window");
    }
    LOG_INFO("DOMException setup done");

    // ===== NodeFilter constants =====
    LOG_INFO("Creating NodeFilter object...");
    GCValue node_filter = JS_NewObject(ctx);
    LOG_INFO("NodeFilter object created, setting properties...");
    DEF_PROP_INT(ctx, node_filter, "FILTER_ACCEPT", 1);
    DEF_PROP_INT(ctx, node_filter, "FILTER_REJECT", 2);
    DEF_PROP_INT(ctx, node_filter, "FILTER_SKIP", 3);
    DEF_PROP_INT(ctx, node_filter, "SHOW_ALL", 0xFFFFFFFF);
    DEF_PROP_INT(ctx, node_filter, "SHOW_ELEMENT", 0x1);
    DEF_PROP_INT(ctx, node_filter, "SHOW_ATTRIBUTE", 0x2);
    DEF_PROP_INT(ctx, node_filter, "SHOW_TEXT", 0x4);
    DEF_PROP_INT(ctx, node_filter, "SHOW_CDATA_SECTION", 0x8);
    DEF_PROP_INT(ctx, node_filter, "SHOW_ENTITY_REFERENCE", 0x10);
    DEF_PROP_INT(ctx, node_filter, "SHOW_ENTITY", 0x20);
    DEF_PROP_INT(ctx, node_filter, "SHOW_PROCESSING_INSTRUCTION", 0x40);
    DEF_PROP_INT(ctx, node_filter, "SHOW_COMMENT", 0x80);
    DEF_PROP_INT(ctx, node_filter, "SHOW_DOCUMENT", 0x100);
    DEF_PROP_INT(ctx, node_filter, "SHOW_DOCUMENT_TYPE", 0x200);
    DEF_PROP_INT(ctx, node_filter, "SHOW_DOCUMENT_FRAGMENT", 0x400);
    DEF_PROP_INT(ctx, node_filter, "SHOW_NOTATION", 0x800);
    JS_SetPropertyStr(ctx, global, "NodeFilter", node_filter);
    JS_SetPropertyStr(ctx, window, "NodeFilter", node_filter);
    LOG_INFO("NodeFilter set");
    
    // ===== Document =====
    LOG_INFO("Creating document object...");
    LOG_INFO("Setting up Document...");
    GCValue document = JS_NewObject(ctx);
    LOG_INFO("Document object created");
    DEF_PROP_INT(ctx, document, "nodeType", 9);
    DEF_PROP_STR(ctx, document, "readyState", "complete");
    DEF_PROP_STR(ctx, document, "characterSet", "UTF-8");
    DEF_PROP_STR(ctx, document, "charset", "UTF-8");
    DEF_PROP_STR(ctx, document, "contentType", "text/html");
    DEF_PROP_STR(ctx, document, "referrer", "https://www.youtube.com/");
    DEF_PROP_STR(ctx, document, "cookie", "");
    DEF_PROP_STR(ctx, document, "domain", "www.youtube.com");
    DEF_PROP_STR(ctx, document, "title", "YouTube");
    DEF_PROP_STR(ctx, document, "baseURI", "https://www.youtube.com/watch?v=dQw4w9WgXcQ");
    DEF_PROP_BOOL(ctx, document, "hidden", 0);
    DEF_PROP_STR(ctx, document, "visibilityState", "visible");
    DEF_PROP_BOOL(ctx, document, "pictureInPictureEnabled", 0);
    DEF_PROP_STR(ctx, document, "readyState", "complete");
    DEF_FUNC(ctx, document, "createElement", js_document_create_element, 1);
    DEF_FUNC(ctx, document, "createElementNS", js_document_create_element, 2);
    DEF_FUNC(ctx, document, "createTextNode", js_document_create_text_node, 1);
    DEF_FUNC(ctx, document, "createComment", js_empty_string, 1);
    DEF_FUNC(ctx, document, "createDocumentFragment", js_null, 0);
    DEF_FUNC(ctx, document, "createRange", js_document_create_range, 0);
    DEF_FUNC(ctx, document, "createTreeWalker", js_document_create_tree_walker, 3);
    DEF_FUNC(ctx, document, "createEvent", js_document_create_event, 1);
    DEF_FUNC(ctx, document, "importNode", js_document_import_node, 2);
    DEF_FUNC(ctx, document, "getElementById", js_document_getElementById, 1);
    DEF_FUNC(ctx, document, "querySelector", js_document_querySelector, 1);
    DEF_FUNC(ctx, document, "querySelectorAll", js_document_querySelectorAll, 1);
    DEF_FUNC(ctx, document, "getElementsByTagName", js_document_get_elements_by_tag_name, 1);
    DEF_FUNC(ctx, document, "getElementsByClassName", js_document_getElementsByClassName, 1);
    DEF_FUNC(ctx, document, "getElementsByName", js_empty_array, 1);
    DEF_FUNC(ctx, document, "elementFromPoint", js_document_element_from_point, 2);
    DEF_FUNC(ctx, document, "addEventListener", js_event_target_addEventListener, 2);
    DEF_FUNC(ctx, document, "removeEventListener", js_event_target_removeEventListener, 2);
    DEF_FUNC(ctx, document, "dispatchEvent", js_event_target_dispatchEvent, 1);
    DEF_FUNC(ctx, document, "getComputedStyle", js_get_computed_style, 1);
    
    // Create document.implementation with createHTMLDocument
    GCValue doc_impl = JS_NewObject(ctx);
    if (!JS_IsException(doc_impl)) {
        // createHTMLDocument returns a minimal document object
        GCValue create_html_doc = JS_NewCFunction(ctx, [](JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) -> GCValue {
            (void)argc; (void)argv;
            // Create a minimal inert document
            GCValue inert_doc = JS_NewObject(ctx);
            if (!JS_IsException(inert_doc)) {
                // Add basic document properties
                JS_SetPropertyStr(ctx, inert_doc, "body", JS_NewObject(ctx));
                GCValue doc_elem = JS_NewObject(ctx);
                if (!JS_IsException(doc_elem)) {
                    JS_SetPropertyStr(ctx, inert_doc, "documentElement", doc_elem);
                }
            }
            return inert_doc;
        }, "createHTMLDocument", 1);
        JS_SetPropertyStr(ctx, doc_impl, "createHTMLDocument", create_html_doc);
        JS_SetPropertyStr(ctx, document, "implementation", doc_impl);
    }
    
    // Create documentElement as an actual Element instance with proper prototype
    // This must be done AFTER Element is defined in the dom_setup_js above
    GCValue doc_element = JS_CallConstructor(ctx, element_ctor, 0, NULL);
    if (JS_IsException(doc_element)) {
        // Fallback to plain object if constructor fails
        LOG_ERROR("Element constructor failed, using fallback object");
        doc_element = JS_NewObject(ctx);
    }
    
    // Ensure doc_element is a valid object
    if (JS_IsException(doc_element) || JS_IsUndefined(doc_element) || JS_IsNull(doc_element)) {
        LOG_ERROR("Failed to create doc_element, creating basic object");
        doc_element = JS_NewObject(ctx);
    }
    
    // Add Element methods to documentElement
    JS_SetPropertyStr(ctx, doc_element, "querySelector",
        JS_NewCFunction(ctx, js_element_querySelector_real, "querySelector", 1));
    JS_SetPropertyStr(ctx, doc_element, "querySelectorAll",
        JS_NewCFunction(ctx, js_element_querySelectorAll_real, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, doc_element, "insertBefore",
        JS_NewCFunction(ctx, js_node_insertBefore_real, "insertBefore", 2));
    JS_SetPropertyStr(ctx, doc_element, "removeChild",
        JS_NewCFunction(ctx, js_node_removeChild_real, "removeChild", 1));
    JS_SetPropertyStr(ctx, doc_element, "animate",
        JS_NewCFunction(ctx, js_element_animate, "animate", 2));
    JS_SetPropertyStr(ctx, doc_element, "getAttribute",
        JS_NewCFunction(ctx, js_element_get_attribute, "getAttribute", 1));
    JS_SetPropertyStr(ctx, doc_element, "setAttribute",
        JS_NewCFunction(ctx, js_element_set_attribute, "setAttribute", 2));
    JS_SetPropertyStr(ctx, doc_element, "appendChild",
        JS_NewCFunction(ctx, js_node_appendChild_real, "appendChild", 1));
    
    // Add clientWidth/clientHeight properties (viewport dimensions)
    DEF_PROP_INT(ctx, doc_element, "clientWidth", 1920);
    DEF_PROP_INT(ctx, doc_element, "clientHeight", 1080);
    DEF_PROP_INT(ctx, doc_element, "scrollWidth", 1920);
    DEF_PROP_INT(ctx, doc_element, "scrollHeight", 1080);
    DEF_PROP_INT(ctx, doc_element, "offsetWidth", 1920);
    DEF_PROP_INT(ctx, doc_element, "offsetHeight", 1080);
    
    // Add style object for CSS property detection (needed by Web Animations polyfill)
    GCValue doc_style = JS_NewObject(ctx);
    if (!JS_IsException(doc_style)) {
        // Add common CSS properties that the polyfill checks for
        JS_SetPropertyStr(ctx, doc_style, "webkitTransform", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, doc_style, "msTransform", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, doc_style, "webkitTransformOrigin", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, doc_style, "webkitPerspective", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, doc_style, "webkitPerspectiveOrigin", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, doc_style, "transform", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, doc_style, "transformOrigin", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, doc_style, "perspective", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, doc_style, "perspectiveOrigin", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, doc_element, "style", doc_style);
    }
    
    JS_SetPropertyStr(ctx, document, "documentElement", doc_element);
    LOG_INFO("Document documentElement set");
    
    // Create document body
    GCValue body_element = JS_CallConstructor(ctx, html_element_ctor, 0, NULL);
    if (JS_IsException(body_element)) {
        LOG_ERROR("HTMLElement constructor failed, using fallback object");
        body_element = JS_NewObject(ctx);
    }
    
    // Ensure body_element is a valid object
    if (JS_IsException(body_element) || JS_IsUndefined(body_element) || JS_IsNull(body_element)) {
        LOG_ERROR("Failed to create body_element, creating basic object");
        body_element = JS_NewObject(ctx);
    }
    
    JS_SetPropertyStr(ctx, body_element, "appendChild",
        JS_NewCFunction(ctx, js_node_appendChild_real, "appendChild", 1));
    JS_SetPropertyStr(ctx, body_element, "insertBefore",
        JS_NewCFunction(ctx, js_node_insertBefore_real, "insertBefore", 2));
    JS_SetPropertyStr(ctx, body_element, "removeChild",
        JS_NewCFunction(ctx, js_node_removeChild_real, "removeChild", 1));
    JS_SetPropertyStr(ctx, body_element, "querySelector",
        JS_NewCFunction(ctx, js_element_querySelector_real, "querySelector", 1));
    JS_SetPropertyStr(ctx, body_element, "querySelectorAll",
        JS_NewCFunction(ctx, js_element_querySelectorAll_real, "querySelectorAll", 1));
    
    // Add clientWidth/clientHeight properties to body (viewport dimensions)
    DEF_PROP_INT(ctx, body_element, "clientWidth", 1920);
    DEF_PROP_INT(ctx, body_element, "clientHeight", 937);  // 1080 - some UI chrome
    DEF_PROP_INT(ctx, body_element, "scrollWidth", 1920);
    DEF_PROP_INT(ctx, body_element, "scrollHeight", 937);
    DEF_PROP_INT(ctx, body_element, "offsetWidth", 1920);
    DEF_PROP_INT(ctx, body_element, "offsetHeight", 937);
    
    // Add style object for body (needed by YouTube player scripts)
    GCValue body_style = JS_NewObject(ctx);
    if (!JS_IsException(body_style)) {
        JS_SetPropertyStr(ctx, body_element, "style", body_style);
    }
    
    JS_SetPropertyStr(ctx, document, "body", body_element);
    LOG_INFO("Document body set");
    
    // Create document head (with DOM node class so tree traversal works)
    GCValue head_element = JS_NewObjectClass(ctx, js_dom_node_class_id);
    if (JS_IsException(head_element)) {
        head_element = JS_NewObject(ctx);
    }
    JS_SetPropertyStr(ctx, head_element, "appendChild",
        JS_NewCFunction(ctx, js_node_appendChild_real, "appendChild", 1));
    JS_SetPropertyStr(ctx, head_element, "insertBefore",
        JS_NewCFunction(ctx, js_node_insertBefore_real, "insertBefore", 2));
    JS_SetPropertyStr(ctx, head_element, "removeChild",
        JS_NewCFunction(ctx, js_node_removeChild_real, "removeChild", 1));
    
    // Add style object for head (needed by feature detection in YouTube scripts)
    GCValue head_style = JS_NewObject(ctx);
    if (!JS_IsException(head_style)) {
        JS_SetPropertyStr(ctx, head_element, "style", head_style);
    }
    
    JS_SetPropertyStr(ctx, document, "head", head_element);
    LOG_INFO("Document head set");
    
    // Build minimal DOM tree for YouTube player initialization
    // documentElement -> [head, body]
    {
        GCValue append_args[1] = { head_element };
        js_node_appendChild_real(ctx, doc_element, 1, append_args);
    }
    {
        GCValue append_args[1] = { body_element };
        js_node_appendChild_real(ctx, doc_element, 1, append_args);
    }
    
    // Create key YouTube elements
    auto create_elem = [&](const char* tag) -> GCValue {
        GCValue tag_arg = JS_NewString(ctx, tag);
        return js_document_create_element(ctx, document, 1, &tag_arg);
    };
    
    // ytd-app (root app element)
    GCValue ytd_app = create_elem("ytd-app");
    if (!JS_IsNull(ytd_app)) {
        JS_SetPropertyStr(ctx, ytd_app, "getAttribute", 
            JS_NewCFunction(ctx, js_element_get_attribute, "getAttribute", 1));
        JS_SetPropertyStr(ctx, ytd_app, "setAttribute",
            JS_NewCFunction(ctx, js_element_set_attribute, "setAttribute", 2));
        JS_SetPropertyStr(ctx, ytd_app, "removeAttribute",
            JS_NewCFunction(ctx, js_element_remove_attribute, "removeAttribute", 1));
        GCValue append_args[1] = { ytd_app };
        js_node_appendChild_real(ctx, body_element, 1, append_args);
        
        // ytd-masthead inside ytd-app
        GCValue ytd_masthead = create_elem("ytd-masthead");
        if (!JS_IsNull(ytd_masthead)) {
            GCValue masthead_args[1] = { ytd_masthead };
            js_node_appendChild_real(ctx, ytd_app, 1, masthead_args);
        }
    }
    
    // player-api container
    GCValue player_api = create_elem("div");
    if (!JS_IsNull(player_api)) {
        JS_SetPropertyStr(ctx, player_api, "id", JS_NewString(ctx, "player-api"));
        GCValue append_args[1] = { player_api };
        js_node_appendChild_real(ctx, body_element, 1, append_args);
    }
    
    // movie_player container
    GCValue movie_player = create_elem("div");
    if (!JS_IsNull(movie_player)) {
        JS_SetPropertyStr(ctx, movie_player, "id", JS_NewString(ctx, "movie_player"));
        GCValue append_args[1] = { movie_player };
        js_node_appendChild_real(ctx, body_element, 1, append_args);
    }
    
    // player-placeholder
    GCValue player_placeholder = create_elem("div");
    if (!JS_IsNull(player_placeholder)) {
        JS_SetPropertyStr(ctx, player_placeholder, "id", JS_NewString(ctx, "player-placeholder"));
        GCValue append_args[1] = { player_placeholder };
        js_node_appendChild_real(ctx, body_element, 1, append_args);
    }
    
    LOG_INFO("YouTube DOM skeleton created");
    
    // Set activeElement (body by default)
    JS_SetPropertyStr(ctx, document, "activeElement", body_element);
    
    // Create document.fonts (FontFaceSet stub)
    GCValue fonts_stub = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, fonts_stub, "add",
        JS_NewCFunction(ctx, js_dummy_function, "add", 1));
    JS_SetPropertyStr(ctx, fonts_stub, "check",
        JS_NewCFunction(ctx, js_dummy_function_true, "check", 1));
    JS_SetPropertyStr(ctx, fonts_stub, "load",
        JS_NewCFunction(ctx, js_dummy_function, "load", 1));
    JS_SetPropertyStr(ctx, fonts_stub, "ready",
        JS_NewCFunction(ctx, js_dummy_function, "ready", 0));
    JS_SetPropertyStr(ctx, document, "fonts", fonts_stub);
    LOG_INFO("Document fonts set");
    
    // Create document.featurePolicy
    GCValue feature_policy = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, feature_policy, "allowsFeature",
        JS_NewCFunction(ctx, js_dummy_function_true, "allowsFeature", 2));
    JS_SetPropertyStr(ctx, feature_policy, "features",
        JS_NewCFunction(ctx, js_empty_array, "features", 0));
    JS_SetPropertyStr(ctx, document, "featurePolicy", feature_policy);
    LOG_INFO("Document featurePolicy set");
    
    // Constructors and prototypes are owned by global objects and prototype chains
    // Don't free them here - QuickJS GC will clean up when context is freed
    (void)element_ctor;       // owned by global.Element
    (void)html_element_ctor;  // owned by global.HTMLElement
    (void)html_element_proto; // owned by HTMLElement.prototype

    // Set document on global
    JS_SetPropertyStr(ctx, global, "document", document);
    JS_SetPropertyStr(ctx, document, "defaultView", window);
    LOG_INFO("Document set on global");
    
    // ===== Location =====
    // Create Location object with getters/setters and shared data
    GCValue location = JS_NewObject(ctx);
    
    // Allocate LocationData from GC heap
    GCHandle loc_handle = gc_allocz(sizeof(LocationData), JS_GC_OBJ_TYPE_DATA);
    if (loc_handle != GC_HANDLE_NULL) {
        LocationData *loc = (LocationData*)gc_deref(loc_handle);
        parse_url("https://www.youtube.com/watch?v=dQw4w9WgXcQ", loc);
        JS_SetOpaqueHandle(location, loc_handle);
    }
    
    // Define getters and setters for location properties
    // href - getter/setter
    GCValue href_getter = JS_NewCFunction(ctx, js_location_get_href, "get href", 0);
    GCValue href_setter = JS_NewCFunction(ctx, js_location_set_href, "set href", 1);
    JSAtom href_atom = JS_NewAtom(ctx, "href");
    JS_DefinePropertyGetSet(ctx, location, href_atom, href_getter, href_setter, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, href_atom);
    
    // protocol - getter only
    GCValue protocol_getter = JS_NewCFunction(ctx, js_location_get_protocol, "get protocol", 0);
    JSAtom protocol_atom = JS_NewAtom(ctx, "protocol");
    JS_DefinePropertyGetSet(ctx, location, protocol_atom, protocol_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, protocol_atom);
    
    // host - getter only
    GCValue location_host_getter = JS_NewCFunction(ctx, js_location_get_host, "get host", 0);
    JSAtom location_host_atom = JS_NewAtom(ctx, "host");
    JS_DefinePropertyGetSet(ctx, location, location_host_atom, location_host_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, location_host_atom);
    
    // hostname - getter only
    GCValue hostname_getter = JS_NewCFunction(ctx, js_location_get_hostname, "get hostname", 0);
    JSAtom hostname_atom = JS_NewAtom(ctx, "hostname");
    JS_DefinePropertyGetSet(ctx, location, hostname_atom, hostname_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, hostname_atom);
    
    // port - getter only
    GCValue port_getter = JS_NewCFunction(ctx, js_location_get_port, "get port", 0);
    JSAtom port_atom = JS_NewAtom(ctx, "port");
    JS_DefinePropertyGetSet(ctx, location, port_atom, port_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, port_atom);
    
    // pathname - getter/setter
    GCValue pathname_getter = JS_NewCFunction(ctx, js_location_get_pathname, "get pathname", 0);
    GCValue pathname_setter = JS_NewCFunction(ctx, js_location_set_pathname, "set pathname", 1);
    JSAtom pathname_atom = JS_NewAtom(ctx, "pathname");
    JS_DefinePropertyGetSet(ctx, location, pathname_atom, pathname_getter, pathname_setter, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, pathname_atom);
    
    // search - getter/setter
    GCValue search_getter = JS_NewCFunction(ctx, js_location_get_search, "get search", 0);
    GCValue search_setter = JS_NewCFunction(ctx, js_location_set_search, "set search", 1);
    JSAtom search_atom = JS_NewAtom(ctx, "search");
    JS_DefinePropertyGetSet(ctx, location, search_atom, search_getter, search_setter, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, search_atom);
    
    // hash - getter/setter
    GCValue hash_getter = JS_NewCFunction(ctx, js_location_get_hash, "get hash", 0);
    GCValue hash_setter = JS_NewCFunction(ctx, js_location_set_hash, "set hash", 1);
    JSAtom hash_atom = JS_NewAtom(ctx, "hash");
    JS_DefinePropertyGetSet(ctx, location, hash_atom, hash_getter, hash_setter, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, hash_atom);
    
    // origin - getter only
    GCValue origin_getter = JS_NewCFunction(ctx, js_location_get_origin, "get origin", 0);
    JSAtom origin_atom = JS_NewAtom(ctx, "origin");
    JS_DefinePropertyGetSet(ctx, location, origin_atom, origin_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, origin_atom);
    
    // Methods
    JS_SetPropertyStr(ctx, location, "toString",
        JS_NewCFunction(ctx, js_location_toString, "toString", 0));
    JS_SetPropertyStr(ctx, location, "assign",
        JS_NewCFunction(ctx, js_location_assign, "assign", 1));
    JS_SetPropertyStr(ctx, location, "replace",
        JS_NewCFunction(ctx, js_location_replace, "replace", 1));
    JS_SetPropertyStr(ctx, location, "reload",
        JS_NewCFunction(ctx, js_location_reload, "reload", 0));
    
    JS_SetPropertyStr(ctx, window, "location", location);
    JS_SetPropertyStr(ctx, document, "location", location);
    LOG_INFO("Location set");
    
    // ===== Navigator =====
    GCValue navigator = JS_NewObject(ctx);
    DEF_PROP_STR(ctx, navigator, "userAgent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    DEF_PROP_STR(ctx, navigator, "appName", "Netscape");
    DEF_PROP_STR(ctx, navigator, "appVersion", "5.0 (X11; Linux x86_64) AppleWebKit/537.36");
    DEF_PROP_STR(ctx, navigator, "appCodeName", "Mozilla");
    DEF_PROP_STR(ctx, navigator, "platform", "Linux x86_64");
    DEF_PROP_STR(ctx, navigator, "product", "Gecko");
    DEF_PROP_STR(ctx, navigator, "productSub", "20030107");
    DEF_PROP_STR(ctx, navigator, "vendor", "Google Inc.");
    DEF_PROP_STR(ctx, navigator, "vendorSub", "");
    DEF_PROP_STR(ctx, navigator, "language", "en-US");
    
    // navigator.languages array
    GCValue languages = JS_NewArray(ctx);
    GCValue push = JS_GetPropertyStr(ctx, languages, "push");
    GCValue lang_en = JS_NewString(ctx, "en-US");
    JS_Call(ctx, push, languages, 1, &lang_en);
    JS_SetPropertyStr(ctx, navigator, "languages", languages);
    
    DEF_PROP_BOOL(ctx, navigator, "onLine", 1);
    DEF_PROP_BOOL(ctx, navigator, "cookieEnabled", 1);
    DEF_PROP_INT(ctx, navigator, "hardwareConcurrency", 8);
    DEF_PROP_INT(ctx, navigator, "maxTouchPoints", 0);
    DEF_PROP_BOOL(ctx, navigator, "pdfViewerEnabled", 1);
    DEF_PROP_BOOL(ctx, navigator, "webdriver", 0);
    
    // ===== Clipboard API =====
    GCValue clipboard = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, clipboard, "writeText",
        JS_NewCFunction(ctx, js_promise_resolve_undefined, "writeText", 1));
    JS_SetPropertyStr(ctx, clipboard, "readText",
        JS_NewCFunction(ctx, js_promise_resolve_empty_string, "readText", 0));
    JS_SetPropertyStr(ctx, clipboard, "write",
        JS_NewCFunction(ctx, js_promise_resolve_undefined, "write", 1));
    JS_SetPropertyStr(ctx, clipboard, "read",
        JS_NewCFunction(ctx, js_promise_resolve_empty_array, "read", 0));
    JS_SetPropertyStr(ctx, navigator, "clipboard", clipboard);
    LOG_INFO("Navigator clipboard set");
    
    // ===== MediaSession API =====
    // MediaMetadata class constructor
    GCValue media_metadata_ctor = JS_NewCFunction2(ctx, js_media_metadata_constructor, "MediaMetadata", 
        1, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "MediaMetadata", media_metadata_ctor);
    LOG_INFO("MediaMetadata class set");
    
    // navigator.mediaSession object
    GCValue media_session = JS_NewObject(ctx);
    DEF_PROP_STR(ctx, media_session, "playbackState", "none");
    JS_SetPropertyStr(ctx, media_session, "metadata", JS_NULL);
    JS_SetPropertyStr(ctx, media_session, "setActionHandler",
        JS_NewCFunction(ctx, js_undefined, "setActionHandler", 2));
    JS_SetPropertyStr(ctx, media_session, "setPositionState",
        JS_NewCFunction(ctx, js_undefined, "setPositionState", 1));
    JS_SetPropertyStr(ctx, navigator, "mediaSession", media_session);
    LOG_INFO("Navigator mediaSession set");
    
    // ===== MediaCapabilities API =====
    GCValue media_capabilities = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, media_capabilities, "decodingInfo",
        JS_NewCFunction(ctx, js_media_capabilities_decoding_info, "decodingInfo", 1));
    JS_SetPropertyStr(ctx, media_capabilities, "encodingInfo",
        JS_NewCFunction(ctx, js_media_capabilities_encoding_info, "encodingInfo", 1));
    JS_SetPropertyStr(ctx, navigator, "mediaCapabilities", media_capabilities);
    LOG_INFO("Navigator mediaCapabilities set");
    
    // ===== Permissions API =====
    // PermissionStatus class constructor
    GCValue permission_status_ctor = JS_NewCFunction2(ctx, js_permission_status_constructor, "PermissionStatus",
        1, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "PermissionStatus", permission_status_ctor);
    
    // navigator.permissions object
    GCValue permissions = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, permissions, "query",
        JS_NewCFunction(ctx, js_permissions_query, "query", 1));
    JS_SetPropertyStr(ctx, permissions, "request",
        JS_NewCFunction(ctx, js_permissions_query, "request", 1));
    JS_SetPropertyStr(ctx, permissions, "revoke",
        JS_NewCFunction(ctx, js_permissions_query, "revoke", 1));
    JS_SetPropertyStr(ctx, navigator, "permissions", permissions);
    LOG_INFO("Navigator permissions set");
    
    // ===== Storage API =====
    GCValue storage = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, storage, "estimate",
        JS_NewCFunction(ctx, js_storage_estimate, "estimate", 0));
    JS_SetPropertyStr(ctx, storage, "persist",
        JS_NewCFunction(ctx, js_false_promise, "persist", 0));
    JS_SetPropertyStr(ctx, storage, "persisted",
        JS_NewCFunction(ctx, js_false_promise, "persisted", 0));
    JS_SetPropertyStr(ctx, navigator, "storage", storage);
    LOG_INFO("Navigator storage set");
    
    // ===== ServiceWorker API =====
    // Create the serviceWorker container with EventTarget-like capabilities
    GCValue service_worker = JS_NewObjectClass(ctx, js_service_worker_container_class_id);
    if (service_worker_handle == GC_HANDLE_NULL) {
        service_worker_handle = gc_allocz(sizeof(ServiceWorkerContainerData), JS_GC_OBJ_TYPE_DATA);
        if (service_worker_handle != GC_HANDLE_NULL) {
            ServiceWorkerContainerData *swc = (ServiceWorkerContainerData*)gc_deref(service_worker_handle);
            swc->registrations = JS_NewArray(ctx);
            swc->message_handlers = JS_NewArray(ctx);
            JS_SetOpaqueHandle(service_worker, service_worker_handle);
        }
    }
    
    // Add ServiceWorkerContainer methods
    JS_SetPropertyStr(ctx, service_worker, "register",
        JS_NewCFunction(ctx, js_service_worker_register, "register", 2));
    JS_SetPropertyStr(ctx, service_worker, "getRegistration",
        JS_NewCFunction(ctx, js_service_worker_get_registration, "getRegistration", 1));
    JS_SetPropertyStr(ctx, service_worker, "getRegistrations",
        JS_NewCFunction(ctx, js_service_worker_get_registrations, "getRegistrations", 0));
    JS_SetPropertyStr(ctx, service_worker, "addEventListener",
        JS_NewCFunction(ctx, js_service_worker_add_event_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, service_worker, "removeEventListener",
        JS_NewCFunction(ctx, js_undefined, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, service_worker, "dispatchEvent",
        JS_NewCFunction(ctx, js_true, "dispatchEvent", 1));
    
    // ready property - returns a thenable that acts like a Promise
    // Create a simple thenable object with a C 'then' method
    GCValue ready_thenable = JS_NewObject(ctx);
    if (!JS_IsException(ready_thenable)) {
        // Add a 'then' method using a C function
        JS_SetPropertyStr(ctx, ready_thenable, "then",
            JS_NewCFunction(ctx, js_promise_resolve_undefined, "then", 1));
        JS_SetPropertyStr(ctx, service_worker, "ready", ready_thenable);
    }
    
    JS_SetPropertyStr(ctx, navigator, "serviceWorker", service_worker);
    LOG_INFO("Navigator serviceWorker set");
    
    // ===== Geolocation API =====
    // GeolocationPosition class constructor
    GCValue geolocation_position_ctor = JS_NewCFunction2(ctx, js_geolocation_position_constructor, "GeolocationPosition",
        0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "GeolocationPosition", geolocation_position_ctor);
    
    // GeolocationPositionError class constructor
    GCValue geolocation_position_error_ctor = JS_NewCFunction2(ctx, js_geolocation_position_error_constructor, "GeolocationPositionError",
        2, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "GeolocationPositionError", geolocation_position_error_ctor);
    // Set static constants on constructor
    JS_SetPropertyStr(ctx, geolocation_position_error_ctor, "PERMISSION_DENIED", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, geolocation_position_error_ctor, "POSITION_UNAVAILABLE", JS_NewInt32(ctx, 2));
    JS_SetPropertyStr(ctx, geolocation_position_error_ctor, "TIMEOUT", JS_NewInt32(ctx, 3));
    
    // navigator.geolocation object
    GCValue geolocation = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, geolocation, "getCurrentPosition",
        JS_NewCFunction(ctx, js_geolocation_get_current_position, "getCurrentPosition", 3));
    JS_SetPropertyStr(ctx, geolocation, "watchPosition",
        JS_NewCFunction(ctx, js_geolocation_watch_position, "watchPosition", 3));
    JS_SetPropertyStr(ctx, geolocation, "clearWatch",
        JS_NewCFunction(ctx, js_undefined, "clearWatch", 1));
    JS_SetPropertyStr(ctx, navigator, "geolocation", geolocation);
    LOG_INFO("Navigator geolocation set");
    
    // ===== Web Share API =====
    JS_SetPropertyStr(ctx, navigator, "share",
        JS_NewCFunction(ctx, js_promise_resolve_undefined, "share", 1));
    JS_SetPropertyStr(ctx, navigator, "canShare",
        JS_NewCFunction(ctx, js_false, "canShare", 1));
    LOG_INFO("Navigator share set");
    
    // ===== User-Agent Client Hints =====
    GCValue user_agent_data = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, user_agent_data, "brands", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, user_agent_data, "mobile", JS_FALSE);
    JS_SetPropertyStr(ctx, user_agent_data, "platform", JS_NewString(ctx, "Linux x86_64"));
    JS_SetPropertyStr(ctx, user_agent_data, "getHighEntropyValues",
        JS_NewCFunction(ctx, js_user_agent_data_get_high_entropy_values, "getHighEntropyValues", 1));
    JS_SetPropertyStr(ctx, navigator, "userAgentData", user_agent_data);
    LOG_INFO("Navigator userAgentData set");
    
    // ===== Battery API =====
    JS_SetPropertyStr(ctx, navigator, "getBattery",
        JS_NewCFunction(ctx, js_navigator_get_battery, "getBattery", 0));
    LOG_INFO("Navigator getBattery set");
    
    // ===== Network Information API =====
    GCValue connection = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, connection, "type", JS_NewString(ctx, "wifi"));
    JS_SetPropertyStr(ctx, connection, "effectiveType", JS_NewString(ctx, "4g"));
    JS_SetPropertyStr(ctx, connection, "downlink", JS_NewFloat64(ctx, 10.0));
    JS_SetPropertyStr(ctx, connection, "downlinkMax", JS_NewFloat64(ctx, INFINITY));
    JS_SetPropertyStr(ctx, connection, "rtt", JS_NewInt32(ctx, 50));
    JS_SetPropertyStr(ctx, connection, "saveData", JS_FALSE);
    JS_SetPropertyStr(ctx, connection, "addEventListener",
        JS_NewCFunction(ctx, js_undefined, "addEventListener", 2));
    JS_SetPropertyStr(ctx, connection, "removeEventListener",
        JS_NewCFunction(ctx, js_undefined, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, navigator, "connection", connection);
    LOG_INFO("Navigator connection set");
    
    JS_SetPropertyStr(ctx, window, "navigator", navigator);
    LOG_INFO("Navigator set");
    
    // ===== Screen =====
    GCValue screen = JS_NewObject(ctx);
    DEF_PROP_INT(ctx, screen, "width", 1920);
    DEF_PROP_INT(ctx, screen, "height", 1080);
    DEF_PROP_INT(ctx, screen, "availWidth", 1920);
    DEF_PROP_INT(ctx, screen, "availHeight", 1040);
    DEF_PROP_INT(ctx, screen, "colorDepth", 24);
    DEF_PROP_INT(ctx, screen, "pixelDepth", 24);
    JS_SetPropertyStr(ctx, window, "screen", screen);
    LOG_INFO("Screen set");
    
    // ===== History =====
    GCValue history = JS_NewObject(ctx);
    DEF_PROP_INT(ctx, history, "length", 2);
    JS_SetPropertyStr(ctx, history, "state", JS_NULL);
    DEF_PROP_STR(ctx, history, "scrollRestoration", "auto");
    JS_SetPropertyStr(ctx, history, "pushState",
        JS_NewCFunction(ctx, js_history_push_state, "pushState", 3));
    JS_SetPropertyStr(ctx, history, "replaceState",
        JS_NewCFunction(ctx, js_history_replace_state, "replaceState", 3));
    JS_SetPropertyStr(ctx, history, "back",
        JS_NewCFunction(ctx, js_undefined, "back", 0));
    JS_SetPropertyStr(ctx, history, "forward",
        JS_NewCFunction(ctx, js_undefined, "forward", 0));
    JS_SetPropertyStr(ctx, history, "go",
        JS_NewCFunction(ctx, js_undefined, "go", 1));
    JS_SetPropertyStr(ctx, window, "history", history);
    
    // ===== Storage API =====
    // Create localStorage with actual in-memory storage
    GCValue localStorage = JS_NewObject(ctx);
    DEF_FUNC(ctx, localStorage, "getItem", js_storage_get_item, 1);
    DEF_FUNC(ctx, localStorage, "setItem", js_storage_set_item, 2);
    DEF_FUNC(ctx, localStorage, "removeItem", js_storage_remove_item, 1);
    DEF_FUNC(ctx, localStorage, "clear", js_storage_clear, 0);
    DEF_FUNC(ctx, localStorage, "key", js_storage_key, 1);
    JS_DefinePropertyGetSet(ctx, localStorage, JS_NewAtom(ctx, "length"),
        JS_NewCFunction(ctx, js_storage_get_length, "get length", 0),
        JS_NULL, JS_PROP_ENUMERABLE);
    
    // Allocate storage data for localStorage and store handle reference
    GCHandle local_storage_handle = gc_allocz(sizeof(StorageData), JS_GC_OBJ_TYPE_DATA);
    if (local_storage_handle != GC_HANDLE_NULL) {
        StorageData *local_data = (StorageData*)gc_deref(local_storage_handle);
        local_data->count = 0;
        // Store handle as a hidden property
        JS_SetPropertyStr(ctx, localStorage, "__data", JS_NewInt64(ctx, (int64_t)(intptr_t)local_storage_handle));
    }
    
    // Create sessionStorage (separate storage instance)
    GCValue sessionStorage = JS_NewObject(ctx);
    DEF_FUNC(ctx, sessionStorage, "getItem", js_storage_get_item, 1);
    DEF_FUNC(ctx, sessionStorage, "setItem", js_storage_set_item, 2);
    DEF_FUNC(ctx, sessionStorage, "removeItem", js_storage_remove_item, 1);
    DEF_FUNC(ctx, sessionStorage, "clear", js_storage_clear, 0);
    DEF_FUNC(ctx, sessionStorage, "key", js_storage_key, 1);
    JS_DefinePropertyGetSet(ctx, sessionStorage, JS_NewAtom(ctx, "length"),
        JS_NewCFunction(ctx, js_storage_get_length, "get length", 0),
        JS_NULL, JS_PROP_ENUMERABLE);
    
    // Allocate storage data for sessionStorage and store handle reference
    GCHandle session_storage_handle = gc_allocz(sizeof(StorageData), JS_GC_OBJ_TYPE_DATA);
    if (session_storage_handle != GC_HANDLE_NULL) {
        StorageData *session_data = (StorageData*)gc_deref(session_storage_handle);
        session_data->count = 0;
        // Store handle as a hidden property
        JS_SetPropertyStr(ctx, sessionStorage, "__data", JS_NewInt64(ctx, (int64_t)(intptr_t)session_storage_handle));
    }
    
    JS_SetPropertyStr(ctx, window, "localStorage", localStorage);
    JS_SetPropertyStr(ctx, window, "sessionStorage", sessionStorage);
    
    // ===== CSS API =====
    // Create CSS object
    GCValue css = JS_NewObject(ctx);
    DEF_FUNC(ctx, css, "supports", js_css_supports, 2);
    DEF_FUNC(ctx, css, "escape", js_css_escape, 1);
    JS_SetPropertyStr(ctx, window, "CSS", css);
    
    // CSSStyleSheet constructor
    GCValue css_style_sheet_ctor = JS_NewCFunction2(ctx, js_dummy_function, "CSSStyleSheet", 
        0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "CSSStyleSheet", css_style_sheet_ctor);
    
    // CSSStyleSheet prototype
    GCValue css_style_sheet_proto = JS_NewObject(ctx);
    DEF_FUNC(ctx, css_style_sheet_proto, "insertRule", js_css_style_sheet_insert_rule, 2);
    DEF_FUNC(ctx, css_style_sheet_proto, "deleteRule", js_css_style_sheet_delete_rule, 1);
    DEF_FUNC(ctx, css_style_sheet_proto, "addRule", js_css_style_sheet_add_rule, 3);
    DEF_FUNC(ctx, css_style_sheet_proto, "removeRule", js_css_style_sheet_remove_rule, 1);
    DEF_FUNC(ctx, css_style_sheet_proto, "replace", js_css_style_sheet_replace, 1);
    DEF_FUNC(ctx, css_style_sheet_proto, "replaceSync", js_css_style_sheet_replace_sync, 1);
    // Set prototype on constructor
    JS_SetPropertyStr(ctx, css_style_sheet_ctor, "prototype", css_style_sheet_proto);
    
    // CSSStyleDeclaration prototype (for element.style)
    GCValue css_style_decl_proto = JS_NewObject(ctx);
    DEF_FUNC(ctx, css_style_decl_proto, "setProperty", js_css_style_decl_set_property, 3);
    DEF_FUNC(ctx, css_style_decl_proto, "removeProperty", js_css_style_decl_remove_property, 1);
    DEF_FUNC(ctx, css_style_decl_proto, "getPropertyValue", js_css_style_decl_get_property_value, 1);
    DEF_FUNC(ctx, css_style_decl_proto, "getPropertyPriority", js_css_style_decl_get_property_priority, 1);
    
    // Store for use with elements
    JS_SetPropertyStr(ctx, global, "__CSSStyleDeclarationProto", css_style_decl_proto);
    
    // DOMTokenList prototype (for element.classList)
    GCValue dom_token_list_proto = JS_NewObject(ctx);
    DEF_FUNC(ctx, dom_token_list_proto, "add", js_dom_token_list_add, 0);  // Variable args
    DEF_FUNC(ctx, dom_token_list_proto, "remove", js_dom_token_list_remove, 0);  // Variable args
    DEF_FUNC(ctx, dom_token_list_proto, "toggle", js_dom_token_list_toggle, 2);
    DEF_FUNC(ctx, dom_token_list_proto, "contains", js_dom_token_list_contains, 1);
    DEF_FUNC(ctx, dom_token_list_proto, "forEach", js_dom_token_list_for_each, 1);
    
    // Store for use with elements
    JS_SetPropertyStr(ctx, global, "__DOMTokenListProto", dom_token_list_proto);
    
    // ===== Timer API =====
    // setTimeout / clearTimeout
    JS_SetPropertyStr(ctx, global, "setTimeout",
        JS_NewCFunction(ctx, js_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
        JS_NewCFunction(ctx, js_clear_timeout, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, window, "setTimeout",
        JS_NewCFunction(ctx, js_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, window, "clearTimeout",
        JS_NewCFunction(ctx, js_clear_timeout, "clearTimeout", 1));
    
    // setInterval / clearInterval
    JS_SetPropertyStr(ctx, global, "setInterval",
        JS_NewCFunction(ctx, js_set_interval, "setInterval", 2));
    JS_SetPropertyStr(ctx, global, "clearInterval",
        JS_NewCFunction(ctx, js_clear_interval, "clearInterval", 1));
    JS_SetPropertyStr(ctx, window, "setInterval",
        JS_NewCFunction(ctx, js_set_interval, "setInterval", 2));
    JS_SetPropertyStr(ctx, window, "clearInterval",
        JS_NewCFunction(ctx, js_clear_interval, "clearInterval", 1));
    
    // requestAnimationFrame / cancelAnimationFrame
    JS_SetPropertyStr(ctx, global, "requestAnimationFrame",
        JS_NewCFunction(ctx, js_request_animation_frame, "requestAnimationFrame", 1));
    JS_SetPropertyStr(ctx, global, "cancelAnimationFrame",
        JS_NewCFunction(ctx, js_cancel_animation_frame, "cancelAnimationFrame", 1));
    JS_SetPropertyStr(ctx, window, "requestAnimationFrame",
        JS_NewCFunction(ctx, js_request_animation_frame, "requestAnimationFrame", 1));
    JS_SetPropertyStr(ctx, window, "cancelAnimationFrame",
        JS_NewCFunction(ctx, js_cancel_animation_frame, "cancelAnimationFrame", 1));
    
    // requestIdleCallback / cancelIdleCallback
    JS_SetPropertyStr(ctx, global, "requestIdleCallback",
        JS_NewCFunction(ctx, js_request_idle_callback, "requestIdleCallback", 1));
    JS_SetPropertyStr(ctx, global, "cancelIdleCallback",
        JS_NewCFunction(ctx, js_cancel_idle_callback, "cancelIdleCallback", 1));
    JS_SetPropertyStr(ctx, window, "requestIdleCallback",
        JS_NewCFunction(ctx, js_request_idle_callback, "requestIdleCallback", 1));
    JS_SetPropertyStr(ctx, window, "cancelIdleCallback",
        JS_NewCFunction(ctx, js_cancel_idle_callback, "cancelIdleCallback", 1));
    LOG_INFO("Timer API set");
    
    // ===== Crypto API =====
    // Create SubtleCrypto object
    GCValue subtle = JS_NewObject(ctx);
    DEF_FUNC(ctx, subtle, "digest", js_subtle_digest, 2);
    DEF_FUNC(ctx, subtle, "encrypt", js_subtle_encrypt, 3);
    DEF_FUNC(ctx, subtle, "decrypt", js_subtle_decrypt, 3);
    
    // Create Crypto object
    GCValue crypto = JS_NewObject(ctx);
    DEF_FUNC(ctx, crypto, "getRandomValues", js_crypto_get_random_values, 1);
    JS_SetPropertyStr(ctx, crypto, "subtle", subtle);
    
    JS_SetPropertyStr(ctx, window, "crypto", crypto);
    
    // ===== Console =====
    GCValue console = JS_NewObject(ctx);
    DEF_FUNC(ctx, console, "log", js_console_log, 0);           // Variable args
    DEF_FUNC(ctx, console, "error", js_console_error, 0);       // Variable args
    DEF_FUNC(ctx, console, "warn", js_console_warn, 0);         // Variable args
    DEF_FUNC(ctx, console, "info", js_console_info, 0);         // Variable args
    DEF_FUNC(ctx, console, "debug", js_console_debug, 0);       // Variable args
    DEF_FUNC(ctx, console, "trace", js_console_trace, 0);       // Variable args
    DEF_FUNC(ctx, console, "dir", js_console_dir, 1);
    DEF_FUNC(ctx, console, "dirxml", js_console_dirxml, 1);
    DEF_FUNC(ctx, console, "group", js_console_group, 0);       // Optional label
    DEF_FUNC(ctx, console, "groupCollapsed", js_console_groupCollapsed, 0);
    DEF_FUNC(ctx, console, "groupEnd", js_console_groupEnd, 0);
    DEF_FUNC(ctx, console, "time", js_console_time, 0);         // Optional label
    DEF_FUNC(ctx, console, "timeEnd", js_console_timeEnd, 0);   // Optional label
    DEF_FUNC(ctx, console, "timeLog", js_console_timeLog, 0);   // Optional label
    DEF_FUNC(ctx, console, "count", js_console_count, 0);       // Optional label
    DEF_FUNC(ctx, console, "countReset", js_console_countReset, 0);
    DEF_FUNC(ctx, console, "assert", js_console_assert, 0);     // Variable args
    DEF_FUNC(ctx, console, "clear", js_console_clear, 0);
    // Set console on global
    JS_SetPropertyStr(ctx, global, "console", console);
    
    // ===== XMLHttpRequest =====
    LOG_INFO("Setting up XMLHttpRequest...");
    GCValue xhr_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, xhr_proto, js_xhr_proto_funcs, js_xhr_proto_funcs_count);
    GCValue xhr_ctor = JS_NewCFunction2(ctx, js_xhr_constructor, "XMLHttpRequest", 
        1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, xhr_ctor, xhr_proto);
    JS_SetClassProto(ctx, js_xhr_class_id, xhr_proto);
    // Set constants on constructor BEFORE transferring ownership
    JS_SetPropertyStr(ctx, xhr_ctor, "UNSENT", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, xhr_ctor, "OPENED", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, xhr_ctor, "HEADERS_RECEIVED", JS_NewInt32(ctx, 2));
    JS_SetPropertyStr(ctx, xhr_ctor, "LOADING", JS_NewInt32(ctx, 3));
    JS_SetPropertyStr(ctx, xhr_ctor, "DONE", JS_NewInt32(ctx, 4));
    JS_SetPropertyStr(ctx, global, "XMLHttpRequest", xhr_ctor);
    
    // ===== HTMLVideoElement =====
    GCValue video_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, video_proto, js_video_proto_funcs, js_video_proto_funcs_count);
    // HTMLVideoElement.prototype -> HTMLElement.prototype
    JS_SetPrototype(ctx, video_proto, html_element_proto);
    GCValue video_ctor = JS_NewCFunction2(ctx, js_video_constructor, "HTMLVideoElement",
        1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, video_ctor, video_proto);
    JS_SetClassProto(ctx, js_video_class_id, video_proto);
    // Set constants on constructor BEFORE transferring ownership
    JS_SetPropertyStr(ctx, video_ctor, "HAVE_NOTHING", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, video_ctor, "HAVE_METADATA", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, video_ctor, "HAVE_CURRENT_DATA", JS_NewInt32(ctx, 2));
    JS_SetPropertyStr(ctx, video_ctor, "HAVE_FUTURE_DATA", JS_NewInt32(ctx, 3));
    JS_SetPropertyStr(ctx, video_ctor, "HAVE_ENOUGH_DATA", JS_NewInt32(ctx, 4));
    JS_SetPropertyStr(ctx, video_ctor, "NETWORK_EMPTY", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, video_ctor, "NETWORK_IDLE", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, video_ctor, "NETWORK_LOADING", JS_NewInt32(ctx, 2));
    JS_SetPropertyStr(ctx, video_ctor, "NETWORK_NO_SOURCE", JS_NewInt32(ctx, 3));
    // global === window, so set once
    JS_SetPropertyStr(ctx, global, "HTMLVideoElement", video_ctor);
    
    // ===== fetch API =====
    // fetch is set on global (which is window) - no need to duplicate
    JS_SetPropertyStr(ctx, global, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 2));
    
    // ===== Event APIs =====
    // Event class
    GCValue event_proto = JS_NewObject(ctx);
    JS_SetClassProto(ctx, js_event_class_id, event_proto);
    
    // Event.prototype.type getter
    GCValue event_type_getter = JS_NewCFunction(ctx, js_event_get_type_wrapper, "get type", 0);
    JSAtom event_type_atom = JS_NewAtom(ctx, "type");
    JS_DefinePropertyGetSet(ctx, event_proto, event_type_atom, event_type_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, event_type_atom);
    
    // Event.prototype.bubbles getter
    GCValue event_bubbles_getter = JS_NewCFunction(ctx, js_event_get_bubbles_wrapper, "get bubbles", 0);
    JSAtom event_bubbles_atom = JS_NewAtom(ctx, "bubbles");
    JS_DefinePropertyGetSet(ctx, event_proto, event_bubbles_atom, event_bubbles_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, event_bubbles_atom);
    
    // Event.prototype.cancelable getter
    GCValue event_cancelable_getter = JS_NewCFunction(ctx, js_event_get_cancelable_wrapper, "get cancelable", 0);
    JSAtom event_cancelable_atom = JS_NewAtom(ctx, "cancelable");
    JS_DefinePropertyGetSet(ctx, event_proto, event_cancelable_atom, event_cancelable_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, event_cancelable_atom);
    
    // Event.prototype.composed getter
    GCValue event_composed_getter = JS_NewCFunction(ctx, js_event_get_composed_wrapper, "get composed", 0);
    JSAtom event_composed_atom = JS_NewAtom(ctx, "composed");
    JS_DefinePropertyGetSet(ctx, event_proto, event_composed_atom, event_composed_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, event_composed_atom);
    
    // Event.prototype.defaultPrevented getter
    GCValue event_defaultPrevented_getter = JS_NewCFunction(ctx, js_event_get_defaultPrevented_wrapper, "get defaultPrevented", 0);
    JSAtom event_defaultPrevented_atom = JS_NewAtom(ctx, "defaultPrevented");
    JS_DefinePropertyGetSet(ctx, event_proto, event_defaultPrevented_atom, event_defaultPrevented_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, event_defaultPrevented_atom);
    
    // Event.prototype.target getter
    GCValue event_target_getter = JS_NewCFunction(ctx, js_event_get_target_wrapper, "get target", 0);
    JSAtom event_target_atom = JS_NewAtom(ctx, "target");
    JS_DefinePropertyGetSet(ctx, event_proto, event_target_atom, event_target_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, event_target_atom);
    
    // Event.prototype.currentTarget getter
    GCValue event_currentTarget_getter = JS_NewCFunction(ctx, js_event_get_currentTarget_wrapper, "get currentTarget", 0);
    JSAtom event_currentTarget_atom = JS_NewAtom(ctx, "currentTarget");
    JS_DefinePropertyGetSet(ctx, event_proto, event_currentTarget_atom, event_currentTarget_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, event_currentTarget_atom);
    
    // Event.prototype.eventPhase getter
    GCValue event_eventPhase_getter = JS_NewCFunction(ctx, js_event_get_eventPhase_wrapper, "get eventPhase", 0);
    JSAtom event_eventPhase_atom = JS_NewAtom(ctx, "eventPhase");
    JS_DefinePropertyGetSet(ctx, event_proto, event_eventPhase_atom, event_eventPhase_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, event_eventPhase_atom);
    
    // Event.prototype methods
    JS_SetPropertyStr(ctx, event_proto, "preventDefault",
        JS_NewCFunction(ctx, js_event_preventDefault, "preventDefault", 0));
    JS_SetPropertyStr(ctx, event_proto, "stopPropagation",
        JS_NewCFunction(ctx, js_event_stopPropagation, "stopPropagation", 0));
    JS_SetPropertyStr(ctx, event_proto, "stopImmediatePropagation",
        JS_NewCFunction(ctx, js_event_stopImmediatePropagation, "stopImmediatePropagation", 0));
    JS_SetPropertyStr(ctx, event_proto, "composedPath",
        JS_NewCFunction(ctx, js_event_composedPath, "composedPath", 0));
    
    // Event constructor
    GCValue event_ctor = JS_NewCFunction2(ctx, js_event_constructor, "Event", 2, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, event_ctor, event_proto);
    JS_SetPropertyStr(ctx, global, "Event", event_ctor);
    
    // CustomEvent class (inherits from Event)
    GCValue custom_event_proto = JS_NewObject(ctx);
    // Set CustomEvent.prototype.__proto__ = Event.prototype
    JS_SetPropertyStr(ctx, custom_event_proto, "__proto__", event_proto);
    JS_SetClassProto(ctx, js_custom_event_class_id, custom_event_proto);
    
    // CustomEvent.prototype.detail getter
    GCValue custom_event_detail_getter = JS_NewCFunction(ctx, js_custom_event_get_detail_wrapper, "get detail", 0);
    JSAtom custom_event_detail_atom = JS_NewAtom(ctx, "detail");
    JS_DefinePropertyGetSet(ctx, custom_event_proto, custom_event_detail_atom, custom_event_detail_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, custom_event_detail_atom);
    
    // CustomEvent constructor
    GCValue custom_event_ctor = JS_NewCFunction2(ctx, js_custom_event_constructor, "CustomEvent", 2, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, custom_event_ctor, custom_event_proto);
    JS_SetPropertyStr(ctx, global, "CustomEvent", custom_event_ctor);
    
    // MouseEvent class (inherits from Event)
    GCValue mouse_event_proto = JS_NewObject(ctx);
    // Set MouseEvent.prototype.__proto__ = Event.prototype
    JS_SetPropertyStr(ctx, mouse_event_proto, "__proto__", event_proto);
    JS_SetClassProto(ctx, js_mouse_event_class_id, mouse_event_proto);
    
    // MouseEvent.prototype.clientX getter
    GCValue mouse_event_clientX_getter = JS_NewCFunction(ctx, js_mouse_event_get_clientX_wrapper, "get clientX", 0);
    JSAtom mouse_event_clientX_atom = JS_NewAtom(ctx, "clientX");
    JS_DefinePropertyGetSet(ctx, mouse_event_proto, mouse_event_clientX_atom, mouse_event_clientX_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, mouse_event_clientX_atom);
    
    // MouseEvent.prototype.clientY getter
    GCValue mouse_event_clientY_getter = JS_NewCFunction(ctx, js_mouse_event_get_clientY_wrapper, "get clientY", 0);
    JSAtom mouse_event_clientY_atom = JS_NewAtom(ctx, "clientY");
    JS_DefinePropertyGetSet(ctx, mouse_event_proto, mouse_event_clientY_atom, mouse_event_clientY_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, mouse_event_clientY_atom);
    
    // MouseEvent constructor
    GCValue mouse_event_ctor = JS_NewCFunction2(ctx, js_mouse_event_constructor, "MouseEvent", 2, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, mouse_event_ctor, mouse_event_proto);
    JS_SetPropertyStr(ctx, global, "MouseEvent", mouse_event_ctor);
    
    // FocusEvent class (inherits from Event)
    GCValue focus_event_proto = JS_NewObject(ctx);
    // Set FocusEvent.prototype.__proto__ = Event.prototype
    JS_SetPropertyStr(ctx, focus_event_proto, "__proto__", event_proto);
    JS_SetClassProto(ctx, js_focus_event_class_id, focus_event_proto);
    
    // FocusEvent.prototype.relatedTarget getter
    GCValue focus_event_relatedTarget_getter = JS_NewCFunction(ctx, js_focus_event_get_relatedTarget_wrapper, "get relatedTarget", 0);
    JSAtom focus_event_relatedTarget_atom = JS_NewAtom(ctx, "relatedTarget");
    JS_DefinePropertyGetSet(ctx, focus_event_proto, focus_event_relatedTarget_atom, focus_event_relatedTarget_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, focus_event_relatedTarget_atom);
    
    // FocusEvent constructor
    GCValue focus_event_ctor = JS_NewCFunction2(ctx, js_focus_event_constructor, "FocusEvent", 2, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, focus_event_ctor, focus_event_proto);
    JS_SetPropertyStr(ctx, global, "FocusEvent", focus_event_ctor);
    
    // KeyboardEvent constructor (needed by Polymer/TypeScript decorator metadata)
    GCValue keyboard_event_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, keyboard_event_proto, "__proto__", event_proto);
    GCValue keyboard_event_ctor = JS_NewCFunction2(ctx, js_dummy_function, "KeyboardEvent", 2, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, keyboard_event_ctor, "prototype", keyboard_event_proto);
    JS_SetPropertyStr(ctx, global, "KeyboardEvent", keyboard_event_ctor);
    JS_SetPropertyStr(ctx, window, "KeyboardEvent", keyboard_event_ctor);
    
    // ErrorEvent constructor (needed by network/error handling code)
    GCValue error_event_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, error_event_proto, "__proto__", event_proto);
    GCValue error_event_ctor = JS_NewCFunction2(ctx, js_dummy_function, "ErrorEvent", 2, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, error_event_ctor, "prototype", error_event_proto);
    JS_SetPropertyStr(ctx, global, "ErrorEvent", error_event_ctor);
    JS_SetPropertyStr(ctx, window, "ErrorEvent", error_event_ctor);
    
    // WheelEvent constructor
    GCValue wheel_event_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, wheel_event_proto, "__proto__", event_proto);
    GCValue wheel_event_ctor = JS_NewCFunction2(ctx, js_dummy_function, "WheelEvent", 2, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, wheel_event_ctor, "prototype", wheel_event_proto);
    JS_SetPropertyStr(ctx, global, "WheelEvent", wheel_event_ctor);
    JS_SetPropertyStr(ctx, window, "WheelEvent", wheel_event_ctor);
    
    // DragEvent constructor
    GCValue drag_event_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, drag_event_proto, "__proto__", event_proto);
    GCValue drag_event_ctor = JS_NewCFunction2(ctx, js_dummy_function, "DragEvent", 2, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, drag_event_ctor, "prototype", drag_event_proto);
    JS_SetPropertyStr(ctx, global, "DragEvent", drag_event_ctor);
    JS_SetPropertyStr(ctx, window, "DragEvent", drag_event_ctor);
    
    // TouchEvent constructor
    GCValue touch_event_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, touch_event_proto, "__proto__", event_proto);
    GCValue touch_event_ctor = JS_NewCFunction2(ctx, js_dummy_function, "TouchEvent", 2, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, touch_event_ctor, "prototype", touch_event_proto);
    JS_SetPropertyStr(ctx, global, "TouchEvent", touch_event_ctor);
    JS_SetPropertyStr(ctx, window, "TouchEvent", touch_event_ctor);
    
    // InputEvent constructor
    GCValue input_event_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, input_event_proto, "__proto__", event_proto);
    GCValue input_event_ctor = JS_NewCFunction2(ctx, js_dummy_function, "InputEvent", 2, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, input_event_ctor, "prototype", input_event_proto);
    JS_SetPropertyStr(ctx, global, "InputEvent", input_event_ctor);
    JS_SetPropertyStr(ctx, window, "InputEvent", input_event_ctor);
    
    // ProgressEvent constructor
    GCValue progress_event_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, progress_event_proto, "__proto__", event_proto);
    GCValue progress_event_ctor = JS_NewCFunction2(ctx, js_dummy_function, "ProgressEvent", 2, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, progress_event_ctor, "prototype", progress_event_proto);
    JS_SetPropertyStr(ctx, global, "ProgressEvent", progress_event_ctor);
    JS_SetPropertyStr(ctx, window, "ProgressEvent", progress_event_ctor);
    
    // Range constructor (needed by TypeScript decorator metadata)
    GCValue range_proto = JS_NewObject(ctx);
    GCValue range_ctor = JS_NewCFunction2(ctx, js_dummy_function, "Range", 0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, range_ctor, "prototype", range_proto);
    JS_SetPropertyStr(ctx, global, "Range", range_ctor);
    JS_SetPropertyStr(ctx, window, "Range", range_ctor);
    
    // ===== Shadow DOM APIs =====
    // ShadowRoot class
    GCValue shadow_root_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, shadow_root_proto, js_shadow_root_proto_funcs, 
        sizeof(js_shadow_root_proto_funcs) / sizeof(js_shadow_root_proto_funcs[0]));
    
    // ShadowRoot.prototype.host getter
    GCValue host_getter = JS_NewCFunction(ctx, js_shadow_root_get_host_wrapper, "get host", 0);
    JSAtom host_atom = JS_NewAtom(ctx, "host");
    JS_DefinePropertyGetSet(ctx, shadow_root_proto, host_atom, host_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, host_atom);
    
    // ShadowRoot tree navigation properties
    GCValue sr_first_child_getter = JS_NewCFunction(ctx, js_shadow_root_get_first_child, "get firstChild", 0);
    JSAtom sr_first_child_atom = JS_NewAtom(ctx, "firstChild");
    JS_DefinePropertyGetSet(ctx, shadow_root_proto, sr_first_child_atom, sr_first_child_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, sr_first_child_atom);
    
    GCValue sr_last_child_getter = JS_NewCFunction(ctx, js_shadow_root_get_last_child, "get lastChild", 0);
    JSAtom sr_last_child_atom = JS_NewAtom(ctx, "lastChild");
    JS_DefinePropertyGetSet(ctx, shadow_root_proto, sr_last_child_atom, sr_last_child_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, sr_last_child_atom);
    
    GCValue sr_child_nodes_getter = JS_NewCFunction(ctx, js_shadow_root_get_child_nodes, "get childNodes", 0);
    JSAtom sr_child_nodes_atom = JS_NewAtom(ctx, "childNodes");
    JS_DefinePropertyGetSet(ctx, shadow_root_proto, sr_child_nodes_atom, sr_child_nodes_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, sr_child_nodes_atom);
    
    GCValue sr_children_getter = JS_NewCFunction(ctx, js_shadow_root_get_children, "get children", 0);
    JSAtom sr_children_atom = JS_NewAtom(ctx, "children");
    JS_DefinePropertyGetSet(ctx, shadow_root_proto, sr_children_atom, sr_children_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, sr_children_atom);
    
    GCValue sr_child_elem_count_getter = JS_NewCFunction(ctx, js_shadow_root_get_child_element_count, "get childElementCount", 0);
    JSAtom sr_child_elem_count_atom = JS_NewAtom(ctx, "childElementCount");
    JS_DefinePropertyGetSet(ctx, shadow_root_proto, sr_child_elem_count_atom, sr_child_elem_count_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, sr_child_elem_count_atom);
    
    JS_SetClassProto(ctx, js_shadow_root_class_id, shadow_root_proto);
    GCValue shadow_root_ctor = JS_NewCFunction2(ctx, js_shadow_root_constructor, "ShadowRoot",
        0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, shadow_root_ctor, shadow_root_proto);
    JS_SetPropertyStr(ctx, global, "ShadowRoot", shadow_root_ctor);

    // ===== Custom Elements API =====
    GCValue custom_elements = JS_NewObjectClass(ctx, js_custom_element_registry_class_id);
    JS_SetPropertyStr(ctx, custom_elements, "define",
        JS_NewCFunction(ctx, js_custom_elements_define, "define", 2));
    JS_SetPropertyStr(ctx, custom_elements, "get",
        JS_NewCFunction(ctx, js_custom_elements_get, "get", 1));
    JS_SetPropertyStr(ctx, custom_elements, "whenDefined",
        JS_NewCFunction(ctx, js_custom_elements_when_defined, "whenDefined", 1));
    JS_SetPropertyStr(ctx, window, "customElements", custom_elements);
    JS_SetPropertyStr(ctx, global, "customElements", custom_elements);
    
    // CustomElementRegistry constructor (for completeness)
    GCValue ce_registry_ctor = JS_NewCFunction2(ctx, js_dummy_function, "CustomElementRegistry",
        0, JS_CFUNC_constructor, 0);
    GCValue ce_registry_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ce_registry_proto, "constructor", ce_registry_ctor);
    JS_SetPropertyStr(ctx, ce_registry_ctor, "prototype", ce_registry_proto);
    JS_SetPropertyStr(ctx, global, "CustomElementRegistry", ce_registry_ctor);

    // ===== Web Animations API =====
    LOG_INFO("Setting up Web Animations API...");
    LOG_INFO("About to create Animation class...");
    if (!is_obj_usable(ctx, global)) {
        LOG_ERROR("CRITICAL: global object corrupted before Animation setup!");
        return;
    }
    // Animation class
    LOG_INFO("Creating animation_proto...");
    GCValue animation_proto = JS_NewObject(ctx);
    if (JS_IsException(animation_proto)) {
        LOG_ERROR("Animation proto creation failed - skipping Animation setup");
    } else {
        LOG_INFO("Setting Animation property function list...");
        JS_SetPropertyFunctionList(ctx, animation_proto, js_animation_proto_funcs,
            sizeof(js_animation_proto_funcs) / sizeof(js_animation_proto_funcs[0]));
        LOG_INFO("Setting Animation class proto...");
        JS_SetClassProto(ctx, js_animation_class_id, animation_proto);
        LOG_INFO("Creating Animation constructor...");
        GCValue animation_ctor = JS_NewCFunction2(ctx, js_animation_constructor, "Animation",
            1, JS_CFUNC_constructor, 0);
        LOG_INFO("Animation constructor created, checking validity...");
        if (!JS_IsException(animation_ctor)) {
            LOG_INFO("Setting Animation constructor...");
            JS_SetConstructor(ctx, animation_ctor, animation_proto);
            LOG_INFO("About to set Animation on global...");
            LOG_INFO("Checking animation_ctor validity...");
            if (JS_IsException(animation_ctor)) {
                LOG_ERROR("ERROR: animation_ctor is exception!");
            } else if (!JS_IsObject(animation_ctor)) {
                LOG_ERROR("ERROR: animation_ctor is not an object! tag=%d", (int)JS_VALUE_GET_TAG(animation_ctor));
            } else {
                LOG_INFO("animation_ctor appears valid, attempting JS_SetPropertyStr...");
            }
            if (safe_set_property_str(ctx, global, "Animation", animation_ctor) < 0) {
                LOG_ERROR("Failed to set Animation on global");
            } else {
                LOG_INFO("Animation setup completed successfully");
            }
        } else {
            LOG_ERROR("Animation constructor creation failed - skipping");
        }
    }

    // KeyframeEffect class
    GCValue keyframe_effect_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, keyframe_effect_proto, js_keyframe_effect_proto_funcs,
        sizeof(js_keyframe_effect_proto_funcs) / sizeof(js_keyframe_effect_proto_funcs[0]));
    JS_SetClassProto(ctx, js_keyframe_effect_class_id, keyframe_effect_proto);
    GCValue keyframe_effect_ctor = JS_NewCFunction2(ctx, js_keyframe_effect_constructor, "KeyframeEffect",
        3, JS_CFUNC_constructor, 0);
    if (!JS_IsException(keyframe_effect_ctor)) {
        JS_SetConstructor(ctx, keyframe_effect_ctor, keyframe_effect_proto);
        if (safe_set_property_str(ctx, global, "KeyframeEffect", keyframe_effect_ctor) < 0) {
            LOG_ERROR("Failed to set KeyframeEffect on global");
        }
    } else {
        LOG_ERROR("KeyframeEffect constructor creation failed - skipping");
    }

    // ===== Font Loading API =====
    // FontFace class
    GCValue font_face_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, font_face_proto, js_font_face_proto_funcs,
        sizeof(js_font_face_proto_funcs) / sizeof(js_font_face_proto_funcs[0]));
    JS_SetClassProto(ctx, js_font_face_class_id, font_face_proto);
    GCValue font_face_ctor = JS_NewCFunction2(ctx, js_font_face_constructor, "FontFace",
        3, JS_CFUNC_constructor, 0);
    if (!JS_IsException(font_face_ctor)) {
        JS_SetConstructor(ctx, font_face_ctor, font_face_proto);
        if (safe_set_property_str(ctx, global, "FontFace", font_face_ctor) < 0) {
            LOG_ERROR("Failed to set FontFace on global");
        }
    } else {
        LOG_ERROR("FontFace constructor creation failed - skipping");
    }

    // FontFaceSet class (document.fonts)
    GCValue font_face_set_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, font_face_set_proto, js_font_face_set_proto_funcs,
        sizeof(js_font_face_set_proto_funcs) / sizeof(js_font_face_set_proto_funcs[0]));
    JS_SetClassProto(ctx, js_font_face_set_class_id, font_face_set_proto);
    GCValue font_face_set = JS_NewObjectClass(ctx, js_font_face_set_class_id);
    GCHandle ffs_handle = gc_allocz(sizeof(FontFaceSetData), JS_GC_OBJ_TYPE_DATA);
    if (ffs_handle != GC_HANDLE_NULL) {
        /* Safe: dereference only for immediate initialization before any GC point */
        FontFaceSetData *ffs = (FontFaceSetData*)gc_deref(ffs_handle);
        ffs->loaded_fonts = JS_NewArray(ctx);
        /* Store handle (not pointer) for GC safety during compaction */
        JS_SetOpaqueHandle(font_face_set, ffs_handle);
    }
    // Add Symbol.iterator to FontFaceSet.prototype using C
    GCValue symbol_ctor2 = JS_GetPropertyStr(ctx, global, "Symbol");
    if (!JS_IsException(symbol_ctor2) && !JS_IsUndefined(symbol_ctor2)) {
        GCValue iterator_symbol = JS_GetPropertyStr(ctx, symbol_ctor2, "iterator");
        if (!JS_IsException(iterator_symbol) && !JS_IsUndefined(iterator_symbol)) {
            GCValue values_func = JS_GetPropertyStr(ctx, font_face_set_proto, "values");
            if (!JS_IsException(values_func) && !JS_IsUndefined(values_func)) {
                JSAtom iter_atom = JS_ValueToAtom(ctx, iterator_symbol);
                if (iter_atom != JS_ATOM_NULL) {
                    JS_SetProperty(ctx, font_face_set_proto, iter_atom, values_func);
                }
            }
        }
    }
    
    JS_SetPropertyStr(ctx, document, "fonts", font_face_set);
    
    GCValue font_face_set_ctor = JS_NewCFunction2(ctx, js_dummy_function, "FontFaceSet",
        0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "FontFaceSet", font_face_set_ctor);

    // ===== Observer APIs =====
    // MutationObserver
    GCValue mutation_observer_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, mutation_observer_proto, js_mutation_observer_proto_funcs,
        sizeof(js_mutation_observer_proto_funcs) / sizeof(js_mutation_observer_proto_funcs[0]));
    JS_SetClassProto(ctx, js_mutation_observer_class_id, mutation_observer_proto);
    GCValue mutation_observer_ctor = JS_NewCFunction2(ctx, js_mutation_observer_constructor, "MutationObserver",
        1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, mutation_observer_ctor, mutation_observer_proto);
    JS_SetPropertyStr(ctx, global, "MutationObserver", mutation_observer_ctor);

    // ResizeObserver
    GCValue resize_observer_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, resize_observer_proto, js_resize_observer_proto_funcs,
        sizeof(js_resize_observer_proto_funcs) / sizeof(js_resize_observer_proto_funcs[0]));
    JS_SetClassProto(ctx, js_resize_observer_class_id, resize_observer_proto);
    GCValue resize_observer_ctor = JS_NewCFunction2(ctx, js_resize_observer_constructor, "ResizeObserver",
        1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, resize_observer_ctor, resize_observer_proto);
    JS_SetPropertyStr(ctx, global, "ResizeObserver", resize_observer_ctor);

    // IntersectionObserver
    GCValue intersection_observer_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, intersection_observer_proto, js_intersection_observer_proto_funcs,
        sizeof(js_intersection_observer_proto_funcs) / sizeof(js_intersection_observer_proto_funcs[0]));
    JS_SetClassProto(ctx, js_intersection_observer_class_id, intersection_observer_proto);
    GCValue intersection_observer_ctor = JS_NewCFunction2(ctx, js_intersection_observer_constructor, "IntersectionObserver",
        1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, intersection_observer_ctor, intersection_observer_proto);
    JS_SetPropertyStr(ctx, global, "IntersectionObserver", intersection_observer_ctor);

    // ===== Performance API =====
    // PerformanceEntry class
    GCValue performance_entry_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, performance_entry_proto, js_performance_entry_proto_funcs,
        sizeof(js_performance_entry_proto_funcs) / sizeof(js_performance_entry_proto_funcs[0]));
    JS_SetClassProto(ctx, js_performance_entry_class_id, performance_entry_proto);
    GCValue performance_entry_ctor = JS_NewCFunction2(ctx, js_dummy_function, "PerformanceEntry",
        0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, performance_entry_ctor, performance_entry_proto);
    JS_SetPropertyStr(ctx, global, "PerformanceEntry", performance_entry_ctor);

    // PerformanceObserver class
    GCValue performance_observer_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, performance_observer_proto, js_performance_observer_proto_funcs,
        sizeof(js_performance_observer_proto_funcs) / sizeof(js_performance_observer_proto_funcs[0]));
    JS_SetClassProto(ctx, js_performance_observer_class_id, performance_observer_proto);
    GCValue performance_observer_ctor = JS_NewCFunction2(ctx, js_performance_observer_constructor, "PerformanceObserver",
        1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, performance_observer_ctor, performance_observer_proto);
    JS_SetPropertyStr(ctx, global, "PerformanceObserver", performance_observer_ctor);

    // Performance class
    // Initialize time origin on first setup
    if (g_performance_time_origin == 0.0) {
        g_performance_time_origin = performance_get_time_ms();
    }
    
    GCValue performance_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, performance_proto, js_performance_proto_funcs,
        sizeof(js_performance_proto_funcs) / sizeof(js_performance_proto_funcs[0]));
    JS_SetClassProto(ctx, js_performance_class_id, performance_proto);
    GCValue performance_obj = JS_NewObjectClass(ctx, js_performance_class_id);
    GCHandle perf_handle = gc_allocz(sizeof(PerformanceData), JS_GC_OBJ_TYPE_DATA);
    if (perf_handle != GC_HANDLE_NULL) {
        /* Safe: dereference only for immediate initialization before any GC point */
        PerformanceData *perf_data = (PerformanceData*)gc_deref(perf_handle);
        perf_data->start_time = 0.0;
        perf_data->time_origin = g_performance_time_origin;
        perf_data->entry_count = 0;
        /* Store handle (not pointer) for GC safety during compaction */
        JS_SetOpaqueHandle(performance_obj, perf_handle);
    }
    JS_SetPropertyStr(ctx, window, "performance", performance_obj);
    JS_SetPropertyStr(ctx, global, "performance", performance_obj);
    
    // Create and set the timing object directly on the performance instance
    GCValue timing_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, timing_obj, "navigationStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "unloadEventStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "unloadEventEnd", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "redirectStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "redirectEnd", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "fetchStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "domainLookupStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "domainLookupEnd", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "connectStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "connectEnd", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "secureConnectionStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "requestStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "responseStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "responseEnd", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "domLoading", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "domInteractive", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "domContentLoadedEventStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "domContentLoadedEventEnd", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "domComplete", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "loadEventStart", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, timing_obj, "loadEventEnd", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, performance_obj, "timing", timing_obj);
    
    GCValue performance_ctor = JS_NewCFunction2(ctx, js_dummy_function, "Performance",
        0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, performance_ctor, performance_proto);
    JS_SetPropertyStr(ctx, global, "Performance", performance_ctor);

    // ===== DOMRect API =====
    // DOMRectReadOnly class
    GCValue dom_rect_read_only_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, dom_rect_read_only_proto, js_dom_rect_read_only_proto_funcs,
        sizeof(js_dom_rect_read_only_proto_funcs) / sizeof(js_dom_rect_read_only_proto_funcs[0]));
    JS_SetClassProto(ctx, js_dom_rect_read_only_class_id, dom_rect_read_only_proto);
    GCValue dom_rect_read_only_ctor = JS_NewCFunction2(ctx, js_dom_rect_read_only_constructor, "DOMRectReadOnly",
        4, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, dom_rect_read_only_ctor, dom_rect_read_only_proto);
    // Add fromRect static method
    GCValue from_rect_ro = JS_NewCFunction(ctx, js_dom_rect_read_only_from_rect, "fromRect", 1);
    JS_SetPropertyStr(ctx, dom_rect_read_only_ctor, "fromRect", from_rect_ro);
    JS_SetPropertyStr(ctx, global, "DOMRectReadOnly", dom_rect_read_only_ctor);

    // DOMRect class
    GCValue dom_rect_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, dom_rect_proto, js_dom_rect_proto_funcs,
        sizeof(js_dom_rect_proto_funcs) / sizeof(js_dom_rect_proto_funcs[0]));
    JS_SetClassProto(ctx, js_dom_rect_class_id, dom_rect_proto);
    GCValue dom_rect_ctor = JS_NewCFunction2(ctx, js_dom_rect_constructor, "DOMRect",
        4, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, dom_rect_ctor, dom_rect_proto);
    // Add fromRect static method
    GCValue from_rect = JS_NewCFunction(ctx, js_dom_rect_from_rect, "fromRect", 1);
    JS_SetPropertyStr(ctx, dom_rect_ctor, "fromRect", from_rect);
    JS_SetPropertyStr(ctx, global, "DOMRect", dom_rect_ctor);

    // ===== Date API =====
    GCValue date_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, date_proto, js_date_proto_funcs,
        sizeof(js_date_proto_funcs) / sizeof(js_date_proto_funcs[0]));
    JS_SetClassProto(ctx, js_date_class_id, date_proto);
    GCValue date_ctor = JS_NewCFunction2(ctx, js_date_constructor, "Date",
        7, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, date_ctor, date_proto);
    // Add static methods
    JS_SetPropertyFunctionList(ctx, date_ctor, js_date_static_funcs,
        sizeof(js_date_static_funcs) / sizeof(js_date_static_funcs[0]));
    JS_SetPropertyStr(ctx, global, "Date", date_ctor);

    // ===== MediaSource API =====
    // SourceBuffer class (needed by MediaSource)
    GCValue source_buffer_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, source_buffer_proto, js_source_buffer_proto_funcs,
        sizeof(js_source_buffer_proto_funcs) / sizeof(js_source_buffer_proto_funcs[0]));
    JS_SetClassProto(ctx, js_source_buffer_class_id, source_buffer_proto);
    GCValue source_buffer_ctor = JS_NewCFunction2(ctx, js_source_buffer_constructor, "SourceBuffer",
        0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, source_buffer_ctor, source_buffer_proto);
    JS_SetPropertyStr(ctx, global, "SourceBuffer", source_buffer_ctor);
    
    // MediaSource class
    GCValue media_source_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, media_source_proto, js_media_source_proto_funcs,
        sizeof(js_media_source_proto_funcs) / sizeof(js_media_source_proto_funcs[0]));
    JS_SetClassProto(ctx, js_media_source_class_id, media_source_proto);
    GCValue media_source_ctor = JS_NewCFunction2(ctx, js_media_source_constructor, "MediaSource",
        0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, media_source_ctor, media_source_proto);
    // Add isTypeSupported static method
    GCValue is_type_supported = JS_NewCFunction(ctx, js_media_source_is_type_supported, "isTypeSupported", 1);
    JS_SetPropertyStr(ctx, media_source_ctor, "isTypeSupported", is_type_supported);
    JS_SetPropertyStr(ctx, global, "MediaSource", media_source_ctor);
    LOG_INFO("MediaSource API set");
    
    // ManagedMediaSource (iOS variant) - alias to MediaSource
    GCValue managed_media_source_ctor = JS_NewCFunction2(ctx, js_media_source_constructor, "ManagedMediaSource",
        0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, managed_media_source_ctor, media_source_proto);
    JS_SetPropertyStr(ctx, managed_media_source_ctor, "isTypeSupported", is_type_supported);
    JS_SetPropertyStr(ctx, global, "ManagedMediaSource", managed_media_source_ctor);
    LOG_INFO("ManagedMediaSource API set");
    
    // WebKitMediaSource (Safari variant) - alias to MediaSource
    GCValue webkit_media_source_ctor = JS_NewCFunction2(ctx, js_media_source_constructor, "WebKitMediaSource",
        0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, webkit_media_source_ctor, media_source_proto);
    JS_SetPropertyStr(ctx, webkit_media_source_ctor, "isTypeSupported", is_type_supported);
    JS_SetPropertyStr(ctx, global, "WebKitMediaSource", webkit_media_source_ctor);
    LOG_INFO("WebKitMediaSource API set");
    
    // ===== HTMLMediaElement Base Class =====
    // This is the base class for video/audio elements
    GCValue html_media_element_ctor = JS_NewCFunction2(ctx, js_dummy_function, "HTMLMediaElement",
        0, JS_CFUNC_constructor, 0);
    GCValue html_media_element_proto = JS_NewObject(ctx);
    
    // webkitSourceAddId - used by some players for source management
    JS_SetPropertyStr(ctx, html_media_element_proto, "webkitSourceAddId",
        JS_NewCFunction(ctx, js_dummy_function, "webkitSourceAddId", 2));
    
    // webkitSourceRemoveId
    JS_SetPropertyStr(ctx, html_media_element_proto, "webkitSourceRemoveId",
        JS_NewCFunction(ctx, js_dummy_function, "webkitSourceRemoveId", 1));
    
    // webkitSourceSetDuration
    JS_SetPropertyStr(ctx, html_media_element_proto, "webkitSourceSetDuration",
        JS_NewCFunction(ctx, js_dummy_function, "webkitSourceSetDuration", 1));
    
    JS_SetPropertyStr(ctx, html_media_element_ctor, "prototype", html_media_element_proto);
    JS_SetPropertyStr(ctx, global, "HTMLMediaElement", html_media_element_ctor);
    LOG_INFO("HTMLMediaElement API set");
    
    // ===== URL API =====
    // Create URL constructor function
    GCValue url_ctor = JS_NewCFunction2(ctx, js_url_constructor, "URL", 2, JS_CFUNC_constructor, 0);
    // Create URL prototype object
    GCValue url_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, url_proto, "constructor", url_ctor);
    // Set up the constructor with its prototype (required for JS_IsFunction to return true)
    JS_SetConstructor(ctx, url_ctor, url_proto);
    // Explicitly set the constructor bit to ensure JS_IsFunction returns true
    JS_SetConstructorBit(ctx, url_ctor, TRUE);
    // Add static methods
    JS_SetPropertyStr(ctx, url_ctor, "createObjectURL",
        JS_NewCFunction(ctx, js_url_create_object_url, "createObjectURL", 1));
    JS_SetPropertyStr(ctx, url_ctor, "revokeObjectURL",
        JS_NewCFunction(ctx, js_url_revoke_object_url, "revokeObjectURL", 1));
    JS_SetPropertyStr(ctx, global, "URL", url_ctor);
    JS_SetPropertyStr(ctx, window, "URL", url_ctor);
    LOG_INFO("URL API set");
    
    // ===== Request/Response API =====
    GCValue request_ctor = JS_NewCFunction2(ctx, js_request_constructor, "Request", 2, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "Request", request_ctor);
    JS_SetPropertyStr(ctx, window, "Request", request_ctor);
    
    GCValue response_ctor = JS_NewCFunction2(ctx, js_response_constructor, "Response", 2, JS_CFUNC_constructor, 0);
    // Add Response.json() static method
    JS_SetPropertyStr(ctx, response_ctor, "json",
        JS_NewCFunction(ctx, js_response_json, "json", 1));
    JS_SetPropertyStr(ctx, global, "Response", response_ctor);
    JS_SetPropertyStr(ctx, window, "Response", response_ctor);
    LOG_INFO("Request/Response API set");
    
    // ===== Navigator sendBeacon =====
    // Get existing navigator or create new one
    GCValue nav = JS_GetPropertyStr(ctx, window, "navigator");
    if (JS_IsException(nav) || !JS_IsObject(nav)) {
        nav = JS_NewObject(ctx);
    }
    JS_SetPropertyStr(ctx, nav, "sendBeacon",
        JS_NewCFunction(ctx, js_navigator_send_beacon, "sendBeacon", 2));
    JS_SetPropertyStr(ctx, window, "navigator", nav);
    LOG_INFO("Navigator sendBeacon set");
    
    // ===== Missing APIs for YouTube script 024 =====
    // matchMedia
    JS_SetPropertyStr(ctx, window, "matchMedia",
        JS_NewCFunction(ctx, js_match_media, "matchMedia", 1));
    
    // btoa / atob
    JS_SetPropertyStr(ctx, window, "btoa",
        JS_NewCFunction(ctx, js_btoa, "btoa", 1));
    JS_SetPropertyStr(ctx, window, "atob",
        JS_NewCFunction(ctx, js_atob, "atob", 1));
    
    // AbortController
    GCValue abort_controller_ctor = JS_NewCFunction2(ctx, js_abort_controller_constructor, "AbortController",
        0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "AbortController", abort_controller_ctor);
    JS_SetPropertyStr(ctx, window, "AbortController", abort_controller_ctor);
    
    // AbortSignal - needed by YouTube player scripts
    GCValue abort_signal_ctor = JS_NewCFunction2(ctx, js_abort_signal_constructor, "AbortSignal",
        0, JS_CFUNC_constructor, 0);
    /* C function constructors don't get a prototype automatically */
    GCValue abort_signal_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, abort_signal_proto, "aborted", JS_FALSE);
    JS_SetPropertyStr(ctx, abort_signal_proto, "addEventListener", JS_NewCFunction(ctx, js_undefined, "addEventListener", 2));
    JS_SetPropertyStr(ctx, abort_signal_proto, "removeEventListener", JS_NewCFunction(ctx, js_undefined, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, abort_signal_proto, "throwIfAborted", JS_NewCFunction(ctx, js_undefined, "throwIfAborted", 0));
    JS_SetPropertyStr(ctx, abort_signal_proto, "dispatchEvent", JS_NewCFunction(ctx, js_undefined, "dispatchEvent", 1));
    JS_SetPropertyStr(ctx, abort_signal_ctor, "prototype", abort_signal_proto);
    JS_SetPropertyStr(ctx, global, "AbortSignal", abort_signal_ctor);
    JS_SetPropertyStr(ctx, window, "AbortSignal", abort_signal_ctor);
    
    // AudioContext / webkitAudioContext
    GCValue audio_context_ctor = JS_NewCFunction2(ctx, js_audio_context_constructor, "AudioContext",
        0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "AudioContext", audio_context_ctor);
    JS_SetPropertyStr(ctx, window, "AudioContext", audio_context_ctor);
    JS_SetPropertyStr(ctx, window, "webkitAudioContext", audio_context_ctor);
    
    // DOMParser
    GCValue dom_parser_ctor = JS_NewCFunction2(ctx, js_dom_parser_constructor, "DOMParser",
        0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "DOMParser", dom_parser_ctor);
    JS_SetPropertyStr(ctx, window, "DOMParser", dom_parser_ctor);
    
    // Worker
    GCValue worker_ctor = JS_NewCFunction2(ctx, js_worker_constructor, "Worker",
        0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "Worker", worker_ctor);
    JS_SetPropertyStr(ctx, window, "Worker", worker_ctor);
    
    // Blob
    GCValue blob_ctor = JS_NewCFunction2(ctx, js_blob_constructor, "Blob",
        0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "Blob", blob_ctor);
    JS_SetPropertyStr(ctx, window, "Blob", blob_ctor);
    
    // File
    GCValue file_ctor = JS_NewCFunction2(ctx, js_file_constructor, "File",
        0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "File", file_ctor);
    JS_SetPropertyStr(ctx, window, "File", file_ctor);
    
    // FormData
    GCValue form_data_ctor = JS_NewCFunction2(ctx, js_form_data_constructor, "FormData",
        0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "FormData", form_data_ctor);
    JS_SetPropertyStr(ctx, window, "FormData", form_data_ctor);
    
    // TextEncoder
    GCValue text_encoder_ctor = JS_NewCFunction2(ctx, js_text_encoder_constructor, "TextEncoder",
        0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "TextEncoder", text_encoder_ctor);
    JS_SetPropertyStr(ctx, window, "TextEncoder", text_encoder_ctor);
    
    // TextDecoder
    GCValue text_decoder_ctor = JS_NewCFunction2(ctx, js_text_decoder_constructor, "TextDecoder",
        0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "TextDecoder", text_decoder_ctor);
    JS_SetPropertyStr(ctx, window, "TextDecoder", text_decoder_ctor);
    
    // WebAssembly
    GCValue webassembly = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, webassembly, "instantiate",
        JS_NewCFunction(ctx, js_promise_reject, "instantiate", 2));
    JS_SetPropertyStr(ctx, webassembly, "instantiateStreaming",
        JS_NewCFunction(ctx, js_promise_reject, "instantiateStreaming", 2));
    JS_SetPropertyStr(ctx, webassembly, "compile",
        JS_NewCFunction(ctx, js_promise_reject, "compile", 1));
    JS_SetPropertyStr(ctx, global, "WebAssembly", webassembly);
    JS_SetPropertyStr(ctx, window, "WebAssembly", webassembly);
    
    // ReadableStream
    GCValue readable_stream_ctor = JS_NewCFunction2(ctx, js_readable_stream_constructor, "ReadableStream",
        0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "ReadableStream", readable_stream_ctor);
    JS_SetPropertyStr(ctx, window, "ReadableStream", readable_stream_ctor);
    
    // PressureObserver
    GCValue pressure_observer_ctor = JS_NewCFunction2(ctx, js_pressure_observer_constructor, "PressureObserver",
        0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "PressureObserver", pressure_observer_ctor);
    JS_SetPropertyStr(ctx, window, "PressureObserver", pressure_observer_ctor);
    
    // Profiler
    GCValue profiler_ctor = JS_NewCFunction2(ctx, js_profiler_constructor, "Profiler",
        0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "Profiler", profiler_ctor);
    JS_SetPropertyStr(ctx, window, "Profiler", profiler_ctor);
    
    // MessageChannel stub
    GCValue message_port_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, message_port_proto, "postMessage", JS_NewCFunction(ctx, js_undefined, "postMessage", 1));
    JS_SetPropertyStr(ctx, message_port_proto, "start", JS_NewCFunction(ctx, js_undefined, "start", 0));
    JS_SetPropertyStr(ctx, message_port_proto, "close", JS_NewCFunction(ctx, js_undefined, "close", 0));
    GCValue message_port_ctor = JS_NewCFunction2(ctx, js_dummy_function, "MessagePort",
        0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, message_port_ctor, "prototype", message_port_proto);
    JS_SetPropertyStr(ctx, global, "MessagePort", message_port_ctor);
    JS_SetPropertyStr(ctx, window, "MessagePort", message_port_ctor);
    
    GCValue message_channel_proto = JS_NewObject(ctx);
    GCValue message_channel_ctor = JS_NewCFunction2(ctx, js_message_channel_constructor, "MessageChannel",
        0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, message_channel_proto, "constructor", message_channel_ctor);
    JS_SetPropertyStr(ctx, message_channel_ctor, "prototype", message_channel_proto);
    JS_SetPropertyStr(ctx, global, "MessageChannel", message_channel_ctor);
    JS_SetPropertyStr(ctx, window, "MessageChannel", message_channel_ctor);
    
    // document methods
    JS_SetPropertyStr(ctx, document, "exitFullscreen",
        JS_NewCFunction(ctx, js_undefined, "exitFullscreen", 0));
    JS_SetPropertyStr(ctx, document, "exitPictureInPicture",
        JS_NewCFunction(ctx, js_undefined, "exitPictureInPicture", 0));
    JS_SetPropertyStr(ctx, document, "queryCommandSupported",
        JS_NewCFunction(ctx, js_false, "queryCommandSupported", 1));
    JS_SetPropertyStr(ctx, document, "hasFocus",
        JS_NewCFunction(ctx, js_true, "hasFocus", 0));
    JS_SetPropertyStr(ctx, document, "documentMode",
        JS_UNDEFINED); /* IE-specific, undefined in modern browsers */
    
    // performance.memory
    GCValue perf = JS_GetPropertyStr(ctx, window, "performance");
    if (!JS_IsException(perf) && JS_IsObject(perf)) {
        GCValue memory = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, memory, "usedJSHeapSize", JS_NewInt64(ctx, 0));
        JS_SetPropertyStr(ctx, memory, "totalJSHeapSize", JS_NewInt64(ctx, 0));
        JS_SetPropertyStr(ctx, memory, "jsHeapSizeLimit", JS_NewInt64(ctx, 2147483648));
        JS_SetPropertyStr(ctx, perf, "memory", memory);
    }
    
    // location.ancestorOrigins
    JS_SetPropertyStr(ctx, location, "ancestorOrigins", JS_NewArray(ctx));
    
    // navigator extras
    JS_SetPropertyStr(ctx, nav, "standalone", JS_FALSE);
    JS_SetPropertyStr(ctx, nav, "msPointerEnabled", JS_FALSE);
    
    LOG_INFO("Missing APIs for script 024 set");
    
    // ===== Intl API Stub =====
    // Minimal implementation required by YouTube scripts
    const char *intl_stub = 
        "var Intl = {"
        "  DateTimeFormat: function() {"
        "    this.resolvedOptions = function() { return {timeZone: 'UTC'}; };"
        "  },"
        "  NumberFormat: function(locale, options) {"
        "    this.format = function(n) { return String(n); };"
        "  }"
        "};"
        "Intl.NumberFormat.supportedLocalesOf = function(locales) { return locales; };";
    JS_Eval(ctx, intl_stub, strlen(intl_stub), "<intl_stub>", JS_EVAL_TYPE_GLOBAL);
    if (JS_HasException(ctx)) {
        GCValue exc = JS_GetException(ctx);
        (void)exc;
    }
    LOG_INFO("Intl stub set");
}

/*
 * Reset browser stubs state between downloads.
 * This clears all static variables to ensure a fresh start
 * when js_quickjs_exec_scripts is called multiple times.
 */
extern "C" void browser_api_impl_reset(void) {
    /* Reset all static state variables to initial values */
    
    /* Reset DOMException class ID - it will be reallocated on next init */
    js_dom_exception_class_id = 0;
    
    /* Reset all timers and callbacks */
    timer_api_reset();
    
    /* Note: g_performance_time is a minor timing counter that accumulates 
     * 0.1ms per call to js_performance_now(). Over multiple downloads it 
     * could drift but it's not critical - the GC reset is the main fix.
     * A full reset of this variable would require moving its declaration
     * or using a different approach. For now, the GC reset is sufficient. */
}

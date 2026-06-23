#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <ctype.h>
#include <time.h>
#include "js_quickjs.h"
#include "cutils.h"
#include "quickjs.h"
#include "quickjs_gc_unified.h"
#include "browser_api_impl.h"
#include "html_dom.h"
#include "css_parser.h"
#include "gc_value_helpers.h"
#include "platform.h"
#include "browser_api_impl_types.h"
#include "browser_api_impl_handles.h"
#include "session_state.h"

// Forward declaration from dom_api.cpp (used to set ownerDocument on created nodes)
extern "C" void dom_node_set_owner_document(JSContextHandle ctx, GCValue node, GCValue doc);

/* Timer API functions from browser_api_impl.cpp */
extern "C" int timer_process_due(JSContextHandle ctx);
extern "C" int timer_has_pending(void);
extern "C" void timer_api_reset(void);

/* Logging wrapper that uses platform abstraction */
static void log_to_file(const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    platform_vlog(LOG_LEVEL_INFO, tag, fmt, args);
    va_end(args);
}

#define MAX_CAPTURED_URLS 64
#define URL_MAX_LEN 2048

#ifdef BE_PLATFORM_ANDROID
// Global asset manager for loading browser stubs (Android only)
static AAssetManager *g_asset_mgr = NULL;

// Set the global asset manager (call from main.c during startup)
void js_quickjs_set_asset_manager(AAssetManager *mgr) {
    g_asset_mgr = mgr;
}
#endif

// Forward declarations
static GCValue js_dummy_function(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static bool is_youtube_url(const char *url);
static int collect_user_headers(JSContextHandle ctx, GCValue headers_obj,
                                char header_bufs[][512], const char **headers_out,
                                int max_count);
static const char* resolve_visitor_id(JSContextHandle ctx, char *out_buf, size_t out_len);

// CSSStyleDeclaration.removeProperty stub
GCValue js_style_remove_property(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    const char *prop = JS_ToCString(ctx, argv[0]);
    if (!prop) return JS_NewString(ctx, "");
    // Get old value
    GCValue old_val = JS_GetPropertyStr(ctx, this_val, prop);
    const char *old_str = JS_IsString(old_val) ? JS_ToCString(ctx, old_val) : "";
    // Delete the property
    JSAtom atom = JS_NewAtom(ctx, prop);
    JS_DeleteProperty(ctx, this_val, atom, 0);
    JS_FreeAtom(ctx, atom);
    return JS_NewString(ctx, old_str ? old_str : "");
}

// CSSStyleDeclaration.setProperty stub
GCValue js_style_set_property(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    const char *prop = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    if (prop && value) {
        JS_SetPropertyStr(ctx, this_val, prop, JS_NewString(ctx, value));
    }
    return JS_UNDEFINED;
}

// CSSStyleDeclaration.getPropertyValue stub
GCValue js_style_get_property_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    const char *prop = JS_ToCString(ctx, argv[0]);
    if (!prop) return JS_NewString(ctx, "");
    GCValue val = JS_GetPropertyStr(ctx, this_val, prop);
    if (JS_IsString(val)) {
        return val;
    }
    return JS_NewString(ctx, "");
}

/**
 * Helper: Safely log a QuickJS exception to Android logcat.
 * 
 * This function safely handles JS exceptions by:
 * - Checking if the exception is a valid string before conversion
 * - Avoiding crashes on null/undefined/non-string exception values
 * - Automatically freeing the C string if conversion succeeds
 * 
 * @param ctx       QuickJS context
 * @param tag       Log tag for Android logcat
 * @param fmt       Format string for the log message (should include %s for error)
 * @param get_stack If true, also logs the stack trace if available
 */
static void js_quickjs_log_exception(JSContextHandle ctx, const char *tag, const char *fmt, bool get_stack) {
    GCValue exception = JS_GetException(ctx);
    if (JS_IsUndefined(exception) || JS_IsNull(exception)) {
        platform_log(LOG_LEVEL_WARN, tag, fmt, "(undefined/null exception)");
        return;
    }
    
    // Convert exception to string - handles both string exceptions and Error objects
    const char *error = NULL;
    // First try to get the 'message' property for Error objects
    if (!JS_IsString(exception)) {
        GCValue msg_val = JS_GetPropertyStr(ctx, exception, "message");
        if (!JS_IsUndefined(msg_val) && !JS_IsNull(msg_val)) {
            error = JS_ToCString(ctx, msg_val);
        }
    }
    
    // If that didn't work, try converting directly to string
    if (!error) {
        error = JS_ToCString(ctx, exception);
    }
    
    // Log the main error message
    platform_log(LOG_LEVEL_WARN, tag, fmt, error ? error : "(failed to stringify exception)");
    
    // Optionally log stack trace
    if (get_stack) {
        GCValue stack_val = JS_GetPropertyStr(ctx, exception, "stack");
        if (!JS_IsUndefined(stack_val) && !JS_IsNull(stack_val)) {
            const char *stack = JS_ToCString(ctx, stack_val);
            if (stack) {
                platform_log(LOG_LEVEL_WARN, tag, "Stack: %s", stack);
            }
        }
        // Also try to get the exception name/type
        GCValue name_val = JS_GetPropertyStr(ctx, exception, "name");
        if (!JS_IsUndefined(name_val) && !JS_IsNull(name_val)) {
            const char *name = JS_ToCString(ctx, name_val);
            if (name) {
                platform_log(LOG_LEVEL_WARN, tag, "Exception type: %s", name);
            }
        }
    }
    
    if (error) {
    }
}

static JSClassID js_http_response_class_id;
extern "C" JSClassID js_xhr_class_id = 0;
extern "C" JSClassID js_video_class_id = 0;

// Global state for URL capture
static char g_captured_urls[MAX_CAPTURED_URLS][URL_MAX_LEN];
static int g_captured_url_count = 0;
static pthread_mutex_t g_url_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Global QuickJS runtime and context - initialized once in android_main */
JSRuntimeHandle g_js_runtime;
JSContextHandle g_js_context;

// Record a captured URL
// BUG FIX #1: Fixed buffer overflow using memcpy with explicit length validation
void record_captured_url(const char *url) {
    if (!url) return;
    
    size_t url_len = strlen(url);
    if (url_len == 0 || url_len >= URL_MAX_LEN) {
        return;
    }
    
    /* Log captured URL for debugging decryption */
    /* Check if URL has signature (decrypted) or needs decryption */
    bool has_sig = strstr(url, "sig=") != NULL || strstr(url, "signature=") != NULL;
    bool has_cipher = strstr(url, "signatureCipher=") != NULL || strstr(url, "sc=") != NULL;
    const char *url_type = has_sig ? "[DECRYPTED]" : (has_cipher ? "[ENCRYPTED]" : "[PLAIN]");
    
    log_to_file("js_quickjs", "[URL_CAPTURED] %s %s", url_type, url);
    platform_log(LOG_LEVEL_INFO, "js_quickjs", "[URL_CAPTURED] %s %s", url_type, url);
    
    pthread_mutex_lock(&g_url_mutex);
    
    // Check for duplicates
    for (int i = 0; i < g_captured_url_count; i++) {
        if (strcmp(g_captured_urls[i], url) == 0) {
            pthread_mutex_unlock(&g_url_mutex);
            return;
        }
    }
    
    // Add new URL using memcpy for safe copy
    if (g_captured_url_count < MAX_CAPTURED_URLS) {
        memcpy(g_captured_urls[g_captured_url_count], url, url_len);
        g_captured_urls[g_captured_url_count][url_len] = '\0';
        g_captured_url_count++;
    }
    
    pthread_mutex_unlock(&g_url_mutex);
}

// XMLHttpRequest implementation with full event simulation
// Note: XMLHttpRequest struct is defined in browser_api_impl_types.h

static void js_xhr_finalizer(JSRuntimeHandle rt, GCValue val) {
    // XMLHttpRequest memory is managed by GC, no manual cleanup needed
    // GCValue fields are automatically garbage collected
    (void)rt;
    (void)val;
}

static void js_xhr_mark(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func) {
    GCHandle xhr_handle = JS_GetOpaqueHandle(val, js_xhr_class_id);
    if (xhr_handle == GC_HANDLE_NULL) return;
    mark_func(rt, xhr_handle);
    XMLHttpRequest *xhr = (XMLHttpRequest *)gc_deref(xhr_handle);
    if (!xhr) return;
    JS_MarkValue(rt, xhr->onload, mark_func);
    JS_MarkValue(rt, xhr->onerror, mark_func);
    JS_MarkValue(rt, xhr->onreadystatechange, mark_func);
    JS_MarkValue(rt, xhr->headers, mark_func);
}

GCValue js_xhr_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    XMLHttpRequestHandle xhr = XMLHttpRequestHandle::create(ctx);
    if (!xhr.valid()) return JS_ThrowTypeError(ctx, "XMLHttpRequest internal error");
    
    xhr.set_headers(JS_NewObject(ctx));
    xhr.set_onload(JS_NULL);
    xhr.set_onerror(JS_NULL);
    xhr.set_onreadystatechange(JS_NULL);
    
    GCValue obj = JS_NewObjectClass(ctx, js_xhr_class_id);
    xhr.attach_to_object(obj);
    return obj;
}

static void js_xhr_fire_state(JSContextHandle ctx, XMLHttpRequestHandle xhr, GCValue this_val, int state) {
    xhr.set_ready_state(state);
    GCValue cb = JS_GetPropertyStr(ctx, this_val, "onreadystatechange");
    if (JS_IsFunction(ctx, cb)) {
        JS_Call(ctx, cb, this_val, 0, NULL);
    }
}

static GCValue js_xhr_open(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    XMLHttpRequestHandle xhr = XMLHttpRequestHandle::from_object_check(ctx, this_val);
    if (!xhr.valid()) return JS_ThrowTypeError(ctx, "XMLHttpRequest internal error");
    
    const char *method = JS_ToCString(ctx, argv[0]);
    const char *url = JS_ToCString(ctx, argv[1]);
    
    if (method && url) {
        xhr.set_method(method);
        xhr.set_url(url);
        xhr.set_ready_state(1); // OPENED
        xhr.set_status(0);
        xhr.set_response_text("");
        xhr.set_response_headers("");
        xhr.set_request_body("");
        
        // Capture the URL - this is where we intercept requests
        record_captured_url(url);
    }
    
    return JS_UNDEFINED;
}

static GCValue js_xhr_send(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    XMLHttpRequestHandle xhr = XMLHttpRequestHandle::from_object_check(ctx, this_val);
    if (!xhr.valid()) return JS_ThrowTypeError(ctx, "XMLHttpRequest internal error");
    
    // Store request body
    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (JS_IsString(argv[0])) {
            const char *s = JS_ToCString(ctx, argv[0]);
            if (s) xhr.set_request_body(s);
        } else if (JS_IsObject(argv[0])) {
            GCValue json_str = JS_JSONStringify(ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);
            if (!JS_IsException(json_str) && JS_IsString(json_str)) {
                const char *s = JS_ToCString(ctx, json_str);
                if (s) xhr.set_request_body(s);
            }
        }
    }
    
    const char *url = xhr.url();
    if (url && is_youtube_url(url)) {
        bool is_post = (strcasecmp(xhr.method(), "POST") == 0);
        char header_bufs[16][512];
        const char *headers[16];
        int header_count = 0;
        
        // User headers stored on the XHR object
        header_count = collect_user_headers(ctx, xhr.headers(), header_bufs, headers, 16);
        
        if (is_post) {
            headers[header_count++] = "Content-Type: application/json";
        }
        headers[header_count++] = "X-YouTube-Client-Name: 1";
        
        char client_version_header[128] = {0};
        snprintf(client_version_header, sizeof(client_version_header),
                 "X-YouTube-Client-Version: 2.20250122.04.00");
        headers[header_count++] = client_version_header;
        
        char visitor_id_header[256] = {0};
        char visitor_buf[256] = {0};
        const char *visitor_id = resolve_visitor_id(ctx, visitor_buf, sizeof(visitor_buf));
        if (visitor_id && visitor_id[0]) {
            snprintf(visitor_id_header, sizeof(visitor_id_header),
                     "X-Goog-Visitor-Id: %s", visitor_id);
            headers[header_count++] = visitor_id_header;
        }
        
        char cookie_header[1024] = {0};
        const char *cookies = platform_http_get_cookies();
        if (cookies && cookies[0]) {
            snprintf(cookie_header, sizeof(cookie_header), "Cookie: %s", cookies);
            headers[header_count++] = cookie_header;
        }
        
        PlatformHttpBuffer response_buffer = {0};
        char error_buf[256] = {0};
        bool success = false;
        int status_code = 0;
        const char *req_body = xhr.request_body();
        size_t req_body_len = strlen(req_body);
        
        if (is_post && req_body_len > 0) {
            success = platform_http_post(url, req_body, req_body_len,
                                         headers, header_count, &response_buffer,
                                         &status_code, error_buf, sizeof(error_buf));
        } else {
            success = platform_http_get_with_headers(url, headers, header_count,
                                                     &response_buffer, error_buf,
                                                     sizeof(error_buf));
            status_code = success ? 200 : 0;
        }
        
        if (success && response_buffer.data && response_buffer.size > 0 && status_code >= 200 && status_code < 300) {
            xhr.set_status(status_code);
            xhr.set_response_text(response_buffer.data);
            char rh[2048];
            snprintf(rh, sizeof(rh), "HTTP/1.1 %d OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n", status_code, response_buffer.size);
            xhr.set_response_headers(rh);
            platform_http_free_buffer(&response_buffer);
            js_xhr_fire_state(ctx, xhr, this_val, 2); // HEADERS_RECEIVED
            js_xhr_fire_state(ctx, xhr, this_val, 3); // LOADING
            js_xhr_fire_state(ctx, xhr, this_val, 4); // DONE
            GCValue onload = JS_GetPropertyStr(ctx, this_val, "onload");
            if (JS_IsFunction(ctx, onload)) JS_Call(ctx, onload, this_val, 0, NULL);
            return JS_UNDEFINED;
        } else {
            platform_http_free_buffer(&response_buffer);
            xhr.set_status(0);
            xhr.set_response_text("");
            js_xhr_fire_state(ctx, xhr, this_val, 4); // DONE
            GCValue onerror = JS_GetPropertyStr(ctx, this_val, "onerror");
            if (JS_IsFunction(ctx, onerror)) JS_Call(ctx, onerror, this_val, 0, NULL);
            return JS_UNDEFINED;
        }
    }
    
    // Fallback for non-YouTube / non-network URLs
    xhr.set_ready_state(4); // DONE
    xhr.set_status(200);
    xhr.set_response_text("{}");
    
    GCValue onreadystatechange = JS_GetPropertyStr(ctx, this_val, "onreadystatechange");
    if (JS_IsFunction(ctx, onreadystatechange)) {
        JS_Call(ctx, onreadystatechange, this_val, 0, NULL);
    }
    GCValue onload = JS_GetPropertyStr(ctx, this_val, "onload");
    if (JS_IsFunction(ctx, onload)) {
        JS_Call(ctx, onload, this_val, 0, NULL);
    }
    
    return JS_UNDEFINED;
}

static GCValue js_xhr_set_request_header(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    XMLHttpRequestHandle xhr = XMLHttpRequestHandle::from_object_check(ctx, this_val);
    if (!xhr.valid()) return JS_ThrowTypeError(ctx, "XMLHttpRequest internal error");
    if (argc < 2) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    if (name && value) {
        JS_SetPropertyStr(ctx, xhr.headers(), name, JS_NewString(ctx, value));
    }
    return JS_UNDEFINED;
}

static GCValue js_xhr_get_response_header(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    XMLHttpRequestHandle xhr = XMLHttpRequestHandle::from_object_check(ctx, this_val);
    if (!xhr.valid()) return JS_ThrowTypeError(ctx, "XMLHttpRequest internal error");
    if (argc < 1) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;
    const char *headers = xhr.response_headers();
    size_t name_len = strlen(name);
    const char *p = headers;
    while (*p) {
        const char *line_end = strchr(p, '\n');
        if (!line_end) line_end = p + strlen(p);
        const char *colon = (const char*)memchr(p, ':', line_end - p);
        if (colon) {
            const char *key = p;
            size_t key_len = colon - p;
            while (key_len > 0 && (key[key_len - 1] == ' ' || key[key_len - 1] == '\t')) key_len--;
            if (key_len == name_len && strncasecmp(key, name, name_len) == 0) {
                const char *val = colon + 1;
                while (val < line_end && (*val == ' ' || *val == '\t')) val++;
                size_t val_len = line_end - val;
                while (val_len > 0 && (val[val_len - 1] == '\r' || val[val_len - 1] == '\n')) val_len--;
                char buf[512];
                if (val_len >= sizeof(buf)) val_len = sizeof(buf) - 1;
                memcpy(buf, val, val_len);
                buf[val_len] = '\0';
                return JS_NewString(ctx, buf);
            }
        }
        p = line_end;
        if (*p == '\n') p++;
    }
    return JS_NULL;
}

static GCValue js_xhr_get_all_response_headers(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    XMLHttpRequestHandle xhr = XMLHttpRequestHandle::from_object_check(ctx, this_val);
    if (!xhr.valid()) return JS_ThrowTypeError(ctx, "XMLHttpRequest internal error");
    return JS_NewString(ctx, xhr.response_headers());
}

static GCValue js_xhr_get_ready_state(JSContextHandle ctx, GCValue this_val) {
    XMLHttpRequestHandle xhr = XMLHttpRequestHandle::from_object_check(ctx, this_val);
    if (!xhr.valid()) return JS_ThrowTypeError(ctx, "XMLHttpRequest internal error");
    return JS_NewInt32(ctx, xhr.ready_state());
}

static GCValue js_xhr_get_status(JSContextHandle ctx, GCValue this_val) {
    XMLHttpRequestHandle xhr = XMLHttpRequestHandle::from_object_check(ctx, this_val);
    if (!xhr.valid()) return JS_ThrowTypeError(ctx, "XMLHttpRequest internal error");
    return JS_NewInt32(ctx, xhr.status());
}

static GCValue js_xhr_get_response_text(JSContextHandle ctx, GCValue this_val) {
    XMLHttpRequestHandle xhr = XMLHttpRequestHandle::from_object_check(ctx, this_val);
    if (!xhr.valid()) return JS_ThrowTypeError(ctx, "XMLHttpRequest internal error");
    return JS_NewString(ctx, xhr.response_text());
}

extern "C" const JSCFunctionListEntry js_xhr_proto_funcs[] = {
    JS_CFUNC_DEF("open", 3, js_xhr_open),
    JS_CFUNC_DEF("send", 1, js_xhr_send),
    JS_CFUNC_DEF("setRequestHeader", 2, js_xhr_set_request_header),
    JS_CFUNC_DEF("getResponseHeader", 1, js_xhr_get_response_header),
    JS_CFUNC_DEF("getAllResponseHeaders", 0, js_xhr_get_all_response_headers),
    JS_CGETSET_DEF("readyState", js_xhr_get_ready_state, NULL),
    JS_CGETSET_DEF("status", js_xhr_get_status, NULL),
    JS_CGETSET_DEF("responseText", js_xhr_get_response_text, NULL),
    JS_PROP_STRING_DEF("responseType", "", JS_PROP_WRITABLE),
    JS_PROP_STRING_DEF("response", "", JS_PROP_WRITABLE),
};
extern "C" const size_t js_xhr_proto_funcs_count = sizeof(js_xhr_proto_funcs) / sizeof(js_xhr_proto_funcs[0]);

// HTMLVideoElement implementation
// Note: HTMLVideoElement struct is defined in browser_api_impl_types.h

static void js_video_finalizer(JSRuntimeHandle rt, GCValue val) {
    // HTMLVideoElement memory is managed by GC, no manual cleanup needed
    // GCValue fields are automatically garbage collected
    (void)rt;
    (void)val;
}

static void js_video_mark(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func) {
    GCHandle video_handle = JS_GetOpaqueHandle(val, js_video_class_id);
    if (video_handle == GC_HANDLE_NULL) return;
    mark_func(rt, video_handle);
    HTMLVideoElement *vid = (HTMLVideoElement *)gc_deref(video_handle);
    if (!vid) return;
    JS_MarkValue(rt, vid->onloadstart, mark_func);
    JS_MarkValue(rt, vid->onloadedmetadata, mark_func);
    JS_MarkValue(rt, vid->oncanplay, mark_func);
    JS_MarkValue(rt, vid->onplay, mark_func);
    JS_MarkValue(rt, vid->onplaying, mark_func);
    JS_MarkValue(rt, vid->onerror, mark_func);
}

GCValue js_video_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::create(ctx);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    
    vid.set_onloadstart(JS_NULL);
    vid.set_onloadedmetadata(JS_NULL);
    vid.set_oncanplay(JS_NULL);
    vid.set_onplay(JS_NULL);
    vid.set_onplaying(JS_NULL);
    vid.set_onerror(JS_NULL);
    
    GCValue obj = JS_NewObjectClass(ctx, js_video_class_id);
    vid.attach_to_object(obj);
    
    /* Set tagName for accurate HTML emulation */
    JS_SetPropertyStr(ctx, obj, "tagName", JS_NewString(ctx, "VIDEO"));
    
    /* Add style object for CSS property access */
    GCValue style = JS_NewObject(ctx);
    if (!JS_IsException(style)) {
        JS_SetPropertyStr(ctx, style, "animationTimingFunction", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, style, "removeProperty",
            JS_NewCFunction(ctx, js_style_remove_property, "removeProperty", 1));
        JS_SetPropertyStr(ctx, style, "setProperty",
            JS_NewCFunction(ctx, js_style_set_property, "setProperty", 3));
        JS_SetPropertyStr(ctx, style, "getPropertyValue",
            JS_NewCFunction(ctx, js_style_get_property_value, "getPropertyValue", 1));
        JS_SetPropertyStr(ctx, obj, "style", style);
    }
    
    /* Add dimension properties expected by player code */
    JS_SetPropertyStr(ctx, obj, "clientWidth", JS_NewInt32(ctx, 640));
    JS_SetPropertyStr(ctx, obj, "clientHeight", JS_NewInt32(ctx, 360));
    JS_SetPropertyStr(ctx, obj, "offsetWidth", JS_NewInt32(ctx, 640));
    JS_SetPropertyStr(ctx, obj, "offsetHeight", JS_NewInt32(ctx, 360));
    JS_SetPropertyStr(ctx, obj, "scrollWidth", JS_NewInt32(ctx, 640));
    JS_SetPropertyStr(ctx, obj, "scrollHeight", JS_NewInt32(ctx, 360));
    
    return obj;
}

static GCValue js_video_load(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    
    vid.set_network_state(2); // NETWORK_LOADING
    vid.set_ready_state(1);   // HAVE_METADATA
    
    // Trigger events that YouTube player expects
    GCValue onloadstart = vid.onloadstart();
    if (!JS_IsNull(onloadstart)) {
        JS_Call(ctx, onloadstart, this_val, 0, NULL);
    }
    GCValue onloadedmetadata = vid.onloadedmetadata();
    if (!JS_IsNull(onloadedmetadata)) {
        JS_Call(ctx, onloadedmetadata, this_val, 0, NULL);
    }
    
    return JS_UNDEFINED;
}

static GCValue js_video_play(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    
    vid.set_paused(false);
    vid.set_ready_state(4); // HAVE_ENOUGH_DATA
    
    GCValue onplay = vid.onplay();
    if (!JS_IsNull(onplay)) {
        JS_Call(ctx, onplay, this_val, 0, NULL);
    }
    GCValue onplaying = vid.onplaying();
    if (!JS_IsNull(onplaying)) {
        JS_Call(ctx, onplaying, this_val, 0, NULL);
    }
    GCValue oncanplay = vid.oncanplay();
    if (!JS_IsNull(oncanplay)) {
        JS_Call(ctx, oncanplay, this_val, 0, NULL);
    }
    
    return JS_UNDEFINED;
}

static GCValue js_video_pause(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    vid.set_paused(true);
    return JS_UNDEFINED;
}

static GCValue js_video_set_src(JSContextHandle ctx, GCValue this_val, GCValue val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    
    const char *src = JS_ToCString(ctx, val);
    if (src) {
        vid.set_src(src);
        record_captured_url(src);
        
        // Resolve blob: URLs back to the registered object (MediaSource, Blob, etc.)
        if (strncmp(src, "blob:", 5) == 0) {
            GCValue global_obj = JS_GetGlobalObject(ctx);
            GCValue registry = JS_GetPropertyStr(ctx, global_obj, "__blobRegistry");
            if (!JS_IsUndefined(registry) && !JS_IsNull(registry)) {
                GCValue obj = JS_GetPropertyStr(ctx, registry, src);
                if (!JS_IsUndefined(obj) && !JS_IsNull(obj)) {
                    JS_SetPropertyStr(ctx, this_val, "__mediaSource", obj);
                    // If it's a MediaSource, transition to open if needed
                    MediaSourceDataHandle ms = MediaSourceDataHandle::from_object(obj);
                    if (ms.valid()) {
                        ms.set_ready_state(1);
                        JS_SetPropertyStr(ctx, this_val, "__blobUrl", JS_NewString(ctx, src));
                    }
                }
            }
        }
    } else {
        vid.set_src("");
    }
    return JS_UNDEFINED;
}

static GCValue js_video_get_src(JSContextHandle ctx, GCValue this_val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    return JS_NewString(ctx, vid.src());
}

static GCValue js_video_get_id(JSContextHandle ctx, GCValue this_val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    return JS_NewString(ctx, vid.id());
}

static GCValue js_video_set_id(JSContextHandle ctx, GCValue this_val, GCValue val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    const char *id = JS_ToCString(ctx, val);
    if (id) {
        vid.set_id(id);
    }
    return JS_UNDEFINED;
}

static GCValue js_video_get_current_time(JSContextHandle ctx, GCValue this_val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    return JS_NewFloat64(ctx, vid.current_time());
}

static GCValue js_video_set_current_time(JSContextHandle ctx, GCValue this_val, GCValue val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    double time;
    JS_ToFloat64(ctx, &time, val);
    vid.set_current_time(time);
    return JS_UNDEFINED;
}

static GCValue js_video_get_duration(JSContextHandle ctx, GCValue this_val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    return JS_NewFloat64(ctx, vid.duration());
}

static GCValue js_video_get_paused(JSContextHandle ctx, GCValue this_val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    return JS_NewBool(ctx, vid.paused());
}

static GCValue js_video_get_ready_state(JSContextHandle ctx, GCValue this_val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    return JS_NewInt32(ctx, vid.ready_state());
}

static GCValue js_video_get_network_state(JSContextHandle ctx, GCValue this_val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    return JS_NewInt32(ctx, vid.network_state());
}

static GCValue js_video_can_play_type(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_NewString(ctx, "");
    // Support common video/audio formats
    if (strstr(type, "video/mp4") || strstr(type, "audio/mp4") ||
        strstr(type, "video/webm") || strstr(type, "audio/webm") ||
        strstr(type, "video/ogg") || strstr(type, "audio/ogg") ||
        strstr(type, "application/vnd.apple.mpegurl") || // HLS
        strstr(type, "application/x-mpegURL")) {
        return JS_NewString(ctx, "probably");
    }
    return JS_NewString(ctx, "");
}

// Generic getter/setter for event callbacks
// NOTE: With GCValue, we just copy the handle. The GC manages the actual object.
#define DEFINE_VIDEO_EVENT_HANDLER(name, getter_suffix, setter_suffix) \
    static GCValue js_video_get_##name(JSContextHandle ctx, GCValue this_val) { \
        HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val); \
        if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error"); \
        return vid.getter_suffix(); \
    } \
    static GCValue js_video_set_##name(JSContextHandle ctx, GCValue this_val, GCValue val) { \
        HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val); \
        if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error"); \
        vid.setter_suffix(val); \
        return JS_UNDEFINED; \
    }

DEFINE_VIDEO_EVENT_HANDLER(onloadstart, onloadstart, set_onloadstart)
DEFINE_VIDEO_EVENT_HANDLER(onloadedmetadata, onloadedmetadata, set_onloadedmetadata)
DEFINE_VIDEO_EVENT_HANDLER(oncanplay, oncanplay, set_oncanplay)
DEFINE_VIDEO_EVENT_HANDLER(onplay, onplay, set_onplay)
DEFINE_VIDEO_EVENT_HANDLER(onplaying, onplaying, set_onplaying)
DEFINE_VIDEO_EVENT_HANDLER(onerror, onerror, set_onerror)

extern "C" const JSCFunctionListEntry js_video_proto_funcs[] = {
    JS_CFUNC_DEF("load", 0, js_video_load),
    JS_CFUNC_DEF("play", 0, js_video_play),
    JS_CFUNC_DEF("pause", 0, js_video_pause),
    JS_CFUNC_DEF("canPlayType", 1, js_video_can_play_type),
    JS_CGETSET_DEF("id", js_video_get_id, js_video_set_id),
    JS_CGETSET_DEF("src", js_video_get_src, js_video_set_src),
    JS_CGETSET_DEF("currentSrc", js_video_get_src, NULL),
    JS_CGETSET_DEF("currentTime", js_video_get_current_time, js_video_set_current_time),
    JS_CGETSET_DEF("duration", js_video_get_duration, NULL),
    JS_CGETSET_DEF("paused", js_video_get_paused, NULL),
    JS_CGETSET_DEF("readyState", js_video_get_ready_state, NULL),
    JS_CGETSET_DEF("networkState", js_video_get_network_state, NULL),
    JS_CGETSET_DEF("buffered", NULL, NULL),
    JS_CGETSET_DEF("played", NULL, NULL),
    JS_CGETSET_DEF("seekable", NULL, NULL),
    JS_CGETSET_DEF("ended", NULL, NULL),
    JS_CGETSET_DEF("autoplay", NULL, NULL),
    JS_CGETSET_DEF("loop", NULL, NULL),
    JS_CGETSET_DEF("muted", NULL, NULL),
    JS_CGETSET_DEF("volume", NULL, NULL),
    JS_CGETSET_DEF("playbackRate", NULL, NULL),
    JS_CGETSET_DEF("defaultPlaybackRate", NULL, NULL),
    JS_CGETSET_DEF("preload", NULL, NULL),
    JS_CGETSET_DEF("crossOrigin", NULL, NULL),
    JS_CGETSET_DEF("onloadstart", js_video_get_onloadstart, js_video_set_onloadstart),
    JS_CGETSET_DEF("onloadedmetadata", js_video_get_onloadedmetadata, js_video_set_onloadedmetadata),
    JS_CGETSET_DEF("oncanplay", js_video_get_oncanplay, js_video_set_oncanplay),
    JS_CGETSET_DEF("onplay", js_video_get_onplay, js_video_set_onplay),
    JS_CGETSET_DEF("onplaying", js_video_get_onplaying, js_video_set_onplaying),
    JS_CGETSET_DEF("onerror", js_video_get_onerror, js_video_set_onerror),
};
extern "C" const size_t js_video_proto_funcs_count = sizeof(js_video_proto_funcs) / sizeof(js_video_proto_funcs[0]);

// Global fetch implementation

static bool is_youtube_url(const char *url) {
    if (!url) return false;
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) p += 8;
    else return false;
    const char *end = p;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;
    size_t len = end - p;
    return (len >= 11 && strncasecmp(p + len - 11, ".youtube.com", 11) == 0) ||
           (len == 11 && strncasecmp(p, "youtube.com", 11) == 0);
}

static int collect_user_headers(JSContextHandle ctx, GCValue headers_obj,
                                char header_bufs[][512], const char **headers_out,
                                int max_count) {
    if (!JS_IsObject(headers_obj)) return 0;
    JSPropertyEnum *props = NULL;
    uint32_t prop_count = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, headers_obj,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
        return 0;
    }
    int count = 0;
    for (uint32_t i = 0; i < prop_count && count < max_count; i++) {
        const char *name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) continue;
        GCValue val = JS_GetProperty(ctx, headers_obj, props[i].atom);
        const char *val_str = JS_ToCString(ctx, val);
        if (val_str) {
            snprintf(header_bufs[count], 512, "%s: %s", name, val_str);
            headers_out[count] = header_bufs[count];
            count++;
        }
    }
    JS_FreePropertyEnum(ctx, props, prop_count);
    return count;
}

static const char* resolve_visitor_id(JSContextHandle ctx, char *out_buf, size_t out_len) {
    if (!ctx || !out_buf || out_len == 0) return g_visitor_data;
    out_buf[0] = '\0';
    GCValue global_obj = JS_GetGlobalObject(ctx);
    GCValue ytcfg = JS_GetPropertyStr(ctx, global_obj, "ytcfg");
    if (!JS_IsUndefined(ytcfg) && !JS_IsNull(ytcfg)) {
        GCValue vd = JS_Eval(ctx,
            "(typeof ytcfg !== 'undefined' && ytcfg.get) ? ytcfg.get('VISITOR_DATA') : null",
            69, "<ytcfg_vd>", JS_EVAL_TYPE_GLOBAL);
        if (!JS_IsUndefined(vd) && !JS_IsNull(vd)) {
            const char *s = JS_ToCString(ctx, vd);
            if (s && s[0]) {
                snprintf(out_buf, out_len, "%s", s);
                return out_buf;
            }
        }
    }
    return g_visitor_data;
}

static GCValue resolve_blob_url_response(JSContextHandle ctx, const char *url) {
    GCValue global_obj = JS_GetGlobalObject(ctx);
    GCValue registry = JS_GetPropertyStr(ctx, global_obj, "__blobRegistry");
    GCValue result = JS_UNDEFINED;
    if (!JS_IsUndefined(registry) && !JS_IsNull(registry)) {
        GCValue obj = JS_GetPropertyStr(ctx, registry, url);
        if (!JS_IsUndefined(obj) && !JS_IsNull(obj)) {
            GCValue response = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, response, "ok", JS_TRUE);
            JS_SetPropertyStr(ctx, response, "status", JS_NewInt32(ctx, 200));
            JS_SetPropertyStr(ctx, response, "statusText", JS_NewString(ctx, "OK"));
            JS_SetPropertyStr(ctx, response, "headers", JS_NewObject(ctx));
            JS_SetPropertyStr(ctx, response, "body", JS_NULL);
            JS_SetPropertyStr(ctx, response, "redirected", JS_FALSE);
            JS_SetPropertyStr(ctx, response, "type", JS_NewString(ctx, "basic"));
            JS_SetPropertyStr(ctx, response, "url", JS_NewString(ctx, url ? url : ""));
            JS_SetPropertyStr(ctx, response, "__body_text", JS_NewString(ctx, url ? url : ""));
            JS_SetPropertyStr(ctx, response, "text", JS_NewCFunction(ctx, [](JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) -> GCValue {
                GCValue global = JS_GetGlobalObject(ctx);
                GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
                GCValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
                GCValue body_text = JS_GetPropertyStr(ctx, this_val, "__body_text");
                GCValue args[1] = { body_text };
                return JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, args);
            }, "text", 0));
            JS_SetPropertyStr(ctx, response, "json", JS_NewCFunction(ctx, [](JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) -> GCValue {
                GCValue global = JS_GetGlobalObject(ctx);
                GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
                GCValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
                GCValue empty = JS_NewObject(ctx);
                GCValue args[1] = { empty };
                return JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, args);
            }, "json", 0));
            GCValue global = JS_GetGlobalObject(ctx);
            GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
            GCValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
            GCValue args[1] = { response };
            result = JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, args);
        }
    }
    return result;
}

// Helper to create a Response object from HTTP response data
static GCValue create_response_from_data(JSContextHandle ctx, const char *url, const char *data, size_t data_len, int status, const char *statusText) {
    GCValue response = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, response, "ok", JS_NewBool(ctx, status >= 200 && status < 300));
    JS_SetPropertyStr(ctx, response, "status", JS_NewInt32(ctx, status));
    JS_SetPropertyStr(ctx, response, "statusText", JS_NewString(ctx, statusText ? statusText : (status >= 200 && status < 300 ? "OK" : "Error")));
    JS_SetPropertyStr(ctx, response, "headers", JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, response, "body", JS_NULL);
    JS_SetPropertyStr(ctx, response, "redirected", JS_FALSE);
    JS_SetPropertyStr(ctx, response, "type", JS_NewString(ctx, "basic"));
    JS_SetPropertyStr(ctx, response, "url", JS_NewString(ctx, url ? url : ""));
    
    // Store response data
    JS_SetPropertyStr(ctx, response, "__body_text", JS_NewStringLen(ctx, data, data_len));
    
    // Try to parse as JSON
    GCValue json_val = JS_ParseJSON(ctx, data, data_len, "<fetch-response>");
    if (JS_IsException(json_val)) {
        json_val = JS_UNDEFINED;
    }
    if (!JS_IsUndefined(json_val)) {
        JS_SetPropertyStr(ctx, response, "__body_json", json_val);
    }
    
    // text() -> Promise.resolve(__body_text)
    JS_SetPropertyStr(ctx, response, "text", JS_NewCFunction(ctx, [](JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) -> GCValue {
        GCValue global = JS_GetGlobalObject(ctx);
        GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
        GCValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
        GCValue body_text = JS_GetPropertyStr(ctx, this_val, "__body_text");
        GCValue args[1] = { body_text };
        return JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, args);
    }, "text", 0));
    
    // json() -> Promise.resolve(__body_json or {})
    JS_SetPropertyStr(ctx, response, "json", JS_NewCFunction(ctx, [](JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) -> GCValue {
        GCValue global = JS_GetGlobalObject(ctx);
        GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
        GCValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
        GCValue body_json = JS_GetPropertyStr(ctx, this_val, "__body_json");
        if (JS_IsUndefined(body_json)) {
            body_json = JS_NewObject(ctx);
        }
        GCValue args[1] = { body_json };
        return JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, args);
    }, "json", 0));
    
    // arrayBuffer() -> Promise.resolve(new ArrayBuffer(data_len))
    JS_SetPropertyStr(ctx, response, "arrayBuffer", JS_NewCFunction(ctx, [](JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) -> GCValue {
        GCValue global = JS_GetGlobalObject(ctx);
        GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
        GCValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
        GCValue body_text = JS_GetPropertyStr(ctx, this_val, "__body_text");
        size_t len;
        const char *str = JS_ToCStringLen(ctx, &len, body_text);
        GCValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t*)str, len);
        GCValue args[1] = { ab };
        return JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, args);
    }, "arrayBuffer", 0));
    
    // blob() -> Promise.resolve(new Blob([]))
    JS_SetPropertyStr(ctx, response, "blob", JS_NewCFunction(ctx, [](JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) -> GCValue {
        GCValue global = JS_GetGlobalObject(ctx);
        GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
        GCValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
        GCValue blob = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, blob, "size", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, blob, "type", JS_NewString(ctx, ""));
        GCValue args[1] = { blob };
        return JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, args);
    }, "blob", 0));
    
    return response;
}


GCValue js_fetch(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    const char *url = NULL;
    
    // Handle Request objects
    if (argc > 0) {
        GCValue url_val = argv[0];
        GCValue url_prop = JS_GetPropertyStr(ctx, url_val, "url");
        if (!JS_IsUndefined(url_prop)) {
            const char *url_from_req = JS_ToCString(ctx, url_prop);
            if (url_from_req && url_from_req[0]) {
                url = url_from_req;
            }
        }
        if (!url) {
            url = JS_ToCString(ctx, url_val);
        }
    }
    
    if (url) {
        record_captured_url(url);
    }
    
    // Get method from Request/init object
    const char *method = "GET";
    if (argc > 0 && JS_IsObject(argv[0])) {
        GCValue method_prop = JS_GetPropertyStr(ctx, argv[0], "method");
        if (!JS_IsUndefined(method_prop)) {
            const char *m = JS_ToCString(ctx, method_prop);
            if (m) method = m;
        }
    }
    if (argc > 1 && JS_IsObject(argv[1])) {
        GCValue method_prop = JS_GetPropertyStr(ctx, argv[1], "method");
        if (!JS_IsUndefined(method_prop)) {
            const char *m = JS_ToCString(ctx, method_prop);
            if (m) method = m;
        }
    }
    
    // Extract POST body from fetch() arguments
    char *post_body = NULL;
    size_t post_body_len = 0;
    
    auto extract_body = [&](GCValue body_val) -> char* {
        if (JS_IsUndefined(body_val) || JS_IsNull(body_val)) return NULL;
        if (JS_IsString(body_val)) {
            const char *s = JS_ToCString(ctx, body_val);
            if (s) {
                size_t len = strlen(s);
                char *buf = (char*)malloc(len + 1);
                if (buf) memcpy(buf, s, len + 1);
                return buf;
            }
            return NULL;
        }
        if (JS_IsObject(body_val)) {
            GCValue json_str = JS_JSONStringify(ctx, body_val, JS_UNDEFINED, JS_UNDEFINED);
            if (!JS_IsException(json_str) && JS_IsString(json_str)) {
                const char *s = JS_ToCString(ctx, json_str);
                if (s) {
                    size_t len = strlen(s);
                    char *buf = (char*)malloc(len + 1);
                    if (buf) memcpy(buf, s, len + 1);
                    return buf;
                }
            }
            const char *s = JS_ToCString(ctx, body_val);
            if (s) {
                size_t len = strlen(s);
                char *buf = (char*)malloc(len + 1);
                if (buf) memcpy(buf, s, len + 1);
                return buf;
            }
        }
        return NULL;
    };
    
    // Priority 1: base64 data URL body (YouTube's hidden payload in Request.__original_url)
    if (argc > 0 && JS_IsObject(argv[0])) {
        GCValue orig_url_val = JS_GetPropertyStr(ctx, argv[0], "__original_url");
        if (!JS_IsUndefined(orig_url_val)) {
            const char *orig_url = JS_ToCString(ctx, orig_url_val);
            if (orig_url && strncmp(orig_url, "data:application/json;base64,", 29) == 0) {
                const char *b64 = orig_url + 29;
                size_t b64_len = strlen(b64);
                static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                size_t out_len = (b64_len / 4) * 3;
                if (b64_len > 0 && b64[b64_len - 1] == '=') out_len--;
                if (b64_len > 1 && b64[b64_len - 2] == '=') out_len--;
                post_body = (char*)malloc(out_len + 1);
                if (post_body) {
                    int val = 0, valb = -8;
                    size_t j = 0;
                    for (size_t i = 0; i < b64_len && b64[i] != '='; i++) {
                        const char *p = strchr(b64_table, b64[i]);
                        if (p) {
                            val = (val << 6) + (p - b64_table);
                            valb += 6;
                            if (valb >= 0) {
                                post_body[j++] = (char)((val >> valb) & 0xFF);
                                valb -= 8;
                            }
                        }
                    }
                    post_body[j] = '\0';
                    post_body_len = j;
                }
            }
        }
    }
    
    // Priority 2: init.body
    if (!post_body && argc > 1 && JS_IsObject(argv[1])) {
        GCValue body_prop = JS_GetPropertyStr(ctx, argv[1], "body");
        post_body = extract_body(body_prop);
        if (post_body) post_body_len = strlen(post_body);
    }
    
    // Priority 3: Request.body
    if (!post_body && argc > 0 && JS_IsObject(argv[0])) {
        GCValue body_prop = JS_GetPropertyStr(ctx, argv[0], "body");
        post_body = extract_body(body_prop);
        if (post_body) post_body_len = strlen(post_body);
    }
    
    // Resolve blob: URLs locally
    if (url && strncmp(url, "blob:", 5) == 0) {
        if (post_body) free(post_body);
        GCValue blob_resp = resolve_blob_url_response(ctx, url);
        if (!JS_IsUndefined(blob_resp)) return blob_resp;
        // Fall through to default empty response
    }
    
    // Real HTTP(S) handling for YouTube endpoints
    if (url && is_youtube_url(url)) {
        bool is_post = (strcasecmp(method, "POST") == 0);
        
        char header_bufs[16][512];
        const char *headers[16];
        int header_count = 0;
        
        // Collect user-provided headers
        if (argc > 1 && JS_IsObject(argv[1])) {
            GCValue headers_obj = JS_GetPropertyStr(ctx, argv[1], "headers");
            header_count = collect_user_headers(ctx, headers_obj, header_bufs, headers, 16);
        }
        
        // Default YouTube headers
        if (is_post) {
            headers[header_count++] = "Content-Type: application/json";
        }
        headers[header_count++] = "X-YouTube-Client-Name: 1";
        
        char client_version_header[128] = {0};
        snprintf(client_version_header, sizeof(client_version_header),
                 "X-YouTube-Client-Version: 2.20250122.04.00");
        GCValue global_obj = JS_GetGlobalObject(ctx);
        GCValue ytcfg = JS_GetPropertyStr(ctx, global_obj, "ytcfg");
        if (!JS_IsUndefined(ytcfg) && !JS_IsNull(ytcfg)) {
            GCValue cv = JS_Eval(ctx,
                "(function() { var v = (typeof ytcfg !== 'undefined' && ytcfg.get) ? ytcfg.get('CLIENT_VERSION') : null; if (!v && typeof yt !== 'undefined' && yt.config_ && yt.config_.INNERTUBE_CLIENT_VERSION) v = yt.config_.INNERTUBE_CLIENT_VERSION; return v; })()",
                230, "<ytcfg_cv>", JS_EVAL_TYPE_GLOBAL);
            if (!JS_IsUndefined(cv) && !JS_IsNull(cv)) {
                const char *cv_str = JS_ToCString(ctx, cv);
                if (cv_str && cv_str[0]) {
                    snprintf(client_version_header, sizeof(client_version_header),
                             "X-YouTube-Client-Version: %s", cv_str);
                }
            }
        }
        headers[header_count++] = client_version_header;
        
        char visitor_id_header[256] = {0};
        char visitor_buf[256] = {0};
        const char *visitor_id = resolve_visitor_id(ctx, visitor_buf, sizeof(visitor_buf));
        if (visitor_id && visitor_id[0]) {
            snprintf(visitor_id_header, sizeof(visitor_id_header),
                     "X-Goog-Visitor-Id: %s", visitor_id);
            headers[header_count++] = visitor_id_header;
        }
        
        char cookie_header[1024] = {0};
        const char *cookies = platform_http_get_cookies();
        if (cookies && cookies[0]) {
            snprintf(cookie_header, sizeof(cookie_header), "Cookie: %s", cookies);
            headers[header_count++] = cookie_header;
        }
        
        PlatformHttpBuffer response_buffer = {0};
        char error_buf[256] = {0};
        bool success = false;
        int status_code = 0;
        
        if (is_post && post_body && post_body_len > 0) {
            success = platform_http_post(url, post_body, post_body_len,
                                         headers, header_count, &response_buffer,
                                         &status_code, error_buf, sizeof(error_buf));
        } else {
            success = platform_http_get_with_headers(url, headers, header_count,
                                                     &response_buffer, error_buf,
                                                     sizeof(error_buf));
            status_code = success ? 200 : 0;
        }
        
        if (success && response_buffer.data && response_buffer.size > 0 && status_code >= 200 && status_code < 300) {
            // Capture googlevideo URLs from response
            char *gv_url = strstr(response_buffer.data, "googlevideo.com");
            if (gv_url) {
                char *url_start = gv_url;
                while (url_start > response_buffer.data && url_start[-1] != '"' && url_start[-1] != '\'' && url_start[-1] != ' ') {
                    url_start--;
                }
                char *url_end = gv_url + strlen("googlevideo.com");
                while (*url_end && *url_end != '"' && *url_end != '\'' && *url_end != ' ' && *url_end != '\\') {
                    url_end++;
                }
                size_t len = url_end - url_start;
                if (len > 0 && len < URL_MAX_LEN) {
                    char captured[URL_MAX_LEN];
                    strncpy(captured, url_start, len);
                    captured[len] = '\0';
                    record_captured_url(captured);
                }
            }
            
            GCValue response_obj = create_response_from_data(ctx, url, response_buffer.data,
                                                             response_buffer.size, status_code, "OK");
            platform_http_free_buffer(&response_buffer);
            if (post_body) free(post_body);
            GCValue global = JS_GetGlobalObject(ctx);
            GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
            GCValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
            GCValue args[1] = { response_obj };
            return JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, args);
        } else {
            platform_http_free_buffer(&response_buffer);
            platform_log(LOG_LEVEL_WARN, "js_fetch", "Real request failed: %s", error_buf[0] ? error_buf : "unknown");
            // Fall through to fallback for youtubei player, otherwise default mock
        }
        
        // youtubei/v1/player fallback: use ytInitialPlayerResponse
        if (strstr(url, "youtubei/v1/player") != NULL) {
            GCValue global2 = JS_GetGlobalObject(ctx);
            GCValue ytip = JS_GetPropertyStr(ctx, global2, "ytInitialPlayerResponse");
            if (!JS_IsUndefined(ytip) && !JS_IsNull(ytip)) {
                GCValue json_str = JS_JSONStringify(ctx, ytip, JS_UNDEFINED, JS_UNDEFINED);
                const char *json_cstr = JS_ToCString(ctx, json_str);
                if (json_cstr) {
                    const char *scan = json_cstr;
                    while ((scan = strstr(scan, "googlevideo.com")) != NULL) {
                        const char *url_start = scan;
                        while (url_start > json_cstr && url_start[-1] != '"' && url_start[-1] != '\'' && url_start[-1] != ' ') {
                            url_start--;
                        }
                        const char *url_end = scan + strlen("googlevideo.com");
                        while (*url_end && *url_end != '"' && *url_end != '\'' && *url_end != ' ' && *url_end != '\\') {
                            url_end++;
                        }
                        size_t len = url_end - url_start;
                        if (len > 0 && len < URL_MAX_LEN) {
                            char captured[URL_MAX_LEN];
                            strncpy(captured, url_start, len);
                            captured[len] = '\0';
                            record_captured_url(captured);
                        }
                        scan = url_end;
                    }
                    GCValue response_obj = create_response_from_data(ctx, url, json_cstr, strlen(json_cstr), 200, "OK");
                    if (post_body) free(post_body);
                    GCValue global = JS_GetGlobalObject(ctx);
                    GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
                    GCValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
                    GCValue args[1] = { response_obj };
                    return JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, args);
                }
            }
        }
    }
    
    if (post_body) free(post_body);
    
    // Default mock response for data URLs and failed real requests
    GCValue response = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, response, "ok", JS_TRUE);
    JS_SetPropertyStr(ctx, response, "status", JS_NewInt32(ctx, 200));
    JS_SetPropertyStr(ctx, response, "statusText", JS_NewString(ctx, "OK"));
    JS_SetPropertyStr(ctx, response, "headers", JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, response, "body", JS_NULL);
    JS_SetPropertyStr(ctx, response, "redirected", JS_FALSE);
    JS_SetPropertyStr(ctx, response, "type", JS_NewString(ctx, "basic"));
    JS_SetPropertyStr(ctx, response, "url", JS_NewString(ctx, url ? url : ""));
    
    JS_SetPropertyStr(ctx, response, "text", JS_NewCFunction(ctx, [](JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) -> GCValue {
        GCValue global = JS_GetGlobalObject(ctx);
        GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
        GCValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
        GCValue empty_str = JS_NewString(ctx, "");
        GCValue args[1] = { empty_str };
        return JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, args);
    }, "text", 0));
    
    JS_SetPropertyStr(ctx, response, "json", JS_NewCFunction(ctx, [](JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) -> GCValue {
        GCValue global = JS_GetGlobalObject(ctx);
        GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
        GCValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
        GCValue empty_obj = JS_NewObject(ctx);
        GCValue args[1] = { empty_obj };
        return JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, args);
    }, "json", 0));
    
    JS_SetPropertyStr(ctx, response, "arrayBuffer", JS_NewCFunction(ctx, [](JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) -> GCValue {
        GCValue global = JS_GetGlobalObject(ctx);
        GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
        GCValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
        GCValue ab = JS_NewArrayBuffer(ctx, NULL, 0, NULL, NULL, false);
        GCValue args[1] = { ab };
        return JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, args);
    }, "arrayBuffer", 0));
    
    JS_SetPropertyStr(ctx, response, "blob", JS_NewCFunction(ctx, [](JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) -> GCValue {
        GCValue global = JS_GetGlobalObject(ctx);
        GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
        GCValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
        GCValue blob = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, blob, "size", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, blob, "type", JS_NewString(ctx, ""));
        GCValue args[1] = { blob };
        return JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, args);
    }, "blob", 0));
    
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    GCValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
    GCValue args[1] = { response };
    return JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, args);
}


// External reference to DOM node class ID
extern JSClassID js_dom_node_class_id;

// Stub animate function for createElement
static GCValue js_element_animate(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    // Return a stub Animation object
    // IMPORTANT: oncancel is set to null (not undefined) so that feature detection
    // in Web Animations polyfill skips the polyfill code path that causes errors
    GCValue anim = JS_NewObject(ctx);
    if (JS_IsException(anim)) return anim;
    JS_SetPropertyStr(ctx, anim, "oncancel", JS_NULL);
    JS_SetPropertyStr(ctx, anim, "cancel", JS_NewCFunction(ctx, js_element_animate, "cancel", 0));
    JS_SetPropertyStr(ctx, anim, "play", JS_NewCFunction(ctx, js_element_animate, "play", 0));
    JS_SetPropertyStr(ctx, anim, "pause", JS_NewCFunction(ctx, js_element_animate, "pause", 0));
    JS_SetPropertyStr(ctx, anim, "finish", JS_NewCFunction(ctx, js_element_animate, "finish", 0));
    JS_SetPropertyStr(ctx, anim, "currentTime", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, anim, "playbackRate", JS_NewFloat64(ctx, 1));
    JS_SetPropertyStr(ctx, anim, "playState", JS_NewString(ctx, "idle"));
    return anim;
}

// Stub for CanvasRenderingContext2D.fillRect
static GCValue js_canvas_fill_rect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // No-op for color parsing
    (void)argc; (void)argv;
    return JS_UNDEFINED;
}

// Stub for CanvasRenderingContext2D.clearRect
static GCValue js_canvas_clear_rect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // No-op for color parsing
    (void)argc; (void)argv;
    return JS_UNDEFINED;
}

// CanvasRenderingContext2D.getImageData stub
static GCValue js_canvas_get_image_data(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Return mock ImageData with transparent black pixel
    GCValue image_data = JS_NewObject(ctx);
    if (!JS_IsException(image_data)) {
        // Create data array [0, 0, 0, 0] (transparent black)
        GCValue data = JS_NewArray(ctx);
        if (!JS_IsException(data)) {
            JS_SetPropertyUint32(ctx, data, 0, JS_NewInt32(ctx, 0));
            JS_SetPropertyUint32(ctx, data, 1, JS_NewInt32(ctx, 0));
            JS_SetPropertyUint32(ctx, data, 2, JS_NewInt32(ctx, 0));
            JS_SetPropertyUint32(ctx, data, 3, JS_NewInt32(ctx, 0));
            JS_SetPropertyStr(ctx, image_data, "data", data);
        }
        JS_SetPropertyStr(ctx, image_data, "width", JS_NewInt32(ctx, 1));
        JS_SetPropertyStr(ctx, image_data, "height", JS_NewInt32(ctx, 1));
    }
    return image_data;
}

// Canvas getContext method
static GCValue js_canvas_get_context(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) {
        return JS_NULL;
    }
    
    const char *context_type = JS_ToCString(ctx, argv[0]);
    if (!context_type) {
        return JS_NULL;
    }
    
    // Only support "2d" context for color parsing
    if (strcasecmp(context_type, "2d") != 0) {
        return JS_NULL;
    }
    
    // Create a minimal 2D rendering context for color parsing
    GCValue ctx2d = JS_NewObject(ctx);
    if (JS_IsException(ctx2d)) {
        return JS_NULL;
    }
    
    // Add fillStyle property (just a string property, getter/setter would be better but this works for basic parsing)
    JS_SetPropertyStr(ctx, ctx2d, "fillStyle", JS_NewString(ctx, "#000"));
    
    // Add methods needed by the Web Animations polyfill
    JS_SetPropertyStr(ctx, ctx2d, "fillRect",
        JS_NewCFunction(ctx, js_canvas_fill_rect, "fillRect", 4));
    JS_SetPropertyStr(ctx, ctx2d, "clearRect",
        JS_NewCFunction(ctx, js_canvas_clear_rect, "clearRect", 4));
    JS_SetPropertyStr(ctx, ctx2d, "getImageData",
        JS_NewCFunction(ctx, js_canvas_get_image_data, "getImageData", 4));
    
    return ctx2d;
}

// Document and Element stubs with createElement support
GCValue js_document_create_element(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Handle createElementNS which passes (namespace, tagName)
    const char *tag = NULL;
    if (argc == 1) {
        tag = JS_ToCString(ctx, argv[0]);
    } else if (argc >= 2) {
        // For createElementNS, argv[0] is namespace, argv[1] is tagName
        tag = JS_ToCString(ctx, argv[1]);
    }
    
    GCValue elem = JS_NULL;
    
    if (tag) {
        // Create proper video element
        if (strcasecmp(tag, "video") == 0) {
            elem = js_video_constructor(ctx, JS_NULL, 0, NULL);
        } else {
            // Generic element with DOMNode data attached
            elem = JS_NewObjectClass(ctx, js_dom_node_class_id);
            if (JS_IsException(elem)) {
                return JS_NULL;
            }
            
            // Set prototype to HTMLElement.prototype so element methods work
            GCValue global_obj = JS_GetGlobalObject(ctx);
            GCValue html_element_ctor = JS_GetPropertyStr(ctx, global_obj, "HTMLElement");
            if (!JS_IsUndefined(html_element_ctor) && !JS_IsException(html_element_ctor)) {
                GCValue html_element_proto = JS_GetPropertyStr(ctx, html_element_ctor, "prototype");
                if (!JS_IsUndefined(html_element_proto) && !JS_IsException(html_element_proto)) {
                    JS_SetPrototype(ctx, elem, html_element_proto);
                }
            }
            
            // Create and attach DOMNode data
            // For HTML documents, tagName should be uppercase per spec
            char upper_tag[64];
            size_t tag_len = strlen(tag);
            size_t copy_len = tag_len < sizeof(upper_tag) - 1 ? tag_len : sizeof(upper_tag) - 1;
            for (size_t i = 0; i < copy_len; i++) {
                upper_tag[i] = toupper((unsigned char)tag[i]);
            }
            upper_tag[copy_len] = '\0';
            
            DOMNodeHandle node = DOMNodeHandle::create(ctx, DOM_NODE_TYPE_ELEMENT, upper_tag);
            if (node.valid()) {
                node.attach_to_object(elem);
                // Set tagName property (uppercase for HTML elements)
                JS_SetPropertyStr(ctx, elem, "tagName", JS_NewString(ctx, upper_tag));
            }
            
            // Add animate method directly to created elements so feature detection works
            JS_SetPropertyStr(ctx, elem, "animate",
                JS_NewCFunction(ctx, js_element_animate, "animate", 2));
            // Add oncancel property set to null (not undefined) so polyfill skips its code path
            JS_SetPropertyStr(ctx, elem, "oncancel", JS_NULL);
            
            // Add style object for CSS property access (needed by Web Animations polyfill)
            GCValue style = JS_NewObject(ctx);
            if (!JS_IsException(style)) {
                // Add animationTimingFunction property getter/setter
                // The polyfill checks valid easing functions via this property
                JS_SetPropertyStr(ctx, style, "animationTimingFunction", JS_NewString(ctx, ""));
                JS_SetPropertyStr(ctx, style, "removeProperty",
                    JS_NewCFunction(ctx, js_style_remove_property, "removeProperty", 1));
                JS_SetPropertyStr(ctx, style, "setProperty",
                    JS_NewCFunction(ctx, js_style_set_property, "setProperty", 3));
                JS_SetPropertyStr(ctx, style, "getPropertyValue",
                    JS_NewCFunction(ctx, js_style_get_property_value, "getPropertyValue", 1));
                JS_SetPropertyStr(ctx, elem, "style", style);
            }
            
            // Add getContext method for canvas elements (needed by Web Animations polyfill for color parsing)
            if (strcasecmp(tag, "canvas") == 0) {
                JS_SetPropertyStr(ctx, elem, "getContext",
                    JS_NewCFunction(ctx, js_canvas_get_context, "getContext", 1));
                // Add width and height properties
                JS_SetPropertyStr(ctx, elem, "width", JS_NewInt32(ctx, 0));
                JS_SetPropertyStr(ctx, elem, "height", JS_NewInt32(ctx, 0));
            }
            
            // Add content property for template elements (needed by Polymer)
            if (strcasecmp(tag, "template") == 0) {
                GCValue content = JS_NewObject(ctx);
                if (!JS_IsException(content)) {
                    JS_SetPropertyStr(ctx, content, "insertBefore",
                        JS_NewCFunction(ctx, js_dummy_function, "insertBefore", 2));
                    JS_SetPropertyStr(ctx, content, "appendChild",
                        JS_NewCFunction(ctx, js_dummy_function, "appendChild", 1));
                    JS_SetPropertyStr(ctx, content, "cloneNode",
                        JS_NewCFunction(ctx, js_dummy_function, "cloneNode", 1));
                    JS_SetPropertyStr(ctx, content, "firstChild", JS_NULL);
                    JS_SetPropertyStr(ctx, elem, "content", content);
                }
            }
        }
    }
    
    // Set ownerDocument on newly created elements.
    if (!JS_IsNull(elem) && !JS_IsUndefined(elem)) {
        dom_node_set_owner_document(ctx, elem, this_val);
    }
    
    return elem;
}

static GCValue js_document_get_element_by_id(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Return null - YouTube will create its own elements
    return JS_NULL;
}

static GCValue js_document_query_selector(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_NULL;
}

static GCValue js_document_query_selector_all(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_NewArray(ctx);
}

static GCValue js_document_get_head(JSContextHandle ctx, GCValue this_val) {
    GCValue head = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, head, "appendChild", JS_NewCFunction(ctx, js_dummy_function, "appendChild", 1));
    return head;
}

static GCValue js_document_get_body(JSContextHandle ctx, GCValue this_val) {
    GCValue body = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, body, "appendChild", JS_NewCFunction(ctx, js_dummy_function, "appendChild", 1));
    return body;
}

static GCValue js_document_get_document_element(JSContextHandle ctx, GCValue this_val) {
    GCValue html = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, html, "getAttribute", JS_NewCFunction(ctx, js_dummy_function, "getAttribute", 1));
    return html;
}

static GCValue js_element_set_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

static GCValue js_element_get_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_NULL;
}

static GCValue js_element_add_event_listener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Store event handlers on the element for dispatch
    const char *event = JS_ToCString(ctx, argv[0]);
    if (event) {
        char prop[128];
        snprintf(prop, sizeof(prop), "__on%s", event);
        JS_SetPropertyStr(ctx, this_val, prop, argv[1]);
    }
    return JS_UNDEFINED;
}

static GCValue js_element_remove_event_listener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

static GCValue js_dummy_function(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// Native logging function for JavaScript debugging
// Logs to Android logcat so we can see JS-side decryption results
static GCValue js_bgmdwnldr_log(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx;
    (void)this_val;
    
    for (int i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            platform_log(LOG_LEVEL_INFO, "js_quickjs", "[JS] %s", str);
        }
    }
    return JS_UNDEFINED;
}

static GCValue js_console_log(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    char msg[4096] = "";
    size_t off = 0;
    for (int i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            size_t len = strlen(str);
            if (off + len + 2 < sizeof(msg)) {
                if (off > 0) msg[off++] = ' ';
                memcpy(msg + off, str, len);
                off += len;
                msg[off] = '\0';
            }
        }
    }
    if (off > 0) {
        platform_log(LOG_LEVEL_INFO, "console", "%s", msg);
    }
    return JS_UNDEFINED;
}

// Called from quickjs.c when a global var is defined
// This immediately syncs the var to window object
// Note: This function is called directly from quickjs.c via forward declaration
extern "C" void js_quickjs_on_global_var_defined(JSContextHandle ctx, JSAtom var_name)
{
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue window_obj = JS_GetPropertyStr(ctx, global, "window");
    
    // Skip if window doesn't exist or isn't an object
    if (JS_IsUndefined(window_obj) || JS_IsNull(window_obj) || !JS_IsObject(window_obj)) {
        
        
        return;
    }
    
    // Skip if window === global (no need to sync to itself)
    // We check this by comparing using JS_StrictEq which returns 1 if equal
    int is_equal = JS_StrictEq(ctx, window_obj, global);
    if (is_equal == 1) {
        return;
    }
    
    // Skip internal properties
    const char *prop_name = JS_AtomToCString(ctx, var_name);
    if (!prop_name) {
        return;
    }
    
    // Skip properties that shouldn't be synced
    if (prop_name[0] == '_' || 
        strcmp(prop_name, "window") == 0 ||
        strcmp(prop_name, "globalThis") == 0 ||
        strcmp(prop_name, "self") == 0 ||
        strcmp(prop_name, "top") == 0 ||
        strcmp(prop_name, "parent") == 0 ||
        strcmp(prop_name, "location") == 0 ||
        strcmp(prop_name, "document") == 0 ||
        strcmp(prop_name, "console") == 0) {
        return;
    }
    
    // Check if property already exists on window
    int has_prop = JS_HasProperty(ctx, window_obj, var_name);
    
    // If not on window, copy it from global
    if (!has_prop) {
        GCValue val = JS_GetProperty(ctx, global, var_name);
        if (!JS_IsException(val)) {
            // Skip undefined values - the callback may be called during closure
            // creation before the variable is actually initialized
            if (!JS_IsUndefined(val)) {
                // With GCValue, just set the property directly
                JS_SetProperty(ctx, window_obj, var_name, val);
            }
        }
    }
    
}

// Initialize browser environment
static void init_browser_environment(JSContextHandle ctx) {
    log_to_file("js_quickjs", "init_browser_environment starting...");
    
    log_to_file("js_quickjs", "Getting global object...");
    GCValue global = JS_GetGlobalObject(ctx);
    
    // Register native logging function
    log_to_file("js_quickjs", "Registering __bgmdwnldr_log...");
    GCValue log_func = JS_NewCFunction(ctx, js_bgmdwnldr_log, "__bgmdwnldr_log", 1);
    log_to_file("js_quickjs", "Created C function, setting property...");
    JS_SetPropertyStr(ctx, global, "__bgmdwnldr_log", log_func);
    log_to_file("js_quickjs", "Property set");
    
    // Initialize all browser stubs (DOM, window, document, XMLHttpRequest, etc.)
    // This sets up constructors, prototype chains, and document.body
    log_to_file("js_quickjs", "Calling init_browser_api_impl...");
    init_browser_api_impl(ctx, global);
    log_to_file("js_quickjs", "init_browser_api_impl complete");
    
    log_to_file("js_quickjs", "init_browser_environment complete");
    
    // Note: This QuickJS uses garbage collection, no need to free values explicitly
    (void)global;  // Suppress unused warning
}

// Static initializer for class IDs
// On GCC we use the constructor attribute; on MSVC we use a C++ static initializer.
// NOTE: This is now also called manually during GC reset
static void js_quickjs_do_init_class_ids(void) {
    if (js_xhr_class_id == 0) {
        JS_NewClassID(&js_xhr_class_id);
    }
    if (js_video_class_id == 0) {
        JS_NewClassID(&js_video_class_id);
    }
}

#ifdef __GNUC__
void __attribute__((constructor)) js_quickjs_init_class_ids_ctor(void) {
    js_quickjs_do_init_class_ids();
}
#else
struct JsQuickJSClassIdInit {
    JsQuickJSClassIdInit() {
        js_quickjs_do_init_class_ids();
    }
};
static JsQuickJSClassIdInit g_js_quickjs_class_id_init;
#endif

void js_quickjs_init_class_ids(void) {
    js_quickjs_do_init_class_ids();
}

/* Reset class IDs - called during GC full reset */
extern "C" void js_quickjs_reset_class_ids(void) {
    js_xhr_class_id = 0;
    js_video_class_id = 0;
    /* Reinitialize immediately */
    js_quickjs_init_class_ids();
}

bool js_quickjs_init(void) {
    // Initialize unified GC first - all memory comes from here
    if (!gc_is_initialized()) {
        if (!gc_init()) {
            return false;
        }
    }
    return true;
}

bool js_quickjs_create_runtime(void) {
    if (!gc_is_initialized()) {
        platform_log(LOG_LEVEL_ERROR, "js_quickjs", 
            "GC not initialized, call js_quickjs_init first");
        return false;
    }
    
    // Create runtime using unified GC allocator
    platform_log(LOG_LEVEL_INFO, "js_quickjs", "Creating global runtime...");
    g_js_runtime = JS_NewRuntime();
    if (!g_js_runtime) {
        platform_log(LOG_LEVEL_ERROR, "js_quickjs", "Runtime creation failed");
        return false;
    }
    platform_log(LOG_LEVEL_INFO, "js_quickjs", "Global runtime created: %u", g_js_runtime.handle());
    
    // Allocate per-document CSS index tables (id/class/tag).
    css_document_state_ensure(g_js_runtime);
    
    // Set limits after successful runtime creation
    JS_SetMemoryLimit(g_js_runtime, 256 * 1024 * 1024); // 256MB
    JS_SetMaxStackSize(g_js_runtime, 8 * 1024 * 1024);  // 8MB
    
    // Create context - this initializes built-in objects
    platform_log(LOG_LEVEL_INFO, "js_quickjs", "Creating global context...");
    g_js_context = JS_NewContext(g_js_runtime);
    
    if (!g_js_context) {
        platform_log(LOG_LEVEL_ERROR, "js_quickjs", "Context creation failed");
        return false;
    }
    platform_log(LOG_LEVEL_INFO, "js_quickjs", "Global context created: %u", g_js_context.handle());
    
    // Enable eval() support - REQUIRED for YouTube player scripts to work
    // YouTube's signature decryption relies heavily on eval() for code execution
    platform_log(LOG_LEVEL_INFO, "js_quickjs", "Enabling eval() support...");
    JS_AddIntrinsicEval(g_js_context);
    
    // Enable RegExp support - REQUIRED for YouTube player scripts
    platform_log(LOG_LEVEL_INFO, "js_quickjs", "Enabling RegExp support...");
    JS_AddIntrinsicRegExp(g_js_context);
    
    // Enable Promise support - REQUIRED for modern YouTube scripts
    platform_log(LOG_LEVEL_INFO, "js_quickjs", "Enabling Promise support...");
    JS_AddIntrinsicPromise(g_js_context);
    
    // Enable Map, Set, WeakMap, WeakSet - REQUIRED for Polymer and modern frameworks
    platform_log(LOG_LEVEL_INFO, "js_quickjs", "Enabling Map/Set/WeakMap support...");
    JS_AddIntrinsicMapSet(g_js_context);
    
    // Enable TypedArrays (Uint8Array, etc.) - REQUIRED for some player scripts
    platform_log(LOG_LEVEL_INFO, "js_quickjs", "Enabling TypedArray support...");
    JS_AddIntrinsicTypedArrays(g_js_context);
    
    // Enable JSON support
    platform_log(LOG_LEVEL_INFO, "js_quickjs", "Enabling JSON support...");
    JS_AddIntrinsicJSON(g_js_context);
    
    // Register custom classes
    JSClassDef xhr_def = {.class_name = "XMLHttpRequest", .finalizer = js_xhr_finalizer, .gc_mark = js_xhr_mark};
    JSClassDef video_def = {.class_name = "HTMLVideoElement", .finalizer = js_video_finalizer, .gc_mark = js_video_mark};
    if (JS_NewClass(g_js_runtime, js_xhr_class_id, &xhr_def) < 0) {
        platform_log(LOG_LEVEL_WARN, "js_quickjs", "Failed to register XMLHttpRequest class");
    }
    if (JS_NewClass(g_js_runtime, js_video_class_id, &video_def) < 0) {
        platform_log(LOG_LEVEL_WARN, "js_quickjs", "Failed to register HTMLVideoElement class");
    }
    
    // Initialize full browser environment with all necessary APIs
    platform_log(LOG_LEVEL_INFO, "js_quickjs", "Initializing browser environment...");
    init_browser_environment(g_js_context);
    platform_log(LOG_LEVEL_INFO, "js_quickjs", "Browser environment initialized");
    
    return true;
}

/* Set up initial DOM state - called once during app initialization.
 * Ensures document.body exists so that scripts can append elements.
 * The actual DOM content should be populated from parsed HTML via
 * html_populate_js_document() when HTML is available.
 */
void js_quickjs_setup_initial_dom(void) {
    if (!g_js_context) {
        log_to_file("js_quickjs", "Cannot setup DOM: context not initialized");
        return;
    }
    
    JSContextHandle ctx = g_js_context;
    
    /* Minimal setup: ensure document.body exists */
    const char *ensure_body_js = 
        "if (typeof document !== 'undefined') {\n"
        "  try {\n"
        "    if (!document.body) {\n"
        "      var body = document.createElement('body');\n"
        "      document.body = body;\n"
        "      if (document.documentElement) {\n"
        "        document.documentElement.appendChild(body);\n"
        "      }\n"
        "    }\n"
        "  } catch(e) {\n"
        "    console.log('Error ensuring body: ' + e.message);\n"
        "  }\n"
        "}\n"
    ;
    
    GCValue result = JS_Eval(ctx, ensure_body_js, strlen(ensure_body_js), "<ensure_body>", 0);
    if (JS_IsException(result)) {
        js_quickjs_log_exception(ctx, "js_quickjs", 
            "Ensure body script threw exception: %s", false);
    }
    
    log_to_file("js_quickjs", "Initial DOM setup complete");
}

void js_quickjs_clear_captured_urls(void) {
    pthread_mutex_lock(&g_url_mutex);
    g_captured_url_count = 0;
    memset(g_captured_urls, 0, sizeof(g_captured_urls));
    pthread_mutex_unlock(&g_url_mutex);
}

void js_quickjs_pump_timers_and_jobs(void) {
    if (!g_js_context.valid()) return;
    JSRuntimeHandle rt = JS_GetRuntime(g_js_context);
    int iterations = 0;
    int callbacks_remaining = 1000;  /* Guard against runaway interval loops */
    while (iterations < 100 && callbacks_remaining > 0) {
        int processed = timer_process_due(g_js_context);
        if (processed > callbacks_remaining) processed = callbacks_remaining;
        callbacks_remaining -= processed;
        int jobs = 0;
        JSContextHandle pctx;
        int ret;
        while (callbacks_remaining > 0 &&
               (ret = JS_ExecutePendingJob(rt, &pctx)) > 0) {
            jobs++;
            callbacks_remaining--;
        }
        (void)ret;
        if (processed == 0 && jobs == 0) break;
        iterations++;
    }
}

void js_quickjs_dispatch_lifecycle_events(void) {
    if (!g_js_context.valid()) return;
    const char *lifecycle_js =
        "document.readyState = 'interactive';"
        "var dcl = new Event('DOMContentLoaded', { bubbles: true });"
        "document.dispatchEvent(dcl);"
        "window.dispatchEvent(dcl);"
        "document.readyState = 'complete';"
        "var loadEvt = new Event('load');"
        "window.dispatchEvent(loadEvt);"
        "document.dispatchEvent(loadEvt);";
    GCValue result = JS_Eval(g_js_context, lifecycle_js, strlen(lifecycle_js),
                             "<lifecycle>", JS_EVAL_TYPE_GLOBAL);
    (void)result;
}

void js_quickjs_cleanup(void) {
    pthread_mutex_lock(&g_url_mutex);
    g_captured_url_count = 0;
    /* Clear all captured URLs */
    memset(g_captured_urls, 0, sizeof(g_captured_urls));
    pthread_mutex_unlock(&g_url_mutex);
    
    /* 
     * With unified GC, all memory (including runtime, context, and all JS objects)
     * is allocated from the GC heap. We don't call JS_FreeRuntime/JS_FreeContext
     * because they would try to traverse and free objects individually, which
     * conflicts with the unified GC's memory layout.
     * 
     * Instead, we just reset the global pointers and let gc_cleanup() free
     * the entire heap in one operation.
     */
    // Free CSS index tables before the runtime pointer is reset.
    css_document_state_destroy(g_js_runtime);

    g_js_context = JSContextHandle();
    g_js_runtime = JSRuntimeHandle();
    
    // Cleanup unified GC - this frees the entire heap including all JS objects
    gc_cleanup();
}

/* Execute scripts in the global QuickJS context and extract a string value.
 * This is used for true browser emulation: we load the watch page scripts,
 * let them populate window.ytcfg, then read values like visitorData from
 * the live JavaScript objects — just like a real browser does.
 */
bool js_quickjs_extract_value(const char **scripts, const size_t *script_lens,
                              int script_count, const char *js_expr,
                              char *out_value, size_t out_value_len) {
    if (!g_js_context || !scripts || script_count <= 0 ||
        !js_expr || !out_value || out_value_len == 0) {
        log_to_file("js_quickjs", "extract_value: invalid arguments");
        return false;
    }
    
    JSContextHandle ctx = g_js_context;
    int success_count = 0;
    
    // Execute each script, catching exceptions so one bad script doesn't
    // stop the whole initialization chain (mirrors real browser behavior)
    for (int i = 0; i < script_count; i++) {
        if (!scripts[i] || script_lens[i] == 0) continue;
        
        char filename[64];
        snprintf(filename, sizeof(filename), "<visitor_extract_%d>", i);
        
        GCValue result = JS_Eval(ctx, scripts[i], script_lens[i], filename, JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result)) {
            js_quickjs_log_exception(ctx, "js_quickjs",
                "extract_value: script %d threw exception (non-fatal): %s", false);
        } else {
            success_count++;
        }
    }
    
    log_to_file("js_quickjs", "extract_value: executed %d/%d scripts successfully",
                success_count, script_count);
    
    // Evaluate the expression to extract the desired value
    GCValue expr_result = JS_Eval(ctx, js_expr, strlen(js_expr), "<extract_expr>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(expr_result)) {
        js_quickjs_log_exception(ctx, "js_quickjs",
            "extract_value: expression threw exception: %s", false);
        return false;
    }
    
    // Check for null/undefined
    if (JS_IsNull(expr_result) || JS_IsUndefined(expr_result)) {
        log_to_file("js_quickjs", "extract_value: expression returned null/undefined");
        return false;
    }
    
    // Convert to string
    const char *str = JS_ToCString(ctx, expr_result);
    if (!str || str[0] == '\0') {
        log_to_file("js_quickjs", "extract_value: expression returned empty string");
        return false;
    }
    
    size_t len = strlen(str);
    if (len >= out_value_len) len = out_value_len - 1;
    memcpy(out_value, str, len);
    out_value[len] = '\0';
    
    log_to_file("js_quickjs", "extract_value: extracted value (len=%zu)", len);
    return true;
}

// Helper to extract attribute value from HTML tag
static char* extract_attr(const char *html, const char *tag_end, const char *attr_name) {
    const char *attr = strstr(html, attr_name);
    if (!attr || attr > tag_end) return NULL;
    
    attr += strlen(attr_name);
    while (*attr && isspace((unsigned char)*attr)) attr++;
    if (*attr != '=') return NULL;
    attr++;
    while (*attr && isspace((unsigned char)*attr)) attr++;
    
    char quote = *attr;
    if (quote != '"' && quote != '\'') return NULL;
    
    attr++;
    const char *end = strchr(attr, quote);
    if (!end || end > tag_end) return NULL;
    
    size_t len = end - attr;
    char *value = (char *)malloc(len + 1);
    if (value) {
        strncpy(value, attr, len);
        value[len] = '\0';
    }
    return value;
}

// Create DOM nodes from parsed HTML document
// Using automatic GC memory management
static int create_dom_nodes_from_parsed_html(JSContextHandle ctx, HtmlDocument *doc) {
    if (!ctx || !doc) return 0;
    
    int count = 0;
    
    /* Create elements from body children */
    HtmlNode *body = html_document_body(doc);
    HtmlNode *node = body ? html_node_first_child(doc, body) : NULL;
    
    while (node) {
        if (node->type == HTML_NODE_ELEMENT) {
            /* Create the element */
            GCValue elem = html_create_element_js(ctx, node->tag_name, node->attributes);
            
            if (!JS_IsNull(elem) && !JS_IsException(elem)) {
                /* Check for video elements specifically */
                if (strcasecmp(node->tag_name, "video") == 0) {
                    
                    /* Extract src attribute if present */
                    HtmlAttribute *attr = node->attributes;
                    while (attr) {
                        if (strcasecmp(attr->name, "src") == 0 && attr->value[0]) {
                            GCValue src_val = JS_NewString(ctx, attr->value);
                            JS_SetPropertyStr(ctx, elem, "src", src_val);
                            
                            record_captured_url(attr->value);
                        }
                        if (strcasecmp(attr->name, "id") == 0 && attr->value[0]) {
                            GCValue id_val = JS_NewString(ctx, attr->value);
                            JS_SetPropertyStr(ctx, elem, "id", id_val);
                            
                        }
                        attr = attr->next;
                    }
                    
                    count++;
                }
                
                /* Add to document.body */
                GCValue global = JS_GetGlobalObject(ctx);
                GCValue js_doc = JS_GetPropertyStr(ctx, global, "document");
                
                if (!JS_IsUndefined(js_doc) && !JS_IsNull(js_doc)) {
                    GCValue doc_body = JS_GetPropertyStr(ctx, js_doc, "body");
                    
                    if (!JS_IsUndefined(doc_body) && !JS_IsNull(doc_body)) {
                        GCValue appendChild = JS_GetPropertyStr(ctx, doc_body, "appendChild");
                        
                        if (!JS_IsUndefined(appendChild) && !JS_IsNull(appendChild)) {
                            GCValue args[1] = { elem };
                            GCValue result = JS_Call(ctx, appendChild, doc_body, 1, args);
                            (void)result;
                        }
                    }
                }
            }
        }
        node = html_node_next_sibling(doc, node);
    }
    
    return count;
}

/* Cooperative execution timeout. The QuickJS interpreter checks the handler
 * between bytecode instructions, so pure-JS infinite loops are caught safely.
 * Hangs inside C functions still require those functions to be fixed. */
struct js_exec_timeout_state {
    struct timespec start;
    double limit_seconds;
};

static int js_exec_timeout_handler(JSRuntimeHandle rt, void *opaque) {
    (void)rt;
    struct js_exec_timeout_state *state = (struct js_exec_timeout_state *)opaque;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - state->start.tv_sec) +
                     (now.tv_nsec - state->start.tv_nsec) / 1e9;
    return elapsed > state->limit_seconds ? 1 : 0;
}

bool js_quickjs_exec_scripts(const char **scripts, const size_t *script_lens, 
                             int script_count, const char *html,
                             JsExecResult *out_result) {
    log_to_file("js_quickjs", "js_quickjs_exec_scripts called, script_count=%d", script_count);
    
    if (!scripts || script_count <= 0 || !out_result) {
        log_to_file("js_quickjs", "Invalid arguments");
        return false;
    }
    
    // NOTE: We do NOT reset any state between script executions.
    // In a real browser, multiple <script> tags in the same HTML document
    // share the same global context and state accumulates across executions.
    // This allows later scripts to access variables/objects set up by earlier scripts.
    
    // Clear output
    memset(out_result, 0, sizeof(JsExecResult));
    out_result->status = JS_EXEC_ERROR;
    
    // Use global runtime and context (initialized once in android_main)
    if (!g_js_runtime || !g_js_context) {
        log_to_file("js_quickjs", "Global runtime/context not initialized");
        return false;
    }
    
    JSContextHandle ctx = g_js_context;
    
    log_to_file("js_quickjs", "Using global runtime=%u, context=%u", g_js_runtime.handle(), g_js_context.handle());

    // Parse the full HTML and populate the DOM tree in JS.
    // This replaces the hardcoded minimal skeleton with the actual page structure,
    // allowing scripts to find real elements via getElementById/querySelector.
    HtmlDocument *doc = NULL;
    if (html && strlen(html) > 0) {
        doc = html_parse(html, strlen(html));
        if (doc) {
            GCValue global = JS_GetGlobalObject(ctx);
            GCValue js_doc = JS_GetPropertyStr(ctx, global, "document");
            if (!JS_IsUndefined(js_doc) && !JS_IsNull(js_doc)) {
                if (html_populate_js_document(ctx, js_doc, doc)) {
                    HtmlNode *doc_root = html_document_root(doc);
                    platform_log(LOG_LEVEL_INFO, "js_quickjs",
                        "Full DOM populated from parsed HTML (root=%s, head=%s, body=%s)",
                        doc_root ? doc_root->tag_name : "none",
                        html_document_head(doc) ? "yes" : "no",
                        html_document_body(doc) ? "yes" : "no");

                    /* Parse and apply inline/external CSS before scripts run. */
                    css_apply_document_styles(ctx, js_doc, doc, NULL);
                } else {
                    platform_log(LOG_LEVEL_WARN, "js_quickjs",
                        "Failed to populate DOM from parsed HTML");
                }
            }
        }
    }

    // Note: Data payload scripts (ytInitialPlayerResponse, ytInitialData, etc.)
    // will execute naturally as part of the scripts array, defining global
    // variables just like in a real browser. No manual injection needed.

    // Execute all scripts
    int success_count = 0;
    platform_log(LOG_LEVEL_INFO, "js_quickjs", 
        "[EXEC] Starting execution of %d scripts", script_count);

    for (int i = 0; i < script_count; i++) {
        if (!scripts[i] || script_lens[i] == 0) {
            platform_log(LOG_LEVEL_WARN, "js_quickjs",
                "[EXEC] Script %d is empty or NULL, skipping", i);
            continue;
        }

        char filename[64];
        snprintf(filename, sizeof(filename), "<script_%d>", i);

        log_to_file("js_quickjs", "Executing script %d: %zu bytes", i, script_lens[i]);
        platform_log(LOG_LEVEL_INFO, "js_quickjs",
            "[EXEC] Executing script %d: %zu bytes", i, script_lens[i]);

        // Limit per-script wall-clock time. Large application bundles can enter
        // long-running initialization loops; this prevents them from blocking
        // the whole pipeline. The limit scales with script size.
        struct js_exec_timeout_state timeout_state;
        clock_gettime(CLOCK_MONOTONIC, &timeout_state.start);
        timeout_state.limit_seconds = 10.0 + (script_lens[i] / (1024.0 * 1024.0)) * 4.0;
        if (timeout_state.limit_seconds > 120.0) timeout_state.limit_seconds = 120.0;
        JS_SetInterruptHandler(JS_GetRuntime(ctx), js_exec_timeout_handler, &timeout_state);

        // Execute script directly
        GCValue result = JS_Eval(ctx, scripts[i], script_lens[i], filename, JS_EVAL_TYPE_GLOBAL);

        // Clear the interrupt handler so later operations (pumping timers, GC)
        // are not subject to the per-script deadline.
        JS_SetInterruptHandler(JS_GetRuntime(ctx), NULL, NULL);

        if (JS_IsException(result)) {
            char log_msg[64];
            snprintf(log_msg, sizeof(log_msg), "[EXEC] Script %d threw exception: %%s", i);
            js_quickjs_log_exception(ctx, "js_quickjs", log_msg, true);
        } else {
            success_count++;
            platform_log(LOG_LEVEL_INFO, "js_quickjs",
                "[EXEC] Script %d executed successfully", i);
            
        }
        
        // Drain both timers and pending Promise jobs after each script so that
        // fetch()/XHR .then() chains and player bootstrap callbacks run.
        js_quickjs_pump_timers_and_jobs();

        // Run a GC cycle after every script to prevent handle exhaustion /
        // memory pressure when many large scripts execute in sequence.
        // NOTE: disabled during script execution because the compacting GC can
        // invalidate closure references that capture C functions.
        // JS_RunGC(JS_GetRuntime(ctx));
    }

    // Process all remaining timers and jobs after all scripts complete.
    js_quickjs_pump_timers_and_jobs();
    log_to_file("js_quickjs", "Drained remaining timers/jobs after all scripts");
    
    log_to_file("js_quickjs", "All %d scripts executed, running discovery...", script_count);
    
    // After scripts load, dispatch DOMContentLoaded to trigger player initialization
    // The video element and ytInitialPlayerResponse were already set up before scripts loaded
    const char *init_player_js = 
        "// Use native logging function if available\n"
        "var _log = (typeof __bgmdwnldr_log !== 'undefined') ? __bgmdwnldr_log : console.log;\n"
        "\n"
        "// Debug: Log all window properties that might be player-related\n"
        "var playerKeys = Object.keys(window).filter(function(k) {\n"
        "  return k.toLowerCase().indexOf('player') >= 0 || k.toLowerCase().indexOf('yt') >= 0 || k.toLowerCase().indexOf('decrypt') >= 0;\n"
        "});\n"
        "_log('[JS_DISCOVERY] Player/yt related globals: ' + playerKeys.join(', '));\n"
        "\n"
        "// Check for specific player objects\n"
        "_log('[JS_DISCOVERY] window.player exists: ' + (typeof window.player));\n"
        "_log('[JS_DISCOVERY] window.ytPlayer exists: ' + (typeof window.ytPlayer));\n"
        "_log('[JS_DISCOVERY] window.yt exists: ' + (typeof window.yt));\n"
        "_log('[JS_DISCOVERY] window.ytcfg exists: ' + (typeof window.ytcfg));\n"
        "_log('[JS_DISCOVERY] window.ytsignals exists: ' + (typeof window.ytsignals));\n"
        "\n"
        "// Dispatch DOMContentLoaded to trigger any player initialization\n"
        "if (typeof window !== 'undefined' && window.dispatchEvent) {\n"
        "  var readyEvent = { type: 'DOMContentLoaded', bubbles: true };\n"
        "  window.dispatchEvent(readyEvent);\n"
        "  _log('[JS_DISCOVERY] Dispatched DOMContentLoaded');\n"
        "}\n"
        "\n"
        "// Log what we have available\n"
        "if (typeof ytInitialPlayerResponse !== 'undefined') {\n"
        "  _log('[JS_DISCOVERY] ytInitialPlayerResponse is available');\n"
        "  // Try to extract streamingData to see if URLs are there\n"
        "  if (ytInitialPlayerResponse.streamingData) {\n"
        "    var formats = ytInitialPlayerResponse.streamingData.formats || [];\n"
        "    var adaptiveFormats = ytInitialPlayerResponse.streamingData.adaptiveFormats || [];\n"
        "    _log('[JS_DISCOVERY] Found ' + formats.length + ' formats and ' + adaptiveFormats.length + ' adaptive formats');\n"
        "    // Count encrypted vs decrypted\n"
        "    var encryptedCount = 0;\n"
        "    var decryptedCount = 0;\n"
        "    for (var i = 0; i < formats.length; i++) {\n"
        "      if (formats[i].signatureCipher) encryptedCount++;\n"
        "      else if (formats[i].url) decryptedCount++;\n"
        "    }\n"
        "    for (var i = 0; i < adaptiveFormats.length; i++) {\n"
        "      if (adaptiveFormats[i].signatureCipher) encryptedCount++;\n"
        "      else if (adaptiveFormats[i].url) decryptedCount++;\n"
        "    }\n"
        "    _log('[JS_DISCOVERY] Encrypted: ' + encryptedCount + ', Decrypted: ' + decryptedCount);\n"
        "    // Log first few URLs if available\n"
        "    for (var i = 0; i < Math.min(3, formats.length); i++) {\n"
        "      if (formats[i].url) _log('[JS_DISCOVERY] Format ' + i + ' URL: ' + formats[i].url.substring(0, 80) + '...');\n"
        "      if (formats[i].signatureCipher) _log('[JS_DISCOVERY] Format ' + i + ' signatureCipher: ' + formats[i].signatureCipher.substring(0, 80) + '...');\n"
        "    }\n"
        "  } else {\n"
        "    _log('[JS_DISCOVERY] NO streamingData in ytInitialPlayerResponse');\n"
        "  }\n"
        "} else {\n"
        "  _log('[JS_DISCOVERY] ytInitialPlayerResponse NOT DEFINED');\n"
        "}\n"
        "if (document.getElementById('movie_player')) {\n"
        "  _log('[JS_DISCOVERY] movie_player element exists');\n"
        "}\n"
        "\n"
        "// === DISCOVER PLAYER APIS ===\n"
        "_log('[JS_DISCOVERY] === DISCOVERING PLAYER APIS ===');\n"
        "\n"
        "// Check for yt object\n"
        "if (typeof yt !== 'undefined') {\n"
        "  _log('[JS_DISCOVERY] yt object found');\n"
        "  for (var key in yt) {\n"
        "    _log('[JS_DISCOVERY]   yt.' + key + ' = ' + typeof yt[key]);\n"
        "  }\n"
        "  if (yt.player) {\n"
        "    _log('[JS_DISCOVERY] yt.player found');\n"
        "    for (var key in yt.player) {\n"
        "      _log('[JS_DISCOVERY]   yt.player.' + key + ' = ' + typeof yt.player[key]);\n"
        "    }\n"
        "  }\n"
        "} else {\n"
        "  _log('[JS_DISCOVERY] yt object NOT found');\n"
        "}\n"
        "\n"
        "// Check for player-related globals\n"
        "var playerGlobals = ['player', 'ytPlayer', 'ytplayer', 'Player'];\n"
        "for (var i = 0; i < playerGlobals.length; i++) {\n"
        "  var name = playerGlobals[i];\n"
        "  if (typeof window[name] !== 'undefined') {\n"
        "    _log('[JS_DISCOVERY] window.' + name + ' = ' + typeof window[name]);\n"
        "  }\n"
        "}\n"
        "\n"
        "// === CHECK FOR DECRYPT FUNCTIONS ===\n"
        "_log('[JS_DISCOVERY] === CHECKING FOR DECRYPT FUNCTIONS ===');\n"
        "var potentialDecryptors = [];\n"
        "for (var key in window) {\n"
        "  if (typeof window[key] === 'function' && key.length > 0 && key.length < 15) {\n"
        "    try {\n"
        "      var fnStr = window[key].toString();\n"
        "      // Look for signature manipulation patterns\n"
        "      if ((fnStr.indexOf('split') > -1 || fnStr.indexOf('reverse') > -1 || fnStr.indexOf('slice') > -1) && \n"
        "          fnStr.length < 800 && fnStr.length > 100) {\n"
        "        potentialDecryptors.push(key);\n"
        "        if (potentialDecryptors.length <= 3) {\n"
        "          _log('[JS_DISCOVERY] Potential decryptor ' + key + ': ' + fnStr.substring(0, 60) + '...');\n"
        "        }\n"
        "      }\n"
        "    } catch(e) {}\n"
        "  }\n"
        "}\n"
        "_log('[JS_DISCOVERY] Found ' + potentialDecryptors.length + ' potential decryptor functions');\n"
        "_log('[JS_DISCOVERY] === END DISCOVERY ===');\n"
    ;
    
    GCValue init_result = JS_Eval(ctx, init_player_js, strlen(init_player_js), "<init_player>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(init_result)) {
        js_quickjs_log_exception(ctx, "js_quickjs",
            "Init player script threw exception: %s", false);
    }

    /* Extract video title and thumbnail from ytInitialPlayerResponse */
    {
        GCValue global = JS_GetGlobalObject(ctx);
        GCValue ytip = JS_GetPropertyStr(ctx, global, "ytInitialPlayerResponse");
        if (!JS_IsUndefined(ytip) && !JS_IsNull(ytip)) {
            GCValue vd = JS_GetPropertyStr(ctx, ytip, "videoDetails");
            if (!JS_IsUndefined(vd) && !JS_IsNull(vd)) {
                GCValue titleVal = JS_GetPropertyStr(ctx, vd, "title");
                const char *titleStr = JS_ToCString(ctx, titleVal);
                if (titleStr) {
                    strncpy(out_result->title, titleStr, sizeof(out_result->title) - 1);
                    out_result->title[sizeof(out_result->title) - 1] = '\0';
                }
                GCValue thumb = JS_GetPropertyStr(ctx, vd, "thumbnail");
                if (!JS_IsUndefined(thumb) && !JS_IsNull(thumb)) {
                    GCValue thumbs = JS_GetPropertyStr(ctx, thumb, "thumbnails");
                    if (!JS_IsUndefined(thumbs) && !JS_IsNull(thumbs)) {
                        /* Pick the last (usually highest-res) thumbnail */
                        GCValue lenVal = JS_GetPropertyStr(ctx, thumbs, "length");
                        int thumbCount = 0;
                        if (!JS_IsUndefined(lenVal)) {
                            int64_t len64;
                            if (JS_ToBigInt64(ctx, &len64, lenVal) == 0) {
                                thumbCount = (int)len64;
                            } else {
                                int32_t len32;
                                if (JS_ToInt32(ctx, &len32, lenVal) == 0) {
                                    thumbCount = len32;
                                }
                            }
                        }
                        if (thumbCount > 0) {
                            GCValue lastThumb = JS_GetPropertyUint32(ctx, thumbs, (uint32_t)(thumbCount - 1));
                            if (!JS_IsUndefined(lastThumb) && !JS_IsNull(lastThumb)) {
                                GCValue urlVal = JS_GetPropertyStr(ctx, lastThumb, "url");
                                const char *urlStr = JS_ToCString(ctx, urlVal);
                                if (urlStr) {
                                    strncpy(out_result->thumbnailUrl, urlStr, sizeof(out_result->thumbnailUrl) - 1);
                                    out_result->thumbnailUrl[sizeof(out_result->thumbnailUrl) - 1] = '\0';
                                }
                            }
                        }
                    }
                }
            }
        }
        if (out_result->title[0]) {
            platform_log(LOG_LEVEL_INFO, "js_quickjs",
                "[METADATA] Title: %.80s", out_result->title);
        }
        if (out_result->thumbnailUrl[0]) {
            platform_log(LOG_LEVEL_INFO, "js_quickjs",
                "[METADATA] Thumbnail: %.80s", out_result->thumbnailUrl);
        }
    }

    // Get captured URLs
    pthread_mutex_lock(&g_url_mutex);
    // BUG FIX #5: Safe string copy using memcpy with proper length validation
    out_result->captured_url_count = g_captured_url_count;
    platform_log(LOG_LEVEL_INFO, "js_quickjs", 
        "[URL_CAPTURE_SUMMARY] Total URLs captured: %d", g_captured_url_count);
    for (int i = 0; i < g_captured_url_count && i < JS_MAX_CAPTURED_URLS; i++) {
        size_t len = strlen(g_captured_urls[i]);
        if (len >= JS_MAX_URL_LEN) {
            len = JS_MAX_URL_LEN - 1;
        }
        memcpy(out_result->captured_urls[i], g_captured_urls[i], len);
        out_result->captured_urls[i][len] = '\0';
        platform_log(LOG_LEVEL_INFO, "js_quickjs", 
            "[URL_CAPTURED_%d] %s", i, out_result->captured_urls[i]);
    }
    pthread_mutex_unlock(&g_url_mutex);
    
    if (doc) {
        html_document_free(doc);
    }

    out_result->status = (success_count > 0) ? JS_EXEC_SUCCESS : JS_EXEC_ERROR;
    
    log_to_file("js_quickjs", "Finished, captured %d URLs, status=%d", 
                out_result->captured_url_count, out_result->status);
    
    // NOTE: We do NOT free the context or runtime here.
    // Multiple scripts from the same HTML document share state (window, document, 
    // prototypes, global variables, etc.) and should execute in the same context.
    // 
    // The runtime is cleaned up after each download submission via js_quickjs_cleanup().
    
    log_to_file("js_quickjs", "Execution complete, returning");
    
    return out_result->status == JS_EXEC_SUCCESS;
}

// BUG FIX #4: Added parameter validation before locking mutex
int js_quickjs_get_captured_urls(char urls[][JS_MAX_URL_LEN], int max_urls) {
    if (!urls || max_urls <= 0) {
        return 0;
    }
    
    pthread_mutex_lock(&g_url_mutex);
    int count = 0;
    for (int i = 0; i < g_captured_url_count && i < max_urls && i < MAX_CAPTURED_URLS; i++) {
        strncpy(urls[i], g_captured_urls[i], JS_MAX_URL_LEN - 1);
        urls[i][JS_MAX_URL_LEN - 1] = '\0';
        count++;
    }
    pthread_mutex_unlock(&g_url_mutex);
    return count;
}


/* Android-specific implementations */
#ifdef BE_PLATFORM_ANDROID

/* Execute scripts with Android asset manager (legacy API for backward compatibility) */
bool js_quickjs_exec_scripts_android(const char **scripts, const size_t *script_lens, 
                                     int script_count, const char *html, 
                                     AAssetManager *asset_mgr,
                                     JsExecResult *out_result) {
    /* Store the asset manager for potential use by platform_asset_read */
    if (asset_mgr) {
        js_quickjs_set_asset_manager(asset_mgr);
    }
    
    /* Call the platform-agnostic version */
    return js_quickjs_exec_scripts(scripts, script_lens, script_count, html, out_result);
}

#endif /* BE_PLATFORM_ANDROID */

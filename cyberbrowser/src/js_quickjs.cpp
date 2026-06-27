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
#include "browser_api_impl_internal.h"
#include "html_dom.h"
#include "css_parser.h"
#include "gc_value_helpers.h"
#include "platform.h"
#include "browser_api_impl_types.h"
#include "browser_api_impl_handles.h"
#include "session_state.h"

// Forward declarations from dom_api.cpp
extern "C" void dom_node_set_owner_document(JSContextHandle ctx, GCValue node, GCValue doc);
extern "C" GCValue js_create_document_fragment(JSContextHandle ctx);
extern "C" GCValue js_document_create_element(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
extern "C" GCValue js_element_querySelector_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
extern "C" GCValue js_element_querySelectorAll_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

/* Timer API functions from browser_api_impl.cpp */
extern "C" int timer_process_due(JSContextHandle ctx);
extern "C" int timer_has_pending(void);
extern "C" void timer_api_reset(void);

/* From QuickJS: last property read on undefined/null (diagnostic) */
extern char g_last_undefined_prop[256];

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
static bool is_http_url(const char *url);
static int collect_user_headers(JSContextHandle ctx, GCValue headers_obj,
                                char header_bufs[][512], const char **headers_out,
                                int max_count);

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
extern char g_last_undefined_prop[256];

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
    
    // For "cannot read property of undefined" diagnostics, log the last property name
    if (error && strstr(error, "cannot read property")) {
        platform_log(LOG_LEVEL_WARN, tag, "Last undefined property: '%s'", g_last_undefined_prop);
    }
    
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
    // Standard HTTP(S) handling with CORS enforcement for XHR.
    if (url && is_http_url(url)) {
        const char *method = xhr.method();
        bool is_post = (strcasecmp(method, "POST") == 0);
        char origin[512];
        js_get_document_origin(ctx, origin, sizeof(origin));
        char request_origin[512];
        if (!parse_url_origin(url, request_origin, sizeof(request_origin))) {
            strncpy(request_origin, origin, sizeof(request_origin) - 1);
            request_origin[sizeof(request_origin) - 1] = '\0';
        }
        bool same_origin = is_same_origin(origin, request_origin);
        bool credentials = xhr.with_credentials() != 0;
        bool send_credentials = same_origin || credentials;

        char header_bufs[24][512];
        const char *headers[24];
        int header_count = 0;

        header_count = collect_user_headers(ctx, xhr.headers(), header_bufs, headers, 24);
        bool has_content_type = false;
        bool has_origin = false;
        bool has_referer = false;
        bool has_cookie = false;
        for (int i = 0; i < header_count; i++) {
            if (strncasecmp(headers[i], "Content-Type:", 13) == 0) has_content_type = true;
            if (strncasecmp(headers[i], "Origin:", 7) == 0) has_origin = true;
            if (strncasecmp(headers[i], "Referer:", 8) == 0) has_referer = true;
            if (strncasecmp(headers[i], "Cookie:", 7) == 0) has_cookie = true;
        }
        if (is_post && !has_content_type) {
            if (header_count < 24) headers[header_count++] = "Content-Type: application/json";
        }
        if (header_count < 24) headers[header_count++] = "Accept: */*";

        char origin_header[512];
        char referer_header[1024];
        char cookie_header[2048];
        if (!has_origin) {
            snprintf(origin_header, sizeof(origin_header), "Origin: %s", origin);
            if (header_count < 24) headers[header_count++] = origin_header;
        }
        if (!has_referer) {
            char doc_url[2048];
            doc_url[0] = '\0';
            GCValue global = JS_GetGlobalObject(ctx);
            GCValue location = JS_GetPropertyStr(ctx, global, "location");
            if (JS_IsObject(location)) {
                GCValue href = JS_GetPropertyStr(ctx, location, "href");
                const char *s = JS_IsString(href) ? JS_ToCString(ctx, href) : NULL;
                if (s) {
                    strncpy(doc_url, s, sizeof(doc_url) - 1);
                    doc_url[sizeof(doc_url) - 1] = '\0';
                    JS_FreeCString(ctx, s);
                }
            }
            if (doc_url[0]) {
                snprintf(referer_header, sizeof(referer_header), "Referer: %s", doc_url);
                if (header_count < 24) headers[header_count++] = referer_header;
            }
        }
        if (send_credentials && !has_cookie) {
            const char *cookies = platform_http_get_cookies();
            if (cookies && cookies[0]) {
                snprintf(cookie_header, sizeof(cookie_header), "Cookie: %s", cookies);
                if (header_count < 24) headers[header_count++] = cookie_header;
            }
        }

        if (!same_origin) {
            if (cors_requires_preflight(method, headers, header_count)) {
                char preflight_err[256] = {0};
                bool preflight_ok = cors_perform_preflight(ctx, url, method, headers, header_count,
                                                           origin, credentials,
                                                           preflight_err, sizeof(preflight_err));
                if (!preflight_ok) {
                    platform_log(LOG_LEVEL_WARN, "js_xhr_send", "CORS preflight failed: %s", preflight_err);
                    xhr.set_status(0);
                    xhr.set_response_text("");
                    js_xhr_fire_state(ctx, xhr, this_val, 4);
                    GCValue onerror = JS_GetPropertyStr(ctx, this_val, "onerror");
                    if (JS_IsFunction(ctx, onerror)) JS_Call(ctx, onerror, this_val, 0, NULL);
                    return JS_UNDEFINED;
                }
            }
        }

        PlatformHttpBuffer response_buffer = {0};
        char error_buf[256] = {0};
        int status_code = 0;
        const char *req_body = xhr.request_body();
        size_t req_body_len = strlen(req_body);

        bool success = platform_http_request(url, method,
                                              is_post && req_body_len > 0 ? req_body : NULL,
                                              req_body_len,
                                              headers, header_count,
                                              &response_buffer, &status_code,
                                              error_buf, sizeof(error_buf));

        if (success && response_buffer.data && status_code >= 200 && status_code < 300) {
            if (!same_origin) {
                char cors_err[256] = {0};
                if (!cors_validate_response_headers(response_buffer.headers, origin, credentials,
                                                    cors_err, sizeof(cors_err))) {
                    platform_http_free_buffer(&response_buffer);
                    platform_log(LOG_LEVEL_WARN, "js_xhr_send", "%s", cors_err);
                    xhr.set_status(0);
                    xhr.set_response_text("");
                    js_xhr_fire_state(ctx, xhr, this_val, 4);
                    GCValue onerror = JS_GetPropertyStr(ctx, this_val, "onerror");
                    if (JS_IsFunction(ctx, onerror)) JS_Call(ctx, onerror, this_val, 0, NULL);
                    return JS_UNDEFINED;
                }
            }
            xhr.set_status(status_code);
            xhr.set_response_text(response_buffer.data);
            if (response_buffer.headers && response_buffer.headers_size > 0) {
                char rh[2048];
                size_t copy_len = response_buffer.headers_size;
                if (copy_len >= sizeof(rh)) copy_len = sizeof(rh) - 1;
                memcpy(rh, response_buffer.headers, copy_len);
                rh[copy_len] = '\0';
                xhr.set_response_headers(rh);
            } else {
                char rh[2048];
                snprintf(rh, sizeof(rh), "HTTP/1.1 %d OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n", status_code, response_buffer.size);
                xhr.set_response_headers(rh);
            }
            platform_http_free_buffer(&response_buffer);
            js_xhr_fire_state(ctx, xhr, this_val, 2);
            js_xhr_fire_state(ctx, xhr, this_val, 3);
            js_xhr_fire_state(ctx, xhr, this_val, 4);
            GCValue onload = JS_GetPropertyStr(ctx, this_val, "onload");
            if (JS_IsFunction(ctx, onload)) JS_Call(ctx, onload, this_val, 0, NULL);
            return JS_UNDEFINED;
        } else {
            platform_http_free_buffer(&response_buffer);
            xhr.set_status(0);
            xhr.set_response_text("");
            js_xhr_fire_state(ctx, xhr, this_val, 4);
            GCValue onerror = JS_GetPropertyStr(ctx, this_val, "onerror");
            if (JS_IsFunction(ctx, onerror)) JS_Call(ctx, onerror, this_val, 0, NULL);
            return JS_UNDEFINED;
        }
    }

    // Fallback for non-network URLs
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

static GCValue js_xhr_get_with_credentials(JSContextHandle ctx, GCValue this_val) {
    XMLHttpRequestHandle xhr = XMLHttpRequestHandle::from_object_check(ctx, this_val);
    if (!xhr.valid()) return JS_ThrowTypeError(ctx, "XMLHttpRequest internal error");
    return JS_NewBool(ctx, xhr.with_credentials());
}

static GCValue js_xhr_set_with_credentials(JSContextHandle ctx, GCValue this_val, GCValue val) {
    XMLHttpRequestHandle xhr = XMLHttpRequestHandle::from_object_check(ctx, this_val);
    if (!xhr.valid()) return JS_ThrowTypeError(ctx, "XMLHttpRequest internal error");
    xhr.set_with_credentials(JS_ToBool(ctx, val) ? 1 : 0);
    return JS_UNDEFINED;
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
    JS_CGETSET_DEF("withCredentials", js_xhr_get_with_credentials, js_xhr_set_with_credentials),
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
    JS_MarkValue(rt, vid->onvolumechange, mark_func);
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
    vid.set_onvolumechange(JS_NULL);
    
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
    
    // Trigger standard media load events
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

static void js_video_dispatch_simple_event(JSContextHandle ctx, GCValue this_val, const char *prop_name) {
    GCValue handler = JS_GetPropertyStr(ctx, this_val, prop_name);
    if (JS_IsFunction(ctx, handler)) {
        JS_Call(ctx, handler, this_val, 0, NULL);
    }
}

static GCValue js_video_create_time_ranges(JSContextHandle ctx, double start, double end) {
    GCValue ranges = JS_NewObject(ctx);
    if (end > start) {
        JS_SetPropertyStr(ctx, ranges, "length", JS_NewInt32(ctx, 1));
        JS_SetPropertyStr(ctx, ranges, "__start", JS_NewFloat64(ctx, start));
        JS_SetPropertyStr(ctx, ranges, "__end", JS_NewFloat64(ctx, end));
    } else {
        JS_SetPropertyStr(ctx, ranges, "length", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, ranges, "__start", JS_NewFloat64(ctx, 0));
        JS_SetPropertyStr(ctx, ranges, "__end", JS_NewFloat64(ctx, 0));
    }
    JS_SetPropertyStr(ctx, ranges, "start",
        JS_NewCFunction(ctx, [](JSContextHandle c, GCValue t, int, GCValue*)->GCValue {
            GCValue s = JS_GetPropertyStr(c, t, "__start");
            double v = 0; JS_ToFloat64(c, &v, s); return JS_NewFloat64(c, v);
        }, "start", 1));
    JS_SetPropertyStr(ctx, ranges, "end",
        JS_NewCFunction(ctx, [](JSContextHandle c, GCValue t, int, GCValue*)->GCValue {
            GCValue e = JS_GetPropertyStr(c, t, "__end");
            double v = 0; JS_ToFloat64(c, &v, e); return JS_NewFloat64(c, v);
        }, "end", 1));
    return ranges;
}

static GCValue js_video_create_track_list(JSContextHandle ctx) {
    GCValue list = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, list, "length", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, list, "getTrackById", JS_NewCFunction(ctx, [](JSContextHandle, GCValue, int, GCValue*)->GCValue {
        return JS_NULL;
    }, "getTrackById", 1));
    return list;
}

static GCValue js_video_get_buffered(JSContextHandle ctx, GCValue this_val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    double dur = vid.duration();
    if (dur > 0 && vid.ready_state() >= 1)
        return js_video_create_time_ranges(ctx, 0.0, dur);
    return js_video_create_time_ranges(ctx, 0.0, 0.0);
}

static GCValue js_video_get_played(JSContextHandle ctx, GCValue this_val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    double t = vid.current_time();
    if (t > 0)
        return js_video_create_time_ranges(ctx, 0.0, t);
    return js_video_create_time_ranges(ctx, 0.0, 0.0);
}

static GCValue js_video_get_seekable(JSContextHandle ctx, GCValue this_val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    double dur = vid.duration();
    if (dur > 0)
        return js_video_create_time_ranges(ctx, 0.0, dur);
    return js_video_create_time_ranges(ctx, 0.0, 0.0);
}

static GCValue js_video_get_ended(JSContextHandle ctx, GCValue this_val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    double dur = vid.duration();
    return JS_NewBool(ctx, dur > 0 && vid.current_time() >= dur && vid.paused());
}

#define DEFINE_VIDEO_BOOL_PROP(name, getter_suffix, setter_suffix) \
    static GCValue js_video_get_##name(JSContextHandle ctx, GCValue this_val) { \
        HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val); \
        if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error"); \
        return JS_NewBool(ctx, vid.getter_suffix()); \
    } \
    static GCValue js_video_set_##name(JSContextHandle ctx, GCValue this_val, GCValue val) { \
        HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val); \
        if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error"); \
        vid.setter_suffix(JS_ToBool(ctx, val)); \
        return JS_UNDEFINED; \
    }

DEFINE_VIDEO_BOOL_PROP(autoplay, autoplay, set_autoplay)
DEFINE_VIDEO_BOOL_PROP(loop, loop, set_loop)

static GCValue js_video_get_muted(JSContextHandle ctx, GCValue this_val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    return JS_NewBool(ctx, vid.muted());
}

static GCValue js_video_set_muted(JSContextHandle ctx, GCValue this_val, GCValue val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    vid.set_muted(JS_ToBool(ctx, val));
    js_video_dispatch_simple_event(ctx, this_val, "onvolumechange");
    return JS_UNDEFINED;
}

static GCValue js_video_get_volume(JSContextHandle ctx, GCValue this_val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    return JS_NewFloat64(ctx, vid.volume());
}

static GCValue js_video_set_volume(JSContextHandle ctx, GCValue this_val, GCValue val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    double v = 1.0; JS_ToFloat64(ctx, &v, val);
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    vid.set_volume(v);
    js_video_dispatch_simple_event(ctx, this_val, "onvolumechange");
    return JS_UNDEFINED;
}

static GCValue js_video_get_playback_rate(JSContextHandle ctx, GCValue this_val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    return JS_NewFloat64(ctx, vid.playback_rate());
}

static GCValue js_video_set_playback_rate(JSContextHandle ctx, GCValue this_val, GCValue val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    double v = 1.0; JS_ToFloat64(ctx, &v, val);
    vid.set_playback_rate(v);
    return JS_UNDEFINED;
}

static GCValue js_video_get_default_playback_rate(JSContextHandle ctx, GCValue this_val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    return JS_NewFloat64(ctx, vid.default_playback_rate());
}

static GCValue js_video_set_default_playback_rate(JSContextHandle ctx, GCValue this_val, GCValue val) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");
    double v = 1.0; JS_ToFloat64(ctx, &v, val);
    vid.set_default_playback_rate(v);
    return JS_UNDEFINED;
}

#define DEFINE_VIDEO_STRING_PROP(name, getter_suffix, setter_suffix) \
    static GCValue js_video_get_##name(JSContextHandle ctx, GCValue this_val) { \
        HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val); \
        if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error"); \
        return JS_NewString(ctx, vid.getter_suffix()); \
    } \
    static GCValue js_video_set_##name(JSContextHandle ctx, GCValue this_val, GCValue val) { \
        HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val); \
        if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error"); \
        const char *s = JS_ToCString(ctx, val); \
        if (s) vid.setter_suffix(s); \
        return JS_UNDEFINED; \
    }

DEFINE_VIDEO_STRING_PROP(preload, preload, set_preload)
DEFINE_VIDEO_STRING_PROP(crossOrigin, cross_origin, set_cross_origin)

static GCValue js_video_get_text_tracks(JSContextHandle ctx, GCValue this_val) {
    (void)this_val;
    return js_video_create_track_list(ctx);
}

static GCValue js_video_get_audio_tracks(JSContextHandle ctx, GCValue this_val) {
    (void)this_val;
    return js_video_create_track_list(ctx);
}

static GCValue js_video_get_video_tracks(JSContextHandle ctx, GCValue this_val) {
    (void)this_val;
    return js_video_create_track_list(ctx);
}

static GCValue js_video_activate(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    HTMLVideoElementHandle vid = HTMLVideoElementHandle::from_object_check(ctx, this_val);
    if (!vid.valid()) return JS_ThrowTypeError(ctx, "VideoElement internal error");

    /* activate() is a non-standard player helper; treat it as a load request. */
    js_video_load(ctx, this_val, 0, NULL);

    /* Add a volumechange listener once. We store the flag on the JS object. */
    GCValue listening = JS_GetPropertyStr(ctx, this_val, "__activate_listening");
    if (!JS_ToBool(ctx, listening)) {
        JS_SetPropertyStr(ctx, this_val, "__activate_listening", JS_TRUE);
        GCValue handler = JS_GetPropertyStr(ctx, this_val, "onvolumechange");
        if (JS_IsFunction(ctx, handler)) {
            /* In a real implementation we'd addEventListener; for now we just
             * remember that activation has registered the listener. */
        }
    }
    return JS_UNDEFINED;
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
DEFINE_VIDEO_EVENT_HANDLER(onvolumechange, onvolumechange, set_onvolumechange)

extern "C" const JSCFunctionListEntry js_video_proto_funcs[] = {
    JS_CFUNC_DEF("load", 0, js_video_load),
    JS_CFUNC_DEF("play", 0, js_video_play),
    JS_CFUNC_DEF("pause", 0, js_video_pause),
    JS_CFUNC_DEF("activate", 0, js_video_activate),
    JS_CFUNC_DEF("canPlayType", 1, js_video_can_play_type),
    JS_CGETSET_DEF("textTracks", js_video_get_text_tracks, NULL),
    JS_CGETSET_DEF("audioTracks", js_video_get_audio_tracks, NULL),
    JS_CGETSET_DEF("videoTracks", js_video_get_video_tracks, NULL),
    JS_CGETSET_DEF("id", js_video_get_id, js_video_set_id),
    JS_CGETSET_DEF("src", js_video_get_src, js_video_set_src),
    JS_CGETSET_DEF("currentSrc", js_video_get_src, NULL),
    JS_CGETSET_DEF("currentTime", js_video_get_current_time, js_video_set_current_time),
    JS_CGETSET_DEF("duration", js_video_get_duration, NULL),
    JS_CGETSET_DEF("paused", js_video_get_paused, NULL),
    JS_CGETSET_DEF("readyState", js_video_get_ready_state, NULL),
    JS_CGETSET_DEF("networkState", js_video_get_network_state, NULL),
    JS_CGETSET_DEF("buffered", js_video_get_buffered, NULL),
    JS_CGETSET_DEF("played", js_video_get_played, NULL),
    JS_CGETSET_DEF("seekable", js_video_get_seekable, NULL),
    JS_CGETSET_DEF("ended", js_video_get_ended, NULL),
    JS_CGETSET_DEF("autoplay", js_video_get_autoplay, js_video_set_autoplay),
    JS_CGETSET_DEF("loop", js_video_get_loop, js_video_set_loop),
    JS_CGETSET_DEF("muted", js_video_get_muted, js_video_set_muted),
    JS_CGETSET_DEF("volume", js_video_get_volume, js_video_set_volume),
    JS_CGETSET_DEF("playbackRate", js_video_get_playback_rate, js_video_set_playback_rate),
    JS_CGETSET_DEF("defaultPlaybackRate", js_video_get_default_playback_rate, js_video_set_default_playback_rate),
    JS_CGETSET_DEF("preload", js_video_get_preload, js_video_set_preload),
    JS_CGETSET_DEF("crossOrigin", js_video_get_crossOrigin, js_video_set_crossOrigin),
    JS_CGETSET_DEF("onloadstart", js_video_get_onloadstart, js_video_set_onloadstart),
    JS_CGETSET_DEF("onloadedmetadata", js_video_get_onloadedmetadata, js_video_set_onloadedmetadata),
    JS_CGETSET_DEF("oncanplay", js_video_get_oncanplay, js_video_set_oncanplay),
    JS_CGETSET_DEF("onplay", js_video_get_onplay, js_video_set_onplay),
    JS_CGETSET_DEF("onplaying", js_video_get_onplaying, js_video_set_onplaying),
    JS_CGETSET_DEF("onerror", js_video_get_onerror, js_video_set_onerror),
};
extern "C" const size_t js_video_proto_funcs_count = sizeof(js_video_proto_funcs) / sizeof(js_video_proto_funcs[0]);

// Global fetch implementation

static bool is_http_url(const char *url) {
    if (!url) return false;
    return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0;
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

/* ============================================================================
 * CORS helpers
 * ============================================================================ */

static GCValue reject_type_error(JSContextHandle ctx, const char *msg) {
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue typeerror_ctor = JS_GetPropertyStr(ctx, global, "TypeError");
    GCValue err_msg = JS_NewString(ctx, msg);
    GCValue err = JS_Call(ctx, typeerror_ctor, JS_UNDEFINED, 1, &err_msg);
    GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    GCValue reject_fn = JS_GetPropertyStr(ctx, promise_ctor, "reject");
    GCValue args[1] = { err };
    return JS_Call(ctx, reject_fn, JS_UNDEFINED, 1, args);
}

static bool js_get_document_origin(JSContextHandle ctx, char *out, size_t out_len) {
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue location = JS_GetPropertyStr(ctx, global, "location");
    out[0] = '\0';
    if (JS_IsObject(location)) {
        GCValue proto_val = JS_GetPropertyStr(ctx, location, "protocol");
        GCValue host_val = JS_GetPropertyStr(ctx, location, "host");
        const char *proto = JS_IsString(proto_val) ? JS_ToCString(ctx, proto_val) : NULL;
        const char *host = JS_IsString(host_val) ? JS_ToCString(ctx, host_val) : NULL;
        if (proto && host) {
            snprintf(out, out_len, "%s//%s", proto, host);
        }
        if (proto) JS_FreeCString(ctx, proto);
        if (host) JS_FreeCString(ctx, host);
    }
    if (out[0] == '\0') {
        strncpy(out, "https://localhost", out_len - 1);
        out[out_len - 1] = '\0';
    }
    return true;
}

static bool parse_url_origin(const char *url, char *out, size_t out_len) {
    if (!url) return false;
    const char *p = url;
    const char *proto_end = strstr(p, "://");
    if (!proto_end) return false;
    char protocol[16];
    size_t proto_len = (size_t)(proto_end - p);
    if (proto_len >= sizeof(protocol)) return false;
    memcpy(protocol, p, proto_len);
    protocol[proto_len] = '\0';

    p = proto_end + 3;
    const char *end = p + strlen(p);
    const char *slash = strchr(p, '/');
    const char *query = strchr(p, '?');
    const char *hash = strchr(p, '#');
    if (slash && slash < end) end = slash;
    if (query && query < end) end = query;
    if (hash && hash < end) end = hash;

    size_t host_len = (size_t)(end - p);
    snprintf(out, out_len, "%s://%.*s", protocol, (int)host_len, p);
    return true;
}

static bool is_same_origin(const char *a, const char *b) {
    return strcasecmp(a, b) == 0;
}

static bool is_cors_safe_method(const char *m) {
    return strcasecmp(m, "GET") == 0 ||
           strcasecmp(m, "HEAD") == 0 ||
           strcasecmp(m, "POST") == 0;
}

static bool is_cors_simple_header_name(const char *name) {
    static const char *simple[] = {
        "Accept", "Accept-Language", "Content-Language", "Content-Type",
        "DPR", "Downlink", "Save-Data", "Viewport-Width", "Width", NULL
    };
    for (int i = 0; simple[i]; i++) {
        if (strcasecmp(name, simple[i]) == 0) return true;
    }
    return false;
}

static bool is_cors_simple_content_type(const char *val) {
    while (*val == ' ' || *val == '\t') val++;
    return strcasecmp(val, "application/x-www-form-urlencoded") == 0 ||
           strcasecmp(val, "multipart/form-data") == 0 ||
           strcasecmp(val, "text/plain") == 0;
}

static void parse_header_name_value(const char *header, char *name, size_t name_len,
                                    char *value, size_t value_len) {
    name[0] = '\0';
    value[0] = '\0';
    const char *colon = strchr(header, ':');
    if (!colon) {
        strncpy(name, header, name_len - 1);
        name[name_len - 1] = '\0';
        return;
    }
    size_t kl = (size_t)(colon - header);
    while (kl > 0 && (header[kl - 1] == ' ' || header[kl - 1] == '\t')) kl--;
    if (kl >= name_len) kl = name_len - 1;
    memcpy(name, header, kl);
    name[kl] = '\0';

    const char *v = colon + 1;
    while (*v == ' ' || *v == '\t') v++;
    strncpy(value, v, value_len - 1);
    value[value_len - 1] = '\0';
}

static bool cors_requires_preflight(const char *method, const char **headers, int header_count) {
    if (!is_cors_safe_method(method)) return true;
    for (int i = 0; i < header_count; i++) {
        char name[128], value[512];
        parse_header_name_value(headers[i], name, sizeof(name), value, sizeof(value));
        if (strcasecmp(name, "Origin") == 0 ||
            strcasecmp(name, "Referer") == 0 ||
            strcasecmp(name, "Cookie") == 0) {
            continue;
        }
        if (!is_cors_simple_header_name(name)) return true;
        if (strcasecmp(name, "Content-Type") == 0 && !is_cors_simple_content_type(value)) {
            return true;
        }
    }
    return false;
}

static bool cors_find_header(const char *headers, const char *name,
                             char *out, size_t out_len) {
    size_t nlen = strlen(name);
    const char *p = headers ? headers : "";
    while (*p) {
        const char *line_end = strchr(p, '\n');
        if (!line_end) line_end = p + strlen(p);
        const char *colon = (const char*)memchr(p, ':', (size_t)(line_end - p));
        if (colon) {
            size_t key_len = (size_t)(colon - p);
            while (key_len > 0 && (p[key_len - 1] == ' ' || p[key_len - 1] == '\t')) key_len--;
            if (key_len == nlen && strncasecmp(p, name, nlen) == 0) {
                const char *v = colon + 1;
                while (v < line_end && (*v == ' ' || *v == '\t')) v++;
                size_t vl = (size_t)(line_end - v);
                while (vl > 0 && (v[vl - 1] == '\r' || v[vl - 1] == '\n' ||
                                  v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
                if (vl >= out_len) vl = out_len - 1;
                memcpy(out, v, vl);
                out[vl] = '\0';
                return true;
            }
        }
        p = line_end;
        if (*p == '\n') p++;
    }
    return false;
}

static bool cors_list_contains(const char *list, const char *token) {
    size_t tlen = strlen(token);
    const char *p = list ? list : "";
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        const char *end = p;
        while (*end && *end != ',') end++;
        size_t len = (size_t)(end - p);
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
        if (len == tlen && strncasecmp(p, token, tlen) == 0) return true;
        p = end;
        if (*p == ',') p++;
    }
    return false;
}

static bool cors_validate_response_headers(const char *headers, const char *origin,
                                           bool credentials, char *err, size_t err_len) {
    char acao[512];
    if (!cors_find_header(headers, "Access-Control-Allow-Origin", acao, sizeof(acao))) {
        snprintf(err, err_len, "CORS blocked: missing Access-Control-Allow-Origin");
        return false;
    }
    if (strcmp(acao, "*") == 0) {
        if (credentials) {
            snprintf(err, err_len, "CORS blocked: wildcard ACAO with credentials");
            return false;
        }
    } else if (strcasecmp(acao, origin) != 0) {
        snprintf(err, err_len, "CORS blocked: Access-Control-Allow-Origin mismatch");
        return false;
    }
    if (credentials) {
        char acac[64];
        if (!cors_find_header(headers, "Access-Control-Allow-Credentials", acac, sizeof(acac)) ||
            strcasecmp(acac, "true") != 0) {
            snprintf(err, err_len, "CORS blocked: Access-Control-Allow-Credentials missing");
            return false;
        }
    }
    return true;
}

static bool cors_perform_preflight(JSContextHandle ctx, const char *url, const char *method,
                                   const char **headers, int header_count,
                                   const char *origin, bool credentials,
                                   char *err, size_t err_len) {
    char hbuf[24][512];
    const char *ph[24];
    int phc = 0;

    char origin_h[512];
    snprintf(origin_h, sizeof(origin_h), "Origin: %s", origin);
    hbuf[phc][0] = '\0';
    strncpy(hbuf[phc], origin_h, sizeof(hbuf[phc]) - 1);
    ph[phc] = hbuf[phc];
    phc++;

    char acrm[128];
    snprintf(acrm, sizeof(acrm), "Access-Control-Request-Method: %s", method);
    hbuf[phc][0] = '\0';
    strncpy(hbuf[phc], acrm, sizeof(hbuf[phc]) - 1);
    ph[phc] = hbuf[phc];
    phc++;

    char acrh_value[2048];
    acrh_value[0] = '\0';
    bool first_acrh = true;
    for (int i = 0; i < header_count; i++) {
        char name[128], value[512];
        parse_header_name_value(headers[i], name, sizeof(name), value, sizeof(value));
        if (strcasecmp(name, "Origin") == 0 ||
            strcasecmp(name, "Referer") == 0 ||
            strcasecmp(name, "Cookie") == 0 ||
            strcasecmp(name, "Accept") == 0) {
            continue;
        }
        bool needs_listing = false;
        if (!is_cors_simple_header_name(name)) {
            needs_listing = true;
        } else if (strcasecmp(name, "Content-Type") == 0 && !is_cors_simple_content_type(value)) {
            needs_listing = true;
        }
        if (needs_listing) {
            if (!first_acrh) strncat(acrh_value, ", ", sizeof(acrh_value) - strlen(acrh_value) - 1);
            strncat(acrh_value, name, sizeof(acrh_value) - strlen(acrh_value) - 1);
            first_acrh = false;
        }
    }
    char acrh_header[2080];
    if (acrh_value[0]) {
        snprintf(acrh_header, sizeof(acrh_header), "Access-Control-Request-Headers: %s", acrh_value);
        hbuf[phc][0] = '\0';
        strncpy(hbuf[phc], acrh_header, sizeof(hbuf[phc]) - 1);
        ph[phc] = hbuf[phc];
        phc++;
    }

    PlatformHttpBuffer resp = {0};
    int status = 0;
    char ebuf[256] = {0};
    bool ok = platform_http_request(url, "OPTIONS", NULL, 0, ph, phc, &resp, &status, ebuf, sizeof(ebuf));
    if (!ok || status < 200 || status >= 300) {
        snprintf(err, err_len, "CORS preflight failed: %s", ebuf[0] ? ebuf : "network error");
        platform_http_free_buffer(&resp);
        return false;
    }

    if (!cors_validate_response_headers(resp.headers, origin, credentials, err, err_len)) {
        platform_http_free_buffer(&resp);
        return false;
    }

    char acam[512];
    if (!cors_find_header(resp.headers, "Access-Control-Allow-Methods", acam, sizeof(acam)) ||
        !cors_list_contains(acam, method)) {
        snprintf(err, err_len, "CORS preflight blocked: method %s not allowed", method);
        platform_http_free_buffer(&resp);
        return false;
    }

    if (acrh_value[0]) {
        char acah[1024];
        if (!cors_find_header(resp.headers, "Access-Control-Allow-Headers", acah, sizeof(acah))) {
            snprintf(err, err_len, "CORS preflight blocked: missing Access-Control-Allow-Headers");
            platform_http_free_buffer(&resp);
            return false;
        }
        const char *p = acrh_value;
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            if (!*p) break;
            const char *end = p;
            while (*end && *end != ',') end++;
            size_t len = (size_t)(end - p);
            while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
            if (len > 0) {
                char name[128];
                if (len >= sizeof(name)) len = sizeof(name) - 1;
                memcpy(name, p, len);
                name[len] = '\0';
                if (!cors_list_contains(acah, name)) {
                    snprintf(err, err_len, "CORS preflight blocked: header %s not allowed", name);
                    platform_http_free_buffer(&resp);
                    return false;
                }
            }
            p = end;
            if (*p == ',') p++;
        }
    }

    platform_http_free_buffer(&resp);
    return true;
}

static GCValue create_http_response(JSContextHandle ctx, const char *url,
                                    const PlatformHttpBuffer *buf, int status,
                                    const char *statusText) {
    GCValue response = JS_NewObject(ctx);
    bool ok = (status >= 200 && status < 300);
    JS_SetPropertyStr(ctx, response, "ok", JS_NewBool(ctx, ok));
    JS_SetPropertyStr(ctx, response, "status", JS_NewInt32(ctx, status));
    JS_SetPropertyStr(ctx, response, "statusText", JS_NewString(ctx, statusText ? statusText : (ok ? "OK" : "Error")));
    JS_SetPropertyStr(ctx, response, "body", JS_NULL);
    JS_SetPropertyStr(ctx, response, "redirected", JS_FALSE);
    JS_SetPropertyStr(ctx, response, "type", JS_NewString(ctx, "basic"));
    JS_SetPropertyStr(ctx, response, "url", JS_NewString(ctx, url ? url : ""));

    GCValue headers_obj = JS_NewObject(ctx);
    if (buf && buf->headers && buf->headers_size > 0) {
        const char *p = buf->headers;
        while (*p) {
            const char *line_end = strchr(p, '\n');
            if (!line_end) line_end = p + strlen(p);
            const char *colon = (const char*)memchr(p, ':', (size_t)(line_end - p));
            if (colon) {
                size_t kl = (size_t)(colon - p);
                while (kl > 0 && (p[kl - 1] == ' ' || p[kl - 1] == '\t')) kl--;
                if (kl > 0 && kl < 128) {
                    char name[128];
                    memcpy(name, p, kl);
                    name[kl] = '\0';
                    const char *v = colon + 1;
                    while (v < line_end && (*v == ' ' || *v == '\t')) v++;
                    size_t vl = (size_t)(line_end - v);
                    while (vl > 0 && (v[vl - 1] == '\r' || v[vl - 1] == '\n' ||
                                      v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
                    if (vl < 2048) {
                        char value[2048];
                        memcpy(value, v, vl);
                        value[vl] = '\0';
                        JS_SetPropertyStr(ctx, headers_obj, name, JS_NewString(ctx, value));
                    }
                }
            }
            p = line_end;
            if (*p == '\n') p++;
        }
    }
    JS_SetPropertyStr(ctx, response, "headers", headers_obj);

    if (buf && buf->data) {
        JS_SetPropertyStr(ctx, response, "__body_text", JS_NewStringLen(ctx, buf->data, buf->size));
        GCValue json_val = JS_ParseJSON(ctx, buf->data, buf->size, "<fetch-response>");
        if (!JS_IsException(json_val)) {
            JS_SetPropertyStr(ctx, response, "__body_json", json_val);
        }
    }

    JS_SetPropertyStr(ctx, response, "text", JS_NewCFunction(ctx, [](JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) -> GCValue {
        (void)argc; (void)argv;
        GCValue global = JS_GetGlobalObject(ctx);
        GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
        GCValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
        GCValue body_text = JS_GetPropertyStr(ctx, this_val, "__body_text");
        GCValue args[1] = { body_text };
        return JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, args);
    }, "text", 0));

    JS_SetPropertyStr(ctx, response, "json", JS_NewCFunction(ctx, [](JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) -> GCValue {
        (void)argc; (void)argv;
        GCValue global = JS_GetGlobalObject(ctx);
        GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
        GCValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
        GCValue body_json = JS_GetPropertyStr(ctx, this_val, "__body_json");
        if (JS_IsUndefined(body_json)) body_json = JS_NewObject(ctx);
        GCValue args[1] = { body_json };
        return JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, args);
    }, "json", 0));

    JS_SetPropertyStr(ctx, response, "arrayBuffer", JS_NewCFunction(ctx, [](JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) -> GCValue {
        (void)argc; (void)argv;
        GCValue global = JS_GetGlobalObject(ctx);
        GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
        GCValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
        GCValue ab = JS_NewArrayBuffer(ctx, NULL, 0, NULL, NULL, false);
        GCValue args[1] = { ab };
        return JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, args);
    }, "arrayBuffer", 0));

    JS_SetPropertyStr(ctx, response, "blob", JS_NewCFunction(ctx, [](JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) -> GCValue {
        (void)argc; (void)argv;
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
    
    // CORS mode and credentials from init/Request
    char mode_buf[32] = "cors";
    char credentials_buf[32] = "same-origin";
    auto copy_opt = [&](const char *prop, char *buf, size_t buf_len, const char *fallback) {
        GCValue val = JS_UNDEFINED;
        if (argc > 1 && JS_IsObject(argv[1])) val = JS_GetPropertyStr(ctx, argv[1], prop);
        if ((JS_IsUndefined(val) || JS_IsNull(val)) && argc > 0 && JS_IsObject(argv[0])) {
            val = JS_GetPropertyStr(ctx, argv[0], prop);
        }
        const char *s = JS_IsString(val) ? JS_ToCString(ctx, val) : NULL;
        if (s && s[0]) {
            strncpy(buf, s, buf_len - 1);
            buf[buf_len - 1] = '\0';
            JS_FreeCString(ctx, s);
        } else {
            strncpy(buf, fallback, buf_len - 1);
            buf[buf_len - 1] = '\0';
        }
    };
    copy_opt("mode", mode_buf, sizeof(mode_buf), "cors");
    copy_opt("credentials", credentials_buf, sizeof(credentials_buf), "same-origin");

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
    
    // Priority 1: init.body
    if (!post_body && argc > 1 && JS_IsObject(argv[1])) {
        GCValue body_prop = JS_GetPropertyStr(ctx, argv[1], "body");
        post_body = extract_body(body_prop);
        if (post_body) post_body_len = strlen(post_body);
    }
    
    // Priority 2: Request.body
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
    
    // Standard HTTP(S) handling with CORS enforcement.
    if (url && is_http_url(url)) {
        bool is_post = (strcasecmp(method, "POST") == 0);
        char origin[512];
        js_get_document_origin(ctx, origin, sizeof(origin));
        char request_origin[512];
        if (!parse_url_origin(url, request_origin, sizeof(request_origin))) {
            strncpy(request_origin, origin, sizeof(request_origin) - 1);
            request_origin[sizeof(request_origin) - 1] = '\0';
        }
        bool same_origin = is_same_origin(origin, request_origin);
        bool cors_no_cors = (strcasecmp(mode_buf, "no-cors") == 0);
        bool cors_same_origin_mode = (strcasecmp(mode_buf, "same-origin") == 0);
        bool credentials_include = (strcasecmp(credentials_buf, "include") == 0);
        bool send_credentials = same_origin || credentials_include;

        if (cors_same_origin_mode && !same_origin) {
            if (post_body) free(post_body);
            return reject_type_error(ctx, "Failed to fetch: same-origin mode requests must be same-origin");
        }

        char header_bufs[24][512];
        const char *headers[24];
        int header_count = 0;

        if (argc > 1 && JS_IsObject(argv[1])) {
            GCValue headers_obj = JS_GetPropertyStr(ctx, argv[1], "headers");
            header_count = collect_user_headers(ctx, headers_obj, header_bufs, headers, 24);
        }

        bool has_content_type = false;
        bool has_origin = false;
        bool has_referer = false;
        bool has_cookie = false;
        for (int i = 0; i < header_count; i++) {
            if (strncasecmp(headers[i], "Content-Type:", 13) == 0) has_content_type = true;
            if (strncasecmp(headers[i], "Origin:", 7) == 0) has_origin = true;
            if (strncasecmp(headers[i], "Referer:", 8) == 0) has_referer = true;
            if (strncasecmp(headers[i], "Cookie:", 7) == 0) has_cookie = true;
        }
        if (is_post && !has_content_type) {
            if (header_count < 24) headers[header_count++] = "Content-Type: application/json";
        }
        if (header_count < 24) headers[header_count++] = "Accept: */*";

        char origin_header[512];
        char referer_header[1024];
        char cookie_header[2048];
        if (!has_origin) {
            snprintf(origin_header, sizeof(origin_header), "Origin: %s", origin);
            if (header_count < 24) headers[header_count++] = origin_header;
        }
        if (!has_referer) {
            char doc_url[2048];
            doc_url[0] = '\0';
            GCValue global = JS_GetGlobalObject(ctx);
            GCValue location = JS_GetPropertyStr(ctx, global, "location");
            if (JS_IsObject(location)) {
                GCValue href = JS_GetPropertyStr(ctx, location, "href");
                const char *s = JS_IsString(href) ? JS_ToCString(ctx, href) : NULL;
                if (s) {
                    strncpy(doc_url, s, sizeof(doc_url) - 1);
                    doc_url[sizeof(doc_url) - 1] = '\0';
                    JS_FreeCString(ctx, s);
                }
            }
            if (doc_url[0]) {
                snprintf(referer_header, sizeof(referer_header), "Referer: %s", doc_url);
                if (header_count < 24) headers[header_count++] = referer_header;
            }
        }
        if (send_credentials && !has_cookie) {
            const char *cookies = platform_http_get_cookies();
            if (cookies && cookies[0]) {
                snprintf(cookie_header, sizeof(cookie_header), "Cookie: %s", cookies);
                if (header_count < 24) headers[header_count++] = cookie_header;
            }
        }

        // CORS preflight for cross-origin non-simple requests.
        if (!same_origin && !cors_no_cors) {
            if (cors_requires_preflight(method, headers, header_count)) {
                char preflight_err[256] = {0};
                bool preflight_ok = cors_perform_preflight(ctx, url, method, headers, header_count,
                                                           origin, credentials_include,
                                                           preflight_err, sizeof(preflight_err));
                if (!preflight_ok) {
                    if (post_body) free(post_body);
                    platform_log(LOG_LEVEL_WARN, "js_fetch", "CORS preflight failed: %s", preflight_err);
                    return reject_type_error(ctx, preflight_err);
                }
            }
        }

        PlatformHttpBuffer response_buffer = {0};
        char error_buf[256] = {0};
        int status_code = 0;
        bool success = platform_http_request(url, method,
                                              post_body && post_body_len > 0 ? post_body : NULL,
                                              post_body_len,
                                              headers, header_count,
                                              &response_buffer, &status_code,
                                              error_buf, sizeof(error_buf));

        if (post_body) free(post_body);
        post_body = NULL;

        if (success && response_buffer.data && status_code >= 200 && status_code < 300) {
            if (!same_origin && !cors_no_cors) {
                char cors_err[256] = {0};
                if (!cors_validate_response_headers(response_buffer.headers, origin, credentials_include,
                                                    cors_err, sizeof(cors_err))) {
                    platform_http_free_buffer(&response_buffer);
                    platform_log(LOG_LEVEL_WARN, "js_fetch", "%s", cors_err);
                    return reject_type_error(ctx, cors_err);
                }
            }
            GCValue response_obj = create_http_response(ctx, url, &response_buffer, status_code, "OK");
            platform_http_free_buffer(&response_buffer);
            GCValue global = JS_GetGlobalObject(ctx);
            GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
            GCValue resolve_fn = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
            GCValue args[1] = { response_obj };
            return JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, args);
        } else {
            platform_http_free_buffer(&response_buffer);
            platform_log(LOG_LEVEL_WARN, "js_fetch", "Generic request failed: %s", error_buf[0] ? error_buf : "unknown");
            return reject_type_error(ctx, error_buf[0] ? error_buf : "Network request failed");
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
    
    static int create_counter = 0;
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
            int cid = ++create_counter;
            JS_SetPropertyStr(ctx, elem, "__cyber_id", JS_NewInt32(ctx, cid));
            if (strcasecmp(tag, "ytd-masthead") == 0 || strcasecmp(tag, "ytd-app") == 0) {
                fprintf(stderr, "[CREATE-ELEM] id=%d tag=%s\n", cid, tag);
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
            
            // Template elements need a real DocumentFragment content so Polymer
            // can parse, query, and clone templates.
            if (strcasecmp(tag, "template") == 0) {
                GCValue content = js_create_document_fragment(ctx);
                if (!JS_IsException(content) && !JS_IsUndefined(content) && !JS_IsNull(content)) {
                    dom_node_set_owner_document(ctx, content, this_val);
                    JS_SetPropertyStr(ctx, elem, "content", content);
                }

                // Use HTMLTemplateElement.prototype if available.
                GCValue global_obj = JS_GetGlobalObject(ctx);
                GCValue template_ctor = JS_GetPropertyStr(ctx, global_obj, "HTMLTemplateElement");
                if (!JS_IsUndefined(template_ctor) && !JS_IsException(template_ctor)) {
                    GCValue template_proto = JS_GetPropertyStr(ctx, template_ctor, "prototype");
                    if (!JS_IsUndefined(template_proto) && !JS_IsException(template_proto)) {
                        JS_SetPrototype(ctx, elem, template_proto);
                    }
                }
            }
        }
    }
    
    // Set ownerDocument on newly created elements.
    if (!JS_IsNull(elem) && !JS_IsUndefined(elem)) {
        dom_node_set_owner_document(ctx, elem, this_val);

        // If a custom element definition already exists, upgrade the newly
        // created element synchronously before returning it (CEReactions).
        // We only do this when a definition is registered, so parser-created
        // nodes that arrive before definitions are not upgraded individually.
        if (tag && strchr(tag, '-')) {
            GCValue global_obj = JS_GetGlobalObject(ctx);
            GCValue customElements = JS_GetPropertyStr(ctx, global_obj, "customElements");
            if (JS_IsObject(customElements)) {
                GCValue get_fn = JS_GetPropertyStr(ctx, customElements, "get");
                if (JS_IsFunction(ctx, get_fn)) {
                    GCValue tag_val = JS_NewString(ctx, tag);
                    GCValue ctor = JS_Call(ctx, get_fn, customElements, 1, &tag_val);
                    if (!JS_IsException(ctor) && !JS_IsNull(ctor) && !JS_IsUndefined(ctor) && JS_IsFunction(ctx, ctor)) {
                        js_cyber_ce_enqueue_upgrade(ctx, JS_UNDEFINED, 1, &elem);
                        GCValue flushing = JS_GetPropertyStr(ctx, global_obj, "__cyber_ce_flushing");
                        if (!JS_ToBool(ctx, flushing)) {
                            js_cyber_ce_flush_reactions(ctx, JS_UNDEFINED, 0, NULL);
                        }
                    }
                    if (JS_IsException(ctor)) JS_GetException(ctx);
                }
            }
        }
    }
    
    return elem;
}

static GCValue js_document_get_element_by_id(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id || !*id) return JS_NULL;

    GCValue doc_elem = JS_GetPropertyStr(ctx, this_val, "documentElement");
    if (JS_IsUndefined(doc_elem) || JS_IsNull(doc_elem)) return JS_NULL;

    // Build a simple #id selector.
    char selector[256];
    snprintf(selector, sizeof(selector), "#%s", id);
    GCValue args[1] = { JS_NewString(ctx, selector) };
    return js_element_querySelector_real(ctx, doc_elem, 1, args);
}

static GCValue js_document_query_selector(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue doc_elem = JS_GetPropertyStr(ctx, this_val, "documentElement");
    if (JS_IsUndefined(doc_elem) || JS_IsNull(doc_elem)) return JS_NULL;
    return js_element_querySelector_real(ctx, doc_elem, argc, argv);
}

static GCValue js_document_query_selector_all(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue doc_elem = JS_GetPropertyStr(ctx, this_val, "documentElement");
    if (JS_IsUndefined(doc_elem) || JS_IsNull(doc_elem)) return JS_NewArray(ctx);
    return js_element_querySelectorAll_real(ctx, doc_elem, argc, argv);
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
    
    // Enable eval() support - required by many player scripts
    platform_log(LOG_LEVEL_INFO, "js_quickjs", "Enabling eval() support...");
    JS_AddIntrinsicEval(g_js_context);
    
    // Enable RegExp support - required by most page scripts
    platform_log(LOG_LEVEL_INFO, "js_quickjs", "Enabling RegExp support...");
    JS_AddIntrinsicRegExp(g_js_context);
    
    // Enable Promise support - required by modern page scripts
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

    // Enable Proxy support - required by modern frameworks and by spec-correct
    // lazy dependency-injection fallbacks without source-code patches.
    platform_log(LOG_LEVEL_INFO, "js_quickjs", "Enabling Proxy support...");
    JS_AddIntrinsicProxy(g_js_context);
    
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
                    fprintf(stderr, "[js_quickjs] Applying document styles...\n");
                    fflush(stderr);
                    css_apply_document_styles(ctx, js_doc, doc, NULL);
                    fprintf(stderr, "[js_quickjs] Document styles applied.\n");
                    fflush(stderr);
                } else {
                    platform_log(LOG_LEVEL_WARN, "js_quickjs",
                        "Failed to populate DOM from parsed HTML");
                }
            }
        }
    }

    // Data payload scripts will execute naturally as part of the scripts array,
    // defining global variables just like in a real browser. No manual injection
    // needed.

    /* Defensive DOM normalization: ensure the root elements have style objects.
     * Some web-animations polyfills do 'prop in document.documentElement.style'
     * and crash if style is undefined. */
    {
        const char *guard = "(function(){"
            "var de=document.documentElement;"
            "if(de&&(!de.style||typeof de.style!='object')) de.style={};"
            "var b=document.body;"
            "if(b&&(!b.style||typeof b.style!='object')) b.style={};"
            "var h=document.head;"
            "if(h&&(!h.style||typeof h.style!='object')) h.style={};"
            "})();";
        GCValue guard_val = JS_Eval(ctx, guard, strlen(guard), "<dom_normalize>", JS_EVAL_TYPE_GLOBAL);
        (void)guard_val; /* GC-managed, no explicit free */
    }

    // Execute all scripts
    int success_count = 0;
    const size_t MAX_EXEC_SCRIPT_SIZE = 64 * 1024 * 1024; /* 64 MiB safety limit */
    platform_log(LOG_LEVEL_INFO, "js_quickjs",
        "[EXEC] Starting execution of %d scripts", script_count);
    fprintf(stderr, "[js_quickjs] START exec loop script_count=%d\n", script_count);
    fflush(stderr);

    for (int i = 0; i < script_count; i++) {
        if (!scripts[i] || script_lens[i] == 0) {
            platform_log(LOG_LEVEL_WARN, "js_quickjs",
                "[EXEC] Script %d is empty or NULL, skipping", i);
            continue;
        }

        if (script_lens[i] > MAX_EXEC_SCRIPT_SIZE) {
            platform_log(LOG_LEVEL_WARN, "js_quickjs",
                "[EXEC] Script %d is %zu bytes, exceeds %zu byte safety limit, skipping",
                i, script_lens[i], MAX_EXEC_SCRIPT_SIZE);
            fprintf(stderr, "[js_quickjs] SKIP script %d size=%zu > limit=%zu\n",
                    i, script_lens[i], MAX_EXEC_SCRIPT_SIZE);
            fflush(stderr);
            continue;
        }

        char filename[64];
        snprintf(filename, sizeof(filename), "<script_%d>", i);

        log_to_file("js_quickjs", "Executing script %d: %zu bytes", i, script_lens[i]);
        platform_log(LOG_LEVEL_INFO, "js_quickjs",
            "[EXEC] Executing script %d: %zu bytes", i, script_lens[i]);
        fprintf(stderr, "[js_quickjs] >>> EVAL script %d/%d size=%zu\n",
                i, script_count, script_lens[i]);
        fflush(stderr);

        // Limit per-script wall-clock time. Large application bundles can enter
        // long-running initialization loops; this prevents them from blocking
        // the whole pipeline. The limit scales with script size.
        struct js_exec_timeout_state timeout_state;
        clock_gettime(CLOCK_MONOTONIC, &timeout_state.start);
        timeout_state.limit_seconds = 10.0 + (script_lens[i] / (1024.0 * 1024.0)) * 4.0;
        if (timeout_state.limit_seconds > 120.0) timeout_state.limit_seconds = 120.0;
        JS_SetInterruptHandler(JS_GetRuntime(ctx), js_exec_timeout_handler, &timeout_state);

        // Reset diagnostic for property-on-undefined errors
        g_last_undefined_prop[0] = '\0';

        // Execute script directly. Domain-specific string patches are not
        // applied; custom-element upgrade and missing standard APIs are
        // implemented in the browser layer instead.
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
        fprintf(stderr, "[js_quickjs] <<< EVAL script %d/%d result=%s\n",
                i, script_count, JS_IsException(result) ? "exception" : "ok");
        fflush(stderr);

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
    
    log_to_file("js_quickjs", "All %d scripts executed", script_count);

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

    fprintf(stderr, "[js_quickjs] END exec success_count=%d captured=%d status=%d\n",
            success_count, out_result->captured_url_count, out_result->status);
    fflush(stderr);

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

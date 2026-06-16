# Technical Specification: Generic URL Decryption

## API Hook Implementation Details

### 1. URL Capture System

```c
// url_capture.h
#ifndef URL_CAPTURE_H
#define URL_CAPTURE_H

#define MAX_CAPTURED_URLS 64
#define MAX_URL_LEN 2048

typedef struct {
    char urls[MAX_CAPTURED_URLS][MAX_URL_LEN];
    int count;
    bool finalized;  // Set to true after capture is complete
} CapturedUrls;

// Initialize capture state
void url_capture_init(void);

// Capture a URL
void url_capture_add(const char* url);

// Check if URL should be captured (googlevideo.com with sig)
bool url_capture_should_capture(const char* url);

// Get captured URLs
const CapturedUrls* url_capture_get(void);

// Reset capture state
void url_capture_reset(void);

#endif
```

```c
// url_capture.c
#include "url_capture.h"
#include <string.h>

static CapturedUrls g_captured = {0};

void url_capture_init(void) {
    g_captured.count = 0;
    g_captured.finalized = false;
}

void url_capture_add(const char* url) {
    if (!url || g_captured.count >= MAX_CAPTURED_URLS) return;
    
    // Check for duplicates
    for (int i = 0; i < g_captured.count; i++) {
        if (strcmp(g_captured.urls[i], url) == 0) {
            return;  // Already captured
        }
    }
    
    strncpy(g_captured.urls[g_captured.count], url, MAX_URL_LEN - 1);
    g_captured.urls[g_captured.count][MAX_URL_LEN - 1] = '\0';
    g_captured.count++;
    
    printf("[URL Capture] #%d: %.100s...\n", g_captured.count, url);
}

bool url_capture_should_capture(const char* url) {
    if (!url) return false;
    
    // Must be googlevideo.com
    if (!strstr(url, "googlevideo.com")) return false;
    
    // Must be videoplayback endpoint
    if (!strstr(url, "videoplayback")) return false;
    
    // Must have decrypted signature
    if (!strstr(url, "sig=") && !strstr(url, "signature=")) return false;
    
    return true;
}

const CapturedUrls* url_capture_get(void) {
    return &g_captured;
}

void url_capture_reset(void) {
    g_captured.count = 0;
    g_captured.finalized = false;
}
```

### 2. Enhanced fetch() Implementation

```cpp
// js_fetch_hook.cpp
#include "quickjs.h"
#include "url_capture.h"

// Mock Response object
static JSValue js_mock_response_text(JSContext* ctx, JSValueConst this_val, 
                                      int argc, JSValueConst* argv) {
    // Return empty string promise
    JSValue promise = JS_NewPromise(ctx);
    JSValue resolve = JS_GetPropertyStr(ctx, promise, "resolve");
    JSValue empty = JS_NewString(ctx, "");
    JS_Call(ctx, resolve, JS_UNDEFINED, 1, &empty);
    JS_FreeValue(ctx, empty);
    JS_FreeValue(ctx, resolve);
    return JS_GetPropertyStr(ctx, promise, "promise");
}

static JSValue js_mock_response_json(JSContext* ctx, JSValueConst this_val, 
                                      int argc, JSValueConst* argv) {
    // Return empty object promise
    JSValue promise = JS_NewPromise(ctx);
    JSValue resolve = JS_GetPropertyStr(ctx, promise, "resolve");
    JSValue empty = JS_NewObject(ctx);
    JS_Call(ctx, resolve, JS_UNDEFINED, 1, &empty);
    JS_FreeValue(ctx, empty);
    JS_FreeValue(ctx, resolve);
    return JS_GetPropertyStr(ctx, promise, "promise");
}

static JSValue js_fetch_hook(JSContext* ctx, JSValueConst this_val, 
                              int argc, JSValueConst* argv) {
    const char* url_str = NULL;
    
    // Extract URL from first argument
    if (JS_IsString(argv[0])) {
        url_str = JS_ToCString(ctx, argv[0]);
    } else {
        // Try to get .url property (Request object)
        JSValue url_prop = JS_GetPropertyStr(ctx, argv[0], "url");
        if (JS_IsString(url_prop)) {
            url_str = JS_ToCString(ctx, url_prop);
        }
        JS_FreeValue(ctx, url_prop);
    }
    
    // Capture if it's a video URL
    if (url_str) {
        if (url_capture_should_capture(url_str)) {
            url_capture_add(url_str);
        }
        JS_FreeCString(ctx, url_str);
    }
    
    // Create mock Response object
    JSValue response = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, response, "ok", JS_TRUE);
    JS_SetPropertyStr(ctx, response, "status", JS_NewInt32(ctx, 200));
    JS_SetPropertyStr(ctx, response, "statusText", JS_NewString(ctx, "OK"));
    JS_SetPropertyStr(ctx, response, "headers", JS_NewObject(ctx));
    
    // Add mock methods
    JS_SetPropertyStr(ctx, response, "text", 
        JS_NewCFunction(ctx, js_mock_response_text, "text", 0));
    JS_SetPropertyStr(ctx, response, "json", 
        JS_NewCFunction(ctx, js_mock_response_json, "json", 0));
    JS_SetPropertyStr(ctx, response, "blob", 
        JS_NewCFunction(ctx, js_mock_response_text, "blob", 0));
    JS_SetPropertyStr(ctx, response, "arrayBuffer", 
        JS_NewCFunction(ctx, js_mock_response_text, "arrayBuffer", 0));
    
    // Return resolved Promise<Response>
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    JSValue resolve_func = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
    JSValue result = JS_Call(ctx, resolve_func, promise_ctor, 1, &response);
    
    JS_FreeValue(ctx, response);
    JS_FreeValue(ctx, resolve_func);
    JS_FreeValue(ctx, promise_ctor);
    JS_FreeValue(ctx, global);
    
    return result;
}

// Register the hook
void register_fetch_hook(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "fetch", 
        JS_NewCFunction(ctx, js_fetch_hook, "fetch", 2));
    JS_FreeValue(ctx, global);
}
```

### 3. URL Constructor Implementation

```cpp
// js_url_constructor.cpp
#include "quickjs.h"
#include "url_capture.h"
#include <string.h>

// Simple URL resolution for common cases
static char* resolve_url(const char* base, const char* rel, char* out, size_t out_len) {
    if (strstr(rel, "://")) {
        // Already absolute
        strncpy(out, rel, out_len - 1);
        out[out_len - 1] = '\0';
        return out;
    }
    
    if (rel[0] == '/') {
        // Protocol-relative or path-absolute
        if (rel[1] == '/') {
            // Protocol-relative: //host/path
            const char* proto_end = strstr(base, "://");
            if (proto_end) {
                size_t proto_len = proto_end - base + 3;  // include "://"
                snprintf(out, out_len, "%.*s%s", (int)proto_len, base, rel + 2);
            } else {
                strncpy(out, rel, out_len - 1);
                out[out_len - 1] = '\0';
            }
        } else {
            // Path-absolute: /path
            const char* host_end = strstr(base, "://");
            if (host_end) {
                host_end += 3;
                const char* path_start = strchr(host_end, '/');
                if (path_start) {
                    size_t base_len = path_start - base;
                    snprintf(out, out_len, "%.*s%s", (int)base_len, base, rel);
                } else {
                    snprintf(out, out_len, "%s%s", base, rel);
                }
            } else {
                strncpy(out, rel, out_len - 1);
                out[out_len - 1] = '\0';
            }
        }
        return out;
    }
    
    // Relative path
    const char* last_slash = strrchr(base, '/');
    if (last_slash && last_slash > strstr(base, "://")) {
        size_t dir_len = last_slash - base + 1;
        snprintf(out, out_len, "%.*s%s", (int)dir_len, base, rel);
    } else {
        snprintf(out, out_len, "%s/%s", base, rel);
    }
    
    return out;
}

static JSValue js_url_constructor(JSContext* ctx, JSValueConst new_target,
                                   int argc, JSValueConst* argv) {
    const char* url_str = JS_ToCString(ctx, argv[0]);
    const char* base_str = NULL;
    
    if (argc > 1) {
        base_str = JS_ToCString(ctx, argv[1]);
    }
    
    char resolved[2048];
    if (base_str) {
        resolve_url(base_str, url_str, resolved, sizeof(resolved));
    } else {
        strncpy(resolved, url_str, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }
    
    // Capture if it's a video URL
    if (url_capture_should_capture(resolved)) {
        url_capture_add(resolved);
    }
    
    // Parse URL components
    JSValue obj = JS_NewObject(ctx);
    
    // href - full URL
    JS_SetPropertyStr(ctx, obj, "href", JS_NewString(ctx, resolved));
    
    // protocol
    const char* proto_end = strstr(resolved, "://");
    if (proto_end) {
        char proto[32];
        size_t len = proto_end - resolved + 1;  // include ':'
        if (len < sizeof(proto)) {
            strncpy(proto, resolved, len);
            proto[len] = '\0';
            JS_SetPropertyStr(ctx, obj, "protocol", JS_NewString(ctx, proto));
        }
    }
    
    // host, hostname, port
    if (proto_end) {
        const char* host_start = proto_end + 3;
        const char* path_start = strchr(host_start, '/');
        const char* query_start = strchr(host_start, '?');
        const char* hash_start = strchr(host_start, '#');
        
        const char* end = host_start + strlen(host_start);
        if (path_start) end = path_start;
        if (query_start && query_start < end) end = query_start;
        if (hash_start && hash_start < end) end = hash_start;
        
        size_t host_len = end - host_start;
        char* host = (char*)malloc(host_len + 1);
        if (host) {
            strncpy(host, host_start, host_len);
            host[host_len] = '\0';
            JS_SetPropertyStr(ctx, obj, "host", JS_NewString(ctx, host));
            
            // hostname (without port)
            char* port_sep = strchr(host, ':');
            if (port_sep) {
                *port_sep = '\0';
                JS_SetPropertyStr(ctx, obj, "hostname", JS_NewString(ctx, host));
                JS_SetPropertyStr(ctx, obj, "port", JS_NewString(ctx, port_sep + 1));
            } else {
                JS_SetPropertyStr(ctx, obj, "hostname", JS_NewString(ctx, host));
                JS_SetPropertyStr(ctx, obj, "port", JS_NewString(ctx, ""));
            }
            
            free(host);
        }
    }
    
    // pathname
    if (proto_end) {
        const char* path_start = strchr(proto_end + 3, '/');
        if (path_start) {
            const char* query_start = strchr(path_start, '?');
            const char* hash_start = strchr(path_start, '#');
            
            size_t len = strlen(path_start);
            if (query_start) len = query_start - path_start;
            if (hash_start && hash_start < path_start + len) {
                len = hash_start - path_start;
            }
            
            char* pathname = (char*)malloc(len + 1);
            if (pathname) {
                strncpy(pathname, path_start, len);
                pathname[len] = '\0';
                JS_SetPropertyStr(ctx, obj, "pathname", JS_NewString(ctx, pathname));
                free(pathname);
            }
        } else {
            JS_SetPropertyStr(ctx, obj, "pathname", JS_NewString(ctx, "/"));
        }
    }
    
    // search
    const char* query = strchr(resolved, '?');
    if (query) {
        const char* hash = strchr(query, '#');
        size_t len = hash ? (hash - query) : strlen(query);
        char* search = (char*)malloc(len + 1);
        if (search) {
            strncpy(search, query, len);
            search[len] = '\0';
            JS_SetPropertyStr(ctx, obj, "search", JS_NewString(ctx, search));
            free(search);
        }
    } else {
        JS_SetPropertyStr(ctx, obj, "search", JS_NewString(ctx, ""));
    }
    
    // hash
    const char* hash = strchr(resolved, '#');
    if (hash) {
        JS_SetPropertyStr(ctx, obj, "hash", JS_NewString(ctx, hash));
    } else {
        JS_SetPropertyStr(ctx, obj, "hash", JS_NewString(ctx, ""));
    }
    
    // toString method
    JS_SetPropertyStr(ctx, obj, "toString", 
        JS_NewCFunction(ctx, js_url_to_string, "toString", 0));
    
    JS_FreeCString(ctx, url_str);
    if (base_str) JS_FreeCString(ctx, base_str);
    
    return obj;
}

void register_url_constructor(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "URL", 
        JS_NewCFunction2(ctx, js_url_constructor, "URL", 1, JS_CFUNC_constructor, 0));
    JS_FreeValue(ctx, global);
}
```

### 4. YouTube-Specific Stubs

```cpp
// youtube_stubs.cpp
#include "quickjs.h"

void register_youtube_stubs(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    
    // window.ytplayer
    JSValue ytplayer = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ytplayer, "config", JS_NULL);
    JS_SetPropertyStr(ctx, global, "ytplayer", ytplayer);
    
    // window.ytcsi
    JSValue ytcsi = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ytcsi, "tick", 
        JS_NewCFunction(ctx, js_dummy_function, "tick", 1));
    JS_SetPropertyStr(ctx, ytcsi, "info", 
        JS_NewCFunction(ctx, js_dummy_function, "info", 2));
    JS_SetPropertyStr(ctx, global, "ytcsi", ytcsi);
    
    // window.ytcfg
    JSValue ytcfg_data = JS_NewObject(ctx);
    JSValue ytcfg = JS_NewObject(ctx);
    
    // Store data object
    JS_SetPropertyStr(ctx, ytcfg, "_data", ytcfg_data);
    
    // get method
    JS_SetPropertyStr(ctx, ytcfg, "get", 
        JS_NewCFunction(ctx, js_ytcfg_get, "get", 2));
    
    // set method  
    JS_SetPropertyStr(ctx, ytcfg, "set", 
        JS_NewCFunction(ctx, js_ytcfg_set, "set", 2));
    
    JS_SetPropertyStr(ctx, global, "ytcfg", ytcfg);
    
    JS_FreeValue(ctx, ytplayer);
    JS_FreeValue(ctx, ytcsi);
    JS_FreeValue(ctx, ytcfg);
    JS_FreeValue(ctx, global);
}

static JSValue js_ytcfg_get(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    const char* key = JS_ToCString(ctx, argv[0]);
    JSValue data = JS_GetPropertyStr(ctx, this_val, "_data");
    JSValue value = JS_GetPropertyStr(ctx, data, key);
    
    if (JS_IsUndefined(value) && argc > 1) {
        // Return default value
        JS_FreeValue(ctx, value);
        value = JS_DupValue(ctx, argv[1]);
    }
    
    JS_FreeValue(ctx, data);
    JS_FreeCString(ctx, key);
    return value;
}

static JSValue js_ytcfg_set(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    const char* key = JS_ToCString(ctx, argv[0]);
    JSValue data = JS_GetPropertyStr(ctx, this_val, "_data");
    JS_SetPropertyStr(ctx, data, key, JS_DupValue(ctx, argv[1]));
    JS_FreeValue(ctx, data);
    JS_FreeCString(ctx, key);
    return JS_UNDEFINED;
}
```

### 5. Execution Pipeline

```cpp
// generic_decrypt_pipeline.cpp
#include "quickjs.h"
#include "html_media_extract.h"
#include "url_capture.h"
#include "youtube_stubs.h"

typedef struct {
    char* html;
    size_t html_len;
    JSContext* ctx;
    CapturedUrls captured;
} DecryptContext;

bool extract_media_url_generic(const char* html, HtmlMediaCandidate* out,
                                char* err, size_t err_len) {
    // 1. Initialize
    url_capture_init();
    
    // 2. Create QuickJS context
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);
    
    // 3. Add all browser stubs
    JSValue global = JS_GetGlobalObject(ctx);
    init_browser_api_impl(ctx, global);
    
    // 4. Register enhanced hooks
    register_fetch_hook(ctx);
    register_url_constructor(ctx);
    register_youtube_stubs(ctx);
    
    // 5. Extract and execute scripts
    ScriptInfo scripts[MAX_SCRIPTS];
    int script_count = extract_scripts_in_order(html, scripts, MAX_SCRIPTS);
    
    printf("[Pipeline] Executing %d scripts...\n", script_count);
    
    for (int i = 0; i < script_count; i++) {
        if (!scripts[i].content) continue;
        
        printf("[Pipeline] Script %d (%s)...\n", i, 
               scripts[i].type == SCRIPT_TYPE_EXTERNAL ? "external" : "inline");
        
        JSValue result = JS_Eval(ctx, scripts[i].content, scripts[i].content_len,
                                 scripts[i].url[0] ? scripts[i].url : "<inline>",
                                 JS_EVAL_TYPE_GLOBAL);
        
        if (JS_IsException(result)) {
            JSValue exception = JS_GetException(ctx);
            const char* error_str = JS_ToCString(ctx, exception);
            printf("[Pipeline] Script %d error: %s\n", i, error_str);
            JS_FreeCString(ctx, error_str);
            JS_FreeValue(ctx, exception);
        }
        
        JS_FreeValue(ctx, result);
    }
    
    // 6. Inject trigger script
    const char* trigger_script = R"(
        // Force player to start loading
        if (window.ytplayer) {
            // Try various initialization methods
            if (window.ytplayer.load) {
                try { window.ytplayer.load(); } catch(e) {}
            }
            
            // Trigger video loading
            if (window.ytplayer.config && window.ytplayer.config.args) {
                var url = window.ytplayer.config.args.url_encoded_fmt_stream_map;
                if (url) {
                    // Parse and trigger fetches
                }
            }
        }
        
        // Try to access player API
        if (window.yt && window.yt.player) {
            // Access player directly
        }
    )";
    
    JSValue trigger_result = JS_Eval(ctx, trigger_script, strlen(trigger_script),
                                      "<trigger>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, trigger_result);
    
    // 7. Check captured URLs
    const CapturedUrls* captured = url_capture_get();
    printf("[Pipeline] Captured %d URLs\n", captured->count);
    
    bool success = false;
    if (captured->count > 0) {
        // Return the first captured URL
        strncpy(out->url, captured->urls[0], 2047);
        out->url[2047] = '\0';
        
        // Determine MIME type from URL params
        if (strstr(out->url, "mime=video")) {
            strncpy(out->mime, "video/mp4", sizeof(out->mime) - 1);
        } else if (strstr(out->url, "mime=audio")) {
            strncpy(out->mime, "audio/mp4", sizeof(out->mime) - 1);
        } else {
            strncpy(out->mime, "video/mp4", sizeof(out->mime) - 1);
        }
        
        success = true;
        printf("[Pipeline] SUCCESS: Captured URL: %.100s...\n", out->url);
    } else {
        snprintf(err, err_len, "No URLs captured after script execution");
        printf("[Pipeline] FAILED: No URLs captured\n");
    }
    
    // 8. Cleanup
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    
    free_script_infos(scripts, script_count);
    
    return success;
}
```

---

## Testing Strategy

```cpp
// test_generic_decrypt.cpp
void test_url_capture() {
    url_capture_init();
    
    // Test 1: Should capture
    url_capture_add("https://rr4---sn-xyz.googlevideo.com/videoplayback?sig=abc123");
    
    // Test 2: Should not capture (no googlevideo.com)
    url_capture_add("https://www.youtube.com/watch?v=dQw4w9WgXcQ");
    
    // Test 3: Should not capture (no sig)
    url_capture_add("https://rr4---sn-xyz.googlevideo.com/videoplayback?expire=123");
    
    const CapturedUrls* captured = url_capture_get();
    assert(captured->count == 1);
    printf("URL capture test PASSED\n");
}

void test_fetch_hook() {
    // Create QuickJS context
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);
    
    url_capture_init();
    register_fetch_hook(ctx);
    
    // Execute JavaScript that calls fetch
    const char* test_js = `
        fetch("https://rr4---sn-xyz.googlevideo.com/videoplayback?sig=test123");
    `;
    
    JSValue result = JS_Eval(ctx, test_js, strlen(test_js), "<test>", 0);
    JS_FreeValue(ctx, result);
    
    const CapturedUrls* captured = url_capture_get();
    assert(captured->count == 1);
    assert(strstr(captured->urls[0], "test123") != NULL);
    
    printf("Fetch hook test PASSED\n");
    
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}
```

---

## Integration Checklist

- [x] URL capture system
- [x] Enhanced fetch() hook
- [x] URL constructor implementation
- [x] YouTube-specific stubs
- [x] Execution pipeline
- [ ] XMLHttpRequest enhancement
- [ ] Response object mock
- [ ] Full integration test
- [ ] Error handling
- [ ] Performance optimization
---

## Critical Discovery: How YouTube Actually Loads Video

Based on code analysis of the player (`youtube_script_024_external.js`), YouTube loads video through **two different mechanisms**:

### Mechanism 1: MediaSource API (DASH Streaming - Primary)
```javascript
// Line 63841, 63851 in player code
if (window.MediaSource) {
    const mediaSource = new window.MediaSource();
    const objectUrl = URL.createObjectURL(mediaSource);
    this.Wd(objectUrl);  // Sets blob: URL on video element
    // Then fetches segments via MediaSource's SourceBuffer
}
```

### Mechanism 2: Direct Video Source (Progressive Fallback)
```javascript
// Line 87363 - g.vD class (video wrapper)
Wd(z) {  // Set source method
    this.K.src = z;  // Direct assignment to videoElement.src
}
```

**This means we need THREE interception points:**
1. `videoElement.src` setter - for direct URL assignment
2. `URL.createObjectURL()` - for MediaSource blob URLs
3. `fetch()` / `XMLHttpRequest` - for MediaSource segment fetching

---

## 5. Video Element `.src` Property Hook

This is the **most direct** way to capture URLs - when YouTube sets `video.src = "https://googlevideo.com/..."`.

```cpp
// js_video_element_hook.cpp
#include "quickjs.h"
#include "url_capture.h"

// Forward declarations
static JSValue js_video_dummy_method(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv);
static JSValue js_video_can_play_type(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv);
static JSValue js_video_src_getter(JSContext* ctx, JSValueConst this_val);
static JSValue js_video_src_setter(JSContext* ctx, JSValueConst this_val, JSValueConst val);

// Create video element with hooked src property
static JSValue js_create_video_element(JSContext* ctx) {
    JSValue video = JS_NewObject(ctx);
    
    // Define src property with getter/setter
    JSValue getter = JS_NewCFunction(ctx, js_video_src_getter, "src_getter", 0);
    JSValue setter = JS_NewCFunction(ctx, js_video_src_setter, "src_setter", 1);
    
    JS_DefineProperty(ctx, video, "src", getter, setter, JS_PROP_CONFIGURABLE);
    
    JS_FreeValue(ctx, getter);
    JS_FreeValue(ctx, setter);
    
    // Add other video element properties
    JS_SetPropertyStr(ctx, video, "currentTime", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, video, "duration", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, video, "paused", JS_TRUE);
    JS_SetPropertyStr(ctx, video, "ended", JS_FALSE);
    JS_SetPropertyStr(ctx, video, "volume", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, video, "playbackRate", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, video, "readyState", JS_NewInt32(ctx, 0)); // HAVE_NOTHING
    JS_SetPropertyStr(ctx, video, "networkState", JS_NewInt32(ctx, 0)); // NETWORK_EMPTY
    JS_SetPropertyStr(ctx, video, "error", JS_NULL);
    
    // Add methods
    JS_SetPropertyStr(ctx, video, "play", 
        JS_NewCFunction(ctx, js_video_dummy_method, "play", 0));
    JS_SetPropertyStr(ctx, video, "pause", 
        JS_NewCFunction(ctx, js_video_dummy_method, "pause", 0));
    JS_SetPropertyStr(ctx, video, "load", 
        JS_NewCFunction(ctx, js_video_dummy_method, "load", 0));
    JS_SetPropertyStr(ctx, video, "canPlayType", 
        JS_NewCFunction(ctx, js_video_can_play_type, "canPlayType", 1));
    
    return video;
}

static JSValue js_video_src_getter(JSContext* ctx, JSValueConst this_val) {
    return JS_GetPropertyStr(ctx, this_val, "_src_internal");
}

static JSValue js_video_src_setter(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    const char* url = JS_ToCString(ctx, val);
    if (url) {
        // CAPTURE: Check if this is a googlevideo URL
        if (url_capture_should_capture(url)) {
            url_capture_add(url);
            printf("[VideoHook] Captured URL from video.src: %.100s...\n", url);
        }
        JS_FreeCString(ctx, url);
    }
    
    // Store the value internally
    JS_SetPropertyStr(ctx, this_val, "_src_internal", JS_DupValue(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_video_dummy_method(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    // Return resolved promise for play()
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    JSValue resolve = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
    JSValue result = JS_Call(ctx, resolve, promise_ctor, 0, NULL);
    JS_FreeValue(ctx, resolve);
    JS_FreeValue(ctx, promise_ctor);
    JS_FreeValue(ctx, global);
    return result;
}

static JSValue js_video_can_play_type(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    // Always say we can play it
    return JS_NewString(ctx, "probably");
}

// Override document.createElement to return hooked video element
static JSValue js_document_create_element_hook(JSContext* ctx, JSValueConst this_val,
                                                int argc, JSValueConst* argv) {
    const char* tag = JS_ToCString(ctx, argv[0]);
    
    JSValue element;
    if (tag && (strcasecmp(tag, "video") == 0 || strcasecmp(tag, "audio") == 0)) {
        // Return hooked video/audio element
        element = js_create_video_element(ctx);
    } else {
        // Return regular element
        element = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, element, "tagName", JS_NewString(ctx, tag ? tag : ""));
    }
    
    JS_FreeCString(ctx, tag);
    return element;
}

void register_video_element_hooks(JSContext* ctx) {
    // Override document.createElement
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue document = JS_GetPropertyStr(ctx, global, "document");
    
    JS_SetPropertyStr(ctx, document, "createElement",
        JS_NewCFunction(ctx, js_document_create_element_hook, "createElement", 1));
    
    JS_FreeValue(ctx, document);
    JS_FreeValue(ctx, global);
}
```

---

## 6. URL.createObjectURL Hook

For MediaSource blob URLs:

```cpp
// js_url_create_object.cpp
#include "quickjs.h"
#include "url_capture.h"

static JSValue js_url_create_object_url(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    // Check if it's a MediaSource
    // In a real implementation, check JS_GetOpaque or class ID
    
    // Generate a fake blob URL
    static int blob_counter = 0;
    char blob_url[256];
    snprintf(blob_url, sizeof(blob_url), "blob:https://youtube.com/%d", ++blob_counter);
    
    printf("[URL Hook] createObjectURL called, returning: %s\n", blob_url);
    
    // If it's a MediaSource, we should hook its SourceBuffer
    // For now, just return the blob URL
    
    return JS_NewString(ctx, blob_url);
}

static JSValue js_url_revoke_object_url(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    // Just a stub
    return JS_UNDEFINED;
}

void register_url_create_object_hooks(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue url_ctor = JS_GetPropertyStr(ctx, global, "URL");
    
    // Add static methods
    JS_SetPropertyStr(ctx, url_ctor, "createObjectURL",
        JS_NewCFunction(ctx, js_url_create_object_url, "createObjectURL", 1));
    JS_SetPropertyStr(ctx, url_ctor, "revokeObjectURL",
        JS_NewCFunction(ctx, js_url_revoke_object_url, "revokeObjectURL", 1));
    
    JS_FreeValue(ctx, url_ctor);
    JS_FreeValue(ctx, global);
}
```

---

## 7. Enhanced XMLHttpRequest Hook

```cpp
// js_xhr_hook.cpp
#include "quickjs.h"
#include "url_capture.h"

typedef struct {
    char url[2048];
    char method[16];
    bool opened;
    JSValue onload;
    JSValue onerror;
    int readyState;
    int status;
} XHRState;

static JSClassID js_xhr_class_id;

static JSValue js_xhr_constructor(JSContext* ctx, JSValueConst new_target,
                                   int argc, JSValueConst* argv) {
    XHRState* state = (XHRState*)js_mallocz(ctx, sizeof(XHRState));
    state->readyState = 0; // UNSENT
    state->status = 0;
    
    JSValue obj = JS_NewObjectClass(ctx, js_xhr_class_id);
    JS_SetOpaque(obj, state);
    
    // Set initial properties
    JS_SetPropertyStr(ctx, obj, "readyState", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, obj, "statusText", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, obj, "responseText", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, obj, "response", JS_NewString(ctx, ""));
    
    return obj;
}

static JSValue js_xhr_open(JSContext* ctx, JSValueConst this_val, 
                            int argc, JSValueConst* argv) {
    XHRState* state = (XHRState*)JS_GetOpaque2(ctx, this_val, js_xhr_class_id);
    if (!state) return JS_EXCEPTION;
    
    const char* method = JS_ToCString(ctx, argv[0]);
    const char* url = JS_ToCString(ctx, argv[1]);
    
    if (method && url) {
        strncpy(state->method, method, sizeof(state->method)-1);
        strncpy(state->url, url, sizeof(state->url)-1);
        state->opened = true;
        state->readyState = 1; // OPENED
        
        // Update property
        JS_SetPropertyStr(ctx, this_val, "readyState", JS_NewInt32(ctx, 1));
    }
    
    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, url);
    
    return JS_UNDEFINED;
}

static JSValue js_xhr_send(JSContext* ctx, JSValueConst this_val, 
                            int argc, JSValueConst* argv) {
    XHRState* state = (XHRState*)JS_GetOpaque2(ctx, this_val, js_xhr_class_id);
    if (!state || !state->opened) return JS_EXCEPTION;
    
    // CAPTURE: Check and capture URL
    if (url_capture_should_capture(state->url)) {
        url_capture_add(state->url);
        printf("[XHR Hook] Captured URL: %.100s...\n", state->url);
    }
    
    // Update state to DONE
    state->readyState = 4;
    state->status = 200;
    JS_SetPropertyStr(ctx, this_val, "readyState", JS_NewInt32(ctx, 4));
    JS_SetPropertyStr(ctx, this_val, "status", JS_NewInt32(ctx, 200));
    JS_SetPropertyStr(ctx, this_val, "statusText", JS_NewString(ctx, "OK"));
    
    // Trigger onload callback asynchronously
    JSValue onload = JS_GetPropertyStr(ctx, this_val, "onload");
    if (JS_IsFunction(ctx, onload)) {
        JSValue event = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, "load"));
        JS_SetPropertyStr(ctx, event, "target", JS_DupValue(ctx, this_val));
        
        JS_Call(ctx, onload, this_val, 1, &event);
        JS_FreeValue(ctx, event);
    }
    JS_FreeValue(ctx, onload);
    
    return JS_UNDEFINED;
}

static JSValue js_xhr_set_request_header(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    // Stub - just accept headers
    return JS_UNDEFINED;
}

static void js_xhr_finalizer(JSRuntime* rt, JSValue val) {
    XHRState* state = (XHRState*)JS_GetOpaque(val, js_xhr_class_id);
    if (state) {
        js_free_rt(rt, state);
    }
}

void register_xmlhttprequest_hook(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    
    // Define XHR class
    JSClassDef xhr_class = {
        .class_name = "XMLHttpRequest",
        .finalizer = js_xhr_finalizer,
    };
    JS_NewClassID(&js_xhr_class_id);
    JS_NewClass(rt, js_xhr_class_id, &xhr_class);
    
    // Create constructor
    JSValue xhr_ctor = JS_NewCFunction2(ctx, js_xhr_constructor, "XMLHttpRequest", 
                                         0, JS_CFUNC_constructor, 0);
    
    // Add prototype methods
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "open", 
        JS_NewCFunction(ctx, js_xhr_open, "open", 3));
    JS_SetPropertyStr(ctx, proto, "send", 
        JS_NewCFunction(ctx, js_xhr_send, "send", 1));
    JS_SetPropertyStr(ctx, proto, "setRequestHeader", 
        JS_NewCFunction(ctx, js_xhr_set_request_header, "setRequestHeader", 2));
    JS_SetPropertyStr(ctx, proto, "abort", 
        JS_NewCFunction(ctx, js_video_dummy_method, "abort", 0));
    JS_SetPropertyStr(ctx, proto, "getResponseHeader", 
        JS_NewCFunction(ctx, js_video_dummy_method, "getResponseHeader", 1));
    JS_SetPropertyStr(ctx, proto, "getAllResponseHeaders", 
        JS_NewCFunction(ctx, js_video_dummy_method, "getAllResponseHeaders", 0));
    
    JS_SetConstructor(ctx, xhr_ctor, proto);
    JS_FreeValue(ctx, proto);
    
    // Register globally
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "XMLHttpRequest", xhr_ctor);
    JS_FreeValue(ctx, global);
}
```

---

## Updated Execution Pipeline

```cpp
bool extract_media_url_generic(const char* html, HtmlMediaCandidate* out,
                                char* err, size_t err_len) {
    // 1. Initialize
    url_capture_init();
    
    // 2. Create QuickJS context
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);
    
    // 3. Add all browser stubs (existing)
    JSValue global = JS_GetGlobalObject(ctx);
    init_browser_api_impl(ctx, global);
    
    // 4. Register ALL hooks (NEW: video element hooks)
    register_fetch_hook(ctx);
    register_xmlhttprequest_hook(ctx);       // Enhanced version
    register_url_constructor(ctx);
    register_url_create_object_hooks(ctx);   // NEW
    register_video_element_hooks(ctx);       // NEW
    
    // 5-10. ... rest of execution (extract scripts, execute, trigger, capture)
    // See previous sections for complete code...
}
```

---

## Required API Implementation Status (Updated)

| API | Status | Priority | Notes |
|-----|--------|----------|-------|
| `fetch()` | ✅ Basic stub | ⭐⭐⭐ High | Needs enhanced hook |
| `XMLHttpRequest` | ✅ Implemented | ⭐⭐⭐ High | Needs enhanced hook (line 87363) |
| `videoElement.src` setter | ❌ Not implemented | ⭐⭐⭐ **Critical** | **Direct URL capture point** |
| `URL.createObjectURL` | ❌ Not implemented | ⭐⭐⭐ **Critical** | For MediaSource blob URLs |
| `URL` constructor | ❌ Not implemented | ⭐⭐ Medium | URL resolution |
| `MediaSource` | ❌ Not implemented | ⭐⭐ Medium | DASH streaming support |
| `SourceBuffer` | ❌ Not implemented | ⭐⭐ Medium | MediaSource segment handling |
| `Promise` | ✅ Implemented | ✅ Ready | Already works |
| `Response` | ❌ Not implemented | ⭐ Low | Mock for fetch |
| `window.ytplayer` | ✅ Auto-created | ✅ Ready | Created by YouTube scripts |
| `window.ytcsi` | ✅ Auto-created | ✅ Ready | Created by YouTube scripts |
| `window.ytcfg` | ✅ Auto-created | ✅ Ready | Created by YouTube scripts |

---

## Updated Integration Checklist

- [x] URL capture system
- [x] Enhanced fetch() hook
- [x] URL constructor implementation
- [x] YouTube-specific stubs (auto-created)
- [x] Execution pipeline
- [ ] **Video element `.src` hook** ⭐⭐⭐ CRITICAL
- [ ] **URL.createObjectURL hook** ⭐⭐⭐ CRITICAL
- [ ] Enhanced XMLHttpRequest hook
- [ ] Response object mock
- [ ] Full integration test
- [ ] Error handling
- [ ] Performance optimization

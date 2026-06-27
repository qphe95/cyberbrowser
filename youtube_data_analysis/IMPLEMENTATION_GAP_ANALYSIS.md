# Implementation Gap Analysis

## What's ALREADY Implemented in cyberbrowser

### ✅ Video Element with URL Capture
**File:** `cyberbrowser/src/js_quickjs.cpp` (lines 345-357)

```cpp
static GCValue js_video_set_src(JSContextHandle ctx, GCValue this_val, GCValue val) {
    const char *src = JS_ToCString(ctx, val);
    if (src) {
        vid.set_src(src);
        record_captured_url(src);  // ✅ CAPTURES URL!
    }
    ...
}
```

When YouTube does `videoElement.src = "https://googlevideo.com/..."`, it's captured!

### ✅ fetch() API
**File:** `cyberbrowser/src/browser_api_impl.cpp` (line 3085)

```cpp
JS_SetPropertyStr(ctx, global, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 2));
```

### ✅ XMLHttpRequest
**File:** `cyberbrowser/src/browser_api_impl.cpp` (lines 3048-3061)

```cpp
GCValue xhr_ctor = JS_NewCFunction2(ctx, js_xhr_constructor, "XMLHttpRequest", ...);
```

### ✅ Console APIs
**File:** `cyberbrowser/src/browser_api_impl.cpp` (lines 3037-3045)

```cpp
GCValue console = JS_NewObject(ctx);
DEF_FUNC(ctx, console, "log", js_console_log, 1);
DEF_FUNC(ctx, console, "error", js_console_log, 1);
DEF_FUNC(ctx, console, "warn", js_console_log, 1);
...
```

### ✅ Timing APIs
**File:** `cyberbrowser/src/browser_api_impl.cpp` (lines 2804-2807)

```cpp
DEF_FUNC(ctx, window, "setTimeout", js_zero, 2);
DEF_FUNC(ctx, window, "setInterval", js_zero, 2);
DEF_FUNC(ctx, window, "clearTimeout", js_undefined, 1);
DEF_FUNC(ctx, window, "clearInterval", js_undefined, 1);
```

### ✅ Location
**File:** `cyberbrowser/src/browser_api_impl.cpp` (lines 2969-2980)

```cpp
GCValue location = JS_NewObject(ctx);
DEF_PROP_STR(ctx, location, "href", "https://www.youtube.com/watch?v=dQw4w9WgXcQ");
DEF_PROP_STR(ctx, location, "protocol", "https:");
DEF_PROP_STR(ctx, location, "host", "www.youtube.com");
...
```

### ✅ Navigator
**File:** `cyberbrowser/src/browser_api_impl.cpp` (lines 2985-3002)

```cpp
GCValue navigator = JS_NewObject(ctx);
DEF_PROP_STR(ctx, navigator, "userAgent", "Mozilla/5.0...");
DEF_PROP_STR(ctx, navigator, "language", "en-US");
DEF_PROP_BOOL(ctx, navigator, "cookieEnabled", 1);
...
```

### ✅ Custom Elements
**File:** `cyberbrowser/src/browser_api_impl.cpp` (lines 3098-3112)

```cpp
GCValue custom_elements = JS_NewObjectClass(ctx, js_custom_element_registry_class_id);
JS_SetPropertyStr(ctx, custom_elements, "define", ...);
JS_SetPropertyStr(ctx, custom_elements, "get", ...);
JS_SetPropertyStr(ctx, window, "customElements", custom_elements);
```

### ✅ DOM Elements (Basic)
- EventTarget, Node, Element, HTMLElement
- Document, DocumentFragment
- querySelector, querySelectorAll, appendChild, etc.

---

## What's MISSING (Need to Implement)

### 🔴 CRITICAL - Blocking URL Extraction

#### 1. `URL.createObjectURL()` / `URL.revokeObjectURL()`
**Why needed:** YouTube uses MediaSource for DASH streaming:
```javascript
const mediaSource = new MediaSource();
const url = URL.createObjectURL(mediaSource);  // Creates blob: URL
video.src = url;
```

**Implementation:**
```cpp
static JSValue js_url_create_object_url(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    // Generate fake blob URL
    static int counter = 0;
    char blob_url[256];
    snprintf(blob_url, sizeof(blob_url), "blob:https://youtube.com/%d", ++counter);
    
    // If it's a MediaSource, we should track it
    // For now, just return the blob URL
    return JS_NewString(ctx, blob_url);
}

void register_url_api(JSContext *ctx) {
    JSValue url_ctor = JS_GetPropertyStr(ctx, global, "URL");
    JS_SetPropertyStr(ctx, url_ctor, "createObjectURL",
        JS_NewCFunction(ctx, js_url_create_object_url, "createObjectURL", 1));
    JS_SetPropertyStr(ctx, url_ctor, "revokeObjectURL",
        JS_NewCFunction(ctx, js_dummy_function, "revokeObjectURL", 1));
}
```

#### 2. `MediaSource` API
**Why needed:** YouTube's primary streaming method:
```javascript
if (window.MediaSource) {
    const ms = new MediaSource();
    video.src = URL.createObjectURL(ms);
    // Then adds SourceBuffers and fetches segments
}
```

**Implementation:**
```cpp
static JSClassID js_media_source_class_id;

static JSValue js_media_source_constructor(JSContext *ctx, JSValueConst new_target,
                                            int argc, JSValueConst *argv) {
    JSValue obj = JS_NewObjectClass(ctx, js_media_source_class_id);
    
    // Add sourceBuffers property
    JS_SetPropertyStr(ctx, obj, "sourceBuffers", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, obj, "activeSourceBuffers", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, obj, "readyState", JS_NewString(ctx, "closed"));
    JS_SetPropertyStr(ctx, obj, "duration", JS_NewFloat64(ctx, NaN));
    
    return obj;
}

static JSValue js_media_source_is_type_supported(JSContext *ctx, JSValueConst this_val,
                                                  int argc, JSValueConst *argv) {
    // Always say yes to video formats
    return JS_TRUE;
}

static JSValue js_media_source_add_source_buffer(JSContext *ctx, JSValueConst this_val,
                                                  int argc, JSValueConst *argv) {
    // Create a mock SourceBuffer
    JSValue sb = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, sb, "mode", JS_NewString(ctx, "segments"));
    JS_SetPropertyStr(ctx, sb, "updating", JS_FALSE);
    JS_SetPropertyStr(ctx, sb, "buffered", JS_NewObject(ctx));  // TimeRanges
    
    // Store reference
    JSValue sourceBuffers = JS_GetPropertyStr(ctx, this_val, "sourceBuffers");
    // Add to array...
    
    return sb;
}

void register_media_source(JSContext *ctx) {
    JSValue media_source_ctor = JS_NewCFunction2(ctx, js_media_source_constructor,
                                                  "MediaSource", 0, JS_CFUNC_constructor, 0);
    
    // Static method
    JS_SetPropertyStr(ctx, media_source_ctor, "isTypeSupported",
        JS_NewCFunction(ctx, js_media_source_is_type_supported, "isTypeSupported", 1));
    
    JS_SetPropertyStr(ctx, global, "MediaSource", media_source_ctor);
    
    // Aliases
    JS_SetPropertyStr(ctx, global, "WebKitMediaSource", media_source_ctor);
    JS_SetPropertyStr(ctx, global, "ManagedMediaSource", media_source_ctor);
}
```

#### 3. Enhanced XMLHttpRequest with URL Capture
**Current status:** Basic XHR exists but may not capture URLs

**Need to add:**
- URL capture in `open()` method
- Better response mocking

```cpp
static JSValue js_xhr_open(JSContext *ctx, JSValueConst this_val, 
                            int argc, JSValueConst *argv) {
    const char* method = JS_ToCString(ctx, argv[0]);
    const char* url = JS_ToCString(ctx, argv[1]);
    
    // CAPTURE URL!
    if (url && strstr(url, "googlevideo.com")) {
        record_captured_url(url);
    }
    
    // Store for later
    JS_SetPropertyStr(ctx, this_val, "_url", JS_NewString(ctx, url));
    JS_SetPropertyStr(ctx, this_val, "_method", JS_NewString(ctx, method));
    JS_SetPropertyStr(ctx, this_val, "readyState", JS_NewInt32(ctx, 1)); // OPENED
    
    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, url);
    return JS_UNDEFINED;
}
```

### 🟡 IMPORTANT - Should Add

#### 4. `fetch()` Enhancement
**Current:** Basic stub exists
**Need:** URL capture + proper Response mock

```cpp
static JSValue js_fetch(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
    const char* url = NULL;
    
    if (JS_IsString(argv[0])) {
        url = JS_ToCString(ctx, argv[0]);
    } else {
        // Request object
        JSValue url_prop = JS_GetPropertyStr(ctx, argv[0], "url");
        if (JS_IsString(url_prop)) {
            url = JS_ToCString(ctx, url_prop);
        }
        JS_FreeValue(ctx, url_prop);
    }
    
    if (url) {
        record_captured_url(url);  // CAPTURE!
        JS_FreeCString(ctx, url);
    }
    
    // Return mock Promise<Response>
    ...
}
```

#### 5. LocalStorage / SessionStorage
```cpp
static JSValue js_storage_get_item(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    const char* key = JS_ToCString(ctx, argv[0]);
    // Return null (no persistence needed)
    JS_FreeCString(ctx, key);
    return JS_NULL;
}

static JSValue js_storage_set_item(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    // No-op (no persistence needed)
    return JS_UNDEFINED;
}

void register_storage(JSContext *ctx) {
    JSValue localStorage = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, localStorage, "getItem",
        JS_NewCFunction(ctx, js_storage_get_item, "getItem", 1));
    JS_SetPropertyStr(ctx, localStorage, "setItem",
        JS_NewCFunction(ctx, js_storage_set_item, "setItem", 2));
    JS_SetPropertyStr(ctx, window, "localStorage", localStorage);
    JS_SetPropertyStr(ctx, window, "sessionStorage", localStorage);  // Share mock
}
```

#### 6. `crypto.getRandomValues`
```cpp
static JSValue js_crypto_get_random_values(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
    // BotGuard uses this - just fill with random-ish data
    JSValue typed_array = argv[0];
    // Get buffer and fill with pseudo-random data
    return typed_array;  // Return same array
}

void register_crypto(JSContext *ctx) {
    JSValue crypto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, crypto, "getRandomValues",
        JS_NewCFunction(ctx, js_crypto_get_random_values, "getRandomValues", 1));
    JS_SetPropertyStr(ctx, window, "crypto", crypto);
}
```

### 🟢 NICE TO HAVE

#### 7. `AbortController`
```cpp
static JSValue js_abort_controller_constructor(JSContext *ctx, ...) {
    JSValue obj = JS_NewObject(ctx);
    JSValue signal = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, signal, "aborted", JS_FALSE);
    JS_SetPropertyStr(ctx, obj, "signal", signal);
    return obj;
}
```

#### 8. `matchMedia`
```cpp
static JSValue js_match_media(JSContext *ctx, ...) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "matches", JS_FALSE);
    JS_SetPropertyStr(ctx, obj, "media", argv[0]);
    return obj;
}
```

---

## Implementation Priority

### Phase 1: Minimum Viable (For URL Extraction)
1. ✅ Video element with `.src` hook - **DONE**
2. 🔴 `URL.createObjectURL()` - **CRITICAL**
3. 🔴 `MediaSource` constructor + `isTypeSupported()` - **CRITICAL**
4. 🔴 Enhanced `XMLHttpRequest.open()` with URL capture - **CRITICAL**
5. 🟡 Enhanced `fetch()` with URL capture - **IMPORTANT**

### Phase 2: Full Compatibility
6. 🟡 `localStorage/sessionStorage`
7. 🟡 `crypto.getRandomValues`
8. 🟢 `AbortController`
9. 🟢 `matchMedia`
10. 🟢 Other minor APIs

---

## Testing Checklist

Once Phase 1 is complete, test with:

```bash
cd cyberbrowser
./build.sh
./build/tests/cyberbrowser-tests
```

Expected behavior:
1. YouTube scripts execute without errors
2. When player sets `video.src`, URL is captured
3. When player uses `URL.createObjectURL(MediaSource)`, blob URL is created
4. When player calls `fetch()` for segments, URL is captured
5. Final captured URL should be `https://...googlevideo.com/videoplayback?sig=...`

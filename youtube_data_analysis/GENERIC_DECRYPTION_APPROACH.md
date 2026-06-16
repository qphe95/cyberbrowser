# Generic URL Decryption via Browser Emulation

## The Core Insight

Instead of reverse-engineering YouTube's signature decryption algorithm, we can **let the player's own JavaScript do the decryption** and capture the result!

**How it works:**
1. Load the player HTML and scripts in a browser emulation environment
2. Hook into network APIs (`fetch()`, `XMLHttpRequest`)
3. When the player decrypts the signature and tries to load the video, capture the URL
4. The captured URL is already decrypted!

This is the most generic and maintainable solution.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Browser Emulation Layer                      │
├─────────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌──────────────────────┐    │
│  │   HTML/JS   │  │   QuickJS   │  │  Network API Hooks   │    │
│  │   Engine    │→ │   Runtime   │→ │  (fetch/XHR/fetch)   │    │
│  └─────────────┘  └─────────────┘  └──────────────────────┘    │
│                                              │                  │
│                                              ↓                  │
│                                    ┌──────────────────┐         │
│                                    │  URL Interceptor │         │
│                                    │  (Captures URLs) │         │
│                                    └──────────────────┘         │
└─────────────────────────────────────────────────────────────────┘
```

---

## Step-by-Step Execution Flow

### Phase 1: Setup

```javascript
// 1. Create QuickJS context with browser stubs
const ctx = JS_NewContext(rt);

// 2. Initialize browser environment
init_browser_api_impl(ctx, global_obj);

// 3. Hook network APIs
hook_fetch_api(ctx);
hook_xmlhttprequest_api(ctx);
hook_url_constructor(ctx);  // Hook new URL() as well
```

### Phase 2: Load Content

```javascript
// 4. Load the YouTube watch page HTML
const html = fetch_youtube_html(video_id);

// 5. Execute inline scripts in order
const scripts = extract_inline_scripts(html);
for (const script of scripts) {
    JS_Eval(ctx, script.content, script.content_len, "<inline>", 0);
}

// 6. Fetch and execute external scripts
const external_scripts = extract_external_script_urls(html);
for (const url of external_scripts) {
    const js = fetch_script(url);
    JS_Eval(ctx, js, js.length, url, 0);
}
```

### Phase 3: Trigger Decryption

```javascript
// 7. Extract streaming data from ytInitialPlayerResponse
const streaming_data = extract_streaming_data(html);

// 8. Store cipher info globally so player can access it
JS_SetPropertyStr(ctx, global_obj, "__bgmdwnldr_ciphers", 
    JS_NewString(ctx, JSON.stringify(streaming_data)));

// 9. Inject trigger script that tells player to "start playback"
const trigger_script = `
    // The player will now:
    // 1. Read the cipher from ytInitialPlayerResponse
    // 2. Decrypt the signature internally
    // 3. Call fetch() or XMLHttpRequest with the decrypted URL
    // 4. Our hooks capture the URL!
    
    // Force player to start loading
    if (window.ytplayer && window.ytplayer.load) {
        window.ytplayer.load();
    }
`;
JS_Eval(ctx, trigger_script, trigger_script.length, "<trigger>", 0);
```

### Phase 4: Capture URLs

```c
// This is called when the player makes a network request
void on_network_request(const char* url, const char* method) {
    // Check if it's a googlevideo.com URL (the decrypted video URL)
    if (strstr(url, "googlevideo.com") && strstr(url, "videoplayback")) {
        // This URL contains the decrypted signature!
        // Example: https://rr4---sn-...googlevideo.com/videoplayback?...&sig=AP...xQ==
        
        if (strstr(url, "sig=") || strstr(url, "signature=")) {
            printf("CAPTURED DECRYPTED URL: %s\n", url);
            store_captured_url(url);
        }
    }
}
```

---

## Required Browser APIs to Hook

### Critical Hooks (Must Implement)

#### 1. `fetch()` API

```javascript
// Original fetch is replaced with our hook
const original_fetch = window.fetch;
window.fetch = function(url, options) {
    // Intercept the URL
    if (typeof url === 'string') {
        on_network_request(url, 'fetch');
    } else if (url instanceof Request) {
        on_network_request(url.url, 'fetch');
    }
    
    // Return a mock response - we don't actually need the data
    return Promise.resolve(new Response('', { status: 200 }));
};
```

#### 2. `XMLHttpRequest` API

```javascript
const OriginalXHR = window.XMLHttpRequest;
window.XMLHttpRequest = function() {
    const xhr = {
        open: function(method, url, async) {
            this._url = url;
            this._method = method;
        },
        send: function(body) {
            on_network_request(this._url, this._method);
            // Trigger onload to signal "success"
            if (this.onload) {
                setTimeout(() => this.onload(), 0);
            }
        },
        // ... other XHR properties/methods
    };
    return xhr;
};
```

#### 3. `URL` Constructor

```javascript
const OriginalURL = window.URL;
window.URL = function(url, base) {
    const fullUrl = base ? resolve_url(base, url) : url;
    
    // Check if this is creating a video URL
    if (fullUrl.includes('googlevideo.com')) {
        on_network_request(fullUrl, 'url-constructor');
    }
    
    return {
        href: fullUrl,
        protocol: 'https:',
        host: extract_host(fullUrl),
        // ... other URL properties
    };
};
```

### Important Supporting APIs

#### 4. `window.ytplayer` Object

The player expects this global object:

```javascript
window.ytplayer = {
    config: null,
    load: function() {
        // Player's internal load function
    },
    // Player will set these properties
};
```

#### 5. `window.ytcsi` (Client Side Instrumentation)

Used for timing and analytics:

```javascript
window.ytcsi = {
    tick: function(label) {
        // Just a stub - called frequently
    },
    info: function(label, data) {
        // Just a stub
    }
};
```

#### 6. `window.ytcfg` Configuration

```javascript
window.ytcfg = {
    data_: {},
    get: function(key, defaultValue) {
        return this.data_[key] !== undefined ? this.data_[key] : defaultValue;
    },
    set: function(key, value) {
        this.data_[key] = value;
    }
};

// Set player configuration
window.ytcfg.set('PLAYER_CONFIG', {
    sts: 19780,  // Signature timestamp
    // ... other config
});
```

---

## Complete Implementation Plan

### Step 1: Enhanced Browser Stubs

Add these to `browser_api_impl.cpp`:

```cpp
// Hook state
static void (*g_url_capture_callback)(const char* url) = NULL;

// Set callback for URL capture
void browser_api_impl_set_url_capture_callback(void (*callback)(const char*)) {
    g_url_capture_callback = callback;
}

// Check and capture URLs
static void check_and_capture_url(JSContext* ctx, const char* url) {
    if (!url) return;
    
    // Check if it's a googlevideo.com video URL
    if (strstr(url, "googlevideo.com") && strstr(url, "videoplayback")) {
        // Check if it has decrypted signature
        if (strstr(url, "sig=") || strstr(url, "signature=")) {
            if (g_url_capture_callback) {
                g_url_capture_callback(url);
            }
        }
    }
}
```

### Step 2: Hook fetch()

```cpp
static JSValue js_fetch_hook(JSContext* ctx, JSValueConst this_val, 
                              int argc, JSValueConst* argv) {
    // Get URL from first argument
    const char* url_str = NULL;
    
    if (JS_IsString(argv[0])) {
        url_str = JS_ToCString(ctx, argv[0]);
    } else {
        // It's a Request object, get .url property
        JSValue url_prop = JS_GetPropertyStr(ctx, argv[0], "url");
        if (JS_IsString(url_prop)) {
            url_str = JS_ToCString(ctx, url_prop);
        }
        JS_FreeValue(ctx, url_prop);
    }
    
    if (url_str) {
        check_and_capture_url(ctx, url_str);
        JS_FreeCString(ctx, url_str);
    }
    
    // Return mock Promise
    JSValue promise = JS_NewPromise(ctx);
    JSValue resolve = JS_GetPropertyStr(ctx, promise, "resolve");
    
    // Resolve with mock Response
    JSValue mock_response = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, mock_response, "ok", JS_TRUE);
    JS_SetPropertyStr(ctx, mock_response, "status", JS_NewInt32(ctx, 200));
    JS_SetPropertyStr(ctx, mock_response, "text", JS_NewCFunction(ctx, 
        js_mock_response_text, "text", 0));
    
    JS_Call(ctx, resolve, JS_UNDEFINED, 1, &mock_response);
    
    JS_FreeValue(ctx, resolve);
    JS_FreeValue(ctx, mock_response);
    
    return JS_GetPropertyStr(ctx, promise, "promise");
}
```

### Step 3: Hook XMLHttpRequest

```cpp
// XHR state
typedef struct {
    char url[2048];
    char method[16];
    bool opened;
} XHRState;

static JSValue js_xhr_open(JSContext* ctx, JSValueConst this_val, 
                            int argc, JSValueConst* argv) {
    XHRState* state = JS_GetOpaque(this_val, js_xhr_class_id);
    if (!state) return JS_EXCEPTION;
    
    const char* method = JS_ToCString(ctx, argv[0]);
    const char* url = JS_ToCString(ctx, argv[1]);
    
    if (method && url) {
        strncpy(state->method, method, sizeof(state->method)-1);
        strncpy(state->url, url, sizeof(state->url)-1);
        state->opened = true;
    }
    
    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, url);
    
    return JS_UNDEFINED;
}

static JSValue js_xhr_send(JSContext* ctx, JSValueConst this_val, 
                            int argc, JSValueConst* argv) {
    XHRState* state = JS_GetOpaque(this_val, js_xhr_class_id);
    if (!state || !state->opened) return JS_EXCEPTION;
    
    // Capture the URL!
    check_and_capture_url(ctx, state->url);
    
    // Trigger onload callback
    JSValue onload = JS_GetPropertyStr(ctx, this_val, "onload");
    if (!JS_IsUndefined(onload) && JS_IsFunction(ctx, onload)) {
        JSValue event = JS_NewObject(ctx);
        JS_Call(ctx, onload, this_val, 1, &event);
        JS_FreeValue(ctx, event);
    }
    JS_FreeValue(ctx, onload);
    
    // Update state
    JS_SetPropertyStr(ctx, this_val, "readyState", JS_NewInt32(ctx, 4)); // DONE
    JS_SetPropertyStr(ctx, this_val, "status", JS_NewInt32(ctx, 200));
    
    return JS_UNDEFINED;
}
```

### Step 4: Execution Pipeline

```cpp
typedef struct {
    char captured_urls[64][2048];
    int captured_count;
} CaptureState;

void on_url_captured(const char* url) {
    printf("[CAPTURED] %s\n", url);
    
    // Store for later retrieval
    if (g_capture_state.captured_count < 64) {
        strncpy(g_capture_state.captured_urls[g_capture_state.captured_count++],
                url, 2047);
    }
}

bool extract_media_url_generic(const char* html, HtmlMediaCandidate* out) {
    // 1. Initialize QuickJS
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);
    
    // 2. Add browser stubs
    JSValue global = JS_GetGlobalObject(ctx);
    init_browser_api_impl(ctx, global);
    
    // 3. Set URL capture hook
    browser_api_impl_set_url_capture_callback(on_url_captured);
    
    // 4. Extract and execute scripts
    ScriptInfo scripts[MAX_SCRIPTS];
    int script_count = extract_scripts_in_order(html, scripts, MAX_SCRIPTS);
    
    for (int i = 0; i < script_count; i++) {
        if (scripts[i].content) {
            JS_Eval(ctx, scripts[i].content, scripts[i].content_len,
                   scripts[i].url, JS_EVAL_TYPE_GLOBAL);
        }
    }
    
    // 5. Inject trigger to force decryption
    const char* trigger = `
        // The player has now loaded
        // It will attempt to access the video URL
        // Our hooks will capture it!
        
        // Force a "prefetch" or "preload" if available
        if (window.ytplayer && window.ytplayer.loadVideoByPlayerVars) {
            // Trigger playback preparation
        }
    `;
    JS_Eval(ctx, trigger, strlen(trigger), "<trigger>", JS_EVAL_TYPE_GLOBAL);
    
    // 6. Check captured URLs
    if (g_capture_state.captured_count > 0) {
        // Return the first captured URL
        strncpy(out->url, g_capture_state.captured_urls[0], 2047);
        out->url[2047] = '\0';
        return true;
    }
    
    return false;
}
```

---

## Advantages of This Approach

### 1. **Generic & Future-Proof**
- Works regardless of YouTube's encryption algorithm changes
- No need to reverse-engineer decryption code
- Handles algorithm updates automatically

### 2. **Complete**
- Captures fully-formed URLs with all parameters
- No need to manually construct URLs
- Handles complex URL generation logic

### 3. **Maintainable**
- Simpler than maintaining decryption algorithm
- Less code to update when YouTube changes
- More reliable

### 4. **Handles Edge Cases**
- Different URL formats (DASH, HLS, progressive)
- Multiple quality options
- Various codec combinations

---

## Required API Implementation Status

| API | Status in bgmdwnldr | Required for Generic Decryption |
|-----|---------------------|--------------------------------|
| `fetch()` | ✅ Basic stub | ✅ Need enhanced hook |
| `XMLHttpRequest` | ✅ Implemented | ✅ Need enhanced hook |
| `URL` | ❌ Not implemented | ✅ Need implementation |
| `window.ytplayer` | ❌ Not stubbed | ✅ Need stub |
| `window.ytcsi` | ❌ Not stubbed | ✅ Need stub |
| `window.ytcfg` | ⚠️ Partial | ✅ Need full implementation |
| `Promise` | ✅ Implemented | ✅ Already works |
| `Response` | ❌ Not implemented | ✅ Need mock |
| `Request` | ❌ Not implemented | ⚠️ Optional |

---

## Implementation Checklist

- [ ] Add URL capture callback mechanism
- [ ] Enhance `fetch()` stub with interception
- [ ] Enhance `XMLHttpRequest` with interception
- [ ] Implement `URL` constructor
- [ ] Implement `Response` mock object
- [ ] Add `window.ytplayer` stub object
- [ ] Add `window.ytcsi` instrumentation stub
- [ ] Complete `window.ytcfg` implementation
- [ ] Create execution pipeline
- [ ] Test with actual YouTube page

---

## Summary

**The key insight:** Instead of fighting YouTube's encryption, we let it decrypt for us!

By hooking network APIs and executing the player scripts in a controlled environment, we can capture the decrypted URLs as the player generates them. This is the most generic, maintainable, and robust approach to handling YouTube's URL encryption.

**Next step:** Implement the missing browser APIs and hook mechanisms listed above.

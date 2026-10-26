#ifndef JS_QUICKJS_H
#define JS_QUICKJS_H

#include <stdbool.h>
#include <stddef.h>

/* QuickJS headers */
#include "quickjs.h"
#include "platform.h"
#include "det_clock.h"

/* Android AssetManager - only available on Android */
#ifdef BE_PLATFORM_ANDROID
struct AAssetManager;
typedef struct AAssetManager AAssetManager;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Global QuickJS runtime and context - initialized once in android_main */
extern JSRuntimeHandle g_js_runtime;
extern JSContextHandle g_js_context;

/* Maximum number of captured URLs */
#define JS_MAX_CAPTURED_URLS 64
#define JS_MAX_URL_LEN 2048

typedef enum {
    JS_EXEC_SUCCESS = 0,
    JS_EXEC_ERROR = -1,
    JS_EXEC_TIMEOUT = -2
} JsExecStatus;

/* Result of JS execution with captured URLs */
typedef struct JsExecResult {
    JsExecStatus status;
    int captured_url_count;
    char captured_urls[JS_MAX_CAPTURED_URLS][JS_MAX_URL_LEN];
    char title[256];
    char thumbnailUrl[2048];
} JsExecResult;

/* CSSStyleDeclaration helper functions exposed for the CSS parser. */
GCValue js_style_remove_property(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_style_set_property(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_style_get_property_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
/* The canvas getContext() native (for element wrappers). */
JSCFunction *js_canvas_get_context_fn(void);

/* Initialize QuickJS runtime (called once in android_main) */
bool js_quickjs_init(void);

/* C stack budget for JS recursion (pass to JS_SetMaxStackSize): 8 MB capped
 * to the thread's real stack minus a safety margin. */
size_t js_stack_budget(void);

/* Create global QuickJS runtime and context (called once in android_main after js_quickjs_init) */
bool js_quickjs_create_runtime(void);

/* Register the XMLHttpRequest / HTMLVideoElement classes with `rt` (idempotent;
 * init_browser_api_impl calls it for every runtime). */
void js_quickjs_register_classes(JSRuntimeHandle rt);

/* Set up initial DOM state (called once in android_main after js_quickjs_create_runtime) */
void js_quickjs_setup_initial_dom(void);

/* Reset class IDs (called during GC full reset) */
void js_quickjs_reset_class_ids(void);

/* Cleanup QuickJS runtime (not needed - runtime lives for app lifetime) */
void js_quickjs_clear_captured_urls(void);
void js_quickjs_cleanup(void);

/* Drain timers and pending JS jobs until quiescent. */
void js_quickjs_pump_timers_and_jobs(void);

/* Asynchronous fetch(): requests in flight (started, not yet settled);
 * settle the finished ones (main thread, returns how many); wait up to `ms`
 * for one to finish. */
/* The page's clock (timers, requestAnimationFrame, idle deadlines): real
 * time (det_clock.h: deterministic under CYBER_DET_TIME) plus the
 * virtual-time offset the event loop adds while it skips idle waits
 * (JS_time_offset_ms, also in Date and performance.now). */
static inline unsigned long long page_time_ms(void) {
    return (unsigned long long)page_clock_ms() + (unsigned long long)JS_time_offset_ms;
}
static inline void page_time_advance(unsigned long long ms) {
    JS_time_offset_ms += (int64_t)ms;
}

int js_quickjs_network_pending(void);
int js_quickjs_net_poll(JSContextHandle ctx);
void js_quickjs_net_wait(unsigned int ms);
/* Wait until every request in flight has finished (settled or not; at most
 * ms); false on timeout.  For CYBER_DET_TIME (det_clock.h). */
bool js_quickjs_net_wait_all(unsigned int ms);

/* A GET on a worker thread, like fetch(), finished by `done` on the main
 * thread from js_quickjs_net_poll (and counted as pending until then).
 * `done` may take resp->data (setting it to NULL). */
typedef void (*JsNetDoneFn)(JSContextHandle ctx, void *user, const char *url, bool ok, int status,
                            PlatformHttpBuffer *resp, const char *err);
bool js_quickjs_net_get(const char *url, JsNetDoneFn done, void *user);

/* Dispatch DOMContentLoaded and load events on document/window. */
void js_quickjs_dispatch_lifecycle_events(void);

/* Execute multiple JS scripts in a browser-like environment
 * Data payload scripts (ytInitialPlayerResponse, ytInitialData, etc.) will
 * execute naturally and define global variables, just like in a real browser.
 * 
 * scripts: array of JS code strings
 * script_lens: array of script lengths  
 * script_count: number of scripts
 * html: original HTML content for parsing video elements (can be NULL)
 * out_result: output structure for captured URLs
 */
/* Run one page script as js_quickjs_exec_scripts runs each (timeout, profile
 * entry, then the timers and jobs it leaves due); `index` and `count` name it
 * in logs; `pump` false for a script run inside another one (written by
 * document.write), where the microtask checkpoint waits for the outer one.
 * `module_url` non-NULL runs it as an ES module of that URL (its imports
 * resolve against it).  It runs in ctx (a frame's realm; NULL for the
 * page's).  Returns whether it completed without an exception. */
/* A page module (url names it, import.meta.url too): compiled, linked and
 * evaluated; its evaluation promise, or JS_EXCEPTION. */
GCValue js_quickjs_eval_module(JSContextHandle ctx, const char *src, size_t len, const char *url, int flags);
void js_quickjs_set_module_url(JSContextHandle ctx, JSModuleDefHandle m, const char *url);

bool js_quickjs_exec_script(JSContextHandle ctx, const char *script, size_t script_len, int index,
                            int count, bool pump, const char *module_url);

bool js_quickjs_exec_scripts(const char **scripts, const size_t *script_lens,
                             int script_count, const char *html,
                             JsExecResult *out_result);

/* Get captured URLs from global storage (for backward compatibility) */
int js_quickjs_get_captured_urls(char urls[][JS_MAX_URL_LEN], int max_urls);

/* Execute scripts and extract a string value from the JS global scope.
 * Used for browser emulation: load page scripts, then read config values like visitorData.
 * The global QuickJS context must already be initialized.
 * Returns true if the expression evaluates to a non-empty string.
 */
bool js_quickjs_extract_value(const char **scripts, const size_t *script_lens,
                              int script_count, const char *js_expr,
                              char *out_value, size_t out_value_len);

/* Android-specific functions */
#ifdef BE_PLATFORM_ANDROID
/* Set the Android asset manager for loading browser stubs */
void js_quickjs_set_asset_manager(AAssetManager *mgr);

/* Execute scripts with Android asset manager (legacy API) */
bool js_quickjs_exec_scripts_android(const char **scripts, const size_t *script_lens, 
                                     int script_count, const char *html, 
                                     AAssetManager *asset_mgr,
                                     JsExecResult *out_result);
#endif

#ifdef __cplusplus
}
#endif

#endif /* JS_QUICKJS_H */

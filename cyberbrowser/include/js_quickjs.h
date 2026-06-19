#ifndef JS_QUICKJS_H
#define JS_QUICKJS_H

#include <stdbool.h>
#include <stddef.h>

/* QuickJS headers */
#include "quickjs.h"

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

/* Initialize QuickJS runtime (called once in android_main) */
bool js_quickjs_init(void);

/* Create global QuickJS runtime and context (called once in android_main after js_quickjs_init) */
bool js_quickjs_create_runtime(void);

/* Set up initial DOM state (called once in android_main after js_quickjs_create_runtime) */
void js_quickjs_setup_initial_dom(void);

/* Reset class IDs (called during GC full reset) */
void js_quickjs_reset_class_ids(void);

/* Cleanup QuickJS runtime (not needed - runtime lives for app lifetime) */
void js_quickjs_clear_captured_urls(void);
void js_quickjs_cleanup(void);

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

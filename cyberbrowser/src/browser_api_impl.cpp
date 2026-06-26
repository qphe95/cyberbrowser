/*
 * Browser Stubs - Shared helpers and top-level initialization
 */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
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

// mbedtls includes (used by throw_dom_exception helper in this file)
#include "mbedtls/md.h"
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS 1
#include "mbedtls/private/gcm.h"

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
extern GCValue js_document_create_document_fragment(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

// DOM querySelector helpers from dom_api.cpp
extern "C" GCValue js_element_querySelector_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
extern "C" GCValue js_element_querySelectorAll_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

// Native DOM mutation helpers from dom_api.cpp (used by fallbacks to bypass
// page-script monkey patches).
extern "C" GCValue js_node_appendChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

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
GCHandle service_worker_handle = GC_HANDLE_NULL;

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

// Native appendChild exposed to JS fallbacks so they can add nodes even when
// page scripts have wrapped Element.prototype.appendChild.
static GCValue js_cyber_append_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0]) ||
        JS_IsUndefined(argv[1]) || JS_IsNull(argv[1])) {
        return JS_ThrowTypeError(ctx, "__cyber_appendChild requires parent and child");
    }
    return js_node_appendChild_real(ctx, argv[0], 1, &argv[1]);
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
    
    // Set ownerDocument on the text node.
    dom_node_set_owner_document(ctx, node, this_val);
    
    return node;
}

// document.createComment() - returns a proper Comment node object
static GCValue js_document_create_comment(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    const char *text = "";
    if (argc >= 1) {
        text = JS_ToCString(ctx, argv[0]);
        if (!text) text = "";
    }

    GCValue node = JS_NewObjectClass(ctx, js_dom_node_class_id);
    if (JS_IsException(node)) {
        return JS_NULL;
    }

    DOMNodeHandle dom_node = DOMNodeHandle::create(ctx, DOM_NODE_TYPE_COMMENT, "#comment");
    if (dom_node.valid()) {
        dom_node.set_node_value(text);
        dom_node.attach_to_object(node);
    }

    JS_SetPropertyStr(ctx, node, "nodeType", JS_NewInt32(ctx, DOM_NODE_TYPE_COMMENT));
    JS_SetPropertyStr(ctx, node, "nodeName", JS_NewString(ctx, "#comment"));
    JS_SetPropertyStr(ctx, node, "data", JS_NewString(ctx, text));
    JS_SetPropertyStr(ctx, node, "textContent", JS_NewString(ctx, text));
    JS_SetPropertyStr(ctx, node, "length", JS_NewInt32(ctx, (int)strlen(text)));
    dom_node_set_owner_document(ctx, node, this_val);
    return node;
}

// document.implementation.createHTMLDocument(title)
static GCValue js_document_implementation_create_html_document(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    const char *title_str = "";
    if (argc >= 1) {
        title_str = JS_ToCString(ctx, argv[0]);
        if (!title_str) title_str = "";
    }

    GCValue doc = JS_NewObjectClass(ctx, js_dom_node_class_id);
    if (JS_IsException(doc)) return doc;

    DOMNodeHandle doc_node = DOMNodeHandle::create(ctx, DOM_NODE_TYPE_DOCUMENT, "#document");
    if (doc_node.valid()) {
        doc_node.attach_to_object(doc);
        doc_node.set_owner_document(doc);
    }

    JS_SetPropertyStr(ctx, doc, "nodeType", JS_NewInt32(ctx, DOM_NODE_TYPE_DOCUMENT));
    JS_SetPropertyStr(ctx, doc, "nodeName", JS_NewString(ctx, "#document"));
    JS_SetPropertyStr(ctx, doc, "contentType", JS_NewString(ctx, "text/html"));
    JS_SetPropertyStr(ctx, doc, "readyState", JS_NewString(ctx, "complete"));
    JS_SetPropertyStr(ctx, doc, "URL", JS_NewString(ctx, "about:blank"));
    JS_SetPropertyStr(ctx, doc, "documentURI", JS_NewString(ctx, "about:blank"));
    JS_SetPropertyStr(ctx, doc, "title", JS_NewString(ctx, title_str));
    JS_SetPropertyStr(ctx, doc, "characterSet", JS_NewString(ctx, "UTF-8"));
    JS_SetPropertyStr(ctx, doc, "charset", JS_NewString(ctx, "UTF-8"));
    JS_SetPropertyStr(ctx, doc, "hidden", JS_FALSE);
    JS_SetPropertyStr(ctx, doc, "visibilityState", JS_NewString(ctx, "visible"));

    DEF_FUNC(ctx, doc, "createElement", js_document_create_element, 1);
    DEF_FUNC(ctx, doc, "createElementNS", js_document_create_element, 2);
    DEF_FUNC(ctx, doc, "createTextNode", js_document_create_text_node, 1);
    DEF_FUNC(ctx, doc, "createComment", js_document_create_comment, 1);
    DEF_FUNC(ctx, doc, "createDocumentFragment", js_create_document_fragment, 0);
    DEF_FUNC(ctx, doc, "createRange", js_document_create_range, 0);
    DEF_FUNC(ctx, doc, "createTreeWalker", js_document_create_tree_walker, 3);
    DEF_FUNC(ctx, doc, "importNode", js_document_import_node, 2);
    DEF_FUNC(ctx, doc, "getElementById", js_document_getElementById, 1);
    DEF_FUNC(ctx, doc, "querySelector", js_document_querySelector, 1);
    DEF_FUNC(ctx, doc, "querySelectorAll", js_document_querySelectorAll, 1);
    DEF_FUNC(ctx, doc, "getElementsByTagName", js_document_get_elements_by_tag_name, 1);
    DEF_FUNC(ctx, doc, "getElementsByClassName", js_document_getElementsByClassName, 1);

    // Build a minimal <html><head><title></title></head><body></body></html> tree.
    GCValue html_tag = JS_NewString(ctx, "html");
    GCValue head_tag = JS_NewString(ctx, "head");
    GCValue body_tag = JS_NewString(ctx, "body");
    GCValue title_tag = JS_NewString(ctx, "title");

    GCValue html = js_document_create_element(ctx, doc, 1, &html_tag);
    GCValue head = js_document_create_element(ctx, doc, 1, &head_tag);
    GCValue body = js_document_create_element(ctx, doc, 1, &body_tag);
    GCValue title_el = js_document_create_element(ctx, doc, 1, &title_tag);

    GCValue title_text_arg = JS_NewString(ctx, title_str);
    GCValue title_text = js_document_create_text_node(ctx, doc, 1, &title_text_arg);

    GCValue append_args[1];
    append_args[0] = title_text;
    js_node_appendChild_real(ctx, title_el, 1, append_args);
    append_args[0] = title_el;
    js_node_appendChild_real(ctx, head, 1, append_args);
    append_args[0] = body;
    js_node_appendChild_real(ctx, html, 1, append_args);
    append_args[0] = head;
    js_node_appendChild_real(ctx, html, 1, append_args);
    append_args[0] = html;
    js_node_appendChild_real(ctx, doc, 1, append_args);

    JS_SetPropertyStr(ctx, doc, "documentElement", html);
    JS_SetPropertyStr(ctx, doc, "head", head);
    JS_SetPropertyStr(ctx, doc, "body", body);

    return doc;
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

// Media-query evaluator for window.matchMedia.
static int js_match_media_get_viewport_width(JSContextHandle ctx) {
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue win = JS_GetPropertyStr(ctx, global, "window");
    GCValue v = JS_GetPropertyStr(ctx, win, "innerWidth");
    int32_t w = 1920;
    JS_ToInt32(ctx, &w, v);
    return w;
}
static int js_match_media_get_viewport_height(JSContextHandle ctx) {
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue win = JS_GetPropertyStr(ctx, global, "window");
    GCValue v = JS_GetPropertyStr(ctx, win, "innerHeight");
    int32_t h = 1080;
    JS_ToInt32(ctx, &h, v);
    return h;
}

static const char *js_mm_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}
static const char *js_mm_skip_ws_rev(const char *start, const char *p) {
    while (p > start && (*(p - 1) == ' ' || *(p - 1) == '\t' || *(p - 1) == '\n' || *(p - 1) == '\r')) p--;
    return p;
}

static bool js_mm_streq_ci(const char *s, const char *e, const char *lit) {
    const char *p = js_mm_skip_ws(s);
    e = js_mm_skip_ws_rev(s, e);
    while (p < e && *lit) {
        if (tolower((unsigned char)*p) != tolower((unsigned char)*lit)) return false;
        p++;
        lit++;
    }
    return p == e && *lit == '\0';
}

static bool js_mm_parse_double(const char *s, const char *e, double *out) {
    s = js_mm_skip_ws(s);
    e = js_mm_skip_ws_rev(s, e);
    char buf[64];
    size_t n = (size_t)(e - s);
    if (n == 0 || n >= sizeof(buf)) return false;
    memcpy(buf, s, n);
    buf[n] = '\0';
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf) return false;
    *out = v;
    return true;
}

static bool js_mm_parse_double_unit(const char *s, const char *e, double *out) {
    s = js_mm_skip_ws(s);
    e = js_mm_skip_ws_rev(s, e);
    char buf[128];
    size_t n = (size_t)(e - s);
    if (n == 0 || n >= sizeof(buf)) return false;
    memcpy(buf, s, n);
    buf[n] = '\0';
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf) return false;
    while (*end == ' ' || *end == '\t') end++;
    if (strncasecmp(end, "dppx", 4) == 0) v *= 96.0;
    else if (strncasecmp(end, "dpcm", 4) == 0) v *= 2.54;
    *out = v;
    return true;
}

static bool js_mm_parse_ratio(const char *s, const char *e, double *out) {
    s = js_mm_skip_ws(s);
    e = js_mm_skip_ws_rev(s, e);
    const char *slash = NULL;
    for (const char *p = s; p < e; p++) if (*p == '/') { slash = p; break; }
    if (!slash) return js_mm_parse_double(s, e, out);
    double num = 0, den = 0;
    if (!js_mm_parse_double(s, slash, &num)) return false;
    if (!js_mm_parse_double(slash + 1, e, &den)) return false;
    if (den == 0.0) return false;
    *out = num / den;
    return true;
}

static bool js_mm_known_boolean_feature(const char *name, const char *name_end) {
    // These are known media features; a bare feature name (without a value) is treated as supported.
    static const char *features[] = {
        "width", "height", "aspect-ratio", "orientation",
        "prefers-color-scheme", "prefers-reduced-motion", "prefers-contrast",
        "forced-colors", "-ms-high-contrast",
        "pointer", "any-pointer", "hover", "any-hover",
        "display-mode", "color-gamut", "dynamic-range", "video-dynamic-range",
        "resolution", "min-resolution", "max-resolution",
        "update", "overflow-block", "overflow-inline", "scripting",
        NULL
    };
    for (int i = 0; features[i]; i++) {
        if (js_mm_streq_ci(name, name_end, features[i])) return true;
    }
    return false;
}

static bool js_mm_eval_feature(const char *inner, const char *inner_end, int width, int height) {
    // inner is the content of a pair of parentheses, e.g. "min-width: 123px".
    const char *colon = NULL;
    for (const char *p = inner; p < inner_end; p++) {
        if (*p == ':') { colon = p; break; }
    }
    if (!colon) {
        return js_mm_known_boolean_feature(inner, inner_end);
    }

    const char *name = inner;
    const char *name_end = colon;
    const char *val = colon + 1;
    const char *val_end = inner_end;

    double v = 0;
    bool has_double = js_mm_parse_double(val, val_end, &v);

    if (js_mm_streq_ci(name, name_end, "min-width")) {
        if (!has_double) return false;
        return width >= v - 0.001;
    }
    if (js_mm_streq_ci(name, name_end, "max-width")) {
        if (!has_double) return false;
        return width <= v + 0.001;
    }
    if (js_mm_streq_ci(name, name_end, "width")) {
        if (!has_double) return false;
        return fabs(width - v) <= 0.5;
    }
    if (js_mm_streq_ci(name, name_end, "min-height")) {
        if (!has_double) return false;
        return height >= v - 0.001;
    }
    if (js_mm_streq_ci(name, name_end, "max-height")) {
        if (!has_double) return false;
        return height <= v + 0.001;
    }
    if (js_mm_streq_ci(name, name_end, "height")) {
        if (!has_double) return false;
        return fabs(height - v) <= 0.5;
    }
    if (js_mm_streq_ci(name, name_end, "min-aspect-ratio")) {
        double r = (double)width / (double)(height ? height : 1);
        double q = 0;
        if (!js_mm_parse_ratio(val, val_end, &q)) return false;
        return r >= q - 0.0001;
    }
    if (js_mm_streq_ci(name, name_end, "max-aspect-ratio")) {
        double r = (double)width / (double)(height ? height : 1);
        double q = 0;
        if (!js_mm_parse_ratio(val, val_end, &q)) return false;
        return r <= q + 0.0001;
    }
    if (js_mm_streq_ci(name, name_end, "aspect-ratio")) {
        double r = (double)width / (double)(height ? height : 1);
        double q = 0;
        if (!js_mm_parse_ratio(val, val_end, &q)) return false;
        return fabs(r - q) <= 0.001;
    }
    if (js_mm_streq_ci(name, name_end, "min-resolution") ||
        js_mm_streq_ci(name, name_end, "max-resolution") ||
        js_mm_streq_ci(name, name_end, "resolution")) {
        double dpi = 0;
        if (!js_mm_parse_double_unit(val, val_end, &dpi)) return false;
        const double device_dpi = 96.0;
        if (js_mm_streq_ci(name, name_end, "min-resolution")) return device_dpi >= dpi - 0.001;
        if (js_mm_streq_ci(name, name_end, "max-resolution")) return device_dpi <= dpi + 0.001;
        return fabs(device_dpi - dpi) <= 0.5;
    }
    if (js_mm_streq_ci(name, name_end, "orientation")) {
        bool landscape = width >= height;
        if (js_mm_streq_ci(val, val_end, "landscape")) return landscape;
        if (js_mm_streq_ci(val, val_end, "portrait")) return !landscape;
        return false;
    }
    if (js_mm_streq_ci(name, name_end, "prefers-color-scheme")) {
        if (js_mm_streq_ci(val, val_end, "dark")) return true;
        if (js_mm_streq_ci(val, val_end, "light")) return false;
        return false;
    }
    if (js_mm_streq_ci(name, name_end, "prefers-reduced-motion")) {
        if (js_mm_streq_ci(val, val_end, "reduce")) return false;
        if (js_mm_streq_ci(val, val_end, "no-preference")) return true;
        return false;
    }
    if (js_mm_streq_ci(name, name_end, "prefers-contrast")) {
        if (js_mm_streq_ci(val, val_end, "more")) return false;
        if (js_mm_streq_ci(val, val_end, "less")) return false;
        if (js_mm_streq_ci(val, val_end, "no-preference")) return true;
        return false;
    }
    if (js_mm_streq_ci(name, name_end, "forced-colors")) {
        if (js_mm_streq_ci(val, val_end, "active")) return false;
        if (js_mm_streq_ci(val, val_end, "none")) return true;
        return false;
    }
    if (js_mm_streq_ci(name, name_end, "-ms-high-contrast")) {
        if (js_mm_streq_ci(val, val_end, "active")) return false;
        if (js_mm_streq_ci(val, val_end, "none")) return true;
        return false;
    }
    if (js_mm_streq_ci(name, name_end, "pointer") || js_mm_streq_ci(name, name_end, "any-pointer")) {
        if (js_mm_streq_ci(val, val_end, "fine")) return true;
        if (js_mm_streq_ci(val, val_end, "coarse")) return false;
        if (js_mm_streq_ci(val, val_end, "none")) return false;
        return false;
    }
    if (js_mm_streq_ci(name, name_end, "hover") || js_mm_streq_ci(name, name_end, "any-hover")) {
        if (js_mm_streq_ci(val, val_end, "hover")) return true;
        if (js_mm_streq_ci(val, val_end, "none")) return false;
        return false;
    }
    if (js_mm_streq_ci(name, name_end, "display-mode")) {
        if (js_mm_streq_ci(val, val_end, "browser")) return true;
        return false;
    }
    if (js_mm_streq_ci(name, name_end, "color-gamut")) {
        if (js_mm_streq_ci(val, val_end, "srgb")) return true;
        if (js_mm_streq_ci(val, val_end, "p3")) return false;
        if (js_mm_streq_ci(val, val_end, "rec2020")) return false;
        return false;
    }
    if (js_mm_streq_ci(name, name_end, "dynamic-range") || js_mm_streq_ci(name, name_end, "video-dynamic-range")) {
        if (js_mm_streq_ci(val, val_end, "standard")) return true;
        if (js_mm_streq_ci(val, val_end, "high")) return false;
        return false;
    }
    if (js_mm_streq_ci(name, name_end, "update")) {
        if (js_mm_streq_ci(val, val_end, "fast")) return true;
        return false;
    }
    if (js_mm_streq_ci(name, name_end, "overflow-block") || js_mm_streq_ci(name, name_end, "overflow-inline")) {
        if (js_mm_streq_ci(val, val_end, "scroll")) return true;
        return false;
    }
    if (js_mm_streq_ci(name, name_end, "scripting")) {
        if (js_mm_streq_ci(val, val_end, "enabled")) return true;
        return false;
    }
    // Unknown feature/value combinations evaluate to false to avoid selecting mobile/specialised branches.
    return false;
}

static bool js_mm_eval_term(const char *s, const char *e, int width, int height) {
    s = js_mm_skip_ws(s);
    e = js_mm_skip_ws_rev(s, e);
    if (s >= e) return true;

    // Media type without parentheses.
    if (js_mm_streq_ci(s, e, "screen") || js_mm_streq_ci(s, e, "all")) return true;
    if (js_mm_streq_ci(s, e, "print") || js_mm_streq_ci(s, e, "tv")) return false;

    // Parenthesised feature.
    if (*s == '(' && *(e - 1) == ')') {
        return js_mm_eval_feature(s + 1, e - 1, width, height);
    }

    // Unknown bare term: false.
    return false;
}

static bool js_mm_eval_condition(const char *s, const char *e, int width, int height) {
    s = js_mm_skip_ws(s);
    e = js_mm_skip_ws_rev(s, e);
    if (s >= e) return true;

    bool negate = false;
    if (e - s >= 4 && strncasecmp(s, "not ", 4) == 0) {
        negate = true;
        s = js_mm_skip_ws(s + 4);
    } else if (e - s >= 5 && strncasecmp(s, "only ", 5) == 0) {
        s = js_mm_skip_ws(s + 5);
    }

    // Split remaining condition by " and " at depth 0.
    const char *p = s;
    int depth = 0;
    bool ok = true;
    const char *term_start = s;
    while (p <= e) {
        if (p < e && *p == '(') depth++;
        else if (p < e && *p == ')') depth--;
        bool at_end = (p == e);
        bool is_and = false;
        if (!at_end && depth == 0 && p + 5 <= e && strncasecmp(p, " and ", 5) == 0) {
            is_and = true;
        }
        if (at_end || is_and) {
            const char *term_end = is_and ? p : e;
            if (!js_mm_eval_term(term_start, term_end, width, height)) { ok = false; break; }
            if (is_and) {
                p += 5;
                term_start = p;
                continue;
            }
        }
        p++;
    }
    return negate ? !ok : ok;
}

static bool js_mm_eval_query(const char *query, int width, int height) {
    char buf[1024];
    strncpy(buf, query, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    const char *p = buf;
    const char *end = buf + strlen(buf);
    int depth = 0;
    const char *cond_start = p;
    while (p <= end) {
        if (p < end && *p == '(') depth++;
        else if (p < end && *p == ')') depth--;
        bool at_end = (p == end);
        bool is_or = false;
        if (!at_end && depth == 0) {
            if (*p == ',') is_or = true;
            else if (p + 4 <= end && strncasecmp(p, " or ", 4) == 0) is_or = true;
        }
        if (at_end || is_or) {
            const char *cond_end = is_or ? p : end;
            if (js_mm_eval_condition(cond_start, cond_end, width, height)) return true;
            if (is_or) {
                if (*p == ',') { p++; cond_start = p; }
                else { p += 4; cond_start = p; }
                continue;
            }
        }
        p++;
    }
    return false;
}

// MediaQueryList listener helpers (viewport is static, so listeners are just stored).
static GCValue js_match_media_add_listener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
    GCValue listeners = JS_GetPropertyStr(ctx, this_val, "__mql_listeners");
    if (!JS_IsArray(ctx, listeners)) {
        listeners = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, this_val, "__mql_listeners", listeners);
    }
    GCValue len_val = JS_GetPropertyStr(ctx, listeners, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, len_val);
    JS_SetPropertyUint32(ctx, listeners, len, argv[0]);
    return JS_UNDEFINED;
}
static GCValue js_match_media_remove_listener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    GCValue listeners = JS_GetPropertyStr(ctx, this_val, "__mql_listeners");
    if (!JS_IsArray(ctx, listeners)) return JS_UNDEFINED;
    GCValue len_val = JS_GetPropertyStr(ctx, listeners, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, len_val);
    for (int i = (int)len - 1; i >= 0; i--) {
        GCValue item = JS_GetPropertyUint32(ctx, listeners, (uint32_t)i);
        if (JS_StrictEq(ctx, item, argv[0])) {
            for (uint32_t j = (uint32_t)i; j + 1 < len; j++) {
                GCValue next = JS_GetPropertyUint32(ctx, listeners, j + 1);
                JS_SetPropertyUint32(ctx, listeners, j, next);
            }
            JS_SetPropertyStr(ctx, listeners, "length", JS_NewInt32(ctx, (int32_t)(len - 1)));
            break;
        }
    }
    return JS_UNDEFINED;
}
static GCValue js_match_media_add_event_listener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    const char *type = JS_ToCString(ctx, argv[0]);
    bool is_change = (type && strcasecmp(type, "change") == 0);
    if (!is_change) return JS_UNDEFINED;
    GCValue args[1] = { argv[1] };
    return js_match_media_add_listener(ctx, this_val, 1, args);
}
static GCValue js_match_media_remove_event_listener(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    const char *type = JS_ToCString(ctx, argv[0]);
    bool is_change = (type && strcasecmp(type, "change") == 0);
    if (!is_change) return JS_UNDEFINED;
    GCValue args[1] = { argv[1] };
    return js_match_media_remove_listener(ctx, this_val, 1, args);
}

static GCValue js_match_media(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc;
    const char *q = "";
    if (argc > 0 && JS_IsString(argv[0])) {
        q = JS_ToCString(ctx, argv[0]);
        if (!q) q = "";
    }
    int width = js_match_media_get_viewport_width(ctx);
    int height = js_match_media_get_viewport_height(ctx);
    bool matches = js_mm_eval_query(q, width, height);

    GCValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "matches", JS_NewBool(ctx, matches));
    JS_SetPropertyStr(ctx, result, "media", JS_NewString(ctx, q));
    JS_SetPropertyStr(ctx, result, "addListener", JS_NewCFunction(ctx, js_match_media_add_listener, "addListener", 1));
    JS_SetPropertyStr(ctx, result, "removeListener", JS_NewCFunction(ctx, js_match_media_remove_listener, "removeListener", 1));
    JS_SetPropertyStr(ctx, result, "addEventListener", JS_NewCFunction(ctx, js_match_media_add_event_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, result, "removeEventListener", JS_NewCFunction(ctx, js_match_media_remove_event_listener, "removeEventListener", 2));
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
    
    // Object.defineProperty: use native QuickJS implementation. The custom
    // polyfill caused crashes with large scripts (e.g., YouTube's kevlar_base
    // app shell) due to descriptor/value lifetime issues.
    // (Native Object.defineProperty is available once base objects are enabled.)
    
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
    
    // DOMException setup, isolated in a lambda so failures can exit early
    // without goto and without affecting the rest of initialization.
    auto setup_dom_exception = [&]() -> bool {
        GCValue dom_exception_proto = JS_NewObject(ctx);
        if (JS_IsException(dom_exception_proto)) {
            LOG_ERROR("Failed to create DOMException prototype");
            return false;
        }
        
        JS_SetClassProto(ctx, js_dom_exception_class_id, dom_exception_proto);
        JS_SetPropertyFunctionList(ctx, dom_exception_proto, js_dom_exception_proto_funcs,
            js_dom_exception_proto_funcs_count);
        
        // Set up prototype chain: DOMException.prototype -> Error.prototype
        GCValue error_ctor = JS_GetPropertyStr(ctx, global, "Error");
        if (!JS_IsException(error_ctor) && !JS_IsUndefined(error_ctor)) {
            GCValue error_proto = JS_GetPropertyStr(ctx, error_ctor, "prototype");
            if (!JS_IsException(error_proto) && !JS_IsUndefined(error_proto)) {
                JS_SetPrototype(ctx, dom_exception_proto, error_proto);
            }
        }
        
        GCValue dom_exception_ctor = JS_NewCFunction2(ctx, js_dom_exception_constructor, "DOMException", 2, JS_CFUNC_constructor, 0);
        if (JS_IsException(dom_exception_ctor)) {
            LOG_ERROR("dom_exception_ctor not usable after creation - SKIPPING DOMException setup");
            return false;
        }
        JS_SetConstructor(ctx, dom_exception_ctor, dom_exception_proto);
        if (JS_IsException(dom_exception_ctor)) {
            LOG_ERROR("dom_exception_ctor not usable after SetConstructor - SKIPPING DOMException setup");
            return false;
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
            return false;
        }
        
        JS_SetPropertyStr(ctx, global, "DOMException", dom_exception_ctor);
        return true;
    };
    
    if (!setup_dom_exception()) {
        LOG_ERROR("DOMException setup FAILED - continuing without DOMException (YouTube player may have reduced functionality)");
    }
    LOG_INFO("Continuing with rest of browser stubs initialization");
    
    // Map constructor
    LOG_INFO("Setting up Map constructor...");
    GCValue map_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, map_proto, js_map_proto_funcs, 
        js_map_proto_funcs_count);
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

    // Native DOM mutation helpers for fallback scripts.
    JS_SetPropertyStr(ctx, global, "__cyber_appendChild",
        JS_NewCFunction(ctx, js_cyber_append_child, "__cyber_appendChild", 2));

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

    // isConnected getter used by custom element lifecycle
    {
        GCValue is_conn_getter = JS_NewCFunction(ctx, js_node_is_connected_getter, "get isConnected", 0);
        JSAtom is_conn_atom = JS_NewAtom(ctx, "isConnected");
        JS_DefinePropertyGetSet(ctx, node_proto, is_conn_atom, is_conn_getter, JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, is_conn_atom);
    }

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

    // Polymer legacy elements expect these transform helpers on the prototype.
    static auto js_element_transform = [](JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
        (void)argc; (void)argv;
        if (JS_IsObject(this_val)) {
            GCValue style = JS_GetPropertyStr(ctx, this_val, "style");
            if (JS_IsObject(style) && argc >= 1) {
                GCValue val = argv[0];
                JS_SetPropertyStr(ctx, style, "transform", val);
                JS_SetPropertyStr(ctx, style, "webkitTransform", val);
            }
        }
        return JS_UNDEFINED;
    };
    static auto js_element_translate3d = [](JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
        if (argc < 3) return JS_UNDEFINED;
        const char *x = JS_ToCString(ctx, argv[0]);
        const char *y = JS_ToCString(ctx, argv[1]);
        const char *z = JS_ToCString(ctx, argv[2]);
        GCValue target = (argc >= 4 && JS_IsObject(argv[3])) ? argv[3] : this_val;
        if (x && y && z && JS_IsObject(target)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "translate3d(%s,%s,%s)", x, y, z);
            GCValue style = JS_GetPropertyStr(ctx, target, "style");
            if (JS_IsObject(style)) {
                JS_SetPropertyStr(ctx, style, "transform", JS_NewString(ctx, buf));
                JS_SetPropertyStr(ctx, style, "webkitTransform", JS_NewString(ctx, buf));
            }
        }
        return JS_UNDEFINED;
    };
    JS_SetPropertyStr(ctx, html_element_proto, "transform",
        JS_NewCFunction(ctx, js_element_transform, "transform", 2));
    JS_SetPropertyStr(ctx, html_element_proto, "translate3d",
        JS_NewCFunction(ctx, js_element_translate3d, "translate3d", 4));

    JS_SetPropertyStr(ctx, html_element_ctor, "prototype", html_element_proto);
    JS_SetPropertyStr(ctx, global, "HTMLElement", html_element_ctor);
    JS_SetPropertyStr(ctx, window, "HTMLElement", html_element_ctor);
    JS_SetPropertyStr(ctx, global, "__cyberHTMLElementProto", html_element_proto);
    // Keep a strong reference to the original native constructor so that
    // ES5 custom-element shims (e.g. Polymer's fast-shim) can capture and
    // extend it without the GC collecting the C constructor.
    JS_SetPropertyStr(ctx, global, "__origHTMLElement", html_element_ctor);
    JS_SetPropertyStr(ctx, window, "__origHTMLElement", html_element_ctor);
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
    JS_SetPrototype(ctx, template_proto, html_element_proto);
    JS_SetPropertyStr(ctx, template_proto, "constructor", template_ctor);
    JS_SetPropertyStr(ctx, template_ctor, "prototype", template_proto);
    JS_SetPropertyStr(ctx, global, "HTMLTemplateElement", template_ctor);
    
    // HTMLSlotElement constructor (needed by ShadyDOM polyfill)
    GCValue slot_ctor = JS_NewCFunction2(ctx, js_dummy_function, "HTMLSlotElement", 0, JS_CFUNC_constructor, 0);
    GCValue slot_proto = JS_NewObject(ctx);
    JS_SetPrototype(ctx, slot_proto, html_element_proto);
    JS_SetPropertyStr(ctx, slot_proto, "constructor", slot_ctor);
    JS_SetPropertyStr(ctx, slot_proto, "assignedNodes",
        JS_NewCFunction(ctx, js_slot_assigned_nodes, "assignedNodes", 1));
    JS_SetPropertyStr(ctx, slot_proto, "assignedElements",
        JS_NewCFunction(ctx, js_slot_assigned_elements, "assignedElements", 1));
    GCValue slot_name_getter = JS_NewCFunction(ctx, js_slot_get_name, "get name", 0);
    JSAtom slot_name_atom = JS_NewAtom(ctx, "name");
    JS_DefinePropertyGetSet(ctx, slot_proto, slot_name_atom, slot_name_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, slot_name_atom);
    JS_SetPropertyStr(ctx, slot_ctor, "prototype", slot_proto);
    JS_SetPropertyStr(ctx, global, "HTMLSlotElement", slot_ctor);
    JS_SetPropertyStr(ctx, window, "HTMLSlotElement", slot_ctor);
    
    // DocumentFragment constructor (needs node_proto)
    GCValue doc_fragment_ctor = JS_NewCFunction2(ctx, js_dummy_function, "DocumentFragment", 0, JS_CFUNC_constructor, 0);
    GCValue doc_fragment_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, doc_fragment_proto, "constructor", doc_fragment_ctor);
    // Inherit Node methods (appendChild, insertBefore, removeChild, cloneNode, ...)
    JS_SetPrototype(ctx, doc_fragment_proto, node_proto);
    // DocumentFragment needs querySelector(All) for template.content usage.
    JS_SetPropertyStr(ctx, doc_fragment_proto, "querySelector",
        JS_NewCFunction(ctx, js_element_querySelector_real, "querySelector", 1));
    JS_SetPropertyStr(ctx, doc_fragment_proto, "querySelectorAll",
        JS_NewCFunction(ctx, js_element_querySelectorAll_real, "querySelectorAll", 1));

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


    // ===== DOM Prototype Chain =====
    // Prototype chains are already wired above via JS_SetPrototype().
    // The previous Object.setPrototypeOf integrity block was inert (it built
    // argument arrays but never invoked the function), so it is removed.


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
    
    // ownerDocument getter
    GCValue owner_document_getter = JS_NewCFunction(ctx, js_node_get_ownerDocument, "get ownerDocument", 0);
    JSAtom owner_document_atom = JS_NewAtom(ctx, "ownerDocument");
    JS_DefinePropertyGetSet(ctx, node_proto, owner_document_atom, owner_document_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, owner_document_atom);
    
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
    JS_SetPropertyStr(ctx, html_element_proto, "hasAttributes",
        JS_NewCFunction(ctx, js_element_has_attributes, "hasAttributes", 0));
    JS_SetPropertyStr(ctx, html_element_proto, "getAttributeNames",
        JS_NewCFunction(ctx, js_element_get_attribute_names, "getAttributeNames", 0));
    JS_SetPropertyStr(ctx, html_element_proto, "matches",
        JS_NewCFunction(ctx, js_element_matches, "matches", 1));
    JS_SetPropertyStr(ctx, html_element_proto, "closest",
        JS_NewCFunction(ctx, js_element_closest, "closest", 1));
    GCValue attr_getter_he = JS_NewCFunction(ctx, js_element_get_attributes, "get attributes", 0);
    JSAtom attr_atom_he = JS_NewAtom(ctx, "attributes");
    JS_DefinePropertyGetSet(ctx, html_element_proto, attr_atom_he, attr_getter_he, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, attr_atom_he);
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
    JS_SetPropertyStr(ctx, element_proto, "hasAttributes",
        JS_NewCFunction(ctx, js_element_has_attributes, "hasAttributes", 0));
    JS_SetPropertyStr(ctx, element_proto, "getAttributeNames",
        JS_NewCFunction(ctx, js_element_get_attribute_names, "getAttributeNames", 0));
    JS_SetPropertyStr(ctx, element_proto, "matches",
        JS_NewCFunction(ctx, js_element_matches, "matches", 1));
    JS_SetPropertyStr(ctx, element_proto, "closest",
        JS_NewCFunction(ctx, js_element_closest, "closest", 1));
    GCValue attr_getter = JS_NewCFunction(ctx, js_element_get_attributes, "get attributes", 0);
    JSAtom attr_atom = JS_NewAtom(ctx, "attributes");
    JS_DefinePropertyGetSet(ctx, element_proto, attr_atom, attr_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, attr_atom);
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
    
    // style getter
    GCValue style_getter = JS_NewCFunction(ctx, js_element_get_style, "get style", 0);
    JSAtom style_atom = JS_NewAtom(ctx, "style");
    JS_DefinePropertyGetSet(ctx, element_proto, style_atom, style_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, style_atom);
    
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
    // textContent getter/setter
    GCValue text_content_getter = JS_NewCFunction(ctx, js_node_get_text_content, "get textContent", 0);
    GCValue text_content_setter = JS_NewCFunction(ctx, js_node_set_text_content, "set textContent", 1);
    JSAtom text_content_atom = JS_NewAtom(ctx, "textContent");
    JS_DefinePropertyGetSet(ctx, node_proto, text_content_atom, text_content_getter, text_content_setter, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, text_content_atom);
    
    // nodeValue getter/setter
    GCValue node_value_getter = JS_NewCFunction(ctx, js_node_get_node_value, "get nodeValue", 0);
    GCValue node_value_setter = JS_NewCFunction(ctx, js_node_set_node_value, "set nodeValue", 1);
    JSAtom node_value_atom = JS_NewAtom(ctx, "nodeValue");
    JS_DefinePropertyGetSet(ctx, node_proto, node_value_atom, node_value_getter, node_value_setter, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, node_value_atom);
    
    // data getter/setter (CharacterData interface, used by Text/Comment nodes)
    GCValue data_getter = JS_NewCFunction(ctx, js_node_get_data, "get data", 0);
    GCValue data_setter = JS_NewCFunction(ctx, js_node_set_data, "set data", 1);
    JSAtom data_atom = JS_NewAtom(ctx, "data");
    JS_DefinePropertyGetSet(ctx, node_proto, data_atom, data_getter, data_setter, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, data_atom);
    
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
    // window.addEventListener/removeEventListener/dispatchEvent are inherited
    // from Window.prototype (which is set to EventTarget.prototype methods).
    // Do NOT define own-property stubs here or they will shadow the real methods.
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
    GCValue document = JS_NewObjectClass(ctx, js_dom_node_class_id);
    if (JS_IsException(document)) {
        document = JS_NewObject(ctx);
    }
    LOG_INFO("Document object created");
    {
        DOMNodeHandle doc_node = DOMNodeHandle::create(ctx, DOM_NODE_TYPE_DOCUMENT, "#document");
        if (doc_node.valid()) {
            doc_node.attach_to_object(document);
            doc_node.set_owner_document(document);
        }
    }
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
    DEF_FUNC(ctx, document, "createComment", js_document_create_comment, 1);
    DEF_FUNC(ctx, document, "createDocumentFragment", js_document_create_document_fragment, 0);
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
    GCValue doc_style_sheets_getter = JS_NewCFunction(ctx, js_document_get_style_sheets, "get styleSheets", 0);
    JS_DefinePropertyGetSet(ctx, document, JS_NewAtom(ctx, "styleSheets"),
        doc_style_sheets_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    GCValue doc_adopted_sheets_getter = JS_NewCFunction(ctx, js_document_get_adopted_style_sheets, "get adoptedStyleSheets", 0);
    GCValue doc_adopted_sheets_setter = JS_NewCFunction(ctx, js_document_set_adopted_style_sheets, "set adoptedStyleSheets", 1);
    JS_DefinePropertyGetSet(ctx, document, JS_NewAtom(ctx, "adoptedStyleSheets"),
        doc_adopted_sheets_getter, doc_adopted_sheets_setter, JS_PROP_ENUMERABLE);
    DEF_FUNC(ctx, document, "elementFromPoint", js_document_element_from_point, 2);
    DEF_FUNC(ctx, document, "addEventListener", js_event_target_addEventListener, 2);
    DEF_FUNC(ctx, document, "removeEventListener", js_event_target_removeEventListener, 2);
    DEF_FUNC(ctx, document, "dispatchEvent", js_event_target_dispatchEvent, 1);
    DEF_FUNC(ctx, document, "getComputedStyle", js_get_computed_style, 1);
    
    // Create document.implementation with createHTMLDocument
    GCValue doc_impl = JS_NewObject(ctx);
    if (!JS_IsException(doc_impl)) {
        JS_SetPropertyStr(ctx, doc_impl, "createHTMLDocument",
            JS_NewCFunction(ctx, js_document_implementation_create_html_document, "createHTMLDocument", 1));
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
    dom_node_set_owner_document(ctx, doc_element, document);
    
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
    dom_node_set_owner_document(ctx, body_element, document);
    
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
    dom_node_set_owner_document(ctx, head_element, document);
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
    
    // Blob / object URL registry used by URL.createObjectURL/revokeObjectURL
    JS_SetPropertyStr(ctx, global, "__blobRegistry", JS_NewObject(ctx));
    
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
    GCValue css_style_sheet_ctor = JS_NewCFunction2(ctx, js_css_style_sheet_constructor, "CSSStyleSheet",
        0, JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "CSSStyleSheet", css_style_sheet_ctor);
    JS_SetPropertyStr(ctx, window, "CSSStyleSheet", css_style_sheet_ctor);

    // CSSStyleSheet prototype
    GCValue css_style_sheet_proto = JS_NewObject(ctx);
    DEF_FUNC(ctx, css_style_sheet_proto, "insertRule", js_css_style_sheet_insert_rule, 2);
    DEF_FUNC(ctx, css_style_sheet_proto, "deleteRule", js_css_style_sheet_delete_rule, 1);
    DEF_FUNC(ctx, css_style_sheet_proto, "addRule", js_css_style_sheet_add_rule, 3);
    DEF_FUNC(ctx, css_style_sheet_proto, "removeRule", js_css_style_sheet_remove_rule, 1);
    DEF_FUNC(ctx, css_style_sheet_proto, "replace", js_css_style_sheet_replace, 1);
    DEF_FUNC(ctx, css_style_sheet_proto, "replaceSync", js_css_style_sheet_replace_sync, 1);
    GCValue css_rules_getter = JS_NewCFunction(ctx, js_css_style_sheet_get_css_rules, "get cssRules", 0);
    JS_DefinePropertyGetSet(ctx, css_style_sheet_proto, JS_NewAtom(ctx, "cssRules"),
        css_rules_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    GCValue rules_getter = JS_NewCFunction(ctx, js_css_style_sheet_get_rules, "get rules", 0);
    JS_DefinePropertyGetSet(ctx, css_style_sheet_proto, JS_NewAtom(ctx, "rules"),
        rules_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    GCValue css_text_getter = JS_NewCFunction(ctx, js_css_style_sheet_get_css_text, "get cssText", 0);
    JS_DefinePropertyGetSet(ctx, css_style_sheet_proto, JS_NewAtom(ctx, "cssText"),
        css_text_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    // Set prototype on constructor
    JS_SetPropertyStr(ctx, css_style_sheet_ctor, "prototype", css_style_sheet_proto);
    
    // CSSStyleDeclaration prototype (for element.style)
    GCValue css_style_decl_proto = JS_NewObject(ctx);
    DEF_FUNC(ctx, css_style_decl_proto, "setProperty", js_css_style_decl_set_property, 3);
    DEF_FUNC(ctx, css_style_decl_proto, "removeProperty", js_css_style_decl_remove_property, 1);
    DEF_FUNC(ctx, css_style_decl_proto, "getPropertyValue", js_css_style_decl_get_property_value, 1);
    DEF_FUNC(ctx, css_style_decl_proto, "getPropertyPriority", js_css_style_decl_get_property_priority, 1);
    DEF_FUNC(ctx, css_style_decl_proto, "item", js_css_style_decl_item, 1);
    GCValue style_css_text_getter = JS_NewCFunction(ctx, js_css_style_decl_get_css_text, "get cssText", 0);
    GCValue style_css_text_setter = JS_NewCFunction(ctx, js_css_style_decl_set_css_text, "set cssText", 1);
    JS_DefinePropertyGetSet(ctx, css_style_decl_proto, JS_NewAtom(ctx, "cssText"),
        style_css_text_getter, style_css_text_setter, JS_PROP_ENUMERABLE);
    GCValue style_length_getter = JS_NewCFunction(ctx, js_css_style_decl_get_length, "get length", 0);
    JS_DefinePropertyGetSet(ctx, css_style_decl_proto, JS_NewAtom(ctx, "length"),
        style_length_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);

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

    // queueMicrotask
    JS_SetPropertyStr(ctx, global, "queueMicrotask",
        JS_NewCFunction(ctx, js_queue_microtask, "queueMicrotask", 1));
    JS_SetPropertyStr(ctx, window, "queueMicrotask",
        JS_NewCFunction(ctx, js_queue_microtask, "queueMicrotask", 1));

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
        js_shadow_root_proto_funcs_count);
    
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
    GCValue ce_define = JS_NewCFunction(ctx, js_custom_elements_define, "define", 2);
    GCValue ce_get = JS_NewCFunction(ctx, js_custom_elements_get, "get", 1);
    GCValue ce_whenDefined = JS_NewCFunction(ctx, js_custom_elements_when_defined, "whenDefined", 1);
    JS_SetPropertyStr(ctx, custom_elements, "define", ce_define);
    JS_SetPropertyStr(ctx, custom_elements, "get", ce_get);
    JS_SetPropertyStr(ctx, custom_elements, "whenDefined", ce_whenDefined);
    // Keep the original C define/get reachable through the registry object and
    // replace the public properties with JS wrappers. The Polymer ES5 shim
    // captures these functions in closures; capturing the JS wrappers instead
    // avoids the compacting GC invalidating C-function references.
    JS_SetPropertyStr(ctx, custom_elements, "__origDefine", ce_define);
    JS_SetPropertyStr(ctx, custom_elements, "__origGet", ce_get);
    JS_SetPropertyStr(ctx, window, "customElements", custom_elements);
    JS_SetPropertyStr(ctx, global, "customElements", custom_elements);
    {
        const char *wrap_ce =
            "(function(){"
            "  var reg = window.customElements;"
            "  reg.define = function(name, ctor, options) {"
            "    return reg.__origDefine.call(this, name, ctor, options);"
            "  };"
            "  reg.get = function(name) {"
            "    return reg.__origGet.call(this, name);"
            "  };"
            "})();";
        JS_Eval(ctx, wrap_ce, strlen(wrap_ce), "<wrap_ce>", JS_EVAL_TYPE_GLOBAL);
    }
    // Intercept Object.defineProperty on customElements.define so the Polymer
    // ES5 shim's adapter wrapper is always guarded against undefined / arrow
    // constructors, even when scripts hold a reference to the function directly.
    {
        const char *patch_odp =
            "(function(){"
            "  window.__origObjectDefineProperty = Object.defineProperty;"
            "  Object.defineProperty = function(obj, prop, desc) {"
            "    if (obj === window.customElements && prop === 'define' && desc && typeof desc.value === 'function') {"
            "      var inner = desc.value;"
            "      desc.value = function(name, ctor, options) {"
            "        if (typeof ctor !== 'function' || !ctor.prototype) return;"
            "        try {"
            "          return inner.call(this, name, ctor, options);"
            "        } catch(e) {"
            "          return;"
            "        }"
            "      };"
            "    }"
            "    return window.__origObjectDefineProperty.call(this, obj, prop, desc);"
            "  };"
            "})();";
        JS_Eval(ctx, patch_odp, strlen(patch_odp), "<patch_odp>", JS_EVAL_TYPE_GLOBAL);
    }

    // Register the C custom-element upgrade helper. It pushes the element onto
    // an upgrade stack before `new ctor()`; the native HTMLElement constructor
    // pops the stack and returns the existing element as `this`.
    JS_SetPropertyStr(ctx, global, "__cyber_upgradeElement",
        JS_NewCFunction(ctx, js_cyber_upgrade_element, "__cyber_upgradeElement", 1));

    // JS wrappers that batch-upgrade elements and implement customElements.upgrade.
    {
        const char *upgrade_js =
            "(function(){"
            "  window.__cyber_upgradeAll = function(name) {"
            "    if (!window.customElements) return;"
            "    var ctor = window.customElements.get(name);"
            "    if (!ctor || !ctor.prototype) return;"
            "    var list = document.getElementsByTagName(name);"
            "    if (typeof console !== 'undefined' && console.error) try { console.error('[CE-UPGRADE] upgradeAll ' + name + ' found=' + list.length); } catch(x) {}"
            "    for (var i = 0; i < list.length; i++) {"
            "      var el = list[i];"
            "      if ((name === 'ytd-masthead' || name === 'ytd-app') && typeof console !== 'undefined' && console.error) {"
            "        try { console.error('[CE-UPGRADE] ' + name + ' id=' + (el.__cyber_id||'?') + ' parent=' + (el.parentNode && el.parentNode.tagName) + '#' + (el.parentNode && el.parentNode.id || '') + ' connected=' + el.isConnected); } catch(x) {}"
            "      }"
            "      window.__cyber_upgradeElement(list[i]);"
            "    }"
            "  };"
            "  window.customElements.upgrade = function(root) {"
            "    if (!root) return;"
            "    var walk = function(node) {"
            "      if (node.nodeType === 1) {"
            "        window.__cyber_upgradeElement(node);"
            "        var children = node.childNodes;"
            "        for (var i = 0; i < children.length; i++) walk(children[i]);"
            "      }"
            "    };"
            "    walk(root);"
            "  };"
            "})();";
        JS_Eval(ctx, upgrade_js, strlen(upgrade_js), "<ce_upgrade>", JS_EVAL_TYPE_GLOBAL);
    }

    // CustomElementRegistry constructor (for completeness)
    GCValue ce_registry_ctor = JS_NewCFunction2(ctx, js_dummy_function, "CustomElementRegistry",
        0, JS_CFUNC_constructor, 0);
    GCValue ce_registry_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ce_registry_proto, "constructor", ce_registry_ctor);
    JS_SetPropertyStr(ctx, ce_registry_ctor, "prototype", ce_registry_proto);
    JS_SetPropertyStr(ctx, global, "CustomElementRegistry", ce_registry_ctor);

    // ===== ShadyDOM API =====
    // YouTube's inline config (script 03) assigns a minimal stub to
    // window.ShadyDOM and expects the browser's webcomponents-sd polyfill to
    // replace it with the full API. The emulator skips the external bundle, so
    // we provide a native ShadyDOM implementation here. A getter/setter keeps
    // the API intact when the page config is applied.
    {
        const char *shady_dom_js =
            "(function(){"
            "  var stored = null;"
            "  function ensure() {"
            "    if (stored) return stored;"
            "    function Wrapper(node) {"
            "      if (!(this instanceof Wrapper)) return new Wrapper(node);"
            "      this.node = node;"
            "    }"
            "    function observeChildren(target, callback) {"
            "      if (!target || typeof MutationObserver === 'undefined') {"
            "        return { takeRecords: function(){ return []; }, disconnect: function(){} };"
            "      }"
            "      var mo = new MutationObserver(callback);"
            "      mo.observe(target, { childList: true, subtree: true });"
            "      return {"
            "        takeRecords: function() { return mo.takeRecords(); },"
            "        disconnect: function() { mo.disconnect(); }"
            "      };"
            "    }"
            "    function unobserveChildren(observer) {"
            "      if (observer && typeof observer.disconnect === 'function') observer.disconnect();"
            "    }"
            "    function composedPath(ev) {"
            "      if (ev && typeof ev.composedPath === 'function') return ev.composedPath();"
            "      return ev && ev.target ? [ev.target] : [];"
            "    }"
            "    stored = {"
            "      inUse: true,"
            "      noPatch: true,"
            "      preferPerformance: true,"
            "      force: true,"
            "      handlesDynamicScoping: true,"
            "      deferConnectionCallbacks: false,"
            "      settings: { noPatch: true, preferPerformance: true, force: true },"
            "      Wrapper: Wrapper,"
            "      wrap: function(node) { return node; },"
            "      wrapIfNeeded: function(node) { return node; },"
            "      patch: function(node) { return node; },"
            "      isShadyRoot: function(node) { return false; },"
            "      observeChildren: observeChildren,"
            "      unobserveChildren: unobserveChildren,"
            "      flush: function() { return false; },"
            "      flushInitial: function(root) {},"
            "      composedPath: composedPath,"
            "      enqueue: function(fn) { if (typeof fn === 'function') fn(); },"
            "      filterMutations: function(mutations, target) { return mutations; }"
            "    };"
            "    return stored;"
            "  }"
            "  function defineShadyDOM(obj) {"
            "    Object.defineProperty(obj, 'ShadyDOM', {"
            "      get: function() { return ensure(); },"
            "      set: function(v) {"
            "        var s = ensure();"
            "        if (v && typeof v === 'object') {"
            "          for (var k in v) {"
            "            if (k === 'Wrapper' && typeof v[k] !== 'function') continue;"
            "            s[k] = v[k];"
            "          }"
            "          s.settings = Object.assign({}, s.settings, v);"
            "        }"
            "      },"
            "      configurable: true"
            "    });"
            "  }"
            "  defineShadyDOM(window);"
            "  if (typeof globalThis !== 'undefined') defineShadyDOM(globalThis);"
            "})();";
        JS_Eval(ctx, shady_dom_js, strlen(shady_dom_js), "<shady_dom_api>", JS_EVAL_TYPE_GLOBAL);
    }

    // ===== YouTube logging stub =====
    // YouTube's error logger (_.$p) expects yt.logging.errors.log to be a
    // callable. Wrap any value assigned here so non-object errors and logging
    // failures do not produce secondary "not a function" timer warnings.
    {
        const char *yt_log_js =
            "(function(){"
            "  if (typeof window === 'undefined') return;"
            "  if (!window.yt) window.yt = {};"
            "  if (!window.yt.logging) window.yt.logging = {};"
            "  if (!window.yt.logging.errors) window.yt.logging.errors = {};"
            "  var actual = null;"
            "  function safeLog(err, severity, a, b, c, d, e) {"
            "    if (err == null || (typeof err !== 'object' && typeof err !== 'function')) {"
            "      err = new Error(String(err));"
            "    }"
            "    if (actual && typeof actual === 'function') {"
            "      try { return actual.call(this, err, severity, a, b, c, d, e); } catch(x) {}"
            "    }"
            "  }"
            "  Object.defineProperty(window.yt.logging.errors, 'log', {"
            "    get: function() { return actual || safeLog; },"
            "    set: function(v) { actual = (typeof v === 'function') ? v : null; },"
            "    configurable: true"
            "  });"
            "})();";
        JS_Eval(ctx, yt_log_js, strlen(yt_log_js), "<yt_log_stub>", JS_EVAL_TYPE_GLOBAL);
    }

    // ===== ParentNode / ChildNode convenience methods =====
    // Polymer's ShadyDOM patch loop (V6k) wraps before/after/replaceWith and
    // prepend/append. If these methods are missing, the wrappers call undefined
    // and later timer-driven DOM code throws "not a function".
    {
        const char *dom_methods_js =
            "(function(){"
            "  if (typeof Node === 'undefined') return;"
            "  function toNodes(args) {"
            "    var out = [];"
            "    for (var i = 0; i < args.length; i++) {"
            "      var n = args[i];"
            "      if (n == null) continue;"
            "      if (typeof n === 'string' || typeof n === 'number') {"
            "        out.push(document.createTextNode(String(n)));"
            "      } else if (n instanceof DocumentFragment) {"
            "        while (n.firstChild) out.push(n.removeChild(n.firstChild));"
            "      } else if (n instanceof Node) {"
            "        out.push(n);"
            "      }"
            "    }"
            "    return out;"
            "  }"
            "  function insertNodes(parent, nodes, ref) {"
            "    for (var i = 0; i < nodes.length; i++) parent.insertBefore(nodes[i], ref);"
            "  }"
            "  var childProto = Element.prototype;"
            "  if (!childProto.before) {"
            "    childProto.before = function() {"
            "      var p = this.parentNode; if (!p) return; insertNodes(p, toNodes(arguments), this);"
            "    };"
            "  }"
            "  if (!childProto.after) {"
            "    childProto.after = function() {"
            "      var p = this.parentNode; if (!p) return; insertNodes(p, toNodes(arguments), this.nextSibling);"
            "    };"
            "  }"
            "  if (!childProto.replaceWith) {"
            "    childProto.replaceWith = function() {"
            "      var p = this.parentNode; if (!p) return;"
            "      var nodes = toNodes(arguments); insertNodes(p, nodes, this); p.removeChild(this);"
            "    };"
            "  }"
            "  if (!childProto.remove) {"
            "    childProto.remove = function() { if (this.parentNode) this.parentNode.removeChild(this); };"
            "  }"
            "  var parentProtos = [Document.prototype, DocumentFragment.prototype, Element.prototype];"
            "  if (typeof ShadowRoot !== 'undefined') parentProtos.push(ShadowRoot.prototype);"
            "  for (var i = 0; i < parentProtos.length; i++) {"
            "    var proto = parentProtos[i]; if (!proto) continue;"
            "    (function(p){"
            "      if (!p.append) p.append = function() {"
            "        var nodes = toNodes(arguments); for (var j = 0; j < nodes.length; j++) this.appendChild(nodes[j]);"
            "      };"
            "      if (!p.prepend) p.prepend = function() {"
            "        var nodes = toNodes(arguments); insertNodes(this, nodes, this.firstChild);"
            "      };"
            "    })(proto);"
            "  }"
            "})();";
        JS_Eval(ctx, dom_methods_js, strlen(dom_methods_js), "<dom_methods>", JS_EVAL_TYPE_GLOBAL);
    }

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
            js_animation_proto_funcs_count);
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
        js_keyframe_effect_proto_funcs_count);
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
        js_font_face_proto_funcs_count);
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
        js_font_face_set_proto_funcs_count);
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
        js_mutation_observer_proto_funcs_count);
    JS_SetClassProto(ctx, js_mutation_observer_class_id, mutation_observer_proto);
    GCValue mutation_observer_ctor = JS_NewCFunction2(ctx, js_mutation_observer_constructor, "MutationObserver",
        1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, mutation_observer_ctor, mutation_observer_proto);
    JS_SetPropertyStr(ctx, global, "MutationObserver", mutation_observer_ctor);

    // ResizeObserver
    GCValue resize_observer_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, resize_observer_proto, js_resize_observer_proto_funcs,
        js_resize_observer_proto_funcs_count);
    JS_SetClassProto(ctx, js_resize_observer_class_id, resize_observer_proto);
    GCValue resize_observer_ctor = JS_NewCFunction2(ctx, js_resize_observer_constructor, "ResizeObserver",
        1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, resize_observer_ctor, resize_observer_proto);
    JS_SetPropertyStr(ctx, global, "ResizeObserver", resize_observer_ctor);

    // IntersectionObserver
    GCValue intersection_observer_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, intersection_observer_proto, js_intersection_observer_proto_funcs,
        js_intersection_observer_proto_funcs_count);
    JS_SetClassProto(ctx, js_intersection_observer_class_id, intersection_observer_proto);
    GCValue intersection_observer_ctor = JS_NewCFunction2(ctx, js_intersection_observer_constructor, "IntersectionObserver",
        1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, intersection_observer_ctor, intersection_observer_proto);
    JS_SetPropertyStr(ctx, global, "IntersectionObserver", intersection_observer_ctor);

    // ===== Performance API =====
    // PerformanceEntry class
    GCValue performance_entry_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, performance_entry_proto, js_performance_entry_proto_funcs,
        js_performance_entry_proto_funcs_count);
    JS_SetClassProto(ctx, js_performance_entry_class_id, performance_entry_proto);
    GCValue performance_entry_ctor = JS_NewCFunction2(ctx, js_dummy_function, "PerformanceEntry",
        0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, performance_entry_ctor, performance_entry_proto);
    JS_SetPropertyStr(ctx, global, "PerformanceEntry", performance_entry_ctor);

    // PerformanceObserver class
    GCValue performance_observer_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, performance_observer_proto, js_performance_observer_proto_funcs,
        js_performance_observer_proto_funcs_count);
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
        js_performance_proto_funcs_count);
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
        js_dom_rect_read_only_proto_funcs_count);
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
        js_dom_rect_proto_funcs_count);
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
        js_date_proto_funcs_count);
    JS_SetClassProto(ctx, js_date_class_id, date_proto);
    GCValue date_ctor = JS_NewCFunction2(ctx, js_date_constructor, "Date",
        7, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, date_ctor, date_proto);
    // Add static methods
    JS_SetPropertyFunctionList(ctx, date_ctor, js_date_static_funcs,
        js_date_static_funcs_count);
    JS_SetPropertyStr(ctx, global, "Date", date_ctor);

    // ===== MediaSource API =====
    // SourceBuffer class (needed by MediaSource)
    GCValue source_buffer_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, source_buffer_proto, js_source_buffer_proto_funcs,
        js_source_buffer_proto_funcs_count);
    JS_SetClassProto(ctx, js_source_buffer_class_id, source_buffer_proto);
    GCValue source_buffer_ctor = JS_NewCFunction2(ctx, js_source_buffer_constructor, "SourceBuffer",
        0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, source_buffer_ctor, source_buffer_proto);
    JS_SetPropertyStr(ctx, global, "SourceBuffer", source_buffer_ctor);
    
    // MediaSource class
    GCValue media_source_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, media_source_proto, js_media_source_proto_funcs,
        js_media_source_proto_funcs_count);
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
    
    // URL.prototype.searchParams getter
    JSAtom search_params_atom = JS_NewAtom(ctx, "searchParams");
    JS_DefinePropertyGetSet(ctx, url_proto, search_params_atom,
        JS_NewCFunction(ctx, js_url_get_search_params, "get searchParams", 0),
        JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, search_params_atom);
    
    // URLSearchParams constructor and prototype
    GCValue usp_ctor = JS_NewCFunction2(ctx, js_url_search_params_constructor, "URLSearchParams", 1, JS_CFUNC_constructor, 0);
    GCValue usp_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, usp_proto, "constructor", usp_ctor);
    JS_SetConstructor(ctx, usp_ctor, usp_proto);
    JS_SetConstructorBit(ctx, usp_ctor, TRUE);
    JS_SetPropertyStr(ctx, usp_proto, "append",
        JS_NewCFunction(ctx, js_url_search_params_append, "append", 2));
    JS_SetPropertyStr(ctx, usp_proto, "delete",
        JS_NewCFunction(ctx, js_url_search_params_delete, "delete", 1));
    JS_SetPropertyStr(ctx, usp_proto, "get",
        JS_NewCFunction(ctx, js_url_search_params_get, "get", 1));
    JS_SetPropertyStr(ctx, usp_proto, "getAll",
        JS_NewCFunction(ctx, js_url_search_params_get_all, "getAll", 1));
    JS_SetPropertyStr(ctx, usp_proto, "has",
        JS_NewCFunction(ctx, js_url_search_params_has, "has", 1));
    JS_SetPropertyStr(ctx, usp_proto, "set",
        JS_NewCFunction(ctx, js_url_search_params_set, "set", 2));
    JS_SetPropertyStr(ctx, usp_proto, "toString",
        JS_NewCFunction(ctx, js_url_search_params_to_string, "toString", 0));
    JS_SetPropertyStr(ctx, usp_proto, "entries",
        JS_NewCFunction(ctx, js_url_search_params_entries, "entries", 0));
    JS_SetPropertyStr(ctx, usp_proto, "keys",
        JS_NewCFunction(ctx, js_url_search_params_keys, "keys", 0));
    JS_SetPropertyStr(ctx, usp_proto, "values",
        JS_NewCFunction(ctx, js_url_search_params_values, "values", 0));
    JS_SetPropertyStr(ctx, global, "URLSearchParams", usp_ctor);
    JS_SetPropertyStr(ctx, window, "URLSearchParams", usp_ctor);
    
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

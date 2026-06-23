/* Auto-generated split from browser_api_impl.cpp */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
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

/* ============================================================================
 * Console API Implementation
 * ============================================================================ */

// Global console data for timers, counters, and groups
static ConsoleData g_console_data = {0};

// Helper to format a single value for console output
void console_format_value(JSContextHandle ctx, GCValue val, char *out_buf, size_t out_len) {
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
void console_format_args(JSContextHandle ctx, int argc, GCValue *argv, char *out_buf, size_t out_len) {
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
double console_get_time_ms(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
double console_get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
#endif

// console.log(...args)
GCValue js_console_log(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
GCValue js_console_warn(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    char msg[4096];
    console_format_args(ctx, argc, argv, msg, sizeof(msg));
    platform_log(LOG_LEVEL_WARN, "console", "%s", msg);
    return JS_UNDEFINED;
}

// console.error(...args)
GCValue js_console_error(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    char msg[4096];
    console_format_args(ctx, argc, argv, msg, sizeof(msg));
    platform_log(LOG_LEVEL_ERROR, "console", "%s", msg);
    return JS_UNDEFINED;
}

// console.info(...args)
GCValue js_console_info(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    char msg[4096];
    console_format_args(ctx, argc, argv, msg, sizeof(msg));
    platform_log(LOG_LEVEL_INFO, "console", "%s", msg);
    return JS_UNDEFINED;
}

// console.debug(...args)
GCValue js_console_debug(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    char msg[4096];
    console_format_args(ctx, argc, argv, msg, sizeof(msg));
    platform_log(LOG_LEVEL_INFO, "console", "%s", msg);
    return JS_UNDEFINED;
}

// console.trace(...args)
GCValue js_console_trace(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    char msg[4096];
    console_format_args(ctx, argc, argv, msg, sizeof(msg));
    platform_log(LOG_LEVEL_INFO, "console", "Trace: %s", msg);
    // Note: Full stack trace would require QuickJS stack inspection
    return JS_UNDEFINED;
}

// console.dir(obj) - display object properties (simplified)
GCValue js_console_dir(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
GCValue js_console_dirxml(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    
    char msg[1024];
    console_format_value(ctx, argv[0], msg, sizeof(msg));
    platform_log(LOG_LEVEL_INFO, "console", "<Element: %s>", msg);
    return JS_UNDEFINED;
}

// console.group(label)
GCValue js_console_group(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
GCValue js_console_groupCollapsed(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
GCValue js_console_groupEnd(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (g_console_data.group_depth > 0) {
        g_console_data.group_depth--;
    }
    return JS_UNDEFINED;
}

// console.time(label)
GCValue js_console_time(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
GCValue js_console_timeEnd(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
GCValue js_console_timeLog(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
GCValue js_console_count(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
GCValue js_console_countReset(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
GCValue js_console_assert(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
GCValue js_console_clear(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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

/* getPropertyValue for a computed-style object. Reads from the object's own
 * properties (which are populated below), converting kebab-case names to
 * camelCase as a fallback. */
static GCValue js_computed_style_get_property_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NewString(ctx, "");
    const char *prop = JS_ToCString(ctx, argv[0]);
    if (!prop || !prop[0]) return JS_NewString(ctx, "");

    GCValue val = JS_GetPropertyStr(ctx, this_val, prop);
    if (!JS_IsUndefined(val) && !JS_IsNull(val)) return val;

    char *camel = css_to_camel_case(prop);
    if (camel) {
        val = JS_GetPropertyStr(ctx, this_val, camel);
        if (!JS_IsUndefined(val) && !JS_IsNull(val)) {
            free(camel);
            return val;
        }
        free(camel);
    }
    return JS_NewString(ctx, "");
}

// getComputedStyle - reads from the per-element computed-style table and
// falls back to sensible defaults so scripts can safely call .replace() etc.
GCValue js_get_computed_style(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    GCValue element = argc > 0 ? argv[0] : JS_UNDEFINED;
    GCValue pseudo = argc > 1 ? argv[1] : JS_UNDEFINED;
    (void)pseudo;

    GCValue style = JS_NewObject(ctx);
    if (JS_IsException(style)) return style;

    /* Provide defaults for properties that large minified bundles read
     * directly. This prevents non-fatal TypeErrors such as
     * "cannot read property 'replace' of undefined". */
    static const char *default_props[][2] = {
        {"fontSize", "16px"},
        {"color", "rgb(0, 0, 0)"},
        {"backgroundColor", "rgba(0, 0, 0, 0)"},
        {"display", "block"},
        {"visibility", "visible"},
        {"overflow", "visible"},
        {"overflowX", "visible"},
        {"overflowY", "visible"},
        {"direction", "ltr"},
        {"zIndex", "auto"},
        {"lineHeight", "normal"},
        {"opacity", "1"},
        {"transitionDuration", "0s"},
        {"width", "auto"},
        {"height", "auto"},
        {"marginTop", "0px"},
        {"marginRight", "0px"},
        {"marginBottom", "0px"},
        {"marginLeft", "0px"},
        {"paddingTop", "0px"},
        {"paddingRight", "0px"},
        {"paddingBottom", "0px"},
        {"paddingLeft", "0px"},
        {"border", "0px none rgb(0, 0, 0)"},
        {"position", "static"},
        {"top", "auto"},
        {"left", "auto"},
        {"right", "auto"},
        {"bottom", "auto"},
        {"transform", "none"},
        {"pointerEvents", "auto"},
        {"whiteSpace", "normal"},
        {"textAlign", "start"},
        {"float", "none"},
        {"clear", "none"},
        {"boxSizing", "content-box"},
        {"fontFamily", "\"Times New Roman\""},
        {"fontWeight", "400"},
        {"letterSpacing", "normal"},
        {"wordSpacing", "normal"},
        {"verticalAlign", "baseline"},
        {"cursor", "auto"},
    };
    for (size_t i = 0; i < sizeof(default_props)/sizeof(default_props[0]); i++) {
        JS_SetPropertyStr(ctx, style, default_props[i][0],
                          JS_NewString(ctx, default_props[i][1]));
    }

    JS_SetPropertyStr(ctx, style, "getPropertyValue",
        JS_NewCFunction(ctx, js_computed_style_get_property_value, "getPropertyValue", 1));

    DOMNodeHandle node = DOMNodeHandle::from_object_check(ctx, element);
    if (node.valid()) {
        /* Override defaults with the actual computed properties stored for
         * this node (the table contains both kebab-case and camelCase keys). */
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
    }
    return style;
}


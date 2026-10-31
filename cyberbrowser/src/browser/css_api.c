/* Auto-generated split from browser_api_impl.c */
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
#include "browser_api_impl_handle_helpers.h"
#include "browser_api_impl_internal.h"
#include "html_dom.h"
#include "dom_js.h"
#include "css_parser.h"
#include "css_props.h"
#include "gc_value_helpers.h"
#include "platform.h"


/* ============================================================================
 * CSS API Implementation
 * ============================================================================ */

// Forward declaration for DOMTokenList.contains

// CSS.supports(property, value) / CSS.supports(conditionText)
GCValue js_css_supports(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'supports' on 'CSS': 1 argument required");
    const char *a = JS_ToCString(ctx, argv[0]);
    if (!a) return JS_EXCEPTION;
    if (argc < 2) {
        /* a <supports-condition>, or a bare declaration (as browsers take it) */
        if (css_supports_condition(a)) return JS_TRUE;
        size_t n = strlen(a) + 3;
        char *wrapped = malloc(n);
        if (!wrapped) return JS_FALSE;
        snprintf(wrapped, n, "(%s)", a);
        bool ok = css_supports_condition(wrapped);
        free(wrapped);
        return JS_NewBool(ctx, ok);
    }
    const char *v = JS_ToCString(ctx, argv[1]);
    if (!v) return JS_EXCEPTION;
    return JS_NewBool(ctx, css_supports_property(a, v));
}

// CSS.escape(value)
GCValue js_css_escape(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    
    const char *input = JS_ToCString(ctx, argv[0]);
    if (!input) return JS_NewString(ctx, "");
    
    // Simple escape - just return the input for now
    // Real implementation would escape special CSS characters
    return JS_NewString(ctx, input);
}

// CSSStyleSheet.insertRule(rule, index)
GCValue js_css_style_sheet_insert_rule(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    dom_request_relayout();   /* forced layout re-checks the sheets */
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
GCValue js_css_style_sheet_delete_rule(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    dom_request_relayout();   /* forced layout re-checks the sheets */
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
GCValue js_css_style_sheet_add_rule(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    dom_request_relayout();   /* forced layout re-checks the sheets */
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
GCValue js_css_style_sheet_remove_rule(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    return js_css_style_sheet_delete_rule(ctx, this_val, argc, argv);
}

static GCValue css_style_sheet_rules(JSContextHandle ctx, GCValue sheet) {
    GCValue rules = JS_GetPropertyStr(ctx, sheet, "__rules");
    if (!JS_IsArray(ctx, rules)) {
        rules = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, sheet, "__rules", rules);
    }
    return rules;
}

GCValue js_css_style_sheet_get_css_rules(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return css_style_sheet_rules(ctx, this_val);
}

GCValue js_css_style_sheet_get_rules(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return css_style_sheet_rules(ctx, this_val);
}

// CSSStyleSheet.replace(text)
GCValue js_css_style_sheet_replace(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    dom_request_relayout();   /* forced layout re-checks the sheets */
    // Synchronously parse the text and return a resolved promise.
    js_css_style_sheet_replace_sync(ctx, this_val, argc, argv);
    return js_create_empty_resolved_promise(ctx);
}

// CSSStyleSheet.replaceSync(text)
GCValue js_css_style_sheet_replace_sync(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    dom_request_relayout();   /* forced layout re-checks the sheets */
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;

    const char *text = JS_ToCString(ctx, argv[0]);
    if (!text) text = "";
    JS_SetPropertyStr(ctx, this_val, "__cssText", JS_NewString(ctx, text));

    GCValue rules = JS_NewArray(ctx);
    JS_SetPropertyStr(ctx, this_val, "__rules", rules);

    // Split top-level rules by matching braces as a basic parser.
    size_t len = strlen(text);
    size_t start = 0;
    int depth = 0;
    uint32_t ridx = 0;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                size_t end = i + 1;
                while (start < len && (text[start] == ' ' || text[start] == '\t' || text[start] == '\n' || text[start] == '\r')) start++;
                while (end > start && (text[end-1] == ' ' || text[end-1] == '\t' || text[end-1] == '\n' || text[end-1] == '\r')) end--;
                if (end > start) {
                    char *rule = (char*)malloc(end - start + 1);
                    if (rule) {
                        memcpy(rule, text + start, end - start);
                        rule[end - start] = '\0';
                        JS_SetPropertyUint32(ctx, rules, ridx++, JS_NewString(ctx, rule));
                        free(rule);
                    }
                }
                start = i + 1;
            }
        }
    }

    return JS_UNDEFINED;
}

// Return the full CSS text for a CSSStyleSheet object.
GCValue js_css_style_sheet_get_css_text(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue sheet = this_val;
    GCValue css_text = JS_GetPropertyStr(ctx, sheet, "__cssText");
    if (JS_IsString(css_text)) {
        return css_text;
    }
    GCValue rules = JS_GetPropertyStr(ctx, sheet, "__rules");
    if (!JS_IsArray(ctx, rules)) {
        return JS_NewString(ctx, "");
    }
    GCValue len_val = JS_GetPropertyStr(ctx, rules, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, len_val);
    if (len == 0) return JS_NewString(ctx, "");
    // Approximate size.
    size_t cap = 256;
    for (uint32_t i = 0; i < len; i++) {
        GCValue r = JS_GetPropertyUint32(ctx, rules, i);
        const char *s = JS_ToCString(ctx, r);
        if (s) cap += strlen(s) + 2;
    }
    char *buf = (char*)malloc(cap);
    if (!buf) return JS_NewString(ctx, "");
    buf[0] = '\0';
    size_t pos = 0;
    for (uint32_t i = 0; i < len; i++) {
        GCValue r = JS_GetPropertyUint32(ctx, rules, i);
        const char *s = JS_ToCString(ctx, r);
        if (s && s[0]) {
            size_t l = strlen(s);
            if (pos + l + 3 > cap) {
                cap = pos + l + 256;
                buf = (char*)realloc(buf, cap);
                if (!buf) return JS_NewString(ctx, "");
            }
            memcpy(buf + pos, s, l);
            pos += l;
            buf[pos++] = '\n';
            buf[pos] = '\0';
        }
    }
    GCValue result = JS_NewString(ctx, buf);
    free(buf);
    return result;
}

// CSSStyleSheet constructor
GCValue js_css_style_sheet_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) return obj;
    GCValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (!JS_IsException(proto) && JS_IsObject(proto)) {
        JS_SetPrototype(ctx, obj, proto);
    }
    JS_SetPropertyStr(ctx, obj, "__rules", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, "text/css"));
    JS_SetPropertyStr(ctx, obj, "href", JS_NULL);
    JS_SetPropertyStr(ctx, obj, "ownerNode", JS_NULL);
    JS_SetPropertyStr(ctx, obj, "ownerRule", JS_NULL);
    JS_SetPropertyStr(ctx, obj, "title", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, obj, "disabled", JS_FALSE);
    return obj;
}

// CSSStyleDeclaration.setProperty(property, value, priority)
GCValue js_css_style_decl_set_property(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
GCValue js_css_style_decl_remove_property(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
GCValue js_css_style_decl_get_property_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
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
GCValue js_css_style_decl_get_property_priority(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    // Return empty string - no !important support in stub
    return JS_NewString(ctx, "");
}

static bool js_css_style_prop_name_valid(const char *name) {
    if (!name || !name[0] || name[0] == '_') return false;
    for (const char *p = name; *p; p++) {
        if (isupper((unsigned char)*p)) return false;
    }
    return true;
}

// Helper: iterate own string properties and call cb with name/value.
static void js_css_style_enum_props(JSContextHandle ctx, GCValue style,
                                    void (*cb)(const char *name, const char *value, void *userdata),
                                    void *userdata) {
    JSPropertyEnum *props = NULL;
    uint32_t prop_count = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, style, JS_GPN_STRING_MASK) != 0 || !props) {
        return;
    }
    for (uint32_t i = 0; i < prop_count; i++) {
        const char *name = JS_AtomToCString(ctx, props[i].atom);
        if (!name || !js_css_style_prop_name_valid(name)) continue;
        GCValue val = JS_GetPropertyStr(ctx, style, name);
        if (!JS_IsString(val)) continue;
        const char *value = JS_ToCString(ctx, val);
        if (value && value[0]) {
            cb(name, value, userdata);
        }
    }
    JS_FreePropertyEnum(ctx, props, prop_count);
}

// CSSStyleDeclaration.cssText getter
GCValue js_css_style_decl_get_css_text(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    size_t cap = 256;
    char *buf = (char*)malloc(cap);
    if (!buf) return JS_NewString(ctx, "");
    buf[0] = '\0';
    size_t pos = 0;

    JSPropertyEnum *props = NULL;
    uint32_t prop_count = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, this_val, JS_GPN_STRING_MASK) == 0 && props) {
        for (uint32_t i = 0; i < prop_count; i++) {
            const char *name = JS_AtomToCString(ctx, props[i].atom);
            if (!name || !js_css_style_prop_name_valid(name)) continue;
            GCValue val = JS_GetPropertyStr(ctx, this_val, name);
            if (!JS_IsString(val)) continue;
            const char *value = JS_ToCString(ctx, val);
            if (!value || !value[0]) continue;

            size_t nl = strlen(name);
            size_t vl = strlen(value);
            size_t need = pos + nl + vl + 4;
            if (need > cap) {
                cap = need + 256;
                buf = (char*)realloc(buf, cap);
                if (!buf) return JS_NewString(ctx, "");
            }
            memcpy(buf + pos, name, nl); pos += nl;
            buf[pos++] = ':'; buf[pos++] = ' ';
            memcpy(buf + pos, value, vl); pos += vl;
            buf[pos++] = ';'; buf[pos++] = ' ';
        }
        JS_FreePropertyEnum(ctx, props, prop_count);
    }

    if (pos >= 2) pos -= 2; // trim trailing "; "
    buf[pos] = '\0';
    GCValue result = JS_NewString(ctx, buf);
    free(buf);
    return result;
}

// CSSStyleDeclaration.cssText setter
GCValue js_css_style_decl_set_css_text(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const char *text = JS_ToCString(ctx, argv[0]);
    if (!text) text = "";

    // Clear existing own string properties (kebab-case only; camelCase aliases
    // are regenerated by the parser).
    JSPropertyEnum *props = NULL;
    uint32_t prop_count = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, this_val, JS_GPN_STRING_MASK) == 0 && props) {
        for (uint32_t i = 0; i < prop_count; i++) {
            const char *name = JS_AtomToCString(ctx, props[i].atom);
            if (!name || name[0] == '_') continue;
            GCValue val = JS_GetPropertyStr(ctx, this_val, name);
            if (JS_IsString(val)) {
                /* Unset properties read as "" (CSSOM), not undefined. */
                JS_SetPropertyStr(ctx, this_val, name, JS_NewString(ctx, ""));
            }
        }
        JS_FreePropertyEnum(ctx, props, prop_count);
    }

    GCValue element = JS_GetPropertyStr(ctx, this_val, "__element");
    if (JS_IsObject(element)) {
        css_apply_inline_style_string(ctx, element, text);
    } else {
        int count = 0;
        CssDeclaration *decls = css_parse_inline_style(text, &count);
        if (decls) {
            for (int i = 0; i < count; i++) {
                css_set_style_property(ctx, this_val, decls[i].property, decls[i].value);
            }
            css_declarations_free(decls, count);
        }
    }
    return JS_UNDEFINED;
}

// CSSStyleDeclaration.length getter
GCValue js_css_style_decl_get_length(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    uint32_t count = 0;
    JSPropertyEnum *props = NULL;
    uint32_t prop_count = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, this_val, JS_GPN_STRING_MASK) == 0 && props) {
        for (uint32_t i = 0; i < prop_count; i++) {
            const char *name = JS_AtomToCString(ctx, props[i].atom);
            if (!name || !js_css_style_prop_name_valid(name)) continue;
            GCValue val = JS_GetPropertyStr(ctx, this_val, name);
            if (JS_IsString(val)) {
                const char *s = JS_ToCString(ctx, val);
                if (s && s[0]) count++;
            }
        }
        JS_FreePropertyEnum(ctx, props, prop_count);
    }
    return JS_NewInt32(ctx, (int32_t)count);
}

// CSSStyleDeclaration.item(index)
GCValue js_css_style_decl_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    int32_t index = 0;
    JS_ToInt32(ctx, &index, argv[0]);
    if (index < 0) return JS_NewString(ctx, "");

    JSPropertyEnum *props = NULL;
    uint32_t prop_count = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, this_val, JS_GPN_STRING_MASK) == 0 && props) {
        uint32_t seen = 0;
        for (uint32_t i = 0; i < prop_count; i++) {
            const char *name = JS_AtomToCString(ctx, props[i].atom);
            if (!name || !js_css_style_prop_name_valid(name)) continue;
            GCValue val = JS_GetPropertyStr(ctx, this_val, name);
            if (!JS_IsString(val)) continue;
            const char *s = JS_ToCString(ctx, val);
            if (!s || !s[0]) continue;
            if (seen == (uint32_t)index) {
                JS_FreePropertyEnum(ctx, props, prop_count);
                return JS_NewString(ctx, name);
            }
            seen++;
        }
        JS_FreePropertyEnum(ctx, props, prop_count);
    }
    return JS_NewString(ctx, "");
}

JSAtom dom_iterator_atom(JSContextHandle ctx) {
    static JSAtom atom = JS_ATOM_NULL;
    static uint32_t serial;     /* (the context it was made in: atoms are its runtime's) */
    if (atom == JS_ATOM_NULL || serial != JS_GetContextSerial(ctx)) {
        GCValue sym = JS_Eval(ctx, "Symbol.iterator", 15, "<symbol>", JS_EVAL_TYPE_GLOBAL);
        atom = JS_ValueToAtom(ctx, sym);
        serial = JS_GetContextSerial(ctx);
    }
    return atom;
}

/* ============================================================================
 * ServiceWorker API Implementation
 * ============================================================================ */

// Forward declarations for ServiceWorker

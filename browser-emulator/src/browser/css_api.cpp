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
 * CSS API Implementation
 * ============================================================================ */

// Forward declaration for DOMTokenList.contains
GCValue js_dom_token_list_contains(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

// CSS.supports(property, value)
GCValue js_css_supports(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_FALSE;
    
    // Always return true for simplicity - actual CSS support checking is complex
    return JS_TRUE;
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

// CSSStyleSheet.replace(text)
GCValue js_css_style_sheet_replace(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    // Returns a Promise - simplified to resolve immediately
    return JS_UNDEFINED;
}

// CSSStyleSheet.replaceSync(text)
GCValue js_css_style_sheet_replace_sync(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    
    // Replace all rules
    GCValue rules = JS_NewArray(ctx);
    JS_SetPropertyStr(ctx, this_val, "__rules", rules);
    
    // Parse and add rules (simplified)
    const char *text = JS_ToCString(ctx, argv[0]);
    if (text) {
        // Just store as one rule for simplicity
        JS_SetPropertyUint32(ctx, rules, 0, JS_NewString(ctx, text));
    }
    
    return JS_UNDEFINED;
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

// DOMTokenList.add(...tokens)
GCValue js_dom_token_list_add(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    
    // Get tokens array
    GCValue tokens = JS_GetPropertyStr(ctx, this_val, "__tokens");
    if (!JS_IsArray(ctx, tokens)) {
        tokens = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, this_val, "__tokens", tokens);
    }
    
    // Add each token
    for (int i = 0; i < argc; i++) {
        if (JS_IsString(argv[i])) {
            const char *token = JS_ToCString(ctx, argv[i]);
            // Check if already exists
            GCValue exists = js_dom_token_list_contains(ctx, this_val, 1, &argv[i]);
            int has_token = JS_ToBool(ctx, exists);
            if (!has_token) {
                // Get length and append
                GCValue len_val = JS_GetPropertyStr(ctx, tokens, "length");
                int len;
                JS_ToInt32(ctx, &len, len_val);
                JS_SetPropertyUint32(ctx, tokens, len, JS_NewString(ctx, token ? token : ""));
            }
        }
    }
    
    return JS_UNDEFINED;
}

// DOMTokenList.remove(...tokens)
GCValue js_dom_token_list_remove(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    
    GCValue tokens = JS_GetPropertyStr(ctx, this_val, "__tokens");
    if (!JS_IsArray(ctx, tokens)) return JS_UNDEFINED;
    
    // Get current length
    GCValue len_val = JS_GetPropertyStr(ctx, tokens, "length");
    int len;
    JS_ToInt32(ctx, &len, len_val);
    
    // Remove each token
    for (int i = 0; i < argc; i++) {
        if (!JS_IsString(argv[i])) continue;
        const char *token = JS_ToCString(ctx, argv[i]);
        if (!token) continue;
        
        // Find and remove
        for (int j = 0; j < len; j++) {
            GCValue item = JS_GetPropertyUint32(ctx, tokens, j);
            const char *item_str = JS_ToCString(ctx, item);
            if (item_str && strcmp(item_str, token) == 0) {
                // Shift remaining elements
                for (int k = j; k < len - 1; k++) {
                    GCValue next = JS_GetPropertyUint32(ctx, tokens, k + 1);
                    JS_SetPropertyUint32(ctx, tokens, k, next);
                }
                JS_SetPropertyUint32(ctx, tokens, len - 1, JS_UNDEFINED);
                len--;
                j--;  // Check same index again
                break;
            }
        }
    }
    
    return JS_UNDEFINED;
}

// DOMTokenList.contains(token) - defined before toggle
GCValue js_dom_token_list_contains(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_FALSE;
    if (!JS_IsString(argv[0])) return JS_FALSE;
    
    const char *token = JS_ToCString(ctx, argv[0]);
    if (!token) return JS_FALSE;
    
    GCValue tokens = JS_GetPropertyStr(ctx, this_val, "__tokens");
    if (!JS_IsArray(ctx, tokens)) return JS_FALSE;
    
    // Get length
    GCValue len_val = JS_GetPropertyStr(ctx, tokens, "length");
    int len;
    JS_ToInt32(ctx, &len, len_val);
    
    // Search for token
    for (int i = 0; i < len; i++) {
        GCValue item = JS_GetPropertyUint32(ctx, tokens, i);
        const char *item_str = JS_ToCString(ctx, item);
        if (item_str && strcmp(item_str, token) == 0) {
            return JS_NewBool(ctx, 1);
        }
    }
    
    return JS_NewBool(ctx, 0);
}

// DOMTokenList.toggle(token, force)
GCValue js_dom_token_list_toggle(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewBool(ctx, 0);
    
    int force = -1;  // -1 means auto
    if (argc >= 2) {
        int bool_val = JS_ToBool(ctx, argv[1]);
        if (bool_val >= 0) {
            force = bool_val ? 1 : 0;
        }
    }
    
    // Check if token exists
    GCValue exists = js_dom_token_list_contains(ctx, this_val, 1, argv);
    int has_token = JS_ToBool(ctx, exists);
    
    if (force == 1 || (force == -1 && !has_token)) {
        // Add token
        js_dom_token_list_add(ctx, this_val, 1, argv);
        return JS_NewBool(ctx, 1);
    } else if (force == 0 || (force == -1 && has_token)) {
        // Remove token
        js_dom_token_list_remove(ctx, this_val, 1, argv);
        return JS_NewBool(ctx, 0);
    }
    
    return JS_NewBool(ctx, has_token);
}

// DOMTokenList.forEach(callback)
GCValue js_dom_token_list_for_each(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
    
    GCValue tokens = JS_GetPropertyStr(ctx, this_val, "__tokens");
    if (!JS_IsArray(ctx, tokens)) return JS_UNDEFINED;
    
    // Get length
    GCValue len_val = JS_GetPropertyStr(ctx, tokens, "length");
    int len;
    JS_ToInt32(ctx, &len, len_val);
    
    // Call callback for each token
    GCValue callback = argv[0];
    for (int i = 0; i < len; i++) {
        GCValue item = JS_GetPropertyUint32(ctx, tokens, i);
        GCValue args[3] = { item, JS_NewInt32(ctx, i), this_val };
        JS_Call(ctx, callback, JS_UNDEFINED, 3, args);
    }
    
    return JS_UNDEFINED;
}

/* ============================================================================
 * ServiceWorker API Implementation
 * ============================================================================ */

// Forward declarations for ServiceWorker

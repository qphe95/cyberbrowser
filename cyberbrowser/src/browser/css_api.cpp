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
    // Synchronously parse the text and return a resolved promise.
    js_css_style_sheet_replace_sync(ctx, this_val, argc, argv);
    return js_create_empty_resolved_promise(ctx);
}

// CSSStyleSheet.replaceSync(text)
GCValue js_css_style_sheet_replace_sync(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;

    GCValue rules = JS_NewArray(ctx);
    JS_SetPropertyStr(ctx, this_val, "__rules", rules);

    const char *text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_UNDEFINED;

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

/* Helpers: DOMTokenList is backed by the associated element's className. */

static GCValue token_list_get_element(JSContextHandle ctx, GCValue this_val) {
    return JS_GetPropertyStr(ctx, this_val, "__element");
}

static char* token_list_get_class_name(JSContextHandle ctx, GCValue elem) {
    GCValue class_val = JS_GetPropertyStr(ctx, elem, "className");
    if (JS_IsUndefined(class_val) || JS_IsNull(class_val)) {
        return strdup("");
    }
    const char *s = JS_ToCString(ctx, class_val);
    return strdup(s ? s : "");
}

static void token_list_set_class_name(JSContextHandle ctx, GCValue elem, const char *class_name) {
    const char *new_val = class_name ? class_name : "";
    JS_SetPropertyStr(ctx, elem, "className", JS_NewString(ctx, new_val));
    DOMNodeHandle node = DOMNodeHandle::from_object(elem);
    if (node.valid()) {
        node.set_class_name(new_val);
        css_index_insert_node(ctx, node);
    }
}

/* Parse className into tokens. Writes up to max_tokens tokens into tokens_out.
 * Returns the actual token count. */
static int token_list_parse(const char *class_name, char **tokens_out, int max_tokens) {
    if (!class_name || !class_name[0]) return 0;
    char *copy = strdup(class_name);
    if (!copy) return 0;
    int count = 0;
    char *saveptr = NULL;
    for (char *tok = strtok_r(copy, " \t\r\n", &saveptr);
         tok && count < max_tokens;
         tok = strtok_r(NULL, " \t\r\n", &saveptr)) {
        if (!tok[0]) continue;
        tokens_out[count] = strdup(tok);
        if (tokens_out[count]) count++;
    }
    free(copy);
    return count;
}

static void token_list_free_tokens(char **tokens, int count) {
    for (int i = 0; i < count; i++) free(tokens[i]);
}

// DOMTokenList.length getter
GCValue js_dom_token_list_get_length(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue elem = token_list_get_element(ctx, this_val);
    if (JS_IsUndefined(elem) || JS_IsNull(elem)) return JS_NewInt32(ctx, 0);
    char *class_name = token_list_get_class_name(ctx, elem);
    char *tokens[256];
    int count = token_list_parse(class_name, tokens, 256);
    token_list_free_tokens(tokens, count);
    free(class_name);
    return JS_NewInt32(ctx, count);
}

// DOMTokenList.item(index)
GCValue js_dom_token_list_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsNumber(argv[0])) return JS_NULL;
    int index;
    JS_ToInt32(ctx, &index, argv[0]);
    if (index < 0) return JS_NULL;

    GCValue elem = token_list_get_element(ctx, this_val);
    if (JS_IsUndefined(elem) || JS_IsNull(elem)) return JS_NULL;

    char *class_name = token_list_get_class_name(ctx, elem);
    char *tokens[256];
    int count = token_list_parse(class_name, tokens, 256);
    GCValue result = JS_NULL;
    if (index < count && tokens[index]) {
        result = JS_NewString(ctx, tokens[index]);
    }
    token_list_free_tokens(tokens, count);
    free(class_name);
    return result;
}

// DOMTokenList.contains(token)
GCValue js_dom_token_list_contains(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsString(argv[0])) return JS_FALSE;
    const char *token = JS_ToCString(ctx, argv[0]);
    if (!token || !token[0]) return JS_FALSE;

    GCValue elem = token_list_get_element(ctx, this_val);
    if (JS_IsUndefined(elem) || JS_IsNull(elem)) return JS_FALSE;

    char *class_name = token_list_get_class_name(ctx, elem);
    char *tokens[256];
    int count = token_list_parse(class_name, tokens, 256);
    bool found = false;
    for (int i = 0; i < count; i++) {
        if (tokens[i] && strcmp(tokens[i], token) == 0) {
            found = true;
            break;
        }
    }
    token_list_free_tokens(tokens, count);
    free(class_name);
    return JS_NewBool(ctx, found ? 1 : 0);
}

// DOMTokenList.add(...tokens)
GCValue js_dom_token_list_add(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue elem = token_list_get_element(ctx, this_val);
    if (JS_IsUndefined(elem) || JS_IsNull(elem)) return JS_UNDEFINED;

    char *class_name = token_list_get_class_name(ctx, elem);
    char *tokens[256];
    int count = token_list_parse(class_name, tokens, 256);

    for (int i = 0; i < argc; i++) {
        if (!JS_IsString(argv[i])) continue;
        const char *token = JS_ToCString(ctx, argv[i]);
        if (!token || !token[0]) continue;
        bool found = false;
        for (int j = 0; j < count; j++) {
            if (tokens[j] && strcmp(tokens[j], token) == 0) {
                found = true;
                break;
            }
        }
        if (!found && count < 256) {
            tokens[count] = strdup(token);
            if (tokens[count]) count++;
        }
    }

    // Rebuild className
    char new_class[4096];
    new_class[0] = '\0';
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        size_t len = strlen(tokens[i]);
        if (pos + len + 1 >= sizeof(new_class)) break;
        if (pos > 0) new_class[pos++] = ' ';
        memcpy(new_class + pos, tokens[i], len);
        pos += len;
    }
    new_class[pos] = '\0';
    token_list_set_class_name(ctx, elem, new_class);

    token_list_free_tokens(tokens, count);
    free(class_name);
    return JS_UNDEFINED;
}

// DOMTokenList.remove(...tokens)
GCValue js_dom_token_list_remove(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue elem = token_list_get_element(ctx, this_val);
    if (JS_IsUndefined(elem) || JS_IsNull(elem)) return JS_UNDEFINED;

    char *class_name = token_list_get_class_name(ctx, elem);
    char *tokens[256];
    int count = token_list_parse(class_name, tokens, 256);

    for (int i = 0; i < argc; i++) {
        if (!JS_IsString(argv[i])) continue;
        const char *token = JS_ToCString(ctx, argv[i]);
        if (!token || !token[0]) continue;
        for (int j = 0; j < count; j++) {
            if (tokens[j] && strcmp(tokens[j], token) == 0) {
                free(tokens[j]);
                tokens[j] = NULL;
            }
        }
    }

    char new_class[4096];
    new_class[0] = '\0';
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        if (!tokens[i]) continue;
        size_t len = strlen(tokens[i]);
        if (pos + len + 1 >= sizeof(new_class)) break;
        if (pos > 0) new_class[pos++] = ' ';
        memcpy(new_class + pos, tokens[i], len);
        pos += len;
    }
    new_class[pos] = '\0';
    token_list_set_class_name(ctx, elem, new_class);

    token_list_free_tokens(tokens, count);
    free(class_name);
    return JS_UNDEFINED;
}

// DOMTokenList.toggle(token, force)
GCValue js_dom_token_list_toggle(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsString(argv[0])) return JS_NewBool(ctx, 0);
    const char *token = JS_ToCString(ctx, argv[0]);
    if (!token || !token[0]) return JS_NewBool(ctx, 0);

    int force = -1;  // -1 means auto
    if (argc >= 2) {
        int bool_val = JS_ToBool(ctx, argv[1]);
        if (bool_val >= 0) {
            force = bool_val ? 1 : 0;
        }
    }

    GCValue elem = token_list_get_element(ctx, this_val);
    if (JS_IsUndefined(elem) || JS_IsNull(elem)) return JS_NewBool(ctx, 0);

    GCValue contains_args[1] = { argv[0] };
    GCValue exists = js_dom_token_list_contains(ctx, this_val, 1, contains_args);
    int has_token = JS_ToBool(ctx, exists);

    if (force == 1 || (force == -1 && !has_token)) {
        js_dom_token_list_add(ctx, this_val, 1, argv);
        return JS_NewBool(ctx, 1);
    } else if (force == 0 || (force == -1 && has_token)) {
        js_dom_token_list_remove(ctx, this_val, 1, argv);
        return JS_NewBool(ctx, 0);
    }

    return JS_NewBool(ctx, has_token);
}

// DOMTokenList.forEach(callback)
GCValue js_dom_token_list_for_each(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;

    GCValue elem = token_list_get_element(ctx, this_val);
    if (JS_IsUndefined(elem) || JS_IsNull(elem)) return JS_UNDEFINED;

    char *class_name = token_list_get_class_name(ctx, elem);
    char *tokens[256];
    int count = token_list_parse(class_name, tokens, 256);

    GCValue callback = argv[0];
    for (int i = 0; i < count; i++) {
        GCValue item = JS_NewString(ctx, tokens[i] ? tokens[i] : "");
        GCValue args[3] = { item, JS_NewInt32(ctx, i), this_val };
        JS_Call(ctx, callback, JS_UNDEFINED, 3, args);
    }

    token_list_free_tokens(tokens, count);
    free(class_name);
    return JS_UNDEFINED;
}

/* ============================================================================
 * ServiceWorker API Implementation
 * ============================================================================ */

// Forward declarations for ServiceWorker

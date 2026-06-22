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
// mbedtls includes for Crypto API (used by DOMException polyfill)
#include "mbedtls/md.h"

// Define macro to access private GCM functions
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS 1
#include "mbedtls/private/gcm.h"

GCValue js_dummy_function_true(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_TRUE;
}

// Generic dummy function that returns undefined
GCValue js_dummy_function(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

// MessageChannel constructor
GCValue js_message_channel_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = js_create_from_ctor_proto(ctx, new_target);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    GCValue port1 = JS_NewObject(ctx);
    GCValue port2 = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "port1", port1);
    JS_SetPropertyStr(ctx, obj, "port2", port2);
    return obj;
}

// Helper: create object from constructor's prototype (like js_create_from_ctor)
GCValue js_create_from_ctor_proto(JSContextHandle ctx, GCValue ctor) {
    GCValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
    if (JS_IsException(proto))
        return JS_EXCEPTION;
    GCValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    if (JS_IsObject(proto)) {
        JS_SetPrototype(ctx, obj, proto);
    }
    return obj;
}

// Constructor for Element - creates object with Element.prototype in chain
// Uses js_dom_node_class_id so DOM node data can be retrieved via JS_GetOpaqueHandle
GCValue js_element_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = JS_NewObjectClass(ctx, js_dom_node_class_id);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    GCValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (!JS_IsException(proto) && JS_IsObject(proto)) {
        JS_SetPrototype(ctx, obj, proto);
    }
    return obj;
}

// Constructor for HTMLElement - creates object with HTMLElement.prototype in chain
// Uses js_dom_node_class_id so DOM node data can be retrieved via JS_GetOpaqueHandle
GCValue js_html_element_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue obj = JS_NewObjectClass(ctx, js_dom_node_class_id);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    GCValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (!JS_IsException(proto) && JS_IsObject(proto)) {
        JS_SetPrototype(ctx, obj, proto);
    }
    return obj;
}

// Static getter for observedAttributes (needed by Polymer mixin chain)
GCValue js_event_target_observed_attributes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewArray(ctx);
}

// ============================================================================
// DOMException Implementation (needed for Web Animations API)
// ============================================================================


// DOMExceptionData struct is defined in browser_api_impl_types.h

JSClassID js_dom_exception_class_id = 0;

void js_dom_exception_finalizer(JSRuntimeHandle rt, GCValue val) {
    // DOMExceptionData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

JSClassDef js_dom_exception_class_def = {
    .class_name = "DOMException",
    .finalizer = js_dom_exception_finalizer,
};

GCValue js_dom_exception_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    DOM_EX_LOGD("DOMException constructor called");
    DOMExceptionDataHandle de = DOMExceptionDataHandle::create();
    if (!de.valid()) return JS_ThrowTypeError(ctx, "Invalid DOM element");
    
    de.set_name("Error");
    de.set_code(0);
    
    if (argc > 0) {
        const char *msg = JS_ToCString(ctx, argv[0]);
        if (msg) {
            de.set_message(msg);
        }
    }
    
    if (argc > 1) {
        const char *name = JS_ToCString(ctx, argv[1]);
        if (name) {
            de.set_name(name);
            int code = 0;
            if (strcmp(name, "IndexSizeError") == 0) code = DOM_EXCEPTION_INDEX_SIZE_ERR;
            else if (strcmp(name, "HierarchyRequestError") == 0) code = DOM_EXCEPTION_HIERARCHY_REQUEST_ERR;
            else if (strcmp(name, "WrongDocumentError") == 0) code = DOM_EXCEPTION_WRONG_DOCUMENT_ERR;
            else if (strcmp(name, "InvalidCharacterError") == 0) code = DOM_EXCEPTION_INVALID_CHARACTER_ERR;
            else if (strcmp(name, "NoModificationAllowedError") == 0) code = DOM_EXCEPTION_NO_MODIFICATION_ALLOWED_ERR;
            else if (strcmp(name, "NotFoundError") == 0) code = DOM_EXCEPTION_NOT_FOUND_ERR;
            else if (strcmp(name, "NotSupportedError") == 0) code = DOM_EXCEPTION_NOT_SUPPORTED_ERR;
            else if (strcmp(name, "InvalidStateError") == 0) code = DOM_EXCEPTION_INVALID_STATE_ERR;
            else if (strcmp(name, "SyntaxError") == 0) code = DOM_EXCEPTION_SYNTAX_ERR;
            else if (strcmp(name, "InvalidModificationError") == 0) code = DOM_EXCEPTION_INVALID_MODIFICATION_ERR;
            else if (strcmp(name, "NamespaceError") == 0) code = DOM_EXCEPTION_NAMESPACE_ERR;
            else if (strcmp(name, "InvalidAccessError") == 0) code = DOM_EXCEPTION_INVALID_ACCESS_ERR;
            else if (strcmp(name, "TypeMismatchError") == 0) code = DOM_EXCEPTION_TYPE_MISMATCH_ERR;
            else if (strcmp(name, "SecurityError") == 0) code = DOM_EXCEPTION_SECURITY_ERR;
            else if (strcmp(name, "NetworkError") == 0) code = DOM_EXCEPTION_NETWORK_ERR;
            else if (strcmp(name, "AbortError") == 0) code = DOM_EXCEPTION_ABORT_ERR;
            else if (strcmp(name, "URLMismatchError") == 0) code = DOM_EXCEPTION_URL_MISMATCH_ERR;
            else if (strcmp(name, "QuotaExceededError") == 0) code = DOM_EXCEPTION_QUOTA_EXCEEDED_ERR;
            else if (strcmp(name, "TimeoutError") == 0) code = DOM_EXCEPTION_TIMEOUT_ERR;
            else if (strcmp(name, "InvalidNodeTypeError") == 0) code = DOM_EXCEPTION_INVALID_NODE_TYPE_ERR;
            else if (strcmp(name, "DataCloneError") == 0) code = DOM_EXCEPTION_DATA_CLONE_ERR;
            de.set_code(code);
        }
    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_dom_exception_class_id);
    if (JS_IsException(obj)) {
        return obj;
    }
    de.attach_to_object(obj);
    return obj;
}

GCValue js_dom_exception_get_name(JSContextHandle ctx, GCValue this_val) {
    DOMExceptionDataHandle de = DOMExceptionDataHandle::from_object_check(ctx, this_val);
    if (!de.valid()) return JS_ThrowTypeError(ctx, "Invalid DOM element");
    return JS_NewString(ctx, de.name());
}

GCValue js_dom_exception_get_message(JSContextHandle ctx, GCValue this_val) {
    DOMExceptionDataHandle de = DOMExceptionDataHandle::from_object_check(ctx, this_val);
    if (!de.valid()) return JS_ThrowTypeError(ctx, "Invalid DOM element");
    return JS_NewString(ctx, de.message());
}

GCValue js_dom_exception_get_code(JSContextHandle ctx, GCValue this_val) {
    DOMExceptionDataHandle de = DOMExceptionDataHandle::from_object_check(ctx, this_val);
    if (!de.valid()) return JS_ThrowTypeError(ctx, "Invalid DOM element");
    return JS_NewInt32(ctx, de.code());
}

const JSCFunctionListEntry js_dom_exception_proto_funcs[] = {
    JS_CGETSET_DEF("name", js_dom_exception_get_name, NULL),
    JS_CGETSET_DEF("message", js_dom_exception_get_message, NULL),
    JS_CGETSET_DEF("code", js_dom_exception_get_code, NULL),
};
const size_t js_dom_exception_proto_funcs_count = sizeof(js_dom_exception_proto_funcs) / sizeof(js_dom_exception_proto_funcs[0]);

// ============================================================================
// ES6+ Polyfills (C implementations)
// ============================================================================

// Object.getPrototypeOf polyfill
GCValue js_object_get_prototype_of(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "requires at least 1 argument(s)");
    
    GCValue obj = argv[0];
    if (JS_IsNull(obj) || JS_IsUndefined(obj)) {
        return JS_ThrowTypeError(ctx, "Object.getPrototypeOf called on null or undefined");
    }
    
    // Get __proto__ property
    GCValue proto = JS_GetPropertyStr(ctx, obj, "__proto__");
    return proto;
}

// Object.defineProperty implementation with correct ownership semantics
// 
// OWNERSHIP RULES for QuickJS API:
// - JS_GetPropertyStr: returns NEW value (caller must free)
// - JS_DefinePropertyValue: TAKES OWNERSHIP of the value (frees it internally)
// - JS_DefinePropertyGetSet: does NOT take ownership (dupes internally)
// - JS_NewAtom: creates atom (caller must free with JS_FreeAtom)
//
// This implementation tracks ownership explicitly to avoid leaks or double-frees.


// SAFE_FREE_VALUE is no longer needed with mark-and-sweep GC
#define SAFE_FREE_VALUE(ctx, val) do { (void)(val); } while(0)

GCValue js_object_define_property(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void) this_val;
    
    JSAtom prop_atom = 0;
    GCValue result = JS_UNDEFINED;
    
    if (argc < 3) {
        return JS_ThrowTypeError(ctx, "Object.defineProperty requires 3 arguments");
    }
    
    GCValue obj = argv[0];
    GCValue prop = argv[1];
    GCValue descriptor = argv[2];
    
    if (JS_IsNull(obj) || JS_IsUndefined(obj)) {
        return JS_ThrowTypeError(ctx, "Object.defineProperty called on null or undefined");
    }
    
    // Convert property to atom
    if (JS_IsSymbol(prop)) {
        prop_atom = JS_ValueToAtom(ctx, prop);
    } else {
        const char *prop_str = JS_ToCString(ctx, prop);
        if (!prop_str) {
            return JS_ThrowTypeError(ctx, "Object.defineProperty: invalid property key");
        }
        prop_atom = JS_NewAtom(ctx, prop_str);
    }
    
    if (prop_atom == JS_ATOM_NULL) {
        return JS_ThrowTypeError(ctx, "Object.defineProperty: invalid property atom");
    }
    
    // Get descriptor properties
    GCValue get_prop = JS_GetPropertyStr(ctx, descriptor, "get");
    GCValue set_prop = JS_GetPropertyStr(ctx, descriptor, "set");
    
    int has_get = !JS_IsException(get_prop) && !JS_IsUndefined(get_prop);
    int has_set = !JS_IsException(set_prop) && !JS_IsUndefined(set_prop);
    
    // Get flags
    GCValue writable_prop = JS_GetPropertyStr(ctx, descriptor, "writable");
    GCValue enumerable_prop = JS_GetPropertyStr(ctx, descriptor, "enumerable");
    GCValue configurable_prop = JS_GetPropertyStr(ctx, descriptor, "configurable");
    
    int writable = !JS_IsException(writable_prop) && JS_ToBool(ctx, writable_prop);
    int enumerable = !JS_IsException(enumerable_prop) && JS_ToBool(ctx, enumerable_prop);
    int configurable = !JS_IsException(configurable_prop) && JS_ToBool(ctx, configurable_prop);
    
    SAFE_FREE_VALUE(ctx, writable_prop);
    SAFE_FREE_VALUE(ctx, enumerable_prop);
    SAFE_FREE_VALUE(ctx, configurable_prop);
    
    int flags = JS_PROP_THROW;
    if (writable) flags |= JS_PROP_WRITABLE;
    if (enumerable) flags |= JS_PROP_ENUMERABLE;
    if (configurable) flags |= JS_PROP_CONFIGURABLE;
    
    int def_result = -1;
    GCValue value = JS_UNDEFINED;
    
    if (has_get || has_set) {
        // === ACCESSOR PROPERTY ===
        int acc_flags = JS_PROP_THROW;
        if (enumerable) acc_flags |= JS_PROP_ENUMERABLE;
        if (configurable) acc_flags |= JS_PROP_CONFIGURABLE;
        def_result = JS_DefinePropertyGetSet(ctx, obj, prop_atom,
            has_get ? get_prop : JS_UNDEFINED,
            has_set ? set_prop : JS_UNDEFINED,
            acc_flags);
    } else {
        // === DATA PROPERTY ===
        value = JS_GetPropertyStr(ctx, descriptor, "value");
        if (JS_IsException(value)) {
            result = JS_EXCEPTION;
            goto cleanup;
        }
        
        // JS_DefinePropertyValue TAKES OWNERSHIP of value
        def_result = JS_DefinePropertyValue(ctx, obj, prop_atom, value, flags);
        // Value is now owned by the object or freed on error, don't free it
        value = JS_UNDEFINED;
    }
    
    if (def_result < 0) {
        result = JS_EXCEPTION;
    } else {
        result = obj;
    }
    
cleanup:
    if (prop_atom) JS_FreeAtom(ctx, prop_atom);
    SAFE_FREE_VALUE(ctx, get_prop);
    SAFE_FREE_VALUE(ctx, set_prop);
    SAFE_FREE_VALUE(ctx, value);
    return result;
}


// Object.getOwnPropertyDescriptor polyfill
GCValue js_object_get_own_property_descriptor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    
    GCValue obj = argv[0];
    const char *prop = JS_ToCString(ctx, argv[1]);
    if (!prop) return JS_UNDEFINED;
    
    JSAtom prop_atom = JS_NewAtom(ctx, prop);
    JSPropertyDescriptor desc_struct = {0};
    int has_prop = JS_GetOwnProperty(ctx, &desc_struct, obj, prop_atom);
    JS_FreeAtom(ctx, prop_atom);
    
    if (has_prop <= 0) {
        return JS_UNDEFINED;
    }
    
    GCValue desc = JS_NewObject(ctx);
    if (desc_struct.flags & JS_PROP_GETSET) {
        JS_SetPropertyStr(ctx, desc, "get", desc_struct.getter);
        JS_SetPropertyStr(ctx, desc, "set", desc_struct.setter);
        JS_SetPropertyStr(ctx, desc, "enumerable", JS_NewBool(ctx, !!(desc_struct.flags & JS_PROP_ENUMERABLE)));
        JS_SetPropertyStr(ctx, desc, "configurable", JS_NewBool(ctx, !!(desc_struct.flags & JS_PROP_CONFIGURABLE)));
    } else {
        JS_SetPropertyStr(ctx, desc, "value", desc_struct.value);
        JS_SetPropertyStr(ctx, desc, "writable", JS_NewBool(ctx, !!(desc_struct.flags & JS_PROP_WRITABLE)));
        JS_SetPropertyStr(ctx, desc, "enumerable", JS_NewBool(ctx, !!(desc_struct.flags & JS_PROP_ENUMERABLE)));
        JS_SetPropertyStr(ctx, desc, "configurable", JS_NewBool(ctx, !!(desc_struct.flags & JS_PROP_CONFIGURABLE)));
    }
    
    return desc;
}

// Object.setPrototypeOf polyfill
GCValue js_object_set_prototype_of(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "requires at least 2 argument(s)");
    
    GCValue obj = argv[0];
    GCValue proto = argv[1];
    
    // Check for null/undefined
    if (JS_IsNull(obj) || JS_IsUndefined(obj)) {
        return JS_ThrowTypeError(ctx, "Object.setPrototypeOf called on null or undefined");
    }
    
    // Set the prototype using __proto__
    GCValue proto_key = JS_NewString(ctx, "__proto__");
    JS_SetProperty(ctx, obj, JS_ValueToAtom(ctx, proto_key), proto);

    return obj;
}

// Object.getOwnPropertySymbols polyfill - returns empty array
GCValue js_object_get_own_property_symbols(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewArray(ctx);
}

// Object.assign polyfill
GCValue js_object_assign(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    
    GCValue target = argv[0];
    
    for (int i = 1; i < argc; i++) {
        GCValue source = argv[i];
        if (JS_IsNull(source) || JS_IsUndefined(source)) continue;
        
        // Use Object.keys to get enumerable properties
        GCValue global = JS_GetGlobalObject(ctx);
        GCValue object_ctor = JS_GetPropertyStr(ctx, global, "Object");
        GCValue keys_func = JS_GetPropertyStr(ctx, object_ctor, "keys");
        GCValue keys = JS_Call(ctx, keys_func, JS_UNDEFINED, 1, &source);



        if (!JS_IsException(keys)) {
            GCValue len_val = JS_GetPropertyStr(ctx, keys, "length");
            uint32_t key_count = 0;
            JS_ToUint32(ctx, &key_count, len_val);

            for (uint32_t j = 0; j < key_count; j++) {
                GCValue key_val = JS_GetPropertyUint32(ctx, keys, j);
                const char *key = JS_ToCString(ctx, key_val);
                if (key) {
                    GCValue val = JS_GetPropertyStr(ctx, source, key);
                    JS_SetPropertyStr(ctx, target, key, val);
                }

            }

        }
    }
    
    return target;
}

// Reflect.construct polyfill
GCValue js_reflect_construct(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "requires at least 2 argument(s)");
    
    GCValue target = argv[0];
    GCValue args_array = argv[1];
    
    // Get length of args array
    GCValue len_val = JS_GetPropertyStr(ctx, args_array, "length");
    uint32_t args_len = 0;
    JS_ToUint32(ctx, &args_len, len_val);

    // Build arguments array
    GCValue *args = (GCValue*)malloc(sizeof(GCValue) * args_len);
    for (uint32_t i = 0; i < args_len; i++) {
        args[i] = JS_GetPropertyUint32(ctx, args_array, i);
    }
    
    // Call constructor
    GCValue result = JS_CallConstructor(ctx, target, (int)args_len, args);
    
    for (uint32_t i = 0; i < args_len; i++) {

    }
    free(args);
    
    return result;
}

// Reflect.apply polyfill
GCValue js_reflect_apply(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 3) return JS_ThrowTypeError(ctx, "requires at least 3 argument(s)");
    
    GCValue func = argv[0];
    GCValue this_arg = argv[1];
    GCValue args_array = argv[2];
    
    // Get length of args array
    GCValue args_len_val = JS_GetPropertyStr(ctx, args_array, "length");
    uint32_t args_len = 0;
    JS_ToUint32(ctx, &args_len, args_len_val);

    // Build arguments array
    GCValue *args = (GCValue*)malloc(sizeof(GCValue) * args_len);
    for (uint32_t i = 0; i < args_len; i++) {
        args[i] = JS_GetPropertyUint32(ctx, args_array, i);
    }
    
    // Call function
    GCValue result = JS_Call(ctx, func, this_arg, (int)args_len, args);
    
    for (uint32_t i = 0; i < args_len; i++) {

    }
    free(args);
    
    return result;
}

// Reflect.has polyfill
GCValue js_reflect_has(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "requires at least 2 argument(s)");
    
    GCValue target = argv[0];
    const char *prop = JS_ToCString(ctx, argv[1]);
    if (!prop) return JS_FALSE;
    
    JSAtom prop_atom = JS_NewAtom(ctx, prop);
    int has_prop = JS_HasProperty(ctx, target, prop_atom);
    JS_FreeAtom(ctx, prop_atom);
    
    return JS_NewBool(ctx, has_prop);
}

// Promise.prototype.finally polyfill
GCValue js_promise_finally(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsObject(this_val)) return JS_ThrowTypeError(ctx, "requires object and at least 1 argument");
    
    GCValue on_finally = argv[0];
    
    // Create the finally handler
    GCValue handler = JS_NewCFunction(ctx, js_dummy_function, "finally_handler", 0);
    
    // Call .then with the handler
    GCValue then_method = JS_GetPropertyStr(ctx, this_val, "then");
    GCValue args[2] = { handler, handler };
    GCValue result = JS_Call(ctx, then_method, this_val, 2, args);


    return result;
}

// String.prototype.includes polyfill
GCValue js_string_includes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_FALSE;
    
    if (argc < 1) {
        return JS_FALSE;
    }
    
    const char *search = JS_ToCString(ctx, argv[0]);
    if (!search) {
        return JS_FALSE;
    }
    
    int32_t start = 0;
    if (argc > 1) {
        JS_ToInt32(ctx, &start, argv[1]);
    }
    
    // Adjust start position
    size_t str_len = strlen(str);
    if (start < 0) start = 0;
    if ((size_t)start > str_len) start = (int32_t)str_len;
    
    // Search for substring
    const char *found = strstr(str + start, search);
    
    
    return JS_NewBool(ctx, found != NULL);
}

// Array.prototype.includes polyfill
GCValue js_array_includes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_FALSE;
    
    GCValue search_element = argv[0];
    int32_t from_index = 0;
    if (argc > 1) {
        JS_ToInt32(ctx, &from_index, argv[1]);
    }
    
    GCValue len_val = JS_GetPropertyStr(ctx, this_val, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, len_val);

    if (from_index < 0) {
        from_index = (int32_t)len + from_index;
        if (from_index < 0) from_index = 0;
    }
    
    for (uint32_t i = (uint32_t)from_index; i < len; i++) {
        GCValue elem = JS_GetPropertyUint32(ctx, this_val, i);
        int is_equal = JS_StrictEq(ctx, elem, search_element);

        if (is_equal) return JS_TRUE;
    }
    
    return JS_FALSE;
}

// Array.from polyfill
GCValue js_array_from(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewArray(ctx);
    
    GCValue array_like = argv[0];
    uint32_t len = 0;
    
    GCValue len_val2 = JS_GetPropertyStr(ctx, array_like, "length");
    if (JS_ToUint32(ctx, &len, len_val2)) {


        return JS_NewArray(ctx);
    }

    GCValue result = JS_NewArray(ctx);
    for (uint32_t i = 0; i < len; i++) {
        GCValue val = JS_GetPropertyUint32(ctx, array_like, i);
        JS_SetPropertyUint32(ctx, result, i, val);
    }
    
    return result;
}

// ============================================================================
// Map Polyfill Implementation
// ============================================================================

// MapData struct is defined in browser_api_impl_types.h

JSClassID js_map_class_id = 0;

void js_map_finalizer(JSRuntimeHandle rt, GCValue val) {
    // MapData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

JSClassDef js_map_class_def = {
    .class_name = "Map",
    .finalizer = js_map_finalizer,
};

GCValue js_map_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    MapDataHandle map = MapDataHandle::create(ctx);
    if (!map.valid()) return JS_ThrowTypeError(ctx, "Invalid Map object");
    
    GCValue obj = JS_NewObjectClass(ctx, js_map_class_id);
    map.attach_to_object(obj);
    return obj;
}

GCValue js_map_set(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    MapDataHandle map = MapDataHandle::from_object_check(ctx, this_val);
    if (!map.valid() || argc < 2) return JS_ThrowTypeError(ctx, "Map requires 2 arguments");
    
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_ThrowTypeError(ctx, "Invalid key");
    
    // Check if key exists
    GCValue entries = map.entries();
    GCValue existing = JS_GetPropertyStr(ctx, entries, key);
    int exists = !JS_IsUndefined(existing);

    if (!exists) map.increment_size();
    
    JS_SetPropertyStr(ctx, entries, key, argv[1]);
    
    return this_val;
}

GCValue js_map_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    MapDataHandle map = MapDataHandle::from_object_check(ctx, this_val);
    if (!map.valid() || argc < 1) return JS_ThrowTypeError(ctx, "Map requires at least 1 argument");
    
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_ThrowTypeError(ctx, "Invalid key");
    
    GCValue entries = map.entries();
    GCValue val = JS_GetPropertyStr(ctx, entries, key);
    
    return val;
}

GCValue js_map_has(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    MapDataHandle map = MapDataHandle::from_object_check(ctx, this_val);
    if (!map.valid() || argc < 1) return JS_ThrowTypeError(ctx, "Map requires at least 1 argument");
    
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_ThrowTypeError(ctx, "Invalid key");
    
    GCValue entries = map.entries();
    GCValue val = JS_GetPropertyStr(ctx, entries, key);
    
    int exists = !JS_IsUndefined(val);

    return JS_NewBool(ctx, exists);
}

GCValue js_map_delete(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    MapDataHandle map = MapDataHandle::from_object_check(ctx, this_val);
    if (!map.valid() || argc < 1) return JS_ThrowTypeError(ctx, "Map requires at least 1 argument");
    
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_ThrowTypeError(ctx, "Invalid key");
    
    GCValue entries = map.entries();
    GCValue val = JS_GetPropertyStr(ctx, entries, key);
    int exists = !JS_IsUndefined(val);

    if (exists) {
        GCValue undefined = JS_UNDEFINED;
        JS_SetPropertyStr(ctx, entries, key, undefined);
        map.decrement_size();
    }
    
    return JS_NewBool(ctx, exists);
}

GCValue js_map_clear(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    MapDataHandle map = MapDataHandle::from_object_check(ctx, this_val);
    if (!map.valid()) return JS_ThrowTypeError(ctx, "Invalid Map object");

    map.set_entries(JS_NewObject(ctx));
    map.set_size(0);
    
    return JS_UNDEFINED;
}

GCValue js_map_get_size(JSContextHandle ctx, GCValue this_val) {
    MapDataHandle map = MapDataHandle::from_object_check(ctx, this_val);
    if (!map.valid()) return JS_ThrowTypeError(ctx, "Invalid Map object");
    return JS_NewInt32(ctx, map.size());
}

// Helper: create an array of [key, value] pairs from a Map's entries object
GCValue js_map_entries_array(JSContextHandle ctx, GCValue this_val) {
    MapDataHandle map = MapDataHandle::from_object_check(ctx, this_val);
    if (!map.valid()) return JS_ThrowTypeError(ctx, "Invalid Map object");
    
    GCValue entries_obj = map.entries();
    
    // Get Object.keys(entries_obj)
    GCValue global_obj = JS_GetGlobalObject(ctx);
    GCValue object_ctor = JS_GetPropertyStr(ctx, global_obj, "Object");
    GCValue keys_fn = JS_GetPropertyStr(ctx, object_ctor, "keys");
    GCValue keys = JS_Call(ctx, keys_fn, JS_UNDEFINED, 1, &entries_obj);
    if (JS_IsException(keys)) return JS_EXCEPTION;
    
    GCValue array = JS_NewArray(ctx);
    if (JS_IsException(array)) return JS_EXCEPTION;
    
    int len = 0;
    GCValue len_val = JS_GetPropertyStr(ctx, keys, "length");
    JS_ToInt32(ctx, &len, len_val);
    
    for (int i = 0; i < len; i++) {
        GCValue key_val = JS_GetPropertyUint32(ctx, keys, i);
        const char *key = JS_ToCString(ctx, key_val);
        if (!key) continue;
        
        GCValue val = JS_GetPropertyStr(ctx, entries_obj, key);
        
        GCValue pair = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, pair, 0, key_val);
        JS_SetPropertyUint32(ctx, pair, 1, val);
        JS_SetPropertyUint32(ctx, array, i, pair);
    }
    
    return array;
}

GCValue js_map_entries(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue array = js_map_entries_array(ctx, this_val);
    if (JS_IsException(array)) return JS_EXCEPTION;
    
    // Return array[Symbol.iterator]()
    GCValue iterator_sym_eval = JS_Eval(ctx, "Symbol.iterator", 15, "<symbol>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(iterator_sym_eval) || JS_IsUndefined(iterator_sym_eval)) return JS_EXCEPTION;
    GCValue iterator_fn = JS_GetProperty(ctx, array, JS_ValueToAtom(ctx, iterator_sym_eval));
    if (JS_IsException(iterator_fn) || !JS_IsFunction(ctx, iterator_fn)) return JS_EXCEPTION;
    GCValue iterator = JS_Call(ctx, iterator_fn, array, 0, NULL);
    return iterator;
}

GCValue js_map_keys(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    MapDataHandle map = MapDataHandle::from_object_check(ctx, this_val);
    if (!map.valid()) return JS_ThrowTypeError(ctx, "Invalid Map object");
    
    GCValue entries_obj = map.entries();
    GCValue global_obj = JS_GetGlobalObject(ctx);
    GCValue object_ctor = JS_GetPropertyStr(ctx, global_obj, "Object");
    GCValue keys_fn = JS_GetPropertyStr(ctx, object_ctor, "keys");
    GCValue keys = JS_Call(ctx, keys_fn, JS_UNDEFINED, 1, &entries_obj);
    
    // Return keys[Symbol.iterator]()
    GCValue iterator_sym_eval = JS_Eval(ctx, "Symbol.iterator", 15, "<symbol>", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(iterator_sym_eval) && !JS_IsUndefined(iterator_sym_eval)) {
        JSAtom iterator_atom = JS_ValueToAtom(ctx, iterator_sym_eval);
        GCValue iterator_fn = JS_GetProperty(ctx, keys, iterator_atom);
        if (!JS_IsException(iterator_fn) && JS_IsFunction(ctx, iterator_fn)) {
            GCValue iterator = JS_Call(ctx, iterator_fn, keys, 0, NULL);
            return iterator;
        }
    }
    return JS_EXCEPTION;
}

GCValue js_map_values(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    MapDataHandle map = MapDataHandle::from_object_check(ctx, this_val);
    if (!map.valid()) return JS_ThrowTypeError(ctx, "Invalid Map object");
    
    GCValue entries_obj = map.entries();
    GCValue global_obj = JS_GetGlobalObject(ctx);
    GCValue object_ctor = JS_GetPropertyStr(ctx, global_obj, "Object");
    GCValue keys_fn = JS_GetPropertyStr(ctx, object_ctor, "keys");
    GCValue keys = JS_Call(ctx, keys_fn, JS_UNDEFINED, 1, &entries_obj);
    
    GCValue array = JS_NewArray(ctx);
    int len = 0;
    GCValue len_val = JS_GetPropertyStr(ctx, keys, "length");
    JS_ToInt32(ctx, &len, len_val);
    
    for (int i = 0; i < len; i++) {
        GCValue key_val = JS_GetPropertyUint32(ctx, keys, i);
        const char *key = JS_ToCString(ctx, key_val);
        if (!key) continue;
        GCValue val = JS_GetPropertyStr(ctx, entries_obj, key);
        JS_SetPropertyUint32(ctx, array, i, val);
    }
    
    // Return array[Symbol.iterator]()
    GCValue iterator_sym_eval = JS_Eval(ctx, "Symbol.iterator", 15, "<symbol>", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(iterator_sym_eval) && !JS_IsUndefined(iterator_sym_eval)) {
        JSAtom iterator_atom = JS_ValueToAtom(ctx, iterator_sym_eval);
        GCValue iterator_fn = JS_GetProperty(ctx, array, iterator_atom);
        if (!JS_IsException(iterator_fn) && JS_IsFunction(ctx, iterator_fn)) {
            GCValue iterator = JS_Call(ctx, iterator_fn, array, 0, NULL);
            return iterator;
        }
    }
    return JS_EXCEPTION;
}

const JSCFunctionListEntry js_map_proto_funcs[] = {
    JS_CFUNC_DEF("set", 2, js_map_set),
    JS_CFUNC_DEF("get", 1, js_map_get),
    JS_CFUNC_DEF("has", 1, js_map_has),
    JS_CFUNC_DEF("delete", 1, js_map_delete),
    JS_CFUNC_DEF("clear", 0, js_map_clear),
    JS_CGETSET_DEF("size", js_map_get_size, NULL),
    JS_CFUNC_DEF("entries", 0, js_map_entries),
    JS_CFUNC_DEF("keys", 0, js_map_keys),
    JS_CFUNC_DEF("values", 0, js_map_values),
};
const size_t js_map_proto_funcs_count = sizeof(js_map_proto_funcs) / sizeof(js_map_proto_funcs[0]);

extern "C" JSClassID js_xhr_class_id;
extern "C" JSClassID js_video_class_id;
extern GCValue js_xhr_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern GCValue js_video_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern GCValue js_fetch(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
extern "C" const JSCFunctionListEntry js_xhr_proto_funcs[];
extern "C" const JSCFunctionListEntry js_video_proto_funcs[];
extern "C" const size_t js_xhr_proto_funcs_count;
extern "C" const size_t js_video_proto_funcs_count;

// Class IDs for new APIs
JSClassID js_shadow_root_class_id = 0;
JSClassID js_animation_class_id = 0;
JSClassID js_keyframe_effect_class_id = 0;
JSClassID js_font_face_class_id = 0;
JSClassID js_font_face_set_class_id = 0;
JSClassID js_custom_element_registry_class_id = 0;
JSClassID js_mutation_observer_class_id = 0;
JSClassID js_resize_observer_class_id = 0;
JSClassID js_intersection_observer_class_id = 0;
JSClassID js_performance_class_id = 0;
JSClassID js_performance_entry_class_id = 0;
JSClassID js_performance_observer_class_id = 0;
JSClassID js_performance_timing_class_id = 0;
JSClassID js_dom_rect_class_id = 0;
JSClassID js_dom_rect_read_only_class_id = 0;
JSClassID js_media_source_class_id = 0;
JSClassID js_source_buffer_class_id = 0;
JSClassID js_date_class_id = 0;

// ============================================================================
// Event Implementation
// ============================================================================

JSClassID js_event_class_id = 0;
JSClassID js_custom_event_class_id = 0;
JSClassID js_mouse_event_class_id = 0;
JSClassID js_focus_event_class_id = 0;

// ============================================================================
// ServiceWorker API Class IDs
// ============================================================================
JSClassID js_service_worker_container_class_id = 0;
JSClassID js_service_worker_registration_class_id = 0;
JSClassID js_service_worker_class_id = 0;

// ============================================================================
// ============================================================================
// Custom Elements API Implementation
// ============================================================================

// CustomElementRegistryData struct is defined in browser_api_impl_types.h

void js_custom_element_registry_finalizer(JSRuntimeHandle rt, GCValue val) {
    // CustomElementRegistryData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

void js_custom_element_registry_mark(JSRuntimeHandle rt, GCValue val,
                                     JS_MarkFunc *mark_func) {
    GCHandle reg_handle = JS_GetOpaqueHandle(val, js_custom_element_registry_class_id);
    if (reg_handle == GC_HANDLE_NULL) return;

    /* Keep the registry data object alive. */
    mark_func(rt, reg_handle);

    CustomElementRegistryData *reg = (CustomElementRegistryData *)gc_deref(reg_handle);
    if (!reg) return;

    /* Mark the JS object that stores tag-name -> constructor mappings. */
    JS_MarkValue(rt, reg->registry, mark_func);
}

JSClassDef js_custom_element_registry_class_def = {
    .class_name = "CustomElementRegistry",
    .finalizer = js_custom_element_registry_finalizer,
    .gc_mark   = js_custom_element_registry_mark,
};

// customElements.define(name, constructor, options)
GCValue js_custom_elements_define(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "define requires at least 2 arguments");
    }
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_ThrowTypeError(ctx, "Invalid name");
    
    platform_log(LOG_LEVEL_INFO, "customElements", "define called with name='%s' (argc=%d)", name, argc);
    
    // Validate name format (must contain hyphen)
    // NOTE: Relaxed for browser emulation - some scripts may pass invalid names
    // and expect them to be silently ignored or caught internally.
    if (strchr(name, '-') == NULL) {
        platform_log(LOG_LEVEL_WARN, "customElements", "Ignoring invalid custom element name: '%s'", name);
        return JS_UNDEFINED;
    }
    
    // Store in registry (the this_val should be the customElements object)
    JS_SetPropertyStr(ctx, this_val, name, argv[1]);
    
    return JS_UNDEFINED;
}

// customElements.get(name)
GCValue js_custom_elements_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_UNDEFINED;
    
    GCValue ctor = JS_GetPropertyStr(ctx, this_val, name);
    
    
    if (JS_IsUndefined(ctor)) {
        return JS_UNDEFINED;
    }
    return ctor;
}

// customElements.whenDefined(name)
GCValue js_custom_elements_when_defined(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Return a resolved promise
    return js_create_empty_resolved_promise(ctx);
}

// ============================================================================
// Web Animations API Implementation
// ============================================================================

// AnimationData and KeyFrameEffectData structs are defined in browser_api_impl_types.h

void js_animation_finalizer(JSRuntimeHandle rt, GCValue val) {
    // AnimationData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

void js_keyframe_effect_finalizer(JSRuntimeHandle rt, GCValue val) {
    // KeyFrameEffectData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

JSClassDef js_animation_class_def = {
    .class_name = "Animation",
    .finalizer = js_animation_finalizer,
};

JSClassDef js_keyframe_effect_class_def = {
    .class_name = "KeyframeEffect",
    .finalizer = js_keyframe_effect_finalizer,
};

// Animation constructor
GCValue js_animation_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    AnimationDataHandle anim = AnimationDataHandle::create(ctx);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");
    
    if (argc > 0) {
        anim.set_effect(argv[0]);
        // Try to get duration from effect
        if (JS_IsObject(argv[0])) {
            GCValue duration_val = JS_GetPropertyStr(ctx, argv[0], "duration");
            double duration;
            if (!JS_IsException(duration_val) && !JS_ToFloat64(ctx, &duration, duration_val)) {
                anim.set_duration(duration);
            }

        }
    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_animation_class_id);
    anim.attach_to_object(obj);
    return obj;
}

// Animation.prototype.play()
GCValue js_animation_play(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    AnimationDataHandle anim = AnimationDataHandle::from_object_check(ctx, this_val);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");
    anim.set_playing();
    return JS_UNDEFINED;
}

// Animation.prototype.pause()
GCValue js_animation_pause(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    AnimationDataHandle anim = AnimationDataHandle::from_object_check(ctx, this_val);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");
    anim.set_paused();
    return JS_UNDEFINED;
}

// Animation.prototype.finish()
GCValue js_animation_finish(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    AnimationDataHandle anim = AnimationDataHandle::from_object_check(ctx, this_val);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");
    anim.set_finished();
    anim.set_current_time(anim.duration());
    
    // Call onfinish callback if set
    GCValue onfinish = anim.onfinish();
    if (!JS_IsNull(onfinish) && JS_IsFunction(ctx, onfinish)) {
        JS_Call(ctx, onfinish, this_val, 0, NULL);
    }
    return JS_UNDEFINED;
}

// Animation.prototype.cancel()
GCValue js_animation_cancel(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    AnimationDataHandle anim = AnimationDataHandle::from_object_check(ctx, this_val);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");
    anim.set_idle();
    anim.set_current_time(0);
    return JS_UNDEFINED;
}

// Animation.prototype.reverse()
GCValue js_animation_reverse(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// Animation.playState getter
GCValue js_animation_get_play_state(JSContextHandle ctx, GCValue this_val) {
    AnimationDataHandle anim = AnimationDataHandle::from_object_check(ctx, this_val);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");
    return JS_NewString(ctx, anim.play_state_string());
}

// Animation.currentTime getter
GCValue js_animation_get_current_time(JSContextHandle ctx, GCValue this_val) {
    AnimationDataHandle anim = AnimationDataHandle::from_object_check(ctx, this_val);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");
    return JS_NewFloat64(ctx, anim.current_time());
}

// Animation.effect getter
GCValue js_animation_get_effect(JSContextHandle ctx, GCValue this_val) {
    AnimationDataHandle anim = AnimationDataHandle::from_object_check(ctx, this_val);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");
    GCValue effect = anim.effect();
    if (JS_IsNull(effect)) return JS_NULL;
    return effect;
}

// Animation.onfinish getter/setter
GCValue js_animation_get_onfinish(JSContextHandle ctx, GCValue this_val) {
    AnimationDataHandle anim = AnimationDataHandle::from_object_check(ctx, this_val);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");
    return anim.onfinish();
}

GCValue js_animation_set_onfinish(JSContextHandle ctx, GCValue this_val, GCValue val) {
    AnimationDataHandle anim = AnimationDataHandle::from_object_check(ctx, this_val);
    if (!anim.valid()) return JS_ThrowTypeError(ctx, "Invalid Animation");

    anim.set_onfinish(val);
    return JS_UNDEFINED;
}

const JSCFunctionListEntry js_animation_proto_funcs[] = {
    JS_CFUNC_DEF("play", 0, js_animation_play),
    JS_CFUNC_DEF("pause", 0, js_animation_pause),
    JS_CFUNC_DEF("finish", 0, js_animation_finish),
    JS_CFUNC_DEF("cancel", 0, js_animation_cancel),
    JS_CFUNC_DEF("reverse", 0, js_animation_reverse),
    JS_CGETSET_DEF("playState", js_animation_get_play_state, NULL),
    JS_CGETSET_DEF("currentTime", js_animation_get_current_time, NULL),
    JS_CGETSET_DEF("effect", js_animation_get_effect, NULL),
    JS_CGETSET_DEF("onfinish", js_animation_get_onfinish, js_animation_set_onfinish),
};
const size_t js_animation_proto_funcs_count = sizeof(js_animation_proto_funcs) / sizeof(js_animation_proto_funcs[0]);

// KeyframeEffect constructor
GCValue js_keyframe_effect_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    KeyFrameEffectDataHandle effect = KeyFrameEffectDataHandle::create();
    if (!effect.valid()) return JS_ThrowTypeError(ctx, "Invalid Effect");
    
    if (argc > 0) {
        effect.set_target(argv[0]);
    }
    if (argc > 1) {
        effect.set_keyframes(argv[1]);
    }
    if (argc > 2 && JS_IsObject(argv[2])) {
        GCValue duration_val = JS_GetPropertyStr(ctx, argv[2], "duration");
        double duration;
        if (!JS_IsException(duration_val) && !JS_ToFloat64(ctx, &duration, duration_val)) {
            effect.set_duration(duration);
        }

        GCValue easing_val = JS_GetPropertyStr(ctx, argv[2], "easing");
        const char *easing = JS_ToCString(ctx, easing_val);
        if (easing) {
            effect.set_easing(easing);
        }

    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_keyframe_effect_class_id);
    effect.attach_to_object(obj);
    return obj;
}

// KeyframeEffect.target getter
GCValue js_keyframe_effect_get_target(JSContextHandle ctx, GCValue this_val) {
    KeyFrameEffectDataHandle effect = KeyFrameEffectDataHandle::from_object_check(ctx, this_val);
    if (!effect.valid()) return JS_ThrowTypeError(ctx, "Invalid Effect");
    GCValue target = effect.target();
    if (JS_IsNull(target)) return JS_NULL;
    return target;
}

// KeyframeEffect.duration getter
GCValue js_keyframe_effect_get_duration(JSContextHandle ctx, GCValue this_val) {
    KeyFrameEffectDataHandle effect = KeyFrameEffectDataHandle::from_object_check(ctx, this_val);
    if (!effect.valid()) return JS_ThrowTypeError(ctx, "Invalid Effect");
    return JS_NewFloat64(ctx, effect.duration());
}

const JSCFunctionListEntry js_keyframe_effect_proto_funcs[] = {
    JS_CGETSET_DEF("target", js_keyframe_effect_get_target, NULL),
    JS_CGETSET_DEF("duration", js_keyframe_effect_get_duration, NULL),
};
const size_t js_keyframe_effect_proto_funcs_count = sizeof(js_keyframe_effect_proto_funcs) / sizeof(js_keyframe_effect_proto_funcs[0]);

// Element.prototype.animate(keyframes, options)
GCValue js_element_animate(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Create KeyframeEffect
    GCValue effect_args[3];
    effect_args[0] = this_val;  // target
    effect_args[1] = argc > 0 ? argv[0] : JS_NULL;  // keyframes
    effect_args[2] = argc > 1 ? argv[1] : JS_NULL;  // options
    
    GCValue effect = js_keyframe_effect_constructor(ctx, JS_UNDEFINED, 3, effect_args);



    if (JS_IsException(effect)) {
        return effect;
    }
    
    // Create Animation with the effect
    GCValue anim_args[1];
    anim_args[0] = effect;
    GCValue animation = js_animation_constructor(ctx, JS_UNDEFINED, 1, anim_args);

    if (JS_IsException(animation)) {
        return animation;
    }
    
    // Auto-play the animation
    AnimationDataHandle anim = AnimationDataHandle::from_object(animation);
    if (anim.valid()) {
        anim.set_playing();
    }
    
    // Set oncancel to null (not undefined) so Web Animations polyfill
    // feature detection skips its wrapping code path
    JS_SetPropertyStr(ctx, animation, "oncancel", JS_NULL);
    
    return animation;
}

// ============================================================================
// Font Loading API Implementation
// ============================================================================

// FontFaceData and FontFaceSetData structs are defined in browser_api_impl_types.h

void js_font_face_finalizer(JSRuntimeHandle rt, GCValue val) {
    // FontFaceData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

void js_font_face_set_finalizer(JSRuntimeHandle rt, GCValue val) {
    // FontFaceSetData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

JSClassDef js_font_face_class_def = {
    .class_name = "FontFace",
    .finalizer = js_font_face_finalizer,
};

JSClassDef js_font_face_set_class_def = {
    .class_name = "FontFaceSet",
    .finalizer = js_font_face_set_finalizer,
};

// FontFace constructor
GCValue js_font_face_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    FontFaceDataHandle ff = FontFaceDataHandle::create();
    if (!ff.valid()) return JS_ThrowTypeError(ctx, "Invalid FontFace");
    
    if (argc > 0) {
        const char *family = JS_ToCString(ctx, argv[0]);
        if (family) {
            ff.set_family(family);
        }
    }
    
    if (argc > 1) {
        const char *source = JS_ToCString(ctx, argv[1]);
        if (source) {
            ff.set_source(source);
        }
    }
    
    if (argc > 2 && JS_IsObject(argv[2])) {
        GCValue display_val = JS_GetPropertyStr(ctx, argv[2], "display");
        const char *display = JS_ToCString(ctx, display_val);
        if (display) {
            ff.set_display(display);
        }

    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_font_face_class_id);
    ff.attach_to_object(obj);
    return obj;
}

// FontFace.load()
GCValue js_font_face_load(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Return a resolved promise with this FontFace
    GCValue result = js_create_resolved_promise(ctx, this_val);

    return result;
}

// FontFace.loaded getter - returns a Promise that resolves to this FontFace
GCValue js_font_face_get_loaded(JSContextHandle ctx, GCValue this_val) {
    FontFaceDataHandle ff = FontFaceDataHandle::from_object_check(ctx, this_val);
    if (!ff.valid()) return JS_ThrowTypeError(ctx, "Invalid FontFace");
    // Return a resolved promise with this FontFace
    GCValue result = js_create_resolved_promise(ctx, this_val);

    return result;
}

// FontFace.family getter
GCValue js_font_face_get_family(JSContextHandle ctx, GCValue this_val) {
    FontFaceDataHandle ff = FontFaceDataHandle::from_object_check(ctx, this_val);
    if (!ff.valid()) return JS_ThrowTypeError(ctx, "Invalid FontFace");
    return JS_NewString(ctx, ff.family());
}

// FontFace.status getter
GCValue js_font_face_get_status(JSContextHandle ctx, GCValue this_val) {
    return JS_NewString(ctx, "loaded");
}

const JSCFunctionListEntry js_font_face_proto_funcs[] = {
    JS_CFUNC_DEF("load", 0, js_font_face_load),
    JS_CGETSET_DEF("family", js_font_face_get_family, NULL),
    JS_CGETSET_DEF("status", js_font_face_get_status, NULL),
    JS_CGETSET_DEF("loaded", js_font_face_get_loaded, NULL),  // Now returns a proper Promise
};
const size_t js_font_face_proto_funcs_count = sizeof(js_font_face_proto_funcs) / sizeof(js_font_face_proto_funcs[0]);

// FontFaceSet.load(fontSpec)
GCValue js_font_face_set_load(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Return a resolved promise with empty array (all fonts "loaded")
    GCValue empty_array = JS_NewArray(ctx);
    GCValue result = js_create_resolved_promise(ctx, empty_array);

    return result;
}

// FontFaceSet.check(fontSpec)
GCValue js_font_face_set_check(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Always return true (fonts are available)
    return JS_TRUE;
}

// FontFaceSet.ready getter
GCValue js_font_face_set_get_ready(JSContextHandle ctx, GCValue this_val) {
    // Return a resolved promise
    return js_create_empty_resolved_promise(ctx);
}

// FontFaceSet.status getter
GCValue js_font_face_set_get_status(JSContextHandle ctx, GCValue this_val) {
    return JS_NewString(ctx, "loaded");
}

// FontFaceSet.add(fontFace)
GCValue js_font_face_set_add(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// FontFaceSet.delete(fontFace)
GCValue js_font_face_set_delete(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_TRUE;
}

// FontFaceSet.clear()
GCValue js_font_face_set_clear(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// FontFaceSet.has(fontFace)
GCValue js_font_face_set_has(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_TRUE;
}

// FontFaceSet.forEach(callback)
GCValue js_font_face_set_forEach(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// FontFaceSet[Symbol.iterator]()
GCValue js_font_face_set_iterator(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue array_ctor = JS_GetPropertyStr(ctx, global, "Array");
    GCValue empty_array = JS_CallConstructor(ctx, array_ctor, 0, NULL);
    GCValue result = JS_GetPropertyStr(ctx, empty_array, "values");
    GCValue iterator = JS_Call(ctx, result, empty_array, 0, NULL);




    return iterator;
}

const JSCFunctionListEntry js_font_face_set_proto_funcs[] = {
    JS_CFUNC_DEF("load", 1, js_font_face_set_load),
    JS_CFUNC_DEF("check", 1, js_font_face_set_check),
    JS_CGETSET_DEF("ready", js_font_face_set_get_ready, NULL),
    JS_CGETSET_DEF("status", js_font_face_set_get_status, NULL),
    JS_CFUNC_DEF("add", 1, js_font_face_set_add),
    JS_CFUNC_DEF("delete", 1, js_font_face_set_delete),
    JS_CFUNC_DEF("clear", 0, js_font_face_set_clear),
    JS_CFUNC_DEF("has", 1, js_font_face_set_has),
    JS_CFUNC_DEF("forEach", 1, js_font_face_set_forEach),
    JS_CFUNC_DEF("values", 0, js_font_face_set_iterator),
    JS_CFUNC_DEF("keys", 0, js_font_face_set_iterator),
    JS_CFUNC_DEF("entries", 0, js_font_face_set_iterator),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, js_font_face_set_iterator),
};
const size_t js_font_face_set_proto_funcs_count = sizeof(js_font_face_set_proto_funcs) / sizeof(js_font_face_set_proto_funcs[0]);

// ============================================================================
// MutationObserver Implementation
// ============================================================================

// MutationObserverData struct is defined in browser_api_impl_types.h

void js_mutation_observer_finalizer(JSRuntimeHandle rt, GCValue val) {
    // MutationObserverData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

JSClassDef js_mutation_observer_class_def = {
    .class_name = "MutationObserver",
    .finalizer = js_mutation_observer_finalizer,
};

// MutationObserver constructor
GCValue js_mutation_observer_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "MutationObserver constructor requires a callback function");
    }
    
    MutationObserverDataHandle mo = MutationObserverDataHandle::create(ctx, argv[0]);
    if (!mo.valid()) return JS_ThrowTypeError(ctx, "Invalid MutationObserver");
    
    GCValue obj = JS_NewObjectClass(ctx, js_mutation_observer_class_id);
    mo.attach_to_object(obj);
    return obj;
}

// MutationObserver.prototype.observe(target, options)
GCValue js_mutation_observer_observe(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// MutationObserver.prototype.disconnect()
GCValue js_mutation_observer_disconnect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// MutationObserver.prototype.takeRecords()
GCValue js_mutation_observer_takeRecords(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_NewArray(ctx);
}

const JSCFunctionListEntry js_mutation_observer_proto_funcs[] = {
    JS_CFUNC_DEF("observe", 2, js_mutation_observer_observe),
    JS_CFUNC_DEF("disconnect", 0, js_mutation_observer_disconnect),
    JS_CFUNC_DEF("takeRecords", 0, js_mutation_observer_takeRecords),
};
const size_t js_mutation_observer_proto_funcs_count = sizeof(js_mutation_observer_proto_funcs) / sizeof(js_mutation_observer_proto_funcs[0]);

// ============================================================================
// ResizeObserver Implementation
// ============================================================================

// ResizeObserverData struct is defined in browser_api_impl_types.h

void js_resize_observer_finalizer(JSRuntimeHandle rt, GCValue val) {
    // ResizeObserverData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

JSClassDef js_resize_observer_class_def = {
    .class_name = "ResizeObserver",
    .finalizer = js_resize_observer_finalizer,
};

// ResizeObserver constructor
GCValue js_resize_observer_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "ResizeObserver constructor requires a callback function");
    }
    
    ResizeObserverDataHandle ro = ResizeObserverDataHandle::create(ctx, argv[0]);
    if (!ro.valid()) return JS_ThrowTypeError(ctx, "Invalid ResizeObserver");
    
    GCValue obj = JS_NewObjectClass(ctx, js_resize_observer_class_id);
    ro.attach_to_object(obj);
    return obj;
}

// ResizeObserver.prototype.observe(target)
GCValue js_resize_observer_observe(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// ResizeObserver.prototype.unobserve(target)
GCValue js_resize_observer_unobserve(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// ResizeObserver.prototype.disconnect()
GCValue js_resize_observer_disconnect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

const JSCFunctionListEntry js_resize_observer_proto_funcs[] = {
    JS_CFUNC_DEF("observe", 1, js_resize_observer_observe),
    JS_CFUNC_DEF("unobserve", 1, js_resize_observer_unobserve),
    JS_CFUNC_DEF("disconnect", 0, js_resize_observer_disconnect),
};
const size_t js_resize_observer_proto_funcs_count = sizeof(js_resize_observer_proto_funcs) / sizeof(js_resize_observer_proto_funcs[0]);

// ============================================================================
// IntersectionObserver Implementation
// ============================================================================

// IntersectionObserverData struct is defined in browser_api_impl_types.h

void js_intersection_observer_finalizer(JSRuntimeHandle rt, GCValue val) {
    // IntersectionObserverData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

void js_intersection_observer_mark(JSRuntimeHandle rt, GCValue val,
                                   JS_MarkFunc *mark_func) {
    GCHandle io_handle = JS_GetOpaqueHandle(val, js_intersection_observer_class_id);
    if (io_handle == GC_HANDLE_NULL) return;

    /* Keep the IntersectionObserver data object alive. */
    mark_func(rt, io_handle);

    IntersectionObserverData *io = (IntersectionObserverData *)gc_deref(io_handle);
    if (!io) return;

    /* Mark callback and root so they are not collected. */
    JS_MarkValue(rt, io->callback, mark_func);
    JS_MarkValue(rt, io->root, mark_func);
}

JSClassDef js_intersection_observer_class_def = {
    .class_name = "IntersectionObserver",
    .finalizer = js_intersection_observer_finalizer,
    .gc_mark   = js_intersection_observer_mark,
};

// IntersectionObserver constructor
GCValue js_intersection_observer_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "IntersectionObserver constructor requires a callback function");
    }
    
    IntersectionObserverDataHandle io = IntersectionObserverDataHandle::create(ctx, argv[0]);
    if (!io.valid()) return JS_ThrowTypeError(ctx, "Invalid IntersectionObserver");
    
    // Parse options if provided
    if (argc > 1 && JS_IsObject(argv[1])) {
        GCValue root_val = JS_GetPropertyStr(ctx, argv[1], "root");
        if (!JS_IsUndefined(root_val) && !JS_IsNull(root_val)) {
            io.set_root(root_val);
        }

        GCValue margin_val = JS_GetPropertyStr(ctx, argv[1], "rootMargin");
        const char *margin = JS_ToCString(ctx, margin_val);
        if (margin) {
            io.set_root_margin(margin);
        }

        GCValue threshold_val = JS_GetPropertyStr(ctx, argv[1], "threshold");
        double threshold;
        if (!JS_IsException(threshold_val) && !JS_ToFloat64(ctx, &threshold, threshold_val)) {
            io.set_threshold(threshold);
        }

    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_intersection_observer_class_id);
    io.attach_to_object(obj);
    return obj;
}

// IntersectionObserver.prototype.observe(target)
GCValue js_intersection_observer_observe(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// IntersectionObserver.prototype.unobserve(target)
GCValue js_intersection_observer_unobserve(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// IntersectionObserver.prototype.disconnect()
GCValue js_intersection_observer_disconnect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// IntersectionObserver.prototype.takeRecords()
GCValue js_intersection_observer_takeRecords(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_NewArray(ctx);
}

const JSCFunctionListEntry js_intersection_observer_proto_funcs[] = {
    JS_CFUNC_DEF("observe", 1, js_intersection_observer_observe),
    JS_CFUNC_DEF("unobserve", 1, js_intersection_observer_unobserve),
    JS_CFUNC_DEF("disconnect", 0, js_intersection_observer_disconnect),
    JS_CFUNC_DEF("takeRecords", 0, js_intersection_observer_takeRecords),
};
const size_t js_intersection_observer_proto_funcs_count = sizeof(js_intersection_observer_proto_funcs) / sizeof(js_intersection_observer_proto_funcs[0]);

// ============================================================================
// Performance API Implementation
// ============================================================================

// PerformanceData, PerformanceEntryData, PerformanceObserverData structs are defined in browser_api_impl_types.h

void js_performance_finalizer(JSRuntimeHandle rt, GCValue val) {
    // PerformanceData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

void js_performance_entry_finalizer(JSRuntimeHandle rt, GCValue val) {
    // PerformanceEntryData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

void js_performance_observer_finalizer(JSRuntimeHandle rt, GCValue val) {
    // PerformanceObserverData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

JSClassDef js_performance_class_def = {
    .class_name = "Performance",
    .finalizer = js_performance_finalizer,
};

JSClassDef js_performance_entry_class_def = {
    .class_name = "PerformanceEntry",
    .finalizer = js_performance_entry_finalizer,
};

JSClassDef js_performance_observer_class_def = {
    .class_name = "PerformanceObserver",
    .finalizer = js_performance_observer_finalizer,
};

// Performance.now() - high resolution timestamp
double g_performance_time_origin = 0.0;

#ifdef _MSC_VER
double performance_get_time_ms(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
double performance_get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
#endif

GCValue js_performance_now(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    // Return high resolution timestamp relative to time origin
    double now = performance_get_time_ms() - g_performance_time_origin;
    return JS_NewFloat64(ctx, now);
}

// Performance.timeOrigin getter
GCValue js_performance_get_time_origin(JSContextHandle ctx, GCValue this_val) {
    (void)this_val;
    return JS_NewFloat64(ctx, g_performance_time_origin);
}

// Helper to get PerformanceData from JS object
static PerformanceData* get_performance_data(JSContextHandle ctx, GCValue obj) {
    (void)ctx;
    // Use gc_deref to get pointer from handle stored in opaque
    GCHandle handle = JS_GetOpaqueHandle(obj, JS_GC_OBJ_TYPE_DATA);
    if (handle == GC_HANDLE_NULL) return NULL;
    return (PerformanceData*)gc_deref(handle);
}

// Performance.getEntries()
GCValue js_performance_get_entries(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue result = JS_NewArray(ctx);
    
    PerformanceData* perf = get_performance_data(ctx, this_val);
    if (!perf) return result;
    
    for (int i = 0; i < perf->entry_count; i++) {
        // Create PerformanceEntry object
        GCValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "name", JS_NewString(ctx, perf->entries[i].name));
        JS_SetPropertyStr(ctx, entry, "entryType", JS_NewString(ctx, perf->entries[i].entryType));
        JS_SetPropertyStr(ctx, entry, "startTime", JS_NewFloat64(ctx, perf->entries[i].startTime));
        JS_SetPropertyStr(ctx, entry, "duration", JS_NewFloat64(ctx, perf->entries[i].duration));
        JS_SetPropertyUint32(ctx, result, i, entry);
    }
    
    return result;
}

// Performance.getEntriesByType(type)
GCValue js_performance_get_entries_by_type(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue result = JS_NewArray(ctx);
    
    if (argc < 1) return result;
    
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return result;
    
    PerformanceData* perf = get_performance_data(ctx, this_val);
    if (!perf) return result;
    
    int idx = 0;
    for (int i = 0; i < perf->entry_count; i++) {
        if (strcmp(perf->entries[i].entryType, type) == 0) {
            GCValue entry = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, entry, "name", JS_NewString(ctx, perf->entries[i].name));
            JS_SetPropertyStr(ctx, entry, "entryType", JS_NewString(ctx, perf->entries[i].entryType));
            JS_SetPropertyStr(ctx, entry, "startTime", JS_NewFloat64(ctx, perf->entries[i].startTime));
            JS_SetPropertyStr(ctx, entry, "duration", JS_NewFloat64(ctx, perf->entries[i].duration));
            JS_SetPropertyUint32(ctx, result, idx++, entry);
        }
    }
    
    return result;
}

// Performance.getEntriesByName(name, type)
GCValue js_performance_get_entries_by_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue result = JS_NewArray(ctx);
    
    if (argc < 1) return result;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return result;
    
    // Optional type filter
    const char *type_filter = NULL;
    if (argc >= 2) {
        type_filter = JS_ToCString(ctx, argv[1]);
    }
    
    PerformanceData* perf = get_performance_data(ctx, this_val);
    if (!perf) return result;
    
    int idx = 0;
    for (int i = 0; i < perf->entry_count; i++) {
        if (strcmp(perf->entries[i].name, name) == 0) {
            // Check type filter if provided
            if (type_filter && strcmp(perf->entries[i].entryType, type_filter) != 0) {
                continue;
            }
            GCValue entry = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, entry, "name", JS_NewString(ctx, perf->entries[i].name));
            JS_SetPropertyStr(ctx, entry, "entryType", JS_NewString(ctx, perf->entries[i].entryType));
            JS_SetPropertyStr(ctx, entry, "startTime", JS_NewFloat64(ctx, perf->entries[i].startTime));
            JS_SetPropertyStr(ctx, entry, "duration", JS_NewFloat64(ctx, perf->entries[i].duration));
            JS_SetPropertyUint32(ctx, result, idx++, entry);
        }
    }
    
    return result;
}

// Performance.mark(name)
GCValue js_performance_mark(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_UNDEFINED;
    
    PerformanceData* perf = get_performance_data(ctx, this_val);
    if (!perf) return JS_UNDEFINED;
    
    if (perf->entry_count >= PERFORMANCE_MAX_ENTRIES) {
        platform_log(LOG_LEVEL_ERROR, "Performance", "FATAL: Performance entry limit of %d exceeded. Aborting.", PERFORMANCE_MAX_ENTRIES);
        abort();
    }
    
    // Add mark entry
    PerformanceEntryData* entry = &perf->entries[perf->entry_count++];
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    strncpy(entry->entryType, "mark", sizeof(entry->entryType) - 1);
    entry->startTime = performance_get_time_ms() - g_performance_time_origin;
    entry->duration = 0;
    
    return JS_UNDEFINED;
}

// Performance.measure(name, startMark, endMark)
GCValue js_performance_measure(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_UNDEFINED;
    
    PerformanceData* perf = get_performance_data(ctx, this_val);
    if (!perf) return JS_UNDEFINED;
    
    if (perf->entry_count >= PERFORMANCE_MAX_ENTRIES) {
        platform_log(LOG_LEVEL_ERROR, "Performance", "FATAL: Performance entry limit of %d exceeded. Aborting.", PERFORMANCE_MAX_ENTRIES);
        abort();
    }
    
    double start_time = 0;
    double end_time = performance_get_time_ms() - g_performance_time_origin;
    
    // Parse startMark if provided
    if (argc >= 2 && JS_IsString(argv[1])) {
        const char *start_mark = JS_ToCString(ctx, argv[1]);
        if (start_mark) {
            // Find the mark
            for (int i = 0; i < perf->entry_count; i++) {
                if (strcmp(perf->entries[i].name, start_mark) == 0 && 
                    strcmp(perf->entries[i].entryType, "mark") == 0) {
                    start_time = perf->entries[i].startTime;
                    break;
                }
            }
        }
    }
    
    // Parse endMark if provided
    if (argc >= 3 && JS_IsString(argv[2])) {
        const char *end_mark = JS_ToCString(ctx, argv[2]);
        if (end_mark) {
            // Find the mark
            for (int i = 0; i < perf->entry_count; i++) {
                if (strcmp(perf->entries[i].name, end_mark) == 0 && 
                    strcmp(perf->entries[i].entryType, "mark") == 0) {
                    end_time = perf->entries[i].startTime;
                    break;
                }
            }
        }
    }
    
    // Add measure entry
    PerformanceEntryData* entry = &perf->entries[perf->entry_count++];
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    strncpy(entry->entryType, "measure", sizeof(entry->entryType) - 1);
    entry->startTime = start_time;
    entry->duration = end_time - start_time;
    
    return JS_UNDEFINED;
}

// Performance.clearMarks(name)
GCValue js_performance_clear_marks(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    PerformanceData* perf = get_performance_data(ctx, this_val);
    if (!perf) return JS_UNDEFINED;
    
    const char *name_filter = NULL;
    if (argc >= 1 && JS_IsString(argv[0])) {
        name_filter = JS_ToCString(ctx, argv[0]);
    }
    
    int write_idx = 0;
    for (int i = 0; i < perf->entry_count; i++) {
        int should_remove = (strcmp(perf->entries[i].entryType, "mark") == 0);
        if (should_remove && name_filter) {
            should_remove = (strcmp(perf->entries[i].name, name_filter) == 0);
        }
        
        if (!should_remove) {
            // Keep this entry
            if (write_idx != i) {
                perf->entries[write_idx] = perf->entries[i];
            }
            write_idx++;
        }
    }
    perf->entry_count = write_idx;
    
    return JS_UNDEFINED;
}

// Performance.clearMeasures(name)
GCValue js_performance_clear_measures(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    PerformanceData* perf = get_performance_data(ctx, this_val);
    if (!perf) return JS_UNDEFINED;
    
    const char *name_filter = NULL;
    if (argc >= 1 && JS_IsString(argv[0])) {
        name_filter = JS_ToCString(ctx, argv[0]);
    }
    
    int write_idx = 0;
    for (int i = 0; i < perf->entry_count; i++) {
        int should_remove = (strcmp(perf->entries[i].entryType, "measure") == 0);
        if (should_remove && name_filter) {
            should_remove = (strcmp(perf->entries[i].name, name_filter) == 0);
        }
        
        if (!should_remove) {
            // Keep this entry
            if (write_idx != i) {
                perf->entries[write_idx] = perf->entries[i];
            }
            write_idx++;
        }
    }
    perf->entry_count = write_idx;
    
    return JS_UNDEFINED;
}

// Performance.clearResourceTimings()
GCValue js_performance_clear_resource_timings(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    PerformanceData* perf = get_performance_data(ctx, this_val);
    if (!perf) return JS_UNDEFINED;
    
    int write_idx = 0;
    for (int i = 0; i < perf->entry_count; i++) {
        int should_remove = (strcmp(perf->entries[i].entryType, "resource") == 0);
        
        if (!should_remove) {
            // Keep this entry
            if (write_idx != i) {
                perf->entries[write_idx] = perf->entries[i];
            }
            write_idx++;
        }
    }
    perf->entry_count = write_idx;
    
    return JS_UNDEFINED;
}

// PerformanceTimingData struct is defined in browser_api_impl_types.h

void js_performance_timing_finalizer(JSRuntimeHandle rt, GCValue val) {
    // PerformanceTimingData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

JSClassDef js_performance_timing_class_def = {
    .class_name = "PerformanceTiming",
    .finalizer = js_performance_timing_finalizer,
};

#define DEF_TIMING_GETTER(field) \
static GCValue js_performance_timing_get_##field(JSContextHandle ctx, GCValue this_val) { \
    PerformanceTimingDataHandle timing = PerformanceTimingDataHandle::from_object_check(ctx, this_val); \
    if (!timing.valid()) return JS_ThrowTypeError(ctx, "Invalid PerformanceTiming"); \
    return JS_NewFloat64(ctx, timing.field()); \
}

DEF_TIMING_GETTER(navigationStart)
DEF_TIMING_GETTER(unloadEventStart)
DEF_TIMING_GETTER(unloadEventEnd)
DEF_TIMING_GETTER(redirectStart)
DEF_TIMING_GETTER(redirectEnd)
DEF_TIMING_GETTER(fetchStart)
DEF_TIMING_GETTER(domainLookupStart)
DEF_TIMING_GETTER(domainLookupEnd)
DEF_TIMING_GETTER(connectStart)
DEF_TIMING_GETTER(connectEnd)
DEF_TIMING_GETTER(secureConnectionStart)
DEF_TIMING_GETTER(requestStart)
DEF_TIMING_GETTER(responseStart)
DEF_TIMING_GETTER(responseEnd)
DEF_TIMING_GETTER(domLoading)
DEF_TIMING_GETTER(domInteractive)
DEF_TIMING_GETTER(domContentLoadedEventStart)
DEF_TIMING_GETTER(domContentLoadedEventEnd)
DEF_TIMING_GETTER(domComplete)
DEF_TIMING_GETTER(loadEventStart)
DEF_TIMING_GETTER(loadEventEnd)

#undef DEF_TIMING_GETTER

const JSCFunctionListEntry js_performance_timing_proto_funcs[] = {
    JS_CGETSET_DEF("navigationStart", js_performance_timing_get_navigationStart, NULL),
    JS_CGETSET_DEF("unloadEventStart", js_performance_timing_get_unloadEventStart, NULL),
    JS_CGETSET_DEF("unloadEventEnd", js_performance_timing_get_unloadEventEnd, NULL),
    JS_CGETSET_DEF("redirectStart", js_performance_timing_get_redirectStart, NULL),
    JS_CGETSET_DEF("redirectEnd", js_performance_timing_get_redirectEnd, NULL),
    JS_CGETSET_DEF("fetchStart", js_performance_timing_get_fetchStart, NULL),
    JS_CGETSET_DEF("domainLookupStart", js_performance_timing_get_domainLookupStart, NULL),
    JS_CGETSET_DEF("domainLookupEnd", js_performance_timing_get_domainLookupEnd, NULL),
    JS_CGETSET_DEF("connectStart", js_performance_timing_get_connectStart, NULL),
    JS_CGETSET_DEF("connectEnd", js_performance_timing_get_connectEnd, NULL),
    JS_CGETSET_DEF("secureConnectionStart", js_performance_timing_get_secureConnectionStart, NULL),
    JS_CGETSET_DEF("requestStart", js_performance_timing_get_requestStart, NULL),
    JS_CGETSET_DEF("responseStart", js_performance_timing_get_responseStart, NULL),
    JS_CGETSET_DEF("responseEnd", js_performance_timing_get_responseEnd, NULL),
    JS_CGETSET_DEF("domLoading", js_performance_timing_get_domLoading, NULL),
    JS_CGETSET_DEF("domInteractive", js_performance_timing_get_domInteractive, NULL),
    JS_CGETSET_DEF("domContentLoadedEventStart", js_performance_timing_get_domContentLoadedEventStart, NULL),
    JS_CGETSET_DEF("domContentLoadedEventEnd", js_performance_timing_get_domContentLoadedEventEnd, NULL),
    JS_CGETSET_DEF("domComplete", js_performance_timing_get_domComplete, NULL),
    JS_CGETSET_DEF("loadEventStart", js_performance_timing_get_loadEventStart, NULL),
    JS_CGETSET_DEF("loadEventEnd", js_performance_timing_get_loadEventEnd, NULL),
    JS_CGETSET_DEF("toJSON", js_performance_timing_get_navigationStart, NULL), // stub
};
const size_t js_performance_timing_proto_funcs_count = sizeof(js_performance_timing_proto_funcs) / sizeof(js_performance_timing_proto_funcs[0]);

// Performance.timing getter - returns a simple object with timing properties
GCValue js_performance_get_timing(JSContextHandle ctx, GCValue this_val) {
    // Get the timing object from the Performance instance's opaque data
    // For simplicity, we store the timing object as a property on the performance instance
    GCValue timing_prop = JS_GetPropertyStr(ctx, this_val, "__timing");
    if (!JS_IsUndefined(timing_prop) && !JS_IsNull(timing_prop)) {
        return timing_prop;
    }

    // Create a simple timing object with all properties set to 0
    GCValue timing_obj = JS_NewObject(ctx);
    
    // Set all timing properties to 0
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
    
    // Store on the performance instance
    JS_SetPropertyStr(ctx, this_val, "__timing", timing_obj);
    
    return timing_obj;
}

const JSCFunctionListEntry js_performance_proto_funcs[] = {
    JS_CFUNC_DEF("now", 0, js_performance_now),
    JS_CGETSET_DEF("timeOrigin", js_performance_get_time_origin, NULL),
    // Note: timing is set directly on the instance, not as a getter on the prototype
    JS_CFUNC_DEF("getEntries", 0, js_performance_get_entries),
    JS_CFUNC_DEF("getEntriesByType", 1, js_performance_get_entries_by_type),
    JS_CFUNC_DEF("getEntriesByName", 1, js_performance_get_entries_by_name),
    JS_CFUNC_DEF("mark", 1, js_performance_mark),
    JS_CFUNC_DEF("measure", 1, js_performance_measure),
    JS_CFUNC_DEF("clearMarks", 0, js_performance_clear_marks),
    JS_CFUNC_DEF("clearMeasures", 0, js_performance_clear_measures),
    JS_CFUNC_DEF("clearResourceTimings", 0, js_performance_clear_resource_timings),
};
const size_t js_performance_proto_funcs_count = sizeof(js_performance_proto_funcs) / sizeof(js_performance_proto_funcs[0]);

// PerformanceEntry.name getter
GCValue js_performance_entry_get_name(JSContextHandle ctx, GCValue this_val) {
    PerformanceEntryDataHandle entry = PerformanceEntryDataHandle::from_object_check(ctx, this_val);
    if (!entry.valid()) return JS_ThrowTypeError(ctx, "Invalid PerformanceEntry");
    return JS_NewString(ctx, entry.name());
}

// PerformanceEntry.entryType getter
GCValue js_performance_entry_get_entry_type(JSContextHandle ctx, GCValue this_val) {
    PerformanceEntryDataHandle entry = PerformanceEntryDataHandle::from_object_check(ctx, this_val);
    if (!entry.valid()) return JS_ThrowTypeError(ctx, "Invalid PerformanceEntry");
    return JS_NewString(ctx, entry.entry_type());
}

// PerformanceEntry.startTime getter
GCValue js_performance_entry_get_start_time(JSContextHandle ctx, GCValue this_val) {
    PerformanceEntryDataHandle entry = PerformanceEntryDataHandle::from_object_check(ctx, this_val);
    if (!entry.valid()) return JS_ThrowTypeError(ctx, "Invalid PerformanceEntry");
    return JS_NewFloat64(ctx, entry.start_time());
}

// PerformanceEntry.duration getter
GCValue js_performance_entry_get_duration(JSContextHandle ctx, GCValue this_val) {
    PerformanceEntryDataHandle entry = PerformanceEntryDataHandle::from_object_check(ctx, this_val);
    if (!entry.valid()) return JS_ThrowTypeError(ctx, "Invalid PerformanceEntry");
    return JS_NewFloat64(ctx, entry.duration());
}

const JSCFunctionListEntry js_performance_entry_proto_funcs[] = {
    JS_CGETSET_DEF("name", js_performance_entry_get_name, NULL),
    JS_CGETSET_DEF("entryType", js_performance_entry_get_entry_type, NULL),
    JS_CGETSET_DEF("startTime", js_performance_entry_get_start_time, NULL),
    JS_CGETSET_DEF("duration", js_performance_entry_get_duration, NULL),
};
const size_t js_performance_entry_proto_funcs_count = sizeof(js_performance_entry_proto_funcs) / sizeof(js_performance_entry_proto_funcs[0]);

// PerformanceObserver constructor
GCValue js_performance_observer_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "PerformanceObserver constructor requires a callback function");
    }
    
    PerformanceObserverDataHandle po = PerformanceObserverDataHandle::create(ctx, argv[0]);
    if (!po.valid()) return JS_ThrowTypeError(ctx, "Invalid PerformanceObserver");
    
    GCValue obj = JS_NewObjectClass(ctx, js_performance_observer_class_id);
    po.attach_to_object(obj);
    return obj;
}

// PerformanceObserver.prototype.observe(options)
GCValue js_performance_observer_observe(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// PerformanceObserver.prototype.disconnect()
GCValue js_performance_observer_disconnect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// PerformanceObserver.prototype.takeRecords()
GCValue js_performance_observer_takeRecords(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_NewArray(ctx);
}

// PerformanceObserver.supportedEntryTypes getter
GCValue js_performance_observer_get_supported_entry_types(JSContextHandle ctx, GCValue this_val) {
    // Return an array of supported entry types
    GCValue array = JS_NewArray(ctx);
    return array;
}

const JSCFunctionListEntry js_performance_observer_proto_funcs[] = {
    JS_CFUNC_DEF("observe", 1, js_performance_observer_observe),
    JS_CFUNC_DEF("disconnect", 0, js_performance_observer_disconnect),
    JS_CFUNC_DEF("takeRecords", 0, js_performance_observer_takeRecords),
    JS_CGETSET_DEF("supportedEntryTypes", js_performance_observer_get_supported_entry_types, NULL),
};
const size_t js_performance_observer_proto_funcs_count = sizeof(js_performance_observer_proto_funcs) / sizeof(js_performance_observer_proto_funcs[0]);

// ============================================================================
// DOMRect Implementation
// ============================================================================

// DOMRectData struct is defined in browser_api_impl_types.h

void js_dom_rect_finalizer(JSRuntimeHandle rt, GCValue val) {
    // DOMRectData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

void js_dom_rect_read_only_finalizer(JSRuntimeHandle rt, GCValue val) {
    // DOMRectData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

JSClassDef js_dom_rect_class_def = {
    .class_name = "DOMRect",
    .finalizer = js_dom_rect_finalizer,
};

JSClassDef js_dom_rect_read_only_class_def = {
    .class_name = "DOMRectReadOnly",
    .finalizer = js_dom_rect_read_only_finalizer,
};

// Element.prototype.getBoundingClientRect - returns a DOMRect
GCValue js_element_getBoundingClientRect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMRectDataHandle rect = DOMRectDataHandle::create();
    if (!rect.valid()) return JS_ThrowTypeError(ctx, "Invalid DOMRect");
    
    rect.set_x(0);
    rect.set_y(0);
    rect.set_width(640);
    rect.set_height(360);
    rect.compute_bounds();
    
    GCValue obj = JS_NewObjectClass(ctx, js_dom_rect_class_id);
    rect.attach_to_object(obj);
    return obj;
}

// DOMRect constructor
GCValue js_dom_rect_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    DOMRectDataHandle rect = DOMRectDataHandle::create();
    if (!rect.valid()) return JS_ThrowTypeError(ctx, "Invalid DOMRect");
    
    double x = 0, y = 0, width = 0, height = 0;
    
    if (argc > 0) JS_ToFloat64(ctx, &x, argv[0]);
    if (argc > 1) JS_ToFloat64(ctx, &y, argv[1]);
    if (argc > 2) JS_ToFloat64(ctx, &width, argv[2]);
    if (argc > 3) JS_ToFloat64(ctx, &height, argv[3]);
    
    rect.set_x(x);
    rect.set_y(y);
    rect.set_width(width);
    rect.set_height(height);
    rect.compute_bounds();
    
    GCValue obj = JS_NewObjectClass(ctx, js_dom_rect_class_id);
    rect.attach_to_object(obj);
    return obj;
}

// DOMRectReadOnly constructor
GCValue js_dom_rect_read_only_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    DOMRectDataHandle rect = DOMRectDataHandle::create();
    if (!rect.valid()) return JS_ThrowTypeError(ctx, "Invalid DOMRect");
    
    double x = 0, y = 0, width = 0, height = 0;
    
    if (argc > 0) JS_ToFloat64(ctx, &x, argv[0]);
    if (argc > 1) JS_ToFloat64(ctx, &y, argv[1]);
    if (argc > 2) JS_ToFloat64(ctx, &width, argv[2]);
    if (argc > 3) JS_ToFloat64(ctx, &height, argv[3]);
    
    rect.set_x(x);
    rect.set_y(y);
    rect.set_width(width);
    rect.set_height(height);
    rect.compute_bounds();
    
    GCValue obj = JS_NewObjectClass(ctx, js_dom_rect_read_only_class_id);
    rect.attach_to_object(obj);
    return obj;
}

// DOMRect.fromRect(other)
GCValue js_dom_rect_from_rect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    double x = 0, y = 0, width = 0, height = 0;
    
    if (argc > 0 && JS_IsObject(argv[0])) {
        GCValue x_val = JS_GetPropertyStr(ctx, argv[0], "x");
        GCValue y_val = JS_GetPropertyStr(ctx, argv[0], "y");
        GCValue w_val = JS_GetPropertyStr(ctx, argv[0], "width");
        GCValue h_val = JS_GetPropertyStr(ctx, argv[0], "height");
        
        JS_ToFloat64(ctx, &x, x_val);
        JS_ToFloat64(ctx, &y, y_val);
        JS_ToFloat64(ctx, &width, w_val);
        JS_ToFloat64(ctx, &height, h_val);




    }
    
    GCValue args[4] = {
        JS_NewFloat64(ctx, x),
        JS_NewFloat64(ctx, y),
        JS_NewFloat64(ctx, width),
        JS_NewFloat64(ctx, height)
    };
    
    GCValue result = js_dom_rect_constructor(ctx, JS_UNDEFINED, 4, args);




    return result;
}

// DOMRectReadOnly.fromRect(other)
GCValue js_dom_rect_read_only_from_rect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    double x = 0, y = 0, width = 0, height = 0;
    
    if (argc > 0 && JS_IsObject(argv[0])) {
        GCValue x_val = JS_GetPropertyStr(ctx, argv[0], "x");
        GCValue y_val = JS_GetPropertyStr(ctx, argv[0], "y");
        GCValue w_val = JS_GetPropertyStr(ctx, argv[0], "width");
        GCValue h_val = JS_GetPropertyStr(ctx, argv[0], "height");
        
        JS_ToFloat64(ctx, &x, x_val);
        JS_ToFloat64(ctx, &y, y_val);
        JS_ToFloat64(ctx, &width, w_val);
        JS_ToFloat64(ctx, &height, h_val);




    }
    
    GCValue args[4] = {
        JS_NewFloat64(ctx, x),
        JS_NewFloat64(ctx, y),
        JS_NewFloat64(ctx, width),
        JS_NewFloat64(ctx, height)
    };
    
    GCValue result = js_dom_rect_read_only_constructor(ctx, JS_UNDEFINED, 4, args);




    return result;
}

#define DEF_DOM_RECT_GETTER(name, getter_func) \
static GCValue js_dom_rect_get_##name(JSContextHandle ctx, GCValue this_val) { \
    DOMRectDataHandle rect = DOMRectDataHandle::from_dom_rect(this_val); \
    if (!rect.valid()) { \
        rect = DOMRectDataHandle::from_dom_rect_read_only(this_val); \
        if (!rect.valid()) return JS_ThrowTypeError(ctx, "Invalid DOMRect"); \
    } \
    return JS_NewFloat64(ctx, rect.getter_func()); \
}

DEF_DOM_RECT_GETTER(x, x)
DEF_DOM_RECT_GETTER(y, y)
DEF_DOM_RECT_GETTER(width, width)
DEF_DOM_RECT_GETTER(height, height)
DEF_DOM_RECT_GETTER(top, top)
DEF_DOM_RECT_GETTER(right, right)
DEF_DOM_RECT_GETTER(bottom, bottom)
DEF_DOM_RECT_GETTER(left, left)

#undef DEF_DOM_RECT_GETTER

#define DEF_DOM_RECT_SETTER(name, setter_func) \
static GCValue js_dom_rect_set_##name(JSContextHandle ctx, GCValue this_val, GCValue val) { \
    DOMRectDataHandle rect = DOMRectDataHandle::from_dom_rect(this_val); \
    if (!rect.valid()) return JS_ThrowTypeError(ctx, "Invalid DOMRect"); \
    double value; \
    JS_ToFloat64(ctx, &value, val); \
    rect.setter_func(value); \
    rect.compute_bounds(); \
    return JS_UNDEFINED; \
}

DEF_DOM_RECT_SETTER(x, set_x)
DEF_DOM_RECT_SETTER(y, set_y)
DEF_DOM_RECT_SETTER(width, set_width)
DEF_DOM_RECT_SETTER(height, set_height)

#undef DEF_DOM_RECT_SETTER

const JSCFunctionListEntry js_dom_rect_proto_funcs[] = {
    JS_CGETSET_DEF("x", js_dom_rect_get_x, js_dom_rect_set_x),
    JS_CGETSET_DEF("y", js_dom_rect_get_y, js_dom_rect_set_y),
    JS_CGETSET_DEF("width", js_dom_rect_get_width, js_dom_rect_set_width),
    JS_CGETSET_DEF("height", js_dom_rect_get_height, js_dom_rect_set_height),
    JS_CGETSET_DEF("top", js_dom_rect_get_top, NULL),
    JS_CGETSET_DEF("right", js_dom_rect_get_right, NULL),
    JS_CGETSET_DEF("bottom", js_dom_rect_get_bottom, NULL),
    JS_CGETSET_DEF("left", js_dom_rect_get_left, NULL),
    JS_CFUNC_DEF("toJSON", 0, js_empty_string),
};
const size_t js_dom_rect_proto_funcs_count = sizeof(js_dom_rect_proto_funcs) / sizeof(js_dom_rect_proto_funcs[0]);

const JSCFunctionListEntry js_dom_rect_read_only_proto_funcs[] = {
    JS_CGETSET_DEF("x", js_dom_rect_get_x, NULL),
    JS_CGETSET_DEF("y", js_dom_rect_get_y, NULL),
    JS_CGETSET_DEF("width", js_dom_rect_get_width, NULL),
    JS_CGETSET_DEF("height", js_dom_rect_get_height, NULL),
    JS_CGETSET_DEF("top", js_dom_rect_get_top, NULL),
    JS_CGETSET_DEF("right", js_dom_rect_get_right, NULL),
    JS_CGETSET_DEF("bottom", js_dom_rect_get_bottom, NULL),
    JS_CGETSET_DEF("left", js_dom_rect_get_left, NULL),
    JS_CFUNC_DEF("toJSON", 0, js_empty_string),
};
const size_t js_dom_rect_read_only_proto_funcs_count = sizeof(js_dom_rect_read_only_proto_funcs) / sizeof(js_dom_rect_read_only_proto_funcs[0]);

// ============================================================================
// Date API Implementation
// ============================================================================

void js_date_finalizer(JSRuntimeHandle rt, GCValue val) {
    // DateData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

JSClassDef js_date_class_def = {
    .class_name = "Date",
    .finalizer = js_date_finalizer,
};

// Helper: Get current time in milliseconds using platform API
long long date_get_current_time_ms(void) {
    return (long long)platform_get_time_ms();
}

// Helper: Parse ISO date string to timestamp (simplified)
long long date_parse_iso_string(const char *str) {
    struct tm tm = {0};
    int ms = 0;
    int tz_offset = 0;
    
    // Try ISO 8601 format: YYYY-MM-DDTHH:MM:SS.sssZ or YYYY-MM-DDTHH:MM:SS.sss+HH:MM
    if (sscanf(str, "%d-%d-%dT%d:%d:%d.%dZ", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &ms) >= 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        time_t t = timegm(&tm);
        if (t != -1) return (long long)t * 1000 + ms;
    }
    
    // Try without milliseconds: YYYY-MM-DDTHH:MM:SSZ
    if (sscanf(str, "%d-%d-%dT%d:%d:%dZ", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        time_t t = timegm(&tm);
        if (t != -1) return (long long)t * 1000;
    }
    
    // Try simple date format: YYYY-MM-DD
    if (sscanf(str, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) >= 3) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm.tm_hour = 0;
        tm.tm_min = 0;
        tm.tm_sec = 0;
        time_t t = timegm(&tm);
        if (t != -1) return (long long)t * 1000;
    }
    
    return 0; // Invalid date
}

// Helper: Convert timestamp to broken-down time
void date_to_utc_time(long long timestamp_ms, struct tm *out_tm, int *out_ms) {
    time_t seconds = (time_t)(timestamp_ms / 1000);
    *out_ms = (int)(timestamp_ms % 1000);
    if (*out_ms < 0) {
        *out_ms += 1000;
        seconds -= 1;
    }
    gmtime_r(&seconds, out_tm);
}

// Helper: Convert timestamp to local time
void date_to_local_time(long long timestamp_ms, struct tm *out_tm, int *out_ms) {
    time_t seconds = (time_t)(timestamp_ms / 1000);
    *out_ms = (int)(timestamp_ms % 1000);
    if (*out_ms < 0) {
        *out_ms += 1000;
        seconds -= 1;
    }
    localtime_r(&seconds, out_tm);
}

// Date constructor
GCValue js_date_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    DateDataHandle date = DateDataHandle::create();
    if (!date.valid()) return JS_ThrowTypeError(ctx, "Invalid Date");
    
    long long timestamp_ms = 0;
    int is_valid = 1;
    
    if (argc == 0) {
        // new Date() - current time
        timestamp_ms = date_get_current_time_ms();
    } else if (argc == 1) {
        // new Date(value) - value can be string or number
        if (JS_IsString(argv[0])) {
            const char *str = JS_ToCString(ctx, argv[0]);
            if (str) {
                timestamp_ms = date_parse_iso_string(str);
            }
        } else if (JS_IsNumber(argv[0])) {
            double val;
            JS_ToFloat64(ctx, &val, argv[0]);
            timestamp_ms = (long long)val;
        } else if (JS_IsObject(argv[0])) {
            // Check if it's another Date object
            DateDataHandle other = DateDataHandle::from_object_check(ctx, argv[0]);
            if (other.valid()) {
                timestamp_ms = other.timestamp_ms();
                is_valid = other.is_valid();
            } else {
                is_valid = 0;
            }
        } else {
            is_valid = 0;
        }
    } else {
        // new Date(year, month, day, hours, minutes, seconds, ms)
        double year = 0, month = 0, day = 1, hours = 0, minutes = 0, seconds = 0, ms = 0;
        JS_ToFloat64(ctx, &year, argv[0]);
        JS_ToFloat64(ctx, &month, argv[1]);
        if (argc > 2) JS_ToFloat64(ctx, &day, argv[2]);
        if (argc > 3) JS_ToFloat64(ctx, &hours, argv[3]);
        if (argc > 4) JS_ToFloat64(ctx, &minutes, argv[4]);
        if (argc > 5) JS_ToFloat64(ctx, &seconds, argv[5]);
        if (argc > 6) JS_ToFloat64(ctx, &ms, argv[6]);
        
        // Handle 2-digit years
        if (year >= 0 && year <= 99) {
            year += 1900;
        }
        
        struct tm tm = {0};
        tm.tm_year = (int)year - 1900;
        tm.tm_mon = (int)month;
        tm.tm_mday = (int)day;
        tm.tm_hour = (int)hours;
        tm.tm_min = (int)minutes;
        tm.tm_sec = (int)seconds;
        
        time_t t = timegm(&tm);
        if (t != -1) {
            timestamp_ms = (long long)t * 1000 + (long long)ms;
        } else {
            is_valid = 0;
        }
    }
    
    date.set_timestamp_ms(timestamp_ms);
    date.set_valid(is_valid);
    
    GCValue obj = JS_NewObjectClass(ctx, js_date_class_id);
    date.attach_to_object(obj);
    return obj;
}

// Date.prototype.getTime()
GCValue js_date_getTime(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid()) return JS_ThrowTypeError(ctx, "Invalid Date object");
    if (!date.is_valid()) return JS_NewFloat64(ctx, NAN);
    return JS_NewFloat64(ctx, (double)date.timestamp_ms());
}

// Date.prototype.valueOf() - same as getTime
GCValue js_date_valueOf(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_date_getTime(ctx, this_val, argc, argv);
}

// Date.prototype.getFullYear()
GCValue js_date_getFullYear(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_year + 1900);
}

// Date.prototype.getUTCFullYear()
GCValue js_date_getUTCFullYear(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_year + 1900);
}

// Date.prototype.getMonth()
GCValue js_date_getMonth(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_mon);
}

// Date.prototype.getUTCMonth()
GCValue js_date_getUTCMonth(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_mon);
}

// Date.prototype.getDate()
GCValue js_date_getDate(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_mday);
}

// Date.prototype.getUTCDate()
GCValue js_date_getUTCDate(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_mday);
}

// Date.prototype.getDay()
GCValue js_date_getDay(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_wday);
}

// Date.prototype.getUTCDay()
GCValue js_date_getUTCDay(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_wday);
}

// Date.prototype.getHours()
GCValue js_date_getHours(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_hour);
}

// Date.prototype.getUTCHours()
GCValue js_date_getUTCHours(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_hour);
}

// Date.prototype.getMinutes()
GCValue js_date_getMinutes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_min);
}

// Date.prototype.getUTCMinutes()
GCValue js_date_getUTCMinutes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_min);
}

// Date.prototype.getSeconds()
GCValue js_date_getSeconds(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_sec);
}

// Date.prototype.getUTCSeconds()
GCValue js_date_getUTCSeconds(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, tm.tm_sec);
}

// Date.prototype.getMilliseconds()
GCValue js_date_getMilliseconds(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, ms);
}

// Date.prototype.getUTCMilliseconds()
GCValue js_date_getUTCMilliseconds(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    return JS_NewInt32(ctx, ms);
}

// Date.prototype.getTimezoneOffset()
GCValue js_date_getTimezoneOffset(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewFloat64(ctx, NAN);
    
    struct tm local_tm, utc_tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &local_tm, &ms);
    date_to_utc_time(date.timestamp_ms(), &utc_tm, &ms);
    
    time_t local_t = mktime(&local_tm);
    time_t utc_t = timegm(&utc_tm);
    int offset_minutes = (int)((local_t - utc_t) / 60);
    
    return JS_NewInt32(ctx, offset_minutes);
}

// Date.prototype.toString()
GCValue js_date_toString(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewString(ctx, "Invalid Date");
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    
    const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    
    char buf[256];
    snprintf(buf, sizeof(buf), "%s %s %02d %04d %02d:%02d:%02d GMT%+.4d",
             days[tm.tm_wday],
             months[tm.tm_mon],
             tm.tm_mday,
             tm.tm_year + 1900,
             tm.tm_hour,
             tm.tm_min,
             tm.tm_sec,
             0); // Simplified timezone offset
    
    return JS_NewString(ctx, buf);
}

// Date.prototype.toDateString()
GCValue js_date_toDateString(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewString(ctx, "Invalid Date");
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    
    const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    
    char buf[64];
    snprintf(buf, sizeof(buf), "%s %s %02d %04d",
             days[tm.tm_wday],
             months[tm.tm_mon],
             tm.tm_mday,
             tm.tm_year + 1900);
    
    return JS_NewString(ctx, buf);
}

// Date.prototype.toTimeString()
GCValue js_date_toTimeString(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewString(ctx, "Invalid Date");
    
    struct tm tm;
    int ms;
    date_to_local_time(date.timestamp_ms(), &tm, &ms);
    
    char buf[64];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d GMT%+.4d",
             tm.tm_hour,
             tm.tm_min,
             tm.tm_sec,
             0); // Simplified timezone offset
    
    return JS_NewString(ctx, buf);
}

// Date.prototype.toUTCString()
GCValue js_date_toUTCString(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) return JS_NewString(ctx, "Invalid Date");
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    
    const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    
    char buf[64];
    snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT",
             days[tm.tm_wday],
             tm.tm_mday,
             months[tm.tm_mon],
             tm.tm_year + 1900,
             tm.tm_hour,
             tm.tm_min,
             tm.tm_sec);
    
    return JS_NewString(ctx, buf);
}

// Date.prototype.toISOString()
GCValue js_date_toISOString(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DateDataHandle date = DateDataHandle::from_object_check(ctx, this_val);
    if (!date.valid() || !date.is_valid()) {
        return JS_ThrowRangeError(ctx, "Invalid Date");
    }
    
    struct tm tm;
    int ms;
    date_to_utc_time(date.timestamp_ms(), &tm, &ms);
    
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm.tm_year + 1900,
             tm.tm_mon + 1,
             tm.tm_mday,
             tm.tm_hour,
             tm.tm_min,
             tm.tm_sec,
             ms);
    
    return JS_NewString(ctx, buf);
}

// Date.prototype.toJSON()
GCValue js_date_toJSON(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_date_toISOString(ctx, this_val, argc, argv);
}

// Date.prototype[Symbol.toPrimitive]
GCValue js_date_toPrimitive(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    
    const char *hint = JS_ToCString(ctx, argv[0]);
    if (!hint) return JS_UNDEFINED;
    
    GCValue result;
    if (strcmp(hint, "string") == 0 || strcmp(hint, "default") == 0) {
        result = js_date_toString(ctx, this_val, 0, NULL);
    } else {
        result = js_date_valueOf(ctx, this_val, 0, NULL);
    }
    
    return result;
}

// Date.now() - static method
GCValue js_date_now(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, (double)date_get_current_time_ms());
}

// Date.parse() - static method
GCValue js_date_parse(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0])) return JS_NewFloat64(ctx, NAN);
    
    const char *str = JS_ToCString(ctx, argv[0]);
    if (!str) return JS_NewFloat64(ctx, NAN);
    
    long long ts = date_parse_iso_string(str);
    
    return JS_NewFloat64(ctx, (double)ts);
}

// Date.UTC() - static method
GCValue js_date_UTC(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_NewFloat64(ctx, NAN);
    
    double year = 0, month = 0, day = 1, hours = 0, minutes = 0, seconds = 0, ms = 0;
    JS_ToFloat64(ctx, &year, argv[0]);
    JS_ToFloat64(ctx, &month, argv[1]);
    if (argc > 2) JS_ToFloat64(ctx, &day, argv[2]);
    if (argc > 3) JS_ToFloat64(ctx, &hours, argv[3]);
    if (argc > 4) JS_ToFloat64(ctx, &minutes, argv[4]);
    if (argc > 5) JS_ToFloat64(ctx, &seconds, argv[5]);
    if (argc > 6) JS_ToFloat64(ctx, &ms, argv[6]);
    
    // Handle 2-digit years
    if (year >= 0 && year <= 99) {
        year += 1900;
    }
    
    struct tm tm = {0};
    tm.tm_year = (int)year - 1900;
    tm.tm_mon = (int)month;
    tm.tm_mday = (int)day;
    tm.tm_hour = (int)hours;
    tm.tm_min = (int)minutes;
    tm.tm_sec = (int)seconds;
    
    time_t t = timegm(&tm);
    if (t != -1) {
        return JS_NewFloat64(ctx, (double)((long long)t * 1000 + (long long)ms));
    }
    return JS_NewFloat64(ctx, NAN);
}

const JSCFunctionListEntry js_date_proto_funcs[] = {
    JS_CFUNC_DEF("getTime", 0, js_date_getTime),
    JS_CFUNC_DEF("valueOf", 0, js_date_valueOf),
    JS_CFUNC_DEF("getFullYear", 0, js_date_getFullYear),
    JS_CFUNC_DEF("getUTCFullYear", 0, js_date_getUTCFullYear),
    JS_CFUNC_DEF("getMonth", 0, js_date_getMonth),
    JS_CFUNC_DEF("getUTCMonth", 0, js_date_getUTCMonth),
    JS_CFUNC_DEF("getDate", 0, js_date_getDate),
    JS_CFUNC_DEF("getUTCDate", 0, js_date_getUTCDate),
    JS_CFUNC_DEF("getDay", 0, js_date_getDay),
    JS_CFUNC_DEF("getUTCDay", 0, js_date_getUTCDay),
    JS_CFUNC_DEF("getHours", 0, js_date_getHours),
    JS_CFUNC_DEF("getUTCHours", 0, js_date_getUTCHours),
    JS_CFUNC_DEF("getMinutes", 0, js_date_getMinutes),
    JS_CFUNC_DEF("getUTCMinutes", 0, js_date_getUTCMinutes),
    JS_CFUNC_DEF("getSeconds", 0, js_date_getSeconds),
    JS_CFUNC_DEF("getUTCSeconds", 0, js_date_getUTCSeconds),
    JS_CFUNC_DEF("getMilliseconds", 0, js_date_getMilliseconds),
    JS_CFUNC_DEF("getUTCMilliseconds", 0, js_date_getUTCMilliseconds),
    JS_CFUNC_DEF("getTimezoneOffset", 0, js_date_getTimezoneOffset),
    JS_CFUNC_DEF("toString", 0, js_date_toString),
    JS_CFUNC_DEF("toDateString", 0, js_date_toDateString),
    JS_CFUNC_DEF("toTimeString", 0, js_date_toTimeString),
    JS_CFUNC_DEF("toUTCString", 0, js_date_toUTCString),
    JS_CFUNC_DEF("toISOString", 0, js_date_toISOString),
    JS_CFUNC_DEF("toJSON", 1, js_date_toJSON),
    JS_CFUNC_DEF("toLocaleString", 0, js_date_toString),
    JS_CFUNC_DEF("toLocaleDateString", 0, js_date_toDateString),
    JS_CFUNC_DEF("toLocaleTimeString", 0, js_date_toTimeString),
};
const size_t js_date_proto_funcs_count = sizeof(js_date_proto_funcs) / sizeof(js_date_proto_funcs[0]);

const JSCFunctionListEntry js_date_static_funcs[] = {
    JS_CFUNC_DEF("now", 0, js_date_now),
    JS_CFUNC_DEF("parse", 1, js_date_parse),
    JS_CFUNC_DEF("UTC", 7, js_date_UTC),
};
const size_t js_date_static_funcs_count = sizeof(js_date_static_funcs) / sizeof(js_date_static_funcs[0]);


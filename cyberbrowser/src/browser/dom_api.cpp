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

/* Global layout-invalidation flag. DOM mutation functions set this to 1 so the
 * main loop knows the native HtmlDocument is stale and must be rebuilt from the
 * current JS DOM before the next layout/render pass. */
volatile int g_dom_needs_layout = 0;

void dom_request_layout(void) {
    g_dom_needs_layout = 1;
}

// Real DOM Node Implementation
// ============================================================================

JSClassID js_dom_node_class_id = 0;

// Forward declarations for DOM functions
GCValue js_node_appendChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_removeChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_insertBefore_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_cloneNode_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_contains_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_getRootNode_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_get_ownerDocument(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_querySelector_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_querySelectorAll_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

// Forward declarations for DOM helper functions (used by ShadowRoot)
bool matches_selector(JSContextHandle ctx, GCValue elem, const char* selector);
DOMNodeHandle get_dom_node(JSContextHandle ctx, GCValue obj);
DOMNodeHandle get_or_create_dom_node(JSContextHandle ctx, GCValue obj, int node_type, const char* node_name);
GCValue query_selector_recursive(JSContextHandle ctx, GCValue elem, const char* selector);
void query_selector_all_recursive(JSContextHandle ctx, GCValue elem, const char* selector, GCValue result_arr, int* idx);

void js_dom_node_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    // Free the lock-free computed-style hash table.  The table itself is
    // malloc'd; the GC will reclaim the CssComputedStyle object separately.
    GCHandle node_handle = JS_GetOpaqueHandle(val, js_dom_node_class_id);
    if (node_handle != GC_HANDLE_NULL) {
        DOMNodeHandle node(node_handle);
        if (node.valid()) {
            GCHandle cs_handle = node.computed_style_handle();
            if (cs_handle != GC_HANDLE_NULL) {
                CssComputedStyle *cs = (CssComputedStyle *)gc_deref(cs_handle);
                if (cs && cs->properties) {
                    lf_hash_destroy(cs->properties);
                    cs->properties = NULL;
                }
            }
        }
    }
}

void js_dom_node_mark(JSRuntimeHandle rt, GCValue val,
                             JS_MarkFunc *mark_func)
{
    (void)rt;
    GCHandle node_handle = JS_GetOpaqueHandle(val, js_dom_node_class_id);
    if (node_handle == GC_HANDLE_NULL) return;

    /* Keep the DOMNode data object alive; it is opaque data, not scanned
     * automatically by the generic object marker. */
    mark_func(rt, node_handle);

    DOMNodeHandle node(node_handle);
    if (!node.valid()) return;

    /* Tree links are stored as GCValue JS object references. */
    JS_MarkValue(rt, node.js_object(), mark_func);
    JS_MarkValue(rt, node.parent_node(), mark_func);
    JS_MarkValue(rt, node.first_child(), mark_func);
    JS_MarkValue(rt, node.last_child(), mark_func);
    JS_MarkValue(rt, node.previous_sibling(), mark_func);
    JS_MarkValue(rt, node.next_sibling(), mark_func);
    JS_MarkValue(rt, node.owner_document(), mark_func);
    JS_MarkValue(rt, node.shadow_root(), mark_func);

    /* Computed-style table.  Keep the table object alive, then mark every
     * value handle stored inside it (they are JS strings). */
    GCHandle cs_handle = node.computed_style_handle();
    if (cs_handle != GC_HANDLE_NULL) {
        mark_func(rt, cs_handle);
        CssComputedStyle *cs = (CssComputedStyle *)gc_deref(cs_handle);
        if (cs && cs->properties) {
            LFHashTable *t = cs->properties;
            for (uint32_t i = 0; i < t->bucket_count; i++) {
                if (t->buckets[i].state == LF_HASH_OCCUPIED &&
                    t->buckets[i].value != GC_HANDLE_NULL) {
                    mark_func(rt, t->buckets[i].value);
                }
            }
        }
    }

    /* Index-table list chaining refers to other DOMNode data handles. */
    GCHandle class_sib = node.next_class_sibling();
    if (class_sib != GC_HANDLE_NULL) mark_func(rt, class_sib);
    GCHandle tag_sib = node.next_tag_sibling();
    if (tag_sib != GC_HANDLE_NULL) mark_func(rt, tag_sib);
}

JSClassDef js_dom_node_class_def = {
    .class_name = "DOMNode",
    .finalizer = js_dom_node_finalizer,
    .gc_mark   = js_dom_node_mark,
};

/* ============================================================================
 * Parallel CSS support: per-element computed-style table and index tables
 * ============================================================================ */

#define CSS_COMPUTED_STYLE_BUCKETS 16
#define CSS_ID_TABLE_BUCKETS       64
#define CSS_CLASS_TABLE_BUCKETS    64
#define CSS_TAG_TABLE_BUCKETS      64

/* Allocate or return the existing computed-style object for a DOM node. */
GCHandle css_ensure_computed_style(DOMNodeHandle node)
{
    GCHandle h = node.computed_style_handle();
    if (h != GC_HANDLE_NULL) return h;

    h = gc_allocz(sizeof(CssComputedStyle), JS_GC_OBJ_TYPE_DATA);
    if (h == GC_HANDLE_NULL) return GC_HANDLE_NULL;

    CssComputedStyle *cs = (CssComputedStyle *)gc_deref(h);
    cs->properties = lf_hash_create(CSS_COMPUTED_STYLE_BUCKETS);
    if (!cs->properties) {
        /* Cannot free the GC handle individually in unified GC; leak is
         * harmless because the handle will be reclaimed at next collection. */
        return GC_HANDLE_NULL;
    }

    node.set_computed_style_handle(h);
    return h;
}

/* Store a computed CSS property for a node.  The value is copied into a JS
 * string handle that is kept alive by the DOMNode's gc_mark callback. */
void css_computed_set_property(JSContextHandle ctx, DOMNodeHandle node,
                               JSAtom prop_atom, const char *value)
{
    if (prop_atom == JS_ATOM_NULL || !value) return;

    GCHandle cs_handle = css_ensure_computed_style(node);
    if (cs_handle == GC_HANDLE_NULL) return;

    CssComputedStyle *cs = (CssComputedStyle *)gc_deref(cs_handle);
    if (!cs || !cs->properties) return;

    GCValue str_val = JS_NewString(ctx, value);
    GCHandle str_handle = GC_VALUE_GET_HANDLE(str_val);
    if (str_handle == GC_HANDLE_NULL) return;

    lf_hash_insert(cs->properties, (uint32_t)prop_atom,
                   (GCHandle)prop_atom, str_handle);

    /* Also store the camelCase alias so getComputedStyle().fontSize works. */
    const char *prop_str = JS_AtomToCString(ctx, prop_atom);
    if (prop_str) {
        char *camel = css_to_camel_case(prop_str);
        if (camel && strcmp(camel, prop_str) != 0) {
            JSAtom camel_atom = JS_NewAtom(ctx, camel);
            if (camel_atom != JS_ATOM_NULL) {
                lf_hash_insert(cs->properties, (uint32_t)camel_atom,
                               (GCHandle)camel_atom, str_handle);
                JS_FreeAtom(ctx, camel_atom);
            }
        }
        free(camel);
    }
}

/* Look up a computed CSS property and return it as a JS value.  Returns
 * JS_UNDEFINED if no computed value exists. */
GCValue css_computed_get_property(JSContextHandle ctx, DOMNodeHandle node,
                                  JSAtom prop_atom)
{
    if (prop_atom == JS_ATOM_NULL) return JS_UNDEFINED;

    GCHandle cs_handle = node.computed_style_handle();
    if (cs_handle == GC_HANDLE_NULL) return JS_UNDEFINED;

    CssComputedStyle *cs = (CssComputedStyle *)gc_deref(cs_handle);
    if (!cs || !cs->properties) return JS_UNDEFINED;

    GCHandle str_handle = lf_hash_lookup(cs->properties, (uint32_t)prop_atom,
                                         (GCHandle)prop_atom);
    if (str_handle == GC_HANDLE_NULL) return JS_UNDEFINED;

    return GC_MKHANDLE(JS_TAG_STRING, str_handle);
}

/* Apply a declaration list to the computed-style table.  The caller is
 * responsible for sorting by specificity if needed. */
void css_computed_apply_declarations(JSContextHandle ctx, DOMNodeHandle node,
                                     CssAppliedDecl *applied, int count)
{
    if (!node.valid() || count <= 0) return;
    for (int i = 0; i < count; i++) {
        JSAtom atom = JS_NewAtom(ctx, applied[i].decl->property);
        if (atom != JS_ATOM_NULL) {
            css_computed_set_property(ctx, node, atom, applied[i].decl->value);
            JS_FreeAtom(ctx, atom);
        }
    }
}

/* Parse an inline style attribute and store its declarations in the
 * computed-style table. */
void css_computed_apply_inline_style(JSContextHandle ctx, DOMNodeHandle node,
                                     const char *style_attr)
{
    if (!node.valid() || !style_attr || !style_attr[0]) return;

    int count = 0;
    CssDeclaration *decls = css_parse_inline_style(style_attr, &count);
    if (!decls) return;

    for (int i = 0; i < count; i++) {
        JSAtom atom = JS_NewAtom(ctx, decls[i].property);
        if (atom != JS_ATOM_NULL) {
            css_computed_set_property(ctx, node, atom, decls[i].value);
            JS_FreeAtom(ctx, atom);
        }
    }
    css_declarations_free(decls, count);
}

/* Allocate and attach the per-document CSS index tables. */
CssDocumentState *css_document_state_ensure(JSRuntimeHandle rt)
{
    CssDocumentState *state = (CssDocumentState *)JS_GetRuntimeOpaque(rt);
    if (state) return state;

    state = (CssDocumentState *)calloc(1, sizeof(CssDocumentState));
    if (!state) return NULL;

    state->id_table    = lf_hash_create(CSS_ID_TABLE_BUCKETS);
    state->class_table = lf_hash_create(CSS_CLASS_TABLE_BUCKETS);
    state->tag_table   = lf_hash_create(CSS_TAG_TABLE_BUCKETS);

    if (!state->id_table || !state->class_table || !state->tag_table) {
        if (state->id_table) lf_hash_destroy(state->id_table);
        if (state->class_table) lf_hash_destroy(state->class_table);
        if (state->tag_table) lf_hash_destroy(state->tag_table);
        free(state);
        return NULL;
    }

    JS_SetRuntimeOpaque(rt, state);
    return state;
}

/* Free the per-document CSS index tables.  Call before gc_cleanup(). */
void css_document_state_destroy(JSRuntimeHandle rt)
{
    CssDocumentState *state = (CssDocumentState *)JS_GetRuntimeOpaque(rt);
    if (!state) return;

    JS_SetRuntimeOpaque(rt, NULL);
    if (state->id_table) lf_hash_destroy(state->id_table);
    if (state->class_table) lf_hash_destroy(state->class_table);
    if (state->tag_table) lf_hash_destroy(state->tag_table);
    free(state);
}

/* Clear all entries from the CSS index tables.  Call when a new document is
 * populated so lookups do not return nodes from a previous document. */
void css_document_state_clear(JSRuntimeHandle rt)
{
    CssDocumentState *state = (CssDocumentState *)JS_GetRuntimeOpaque(rt);
    if (!state) return;

    if (state->id_table) { lf_hash_destroy(state->id_table); state->id_table = lf_hash_create(CSS_ID_TABLE_BUCKETS); }
    if (state->class_table) { lf_hash_destroy(state->class_table); state->class_table = lf_hash_create(CSS_CLASS_TABLE_BUCKETS); }
    if (state->tag_table) { lf_hash_destroy(state->tag_table); state->tag_table = lf_hash_create(CSS_TAG_TABLE_BUCKETS); }
}

/* Insert a DOM node into the id/class/tag index tables.  Must be called after
 * the node's id, class, and tag attributes are initialized and the node is
 * attached to its public JS object. */
void css_index_insert_node(JSContextHandle ctx, DOMNodeHandle node)
{
    if (!node.valid()) return;

    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    CssDocumentState *state = css_document_state_ensure(rt);
    if (!state) return;

    GCHandle node_handle = node.handle();
    GCValue js_obj = node.js_object();

    /* Read id/className from the JS object when available; the DOMNode fields
     * are not always kept in sync by the current property setters. */
    const char *id = node.id();
    char id_buf[256];
    if ((!id || !id[0]) && !JS_IsNull(js_obj) && !JS_IsUndefined(js_obj)) {
        GCValue id_val = JS_GetPropertyStr(ctx, js_obj, "id");
        const char *id_str = JS_ToCString(ctx, id_val);
        if (id_str) {
            strncpy(id_buf, id_str, sizeof(id_buf) - 1);
            id_buf[sizeof(id_buf) - 1] = '\0';
            id = id_buf;
        }
    }
    if (id && id[0]) {
        JSAtom id_atom = JS_NewAtom(ctx, id);
        if (id_atom != JS_ATOM_NULL) {
            lf_hash_insert(state->id_table, (uint32_t)id_atom,
                           (GCHandle)id_atom, node_handle);
            JS_FreeAtom(ctx, id_atom);
        }
    }

    const char *class_name = node.class_name();
    char class_buf[1024];
    if ((!class_name || !class_name[0]) && !JS_IsNull(js_obj) && !JS_IsUndefined(js_obj)) {
        GCValue class_val = JS_GetPropertyStr(ctx, js_obj, "className");
        const char *class_str = JS_ToCString(ctx, class_val);
        if (class_str) {
            strncpy(class_buf, class_str, sizeof(class_buf) - 1);
            class_buf[sizeof(class_buf) - 1] = '\0';
            class_name = class_buf;
        }
    }
    if (class_name && class_name[0]) {
        /* Class attribute may contain multiple classes separated by spaces. */
        char *copy = strdup(class_name);
        if (copy) {
            char *saveptr = NULL;
            for (char *tok = strtok_r(copy, " \t\r\n", &saveptr);
                 tok != NULL;
                 tok = strtok_r(NULL, " \t\r\n", &saveptr)) {
                if (!tok[0]) continue;
                JSAtom class_atom = JS_NewAtom(ctx, tok);
                if (class_atom == JS_ATOM_NULL) continue;

                GCHandle prev_head = lf_hash_lookup(state->class_table,
                                                    (uint32_t)class_atom,
                                                    (GCHandle)class_atom);
                node.set_next_class_sibling(prev_head);
                lf_hash_insert(state->class_table, (uint32_t)class_atom,
                               (GCHandle)class_atom, node_handle);
                JS_FreeAtom(ctx, class_atom);
            }
            free(copy);
        }
    }

    const char *tag = node.node_name();
    if (tag && tag[0]) {
        JSAtom tag_atom = JS_NewAtom(ctx, tag);
        if (tag_atom != JS_ATOM_NULL) {
            GCHandle prev_head = lf_hash_lookup(state->tag_table,
                                                (uint32_t)tag_atom,
                                                (GCHandle)tag_atom);
            node.set_next_tag_sibling(prev_head);
            lf_hash_insert(state->tag_table, (uint32_t)tag_atom,
                           (GCHandle)tag_atom, node_handle);
            JS_FreeAtom(ctx, tag_atom);
        }
    }
}

/* Return the DOM node JS object with the given id, or JS_NULL if none. */
GCValue css_get_element_by_id(JSContextHandle ctx, JSAtom id)
{
    if (id == JS_ATOM_NULL) return JS_NULL;

    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    CssDocumentState *state = (CssDocumentState *)JS_GetRuntimeOpaque(rt);
    if (!state || !state->id_table) return JS_NULL;

    GCHandle node_handle = lf_hash_lookup(state->id_table, (uint32_t)id,
                                          (GCHandle)id);
    if (node_handle == GC_HANDLE_NULL) return JS_NULL;

    DOMNodeHandle node(node_handle);
    if (!node.valid()) return JS_NULL;

    GCValue obj = node.js_object();
    if (JS_IsNull(obj) || JS_IsUndefined(obj)) return JS_NULL;
    return obj;
}

/* Return an array of DOM node JS objects with the given class name. */
GCValue css_get_elements_by_class_name(JSContextHandle ctx, JSAtom class_atom)
{
    if (class_atom == JS_ATOM_NULL) return JS_NewArray(ctx);

    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    CssDocumentState *state = (CssDocumentState *)JS_GetRuntimeOpaque(rt);
    if (!state || !state->class_table) return JS_NewArray(ctx);

    GCValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    GCHandle cur = lf_hash_lookup(state->class_table, (uint32_t)class_atom,
                                  (GCHandle)class_atom);
    while (cur != GC_HANDLE_NULL) {
        DOMNodeHandle node(cur);
        if (!node.valid()) break;
        GCValue obj = node.js_object();
        if (!JS_IsNull(obj) && !JS_IsUndefined(obj)) {
            JS_SetPropertyUint32(ctx, arr, idx++, obj);
        }
        cur = node.next_class_sibling();
    }
    return arr;
}

/* Return an array of DOM node JS objects with the given tag name. */
GCValue css_get_elements_by_tag_name(JSContextHandle ctx, JSAtom tag_atom)
{
    if (tag_atom == JS_ATOM_NULL) return JS_NewArray(ctx);

    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    CssDocumentState *state = (CssDocumentState *)JS_GetRuntimeOpaque(rt);
    if (!state || !state->tag_table) return JS_NewArray(ctx);

    GCValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    GCHandle cur = lf_hash_lookup(state->tag_table, (uint32_t)tag_atom,
                                  (GCHandle)tag_atom);
    while (cur != GC_HANDLE_NULL) {
        DOMNodeHandle node(cur);
        if (!node.valid()) break;
        GCValue obj = node.js_object();
        if (!JS_IsNull(obj) && !JS_IsUndefined(obj)) {
            JS_SetPropertyUint32(ctx, arr, idx++, obj);
        }
        cur = node.next_tag_sibling();
    }
    return arr;
}

// URL capture callback for intercepted media URLs
static URLCaptureCallback g_url_capture_callback = NULL;

void browser_api_impl_set_url_capture_callback(URLCaptureCallback callback) {
    g_url_capture_callback = callback;
}

// Helper to capture URLs
void capture_url(const char *url) {
    if (g_url_capture_callback && url && *url) {
        g_url_capture_callback(url);
    }
}

// Debug helper to capture URLs with source tracking
void capture_url_debug(const char *url, const char *source) {
    if (url && *url && strstr(url, "data:") == url) {
        fprintf(stderr, "[CAPTURE_DEBUG] [%s] data URL: %.100s\n", source, url);
    }
    capture_url(url);
}

// Helper to throw a DOMException
GCValue throw_dom_exception(JSContextHandle ctx, const char* name, const char* message) {
    // Create DOMException instance
    DOMExceptionDataHandle de = DOMExceptionDataHandle::create();
    if (!de.valid()) {
        return JS_ThrowTypeError(ctx, "%s: %s", name, message);
    }
    
    de.set_name(name);
    de.set_message(message);
    
    // Set appropriate code based on name
    int code = 0;
    if (strcmp(name, "IndexSizeError") == 0) code = 1;
    else if (strcmp(name, "HierarchyRequestError") == 0) code = 3;
    else if (strcmp(name, "WrongDocumentError") == 0) code = 4;
    else if (strcmp(name, "InvalidCharacterError") == 0) code = 5;
    else if (strcmp(name, "NoModificationAllowedError") == 0) code = 7;
    else if (strcmp(name, "NotFoundError") == 0) code = 8;
    else if (strcmp(name, "NotSupportedError") == 0) code = 9;
    else if (strcmp(name, "InvalidStateError") == 0) code = 11;
    else if (strcmp(name, "SyntaxError") == 0) code = 12;
    else if (strcmp(name, "InvalidModificationError") == 0) code = 13;
    else if (strcmp(name, "NamespaceError") == 0) code = 14;
    else if (strcmp(name, "InvalidAccessError") == 0) code = 15;
    else if (strcmp(name, "TypeMismatchError") == 0) code = 17;
    else if (strcmp(name, "SecurityError") == 0) code = 18;
    else if (strcmp(name, "NetworkError") == 0) code = 19;
    else if (strcmp(name, "AbortError") == 0) code = 20;
    else if (strcmp(name, "URLMismatchError") == 0) code = 21;
    else if (strcmp(name, "QuotaExceededError") == 0) code = 22;
    else if (strcmp(name, "TimeoutError") == 0) code = 23;
    else if (strcmp(name, "InvalidNodeTypeError") == 0) code = 24;
    else if (strcmp(name, "DataCloneError") == 0) code = 25;
    de.set_code(code);
    
    // Create the exception object
    GCValue exc = JS_NewObjectClass(ctx, js_dom_exception_class_id);
    if (JS_IsException(exc)) {
        return JS_ThrowTypeError(ctx, "%s: %s", name, message);
    }
    
    de.attach_to_object(exc);
    
    // Set name and message properties on the exception object
    JS_SetPropertyStr(ctx, exc, "name", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, exc, "message", JS_NewString(ctx, message));
    JS_SetPropertyStr(ctx, exc, "code", JS_NewInt32(ctx, code));
    
    return JS_Throw(ctx, exc);
}

// Helper macros

// Dummy then function for mock promises
GCValue js_promise_then(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Call the onFulfilled callback immediately with undefined
    if (argc > 0 && JS_IsFunction(ctx, argv[0])) {
        GCValue undefined = JS_UNDEFINED;
        GCValue result = JS_Call(ctx, argv[0], JS_UNDEFINED, 1, &undefined);

    }
    return this_val;
}

// Helper to create a resolved Promise
GCValue js_create_resolved_promise(JSContextHandle ctx, GCValue value) {
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    
    // Check if Promise constructor exists and is an object
    if (JS_IsException(promise_ctor) || !JS_IsObject(promise_ctor)) {


        // Fallback: return a mock promise-like object
        GCValue mock_promise = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, mock_promise, "then", 
            JS_NewCFunction(ctx, js_promise_then, "then", 2));
        return mock_promise;
    }
    
    GCValue resolve_func = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
    
    // Check if Promise.resolve exists and is a function
    if (JS_IsException(resolve_func) || !JS_IsFunction(ctx, resolve_func)) {



        // Fallback: return a mock promise-like object
        GCValue mock_promise = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, mock_promise, "then", 
            JS_NewCFunction(ctx, js_promise_then, "then", 2));
        return mock_promise;
    }
    
    // Call Promise.resolve with the Promise constructor as 'this'
    GCValue result = JS_Call(ctx, resolve_func, promise_ctor, 1, &value);



    return result;
}

// Helper to create an empty resolved Promise
GCValue js_create_empty_resolved_promise(JSContextHandle ctx) {
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    
    // Check if Promise constructor exists and is an object
    if (JS_IsException(promise_ctor) || !JS_IsObject(promise_ctor)) {


        // Fallback: return a mock promise-like object
        GCValue mock_promise = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, mock_promise, "then", 
            JS_NewCFunction(ctx, js_promise_then, "then", 2));
        return mock_promise;
    }
    
    GCValue resolve_func = JS_GetPropertyStr(ctx, promise_ctor, "resolve");
    
    // Check if Promise.resolve exists and is a function
    if (JS_IsException(resolve_func) || !JS_IsFunction(ctx, resolve_func)) {



        // Fallback: return a mock promise-like object
        GCValue mock_promise = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, mock_promise, "then", 
            JS_NewCFunction(ctx, js_promise_then, "then", 2));
        return mock_promise;
    }
    
    // Call Promise.resolve with the Promise constructor as 'this'
    GCValue result = JS_Call(ctx, resolve_func, promise_ctor, 0, NULL);



    return result;
}

// ============================================================================
// Shadow DOM Implementation
// ============================================================================

// ShadowRootData struct is defined in browser_api_impl_types.h

void js_shadow_root_finalizer(JSRuntimeHandle rt, GCValue val) {
    // ShadowRootData memory is managed by GC, no manual cleanup needed
    (void)rt;
    (void)val;
}

void js_shadow_root_mark(JSRuntimeHandle rt, GCValue val,
                         JS_MarkFunc *mark_func) {
    GCHandle sr_handle = JS_GetOpaqueHandle(val, js_shadow_root_class_id);
    if (sr_handle == GC_HANDLE_NULL) return;

    /* Keep the ShadowRoot data object alive. */
    mark_func(rt, sr_handle);

    ShadowRootData *sr = (ShadowRootData *)gc_deref(sr_handle);
    if (!sr) return;

    /* Mark JSValue fields stored in C memory so the GC sees them. */
    JS_MarkValue(rt, sr->host, mark_func);
    JS_MarkValue(rt, sr->innerHTML, mark_func);
    JS_MarkValue(rt, sr->first_child, mark_func);
    JS_MarkValue(rt, sr->last_child, mark_func);
}

JSClassDef js_shadow_root_class_def = {
    .class_name = "ShadowRoot",
    .finalizer = js_shadow_root_finalizer,
    .gc_mark   = js_shadow_root_mark,
};

// ShadowRoot constructor - called when new ShadowRoot() is invoked
GCValue js_shadow_root_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    // ShadowRoot cannot be constructed directly, it must be created via attachShadow
    // Return a new object with ShadowRoot prototype
    GCValue obj = JS_NewObjectClass(ctx, js_shadow_root_class_id);
    if (JS_IsException(obj)) {
        return JS_EXCEPTION;
    }
    return obj;
}

// ============================================================================
// ============================================================================
// Real DOM Tree Implementation
// ============================================================================

// Helper: Check if a node has the DOM node data attached
bool is_dom_node(JSContextHandle ctx, GCValue obj) {
    if (JS_IsNull(obj) || JS_IsUndefined(obj)) return false;
    GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_dom_node_class_id);
    return h != GC_HANDLE_NULL;
}

// Helper: Get or create DOM node data for a JS object
DOMNodeHandle get_or_create_dom_node(JSContextHandle ctx, GCValue obj, int node_type, const char* node_name) {
    if (is_dom_node(ctx, obj)) {
        return DOMNodeHandle::from_object_check(ctx, obj);
    }
    
    // Create new DOM node data
    DOMNodeHandle node = DOMNodeHandle::create(ctx, node_type, node_name);
    if (node.valid()) {
        node.attach_to_object(obj);
    }
    return node;
}

// Helper: Get DOM node data if it exists
DOMNodeHandle get_dom_node(JSContextHandle ctx, GCValue obj) {
    return DOMNodeHandle::from_object(obj);
}

// Real appendChild implementation
GCValue js_node_appendChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "appendChild: invalid argument");
    }
    
    GCValue child = argv[0];
    
    // Get or create DOM data for parent
    DOMNodeHandle parent = get_or_create_dom_node(ctx, this_val, DOM_NODE_TYPE_ELEMENT, "");
    if (!parent.valid()) {
        return JS_ThrowTypeError(ctx, "appendChild: invalid parent node");
    }
    
    // Get or create DOM data for child
    DOMNodeHandle child_node = get_or_create_dom_node(ctx, child, DOM_NODE_TYPE_ELEMENT, "");
    if (!child_node.valid()) {
        return JS_ThrowTypeError(ctx, "appendChild: invalid child node");
    }
    
    // Remove child from its current parent if any
    GCValue old_parent_val = child_node.parent_node();
    if (!JS_IsNull(old_parent_val)) {
        // Call removeChild on the old parent
        GCValue remove_args[1] = { child };
        js_node_removeChild_real(ctx, old_parent_val, 1, remove_args);
    }
    
    // Set child's parent
    child_node.set_parent_node(this_val);
    
    // Link child into parent's child list
    GCValue first_child = parent.first_child();
    GCValue last_child = parent.last_child();
    
    if (JS_IsNull(first_child)) {
        // First child - clear any stale sibling references
        parent.set_first_child(child);
        parent.set_last_child(child);
        child_node.set_previous_sibling(JS_NULL);
        child_node.set_next_sibling(JS_NULL);
    } else {
        // Append to end
        DOMNodeHandle last_node = get_dom_node(ctx, last_child);
        if (last_node.valid()) {
            last_node.set_next_sibling(child);
        }
        child_node.set_previous_sibling(last_child);
        child_node.set_next_sibling(JS_NULL);
        parent.set_last_child(child);
    }
    
    dom_request_layout();
    return child;
}

// Real removeChild implementation
GCValue js_node_removeChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "removeChild: invalid argument");
    }
    
    GCValue child = argv[0];
    
    // Get DOM data
    DOMNodeHandle parent = get_dom_node(ctx, this_val);
    DOMNodeHandle child_node = get_dom_node(ctx, child);
    
    if (!parent.valid() || !child_node.valid()) {
        // If no DOM data, just return the child (no-op for non-DOM nodes)
        return child;
    }
    
    // Verify child is actually a child of parent
    GCValue child_parent = child_node.parent_node();
    if (!JS_StrictEq(ctx, child_parent, this_val)) {
        return throw_dom_exception(ctx, "NotFoundError", "Node is not a child of this node");
    }
    
    // Get sibling references
    GCValue prev_sibling = child_node.previous_sibling();
    GCValue next_sibling = child_node.next_sibling();
    
    // Unlink from previous sibling
    if (!JS_IsNull(prev_sibling)) {
        DOMNodeHandle prev_node = get_dom_node(ctx, prev_sibling);
        if (prev_node.valid()) {
            prev_node.set_next_sibling(next_sibling);
        }
    } else {
        // Child was first child, update parent's firstChild
        parent.set_first_child(next_sibling);
    }
    
    // Unlink from next sibling
    if (!JS_IsNull(next_sibling)) {
        DOMNodeHandle next_node = get_dom_node(ctx, next_sibling);
        if (next_node.valid()) {
            next_node.set_previous_sibling(prev_sibling);
        }
    } else {
        // Child was last child, update parent's lastChild
        parent.set_last_child(prev_sibling);
    }
    
    // Clear child's references
    child_node.set_parent_node(JS_NULL);
    child_node.set_previous_sibling(JS_NULL);
    child_node.set_next_sibling(JS_NULL);
    
    dom_request_layout();
    return child;
}

// Real insertBefore implementation
GCValue js_node_insertBefore_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "insertBefore: invalid arguments");
    }
    
    GCValue new_child = argv[0];
    GCValue ref_child = argv[1];  // Can be null (append at end)
    
    // Get or create DOM data
    DOMNodeHandle parent = get_or_create_dom_node(ctx, this_val, DOM_NODE_TYPE_ELEMENT, "");
    if (!parent.valid()) {
        return JS_ThrowTypeError(ctx, "insertBefore: invalid parent node");
    }
    
    DOMNodeHandle new_node = get_or_create_dom_node(ctx, new_child, DOM_NODE_TYPE_ELEMENT, "");
    if (!new_node.valid()) {
        return JS_ThrowTypeError(ctx, "insertBefore: invalid new child node");
    }
    
    // Remove from current parent if any
    GCValue old_parent_val = new_node.parent_node();
    if (!JS_IsNull(old_parent_val)) {
        GCValue remove_args[1] = { new_child };
        js_node_removeChild_real(ctx, old_parent_val, 1, remove_args);
    }
    
    // If refChild is null, append at end
    if (JS_IsNull(ref_child)) {
        return js_node_appendChild_real(ctx, this_val, 1, argv);
    }
    
    // Get ref child's DOM data
    DOMNodeHandle ref_node = get_dom_node(ctx, ref_child);
    if (!ref_node.valid()) {
        return throw_dom_exception(ctx, "NotFoundError", "Reference node not found");
    }
    
    // Verify ref_child is a child of parent
    GCValue ref_parent = ref_node.parent_node();
    if (!JS_StrictEq(ctx, ref_parent, this_val)) {
        return throw_dom_exception(ctx, "NotFoundError", "Reference node is not a child of this node");
    }
    
    // Insert before ref_child
    GCValue prev_sibling = ref_node.previous_sibling();
    
    // Update new node's links
    new_node.set_parent_node(this_val);
    new_node.set_previous_sibling(prev_sibling);
    new_node.set_next_sibling(ref_child);
    
    // Update ref child's previous sibling link
    ref_node.set_previous_sibling(new_child);
    
    // Update previous sibling's next link
    if (!JS_IsNull(prev_sibling)) {
        DOMNodeHandle prev_node = get_dom_node(ctx, prev_sibling);
        if (prev_node.valid()) {
            prev_node.set_next_sibling(new_child);
        }
    } else {
        // new_child is now the first child
        parent.set_first_child(new_child);
    }
    
    dom_request_layout();
    return new_child;
}

// Real cloneNode implementation
GCValue js_node_cloneNode_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    bool deep = false;
    if (argc > 0) {
        deep = JS_ToBool(ctx, argv[0]);
    }
    
    DOMNodeHandle original = get_dom_node(ctx, this_val);
    if (!original.valid()) {
        // No DOM data, return a basic object copy
        return JS_NewObject(ctx);
    }
    
    // Create new node of same type
    GCValue clone = JS_NewObjectClass(ctx, js_dom_node_class_id);
    DOMNodeHandle clone_node = DOMNodeHandle::create(ctx, original.node_type(), original.node_name());
    if (!clone_node.valid()) {
        return JS_ThrowInternalError(ctx, "cloneNode: failed to create clone");
    }
    
    clone_node.attach_to_object(clone);
    clone_node.set_node_value(original.node_value());
    clone_node.set_id(original.id());
    clone_node.set_class_name(original.class_name());
    
    // Copy attributes
    // (This would need iteration over all attributes in a full implementation)
    
    // If deep clone, clone all children
    if (deep) {
        GCValue child = original.first_child();
        while (!JS_IsNull(child)) {
            DOMNodeHandle child_node = get_dom_node(ctx, child);
            if (child_node.valid()) {
                GCValue clone_child = js_node_cloneNode_real(ctx, child, 1, argv);
                if (!JS_IsException(clone_child)) {
                    GCValue append_args[1] = { clone_child };
                    js_node_appendChild_real(ctx, clone, 1, append_args);
                }
            }
            child = child_node.next_sibling();
        }
    }
    
    return clone;
}

// Real contains implementation
GCValue js_node_contains_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_FALSE;
    }
    
    GCValue other = argv[0];
    
    // Check if same node
    if (JS_StrictEq(ctx, this_val, other)) {
        return JS_TRUE;
    }
    
    // Walk up the tree from other node
    DOMNodeHandle other_node = get_dom_node(ctx, other);
    while (other_node.valid()) {
        GCValue parent = other_node.parent_node();
        if (JS_StrictEq(ctx, parent, this_val)) {
            return JS_TRUE;
        }
        other_node = get_dom_node(ctx, parent);
    }
    
    return JS_FALSE;
}

// Real getRootNode implementation
GCValue js_node_getRootNode_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    
    GCValue current = this_val;
    DOMNodeHandle current_node = get_dom_node(ctx, current);
    
    while (current_node.valid()) {
        GCValue parent = current_node.parent_node();
        if (JS_IsNull(parent)) {
            break;
        }
        current = parent;
        current_node = get_dom_node(ctx, current);
    }
    
    return current;
}

// Tree navigation property getters
GCValue js_node_get_firstChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    GCValue first = node.first_child();
    return JS_IsNull(first) ? JS_NULL : first;
}

GCValue js_node_get_lastChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    GCValue last = node.last_child();
    return JS_IsNull(last) ? JS_NULL : last;
}

GCValue js_node_get_nextSibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    GCValue next = node.next_sibling();
    return JS_IsNull(next) ? JS_NULL : next;
}

GCValue js_node_get_previousSibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    GCValue prev = node.previous_sibling();
    return JS_IsNull(prev) ? JS_NULL : prev;
}

/* Set both the internal DOMNode ownerDocument and the JS ownerDocument property. */
void dom_node_set_owner_document(JSContextHandle ctx, GCValue node, GCValue doc) {
    DOMNodeHandle dom_node = get_dom_node(ctx, node);
    if (dom_node.valid()) {
        dom_node.set_owner_document(doc);
    }
    // Also set as own property for code that reads it before the getter is available
    // or for nodes without DOMNode data.
    JS_SetPropertyStr(ctx, node, "ownerDocument", doc);
}

GCValue js_node_get_ownerDocument(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    GCValue doc = node.owner_document();
    if (JS_IsUndefined(doc) || JS_IsNull(doc)) {
        return JS_NULL;
    }
    return doc;
}

GCValue js_node_get_parentNode(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    GCValue parent = node.parent_node();
    return JS_IsNull(parent) ? JS_NULL : parent;
}

GCValue js_node_get_parentElement(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue parent = js_node_get_parentNode(ctx, this_val, 0, NULL);
    if (JS_IsNull(parent)) {
        return JS_NULL;
    }
    // Check if parent is an element
    DOMNodeHandle parent_node = get_dom_node(ctx, parent);
    if (parent_node.valid() && parent_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
        return parent;
    }
    return JS_NULL;
}

GCValue js_node_get_childNodes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue arr = JS_NewArray(ctx);
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return arr;
    }
    
    int idx = 0;
    GCValue child = node.first_child();
    while (!JS_IsNull(child)) {
        JS_SetPropertyUint32(ctx, arr, idx++, child);
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }
    
    return arr;
}

GCValue js_node_get_nodeType(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NewInt32(ctx, DOM_NODE_TYPE_ELEMENT);  // Default to element
    }
    return JS_NewInt32(ctx, node.node_type());
}

GCValue js_node_get_nodeName(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid() || strlen(node.node_name()) == 0) {
        // Try to get from object
        GCValue tagName = JS_GetPropertyStr(ctx, this_val, "tagName");
        if (!JS_IsUndefined(tagName)) {
            return tagName;
        }
        return JS_NewString(ctx, "DIV");
    }
    return JS_NewString(ctx, node.node_name());
}

// Element.prototype.tagName getter
GCValue js_element_get_tagName(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid() || strlen(node.node_name()) == 0) {
        return JS_NewString(ctx, "DIV");
    }
    return JS_NewString(ctx, node.node_name());
}

// Element tree navigation getters
GCValue js_element_get_firstElementChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    
    GCValue child = node.first_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            return child;
        }
        child = child_node.next_sibling();
    }
    return JS_NULL;
}

GCValue js_element_get_lastElementChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    
    GCValue child = node.last_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            return child;
        }
        child = child_node.previous_sibling();
    }
    return JS_NULL;
}

GCValue js_element_get_nextElementSibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    
    GCValue sibling = node.next_sibling();
    while (!JS_IsNull(sibling)) {
        DOMNodeHandle sib_node = get_dom_node(ctx, sibling);
        if (sib_node.valid() && sib_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            return sibling;
        }
        sibling = sib_node.next_sibling();
    }
    return JS_NULL;
}

GCValue js_element_get_previousElementSibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NULL;
    }
    
    GCValue sibling = node.previous_sibling();
    while (!JS_IsNull(sibling)) {
        DOMNodeHandle sib_node = get_dom_node(ctx, sibling);
        if (sib_node.valid() && sib_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            return sibling;
        }
        sibling = sib_node.previous_sibling();
    }
    return JS_NULL;
}

GCValue js_element_get_childElementCount(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NewInt32(ctx, 0);
    }
    
    int count = 0;
    GCValue child = node.first_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            count++;
        }
        child = child_node.next_sibling();
    }
    return JS_NewInt32(ctx, count);
}

GCValue js_element_get_children(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue arr = JS_NewArray(ctx);
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return arr;
    }
    
    int idx = 0;
    GCValue child = node.first_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            JS_SetPropertyUint32(ctx, arr, idx++, child);
        }
        child = child_node.next_sibling();
    }
    
    return arr;
}

// Element.prototype.querySelector
GCValue js_element_querySelector(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_NULL;
}

// Element.prototype.querySelectorAll
GCValue js_element_querySelectorAll(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_NewArray(ctx);
}

// Helper to recursively collect elements by tag name
void collect_elements_by_tag(JSContextHandle ctx, DOMNodeHandle node, const char *tag_name, GCValue arr, int *idx) {
    if (!node.valid()) return;
    
    GCValue child = node.first_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            // Check if tag name matches
            const char *child_tag = child_node.node_name();
            if (child_tag && strcasecmp(child_tag, tag_name) == 0) {
                JS_SetPropertyUint32(ctx, arr, (*idx)++, child);
            }
            // Recurse into children
            collect_elements_by_tag(ctx, child_node, tag_name, arr, idx);
        }
        child = child_node.next_sibling();
    }
}

// Element.prototype.getElementsByTagName
GCValue js_element_get_elements_by_tag_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NewArray(ctx);
    
    const char *tag_name = JS_ToCString(ctx, argv[0]);
    if (!tag_name) return JS_NewArray(ctx);
    
    GCValue arr = JS_NewArray(ctx);
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return arr;
    }
    
    int idx = 0;
    collect_elements_by_tag(ctx, node, tag_name, arr, &idx);
    
    return arr;
}

// Document.prototype.getElementsByTagName
GCValue js_document_get_elements_by_tag_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NewArray(ctx);
    
    const char *tag_name = JS_ToCString(ctx, argv[0]);
    if (!tag_name) return JS_NewArray(ctx);
    
    GCValue arr = JS_NewArray(ctx);
    int idx = 0;
    
    // Special case: if looking for 'head', check document.head directly
    if (strcasecmp(tag_name, "head") == 0) {
        GCValue head = JS_GetPropertyStr(ctx, this_val, "head");
        if (!JS_IsUndefined(head) && !JS_IsNull(head)) {
            JS_SetPropertyUint32(ctx, arr, idx++, head);
        }
    }
    // Special case: if looking for 'body', check document.body directly
    else if (strcasecmp(tag_name, "body") == 0) {
        GCValue body = JS_GetPropertyStr(ctx, this_val, "body");
        if (!JS_IsUndefined(body) && !JS_IsNull(body)) {
            JS_SetPropertyUint32(ctx, arr, idx++, body);
        }
    }
    // For other tags, try to traverse the DOM tree
    else {
        // Get document element (documentElement) - usually <html>
        GCValue doc_elem = JS_GetPropertyStr(ctx, this_val, "documentElement");
        if (!JS_IsNull(doc_elem) && !JS_IsUndefined(doc_elem)) {
            DOMNodeHandle node = get_dom_node(ctx, doc_elem);
            if (node.valid()) {
                // Check document element itself
                const char *node_tag = node.node_name();
                if (node_tag && strcasecmp(node_tag, tag_name) == 0) {
                    JS_SetPropertyUint32(ctx, arr, idx++, doc_elem);
                }
                // Recurse into children
                collect_elements_by_tag(ctx, node, tag_name, arr, &idx);
            }
        }
    }
    
    return arr;
}

// Document.prototype.getElementsByClassName - use the lock-free class index table.
GCValue js_document_getElementsByClassName(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewArray(ctx);

    const char *class_name = JS_ToCString(ctx, argv[0]);
    if (!class_name || !class_name[0]) return JS_NewArray(ctx);

    JSAtom class_atom = JS_NewAtom(ctx, class_name);
    if (class_atom == JS_ATOM_NULL) return JS_NewArray(ctx);
    GCValue arr = css_get_elements_by_class_name(ctx, class_atom);
    JS_FreeAtom(ctx, class_atom);
    return arr;
}

// Element.prototype.setAttribute
GCValue js_element_set_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "requires at least 2 argument(s)");
    
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    
    if (name && value) {
        // Store attribute on the object itself
        JS_SetPropertyStr(ctx, this_val, name, JS_NewString(ctx, value));
        
        // Keep the internal DOMNode attribute table in sync so serialization
        // back to HTML produces the mutated attributes.
        DOMNodeHandle node = get_dom_node(ctx, this_val);
        if (node.valid()) {
            node.set_attribute(name, value);
        }
        
        // Capture URL if src is being set on any element
        if (name && strcmp(name, "src") == 0 && value && value[0]) {
            capture_url_debug(value, "element_setAttribute_src");
        }
    }
    dom_request_layout();
    return JS_UNDEFINED;
}

// Element.prototype.getAttribute
GCValue js_element_get_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;
    
    GCValue val = JS_GetPropertyStr(ctx, this_val, name);
    if (JS_IsUndefined(val) || JS_IsNull(val)) {
        return JS_NULL;
    }
    
    // Convert to string
    const char *str = JS_ToCString(ctx, val);
    if (str) {
        return JS_NewString(ctx, str);
    }
    return JS_NULL;
}

// Element.prototype.removeAttribute
GCValue js_element_remove_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (name) {
        JSAtom atom = JS_NewAtom(ctx, name);
        JS_DeleteProperty(ctx, this_val, atom, 0);
        JS_FreeAtom(ctx, atom);
        
        DOMNodeHandle node = get_dom_node(ctx, this_val);
        if (node.valid()) {
            node.remove_attribute(name);
        }
    }
    dom_request_layout();
    return JS_UNDEFINED;
}

// Element.prototype.hasAttribute
GCValue js_element_has_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_FALSE;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_FALSE;
    
    GCValue val = JS_GetPropertyStr(ctx, this_val, name);
    return JS_NewBool(ctx, !JS_IsUndefined(val));
}

// Element.prototype.toggleAttribute
GCValue js_element_toggle_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_FALSE;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_FALSE;
    
    JSAtom atom = JS_NewAtom(ctx, name);
    GCValue val = JS_GetProperty(ctx, this_val, atom);
    if (JS_IsUndefined(val)) {
        // Add attribute (empty string)
        JS_SetPropertyStr(ctx, this_val, name, JS_NewString(ctx, ""));
        JS_FreeAtom(ctx, atom);
        dom_request_layout();
        return JS_TRUE;
    } else {
        // Remove attribute
        JS_DeleteProperty(ctx, this_val, atom, 0);
        JS_FreeAtom(ctx, atom);
        dom_request_layout();
        return JS_FALSE;
    }
}

// Element.prototype.setAttributeNS
GCValue js_element_set_attribute_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "setAttributeNS requires 3 arguments");
    // Ignore namespace, treat as regular setAttribute
    GCValue r = js_element_set_attribute(ctx, this_val, argc - 1, argv + 1);
    dom_request_layout();
    return r;
}

// Element.prototype.getAttributeNS
GCValue js_element_get_attribute_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_NULL;
    // Ignore namespace, treat as regular getAttribute
    return js_element_get_attribute(ctx, this_val, argc - 1, argv + 1);
}

// Element.prototype.removeAttributeNS
GCValue js_element_remove_attribute_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    // Ignore namespace, treat as regular removeAttribute
    GCValue r = js_element_remove_attribute(ctx, this_val, argc - 1, argv + 1);
    dom_request_layout();
    return r;
}

// Helper to create a generic element stub for querySelector fallback
GCValue create_generic_element_stub(JSContextHandle ctx) {
    GCValue stub = JS_NewObject(ctx);
    if (JS_IsException(stub)) return JS_NULL;
    
    // Common element properties
    GCValue style = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, stub, "style", style);
    
    JS_SetPropertyStr(ctx, stub, "tagName", JS_NewString(ctx, "DIV"));
    JS_SetPropertyStr(ctx, stub, "nodeName", JS_NewString(ctx, "DIV"));
    JS_SetPropertyStr(ctx, stub, "localName", JS_NewString(ctx, "div"));
    
    GCValue classList = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, classList, "add", JS_NewCFunction(ctx, js_dummy_function, "add", 1));
    JS_SetPropertyStr(ctx, classList, "remove", JS_NewCFunction(ctx, js_dummy_function, "remove", 1));
    JS_SetPropertyStr(ctx, classList, "contains", JS_NewCFunction(ctx, js_dummy_function, "contains", 1));
    JS_SetPropertyStr(ctx, classList, "toggle", JS_NewCFunction(ctx, js_dummy_function, "toggle", 1));
    JS_SetPropertyStr(ctx, stub, "classList", classList);
    
    GCValue dataset = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, stub, "dataset", dataset);
    
    JS_SetPropertyStr(ctx, stub, "children", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, stub, "childNodes", JS_NewArray(ctx));
    
    // Common element methods
    JS_SetPropertyStr(ctx, stub, "getAttribute", JS_NewCFunction(ctx, js_element_get_attribute, "getAttribute", 1));
    JS_SetPropertyStr(ctx, stub, "setAttribute", JS_NewCFunction(ctx, js_element_set_attribute, "setAttribute", 2));
    JS_SetPropertyStr(ctx, stub, "hasAttribute", JS_NewCFunction(ctx, js_element_has_attribute, "hasAttribute", 1));
    JS_SetPropertyStr(ctx, stub, "removeAttribute", JS_NewCFunction(ctx, js_dummy_function, "removeAttribute", 1));
    JS_SetPropertyStr(ctx, stub, "getBoundingClientRect", JS_NewCFunction(ctx, js_dummy_function, "getBoundingClientRect", 0));
    JS_SetPropertyStr(ctx, stub, "querySelector", JS_NewCFunction(ctx, js_element_querySelector, "querySelector", 1));
    JS_SetPropertyStr(ctx, stub, "querySelectorAll", JS_NewCFunction(ctx, js_element_querySelectorAll, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, stub, "appendChild", JS_NewCFunction(ctx, js_dummy_function, "appendChild", 1));
    JS_SetPropertyStr(ctx, stub, "removeChild", JS_NewCFunction(ctx, js_dummy_function, "removeChild", 1));
    JS_SetPropertyStr(ctx, stub, "insertBefore", JS_NewCFunction(ctx, js_dummy_function, "insertBefore", 2));
    JS_SetPropertyStr(ctx, stub, "addEventListener", JS_NewCFunction(ctx, js_event_target_addEventListener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, stub, "removeEventListener", JS_NewCFunction(ctx, js_event_target_removeEventListener, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, stub, "dispatchEvent", JS_NewCFunction(ctx, js_event_target_dispatchEvent, "dispatchEvent", 1));
    JS_SetPropertyStr(ctx, stub, "focus", JS_NewCFunction(ctx, js_dummy_function, "focus", 0));
    JS_SetPropertyStr(ctx, stub, "blur", JS_NewCFunction(ctx, js_dummy_function, "blur", 0));
    JS_SetPropertyStr(ctx, stub, "click", JS_NewCFunction(ctx, js_dummy_function, "click", 0));
    JS_SetPropertyStr(ctx, stub, "closest", JS_NewCFunction(ctx, js_element_querySelector, "closest", 1));
    JS_SetPropertyStr(ctx, stub, "matches", JS_NewCFunction(ctx, js_dummy_function, "matches", 1));
    
    // Shadow root
    JS_SetPropertyStr(ctx, stub, "shadowRoot", JS_NULL);
    JS_SetPropertyStr(ctx, stub, "attachShadow", JS_NewCFunction(ctx, js_element_attach_shadow, "attachShadow", 1));
    
    // Parent/owner references
    JS_SetPropertyStr(ctx, stub, "parentNode", JS_NULL);
    JS_SetPropertyStr(ctx, stub, "parentElement", JS_NULL);
    JS_SetPropertyStr(ctx, stub, "firstChild", JS_NULL);
    JS_SetPropertyStr(ctx, stub, "lastChild", JS_NULL);
    JS_SetPropertyStr(ctx, stub, "nextSibling", JS_NULL);
    JS_SetPropertyStr(ctx, stub, "previousSibling", JS_NULL);
    
    // Content properties
    JS_SetPropertyStr(ctx, stub, "innerHTML", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, stub, "outerHTML", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, stub, "textContent", JS_NewString(ctx, ""));
    
    return stub;
}

// Document.getElementById - use the lock-free id index table.
GCValue js_document_getElementById(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id || !*id) return JS_NULL;
    
    JSAtom id_atom = JS_NewAtom(ctx, id);
    if (id_atom == JS_ATOM_NULL) return JS_NULL;
    GCValue result = css_get_element_by_id(ctx, id_atom);
    JS_FreeAtom(ctx, id_atom);
    return result;
}

// Document.querySelector - search real DOM tree
GCValue js_document_querySelector(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    
    const char *selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return JS_NULL;
    
    // Get document.documentElement
    GCValue doc_elem = JS_GetPropertyStr(ctx, this_val, "documentElement");
    if (JS_IsNull(doc_elem) || JS_IsUndefined(doc_elem)) {
        // Fallback: return stub if no documentElement
        return create_generic_element_stub(ctx);
    }
    
    // Search the DOM tree
    GCValue result = query_selector_recursive(ctx, doc_elem, selector);
    if (JS_IsNull(result)) {
        // Fallback: return stub so code doesn't crash
        return create_generic_element_stub(ctx);
    }
    
    return result;
}

// Document.querySelectorAll - search real DOM tree
GCValue js_document_querySelectorAll(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue result = JS_NewArray(ctx);
    if (argc < 1) return result;
    
    const char *selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return result;
    
    // Get document.documentElement
    GCValue doc_elem = JS_GetPropertyStr(ctx, this_val, "documentElement");
    if (JS_IsNull(doc_elem) || JS_IsUndefined(doc_elem)) return result;
    
    int idx = 0;
    query_selector_all_recursive(ctx, doc_elem, selector, result, &idx);
    
    return result;
}

// Element.prototype.click
GCValue js_element_click(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // No-op for stub implementation
    return JS_UNDEFINED;
}

// Element.prototype.getAnimations
GCValue js_element_get_animations(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_NewArray(ctx);
}

// ============================================================================
// Real querySelector/querySelectorAll Implementation
// ============================================================================

// Simple selector matcher - supports tag name, #id, .class selectors
bool matches_selector(JSContextHandle ctx, GCValue elem, const char* selector) {
    if (!selector || !*selector) return false;
    
    DOMNodeHandle node = get_dom_node(ctx, elem);
    
    // ID selector: #id
    if (selector[0] == '#') {
        const char* target_id = selector + 1;
        // First check DOMNode data
        if (node.valid() && strcmp(node.id(), target_id) == 0) {
            return true;
        }
        // Also check JS object property
        GCValue id_val = JS_GetPropertyStr(ctx, elem, "id");
        const char* id_str = JS_ToCString(ctx, id_val);
        if (id_str && strcmp(id_str, target_id) == 0) {
            return true;
        }
        return false;
    }
    
    // Class selector: .class
    if (selector[0] == '.') {
        const char* target_class = selector + 1;
        // First check DOMNode data
        if (node.valid() && strstr(node.class_name(), target_class) != NULL) {
            return true;
        }
        // Also check JS object property
        GCValue class_val = JS_GetPropertyStr(ctx, elem, "className");
        const char* class_str = JS_ToCString(ctx, class_val);
        if (class_str && strstr(class_str, target_class) != NULL) {
            return true;
        }
        return false;
    }
    
    // Tag selector
    const char* tag_name = node.valid() ? node.node_name() : "";
    // Check tagName property on object
    if (tag_name[0] == '\0') {
        GCValue tag_val = JS_GetPropertyStr(ctx, elem, "tagName");
        const char* tag_str = JS_ToCString(ctx, tag_val);
        if (tag_str) {
            return strcasecmp(tag_str, selector) == 0;
        }
        return false;
    }
    return strcasecmp(tag_name, selector) == 0;
}

// Recursive querySelector helper
GCValue query_selector_recursive(JSContextHandle ctx, GCValue elem, const char* selector) {
    DOMNodeHandle node = get_dom_node(ctx, elem);
    if (!node.valid()) return JS_NULL;
    
    // Check this element
    if (matches_selector(ctx, elem, selector)) {
        return elem;
    }
    
    // Check children recursively
    GCValue child = node.first_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            GCValue result = query_selector_recursive(ctx, child, selector);
            if (!JS_IsNull(result)) {
                return result;
            }
        }
        child = child_node.next_sibling();
    }
    
    return JS_NULL;
}

// Recursive querySelectorAll helper
void query_selector_all_recursive(JSContextHandle ctx, GCValue elem, const char* selector, GCValue result_arr, int* idx) {
    DOMNodeHandle node = get_dom_node(ctx, elem);
    if (!node.valid()) return;
    
    // Check this element
    if (matches_selector(ctx, elem, selector)) {
        JS_SetPropertyUint32(ctx, result_arr, (*idx)++, elem);
    }
    
    // Check children recursively
    GCValue child = node.first_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            query_selector_all_recursive(ctx, child, selector, result_arr, idx);
        }
        child = child_node.next_sibling();
    }
}

// Real querySelector implementation
GCValue js_element_querySelector_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    
    const char* selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return JS_NULL;
    
    // Start from this element
    GCValue result = query_selector_recursive(ctx, this_val, selector);
    
    return result;
}

// Real querySelectorAll implementation
GCValue js_element_querySelectorAll_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue result = JS_NewArray(ctx);
    if (argc < 1) return result;
    
    const char* selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return result;
    
    int idx = 0;
    query_selector_all_recursive(ctx, this_val, selector, result, &idx);
    
    return result;
}

// ============================================================================
// Element Content Getters/Setters
// ============================================================================

// classList getter - returns a functional DOMTokenList backed by className
GCValue js_element_get_classList(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    // Check if element already has a classList
    GCValue existing = JS_GetPropertyStr(ctx, this_val, "__classList");
    if (!JS_IsUndefined(existing) && !JS_IsNull(existing)) {
        return existing;
    }
    // Create functional DOMTokenList
    GCValue classList = JS_NewObject(ctx);
    // Back-reference to the element so token methods can sync className
    JS_SetPropertyStr(ctx, classList, "__element", this_val);
    JS_SetPropertyStr(ctx, classList, "add", JS_NewCFunction(ctx, js_dom_token_list_add, "add", 1));
    JS_SetPropertyStr(ctx, classList, "remove", JS_NewCFunction(ctx, js_dom_token_list_remove, "remove", 1));
    JS_SetPropertyStr(ctx, classList, "toggle", JS_NewCFunction(ctx, js_dom_token_list_toggle, "toggle", 2));
    JS_SetPropertyStr(ctx, classList, "contains", JS_NewCFunction(ctx, js_dom_token_list_contains, "contains", 1));
    JS_SetPropertyStr(ctx, classList, "item", JS_NewCFunction(ctx, js_dom_token_list_item, "item", 1));
    JS_SetPropertyStr(ctx, classList, "forEach", JS_NewCFunction(ctx, js_dom_token_list_for_each, "forEach", 1));
    GCValue length_getter = JS_NewCFunction(ctx, js_dom_token_list_get_length, "get length", 0);
    JSAtom length_atom = JS_NewAtom(ctx, "length");
    JS_DefinePropertyGetSet(ctx, classList, length_atom, length_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, length_atom);
    // Store on element for reuse
    JS_SetPropertyStr(ctx, this_val, "__classList", classList);
    return classList;
}

// dataset getter - returns DOMStringMap stub (needed by YouTube player)
GCValue js_element_get_dataset(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    // Check if element already has a dataset
    GCValue existing = JS_GetPropertyStr(ctx, this_val, "__dataset");
    if (!JS_IsUndefined(existing) && !JS_IsNull(existing)) {
        return existing;
    }
    // Create empty dataset object
    GCValue dataset = JS_NewObject(ctx);
    // Store on element for reuse
    JS_SetPropertyStr(ctx, this_val, "__dataset", dataset);
    return dataset;
}

// Element.prototype.innerHTML getter - basic serialization
static void serialize_inner_html(JSContextHandle ctx, GCValue node, char *buf, size_t buf_size, size_t *pos);

static const char* safe_string(JSContextHandle ctx, GCValue val) {
    if (JS_IsUndefined(val) || JS_IsNull(val)) return NULL;
    const char *s = JS_ToCString(ctx, val);
    return (s && s[0]) ? s : NULL;
}

static void serialize_element_html(JSContextHandle ctx, DOMNodeHandle node, char *buf, size_t buf_size, size_t *pos) {
    const char *tag = node.node_name();
    if (!tag || !tag[0]) tag = "DIV";
    size_t tag_len = strlen(tag);
    if (*pos + tag_len + 2 >= buf_size) return;
    buf[(*pos)++] = '<';
    memcpy(buf + *pos, tag, tag_len);
    *pos += tag_len;

    // Serialize class and id attributes explicitly
    GCValue class_val = JS_GetPropertyStr(ctx, node.js_object(), "className");
    const char *class_str = safe_string(ctx, class_val);
    if (class_str) {
        size_t len = strlen(class_str);
        if (*pos + len + 9 < buf_size) {
            memcpy(buf + *pos, " class=\"", 8);
            *pos += 8;
            memcpy(buf + *pos, class_str, len);
            *pos += len;
            buf[(*pos)++] = '"';
        }
    }
    GCValue id_val = JS_GetPropertyStr(ctx, node.js_object(), "id");
    const char *id_str = safe_string(ctx, id_val);
    if (id_str) {
        size_t len = strlen(id_str);
        if (*pos + len + 5 < buf_size) {
            memcpy(buf + *pos, " id=\"", 5);
            *pos += 5;
            memcpy(buf + *pos, id_str, len);
            *pos += len;
            buf[(*pos)++] = '"';
        }
    }

    if (*pos + 1 >= buf_size) return;
    buf[(*pos)++] = '>';
    buf[*pos] = '\0';

    // Serialize children
    GCValue child = node.first_child();
    while (!JS_IsNull(child)) {
        serialize_inner_html(ctx, child, buf, buf_size, pos);
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }

    if (*pos + tag_len + 3 >= buf_size) return;
    buf[(*pos)++] = '<';
    buf[(*pos)++] = '/';
    memcpy(buf + *pos, tag, tag_len);
    *pos += tag_len;
    buf[(*pos)++] = '>';
    buf[*pos] = '\0';
}

static void serialize_inner_html(JSContextHandle ctx, GCValue node, char *buf, size_t buf_size, size_t *pos) {
    DOMNodeHandle dom_node = get_dom_node(ctx, node);
    if (dom_node.valid() && dom_node.node_type() == DOM_NODE_TYPE_TEXT) {
        const char *val = dom_node.node_value();
        if (val && val[0]) {
            size_t len = strlen(val);
            if (*pos + len < buf_size) {
                memcpy(buf + *pos, val, len);
                *pos += len;
                buf[*pos] = '\0';
            }
        }
        return;
    }
    if (dom_node.valid() && dom_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
        serialize_element_html(ctx, dom_node, buf, buf_size, pos);
    }
}

GCValue js_element_get_inner_html(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    char buf[4096];
    buf[0] = '\0';
    size_t pos = 0;
    // innerHTML serializes children, not the element itself.
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (node.valid()) {
        GCValue child = node.first_child();
        while (!JS_IsNull(child)) {
            serialize_inner_html(ctx, child, buf, sizeof(buf), &pos);
            DOMNodeHandle child_node = get_dom_node(ctx, child);
            if (!child_node.valid()) break;
            child = child_node.next_sibling();
        }
    }
    if (pos < sizeof(buf)) buf[pos] = '\0';
    return JS_NewString(ctx, buf);
}

// Element.prototype.innerHTML setter
GCValue js_element_set_inner_html(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    platform_log(LOG_LEVEL_INFO, "dom_api", "js_element_set_inner_html called, argc=%d", argc);
    if (argc < 1) return JS_UNDEFINED;
    const char *html = JS_ToCString(ctx, argv[0]);
    if (!html) html = "";
    platform_log(LOG_LEVEL_INFO, "dom_api", "js_element_set_inner_html: html=%.50s", html);
    html_element_set_inner_html(ctx, this_val, html);
    dom_request_layout();
    return JS_UNDEFINED;
}

// Element.prototype.outerHTML getter
GCValue js_element_get_outer_html(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    // Return empty string for stub
    return JS_NewString(ctx, "");
}

// Element.prototype.outerHTML setter
GCValue js_element_set_outer_html(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    // No-op for stub - in real implementation would replace element
    (void)argv;
    dom_request_layout();
    return JS_UNDEFINED;
}

// ============================================================================
// Node Content Getters/Setters
// ============================================================================

// Helper: recursively collect text content from an element's descendants.
static void collect_text_content(JSContextHandle ctx, DOMNodeHandle node, char *buf, size_t buf_size, size_t *pos) {
    if (!node.valid()) return;
    if (node.node_type() == DOM_NODE_TYPE_TEXT) {
        const char *val = node.node_value();
        if (val && val[0]) {
            size_t len = strlen(val);
            if (*pos + len < buf_size) {
                memcpy(buf + *pos, val, len);
                *pos += len;
                buf[*pos] = '\0';
            }
        }
        return;
    }
    GCValue child = node.first_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        collect_text_content(ctx, child_node, buf, buf_size, pos);
        if (child_node.valid()) {
            child = child_node.next_sibling();
        } else {
            break;
        }
    }
}

// Node.prototype.textContent getter
GCValue js_node_get_text_content(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) {
        return JS_NewString(ctx, "");
    }
    if (node.node_type() == DOM_NODE_TYPE_TEXT) {
        const char *val = node.node_value();
        return JS_NewString(ctx, val ? val : "");
    }
    char buf[4096];
    buf[0] = '\0';
    size_t pos = 0;
    collect_text_content(ctx, node, buf, sizeof(buf), &pos);
    return JS_NewString(ctx, buf);
}

// Node.prototype.textContent setter
GCValue js_node_set_text_content(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    const char *text = JS_ToCString(ctx, argv[0]);
    if (!text) text = "";

    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) return JS_UNDEFINED;

    // Remove all children
    GCValue child = node.first_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        GCValue next = JS_NULL;
        if (child_node.valid()) {
            next = child_node.next_sibling();
        }
        GCValue remove_args[1] = { child };
        js_node_removeChild_real(ctx, this_val, 1, remove_args);
        child = next;
    }

    // Insert a single text node if text is non-empty
    if (text[0]) {
        GCValue doc = node.owner_document();
        GCValue text_node = JS_NULL;
        if (!JS_IsUndefined(doc) && !JS_IsNull(doc)) {
            GCValue createTextNode = JS_GetPropertyStr(ctx, doc, "createTextNode");
            if (!JS_IsUndefined(createTextNode) && !JS_IsNull(createTextNode)) {
                GCValue args[1] = { JS_NewString(ctx, text) };
                text_node = JS_Call(ctx, createTextNode, doc, 1, args);
            }
        }
        if (JS_IsNull(text_node) || JS_IsUndefined(text_node)) {
            text_node = JS_NewString(ctx, text);
        }
        GCValue append_args[1] = { text_node };
        js_node_appendChild_real(ctx, this_val, 1, append_args);
    }

    dom_request_layout();
    return JS_UNDEFINED;
}

// Node.prototype.nodeValue getter
GCValue js_node_get_node_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    // For Element nodes, nodeValue is null
    return JS_NULL;
}

// Node.prototype.nodeValue setter
GCValue js_node_set_node_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // No-op - nodeValue is read-only for most node types
    (void)argc; (void)argv;
    return JS_UNDEFINED;
}

// ============================================================================
// Document Methods Implementation
// ============================================================================

// document.createRange()
GCValue js_document_create_range(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue range = JS_NewObject(ctx);
    // Range properties
    JS_SetPropertyStr(ctx, range, "collapsed", JS_TRUE);
    JS_SetPropertyStr(ctx, range, "commonAncestorContainer", JS_NULL);
    JS_SetPropertyStr(ctx, range, "endContainer", JS_NULL);
    JS_SetPropertyStr(ctx, range, "endOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, range, "startContainer", JS_NULL);
    JS_SetPropertyStr(ctx, range, "startOffset", JS_NewInt32(ctx, 0));
    return range;
}

// document.createTreeWalker()
GCValue js_document_create_tree_walker(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue tree_walker = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tree_walker, "currentNode", JS_NULL);
    JS_SetPropertyStr(ctx, tree_walker, "root", JS_NULL);
    JS_SetPropertyStr(ctx, tree_walker, "whatToShow", JS_NewInt32(ctx, 0xFFFFFFFF)); // SHOW_ALL
    JS_SetPropertyStr(ctx, tree_walker, "filter", JS_NULL);
    // Methods
    JS_SetPropertyStr(ctx, tree_walker, "firstChild", JS_NewCFunction(ctx, js_dummy_function, "firstChild", 0));
    JS_SetPropertyStr(ctx, tree_walker, "lastChild", JS_NewCFunction(ctx, js_dummy_function, "lastChild", 0));
    JS_SetPropertyStr(ctx, tree_walker, "nextNode", JS_NewCFunction(ctx, js_dummy_function, "nextNode", 0));
    JS_SetPropertyStr(ctx, tree_walker, "nextSibling", JS_NewCFunction(ctx, js_dummy_function, "nextSibling", 0));
    JS_SetPropertyStr(ctx, tree_walker, "parentNode", JS_NewCFunction(ctx, js_dummy_function, "parentNode", 0));
    JS_SetPropertyStr(ctx, tree_walker, "previousNode", JS_NewCFunction(ctx, js_dummy_function, "previousNode", 0));
    JS_SetPropertyStr(ctx, tree_walker, "previousSibling", JS_NewCFunction(ctx, js_dummy_function, "previousSibling", 0));
    return tree_walker;
}

// Event initEvent implementation
GCValue js_event_init_event(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc >= 1) {
        const char *type = JS_ToCString(ctx, argv[0]);
        if (type) {
            JS_SetPropertyStr(ctx, this_val, "type", JS_NewString(ctx, type));
        }
    }
    if (argc >= 2) {
        JS_SetPropertyStr(ctx, this_val, "bubbles", JS_NewBool(ctx, JS_ToBool(ctx, argv[1])));
    }
    if (argc >= 3) {
        JS_SetPropertyStr(ctx, this_val, "cancelable", JS_NewBool(ctx, JS_ToBool(ctx, argv[2])));
    }
    return JS_UNDEFINED;
}

// Event initCustomEvent implementation
GCValue js_event_init_custom_event(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc >= 1) {
        const char *type = JS_ToCString(ctx, argv[0]);
        if (type) {
            JS_SetPropertyStr(ctx, this_val, "type", JS_NewString(ctx, type));
        }
    }
    if (argc >= 2) {
        JS_SetPropertyStr(ctx, this_val, "bubbles", JS_NewBool(ctx, JS_ToBool(ctx, argv[1])));
    }
    if (argc >= 3) {
        JS_SetPropertyStr(ctx, this_val, "cancelable", JS_NewBool(ctx, JS_ToBool(ctx, argv[2])));
    }
    if (argc >= 4) {
        JS_SetPropertyStr(ctx, this_val, "detail", argv[3]);
    }
    return JS_UNDEFINED;
}

// document.createEvent()
GCValue js_document_create_event(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue event = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, event, "bubbles", JS_FALSE);
    JS_SetPropertyStr(ctx, event, "cancelable", JS_FALSE);
    JS_SetPropertyStr(ctx, event, "composed", JS_FALSE);
    JS_SetPropertyStr(ctx, event, "currentTarget", JS_NULL);
    JS_SetPropertyStr(ctx, event, "defaultPrevented", JS_FALSE);
    JS_SetPropertyStr(ctx, event, "eventPhase", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, event, "target", JS_NULL);
    JS_SetPropertyStr(ctx, event, "timeStamp", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, ""));
    // Methods
    JS_SetPropertyStr(ctx, event, "initEvent", JS_NewCFunction(ctx, js_event_init_event, "initEvent", 3));
    JS_SetPropertyStr(ctx, event, "initCustomEvent", JS_NewCFunction(ctx, js_event_init_custom_event, "initCustomEvent", 4));
    JS_SetPropertyStr(ctx, event, "preventDefault", JS_NewCFunction(ctx, js_dummy_function, "preventDefault", 0));
    JS_SetPropertyStr(ctx, event, "stopPropagation", JS_NewCFunction(ctx, js_dummy_function, "stopPropagation", 0));
    JS_SetPropertyStr(ctx, event, "stopImmediatePropagation", JS_NewCFunction(ctx, js_dummy_function, "stopImmediatePropagation", 0));
    return event;
}

// document.importNode()
GCValue js_document_import_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    // Return a shallow copy of the node
    GCValue clone = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, clone, "nodeType", JS_NewInt32(ctx, 1)); // ELEMENT_NODE
    JS_SetPropertyStr(ctx, clone, "nodeName", JS_NewString(ctx, "DIV"));
    return clone;
}

// document.elementFromPoint()
GCValue js_document_element_from_point(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Return documentElement for any point
    return JS_GetPropertyStr(ctx, this_val, "documentElement");
}

// ============================================================================
// Node Implementation
// ============================================================================

GCValue js_node_appendChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    // Return the appended child
    return argv[0];
}

GCValue js_node_insertBefore(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    return argv[0];
}

GCValue js_node_removeChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    return argv[0];
}

GCValue js_node_cloneNode(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Return a new empty object as cloned node
    return JS_NewObject(ctx);
}

GCValue js_node_contains(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_FALSE;
}

// Node.prototype.getRootNode - CRITICAL for Shadow DOM
GCValue js_node_get_root_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Return document.documentElement as root
    GCValue document = JS_GetPropertyStr(ctx, this_val, "ownerDocument");
    if (JS_IsUndefined(document)) {
        return this_val; // Return self if no owner
    }
    GCValue root = JS_GetPropertyStr(ctx, document, "documentElement");
    if (JS_IsUndefined(root) || JS_IsNull(root)) {
        return this_val;
    }
    return root;
}


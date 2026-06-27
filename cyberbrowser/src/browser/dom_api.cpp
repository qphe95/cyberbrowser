/* Auto-generated split from browser_api_impl.cpp */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include <vector>
#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_types.h"
#include "browser_api_impl_handles.h"
#include "browser_api_impl_internal.h"
#include "html_dom.h"
#include "css_parser.h"
#include "css_layout.h"
#include "gc_value_helpers.h"
#include "platform.h"

// createElement lives in js_quickjs.cpp and is used by the outerHTML setter.
extern GCValue js_document_create_element(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

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

// Helpers for DocumentFragment expansion in appendChild/insertBefore.
static bool is_document_fragment_node(JSContextHandle ctx, GCValue node) {
    DOMNodeHandle n = get_dom_node(ctx, node);
    return n.valid() && n.node_type() == DOM_NODE_TYPE_DOCUMENT_FRAGMENT;
}
static std::vector<GCValue> collect_fragment_children(JSContextHandle ctx, GCValue fragment) {
    std::vector<GCValue> children;
    DOMNodeHandle frag = get_dom_node(ctx, fragment);
    if (!frag.valid()) return children;
    GCValue cur = frag.first_child();
    while (!JS_IsNull(cur) && !JS_IsUndefined(cur)) {
        children.push_back(cur);
        DOMNodeHandle cur_node = get_dom_node(ctx, cur);
        cur = cur_node.valid() ? cur_node.next_sibling() : JS_NULL;
    }
    return children;
}
GCValue js_node_contains_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_getRootNode_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_get_ownerDocument(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_querySelector_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_querySelectorAll_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_document_get_adopted_style_sheets(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_document_set_adopted_style_sheets(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

// Forward declarations for DOM helper functions (used by ShadowRoot)
bool matches_selector(JSContextHandle ctx, GCValue elem, const char* selector);
DOMNodeHandle get_dom_node(JSContextHandle ctx, GCValue obj);
DOMNodeHandle get_or_create_dom_node(JSContextHandle ctx, GCValue obj, int node_type, const char* node_name);
GCValue query_selector_recursive(JSContextHandle ctx, GCValue elem, const char* selector);
void query_selector_all_recursive(JSContextHandle ctx, GCValue elem, const char* selector, GCValue result_arr, int* idx);

// Custom element lifecycle helpers
static bool dom_node_is_connected(JSContextHandle ctx, GCValue node) {
    GCValue cur = node;
    int safety = 0;
    while (!JS_IsUndefined(cur) && !JS_IsNull(cur) && JS_IsObject(cur) && safety++ < 10000) {
        DOMNodeHandle n = get_dom_node(ctx, cur);
        if (n.valid()) {
            if (n.node_type() == DOM_NODE_TYPE_DOCUMENT) return true;
            // Nodes inside a template content are not connected.  A ShadowRoot
            // is also implemented as a fragment-like container, but it is
            // connected when its host element is connected.
            if (n.node_type() == DOM_NODE_TYPE_DOCUMENT_FRAGMENT) {
                GCValue host = JS_GetPropertyStr(ctx, cur, "host");
                if (!JS_IsUndefined(host) && !JS_IsNull(host) && JS_IsObject(host)) {
                    cur = host;
                    continue;
                }
                return false;
            }
            cur = n.parent_node();
        } else {
            // The parent may be a ShadowRoot object (not a DOMNode-backed node).
            // Continue connectivity checks from the shadow host.
            GCValue host = JS_GetPropertyStr(ctx, cur, "host");
            if (!JS_IsUndefined(host) && !JS_IsNull(host) && JS_IsObject(host)) {
                cur = host;
                continue;
            }
            break;
        }
    }
    return false;
}

GCValue js_node_is_connected_getter(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc;
    (void)argv;
    bool connected = dom_node_is_connected(ctx, this_val);
    return JS_NewBool(ctx, connected);
}

static void invoke_custom_element_callback(JSContextHandle ctx, GCValue elem, const char *name) {
    // Custom element lifecycle callbacks are CEReactions and must run after
    // any queued upgrade reactions have completed, not synchronously during the
    // DOM mutation. Enqueue them into the custom-element reaction queue.
    js_cyber_ce_enqueue_callback(ctx, elem, name);
}

static void invoke_attribute_changed(JSContextHandle ctx, GCValue elem,
                                     const char *name, const char *old_val,
                                     const char *new_val) {
    GCValue cb = JS_GetPropertyStr(ctx, elem, "attributeChangedCallback");
    if (JS_IsException(cb)) {
        JS_GetException(ctx); // clear
        return;
    }
    if (!JS_IsUndefined(cb) && !JS_IsNull(cb) && JS_IsFunction(ctx, cb)) {
        GCValue args[3];
        args[0] = JS_NewString(ctx, name);
        args[1] = old_val ? JS_NewString(ctx, old_val) : JS_NULL;
        args[2] = new_val ? JS_NewString(ctx, new_val) : JS_NULL;
        GCValue ret = JS_Call(ctx, cb, elem, 3, args);
        if (JS_IsException(ret)) {
            JS_GetException(ctx); // clear
        }
    }
}

void js_dom_node_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    // Free the lock-free computed-style hash table.  The table itself is
    // malloc'd; the GC will reclaim the CssComputedStyle object separately.
    GCHandle node_handle = JS_GetOpaqueHandle(val, js_dom_node_class_id);
    if (node_handle != GC_HANDLE_NULL) {
        DOMNodeHandle node(node_handle);
        if (node.valid()) {
            node.free_attributes();
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
        /* getElementsByTagName is case-insensitive in HTML documents, so index
         * every element by a canonical lower-case tag name. */
        char lower_tag[256];
        size_t tag_len = strlen(tag);
        if (tag_len >= sizeof(lower_tag)) tag_len = sizeof(lower_tag) - 1;
        for (size_t i = 0; i < tag_len; i++) {
            lower_tag[i] = (char)tolower((unsigned char)tag[i]);
        }
        lower_tag[tag_len] = '\0';
        JSAtom tag_atom = JS_NewAtom(ctx, lower_tag);
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

    /* Keep the backing DOMNode alive; it stores the actual tree pointers. */
    if (sr->dom_node != GC_HANDLE_NULL) {
        mark_func(rt, sr->dom_node);
    }
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

// Helper: Get DOM node data if it exists.  ShadowRoot objects are backed by a
// regular DOMNode stored inside their ShadowRootData, so this lets the rest of
// the DOM implementation treat a ShadowRoot as just another node parent.
DOMNodeHandle get_dom_node(JSContextHandle ctx, GCValue obj) {
    (void)ctx;
    DOMNodeHandle node = DOMNodeHandle::from_object(obj);
    if (node.valid()) return node;

    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object(obj);
    if (sr.valid()) {
        GCHandle h = sr.dom_node();
        if (h != GC_HANDLE_NULL) return DOMNodeHandle(h);
    }
    return DOMNodeHandle();
}

// Helper: Get or create DOM node data for a JS object
DOMNodeHandle get_or_create_dom_node(JSContextHandle ctx, GCValue obj, int node_type, const char* node_name) {
    DOMNodeHandle node = get_dom_node(ctx, obj);
    if (node.valid()) {
        return node;
    }

    // Create new DOM node data
    node = DOMNodeHandle::create(ctx, node_type, node_name);
    if (node.valid()) {
        node.attach_to_object(obj);
    }
    return node;
}

// Real appendChild implementation
GCValue js_node_appendChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "appendChild: invalid argument");
    }
    
    GCValue child = argv[0];
    
    // DocumentFragment: append its children, not the fragment itself.
    if (is_document_fragment_node(ctx, child)) {
        std::vector<GCValue> children = collect_fragment_children(ctx, child);
        for (GCValue c : children) {
            GCValue args[1] = { c };
            js_node_appendChild_real(ctx, this_val, 1, args);
        }
        return child;
    }
    
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

    // Trigger custom element connectedCallback only when the node is actually
    // connected to the document (not inside a template fragment).
    if (dom_node_is_connected(ctx, child)) {
        invoke_custom_element_callback(ctx, child, "connectedCallback");
    }

    // Enqueue upgrade reactions for any custom elements in the inserted subtree.
    // The HTML spec upgrades elements when they are inserted into a document
    // (or when a definition is registered), not just on explicit upgrade() calls.
    js_cyber_ce_enqueue_upgrade_subtree(ctx, child);
    js_cyber_ce_schedule_flush(ctx);

    GCValue added_arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, added_arr, 0, child);
    GCValue removed_arr = JS_NewArray(ctx);
    mo_notify_child_list(ctx, this_val, added_arr, removed_arr);

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
    {
        GCValue tagv = JS_GetPropertyStr(ctx, child, "tagName");
        const char *tag = JS_ToCString(ctx, tagv);
        GCValue parentv = JS_GetPropertyStr(ctx, this_val, "tagName");
        const char *ptag = JS_ToCString(ctx, parentv);
        GCValue idv = JS_GetPropertyStr(ctx, child, "__cyber_id");
        int cid = -1;
        JS_ToInt32(ctx, &cid, idv);
        if (tag && (strcasecmp(tag, "ytd-masthead") == 0 || strcasecmp(tag, "ytd-app") == 0)) {
            platform_log(LOG_LEVEL_WARN, "dom_api", "REMOVE id=%d %s from %s", cid, tag, ptag ? ptag : "?");
        }
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

    // Trigger custom element disconnectedCallback only if it was connected.
    // We already removed it, so check via the saved parent chain: if the old
    // parent chain reached the document, fire the callback.
    {
        bool was_connected = false;
        GCValue cur = this_val;
        while (!JS_IsUndefined(cur) && !JS_IsNull(cur) && JS_IsObject(cur)) {
            DOMNodeHandle n = get_dom_node(ctx, cur);
            if (!n.valid()) break;
            if (n.node_type() == DOM_NODE_TYPE_DOCUMENT) { was_connected = true; break; }
            if (n.node_type() == DOM_NODE_TYPE_DOCUMENT_FRAGMENT) break;
            cur = n.parent_node();
        }
        if (was_connected) {
            invoke_custom_element_callback(ctx, child, "disconnectedCallback");
        }
    }
    js_cyber_ce_schedule_flush(ctx);

    GCValue added_arr2 = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, added_arr2, 0, child);
    GCValue removed_arr2 = JS_NewArray(ctx);
    mo_notify_child_list(ctx, this_val, added_arr2, removed_arr2);

    return child;
}

// Real insertBefore implementation
GCValue js_node_insertBefore_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "insertBefore: invalid arguments");
    }
    
    GCValue new_child = argv[0];
    GCValue ref_child = argv[1];  // Can be null (append at end)
    
    // DocumentFragment: insert its children before ref_child, not the fragment itself.
    if (is_document_fragment_node(ctx, new_child)) {
        std::vector<GCValue> children = collect_fragment_children(ctx, new_child);
        for (GCValue c : children) {
            GCValue args[2] = { c, ref_child };
            js_node_insertBefore_real(ctx, this_val, 2, args);
        }
        return new_child;
    }
    
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

    // Trigger custom element connectedCallback only when the node is actually
    // connected to the document.
    if (dom_node_is_connected(ctx, new_child)) {
        invoke_custom_element_callback(ctx, new_child, "connectedCallback");
    }

    // Enqueue upgrade reactions for any custom elements in the inserted subtree.
    js_cyber_ce_enqueue_upgrade_subtree(ctx, new_child);
    js_cyber_ce_schedule_flush(ctx);

    GCValue added_arr3 = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, added_arr3, 0, new_child);
    GCValue removed_arr3 = JS_NewArray(ctx);
    mo_notify_child_list(ctx, this_val, added_arr3, removed_arr3);

    return new_child;
}

// Real replaceChild implementation
GCValue js_node_replaceChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0]) ||
        JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])) {
        return JS_ThrowTypeError(ctx, "replaceChild: invalid arguments");
    }

    GCValue new_child = argv[0];
    GCValue old_child = argv[1];

    // Replacing a node with itself is a no-op.
    if (JS_StrictEq(ctx, new_child, old_child)) {
        return old_child;
    }

    DOMNodeHandle parent = get_dom_node(ctx, this_val);
    DOMNodeHandle old_node = get_dom_node(ctx, old_child);
    if (!parent.valid() || !old_node.valid()) {
        return throw_dom_exception(ctx, "NotFoundError", "Node not found");
    }

    // Verify oldChild is a child of this node.
    GCValue old_parent = old_node.parent_node();
    if (!JS_StrictEq(ctx, old_parent, this_val)) {
        return throw_dom_exception(ctx, "NotFoundError", "Node is not a child of this node");
    }

    // Insert newChild before oldChild, then remove oldChild. insertBefore handles
    // DocumentFragment expansion and reparenting.
    GCValue insert_args[2] = { new_child, old_child };
    js_node_insertBefore_real(ctx, this_val, 2, insert_args);

    GCValue remove_args[1] = { old_child };
    js_node_removeChild_real(ctx, this_val, 1, remove_args);

    return old_child;
}

// Helper: true if node is a text node.
static bool is_text_node(JSContextHandle ctx, GCValue node) {
    DOMNodeHandle n = get_dom_node(ctx, node);
    return n.valid() && n.node_type() == DOM_NODE_TYPE_TEXT;
}

// Real normalize implementation: merge adjacent text nodes and remove empty ones.
GCValue js_node_normalize_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle parent = get_dom_node(ctx, this_val);
    if (!parent.valid()) {
        return JS_UNDEFINED;
    }

    GCValue child = parent.first_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (!child_node.valid()) {
            child = child_node.next_sibling();
            continue;
        }

        if (child_node.node_type() != DOM_NODE_TYPE_TEXT) {
            // Recursively normalize descendants first, then advance.
            GCValue next = child_node.next_sibling();
            js_node_normalize_real(ctx, child, 0, NULL);
            child = next;
            continue;
        }

        // Merge adjacent text nodes.
        GCValue next = child_node.next_sibling();
        if (!JS_IsNull(next) && is_text_node(ctx, next)) {
            DOMNodeHandle next_node = get_dom_node(ctx, next);
            const char *t1 = child_node.node_value();
            const char *t2 = next_node.node_value();
            size_t len1 = t1 ? strlen(t1) : 0;
            size_t len2 = t2 ? strlen(t2) : 0;
            char *merged = (char *)malloc(len1 + len2 + 1);
            if (merged) {
                if (len1) memcpy(merged, t1, len1);
                if (len2) memcpy(merged + len1, t2, len2);
                merged[len1 + len2] = '\0';
                child_node.set_node_value(merged);
                free(merged);
            }
            GCValue rem_args[1] = { next };
            js_node_removeChild_real(ctx, this_val, 1, rem_args);
            continue;
        }

        // Remove empty text nodes.
        const char *val = child_node.node_value();
        if (!val || !*val) {
            GCValue rem_args[1] = { child };
            js_node_removeChild_real(ctx, this_val, 1, rem_args);
            child = next;
            continue;
        }

        child = next;
    }

    return JS_UNDEFINED;
}

// Helpers for compareDocumentPosition
static GCValue common_ancestor_node(JSContextHandle ctx, GCValue a, GCValue b) {
    // Collect ancestors of a (including a itself)
    GCValue cur = a;
    GCValue ancestors[256];
    int anc_count = 0;
    while (!JS_IsNull(cur) && JS_IsObject(cur) && anc_count < 256) {
        ancestors[anc_count++] = cur;
        DOMNodeHandle n = get_dom_node(ctx, cur);
        if (!n.valid()) break;
        cur = n.parent_node();
    }

    cur = b;
    while (!JS_IsNull(cur) && JS_IsObject(cur)) {
        for (int i = 0; i < anc_count; i++) {
            if (JS_StrictEq(ctx, ancestors[i], cur)) {
                return cur;
            }
        }
        DOMNodeHandle n = get_dom_node(ctx, cur);
        if (!n.valid()) break;
        cur = n.parent_node();
    }
    return JS_NULL;
}

static GCValue child_containing_node(JSContextHandle ctx, GCValue ancestor, GCValue node) {
    GCValue cur = node;
    while (!JS_IsNull(cur) && JS_IsObject(cur)) {
        DOMNodeHandle n = get_dom_node(ctx, cur);
        if (!n.valid()) break;
        GCValue p = n.parent_node();
        if (JS_StrictEq(ctx, p, ancestor)) {
            return cur;
            }
        cur = p;
    }
    return JS_NULL;
}

// Real compareDocumentPosition implementation
GCValue js_node_compareDocumentPosition_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "compareDocumentPosition: invalid argument");
    }

    GCValue other = argv[0];
    if (JS_StrictEq(ctx, this_val, other)) {
        return JS_NewInt32(ctx, 0);
    }

    // If the nodes are in disconnected trees, report disconnected.
    GCValue root1 = js_node_getRootNode_real(ctx, this_val, 0, NULL);
    GCValue root2 = js_node_getRootNode_real(ctx, other, 0, NULL);
    if (!JS_StrictEq(ctx, root1, root2)) {
        return JS_NewInt32(ctx, 1 /* DOCUMENT_POSITION_DISCONNECTED */ | 32 /* DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC */);
    }

    // Ancestor/descendant relationships.
    GCValue args[1] = { other };
    GCValue contains1 = js_node_contains_real(ctx, this_val, 1, args);
    if (JS_ToBool(ctx, contains1)) {
        // other is contained by this -> other follows this in document order.
        return JS_NewInt32(ctx, 4 /* DOCUMENT_POSITION_FOLLOWING */ | 16 /* DOCUMENT_POSITION_CONTAINED_BY */);
    }
    args[0] = this_val;
    GCValue contains2 = js_node_contains_real(ctx, other, 1, args);
    if (JS_ToBool(ctx, contains2)) {
        // other contains this -> other precedes this.
        return JS_NewInt32(ctx, 2 /* DOCUMENT_POSITION_PRECEDING */ | 8 /* DOCUMENT_POSITION_CONTAINS */);
    }

    GCValue common = common_ancestor_node(ctx, this_val, other);
    if (JS_IsNull(common)) {
        return JS_NewInt32(ctx, 1 | 32);
    }

    GCValue c1 = child_containing_node(ctx, common, this_val);
    GCValue c2 = child_containing_node(ctx, common, other);
    if (JS_StrictEq(ctx, c1, c2) || JS_IsNull(c1) || JS_IsNull(c2)) {
        return JS_NewInt32(ctx, 0);
    }

    // Walk c1's next siblings; if we hit c2, c2 follows c1.
    GCValue cur = c1;
    while (!JS_IsNull(cur) && JS_IsObject(cur)) {
        if (JS_StrictEq(ctx, cur, c2)) {
            return JS_NewInt32(ctx, 4 /* FOLLOWING */);
        }
        DOMNodeHandle n = get_dom_node(ctx, cur);
        if (!n.valid()) break;
        cur = n.next_sibling();
    }
    return JS_NewInt32(ctx, 2 /* PRECEDING */);
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
    int attr_count = original.attribute_count();
    const DOMAttribute *attrs = original.attributes();
    for (int i = 0; i < attr_count && attrs; i++) {
        clone_node.set_attribute(attrs[i].name, attrs[i].value);
    }

    // Wire up a sensible prototype so the clone has DOM methods.
    {
        GCValue global_obj = JS_GetGlobalObject(ctx);
        if (original.node_type() == DOM_NODE_TYPE_ELEMENT) {
            bool is_template = (strcmp(original.node_name(), "TEMPLATE") == 0);
            GCValue ctor = JS_GetPropertyStr(ctx, global_obj,
                is_template ? "HTMLTemplateElement" : "HTMLElement");
            if (!JS_IsUndefined(ctor) && !JS_IsException(ctor)) {
                GCValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
                if (!JS_IsUndefined(proto) && !JS_IsException(proto)) {
                    JS_SetPrototype(ctx, clone, proto);
                }
            }
        } else if (original.node_type() == DOM_NODE_TYPE_DOCUMENT_FRAGMENT) {
            GCValue ctor = JS_GetPropertyStr(ctx, global_obj, "DocumentFragment");
            if (!JS_IsUndefined(ctor) && !JS_IsException(ctor)) {
                GCValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
                if (!JS_IsUndefined(proto) && !JS_IsException(proto)) {
                    JS_SetPrototype(ctx, clone, proto);
                }
            }
        } else {
            GCValue node_ctor = JS_GetPropertyStr(ctx, global_obj, "Node");
            if (!JS_IsUndefined(node_ctor) && !JS_IsException(node_ctor)) {
                GCValue node_proto = JS_GetPropertyStr(ctx, node_ctor, "prototype");
                if (!JS_IsUndefined(node_proto) && !JS_IsException(node_proto)) {
                    JS_SetPrototype(ctx, clone, node_proto);
                }
            }
        }
    }

    // Templates store their children in .content, not in the light DOM.
    if (original.node_type() == DOM_NODE_TYPE_ELEMENT && strcmp(original.node_name(), "TEMPLATE") == 0) {
        GCValue orig_content = JS_GetPropertyStr(ctx, this_val, "content");
        if (!JS_IsUndefined(orig_content) && !JS_IsNull(orig_content) && JS_IsObject(orig_content)) {
            GCValue cloned_content = js_node_cloneNode_real(ctx, orig_content, 1, argv);
            if (!JS_IsException(cloned_content) && !JS_IsUndefined(cloned_content) && !JS_IsNull(cloned_content)) {
                JS_SetPropertyStr(ctx, clone, "content", cloned_content);
            }
        }
    }

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
    bool composed = false;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        GCValue v = JS_GetPropertyStr(ctx, argv[0], "composed");
        composed = JS_ToBool(ctx, v);
    }

    GCValue current = this_val;
    DOMNodeHandle current_node = get_dom_node(ctx, current);

    while (current_node.valid() || JS_IsObject(current)) {
        GCValue parent = JS_NULL;
        if (current_node.valid()) {
            parent = current_node.parent_node();
        }
        if (JS_IsNull(parent)) {
            if (composed) {
                // If current is a ShadowRoot, cross to its host and continue.
                GCValue host = JS_GetPropertyStr(ctx, current, "host");
                if (!JS_IsNull(host) && !JS_IsUndefined(host) && JS_IsObject(host)) {
                    current = host;
                    current_node = get_dom_node(ctx, current);
                    continue;
                }
            }
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

// Element.prototype.style getter
GCValue js_element_get_style(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid() || node.node_type() != DOM_NODE_TYPE_ELEMENT) {
        return JS_UNDEFINED;
    }
    return css_ensure_style_object(ctx, this_val);
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
        return arr;
    }
    // Special case: if looking for 'body', check document.body directly
    if (strcasecmp(tag_name, "body") == 0) {
        GCValue body = JS_GetPropertyStr(ctx, this_val, "body");
        if (!JS_IsUndefined(body) && !JS_IsNull(body)) {
            JS_SetPropertyUint32(ctx, arr, idx++, body);
        }
        return arr;
    }

    /* The tag index stores a canonical lower-case key, so normalize the
     * query before looking it up. */
    char lower_tag[256];
    size_t tag_len = strlen(tag_name);
    if (tag_len >= sizeof(lower_tag)) tag_len = sizeof(lower_tag) - 1;
    for (size_t i = 0; i < tag_len; i++) {
        lower_tag[i] = (char)tolower((unsigned char)tag_name[i]);
    }
    lower_tag[tag_len] = '\0';

    JSAtom tag_atom = JS_NewAtom(ctx, lower_tag);
    if (tag_atom != JS_ATOM_NULL) {
        GCValue indexed = css_get_elements_by_tag_name(ctx, tag_atom);
        JS_FreeAtom(ctx, tag_atom);
        if (!JS_IsUndefined(indexed) && !JS_IsException(indexed)) {
            GCValue len_val = JS_GetPropertyStr(ctx, indexed, "length");
            uint32_t n = 0;
            JS_ToUint32(ctx, &n, len_val);
            for (uint32_t i = 0; i < n; i++) {
                GCValue el = JS_GetPropertyUint32(ctx, indexed, i);
                JS_SetPropertyUint32(ctx, arr, idx++, el);
            }
        }
    }

    return arr;
}

// Document.prototype.adoptedStyleSheets getter/setter.
GCValue js_document_get_adopted_style_sheets(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue sheets = JS_GetPropertyStr(ctx, this_val, "__adoptedStyleSheets");
    if (JS_IsUndefined(sheets) || JS_IsNull(sheets) || !JS_IsArray(ctx, sheets)) {
        sheets = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, this_val, "__adoptedStyleSheets", sheets);
    }
    return sheets;
}

GCValue js_document_set_adopted_style_sheets(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "adoptedStyleSheets setter requires an array");
    JS_SetPropertyStr(ctx, this_val, "__adoptedStyleSheets", argv[0]);
    dom_request_layout();
    return JS_UNDEFINED;
}

static void js_get_document_url(JSContextHandle ctx, char *out, size_t out_len) {
    out[0] = '\0';
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue location = JS_GetPropertyStr(ctx, global, "location");
    if (JS_IsObject(location)) {
        GCValue href = JS_GetPropertyStr(ctx, location, "href");
        const char *s = JS_IsString(href) ? JS_ToCString(ctx, href) : NULL;
        if (s) {
            strncpy(out, s, out_len - 1);
            out[out_len - 1] = '\0';
            JS_FreeCString(ctx, s);
        }
    }
    if (out[0] == '\0') {
        strncpy(out, "https://localhost/", out_len - 1);
        out[out_len - 1] = '\0';
    }
}

// Document.prototype.cookie getter/setter backed by the domain/path-scoped cookie jar.
GCValue js_document_cookie_getter(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    char doc_url[2048];
    js_get_document_url(ctx, doc_url, sizeof(doc_url));
    const char *cookies = platform_http_get_cookies_for_document(doc_url);
    return JS_NewString(ctx, cookies ? cookies : "");
}

GCValue js_document_cookie_setter(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const char *cookie_line = JS_ToCString(ctx, argv[0]);
    if (!cookie_line || !cookie_line[0]) {
        if (cookie_line) JS_FreeCString(ctx, cookie_line);
        return JS_UNDEFINED;
    }
    char doc_url[2048];
    js_get_document_url(ctx, doc_url, sizeof(doc_url));
    platform_http_set_cookie_for_document(doc_url, cookie_line);
    JS_FreeCString(ctx, cookie_line);
    return JS_UNDEFINED;
}

// Document.prototype.styleSheets - return a live-ish collection of stylesheets.
GCValue js_document_get_style_sheets(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue adopted = js_document_get_adopted_style_sheets(ctx, this_val, 0, NULL);
    if (!JS_IsArray(ctx, adopted)) {
        adopted = JS_NewArray(ctx);
    }

    /* Ensure a UA stylesheet is available for scripts that inspect
     * document.styleSheets (e.g. Polymer's style gathering). */
    GCValue ua = JS_GetPropertyStr(ctx, this_val, "__uaStyleSheet");
    if (JS_IsUndefined(ua) || JS_IsNull(ua) || !JS_IsObject(ua)) {
        GCValue global = JS_GetGlobalObject(ctx);
        GCValue ctor = JS_GetPropertyStr(ctx, global, "CSSStyleSheet");
        if (JS_IsFunction(ctx, ctor)) {
            ua = JS_Call(ctx, ctor, JS_UNDEFINED, 0, NULL);
            if (JS_IsObject(ua)) {
                GCValue css_arg = JS_NewString(ctx,
                    "head, script, style, link, meta, title, base, template, noscript { display: none !important; }"
                    " body { margin: 8px; }");
                GCValue repl_args[1] = { css_arg };
                js_css_style_sheet_replace_sync(ctx, ua, 1, repl_args);
                JS_SetPropertyStr(ctx, this_val, "__uaStyleSheet", ua);
            }
        }
    }

    GCValue result = JS_NewArray(ctx);
    uint32_t idx = 0;
    if (JS_IsObject(ua)) {
        JS_SetPropertyUint32(ctx, result, idx++, ua);
    }
    if (JS_IsArray(ctx, adopted)) {
        int32_t alen = 0;
        GCValue lenv = JS_GetPropertyStr(ctx, adopted, "length");
        JS_ToInt32(ctx, &alen, lenv);
        for (int32_t i = 0; i < alen; i++) {
            GCValue item = JS_GetPropertyUint32(ctx, adopted, (uint32_t)i);
            JS_SetPropertyUint32(ctx, result, idx++, item);
        }
    }
    return result;
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
        // Capture the old value before overwriting.
        const char *old_value = NULL;
        DOMNodeHandle node = get_dom_node(ctx, this_val);
        if (node.valid()) {
            old_value = node.get_attribute(name);
        }

        // Store attribute on the object itself
        JS_SetPropertyStr(ctx, this_val, name, JS_NewString(ctx, value));
        
        // Keep the internal DOMNode attribute table in sync so serialization
        // back to HTML produces the mutated attributes.
        if (node.valid()) {
            node.set_attribute(name, value);
        }
        
        // Capture URL if src is being set on any element
        if (name && strcmp(name, "src") == 0 && value && value[0]) {
            capture_url_debug(value, "element_setAttribute_src");
        }

        // Notify custom elements.
        invoke_attribute_changed(ctx, this_val, name, old_value, value);

        // Notify MutationObserver observers.
        char old_buf[512];
        old_buf[0] = '\0';
        if (old_value) {
            strncpy(old_buf, old_value, sizeof(old_buf) - 1);
            old_buf[sizeof(old_buf) - 1] = '\0';
        }
        mo_notify_attribute(ctx, this_val, name, old_buf);
    }
    dom_request_layout();
    return JS_UNDEFINED;
}

// Element.prototype.getAttribute
GCValue js_element_get_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;

    // Read from the authoritative content-attribute store first. This fixes
    // getAttribute('style') returning the live CSSStyleDeclaration object.
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (node.valid()) {
        const char *val = node.get_attribute(name);
        if (val) {
            return JS_NewString(ctx, val);
        }
    }

    // Fall back to a string-valued own property (e.g. el.id set directly).
    GCValue val = JS_GetPropertyStr(ctx, this_val, name);
    if (JS_IsString(val)) {
        const char *str = JS_ToCString(ctx, val);
        if (str) {
            return JS_NewString(ctx, str);
        }
    }
    return JS_NULL;
}

// Element.prototype.removeAttribute
GCValue js_element_remove_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (name) {
        DOMNodeHandle node = get_dom_node(ctx, this_val);
        const char *old_value = node.valid() ? node.get_attribute(name) : NULL;

        JSAtom atom = JS_NewAtom(ctx, name);
        JS_DeleteProperty(ctx, this_val, atom, 0);
        JS_FreeAtom(ctx, atom);
        
        if (node.valid()) {
            node.remove_attribute(name);
        }

        invoke_attribute_changed(ctx, this_val, name, old_value, NULL);

        char old_buf[512];
        old_buf[0] = '\0';
        if (old_value) {
            strncpy(old_buf, old_value, sizeof(old_buf) - 1);
            old_buf[sizeof(old_buf) - 1] = '\0';
        }
        mo_notify_attribute(ctx, this_val, name, old_buf);
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

// Helper: check if a whitespace-separated token list contains a token (case-insensitive).
static bool token_list_contains(const char *list, const char *token, size_t token_len) {
    if (!list || !token || token_len == 0) return false;
    const char *p = list;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        const char *start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        size_t len = (size_t)(p - start);
        if (len == token_len && strncasecmp(start, token, len) == 0) return true;
    }
    return false;
}

// Helper: match an attribute value according to a CSS attribute selector operator.
static bool attr_match(const char *attr_val, int op, const char *val, size_t val_len) {
    if (!attr_val) return false;
    size_t attr_len = strlen(attr_val);
    switch (op) {
        case 0: // [attr] (presence)
            return true;
        case 1: // [attr=value]
            return attr_len == val_len && (val_len == 0 || memcmp(attr_val, val, val_len) == 0);
        case 2: // [attr~=value]
            return token_list_contains(attr_val, val, val_len);
        case 3: // [attr|=value]
            return (attr_len == val_len && strncasecmp(attr_val, val, val_len) == 0) ||
                   (attr_len > val_len && strncasecmp(attr_val, val, val_len) == 0 && attr_val[val_len] == '-');
        case 4: // [attr^=value]
            return attr_len >= val_len && strncasecmp(attr_val, val, val_len) == 0;
        case 5: // [attr$=value]
            return attr_len >= val_len && strncasecmp(attr_val + attr_len - val_len, val, val_len) == 0;
        case 6: // [attr*=value]
            if (val_len == 0) return true;
            for (size_t i = 0; i + val_len <= attr_len; i++) {
                if (strncasecmp(attr_val + i, val, val_len) == 0) return true;
            }
            return false;
    }
    return false;
}

// Compound selector matcher.
// Supports: tag, #id, .class (token-aware), [attr], [attr=value/~=/|=/^=/$=/*=], and simple pseudo-classes.
bool matches_selector(JSContextHandle ctx, GCValue elem, const char* selector) {
    if (!selector || !*selector) return false;

    DOMNodeHandle node = get_dom_node(ctx, elem);
    if (!node.valid() || node.node_type() != DOM_NODE_TYPE_ELEMENT) return false;

    const char *p = selector;

    // Optional type (tag) selector at the start.
    if (isalpha((unsigned char)*p) || *p == '*' || *p == '|') {
        const char *start = p;
        while (*p && *p != '#' && *p != '.' && *p != '[' && *p != ':') p++;
        size_t tag_len = (size_t)(p - start);
        if (tag_len > 0 && !(tag_len == 1 && start[0] == '*')) {
            const char *tag_name = node.node_name();
            size_t node_tag_len = tag_name ? strlen(tag_name) : 0;
            if (tag_len != node_tag_len || strncasecmp(start, tag_name, tag_len) != 0) {
                return false;
            }
        }
    }

    while (*p) {
        if (*p == '#') {
            p++;
            const char *start = p;
            while (*p && *p != '#' && *p != '.' && *p != '[' && *p != ':') p++;
            size_t len = (size_t)(p - start);
            const char *id = node.id();
            size_t id_len = id ? strlen(id) : 0;
            bool id_ok = (len == id_len && strncasecmp(start, id, len) == 0);
            if (!id_ok) {
                GCValue id_val = JS_GetPropertyStr(ctx, elem, "id");
                const char *id_prop = JS_ToCString(ctx, id_val);
                size_t id_prop_len = id_prop ? strlen(id_prop) : 0;
                id_ok = (len == id_prop_len && strncasecmp(start, id_prop, len) == 0);
            }
            if (!id_ok) return false;
        } else if (*p == '.') {
            p++;
            const char *start = p;
            while (*p && *p != '#' && *p != '.' && *p != '[' && *p != ':') p++;
            size_t len = (size_t)(p - start);
            if (!token_list_contains(node.class_name(), start, len)) {
                GCValue class_val = JS_GetPropertyStr(ctx, elem, "className");
                const char *class_prop = JS_ToCString(ctx, class_val);
                if (!token_list_contains(class_prop, start, len)) return false;
            }
        } else if (*p == '[') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            const char *name_start = p;
            while (*p && *p != ']' && *p != '=' && *p != ' ' && *p != '\t' &&
                   *p != '~' && *p != '|' && *p != '^' && *p != '$' && *p != '*') p++;
            size_t name_len = (size_t)(p - name_start);

            int op = 0;
            if (*p == '~' || *p == '|' || *p == '^' || *p == '$' || *p == '*') {
                char op_char = *p;
                p++;
                if (*p == '=') {
                    p++;
                    switch (op_char) {
                        case '~': op = 2; break;
                        case '|': op = 3; break;
                        case '^': op = 4; break;
                        case '$': op = 5; break;
                        case '*': op = 6; break;
                    }
                }
            } else if (*p == '=') {
                op = 1;
                p++;
            }

            while (*p == ' ' || *p == '\t') p++;
            char quote = 0;
            if (*p == '"' || *p == '\'') { quote = *p; p++; }
            const char *val_start = p;
            if (quote) {
                while (*p && *p != quote) p++;
            } else {
                while (*p && *p != ']' && *p != ' ' && *p != '\t') p++;
            }
            size_t val_len = (size_t)(p - val_start);
            if (quote && *p == quote) p++;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != ']') return false; // malformed attribute selector
            p++; // skip ']'

            char *attr_name = (char *)malloc(name_len + 1);
            if (!attr_name) return false;
            memcpy(attr_name, name_start, name_len);
            attr_name[name_len] = '\0';
            const char *attr_val = node.get_attribute(attr_name);
            free(attr_name);
            if (!attr_match(attr_val, op, val_start, val_len)) return false;
        } else if (*p == ':') {
            // Skip pseudo-classes (including functional pseudo-classes like :not(...)).
            p++;
            while (*p && *p != '(' && *p != '#' && *p != '.' && *p != '[' && *p != ':') p++;
            if (*p == '(') {
                int depth = 1;
                p++;
                while (*p && depth > 0) {
                    if (*p == '(') depth++;
                    else if (*p == ')') depth--;
                    p++;
                }
            }
        } else {
            // Skip unexpected characters to avoid infinite loops.
            p++;
        }
    }

    return true;
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

// dataset getter - returns DOMStringMap stub
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
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid() || node.node_type() != DOM_NODE_TYPE_ELEMENT) {
        return JS_NewString(ctx, "");
    }
    char buf[4096];
    buf[0] = '\0';
    size_t pos = 0;
    serialize_element_html(ctx, node, buf, sizeof(buf), &pos);
    return JS_NewString(ctx, buf);
}

// Element.prototype.outerHTML setter
GCValue js_element_set_outer_html(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    const char *html = JS_ToCString(ctx, argv[0]);
    if (!html) html = "";

    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) return JS_UNDEFINED;
    GCValue parent = node.parent_node();
    if (JS_IsNull(parent) || JS_IsUndefined(parent)) return JS_UNDEFINED;

    // Parse the replacement HTML into a temporary container.
    GCValue div_tag = JS_NewString(ctx, "div");
    GCValue temp = js_document_create_element(ctx, parent, 1, &div_tag);
    html_element_set_inner_html(ctx, temp, html);

    // Insert parsed children before this element.
    DOMNodeHandle temp_node = get_dom_node(ctx, temp);
    GCValue child = temp_node.first_child();
    while (!JS_IsNull(child) && JS_IsObject(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        GCValue next = child_node.valid() ? child_node.next_sibling() : JS_NULL;
        GCValue insert_args[2] = { child, this_val };
        js_node_insertBefore_real(ctx, parent, 2, insert_args);
        if (!JS_IsObject(next)) break;
        child = next;
    }

    // Remove the original element.
    GCValue remove_args[1] = { this_val };
    js_node_removeChild_real(ctx, parent, 1, remove_args);

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
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) return JS_NULL;
    int t = node.node_type();
    if (t == DOM_NODE_TYPE_TEXT || t == DOM_NODE_TYPE_COMMENT) {
        const char *val = node.node_value();
        return JS_NewString(ctx, val ? val : "");
    }
    return JS_NULL;
}

// Node.prototype.nodeValue setter
GCValue js_node_set_node_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) return JS_UNDEFINED;
    int t = node.node_type();
    if (t == DOM_NODE_TYPE_TEXT || t == DOM_NODE_TYPE_COMMENT) {
        const char *text = JS_ToCString(ctx, argv[0]);
        if (!text) text = "";
        const char *old = node.node_value();
        char old_buf[256];
        old_buf[0] = '\0';
        if (old) {
            strncpy(old_buf, old, sizeof(old_buf) - 1);
            old_buf[sizeof(old_buf) - 1] = '\0';
        }
        node.set_node_value(text);
        // Keep the .data / .textContent mirrors in sync.
        JS_SetPropertyStr(ctx, this_val, "data", JS_NewString(ctx, text));
        JS_SetPropertyStr(ctx, this_val, "textContent", JS_NewString(ctx, text));
        mo_notify_character_data(ctx, this_val, old_buf);
    }
    return JS_UNDEFINED;
}

// CharacterData.prototype.data getter/setter (used by Text/Comment nodes).
GCValue js_node_get_data(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_node_get_node_value(ctx, this_val, argc, argv);
}

GCValue js_node_set_data(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_node_set_node_value(ctx, this_val, argc, argv);
}

// ============================================================================
// Document Methods Implementation
// ============================================================================

// ============================================================================
// Range / Selection Implementation
// ============================================================================

static GCValue range_get_proto(JSContextHandle ctx, GCValue range) {
    return JS_GetPrototype(ctx, range);
}

static GCValue range_new_instance(JSContextHandle ctx, GCValue range_ctor_or_proto) {
    GCValue proto = JS_NULL;
    if (JS_IsFunction(ctx, range_ctor_or_proto)) {
        proto = JS_GetPropertyStr(ctx, range_ctor_or_proto, "prototype");
    } else if (JS_IsObject(range_ctor_or_proto)) {
        proto = range_ctor_or_proto;
    }
    if (JS_IsUndefined(proto) || JS_IsNull(proto)) {
        proto = JS_NewObject(ctx);
    }
    GCValue range = JS_NewObjectProto(ctx, proto);
    JS_SetPropertyStr(ctx, range, "startContainer", JS_NULL);
    JS_SetPropertyStr(ctx, range, "startOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, range, "endContainer", JS_NULL);
    JS_SetPropertyStr(ctx, range, "endOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, range, "collapsed", JS_TRUE);
    JS_SetPropertyStr(ctx, range, "commonAncestorContainer", JS_NULL);
    return range;
}

static void range_update_collapsed(JSContextHandle ctx, GCValue range) {
    GCValue sc = JS_GetPropertyStr(ctx, range, "startContainer");
    GCValue ec = JS_GetPropertyStr(ctx, range, "endContainer");
    int32_t so = 0, eo = 0;
    JS_ToInt32(ctx, &so, JS_GetPropertyStr(ctx, range, "startOffset"));
    JS_ToInt32(ctx, &eo, JS_GetPropertyStr(ctx, range, "endOffset"));
    JS_SetPropertyStr(ctx, range, "collapsed",
        JS_NewBool(ctx, JS_StrictEq(ctx, sc, ec) && so == eo));
}

static void range_update_common_ancestor(JSContextHandle ctx, GCValue range) {
    GCValue sc = JS_GetPropertyStr(ctx, range, "startContainer");
    GCValue ec = JS_GetPropertyStr(ctx, range, "endContainer");
    if (JS_IsNull(sc) || JS_IsNull(ec) || JS_IsUndefined(sc) || JS_IsUndefined(ec)) {
        JS_SetPropertyStr(ctx, range, "commonAncestorContainer", JS_NULL);
        return;
    }
    GCValue anc = common_ancestor_node(ctx, sc, ec);
    JS_SetPropertyStr(ctx, range, "commonAncestorContainer", anc);
}

static int node_child_index(JSContextHandle ctx, GCValue parent, GCValue child) {
    if (JS_IsNull(parent) || JS_IsNull(child)) return -1;
    DOMNodeHandle p = get_dom_node(ctx, parent);
    if (!p.valid()) return -1;
    int idx = 0;
    GCValue c = p.first_child();
    while (!JS_IsNull(c)) {
        if (JS_StrictEq(ctx, c, child)) return idx;
        DOMNodeHandle cn = get_dom_node(ctx, c);
        if (!cn.valid()) break;
        c = cn.next_sibling();
        idx++;
    }
    return -1;
}

static int node_child_count(JSContextHandle ctx, GCValue parent) {
    if (JS_IsNull(parent)) return 0;
    DOMNodeHandle p = get_dom_node(ctx, parent);
    if (!p.valid()) return 0;
    int count = 0;
    GCValue c = p.first_child();
    while (!JS_IsNull(c)) {
        count++;
        DOMNodeHandle cn = get_dom_node(ctx, c);
        if (!cn.valid()) break;
        c = cn.next_sibling();
    }
    return count;
}

// Range constructor (also used by document.createRange)
GCValue js_range_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return range_new_instance(ctx, new_target);
}

GCValue js_range_set_start(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "setStart requires 2 arguments");
    int32_t offset = 0;
    JS_ToInt32(ctx, &offset, argv[1]);
    JS_SetPropertyStr(ctx, this_val, "startContainer", argv[0]);
    JS_SetPropertyStr(ctx, this_val, "startOffset", JS_NewInt32(ctx, offset));
    range_update_collapsed(ctx, this_val);
    range_update_common_ancestor(ctx, this_val);
    return JS_UNDEFINED;
}

GCValue js_range_set_end(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "setEnd requires 2 arguments");
    int32_t offset = 0;
    JS_ToInt32(ctx, &offset, argv[1]);
    JS_SetPropertyStr(ctx, this_val, "endContainer", argv[0]);
    JS_SetPropertyStr(ctx, this_val, "endOffset", JS_NewInt32(ctx, offset));
    range_update_collapsed(ctx, this_val);
    range_update_common_ancestor(ctx, this_val);
    return JS_UNDEFINED;
}

GCValue js_range_set_start_before(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "setStartBefore requires 1 argument");
    GCValue parent = js_node_get_parentNode(ctx, argv[0], 0, NULL);
    int idx = node_child_index(ctx, parent, argv[0]);
    JS_SetPropertyStr(ctx, this_val, "startContainer", parent);
    JS_SetPropertyStr(ctx, this_val, "startOffset", JS_NewInt32(ctx, idx));
    range_update_collapsed(ctx, this_val);
    range_update_common_ancestor(ctx, this_val);
    return JS_UNDEFINED;
}

GCValue js_range_set_start_after(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "setStartAfter requires 1 argument");
    GCValue parent = js_node_get_parentNode(ctx, argv[0], 0, NULL);
    int idx = node_child_index(ctx, parent, argv[0]);
    JS_SetPropertyStr(ctx, this_val, "startContainer", parent);
    JS_SetPropertyStr(ctx, this_val, "startOffset", JS_NewInt32(ctx, idx + 1));
    range_update_collapsed(ctx, this_val);
    range_update_common_ancestor(ctx, this_val);
    return JS_UNDEFINED;
}

GCValue js_range_set_end_before(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "setEndBefore requires 1 argument");
    GCValue parent = js_node_get_parentNode(ctx, argv[0], 0, NULL);
    int idx = node_child_index(ctx, parent, argv[0]);
    JS_SetPropertyStr(ctx, this_val, "endContainer", parent);
    JS_SetPropertyStr(ctx, this_val, "endOffset", JS_NewInt32(ctx, idx));
    range_update_collapsed(ctx, this_val);
    range_update_common_ancestor(ctx, this_val);
    return JS_UNDEFINED;
}

GCValue js_range_set_end_after(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "setEndAfter requires 1 argument");
    GCValue parent = js_node_get_parentNode(ctx, argv[0], 0, NULL);
    int idx = node_child_index(ctx, parent, argv[0]);
    JS_SetPropertyStr(ctx, this_val, "endContainer", parent);
    JS_SetPropertyStr(ctx, this_val, "endOffset", JS_NewInt32(ctx, idx + 1));
    range_update_collapsed(ctx, this_val);
    range_update_common_ancestor(ctx, this_val);
    return JS_UNDEFINED;
}

GCValue js_range_select_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "selectNode requires 1 argument");
    GCValue parent = js_node_get_parentNode(ctx, argv[0], 0, NULL);
    int idx = node_child_index(ctx, parent, argv[0]);
    JS_SetPropertyStr(ctx, this_val, "startContainer", parent);
    JS_SetPropertyStr(ctx, this_val, "startOffset", JS_NewInt32(ctx, idx));
    JS_SetPropertyStr(ctx, this_val, "endContainer", parent);
    JS_SetPropertyStr(ctx, this_val, "endOffset", JS_NewInt32(ctx, idx + 1));
    JS_SetPropertyStr(ctx, this_val, "collapsed", JS_FALSE);
    range_update_common_ancestor(ctx, this_val);
    return JS_UNDEFINED;
}

GCValue js_range_select_node_contents(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "selectNodeContents requires 1 argument");
    int count = node_child_count(ctx, argv[0]);
    JS_SetPropertyStr(ctx, this_val, "startContainer", argv[0]);
    JS_SetPropertyStr(ctx, this_val, "startOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, this_val, "endContainer", argv[0]);
    JS_SetPropertyStr(ctx, this_val, "endOffset", JS_NewInt32(ctx, count));
    JS_SetPropertyStr(ctx, this_val, "collapsed", JS_NewBool(ctx, count == 0));
    range_update_common_ancestor(ctx, this_val);
    return JS_UNDEFINED;
}

GCValue js_range_collapse(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    bool to_start = true;
    if (argc > 0) to_start = JS_ToBool(ctx, argv[0]);
    if (to_start) {
        GCValue sc = JS_GetPropertyStr(ctx, this_val, "startContainer");
        int32_t so = 0;
        JS_ToInt32(ctx, &so, JS_GetPropertyStr(ctx, this_val, "startOffset"));
        JS_SetPropertyStr(ctx, this_val, "endContainer", sc);
        JS_SetPropertyStr(ctx, this_val, "endOffset", JS_NewInt32(ctx, so));
    } else {
        GCValue ec = JS_GetPropertyStr(ctx, this_val, "endContainer");
        int32_t eo = 0;
        JS_ToInt32(ctx, &eo, JS_GetPropertyStr(ctx, this_val, "endOffset"));
        JS_SetPropertyStr(ctx, this_val, "startContainer", ec);
        JS_SetPropertyStr(ctx, this_val, "startOffset", JS_NewInt32(ctx, eo));
    }
    JS_SetPropertyStr(ctx, this_val, "collapsed", JS_TRUE);
    range_update_common_ancestor(ctx, this_val);
    return JS_UNDEFINED;
}

GCValue js_range_clone_range(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue proto = JS_GetPrototype(ctx, this_val);
    GCValue clone = range_new_instance(ctx, proto);
    GCValue sc = JS_GetPropertyStr(ctx, this_val, "startContainer");
    GCValue so = JS_GetPropertyStr(ctx, this_val, "startOffset");
    GCValue ec = JS_GetPropertyStr(ctx, this_val, "endContainer");
    GCValue eo = JS_GetPropertyStr(ctx, this_val, "endOffset");
    JS_SetPropertyStr(ctx, clone, "startContainer", sc);
    JS_SetPropertyStr(ctx, clone, "startOffset", so);
    JS_SetPropertyStr(ctx, clone, "endContainer", ec);
    JS_SetPropertyStr(ctx, clone, "endOffset", eo);
    range_update_collapsed(ctx, clone);
    range_update_common_ancestor(ctx, clone);
    return clone;
}

GCValue js_range_to_string(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    // Full text extraction is complex; return the textContent of the common
    // ancestor as a best-effort approximation.
    GCValue anc = JS_GetPropertyStr(ctx, this_val, "commonAncestorContainer");
    if (JS_IsNull(anc) || JS_IsUndefined(anc)) return JS_NewString(ctx, "");
    GCValue tc = JS_GetPropertyStr(ctx, anc, "textContent");
    if (JS_IsUndefined(tc) || JS_IsNull(tc)) return JS_NewString(ctx, "");
    return tc;
}

GCValue js_range_get_bounding_client_rect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue rect = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rect, "x", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, rect, "y", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, rect, "width", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, rect, "height", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, rect, "top", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, rect, "left", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, rect, "right", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, rect, "bottom", JS_NewFloat64(ctx, 0));
    return rect;
}

GCValue js_range_get_client_rects(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return JS_NewArray(ctx);
}

GCValue js_range_detach(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return JS_UNDEFINED;
}

GCValue js_range_delete_contents(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    // Best-effort: if the range is collapsed, nothing to do.
    GCValue collapsed = JS_GetPropertyStr(ctx, this_val, "collapsed");
    if (JS_ToBool(ctx, collapsed)) return JS_UNDEFINED;
    // Otherwise clear the endpoints to avoid repeated deletions.
    JS_SetPropertyStr(ctx, this_val, "endContainer", JS_GetPropertyStr(ctx, this_val, "startContainer"));
    JS_SetPropertyStr(ctx, this_val, "endOffset", JS_GetPropertyStr(ctx, this_val, "startOffset"));
    JS_SetPropertyStr(ctx, this_val, "collapsed", JS_TRUE);
    return JS_UNDEFINED;
}

GCValue js_range_extract_contents(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue frag = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, frag, "nodeType", JS_NewInt32(ctx, DOM_NODE_TYPE_DOCUMENT_FRAGMENT));
    // Deletion is a no-op for now; return an empty fragment.
    js_range_delete_contents(ctx, this_val, 0, NULL);
    return frag;
}

GCValue js_range_insert_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "insertNode requires 1 argument");
    GCValue sc = JS_GetPropertyStr(ctx, this_val, "startContainer");
    int32_t so = 0;
    JS_ToInt32(ctx, &so, JS_GetPropertyStr(ctx, this_val, "startOffset"));
    DOMNodeHandle n = get_dom_node(ctx, sc);
    if (n.valid() && n.node_type() == DOM_NODE_TYPE_ELEMENT) {
        GCValue child = n.first_child();
        int idx = 0;
        while (!JS_IsNull(child) && idx < so) {
            DOMNodeHandle cn = get_dom_node(ctx, child);
            if (!cn.valid()) break;
            child = cn.next_sibling();
            idx++;
        }
        GCValue args[2] = { argv[0], child };
        js_node_insertBefore_real(ctx, sc, 2, args);
    }
    return JS_UNDEFINED;
}

GCValue js_range_surround_contents(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return JS_UNDEFINED;
}

const JSCFunctionListEntry js_range_proto_funcs[] = {
    JS_CFUNC_DEF("setStart", 2, js_range_set_start),
    JS_CFUNC_DEF("setEnd", 2, js_range_set_end),
    JS_CFUNC_DEF("setStartBefore", 1, js_range_set_start_before),
    JS_CFUNC_DEF("setStartAfter", 1, js_range_set_start_after),
    JS_CFUNC_DEF("setEndBefore", 1, js_range_set_end_before),
    JS_CFUNC_DEF("setEndAfter", 1, js_range_set_end_after),
    JS_CFUNC_DEF("selectNode", 1, js_range_select_node),
    JS_CFUNC_DEF("selectNodeContents", 1, js_range_select_node_contents),
    JS_CFUNC_DEF("collapse", 1, js_range_collapse),
    JS_CFUNC_DEF("cloneRange", 0, js_range_clone_range),
    JS_CFUNC_DEF("toString", 0, js_range_to_string),
    JS_CFUNC_DEF("getBoundingClientRect", 0, js_range_get_bounding_client_rect),
    JS_CFUNC_DEF("getClientRects", 0, js_range_get_client_rects),
    JS_CFUNC_DEF("detach", 0, js_range_detach),
    JS_CFUNC_DEF("deleteContents", 0, js_range_delete_contents),
    JS_CFUNC_DEF("extractContents", 0, js_range_extract_contents),
    JS_CFUNC_DEF("insertNode", 1, js_range_insert_node),
    JS_CFUNC_DEF("surroundContents", 1, js_range_surround_contents),
};
const size_t js_range_proto_funcs_count = sizeof(js_range_proto_funcs) / sizeof(js_range_proto_funcs[0]);

// document.createRange() returns a Range instance.
GCValue js_document_create_range(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue range_ctor = JS_GetPropertyStr(ctx, global, "Range");
    GCValue range_proto = JS_GetPropertyStr(ctx, range_ctor, "prototype");
    GCValue range = JS_NewObjectProto(ctx, range_proto);
    JS_SetPropertyStr(ctx, range, "startContainer", JS_NULL);
    JS_SetPropertyStr(ctx, range, "startOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, range, "endContainer", JS_NULL);
    JS_SetPropertyStr(ctx, range, "endOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, range, "collapsed", JS_TRUE);
    JS_SetPropertyStr(ctx, range, "commonAncestorContainer", JS_NULL);
    return range;
}

// Selection helper: update Selection properties from its stored Range.
static void selection_update_from_range(JSContextHandle ctx, GCValue selection) {
    GCValue range = JS_GetPropertyStr(ctx, selection, "__range");
    if (JS_IsNull(range) || JS_IsUndefined(range)) {
        JS_SetPropertyStr(ctx, selection, "anchorNode", JS_NULL);
        JS_SetPropertyStr(ctx, selection, "anchorOffset", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, selection, "focusNode", JS_NULL);
        JS_SetPropertyStr(ctx, selection, "focusOffset", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, selection, "isCollapsed", JS_TRUE);
        JS_SetPropertyStr(ctx, selection, "rangeCount", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, selection, "type", JS_NewString(ctx, "None"));
        return;
    }
    GCValue sc = JS_GetPropertyStr(ctx, range, "startContainer");
    GCValue so = JS_GetPropertyStr(ctx, range, "startOffset");
    GCValue ec = JS_GetPropertyStr(ctx, range, "endContainer");
    GCValue eo = JS_GetPropertyStr(ctx, range, "endOffset");
    GCValue collapsed = JS_GetPropertyStr(ctx, range, "collapsed");
    JS_SetPropertyStr(ctx, selection, "anchorNode", sc);
    JS_SetPropertyStr(ctx, selection, "anchorOffset", so);
    JS_SetPropertyStr(ctx, selection, "focusNode", ec);
    JS_SetPropertyStr(ctx, selection, "focusOffset", eo);
    JS_SetPropertyStr(ctx, selection, "isCollapsed", collapsed);
    JS_SetPropertyStr(ctx, selection, "rangeCount", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, selection, "type", JS_ToBool(ctx, collapsed) ? JS_NewString(ctx, "Caret") : JS_NewString(ctx, "Range"));
}

GCValue js_selection_get_range_at(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue range = JS_GetPropertyStr(ctx, this_val, "__range");
    if (JS_IsNull(range) || JS_IsUndefined(range)) return JS_NULL;
    return range;
}

GCValue js_selection_remove_all_ranges(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    JS_SetPropertyStr(ctx, this_val, "__range", JS_NULL);
    selection_update_from_range(ctx, this_val);
    return JS_UNDEFINED;
}

GCValue js_selection_add_range(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    JS_SetPropertyStr(ctx, this_val, "__range", argv[0]);
    selection_update_from_range(ctx, this_val);
    return JS_UNDEFINED;
}

GCValue js_selection_remove_range(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue current = JS_GetPropertyStr(ctx, this_val, "__range");
    if (argc > 0 && JS_StrictEq(ctx, current, argv[0])) {
        JS_SetPropertyStr(ctx, this_val, "__range", JS_NULL);
    }
    selection_update_from_range(ctx, this_val);
    return JS_UNDEFINED;
}

GCValue js_selection_collapse(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    GCValue range = js_document_create_range(ctx, JS_NULL, 0, NULL);
    int32_t offset = 0;
    if (argc > 1) JS_ToInt32(ctx, &offset, argv[1]);
    JS_SetPropertyStr(ctx, range, "startContainer", argv[0]);
    JS_SetPropertyStr(ctx, range, "startOffset", JS_NewInt32(ctx, offset));
    JS_SetPropertyStr(ctx, range, "endContainer", argv[0]);
    JS_SetPropertyStr(ctx, range, "endOffset", JS_NewInt32(ctx, offset));
    JS_SetPropertyStr(ctx, range, "collapsed", JS_TRUE);
    range_update_common_ancestor(ctx, range);
    JS_SetPropertyStr(ctx, this_val, "__range", range);
    selection_update_from_range(ctx, this_val);
    return JS_UNDEFINED;
}

GCValue js_selection_extend(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    GCValue range = JS_GetPropertyStr(ctx, this_val, "__range");
    if (JS_IsNull(range) || JS_IsUndefined(range)) return JS_UNDEFINED;
    int32_t offset = 0;
    JS_ToInt32(ctx, &offset, argv[1]);
    JS_SetPropertyStr(ctx, range, "endContainer", argv[0]);
    JS_SetPropertyStr(ctx, range, "endOffset", JS_NewInt32(ctx, offset));
    range_update_collapsed(ctx, range);
    range_update_common_ancestor(ctx, range);
    selection_update_from_range(ctx, this_val);
    return JS_UNDEFINED;
}

GCValue js_selection_select_all_children(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    GCValue range = js_document_create_range(ctx, JS_NULL, 0, NULL);
    js_range_select_node_contents(ctx, range, 1, argv);
    JS_SetPropertyStr(ctx, this_val, "__range", range);
    selection_update_from_range(ctx, this_val);
    return JS_UNDEFINED;
}

GCValue js_selection_delete_from_document(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue range = JS_GetPropertyStr(ctx, this_val, "__range");
    if (!JS_IsNull(range) && !JS_IsUndefined(range)) {
        js_range_delete_contents(ctx, range, 0, NULL);
        selection_update_from_range(ctx, this_val);
    }
    return JS_UNDEFINED;
}

GCValue js_selection_to_string(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue range = JS_GetPropertyStr(ctx, this_val, "__range");
    if (JS_IsNull(range) || JS_IsUndefined(range)) return JS_NewString(ctx, "");
    return js_range_to_string(ctx, range, 0, NULL);
}

static GCValue js_selection_contains_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_FALSE;
}

static void js_selection_attach_methods(JSContextHandle ctx, GCValue selection) {
    JS_SetPropertyStr(ctx, selection, "getRangeAt",
        JS_NewCFunction(ctx, js_selection_get_range_at, "getRangeAt", 1));
    JS_SetPropertyStr(ctx, selection, "removeAllRanges",
        JS_NewCFunction(ctx, js_selection_remove_all_ranges, "removeAllRanges", 0));
    JS_SetPropertyStr(ctx, selection, "addRange",
        JS_NewCFunction(ctx, js_selection_add_range, "addRange", 1));
    JS_SetPropertyStr(ctx, selection, "removeRange",
        JS_NewCFunction(ctx, js_selection_remove_range, "removeRange", 1));
    JS_SetPropertyStr(ctx, selection, "collapse",
        JS_NewCFunction(ctx, js_selection_collapse, "collapse", 2));
    JS_SetPropertyStr(ctx, selection, "extend",
        JS_NewCFunction(ctx, js_selection_extend, "extend", 2));
    JS_SetPropertyStr(ctx, selection, "selectAllChildren",
        JS_NewCFunction(ctx, js_selection_select_all_children, "selectAllChildren", 1));
    JS_SetPropertyStr(ctx, selection, "deleteFromDocument",
        JS_NewCFunction(ctx, js_selection_delete_from_document, "deleteFromDocument", 0));
    JS_SetPropertyStr(ctx, selection, "toString",
        JS_NewCFunction(ctx, js_selection_to_string, "toString", 0));
    JS_SetPropertyStr(ctx, selection, "containsNode",
        JS_NewCFunction(ctx, js_selection_contains_node, "containsNode", 1));
}

GCValue js_get_selection(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue selection = JS_GetPropertyStr(ctx, global, "__cyberSelection");
    if (JS_IsUndefined(selection) || JS_IsNull(selection)) {
        selection = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, selection, "__range", JS_NULL);
        js_selection_attach_methods(ctx, selection);
        selection_update_from_range(ctx, selection);
        JS_SetPropertyStr(ctx, global, "__cyberSelection", selection);
    }
    return selection;
}

// ============================================================================
// DOMParser / XMLSerializer Implementation
// ============================================================================

GCValue js_dom_parser_parse_from_string(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "parseFromString requires 2 arguments");
    const char *str = JS_ToCString(ctx, argv[0]);
    const char *mime = JS_ToCString(ctx, argv[1]);
    if (!str) str = "";
    if (!mime) mime = "text/html";

    bool as_xml = (strstr(mime, "xml") != NULL);
    HtmlDocument *doc = html_parse(str, strlen(str));
    if (!doc) return JS_NULL;

    GCValue js_doc = html_create_js_document(ctx, doc);
    html_document_free(doc);

    if (!JS_IsUndefined(js_doc) && !JS_IsNull(js_doc)) {
        JS_SetPropertyStr(ctx, js_doc, "contentType", JS_NewString(ctx, as_xml ? "application/xml" : "text/html"));
    }
    return js_doc;
}

GCValue js_dom_parser_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)new_target; (void)argc; (void)argv;
    GCValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "parseFromString",
        JS_NewCFunction(ctx, js_dom_parser_parse_from_string, "parseFromString", 2));
    return obj;
}

GCValue js_xml_serializer_serialize_to_string(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) return JS_NewString(ctx, "");
    char *html = html_serialize_js_node(ctx, argv[0]);
    GCValue result = JS_NewString(ctx, html ? html : "");
    free(html);
    return result;
}

GCValue js_xml_serializer_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)new_target; (void)argc; (void)argv;
    GCValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "serializeToString",
        JS_NewCFunction(ctx, js_xml_serializer_serialize_to_string, "serializeToString", 1));
    return obj;
}

// ============================================================================
// TreeWalker Implementation
// ============================================================================

static GCValue tree_walker_get_current(JSContextHandle ctx, GCValue tw) {
    return JS_GetPropertyStr(ctx, tw, "currentNode");
}

static void tree_walker_set_current(JSContextHandle ctx, GCValue tw, GCValue node) {
    JS_SetPropertyStr(ctx, tw, "currentNode", node);
}

static GCValue tree_walker_get_root(JSContextHandle ctx, GCValue tw) {
    return JS_GetPropertyStr(ctx, tw, "root");
}

static int tree_walker_accept_node(JSContextHandle ctx, GCValue tw, GCValue node) {
    if (JS_IsNull(node) || JS_IsUndefined(node) || !JS_IsObject(node)) return 0;
    GCValue filter = JS_GetPropertyStr(ctx, tw, "filter");
    if (JS_IsFunction(ctx, filter)) {
        GCValue args[1] = { node };
        GCValue result = JS_Call(ctx, filter, tw, 1, args);
        if (JS_IsException(result)) return 0;
        int32_t code = 0;
        JS_ToInt32(ctx, &code, result);
        return code == 1; // NodeFilter.FILTER_ACCEPT
    }
    return 1;
}

GCValue js_tree_walker_parent_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue current = tree_walker_get_current(ctx, this_val);
    GCValue root = tree_walker_get_root(ctx, this_val);
    if (JS_IsNull(current) || JS_IsUndefined(current) || JS_StrictEq(ctx, current, root)) return JS_NULL;
    DOMNodeHandle node = get_dom_node(ctx, current);
    if (!node.valid()) return JS_NULL;
    GCValue parent = node.parent_node();
    if (JS_IsNull(parent) || JS_StrictEq(ctx, parent, root)) return JS_NULL;
    if (tree_walker_accept_node(ctx, this_val, parent)) {
        tree_walker_set_current(ctx, this_val, parent);
        return parent;
    }
    return JS_NULL;
}

GCValue js_tree_walker_first_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue current = tree_walker_get_current(ctx, this_val);
    if (JS_IsNull(current) || JS_IsUndefined(current)) return JS_NULL;
    DOMNodeHandle node = get_dom_node(ctx, current);
    if (!node.valid()) return JS_NULL;
    GCValue child = node.first_child();
    while (!JS_IsNull(child)) {
        if (tree_walker_accept_node(ctx, this_val, child)) {
            tree_walker_set_current(ctx, this_val, child);
            return child;
        }
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }
    return JS_NULL;
}

GCValue js_tree_walker_last_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue current = tree_walker_get_current(ctx, this_val);
    if (JS_IsNull(current) || JS_IsUndefined(current)) return JS_NULL;
    DOMNodeHandle node = get_dom_node(ctx, current);
    if (!node.valid()) return JS_NULL;
    GCValue child = node.last_child();
    while (!JS_IsNull(child)) {
        if (tree_walker_accept_node(ctx, this_val, child)) {
            tree_walker_set_current(ctx, this_val, child);
            return child;
        }
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (!child_node.valid()) break;
        child = child_node.previous_sibling();
    }
    return JS_NULL;
}

GCValue js_tree_walker_next_sibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue current = tree_walker_get_current(ctx, this_val);
    if (JS_IsNull(current) || JS_IsUndefined(current)) return JS_NULL;
    GCValue root = tree_walker_get_root(ctx, this_val);
    DOMNodeHandle node = get_dom_node(ctx, current);
    while (node.valid()) {
        GCValue sib = node.next_sibling();
        if (JS_IsNull(sib)) break;
        if (!JS_StrictEq(ctx, sib, root) && tree_walker_accept_node(ctx, this_val, sib)) {
            tree_walker_set_current(ctx, this_val, sib);
            return sib;
        }
        node = get_dom_node(ctx, sib);
    }
    return JS_NULL;
}

GCValue js_tree_walker_previous_sibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue current = tree_walker_get_current(ctx, this_val);
    if (JS_IsNull(current) || JS_IsUndefined(current)) return JS_NULL;
    GCValue root = tree_walker_get_root(ctx, this_val);
    DOMNodeHandle node = get_dom_node(ctx, current);
    while (node.valid()) {
        GCValue sib = node.previous_sibling();
        if (JS_IsNull(sib)) break;
        if (!JS_StrictEq(ctx, sib, root) && tree_walker_accept_node(ctx, this_val, sib)) {
            tree_walker_set_current(ctx, this_val, sib);
            return sib;
        }
        node = get_dom_node(ctx, sib);
    }
    return JS_NULL;
}

GCValue js_tree_walker_next_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue current = tree_walker_get_current(ctx, this_val);
    GCValue root = tree_walker_get_root(ctx, this_val);
    if (JS_IsNull(current) || JS_IsUndefined(current)) return JS_NULL;

    GCValue node = current;
    while (!JS_IsNull(node) && JS_IsObject(node)) {
        DOMNodeHandle n = get_dom_node(ctx, node);
        if (!n.valid()) break;
        GCValue child = n.first_child();
        while (!JS_IsNull(child)) {
            if (tree_walker_accept_node(ctx, this_val, child)) {
                tree_walker_set_current(ctx, this_val, child);
                return child;
            }
            DOMNodeHandle child_node = get_dom_node(ctx, child);
            if (!child_node.valid()) break;
            child = child_node.next_sibling();
        }
        // No accepted children; try next sibling, then ancestors' next siblings.
        while (!JS_IsNull(node) && JS_IsObject(node)) {
            if (JS_StrictEq(ctx, node, root)) return JS_NULL;
            DOMNodeHandle n2 = get_dom_node(ctx, node);
            if (!n2.valid()) return JS_NULL;
            GCValue sib = n2.next_sibling();
            while (!JS_IsNull(sib)) {
                if (tree_walker_accept_node(ctx, this_val, sib)) {
                    tree_walker_set_current(ctx, this_val, sib);
                    return sib;
                }
                DOMNodeHandle sib_node = get_dom_node(ctx, sib);
                if (!sib_node.valid()) break;
                sib = sib_node.next_sibling();
            }
            node = n2.parent_node();
        }
    }
    return JS_NULL;
}

GCValue js_tree_walker_previous_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue current = tree_walker_get_current(ctx, this_val);
    GCValue root = tree_walker_get_root(ctx, this_val);
    if (JS_IsNull(current) || JS_IsUndefined(current)) return JS_NULL;

    GCValue node = current;
    while (!JS_IsNull(node) && JS_IsObject(node)) {
        if (JS_StrictEq(ctx, node, root)) return JS_NULL;
        DOMNodeHandle n = get_dom_node(ctx, node);
        if (!n.valid()) break;
        GCValue sib = n.previous_sibling();
        if (!JS_IsNull(sib)) {
            // Descend to deepest last-child of previous sibling.
            node = sib;
            while (!JS_IsNull(node)) {
                DOMNodeHandle n2 = get_dom_node(ctx, node);
                if (!n2.valid()) break;
                GCValue last = n2.last_child();
                if (JS_IsNull(last)) break;
                node = last;
            }
            if (tree_walker_accept_node(ctx, this_val, node)) {
                tree_walker_set_current(ctx, this_val, node);
                return node;
            }
            continue;
        }
        GCValue parent = n.parent_node();
        if (JS_IsNull(parent) || JS_StrictEq(ctx, parent, root)) return JS_NULL;
        if (tree_walker_accept_node(ctx, this_val, parent)) {
            tree_walker_set_current(ctx, this_val, parent);
            return parent;
        }
        node = parent;
    }
    return JS_NULL;
}

// document.createTreeWalker()
GCValue js_document_create_tree_walker(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    GCValue tree_walker = JS_NewObject(ctx);
    GCValue root = (argc >= 1) ? argv[0] : JS_NULL;
    int32_t whatToShow = 0xFFFFFFFF; // SHOW_ALL
    if (argc >= 2) JS_ToInt32(ctx, &whatToShow, argv[1]);
    GCValue filter = (argc >= 3 && !JS_IsNull(argv[2]) && !JS_IsUndefined(argv[2])) ? argv[2] : JS_NULL;

    JS_SetPropertyStr(ctx, tree_walker, "root", root);
    JS_SetPropertyStr(ctx, tree_walker, "currentNode", root);
    JS_SetPropertyStr(ctx, tree_walker, "whatToShow", JS_NewInt32(ctx, whatToShow));
    JS_SetPropertyStr(ctx, tree_walker, "filter", filter);
    JS_SetPropertyStr(ctx, tree_walker, "firstChild", JS_NewCFunction(ctx, js_tree_walker_first_child, "firstChild", 0));
    JS_SetPropertyStr(ctx, tree_walker, "lastChild", JS_NewCFunction(ctx, js_tree_walker_last_child, "lastChild", 0));
    JS_SetPropertyStr(ctx, tree_walker, "nextNode", JS_NewCFunction(ctx, js_tree_walker_next_node, "nextNode", 0));
    JS_SetPropertyStr(ctx, tree_walker, "nextSibling", JS_NewCFunction(ctx, js_tree_walker_next_sibling, "nextSibling", 0));
    JS_SetPropertyStr(ctx, tree_walker, "parentNode", JS_NewCFunction(ctx, js_tree_walker_parent_node, "parentNode", 0));
    JS_SetPropertyStr(ctx, tree_walker, "previousNode", JS_NewCFunction(ctx, js_tree_walker_previous_node, "previousNode", 0));
    JS_SetPropertyStr(ctx, tree_walker, "previousSibling", JS_NewCFunction(ctx, js_tree_walker_previous_sibling, "previousSibling", 0));
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

// Helper: create a real DocumentFragment backed by the DOM node tree.
GCValue js_create_document_fragment(JSContextHandle ctx) {
    GCValue frag = JS_NewObjectClass(ctx, js_dom_node_class_id);
    if (JS_IsException(frag)) return frag;

    DOMNodeHandle frag_node = DOMNodeHandle::create(ctx, DOM_NODE_TYPE_DOCUMENT_FRAGMENT, "#document-fragment");
    if (!frag_node.valid()) {
        return JS_ThrowInternalError(ctx, "failed to create DocumentFragment");
    }
    frag_node.attach_to_object(frag);

    GCValue global = JS_GetGlobalObject(ctx);
    GCValue df_ctor = JS_GetPropertyStr(ctx, global, "DocumentFragment");
    if (!JS_IsUndefined(df_ctor) && !JS_IsException(df_ctor)) {
        GCValue df_proto = JS_GetPropertyStr(ctx, df_ctor, "prototype");
        if (!JS_IsUndefined(df_proto) && !JS_IsException(df_proto)) {
            JS_SetPrototype(ctx, frag, df_proto);
        } else {
            GCValue node_ctor = JS_GetPropertyStr(ctx, global, "Node");
            if (!JS_IsUndefined(node_ctor) && !JS_IsException(node_ctor)) {
                GCValue node_proto = JS_GetPropertyStr(ctx, node_ctor, "prototype");
                if (!JS_IsUndefined(node_proto) && !JS_IsException(node_proto)) {
                    JS_SetPrototype(ctx, frag, node_proto);
                }
            }
        }
    }
    return frag;
}

// document.createDocumentFragment()
GCValue js_document_create_document_fragment(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue frag = js_create_document_fragment(ctx);
    if (!JS_IsException(frag) && !JS_IsUndefined(frag) && !JS_IsNull(frag)) {
        dom_node_set_owner_document(ctx, frag, this_val);
    }
    return frag;
}

// document.importNode()
GCValue js_document_import_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "importNode requires a node");
    }

    bool deep = false;
    if (argc > 1) {
        deep = JS_ToBool(ctx, argv[1]);
    }

    GCValue deep_val = JS_NewBool(ctx, deep);
    GCValue clone_args[1] = { deep_val };
    GCValue clone = js_node_cloneNode_real(ctx, argv[0], 1, clone_args);
    if (JS_IsException(clone)) {
        return clone;
    }

    dom_node_set_owner_document(ctx, clone, this_val);
    return clone;
}

// Recursive hit-test helper for elementFromPoint.
static GCValue hit_test_layout_node(JSContextHandle ctx, LayoutContext *layout, int idx, double x, double y) {
    if (!layout || idx < 0 || idx >= layout->tree.count) return JS_NULL;
    LayoutBox *box = &layout->boxes[idx];
    LayoutNodeRef *ref = &layout->tree.nodes[idx];

    bool inside = (x >= box->x && x <= box->x + box->width &&
                   y >= box->y && y <= box->y + box->height);
    if (!inside) return JS_NULL;

    // Try children in reverse DOM order (last sibling paints on top).
    int last_child = -1;
    int child = ref->first_child_idx;
    while (child >= 0) {
        last_child = child;
        child = layout->tree.nodes[child].next_sibling_idx;
    }
    while (last_child >= 0) {
        GCValue child_hit = hit_test_layout_node(ctx, layout, last_child, x, y);
        if (!JS_IsNull(child_hit)) return child_hit;
        last_child = layout->tree.nodes[last_child].prev_sibling_idx;
    }

    // Deepest hit: map back to the JS object if this is an element.
    if (ref->dom_node_idx >= 0 && layout->doc) {
        HtmlNode *hnode = (HtmlNode *)po_array_payload(&layout->doc->array, ref->dom_node_idx);
        if (hnode && hnode->has_js_object && hnode->type == HTML_NODE_ELEMENT &&
            !JS_IsUndefined(hnode->js_object) && !JS_IsNull(hnode->js_object)) {
            return hnode->js_object;
        }
    }
    return JS_NULL;
}

// document.elementFromPoint()
GCValue js_document_element_from_point(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    double x = 0, y = 0;
    if (argc > 0) JS_ToFloat64(ctx, &x, argv[0]);
    if (argc > 1) JS_ToFloat64(ctx, &y, argv[1]);

    GCValue doc_elem = JS_GetPropertyStr(ctx, this_val, "documentElement");
    if (JS_IsUndefined(doc_elem) || JS_IsNull(doc_elem)) return JS_NULL;

    // Build a native HtmlDocument from the current JS DOM and run layout.
    HtmlDocument *doc = html_document_from_js_dom(ctx, this_val);
    if (!doc) return doc_elem;

    LayoutContext layout;
    memset(&layout, 0, sizeof(layout));
    bool ok = css_layout_run(&layout, doc, NULL, 1920.0, 1080.0);
    GCValue result = doc_elem;
    if (ok && layout.tree.root_idx >= 0) {
        GCValue hit = hit_test_layout_node(ctx, &layout, layout.tree.root_idx, x, y);
        if (!JS_IsNull(hit)) result = hit;
    }
    if (ok) css_layout_tree_free(&layout);
    html_document_free(doc);
    return result;
}


// ============================================================================
// Element.attributes / NamedNodeMap helpers
// ============================================================================

static GCValue create_attr_obj(JSContextHandle ctx, GCValue owner_element,
                               const char *name, const char *value) {
    GCValue attr = JS_NewObject(ctx);
    if (JS_IsException(attr)) return attr;
    JS_SetPropertyStr(ctx, attr, "name", JS_NewString(ctx, name ? name : ""));
    JS_SetPropertyStr(ctx, attr, "value", JS_NewString(ctx, value ? value : ""));
    JS_SetPropertyStr(ctx, attr, "nodeName", JS_NewString(ctx, name ? name : ""));
    JS_SetPropertyStr(ctx, attr, "nodeValue", JS_NewString(ctx, value ? value : ""));
    JS_SetPropertyStr(ctx, attr, "namespaceURI", JS_NULL);
    JS_SetPropertyStr(ctx, attr, "specified", JS_TRUE);
    JS_SetPropertyStr(ctx, attr, "ownerElement", owner_element);
    return attr;
}

static GCValue named_node_map_get_attr(JSContextHandle ctx, GCValue map,
                                       const char *name) {
    if (!name) return JS_NULL;
    GCValue elem = JS_GetPropertyStr(ctx, map, "ownerElement");
    if (!JS_IsObject(elem)) return JS_NULL;
    DOMNodeHandle node = get_dom_node(ctx, elem);
    if (!node.valid()) return JS_NULL;
    const char *value = node.get_attribute(name);
    if (!value) return JS_NULL;
    return create_attr_obj(ctx, elem, name, value);
}

static GCValue named_node_map_get_item(JSContextHandle ctx, GCValue map, int index) {
    GCValue elem = JS_GetPropertyStr(ctx, map, "ownerElement");
    if (!JS_IsObject(elem)) return JS_NULL;
    DOMNodeHandle node = get_dom_node(ctx, elem);
    if (!node.valid()) return JS_NULL;
    int count = node.attribute_count();
    const DOMAttribute *attrs = node.attributes();
    if (index < 0 || index >= count || !attrs) return JS_NULL;
    return create_attr_obj(ctx, elem, attrs[index].name, attrs[index].value);
}

GCValue js_named_node_map_get_named_item(JSContextHandle ctx, GCValue this_val,
                                         int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    return named_node_map_get_attr(ctx, this_val, name);
}

GCValue js_named_node_map_get_named_item_ns(JSContextHandle ctx, GCValue this_val,
                                            int argc, GCValue *argv) {
    (void)argc;
    if (argc < 2) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[1]);
    return named_node_map_get_attr(ctx, this_val, name);
}

GCValue js_named_node_map_item(JSContextHandle ctx, GCValue this_val,
                               int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    int32_t index = 0;
    JS_ToInt32(ctx, &index, argv[0]);
    return named_node_map_get_item(ctx, this_val, index);
}

GCValue js_named_node_map_set_named_item(JSContextHandle ctx, GCValue this_val,
                                         int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "setNamedItem requires an attr node");
    GCValue elem = JS_GetPropertyStr(ctx, this_val, "ownerElement");
    if (!JS_IsObject(elem)) return JS_NULL;

    GCValue name_val = JS_GetPropertyStr(ctx, argv[0], "name");
    GCValue value_val = JS_GetPropertyStr(ctx, argv[0], "value");
    const char *name = JS_ToCString(ctx, name_val);
    const char *value = JS_ToCString(ctx, value_val);
    if (name) {
        GCValue set_args[2];
        set_args[0] = JS_NewString(ctx, name);
        set_args[1] = JS_NewString(ctx, value ? value : "");
        js_element_set_attribute(ctx, elem, 2, set_args);
    }
    return argv[0];
}

GCValue js_named_node_map_remove_named_item(JSContextHandle ctx, GCValue this_val,
                                            int argc, GCValue *argv) {
    if (argc < 1) return throw_dom_exception(ctx, "NotFoundError", "Attribute not found");
    GCValue elem = JS_GetPropertyStr(ctx, this_val, "ownerElement");
    if (!JS_IsObject(elem)) return throw_dom_exception(ctx, "NotFoundError", "Attribute not found");

    DOMNodeHandle node = get_dom_node(ctx, elem);
    if (!node.valid()) return throw_dom_exception(ctx, "NotFoundError", "Attribute not found");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return throw_dom_exception(ctx, "NotFoundError", "Attribute not found");

    const char *old_value = node.get_attribute(name);
    if (!old_value) return throw_dom_exception(ctx, "NotFoundError", "Attribute not found");

    GCValue old_attr = create_attr_obj(ctx, elem, name, old_value);
    GCValue rem_args[1] = { JS_NewString(ctx, name) };
    js_element_remove_attribute(ctx, elem, 1, rem_args);
    return old_attr;
}

// Element.prototype.attributes getter
GCValue js_element_get_attributes(JSContextHandle ctx, GCValue this_val,
                                  int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue map = JS_NewObject(ctx);
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    int count = node.valid() ? node.attribute_count() : 0;
    const DOMAttribute *attrs = node.valid() ? node.attributes() : nullptr;

    for (int i = 0; i < count && attrs; i++) {
        GCValue attr = create_attr_obj(ctx, this_val, attrs[i].name, attrs[i].value);
        JS_SetPropertyUint32(ctx, map, (uint32_t)i, attr);
    }

    JS_SetPropertyStr(ctx, map, "length", JS_NewInt32(ctx, count));
    JS_SetPropertyStr(ctx, map, "ownerElement", this_val);
    JS_SetPropertyStr(ctx, map, "getNamedItem",
        JS_NewCFunction(ctx, js_named_node_map_get_named_item, "getNamedItem", 1));
    JS_SetPropertyStr(ctx, map, "getNamedItemNS",
        JS_NewCFunction(ctx, js_named_node_map_get_named_item_ns, "getNamedItemNS", 2));
    JS_SetPropertyStr(ctx, map, "item",
        JS_NewCFunction(ctx, js_named_node_map_item, "item", 1));
    JS_SetPropertyStr(ctx, map, "setNamedItem",
        JS_NewCFunction(ctx, js_named_node_map_set_named_item, "setNamedItem", 1));
    JS_SetPropertyStr(ctx, map, "removeNamedItem",
        JS_NewCFunction(ctx, js_named_node_map_remove_named_item, "removeNamedItem", 1));
    return map;
}

// Element.prototype.hasAttributes()
GCValue js_element_has_attributes(JSContextHandle ctx, GCValue this_val,
                                  int argc, GCValue *argv) {
    (void)argc; (void)argv;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    bool has = node.valid() && node.attribute_count() > 0;
    return JS_NewBool(ctx, has);
}

// Element.prototype.getAttributeNames()
GCValue js_element_get_attribute_names(JSContextHandle ctx, GCValue this_val,
                                       int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue arr = JS_NewArray(ctx);
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) return arr;
    int count = node.attribute_count();
    const DOMAttribute *attrs = node.attributes();
    for (int i = 0; i < count && attrs; i++) {
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, attrs[i].name));
    }
    return arr;
}

// ============================================================================
// Element.prototype.matches / closest
// ============================================================================

GCValue js_element_matches(JSContextHandle ctx, GCValue this_val,
                           int argc, GCValue *argv) {
    if (argc < 1) return JS_FALSE;
    const char *selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return JS_FALSE;
    return JS_NewBool(ctx, matches_selector(ctx, this_val, selector));
}

GCValue js_element_closest(JSContextHandle ctx, GCValue this_val,
                           int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    const char *selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return JS_NULL;

    GCValue cur = this_val;
    while (!JS_IsNull(cur) && JS_IsObject(cur)) {
        if (matches_selector(ctx, cur, selector)) {
            return cur;
        }
        DOMNodeHandle node = get_dom_node(ctx, cur);
        if (!node.valid()) break;
        cur = node.parent_node();
    }
    return JS_NULL;
}

// ============================================================================
// HTMLSlotElement helpers
// ============================================================================

static GCValue slot_find_host(JSContextHandle ctx, GCValue slot) {
    GCValue parent = JS_GetPropertyStr(ctx, slot, "parentNode");
    if (!JS_IsNull(parent) && !JS_IsUndefined(parent) && JS_IsObject(parent)) {
        GCValue host = JS_GetPropertyStr(ctx, parent, "host");
        if (!JS_IsNull(host) && !JS_IsUndefined(host) && JS_IsObject(host)) {
            return host;
        }
    }
    return JS_NULL;
}

static const char *slot_get_name(JSContextHandle ctx, GCValue slot) {
    GCValue name_val = JS_GetPropertyStr(ctx, slot, "name");
    if (JS_IsUndefined(name_val) || JS_IsNull(name_val)) {
        DOMNodeHandle node = get_dom_node(ctx, slot);
        if (node.valid()) {
            const char *n = node.get_attribute("name");
            if (n) return n;
        }
        return "";
    }
    const char *name = JS_ToCString(ctx, name_val);
    return name ? name : "";
}

static void slot_collect_assigned(JSContextHandle ctx, GCValue host,
                                  const char *slot_name, bool elements_only,
                                  bool flatten, GCValue out);

static GCValue slot_assigned_nodes_impl(JSContextHandle ctx, GCValue slot, bool flatten);

static void slot_collect_assigned(JSContextHandle ctx, GCValue host,
                                  const char *slot_name, bool elements_only,
                                  bool flatten, GCValue out) {
    GCValue child_nodes = JS_GetPropertyStr(ctx, host, "childNodes");
    if (!JS_IsArray(ctx, child_nodes)) return;
    GCValue len_val = JS_GetPropertyStr(ctx, child_nodes, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, len_val);

    uint32_t idx = 0;
    GCValue olen = JS_GetPropertyStr(ctx, out, "length");
    JS_ToUint32(ctx, &idx, olen);

    for (uint32_t i = 0; i < len; i++) {
        GCValue child = JS_GetPropertyUint32(ctx, child_nodes, i);
        if (JS_IsNull(child) || JS_IsUndefined(child)) continue;

        const char *child_slot = "";
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid()) {
            const char *s = child_node.get_attribute("slot");
            if (s) child_slot = s;
        }
        bool matches = (slot_name[0] == '\0' && child_slot[0] == '\0') ||
                       (slot_name[0] != '\0' && strcmp(child_slot, slot_name) == 0);
        if (!matches) continue;

        if (elements_only) {
            GCValue nt = JS_GetPropertyStr(ctx, child, "nodeType");
            int32_t type = 0;
            JS_ToInt32(ctx, &type, nt);
            if (type != 1) continue;
        }

        if (flatten) {
            GCValue tag = JS_GetPropertyStr(ctx, child, "tagName");
            const char *tagc = JS_ToCString(ctx, tag);
            if (tagc && strcasecmp(tagc, "slot") == 0) {
                GCValue nested = slot_assigned_nodes_impl(ctx, child, true);
                if (JS_IsArray(ctx, nested)) {
                    GCValue nlen = JS_GetPropertyStr(ctx, nested, "length");
                    uint32_t nl = 0;
                    JS_ToUint32(ctx, &nl, nlen);
                    for (uint32_t j = 0; j < nl; j++) {
                        GCValue nchild = JS_GetPropertyUint32(ctx, nested, j);
                        JS_SetPropertyUint32(ctx, out, idx++, nchild);
                    }
                }
                continue;
            }
        }

        JS_SetPropertyUint32(ctx, out, idx++, child);
    }
}

static GCValue slot_assigned_nodes_impl(JSContextHandle ctx, GCValue slot, bool flatten) {
    GCValue result = JS_NewArray(ctx);
    const char *slot_name = slot_get_name(ctx, slot);
    GCValue host = slot_find_host(ctx, slot);

    if (!JS_IsNull(host)) {
        slot_collect_assigned(ctx, host, slot_name, false, flatten, result);
    }

    GCValue len_val = JS_GetPropertyStr(ctx, result, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, len_val);
    if (len == 0) {
        GCValue own = JS_GetPropertyStr(ctx, slot, "childNodes");
        if (JS_IsArray(ctx, own)) {
            GCValue olen = JS_GetPropertyStr(ctx, own, "length");
            uint32_t ol = 0;
            JS_ToUint32(ctx, &ol, olen);
            for (uint32_t i = 0; i < ol; i++) {
                GCValue child = JS_GetPropertyUint32(ctx, own, i);
                JS_SetPropertyUint32(ctx, result, i, child);
            }
        }
    }
    return result;
}

GCValue js_slot_assigned_nodes(JSContextHandle ctx, GCValue this_val,
                               int argc, GCValue *argv) {
    bool flatten = false;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        GCValue v = JS_GetPropertyStr(ctx, argv[0], "flatten");
        flatten = JS_ToBool(ctx, v);
    }
    return slot_assigned_nodes_impl(ctx, this_val, flatten);
}

GCValue js_slot_assigned_elements(JSContextHandle ctx, GCValue this_val,
                                  int argc, GCValue *argv) {
    bool flatten = false;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        GCValue v = JS_GetPropertyStr(ctx, argv[0], "flatten");
        flatten = JS_ToBool(ctx, v);
    }

    GCValue result = JS_NewArray(ctx);
    const char *slot_name = slot_get_name(ctx, this_val);
    GCValue host = slot_find_host(ctx, this_val);

    if (!JS_IsNull(host)) {
        slot_collect_assigned(ctx, host, slot_name, true, flatten, result);
    }

    GCValue len_val = JS_GetPropertyStr(ctx, result, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, len_val);
    if (len == 0) {
        GCValue own = JS_GetPropertyStr(ctx, this_val, "childNodes");
        if (JS_IsArray(ctx, own)) {
            GCValue olen = JS_GetPropertyStr(ctx, own, "length");
            uint32_t ol = 0;
            JS_ToUint32(ctx, &ol, olen);
            uint32_t idx = 0;
            for (uint32_t i = 0; i < ol; i++) {
                GCValue child = JS_GetPropertyUint32(ctx, own, i);
                GCValue nt = JS_GetPropertyStr(ctx, child, "nodeType");
                int32_t type = 0;
                JS_ToInt32(ctx, &type, nt);
                if (type == 1) {
                    JS_SetPropertyUint32(ctx, result, idx++, child);
                }
            }
        }
    }
    return result;
}

GCValue js_slot_get_name(JSContextHandle ctx, GCValue this_val,
                         int argc, GCValue *argv) {
    (void)argc; (void)argv;
    const char *name = "";
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (node.valid()) {
        const char *n = node.get_attribute("name");
        if (n) name = n;
    }
    return JS_NewString(ctx, name);
}

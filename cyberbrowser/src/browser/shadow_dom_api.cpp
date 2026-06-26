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


GCValue js_focus_event_get_relatedTarget_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_focus_event_get_relatedTarget(ctx, this_val);
}

GCValue js_shadow_root_get_host(JSContextHandle ctx, GCValue this_val) {
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) return JS_ThrowTypeError(ctx, "Invalid ShadowRoot");
    return sr.host();
}

GCValue js_shadow_root_get_host_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_shadow_root_get_host(ctx, this_val);
}

GCValue js_shadow_root_get_mode(JSContextHandle ctx, GCValue this_val) {
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) return JS_ThrowTypeError(ctx, "Invalid ShadowRoot");
    return JS_NewString(ctx, sr.mode());
}

GCValue js_shadow_root_get_mode_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_shadow_root_get_mode(ctx, this_val);
}

GCValue js_shadow_root_get_innerHTML(JSContextHandle ctx, GCValue this_val) {
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) return JS_ThrowTypeError(ctx, "Invalid ShadowRoot");
    return sr.innerHTML();
}

GCValue js_shadow_root_set_innerHTML(JSContextHandle ctx, GCValue this_val, GCValue val) {
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) return JS_ThrowTypeError(ctx, "Invalid ShadowRoot");

    // Keep the raw string property available for code that reads it, and also
    // parse the markup into real shadow DOM nodes so layout/rendering can see
    // content stamped by Polymer connectedCallbacks.
    sr.set_innerHTML(val);
    const char *html = JS_ToCString(ctx, val);
    if (html) {
        html_shadow_root_set_inner_html(ctx, this_val, html);
    }
    return JS_UNDEFINED;
}

// Forward declarations for ShadowRoot DOM tree functions
GCValue js_shadow_root_append_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_shadow_root_remove_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_shadow_root_insert_before(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_shadow_root_get_first_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_shadow_root_get_last_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_shadow_root_get_child_nodes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

// Recursive querySelector helper for ShadowRoot - uses same selector matching as regular DOM
GCValue query_selector_shadow_recursive(JSContextHandle ctx, GCValue elem, const char* selector) {
    if (JS_IsNull(elem)) return JS_NULL;
    
    // Check this element using the standard matches_selector helper
    DOMNodeHandle node = get_dom_node(ctx, elem);
    if (node.valid() && node.node_type() == DOM_NODE_TYPE_ELEMENT) {
        if (matches_selector(ctx, elem, selector)) {
            return elem;
        }
    }
    
    // Check children recursively
    DOMNodeHandle elem_node = get_dom_node(ctx, elem);
    if (elem_node.valid()) {
        GCValue child = elem_node.first_child();
        while (!JS_IsNull(child)) {
            GCValue result = query_selector_shadow_recursive(ctx, child, selector);
            if (!JS_IsNull(result)) {
                return result;
            }
            DOMNodeHandle child_node = get_dom_node(ctx, child);
            if (!child_node.valid()) break;
            child = child_node.next_sibling();
        }
    }
    
    return JS_NULL;
}

void query_selector_all_shadow_recursive(JSContextHandle ctx, GCValue elem, const char* selector, GCValue result_arr, int* idx) {
    if (JS_IsNull(elem)) return;
    
    // Check this element
    DOMNodeHandle node = get_dom_node(ctx, elem);
    if (node.valid() && node.node_type() == DOM_NODE_TYPE_ELEMENT) {
        if (matches_selector(ctx, elem, selector)) {
            JS_SetPropertyUint32(ctx, result_arr, (*idx)++, elem);
        }
    }
    
    // Check children recursively
    DOMNodeHandle elem_node = get_dom_node(ctx, elem);
    if (elem_node.valid()) {
        GCValue child = elem_node.first_child();
        while (!JS_IsNull(child)) {
            query_selector_all_shadow_recursive(ctx, child, selector, result_arr, idx);
            DOMNodeHandle child_node = get_dom_node(ctx, child);
            if (!child_node.valid()) break;
            child = child_node.next_sibling();
        }
    }
}

GCValue js_shadow_root_querySelector(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    
    const char* selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return JS_NULL;
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) return JS_NULL;
    
    // Search through all children and their descendants
    GCValue child = sr.first_child();
    while (!JS_IsNull(child)) {
        GCValue result = query_selector_shadow_recursive(ctx, child, selector);
        if (!JS_IsNull(result)) {
            return result;
        }
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }
    
    return JS_NULL;
}

GCValue js_shadow_root_querySelectorAll(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue result = JS_NewArray(ctx);
    if (argc < 1) return result;
    
    const char* selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return result;
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) return result;
    
    int idx = 0;
    GCValue child = sr.first_child();
    while (!JS_IsNull(child)) {
        query_selector_all_shadow_recursive(ctx, child, selector, result, &idx);
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }
    
    return result;
}

GCValue js_shadow_root_getElementById(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    
    const char* id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_NULL;
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) return JS_NULL;
    
    // Search through children for element with matching id
    GCValue child = sr.first_child();
    while (!JS_IsNull(child)) {
        // Check if this child has the id
        GCValue id_val = JS_GetPropertyStr(ctx, child, "id");
        const char* child_id = JS_ToCString(ctx, id_val);
        if (child_id && strcmp(child_id, id) == 0) {
            return child;
        }
        
        // Check child nodes recursively
        DOMNodeHandle child_node = DOMNodeHandle::from_object_check(ctx, child);
        if (child_node.valid()) {
            GCValue next = child_node.next_sibling();
            child = next;
        } else {
            break;
        }
    }
    
    return JS_NULL;
}

// ShadowRoot tree manipulation - appendChild
GCValue js_shadow_root_append_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "appendChild: invalid argument");
    }
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) {
        return JS_ThrowTypeError(ctx, "appendChild: invalid ShadowRoot");
    }
    
    GCValue child = argv[0];
    
    // Get or create DOM data for child
    DOMNodeHandle child_node = get_or_create_dom_node(ctx, child, DOM_NODE_TYPE_ELEMENT, "");
    if (!child_node.valid()) {
        return JS_ThrowTypeError(ctx, "appendChild: failed to create DOM node");
    }
    
    // Remove child from its current parent if any
    GCValue old_parent_val = child_node.parent_node();
    if (!JS_IsNull(old_parent_val)) {
        // Check if old parent is a ShadowRoot
        ShadowRootDataHandle old_parent_sr = ShadowRootDataHandle::from_object_check(ctx, old_parent_val);
        if (old_parent_sr.valid()) {
            // Remove from ShadowRoot
            GCValue remove_args[1] = { child };
            js_shadow_root_remove_child(ctx, old_parent_val, 1, remove_args);
        } else {
            // Remove from regular DOM node
            GCValue remove_args[1] = { child };
            js_node_removeChild_real(ctx, old_parent_val, 1, remove_args);
        }
    }
    
    // Set child's parent to this shadow root
    child_node.set_parent_node(this_val);
    
    // Link child into shadow root's child list
    GCValue first_child = sr.first_child();
    GCValue last_child = sr.last_child();
    
    if (JS_IsNull(first_child)) {
        // First child
        sr.set_first_child(child);
        sr.set_last_child(child);
    } else {
        // Append to end
        DOMNodeHandle last_node = get_dom_node(ctx, last_child);
        if (last_node.valid()) {
            last_node.set_next_sibling(child);
        }
        child_node.set_previous_sibling(last_child);
        child_node.set_next_sibling(JS_NULL);
        sr.set_last_child(child);
    }
    
    sr.increment_child_count();
    
    return child;
}

// ShadowRoot removeChild
GCValue js_shadow_root_remove_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "removeChild: invalid argument");
    }
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) {
        return JS_ThrowTypeError(ctx, "removeChild: invalid ShadowRoot");
    }
    
    GCValue child = argv[0];
    
    // Get DOM data for child
    DOMNodeHandle child_node = get_dom_node(ctx, child);
    if (!child_node.valid()) {
        return child;
    }
    
    // Verify child is actually a child of this shadow root
    GCValue child_parent = child_node.parent_node();
    if (!JS_StrictEq(ctx, child_parent, this_val)) {
        return throw_dom_exception(ctx, "NotFoundError", "Node is not a child of this shadow root");
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
        // Child was first child, update shadow root's firstChild
        sr.set_first_child(next_sibling);
    }
    
    // Unlink from next sibling
    if (!JS_IsNull(next_sibling)) {
        DOMNodeHandle next_node = get_dom_node(ctx, next_sibling);
        if (next_node.valid()) {
            next_node.set_previous_sibling(prev_sibling);
        }
    } else {
        // Child was last child, update shadow root's lastChild
        sr.set_last_child(prev_sibling);
    }
    
    // Clear child's references
    child_node.set_parent_node(JS_NULL);
    child_node.set_previous_sibling(JS_NULL);
    child_node.set_next_sibling(JS_NULL);
    
    sr.decrement_child_count();
    
    return child;
}

// ShadowRoot insertBefore
GCValue js_shadow_root_insert_before(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "insertBefore: invalid arguments");
    }
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) {
        return JS_ThrowTypeError(ctx, "insertBefore: invalid ShadowRoot");
    }
    
    GCValue new_child = argv[0];
    GCValue ref_child = argv[1];  // Can be null (append at end)
    
    // Get or create DOM data for new child
    DOMNodeHandle new_node = get_or_create_dom_node(ctx, new_child, DOM_NODE_TYPE_ELEMENT, "");
    if (!new_node.valid()) {
        return JS_ThrowTypeError(ctx, "insertBefore: failed to create DOM node");
    }
    
    // Remove from current parent if any
    GCValue old_parent_val = new_node.parent_node();
    if (!JS_IsNull(old_parent_val)) {
        if (JS_StrictEq(ctx, old_parent_val, this_val)) {
            // Already in this shadow root, just remove first
            GCValue remove_args[1] = { new_child };
            js_shadow_root_remove_child(ctx, this_val, 1, remove_args);
        } else {
            // Remove from old parent
            ShadowRootDataHandle old_parent_sr = ShadowRootDataHandle::from_object_check(ctx, old_parent_val);
            if (old_parent_sr.valid()) {
                GCValue remove_args[1] = { new_child };
                js_shadow_root_remove_child(ctx, old_parent_val, 1, remove_args);
            } else {
                GCValue remove_args[1] = { new_child };
                js_node_removeChild_real(ctx, old_parent_val, 1, remove_args);
            }
        }
    }
    
    // If refChild is null, append at end
    if (JS_IsNull(ref_child)) {
        return js_shadow_root_append_child(ctx, this_val, 1, argv);
    }
    
    // Get ref child's DOM data
    DOMNodeHandle ref_node = get_dom_node(ctx, ref_child);
    if (!ref_node.valid()) {
        return throw_dom_exception(ctx, "NotFoundError", "Reference node not found");
    }
    
    // Verify ref_child is a child of this shadow root
    GCValue ref_parent = ref_node.parent_node();
    if (!JS_StrictEq(ctx, ref_parent, this_val)) {
        return throw_dom_exception(ctx, "NotFoundError", "Reference node is not a child of this shadow root");
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
        sr.set_first_child(new_child);
    }
    
    sr.increment_child_count();
    
    return new_child;
}

// ShadowRoot firstChild getter
GCValue js_shadow_root_get_first_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) {
        return JS_NULL;
    }
    GCValue first = sr.first_child();
    return JS_IsNull(first) ? JS_NULL : first;
}

// ShadowRoot lastChild getter
GCValue js_shadow_root_get_last_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) {
        return JS_NULL;
    }
    GCValue last = sr.last_child();
    return JS_IsNull(last) ? JS_NULL : last;
}

// ShadowRoot childNodes getter
GCValue js_shadow_root_get_child_nodes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue arr = JS_NewArray(ctx);
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) {
        return arr;
    }
    
    int idx = 0;
    GCValue child = sr.first_child();
    while (!JS_IsNull(child)) {
        JS_SetPropertyUint32(ctx, arr, idx++, child);
        
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }
    
    return arr;
}

// ShadowRoot children getter (elements only)
GCValue js_shadow_root_get_children(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue arr = JS_NewArray(ctx);
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) {
        return arr;
    }
    
    int idx = 0;
    GCValue child = sr.first_child();
    while (!JS_IsNull(child)) {
        // Only include element nodes
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            JS_SetPropertyUint32(ctx, arr, idx++, child);
        }
        
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }
    
    return arr;
}

// ShadowRoot childElementCount getter
GCValue js_shadow_root_get_child_element_count(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) {
        return JS_NewInt32(ctx, 0);
    }
    
    int count = 0;
    GCValue child = sr.first_child();
    while (!JS_IsNull(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            count++;
        }
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }
    
    return JS_NewInt32(ctx, count);
}

// ShadowRoot contains
GCValue js_shadow_root_contains(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_FALSE;
    }
    
    GCValue other = argv[0];
    
    // Check if same node (ShadowRoot itself)
    if (JS_StrictEq(ctx, this_val, other)) {
        return JS_FALSE;  // ShadowRoot cannot contain itself
    }
    
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) {
        return JS_FALSE;
    }
    
    // Walk through children
    GCValue child = sr.first_child();
    while (!JS_IsNull(child)) {
        // Check if this is the node
        if (JS_StrictEq(ctx, child, other)) {
            return JS_TRUE;
        }
        
        // Check children recursively using Node.contains
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid()) {
            GCValue contains_args[1] = { other };
            GCValue result = js_node_contains_real(ctx, child, 1, contains_args);
            if (JS_ToBool(ctx, result)) {
                return JS_TRUE;
            }
            child = child_node.next_sibling();
        } else {
            break;
        }
    }
    
    return JS_FALSE;
}

static void shadow_root_collect_by_tag(JSContextHandle ctx, GCValue node, const char *tag_name,
                                        GCValue arr, uint32_t *idx) {
    if (*idx >= 10000) return;
    DOMNodeHandle n = get_dom_node(ctx, node);
    if (!n.valid() || n.node_type() != DOM_NODE_TYPE_ELEMENT) return;
    const char *name = n.node_name();
    if (name && strcasecmp(name, tag_name) == 0) {
        JS_SetPropertyUint32(ctx, arr, (*idx)++, node);
    }
    GCValue child = n.first_child();
    while (!JS_IsNull(child) && JS_IsObject(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (!child_node.valid()) break;
        shadow_root_collect_by_tag(ctx, child, tag_name, arr, idx);
        child = child_node.next_sibling();
    }
}

GCValue js_shadow_root_get_owner_document_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) return JS_NULL;
    GCValue host = sr.host();
    if (JS_IsNull(host) || JS_IsUndefined(host) || !JS_IsObject(host)) return JS_NULL;
    return JS_GetPropertyStr(ctx, host, "ownerDocument");
}

GCValue js_shadow_root_get_parent_node_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    (void)this_val;
    return JS_NULL;
}

GCValue js_shadow_root_get_elements_by_tag_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NewArray(ctx);
    const char *tag_name = JS_ToCString(ctx, argv[0]);
    if (!tag_name) return JS_NewArray(ctx);

    GCValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;

    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object_check(ctx, this_val);
    if (!sr.valid()) return arr;
    GCValue child = sr.first_child();
    while (!JS_IsNull(child) && JS_IsObject(child)) {
        shadow_root_collect_by_tag(ctx, child, tag_name, arr, &idx);
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }
    return arr;
}

GCValue js_shadow_root_get_adopted_style_sheets(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue sheets = JS_GetPropertyStr(ctx, this_val, "__adoptedStyleSheets");
    if (JS_IsUndefined(sheets) || JS_IsNull(sheets) || !JS_IsArray(ctx, sheets)) {
        sheets = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, this_val, "__adoptedStyleSheets", sheets);
    }
    return sheets;
}

GCValue js_shadow_root_set_adopted_style_sheets(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "adoptedStyleSheets setter requires an array");
    JS_SetPropertyStr(ctx, this_val, "__adoptedStyleSheets", argv[0]);

    // Collect CSS text from all adopted sheets and inject it as a <style>
    // element in document.head so the layout engine can parse the rules.
    GCValue sheets = argv[0];
    size_t total_len = 1;
    if (JS_IsArray(ctx, sheets)) {
        GCValue len_val = JS_GetPropertyStr(ctx, sheets, "length");
        uint32_t n = 0;
        JS_ToUint32(ctx, &n, len_val);
        for (uint32_t i = 0; i < n; i++) {
            GCValue sheet = JS_GetPropertyUint32(ctx, sheets, i);
            GCValue text = js_css_style_sheet_get_css_text(ctx, sheet);
            const char *s = JS_ToCString(ctx, text);
            if (s) total_len += strlen(s) + 2;
        }
    }
    char *css = (char*)malloc(total_len);
    if (!css) return JS_UNDEFINED;
    css[0] = '\0';
    if (JS_IsArray(ctx, sheets)) {
        GCValue len_val = JS_GetPropertyStr(ctx, sheets, "length");
        uint32_t n = 0;
        JS_ToUint32(ctx, &n, len_val);
        for (uint32_t i = 0; i < n; i++) {
            GCValue sheet = JS_GetPropertyUint32(ctx, sheets, i);
            GCValue text = js_css_style_sheet_get_css_text(ctx, sheet);
            const char *s = JS_ToCString(ctx, text);
            if (s && s[0]) {
                strcat(css, s);
                strcat(css, "\n");
            }
        }
    }

    GCValue host = js_shadow_root_get_host(ctx, this_val);
    GCValue doc = JS_GetPropertyStr(ctx, host, "ownerDocument");
    if (JS_IsObject(doc)) {
        GCValue head = JS_GetPropertyStr(ctx, doc, "head");
        if (JS_IsObject(head)) {
            // Remove any previously injected style element for this shadow root.
            GCValue old_style = JS_GetPropertyStr(ctx, this_val, "__adoptedStyleElement");
            if (JS_IsObject(old_style)) {
                GCValue remove_fn = JS_GetPropertyStr(ctx, head, "removeChild");
                if (JS_IsFunction(ctx, remove_fn)) {
                    GCValue remove_args[1] = { old_style };
                    JS_Call(ctx, remove_fn, head, 1, remove_args);
                }
                JS_SetPropertyStr(ctx, this_val, "__adoptedStyleElement", JS_UNDEFINED);
            }

            if (css[0]) {
                GCValue create_elem = JS_GetPropertyStr(ctx, doc, "createElement");
                GCValue style_tag = JS_NewString(ctx, "style");
                GCValue create_args[1] = { style_tag };
                GCValue style = JS_Call(ctx, create_elem, doc, 1, create_args);
                if (JS_IsObject(style)) {
                    JS_SetPropertyStr(ctx, style, "textContent", JS_NewString(ctx, css));
                    JS_SetPropertyStr(ctx, style, "__cyber_adopted_style", JS_TRUE);
                    GCValue append_fn = JS_GetPropertyStr(ctx, head, "appendChild");
                    GCValue append_args[1] = { style };
                    JS_Call(ctx, append_fn, head, 1, append_args);
                    JS_SetPropertyStr(ctx, this_val, "__adoptedStyleElement", style);
                }
            }
        }
    }
    free(css);

    dom_request_layout();
    return JS_UNDEFINED;
}

GCValue js_shadow_root_get_style_sheets(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_shadow_root_get_adopted_style_sheets(ctx, this_val, 0, NULL);
}

const JSCFunctionListEntry js_shadow_root_proto_funcs[] = {
    JS_CGETSET_DEF("host", js_shadow_root_get_host, NULL),
    JS_CGETSET_DEF("mode", js_shadow_root_get_mode, NULL),
    JS_CGETSET_DEF("innerHTML", js_shadow_root_get_innerHTML, js_shadow_root_set_innerHTML),
    JS_CFUNC_DEF("querySelector", 1, js_shadow_root_querySelector),
    JS_CFUNC_DEF("querySelectorAll", 1, js_shadow_root_querySelectorAll),
    JS_CFUNC_DEF("getElementById", 1, js_shadow_root_getElementById),
    JS_CFUNC_DEF("appendChild", 1, js_shadow_root_append_child),
    JS_CFUNC_DEF("removeChild", 1, js_shadow_root_remove_child),
    JS_CFUNC_DEF("insertBefore", 2, js_shadow_root_insert_before),
    JS_CFUNC_DEF("contains", 1, js_shadow_root_contains),
    JS_CFUNC_DEF("getElementsByTagName", 1, js_shadow_root_get_elements_by_tag_name),
    JS_CFUNC_DEF("addEventListener", 2, js_dummy_function),
    JS_CFUNC_DEF("removeEventListener", 2, js_dummy_function),
    JS_CFUNC_DEF("dispatchEvent", 1, js_dummy_function_true),
    JS_CGETSET_DEF("ownerDocument", js_shadow_root_get_owner_document_wrapper, NULL),
    JS_CGETSET_DEF("parentNode", js_shadow_root_get_parent_node_wrapper, NULL),
    JS_CGETSET_DEF("parentElement", js_shadow_root_get_parent_node_wrapper, NULL),
    JS_CGETSET_DEF("adoptedStyleSheets", js_shadow_root_get_adopted_style_sheets, js_shadow_root_set_adopted_style_sheets),
    JS_CGETSET_DEF("styleSheets", js_shadow_root_get_style_sheets, NULL),
    JS_PROP_INT32_DEF("nodeType", 11, JS_PROP_ENUMERABLE),  // DOCUMENT_FRAGMENT_NODE
    JS_PROP_STRING_DEF("nodeName", "#document-fragment", JS_PROP_ENUMERABLE),
};
const size_t js_shadow_root_proto_funcs_count = sizeof(js_shadow_root_proto_funcs) / sizeof(js_shadow_root_proto_funcs[0]);

// Element.prototype.attachShadow()
GCValue js_element_attach_shadow(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "attachShadow requires an init object");
    }
    
    // Get mode from init object
    GCValue mode_val = JS_GetPropertyStr(ctx, argv[0], "mode");
    const char *mode = JS_ToCString(ctx, mode_val);
    if (!mode) mode = "closed";
    
    // Create ShadowRoot instance
    ShadowRootDataHandle sr = ShadowRootDataHandle::create(ctx, this_val, mode);
    if (!sr.valid()) {
        return JS_ThrowInternalError(ctx, "attachShadow: failed to create ShadowRoot");
    }
    
    GCValue shadow_root = JS_NewObjectClass(ctx, js_shadow_root_class_id);
    sr.attach_to_object(shadow_root);
    
    // Store shadowRoot reference on the element (internal property __shadowRoot)
    JS_SetPropertyStr(ctx, this_val, "__shadowRoot", shadow_root);

    // Polymer's _attachDom passes a pre-built fragment as `shadyUpgradeFragment`.
    // Move its children into the new shadow root so the composed tree is ready.
    GCValue upgrade_fragment = JS_GetPropertyStr(ctx, argv[0], "shadyUpgradeFragment");
    if (!JS_IsNull(upgrade_fragment) && !JS_IsUndefined(upgrade_fragment) && JS_IsObject(upgrade_fragment)) {
        GCValue fragment_children = JS_GetPropertyStr(ctx, upgrade_fragment, "childNodes");
        if (JS_IsArray(ctx, fragment_children)) {
            GCValue len_val = JS_GetPropertyStr(ctx, fragment_children, "length");
            uint32_t len = 0;
            JS_ToUint32(ctx, &len, len_val);
            // Snapshot the children first so appending them doesn't shift indices.
            GCValue *children = (GCValue*)malloc(sizeof(GCValue) * (len ? len : 1));
            for (uint32_t i = 0; i < len; i++) {
                children[i] = JS_GetPropertyUint32(ctx, fragment_children, i);
            }
            for (uint32_t i = 0; i < len; i++) {
                GCValue append_args[1] = { children[i] };
                js_shadow_root_append_child(ctx, shadow_root, 1, append_args);
            }
            free(children);
        }
    }

    return shadow_root;
}

// Element.prototype.shadowRoot getter
GCValue js_element_get_shadow_root(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue shadow = JS_GetPropertyStr(ctx, this_val, "__shadowRoot");
    if (JS_IsUndefined(shadow)) {

        return JS_NULL;
    }
    
    // Check if mode is "open" - if closed, return null
    ShadowRootDataHandle sr = ShadowRootDataHandle::from_object(shadow);
    if (sr.valid() && sr.is_closed()) {
        return JS_NULL;
    }
    
    return shadow;
}


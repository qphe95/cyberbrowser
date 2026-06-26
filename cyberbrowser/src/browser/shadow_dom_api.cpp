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

// ShadowRoot tree operations now delegate to the regular DOM node
// implementation.  A ShadowRoot is backed by a DOMNode of type
// DOCUMENT_FRAGMENT_NODE, so appendChild/removeChild/insertBefore and all
// traversal getters work exactly like a normal fragment parent.

GCValue js_shadow_root_append_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_node_appendChild_real(ctx, this_val, argc, argv);
}

GCValue js_shadow_root_remove_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_node_removeChild_real(ctx, this_val, argc, argv);
}

GCValue js_shadow_root_insert_before(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_node_insertBefore_real(ctx, this_val, argc, argv);
}

GCValue js_shadow_root_get_first_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_node_get_firstChild(ctx, this_val, argc, argv);
}

GCValue js_shadow_root_get_last_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_node_get_lastChild(ctx, this_val, argc, argv);
}

GCValue js_shadow_root_get_child_nodes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_node_get_childNodes(ctx, this_val, argc, argv);
}

GCValue js_shadow_root_get_children(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue arr = JS_NewArray(ctx);
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) return arr;
    int idx = 0;
    GCValue child = node.first_child();
    while (!JS_IsNull(child) && !JS_IsUndefined(child) && JS_IsObject(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            JS_SetPropertyUint32(ctx, arr, idx++, child);
        }
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }
    return arr;
}

GCValue js_shadow_root_get_child_element_count(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    int count = 0;
    DOMNodeHandle node = get_dom_node(ctx, this_val);
    if (!node.valid()) return JS_NewInt32(ctx, 0);
    GCValue child = node.first_child();
    while (!JS_IsNull(child) && !JS_IsUndefined(child) && JS_IsObject(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            count++;
        }
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }
    return JS_NewInt32(ctx, count);
}

GCValue js_shadow_root_contains(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_node_contains_real(ctx, this_val, argc, argv);
}

GCValue js_shadow_root_getElementById(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_NULL;

    DOMNodeHandle root = get_dom_node(ctx, this_val);
    if (!root.valid()) return JS_NULL;

    GCValue child = root.first_child();
    while (!JS_IsNull(child) && !JS_IsUndefined(child) && JS_IsObject(child)) {
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (child_node.valid() && child_node.node_type() == DOM_NODE_TYPE_ELEMENT) {
            GCValue id_val = JS_GetPropertyStr(ctx, child, "id");
            const char *child_id = JS_ToCString(ctx, id_val);
            if (child_id && strcmp(child_id, id) == 0) {
                return child;
            }
            GCValue args[1] = { argv[0] };
            GCValue found = js_shadow_root_getElementById(ctx, child, 1, args);
            if (!JS_IsNull(found) && !JS_IsUndefined(found)) {
                return found;
            }
        }
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }
    return JS_NULL;
}

GCValue js_shadow_root_querySelector(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_element_querySelector_real(ctx, this_val, argc, argv);
}

GCValue js_shadow_root_querySelectorAll(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_element_querySelectorAll_real(ctx, this_val, argc, argv);
}

static void shadow_root_collect_by_tag(JSContextHandle ctx, GCValue node, const char *tag_name,
                                        GCValue arr, uint32_t *idx) {
    if (*idx >= 10000) return;
    DOMNodeHandle n = get_dom_node(ctx, node);
    if (!n.valid() || n.node_type() != DOM_NODE_TYPE_ELEMENT) return;
    const char *name = n.node_name();
    if (name && (strcasecmp(name, tag_name) == 0 || strcmp(tag_name, "*") == 0)) {
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

GCValue js_shadow_root_get_elements_by_tag_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue arr = JS_NewArray(ctx);
    if (argc < 1) return arr;
    const char *tag_name = JS_ToCString(ctx, argv[0]);
    if (!tag_name) return arr;

    DOMNodeHandle root = get_dom_node(ctx, this_val);
    if (!root.valid()) return arr;

    uint32_t idx = 0;
    GCValue child = root.first_child();
    while (!JS_IsNull(child) && JS_IsObject(child)) {
        shadow_root_collect_by_tag(ctx, child, tag_name, arr, &idx);
        DOMNodeHandle child_node = get_dom_node(ctx, child);
        if (!child_node.valid()) break;
        child = child_node.next_sibling();
    }
    return arr;
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
    JS_CGETSET_DEF("firstChild", js_shadow_root_get_first_child, NULL),
    JS_CGETSET_DEF("lastChild", js_shadow_root_get_last_child, NULL),
    JS_CGETSET_DEF("childNodes", js_shadow_root_get_child_nodes, NULL),
    JS_CGETSET_DEF("children", js_shadow_root_get_children, NULL),
    JS_CGETSET_DEF("childElementCount", js_shadow_root_get_child_element_count, NULL),
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

    // Create ShadowRoot instance.  This also allocates a regular DOMNode that
    // will store the shadow tree's children, so ShadowRoot behaves like any
    // other fragment parent for appendChild/querySelector/traversal.
    ShadowRootDataHandle sr = ShadowRootDataHandle::create(ctx, this_val, mode);
    if (!sr.valid()) {
        return JS_ThrowInternalError(ctx, "attachShadow: failed to create ShadowRoot");
    }

    GCValue shadow_root = JS_NewObjectClass(ctx, js_shadow_root_class_id);
    sr.attach_to_object(shadow_root);

    // Wire the backing DOMNode to the new ShadowRoot JS object.
    GCHandle dn = sr.dom_node();
    if (dn != GC_HANDLE_NULL) {
        DOMNodeHandle dom_node(dn);
        if (dom_node.valid()) {
            dom_node.set_js_object(shadow_root);
            GCValue owner_doc = JS_GetPropertyStr(ctx, this_val, "ownerDocument");
            if (!JS_IsUndefined(owner_doc) && !JS_IsNull(owner_doc)) {
                dom_node.set_owner_document(owner_doc);
            }
        }
    }

    // Keep the host accessible to DOM connectivity checks (e.g. isConnected).
    JS_SetPropertyStr(ctx, shadow_root, "host", this_val);

    // Store shadowRoot reference on the element (internal property __shadowRoot
    // and the native DOMNode shadow_root field).
    JS_SetPropertyStr(ctx, this_val, "__shadowRoot", shadow_root);
    DOMNodeHandle host_node = get_dom_node(ctx, this_val);
    if (host_node.valid()) {
        host_node.set_shadow_root(shadow_root);
    }

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
                js_node_appendChild_real(ctx, shadow_root, 1, append_args);
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

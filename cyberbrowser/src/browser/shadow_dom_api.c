/* Auto-generated split from browser_api_impl.c */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_types.h"
#include "browser_api_impl_handle_helpers.h"
#include "browser_api_impl_internal.h"
#include "html_dom.h"
#include "dom_js.h"
#include "dom_query.h"
#include "css_parser.h"
#include "gc_value_helpers.h"
#include "platform.h"


GCValue js_focus_event_get_relatedTarget_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_focus_event_get_relatedTarget(ctx, this_val);
}

GCValue js_shadow_root_get_host(JSContextHandle ctx, GCValue this_val) {
    ShadowRootDataHandle sr = JS_GetOpaqueHandle2(ctx, this_val, js_shadow_root_class_id);
    if (SAFE_IS_NULL(sr)) return JS_ThrowTypeError(ctx, "Invalid ShadowRoot");
    return SAFE_GET_OR(sr, ShadowRootDataHandle, host, JS_NULL);
}

GCValue js_shadow_root_get_host_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_shadow_root_get_host(ctx, this_val);
}

GCValue js_shadow_root_get_mode(JSContextHandle ctx, GCValue this_val) {
    ShadowRootDataHandle sr = JS_GetOpaqueHandle2(ctx, this_val, js_shadow_root_class_id);
    if (SAFE_IS_NULL(sr)) return JS_ThrowTypeError(ctx, "Invalid ShadowRoot");
    return JS_NewString(ctx, SAFE_GET_OR(sr, ShadowRootDataHandle, mode, ""));
}

GCValue js_shadow_root_get_mode_wrapper(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return js_shadow_root_get_mode(ctx, this_val);
}

GCValue js_shadow_root_get_innerHTML(JSContextHandle ctx, GCValue this_val) {
    ShadowRootDataHandle sr = JS_GetOpaqueHandle2(ctx, this_val, js_shadow_root_class_id);
    if (SAFE_IS_NULL(sr)) return JS_ThrowTypeError(ctx, "Invalid ShadowRoot");
    return SAFE_GET_OR(sr, ShadowRootDataHandle, innerHTML, JS_NULL);
}

GCValue js_shadow_root_set_innerHTML(JSContextHandle ctx, GCValue this_val, GCValue val) {
    ShadowRootDataHandle sr = JS_GetOpaqueHandle2(ctx, this_val, js_shadow_root_class_id);
    if (SAFE_IS_NULL(sr)) return JS_ThrowTypeError(ctx, "Invalid ShadowRoot");

    // Keep the raw string property available for code that reads it, and also
    // parse the markup into real shadow DOM nodes so layout/rendering can see
    // content stamped by Polymer connectedCallbacks.
    SAFE_SET(sr, ShadowRootDataHandle, innerHTML, val);
    const char *html = JS_ToCString(ctx, val);
    if (html) {
        html_shadow_root_set_inner_html(ctx, this_val, html);
    }
    return JS_UNDEFINED;
}

// ShadowRoot tree operations now delegate to the regular DOM node
// implementation.  A ShadowRoot is backed by a store fragment (node type
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

GCValue js_shadow_root_get_first_child(JSContextHandle ctx, GCValue this_val) {
    return js_node_get_firstChild(ctx, this_val, 0, NULL);
}

GCValue js_shadow_root_get_last_child(JSContextHandle ctx, GCValue this_val) {
    return js_node_get_lastChild(ctx, this_val, 0, NULL);
}

GCValue js_shadow_root_get_child_nodes(JSContextHandle ctx, GCValue this_val) {
    return js_node_get_childNodes(ctx, this_val, 0, NULL);
}

GCValue js_shadow_root_get_children(JSContextHandle ctx, GCValue this_val) {
    GCValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    for (NodeId c = dom_first_child(dom_node_of(this_val)); c; c = dom_next_sibling(c))
        if (dom_kind(c) == DOM_K_ELEMENT) JS_SetPropertyUint32(ctx, arr, idx++, dom_js(ctx, c));
    return arr;
}

GCValue js_shadow_root_get_child_element_count(JSContextHandle ctx, GCValue this_val) {
    int count = 0;
    for (NodeId c = dom_first_child(dom_node_of(this_val)); c; c = dom_next_sibling(c))
        if (dom_kind(c) == DOM_K_ELEMENT) count++;
    return JS_NewInt32(ctx, count);
}

GCValue js_shadow_root_contains(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_node_contains_real(ctx, this_val, argc, argv);
}

/* First element in the shadow tree (preorder) whose id is `id`. */
GCValue js_shadow_root_getElementById(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_NULL;
    NodeId root = dom_node_of(this_val);
    if (!root) return JS_NULL;
    uint32_t r = dom_slot(root);
    for (uint32_t s = dom_next_in_subtree(r, r); s; s = dom_next_in_subtree(s, r)) {
        if (g_dom.kind[s] != DOM_K_ELEMENT) continue;
        const char *v = dom_attr(dom_id_of(s), "id");
        if (v && strcmp(v, id) == 0) { JS_FreeCString(ctx, id); return dom_js(ctx, dom_id_of(s)); }
    }
    JS_FreeCString(ctx, id);
    return JS_NULL;
}

GCValue js_shadow_root_querySelector(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_element_querySelector_real(ctx, this_val, argc, argv);
}

GCValue js_shadow_root_querySelectorAll(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_element_querySelectorAll_real(ctx, this_val, argc, argv);
}

GCValue js_shadow_root_get_elements_by_tag_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue arr = JS_NewArray(ctx);
    if (argc < 1) return arr;
    size_t len = 0;
    const char *tag_name = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!tag_name) return arr;
    NodeId root = dom_node_of(this_val);
    if (root) {
        DomNodeList l = dom_query_by_tag_in(root, tag_name, len, false);
        for (uint32_t i = 0; i < l.n && i < 10000; i++)
            JS_SetPropertyUint32(ctx, arr, i, dom_js(ctx, l.ids[i]));
        dom_node_list_free(&l);
    }
    JS_FreeCString(ctx, tag_name);
    return arr;
}

GCValue js_shadow_root_get_owner_document_wrapper(JSContextHandle ctx, GCValue this_val) {
    ShadowRootDataHandle sr = JS_GetOpaqueHandle2(ctx, this_val, js_shadow_root_class_id);
    if (SAFE_IS_NULL(sr)) return JS_NULL;
    GCValue host = SAFE_GET_OR(sr, ShadowRootDataHandle, host, JS_NULL);
    if (JS_IsNull(host) || JS_IsUndefined(host) || !JS_IsObject(host)) return JS_NULL;
    return JS_GetPropertyStr(ctx, host, "ownerDocument");
}

GCValue js_shadow_root_get_parent_node_wrapper(JSContextHandle ctx, GCValue this_val) {
    (void)ctx; (void)this_val;
    return JS_NULL;
}

/* Shadow roots that have (or may get) adopted stylesheets, in registration
 * order, kept in window.__cyber_adopting_roots so layout visits just these
 * instead of walking every element for shadow roots. */
static void shadow_root_register_adopting(JSContextHandle ctx, GCValue root) {
    GCValue flag = JS_GetPropertyStr(ctx, root, "__cyber_adopt_reg");
    if (JS_ToBool(ctx, flag)) return;
    JS_SetPropertyStr(ctx, root, "__cyber_adopt_reg", JS_TRUE);
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue roots = JS_GetPropertyStr(ctx, global, "__cyber_adopting_roots");
    if (!JS_IsArray(ctx, roots)) {
        roots = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__cyber_adopting_roots", roots);
    }
    GCValue len_val = JS_GetPropertyStr(ctx, roots, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, len_val);
    JS_SetPropertyUint32(ctx, roots, n, root);
}

GCValue js_shadow_root_get_adopted_style_sheets(JSContextHandle ctx, GCValue this_val) {
    GCValue sheets = JS_GetPropertyStr(ctx, this_val, "__adoptedStyleSheets");
    if (JS_IsUndefined(sheets) || JS_IsNull(sheets) || !JS_IsArray(ctx, sheets)) {
        sheets = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, this_val, "__adoptedStyleSheets", sheets);
        shadow_root_register_adopting(ctx, this_val);   /* it may be pushed to */
    }
    return sheets;
}

GCValue js_shadow_root_set_adopted_style_sheets(JSContextHandle ctx, GCValue this_val, GCValue val) {
    JS_SetPropertyStr(ctx, this_val, "__adoptedStyleSheets", val);
    shadow_root_register_adopting(ctx, this_val);

    // Assign a unique scope class to the host element.  The layout engine uses
    // this class to prefix every selector from these adopted sheets so they only
    // match nodes inside this host's flattened shadow tree.
    GCValue host = js_shadow_root_get_host(ctx, this_val);
    if (JS_IsObject(host)) {
        NodeId host_node = dom_node_of(host);
        if (dom_kind(host_node) == DOM_K_ELEMENT) {
            static int scope_counter = 0;
            const char *sid_attr = dom_attr(host_node, "__cyber_scope_id");
            char sid[32] = {0};
            if (!sid_attr || !sid_attr[0]) {
                snprintf(sid, sizeof(sid), "cs%d", ++scope_counter);
                dom_set_attr_str(host_node, "__cyber_scope_id", sid);
            } else {
                strncpy(sid, sid_attr, sizeof(sid) - 1);
                sid[sizeof(sid) - 1] = '\0';
            }
            char scope_class[48];
            snprintf(scope_class, sizeof(scope_class), "cyber-scope-%s", sid);
            const char *existing = dom_attr(host_node, "class");
            char combined[256];
            if (existing && existing[0]) {
                if (strstr(existing, scope_class) == NULL) {
                    snprintf(combined, sizeof(combined), "%s %s", existing, scope_class);
                    dom_set_attr_str(host_node, "class", combined);
                }
            } else {
                dom_set_attr_str(host_node, "class", scope_class);
            }
        }
    }

    dom_request_relayout();
    return JS_UNDEFINED;
}

GCValue js_shadow_root_get_style_sheets(JSContextHandle ctx, GCValue this_val) {
    return js_shadow_root_get_adopted_style_sheets(ctx, this_val);
}

GCValue js_shadow_root_get_delegates_focus(JSContextHandle ctx, GCValue this_val) {
    ShadowRootDataHandle sr = JS_GetOpaqueHandle2(ctx, this_val, js_shadow_root_class_id);
    if (SAFE_IS_NULL(sr)) return JS_ThrowTypeError(ctx, "Invalid ShadowRoot");
    return JS_NewBool(ctx, SAFE_GET_OR(sr, ShadowRootDataHandle, delegates_focus, false));
}

GCValue js_shadow_root_get_slot_assignment(JSContextHandle ctx, GCValue this_val) {
    ShadowRootDataHandle sr = JS_GetOpaqueHandle2(ctx, this_val, js_shadow_root_class_id);
    if (SAFE_IS_NULL(sr)) return JS_ThrowTypeError(ctx, "Invalid ShadowRoot");
    return JS_NewString(ctx, SAFE_GET_OR(sr, ShadowRootDataHandle, manual_slot_assignment, false) ? "manual" : "named");
}

const JSCFunctionListEntry js_shadow_root_proto_funcs[] = {
    JS_CGETSET_DEF("host", js_shadow_root_get_host, NULL),
    JS_CGETSET_DEF("mode", js_shadow_root_get_mode, NULL),
    JS_CGETSET_DEF("delegatesFocus", js_shadow_root_get_delegates_focus, NULL),
    JS_CGETSET_DEF("slotAssignment", js_shadow_root_get_slot_assignment, NULL),
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

// Spec-valid host local names for Element.prototype.attachShadow().
// Custom element names (containing a hyphen) are always valid.
static bool is_valid_shadow_host_local_name(const char *name) {
    if (!name) return false;
    if (strchr(name, '-') != NULL) return true;
    static const char *allowed[] = {
        "article", "aside", "blockquote", "body", "div", "footer",
        "h1", "h2", "h3", "h4", "h5", "h6", "header", "main",
        "nav", "p", "section", "span", NULL
    };
    for (const char **p = allowed; *p; p++) {
        if (strcasecmp(name, *p) == 0) return true;
    }
    return false;
}

// Element.prototype.attachShadow()
GCValue js_element_attach_shadow(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "attachShadow requires an init object");
    }

    // 1. Validate host restrictions.
    NodeId host_node = dom_node_of(this_val);
    if (dom_kind(host_node) != DOM_K_ELEMENT) {
        return JS_ThrowTypeError(ctx, "attachShadow called on non-element");
    }

    const char *tag_name = dom_node_name(host_node);
    fprintf(stderr, "[ATTACH-SHADOW-ENTER] host=%s\n", tag_name);
    fflush(stderr);

    GCValue existing_shadow = JS_GetPropertyStr(ctx, this_val, "__shadowRoot");
    if (!JS_IsUndefined(existing_shadow) && !JS_IsNull(existing_shadow) && JS_IsObject(existing_shadow)) {
        return JS_ThrowTypeError(ctx, "NotSupportedError: element already has a shadow root");
    }
    if (!is_valid_shadow_host_local_name(tag_name)) {
        return JS_ThrowTypeError(ctx, "NotSupportedError: element does not support shadow root");
    }

    // 2. Validate init options.
    GCValue mode_val = JS_GetPropertyStr(ctx, argv[0], "mode");
    const char *mode = JS_ToCString(ctx, mode_val);
    if (!mode || (strcmp(mode, "open") != 0 && strcmp(mode, "closed") != 0)) {
        return JS_ThrowTypeError(ctx, "attachShadow init.mode must be 'open' or 'closed'");
    }

    GCValue delegates_focus_val = JS_GetPropertyStr(ctx, argv[0], "delegatesFocus");
    bool delegates_focus = JS_ToBool(ctx, delegates_focus_val);

    bool manual_slot_assignment = false;
    GCValue slot_assignment_val = JS_GetPropertyStr(ctx, argv[0], "slotAssignment");
    if (!JS_IsUndefined(slot_assignment_val) && !JS_IsNull(slot_assignment_val)) {
        const char *sa = JS_ToCString(ctx, slot_assignment_val);
        if (sa && (strcmp(sa, "named") == 0 || strcmp(sa, "manual") == 0)) {
            manual_slot_assignment = (strcmp(sa, "manual") == 0);
            JS_FreeCString(ctx, sa);
        } else {
            JS_FreeCString(ctx, sa);
            return JS_ThrowTypeError(ctx, "attachShadow init.slotAssignment must be 'named' or 'manual'");
        }
    }

    // 3. Create ShadowRoot instance.
    ShadowRootDataHandle sr = shadow_root_create(ctx, this_val, mode);
    JS_FreeCString(ctx, mode);
    if (SAFE_IS_NULL(sr)) {
        return JS_ThrowInternalError(ctx, "attachShadow: failed to create ShadowRoot");
    }
    SAFE_SET(sr, ShadowRootDataHandle, delegates_focus, delegates_focus);
    SAFE_SET(sr, ShadowRootDataHandle, manual_slot_assignment, manual_slot_assignment);

    GCValue shadow_root = JS_NewObjectClass(ctx, js_shadow_root_class_id);
    JS_SetOpaqueHandle(shadow_root, sr);
    {
        GCValue global = JS_GetGlobalObject(ctx);
        GCValue sr_ctor = JS_GetPropertyStr(ctx, global, "ShadowRoot");
        GCValue sr_proto = JS_IsObject(sr_ctor) ? JS_GetPropertyStr(ctx, sr_ctor, "prototype") : JS_UNDEFINED;
        if (JS_IsObject(sr_proto)) JS_SetPrototype(ctx, shadow_root, sr_proto);
    }

    // Bind the shadow root object to its store fragment.
    {
        DomWrapHandle wd = SAFE_GET_OR(sr, ShadowRootDataHandle, dom_node, GC_HANDLE_NULL);
        DomWrap *w = wd ? (DomWrap *)gc_deref(wd) : NULL;
        NodeId frag = w ? w->id : DOM_NULL;
        if (dom_valid(frag)) {
            dom_bind(ctx, frag, shadow_root);
            g_dom.owner[dom_slot(frag)] = g_dom.owner[dom_slot(host_node)];
        }
    }

    // Keep the host accessible to DOM connectivity checks (e.g. isConnected).
    JS_SetPropertyStr(ctx, shadow_root, "host", this_val);

    // Store shadowRoot reference on the element.
    JS_SetPropertyStr(ctx, this_val, "__shadowRoot", shadow_root);

    // Polymer's _attachDom passes a pre-built fragment as `shadyUpgradeFragment`.
    // Move its children into the new shadow root so the composed tree is ready.
    GCValue upgrade_fragment = JS_GetPropertyStr(ctx, argv[0], "shadyUpgradeFragment");
    bool has_upgrade = !JS_IsNull(upgrade_fragment) && !JS_IsUndefined(upgrade_fragment) && JS_IsObject(upgrade_fragment);
    if (has_upgrade) {
        GCValue frag_first = js_node_get_firstChild(ctx, upgrade_fragment, 0, NULL);
        GCValue frag_child_nodes = JS_GetPropertyStr(ctx, upgrade_fragment, "childNodes");
        int frag_len = 0;
        if (JS_IsArray(ctx, frag_child_nodes)) {
            JS_ToInt32(ctx, &frag_len, JS_GetPropertyStr(ctx, frag_child_nodes, "length"));
        }
        GCValue first_name_v = JS_GetPropertyStr(ctx, frag_first, "nodeName");
        const char *first_name = JS_IsObject(frag_first) ? JS_ToCString(ctx, first_name_v) : "null";
        fprintf(stderr, "[ATTACH-SHADOW] host=%s fragment_children=%d first=%s\n", tag_name, frag_len, first_name ? first_name : "null");
        JS_FreeCString(ctx, first_name);
        fflush(stderr);
    }
    if (!JS_IsNull(upgrade_fragment) && !JS_IsUndefined(upgrade_fragment) && JS_IsObject(upgrade_fragment)) {
        GCValue fragment_children = JS_GetPropertyStr(ctx, upgrade_fragment, "childNodes");
        if (JS_IsArray(ctx, fragment_children)) {
            GCValue len_val = JS_GetPropertyStr(ctx, fragment_children, "length");
            uint32_t len = 0;
            JS_ToUint32(ctx, &len, len_val);
            GCValue *children = (GCValue*)malloc(sizeof(GCValue) * (len ? len : 1));
            for (uint32_t i = 0; i < len; i++) {
                GC_SET(children[i], JS_GetPropertyUint32(ctx, fragment_children, i));
            }
            for (uint32_t i = 0; i < len; i++) {
                GCValue append_args[1] = { children[i] };
                js_node_appendChild_real(ctx, shadow_root, 1, append_args);
            }
            free(children);
        }
    }

    // Polymer's `ready` runs before `_readyClients`, so `this.root` is still
    // the original stamped fragment.  Make that fragment's Shady tree mirror
    // the children we just moved into our ShadowRoot, so scoped querySelector
    // on the fragment finds the stamped content (e.g. #button).
    if (!JS_IsNull(upgrade_fragment) && !JS_IsUndefined(upgrade_fragment) && JS_IsObject(upgrade_fragment)) {
        GCValue frag_shady = JS_GetPropertyStr(ctx, upgrade_fragment, "__shady");
        if (!JS_IsObject(frag_shady)) {
            frag_shady = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, upgrade_fragment, "__shady", frag_shady);
        }
        GCValue frag_first = js_node_get_firstChild(ctx, shadow_root, 0, NULL);
        GCValue frag_last = js_node_get_lastChild(ctx, shadow_root, 0, NULL);
        JS_SetPropertyStr(ctx, frag_shady, "firstChild", frag_first);
        JS_SetPropertyStr(ctx, frag_shady, "lastChild", frag_last);
        JS_SetPropertyStr(ctx, frag_shady, "parentNode", JS_NULL);
        JS_SetPropertyStr(ctx, frag_shady, "childNodes", JS_UNDEFINED);

        GCValue child = frag_first;
        GCValue prev = JS_NULL;
        while (!JS_IsNull(child) && !JS_IsUndefined(child) && JS_IsObject(child)) {
            GCValue ns = js_node_get_nextSibling(ctx, child, 0, NULL);
            GCValue child_shady = JS_GetPropertyStr(ctx, child, "__shady");
            if (!JS_IsObject(child_shady)) {
                child_shady = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, child, "__shady", child_shady);
            }
            JS_SetPropertyStr(ctx, child_shady, "parentNode", upgrade_fragment);
            JS_SetPropertyStr(ctx, child_shady, "previousSibling", prev);
            JS_SetPropertyStr(ctx, child_shady, "nextSibling", ns);
            JS_SetPropertyStr(ctx, child_shady, "childNodes", JS_UNDEFINED);
            prev = child;
            child = ns;
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
    ShadowRootDataHandle sr = JS_GetOpaqueHandle(shadow, js_shadow_root_class_id);
    if (!SAFE_IS_NULL(sr) && shadow_root_is_closed(sr)) {
        return JS_NULL;
    }
    
    return shadow;
}

/*
 * node_namespaces.c - Node's namespace lookups (lookupNamespaceURI,
 * lookupPrefix, isDefaultNamespace: DOM 4.4 "locate a namespace" and
 * "locate a namespace prefix") and getElementsByTagName(NS) by qualified
 * name and by namespace and local name (DOM 4.2.6).
 *
 * Attributes keep no namespace in the store yet: an attribute named
 * "xmlns" or "xmlns:p" stands for the namespace declaration it is in XML.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_internal.h"
#include "dom_js.h"
#include "dom_store.h"

extern NodeId g_dom_document;

#define XML_NS "http://www.w3.org/XML/1998/namespace"
#define XMLNS_NS "http://www.w3.org/2000/xmlns/"

static NodeId parent_element(NodeId n) {
    NodeId p = dom_parent(n);
    return dom_kind(p) == DOM_K_ELEMENT ? p : DOM_NULL;
}

static NodeId document_element(NodeId d) {
    for (NodeId c = dom_first_child(d); c; c = dom_next_sibling(c))
        if (dom_kind(c) == DOM_K_ELEMENT) return c;
    return DOM_NULL;
}

/* The element whose namespaces a node's lookups see. */
static NodeId lookup_element(NodeId n) {
    switch (dom_kind(n)) {
    case DOM_K_ELEMENT: return n;
    case DOM_K_DOCUMENT: return document_element(n);
    case DOM_K_DOCTYPE:
    case DOM_K_FRAGMENT: return DOM_NULL;
    default: return parent_element(n);
    }
}

/* "locate a namespace" (prefix NULL: the default namespace); NULL for none. */
static const char *locate_namespace(NodeId el, const char *prefix) {
    if (prefix && !strcmp(prefix, "xml")) return XML_NS;
    if (prefix && !strcmp(prefix, "xmlns")) return XMLNS_NS;
    for (; el; el = parent_element(el)) {
        const char *uri = dom_ns_uri(dom_ns(el)), *p = dom_prefix(el);
        if (uri && ((!prefix && !p) || (prefix && p && !strcmp(prefix, p)))) return uri;
        char name[300];
        if (prefix) snprintf(name, sizeof name, "xmlns:%s", prefix);
        const char *decl = dom_get_attr_str(el, prefix ? name : "xmlns");
        if (decl) return decl[0] ? decl : NULL;
    }
    return NULL;
}

/* "locate a namespace prefix"; NULL for none. */
static const char *locate_prefix(NodeId el, const char *ns) {
    for (; el; el = parent_element(el)) {
        const char *uri = dom_ns_uri(dom_ns(el)), *p = dom_prefix(el);
        if (uri && p && !strcmp(uri, ns)) return p;
        for (int i = 0; i < dom_attr_count(el); i++) {
            const DomAttr *a = dom_attr_at(el, i);
            const char *an = css_atom_str(a->name);
            if (an && !strncmp(an, "xmlns:", 6) && a->value.s && !strcmp(a->value.s, ns)) return an + 6;
        }
    }
    return NULL;
}

/* A DOMString? argument: NULL for null or undefined (or the empty string,
 * when empty_is_null). */
static bool nullable_arg(JSContextHandle ctx, int argc, GCValue *argv, bool empty_is_null, const char **out) {
    *out = NULL;
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) return true;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return false;
    *out = empty_is_null && !s[0] ? NULL : s;
    return true;
}

static GCValue string_or_null(JSContextHandle ctx, const char *s) {
    return s ? JS_NewString(ctx, s) : JS_NULL;
}

static GCValue js_lookup_namespace_uri(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    const char *prefix;
    if (!nullable_arg(ctx, argc, argv, true, &prefix)) return JS_EXCEPTION;
    NodeId n = dom_node_of(this_val);
    if (!n) return JS_ThrowTypeError(ctx, "Illegal invocation");
    return string_or_null(ctx, locate_namespace(lookup_element(n), prefix));
}

static GCValue js_lookup_prefix(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    const char *ns;
    if (!nullable_arg(ctx, argc, argv, true, &ns)) return JS_EXCEPTION;
    NodeId n = dom_node_of(this_val);
    if (!n) return JS_ThrowTypeError(ctx, "Illegal invocation");
    return ns ? string_or_null(ctx, locate_prefix(lookup_element(n), ns)) : JS_NULL;
}

static GCValue js_is_default_namespace(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    const char *ns;
    if (!nullable_arg(ctx, argc, argv, true, &ns)) return JS_EXCEPTION;
    NodeId n = dom_node_of(this_val);
    if (!n) return JS_ThrowTypeError(ctx, "Illegal invocation");
    const char *d = locate_namespace(lookup_element(n), NULL);
    return JS_NewBool(ctx, (!d && !ns) || (d && ns && !strcmp(d, ns)));
}

/* ---- getElementsByTagName(NS) ---- */

/* An HTMLCollection of the array's elements (array-like, not an Array). */
static GCValue collection(JSContextHandle ctx, GCValue arr) {
    GCValue ctor = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "HTMLCollection");
    GCValue proto = JS_IsObject(ctor) ? JS_GetPropertyStr(ctx, ctor, "prototype") : JS_UNDEFINED;
    GCValue list = JS_IsObject(proto) ? JS_NewObjectProto(ctx, proto) : JS_NewObject(ctx);
    if (JS_IsException(list)) return list;
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, JS_GetPropertyStr(ctx, arr, "length"));
    for (uint32_t i = 0; i < n; i++)
        JS_DefinePropertyValueUint32(ctx, list, i, JS_GetPropertyUint32(ctx, arr, i), JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx, list, "length", JS_NewUint32(ctx, n), 0);
    return list;
}

/* The root's descendant elements that match, in tree order.  qualified:
 * by qualified name (an HTML element in an HTML document lower-cased);
 * else by namespace (NULL with any_ns: any) and local name ("*": any). */
static GCValue elements_by(JSContextHandle ctx, NodeId root, bool qualified, const char *name, bool any_ns,
                           uint32_t ns) {
    GCValue arr = JS_NewArray(ctx);
    if (!root) return collection(ctx, arr);
    NodeId doc = dom_kind(root) == DOM_K_DOCUMENT ? root : (dom_owner(root) ? dom_owner(root) : g_dom_document);
    bool html_doc = dom_is_html_document(doc), all = !strcmp(name, "*");
    size_t nl = strlen(name);
    char *lower = malloc(nl + 1);
    if (!lower) return JS_ThrowOutOfMemory(ctx);
    for (size_t i = 0; i <= nl; i++) lower[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    uint32_t r = dom_slot(root), k = 0;
    for (uint32_t s = dom_next_in_subtree(r, r); s; s = dom_next_in_subtree(s, r)) {
        if (g_dom.kind[s] != DOM_K_ELEMENT) continue;
        NodeId e = dom_id_of(s);
        bool match;
        if (qualified) {
            if (all) {
                match = true;
            } else {
                const char *p = dom_prefix(e), *l = dom_local_name(e);
                const char *want = html_doc && g_dom.ns[s] == DOM_NS_HTML ? lower : name;
                size_t pl = p ? strlen(p) : 0;
                match = p ? !strncmp(want, p, pl) && want[pl] == ':' && !strcmp(want + pl + 1, l) : !strcmp(want, l);
            }
        } else {
            match = (any_ns || g_dom.ns[s] == ns) && (all || !strcmp(name, dom_local_name(e)));
        }
        if (match) JS_SetPropertyUint32(ctx, arr, k++, dom_js(ctx, e));
    }
    free(lower);
    return collection(ctx, arr);
}

static NodeId search_root(GCValue v) {
    NodeId n = dom_node_of(v);
    int k = dom_kind(n);
    return k == DOM_K_ELEMENT || k == DOM_K_DOCUMENT ? n : DOM_NULL;
}

static GCValue js_get_elements_by_tag_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    NodeId root = search_root(this_val);
    if (!root) return JS_ThrowTypeError(ctx, "Illegal invocation");
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'getElementsByTagName': 1 argument required.");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    return elements_by(ctx, root, true, name, false, 0);
}

static GCValue js_get_elements_by_tag_name_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    NodeId root = search_root(this_val);
    if (!root) return JS_ThrowTypeError(ctx, "Illegal invocation");
    if (argc < 2) return JS_ThrowTypeError(ctx, "Failed to execute 'getElementsByTagNameNS': 2 arguments required.");
    const char *uri;
    if (!nullable_arg(ctx, argc, argv, true, &uri)) return JS_EXCEPTION;
    const char *local = JS_ToCString(ctx, argv[1]);
    if (!local) return JS_EXCEPTION;
    bool any = uri && !strcmp(uri, "*");
    return elements_by(ctx, root, false, local, any, any ? 0 : dom_ns_of(uri, uri ? strlen(uri) : 0));
}

/* ---- installing ---- */

static void method(JSContextHandle ctx, GCValue proto, const char *name, JSCFunction *fn, int length) {
    if (!JS_IsObject(proto)) return;
    JS_DefinePropertyValueStr(ctx, proto, name, JS_NewCFunction(ctx, fn, name, length),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
}

static GCValue proto_of(JSContextHandle ctx, GCValue global, const char *name) {
    GCValue c = JS_GetPropertyStr(ctx, global, name);
    return JS_IsObject(c) ? JS_GetPropertyStr(ctx, c, "prototype") : JS_UNDEFINED;
}

void node_namespaces_install(JSContextHandle ctx, GCValue global) {
    GCValue node = proto_of(ctx, global, "Node");
    method(ctx, node, "lookupNamespaceURI", js_lookup_namespace_uri, 1);
    method(ctx, node, "lookupPrefix", js_lookup_prefix, 1);
    method(ctx, node, "isDefaultNamespace", js_is_default_namespace, 1);
    static const char *const holders[] = { "Document", "Element", "HTMLElement" };
    for (size_t i = 0; i < sizeof holders / sizeof holders[0]; i++) {
        GCValue p = proto_of(ctx, global, holders[i]);
        method(ctx, p, "getElementsByTagName", js_get_elements_by_tag_name, 1);
        method(ctx, p, "getElementsByTagNameNS", js_get_elements_by_tag_name_ns, 2);
    }
    /* (the page's document had its own getElementsByTagName) */
    GCValue page = JS_GetPropertyStr(ctx, global, "document");
    if (JS_IsObject(page)) {
        JSAtom a = JS_NewAtom(ctx, "getElementsByTagName");
        JS_DeleteProperty(ctx, page, a, 0);
        JS_FreeAtom(ctx, a);
    }
}

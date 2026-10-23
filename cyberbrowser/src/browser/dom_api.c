/*
 * DOM API over the node store (PARALLEL_DOM.md §6, §7).
 *
 * Nodes live in the store (dom_store.h); JS objects are lazy wrappers
 * (dom_wrap.h).  Every API takes the NodeId of its wrapper arguments, works
 * on the store, and returns wrappers created on demand with dom_js().
 */
#include "cyber_env.h"
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
#include "css_parser.h"
#include "css_layout.h"
#include "gc_value_helpers.h"
#include "platform.h"
#include "cyber_profile.h"
#include "http_download.h"
#include "url_utils.h"
#include "js_quickjs.h"
#include "dom_store.h"
#include "dom_parse.h"
#include "dom_snapshot.h"
#include "dom_wrap.h"
#include "dom_js.h"
#include "dom_fetch.h"
#include "dom_query.h"

// createElement lives in js_quickjs.c and is used by the outerHTML setter.
extern GCValue js_document_create_element(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
const char *g_cyber_start_url = NULL;

/* Global layout-invalidation flag. DOM mutation functions set this to 1 so the
 * main loop knows the layout is stale. */
volatile int g_dom_needs_layout = 0;
static int g_dom_dynamic_scripts_enabled = 0;
NodeId g_dom_document = DOM_NULL;
/* The root element most recently populated from HTML: the legacy index
 * lookups (css_get_element_by_*) answer for that tree, as the tables they
 * replace were rebuilt by each populate. */
NodeId g_dom_index_root = DOM_NULL;
static NodeId index_doc(void) { return g_dom_document; }

void dom_enable_dynamic_script_loading(void) {
    g_dom_dynamic_scripts_enabled = 1;
}

uint64_t g_dom_layout_requests;

uint64_t dom_change_stamp(void) {
    return g_dom.connected_version +
           JS_watched_write_epoch + g_dom_layout_requests;
}

void dom_request_layout(void) {
    g_dom_needs_layout = 1;
    g_dom_layout_requests++;
}

uint64_t g_dom_relayout_requests;

void dom_request_relayout(void) {
    g_dom_relayout_requests++;
    dom_request_layout();
}

// Real DOM Node Implementation
// ============================================================================

JSClassID js_dom_node_class_id = 0;

GCValue js_node_appendChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_removeChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_insertBefore_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_cloneNode_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_contains_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_getRootNode_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_node_get_ownerDocument(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_querySelector_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_querySelectorAll_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_firstElementChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_lastElementChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_children(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
GCValue js_element_get_childElementCount(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

/* Growable malloc'd list of GCValues. Declare locals with GC_VALUE_VEC_AUTO so
 * the storage is freed when the variable goes out of scope.  The values are
 * rooted while listed: callers run script between pushing and using them
 * (each insertion's connectedCallback), and a concurrent cycle stepping in
 * meanwhile cannot see malloc'd memory -- a detached child's wrapper held
 * only here would be freed. */
typedef struct GCValueVec {
    GCValue *data;
    size_t count;
    size_t capacity;
} GCValueVec;
static void gc_value_vec_push(GCValueVec *v, GCValue val) {
    if (v->count == v->capacity) {
        size_t cap = v->capacity ? v->capacity * 2 : 8;
        GCValue *data = (GCValue *)realloc(v->data, cap * sizeof(GCValue));
        if (!data) abort();
        v->data = data;
        v->capacity = cap;
    }
    GC_SET(v->data[v->count++], val);
    if (GC_VALUE_GET_TAG(val) < 0 && GC_VALUE_GET_HANDLE(val) != GC_HANDLE_NULL)
        gc_add_root(GC_VALUE_GET_HANDLE(val));
}
static void gc_value_vec_free(GCValueVec *v) {
    for (size_t i = 0; i < v->count; i++)
        if (GC_VALUE_GET_TAG(v->data[i]) < 0 && GC_VALUE_GET_HANDLE(v->data[i]) != GC_HANDLE_NULL)
            gc_remove_root(GC_VALUE_GET_HANDLE(v->data[i]));
    free(v->data);
    v->data = NULL;
    v->count = v->capacity = 0;
}
#define GC_VALUE_VEC_AUTO __attribute__((cleanup(gc_value_vec_free)))

/* ============================================================================
 * Names
 * ============================================================================ */

static char **g_upper_names;      /* by atom id */
static uint32_t g_upper_cap;

const char *dom_node_name(NodeId id) {
    switch (dom_kind(id)) {
    case DOM_K_TEXT: return "#text";
    case DOM_K_COMMENT: return "#comment";
    case DOM_K_DOCUMENT: return "#document";
    case DOM_K_FRAGMENT: return "#document-fragment";
    case DOM_K_DOCTYPE: return "html";
    case DOM_K_ELEMENT: break;
    default: return "";
    }
    CssAtom a = dom_tag(id);
    if (a == CSS_ATOM_NONE || a == CSS_ATOM_EMPTY) return "DIV";
    if (a >= g_upper_cap) {
        uint32_t cap = g_upper_cap ? g_upper_cap : 256;
        while (cap <= a) cap *= 2;
        g_upper_names = realloc(g_upper_names, cap * sizeof(char *));
        memset(g_upper_names + g_upper_cap, 0, (cap - g_upper_cap) * sizeof(char *));
        g_upper_cap = cap;
    }
    if (!g_upper_names[a]) {
        const char *s = css_atom_str(a);
        size_t n = strlen(s);
        char *u = malloc(n + 1);
        for (size_t i = 0; i < n; i++) u[i] = (char)toupper((unsigned char)s[i]);
        u[n] = '\0';
        g_upper_names[a] = u;
    }
    return g_upper_names[a];
}

const char *dom_local_name(NodeId id) {
    if (dom_kind(id) != DOM_K_ELEMENT) return "";
    CssAtom a = dom_tag(id);
    if (a == CSS_ATOM_NONE || a == CSS_ATOM_EMPTY) return "div";
    return css_atom_str(a);
}

static inline bool dom_is_element(NodeId n) { return dom_kind(n) == DOM_K_ELEMENT; }
static inline bool dom_tag_is(NodeId n, const char *name) {
    return dom_is_element(n) && strcmp(dom_local_name(n), name) == 0;
}

/* ============================================================================
 * The DOM node class: wrapper objects of store nodes
 * ============================================================================ */

void js_dom_node_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    dom_wrap_finalize(JS_GetOpaqueHandle(val, js_dom_node_class_id), val);
}

void js_dom_node_mark(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func) {
    GCHandle wd = JS_GetOpaqueHandle(val, js_dom_node_class_id);
    if (wd != GC_HANDLE_NULL) dom_wrap_mark(rt, wd, mark_func);
}

JSClassDef js_dom_node_class_def = {
    .class_name = "DOMNode",
    .finalizer = js_dom_node_finalizer,
    .gc_mark   = js_dom_node_mark,
};

static GCValue global_proto(JSContextHandle ctx, const char *ctor_name) {
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue ctor = JS_GetPropertyStr(ctx, global, ctor_name);
    if (!JS_IsObject(ctor)) return JS_UNDEFINED;
    GCValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
    return JS_IsObject(proto) ? proto : JS_UNDEFINED;
}

/* The element's style object: DomWrap.style for store wrappers. */
/* Style object -> element, so a write to el.style marks the element dirty
 * (the render snapshot reuses clean subtrees).  Open addressing on the
 * object's GC handle; a recycled handle only marks some element dirty. */
static GCHandle *g_style_obj;
static NodeId *g_style_owner;
static uint32_t g_style_cap, g_style_n;

static void style_owner_written(GCHandle obj) {
    if (!g_style_cap) return;
    uint32_t h = (obj * 2654435761u) & (g_style_cap - 1);
    while (g_style_obj[h]) {
        if (g_style_obj[h] == obj) {
            if (dom_valid(g_style_owner[h])) dom_mark_dirty(g_style_owner[h], DOMF_DIRTY_ATTR);
            return;
        }
        h = (h + 1) & (g_style_cap - 1);
    }
}

static void style_owner_note(GCValue style, NodeId owner) {
    if (GC_VALUE_GET_TAG(style) >= 0 || !owner) return;
    GCHandle obj = GC_VALUE_GET_HANDLE(style);
    if (obj == GC_HANDLE_NULL) return;
    if (2 * (g_style_n + 1) > g_style_cap) {
        uint32_t nc = g_style_cap ? g_style_cap * 2 : 4096;
        GCHandle *no = (GCHandle *)calloc(nc, sizeof(GCHandle));
        NodeId *nw = (NodeId *)calloc(nc, sizeof(NodeId));
        if (!no || !nw) { free(no); free(nw); return; }
        for (uint32_t i = 0; i < g_style_cap; i++) {
            if (!g_style_obj[i]) continue;
            uint32_t h = (g_style_obj[i] * 2654435761u) & (nc - 1);
            while (no[h]) h = (h + 1) & (nc - 1);
            no[h] = g_style_obj[i];
            nw[h] = g_style_owner[i];
        }
        free(g_style_obj);
        free(g_style_owner);
        g_style_obj = no;
        g_style_owner = nw;
        g_style_cap = nc;
        JS_watched_write_hook = style_owner_written;
    }
    uint32_t h = (obj * 2654435761u) & (g_style_cap - 1);
    while (g_style_obj[h] && g_style_obj[h] != obj) h = (h + 1) & (g_style_cap - 1);
    if (!g_style_obj[h]) g_style_n++;
    g_style_obj[h] = obj;
    g_style_owner[h] = owner;
}

GCValue dom_element_style(JSContextHandle ctx, GCValue el, bool create) {
    DomWrapHandle wd = dom_wrap_of(el);
    if (wd == GC_HANDLE_NULL) {
        GCValue s = JS_GetPropertyStr(ctx, el, "__style");
        if (JS_IsObject(s) || !create) return JS_IsObject(s) ? s : JS_UNDEFINED;
        return css_ensure_style_object(ctx, el);
    }
    DomWrap *w = (DomWrap *)gc_deref(wd);
    if (w && JS_IsObject(w->style)) return w->style;
    if (!create) return JS_UNDEFINED;
    GCValue style = css_new_style_object(ctx, el);
    SAFE_SET(wd, DomWrapHandle, style, style);
    /* Inline style attribute declarations land on the new object. */
    NodeId n = dom_node_of(el);
    style_owner_note(style, n);
    const char *attr = dom_attr(n, "style");
    if (attr && attr[0]) css_apply_inline_style_to(ctx, style, attr);
    return style;
}

/* Per-element JS state the page expects on a fresh wrapper. */
/* Attributes reflected by accessors on the element's prototype, which must
 * not be mirrored into own properties (they would shadow the accessor, or
 * re-enter it from setAttribute): href of <a>/<area> (HTMLHyperlinkElementUtils). */
static bool dom_attr_has_accessor(NodeId id, const char *name) {
    /* Reflected on Element.prototype by html_element_api.c (assigning the
     * attribute string would run the setter: hidden="" would remove it). */
    static const char *const reflected[] = {
        "hidden", "title", "lang", "dir", "draggable", "translate", "inert", NULL
    };
    for (int i = 0; reflected[i]; i++)
        if (strcasecmp(name, reflected[i]) == 0) return true;
    if (strcasecmp(name, "href") == 0 && (dom_tag_is(id, "a") || dom_tag_is(id, "area"))) return true;
    /* <link> reflections (html_element_api.c). */
    if (dom_tag_is(id, "link") &&
        (strcasecmp(name, "href") == 0 || strcasecmp(name, "rel") == 0 || strcasecmp(name, "media") == 0 ||
         strcasecmp(name, "type") == 0 || strcasecmp(name, "as") == 0 || strcasecmp(name, "integrity") == 0 ||
         strcasecmp(name, "hreflang") == 0)) return true;
    /* src / srcset of <img> (image_api.c): setting them starts a load. */
    return (strcasecmp(name, "src") == 0 || strcasecmp(name, "srcset") == 0) && dom_tag_is(id, "img");
}

void dom_element_setup_wrapper(JSContextHandle ctx, NodeId id, GCValue obj) {
    uint32_t s = dom_slot(id);
    /* Attributes are reflected as plain properties (the legacy DOM did this
     * for every parsed element); id, class and style have accessors. */
    DomAttrList *l = &g_dom.attrs[s];
    for (int i = 0; i < l->n; i++) {
        const char *name = css_atom_str(l->a[i].name);
        if (!name || !name[0]) continue;
        if (strcmp(name, "id") == 0 || strcmp(name, "class") == 0 || strcmp(name, "style") == 0) continue;
        if (dom_attr_has_accessor(id, name)) continue;
        DomStr v = g_dom.attrs[s].a[i].value;      /* re-read: setters may run JS */
        JS_SetPropertyStr(ctx, obj, name, JS_NewStringLen(ctx, v.s ? v.s : "", v.len));
        if (!dom_valid(id)) return;
        l = &g_dom.attrs[s];
    }
    if (g_dom.flags[s] & DOMF_PARSER_SCRIPT)
        JS_SetPropertyStr(ctx, obj, "__cyber_parser_script", JS_TRUE);
    if (dom_tag_is(id, "template")) {
        /* `content` has always been a writable own property of templates
         * (template polyfills test for and replace it). */
        NodeId c = dom_template_content(id);
        if (!c) {
            c = dom_create_fragment(dom_owner(id));
            dom_set_template_content(id, c);
        }
        GCValue content = dom_js(ctx, c);
        if (JS_IsObject(content)) {
            JS_SetPropertyStr(ctx, obj, "content", content);
            NodeId d = dom_owner(id);
            if (dom_has_wrapper(d))
                JS_DefinePropertyValueStr(ctx, content, "ownerDocument", dom_js_existing(d),
                                          JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        }
    }
    if (dom_tag_is(id, "canvas")) {
        JS_SetPropertyStr(ctx, obj, "getContext", JS_NewCFunction(ctx, js_canvas_get_context_fn(), "getContext", 1));
        JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, 0));
    }
}

/* Factory for wrappers of store nodes (dom_wrap.h). */
/* Interface (global constructor) whose prototype a new element wrapper gets,
 * so that `el instanceof HTMLInputElement` etc. hold and polyfills that patch
 * a specific interface (ShadyDOM patches HTMLSlotElement.prototype) reach it.
 * Interfaces the browser does not define fall back to HTMLElement. */
static const char *element_interface_name(NodeId id) {
    static const struct { const char *tag, *iface; } map[] = {
        { "a", "HTMLAnchorElement" },       { "area", "HTMLAreaElement" },
        { "button", "HTMLButtonElement" },  { "canvas", "HTMLCanvasElement" },
        { "div", "HTMLDivElement" },        { "fencedframe", "HTMLFencedFrameElement" },
        { "form", "HTMLFormElement" },      { "iframe", "HTMLIFrameElement" },
        { "img", "HTMLImageElement" },      { "input", "HTMLInputElement" },
        { "link", "HTMLLinkElement" },      { "option", "HTMLOptionElement" },
        { "script", "HTMLScriptElement" },  { "select", "HTMLSelectElement" },
        { "slot", "HTMLSlotElement" },      { "style", "HTMLStyleElement" },
        { "template", "HTMLTemplateElement" }, { "textarea", "HTMLTextAreaElement" },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (dom_tag_is(id, map[i].tag)) return map[i].iface;
    return "HTMLElement";
}

GCValue dom_create_wrapper(JSContextHandle ctx, NodeId id) {
    int kind = dom_kind(id);
    GCValue obj = JS_UNDEFINED;
    switch (kind) {
    case DOM_K_ELEMENT:
        /* Media elements (video, audio) share the native media class: load(),
         * play(), pause(), currentTime, canPlayType, ... */
        if (dom_tag_is(id, "video") || dom_tag_is(id, "audio")) {
            obj = js_video_constructor(ctx, JS_NULL, 0, NULL);
            if (!JS_IsObject(obj)) return JS_NULL;
            dom_bind(ctx, id, obj);
        } else {
            obj = JS_NewObjectClass(ctx, js_dom_node_class_id);
            if (JS_IsException(obj)) return JS_NULL;
            const char *proto_name = element_interface_name(id);
            GCValue proto = global_proto(ctx, proto_name);
            if (!JS_IsObject(proto)) proto = global_proto(ctx, "HTMLElement");
            if (JS_IsObject(proto)) JS_SetPrototype(ctx, obj, proto);
            dom_bind(ctx, id, obj);
        }
        dom_element_setup_wrapper(ctx, id, obj);
        return obj;
    case DOM_K_TEXT: {
        /* Text.prototype is the CharacterData prototype (data, length, ...). */
        obj = JS_NewObjectClass(ctx, js_dom_node_class_id);
        GCValue proto = global_proto(ctx, "Text");
        if (!JS_IsObject(proto)) proto = global_proto(ctx, "Node");
        if (JS_IsObject(proto)) JS_SetPrototype(ctx, obj, proto);
        dom_bind(ctx, id, obj);
        return obj;
    }
    case DOM_K_COMMENT: {
        /* data/length/textContent come from the CharacterData prototype, so
         * they follow the store (own snapshots went stale on change). */
        obj = JS_NewObjectClass(ctx, js_dom_node_class_id);
        GCValue proto = global_proto(ctx, "Comment");
        if (!JS_IsObject(proto)) proto = global_proto(ctx, "Node");
        if (JS_IsObject(proto)) JS_SetPrototype(ctx, obj, proto);
        dom_bind(ctx, id, obj);
        JS_SetPropertyStr(ctx, obj, "nodeType", JS_NewInt32(ctx, DOM_NODE_TYPE_COMMENT));
        JS_SetPropertyStr(ctx, obj, "nodeName", JS_NewString(ctx, "#comment"));
        return obj;
    }
    case DOM_K_FRAGMENT: {
        obj = JS_NewObjectClass(ctx, js_dom_node_class_id);
        GCValue proto = global_proto(ctx, "DocumentFragment");
        if (!JS_IsObject(proto)) proto = global_proto(ctx, "Node");
        if (JS_IsObject(proto)) JS_SetPrototype(ctx, obj, proto);
        dom_bind(ctx, id, obj);
        return obj;
    }
    case DOM_K_DOCUMENT: {
        obj = JS_NewObjectClass(ctx, js_dom_node_class_id);
        GCValue proto = global_proto(ctx, "Document");
        if (!JS_IsObject(proto)) proto = global_proto(ctx, "Node");
        if (JS_IsObject(proto)) JS_SetPrototype(ctx, obj, proto);
        dom_bind(ctx, id, obj);
        JS_SetPropertyStr(ctx, obj, "nodeType", JS_NewInt32(ctx, DOM_NODE_TYPE_DOCUMENT));
        return obj;
    }
    default:
        return JS_NULL;
    }
}

/* ============================================================================
 * Connectivity (with the JS-level shadow roots of polyfills)
 * ============================================================================ */

/* The node a fragment root continues to for connectivity: a native shadow
 * root's host, or the `host` property of a polyfill shadow root. */
static NodeId root_host(JSContextHandle ctx, NodeId root) {
    NodeId h = dom_host(root);
    if (h && (dom_flags(root) & DOMF_SHADOW_ROOT)) return h;
    /* A DocumentFragment is not a shadow root because script gave it a
     * host: ShadyDOM turns its stamped template fragment into a "shadow
     * root" (ShadowRoot.prototype, a host) and only then appends it to the
     * host.  Until then its content is in a detached fragment, as in any
     * browser; counting it connected made moving it into the host
     * disconnect custom elements that were never connected (YouTube's
     * subscribe button cleared itself on that disconnect). */
    if (dom_kind(root) != DOM_K_ELEMENT) return DOM_NULL;
    if (!dom_has_wrapper(root)) return DOM_NULL;
    GCValue obj = dom_js_existing(root);
    {
        /* Polyfill shadow roots made of plain objects report nodeType 11.
         * Checked first: the root of a detached subtree is usually an
         * ordinary element, whose "host" can be a costly getter (a URL's
         * host on <a>, or page code a polyfill patched in). */
        GCValue nt = JS_GetPropertyStr(ctx, obj, "nodeType");
        int32_t t = 0;
        JS_ToInt32(ctx, &t, nt);
        if (t != DOM_NODE_TYPE_DOCUMENT_FRAGMENT) return DOM_NULL;
    }
    GCValue host = JS_GetPropertyStr(ctx, obj, "host");
    if (!JS_IsObject(host)) return DOM_NULL;
    return dom_node_of(host);
}

bool dom_api_is_connected(JSContextHandle ctx, NodeId n) {
    for (int guard = 0; n && guard < 64; guard++) {
        NodeId r = dom_root(n);
        if (dom_kind(r) == DOM_K_DOCUMENT) return true;
        if (dom_flags(r) & DOMF_TEMPLATE_CONTENT) return false;
        n = root_host(ctx, r);
    }
    return false;
}

static bool dom_node_is_connected(JSContextHandle ctx, GCValue node) {
    return dom_api_is_connected(ctx, dom_node_of(node));
}

GCValue js_node_is_connected_getter(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return JS_NewBool(ctx, dom_node_is_connected(ctx, this_val));
}

/* ============================================================================
 * Custom element reactions
 * ============================================================================ */

static void invoke_custom_element_callback(JSContextHandle ctx, GCValue elem, const char *name) {
    // Custom element lifecycle callbacks are CEReactions and must run after
    // any queued upgrade reactions have completed, not synchronously during the
    // DOM mutation. Enqueue them into the custom-element reaction queue.
    js_cyber_ce_enqueue_callback(ctx, elem, name);
}

// Flush the custom-element reaction queue for the current CEReactions stack
// frame.  Callers must push a frame before enqueueing reactions and pop it
// afterwards, matching the HTML spec's CEReactions stack.
static void dom_flush_ce_reactions(JSContextHandle ctx) {
    js_cyber_ce_flush_reactions(ctx, JS_UNDEFINED, 0, NULL);
}

/* Is `name` in the observedAttributes of elem's custom element class?  The
 * list is read once per constructor (custom element definitions capture it
 * at define time) and kept as an own property of the constructor, so a
 * subclass never reuses its parent's list. */
static bool attribute_is_observed(JSContextHandle ctx, GCValue elem, const char *name) {
    GCValue ctor = JS_GetPropertyStr(ctx, elem, "constructor");
    if (!JS_IsObject(ctor)) return false;
    JSAtom key = JS_NewAtom(ctx, "__cyber_observed");
    JSPropertyDescriptor desc = JS_PROPERTY_DESCRIPTOR_INIT;
    GCValue set = JS_UNDEFINED;
    if (JS_GetOwnProperty(ctx, &desc, ctor, key) > 0) set = desc.value;
    if (!JS_IsObject(set)) {
        set = JS_NewObjectProto(ctx, JS_NULL);
        GCValue list = JS_GetPropertyStr(ctx, ctor, "observedAttributes");
        if (JS_IsException(list)) { JS_GetException(ctx); list = JS_UNDEFINED; }
        if (JS_IsObject(list)) {
            uint32_t n = 0;
            JS_ToUint32(ctx, &n, JS_GetPropertyStr(ctx, list, "length"));
            for (uint32_t i = 0; i < n; i++) {
                const char *s = JS_ToCString(ctx, JS_GetPropertyUint32(ctx, list, i));
                if (s) JS_SetPropertyStr(ctx, set, s, JS_TRUE);
            }
        }
        JS_DefinePropertyValue(ctx, ctor, key, set, JS_PROP_CONFIGURABLE);
    }
    JS_FreeAtom(ctx, key);
    GCValue hit = JS_GetPropertyStr(ctx, set, name);
    return JS_ToBool(ctx, hit);
}

static void invoke_attribute_changed(JSContextHandle ctx, GCValue elem,
                                     const char *name, const char *old_val,
                                     const char *new_val) {
    /* Custom element reactions only for observed attributes (HTML §4.13). */
    if (!attribute_is_observed(ctx, elem, name)) return;
    GCValue cb = JS_GetPropertyStr(ctx, elem, "attributeChangedCallback");
    if (JS_IsException(cb)) {
        JS_GetException(ctx); // clear
        return;
    }
    if (!JS_IsUndefined(cb) && !JS_IsNull(cb) && JS_IsFunction(ctx, cb)) {
        GCValue args[3] = { [0 ... (3) - 1] = GC_UNDEFINED_INIT };
        args[0] = JS_NewString(ctx, name);
        args[1] = old_val ? JS_NewString(ctx, old_val) : JS_NULL;
        args[2] = new_val ? JS_NewString(ctx, new_val) : JS_NULL;
        GCValue ret = JS_Call(ctx, cb, elem, 3, args);
        if (JS_IsException(ret)) {
            JS_GetException(ctx); // clear
        }
    }
}

/* Candidates (elements whose name contains '-') of the light subtree of
 * `root`, in document order.  Branches without candidates are skipped. */
static void collect_ce_candidates(NodeId root, uint32_t **out, uint32_t *n) {
    uint32_t cap = 0;
    *out = NULL;
    *n = 0;
    if (!dom_valid(root) || !dom_subtree_has_ce(root)) return;
    uint32_t r = dom_slot(root);
    for (uint32_t s = r; s; ) {
        if (g_dom.kind[s] == DOM_K_ELEMENT && (g_dom.flags[s] & DOMF_CE_CANDIDATE)) {
            if (*n == cap) { cap = cap ? cap * 2 : 16; *out = realloc(*out, cap * sizeof(uint32_t)); }
            (*out)[(*n)++] = dom_id_of(s);
        }
        bool down = g_dom.first[s] && (g_dom.flags[s] & (DOMF_CE_DESC | DOMF_CE_CANDIDATE));
        if (down && (g_dom.flags[s] & DOMF_CE_DESC)) { s = g_dom.first[s]; continue; }
        while (s && s != r && !g_dom.next[s]) s = g_dom.parent[s];
        if (!s || s == r) break;
        s = g_dom.next[s];
    }
}

/* A subtree was inserted into a connected tree: enqueue an upgrade for every
 * custom element candidate in it, then connectedCallbacks for the elements
 * that already have one (upgraded elements).  Upgrades run only on connect. */
void dom_ce_on_connect(JSContextHandle ctx, NodeId subtree) {
    CP_ACC("CE connect+upgrade");
    uint32_t *ids = NULL, n = 0;
    collect_ce_candidates(subtree, &ids, &n);
    if (!n) { free(ids); return; }
    GCValueVec els GC_VALUE_VEC_AUTO = { 0 };
    for (uint32_t i = 0; i < n; i++) {
        GCValue el = dom_js(ctx, ids[i]);
        if (JS_IsObject(el)) gc_value_vec_push(&els, el);
    }
    free(ids);
    for (size_t i = 0; i < els.count; i++) {
        GCValue el = els.data[i];
        js_cyber_ce_enqueue_upgrade(ctx, JS_UNDEFINED, 1, &el);
    }
    for (size_t i = 0; i < els.count; i++) {
        if (dom_node_is_connected(ctx, els.data[i]))
            invoke_custom_element_callback(ctx, els.data[i], "connectedCallback");
    }
}

/* Nodes created by cloning or importing into a document with a browsing
 * context: their defined custom elements upgrade before the API returns
 * ([CEReactions]), connected or not.  A template stamped with importNode
 * then has its elements' accessors in place before the stamping code sets
 * properties on them (a property set on a not-yet-upgraded element would
 * stay an own data property shadowing the element's accessor). */
static void dom_ce_upgrade_created(JSContextHandle ctx, NodeId subtree) {
    uint32_t *ids = NULL, n = 0;
    collect_ce_candidates(subtree, &ids, &n);
    if (!n) { free(ids); return; }
    GCValueVec els GC_VALUE_VEC_AUTO = { 0 };
    for (uint32_t i = 0; i < n; i++) {
        GCValue el = dom_js(ctx, ids[i]);
        if (JS_IsObject(el)) gc_value_vec_push(&els, el);
    }
    free(ids);
    js_cyber_ce_push_stack(ctx);
    for (size_t i = 0; i < els.count; i++) {
        GCValue el = els.data[i];
        js_cyber_ce_enqueue_upgrade(ctx, JS_UNDEFINED, 1, &el);
    }
    dom_flush_ce_reactions(ctx);
    js_cyber_ce_pop_stack(ctx);
}

/* ============================================================================
 * Computed-style table (getComputedStyle) on the wrapper record
 * ============================================================================ */

#define CSS_COMPUTED_STYLE_BUCKETS 16

GCHandle css_ensure_computed_style(DomWrapHandle node)
{
    if (node == GC_HANDLE_NULL) return GC_HANDLE_NULL;
    GCHandle h = SAFE_GET_OR(node, DomWrapHandle, computed_style, GC_HANDLE_NULL);
    if (h != GC_HANDLE_NULL) return h;
    h = gc_allocz(sizeof(CssComputedStyle), JS_GC_OBJ_TYPE_DATA);
    if (h == GC_HANDLE_NULL) return GC_HANDLE_NULL;
    CssComputedStyle *cs = (CssComputedStyle *)gc_deref(h);
    cs->properties = lf_hash_create(CSS_COMPUTED_STYLE_BUCKETS);
    if (!cs->properties) return GC_HANDLE_NULL;
    SAFE_SET_HANDLE(node, DomWrapHandle, computed_style, h);
    return h;
}

/* Store a computed CSS property for a node.  The value is copied into a JS
 * string handle that is kept alive by the wrapper's gc_mark callback. */
void css_computed_set_property(JSContextHandle ctx, DomWrapHandle node,
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
    lf_hash_insert(cs->properties, (uint32_t)prop_atom, (GCHandle)prop_atom, str_handle);
    /* The table is malloc'd: tell the collector about the reference. */
    gc_note_ref(str_handle);
    const char *prop_str = JS_AtomToCString(ctx, prop_atom);
    if (prop_str) {
        char *camel = css_to_camel_case(prop_str);
        if (camel && strcmp(camel, prop_str) != 0) {
            JSAtom camel_atom = JS_NewAtom(ctx, camel);
            if (camel_atom != JS_ATOM_NULL) {
                lf_hash_insert(cs->properties, (uint32_t)camel_atom, (GCHandle)camel_atom, str_handle);
                JS_FreeAtom(ctx, camel_atom);
            }
        }
        free(camel);
    }
}

GCValue css_computed_get_property(JSContextHandle ctx, DomWrapHandle node, JSAtom prop_atom)
{
    (void)ctx;
    if (prop_atom == JS_ATOM_NULL || node == GC_HANDLE_NULL) return JS_UNDEFINED;
    GCHandle cs_handle = SAFE_GET_OR(node, DomWrapHandle, computed_style, GC_HANDLE_NULL);
    if (cs_handle == GC_HANDLE_NULL) return JS_UNDEFINED;
    CssComputedStyle *cs = (CssComputedStyle *)gc_deref(cs_handle);
    if (!cs || !cs->properties) return JS_UNDEFINED;
    GCHandle str_handle = lf_hash_lookup(cs->properties, (uint32_t)prop_atom, (GCHandle)prop_atom);
    if (str_handle == GC_HANDLE_NULL) return JS_UNDEFINED;
    return GC_MKHANDLE(JS_TAG_STRING, str_handle);
}

void css_computed_apply_declarations(JSContextHandle ctx, DomWrapHandle node,
                                     CssAppliedDecl *applied, int count)
{
    if (node == GC_HANDLE_NULL || count <= 0) return;
    for (int i = 0; i < count; i++) {
        JSAtom atom = JS_NewAtom(ctx, applied[i].decl->property);
        if (atom != JS_ATOM_NULL) {
            css_computed_set_property(ctx, node, atom, applied[i].decl->value);
            JS_FreeAtom(ctx, atom);
        }
    }
}

void css_computed_apply_inline_style(JSContextHandle ctx, DomWrapHandle node, const char *style_attr)
{
    if (node == GC_HANDLE_NULL || !style_attr || !style_attr[0]) return;
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

/* ============================================================================
 * Element lookups by id / class / tag (the store replaces the GC-heap index
 * tables; these keep their legacy entry points).
 * ============================================================================ */

CssDocumentState *css_document_state_ensure(JSRuntimeHandle rt) { (void)rt; return NULL; }
void css_document_state_destroy(JSRuntimeHandle rt) { (void)rt; }
void css_document_state_clear(JSRuntimeHandle rt) { (void)rt; }
void css_index_insert_node(JSContextHandle ctx, DomWrapHandle node) { (void)ctx; (void)node; }

GCValue css_get_element_by_id(JSContextHandle ctx, JSAtom id)
{
    const char *s = JS_AtomToCString(ctx, id);
    if (!s) return JS_NULL;
    NodeId n = DOM_NULL;
    if (dom_valid(g_dom_index_root)) {
        uint32_t r = dom_slot(g_dom_index_root);
        for (uint32_t x = r; x && !n; x = dom_next_in_subtree(x, r)) {
            if (g_dom.kind[x] != DOM_K_ELEMENT) continue;
            const char *v = dom_attr(dom_id_of(x), "id");
            if (v && strcmp(v, s) == 0) n = dom_id_of(x);
        }
    } else {
        n = dom_query_by_id(index_doc(), s, strlen(s));
    }
    JS_FreeCString(ctx, s);
    return dom_js(ctx, n);
}

static GCValue nodes_to_array(JSContextHandle ctx, const NodeId *ids, uint32_t n) {
    GCValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < n; i++) JS_SetPropertyUint32(ctx, arr, i, dom_js(ctx, ids[i]));
    return arr;
}

GCValue css_get_elements_by_class_name(JSContextHandle ctx, JSAtom class_atom)
{
    const char *s = JS_AtomToCString(ctx, class_atom);
    if (!s) return JS_NewArray(ctx);
    DomNodeList l = dom_valid(g_dom_index_root) ? dom_query_by_class_in(g_dom_index_root, s, strlen(s))
                                                : dom_query_by_class(index_doc(), s, strlen(s));
    JS_FreeCString(ctx, s);
    GCValue arr = nodes_to_array(ctx, l.ids, l.n);
    dom_node_list_free(&l);
    return arr;
}

GCValue css_get_elements_by_tag_name(JSContextHandle ctx, JSAtom tag_atom)
{
    const char *s = JS_AtomToCString(ctx, tag_atom);
    if (!s) return JS_NewArray(ctx);
    DomNodeList l = dom_valid(g_dom_index_root) ? dom_query_by_tag_in(g_dom_index_root, s, strlen(s), true)
                                                : dom_query_by_tag(index_doc(), s, strlen(s));
    JS_FreeCString(ctx, s);
    GCValue arr = nodes_to_array(ctx, l.ids, l.n);
    dom_node_list_free(&l);
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
    DOMExceptionDataHandle de = dom_exception_create();
    if (SAFE_IS_NULL(de)) {
        return JS_ThrowTypeError(ctx, "%s: %s", name, message);
    }
    
    SAFE_SET_STR(de, DOMExceptionDataHandle, name, name);
    SAFE_SET_STR(de, DOMExceptionDataHandle, message, message);
    
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
    SAFE_SET(de, DOMExceptionDataHandle, code, code);
    
    // Create the exception object
    GCValue exc = JS_NewObjectClass(ctx, js_dom_exception_class_id);
    if (JS_IsException(exc)) {
        return JS_ThrowTypeError(ctx, "%s: %s", name, message);
    }
    
    JS_SetOpaqueHandle(exc, de);
    
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



/* ============================================================================
 * Shadow DOM class
 * ============================================================================ */

void js_shadow_root_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    GCHandle sr_handle = JS_GetOpaqueHandle(val, js_shadow_root_class_id);
    if (sr_handle == GC_HANDLE_NULL) return;
    ShadowRootData *sr = (ShadowRootData *)gc_deref(sr_handle);
    if (!sr) return;
    dom_wrap_finalize(sr->dom_node, val);
}

void js_shadow_root_mark(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func) {
    GCHandle sr_handle = JS_GetOpaqueHandle(val, js_shadow_root_class_id);
    if (sr_handle == GC_HANDLE_NULL) return;
    mark_func(rt, sr_handle);
    ShadowRootData *sr = (ShadowRootData *)gc_deref(sr_handle);
    if (!sr) return;
    JS_MarkValue(rt, sr->host, mark_func);
    JS_MarkValue(rt, sr->innerHTML, mark_func);
    if (sr->dom_node != GC_HANDLE_NULL) dom_wrap_mark(rt, sr->dom_node, mark_func);
}

JSClassDef js_shadow_root_class_def = {
    .class_name = "ShadowRoot",
    .finalizer = js_shadow_root_finalizer,
    .gc_mark   = js_shadow_root_mark,
};

// ShadowRoot constructor - called when new ShadowRoot() is invoked
GCValue js_shadow_root_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)argc; (void)argv; (void)new_target;
    GCValue obj = JS_NewObjectClass(ctx, js_shadow_root_class_id);
    if (JS_IsException(obj)) return JS_EXCEPTION;
    return obj;
}

// ============================================================================
// Node access
// ============================================================================

bool is_dom_node(JSContextHandle ctx, GCValue obj) {
    (void)ctx;
    return dom_node_of(obj) != DOM_NULL;
}

static NodeId owner_of_value(JSContextHandle ctx, GCValue doc) {
    (void)ctx;
    NodeId d = dom_node_of(doc);
    return dom_kind(d) == DOM_K_DOCUMENT ? d : g_dom_document;
}

/* Resolve a URL against window.location.href.  Only handles the http/https
 * cases needed for YouTube module loading. */
static char* dom_resolve_url(const char *base, const char *href) {
    if (!href || !href[0]) return base ? strdup(base) : NULL;
    if (url_has_scheme(href)) return strdup(href);

    const char *scheme_end = base ? strstr(base, "://") : NULL;
    if (!scheme_end) return strdup(href);

    const char *path_start = strchr(scheme_end + 3, '/');
    size_t origin_len = path_start ? (size_t)(path_start - base) : strlen(base);

    if (href[0] == '/' && href[1] == '/') {
        size_t scheme_only_len = (size_t)(scheme_end - base);
        char *out = (char*)malloc(scheme_only_len + 1 + strlen(href) + 1);
        if (!out) return strdup(href);
        memcpy(out, base, scheme_only_len);
        out[scheme_only_len] = ':';
        strcpy(out + scheme_only_len + 1, href);
        return out;
    }
    if (href[0] == '/') {
        char *out = (char*)malloc(origin_len + strlen(href) + 1);
        if (!out) return strdup(href);
        memcpy(out, base, origin_len);
        strcpy(out + origin_len, href);
        return out;
    }
    const char *base_dir = path_start ? path_start : "/";
    const char *last_slash = strrchr(base_dir, '/');
    size_t dir_len = last_slash ? (size_t)(last_slash - base_dir + 1) : strlen(base_dir);
    char *out = (char*)malloc(origin_len + dir_len + strlen(href) + 1);
    if (!out) return strdup(href);
    memcpy(out, base, origin_len);
    memcpy(out + origin_len, base_dir, dir_len);
    strcpy(out + origin_len + dir_len, href);
    return out;
}

static void dom_dispatch_script_event(JSContextHandle ctx, GCValue script, const char *type) {
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue event_ctor = JS_GetPropertyStr(ctx, global, "Event");
    GCValue type_val = JS_NewString(ctx, type);
    GCValue event = JS_IsFunction(ctx, event_ctor)
        ? JS_CallConstructor(ctx, event_ctor, 1, &type_val)
        : JS_UNDEFINED;
    if (!JS_IsUndefined(event) && !JS_IsNull(event) && !JS_IsException(event)) {
        js_event_target_dispatchEvent(ctx, script, 1, &event);
    }
}

static bool dom_script_type_is_executable(JSContextHandle ctx, GCValue script) {
    GCValue typev = JS_GetPropertyStr(ctx, script, "type");
    if (JS_IsUndefined(typev) || JS_IsNull(typev)) return true;
    const char *type = JS_IsString(typev) ? JS_ToCString(ctx, typev) : NULL;
    bool ok = true;
    if (type && type[0]) {
        if (strstr(type, "module") || strstr(type, "importmap")) ok = false;
    }
    if (type) JS_FreeCString(ctx, type);
    return ok;
}

static void dom_run_fetched_script(JSContextHandle ctx, GCValue script, const char *url, bool ok,
                                   const char *data, size_t size, const char *err);
static bool dom_queue_dynamic_script(JSContextHandle ctx, GCValue script, const char *url);

static void dom_execute_external_script(JSContextHandle ctx, GCValue script, const char *src) {
    if (!g_dom_dynamic_scripts_enabled) return;
    if (!src || !src[0]) return;

    GCValue loaded = JS_GetPropertyStr(ctx, script, "__cyber_script_loaded");
    if (JS_ToBool(ctx, loaded)) return;
    if (dom_flags(dom_node_of(script)) & DOMF_PARSER_SCRIPT) return;
    GCValue parser = JS_GetPropertyStr(ctx, script, "__cyber_parser_script");
    if (JS_ToBool(ctx, parser)) return;
    if (!dom_node_is_connected(ctx, script)) return;
    if (!dom_script_type_is_executable(ctx, script)) return;

    GCValue global = JS_GetGlobalObject(ctx);
    const char *base = NULL;
    int base_needs_free = 0;
    GCValue loc = JS_GetPropertyStr(ctx, global, "location");
    GCValue href = JS_GetPropertyStr(ctx, loc, "href");
    if (JS_IsString(href)) {
        base = JS_ToCString(ctx, href);
        base_needs_free = 1;
    }
    if (!base || !base[0]) {
        GCValue doc = JS_GetPropertyStr(ctx, global, "document");
        GCValue baseuri = JS_GetPropertyStr(ctx, doc, "baseURI");
        if (JS_IsString(baseuri)) {
            if (base_needs_free) JS_FreeCString(ctx, base);
            base = JS_ToCString(ctx, baseuri);
            base_needs_free = 1;
        }
    }
    if ((!base || !base[0]) && g_cyber_start_url && g_cyber_start_url[0]) {
        if (base_needs_free) JS_FreeCString(ctx, base);
        base = g_cyber_start_url;
        base_needs_free = 0;
    }

    char *url = dom_resolve_url(base ? base : "", src);
    if (base_needs_free) JS_FreeCString(ctx, base);
    if (!url) return;

    if (!url_is_network_url(url)) {
        free(url);
        return;
    }

    /* Mark as loaded before the network fetch so recursive insertions of the
     * same node do not re-trigger execution. */
    JS_SetPropertyStr(ctx, script, "__cyber_script_loaded", JS_TRUE);
    JS_SetPropertyStr(ctx, script, "readyState", JS_NewString(ctx, "loading"));

    platform_log(LOG_LEVEL_INFO, "dom_api", "Dynamic script load: %s", url);
    if (dom_queue_dynamic_script(ctx, script, url)) {
        free(url);
        return;
    }
    HttpBuffer buffer = {0};
    char err[256] = {0};
    cp_profile_begin("fetch-dynamic-script", "net");
    bool ok = dom_fetch_script(url, &buffer, err, sizeof(err));
    cp_profile_end("fetch-dynamic-script");
    dom_run_fetched_script(ctx, script, url, ok, buffer.data, buffer.size, err);
    free(url);
    if (buffer.data) free(buffer.data);
}

/* Evaluate a fetched script-inserted script and fire load / error. */
static void dom_run_fetched_script(JSContextHandle ctx, GCValue script, const char *url, bool ok,
                                   const char *data, size_t size, const char *err) {
    GCValue global = JS_GetGlobalObject(ctx);
    if (ok && data && size > 0) {
        HttpBuffer buffer = { (char *)data, size, NULL, 0 };
        if (CYBER_ENV(CYBER_T_DUMP_SCRIPTS)) {
            static int dyn_dump_idx = 0;
            char dump_name[64];
            snprintf(dump_name, sizeof(dump_name), "exec_dyn_%d.js", dyn_dump_idx++);
            FILE *df = fopen(dump_name, "wb");
            if (df) {
                fwrite(buffer.data, 1, buffer.size, df);
                fclose(df);
            }
            platform_log(LOG_LEVEL_INFO, "dom_api", "Dumped dynamic script to %s url=%s", dump_name, url);
        }
        GCValue doc = JS_GetPropertyStr(ctx, global, "document");
        GCValue prev_current = JS_GetPropertyStr(ctx, doc, "currentScript");
        JS_SetPropertyStr(ctx, doc, "currentScript", script);
        JS_SetPropertyStr(ctx, script, "readyState", JS_NewString(ctx, "loaded"));

        cp_profile_begin("eval-dynamic-script", "js");
        GCValue result = JS_Eval(ctx, buffer.data, buffer.size, url, JS_EVAL_TYPE_GLOBAL);
        cp_profile_end("eval-dynamic-script");

        JS_SetPropertyStr(ctx, script, "readyState", JS_NewString(ctx, "complete"));
        JS_SetPropertyStr(ctx, doc, "currentScript", prev_current);
        if (JS_IsException(result)) {
            GCValue exc = JS_GetException(ctx);
            const char *estr = JS_ToCString(ctx, exc);
            platform_log(LOG_LEVEL_WARN, "dom_api", "Dynamic script eval error: %s", estr ? estr : "?");
            if (estr) JS_FreeCString(ctx, estr);
            dom_dispatch_script_event(ctx, script, "error");
        } else {
            dom_dispatch_script_event(ctx, script, "load");
        }
    } else {
        platform_log(LOG_LEVEL_WARN, "dom_api", "Dynamic script fetch failed: %s (%s)", url, err && err[0] ? err : "unknown");
        JS_SetPropertyStr(ctx, script, "readyState", JS_NewString(ctx, "error"));
        dom_dispatch_script_event(ctx, script, "error");
    }
}

/* Script-inserted scripts load on network threads (js_quickjs_net_get) so
 * their downloads overlap each other and the page's scripts, as in browsers
 * where they are async. They run from the event loop in insertion order
 * (one valid order for async scripts, and the one the spec requires for
 * async = false). */
typedef struct DynScript {
    struct DynScript *next;
    GCValue script;             /* rooted while queued */
    char *url;
    bool done, ok;
    char *data;
    size_t size;
    char err[256];
} DynScript;

static DynScript *g_dyn_head, *g_dyn_tail;

static void dyn_root(GCValue v, bool add) {
    if (GC_VALUE_GET_TAG(v) >= 0) return;
    GCHandle h = GC_VALUE_GET_HANDLE(v);
    if (h == GC_HANDLE_NULL) return;
    if (add) gc_add_root(h); else gc_remove_root(h);
}

static void dyn_drain(JSContextHandle ctx) {
    while (g_dyn_head && g_dyn_head->done) {
        DynScript *d = g_dyn_head;
        g_dyn_head = d->next;
        if (!g_dyn_head) g_dyn_tail = NULL;
        dom_run_fetched_script(ctx, d->script, d->url, d->ok, d->data, d->size, d->err);
        dyn_root(d->script, false);
        free(d->url);
        free(d->data);
        free(d);
    }
}

static void dyn_done(JSContextHandle ctx, void *user, const char *url, bool ok, int status,
                     PlatformHttpBuffer *resp, const char *err) {
    (void)url;
    DynScript *d = (DynScript *)user;
    d->ok = ok && status >= 200 && status < 300 && resp->data && resp->size > 0;
    if (d->ok) {
        d->data = resp->data;
        d->size = resp->size;
        resp->data = NULL;
        resp->size = 0;
    } else {
        snprintf(d->err, sizeof(d->err), "%s", err && err[0] ? err : status ? "HTTP error" : "fetch failed");
    }
    d->done = true;
    dyn_drain(ctx);
}

static bool dom_queue_dynamic_script(JSContextHandle ctx, GCValue script, const char *url) {
    static int sync = -1;
    if (sync < 0) sync = cyber_getenv("CYBER_SYNC_SCRIPTS") != NULL;
    if (sync) return false;
    (void)ctx;
    DynScript *d = (DynScript *)calloc(1, sizeof(*d));
    if (!d) return false;
    d->url = strdup(url);
    if (!d->url) { free(d); return false; }
    d->script = script;
    dyn_root(script, true);
    if (g_dyn_tail) g_dyn_tail->next = d; else g_dyn_head = d;
    g_dyn_tail = d;
    if (!js_quickjs_net_get(url, dyn_done, d)) {
        /* Could not start: unqueue (it is the tail) and load inline. */
        DynScript **pp = &g_dyn_head;
        DynScript *prev = NULL;
        while (*pp != d) { prev = *pp; pp = &(*pp)->next; }
        *pp = NULL;
        g_dyn_tail = prev;
        dyn_root(script, false);
        free(d->url);
        free(d);
        return false;
    }
    return true;
}

static void dom_maybe_load_script(JSContextHandle ctx, GCValue node) {
    if (!g_dom_dynamic_scripts_enabled) return;
    if (!dom_tag_is(dom_node_of(node), "script")) return;
    GCValue srcv = JS_GetPropertyStr(ctx, node, "src");
    const char *src = JS_IsString(srcv) ? JS_ToCString(ctx, srcv) : NULL;
    if (src && src[0]) {
        dom_execute_external_script(ctx, node, src);
    }
    if (src) JS_FreeCString(ctx, src);
}

/* Stylesheets of inserted links still downloading (the fetch pool has
 * them; a layout that needs one meanwhile waits for it there). */
typedef struct SheetLoad {
    GCValue link;
    char *url;
    struct SheetLoad *next;
} SheetLoad;
static SheetLoad *g_sheet_loads;
static int g_sheet_loads_n;

int dom_pending_stylesheet_loads(void) { return g_sheet_loads_n; }

int dom_poll_stylesheet_loads(JSContextHandle ctx) {
    int fired = 0;
    SheetLoad **pp = &g_sheet_loads;
    while (*pp) {
        SheetLoad *s = *pp;
        /* Done: downloaded (take it into the cache) or already taken in. */
        if (!css_layout_stylesheet_cached(s->url) && !dom_fetch_ready(s->url)) { pp = &s->next; continue; }
        *pp = s->next;
        g_sheet_loads_n--;
        bool ok = css_layout_fetch_stylesheet_url(s->url);
        if (ok) dom_request_relayout();
        dom_dispatch_script_event(ctx, s->link, ok ? "load" : "error");
        dyn_root(s->link, false);
        free(s->url);
        free(s);
        fired++;
    }
    return fired;
}

/* A connected <link rel="stylesheet" href> loads its sheet and fires load
 * (or error) once, as in browsers; pages wait for it (YouTube's player
 * inserts link#player-css and only creates the player on its load event).
 * A sheet not fetched yet downloads in the background: the inserting script
 * goes on, and load fires from the event loop once it is in. */
static void dom_maybe_load_stylesheet(JSContextHandle ctx, GCValue node) {
    NodeId n = dom_node_of(node);
    if (!dom_tag_is(n, "link") || !dom_node_is_connected(ctx, node)) return;
    const char *rel = dom_attr(n, "rel");
    const char *href = dom_attr(n, "href");
    if (!rel || !href || !href[0]) return;
    /* rel is a token list: "stylesheet", "preload stylesheet", ... */
    bool sheet = false;
    for (const char *p = rel; *p; ) {
        while (*p == ' ' || *p == '\t') p++;
        const char *e = p;
        while (*e && *e != ' ' && *e != '\t') e++;
        if (e - p == 10 && strncasecmp(p, "stylesheet", 10) == 0) sheet = true;
        p = e;
    }
    if (!sheet) return;
    GCValue done = JS_GetPropertyStr(ctx, node, "__cyber_sheet_loaded");
    if (JS_ToBool(ctx, done)) return;
    JS_SetPropertyStr(ctx, node, "__cyber_sheet_loaded", JS_TRUE);

    GCValue global = JS_GetGlobalObject(ctx);
    GCValue loc = JS_GetPropertyStr(ctx, global, "location");
    GCValue hrefv = JS_GetPropertyStr(ctx, loc, "href");
    const char *base = JS_IsString(hrefv) ? JS_ToCString(ctx, hrefv) : NULL;
    char *url = dom_resolve_url(base && base[0] ? base : (g_cyber_start_url ? g_cyber_start_url : ""), href);
    if (base) JS_FreeCString(ctx, base);
    if (url && url_is_network_url(url) && !css_layout_stylesheet_cached(url) &&
        !cyber_getenv("CYBER_SYNC_SHEETS")) {
        SheetLoad *s = (SheetLoad *)calloc(1, sizeof(*s));
        if (s) {
            s->link = node;
            s->url = url;
            dyn_root(node, true);
            dom_fetch_prefetch_first(url);
            s->next = g_sheet_loads;
            g_sheet_loads = s;
            g_sheet_loads_n++;
            return;
        }
    }
    bool ok = url && url_is_network_url(url) && css_layout_fetch_stylesheet_url(url);
    free(url);
    dom_dispatch_script_event(ctx, node, ok ? "load" : "error");
}

/* ============================================================================
 * Tree mutation
 * ============================================================================ */

static void trace_root_insertion(JSContextHandle ctx, GCValue parent, GCValue child, const char *label) {
    GCValue nt = JS_GetPropertyStr(ctx, parent, "nodeType");
    int32_t ntv = 0;
    JS_ToInt32(ctx, &ntv, nt);
    GCValue shady = JS_GetPropertyStr(ctx, parent, "__shady");
    bool host_is_app = ntv == 1 && dom_tag_is(dom_node_of(parent), "ytd-app");
    if (JS_IsObject(shady) && (ntv == 11 || host_is_app)) {
        NodeId c = dom_node_of(child);
        const char *trace_js = "(new Error('ri')).stack";
        GCValue tv = JS_Eval(ctx, trace_js, strlen(trace_js), "<ritrace>", JS_EVAL_TYPE_GLOBAL);
        const char *ts = JS_ToCString(ctx, tv);
        fprintf(stderr, "[%s] into=%s child=%s:\n%s\n", label,
                host_is_app ? "YTD-APP" : "FRAG", dom_node_name(c), ts ? ts : "?");
        fflush(stderr);
    }
}

static void notify_child_list(JSContextHandle ctx, GCValue target, GCValue node) {
    GCValue added = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, added, 0, node);
    GCValue removed = JS_NewArray(ctx);
    mo_notify_child_list(ctx, target, added, removed);
}

/* Insert `child` under `parent` before `ref` (DOM_NULL appends), with the
 * DOM API's side effects.  `child` must not be a fragment. */
bool dom_api_insert(JSContextHandle ctx, NodeId parent, NodeId child, NodeId ref) {
    CP_ACC("DOM insert");
    if (!dom_valid(parent) || !dom_valid(child)) return false;
    if (dom_contains(child, parent) || dom_kind(child) == DOM_K_DOCUMENT) {
        throw_dom_exception(ctx, "HierarchyRequestError", "The new child contains the parent");
        return false;
    }
    /* Wrappers only when something will see them (see dom_api_remove). */
    GCValue parent_js = dom_js(ctx, parent);
    bool observers = mo_child_list_observed(ctx, parent_js);
    bool need_js = observers || dom_has_wrapper(child) || dom_tag_is(child, "script") ||
                   dom_tag_is(child, "link");
    GCValue child_js = need_js ? dom_js(ctx, child) : JS_NULL;
    /* Remove the child from its current parent first (with its reactions). */
    NodeId old_parent = dom_parent(child);
    if (old_parent) {
        if (ref == child) ref = dom_next_sibling(child);
        dom_api_remove(ctx, old_parent, child);
        if (!dom_valid(parent) || !dom_valid(child)) return false;
        if (dom_parent(child)) dom_detach(child, false);
    }
    if (ref && dom_parent(ref) != parent) ref = DOM_NULL;
    dom_attach(parent, child, ref);
    dom_request_layout();

    // CEReactions frame for this insertion: any connectedCallback and upgrade
    // reactions queued below are flushed before we return, so a parent
    // constructor can rely on its inserted children being upgraded.
    js_cyber_ce_push_stack(ctx);
    if (dom_api_is_connected(ctx, child)) dom_ce_on_connect(ctx, child);
    dom_flush_ce_reactions(ctx);
    js_cyber_ce_pop_stack(ctx);

    // Load external scripts that are inserted dynamically after the initial
    // page load (e.g. YouTube's module loader).
    if (need_js) dom_maybe_load_script(ctx, child_js);
    if (need_js && g_dom_dynamic_scripts_enabled) dom_maybe_load_stylesheet(ctx, child_js);
    if (observers) notify_child_list(ctx, parent_js, child_js);
    return true;
}

/* Remove `child` for good (replaceChildren, textContent, innerHTML...): an
 * unwrapped removed subtree is freed at once. */
bool dom_api_remove_discard(JSContextHandle ctx, NodeId parent, NodeId child) {
    if (!dom_api_remove(ctx, parent, child)) return false;
    dom_free_if_unwrapped(child);
    return true;
}

bool dom_api_remove(JSContextHandle ctx, NodeId parent, NodeId child) {
    CP_ACC("DOM remove");
    if (!dom_valid(parent) || !dom_valid(child) || dom_parent(child) != parent) return false;
    /* Wrappers are only needed for callbacks (a wrapped node may carry
     * them) and mutation records. */
    GCValue parent_js = dom_js(ctx, parent);
    bool observers = mo_child_list_observed(ctx, parent_js);
    bool need_js = observers || dom_has_wrapper(child);
    GCValue child_js = need_js ? dom_js(ctx, child) : JS_NULL;
    if (dom_tag_is(child, "ytd-masthead") || dom_tag_is(child, "ytd-app")) {
        platform_log(LOG_LEVEL_WARN, "dom_api", "REMOVE %s from %s", dom_node_name(child), dom_node_name(parent));
        if (CYBER_ENV(CYBER_T_REMOVE)) {
            const char *trace_js = "(new Error('rm')).stack";
            GCValue tv = JS_Eval(ctx, trace_js, strlen(trace_js), "<rmtrace>", JS_EVAL_TYPE_GLOBAL);
            const char *ts = JS_ToCString(ctx, tv);
            fprintf(stderr, "[RM-TRACE] %s from %s:\n%s\n", dom_node_name(child), dom_node_name(parent), ts ? ts : "?");
            fflush(stderr);
        }
    }
    bool was_connected = dom_api_is_connected(ctx, parent);
    dom_detach(child, false);
    dom_request_layout();
    /* Every custom element of the removed subtree is disconnected, not just
     * its root (HTML "remove" steps), in tree order.  Components that clear
     * their rendering on disconnect then render once when reinserted. */
    if (was_connected) {
        uint32_t *ids = NULL, n = 0;
        collect_ce_candidates(child, &ids, &n);
        bool root_listed = false;
        for (uint32_t i = 0; i < n; i++) {
            if (ids[i] == child) root_listed = true;
            if (!dom_has_wrapper(ids[i])) continue;      /* never upgraded */
            invoke_custom_element_callback(ctx, dom_js(ctx, ids[i]), "disconnectedCallback");
        }
        free(ids);
        if (need_js && !root_listed) invoke_custom_element_callback(ctx, child_js, "disconnectedCallback");
    }
    dom_flush_ce_reactions(ctx);
    /* (The legacy DOM reported the removed node as added; kept for the
     * polyfills that were tuned against it.) */
    if (observers) notify_child_list(ctx, parent_js, child_js);
    return true;
}

/* Children of a fragment, as wrappers (for insertion of fragments). */
static GCValueVec collect_fragment_children(JSContextHandle ctx, NodeId frag) {
    GCValueVec children = {0};
    for (NodeId c = dom_first_child(frag); c; c = dom_next_sibling(c))
        gc_value_vec_push(&children, dom_js(ctx, c));
    return children;
}

// Real appendChild implementation
GCValue js_node_appendChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "appendChild: invalid argument");
    }
    GCValue child = argv[0];
    if (CYBER_ENV(CYBER_T_ROOTINS)) trace_root_insertion(ctx, this_val, child, "ROOT-INS");

    NodeId parent = dom_node_ensure(ctx, this_val, DOM_K_ELEMENT);
    if (!parent) return JS_ThrowTypeError(ctx, "appendChild: invalid parent node");
    if (JS_IsString(child)) {
        /* Plain strings were accepted by some legacy paths: make text. */
        size_t n = 0;
        const char *s = JS_ToCStringLen(ctx, &n, child);
        NodeId t = dom_create_chardata(DOM_K_TEXT, s ? s : "", s ? n : 0, dom_owner(parent));
        if (s) JS_FreeCString(ctx, s);
        child = dom_js(ctx, t);
    }
    NodeId c = dom_node_ensure(ctx, child, DOM_K_ELEMENT);
    if (!c) return JS_ThrowTypeError(ctx, "appendChild: invalid child node");

    // DocumentFragment: append its children, not the fragment itself.
    if (dom_kind(c) == DOM_K_FRAGMENT) {
        GCValueVec children GC_VALUE_VEC_AUTO = collect_fragment_children(ctx, c);
        for (size_t i = 0; i < children.count; i++) {
            GCValue args[1] = { children.data[i] };
            GCValue r = js_node_appendChild_real(ctx, this_val, 1, args);
            if (JS_IsException(r)) return r;
        }
        return child;
    }
    if (!dom_api_insert(ctx, parent, c, DOM_NULL)) return JS_EXCEPTION;
    return child;
}

// Real removeChild implementation
GCValue js_node_removeChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "removeChild: invalid argument");
    }
    GCValue child = argv[0];
    NodeId parent = dom_node_of(this_val);
    NodeId c = dom_node_of(child);
    if (!parent || !c) {
        // If no DOM data, just return the child (no-op for non-DOM nodes)
        return child;
    }
    if (dom_parent(c) != parent) {
        return throw_dom_exception(ctx, "NotFoundError", "Node is not a child of this node");
    }
    dom_api_remove(ctx, parent, c);
    return child;
}

// Convert a ParentNode method argument to a node.  DOM nodes are used as-is;
// strings become text nodes in the owner's document.
static GCValue parent_node_arg_to_node(JSContextHandle ctx, GCValue parent, GCValue arg) {
    if (JS_IsString(arg)) {
        size_t n = 0;
        const char *s = JS_ToCStringLen(ctx, &n, arg);
        NodeId p = dom_node_of(parent);
        NodeId t = dom_create_chardata(DOM_K_TEXT, s ? s : "", s ? n : 0, p ? dom_owner(p) : g_dom_document);
        if (s) JS_FreeCString(ctx, s);
        return dom_js(ctx, t);
    }
    if (JS_IsObject(arg)) return arg;
    return JS_NULL;
}

static void expand_nodes(JSContextHandle ctx, GCValue parent, int argc, GCValue *argv, GCValueVec *to_insert) {
    for (int i = 0; i < argc; i++) {
        GCValue node = parent_node_arg_to_node(ctx, parent, argv[i]);
        if (JS_IsNull(node) || JS_IsUndefined(node)) continue;
        NodeId n = dom_node_of(node);
        if (dom_kind(n) == DOM_K_FRAGMENT) {
            GCValueVec frag_children GC_VALUE_VEC_AUTO = collect_fragment_children(ctx, n);
            for (size_t j = 0; j < frag_children.count; j++) gc_value_vec_push(to_insert, frag_children.data[j]);
        } else {
            gc_value_vec_push(to_insert, node);
        }
    }
}

// Element/DocumentFragment.append(...nodes) and .prepend(...nodes)
static GCValue js_parent_node_append_or_prepend(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, bool prepend) {
    if (argc < 1) return JS_UNDEFINED;
    GCValueVec to_insert GC_VALUE_VEC_AUTO = {0};
    expand_nodes(ctx, this_val, argc, argv, &to_insert);
    if (to_insert.count == 0) return JS_UNDEFINED;
    if (prepend) {
        GCValue first = js_node_get_firstChild(ctx, this_val, 0, NULL);
        for (size_t i = 0; i < to_insert.count; i++) {
            GCValue node = to_insert.data[i];
            GCValue args[2] = { node, first };
            GCValue r = js_node_insertBefore_real(ctx, this_val, 2, args);
            if (JS_IsException(r)) return r;
        }
    } else {
        for (size_t i = 0; i < to_insert.count; i++) {
            GCValue args[1] = { to_insert.data[i] };
            GCValue r = js_node_appendChild_real(ctx, this_val, 1, args);
            if (JS_IsException(r)) return r;
        }
    }
    return JS_UNDEFINED;
}

GCValue js_element_append(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_parent_node_append_or_prepend(ctx, this_val, argc, argv, false);
}

GCValue js_element_prepend(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_parent_node_append_or_prepend(ctx, this_val, argc, argv, true);
}

GCValue js_element_replaceChildren(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    NodeId p = dom_node_of(this_val);
    while (p && dom_first_child(p)) {
        if (!dom_api_remove_discard(ctx, p, dom_first_child(p))) break;
    }
    return js_parent_node_append_or_prepend(ctx, this_val, argc, argv, false);
}

// ChildNode methods: before/after/replaceWith
static GCValue js_child_node_insert(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int mode) {
    if (argc < 1) return JS_UNDEFINED;
    GCValue parent = JS_GetPropertyStr(ctx, this_val, "parentNode");
    if (JS_IsNull(parent) || JS_IsUndefined(parent) || !JS_IsObject(parent)) return JS_UNDEFINED;
    GCValue ref = this_val;
    if (mode == 1) ref = js_node_get_nextSibling(ctx, this_val, 0, NULL);
    GCValueVec to_insert GC_VALUE_VEC_AUTO = {0};
    expand_nodes(ctx, parent, argc, argv, &to_insert);
    for (size_t i = 0; i < to_insert.count; i++) {
        GCValue args[2] = { to_insert.data[i], ref };
        GCValue r = js_node_insertBefore_real(ctx, parent, 2, args);
        if (JS_IsException(r)) return r;
    }
    if (mode == 2) {
        GCValue rem_args[1] = { this_val };
        js_node_removeChild_real(ctx, parent, 1, rem_args);
    }
    return JS_UNDEFINED;
}

GCValue js_element_before(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_child_node_insert(ctx, this_val, argc, argv, 0);
}

GCValue js_element_after(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_child_node_insert(ctx, this_val, argc, argv, 1);
}

GCValue js_element_replaceWith(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_child_node_insert(ctx, this_val, argc, argv, 2);
}

// Real insertBefore implementation
GCValue js_node_insertBefore_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "insertBefore: invalid arguments");
    }
    GCValue new_child = argv[0];
    /* WebIDL: refChild is `Node?` — undefined converts to null (append at end). */
    GCValue ref_child = JS_IsUndefined(argv[1]) ? JS_NULL : argv[1];
    if (CYBER_ENV(CYBER_T_ROOTINS)) trace_root_insertion(ctx, this_val, new_child, "ROOT-INS-BEFORE");

    NodeId parent = dom_node_ensure(ctx, this_val, DOM_K_ELEMENT);
    if (!parent) return JS_ThrowTypeError(ctx, "insertBefore: invalid parent node");
    NodeId c = dom_node_ensure(ctx, new_child, DOM_K_ELEMENT);
    if (!c) return JS_ThrowTypeError(ctx, "insertBefore: invalid new child node");

    NodeId ref = DOM_NULL;
    if (!JS_IsNull(ref_child)) {
        ref = dom_node_of(ref_child);
        if (!ref) {
            if (CYBER_ENV(CYBER_T_INS)) {
                const char *trace_js = "(new Error('ib')).stack";
                GCValue tv = JS_Eval(ctx, trace_js, strlen(trace_js), "<ibtrace>", JS_EVAL_TYPE_GLOBAL);
                const char *ts = JS_ToCString(ctx, tv);
                fprintf(stderr, "[INS-NOTFOUND] parent=%s:\n%s\n", dom_node_name(parent), ts ? ts : "?");
                fflush(stderr);
                JS_FreeCString(ctx, ts);
            }
            return throw_dom_exception(ctx, "NotFoundError", "Reference node not found");
        }
        if (dom_parent(ref) != parent) {
            return throw_dom_exception(ctx, "NotFoundError", "Reference node is not a child of this node");
        }
    }

    // DocumentFragment: insert its children before ref_child, not the fragment itself.
    if (dom_kind(c) == DOM_K_FRAGMENT) {
        GCValueVec children GC_VALUE_VEC_AUTO = collect_fragment_children(ctx, c);
        for (size_t i = 0; i < children.count; i++) {
            GCValue args[2] = { children.data[i], ref_child };
            GCValue r = js_node_insertBefore_real(ctx, this_val, 2, args);
            if (JS_IsException(r)) return r;
        }
        return new_child;
    }
    if (ref == c) return new_child;
    if (!dom_api_insert(ctx, parent, c, ref)) return JS_EXCEPTION;
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
    if (JS_StrictEq(ctx, new_child, old_child)) return old_child;
    NodeId parent = dom_node_of(this_val);
    NodeId old_node = dom_node_of(old_child);
    if (!parent || !old_node) return throw_dom_exception(ctx, "NotFoundError", "Node not found");
    if (dom_parent(old_node) != parent)
        return throw_dom_exception(ctx, "NotFoundError", "Node is not a child of this node");
    GCValue insert_args[2] = { new_child, old_child };
    GCValue r = js_node_insertBefore_real(ctx, this_val, 2, insert_args);
    if (JS_IsException(r)) return r;
    GCValue remove_args[1] = { old_child };
    js_node_removeChild_real(ctx, this_val, 1, remove_args);
    return old_child;
}

// Real normalize implementation: merge adjacent text nodes and remove empty ones.
GCValue js_node_normalize_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId parent = dom_node_of(this_val);
    if (!parent) return JS_UNDEFINED;
    NodeId child = dom_first_child(parent);
    while (child) {
        NodeId next = dom_next_sibling(child);
        if (dom_kind(child) != DOM_K_TEXT) {
            GCValue cj = dom_js(ctx, child);
            js_node_normalize_real(ctx, cj, 0, NULL);
            child = next;
            continue;
        }
        if (next && dom_kind(next) == DOM_K_TEXT) {
            uint32_t l1 = 0, l2 = 0;
            const char *t1 = dom_text(child, &l1);
            const char *t2 = dom_text(next, &l2);
            char *merged = (char *)malloc(l1 + l2 + 1);
            memcpy(merged, t1, l1);
            memcpy(merged + l1, t2, l2);
            dom_set_text(child, merged, l1 + l2);
            free(merged);
            dom_api_remove_discard(ctx, parent, next);
            continue;
        }
        uint32_t len = 0;
        dom_text(child, &len);
        if (len == 0) {
            dom_api_remove_discard(ctx, parent, child);
            child = next;
            continue;
        }
        child = next;
    }
    return JS_UNDEFINED;
}

// Real compareDocumentPosition implementation
GCValue js_node_compareDocumentPosition_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "compareDocumentPosition: invalid argument");
    }
    GCValue other = argv[0];
    if (JS_StrictEq(ctx, this_val, other)) return JS_NewInt32(ctx, 0);
    NodeId a = dom_node_of(this_val), b = dom_node_of(other);
    if (!a || !b || dom_root(a) != dom_root(b))
        return JS_NewInt32(ctx, 1 /* DISCONNECTED */ | 32 /* IMPLEMENTATION_SPECIFIC */);
    if (dom_contains(a, b)) return JS_NewInt32(ctx, 4 /* FOLLOWING */ | 16 /* CONTAINED_BY */);
    if (dom_contains(b, a)) return JS_NewInt32(ctx, 2 /* PRECEDING */ | 8 /* CONTAINS */);
    /* Children of the common ancestor on each side. */
    NodeId ca = a, cb = b;
    for (NodeId x = a; x; x = dom_parent(x)) {
        for (NodeId y = b; y; y = dom_parent(y)) {
            if (dom_parent(x) && dom_parent(x) == dom_parent(y)) { ca = x; cb = y; goto found; }
        }
    }
    return JS_NewInt32(ctx, 1 | 32);
found:
    for (NodeId cur = ca; cur; cur = dom_next_sibling(cur))
        if (cur == cb) return JS_NewInt32(ctx, 4 /* FOLLOWING */);
    return JS_NewInt32(ctx, 2 /* PRECEDING */);
}

/* Copy a node (and, when deep, its subtree) inside the store.  Shares the
 * strings of the original (arena blocks are retained). */
static NodeId clone_node(NodeId src, bool deep, NodeId owner);

static NodeId clone_one(NodeId src, NodeId owner) {
    uint32_t s = dom_slot(src);
    uint32_t d = dom_alloc_slot(g_dom.kind[s], owner);
    g_dom.tag[d] = g_dom.tag[s];
    g_dom.flags[d] = (uint16_t)(g_dom.flags[s] & (DOMF_CE_CANDIDATE | DOMF_RAW_TEXT | DOMF_VOID | DOMF_SELF_CLOSED));
    g_dom.id_atom[d] = g_dom.id_atom[s];
    if (g_dom.text[s].len) g_dom.text[d] = dom_str_dup(g_dom.text[s].s, g_dom.text[s].len);
    DomAttrList *l = &g_dom.attrs[s];
    for (int i = 0; i < l->n; i++) {
        DomStr v = g_dom.attrs[s].a[i].value;
        dom_set_attr(dom_id_of(d), g_dom.attrs[s].a[i].name, v.s ? v.s : "", v.len);
    }
    return dom_id_of(d);
}

static NodeId clone_node(NodeId src, bool deep, NodeId owner) {
    NodeId root = clone_one(src, owner);
    NodeId content = dom_template_content(src);
    if (content) {
        NodeId cc = clone_node(content, true, owner);
        dom_set_template_content(root, cc);
    }
    if (!deep) return root;
    for (NodeId c = dom_first_child(src); c; c = dom_next_sibling(c)) {
        NodeId cc = clone_node(c, true, owner);
        dom_attach(root, cc, DOM_NULL);
    }
    return root;
}

/* Inline styles set through element.style live in the wrapper's style
 * object, not in the store's style attribute; a clone copies attributes, so
 * carry them over too (CSSOM reflects the declarations into the attribute,
 * and a clone has them).  `dst` mirrors `src`'s structure. */
static void clone_copy_styles(JSContextHandle ctx, NodeId src, NodeId dst) {
    if (dom_kind(src) == DOM_K_ELEMENT && dom_has_wrapper(src)) {
        GCValue style = dom_element_style(ctx, dom_js_existing(src), false);
        char *css = html_style_object_css_text(ctx, style);
        if (css) {
            dom_set_attr_str(dst, "style", css);
            free(css);
        }
    }
    NodeId a = dom_first_child(src), b = dom_first_child(dst);
    for (; a && b; a = dom_next_sibling(a), b = dom_next_sibling(b))
        clone_copy_styles(ctx, a, b);
}

// Real cloneNode implementation
GCValue js_node_cloneNode_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    bool deep = argc > 0 && JS_ToBool(ctx, argv[0]);
    NodeId original = dom_node_of(this_val);
    if (!original) return JS_NewObject(ctx);
    /* The clone belongs to the same document as the original (DOM spec:
     * cloneNode keeps the node document; importNode re-documents it). */
    NodeId copy = clone_node(original, deep, dom_owner(original));
    if (deep) clone_copy_styles(ctx, original, copy);
    else if (dom_kind(original) == DOM_K_ELEMENT && dom_has_wrapper(original)) {
        char *css = html_style_object_css_text(ctx, dom_element_style(ctx, dom_js_existing(original), false));
        if (css) { dom_set_attr_str(copy, "style", css); free(css); }
    }
    return dom_js(ctx, copy);
}

// Real contains implementation
GCValue js_node_contains_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) return JS_FALSE;
    if (JS_StrictEq(ctx, this_val, argv[0])) return JS_TRUE;
    NodeId a = dom_node_of(this_val), b = dom_node_of(argv[0]);
    return JS_NewBool(ctx, a && b && dom_contains(a, b));
}

// Real getRootNode implementation
GCValue js_node_getRootNode_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    bool composed = false;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        GCValue v = JS_GetPropertyStr(ctx, argv[0], "composed");
        composed = JS_ToBool(ctx, v);
    }
    NodeId n = dom_node_of(this_val);
    if (!n) return this_val;
    for (int guard = 0; guard < 64; guard++) {
        NodeId r = dom_root(n);
        if (!composed) return dom_js(ctx, r);
        NodeId h = root_host(ctx, r);
        if (!h) return dom_js(ctx, r);
        n = h;
    }
    return dom_js(ctx, dom_root(n));
}

// Tree navigation property getters
GCValue js_node_get_firstChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return dom_js_first(ctx, dom_node_of(this_val));
}

GCValue js_node_get_lastChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return dom_js_last(ctx, dom_node_of(this_val));
}

GCValue js_node_get_nextSibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return dom_js_next(ctx, dom_node_of(this_val));
}

GCValue js_node_get_previousSibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return dom_js_prev(ctx, dom_node_of(this_val));
}

/* Set both the store's owner document and the JS ownerDocument property. */
void dom_node_set_owner_document(JSContextHandle ctx, GCValue node, GCValue doc) {
    NodeId n = dom_node_of(node);
    NodeId d = dom_node_of(doc);
    if (n && dom_kind(d) == DOM_K_DOCUMENT) g_dom.owner[dom_slot(n)] = dom_slot(d);
    // Define an own writable data property so polyfills that assign to
    // ownerDocument do not trip over the read-only getter on Node.prototype.
    JS_DefinePropertyValueStr(ctx, node, "ownerDocument", doc,
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

GCValue js_node_get_ownerDocument(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    if (!n || dom_kind(n) == DOM_K_DOCUMENT) return JS_NULL;
    NodeId d = dom_owner(n);
    if (!d) d = g_dom_document;
    return dom_js(ctx, d);
}

/* Setter: polyfills assign ownerDocument; keep the value as an own property. */
GCValue js_node_set_ownerDocument(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    JS_DefinePropertyValueStr(ctx, this_val, "ownerDocument", argv[0], JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    return JS_UNDEFINED;
}

GCValue js_node_get_parentNode(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return dom_js_parent(ctx, dom_node_of(this_val));
}

GCValue js_node_get_parentElement(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId p = dom_parent(dom_node_of(this_val));
    return dom_kind(p) == DOM_K_ELEMENT ? dom_js(ctx, p) : JS_NULL;
}

GCValue js_node_get_childNodes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue arr = JS_NewArray(ctx);
    NodeId n = dom_node_of(this_val);
    uint32_t idx = 0;
    for (NodeId c = dom_first_child(n); c; c = dom_next_sibling(c))
        JS_SetPropertyUint32(ctx, arr, idx++, dom_js(ctx, c));
    return arr;
}

GCValue js_node_get_nodeType(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    if (!n) return JS_NewInt32(ctx, DOM_NODE_TYPE_ELEMENT);  // Default to element
    return JS_NewInt32(ctx, dom_kind(n));
}

GCValue js_node_get_nodeName(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    if (!n) {
        GCValue tagName = JS_GetPropertyStr(ctx, this_val, "tagName");
        if (!JS_IsUndefined(tagName)) return tagName;
        return JS_NewString(ctx, "DIV");
    }
    return JS_NewString(ctx, dom_node_name(n));
}

// Element.prototype.tagName getter
GCValue js_element_get_tagName(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    if (dom_kind(n) != DOM_K_ELEMENT) return JS_NewString(ctx, "DIV");
    return JS_NewString(ctx, dom_node_name(n));
}

/* Element.localName: the tag name in lowercase.  Polymer's template parser
 * and the ShadyDOM polyfill detect <slot> elements via `localName === "slot"`. */
GCValue js_element_get_localName(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    return JS_NewString(ctx, dom_kind(n) == DOM_K_ELEMENT ? dom_local_name(n) : "div");
}

static NodeId first_element(NodeId n) { while (n && dom_kind(n) != DOM_K_ELEMENT) n = dom_next_sibling(n); return n; }
static NodeId last_element(NodeId n) { while (n && dom_kind(n) != DOM_K_ELEMENT) n = dom_prev_sibling(n); return n; }

GCValue js_element_get_firstElementChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return dom_js(ctx, first_element(dom_first_child(dom_node_of(this_val))));
}

GCValue js_element_get_lastElementChild(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return dom_js(ctx, last_element(dom_last_child(dom_node_of(this_val))));
}

GCValue js_element_get_nextElementSibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return dom_js(ctx, first_element(dom_next_sibling(dom_node_of(this_val))));
}

GCValue js_element_get_previousElementSibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return dom_js(ctx, last_element(dom_prev_sibling(dom_node_of(this_val))));
}

GCValue js_element_get_childElementCount(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    int count = 0;
    for (NodeId c = dom_first_child(dom_node_of(this_val)); c; c = dom_next_sibling(c))
        if (dom_kind(c) == DOM_K_ELEMENT) count++;
    return JS_NewInt32(ctx, count);
}

GCValue js_element_get_children(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    for (NodeId c = dom_first_child(dom_node_of(this_val)); c; c = dom_next_sibling(c))
        if (dom_kind(c) == DOM_K_ELEMENT) JS_SetPropertyUint32(ctx, arr, idx++, dom_js(ctx, c));
    return arr;
}

// Element.prototype.style getter / setter
GCValue js_element_get_style(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    if (n && dom_kind(n) != DOM_K_ELEMENT) return JS_UNDEFINED;
    return dom_element_style(ctx, this_val, true);
}

/* [PutForwards=cssText]: any value is converted to a string and assigned to
 * style.cssText, as in browsers (el.style = null sets cssText "null"). */
GCValue js_element_set_style(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    NodeId n = dom_node_of(this_val);
    if (n && dom_kind(n) != DOM_K_ELEMENT) return JS_UNDEFINED;
    GCValue text = JS_ToString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(text)) return text;
    GCValue style = dom_element_style(ctx, this_val, true);
    if (JS_IsObject(style) && JS_SetPropertyStr(ctx, style, "cssText", text) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

// Element.prototype.getElementsByTagName
GCValue js_element_get_elements_by_tag_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NewArray(ctx);
    size_t n = 0;
    const char *tag_name = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!tag_name) return JS_NewArray(ctx);
    DomNodeList l = dom_query_by_tag_in(dom_node_of(this_val), tag_name, n, false);
    JS_FreeCString(ctx, tag_name);
    GCValue arr = nodes_to_array(ctx, l.ids, l.n);
    dom_node_list_free(&l);
    return arr;
}

// Document.prototype.getElementsByTagName
GCValue js_document_get_elements_by_tag_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NewArray(ctx);
    size_t n = 0;
    const char *tag_name = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!tag_name) return JS_NewArray(ctx);
    GCValue arr = JS_NewArray(ctx);
    // Special cases: 'head' and 'body' answer document.head / document.body.
    if (strcasecmp(tag_name, "head") == 0 || strcasecmp(tag_name, "body") == 0) {
        GCValue el = JS_GetPropertyStr(ctx, this_val, strcasecmp(tag_name, "head") == 0 ? "head" : "body");
        if (!JS_IsUndefined(el) && !JS_IsNull(el)) JS_SetPropertyUint32(ctx, arr, 0, el);
        JS_FreeCString(ctx, tag_name);
        return arr;
    }
    NodeId doc = dom_node_of(this_val);
    if (dom_kind(doc) != DOM_K_DOCUMENT) doc = g_dom_document;
    DomNodeList l = dom_query_by_tag_in(doc, tag_name, n, false);
    JS_FreeCString(ctx, tag_name);
    for (uint32_t i = 0; i < l.n; i++) JS_SetPropertyUint32(ctx, arr, i, dom_js(ctx, l.ids[i]));
    dom_node_list_free(&l);
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
    dom_request_relayout();
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
    if (!JS_IsArray(ctx, adopted)) adopted = JS_NewArray(ctx);
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
    if (JS_IsObject(ua)) JS_SetPropertyUint32(ctx, result, idx++, ua);
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

// Document.prototype.getElementsByClassName
GCValue js_document_getElementsByClassName(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NewArray(ctx);
    size_t n = 0;
    const char *class_name = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!class_name || !class_name[0]) return JS_NewArray(ctx);
    NodeId root = dom_node_of(this_val);
    if (!root) root = g_dom_document;
    DomNodeList l = dom_query_by_class_in(root, class_name, n);
    JS_FreeCString(ctx, class_name);
    GCValue arr = nodes_to_array(ctx, l.ids, l.n);
    dom_node_list_free(&l);
    return arr;
}

/* ============================================================================
 * Attributes
 * ============================================================================ */

/* Copy of an attribute value (the store may change under JS callbacks). */
static char *attr_dup(NodeId n, const char *name) {
    const char *v = dom_attr(n, name);
    return v ? strdup(v) : NULL;
}

static void attr_changed(JSContextHandle ctx, GCValue el, NodeId n, const char *name,
                         const char *old_value, const char *new_value) {
    (void)n;
    dom_mark_dirty(n, DOMF_DIRTY_ATTR);
    dom_query_note_attr(n, name);
    invoke_attribute_changed(ctx, el, name, old_value, new_value);
    mo_notify_attribute(ctx, el, name, old_value ? old_value : "");
    dom_request_layout();
}

// Element.prototype.setAttribute
GCValue js_element_set_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "requires at least 2 argument(s)");
    const char *name = JS_ToCString(ctx, argv[0]);
    size_t vlen = 0;
    const char *value = JS_ToCStringLen(ctx, &vlen, argv[1]);
    if (name && value) {
        NodeId n = dom_node_of(this_val);
        char *old_value = n ? attr_dup(n, name) : NULL;
        // Store the attribute on the object itself too (the legacy DOM's
        // reflection), except for names with a reflecting accessor.
        if (strcmp(name, "id") != 0 && strcmp(name, "class") != 0 && strcmp(name, "style") != 0 &&
            !(n && dom_attr_has_accessor(n, name))) {
            JS_SetPropertyStr(ctx, this_val, name, JS_NewStringLen(ctx, value, vlen));
        }
        n = dom_node_of(this_val);
        if (n) {
            css_atom_reserve(1);
            dom_set_attr(n, css_atom_intern(name, strlen(name)), value, vlen);
            if (strcasecmp(name, "style") == 0) {
                /* The attribute replaces the live style object's declarations. */
                GCValue style = dom_element_style(ctx, this_val, false);
                if (JS_IsObject(style))
                    JS_SetPropertyStr(ctx, style, "cssText", JS_NewStringLen(ctx, value, vlen));
            }
        }
        if (strcmp(name, "src") == 0 && value[0]) {
            capture_url_debug(value, "element_setAttribute_src");
            if (g_dom_dynamic_scripts_enabled && dom_tag_is(n, "script"))
                dom_execute_external_script(ctx, this_val, value);
        }
        attr_changed(ctx, this_val, n, name, old_value, value);
        free(old_value);
    }
    if (name) JS_FreeCString(ctx, name);
    if (value) JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}

// Element.prototype.id / className — reflected content attributes.
GCValue js_element_get_id(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    const char *val = dom_attr(dom_node_of(this_val), "id");
    return JS_NewString(ctx, val ? val : "");
}

static GCValue set_reflected(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, const char *attr) {
    if (argc < 1) return JS_UNDEFINED;
    size_t vlen = 0;
    const char *v = JS_ToCStringLen(ctx, &vlen, argv[0]);
    if (!v) return JS_UNDEFINED;
    NodeId n = dom_node_of(this_val);
    if (n) {
        char *old = attr_dup(n, attr);
        dom_set_attr_str(n, attr, v);
        attr_changed(ctx, this_val, n, attr, old, v);
        free(old);
    }
    JS_FreeCString(ctx, v);
    return JS_UNDEFINED;
}

GCValue js_element_set_id(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return set_reflected(ctx, this_val, argc, argv, "id");
}

GCValue js_element_get_class_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    const char *val = dom_attr(dom_node_of(this_val), "class");
    return JS_NewString(ctx, val ? val : "");
}

GCValue js_element_set_class_name(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return set_reflected(ctx, this_val, argc, argv, "class");
}

// Element.prototype.getAttribute
GCValue js_element_get_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;
    NodeId n = dom_node_of(this_val);
    if (n) {
        uint32_t len = 0;
        CssAtom a = css_atom_find(name, strlen(name));
        const char *val = a ? dom_get_attr(n, a, &len) : NULL;
        if (val) {
            JS_FreeCString(ctx, name);
            return JS_NewStringLen(ctx, val, len);
        }
    }
    // Fall back to a string-valued own data property for `id` only (el.id
    // set directly).  Other own properties (e.g. Polymer's `is`) are not
    // attributes.  Never run accessors here: a reflecting getter such as
    // HTMLAnchorElement.prototype.href calls getAttribute() itself.
    if (strcmp(name, "id") != 0) {
        JS_FreeCString(ctx, name);
        return JS_NULL;
    }
    JSAtom atom = JS_NewAtom(ctx, name);
    JS_FreeCString(ctx, name);
    JSPropertyDescriptor desc = JS_PROPERTY_DESCRIPTOR_INIT;
    int has = JS_GetOwnProperty(ctx, &desc, this_val, atom);
    JS_FreeAtom(ctx, atom);
    if (has > 0 && !(desc.flags & JS_PROP_GETSET) && JS_IsString(desc.value))
        return desc.value;
    return JS_NULL;
}

// Element.prototype.removeAttribute
GCValue js_element_remove_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (name) {
        NodeId n = dom_node_of(this_val);
        char *old_value = n ? attr_dup(n, name) : NULL;
        if (strcmp(name, "id") != 0 && strcmp(name, "class") != 0 && strcmp(name, "style") != 0) {
            JSAtom atom = JS_NewAtom(ctx, name);
            JS_DeleteProperty(ctx, this_val, atom, 0);
            JS_FreeAtom(ctx, atom);
        }
        n = dom_node_of(this_val);
        if (n) {
            CssAtom a = css_atom_find(name, strlen(name));
            if (a) dom_remove_attr(n, a);
            if (strcasecmp(name, "style") == 0) {
                GCValue style = dom_element_style(ctx, this_val, false);
                if (JS_IsObject(style)) JS_SetPropertyStr(ctx, style, "cssText", JS_NewString(ctx, ""));
            }
        }
        invoke_attribute_changed(ctx, this_val, name, old_value, NULL);
        mo_notify_attribute(ctx, this_val, name, old_value ? old_value : "");
        if (n) { dom_mark_dirty(n, DOMF_DIRTY_ATTR); dom_query_note_attr(n, name); }
        free(old_value);
        JS_FreeCString(ctx, name);
    }
    dom_request_layout();
    return JS_UNDEFINED;
}

// Element.prototype.hasAttribute
GCValue js_element_has_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_FALSE;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_FALSE;
    NodeId n = dom_node_of(this_val);
    bool has = n && dom_attr(n, name) != NULL;
    if (!has && strcmp(name, "id") == 0) {
        GCValue val = JS_GetPropertyStr(ctx, this_val, name);
        has = !JS_IsUndefined(val) && JS_IsString(val);
    }
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, has);
}

// Element.prototype.toggleAttribute
GCValue js_element_toggle_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_FALSE;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_FALSE;
    NodeId n = dom_node_of(this_val);
    bool has = n && dom_attr(n, name) != NULL;
    bool want = argc > 1 && !JS_IsUndefined(argv[1]) ? JS_ToBool(ctx, argv[1]) : !has;
    GCValue name_v = JS_NewString(ctx, name);
    JS_FreeCString(ctx, name);
    if (want && !has) {
        GCValue args[2] = { name_v, JS_NewString(ctx, "") };
        js_element_set_attribute(ctx, this_val, 2, args);
    } else if (!want && has) {
        GCValue args[1] = { name_v };
        js_element_remove_attribute(ctx, this_val, 1, args);
    }
    dom_request_layout();
    return JS_NewBool(ctx, want);
}

// Element.prototype.setAttributeNS
GCValue js_element_set_attribute_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "setAttributeNS requires 3 arguments");
    return js_element_set_attribute(ctx, this_val, argc - 1, argv + 1);
}

// Element.prototype.getAttributeNS
GCValue js_element_get_attribute_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_NULL;
    return js_element_get_attribute(ctx, this_val, argc - 1, argv + 1);
}

// Element.prototype.removeAttributeNS
GCValue js_element_remove_attribute_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    return js_element_remove_attribute(ctx, this_val, argc - 1, argv + 1);
}

// Document.getElementById
GCValue js_document_getElementById(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    size_t n = 0;
    const char *id = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!id || !*id) { if (id) JS_FreeCString(ctx, id); return JS_NULL; }
    NodeId doc = dom_node_of(this_val);
    if (dom_kind(doc) != DOM_K_DOCUMENT) doc = g_dom_document;
    NodeId found = dom_query_by_id(doc, id, n);
    JS_FreeCString(ctx, id);
    return dom_js(ctx, found);
}

/* querySelector[All] over a scope (§7 "Queries"): compiled selectors, index
 * candidates, document order. */
static GCValue query_first(JSContextHandle ctx, NodeId scope, GCValue sel, bool include_self) {
    CP_ACC("querySelector");
    size_t n = 0;
    const char *s = JS_ToCStringLen(ctx, &n, sel);
    if (!s) return JS_NULL;
    NodeId r = dom_query_first(scope, s, n, include_self);
    JS_FreeCString(ctx, s);
    return dom_js(ctx, r);
}

static GCValue query_all(JSContextHandle ctx, NodeId scope, GCValue sel, bool include_self) {
    CP_ACC("querySelectorAll");
    size_t n = 0;
    const char *s = JS_ToCStringLen(ctx, &n, sel);
    if (!s) return JS_NewArray(ctx);
    DomNodeList l = dom_query_all(scope, s, n, include_self);
    JS_FreeCString(ctx, s);
    GCValue arr = nodes_to_array(ctx, l.ids, l.n);
    dom_node_list_free(&l);
    return arr;
}

// Document.querySelector
GCValue js_document_querySelector(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    GCValue doc_elem = JS_GetPropertyStr(ctx, this_val, "documentElement");
    if (JS_IsNull(doc_elem) || JS_IsUndefined(doc_elem)) return JS_NULL;
    /* null when nothing matches.  (This used to return a fake <div>, which
     * made "is there a continuation trigger?" checks always true.) */
    return query_first(ctx, dom_node_of(doc_elem), argv[0], true);
}

// Document.querySelectorAll
GCValue js_document_querySelectorAll(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NewArray(ctx);
    GCValue doc_elem = JS_GetPropertyStr(ctx, this_val, "documentElement");
    if (JS_IsNull(doc_elem) || JS_IsUndefined(doc_elem)) return JS_NewArray(ctx);
    return query_all(ctx, dom_node_of(doc_elem), argv[0], true);
}

// Element.prototype.click
GCValue js_element_click(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

// Element.prototype.getAnimations
GCValue js_element_get_animations(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewArray(ctx);
}

bool matches_selector(JSContextHandle ctx, GCValue elem, const char* selector) {
    (void)ctx;
    if (!selector) return false;
    return dom_query_matches(dom_node_of(elem), selector, strlen(selector));
}

// Real querySelector implementation (the legacy matcher included the scope
// element itself; kept for compatibility).
GCValue js_element_querySelector_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    return query_first(ctx, dom_node_of(this_val), argv[0], false);
}

GCValue js_element_querySelectorAll_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NewArray(ctx);
    return query_all(ctx, dom_node_of(this_val), argv[0], false);
}

GCValue query_selector_recursive(JSContextHandle ctx, GCValue elem, const char* selector) {
    NodeId r = dom_query_first(dom_node_of(elem), selector, strlen(selector), true);
    return dom_js(ctx, r);
}

void query_selector_all_recursive(JSContextHandle ctx, GCValue elem, const char* selector, GCValue result_arr, int* idx) {
    DomNodeList l = dom_query_all(dom_node_of(elem), selector, strlen(selector), true);
    for (uint32_t i = 0; i < l.n; i++) JS_SetPropertyUint32(ctx, result_arr, (uint32_t)(*idx)++, dom_js(ctx, l.ids[i]));
    dom_node_list_free(&l);
}

// ============================================================================
// Element Content Getters/Setters
// ============================================================================

// classList getter - returns a functional DOMTokenList backed by className
GCValue js_element_get_classList(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue existing = JS_GetPropertyStr(ctx, this_val, "__classList");
    if (!JS_IsUndefined(existing) && !JS_IsNull(existing)) return existing;
    GCValue classList = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, classList, "__element", this_val);
    JS_SetPropertyStr(ctx, classList, "add", JS_NewCFunction(ctx, js_dom_token_list_add, "add", 1));
    JS_SetPropertyStr(ctx, classList, "remove", JS_NewCFunction(ctx, js_dom_token_list_remove, "remove", 1));
    JS_SetPropertyStr(ctx, classList, "toggle", JS_NewCFunction(ctx, js_dom_token_list_toggle, "toggle", 2));
    JS_SetPropertyStr(ctx, classList, "contains", JS_NewCFunction(ctx, js_dom_token_list_contains, "contains", 1));
    JS_SetPropertyStr(ctx, classList, "supports", JS_NewCFunction(ctx, js_dom_token_list_supports, "supports", 1));
    JS_SetPropertyStr(ctx, classList, "item", JS_NewCFunction(ctx, js_dom_token_list_item, "item", 1));
    JS_SetPropertyStr(ctx, classList, "forEach", JS_NewCFunction(ctx, js_dom_token_list_for_each, "forEach", 1));
    /* Iterable (for...of, spread), as DOMTokenList is. */
    GCValue values = JS_NewCFunction(ctx, js_dom_token_list_values, "values", 0);
    JS_SetPropertyStr(ctx, classList, "values", values);
    JS_SetProperty(ctx, classList, dom_iterator_atom(ctx), values);
    GCValue length_getter = JS_NewCFunction(ctx, js_dom_token_list_get_length, "get length", 0);
    JSAtom length_atom = JS_NewAtom(ctx, "length");
    JS_DefinePropertyGetSet(ctx, classList, length_atom, length_getter, JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, length_atom);
    JS_SetPropertyStr(ctx, this_val, "__classList", classList);
    return classList;
}

// dataset getter - returns DOMStringMap stub
GCValue js_element_get_dataset(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue existing = JS_GetPropertyStr(ctx, this_val, "__dataset");
    if (!JS_IsUndefined(existing) && !JS_IsNull(existing)) return existing;
    GCValue dataset = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, this_val, "__dataset", dataset);
    return dataset;
}

/* ---- serialization (innerHTML / outerHTML getters, XMLSerializer) ---- */

typedef struct { char *data; size_t len, cap; } SBuf;
static void sb_put(SBuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        while (cap < b->len + n + 1) cap *= 2;
        b->data = realloc(b->data, cap);
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}
static void sb_str(SBuf *b, const char *s) { sb_put(b, s, strlen(s)); }
static void sb_escape(SBuf *b, const char *s, size_t n, bool attr) {
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        const char *rep = NULL;
        switch (s[i]) {
        case '&': rep = "&amp;"; break;
        case '<': rep = attr ? NULL : "&lt;"; break;
        case '>': rep = attr ? NULL : "&gt;"; break;
        case '"': rep = attr ? "&quot;" : NULL; break;
        case '\xa0': break;
        }
        if (!rep) continue;
        sb_put(b, s + run, i - run);
        sb_str(b, rep);
        run = i + 1;
    }
    sb_put(b, s + run, n - run);
}

static void serialize_node(SBuf *b, NodeId n);

static void serialize_children(SBuf *b, NodeId n) {
    NodeId src = dom_template_content(n) ? dom_template_content(n) : n;
    for (NodeId c = dom_first_child(src); c; c = dom_next_sibling(c)) serialize_node(b, c);
}

static void serialize_node(SBuf *b, NodeId n) {
    uint32_t len = 0;
    switch (dom_kind(n)) {
    case DOM_K_TEXT: {
        const char *t = dom_text(n, &len);
        NodeId p = dom_parent(n);
        if (p && (dom_flags(p) & DOMF_RAW_TEXT)) sb_put(b, t, len);
        else sb_escape(b, t, len, false);
        break;
    }
    case DOM_K_COMMENT: {
        const char *t = dom_text(n, &len);
        sb_str(b, "<!--");
        sb_put(b, t, len);
        sb_str(b, "-->");
        break;
    }
    case DOM_K_ELEMENT: {
        const char *tag = dom_local_name(n);
        sb_str(b, "<");
        sb_str(b, tag);
        for (int i = 0; i < dom_attr_count(n); i++) {
            const DomAttr *a = dom_attr_at(n, i);
            sb_str(b, " ");
            sb_str(b, dom_attr_name_for_output(css_atom_str(a->name)));
            sb_str(b, "=\"");
            sb_escape(b, a->value.s ? a->value.s : "", a->value.len, true);
            sb_str(b, "\"");
        }
        sb_str(b, ">");
        if (dom_flags(n) & DOMF_VOID) break;
        serialize_children(b, n);
        sb_str(b, "</");
        sb_str(b, tag);
        sb_str(b, ">");
        break;
    }
    case DOM_K_DOCUMENT:
    case DOM_K_FRAGMENT:
        serialize_children(b, n);
        break;
    default:
        break;
    }
}

char *dom_serialize(JSContextHandle ctx, NodeId n, bool outer, size_t *len_out) {
    CP_ACC("innerHTML+outerHTML get");
    (void)ctx;
    SBuf b = { 0 };
    sb_put(&b, "", 0);
    if (outer) serialize_node(&b, n);
    else serialize_children(&b, n);
    if (len_out) *len_out = b.len;
    return b.data;
}

GCValue js_element_get_inner_html(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    if (!n) return JS_NewString(ctx, "");
    size_t len = 0;
    char *s = dom_serialize(ctx, n, false, &len);
    GCValue out = JS_NewStringLen(ctx, s, len);
    free(s);
    return out;
}

// Element.prototype.innerHTML setter
GCValue js_element_set_inner_html(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    size_t len = 0;
    const char *html = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!html) { html = ""; len = 0; }
    html_element_set_inner_html_len(ctx, this_val, html, len);
    dom_request_layout();
    return JS_UNDEFINED;
}

// Element.prototype.outerHTML getter
GCValue js_element_get_outer_html(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    if (dom_kind(n) != DOM_K_ELEMENT) return JS_NewString(ctx, "");
    size_t len = 0;
    char *s = dom_serialize(ctx, n, true, &len);
    GCValue out = JS_NewStringLen(ctx, s, len);
    free(s);
    return out;
}

// Element.prototype.outerHTML setter
GCValue js_element_set_outer_html(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    size_t len = 0;
    const char *html = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!html) { html = ""; len = 0; }
    NodeId n = dom_node_of(this_val);
    NodeId parent = dom_parent(n);
    if (!parent) return JS_UNDEFINED;
    /* Parse into a temporary container, move the result before this element,
     * then remove the element. */
    NodeId temp = dom_create_element_str("div", 3, dom_owner(n));
    dom_set_inner_html(ctx, temp, html, len);
    while (dom_first_child(temp)) {
        NodeId c = dom_first_child(temp);
        dom_detach(c, false);
        if (!dom_api_insert(ctx, parent, c, n)) break;
    }
    dom_free_subtree(temp);
    if (dom_parent(n) == parent) dom_api_remove_discard(ctx, parent, n);
    dom_request_layout();
    return JS_UNDEFINED;
}

// ============================================================================
// Node Content Getters/Setters
// ============================================================================

static void collect_text(NodeId n, SBuf *b) {
    uint32_t r = dom_slot(n);
    for (uint32_t s = r; s; s = dom_next_in_subtree(s, r))
        if (g_dom.kind[s] == DOM_K_TEXT && g_dom.text[s].len) sb_put(b, g_dom.text[s].s, g_dom.text[s].len);
}

// Node.prototype.textContent getter
GCValue js_node_get_text_content(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    if (!n) return JS_NewString(ctx, "");
    int k = dom_kind(n);
    if (k == DOM_K_TEXT || k == DOM_K_COMMENT) {
        uint32_t len = 0;
        const char *t = dom_text(n, &len);
        return JS_NewStringLen(ctx, t, len);
    }
    SBuf b = { 0 };
    collect_text(n, &b);
    GCValue out = JS_NewStringLen(ctx, b.data ? b.data : "", b.len);
    free(b.data);
    return out;
}

// Node.prototype.textContent setter
GCValue js_node_set_text_content(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    size_t tlen = 0;
    const char *text = JS_IsNull(argv[0]) ? "" : JS_ToCStringLen(ctx, &tlen, argv[0]);
    if (!text) { text = ""; tlen = 0; }
    NodeId n = dom_node_of(this_val);
    if (!n) return JS_UNDEFINED;
    int k = dom_kind(n);
    // For text/comment nodes, textContent behaves like nodeValue/data: update
    // the text and fire a characterData mutation (polyfill schedulers rely
    // on this as a microtask trigger).
    if (k == DOM_K_TEXT || k == DOM_K_COMMENT) {
        uint32_t olen = 0;
        const char *old = dom_text(n, &olen);
        char *old_copy = strndup(old, olen);
        dom_set_text(n, text, tlen);
        dom_mark_dirty(n, DOMF_DIRTY_TEXT);
        mo_notify_character_data(ctx, this_val, old_copy);
        free(old_copy);
        return JS_UNDEFINED;
    }
    char *copy = strndup(text, tlen);
    while (dom_first_child(n)) {
        if (!dom_api_remove_discard(ctx, n, dom_first_child(n))) break;
    }
    if (tlen) {
        NodeId t = dom_create_chardata(DOM_K_TEXT, copy, tlen, dom_owner(n));
        dom_api_insert(ctx, n, t, DOM_NULL);
    }
    free(copy);
    dom_request_layout();
    return JS_UNDEFINED;
}

// Node.prototype.nodeValue getter
GCValue js_node_get_node_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    int k = dom_kind(n);
    if (k == DOM_K_TEXT || k == DOM_K_COMMENT) {
        uint32_t len = 0;
        const char *t = dom_text(n, &len);
        return JS_NewStringLen(ctx, t, len);
    }
    return JS_NULL;
}

// Node.prototype.nodeValue setter
GCValue js_node_set_node_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    NodeId n = dom_node_of(this_val);
    int k = dom_kind(n);
    if (k == DOM_K_TEXT || k == DOM_K_COMMENT) {
        size_t tlen = 0;
        const char *text = JS_ToCStringLen(ctx, &tlen, argv[0]);
        if (!text) { text = ""; tlen = 0; }
        uint32_t olen = 0;
        const char *old = dom_text(n, &olen);
        char *old_copy = strndup(old, olen);
        dom_set_text(n, text, tlen);
        dom_mark_dirty(n, DOMF_DIRTY_TEXT);
        /* NOTE: do NOT mirror into "data"/"textContent" JS properties: a [[Set]]
         * on "textContent" re-invokes polyfill setters (infinite recursion). */
        mo_notify_character_data(ctx, this_val, old_copy);
        free(old_copy);
    }
    return JS_UNDEFINED;
}

// CharacterData.prototype.data getter/setter (used by Text/Comment nodes).
GCValue js_node_get_data(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_node_get_node_value(ctx, this_val, argc, argv);
}

GCValue js_node_set_data(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_node_set_node_value(ctx, this_val, argc, argv);
}


static GCValue common_ancestor_node(JSContextHandle ctx, GCValue a, GCValue b);


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
    (void)ctx;
    NodeId p = dom_node_of(parent), c = dom_node_of(child);
    if (!p || !c) return -1;
    int idx = 0;
    for (NodeId x = dom_first_child(p); x; x = dom_next_sibling(x), idx++)
        if (x == c) return idx;
    return -1;
}

static int node_child_count(JSContextHandle ctx, GCValue parent) {
    (void)ctx;
    int count = 0;
    for (NodeId x = dom_first_child(dom_node_of(parent)); x; x = dom_next_sibling(x)) count++;
    return count;
}

/* Common inclusive ancestor of two nodes (JS_NULL in different trees). */
static GCValue common_ancestor_node(JSContextHandle ctx, GCValue a, GCValue b) {
    NodeId na = dom_node_of(a), nb = dom_node_of(b);
    for (NodeId x = na; x; x = dom_parent(x))
        if (dom_contains(x, nb)) return dom_js(ctx, x);
    return JS_NULL;
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
    NodeId n = dom_node_of(sc);
    if (dom_kind(n) == DOM_NODE_TYPE_ELEMENT) {
        NodeId child = dom_first_child(n);
        for (int idx = 0; child && idx < so; idx++) child = dom_next_sibling(child);
        GCValue args[2] = { argv[0], dom_js(ctx, child) };
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
    NodeId node = dom_node_of(current);
    if (!(node)) return JS_NULL;
    GCValue parent = dom_js_parent(ctx, node);
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
    NodeId node = dom_node_of(current);
    if (!(node)) return JS_NULL;
    GCValue child = dom_js_first(ctx, node);
    while (!JS_IsNull(child)) {
        if (tree_walker_accept_node(ctx, this_val, child)) {
            tree_walker_set_current(ctx, this_val, child);
            return child;
        }
        NodeId child_node = dom_node_of(child);
        if (!(child_node)) break;
        child = dom_js_next(ctx, child_node);
    }
    return JS_NULL;
}

GCValue js_tree_walker_last_child(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue current = tree_walker_get_current(ctx, this_val);
    if (JS_IsNull(current) || JS_IsUndefined(current)) return JS_NULL;
    NodeId node = dom_node_of(current);
    if (!(node)) return JS_NULL;
    GCValue child = dom_js_last(ctx, node);
    while (!JS_IsNull(child)) {
        if (tree_walker_accept_node(ctx, this_val, child)) {
            tree_walker_set_current(ctx, this_val, child);
            return child;
        }
        NodeId child_node = dom_node_of(child);
        if (!(child_node)) break;
        child = dom_js_prev(ctx, child_node);
    }
    return JS_NULL;
}

GCValue js_tree_walker_next_sibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue current = tree_walker_get_current(ctx, this_val);
    if (JS_IsNull(current) || JS_IsUndefined(current)) return JS_NULL;
    GCValue root = tree_walker_get_root(ctx, this_val);
    NodeId node = dom_node_of(current);
    while (!!(node)) {
        GCValue sib = dom_js_next(ctx, node);
        if (JS_IsNull(sib)) break;
        if (!JS_StrictEq(ctx, sib, root) && tree_walker_accept_node(ctx, this_val, sib)) {
            tree_walker_set_current(ctx, this_val, sib);
            return sib;
        }
        node = dom_node_of(sib);
    }
    return JS_NULL;
}

GCValue js_tree_walker_previous_sibling(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue current = tree_walker_get_current(ctx, this_val);
    if (JS_IsNull(current) || JS_IsUndefined(current)) return JS_NULL;
    GCValue root = tree_walker_get_root(ctx, this_val);
    NodeId node = dom_node_of(current);
    while (!!(node)) {
        GCValue sib = dom_js_prev(ctx, node);
        if (JS_IsNull(sib)) break;
        if (!JS_StrictEq(ctx, sib, root) && tree_walker_accept_node(ctx, this_val, sib)) {
            tree_walker_set_current(ctx, this_val, sib);
            return sib;
        }
        node = dom_node_of(sib);
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
        NodeId n = dom_node_of(node);
        if (!(n)) break;
        GCValue child = dom_js_first(ctx, n);
        while (!JS_IsNull(child)) {
            if (tree_walker_accept_node(ctx, this_val, child)) {
                tree_walker_set_current(ctx, this_val, child);
                return child;
            }
            NodeId child_node = dom_node_of(child);
            if (!(child_node)) break;
            child = dom_js_next(ctx, child_node);
        }
        // No accepted children; try next sibling, then ancestors' next siblings.
        while (!JS_IsNull(node) && JS_IsObject(node)) {
            if (JS_StrictEq(ctx, node, root)) return JS_NULL;
            NodeId n2 = dom_node_of(node);
            if (!(n2)) return JS_NULL;
            GCValue sib = dom_js_next(ctx, n2);
            while (!JS_IsNull(sib)) {
                if (tree_walker_accept_node(ctx, this_val, sib)) {
                    tree_walker_set_current(ctx, this_val, sib);
                    return sib;
                }
                NodeId sib_node = dom_node_of(sib);
                if (!(sib_node)) break;
                sib = dom_js_next(ctx, sib_node);
            }
            node = dom_js_parent(ctx, n2);
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
        NodeId n = dom_node_of(node);
        if (!(n)) break;
        GCValue sib = dom_js_prev(ctx, n);
        if (!JS_IsNull(sib)) {
            // Descend to deepest last-child of previous sibling.
            node = sib;
            while (!JS_IsNull(node)) {
                NodeId n2 = dom_node_of(node);
                if (!(n2)) break;
                GCValue last = dom_js_last(ctx, n2);
                if (JS_IsNull(last)) break;
                node = last;
            }
            if (tree_walker_accept_node(ctx, this_val, node)) {
                tree_walker_set_current(ctx, this_val, node);
                return node;
            }
            continue;
        }
        GCValue parent = dom_js_parent(ctx, n);
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
    NodeId node = dom_node_of(elem);
    if (!(node)) return JS_NULL;
    const char *value = dom_attr(node, name);
    if (!value) return JS_NULL;
    return create_attr_obj(ctx, elem, name, value);
}

static GCValue named_node_map_get_item(JSContextHandle ctx, GCValue map, int index) {
    GCValue elem = JS_GetPropertyStr(ctx, map, "ownerElement");
    if (!JS_IsObject(elem)) return JS_NULL;
    NodeId node = dom_node_of(elem);
    if (!(node)) return JS_NULL;
    const DomAttr *a = dom_attr_at(node, index);
    if (!a) return JS_NULL;
    return create_attr_obj(ctx, elem, dom_attr_name_for_output(css_atom_str(a->name)), a->value.s ? a->value.s : "");
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
        GCValue set_args[2] = { [0 ... (2) - 1] = GC_UNDEFINED_INIT };
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

    NodeId node = dom_node_of(elem);
    if (!(node)) return throw_dom_exception(ctx, "NotFoundError", "Attribute not found");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return throw_dom_exception(ctx, "NotFoundError", "Attribute not found");

    const char *old_value = dom_attr(node, name);
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
    NodeId node = dom_node_of(this_val);
    int count = dom_attr_count(node);
    for (int i = 0; i < count; i++) {
        const DomAttr *a = dom_attr_at(node, i);
        if (!a) break;
        char *v = strndup(a->value.s ? a->value.s : "", a->value.len);
        GCValue attr = create_attr_obj(ctx, this_val, dom_attr_name_for_output(css_atom_str(a->name)), v);
        free(v);
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
    /* Iterable like NamedNodeMap: Array.prototype's iterator walks the
     * indexed attributes by length. */
    {
        GCValue array_ctor = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "Array");
        GCValue array_proto = JS_GetPropertyStr(ctx, array_ctor, "prototype");
        JS_SetProperty(ctx, map, dom_iterator_atom(ctx), JS_GetProperty(ctx, array_proto, dom_iterator_atom(ctx)));
    }
    return map;
}

// Element.prototype.hasAttributes()
GCValue js_element_has_attributes(JSContextHandle ctx, GCValue this_val,
                                  int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId node = dom_node_of(this_val);
    bool has = dom_attr_count(node) > 0;
    return JS_NewBool(ctx, has);
}

// Element.prototype.getAttributeNames()
GCValue js_element_get_attribute_names(JSContextHandle ctx, GCValue this_val,
                                       int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue arr = JS_NewArray(ctx);
    NodeId node = dom_node_of(this_val);
    if (!(node)) return arr;
    int count = dom_attr_count(node);
    for (int i = 0; i < count; i++) {
        const DomAttr *a = dom_attr_at(node, i);
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, dom_attr_name_for_output(css_atom_str(a->name))));
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
        NodeId node = dom_node_of(cur);
        if (!(node)) break;
        cur = dom_js_parent(ctx, node);
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
        NodeId node = dom_node_of(slot);
        if (!!(node)) {
            const char *n = dom_attr(node, "name");
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
        NodeId child_node = dom_node_of(child);
        if (!!(child_node)) {
            const char *s = dom_attr(child_node, "slot");
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
    NodeId node = dom_node_of(this_val);
    if (!!(node)) {
        const char *n = dom_attr(node, "name");
        if (n) name = n;
    }
    return JS_NewString(ctx, name);
}


// ============================================================================
// DOMParser / XMLSerializer Implementation
// ============================================================================

/* A new document node populated from parsed HTML, with its wrapper. */
GCValue dom_new_parsed_document(JSContextHandle ctx, const char *html, size_t len);

GCValue js_dom_parser_parse_from_string(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "parseFromString requires 2 arguments");
    size_t len = 0;
    const char *str = JS_ToCStringLen(ctx, &len, argv[0]);
    const char *mime = JS_ToCString(ctx, argv[1]);
    if (!str) { str = ""; len = 0; }
    bool as_xml = mime && strstr(mime, "xml") != NULL;
    if (!len) return JS_NULL;
    GCValue js_doc = dom_new_parsed_document(ctx, str, len);
    if (JS_IsObject(js_doc))
        JS_SetPropertyStr(ctx, js_doc, "contentType", JS_NewString(ctx, as_xml ? "application/xml" : "text/html"));
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
    NodeId n = dom_node_of(argv[0]);
    size_t len = 0;
    char *html = n ? dom_serialize(ctx, n, true, &len) : NULL;
    GCValue result = JS_NewStringLen(ctx, html ? html : "", html ? len : 0);
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
// Fragments, importNode, elementFromPoint
// ============================================================================

// Helper: create a DocumentFragment (store node + wrapper).
GCValue js_create_document_fragment(JSContextHandle ctx) {
    NodeId f = dom_create_fragment(g_dom_document);
    return dom_js(ctx, f);
}

// document.createDocumentFragment()
GCValue js_document_create_document_fragment(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId f = dom_create_fragment(owner_of_value(ctx, this_val));
    GCValue frag = dom_js(ctx, f);
    if (JS_IsObject(frag)) dom_node_set_owner_document(ctx, frag, this_val);
    return frag;
}

// document.importNode()
GCValue js_document_import_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "importNode requires a node");
    bool deep = argc > 1 && JS_ToBool(ctx, argv[1]);
    GCValue deep_val = JS_NewBool(ctx, deep);
    GCValue clone = js_node_cloneNode_real(ctx, argv[0], 1, &deep_val);
    if (JS_IsException(clone)) return clone;
    dom_node_set_owner_document(ctx, clone, this_val);
    /* Imported into the document: its custom elements upgrade now. */
    NodeId cn = dom_node_of(clone);
    if (cn && dom_owner(cn) == g_dom_document) dom_ce_upgrade_created(ctx, cn);
    return clone;
}

/* Hit test of the last layout (§7, "Rendering"): deepest box under the point
 * whose DOM node is an element, as its wrapper. */
static GCValue hit_test_layout_node(JSContextHandle ctx, LayoutContext *layout, int idx, double x, double y) {
    if (!layout || idx < 0 || idx >= layout->tree.count) return JS_NULL;
    LayoutBox *box = &layout->boxes[idx];
    LayoutNodeRef *ref = &layout->tree.nodes[idx];
    bool inside = (x >= box->x && x <= box->x + box->width &&
                   y >= box->y && y <= box->y + box->height);
    if (!inside) return JS_NULL;
    int last_child = -1;
    for (int child = ref->first_child_idx; child >= 0; child = layout->tree.nodes[child].next_sibling_idx)
        last_child = child;
    while (last_child >= 0) {
        GCValue child_hit = hit_test_layout_node(ctx, layout, last_child, x, y);
        if (!JS_IsNull(child_hit)) return child_hit;
        last_child = layout->tree.nodes[last_child].prev_sibling_idx;
    }
    if (ref->dom_node_idx >= 0 && layout->doc) {
        HtmlNode *hnode = (HtmlNode *)po_array_payload(&layout->doc->array, ref->dom_node_idx);
        if (hnode && hnode->type == HTML_NODE_ELEMENT && dom_valid(hnode->store_id) &&
            dom_kind(hnode->store_id) == DOM_K_ELEMENT)
            return dom_js(ctx, hnode->store_id);
    }
    return JS_NULL;
}

/* The layout elementFromPoint() hit-tests, kept while the document renders
 * the same (§7: no rebuild and no relayout when nothing changed). */
static struct {
    uint64_t key;
    HtmlDocument *doc;
    LayoutContext layout;
    bool ok;
} g_hit_cache;

// document.elementFromPoint()
GCValue js_document_element_from_point(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    double x = 0, y = 0;
    if (argc > 0) JS_ToFloat64(ctx, &x, argv[0]);
    if (argc > 1) JS_ToFloat64(ctx, &y, argv[1]);
    GCValue doc_elem = JS_GetPropertyStr(ctx, this_val, "documentElement");
    if (JS_IsUndefined(doc_elem) || JS_IsNull(doc_elem)) return JS_NULL;
    bool unchanged = false;
    uint64_t key = g_hit_cache.key;
    HtmlDocument *doc = html_document_from_js_dom_if_changed(ctx, this_val, &key, &unchanged);
    if (!unchanged) {
        if (g_hit_cache.ok) css_layout_tree_free(&g_hit_cache.layout);
        if (g_hit_cache.doc) html_document_free(g_hit_cache.doc);
        memset(&g_hit_cache, 0, sizeof(g_hit_cache));
        if (!doc) return doc_elem;
        g_hit_cache.doc = doc;
        g_hit_cache.key = key;
        g_hit_cache.ok = css_layout_run(&g_hit_cache.layout, doc, NULL, 1920.0, 1080.0);
    }
    GCValue result = doc_elem;
    LayoutContext *layout = &g_hit_cache.layout;
    if (g_hit_cache.ok && layout->tree.root_idx >= 0) {
        GCValue hit = hit_test_layout_node(ctx, layout, layout->tree.root_idx, x, y);
        if (!JS_IsNull(hit)) result = hit;
    }
    return result;
}

/*
 * DOM API over the node store (PARALLEL_DOM.md §6, §7).
 *
 * Nodes live in the store (dom_store.h); JS objects are lazy wrappers
 * (dom_wrap.h).  Every API takes the NodeId of its wrapper arguments, works
 * on the store, and returns wrappers created on demand with dom_js().
 */
#include "cyber_env.h"
#include <string.h>
#include <limits.h>
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
#include "layout_api.h"
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
#include "css_anim.h"
#include "css_style_decl.h"
#include "dom_fetch.h"
#include "dom_query.h"
#include "code_cache.h"
#include "image_api.h"
#include "dom_range.h"

/* A CharacterData method's part of the data its next set replaces
 * (dom_api_note_replace_data). */
typedef struct RangeReplace { bool set; uint32_t off, count, len; } RangeReplace;
static RangeReplace g_replace;

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
    if (GC_IS_REFERENCE(val) && GC_VALUE_GET_HANDLE(val) != GC_HANDLE_NULL)
        gc_add_root(GC_VALUE_GET_HANDLE(val));
}
static void gc_value_vec_free(GCValueVec *v) {
    for (size_t i = 0; i < v->count; i++)
        if (GC_IS_REFERENCE(v->data[i]) && GC_VALUE_GET_HANDLE(v->data[i]) != GC_HANDLE_NULL)
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
    case DOM_K_CDATA: return "#cdata-section";
    case DOM_K_DOCTYPE:     /* its name */
    case DOM_K_PI:          /* its target */
        return dom_name_str(g_dom.lname[dom_slot(id)]);
    case DOM_K_ELEMENT: break;
    default: return "";
    }
    CssAtom a = dom_tag(id);
    if (a == CSS_ATOM_NONE || a == CSS_ATOM_EMPTY) return "DIV";
    uint32_t s = dom_slot(id);
    NodeId doc = dom_owner(id) ? dom_owner(id) : g_dom_document;
    bool upper = g_dom.ns[s] == DOM_NS_HTML && dom_is_html_document(doc);
    if (g_dom.prefix[s] || g_dom.lname[s] || !upper) {
        /* (the qualified name, as written; interned so it stays) */
        const char *p = dom_prefix(id), *l = dom_local_name(id);
        size_t pn = p ? strlen(p) : 0, ln = strlen(l);
        char buf[512];
        if (pn + ln + 2 > sizeof buf) return l;
        size_t n = 0;
        if (p) { memcpy(buf, p, pn); buf[pn] = ':'; n = pn + 1; }
        memcpy(buf + n, l, ln);
        n += ln;
        if (upper)
            for (size_t i = 0; i < n; i++) buf[i] = (char)toupper((unsigned char)buf[i]);
        return dom_name_str(dom_name_intern(buf, n));
    }
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

/* A new document in this process (page_engine_reset): no style objects. */
void dom_api_reset(void) {
    if (g_style_cap) {
        memset(g_style_obj, 0, g_style_cap * sizeof(*g_style_obj));
        memset(g_style_owner, 0, g_style_cap * sizeof(*g_style_owner));
    }
    g_style_n = 0;
}

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
    if (!GC_IS_REFERENCE(style) || !owner) return;
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
    /* (the style attribute's declarations are the new block's) */
    GCValue style = css_new_style_object(ctx, el);
    SAFE_SET(wd, DomWrapHandle, style, style);
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

/* Whether the legacy reflection of attribute `name` as a plain property
 * applies to obj: not over an IDL attribute or method of its interface
 * (assigning disabled="" through its setter would remove the attribute,
 * value="x" would set the control's value). */
static bool dom_attr_reflects_as_expando(JSContextHandle ctx, NodeId id, GCValue obj, const char *name) {
    if (strcmp(name, "id") == 0 || strcmp(name, "class") == 0 || strcmp(name, "style") == 0) return false;
    if (id && dom_attr_has_accessor(id, name)) return false;
    GCValue proto = JS_GetPrototype(ctx, obj);
    if (!JS_IsObject(proto)) return true;
    JSAtom a = JS_NewAtom(ctx, name);
    int has = JS_HasProperty(ctx, proto, a);
    JS_FreeAtom(ctx, a);
    return has <= 0;
}

void dom_element_setup_wrapper(JSContextHandle ctx, NodeId id, GCValue obj) {
    uint32_t s = dom_slot(id);
    /* Attributes are reflected as plain properties (the legacy DOM did this
     * for every parsed element); id, class and style have accessors. */
    DomAttrList *l = &g_dom.attrs[s];
    for (int i = 0; i < l->n; i++) {
        const char *name = css_atom_str(l->a[i].name);
        if (!name || !name[0]) continue;
        if (!dom_attr_reflects_as_expando(ctx, id, obj, name)) continue;
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
            c = dom_create_fragment(template_contents_owner(dom_owner(id)));
            dom_set_template_content(id, c);
        }
        GCValue content = dom_js(ctx, c);
        if (JS_IsObject(content)) JS_SetPropertyStr(ctx, obj, "content", content);
    }
}

/* A document's "appropriate template contents owner document": a document of
 * its kind with no browsing context, made once (its slot in the document's
 * content column). */
NodeId template_contents_owner(NodeId doc) {
    if (!dom_valid(doc) || dom_kind(doc) != DOM_K_DOCUMENT) return doc;
    /* (itself such a one: it is its own) */
    if (dom_flags(doc) & DOMF_INERT_DOC) return doc;
    uint32_t s = dom_slot(doc), c = g_dom.content[s];
    if (c && g_dom.kind[c] == DOM_K_DOCUMENT) return dom_id_of(c);
    NodeId inert = dom_create_document();
    uint32_t i = dom_slot(inert);
    g_dom.flags[i] |= DOMF_INERT_DOC | (dom_flags(doc) & DOMF_XML_DOCUMENT);
    g_dom.realm[i] = g_dom.realm[s];
    g_dom.content[s] = i;
    return inert;
}

/* Factory for wrappers of store nodes (dom_wrap.h). */
/* Interface (global constructor) whose prototype a new element wrapper gets,
 * so that `el instanceof HTMLInputElement` etc. hold and polyfills that patch
 * a specific interface (ShadyDOM patches HTMLSlotElement.prototype) reach it.
 * Interfaces the browser does not define fall back to HTMLElement. */
static const char *element_interface_name(NodeId id) {
    /* (other namespaces' elements: SVG's are SVGElements, the rest Elements) */
    if (dom_ns(id) != DOM_NS_HTML) {
        if (dom_ns(id) != DOM_NS_SVG) return "Element";
        return !strcmp(dom_local_name(id), "a") ? "SVGAElement" : "SVGElement";
    }
    /* (HTML's element names are lower-case: an upper-case local name, as
     * createElementNS makes, is no HTML element's) */
    for (const char *p = dom_local_name(id); p && *p; p++)
        if (*p >= 'A' && *p <= 'Z') return "HTMLUnknownElement";
    /* HTML's elements and their interfaces (HTML section 4, "DOM
     * interface"); the others are HTMLElement. */
    static const struct { const char *tag, *iface; } map[] = {
        { "a", "HTMLAnchorElement" },       { "area", "HTMLAreaElement" },
        { "base", "HTMLBaseElement" },      { "blockquote", "HTMLQuoteElement" },
        { "body", "HTMLBodyElement" },      { "br", "HTMLBRElement" },
        { "button", "HTMLButtonElement" },  { "canvas", "HTMLCanvasElement" },
        { "caption", "HTMLTableCaptionElement" }, { "col", "HTMLTableColElement" },
        { "colgroup", "HTMLTableColElement" }, { "data", "HTMLDataElement" },
        { "datalist", "HTMLDataListElement" }, { "del", "HTMLModElement" },
        { "dir", "HTMLDirectoryElement" },  { "font", "HTMLFontElement" },
        { "frame", "HTMLFrameElement" },    { "frameset", "HTMLFrameSetElement" },
        { "param", "HTMLParamElement" },    { "listing", "HTMLPreElement" },
        { "xmp", "HTMLPreElement" },        { "marquee", "HTMLMarqueeElement" },
        { "details", "HTMLDetailsElement" }, { "dialog", "HTMLDialogElement" },
        { "div", "HTMLDivElement" },        { "dl", "HTMLDListElement" },
        { "embed", "HTMLEmbedElement" },    { "fencedframe", "HTMLFencedFrameElement" },
        { "fieldset", "HTMLFieldSetElement" }, { "form", "HTMLFormElement" },
        { "h1", "HTMLHeadingElement" },     { "h2", "HTMLHeadingElement" },
        { "h3", "HTMLHeadingElement" },     { "h4", "HTMLHeadingElement" },
        { "h5", "HTMLHeadingElement" },     { "h6", "HTMLHeadingElement" },
        { "head", "HTMLHeadElement" },      { "hr", "HTMLHRElement" },
        { "html", "HTMLHtmlElement" },      { "iframe", "HTMLIFrameElement" },
        { "img", "HTMLImageElement" },      { "input", "HTMLInputElement" },
        { "ins", "HTMLModElement" },        { "label", "HTMLLabelElement" },
        { "legend", "HTMLLegendElement" },  { "li", "HTMLLIElement" },
        { "link", "HTMLLinkElement" },      { "map", "HTMLMapElement" },
        { "menu", "HTMLMenuElement" },      { "meta", "HTMLMetaElement" },
        { "meter", "HTMLMeterElement" },    { "object", "HTMLObjectElement" },
        { "ol", "HTMLOListElement" },       { "optgroup", "HTMLOptGroupElement" },
        { "option", "HTMLOptionElement" },  { "output", "HTMLOutputElement" },
        { "p", "HTMLParagraphElement" },    { "picture", "HTMLPictureElement" },
        { "pre", "HTMLPreElement" },        { "progress", "HTMLProgressElement" },
        { "q", "HTMLQuoteElement" },        { "script", "HTMLScriptElement" },
        { "select", "HTMLSelectElement" },  { "slot", "HTMLSlotElement" },
        { "source", "HTMLSourceElement" },  { "span", "HTMLSpanElement" },
        { "style", "HTMLStyleElement" },    { "table", "HTMLTableElement" },
        { "tbody", "HTMLTableSectionElement" }, { "td", "HTMLTableCellElement" },
        { "template", "HTMLTemplateElement" }, { "textarea", "HTMLTextAreaElement" },
        { "tfoot", "HTMLTableSectionElement" }, { "th", "HTMLTableCellElement" },
        { "thead", "HTMLTableSectionElement" }, { "time", "HTMLTimeElement" },
        { "title", "HTMLTitleElement" },    { "tr", "HTMLTableRowElement" },
        { "track", "HTMLTrackElement" },    { "ul", "HTMLUListElement" },
    };
    enum { N = sizeof(map) / sizeof(map[0]) };
    static CssAtom atoms[N];
    if (!atoms[0]) {
        css_atom_reserve(N);
        for (size_t i = 0; i < N; i++) atoms[i] = css_atom_intern(map[i].tag, strlen(map[i].tag));
    }
    CssAtom t = dom_tag(id);
    for (size_t i = 0; i < N; i++)
        if (atoms[i] == t) return map[i].iface;
    /* (the elements whose interface is HTMLElement itself; any other name
     * without a '-', HTMLUnknownElement) */
    static const char *const plain[] = {
        "abbr", "acronym", "address", "article", "aside", "b", "basefont", "bdi", "bdo", "big", "center", "cite",
        "code", "dd", "dfn", "dt", "em", "figcaption", "figure", "footer", "header", "hgroup", "i", "kbd", "main",
        "mark", "nav", "nobr", "noembed", "noframes", "noscript", "plaintext", "rb", "rp", "rt", "rtc", "ruby", "s",
        "samp", "search", "section", "small", "strike", "strong", "sub", "summary", "sup", "tt", "u", "var", "wbr",
        "math", "svg", "image", "selectedcontent",
    };
    enum { NP = sizeof(plain) / sizeof(plain[0]) };
    static CssAtom patoms[NP];
    if (!patoms[0]) {
        css_atom_reserve(NP);
        for (size_t i = 0; i < NP; i++) patoms[i] = css_atom_intern(plain[i], strlen(plain[i]));
    }
    for (size_t i = 0; i < NP; i++)
        if (patoms[i] == t) return "HTMLElement";
    const char *name = css_atom_str(t);
    return name && !strchr(name, '-') ? "HTMLUnknownElement" : "HTMLElement";
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
            if (dom_tag_is(id, "audio")) {
                GCValue ap = global_proto(ctx, "HTMLAudioElement");
                if (JS_IsObject(ap)) JS_SetPrototype(ctx, obj, ap);
            }
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
        /* (nodeType and nodeName are Node.prototype's getters, by kind:
         * assigning them threw, and built a stack trace, per comment) */
        return obj;
    }
    case DOM_K_CDATA:
    case DOM_K_PI:
    case DOM_K_DOCTYPE: {
        obj = JS_NewObjectClass(ctx, js_dom_node_class_id);
        GCValue proto = global_proto(ctx, kind == DOM_K_CDATA ? "CDATASection"
                                          : kind == DOM_K_PI  ? "ProcessingInstruction"
                                                              : "DocumentType");
        if (!JS_IsObject(proto)) proto = global_proto(ctx, "Node");
        if (JS_IsObject(proto)) JS_SetPrototype(ctx, obj, proto);
        dom_bind(ctx, id, obj);
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
static bool g_insert_connecting;
void dom_ce_on_connect(JSContextHandle ctx, NodeId subtree) {
    CP_ACC("CE connect+upgrade");
    /* (inserted by script: after the whole insertion, post_insert_collect) */
    if (!g_insert_connecting) frame_api_on_connect(ctx, subtree);
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

/* A list of nodes made from an array of them: an array-like object (not an
 * Array: Array.isArray() is false for one) whose prototype is the realm's
 * `kind` ("NodeList" or "HTMLCollection") prototype, so item(), forEach and
 * `instanceof` work (and pages may extend those prototypes). */
static GCValue nodes_to_list(JSContextHandle ctx, const NodeId *ids, uint32_t n, const char *kind) {
    return live_list_static(ctx, ids, n, !strcmp(kind, "HTMLCollection"));
}

static GCValue nodes_to_array(JSContextHandle ctx, const NodeId *ids, uint32_t n) {
    return nodes_to_list(ctx, ids, n, "HTMLCollection");
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
    else if (strcmp(name, "InUseAttributeError") == 0) code = 10;
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
static char* dom_resolve_url(const char *base, const char *href);

/* href against the document's URL (malloc'd). */
char *dom_resolve_document_url(JSContextHandle ctx, const char *href) {
    GCValue loc = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "location");
    GCValue hrefv = JS_GetPropertyStr(ctx, loc, "href");
    const char *base = JS_IsString(hrefv) ? JS_ToCString(ctx, hrefv) : NULL;
    char *url = dom_resolve_url(base && base[0] ? base : (g_cyber_start_url ? g_cyber_start_url : ""), href);
    if (base) JS_FreeCString(ctx, base);
    return url;
}

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

/* What a script element runs as (HTML 4.12.1.1, "prepare the script
 * element"): DOM_SCRIPT_NONE, DOM_SCRIPT_CLASSIC (but not with nomodule,
 * which only browsers without modules run) or DOM_SCRIPT_MODULE. */
enum { DOM_SCRIPT_NONE, DOM_SCRIPT_CLASSIC, DOM_SCRIPT_MODULE };
static int dom_script_kind(JSContextHandle ctx, GCValue script) {
    NodeId n = dom_node_of(script);
    const char *type = n ? dom_get_attr_str(n, "type") : NULL;
    /* (script.type = x may have set the object's property only) */
    if (!type) {
        GCValue typev = JS_GetPropertyStr(ctx, script, "type");
        type = JS_IsString(typev) ? JS_ToCString(ctx, typev) : NULL;
    }
    if (type && type[0]) {
        if (strcasecmp(type, "module") == 0) return DOM_SCRIPT_MODULE;
        if (strstr(type, "importmap") || (!strstr(type, "javascript") && !strstr(type, "ecmascript") &&
                                         strcasecmp(type, "text/jscript") != 0))
            return DOM_SCRIPT_NONE;
    }
    if (n && dom_get_attr_str(n, "nomodule")) return DOM_SCRIPT_NONE;
    return DOM_SCRIPT_CLASSIC;
}

static bool dom_script_type_is_executable(JSContextHandle ctx, GCValue script) {
    return dom_script_kind(ctx, script) != DOM_SCRIPT_NONE;
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

        /* (a module is named by its URL; its imports load as it is linked) */
        bool module = dom_script_kind(ctx, script) == DOM_SCRIPT_MODULE;
        if (module) JS_SetPropertyStr(ctx, doc, "currentScript", JS_NULL);
        cp_profile_begin("eval-dynamic-script", "js");
        /* (A classic script compiled before runs from its bytecode: code_cache.h.) */
        GCValue cached = module ? JS_UNDEFINED : code_cache_lookup(ctx, buffer.data, buffer.size);
        GCValue result = module ? js_quickjs_eval_module(ctx, buffer.data, buffer.size, url, 0)
                         : !JS_IsUndefined(cached) ? JS_EvalFunction(ctx, cached)
                                                   : JS_Eval(ctx, buffer.data, buffer.size, url, JS_EVAL_TYPE_GLOBAL);
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
    if (!GC_IS_REFERENCE(v)) return;
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

/* A script-inserted script with no src (HTML "prepare the script element"):
 * its child text, run once; an empty one isn't started, and runs when it
 * has text.  An exception goes to reportError. */
static void dom_run_inline_script(JSContextHandle ctx, GCValue script) {
    NodeId n = dom_node_of(script);
    if (dom_flags(n) & DOMF_PARSER_SCRIPT) return;
    if (JS_ToBool(ctx, JS_GetPropertyStr(ctx, script, "__cyber_script_loaded"))) return;
    if (JS_ToBool(ctx, JS_GetPropertyStr(ctx, script, "__cyber_parser_script"))) return;
    if (!dom_node_is_connected(ctx, script) || !dom_script_type_is_executable(ctx, script)) return;
    size_t len = 0;
    for (NodeId c = dom_first_child(n); c; c = dom_next_sibling(c))
        if (dom_kind(c) == DOM_K_TEXT || dom_kind(c) == DOM_K_CDATA) {
            uint32_t l = 0;
            dom_text(c, &l);
            len += l;
        }
    if (!len) return;
    char *text = malloc(len + 1);
    if (!text) return;
    size_t k = 0;
    for (NodeId c = dom_first_child(n); c; c = dom_next_sibling(c))
        if (dom_kind(c) == DOM_K_TEXT || dom_kind(c) == DOM_K_CDATA) {
            uint32_t l = 0;
            const char *t = dom_text(c, &l);
            memcpy(text + k, t, l);
            k += l;
        }
    text[k] = 0;
    JS_SetPropertyStr(ctx, script, "__cyber_script_loaded", JS_TRUE);
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue doc = JS_GetPropertyStr(ctx, global, "document");
    GCValue prev_current = JS_GetPropertyStr(ctx, doc, "currentScript");
    bool module = dom_script_kind(ctx, script) == DOM_SCRIPT_MODULE;
    JS_SetPropertyStr(ctx, doc, "currentScript", module ? JS_NULL : script);
    GCValue href = JS_GetPropertyStr(ctx, JS_GetPropertyStr(ctx, global, "location"), "href");
    const char *url = JS_IsString(href) ? JS_ToCString(ctx, href) : NULL;
    GCValue result = module ? js_quickjs_eval_module(ctx, text, k, url ? url : "", 0)
                            : JS_Eval(ctx, text, k, url ? url : "<inline>", JS_EVAL_TYPE_GLOBAL);
    if (url) JS_FreeCString(ctx, url);
    free(text);
    JS_SetPropertyStr(ctx, doc, "currentScript", prev_current);
    if (JS_IsException(result)) {
        GCValue exc = JS_GetException(ctx);
        GCValue report = JS_GetPropertyStr(ctx, global, "reportError");
        if (JS_IsFunction(ctx, report)) {
            GCValue rr = JS_Call(ctx, report, global, 1, &exc);
            if (JS_IsException(rr)) JS_GetException(ctx);
        }
    }
}

static void dom_maybe_load_script(JSContextHandle ctx, GCValue node) {
    if (!g_dom_dynamic_scripts_enabled) return;
    if (!dom_tag_is(dom_node_of(node), "script")) return;
    /* (scripting is disabled in a document with no browsing context) */
    if (dom_owner(dom_node_of(node)) != dom_node_of(JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "document"))) return;
    GCValue srcv = JS_GetPropertyStr(ctx, node, "src");
    const char *src = JS_IsString(srcv) ? JS_ToCString(ctx, srcv) : NULL;
    if (src && src[0]) {
        dom_execute_external_script(ctx, node, src);
    } else if (dom_attr_index_qualified(dom_node_of(node), "src", 3) < 0) {
        dom_run_inline_script(ctx, node);
    }
    if (src) JS_FreeCString(ctx, src);
}

/* ---- scripts an insertion brought in: run when the operation is done ---- */

static struct { NodeId *ids; uint32_t n, cap; } g_post_insert;

static void post_insert_queue(NodeId id) {
    if (g_post_insert.n == g_post_insert.cap) {
        g_post_insert.cap = g_post_insert.cap ? g_post_insert.cap * 2 : 8;
        g_post_insert.ids = realloc(g_post_insert.ids, g_post_insert.cap * sizeof(NodeId));
    }
    g_post_insert.ids[g_post_insert.n++] = id;
}

static CssAtom g_meta_tag;
static CssAtom g_script_tag, g_iframe_tag, g_frame_tag;
/* (a shadow tree comes along: it's connected with its host) */
static void post_insert_subtree(uint32_t r) {
    for (uint32_t s = r; s; s = dom_next_in_subtree(s, r)) {
        if (g_dom.kind[s] != DOM_K_ELEMENT) continue;
        if (g_dom.tag[s] == g_iframe_tag || g_dom.tag[s] == g_frame_tag) post_insert_queue(dom_id_of(s));
        else if (g_dom.tag[s] == g_meta_tag) {
            g_dom.flags[s] |= DOMF_META_PENDING;
            post_insert_queue(dom_id_of(s));
        }
        else if (g_dom.tag[s] == g_script_tag && g_dom_dynamic_scripts_enabled && !(g_dom.flags[s] & DOMF_PARSER_SCRIPT))
            post_insert_queue(dom_id_of(s));
        if (g_dom.shadow[s]) post_insert_subtree(g_dom.shadow[s]);
    }
}

/* The scripts and iframes in child's subtree (tree order), or its parent
 * script when text went into one. */
static void post_insert_collect(NodeId parent, NodeId child) {
    if (dom_kind(child) != DOM_K_ELEMENT) {
        if (g_dom_dynamic_scripts_enabled && dom_tag_is(parent, "script")) post_insert_queue(parent);
        return;
    }
    if (!g_script_tag) {
        css_atom_reserve(4);
        g_meta_tag = css_atom_intern("meta", 4);
        g_script_tag = css_atom_intern("script", 6);
        g_iframe_tag = css_atom_intern("iframe", 6);
        g_frame_tag = css_atom_intern("frame", 5);
    }
    post_insert_subtree(dom_slot(child));
}

/* (each run takes the queue: an insertion by one of them has its own) */
void dom_post_insert_flush(JSContextHandle ctx) {
    while (g_post_insert.n) {
        uint32_t n = g_post_insert.n;
        NodeId *ids = g_post_insert.ids;
        g_post_insert.ids = NULL;
        g_post_insert.n = g_post_insert.cap = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (!dom_valid(ids[i])) continue;
            /* (a meta's post-insertion steps: it counts from here) */
            if (dom_tag(ids[i]) == g_meta_tag) {
                g_dom.flags[dom_slot(ids[i])] &= (uint16_t)~DOMF_META_PENDING;
                continue;
            }
            if (dom_tag(ids[i]) == g_iframe_tag || dom_tag(ids[i]) == g_frame_tag) {
                bool was = g_frame_blank_now;
                g_frame_blank_now = true;
                if (dom_is_connected(ids[i])) frame_api_on_connect(ctx, ids[i]);
                g_frame_blank_now = was;
            } else {
                dom_maybe_load_script(ctx, dom_js(ctx, ids[i]));
            }
        }
        free(ids);
    }
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
    /* (a <style>: its sheet is there; load comes in a task) */
    if (dom_tag_is(n, "style") && dom_node_is_connected(ctx, node)) {
        GCValue fn = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "__cyberStyleInserted");
        if (JS_IsFunction(ctx, fn) && JS_IsException(JS_Call(ctx, fn, JS_UNDEFINED, 1, &node))) JS_GetException(ctx);
        return;
    }
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
    bool ok = url && (!strncasecmp(url, "data:", 5) || (url_is_network_url(url) && css_layout_fetch_stylesheet_url(url)));
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

/* A childList record of node added to (or removed from) target, between
 * prev and next. */
static void notify_child_list(JSContextHandle ctx, GCValue target, GCValue node, bool removal, NodeId prev,
                              NodeId next) {
    GCValue list = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, list, 0, node);
    GCValue empty = JS_NewArray(ctx);
    mo_notify_child_list(ctx, target, removal ? empty : list, removal ? list : empty, dom_js(ctx, prev), dom_js(ctx, next));
}

/* DOM "adopt": the subtree of `node` (its shadow roots' too; template
 * contents keep theirs) becomes document `doc`'s.  Wrappers drop the
 * ownerDocument they were given, so Node.prototype's getter reads the store. */
void dom_api_adopt(JSContextHandle ctx, NodeId node, NodeId doc) {
    uint32_t root = dom_slot(node), d = dom_slot(doc);
    JSAtom od = JS_NewAtom(ctx, "ownerDocument");
    for (uint32_t s = root; s; s = dom_next_in_subtree(s, root)) {
        g_dom.owner[s] = d;
        if (g_dom.wrapper[s]) JS_DeleteProperty(ctx, dom_js(ctx, dom_id_of(s)), od, 0);
        if (g_dom.shadow[s]) dom_api_adopt(ctx, dom_id_of(g_dom.shadow[s]), doc);
        /* (a template's contents follow, to the new document's owner for them) */
        if (g_dom.kind[s] == DOM_K_ELEMENT && g_dom.content[s])
            dom_api_adopt(ctx, dom_id_of(g_dom.content[s]), template_contents_owner(doc));
    }
    JS_FreeAtom(ctx, od);
}

/* Set while moveBefore moves a node: its removal and insertion run none of
 * the removing and inserting steps (it stays connected throughout). */
static bool g_dom_moving;

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
    NodeId pdoc = dom_kind(parent) == DOM_K_DOCUMENT ? parent : dom_owner(parent);
    if (pdoc && dom_owner(child) != pdoc) dom_api_adopt(ctx, child, pdoc);
    dom_attach(parent, child, ref);
    dom_request_layout();
    /* (an <img>'s <picture> changed: it picks its source again) */
    if (dom_tag_is(parent, "picture")) image_api_picture_changed(parent);

    // CEReactions frame for this insertion: any connectedCallback and upgrade
    // reactions queued below are flushed before we return, so a parent
    // constructor can rely on its inserted children being upgraded.
    if (g_dom_moving) {
        if (observers)
            notify_child_list(ctx, parent_js, child_js, false, dom_prev_sibling(child), dom_next_sibling(child));
        return true;
    }
    js_cyber_ce_push_stack(ctx);
    bool connected = dom_api_is_connected(ctx, child);
    if (connected) {
        bool was = g_insert_connecting;
        g_insert_connecting = true;
        dom_ce_on_connect(ctx, child);
        g_insert_connecting = was;
    }
    dom_flush_ce_reactions(ctx);
    js_cyber_ce_pop_stack(ctx);

    /* (its mutation record is queued first: an inserted script can see it) */
    if (observers)
        notify_child_list(ctx, parent_js, child_js, false, dom_prev_sibling(child), dom_next_sibling(child));
    // Scripts inserted dynamically after the initial page load (e.g. YouTube's
    // module loader), the node's and any in its subtree, once the operation
    // is done.
    if (connected) {
        post_insert_collect(parent, child);
        if (!mo_batch_active()) dom_post_insert_flush(ctx);
    }
    if (need_js && g_dom_dynamic_scripts_enabled) dom_maybe_load_stylesheet(ctx, child_js);
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
    NodeId prev_sib = dom_prev_sibling(child), next_sib = dom_next_sibling(child);
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
    /* (a subtree observer of another kind still follows it) */
    if (!observers) mo_note_removal(ctx, parent_js, child_js);
    dom_detach(child, false);
    if (was_connected && !g_dom_moving) {
        frame_api_on_disconnect();
        anim_node_removed(child);
    }
    dom_request_layout();
    if (dom_tag_is(parent, "picture")) {
        image_api_picture_changed(parent);
        image_api_relevant(child);
    }
    /* Every custom element of the removed subtree is disconnected, not just
     * its root (HTML "remove" steps), in tree order.  Components that clear
     * their rendering on disconnect then render once when reinserted. */
    if (was_connected && !g_dom_moving) {
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
    if (observers) notify_child_list(ctx, parent_js, child_js, true, prev_sib, next_sib);
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
/* Is a a host-including inclusive ancestor of b? */
static bool host_including_ancestor(NodeId a, NodeId b) {
    while (b) {
        if (a == b) return true;
        NodeId p = dom_parent(b);
        if (!p && dom_kind(b) == DOM_K_FRAGMENT) p = dom_host(b);
        b = p;
    }
    return false;
}

/* DOM "ensure pre-insertion validity" of inserting node into parent before
 * child (0: at the end), or with replaced set the replace algorithm's checks
 * (child is then the one replaced; none: all of them, as replaceChildren
 * checks).  Throws and returns false if invalid. */
static bool insert_valid(JSContextHandle ctx, NodeId parent, NodeId node, NodeId child, bool replacing);

/* Range's insert (range_api.c): pre-insertion validity. */
bool dom_api_insert_valid(JSContextHandle ctx, NodeId parent, NodeId node, NodeId child) {
    return insert_valid(ctx, parent, node, child, false);
}

static bool insert_valid(JSContextHandle ctx, NodeId parent, NodeId node, NodeId child, bool replacing) {
    int pk = dom_kind(parent), nk = dom_kind(node);
    if (pk != DOM_K_DOCUMENT && pk != DOM_K_FRAGMENT && pk != DOM_K_ELEMENT) {
        throw_dom_exception(ctx, "HierarchyRequestError", "The parent cannot have children.");
        return false;
    }
    if (host_including_ancestor(node, parent)) {
        throw_dom_exception(ctx, "HierarchyRequestError", "The new child contains the parent.");
        return false;
    }
    if (child && dom_parent(child) != parent) {
        throw_dom_exception(ctx, "NotFoundError", "The reference node is not a child of this node.");
        return false;
    }
    if (nk != DOM_K_FRAGMENT && nk != DOM_K_DOCTYPE && nk != DOM_K_ELEMENT && !dom_kind_is_chardata(nk)) {
        throw_dom_exception(ctx, "HierarchyRequestError", "The node cannot be inserted.");
        return false;
    }
    if (((nk == DOM_K_TEXT || nk == DOM_K_CDATA) && pk == DOM_K_DOCUMENT) || (nk == DOM_K_DOCTYPE && pk != DOM_K_DOCUMENT)) {
        throw_dom_exception(ctx, "HierarchyRequestError", "The node cannot be a child of this parent.");
        return false;
    }
    if (pk != DOM_K_DOCUMENT) return true;
    /* (a document: one doctype, then one element, and no text) */
    int elements = nk == DOM_K_ELEMENT;
    bool text = false;
    if (nk == DOM_K_FRAGMENT)
        for (NodeId c = dom_first_child(node); c; c = dom_next_sibling(c)) {
            if (dom_kind(c) == DOM_K_ELEMENT) elements++;
            else if (dom_kind(c) == DOM_K_TEXT || dom_kind(c) == DOM_K_CDATA) text = true;
        }
    bool bad = text || elements > 1;
    bool has_element = false, has_doctype = false, doctype_after = false, element_before = false;
    for (NodeId c = replacing && !child ? DOM_NULL : dom_first_child(parent), seen = 0; c; c = dom_next_sibling(c)) {
        if (c == child) { seen = c; if (replacing) continue; }
        int k = dom_kind(c);
        if (k == DOM_K_ELEMENT) { has_element = true; if (!seen && child) element_before = true; }
        if (k == DOM_K_DOCTYPE) { has_doctype = true; if (seen) doctype_after = true; }
    }
    if (elements == 1)
        bad = bad || has_element || (child && dom_kind(child) == DOM_K_DOCTYPE && !replacing) || doctype_after;
    if (nk == DOM_K_DOCTYPE)
        bad = bad || has_doctype || element_before || (!child && has_element);
    if (bad) throw_dom_exception(ctx, "HierarchyRequestError", "The document cannot have this child there.");
    return !bad;
}

/* Whether v is an Attr (one is never a child: a HierarchyRequestError). */
static bool is_attr_value(JSContextHandle ctx, GCValue v) {
    if (!JS_IsObject(v)) return false;
    JSAtom a = JS_NewAtom(ctx, "__a_name");
    int own = JS_GetOwnProperty(ctx, NULL, v, a);
    JS_FreeAtom(ctx, a);
    return own > 0;
}

static GCValue attr_child_error(JSContextHandle ctx) {
    return throw_dom_exception(ctx, "HierarchyRequestError", "An attribute cannot be a child.");
}

/* Whether v is an object that is no Node (no store node, not an Attr, no
 * Node.prototype on its chain): a Node argument's TypeError. */
static bool not_a_node(JSContextHandle ctx, GCValue v) {
    if (!JS_IsObject(v) || dom_node_of(v) || is_attr_value(ctx, v)) return !JS_IsObject(v);
    GCValue np = global_proto(ctx, "Node");
    GCValue p = JS_GetPrototype(ctx, v);
    for (int depth = 0; JS_IsObject(p) && depth < 64; depth++) {
        if (JS_StrictEq(ctx, p, np)) return false;
        p = JS_GetPrototype(ctx, p);
    }
    return true;
}

GCValue js_node_appendChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "appendChild: invalid argument");
    }
    if (is_attr_value(ctx, argv[0])) return attr_child_error(ctx);
    if (not_a_node(ctx, argv[0]) && !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Failed to execute 'appendChild' on 'Node': parameter 1 is not of type 'Node'.");
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
    if (!insert_valid(ctx, parent, c, DOM_NULL, false)) return JS_EXCEPTION;

    // DocumentFragment: append its children, not the fragment itself.
    if (dom_kind(c) == DOM_K_FRAGMENT) {
        GCValueVec children GC_VALUE_VEC_AUTO = collect_fragment_children(ctx, c);
        mo_batch_begin(ctx);
        for (size_t i = 0; i < children.count; i++) {
            GCValue args[1] = { children.data[i] };
            GCValue r = js_node_appendChild_real(ctx, this_val, 1, args);
            if (JS_IsException(r)) { mo_batch_end(ctx); return r; }
        }
        mo_batch_end(ctx);
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
    if (!c && is_attr_value(ctx, child)) return throw_dom_exception(ctx, "NotFoundError", "Node is not a child of this node");
    if (!c && !JS_IsObject(child))
        return JS_ThrowTypeError(ctx, "Failed to execute 'removeChild' on 'Node': parameter 1 is not of type 'Node'.");
    if (!c) {
        /* (an object that isn't a Node: a TypeError; a node-like object
         * with no store node: nothing to remove) */
        GCValue np = global_proto(ctx, "Node");
        GCValue p = JS_GetPrototype(ctx, child);
        bool nodeish = false;
        for (int depth = 0; JS_IsObject(p) && depth < 64 && !nodeish; depth++) {
            nodeish = JS_StrictEq(ctx, p, np);
            p = JS_GetPrototype(ctx, p);
        }
        if (!nodeish)
            return JS_ThrowTypeError(ctx, "Failed to execute 'removeChild' on 'Node': parameter 1 is not of type 'Node'.");
        return child;
    }
    if (!parent) return child;      /* (no-op for a parent with no store node) */
    if (dom_parent(c) != parent) {
        return throw_dom_exception(ctx, "NotFoundError", "Node is not a child of this node");
    }
    dom_api_remove(ctx, parent, c);
    return child;
}

/* DOM "convert nodes into a node": the one argument's node, or a fragment of
 * them all (anything not a Node becoming a Text of its string), in the
 * document of parent. */
static GCValue convert_nodes(JSContextHandle ctx, NodeId parent, int argc, GCValue *argv) {
    NodeId doc = dom_kind(parent) == DOM_K_DOCUMENT ? parent : (dom_owner(parent) ? dom_owner(parent) : g_dom_document);
    GCValueVec nodes GC_VALUE_VEC_AUTO = { 0 };
    for (int i = 0; i < argc; i++) {
        if (JS_IsObject(argv[i]) && dom_node_of(argv[i])) {
            gc_value_vec_push(&nodes, argv[i]);
            continue;
        }
        size_t n = 0;
        const char *str = JS_ToCStringLen(ctx, &n, argv[i]);
        if (!str) return JS_EXCEPTION;
        GCValue t = dom_js(ctx, dom_create_chardata(DOM_K_TEXT, str, n, doc));
        JS_FreeCString(ctx, str);
        gc_value_vec_push(&nodes, t);
    }
    if (nodes.count == 1) return nodes.data[0];
    GCValue frag = dom_js(ctx, dom_create_fragment(doc));
    if (!JS_IsObject(frag)) return JS_EXCEPTION;
    for (size_t i = 0; i < nodes.count; i++) {
        GCValue r = js_node_appendChild_real(ctx, frag, 1, &nodes.data[i]);
        if (JS_IsException(r)) return r;
    }
    return frag;
}

/* Is v one of the arguments (a Node passed in)? */
static bool among(JSContextHandle ctx, NodeId n, int argc, GCValue *argv) {
    for (int i = 0; i < argc; i++)
        if (JS_IsObject(argv[i]) && dom_node_of(argv[i]) == n) return true;
    (void)ctx;
    return false;
}

// ParentNode.append(...nodes), .prepend(...nodes)
static GCValue js_parent_node_append_or_prepend(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, bool prepend) {
    NodeId p = dom_node_of(this_val);
    if (!p) return JS_ThrowTypeError(ctx, "Illegal invocation");
    GCValue node = convert_nodes(ctx, p, argc, argv);
    if (JS_IsException(node)) return node;
    if (!argc) return JS_UNDEFINED;
    GCValue args[2] = { node, prepend ? dom_js(ctx, dom_first_child(p)) : JS_NULL };
    GCValue r = js_node_insertBefore_real(ctx, this_val, 2, args);
    return JS_IsException(r) ? r : JS_UNDEFINED;
}

GCValue js_element_append(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_parent_node_append_or_prepend(ctx, this_val, argc, argv, false);
}

GCValue js_element_prepend(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_parent_node_append_or_prepend(ctx, this_val, argc, argv, true);
}

// ParentNode.replaceChildren(...nodes): checked first, then all replaced.
GCValue js_element_replaceChildren(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    NodeId p = dom_node_of(this_val);
    if (!p) return JS_ThrowTypeError(ctx, "Illegal invocation");
    GCValue node = convert_nodes(ctx, p, argc, argv);
    if (JS_IsException(node)) return node;
    if (argc && !insert_valid(ctx, p, dom_node_of(node), DOM_NULL, true)) return JS_EXCEPTION;
    /* (the node out of where it is first: a record of its own) */
    NodeId nn = argc ? dom_node_of(node) : DOM_NULL;
    if (nn && dom_kind(nn) != DOM_K_FRAGMENT && dom_parent(nn) && dom_parent(nn) != p) {
        GCValue pj = dom_js(ctx, dom_parent(nn));
        GCValue r0 = js_node_removeChild_real(ctx, pj, 1, &node);
        if (JS_IsException(r0)) return r0;
    }
    mo_batch_begin(ctx);
    while (dom_first_child(p))
        if (!dom_api_remove_discard(ctx, p, dom_first_child(p))) break;
    GCValue r = argc ? js_node_appendChild_real(ctx, this_val, 1, &node) : JS_UNDEFINED;
    mo_batch_end(ctx);
    return JS_IsException(r) ? r : JS_UNDEFINED;
}

/* ChildNode.before (mode 0), after (1), replaceWith (2): around the viable
 * previous or next sibling (one not among the arguments). */
static GCValue js_child_node_insert(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int mode) {
    NodeId self = dom_node_of(this_val), parent = dom_parent(self);
    if (!parent) return JS_UNDEFINED;
    GCValue parent_js = dom_js(ctx, parent);
    NodeId ref;
    if (mode == 0) {
        NodeId prev = dom_prev_sibling(self);
        while (prev && among(ctx, prev, argc, argv)) prev = dom_prev_sibling(prev);
        ref = DOM_NULL;
        GCValue node = convert_nodes(ctx, parent, argc, argv);
        if (JS_IsException(node)) return node;
        if (!argc) return JS_UNDEFINED;
        ref = prev ? dom_next_sibling(prev) : dom_first_child(parent);
        GCValue args[2] = { node, dom_js(ctx, ref) };
        GCValue r = js_node_insertBefore_real(ctx, parent_js, 2, args);
        return JS_IsException(r) ? r : JS_UNDEFINED;
    }
    NodeId next = dom_next_sibling(self);
    while (next && among(ctx, next, argc, argv)) next = dom_next_sibling(next);
    GCValue node = convert_nodes(ctx, parent, argc, argv);
    if (JS_IsException(node)) return node;
    if (mode == 2 && dom_parent(self) == parent) {
        if (!argc) {
            js_node_removeChild_real(ctx, parent_js, 1, &this_val);
            return JS_UNDEFINED;
        }
        GCValue args[2] = { node, this_val };
        GCValue r = js_node_replaceChild_real(ctx, parent_js, 2, args);
        return JS_IsException(r) ? r : JS_UNDEFINED;
    }
    if (!argc) return JS_UNDEFINED;
    GCValue args[2] = { node, dom_js(ctx, next) };
    GCValue r = js_node_insertBefore_real(ctx, parent_js, 2, args);
    return JS_IsException(r) ? r : JS_UNDEFINED;
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
    if (is_attr_value(ctx, argv[0])) return attr_child_error(ctx);
    GCValue new_child = argv[0];
    /* WebIDL: refChild is `Node?` — undefined converts to null (append at end). */
    GCValue ref_child = JS_IsUndefined(argv[1]) ? JS_NULL : argv[1];
    if (not_a_node(ctx, new_child) || (!JS_IsNull(ref_child) && not_a_node(ctx, ref_child)))
        return JS_ThrowTypeError(ctx, "Failed to execute 'insertBefore' on 'Node': parameter is not of type 'Node'.");
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
    }
    if (!insert_valid(ctx, parent, c, ref, false)) return JS_EXCEPTION;

    // DocumentFragment: insert its children before ref_child, not the fragment itself.
    if (dom_kind(c) == DOM_K_FRAGMENT) {
        GCValueVec children GC_VALUE_VEC_AUTO = collect_fragment_children(ctx, c);
        mo_batch_begin(ctx);
        for (size_t i = 0; i < children.count; i++) {
            GCValue args[2] = { children.data[i], ref_child };
            GCValue r = js_node_insertBefore_real(ctx, this_val, 2, args);
            if (JS_IsException(r)) { mo_batch_end(ctx); return r; }
        }
        mo_batch_end(ctx);
        return new_child;
    }
    /* (before itself: before what follows it, after it is taken out) */
    if (ref == c) ref = dom_next_sibling(c);
    if (!dom_api_insert(ctx, parent, c, ref)) return JS_EXCEPTION;
    return new_child;
}

/* Node.hasChildNodes() */
GCValue js_node_has_child_nodes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    if (!n) return JS_ThrowTypeError(ctx, "Illegal invocation");
    return JS_NewBool(ctx, dom_first_child(n) != DOM_NULL);
}

/* The root of n, through shadow roots to their hosts. */
static NodeId shadow_including_root(NodeId n) {
    for (;;) {
        NodeId p = dom_parent(n);
        if (!p && dom_kind(n) == DOM_K_FRAGMENT) p = dom_host(n);
        if (!p) return n;
        n = p;
    }
}

/* ParentNode.moveBefore(node, child) (DOM 4.2.6 "move"): node out of where
 * it is and in before child, keeping its state (no removing or inserting
 * steps: focus, transitions, iframes and custom elements stay as they were,
 * but for connectedMoveCallback). */
GCValue js_node_move_before(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "Failed to execute 'moveBefore' on 'Node': 2 arguments required.");
    NodeId parent = dom_node_of(this_val);
    NodeId node = JS_IsObject(argv[0]) ? dom_node_of(argv[0]) : DOM_NULL;
    if (!parent || !node)
        return JS_ThrowTypeError(ctx, "Failed to execute 'moveBefore' on 'Node': parameter 1 is not of type 'Node'.");
    NodeId child = DOM_NULL;
    if (!JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
        child = JS_IsObject(argv[1]) ? dom_node_of(argv[1]) : DOM_NULL;
        if (!child) return JS_ThrowTypeError(ctx, "Failed to execute 'moveBefore' on 'Node': parameter 2 is not of type 'Node'.");
    }
    /* "ensure pre-move validity" */
    int pk = dom_kind(parent), nk = dom_kind(node);
    const char *bad = NULL;
    if (shadow_including_root(parent) != shadow_including_root(node)) bad = "The node is in another tree.";
    else if (pk != DOM_K_DOCUMENT && pk != DOM_K_FRAGMENT && pk != DOM_K_ELEMENT) bad = "The parent cannot have children.";
    else if (host_including_ancestor(node, parent)) bad = "The node contains the parent.";
    if (bad) return throw_dom_exception(ctx, "HierarchyRequestError", bad);
    if (child && dom_parent(child) != parent)
        return throw_dom_exception(ctx, "NotFoundError", "The reference node is not a child of this node.");
    if (nk != DOM_K_ELEMENT && !dom_kind_is_chardata(nk)) bad = "The node cannot be moved.";
    else if (nk == DOM_K_TEXT && pk == DOM_K_DOCUMENT) bad = "The document cannot have text children.";
    else if (pk == DOM_K_DOCUMENT && nk == DOM_K_ELEMENT) {
        bool doctype_after = false;
        for (NodeId c = child; c; c = dom_next_sibling(c))
            if (dom_kind(c) == DOM_K_DOCTYPE) doctype_after = true;
        for (NodeId c = dom_first_child(parent); c && !bad; c = dom_next_sibling(c))
            if (dom_kind(c) == DOM_K_ELEMENT) bad = "The document has an element already.";
        if (!bad && doctype_after) bad = "The element would come before the doctype.";
    }
    if (bad) return throw_dom_exception(ctx, "HierarchyRequestError", bad);
    NodeId ref = child == node ? dom_next_sibling(node) : child;
    bool connected = dom_api_is_connected(ctx, node);
    bool was = g_dom_moving;
    g_dom_moving = true;
    NodeId old_parent = dom_parent(node);
    if (old_parent) dom_api_remove(ctx, old_parent, node);
    bool ok = dom_api_insert(ctx, parent, node, ref);
    g_dom_moving = was;
    if (!ok) return JS_EXCEPTION;
    /* (custom elements: connectedMoveCallback, or else disconnected and
     * connected again) */
    if (connected && !was) {
        uint32_t *ids = NULL, n = 0;
        collect_ce_candidates(node, &ids, &n);
        for (uint32_t i = 0; i < n; i++) {
            if (!dom_has_wrapper(ids[i])) continue;
            GCValue el = dom_js(ctx, ids[i]);
            GCValue mv = JS_GetPropertyStr(ctx, el, "connectedMoveCallback");
            if (JS_IsFunction(ctx, mv)) {
                invoke_custom_element_callback(ctx, el, "connectedMoveCallback");
            } else {
                invoke_custom_element_callback(ctx, el, "disconnectedCallback");
                invoke_custom_element_callback(ctx, el, "connectedCallback");
            }
        }
        free(ids);
        dom_flush_ce_reactions(ctx);
    }
    return JS_UNDEFINED;
}

// Real replaceChild implementation
GCValue js_node_replaceChild_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0]) ||
        JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])) {
        return JS_ThrowTypeError(ctx, "replaceChild: invalid arguments");
    }
    if (is_attr_value(ctx, argv[0])) return attr_child_error(ctx);
    GCValue new_child = argv[0];
    GCValue old_child = argv[1];
    NodeId parent = dom_node_of(this_val);
    NodeId old_node = dom_node_of(old_child), new_node = dom_node_of(new_child);
    if (!parent || !old_node || !new_node) return JS_ThrowTypeError(ctx, "replaceChild: invalid arguments");
    if (!insert_valid(ctx, parent, new_node, old_node, true)) return JS_EXCEPTION;
    /* (DOM "replace": old_child goes, then new_child goes before what followed
     * it, or what followed new_child when that was it; itself too: out and
     * back in; one record) */
    NodeId ref = dom_next_sibling(old_node);
    if (ref == new_node) ref = dom_next_sibling(new_node);
    /* (new_child out of where it is first: a record of its own) */
    if (dom_kind(new_node) != DOM_K_FRAGMENT && dom_parent(new_node)) {
        GCValue a[1] = { new_child };
        GCValue pj = dom_js(ctx, dom_parent(new_node));
        js_node_removeChild_real(ctx, pj, 1, a);
    }
    mo_batch_begin(ctx);
    if (dom_parent(old_node) == parent) {
        GCValue remove_args[1] = { old_child };
        js_node_removeChild_real(ctx, this_val, 1, remove_args);
    }
    GCValue insert_args[2] = { new_child, dom_js(ctx, ref) };
    GCValue r = js_node_insertBefore_real(ctx, this_val, 2, insert_args);
    mo_batch_end(ctx);
    if (JS_IsException(r)) return r;
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
        /* (an empty one goes first: the next keeps its node) */
        uint32_t clen = 0;
        dom_text(child, &clen);
        if (clen == 0) {
            dom_api_remove_discard(ctx, parent, child);
            child = next;
            continue;
        }
        if (next && dom_kind(next) == DOM_K_TEXT) {
            uint32_t l1 = 0, l2 = 0;
            const char *t1 = dom_text(child, &l1);
            const char *t2 = dom_text(next, &l2);
            uint32_t u1 = g_dom_ranges_live ? dom_utf16_len(t1, l1) : 0;
            char *merged = (char *)malloc(l1 + l2 + 1);
            memcpy(merged, t1, l1);
            memcpy(merged + l1, t2, l2);
            dom_set_text(child, merged, l1 + l2);
            free(merged);
            /* (live ranges in next, or at it, move into child: DOM normalize()) */
            if (g_dom_ranges_live) {
                uint32_t idx = 0;
                for (NodeId x = dom_prev_sibling(next); x; x = dom_prev_sibling(x)) idx++;
                dom_range_merged(child, u1, next, parent, idx);
            }
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
    /* (other trees: disconnected, and before or after by a consistent
     * order of the trees, their roots' ids) */
    if (!a || !b || dom_root(a) != dom_root(b))
        return JS_NewInt32(ctx, 1 /* DISCONNECTED */ | 32 /* IMPLEMENTATION_SPECIFIC */ |
                                    (dom_root(b) < dom_root(a) ? 2 /* PRECEDING */ : 4 /* FOLLOWING */));
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
    g_dom.flags[d] = (uint16_t)(g_dom.flags[s] & (DOMF_CE_CANDIDATE | DOMF_RAW_TEXT | DOMF_VOID | DOMF_SELF_CLOSED |
                                                  DOMF_XML_DOCUMENT));
    /* (a script's "already started" comes along, but a template's contents
     * have not started: their root is a fragment) */
    if (g_dom.flags[s] & DOMF_PARSER_SCRIPT) {
        uint32_t r = s;
        while (g_dom.parent[r]) r = g_dom.parent[r];
        if (g_dom.kind[r] != DOM_K_FRAGMENT) g_dom.flags[d] |= DOMF_PARSER_SCRIPT;
    }
    g_dom.id_atom[d] = g_dom.id_atom[s];
    g_dom.ns[d] = g_dom.ns[s];
    g_dom.prefix[d] = g_dom.prefix[s];
    g_dom.lname[d] = g_dom.lname[s];
    if (g_dom.text[s].len) g_dom.text[d] = dom_str_dup(g_dom.text[s].s, g_dom.text[s].len);
    DomAttrList *l = &g_dom.attrs[s];
    for (int i = 0; i < l->n; i++) {
        DomStr v = g_dom.attrs[s].a[i].value;
        const char *q = dom_attr_qualified_name(&g_dom.attrs[s].a[i]);
        dom_set_attr_at(dom_id_of(d), -1, g_dom.attrs[s].a[i].ns, q, strlen(q), v.s ? v.s : "", v.len);
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
static GCValue attr_clone(JSContextHandle ctx, GCValue attr);

GCValue js_node_cloneNode_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    bool deep = argc > 0 && JS_ToBool(ctx, argv[0]);
    NodeId original = dom_node_of(this_val);
    if (!original && is_attr_value(ctx, this_val)) return attr_clone(ctx, this_val);
    if (!original) return JS_NewObject(ctx);
    /* The clone belongs to the same document as the original (DOM spec:
     * cloneNode keeps the node document; importNode re-documents it). */
    NodeId copy = clone_node(original, deep, dom_owner(original));
    if (dom_kind(original) == DOM_K_DOCUMENT) {
        /* (a document's clone has its URL and content type) */
        GCValue cj = dom_js(ctx, copy);
        static const char *const keep[] = { "URL", "contentType", "baseURI" },
                                *const hidden[] = { "__cyber_url", "__cyber_ct", "__cyber_base" };
        /* (an XMLDocument's clone is one) */
        if (JS_IsObject(cj) && dom_flags(original) & DOMF_XML_DOCUMENT) {
            GCValue xd = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "XMLDocument");
            GCValue xp = JS_IsObject(xd) ? JS_GetPropertyStr(ctx, xd, "prototype") : JS_UNDEFINED;
            if (JS_IsObject(xp)) JS_SetPrototype(ctx, cj, xp);
        }
        for (int i = 0; i < 3 && JS_IsObject(cj); i++)
            JS_DefinePropertyValueStr(ctx, cj, hidden[i], JS_GetPropertyStr(ctx, this_val, keep[i]), JS_PROP_CONFIGURABLE);
        return cj;
    }
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
    if (!b) return JS_ThrowTypeError(ctx, "Failed to execute 'contains' on 'Node': parameter 1 is not of type 'Node'.");
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
    if (n && dom_kind(d) == DOM_K_DOCUMENT) {
        /* (Node.prototype's getter reads the store) */
        g_dom.owner[dom_slot(n)] = dom_slot(d);
        /* (a template's contents: the inert document's) */
        NodeId tc = dom_kind(n) == DOM_K_ELEMENT ? dom_template_content(n) : DOM_NULL;
        if (tc) g_dom.owner[dom_slot(tc)] = dom_slot(template_contents_owner(d));
        return;
    }
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

/* [SameObject]: the node's one live NodeList (live_list.c) */
GCValue js_node_get_childNodes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return live_list_of_node(ctx, this_val, LL_CHILD_NODES);
}

GCValue js_node_get_nodeType(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    /* (not a node: Node.prototype or one inheriting from it, which
     * testharness's is_node() tells from a node by this throwing) */
    if (!n) return JS_ThrowTypeError(ctx, "Illegal invocation");
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

GCValue js_element_get_namespaceURI(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    const char *u = dom_kind(n) == DOM_K_ELEMENT ? dom_ns_uri(dom_ns(n)) : NULL;
    return u ? JS_NewString(ctx, u) : JS_NULL;
}

GCValue js_element_get_prefix(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    const char *p = dom_prefix(dom_node_of(this_val));
    return p ? JS_NewString(ctx, p) : JS_NULL;
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

/* [SameObject]: the node's one live HTMLCollection of its element children */
GCValue js_element_get_children(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    return live_list_of_node(ctx, this_val, LL_CHILDREN);
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
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'getElementsByClassName': 1 argument required.");
    const char *class_name = JS_ToCString(ctx, argv[0]);
    if (!class_name) return JS_EXCEPTION;
    NodeId root = dom_node_of(this_val);
    if (!root) root = g_dom_document;
    /* (live: an HTMLCollection over the root, live_list.c) */
    GCValue list = live_list_new(ctx, LL_CLASS, root, class_name, 0, false, true);
    JS_FreeCString(ctx, class_name);
    return list;
}

/* ============================================================================
 * Attributes
 * ============================================================================ */

/* Copy of an attribute value (the store may change under JS callbacks). */
static char *attr_dup(NodeId n, const char *name) {
    const char *v = dom_attr(n, name);
    return v ? strdup(v) : NULL;
}

/* The handler types a <body> or <frameset> forwards to its window (HTML
 * 8.1.8.2: the window-reflecting body element event handler set and
 * WindowEventHandlers). */
static bool body_forwarded(const char *type) {
    static const char *const k[] = { "blur", "error", "focus", "load", "resize", "scroll", "afterprint", "beforeprint",
                                     "beforeunload", "hashchange", "languagechange", "message", "messageerror",
                                     "offline", "online", "pagehide", "pageshow", "popstate", "rejectionhandled",
                                     "storage", "unhandledrejection", "unload" };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) if (!strcmp(k[i], type)) return true;
    return false;
}

/* An on<type> content attribute changed (HTML 8.1.8.1 "attribute change
 * steps"): the element's handler goes, to be compiled from the attribute
 * when next read (dom_events.js); a <body>'s or <frameset>'s forwarded one
 * is the window's, compiled now (or null when the attribute went). */
static void handler_attr_changed(JSContextHandle ctx, GCValue el, NodeId n, const char *type, const char *value) {
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue symbol = JS_GetPropertyStr(ctx, global, "Symbol");
    GCValue key = JS_NewString(ctx, "cyber.eventHandlers");
    GCValue sym = JS_Call(ctx, JS_GetPropertyStr(ctx, symbol, "for"), symbol, 1, &key);
    if (!JS_IsSymbol(sym)) return;
    JSAtom ha = JS_ValueToAtom(ctx, sym);
    bool fwd = dom_ns(n) == DOM_NS_HTML && (dom_tag_is(n, "body") || dom_tag_is(n, "frameset")) && body_forwarded(type);
    GCValue target = fwd ? global : el;
    GCValue h = JS_GetProperty(ctx, target, ha);
    if (!JS_IsObject(h)) {
        if (!fwd) { JS_FreeAtom(ctx, ha); return; }
        h = JS_NewObjectProto(ctx, JS_NULL);
        JS_DefinePropertyValue(ctx, target, ha, h, JS_PROP_CONFIGURABLE);
    }
    JS_FreeAtom(ctx, ha);
    JSAtom ta = JS_NewAtom(ctx, type);
    if (!fwd) {
        JS_DeleteProperty(ctx, h, ta, 0);
    } else {
        GCValue fn = JS_NULL;
        if (value) {
            /* (window's onerror: its five arguments) */
            bool err = !strcmp(type, "error");
            GCValue args[6] = { JS_NewString(ctx, "event"), JS_NewString(ctx, "source"), JS_NewString(ctx, "lineno"),
                                JS_NewString(ctx, "colno"), JS_NewString(ctx, "error"), JS_NewString(ctx, value) };
            if (!err) args[1] = args[5];
            fn = JS_CallConstructor(ctx, JS_GetPropertyStr(ctx, global, "Function"), err ? 6 : 2, args);
            if (JS_IsException(fn)) {
                JS_GetException(ctx);      /* (a script error, reported nowhere here) */
                fn = JS_NULL;
            }
        }
        JS_SetProperty(ctx, h, ta, fn);
    }
    JS_FreeAtom(ctx, ta);
}

/* (ns: the attribute's namespace; an on* one in a namespace is no handler) */
static void attr_changed_ns(JSContextHandle ctx, GCValue el, NodeId n, const char *name,
                            const char *old_value, const char *new_value, uint32_t ns) {
    dom_mark_dirty(n, DOMF_DIRTY_ATTR);
    if (!ns && name[0] == 'o' && name[1] == 'n' && name[2]) handler_attr_changed(ctx, el, n, name + 2, new_value);
    dom_query_note_attr(n, name);
    invoke_attribute_changed(ctx, el, name, old_value, new_value);
    mo_notify_attribute_ns(ctx, el, name, ns ? dom_name_str(ns) : NULL, old_value);
    /* (a link that has just become a stylesheet loads) */
    if (!ns && (!strcasecmp(name, "rel") || !strcasecmp(name, "href")) && dom_tag_is(n, "link")) dom_maybe_load_stylesheet(ctx, el);
    /* (a media element's src set: it loads; removed: it does not) */
    if (!ns && new_value && !strcasecmp(name, "src") && (dom_tag_is(n, "video") || dom_tag_is(n, "audio")))
        js_video_src_attr_changed(ctx, el, new_value);
    dom_request_layout();
}

static void attr_changed(JSContextHandle ctx, GCValue el, NodeId n, const char *name,
                         const char *old_value, const char *new_value) {
    attr_changed_ns(ctx, el, n, name, old_value, new_value, 0);
}

/* An attribute name argument as the element's methods look it up (DOM
 * 4.9: lower-cased for an HTML element in an HTML document), malloc'd. */
static char *attr_lookup_name(NodeId n, const char *name, size_t len) {
    char *s = malloc(len + 1);
    if (!s) return NULL;
    NodeId doc = dom_owner(n) ? dom_owner(n) : g_dom_document;
    bool lower = dom_ns(n) == DOM_NS_HTML && dom_is_html_document(doc);
    for (size_t i = 0; i < len; i++) s[i] = (char)(lower && name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    s[len] = 0;
    return s;
}

/* A valid attribute local name (DOM 1.4): no ASCII white space, NUL, '/',
 * '=' or '>'. */
static bool valid_attr_name(const char *s, size_t n) {
    if (!n) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == 0 || c == '/' || c == '=' || c == '>')
            return false;
    }
    return true;
}

/* The value of attribute i of n, copied (NULL if none). */
static void attr_detach(JSContextHandle ctx, GCValue el, NodeId n, int idx);

static char *attr_value_at(NodeId n, int i) {
    const DomAttr *a = i >= 0 ? dom_attr_at(n, i) : NULL;
    return a ? strndup(a->value.s ? a->value.s : "", a->value.len) : NULL;
}

/* The name an attribute is shown by (as written; the parser's SVG
 * attributes in their case). */
static const char *attr_output_name(const DomAttr *a) {
    return a->qname ? dom_attr_qualified_name(a) : dom_attr_name_for_output(css_atom_str(a->name));
}

// Element.prototype.setAttribute
GCValue js_element_set_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "Failed to execute 'setAttribute' on 'Element': 2 arguments required.");
    size_t nlen = 0, vlen = 0;
    const char *raw = JS_ToCStringLen(ctx, &nlen, argv[0]);
    if (!raw) return JS_EXCEPTION;
    const char *value = JS_ToCStringLen(ctx, &vlen, argv[1]);
    if (!value) return JS_EXCEPTION;
    if (!valid_attr_name(raw, nlen))
        return throw_dom_exception(ctx, "InvalidCharacterError", "The attribute name is not a valid name.");
    NodeId n = dom_node_of(this_val);
    char *name = n ? attr_lookup_name(n, raw, nlen) : strndup(raw, nlen);
    if (!name) return JS_ThrowOutOfMemory(ctx);
    char *old_value = n ? attr_value_at(n, dom_attr_index_qualified(n, name, nlen)) : NULL;
    // Store the attribute on the object itself too (the legacy DOM's
    // reflection), except for names its interface has.
    if (dom_attr_reflects_as_expando(ctx, n, this_val, name))
        JS_SetPropertyStr(ctx, this_val, name, JS_NewStringLen(ctx, value, vlen));
    n = dom_node_of(this_val);
    if (n) {
        dom_set_attr_at(n, dom_attr_index_qualified(n, name, nlen), 0, name, nlen, value, vlen);
        if (strcasecmp(name, "style") == 0) {
            /* The attribute replaces the live style object's declarations. */
            GCValue style = dom_element_style(ctx, this_val, false);
            if (JS_IsObject(style)) css_style_decl_sync_from_attr(ctx, style, value);
        }
    }
    if (strcmp(name, "src") == 0 && value[0]) {
        capture_url_debug(value, "element_setAttribute_src");
        if (g_dom_dynamic_scripts_enabled && dom_tag_is(n, "script"))
            dom_execute_external_script(ctx, this_val, value);
    }
    attr_changed(ctx, this_val, n, name, old_value, value);
    free(old_value);
    free(name);
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

/* An attribute set from C the way setAttribute sets it (DOMTokenList). */
void dom_element_set_attr(JSContextHandle ctx, NodeId n, const char *name, const char *value) {
    char *old = attr_dup(n, name);
    dom_set_attr_str(n, name, value);
    attr_changed(ctx, dom_js(ctx, n), n, name, old, value);
    free(old);
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
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'getAttribute' on 'Element': 1 argument required.");
    size_t nlen = 0;
    const char *raw = JS_ToCStringLen(ctx, &nlen, argv[0]);
    if (!raw) return JS_EXCEPTION;
    NodeId n = dom_node_of(this_val);
    if (n) {
        char *name = attr_lookup_name(n, raw, nlen);
        const DomAttr *a = name ? dom_attr_at(n, dom_attr_index_qualified(n, name, nlen)) : NULL;
        bool is_id = name && !strcmp(name, "id");
        free(name);
        if (a) return JS_NewStringLen(ctx, a->value.s ? a->value.s : "", a->value.len);
        if (!is_id) return JS_NULL;
    } else if (strcmp(raw, "id") != 0) {
        return JS_NULL;
    }
    // Fall back to a string-valued own data property for `id` only (el.id
    // set directly).  Other own properties (e.g. Polymer's `is`) are not
    // attributes.  Never run accessors here: a reflecting getter such as
    // HTMLAnchorElement.prototype.href calls getAttribute() itself.
    JSAtom atom = JS_NewAtom(ctx, "id");
    JSPropertyDescriptor desc = JS_PROPERTY_DESCRIPTOR_INIT;
    int has = JS_GetOwnProperty(ctx, &desc, this_val, atom);
    JS_FreeAtom(ctx, atom);
    if (has > 0 && !(desc.flags & JS_PROP_GETSET) && JS_IsString(desc.value))
        return desc.value;
    return JS_NULL;
}

// Element.prototype.removeAttribute
GCValue js_element_remove_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'removeAttribute' on 'Element': 1 argument required.");
    size_t nlen = 0;
    const char *raw = JS_ToCStringLen(ctx, &nlen, argv[0]);
    if (!raw) return JS_EXCEPTION;
    NodeId n = dom_node_of(this_val);
    char *name = n ? attr_lookup_name(n, raw, nlen) : strndup(raw, nlen);
    if (!name) return JS_ThrowOutOfMemory(ctx);
    int idx = n ? dom_attr_index_qualified(n, name, nlen) : -1;
    char *old_value = n ? attr_value_at(n, idx) : NULL;
    if (strcmp(name, "id") != 0 && strcmp(name, "class") != 0 && strcmp(name, "style") != 0) {
        JSAtom atom = JS_NewAtom(ctx, name);
        JS_DeleteProperty(ctx, this_val, atom, 0);
        JS_FreeAtom(ctx, atom);
    }
    n = dom_node_of(this_val);
    if (n && idx >= 0) {
        attr_detach(ctx, this_val, n, dom_attr_index_qualified(n, name, nlen));
        dom_remove_attr_at(n, dom_attr_index_qualified(n, name, nlen));
        if (strcasecmp(name, "style") == 0) {
            GCValue style = dom_element_style(ctx, this_val, false);
            if (JS_IsObject(style)) css_style_decl_sync_from_attr(ctx, style, "");
        }
    }
    if (old_value) {
        if (n && name[0] == 'o' && name[1] == 'n' && name[2]) handler_attr_changed(ctx, this_val, n, name + 2, NULL);
        invoke_attribute_changed(ctx, this_val, name, old_value, NULL);
        mo_notify_attribute(ctx, this_val, name, old_value);
        if (n) { dom_mark_dirty(n, DOMF_DIRTY_ATTR); dom_query_note_attr(n, name); }
        dom_request_layout();
    }
    free(old_value);
    free(name);
    return JS_UNDEFINED;
}

// Element.prototype.hasAttribute
GCValue js_element_has_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'hasAttribute' on 'Element': 1 argument required.");
    size_t nlen = 0;
    const char *raw = JS_ToCStringLen(ctx, &nlen, argv[0]);
    if (!raw) return JS_EXCEPTION;
    NodeId n = dom_node_of(this_val);
    char *name = n ? attr_lookup_name(n, raw, nlen) : NULL;
    bool has = name && dom_attr_index_qualified(n, name, nlen) >= 0;
    free(name);
    return JS_NewBool(ctx, has);
}

// Element.prototype.toggleAttribute
GCValue js_element_toggle_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'toggleAttribute' on 'Element': 1 argument required.");
    size_t nlen = 0;
    const char *raw = JS_ToCStringLen(ctx, &nlen, argv[0]);
    if (!raw) return JS_EXCEPTION;
    if (!valid_attr_name(raw, nlen))
        return throw_dom_exception(ctx, "InvalidCharacterError", "The attribute name is not a valid name.");
    NodeId n = dom_node_of(this_val);
    char *name = n ? attr_lookup_name(n, raw, nlen) : NULL;
    bool has = name && dom_attr_index_qualified(n, name, nlen) >= 0;
    bool want = argc > 1 && !JS_IsUndefined(argv[1]) ? JS_ToBool(ctx, argv[1]) : !has;
    GCValue name_v = JS_NewString(ctx, name ? name : raw);
    free(name);
    if (want && !has) {
        GCValue args[2] = { name_v, JS_NewString(ctx, "") };
        if (JS_IsException(js_element_set_attribute(ctx, this_val, 2, args))) return JS_EXCEPTION;
    } else if (!want && has) {
        GCValue args[1] = { name_v };
        if (JS_IsException(js_element_remove_attribute(ctx, this_val, 1, args))) return JS_EXCEPTION;
    }
    dom_request_layout();
    return JS_NewBool(ctx, want);
}

/* A namespace argument for an attribute (null, undefined, "": none). */
static bool attr_ns_arg(JSContextHandle ctx, GCValue v, uint32_t *ns) {
    *ns = 0;
    if (JS_IsNull(v) || JS_IsUndefined(v)) return true;
    size_t n = 0;
    const char *s = JS_ToCStringLen(ctx, &n, v);
    if (!s) return false;
    if (n) *ns = dom_name_intern(s, n);
    return true;
}

/* (DOM "valid namespace prefix": some, with no white space, NUL, / or >) */
static bool valid_ns_prefix(const char *s, size_t n) {
    if (!n) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == 0 || c == '/' || c == '>') return false;
    }
    return true;
}

// Element.prototype.setAttributeNS
GCValue js_element_set_attribute_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "Failed to execute 'setAttributeNS' on 'Element': 3 arguments required.");
    uint32_t ns;
    if (!attr_ns_arg(ctx, argv[0], &ns)) return JS_EXCEPTION;
    size_t qn = 0, vlen = 0;
    const char *q = JS_ToCStringLen(ctx, &qn, argv[1]);
    if (!q) return JS_EXCEPTION;
    char *qname = strndup(q, qn);
    const char *value = JS_ToCStringLen(ctx, &vlen, argv[2]);
    if (!value || !qname) { free(qname); return JS_EXCEPTION; }
    /* (validate and extract, for an attribute) */
    const char *colon = memchr(qname, ':', qn), *local = qname;
    size_t pn = colon ? (size_t)(colon - qname) : 0, ln = qn;
    const char *err = NULL, *kind = NULL;
    if (colon) {
        local = colon + 1;
        ln = qn - pn - 1;
        if (!valid_ns_prefix(qname, pn)) err = "InvalidCharacterError", kind = "The namespace prefix is not valid.";
    }
    if (!err && !valid_attr_name(local, ln)) err = "InvalidCharacterError", kind = "The local name is not valid.";
    if (!err) {
        bool xmlns_name = (qn == 5 && !memcmp(qname, "xmlns", 5)) || (pn == 5 && colon && !memcmp(qname, "xmlns", 5));
        if ((colon && !ns) || (colon && pn == 3 && !memcmp(qname, "xml", 3) && ns != DOM_NS_XML) ||
            (xmlns_name && ns != DOM_NS_XMLNS) || (ns == DOM_NS_XMLNS && !xmlns_name))
            err = "NamespaceError", kind = "The namespace and qualified name do not go together.";
    }
    if (err) {
        free(qname);
        return throw_dom_exception(ctx, err, kind);
    }
    NodeId n = dom_node_of(this_val);
    char *lname = strndup(local, ln);
    int idx = n ? dom_attr_index_ns(n, ns, local, ln) : -1;
    char *old_value = n ? attr_value_at(n, idx) : NULL;
    if (n) dom_set_attr_at(n, idx, ns, qname, qn, value, vlen);
    if (n && lname) attr_changed_ns(ctx, this_val, n, lname, old_value, value, ns);
    free(old_value);
    free(lname);
    free(qname);
    return JS_UNDEFINED;
}

// Element.prototype.getAttributeNS
GCValue js_element_get_attribute_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "Failed to execute 'getAttributeNS' on 'Element': 2 arguments required.");
    uint32_t ns;
    if (!attr_ns_arg(ctx, argv[0], &ns)) return JS_EXCEPTION;
    size_t ln = 0;
    const char *local = JS_ToCStringLen(ctx, &ln, argv[1]);
    if (!local) return JS_EXCEPTION;
    NodeId n = dom_node_of(this_val);
    const DomAttr *a = n ? dom_attr_at(n, dom_attr_index_ns(n, ns, local, ln)) : NULL;
    return a ? JS_NewStringLen(ctx, a->value.s ? a->value.s : "", a->value.len) : JS_NULL;
}

// Element.prototype.removeAttributeNS
GCValue js_element_remove_attribute_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "Failed to execute 'removeAttributeNS' on 'Element': 2 arguments required.");
    uint32_t ns;
    if (!attr_ns_arg(ctx, argv[0], &ns)) return JS_EXCEPTION;
    size_t ln = 0;
    const char *l = JS_ToCStringLen(ctx, &ln, argv[1]);
    if (!l) return JS_EXCEPTION;
    char *local = strndup(l, ln);
    NodeId n = dom_node_of(this_val);
    int idx = n && local ? dom_attr_index_ns(n, ns, local, ln) : -1;
    if (idx >= 0) {
        char *old_value = attr_value_at(n, idx);
        attr_detach(ctx, this_val, n, idx);
        dom_remove_attr_at(n, idx);
        if (!ns && local[0] == 'o' && local[1] == 'n' && local[2]) handler_attr_changed(ctx, this_val, n, local + 2, NULL);
        invoke_attribute_changed(ctx, this_val, local, old_value, NULL);
        mo_notify_attribute_ns(ctx, this_val, local, ns ? dom_name_str(ns) : NULL, old_value);
        dom_mark_dirty(n, DOMF_DIRTY_ATTR);
        dom_query_note_attr(n, local);
        dom_request_layout();
        free(old_value);
    }
    free(local);
    return JS_UNDEFINED;
}

// Element.prototype.hasAttributeNS
GCValue js_element_has_attribute_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "Failed to execute 'hasAttributeNS' on 'Element': 2 arguments required.");
    uint32_t ns;
    if (!attr_ns_arg(ctx, argv[0], &ns)) return JS_EXCEPTION;
    size_t ln = 0;
    const char *local = JS_ToCStringLen(ctx, &ln, argv[1]);
    if (!local) return JS_EXCEPTION;
    NodeId n = dom_node_of(this_val);
    return JS_NewBool(ctx, n && dom_attr_index_ns(n, ns, local, ln) >= 0);
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

/* A SyntaxError for a selector that doesn't parse (frees it). */
static GCValue bad_selector(JSContextHandle ctx, const char *s) {
    char msg[160];
    snprintf(msg, sizeof msg, "'%.120s' is not a valid selector", s);
    JS_FreeCString(ctx, s);
    return throw_dom_exception(ctx, "SyntaxError", msg);
}

/* A selector with :target in it: the fragment it matches, the realm's
 * location's (read only then). */
static void query_note_target(JSContextHandle ctx, const char *s, size_t n) {
    if (!n || !strstr(s, ":target")) return;     /* (s ends in a NUL) */
    GCValue loc = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "location");
    GCValue h = JS_IsObject(loc) ? JS_GetPropertyStr(ctx, loc, "hash") : JS_UNDEFINED;
    size_t hl = 0;
    const char *hs = JS_IsString(h) ? JS_ToCStringLen(ctx, &hl, h) : NULL;
    if (hs && hl > 1 && hs[0] == '#') dom_query_set_target(hs + 1, hl - 1);
    else dom_query_set_target(NULL, 0);
    if (hs) JS_FreeCString(ctx, hs);
}

/* querySelector[All] over a scope (§7 "Queries"): compiled selectors, index
 * candidates, document order. */
static GCValue query_first(JSContextHandle ctx, NodeId scope, GCValue sel, bool include_self) {
    CP_ACC("querySelector");
    size_t n = 0;
    const char *s = JS_ToCStringLen(ctx, &n, sel);
    if (!s) return JS_EXCEPTION;
    if (!dom_query_valid(s, n)) return bad_selector(ctx, s);
    query_note_target(ctx, s, n);
    NodeId r = dom_query_first(scope, s, n, include_self);
    JS_FreeCString(ctx, s);
    return dom_js(ctx, r);
}

static GCValue query_all(JSContextHandle ctx, NodeId scope, GCValue sel, bool include_self) {
    CP_ACC("querySelectorAll");
    size_t n = 0;
    const char *s = JS_ToCStringLen(ctx, &n, sel);
    if (!s) return JS_EXCEPTION;
    if (!dom_query_valid(s, n)) return bad_selector(ctx, s);
    query_note_target(ctx, s, n);
    DomNodeList l = dom_query_all(scope, s, n, include_self);
    JS_FreeCString(ctx, s);
    GCValue arr = nodes_to_list(ctx, l.ids, l.n, "NodeList");
    dom_node_list_free(&l);
    return arr;
}

// Document.querySelector
GCValue js_document_querySelector(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'querySelector': 1 argument required.");
    GCValue doc_elem = JS_GetPropertyStr(ctx, this_val, "documentElement");
    if (JS_IsNull(doc_elem) || JS_IsUndefined(doc_elem)) return JS_NULL;
    /* null when nothing matches.  (This used to return a fake <div>, which
     * made "is there a continuation trigger?" checks always true.) */
    return query_first(ctx, dom_node_of(doc_elem), argv[0], true);
}

// Document.querySelectorAll
GCValue js_document_querySelectorAll(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'querySelectorAll': 1 argument required.");
    GCValue doc_elem = JS_GetPropertyStr(ctx, this_val, "documentElement");
    if (JS_IsNull(doc_elem) || JS_IsUndefined(doc_elem)) return JS_NewArray(ctx);
    return query_all(ctx, dom_node_of(doc_elem), argv[0], true);
}

// Element.prototype.click
GCValue js_element_click(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

bool matches_selector(JSContextHandle ctx, GCValue elem, const char* selector) {
    if (!selector) return false;
    query_note_target(ctx, selector, strlen(selector));
    return dom_query_matches(dom_node_of(elem), selector, strlen(selector));
}

// Real querySelector implementation (the legacy matcher included the scope
// element itself; kept for compatibility).
GCValue js_element_querySelector_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'querySelector': 1 argument required.");
    return query_first(ctx, dom_node_of(this_val), argv[0], false);
}

GCValue js_element_querySelectorAll_real(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'querySelectorAll': 1 argument required.");
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

// (element.dataset: dom_string_map.c)

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
        /* (HTML's, SVG's and MathML's by local name, others' qualified name) */
        const char *tag = dom_local_name(n), *prefix = dom_prefix(n);
        uint32_t ns = dom_ns(n);
        if (prefix && ns != DOM_NS_HTML && ns != DOM_NS_SVG && ns != DOM_NS_MATHML) tag = dom_node_name(n);
        sb_str(b, "<");
        sb_str(b, tag);
        for (int i = 0; i < dom_attr_count(n); i++) {
            const DomAttr *a = dom_attr_at(n, i);
            sb_str(b, " ");
            sb_str(b, attr_output_name(a));
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
    /* (DOM "replace all": one childList record) */
    mo_batch_begin(ctx);
    html_element_set_inner_html_len(ctx, this_val, html, len);
    mo_batch_end(ctx);
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
    /* (DOM "replace": one childList record) */
    mo_batch_begin(ctx);
    while (dom_first_child(temp)) {
        NodeId c = dom_first_child(temp);
        dom_detach(c, false);
        if (!dom_api_insert(ctx, parent, c, n)) break;
    }
    dom_free_subtree(temp);
    if (dom_parent(n) == parent) dom_api_remove_discard(ctx, parent, n);
    mo_batch_end(ctx);
    dom_request_layout();
    return JS_UNDEFINED;
}

// ============================================================================
// Node Content Getters/Setters
// ============================================================================

static void collect_text(NodeId n, SBuf *b) {
    uint32_t r = dom_slot(n);
    for (uint32_t s = r; s; s = dom_next_in_subtree(s, r))
        if ((g_dom.kind[s] == DOM_K_TEXT || g_dom.kind[s] == DOM_K_CDATA) && g_dom.text[s].len)
            sb_put(b, g_dom.text[s].s, g_dom.text[s].len);
}

// Node.prototype.textContent getter
GCValue js_node_get_text_content(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    if (!n) return JS_NewString(ctx, "");
    int k = dom_kind(n);
    if (k == DOM_K_DOCUMENT || k == DOM_K_DOCTYPE) return JS_NULL;
    if (dom_kind_is_chardata(k)) {
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
    /* (DOMString?: null and undefined are the empty string) */
    const char *text = JS_IsNull(argv[0]) || JS_IsUndefined(argv[0]) ? "" : JS_ToCStringLen(ctx, &tlen, argv[0]);
    if (!text) { text = ""; tlen = 0; }
    NodeId n = dom_node_of(this_val);
    if (!n) return JS_UNDEFINED;
    int k = dom_kind(n);
    if (k == DOM_K_DOCUMENT || k == DOM_K_DOCTYPE) return JS_UNDEFINED;
    // For text/comment nodes, textContent behaves like nodeValue/data: update
    // the text and fire a characterData mutation (polyfill schedulers rely
    // on this as a microtask trigger).
    if (dom_kind_is_chardata(k)) {
        uint32_t olen = 0;
        const char *old = dom_text(n, &olen);
        char *old_copy = strndup(old, olen);
        /* (its live ranges: DOM "replace data", all of it unless a
         * CharacterData method said which part) */
        RangeReplace rr = g_replace;
        g_replace.set = false;
        if (g_dom_ranges_live && !rr.set) rr = (RangeReplace){ true, 0, dom_utf16_len(old, olen), dom_utf16_len(text, tlen) };
        dom_set_text(n, text, tlen);
        dom_mark_dirty(n, DOMF_DIRTY_TEXT);
        if (g_dom_ranges_live) dom_range_replaced(n, rr.off, rr.count, rr.len);
        mo_notify_character_data(ctx, this_val, old_copy);
        free(old_copy);
        return JS_UNDEFINED;
    }
    char *copy = (char *)malloc(tlen + 1);   /* (not strndup: the text may hold a NUL) */
    if (!copy) return JS_ThrowOutOfMemory(ctx);
    memcpy(copy, text, tlen);
    copy[tlen] = 0;
    mo_batch_begin(ctx);
    while (dom_first_child(n)) {
        if (!dom_api_remove_discard(ctx, n, dom_first_child(n))) break;
    }
    if (tlen) {
        NodeId t = dom_create_chardata(DOM_K_TEXT, copy, tlen, dom_owner(n));
        dom_api_insert(ctx, n, t, DOM_NULL);
    }
    mo_batch_end(ctx);
    free(copy);
    dom_request_layout();
    return JS_UNDEFINED;
}

// Node.prototype.nodeValue getter
GCValue js_node_get_node_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    int k = dom_kind(n);
    if (dom_kind_is_chardata(k)) {
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
    if (dom_kind_is_chardata(k)) {
        /* (nodeValue is a DOMString?, data [LegacyNullToEmptyString]: null is "") */
        size_t tlen = 0;
        /* (DOMString?: null and undefined are the empty string) */
    const char *text = JS_IsNull(argv[0]) || JS_IsUndefined(argv[0]) ? "" : JS_ToCStringLen(ctx, &tlen, argv[0]);
        if (!text) { text = ""; tlen = 0; }
        uint32_t olen = 0;
        const char *old = dom_text(n, &olen);
        char *old_copy = strndup(old, olen);
        /* (its live ranges: DOM "replace data", all of it unless a
         * CharacterData method said which part) */
        RangeReplace rr = g_replace;
        g_replace.set = false;
        if (g_dom_ranges_live && !rr.set) rr = (RangeReplace){ true, 0, dom_utf16_len(old, olen), dom_utf16_len(text, tlen) };
        dom_set_text(n, text, tlen);
        dom_mark_dirty(n, DOMF_DIRTY_TEXT);
        if (g_dom_ranges_live) dom_range_replaced(n, rr.off, rr.count, rr.len);
        /* NOTE: do NOT mirror into "data"/"textContent" JS properties: a [[Set]]
         * on "textContent" re-invokes polyfill setters (infinite recursion). */
        mo_notify_character_data(ctx, this_val, old_copy);
        free(old_copy);
    }
    return JS_UNDEFINED;
}

void dom_api_note_replace_data(uint32_t off, uint32_t count, uint32_t len) {
    g_replace = (RangeReplace){ true, off, count, len };
}

// CharacterData.prototype.data getter/setter (used by Text/Comment nodes).
GCValue js_node_get_data(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return js_node_get_node_value(ctx, this_val, argc, argv);
}

/* data: [LegacyNullToEmptyString] DOMString (undefined is "undefined";
 * nodeValue's DOMString? makes it "") */
GCValue js_node_set_data(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue v = argc > 0 && JS_IsUndefined(argv[0]) ? JS_NewString(ctx, "undefined") : argc > 0 ? argv[0] : JS_UNDEFINED;
    return js_node_set_node_value(ctx, this_val, argc, &v);
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
    if (JS_IsException(range)) return range;
    GCValue args[2] = { argv[0], argc > 1 ? argv[1] : JS_NewInt32(ctx, 0) };
    GCValue r = js_range_set_start(ctx, range, 2, args);
    if (JS_IsException(r)) return r;
    JS_SetPropertyStr(ctx, this_val, "__range", range);
    selection_update_from_range(ctx, this_val);
    return JS_UNDEFINED;
}

GCValue js_selection_extend(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    GCValue range = JS_GetPropertyStr(ctx, this_val, "__range");
    if (JS_IsNull(range) || JS_IsUndefined(range)) return JS_UNDEFINED;
    GCValue r = js_range_set_end(ctx, range, 2, argv);
    if (JS_IsException(r)) return r;
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
/* ---- Attr (DOM 4.9): its attributes are Attr.prototype's getters over
 * the object's hidden fields (__a_<name>); Attr.prototype inherits
 * Node.prototype ---- */

static void attr_set(JSContextHandle ctx, GCValue attr, const char *field, GCValue v) {
    char key[40];
    snprintf(key, sizeof key, "__a_%s", field);
    JS_DefinePropertyValueStr(ctx, attr, key, v, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

static const char *const k_attr_fields[] = { "name", "nodeName", "namespaceURI", "localName", "prefix", "nodeType",
                                             "specified", "ownerElement", "ownerDocument" };

static GCValue js_attr_field_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    (void)argc; (void)argv;
    char key[40];
    snprintf(key, sizeof key, "__a_%s", k_attr_fields[magic]);
    JSAtom a = JS_NewAtom(ctx, key), n = JS_NewAtom(ctx, "__a_name");
    int own = JS_IsObject(this_val) ? JS_GetOwnProperty(ctx, NULL, this_val, n) : 0;
    JS_FreeAtom(ctx, n);
    if (own <= 0) {
        JS_FreeAtom(ctx, a);
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    }
    GCValue v = JS_GetProperty(ctx, this_val, a);
    JS_FreeAtom(ctx, a);
    /* (an owned Attr's document is its element's: it moves with adoption) */
    if (!strcmp(k_attr_fields[magic], "ownerDocument")) {
        GCValue el = JS_GetPropertyStr(ctx, this_val, "__a_ownerElement");
        if (JS_IsObject(el)) return JS_GetPropertyStr(ctx, el, "ownerDocument");
    }
    return v;
}

static GCValue js_attr_value_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);
static GCValue js_attr_value_set(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

static GCValue js_attr_ctor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* The Attr interface on the global (its prototype Node's). */
/* A control's validity for :valid and :invalid (form_controls.js has the
 * constraints): 0 not a candidate for validation, 1 valid, 2 invalid. */
static int control_validity(NodeId n) {
    extern JSContextHandle g_js_context;
    JSContextHandle ctx = g_js_context;
    GCValue el = dom_js(ctx, n);
    if (!JS_IsObject(el) || JS_ToBool(ctx, JS_GetPropertyStr(ctx, el, "willValidate")) <= 0) return 0;
    GCValue v = JS_GetPropertyStr(ctx, el, "validity");
    if (!JS_IsObject(v)) return 0;
    return JS_ToBool(ctx, JS_GetPropertyStr(ctx, v, "valid")) > 0 ? 1 : 2;
}

/* The Referer for a request from this document, its URL in url: changed as
 * the last <meta name=referrer> in effect says (empty: none is sent). */
void dom_referrer_apply(JSContextHandle ctx, char *url, size_t cap) {
    NodeId doc = dom_node_of(JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "document"));
    if (!doc) return;
    static const char sel[] = "meta[name=referrer i]";
    DomNodeList l = dom_query_all(doc, sel, sizeof sel - 1, false);
    const char *policy = NULL;
    for (uint32_t i = 0; i < l.n; i++)
        if (!(g_dom.flags[dom_slot(l.ids[i])] & DOMF_META_PENDING)) {
            const char *c = dom_get_attr_str(l.ids[i], "content");
            if (c && c[0]) policy = c;
        }
    dom_node_list_free(&l);
    if (!policy) return;
    if (!strcasecmp(policy, "no-referrer") || !strcasecmp(policy, "never")) {
        url[0] = 0;
    } else if (!strcasecmp(policy, "origin") || !strcasecmp(policy, "strict-origin")) {
        /* (scheme://host:port/) */
        char *p = strstr(url, "://");
        p = p ? strchr(p + 3, '/') : NULL;
        if (p && (size_t)(p + 1 - url) < cap) p[1] = 0;
    }
}

void attr_api_install(JSContextHandle ctx, GCValue global) {
    g_dom_query_validity = control_validity;
    GCValue node = JS_GetPropertyStr(ctx, global, "Node");
    GCValue np = JS_IsObject(node) ? JS_GetPropertyStr(ctx, node, "prototype") : JS_UNDEFINED;
    GCValue proto = JS_NewObject(ctx);
    if (JS_IsObject(np)) JS_SetPrototype(ctx, proto, np);
    GCValue c = JS_NewCFunction2(ctx, js_attr_ctor, "Attr", 0, JS_CFUNC_constructor_or_func, 0);
    JS_SetConstructor(ctx, c, proto);
    if (JS_IsObject(node)) JS_SetPrototype(ctx, c, node);
    for (int i = 0; i < (int)(sizeof k_attr_fields / sizeof k_attr_fields[0]); i++) {
        char gn[48];
        snprintf(gn, sizeof gn, "get %s", k_attr_fields[i]);
        JSAtom a = JS_NewAtom(ctx, k_attr_fields[i]);
        JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunctionMagic(ctx, js_attr_field_get, gn, 0, JS_CFUNC_generic_magic, i),
                                JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
    }
    static const char *const vnames[] = { "value", "nodeValue", "textContent" };
    for (int i = 0; i < 3; i++) {
        JSAtom at = JS_NewAtom(ctx, vnames[i]);
        JS_DefinePropertyGetSet(ctx, proto, at, JS_NewCFunction(ctx, js_attr_value_get, vnames[i], 0),
                                JS_NewCFunction(ctx, js_attr_value_set, vnames[i], 1),
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, at);
    }
    GCValue sym = JS_GetPropertyStr(ctx, global, "Symbol");
    JSAtom tag = JS_ValueToAtom(ctx, JS_GetPropertyStr(ctx, sym, "toStringTag"));
    JS_DefinePropertyValue(ctx, proto, tag, JS_NewString(ctx, "Attr"), JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, tag);
    JS_DefinePropertyValueStr(ctx, global, "Attr", c, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    GCValue win = JS_GetPropertyStr(ctx, global, "window");
    if (JS_IsObject(win) && !JS_StrictEq(ctx, win, global))
        JS_DefinePropertyValueStr(ctx, win, "Attr", c, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

// Element.attributes / NamedNodeMap helpers
// ============================================================================

static GCValue create_attr_obj(JSContextHandle ctx, GCValue owner_element,
                               const char *name, const char *value);

/* An Attr's clone (DOM "clone a node"): its names and value, no owner. */
static GCValue attr_clone(JSContextHandle ctx, GCValue attr) {
    const char *name = JS_ToCString(ctx, JS_GetPropertyStr(ctx, attr, "__a_name"));
    const char *value = JS_ToCString(ctx, JS_GetPropertyStr(ctx, attr, "value"));
    GCValue c = create_attr_obj(ctx, JS_NULL, name ? name : "", value ? value : "");
    if (name) JS_FreeCString(ctx, name);
    if (value) JS_FreeCString(ctx, value);
    if (JS_IsException(c)) return c;
    static const char *const f[] = { "namespaceURI", "prefix", "localName", "ownerDocument" };
    for (int i = 0; i < 4; i++) attr_set(ctx, c, f[i], JS_GetPropertyStr(ctx, attr, f[i]));
    JS_DefinePropertyValueStr(ctx, c, "__value", JS_GetPropertyStr(ctx, attr, "value"), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    return c;
}

/* ---- an element's Attr objects ----
 * One Attr per attribute while it is the element's: the element keeps them,
 * in the order made, in a hidden array (__cyber_attrs) found by namespace
 * and local name.  Removing the attribute detaches its Attr (it keeps the
 * value and loses its owner). */

static GCValue attr_list(JSContextHandle ctx, GCValue el, bool make) {
    GCValue l = JS_GetPropertyStr(ctx, el, "__cyber_attrs");
    if (JS_IsObject(l) || !make) return l;
    l = JS_NewArray(ctx);
    JS_DefinePropertyValueStr(ctx, el, "__cyber_attrs", l, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    return l;
}

static uint32_t attr_list_len(JSContextHandle ctx, GCValue l) {
    uint32_t n = 0;
    if (JS_IsObject(l)) JS_ToUint32(ctx, &n, JS_GetPropertyStr(ctx, l, "length"));
    return n;
}

/* The index in l of the Attr for (ns, local) (ns NULL: none), or -1. */
static int attr_list_find(JSContextHandle ctx, GCValue l, const char *ns, const char *local) {
    uint32_t n = attr_list_len(ctx, l);
    for (uint32_t i = 0; i < n; i++) {
        GCValue a = JS_GetPropertyUint32(ctx, l, i);
        GCValue an = JS_GetPropertyStr(ctx, a, "__a_namespaceURI"), al = JS_GetPropertyStr(ctx, a, "__a_localName");
        const char *as = JS_IsString(an) ? JS_ToCString(ctx, an) : NULL, *ls = JS_ToCString(ctx, al);
        bool eq = (ns ? as && !strcmp(as, ns) : !as) && ls && !strcmp(ls, local);
        if (as) JS_FreeCString(ctx, as);
        if (ls) JS_FreeCString(ctx, ls);
        if (eq) return (int)i;
    }
    return -1;
}

static void attr_list_drop(JSContextHandle ctx, GCValue l, int k) {
    uint32_t n = attr_list_len(ctx, l);
    if (k < 0 || (uint32_t)k >= n) return;
    for (uint32_t i = (uint32_t)k; i + 1 < n; i++) JS_SetPropertyUint32(ctx, l, i, JS_GetPropertyUint32(ctx, l, i + 1));
    JS_SetPropertyStr(ctx, l, "length", JS_NewUint32(ctx, n - 1));
}

static const char *attr_ns_str(const DomAttr *a) { return a->ns ? dom_name_str(a->ns) : NULL; }
static const char *attr_local_str(const DomAttr *a) { return a->ns ? dom_attr_local_name(a) : attr_output_name(a); }

/* Before attribute idx of el (node n) goes: its Attr, if made, keeps the
 * value and has no owner. */
static void attr_detach(JSContextHandle ctx, GCValue el, NodeId n, int idx) {
    const DomAttr *a = dom_attr_at(n, idx);
    GCValue l = a ? attr_list(ctx, el, false) : JS_UNDEFINED;
    if (!JS_IsObject(l)) return;
    int k = attr_list_find(ctx, l, attr_ns_str(a), attr_local_str(a));
    if (k < 0) return;
    GCValue attr = JS_GetPropertyUint32(ctx, l, (uint32_t)k);
    JS_DefinePropertyValueStr(ctx, attr, "__value", JS_NewStringLen(ctx, a->value.s ? a->value.s : "", a->value.len),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    attr_set(ctx, attr, "ownerElement", JS_NULL);
    attr_list_drop(ctx, l, k);
}

static GCValue attr_make_at(JSContextHandle ctx, GCValue el, const DomAttr *a);

/* The Attr object of attribute i of node (owner element el). */
static GCValue attr_obj_at(JSContextHandle ctx, GCValue el, NodeId node, int i) {
    const DomAttr *a = dom_attr_at(node, i);
    if (!a) return JS_NULL;
    GCValue l = attr_list(ctx, el, true);
    int k = attr_list_find(ctx, l, attr_ns_str(a), attr_local_str(a));
    if (k >= 0) return JS_GetPropertyUint32(ctx, l, (uint32_t)k);
    GCValue made = attr_make_at(ctx, el, a);
    if (JS_IsObject(made)) JS_SetPropertyUint32(ctx, l, attr_list_len(ctx, l), made);
    return made;
}

static GCValue attr_make_at(JSContextHandle ctx, GCValue el, const DomAttr *a) {
    char *v = strndup(a->value.s ? a->value.s : "", a->value.len);
    GCValue attr = create_attr_obj(ctx, el, attr_output_name(a), v);
    free(v);
    if (JS_IsException(attr) || !a->ns) return attr;
    size_t pl = 0;
    const char *p = dom_attr_prefix(a, &pl);
    attr_set(ctx, attr, "namespaceURI", JS_NewString(ctx, dom_name_str(a->ns)));
    attr_set(ctx, attr, "prefix", p ? JS_NewStringLen(ctx, p, pl) : JS_NULL);
    attr_set(ctx, attr, "localName", JS_NewString(ctx, dom_attr_local_name(a)));
    return attr;
}

/* An Attr's value (also its nodeValue and textContent): its owner element's
 * attribute while it has one, else what it holds (__value). */
static GCValue js_attr_value_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue el = JS_GetPropertyStr(ctx, this_val, "ownerElement");
    if (JS_IsObject(el) && dom_node_of(el)) {
        GCValue a[2] = { JS_GetPropertyStr(ctx, this_val, "namespaceURI"), JS_GetPropertyStr(ctx, this_val, "localName") };
        GCValue v = JS_Call(ctx, JS_GetPropertyStr(ctx, el, "getAttributeNS"), el, 2, a);
        if (JS_IsString(v)) return v;
    }
    GCValue v = JS_GetPropertyStr(ctx, this_val, "__value");
    return JS_IsString(v) ? v : JS_NewString(ctx, "");
}

static GCValue js_attr_value_set(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue v = argc > 0 && !JS_IsNull(argv[0]) ? JS_ToString(ctx, argv[0]) : JS_NewString(ctx, "");
    if (JS_IsException(v)) return v;
    JS_DefinePropertyValueStr(ctx, this_val, "__value", v, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    GCValue el = JS_GetPropertyStr(ctx, this_val, "ownerElement");
    if (JS_IsObject(el) && dom_node_of(el)) {
        GCValue a[3] = { JS_GetPropertyStr(ctx, this_val, "namespaceURI"), JS_GetPropertyStr(ctx, this_val, "name"), v };
        GCValue r = JS_Call(ctx, JS_GetPropertyStr(ctx, el, "setAttributeNS"), el, 3, a);
        if (JS_IsException(r)) return r;
    }
    return JS_UNDEFINED;
}

static GCValue create_attr_obj(JSContextHandle ctx, GCValue owner_element,
                               const char *name, const char *value) {
    GCValue attr = JS_NewObject(ctx);
    if (JS_IsException(attr)) return attr;
    attr_set(ctx, attr, "name", JS_NewString(ctx, name ? name : ""));
    JS_DefinePropertyValueStr(ctx, attr, "__value", JS_NewString(ctx, value ? value : ""),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    attr_set(ctx, attr, "nodeName", JS_NewString(ctx, name ? name : ""));
    attr_set(ctx, attr, "namespaceURI", JS_NULL);
    attr_set(ctx, attr, "localName", JS_NewString(ctx, name ? name : ""));
    attr_set(ctx, attr, "prefix", JS_NULL);
    attr_set(ctx, attr, "nodeType", JS_NewInt32(ctx, 2));
    attr_set(ctx, attr, "specified", JS_TRUE);
    attr_set(ctx, attr, "ownerElement", owner_element);
    attr_set(ctx, attr, "ownerDocument", JS_IsObject(owner_element) ? JS_GetPropertyStr(ctx, owner_element, "ownerDocument")
                                                                    : JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "document"));
    /* (an Attr, when the realm has the interface) */
    GCValue proto = global_proto(ctx, "Attr");
    if (JS_IsObject(proto)) JS_SetPrototype(ctx, attr, proto);
    return attr;
}

/* ---- Attr nodes ----
 * An Attr is an object over its owner element's attribute (create_attr_obj:
 * value reads and writes through); one with no owner holds its value. */

static GCValue call_method(JSContextHandle ctx, GCValue obj, const char *m, int argc, GCValue *argv) {
    return JS_Call(ctx, JS_GetPropertyStr(ctx, obj, m), obj, argc, argv);
}

/* document.createAttribute(localName): lower-cased in an HTML document. */
GCValue js_document_create_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (SAFE_VALID(dom_realm_of_node(dom_node_of(this_val)))) ctx = dom_realm_of_node(dom_node_of(this_val));
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'createAttribute' on 'Document': 1 argument required.");
    size_t n = 0;
    const char *raw = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!raw) return JS_EXCEPTION;
    if (!valid_attr_name(raw, n)) return throw_dom_exception(ctx, "InvalidCharacterError", "The attribute name is not a valid name.");
    char *name = strndup(raw, n);
    if (!name) return JS_ThrowOutOfMemory(ctx);
    NodeId d = dom_node_of(this_val);
    if (dom_is_html_document(d))
        for (char *p = name; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    GCValue attr = create_attr_obj(ctx, JS_NULL, name, "");
    free(name);
    if (JS_IsObject(attr)) attr_set(ctx, attr, "ownerDocument", this_val);
    return attr;
}

static bool valid_ns_prefix(const char *s, size_t n);

/* document.createAttributeNS(namespace, qualifiedName) */
GCValue js_document_create_attribute_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "Failed to execute 'createAttributeNS' on 'Document': 2 arguments required.");
    size_t n = 0;
    const char *q = JS_ToCStringLen(ctx, &n, argv[1]);
    if (!q) return JS_EXCEPTION;
    const char *colon = memchr(q, ':', n);
    /* (a prefix may hold what a local name can't, DOM "validate and extract") */
    if (colon ? !valid_ns_prefix(q, (size_t)(colon - q)) || !valid_attr_name(colon + 1, n - (size_t)(colon - q) - 1) : !valid_attr_name(q, n))
        return throw_dom_exception(ctx, "InvalidCharacterError", "The attribute name is not a valid name.");
    GCValue attr = create_attr_obj(ctx, JS_NULL, q, "");
    if (JS_IsException(attr)) return attr;
    attr_set(ctx, attr, "ownerDocument", this_val);
    GCValue ns = JS_IsNull(argv[0]) || JS_IsUndefined(argv[0]) ? JS_NULL : JS_ToString(ctx, argv[0]);
    if (JS_IsString(ns)) {
        size_t nl = 0;
        const char *nss = JS_ToCStringLen(ctx, &nl, ns);
        if (nss && nl == 0) ns = JS_NULL;
    }
    if (colon && JS_IsNull(ns)) return throw_dom_exception(ctx, "NamespaceError", "A prefix needs a namespace.");
    attr_set(ctx, attr, "namespaceURI", ns);
    attr_set(ctx, attr, "prefix", colon ? JS_NewStringLen(ctx, q, (size_t)(colon - q)) : JS_NULL);
    attr_set(ctx, attr, "localName", colon ? JS_NewString(ctx, colon + 1) : JS_NewStringLen(ctx, q, n));
    return attr;
}

/* element.getAttributeNode(name) / getAttributeNodeNS(namespace, localName) */
static GCValue attr_node_get(JSContextHandle ctx, GCValue el, bool ns_form, int argc, GCValue *argv) {
    NodeId node = dom_node_of(el);
    if (!node) return JS_NULL;
    int count = dom_attr_count(node);
    for (int i = 0; i < count; i++) {
        GCValue a = attr_obj_at(ctx, el, node, i);
        if (!JS_IsObject(a)) continue;
        if (ns_form) {
            GCValue ns = argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]) ? JS_ToString(ctx, argv[0]) : JS_NULL;
            GCValue ans = JS_GetPropertyStr(ctx, a, "namespaceURI");
            bool ns_eq = (JS_IsNull(ns) || (JS_IsString(ns) && JS_ToCString(ctx, ns)[0] == 0)) ? JS_IsNull(ans) : JS_StrictEq(ctx, ns, ans);
            if (ns_eq && argc > 1 && JS_StrictEq(ctx, JS_GetPropertyStr(ctx, a, "localName"), JS_ToString(ctx, argv[1]))) return a;
        } else {
            GCValue v = call_method(ctx, el, "getAttribute", argc, argv);
            if (JS_IsNull(v) || JS_IsException(v)) return JS_IsException(v) ? v : JS_NULL;
            /* (the first attribute whose qualified name matches) */
            GCValue want = argc > 0 ? JS_ToString(ctx, argv[0]) : JS_NewString(ctx, "undefined");
            const char *w = JS_ToCString(ctx, want), *nm = JS_ToCString(ctx, JS_GetPropertyStr(ctx, a, "name"));
            if (w && nm && (dom_is_html_document(dom_owner(node)) && dom_ns(node) == DOM_NS_HTML ? !strcasecmp(w, nm) : !strcmp(w, nm)))
                return a;
        }
    }
    return JS_NULL;
}

GCValue js_element_get_attribute_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return attr_node_get(ctx, this_val, false, argc, argv);
}

GCValue js_element_get_attribute_node_ns(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return attr_node_get(ctx, this_val, true, argc, argv);
}

/* element.setAttributeNode(attr) (and NS): its value set on this element,
 * which becomes its owner; the Attr it replaces, or null. */
GCValue js_element_set_attribute_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsObject(argv[0]) || JS_IsUndefined(JS_GetPropertyStr(ctx, argv[0], "localName")))
        return JS_ThrowTypeError(ctx, "Failed to execute 'setAttributeNode' on 'Element': parameter 1 is not of type 'Attr'.");
    GCValue attr = argv[0];
    GCValue owner = JS_GetPropertyStr(ctx, attr, "ownerElement");
    if (JS_IsObject(owner) && !JS_StrictEq(ctx, owner, this_val))
        return throw_dom_exception(ctx, "InUseAttributeError", "The attribute is in use by another element.");
    GCValue ns = JS_GetPropertyStr(ctx, attr, "namespaceURI");
    GCValue a2[2] = { ns, JS_GetPropertyStr(ctx, attr, "localName") };
    GCValue old = attr_node_get(ctx, this_val, true, 2, a2);
    if (JS_IsObject(old) && JS_StrictEq(ctx, old, attr)) return attr;
    GCValue value = JS_GetPropertyStr(ctx, attr, "value");
    if (JS_IsObject(old)) {
        /* (the replaced Attr keeps its value and has no owner now) */
        JS_DefinePropertyValueStr(ctx, old, "__value", JS_GetPropertyStr(ctx, old, "value"), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        attr_set(ctx, old, "ownerElement", JS_NULL);
    }
    GCValue a3[3] = { ns, JS_GetPropertyStr(ctx, attr, "name"), value };
    GCValue r = call_method(ctx, this_val, "setAttributeNS", 3, a3);
    if (JS_IsException(r)) return r;
    attr_set(ctx, attr, "ownerElement", this_val);
    attr_set(ctx, attr, "ownerDocument", JS_GetPropertyStr(ctx, this_val, "ownerDocument"));
    {
        GCValue l = attr_list(ctx, this_val, true);
        const char *nss = JS_IsString(ns) ? JS_ToCString(ctx, ns) : NULL;
        const char *ls = JS_ToCString(ctx, a2[1]);
        if (ls) attr_list_drop(ctx, l, attr_list_find(ctx, l, nss && *nss ? nss : NULL, ls));
        if (nss) JS_FreeCString(ctx, nss);
        if (ls) JS_FreeCString(ctx, ls);
        JS_SetPropertyUint32(ctx, l, attr_list_len(ctx, l), attr);
    }
    return JS_IsObject(old) ? old : JS_NULL;
}

/* element.removeAttributeNode(attr) */
GCValue js_element_remove_attribute_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "Failed to execute 'removeAttributeNode' on 'Element': parameter 1 is not of type 'Attr'.");
    GCValue attr = argv[0];
    GCValue ns = JS_GetPropertyStr(ctx, attr, "namespaceURI");
    GCValue a2[2] = { ns, JS_GetPropertyStr(ctx, attr, "localName") };
    GCValue have = attr_node_get(ctx, this_val, true, 2, a2);
    if (!JS_IsObject(have) || !JS_StrictEq(ctx, JS_GetPropertyStr(ctx, attr, "ownerElement"), this_val))
        return throw_dom_exception(ctx, "NotFoundError", "The attribute is not an attribute of this element.");
    JS_DefinePropertyValueStr(ctx, attr, "__value", JS_GetPropertyStr(ctx, attr, "value"), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    GCValue r = call_method(ctx, this_val, "removeAttributeNS", 2, a2);
    if (JS_IsException(r)) return r;
    attr_set(ctx, attr, "ownerElement", JS_NULL);
    return attr;
}

/* ---- NamedNodeMap (DOM 4.9.1): element.attributes ----
 * A map is an object of its class over its element (its opaque cell holds
 * the NodeId);
 * map[i] and map[name] are exotic properties read from the node store, so
 * they are live.  Each element keeps its one map (a hidden __cyber_nnm);
 * the methods are NamedNodeMap.prototype's. */

static JSClassID js_nnm_class_id;

typedef struct NnmCell { NodeId el; } NnmCell;

static GCValue nnm_element(JSContextHandle ctx, GCValue map) {
    GCHandle h = JS_IsObject(map) ? JS_GetOpaqueHandle2(ctx, map, js_nnm_class_id) : GC_HANDLE_NULL;
    NnmCell *c = h != GC_HANDLE_NULL ? (NnmCell *)gc_deref(h) : NULL;
    return c && c->el ? dom_js(ctx, c->el) : JS_UNDEFINED;
}

static void js_nnm_mark(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func) {
    GCHandle h = JS_GetOpaqueHandle(val, js_nnm_class_id);
    if (h != GC_HANDLE_NULL) mark_func(rt, h);
}

#define THIS_MAP(el, n)                                                         \
    GCValue el = nnm_element(ctx, this_val);                                    \
    if (!JS_IsObject(el)) return JS_ThrowTypeError(ctx, "Illegal invocation");  \
    NodeId n = dom_node_of(el)

/* Whether the attribute's qualified name is one of the map's supported
 * property names: an HTML element in an HTML document has its lower-case
 * ones only. */
static bool nnm_name_supported(NodeId n, const char *q) {
    if (dom_ns(n) != DOM_NS_HTML || !dom_is_html_document(dom_owner(n))) return true;
    for (const char *p = q; *p; p++) if (*p >= 'A' && *p <= 'Z') return false;
    return true;
}

/* The index of the first attribute named q among the supported names, or -1. */
static int nnm_named_index(NodeId n, const char *q) {
    int count = dom_attr_count(n);
    for (int i = 0; i < count; i++) {
        const char *an = attr_output_name(dom_attr_at(n, i));
        if (!strcmp(an, q)) return nnm_name_supported(n, an) ? i : -1;
    }
    return -1;
}

static bool nnm_atom_index(JSContextHandle ctx, JSAtom prop, uint32_t *i) {
    const char *s = JS_AtomToCString(ctx, prop);
    if (!s) return false;
    bool ok = s[0] >= '0' && s[0] <= '9' && (s[0] != '0' || !s[1]);
    uint64_t v = 0;
    for (const char *p = s; ok && *p; p++) {
        ok = *p >= '0' && *p <= '9';
        v = v * 10 + (uint64_t)(*p - '0');
        if (v >= 0xffffffffu) ok = false;
    }
    JS_FreeCString(ctx, s);
    if (ok) *i = (uint32_t)v;
    return ok;
}

static int js_nnm_get_own(JSContextHandle ctx, JSPropertyDescriptor *desc, GCValue obj, JSAtom prop) {
    GCValue el = nnm_element(ctx, obj);
    NodeId n = JS_IsObject(el) ? dom_node_of(el) : DOM_NULL;
    if (!n) return 0;
    uint32_t i;
    int at = -1;
    bool named = false;
    if (nnm_atom_index(ctx, prop, &i)) {
        at = i < (uint32_t)dom_attr_count(n) ? (int)i : -1;
    } else {
        GCValue key = JS_AtomToValue(ctx, prop);
        if (!JS_IsString(key)) return 0;
        /* (a named property shows only when nothing on the prototype chain has the name) */
        GCValue proto = JS_GetPrototype(ctx, obj);
        if (JS_IsObject(proto) && JS_HasProperty(ctx, proto, prop) > 0) return 0;
        const char *q = JS_ToCString(ctx, key);
        if (!q) return 0;
        at = nnm_named_index(n, q);
        JS_FreeCString(ctx, q);
        named = true;
    }
    if (at < 0) return 0;
    if (desc) {
        desc->flags = named ? JS_PROP_CONFIGURABLE : JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
        desc->value = attr_obj_at(ctx, el, n, at);
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
    }
    return 1;
}

/* The indices, then the supported names (each once). */
static int js_nnm_get_own_names(JSContextHandle ctx, JSPropertyEnum **ptab, uint32_t *plen, GCValue obj) {
    GCValue el = nnm_element(ctx, obj);
    NodeId n = JS_IsObject(el) ? dom_node_of(el) : DOM_NULL;
    int count = n ? dom_attr_count(n) : 0;
    GCHandle h = gc_allocz(sizeof(JSPropertyEnum) * (size_t)(2 * count + 1), JS_GC_OBJ_TYPE_DATA);
    JSPropertyEnum *tab = h != GC_HANDLE_NULL ? (JSPropertyEnum *)gc_deref(h) : NULL;
    if (!tab) return -1;
    uint32_t k = 0;
    for (int i = 0; i < count; i++) tab[k++].atom = JS_NewAtomUInt32(ctx, (uint32_t)i);
    for (int i = 0; i < count; i++) {
        const char *q = attr_output_name(dom_attr_at(n, i));
        if (!nnm_name_supported(n, q) || nnm_named_index(n, q) != i) continue;
        JSAtom a = JS_NewAtom(ctx, q);
        GCValue proto = JS_GetPrototype(ctx, obj);
        if (JS_IsObject(proto) && JS_HasProperty(ctx, proto, a) > 0) { JS_FreeAtom(ctx, a); continue; }
        tab[k++].atom = a;
    }
    *ptab = tab;
    *plen = k;
    return 0;
}

static JSClassExoticMethods js_nnm_exotic = {
    .get_own_property = js_nnm_get_own,
    .get_own_property_names = js_nnm_get_own_names,
};

static JSClassDef js_nnm_class_def = {
    .class_name = "NamedNodeMap",
    .gc_mark = js_nnm_mark,
    .exotic = &js_nnm_exotic,
};

static GCValue js_nnm_length(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS_MAP(el, n);
    return JS_NewInt32(ctx, n ? dom_attr_count(n) : 0);
}

static GCValue js_nnm_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_MAP(el, n);
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'item' on 'NamedNodeMap': 1 argument required.");
    uint32_t i = 0;
    if (JS_ToUint32(ctx, &i, argv[0])) return JS_EXCEPTION;
    return n && i < (uint32_t)dom_attr_count(n) ? attr_obj_at(ctx, el, n, (int)i) : JS_NULL;
}

/* getNamedItem(qualifiedName) (magic 0) / getNamedItemNS(namespace, localName) (1) */
static GCValue js_nnm_get_named(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    THIS_MAP(el, n);
    (void)n;
    if (argc < 1 + magic)
        return JS_ThrowTypeError(ctx, "Failed to execute '%s' on 'NamedNodeMap': %d argument%s required.",
                                 magic ? "getNamedItemNS" : "getNamedItem", 1 + magic, magic ? "s" : "");
    return attr_node_get(ctx, el, magic, argc, argv);
}

/* setNamedItem(attr) / setNamedItemNS(attr): setAttributeNode */
static GCValue js_nnm_set_named(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    THIS_MAP(el, n);
    (void)n; (void)magic;
    return js_element_set_attribute_node(ctx, el, argc, argv);
}

/* removeNamedItem(qualifiedName) (magic 0) / removeNamedItemNS(namespace, localName) (1):
 * the removed Attr, or a NotFoundError */
static GCValue js_nnm_remove_named(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    THIS_MAP(el, n);
    (void)n;
    if (argc < 1 + magic)
        return JS_ThrowTypeError(ctx, "Failed to execute '%s' on 'NamedNodeMap': %d argument%s required.",
                                 magic ? "removeNamedItemNS" : "removeNamedItem", 1 + magic, magic ? "s" : "");
    GCValue attr = attr_node_get(ctx, el, magic, argc, argv);
    if (JS_IsException(attr)) return attr;
    if (!JS_IsObject(attr)) return throw_dom_exception(ctx, "NotFoundError", "No item with that name was found.");
    GCValue r = js_element_remove_attribute_node(ctx, el, 1, &attr);
    return JS_IsException(r) ? r : attr;
}

static GCValue js_nnm_ctor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* Element.prototype.attributes: the element's map, made once. */
GCValue js_element_get_attributes(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    if (!dom_node_of(this_val)) return JS_ThrowTypeError(ctx, "Illegal invocation");
    GCValue existing = JS_GetPropertyStr(ctx, this_val, "__cyber_nnm");
    if (JS_IsObject(existing)) return existing;
    GCValue proto = global_proto(ctx, "NamedNodeMap");
    GCValue map = JS_IsObject(proto) ? JS_NewObjectProtoClass(ctx, proto, js_nnm_class_id)
                                     : JS_NewObjectClass(ctx, js_nnm_class_id);
    if (JS_IsException(map)) return map;
    GCHandle h = gc_allocz(sizeof(NnmCell), JS_GC_OBJ_TYPE_DATA);
    if (h == GC_HANDLE_NULL) return JS_ThrowOutOfMemory(ctx);
    ((NnmCell *)gc_deref(h))->el = dom_node_of(this_val);
    JS_SetOpaqueHandle(map, h);
    JS_DefinePropertyValueStr(ctx, this_val, "__cyber_nnm", map, JS_PROP_CONFIGURABLE);
    return map;
}

/* The NamedNodeMap interface on the global. */
void named_node_map_install(JSContextHandle ctx, GCValue global) {
    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    if (!js_nnm_class_id) JS_NewClassID(&js_nnm_class_id);
    if (!JS_IsRegisteredClass(rt, js_nnm_class_id)) JS_NewClass(rt, js_nnm_class_id, &js_nnm_class_def);
    GCValue proto = JS_NewObject(ctx);
    JSAtom a = JS_NewAtom(ctx, "length");
    JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction(ctx, js_nnm_length, "get length", 0), JS_UNDEFINED,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
    static const struct { const char *name; JSCFunctionMagic *fn; int length, magic; } ops[] = {
        { "getNamedItem", js_nnm_get_named, 1, 0 }, { "getNamedItemNS", js_nnm_get_named, 2, 1 },
        { "setNamedItem", js_nnm_set_named, 1, 0 }, { "setNamedItemNS", js_nnm_set_named, 1, 1 },
        { "removeNamedItem", js_nnm_remove_named, 1, 0 }, { "removeNamedItemNS", js_nnm_remove_named, 2, 1 },
    };
    JS_DefinePropertyValueStr(ctx, proto, "item", JS_NewCFunction(ctx, js_nnm_item, "item", 1), JS_PROP_C_W_E);
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
        JS_DefinePropertyValueStr(ctx, proto, ops[i].name,
                                  JS_NewCFunctionMagic(ctx, ops[i].fn, ops[i].name, ops[i].length, JS_CFUNC_generic_magic, ops[i].magic),
                                  JS_PROP_C_W_E);
    /* (iterable: an indexed getter and length) */
    GCValue ap = JS_GetPropertyStr(ctx, JS_GetPropertyStr(ctx, global, "Array"), "prototype");
    JS_DefinePropertyValue(ctx, proto, dom_iterator_atom(ctx), JS_GetPropertyStr(ctx, ap, "values"),
                           JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    GCValue sym = JS_GetPropertyStr(ctx, global, "Symbol");
    JSAtom tag = JS_ValueToAtom(ctx, JS_GetPropertyStr(ctx, sym, "toStringTag"));
    JS_DefinePropertyValue(ctx, proto, tag, JS_NewString(ctx, "NamedNodeMap"), JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, tag);
    GCValue c = JS_NewCFunction2(ctx, js_nnm_ctor, "NamedNodeMap", 0, JS_CFUNC_constructor_or_func, 0);
    JS_SetConstructor(ctx, c, proto);
    JS_DefinePropertyValueStr(ctx, global, "NamedNodeMap", c, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    GCValue win = JS_GetPropertyStr(ctx, global, "window");
    if (JS_IsObject(win) && !JS_StrictEq(ctx, win, global))
        JS_DefinePropertyValueStr(ctx, win, "NamedNodeMap", c, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
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
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, attr_output_name(a)));
    }
    return arr;
}

// ============================================================================
// Element.prototype.matches / closest
// ============================================================================

GCValue js_element_matches(JSContextHandle ctx, GCValue this_val,
                           int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'matches': 1 argument required.");
    const char *selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return JS_EXCEPTION;
    if (!dom_query_valid(selector, strlen(selector))) return bad_selector(ctx, selector);
    bool r = matches_selector(ctx, this_val, selector);
    JS_FreeCString(ctx, selector);
    return JS_NewBool(ctx, r);
}

GCValue js_element_closest(JSContextHandle ctx, GCValue this_val,
                           int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'closest': 1 argument required.");
    const char *selector = JS_ToCString(ctx, argv[0]);
    if (!selector) return JS_EXCEPTION;
    if (!dom_query_valid(selector, strlen(selector))) return bad_selector(ctx, selector);

    query_note_target(ctx, selector, strlen(selector));
    NodeId scope = dom_node_of(this_val);
    GCValue cur = this_val;
    while (!JS_IsNull(cur) && JS_IsObject(cur)) {
        if (dom_query_matches_scoped(dom_node_of(cur), selector, strlen(selector), scope)) {
            JS_FreeCString(ctx, selector);
            return cur;
        }
        NodeId node = dom_node_of(cur);
        if (!(node)) break;
        cur = dom_js_parent(ctx, node);
    }
    JS_FreeCString(ctx, selector);
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
    if (!JS_IsObject(child_nodes)) return;
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
        if (JS_IsObject(own)) {
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
        if (JS_IsObject(own)) {
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
    /* (DOMParserSupportedType; an XML type makes an XML document) */
    static const char *const types[] = { "text/html", "text/xml", "application/xml", "application/xhtml+xml",
                                         "image/svg+xml" };
    size_t t = 0;
    while (t < sizeof types / sizeof types[0] && (!mime || strcmp(mime, types[t]))) t++;
    if (t == sizeof types / sizeof types[0])
        return JS_ThrowTypeError(ctx, "Failed to execute 'parseFromString' on 'DOMParser': The provided value '%s' is "
                                      "not a valid enum value of type DOMParserSupportedType.", mime ? mime : "");
    if (t) return dom_new_xml_document(ctx, str, len, types[t]);
    /* (an empty string is a document with an empty body) */
    GCValue js_doc = dom_new_parsed_document(ctx, str, len);
    if (JS_IsObject(js_doc))
        JS_DefinePropertyValueStr(ctx, js_doc, "__cyber_ct", JS_NewString(ctx, types[t]), JS_PROP_CONFIGURABLE);
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

// new DocumentFragment(): in the current global's document
GCValue js_document_fragment_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    GCValue doc = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "document");
    GCValue frag = js_document_create_document_fragment(ctx, doc, argc, argv);
    GCValue proto = JS_IsObject(frag) && JS_IsObject(new_target) ? JS_GetPropertyStr(ctx, new_target, "prototype") : JS_UNDEFINED;
    if (JS_IsObject(proto)) JS_SetPrototype(ctx, frag, proto);
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
    if (is_attr_value(ctx, clone)) {
        attr_set(ctx, clone, "ownerDocument", this_val);
        return clone;
    }
    /* (all of it, not only its root) */
    NodeId imported = dom_node_of(clone), into = dom_node_of(this_val);
    if (imported && into && dom_owner(imported) != into) dom_api_adopt(ctx, imported, into);
    dom_node_set_owner_document(ctx, clone, this_val);
    /* Imported into the document: its custom elements upgrade now. */
    NodeId cn = dom_node_of(clone);
    if (cn && dom_owner(cn) == g_dom_document) dom_ce_upgrade_created(ctx, cn);
    return clone;
}

/* Hit test of the last layout (§7, "Rendering"): deepest box under the point
 * whose DOM node is an element, as its wrapper. */
/* (where the test began, and the viewport's scroll: a fixed box is on the viewport) */
static double g_hit_x, g_hit_y;
static float g_hit_vx, g_hit_vy;

static void hit_test_begin(LayoutContext *layout, double x, double y) {
    g_hit_x = x;
    g_hit_y = y;
    g_hit_vx = g_hit_vy = 0.0f;
    HtmlNode *rn = g_dom_scrolls.n ? css_layout_box_node(layout, layout->tree.root_idx) : NULL;
    if (rn) dom_scroll_get(dom_owner(rn->store_id), &g_hit_vx, &g_hit_vy);
}

static GCValue hit_test_layout_node(JSContextHandle ctx, LayoutContext *layout, int idx, double x, double y);

static GCValue hit_child(JSContextHandle ctx, LayoutContext *layout, const LayoutBox *box, int child, double x, double y,
                         float sx, float sy) {
    const LayoutStyle *cs = layout->boxes[child].s;
    bool away = cs->position == CSS_POSITION_ABSOLUTE && box->s->position == CSS_POSITION_STATIC;
    double cx = away ? x : x + sx, cy = away ? y : y + sy;
    if (cs->position == CSS_POSITION_FIXED) { cx = g_hit_x - g_hit_vx; cy = g_hit_y - g_hit_vy; }
    return hit_test_layout_node(ctx, layout, child, cx, cy);
}

/* The positioned children with z-index at or over zero (or under it), highest first. */
static GCValue hit_positioned(JSContextHandle ctx, LayoutContext *layout, const LayoutBox *box, int last_child, double x,
                              double y, float sx, float sy, bool under) {
    int limit = INT_MAX;
    for (;;) {
        bool any = false;
        int z = 0;
        for (int c = last_child; c >= 0; c = layout->tree.nodes[c].prev_sibling_idx) {
            const LayoutStyle *cs = layout->boxes[c].s;
            if (cs->position == CSS_POSITION_STATIC) continue;
            int cz = cs->z_index_set ? cs->z_index : 0;
            if ((cz < 0) != under || cz >= limit) continue;
            if (!any || cz > z) { any = true; z = cz; }
        }
        if (!any) return JS_NULL;
        for (int c = last_child; c >= 0; c = layout->tree.nodes[c].prev_sibling_idx) {
            const LayoutStyle *cs = layout->boxes[c].s;
            if (cs->position == CSS_POSITION_STATIC || (cs->z_index_set ? cs->z_index : 0) != z) continue;
            GCValue hit = hit_child(ctx, layout, box, c, x, y, sx, sy);
            if (!JS_IsNull(hit)) return hit;
        }
        limit = z;
    }
}

static GCValue hit_test_layout_node(JSContextHandle ctx, LayoutContext *layout, int idx, double x, double y) {
    if (!layout || idx < 0 || idx >= layout->tree.count) return JS_NULL;
    LayoutBox *box = &layout->boxes[idx];
    LayoutNodeRef *ref = &layout->tree.nodes[idx];
    if (box->s->display == CSS_DISPLAY_NONE) return JS_NULL;   /* not laid out (or styled) */
    bool inside = (x >= box->x && x <= box->x + box->width &&
                   y >= box->y && y <= box->y + box->height);
    /* (what overflows a box is still there, unless it clips) */
    if (!inside && css_layout_box_scrolls(layout, idx)) return JS_NULL;
    /* (its children are where its scroll offset has them) */
    float sx = 0.0f, sy = 0.0f;
    css_layout_box_offset(layout, idx, &sx, &sy);
    int last_child = -1;
    for (int child = ref->first_child_idx; child >= 0; child = layout->tree.nodes[child].next_sibling_idx)
        last_child = child;
    /* (front to back: positioned children by z-index, those under zero behind
     * the rest; at one level, last in the tree first) */
    GCValue over = hit_positioned(ctx, layout, box, last_child, x, y, sx, sy, false);
    if (!JS_IsNull(over)) return over;
    for (int child = last_child; child >= 0; child = layout->tree.nodes[child].prev_sibling_idx) {
        if (layout->boxes[child].s->position != CSS_POSITION_STATIC) continue;
        GCValue child_hit = hit_child(ctx, layout, box, child, x, y, sx, sy);
        if (!JS_IsNull(child_hit)) return child_hit;
    }
    over = hit_positioned(ctx, layout, box, last_child, x, y, sx, sy, true);
    if (!JS_IsNull(over)) return over;

    if (inside && box->s->visibility != CSS_VISIBILITY_HIDDEN && ref->dom_node_idx >= 0 && layout->doc) {
        HtmlNode *hnode = (HtmlNode *)po_array_payload(&layout->doc->array, ref->dom_node_idx);
        if (hnode && hnode->type == HTML_NODE_ELEMENT && dom_valid(hnode->store_id) &&
            dom_kind(hnode->store_id) == DOM_K_ELEMENT) {
            return dom_js(ctx, hnode->store_id);
        }
    }
    return JS_NULL;
}

/* The element under (x, y) of a layout (document coordinates), or null. */
GCValue dom_element_at(JSContextHandle ctx, LayoutContext *layout, double x, double y) {
    if (!layout || layout->tree.root_idx < 0) return JS_NULL;
    hit_test_begin(layout, x, y);
    return hit_test_layout_node(ctx, layout, layout->tree.root_idx, x, y);
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
        double vw = 1920.0, vh = 1080.0;
        JS_ToFloat64(ctx, &vw, JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "innerWidth"));
        JS_ToFloat64(ctx, &vh, JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "innerHeight"));
        g_hit_cache.ok = css_layout_run(&g_hit_cache.layout, doc, NULL, vw, vh);
    }
    GCValue result = doc_elem;
    LayoutContext *layout = &g_hit_cache.layout;
    if (g_hit_cache.ok && layout->tree.root_idx >= 0) {
        /* (x, y are the viewport's) */
        float vx = 0.0f, vy = 0.0f;
        dom_scroll_get(dom_node_of(this_val), &vx, &vy);
        x += vx;
        y += vy;
        hit_test_begin(layout, x, y);
        GCValue hit = hit_test_layout_node(ctx, layout, layout->tree.root_idx, x, y);
        if (!JS_IsNull(hit)) result = hit;
    }
    return result;
}

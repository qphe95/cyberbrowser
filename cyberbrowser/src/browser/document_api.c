/*
 * document_api.c - Document's members on Document.prototype, XMLDocument and
 * DOMImplementation (DOM 4.5-4.6).
 *
 * The getters (documentElement, doctype, head, body, implementation,
 * characterSet) read the store, so documents made by createDocument,
 * createHTMLDocument or DOMParser need no own copies; the page's document
 * keeps the own properties it was given, which shadow them.
 *
 * A DOMImplementation is made once per document ([SameObject]); it keeps its
 * document under a hidden property.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_internal.h"
#include "dom_js.h"
#include "dom_store.h"
#include "xml_parse.h"

extern NodeId g_dom_document;

static NodeId doc_of(GCValue v) {
    NodeId d = dom_node_of(v);
    return dom_kind(d) == DOM_K_DOCUMENT ? d : DOM_NULL;
}

#define THIS_DOC(d)                                                          \
    NodeId d = doc_of(this_val);                                             \
    if (!d) return JS_ThrowTypeError(ctx, "Illegal invocation")

static GCValue global_proto(JSContextHandle ctx, const char *name) {
    GCValue c = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), name);
    return JS_IsObject(c) ? JS_GetPropertyStr(ctx, c, "prototype") : JS_UNDEFINED;
}

/* ---- XML names (XML 1.0 "Name" production) ---- */

/* The code point at s (UTF-8; an engine-encoded lone surrogate is one too),
 * and its length. */
static uint32_t utf8_at(const char *s, size_t n, size_t *len) {
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80 || n < 2) { *len = 1; return u[0]; }
    if (u[0] < 0xE0) { *len = 2; return ((u[0] & 0x1Fu) << 6) | (u[1] & 0x3Fu); }
    if (u[0] < 0xF0 || n < 4) {
        *len = n < 3 ? n : 3;
        return n < 3 ? 0xFFFD : ((u[0] & 0x0Fu) << 12) | ((u[1] & 0x3Fu) << 6) | (u[2] & 0x3Fu);
    }
    *len = 4;
    return ((u[0] & 0x07u) << 18) | ((u[1] & 0x3Fu) << 12) | ((u[2] & 0x3Fu) << 6) | (u[3] & 0x3Fu);
}

static bool name_start_char(uint32_t c) {
    return c == ':' || (c >= 'A' && c <= 'Z') || c == '_' || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c <= 0xD6) ||
           (c >= 0xD8 && c <= 0xF6) || (c >= 0xF8 && c <= 0x2FF) || (c >= 0x370 && c <= 0x37D) ||
           (c >= 0x37F && c <= 0x1FFF) || (c >= 0x200C && c <= 0x200D) || (c >= 0x2070 && c <= 0x218F) ||
           (c >= 0x2C00 && c <= 0x2FEF) || (c >= 0x3001 && c <= 0xD7FF) || (c >= 0xF900 && c <= 0xFDCF) ||
           (c >= 0xFDF0 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0xEFFFF);
}

static bool name_char(uint32_t c) {
    return name_start_char(c) || c == '-' || c == '.' || (c >= '0' && c <= '9') || c == 0xB7 ||
           (c >= 0x300 && c <= 0x36F) || (c >= 0x203F && c <= 0x2040);
}

bool dom_xml_is_name(const char *s, size_t n) {
    if (!n) return false;
    for (size_t i = 0; i < n;) {
        size_t l;
        uint32_t c = utf8_at(s + i, n - i, &l);
        if (!(i ? name_char(c) : name_start_char(c))) return false;
        i += l;
    }
    return true;
}

/* ---- Document's getters ---- */

static NodeId first_child_of_kind(NodeId p, int kind) {
    for (NodeId c = dom_first_child(p); c; c = dom_next_sibling(c))
        if (dom_kind(c) == kind) return c;
    return DOM_NULL;
}

static NodeId html_child(NodeId p, const char *name) {
    for (NodeId c = dom_first_child(p); c; c = dom_next_sibling(c))
        if (dom_kind(c) == DOM_K_ELEMENT && dom_ns(c) == DOM_NS_HTML && !strcmp(dom_local_name(c), name)) return c;
    return DOM_NULL;
}

static GCValue js_document_element(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS_DOC(d);
    return dom_js(ctx, first_child_of_kind(d, DOM_K_ELEMENT));
}

static GCValue js_doctype(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS_DOC(d);
    return dom_js(ctx, first_child_of_kind(d, DOM_K_DOCTYPE));
}

/* head, body: the html root element's head / body (or frameset) child. */
static GCValue js_head_body(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    (void)argc; (void)argv;
    THIS_DOC(d);
    NodeId root = first_child_of_kind(d, DOM_K_ELEMENT);
    if (!root || dom_ns(root) != DOM_NS_HTML || strcmp(dom_local_name(root), "html")) return JS_NULL;
    if (magic == 0) return dom_js(ctx, html_child(root, "head"));
    for (NodeId c = dom_first_child(root); c; c = dom_next_sibling(c))
        if (dom_kind(c) == DOM_K_ELEMENT && dom_ns(c) == DOM_NS_HTML &&
            (!strcmp(dom_local_name(c), "body") || !strcmp(dom_local_name(c), "frameset")))
            return dom_js(ctx, c);
    return JS_NULL;
}

/* The title element: an SVG root's first title child, else the first HTML
 * title in tree order. */
static NodeId title_element(NodeId d) {
    NodeId root = first_child_of_kind(d, DOM_K_ELEMENT);
    if (root && dom_ns(root) == DOM_NS_SVG && !strcmp(dom_local_name(root), "svg")) {
        for (NodeId c = dom_first_child(root); c; c = dom_next_sibling(c))
            if (dom_kind(c) == DOM_K_ELEMENT && dom_ns(c) == DOM_NS_SVG && !strcmp(dom_local_name(c), "title")) return c;
        return DOM_NULL;
    }
    uint32_t r = dom_slot(d);
    for (uint32_t s = dom_next_in_subtree(r, r); s; s = dom_next_in_subtree(s, r)) {
        NodeId c = dom_id_of(s);
        if (g_dom.kind[s] == DOM_K_ELEMENT && g_dom.ns[s] == DOM_NS_HTML && !strcmp(dom_local_name(c), "title")) return c;
    }
    return DOM_NULL;
}

/* title: the title element's text, ASCII white space stripped and collapsed. */
static GCValue js_title_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS_DOC(d);
    NodeId t = title_element(d);
    if (!t) return JS_NewString(ctx, "");
    size_t cap = 64, n = 0;
    char *buf = malloc(cap);
    if (!buf) return JS_ThrowOutOfMemory(ctx);
    bool space = false;
    for (NodeId c = dom_first_child(t); c; c = dom_next_sibling(c)) {
        if (dom_kind(c) != DOM_K_TEXT && dom_kind(c) != DOM_K_CDATA) continue;
        uint32_t l = 0;
        const char *s = dom_text(c, &l);
        for (uint32_t i = 0; i < l; i++) {
            char ch = s[i];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\f' || ch == '\r') { space = n > 0; continue; }
            if (n + 2 > cap) {
                char *nb = realloc(buf, cap *= 2);
                if (!nb) { free(buf); return JS_ThrowOutOfMemory(ctx); }
                buf = nb;
            }
            if (space) buf[n++] = ' ';
            space = false;
            buf[n++] = ch;
        }
    }
    GCValue r = JS_NewStringLen(ctx, buf, n);
    free(buf);
    return r;
}

/* title = v: the title element's text (one made in the head of an HTML
 * document if there is none). */
static GCValue js_title_set(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_DOC(d);
    if (argc < 1) return JS_UNDEFINED;
    NodeId t = title_element(d);
    GCValue tv;
    if (t) {
        tv = dom_js(ctx, t);
    } else {
        GCValue head = js_head_body(ctx, this_val, 0, NULL, 0);
        if (!JS_IsObject(head)) return JS_UNDEFINED;
        GCValue name = JS_NewString(ctx, "title");
        tv = js_document_create_element(ctx, this_val, 1, &name);
        if (JS_IsException(tv) || JS_IsException(js_node_appendChild_real(ctx, head, 1, &tv))) return JS_EXCEPTION;
    }
    GCValue r = js_node_set_text_content(ctx, tv, 1, argv);
    return JS_IsException(r) ? r : JS_UNDEFINED;
}

/* The first "charset" in s, ASCII case-insensitively, or NULL. */
static const char *find_charset(const char *s) {
    for (; *s; s++)
        if (!strncasecmp(s, "charset", 7)) return s;
    return NULL;
}

/* The encoding a meta element declares: its charset, or the charset= in an
 * http-equiv=content-type's content (HTML "extracting a character encoding
 * from a meta element", simplified); NULL if none. */
static const char *meta_encoding(NodeId m) {
    const char *cs = dom_get_attr_str(m, "charset");
    if (cs) return encoding_name_for_label(cs, strlen(cs));
    const char *he = dom_get_attr_str(m, "http-equiv"), *content = dom_get_attr_str(m, "content");
    if (!he || strcasecmp(he, "content-type") || !content) return NULL;
    for (const char *p = content; (p = find_charset(p)); p += 7) {
        const char *v = p + 7;
        while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\f' || *v == '\r') v++;
        if (*v != '=') continue;
        v++;
        while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\f' || *v == '\r') v++;
        char q = (*v == '"' || *v == '\'') ? *v++ : 0;
        size_t n = 0;
        while (v[n] && (q ? v[n] != q : !(v[n] == ' ' || v[n] == ';' || v[n] == '\t' || v[n] == '\n'))) n++;
        return encoding_name_for_label(v, n);
    }
    return NULL;
}

/* characterSet: the encoding the document's first meta declaration names
 * (UTF-16 is UTF-8 there, x-user-defined windows-1252), else UTF-8. */
static GCValue js_character_set(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS_DOC(d);
    const char *name = NULL;
    if (dom_is_html_document(d)) {
        uint32_t r = dom_slot(d);
        for (uint32_t s = dom_next_in_subtree(r, r); s && !name; s = dom_next_in_subtree(s, r)) {
            NodeId c = dom_id_of(s);
            if (g_dom.kind[s] == DOM_K_ELEMENT && g_dom.ns[s] == DOM_NS_HTML && !strcmp(dom_local_name(c), "meta"))
                name = meta_encoding(c);
        }
    }
    if (!name || !strncmp(name, "UTF-16", 6)) name = "UTF-8";
    else if (!strcmp(name, "x-user-defined")) name = "windows-1252";
    return JS_NewString(ctx, name);
}

static GCValue js_implementation(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS_DOC(d);
    (void)d;
    GCValue impl = JS_GetPropertyStr(ctx, this_val, "__cb_implementation");
    if (JS_IsObject(impl)) return impl;
    GCValue proto = global_proto(ctx, "DOMImplementation");
    impl = JS_IsObject(proto) ? JS_NewObjectProto(ctx, proto) : JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, impl, "__cb_document", this_val, 0);
    JS_DefinePropertyValueStr(ctx, this_val, "__cb_implementation", impl, 0);
    return impl;
}

/* ---- Document's methods ---- */

static GCValue adopt_new(JSContextHandle ctx, GCValue this_val, NodeId n) {
    GCValue node = dom_js(ctx, n);
    if (JS_IsObject(node)) dom_node_set_owner_document(ctx, node, this_val);
    return node;
}

/* createCDATASection(data): not in HTML documents, nor with "]]>". */
static GCValue js_create_cdata_section(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_DOC(d);
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'createCDATASection' on 'Document': 1 argument required.");
    if (dom_is_html_document(d))
        return throw_dom_exception(ctx, "NotSupportedError", "This operation is not supported for HTML documents.");
    size_t n = 0;
    const char *s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s) return JS_EXCEPTION;
    for (size_t i = 0; i + 2 < n; i++)
        if (s[i] == ']' && s[i + 1] == ']' && s[i + 2] == '>')
            return throw_dom_exception(ctx, "InvalidCharacterError", "The data contains ']]>'.");
    return adopt_new(ctx, this_val, dom_create_chardata(DOM_K_CDATA, s, n, d));
}

/* createProcessingInstruction(target, data): target a Name, data without "?>". */
static GCValue js_create_processing_instruction(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_DOC(d);
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "Failed to execute 'createProcessingInstruction' on 'Document': 2 arguments required.");
    size_t tn = 0, dn = 0;
    const char *t = JS_ToCStringLen(ctx, &tn, argv[0]);
    if (!t) return JS_EXCEPTION;
    char *target = strndup(t, tn);
    const char *s = JS_ToCStringLen(ctx, &dn, argv[1]);
    if (!s || !target) { free(target); return JS_EXCEPTION; }
    if (!dom_xml_is_name(target, tn)) {
        free(target);
        return throw_dom_exception(ctx, "InvalidCharacterError", "The target is not a valid name.");
    }
    for (size_t i = 0; i + 1 < dn; i++)
        if (s[i] == '?' && s[i + 1] == '>') {
            free(target);
            return throw_dom_exception(ctx, "InvalidCharacterError", "The data contains '?>'.");
        }
    GCValue r = adopt_new(ctx, this_val, dom_create_pi(target, tn, s, dn, d));
    free(target);
    return r;
}

/* adoptNode(node): node, out of its parent, into this document. */
static GCValue js_adopt_node(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_DOC(d);
    if (argc < 1) return JS_ThrowTypeError(ctx, "Failed to execute 'adoptNode' on 'Document': 1 argument required.");
    NodeId n = dom_node_of(argv[0]);
    if (!n) return JS_ThrowTypeError(ctx, "Failed to execute 'adoptNode' on 'Document': parameter 1 is not of type 'Node'.");
    if (dom_kind(n) == DOM_K_DOCUMENT)
        return throw_dom_exception(ctx, "NotSupportedError", "Documents cannot be adopted.");
    if (dom_flags(n) & DOMF_SHADOW_ROOT)
        return throw_dom_exception(ctx, "HierarchyRequestError", "Shadow roots cannot be adopted.");
    if (dom_kind(n) == DOM_K_FRAGMENT && dom_host(n)) return argv[0];   /* (a template's content) */
    NodeId p = dom_parent(n);
    if (p) {
        GCValue pj = dom_js(ctx, p);
        GCValue r = js_node_removeChild_real(ctx, pj, 1, argv);
        if (JS_IsException(r)) return r;
    }
    if (dom_owner(n) != d) dom_api_adopt(ctx, n, d);
    return argv[0];
}

/* compatMode: an HTML document without a doctype is in quirks mode
 * (approximately: the parser decides it from the doctype itself). */
static GCValue js_compat_mode(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS_DOC(d);
    bool quirks = dom_is_html_document(d) && !first_child_of_kind(d, DOM_K_DOCTYPE);
    return JS_NewString(ctx, quirks ? "BackCompat" : "CSS1Compat");
}

/* ---- documents ---- */

/* A new document: an XML one (XMLDocument) or an HTML one, of this
 * content type, at about:blank. */
static GCValue new_document(JSContextHandle ctx, bool xml, const char *content_type) {
    NodeId d = dom_create_document();
    if (xml) g_dom.flags[dom_slot(d)] |= DOMF_XML_DOCUMENT;
    GCValue doc = dom_js(ctx, d);
    if (!JS_IsObject(doc)) return JS_EXCEPTION;
    if (xml) {
        GCValue proto = global_proto(ctx, "XMLDocument");
        if (JS_IsObject(proto)) JS_SetPrototype(ctx, doc, proto);
    }
    JS_DefinePropertyValueStr(ctx, doc, "contentType", JS_NewString(ctx, content_type), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, doc, "URL", JS_NewString(ctx, "about:blank"), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, doc, "documentURI", JS_NewString(ctx, "about:blank"), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, doc, "readyState", JS_NewString(ctx, "complete"), JS_PROP_CONFIGURABLE);
    /* (no browsing context: no window, no location) */
    JS_DefinePropertyValueStr(ctx, doc, "location", JS_NULL, JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, doc, "defaultView", JS_NULL, JS_PROP_CONFIGURABLE);
    return doc;
}

/* An XML document of these bytes, of this content type: not well-formed,
 * a parsererror element with the message as its root (as Firefox makes). */
GCValue dom_new_xml_document(JSContextHandle ctx, const char *s, size_t n, const char *content_type) {
    GCValue doc = new_document(ctx, true, content_type);
    if (JS_IsException(doc)) return doc;
    NodeId d = doc_of(doc);
    char err[256];
    if (!dom_parse_xml(s, n, d, err, sizeof err)) {
        while (dom_first_child(d)) {
            NodeId c = dom_first_child(d);
            dom_unlink(c);
            dom_free_subtree(c);
        }
        static const char ns[] = "http://www.mozilla.org/newlayout/xml/parsererror.xml";
        NodeId pe = dom_create_element_str("parsererror", 11, d);
        dom_set_element_name(pe, dom_ns_of(ns, sizeof ns - 1), NULL, 0, "parsererror", 11);
        char msg[300];
        int ml = snprintf(msg, sizeof msg, "XML Parsing Error: %s", err);
        dom_link_append(pe, dom_create_chardata(DOM_K_TEXT, msg, (size_t)ml, d));
        dom_link_append(d, pe);
    }
    return doc;
}

static bool append(JSContextHandle ctx, GCValue parent, GCValue child) {
    return !JS_IsException(js_node_appendChild_real(ctx, parent, 1, &child));
}

/* The DOMImplementation's document. */
static GCValue impl_document(JSContextHandle ctx, GCValue this_val) {
    GCValue d = JS_IsObject(this_val) ? JS_GetPropertyStr(ctx, this_val, "__cb_document") : JS_UNDEFINED;
    return doc_of(d) ? d : JS_UNDEFINED;
}

#define THIS_IMPL(doc)                                                       \
    GCValue doc = impl_document(ctx, this_val);                              \
    if (JS_IsUndefined(doc)) return JS_ThrowTypeError(ctx, "Illegal invocation")

/* createDocumentType(name, publicId, systemId) */
static GCValue js_create_document_type(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_IMPL(doc);
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "Failed to execute 'createDocumentType' on 'DOMImplementation': 3 arguments required.");
    size_t nn = 0, pn = 0, sn = 0;
    const char *s[3];
    size_t *ln[3] = { &nn, &pn, &sn };
    char *copy[3] = { NULL, NULL, NULL };
    for (int i = 0; i < 3; i++) {
        s[i] = JS_ToCStringLen(ctx, ln[i], argv[i]);
        if (!s[i] || !(copy[i] = malloc(*ln[i] + 1))) {
            for (int j = 0; j < i; j++) free(copy[j]);
            return JS_EXCEPTION;
        }
        memcpy(copy[i], s[i], *ln[i]);
        copy[i][*ln[i]] = 0;
    }
    /* (a valid doctype name: no ASCII white space, NUL or '>') */
    bool ok = true;
    for (size_t i = 0; ok && i < nn; i++) {
        char c = copy[0][i];
        ok = !(c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == 0 || c == '>');
    }
    GCValue r = ok ? adopt_new(ctx, doc, dom_create_doctype(copy[0], nn, copy[1], pn, copy[2], sn, doc_of(doc)))
                   : throw_dom_exception(ctx, "InvalidCharacterError", "The doctype name is not valid.");
    for (int i = 0; i < 3; i++) free(copy[i]);
    return r;
}

/* createDocument(namespace, qualifiedName, doctype = null): an XMLDocument
 * with the doctype and an element so named (none for ""). */
static GCValue js_create_document(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_IMPL(doc);
    (void)doc;
    if (argc < 2) return JS_ThrowTypeError(ctx, "Failed to execute 'createDocument' on 'DOMImplementation': 2 arguments required.");
    GCValue dt = argc > 2 ? argv[2] : JS_NULL;
    if (!JS_IsNull(dt) && !JS_IsUndefined(dt) && dom_kind(dom_node_of(dt)) != DOM_K_DOCTYPE)
        return JS_ThrowTypeError(ctx, "Failed to execute 'createDocument' on 'DOMImplementation': parameter 3 is not of type 'DocumentType'.");
    size_t un = 0;
    const char *uri = NULL;
    if (!JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) {
        uri = JS_ToCStringLen(ctx, &un, argv[0]);
        if (!uri) return JS_EXCEPTION;
    }
    uint32_t ns = dom_ns_of(uri, un);
    const char *ct = ns == DOM_NS_HTML ? "application/xhtml+xml" : ns == DOM_NS_SVG ? "image/svg+xml" : "application/xml";
    GCValue nd = new_document(ctx, true, ct);
    if (JS_IsException(nd)) return nd;
    GCValue el = JS_NULL;
    /* (qualifiedName is [LegacyNullToEmptyString]) */
    GCValue qn = JS_IsNull(argv[1]) ? JS_NewString(ctx, "") : JS_ToString(ctx, argv[1]);
    if (JS_IsException(qn)) return qn;
    const char *q = JS_ToCString(ctx, qn);
    if (q && q[0]) {
        GCValue args[2] = { argv[0], qn };
        el = js_document_create_element_ns(ctx, nd, 2, args);
        if (JS_IsException(el)) return el;
    }
    if (!JS_IsNull(dt) && !JS_IsUndefined(dt) && !append(ctx, nd, dt)) return JS_EXCEPTION;
    if (JS_IsObject(el) && !append(ctx, nd, el)) return JS_EXCEPTION;
    return nd;
}

/* createHTMLDocument(title): <!DOCTYPE html><html><head><title>title</title></head><body></body></html>
 * (no title element when title is undefined). */
static GCValue js_create_html_document(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_IMPL(doc);
    (void)doc;
    GCValue nd = new_document(ctx, false, "text/html");
    if (JS_IsException(nd)) return nd;
    NodeId d = doc_of(nd);
    if (!append(ctx, nd, adopt_new(ctx, nd, dom_create_doctype("html", 4, "", 0, "", 0, d)))) return JS_EXCEPTION;
    GCValue names[3] = { JS_NewString(ctx, "html"), JS_NewString(ctx, "head"), JS_NewString(ctx, "body") };
    GCValue html = js_document_create_element(ctx, nd, 1, &names[0]);
    GCValue head = js_document_create_element(ctx, nd, 1, &names[1]);
    GCValue body = js_document_create_element(ctx, nd, 1, &names[2]);
    if (!append(ctx, nd, html) || !append(ctx, html, head)) return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        GCValue tn = JS_NewString(ctx, "title");
        GCValue title = js_document_create_element(ctx, nd, 1, &tn);
        GCValue text = js_document_create_text_node(ctx, nd, 1, &argv[0]);
        if (!append(ctx, title, text) || !append(ctx, head, title)) return JS_EXCEPTION;
    }
    if (!append(ctx, html, body)) return JS_EXCEPTION;
    return nd;
}

static GCValue js_has_feature(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_TRUE;
}

/* ---- installing ---- */

static GCValue illegal_ctor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

static void getter(JSContextHandle ctx, GCValue proto, const char *name, JSCFunctionMagic *fn, int magic) {
    JSAtom a = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunctionMagic(ctx, fn, name, 0, JS_CFUNC_generic_magic, magic),
                            JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
}

static GCValue plain_getter(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    static JSCFunction *const fns[] = { js_document_element, js_doctype, js_character_set, js_implementation };
    return fns[magic](ctx, this_val, argc, argv);
}

static void method(JSContextHandle ctx, GCValue proto, const char *name, JSCFunction *fn, int length) {
    JS_DefinePropertyValueStr(ctx, proto, name, JS_NewCFunction(ctx, fn, name, length),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
}

static GCValue interface(JSContextHandle ctx, GCValue global, const char *name, GCValue parent) {
    GCValue proto = JS_NewObject(ctx);
    if (JS_IsObject(parent)) JS_SetPrototype(ctx, proto, parent);
    GCValue c = JS_NewCFunction2(ctx, illegal_ctor, name, 0, JS_CFUNC_constructor_or_func, 0);
    JS_SetConstructor(ctx, c, proto);
    JS_DefinePropertyValueStr(ctx, global, name, c, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    GCValue win = JS_GetPropertyStr(ctx, global, "window");
    if (JS_IsObject(win) && !JS_StrictEq(ctx, win, global))
        JS_DefinePropertyValueStr(ctx, win, name, c, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    GCValue sym = JS_GetPropertyStr(ctx, global, "Symbol");
    JSAtom tag = JS_ValueToAtom(ctx, JS_GetPropertyStr(ctx, sym, "toStringTag"));
    JS_DefinePropertyValue(ctx, proto, tag, JS_NewString(ctx, name), JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, tag);
    return proto;
}

void document_api_install(JSContextHandle ctx, GCValue global) {
    GCValue dp = global_proto(ctx, "Document");
    if (!JS_IsObject(dp)) return;
    getter(ctx, dp, "documentElement", plain_getter, 0);
    getter(ctx, dp, "doctype", plain_getter, 1);
    getter(ctx, dp, "characterSet", plain_getter, 2);
    getter(ctx, dp, "charset", plain_getter, 2);
    getter(ctx, dp, "inputEncoding", plain_getter, 2);
    getter(ctx, dp, "implementation", plain_getter, 3);
    getter(ctx, dp, "head", js_head_body, 0);
    getter(ctx, dp, "body", js_head_body, 1);
    {
        JSAtom a = JS_NewAtom(ctx, "title");
        JS_DefinePropertyGetSet(ctx, dp, a, JS_NewCFunction(ctx, js_title_get, "get title", 0),
                                JS_NewCFunction(ctx, js_title_set, "set title", 1), JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
    }

    method(ctx, dp, "createElement", js_document_create_element, 1);
    method(ctx, dp, "createElementNS", js_document_create_element_ns, 2);
    method(ctx, dp, "createTextNode", js_document_create_text_node, 1);
    method(ctx, dp, "createComment", js_document_create_comment, 1);
    method(ctx, dp, "createCDATASection", js_create_cdata_section, 1);
    method(ctx, dp, "createProcessingInstruction", js_create_processing_instruction, 2);
    method(ctx, dp, "createDocumentFragment", js_document_create_document_fragment, 0);
    method(ctx, dp, "createRange", js_document_create_range, 0);
    method(ctx, dp, "importNode", js_document_import_node, 1);
    method(ctx, dp, "adoptNode", js_adopt_node, 1);
    {
        JSAtom at = JS_NewAtom(ctx, "compatMode");
        JS_DefinePropertyGetSet(ctx, dp, at, JS_NewCFunction(ctx, js_compat_mode, "get compatMode", 0), JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, at);
    }
    method(ctx, dp, "getElementById", js_document_getElementById, 1);
    method(ctx, dp, "querySelector", js_document_querySelector, 1);
    method(ctx, dp, "querySelectorAll", js_document_querySelectorAll, 1);
    method(ctx, dp, "getElementsByTagName", js_document_get_elements_by_tag_name, 1);
    method(ctx, dp, "getElementsByClassName", js_document_getElementsByClassName, 1);

    interface(ctx, global, "XMLDocument", dp);

    GCValue ip = interface(ctx, global, "DOMImplementation", JS_UNDEFINED);
    JS_DefinePropertyValueStr(ctx, ip, "createDocumentType", JS_NewCFunction(ctx, js_create_document_type, "createDocumentType", 3),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx, ip, "createDocument", JS_NewCFunction(ctx, js_create_document, "createDocument", 2),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx, ip, "createHTMLDocument", JS_NewCFunction(ctx, js_create_html_document, "createHTMLDocument", 0),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx, ip, "hasFeature", JS_NewCFunction(ctx, js_has_feature, "hasFeature", 0),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);

    /* window.frames is the window (HTML 7.2.3). */
    if (JS_IsUndefined(JS_GetPropertyStr(ctx, global, "frames"))) {
        GCValue win = JS_GetPropertyStr(ctx, global, "window");
        JS_DefinePropertyValueStr(ctx, global, "frames", JS_IsObject(win) ? win : global,
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    }

    /* The page's document: its implementation is the prototype's getter's. */
    GCValue page = JS_GetPropertyStr(ctx, global, "document");
    if (JS_IsObject(page)) {
        JSAtom a = JS_NewAtom(ctx, "implementation");
        JS_DeleteProperty(ctx, page, a, 0);
        JS_FreeAtom(ctx, a);
    }
}

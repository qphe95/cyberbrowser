/*
 * HTML DOM Parser - Implementation
 * 
 * A lightweight HTML parser that creates proper DOM nodes in the QuickJS engine.
 * This allows JavaScript running in the engine to interact with the parsed HTML
 * structure through standard DOM APIs.
 * 
 * Nodes are stored in a PreOrderCompactionArray for cache-friendly pre-order
 * layout.
 */

#include "cyber_profile.h"
#include "cyber_env.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "html_dom.h"
#include "gc_value_helpers.h"
#include "platform.h"
#include "browser_api_impl.h"
#include "browser_api_impl_handle_helpers.h"
#include "browser_api_impl_internal.h"
#include "css_parser.h"
#include "dom_parse.h"
#include "dom_snapshot.h"
#include "dom_store.h"
#include "dom_wrap.h"
#include "dom_js.h"

#define LOG_TAG "html_dom"
#define LOG_INFO(...) platform_log(LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)
#define LOG_ERROR(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)
#define LOG_WARN(...) platform_log(LOG_LEVEL_WARN, LOG_TAG, __VA_ARGS__)

/* List of self-closing HTML tags (void elements) */
static const char *SELF_CLOSING_TAGS[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input",
    "link", "meta", "param", "source", "track", "wbr", NULL
};

/* List of tags that contain raw text content */
static const char *RAW_CONTENT_TAGS[] = {
    "script", "style", "textarea", "title", NULL
};

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

bool html_is_self_closing_tag(const char *tag_name) {
    if (!tag_name) return false;
    for (int i = 0; SELF_CLOSING_TAGS[i]; i++) {
        if (strcasecmp(tag_name, SELF_CLOSING_TAGS[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool html_is_raw_content_tag(const char *tag_name) {
    if (!tag_name) return false;
    for (int i = 0; RAW_CONTENT_TAGS[i]; i++) {
        if (strcasecmp(tag_name, RAW_CONTENT_TAGS[i]) == 0) {
            return true;
        }
    }
    return false;
}

void html_tag_name_normalize(char *tag_name) {
    if (!tag_name) return;
    for (char *p = tag_name; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
}

/* Decode common HTML entities */
int html_decode_entities(char *str, size_t len) {
    if (!str || len == 0) return 0;
    
    char *dest = str;
    const char *src = str;
    const char *end = str + len;
    
    while (*src && src < end) {
        if (*src == '&') {
            if (strncmp(src, "&lt;", 4) == 0) {
                *dest++ = '<';
                src += 4;
            } else if (strncmp(src, "&gt;", 4) == 0) {
                *dest++ = '>';
                src += 4;
            } else if (strncmp(src, "&amp;", 5) == 0) {
                *dest++ = '&';
                src += 5;
            } else if (strncmp(src, "&quot;", 6) == 0) {
                *dest++ = '"';
                src += 6;
            } else if (strncmp(src, "&apos;", 6) == 0) {
                *dest++ = '\'';
                src += 6;
            } else if (strncmp(src, "&nbsp;", 6) == 0) {
                *dest++ = ' ';
                src += 6;
            } else if (src[1] == '#' && isdigit((unsigned char)src[2])) {
                /* Numeric entity: &#123; */
                src += 2;
                int val = 0;
                while (isdigit((unsigned char)*src)) {
                    val = val * 10 + (*src - '0');
                    src++;
                }
                if (*src == ';') src++;
                *dest++ = (char)(val > 0 && val < 256 ? val : '?');
            } else {
                *dest++ = *src++;
            }
        } else {
            *dest++ = *src++;
        }
    }
    *dest = '\0';
    return (int)(dest - str);
}

/* ============================================================================
 * Tree / Node Helpers
 * ============================================================================ */

static HtmlNode* html_node_at(HtmlDocument *doc, int idx) {
    if (!doc || idx < 0) return NULL;
    return (HtmlNode*)po_array_payload(&doc->array, idx);
}


static void html_node_free_contents(HtmlNode *node) {
    if (!node) return;
    /* Attribute names and values live in the document's arena. */
    if (node->text_content) {
        free(node->text_content);
        node->text_content = NULL;
    }
    /* Note: We don't free js_object here - that's managed by QuickJS GC */
}

void html_document_arena_free(HtmlDocument *doc);

void html_document_free(HtmlDocument *doc) {
    if (!doc) return;
    if (doc->refs > 1) { doc->refs--; return; }
    
    size_t n = po_array_count(&doc->array);
    for (size_t i = 0; i < n; i++) {
        HtmlNode *node = html_node_at(doc, (int)i);
        if (node) html_node_free_contents(node);
    }
    
    po_array_free(&doc->array);
    html_document_arena_free(doc);
    
    if (doc->title) free(doc->title);
    free(doc);
}

/* ============================================================================
 * Parsing: the node store's tokenizer and tree builder (dom_parse.c), then a
 * layout snapshot of the result (dom_snapshot.c).
 * ============================================================================ */

HtmlDocument* html_parse(const char *html, size_t html_len) {
    if (!html || html_len == 0) return NULL;
    NodeId frag = dom_parse_html(html, html_len, DOM_NULL);
    HtmlDocument *doc = dom_snapshot_parsed(frag);
    dom_free_subtree(frag);
    if (doc)
        LOG_INFO("Parsed HTML document: found %s, %s",
                 doc->head_idx >= 0 ? "<head>" : "no <head>",
                 doc->body_idx >= 0 ? "<body>" : "no <body>");
    return doc;
}

/* ============================================================================
 * The JS document over the node store
 * ============================================================================
 * Parsed documents and fragments go straight into the store; wrappers are
 * created only for the nodes scripts touch (dom_wrap.h).
 */

/* Forward declarations from js_quickjs.c / dom_api.c */
extern JSClassID js_video_class_id;
extern GCValue js_video_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
extern NodeId g_dom_index_root;

static NodeId owner_doc_of(GCValue js_doc) {
    NodeId d = dom_node_of(js_doc);
    return dom_kind(d) == DOM_K_DOCUMENT ? d : g_dom_document;
}

/* A store element with the given attributes. */
static NodeId element_from_attrs(const char *tag_name, HtmlAttribute *attrs, NodeId owner) {
    NodeId el = dom_create_element_str(tag_name, strlen(tag_name), owner);
    for (HtmlAttribute *a = attrs; a; a = a->next)
        dom_set_attr_str(el, a->name, a->value);
    if (strcasecmp(tag_name, "script") == 0) g_dom.flags[dom_slot(el)] |= DOMF_PARSER_SCRIPT;
    return el;
}

/* Create a JavaScript element for a given tag */
GCValue html_create_element_js(JSContextHandle ctx, const char *tag_name, HtmlAttribute *attrs) {
    if (!tag_name || !SAFE_VALID(ctx)) return JS_NULL;
    return dom_js(ctx, element_from_attrs(tag_name, attrs, g_dom_document));
}

/* Create a JavaScript element the way document.createElement() would, with
 * the parsed attributes. */
GCValue html_create_element_js_with_document(JSContextHandle ctx, GCValue js_doc, const char *tag_name, HtmlAttribute *attrs) {
    if (!tag_name || !SAFE_VALID(ctx)) return JS_NULL;
    GCValue el = dom_js(ctx, element_from_attrs(tag_name, attrs, owner_doc_of(js_doc)));
    if (!JS_IsUndefined(js_doc) && !JS_IsNull(js_doc)) dom_node_set_owner_document(ctx, el, js_doc);
    return el;
}

/* Template elements keep their parsed children in a content fragment. */
void dom_adopt_template_contents(NodeId root) {
    if (!dom_valid(root)) return;
    static CssAtom a_template;
    if (!a_template) { css_atom_reserve(1); a_template = css_atom_intern("template", 8); }
    uint32_t r = dom_slot(root);
    /* Collect first: moving children changes the walk. */
    uint32_t *list = NULL, n = 0, cap = 0;
    for (uint32_t s = r; s; s = dom_next_in_subtree(s, r)) {
        if (g_dom.kind[s] == DOM_K_ELEMENT && g_dom.tag[s] == a_template) {
            if (n == cap) { cap = cap ? cap * 2 : 16; list = realloc(list, cap * sizeof(uint32_t)); }
            list[n++] = s;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        NodeId t = dom_id_of(list[i]);
        NodeId frag = dom_template_content(t);
        if (!frag) {
            frag = dom_create_fragment(dom_owner(t));
            dom_set_template_content(t, frag);
        }
        while (dom_first_child(t)) {
            NodeId c = dom_first_child(t);
            dom_detach(c, false);
            dom_attach(frag, c, DOM_NULL);
        }
    }
    free(list);
}

/* Build a store fragment from an HtmlDocument (the legacy entry points that
 * take a parsed HtmlDocument). */
static void copy_html_children(HtmlDocument *doc, int idx, NodeId parent, NodeId owner, int depth) {
    for (int c = idx; c >= 0; c = po_array_next_sibling(&doc->array, c)) {
        if (!po_array_is_active(&doc->array, c)) continue;
        HtmlNode *hn = po_array_payload(&doc->array, c);
        if (!hn) continue;
        if (hn->type == HTML_NODE_TEXT) {
            NodeId t = dom_create_chardata(DOM_K_TEXT, hn->text_content ? hn->text_content : "", hn->text_len, owner);
            dom_attach(parent, t, DOM_NULL);
            continue;
        }
        if (hn->type != HTML_NODE_ELEMENT) continue;
        NodeId n = element_from_attrs(hn->tag_name, hn->attributes, owner);
        dom_attach(parent, n, DOM_NULL);
        if (hn->text_content && hn->text_len && html_is_raw_content_tag(hn->tag_name)) {
            NodeId t = dom_create_chardata(DOM_K_TEXT, hn->text_content, hn->text_len, owner);
            dom_attach(n, t, DOM_NULL);
        }
        if (depth < HTML_MAX_NESTING_DEPTH)
            copy_html_children(doc, po_array_first_child(&doc->array, c), n, owner, depth + 1);
    }
}

static NodeId fragment_from_html_document(HtmlDocument *doc, NodeId owner) {
    NodeId frag = dom_create_fragment(owner);
    if (doc) copy_html_children(doc, doc->array.first_root, frag, owner, 0);
    return frag;
}

/* The atom of a fixed name, made once per context. */
typedef struct HtmlAtom { uint32_t serial; JSAtom atom; } HtmlAtom;
static JSAtom html_atom(JSContextHandle ctx, HtmlAtom *c, const char *name) {
    uint32_t serial = JS_GetContextSerial(ctx);
    if (c->serial != serial) {
        c->atom = JS_NewAtom(ctx, name);
        c->serial = serial;
    }
    return c->atom;
}

/* Helper: transfer numeric properties and style from old element to new element */
static void html_transfer_element_properties(JSContextHandle ctx, GCValue new_elem, GCValue old_elem) {
    if (!JS_IsObject(old_elem)) return;
    static const char *const numeric_props[] = {
        "clientWidth", "clientHeight", "scrollWidth", "scrollHeight",
        "offsetWidth", "offsetHeight", NULL
    };
    static HtmlAtom numeric_atoms[6], style_atom;
    /* Only values assigned on the old element move: the prototype's are
     * read-only getters (setting them does nothing), and calling them laid
     * out the skeleton page that is being replaced. */
    for (int i = 0; numeric_props[i]; i++) {
        JSAtom a = html_atom(ctx, &numeric_atoms[i], numeric_props[i]);
        JSPropertyDescriptor desc = JS_PROPERTY_DESCRIPTOR_INIT;
        if (JS_GetOwnProperty(ctx, &desc, old_elem, a) > 0 && !(desc.flags & JS_PROP_GETSET) &&
            !JS_IsUndefined(desc.value) && !JS_IsNull(desc.value))
            JS_SetProperty(ctx, new_elem, a, desc.value);
    }
    JSAtom sa = html_atom(ctx, &style_atom, "style");
    GCValue old_style = JS_GetProperty(ctx, old_elem, sa);
    GCValue new_style = JS_GetProperty(ctx, new_elem, sa);
    if (JS_IsObject(old_style) && JS_IsObject(new_style)) {
        JSPropertyEnum *tab = NULL;
        uint32_t plen = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &plen, old_style, JS_GPN_STRING_MASK) == 0 && tab) {
            for (uint32_t i = 0; i < plen; i++) {
                const char *key = JS_AtomToCString(ctx, tab[i].atom);
                if (key && strcmp(key, "removeProperty") != 0 &&
                    strcmp(key, "setProperty") != 0 &&
                    strcmp(key, "getPropertyValue") != 0 && key[0] != '_') {
                    GCValue val = JS_GetProperty(ctx, old_style, tab[i].atom);
                    JS_SetProperty(ctx, new_style, tab[i].atom, val);
                }
            }
            JS_FreePropertyEnum(ctx, tab, plen);
        }
    }
}

static NodeId shallow_copy(NodeId src, const char *fallback_tag, NodeId owner) {
    if (!dom_valid(src)) return dom_create_element_str(fallback_tag, strlen(fallback_tag), owner);
    NodeId el = dom_create_element(dom_tag(src), owner);
    for (int i = 0; i < dom_attr_count(src); i++) {
        const DomAttr *a = dom_attr_at(src, i);
        dom_set_attr(el, a->name, a->value.s ? a->value.s : "", a->value.len);
    }
    return el;
}

/* Move every child of `from` to the end of `to` (store only). */
static void move_children(NodeId from, NodeId to, bool skip_head_body) {
    NodeId c = dom_first_child(from);
    while (c) {
        NodeId next = dom_next_sibling(c);
        if (skip_head_body && dom_kind(c) == DOM_K_ELEMENT) {
            const char *t = dom_tag_name(c);
            if (strcmp(t, "head") == 0 || strcmp(t, "body") == 0) { c = next; continue; }
        }
        dom_detach(c, false);
        dom_attach(to, c, DOM_NULL);
        c = next;
    }
}

/* The top-level nodes of markup that has no html, head or body element
 * (HTML 13.2.6.4.1-6): the leading metadata elements go into the head,
 * everything from the first other content on into the body. */
static void imply_head_and_body(NodeId frag, NodeId head, NodeId body) {
    static const char *const metadata[] = {
        "base", "link", "meta", "noscript", "script", "style", "template", "title", NULL
    };
    bool in_head = true;
    NodeId c = dom_first_child(frag);
    while (c) {
        NodeId next = dom_next_sibling(c);
        NodeId to = body;
        if (in_head) {
            int k = dom_kind(c);
            if (k == DOM_K_ELEMENT) {
                const char *t = dom_tag_name(c);
                bool meta = false;
                for (int i = 0; metadata[i] && !meta; i++) meta = strcmp(t, metadata[i]) == 0;
                if (meta) to = head;
                else in_head = false;
            } else if (k == DOM_K_TEXT) {
                uint32_t n = 0;
                const char *s = dom_text(c, &n);
                bool blank = true;
                for (uint32_t i = 0; s && i < n && blank; i++)
                    blank = s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f';
                if (blank) { c = next; continue; }     /* (dropped, as the parser does) */
                in_head = false;
            } else {
                to = head;
            }
        }
        dom_detach(c, false);
        dom_attach(to, c, DOM_NULL);
        c = next;
    }
}

/* Populate document `js_doc` from a parsed fragment: NEW documentElement,
 * head and body (copies of the parsed ones, as the legacy DOM did), the
 * parsed head and body children moved under them, and the root's other
 * children after them.  Properties of the old skeleton are transferred.
 * `page`: it is the page's document (the DOM index follows its root). */
static bool populate_from_fragment(JSContextHandle ctx, GCValue js_doc, NodeId frag, bool page) {
    NodeId doc = dom_node_of(js_doc);
    if (dom_kind(doc) != DOM_K_DOCUMENT) return false;
    DomParseRoots roots = dom_parse_roots(frag);
    GCValue old_doc_element = JS_GetPropertyStr(ctx, js_doc, "documentElement");
    GCValue old_body_element = JS_GetPropertyStr(ctx, js_doc, "body");
    GCValue old_head_element = JS_GetPropertyStr(ctx, js_doc, "head");

    /* (markup without an <html> element, "<li>..</li>", is still a whole
     * document: html, head and body are implied around it) */
    bool has_html = roots.root && strcmp(dom_tag_name(roots.root), "html") == 0;
    NodeId de = shallow_copy(has_html ? roots.root : DOM_NULL, "html", doc);
    NodeId head = shallow_copy(roots.head, "head", doc);
    NodeId body = shallow_copy(roots.body, "body", doc);
    if (roots.head) move_children(roots.head, head, false);
    if (roots.body) move_children(roots.body, body, false);
    dom_attach(de, head, DOM_NULL);
    dom_attach(de, body, DOM_NULL);
    if (has_html) move_children(roots.root, de, true);
    else if (!roots.head && !roots.body) imply_head_and_body(frag, head, body);
    dom_adopt_template_contents(de);

    GCValue new_doc_element = dom_js(ctx, de);
    GCValue new_head_element = dom_js(ctx, head);
    GCValue new_body_element = dom_js(ctx, body);
    dom_node_set_owner_document(ctx, new_doc_element, js_doc);
    dom_node_set_owner_document(ctx, new_head_element, js_doc);
    dom_node_set_owner_document(ctx, new_body_element, js_doc);

    /* The new root replaces the document's current root element (the
     * skeleton, or the previous page). */
    NodeId old_de = dom_node_of(old_doc_element);
    if (old_de && dom_parent(old_de) == doc) dom_detach(old_de, true);

    /* Attach the root <html> element to the document. */
    js_cyber_ce_push_stack(ctx);
    dom_attach(doc, de, DOM_NULL);
    dom_ce_on_connect(ctx, de);
    js_cyber_ce_flush_reactions(ctx, JS_UNDEFINED, 0, NULL);
    js_cyber_ce_pop_stack(ctx);

    html_transfer_element_properties(ctx, new_doc_element, old_doc_element);
    html_transfer_element_properties(ctx, new_body_element, old_body_element);
    html_transfer_element_properties(ctx, new_head_element, old_head_element);

    JS_SetPropertyStr(ctx, js_doc, "documentElement", new_doc_element);
    JS_SetPropertyStr(ctx, js_doc, "head", new_head_element);
    JS_SetPropertyStr(ctx, js_doc, "body", new_body_element);
    JS_SetPropertyStr(ctx, new_doc_element, "head", new_head_element);
    JS_SetPropertyStr(ctx, new_doc_element, "body", new_body_element);
    /* (the page's root only: not a DOMParser document's) */
    if (page) g_dom_index_root = de;
    dom_request_layout();
    return true;
}

bool html_populate_js_document_from_html(JSContextHandle ctx, GCValue js_doc, const char *html, size_t len) {
    if (!SAFE_VALID(ctx) || !html || !len) return false;
    NodeId doc = owner_doc_of(js_doc);
    NodeId frag = dom_parse_html(html, len, doc);
    bool ok = populate_from_fragment(ctx, js_doc, frag, true);
    dom_free_subtree(frag);
    LOG_INFO("JS document populated from HTML (%zu bytes, %u nodes in store)", len, g_dom.live);
    return ok;
}

/* ============================================================================
 * Streaming the page into the document (html_doc_stream_*)
 * ============================================================================ */

struct HtmlDocStream {
    JSContextHandle ctx;
    GCValue js_doc;                         /* rooted, as are the old elements */
    GCValue old_de, old_head, old_body;     /* the skeleton's, for their properties */
    DomStream *st;
    NodeId doc, de, head, body;
};

static void stream_root(GCValue v, bool add) {
    if (!JS_IsObject(v)) return;
    GCHandle h = GC_VALUE_GET_HANDLE(v);
    if (h == GC_HANDLE_NULL) return;
    if (add) gc_add_root(h); else gc_remove_root(h);
}

HtmlDocStream *html_doc_stream_begin(JSContextHandle ctx, GCValue js_doc) {
    NodeId doc = dom_node_of(js_doc);
    if (dom_kind(doc) != DOM_K_DOCUMENT) return NULL;
    HtmlDocStream *s = (HtmlDocStream *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->ctx = ctx;
    s->doc = doc;
    s->js_doc = js_doc;
    s->old_de = JS_GetPropertyStr(ctx, js_doc, "documentElement");
    s->old_head = JS_GetPropertyStr(ctx, js_doc, "head");
    s->old_body = JS_GetPropertyStr(ctx, js_doc, "body");
    stream_root(s->js_doc, true);
    stream_root(s->old_de, true);
    stream_root(s->old_head, true);
    stream_root(s->old_body, true);
    /* The page replaces the skeleton's root element. */
    NodeId old = dom_node_of(s->old_de);
    if (old && dom_parent(old) == doc) dom_detach(old, true);
    JS_SetPropertyStr(ctx, js_doc, "readyState", JS_NewString(ctx, "loading"));
    s->st = dom_stream_begin(doc, doc);
    if (!s->st) { html_doc_stream_end(s); return NULL; }
    return s;
}

void html_doc_stream_append(HtmlDocStream *s, const char *data, size_t len) {
    if (s) dom_stream_append(s->st, data, len);
}

/* The first element child of `parent` (named `tag`, or any when NULL). */
static NodeId first_child_tagged(NodeId parent, const char *tag) {
    for (NodeId c = dom_first_child(parent); c; c = dom_next_sibling(c))
        if (dom_kind(c) == DOM_K_ELEMENT && (!tag || strcmp(dom_tag_name(c), tag) == 0)) return c;
    return DOM_NULL;
}

/* The document's root, head and body as the parser makes them, with the
 * skeleton's properties moved over (as html_populate_js_document_from_html
 * does for the whole page at once). */
static void stream_note_root_elements(HtmlDocStream *s) {
    JSContextHandle ctx = s->ctx;
    /* (The root is the first element, whatever its name, as for the whole
     * page at once.) */
    if (!s->de && (s->de = first_child_tagged(s->doc, NULL))) {
        GCValue e = dom_js(ctx, s->de);
        dom_node_set_owner_document(ctx, e, s->js_doc);
        html_transfer_element_properties(ctx, e, s->old_de);
        JS_SetPropertyStr(ctx, s->js_doc, "documentElement", e);
        /* (the page's root only: a frame's document has its own) */
        if (s->doc == g_dom_document) g_dom_index_root = s->de;
    }
    if (!s->de) return;
    if (!s->head && (s->head = first_child_tagged(s->de, "head"))) {
        GCValue e = dom_js(ctx, s->head);
        dom_node_set_owner_document(ctx, e, s->js_doc);
        html_transfer_element_properties(ctx, e, s->old_head);
        JS_SetPropertyStr(ctx, s->js_doc, "head", e);
        JS_SetPropertyStr(ctx, dom_js(ctx, s->de), "head", e);
    }
    if (!s->body && (s->body = first_child_tagged(s->de, "body"))) {
        GCValue e = dom_js(ctx, s->body);
        dom_node_set_owner_document(ctx, e, s->js_doc);
        html_transfer_element_properties(ctx, e, s->old_body);
        JS_SetPropertyStr(ctx, s->js_doc, "body", e);
        JS_SetPropertyStr(ctx, dom_js(ctx, s->de), "body", e);
    }
}

static bool in_document(uint32_t slot, uint32_t doc) {
    for (uint32_t p = slot; p; p = g_dom.parent[p]) if (p == doc) return true;
    return false;
}

GCValue html_doc_stream_step(HtmlDocStream *s, bool final) {
    if (!s) return JS_NULL;
    JSContextHandle ctx = s->ctx;
    NodeId script = dom_stream_pump(s->st, final);
    stream_note_root_elements(s);
    /* What the parser inserted is connected now: custom elements defined so
     * far upgrade and get connectedCallback, once per new subtree. */
    uint32_t n = 0;
    const uint32_t *roots = dom_stream_new_roots(s->st, &n);
    /* The parser inserts one node at a time: each is a childList record for
     * observers of its parent, in document order. */
    if (n && mo_any_observing(ctx)) {
        uint32_t doc = dom_slot(s->doc), last_parent = 0;
        bool observed = false;
        for (uint32_t i = 0; i < n; i++) {
            if (!in_document(roots[i], doc)) continue;
            for (uint32_t k = roots[i]; k; k = dom_next_in_subtree(k, roots[i])) {
                uint32_t p = g_dom.parent[k];
                if (p != last_parent) {
                    last_parent = p;
                    observed = mo_child_list_observed(ctx, dom_js(ctx, dom_id_of(p)));
                }
                if (!observed) continue;
                GCValue added = JS_NewArray(ctx);
                JS_SetPropertyUint32(ctx, added, 0, dom_js(ctx, dom_id_of(k)));
                mo_notify_child_list(ctx, dom_js(ctx, dom_id_of(p)), added, JS_NewArray(ctx));
            }
        }
    }
    if (n) {
        js_cyber_ce_push_stack(ctx);
        for (uint32_t i = 0; i < n; i++)
            if (g_dom.kind[roots[i]] == DOM_K_ELEMENT && in_document(roots[i], dom_slot(s->doc)))
                dom_ce_on_connect(ctx, dom_id_of(roots[i]));
        js_cyber_ce_flush_reactions(ctx, JS_UNDEFINED, 0, NULL);
        js_cyber_ce_pop_stack(ctx);
        dom_request_layout();
    }
    return script ? dom_js(ctx, script) : JS_NULL;
}

DomStream *html_doc_stream_parser(HtmlDocStream *s) {
    return s ? s->st : NULL;
}

const char *html_doc_stream_source(HtmlDocStream *s, size_t *len) {
    return s ? dom_stream_source(s->st, len) : NULL;
}

void html_doc_stream_end(HtmlDocStream *s) {
    if (!s) return;
    JSContextHandle ctx = s->ctx;
    /* A page without <html>, <head> or <body> gets them, as the whole-page
     * population does: what the parser put at the top goes into the root. */
    if (s->st) {
        if (!s->de) {
            NodeId de = shallow_copy(DOM_NULL, "html", s->doc);
            move_children(s->doc, de, false);
            dom_attach(s->doc, de, DOM_NULL);
        }
        stream_note_root_elements(s);
        if (s->de && !s->head) {
            NodeId head = shallow_copy(DOM_NULL, "head", s->doc);
            dom_attach(s->de, head, dom_first_child(s->de));
        }
        if (s->de && !s->body) {
            /* (After the head, as html_populate_js_document_from_html puts it.) */
            NodeId body = shallow_copy(DOM_NULL, "body", s->doc);
            NodeId head = first_child_tagged(s->de, "head");
            dom_attach(s->de, body, head ? dom_next_sibling(head) : dom_first_child(s->de));
        }
        stream_note_root_elements(s);
        dom_request_layout();
        dom_stream_end(s->st);
    }
    stream_root(s->js_doc, false);
    stream_root(s->old_de, false);
    stream_root(s->old_head, false);
    stream_root(s->old_body, false);
    (void)ctx;
    free(s);
}

bool html_populate_js_document(JSContextHandle ctx, GCValue js_doc, HtmlDocument *doc) {
    if (!SAFE_VALID(ctx) || !doc) return false;
    if (JS_IsUndefined(js_doc) || JS_IsNull(js_doc)) return false;
    NodeId frag = fragment_from_html_document(doc, owner_doc_of(js_doc));
    bool ok = populate_from_fragment(ctx, js_doc, frag, true);
    dom_free_subtree(frag);
    return ok;
}

/* A new document object (store document + wrapper) with the methods page
 * code uses on documents. */
static GCValue new_js_document(JSContextHandle ctx) {
    NodeId d = dom_create_document();
    GCValue js_doc = dom_js(ctx, d);
    JS_SetPropertyStr(ctx, js_doc, "nodeType", JS_NewInt32(ctx, 9));
    JS_SetPropertyStr(ctx, js_doc, "readyState", JS_NewString(ctx, "complete"));
    JS_SetPropertyStr(ctx, js_doc, "characterSet", JS_NewString(ctx, "UTF-8"));
    JS_SetPropertyStr(ctx, js_doc, "contentType", JS_NewString(ctx, "text/html"));
    JS_SetPropertyStr(ctx, js_doc, "createElement",
        JS_NewCFunction(ctx, js_document_create_element, "createElement", 1));
    JS_SetPropertyStr(ctx, js_doc, "createTextNode",
        JS_NewCFunction(ctx, js_document_create_text_node, "createTextNode", 1));
    JS_SetPropertyStr(ctx, js_doc, "createDocumentFragment",
        JS_NewCFunction(ctx, js_document_create_document_fragment, "createDocumentFragment", 0));
    JS_SetPropertyStr(ctx, js_doc, "getElementById",
        JS_NewCFunction(ctx, js_document_getElementById, "getElementById", 1));
    JS_SetPropertyStr(ctx, js_doc, "querySelector",
        JS_NewCFunction(ctx, js_document_querySelector, "querySelector", 1));
    JS_SetPropertyStr(ctx, js_doc, "querySelectorAll",
        JS_NewCFunction(ctx, js_document_querySelectorAll, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, js_doc, "getElementsByTagName",
        JS_NewCFunction(ctx, js_document_get_elements_by_tag_name, "getElementsByTagName", 1));
    JS_SetPropertyStr(ctx, js_doc, "getElementsByClassName",
        JS_NewCFunction(ctx, js_document_getElementsByClassName, "getElementsByClassName", 1));
    return js_doc;
}

GCValue dom_new_parsed_document(JSContextHandle ctx, const char *html, size_t len) {
    GCValue js_doc = new_js_document(ctx);
    NodeId frag = dom_parse_html(html, len, dom_node_of(js_doc));
    populate_from_fragment(ctx, js_doc, frag, false);
    dom_free_subtree(frag);
    return js_doc;
}

/* Create JavaScript document object with parsed HTML structure */
GCValue html_create_js_document(JSContextHandle ctx, HtmlDocument *doc) {
    if (!SAFE_VALID(ctx) || !doc) return JS_NULL;
    GCValue js_doc = new_js_document(ctx);
    NodeId frag = fragment_from_html_document(doc, dom_node_of(js_doc));
    populate_from_fragment(ctx, js_doc, frag, true);
    dom_free_subtree(frag);
    return js_doc;
}

/* Main entry point: parse HTML and create DOM in JS context */
bool html_create_dom_in_js(JSContextHandle ctx, HtmlDocument *doc) {
    if (!SAFE_VALID(ctx) || !doc) return false;
    GCValue js_doc = html_create_js_document(ctx, doc);
    if (JS_IsNull(js_doc) || JS_IsException(js_doc)) return false;
    GCValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "document", js_doc);
    GCValue doc_elem = JS_GetPropertyStr(ctx, js_doc, "documentElement");
    if (!JS_IsNull(doc_elem) && !JS_IsUndefined(doc_elem))
        JS_SetPropertyStr(ctx, global, "documentElement", doc_elem);
    return true;
}

/* innerHTML: remove the children of `target`, then insert the parsed nodes
 * (the parsed <body>'s children when the input has a non-empty body, else
 * the top-level nodes; a text node with the raw string when nothing parsed).
 * Insertions go through the DOM API so custom elements and observers see
 * them. */
void dom_set_inner_html(JSContextHandle ctx, NodeId target, const char *html, size_t len) {
    CP_ACC("innerHTML set");
    if (!dom_valid(target)) return;
    while (dom_first_child(target)) {
        if (!dom_api_remove_discard(ctx, target, dom_first_child(target))) {
            dom_detach(dom_first_child(target), true);
        }
    }
    if (!len) return;
    NodeId owner = dom_kind(target) == DOM_K_DOCUMENT ? target : dom_owner(target);
    NodeId frag = dom_parse_html_fragment(html, len, owner);
    dom_adopt_template_contents(frag);
    NodeId start = dom_parse_fragment_source(frag);
    if (!start) {
        NodeId t = dom_create_chardata(DOM_K_TEXT, html, len, owner);
        dom_api_insert(ctx, target, t, DOM_NULL);
    } else {
        NodeId container = dom_parent(start);
        for (NodeId c = start; c; ) {
            NodeId next = dom_next_sibling(c);
            dom_detach(c, false);
            if (!dom_valid(target) || !dom_api_insert(ctx, target, c, DOM_NULL)) {
                if (dom_valid(c) && !dom_parent(c)) dom_free_if_unwrapped(c);
            }
            c = next;
            if (c && dom_parent(c) != container) break;
        }
    }
    dom_free_subtree(frag);
}

/* Set the innerHTML of a ShadowRoot. */
bool html_shadow_root_set_inner_html(JSContextHandle ctx, GCValue shadow_root, const char *html) {
    if (!SAFE_VALID(ctx) || !JS_IsObject(shadow_root) || !html) return false;
    NodeId n = dom_node_of(shadow_root);
    if (!n) return false;
    dom_set_inner_html(ctx, n, html, strlen(html));
    return true;
}

/* Set the innerHTML of an element (template elements: their content). */
bool html_element_set_inner_html_len(JSContextHandle ctx, GCValue elem, const char *html, size_t len) {
    if (!SAFE_VALID(ctx) || !JS_IsObject(elem) || !html) return false;
    NodeId n = dom_node_ensure(ctx, elem, DOM_K_ELEMENT);
    if (!n) return false;
    if (dom_kind(n) == DOM_K_ELEMENT && strcmp(dom_tag_name(n), "template") == 0) {
        NodeId c = dom_template_content(n);
        if (!c) { c = dom_create_fragment(dom_owner(n)); dom_set_template_content(n, c); }
        n = c;
    }
    if (CYBER_ENV(CYBER_T_INNERHTML) && dom_kind(n) != DOM_K_ELEMENT) {
        const char *trace_js = "(new Error('ih')).stack";
        GCValue tv = JS_Eval(ctx, trace_js, strlen(trace_js), "<ihtrace>", JS_EVAL_TYPE_GLOBAL);
        const char *ts = JS_ToCString(ctx, tv);
        fprintf(stderr, "[IH-TRACE] nodeType=%d:\n%s\n", dom_kind(n), ts ? ts : "?");
        fflush(stderr);
    }
    dom_set_inner_html(ctx, n, html, len);
    return true;
}

bool html_element_set_inner_html(JSContextHandle ctx, GCValue elem, const char *html) {
    return html ? html_element_set_inner_html_len(ctx, elem, html, strlen(html)) : false;
}


/* ============================================================================
 * Document accessors and search
 * ============================================================================ */

HtmlNode* html_document_root(HtmlDocument *doc) {
    return html_node_at(doc, doc ? doc->root_idx : -1);
}

HtmlNode* html_document_head(HtmlDocument *doc) {
    return html_node_at(doc, doc ? doc->head_idx : -1);
}

HtmlNode* html_document_body(HtmlDocument *doc) {
    return html_node_at(doc, doc ? doc->body_idx : -1);
}

HtmlNode* html_node_parent(HtmlDocument *doc, HtmlNode *node) {
    int idx = po_array_index_from_payload(&doc->array, node);
    int p = po_array_parent(&doc->array, idx);
    return html_node_at(doc, p);
}

HtmlNode* html_node_first_child(HtmlDocument *doc, HtmlNode *node) {
    int idx = po_array_index_from_payload(&doc->array, node);
    int c = po_array_first_child(&doc->array, idx);
    return html_node_at(doc, c);
}

HtmlNode* html_node_last_child(HtmlDocument *doc, HtmlNode *node) {
    int idx = po_array_index_from_payload(&doc->array, node);
    int c = po_array_last_child(&doc->array, idx);
    return html_node_at(doc, c);
}

HtmlNode* html_node_next_sibling(HtmlDocument *doc, HtmlNode *node) {
    int idx = po_array_index_from_payload(&doc->array, node);
    int s = po_array_next_sibling(&doc->array, idx);
    return html_node_at(doc, s);
}

HtmlNode* html_node_prev_sibling(HtmlDocument *doc, HtmlNode *node) {
    int idx = po_array_index_from_payload(&doc->array, node);
    int s = po_array_prev_sibling(&doc->array, idx);
    return html_node_at(doc, s);
}

/* Helper to get element by tag name from document */
HtmlNode* html_document_get_element_by_tag(HtmlDocument *doc, const char *tag_name) {
    if (!doc || !tag_name) return NULL;
    
    /* Check head */
    HtmlNode *head = html_document_head(doc);
    if (head && strcasecmp(head->tag_name, tag_name) == 0) {
        return head;
    }
    
    /* Check body */
    HtmlNode *body = html_document_body(doc);
    if (body && strcasecmp(body->tag_name, tag_name) == 0) {
        return body;
    }
    
    /* Search in root */
    HtmlNode *root = html_document_root(doc);
    if (root && strcasecmp(root->tag_name, tag_name) == 0) {
        return root;
    }
    
    /* Breadth-first search over the tree */
    int queue[HTML_MAX_NESTING_DEPTH];
    int head_q = 0, tail = 0;
    
    if (doc->root_idx >= 0) queue[tail++] = doc->root_idx;
    
    while (head_q < tail) {
        int current_idx = queue[head_q++];
        HtmlNode *current = html_node_at(doc, current_idx);
        
        if (current && strcasecmp(current->tag_name, tag_name) == 0) {
            return current;
        }
        
        int child_idx = po_array_first_child(&doc->array, current_idx);
        while (child_idx >= 0 && tail < HTML_MAX_NESTING_DEPTH) {
            queue[tail++] = child_idx;
            child_idx = po_array_next_sibling(&doc->array, child_idx);
        }
    }
    
    return NULL;
}

/* Helper to get element by id */
HtmlNode* html_document_get_element_by_id(HtmlDocument *doc, const char *id) {
    if (!doc || !id || !id[0]) return NULL;
    
    int stack[HTML_MAX_NESTING_DEPTH];
    int stack_top = 0;
    
    if (doc->root_idx >= 0) stack[stack_top++] = doc->root_idx;
    
    while (stack_top > 0) {
        int current_idx = stack[--stack_top];
        HtmlNode *current = html_node_at(doc, current_idx);
        if (!current) continue;
        
        for (HtmlAttribute *attr = current->attributes; attr != NULL; attr = attr->next) {
            if (strcasecmp(attr->name, "id") == 0 && strcasecmp(attr->value, id) == 0) {
                return current;
            }
        }
        
        int child_idx = po_array_first_child(&doc->array, current_idx);
        int children[HTML_MAX_NESTING_DEPTH];
        int child_count = 0;
        while (child_idx >= 0 && child_count < HTML_MAX_NESTING_DEPTH) {
            children[child_count++] = child_idx;
            child_idx = po_array_next_sibling(&doc->array, child_idx);
        }
        for (int i = child_count - 1; i >= 0 && stack_top < HTML_MAX_NESTING_DEPTH; i--) {
            stack[stack_top++] = children[i];
        }
    }
    
    return NULL;
}

/* Helper to get all elements by tag name */
int html_document_get_elements_by_tag(HtmlDocument *doc, const char *tag_name,
                                       HtmlNode **out_nodes, int max_nodes) {
    if (!doc || !tag_name || !out_nodes || max_nodes <= 0) return 0;
    
    int count = 0;
    
    /* Search recursively using a stack */
    int stack[HTML_MAX_NESTING_DEPTH];
    int stack_top = 0;
    
    if (doc->root_idx >= 0) stack[stack_top++] = doc->root_idx;
    
    while (stack_top > 0 && count < max_nodes) {
        int current_idx = stack[--stack_top];
        HtmlNode *current = html_node_at(doc, current_idx);
        
        if (current && strcasecmp(current->tag_name, tag_name) == 0) {
            out_nodes[count++] = current;
        }
        
        /* Add children to stack (in reverse order for correct order) */
        int child_idx = po_array_first_child(&doc->array, current_idx);
        int children[HTML_MAX_NESTING_DEPTH];
        int child_count = 0;
        
        while (child_idx >= 0 && child_count < HTML_MAX_NESTING_DEPTH) {
            children[child_count++] = child_idx;
            child_idx = po_array_next_sibling(&doc->array, child_idx);
        }
        
        for (int i = child_count - 1; i >= 0 && stack_top < HTML_MAX_NESTING_DEPTH; i--) {
            stack[stack_top++] = children[i];
        }
    }
    
    return count;
}

/* ============================================================================
 * Render snapshot: the store (plus the JS state layout depends on) as HTML,
 * re-parsed into the native HtmlDocument the layout engine reads.
 * ============================================================================
 * The serialization reproduces the legacy one exactly: lowercase tags, store
 * attributes with the style attribute merged with the element's style
 * object, attributes scripts set only as JS properties, raw-text elements
 * from textContent, and open shadow roots composed with their hosts' light
 * children through slots (read through JS, where ShadyDOM patches them).
 * Comments, stray closing-tag text and comment-marker text are dropped.
 * Elements the reparse produces carry the store id they came from.
 */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    /* Store ids of the emitted elements in order (0 for objects without a
     * node); only kept by the render snapshot. */
    uint32_t *ids;
    const char **tags;       /* interned (css_atom_str), not owned */
    size_t nids, capids;
    bool track;
    bool omit_script_text;   /* render snapshot: script text never renders */
    /* Incremental render snapshot (see SnapCache): the generation this
     * buffer records element fragments under (0 = none), whether clean
     * fragments of the previous snapshot may be copied, and a count of
     * reads of state the store does not track (the subtrees around them
     * are not recorded). */
    uint32_t gen;
    bool reuse;
    uint32_t taint;
    bool shady;              /* the page renders shadow roots with ShadyDOM */
} SerializeBuffer;

static bool serialize_buf_init(SerializeBuffer *buf) {
    if (!buf) return false;
    memset(buf, 0, sizeof(*buf));
    buf->data = (char *)malloc(4096);
    if (!buf->data) return false;
    buf->data[0] = '\0';
    buf->cap = 4096;
    return true;
}

static void serialize_buf_free(SerializeBuffer *buf) {
    if (!buf) return;
    free(buf->data);
    free(buf->ids);
    free(buf->tags);
    memset(buf, 0, sizeof(*buf));
}

static bool serialize_buf_grow(SerializeBuffer *buf, size_t extra) {
    if (!buf || !buf->data) return false;
    size_t needed = buf->len + extra + 1;
    if (needed <= buf->cap) return true;
    size_t new_cap = buf->cap * 2;
    while (new_cap < needed) new_cap *= 2;
    char *new_data = (char *)realloc(buf->data, new_cap);
    if (!new_data) return false;
    buf->data = new_data;
    buf->cap = new_cap;
    return true;
}

static bool serialize_buf_append_raw(SerializeBuffer *buf, const char *s, size_t n) {
    if (!buf || !s) return false;
    if (n == 0) return true;
    if (!serialize_buf_grow(buf, n)) return false;
    memcpy(buf->data + buf->len, s, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return true;
}

static bool serialize_buf_append(SerializeBuffer *buf, const char *s) {
    if (!s) return true;
    return serialize_buf_append_raw(buf, s, strlen(s));
}

/* CYBER_DOM_LEGACY_LIMITS=1 (verification against the legacy DOM): cut
 * attribute names / values and text the way its fixed buffers did. */
static int legacy_limits(void) {
    static int v = -1;
    if (v < 0) v = cyber_getenv("CYBER_DOM_LEGACY_LIMITS") ? 1 : 0;
    return v;
}
static void serialize_note_element(SerializeBuffer *buf, NodeId id, const char *tag) {
    if (!buf->track) return;
    if (buf->nids == buf->capids) {
        buf->capids = buf->capids ? buf->capids * 2 : 256;
        buf->ids = realloc(buf->ids, buf->capids * sizeof(uint32_t));
        buf->tags = realloc(buf->tags, buf->capids * sizeof(char *));
    }
    buf->ids[buf->nids] = id;
    css_atom_reserve(1);
    buf->tags[buf->nids] = css_atom_str(css_atom_intern(tag, strlen(tag)));
    buf->nids++;
}

/* Escaped text, stopping at an embedded NUL (as the C-string serializer did). */
static void serialize_buf_append_escaped_n(SerializeBuffer *buf, const char *s, size_t n) {
    if (!s) return;
    const char *run = s;
    const char *end = s + n;
    const char *p;
    for (p = s; p < end && *p; p++) {
        const char *rep = NULL;
        char tmp[8];
        switch (*p) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            default:
                if ((unsigned char)*p < 0x20 && !isspace((unsigned char)*p)) {
                    snprintf(tmp, sizeof(tmp), "&#%d;", (unsigned char)*p);
                    rep = tmp;
                }
                break;
        }
        if (rep) {
            serialize_buf_append_raw(buf, run, (size_t)(p - run));
            serialize_buf_append(buf, rep);
            run = p + 1;
        }
    }
    serialize_buf_append_raw(buf, run, (size_t)(p - run));
}

static void serialize_buf_append_escaped(SerializeBuffer *buf, const char *s) {
    if (s) serialize_buf_append_escaped_n(buf, s, strlen(s));
}

static void serialize_buf_append_attr(SerializeBuffer *buf, const char *name, const char *value) {
    serialize_buf_append(buf, " ");
    serialize_buf_append(buf, name);
    serialize_buf_append(buf, "=\"");
    serialize_buf_append_escaped(buf, value);
    serialize_buf_append(buf, "\"");
}

/* Read a string property from a JS object. Returns a freshly allocated string
 * that the caller must free, or NULL if the property is missing/undefined. */
/* The object's own string data property `prop` (no getters, no prototype
 * chain), or NULL. */
static char *js_object_get_own_string_atom(JSContextHandle ctx, GCValue obj, JSAtom atom) {
    if (!JS_IsObject(obj)) return NULL;
    JSPropertyDescriptor desc = JS_PROPERTY_DESCRIPTOR_INIT;
    int has = JS_GetOwnProperty(ctx, &desc, obj, atom);
    if (has <= 0 || (desc.flags & JS_PROP_GETSET) || !JS_IsString(desc.value)) return NULL;
    const char *s = JS_ToCString(ctx, desc.value);
    char *out = s ? strdup(s) : NULL;
    if (s) JS_FreeCString(ctx, s);
    return out;
}

/* js_object_get_string through an atom. */
static char *js_object_get_string_atom(JSContextHandle ctx, GCValue obj, JSAtom atom) {
    if (!JS_IsObject(obj)) return NULL;
    GCValue val = JS_GetProperty(ctx, obj, atom);
    if (!JS_IsString(val)) return NULL;
    const char *s = JS_ToCString(ctx, val);
    return s ? strdup(s) : NULL;
}

/* Elements whose script-only properties (value, a video's src, ...) live in
 * native accessors rather than attributes. */
static bool ser_has_native_props(const char *tag) {
    static const char *const tags[] = {
        "input", "textarea", "select", "option", "button", "video", "audio",
        "source", "img", "canvas", "iframe", "embed", "object", NULL
    };
    for (int i = 0; tags[i]; i++) if (strcmp(tag, tags[i]) == 0) return true;
    return false;
}

/* A node as the serializer sees it: a store node (with its wrapper, if it
 * has one), or a script object that is not bound to a node (ShadyDOM keeps
 * some shadow-tree nodes that way). */
typedef struct SerNode {
    NodeId id;          /* DOM_NULL for an unbound object */
    GCValue obj;        /* the wrapper / object, JS_NULL when unwrapped */
} SerNode;

static void html_serialize_node(JSContextHandle ctx, SerNode n, SerializeBuffer *buf);

static SerNode ser_of_value(GCValue v) {
    SerNode s = { DOM_NULL, JS_NULL };
    if (!JS_IsObject(v)) return s;
    s.obj = v;
    s.id = dom_node_of(v);
    return s;
}

static SerNode ser_of_node(NodeId id) {
    SerNode s = { id, JS_NULL };
    if (dom_has_wrapper(id)) s.obj = dom_js_existing(id);
    return s;
}

static int ser_node_type(JSContextHandle ctx, SerNode n) {
    if (n.id) {
        switch (dom_kind(n.id)) {
            case DOM_K_ELEMENT: return 1;
            case DOM_K_TEXT: return 3;
            case DOM_K_COMMENT: return 8;
            case DOM_K_DOCUMENT: return 9;
            case DOM_K_FRAGMENT: return 11;
            default: return 0;
        }
    }
    if (!JS_IsObject(n.obj)) return 0;
    GCValue jnt = JS_GetPropertyStr(ctx, n.obj, "nodeType");
    int32_t t = 0;
    JS_ToInt32(ctx, &t, jnt);
    return t;
}

/* Text of a text node / textContent of an element. */
static void ser_text_content(NodeId id, SerializeBuffer *out) {
    if (dom_kind(id) == DOM_K_TEXT) {
        uint32_t len = 0;
        const char *s = dom_text(id, &len);
        serialize_buf_append_raw(out, s, strnlen(s, len));
        return;
    }
    uint32_t r = dom_slot(id);
    for (uint32_t s = r; s; s = dom_next_in_subtree(s, r)) {
        if (g_dom.kind[s] == DOM_K_TEXT) {
            uint32_t len = 0;
            const char *t = dom_text(dom_id_of(s), &len);
            size_t tl = strnlen(t, len);
            if (legacy_limits() && tl > 4095) tl = 4095;
            serialize_buf_append_raw(out, t, tl);
        }
    }
}

typedef struct LightChildEntry {
    SerNode node;
    const char *slot;
    bool used;
} LightChildEntry;

typedef struct LightChildList {
    LightChildEntry *data;
    size_t count;
    size_t capacity;
} LightChildList;
static void light_child_list_push(LightChildList *l, SerNode node, const char *slot) {
    if (l->count == l->capacity) {
        size_t cap = l->capacity ? l->capacity * 2 : 8;
        LightChildEntry *data = (LightChildEntry *)realloc(l->data, cap * sizeof(LightChildEntry));
        if (!data) abort();
        l->data = data;
        l->capacity = cap;
    }
    l->data[l->count].node = node;
    GC_SET(l->data[l->count].node.obj, node.obj);
    l->data[l->count].slot = slot;
    l->data[l->count].used = false;
    l->count++;
}
static void light_child_list_free(LightChildList *l) {
    free(l->data);
    l->data = NULL;
    l->count = l->capacity = 0;
}

/* Is there rendered content in the (open) shadow root: an element or
 * non-whitespace text, read through JS (the composed tree under ShadyDOM). */
static bool shadow_has_content(JSContextHandle ctx, GCValue shadow) {
    GCValue probe = JS_GetPropertyStr(ctx, shadow, "firstChild");
    while (!JS_IsUndefined(probe) && !JS_IsNull(probe) && JS_IsObject(probe)) {
        SerNode pn = ser_of_value(probe);
        int nt = ser_node_type(ctx, pn);
        if (nt == 1) return true;
        if (nt == 3) {
            GCValue tc = JS_GetPropertyStr(ctx, probe, "textContent");
            const char *val = JS_ToCString(ctx, tc);
            bool nonws = false;
            if (val) {
                for (const char *p = val; *p; p++) {
                    if (!isspace((unsigned char)*p)) { nonws = true; break; }
                }
                JS_FreeCString(ctx, val);
            }
            if (nonws) return true;
        }
        probe = JS_GetPropertyStr(ctx, probe, "nextSibling");
    }
    return false;
}

/* Serialize the rendered children of an element.  If the host has an open
 * shadow root with content, light-DOM children are composed into the shadow
 * tree by matching <slot> elements.  Light children come from the store (the
 * ShadyDOM polyfill patches the JS-level getters to return the composed
 * tree); the shadow tree is read through JS. */
/* `store_path`: NULL forces composition through JS (verification); else set
 * when the host was serialized from the store (ShadyDOM). */
static void html_serialize_children_impl(JSContextHandle ctx, SerNode host, SerializeBuffer *buf,
                                         bool *store_path) {
    LightChildList light __attribute__((cleanup(light_child_list_free))) = {0};
    for (NodeId c = dom_first_child(host.id); c; c = dom_next_sibling(c)) {
        const char *slot_name = "";
        if (dom_kind(c) == DOM_K_ELEMENT) {
            const char *s = dom_attr(c, "slot");
            if (s) slot_name = s;
        }
        light_child_list_push(&light, ser_of_node(c), slot_name);
    }

    GCValue shadow = JS_NULL;
    if (JS_IsObject(host.obj)) {
        shadow = JS_GetPropertyStr(ctx, host.obj, "shadowRoot");
        if (JS_IsNull(shadow) || JS_IsUndefined(shadow))
            shadow = JS_GetPropertyStr(ctx, host.obj, "__CE_shadowRoot");
    }
    /* A store shadow root changes only through the store (its marks reach the
     * host); any other shadow object is composed through JS. */
    if (JS_IsObject(shadow)) {
        NodeId sid = dom_node_of(shadow);
        bool store_shadow = sid && host.id && g_dom.shadow[dom_slot(host.id)] == dom_slot(sid);
        /* ShadyDOM renders a shadow tree into the host's real children
         * (store nodes), leaving its <slot> elements in place around what
         * they distribute: that is the composition, without reading JS. */
        if (!store_shadow && store_path && buf->shady && host.id) {
            *store_path = true;
            for (NodeId c = dom_first_child(host.id); c; c = dom_next_sibling(c)) {
                if (dom_kind(c) == DOM_K_ELEMENT && strcmp(dom_tag_name(c), "slot") == 0) {
                    for (NodeId g = dom_first_child(c); g; g = dom_next_sibling(g))
                        html_serialize_node(ctx, ser_of_node(g), buf);
                } else {
                    html_serialize_node(ctx, ser_of_node(c), buf);
                }
            }
            return;
        }
        if (!store_shadow) buf->taint++;
    }
    if (!JS_IsObject(shadow) || !shadow_has_content(ctx, shadow)) {
        for (size_t i = 0; i < light.count; i++)
            html_serialize_node(ctx, light.data[i].node, buf);
        return;
    }

    GCValue schild = JS_GetPropertyStr(ctx, shadow, "firstChild");
    while (!JS_IsUndefined(schild) && !JS_IsNull(schild) && JS_IsObject(schild)) {
        SerNode sn = ser_of_value(schild);
        int snt = ser_node_type(ctx, sn);
        bool is_slot = false;
        if (snt == 1) {
            if (sn.id) {
                is_slot = strcmp(dom_tag_name(sn.id), "slot") == 0;
            } else {
                GCValue stag_val = JS_GetPropertyStr(ctx, schild, "nodeName");
                const char *stag = JS_ToCString(ctx, stag_val);
                is_slot = stag && strcasecmp(stag, "slot") == 0;
                if (stag) JS_FreeCString(ctx, stag);
            }
        }
        if (is_slot) {
            /* Distribute light-DOM children with a matching slot name, or
             * emit the slot's fallback content. */
            char slot_name[256] = "";
            if (sn.id) {
                const char *s = dom_attr(sn.id, "name");
                if (s) snprintf(slot_name, sizeof(slot_name), "%s", s);
            } else {
                GCValue sav = JS_GetPropertyStr(ctx, schild, "getAttribute");
                if (JS_IsFunction(ctx, sav)) {
                    GCValue args[1] = { JS_NewString(ctx, "name") };
                    GCValue sv = JS_Call(ctx, sav, schild, 1, args);
                    const char *s = JS_ToCString(ctx, sv);
                    if (s && s[0]) snprintf(slot_name, sizeof(slot_name), "%s", s);
                    if (s) JS_FreeCString(ctx, s);
                }
            }
            bool any = false;
            for (size_t i = 0; i < light.count; i++) {
                if (!light.data[i].used && strcmp(light.data[i].slot, slot_name) == 0) {
                    html_serialize_node(ctx, light.data[i].node, buf);
                    light.data[i].used = true;
                    any = true;
                }
            }
            if (!any) {
                GCValue fb = JS_GetPropertyStr(ctx, schild, "firstChild");
                while (!JS_IsNull(fb) && !JS_IsUndefined(fb) && JS_IsObject(fb)) {
                    html_serialize_node(ctx, ser_of_value(fb), buf);
                    fb = JS_GetPropertyStr(ctx, fb, "nextSibling");
                }
            }
        } else {
            html_serialize_node(ctx, sn, buf);
        }
        schild = JS_GetPropertyStr(ctx, schild, "nextSibling");
    }
}

static void html_serialize_children(JSContextHandle ctx, SerNode host, SerializeBuffer *buf) {
    size_t start = buf->len;
    bool store_path = false;
    html_serialize_children_impl(ctx, host, buf, &store_path);
    /* CYBER_SNAPSHOT_VERIFY=1: a ShadyDOM host's store children (slots
     * unwrapped) must equal its composition through JS. */
    static int verify = -1;
    if (verify < 0) verify = cyber_getenv("CYBER_SNAPSHOT_VERIFY") ? 1 : 0;
    if (!verify || !store_path) return;
    SerializeBuffer js;
    if (!serialize_buf_init(&js)) return;
    js.omit_script_text = buf->omit_script_text;
    html_serialize_children_impl(ctx, host, &js, NULL);
    size_t n = buf->len - start;
    static unsigned long checks, mismatches;
    checks++;
    if (n != js.len || memcmp(buf->data + start, js.data, n) != 0) {
        size_t i = 0;
        while (i < n && i < js.len && buf->data[start + i] == js.data[i]) i++;
        size_t from = i > 80 ? i - 80 : 0;
        if (++mismatches <= 10)
            fprintf(stderr, "[snapshot-verify] ShadyDOM <%s> store children differ from composition at %zu\n"
                            "  store: %.200s\n  composed: %.200s\n", dom_tag_name(host.id), i,
                    buf->data + start + from, from < js.len ? js.data + from : "");
    }
    if (checks % 100000 == 0)
        fprintf(stderr, "[snapshot-verify] %lu ShadyDOM hosts checked, %lu differ\n", checks, mismatches);
    serialize_buf_free(&js);
}

/* Attributes of an unbound script object, through getAttributeNames(). */
typedef struct LooseAttrs { char **names, **values; int n; } LooseAttrs;
static void loose_attrs_free(LooseAttrs *a) {
    for (int i = 0; i < a->n; i++) { free(a->names[i]); free(a->values[i]); }
    free(a->names); free(a->values);
}
static void loose_attrs_read(JSContextHandle ctx, GCValue node, LooseAttrs *out) {
    memset(out, 0, sizeof(*out));
    GCValue gan = JS_GetPropertyStr(ctx, node, "getAttributeNames");
    if (!JS_IsFunction(ctx, gan)) return;
    GCValue names_arr = JS_Call(ctx, gan, node, 0, NULL);
    if (!JS_IsArray(ctx, names_arr)) return;
    GCValue len_val = JS_GetPropertyStr(ctx, names_arr, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, len_val);
    for (uint32_t i = 0; i < len; i++) {
        GCValue name_val = JS_GetPropertyUint32(ctx, names_arr, i);
        const char *name = JS_ToCString(ctx, name_val);
        if (name && name[0]) {
            GCValue gav = JS_GetPropertyStr(ctx, node, "getAttribute");
            if (JS_IsFunction(ctx, gav)) {
                GCValue args[1] = { name_val };
                GCValue v = JS_Call(ctx, gav, node, 1, args);
                const char *vs = JS_ToCString(ctx, v);
                if (vs) {
                    /* Set semantics: a repeated name replaces the value. */
                    int k;
                    for (k = 0; k < out->n; k++) if (strcasecmp(out->names[k], name) == 0) break;
                    if (k == out->n) {
                        out->names = realloc(out->names, (out->n + 1) * sizeof(char *));
                        out->values = realloc(out->values, (out->n + 1) * sizeof(char *));
                        out->names[k] = strdup(name);
                        out->values[k] = NULL;
                        out->n++;
                    }
                    free(out->values[k]);
                    out->values[k] = strdup(vs);
                    JS_FreeCString(ctx, vs);
                }
            }
        }
        if (name) JS_FreeCString(ctx, name);
    }
}

static void append_style_object(JSContextHandle ctx, GCValue style_obj, SerializeBuffer *style_buf) {
    JSPropertyEnum *tab = NULL;
    uint32_t plen = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &plen, style_obj, JS_GPN_STRING_MASK) != 0 || !tab) return;
    for (uint32_t i = 0; i < plen; i++) {
        char key_buf[128];
        const char *key0 = JS_AtomToCString(ctx, tab[i].atom);
        if (!key0 || !key0[0]) continue;
        snprintf(key_buf, sizeof(key_buf), "%s", key0);
        const char *key = key_buf;
        if (key[0] == '_') continue;
        if (strcmp(key, "removeProperty") == 0 ||
            strcmp(key, "setProperty") == 0 ||
            strcmp(key, "getPropertyValue") == 0 ||
            strcmp(key, "getPropertyPriority") == 0 ||
            strcmp(key, "item") == 0 ||
            strcmp(key, "length") == 0 ||
            strcmp(key, "cssText") == 0) continue;
        GCValue val = JS_GetProperty(ctx, style_obj, tab[i].atom);
        if (!JS_IsString(val)) continue;
        /* Engine paths (cssText, setProperty, inline style) store both the
         * kebab-case and the camelCase name; script writes go to camelCase
         * (el.style.marginTop = ...), leaving the kebab twin stale.  So a
         * kebab-case name with a camelCase twin is skipped: the twin, even
         * when "" (cleared), is the current value. */
        if (strchr(key, '-')) {
            char *camel = css_to_camel_case(key);
            bool twin = camel && strcmp(camel, key) != 0 &&
                        JS_IsString(JS_GetPropertyStr(ctx, style_obj, camel));
            free(camel);
            if (twin) continue;
        }
        char val_buf[1024];
        const char *s0 = JS_ToCString(ctx, val);
        if (!s0 || !s0[0]) continue;
        snprintf(val_buf, sizeof(val_buf), "%s", s0);
        char kebab[128];
        size_t kn = 0;
        for (const char *p = key; *p && kn < sizeof(kebab) - 2; p++) {
            if (*p >= 'A' && *p <= 'Z') {
                kebab[kn++] = '-';
                kebab[kn++] = (char)tolower((unsigned char)*p);
            } else {
                kebab[kn++] = *p;
            }
        }
        kebab[kn] = '\0';
        serialize_buf_append(style_buf, kebab);
        serialize_buf_append(style_buf, ":");
        serialize_buf_append(style_buf, val_buf);
        serialize_buf_append(style_buf, ";");
    }
    JS_FreePropertyEnum(ctx, tab, plen);
}

/* The style object an element has always had, when it has not been created
 * yet: the createElement() object with the inline style attribute applied
 * (each declaration under its name and its camelCase name).  Emits exactly
 * what append_style_object() would for that object. */
typedef struct { char *k; char *v; } StyleProp;
static int style_prop_find(StyleProp *p, int n, const char *k) {
    for (int i = 0; i < n; i++) if (strcmp(p[i].k, k) == 0) return i;
    return -1;
}
static int style_prop_set(StyleProp **p, int *n, int *cap, const char *k, const char *v) {
    int i = style_prop_find(*p, *n, k);
    if (i >= 0) { free((*p)[i].v); (*p)[i].v = strdup(v); return i; }
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 16; *p = realloc(*p, *cap * sizeof(StyleProp)); }
    (*p)[*n].k = strdup(k);
    (*p)[*n].v = strdup(v);
    return (*n)++;
}
static void append_emulated_style_object(const char *attr, SerializeBuffer *style_buf) {
    if (!attr || !attr[0]) return;
    int count = 0;
    CssDeclaration *decls = css_parse_inline_style(attr, &count);
    if (!decls) return;
    StyleProp *props = NULL;
    int n = 0, cap = 0;
    style_prop_set(&props, &n, &cap, "animationTimingFunction", "");
    for (int i = 0; i < count; i++) {
        if (!decls[i].property || !decls[i].value) continue;
        style_prop_set(&props, &n, &cap, decls[i].property, decls[i].value);
        char *camel = css_to_camel_case(decls[i].property);
        if (camel && strcmp(camel, decls[i].property) != 0)
            style_prop_set(&props, &n, &cap, camel, decls[i].value);
        free(camel);
    }
    css_declarations_free(decls, count);
    for (int i = 0; i < n; i++) {
        char key[128];
        snprintf(key, sizeof(key), "%s", props[i].k);
        if (!key[0] || key[0] == '_') continue;
        int ki = style_prop_find(props, n, key);
        const char *v = ki >= 0 ? props[ki].v : NULL;
        if (!v || !v[0]) continue;
        char val_buf[1024];
        snprintf(val_buf, sizeof(val_buf), "%s", v);
        char kebab[128];
        size_t kn = 0;
        bool had_upper = false;
        for (const char *p = key; *p && kn < sizeof(kebab) - 2; p++) {
            if (*p >= 'A' && *p <= 'Z') {
                kebab[kn++] = '-';
                kebab[kn++] = (char)tolower((unsigned char)*p);
                had_upper = true;
            } else {
                kebab[kn++] = *p;
            }
        }
        kebab[kn] = '\0';
        if (had_upper && style_prop_find(props, n, kebab) >= 0) continue;
        serialize_buf_append(style_buf, kebab);
        serialize_buf_append(style_buf, ":");
        serialize_buf_append(style_buf, val_buf);
        serialize_buf_append(style_buf, ";");
    }
    for (int i = 0; i < n; i++) { free(props[i].k); free(props[i].v); }
    free(props);
}

static const char *legacy_cut(const char *s, size_t max, char *tmp) {
    if (!legacy_limits() || !s || strlen(s) < max) return s;
    memcpy(tmp, s, max - 1);
    tmp[max - 1] = '\0';
    return tmp;
}

/* Attribute names are stored lower case; SVG's camelCase attributes are
 * written back in their SVG spelling (the HTML parser's "adjust SVG
 * attributes" table). */
const char *dom_attr_name_for_output(const char *lower) {
    static const char *const svg_names[] = {
        "attributeName", "attributeType", "baseFrequency", "baseProfile", "calcMode",
        "clipPathUnits", "diffuseConstant", "edgeMode", "filterUnits", "glyphRef",
        "gradientTransform", "gradientUnits", "kernelMatrix", "kernelUnitLength",
        "keyPoints", "keySplines", "keyTimes", "lengthAdjust", "limitingConeAngle",
        "markerHeight", "markerUnits", "markerWidth", "maskContentUnits", "maskUnits",
        "numOctaves", "pathLength", "patternContentUnits", "patternTransform",
        "patternUnits", "pointsAtX", "pointsAtY", "pointsAtZ", "preserveAlpha",
        "preserveAspectRatio", "primitiveUnits", "refX", "refY", "repeatCount",
        "repeatDur", "requiredExtensions", "requiredFeatures", "specularConstant",
        "specularExponent", "spreadMethod", "startOffset", "stdDeviation",
        "stitchTiles", "surfaceScale", "systemLanguage", "tableValues", "targetX",
        "targetY", "textLength", "viewBox", "viewTarget", "xChannelSelector",
        "yChannelSelector", "zoomAndPan", NULL
    };
    if (!lower) return lower;
    bool has_upper_form = false;
    for (const char *p = lower; *p; p++) if (*p >= 'a' && *p <= 'z') { has_upper_form = true; break; }
    if (!has_upper_form) return lower;
    for (int i = 0; svg_names[i]; i++)
        if (strcasecmp(svg_names[i], lower) == 0) return svg_names[i];
    return lower;
}

/* Incremental render snapshot.  Each snapshot keeps its buffer; an element
 * whose subtree read nothing the store does not track records where its
 * fragment (text and element ids) lies in it.  The next snapshot copies
 * the fragment of an element with no dirty bit (on it or below: the store
 * marks every mutation, and style object writes mark their element) and
 * re-bases the records inside it.  What the dirty bits miss (properties
 * that live only on JS objects) the periodic full snapshot picks up. */
typedef struct SnapFrag {
    NodeId id;
    uint32_t gen;
    uint32_t t0, t1;            /* text range in the snapshot buffer */
    uint32_t i0, i1;            /* element id range */
    /* The element's start tag alone ("<div ...>"), valid in snapshot
     * open_gen when it read nothing untracked; reused while the element has
     * no DIRTY_ATTR (its subtree may have changed). */
    uint32_t open_gen;
    uint32_t o0, o1;
} SnapFrag;

static struct {
    SnapFrag *f;                /* by store slot */
    uint32_t cap;
    uint32_t gen;               /* generation of `prev` */
    SerializeBuffer prev;       /* the last snapshot's buffer */
    uint64_t reused_bytes, reused_elems;
} g_snap;

static SnapFrag *snap_frag(NodeId id, bool grow) {
    uint32_t s = dom_slot(id);
    if (s >= g_snap.cap) {
        if (!grow) return NULL;
        uint32_t nc = g_snap.cap ? g_snap.cap : 4096;
        while (nc <= s) nc *= 2;
        SnapFrag *nf = (SnapFrag *)realloc(g_snap.f, (size_t)nc * sizeof(SnapFrag));
        if (!nf) return NULL;
        memset(nf + g_snap.cap, 0, (size_t)(nc - g_snap.cap) * sizeof(SnapFrag));
        g_snap.f = nf;
        g_snap.cap = nc;
    }
    return &g_snap.f[s];
}


/* Copy `id`'s clean fragment of the previous snapshot into buf. */
static bool snap_try_reuse(SerializeBuffer *buf, NodeId id) {
    if (!buf->reuse || !buf->gen || !g_snap.gen || (dom_flags(id) & DOMF_DIRTY_MASK)) return false;
    SnapFrag *e = snap_frag(id, false);
    if (!e || e->id != id || e->gen != g_snap.gen) return false;
    const SerializeBuffer *p = &g_snap.prev;
    if (e->t1 > p->len || e->i1 > p->nids || e->i0 >= e->i1 || p->ids[e->i0] != id) return false;
    size_t nt = e->t1 - e->t0, ni = e->i1 - e->i0;
    if (!serialize_buf_grow(buf, nt)) return false;
    if (buf->nids + ni > buf->capids) {
        size_t nc = buf->capids ? buf->capids : 256;
        while (nc < buf->nids + ni) nc *= 2;
        uint32_t *nids = realloc(buf->ids, nc * sizeof(uint32_t));
        if (!nids) return false;
        buf->ids = nids;
        const char **ntags = realloc(buf->tags, nc * sizeof(char *));
        if (!ntags) return false;
        buf->tags = ntags;
        buf->capids = nc;
    }
    int64_t dt = (int64_t)buf->len - (int64_t)e->t0, di = (int64_t)buf->nids - (int64_t)e->i0;
    uint32_t t0 = e->t0, t1 = e->t1;
    memcpy(buf->data + buf->len, p->data + e->t0, nt);
    buf->len += nt;
    buf->data[buf->len] = '\0';
    memcpy(buf->ids + buf->nids, p->ids + e->i0, ni * sizeof(uint32_t));
    memcpy(buf->tags + buf->nids, p->tags + e->i0, ni * sizeof(char *));
    /* Re-base the records of the elements inside (and of `id` itself). */
    for (size_t k = buf->nids; k < buf->nids + ni; k++) {
        NodeId c = buf->ids[k];
        SnapFrag *ce = c ? snap_frag(c, false) : NULL;
        if (!ce || ce->id != c || ce->gen != g_snap.gen || ce->t0 < t0 || ce->t1 > t1) continue;
        ce->t0 = (uint32_t)(ce->t0 + dt);
        ce->t1 = (uint32_t)(ce->t1 + dt);
        ce->i0 = (uint32_t)(ce->i0 + di);
        ce->i1 = (uint32_t)(ce->i1 + di);
        ce->gen = buf->gen;
        if (ce->open_gen == g_snap.gen && ce->o0 >= t0 && ce->o1 <= t1) {
            ce->o0 = (uint32_t)(ce->o0 + dt);
            ce->o1 = (uint32_t)(ce->o1 + dt);
            ce->open_gen = buf->gen;
        }
    }
    buf->nids += ni;
    g_snap.reused_bytes += nt;
    g_snap.reused_elems += ni;
    return true;
}

/* Copy `id`'s start tag of the previous snapshot (attributes and style
 * unchanged since). */
static bool snap_try_reuse_open(SerializeBuffer *buf, NodeId id) {
    if (!buf->reuse || !buf->gen || !g_snap.gen || (dom_flags(id) & DOMF_DIRTY_ATTR)) return false;
    SnapFrag *e = snap_frag(id, false);
    if (!e || e->id != id || e->open_gen != g_snap.gen || e->o1 > g_snap.prev.len || e->o0 >= e->o1) return false;
    return serialize_buf_append_raw(buf, g_snap.prev.data + e->o0, e->o1 - e->o0);
}

static void ser_buf_attr_sink(void *ud, const char *name, const char *value) {
    serialize_buf_append_attr((SerializeBuffer *)ud, name, value);
}

/* The attributes an element renders with, in order, through sink: its
 * store (or script-object) attributes, the style attribute merged with its
 * style object, then attributes scripts set only as JS properties.  Shared
 * by the serializer and the layout mirror. */
typedef void (*SerAttrSink)(void *ud, const char *name, const char *value);
static void ser_element_attrs(JSContextHandle ctx, SerNode n, const char *tag_lower, LooseAttrs *loose_in,
                              SerAttrSink sink, void *ud) {
    /* Attributes; the style attribute merges with the style object below. */
    const char *attr_style = NULL;
    char *attr_style_copy = NULL;
    if (n.id) {
        int na = dom_attr_count(n.id);
        /* The legacy DOM listed a parsed element's id and class first. */
        int *order = malloc((size_t)(na ? na : 1) * sizeof(int)), no = 0;
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < na; i++) {
                const char *nm = css_atom_str(dom_attr_at(n.id, i)->name);
                bool ic = legacy_limits() && (strcmp(nm, "id") == 0 || strcmp(nm, "class") == 0);
                if (ic == (pass == 0)) order[no++] = i;
            }
        for (int oi = 0; oi < no; oi++) {
            int i = order[oi];
            const DomAttr *a = dom_attr_at(n.id, i);
            static char ntmp[64], vtmp[2048];
            const char *name = legacy_cut(dom_attr_name_for_output(css_atom_str(a->name)), 64, ntmp);
            const char *value = legacy_cut(a->value.s ? a->value.s : "", 2048, vtmp);
            if (!name || !name[0]) continue;
            if (strcmp(name, "style") == 0) { free(attr_style_copy); attr_style = attr_style_copy = strdup(value); continue; }
            sink(ud, name, value);
        }
        free(order);
    } else {
        for (int i = 0; i < loose_in->n; i++) {
            if (strcasecmp(loose_in->names[i], "style") == 0) { attr_style = loose_in->values[i]; continue; }
            sink(ud, loose_in->names[i], loose_in->values[i]);
        }
    }

    {
        SerializeBuffer style_buf;
        serialize_buf_init(&style_buf);
        if (attr_style && attr_style[0]) {
            serialize_buf_append(&style_buf, attr_style);
            size_t sl = strlen(attr_style);
            if (sl > 0 && attr_style[sl - 1] != ';')
                serialize_buf_append(&style_buf, ";");
        }
        GCValue style_obj = JS_UNDEFINED;
        if (JS_IsObject(n.obj))
            style_obj = n.id ? dom_element_style(ctx, n.obj, false)
                             : JS_GetPropertyStr(ctx, n.obj, "__style");
        if (JS_IsObject(style_obj)) append_style_object(ctx, style_obj, &style_buf);
        else if (n.id) append_emulated_style_object(attr_style, &style_buf);
        if (style_buf.data && style_buf.len > 0)
            sink(ud, "style", style_buf.data);
        serialize_buf_free(&style_buf);
    }
    free(attr_style_copy);

    /* Attributes set as JS properties only (scripts assigning el.src etc.). */
    if (JS_IsObject(n.obj)) {
        static const struct { const char *prop; const char *attr; } extra_attrs[] = {
            { "id", "id" }, { "className", "class" }, { "class", "class" },
            { "src", "src" }, { "href", "href" }, { "style", "style" },
            { "alt", "alt" }, { "title", "title" }, { "width", "width" },
            { "height", "height" }, { "type", "type" }, { "value", "value" },
            { "name", "name" }, { "placeholder", "placeholder" }, { NULL, NULL }
        };
        /* On a store element every accessor that matters reflects an
         * attribute (checked first), except on the elements with native
         * state; elsewhere only own data properties can add anything, and
         * reading those skips the getters on the prototype chain. */
        bool own_only = n.id && !ser_has_native_props(tag_lower);
        static HtmlAtom extra_atoms[sizeof(extra_attrs) / sizeof(extra_attrs[0])];
        for (int i = 0; extra_attrs[i].prop; i++) {
            bool has;
            if (n.id) {
                has = dom_attr(n.id, extra_attrs[i].attr) != NULL;
            } else {
                has = false;
                for (int k = 0; k < loose_in->n; k++)
                    if (strcasecmp(loose_in->names[k], extra_attrs[i].attr) == 0) { has = true; break; }
            }
            if (has) continue;
            JSAtom pa = html_atom(ctx, &extra_atoms[i], extra_attrs[i].prop);
            char *value = own_only ? js_object_get_own_string_atom(ctx, n.obj, pa)
                                   : js_object_get_string_atom(ctx, n.obj, pa);
            if (value && value[0]) sink(ud, extra_attrs[i].attr, value);
            free(value);
        }
    }
}

static void html_serialize_element_body(JSContextHandle ctx, SerNode n, SerializeBuffer *buf) {
    char tag_lower[64];
    LooseAttrs loose = {0};
    if (!n.id) buf->taint++;
    if (n.id) {
        const char *tag = dom_tag_name(n.id);
        if (!tag || !tag[0]) tag = "div";
        snprintf(tag_lower, sizeof(tag_lower), "%s", tag);
    } else {
        GCValue jnn = JS_GetPropertyStr(ctx, n.obj, "nodeName");
        const char *nn = JS_ToCString(ctx, jnn);
        snprintf(tag_lower, sizeof(tag_lower), "%s", nn && nn[0] ? nn : "#unknown");
        if (nn) JS_FreeCString(ctx, nn);
        loose_attrs_read(ctx, n.obj, &loose);
    }
    for (char *p = tag_lower; *p; p++) *p = (char)tolower((unsigned char)*p);
    bool native = ser_has_native_props(tag_lower);
    if (native) buf->taint++;   /* getters over native state */
    serialize_note_element(buf, n.id, tag_lower);

    size_t open0 = buf->len;
    if (n.id && !native && snap_try_reuse_open(buf, n.id)) {
        loose_attrs_free(&loose);
        goto open_done;
    }
    serialize_buf_append(buf, "<");
    serialize_buf_append(buf, tag_lower);
    ser_element_attrs(ctx, n, tag_lower, &loose, ser_buf_attr_sink, buf);
    loose_attrs_free(&loose);
    serialize_buf_append(buf, ">");

open_done:
    if (n.id && !native && buf->gen) {
        SnapFrag *e = snap_frag(n.id, true);
        if (e) {
            if (e->id != n.id) { memset(e, 0, sizeof(*e)); e->id = n.id; }
            e->open_gen = buf->gen;
            e->o0 = (uint32_t)open0;
            e->o1 = (uint32_t)buf->len;
        }
    }
    if (html_is_self_closing_tag(tag_lower)) return;

    if (buf->omit_script_text && strcmp(tag_lower, "script") == 0) {
        /* Nothing to emit. */
    } else if (html_is_raw_content_tag(tag_lower)) {
        /* Raw text: textContent, unescaped. */
        if (n.id && legacy_limits()) {
            ser_text_content(n.id, buf);
        } else if (JS_IsObject(n.obj)) {
            buf->taint++;
            GCValue tc = JS_GetPropertyStr(ctx, n.obj, "textContent");
            const char *text = JS_ToCString(ctx, tc);
            if (text) { serialize_buf_append(buf, text); JS_FreeCString(ctx, text); }
        } else if (n.id) {
            ser_text_content(n.id, buf);
        }
    } else if (n.id) {
        html_serialize_children(ctx, n, buf);
    }

    serialize_buf_append(buf, "</");
    serialize_buf_append(buf, tag_lower);
    serialize_buf_append(buf, ">");
}

static void html_serialize_element_body(JSContextHandle ctx, SerNode n, SerializeBuffer *buf);

static void html_serialize_element(JSContextHandle ctx, SerNode n, SerializeBuffer *buf) {
    if (n.id && snap_try_reuse(buf, n.id)) return;
    size_t t0 = buf->len, i0 = buf->nids;
    uint32_t taint0 = buf->taint;
    html_serialize_element_body(ctx, n, buf);
    if (!buf->gen || !n.id) return;
    SnapFrag *e = snap_frag(n.id, true);
    if (!e) return;
    e->id = n.id;
    /* Recorded only when nothing below read untracked state (and the
     * offsets fit the record). */
    e->gen = buf->taint == taint0 && buf->len <= UINT32_MAX && buf->nids <= UINT32_MAX ? buf->gen : 0;
    e->t0 = (uint32_t)t0;
    e->t1 = (uint32_t)buf->len;
    e->i0 = (uint32_t)i0;
    e->i1 = (uint32_t)buf->nids;
}

static void html_serialize_text(const char *val, size_t len, SerializeBuffer *buf) {
    len = strnlen(val, len);
    if (legacy_limits() && len > 4095) len = 4095;
    /* Skip stray closing tags emitted after the document end. */
    const char *p = val, *end = val + len;
    while (p < end && isspace((unsigned char)*p)) p++;
    size_t rest = (size_t)(end - p);
    if (rest >= 7 && (strncmp(p, "</body>", 7) == 0 || strncmp(p, "</html>", 7) == 0)) return;
    /* Skip text that is just an HTML comment marker (Polymer css-build). */
    size_t tl = rest;
    while (tl > 0 && isspace((unsigned char)p[tl - 1])) tl--;
    if (tl >= 7 && strncmp(p, "<!--", 4) == 0 && strncmp(p + tl - 3, "-->", 3) == 0) return;
    serialize_buf_append_escaped_n(buf, val, len);
}

static void html_serialize_node(JSContextHandle ctx, SerNode n, SerializeBuffer *buf) {
    if (!n.id && !JS_IsObject(n.obj)) return;
    int nt = ser_node_type(ctx, n);
    if (nt == 3) {
        if (n.id) {
            uint32_t len = 0;
            const char *s = dom_text(n.id, &len);
            html_serialize_text(s, len, buf);
        } else {
            buf->taint++;
            GCValue tc = JS_GetPropertyStr(ctx, n.obj, "textContent");
            size_t len = 0;
            const char *tcs = JS_ToCStringLen(ctx, &len, tc);
            if (tcs) { html_serialize_text(tcs, len, buf); JS_FreeCString(ctx, tcs); }
        }
        return;
    }
    if (nt == 1) html_serialize_element(ctx, n, buf);
    /* Comments, documents and fragments are not emitted. */
}

/* Serialize a JS DOM node (and its descendants) to a heap-allocated HTML
 * string. The caller must free the returned pointer. */
char *html_serialize_js_node(JSContextHandle ctx, GCValue node) {
    SerializeBuffer buf;
    if (!serialize_buf_init(&buf)) return NULL;
    if (JS_IsString(node)) {
        const char *s = JS_ToCString(ctx, node);
        if (s) { serialize_buf_append_escaped(&buf, s); JS_FreeCString(ctx, s); }
    } else {
        html_serialize_node(ctx, ser_of_value(node), &buf);
    }
    char *result = buf.data;
    buf.data = NULL;
    serialize_buf_free(&buf);
    return result;
}

/* Give the reparsed elements the store ids they were serialized from.  The
 * parse reproduces the serialized element sequence except where the parser
 * drops or re-nests elements, so a short lookahead resynchronises. */
static void map_store_ids(HtmlDocument *doc, SerializeBuffer *buf) {
    size_t pos = 0;
    size_t n = po_array_count(&doc->array);
    for (size_t i = 0; i < n && pos < buf->nids; i++) {
        if (!po_array_is_active(&doc->array, (int)i)) continue;
        HtmlNode *hn = po_array_payload(&doc->array, (int)i);
        if (!hn || hn->type != HTML_NODE_ELEMENT) continue;
        hn->store_id = 0;
        for (size_t k = pos; k < buf->nids && k < pos + 8; k++) {
            if (strcmp(buf->tags[k], hn->tag_name) == 0) {
                hn->store_id = buf->ids[k];
                pos = k + 1;
                break;
            }
        }
    }
}

static GCValue render_root_element(JSContextHandle ctx, GCValue js_doc) {
    GCValue doc_elem = JS_NULL;
    if (!JS_IsUndefined(js_doc) && !JS_IsNull(js_doc))
        doc_elem = JS_GetPropertyStr(ctx, js_doc, "documentElement");
    if (JS_IsUndefined(doc_elem) || JS_IsNull(doc_elem)) {
        GCValue global = JS_GetGlobalObject(ctx);
        doc_elem = JS_GetPropertyStr(ctx, global, "documentElement");
    }
    return doc_elem;
}

/* Snapshot text hash, eight bytes a step (the text runs to megabytes). */
static uint64_t fnv1a64(const char *p, size_t n) {
    uint64_t h = 1469598103934665603ULL ^ (n * 0x9E3779B97F4A7C15ULL);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ULL;
        h ^= h >> 29;
    }
    for (; i < n; i++) { h ^= (unsigned char)p[i]; h *= 1099511628211ULL; }
    return h | 1;   /* never 0: 0 means "no snapshot yet" */
}

static uint64_t g_render_snapshots, g_render_reuses;

void html_render_stats(uint64_t *snapshots, uint64_t *reuses) {
    if (snapshots) *snapshots = g_render_snapshots;
    if (reuses) *reuses = g_render_reuses;
}

/* Build the render snapshot.  With `key`: when the document serializes to
 * the same text as the snapshot `*key` names, nothing is rebuilt (returns
 * NULL with *unchanged set); otherwise *key names the new snapshot.  Taking a
 * snapshot clears the store's dirty bits for the document (§7). */
static bool snapshot_serialize(JSContextHandle ctx, GCValue doc_elem, SerializeBuffer *buf,
                               uint32_t gen, bool reuse) {
    if (!serialize_buf_init(buf)) return false;
    buf->track = true;
    buf->omit_script_text = true;
    buf->gen = gen;
    buf->reuse = reuse;
    /* CYBER_SNAPSHOT_SHADY=0 composes ShadyDOM roots through JS instead. */
    static int shady_env = -1;
    if (shady_env < 0) {
        const char *e = cyber_getenv("CYBER_SNAPSHOT_SHADY");
        shady_env = !(e && e[0] == '0');
    }
    if (shady_env) {
        GCValue sd = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "ShadyDOM");
        buf->shady = JS_IsObject(sd) && JS_ToBool(ctx, JS_GetPropertyStr(ctx, sd, "inUse"));
    }
    serialize_buf_append(buf, "<!doctype html>\n");
    html_serialize_node(ctx, ser_of_value(doc_elem), buf);
    return true;
}

HtmlDocument *html_mirror_document(JSContextHandle ctx, GCValue js_doc, bool full,
                                   uint64_t *version, bool *changed);

static HtmlDocument *snapshot_build(JSContextHandle ctx, GCValue js_doc, uint64_t *key, bool *unchanged,
                                    bool reuse) {
    if (unchanged) *unchanged = false;
    if (!SAFE_VALID(ctx)) return NULL;
    /* The layout mirror, kept up to date in place (a full update re-reads
     * what only JS objects hold, as a non-incremental snapshot did).  The
     * key is its version; each caller holds a reference. */
    {
        uint64_t version = 0;
        bool changed = false;
        HtmlDocument *m = html_mirror_document(ctx, js_doc, !reuse, &version, &changed);
        if (m) {
            if (key) {
                if (*key == version) {
                    if (unchanged) *unchanged = true;
                    g_render_reuses++;
                    return NULL;
                }
                *key = version;
            }
            m->refs++;
            g_render_snapshots++;
            return m;
        }
    }
    GCValue doc_elem = render_root_element(ctx, js_doc);
    if (JS_IsUndefined(doc_elem) || JS_IsNull(doc_elem)) return NULL;

    static uint32_t s_gen;
    if (++s_gen == 0) s_gen = 1;
    SerializeBuffer buf;
    bool ok;
    /* Getters the serializer calls can run page code that mutates the DOM
     * (polyfills flush on access); the dirty bits of those mutations would
     * be cleared below, so such a snapshot leaves nothing to reuse. */
    uint64_t dirty0 = g_dom.dirty_version;
    if (reuse) {
        CP_ACC("snapshot: serialize (incremental)");
        ok = snapshot_serialize(ctx, doc_elem, &buf, s_gen, reuse);
    } else {
        CP_ACC("snapshot: serialize (full)");
        ok = snapshot_serialize(ctx, doc_elem, &buf, s_gen, reuse);
    }
    if (!ok) return NULL;
    bool mutated = g_dom.dirty_version != dirty0;
    static int verify_log = -1;
    if (verify_log < 0) verify_log = cyber_getenv("CYBER_SNAPSHOT_VERIFY") ? 1 : 0;
    if (mutated && verify_log) fprintf(stderr, "[snapshot-verify] DOM mutated during serialization\n");
    /* CYBER_SNAPSHOT_VERIFY=1: an incremental snapshot must equal a full one. */
    static int verify = -1;
    if (verify < 0) verify = cyber_getenv("CYBER_SNAPSHOT_VERIFY") ? 1 : 0;
    if (verify && reuse) {
        SerializeBuffer full;
        if (snapshot_serialize(ctx, doc_elem, &full, 0, false)) {
            bool same = full.len == buf.len && memcmp(full.data, buf.data, buf.len) == 0 &&
                        full.nids == buf.nids && memcmp(full.ids, buf.ids, buf.nids * sizeof(uint32_t)) == 0;
            static unsigned long checks, mismatches;
            checks++;
            if (!same) {
                size_t i = 0;
                while (i < full.len && i < buf.len && full.data[i] == buf.data[i]) i++;
                size_t from = i > 120 ? i - 120 : 0;
                fprintf(stderr, "[snapshot-verify] mismatch %lu/%lu at byte %zu (full %zu, incremental %zu bytes)\n"
                                "  full: %.240s\n  incr: %.240s\n",
                        ++mismatches, checks, i, full.len, buf.len, full.data + from,
                        from < buf.len ? buf.data + from : "");
                size_t k = 0;
                while (k < full.nids && k < buf.nids && full.ids[k] == buf.ids[k]) k++;
                if (k < full.nids || k < buf.nids)
                    fprintf(stderr, "  ids differ at element %zu of %zu/%zu: full %u <%s> (kind %d flags %x), incr %u <%s> (kind %d flags %x)\n",
                            k, full.nids, buf.nids, k < full.nids ? full.ids[k] : 0, k < full.nids ? full.tags[k] : "-",
                            k < full.nids ? dom_kind(full.ids[k]) : -1, k < full.nids ? dom_flags(full.ids[k]) : 0,
                            k < buf.nids ? buf.ids[k] : 0, k < buf.nids ? buf.tags[k] : "-",
                            k < buf.nids ? dom_kind(buf.ids[k]) : -1, k < buf.nids ? dom_flags(buf.ids[k]) : 0);
                /* Keep the page correct: use the full text. */
                serialize_buf_free(&buf);
                buf = full;
                buf.gen = 0;
            } else {
                serialize_buf_free(&full);
            }
            if (checks % 100 == 0)
                fprintf(stderr, "[snapshot-verify] %lu checks, %lu mismatches\n", checks, mismatches);
        }
    }

    NodeId dn = dom_node_of(js_doc);
    if (dom_kind(dn) != DOM_K_DOCUMENT) dn = g_dom_document;
    HtmlDocument *doc = NULL;
    bool same = false;
    if (key) {
        uint64_t k;
        {
            CP_ACC("snapshot: hash");
            k = fnv1a64(buf.data, buf.len);
        }
        if (*key == k) same = true;
        else *key = k;
    }
    if (same) {
        if (unchanged) *unchanged = true;
        g_render_reuses++;
    } else {
        {
            CP_ACC("snapshot: parse");
            doc = html_parse(buf.data, buf.len);
        }
        if (doc) map_store_ids(doc, &buf);
        g_render_snapshots++;
    }
    dom_clear_dirty(dn);
    if (verify_log) {
        /* Reuse relies on the clear reaching every dirty bit in the document. */
        uint32_t r = dom_slot(dn);
        static int reported;
        for (uint32_t s = r; s && reported < 5; s = dom_next_in_subtree(s, r)) {
            if (!(g_dom.flags[s] & DOMF_DIRTY_MASK)) continue;
            reported++;
            fprintf(stderr, "[snapshot-verify] dirty bits %x on <%s> survive the clear\n", g_dom.flags[s],
                    g_dom.kind[s] == DOM_K_ELEMENT ? css_atom_str(g_dom.tag[s]) : "#node");
        }
    }
    /* This snapshot's buffer is the next one's source of fragments. */
    serialize_buf_free(&g_snap.prev);
    g_snap.prev = buf;
    g_snap.gen = mutated ? 0 : buf.gen;
    return doc;
}

HtmlDocument *html_document_from_js_dom_if_changed(JSContextHandle ctx, GCValue js_doc,
                                                   uint64_t *key, bool *unchanged) {
    return snapshot_build(ctx, js_doc, key, unchanged, false);
}

HtmlDocument *html_document_from_js_dom_incremental(JSContextHandle ctx, GCValue js_doc,
                                                    uint64_t *key, bool *unchanged) {
    return snapshot_build(ctx, js_doc, key, unchanged, true);
}

void html_snapshot_stats(uint64_t *reused_bytes, uint64_t *reused_elements) {
    if (reused_bytes) *reused_bytes = g_snap.reused_bytes;
    if (reused_elements) *reused_elements = g_snap.reused_elems;
}

/* Build a fresh native HtmlDocument from the JS document's current DOM. */
HtmlDocument *html_document_from_js_dom(JSContextHandle ctx, GCValue js_doc) {
    return html_document_from_js_dom_if_changed(ctx, js_doc, NULL, NULL);
}

/* An element's style object as CSS text ("width: 1px; ..."), malloc'ed, or
 * NULL when it declares nothing.  Uses the render serializer's conversion
 * (camelCase properties, vendor seeds, ...). */
char *html_style_object_css_text(JSContextHandle ctx, GCValue style_obj) {
    if (!JS_IsObject(style_obj)) return NULL;
    SerializeBuffer b;
    if (!serialize_buf_init(&b)) return NULL;
    append_style_object(ctx, style_obj, &b);
    if (!b.data || b.len == 0) { serialize_buf_free(&b); return NULL; }
    char *out = strdup(b.data);
    serialize_buf_free(&b);
    return out;
}

/* ============================================================================
 * Layout mirror: one HtmlDocument kept in step with the live document
 *
 * Instead of serializing the document to HTML and parsing that back for
 * every layout, the mirror is built from the store once and then updated in
 * place: a subtree with no dirty bit is left as it is, a dirty element gets
 * its attributes (and raw text) read again, and an element whose children
 * changed has its child list reconciled, reusing the mirror nodes of the
 * children that stay (by store id), so indices of unchanged nodes are
 * stable.  It renders what the serializer + parser pair rendered: the same
 * attributes (ser_element_attrs), the same composition of shadow roots, the
 * text filters of html_serialize_text and the parser's merging of adjacent
 * text.  What the dirty bits miss (properties only JS objects hold, as on
 * elements with native state) a full update reads; setters of native state
 * that layout depends on mark their element dirty (video src).  A page whose shadow roots only JS can compose (not
 * ShadyDOM, not store shadow roots) is left to the serializer.
 * CYBER_MIRROR_VERIFY=1 compares each update with serialize + parse.
 * ============================================================================ */

typedef struct MirrorMap {
    NodeId *keys;
    int *vals;
    uint32_t cap, n;
} MirrorMap;

static struct {
    HtmlDocument *doc;
    NodeId root_id;
    bool valid;
    uint64_t version;
    MirrorMap map;              /* store id -> mirror index (validated on use) */
    MirrorMap host;             /* store id -> 1 ShadyDOM host, 2 store shadow host (else 0) */
    /* Per update. */
    JSContextHandle ctx;
    bool shady, full, changed, unsupported;
    bool structure;             /* nodes added, removed or moved: reorder */
    int building;               /* inside a new subtree: no change records */
    uint64_t updates, rebuilds, nodes_built;
    /* Change log (html_mirror_changes): log[i] has position log_base + i;
     * positions before log_reset are unknown. */
    HtmlMirrorChange *log;
    size_t nlog, cap_log;
    uint64_t log_base, log_reset;
    uint64_t sheet_sig;         /* style/link elements and their parents, in order */
    bool sheet_sig_valid;
} g_mir;

#define MIR_LOG_MAX 16384

static void mir_log_reset(void) {
    g_mir.log_base += g_mir.nlog;
    g_mir.nlog = 0;
    g_mir.log_reset = g_mir.log_base;
}

static void mir_log(uint8_t kind, NodeId id, const HtmlAttribute *old_attrs, const HtmlAttribute *new_attrs) {
    if (kind == HTML_MIRROR_SHEETS && g_mir.doc) g_mir.doc->sheet_gen++;   /* even when the log overflows */
    if (g_mir.nlog >= MIR_LOG_MAX) mir_log_reset();
    if (g_mir.nlog == g_mir.cap_log) {
        size_t nc = g_mir.cap_log ? g_mir.cap_log * 2 : 256;
        HtmlMirrorChange *nl = (HtmlMirrorChange *)realloc(g_mir.log, nc * sizeof(*nl));
        if (!nl) { mir_log_reset(); return; }
        g_mir.log = nl;
        g_mir.cap_log = nc;
    }
    HtmlMirrorChange *c = &g_mir.log[g_mir.nlog++];
    c->id = id;
    c->kind = kind;
    c->old_attrs = old_attrs;
    c->new_attrs = new_attrs;
    c->empty_changed = 0;
}

uint64_t html_mirror_change_seq(void) { return g_mir.log_base + g_mir.nlog; }

bool html_mirror_changes(const HtmlDocument *doc, uint64_t since, const HtmlMirrorChange **out, size_t *n) {
    *out = NULL;
    *n = 0;
    if (!doc || doc != g_mir.doc || !g_mir.valid) return false;
    if (since < g_mir.log_reset || since < g_mir.log_base || since > g_mir.log_base + g_mir.nlog) return false;
    *out = g_mir.log + (since - g_mir.log_base);
    *n = (size_t)(g_mir.log_base + g_mir.nlog - since);
    return true;
}

static bool mir_is_sheet_tag(const char *tag) {
    return tag && (strcmp(tag, "style") == 0 || strcmp(tag, "link") == 0);
}

static void mir_map_put_in(MirrorMap *m, NodeId id, int idx) {
    if (2 * (m->n + 1) > m->cap) {
        uint32_t nc = m->cap ? m->cap * 2 : 16384;
        NodeId *nk = (NodeId *)calloc(nc, sizeof(NodeId));
        int *nv = (int *)malloc(nc * sizeof(int));
        if (!nk || !nv) { free(nk); free(nv); return; }
        for (uint32_t i = 0; i < m->cap; i++) {
            if (!m->keys[i]) continue;
            uint32_t h = (m->keys[i] * 2654435761u) & (nc - 1);
            while (nk[h]) h = (h + 1) & (nc - 1);
            nk[h] = m->keys[i];
            nv[h] = m->vals[i];
        }
        free(m->keys);
        free(m->vals);
        m->keys = nk;
        m->vals = nv;
        m->cap = nc;
    }
    uint32_t h = (id * 2654435761u) & (m->cap - 1);
    while (m->keys[h] && m->keys[h] != id) h = (h + 1) & (m->cap - 1);
    if (!m->keys[h]) m->n++;
    m->keys[h] = id;
    m->vals[h] = idx;
}

static void mir_map_put(NodeId id, int idx) { mir_map_put_in(&g_mir.map, id, idx); }

static int mir_map_raw(const MirrorMap *m, NodeId id) {
    if (!id || !m->cap) return -1;
    uint32_t h = (id * 2654435761u) & (m->cap - 1);
    while (m->keys[h]) {
        if (m->keys[h] == id) return m->vals[h];
        h = (h + 1) & (m->cap - 1);
    }
    return -1;
}

/* The live mirror node of store node id, or -1. */
static int mir_map_get(NodeId id) {
    MirrorMap *m = &g_mir.map;
    if (!id || !m->cap) return -1;
    uint32_t h = (id * 2654435761u) & (m->cap - 1);
    while (m->keys[h]) {
        if (m->keys[h] == id) {
            int idx = m->vals[h];
            HtmlDocument *d = g_mir.doc;
            if (idx < 0 || (size_t)idx >= po_array_count(&d->array) || !po_array_is_active(&d->array, idx))
                return -1;
            HtmlNode *hn = po_array_payload(&d->array, idx);
            return hn && hn->store_id == id ? idx : -1;
        }
        h = (h + 1) & (m->cap - 1);
    }
    return -1;
}

int html_mirror_index_of(const HtmlDocument *doc, uint32_t id) {
    if (!doc || doc != g_mir.doc || !g_mir.valid) return -1;
    return mir_map_get(id);
}

static void mir_map_clear(MirrorMap *m) {
    if (m->keys) memset(m->keys, 0, m->cap * sizeof(NodeId));
    m->n = 0;
}

static void mir_map_reset(void) {
    mir_map_clear(&g_mir.map);
    mir_map_clear(&g_mir.host);
}

static void mir_set_host(NodeId s, unsigned char kind) {
    if (kind || mir_map_raw(&g_mir.host, s) > 0) mir_map_put_in(&g_mir.host, s, kind);
}

static unsigned char mir_host(NodeId s) {
    int k = mir_map_raw(&g_mir.host, s);
    return k > 0 ? (unsigned char)k : 0;
}

static const char *mir_intern(const char *s, size_t n) {
    css_atom_reserve(1);
    return css_atom_str(css_atom_intern(s, n));
}

/* ---- Attributes ---- */

typedef struct MirAttrs {
    const char *names[64];
    char *values[64];
    int n;
    bool in_svg;
} MirAttrs;

static void mir_attr_sink(void *ud, const char *name, const char *value) {
    MirAttrs *a = (MirAttrs *)ud;
    if (!name || !name[0] || a->n == 64) return;
    /* As the parser reads it back: lower case, SVG spellings in SVG. */
    char lower[128];
    size_t k = 0;
    for (; name[k] && k < sizeof(lower) - 1; k++) lower[k] = (char)tolower((unsigned char)name[k]);
    lower[k] = '\0';
    const char *nm = a->in_svg ? dom_attr_name_for_output(lower) : lower;
    const char *in = mir_intern(nm, strlen(nm));
    for (int i = 0; i < a->n; i++) if (a->names[i] == in) return;   /* first one wins */
    a->names[a->n] = in;
    a->values[a->n] = strdup(value ? value : "");
    a->n++;
}

static void mir_attrs_free(MirAttrs *a) {
    for (int i = 0; i < a->n; i++) free(a->values[i]);
    a->n = 0;
}

/* Read element s's attributes into mirror node idx; true when they changed. */
static bool mir_read_attrs(NodeId s, int idx, const char *tag_lower, bool in_svg) {
    const HtmlAttribute *old_list = NULL;
    MirAttrs a;
    a.n = 0;
    a.in_svg = in_svg;
    LooseAttrs loose = {0};
    ser_element_attrs(g_mir.ctx, ser_of_node(s), tag_lower, &loose, mir_attr_sink, &a);
    HtmlNode *hn = po_array_payload(&g_mir.doc->array, idx);
    bool same = true;
    int k = 0;
    for (HtmlAttribute *at = hn->attributes; at; at = at->next, k++) {
        if (k >= a.n || at->name != a.names[k] || strcmp(at->value, a.values[k]) != 0) { same = false; break; }
    }
    if (same && k != a.n) same = false;
    if (!same) {
        old_list = hn->attributes;
        hn->attributes = NULL;      /* the old list stays in the arena */
        html_document_attrs_changed(g_mir.doc, idx);
        for (int i = 0; i < a.n; i++)
            html_document_add_attr(g_mir.doc, idx, a.names[i], a.values[i], strlen(a.values[i]));
        if (!g_mir.building) {
            hn = po_array_payload(&g_mir.doc->array, idx);
            mir_log(HTML_MIRROR_ATTRS, s, old_list, hn->attributes);
            if (mir_is_sheet_tag(tag_lower)) mir_log(HTML_MIRROR_SHEETS, s, NULL, NULL);
        }
    }
    mir_attrs_free(&a);
    return !same;
}

/* ---- Text ---- */

/* The part of a text node html_serialize_text writes (none: false). */
static bool mir_text_kept(const char *val, size_t len, size_t *out_len) {
    len = strnlen(val, len);
    if (legacy_limits() && len > 4095) len = 4095;
    const char *p = val, *end = val + len;
    while (p < end && isspace((unsigned char)*p)) p++;
    size_t rest = (size_t)(end - p);
    if (rest >= 7 && (strncmp(p, "</body>", 7) == 0 || strncmp(p, "</html>", 7) == 0)) return false;
    size_t tl = rest;
    while (tl > 0 && isspace((unsigned char)p[tl - 1])) tl--;
    if (tl >= 7 && strncmp(p, "<!--", 4) == 0 && strncmp(p + tl - 3, "-->", 3) == 0) return false;
    *out_len = len;
    return len > 0;
}

typedef struct MirBuf { char *s; size_t n, cap; } MirBuf;

static void mir_buf_add(MirBuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->n + n + 1) nc *= 2;
        char *ns = (char *)realloc(b->s, nc);
        if (!ns) return;
        b->s = ns;
        b->cap = nc;
    }
    /* The tokenizer turns CR LF and lone CR into LF. */
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\r') {
            if (i + 1 < n && s[i + 1] == '\n') continue;
            c = '\n';
        }
        b->s[b->n++] = c;
    }
    b->s[b->n] = '\0';
}

/* Raw-text element content: the text of its subtree. */
static char *mir_raw_text(NodeId s, size_t *len_out) {
    MirBuf b = {0};
    uint32_t r = dom_slot(s);
    for (uint32_t x = r; x; x = dom_next_in_subtree(x, r)) {
        if (g_dom.kind[x] != DOM_K_TEXT) continue;
        uint32_t tl = 0;
        const char *t = dom_text(dom_id_of(x), &tl);
        size_t n = strnlen(t, tl);
        if (legacy_limits() && n > 4095) n = 4095;
        mir_buf_add(&b, t, n);
    }
    if (!b.s) { b.s = (char *)malloc(1); if (b.s) b.s[0] = '\0'; }
    *len_out = b.n;
    return b.s;
}

/* ---- Composition ---- */

typedef struct NodeList { NodeId *v; int n, cap; } NodeList;

static void nl_push(NodeList *l, NodeId id) {
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        NodeId *nv = (NodeId *)realloc(l->v, (size_t)nc * sizeof(NodeId));
        if (!nv) return;
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n++] = id;
}

static bool mir_is_tag(NodeId id, const char *tag) {
    return dom_kind(id) == DOM_K_ELEMENT && strcmp(dom_tag_name(id), tag) == 0;
}

/* Is there an element or non-blank text among store node root's children? */
static bool mir_has_content(NodeId root) {
    for (NodeId c = dom_first_child(root); c; c = dom_next_sibling(c)) {
        if (dom_kind(c) == DOM_K_ELEMENT) return true;
        if (dom_kind(c) == DOM_K_TEXT) {
            uint32_t tl = 0;
            const char *t = dom_text(c, &tl);
            for (uint32_t i = 0; i < tl; i++) if (!isspace((unsigned char)t[i])) return true;
        }
    }
    return false;
}

/* The nodes element s renders as children, in order (as the serializer
 * composes them); sets the host kind of mirror node m. */
static void mir_compose(NodeId s, int m, NodeList *out) {
    SerNode sn = ser_of_node(s);
    unsigned char kind = 0;
    GCValue shadow = JS_NULL;
    if (JS_IsObject(sn.obj)) {
        shadow = JS_GetPropertyStr(g_mir.ctx, sn.obj, "shadowRoot");
        if (JS_IsNull(shadow) || JS_IsUndefined(shadow))
            shadow = JS_GetPropertyStr(g_mir.ctx, sn.obj, "__CE_shadowRoot");
    }
    if (JS_IsObject(shadow)) {
        NodeId sid = dom_node_of(shadow);
        bool store_shadow = sid && g_dom.shadow[dom_slot(s)] == dom_slot(sid);
        if (!store_shadow) {
            if (!g_mir.shady) { g_mir.unsupported = true; return; }
            /* ShadyDOM: the real children, top-level <slot>s unwrapped. */
            kind = 1;
            for (NodeId c = dom_first_child(s); c; c = dom_next_sibling(c)) {
                if (mir_is_tag(c, "slot")) {
                    for (NodeId g = dom_first_child(c); g; g = dom_next_sibling(g)) nl_push(out, g);
                } else {
                    nl_push(out, c);
                }
            }
            mir_set_host(s, kind);
            return;
        }
        kind = 2;
        if (mir_has_content(sid)) {
            /* Light children distributed to the shadow tree's slots. */
            NodeList light = {0};
            for (NodeId c = dom_first_child(s); c; c = dom_next_sibling(c)) nl_push(&light, c);
            unsigned char *used = (unsigned char *)calloc((size_t)light.n + 1, 1);
            for (NodeId c = dom_first_child(sid); c; c = dom_next_sibling(c)) {
                if (!mir_is_tag(c, "slot")) { nl_push(out, c); continue; }
                const char *name = dom_attr(c, "name");
                char slot_name[256];
                snprintf(slot_name, sizeof(slot_name), "%s", name ? name : "");
                bool any = false;
                for (int i = 0; i < light.n; i++) {
                    if (used && used[i]) continue;
                    const char *ls = dom_kind(light.v[i]) == DOM_K_ELEMENT ? dom_attr(light.v[i], "slot") : NULL;
                    if (strcmp(ls ? ls : "", slot_name) != 0) continue;
                    nl_push(out, light.v[i]);
                    if (used) used[i] = 1;
                    any = true;
                }
                if (!any)
                    for (NodeId g = dom_first_child(c); g; g = dom_next_sibling(g)) nl_push(out, g);
            }
            free(used);
            free(light.v);
            mir_set_host(s, kind);
            return;
        }
    }
    mir_set_host(s, kind);
    (void)m;
    for (NodeId c = dom_first_child(s); c; c = dom_next_sibling(c)) nl_push(out, c);
}

/* ---- Building and updating ---- */

static void mir_reconcile(NodeId s, int m, bool child_svg);

static void mir_tag_lower(NodeId s, char out[64]) {
    const char *tag = dom_tag_name(s);
    if (!tag || !tag[0]) tag = "div";
    snprintf(out, 64, "%s", tag);
    for (char *p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
}

static bool mir_child_svg(bool in_svg, const char *tag_lower) {
    if (in_svg) return strcmp(tag_lower, "foreignobject") != 0;
    return strcmp(tag_lower, "svg") == 0;
}

/* Element s as a new mirror node under parent (with its subtree). */
static int mir_build_inner(NodeId s, int idx, const char *tag_lower, bool in_svg);

static int mir_build_element(NodeId s, int parent, bool in_svg) {
    char tag_lower[64];
    mir_tag_lower(s, tag_lower);
    int idx = html_document_add(g_mir.doc, parent, HTML_NODE_ELEMENT);
    if (idx < 0) { g_mir.unsupported = true; return -1; }
    HtmlNode *hn = po_array_payload(&g_mir.doc->array, idx);
    const char *tag = dom_tag_name(s);
    hn->tag_name = in_svg && tag && tag[0] ? mir_intern(tag, strlen(tag)) : mir_intern(tag_lower, strlen(tag_lower));
    hn->store_id = s;
    mir_map_put(s, idx);
    g_mir.nodes_built++;
    g_mir.building++;
    idx = mir_build_inner(s, idx, tag_lower, in_svg);
    g_mir.building--;
    return idx;
}

static int mir_build_inner(NodeId s, int idx, const char *tag_lower, bool in_svg) {
    HtmlNode *hn;
    mir_read_attrs(s, idx, tag_lower, in_svg);
    if (html_is_self_closing_tag(tag_lower)) return idx;
    if (html_is_raw_content_tag(tag_lower)) {
        hn = po_array_payload(&g_mir.doc->array, idx);
        if (strcmp(tag_lower, "script") == 0) {
            hn->text_content = strdup("");
            hn->text_len = 0;
        } else {
            hn->text_content = mir_raw_text(s, &hn->text_len);
        }
        return idx;
    }
    mir_reconcile(s, idx, mir_child_svg(in_svg, tag_lower));
    return idx;
}

static void mir_update_element(NodeId s, int m, bool in_svg);

/* Make mirror node m's children the rendering of s's children. */
static void mir_reconcile(NodeId s, int m, bool child_svg) {
    HtmlDocument *d = g_mir.doc;
    NodeList items = {0};
    mir_compose(s, m, &items);
    if (g_mir.unsupported) { free(items.v); return; }
    /* Detach the current children; the ones that stay are linked again. */
    int old_local[64];
    int *old = old_local, nold = 0, cap_old = 64;
    for (int c = po_array_first_child(&d->array, m); c >= 0; ) {
        int next = po_array_next_sibling(&d->array, c);
        if (nold == cap_old) {
            int nc = cap_old * 2;
            int *no = (int *)malloc((size_t)nc * sizeof(int));
            if (!no) break;
            memcpy(no, old, (size_t)nold * sizeof(int));
            if (old != old_local) free(old);
            old = no;
            cap_old = nc;
        }
        old[nold++] = c;
        po_array_detach(&d->array, c);
        c = next;
    }
    int pos = 0;            /* index into old[] while the order is unchanged */
    bool reordered = false;
    bool here = false;      /* a text run here changed */
    MirBuf run = {0};
    NodeId run_first = DOM_NULL;
    for (int i = 0; i <= items.n; i++) {
        NodeId c = i < items.n ? items.v[i] : DOM_NULL;
        int k = c ? dom_kind(c) : -1;
        if (k == DOM_K_TEXT) {
            uint32_t tl = 0;
            const char *t = dom_text(c, &tl);
            size_t keep = 0;
            if (mir_text_kept(t, tl, &keep)) {
                if (!run_first) run_first = c;
                mir_buf_add(&run, t, keep);
            }
            continue;
        }
        if (k != DOM_K_ELEMENT && c) continue;     /* comments and the like render nothing */
        /* End of a text run: one text node, as the parser merges them. */
        if (run_first && run.n > 0) {
            int ti = mir_map_get(run_first);
            HtmlNode *tn = ti >= 0 ? po_array_payload(&d->array, ti) : NULL;
            if (tn && tn->type == HTML_NODE_TEXT && po_array_parent(&d->array, ti) == PO_ARRAY_DETACHED) {
                if (!tn->text_content || tn->text_len != run.n || memcmp(tn->text_content, run.s, run.n) != 0) {
                    free(tn->text_content);
                    tn->text_content = (char *)malloc(run.n + 1);
                    if (tn->text_content) memcpy(tn->text_content, run.s, run.n + 1);
                    tn->text_len = run.n;
                    g_mir.changed = here = true;
                }
                po_array_append(&d->array, m, ti);
            } else {
                ti = html_document_add(d, m, HTML_NODE_TEXT);
                if (ti >= 0) {
                    tn = po_array_payload(&d->array, ti);
                    tn->text_content = (char *)malloc(run.n + 1);
                    if (tn->text_content) memcpy(tn->text_content, run.s, run.n + 1);
                    tn->text_len = run.n;
                    tn->store_id = run_first;
                    mir_map_put(run_first, ti);
                }
                g_mir.changed = g_mir.structure = true;
            }
            if (!reordered && (pos >= nold || old[pos] != ti)) reordered = true;
            pos++;
        }
        run.n = 0;
        if (run.s) run.s[0] = '\0';
        run_first = DOM_NULL;
        if (!c) break;
        /* An element: its node if it has one, else a new subtree. */
        int ci = mir_map_get(c);
        HtmlNode *cn = ci >= 0 ? po_array_payload(&d->array, ci) : NULL;
        if (cn && cn->type == HTML_NODE_ELEMENT) {
            if (po_array_parent(&d->array, ci) != PO_ARRAY_DETACHED) {
                po_array_detach(&d->array, ci);     /* moved here from elsewhere */
                g_mir.changed = g_mir.structure = true;
            }
            po_array_append(&d->array, m, ci);
            mir_update_element(c, ci, child_svg);
        } else {
            ci = mir_build_element(c, m, child_svg);
            g_mir.changed = g_mir.structure = true;
            if (g_mir.unsupported) break;
        }
        if (!reordered && (pos >= nold || old[pos] != ci)) reordered = true;
        pos++;
    }
    if (pos != nold) reordered = true;
    if (reordered) g_mir.changed = g_mir.structure = true;
    if ((reordered || here) && !g_mir.building) {
        mir_log(HTML_MIRROR_CHILDREN, s, NULL, NULL);
        if (g_mir.nlog) g_mir.log[g_mir.nlog - 1].empty_changed = (nold > 0) != (pos > 0);
    }
    /* What was not linked again is gone. */
    for (int i = 0; i < nold; i++)
        if (po_array_is_active(&d->array, old[i]) && po_array_parent(&d->array, old[i]) == PO_ARRAY_DETACHED)
            po_array_delete(&d->array, old[i]);
    if (old != old_local) free(old);
    free(run.s);
    free(items.v);
}

/* Does element s have a direct text child (or, for a host, anything) dirty? */
static bool mir_children_dirty(NodeId s, int m) {
    uint16_t f = dom_flags(s);
    if (f & DOMF_DIRTY_CHILDREN) return true;
    if (!(f & DOMF_DIRTY_DESCENDANT)) return false;
    if (mir_host(s)) return true;
    (void)m;
    for (NodeId c = dom_first_child(s); c; c = dom_next_sibling(c))
        if (dom_kind(c) == DOM_K_TEXT && (dom_flags(c) & DOMF_DIRTY_MASK)) return true;
    return false;
}

static void mir_update_element(NodeId s, int m, bool in_svg) {
    uint16_t f = dom_flags(s);
    if (!(f & DOMF_DIRTY_MASK) && !g_mir.full) return;
    char tag_lower[64];
    mir_tag_lower(s, tag_lower);
    /* (A full update re-reads what only JS objects hold: an element that
     * has no wrapper has nothing but its attributes, which are tracked.) */
    if ((f & DOMF_DIRTY_ATTR) || (g_mir.full && g_dom.wrapper[dom_slot(s)])) {
        if (mir_read_attrs(s, m, tag_lower, in_svg)) g_mir.changed = true;
    }
    if (html_is_self_closing_tag(tag_lower)) return;
    if (html_is_raw_content_tag(tag_lower)) {
        if (strcmp(tag_lower, "script") == 0) return;
        if (!(f & (DOMF_DIRTY_CHILDREN | DOMF_DIRTY_DESCENDANT)) && !g_mir.full) return;
        size_t n = 0;
        char *t = mir_raw_text(s, &n);
        HtmlNode *hn = po_array_payload(&g_mir.doc->array, m);
        if (!hn->text_content || hn->text_len != n || memcmp(hn->text_content, t, n) != 0) {
            free(hn->text_content);
            hn->text_content = t;
            hn->text_len = n;
            g_mir.changed = true;
            mir_log(strcmp(tag_lower, "style") == 0 ? HTML_MIRROR_SHEETS : HTML_MIRROR_CHILDREN, s, NULL, NULL);
        } else {
            free(t);
        }
        return;
    }
    bool child_svg = mir_child_svg(in_svg, tag_lower);
    if (mir_children_dirty(s, m)) {
        mir_reconcile(s, m, child_svg);
        return;
    }
    /* Same children: visit the dirty ones (all, in a full update). */
    HtmlDocument *d = g_mir.doc;
    for (int c = po_array_first_child(&d->array, m); c >= 0 && !g_mir.unsupported;
         c = po_array_next_sibling(&d->array, c)) {
        HtmlNode *cn = po_array_payload(&d->array, c);
        if (!cn || cn->type != HTML_NODE_ELEMENT || !cn->store_id) continue;
        if (!dom_valid(cn->store_id)) { g_mir.unsupported = true; return; }
        mir_update_element(cn->store_id, c, child_svg);
    }
}

static void mir_drop(void) {
    mir_log_reset();
    if (g_mir.doc) html_document_free(g_mir.doc);   /* the mirror's own reference */
    g_mir.doc = NULL;
    g_mir.valid = false;
    mir_map_reset();
}

static bool mir_rebuild(NodeId root) {
    mir_drop();
    g_mir.doc = html_document_new();
    if (!g_mir.doc) return false;
    g_mir.doc->sheets_tracked = true;
    g_mir.doc->refs = 1;
    g_mir.root_id = root;
    g_mir.rebuilds++;
    mir_log_reset();
    g_mir.sheet_sig_valid = false;
    mir_build_element(root, -1, false);
    g_mir.changed = true;
    return !g_mir.unsupported;
}

static int mir_verify(JSContextHandle ctx, GCValue js_doc);

/* Which style and link elements there are, in order, under which parents
 * (sheets are scoped to their host): a SHEETS record when that changed. */
static void mir_note_sheets(void) {
    HtmlDocument *d = g_mir.doc;
    size_t n = po_array_count(&d->array);
    uint64_t sig = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        if (!po_array_is_active(&d->array, (int)i)) continue;
        HtmlNode *hn = po_array_payload(&d->array, (int)i);
        if (!hn || hn->type != HTML_NODE_ELEMENT || !mir_is_sheet_tag(hn->tag_name)) continue;
        int p = po_array_parent(&d->array, (int)i);
        HtmlNode *pn = p >= 0 ? po_array_payload(&d->array, p) : NULL;
        uint64_t v[2] = { hn->store_id, pn ? pn->store_id : 0 };
        for (int k = 0; k < 2; k++) { sig ^= v[k]; sig *= 1099511628211ull; }
    }
    if (g_mir.sheet_sig_valid && sig != g_mir.sheet_sig) mir_log(HTML_MIRROR_SHEETS, 0, NULL, NULL);
    g_mir.sheet_sig = sig;
    g_mir.sheet_sig_valid = true;
}

static void mir_reorder(void) {
    HtmlDocument *d = g_mir.doc;
    size_t n = po_array_count(&d->array);
    for (size_t i = 0; i < n; i++) {
        if (po_array_is_active(&d->array, (int)i)) continue;
        HtmlNode *hn = po_array_payload(&d->array, (int)i);
        if (hn) { free(hn->text_content); hn->text_content = NULL; }
    }
    po_array_reorder(&d->array);
    d->struct_gen++;
    mir_map_clear(&g_mir.map);
    n = po_array_count(&d->array);
    for (size_t i = 0; i < n; i++) {
        HtmlNode *hn = po_array_payload(&d->array, (int)i);
        if (hn && hn->store_id) mir_map_put(hn->store_id, (int)i);
    }
}

/* Bring the mirror up to date; NULL when this page needs the serializer
 * (then the next update rebuilds).  *changed: anything differs from the
 * previous update. */
static HtmlDocument *mirror_update(JSContextHandle ctx, GCValue js_doc, bool full, bool *changed) {
    if (changed) *changed = false;
    GCValue doc_elem = render_root_element(ctx, js_doc);
    NodeId root = dom_node_of(doc_elem);
    if (!root || dom_kind(root) != DOM_K_ELEMENT) {
        return NULL;
    }
    g_mir.ctx = ctx;
    g_mir.full = full;
    g_mir.changed = false;
    g_mir.unsupported = false;
    g_mir.structure = false;
    {
        CP_ACC("mirror: ShadyDOM check");
        GCValue sd = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "ShadyDOM");
        g_mir.shady = JS_IsObject(sd) && JS_ToBool(ctx, JS_GetPropertyStr(ctx, sd, "inUse"));
    }
    uint64_t dirty0 = g_dom.dirty_version;
    bool rebuild = !g_mir.valid || !g_mir.doc || g_mir.root_id != root;
    if (!rebuild) {
        /* Deleted nodes and replaced attribute lists pile up: start over
         * when they outweigh the live document. */
        PreOrderCompactionArray *a = &g_mir.doc->array;
        if (a->tombstone_count > a->active_count + 4096) rebuild = true;
    }
    if (rebuild) {
        CP_ACC("mirror: rebuild");
        g_mir.valid = mir_rebuild(root);
    } else if (full) {
        CP_ACC("mirror: full update");
        int ri = mir_map_get(root);
        if (ri < 0) g_mir.valid = mir_rebuild(root);
        else mir_update_element(root, ri, false);
    } else {
        CP_ACC("mirror: dirty update");
        int ri = mir_map_get(root);
        if (ri < 0) g_mir.valid = mir_rebuild(root);
        else mir_update_element(root, ri, false);
    }
    g_mir.updates++;
    NodeId dn = dom_node_of(js_doc);
    if (dom_kind(dn) != DOM_K_DOCUMENT) dn = g_dom_document;
    if (g_mir.unsupported || !g_mir.doc) {
        mir_drop();
        return NULL;
    }
    g_mir.valid = true;
    /* Layout and selector matching read the array in document order (a
     * subtree is a contiguous range): put added and moved nodes back in
     * pre-order, dropping the deleted ones, and index the result. */
    {
        CP_ACC("mirror: reorder + sheets");
        if (g_mir.structure) mir_reorder();
        if (g_mir.structure || !g_mir.sheet_sig_valid) mir_note_sheets();
        html_document_find_roots(g_mir.doc);
    }
    /* Page code run by getters during the update: its marks must survive
     * the clear (the next update sees them), so leave the bits. */
    {
        CP_ACC("mirror: clear dirty bits");
        if (g_dom.dirty_version == dirty0) dom_clear_dirty(dn);
    }
    if (g_mir.changed) g_mir.version++;
    if (changed) *changed = g_mir.changed;
    static int verify = -1;
    if (verify < 0) verify = cyber_getenv("CYBER_MIRROR_VERIFY") ? 1 : 0;
    if (verify) mir_verify(ctx, js_doc);
    return g_mir.doc;
}

HtmlDocument *html_mirror_document(JSContextHandle ctx, GCValue js_doc, bool full,
                                   uint64_t *version, bool *changed) {
    static int off = -1;
    if (off < 0) {
        const char *e = cyber_getenv("CYBER_MIRROR");
        off = e && e[0] == '0';
    }
    if (off) return NULL;
    CP_ACC("mirror: update");
    HtmlDocument *d = mirror_update(ctx, js_doc, full, changed);
    if (d && version) *version = g_mir.version;   /* >= 1: every build changes it */
    return d;
}

void html_mirror_stats(uint64_t *updates, uint64_t *rebuilds, uint64_t *nodes_built) {
    if (updates) *updates = g_mir.updates;
    if (rebuilds) *rebuilds = g_mir.rebuilds;
    if (nodes_built) *nodes_built = g_mir.nodes_built;
}

/* ---- Verification (CYBER_MIRROR_VERIFY=1) ---- */

static void mir_path(HtmlDocument *d, int idx, char *out, size_t cap) {
    char parts[16][48];
    int n = 0;
    for (int i = idx; i >= 0 && n < 16; i = po_array_parent(&d->array, i)) {
        HtmlNode *hn = po_array_payload(&d->array, i);
        const char *cls = NULL;
        for (HtmlAttribute *a = hn ? hn->attributes : NULL; a; a = a->next)
            if (strcmp(a->name, "class") == 0) cls = a->value;
        snprintf(parts[n++], sizeof(parts[0]), "%s%s%.20s", hn && hn->type == HTML_NODE_TEXT ? "#text" : hn ? hn->tag_name : "?",
                 cls ? "." : "", cls ? cls : "");
    }
    size_t o = 0;
    out[0] = '\0';
    for (int k = n - 1; k >= 0 && o + 2 < cap; k--)
        o += (size_t)snprintf(out + o, cap - o, "%s%s", parts[k], k ? " > " : "");
}

static int mir_cmp(HtmlDocument *a, int ia, HtmlDocument *b, int ib, int *reported) {
    int diffs = 0;
    HtmlNode *x = po_array_payload(&a->array, ia), *y = po_array_payload(&b->array, ib);
    const char *why = NULL;
    char detail[256] = "";
    if (x->type != y->type) why = "node type";
    else if (x->type == HTML_NODE_ELEMENT) {
        if (strcmp(x->tag_name, y->tag_name) != 0) { why = "tag"; snprintf(detail, sizeof(detail), "%s vs %s", x->tag_name, y->tag_name); }
        else if (x->store_id != y->store_id) why = "store id";
        else {
            HtmlAttribute *p = x->attributes, *q = y->attributes;
            for (; p && q; p = p->next, q = q->next)
                if (strcmp(p->name, q->name) != 0 || strcmp(p->value, q->value) != 0) break;
            if (p || q) {
                why = "attributes";
                snprintf(detail, sizeof(detail), "mirror %s=\"%.60s\" vs parse %s=\"%.60s\"", p ? p->name : "-",
                         p ? p->value : "", q ? q->name : "-", q ? q->value : "");
            } else if ((x->text_content == NULL) != (y->text_content == NULL) ||
                       (x->text_content && strcmp(x->text_content, y->text_content) != 0)) {
                why = "raw text";
            }
        }
    } else if (x->type == HTML_NODE_TEXT) {
        if (strcmp(x->text_content ? x->text_content : "", y->text_content ? y->text_content : "") != 0) {
            why = "text";
            snprintf(detail, sizeof(detail), "mirror \"%.50s\" vs parse \"%.50s\"", x->text_content ? x->text_content : "",
                     y->text_content ? y->text_content : "");
        }
    }
    if (why) {
        if ((*reported)++ < 12) {
            char path[512];
            mir_path(a, ia, path, sizeof(path));
            fprintf(stderr, "[mirror-verify] %s differs at %s %s\n", why, path, detail);
        }
        return 1;
    }
    int ca = po_array_first_child(&a->array, ia), cb = po_array_first_child(&b->array, ib);
    for (; ca >= 0 && cb >= 0; ca = po_array_next_sibling(&a->array, ca), cb = po_array_next_sibling(&b->array, cb))
        diffs += mir_cmp(a, ca, b, cb, reported);
    if (ca >= 0 || cb >= 0) {
        if ((*reported)++ < 12) {
            char path[512];
            mir_path(a, ia, path, sizeof(path));
            HtmlNode *extra = ca >= 0 ? po_array_payload(&a->array, ca) : po_array_payload(&b->array, cb);
            fprintf(stderr, "[mirror-verify] child count differs at %s (extra in %s: %s \"%.40s\")\n", path,
                    ca >= 0 ? "mirror" : "parse", extra->type == HTML_NODE_TEXT ? "#text" : extra->tag_name,
                    extra->type == HTML_NODE_TEXT && extra->text_content ? extra->text_content : "");
        }
        diffs++;
    }
    return diffs;
}

static int mir_verify(JSContextHandle ctx, GCValue js_doc) {
    SerializeBuffer buf;
    GCValue doc_elem = render_root_element(ctx, js_doc);
    if (!snapshot_serialize(ctx, doc_elem, &buf, 0, false)) return -1;
    HtmlDocument *p = html_parse(buf.data, buf.len);
    if (p) map_store_ids(p, &buf);
    serialize_buf_free(&buf);
    static unsigned long checks, failing;
    checks++;
    if (!p) return -1;
    int reported = 0;
    int diffs = 0;
    if (g_mir.doc->root_idx >= 0 && p->root_idx >= 0) diffs = mir_cmp(g_mir.doc, g_mir.doc->root_idx, p, p->root_idx, &reported);
    else diffs = g_mir.doc->root_idx != p->root_idx;
    /* The array must be in pre-order without tombstones (index ranges are
     * subtrees for layout and selector matching). */
    {
        PreOrderCompactionArray *a = &g_mir.doc->array;
        int expect = 0;
        bool ordered = a->tombstone_count == 0 && a->count == a->active_count;
        for (int r = a->first_root; r >= 0 && ordered; r = po_array_next_sibling(a, r)) {
            /* Pre-order walk over the links (no stack). */
            int i = r;
            while (ordered) {
                if (i != expect++) { ordered = false; break; }
                int c = po_array_first_child(a, i);
                if (c >= 0) { i = c; continue; }
                while (i != r && po_array_next_sibling(a, i) < 0) i = po_array_parent(a, i);
                if (i == r) break;
                i = po_array_next_sibling(a, i);
            }
        }
        if (!ordered || (size_t)expect != a->count) {
            fprintf(stderr, "[mirror-verify] array not in pre-order (count %zu, active %zu, tombstones %zu)\n",
                    a->count, a->active_count, a->tombstone_count);
            diffs++;
        }
    }
    if (diffs) failing++;
    if (diffs || checks % 100 == 0)
        fprintf(stderr, "[mirror-verify] check %lu: %d differences (%lu of %lu checks differ)\n", checks, diffs, failing, checks);
    html_document_free(p);
    return diffs;
}

int html_mirror_check(JSContextHandle ctx, GCValue js_doc) {
    if (!g_mir.doc || !g_mir.valid) return -1;
    return mir_verify(ctx, js_doc);
}

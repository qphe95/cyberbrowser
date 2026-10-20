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

/* Helper: transfer numeric properties and style from old element to new element */
static void html_transfer_element_properties(JSContextHandle ctx, GCValue new_elem, GCValue old_elem) {
    if (JS_IsUndefined(old_elem) || JS_IsNull(old_elem)) return;
    const char *numeric_props[] = {
        "clientWidth", "clientHeight", "scrollWidth", "scrollHeight",
        "offsetWidth", "offsetHeight", NULL
    };
    for (int i = 0; numeric_props[i]; i++) {
        GCValue val = JS_GetPropertyStr(ctx, old_elem, numeric_props[i]);
        if (!JS_IsUndefined(val) && !JS_IsNull(val)) {
            JS_SetPropertyStr(ctx, new_elem, numeric_props[i], val);
        }
    }
    GCValue old_style = JS_GetPropertyStr(ctx, old_elem, "style");
    GCValue new_style = JS_GetPropertyStr(ctx, new_elem, "style");
    if (JS_IsObject(old_style) && JS_IsObject(new_style)) {
        JSPropertyEnum *tab = NULL;
        uint32_t plen = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &plen, old_style, JS_GPN_STRING_MASK) == 0 && tab) {
            for (uint32_t i = 0; i < plen; i++) {
                const char *key = JS_AtomToCString(ctx, tab[i].atom);
                if (key && strcmp(key, "removeProperty") != 0 &&
                    strcmp(key, "setProperty") != 0 &&
                    strcmp(key, "getPropertyValue") != 0 && key[0] != '_') {
                    GCValue val = JS_GetPropertyStr(ctx, old_style, key);
                    JS_SetPropertyStr(ctx, new_style, key, val);
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

/* Populate document `js_doc` from a parsed fragment: NEW documentElement,
 * head and body (copies of the parsed ones, as the legacy DOM did), the
 * parsed head and body children moved under them, and the root's other
 * children after them.  Properties of the old skeleton are transferred. */
static bool populate_from_fragment(JSContextHandle ctx, GCValue js_doc, NodeId frag) {
    NodeId doc = dom_node_of(js_doc);
    if (dom_kind(doc) != DOM_K_DOCUMENT) return false;
    DomParseRoots roots = dom_parse_roots(frag);
    GCValue old_doc_element = JS_GetPropertyStr(ctx, js_doc, "documentElement");
    GCValue old_body_element = JS_GetPropertyStr(ctx, js_doc, "body");
    GCValue old_head_element = JS_GetPropertyStr(ctx, js_doc, "head");

    NodeId de = shallow_copy(roots.root, "html", doc);
    NodeId head = shallow_copy(roots.head, "head", doc);
    NodeId body = shallow_copy(roots.body, "body", doc);
    if (roots.head) move_children(roots.head, head, false);
    if (roots.body) move_children(roots.body, body, false);
    dom_attach(de, head, DOM_NULL);
    dom_attach(de, body, DOM_NULL);
    if (roots.root) move_children(roots.root, de, true);
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
    g_dom_index_root = de;
    dom_request_layout();
    return true;
}

bool html_populate_js_document_from_html(JSContextHandle ctx, GCValue js_doc, const char *html, size_t len) {
    if (!SAFE_VALID(ctx) || !html || !len) return false;
    NodeId doc = owner_doc_of(js_doc);
    NodeId frag = dom_parse_html(html, len, doc);
    bool ok = populate_from_fragment(ctx, js_doc, frag);
    dom_free_subtree(frag);
    LOG_INFO("JS document populated from HTML (%zu bytes, %u nodes in store)", len, g_dom.live);
    return ok;
}

bool html_populate_js_document(JSContextHandle ctx, GCValue js_doc, HtmlDocument *doc) {
    if (!SAFE_VALID(ctx) || !doc) return false;
    if (JS_IsUndefined(js_doc) || JS_IsNull(js_doc)) return false;
    NodeId frag = fragment_from_html_document(doc, owner_doc_of(js_doc));
    bool ok = populate_from_fragment(ctx, js_doc, frag);
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
    populate_from_fragment(ctx, js_doc, frag);
    dom_free_subtree(frag);
    return js_doc;
}

/* Create JavaScript document object with parsed HTML structure */
GCValue html_create_js_document(JSContextHandle ctx, HtmlDocument *doc) {
    if (!SAFE_VALID(ctx) || !doc) return JS_NULL;
    GCValue js_doc = new_js_document(ctx);
    NodeId frag = fragment_from_html_document(doc, dom_node_of(js_doc));
    populate_from_fragment(ctx, js_doc, frag);
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
    const char **tags;
    size_t nids, capids;
    bool track;
    bool omit_script_text;   /* render snapshot: script text never renders */
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
    for (size_t i = 0; i < buf->nids; i++) free((void *)buf->tags[i]);
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
    buf->tags[buf->nids] = strdup(tag);
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
static char *js_object_get_string(JSContextHandle ctx, GCValue obj, const char *prop) {
    if (JS_IsUndefined(obj) || JS_IsNull(obj)) return NULL;
    GCValue val = JS_GetPropertyStr(ctx, obj, prop);
    if (JS_IsUndefined(val) || JS_IsNull(val)) return NULL;
    if (!JS_IsString(val)) return NULL;
    const char *s = JS_ToCString(ctx, val);
    if (!s) return NULL;
    return strdup(s);
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
static void html_serialize_children(JSContextHandle ctx, SerNode host, SerializeBuffer *buf) {
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
        GCValue val = JS_GetPropertyStr(ctx, style_obj, key);
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

static void html_serialize_element(JSContextHandle ctx, SerNode n, SerializeBuffer *buf) {
    char tag_lower[64];
    LooseAttrs loose = {0};
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
    serialize_note_element(buf, n.id, tag_lower);

    serialize_buf_append(buf, "<");
    serialize_buf_append(buf, tag_lower);

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
            serialize_buf_append_attr(buf, name, value);
        }
        free(order);
    } else {
        for (int i = 0; i < loose.n; i++) {
            if (strcasecmp(loose.names[i], "style") == 0) { attr_style = loose.values[i]; continue; }
            serialize_buf_append_attr(buf, loose.names[i], loose.values[i]);
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
            serialize_buf_append_attr(buf, "style", style_buf.data);
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
        for (int i = 0; extra_attrs[i].prop; i++) {
            bool has;
            if (n.id) {
                has = dom_attr(n.id, extra_attrs[i].attr) != NULL;
            } else {
                has = false;
                for (int k = 0; k < loose.n; k++)
                    if (strcasecmp(loose.names[k], extra_attrs[i].attr) == 0) { has = true; break; }
            }
            if (has) continue;
            char *value = js_object_get_string(ctx, n.obj, extra_attrs[i].prop);
            if (value && value[0]) serialize_buf_append_attr(buf, extra_attrs[i].attr, value);
            free(value);
        }
    }
    loose_attrs_free(&loose);

    if (html_is_self_closing_tag(tag_lower)) {
        serialize_buf_append(buf, ">");
        return;
    }
    serialize_buf_append(buf, ">");

    if (buf->omit_script_text && strcmp(tag_lower, "script") == 0) {
        /* Nothing to emit. */
    } else if (html_is_raw_content_tag(tag_lower)) {
        /* Raw text: textContent, unescaped. */
        if (n.id && legacy_limits()) {
            ser_text_content(n.id, buf);
        } else if (JS_IsObject(n.obj)) {
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

static uint64_t fnv1a64(const char *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 1099511628211ULL; }
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
HtmlDocument *html_document_from_js_dom_if_changed(JSContextHandle ctx, GCValue js_doc,
                                                   uint64_t *key, bool *unchanged) {
    if (unchanged) *unchanged = false;
    if (!SAFE_VALID(ctx)) return NULL;
    GCValue doc_elem = render_root_element(ctx, js_doc);
    if (JS_IsUndefined(doc_elem) || JS_IsNull(doc_elem)) return NULL;

    SerializeBuffer buf;
    if (!serialize_buf_init(&buf)) return NULL;
    buf.track = true;
    buf.omit_script_text = true;
    serialize_buf_append(&buf, "<!doctype html>\n");
    html_serialize_node(ctx, ser_of_value(doc_elem), &buf);

    NodeId dn = dom_node_of(js_doc);
    if (dom_kind(dn) != DOM_K_DOCUMENT) dn = g_dom_document;
    if (key) {
        uint64_t k = fnv1a64(buf.data, buf.len);
        if (*key == k) {
            if (unchanged) *unchanged = true;
            g_render_reuses++;
            dom_clear_dirty(dn);
            serialize_buf_free(&buf);
            return NULL;
        }
        *key = k;
    }
    HtmlDocument *doc = html_parse(buf.data, buf.len);
    if (doc) map_store_ids(doc, &buf);
    serialize_buf_free(&buf);
    g_render_snapshots++;
    dom_clear_dirty(dn);
    return doc;
}

/* Build a fresh native HtmlDocument from the JS document's current DOM. */
HtmlDocument *html_document_from_js_dom(JSContextHandle ctx, GCValue js_doc) {
    return html_document_from_js_dom_if_changed(ctx, js_doc, NULL, NULL);
}

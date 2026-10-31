/*
 * Browser-layer helpers over the node store and its wrappers.
 *
 * The DOM APIs (dom_api.c and friends) take wrappers from JS, work on the
 * store through NodeIds and hand wrappers back with dom_js(), which creates
 * them on first use.  These helpers keep that code short.
 */
#ifndef DOM_JS_H
#define DOM_JS_H

#include <quickjs.h>
#include "dom_store.h"
#include "dom_wrap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* nodeName: an element's qualified name (upper-cased for an HTML element in
 * an HTML document; "DIV" when the element has no name), "#text",
 * "#comment", "#document", "#document-fragment".  (localName: dom_store.h.) */
const char *dom_node_name(NodeId id);

/* Navigation returning wrappers (JS_NULL when there is no node). */
static inline GCValue dom_js_parent(JSContextHandle ctx, NodeId n) { return dom_js(ctx, dom_parent(n)); }
static inline GCValue dom_js_first(JSContextHandle ctx, NodeId n) { return dom_js(ctx, dom_first_child(n)); }
static inline GCValue dom_js_last(JSContextHandle ctx, NodeId n) { return dom_js(ctx, dom_last_child(n)); }
static inline GCValue dom_js_next(JSContextHandle ctx, NodeId n) { return dom_js(ctx, dom_next_sibling(n)); }
static inline GCValue dom_js_prev(JSContextHandle ctx, NodeId n) { return dom_js(ctx, dom_prev_sibling(n)); }

/* Attribute helpers with C strings. */
static inline const char *dom_attr(NodeId n, const char *name) { return dom_get_attr_str(n, name); }

/* The main document's node (set when the document object is created). */
extern NodeId g_dom_document;

/* Create the wrapper object of a store node (the DomWrapFactory). */
GCValue dom_create_wrapper(JSContextHandle ctx, NodeId id);
/* Prepare a fresh wrapper object for an element: per-element JS state the
 * page expects (attributes reflected as properties, canvas/template bits). */
void dom_element_setup_wrapper(JSContextHandle ctx, NodeId id, GCValue obj);
/* The element's CSSStyleDeclaration object (created on first use). */
GCValue dom_element_style(JSContextHandle ctx, GCValue el, bool create);

/* Structural mutations with the DOM API's side effects (custom element
 * reactions, script loading, mutation observers, layout invalidation).
 * Return false and throw on hierarchy errors. */
bool dom_api_insert(JSContextHandle ctx, NodeId parent, NodeId child, NodeId ref);
bool dom_api_remove(JSContextHandle ctx, NodeId parent, NodeId child);
/* dom_api_remove() for a node that is dropped: frees it when unwrapped. */
bool dom_api_remove_discard(JSContextHandle ctx, NodeId parent, NodeId child);

/* Parse `html` and append the result under `target` the way the legacy
 * innerHTML did (body children, else the top-level nodes, else a text
 * node with the raw string).  Existing children are removed first. */
void dom_set_inner_html(JSContextHandle ctx, NodeId target, const char *html, size_t len);

/* An attribute name as written out (SVG camelCase names restored). */
const char *dom_attr_name_for_output(const char *lower);

/* Serialize a subtree to HTML (innerHTML/outerHTML/XMLSerializer). */
char *dom_serialize(JSContextHandle ctx, NodeId n, bool outer, size_t *len_out);

/* Move a parsed element's children into a template content fragment for
 * every template element in the subtree (the parser's template handling). */
void dom_adopt_template_contents(NodeId root);

/* Custom-element reactions for an insertion into a connected tree: enqueue
 * upgrades and connectedCallbacks for the candidates in the subtree. */
void dom_ce_on_connect(JSContextHandle ctx, NodeId subtree);

#ifdef __cplusplus
}
#endif

#endif /* DOM_JS_H */

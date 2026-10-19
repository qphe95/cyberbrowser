/*
 * Layout snapshots of the node store (PARALLEL_DOM.md §7, "Rendering").
 *
 * The layout and the style system read an HtmlDocument: a preorder array of
 * HtmlNodes.
 *
 *   dom_snapshot_parsed()   a parsed fragment as the legacy html_parse()
 *                           produced it (what html_parse() now returns),
 *                           built by a linear C walk of the store.
 *
 * The render snapshot of a live JS document (html_document_from_js_dom in
 * html_dom.c) also reads JS-side state (style objects, polyfill shadow
 * roots); it writes the rendered tree as HTML and parses it with the store
 * parser, which reproduces the legacy render exactly.  Callers that keep a
 * snapshot key skip the rebuild when the rendered text did not change.
 */
#ifndef DOM_SNAPSHOT_H
#define DOM_SNAPSHOT_H

#include "dom_store.h"
#include "html_dom.h"

#ifdef __cplusplus
extern "C" {
#endif

HtmlDocument *html_document_new(void);
/* Append a node under `parent` (-1 = top level).  Returns its index. */
int html_document_add(HtmlDocument *doc, int parent, HtmlNodeType type);
/* Copy a string into the document's arena (NUL-terminated). */
const char *html_document_strdup(HtmlDocument *doc, const char *s, size_t n);
/* Add an attribute at the end of node `idx`'s list. */
void html_document_add_attr(HtmlDocument *doc, int idx, const char *name, const char *value, size_t vlen);
/* Fill root/head/body the way html_parse() did, from the top-level nodes. */
void html_document_find_roots(HtmlDocument *doc);

/* Snapshot of a parsed container (fragment or element): its children become
 * the document's top-level nodes. */
HtmlDocument *dom_snapshot_parsed(NodeId container);

#ifdef __cplusplus
}
#endif

#endif /* DOM_SNAPSHOT_H */

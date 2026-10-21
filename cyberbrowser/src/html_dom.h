/*
 * HTML DOM Parser - Header file
 * 
 * Provides HTML parsing and DOM node creation for the QuickJS engine.
 */

#ifndef HTML_DOM_H
#define HTML_DOM_H

#include <quickjs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "preorder_compaction_array.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum sizes for parsing */
#define HTML_MAX_TAG_NAME_LEN 128
#define HTML_MAX_ATTR_NAME_LEN 128
#define HTML_MAX_ATTR_VALUE_LEN 4096
#define HTML_MAX_ATTRS_PER_ELEMENT 64
#define HTML_MAX_NESTING_DEPTH 256

/* HTML node types (matching DOM spec) */
typedef enum {
    HTML_NODE_ELEMENT = 1,
    HTML_NODE_TEXT = 3,
    HTML_NODE_COMMENT = 8,
    HTML_NODE_DOCUMENT = 9,
    HTML_NODE_DOCUMENT_FRAGMENT = 11
} HtmlNodeType;

/* Forward declarations */
typedef struct HtmlNode HtmlNode;
typedef struct HtmlDocument HtmlDocument;
typedef struct HtmlAttribute HtmlAttribute;

/* HTML attribute structure.  Names and values are owned by the document
 * (its arena); names are lower case. */
typedef struct HtmlAttribute {
    const char *name;
    const char *value;
    struct HtmlAttribute *next;
} HtmlAttribute;

/* HTML node payload stored in PreOrderCompactionArray (header must be first).
 * This is the layout's input: a snapshot of the node store (dom_snapshot.h). */
struct HtmlNode {
    PreOrderCompactionArrayNode array_node;  /* Tree links and state */
    HtmlNodeType type;
    const char *tag_name;                    /* elements: lower-case name; others: "" */
    char *text_content;                      /* text nodes and raw-text elements (malloc'd) */
    size_t text_len;
    HtmlAttribute *attributes;
    GCValue js_object;                       /* QuickJS object reference */
    int has_js_object;                       /* Flag to track if js_object is valid */
    GCHandle layout_box_handle;              /* Optional cached LayoutBox handle */
    uint32_t store_id;                       /* NodeId in the node store (0 = none) */
};

struct HtmlArenaBlock;

/* HTML document structure */
struct HtmlDocument {
    PreOrderCompactionArray array;  /* Owns all HtmlNode nodes */
    int root_idx;                 /* Index of document root (usually <html>) */
    int head_idx;                 /* Index of <head> element */
    int body_idx;                 /* Index of <body> element */
    char *title;                  /* Document title */
    
    /* Parser state */
    int nesting_depth;
    int current_parent_idx;

    struct HtmlArenaBlock *arena; /* attribute names/values */

    /* Holders beyond the first (html_document_free drops one; the last
     * frees).  The layout mirror hands its document out this way. */
    int refs;
};


/* ============================================================================
 * HTML DOM Functions
 * ============================================================================ */

/* Parse HTML string and create a document structure */
HtmlDocument* html_parse(const char *html, size_t html_len);

/* Free HTML document and all associated nodes */
void html_document_free(HtmlDocument *doc);

/* Create DOM nodes in QuickJS context from parsed HTML document */
bool html_create_dom_in_js(JSContextHandle ctx, HtmlDocument *doc);

/* Populate an existing JS document object from parsed HTML.
 * Uses document.createElement() so elements get proper DOM prototypes.
 * The existing document's createElement, body, head, and appendChild are used. */
bool html_populate_js_document(JSContextHandle ctx, GCValue js_doc, HtmlDocument *doc);
/* The same straight from HTML text (parsed into the node store). */
bool html_populate_js_document_from_html(JSContextHandle ctx, GCValue js_doc, const char *html, size_t len);

/* Create a single HTML element in QuickJS */
GCValue html_create_element_js(JSContextHandle ctx, const char *tag_name, HtmlAttribute *attrs);

/* Create a single HTML element using document.createElement() if available */
GCValue html_create_element_js_with_document(JSContextHandle ctx, GCValue js_doc, const char *tag_name, HtmlAttribute *attrs);

/* Set the innerHTML of an element by parsing the HTML string and replacing
 * the element's children with the parsed nodes. */
bool html_element_set_inner_html(JSContextHandle ctx, GCValue elem, const char *html);
bool html_element_set_inner_html_len(JSContextHandle ctx, GCValue elem, const char *html, size_t len);

/* Set the innerHTML of a ShadowRoot by parsing the HTML string and replacing
 * the shadow root's children with the parsed nodes. */
bool html_shadow_root_set_inner_html(JSContextHandle ctx, GCValue shadow_root, const char *html);

/* Access well-known document nodes (transient pointers; valid until tree mutates). */
HtmlNode* html_document_root(HtmlDocument *doc);
HtmlNode* html_document_head(HtmlDocument *doc);
HtmlNode* html_document_body(HtmlDocument *doc);

/* Tree navigation helpers (transient pointers; valid until tree mutates). */
HtmlNode* html_node_parent(HtmlDocument *doc, HtmlNode *node);
HtmlNode* html_node_first_child(HtmlDocument *doc, HtmlNode *node);
HtmlNode* html_node_last_child(HtmlDocument *doc, HtmlNode *node);
HtmlNode* html_node_next_sibling(HtmlDocument *doc, HtmlNode *node);
HtmlNode* html_node_prev_sibling(HtmlDocument *doc, HtmlNode *node);

/* Helper to get element by tag name from document */
HtmlNode* html_document_get_element_by_tag(HtmlDocument *doc, const char *tag_name);

/* Helper to get element by id from document */
HtmlNode* html_document_get_element_by_id(HtmlDocument *doc, const char *id);

/* Helper to get all elements by tag name */
int html_document_get_elements_by_tag(HtmlDocument *doc, const char *tag_name, 
                                       HtmlNode **out_nodes, int max_nodes);

/* Create JavaScript document object with parsed HTML elements */
GCValue html_create_js_document(JSContextHandle ctx, HtmlDocument *doc);

/* Serialize a JS DOM node (and its descendants) to a heap-allocated HTML
 * string. The caller must free the returned pointer. */
char *html_serialize_js_node(JSContextHandle ctx, GCValue node);
/* An element style object as CSS text (malloc'ed), NULL when empty. */
char *html_style_object_css_text(JSContextHandle ctx, GCValue style_obj);

/* Build a fresh native HtmlDocument from the JS document's current DOM. */
HtmlDocument *html_document_from_js_dom(JSContextHandle ctx, GCValue js_doc);
/* The same, skipped when the document still serializes to the snapshot
 * `*key` names (returns NULL with *unchanged set); *key is updated. */
HtmlDocument *html_document_from_js_dom_if_changed(JSContextHandle ctx, GCValue js_doc,
                                                   uint64_t *key, bool *unchanged);
/* The same, copying the previous snapshot's text for subtrees the store has
 * not marked dirty since (and that read no JS-only state).  Changes to
 * properties that live only on JS objects show up at the next full
 * (non-incremental) snapshot.  CYBER_SNAPSHOT_VERIFY=1 compares each with
 * a full snapshot. */
HtmlDocument *html_document_from_js_dom_incremental(JSContextHandle ctx, GCValue js_doc,
                                                    uint64_t *key, bool *unchanged);
/* The layout mirror (html_dom.c): the document as layout reads it, kept in
 * step with the live DOM in place.  The snapshot functions above return it
 * (each caller holds a reference; html_document_free drops it).  NULL when
 * the page needs the serializer (CYBER_MIRROR=0: always). */
HtmlDocument *html_mirror_document(JSContextHandle ctx, GCValue js_doc, bool full,
                                   uint64_t *version, bool *changed);
void html_mirror_stats(uint64_t *updates, uint64_t *rebuilds, uint64_t *nodes_built);
/* Test hook: differences between the mirror and serialize + parse now
 * (-1: no mirror). */
int html_mirror_check(JSContextHandle ctx, GCValue js_doc);

/* Bytes and elements incremental snapshots copied (tests, benchmarks). */
void html_snapshot_stats(uint64_t *reused_bytes, uint64_t *reused_elements);
/* Render snapshots built / reused (tests, benchmarks). */
void html_render_stats(uint64_t *snapshots, uint64_t *reuses);

/* ============================================================================
 * HTML Utility Functions
 * ============================================================================ */

/* Decode HTML entities in place (modifies the string) */
int html_decode_entities(char *str, size_t len);

/* Check if a tag is self-closing */
bool html_is_self_closing_tag(const char *tag_name);

/* Check if a tag should have raw content (script, style, etc.) */
bool html_is_raw_content_tag(const char *tag_name);

/* Convert tag name to lowercase */
void html_tag_name_normalize(char *tag_name);

#ifdef __cplusplus
}
#endif

#endif /* HTML_DOM_H */

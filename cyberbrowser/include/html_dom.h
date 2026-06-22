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

/* HTML attribute structure */
typedef struct HtmlAttribute {
    char name[HTML_MAX_ATTR_NAME_LEN];
    char value[HTML_MAX_ATTR_VALUE_LEN];
    struct HtmlAttribute *next;
} HtmlAttribute;

/* HTML node payload stored in PreOrderCompactionArray (header must be first). */
struct HtmlNode {
    PreOrderCompactionArrayNode array_node;  /* Tree links and state */
    HtmlNodeType type;
    char tag_name[HTML_MAX_TAG_NAME_LEN];    /* For element nodes */
    char *text_content;                      /* For text/comment nodes */
    size_t text_len;
    HtmlAttribute *attributes;
    GCValue js_object;                       /* QuickJS object reference */
    int has_js_object;                       /* Flag to track if js_object is valid */
    GCHandle layout_box_handle;              /* Optional cached LayoutBox handle */
};

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
};

/* Parser state structure */
typedef struct {
    const char *html;
    size_t html_len;
    size_t pos;
    int line, column;
    int nesting_depth;
    HtmlDocument *document;
} HtmlParser;

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

/* Create a single HTML element in QuickJS */
GCValue html_create_element_js(JSContextHandle ctx, const char *tag_name, HtmlAttribute *attrs);

/* Create a single HTML element using document.createElement() if available */
GCValue html_create_element_js_with_document(JSContextHandle ctx, GCValue js_doc, const char *tag_name, HtmlAttribute *attrs);

/* Set the innerHTML of an element by parsing the HTML string and replacing
 * the element's children with the parsed nodes. */
bool html_element_set_inner_html(JSContextHandle ctx, GCValue elem, const char *html);

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

/* Build a fresh native HtmlDocument from the JS document's current DOM. */
HtmlDocument *html_document_from_js_dom(JSContextHandle ctx, GCValue js_doc);

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

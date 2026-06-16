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

/* HTML node structure */
struct HtmlNode {
    HtmlNodeType type;
    char tag_name[HTML_MAX_TAG_NAME_LEN];  /* For element nodes */
    char *text_content;                     /* For text/comment nodes */
    size_t text_len;
    
    HtmlAttribute *attributes;
    HtmlNode *first_child;
    HtmlNode *last_child;
    HtmlNode *next_sibling;
    HtmlNode *prev_sibling;
    HtmlNode *parent;
    
    /* QuickJS object reference (created when node is added to document) */
    GCValue js_object;
    int has_js_object;  /* Flag to track if js_object is valid */
};

/* HTML document structure */
struct HtmlDocument {
    HtmlNode *root;           /* Document root (usually <html>) */
    HtmlNode *head;           /* Reference to <head> element */
    HtmlNode *body;           /* Reference to <body> element */
    char *title;              /* Document title */
    
    /* Parser state */
    int nesting_depth;
    HtmlNode *current_parent;
};

/* Parser state structure */
typedef struct {
    const char *html;
    size_t html_len;
    size_t pos;
    int line;
    int column;
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

/* Helper to get element by tag name from document */
HtmlNode* html_document_get_element_by_tag(HtmlDocument *doc, const char *tag_name);

/* Helper to get all elements by tag name */
int html_document_get_elements_by_tag(HtmlDocument *doc, const char *tag_name, 
                                       HtmlNode **out_nodes, int max_nodes);

/* Create JavaScript document object with parsed HTML elements */
GCValue html_create_js_document(JSContextHandle ctx, HtmlDocument *doc);

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

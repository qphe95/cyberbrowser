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
    uint32_t attr_gen;                       /* doc->attr_gen when its attribute list last changed */
    uint64_t attr_hash;                      /* tag + attributes hash for restyle keys (0 = not computed) */
    /* Restyle :has() checks of this element's tag and attributes against
     * the style set's key sets (css_layout.c): valid while has_memo_stamp
     * matches its attr_hash and the key sets' generation; -1 = not known. */
    uint64_t has_memo_stamp;
    int8_t has_memo[5];
    /* How its style cache entry was last stored: 0 not known, 1 display:none,
     * 2 shown (css_layout.c: a tree build leaving hidden subtrees out skips
     * the lookup for 2). */
    unsigned char hidden_hint;
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

    /* What caches over the document key on: serial is unique per document,
     * struct_gen changes when nodes are added, removed or moved, and
     * attr_gen counts attribute-list changes (each node keeps the value of
     * its last one). */
    uint64_t serial;
    uint64_t struct_gen;
    uint32_t attr_gen;
    /* Bumped when the document's stylesheets may have changed (a style or
     * link element added, removed, moved or edited: the mirror's SHEETS
     * records), when sheets_tracked (the mirror's document; others may be
     * edited untracked). */
    uint64_t sheet_gen;
    bool sheets_tracked;
};

/* Note that node `idx`'s attribute list was replaced or extended. */
void html_document_attrs_changed(HtmlDocument *doc, int idx);


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

/* The page streamed into `js_doc` as it arrives (HTML's parsing model): the
 * skeleton's root is replaced, documentElement / head / body follow the
 * parser, readyState is "loading", and what the parser inserts is connected
 * at once (custom elements defined so far upgrade).  step builds what has
 * arrived and returns the parser-inserted <script> element to run before
 * calling it again, or JS_NULL when it needs more input (or, with `final`,
 * is done).  end completes a page that lacked <html>, <head> or <body>. */
typedef struct HtmlDocStream HtmlDocStream;
HtmlDocStream *html_doc_stream_begin(JSContextHandle ctx, GCValue js_doc);
void html_doc_stream_append(HtmlDocStream *s, const char *data, size_t len);
GCValue html_doc_stream_step(HtmlDocStream *s, bool final);
const char *html_doc_stream_source(HtmlDocStream *s, size_t *len);
void html_doc_stream_end(HtmlDocStream *s);
/* The stream's document made the XML document of data instead (mime: its
 * MIME type): parsed whole, its parsing ended. */
void html_doc_stream_xml(HtmlDocStream *s, const char *data, size_t len, const char *mime);
/* The underlying parser stream (document.write's insertion point). */
struct DomStream *html_doc_stream_parser(HtmlDocStream *s);

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
/* What the mirror changed, for consumers that keep results between updates
 * (layout_api's forced layout).  Each update appends one record per element
 * whose attribute list changed (old and new lists, both alive while the
 * document is) and per element whose rendered children changed (added,
 * removed, moved, text); new subtrees appear only as their parent's record.
 * A SHEETS record says the document's stylesheets may differ (a style or
 * link element added, removed, moved or edited). */
typedef enum { HTML_MIRROR_ATTRS = 1, HTML_MIRROR_CHILDREN = 2, HTML_MIRROR_SHEETS = 3 } HtmlMirrorChangeKind;
typedef struct HtmlMirrorChange {
    uint32_t id;                    /* store NodeId of the element */
    uint8_t kind;                   /* HtmlMirrorChangeKind */
    uint8_t empty_changed;          /* CHILDREN: the element went from no children to some, or back */
    const HtmlAttribute *old_attrs; /* ATTRS: the list before and after */
    const HtmlAttribute *new_attrs;
} HtmlMirrorChange;

/* The current end of the change log (pass it to html_mirror_changes later). */
uint64_t html_mirror_change_seq(void);
/* Records appended after position `since` in *out / *n.  False when they are
 * not all known (the log was reset by a rebuild or overflowed, or `doc` is
 * not the mirror document): treat the document as changed everywhere. */
bool html_mirror_changes(const HtmlDocument *doc, uint64_t since, const HtmlMirrorChange **out, size_t *n);

/* Index of store node `id` in the mirror document `doc`, or -1 (not in it,
 * or `doc` is not the mirror's). */
int html_mirror_index_of(const HtmlDocument *doc, uint32_t id);

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

/* Legacy HTML parser (test oracle).  See legacy_html_parse.c. */
#ifndef LEGACY_HTML_PARSE_H
#define LEGACY_HTML_PARSE_H
#include <stddef.h>
#include "preorder_compaction_array.h"

#define LG_MAX_TAG_NAME_LEN 128
#define LG_MAX_ATTR_NAME_LEN 128
#define LG_MAX_ATTR_VALUE_LEN 4096
#define LG_MAX_NESTING_DEPTH 256

typedef enum { LG_NODE_ELEMENT = 1, LG_NODE_TEXT = 3 } LgNodeType;

typedef struct LgAttr {
    char name[LG_MAX_ATTR_NAME_LEN];
    char value[LG_MAX_ATTR_VALUE_LEN];
    struct LgAttr *next;
} LgAttr;

typedef struct LgNode {
    PreOrderCompactionArrayNode array_node;
    LgNodeType type;
    char tag_name[LG_MAX_TAG_NAME_LEN];
    char *text_content;
    size_t text_len;
    LgAttr *attributes;
    int js_object, has_js_object;
} LgNode;

typedef struct LgDoc {
    PreOrderCompactionArray array;
    int root_idx, head_idx, body_idx;
    char *title;
    int nesting_depth, current_parent_idx;
} LgDoc;

typedef struct {
    const char *html;
    size_t lg_len, pos;
    int line, column, nesting_depth;
    LgDoc *document;
} LgParser;

LgDoc *lg_parse(const char *html, size_t html_len);
void lg_document_free(LgDoc *doc);
#endif

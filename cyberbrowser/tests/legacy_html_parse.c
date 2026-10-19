/*
 * The legacy recursive HTML parser (html_dom.c before the node store), kept
 * verbatim as the oracle for the parser-equivalence tests.  Only the types are
 * renamed and given their old fixed-size buffers.
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "preorder_compaction_array.h"
#include "legacy_html_parse.h"

/* List of self-closing HTML tags (void elements) */
static const char *LG_SELF_CLOSING[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input",
    "link", "meta", "param", "source", "track", "wbr", NULL
};

/* List of tags that contain raw text content */
static const char *LG_RAW_TAGS[] = {
    "script", "style", "textarea", "title", NULL
};

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

static bool lg_is_self_closing_tag(const char *tag_name) {
    if (!tag_name) return false;
    for (int i = 0; LG_SELF_CLOSING[i]; i++) {
        if (strcasecmp(tag_name, LG_SELF_CLOSING[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool lg_is_raw_content_tag(const char *tag_name) {
    if (!tag_name) return false;
    for (int i = 0; LG_RAW_TAGS[i]; i++) {
        if (strcasecmp(tag_name, LG_RAW_TAGS[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void lg_tag_name_normalize(char *tag_name) {
    if (!tag_name) return;
    for (char *p = tag_name; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
}

/* Decode common HTML entities */
static int lg_decode_entities(char *str, size_t len) {
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

static LgNode* lg_node_at(LgDoc *doc, int idx) {
    if (!doc || idx < 0) return NULL;
    return (LgNode*)po_array_payload(&doc->array, idx);
}

static int lg_node_create(LgDoc *doc, LgNodeType type, int parent_idx) {
    int idx = po_array_add(&doc->array, parent_idx);
    if (idx < 0) return -1;

    LgNode *node = lg_node_at(doc, idx);
    node->type = type;
    node->js_object = 0;
    node->has_js_object = 0;
    return idx;
}

static LgAttr* lg_attribute_create(const char *name, const char *value) {
    LgAttr *attr = (LgAttr*)calloc(1, sizeof(LgAttr));
    if (!attr) return NULL;
    
    strncpy(attr->name, name, LG_MAX_ATTR_NAME_LEN - 1);
    attr->name[LG_MAX_ATTR_NAME_LEN - 1] = '\0';
    
    if (value) {
        strncpy(attr->value, value, LG_MAX_ATTR_VALUE_LEN - 1);
        attr->value[LG_MAX_ATTR_VALUE_LEN - 1] = '\0';
        lg_decode_entities(attr->value, strlen(attr->value));
    }
    
    return attr;
}

static void lg_node_free_contents(LgNode *node) {
    if (!node) return;
    
    LgAttr *attr = node->attributes;
    while (attr) {
        LgAttr *next = attr->next;
        free(attr);
        attr = next;
    }
    
    if (node->text_content) {
        free(node->text_content);
    }
    
    /* Note: We don't free js_object here - that's managed by QuickJS GC */
}

void lg_document_free(LgDoc *doc) {
    if (!doc) return;
    
    size_t n = po_array_count(&doc->array);
    for (size_t i = 0; i < n; i++) {
        LgNode *node = lg_node_at(doc, (int)i);
        if (node) lg_node_free_contents(node);
    }
    
    po_array_free(&doc->array);
    
    if (doc->title) free(doc->title);
    free(doc);
}

/* ============================================================================
 * Parser Implementation
 * ============================================================================ */

static void lgp_skip_whitespace(LgParser *p) {
    while (p->pos < p->lg_len && isspace((unsigned char)p->html[p->pos])) {
        if (p->html[p->pos] == '\n') {
            p->line++;
            p->column = 1;
        } else {
            p->column++;
        }
        p->pos++;
    }
}

static bool lgp_match(LgParser *p, const char *str) {
    size_t len = strlen(str);
    if (p->pos + len > p->lg_len) return false;
    return (strncasecmp(p->html + p->pos, str, len) == 0);
}

static char* lgp_read_tag_name(LgParser *p) {
    static char tag_name[LG_MAX_TAG_NAME_LEN];
    size_t i = 0;
    
    /* Skip initial whitespace */
    lgp_skip_whitespace(p);
    
    /* Read tag name */
    while (p->pos < p->lg_len && i < LG_MAX_TAG_NAME_LEN - 1) {
        char c = p->html[p->pos];
        if (c == '>' || c == '/' || c == ' ' || c == '\t' || 
            c == '\n' || c == '\r' || c == '\f') {
            break;
        }
        tag_name[i++] = c;
        p->pos++;
        p->column++;
    }
    
    tag_name[i] = '\0';
    lg_tag_name_normalize(tag_name);
    
    return i > 0 ? tag_name : NULL;
}

static LgAttr* lgp_read_attribute(LgParser *p) {
    lgp_skip_whitespace(p);
    
    /* Check for end of tag */
    if (p->pos >= p->lg_len || p->html[p->pos] == '>' || 
        (p->html[p->pos] == '/' && p->pos + 1 < p->lg_len && p->html[p->pos + 1] == '>')) {
        return NULL;
    }
    
    /* Read attribute name */
    char name[LG_MAX_ATTR_NAME_LEN];
    size_t name_len = 0;
    
    while (p->pos < p->lg_len && name_len < LG_MAX_ATTR_NAME_LEN - 1) {
        char c = p->html[p->pos];
        if (c == '=' || c == '>' || c == '/' || isspace((unsigned char)c)) {
            break;
        }
        name[name_len++] = c;
        p->pos++;
        p->column++;
    }
    name[name_len] = '\0';
    
    if (name_len == 0) return NULL;
    
    /* Check for value */
    char value[LG_MAX_ATTR_VALUE_LEN] = {0};
    
    lgp_skip_whitespace(p);
    
    if (p->pos < p->lg_len && p->html[p->pos] == '=') {
        p->pos++; /* skip = */
        p->column++;
        
        lgp_skip_whitespace(p);
        
        /* Read value */
        if (p->pos < p->lg_len) {
            char quote = p->html[p->pos];
            if (quote == '"' || quote == '\'') {
                p->pos++; /* skip opening quote */
                p->column++;
                
                size_t val_len = 0;
                while (p->pos < p->lg_len && p->html[p->pos] != quote && 
                       val_len < LG_MAX_ATTR_VALUE_LEN - 1) {
                    value[val_len++] = p->html[p->pos++];
                    p->column++;
                }
                value[val_len] = '\0';
                
                if (p->pos < p->lg_len && p->html[p->pos] == quote) {
                    p->pos++; /* skip closing quote */
                    p->column++;
                }
            } else {
                /* Unquoted value */
                size_t val_len = 0;
                while (p->pos < p->lg_len && val_len < LG_MAX_ATTR_VALUE_LEN - 1) {
                    char c = p->html[p->pos];
                    if (c == '>' || c == '/' || isspace((unsigned char)c)) {
                        break;
                    }
                    value[val_len++] = c;
                    p->pos++;
                    p->column++;
                }
                value[val_len] = '\0';
            }
        }
    }
    
    return lg_attribute_create(name, value[0] ? value : NULL);
}

static int lgp_parse_element(LgParser *p, int parent_idx);
static int lgp_parse_text(LgParser *p, int parent_idx);

static void lgp_skip_comment(LgParser *p) {
    if (lgp_match(p, "<!--")) {
        p->pos += 4;
        while (p->pos < p->lg_len) {
            if (lgp_match(p, "-->")) {
                p->pos += 3;
                break;
            }
            p->pos++;
        }
    }
}

static void lgp_skip_doctype(LgParser *p) {
    if (lgp_match(p, "<!doctype") || lgp_match(p, "<!DOCTYPE")) {
        p->pos += 9;
        while (p->pos < p->lg_len && p->html[p->pos] != '>') {
            p->pos++;
        }
        if (p->pos < p->lg_len) p->pos++; /* skip > */
    }
}

static char* lgp_read_raw_content(LgParser *p, const char *end_tag) {
    size_t start = p->pos;
    size_t end_tag_len = strlen(end_tag);
    
    while (p->pos < p->lg_len) {
        if (p->html[p->pos] == '<') {
            /* Check for end tag */
            if (p->pos + 1 < p->lg_len && p->html[p->pos + 1] == '/') {
                if (strncasecmp(p->html + p->pos + 2, end_tag, end_tag_len) == 0) {
                    break;
                }
            }
        }
        p->pos++;
    }
    
    size_t len = p->pos - start;
    char *content = (char*)malloc(len + 1);
    if (content) {
        memcpy(content, p->html + start, len);
        content[len] = '\0';
    }
    
    return content;
}

static int lgp_parse_element(LgParser *p, int parent_idx) {
    if (p->pos >= p->lg_len || p->html[p->pos] != '<') {
        return -1;
    }
    
    p->pos++; /* skip < */
    
    /* Check for closing tag */
    if (p->pos < p->lg_len && p->html[p->pos] == '/') {
        /* Closing tag - let caller handle */
        p->pos--; /* back up */
        return -1;
    }
    
    /* Read tag name */
    char *tag_name = lgp_read_tag_name(p);
    if (!tag_name) {
        /* Skip malformed tag */
        while (p->pos < p->lg_len && p->html[p->pos] != '>') p->pos++;
        if (p->pos < p->lg_len) p->pos++;
        return -1;
    }
    
    /* Create element node */
    int node_idx = lg_node_create(p->document, LG_NODE_ELEMENT, parent_idx);
    if (node_idx < 0) return -1;
    
    LgNode *node = lg_node_at(p->document, node_idx);
    strncpy(node->tag_name, tag_name, LG_MAX_TAG_NAME_LEN - 1);
    node->tag_name[LG_MAX_TAG_NAME_LEN - 1] = '\0';
    /* Parse attributes */
    LgAttr **attr_tail = &node->attributes;
    while (1) {
        lgp_skip_whitespace(p);
        
        /* Check for self-closing or end of tag */
        if (p->pos >= p->lg_len) break;
        
        if (p->html[p->pos] == '>') {
            p->pos++; /* skip > */
            break;
        }
        
        if (p->html[p->pos] == '/' && p->pos + 1 < p->lg_len && p->html[p->pos + 1] == '>') {
            p->pos += 2; /* skip /> */
            return node_idx;
        }
        
        /* Parse attribute */
        LgAttr *attr = lgp_read_attribute(p);
        if (!attr) {
            /* Skip unknown character */
            if (p->pos < p->lg_len && p->html[p->pos] != '>' && 
                !(p->html[p->pos] == '/' && p->pos + 1 < p->lg_len && p->html[p->pos + 1] == '>')) {
                p->pos++;
            } else {
                break;
            }
            continue;
        }
        
        *attr_tail = attr;
        attr_tail = &attr->next;
    }
    
    /* Handle raw content tags (script, style, etc.) */
    if (lg_is_raw_content_tag(node->tag_name)) {
        char end_tag[LG_MAX_TAG_NAME_LEN + 4];
        snprintf(end_tag, sizeof(end_tag), "</%s>", node->tag_name);
        
        char *content = lgp_read_raw_content(p, node->tag_name);
        if (content) {
            node->text_content = content;
            node->text_len = strlen(content);
        }
        
        /* Skip the closing tag */
        size_t end_tag_len = strlen(end_tag);
        if (p->pos + end_tag_len <= p->lg_len) {
            if (strncasecmp(p->html + p->pos, end_tag, end_tag_len) == 0) {
                p->pos += end_tag_len;
            }
        }
        
        return node_idx;
    }
    
    /* Parse children for non-self-closing tags */
    if (!lg_is_self_closing_tag(node->tag_name)) {
        p->nesting_depth++;
        
        while (p->pos < p->lg_len && p->nesting_depth < LG_MAX_NESTING_DEPTH) {
            /* Do NOT skip whitespace here: whitespace between children is
             * meaningful between inline-level siblings (it produces the space
             * in "text <a>link</a> text").  lgp_parse_text collapses it to
             * a single space, and the layout collapses it to nothing at block
             * boundaries. */
            if (p->pos >= p->lg_len) break;

            /* Skip comments */
            if (lgp_match(p, "<!--")) {
                lgp_skip_comment(p);
                continue;
            }
            
            /* Check for closing tag */
            if (p->html[p->pos] == '<' && p->pos + 1 < p->lg_len && p->html[p->pos + 1] == '/') {
                /* Check if it's our closing tag */
                size_t check_pos = p->pos + 2;
                size_t tag_len = strlen(node->tag_name);
                
                if (p->lg_len - check_pos >= tag_len &&
                    strncasecmp(p->html + check_pos, node->tag_name, tag_len) == 0) {
                    /* It's our closing tag - skip it */
                    p->pos = check_pos + tag_len;
                    while (p->pos < p->lg_len && p->html[p->pos] != '>') p->pos++;
                    if (p->pos < p->lg_len) p->pos++;
                    break;
                }
                /* Stray closing tag for a different element: drop it rather
                 * than letting its text leak into the document. */
                p->pos += 2;
                while (p->pos < p->lg_len && p->html[p->pos] != '>') p->pos++;
                if (p->pos < p->lg_len) p->pos++;
                continue;
            }
            
            /* Parse child element or text */
            if (p->html[p->pos] == '<') {
                int child_idx = lgp_parse_element(p, node_idx);
                if (child_idx < 0) {
                    /* Try to recover from parsing error */
                    p->pos++;
                }
            } else {
                int text_idx = lgp_parse_text(p, node_idx);
                (void)text_idx;
            }
            /* Adding children can grow (realloc) the node array: reload. */
            node = lg_node_at(p->document, node_idx);
        }
        
        p->nesting_depth--;
    }
    
    return node_idx;
}

static int lgp_parse_text(LgParser *p, int parent_idx) {
    if (p->pos >= p->lg_len) return -1;
    
    size_t start = p->pos;
    
    while (p->pos < p->lg_len && p->html[p->pos] != '<') {
        p->pos++;
    }
    
    size_t len = p->pos - start;
    if (len == 0) return -1;
    
    /* Collapse whitespace runs to a single space, like CSS white-space:normal.
     * Leading and trailing single spaces are preserved: they are the gaps
     * between inline-level siblings (e.g. "text <a>link</a> text").  A node
     * that is entirely whitespace collapses to a lone space and has no size
     * in layout. */
    char *collapsed = (char*)malloc(len + 1);
    if (!collapsed) return -1;
    size_t cl = 0;
    bool pending_space = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)p->html[start + i];
        if (isspace(ch)) {
            pending_space = true;
            continue;
        }
        if (pending_space) collapsed[cl++] = ' ';
        pending_space = false;
        collapsed[cl++] = (char)ch;
    }
    if (pending_space && cl > 0) collapsed[cl++] = ' ';
    if (cl == 0) collapsed[cl++] = ' ';  /* pure-whitespace node: one space */
    collapsed[cl] = '\0';
    
    int node_idx = lg_node_create(p->document, LG_NODE_TEXT, parent_idx);
    if (node_idx < 0) {
        free(collapsed);
        return -1;
    }
    
    LgNode *node = lg_node_at(p->document, node_idx);
    node->text_content = collapsed;
    node->text_len = cl;
    
    /* Decode entities */
    node->text_len = lg_decode_entities(node->text_content, cl);
    
    return node_idx;
}

LgDoc* lg_parse(const char *html, size_t lg_len) {
    if (!html || lg_len == 0) return NULL;
    
    LgDoc *doc = (LgDoc*)calloc(1, sizeof(LgDoc));
    if (!doc) return NULL;
    
    if (!po_array_init(&doc->array, sizeof(LgNode), 64)) {
        free(doc);
        return NULL;
    }
    doc->root_idx = -1;
    doc->head_idx = -1;
    doc->body_idx = -1;
    doc->current_parent_idx = -1;
    
    LgParser parser = {
        .html = html,
        .lg_len = lg_len,
        .pos = 0,
        .line = 1,
        .column = 1,
        .nesting_depth = 0,
        .document = doc
    };
    
    /* Skip DOCTYPE if present */
    lgp_skip_doctype(&parser);
    
    /* Skip any leading whitespace/comments */
    while (parser.pos < parser.lg_len) {
        lgp_skip_whitespace(&parser);
        if (lgp_match(&parser, "<!--")) {
            lgp_skip_comment(&parser);
        } else {
            break;
        }
    }
    
    /* Parse root elements */
    while (parser.pos < parser.lg_len) {
        lgp_skip_whitespace(&parser);
        
        if (parser.pos >= parser.lg_len) break;
        
        if (lgp_match(&parser, "<!--")) {
            lgp_skip_comment(&parser);
            continue;
        }
        
        if (parser.html[parser.pos] == '<') {
            int node_idx = lgp_parse_element(&parser, -1);
            if (node_idx >= 0) {
                LgNode *node = lg_node_at(doc, node_idx);
                
                /* Track root. The parser produces a single root in normal HTML. */
                if (doc->root_idx < 0) {
                    doc->root_idx = node_idx;
                }
                
                /* Track head and body */
                if (node && node->type == LG_NODE_ELEMENT) {
                    if (strcasecmp(node->tag_name, "head") == 0) {
                        doc->head_idx = node_idx;
                    } else if (strcasecmp(node->tag_name, "body") == 0) {
                        doc->body_idx = node_idx;
                    } else if (strcasecmp(node->tag_name, "html") == 0) {
                        /* Look for head and body in html element children */
                        int child_idx = po_array_first_child(&doc->array, node_idx);
                        while (child_idx >= 0) {
                            LgNode *child = lg_node_at(doc, child_idx);
                            if (child && child->type == LG_NODE_ELEMENT) {
                                if (strcasecmp(child->tag_name, "head") == 0) {
                                    doc->head_idx = child_idx;
                                } else if (strcasecmp(child->tag_name, "body") == 0) {
                                    doc->body_idx = child_idx;
                                }
                            }
                            child_idx = po_array_next_sibling(&doc->array, child_idx);
                        }
                    }
                }
            } else {
                /* Try to recover */
                if (parser.pos < parser.lg_len && parser.html[parser.pos] == '<') {
                    parser.pos++;
                }
            }
        } else {
            /* Text node at root level - usually whitespace, skip it */
            int text_idx = lgp_parse_text(&parser, -1);
            if (text_idx >= 0) {
                LgNode *text = lg_node_at(doc, text_idx);
                lg_node_free_contents(text);
                text->text_content = NULL;
                text->text_len = 0;
                po_array_delete(&doc->array, text_idx);
            }
        }
    }
    
        
    return doc;
}


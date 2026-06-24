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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "html_dom.h"
#include "gc_value_helpers.h"
#include "platform.h"
#include "browser_api_impl.h"
#include "browser_api_impl_handles.h"
#include "browser_api_impl_internal.h"
#include "css_parser.h"

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

static int html_node_create(HtmlDocument *doc, HtmlNodeType type, int parent_idx) {
    int idx = po_array_add(&doc->array, parent_idx);
    if (idx < 0) return -1;

    HtmlNode *node = html_node_at(doc, idx);
    node->type = type;
    node->js_object = JS_UNDEFINED;
    node->has_js_object = 0;
    return idx;
}

static HtmlAttribute* html_attribute_create(const char *name, const char *value) {
    HtmlAttribute *attr = (HtmlAttribute*)calloc(1, sizeof(HtmlAttribute));
    if (!attr) return NULL;
    
    strncpy(attr->name, name, HTML_MAX_ATTR_NAME_LEN - 1);
    attr->name[HTML_MAX_ATTR_NAME_LEN - 1] = '\0';
    
    if (value) {
        strncpy(attr->value, value, HTML_MAX_ATTR_VALUE_LEN - 1);
        attr->value[HTML_MAX_ATTR_VALUE_LEN - 1] = '\0';
        html_decode_entities(attr->value, strlen(attr->value));
    }
    
    return attr;
}

static void html_node_free_contents(HtmlNode *node) {
    if (!node) return;
    
    HtmlAttribute *attr = node->attributes;
    while (attr) {
        HtmlAttribute *next = attr->next;
        free(attr);
        attr = next;
    }
    
    if (node->text_content) {
        free(node->text_content);
    }
    
    /* Note: We don't free js_object here - that's managed by QuickJS GC */
}

void html_document_free(HtmlDocument *doc) {
    if (!doc) return;
    
    size_t n = po_array_count(&doc->array);
    for (size_t i = 0; i < n; i++) {
        HtmlNode *node = html_node_at(doc, (int)i);
        if (node) html_node_free_contents(node);
    }
    
    po_array_free(&doc->array);
    
    if (doc->title) free(doc->title);
    free(doc);
}

/* ============================================================================
 * Parser Implementation
 * ============================================================================ */

static void parser_skip_whitespace(HtmlParser *p) {
    while (p->pos < p->html_len && isspace((unsigned char)p->html[p->pos])) {
        if (p->html[p->pos] == '\n') {
            p->line++;
            p->column = 1;
        } else {
            p->column++;
        }
        p->pos++;
    }
}

static bool parser_match(HtmlParser *p, const char *str) {
    size_t len = strlen(str);
    if (p->pos + len > p->html_len) return false;
    return (strncasecmp(p->html + p->pos, str, len) == 0);
}

static char* parser_read_tag_name(HtmlParser *p) {
    static char tag_name[HTML_MAX_TAG_NAME_LEN];
    size_t i = 0;
    
    /* Skip initial whitespace */
    parser_skip_whitespace(p);
    
    /* Read tag name */
    while (p->pos < p->html_len && i < HTML_MAX_TAG_NAME_LEN - 1) {
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
    html_tag_name_normalize(tag_name);
    
    return i > 0 ? tag_name : NULL;
}

static HtmlAttribute* parser_read_attribute(HtmlParser *p) {
    parser_skip_whitespace(p);
    
    /* Check for end of tag */
    if (p->pos >= p->html_len || p->html[p->pos] == '>' || 
        (p->html[p->pos] == '/' && p->pos + 1 < p->html_len && p->html[p->pos + 1] == '>')) {
        return NULL;
    }
    
    /* Read attribute name */
    char name[HTML_MAX_ATTR_NAME_LEN];
    size_t name_len = 0;
    
    while (p->pos < p->html_len && name_len < HTML_MAX_ATTR_NAME_LEN - 1) {
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
    char value[HTML_MAX_ATTR_VALUE_LEN] = {0};
    
    parser_skip_whitespace(p);
    
    if (p->pos < p->html_len && p->html[p->pos] == '=') {
        p->pos++; /* skip = */
        p->column++;
        
        parser_skip_whitespace(p);
        
        /* Read value */
        if (p->pos < p->html_len) {
            char quote = p->html[p->pos];
            if (quote == '"' || quote == '\'') {
                p->pos++; /* skip opening quote */
                p->column++;
                
                size_t val_len = 0;
                while (p->pos < p->html_len && p->html[p->pos] != quote && 
                       val_len < HTML_MAX_ATTR_VALUE_LEN - 1) {
                    value[val_len++] = p->html[p->pos++];
                    p->column++;
                }
                value[val_len] = '\0';
                
                if (p->pos < p->html_len && p->html[p->pos] == quote) {
                    p->pos++; /* skip closing quote */
                    p->column++;
                }
            } else {
                /* Unquoted value */
                size_t val_len = 0;
                while (p->pos < p->html_len && val_len < HTML_MAX_ATTR_VALUE_LEN - 1) {
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
    
    return html_attribute_create(name, value[0] ? value : NULL);
}

static int parser_parse_element(HtmlParser *p, int parent_idx);
static int parser_parse_text(HtmlParser *p, int parent_idx);

static void parser_skip_comment(HtmlParser *p) {
    if (parser_match(p, "<!--")) {
        p->pos += 4;
        while (p->pos < p->html_len) {
            if (parser_match(p, "-->")) {
                p->pos += 3;
                break;
            }
            p->pos++;
        }
    }
}

static void parser_skip_doctype(HtmlParser *p) {
    if (parser_match(p, "<!doctype") || parser_match(p, "<!DOCTYPE")) {
        p->pos += 9;
        while (p->pos < p->html_len && p->html[p->pos] != '>') {
            p->pos++;
        }
        if (p->pos < p->html_len) p->pos++; /* skip > */
    }
}

static char* parser_read_raw_content(HtmlParser *p, const char *end_tag) {
    size_t start = p->pos;
    size_t end_tag_len = strlen(end_tag);
    
    while (p->pos < p->html_len) {
        if (p->html[p->pos] == '<') {
            /* Check for end tag */
            if (p->pos + 1 < p->html_len && p->html[p->pos + 1] == '/') {
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

static int parser_parse_element(HtmlParser *p, int parent_idx) {
    if (p->pos >= p->html_len || p->html[p->pos] != '<') {
        return -1;
    }
    
    p->pos++; /* skip < */
    
    /* Check for closing tag */
    if (p->pos < p->html_len && p->html[p->pos] == '/') {
        /* Closing tag - let caller handle */
        p->pos--; /* back up */
        return -1;
    }
    
    /* Read tag name */
    char *tag_name = parser_read_tag_name(p);
    if (!tag_name) {
        /* Skip malformed tag */
        while (p->pos < p->html_len && p->html[p->pos] != '>') p->pos++;
        if (p->pos < p->html_len) p->pos++;
        return -1;
    }
    
    /* Create element node */
    int node_idx = html_node_create(p->document, HTML_NODE_ELEMENT, parent_idx);
    if (node_idx < 0) return -1;
    
    HtmlNode *node = html_node_at(p->document, node_idx);
    strncpy(node->tag_name, tag_name, HTML_MAX_TAG_NAME_LEN - 1);
    node->tag_name[HTML_MAX_TAG_NAME_LEN - 1] = '\0';
    
    /* Parse attributes */
    HtmlAttribute **attr_tail = &node->attributes;
    while (1) {
        parser_skip_whitespace(p);
        
        /* Check for self-closing or end of tag */
        if (p->pos >= p->html_len) break;
        
        if (p->html[p->pos] == '>') {
            p->pos++; /* skip > */
            break;
        }
        
        if (p->html[p->pos] == '/' && p->pos + 1 < p->html_len && p->html[p->pos + 1] == '>') {
            p->pos += 2; /* skip /> */
            return node_idx;
        }
        
        /* Parse attribute */
        HtmlAttribute *attr = parser_read_attribute(p);
        if (!attr) {
            /* Skip unknown character */
            if (p->pos < p->html_len && p->html[p->pos] != '>' && 
                !(p->html[p->pos] == '/' && p->pos + 1 < p->html_len && p->html[p->pos + 1] == '>')) {
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
    if (html_is_raw_content_tag(node->tag_name)) {
        char end_tag[HTML_MAX_TAG_NAME_LEN + 4];
        snprintf(end_tag, sizeof(end_tag), "</%s>", node->tag_name);
        
        char *content = parser_read_raw_content(p, node->tag_name);
        if (content) {
            node->text_content = content;
            node->text_len = strlen(content);
        }
        
        /* Skip the closing tag */
        size_t end_tag_len = strlen(end_tag);
        if (p->pos + end_tag_len <= p->html_len) {
            if (strncasecmp(p->html + p->pos, end_tag, end_tag_len) == 0) {
                p->pos += end_tag_len;
            }
        }
        
        return node_idx;
    }
    
    /* Parse children for non-self-closing tags */
    if (!html_is_self_closing_tag(node->tag_name)) {
        p->nesting_depth++;
        
        while (p->pos < p->html_len && p->nesting_depth < HTML_MAX_NESTING_DEPTH) {
            parser_skip_whitespace(p);
            
            if (p->pos >= p->html_len) break;
            
            /* Check for closing tag */
            if (p->html[p->pos] == '<' && p->pos + 1 < p->html_len && p->html[p->pos + 1] == '/') {
                /* Check if it's our closing tag */
                size_t check_pos = p->pos + 2;
                size_t tag_len = strlen(node->tag_name);
                
                if (p->html_len - check_pos >= tag_len &&
                    strncasecmp(p->html + check_pos, node->tag_name, tag_len) == 0) {
                    /* It's our closing tag - skip it */
                    p->pos = check_pos + tag_len;
                    while (p->pos < p->html_len && p->html[p->pos] != '>') p->pos++;
                    if (p->pos < p->html_len) p->pos++;
                    break;
                }
            }
            
            /* Parse child element or text */
            if (p->html[p->pos] == '<') {
                int child_idx = parser_parse_element(p, node_idx);
                if (child_idx < 0) {
                    /* Try to recover from parsing error */
                    p->pos++;
                }
            } else {
                int text_idx = parser_parse_text(p, node_idx);
                (void)text_idx;
            }
        }
        
        p->nesting_depth--;
    }
    
    return node_idx;
}

static int parser_parse_text(HtmlParser *p, int parent_idx) {
    if (p->pos >= p->html_len) return -1;
    
    size_t start = p->pos;
    
    while (p->pos < p->html_len && p->html[p->pos] != '<') {
        p->pos++;
    }
    
    size_t len = p->pos - start;
    if (len == 0) return -1;
    
    /* Trim trailing whitespace but preserve leading for preformatted text */
    while (len > 0 && isspace((unsigned char)p->html[start + len - 1])) {
        len--;
    }
    
    if (len == 0) return -1;
    
    int node_idx = html_node_create(p->document, HTML_NODE_TEXT, parent_idx);
    if (node_idx < 0) return -1;
    
    HtmlNode *node = html_node_at(p->document, node_idx);
    node->text_content = (char*)malloc(len + 1);
    if (!node->text_content) {
        /* Cannot safely remove from tree here; leave as empty text node */
        return node_idx;
    }
    
    memcpy(node->text_content, p->html + start, len);
    node->text_content[len] = '\0';
    node->text_len = len;
    
    /* Decode entities */
    html_decode_entities(node->text_content, len);
    
    return node_idx;
}

HtmlDocument* html_parse(const char *html, size_t html_len) {
    if (!html || html_len == 0) return NULL;
    
    HtmlDocument *doc = (HtmlDocument*)calloc(1, sizeof(HtmlDocument));
    if (!doc) return NULL;
    
    if (!po_array_init(&doc->array, sizeof(HtmlNode), 64)) {
        free(doc);
        return NULL;
    }
    doc->root_idx = -1;
    doc->head_idx = -1;
    doc->body_idx = -1;
    doc->current_parent_idx = -1;
    
    HtmlParser parser = {
        .html = html,
        .html_len = html_len,
        .pos = 0,
        .line = 1,
        .column = 1,
        .nesting_depth = 0,
        .document = doc
    };
    
    /* Skip DOCTYPE if present */
    parser_skip_doctype(&parser);
    
    /* Skip any leading whitespace/comments */
    while (parser.pos < parser.html_len) {
        parser_skip_whitespace(&parser);
        if (parser_match(&parser, "<!--")) {
            parser_skip_comment(&parser);
        } else {
            break;
        }
    }
    
    /* Parse root elements */
    while (parser.pos < parser.html_len) {
        parser_skip_whitespace(&parser);
        
        if (parser.pos >= parser.html_len) break;
        
        if (parser_match(&parser, "<!--")) {
            parser_skip_comment(&parser);
            continue;
        }
        
        if (parser.html[parser.pos] == '<') {
            int node_idx = parser_parse_element(&parser, -1);
            if (node_idx >= 0) {
                HtmlNode *node = html_node_at(doc, node_idx);
                
                /* Track root. The parser produces a single root in normal HTML. */
                if (doc->root_idx < 0) {
                    doc->root_idx = node_idx;
                }
                
                /* Track head and body */
                if (node && node->type == HTML_NODE_ELEMENT) {
                    if (strcasecmp(node->tag_name, "head") == 0) {
                        doc->head_idx = node_idx;
                    } else if (strcasecmp(node->tag_name, "body") == 0) {
                        doc->body_idx = node_idx;
                    } else if (strcasecmp(node->tag_name, "html") == 0) {
                        /* Look for head and body in html element children */
                        int child_idx = po_array_first_child(&doc->array, node_idx);
                        while (child_idx >= 0) {
                            HtmlNode *child = html_node_at(doc, child_idx);
                            if (child && child->type == HTML_NODE_ELEMENT) {
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
                if (parser.pos < parser.html_len && parser.html[parser.pos] == '<') {
                    parser.pos++;
                }
            }
        } else {
            /* Text node at root level - usually whitespace, skip it */
            int text_idx = parser_parse_text(&parser, -1);
            if (text_idx >= 0) {
                HtmlNode *text = html_node_at(doc, text_idx);
                html_node_free_contents(text);
                text->text_content = NULL;
                text->text_len = 0;
                po_array_delete(&doc->array, text_idx);
            }
        }
    }
    
    LOG_INFO("Parsed HTML document: found %s, %s",
             doc->head_idx >= 0 ? "<head>" : "no <head>",
             doc->body_idx >= 0 ? "<body>" : "no <body>");
    
    return doc;
}

/* ============================================================================
 * DOM Node Creation in QuickJS
 * ============================================================================ */

/* Forward declaration from js_quickjs.c */
extern "C" JSClassID js_video_class_id;
extern GCValue js_video_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);

/* Create a JavaScript element for a given tag */
GCValue html_create_element_js(JSContextHandle ctx, const char *tag_name, HtmlAttribute *attrs) {
    if (!tag_name || !ctx) return JS_NULL;
    
    GCValue element;
    
    /* Special handling for video elements */
    if (strcasecmp(tag_name, "video") == 0) {
        extern GCValue js_video_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv);
        element = js_video_constructor(ctx, JS_NULL, 0, NULL);
    } else {
        /* Create generic element with DOMNode backing data so parallel CSS
         * computed-style tables and index tables can attach to it. */
        element = JS_NewObjectClass(ctx, js_dom_node_class_id);
        if (!JS_IsException(element)) {
            DOMNodeHandle node = DOMNodeHandle::create(ctx, DOM_NODE_TYPE_ELEMENT, tag_name);
            if (node.valid()) {
                node.attach_to_object(element);
                /* Copy parsed attributes into the DOMNode so the JS DOM can be
                 * serialized back to HTML without losing information. */
                HtmlAttribute *attr = attrs;
                while (attr) {
                    node.set_attribute(attr->name, attr->value);
                    attr = attr->next;
                }
            }
        }
    }
    
    if (JS_IsException(element)) {
        return JS_NULL;
    }
    
    /* Set tagName property */
    JS_SetPropertyStr(ctx, element, "tagName", JS_NewString(ctx, tag_name));
    
    /* Create attributes map */
    GCValue attr_map = JS_NewObject(ctx);
    
    /* Set id and className if present */
    HtmlAttribute *attr = attrs;
    while (attr) {
        if (strcasecmp(attr->name, "id") == 0) {
            JS_SetPropertyStr(ctx, element, "id", JS_NewString(ctx, attr->value));
        } else if (strcasecmp(attr->name, "class") == 0) {
            JS_SetPropertyStr(ctx, element, "className", JS_NewString(ctx, attr->value));
        }
        
        /* Add to attributes map */
        JS_SetPropertyStr(ctx, attr_map, attr->name, JS_NewString(ctx, attr->value));
        
        attr = attr->next;
    }
    
    JS_SetPropertyStr(ctx, element, "attributes", attr_map);
    
    /* Add childNodes array */
    JS_SetPropertyStr(ctx, element, "childNodes", JS_NewArray(ctx));
    
    /* Special handling for script elements */
    if (strcasecmp(tag_name, "script") == 0) {
        /* Set type property from type attribute (default to "text/javascript") */
        GCValue type_attr = JS_GetPropertyStr(ctx, attr_map, "type");
        if (JS_IsUndefined(type_attr) || JS_IsNull(type_attr)) {
            JS_SetPropertyStr(ctx, element, "type", JS_NewString(ctx, "text/javascript"));
        } else {
            JS_SetPropertyStr(ctx, element, "type", type_attr);
        }
        
        /* Set src property from src attribute if present */
        GCValue src_attr = JS_GetPropertyStr(ctx, attr_map, "src");
        if (!JS_IsUndefined(src_attr) && !JS_IsNull(src_attr)) {
            JS_SetPropertyStr(ctx, element, "src", src_attr);
        }
    }
    
    return element;
}

/* Create a JavaScript element using document.createElement() when available.
 * This ensures elements get proper DOM prototypes (Element, HTMLElement, etc.)
 * rather than plain objects. */
GCValue html_create_element_js_with_document(JSContextHandle ctx, GCValue js_doc, const char *tag_name, HtmlAttribute *attrs) {
    if (!tag_name || !ctx) return JS_NULL;
    
    GCValue element;
    
    /* Try to use document.createElement() for proper DOM prototypes */
    if (!JS_IsUndefined(js_doc) && !JS_IsNull(js_doc)) {
        GCValue createElement = JS_GetPropertyStr(ctx, js_doc, "createElement");
        if (!JS_IsUndefined(createElement) && !JS_IsNull(createElement)) {
            GCValue args[1] = { JS_NewString(ctx, tag_name) };
            element = JS_Call(ctx, createElement, js_doc, 1, args);
            if (!JS_IsException(element) && !JS_IsUndefined(element) && !JS_IsNull(element)) {
                /* Set attributes from parsed HTML.  The inline 'style' attribute is
                 * special-cased: document.createElement already created a style
                 * object, and copying the raw string would clobber it.  We parse
                 * the attribute and apply declarations to the existing object. */
                const char *inline_style = NULL;
                HtmlAttribute *attr = attrs;
                while (attr) {
                    if (strcasecmp(attr->name, "id") == 0) {
                        JS_SetPropertyStr(ctx, element, "id", JS_NewString(ctx, attr->value));
                    } else if (strcasecmp(attr->name, "class") == 0) {
                        JS_SetPropertyStr(ctx, element, "className", JS_NewString(ctx, attr->value));
                    } else if (strcasecmp(attr->name, "style") == 0) {
                        inline_style = attr->value;
                    } else {
                        /* Set other attributes directly */
                        JS_SetPropertyStr(ctx, element, attr->name, JS_NewString(ctx, attr->value));
                    }
                    attr = attr->next;
                }
                if (inline_style && inline_style[0]) {
                    css_apply_inline_style_string(ctx, element, inline_style);
                }
                DOMNodeHandle node = DOMNodeHandle::from_object(element);
                if (node.valid()) {
                    /* Copy parsed attributes into the DOMNode for HTML serialization. */
                    attr = attrs;
                    while (attr) {
                        node.set_attribute(attr->name, attr->value);
                        attr = attr->next;
                    }
                    css_index_insert_node(ctx, node);
                }
                return element;
            }
            /* Fall through to plain object if createElement failed */
        }
    }
    
    /* Fallback to plain object if document.createElement not available */
    element = html_create_element_js(ctx, tag_name, attrs);
    {
        DOMNodeHandle node = DOMNodeHandle::from_object(element);
        if (node.valid()) css_index_insert_node(ctx, node);
    }
    /* Ensure ownerDocument is set even for fallback-created elements. */
    if (!JS_IsUndefined(js_doc) && !JS_IsNull(js_doc)) {
        dom_node_set_owner_document(ctx, element, js_doc);
    }
    return element;
}

/* Recursively create DOM nodes in JS with automatic GC memory management */
static bool html_node_create_js_recursive(JSContextHandle ctx, HtmlDocument *doc, int node_idx, GCValue parent) {
    if (!ctx || !doc || node_idx < 0) return false;
    
    HtmlNode *node = html_node_at(doc, node_idx);
    if (!node) return false;
    
    GCValue js_node = JS_UNDEFINED;
    
    switch (node->type) {
        case HTML_NODE_ELEMENT: {
            js_node = html_create_element_js(ctx, node->tag_name, node->attributes);
            
            /* Process children */
            if (!JS_IsNull(js_node)) {
                int child_idx = po_array_first_child(&doc->array, node_idx);
                while (child_idx >= 0) {
                    html_node_create_js_recursive(ctx, doc, child_idx, js_node);
                    child_idx = po_array_next_sibling(&doc->array, child_idx);
                }
                
                /* For raw content tags (script, style), set textContent from node's text_content */
                if (html_is_raw_content_tag(node->tag_name) && node->text_content) {
                    JS_SetPropertyStr(ctx, js_node, "textContent", 
                                      JS_NewString(ctx, node->text_content));
                }
                
                /* If we have a parent, append this element */
                if (!JS_IsUndefined(parent) && !JS_IsNull(parent)) {
                    /* Note: GCValue uses automatic garbage collection */
                    GCValue appendChild = JS_GetPropertyStr(ctx, parent, "appendChild");
                    
                    if (!JS_IsUndefined(appendChild) && !JS_IsNull(appendChild)) {
                        GCValue args[1] = { js_node };
                        GCValue result = JS_Call(ctx, appendChild, parent, 1, args);
                        (void)result;
                    }
                }
            }
            break;
        }
        
        case HTML_NODE_TEXT: {
            if (node->text_content && strlen(node->text_content) > 0) {
                /* Create text node (as a simple string for now) */
                js_node = JS_NewString(ctx, node->text_content);
                
                /* Add to parent's innerHTML or childNodes if needed */
                if (!JS_IsUndefined(parent) && !JS_IsNull(parent)) {
                    GCValue childNodes = JS_GetPropertyStr(ctx, parent, "childNodes");
                    
                    if (JS_IsArray(ctx, childNodes)) {
                        GCValue push = JS_GetPropertyStr(ctx, childNodes, "push");
                        GCValue args[1] = { js_node };
                        GCValue result = JS_Call(ctx, push, childNodes, 1, args);
                        (void)result;
                    }
                }
            }
            break;
        }
        
        default:
            break;
    }
    
    if (!JS_IsUndefined(js_node)) {
        node->js_object = js_node;
        node->has_js_object = 1;
    }
    
    return true;
}

/* Create JavaScript document object with parsed HTML structure */
GCValue html_create_js_document(JSContextHandle ctx, HtmlDocument *doc) {
    if (!ctx || !doc) return JS_NULL;
    
    GCValue js_doc = JS_NewObject(ctx);
    
    /* Set document properties */
    JS_SetPropertyStr(ctx, js_doc, "nodeType", JS_NewInt32(ctx, 9)); /* DOCUMENT_NODE */
    JS_SetPropertyStr(ctx, js_doc, "readyState", JS_NewString(ctx, "complete"));
    JS_SetPropertyStr(ctx, js_doc, "characterSet", JS_NewString(ctx, "UTF-8"));
    JS_SetPropertyStr(ctx, js_doc, "contentType", JS_NewString(ctx, "text/html"));
    
    /* Create documentElement (html or first root element) */
    GCValue doc_element = JS_NULL;
    HtmlNode *root = html_document_root(doc);
    if (root) {
        doc_element = html_create_element_js(ctx, root->tag_name, root->attributes);
        
        /* Process children of root */
        int child_idx = po_array_first_child(&doc->array, doc->root_idx);
        while (child_idx >= 0) {
            html_node_create_js_recursive(ctx, doc, child_idx, doc_element);
            child_idx = po_array_next_sibling(&doc->array, child_idx);
        }
    } else {
        /* Create a minimal html element */
        doc_element = html_create_element_js(ctx, "html", NULL);
    }
    
    JS_SetPropertyStr(ctx, js_doc, "documentElement", doc_element);
    
    /* Create body element reference */
    GCValue body_element = JS_NULL;
    HtmlNode *body = html_document_body(doc);
    if (body) {
        body_element = html_create_element_js(ctx, body->tag_name, body->attributes);
        
        /* Process body children */
        int child_idx = po_array_first_child(&doc->array, doc->body_idx);
        while (child_idx >= 0) {
            html_node_create_js_recursive(ctx, doc, child_idx, body_element);
            child_idx = po_array_next_sibling(&doc->array, child_idx);
        }
    } else {
        body_element = html_create_element_js(ctx, "body", NULL);
    }
    
    JS_SetPropertyStr(ctx, js_doc, "body", body_element);
    JS_SetPropertyStr(ctx, doc_element, "body", body_element);
    
    /* Create head element reference */
    GCValue head_element = JS_NULL;
    HtmlNode *head = html_document_head(doc);
    if (head) {
        head_element = html_create_element_js(ctx, head->tag_name, head->attributes);
        
        /* Process head children */
        int child_idx = po_array_first_child(&doc->array, doc->head_idx);
        while (child_idx >= 0) {
            html_node_create_js_recursive(ctx, doc, child_idx, head_element);
            child_idx = po_array_next_sibling(&doc->array, child_idx);
        }
    } else {
        head_element = html_create_element_js(ctx, "head", NULL);
    }
    
    JS_SetPropertyStr(ctx, js_doc, "head", head_element);
    JS_SetPropertyStr(ctx, doc_element, "head", head_element);
    
    /* Add document methods */
    /* Note: createElement is provided by js_quickjs.c */
    
    return js_doc;
}

/* Main entry point: parse HTML and create DOM in JS context */
bool html_create_dom_in_js(JSContextHandle ctx, HtmlDocument *doc) {
    if (!ctx || !doc) return false;
    
    LOG_INFO("Creating DOM in JS context");
    
    /* CSS index tables are per-runtime state; clear them for the new doc. */
    css_document_state_clear(JS_GetRuntime(ctx));
    
    /* Create the document object in JS first (not tracked - persistent) */
    GCValue js_doc = html_create_js_document(ctx, doc);
    
    if (JS_IsNull(js_doc) || JS_IsException(js_doc)) {
        LOG_ERROR("Failed to create JS document");
        return false;
    }
    
    /* Note: GCValue uses automatic garbage collection */
    /* Get global object and set document */
    GCValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "document", js_doc);
    
    /* Also set documentElement on window */
    GCValue doc_elem = JS_GetPropertyStr(ctx, js_doc, "documentElement");
    if (!JS_IsNull(doc_elem) && !JS_IsUndefined(doc_elem)) {
        JS_SetPropertyStr(ctx, global, "documentElement", doc_elem);
    }
    
    LOG_INFO("DOM created successfully in JS context");
    return true;
}

/* Recursively populate JS DOM from parsed HTML using document.createElement().
 * This ensures elements get proper DOM prototypes. */
static bool html_node_populate_js_recursive(JSContextHandle ctx, GCValue js_doc, HtmlDocument *doc, int node_idx, GCValue parent) {
    if (!ctx || !doc || node_idx < 0) return false;
    
    HtmlNode *node = html_node_at(doc, node_idx);
    if (!node) return false;
    
    GCValue js_node = JS_UNDEFINED;
    
    switch (node->type) {
        case HTML_NODE_ELEMENT: {
            js_node = html_create_element_js_with_document(ctx, js_doc, node->tag_name, node->attributes);
            
            /* Process children */
            if (!JS_IsNull(js_node) && !JS_IsUndefined(js_node)) {
                int child_idx = po_array_first_child(&doc->array, node_idx);
                while (child_idx >= 0) {
                    html_node_populate_js_recursive(ctx, js_doc, doc, child_idx, js_node);
                    child_idx = po_array_next_sibling(&doc->array, child_idx);
                }
                
                /* For raw content tags (script, style), set textContent */
                if (html_is_raw_content_tag(node->tag_name) && node->text_content) {
                    JS_SetPropertyStr(ctx, js_node, "textContent", 
                                      JS_NewString(ctx, node->text_content));
                }
                
                /* Remember the JS object so the CSS applier can find it. */
                node->js_object = js_node;
                node->has_js_object = 1;

                /* If we have a parent, append this element */
                if (!JS_IsUndefined(parent) && !JS_IsNull(parent)) {
                    GCValue appendChild = JS_GetPropertyStr(ctx, parent, "appendChild");
                    GCValue args[1] = { js_node };
                    if (JS_IsFunction(ctx, appendChild)) {
                        JS_Call(ctx, appendChild, parent, 1, args);
                    } else {
                        js_node_appendChild_real(ctx, parent, 1, args);
                    }
                }
            }
            break;
        }
        
        case HTML_NODE_TEXT: {
            if (node->text_content && strlen(node->text_content) > 0) {
                /* Try to create text node via document.createTextNode */
                GCValue createTextNode = JS_GetPropertyStr(ctx, js_doc, "createTextNode");
                if (!JS_IsUndefined(createTextNode) && !JS_IsNull(createTextNode)) {
                    GCValue args[1] = { JS_NewString(ctx, node->text_content) };
                    js_node = JS_Call(ctx, createTextNode, js_doc, 1, args);
                } else {
                    js_node = JS_NewString(ctx, node->text_content);
                }
                
                if (!JS_IsUndefined(parent) && !JS_IsNull(parent)) {
                    GCValue appendChild = JS_GetPropertyStr(ctx, parent, "appendChild");
                    GCValue args[1] = { js_node };
                    if (JS_IsFunction(ctx, appendChild)) {
                        JS_Call(ctx, appendChild, parent, 1, args);
                    } else {
                        js_node_appendChild_real(ctx, parent, 1, args);
                    }
                }
            }
            break;
        }
        
        default:
            break;
    }
    
    return true;
}

/* Helper: copy attributes from parsed HTML node to JS element */
static void html_copy_attributes_to_js(JSContextHandle ctx, GCValue elem, HtmlAttribute *attrs) {
    HtmlAttribute *attr = attrs;
    while (attr) {
        if (attr->value[0]) {
            JS_SetPropertyStr(ctx, elem, attr->name, JS_NewString(ctx, attr->value));
        }
        attr = attr->next;
    }
}

/* Helper: transfer numeric properties and style from old element to new element */
static void html_transfer_element_properties(JSContextHandle ctx, GCValue new_elem, GCValue old_elem) {
    if (JS_IsUndefined(old_elem) || JS_IsNull(old_elem)) return;
    
    /* Transfer dimension properties */
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
    
    /* Merge style object properties from old element into new element's style */
    GCValue old_style = JS_GetPropertyStr(ctx, old_elem, "style");
    GCValue new_style = JS_GetPropertyStr(ctx, new_elem, "style");
    if (!JS_IsUndefined(old_style) && !JS_IsNull(old_style) &&
        !JS_IsUndefined(new_style) && !JS_IsNull(new_style)) {
        JSPropertyEnum *tab = NULL;
        uint32_t plen = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &plen, old_style, JS_GPN_STRING_MASK) == 0 && tab) {
            for (uint32_t i = 0; i < plen; i++) {
                const char *key = JS_AtomToCString(ctx, tab[i].atom);
                if (key && strcmp(key, "removeProperty") != 0 &&
                    strcmp(key, "setProperty") != 0 &&
                    strcmp(key, "getPropertyValue") != 0) {
                    GCValue val = JS_GetPropertyStr(ctx, old_style, key);
                    JS_SetPropertyStr(ctx, new_style, key, val);
                }
            }
            JS_FreePropertyEnum(ctx, tab, plen);
        }
    }
}

/* Populate an existing JS document object from parsed HTML.
 * Creates NEW documentElement, head, and body from the parsed HTML tags,
 * so document.head and document.body are the actual parsed elements.
 * Critical properties from the old skeleton are transferred to the new elements. */
bool html_populate_js_document(JSContextHandle ctx, GCValue js_doc, HtmlDocument *doc) {
    if (!ctx || !doc) return false;
    if (JS_IsUndefined(js_doc) || JS_IsNull(js_doc)) return false;
    
    /* The CSS index tables are per-runtime state; clear them before populating
     * a new document so lookups do not return stale nodes. */
    css_document_state_clear(JS_GetRuntime(ctx));
    
    LOG_INFO("Populating JS document from parsed HTML");
    
    /* Get existing documentElement, body, head (the hardcoded skeleton) */
    GCValue old_doc_element = JS_GetPropertyStr(ctx, js_doc, "documentElement");
    GCValue old_body_element = JS_GetPropertyStr(ctx, js_doc, "body");
    GCValue old_head_element = JS_GetPropertyStr(ctx, js_doc, "head");
    
    HtmlNode *root = html_document_root(doc);
    HtmlNode *head = html_document_head(doc);
    HtmlNode *body = html_document_body(doc);
    
    /* Create NEW elements from the parsed HTML tags */
    GCValue new_doc_element = html_create_element_js_with_document(
        ctx, js_doc,
        root ? root->tag_name : "html",
        root ? root->attributes : NULL);
    
    GCValue new_head_element = html_create_element_js_with_document(
        ctx, js_doc,
        head ? head->tag_name : "head",
        head ? head->attributes : NULL);
    
    GCValue new_body_element = html_create_element_js_with_document(
        ctx, js_doc,
        body ? body->tag_name : "body",
        body ? body->attributes : NULL);
    
    /* Ensure the root elements point back to the document. */
    dom_node_set_owner_document(ctx, new_doc_element, js_doc);
    dom_node_set_owner_document(ctx, new_head_element, js_doc);
    dom_node_set_owner_document(ctx, new_body_element, js_doc);
    
    /* Attach the root <html> element to the document so parentNode / isConnected work. */
    {
        GCValue args[1] = { new_doc_element };
        js_node_appendChild_real(ctx, js_doc, 1, args);
    }
    
    /* Transfer critical properties (dimensions, style) from old skeleton */
    html_transfer_element_properties(ctx, new_doc_element, old_doc_element);
    html_transfer_element_properties(ctx, new_body_element, old_body_element);
    html_transfer_element_properties(ctx, new_head_element, old_head_element);
    
    /* Populate new head with parsed head children */
    if (doc->head_idx >= 0) {
        int child_idx = po_array_first_child(&doc->array, doc->head_idx);
        while (child_idx >= 0) {
            html_node_populate_js_recursive(ctx, js_doc, doc, child_idx, new_head_element);
            child_idx = po_array_next_sibling(&doc->array, child_idx);
        }
    }
    
    /* Populate new body with parsed body children */
    if (doc->body_idx >= 0) {
        int child_idx = po_array_first_child(&doc->array, doc->body_idx);
        while (child_idx >= 0) {
            html_node_populate_js_recursive(ctx, js_doc, doc, child_idx, new_body_element);
            child_idx = po_array_next_sibling(&doc->array, child_idx);
        }
    }
    
    /* Append head and body to documentElement */
    {
        GCValue appendChild = JS_GetPropertyStr(ctx, new_doc_element, "appendChild");
        if (!JS_IsUndefined(appendChild) && !JS_IsNull(appendChild)) {
            GCValue args1[1] = { new_head_element };
            JS_Call(ctx, appendChild, new_doc_element, 1, args1);
            GCValue args2[1] = { new_body_element };
            JS_Call(ctx, appendChild, new_doc_element, 1, args2);
        }
    }
    
    /* Add other children of parsed root to documentElement (skip head/body) */
    if (doc->root_idx >= 0) {
        int child_idx = po_array_first_child(&doc->array, doc->root_idx);
        while (child_idx >= 0) {
            HtmlNode *child = html_node_at(doc, child_idx);
            if (child && child->type == HTML_NODE_ELEMENT) {
                if (strcasecmp(child->tag_name, "head") == 0 ||
                    strcasecmp(child->tag_name, "body") == 0) {
                    child_idx = po_array_next_sibling(&doc->array, child_idx);
                    continue;
                }
            }
            html_node_populate_js_recursive(ctx, js_doc, doc, child_idx, new_doc_element);
            child_idx = po_array_next_sibling(&doc->array, child_idx);
        }
    }
    
    /* Replace old elements with new ones in the document */
    JS_SetPropertyStr(ctx, js_doc, "documentElement", new_doc_element);
    JS_SetPropertyStr(ctx, js_doc, "head", new_head_element);
    JS_SetPropertyStr(ctx, js_doc, "body", new_body_element);
    JS_SetPropertyStr(ctx, new_doc_element, "head", new_head_element);
    JS_SetPropertyStr(ctx, new_doc_element, "body", new_body_element);
    
    LOG_INFO("JS document populated: new docElem/head/body from parsed HTML");
    return true;
}

/* Set the innerHTML of an element by parsing the HTML string and replacing
 * the element's children with the parsed nodes. Uses document.createElement()
 * from the element's ownerDocument so nodes get proper prototypes. */
bool html_element_set_inner_html(JSContextHandle ctx, GCValue elem, const char *html) {
    if (!ctx || JS_IsUndefined(elem) || JS_IsNull(elem) || !html) return false;

    // Templates store their children in the content DocumentFragment, not in
    // the template element itself.
    bool is_template = false;
    GCValue tag_val = JS_GetPropertyStr(ctx, elem, "tagName");
    if (JS_IsString(tag_val)) {
        const char *tag_str = JS_ToCString(ctx, tag_val);
        if (tag_str && strcasecmp(tag_str, "template") == 0) {
            is_template = true;
        }
    }
    GCValue target = elem;
    if (is_template) {
        GCValue content = JS_GetPropertyStr(ctx, elem, "content");
        if (!JS_IsUndefined(content) && !JS_IsNull(content) && JS_IsObject(content)) {
            target = content;
        }
    }

    GCValue doc = JS_GetPropertyStr(ctx, target, "ownerDocument");
    if (JS_IsUndefined(doc) || JS_IsNull(doc)) {
        GCValue global = JS_GetGlobalObject(ctx);
        doc = JS_GetPropertyStr(ctx, global, "document");
    }

    HtmlDocument *frag_doc = html_parse(html, strlen(html));
    if (!frag_doc) return false;

    // Remove existing children from the target (template content or element)
    DOMNodeHandle target_node = DOMNodeHandle::from_object(target);
    if (target_node.valid()) {
        GCValue child = target_node.first_child();
        while (!JS_IsNull(child)) {
            DOMNodeHandle child_node = DOMNodeHandle::from_object(child);
            GCValue next = child_node.valid() ? child_node.next_sibling() : JS_NULL;
            GCValue remove_args[1] = { child };
            js_node_removeChild_real(ctx, target, 1, remove_args);
            child = next;
        }
    }

    // Append parsed children. For full documents use <body>; otherwise append the
    // root-level fragment node(s) directly (e.g. innerHTML = '<div>...</div>').
    int start_idx = -1;
    if (frag_doc->body_idx >= 0) {
        start_idx = po_array_first_child(&frag_doc->array, frag_doc->body_idx);
    }
    if (start_idx < 0 && frag_doc->root_idx >= 0) {
        start_idx = frag_doc->root_idx;
    }

    int child_idx = start_idx;
    while (child_idx >= 0) {
        html_node_populate_js_recursive(ctx, doc, frag_doc, child_idx, target);
        child_idx = po_array_next_sibling(&frag_doc->array, child_idx);
    }

    // If the parser produced no nodes (e.g. plain text), append a text node.
    if (start_idx < 0 && html[0]) {
        GCValue createTextNode = JS_GetPropertyStr(ctx, doc, "createTextNode");
        if (!JS_IsUndefined(createTextNode) && !JS_IsNull(createTextNode)) {
            GCValue args[1] = { JS_NewString(ctx, html) };
            GCValue text_node = JS_Call(ctx, createTextNode, doc, 1, args);
            if (!JS_IsException(text_node) && !JS_IsUndefined(text_node) && !JS_IsNull(text_node)) {
                GCValue appendChild = JS_GetPropertyStr(ctx, target, "appendChild");
                if (!JS_IsUndefined(appendChild) && !JS_IsNull(appendChild)) {
                    GCValue aargs[1] = { text_node };
                    JS_Call(ctx, appendChild, target, 1, aargs);
                }
            }
        }
    }

    html_document_free(frag_doc);
    return true;
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
 * JS DOM serialization - rebuild native HTML from the JS DOM tree
 * ============================================================================
 * The browser maintains two DOM trees: the native HtmlDocument used by the
 * layout engine and the JavaScript DOM mutated by page scripts.  These helpers
 * serialize the JS DOM back to HTML so we can re-parse it into a fresh native
 * document after mutations occur.
 */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} SerializeBuffer;

static bool serialize_buf_init(SerializeBuffer *buf) {
    if (!buf) return false;
    buf->data = (char *)malloc(4096);
    if (!buf->data) return false;
    buf->data[0] = '\0';
    buf->len = 0;
    buf->cap = 4096;
    return true;
}

static void serialize_buf_free(SerializeBuffer *buf) {
    if (!buf) return;
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
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

static void serialize_buf_append_escaped(SerializeBuffer *buf, const char *s) {
    if (!s) return;
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '&': serialize_buf_append(buf, "&amp;"); break;
            case '<': serialize_buf_append(buf, "&lt;"); break;
            case '>': serialize_buf_append(buf, "&gt;"); break;
            case '"': serialize_buf_append(buf, "&quot;"); break;
            case '\'': serialize_buf_append(buf, "&#39;"); break;
            default:
                if ((unsigned char)*p < 0x20 && !isspace((unsigned char)*p)) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "&#%d;", (unsigned char)*p);
                    serialize_buf_append(buf, tmp);
                } else {
                    char tmp[2] = { *p, '\0' };
                    serialize_buf_append(buf, tmp);
                }
                break;
        }
    }
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

/* Return true if the attribute name is already present in the DOMNode table. */
static bool dom_node_has_attribute(DOMNodeHandle node, const char *name) {
    if (!node.valid() || !name) return false;
    for (int i = 0; i < node.attribute_count(); i++) {
        if (strcasecmp(node.attributes()[i].name, name) == 0) return true;
    }
    return false;
}

static void html_serialize_js_node_internal(JSContextHandle ctx, GCValue node, SerializeBuffer *buf);

static void html_serialize_js_element(JSContextHandle ctx, DOMNodeHandle node, GCValue node_val, SerializeBuffer *buf) {
    const char *tag = node.node_name();
    if (!tag || !tag[0]) tag = "div";
    
    /* Normalize tag name to lowercase for HTML serialization. */
    char tag_lower[64];
    size_t tag_len = strlen(tag);
    if (tag_len >= sizeof(tag_lower)) tag_len = sizeof(tag_lower) - 1;
    for (size_t i = 0; i < tag_len; i++) tag_lower[i] = (char)tolower((unsigned char)tag[i]);
    tag_lower[tag_len] = '\0';
    
    serialize_buf_append(buf, "<");
    serialize_buf_append(buf, tag_lower);
    
    /* Emit attributes from the internal DOMNode attribute table. */
    for (int i = 0; i < node.attribute_count(); i++) {
        const char *name = node.attributes()[i].name;
        const char *value = node.attributes()[i].value;
        if (!name || !name[0]) continue;
        serialize_buf_append_attr(buf, name, value ? value : "");
    }
    
    /* Catch attributes that were set as JS properties but not reflected into
     * the DOMNode table (e.g. direct property assignments by scripts). */
    struct { const char *prop; const char *attr; } extra_attrs[] = {
        { "id", "id" },
        { "className", "class" },
        { "class", "class" },
        { "src", "src" },
        { "href", "href" },
        { "style", "style" },
        { "alt", "alt" },
        { "title", "title" },
        { "width", "width" },
        { "height", "height" },
        { "type", "type" },
        { "value", "value" },
        { "name", "name" },
        { "placeholder", "placeholder" },
        { NULL, NULL }
    };
    for (int i = 0; extra_attrs[i].prop; i++) {
        if (dom_node_has_attribute(node, extra_attrs[i].attr)) continue;
        char *value = js_object_get_string(ctx, node_val, extra_attrs[i].prop);
        if (value && value[0]) {
            serialize_buf_append_attr(buf, extra_attrs[i].attr, value);
        }
        free(value);
    }
    
    bool is_void = html_is_self_closing_tag(tag_lower);
    bool is_raw = html_is_raw_content_tag(tag_lower);
    
    if (is_void) {
        serialize_buf_append(buf, ">");
        return;
    }
    
    serialize_buf_append(buf, ">");
    
    if (is_raw) {
        /* For raw tags emit text content directly without escaping. */
        GCValue tc = JS_GetPropertyStr(ctx, node_val, "textContent");
        const char *text = JS_ToCString(ctx, tc);
        if (text) {
            serialize_buf_append(buf, text);
        }
    } else {
        /* Recurse into children. */
        GCValue child = node.first_child();
        while (!JS_IsNull(child)) {
            html_serialize_js_node_internal(ctx, child, buf);
            DOMNodeHandle child_node = get_dom_node(ctx, child);
            if (!child_node.valid()) break;
            child = child_node.next_sibling();
        }

        /* Serialize open shadow roots so Polymer-stamped content survives the
         * JS DOM -> native DOM round-trip. */
        GCValue shadow = JS_GetPropertyStr(ctx, node_val, "shadowRoot");
        if (!JS_IsUndefined(shadow) && !JS_IsNull(shadow) && JS_IsObject(shadow)) {
            GCValue shadow_child = JS_GetPropertyStr(ctx, shadow, "firstChild");
            while (!JS_IsUndefined(shadow_child) && !JS_IsNull(shadow_child)) {
                html_serialize_js_node_internal(ctx, shadow_child, buf);
                DOMNodeHandle shadow_child_node = get_dom_node(ctx, shadow_child);
                if (!shadow_child_node.valid()) break;
                shadow_child = shadow_child_node.next_sibling();
            }
        }
    }
    
    serialize_buf_append(buf, "</");
    serialize_buf_append(buf, tag_lower);
    serialize_buf_append(buf, ">");
}

static void html_serialize_js_node_internal(JSContextHandle ctx, GCValue node, SerializeBuffer *buf) {
    if (JS_IsUndefined(node) || JS_IsNull(node)) return;
    
    /* Plain string children may exist from older DOM creation paths. */
    if (JS_IsString(node)) {
        const char *s = JS_ToCString(ctx, node);
        if (s) serialize_buf_append_escaped(buf, s);
        return;
    }
    
    DOMNodeHandle dom_node = get_dom_node(ctx, node);
    if (!dom_node.valid()) {
        /* Unknown object child: try to serialize children recursively. */
        GCValue first = JS_GetPropertyStr(ctx, node, "firstChild");
        if (!JS_IsUndefined(first) && !JS_IsNull(first)) {
            html_serialize_js_node_internal(ctx, first, buf);
        }
        return;
    }
    
    int node_type = dom_node.node_type();
    if (node_type == DOM_NODE_TYPE_TEXT) {
        const char *val = dom_node.node_value();
        serialize_buf_append_escaped(buf, val ? val : "");
        return;
    }
    if (node_type == DOM_NODE_TYPE_COMMENT) {
        return; /* Skip comments in serialization. */
    }
    if (node_type == DOM_NODE_TYPE_ELEMENT) {
        html_serialize_js_element(ctx, dom_node, node, buf);
    }
}

/* Serialize a JS DOM node (and its descendants) to a heap-allocated HTML
 * string. The caller must free the returned pointer. */
char *html_serialize_js_node(JSContextHandle ctx, GCValue node) {
    SerializeBuffer buf;
    if (!serialize_buf_init(&buf)) return NULL;
    html_serialize_js_node_internal(ctx, node, &buf);
    char *result = strdup(buf.data);
    serialize_buf_free(&buf);
    return result;
}

/* Build a fresh native HtmlDocument from the JS document's current DOM. */
HtmlDocument *html_document_from_js_dom(JSContextHandle ctx, GCValue js_doc) {
    if (!ctx) return NULL;
    
    GCValue doc_elem = JS_NULL;
    if (!JS_IsUndefined(js_doc) && !JS_IsNull(js_doc)) {
        doc_elem = JS_GetPropertyStr(ctx, js_doc, "documentElement");
    }
    if (JS_IsUndefined(doc_elem) || JS_IsNull(doc_elem)) {
        /* Fallback: look for documentElement on the global object. */
        GCValue global = JS_GetGlobalObject(ctx);
        doc_elem = JS_GetPropertyStr(ctx, global, "documentElement");
    }
    if (JS_IsUndefined(doc_elem) || JS_IsNull(doc_elem)) {
        return NULL;
    }
    
    char *inner = html_serialize_js_node(ctx, doc_elem);
    if (!inner) return NULL;
    
    size_t html_len = strlen(inner) + 32;
    char *html = (char *)malloc(html_len);
    if (!html) {
        free(inner);
        return NULL;
    }
    snprintf(html, html_len, "<!doctype html>\n%s", inner);
    free(inner);
    
    HtmlDocument *doc = html_parse(html, strlen(html));
    free(html);
    return doc;
}

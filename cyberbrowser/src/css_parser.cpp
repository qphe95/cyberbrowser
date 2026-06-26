/*
 * CSS Parser - Implementation
 *
 * A small, self-contained CSS parser for applying styles to the DOM tree.
 */

#include "css_parser.h"
#include "http_download.h"
#include "platform.h"
#include "url_utils.h"
#include "js_quickjs.h"
#include "browser_api_impl.h"
#include "browser_api_impl_handles.h"
#include "html_dom.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#define LOG_TAG "css_parser"
#define LOG_INFO(...) platform_log(LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)
#define LOG_WARN(...) platform_log(LOG_LEVEL_WARN, LOG_TAG, __VA_ARGS__)
#define LOG_ERROR(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)

/* ============================================================================
 * String helpers
 * ============================================================================ */

static bool css_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static void css_skip_space(const char *s, size_t len, size_t *pos) {
    while (*pos < len && css_is_space(s[*pos])) (*pos)++;
}

static void css_skip_space_and_comments(const char *s, size_t len, size_t *pos) {
    while (*pos < len) {
        css_skip_space(s, len, pos);
        if (*pos + 1 < len && s[*pos] == '/' && s[*pos + 1] == '*') {
            *pos += 2;
            while (*pos < len) {
                if (*pos + 1 < len && s[*pos] == '*' && s[*pos + 1] == '/') {
                    *pos += 2;
                    break;
                }
                (*pos)++;
            }
            continue;
        }
        break;
    }
}

static char* css_strndup_trim(const char *s, size_t n) {
    while (n > 0 && css_is_space(s[0])) { s++; n--; }
    while (n > 0 && css_is_space(s[n - 1])) n--;
    char *out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static void css_str_tolower(char *s) {
    for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p);
}

/* Copy at most dst_size-1 bytes, lowercasing. */
static void css_strncpy_lower(char *dst, const char *src, size_t n, size_t dst_size) {
    size_t i;
    for (i = 0; i < n && i + 1 < dst_size && src[i]; i++) {
        dst[i] = (char)tolower((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

/* Convert a CSS property like "background-color" to "backgroundColor". */
char* css_to_camel_case(const char *prop) {
    size_t len = strlen(prop);
    char *out = (char*)malloc(len + 1);
    if (!out) return NULL;
    size_t j = 0;
    bool upper_next = false;
    for (size_t i = 0; i < len; i++) {
        char c = prop[i];
        if (c == '-') {
            upper_next = true;
            continue;
        }
        if (upper_next) {
            out[j++] = (char)toupper((unsigned char)c);
            upper_next = false;
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
    return out;
}

/* ============================================================================
 * Declarations
 * ============================================================================ */

static bool css_declaration_add(CssRule *rule, const char *prop, size_t prop_len,
                                const char *value, size_t value_len) {
    char *p = css_strndup_trim(prop, prop_len);
    char *v = css_strndup_trim(value, value_len);
    if (!p || !v) {
        free(p); free(v);
        return false;
    }
    if (p[0] == '\0' || v[0] == '\0') {
        free(p); free(v);
        return false;
    }
    css_str_tolower(p);

    if (rule->declaration_count >= rule->declaration_capacity) {
        int new_cap = rule->declaration_capacity ? rule->declaration_capacity * 2 : 4;
        CssDeclaration *new_decls = (CssDeclaration*)realloc(rule->declarations,
                                                             new_cap * sizeof(CssDeclaration));
        if (!new_decls) {
            free(p); free(v);
            return false;
        }
        rule->declarations = new_decls;
        rule->declaration_capacity = new_cap;
    }
    rule->declarations[rule->declaration_count].property = p;
    rule->declarations[rule->declaration_count].value = v;
    rule->declaration_count++;
    return true;
}

/* Parse a declaration block between { and }. pos should point just past '{'. */
static void css_parse_declaration_block(const char *s, size_t len, size_t *pos,
                                        CssRule *rule) {
    while (*pos < len) {
        css_skip_space_and_comments(s, len, pos);
        if (*pos >= len) break;
        if (s[*pos] == '}') { (*pos)++; break; }

        /* Read property until ':'. */
        size_t prop_start = *pos;
        bool in_quote = false;
        char quote_char = 0;
        while (*pos < len) {
            char c = s[*pos];
            if (!in_quote) {
                if (c == '"' || c == '\'') { in_quote = true; quote_char = c; }
                else if (c == ':') break;
                else if (c == '}' || c == ';') break;
            } else {
                if (c == quote_char) in_quote = false;
                else if (c == '\\' && *pos + 1 < len) (*pos)++;
            }
            (*pos)++;
        }
        size_t prop_end = *pos;
        if (*pos < len && s[*pos] == ':') {
            (*pos)++;
            /* Read value until ';' or '}', respecting quotes and balanced
             * parentheses so data URLs such as url(data:image/png;base64,...)
             * are not truncated at the semicolon. */
            css_skip_space(s, len, pos);
            size_t val_start = *pos;
            in_quote = false; quote_char = 0;
            int paren_depth = 0;
            while (*pos < len) {
                char c = s[*pos];
                if (!in_quote) {
                    if (c == '"' || c == '\'') { in_quote = true; quote_char = c; }
                    else if (c == '(') { paren_depth++; }
                    else if (c == ')') { if (paren_depth > 0) paren_depth--; }
                    else if (paren_depth == 0 && (c == ';' || c == '}')) break;
                } else {
                    if (c == quote_char) in_quote = false;
                    else if (c == '\\' && *pos + 1 < len) (*pos)++;
                }
                (*pos)++;
            }
            size_t val_end = *pos;
            css_declaration_add(rule, s + prop_start, prop_end - prop_start,
                                s + val_start, val_end - val_start);
            if (*pos < len && s[*pos] == ';') (*pos)++;
        } else {
            /* Malformed declaration, skip to next ';' or '}'. */
            while (*pos < len && s[*pos] != ';' && s[*pos] != '}') (*pos)++;
            if (*pos < len && s[*pos] == ';') (*pos)++;
        }
    }
}

CssDeclaration* css_parse_inline_style(const char *style_attr, int *out_count) {
    if (!style_attr || !out_count) return NULL;
    *out_count = 0;
    size_t len = strlen(style_attr);
    if (len == 0) return NULL;

    CssDeclaration *decls = NULL;
    int count = 0;
    int cap = 0;
    size_t pos = 0;

    while (pos < len) {
        css_skip_space(style_attr, len, &pos);
        if (pos >= len) break;

        size_t prop_start = pos;
        while (pos < len && style_attr[pos] != ':' && style_attr[pos] != ';') pos++;
        size_t prop_end = pos;
        if (pos < len && style_attr[pos] == ':') {
            pos++;
            css_skip_space(style_attr, len, &pos);
            size_t val_start = pos;
            int paren_depth = 0;
            while (pos < len) {
                char c = style_attr[pos];
                if (c == '(') { paren_depth++; }
                else if (c == ')') { if (paren_depth > 0) paren_depth--; }
                else if (paren_depth == 0 && c == ';') break;
                pos++;
            }
            size_t val_end = pos;

            char *p = css_strndup_trim(style_attr + prop_start, prop_end - prop_start);
            char *v = css_strndup_trim(style_attr + val_start, val_end - val_start);
            if (p && v && p[0] && v[0]) {
                css_str_tolower(p);
                if (count >= cap) {
                    cap = cap ? cap * 2 : 4;
                    CssDeclaration *new_decls = (CssDeclaration*)realloc(decls, cap * sizeof(CssDeclaration));
                    if (!new_decls) { free(p); free(v); break; }
                    decls = new_decls;
                }
                decls[count].property = p;
                decls[count].value = v;
                count++;
            } else {
                free(p); free(v);
            }
            if (pos < len && style_attr[pos] == ';') pos++;
        } else {
            while (pos < len && style_attr[pos] != ';') pos++;
            if (pos < len && style_attr[pos] == ';') pos++;
        }
    }

    *out_count = count;
    return decls;
}

void css_declarations_free(CssDeclaration *decls, int count) {
    if (!decls) return;
    for (int i = 0; i < count; i++) {
        free(decls[i].property);
        free(decls[i].value);
    }
    free(decls);
}

/* ============================================================================
 * Stylesheet parser
 * ============================================================================ */

static CssRule* css_stylesheet_add_rule(CssStylesheet *sheet) {
    if (sheet->rule_count >= sheet->rule_capacity) {
        int new_cap = sheet->rule_capacity ? sheet->rule_capacity * 2 : 16;
        CssRule *new_rules = (CssRule*)realloc(sheet->rules, new_cap * sizeof(CssRule));
        if (!new_rules) return NULL;
        sheet->rules = new_rules;
        sheet->rule_capacity = new_cap;
    }
    CssRule *rule = &sheet->rules[sheet->rule_count++];
    memset(rule, 0, sizeof(*rule));
    return rule;
}

/* Read a selector until the first unquoted '{'. Returns NULL if none found. */
static char* css_read_selector(const char *s, size_t len, size_t *pos) {
    css_skip_space_and_comments(s, len, pos);
    size_t start = *pos;
    bool in_quote = false;
    char quote_char = 0;
    while (*pos < len) {
        char c = s[*pos];
        if (!in_quote) {
            if (c == '"' || c == '\'') { in_quote = true; quote_char = c; }
            else if (c == '{') break;
            else if (c == '}') break; /* malformed */
        } else {
            if (c == quote_char) in_quote = false;
            else if (c == '\\' && *pos + 1 < len) (*pos)++;
        }
        (*pos)++;
    }
    if (*pos >= len || s[*pos] != '{') return NULL;
    return css_strndup_trim(s + start, *pos - start);
}

/* Parse an at-rule. Block-form at-rules (e.g. @media, @supports) contain
 * regular style rules; we extract those rules so layout can use them.
 * Statement at-rules (e.g. @import) are skipped. */
static void css_parse_at_rule(const char *s, size_t len, size_t *pos, CssStylesheet *sheet) {
    /* Skip '@' and identifier. */
    (*pos)++;
    while (*pos < len && (isalnum((unsigned char)s[*pos]) || s[*pos] == '-')) (*pos)++;
    css_skip_space_and_comments(s, len, pos);
    if (*pos < len && s[*pos] == '{') {
        /* Capture the media/supports condition before consuming the block. */
        size_t cond_start = *pos;
        /* condition starts after any leading space and ends at the '{' */
        size_t cond_end = *pos;
        while (cond_start > 0 && css_is_space(s[cond_start - 1])) cond_start--;
        char *media_cond = css_strndup_trim(s + cond_start, cond_end - cond_start);

        /* Block-form at-rule: parse nested rules. */
        (*pos)++;
        while (*pos < len) {
            css_skip_space_and_comments(s, len, pos);
            if (*pos >= len) break;
            if (s[*pos] == '}') { (*pos)++; break; }
            if (s[*pos] == '@') {
                css_parse_at_rule(s, len, pos, sheet);
                continue;
            }
            char *selector = css_read_selector(s, len, pos);
            if (!selector) {
                /* Could be malformed; try to recover. */
                (*pos)++;
                continue;
            }
            if (*pos >= len || s[*pos] != '{') {
                free(selector);
                free(media_cond);
                continue;
            }
            (*pos)++; /* skip '{' */
            CssRule *rule = css_stylesheet_add_rule(sheet);
            if (!rule) {
                free(selector);
                free(media_cond);
                break;
            }
            rule->selector_text = selector;
            rule->media_query = media_cond ? strdup(media_cond) : NULL;
            css_parse_declaration_block(s, len, pos, rule);
        }
        free(media_cond);
    } else {
        /* Statement form; skip until ';' or block end. */
        while (*pos < len && s[*pos] != ';' && s[*pos] != '}') (*pos)++;
        if (*pos < len && s[*pos] == ';') (*pos)++;
    }
}

CssStylesheet* css_stylesheet_parse(const char *css, size_t len) {
    if (!css || len == 0) return NULL;
    CssStylesheet *sheet = (CssStylesheet*)calloc(1, sizeof(CssStylesheet));
    if (!sheet) return NULL;

    size_t pos = 0;
    while (pos < len) {
        css_skip_space_and_comments(css, len, &pos);
        if (pos >= len) break;

        if (css[pos] == '@') {
            css_parse_at_rule(css, len, &pos, sheet);
            continue;
        }
        if (css[pos] == '}') { pos++; continue; }

        char *selector = css_read_selector(css, len, &pos);
        if (!selector) {
            /* Could be malformed; try to recover. */
            pos++;
            continue;
        }
        if (pos >= len || css[pos] != '{') {
            free(selector);
            continue;
        }
        pos++; /* skip '{' */

        CssRule *rule = css_stylesheet_add_rule(sheet);
        if (!rule) {
            free(selector);
            break;
        }
        rule->selector_text = selector;
        css_parse_declaration_block(css, len, &pos, rule);
    }

    if (sheet->rule_count == 0) {
        free(sheet);
        return NULL;
    }
    return sheet;
}

void css_stylesheet_free(CssStylesheet *sheet) {
    if (!sheet) return;
    for (int i = 0; i < sheet->rule_count; i++) {
        CssRule *rule = &sheet->rules[i];
        free(rule->selector_text);
        free(rule->media_query);
        css_declarations_free(rule->declarations, rule->declaration_count);
    }
    free(sheet->rules);
    free(sheet);
}

/* ============================================================================
 * Selector parsing and matching
 * ============================================================================ */

#define CSS_MAX_SIMPLE_PARTS 16
#define CSS_MAX_CLASSES 16

typedef struct CssSimpleSelector {
    char tag[64];
    char id[128];
    char classes[CSS_MAX_CLASSES][64];
    int class_count;
    bool has_tag;
    bool has_id;
    bool universal;
} CssSimpleSelector;

/* combinator that precedes this simple selector in document order */
typedef struct CssSelectorPart {
    CssSimpleSelector simple;
    int combinator; /* 0 = none (first), 1 = descendant (space), 2 = child (>) */
} CssSelectorPart;

/* combinator values */
#define CSS_COMB_NONE       0
#define CSS_COMB_DESCENDANT 1
#define CSS_COMB_CHILD      2

static void css_parse_simple_selector(const char *s, size_t n, CssSimpleSelector *out) {
    memset(out, 0, sizeof(*out));
    size_t i = 0;
    css_skip_space(s, n, &i);
    if (i >= n) return;

    if (s[i] == '*') {
        out->universal = true;
        i++;
    } else if (s[i] != '.' && s[i] != '#' && s[i] != '[') {
        /* tag */
        size_t start = i;
        while (i < n && s[i] != '.' && s[i] != '#' && s[i] != '[') i++;
        css_strncpy_lower(out->tag, s + start, i - start, sizeof(out->tag));
        out->has_tag = out->tag[0] != '\0';
    }

    while (i < n) {
        css_skip_space(s, n, &i);
        if (i >= n) break;
        if (s[i] == '.') {
            i++;
            size_t start = i;
            while (i < n && s[i] != '.' && s[i] != '#' && s[i] != '[') i++;
            if (out->class_count < CSS_MAX_CLASSES) {
                css_strncpy_lower(out->classes[out->class_count], s + start, i - start,
                                  sizeof(out->classes[0]));
                out->class_count++;
            }
        } else if (s[i] == '#') {
            i++;
            size_t start = i;
            while (i < n && s[i] != '.' && s[i] != '#' && s[i] != '[') i++;
            css_strncpy_lower(out->id, s + start, i - start, sizeof(out->id));
            out->has_id = out->id[0] != '\0';
        } else if (s[i] == '[') {
            /* Skip attribute selector for matching purposes. */
            i++;
            int depth = 1;
            while (i < n && depth > 0) {
                if (s[i] == '[') depth++;
                else if (s[i] == ']') depth--;
                else if (s[i] == '"' || s[i] == '\'') {
                    char q = s[i++];
                    while (i < n && s[i] != q) i++;
                }
                i++;
            }
        } else {
            break;
        }
    }
}

/* Parse one selector chain (no commas). Returns number of parts. */
static int css_parse_selector_chain(const char *s, CssSelectorPart *parts, int max_parts) {
    size_t len = strlen(s);
    size_t i = 0;
    int count = 0;

    while (i < len && count < max_parts) {
        css_skip_space(s, len, &i);
        if (i >= len) break;

        CssSelectorPart *part = &parts[count++];
        part->combinator = (count == 1) ? CSS_COMB_NONE : CSS_COMB_DESCENDANT;

        if (count > 1) {
            /* Check for explicit combinator. */
            if (s[i] == '>') {
                part->combinator = CSS_COMB_CHILD;
                i++;
                css_skip_space(s, len, &i);
            }
        }

        size_t start = i;
        bool in_quote = false;
        char quote_char = 0;
        while (i < len) {
            char c = s[i];
            if (!in_quote) {
                if (c == '"' || c == '\'') { in_quote = true; quote_char = c; }
                else if (c == ' ' || c == '>' || c == '+') break;
            } else {
                if (c == quote_char) in_quote = false;
            }
            i++;
        }

        css_parse_simple_selector(s + start, i - start, &part->simple);
    }
    return count;
}

static int css_specificity_from_simple(const CssSimpleSelector *simple) {
    int a = simple->has_id ? 1 : 0;
    int b = simple->class_count;
    if (simple->universal) b = 0;
    int c = (simple->has_tag && !simple->universal) ? 1 : 0;
    return a * 1000 + b * 10 + c;
}

static int css_specificity_from_chain(const CssSelectorPart *parts, int count) {
    int spec = 0;
    for (int i = 0; i < count; i++) {
        spec += css_specificity_from_simple(&parts[i].simple);
    }
    return spec;
}

/* HTML node helpers. */
static const char* html_node_attr_value(HtmlNode *node, const char *name) {
    if (!node || !node->attributes) return NULL;
    for (HtmlAttribute *a = node->attributes; a; a = a->next) {
        if (strcasecmp(a->name, name) == 0) return a->value;
    }
    return NULL;
}

static bool html_node_class_contains(HtmlNode *node, const char *cls) {
    const char *class_attr = html_node_attr_value(node, "class");
    if (!class_attr || !cls || !cls[0]) return false;
    size_t cls_len = strlen(cls);
    const char *p = class_attr;
    while (*p) {
        while (*p && css_is_space(*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !css_is_space(*p)) p++;
        size_t len = (size_t)(p - start);
        if (len == cls_len && strncasecmp(start, cls, len) == 0) return true;
    }
    return false;
}

static HtmlNode* html_node_parent_node(HtmlDocument *doc, HtmlNode *node) {
    if (!doc || !node) return NULL;
    int idx = po_array_index_from_payload(&doc->array, node);
    if (idx < 0) return NULL;
    int p = po_array_parent(&doc->array, idx);
    if (p < 0) return NULL;
    return (HtmlNode*)po_array_payload(&doc->array, p);
}

static bool css_simple_matches(const CssSimpleSelector *simple, HtmlNode *node) {
    if (!node || node->type != HTML_NODE_ELEMENT) return false;
    if (simple->has_id) {
        const char *id = html_node_attr_value(node, "id");
        if (!id || strcasecmp(id, simple->id) != 0) return false;
    }
    for (int i = 0; i < simple->class_count; i++) {
        if (!html_node_class_contains(node, simple->classes[i])) return false;
    }
    if (simple->has_tag) {
        if (strcasecmp(node->tag_name, simple->tag) != 0) return false;
    }
    /* Universal with no id/class always true. */
    return true;
}

static bool css_chain_matches(const CssSelectorPart *parts, int count,
                              HtmlDocument *doc, HtmlNode *node) {
    if (count <= 0) return false;
    HtmlNode *current = node;
    for (int i = count - 1; i >= 0; i--) {
        if (!current) return false;
        if (!css_simple_matches(&parts[i].simple, current)) return false;
        if (i == 0) return true;

        int comb = parts[i].combinator; /* relates part i to part i-1 */
        if (comb == CSS_COMB_CHILD) {
            current = html_node_parent_node(doc, current);
        } else { /* descendant */
            current = html_node_parent_node(doc, current);
            while (current && !css_simple_matches(&parts[i - 1].simple, current)) {
                current = html_node_parent_node(doc, current);
            }
            if (!current) return false;
        }
    }
    return true;
}

bool css_selector_matches_one(const char *selector, HtmlDocument *doc, HtmlNode *node) {
    CssSelectorPart parts[CSS_MAX_SIMPLE_PARTS];
    int count = css_parse_selector_chain(selector, parts, CSS_MAX_SIMPLE_PARTS);
    return css_chain_matches(parts, count, doc, node);
}

bool css_selector_matches(const char *selector, HtmlDocument *doc, HtmlNode *node) {
    size_t len = strlen(selector);
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        bool in_paren = false;
        bool in_quote = false;
        char quote_char = 0;
        while (i < len) {
            char c = selector[i];
            if (!in_quote) {
                if (c == '"' || c == '\'') { in_quote = true; quote_char = c; }
                else if (c == ',' && !in_paren) break;
            } else {
                if (c == quote_char) in_quote = false;
            }
            i++;
        }
        char *part = css_strndup_trim(selector + start, i - start);
        if (part && part[0]) {
            bool ok = css_selector_matches_one(part, doc, node);
            free(part);
            if (ok) return true;
        } else {
            free(part);
        }
        if (i < len && selector[i] == ',') i++;
    }
    return false;
}

/* ============================================================================
 * Style application
 * ============================================================================ */

int css_applied_decl_compare(const void *a, const void *b) {
    const CssAppliedDecl *da = (const CssAppliedDecl*)a;
    const CssAppliedDecl *db = (const CssAppliedDecl*)b;
    if (da->specificity != db->specificity) return da->specificity - db->specificity;
    return da->order - db->order;
}

void css_set_style_property(JSContextHandle ctx, GCValue style, const char *prop, const char *value) {
    if (!prop || !value) return;
    JS_SetPropertyStr(ctx, style, prop, JS_NewString(ctx, value));
    char *camel = css_to_camel_case(prop);
    if (camel) {
        if (strcmp(camel, prop) != 0) {
            JS_SetPropertyStr(ctx, style, camel, JS_NewString(ctx, value));
        }
        free(camel);
    }
}

static void css_seed_vendor_style_properties(JSContextHandle ctx, GCValue style) {
    /* Web Animations polyfill and other libraries do vendor-prefix detection
     * with `'webkitTransform' in element.style`.  Seed the common transform
     * properties so those `in` checks complete without throwing. */
    static const char *props[] = {
        "webkitTransform", "msTransform",
        "webkitTransformOrigin",
        "webkitPerspective", "webkitPerspectiveOrigin",
        "transform", "transformOrigin",
        "perspective", "perspectiveOrigin",
        NULL
    };
    for (int i = 0; props[i]; i++) {
        JS_SetPropertyStr(ctx, style, props[i], JS_NewString(ctx, ""));
    }
}

GCValue css_ensure_style_object(JSContextHandle ctx, GCValue element) {
    /* Use a private internal slot so we don't recurse through the public
     * `style` accessor getter when it calls this helper. */
    GCValue style = JS_GetPropertyStr(ctx, element, "__style");
    if (JS_IsUndefined(style) || JS_IsNull(style) || !JS_IsObject(style)) {
        style = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, style, "animationTimingFunction", JS_NewString(ctx, ""));
        css_seed_vendor_style_properties(ctx, style);
        JS_SetPropertyStr(ctx, style, "__element", element);

        GCValue global = JS_GetGlobalObject(ctx);
        GCValue proto = JS_GetPropertyStr(ctx, global, "__CSSStyleDeclarationProto");
        if (JS_IsObject(proto)) {
            JS_SetPrototype(ctx, style, proto);
        }
        JS_SetPropertyStr(ctx, element, "__style", style);
    }
    return style;
}

void css_apply_inline_style_string(JSContextHandle ctx, GCValue element,
                                   const char *style_attr) {
    if (!style_attr || !style_attr[0]) return;
    int count = 0;
    CssDeclaration *decls = css_parse_inline_style(style_attr, &count);
    if (!decls) return;
    GCValue style = css_ensure_style_object(ctx, element);

    for (int i = 0; i < count; i++) {
        css_set_style_property(ctx, style, decls[i].property, decls[i].value);
    }
    css_declarations_free(decls, count);
}

static void css_apply_inline_style(JSContextHandle ctx, GCValue element, HtmlNode *node) {
    const char *style_attr = html_node_attr_value(node, "style");
    css_apply_inline_style_string(ctx, element, style_attr);
}

static void css_apply_declarations(JSContextHandle ctx, GCValue element,
                                   CssAppliedDecl *applied, int count) {
    if (count <= 0) return;
    GCValue style = css_ensure_style_object(ctx, element);

    for (int i = 0; i < count; i++) {
        css_set_style_property(ctx, style, applied[i].decl->property, applied[i].decl->value);
    }
}

/* ============================================================================
 * Stylesheet loading
 * ============================================================================ */

static char* css_resolve_url(const char *base_url, const char *href) {
    if (!href || !href[0]) return NULL;
    if (url_has_scheme(href)) {
        return strdup(href);
    }
    if (strncmp(href, "//", 2) == 0) {
        char buf[2048];
        snprintf(buf, sizeof(buf), "https:%s", href);
        return strdup(buf);
    }
    if (href[0] == '/') {
        const char *base = base_url && base_url[0] ? base_url : "https://www.youtube.com";
        char buf[2048];
        snprintf(buf, sizeof(buf), "%s%s", base, href);
        return strdup(buf);
    }
    /* Relative path. */
    const char *base = base_url && base_url[0] ? base_url : "https://www.youtube.com/";
    char buf[2048];
    if (base[strlen(base) - 1] == '/') {
        snprintf(buf, sizeof(buf), "%s%s", base, href);
    } else {
        /* Strip to last slash. */
        const char *last_slash = strrchr(base, '/');
        if (last_slash) {
            size_t base_len = (size_t)(last_slash - base) + 1;
            snprintf(buf, sizeof(buf), "%.*s%s", (int)base_len, base, href);
        } else {
            snprintf(buf, sizeof(buf), "%s/%s", base, href);
        }
    }
    return strdup(buf);
}

static CssStylesheet* css_fetch_stylesheet(const char *base_url, const char *href) {
    char *url = css_resolve_url(base_url, href);
    if (!url) return NULL;
    LOG_INFO("Fetching stylesheet: %.80s", url);

    HttpBuffer buffer = {0};
    char err[256] = {0};
    bool ok = http_get_to_memory(url, &buffer, err, sizeof(err));
    CssStylesheet *sheet = NULL;
    if (ok && buffer.data && buffer.size > 0) {
        LOG_INFO("Fetched stylesheet (%zu bytes)", buffer.size);
        sheet = css_stylesheet_parse(buffer.data, buffer.size);
    } else {
        LOG_WARN("Failed to fetch stylesheet %.80s: %s", url, err[0] ? err : "unknown");
    }
    free(url);
    if (buffer.data) free(buffer.data);
    return sheet;
}

/* ============================================================================
 * Document traversal and application
 * ============================================================================ */

typedef struct CssSheetList {
    CssStylesheet **sheets;
    int count;
    int capacity;
} CssSheetList;

static bool css_sheet_list_add(CssSheetList *list, CssStylesheet *sheet) {
    if (!sheet) return false;
    if (list->count >= list->capacity) {
        int new_cap = list->capacity ? list->capacity * 2 : 4;
        CssStylesheet **new_sheets = (CssStylesheet**)realloc(list->sheets,
                                                               new_cap * sizeof(CssStylesheet*));
        if (!new_sheets) {
            css_stylesheet_free(sheet);
            return false;
        }
        list->sheets = new_sheets;
        list->capacity = new_cap;
    }
    list->sheets[list->count++] = sheet;
    return true;
}

static void css_sheet_list_free(CssSheetList *list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        css_stylesheet_free(list->sheets[i]);
    }
    free(list->sheets);
    list->sheets = NULL;
    list->count = list->capacity = 0;
}

static void css_collect_stylesheets_recursive(HtmlDocument *doc, int node_idx,
                                              CssSheetList *list,
                                              const char *base_url) {
    if (node_idx < 0) return;
    HtmlNode *node = (HtmlNode*)po_array_payload(&doc->array, node_idx);
    if (!node || node->type != HTML_NODE_ELEMENT) goto next;

    if (strcasecmp(node->tag_name, "style") == 0 && node->text_content && node->text_content[0]) {
        CssStylesheet *sheet = css_stylesheet_parse(node->text_content, strlen(node->text_content));
        if (sheet) {
            LOG_INFO("Parsed inline <style> stylesheet with %d rules", sheet->rule_count);
            css_sheet_list_add(list, sheet);
        }
    } else if (strcasecmp(node->tag_name, "link") == 0) {
        const char *rel = html_node_attr_value(node, "rel");
        const char *href = html_node_attr_value(node, "href");
        if (rel && href && strcasecmp(rel, "stylesheet") == 0) {
            CssStylesheet *sheet = css_fetch_stylesheet(base_url, href);
            if (sheet) css_sheet_list_add(list, sheet);
        }
    }

next:
    int child = po_array_first_child(&doc->array, node_idx);
    while (child >= 0) {
        css_collect_stylesheets_recursive(doc, child, list, base_url);
        child = po_array_next_sibling(&doc->array, child);
    }
}

static int css_specificity_from_selector_text(const char *selector);

/* Per-element result produced by the parallel matching phase. */
typedef struct CssElementResult {
    int node_idx;
    CssAppliedDecl *applied;
    int applied_count;
} CssElementResult;

/* Match selectors for a single element and store the resulting declarations.
 * This runs on worker threads and only reads shared DOM/CSS data; it does not
 * touch the JS heap. */
static void css_match_one_node(HtmlDocument *doc, int node_idx, CssSheetList *list,
                               CssElementResult *result) {
    result->node_idx = node_idx;
    result->applied = NULL;
    result->applied_count = 0;

    HtmlNode *node = (HtmlNode*)po_array_payload(&doc->array, node_idx);
    if (!node || node->type != HTML_NODE_ELEMENT || !node->has_js_object) return;

    int applied_cap = 64;
    int applied_count = 0;
    CssAppliedDecl *applied = (CssAppliedDecl*)malloc(applied_cap * sizeof(CssAppliedDecl));
    if (!applied) return;

    for (int s = 0; s < list->count; s++) {
        CssStylesheet *sheet = list->sheets[s];
        for (int r = 0; r < sheet->rule_count; r++) {
            CssRule *rule = &sheet->rules[r];
            if (!rule->selector_text || !rule->selector_text[0]) continue;
            if (!css_selector_matches(rule->selector_text, doc, node)) continue;
            if (!css_rule_media_matches(rule, 1024.0)) continue; /* viewport unknown here; use a default */
            int spec = rule->specificity;
            if (spec == 0) spec = css_specificity_from_selector_text(rule->selector_text);
            for (int d = 0; d < rule->declaration_count; d++) {
                if (applied_count >= applied_cap) {
                    int new_cap = applied_cap * 2;
                    CssAppliedDecl *new_app = (CssAppliedDecl*)realloc(applied,
                                                                        new_cap * sizeof(CssAppliedDecl));
                    if (!new_app) break;
                    applied = new_app;
                    applied_cap = new_cap;
                }
                applied[applied_count].decl = &rule->declarations[d];
                applied[applied_count].specificity = spec;
                applied[applied_count].order = s * 1000000 + r * 1000 + d;
                applied_count++;
            }
        }
    }

    if (applied_count == 0) {
        free(applied);
    } else {
        result->applied = applied;
        result->applied_count = applied_count;
    }
}

typedef struct CssMatchJob {
    JSContextHandle ctx;
    HtmlDocument *doc;
    CssSheetList *list;
    CssElementResult *results;
    int start;
    int end;
} CssMatchJob;

/* Worker implementation: match selectors, sort declarations by specificity,
 * and apply them to both the JS element.style object and the lock-free
 * computed-style table.  Each element is owned by exactly one worker, so JS
 * object mutation is safe even though the context is shared. */
static void css_match_node_styles_job_impl(CssMatchJob *job)
{
    for (int i = job->start; i < job->end; i++) {
        CssElementResult *res = &job->results[i];
        css_match_one_node(job->doc, res->node_idx, job->list, res);

        /* Sort the matched declarations so they can be applied in cascade order. */
        if (res->applied_count > 0) {
            qsort(res->applied, (size_t)res->applied_count,
                  sizeof(CssAppliedDecl), css_applied_decl_compare);
        }

        HtmlNode *node = (HtmlNode*)po_array_payload(&job->doc->array, res->node_idx);
        if (!node || node->type != HTML_NODE_ELEMENT || !node->has_js_object) {
            free(res->applied);
            res->applied = NULL;
            res->applied_count = 0;
            continue;
        }

        GCValue element = node->js_object;
        if (JS_IsUndefined(element) || JS_IsNull(element)) {
            free(res->applied);
            res->applied = NULL;
            res->applied_count = 0;
            continue;
        }

        DOMNodeHandle dom_node = DOMNodeHandle::from_object(element);
        if (!dom_node.valid()) {
            free(res->applied);
            res->applied = NULL;
            res->applied_count = 0;
            continue;
        }

        /* Apply stylesheet declarations and inline styles to the JS element.style
         * object.  Each element is owned by exactly one worker, so this is safe. */
        css_apply_declarations(job->ctx, element, res->applied, res->applied_count);
        css_apply_inline_style(job->ctx, element, node);

        /* Mirror the same values in getComputedStyle's lock-free table. */
        if (res->applied_count > 0) {
            css_computed_apply_declarations(job->ctx, dom_node,
                                            res->applied, res->applied_count);
        }
        const char *style_attr = html_node_attr_value(node, "style");
        if (style_attr && style_attr[0]) {
            css_computed_apply_inline_style(job->ctx, dom_node, style_attr);
        }

        free(res->applied);
        res->applied = NULL;
        res->applied_count = 0;
    }
}

/* Collect every element node that has a backing JS object into a results array. */
static CssElementResult* css_collect_element_results(HtmlDocument *doc, int *out_count) {
    int cap = 64;
    int count = 0;
    CssElementResult *results = (CssElementResult*)malloc(cap * sizeof(CssElementResult));
    if (!results) {
        *out_count = 0;
        return NULL;
    }

    size_t n = po_array_count(&doc->array);
    for (size_t i = 0; i < n; i++) {
        HtmlNode *node = (HtmlNode*)po_array_payload(&doc->array, (int)i);
        if (!node || node->type != HTML_NODE_ELEMENT || !node->has_js_object) continue;

        if (count >= cap) {
            int new_cap = cap * 2;
            CssElementResult *new_results = (CssElementResult*)realloc(results,
                                                                        new_cap * sizeof(CssElementResult));
            if (!new_results) break;
            results = new_results;
            cap = new_cap;
        }
        results[count].node_idx = (int)i;
        results[count].applied = NULL;
        results[count].applied_count = 0;
        count++;
    }

    *out_count = count;
    return results;
}

/* Use the GC thread pool to match selectors for every element, sort
 * declarations, and apply them to both the JS element.style object and the
 * lock-free computed-style table.  Each worker owns a disjoint chunk of
 * elements, so all style application is parallel. */
static void css_apply_node_styles_parallel(JSContextHandle ctx, HtmlDocument *doc,
                                           CssSheetList *list) {
    int element_count = 0;
    CssElementResult *results = css_collect_element_results(doc, &element_count);
    if (!results || element_count == 0) {
        free(results);
        return;
    }

    /* Pre-allocate computed-style objects on the main thread so the serial
     * pass below can write lock-free without racing on lazy allocation. */
    for (int i = 0; i < element_count; i++) {
        HtmlNode *node = (HtmlNode*)po_array_payload(&doc->array, results[i].node_idx);
        if (!node || !node->has_js_object) continue;
        GCValue element = node->js_object;
        if (JS_IsUndefined(element) || JS_IsNull(element)) continue;
        DOMNodeHandle dom_node = DOMNodeHandle::from_object(element);
        if (dom_node.valid()) {
            css_ensure_computed_style(dom_node);
        }
    }

    /* Run style matching and application serially on the main thread.
     * The previous worker-thread implementation shared the JS context across
     * threads, which is not safe for QuickJS and caused intermittent segfaults. */
    CssMatchJob job;
    job.ctx = ctx;
    job.doc = doc;
    job.list = list;
    job.results = results;
    job.start = 0;
    job.end = element_count;
    css_match_node_styles_job_impl(&job);

    for (int i = 0; i < element_count; i++) {
        free(results[i].applied);
    }
    free(results);
}

bool css_rule_media_matches(const CssRule *rule, double viewport_width) {
    if (!rule || !rule->media_query || !rule->media_query[0]) return true;
    const char *s = rule->media_query;
    bool result = true;
    bool expect_and = false;  /* we only support 'and'-joined media features */

    while (*s) {
        while (*s && css_is_space(*s)) s++;
        if (!*s) break;

        if (strncasecmp(s, "only", 4) == 0) {
            s += 4;
            while (*s && css_is_space(*s)) s++;
        }
        if (strncasecmp(s, "screen", 6) == 0 ||
            strncasecmp(s, "all", 3) == 0 ||
            strncasecmp(s, "print", 5) == 0) {
            while (*s && !css_is_space(*s) && *s != '(') s++;
            while (*s && css_is_space(*s)) s++;
        }

        if (*s == '(') {
            s++;
            while (*s && css_is_space(*s)) s++;
            bool is_min = false, is_max = false;
            const char *prop = s;
            while (*s && *s != ':' && *s != ')') s++;
            size_t prop_len = (size_t)(s - prop);
            if (*s == ':') {
                s++;
                while (*s && css_is_space(*s)) s++;
                char *end = NULL;
                double val = strtod(s, &end);
                if (prop_len >= 8 && strncasecmp(prop, "min-width", 9) == 0) is_min = true;
                else if (prop_len >= 8 && strncasecmp(prop, "max-width", 9) == 0) is_max = true;
                bool matches = true;
                if (is_min) matches = viewport_width >= val - 0.5;
                else if (is_max) matches = viewport_width <= val + 0.5;
                if (expect_and) result = result && matches;
                else result = matches;
                expect_and = true;
                s = end;
            }
            while (*s && *s != ')') s++;
            if (*s == ')') s++;
        } else if (strncasecmp(s, "and", 3) == 0) {
            s += 3;
            expect_and = true;
            continue;
        } else {
            /* Unknown token; skip until next space/paren. */
            while (*s && !css_is_space(*s) && *s != '(') s++;
        }
        while (*s && css_is_space(*s)) s++;
        if (*s == ',' || strncasecmp(s, "or", 2) == 0) return true; /* be permissive for comma/or */
    }
    return result;
}

/* Recompute specificity from raw selector text (used when not precomputed). */
int css_specificity_from_selector_text(const char *selector) {
    CssSelectorPart parts[CSS_MAX_SIMPLE_PARTS];
    int count = css_parse_selector_chain(selector, parts, CSS_MAX_SIMPLE_PARTS);
    return css_specificity_from_chain(parts, count);
}

void css_apply_document_styles(JSContextHandle ctx, GCValue js_doc,
                               HtmlDocument *doc, const char *base_url) {
    (void)js_doc;
    if (!ctx || !doc) return;

    CssSheetList list = {0};
    int root_idx = doc->root_idx;
    if (root_idx < 0) {
        /* Try any root. */
        size_t n = po_array_count(&doc->array);
        for (size_t i = 0; i < n; i++) {
            HtmlNode *node = (HtmlNode*)po_array_payload(&doc->array, (int)i);
            if (node && node->type == HTML_NODE_ELEMENT && po_array_parent(&doc->array, (int)i) < 0) {
                root_idx = (int)i;
                break;
            }
        }
    }
    if (root_idx < 0) return;

    LOG_INFO("Collecting stylesheets from DOM...");
    css_collect_stylesheets_recursive(doc, root_idx, &list, base_url);
    LOG_INFO("Collected %d stylesheet(s), applying to DOM", list.count);

    css_apply_node_styles_parallel(ctx, doc, &list);

    css_sheet_list_free(&list);
}

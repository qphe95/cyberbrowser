/*
 * CSS Parser - Implementation
 *
 * A small, self-contained CSS parser for applying styles to the DOM tree.
 */

#include "css_parser.h"
#include "dom_js.h"
#include "browser/css_style_decl.h"
#include "http_download.h"
#include "platform.h"
#include "url_utils.h"
#include "js_quickjs.h"
#include "browser_api_impl.h"
#include "browser_api_impl_handle_helpers.h"
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

    /* Detect and strip trailing !important (case-insensitive) from the value.
     * The important flag is stored on the declaration so the cascade can
     * prioritize it above normal declarations. */
    bool is_important = false;
    {
        size_t vlen = strlen(v);
        if (vlen >= 10 && strncasecmp(v + vlen - 10, "!important", 10) == 0) {
            is_important = true;
            v[vlen - 10] = '\0';
            size_t trimmed = vlen - 10;
            while (trimmed > 0 && css_is_space(v[trimmed - 1])) v[--trimmed] = '\0';
        }
    }

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
    rule->declarations[rule->declaration_count].important = is_important;
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
                /* Strip !important from inline style values. */
                bool is_imp = false;
                size_t vlen = strlen(v);
                if (vlen >= 10 && strncasecmp(v + vlen - 10, "!important", 10) == 0) {
                    is_imp = true;
                    v[vlen - 10] = '\0';
                    size_t trimmed = vlen - 10;
                    while (trimmed > 0 && css_is_space(v[trimmed - 1])) v[--trimmed] = '\0';
                }
                if (count >= cap) {
                    cap = cap ? cap * 2 : 4;
                    CssDeclaration *new_decls = (CssDeclaration*)realloc(decls, cap * sizeof(CssDeclaration));
                    if (!new_decls) { free(p); free(v); break; }
                    decls = new_decls;
                }
                decls[count].property = p;
                decls[count].value = v;
                decls[count].important = is_imp;
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

/* Read an at-rule precondition: everything up to (not including) the next
 * '{' or ';' at paren depth 0, or a block-closing '}'.  Returns a malloc'd
 * trimmed string (possibly empty). */
static char* css_read_at_condition(const char *s, size_t len, size_t *pos) {
    css_skip_space_and_comments(s, len, pos);
    size_t start = *pos;
    int depth = 0;
    char quote = 0;
    while (*pos < len) {
        char c = s[*pos];
        if (quote) {
            if (c == quote) quote = 0;
            else if (c == '\\' && *pos + 1 < len) (*pos)++;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(') {
            depth++;
        } else if (c == ')') {
            if (depth > 0) depth--;
        } else if (depth == 0 && (c == '{' || c == ';' || c == '}')) {
            break;
        }
        (*pos)++;
    }
    return css_strndup_trim(s + start, *pos - start);
}

/* Parse an at-rule.  @media blocks are entered and their condition is
 * attached to every nested rule (ANDed with any enclosing @media condition).
 * Other block at-rules whose contents are style rules (@supports, @layer)
 * are entered unconditionally; at-rules whose contents are not style rules
 * (@font-face, @keyframes, @page) are skipped wholesale.  Statement at-rules
 * (@import, @charset) are skipped. */
static void css_parse_at_rule(const char *s, size_t len, size_t *pos,
                              CssStylesheet *sheet, const char *parent_cond) {
    /* Skip '@' and identifier. */
    (*pos)++;
    size_t kw_start = *pos;
    while (*pos < len && (isalnum((unsigned char)s[*pos]) || s[*pos] == '-')) (*pos)++;
    size_t kw_len = *pos - kw_start;
    bool is_media = (kw_len == 5 && strncasecmp(s + kw_start, "media", 5) == 0);
    bool skip_block =
        (kw_len == 9  && strncasecmp(s + kw_start, "font-face", 9) == 0) ||
        (kw_len == 8  && strncasecmp(s + kw_start, "keyframes", 8) == 0) ||
        (kw_len == 17 && strncasecmp(s + kw_start, "-webkit-keyframes", 17) == 0) ||
        (kw_len == 4  && strncasecmp(s + kw_start, "page", 4) == 0);

    char *cond = css_read_at_condition(s, len, pos);

    if (*pos >= len || s[*pos] != '{') {
        /* Statement form (@import, @charset, ...): skip until ';' or '}'. */
        free(cond);
        while (*pos < len && s[*pos] != ';' && s[*pos] != '}') (*pos)++;
        if (*pos < len && s[*pos] == ';') (*pos)++;
        return;
    }
    (*pos)++; /* skip '{' */

    if (skip_block) {
        free(cond);
        int depth = 1;
        char quote = 0;
        while (*pos < len && depth > 0) {
            char c = s[*pos];
            if (quote) {
                if (c == quote) quote = 0;
                else if (c == '\\' && *pos + 1 < len) (*pos)++;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '{') {
                depth++;
            } else if (c == '}') {
                depth--;
            }
            (*pos)++;
        }
        return;
    }

    /* Effective condition for rules in this block: ANDed with any enclosing
     * @media condition.  Non-@media blocks attach no condition (their rules
     * apply unconditionally). */
    char *eff = NULL;
    if (is_media) {
        if (parent_cond && parent_cond[0] && cond && cond[0]) {
            /* Each query list must match: joined by CSS_MEDIA_AND. */
            size_t n = strlen(parent_cond) + strlen(cond) + 2;
            eff = (char *)malloc(n);
            if (eff) snprintf(eff, n, "%s%c%s", parent_cond, CSS_MEDIA_AND, cond);
        } else if (parent_cond && parent_cond[0]) {
            eff = strdup(parent_cond);
        } else if (cond && cond[0]) {
            eff = strdup(cond);
        }
    }
    free(cond);

    /* Block-form at-rule: parse nested rules. */
    while (*pos < len) {
        css_skip_space_and_comments(s, len, pos);
        if (*pos >= len) break;
        if (s[*pos] == '}') { (*pos)++; break; }
        if (s[*pos] == '@') {
            css_parse_at_rule(s, len, pos, sheet, eff ? eff : "");
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
            continue;
        }
        (*pos)++; /* skip '{' */
        CssRule *rule = css_stylesheet_add_rule(sheet);
        if (!rule) {
            free(selector);
            break;
        }
        rule->selector_text = selector;
        rule->media_query = (eff && eff[0]) ? strdup(eff) : NULL;
        css_parse_declaration_block(s, len, pos, rule);
    }
    free(eff);
}

CssStylesheet* css_stylesheet_parse(const char *css, size_t len) {
    size_t end = 0;
    CssStylesheet *sheet = css_stylesheet_parse_range(css, len, 0, len, &end);
    if (sheet && sheet->rule_count == 0) {
        free(sheet);
        return NULL;
    }
    return sheet;
}

/* Top-level parse of css[start..) that stops at the first top-level item
 * starting at or after `stop` (an item that starts before `stop` is parsed
 * to its end, reading past `stop`).  *end_pos receives where the parser
 * stopped.  Parsing [0,len) in one call or in consecutive ranges whose
 * starts equal the previous range's end_pos gives identical rules: the
 * top-level parser carries no state besides the position. */
CssStylesheet* css_stylesheet_parse_range(const char *css, size_t len,
                                          size_t start, size_t stop, size_t *end_pos) {
    if (end_pos) *end_pos = start;
    if (!css || len == 0) return NULL;
    CssStylesheet *sheet = (CssStylesheet*)calloc(1, sizeof(CssStylesheet));
    if (!sheet) return NULL;
    if (stop > len) stop = len;

    size_t pos = start;
    while (pos < stop) {
        css_skip_space_and_comments(css, len, &pos);
        if (pos >= len) break;

        if (css[pos] == '@') {
            css_parse_at_rule(css, len, &pos, sheet, "");
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

    if (end_pos) *end_pos = pos;
    return sheet;   /* possibly with zero rules */
}

void css_stylesheet_free(CssStylesheet *sheet) {
    if (!sheet) return;
    for (int i = 0; i < sheet->rule_count; i++) {
        CssRule *rule = &sheet->rules[i];
        free(rule->selector_text);
        free(rule->media_query);
        css_declarations_free(rule->declarations, rule->declaration_count);
        css_rule_compiled_free(rule);
    }
    free(sheet->rules);
    free(sheet);
}

/* ============================================================================
 * Selector parsing and matching
 * ============================================================================ */

#include "css_selector_internal.h"

static void css_parse_simple_selector(const char *s, size_t n, CssSimpleSelector *out);

/* Consume one pseudo token starting at s[*pi] == ':'.  Handles an optional
 * second ':' (pseudo-element), the name, and an optional balanced (...)
 * argument (quotes respected).  Returns the name span and argument span. */
static void css_consume_pseudo(const char *s, size_t n, size_t *pi,
                               const char **name, size_t *name_len,
                               const char **arg, size_t *arg_len,
                               bool *is_element)
{
    size_t i = *pi;
    *name = NULL; *name_len = 0; *arg = NULL; *arg_len = 0;
    *is_element = false;
    i++; /* first ':' */
    if (i < n && s[i] == ':') { i++; *is_element = true; }
    size_t ns = i;
    while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '-')) i++;
    *name = s + ns; *name_len = i - ns;
    if (i < n && s[i] == '(') {
        int depth = 1;
        i++;
        size_t as = i;
        while (i < n && depth > 0) {
            if (s[i] == '(') depth++;
            else if (s[i] == ')') { depth--; if (depth == 0) break; }
            else if (s[i] == '"' || s[i] == '\'') {
                char q = s[i++];
                while (i < n && s[i] != q) i++;
            }
            i++;
        }
        *arg = s + as;
        *arg_len = (i > as) ? (i - as) : 0;
        if (i < n && s[i] == ')') i++;
    }
    *pi = i;
}

static bool css_pseudo_name_is(const char *name, size_t nl, const char *kw)
{
    size_t kl = strlen(kw);
    return nl == kl && strncasecmp(name, kw, nl) == 0;
}

/* Parse an An+B expression ("odd", "even", "n", "2n+1", "-n+3", "3"). */
static void css_parse_nth(const char *arg, size_t al, int *out_a, int *out_b)
{
    char buf[64];
    size_t k = al < sizeof(buf) - 1 ? al : sizeof(buf) - 1;
    size_t w = 0;
    for (size_t j = 0; j < k; j++) {
        if (!css_is_space(arg[j])) buf[w++] = (char)tolower((unsigned char)arg[j]);
    }
    buf[w] = '\0';
    *out_a = 0; *out_b = 0;
    if (strcmp(buf, "odd") == 0) { *out_a = 2; *out_b = 1; return; }
    if (strcmp(buf, "even") == 0) { *out_a = 2; *out_b = 0; return; }
    char *np = strchr(buf, 'n');
    if (!np) { *out_a = 0; *out_b = atoi(buf); return; }
    if (np == buf) *out_a = 1;
    else if (np == buf + 1 && buf[0] == '-') *out_a = -1;
    else if (np == buf + 1 && buf[0] == '+') *out_a = 1;
    else *out_a = atoi(buf);
    *out_b = atoi(np + 1);
}

/* Record a parsed pseudo-class/element on the simple selector. */
static void css_apply_pseudo(CssSimpleSelector *out,
                             const char *name, size_t nl,
                             const char *arg, size_t al,
                             bool is_element)
{
    if (is_element) { out->is_pseudo_element = true; return; }
    /* Legacy single-colon pseudo-elements. */
    if (css_pseudo_name_is(name, nl, "before") ||
        css_pseudo_name_is(name, nl, "after") ||
        css_pseudo_name_is(name, nl, "first-letter") ||
        css_pseudo_name_is(name, nl, "first-line") ||
        css_pseudo_name_is(name, nl, "marker") ||
        css_pseudo_name_is(name, nl, "placeholder") ||
        css_pseudo_name_is(name, nl, "selection") ||
        css_pseudo_name_is(name, nl, "backdrop") ||
        css_pseudo_name_is(name, nl, "file-selector-button")) {
        out->is_pseudo_element = true;
        return;
    }
    if (css_pseudo_name_is(name, nl, "root")) {
        out->is_root = true;
        out->has_substantive = true;
        return;
    }
    if (css_pseudo_name_is(name, nl, "empty")) { out->requires_empty = true; return; }
    if (css_pseudo_name_is(name, nl, "link")) { out->requires_link = true; return; }
    /* Dynamic states: never true for a freshly-rendered static page. */
    if (css_pseudo_name_is(name, nl, "visited") ||
        css_pseudo_name_is(name, nl, "hover") ||
        css_pseudo_name_is(name, nl, "active") ||
        css_pseudo_name_is(name, nl, "focus") ||
        css_pseudo_name_is(name, nl, "focus-visible") ||
        css_pseudo_name_is(name, nl, "focus-within") ||
        css_pseudo_name_is(name, nl, "target") ||
        css_pseudo_name_is(name, nl, "target-within") ||
        css_pseudo_name_is(name, nl, "checked") ||
        css_pseudo_name_is(name, nl, "disabled") ||
        css_pseudo_name_is(name, nl, "indeterminate") ||
        css_pseudo_name_is(name, nl, "read-write") ||
        css_pseudo_name_is(name, nl, "autofill") ||
        css_pseudo_name_is(name, nl, "playing") ||
        css_pseudo_name_is(name, nl, "paused") ||
        css_pseudo_name_is(name, nl, "current") ||
        css_pseudo_name_is(name, nl, "past") ||
        css_pseudo_name_is(name, nl, "future")) {
        out->never_match = true;
        return;
    }
    if (css_pseudo_name_is(name, nl, "first-child")) { out->child_pos = 1; return; }
    if (css_pseudo_name_is(name, nl, "last-child")) { out->child_pos = 2; return; }
    if (css_pseudo_name_is(name, nl, "only-child")) { out->child_pos = 3; return; }
    if (css_pseudo_name_is(name, nl, "first-of-type")) { out->type_pos = 1; return; }
    if (css_pseudo_name_is(name, nl, "last-of-type")) { out->type_pos = 2; return; }
    if (css_pseudo_name_is(name, nl, "only-of-type")) { out->type_pos = 3; return; }
    if (css_pseudo_name_is(name, nl, "nth-child")) {
        out->nth_kind = 1; css_parse_nth(arg, al, &out->nth_a, &out->nth_b); return;
    }
    if (css_pseudo_name_is(name, nl, "nth-last-child")) {
        out->nth_kind = 2; css_parse_nth(arg, al, &out->nth_a, &out->nth_b); return;
    }
    if (css_pseudo_name_is(name, nl, "nth-of-type")) {
        out->nth_kind = 3; css_parse_nth(arg, al, &out->nth_a, &out->nth_b); return;
    }
    if (css_pseudo_name_is(name, nl, "nth-last-of-type")) {
        out->nth_kind = 4; css_parse_nth(arg, al, &out->nth_a, &out->nth_b); return;
    }
    if (css_pseudo_name_is(name, nl, "not")) {
        /* Parse the first compound of the argument as a simple selector and
         * store it flattened; complex/list arguments degrade to "always match"
         * (the pseudo is effectively dropped), matching legacy behavior. */
        CssSimpleSelector tmp;
        css_parse_simple_selector(arg, al, &tmp);
        if (tmp.has_tag || tmp.has_id || tmp.class_count > 0 || tmp.attr_count > 0 ||
            tmp.universal) {
            out->not_active = true;
            out->not_has_tag = tmp.has_tag;
            out->not_has_id = tmp.has_id;
            memcpy(out->not_tag, tmp.tag, sizeof(out->not_tag));
            memcpy(out->not_id, tmp.id, sizeof(out->not_id));
            memcpy(out->not_classes, tmp.classes, sizeof(out->not_classes));
            out->not_class_count = tmp.class_count;
            memcpy(out->not_attrs, tmp.attrs, sizeof(out->not_attrs));
            memcpy(out->not_attr_ops, tmp.attr_ops, sizeof(out->not_attr_ops));
            memcpy(out->not_attr_ci, tmp.attr_ci, sizeof(out->not_attr_ci));
            memcpy(out->not_attr_vals, tmp.attr_vals, sizeof(out->not_attr_vals));
            out->not_attr_count = tmp.attr_count;
        }
        return;
    }
    if (css_pseudo_name_is(name, nl, "where") ||
        css_pseudo_name_is(name, nl, "is") ||
        css_pseudo_name_is(name, nl, "matches")) {
        /* Keep the argument text; it is evaluated recursively at match time.
         * Without this, a:where(.new) would degrade to "a" and match every
         * link on the page. */
        if (arg && al > 0) {
            size_t copy = al;
            if (copy >= sizeof(out->where_arg)) copy = sizeof(out->where_arg) - 1;
            memcpy(out->where_arg, arg, copy);
            out->where_arg[copy] = '\0';
            out->where_active = true;
            out->has_substantive = true;
        }
        return;
    }
    if (css_pseudo_name_is(name, nl, "has")) {
        /* Relative selector: an optional leading combinator, then a selector
         * matched against descendants / children / following siblings. */
        if (arg && al > 0) {
            size_t k = 0;
            while (k < al && css_is_space(arg[k])) k++;
            char comb = 0;
            if (k < al && (arg[k] == '>' || arg[k] == '+' || arg[k] == '~')) {
                comb = arg[k++];
                while (k < al && css_is_space(arg[k])) k++;
            }
            size_t copy = al - k;
            if (copy >= sizeof(out->has_arg)) copy = sizeof(out->has_arg) - 1;
            memcpy(out->has_arg, arg + k, copy);
            out->has_arg[copy] = '\0';
            out->has_comb = comb;
            out->has_active = copy > 0;
        }
        return;
    }
    /* Unknown pseudo-classes are ignored, i.e. they neither add nor remove
     * constraints. */
}

static void css_parse_simple_selector(const char *s, size_t n, CssSimpleSelector *out) {
    memset(out, 0, sizeof(*out));
    size_t i = 0;
    css_skip_space(s, n, &i);
    if (i >= n) return;

    if (s[i] == '*') {
        out->universal = true;
        out->has_substantive = true;
        i++;
    } else if (s[i] == ':') {
        /* Leading pseudo-class/element. */
        const char *name, *arg;
        size_t name_len, arg_len;
        bool is_element;
        css_consume_pseudo(s, n, &i, &name, &name_len, &arg, &arg_len, &is_element);
        css_apply_pseudo(out, name, name_len, arg, arg_len, is_element);
    } else if (s[i] != '.' && s[i] != '#' && s[i] != '[') {
        /* tag */
        size_t start = i;
        while (i < n && s[i] != '.' && s[i] != '#' && s[i] != '[' &&
               s[i] != ':') i++;
        css_strncpy_lower(out->tag, s + start, i - start, sizeof(out->tag));
        out->has_tag = out->tag[0] != '\0';
        out->has_substantive = out->has_substantive || out->has_tag;
    }

    while (i < n) {
        css_skip_space(s, n, &i);
        if (i >= n) break;
        if (s[i] == '.') {
            i++;
            size_t start = i;
            while (i < n && s[i] != '.' && s[i] != '#' && s[i] != '[' && s[i] != ':') i++;
            if (out->class_count < CSS_MAX_CLASSES) {
                css_strncpy_lower(out->classes[out->class_count], s + start, i - start,
                                  sizeof(out->classes[0]));
                out->class_count++;
                out->has_substantive = true;
            }
        } else if (s[i] == '#') {
            i++;
            size_t start = i;
            while (i < n && s[i] != '.' && s[i] != '#' && s[i] != '[' && s[i] != ':') i++;
            css_strncpy_lower(out->id, s + start, i - start, sizeof(out->id));
            out->has_id = out->id[0] != '\0';
            out->has_substantive = out->has_substantive || out->has_id;
        } else if (s[i] == ':') {
            /* Pseudo-class/element. */
            const char *name, *arg;
            size_t name_len, arg_len;
            bool is_element;
            css_consume_pseudo(s, n, &i, &name, &name_len, &arg, &arg_len, &is_element);
            css_apply_pseudo(out, name, name_len, arg, arg_len, is_element);
        } else if (s[i] == '[') {
            /* Parse attribute selector: [name], [name=value], [name~=value],
             * [name|=value], [name^=value], [name$=value], [name*=value],
             * each optionally followed by an i/s case flag. */
            i++;
            while (i < n && css_is_space(s[i])) i++;
            size_t attr_start = i;
            while (i < n && s[i] != '=' && s[i] != '~' && s[i] != '|' &&
                   s[i] != '^' && s[i] != '$' && s[i] != '*' && s[i] != ']' &&
                   !css_is_space(s[i])) i++;
            size_t attr_end = i;
            while (i < n && css_is_space(s[i])) i++;
            int op = CSS_ATTR_OP_EXISTS;
            bool ci = false;
            char val[96]; val[0] = '\0';
            bool bad = false;
            if (i < n && s[i] != ']') {
                char c = s[i];
                if (c == '=') { op = CSS_ATTR_OP_EQ; i++; }
                else if (i + 1 < n && s[i + 1] == '=') {
                    switch (c) {
                    case '~': op = CSS_ATTR_OP_WORD; break;
                    case '|': op = CSS_ATTR_OP_DASH; break;
                    case '^': op = CSS_ATTR_OP_PREFIX; break;
                    case '$': op = CSS_ATTR_OP_SUFFIX; break;
                    case '*': op = CSS_ATTR_OP_SUBSTR; break;
                    default: bad = true; break;
                    }
                    i += 2;
                } else bad = true;
                while (i < n && css_is_space(s[i])) i++;
                size_t vl = 0;
                if (!bad && i < n && (s[i] == '"' || s[i] == '\'')) {
                    char q = s[i++];
                    while (i < n && s[i] != q) {
                        char ch = s[i];
                        if (ch == '\\' && i + 1 < n) { ch = s[++i]; }
                        if (vl + 1 < sizeof(val)) val[vl++] = ch;
                        i++;
                    }
                    if (i < n) i++;
                } else if (!bad) {
                    while (i < n && s[i] != ']' && !css_is_space(s[i])) {
                        char ch = s[i];
                        if (ch == '\\' && i + 1 < n) { ch = s[++i]; }
                        if (vl + 1 < sizeof(val)) val[vl++] = ch;
                        i++;
                    }
                }
                val[vl] = '\0';
                while (i < n && css_is_space(s[i])) i++;
                if (i < n && (s[i] == 'i' || s[i] == 'I')) { ci = true; i++; }
                else if (i < n && (s[i] == 's' || s[i] == 'S')) { i++; }
            }
            if (attr_end > attr_start && out->attr_count < CSS_MAX_ATTRS) {
                int k = out->attr_count;
                css_strncpy_lower(out->attrs[k], s + attr_start,
                                  attr_end - attr_start, sizeof(out->attrs[0]));
                out->attr_ops[k] = (unsigned char)op;
                out->attr_ci[k] = ci;
                memcpy(out->attr_vals[k], val, sizeof(val));
                if (bad) out->never_match = true;
                out->attr_count++;
                out->has_substantive = true;
            }
            /* Skip to the closing bracket. */
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

        int comb = CSS_COMB_DESCENDANT;
        if (s[i] == '>' || s[i] == '+' || s[i] == '~') {
            comb = s[i] == '>' ? CSS_COMB_CHILD
                 : s[i] == '+' ? CSS_COMB_ADJACENT : CSS_COMB_SIBLING;
            i++;
            css_skip_space(s, len, &i);
            if (i >= len) break;
        }
        CssSelectorPart *part = &parts[count++];
        part->combinator = (count == 1) ? CSS_COMB_NONE : comb;

        /* One compound: stop at top-level whitespace or a combinator, but not
         * inside [...], (...) or quotes (e.g. :nth-child(2n+1), [a="x y"]). */
        size_t start = i;
        bool in_quote = false;
        char quote_char = 0;
        int depth = 0;
        while (i < len) {
            char c = s[i];
            if (in_quote) {
                if (c == '\\' && i + 1 < len) i++;
                else if (c == quote_char) in_quote = false;
            } else if (c == '\\' && i + 1 < len) {
                i++;
            } else if (c == '"' || c == '\'') {
                in_quote = true; quote_char = c;
            } else if (c == '[' || c == '(') {
                depth++;
            } else if (c == ']' || c == ')') {
                if (depth > 0) depth--;
            } else if (depth == 0 && (css_is_space(c) || c == '>' || c == '+' || c == '~')) {
                break;
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
    b += simple->attr_count;
    b += (simple->child_pos ? 1 : 0) + (simple->type_pos ? 1 : 0) +
         (simple->nth_kind ? 1 : 0) + (simple->is_root ? 1 : 0) +
         (simple->requires_link ? 1 : 0) + (simple->requires_empty ? 1 : 0);
    if (simple->not_active) {
        a += simple->not_has_id ? 1 : 0;
        b += simple->not_class_count + simple->not_attr_count;
    }
    int c = (simple->has_tag && !simple->universal) ? 1 : 0;
    if (simple->not_active && simple->not_has_tag) c++;
    return a * 1000 + b * 10 + c;
}

static int css_specificity_from_chain(const CssSelectorPart *parts, int count);
int css_sel_parse_chain(const char *s, CssSelectorPart *parts, int max_parts) {
    return css_parse_selector_chain(s, parts, max_parts);
}
int css_sel_chain_specificity(const CssSelectorPart *parts, int count) {
    return css_specificity_from_chain(parts, count);
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

/* 1-based position of `node` among its element siblings; when matching type,
 * only siblings with the same tag are counted.  from_end counts from the
 * last sibling backwards. */
static int css_element_index(HtmlDocument *doc, HtmlNode *node,
                             bool same_type, bool from_end)
{
    if (!doc) return 0; /* cannot verify position: positional pseudo fails */
    HtmlNode *parent = html_node_parent_node(doc, node);
    if (!parent) return 1;
    int pos = 0, total = 0;
    int pidx = po_array_index_from_payload(&doc->array, parent);
    if (pidx < 0) return 1;
    for (int c = po_array_first_child(&doc->array, pidx); c >= 0;
         c = po_array_next_sibling(&doc->array, c)) {
        HtmlNode *sib = (HtmlNode*)po_array_payload(&doc->array, c);
        if (!sib || sib->type != HTML_NODE_ELEMENT) continue;
        if (same_type && strcasecmp(sib->tag_name, node->tag_name) != 0) continue;
        total++;
        if (sib == node) pos = total;
    }
    if (pos == 0) return 0;
    return from_end ? (total - pos + 1) : pos;
}

static bool css_nth_matches(int a, int b, int pos)
{
    if (pos <= 0) return false;
    if (a == 0) return pos == b;
    int d = pos - b;
    if (d % a != 0) return false;
    return d / a >= 0;
}

static bool css_simple_matches_ex(const CssSimpleSelector *simple, HtmlDocument *doc,
                                  HtmlNode *node) {
    if (!node || node->type != HTML_NODE_ELEMENT) return false;
    /* A selector with no concrete key (tag/id/class/attr/universal/root) —
     * e.g. a bare pseudo-element like ::before or ::view-transition-old(...)
     * — does not match any real element.  Without this, such selectors would
     * fall through to the "universal" return-true below and erroneously match
     * every node, applying pseudo-element-only declarations (often
     * display:none) to real elements. */
    if (!simple->has_substantive) return false;
    if (simple->never_match) return false;
    if (simple->is_root) {
        /* :root matches the document root element (html). */
        if (strcasecmp(node->tag_name, "html") != 0) return false;
    }
    if (simple->requires_link) {
        /* :link matches unvisited <a>/<area> with an href; this engine keeps
         * no browsing history, so every href link is unvisited. */
        bool link_tag = strcasecmp(node->tag_name, "a") == 0 ||
                        strcasecmp(node->tag_name, "area") == 0;
        if (!link_tag || !html_node_attr_value(node, "href")) return false;
    }
    if (simple->has_id) {
        const char *id = html_node_attr_value(node, "id");
        if (!id || strcasecmp(id, simple->id) != 0) return false;
    }
    for (int i = 0; i < simple->class_count; i++) {
        if (!html_node_class_contains(node, simple->classes[i])) return false;
    }
    for (int i = 0; i < simple->attr_count; i++) {
        if (!css_attr_value_matches(simple->attr_ops[i], simple->attr_ci[i],
                                    simple->attr_vals[i],
                                    html_node_attr_value(node, simple->attrs[i])))
            return false;
    }
    if (simple->requires_empty) {
        /* :empty matches an element with no child nodes (elements or text).
         * The compaction-array first_child index is < 0 when there are none. */
        if (node->array_node.first_child >= 0) return false;
    }
    if (simple->child_pos) {
        int pos = css_element_index(doc, node, false, simple->child_pos == 2);
        if (simple->child_pos == 3) {
            if (css_element_index(doc, node, false, false) != 1 ||
                css_element_index(doc, node, false, true) != 1) return false;
        } else if (pos != 1) return false;
    }
    if (simple->type_pos) {
        int pos = css_element_index(doc, node, true, simple->type_pos == 2);
        if (simple->type_pos == 3) {
            if (css_element_index(doc, node, true, false) != 1 ||
                css_element_index(doc, node, true, true) != 1) return false;
        } else if (pos != 1) return false;
    }
    if (simple->nth_kind) {
        bool same_type = simple->nth_kind >= 3;
        bool from_end = (simple->nth_kind % 2) == 0;
        int pos = css_element_index(doc, node, same_type, from_end);
        if (!css_nth_matches(simple->nth_a, simple->nth_b, pos)) return false;
    }
    if (simple->not_active) {
        /* Negated simple argument: the node must NOT match all of it. */
        bool hit = true;
        if (simple->not_has_tag &&
            strcasecmp(node->tag_name, simple->not_tag) != 0) hit = false;
        if (hit && simple->not_has_id) {
            const char *id = html_node_attr_value(node, "id");
            if (!id || strcasecmp(id, simple->not_id) != 0) hit = false;
        }
        if (hit) {
            for (int i = 0; i < simple->not_class_count && hit; i++) {
                if (!html_node_class_contains(node, simple->not_classes[i])) hit = false;
            }
        }
        if (hit) {
            for (int i = 0; i < simple->not_attr_count && hit; i++) {
                if (!css_attr_value_matches(simple->not_attr_ops[i], simple->not_attr_ci[i],
                                            simple->not_attr_vals[i],
                                            html_node_attr_value(node, simple->not_attrs[i])))
                    hit = false;
            }
        }
        if (hit) return false;
    }
    if (simple->has_active) return false;   /* compiled matcher only */
    if (simple->where_active) {
        /* :where()/:is() — the element must match the inner selector. */
        if (!css_selector_matches(simple->where_arg, doc, node)) return false;
    }
    if (simple->has_tag) {
        if (strcasecmp(node->tag_name, simple->tag) != 0) return false;
    }
    /* Universal with no id/class/attr always true. */
    return true;
}

static bool css_simple_matches(const CssSimpleSelector *simple, HtmlNode *node) {
    return css_simple_matches_ex(simple, NULL, node);
}

static bool css_chain_matches(const CssSelectorPart *parts, int count,
                              HtmlDocument *doc, HtmlNode *node) {
    if (count <= 0) return false;
    /* The key (rightmost) simple selector determines what the rule targets.
     * If it is a pseudo-element (::before, ::-webkit-scrollbar, ...) the rule
     * styles an abstract box that has no real DOM node, so it never matches. */
    if (parts[count - 1].simple.is_pseudo_element) return false;
    HtmlNode *current = node;
    /* Guard against cycles in the DOM parent chain. The tree should be
     * acyclic; this cap makes matching robust if a malformed/adopted tree
     * introduces a parent loop. */
    const int MAX_PARENT_WALK = 4096;
    for (int i = count - 1; i >= 0; i--) {
        if (!current) return false;
        if (!css_simple_matches_ex(&parts[i].simple, doc, current)) return false;
        if (i == 0) return true;

        int comb = parts[i].combinator; /* relates part i to part i-1 */
        if (comb == CSS_COMB_CHILD) {
            current = html_node_parent_node(doc, current);
        } else if (comb == CSS_COMB_ADJACENT || comb == CSS_COMB_SIBLING) {
            HtmlNode *sib = NULL;
            int idx = po_array_index_from_payload(&doc->array, current);
            int prev = -1;
            int par = idx >= 0 ? po_array_parent(&doc->array, idx) : -1;
            if (par < 0) return false;
            for (int c = po_array_first_child(&doc->array, par); c >= 0 && c != idx;
                 c = po_array_next_sibling(&doc->array, c)) {
                HtmlNode *cn = (HtmlNode *)po_array_payload(&doc->array, c);
                if (!cn || cn->type != HTML_NODE_ELEMENT) continue;
                if (comb == CSS_COMB_ADJACENT) prev = c;
                else if (css_simple_matches_ex(&parts[i - 1].simple, doc, cn)) prev = c;
            }
            if (prev >= 0) sib = (HtmlNode *)po_array_payload(&doc->array, prev);
            if (!sib) return false;
            current = sib;
        } else { /* descendant */
            current = html_node_parent_node(doc, current);
            int steps = 0;
            while (current && !css_simple_matches_ex(&parts[i - 1].simple, doc, current)) {
                current = html_node_parent_node(doc, current);
                if (++steps > MAX_PARENT_WALK) return false;
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
        int paren_depth = 0;
        bool in_quote = false;
        char quote_char = 0;
        while (i < len) {
            char c = selector[i];
            if (!in_quote) {
                if (c == '"' || c == '\'') { in_quote = true; quote_char = c; }
                else if (c == '(') paren_depth++;
                else if (c == ')') { if (paren_depth > 0) paren_depth--; }
                else if (c == ',' && paren_depth == 0) break;
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

/* Compile a selector string into a cached CssCompiledSelector. Splits on
 * top-level commas (respecting quotes/parens) and parses each group into a
 * chain. Returns NULL if no usable chain was produced. */
static CssCompiledSelector* css_compile_selector(const char *selector) {
    if (!selector || !selector[0]) return NULL;
    CssCompiledSelector *cs = (CssCompiledSelector*)calloc(1, sizeof(*cs));
    if (!cs) return NULL;

    int cap = 4;
    cs->chains = (CssSelectorChain*)malloc((size_t)cap * sizeof(CssSelectorChain));
    if (!cs->chains) { free(cs); return NULL; }

    size_t len = strlen(selector);
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        int paren_depth = 0;
        bool in_quote = false;
        char quote_char = 0;
        while (i < len) {
            char c = selector[i];
            if (!in_quote) {
                if (c == '"' || c == '\'') { in_quote = true; quote_char = c; }
                else if (c == '(') paren_depth++;
                else if (c == ')') { if (paren_depth > 0) paren_depth--; }
                else if (c == ',' && paren_depth == 0) break;
            } else {
                if (c == quote_char) in_quote = false;
            }
            i++;
        }
        char *part = css_strndup_trim(selector + start, i - start);
        if (part && part[0]) {
            if (cs->chain_count >= cap) {
                int new_cap = cap * 2;
                CssSelectorChain *new_chains = (CssSelectorChain*)realloc(
                    cs->chains, (size_t)new_cap * sizeof(CssSelectorChain));
                if (!new_chains) { free(part); break; }
                cs->chains = new_chains;
                cap = new_cap;
            }
            CssSelectorChain *chain = &cs->chains[cs->chain_count];
            chain->part_count = css_parse_selector_chain(part, chain->parts,
                                                        CSS_MAX_SIMPLE_PARTS);
            if (chain->part_count > 0) cs->chain_count++;
        }
        free(part);
        if (i < len && selector[i] == ',') i++;
    }

    if (cs->chain_count == 0) {
        free(cs->chains);
        free(cs);
        return NULL;
    }
    return cs;
}

void css_rule_compiled_free(CssRule *rule) {
    if (!rule || !rule->compiled) return;
    free(rule->compiled->chains);
    free(rule->compiled);
    rule->compiled = NULL;
}

bool css_rule_matches(const CssRule *rule, HtmlDocument *doc, HtmlNode *node) {
    if (!rule || !rule->selector_text || !node) return false;

    /* Lazily compile and cache the selector. The rule is logically const from
     * the caller's perspective; the cache is an internal mutable detail. */
    CssCompiledSelector *cs = rule->compiled;
    if (!cs) {
        cs = css_compile_selector(rule->selector_text);
        ((CssRule*)rule)->compiled = cs;  /* may be NULL if uncompileable */
    }
    if (!cs) return false;

    for (int c = 0; c < cs->chain_count; c++) {
        const CssSelectorChain *chain = &cs->chains[c];
        if (css_chain_matches(chain->parts, chain->part_count, doc, node)) {
            return true;
        }
    }
    return false;
}

/* ============================================================================
 * Style application
 * ============================================================================ */

int css_applied_decl_compare(const void *a, const void *b) {
    const CssAppliedDecl *da = (const CssAppliedDecl*)a;
    const CssAppliedDecl *db = (const CssAppliedDecl*)b;
    /* Per CSS Cascade spec: !important declarations win over normal,
     * regardless of specificity.  Within the same importance level,
     * higher specificity wins; ties broken by source order. */
    if (da->important != db->important) return da->important ? 1 : -1;
    if (da->specificity != db->specificity) return da->specificity - db->specificity;
    return da->order - db->order;
}

void css_set_style_property(JSContextHandle ctx, GCValue style, const char *prop, const char *value) {
    if (!prop || !value) return;
    if (css_style_decl_is(ctx, style)) { css_style_decl_set(ctx, style, prop, value, ""); return; }
    JS_SetPropertyStr(ctx, style, prop, JS_NewString(ctx, value));
    char *camel = css_to_camel_case(prop);
    if (camel) {
        if (strcmp(camel, prop) != 0) {
            JS_SetPropertyStr(ctx, style, camel, JS_NewString(ctx, value));
        }
        free(camel);
    }
}

GCValue css_ensure_style_object(JSContextHandle ctx, GCValue element) {
    /* Store wrappers keep their style object in the wrapper record. */
    if (dom_wrap_of(element) != GC_HANDLE_NULL) return dom_element_style(ctx, element, true);
    /* Use a private internal slot so we don't recurse through the public
     * `style` accessor getter when it calls this helper. */
    GCValue style = JS_GetPropertyStr(ctx, element, "__style");
    if (JS_IsUndefined(style) || JS_IsNull(style) || !JS_IsObject(style)) {
        style = css_style_decl_new(ctx, DOM_NULL, NULL);
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

void css_apply_inline_style_to(JSContextHandle ctx, GCValue style, const char *style_attr) {
    if (!style_attr || !style_attr[0] || !JS_IsObject(style)) return;
    int count = 0;
    CssDeclaration *decls = css_parse_inline_style(style_attr, &count);
    if (!decls) return;
    for (int i = 0; i < count; i++)
        css_set_style_property(ctx, style, decls[i].property, decls[i].value);
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
/* Recompute specificity from raw selector text (used when not precomputed). */
int css_specificity_from_selector_text(const char *selector) {
    CssSelectorPart parts[CSS_MAX_SIMPLE_PARTS];
    int count = css_parse_selector_chain(selector, parts, CSS_MAX_SIMPLE_PARTS);
    return css_specificity_from_chain(parts, count);
}

/* Specificity of a selector LIST against a concrete node: per CSS Cascade,
 * a rule's specificity is that of the most specific complex selector (list
 * branch) that matches the element — not the sum over all branches. */
int css_specificity_from_selector_text_matching(const char *selector,
                                                HtmlDocument *doc,
                                                HtmlNode *node) {
    int best = 0;
    if (!selector) return 0;
    size_t len = strlen(selector);
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        int paren_depth = 0;
        bool in_quote = false;
        char quote_char = 0;
        while (i < len) {
            char c = selector[i];
            if (!in_quote) {
                if (c == '"' || c == '\'') { in_quote = true; quote_char = c; }
                else if (c == '(') paren_depth++;
                else if (c == ')') { if (paren_depth > 0) paren_depth--; }
                else if (c == ',' && paren_depth == 0) break;
            } else {
                if (c == quote_char) in_quote = false;
            }
            i++;
        }
        char *part = css_strndup_trim(selector + start, i - start);
        if (part && part[0]) {
            if (css_selector_matches_one(part, doc, node)) {
                CssSelectorPart parts[CSS_MAX_SIMPLE_PARTS];
                int count = css_parse_selector_chain(part, parts,
                                                     CSS_MAX_SIMPLE_PARTS);
                int spec = css_specificity_from_chain(parts, count);
                if (spec > best) best = spec;
            }
            free(part);
        } else {
            free(part);
        }
        if (i < len && selector[i] == ',') i++;
    }
    return best;
}

/* ============================================================================
 * Media query evaluation
 *
 * Media Queries 4 against the one medium there is: a dark-scheme screen of
 * the viewport's size at 1dppx, with a fine pointer that hovers.  Types:
 * screen/all match, everything else (print, speech, ...) does not.  Plain
 * (min-width: 40em), boolean (hover) and range (400px <= width < 60em)
 * features; not/and/or conditions.  An unknown feature or malformed
 * condition is "unknown", which (even under not) never matches.
 * ============================================================================ */

static void css_media_skip_ws(const char **sp) {
    while (**sp == ' ' || **sp == '\t' || **sp == '\n' || **sp == '\r' || **sp == '\f') (*sp)++;
}

static double css_media_expr(const char **sp);

static double css_media_number(const char **sp) {
    css_media_skip_ws(sp);
    char *end = NULL;
    double v = strtod(*sp, &end);
    if (end == *sp) return 0.0;
    *sp = end;
    static const struct { const char *u; double px; } units[] = {
        { "rem", 16.0 }, { "em", 16.0 }, { "px", 1.0 }, { "in", 96.0 }, { "cm", 96.0 / 2.54 },
        { "mm", 96.0 / 25.4 }, { "pt", 96.0 / 72.0 }, { "pc", 16.0 }, { "q", 96.0 / 101.6 },
    };
    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        size_t n = strlen(units[i].u);
        if (strncasecmp(*sp, units[i].u, n) == 0 && !isalpha((unsigned char)(*sp)[n])) {
            *sp += n;
            return v * units[i].px;
        }
    }
    return v;
}

static double css_media_factor(const char **sp) {
    css_media_skip_ws(sp);
    if (**sp == '(') {
        (*sp)++;
        double v = css_media_expr(sp);
        css_media_skip_ws(sp);
        if (**sp == ')') (*sp)++;
        return v;
    }
    if (**sp == '-') { (*sp)++; return -css_media_factor(sp); }
    if (**sp == '+') { (*sp)++; return css_media_factor(sp); }
    return css_media_number(sp);
}

static double css_media_term(const char **sp) {
    double v = css_media_factor(sp);
    for (;;) {
        css_media_skip_ws(sp);
        if (**sp == '*') { (*sp)++; v *= css_media_factor(sp); }
        else if (**sp == '/') {
            (*sp)++;
            double d = css_media_factor(sp);
            if (d != 0.0) v /= d;
        }
        else return v;
    }
}

static double css_media_expr(const char **sp) {
    double v = css_media_term(sp);
    for (;;) {
        css_media_skip_ws(sp);
        if (**sp == '+') { (*sp)++; v += css_media_term(sp); }
        else if (**sp == '-') { (*sp)++; v -= css_media_term(sp); }
        else return v;
    }
}

/* Parse a media length: a plain number with optional unit, or calc(...). */
static double css_media_length(const char **sp) {
    css_media_skip_ws(sp);
    if (strncasecmp(*sp, "calc", 4) == 0) {
        *sp += 4;
        css_media_skip_ws(sp);
        if (**sp == '(') {
            (*sp)++;
            double v = css_media_expr(sp);
            css_media_skip_ws(sp);
            if (**sp == ')') (*sp)++;
            return v;
        }
    }
    return css_media_number(sp);
}

/* The [s, e) operand trimmed into buf (lower case when fold): false when
 * empty or too long. */
static bool mq_operand(const char *s, const char *e, char *buf, size_t cap, bool fold) {
    while (s < e && css_is_space(*s)) s++;
    while (e > s && css_is_space(e[-1])) e--;
    size_t n = (size_t)(e - s);
    if (n == 0 || n >= cap) return false;
    for (size_t i = 0; i < n; i++) buf[i] = fold ? (char)tolower((unsigned char)s[i]) : s[i];
    buf[n] = '\0';
    return true;
}

/* A whole operand as a length in px (or a plain number). */
static bool mq_length(const char *s, const char *e, double *out) {
    char buf[256];
    if (!mq_operand(s, e, buf, sizeof(buf), false)) return false;
    const char *p = buf;
    *out = css_media_length(&p);
    css_media_skip_ws(&p);
    return p != buf && !*p;
}

static bool mq_number(const char *s, const char *e, double *out) {
    char buf[64];
    if (!mq_operand(s, e, buf, sizeof(buf), false)) return false;
    char *end = NULL;
    *out = strtod(buf, &end);
    return end != buf && !*end;
}

/* A resolution in dppx: <n>dpi, dpcm, dppx or x. */
static bool mq_resolution(const char *s, const char *e, double *out) {
    char buf[64];
    if (!mq_operand(s, e, buf, sizeof(buf), true)) return false;
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf) return false;
    if (strcmp(end, "dppx") == 0 || strcmp(end, "x") == 0) *out = v;
    else if (strcmp(end, "dpi") == 0) *out = v / 96.0;
    else if (strcmp(end, "dpcm") == 0) *out = v * 2.54 / 96.0;
    else return false;
    return true;
}

/* A ratio: <n> or <n> / <n>. */
static bool mq_ratio(const char *s, const char *e, double *out) {
    const char *slash = memchr(s, '/', (size_t)(e - s));
    if (!slash) return mq_number(s, e, out);
    double a, b;
    if (!mq_number(s, slash, &a) || !mq_number(slash + 1, e, &b) || b == 0.0) return false;
    *out = a / b;
    return true;
}

enum { MQ_LEN, MQ_RATIO, MQ_RES, MQ_NUM, MQ_IDENT };

typedef struct MqFeature {
    const char *name;
    int type;
    double num;         /* the screen's value (MQ_IDENT: unused) */
    const char *ident;  /* MQ_IDENT: the screen's value */
} MqFeature;

/* The feature named name (lower case) on a vw x vh screen; false if unknown. */
static bool mq_feature_lookup(const char *name, double vw, double vh, MqFeature *f) {
    static const struct { const char *name, *value; } idents[] = {
        { "prefers-color-scheme", "dark" }, { "prefers-reduced-motion", "no-preference" },
        { "prefers-contrast", "no-preference" }, { "prefers-reduced-transparency", "no-preference" },
        { "prefers-reduced-data", "no-preference" }, { "forced-colors", "none" },
        { "inverted-colors", "none" }, { "-ms-high-contrast", "none" },
        { "hover", "hover" }, { "any-hover", "hover" }, { "pointer", "fine" }, { "any-pointer", "fine" },
        { "display-mode", "browser" }, { "color-gamut", "srgb" }, { "dynamic-range", "standard" },
        { "video-dynamic-range", "standard" }, { "update", "fast" }, { "overflow-block", "scroll" },
        { "overflow-inline", "scroll" }, { "scripting", "enabled" }, { "scan", "progressive" },
    };
    memset(f, 0, sizeof(*f));
    f->name = name;
    double ratio = vh > 0 ? vw / vh : 0;
    if (strcmp(name, "width") == 0 || strcmp(name, "device-width") == 0) { f->type = MQ_LEN; f->num = vw; }
    else if (strcmp(name, "height") == 0 || strcmp(name, "device-height") == 0) { f->type = MQ_LEN; f->num = vh; }
    else if (strcmp(name, "aspect-ratio") == 0 || strcmp(name, "device-aspect-ratio") == 0) { f->type = MQ_RATIO; f->num = ratio; }
    else if (strcmp(name, "resolution") == 0) { f->type = MQ_RES; f->num = 1.0; }
    else if (strcmp(name, "device-pixel-ratio") == 0) { f->type = MQ_NUM; f->num = 1.0; }
    else if (strcmp(name, "color") == 0) { f->type = MQ_NUM; f->num = 8.0; }
    else if (strcmp(name, "color-index") == 0 || strcmp(name, "monochrome") == 0 ||
             strcmp(name, "grid") == 0) { f->type = MQ_NUM; f->num = 0.0; }
    else if (strcmp(name, "orientation") == 0) { f->type = MQ_IDENT; f->ident = vh >= vw ? "portrait" : "landscape"; }
    else {
        for (size_t i = 0; i < sizeof(idents) / sizeof(idents[0]); i++)
            if (strcmp(name, idents[i].name) == 0) { f->type = MQ_IDENT; f->ident = idents[i].value; return true; }
        return false;
    }
    return true;
}

/* An operand of a range feature's type; false when it does not parse. */
static bool mq_value(const MqFeature *f, const char *s, const char *e, double *out) {
    switch (f->type) {
    case MQ_LEN:   return mq_length(s, e, out);
    case MQ_RATIO: return mq_ratio(s, e, out);
    case MQ_RES:   return mq_resolution(s, e, out);
    case MQ_NUM:   return mq_number(s, e, out);
    default:       return false;
    }
}

enum { MQ_LT, MQ_LE, MQ_GT, MQ_GE, MQ_EQ };

static bool mq_compare(double a, int op, double b) {
    const double eps = 1e-9;
    switch (op) {
    case MQ_LT: return a < b - eps;
    case MQ_LE: return a <= b + eps;
    case MQ_GT: return a > b + eps;
    case MQ_GE: return a >= b - eps;
    default:    return a >= b - eps && a <= b + eps;
    }
}

/* Whether [s, e) is a feature name (an identifier, not a number). */
static bool mq_is_name(const char *s, const char *e) {
    while (s < e && css_is_space(*s)) s++;
    if (s >= e) return false;
    if (*s == '-') s++;
    return s < e && (isalpha((unsigned char)*s) || *s == '-');
}

/* A feature in parentheses (its text [s, e)): 1, 0, or -1 for unknown. */
static int mq_feature(const char *s, const char *e, double vw, double vh) {
    char name[64];
    MqFeature f;
    /* Range form: up to two comparison operators at depth 0. */
    const char *ops[2] = { NULL, NULL };
    int opk[2] = { 0, 0 }, opn[2] = { 0, 0 }, nops = 0, depth = 0;
    const char *colon = NULL;
    for (const char *p = s; p < e; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        else if (depth == 0 && *p == ':' && !colon) colon = p;
        else if (depth == 0 && (*p == '<' || *p == '>' || *p == '=')) {
            if (nops == 2) return -1;
            int k, n = 1;
            if (*p == '=') k = MQ_EQ;
            else if (p + 1 < e && p[1] == '=') { k = *p == '<' ? MQ_LE : MQ_GE; n = 2; }
            else k = *p == '<' ? MQ_LT : MQ_GT;
            ops[nops] = p; opk[nops] = k; opn[nops] = n; nops++;
            p += n - 1;
        }
    }
    if (colon && nops) return -1;

    if (colon) {
        if (!mq_operand(s, colon, name, sizeof(name), true)) return -1;
        const char *n = name;
        if (strncmp(n, "-webkit-", 8) == 0 && strstr(n, "device-pixel-ratio")) n += 8;
        int prefix = 0;     /* 1 min-, 2 max- */
        if (strncmp(n, "min-", 4) == 0) { prefix = 1; n += 4; }
        else if (strncmp(n, "max-", 4) == 0) { prefix = 2; n += 4; }
        if (!mq_feature_lookup(n, vw, vh, &f)) return -1;
        if (f.type == MQ_IDENT) {
            char v[64];
            if (prefix || !mq_operand(colon + 1, e, v, sizeof(v), true)) return -1;
            return strcmp(v, f.ident) == 0;
        }
        double v;
        if (!mq_value(&f, colon + 1, e, &v)) return -1;
        return mq_compare(f.num, prefix == 1 ? MQ_GE : prefix == 2 ? MQ_LE : MQ_EQ, v);
    }

    if (!nops) {
        /* Boolean context: true unless the value is zero, none or no-preference. */
        if (!mq_operand(s, e, name, sizeof(name), true) || !mq_feature_lookup(name, vw, vh, &f)) return -1;
        if (f.type == MQ_IDENT) return strcmp(f.ident, "none") != 0 && strcmp(f.ident, "no-preference") != 0;
        return f.num != 0.0;
    }

    /* "name op value", "value op name" or "value op name op value". */
    const char *a_end = ops[0], *b_start = ops[0] + opn[0];
    if (nops == 1) {
        bool name_first = mq_is_name(s, a_end);
        const char *ns = name_first ? s : b_start, *ne = name_first ? a_end : e;
        const char *vs = name_first ? b_start : s, *ve = name_first ? e : a_end;
        if (!mq_operand(ns, ne, name, sizeof(name), true) || !mq_feature_lookup(name, vw, vh, &f) ||
            f.type == MQ_IDENT) return -1;
        double v;
        if (!mq_value(&f, vs, ve, &v)) return -1;
        return name_first ? mq_compare(f.num, opk[0], v) : mq_compare(v, opk[0], f.num);
    }
    /* Both operators point the same way (< or >, neither =). */
    bool lt0 = opk[0] == MQ_LT || opk[0] == MQ_LE, lt1 = opk[1] == MQ_LT || opk[1] == MQ_LE;
    if (opk[0] == MQ_EQ || opk[1] == MQ_EQ || lt0 != lt1) return -1;
    if (!mq_operand(b_start, ops[1], name, sizeof(name), true) || !mq_feature_lookup(name, vw, vh, &f) ||
        f.type == MQ_IDENT) return -1;
    double lo, hi;
    if (!mq_value(&f, s, a_end, &lo) || !mq_value(&f, ops[1] + opn[1], e, &hi)) return -1;
    return mq_compare(lo, opk[0], f.num) && mq_compare(f.num, opk[1], hi);
}

typedef struct Mq {
    const char *p, *end;
    double vw, vh;
    bool bad;           /* malformed */
} Mq;

static void mq_ws(Mq *m) {
    while (m->p < m->end && css_is_space(*m->p)) m->p++;
}

/* The keyword word next (case-insensitive, then a space or '('): consumed. */
static bool mq_word(Mq *m, const char *word) {
    mq_ws(m);
    size_t n = strlen(word);
    if ((size_t)(m->end - m->p) <= n || strncasecmp(m->p, word, n) != 0) return false;
    char c = m->p[n];
    if (!css_is_space(c) && c != '(') return false;
    m->p += n;
    return true;
}

static int mq_condition(Mq *m);

/* "( condition )" or "( feature )"; anything else in parentheses is unknown. */
static int mq_in_parens(Mq *m) {
    mq_ws(m);
    if (m->p >= m->end || *m->p != '(') { m->bad = true; return -1; }
    const char *close = m->p + 1;
    int depth = 1;
    for (; close < m->end; close++) {
        if (*close == '(') depth++;
        else if (*close == ')' && --depth == 0) break;
    }
    if (close >= m->end) { m->bad = true; return -1; }
    Mq in = { m->p + 1, close, m->vw, m->vh, false };
    mq_ws(&in);
    int r;
    if (in.p < in.end && (*in.p == '(' || mq_word(&in, "not"))) {
        in.p = m->p + 1;
        r = mq_condition(&in);
        mq_ws(&in);
        if (in.bad || in.p != in.end) r = -1;
    } else {
        r = mq_feature(m->p + 1, close, m->vw, m->vh);
    }
    m->p = close + 1;
    return r;
}

/* not <in-parens> | <in-parens> [and <in-parens>]* | <in-parens> [or <in-parens>]*,
 * three-valued (-1 unknown). */
static int mq_condition(Mq *m) {
    if (mq_word(m, "not")) {
        int r = mq_in_parens(m);
        return r < 0 ? -1 : !r;
    }
    int r = mq_in_parens(m);
    int joiner = 0;     /* 1 and, 2 or */
    for (;;) {
        int j = mq_word(m, "and") ? 1 : mq_word(m, "or") ? 2 : 0;
        if (!j) break;
        if (joiner && j != joiner) { m->bad = true; return -1; }   /* mixed without parens */
        joiner = j;
        int t = mq_in_parens(m);
        if (j == 1) r = (r == 0 || t == 0) ? 0 : (r < 0 || t < 0) ? -1 : 1;
        else        r = (r == 1 || t == 1) ? 1 : (r < 0 || t < 0) ? -1 : 0;
    }
    return r;
}

/* One query of a list, [s, e). */
static bool mq_query(const char *s, const char *e, double vw, double vh) {
    Mq m = { s, e, vw, vh, false };
    mq_ws(&m);
    if (m.p >= m.end) return false;
    /* "(...)" and "not (...)" are a condition; otherwise a media type leads. */
    const char *start = m.p;
    bool condition = *m.p == '(';
    if (!condition && mq_word(&m, "not")) {
        mq_ws(&m);
        condition = m.p < m.end && *m.p == '(';
    }
    m.p = start;
    if (condition) {
        int r = mq_condition(&m);
        mq_ws(&m);
        return !m.bad && m.p == m.end && r == 1;
    }
    bool negate = false;
    if (mq_word(&m, "not")) negate = true;
    else mq_word(&m, "only");
    mq_ws(&m);
    const char *t = m.p;
    while (m.p < m.end && (isalnum((unsigned char)*m.p) || *m.p == '-')) m.p++;
    size_t tl = (size_t)(m.p - t);
    if (!tl) return false;
    bool type = (tl == 6 && strncasecmp(t, "screen", 6) == 0) || (tl == 3 && strncasecmp(t, "all", 3) == 0);
    int r = type ? 1 : 0;
    mq_ws(&m);
    if (m.p < m.end) {
        if (!mq_word(&m, "and")) return false;
        int c;
        if (mq_word(&m, "not")) { c = mq_in_parens(&m); c = c < 0 ? -1 : !c; }
        else {
            /* <condition-without-or> */
            c = mq_in_parens(&m);
            while (mq_word(&m, "and")) {
                int d = mq_in_parens(&m);
                c = (c == 0 || d == 0) ? 0 : (c < 0 || d < 0) ? -1 : 1;
            }
        }
        mq_ws(&m);
        if (m.bad || m.p != m.end || c < 0) return false;
        r = r && c == 1;
    }
    return negate ? !r : r;
}

bool css_media_query_matches(const char *query, double vw, double vh) {
    if (!query) return true;
    const char *s = query;
    while (css_is_space(*s)) s++;
    if (!*s) return true;
    /* A list matches when any comma-separated query does. */
    for (;;) {
        const char *comma = s;
        int depth = 0;
        for (; *comma; comma++) {
            if (*comma == '(') depth++;
            else if (*comma == ')') { if (depth > 0) depth--; }
            else if (*comma == ',' && depth == 0) break;
        }
        if (mq_query(s, comma, vw, vh)) return true;
        if (!*comma) return false;
        s = comma + 1;
    }
}

bool css_rule_media_matches(const CssRule *rule, double viewport_width, double viewport_height) {
    if (!rule || !rule->media_query || !rule->media_query[0]) return true;
    /* Nested @media blocks: every list (joined by CSS_MEDIA_AND) must match. */
    const char *s = rule->media_query;
    for (;;) {
        const char *sep = strchr(s, CSS_MEDIA_AND);
        size_t n = sep ? (size_t)(sep - s) : strlen(s);
        char stack[512];
        char *q = n < sizeof(stack) ? stack : (char *)malloc(n + 1);
        if (!q) return false;
        memcpy(q, s, n);
        q[n] = '\0';
        bool ok = css_media_query_matches(q, viewport_width, viewport_height);
        if (q != stack) free(q);
        if (!ok) return false;
        if (!sep) return true;
        s = sep + 1;
    }
}

static char* css_strdup(const char *s) {
    size_t n = strlen(s);
    char *out = (char*)malloc(n + 1);
    if (out) memcpy(out, s, n + 1);
    return out;
}

/* Return the index of the closing ')' matching the '(' at start. */
static size_t css_match_paren(const char *s, size_t start, size_t len) {
    int depth = 1;
    size_t i = start + 1;
    while (i < len) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') {
            depth--;
            if (depth == 0) return i;
        } else if (s[i] == '"' || s[i] == '\'') {
            char q = s[i++];
            while (i < len && s[i] != q) i++;
        }
        i++;
    }
    return len;
}

/* Append a formatted string to a dynamically-grown buffer. */
static void css_buf_append(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
    if (!*buf) {
        *cap = n + 64;
        *buf = (char*)malloc(*cap);
        if (!*buf) return;
        *len = 0;
    }
    if (*len + n + 1 > *cap) {
        size_t new_cap = *cap * 2;
        while (new_cap < *len + n + 1) new_cap *= 2;
        char *new_buf = (char*)realloc(*buf, new_cap);
        if (!new_buf) return;
        *buf = new_buf;
        *cap = new_cap;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
}

static void css_buf_append_cstr(char **buf, size_t *len, size_t *cap, const char *s) {
    css_buf_append(buf, len, cap, s, strlen(s));
}

/* Transform a single selector string for a given shadow host tag. */
static char* css_transform_one_selector(const char *sel, size_t len, const char *host_tag) {
    size_t i = 0;
    while (i < len && css_is_space(sel[i])) i++;
    size_t start = i;

    char *out = NULL;
    size_t out_len = 0, out_cap = 0;

    if (i + 6 <= len && strncasecmp(sel + i, ":host(", 6) == 0) {
        /* :host(<qualifier>) ...  ->  host_tag<qualifier> ... */
        size_t open = i + 5;
        size_t close = css_match_paren(sel, open, len);
        css_buf_append_cstr(&out, &out_len, &out_cap, host_tag);
        if (close > open + 1) {
            css_buf_append(&out, &out_len, &out_cap, sel + open + 1, close - open - 1);
        }
        i = close + 1;
        css_buf_append(&out, &out_len, &out_cap, sel + i, len - i);
    } else if (i + 5 <= len && strncasecmp(sel + i, ":host", 5) == 0) {
        /* :host ...  ->  host_tag ... */
        css_buf_append_cstr(&out, &out_len, &out_cap, host_tag);
        i += 5;
        css_buf_append(&out, &out_len, &out_cap, sel + i, len - i);
    } else if (i + 9 <= len && strncasecmp(sel + i, "::slotted", 9) == 0) {
        /* ::slotted(<qualifier>) ... -> host_tag > <qualifier> ... */
        size_t open = i + 8;
        size_t close = css_match_paren(sel, open, len);
        css_buf_append_cstr(&out, &out_len, &out_cap, host_tag);
        css_buf_append_cstr(&out, &out_len, &out_cap, " > ");
        if (close > open + 1) {
            css_buf_append(&out, &out_len, &out_cap, sel + open + 1, close - open - 1);
        } else {
            css_buf_append_cstr(&out, &out_len, &out_cap, "*");
        }
        i = close + 1;
        css_buf_append(&out, &out_len, &out_cap, sel + i, len - i);
    } else if (i + 14 <= len && strncasecmp(sel + i, ":host-context(", 14) == 0) {
        /* Drop :host-context rules; we cannot model outer ancestors. */
        free(out);
        return css_strdup("");
    } else {
        /* Default: scope to host descendants. */
        size_t host_len = strlen(host_tag);
        /* Don't double-scope if selector already starts with the host tag. */
        if (len > host_len && strncasecmp(sel + i, host_tag, host_len) == 0 &&
            (i + host_len >= len || css_is_space(sel[i + host_len]) || sel[i + host_len] == '.' ||
             sel[i + host_len] == '#' || sel[i + host_len] == '[' || sel[i + host_len] == ':' ||
             sel[i + host_len] == '>' || sel[i + host_len] == '+' || sel[i + host_len] == '~')) {
            css_buf_append(&out, &out_len, &out_cap, sel, len);
        } else {
            css_buf_append_cstr(&out, &out_len, &out_cap, host_tag);
            css_buf_append_cstr(&out, &out_len, &out_cap, " ");
            css_buf_append(&out, &out_len, &out_cap, sel, len);
        }
    }
    return out ? out : css_strdup("");
}

/* Split a selector list on commas and transform each part. */
static char* css_transform_selectors_for_host(const char *selector_text, const char *host_tag) {
    if (!selector_text || !host_tag) return css_strdup(selector_text ? selector_text : "");

    char *out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t len = strlen(selector_text);
    size_t i = 0;
    bool first = true;

    while (i <= len) {
        /* Skip leading whitespace for this part. */
        size_t part_start = i;
        size_t j = i;
        int depth = 0;
        bool in_str = false;
        char q = 0;
        while (j < len && !(selector_text[j] == ',' && depth == 0 && !in_str)) {
            char c = selector_text[j];
            if (!in_str) {
                if (c == '(') depth++;
                else if (c == ')') depth--;
                else if (c == '"' || c == '\'') { in_str = true; q = c; }
            } else if (c == q && (j == 0 || selector_text[j - 1] != '\\')) {
                in_str = false;
            }
            j++;
        }
        size_t part_len = j - part_start;
        char *part = css_transform_one_selector(selector_text + part_start, part_len, host_tag);
        if (part && part[0]) {
            if (!first) css_buf_append_cstr(&out, &out_len, &out_cap, ", ");
            css_buf_append_cstr(&out, &out_len, &out_cap, part);
            first = false;
        }
        free(part);
        i = j + 1;
    }
    return out ? out : css_strdup("");
}

void css_scope_stylesheet(CssStylesheet *sheet, const char *host_tag) {
    if (!sheet || !host_tag) return;
    for (int r = 0; r < sheet->rule_count; r++) {
        CssRule *rule = &sheet->rules[r];
        if (!rule->selector_text) continue;
        char *new_sel = css_transform_selectors_for_host(rule->selector_text, host_tag);
        free(rule->selector_text);
        rule->selector_text = new_sel;
        rule->specificity = 0;
        /* The selector text changed, so any cached parse is now stale. */
        css_rule_compiled_free(rule);
    }
}




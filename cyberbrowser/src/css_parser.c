/*
 * CSS Parser - Implementation
 *
 * A small, self-contained CSS parser for applying styles to the DOM tree.
 */

#include "css_parser.h"
#include "css_tokenizer.h"
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

/* A selector name's end from i: up to one of the stop characters, an
 * escaped character (\X) being part of the name. */
static size_t css_sel_name_end(const char *s, size_t n, size_t i, const char *stop)
{
    while (i < n && !strchr(stop, s[i])) {
        if (s[i] == '\\' && i + 1 < n) i++;
        i++;
    }
    return i;
}

/* A selector name copied lowercase, \X read as X (at most dst_size-1
 * bytes). */
static void css_sel_name_copy(char *dst, const char *src, size_t n, size_t dst_size)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < dst_size && src[i]; i++) {
        if (src[i] == '\\' && i + 1 < n) i++;
        dst[o++] = (char)tolower((unsigned char)src[i]);
    }
    dst[o] = '\0';
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
        /* (white space may come between the ! and important) */
        size_t vlen = strlen(v);
        if (vlen >= 10 && strncasecmp(v + vlen - 9, "important", 9) == 0) {
            size_t b = vlen - 9;
            while (b > 0 && css_is_space(v[b - 1])) b--;
            if (b > 0 && v[b - 1] == '!') {
                is_important = true;
                size_t trimmed = b - 1;
                v[trimmed] = '\0';
                while (trimmed > 0 && css_is_space(v[trimmed - 1])) v[--trimmed] = '\0';
            }
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
    rule->declarations[rule->declaration_count].invalid = 0;
    rule->declaration_count++;
    return true;
}

/* A character that a plain declaration block never has outside a string:
 * the token path (escapes, nested blocks, at-rules, brackets, comments,
 * CDO / CDC) reads those. */
static bool decl_complex_char(const char *s, size_t len, size_t i)
{
    char c = s[i];
    return c == '\\' || c == '{' || c == '@' || c == '[' || c == ']' || c == '<' ||
           (c == '/' && i + 1 < len && s[i + 1] == '*');
}

/* A ! that is the !important at a value's end (then white space and ; or }). */
static bool decl_bang_important(const char *s, size_t len, size_t i)
{
    i++;
    while (i < len && css_is_space(s[i])) i++;
    if (i + 9 > len || strncasecmp(s + i, "important", 9) != 0) return false;
    i += 9;
    while (i < len && css_is_space(s[i])) i++;
    return i >= len || s[i] == ';' || s[i] == '}';
}

/* The fast path for a declaration block between { and } (pos just past the
 * '{'): plain declarations scanned as text.  False, with nothing added and
 * pos as it was, on anything only the token path reads right. */
/* The characters the fast path looks at; any other is part of a name or a
 * value as it is. */
static const unsigned char k_decl_special[256] = {
    ['"'] = 1, ['\''] = 1, [':'] = 1, [';'] = 1, ['}'] = 1, ['{'] = 1, ['!'] = 1, ['\\'] = 1, ['@'] = 1,
    ['['] = 1, [']'] = 1, ['<'] = 1, ['/'] = 1, ['('] = 1, [')'] = 1, ['\n'] = 1, ['\r'] = 1, ['\f'] = 1,
};

/* Name characters (1), and those an identifier can start with (2: '-' is
 * one when what follows is one too). */
static const unsigned char k_name_char[256] = {
    ['a' ... 'z'] = 2, ['A' ... 'Z'] = 2, ['_'] = 2, ['-'] = 2, [0x80 ... 0xff] = 2, ['0' ... '9'] = 1,
};

static bool css_decl_block_fast(const char *s, size_t len, size_t *pos, CssRule *rule) {
    size_t pos0 = *pos;
    int n0 = rule->declaration_count;
    while (*pos < len) {
        css_skip_space(s, len, pos);
        if (*pos >= len) break;
        if (s[*pos] == '}') { (*pos)++; break; }
        if (s[*pos] == ';') { (*pos)++; continue; }

        /* Read property until ':'. */
        size_t prop_start = *pos;
        bool in_quote = false;
        char quote_char = 0;
        while (*pos < len) {
            char c = s[*pos];
            if (!k_decl_special[(unsigned char)c]) { (*pos)++; continue; }
            if (!in_quote) {
                if (c == '"' || c == '\'') { in_quote = true; quote_char = c; }
                else if (c == ':') break;
                else if (c == '}' || c == ';') break;
                else if (c == '!' || decl_complex_char(s, len, *pos)) goto bail;
            } else {
                if (c == quote_char) in_quote = false;
                else if (c == '\\' || c == '\n' || c == '\r' || c == '\f') goto bail;
            }
            (*pos)++;
        }
        size_t prop_end = *pos;
        /* (a property that is not one identifier: the token path's) */
        size_t pe = prop_end;
        while (pe > prop_start && css_is_space(s[pe - 1])) pe--;
        if (pe == prop_start || k_name_char[(unsigned char)s[prop_start]] != 2 ||
            (s[prop_start] == '-' && (pe - prop_start < 2 || k_name_char[(unsigned char)s[prop_start + 1]] != 2)))
            goto bail;
        for (size_t i = prop_start + 1; i < pe; i++)
            if (!k_name_char[(unsigned char)s[i]]) goto bail;
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
                if (!k_decl_special[(unsigned char)c]) { (*pos)++; continue; }
                if (!in_quote) {
                    if (c == '"' || c == '\'') { in_quote = true; quote_char = c; }
                    else if (c == '(') { paren_depth++; }
                    else if (c == ')') { if (paren_depth > 0) paren_depth--; }
                    else if (paren_depth == 0 && (c == ';' || c == '}')) break;
                    else if (decl_complex_char(s, len, *pos) || (c == '!' && !decl_bang_important(s, len, *pos)))
                        goto bail;
                } else {
                    if (c == quote_char) in_quote = false;
                    else if (c == '\\' || c == '\n' || c == '\r' || c == '\f') goto bail;
                }
                (*pos)++;
            }
            size_t val_end = *pos;
            css_declaration_add(rule, s + prop_start, prop_end - prop_start,
                                s + val_start, val_end - val_start);
            if (*pos < len && s[*pos] == ';') (*pos)++;
        } else {
            goto bail;   /* (a malformed declaration: the token path's recovery) */
        }
    }
    return true;
bail:
    /* (back to where it began, for the token path) */
    for (int i = n0; i < rule->declaration_count; i++) {
        free(rule->declarations[i].property);
        free(rule->declarations[i].value);
    }
    rule->declaration_count = n0;
    *pos = pos0;
    return false;
}

/* ---- Declaration lists (CSS Syntax 3 section 5.4.5), from tokens ----
 * A declaration is an identifier, white space, a colon and its value: the
 * component values up to a semicolon at its own nesting level (blocks
 * and functions nest).  !important is the value's last two tokens, a !
 * and the identifier important.  An at-rule in a declaration list is
 * ignored with its block; anything else that is not a declaration is
 * skipped up to its semicolon, or through its {} block (a nested rule, not
 * kept).  A declaration with a bad string or url in it is dropped.  Properties are their names with escapes
 * resolved, lower case; values the source text (trimmed), or rebuilt from
 * the tokens when an escape or a comment is in it. */

typedef struct { CssToken *t; int n, cap; } TokBuf;

static bool tokbuf_push(TokBuf *b, const CssToken *t)
{
    if (b->n == b->cap) {
        int nc = b->cap ? b->cap * 2 : 32;
        CssToken *nt = (CssToken *)realloc(b->t, (size_t)nc * sizeof(CssToken));
        if (!nt) return false;
        b->t = nt;
        b->cap = nc;
    }
    b->t[b->n++] = *t;
    return true;
}

/* Where a declaration goes. */
typedef void (*DeclSink)(void *ctx, char *prop, char *value, bool important);

/* Append text to a growing string. */
typedef struct { char *p; size_t n, cap; } Str;

static void str_put(Str *o, const char *p, size_t l)
{
    if (o->n + l + 1 > o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 64;
        while (nc < o->n + l + 1) nc *= 2;
        char *np = (char *)realloc(o->p, nc);
        if (!np) return;
        o->p = np;
        o->cap = nc;
    }
    memcpy(o->p + o->n, p, l);
    o->n += l;
    o->p[o->n] = 0;
}

/* A name (escapes resolved) as value text that reads back as the same
 * token: a character that is not a name character, or a digit that would
 * start it (ident), as a hex escape ("test\f oo"). */
static void val_put_name(Str *o, const char *v, size_t l, bool ident)
{
    for (size_t i = 0; i < l; i++) {
        unsigned char c = (unsigned char)v[i];
        bool lead_digit = ident && c >= '0' && c <= '9' && (i == 0 || (i == 1 && v[0] == '-'));
        if (k_name_char[c] && !lead_digit) {
            str_put(o, v + i, 1);
        } else {
            char h[8];
            snprintf(h, sizeof(h), "\\%x ", c);
            str_put(o, h, strlen(h));
        }
    }
}

/* Value tokens [a, b) as text: the source when it is the same thing,
 * else each token rebuilt (white space one space, strings and urls
 * quoted). */
static char *decl_value_text(const CssTz *tz, const CssToken *t, int a, int b)
{
    bool plain = true;
    for (int k = a; k < b && plain; k++) {
        if (t[k].flags & CT_F_ARENA) plain = false;
        /* (a url whose url( was written with escapes: \75 rl() */
        else if (t[k].type == CT_URL && strncasecmp(tz->s + t[k].start, "url(", 4) != 0) plain = false;
        else if (k + 1 < b && t[k].end != t[k + 1].start) plain = false;   /* (a comment between) */
    }
    if (plain) {
        size_t l = t[b - 1].end - t[a].start;
        char *v = (char *)malloc(l + 1);
        if (!v) return NULL;
        memcpy(v, tz->s + t[a].start, l);
        v[l] = 0;
        return v;
    }
    Str o = { NULL, 0, 0 };
    str_put(&o, "", 0);
    for (int k = a; k < b; k++) {
        const CssToken *x = &t[k];
        if (x->type == CT_WHITESPACE) {
            if (o.n && o.p[o.n - 1] != ' ') str_put(&o, " ", 1);
            continue;
        }
        /* (a comment between two tokens: they stay apart) */
        if (k > a && t[k - 1].end != x->start && t[k - 1].type != CT_WHITESPACE && o.n && o.p[o.n - 1] != ' ')
            str_put(&o, " ", 1);
        if (!(x->flags & CT_F_ARENA) && x->type != CT_URL) {
            str_put(&o, tz->s + x->start, x->end - x->start);
            continue;
        }
        size_t vl;
        const char *v = css_tok_val(&tz->arena, x, &vl);
        char num[64];
        switch (x->type) {
        case CT_FUNCTION: val_put_name(&o, v, vl, true); str_put(&o, "(", 1); break;
        case CT_AT_KEYWORD: str_put(&o, "@", 1); val_put_name(&o, v, vl, true); break;
        case CT_HASH: str_put(&o, "#", 1); val_put_name(&o, v, vl, false); break;
        case CT_IDENT: val_put_name(&o, v, vl, true); break;
        case CT_DIMENSION:
            snprintf(num, sizeof(num), "%.10g", x->num);
            str_put(&o, num, strlen(num));
            val_put_name(&o, v, vl, true);
            break;
        case CT_STRING:
        case CT_URL:
            if (x->type == CT_URL) str_put(&o, "url(", 4);
            str_put(&o, "\"", 1);
            for (size_t i = 0; i < vl; i++) {
                if (v[i] == '"' || v[i] == '\\') str_put(&o, "\\", 1);
                if (v[i] == '\n') { str_put(&o, "\\a ", 3); continue; }
                str_put(&o, v + i, 1);
            }
            str_put(&o, "\"", 1);
            if (x->type == CT_URL) str_put(&o, ")", 1);
            break;
        default: str_put(&o, v, vl); break;
        }
    }
    return o.p;
}

/* One declaration's tokens (the first an identifier): to sink when it is
 * one. */
static void decl_finish(const CssTz *tz, const TokBuf *b, DeclSink sink, void *ctx)
{
    const CssToken *t = b->t;
    int n = b->n, k = 1;
    while (k < n && t[k].type == CT_WHITESPACE) k++;
    if (k >= n || t[k].type != CT_COLON) return;
    k++;
    while (k < n && t[k].type == CT_WHITESPACE) k++;
    int e = n;
    while (e > k && t[e - 1].type == CT_WHITESPACE) e--;
    bool important = false;
    if (e > k && t[e - 1].type == CT_IDENT) {
        size_t il;
        const char *iv = css_tok_val(&tz->arena, &t[e - 1], &il);
        int bang = e - 2;
        while (bang >= k && t[bang].type == CT_WHITESPACE) bang--;
        if (il == 9 && strncasecmp(iv, "important", 9) == 0 && bang >= k && t[bang].type == CT_DELIM &&
            t[bang].delim == '!') {
            important = true;
            e = bang;
            while (e > k && t[e - 1].type == CT_WHITESPACE) e--;
        }
    }
    if (e <= k) return;   /* (no value) */
    /* (a bad string or url fits no property's grammar) */
    for (int i = k; i < e; i++)
        if (t[i].type == CT_BAD_STRING || t[i].type == CT_BAD_URL) return;
    size_t pl;
    const char *pv = css_tok_val(&tz->arena, &t[0], &pl);
    char *prop = (char *)malloc(pl + 1);
    char *value = decl_value_text(tz, t, k, e);
    if (!prop || !value || !value[0]) { free(prop); free(value); return; }
    memcpy(prop, pv, pl);
    prop[pl] = 0;
    css_str_tolower(prop);
    sink(ctx, prop, value, important);
}

/* Consume a list of declarations from s at pos: up to the } that ends its
 * block (in_block) or the end.  Returns the offset after it. */
static size_t css_consume_declarations(const char *s, size_t len, size_t pos, bool in_block, DeclSink sink,
                                       void *ctx)
{
    CssTz tz;
    css_tz_init(&tz, s, len, pos);
    TokBuf buf = { NULL, 0, 0 };
    CssToken t;
    size_t end = len;
    /* Closers expected at each nesting level of the item being read. */
    char closers[64];
    int depth = 0;
    enum { NONE, DECL, SKIP, AT } mode = NONE;
    size_t mark = css_tz_arena_mark(&tz);
    while (css_tz_next(&tz, &t)) {
        if (t.type == CT_EOF) {
            if (mode == DECL) decl_finish(&tz, &buf, sink, ctx);
            break;
        }
        if (depth > 0) {
            char c = t.type == CT_RPAREN ? ')' : t.type == CT_RBRACKET ? ']' : t.type == CT_RBRACE ? '}' : 0;
            if (c && c == closers[depth - 1]) depth--;
            else if ((t.type == CT_FUNCTION || t.type == CT_LPAREN) && depth < 64) closers[depth++] = ')';
            else if (t.type == CT_LBRACKET && depth < 64) closers[depth++] = ']';
            else if (t.type == CT_LBRACE && depth < 64) closers[depth++] = '}';
            if (mode == DECL) tokbuf_push(&buf, &t);
            /* (an at-rule ends with its block, and so does anything else
             * that is not a declaration: it is read as a nested rule) */
            if ((mode == AT || mode == SKIP) && depth == 0 && t.type == CT_RBRACE) mode = NONE;
            continue;
        }
        if (in_block && t.type == CT_RBRACE) {
            if (mode == DECL) decl_finish(&tz, &buf, sink, ctx);
            end = t.end;
            break;
        }
        if (t.type == CT_SEMICOLON) {
            if (mode == DECL) decl_finish(&tz, &buf, sink, ctx);
            mode = NONE;
            buf.n = 0;
            css_tz_arena_reset(&tz, mark);
            continue;
        }
        if (mode == NONE) {
            if (t.type == CT_WHITESPACE) continue;
            mode = t.type == CT_IDENT ? DECL : t.type == CT_AT_KEYWORD ? AT : SKIP;
            buf.n = 0;
        }
        if (t.type == CT_FUNCTION || t.type == CT_LPAREN) closers[depth++] = ')';   /* (depth is 0 here) */
        else if (t.type == CT_LBRACKET) closers[depth++] = ']';
        else if (t.type == CT_LBRACE) closers[depth++] = '}';
        if (mode == DECL) tokbuf_push(&buf, &t);
    }
    free(buf.t);
    css_tz_free(&tz);
    return end;
}

static void sink_rule(void *ctx, char *prop, char *value, bool important)
{
    CssRule *rule = (CssRule *)ctx;
    if (rule->declaration_count >= rule->declaration_capacity) {
        int nc = rule->declaration_capacity ? rule->declaration_capacity * 2 : 4;
        CssDeclaration *nd = (CssDeclaration *)realloc(rule->declarations, (size_t)nc * sizeof(CssDeclaration));
        if (!nd) { free(prop); free(value); return; }
        rule->declarations = nd;
        rule->declaration_capacity = nc;
    }
    CssDeclaration *d = &rule->declarations[rule->declaration_count++];
    d->property = prop;
    d->value = value;
    d->important = important;
    d->invalid = 0;
}

/* Parse a declaration block between { and }. pos should point just past '{'. */
static void css_parse_declaration_block(const char *s, size_t len, size_t *pos,
                                        CssRule *rule) {
    /* (the plain blocks of real sheets as text; the rest from tokens) */
    if (css_decl_block_fast(s, len, pos, rule)) return;
    *pos = css_consume_declarations(s, len, *pos, true, sink_rule, rule);
}

CssDeclaration* css_parse_inline_style(const char *style_attr, int *out_count) {
    if (!style_attr || !out_count) return NULL;
    *out_count = 0;
    size_t len = strlen(style_attr);
    if (len == 0) return NULL;
    CssRule r;
    memset(&r, 0, sizeof(r));
    css_consume_declarations(style_attr, len, 0, false, sink_rule, &r);
    *out_count = r.declaration_count;
    return r.declarations;
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

/* Removes comments from a selector in place.  A comment separates tokens, so
 * one between two identifier characters (`bar` and `i` in `[a=bar/ ** /i]`)
 * becomes a space; elsewhere it vanishes, keeping `div` and `.x` one compound. */
static void css_strip_comments(char *s) {
    char *o = s, q = 0;
    for (char *p = s; *p; ) {
        if (q) {
            if (*p == '\\' && p[1]) *o++ = *p++;
            else if (*p == q) q = 0;
            *o++ = *p++;
        } else if (*p == '"' || *p == '\'') {
            q = *p;
            *o++ = *p++;
        } else if (p[0] == '/' && p[1] == '*') {
            char *e = strstr(p + 2, "*/");
            p = e ? e + 2 : p + strlen(p);
            unsigned char a = o > s ? (unsigned char)o[-1] : 0, b = (unsigned char)*p;
            if ((isalnum(a) || a == '-' || a == '_' || a >= 0x80) &&
                (isalnum(b) || b == '-' || b == '_' || b == '\\' || b >= 0x80))
                *o++ = ' ';
        } else {
            *o++ = *p++;
        }
    }
    while (o > s && css_is_space(o[-1])) o--;
    *o = '\0';
}

/* A qualified rule's prelude that cannot be a selector list: a ';' or '@'
 * outside strings and brackets (CSS Syntax: a ';' does not end a prelude,
 * so "junk; .a { }" is one rule with an invalid selector), an empty
 * selector (a comma at either end, or two together), or one starting with
 * what no compound starts with (a digit: "1badselector").  The selector
 * parser takes the rest. */
static bool css_prelude_invalid(const char *sel)
{
    const char *p = sel;
    char q = 0;
    int br = 0;
    bool first = true;   /* at a selector's start */
    for (;; p++) {
        if (first) {
            while (css_is_space(*p)) p++;
            unsigned char c = (unsigned char)*p;
            if (!c || c == ',') return true;
            if (!(isalpha(c) || c >= 0x80 || strchr("_*.#[:|\\&-", c))) return true;
            if (c == '-' && isdigit((unsigned char)p[1])) return true;
            first = false;
        }
        char c = *p;
        if (!c) return false;
        if (q) {
            if (c == '\\' && p[1]) p++;
            else if (c == q) q = 0;
        } else if (c == '\\' && p[1]) {
            p++;
        } else if (c == '"' || c == '\'') {
            q = c;
        } else if (c == '(' || c == '[') {
            br++;
        } else if ((c == ')' || c == ']') && br > 0) {
            br--;
        } else if (br == 0 && (c == ';' || c == '@')) {
            return true;
        } else if (c == ',' && br == 0) {
            first = true;
        } else if (c == ':' && br == 0) {
            /* (a pseudo-element, ::x or the legacy :before, :after,
             * :first-line and :first-letter, ends its selector: only
             * pseudo-classes may follow it; :lang() needs an argument) */
            bool dbl = p[1] == ':';
            const char *n = p + 1 + dbl;
            size_t nl = 0;
            while (isalnum((unsigned char)n[nl]) || n[nl] == '-' || n[nl] == '_') nl++;
            if (nl == 4 && !strncasecmp(n, "lang", 4) && n[4] == '(') {
                const char *a = n + 5;
                while (css_is_space(*a)) a++;
                if (*a == ')') return true;
            }
            bool pe = dbl || (nl == 6 && !strncasecmp(n, "before", 6)) || (nl == 5 && !strncasecmp(n, "after", 5)) ||
                      (nl == 10 && !strncasecmp(n, "first-line", 10)) ||
                      (nl == 12 && !strncasecmp(n, "first-letter", 12));
            if (pe && nl > 0 && n[nl] != '(') {
                const char *a = n + nl;
                while (css_is_space(*a)) a++;
                if (*a && *a != ',' && !(a == n + nl && *a == ':')) return true;
                p = n + nl - 1;
            }
        }
    }
}

/* Consume a rule's declaration block (after its '{') without keeping it. */
static void css_skip_rule_block(const char *s, size_t len, size_t *pos)
{
    CssRule dummy;
    memset(&dummy, 0, sizeof(dummy));
    css_parse_declaration_block(s, len, pos, &dummy);
    css_declarations_free(dummy.declarations, dummy.declaration_count);
}

/* ---- Rule lists (CSS Syntax 3 sections 5.4.1 - 5.4.3) ----
 * A rule list holds qualified rules (a prelude, then a {} block: style
 * rules) and at-rules (@name, a prelude, then ; or a block).  Brackets and
 * strings nest in a prelude: a { or ; inside () or [] does not end it, and
 * at the top level a } is part of one.  At the top level <!-- and --> are
 * ignored, and so are <![CDATA[ and ]]> (which the XML parser takes out of
 * an XHTML document's style; ours is an HTML parser).  In a block the }
 * that closes it ends the list, and an at-rule it interrupts.  A prelude is
 * read as text when it has none of what only the tokens read right
 * (escapes, comments, unbalanced brackets, } ; @ <, --> and a newline in a
 * string); else from tokens. */

/* An identifier starts at s[i] (CSS Syntax 4.3.9). */
static bool css_ident_starts(const char *s, size_t len, size_t i)
{
    if (i >= len) return false;
    unsigned char c = (unsigned char)s[i];
    if (c != '-' && k_name_char[c] == 2) return true;
    if (c == '\\') return i + 1 < len && s[i + 1] != '\n' && s[i + 1] != '\r' && s[i + 1] != '\f';
    if (c != '-' || i + 1 >= len) return false;
    unsigned char d = (unsigned char)s[i + 1];
    if (k_name_char[d] == 2) return true;
    return d == '\\' && i + 2 < len && s[i + 2] != '\n' && s[i + 2] != '\r' && s[i + 2] != '\f';
}

/* The characters the text prelude scan looks at. */
static const unsigned char k_prelude_special[256] = {
    ['{'] = 1, ['}'] = 1, ['('] = 1, [')'] = 1, ['['] = 1, [']'] = 1, ['"'] = 1, ['\''] = 1,
    ['/'] = 1, ['\\'] = 1, [';'] = 1, ['@'] = 1, ['<'] = 1, ['>'] = 1, [','] = 1, ['.'] = 1, ['#'] = 1,
};

/* A selector's start at s[i] (white space skipped) that no selector has:
 * the list's end ('{') or a comma (an empty selector), or a character no
 * compound starts with ("1badselector"). */
static bool css_sel_start_bad(const char *s, size_t len, size_t i)
{
    while (i < len && css_is_space(s[i])) i++;
    if (i >= len) return true;
    unsigned char c = (unsigned char)s[i];
    if (!(isalpha(c) || c >= 0x80 || strchr("_*.#[:|\\&-", c))) return true;
    return c == '-' && i + 1 < len && isdigit((unsigned char)s[i + 1]);
}

/* A qualified rule's prelude from pos as text (at: an at-rule's, which a
 * ';' also ends): true with *brace its '{' (or ';'), or false where the
 * token path has to read it.  For a qualified rule *bad tells whether the
 * selector list has a selector css_prelude_invalid turns down. */
static bool css_prelude_fast(const char *s, size_t len, size_t pos, bool at, size_t *brace, bool *bad)
{
    char st[32];
    int d = 0;
    if (!at && css_sel_start_bad(s, len, pos)) *bad = true;
    for (size_t i = pos; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!k_prelude_special[c]) continue;
        switch (c) {
        case '{':
            if (d) return false;
            *brace = i;
            return true;
        case '[':
            if (!at) {
                /* (an attribute's name, maybe after a namespace) */
                size_t j = i + 1;
                while (j < len && css_is_space(s[j])) j++;
                if (!(j < len && (s[j] == '*' || s[j] == '|')) && !css_ident_starts(s, len, j)) *bad = true;
            }
            /* fall through */
        case '(':
            if (d == (int)sizeof(st)) return false;
            st[d++] = c == '(' ? ')' : ']';
            break;
        case '.':
        case '#':
            /* (a class or an id is an identifier: not .6x, #-1x) */
            if (!at && !css_ident_starts(s, len, i + 1)) *bad = true;
            break;
        case ')':
        case ']':
            if (!d || st[d - 1] != (char)c) return false;
            d--;
            break;
        case '"':
        case '\'':
            for (i++; i < len && s[i] != (char)c; i++)
                if (s[i] == '\\' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f') return false;
            if (i >= len) return false;
            break;
        case '/':
            if (i + 1 < len && s[i + 1] == '*') return false;
            break;
        case '>':
            if (i >= pos + 2 && s[i - 1] == '-' && s[i - 2] == '-') return false;   /* (-->) */
            break;
        case ';':
            if (!at || d) return false;
            *brace = i;
            return true;
        case ',':
            if (!at && !d && css_sel_start_bad(s, len, i + 1)) *bad = true;
            break;
        default:   /* \ } @ < */
            return false;
        }
    }
    return false;
}

/* A block from pos (just after its '{') as text, to the '}' that closes
 * it: true with *end after it, or false where the tokens have to read it
 * (an escape, a comment, a newline in a string, unbalanced brackets). */
static bool css_block_skip_fast(const char *s, size_t len, size_t pos, size_t *end)
{
    char st[32];
    int d = 1;
    st[0] = '}';
    for (size_t i = pos; i < len; i++) {
        char c = s[i];
        switch (c) {
        case '{':
        case '(':
        case '[':
            if (d == (int)sizeof(st)) return false;
            st[d++] = c == '{' ? '}' : c == '(' ? ')' : ']';
            break;
        case '}':
        case ')':
        case ']':
            if (st[d - 1] != c) return false;
            if (--d == 0) {
                *end = i + 1;
                return true;
            }
            break;
        case '"':
        case '\'':
            for (i++; i < len && s[i] != c; i++)
                if (s[i] == '\\' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f') return false;
            if (i >= len) return false;
            break;
        case '\\':
            return false;
        case '/':
            if (i + 1 < len && s[i + 1] == '*') return false;
            break;
        default:
            break;
        }
    }
    return false;
}

/* Closers of the blocks open in a prelude (past 64 deep, any closer). */
typedef struct { char c[64]; int d; } Closers;

static void closers_open(Closers *k, const CssToken *t)
{
    char c = t->type == CT_FUNCTION || t->type == CT_LPAREN ? ')' : t->type == CT_LBRACKET ? ']'
           : t->type == CT_LBRACE ? '}' : 0;
    if (!c) return;
    if (k->d < 64) k->c[k->d] = c;
    k->d++;
}

/* t closes the innermost open block. */
static bool closers_close(Closers *k, const CssToken *t)
{
    char c = t->type == CT_RPAREN ? ')' : t->type == CT_RBRACKET ? ']' : t->type == CT_RBRACE ? '}' : 0;
    if (!c || !k->d || (k->d <= 64 && k->c[k->d - 1] != c)) return false;
    k->d--;
    return true;
}

/* Tokens from tz up to the } that closes the block just opened: the
 * offset after it. */
static size_t css_tokens_skip_block(CssTz *tz)
{
    Closers k = { { '}' }, 1 };
    CssToken t;
    while (css_tz_next(tz, &t) && t.type != CT_EOF) {
        if (!closers_close(&k, &t)) closers_open(&k, &t);
        if (!k.d) return t.end;
    }
    return tz->n;
}

/* An identifier's value in selector text: name characters as they are,
 * any other (and a digit that would start it) as \X, which the selector
 * parser reads as the character X. */
static void sel_put_ident(Str *o, const char *v, size_t l)
{
    for (size_t i = 0; i < l; i++) {
        unsigned char c = (unsigned char)v[i];
        bool lead_digit = c >= '0' && c <= '9' && (i == 0 || (i == 1 && v[0] == '-'));
        if (!k_name_char[c] || lead_digit) str_put(o, "\\", 1);
        str_put(o, v + i, 1);
    }
}

/* Token t of a prelude appended to selector text o. */
static void sel_put_token(Str *o, const CssTz *tz, const CssToken *t)
{
    size_t vl;
    const char *v = css_tok_val(&tz->arena, t, &vl);
    switch (t->type) {
    case CT_WHITESPACE:
        if (o->n && o->p[o->n - 1] != ' ') str_put(o, " ", 1);
        return;
    case CT_IDENT:
        sel_put_ident(o, v, vl);
        return;
    case CT_FUNCTION:
        sel_put_ident(o, v, vl);
        str_put(o, "(", 1);
        return;
    case CT_HASH:
        str_put(o, "#", 1);
        sel_put_ident(o, v, vl);
        return;
    case CT_STRING:
        str_put(o, "\"", 1);
        for (size_t i = 0; i < vl; i++) {
            if (v[i] == '"' || v[i] == '\\') str_put(o, "\\", 1);
            str_put(o, v + i, 1);
        }
        str_put(o, "\"", 1);
        return;
    default:
        break;
    }
    str_put(o, tz->s + t->start, t->end - t->start);
}

/* A qualified rule's prelude from tokens, from pos.  *block: it ended at a
 * '{' (else at the '}' closing the enclosing block - nested - or at the
 * end); *pre_end where the prelude ends; *bad when it has what no
 * selector has; *sel (when not NULL) its text from the tokens: escapes
 * as \X, comments gone, white space one space.  Returns where the rule's
 * block starts (after its '{'), or where reading stopped. */
static size_t css_prelude_tokens(const char *s, size_t len, size_t pos, bool nested, bool *block,
                                 size_t *pre_end, bool *bad, char **sel)
{
    CssTz tz;
    css_tz_init(&tz, s, len, pos);
    Closers k = { { 0 }, 0 };
    CssToken t;
    size_t end = len, prev_end = pos;
    Str o = { NULL, 0, 0 };
    str_put(&o, "", 0);
    /* (after '.': an identifier; after '[': one, '*' or '|', white space
     * first) */
    enum { ANY, IDENT_NEXT, ATTR_NEXT } want = ANY;
    *block = false;
    *pre_end = len;
    while (css_tz_next(&tz, &t) && t.type != CT_EOF) {
        if (t.type == CT_BAD_STRING || t.type == CT_BAD_URL || t.type == CT_CDO || t.type == CT_CDC ||
            t.type == CT_SEMICOLON || t.type == CT_AT_KEYWORD)
            *bad = true;
        if (!(k.d == 0 && (t.type == CT_LBRACE || (t.type == CT_RBRACE && nested)))) {
            if (want == IDENT_NEXT && t.type != CT_IDENT) *bad = true;
            if (want == ATTR_NEXT && t.type != CT_WHITESPACE && t.type != CT_IDENT &&
                !(t.type == CT_DELIM && (t.delim == '*' || t.delim == '|')))
                *bad = true;
            if (want == IDENT_NEXT || t.type != CT_WHITESPACE) want = ANY;
            if (t.type == CT_DELIM && t.delim == '.') want = IDENT_NEXT;
            else if (t.type == CT_LBRACKET) want = ATTR_NEXT;
            else if (t.type == CT_HASH && !(t.flags & CT_F_ID)) *bad = true;
            /* (a number only in a function's argument: :nth-child(2n+1)) */
            else if ((t.type == CT_NUMBER || t.type == CT_PERCENTAGE || t.type == CT_DIMENSION) &&
                     (k.d == 0 || (k.d <= 64 && k.c[k.d - 1] == ']')))
                *bad = true;
            /* (a comment between two names keeps them apart) */
            if (t.start != prev_end && t.type != CT_WHITESPACE && o.n && k_name_char[(unsigned char)o.p[o.n - 1]] &&
                (k_name_char[(unsigned char)s[t.start]] || s[t.start] == '\\'))
                str_put(&o, " ", 1);
            sel_put_token(&o, &tz, &t);
            prev_end = t.end;
        }
        if (k.d) {
            if (t.type == CT_LBRACE || t.type == CT_RBRACE) *bad = true;
            if (!closers_close(&k, &t)) closers_open(&k, &t);
            continue;
        }
        if (t.type == CT_LBRACE) {
            *block = true;
            *pre_end = t.start;
            end = t.end;
            break;
        }
        if (t.type == CT_RBRACE) {
            if (nested) {
                *pre_end = end = t.start;
                break;
            }
            *bad = true;
        } else if (t.type == CT_RPAREN || t.type == CT_RBRACKET) {
            *bad = true;
        }
        closers_open(&k, &t);
    }
    if (want != ANY) *bad = true;
    css_tz_free(&tz);
    while (o.n && o.p[o.n - 1] == ' ') o.p[--o.n] = 0;
    if (sel && o.p) {
        size_t a = o.p[0] == ' ';
        *sel = strdup(o.p + a);
    }
    free(o.p);
    return end;
}

typedef enum { AT_SKIP, AT_MEDIA, AT_NEST } AtKind;

/* What an at-rule's block holds: style rules under a media condition
 * (@media), style rules (@supports, @layer, @container, ...: entered
 * unconditionally), or else nothing kept. */
static AtKind css_at_kind(const char *n, size_t l)
{
    static const char *const nest[] = { "supports", "layer", "container", "scope", "starting-style", "document",
                                        "-moz-document" };
    if (l == 5 && !strncasecmp(n, "media", 5)) return AT_MEDIA;
    for (size_t i = 0; i < sizeof(nest) / sizeof(nest[0]); i++)
        if (strlen(nest[i]) == l && !strncasecmp(n, nest[i], l)) return AT_NEST;
    return AT_SKIP;
}

/* An at-rule some browser knows (an @import after one is ignored; after an
 * unknown one, not). */
static bool css_at_known(const char *n, size_t l)
{
    static const char *const known[] = { "media", "supports", "layer", "container", "scope", "starting-style",
                                         "document", "-moz-document", "font-face", "keyframes",
                                         "-webkit-keyframes", "page", "namespace", "counter-style", "property",
                                         "font-feature-values", "font-palette-values", "view-transition" };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (strlen(known[i]) == l && !strncasecmp(n, known[i], l)) return true;
    return false;
}

static size_t css_rule_list(const char *s, size_t len, size_t pos, size_t stop, bool nested,
                            CssStylesheet *sheet, const char *cond);

/* The at-rule at pos (its '@').  Its style rules go to sheet (when not
 * NULL) under cond.  *counts: it is one an @import cannot follow.  Returns
 * where it ends. */
static size_t css_at_rule(const char *s, size_t len, size_t pos, bool nested, CssStylesheet *sheet,
                          const char *cond, bool *counts)
{
    /* (a plain name and prelude as text; else from tokens) */
    char name[32];
    size_t nl = 0, q = pos + 1;
    while (q < len && (isalnum((unsigned char)s[q]) || s[q] == '-' || s[q] == '_')) {
        if (nl < sizeof(name)) name[nl] = s[q];
        nl++;
        q++;
    }
    size_t pre = q, pre_end = len, end = len;
    bool block = false;
    bool unused = false;
    bool fast = nl <= sizeof(name) && !(q < len && (s[q] == '\\' || (unsigned char)s[q] >= 0x80)) &&
                css_prelude_fast(s, len, q, true, &pre_end, &unused);
    if (fast) {
        block = s[pre_end] == '{';
        end = pre_end + 1;
    } else {
        CssTz tz;
        css_tz_init(&tz, s, len, pos);
        CssToken t;
        if (!css_tz_next(&tz, &t) || t.type != CT_AT_KEYWORD) {
            css_tz_free(&tz);
            return pos + 1;
        }
        size_t vl;
        const char *nv = css_tok_val(&tz.arena, &t, &vl);
        nl = vl < sizeof(name) ? vl : sizeof(name);
        memcpy(name, nv, nl);
        if (vl > sizeof(name)) nl = 0;   /* (no at-rule known has so long a name) */
        pre = t.end;
        Closers k = { { 0 }, 0 };
        while (css_tz_next(&tz, &t) && t.type != CT_EOF) {
            if (k.d) {
                if (!closers_close(&k, &t)) closers_open(&k, &t);
                continue;
            }
            if (t.type == CT_SEMICOLON) {
                pre_end = t.start;
                end = t.end;
                break;
            }
            if (t.type == CT_LBRACE) {
                pre_end = t.start;
                end = t.end;
                block = true;
                break;
            }
            if (t.type == CT_RBRACE && nested) {
                pre_end = end = t.start;
                break;
            }
            closers_open(&k, &t);
        }
        css_tz_free(&tz);
    }
    AtKind kind = css_at_kind(name, nl);
    bool is_charset = nl == 7 && !strncasecmp(name, "charset", 7), is_import = nl == 6 && !strncasecmp(name, "import", 6),
         is_layer = nl == 5 && !strncasecmp(name, "layer", 5);
    /* (one that needs a block, @media; or @page;, is invalid: dropped, it
     * does not count; of the statements only @namespace does) */
    bool is_namespace = nl == 9 && !strncasecmp(name, "namespace", 9);
    if (counts) *counts = css_at_known(name, nl) && !is_charset && !is_import && (block ? true : is_namespace);
    if (!block) return end;
    if (kind == AT_SKIP || !sheet) {
        size_t e;
        if (css_block_skip_fast(s, len, end, &e)) return e;
        CssTz tz;
        css_tz_init(&tz, s, len, end);
        end = css_tokens_skip_block(&tz);
        css_tz_free(&tz);
        return end;
    }
    char *eff = NULL;
    if (kind == AT_MEDIA) {
        char *c = css_strndup_trim(s + pre, pre_end - pre);
        if (c && strstr(c, "/*")) css_strip_comments(c);
        if (cond && cond[0] && c && c[0]) {
            /* Each query list must match: joined by CSS_MEDIA_AND. */
            size_t n = strlen(cond) + strlen(c) + 2;
            eff = (char *)malloc(n);
            if (eff) snprintf(eff, n, "%s%c%s", cond, CSS_MEDIA_AND, c);
        } else if (cond && cond[0]) {
            eff = strdup(cond);
        } else if (c && c[0]) {
            eff = strdup(c);
        }
        free(c);
    } else if (cond && cond[0]) {
        eff = strdup(cond);
    }
    end = css_rule_list(s, len, end, len, true, sheet, eff ? eff : "");
    free(eff);
    return end;
}

/* The qualified rule or at-rule at pos (white space and comments skipped):
 * added to sheet unless NULL.  *counts: a valid style rule or a known
 * at-rule.  Returns where it ends. */
static size_t css_rule_item(const char *s, size_t len, size_t pos, bool nested, CssStylesheet *sheet,
                            const char *cond, bool *counts)
{
    if (counts) *counts = false;
    if (s[pos] == '@' && css_ident_starts(s, len, pos + 1)) return css_at_rule(s, len, pos, nested, sheet, cond, counts);
    size_t start = pos, pre_end, brace;
    bool block = true, bad = false, fast = css_prelude_fast(s, len, pos, false, &brace, &bad);
    char *tsel = NULL;
    if (fast) {
        pre_end = brace;
        pos = brace + 1;
    } else {
        bad = false;
        pos = css_prelude_tokens(s, len, pos, nested, &block, &pre_end, &bad, &tsel);
        if (!block) {
            free(tsel);
            return pos;
        }
    }
    char *sel = bad ? NULL : fast ? css_strndup_trim(s + start, pre_end - start) : tsel;
    if (sel != tsel) free(tsel);
    /* (the text scan has checked what css_prelude_invalid looks for, but
     * for pseudo-elements and :lang()) */
    if (!sel || ((!fast || strchr(sel, ':')) && css_prelude_invalid(sel))) {
        free(sel);
        css_skip_rule_block(s, len, &pos);
        return pos;
    }
    if (counts) *counts = true;
    CssRule *rule = sheet ? css_stylesheet_add_rule(sheet) : NULL;
    if (!rule) {
        free(sel);
        css_skip_rule_block(s, len, &pos);
        return pos;
    }
    rule->selector_text = sel;
    rule->media_query = cond && cond[0] ? strdup(cond) : NULL;
    css_parse_declaration_block(s, len, &pos, rule);
    return pos;
}

/* At the top level: <!--, -->, <![CDATA[ or ]]> at pos, its length. */
static size_t css_top_ignored(const char *s, size_t len, size_t pos)
{
    if (s[pos] == '<') {
        if (len - pos >= 4 && !memcmp(s + pos, "<!--", 4)) return 4;
        if (len - pos >= 9 && !memcmp(s + pos, "<![CDATA[", 9)) return 9;
    } else if (s[pos] == '-') {
        if (len - pos >= 3 && !memcmp(s + pos, "-->", 3)) return 3;
    } else if (s[pos] == ']') {
        if (len - pos >= 3 && !memcmp(s + pos, "]]>", 3)) return 3;
    }
    return 0;
}

/* Consume a list of rules from pos: nested, up to the } that closes its
 * block (returning the offset after it); else the top level, items that
 * start before stop.  Style rules go to sheet under the media condition
 * cond. */
static size_t css_rule_list(const char *s, size_t len, size_t pos, size_t stop, bool nested,
                            CssStylesheet *sheet, const char *cond)
{
    while (pos < len && (nested || pos < stop)) {
        css_skip_space_and_comments(s, len, &pos);
        if (pos >= len) break;
        if (nested) {
            if (s[pos] == '}') return pos + 1;
        } else {
            size_t ig = css_top_ignored(s, len, pos);
            if (ig) {
                pos += ig;
                continue;
            }
        }
        pos = css_rule_item(s, len, pos, nested, sheet, cond, NULL);
    }
    return pos;
}

size_t css_sheet_item_skip(const char *css, size_t len, size_t pos, bool *counts)
{
    *counts = false;
    css_skip_space_and_comments(css, len, &pos);
    if (pos >= len) return len;
    size_t ig = css_top_ignored(css, len, pos);
    if (ig) return pos + ig;
    return css_rule_item(css, len, pos, false, NULL, "", counts);
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
    size_t pos = css_rule_list(css, len, start, stop, false, sheet, "");
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
        i = css_sel_name_end(s, n, i, ".#[:");
        css_sel_name_copy(out->tag, s + start, i - start, sizeof(out->tag));
        out->has_tag = out->tag[0] != '\0';
        out->has_substantive = out->has_substantive || out->has_tag;
    }

    while (i < n) {
        css_skip_space(s, n, &i);
        if (i >= n) break;
        if (s[i] == '.') {
            i++;
            size_t start = i;
            i = css_sel_name_end(s, n, i, ".#[:");
            if (out->class_count < CSS_MAX_CLASSES) {
                css_sel_name_copy(out->classes[out->class_count], s + start, i - start,
                                  sizeof(out->classes[0]));
                out->class_count++;
                out->has_substantive = true;
            }
        } else if (s[i] == '#') {
            i++;
            size_t start = i;
            i = css_sel_name_end(s, n, i, ".#[:");
            css_sel_name_copy(out->id, s + start, i - start, sizeof(out->id));
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
            /* `*|name` / `|name`: attributes carry no namespace here. */
            if (i + 1 < n && s[i] == '*' && s[i + 1] == '|') i += 2;
            else if (i + 1 < n && s[i] == '|' && s[i + 1] != '=') i++;
            size_t attr_start = i;
            i = css_sel_name_end(s, n, i, "=~|^$*] \t\n\r\f");
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
                /* The flag is the identifier i or s (escapes allowed);
                 * without one, HTML's legacy attributes ignore case. */
                bool flag = false;
                if (!bad && i < n && s[i] != ']') {
                    char f[8];
                    size_t fl = 0;
                    while (i < n && s[i] != ']' && !css_is_space(s[i]) && fl < sizeof f - 1) {
                        if (s[i] == '\\' && i + 1 < n && isxdigit((unsigned char)s[i + 1])) {
                            unsigned c = 0;
                            for (int k = 0; k < 6 && i + 1 < n && isxdigit((unsigned char)s[i + 1]); k++, i++)
                                c = c * 16 + (unsigned)(isdigit((unsigned char)s[i + 1]) ? s[i + 1] - '0' : tolower((unsigned char)s[i + 1]) - 'a' + 10);
                            f[fl++] = c < 0x80 ? (char)c : '?';
                            i++;
                            if (i < n && css_is_space(s[i])) i++;
                            continue;
                        }
                        if (s[i] == '\\' && i + 1 < n) i++;
                        f[fl++] = s[i++];
                    }
                    while (i < n && css_is_space(s[i])) i++;
                    if (fl == 1 && (f[0] == 'i' || f[0] == 'I')) ci = true;
                    else if (!(fl == 1 && (f[0] == 's' || f[0] == 'S'))) bad = true;
                    if (i < n && s[i] != ']') bad = true;
                    flag = true;
                }
                if (!flag && css_attr_legacy_ci(s + attr_start, attr_end - attr_start)) ci = true;
            }
            if (attr_end > attr_start && out->attr_count < CSS_MAX_ATTRS) {
                int k = out->attr_count;
                css_sel_name_copy(out->attrs[k], s + attr_start,
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
            if (c == '\\' && i + 1 < len) { i += 2; continue; }   /* (\X: one character) */
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
            if (c == '\\' && i + 1 < len) { i += 2; continue; }   /* (\X: one character) */
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
            if (c == '\\' && i + 1 < len) { i += 2; continue; }   /* (\X: one character) */
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




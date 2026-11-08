/*
 * CSS Parser - Implementation
 *
 * A small, self-contained CSS parser for applying styles to the DOM tree.
 */

#include <stdbool.h>
#include <strings.h>
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

/* A transition has been declared somewhere: see lq_who_affected. */
bool g_css_saw_transition;
bool g_css_saw_composition;     /* (animation-composition: what the engine's tracks don't do) */
int g_css_attr_any_ns;
int g_css_type_ns;

/* @namespace prefixes seen (all sheets alike; a later one of a prefix wins), and the default */
static struct { char p[32]; char uri[200]; } g_css_ns[16];
static int g_css_nns;
static char g_css_ns_default[200];
static bool g_css_ns_has_default;
static const char *css_ns_lookup(const char *p, size_t n)
{
    for (int i = g_css_nns - 1; i >= 0; i--) if (strlen(g_css_ns[i].p) == n && !strncmp(g_css_ns[i].p, p, n)) return g_css_ns[i].uri;
    return NULL;
}

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
    /* (a custom property can be nothing but space; "--" alone isn't one) */
    if (!strcmp(p, "--")) { free(p); free(v); return false; }
    if (p[0] == '-' && p[1] == '-' && v[0] == '\0') { free(v); v = strdup(" "); }
    if (p[0] == '\0' || v[0] == '\0') {
        free(p); free(v);
        return false;
    }
    if (!(p[0] == '-' && p[1] == '-')) css_str_tolower(p);

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
    if (!strncasecmp(p, "transition", 10)) g_css_saw_transition = true;
    if (!strncasecmp(p, "animation-composition", 21)) g_css_saw_composition = true;
    rule->declarations[rule->declaration_count].property = p;
    rule->declarations[rule->declaration_count].value = v;
    rule->declarations[rule->declaration_count].important = is_important;
    rule->declarations[rule->declaration_count].feature = css_note_property(p);
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
    /* (a custom property's name is as written, and it can be nothing but space) */
    bool custom = pl > 2 && pv[0] == '-' && pv[1] == '-';
    if (pl == 2 && pv[0] == '-' && pv[1] == '-') { free(prop); free(value); return; }
    if (custom && value && !value[0]) { free(value); value = strdup(" "); }
    if (!prop || !value || !value[0]) { free(prop); free(value); return; }
    memcpy(prop, pv, pl);
    prop[pl] = 0;
    if (!custom) css_str_tolower(prop);
    sink(ctx, prop, value, important);
}

/* A style rule's block (CSS Nesting): the rules and at-rules in it, as the
 * declaration list goes by them, for css_parse_rule_block. */
typedef struct { size_t start, brace; int decls; } CssNestItem;
typedef struct { CssNestItem *v; int n, cap; bool on; } CssNestList;
static __thread CssNestList t_nest;
/* (inside a nested at-rule: the selector its declarations and rules are of) */
static __thread const char *t_nest_parent;
static char *scope_apply(const char *sel, const char *a, const char *b, bool prefix);

static void css_nest_push(size_t start, size_t brace, int decls)
{
    if (t_nest.n == t_nest.cap) {
        int nc = t_nest.cap ? t_nest.cap * 2 : 8;
        CssNestItem *nv = (CssNestItem *)realloc(t_nest.v, (size_t)nc * sizeof(CssNestItem));
        if (!nv) return;
        t_nest.v = nv;
        t_nest.cap = nc;
    }
    t_nest.v[t_nest.n++] = (CssNestItem){ start, brace, decls };
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
    size_t item_start = 0, item_brace = 0;
    bool nesting = t_nest.on && in_block;
    t_nest.on = false;   /* (for this block only) */
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
            if (t.type == CT_LBRACE && !item_brace) item_brace = t.start;
            /* (an at-rule ends with its block, and so does anything else
             * that is not a declaration: it is read as a nested rule) */
            if ((mode == AT || mode == SKIP) && depth == 0 && t.type == CT_RBRACE) {
                if (nesting && item_brace) css_nest_push(item_start, item_brace, ((CssRule *)ctx)->declaration_count);
                mode = NONE;
            }
            /* ("a:hover { }" starts as a declaration would; "--x: { }" is one) */
            if (nesting && mode == DECL && depth == 0 && t.type == CT_RBRACE && item_brace && buf.n) {
                size_t pl;
                const char *pv = css_tok_val(&tz.arena, &buf.t[0], &pl);
                if (!(pl >= 2 && pv[0] == '-' && pv[1] == '-')) {
                    css_nest_push(item_start, item_brace, ((CssRule *)ctx)->declaration_count);
                    mode = NONE;
                    buf.n = 0;
                }
            }
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
            item_start = t.start;
            item_brace = 0;
        }
        if (t.type == CT_LBRACE) item_brace = t.start;
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
    if (prop && !strncasecmp(prop, "transition", 10)) g_css_saw_transition = true;
    if (prop && !strncasecmp(prop, "animation-composition", 21)) g_css_saw_composition = true;
    d->property = prop;
    d->value = value;
    d->important = important;
    d->feature = css_note_property(prop);
    d->invalid = 0;
}

volatile int g_css_render_features;

static const char *flow_side(bool block, bool end, int wm, bool rtl)
{
    if (block) {
        switch (wm) {
        case 1: case 3: return end ? "left" : "right";
        case 2: case 4: return end ? "right" : "left";
        default: return end ? "bottom" : "top";
        }
    }
    bool flip = rtl;
    if (wm == 4) flip = !flip;      /* (sideways-lr: inline runs bottom to top) */
    if (wm == 0) return end != flip ? "right" : "left";
    return end != flip ? "bottom" : "top";
}

bool css_logical_physical(const char *prop, int wm, bool rtl, char *out, size_t cap)
{
    /* (a corner: its block side, then its inline side) */
    if (!strncmp(prop, "border-", 7) && (!strncmp(prop + 7, "start-", 6) || !strncmp(prop + 7, "end-", 4))) {
        const char *a = prop + 7;
        bool bend = !strncmp(a, "end-", 4);
        a += bend ? 4 : 6;
        bool iend = !strncmp(a, "end-", 4);
        if (!iend && strncmp(a, "start-", 6)) return false;
        a += iend ? 4 : 6;
        if (strcmp(a, "radius")) return false;
        const char *bs = flow_side(true, bend, wm, rtl), *is = flow_side(false, iend, wm, rtl);
        bool b_vert = !strcmp(bs, "top") || !strcmp(bs, "bottom");
        snprintf(out, cap, "border-%s-%s-radius", b_vert ? bs : is, b_vert ? is : bs);
        return true;
    }
    if (!strstr(prop, "block") && !strstr(prop, "inline")) return false;
    bool vertical = wm != 0;
    static const struct { const char *name, *h, *v; } sizes[] = {
        { "block-size", "height", "width" }, { "inline-size", "width", "height" },
        { "min-block-size", "min-height", "min-width" }, { "min-inline-size", "min-width", "min-height" },
        { "max-block-size", "max-height", "max-width" }, { "max-inline-size", "max-width", "max-height" },
        { NULL, NULL, NULL } };
    for (int i = 0; sizes[i].name; i++)
        if (!strcmp(prop, sizes[i].name)) { snprintf(out, cap, "%s", vertical ? sizes[i].v : sizes[i].h); return true; }
    static const char *const pre[] = { "margin", "padding", "inset", "border", "scroll-margin", "scroll-padding", NULL };
    for (int i = 0; pre[i]; i++) {
        size_t n = strlen(pre[i]);
        if (strncmp(prop, pre[i], n) || prop[n] != '-') continue;
        const char *a = prop + n + 1;
        bool block = !strncmp(a, "block-", 6);
        if (!block && strncmp(a, "inline-", 7)) return false;
        a += block ? 6 : 7;
        bool end = !strncmp(a, "end", 3);
        if (!end && strncmp(a, "start", 5)) return false;
        const char *suffix = a + (end ? 3 : 5);
        if (*suffix && *suffix != '-') return false;
        const char *side = flow_side(block, end, wm, rtl);
        if (i == 2) snprintf(out, cap, "%s%s", side, suffix);
        else snprintf(out, cap, "%s-%s%s", pre[i], side, suffix);
        return true;
    }
    return false;
}

bool css_logical_shorthand(const char *prop, char start[64], char end[64])
{
    static const char *const names[] = { "margin-block", "margin-inline", "padding-block", "padding-inline", "inset-block", "inset-inline",
        "border-block-width", "border-inline-width", "border-block-style", "border-inline-style", "border-block-color", "border-inline-color", "border-block", "border-inline", NULL };
    for (int i = 0; names[i]; i++) {
        if (strcmp(prop, names[i])) continue;
        /* (the name, with start or end before any -width) */
        const char *axis = strstr(prop, "-block") ? "-block" : "-inline";
        const char *at = strstr(prop, axis) + strlen(axis);
        size_t head = (size_t)(at - prop);
        snprintf(start, 64, "%.*s-start%s", (int)head, prop, at);
        snprintf(end, 64, "%.*s-end%s", (int)head, prop, at);
        return true;
    }
    return false;
}

unsigned char css_note_property(const char *p) {
    unsigned char bit = 0;
    if (p[0] == 'c') { if (!strcmp(p, "content-visibility")) bit = CSS_FEAT_CONTENT_VIS; }
    else if (p[0] == 's' && p[1] == 'c') {
        if (!strcmp(p, "scroll-snap-type")) bit = CSS_FEAT_SNAP;
        else if (!strcmp(p, "scroll-snap-align")) bit = CSS_FEAT_SNAP_ALIGN;
        else if (!strcmp(p, "scroll-initial-target")) bit = CSS_FEAT_INITIAL_TARGET;
        else if (!strcmp(p, "scroll-marker-group")) bit = CSS_FEAT_MARKER_GROUP;
        else if (!strcmp(p, "scroll-target-group")) bit = CSS_FEAT_TARGET_GROUP;
    }
    if (bit) g_css_render_features |= bit;
    return bit;
}

/* Parse a declaration block between { and }. pos should point just past '{'. */
static void css_parse_declaration_block(const char *s, size_t len, size_t *pos,
                                        CssRule *rule) {
    /* (the plain blocks of real sheets as text; the rest from tokens) */
    if (!css_decl_block_fast(s, len, pos, rule))
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

/* (inside @layer: where the rules go) */
static __thread char *t_layer_path;
static __thread char *t_scope_a, *t_scope_b;
/* (the block being parsed is an @scope's own: its rules are relative to the root) */
static __thread bool t_scope_body;

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
    if (t_layer_path) rule->layer_name = strdup(t_layer_path);
    if (t_scope_a) rule->scope_root = strdup(t_scope_a);
    return rule;
}

/* The next of @layer's names at *p: "a.b" (false: not one). */
static bool layer_name_next(const char **p, char *out, size_t cap)
{
    const char *q = *p;
    size_t n = 0;
    for (;;) {
        const char *st = q;
        if (*q == '-' && q[1] == '-') q += 2;
        else if (*q == '-') q++;
        if (!(isalpha((unsigned char)*q) || *q == '_' || (unsigned char)*q >= 0x80 || (*q == '-' && q > st))) return false;
        while (isalnum((unsigned char)*q) || *q == '_' || *q == '-' || (unsigned char)*q >= 0x80) q++;
        if ((size_t)(q - st) + n + 2 > cap) return false;
        memcpy(out + n, st, (size_t)(q - st));
        n += (size_t)(q - st);
        if (*q != '.') break;
        out[n++] = '.';
        q++;
    }
    out[n] = 0;
    *p = q;
    return true;
}

/* The path of a name under the layer it's in (malloc'd). */
static char *layer_join(const char *name)
{
    size_t n = (t_layer_path ? strlen(t_layer_path) + 1 : 0) + strlen(name) + 1;
    char *o = (char *)malloc(n);
    if (o) snprintf(o, n, "%s%s%s", t_layer_path ? t_layer_path : "", t_layer_path ? "." : "", name);
    return o;
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

/* Whether name [n, n + nl) is a pseudo-class (or with elem, a
 * pseudo-element) that browsers know: a selector with any other makes its
 * whole rule invalid (Selectors 4 section 3.2).  Names with a -webkit-
 * prefix are taken as known (there are too many to list); other vendors'
 * are not, as in Chromium. */
static bool css_pseudo_known(const char *n, size_t nl, bool elem)
{
    static const char *const classes[] = {
        "active", "active-view-transition", "active-view-transition-type", "any-link", "autofill", "blank", "buffering", "checked", "closed", "current", "default",
        "defined", "dir", "disabled", "empty", "enabled", "first", "first-child", "first-of-type", "focus",
        "focus-visible", "focus-within", "fullscreen", "future", "has", "has-slotted", "heading", "host", "host-context", "hover",
        "in-range", "indeterminate", "invalid", "is", "lang", "last-child", "last-of-type", "left", "link",
        "local-link", "matches", "modal", "muted", "not", "nth-child", "nth-last-child", "nth-last-of-type",
        "nth-of-type", "only-child", "only-of-type", "open", "optional", "out-of-range", "past", "paused",
        "picture-in-picture", "placeholder-shown", "playing", "popover-open", "read-only", "read-write",
        "required", "right", "root", "scope", "seeking", "stalled", "state", "target", "target-after", "target-before",
        "target-current", "target-within",
        "user-invalid", "user-valid", "valid", "visited", "volume-locked", "where", "xr-overlay", NULL };
    static const char *const elements[] = {
        "after", "backdrop", "before", "checkmark", "column", "cue", "cue-region", "details-content",
        "file-selector-button", "first-letter", "first-line", "grammar-error", "highlight", "marker", "part",
        "picker", "picker-icon", "placeholder", "scroll-button", "scroll-marker", "scroll-marker-group",
        "search-text", "selection", "slotted", "spelling-error", "target-text", "view-transition",
        "view-transition-group", "view-transition-group-children", "view-transition-image-pair", "view-transition-new", "view-transition-old",
        NULL };
    if (nl > 8 && !strncasecmp(n, "-webkit-", 8)) return true;
    for (const char *const *k = elem ? elements : classes; *k; k++)
        if (strlen(*k) == nl && !strncasecmp(n, *k, nl)) return true;
    return false;
}

/* A qualified rule's prelude that cannot be a selector list: a ';' or '@'
 * outside strings and brackets (CSS Syntax: a ';' does not end a prelude,
 * so "junk; .a { }" is one rule with an invalid selector), an empty
 * selector (a comma at either end, or two together), or one starting with
 * what no compound starts with (a digit: "1badselector").  The selector
 * parser takes the rest. */
static bool css_prelude_invalid(const char *sel);
/* (for @supports selector(): the grammar style rules are held to) */
bool css_selector_text_invalid(const char *sel) { return css_prelude_invalid(sel); }

static bool css_prelude_invalid(const char *sel)
{
    /* (view transition pseudo-elements: the whole selector's grammar) */
    extern bool dom_selector_valid(const char *s);
    if (strstr(sel, "::view-transition") && !dom_selector_valid(sel)) return true;
    /* (case-insensitive substring search) */
    #define css_ci_find(h_, n_) ({ const char *r_ = NULL; size_t nl_ = strlen(n_); for (const char *q_ = (h_); *q_ && !r_; q_++) if (!strncasecmp(q_, (n_), nl_)) r_ = q_; r_; })
    /* (:not() empty, or with a pseudo-element in it: invalid) */
    for (const char *nt = css_ci_find(sel, ":not("); nt; nt = css_ci_find(nt + 5, ":not(")) {
        const char *a = nt + 5, *e = a;
        int d = 1;
        while (*e && d) { if (*e == '(') d++; else if (*e == ')') d--; if (d) e++; }
        const char *t = a;
        while (t < e && css_is_space(*t)) t++;
        if (t == e) return true;
        for (const char *c = a; c < e; c++) {
            if (*c != ':') continue;
            if (c[1] == ':') return true;
            if (!strncasecmp(c + 1, "first-line", 10) || !strncasecmp(c + 1, "first-letter", 12) ||
                !strncasecmp(c + 1, "before", 6) || !strncasecmp(c + 1, "after", 5)) return true;
        }
    }
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
        } else if (br == 0 && strchr("%!$?=^`/{}<", c)) {
            return true;   /* (no selector has these outside brackets) */
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
            if (nl > 0 && !css_pseudo_known(n, nl, pe)) return true;
            if (pe && nl > 0 && n[nl] != '(') {
                const char *a = n + nl;
                while (css_is_space(*a)) a++;
                if (*a && *a != ',' && !(a == n + nl && *a == ':')) return true;
                p = n + nl - 1;
            } else if (pe && nl > 0) {
                p = n + nl - 1;   /* (::name(: on to its argument) */
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

/* The @container preludes around what's being parsed. */
static __thread char *t_cq_cond;
bool g_css_saw_container;

static void css_parse_rule_block(const char *s, size_t len, size_t *pos, CssRule *rule, CssStylesheet *sheet, const char *cond);
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

/* ---- @keyframes (CSS Animations 1 section 3) ---- */

/* The offsets of a keyframe selector list ("from", "to", "N%", comma
 * separated): how many (at most cap), 0 when one is invalid. */
static int css_keyframe_offsets(const char *sel, float *out, int cap)
{
    int n = 0;
    for (const char *p = sel; *p;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f') p++;
        const char *q = p;
        while (*q && *q != ',') q++;
        const char *e = q;
        while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r' || e[-1] == '\f')) e--;
        float v;
        if (e - p == 4 && !strncasecmp(p, "from", 4)) v = 0;
        else if (e - p == 2 && !strncasecmp(p, "to", 2)) v = 1;
        else {
            char *end = NULL;
            double d = strtod(p, &end);
            if (end == p || end + 1 != e || *end != '%' || d < 0 || d > 100) return 0;
            v = (float)(d / 100.0);
        }
        if (n == cap) return 0;
        out[n++] = v;
        p = *q ? q + 1 : q;
    }
    return n;
}

static int css_frame_cmp(const void *a, const void *b)
{
    float x = ((const CssKeyframe *)a)->offset, y = ((const CssKeyframe *)b)->offset;
    return x < y ? -1 : x > y;
}

/* An @keyframes rule: prelude s[pre .. pre_end), its block from body
 * (after the '{').  Returns where it ends. */
static size_t css_keyframes_rule(const char *s, size_t len, size_t pre, size_t pre_end, size_t body,
                                 CssStylesheet *sheet)
{
    char *name = css_strndup_trim(s + pre, pre_end - pre);
    if (name && strstr(name, "/*")) css_strip_comments(name);
    if (name && (name[0] == '"' || name[0] == '\'')) {
        size_t l = strlen(name);
        if (l >= 2 && name[l - 1] == name[0]) { memmove(name, name + 1, l - 2); name[l - 2] = 0; }
    }
    size_t pos = body;
    int first = sheet->frame_n;
    while (pos < len) {
        css_skip_space_and_comments(s, len, &pos);
        if (pos >= len) break;
        if (s[pos] == '}') { pos++; break; }
        size_t st = pos;
        while (pos < len && s[pos] != '{' && s[pos] != '}') pos++;
        if (pos >= len || s[pos] == '}') continue;
        char *sel = css_strndup_trim(s + st, pos - st);
        if (sel && strstr(sel, "/*")) css_strip_comments(sel);
        pos++;
        float off[32];
        int k = sel ? css_keyframe_offsets(sel, off, 32) : 0;
        free(sel);
        CssRule r;
        memset(&r, 0, sizeof r);
        css_parse_declaration_block(s, len, &pos, &r);
        if (!k || !name || !name[0]) {
            css_declarations_free(r.declarations, r.declaration_count);
            continue;
        }
        if (sheet->frame_n + k > sheet->frame_cap) {
            int cap = sheet->frame_cap ? sheet->frame_cap * 2 : 8;
            while (cap < sheet->frame_n + k) cap *= 2;
            CssKeyframe *f = (CssKeyframe *)realloc(sheet->frames, (size_t)cap * sizeof *f);
            if (!f) { css_declarations_free(r.declarations, r.declaration_count); continue; }
            sheet->frames = f;
            sheet->frame_cap = cap;
        }
        for (int i = 0; i < k; i++)
            sheet->frames[sheet->frame_n++] = (CssKeyframe){ off[i], i == 0, r.declarations, r.declaration_count };
    }
    if (!name || !name[0] || !strcasecmp(name, "none")) {
        for (int i = first; i < sheet->frame_n; i++)
            if (sheet->frames[i].owns) css_declarations_free(sheet->frames[i].decls, sheet->frames[i].n);
        sheet->frame_n = first;
        free(name);
        return pos;
    }
    /* (stable: frames at one offset keep their order, the last wins) */
    for (int i = first + 1; i < sheet->frame_n; i++)
        for (int j = i; j > first && css_frame_cmp(&sheet->frames[j - 1], &sheet->frames[j]) > 0; j--) {
            CssKeyframe t = sheet->frames[j];
            sheet->frames[j] = sheet->frames[j - 1];
            sheet->frames[j - 1] = t;
        }
    if (sheet->kf_n == sheet->kf_cap) {
        int cap = sheet->kf_cap ? sheet->kf_cap * 2 : 4;
        CssKeyframes *a = (CssKeyframes *)realloc(sheet->kf, (size_t)cap * sizeof *a);
        if (!a) { free(name); return pos; }
        sheet->kf = a;
        sheet->kf_cap = cap;
    }
    sheet->kf[sheet->kf_n++] = (CssKeyframes){ name, first, sheet->frame_n - first };
    return pos;
}

const CssKeyframes *css_keyframes_find(const CssStylesheet *sheet, const char *name)
{
    if (!sheet || !name) return NULL;
    for (int i = sheet->kf_n - 1; i >= 0; i--)
        if (!strcmp(sheet->kf[i].name, name)) return &sheet->kf[i];
    return NULL;
}

void css_keyframes_take(CssStylesheet *dst, CssStylesheet *src)
{
    if (!src->kf_n && !src->frame_n) return;
    int fn = dst->frame_n + src->frame_n, kn = dst->kf_n + src->kf_n;
    CssKeyframe *f = (CssKeyframe *)realloc(dst->frames, (size_t)(fn ? fn : 1) * sizeof *f);
    if (f) dst->frames = f;
    CssKeyframes *k = (CssKeyframes *)realloc(dst->kf, (size_t)(kn ? kn : 1) * sizeof *k);
    if (k) dst->kf = k;
    if (!f || !k) return;
    if (src->frame_n) memcpy(dst->frames + dst->frame_n, src->frames, (size_t)src->frame_n * sizeof *f);
    for (int i = 0; i < src->kf_n; i++) {
        dst->kf[dst->kf_n + i] = src->kf[i];
        dst->kf[dst->kf_n + i].first += dst->frame_n;
    }
    dst->frame_n = dst->frame_cap = fn;
    dst->kf_n = dst->kf_cap = kn;
    free(src->frames);
    free(src->kf);
    src->frames = NULL;
    src->kf = NULL;
    src->frame_n = src->frame_cap = src->kf_n = src->kf_cap = 0;
}

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
    bool is_charset = nl == 7 && !strncasecmp(name, "charset", 7), is_import = nl == 6 && !strncasecmp(name, "import", 6);
    /* (one that needs a block, @media; or @page;, is invalid: dropped, it
     * does not count; of the statements only @namespace does) */
    bool is_namespace = nl == 9 && !strncasecmp(name, "namespace", 9);
    if (is_namespace && !block) {
        /* (its prefix, if any, and URI: url(...) or a string) */
        char *c = css_strndup_trim(s + pre, pre_end - pre);
        const char *p = c ? c : "";
        char pfx[32] = { 0 }, uri[200] = { 0 };
        size_t k = 0;
        while (*p && !css_is_space(*p) && strncasecmp(p, "url(", 4) && *p != '"' && *p != '\'' && k + 1 < sizeof pfx) pfx[k++] = *p++;
        while (*p && css_is_space(*p)) p++;
        if (!*p && k) { memcpy(uri, pfx, sizeof pfx); pfx[0] = 0; p = ""; }
        const char *u = p;
        if (!strncasecmp(u, "url(", 4)) u += 4;
        while (*u && css_is_space(*u)) u++;
        char q = (*u == '"' || *u == '\'') ? *u++ : 0;
        size_t ul = 0;
        while (*u && (q ? *u != q : (*u != ')' && !css_is_space(*u))) && ul + 1 < sizeof uri) uri[ul++] = *u++;
        if (ul) uri[ul] = 0;
        /* (a prefix seen before: its URI now, moved last so a lookup finds it; the table never fills with repeats) */
        int at = -1;
        for (int i = 0; pfx[0] && i < g_css_nns; i++) if (!strcmp(g_css_ns[i].p, pfx)) at = i;
        if (at >= 0) {
            for (int i = at; i + 1 < g_css_nns; i++) g_css_ns[i] = g_css_ns[i + 1];
            g_css_nns--;
        }
        if (pfx[0] && g_css_nns < 16) {
            snprintf(g_css_ns[g_css_nns].p, sizeof g_css_ns[0].p, "%s", pfx);
            snprintf(g_css_ns[g_css_nns].uri, sizeof g_css_ns[0].uri, "%s", uri);
            g_css_nns++;
        } else if (!pfx[0]) { snprintf(g_css_ns_default, sizeof g_css_ns_default, "%s", uri); g_css_ns_has_default = true; }
        free(c);
    }
    if (counts) *counts = css_at_known(name, nl) && !is_charset && !is_import && (block ? true : is_namespace);
    bool is_layer = nl == 5 && !strncasecmp(name, "layer", 5);
    char *layer_here = NULL;
    if (is_layer) {
        /* @layer a, b; or @layer [a]? { }: the order is by what's named first */
        char *c = css_strndup_trim(s + pre, pre_end - pre);
        if (c && strstr(c, "/*")) css_strip_comments(c);
        const char *p = c ? c : "";
        char names[16][128];
        int nn = 0;
        bool bad = false;
        while (css_is_space(*p)) p++;
        while (*p && !bad) {
            if (nn >= 16 || !layer_name_next(&p, names[nn], sizeof names[0])) { bad = true; break; }
            nn++;
            while (css_is_space(*p)) p++;
            if (*p == ',') {
                p++;
                while (css_is_space(*p)) p++;
                if (!*p) bad = true;
            } else if (*p) bad = true;
        }
        free(c);
        if (block ? nn > 1 : nn == 0) bad = true;
        if (bad) {
            if (!block) return end;
            CssTz tz;
            css_tz_init(&tz, s, len, end);
            end = css_tokens_skip_block(&tz);
            css_tz_free(&tz);
            return end;
        }
        if (block) {
            if (nn) layer_here = layer_join(names[0]);
            else {
                /* (with no name: one of its own) */
                static int anon;
                char an[32];
                snprintf(an, sizeof an, "\x01%d", __atomic_add_fetch(&anon, 1, __ATOMIC_RELAXED));
                layer_here = layer_join(an);
            }
        }
        if (sheet) {
            char *held = t_layer_path;
            for (int k = 0; k < nn || (block && k == 0); k++) {
                char *pa = block ? strdup(layer_here) : layer_join(names[k]);
                t_layer_path = pa;
                css_stylesheet_add_rule(sheet);
                t_layer_path = held;
                free(pa);
            }
        }
    }
    if (!block) return end;
    if (sheet && ((nl == 9 && !strncasecmp(name, "keyframes", 9)) || (nl == 17 && !strncasecmp(name, "-webkit-keyframes", 17))))
        return css_keyframes_rule(s, len, pre, pre_end, end, sheet);
    if (sheet && nl == 12 && !strncasecmp(name, "position-try", 12)) {
        char *c = css_strndup_trim(s + pre, pre_end - pre);
        CssRule *r = c && c[0] == '-' && c[1] == '-' ? css_stylesheet_add_rule(sheet) : NULL;
        if (r) {
            r->try_name = c;
            css_parse_rule_block(s, len, &end, r, sheet, cond);
            return end;
        }
        free(c);
    }
    if (kind == AT_SKIP || !sheet) {
        free(layer_here);
        size_t e;
        if (css_block_skip_fast(s, len, end, &e)) return e;
        CssTz tz;
        css_tz_init(&tz, s, len, end);
        end = css_tokens_skip_block(&tz);
        css_tz_free(&tz);
        return end;
    }
    if (nl == 8 && !strncasecmp(name, "supports", 8)) {
        /* (@supports: its block only when the condition holds; an invalid one is false) */
        extern bool css_supports_condition(const char *cond);
        char *c = css_strndup_trim(s + pre, pre_end - pre);
        if (c && strstr(c, "/*")) css_strip_comments(c);
        bool on = c && c[0] && css_supports_condition(c);
        free(c);
        if (!on) {
            free(layer_here);
            size_t e;
            if (css_block_skip_fast(s, len, end, &e)) return e;
            CssTz tz;
            css_tz_init(&tz, s, len, end);
            end = css_tokens_skip_block(&tz);
            css_tz_free(&tz);
            return end;
        }
    }
    bool is_scope = nl == 5 && !strncasecmp(name, "scope", 5);
    char *sc_a = NULL, *sc_b = NULL;
    if (is_scope) {
        /* [(start)]? [to (end)]? */
        char *c = css_strndup_trim(s + pre, pre_end - pre);
        if (c && strstr(c, "/*")) css_strip_comments(c);
        const char *p = c ? c : "";
        char *parts[2] = { NULL, NULL };
        bool bad = false;
        for (int k = 0; k < 2 && !bad; k++) {
            while (css_is_space(*p)) p++;
            if (!*p) break;
            if (!strncasecmp(p, "to", 2) && (css_is_space(p[2]) || p[2] == '(')) {
                if (parts[1]) { bad = true; break; }
                p += 2;
                while (css_is_space(*p)) p++;
                k = 1;
            } else if (k == 1) { bad = true; break; }
            if (*p != '(') { bad = true; break; }
            int depth = 0;
            const char *q = p;
            for (; *q; q++) {
                if (*q == '(') depth++;
                else if (*q == ')' && --depth == 0) break;
            }
            if (!*q) { bad = true; break; }
            parts[k] = css_strndup_trim(p + 1, (size_t)(q - p - 1));
            /* (no pseudo-elements; a relative selector checked past its combinator) */
            const char *pv = parts[k] ? parts[k] : "";
            if (*pv == '>' || *pv == '+' || *pv == '~') { pv++; while (css_is_space(*pv)) pv++; }
            bool pe_in = false;
            for (const char *q2 = pv, *qb = NULL; *q2 && !pe_in; q2++) {
                if (*q2 == '[') qb = q2;
                else if (*q2 == ']') qb = NULL;
                else if (!qb && q2[0] == ':' && q2[1] == ':') pe_in = true;
            }
            if (!pv[0] || pe_in || css_prelude_invalid(pv)) bad = true;
            p = q + 1;
        }
        while (!bad && css_is_space(*p)) p++;
        if (*p) bad = true;
        free(c);
        if (bad) {
            free(parts[0]); free(parts[1]);
            CssTz tz;
            css_tz_init(&tz, s, len, end);
            end = css_tokens_skip_block(&tz);
            css_tz_free(&tz);
            return end;
        }
        /* (with no start: the owner's parent) */
        const char *start = parts[0] ? parts[0] : ":has(> style, > link)";
        const char *oa = t_nest_parent ? t_nest_parent : t_scope_a, *ob = t_nest_parent ? NULL : t_scope_b;
        sc_a = oa ? scope_apply(start, oa, ob, !t_nest_parent) : strdup(start);
        if (parts[1]) sc_b = scope_apply(parts[1], sc_a, NULL, true);
        free(parts[0]); free(parts[1]);
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
    char *held_cq = t_cq_cond;
    char *cq_here = NULL;
    if (nl == 9 && !strncasecmp(name, "container", 9)) {
        char *c = css_strndup_trim(s + pre, pre_end - pre);
        if (c && strstr(c, "/*")) css_strip_comments(c);
        if (c) {
            size_t n = strlen(c) + (held_cq ? strlen(held_cq) : 0) + 2;
            cq_here = (char *)malloc(n);
            if (cq_here) {
                if (held_cq) snprintf(cq_here, n, "%s\x01%s", held_cq, c);
                else snprintf(cq_here, n, "%s", c);
                t_cq_cond = cq_here;
                g_css_saw_container = true;
            }
            free(c);
        }
    }
    char *held_sa = t_scope_a, *held_sb = t_scope_b;
    const char *held_np = t_nest_parent;
    if (is_scope) { t_scope_a = sc_a; t_scope_b = sc_b; t_nest_parent = NULL; }
    char *held_layer = t_layer_path;
    if (layer_here) t_layer_path = layer_here;
    end = css_rule_list(s, len, end, len, true, sheet, eff ? eff : "");
    t_layer_path = held_layer;
    free(layer_here);
    if (is_scope) { t_scope_a = held_sa; t_scope_b = held_sb; t_nest_parent = held_np; free(sc_a); free(sc_b); }
    t_cq_cond = held_cq;
    free(cq_here);
    free(eff);
    return end;
}

/* The qualified rule or at-rule at pos (white space and comments skipped):
 * added to sheet unless NULL.  *counts: a valid style rule or a known
 * at-rule.  Returns where it ends. */
static void css_parse_rule_block(const char *s, size_t len, size_t *pos, CssRule *rule, CssStylesheet *sheet, const char *cond);

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
    if (t_scope_a) {
        char *sw = scope_apply(sel, t_scope_a, t_scope_b, true);
        if (sw) { free(sel); sel = sw; }
    }
    rule->selector_text = sel;
    rule->media_query = cond && cond[0] ? strdup(cond) : NULL; rule->container_query = t_cq_cond ? strdup(t_cq_cond) : NULL;
    css_parse_rule_block(s, len, &pos, rule, sheet, cond);
    return pos;
}

/* ---- CSS Nesting: a nested rule becomes a rule of the sheet, its selector
 * resolved against its parent's; declarations after it, a rule of the
 * parent's selector after it. ---- */
static void sel_append(char **buf, size_t *n, size_t *cap, const char *t, size_t l)
{
    if (*n + l + 1 > *cap) {
        size_t nc = (*n + l + 1) * 2;
        char *nb = (char *)realloc(*buf, nc);
        if (!nb) return;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *n, t, l);
    *n += l;
    (*buf)[*n] = 0;
}

/* sel in a scope: :scope and & as the root; without them under it; not past the limit. */
static char *scope_apply(const char *sel, const char *a, const char *b, bool prefix)
{
    size_t n = 0, cap = strlen(sel) + 64;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    out[0] = 0;
    const char *p = sel;
    while (*p) {
        const char *q = p, *pe = NULL;
        int depth = 0;
        char quote = 0;
        bool ref = false;
        for (; *q; q++) {
            if (*q == '\\' && q[1]) { q++; continue; }
            if (quote) { if (*q == quote) quote = 0; continue; }
            if (*q == '"' || *q == '\'') quote = *q;
            else if (*q == '(' || *q == '[') depth++;
            else if ((*q == ')' || *q == ']') && depth > 0) depth--;
            else if (*q == '&') ref = true;
            else if (*q == ':' && !strncasecmp(q, ":scope", 6) && !isalnum((unsigned char)q[6]) && q[6] != '-') ref = true;
            else if (*q == ':' && q[1] == ':' && !depth && !pe) pe = q;
            else if (*q == ',' && depth == 0) break;
        }
        const char *x = p, *y = q;
        while (x < y && css_is_space(*x)) x++;
        while (y > x && css_is_space(y[-1])) y--;
        if (!pe || pe > y) pe = y;
        if (n) sel_append(&out, &n, &cap, ", ", 2);
        if (!ref && prefix) {
            sel_append(&out, &n, &cap, ":where(", 7);
            sel_append(&out, &n, &cap, a, strlen(a));
            sel_append(&out, &n, &cap, ") ", 2);
        }
        quote = 0;
        for (const char *c = x; c < pe; c++) {
            if (*c == '\\' && c + 1 < pe) { sel_append(&out, &n, &cap, c, 2); c++; continue; }
            if (quote) { if (*c == quote) quote = 0; sel_append(&out, &n, &cap, c, 1); continue; }
            if (*c == '"' || *c == '\'') { quote = *c; sel_append(&out, &n, &cap, c, 1); continue; }
            if (*c == '&') {
                sel_append(&out, &n, &cap, ":where(", 7);
                sel_append(&out, &n, &cap, a, strlen(a));
                sel_append(&out, &n, &cap, ")", 1);
                continue;
            }
            if (*c == ':' && !strncasecmp(c, ":scope", 6) && !isalnum((unsigned char)c[6]) && c[6] != '-') {
                /* (of a pseudo-class's weight: the root's own is not counted) */
                sel_append(&out, &n, &cap, ":is(:where(", 11);
                sel_append(&out, &n, &cap, a, strlen(a));
                sel_append(&out, &n, &cap, "), .__cb_s)", 11);
                c += 5;
                continue;
            }
            sel_append(&out, &n, &cap, c, 1);
        }
        if (b) {
            /* (the limit, already in terms of the root, and what's under it) */
            sel_append(&out, &n, &cap, ":not(:where(", 12);
            sel_append(&out, &n, &cap, b, strlen(b));
            sel_append(&out, &n, &cap, "), :where(", 10);
            sel_append(&out, &n, &cap, b, strlen(b));
            sel_append(&out, &n, &cap, ") *)", 4);
        }
        if (pe < y) sel_append(&out, &n, &cap, pe, (size_t)(y - pe));
        p = *q ? q + 1 : q;
    }
    return out;
}

/* A compound that "&" can be written out as, as it is, before what follows. */
static bool nest_parent_plain(const char *p)
{
    if (*p != '.' && *p != '#') return false;
    for (; *p; p++)
        if (strchr(" \t\n\f\r,>+~()[\"'\\", *p) || (p[0] == ':' && p[1] == ':')) return false;
    return true;
}

static char *nest_resolve(const char *parent, const char *child)
{
    size_t n = 0, cap = strlen(parent) + strlen(child) + 16;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    out[0] = 0;
    bool plain = nest_parent_plain(parent);
    const char *p = child;
    while (*p) {
        /* One selector of the list. */
        const char *q = p;
        int depth = 0;
        char quote = 0;
        bool amp = false;
        for (; *q; q++) {
            if (*q == '\\' && q[1]) { q++; continue; }
            if (quote) { if (*q == quote) quote = 0; continue; }
            if (*q == '"' || *q == '\'') quote = *q;
            else if (*q == '(' || *q == '[') depth++;
            else if ((*q == ')' || *q == ']') && depth > 0) depth--;
            else if (*q == '&') amp = true;
            else if (*q == ',' && depth == 0) break;
        }
        const char *a = p, *b = q;
        while (a < b && css_is_space(*a)) a++;
        while (b > a && css_is_space(b[-1])) b--;
        if (n) sel_append(&out, &n, &cap, ", ", 2);
        if (!amp) {
            if (plain) sel_append(&out, &n, &cap, parent, strlen(parent));
            else {
                sel_append(&out, &n, &cap, ":is(", 4);
                sel_append(&out, &n, &cap, parent, strlen(parent));
                sel_append(&out, &n, &cap, ")", 1);
            }
            sel_append(&out, &n, &cap, " ", 1);
            sel_append(&out, &n, &cap, a, (size_t)(b - a));
        } else {
            for (const char *c = a; c < b; c++) {
                if (*c == '\\' && c + 1 < b) { sel_append(&out, &n, &cap, c, 2); c++; continue; }
                if (*c != '&') { sel_append(&out, &n, &cap, c, 1); continue; }
                /* (".a" before ".b", ":hover" or a combinator; not before a name) */
                bool next_name = c + 1 < b && (isalnum((unsigned char)c[1]) || c[1] == '-' || c[1] == '_');
                if (plain && !next_name) sel_append(&out, &n, &cap, parent, strlen(parent));
                else {
                    sel_append(&out, &n, &cap, ":is(", 4);
                    sel_append(&out, &n, &cap, parent, strlen(parent));
                    sel_append(&out, &n, &cap, ")", 1);
                }
            }
        }
        p = *q ? q + 1 : q;
    }
    return out;
}

/* A style rule's block, at pos (past its {): its declarations, then its rules. */
static void css_parse_rule_block(const char *s, size_t len, size_t *pos, CssRule *rule, CssStylesheet *sheet, const char *cond)
{
    bool body = t_scope_body;
    t_scope_body = false;
    CssNestList saved = t_nest;
    memset(&t_nest, 0, sizeof t_nest);
    t_nest.on = sheet != NULL;
    css_parse_declaration_block(s, len, pos, rule);
    CssNestList mine = t_nest;
    t_nest = saved;
    if (!mine.n) { free(mine.v); return; }
    /* (adding a rule may move this one) */
    char *parent = strdup(rule->selector_text);
    const char *outer = t_nest_parent;
    /* (what comes after the first nested rule waits for the rules before it) */
    int total = rule->declaration_count, first = mine.v[0].decls;
    CssDeclaration *later = total > first ? (CssDeclaration *)malloc((size_t)(total - first) * sizeof(CssDeclaration)) : NULL;
    if (later) {
        memcpy(later, rule->declarations + first, (size_t)(total - first) * sizeof(CssDeclaration));
        rule->declaration_count = first;
    } else {
        first = total;
    }
    for (int i = 0; parent && i < mine.n; i++) {
        size_t a = mine.v[i].start, b = mine.v[i].brace;
        if (s[a] == '@') {
            t_nest_parent = body ? NULL : parent;
            css_at_rule(s, len, a, true, sheet, cond, NULL);
            t_nest_parent = outer;
        } else {
            char *child = css_strndup_trim(s + a, b - a);
            char *resolved = !child || !child[0] ? NULL : body ? scope_apply(child, t_scope_a, t_scope_b, true) : nest_resolve(parent, child);
            free(child);
            size_t p = b + 1;
            CssRule *r = resolved && !css_prelude_invalid(resolved) ? css_stylesheet_add_rule(sheet) : NULL;
            if (r) {
                r->selector_text = resolved;
                r->media_query = cond && cond[0] ? strdup(cond) : NULL; r->container_query = t_cq_cond ? strdup(t_cq_cond) : NULL;
                css_parse_rule_block(s, len, &p, r, sheet, cond);
            } else {
                free(resolved);
            }
        }
        /* The declarations up to the next one. */
        int from = mine.v[i].decls, to = i + 1 < mine.n ? mine.v[i + 1].decls : total;
        if (later && to > from) {
            CssRule *r = css_stylesheet_add_rule(sheet);
            if (r) {
                r->selector_text = strdup(parent);
                r->media_query = cond && cond[0] ? strdup(cond) : NULL; r->container_query = t_cq_cond ? strdup(t_cq_cond) : NULL;
                r->declarations = (CssDeclaration *)malloc((size_t)(to - from) * sizeof(CssDeclaration));
                if (r->declarations) {
                    memcpy(r->declarations, later + (from - first), (size_t)(to - from) * sizeof(CssDeclaration));
                    r->declaration_count = r->declaration_capacity = to - from;
                }
            }
        }
    }
    free(later);
    free(parent);
    free(mine.v);
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
    /* (in a nested at-rule the block is its parent's, under the condition) */
    if (nested && t_nest_parent && sheet) {
        CssRule *r = css_stylesheet_add_rule(sheet);
        if (r) {
            r->selector_text = strdup(t_nest_parent);
            r->media_query = cond && cond[0] ? strdup(cond) : NULL; r->container_query = t_cq_cond ? strdup(t_cq_cond) : NULL;
            const char *outer = t_nest_parent;
            t_nest_parent = NULL;
            css_parse_rule_block(s, len, &pos, r, sheet, cond);
            t_nest_parent = outer;
            return pos;
        }
    }
    /* (an @scope's block: declarations of their own apply to the root, without weight) */
    if (nested && t_scope_a && sheet) {
        char *root = scope_apply(":scope", t_scope_a, t_scope_b, false);
        size_t rl = root ? strlen(root) + 9 : 0;
        char *rs = root ? (char *)malloc(rl) : NULL;
        CssRule *r = rs ? css_stylesheet_add_rule(sheet) : NULL;
        if (r) {
            snprintf(rs, rl, ":where(%s)", root);
            free(root);
            r->selector_text = rs;
            r->media_query = cond && cond[0] ? strdup(cond) : NULL; r->container_query = t_cq_cond ? strdup(t_cq_cond) : NULL;
            bool held = t_scope_body;
            t_scope_body = true;
            css_parse_rule_block(s, len, &pos, r, sheet, cond);
            t_scope_body = held;
            return pos;
        }
        free(root);
        free(rs);
    }
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
    if (sheet && sheet->rule_count == 0 && sheet->kf_n == 0) {
        css_stylesheet_free(sheet);
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
        free(rule->layer_name);
        free(rule->scope_root);
        free(rule->try_name);
        free(rule->media_query);
        free(rule->container_query);
        css_declarations_free(rule->declarations, rule->declaration_count);
        css_rule_compiled_free(rule);
    }
    free(sheet->rules);
    for (int i = 0; i < sheet->frame_n; i++)
        if (sheet->frames[i].owns) css_declarations_free(sheet->frames[i].decls, sheet->frames[i].n);
    for (int i = 0; i < sheet->kf_n; i++) free(sheet->kf[i].name);
    free(sheet->frames);
    free(sheet->kf);
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

/* (whether [arg, arg + al) is An+B: odd, even, an integer, or [+-]?[digits]n[+-digits]) */
static bool css_nth_ok(const char *arg, size_t al)
{
    char b[64];
    size_t w = 0;
    for (size_t j = 0; j < al && w + 1 < sizeof b; j++) if (!css_is_space(arg[j])) b[w++] = (char)tolower((unsigned char)arg[j]);
    b[w] = 0;
    if (!w) return false;
    if (!strcmp(b, "odd") || !strcmp(b, "even")) return true;
    size_t i = 0;
    if (b[i] == '+' || b[i] == '-') i++;
    size_t d0 = i;
    while (isdigit((unsigned char)b[i])) i++;
    if (!b[i]) return i > d0;
    if (b[i] != 'n') return false;
    i++;
    if (!b[i]) return true;
    if (b[i] != '+' && b[i] != '-') return false;
    i++;
    d0 = i;
    while (isdigit((unsigned char)b[i])) i++;
    return !b[i] && i > d0;
}

uint32_t css_state_bit(const char *name, size_t nl)
{
    static const struct { const char *n; uint32_t bit; } k[] = {
        { "hover", DOM_ST_HOVER }, { "active", DOM_ST_ACTIVE }, { "focus", DOM_ST_FOCUS },
        { "focus-visible", DOM_ST_FOCUS_VISIBLE }, { "focus-within", DOM_ST_FOCUS_WITHIN }, { "target", DOM_ST_TARGET },
        { "checked", DOM_ST_CHECKED }, { "indeterminate", DOM_ST_INDETERMINATE }, { "playing", DOM_ST_PLAYING },
        { "paused", DOM_ST_PAUSED }, { "muted", DOM_ST_MUTED }, { "seeking", DOM_ST_SEEKING },
        { "buffering", DOM_ST_BUFFERING }, { "stalled", DOM_ST_STALLED }, { "enabled", DOM_ST_ENABLED },
        { "disabled", DOM_ST_DISABLED }, { "read-write", DOM_ST_READ_WRITE }, { "read-only", DOM_ST_READ_ONLY },
        { "required", DOM_ST_REQUIRED }, { "optional", DOM_ST_OPTIONAL },
        { "modal", DOM_ST_MODAL }, { "popover-open", DOM_ST_POPOVER_OPEN },
        { "target-current", DOM_ST_TARGET_CURRENT },   /* (an anchor of a scroll-target-group: its target the current one) */
        { "valid", DOM_ST_VALID }, { "invalid", DOM_ST_INVALID },
        { "placeholder-shown", DOM_ST_PLACEHOLDER_SHOWN }, { "default", DOM_ST_DEFAULT },
        { "in-range", DOM_ST_IN_RANGE }, { "out-of-range", DOM_ST_OUT_OF_RANGE },
    };
    extern bool g_css_saw_validity;
    for (size_t i = sizeof k / sizeof k[0] - 6; i < sizeof k / sizeof k[0]; i++)
        if (strlen(k[i].n) == nl && !strncasecmp(k[i].n, name, nl)) g_css_saw_validity = true;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (strlen(k[i].n) == nl && !strncasecmp(k[i].n, name, nl)) return k[i].bit;
    return 0;
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
    /* (the shadow host a scoped sheet's :host became: by its NodeId) */
    if (css_pseudo_name_is(name, nl, "-webkit-cyber-host")) {
        out->node_id = arg ? (uint32_t)strtoul(arg, NULL, 10) : 0;
        if (!out->node_id) out->never_match = true;
        out->has_substantive = true;
        return;
    }
    /* (:host outside of a shadow tree's own rewriting: there's no such element) */
    if (css_pseudo_name_is(name, nl, "host") || css_pseudo_name_is(name, nl, "host-context")) {
        out->never_match = true;
        return;
    }
    /* (:scope outside @scope: the root) */
    if (css_pseudo_name_is(name, nl, "root") || css_pseudo_name_is(name, nl, "scope")) {
        out->is_root = true;
        out->has_substantive = true;
        return;
    }
    if (css_pseudo_name_is(name, nl, "empty")) { out->requires_empty = true; return; }
    if (css_pseudo_name_is(name, nl, "link") || css_pseudo_name_is(name, nl, "any-link")) {
        out->requires_link = true;
        out->link_kind = css_pseudo_name_is(name, nl, "link") ? 1 : 0;
        return;
    }
    /* (:visited: a link to this document itself, the one page in its history) */
    if (css_pseudo_name_is(name, nl, "visited")) { out->requires_link = true; out->link_kind = 2; return; }
    uint32_t state_bit = css_state_bit(name, nl);
    if (state_bit) {
        out->state_mask |= state_bit;
        out->has_substantive = true;
        return;
    }
    /* (states nothing here has: no history, no autofill, no timeline) */
    if (css_pseudo_name_is(name, nl, "target-within") ||
        css_pseudo_name_is(name, nl, "autofill") ||
        css_pseudo_name_is(name, nl, "current") ||
        css_pseudo_name_is(name, nl, "past") ||
        css_pseudo_name_is(name, nl, "future") ||
        css_pseudo_name_is(name, nl, "local-link") || css_pseudo_name_is(name, nl, "open") ||
        css_pseudo_name_is(name, nl, "closed") || css_pseudo_name_is(name, nl, "fullscreen") ||
        css_pseudo_name_is(name, nl, "picture-in-picture") || css_pseudo_name_is(name, nl, "volume-locked") ||
        css_pseudo_name_is(name, nl, "xr-overlay") || css_pseudo_name_is(name, nl, "user-valid") ||
        css_pseudo_name_is(name, nl, "user-invalid") || css_pseudo_name_is(name, nl, "first") ||
        css_pseudo_name_is(name, nl, "left") || css_pseudo_name_is(name, nl, "right") ||
        css_pseudo_name_is(name, nl, "blank")) {
        out->never_match = true;
        return;
    }
    if (css_pseudo_name_is(name, nl, "first-child")) { out->child_pos = 1; return; }
    if (css_pseudo_name_is(name, nl, "last-child")) { out->child_pos = 2; return; }
    if (css_pseudo_name_is(name, nl, "only-child")) { out->child_pos = 3; return; }
    if (css_pseudo_name_is(name, nl, "first-of-type")) { out->type_pos = 1; return; }
    if (css_pseudo_name_is(name, nl, "last-of-type")) { out->type_pos = 2; return; }
    if (css_pseudo_name_is(name, nl, "only-of-type")) { out->type_pos = 3; return; }
    if (css_pseudo_name_is(name, nl, "nth-child") || css_pseudo_name_is(name, nl, "nth-last-child")) {
        out->nth_kind = css_pseudo_name_is(name, nl, "nth-child") ? 1 : 2;
        /* (An+B of S: the selector list after " of ") */
        size_t cut = al;
        /* (the ident "of": what follows it no name character, a comment, '.', '[' as well as space) */
        for (size_t j = 0; arg && j + 3 < al; j++) {
            char nx = arg[j + 3];
            if (css_is_space(arg[j]) && (arg[j + 1] == 'o' || arg[j + 1] == 'O') && (arg[j + 2] == 'f' || arg[j + 2] == 'F') &&
                !(isalnum((unsigned char)nx) || nx == '-' || nx == '_' || (unsigned char)nx >= 0x80)) { cut = j; break; }
        }
        css_parse_nth(arg, cut, &out->nth_a, &out->nth_b);
        if (!css_nth_ok(arg, cut)) out->never_match = true;
        if (cut < al) {
            size_t s0 = cut + 3;
            for (;;) {
                while (s0 < al && css_is_space(arg[s0])) s0++;
                if (s0 + 1 < al && arg[s0] == '/' && arg[s0 + 1] == '*') {
                    size_t e = s0 + 2;
                    while (e + 1 < al && !(arg[e] == '*' && arg[e + 1] == '/')) e++;
                    s0 = e + 2 <= al ? e + 2 : al;
                    continue;
                }
                break;
            }
            size_t n = al - s0;
            if (n == 0) out->never_match = true;   /* (of nothing: invalid) */
            if (n > 0 && n < sizeof(out->nth_of_arg)) {
                memcpy(out->nth_of_arg, arg + s0, n);
                out->nth_of_arg[n] = '\0';
                out->nth_of_active = true;
            }
        }
        return;
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
        /* (a list, a combinator or another pseudo-class in it: the list,
         * evaluated at match time; a lone state flattens like the rest) */
        size_t pk = 0, pe = al;
        while (pk < pe && css_is_space(arg[pk])) pk++;
        while (pe > pk && css_is_space(arg[pe - 1])) pe--;
        /* (a second :not() in the compound: into the list form, not A and not B being not (A, B)) */
        if ((out->not_active || out->not_list_active) && pe > pk) {
            size_t cur = out->not_list_active ? strlen(out->not_list_arg) : 0;
            if (cur + (pe - pk) + 3 < sizeof(out->not_list_arg)) {
                if (cur) { out->not_list_arg[cur++] = ','; out->not_list_arg[cur++] = ' '; }
                memcpy(out->not_list_arg + cur, arg + pk, pe - pk);
                out->not_list_arg[cur + (pe - pk)] = 0;
                out->not_list_active = true;
                out->has_substantive = true;
            }
            return;
        }
        bool flat = pe > pk && pe - pk < sizeof(out->not_list_arg);
        for (size_t i = pk; flat && i < pe; i++)
            if (strchr(",>+~ \t\n()", arg[i])) flat = false;
        if (flat)
            for (const char *c = memchr(arg + pk, ':', pe - pk); c && flat; c = memchr(c + 1, ':', (size_t)(arg + pe - c - 1))) {
                const char *e = c + 1;
                while (e < arg + pe && (isalnum((unsigned char)*e) || *e == '-')) e++;
                if (*(c + 1) == ':' || !css_state_bit(c + 1, (size_t)(e - c - 1))) flat = false;
            }
        if (!flat && pe > pk && pe - pk < sizeof(out->not_list_arg)) {
            memcpy(out->not_list_arg, arg + pk, pe - pk);
            out->not_list_arg[pe - pk] = '\0';
            out->not_list_active = true;
            out->has_substantive = true;
            return;
        }
        CssSimpleSelector tmp;
        /* (namespaces aren't flattened into :not(): as before, not tested there) */
        css_parse_simple_selector(arg, al, &tmp);
        if (tmp.has_tag || tmp.has_id || tmp.class_count > 0 || tmp.attr_count > 0 ||
            tmp.universal || tmp.state_mask) {
            out->not_active = true;
            out->has_substantive = true;
            out->not_state = tmp.state_mask;
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
            out->is_weighted = !css_pseudo_name_is(name, nl, "where");
            out->has_substantive = true;
        }
        return;
    }
    if (css_pseudo_name_is(name, nl, "heading") && !out->where_active) {
        /* (as a test of those tags: none, or the levels asked for) */
        char list[64] = "";
        size_t used = 0;
        for (int level = 1; level <= 6; level++) {
            bool want = al == 0;
            for (size_t k = 0; k < al && !want; k++) {
                if (arg[k] < '0' || arg[k] > '9') continue;
                size_t j = k;
                int v = 0;
                while (j < al && arg[j] >= '0' && arg[j] <= '9') v = v * 10 + (arg[j++] - '0');
                if (v == level) want = true;
                k = j;
            }
            if (want) used += (size_t)snprintf(list + used, sizeof(list) - used, "%sh%d", used ? ", " : "", level);
        }
        snprintf(out->where_arg, sizeof(out->where_arg), "%s", used ? list : "h0:not(*)");
        out->where_active = true;
        out->has_substantive = true;
        return;
    }
    /* (:active-view-transition, -type(a, b): the root while a view transition runs,
     * its types in data-cyber-avt (platform_api.js), as :is() of attribute tests) */
    if (css_pseudo_name_is(name, nl, "active-view-transition")) {
        if (arg) { out->never_match = true; return; }
        if (!out->where_active) {
            snprintf(out->where_arg, sizeof(out->where_arg), "[data-cyber-avt]");
            out->where_active = true;
            out->is_weighted = true;
        }
        out->has_substantive = true;
        return;
    }
    if (css_pseudo_name_is(name, nl, "active-view-transition-type")) {
        char list[sizeof(out->where_arg)];
        size_t used = 0, k = 0;
        bool ok = arg && al > 0;
        while (ok && k <= al) {
            size_t e = k;
            while (e < al && arg[e] != ',') e++;
            size_t a0 = k, a1 = e;
            while (a0 < a1 && css_is_space(arg[a0])) a0++;
            while (a1 > a0 && css_is_space(arg[a1 - 1])) a1--;
            if (a1 == a0 || a1 - a0 > 60) { ok = false; break; }
            for (size_t q = a0; q < a1 && ok; q++) {
                unsigned char ch = (unsigned char)arg[q];
                ok = ch >= 0x80 || ch == '-' || ch == '_' || (ch >= '0' && ch <= '9' && q > a0) || ((ch | 32) >= 'a' && (ch | 32) <= 'z');
            }
            if (!ok) break;
            used += (size_t)snprintf(list + used, used < sizeof list ? sizeof list - used : 0, "%s[data-cyber-avt~=\"%.*s\"]",
                                     used ? ", " : "", (int)(a1 - a0), arg + a0);
            k = e + 1;
        }
        if (!ok || used >= sizeof list) { out->never_match = true; return; }
        snprintf(out->where_arg, sizeof(out->where_arg), "%s", list);
        out->where_active = true;
        out->is_weighted = true;
        out->has_substantive = true;
        return;
    }
    if (css_pseudo_name_is(name, nl, "dir")) {
        /* (the element's directionality: its dir attribute's or the nearest ancestor's) */
        size_t k = 0, e = al;
        while (k < e && css_is_space(arg[k])) k++;
        while (e > k && css_is_space(arg[e - 1])) e--;
        out->dir_val = e - k == 3 && !strncasecmp(arg + k, "ltr", 3) ? 1 : e - k == 3 && !strncasecmp(arg + k, "rtl", 3) ? 2 : 3;
        out->has_substantive = true;
        return;
    }
    if (css_pseudo_name_is(name, nl, "lang")) {
        /* The element's language, its lang attribute's or the nearest
         * ancestor's (else the document's, from a content-language meta),
         * is the range or starts with it and a '-' (case insensitive): as
         * a test of those attributes. */
        size_t k = 0, e = al;
        /* (ranges the attribute tests can't say, RFC 4647 extended filtering:
         * matched as they are) */
        bool complex = false;
        for (size_t i = 0; i < al; i++) if (arg[i] == ',' || arg[i] == '*' || arg[i] == '\\') complex = true;
        /* (each item a string or an identifier, else nothing matches) */
        for (size_t i = 0; i < al;) {
            while (i < al && css_is_space(arg[i])) i++;
            if (i < al && (isdigit((unsigned char)arg[i]) || ((arg[i] == '-' || arg[i] == '+') && i + 1 < al && isdigit((unsigned char)arg[i + 1])))) {
                out->never_match = true;
                return;
            }
            while (i < al && arg[i] != ',') i++;
            if (i < al) i++;
        }
        if (complex) {
            if (al > 0 && al < sizeof(out->lang_ranges) && !out->lang_ranges[0]) {
                memcpy(out->lang_ranges, arg, al);
                out->lang_ranges[al] = '\0';
                out->has_substantive = true;
                out->lang_count++;
            } else out->never_match = true;
            return;
        }
        while (k < e && (css_is_space(arg[k]) || arg[k] == '"' || arg[k] == '\'')) k++;
        while (e > k && (css_is_space(arg[e - 1]) || arg[e - 1] == '"' || arg[e - 1] == '\'')) e--;
        if (e > k && e - k < 40 && !out->where_active) {
            char r[48];
            memcpy(r, arg + k, e - k);
            r[e - k] = '\0';
            snprintf(out->where_arg, sizeof(out->where_arg),
                     "[lang|=\"%s\" i], [lang|=\"%s\" i] *:not([lang]), "
                     "html:not([lang]):has(meta[http-equiv=\"content-language\" i][content|=\"%s\" i]), "
                     "html:not([lang]):has(meta[http-equiv=\"content-language\" i][content|=\"%s\" i]) *:not([lang])",
                     r, r, r, r);
            out->where_active = true;
            out->has_substantive = true;
            out->lang_count++;
        }
        return;
    }
    if (css_pseudo_name_is(name, nl, "has")) {
        /* Relative selector: an optional leading combinator, then a selector
         * matched against descendants / children / following siblings. */
        out->has_substantive = true;
        /* (:visited inside :has() never matches: no history leaks through it) */
        if (arg && al > 0) {
            for (size_t q = 0; q + 8 <= al; q++)
                if (!strncasecmp(arg + q, ":visited", 8)) { out->never_match = true; return; }
        }
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

/* An empty simple selector: its counts and flags zero, its strings empty.
 * The arrays (some 6 KB) are read only up to their counts or under their
 * flags, so they are not cleared: a compound is parsed tens of thousands
 * of times when a large sheet compiles. */
static void css_simple_reset(CssSimpleSelector *out) {
    out->tag[0] = out->id[0] = out->not_tag[0] = out->not_id[0] = '\0';
    out->where_arg[0] = out->has_arg[0] = '\0';
    out->class_count = out->attr_count = 0;
    out->has_tag = out->has_id = out->is_root = out->universal = false;
    out->tag_upper = false;
    out->has_substantive = out->is_pseudo_element = out->requires_empty = false;
    out->never_match = out->requires_link = false;
    out->link_kind = 0;
    out->state_mask = out->not_state = 0;
    out->not_list_active = false;
    out->not_list_arg[0] = '\0';
    out->child_pos = out->type_pos = out->nth_kind = out->nth_a = out->nth_b = 0;
    out->dir_val = 0;
    out->node_id = 0;
    out->lang_ranges[0] = '\0';
    out->nth_of_active = false;
    out->nth_of_arg[0] = '\0';
    out->not_active = out->not_has_tag = out->not_has_id = false;
    out->not_class_count = out->not_attr_count = 0;
    out->where_active = out->has_active = out->is_weighted = false;
    out->lang_count = 0;
    out->has_comb = 0;
}

static void css_parse_simple_selector(const char *s, size_t n, CssSimpleSelector *out) {
    css_simple_reset(out);
    size_t i = 0;
    css_skip_space(s, n, &i);
    if (i >= n) return;
    /* (a namespace prefix before the type: ns|, *| or |; the namespace as a test of "-cb-ns") */
    {
        size_t k = i;
        if (s[k] == '*') k++;
        else if (s[k] != '|') k = css_sel_name_end(s, n, k, ".#[:|");
        const char *want = NULL;
        bool has_prefix = k < n && s[k] == '|' && (k + 1 >= n || s[k + 1] != '=');
        if (has_prefix) {
            if (k == i) want = "";
            else if (!(k == i + 1 && s[i] == '*')) {
                want = css_ns_lookup(s + i, k - i);
                if (!want) out->never_match = true;     /* (a prefix never declared: invalid) */
            }
            i = k + 1;
        }
        /* (a default namespace isn't applied: the table outlives its sheet) */
        (void)g_css_ns_default; (void)g_css_ns_has_default;
        if (want && out->attr_count < CSS_MAX_ATTRS) {
            int a = out->attr_count++;
            snprintf(out->attrs[a], sizeof out->attrs[0], "-cb-ns");
            out->attr_ops[a] = CSS_ATTR_OP_EQ;
            out->attr_ci[a] = false;
            snprintf(out->attr_vals[a], sizeof out->attr_vals[0], "%s", want);
            g_css_type_ns = 1;
        }
    }

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
        for (size_t k = start; k < i; k++) if (s[k] >= 'A' && s[k] <= 'Z') out->tag_upper = true;
        out->has_tag = out->tag[0] != '\0';
        out->has_substantive = out->has_substantive || out->has_tag;
    }

    while (i < n) {
        css_skip_space(s, n, &i);
        if (i >= n) break;
        if (s[i] == '.') {
            i++;
            size_t start = i;
            i = css_sel_name_end(s, n, i, ".#[:*");
            /* (.foo-*: a class prefix, an attribute test on class's tokens) */
            if (i < n && s[i] == '*' && i > start + 1 && s[i - 1] == '-') {
                if (out->attr_count < CSS_MAX_ATTRS) {
                    int k = out->attr_count;
                    css_sel_name_copy(out->attrs[k], "class", 5, sizeof(out->attrs[0]));
                    out->attr_ops[k] = (unsigned char)CSS_ATTR_OP_TOKPREFIX;
                    out->attr_ci[k] = false;
                    size_t vl = i - start < sizeof(out->attr_vals[0]) - 1 ? i - start : sizeof(out->attr_vals[0]) - 1;
                    memcpy(out->attr_vals[k], s + start, vl);
                    out->attr_vals[k][vl] = 0;
                    out->attr_count++;
                    out->has_substantive = true;
                }
                i++;
                if (i < n && s[i] == '*') out->never_match = true;
                continue;
            }
            if (i < n && s[i] == '*') out->never_match = true;     /* (.foo*: no such selector) */
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
            /* `*|name`: any namespace (a "*|" entry); `|name`: none; `p|name`: the
             * qualified name p:name, as the document writes it */
            char qual[64];
            size_t ql = 0;
            if (i + 1 < n && s[i] == '*' && s[i + 1] == '|') { i += 2; qual[0] = '*'; qual[1] = '|'; ql = 2; g_css_attr_any_ns = 1; }
            else if (i + 1 < n && s[i] == '|' && s[i + 1] != '=') i++;
            else {
                size_t j = css_sel_name_end(s, n, i, "=~|^$*] \t\n\r\f");
                if (j > i && j + 1 < n && s[j] == '|' && s[j + 1] != '=' && j - i + 1 < sizeof qual) {
                    memcpy(qual, s + i, j - i);
                    ql = j - i;
                    qual[ql++] = ':';
                    i = j + 1;
                }
            }
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
                if (ql) {
                    char t[64];
                    snprintf(t, sizeof t, "%.*s%s", (int)ql, qual, out->attrs[k]);
                    memcpy(out->attrs[k], t, sizeof t);
                }
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

/* A selector list's most specific selector (past a relative's combinator). */
static int css_list_best_specificity(const char *p)
{
    int best = 0;
    while (*p) {
        const char *e = p;
        int depth = 0;
        for (; *e && (depth || *e != ','); e++) depth += *e == '(' ? 1 : *e == ')' ? -1 : 0;
        const char *st = p;
        while (st < e && css_is_space(*st)) st++;
        if (st < e && (*st == '>' || *st == '+' || *st == '~')) st++;
        char one[400];
        size_t l = (size_t)(e - st);
        if (l >= sizeof one) l = sizeof one - 1;
        memcpy(one, st, l);
        one[l] = '\0';
        int s = css_specificity_from_selector_text(one);
        if (s > best) best = s;
        p = *e ? e + 1 : e;
    }
    return best;
}

static int css_specificity_from_simple(const CssSimpleSelector *simple) {
    int a = simple->has_id ? 1 : 0;
    int b = simple->class_count;
    if (simple->universal) b = 0;
    b += simple->attr_count;
    b += (simple->child_pos ? 1 : 0) + (simple->type_pos ? 1 : 0) +
         (simple->nth_kind ? 1 : 0) + (simple->is_root ? 1 : 0) +
         (simple->requires_link ? 1 : 0) + (simple->requires_empty ? 1 : 0) + simple->lang_count;
    b += __builtin_popcount(simple->state_mask);
    if (simple->not_active) {
        a += simple->not_has_id ? 1 : 0;
        b += simple->not_class_count + simple->not_attr_count + __builtin_popcount(simple->not_state);
    }
    int c = (simple->has_tag && !simple->universal) ? 1 : 0;
    if (simple->not_active && simple->not_has_tag) c++;
    int spec = a * 1000 + b * 10 + c;
    /* (:not() of a list: its most specific selector) */
    if (simple->not_list_active) spec += css_list_best_specificity(simple->not_list_arg);
    if (simple->nth_of_active) spec += css_list_best_specificity(simple->nth_of_arg);
    if (simple->where_active && simple->is_weighted) spec += css_list_best_specificity(simple->where_arg);
    if (simple->has_active) spec += css_list_best_specificity(simple->has_arg);
    return spec;
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
    if (!css_simple_substantive(simple)) return false;
    if (simple->never_match || simple->state_mask) return false;
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
        /* (visited: a link to the document itself, an empty href) */
        const char *hv = html_node_attr_value(node, "href");
        while (*hv == ' ' || *hv == 9 || *hv == 10) hv++;
        if (simple->link_kind == 1 && !*hv) return false;
        if (simple->link_kind == 2 && *hv) return false;
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
    if (da->proximity != db->proximity) return db->proximity - da->proximity;
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

static __thread double t_mq_vw, t_mq_vh;     /* (the viewport a query is asked of) */
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
    /* (of the viewport; small, large and dynamic are the one here) */
    {
        const char *u = *sp;
        if ((*u == 's' || *u == 'l' || *u == 'd' || *u == 'S' || *u == 'L' || *u == 'D') && (u[1] == 'v' || u[1] == 'V')) u++;
        static const char *const vu[] = { "vw", "vi", "vh", "vb", "vmin", "vmax" };
        for (int i = 0; i < 6; i++) {
            size_t n = strlen(vu[i]);
            if (strncasecmp(u, vu[i], n) || isalpha((unsigned char)u[n])) continue;
            *sp = u + n;
            double b = i < 2 ? t_mq_vw : i < 4 ? t_mq_vh : i == 4 ? (t_mq_vw < t_mq_vh ? t_mq_vw : t_mq_vh) : (t_mq_vw > t_mq_vh ? t_mq_vw : t_mq_vh);
            return v * b / 100.0;
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
    if (!mq_number(s, slash, &a) || !mq_number(slash + 1, e, &b) || a < 0.0 || b < 0.0) return false;
    /* (a degenerate ratio is a value, matching nothing but itself) */
    *out = b != 0.0 ? a / b : NAN;
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
static const char *g_mq_color_scheme;

bool css_media_set_color_scheme(const char *scheme) {
    if (scheme == g_mq_color_scheme) return false;
    g_mq_color_scheme = scheme;
    return true;
}

/* (navigator.preferences' overrides of the user's preferences, by feature; NULL: none) */
static const char *const k_mq_pref_names[] = { "prefers-color-scheme", "prefers-reduced-motion", "prefers-contrast",
                                               "prefers-reduced-transparency", "prefers-reduced-data" };
static char g_mq_pref[5][24];

bool css_media_set_pref(const char *name, const char *value) {
    for (int i = 0; i < 5; i++) {
        if (strcmp(name, k_mq_pref_names[i])) continue;
        const char *v = value ? value : "";
        if (!strcmp(g_mq_pref[i], v)) return false;
        snprintf(g_mq_pref[i], sizeof g_mq_pref[i], "%s", v);
        return true;
    }
    return false;
}

static bool mq_feature_lookup(const char *name, double vw, double vh, MqFeature *f) {
    t_mq_vw = vw;
    t_mq_vh = vh;
    static const struct { const char *name, *value; } idents[] = {
        { "prefers-color-scheme", "dark" }, { "prefers-reduced-motion", "no-preference" },
        { "prefers-contrast", "no-preference" }, { "prefers-reduced-transparency", "no-preference" },
        { "prefers-reduced-data", "no-preference" }, { "forced-colors", "none" },
        { "inverted-colors", "none" }, { "-ms-high-contrast", "none" },
        { "hover", "hover" }, { "any-hover", "hover" }, { "pointer", "fine" }, { "any-pointer", "fine" },
        { "display-mode", "browser" }, { "color-gamut", "srgb" }, { "dynamic-range", "standard" },
        { "video-dynamic-range", "standard" }, { "update", "fast" }, { "overflow-block", "scroll" },
        { "overflow-inline", "scroll" }, { "scripting", "enabled" }, { "scan", "none" },
        { "display-state", "normal" }, { "resizable", "true" }, { "navigation-controls", "none" },
    };
    memset(f, 0, sizeof(*f));
    f->name = name;
    double ratio = vh > 0 ? vw / vh : 0;
    /* (the device: what window.screen says) */
    if (strcmp(name, "device-width") == 0) { f->type = MQ_LEN; f->num = 1920; }
    else if (strcmp(name, "device-height") == 0) { f->type = MQ_LEN; f->num = 1080; }
    else if (strcmp(name, "device-aspect-ratio") == 0) { f->type = MQ_RATIO; f->num = 1920.0 / 1080.0; }
    else if (strcmp(name, "width") == 0) { f->type = MQ_LEN; f->num = vw; }
    else if (strcmp(name, "height") == 0) { f->type = MQ_LEN; f->num = vh; }
    else if (strcmp(name, "aspect-ratio") == 0) { f->type = MQ_RATIO; f->num = ratio; }
    else if (strcmp(name, "resolution") == 0) { f->type = MQ_RES; f->num = 1.0; }
    else if (strcmp(name, "device-pixel-ratio") == 0) { f->type = MQ_NUM; f->num = 1.0; }
    else if (strcmp(name, "color") == 0) { f->type = MQ_NUM; f->num = 8.0; }
    else if (strcmp(name, "color-index") == 0 || strcmp(name, "monochrome") == 0 ||
             strcmp(name, "grid") == 0) { f->type = MQ_NUM; f->num = 0.0; }
    else if (strcmp(name, "orientation") == 0) { f->type = MQ_IDENT; f->ident = vh >= vw ? "portrait" : "landscape"; }
    else {
        for (size_t i = 0; i < sizeof(idents) / sizeof(idents[0]); i++)
            if (strcmp(name, idents[i].name) == 0) {
                f->type = MQ_IDENT;
                f->ident = i == 0 && g_mq_color_scheme ? g_mq_color_scheme : idents[i].value;
                for (int k = 0; k < 5; k++) if (!strcmp(name, k_mq_pref_names[k]) && g_mq_pref[k][0]) f->ident = g_mq_pref[k];
                return true; }
        return false;
    }
    return true;
}

/* Whether v is one of the values feature name has. */
static bool mq_ident_allowed(const char *name, const char *v) {
    static const char *const values[][2] = {
        { "orientation", " portrait landscape " }, { "prefers-color-scheme", " light dark " },
        { "prefers-reduced-motion", " no-preference reduce " }, { "prefers-contrast", " no-preference less more custom " },
        { "prefers-reduced-transparency", " no-preference reduce " }, { "prefers-reduced-data", " no-preference reduce " },
        { "forced-colors", " none active " }, { "inverted-colors", " none inverted " },
        { "hover", " none hover " }, { "any-hover", " none hover " },
        { "pointer", " none coarse fine " }, { "any-pointer", " none coarse fine " },
        { "display-mode", " fullscreen standalone minimal-ui browser picture-in-picture window-controls-overlay borderless unframed tabbed " },
        { "color-gamut", " srgb p3 rec2020 " }, { "dynamic-range", " standard high " },
        { "video-dynamic-range", " standard high " }, { "update", " none slow fast " },
        { "overflow-block", " none scroll paged " }, { "overflow-inline", " none scroll " },
        { "scripting", " none initial-only enabled " }, { "scan", " interlace progressive " },
        { "display-state", " normal minimized maximized fullscreen " }, { "resizable", " true false " },
        { "navigation-controls", " none back-button " },
    };
    char key[72];
    snprintf(key, sizeof key, " %s ", v);
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
        if (!strcmp(name, values[i][0])) return strstr(values[i][1], key) != NULL;
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
            if (!mq_ident_allowed(n, v)) return -1;
            return strcmp(v, f.ident) == 0;
        }
        double v;
        if (!mq_value(&f, colon + 1, e, &v)) return -1;
        /* (a whole number's written as one) */
        if (f.type == MQ_NUM && strcmp(n, "device-pixel-ratio") && memchr(colon + 1, '.', (size_t)(e - colon - 1))) return -1;
        /* (out of the feature's range: not a value of it) */
        if ((f.type == MQ_RES || f.type == MQ_RATIO) && v < 0) return -1;
        /* (a whole number; a negative one is a value that matches nothing) */
        if (f.type == MQ_NUM && strcmp(n, "device-pixel-ratio") && v != (double)(long long)v) return -1;
        if (f.type == MQ_NUM && v < 0 && !strcmp(n, "grid")) return -1;
        if (!strcmp(n, "grid") && (prefix || (v != 0 && v != 1))) return -1;
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
    /* (the end of the query closes what's open) */
    if (close > m->end) close = m->end;
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
    m->p = close < m->end ? close + 1 : m->end;
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
    /* (a reserved word for the type: the query is invalid, not all, whatever not says) */
    static const char *const reserved[] = { "and", "not", "or", "only", "layer" };
    for (size_t k = 0; k < sizeof reserved / sizeof reserved[0]; k++)
        if (tl == strlen(reserved[k]) && !strncasecmp(t, reserved[k], tl)) return false;
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

static bool css_mem_has_host(const char *s, size_t n);
/* (the compound around a :host at k in sel [0, len): only :host, :host()
 * and :has() beside it, the host being featureless; false: it matches nothing) */
static bool css_host_compound_ok(const char *sel, size_t len, size_t k)
{
    /* (its start: back to a combinator, space, '(' or ',' outside brackets) */
    size_t a = k;
    int depth = 0;
    while (a > 0) {
        char c = sel[a - 1];
        if (c == ')') depth++;
        else if (c == '(') { if (!depth) break; depth--; }
        else if (!depth && (css_is_space(c) || c == ',' || c == '>' || c == '+' || c == '~')) break;
        a--;
    }
    size_t e = k;
    depth = 0;
    while (e < len) {
        char c = sel[e];
        if (c == '(') depth++;
        else if (c == ')') { if (!depth) break; depth--; }
        else if (!depth && (css_is_space(c) || c == ',' || c == '>' || c == '+' || c == '~')) break;
        e++;
    }
    for (size_t p = a; p < e;) {
        if (sel[p] != ':') return false;
        if (p + 1 < e && sel[p + 1] == ':') {
            /* (a pseudo-element of the host: fine) */
            size_t q = p + 2;
            while (q < e && sel[q] != ':' && sel[q] != '(') q++;
            if (q < e && sel[q] == '(') q = css_match_paren(sel, q, e) + 1;
            p = q;
            continue;
        }
        size_t q = p + 1;
        while (q < e && sel[q] != ':' && sel[q] != '(') q++;
        size_t nl = q - p - 1;
        bool host = nl == 4 && !strncasecmp(sel + p + 1, "host", 4), has = nl == 3 && !strncasecmp(sel + p + 1, "has", 3);
        /* (:not(), :is(), :where() of the host itself) */
        bool logic = (nl == 3 && !strncasecmp(sel + p + 1, "not", 3)) ||
                     (nl == 2 && !strncasecmp(sel + p + 1, "is", 2)) || (nl == 5 && !strncasecmp(sel + p + 1, "where", 5));
        if (logic && q < e && sel[q] == '(') {
            size_t c2 = css_match_paren(sel, q, e);
            if (!css_mem_has_host(sel + q, c2 - q)) return false;
            p = c2 + 1;
            continue;
        }
        if (!host && !has) return false;
        if (q < e && sel[q] == '(') q = css_match_paren(sel, q, e) + 1;
        else if (has) return false;
        p = q;
    }
    return true;
}

/* (whether [s, s + n) names :host, not :host-context) */
static bool css_mem_has_host(const char *s, size_t n)
{
    for (size_t k = 0; k + 5 <= n; k++)
        if (!strncasecmp(s + k, ":host", 5) && (k + 13 > n || strncasecmp(s + k, ":host-context", 13))) return true;
    return false;
}

/* Transform a single selector string for a given shadow host tag. */
static char* css_transform_one_selector(const char *sel, size_t len, const char *host_tag) {
    size_t i = 0;
    while (i < len && css_is_space(sel[i])) i++;
    size_t start = i;

    char *out = NULL;
    size_t out_len = 0, out_cap = 0;

    if (host_tag[0] == ':' && !strncasecmp(sel + i, ":host", 5) && strncasecmp(sel + i, ":host-context", 13) &&
        (i + 5 >= len || sel[i + 5] == '(' || sel[i + 5] == ':' || sel[i + 5] == '.' || sel[i + 5] == '[' ||
         sel[i + 5] == '#' || css_is_space(sel[i + 5]) || sel[i + 5] == '>' || sel[i + 5] == '+' || sel[i + 5] == '~')) {
        /* (a host by its node: :host(x) is x then the host's pseudo, x may name a type) */
        if (!css_host_compound_ok(sel, len, i)) return css_strdup("");
        if (sel[i + 5] == '(') {
            size_t open = i + 5;
            size_t close = css_match_paren(sel, open, len);
            if (close > open + 1) css_buf_append(&out, &out_len, &out_cap, sel + open + 1, close - open - 1);
            css_buf_append_cstr(&out, &out_len, &out_cap, host_tag);
            i = close + 1;
        } else {
            css_buf_append_cstr(&out, &out_len, &out_cap, host_tag);
            i += 5;
        }
        /* (the rest, any further :host in it the host's pseudo too) */
        for (size_t k = i; k < len;) {
            if (k + 5 <= len && !strncasecmp(sel + k, ":host", 5) && (k + 13 > len || strncasecmp(sel + k, ":host-context", 13))) {
                if (k + 5 < len && sel[k + 5] == '(') {
                    size_t close = css_match_paren(sel, k + 5, len);
                    css_buf_append_cstr(&out, &out_len, &out_cap, ":is(");
                    if (close > k + 6) css_buf_append(&out, &out_len, &out_cap, sel + k + 6, close - k - 6);
                    css_buf_append_cstr(&out, &out_len, &out_cap, ")");
                    css_buf_append_cstr(&out, &out_len, &out_cap, host_tag);
                    k = close + 1;
                } else {
                    css_buf_append_cstr(&out, &out_len, &out_cap, host_tag);
                    k += 5;
                }
                continue;
            }
            css_buf_append(&out, &out_len, &out_cap, sel + k, 1);
            k++;
        }
    } else if (i + 6 <= len && strncasecmp(sel + i, ":host(", 6) == 0) {
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
        size_t open = i + 9;
        if (open >= len || sel[open] != '(') { free(out); return css_strdup(""); }
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
    } else if (host_tag[0] == ':' && i + 14 <= len && strncasecmp(sel + i, ":host-context(", 14) == 0) {
        /* (:host-context(x): the host when it or an ancestor is x) */
        size_t open = i + 13, close = css_match_paren(sel, open, len);
        css_buf_append_cstr(&out, &out_len, &out_cap, ":is(:is(");
        css_buf_append(&out, &out_len, &out_cap, sel + open + 1, close > open ? close - open - 1 : 0);
        css_buf_append_cstr(&out, &out_len, &out_cap, ") ");
        css_buf_append_cstr(&out, &out_len, &out_cap, host_tag);
        css_buf_append_cstr(&out, &out_len, &out_cap, ", :is(");
        css_buf_append(&out, &out_len, &out_cap, sel + open + 1, close > open ? close - open - 1 : 0);
        css_buf_append_cstr(&out, &out_len, &out_cap, ")");
        css_buf_append_cstr(&out, &out_len, &out_cap, host_tag);
        css_buf_append_cstr(&out, &out_len, &out_cap, ")");
        i = close + 1;
        css_buf_append(&out, &out_len, &out_cap, sel + i, len - i);
    } else if (i + 14 <= len && strncasecmp(sel + i, ":host-context(", 14) == 0) {
        /* Drop :host-context rules; we cannot model outer ancestors. */
        free(out);
        return css_strdup("");
    } else if (host_tag[0] == ':' && css_mem_has_host(sel + i, len - i)) {
        /* (:host inside :is(), :not() and the like: the host's pseudo put there, no scoping prefix) */
        for (size_t k = i; k < len;) {
            if (k + 5 <= len && !strncasecmp(sel + k, ":host", 5) && (k + 13 > len || strncasecmp(sel + k, ":host-context", 13))) {
                if (!css_host_compound_ok(sel, len, k)) { free(out); return css_strdup(""); }
                if (k + 5 < len && sel[k + 5] == '(') {
                    size_t close = css_match_paren(sel, k + 5, len);
                    css_buf_append_cstr(&out, &out_len, &out_cap, ":is(");
                    if (close > k + 6) css_buf_append(&out, &out_len, &out_cap, sel + k + 6, close - k - 6);
                    css_buf_append_cstr(&out, &out_len, &out_cap, ")");
                    css_buf_append_cstr(&out, &out_len, &out_cap, host_tag);
                    k = close + 1;
                } else {
                    css_buf_append_cstr(&out, &out_len, &out_cap, host_tag);
                    k += 5;
                }
                continue;
            }
            css_buf_append(&out, &out_len, &out_cap, sel + k, 1);
            k++;
        }
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

/* ::part(a b): in the flattened tree a shadow tree's elements are its host's
 * descendants, so a part is a descendant whose part attribute has each name. */
void css_rewrite_parts(CssStylesheet *sheet) {
    if (!sheet) return;
    for (int r = 0; r < sheet->rule_count; r++) {
        CssRule *rule = &sheet->rules[r];
        const char *s = rule->selector_text;
        if (!s || !strstr(s, "::part(")) continue;
        char *out = NULL;
        size_t out_len = 0, out_cap = 0, len = strlen(s), i = 0;
        while (i < len) {
            if (i + 7 <= len && !strncmp(s + i, "::part(", 7)) {
                size_t open = i + 6, close = css_match_paren(s, open, len);
                css_buf_append_cstr(&out, &out_len, &out_cap, " ");
                size_t k = open + 1;
                bool any = false;
                while (k < close) {
                    while (k < close && css_is_space(s[k])) k++;
                    size_t st = k;
                    while (k < close && !css_is_space(s[k])) k++;
                    if (k > st) {
                        css_buf_append_cstr(&out, &out_len, &out_cap, "[part~=\"");
                        css_buf_append(&out, &out_len, &out_cap, s + st, k - st);
                        css_buf_append_cstr(&out, &out_len, &out_cap, "\"]");
                        any = true;
                    }
                }
                /* (no name: nothing it can be) */
                if (!any) css_buf_append_cstr(&out, &out_len, &out_cap, ":not(*)");
                i = close < len ? close + 1 : len;
                continue;
            }
            css_buf_append(&out, &out_len, &out_cap, s + i, 1);
            i++;
        }
        if (!out) continue;
        free(rule->selector_text);
        rule->selector_text = out;
        rule->specificity = 0;
        rule->scoped = 1;
        css_rule_compiled_free(rule);
    }
}

/* (whether a selector of the list has the host as its subject: :host and
 * nothing after its compound; an element in the tree is no outer-context rule) */
static bool css_host_subject(const char *sel)
{
    for (const char *p = sel; *p;) {
        while (css_is_space(*p) || *p == ',') p++;
        int depth = 0;
        const char *e = p;
        while (*e && (depth || *e != ',')) { if (*e == '(') depth++; else if (*e == ')') depth--; e++; }
        const char *t = e;
        while (t > p && css_is_space(t[-1])) t--;
        bool comb = false;
        if (!strncasecmp(p, ":host", 5) && strncasecmp(p, ":host-context", 13)) {
            depth = 0;
            for (const char *q = p; q < t; q++) {
                if (*q == '(') depth++;
                else if (*q == ')') depth--;
                else if (!depth && (css_is_space(*q) || *q == '>' || *q == '+' || *q == '~')) { comb = true; break; }
            }
            if (!comb) return true;
        }
        /* (::slotted(): light-tree elements, the outer context too) */
        for (const char *q = p; q + 9 <= t; q++) if (!strncasecmp(q, "::slotted", 9)) return true;
        p = e;
    }
    return false;
}

void css_scope_stylesheet(CssStylesheet *sheet, const char *host_tag) {
    if (!sheet || !host_tag) return;
    for (int r = 0; r < sheet->rule_count; r++) {
        CssRule *rule = &sheet->rules[r];
        if (!rule->selector_text) continue;
        /* (a rule for the host: the shadow tree's context in the cascade, the
         * rest match only in the tree, the page's rules there as well) */
        rule->shadow = css_host_subject(rule->selector_text);
        rule->scoped = host_tag[0] == ':';
        char *new_sel = css_transform_selectors_for_host(rule->selector_text, host_tag);
        free(rule->selector_text);
        rule->selector_text = new_sel;
        rule->specificity = 0;
        /* The selector text changed, so any cached parse is now stale. */
        css_rule_compiled_free(rule);
    }
}




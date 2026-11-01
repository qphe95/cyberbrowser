#ifndef CSS_TOKENIZER_H
#define CSS_TOKENIZER_H

/* The CSS Syntax Level 3 tokenizer (section 4): a stylesheet's UTF-8 text as
 * tokens, escapes resolved, comments dropped (CSS_PARSER_DESIGN.md). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CT_EOF,
    CT_IDENT,
    CT_FUNCTION,        /* name( : value is the name */
    CT_AT_KEYWORD,      /* @name : value is the name */
    CT_HASH,            /* #name : value is the name; CT_F_ID when it is an identifier */
    CT_STRING,          /* value without the quotes */
    CT_BAD_STRING,
    CT_URL,             /* url(...) unquoted: value is the URL */
    CT_BAD_URL,
    CT_DELIM,           /* delim: the code point */
    CT_NUMBER,          /* num; CT_F_INTEGER */
    CT_PERCENTAGE,      /* num */
    CT_DIMENSION,       /* num, value is the unit */
    CT_WHITESPACE,
    CT_CDO,             /* <!-- */
    CT_CDC,             /* --> */
    CT_COLON,
    CT_SEMICOLON,
    CT_COMMA,
    CT_LBRACKET,
    CT_RBRACKET,
    CT_LPAREN,
    CT_RPAREN,
    CT_LBRACE,
    CT_RBRACE,
} CssTokType;

#define CT_F_ID       1   /* a hash token whose name is an identifier */
#define CT_F_INTEGER  2   /* a number written without a fraction or exponent */
#define CT_F_ARENA    4   /* the value is in the tokens' arena (it had escapes), else in the source */

typedef struct {
    uint8_t type;       /* CssTokType */
    uint8_t flags;
    uint32_t start, end;        /* its text in the source: [start, end) */
    uint32_t val, val_len;      /* its value: offset into the source or (CT_F_ARENA) the arena */
    uint32_t delim;             /* CT_DELIM: the code point */
    double num;                 /* CT_NUMBER, CT_PERCENTAGE, CT_DIMENSION */
} CssToken;

typedef struct {
    CssToken *t;
    int n, cap;
    char *arena;                /* values with escapes resolved (UTF-8) */
    size_t an, acap;
    const char *src;            /* the text tokenized (not owned) */
} CssTokens;

/* Tokenize s (n bytes, UTF-8) into out (appending; zero it first).  The
 * last token is CT_EOF.  False when out of memory. */
bool css_tokenize(const char *s, size_t n, CssTokens *out);
void css_tokens_free(CssTokens *ts);

/* A token's value (not NUL-terminated: len bytes). */
static inline const char *css_tok_val(const CssTokens *ts, const CssToken *t, size_t *len)
{
    *len = t->val_len;
    return (t->flags & CT_F_ARENA ? ts->arena : ts->src) + t->val;
}

#endif

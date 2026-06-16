/*
 * Fast token dispatch table for QuickJS
 * 
 * Uses a 256-entry lookup table for O(1) token type identification.
 * This avoids the switch statement overhead in the hot path.
 */
#ifndef JS_FAST_DISPATCH_H
#define JS_FAST_DISPATCH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Token type categories for fast dispatch */
typedef enum {
    TOK_CAT_WHITESPACE,    /* Space, tab, etc. - skip */
    TOK_CAT_NEWLINE,       /* \n, \r - track line */
    TOK_CAT_EOF,          /* End of file */
    TOK_CAT_IDENTIFIER,   /* a-z, A-Z, _, $ */
    TOK_CAT_NUMBER,       /* 0-9 */
    TOK_CAT_STRING,       /* ' " */
    TOK_CAT_TEMPLATE,     /* ` */
    TOK_CAT_SLASH,        /* / - comment or divide */
    TOK_CAT_PUNCT,        /* Single-char punctuators: ; , ( ) [ ] { } */
    TOK_CAT_OPERATOR,     /* Operators: + - * % < > = ! & | ^ ~ ? : . */
    TOK_CAT_INVALID,      /* Invalid chars */
    TOK_CAT_COUNT
} JSTokenCategory;

/* Extended info for multi-char operators */
typedef struct JSTokenDispatchEntry {
    uint8_t category;      /* JSTokenCategory */
    uint8_t single_token;  /* If non-zero, this is the token value */
    uint8_t need_second;   /* If non-zero, need to check next char */
    uint8_t reserved;
} JSTokenDispatchEntry;

/* Global dispatch table - initialized at startup */
extern JSTokenDispatchEntry js_token_dispatch[256];

/* Initialize dispatch table */
void js_token_dispatch_init(void);

/* Fast category lookup */
static inline JSTokenCategory js_token_get_category(uint8_t c) {
    return (JSTokenCategory)js_token_dispatch[c].category;
}

/* Check if char can start an identifier */
static inline bool js_token_is_ident_start(uint8_t c) {
    return (c >= 'a' && c <= 'z') || 
           (c >= 'A' && c <= 'Z') || 
           c == '_' || c == '$';
}

/* Check if char can continue an identifier */
static inline bool js_token_is_ident_continue(uint8_t c) {
    return js_token_is_ident_start(c) || (c >= '0' && c <= '9');
}

/* Fast skip whitespace */
const uint8_t *js_fast_skip_whitespace(const uint8_t *p, const uint8_t *end, 
                                        bool *got_lf);

/* Fast identifier scanning */
int js_fast_scan_identifier(const uint8_t *p, const uint8_t *end, 
                            const uint8_t **endptr);

#ifdef __cplusplus
}
#endif

#endif /* JS_FAST_DISPATCH_H */

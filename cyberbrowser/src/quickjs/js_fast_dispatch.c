/*
 * Fast token dispatch implementation
 */
#include "js_fast_dispatch.h"
#include <string.h>

/* Global dispatch table */
JSTokenDispatchEntry js_token_dispatch[256];

void js_token_dispatch_init(void) {
    /* Clear table */
    memset(js_token_dispatch, 0, sizeof(js_token_dispatch));
    
    /* Whitespace */
    js_token_dispatch[' '].category = TOK_CAT_WHITESPACE;
    js_token_dispatch['\t'].category = TOK_CAT_WHITESPACE;
    js_token_dispatch['\f'].category = TOK_CAT_WHITESPACE;
    js_token_dispatch['\v'].category = TOK_CAT_WHITESPACE;
    
    /* Newlines */
    js_token_dispatch['\n'].category = TOK_CAT_NEWLINE;
    js_token_dispatch['\r'].category = TOK_CAT_NEWLINE;
    
    /* EOF */
    js_token_dispatch[0].category = TOK_CAT_EOF;
    
    /* Identifiers */
    for (int c = 'a'; c <= 'z'; c++) {
        js_token_dispatch[c].category = TOK_CAT_IDENTIFIER;
    }
    for (int c = 'A'; c <= 'Z'; c++) {
        js_token_dispatch[c].category = TOK_CAT_IDENTIFIER;
    }
    js_token_dispatch['_'].category = TOK_CAT_IDENTIFIER;
    js_token_dispatch['$'].category = TOK_CAT_IDENTIFIER;
    
    /* Numbers */
    for (int c = '0'; c <= '9'; c++) {
        js_token_dispatch[c].category = TOK_CAT_NUMBER;
    }
    
    /* Strings */
    js_token_dispatch['\''].category = TOK_CAT_STRING;
    js_token_dispatch['"'].category = TOK_CAT_STRING;
    
    /* Template */
    js_token_dispatch['`'].category = TOK_CAT_TEMPLATE;
    
    /* Slash (comment or divide) */
    js_token_dispatch['/'].category = TOK_CAT_SLASH;
    
    /* Single-char punctuators */
    js_token_dispatch[';'].category = TOK_CAT_PUNCT;
    js_token_dispatch[','].category = TOK_CAT_PUNCT;
    js_token_dispatch['('].category = TOK_CAT_PUNCT;
    js_token_dispatch[')'].category = TOK_CAT_PUNCT;
    js_token_dispatch['['].category = TOK_CAT_PUNCT;
    js_token_dispatch[']'].category = TOK_CAT_PUNCT;
    js_token_dispatch['{'].category = TOK_CAT_PUNCT;
    js_token_dispatch['}'].category = TOK_CAT_PUNCT;
    
    /* Operators */
    js_token_dispatch['+'].category = TOK_CAT_OPERATOR;
    js_token_dispatch['-'].category = TOK_CAT_OPERATOR;
    js_token_dispatch['*'].category = TOK_CAT_OPERATOR;
    js_token_dispatch['%'].category = TOK_CAT_OPERATOR;
    js_token_dispatch['<'].category = TOK_CAT_OPERATOR;
    js_token_dispatch['>'].category = TOK_CAT_OPERATOR;
    js_token_dispatch['='].category = TOK_CAT_OPERATOR;
    js_token_dispatch['!'].category = TOK_CAT_OPERATOR;
    js_token_dispatch['&'].category = TOK_CAT_OPERATOR;
    js_token_dispatch['|'].category = TOK_CAT_OPERATOR;
    js_token_dispatch['^'].category = TOK_CAT_OPERATOR;
    js_token_dispatch['~'].category = TOK_CAT_OPERATOR;
    js_token_dispatch['?'].category = TOK_CAT_OPERATOR;
    js_token_dispatch[':'].category = TOK_CAT_OPERATOR;
    js_token_dispatch['.'].category = TOK_CAT_OPERATOR;
}

const uint8_t *js_fast_skip_whitespace(const uint8_t *p, const uint8_t *end,
                                        bool *got_lf) {
    while (p < end) {
        uint8_t c = *p;
        JSTokenCategory cat = js_token_get_category(c);
        
        if (cat == TOK_CAT_WHITESPACE) {
            p++;
        } else if (cat == TOK_CAT_NEWLINE) {
            *got_lf = true;
            p++;
        } else {
            break;
        }
    }
    return p;
}

int js_fast_scan_identifier(const uint8_t *p, const uint8_t *end,
                            const uint8_t **endptr) {
    const uint8_t *start = p;
    
    if (p >= end) return 0;
    
    /* First char must be identifier start */
    if (!js_token_is_ident_start(*p)) {
        return 0;
    }
    p++;
    
    /* Subsequent chars can be identifier continue */
    while (p < end && js_token_is_ident_continue(*p)) {
        p++;
    }
    
    *endptr = p;
    return p - start;
}

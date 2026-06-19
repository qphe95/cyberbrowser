/*
 * CSS Parser - Lightweight stylesheet/inline-style parser for the DOM.
 *
 * Supports a practical subset of CSS:
 *   - rule sets with selectors (tag, .class, #id, *, descendant, child)
 *   - declarations property: value;
 *   - inline style="..." attributes
 *   - <style> element text and external <link rel="stylesheet"> href fetching
 */

#ifndef CSS_PARSER_H
#define CSS_PARSER_H

#include <stddef.h>
#include <stdbool.h>
#include "html_dom.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One property/value pair. */
typedef struct CssDeclaration {
    char *property;
    char *value;
} CssDeclaration;

/* One rule: a selector string plus its declarations. */
typedef struct CssRule {
    char *selector_text;
    int specificity;
    CssDeclaration *declarations;
    int declaration_count;
    int declaration_capacity;
} CssRule;

/* A parsed stylesheet. */
typedef struct CssStylesheet {
    CssRule *rules;
    int rule_count;
    int rule_capacity;
} CssStylesheet;

/* Parse a full stylesheet. Returns NULL on empty/invalid input. */
CssStylesheet* css_stylesheet_parse(const char *css, size_t len);

/* Free a stylesheet and all owned strings. */
void css_stylesheet_free(CssStylesheet *sheet);

/* Parse an inline style attribute. Returns an array and fills *out_count.
 * Free with css_declarations_free. */
CssDeclaration* css_parse_inline_style(const char *style_attr, int *out_count);

/* Free an inline-style declaration array. */
void css_declarations_free(CssDeclaration *decls, int count);

/* Convert a CSS property like "background-color" to "backgroundColor". */
char* css_to_camel_case(const char *prop);

/*
 * Walk the parsed HTML document, collect inline <style> sheets and external
 * stylesheets, fetch the external ones, then apply matching rules to every
 * element's JS style object.  base_url may be NULL; relative URLs fall back to
 * https://www.youtube.com/ .
 */
void css_apply_document_styles(JSContextHandle ctx, GCValue js_doc,
                               HtmlDocument *doc, const char *base_url);

#ifdef __cplusplus
}
#endif

#endif /* CSS_PARSER_H */

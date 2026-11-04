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

/* Features the rendering tick looks after (layout_render_tick).  A declaration
 * of one carries its bit, the cascade puts it on the box's style, and the end of
 * layout lists the elements that have any (css_layout_render_list): what the
 * tick costs goes by how many elements use them, not by the page's size. */
enum { CSS_FEAT_SNAP = 1, CSS_FEAT_CONTENT_VIS = 2, CSS_FEAT_INITIAL_TARGET = 4, CSS_FEAT_MEDIA_LISTENERS = 8,
       CSS_FEAT_SNAP_ALIGN = 16, CSS_FEAT_MARKER_GROUP = 32 };
extern volatile int g_css_render_features;
/* The physical longhand of a logical one, in a box of that writing mode
 * (0 horizontal-tb, 1 vertical-rl, 2 vertical-lr, 3 sideways-rl, 4 sideways-lr)
 * and direction; false when prop isn't logical. */
bool css_logical_physical(const char *prop, int wm, bool rtl, char *out, size_t cap);
/* A logical shorthand (margin-inline, inset-block, border-block-width...):
 * its two longhands, start then end; false when prop isn't one. */
bool css_logical_shorthand(const char *prop, char start[64], char end[64]);
/* The feature bit of a property, noted as seen in this document. */
unsigned char css_note_property(const char *prop);
/* A page's <meta name=color-scheme> asking for one: prefers-color-scheme says
 * so ("light", "dark", static strings; NULL: as usual).  True if it changed. */
bool css_media_set_color_scheme(const char *scheme);

/* One property/value pair. */
typedef struct CssDeclaration {
    char *property;
    char *value;
    bool important;
    unsigned char feature;  /* CSS_FEAT_* this property is (css_note_property), or 0 */
    unsigned char invalid;  /* layout's cache of css_value_invalid: 0 not known yet, 1 valid, 2 invalid */
} CssDeclaration;

/* Forward declaration of the lazily-compiled selector representation. */
typedef struct CssCompiledSelector CssCompiledSelector;

/* One rule: a selector string plus its declarations. */
typedef struct CssRule {
    char *selector_text;
    char *media_query;   /* enclosing @media condition, e.g. "(min-width:792px)" */
    int specificity;
    CssDeclaration *declarations;
    int declaration_count;
    int declaration_capacity;
    /* Lazily-parsed selector, cached so the matcher does not re-parse the
     * selector text for every DOM node it is tested against. NULL until first
     * used (or if compilation yielded no usable selector). */
    CssCompiledSelector *compiled;
} CssRule;

/* A parsed stylesheet. */
typedef struct CssStylesheet {
    CssRule *rules;
    int rule_count;
    int rule_capacity;
    bool user_agent;    /* the user agent's: below every author rule in the cascade */
    /* @keyframes rules (CSS Animations 1 section 3): each a name and a run
     * of frames (frames[first .. first + count), in offset order).  A frame
     * listed under several selectors ("0%, 100% {}") is one frame per
     * offset; the first of them owns the declarations. */
    struct CssKeyframes *kf;
    int kf_n, kf_cap;
    struct CssKeyframe *frames;
    int frame_n, frame_cap;
} CssStylesheet;

typedef struct CssKeyframe {
    float offset;               /* 0 .. 1 */
    bool owns;                  /* frees decls */
    CssDeclaration *decls;
    int n;
} CssKeyframe;

typedef struct CssKeyframes {
    char *name;
    int first, count;
} CssKeyframes;

/* The last @keyframes named name in sheet (NULL: none). */
const CssKeyframes *css_keyframes_find(const CssStylesheet *sheet, const char *name);
/* Move src's @keyframes to the end of dst's (src keeps none). */
void css_keyframes_take(CssStylesheet *dst, CssStylesheet *src);

/* Parse a full stylesheet. Returns NULL on empty/invalid input. */
CssStylesheet* css_stylesheet_parse(const char *css, size_t len);

/* Parse the top-level items of css that start in [start, stop); see the
 * comment in css_parser.c.  Returns a sheet (possibly empty) and the stop
 * position in *end_pos.  Used for chunked parallel parsing. */
CssStylesheet* css_stylesheet_parse_range(const char *css, size_t len,
                                          size_t start, size_t stop, size_t *end_pos);

/* The top-level item of css at pos (after white space and comments): where
 * it ends, and in *counts whether it is a valid style rule or an at-rule
 * some browser knows (an @import after one is ignored). */
size_t css_sheet_item_skip(const char *css, size_t len, size_t pos, bool *counts);

/* Free a stylesheet and all owned strings. */
void css_stylesheet_free(CssStylesheet *sheet);

/* Parse an inline style attribute. Returns an array and fills *out_count.
 * Free with css_declarations_free. */
CssDeclaration* css_parse_inline_style(const char *style_attr, int *out_count);

/* Free an inline-style declaration array. */
void css_declarations_free(CssDeclaration *decls, int count);

/* Convert a CSS property like "background-color" to "backgroundColor". */
char* css_to_camel_case(const char *prop);

/* Selector matching: returns true if the selector matches the given DOM node. */
bool css_selector_matches(const char *selector, HtmlDocument *doc, HtmlNode *node);
/* The DOM_ST_* of a pseudo-class that is a state of the element (0: not one). */
uint32_t css_state_bit(const char *name, size_t nl);

/* Match a rule's selector against a node. The selector is parsed once and
 * cached on the rule (rule->compiled), so repeated calls over many nodes do
 * not re-parse the selector text each time. */
bool css_rule_matches(const CssRule *rule, HtmlDocument *doc, HtmlNode *node);

/* Free a rule's cached compiled selector (set rule->compiled to NULL). */
void css_rule_compiled_free(CssRule *rule);

/* qsort comparator for CssAppliedDecl (specificity ascending, then order). */
int css_applied_decl_compare(const void *a, const void *b);

/* Compute specificity from raw selector text. */
int css_specificity_from_selector_text(const char *selector);

/* Specificity of the most specific selector-list branch matching this node. */
int css_specificity_from_selector_text_matching(const char *selector,
                                                HtmlDocument *doc,
                                                HtmlNode *node);

/* Returns true if a rule's enclosing @media condition matches the viewport. */
bool css_rule_media_matches(const CssRule *rule, double viewport_width, double viewport_height);

/* Joins the query lists of nested @media blocks in CssRule.media_query:
 * the rule applies when every list matches. */
#define CSS_MEDIA_AND '\x1f'

/* Whether a media query list (Media Queries 4: types, and/or/not, plain and
 * range features) matches a screen of this viewport (CSS px).  The screen
 * is dark-scheme (as matchMedia has always said), 1dppx, with a fine
 * pointer that hovers. */
bool css_media_query_matches(const char *query, double viewport_width, double viewport_height);

/* Transform a stylesheet's selectors for a shadow host (e.g. :host, ::slotted).
 * Used by both the JS CSS applier and the layout-engine collector. */
void css_scope_stylesheet(CssStylesheet *sheet, const char *host_tag);

/* One matched declaration with specificity/order for cascading. */
typedef struct CssAppliedDecl {
    const CssDeclaration *decl;
    int specificity;
    int order;
    bool important;
    int rule;        /* global rule index in the compiled style set (-1 if none) */
} CssAppliedDecl;

/* Parse an inline style attribute string and apply its declarations to the
 * element's JS style object, creating the style object if necessary. */
void css_apply_inline_style_string(JSContextHandle ctx, GCValue element,
                                   const char *style_attr);

/* Return the element's CSSStyleDeclaration object, creating it if necessary. */
GCValue css_ensure_style_object(JSContextHandle ctx, GCValue element);
/* A new element style object (js_quickjs.c) and inline declarations applied
 * to a style object. */
GCValue css_new_style_object(JSContextHandle ctx, GCValue element);
void css_apply_inline_style_to(JSContextHandle ctx, GCValue style, const char *style_attr);

/* Set a CSS property on a CSSStyleDeclaration object (kebab-case + camelCase). */
void css_set_style_property(JSContextHandle ctx, GCValue style, const char *prop, const char *value);

#ifdef __cplusplus
}
#endif

#endif /* CSS_PARSER_H */

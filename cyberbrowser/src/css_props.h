/*
 * css_props.h - the CSS property table: each property's name, whether it
 * inherits, its initial value and its value grammar in the value definition
 * syntax of CSS Values 4 (keywords, <types>, <'properties'>, functions,
 * juxtaposition, && || |, and the ? * + # {m,n} ! multipliers).
 *
 * A value is checked against its property's grammar and serialized in the
 * canonical form of CSSOM ("serialize a CSS value"): keywords lower-cased,
 * numbers in shortest form, sRGB colors as rgb()/rgba(), || and && groups in
 * the grammar's order.  Shorthands expand into their longhands and are
 * serialized back from them.
 */
#ifndef CSS_PROPS_H
#define CSS_PROPS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How a shorthand maps onto its longhands. */
enum {
    CSS_SH_NONE = 0,    /* a longhand */
    CSS_SH_GRAMMAR,     /* each longhand is a <'longhand'> of the grammar; missing ones are initial */
    CSS_SH_BOX,         /* 1-4 values: top right bottom left */
    CSS_SH_PAIR,        /* 1-2 values: first second (the second defaults to the first) */
    CSS_SH_SPECIAL,     /* css_props.c knows it */
};

typedef struct CssPropDef {
    const char *name;       /* lower case, e.g. "background-color" */
    const char *grammar;    /* value definition syntax */
    const char *initial;    /* the initial value, as it serializes */
    const char *longhands;  /* a shorthand's longhands, space-separated (NULL for a longhand) */
    unsigned char shorthand;    /* CSS_SH_* */
    unsigned char inherited;
    unsigned char list;         /* a comma-separated list property (background-image, transition-duration, ...) */
} CssPropDef;

/* The table (longhands and shorthands, in alphabetical order). */
int css_prop_count(void);
const CssPropDef *css_prop_at(int i);
/* By name (ASCII case-insensitive; vendor aliases such as -webkit-box-shadow
 * map to the standard property).  NULL when unknown. */
const CssPropDef *css_prop_find(const char *name, size_t len);
/* By IDL attribute name: camelCase (backgroundColor), webkitCased
 * (webkitTransform), cssFloat, or the dashed name.  NULL when none. */
const CssPropDef *css_prop_find_idl(const char *name, size_t len);
/* The legacy / vendor aliases: name i (NULL past the end), and its property. */
const char *css_prop_alias_at(int i, const CssPropDef **prop);
/* The camelCase IDL attribute of a property name (malloc'd). */
char *css_prop_camel(const char *name);
/* A custom property name (--x)? */
bool css_prop_is_custom(const char *name, size_t len);

/* The CSS-wide keywords (initial, inherit, unset, revert, revert-layer). */
bool css_is_wide_keyword(const char *value, size_t len);

/* Parse value for property p: its canonical serialization (malloc'd), or
 * NULL when the value is not valid for p.  A value with var() is kept as
 * written (trimmed) since it is checked only when substituted.  For a
 * shorthand this checks the value and serializes it as a whole. */
char *css_prop_parse(const CssPropDef *p, const char *value);

/* Expand a shorthand's value into its longhands: out[i] (malloc'd) is the
 * value of the i-th longhand in p->longhands order.  Returns the longhand
 * count, or -1 when the value is invalid.  A CSS-wide keyword or var() gives
 * every longhand that value (var(): the pending-substitution value, ""). */
int css_prop_expand(const CssPropDef *p, const char *value, char ***out);
/* The longhands of a shorthand (space-separated names split): count, and
 * names[i] pointers into a static copy. */
int css_prop_longhands(const CssPropDef *p, const char ***names);
/* The shorthands that have longhand p (in table order): their count, and
 * *out pointing at a static list. */
int css_prop_shorthands_of(const CssPropDef *p, const CssPropDef *const **out);
/* A shorthand's value from its longhands' values (vals[i] for the i-th
 * longhand; NULL = not set): malloc'd, or NULL ("") when it cannot be
 * represented (a longhand missing, mixed CSS-wide keywords, ...). */
char *css_prop_shorthand_serialize(const CssPropDef *p, const char *const *vals);

/* What computed values depend on (CSS Values 4 computed values). */
typedef struct CssComputeEnv {
    double font_size;       /* px: em (for font-size itself, the parent's) */
    double root_font_size;  /* px: rem */
    double vw, vh;          /* the viewport, px */
    double color[4];        /* currentcolor (0-255, alpha 0-1) */
    double ch_ratio, ex_ratio;  /* ch and ex over em, of the font (0: not known) */
    double cq_w, cq_h;          /* the query container's content box, px (0: none, the viewport's) */
} CssComputeEnv;

/* The computed value of a valid specified value of longhand p (as
 * css_prop_parse gives it): lengths in px, colors as rgb()/rgba(),
 * currentcolor resolved, times in s, angles in deg, calc() resolved as far
 * as it can be.  Keyword-specific rules (font-weight, border widths, ...)
 * are the caller's.  Malloc'd; NULL when the value does not parse. */
char *css_prop_compute(const CssPropDef *p, const char *value, const CssComputeEnv *env);

/* CSS.supports(property, value). */
bool css_supports_property(const char *prop, const char *value);
/* A value layout should ignore: outside the grammar of a property checked
 * here (colors, backgrounds, box sizes and offsets, font-size, line-height,
 * margins, paddings).  Other properties, custom ones, wide keywords and
 * values with var() are never. */
bool css_value_invalid(const char *prop, const char *value);
/* CSS.supports(conditionText) / @supports conditions. */
bool css_supports_condition(const char *cond);

/* A color value's sRGB components when it is one (0-255 and 0-1 alpha);
 * false for currentcolor, system colors and colors outside sRGB syntax. */
bool css_color_parse(const char *value, double rgba[4]);
/* "rgb(r, g, b)" or "rgba(r, g, b, a)" (malloc'd). */
char *css_color_serialize_rgba(const double rgba[4]);

/* Registered custom properties. */
int css_reg_count(void);
uint64_t css_reg_signature(void);
const char *css_reg_name(int r);
const char *css_reg_syntax(int r);
const char *css_reg_initial(int r);
bool css_reg_inherits(int r);
int css_reg_find(const char *name);
bool css_reg_scripted(const char *name);
bool css_reg_add(const char *name, const char *syntax, bool inherits, const char *initial, bool rule);
void css_reg_forget_rules(void);
void css_reg_reset(void);
void css_reg_scan_css(const char *css, size_t len);
bool css_syntax_valid(const char *syntax);
char *css_syntax_parse(const char *syntax, const char *value);
bool css_syntax_component(const char *syntax, const char *value, char *type, size_t cap, char *mult);
bool css_value_depends_on_context(const char *v);
bool css_calc_number(const char *v, double *out);
/* clip-path's or shape-outside's value in its computed form; NULL if it already is. */
char *css_shape_computed(const char *n, const char *v);
/* An image list's radial and conic gradients in their computed form; NULL if already. */
char *css_gradient_computed(const char *v);
extern __thread bool g_css_shape_path_normal;   /* (and path() data as absolute commands) */

#ifdef __cplusplus
}
#endif

#endif

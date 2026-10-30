/*
 * Internal selector representation shared by the CSS parser and the compiled
 * style matcher (css_style.c).  Not part of the public API.
 */
#ifndef CSS_SELECTOR_INTERNAL_H
#define CSS_SELECTOR_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#define CSS_MAX_SIMPLE_PARTS 16
#define CSS_MAX_CLASSES 16
#define CSS_MAX_ATTRS 8

/* Attribute selector operators. */
#define CSS_ATTR_OP_EXISTS   0  /* [a]      */
#define CSS_ATTR_OP_EQ       1  /* [a=v]    */
#define CSS_ATTR_OP_WORD     2  /* [a~=v]   */
#define CSS_ATTR_OP_DASH     3  /* [a|=v]   */
#define CSS_ATTR_OP_PREFIX   4  /* [a^=v]   */
#define CSS_ATTR_OP_SUFFIX   5  /* [a$=v]   */
#define CSS_ATTR_OP_SUBSTR   6  /* [a*=v]   */

#include <string.h>
#include <strings.h>

/* HTML's attributes whose values selectors compare ASCII case-insensitively
 * unless the selector says `s` (HTML "Selectors", case-sensitivity). */
static inline bool css_attr_legacy_ci(const char *name, size_t n) {
    static const char *const names[] = {
        "accept", "accept-charset", "align", "alink", "axis", "bgcolor", "charset", "checked",
        "clear", "codetype", "color", "compact", "declare", "defer", "dir", "direction",
        "disabled", "enctype", "face", "frame", "hreflang", "http-equiv", "lang", "language",
        "link", "media", "method", "multiple", "nohref", "noresize", "noshade", "nowrap",
        "readonly", "rel", "rev", "rules", "scope", "scrolling", "selected", "shape", "target",
        "text", "type", "valign", "valuetype", "vlink",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strlen(names[i]) == n && strncasecmp(names[i], name, n) == 0) return true;
    return false;
}

/* Does attribute value `have` (NULL = attribute absent) satisfy the test? */
static inline bool css_attr_value_matches(int op, bool ci, const char *want,
                                          const char *have) {
    if (!have) return false;
    if (op == CSS_ATTR_OP_EXISTS) return true;
    size_t wl = strlen(want), hl = strlen(have);
    int (*ncmp)(const char *, const char *, size_t) = ci ? strncasecmp : strncmp;
    switch (op) {
    case CSS_ATTR_OP_EQ:
        return wl == hl && ncmp(have, want, wl) == 0;
    case CSS_ATTR_OP_DASH:
        return hl >= wl && ncmp(have, want, wl) == 0 &&
               (hl == wl || have[wl] == '-');
    case CSS_ATTR_OP_PREFIX:
        return wl > 0 && hl >= wl && ncmp(have, want, wl) == 0;
    case CSS_ATTR_OP_SUFFIX:
        return wl > 0 && hl >= wl && ncmp(have + hl - wl, want, wl) == 0;
    case CSS_ATTR_OP_SUBSTR:
        if (wl == 0) return false;
        for (size_t i = 0; i + wl <= hl; i++)
            if (ncmp(have + i, want, wl) == 0) return true;
        return false;
    case CSS_ATTR_OP_WORD: {
        if (wl == 0) return false;
        for (size_t i = 0; i < wl; i++)
            if (want[i] == ' ' || want[i] == '\t' || want[i] == '\n') return false;
        const char *p = have;
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f') p++;
            const char *st = p;
            while (*p && !(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f')) p++;
            if ((size_t)(p - st) == wl && ncmp(st, want, wl) == 0) return true;
        }
        return false;
    }
    default:
        return true;
    }
}

typedef struct CssSimpleSelector {
    char tag[64];
    char id[128];
    char classes[CSS_MAX_CLASSES][64];
    int class_count;
    char attrs[CSS_MAX_ATTRS][64];  /* attribute names that must be present */
    int attr_count;
    /* Per-attribute value test: op is a CSS_ATTR_OP_* value; the value is
     * stored verbatim (case is folded at match time when attr_ci is set). */
    unsigned char attr_ops[CSS_MAX_ATTRS];
    bool attr_ci[CSS_MAX_ATTRS];
    char attr_vals[CSS_MAX_ATTRS][96];
    bool has_tag;
    bool has_id;
    bool is_root;    /* :root pseudo-class */
    bool universal;
    bool has_substantive;    /* a tag/id/class/attr/universal was matched */
    bool is_pseudo_element;  /* a ::pseudo-element (::before, ::-webkit-...) is present */
    bool requires_empty;     /* :empty — element must have no element/text children */
    bool never_match;        /* dynamic pseudo-class (:visited, :hover, ...) — never true in a static render */
    bool requires_link;      /* :link — must be <a>/<area> with an href attribute */
    int  child_pos;          /* 0=none, 1=:first-child, 2=:last-child, 3=:only-child */
    int  type_pos;           /* 0=none, 1=:first-of-type, 2=:last-of-type, 3=:only-of-type */
    int  nth_kind;           /* 0=none, 1=nth-child, 2=nth-last-child, 3=nth-of-type, 4=nth-last-of-type */
    int  nth_a, nth_b;       /* an+b coefficients */
    /* Flattened :not(<simple-selector>) argument (first compound only). */
    bool not_active;
    bool not_has_tag;
    bool not_has_id;
    char not_tag[64];
    char not_id[128];
    char not_classes[CSS_MAX_CLASSES][64];
    int  not_class_count;
    char not_attrs[CSS_MAX_ATTRS][64];
    int  not_attr_count;
    unsigned char not_attr_ops[CSS_MAX_ATTRS];
    bool not_attr_ci[CSS_MAX_ATTRS];
    char not_attr_vals[CSS_MAX_ATTRS][96];
    /* :where()/:is() argument, kept as raw selector text and evaluated
     * recursively at match time.  :where() contributes zero specificity;
     * :is() is treated the same here (a slight specificity deviation). */
    bool where_active;
    char where_arg[160];
    /* :has(<relative selector>): leading combinator (0 descendant, '>' '+'
     * '~') and the remaining selector text. */
    bool has_active;
    char has_comb;
    char has_arg[160];
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
#define CSS_COMB_ADJACENT   3   /* a + b */
#define CSS_COMB_SIBLING    4   /* a ~ b */

/* A fully-parsed selector chain (the text between commas). */
typedef struct CssSelectorChain {
    CssSelectorPart parts[CSS_MAX_SIMPLE_PARTS];
    int part_count;
} CssSelectorChain;

/* A compiled selector: an array of chains (one per comma group). This is
 * cached on a CssRule so matching does not re-parse the selector text for
 * every node it is tested against. */
struct CssCompiledSelector {
    CssSelectorChain *chains;
    int chain_count;
};

/* Parse one selector chain (no commas).  Returns the number of parts. */
int css_sel_parse_chain(const char *s, CssSelectorPart *parts, int max_parts);

/* Specificity of a parsed chain (a*1000 + b*10 + c). */
int css_sel_chain_specificity(const CssSelectorPart *parts, int count);

/* True for the parser's whitespace set (space, tab, CR, LF, FF). */
static inline bool css_sel_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

#endif /* CSS_SELECTOR_INTERNAL_H */

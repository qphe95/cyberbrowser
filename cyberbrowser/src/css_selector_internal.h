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
#define CSS_ATTR_OP_TOKPREFIX 7 /* .v-*: a token starting with v */

/* (set once a sheet has [*|name]: the style snapshot then gives each attribute
 * a "*|local" entry too, whatever its prefix) */
extern int g_css_attr_any_ns;
/* (set once a type selector names a namespace: the snapshot then gives each
 * element a "-cb-ns" entry, its namespace's URI ("" none)) */
extern int g_css_type_ns;

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
    case CSS_ATTR_OP_TOKPREFIX: {
        if (wl == 0) return false;
        const char *p = have;
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f') p++;
            const char *st = p;
            while (*p && !(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f')) p++;
            /* (more after it, and not another dash) */
            if ((size_t)(p - st) > wl && st[wl] != '-' && ncmp(st, want, wl) == 0) return true;
        }
        return false;
    }
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
    bool tag_upper;          /* (its type selector has capitals: in an XML document, where names are case-sensitive, no lowercase element) */
    bool has_substantive;    /* a tag/id/class/attr/universal was matched */
    bool is_pseudo_element;  /* a ::pseudo-element (::before, ::-webkit-...) is present */
    bool requires_empty;     /* :empty — element must have no element/text children */
    bool anim_img;           /* :animated-image: its image of more than one frame */
    bool ce_defined;         /* :defined: not a custom element without a definition */
    char hs_sel[96];         /* (:has-slotted(sel): what a flattened assigned element must match) */
    bool never_match;        /* dynamic pseudo-class (:visited, :hover, ...) — never true in a static render */
    bool requires_link;
    unsigned char link_kind; /* (with requires_link: 1 :link (not visited), 0 :any-link; 2 :visited) */      /* :link — must be <a>/<area> with an href attribute */
    uint32_t state_mask;     /* :hover, :checked, ...: the DOM_ST_* the element must have */
    uint32_t not_state;      /* (of the flattened :not()) */
    bool not_list_active;    /* :not() of anything else: not_list_arg, evaluated as a list */
    char not_list_arg[400];
    int  child_pos;          /* 0=none, 1=:first-child, 2=:last-child, 3=:only-child */
    int  dir_val;            /* :dir(): 1 ltr, 2 rtl, 3 neither (matches nothing) */
    uint32_t slotted_id;     /* (:-webkit-cyber-slotted(N), ::slotted() of host N's tree: a node assigned to a slot of it) */
    bool slotted_host;       /* (written after :host: that much more specific) */
    uint32_t node_id;        /* (:-webkit-cyber-host(N), a shadow host's scoping: the element with NodeId N) */
    char lang_ranges[96];    /* (:lang() with a list, wildcards or several subtags: its ranges, extended filtering) */
    int  type_pos;           /* 0=none, 1=:first-of-type, 2=:last-of-type, 3=:only-of-type */
    int  nth_kind;           /* 0=none, 1=nth-child, 2=nth-last-child, 3=nth-of-type, 4=nth-last-of-type */
    int  nth_a, nth_b;       /* an+b coefficients */
    bool nth_of_active;      /* :nth-child(An+B of S): only siblings matching S counted */
    char nth_of_arg[300];
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
    bool is_weighted;           /* :is(), not :where(): weighs its most specific selector */
    unsigned char lang_count;   /* :lang() pseudo-classes (each one in the specificity) */
    char where_arg[400];
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
/* (a compound of nothing but pseudo-classes tests something too; a pseudo-element alone doesn't) */
static inline bool css_simple_substantive(const CssSimpleSelector *s) {
    return s->has_substantive || s->child_pos || s->dir_val || s->node_id || s->slotted_id || s->lang_ranges[0] || s->type_pos || s->nth_kind || s->state_mask || s->requires_empty ||
           s->requires_link || s->not_active || s->not_list_active || s->lang_count;
}

static inline bool css_sel_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

#endif /* CSS_SELECTOR_INTERNAL_H */

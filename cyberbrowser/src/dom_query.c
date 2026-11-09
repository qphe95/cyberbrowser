/* Selector queries and indexes over the node store.  See dom_query.h. */
#include "dom_query.h"
#include "cyber_env.h"
#include "job_system.h"
#include "css_selector_internal.h"
#include "css_parser.h"
#include "css_style.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static DomQueryStats g_qstats;
void dom_query_stats(DomQueryStats *out) { *out = g_qstats; }

void dom_node_list_free(DomNodeList *l) { free(l->ids); l->ids = NULL; l->n = l->cap = 0; }

static void nl_push(DomNodeList *l, NodeId id) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 16; l->ids = realloc(l->ids, l->cap * sizeof(NodeId)); }
    l->ids[l->n++] = id;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ============================================================================
 * Compiled selectors
 * ============================================================================ */

enum {
    SS_TAG, SS_UNIVERSAL, SS_ID, SS_CLASS, SS_ATTR,
    SS_NOT, SS_IS, SS_HAS, SS_ANCHOR,
    SS_FIRST_CHILD, SS_LAST_CHILD, SS_ONLY_CHILD, SS_NTH_CHILD, SS_NTH_LAST_CHILD,
    SS_NTH_OF_TYPE, SS_NTH_LAST_OF_TYPE,
    SS_FIRST_OF_TYPE, SS_LAST_OF_TYPE, SS_ONLY_OF_TYPE, SS_EMPTY, SS_ROOT, SS_SCOPE,
    SS_DISABLED, SS_ENABLED,
    SS_STATE,        /* dom_element_state has all of mask */
    SS_NO_NS,        /* |type: an element in no namespace */
    SS_LINK,         /* :link, :any-link: a and area with href */
    SS_TARGET,       /* :target: the element the URL's fragment names */
    SS_LANG,         /* :lang(val) */
    SS_DIR,          /* :dir(ltr), :dir(rtl): a is 1 for rtl */
    SS_HEADING,      /* :heading, :heading(levels): a, a bit for each level */
    SS_HOST,         /* :host: only across a combinator out of a shadow tree */
    SS_VALID, SS_INVALID,
    SS_ANY,          /* unsupported pseudo-class: matches */
    SS_NEVER,        /* pseudo-element: never matches */
};
enum { AO_EXISTS, AO_EQ, AO_WORD, AO_DASH, AO_PREFIX, AO_SUFFIX, AO_SUBSTR, AO_TOKPREFIX };

struct SelList;

typedef struct SelSimple {
    uint8_t kind, op, ci;
    bool any_ns;                /* [*|name] */
    CssAtom atom;               /* tag / attribute name */
    char *val;                  /* id, class, attribute value */
    char *raw;                  /* an attribute name with upper case, as written (else NULL) */
    uint32_t vlen;
    int a, b;                   /* an+b */
    struct SelList *sub;        /* :not / :is */
} SelSimple;

typedef struct SelCompound {
    bool pe;            /* it ends in a pseudo-element: nothing may be combined after it */
    SelSimple *s;
    int n;
    char comb;                  /* combinator to the compound on the left */
} SelCompound;

typedef struct SelComplex {
    SelCompound *c;
    int n;
} SelComplex;

typedef struct SelList {
    SelComplex *x;
    int n;
    bool ok;
} SelList;

static void sel_free(SelList *l);

typedef struct SelParser {
    const char *p, *e;
    bool err;
    bool relative;  /* in :has(): each from the anchor */
    bool no_pe;     /* in :not() and the like: no pseudo-elements */
} SelParser;

/* Comments separate tokens without being whitespace (a comment between `div`
 * and `.x` leaves one compound); an unterminated one runs to the end. */
static void sp_comments(SelParser *sp) {
    while (sp->p + 1 < sp->e && sp->p[0] == '/' && sp->p[1] == '*') {
        const char *q = sp->p + 2;
        while (q + 1 < sp->e && !(q[0] == '*' && q[1] == '/')) q++;
        sp->p = q + 1 < sp->e ? q + 2 : sp->e;
    }
}

/* Skips whitespace and comments; whether there was whitespace. */
static bool sp_ws(SelParser *sp) {
    bool ws = false;
    for (;;) {
        sp_comments(sp);
        if (sp->p >= sp->e || !isspace((unsigned char)*sp->p)) return ws;
        sp->p++;
        ws = true;
    }
}

/* (NUL: U+FFFD, CSS Syntax 3.3's preprocessing) */
static bool ident_char(unsigned char c) {
    return isalnum(c) || c == '-' || c == '_' || c >= 0x80 || c == '\\' || c == 0;
}

static size_t put_utf8(char *o, uint32_t c) {
    if (c == 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | (c >> 6 & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | (c >> 12 & 63));
    o[2] = (char)(0x80 | (c >> 6 & 63)); o[3] = (char)(0x80 | (c & 63));
    return 4;
}

/* A hex escape's code point, at the digits after the backslash; eats one
 * whitespace after them. */
static uint32_t sp_hex(SelParser *sp) {
    uint32_t c = 0;
    for (int k = 0; k < 6 && sp->p < sp->e && isxdigit((unsigned char)*sp->p); k++, sp->p++)
        c = c * 16 + (uint32_t)(isdigit((unsigned char)*sp->p) ? *sp->p - '0' : tolower((unsigned char)*sp->p) - 'a' + 10);
    /* (CR LF is one newline) */
    if (sp->p + 1 < sp->e && sp->p[0] == '\r' && sp->p[1] == '\n') sp->p += 2;
    else if (sp->p < sp->e && isspace((unsigned char)*sp->p)) sp->p++;
    return c;
}

/* An identifier with backslash escapes (hex ones too), unescaped into a new string. */
static char *sp_ident(SelParser *sp, uint32_t *len) {
    const char *b = sp->p;
    char *out = malloc((size_t)(sp->e - b) * 4 + 1);
    uint32_t n = 0;
    while (sp->p < sp->e && ident_char((unsigned char)*sp->p)) {
        if (*sp->p == 0) {      /* (NUL) */
            n += (uint32_t)put_utf8(out + n, 0xFFFD);
            sp->p++;
            continue;
        }
        if (*sp->p == '\\') {
            if (sp->p + 1 >= sp->e) {       /* (an escape at the end: U+FFFD) */
                n += (uint32_t)put_utf8(out + n, 0xFFFD);
                sp->p++;
                continue;
            }
            if (sp->p[1] == '\n') break;
            sp->p++;
            if (isxdigit((unsigned char)*sp->p)) {
                n += (uint32_t)put_utf8(out + n, sp_hex(sp));
                continue;
            }
            out[n++] = *sp->p++;
            continue;
        }
        out[n++] = *sp->p++;
    }
    out[n] = '\0';
    if (!n) { free(out); sp->err = true; return NULL; }
    *len = n;
    return out;
}

/* Does an identifier start at p (CSS Syntax 4.3.9: a letter, '_', non-ASCII
 * or an escape, after an optional '-'; or "--")? */
static bool ident_start(const SelParser *sp) {
    const char *p = sp->p;
    if (p < sp->e && *p == '-') {
        p++;
        if (p < sp->e && *p == '-') return true;
    }
    if (p >= sp->e) return false;
    unsigned char c = (unsigned char)*p;
    return isalpha(c) || c == '_' || c >= 0x80 || c == 0 || (c == '\\' && (p + 1 >= sp->e || p[1] != '\n'));
}

/* The pseudo-classes and pseudo-elements a selector may name (Selectors 4,
 * HTML, CSS Scoping and Pseudo-Elements, and the -webkit- ones Chrome has);
 * any other is invalid. */
static bool known_name(const char *n, const char *const *list) {
    for (; *list; list++)
        if (!strcmp(n, *list)) return true;
    return false;
}
static const char *const k_pseudo_classes[] = {
    "has-slotted", "active", "any-link", "autofill", "blank", "buffering", "checked", "closed", "current", "default", "defined",
    "dir", "disabled", "empty", "enabled", "first", "first-child", "first-of-type", "focus", "focus-visible",
    "focus-within", "fullscreen", "future", "has", "heading", "host", "host-context", "hover", "in-range", "indeterminate",
    "invalid", "is", "lang", "last-child", "last-of-type", "left", "link", "local-link", "matches", "modal", "muted",
    "not", "nth-child", "nth-col", "nth-last-child", "nth-last-col", "nth-last-of-type", "nth-of-type",
    "only-child", "only-of-type", "open", "optional", "out-of-range", "past", "paused", "picture-in-picture",
    "placeholder-shown", "playing", "popover-open", "read-only", "read-write", "required", "right", "root",
    "scope", "seeking", "stalled", "state", "target", "target-within", "user-invalid", "user-valid", "valid",
    "visited", "volume-locked", "where", "xr-overlay", "-webkit-any", "-webkit-any-link", "-webkit-autofill",
    "-webkit-drag", "-webkit-full-screen",
    NULL };
static const char *const k_pseudo_elements[] = {
    "after", "backdrop", "before", "cue", "cue-region", "details-content", "file-selector-button", "first-letter",
    "first-line", "grammar-error", "highlight", "marker", "part", "picker", "placeholder", "scroll-marker",
    "scroll-marker-group", "selection", "slider-fill", "slider-thumb", "slider-track", "slotted", "spelling-error", "target-text", "view-transition",
    "view-transition-group", "view-transition-group-children", "view-transition-image-pair", "view-transition-new", "view-transition-old", NULL };

static SelList *parse_list(SelParser *sp, bool nested);

static void add_simple(SelCompound *c, SelSimple s) {
    c->s = realloc(c->s, (size_t)(c->n + 1) * sizeof(SelSimple));
    c->s[c->n++] = s;
}

static bool parse_nth(const char *s, size_t n, int *a, int *b) {
    char buf[64];
    /* `An+B of S`: the S filter isn't applied (it counts all siblings). */
    for (size_t i = 1; i + 3 < n; i++)
        if (isspace((unsigned char)s[i - 1]) && tolower((unsigned char)s[i]) == 'o' &&
            tolower((unsigned char)s[i + 1]) == 'f' && isspace((unsigned char)s[i + 2])) { n = i; break; }
    if (n >= sizeof buf) return false;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) if (!isspace((unsigned char)s[i])) buf[k++] = (char)tolower((unsigned char)s[i]);
    buf[k] = '\0';
    if (strcmp(buf, "odd") == 0) { *a = 2; *b = 1; return true; }
    if (strcmp(buf, "even") == 0) { *a = 2; *b = 0; return true; }
    char *nx = strchr(buf, 'n');
    if (!nx) { char *end; long v = strtol(buf, &end, 10); if (*end) return false; *a = 0; *b = (int)v; return true; }
    *nx = '\0';
    if (buf[0] == '\0' || strcmp(buf, "+") == 0) *a = 1;
    else if (strcmp(buf, "-") == 0) *a = -1;
    else { char *end; long v = strtol(buf, &end, 10); if (*end) return false; *a = (int)v; }
    const char *rest = nx + 1;
    if (!*rest) { *b = 0; return true; }
    char *end;
    long v = strtol(rest, &end, 10);
    if (*end) return false;
    *b = (int)v;
    return true;
}

/* A forgiving selector list (:is, :where): the branches that parse; an
 * empty list (matching nothing) when none do. */
static SelList *parse_forgiving(const char *a, size_t n) {
    SelList *l = calloc(1, sizeof(SelList));
    l->ok = true;
    size_t b = 0;
    int depth = 0;
    char q = 0;
    for (size_t i = 0; i <= n; i++) {
        char c = i < n ? a[i] : ',';
        if (q) { if (c == '\\') i++; else if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') { q = c; continue; }
        if (c == '(' || c == '[') depth++;
        else if (c == ')' || c == ']') depth--;
        if (c != ',' || depth > 0) continue;
        SelParser sp = { a + b, a + i, false };
        sp.no_pe = true;
        SelList *one = parse_list(&sp, true);
        if (one->ok && !sp.err && one->n == 1) {
            l->x = realloc(l->x, (size_t)(l->n + 1) * sizeof(SelComplex));
            l->x[l->n++] = one->x[0];
            one->n = 0;
        }
        sel_free(one);
        b = i + 1;
    }
    return l;
}

/* Parse one compound; returns false at the end of the complex selector. */
static bool parse_compound(SelParser *sp, SelCompound *c) {
    bool any = false;
    int vt = 0;     /* after a view transition pseudo-element: nothing, 2: only :only-child */
    while (sp->p < sp->e && !sp->err) {
        sp_comments(sp);
        if (sp->p >= sp->e) break;
        /* `*|` and `|` before a type selector: any namespace / none */
        if (!any && sp->p + 1 < sp->e && sp->p[0] == '*' && sp->p[1] == '|') sp->p += 2;
        else if (!any && sp->p + 1 < sp->e && sp->p[0] == '|' && sp->p[1] != '=') {
            sp->p++;
            SelSimple ns;
            memset(&ns, 0, sizeof ns);
            ns.kind = SS_NO_NS;
            add_simple(c, ns);
        }
        if (sp->p >= sp->e) { sp->err = true; break; }
        char ch = *sp->p;
        SelSimple s;
        memset(&s, 0, sizeof s);
        if (vt && (ch == '*' || ch == '.' || ch == '#' || ch == '[' || ch == ':' || isalpha((unsigned char)ch))) {
            if (vt != 2 || sp->e - sp->p < 11 || strncasecmp(sp->p, ":only-child", 11)) { sp->err = true; break; }
        }
        if (ch == '*') {
            sp->p++;
            s.kind = SS_UNIVERSAL;
        } else if (ch == '#') {
            sp->p++;
            if (!ident_start(sp)) { sp->err = true; break; }
            s.kind = SS_ID;
            s.val = sp_ident(sp, &s.vlen);
        } else if (ch == '.') {
            sp->p++;
            if (!ident_start(sp)) { sp->err = true; break; }
            s.kind = SS_CLASS;
            s.val = sp_ident(sp, &s.vlen);
            /* (.foo-*: a class that starts so) */
            if (s.val && s.vlen > 1 && s.val[s.vlen - 1] == '-' && sp->p < sp->e && *sp->p == '*') {
                sp->p++;
                css_atom_reserve(1);
                s.kind = SS_ATTR;
                s.atom = css_atom_intern("class", 5);
                s.op = AO_TOKPREFIX;
                if (sp->p < sp->e && *sp->p == '*') { sp->err = true; free(s.val); break; }
            } else if (s.val && s.vlen) {
                css_atom_reserve(1);
                s.atom = css_atom_intern(s.val, s.vlen);
                for (uint32_t k = 0; k < s.vlen; k++)
                    if (s.val[k] >= 'A' && s.val[k] <= 'Z') { s.ci = 2; break; }   /* (upper case: checked exactly) */
            }
        } else if (ch == '[') {
            sp->p++;
            sp_ws(sp);
            /* `*|name` and `|name`: any and no namespace (attributes carry
             * none here).  A prefix would need @namespace, so it's invalid. */
            if (sp->p + 1 < sp->e && sp->p[0] == '*' && sp->p[1] == '|') { sp->p += 2; s.any_ns = true; }
            else if (sp->p + 1 < sp->e && sp->p[0] == '|' && sp->p[1] != '=') sp->p++;
            uint32_t nl = 0;
            char *name = sp_ident(sp, &nl);
            if (name && sp->p + 1 < sp->e && sp->p[0] == '|' && sp->p[1] != '=') { free(name); sp->err = true; break; }
            if (!name) break;
            css_atom_reserve(1);
            s.kind = SS_ATTR;
            s.atom = css_atom_intern(name, nl);
            for (uint32_t k = 0; k < nl && !s.raw; k++)
                if (name[k] >= 'A' && name[k] <= 'Z') s.raw = strndup(name, nl);
            bool legacy = css_attr_legacy_ci(name, nl);
            free(name);
            sp_ws(sp);
            s.op = AO_EXISTS;
            if (sp->p < sp->e && *sp->p != ']') {
                char o = *sp->p;
                if (o == '=') { s.op = AO_EQ; sp->p++; }
                else if (sp->p + 1 < sp->e && sp->p[1] == '=' && strchr("~|^$*", o)) {
                    s.op = o == '~' ? AO_WORD : o == '|' ? AO_DASH : o == '^' ? AO_PREFIX : o == '$' ? AO_SUFFIX : AO_SUBSTR;
                    sp->p += 2;
                } else { sp->err = true; break; }
                sp_ws(sp);
                if (sp->p < sp->e && (*sp->p == '"' || *sp->p == '\'')) {
                    char q = *sp->p++;
                    s.val = malloc((size_t)(sp->e - sp->p) * 4 + 1);
                    while (sp->p < sp->e && *sp->p != q) {
                        if (*sp->p == '\\' && sp->p + 1 < sp->e) {
                            sp->p++;
                            if (*sp->p == '\n') { sp->p++; continue; }
                            if (isxdigit((unsigned char)*sp->p)) {
                                s.vlen += (uint32_t)put_utf8(s.val + s.vlen, sp_hex(sp));
                                continue;
                            }
                        }
                        s.val[s.vlen++] = *sp->p++;
                    }
                    s.val[s.vlen] = '\0';
                    if (sp->p < sp->e) sp->p++;     /* (the end of the input closes a string) */
                } else {
                    s.val = sp_ident(sp, &s.vlen);
                    if (!s.val) break;
                }
                sp_ws(sp);
                /* The flag: exactly i or s, ASCII case-insensitively (escapes allowed). */
                if (sp->p < sp->e && *sp->p != ']') {
                    uint32_t fl = 0;
                    char *f = ident_char((unsigned char)*sp->p) ? sp_ident(sp, &fl) : NULL;
                    char fc = f && fl == 1 ? (char)tolower((unsigned char)f[0]) : 0;
                    free(f);
                    if (fc != 'i' && fc != 's') { sp->err = true; free(s.val); free(s.raw); break; }
                    s.ci = fc == 'i';
                    sp_ws(sp);
                } else {
                    s.ci = legacy ? 3 : 0;
                }
            }
            /* (and the end of the input the brackets: CSS Syntax 5.4.8) */
            if (sp->p < sp->e && *sp->p != ']') { sp->err = true; free(s.val); free(s.raw); break; }
            if (sp->p < sp->e) sp->p++;
        } else if (ch == ':') {
            sp->p++;
            if (sp->p < sp->e && *sp->p == ':') {
                sp->p++;
                uint32_t nl = 0;
                char *name = ident_start(sp) ? sp_ident(sp, &nl) : NULL;
                if (!name) { sp->err = true; break; }
                for (char *q = name; *q; q++) *q = (char)tolower((unsigned char)*q);
                bool known = known_name(name, k_pseudo_elements) || !strncmp(name, "-webkit-", 8);
                bool vt_fn = !strncmp(name, "view-transition-", 16), vt_root = !strcmp(name, "view-transition");
                bool hl = !strcmp(name, "highlight");
                free(name);
                /* (::highlight(): one name, or *) */
                if (hl && sp->p < sp->e && *sp->p == '(') {
                    const char *a = sp->p + 1, *e = a;
                    while (e < sp->e && *e != ')') e++;
                    while (a < e && isspace((unsigned char)*a)) a++;
                    const char *z = e;
                    while (z > a && isspace((unsigned char)z[-1])) z--;
                    bool ok = z > a;
                    if (ok && !(z - a == 1 && *a == '*')) {
                        for (const char *k = a; k < z && ok; k++)
                            ok = isalnum((unsigned char)*k) || *k == '-' || *k == '_' || (unsigned char)*k >= 0x80 || *k == '\\';
                        if (ok && isdigit((unsigned char)*a)) ok = false;
                    }
                    if (!ok) { sp->err = true; break; }
                }
                if (!known || sp->no_pe) { sp->err = true; break; }
                bool paren = sp->p < sp->e && *sp->p == '(';
                if (vt_root && paren) { sp->err = true; break; }
                if (vt_fn) {
                    const char *a = sp->p + 1;
                    while (paren && a < sp->e && isspace((unsigned char)*a)) a++;
                    if (!paren || a >= sp->e || *a == ')') { sp->err = true; break; }
                    /* (* or a name, then classes) */
                    const char *w = a;
                    bool shape = true;
                    #define VT_IDENT() do { while (w < sp->e && (isalnum((unsigned char)*w) || *w == '-' || *w == '_' || *w == 92 || (unsigned char)*w >= 0x80)) { if (*w == 92 && w + 1 < sp->e) w++; w++; } } while (0)
                    if (*w == '*') w++;
                    else VT_IDENT();
                    while (shape && w < sp->e && *w == '.') {
                        const char *st = ++w;
                        VT_IDENT();
                        if (w == st) shape = false;
                    }
                    #undef VT_IDENT
                    while (w < sp->e && isspace((unsigned char)*w)) w++;
                    if (!shape || (w < sp->e && *w != ')')) { sp->err = true; break; }
                    /* (a <custom-ident>: not these) */
                    static const char *const no[] = { "default", "initial", "inherit", "unset", "revert", "revert-layer", "none" };
                    bool bad = false;
                    for (size_t k = 0; k < sizeof no / sizeof no[0]; k++) {
                        size_t kl = strlen(no[k]);
                        if ((size_t)(sp->e - a) > kl && !strncasecmp(a, no[k], kl) && (a[kl] == ')' || a[kl] == '.' || isspace((unsigned char)a[kl]))) bad = true;
                    }
                    if (bad) { sp->err = true; break; }
                }
                vt = vt_root ? 1 : vt_fn ? 2 : 0;
                c->pe = true;
                /* (functional ones: their arguments, to the ')') */
                if (sp->p < sp->e && *sp->p == '(') {
                    int depth = 0;
                    for (; sp->p < sp->e; sp->p++) {
                        if (*sp->p == '(') depth++;
                        else if (*sp->p == ')' && !--depth) break;
                    }
                    if (sp->p < sp->e) sp->p++;     /* (the end of the input closes them) */
                }
                s.kind = SS_NEVER;
            } else {
                uint32_t nl = 0;
                char *name = ident_start(sp) ? sp_ident(sp, &nl) : NULL;
                if (!name) { sp->err = true; break; }
                for (char *q = name; *q; q++) *q = (char)tolower((unsigned char)*q);
                const char *args = NULL;
                size_t alen = 0;
                if (sp->p < sp->e && *sp->p == '(') {
                    sp->p++;
                    args = sp->p;
                    /* to the matching ')': brackets and strings inside must
                     * close before it (an unclosed one spoils the whole), but
                     * the end of the input closes them all */
                    int depth = 1, square = 0;
                    char quote = 0;
                    while (sp->p < sp->e && depth) {
                        char c = *sp->p;
                        if (quote) { if (c == '\\' && sp->p + 1 < sp->e) sp->p++; else if (c == quote) quote = 0; }
                        else if (c == '"' || c == '\'') quote = c;
                        else if (c == '[') square++;
                        else if (c == ']') square--;
                        else if (c == '(') depth++;
                        else if (c == ')') depth--;
                        if (depth) sp->p++;
                    }
                    if (sp->p < sp->e && (square || quote)) { sp->err = true; free(name); break; }
                    alen = (size_t)(sp->p - args);
                    if (sp->p < sp->e) sp->p++;
                }
                if (strcmp(name, "has") == 0) {
                    /* (relative selectors: each from an anchor, the element) */
                    if (!args) { sp->err = true; free(name); break; }
                    SelParser sub = { args, args + alen, false, true };
                    sub.no_pe = true;
                    s.sub = parse_list(&sub, true);
                    if (!s.sub || !s.sub->ok) { sel_free(s.sub); sp->err = true; free(name); break; }
                    s.kind = SS_HAS;
                    for (int q = 0; q < s.sub->n; q++)
                        if (s.sub->x[q].n > 1 && (s.sub->x[q].c[1].comb == '+' || s.sub->x[q].c[1].comb == '~')) s.a = 1;
                } else if (strcmp(name, "not") == 0 || strcmp(name, "is") == 0 || strcmp(name, "where") == 0 ||
                    strcmp(name, "matches") == 0 || strcmp(name, "-webkit-any") == 0) {
                    if (!args) { sp->err = true; free(name); break; }
                    bool is_not = strcmp(name, "not") == 0;
                    s.sub = is_not ? NULL : parse_forgiving(args, alen);
                    if (is_not) {
                        SelParser sub = { args, args + alen, false };
                        sub.no_pe = true;
                        s.sub = parse_list(&sub, true);
                        if (!s.sub || !s.sub->ok) { sel_free(s.sub); sp->err = true; free(name); break; }
                    }
                    s.kind = is_not ? SS_NOT : SS_IS;
                } else if (strcmp(name, "nth-child") == 0 || strcmp(name, "nth-last-child") == 0) {
                    if (!args || !parse_nth(args, alen, &s.a, &s.b)) { sp->err = true; free(name); break; }
                    s.kind = name[4] == 'l' ? SS_NTH_LAST_CHILD : SS_NTH_CHILD;
                } else if (strcmp(name, "nth-of-type") == 0 || strcmp(name, "nth-last-of-type") == 0) {
                    if (!args || !parse_nth(args, alen, &s.a, &s.b)) { sp->err = true; free(name); break; }
                    s.kind = name[4] == 'l' ? SS_NTH_LAST_OF_TYPE : SS_NTH_OF_TYPE;
                } else if (strcmp(name, "first-child") == 0) s.kind = SS_FIRST_CHILD;
                else if (strcmp(name, "last-child") == 0) s.kind = SS_LAST_CHILD;
                else if (strcmp(name, "only-child") == 0) s.kind = SS_ONLY_CHILD;
                else if (strcmp(name, "first-of-type") == 0) s.kind = SS_FIRST_OF_TYPE;
                else if (strcmp(name, "last-of-type") == 0) s.kind = SS_LAST_OF_TYPE;
                else if (strcmp(name, "only-of-type") == 0) s.kind = SS_ONLY_OF_TYPE;
                else if (strcmp(name, "empty") == 0) s.kind = SS_EMPTY;
                else if (strcmp(name, "root") == 0) s.kind = SS_ROOT;
                else if (strcmp(name, "scope") == 0) s.kind = SS_SCOPE;
                else if (strcmp(name, "disabled") == 0) s.kind = SS_DISABLED;
                else if (strcmp(name, "enabled") == 0) s.kind = SS_ENABLED;
                else if (strcmp(name, "link") == 0 || strcmp(name, "any-link") == 0 || strcmp(name, "-webkit-any-link") == 0)
                    s.kind = SS_LINK;
                else if (strcmp(name, "visited") == 0) s.kind = SS_NEVER;      /* (no history here) */
                /* (states nothing here enters: no user edits tracked, no fullscreen, no top layer) */
                else if (!strcmp(name, "user-valid") || !strcmp(name, "user-invalid") || !strcmp(name, "fullscreen") ||
                         !strcmp(name, "picture-in-picture") || !strcmp(name, "volume-locked") || !strcmp(name, "xr-overlay") ||
                         !strcmp(name, "local-link") || !strcmp(name, "blank") || !strcmp(name, "first") ||
                         !strcmp(name, "left") || !strcmp(name, "right")) s.kind = SS_NEVER;
                else if ((s.a = (int)css_state_bit(name, strlen(name))) != 0 && strcmp(name, "disabled") && strcmp(name, "enabled") &&
                         strcmp(name, "target") && strcmp(name, "valid") && strcmp(name, "invalid") &&
                         strcmp(name, "placeholder-shown") && strcmp(name, "default") && strcmp(name, "in-range") && strcmp(name, "out-of-range")) s.kind = SS_STATE;
                else if (strcmp(name, "target") == 0) s.kind = SS_TARGET;
                else if (strcmp(name, "heading") == 0) {
                    s.kind = SS_HEADING;
                    s.a = args ? 0 : 0x3fe;
                    for (size_t k = 0; args && k < alen; k++) {
                        if (args[k] < '0' || args[k] > '9') continue;
                        int v = 0;
                        while (k < alen && args[k] >= 0x30 && args[k] <= 0x39) { if (v < 100) v = v * 10 + (args[k] - 0x30); k++; }
                        if (v >= 1 && v <= 9) s.a |= 1 << v;
                    }
                }
                else if (strcmp(name, "lang") == 0) {
                    /* (the argument: an identifier or a string, trimmed) */
                    const char *a = args, *ae = args ? args + alen : NULL;
                    while (a && a < ae && (*a == ' ' || *a == '\t' || *a == '\n')) a++;
                    while (a && ae > a && (ae[-1] == ' ' || ae[-1] == '\t' || ae[-1] == '\n')) ae--;
                    if (a && ae - a >= 2 && (*a == '"' || *a == '\'') && ae[-1] == *a) { a++; ae--; }
                    if (!a || ae <= a) { sp->err = true; free(name); break; }
                    s.kind = SS_LANG;
                    s.vlen = (uint32_t)(ae - a);
                    s.val = strndup(a, s.vlen);
                }
                else if (strcmp(name, "dir") == 0) {
                    const char *a = args, *ae = args ? args + alen : NULL;
                    while (a && a < ae && (*a == ' ' || *a == '\t' || *a == '\n')) a++;
                    while (a && ae > a && (ae[-1] == ' ' || ae[-1] == '\t' || ae[-1] == '\n')) ae--;
                    if (!a || ae <= a) { sp->err = true; free(name); break; }
                    /* (only those two ever match, whatever else it's given) */
                    s.kind = ae - a == 3 && !strncasecmp(a, "ltr", 3) ? SS_DIR : ae - a == 3 && !strncasecmp(a, "rtl", 3) ? SS_DIR : SS_NEVER;
                    s.a = ae - a == 3 && !strncasecmp(a, "rtl", 3) ? 1 : 0;
                }
                else if (strcmp(name, "before") == 0 || strcmp(name, "after") == 0 ||
                         strcmp(name, "first-line") == 0 || strcmp(name, "first-letter") == 0) s.kind = SS_NEVER;
                else if (strcmp(name, "host") == 0 && !args) s.kind = SS_HOST;
                else if (strcmp(name, "has-slotted") == 0) {
                    /* (a slot's assigned nodes: a selector of them, no child combinator; nothing's slotted here) */
                    if (args) {
                        bool child = false;
                        for (size_t k = 0; k < alen; k++) if (args[k] == '>') child = true;
                        SelParser sub = { args, args + alen, false };
                        sub.no_pe = true;
                        SelList *l = child ? NULL : parse_list(&sub, true);
                        bool ok = l && l->ok && l->n == 1;
                        sel_free(l);
                        if (!ok) { sp->err = true; free(name); break; }
                    }
                    s.kind = SS_NEVER;
                }
                else if (strcmp(name, "valid") == 0 || strcmp(name, "invalid") == 0) {
                    s.kind = name[0] == 'v' ? SS_VALID : SS_INVALID;
                    css_atom_reserve(1);
                    s.atom = css_atom_intern("form", 4);
                }
                else if (known_name(name, k_pseudo_classes)) s.kind = SS_ANY;
                else { sp->err = true; free(name); break; }
                free(name);
            }
        } else if (ident_char((unsigned char)ch)) {
            if (!ident_start(sp)) { sp->err = true; break; }
            uint32_t nl = 0;
            char *name = sp_ident(sp, &nl);
            if (!name) break;
            if (any) { sp->err = true; free(name); break; }   /* type selector must come first */
            css_atom_reserve(1);
            s.kind = SS_TAG;
            s.atom = css_atom_intern(name, nl);
            for (uint32_t k = 0; k < nl && !s.raw; k++)
                if (name[k] >= 'A' && name[k] <= 'Z') s.raw = strndup(name, nl);
            free(name);
        } else {
            break;
        }
        if (sp->err) break;
        add_simple(c, s);
        any = true;
    }
    if (!any) sp->err = true;
    return !sp->err;
}

static SelList *parse_list(SelParser *sp, bool nested) {
    SelList *l = calloc(1, sizeof(SelList));
    l->ok = true;
    for (;;) {
        sp_ws(sp);
        SelComplex cx = { NULL, 0 };
        char comb = 0;
        if (sp->relative) {
            comb = ' ';
            if (sp->p < sp->e && (*sp->p == '>' || *sp->p == '+' || *sp->p == '~')) {
                comb = *sp->p++;
                sp_ws(sp);
            }
            SelCompound a = { .s = NULL, .n = 0 };
            SelSimple as;
            memset(&as, 0, sizeof(as));
            as.kind = SS_ANCHOR;
            add_simple(&a, as);
            cx.c = malloc(sizeof(SelCompound));
            cx.c[cx.n++] = a;
        }
        for (;;) {
            SelCompound c = { .s = NULL, .n = 0, .comb = comb };
            if (!parse_compound(sp, &c)) { free(c.s); break; }
            bool ended = c.pe;
            cx.c = realloc(cx.c, (size_t)(cx.n + 1) * sizeof(SelCompound));
            cx.c[cx.n++] = c;
            bool ws = sp_ws(sp);
            if (sp->p >= sp->e || *sp->p == ',') break;
            if (ended) {
                sp->err = true;
                break;
            }
            if (*sp->p == '>' || *sp->p == '+' || *sp->p == '~') {
                comb = *sp->p++;
                sp_ws(sp);
            } else if (ws) {
                comb = ' ';
            } else {
                sp->err = true;
                break;
            }
        }
        if (sp->err || !cx.n) {
            for (int i = 0; i < cx.n; i++) free(cx.c[i].s);
            free(cx.c);
            l->ok = false;
            break;
        }
        l->x = realloc(l->x, (size_t)(l->n + 1) * sizeof(SelComplex));
        l->x[l->n++] = cx;
        sp_ws(sp);
        if (sp->p >= sp->e) break;
        if (*sp->p == ',') { sp->p++; continue; }
        l->ok = false;
        break;
    }
    (void)nested;
    return l;
}

static void sel_free(SelList *l) {
    if (!l) return;
    for (int i = 0; i < l->n; i++) {
        for (int j = 0; j < l->x[i].n; j++) {
            SelCompound *c = &l->x[i].c[j];
            for (int k = 0; k < c->n; k++) {
                free(c->s[k].val);
                free(c->s[k].raw);
                sel_free(c->s[k].sub);
            }
            free(c->s);
        }
        free(l->x[i].c);
    }
    free(l->x);
    free(l);
}

/* Cache of compiled selectors by text (engine thread). */
#define SEL_CACHE 256
typedef struct { char *text; size_t len; uint32_t hash; SelList *list; uint64_t used; } SelEnt;
static SelEnt g_sel_cache[SEL_CACHE];
static uint64_t g_sel_tick;

/* Is it a selector list querySelector takes? */
bool dom_selector_valid(const char *s)
{
    SelParser sp = { s, s + strlen(s), false };
    SelList *l = parse_list(&sp, false);
    bool ok = l && l->ok && !sp.err;
    sel_free(l);
    return ok;
}

static SelList *sel_get(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    SelEnt *victim = &g_sel_cache[0];
    for (int i = 0; i < SEL_CACHE; i++) {
        SelEnt *e = &g_sel_cache[i];
        if (e->text && e->hash == h && e->len == n && memcmp(e->text, s, n) == 0) {
            e->used = ++g_sel_tick;
            g_qstats.cache_hits++;
            return e->list;
        }
        if (!e->text) { victim = e; break; }
        if (e->used < victim->used) victim = e;
    }
    SelParser sp = { s, s + n, false };
    SelList *l = parse_list(&sp, false);
    if (sp.err) l->ok = false;
    g_qstats.compiles++;
    free(victim->text);
    sel_free(victim->list);
    victim->text = malloc(n + 1);
    memcpy(victim->text, s, n);
    victim->text[n] = '\0';
    victim->len = n;
    victim->hash = h;
    victim->list = l;
    victim->used = ++g_sel_tick;
    return l;
}

bool dom_query_valid(const char *sel, size_t len) {
    SelList *l = sel_get(sel, len);
    return l && l->ok && l->n;
}

/* ============================================================================
 * Matching (right to left)
 * ============================================================================ */

static bool word_in(const char *list, size_t ln, const char *w, size_t wn, bool ci) {
    size_t i = 0;
    while (i < ln) {
        while (i < ln && isspace((unsigned char)list[i])) i++;
        size_t b = i;
        while (i < ln && !isspace((unsigned char)list[i])) i++;
        if (i - b == wn && wn && (ci ? strncasecmp(list + b, w, wn) == 0 : memcmp(list + b, w, wn) == 0)) return true;
    }
    return false;
}

static bool cmp_n(const char *a, const char *b, size_t n, bool ci) {
    return ci ? strncasecmp(a, b, n) == 0 : memcmp(a, b, n) == 0;
}

/* (an attribute selector's name matches ASCII-case-insensitively on an HTML
 * element in an HTML document, else as written) */
static bool attr_ok(uint32_t s, const SelSimple *ss) {
    DomAttrList *l = &g_dom.attrs[s];
    bool exact = !(g_dom.ns[s] == DOM_NS_HTML && dom_is_html_document(dom_owner(dom_id_of(s))));
    const char *written = ss->raw ? ss->raw : css_atom_str(ss->atom);
    for (int i = 0; i < l->n; i++) {
        if (l->a[i].name != ss->atom) {
            /* (under its qualified name, with a prefix) */
            const char *qn = ss->any_ns ? dom_attr_qualified_name(&l->a[i]) : NULL;
            const char *colon = qn ? strchr(qn, ':') : NULL;
            const char *want = colon ? css_atom_str(ss->atom) : NULL;
            if (!want || strcmp(colon + 1, want) != 0) continue;
        } else if (exact && written && strcmp(dom_attr_qualified_name(&l->a[i]), written) != 0) continue;
        /* (an HTML element's: the selector's name, lower-cased, as the attribute's was written) */
        else if (!exact && l->a[i].qname) {
            const char *qn = dom_attr_qualified_name(&l->a[i]);
            bool upper = false;
            for (const char *c = qn; *c && !upper; c++) upper = *c >= 'A' && *c <= 'Z';
            if (upper) continue;
        }
        /* (no namespace asked for: an attribute in none) */
        if (!ss->any_ns && l->a[i].ns) continue;
        const char *v = l->a[i].value.s ? l->a[i].value.s : "";
        size_t vn = l->a[i].value.len, n = ss->vlen;
        /* (the legacy case-insensitive ones: an HTML element's, in no namespace) */
        bool ci = ss->ci == 3 ? !exact && !l->a[i].ns : ss->ci;
        bool m = false;
        switch (ss->op) {
        case AO_EXISTS: m = true; break;
        case AO_EQ: m = vn == n && cmp_n(v, ss->val, n, ci); break;
        case AO_WORD: m = word_in(v, vn, ss->val, n, ci); break;
        case AO_DASH: m = (vn == n && cmp_n(v, ss->val, n, ci)) || (vn > n && cmp_n(v, ss->val, n, ci) && v[n] == '-'); break;
        case AO_PREFIX: m = n && vn >= n && cmp_n(v, ss->val, n, ci); break;
        case AO_SUFFIX: m = n && vn >= n && cmp_n(v + vn - n, ss->val, n, ci); break;
        case AO_SUBSTR:
            for (size_t k = 0; n && k + n <= vn && !m; k++) m = cmp_n(v + k, ss->val, n, ci);
            break;
        case AO_TOKPREFIX:
            /* (a class prefix: some token that starts so) */
            for (size_t k = 0; k < vn && !m; ) {
                while (k < vn && (v[k] == ' ' || v[k] == '\t' || v[k] == '\n' || v[k] == '\r' || v[k] == '\f')) k++;
                size_t st = k;
                while (k < vn && !(v[k] == ' ' || v[k] == '\t' || v[k] == '\n' || v[k] == '\r' || v[k] == '\f')) k++;
                m = k - st > n && n && v[st + n] != '-' && cmp_n(v + st, ss->val, n, ci);
            }
            break;
        }
        /* (any namespace's: the next one may) */
        if (m || !ss->any_ns) return m;
    }
    return false;
}

static CssAtom g_a_id, g_a_class, g_a_checked, g_a_disabled, g_a_href, g_a_lang;
/* (the tags :link and :enabled look at) */
static CssAtom g_t_a, g_t_area, g_t_form[7], g_t_legend;

static void atoms_init(void) {
    if (g_a_id) return;
    css_atom_reserve(16);
    g_t_legend = css_atom_intern("legend", 6);
    g_a_id = css_atom_intern("id", 2);
    g_a_class = css_atom_intern("class", 5);
    g_a_checked = css_atom_intern("checked", 7);
    g_a_disabled = css_atom_intern("disabled", 8);
    g_a_href = css_atom_intern("href", 4);
    g_a_lang = css_atom_intern("lang", 4);
    g_t_a = css_atom_intern("a", 1);
    g_t_area = css_atom_intern("area", 4);
    static const char *const form[7] = { "button", "input", "select", "textarea", "optgroup", "option", "fieldset" };
    for (int i = 0; i < 7; i++) g_t_form[i] = css_atom_intern(form[i], strlen(form[i]));
}

/* The fragment :target matches (dom_query_set_target). */
static struct { char *s; uint32_t n; } g_target;

void dom_query_set_target(const char *frag, size_t n) {
    free(g_target.s);
    g_target.s = n ? strndup(frag, n) : NULL;
    g_target.n = (uint32_t)n;
}

int (*g_dom_query_validity)(NodeId el);
int (*g_dom_query_formstate)(NodeId el);   /* (bits: 1 placeholder shown, 2 default, 4 in range, 8 out of range) */

/* Whether s is a form control :enabled and :disabled apply to. */
static bool form_control(uint32_t s) {
    if (g_dom.ns[s] != DOM_NS_HTML) return false;
    for (int i = 0; i < 7; i++) if (g_dom.tag[s] == g_t_form[i]) return true;
    return false;
}

static const char *attr_val(uint32_t s, CssAtom a, uint32_t *len);

/* A form control's "disabled" (HTML 4.10.18.5): its disabled attribute; in
 * a disabled fieldset, not in that fieldset's first legend; an option in a
 * disabled optgroup.  (g_t_form: button input select textarea optgroup
 * option fieldset) */
static bool control_disabled(uint32_t s) {
    uint32_t n;
    if (attr_val(s, g_a_disabled, &n)) return true;
    uint32_t p = g_dom.parent[s];
    if (g_dom.tag[s] == g_t_form[5])
        return p && g_dom.kind[p] == DOM_K_ELEMENT && g_dom.tag[p] == g_t_form[4] && attr_val(p, g_a_disabled, &n);
    if (g_dom.tag[s] == g_t_form[4]) return false;
    for (uint32_t child = s, x = p; x && g_dom.kind[x] == DOM_K_ELEMENT; child = x, x = g_dom.parent[x]) {
        if (g_dom.tag[x] != g_t_form[6] || g_dom.ns[x] != DOM_NS_HTML || !attr_val(x, g_a_disabled, &n)) continue;
        /* (the fieldset's first legend child: not disabled by it) */
        uint32_t legend = 0;
        for (uint32_t c = g_dom.first[x]; c && !legend; c = g_dom.next[c])
            if (g_dom.kind[c] == DOM_K_ELEMENT && g_dom.tag[c] == g_t_legend && g_dom.ns[c] == DOM_NS_HTML) legend = c;
        if (!legend || child != legend) return true;
    }
    return false;
}

/* Each form control's, form's and fieldset's validity, as :valid and :invalid
 * match it in style (they can't ask script while styling); what changed is
 * restyled. Only when some style asks (g_css_saw_validity). */
static uint8_t *g_validity;
static uint32_t g_validity_cap;
bool g_css_saw_validity;
void dom_validity_refresh(void)
{
    if (!g_css_saw_validity || !g_dom_query_validity) return;
    atoms_init();
    if (g_validity_cap < g_dom.count) {
        uint8_t *nv = (uint8_t *)realloc(g_validity, g_dom.count);
        if (!nv) return;
        memset(nv + g_validity_cap, 0, g_dom.count - g_validity_cap);
        g_validity = nv;
        g_validity_cap = g_dom.count;
    }
    for (uint32_t s = 1; s < g_dom.count; s++) {
        if (g_dom.kind[s] != DOM_K_ELEMENT || g_dom.ns[s] != DOM_NS_HTML) continue;
        uint8_t v = 0;
        static CssAtom t_form;
        if (!t_form) { css_atom_reserve(1); t_form = css_atom_intern("form", 4); }
        if (g_dom.tag[s] == t_form || g_dom.tag[s] == g_t_form[6]) {
            v = 1;
            for (uint32_t x = g_dom.first[s]; x && v == 1; x = dom_next_in_subtree(x, s))
                if (g_dom.kind[x] == DOM_K_ELEMENT && form_control(x) && g_dom_query_validity(dom_id_of(x)) == 2) v = 2;
        } else if (form_control(s)) {
            v = (uint8_t)g_dom_query_validity(dom_id_of(s));
            extern int (*g_dom_query_formstate)(NodeId el);
            /* (and what else its value says: placeholder shown, default, in or out of range) */
            if (g_dom_query_formstate) v |= (uint8_t)(g_dom_query_formstate(dom_id_of(s)) << 2);
        } else continue;
        if (v != g_validity[s]) {
            g_validity[s] = v;
            dom_mark_dirty(dom_id_of(s), DOMF_DIRTY_ATTR);
        }
    }
}

/* (a slot: does anything flatten into it? html_dom.c, which knows attributes) */
bool (*g_dom_slot_flattened)(NodeId el, int depth);

uint32_t dom_element_state(NodeId el)
{
    if (!dom_valid(el)) return 0;
    atoms_init();
    uint32_t s = dom_slot(el), st = g_dom.state[s], n;
    if (g_dom.kind[s] == DOM_K_ELEMENT && g_dom_slot_flattened && !strcmp(dom_tag_name(el), "slot") && g_dom_slot_flattened(el, 0)) st |= DOM_ST_HAS_SLOTTED;
    if (s < g_dom_top_cap && g_dom_top[s]) {
        if (g_dom_top[s] & 1) st |= DOM_ST_MODAL;
        if (g_dom_top[s] & 2) st |= DOM_ST_POPOVER_OPEN;
    }
    if (g_validity && s < g_validity_cap && g_validity[s]) {
        uint8_t v = g_validity[s];
        if ((v & 3) == 1) st |= DOM_ST_VALID;
        else if ((v & 3) == 2) st |= DOM_ST_INVALID;
        if (v & 4) st |= DOM_ST_PLACEHOLDER_SHOWN;
        if (v & 8) st |= DOM_ST_DEFAULT;
        if (v & 16) st |= DOM_ST_IN_RANGE;
        if (v & 32) st |= DOM_ST_OUT_OF_RANGE;
    }
    if (g_dom.kind[s] != DOM_K_ELEMENT || g_dom.ns[s] != DOM_NS_HTML) return st;
    /* (each element, each time it is read for style: by atom, not by name) */
    static CssAtom a_ce, t_video, t_audio;
    if (!a_ce) {
        css_atom_reserve(3);
        a_ce = css_atom_intern("contenteditable", 15);
        t_video = css_atom_intern("video", 5);
        t_audio = css_atom_intern("audio", 5);
    }
    uint32_t cl = 0;
    const char *ce = g_dom.attrs[s].n ? attr_val(s, a_ce, &cl) : NULL;
    bool rw = ce && !(cl == 5 && !strncasecmp(ce, "false", 5));
    if (form_control(s)) {
        bool off = control_disabled(s);
        st |= off ? DOM_ST_DISABLED : DOM_ST_ENABLED;
        CssAtom t = g_dom.tag[s];
        bool input = t == g_t_form[1], textarea = t == g_t_form[3], select = t == g_t_form[2], option = t == g_t_form[5];
        const char *ty = input ? dom_get_attr_str(el, "type") : NULL;
        if (input || option) {
            /* (the attribute until the element's own checkedness is set) */
            bool has = option ? dom_get_attr_str(el, "selected") != NULL
                              : ty && (!strcasecmp(ty, "checkbox") || !strcasecmp(ty, "radio")) && attr_val(s, g_a_checked, &n);
            if (!(st & DOM_ST_CHECK_SET)) st = has ? st | DOM_ST_CHECKED : st & ~(uint32_t)DOM_ST_CHECKED;
        }
        if (input || select || textarea) st |= dom_get_attr_str(el, "required") ? DOM_ST_REQUIRED : DOM_ST_OPTIONAL;
        if (textarea || input) {
            static const char *const text[] = { "text", "search", "url", "tel", "email", "password", "date", "month",
                                                "week", "time", "datetime-local", "number" };
            bool editable = textarea || !ty;
            for (size_t i = 0; !editable && i < sizeof text / sizeof text[0]; i++) editable = !strcasecmp(ty, text[i]);
            /* (an unknown type is a text field) */
            if (!editable) {
                static const char *const other[] = { "hidden", "range", "color", "checkbox", "radio", "file", "submit", "image", "reset", "button" };
                editable = true;
                for (size_t i = 0; editable && i < sizeof other / sizeof other[0]; i++) editable = strcasecmp(ty, other[i]) != 0;
            }
            if (editable && !off && !dom_get_attr_str(el, "readonly")) rw = true;
        }
    }
    st |= rw ? DOM_ST_READ_WRITE : DOM_ST_READ_ONLY;
    if (g_dom.tag[s] == t_video || g_dom.tag[s] == t_audio) {
        if (!(st & DOM_ST_PLAYING)) st |= DOM_ST_PAUSED;
        if (!(st & DOM_ST_MUTE_SET)) st = dom_get_attr_str(el, "muted") ? st | DOM_ST_MUTED : st & ~(uint32_t)DOM_ST_MUTED;
    }
    return st;
}

/* :lang(want): the language of s (its nearest lang="" (xml:lang)) is want or
 * starts with want and a '-', ASCII case-insensitively. */
static bool lang_ok(uint32_t s, const char *want, uint32_t wn) {
    for (uint32_t x = s; x && g_dom.kind[x] == DOM_K_ELEMENT; x = g_dom.parent[x]) {
        const DomAttrList *l = &g_dom.attrs[x];
        const DomAttr *found = NULL;
        for (int i = 0; i < l->n && !found; i++) {
            const char *q = dom_attr_qualified_name(&l->a[i]);
            if ((l->a[i].ns == DOM_NS_XML && !strcmp(q, "xml:lang")) || (!l->a[i].ns && l->a[i].name == g_a_lang)) found = &l->a[i];
        }
        if (!found) continue;
        uint32_t n = found->value.len;
        const char *v = found->value.s ? found->value.s : "";
        if (n < wn || strncasecmp(v, want, wn) != 0) return false;
        return n == wn || v[wn] == '-';
    }
    return false;
}

static const char *attr_val(uint32_t s, CssAtom a, uint32_t *len);

/* ---- an element's directionality (HTML 3.2.6.5): 0 ltr, 1 rtl ---- */
const char *(*g_dom_query_value)(NodeId el);

/* The first strong character's: 0 left to right, 1 right to left, -1 none. */
static int bidi_first_strong(const char *t, size_t n)
{
    for (size_t i = 0; i < n; ) {
        unsigned char c = (unsigned char)t[i];
        uint32_t cp = c;
        size_t l = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (l > 1 && i + l <= n) {
            cp = c & (0xFF >> (l + 1));
            for (size_t k = 1; k < l; k++) cp = (cp << 6) | ((unsigned char)t[i + k] & 0x3F);
        }
        i += l;
        if ((cp >= 0x590 && cp <= 0x8FF) || (cp >= 0xFB1D && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF) ||
            (cp >= 0x10800 && cp <= 0x10FFF) || (cp >= 0x1E800 && cp <= 0x1EFFF) || cp == 0x200F) {
            /* (Arabic's digits and marks are weak) */
            if ((cp >= 0x591 && cp <= 0x5BD) || (cp >= 0x660 && cp <= 0x669) || (cp >= 0x6F0 && cp <= 0x6F9) || (cp >= 0x610 && cp <= 0x61A) || (cp >= 0x64B && cp <= 0x65F)) continue;
            return 1;
        }
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == 0x200E) return 0;
        if (cp >= 0xC0 && cp != 0xD7 && cp != 0xF7 && !(cp >= 0x2000 && cp <= 0x2BFF) && !(cp >= 0x3000 && cp <= 0x303F) && !(cp >= 0x300 && cp <= 0x36F)) return 0;
    }
    return -1;
}

static const char *dir_attr(uint32_t s)
{
    static CssAtom a;
    if (!a) { css_atom_reserve(1); a = css_atom_intern("dir", 3); }
    uint32_t n = 0;
    const char *v = g_dom.ns[s] == DOM_NS_HTML ? attr_val(s, a, &n) : NULL;
    if (!v) return NULL;
    if (n == 3 && !strncasecmp(v, "ltr", 3)) return "ltr";
    if (n == 3 && !strncasecmp(v, "rtl", 3)) return "rtl";
    if (n == 4 && !strncasecmp(v, "auto", 4)) return "auto";
    return NULL;
}

static bool tag_is(uint32_t s, const char *t)
{
    
    const char *q = css_atom_str(g_dom.tag[s]);
    return q && !strcasecmp(q, t);
}

static int auto_dir(uint32_t s)
{
    int found = -1;
    uint32_t x = g_dom.first[s];
    while (x && found < 0) {
        bool skip = false;
        if (g_dom.kind[x] == DOM_K_ELEMENT) {
            skip = (g_dom.ns[x] == DOM_NS_HTML && (tag_is(x, "bdi") || tag_is(x, "script") || tag_is(x, "style") || tag_is(x, "textarea"))) || dir_attr(x) != NULL;
        } else if (g_dom.kind[x] == DOM_K_TEXT) {
            uint32_t n = 0;
            const char *t = dom_text(dom_id_of(x), &n);
            if (t) found = bidi_first_strong(t, n);
        }
        if (skip) {
            /* (past it, not into it) */
            uint32_t nx = g_dom.next[x];
            for (uint32_t u = x; !nx && u != s; ) {
                u = g_dom.parent[u];
                if (u == s) break;
                nx = g_dom.next[u];
            }
            x = nx;
        } else x = dom_next_in_subtree(x, s);
    }
    return found;
}

static int elem_dir(uint32_t s)
{
    for (uint32_t x = s; x; ) {
        if (g_dom.kind[x] != DOM_K_ELEMENT) {
            /* (a shadow root: its host's) */
            uint32_t h = g_dom.host[x];
            if (!h) return 0;
            x = h;
            continue;
        }
        const char *d = dir_attr(x);
        if (d && d[0] == 'l') return 0;
        if (d && d[0] == 'r') return 1;
        bool input = g_dom.ns[x] == DOM_NS_HTML && tag_is(x, "input");
        if (d) {
            /* auto: a control by its value, the rest by what they hold */
            int got = -1;
            if ((input || (g_dom.ns[x] == DOM_NS_HTML && tag_is(x, "textarea"))) && g_dom_query_value) {
                const char *v = g_dom_query_value(dom_id_of(x));
                if (v) got = bidi_first_strong(v, strlen(v));
            } else got = auto_dir(x);
            return got > 0;
        }
        /* (a phone number runs left to right, wherever it is) */
        if (input) {
            const char *ty = dom_get_attr_str(dom_id_of(x), "type");
            if (ty && !strcasecmp(ty, "tel")) return 0;
        }
        /* (bdi with no dir: as auto) */
        if (g_dom.ns[x] == DOM_NS_HTML && tag_is(x, "bdi")) return auto_dir(x) > 0;
        x = g_dom.parent[x];
    }
    return 0;
}

static const char *attr_val(uint32_t s, CssAtom a, uint32_t *len) {
    DomAttrList *l = &g_dom.attrs[s];
    for (int i = 0; i < l->n; i++)
        if (l->a[i].name == a) { *len = l->a[i].value.len; return l->a[i].value.s ? l->a[i].value.s : ""; }
    return NULL;
}

static bool nth_ok(int a, int b, int pos) {
    /* pos is 1-based: exists n >= 0 with a*n + b == pos */
    if (a == 0) return pos == b;
    int d = pos - b;
    if (a > 0) return d >= 0 && d % a == 0;
    return d <= 0 && (-d) % (-a) == 0;
}

static bool list_matches(const SelList *l, uint32_t s, uint32_t scope);
static uint32_t g_has_anchor;

static bool simple_ok(uint32_t s, const SelSimple *ss, uint32_t scope) {
    switch (ss->kind) {
    case SS_TAG: {
        if (g_dom.tag[s] != ss->atom) return false;
        /* (an HTML element in an HTML document: any case; else as it's written) */
        NodeId id = dom_id_of(s);
        if (g_dom.ns[s] == DOM_NS_HTML && dom_is_html_document(dom_owner(id))) return true;
        const char *want = ss->raw ? ss->raw : css_atom_str(ss->atom);
        return !want || strcmp(dom_local_name(id), want) == 0;
    }
    case SS_UNIVERSAL: return true;
    case SS_ID: {
        uint32_t n = 0;
        const char *v = attr_val(s, g_a_id, &n);
        return v && n == ss->vlen && memcmp(v, ss->val, n) == 0;
    }
    case SS_CLASS: {
        /* (the element's class atoms: its bloom filter, then the atoms; an
         * upper-case letter on either side, the string exactly) */
        if (!ss->atom || !(g_dom.cls_bloom[s] & DOM_CLS_BIT(ss->atom))) return false;
        const CssAtom *c = dom_cls_atoms(s);
        uint32_t nc = dom_cls_count(s);
        for (uint32_t k = 0; k < nc; k++) {
            if (c[k] != ss->atom) continue;
            if (ss->ci != 2 && !dom_cls_upper(s)) return true;
            /* (quirks mode: as they are) */
            NodeId cd = dom_owner(dom_id_of(s));
            if (dom_is_html_document(cd)) {
                NodeId cc = dom_first_child(cd);
                while (cc && dom_kind(cc) != DOM_K_DOCTYPE) cc = dom_next_sibling(cc);
                if (!cc) return true;
            }
            uint32_t n = 0;
            const char *v = attr_val(s, g_a_class, &n);
            return v && word_in(v, n, ss->val, ss->vlen, false);
        }
        return false;
    }
    case SS_ATTR: return attr_ok(s, ss);
    case SS_ANCHOR: return s == g_has_anchor;
    case SS_HAS: {
        uint32_t was = g_has_anchor;
        bool found = false;
        g_has_anchor = s;
        for (uint32_t d = g_dom.first[s] ? dom_next_in_subtree(s, s) : 0; d && !found; d = dom_next_in_subtree(d, s))
            found = g_dom.kind[d] == DOM_K_ELEMENT && list_matches(ss->sub, d, scope);
        /* (+ and ~ look at what follows) */
        if (ss->a)
            for (uint32_t x = g_dom.next[s]; x && !found; x = g_dom.next[x])
                for (uint32_t d = x; d && !found; d = dom_next_in_subtree(d, x))
                    found = g_dom.kind[d] == DOM_K_ELEMENT && list_matches(ss->sub, d, scope);
        g_has_anchor = was;
        return found;
    }
    case SS_NOT: return !list_matches(ss->sub, s, scope);
    case SS_IS: return list_matches(ss->sub, s, scope);
    case SS_FIRST_CHILD: case SS_LAST_CHILD: case SS_ONLY_CHILD:
    case SS_NTH_CHILD: case SS_NTH_LAST_CHILD: case SS_NTH_OF_TYPE: case SS_NTH_LAST_OF_TYPE:
    case SS_FIRST_OF_TYPE: case SS_LAST_OF_TYPE: case SS_ONLY_OF_TYPE: {
        /* (no parent: the only child there is) */
        bool of_type = ss->kind == SS_FIRST_OF_TYPE || ss->kind == SS_LAST_OF_TYPE || ss->kind == SS_ONLY_OF_TYPE ||
                       ss->kind == SS_NTH_OF_TYPE || ss->kind == SS_NTH_LAST_OF_TYPE;
        int before = 0, after = 0;
        for (uint32_t x = g_dom.prev[s]; x; x = g_dom.prev[x])
            if (g_dom.kind[x] == DOM_K_ELEMENT && (!of_type || g_dom.tag[x] == g_dom.tag[s])) before++;
        for (uint32_t x = g_dom.next[s]; x; x = g_dom.next[x])
            if (g_dom.kind[x] == DOM_K_ELEMENT && (!of_type || g_dom.tag[x] == g_dom.tag[s])) after++;
        switch (ss->kind) {
        case SS_FIRST_CHILD: case SS_FIRST_OF_TYPE: return before == 0;
        case SS_LAST_CHILD: case SS_LAST_OF_TYPE: return after == 0;
        case SS_ONLY_CHILD: case SS_ONLY_OF_TYPE: return before == 0 && after == 0;
        case SS_NTH_CHILD: case SS_NTH_OF_TYPE: return nth_ok(ss->a, ss->b, before + 1);
        default: return nth_ok(ss->a, ss->b, after + 1);
        }
    }
    case SS_EMPTY:
        for (uint32_t c = g_dom.first[s]; c; c = g_dom.next[c])
            if (g_dom.kind[c] == DOM_K_ELEMENT || ((g_dom.kind[c] == DOM_K_TEXT || g_dom.kind[c] == DOM_K_CDATA) && g_dom.text[c].len)) return false;
        return true;
    case SS_ROOT: return g_dom.parent[s] && g_dom.kind[g_dom.parent[s]] == DOM_K_DOCUMENT;
    case SS_SCOPE: return s == scope;
    case SS_STATE: return (dom_element_state(dom_id_of(s)) & (uint32_t)ss->a) == (uint32_t)ss->a;
    case SS_DISABLED: return form_control(s) && control_disabled(s);
    case SS_ENABLED: return form_control(s) && !control_disabled(s);
    case SS_NO_NS: return g_dom.ns[s] == DOM_NS_NULL;
    case SS_LINK: {
        uint32_t n;
        return g_dom.ns[s] == DOM_NS_HTML && (g_dom.tag[s] == g_t_a || g_dom.tag[s] == g_t_area) &&
               attr_val(s, g_a_href, &n) != NULL;
    }
    case SS_TARGET: {
        uint32_t n = 0;
        const char *v = g_target.s ? attr_val(s, g_a_id, &n) : NULL;
        if (!v || n != g_target.n || memcmp(v, g_target.s, n)) return false;
        /* (the document's indicated part: an element in a document) */
        uint32_t r = s;
        while (g_dom.parent[r]) r = g_dom.parent[r];
        return g_dom.kind[r] == DOM_K_DOCUMENT;
    }
    case SS_LANG: return lang_ok(s, ss->val, ss->vlen);
    case SS_DIR: return elem_dir(s) == ss->a;
    case SS_HEADING: {
        if (g_dom.ns[s] != DOM_NS_HTML) return false;
        static uint32_t h[7];
        if (!h[1]) for (int i = 1; i <= 6; i++) { char t[3] = { 'h', (char)('0' + i), 0 }; h[i] = css_atom_intern(t, 2); }
        static CssAtom off_a, reset_a;
        if (!off_a) { off_a = css_atom_intern("headingoffset", 13); reset_a = css_atom_intern("headingreset", 12); }
        for (int i = 1; i <= 6; i++) {
            if (g_dom.tag[s] != h[i]) continue;
            /* (its level: offsets of it and what's above, up to a reset; at most 9) */
            int lv = i;
            for (uint32_t x = s; x && g_dom.kind[x] == DOM_K_ELEMENT && lv < 9; x = g_dom.parent[x]) {
                uint32_t l = 0;
                const char *v = attr_val(x, off_a, &l);
                if (v && l && l < 8) {
                    char b[8];
                    memcpy(b, v, l);
                    b[l] = 0;
                    char *e = NULL;
                    long o = strtol(b, &e, 10);
                    if (e != b && o > 0) lv += o > 8 ? 8 : (int)o;
                }
                if (attr_val(x, reset_a, &l)) break;
            }
            if (lv > 9) lv = 9;
            return (ss->a >> lv) & 1;
        }
        return false;
    }
    case SS_VALID:
    case SS_INVALID: {
        if (g_dom.ns[s] != DOM_NS_HTML || !g_dom_query_validity) return false;
        /* (a form or fieldset: by the controls in it, HTML 4.16.3) */
        if (g_dom.tag[s] == ss->atom || g_dom.tag[s] == g_t_form[6]) {
            for (uint32_t x = g_dom.first[s]; x; x = dom_next_in_subtree(x, s))
                if (g_dom.kind[x] == DOM_K_ELEMENT && form_control(x) && g_dom_query_validity(dom_id_of(x)) == 2)
                    return ss->kind == SS_INVALID;
            return ss->kind == SS_VALID;
        }
        if (!form_control(s)) return false;
        int v = g_dom_query_validity(dom_id_of(s));
        return v == (ss->kind == SS_VALID ? 1 : 2);
    }
    case SS_ANY: return true;
    default: return false;
    }
}

static bool compound_ok(uint32_t s, const SelCompound *c, uint32_t scope) {
    if (g_dom.kind[s] != DOM_K_ELEMENT) return false;
    for (int i = 0; i < c->n; i++) if (!simple_ok(s, &c->s[i], scope)) return false;
    return true;
}

static uint32_t prev_element(uint32_t s) {
    for (uint32_t x = g_dom.prev[s]; x; x = g_dom.prev[x]) if (g_dom.kind[x] == DOM_K_ELEMENT) return x;
    return 0;
}

/* The leftmost compound, all :host: what a shadow tree's root stands for. */
static bool host_at_left(const SelComplex *cx, int i) {
    if (i != 1 || cx->c[0].n == 0) return false;
    for (int k = 0; k < cx->c[0].n; k++) if (cx->c[0].s[k].kind != SS_HOST) return false;
    return true;
}

/* Does compound i (and everything to its left) match at s? */
static bool complex_at(const SelComplex *cx, int i, uint32_t s, uint32_t scope) {
    if (!compound_ok(s, &cx->c[i], scope)) return false;
    if (i == 0) return true;
    switch (cx->c[i].comb) {
    case '>': {
        uint32_t p = g_dom.parent[s];
        if (p && (g_dom.flags[p] & DOMF_SHADOW_ROOT)) return host_at_left(cx, i);
        return p && complex_at(cx, i - 1, p, scope);
    }
    case '+': {
        uint32_t p = prev_element(s);
        return p && complex_at(cx, i - 1, p, scope);
    }
    case '~':
        for (uint32_t p = prev_element(s); p; p = prev_element(p))
            if (complex_at(cx, i - 1, p, scope)) return true;
        return false;
    default:
        for (uint32_t p = g_dom.parent[s]; p; p = g_dom.parent[p]) {
            if (g_dom.flags[p] & DOMF_SHADOW_ROOT) return host_at_left(cx, i);
            if (complex_at(cx, i - 1, p, scope)) return true;
        }
        return false;
    }
}

static bool list_matches(const SelList *l, uint32_t s, uint32_t scope) {
    if (!l || !l->ok) return false;
    for (int i = 0; i < l->n; i++)
        if (complex_at(&l->x[i], l->x[i].n - 1, s, scope)) return true;
    return false;
}

bool dom_query_matches(NodeId el, const char *sel, size_t len) {
    return dom_query_matches_scoped(el, sel, len, el);
}

bool dom_query_matches_scoped(NodeId el, const char *sel, size_t len, NodeId scope) {
    if (!dom_valid(el) || dom_kind(el) != DOM_K_ELEMENT) return false;
    atoms_init();
    SelList *l = sel_get(sel, len);
    return list_matches(l, dom_slot(el), dom_valid(scope) ? dom_slot(scope) : 0);
}

/* ============================================================================
 * Indexes (counting sort over the document's preorder)
 * ============================================================================ */

typedef struct DomIndex {
    uint32_t root;              /* document slot indexed (0 = none) */
    uint8_t root_gen;
    uint64_t sv, cv;            /* store versions at build time */
    uint32_t *order;            /* preorder slots */
    uint32_t n;
    uint32_t *pos;              /* by slot: preorder position + 1 (0 = not indexed) */
    uint32_t pos_cap;
    uint32_t *end;              /* by position: end of the subtree (exclusive) */
    /* tag: atom -> [off, off+cnt) into tag_list (positions) */
    uint32_t natoms;
    uint32_t *tag_off, *tag_cnt, *tag_list;
    uint32_t *cls_off, *cls_cnt, *cls_list;
    uint32_t ncls;
    /* By position: the element's class atoms, cpool[cpos[i] .. cpos[i+1]),
     * parsed once per build for the passes that read them. */
    uint32_t *cpos;
    CssAtom *cpool;
    uint32_t cpos_cap, cpool_cap;
} DomIndex;

static DomIndex g_ix;

/* Class tokens of element s as atoms (lower-cased; callers re-check case),
 * as the store keeps them. */
static uint32_t class_atoms(uint32_t s, CssAtom *out, uint32_t max) {
    uint32_t n = dom_cls_count(s);
    if (n > max) n = max;
    memcpy(out, dom_cls_atoms(s), n * sizeof(CssAtom));
    return n;
}

typedef struct IxJob {
    DomIndex *ix;
    uint32_t nranges;
    uint32_t *tag_cnt;          /* per range x natoms (counts, then offsets) */
    uint32_t *cls_cnt;
} IxJob;

#define IX_RANGE 8192u
#define IX_MAX_CLASSES 64u

static void ix_count_job(Job *job) {
    IxJob *J = job->ctx;
    DomIndex *ix = J->ix;
    uint32_t r = job->a, b = r * IX_RANGE, e = b + IX_RANGE < ix->n ? b + IX_RANGE : ix->n;
    uint32_t *tc = J->tag_cnt + (size_t)r * ix->natoms, *cc = J->cls_cnt + (size_t)r * ix->natoms;
    for (uint32_t i = b; i < e; i++) {
        uint32_t s = ix->order[i];
        if (g_dom.kind[s] != DOM_K_ELEMENT) continue;
        if (g_dom.tag[s] < ix->natoms) tc[g_dom.tag[s]]++;
        for (uint32_t k = ix->cpos[i]; k < ix->cpos[i + 1]; k++)
            if (ix->cpool[k] < ix->natoms) cc[ix->cpool[k]]++;
    }
}

static void ix_scatter_job(Job *job) {
    IxJob *J = job->ctx;
    DomIndex *ix = J->ix;
    uint32_t r = job->a, b = r * IX_RANGE, e = b + IX_RANGE < ix->n ? b + IX_RANGE : ix->n;
    uint32_t *to = J->tag_cnt + (size_t)r * ix->natoms, *co = J->cls_cnt + (size_t)r * ix->natoms;
    for (uint32_t i = b; i < e; i++) {
        uint32_t s = ix->order[i];
        if (g_dom.kind[s] != DOM_K_ELEMENT) continue;
        if (g_dom.tag[s] < ix->natoms) ix->tag_list[to[g_dom.tag[s]]++] = i;
        for (uint32_t k = ix->cpos[i]; k < ix->cpos[i + 1]; k++)
            if (ix->cpool[k] < ix->natoms) ix->cls_list[co[ix->cpool[k]]++] = i;
    }
}

static void ix_root_job(Job *job) {
    IxJob *J = job->ctx;
    for (uint32_t r = 0; r < J->nranges; r++) job_spawn(job_new(ix_count_job, job, J, r, 0));
}

static void ix_scatter_root_job(Job *job) {
    IxJob *J = job->ctx;
    for (uint32_t r = 0; r < J->nranges; r++) job_spawn(job_new(ix_scatter_job, job, J, r, 0));
}

static void index_build(uint32_t root) {
    double t0 = now_ms();
    atoms_init();
    DomIndex *ix = &g_ix;
    ix->root = root;
    ix->root_gen = g_dom.gen[root];
    ix->sv = g_dom.structure_version;
    ix->cv = g_dom.class_version;
    /* Preorder of the document's light tree. */
    ix->n = 0;
    uint32_t cap = g_dom.live + 1;
    ix->order = realloc(ix->order, cap * sizeof(uint32_t));
    ix->end = realloc(ix->end, cap * sizeof(uint32_t));
    if (ix->pos_cap < g_dom.count) {
        ix->pos = realloc(ix->pos, g_dom.cap * sizeof(uint32_t));
        ix->pos_cap = g_dom.cap;
    }
    memset(ix->pos, 0, ix->pos_cap * sizeof(uint32_t));
    {
        uint32_t *stack = malloc(256 * sizeof(uint32_t)), sp = 0, scap = 256;
        for (uint32_t s = root; s; ) {
            uint32_t i = ix->n++;
            ix->order[i] = s;
            ix->pos[s] = i + 1;
            if (g_dom.first[s]) {
                if (sp == scap) { scap *= 2; stack = realloc(stack, scap * sizeof(uint32_t)); }
                stack[sp++] = i;
                s = g_dom.first[s];
                continue;
            }
            ix->end[i] = i + 1;
            while (s && s != root && !g_dom.next[s]) {
                s = g_dom.parent[s];
                if (s && sp) { uint32_t pi = stack[--sp]; ix->end[pi] = ix->n; }
            }
            if (!s || s == root) break;
            s = g_dom.next[s];
        }
        while (sp) { uint32_t pi = stack[--sp]; ix->end[pi] = ix->n; }
        if (ix->n) ix->end[0] = ix->n;
        free(stack);
    }
    /* Class atoms of every element, parsed once (which also makes every
     * class name an atom before the jobs run; they only read). */
    if (ix->cpos_cap < ix->n + 1) {
        ix->cpos_cap = ix->n + 1;
        ix->cpos = realloc(ix->cpos, ix->cpos_cap * sizeof(uint32_t));
    }
    {
        CssAtom tmp[IX_MAX_CLASSES];
        uint32_t used = 0;
        for (uint32_t i = 0; i < ix->n; i++) {
            ix->cpos[i] = used;
            if (g_dom.kind[ix->order[i]] != DOM_K_ELEMENT) continue;
            uint32_t nc = class_atoms(ix->order[i], tmp, IX_MAX_CLASSES);
            if (used + nc > ix->cpool_cap) {
                ix->cpool_cap = (used + nc) * 2 + 256;
                ix->cpool = realloc(ix->cpool, ix->cpool_cap * sizeof(CssAtom));
            }
            memcpy(ix->cpool + used, tmp, nc * sizeof(CssAtom));
            used += nc;
        }
        ix->cpos[ix->n] = used;
    }
    ix->natoms = (uint32_t)css_atom_count() + 2;
    uint32_t nr = (ix->n + IX_RANGE - 1) / IX_RANGE;
    if (!nr) nr = 1;
    IxJob J = { ix, nr, calloc((size_t)nr * ix->natoms, sizeof(uint32_t)), calloc((size_t)nr * ix->natoms, sizeof(uint32_t)) };
    bool par = nr > 1 && job_thread_count() > 1;
    if (par) job_graph_run(job_new(ix_root_job, NULL, &J, 0, 0));
    else for (uint32_t r = 0; r < nr; r++) { Job j = { 0 }; j.ctx = &J; j.a = r; ix_count_job(&j); }
    /* Prefix sums: per atom, ranges in order (so lists come out sorted). */
    ix->tag_off = realloc(ix->tag_off, ix->natoms * sizeof(uint32_t));
    ix->tag_cnt = realloc(ix->tag_cnt, ix->natoms * sizeof(uint32_t));
    ix->cls_off = realloc(ix->cls_off, ix->natoms * sizeof(uint32_t));
    ix->cls_cnt = realloc(ix->cls_cnt, ix->natoms * sizeof(uint32_t));
    uint32_t toff = 0, coff = 0;
    for (uint32_t a = 0; a < ix->natoms; a++) {
        ix->tag_off[a] = toff;
        ix->cls_off[a] = coff;
        for (uint32_t r = 0; r < nr; r++) {
            uint32_t *t = &J.tag_cnt[(size_t)r * ix->natoms + a], *c = &J.cls_cnt[(size_t)r * ix->natoms + a];
            uint32_t tn = *t, cn = *c;
            *t = toff; *c = coff;
            toff += tn; coff += cn;
        }
        ix->tag_cnt[a] = toff - ix->tag_off[a];
        ix->cls_cnt[a] = coff - ix->cls_off[a];
    }
    ix->tag_list = realloc(ix->tag_list, (toff + 1) * sizeof(uint32_t));
    ix->cls_list = realloc(ix->cls_list, (coff + 1) * sizeof(uint32_t));
    if (par) job_graph_run(job_new(ix_scatter_root_job, NULL, &J, 0, 0));
    else for (uint32_t r = 0; r < nr; r++) { Job j = { 0 }; j.ctx = &J; j.a = r; ix_scatter_job(&j); }
    free(J.tag_cnt);
    free(J.cls_cnt);
    g_qstats.index_builds++;
    g_qstats.last_build_ms = now_ms() - t0;
    if (cyber_getenv("DOM_LOG"))
        fprintf(stderr, "[dom-index] %u nodes, %u ranges%s, %.3f ms\n", ix->n, nr, par ? " (parallel)" : "", g_qstats.last_build_ms);
}

/* The index of document `doc`, rebuilt when the store's versions moved. */
static DomIndex *index_for(NodeId doc) {
    if (!dom_valid(doc)) return NULL;
    uint32_t r = dom_slot(doc);
    DomIndex *ix = &g_ix;
    if (ix->root == r && ix->root_gen == g_dom.gen[r] && ix->sv == g_dom.structure_version &&
        ix->cv == g_dom.class_version && ix->pos_cap >= g_dom.count)
        return ix;
    index_build(r);
    return ix;
}

/* Preorder position range of `scope` in the index, or false if outside. */
static bool index_range(DomIndex *ix, uint32_t scope, bool include_self, uint32_t *b, uint32_t *e) {
    if (scope >= ix->pos_cap || !ix->pos[scope]) return false;
    uint32_t p = ix->pos[scope] - 1;
    *b = include_self ? p : p + 1;
    *e = ix->end[p];
    return true;
}

static uint32_t lower_bound(const uint32_t *v, uint32_t n, uint32_t x) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (v[m] < x) lo = m + 1; else hi = m; }
    return lo;
}

/* The document a node's light tree belongs to (0 if none). */
static uint32_t doc_of(uint32_t s) {
    while (g_dom.parent[s]) s = g_dom.parent[s];
    return g_dom.kind[s] == DOM_K_DOCUMENT ? s : 0;
}

/* ============================================================================
 * Queries
 * ============================================================================ */

/* Pick the compound simple selector the index can serve. */
static int index_key(const SelCompound *c, int *kind, CssAtom *atom, const char **val, uint32_t *vlen) {
    int best = 0;
    for (int i = 0; i < c->n; i++) {
        const SelSimple *s = &c->s[i];
        if (s->kind == SS_ID) { *kind = SS_ID; *val = s->val; *vlen = s->vlen; return 3; }
        if (s->kind == SS_CLASS && best < 2) {
            CssAtom a = css_atom_find(s->val, s->vlen);
            *kind = SS_CLASS; *atom = a; *val = s->val; *vlen = s->vlen; best = 2;
        }
        if (s->kind == SS_TAG && best < 1) { *kind = SS_TAG; *atom = s->atom; best = 1; }
    }
    return best;
}

typedef struct QueryOut { DomNodeList *all; NodeId first; bool want_first; } QueryOut;

static bool emit(QueryOut *q, uint32_t s) {
    if (q->want_first) { q->first = dom_id_of(s); return true; }
    nl_push(q->all, dom_id_of(s));
    return false;
}

static void run_query(uint32_t scope, SelList *l, bool include_self, QueryOut *q) {
    if (!l || !l->ok || !l->n) return;
    atoms_init();
    /* Index path: one complex selector whose rightmost compound has a key,
     * and a scope inside the main document's light tree. */
    uint32_t doc = doc_of(scope);
    if (l->n == 1 && doc) {
        const SelComplex *cx = &l->x[0];
        int kind = -1; CssAtom atom = 0; const char *val = NULL; uint32_t vlen = 0;
        if (index_key(&cx->c[cx->n - 1], &kind, &atom, &val, &vlen) > 0) {
            if (kind == SS_ID) {
                /* Id: candidates from the incremental id index, checked for
                 * scope by walking up; several hits are put in document order. */
                CssAtom a = css_atom_find(val, vlen);
                uint32_t n = 0;
                const uint32_t *cand = dom_id_index_lookup(a, &n);
                uint32_t *hits = NULL, nh = 0;
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t s = cand[i];
                    if (s >= g_dom.count || g_dom.kind[s] != DOM_K_ELEMENT || g_dom.id_atom[s] != a) continue;
                    if (!include_self && s == scope) continue;
                    uint32_t x = s;
                    while (x && x != scope) x = g_dom.parent[x];
                    if (!x) continue;
                    if (!complex_at(cx, cx->n - 1, s, scope)) continue;
                    bool dup = false;
                    for (uint32_t k = 0; k < nh; k++) if (hits[k] == s) dup = true;
                    if (dup) continue;
                    hits = realloc(hits, (nh + 1) * sizeof(uint32_t));
                    hits[nh++] = s;
                }
                if (nh > 1) {
                    DomIndex *ix = index_for(dom_id_of(doc));
                    for (uint32_t i = 1; i < nh; i++)
                        for (uint32_t j = i; j > 0 && ix->pos[hits[j - 1]] > ix->pos[hits[j]]; j--) {
                            uint32_t t = hits[j]; hits[j] = hits[j - 1]; hits[j - 1] = t;
                        }
                }
                g_qstats.index_hits++;
                for (uint32_t i = 0; i < nh; i++) if (emit(q, hits[i])) break;
                free(hits);
                return;
            }
            DomIndex *ix = index_for(dom_id_of(doc));
            if (kind == SS_CLASS) {
                atom = css_atom_find(val, vlen);     /* (every class token is an atom) */
                if (!atom) return;
            }
            uint32_t b = 0, e = 0;
            if (!ix || !index_range(ix, scope, include_self, &b, &e)) goto scan;
            const uint32_t *list = NULL;
            uint32_t cnt = 0;
            if (atom < ix->natoms) {
                if (kind == SS_TAG) { list = ix->tag_list + ix->tag_off[atom]; cnt = ix->tag_cnt[atom]; }
                else { list = ix->cls_list + ix->cls_off[atom]; cnt = ix->cls_cnt[atom]; }
            }
            g_qstats.index_hits++;
            for (uint32_t i = lower_bound(list, cnt, b); i < cnt && list[i] < e; i++) {
                if (i && list[i] == list[i - 1]) continue;     /* repeated class token */
                uint32_t s = ix->order[list[i]];
                if (complex_at(cx, cx->n - 1, s, scope) && emit(q, s)) return;
            }
            return;
        }
    }
scan:
    g_qstats.scans++;
    {
        uint32_t s = include_self ? scope : g_dom.first[scope];
        if (!include_self && !s) return;
        for (; s; s = dom_next_in_subtree(s, scope)) {
            if (!include_self && s == scope) continue;
            if (g_dom.kind[s] == DOM_K_ELEMENT && list_matches(l, s, scope) && emit(q, s)) return;
        }
    }
}

NodeId dom_query_first(NodeId scope, const char *sel, size_t len, bool include_self) {
    if (!dom_valid(scope)) return DOM_NULL;
    SelList *l = sel_get(sel, len);
    QueryOut q = { NULL, DOM_NULL, true };
    run_query(dom_slot(scope), l, include_self, &q);
    return q.first;
}

DomNodeList dom_query_all(NodeId scope, const char *sel, size_t len, bool include_self) {
    DomNodeList out = { 0 };
    if (!dom_valid(scope)) return out;
    SelList *l = sel_get(sel, len);
    QueryOut q = { &out, DOM_NULL, false };
    run_query(dom_slot(scope), l, include_self, &q);
    return out;
}

NodeId dom_query_by_id(NodeId doc, const char *id, size_t len) {
    if (!dom_valid(doc) || !len) return DOM_NULL;
    atoms_init();
    CssAtom a = css_atom_find(id, len);
    uint32_t n = 0;
    const uint32_t *cand = dom_id_index_lookup(a, &n);
    uint32_t d = dom_slot(doc);
    uint32_t best = 0, best_pos = UINT32_MAX;
    DomIndex *ix = NULL;
    uint32_t found = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = cand[i];
        if (s >= g_dom.count || g_dom.kind[s] != DOM_K_ELEMENT || g_dom.id_atom[s] != a) continue;
        uint32_t vl = 0;
        const char *v = attr_val(s, g_a_id, &vl);
        if (!v || vl != len || memcmp(v, id, len) != 0) continue;
        if (doc_of(s) != d) continue;
        found++;
        if (found == 1) { best = s; continue; }
        /* Several: document order decides. */
        if (!ix) { ix = index_for(doc); if (ix && best < ix->pos_cap) best_pos = ix->pos[best]; }
        if (ix && s < ix->pos_cap && ix->pos[s] && ix->pos[s] < best_pos) { best = s; best_pos = ix->pos[s]; }
    }
    return best ? dom_id_of(best) : DOM_NULL;
}

static DomNodeList by_key(NodeId root, int kind, const char *v, size_t len, bool include_self) {
    DomNodeList out = { 0 };
    if (!dom_valid(root) || !len) return out;
    atoms_init();
    char *sel = malloc(len + 2);
    size_t k = 0;
    if (kind == SS_CLASS) sel[k++] = '.';
    memcpy(sel + k, v, len);
    k += len;
    sel[k] = '\0';
    if (kind == SS_TAG && len == 1 && v[0] == '*') {
        free(sel);
        return dom_query_all(root, "*", 1, include_self);
    }
    out = dom_query_all(root, sel, k, include_self);
    free(sel);
    return out;
}

DomNodeList dom_query_by_class(NodeId doc, const char *cls, size_t len) {
    return dom_query_by_class_in(doc, cls, len);
}

DomNodeList dom_query_by_tag(NodeId doc, const char *tag, size_t len) {
    return by_key(doc, SS_TAG, tag, len, false);
}

DomNodeList dom_query_by_tag_in(NodeId root, const char *tag, size_t len, bool include_self) {
    return by_key(root, SS_TAG, tag, len, include_self);
}

DomNodeList dom_query_by_class_in(NodeId root, const char *cls, size_t len) {
    /* "a b" means elements with both classes: build ".a.b". */
    DomNodeList out = { 0 };
    if (!dom_valid(root)) return out;
    char *sel = malloc(5 * len + 2);
    size_t k = 0, i = 0;
#define CLASS_WS(c) ((c) == ' ' || (c) == '\t' || (c) == '\n' || (c) == '\f' || (c) == '\r')
    while (i < len) {
        while (i < len && CLASS_WS(cls[i])) i++;
        size_t b = i;
        while (i < len && !CLASS_WS(cls[i])) i++;
        if (i > b) sel[k++] = '.';
        for (size_t q = b; q < i; q++) {
            /* (what isn't an identifier's, as an escape) */
            unsigned char c = (unsigned char)cls[q];
            if (c >= 0x80 || c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (q > b && (c == '-' || (c >= '0' && c <= '9')))) sel[k++] = (char)c;
            else k += (size_t)snprintf(sel + k, 5, "\\%02x ", c);
        }
    }
#undef CLASS_WS
    sel[k] = '\0';
    if (k) out = dom_query_all(root, sel, k, false);
    free(sel);
    return out;
}

void dom_query_note_attr(NodeId n, const char *name) {
    (void)n; (void)name;   /* versions in the store cover invalidation */
}

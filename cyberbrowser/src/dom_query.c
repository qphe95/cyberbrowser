/* Selector queries and indexes over the node store.  See dom_query.h. */
#include "dom_query.h"
#include "cyber_env.h"
#include "job_system.h"

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
    SS_NOT, SS_IS,
    SS_FIRST_CHILD, SS_LAST_CHILD, SS_ONLY_CHILD, SS_NTH_CHILD, SS_NTH_LAST_CHILD,
    SS_FIRST_OF_TYPE, SS_LAST_OF_TYPE, SS_ONLY_OF_TYPE, SS_EMPTY, SS_ROOT, SS_SCOPE,
    SS_CHECKED, SS_DISABLED, SS_ENABLED,
    SS_ANY,          /* unsupported pseudo-class: matches */
    SS_NEVER,        /* pseudo-element: never matches */
};
enum { AO_EXISTS, AO_EQ, AO_WORD, AO_DASH, AO_PREFIX, AO_SUFFIX, AO_SUBSTR };

struct SelList;

typedef struct SelSimple {
    uint8_t kind, op, ci;
    CssAtom atom;               /* tag / attribute name */
    char *val;                  /* id, class, attribute value */
    uint32_t vlen;
    int a, b;                   /* an+b */
    struct SelList *sub;        /* :not / :is */
} SelSimple;

typedef struct SelCompound {
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
} SelParser;

static void sp_ws(SelParser *sp) { while (sp->p < sp->e && isspace((unsigned char)*sp->p)) sp->p++; }

static bool ident_char(unsigned char c) {
    return isalnum(c) || c == '-' || c == '_' || c >= 0x80 || c == '\\';
}

/* An identifier with simple backslash escapes, unescaped into a new string. */
static char *sp_ident(SelParser *sp, uint32_t *len) {
    const char *b = sp->p;
    char *out = malloc((size_t)(sp->e - b) + 1);
    uint32_t n = 0;
    while (sp->p < sp->e && ident_char((unsigned char)*sp->p)) {
        if (*sp->p == '\\' && sp->p + 1 < sp->e) { sp->p++; out[n++] = *sp->p++; continue; }
        out[n++] = *sp->p++;
    }
    out[n] = '\0';
    if (!n) { free(out); sp->err = true; return NULL; }
    *len = n;
    return out;
}

static SelList *parse_list(SelParser *sp, bool nested);

static void add_simple(SelCompound *c, SelSimple s) {
    c->s = realloc(c->s, (size_t)(c->n + 1) * sizeof(SelSimple));
    c->s[c->n++] = s;
}

static bool parse_nth(const char *s, size_t n, int *a, int *b) {
    char buf[64];
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

/* Parse one compound; returns false at the end of the complex selector. */
static bool parse_compound(SelParser *sp, SelCompound *c) {
    bool any = false;
    while (sp->p < sp->e && !sp->err) {
        char ch = *sp->p;
        SelSimple s;
        memset(&s, 0, sizeof s);
        if (ch == '*') {
            sp->p++;
            s.kind = SS_UNIVERSAL;
        } else if (ch == '#') {
            sp->p++;
            s.kind = SS_ID;
            s.val = sp_ident(sp, &s.vlen);
        } else if (ch == '.') {
            sp->p++;
            s.kind = SS_CLASS;
            s.val = sp_ident(sp, &s.vlen);
        } else if (ch == '[') {
            sp->p++;
            sp_ws(sp);
            uint32_t nl = 0;
            char *name = sp_ident(sp, &nl);
            if (!name) break;
            css_atom_reserve(1);
            s.kind = SS_ATTR;
            s.atom = css_atom_intern(name, nl);
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
                    const char *vb = sp->p;
                    while (sp->p < sp->e && *sp->p != q) sp->p++;
                    if (sp->p >= sp->e) { sp->err = true; break; }
                    s.vlen = (uint32_t)(sp->p - vb);
                    s.val = malloc(s.vlen + 1);
                    memcpy(s.val, vb, s.vlen);
                    s.val[s.vlen] = '\0';
                    sp->p++;
                } else {
                    s.val = sp_ident(sp, &s.vlen);
                    if (!s.val) break;
                }
                sp_ws(sp);
                if (sp->p < sp->e && (*sp->p == 'i' || *sp->p == 'I')) { s.ci = 1; sp->p++; sp_ws(sp); }
                else if (sp->p < sp->e && (*sp->p == 's' || *sp->p == 'S')) { sp->p++; sp_ws(sp); }
            }
            if (sp->p >= sp->e || *sp->p != ']') { sp->err = true; free(s.val); break; }
            sp->p++;
        } else if (ch == ':') {
            sp->p++;
            if (sp->p < sp->e && *sp->p == ':') {
                sp->p++;
                uint32_t nl = 0;
                free(sp_ident(sp, &nl));
                s.kind = SS_NEVER;
            } else {
                uint32_t nl = 0;
                char *name = sp_ident(sp, &nl);
                if (!name) break;
                for (char *q = name; *q; q++) *q = (char)tolower((unsigned char)*q);
                const char *args = NULL;
                size_t alen = 0;
                if (sp->p < sp->e && *sp->p == '(') {
                    sp->p++;
                    args = sp->p;
                    int depth = 1;
                    while (sp->p < sp->e && depth) {
                        if (*sp->p == '(') depth++;
                        else if (*sp->p == ')') depth--;
                        if (depth) sp->p++;
                    }
                    if (sp->p >= sp->e) { sp->err = true; free(name); break; }
                    alen = (size_t)(sp->p - args);
                    sp->p++;
                }
                if (strcmp(name, "not") == 0 || strcmp(name, "is") == 0 || strcmp(name, "where") == 0 ||
                    strcmp(name, "matches") == 0 || strcmp(name, "-webkit-any") == 0) {
                    if (!args) { sp->err = true; free(name); break; }
                    SelParser sub = { args, args + alen, false };
                    s.sub = parse_list(&sub, true);
                    if (!s.sub || !s.sub->ok) { sel_free(s.sub); sp->err = true; free(name); break; }
                    s.kind = strcmp(name, "not") == 0 ? SS_NOT : SS_IS;
                } else if (strcmp(name, "nth-child") == 0 || strcmp(name, "nth-last-child") == 0) {
                    if (!args || !parse_nth(args, alen, &s.a, &s.b)) { sp->err = true; free(name); break; }
                    s.kind = name[4] == 'l' ? SS_NTH_LAST_CHILD : SS_NTH_CHILD;
                } else if (strcmp(name, "first-child") == 0) s.kind = SS_FIRST_CHILD;
                else if (strcmp(name, "last-child") == 0) s.kind = SS_LAST_CHILD;
                else if (strcmp(name, "only-child") == 0) s.kind = SS_ONLY_CHILD;
                else if (strcmp(name, "first-of-type") == 0) s.kind = SS_FIRST_OF_TYPE;
                else if (strcmp(name, "last-of-type") == 0) s.kind = SS_LAST_OF_TYPE;
                else if (strcmp(name, "only-of-type") == 0) s.kind = SS_ONLY_OF_TYPE;
                else if (strcmp(name, "empty") == 0) s.kind = SS_EMPTY;
                else if (strcmp(name, "root") == 0) s.kind = SS_ROOT;
                else if (strcmp(name, "scope") == 0) s.kind = SS_SCOPE;
                else if (strcmp(name, "checked") == 0) s.kind = SS_CHECKED;
                else if (strcmp(name, "disabled") == 0) s.kind = SS_DISABLED;
                else if (strcmp(name, "enabled") == 0) s.kind = SS_ENABLED;
                else if (strcmp(name, "before") == 0 || strcmp(name, "after") == 0 ||
                         strcmp(name, "first-line") == 0 || strcmp(name, "first-letter") == 0) s.kind = SS_NEVER;
                else s.kind = SS_ANY;
                free(name);
            }
        } else if (ident_char((unsigned char)ch)) {
            uint32_t nl = 0;
            char *name = sp_ident(sp, &nl);
            if (!name) break;
            if (any) { sp->err = true; free(name); break; }   /* type selector must come first */
            css_atom_reserve(1);
            s.kind = SS_TAG;
            s.atom = css_atom_intern(name, nl);
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
        for (;;) {
            SelCompound c = { NULL, 0, comb };
            if (!parse_compound(sp, &c)) { free(c.s); break; }
            cx.c = realloc(cx.c, (size_t)(cx.n + 1) * sizeof(SelCompound));
            cx.c[cx.n++] = c;
            const char *before = sp->p;
            sp_ws(sp);
            if (sp->p >= sp->e || *sp->p == ',') break;
            if (*sp->p == '>' || *sp->p == '+' || *sp->p == '~') {
                comb = *sp->p++;
                sp_ws(sp);
            } else if (sp->p > before) {
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

static bool attr_ok(uint32_t s, const SelSimple *ss) {
    DomAttrList *l = &g_dom.attrs[s];
    for (int i = 0; i < l->n; i++) {
        if (l->a[i].name != ss->atom) continue;
        const char *v = l->a[i].value.s ? l->a[i].value.s : "";
        size_t vn = l->a[i].value.len, n = ss->vlen;
        bool ci = ss->ci;
        switch (ss->op) {
        case AO_EXISTS: return true;
        case AO_EQ: return vn == n && cmp_n(v, ss->val, n, ci);
        case AO_WORD: return word_in(v, vn, ss->val, n, ci);
        case AO_DASH: return (vn == n && cmp_n(v, ss->val, n, ci)) || (vn > n && cmp_n(v, ss->val, n, ci) && v[n] == '-');
        case AO_PREFIX: return n && vn >= n && cmp_n(v, ss->val, n, ci);
        case AO_SUFFIX: return n && vn >= n && cmp_n(v + vn - n, ss->val, n, ci);
        case AO_SUBSTR:
            if (!n) return false;
            for (size_t k = 0; k + n <= vn; k++) if (cmp_n(v + k, ss->val, n, ci)) return true;
            return false;
        }
        return false;
    }
    return false;
}

static CssAtom g_a_id, g_a_class, g_a_checked, g_a_disabled;

static void atoms_init(void) {
    if (g_a_id) return;
    css_atom_reserve(4);
    g_a_id = css_atom_intern("id", 2);
    g_a_class = css_atom_intern("class", 5);
    g_a_checked = css_atom_intern("checked", 7);
    g_a_disabled = css_atom_intern("disabled", 8);
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

static bool simple_ok(uint32_t s, const SelSimple *ss, uint32_t scope) {
    switch (ss->kind) {
    case SS_TAG: return g_dom.tag[s] == ss->atom;
    case SS_UNIVERSAL: return true;
    case SS_ID: {
        uint32_t n = 0;
        const char *v = attr_val(s, g_a_id, &n);
        return v && n == ss->vlen && memcmp(v, ss->val, n) == 0;
    }
    case SS_CLASS: {
        uint32_t n = 0;
        const char *v = attr_val(s, g_a_class, &n);
        return v && word_in(v, n, ss->val, ss->vlen, false);
    }
    case SS_ATTR: return attr_ok(s, ss);
    case SS_NOT: return !list_matches(ss->sub, s, scope);
    case SS_IS: return list_matches(ss->sub, s, scope);
    case SS_FIRST_CHILD: case SS_LAST_CHILD: case SS_ONLY_CHILD:
    case SS_NTH_CHILD: case SS_NTH_LAST_CHILD:
    case SS_FIRST_OF_TYPE: case SS_LAST_OF_TYPE: case SS_ONLY_OF_TYPE: {
        if (!g_dom.parent[s]) return false;
        bool of_type = ss->kind == SS_FIRST_OF_TYPE || ss->kind == SS_LAST_OF_TYPE || ss->kind == SS_ONLY_OF_TYPE;
        int before = 0, after = 0;
        for (uint32_t x = g_dom.prev[s]; x; x = g_dom.prev[x])
            if (g_dom.kind[x] == DOM_K_ELEMENT && (!of_type || g_dom.tag[x] == g_dom.tag[s])) before++;
        for (uint32_t x = g_dom.next[s]; x; x = g_dom.next[x])
            if (g_dom.kind[x] == DOM_K_ELEMENT && (!of_type || g_dom.tag[x] == g_dom.tag[s])) after++;
        switch (ss->kind) {
        case SS_FIRST_CHILD: case SS_FIRST_OF_TYPE: return before == 0;
        case SS_LAST_CHILD: case SS_LAST_OF_TYPE: return after == 0;
        case SS_ONLY_CHILD: case SS_ONLY_OF_TYPE: return before == 0 && after == 0;
        case SS_NTH_CHILD: return nth_ok(ss->a, ss->b, before + 1);
        default: return nth_ok(ss->a, ss->b, after + 1);
        }
    }
    case SS_EMPTY:
        for (uint32_t c = g_dom.first[s]; c; c = g_dom.next[c])
            if (g_dom.kind[c] == DOM_K_ELEMENT || (g_dom.kind[c] == DOM_K_TEXT && g_dom.text[c].len)) return false;
        return true;
    case SS_ROOT: return g_dom.parent[s] && g_dom.kind[g_dom.parent[s]] == DOM_K_DOCUMENT;
    case SS_SCOPE: return s == scope;
    case SS_CHECKED: { uint32_t n; return attr_val(s, g_a_checked, &n) != NULL; }
    case SS_DISABLED: { uint32_t n; return attr_val(s, g_a_disabled, &n) != NULL; }
    case SS_ENABLED: { uint32_t n; return attr_val(s, g_a_disabled, &n) == NULL; }
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

/* Does compound i (and everything to its left) match at s? */
static bool complex_at(const SelComplex *cx, int i, uint32_t s, uint32_t scope) {
    if (!compound_ok(s, &cx->c[i], scope)) return false;
    if (i == 0) return true;
    switch (cx->c[i].comb) {
    case '>': {
        uint32_t p = g_dom.parent[s];
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
        for (uint32_t p = g_dom.parent[s]; p; p = g_dom.parent[p])
            if (complex_at(cx, i - 1, p, scope)) return true;
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
    if (!dom_valid(el) || dom_kind(el) != DOM_K_ELEMENT) return false;
    atoms_init();
    SelList *l = sel_get(sel, len);
    return list_matches(l, dom_slot(el), dom_slot(el));
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
} DomIndex;

static DomIndex g_ix;

/* Class tokens of element s as atoms (lower-cased; callers re-check case). */
static uint32_t class_atoms(uint32_t s, CssAtom *out, uint32_t max) {
    uint32_t n = 0, len = 0;
    const char *v = attr_val(s, g_a_class, &len);
    if (!v) return 0;
    size_t i = 0;
    while (i < len && n < max) {
        while (i < len && isspace((unsigned char)v[i])) i++;
        size_t b = i;
        while (i < len && !isspace((unsigned char)v[i])) i++;
        if (i > b) {
            CssAtom a = css_atom_find(v + b, i - b);
            if (!a) { css_atom_reserve(1); a = css_atom_intern(v + b, i - b); }
            out[n++] = a;
        }
    }
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
    CssAtom cls[IX_MAX_CLASSES];
    for (uint32_t i = b; i < e; i++) {
        uint32_t s = ix->order[i];
        if (g_dom.kind[s] != DOM_K_ELEMENT) continue;
        if (g_dom.tag[s] < ix->natoms) tc[g_dom.tag[s]]++;
        uint32_t nc = class_atoms(s, cls, IX_MAX_CLASSES);
        for (uint32_t k = 0; k < nc; k++) if (cls[k] < ix->natoms) cc[cls[k]]++;
    }
}

static void ix_scatter_job(Job *job) {
    IxJob *J = job->ctx;
    DomIndex *ix = J->ix;
    uint32_t r = job->a, b = r * IX_RANGE, e = b + IX_RANGE < ix->n ? b + IX_RANGE : ix->n;
    uint32_t *to = J->tag_cnt + (size_t)r * ix->natoms, *co = J->cls_cnt + (size_t)r * ix->natoms;
    CssAtom cls[IX_MAX_CLASSES];
    for (uint32_t i = b; i < e; i++) {
        uint32_t s = ix->order[i];
        if (g_dom.kind[s] != DOM_K_ELEMENT) continue;
        if (g_dom.tag[s] < ix->natoms) ix->tag_list[to[g_dom.tag[s]]++] = i;
        uint32_t nc = class_atoms(s, cls, IX_MAX_CLASSES);
        for (uint32_t k = 0; k < nc; k++) if (cls[k] < ix->natoms) ix->cls_list[co[cls[k]]++] = i;
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
    /* Make sure class names are atoms before jobs run (they only look up). */
    CssAtom tmp[IX_MAX_CLASSES];
    for (uint32_t i = 0; i < ix->n; i++)
        if (g_dom.kind[ix->order[i]] == DOM_K_ELEMENT) class_atoms(ix->order[i], tmp, IX_MAX_CLASSES);
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
                atom = css_atom_find(val, vlen);     /* the build interned every class token */
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
    char *sel = malloc(2 * len + 2);
    size_t k = 0, i = 0;
    while (i < len) {
        while (i < len && isspace((unsigned char)cls[i])) i++;
        size_t b = i;
        while (i < len && !isspace((unsigned char)cls[i])) i++;
        if (i > b) { sel[k++] = '.'; memcpy(sel + k, cls + b, i - b); k += i - b; }
    }
    sel[k] = '\0';
    if (k) out = dom_query_all(root, sel, k, false);
    free(sel);
    return out;
}

void dom_query_note_attr(NodeId n, const char *name) {
    (void)n; (void)name;   /* versions in the store cover invalidation */
}

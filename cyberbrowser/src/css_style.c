/*
 * Compiled style matching.  See css_style.h and PARALLEL_STYLE_LAYOUT.md.
 */
#include "css_style.h"
#include "css_selector_internal.h"
#include "preorder_compaction_array.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ============================================================================
 * Atom table: open addressing, lock-free insert (CAS on an empty slot),
 * capacity reserved while no other thread interns.
 * ============================================================================ */

typedef struct AtomRec {
    uint32_t hash;
    uint32_t len;
    CssAtom id;
    char s[];
} AtomRec;

static _Atomic(AtomRec *) *g_atom_slots;
static uint32_t g_atom_mask;
static AtomRec **g_atom_by_id;
static uint32_t g_atom_by_id_cap;
static _Atomic uint32_t g_atom_next_id = 2;
static _Atomic uint32_t g_atom_used;

static uint32_t atom_hash(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

static void atom_table_init(uint32_t cap)
{
    g_atom_slots = calloc(cap, sizeof(*g_atom_slots));
    g_atom_mask = cap - 1;
    g_atom_by_id_cap = cap;
    g_atom_by_id = calloc(cap, sizeof(AtomRec *));
}

void css_atom_reserve(size_t extra)
{
    if (!g_atom_slots) atom_table_init(1u << 14);
    uint32_t used = atomic_load_explicit(&g_atom_used, memory_order_relaxed);
    uint32_t next = atomic_load_explicit(&g_atom_next_id, memory_order_relaxed);
    size_t need = ((size_t)used + extra) * 2;
    if (need > (size_t)g_atom_mask + 1) {
        uint32_t cap = (g_atom_mask + 1) * 2;
        while (cap < need) cap *= 2;
        _Atomic(AtomRec *) *slots = calloc(cap, sizeof(*slots));
        for (uint32_t i = 0; i <= g_atom_mask; i++) {
            AtomRec *r = atomic_load_explicit(&g_atom_slots[i], memory_order_relaxed);
            if (!r) continue;
            uint32_t j = r->hash & (cap - 1);
            while (atomic_load_explicit(&slots[j], memory_order_relaxed)) j = (j + 1) & (cap - 1);
            atomic_store_explicit(&slots[j], r, memory_order_relaxed);
        }
        free((void *)g_atom_slots);
        g_atom_slots = slots;
        g_atom_mask = cap - 1;
    }
    if ((size_t)next + extra >= g_atom_by_id_cap) {
        uint32_t cap = g_atom_by_id_cap * 2;
        while ((size_t)cap <= (size_t)next + extra) cap *= 2;
        AtomRec **by = calloc(cap, sizeof(AtomRec *));
        memcpy(by, g_atom_by_id, g_atom_by_id_cap * sizeof(AtomRec *));
        free(g_atom_by_id);
        g_atom_by_id = by;
        g_atom_by_id_cap = cap;
    }
    atomic_thread_fence(memory_order_release);
}

static CssAtom atom_lookup_lower(const char *low, size_t len, uint32_t h, bool insert)
{
    uint32_t i = h & g_atom_mask;
    AtomRec *mine = NULL;
    for (uint32_t probes = 0; probes <= g_atom_mask; probes++) {
        AtomRec *r = atomic_load_explicit(&g_atom_slots[i], memory_order_acquire);
        if (!r) {
            if (!insert) return CSS_ATOM_NONE;
            if (!mine) {
                mine = malloc(sizeof(AtomRec) + len + 1);
                mine->hash = h;
                mine->len = (uint32_t)len;
                memcpy(mine->s, low, len);
                mine->s[len] = '\0';
                mine->id = atomic_fetch_add_explicit(&g_atom_next_id, 1, memory_order_relaxed);
            }
            AtomRec *expected = NULL;
            if (atomic_compare_exchange_strong_explicit(&g_atom_slots[i], &expected, mine,
                                                        memory_order_acq_rel,
                                                        memory_order_acquire)) {
                if (mine->id < g_atom_by_id_cap) g_atom_by_id[mine->id] = mine;
                atomic_fetch_add_explicit(&g_atom_used, 1, memory_order_relaxed);
                return mine->id;
            }
            r = expected;
        }
        if (r->hash == h && r->len == len && memcmp(r->s, low, len) == 0) {
            free(mine);   /* lost a race to an equal string: its id gap is harmless */
            return r->id;
        }
        i = (i + 1) & g_atom_mask;
    }
    free(mine);
    return CSS_ATOM_NONE;   /* table full: callers reserve, so this does not happen */
}

static CssAtom atom_get(const char *s, size_t len, bool insert)
{
    if (len == 0) return CSS_ATOM_EMPTY;
    if (!g_atom_slots) css_atom_reserve(0);
    char stack[256];
    char *low = len <= sizeof(stack) ? stack : malloc(len);
    for (size_t i = 0; i < len; i++) low[i] = (char)tolower((unsigned char)s[i]);
    CssAtom a = atom_lookup_lower(low, len, atom_hash(low, len), insert);
    if (low != stack) free(low);
    return a;
}

CssAtom css_atom_intern(const char *s, size_t len) { return atom_get(s, len, true); }
CssAtom css_atom_find(const char *s, size_t len) { return atom_get(s, len, false); }

const char *css_atom_str(CssAtom a)
{
    if (a == CSS_ATOM_EMPTY) return "";
    if (a < g_atom_by_id_cap && g_atom_by_id && g_atom_by_id[a]) return g_atom_by_id[a]->s;
    return "";
}

size_t css_atom_count(void) { return atomic_load(&g_atom_next_id); }

static CssAtom atom_cstr(const char *s) { return css_atom_intern(s, strlen(s)); }

/* ============================================================================
 * Growable arrays (build time only)
 * ============================================================================ */

#define VEC(T) struct { T *v; size_t n, cap; }
typedef VEC(int32_t) I32Vec;
#define VEC_PUSH(vec, x) do { \
    if ((vec).n == (vec).cap) { \
        (vec).cap = (vec).cap ? (vec).cap * 2 : 16; \
        (vec).v = realloc((vec).v, (vec).cap * sizeof(*(vec).v)); \
    } \
    (vec).v[(vec).n++] = (x); \
} while (0)

/* ============================================================================
 * Per-document element features
 * ============================================================================ */

enum { DF_LINK = 1, DF_HAS_CHILD = 2 };

struct CssStyleDoc {
    int count;
    uint8_t *is_elem;
    uint8_t *flags;
    CssAtom *tag, *id;
    uint32_t *cls_off; uint16_t *cls_n;
    uint32_t *attr_off; uint16_t *attr_n;
    CssAtom *pool;
    const char **pool_val;   /* attribute value per pool entry (NULL for classes) */
    int32_t *parent;
    int32_t *pos_first, *pos_last, *type_first, *type_last;
    int32_t *prev_elem;   /* previous element sibling, -1 = none */
    int32_t *next_elem;   /* next element sibling, -1 = none */
    int32_t *sub_end;     /* one past the last descendant (preorder) */
};

static const char *node_attr(HtmlNode *node, const char *name)
{
    for (HtmlAttribute *a = node->attributes; a; a = a->next)
        if (strcasecmp(a->name, name) == 0) return a->value;
    return NULL;
}

CssStyleDoc *css_styledoc_build(HtmlDocument *doc)
{
    CssStyleDoc *d = calloc(1, sizeof(*d));
    int n = (int)po_array_count(&doc->array);
    d->count = n;
    d->is_elem = calloc((size_t)n + 1, 1);
    d->flags = calloc((size_t)n + 1, 1);
    d->tag = calloc((size_t)n + 1, sizeof(CssAtom));
    d->id = calloc((size_t)n + 1, sizeof(CssAtom));
    d->cls_off = calloc((size_t)n + 1, sizeof(uint32_t));
    d->cls_n = calloc((size_t)n + 1, sizeof(uint16_t));
    d->attr_off = calloc((size_t)n + 1, sizeof(uint32_t));
    d->attr_n = calloc((size_t)n + 1, sizeof(uint16_t));
    d->parent = malloc(((size_t)n + 1) * sizeof(int32_t));
    d->pos_first = calloc((size_t)n + 1, sizeof(int32_t));
    d->prev_elem = malloc(((size_t)n + 1) * sizeof(int32_t));
    d->next_elem = malloc(((size_t)n + 1) * sizeof(int32_t));
    d->sub_end = malloc(((size_t)n + 1) * sizeof(int32_t));
    for (int i = 0; i <= n; i++) { d->prev_elem[i] = -1; d->next_elem[i] = -1; d->sub_end[i] = i + 1; }
    d->pos_last = calloc((size_t)n + 1, sizeof(int32_t));
    d->type_first = calloc((size_t)n + 1, sizeof(int32_t));
    d->type_last = calloc((size_t)n + 1, sizeof(int32_t));

    /* Worst case: every attribute and class token becomes a new atom. */
    size_t est = 64;
    for (int i = 0; i < n; i++) {
        HtmlNode *node = po_array_payload(&doc->array, i);
        if (!node || node->type != HTML_NODE_ELEMENT) continue;
        est += 2;
        for (HtmlAttribute *a = node->attributes; a; a = a->next) est += 2 + strlen(a->value) / 2;
    }
    css_atom_reserve(est);

    VEC(CssAtom) pool = {0};
    VEC(const char *) pval = {0};
    for (int i = 0; i < n; i++) {
        d->parent[i] = po_array_parent(&doc->array, i);
        HtmlNode *node = po_array_payload(&doc->array, i);
        if (!node || node->type != HTML_NODE_ELEMENT) continue;
        d->is_elem[i] = 1;
        d->tag[i] = atom_cstr(node->tag_name);
        if (node->array_node.first_child >= 0) d->flags[i] |= DF_HAS_CHILD;
        const char *id = node_attr(node, "id");
        d->id[i] = id ? atom_cstr(id) : CSS_ATOM_NONE;
        if ((strcasecmp(node->tag_name, "a") == 0 || strcasecmp(node->tag_name, "area") == 0) &&
            node_attr(node, "href"))
            d->flags[i] |= DF_LINK;
        /* Class tokens of the first class attribute (the legacy lookup). */
        d->cls_off[i] = (uint32_t)pool.n;
        const char *cls = node_attr(node, "class");
        if (cls) {
            const char *p = cls;
            while (*p) {
                while (*p && css_sel_is_space(*p)) p++;
                if (!*p) break;
                const char *st = p;
                while (*p && !css_sel_is_space(*p)) p++;
                VEC_PUSH(pool, css_atom_intern(st, (size_t)(p - st)));
                VEC_PUSH(pval, (const char *)NULL);
            }
        }
        d->cls_n[i] = (uint16_t)(pool.n - d->cls_off[i]);
        d->attr_off[i] = (uint32_t)pool.n;
        for (HtmlAttribute *a = node->attributes; a; a = a->next) {
            VEC_PUSH(pool, atom_cstr(a->name));
            VEC_PUSH(pval, (const char *)(a->value ? a->value : ""));
        }
        d->attr_n[i] = (uint16_t)(pool.n - d->attr_off[i]);
    }
    d->pool = pool.v;
    d->pool_val = pval.v;

    /* Sibling positions, computed the way css_element_index counts them. */
    size_t natoms = css_atom_count() + 1;
    int32_t *tcount = calloc(natoms, sizeof(int32_t));
    VEC(int) kids = {0};
    for (int i = 0; i < n; i++) {
        if (d->parent[i] < 0 && d->is_elem[i]) {
            d->pos_first[i] = d->pos_last[i] = d->type_first[i] = d->type_last[i] = 1;
        }
        kids.n = 0;
        for (int c = po_array_first_child(&doc->array, i); c >= 0;
             c = po_array_next_sibling(&doc->array, c)) {
            if (c < n && d->is_elem[c]) VEC_PUSH(kids, c);
        }
        if (kids.n == 0) continue;
        int total = (int)kids.n;
        for (int k = 0; k < total; k++) {
            int c = kids.v[k];
            /* A node is positioned among the children of its own parent. */
            if (d->parent[c] != i) continue;
            d->pos_first[c] = k + 1;
            d->pos_last[c] = total - k;
            if (k > 0 && d->parent[kids.v[k - 1]] == i) {
                d->prev_elem[c] = kids.v[k - 1];
                d->next_elem[kids.v[k - 1]] = c;
            }
        }
        for (int k = 0; k < total; k++) tcount[d->tag[kids.v[k]]]++;
        for (int k = 0; k < total; k++) {
            int c = kids.v[k];
            int32_t *cnt = &tcount[d->tag[c]];
            if (d->parent[c] == i) {
                d->type_last[c] = *cnt;     /* remaining same-tag siblings incl. this one */
            }
            (*cnt)--;
        }
        for (int k = 0; k < total; k++) tcount[d->tag[kids.v[k]]] = 0;
        for (int k = 0; k < total; k++) {
            int c = kids.v[k];
            int32_t v = ++tcount[d->tag[c]];
            if (d->parent[c] == i) d->type_first[c] = v;
        }
        for (int k = 0; k < total; k++) tcount[d->tag[kids.v[k]]] = 0;
    }
    free(kids.v);
    for (int i = n - 1; i >= 0; i--) {
        int p = d->parent[i];
        if (p >= 0 && p < i && d->sub_end[i] > d->sub_end[p]) d->sub_end[p] = d->sub_end[i];
    }
    free(tcount);
    return d;
}

void css_styledoc_free(CssStyleDoc *d)
{
    if (!d) return;
    free(d->is_elem); free(d->flags); free(d->tag); free(d->id);
    free(d->cls_off); free(d->cls_n); free(d->attr_off); free(d->attr_n);
    free(d->pool); free(d->pool_val); free(d->parent);
    free(d->prev_elem); free(d->next_elem); free(d->sub_end); free(d->pos_first); free(d->pos_last); free(d->type_first); free(d->type_last);
    free(d);
}

/* ============================================================================
 * Compiled selectors
 * ============================================================================ */

typedef struct CComp {
    CssAtom tag, id;
    uint32_t cls_off, attr_off, ncls_off, nattr_off;
    uint32_t atest_off, natest_off;   /* into CssStyleSet.atests, parallel to attrs */
    uint8_t cls_n, attr_n, ncls_n, nattr_n;
    uint8_t attr_vals, nattr_vals;    /* any value test present */
    uint8_t root, link, empty, child_pos, type_pos, nth_kind;
    uint8_t not_active, not_has_tag, not_has_id;
    uint8_t comb;
    uint8_t never;
    int nth_a, nth_b;
    CssAtom not_tag, not_id;
    int32_t where;   /* list index, -1 = none */
    int32_t has;     /* :has() list index, -1 = none */
    uint8_t has_comb;
} CComp;

#define BLOOM_REQ 8
typedef struct CChain {
    uint32_t first;
    uint16_t n;
    uint8_t never;
    uint8_t bloom_n;
    int32_t spec;
    int32_t owner;          /* rule g, after-branch id, or -1 */
    CssAtom bloom[BLOOM_REQ];
} CChain;

typedef struct CList { uint32_t first, n; } CList;

typedef struct CRule {
    const CssRule *rule;
    int sheet, index;
    int32_t aspect;         /* list index, -1 = none */
} CRule;

typedef struct CAfter {
    int32_t rule;
    int32_t spec;
    int32_t is_before;
} CAfter;

/* Bucket map: key (kind << 32 | atom) -> [off, off+n) in bucket_items. */
enum { K_ID = 1, K_CLS = 2, K_ATTR = 3, K_TAG = 4 };
typedef struct BucketSlot { uint64_t key; uint32_t off, n; } BucketSlot;
typedef struct BucketMap {
    BucketSlot *slots;
    uint32_t mask;
    uint32_t *items;
    uint32_t *universal; uint32_t universal_n;
} BucketMap;

typedef struct AttrTest {
    uint8_t op, ci;
    char *val;
} AttrTest;

struct CssStyleSet {
    AttrTest *atests; size_t natests;
    CComp *comps; size_t ncomps;
    CChain *chains; size_t nchains;
    CList *lists; size_t nlists;
    CssAtom *atoms; size_t natoms;
    CRule *rules; int nrules;
    CAfter *afters; int nafters;
    BucketMap rule_buckets, after_buckets;
    CssAtom atom_html;
    uint32_t *reval; uint32_t nreval;   /* position-dependent chains (§7.5) */
};

typedef struct Builder {
    VEC(CComp) comps;
    VEC(CChain) chains;
    VEC(CList) lists;
    VEC(CssAtom) atoms;
    VEC(AttrTest) atests;
    CssSelectorPart *parts;   /* scratch for the parser (large structs) */
} Builder;

static uint32_t push_atoms(Builder *b, char (*names)[64], int count)
{
    uint32_t off = (uint32_t)b->atoms.n;
    for (int i = 0; i < count; i++) VEC_PUSH(b->atoms, atom_cstr(names[i]));
    return off;
}

static uint32_t push_atests(Builder *b, const unsigned char *ops, const bool *ci,
                            char (*vals)[96], int count, uint8_t *any)
{
    uint32_t off = (uint32_t)b->atests.n;
    for (int i = 0; i < count; i++) {
        AttrTest t = { ops[i], ci[i], NULL };
        if (ops[i] != CSS_ATTR_OP_EXISTS) { t.val = strdup(vals[i]); *any = 1; }
        VEC_PUSH(b->atests, t);
    }
    return off;
}

static int32_t compile_list(Builder *b, const char *text);

static void compile_comp(Builder *b, const CssSimpleSelector *s, int comb, CComp *c)
{
    memset(c, 0, sizeof(*c));
    c->where = -1;
    c->has = -1;
    c->comb = (uint8_t)comb;
    c->never = (!s->has_substantive || s->never_match) ? 1 : 0;
    c->tag = s->has_tag ? atom_cstr(s->tag) : CSS_ATOM_NONE;
    c->id = s->has_id ? atom_cstr(s->id) : CSS_ATOM_NONE;
    c->cls_n = (uint8_t)s->class_count;
    c->cls_off = push_atoms(b, (char (*)[64])s->classes, s->class_count);
    c->attr_n = (uint8_t)s->attr_count;
    c->attr_off = push_atoms(b, (char (*)[64])s->attrs, s->attr_count);
    c->atest_off = push_atests(b, s->attr_ops, s->attr_ci, (char (*)[96])s->attr_vals,
                               s->attr_count, &c->attr_vals);
    c->root = s->is_root;
    c->link = s->requires_link;
    c->empty = s->requires_empty;
    c->child_pos = (uint8_t)s->child_pos;
    c->type_pos = (uint8_t)s->type_pos;
    c->nth_kind = (uint8_t)s->nth_kind;
    c->nth_a = s->nth_a;
    c->nth_b = s->nth_b;
    if (s->not_active) {
        c->not_active = 1;
        c->not_has_tag = s->not_has_tag;
        c->not_has_id = s->not_has_id;
        c->not_tag = s->not_has_tag ? atom_cstr(s->not_tag) : CSS_ATOM_NONE;
        c->not_id = s->not_has_id ? atom_cstr(s->not_id) : CSS_ATOM_NONE;
        c->ncls_n = (uint8_t)s->not_class_count;
        c->ncls_off = push_atoms(b, (char (*)[64])s->not_classes, s->not_class_count);
        c->nattr_n = (uint8_t)s->not_attr_count;
        c->nattr_off = push_atoms(b, (char (*)[64])s->not_attrs, s->not_attr_count);
        c->natest_off = push_atests(b, s->not_attr_ops, s->not_attr_ci,
                                    (char (*)[96])s->not_attr_vals, s->not_attr_count,
                                    &c->nattr_vals);
    }
}

/* Parse one chain of text into the builder.  Returns the chain index or -1
 * if the parser produced no parts (the legacy compiler drops those). */
static int32_t compile_chain(Builder *b, const char *text, int32_t owner)
{
    int count = css_sel_parse_chain(text, b->parts, CSS_MAX_SIMPLE_PARTS);
    if (count <= 0) return -1;
    CChain ch;
    memset(&ch, 0, sizeof(ch));
    ch.n = (uint16_t)count;
    ch.owner = owner;
    ch.spec = css_sel_chain_specificity(b->parts, count);
    /* Where-lists are compiled after copying the parts, since compiling them
     * reuses the parser scratch. */
    CssSelectorPart *local = malloc(sizeof(CssSelectorPart) * (size_t)count);
    memcpy(local, b->parts, sizeof(CssSelectorPart) * (size_t)count);
    CComp *tmp = malloc(sizeof(CComp) * (size_t)count);
    for (int i = 0; i < count; i++) {
        compile_comp(b, &local[i].simple, local[i].combinator, &tmp[i]);
        if (tmp[i].never) ch.never = 1;
    }
    /* The rightmost compound must not be a pseudo-element (it styles an
     * abstract box, so the legacy matcher never matches it). */
    if (local[count - 1].simple.is_pseudo_element) ch.never = 1;
    for (int i = 0; i < count; i++) {
        if (local[i].simple.where_active) tmp[i].where = compile_list(b, local[i].simple.where_arg);
        if (local[i].simple.has_active) {
            tmp[i].has = compile_list(b, local[i].simple.has_arg);
            tmp[i].has_comb = (uint8_t)local[i].simple.has_comb;
            if (tmp[i].has < 0) ch.never = 1;
        }
    }
    /* Ancestor requirements for the Bloom filter. */
    for (int i = 0; i + 1 < count && ch.bloom_n < BLOOM_REQ; i++) {
        /* Only compounds that must be ancestors of the subject: a compound
         * followed by a sibling combinator is a sibling, not an ancestor. */
        if (tmp[i + 1].comb != CSS_COMB_CHILD && tmp[i + 1].comb != CSS_COMB_DESCENDANT) continue;
        CssAtom req[2 + CSS_MAX_CLASSES];
        int nr = 0;
        if (tmp[i].tag) req[nr++] = tmp[i].tag;
        if (tmp[i].root) req[nr++] = atom_cstr("html");
        if (tmp[i].id) req[nr++] = tmp[i].id;
        for (int k = 0; k < tmp[i].cls_n; k++) {
            CssAtom a = b->atoms.v[tmp[i].cls_off + k];
            if (a != CSS_ATOM_EMPTY) req[nr++] = a;
        }
        for (int k = 0; k < nr && ch.bloom_n < BLOOM_REQ; k++) {
            bool dup = false;
            for (int j = 0; j < ch.bloom_n; j++) if (ch.bloom[j] == req[k]) dup = true;
            if (!dup) ch.bloom[ch.bloom_n++] = req[k];
        }
    }
    ch.first = (uint32_t)b->comps.n;
    for (int i = 0; i < count; i++) VEC_PUSH(b->comps, tmp[i]);
    free(tmp);
    free(local);
    VEC_PUSH(b->chains, ch);
    return (int32_t)(b->chains.n - 1);
}

/* Split a selector list at top-level commas exactly as css_compile_selector
 * and css_selector_matches do, trimming each branch.  Calls fn per branch. */
typedef void (*BranchFn)(Builder *b, const char *branch, void *ud);
static void for_each_branch(Builder *b, const char *selector, BranchFn fn, void *ud)
{
    size_t len = strlen(selector);
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        int paren_depth = 0;
        bool in_quote = false;
        char quote_char = 0;
        while (i < len) {
            char c = selector[i];
            if (!in_quote) {
                if (c == '"' || c == '\'') { in_quote = true; quote_char = c; }
                else if (c == '(') paren_depth++;
                else if (c == ')') { if (paren_depth > 0) paren_depth--; }
                else if (c == ',' && paren_depth == 0) break;
            } else {
                if (c == quote_char) in_quote = false;
            }
            i++;
        }
        size_t s0 = start, n = i - start;
        while (n > 0 && css_sel_is_space(selector[s0])) { s0++; n--; }
        while (n > 0 && css_sel_is_space(selector[s0 + n - 1])) n--;
        if (n > 0) {
            char *part = malloc(n + 1);
            memcpy(part, selector + s0, n);
            part[n] = '\0';
            fn(b, part, ud);
            free(part);
        }
        if (i < len && selector[i] == ',') i++;
    }
}

typedef struct ListCtx { I32Vec chains; } ListCtx;
static void list_branch(Builder *b, const char *branch, void *ud)
{
    ListCtx *lc = ud;
    int32_t ch = compile_chain(b, branch, -1);
    if (ch >= 0) VEC_PUSH(lc->chains, ch);
}

/* A list's chains are not contiguous (nested where-lists interleave), so a
 * list stores its chain indices as a run in the uint32 pool `atoms`. */
static int32_t compile_list(Builder *b, const char *text)
{
    ListCtx lc = {0};
    for_each_branch(b, text, list_branch, &lc);
    CList l;
    l.first = (uint32_t)b->atoms.n;
    l.n = (uint32_t)lc.chains.n;
    for (size_t i = 0; i < lc.chains.n; i++) VEC_PUSH(b->atoms, (CssAtom)lc.chains.v[i]);
    free(lc.chains.v);
    VEC_PUSH(b->lists, l);
    return (int32_t)(b->lists.n - 1);
}

/* ============================================================================
 * Matching
 * ============================================================================ */

static inline bool has_atom(const CssAtom *list, uint32_t n, CssAtom a)
{
    for (uint32_t i = 0; i < n; i++) if (list[i] == a) return true;
    return false;
}

static bool list_match(const CssStyleSet *S, const CssStyleDoc *D, int32_t li, int node);

static inline bool nth_matches(int a, int b, int pos)
{
    if (pos <= 0) return false;
    if (a == 0) return pos == b;
    int d = pos - b;
    if (d % a != 0) return false;
    return d / a >= 0;
}

/* Attribute test against element n: the first attribute with this name
 * (atoms are case-folded) must satisfy the operator. */
static bool attr_test(const CssStyleSet *S, const CssStyleDoc *D, int n, CssAtom name,
                      const AttrTest *t)
{
    (void)S;
    const CssAtom *attrs = D->pool + D->attr_off[n];
    for (uint32_t k = 0; k < D->attr_n[n]; k++) {
        if (attrs[k] != name) continue;
        if (t->op == CSS_ATTR_OP_EXISTS) return true;
        return css_attr_value_matches(t->op, t->ci, t->val, D->pool_val[D->attr_off[n] + k]);
    }
    return false;
}

static bool list_match(const CssStyleSet *S, const CssStyleDoc *D, int32_t li, int node);

/* :has(): some element in the relative position matches the argument. */
static bool has_match(const CssStyleSet *S, const CssStyleDoc *D, const CComp *c, int n)
{
    switch (c->has_comb) {
    case '+':
        return D->next_elem[n] >= 0 && list_match(S, D, c->has, D->next_elem[n]);
    case '~':
        for (int k = D->next_elem[n]; k >= 0; k = D->next_elem[k])
            if (list_match(S, D, c->has, k)) return true;
        return false;
    case '>':
        for (int k = n + 1; k < D->sub_end[n]; k = D->sub_end[k])
            if (D->is_elem[k] && D->parent[k] == n && list_match(S, D, c->has, k)) return true;
        return false;
    default: {
        int end = D->sub_end[n];
        if (end - n > 20000) end = n + 20000;
        for (int k = n + 1; k < end; k++)
            if (D->is_elem[k] && list_match(S, D, c->has, k)) return true;
        return false;
    }
    }
}

static bool comp_match(const CssStyleSet *S, const CssStyleDoc *D, const CComp *c, int n)
{
    if (n < 0 || !D->is_elem[n]) return false;
    if (c->never) return false;
    if (c->root && D->tag[n] != S->atom_html) return false;
    if (c->link && !(D->flags[n] & DF_LINK)) return false;
    if (c->id && D->id[n] != c->id) return false;
    const CssAtom *cls = D->pool + D->cls_off[n];
    for (uint32_t i = 0; i < c->cls_n; i++)
        if (!has_atom(cls, D->cls_n[n], S->atoms[c->cls_off + i])) return false;
    const CssAtom *attrs = D->pool + D->attr_off[n];
    if (c->attr_vals) {
        for (uint32_t i = 0; i < c->attr_n; i++)
            if (!attr_test(S, D, n, S->atoms[c->attr_off + i], &S->atests[c->atest_off + i]))
                return false;
    } else {
        for (uint32_t i = 0; i < c->attr_n; i++)
            if (!has_atom(attrs, D->attr_n[n], S->atoms[c->attr_off + i])) return false;
    }
    if (c->empty && (D->flags[n] & DF_HAS_CHILD)) return false;
    if (c->child_pos) {
        if (c->child_pos == 3) {
            if (D->pos_first[n] != 1 || D->pos_last[n] != 1) return false;
        } else if ((c->child_pos == 2 ? D->pos_last[n] : D->pos_first[n]) != 1) return false;
    }
    if (c->type_pos) {
        if (c->type_pos == 3) {
            if (D->type_first[n] != 1 || D->type_last[n] != 1) return false;
        } else if ((c->type_pos == 2 ? D->type_last[n] : D->type_first[n]) != 1) return false;
    }
    if (c->nth_kind) {
        bool same_type = c->nth_kind >= 3;
        bool from_end = (c->nth_kind % 2) == 0;
        int pos = same_type ? (from_end ? D->type_last[n] : D->type_first[n])
                            : (from_end ? D->pos_last[n] : D->pos_first[n]);
        if (!nth_matches(c->nth_a, c->nth_b, pos)) return false;
    }
    if (c->not_active) {
        bool hit = true;
        if (c->not_has_tag && D->tag[n] != c->not_tag) hit = false;
        if (hit && c->not_has_id && D->id[n] != c->not_id) hit = false;
        for (uint32_t i = 0; hit && i < c->ncls_n; i++)
            if (!has_atom(cls, D->cls_n[n], S->atoms[c->ncls_off + i])) hit = false;
        for (uint32_t i = 0; hit && i < c->nattr_n; i++) {
            if (c->nattr_vals) {
                if (!attr_test(S, D, n, S->atoms[c->nattr_off + i], &S->atests[c->natest_off + i]))
                    hit = false;
            } else if (!has_atom(attrs, D->attr_n[n], S->atoms[c->nattr_off + i])) hit = false;
        }
        if (hit) return false;
    }
    if (c->tag && D->tag[n] != c->tag) return false;
    if (c->where >= 0 && !list_match(S, D, c->where, n)) return false;
    if (c->has >= 0 && !has_match(S, D, c, n)) return false;
    return true;
}

/* Right-to-left with backtracking over descendant / general-sibling
 * combinators.  `budget` bounds pathological selector/DOM combinations. */
static bool chain_match_from(const CssStyleSet *S, const CssStyleDoc *D, const CComp *parts,
                             int i, int cur, int *budget)
{
    if (cur < 0 || --*budget < 0) return false;
    if (!comp_match(S, D, &parts[i], cur)) return false;
    if (i == 0) return true;
    switch (parts[i].comb) {
    case CSS_COMB_CHILD:
        return chain_match_from(S, D, parts, i - 1, D->parent[cur], budget);
    case CSS_COMB_ADJACENT:
        return chain_match_from(S, D, parts, i - 1, D->prev_elem[cur], budget);
    case CSS_COMB_SIBLING:
        for (int sib = D->prev_elem[cur]; sib >= 0; sib = D->prev_elem[sib]) {
            if (chain_match_from(S, D, parts, i - 1, sib, budget)) return true;
            if (*budget < 0) return false;
        }
        return false;
    default:
        for (int anc = D->parent[cur]; anc >= 0; anc = D->parent[anc]) {
            if (chain_match_from(S, D, parts, i - 1, anc, budget)) return true;
            if (*budget < 0) return false;
        }
        return false;
    }
}

static bool chain_match(const CssStyleSet *S, const CssStyleDoc *D, const CChain *ch, int node)
{
    if (ch->never) return false;
    int budget = 20000;
    return chain_match_from(S, D, S->comps + ch->first, ch->n - 1, node, &budget);
}

static bool list_match(const CssStyleSet *S, const CssStyleDoc *D, int32_t li, int node)
{
    const CList *l = &S->lists[li];
    for (uint32_t i = 0; i < l->n; i++) {
        if (chain_match(S, D, &S->chains[S->atoms[l->first + i]], node)) return true;
    }
    return false;
}

/* ============================================================================
 * Buckets
 * ============================================================================ */

typedef struct KeyItem { uint64_t key; uint32_t item; } KeyItem;

static int keyitem_cmp(const void *a, const void *b)
{
    const KeyItem *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->item < y->item ? -1 : (x->item > y->item);
}

static inline uint32_t key_hash(uint64_t k)
{
    k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33;
    return (uint32_t)k;
}

static void bucket_build(BucketMap *m, KeyItem *ki, size_t n, uint32_t *uni, uint32_t nuni)
{
    qsort(ki, n, sizeof(KeyItem), keyitem_cmp);
    size_t distinct = 0;
    for (size_t i = 0; i < n; i++) if (i == 0 || ki[i].key != ki[i - 1].key) distinct++;
    uint32_t cap = 16;
    while (cap < distinct * 2 + 1) cap *= 2;
    m->slots = calloc(cap, sizeof(BucketSlot));
    m->mask = cap - 1;
    m->items = malloc((n + 1) * sizeof(uint32_t));
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && ki[j].key == ki[i].key) { m->items[j] = ki[j].item; j++; }
        uint32_t h = key_hash(ki[i].key) & m->mask;
        while (m->slots[h].n) h = (h + 1) & m->mask;
        m->slots[h].key = ki[i].key;
        m->slots[h].off = (uint32_t)i;
        m->slots[h].n = (uint32_t)(j - i);
        i = j;
    }
    m->universal = uni;
    m->universal_n = nuni;
}

static inline const BucketSlot *bucket_find(const BucketMap *m, int kind, CssAtom a)
{
    uint64_t key = ((uint64_t)kind << 32) | a;
    uint32_t h = key_hash(key) & m->mask;
    while (m->slots[h].n) {
        if (m->slots[h].key == key) return &m->slots[h];
        h = (h + 1) & m->mask;
    }
    return NULL;
}

static void bucket_free(BucketMap *m)
{
    free(m->slots); free(m->items); free(m->universal);
}

/* Key of a chain's rightmost compound; false = universal bucket. */
static bool chain_key(const CssStyleSet *S, const CChain *ch, uint64_t *key)
{
    const CComp *c = &S->comps[ch->first + ch->n - 1];
    if (c->id) { *key = ((uint64_t)K_ID << 32) | c->id; return true; }
    if (c->cls_n) { *key = ((uint64_t)K_CLS << 32) | S->atoms[c->cls_off]; return true; }
    if (c->attr_n) { *key = ((uint64_t)K_ATTR << 32) | S->atoms[c->attr_off]; return true; }
    if (c->tag) { *key = ((uint64_t)K_TAG << 32) | c->tag; return true; }
    if (c->root) { *key = ((uint64_t)K_TAG << 32) | S->atom_html; return true; }
    return false;
}

/* ============================================================================
 * StyleSet build
 * ============================================================================ */

static bool chain_positional(const CssStyleSet *S, uint32_t ci, uint8_t *memo);

static bool list_positional(const CssStyleSet *S, int32_t li, uint8_t *memo)
{
    const CList *l = &S->lists[li];
    for (uint32_t k = 0; k < l->n; k++)
        if (chain_positional(S, S->atoms[l->first + k], memo)) return true;
    return false;
}

static bool chain_positional(const CssStyleSet *S, uint32_t ci, uint8_t *memo)
{
    if (memo[ci]) return memo[ci] == 2;
    memo[ci] = 1;
    const CChain *ch = &S->chains[ci];
    bool pos = false;
    if (!ch->never) {
        for (uint32_t i = 0; i < ch->n && !pos; i++) {
            const CComp *c = &S->comps[ch->first + i];
            if (c->child_pos || c->type_pos || c->nth_kind || c->empty ||
                c->comb == CSS_COMB_ADJACENT || c->comb == CSS_COMB_SIBLING || c->has >= 0) pos = true;
            else if (c->where >= 0 && list_positional(S, c->where, memo)) pos = true;
        }
    }
    memo[ci] = pos ? 2 : 1;
    return pos;
}

typedef struct RuleCtx { int32_t g; I32Vec *out; } RuleCtx;
static void rule_branch(Builder *b, const char *branch, void *ud)
{
    RuleCtx *rc = ud;
    int32_t ch = compile_chain(b, branch, rc->g);
    if (ch >= 0) VEC_PUSH(*rc->out, ch);
}

/* Legacy ::after branch parsing (layout_pseudo_branch_matches): the branch
 * must end with "::after" (or ":after"); the base before it is compiled. */
static int32_t after_base_list(Builder *b, const char *branch, size_t blen, bool before)
{
    char buf[512];
    if (blen == 0 || blen >= sizeof(buf)) return -1;
    memcpy(buf, branch, blen);
    buf[blen] = '\0';
    const char *kw = before ? "::before" : "::after";
    char *pseudo = strstr(buf, kw);
    size_t kwlen = strlen(kw);
    if (!pseudo) { kw = before ? ":before" : ":after"; pseudo = strstr(buf, kw); kwlen = strlen(kw); }
    if (!pseudo) return -1;
    char *tail = pseudo + kwlen;
    while (*tail && isspace((unsigned char)*tail)) tail++;
    if (*tail) return -1;
    *pseudo = '\0';
    size_t l = strlen(buf);
    while (l > 0 && isspace((unsigned char)buf[l - 1])) buf[--l] = '\0';
    char *base = buf;
    while (*base && isspace((unsigned char)*base)) base++;
    if (!*base) return -1;
    return compile_list(b, base);
}

CssStyleSet *css_styleset_build(CssStylesheet *const *sheets, int count,
                                double vw, double vh)
{
    CssStyleSet *S = calloc(1, sizeof(*S));
    Builder b = {0};
    b.parts = malloc(sizeof(CssSelectorPart) * CSS_MAX_SIMPLE_PARTS);

    size_t est = 64;
    int nrules = 0;
    for (int s = 0; s < count; s++) {
        if (!sheets[s]) continue;
        nrules += sheets[s]->rule_count;
        for (int r = 0; r < sheets[s]->rule_count; r++)
            if (sheets[s]->rules[r].selector_text)
                est += strlen(sheets[s]->rules[r].selector_text) / 2 + 2;
    }
    css_atom_reserve(est);
    S->atom_html = atom_cstr("html");

    S->rules = calloc((size_t)nrules + 1, sizeof(CRule));
    VEC(KeyItem) keys = {0};
    VEC(uint32_t) uni = {0};
    VEC(KeyItem) akeys = {0};
    VEC(uint32_t) auni = {0};
    VEC(CAfter) afters = {0};
    I32Vec rchains = {0};

    int g = 0;
    for (int s = 0; s < count; s++) {
        CssStylesheet *sheet = sheets[s];
        if (!sheet) continue;
        for (int r = 0; r < sheet->rule_count; r++, g++) {
            CssRule *rule = &sheet->rules[r];
            CRule *cr = &S->rules[g];
            cr->rule = rule;
            cr->sheet = s;
            cr->index = r;
            cr->aspect = -1;
            if (!rule->selector_text || !rule->selector_text[0]) continue;
            const char *sel = rule->selector_text;

            /* Aspect base: everything before the first ":before"/":after". */
            const char *pseudo = strstr(sel, ":before");
            if (!pseudo) pseudo = strstr(sel, ":after");
            if (pseudo) {
                size_t base_len = (size_t)(pseudo - sel);
                while (base_len > 0 && isspace((unsigned char)sel[base_len - 1])) base_len--;
                size_t start = 0;
                while (start < base_len && isspace((unsigned char)sel[start])) start++;
                if (base_len > start) {
                    char *base = malloc(base_len - start + 1);
                    memcpy(base, sel + start, base_len - start);
                    base[base_len - start] = '\0';
                    cr->aspect = compile_list(&b, base);
                    free(base);
                }
            }

            if (!css_rule_media_matches(rule, vw, vh)) continue;

            /* Normal branches into the rule buckets. */
            rchains.n = 0;
            RuleCtx rc = { g, &rchains };
            for_each_branch(&b, sel, rule_branch, &rc);
            for (size_t k = 0; k < rchains.n; k++) {
                const CChain *ch = &b.chains.v[rchains.v[k]];
                if (ch->never) continue;
                /* Key from the builder arrays (S not populated yet). */
                CssStyleSet tmp = { .comps = b.comps.v, .atoms = b.atoms.v, .atests = b.atests.v, .atom_html = S->atom_html };
                uint64_t key;
                if (chain_key(&tmp, ch, &key)) { KeyItem ki = { key, (uint32_t)rchains.v[k] }; VEC_PUSH(keys, ki); }
                else VEC_PUSH(uni, (uint32_t)rchains.v[k]);
            }

            /* ::after branches (naive comma split, as the legacy code did). */
            if (strstr(sel, ":after") || strstr(sel, ":before")) {
                const char *bp = sel;
                while (*bp) {
                    const char *comma = strchr(bp, ',');
                    size_t blen = comma ? (size_t)(comma - bp) : strlen(bp);
                    while (blen > 0 && isspace((unsigned char)*bp)) { bp++; blen--; }
                    while (blen > 0 && isspace((unsigned char)bp[blen - 1])) blen--;
                    if (blen > 0) {
                        int32_t is_before = 0;
                        int32_t li = after_base_list(&b, bp, blen, false);
                        if (li < 0) { li = after_base_list(&b, bp, blen, true); is_before = 1; }
                        if (li >= 0) {
                            CAfter af = { g, 0, is_before };
                            if (blen < 512) {
                                char specbuf[512];
                                memcpy(specbuf, bp, blen);
                                specbuf[blen] = '\0';
                                int pc = css_sel_parse_chain(specbuf, b.parts, CSS_MAX_SIMPLE_PARTS);
                                af.spec = css_sel_chain_specificity(b.parts, pc);
                            }
                            int32_t aid = (int32_t)afters.n;
                            VEC_PUSH(afters, af);
                            CList l = b.lists.v[li];
                            for (uint32_t k = 0; k < l.n; k++) {
                                int32_t ci = (int32_t)b.atoms.v[l.first + k];
                                CChain *ch = &b.chains.v[ci];
                                ch->owner = aid;
                                if (ch->never) continue;
                                CssStyleSet tmp = { .comps = b.comps.v, .atoms = b.atoms.v, .atests = b.atests.v, .atom_html = S->atom_html };
                                uint64_t key;
                                if (chain_key(&tmp, ch, &key)) { KeyItem ki = { key, (uint32_t)ci }; VEC_PUSH(akeys, ki); }
                                else VEC_PUSH(auni, (uint32_t)ci);
                            }
                        }
                    }
                    if (!comma) break;
                    bp = comma + 1;
                }
            }
        }
    }
    free(rchains.v);
    S->nrules = g;
    S->comps = b.comps.v; S->ncomps = b.comps.n;
    S->chains = b.chains.v; S->nchains = b.chains.n;
    S->lists = b.lists.v; S->nlists = b.lists.n;
    S->atoms = b.atoms.v; S->natoms = b.atoms.n;
    S->atests = b.atests.v; S->natests = b.atests.n;
    S->afters = afters.v; S->nafters = (int)afters.n;
    /* Revalidation chains: every chain whose result can differ between two
     * elements with equal features and equivalent parents — those testing
     * sibling position or :empty, directly or inside :where()/:is(). */
    {
        I32Vec rv = {0};
        uint8_t *memo = calloc(S->nchains + 1, 1);   /* 0 unknown, 1 no, 2 yes */
        for (size_t i = 0; i < keys.n; i++) if (chain_positional(S, keys.v[i].item, memo)) VEC_PUSH(rv, (int32_t)keys.v[i].item);
        for (size_t i = 0; i < uni.n; i++) if (chain_positional(S, uni.v[i], memo)) VEC_PUSH(rv, (int32_t)uni.v[i]);
        for (size_t i = 0; i < akeys.n; i++) if (chain_positional(S, akeys.v[i].item, memo)) VEC_PUSH(rv, (int32_t)akeys.v[i].item);
        for (size_t i = 0; i < auni.n; i++) if (chain_positional(S, auni.v[i], memo)) VEC_PUSH(rv, (int32_t)auni.v[i]);
        for (int g2 = 0; g2 < S->nrules; g2++) {
            if (S->rules[g2].aspect < 0) continue;
            const CList *l = &S->lists[S->rules[g2].aspect];
            for (uint32_t k = 0; k < l->n; k++) {
                uint32_t ci = S->atoms[l->first + k];
                if (chain_positional(S, ci, memo)) VEC_PUSH(rv, (int32_t)ci);
            }
        }
        free(memo);
        S->reval = (uint32_t *)rv.v;
        S->nreval = (uint32_t)rv.n;
    }
    bucket_build(&S->rule_buckets, keys.v, keys.n, uni.v, (uint32_t)uni.n);
    bucket_build(&S->after_buckets, akeys.v, akeys.n, auni.v, (uint32_t)auni.n);
    free(keys.v);
    free(akeys.v);
    free(b.parts);
    return S;
}

void css_styleset_free(CssStyleSet *S)
{
    if (!S) return;
    for (size_t i = 0; i < S->natests; i++) free(S->atests[i].val);
    free(S->atests);
    free(S->comps); free(S->chains); free(S->lists); free(S->atoms);
    free(S->rules); free(S->afters); free(S->reval);
    bucket_free(&S->rule_buckets);
    bucket_free(&S->after_buckets);
    free(S);
}

int css_styleset_rule_count(const CssStyleSet *S) { return S->nrules; }
const CssRule *css_styleset_rule(const CssStyleSet *S, int g) { return S->rules[g].rule; }
int css_styleset_rule_order(const CssStyleSet *S, int g)
{
    return (int)((uint32_t)S->rules[g].sheet * 1000000u + (uint32_t)S->rules[g].index * 1000u);
}

void css_styleset_stats(const CssStyleSet *S, int *chains, int *universal)
{
    if (chains) *chains = (int)S->nchains;
    if (universal) *universal = (int)S->rule_buckets.universal_n;
}

bool css_style_rule_has_aspect_base(const CssStyleSet *S, int g)
{
    return g >= 0 && g < S->nrules && S->rules[g].aspect >= 0;
}

bool css_style_aspect_base_matches(const CssStyleSet *S, const CssStyleDoc *D, int g, int dom)
{
    if (!css_style_rule_has_aspect_base(S, g)) return false;
    return list_match(S, D, S->rules[g].aspect, dom);
}

/* ============================================================================
 * Scratch, Bloom filter, matching entry points
 * ============================================================================ */

#define BLOOM_BITS 4096
struct CssStyleScratch {
    const CssStyleSet *set;
    uint32_t stamp;
    uint32_t *rule_stamp;
    int32_t *rule_spec;
    I32Vec matched;
    VEC(CssAppliedDecl) applied;
    uint32_t *after_stamp;
    I32Vec after_hits;
    VEC(CssAfterMatch) after_out;
    uint16_t bloom[BLOOM_BITS];
    I32Vec stack;
    CssMatchStats stats;
};

CssStyleScratch *css_style_scratch_new(const CssStyleSet *S)
{
    CssStyleScratch *t = calloc(1, sizeof(*t));
    t->set = S;
    t->rule_stamp = calloc((size_t)S->nrules + 1, sizeof(uint32_t));
    t->rule_spec = calloc((size_t)S->nrules + 1, sizeof(int32_t));
    t->after_stamp = calloc((size_t)S->nafters + 1, sizeof(uint32_t));
    return t;
}

void css_style_scratch_free(CssStyleScratch *t)
{
    if (!t) return;
    free(t->rule_stamp); free(t->rule_spec); free(t->matched.v); free(t->applied.v);
    free(t->after_stamp); free(t->after_hits.v); free(t->after_out.v); free(t->stack.v);
    free(t);
}

const CssMatchStats *css_style_scratch_stats(const CssStyleScratch *t) { return &t->stats; }

static inline void bloom_bits(CssAtom a, uint32_t *b1, uint32_t *b2)
{
    uint32_t h = a * 0x9E3779B1u;
    *b1 = h & (BLOOM_BITS - 1);
    *b2 = (h >> 16) & (BLOOM_BITS - 1);
}

static void bloom_add_node(CssStyleScratch *t, const CssStyleDoc *D, int n, int delta)
{
    if (n < 0 || !D->is_elem[n]) return;
    CssAtom feats[3];
    int nf = 0;
    feats[nf++] = D->tag[n];
    if (D->id[n]) feats[nf++] = D->id[n];
    for (int i = 0; i < nf; i++) {
        uint32_t b1, b2;
        bloom_bits(feats[i], &b1, &b2);
        t->bloom[b1] += (uint16_t)delta;
        t->bloom[b2] += (uint16_t)delta;
    }
    const CssAtom *cls = D->pool + D->cls_off[n];
    for (uint32_t i = 0; i < D->cls_n[n]; i++) {
        uint32_t b1, b2;
        bloom_bits(cls[i], &b1, &b2);
        t->bloom[b1] += (uint16_t)delta;
        t->bloom[b2] += (uint16_t)delta;
    }
}

void css_style_bloom_enter(CssStyleScratch *t, const CssStyleDoc *D, int parent_dom)
{
    while (t->stack.n > 0 && t->stack.v[t->stack.n - 1] != parent_dom) {
        bloom_add_node(t, D, t->stack.v[--t->stack.n], -1);
    }
    if (t->stack.n == 0 && parent_dom >= 0) {
        /* Started on an unrelated subtree: rebuild from the ancestor chain. */
        I32Vec chain = {0};
        for (int p = parent_dom; p >= 0; p = D->parent[p]) {
            VEC_PUSH(chain, p);
            if (chain.n > 8192) break;
        }
        for (size_t i = chain.n; i > 0; i--) {
            VEC_PUSH(t->stack, chain.v[i - 1]);
            bloom_add_node(t, D, chain.v[i - 1], +1);
        }
        free(chain.v);
    }
}

void css_style_bloom_push(CssStyleScratch *t, const CssStyleDoc *D, int dom)
{
    VEC_PUSH(t->stack, dom);
    bloom_add_node(t, D, dom, +1);
}

static inline bool bloom_ok(const CssStyleScratch *t, const CChain *ch)
{
    for (int i = 0; i < ch->bloom_n; i++) {
        uint32_t b1, b2;
        bloom_bits(ch->bloom[i], &b1, &b2);
        if (!t->bloom[b1] || !t->bloom[b2]) return false;
    }
    return true;
}

/* Visit every candidate chain for node `n` in map `m`. */
typedef void (*CandFn)(CssStyleScratch *t, const CChain *ch, int32_t ci);
static void for_candidates(const CssStyleSet *S, const CssStyleDoc *D, const BucketMap *m,
                           int n, CssStyleScratch *t, int dom, CandFn fn)
{
    (void)dom;
#define VISIT_SLOT(slot) do { const BucketSlot *_s = (slot); if (_s) { \
        for (uint32_t _i = 0; _i < _s->n; _i++) { uint32_t ci = m->items[_s->off + _i]; \
            const CChain *ch = &S->chains[ci]; t->stats.candidates++; \
            if (!bloom_ok(t, ch)) { t->stats.bloom_rejects++; continue; } \
            t->stats.full_matches++; \
            if (chain_match(S, D, ch, n)) fn(t, ch, (int32_t)ci); } } } while (0)
    if (D->id[n]) VISIT_SLOT(bucket_find(m, K_ID, D->id[n]));
    const CssAtom *cls = D->pool + D->cls_off[n];
    for (uint32_t i = 0; i < D->cls_n[n]; i++) {
        /* A duplicated class token would visit its bucket twice; skip it. */
        bool dup = false;
        for (uint32_t j = 0; j < i; j++) if (cls[j] == cls[i]) { dup = true; break; }
        if (!dup) VISIT_SLOT(bucket_find(m, K_CLS, cls[i]));
    }
    const CssAtom *attrs = D->pool + D->attr_off[n];
    for (uint32_t i = 0; i < D->attr_n[n]; i++) {
        bool dup = false;
        for (uint32_t j = 0; j < i; j++) if (attrs[j] == attrs[i]) { dup = true; break; }
        if (!dup) VISIT_SLOT(bucket_find(m, K_ATTR, attrs[i]));
    }
    VISIT_SLOT(bucket_find(m, K_TAG, D->tag[n]));
    for (uint32_t i = 0; i < m->universal_n; i++) {
        uint32_t ci = m->universal[i];
        const CChain *ch = &S->chains[ci];
        t->stats.candidates++;
        if (!bloom_ok(t, ch)) { t->stats.bloom_rejects++; continue; }
        t->stats.full_matches++;
        if (chain_match(S, D, ch, n)) fn(t, ch, (int32_t)ci);
    }
#undef VISIT_SLOT
}

static void on_rule_hit(CssStyleScratch *t, const CChain *ch, int32_t ci)
{
    (void)ci;
    int32_t g = ch->owner;
    if (t->rule_stamp[g] != t->stamp) {
        t->rule_stamp[g] = t->stamp;
        t->rule_spec[g] = ch->spec;
        VEC_PUSH(t->matched, g);
    } else if (ch->spec > t->rule_spec[g]) {
        t->rule_spec[g] = ch->spec;
    }
}

int css_style_match(const CssStyleSet *S, const CssStyleDoc *D, int dom,
                    CssStyleScratch *t, const CssAppliedDecl **out)
{
    *out = NULL;
    if (dom < 0 || dom >= D->count || !D->is_elem[dom]) return 0;
    if (++t->stamp == 0) {
        memset(t->rule_stamp, 0, (size_t)S->nrules * sizeof(uint32_t));
        memset(t->after_stamp, 0, (size_t)S->nafters * sizeof(uint32_t));
        t->stamp = 1;
    }
    t->stats.nodes++;
    t->matched.n = 0;
    for_candidates(S, D, &S->rule_buckets, dom, t, dom, on_rule_hit);
    t->stats.matched_rules += t->matched.n;
    t->applied.n = 0;
    for (size_t k = 0; k < t->matched.n; k++) {
        int32_t g = t->matched.v[k];
        const CRule *cr = &S->rules[g];
        const CssRule *rule = cr->rule;
        for (int d = 0; d < rule->declaration_count; d++) {
            CssAppliedDecl a;
            a.decl = &rule->declarations[d];
            a.specificity = t->rule_spec[g];
            a.order = (int)((uint32_t)cr->sheet * 1000000u + (uint32_t)cr->index * 1000u + (uint32_t)d);
            a.important = rule->declarations[d].important;
            a.rule = g;
            VEC_PUSH(t->applied, a);
        }
    }
    if (t->applied.n > 1)
        qsort(t->applied.v, t->applied.n, sizeof(CssAppliedDecl), css_applied_decl_compare);
    *out = t->applied.v;
    return (int)t->applied.n;
}

static void on_after_hit(CssStyleScratch *t, const CChain *ch, int32_t ci)
{
    (void)ci;
    int32_t a = ch->owner;
    if (t->after_stamp[a] != t->stamp) {
        t->after_stamp[a] = t->stamp;
        VEC_PUSH(t->after_hits, a);
    }
}

int css_style_match_after(const CssStyleSet *S, const CssStyleDoc *D, int dom,
                          CssStyleScratch *t, const CssAfterMatch **out)
{
    *out = NULL;
    t->after_out.n = 0;
    if (S->nafters == 0 || dom < 0 || dom >= D->count || !D->is_elem[dom]) return 0;
    /* Shares the stamp of the preceding css_style_match call for `dom`. */
    t->after_hits.n = 0;
    for_candidates(S, D, &S->after_buckets, dom, t, dom, on_after_hit);
    /* Branch ids increase with (rule, branch order): sort, then keep the
     * first matching branch of each rule. */
    for (size_t i = 1; i < t->after_hits.n; i++) {
        int32_t v = t->after_hits.v[i];
        size_t j = i;
        while (j > 0 && t->after_hits.v[j - 1] > v) { t->after_hits.v[j] = t->after_hits.v[j - 1]; j--; }
        t->after_hits.v[j] = v;
    }
    int32_t last_rule = -1;
    for (size_t i = 0; i < t->after_hits.n; i++) {
        const CAfter *af = &S->afters[t->after_hits.v[i]];
        if (af->rule == last_rule) continue;
        last_rule = af->rule;
        CssAfterMatch m = { af->rule, af->spec, af->is_before };
        VEC_PUSH(t->after_out, m);
    }
    *out = t->after_out.v;
    return (int)t->after_out.n;
}

/* ============================================================================
 * Style sharing support (§7.5)
 * ============================================================================ */

int css_style_reval_count(const CssStyleSet *S) { return (int)S->nreval; }

void css_style_reval_bits(const CssStyleSet *S, const CssStyleDoc *D, int dom, uint64_t *bits)
{
    uint32_t words = (S->nreval + 63) / 64;
    memset(bits, 0, words * sizeof(uint64_t));
    for (uint32_t i = 0; i < S->nreval; i++)
        if (chain_match(S, D, &S->chains[S->reval[i]], dom)) bits[i >> 6] |= 1ull << (i & 63);
}

bool css_style_shareable(const CssStyleDoc *D, int dom)
{
    return dom >= 0 && dom < D->count && D->is_elem[dom] && D->id[dom] == CSS_ATOM_NONE;
}

bool css_style_features_equal(const CssStyleDoc *D, int a, int b)
{
    if (D->tag[a] != D->tag[b] || D->id[a] != D->id[b]) return false;
    if ((D->flags[a] & DF_LINK) != (D->flags[b] & DF_LINK)) return false;
    if (D->cls_n[a] != D->cls_n[b] || D->attr_n[a] != D->attr_n[b]) return false;
    if (memcmp(D->pool + D->cls_off[a], D->pool + D->cls_off[b], D->cls_n[a] * sizeof(CssAtom))) return false;
    if (memcmp(D->pool + D->attr_off[a], D->pool + D->attr_off[b], D->attr_n[a] * sizeof(CssAtom))) return false;
    for (uint32_t k = 0; k < D->attr_n[a]; k++) {
        const char *va = D->pool_val[D->attr_off[a] + k], *vb = D->pool_val[D->attr_off[b] + k];
        if (va != vb && strcmp(va ? va : "", vb ? vb : "") != 0) return false;
    }
    return true;
}

CssAtom css_style_tag(const CssStyleDoc *D, int dom) { return D->tag[dom]; }

/* Node store.  See dom_store.h and PARALLEL_DOM.md §4. */
#include "dom_store.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

DomStore g_dom;

/* ============================================================================
 * Arena blocks: reference counted, registered in a two-level table so jobs can
 * create blocks concurrently (CAS-installed pages, atomic id counter).
 * ============================================================================ */

#define BLK_PAGE_BITS 10u
#define BLK_PAGE_SIZE (1u << BLK_PAGE_BITS)
#define BLK_PAGES     4096u
#define BLK_DEFAULT   (64u * 1024u)

typedef struct DomBlock {
    char *mem;
    uint32_t refs;
} DomBlock;

static _Atomic(DomBlock *) g_blk_pages[BLK_PAGES];
static _Atomic uint32_t g_blk_next = 1;
static _Atomic uint32_t g_blk_free_head;   /* engine thread only (released ids) */
static _Atomic size_t g_blk_live;

static DomBlock *blk_get(uint32_t id) {
    DomBlock *page = atomic_load_explicit(&g_blk_pages[id >> BLK_PAGE_BITS], memory_order_acquire);
    return page ? &page[id & (BLK_PAGE_SIZE - 1)] : NULL;
}

static uint32_t blk_new(size_t size) {
    uint32_t id = atomic_fetch_add_explicit(&g_blk_next, 1, memory_order_relaxed);
    uint32_t p = id >> BLK_PAGE_BITS;
    if (p >= BLK_PAGES) { fprintf(stderr, "dom_store: arena block table full\n"); abort(); }
    DomBlock *page = atomic_load_explicit(&g_blk_pages[p], memory_order_acquire);
    if (!page) {
        DomBlock *fresh = calloc(BLK_PAGE_SIZE, sizeof(DomBlock));
        DomBlock *expect = NULL;
        if (atomic_compare_exchange_strong_explicit(&g_blk_pages[p], &expect, fresh,
                                                    memory_order_acq_rel, memory_order_acquire))
            page = fresh;
        else { free(fresh); page = expect; }
    }
    DomBlock *b = &page[id & (BLK_PAGE_SIZE - 1)];
    b->mem = malloc(size);
    b->refs = 1;                                   /* the arena's own hold */
    atomic_fetch_add_explicit(&g_blk_live, 1, memory_order_relaxed);
    return id;
}

void dom_block_retain(uint32_t blk) {
    if (blk) blk_get(blk)->refs++;
}

void dom_block_release(uint32_t blk) {
    if (!blk) return;
    DomBlock *b = blk_get(blk);
    if (b && b->refs && --b->refs == 0) {
        free(b->mem);
        b->mem = NULL;
        atomic_fetch_sub_explicit(&g_blk_live, 1, memory_order_relaxed);
    }
}

size_t dom_blocks_live(void) { return atomic_load_explicit(&g_blk_live, memory_order_relaxed); }

void *dom_arena_alloc(DomArena *ar, size_t n, uint32_t *blk_out) {
    n = (n + 7) & ~(size_t)7;
    if (!ar->blk || ar->used + n > ar->cap) {
        if (n > BLK_DEFAULT / 4) {
            /* Large allocation: a block of its own, released with its user. */
            uint32_t id = blk_new(n);
            DomBlock *b = blk_get(id);
            *blk_out = id;              /* the arena's hold becomes the user's */
            return b->mem;
        }
        size_t size = ar->next_size ? ar->next_size : BLK_DEFAULT;
        if (size < 256) size = 256;
        while (size < n) size *= 2;
        dom_arena_finish(ar);
        ar->blk = blk_new(size);
        ar->base = blk_get(ar->blk)->mem;
        ar->used = 0;
        ar->cap = size;
        ar->next_size = size * 2 < BLK_DEFAULT ? size * 2 : BLK_DEFAULT;
    }
    void *p = ar->base + ar->used;
    ar->used += n;
    blk_get(ar->blk)->refs++;
    *blk_out = ar->blk;
    return p;
}

DomStr dom_arena_str(DomArena *ar, const char *s, size_t n) {
    DomStr r;
    if (n == 0) { r.s = NULL; r.len = 0; r.blk = 0; return r; }
    r.s = dom_arena_alloc(ar, n + 1, &r.blk);
    memcpy(r.s, s, n);
    r.s[n] = '\0';
    r.len = (uint32_t)n;
    return r;
}

void dom_arena_finish(DomArena *ar) {
    if (ar->blk) dom_block_release(ar->blk);
    ar->blk = 0;
    ar->base = NULL;
    ar->used = ar->cap = 0;
}

DomStr dom_str_dup(const char *s, size_t n) {
    DomStr r = { NULL, 0, 0 };
    if (!s || n == 0) return r;
    r.s = malloc(n + 1);
    memcpy(r.s, s, n);
    r.s[n] = '\0';
    r.len = (uint32_t)n;
    return r;
}

void dom_str_release(DomStr *str) {
    if (str->blk) dom_block_release(str->blk);
    else free(str->s);
    str->s = NULL;
    str->len = 0;
    str->blk = 0;
}

/* ============================================================================
 * Slots
 * ============================================================================ */

#define GROW(field, type) do { \
    void *np = realloc(g_dom.field, (size_t)cap * sizeof(type)); \
    if (!np) { fprintf(stderr, "dom_store: out of memory\n"); abort(); } \
    g_dom.field = np; \
    memset((type *)g_dom.field + old, 0, (size_t)(cap - old) * sizeof(type)); \
} while (0)

static void store_grow(uint32_t need) {
    if (need <= g_dom.cap) return;
    uint32_t old = g_dom.cap;
    uint32_t cap = old ? old : 1024;
    while (cap < need) cap *= 2;
    if (cap > DOM_SLOT_MASK) { fprintf(stderr, "dom_store: too many nodes\n"); abort(); }
    GROW(kind, uint8_t);
    GROW(gen, uint8_t);
    GROW(flags, uint16_t);
    GROW(tag, CssAtom);
    GROW(parent, uint32_t);
    GROW(first, uint32_t);
    GROW(last, uint32_t);
    GROW(next, uint32_t);
    GROW(prev, uint32_t);
    GROW(host, uint32_t);
    GROW(shadow, uint32_t);
    GROW(content, uint32_t);
    GROW(owner, uint32_t);
    GROW(tree, uint32_t);
    GROW(wrapper, uint32_t);
    GROW(wrap_idx, uint32_t);
    GROW(text, DomStr);
    GROW(attrs, DomAttrList);
    GROW(id_atom, CssAtom);
    GROW(order, uint32_t);
    g_dom.cap = cap;
}

void dom_store_init(void) {
    if (g_dom.cap) return;
    store_grow(1024);
    g_dom.count = 1;               /* slot 0 = none */
    g_dom.trees_cap = 64;
    g_dom.trees = calloc(g_dom.trees_cap, sizeof(DomTreeRec));
    g_dom.ntrees = 1;              /* record 0 = none */
}

void dom_store_reserve(uint32_t extra) {
    if (!g_dom.cap) dom_store_init();
    store_grow(g_dom.count + extra + 1);
}

uint32_t dom_alloc_slot(int kind, NodeId owner) {
    if (!g_dom.cap) dom_store_init();
    uint32_t s = g_dom.free_head;
    if (s) {
        g_dom.free_head = g_dom.next[s];
    } else {
        store_grow(g_dom.count + 1);
        s = g_dom.count++;
    }
    g_dom.kind[s] = (uint8_t)kind;
    g_dom.flags[s] = 0;
    g_dom.tag[s] = CSS_ATOM_NONE;
    g_dom.parent[s] = g_dom.first[s] = g_dom.last[s] = g_dom.next[s] = g_dom.prev[s] = 0;
    g_dom.host[s] = g_dom.shadow[s] = g_dom.content[s] = 0;
    g_dom.owner[s] = dom_valid(owner) ? dom_slot(owner) : 0;
    g_dom.tree[s] = 0;
    g_dom.wrapper[s] = 0;
    g_dom.wrap_idx[s] = 0;
    g_dom.text[s] = (DomStr){ NULL, 0, 0 };
    g_dom.attrs[s] = (DomAttrList){ NULL, 0, 0, 0 };
    g_dom.id_atom[s] = CSS_ATOM_NONE;
    g_dom.order[s] = ++g_dom.seq;
    g_dom.live++;
    return s;
}

static void attrs_release(DomAttrList *l) {
    for (int i = 0; i < l->n; i++) dom_str_release(&l->a[i].value);
    if (l->blk) dom_block_release(l->blk);
    else free(l->a);
    *l = (DomAttrList){ NULL, 0, 0, 0 };
}

void dom_free_slot(uint32_t s) {
    if (!s || s >= g_dom.count || g_dom.kind[s] == DOM_K_FREE) return;
    dom_str_release(&g_dom.text[s]);
    attrs_release(&g_dom.attrs[s]);
    if (g_dom.tree[s]) {
        DomTreeRec *t = &g_dom.trees[g_dom.tree[s]];
        if (t->root == dom_id_of(s)) t->root = DOM_NULL;   /* the tree object outlives its root */
        g_dom.tree[s] = 0;
    }
    g_dom.kind[s] = DOM_K_FREE;
    g_dom.gen[s]++;
    g_dom.wrapper[s] = 0;
    g_dom.next[s] = g_dom.free_head;
    g_dom.free_head = s;
    g_dom.live--;
}

void dom_store_reset(void) {
    for (uint32_t s = 1; s < g_dom.count; s++)
        if (g_dom.kind[s] != DOM_K_FREE) {
            g_dom.tree[s] = 0;
            dom_free_slot(s);
        }
    memset(g_dom.trees, 0, g_dom.trees_cap * sizeof(DomTreeRec));
    g_dom.ntrees = 1;
    g_dom.tree_free = 0;
    g_dom.structure_version++;
}

/* ============================================================================
 * Creation
 * ============================================================================ */

static const char *const k_void_tags[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input",
    "link", "meta", "param", "source", "track", "wbr", NULL
};
static const char *const k_raw_tags[] = { "script", "style", "textarea", "title", NULL };

static uint16_t *g_tag_flags;      /* by atom id; bit 15 = computed */
static uint32_t g_tag_flags_cap;

uint16_t dom_tag_flags(CssAtom tag) {
    if (tag < g_tag_flags_cap && (g_tag_flags[tag] & 0x8000u)) return g_tag_flags[tag] & 0x7FFFu;
    const char *s = css_atom_str(tag);
    uint16_t f = 0;
    if (s && *s) {
        if (strchr(s, '-')) f |= DOMF_CE_CANDIDATE;
        for (int i = 0; k_void_tags[i]; i++) if (strcmp(s, k_void_tags[i]) == 0) { f |= DOMF_VOID; break; }
        for (int i = 0; k_raw_tags[i]; i++) if (strcmp(s, k_raw_tags[i]) == 0) { f |= DOMF_RAW_TEXT; break; }
    }
    if (tag >= g_tag_flags_cap) {
        uint32_t cap = g_tag_flags_cap ? g_tag_flags_cap : 1024;
        while (cap <= tag) cap *= 2;
        g_tag_flags = realloc(g_tag_flags, cap * sizeof(uint16_t));
        memset(g_tag_flags + g_tag_flags_cap, 0, (cap - g_tag_flags_cap) * sizeof(uint16_t));
        g_tag_flags_cap = cap;
    }
    g_tag_flags[tag] = (uint16_t)(f | 0x8000u);
    return f;
}

NodeId dom_create_document(void) {
    uint32_t s = dom_alloc_slot(DOM_K_DOCUMENT, DOM_NULL);
    g_dom.owner[s] = s;
    return dom_id_of(s);
}

NodeId dom_create_element(CssAtom tag, NodeId owner) {
    uint32_t s = dom_alloc_slot(DOM_K_ELEMENT, owner);
    g_dom.tag[s] = tag;
    g_dom.flags[s] = dom_tag_flags(tag);
    return dom_id_of(s);
}

NodeId dom_create_element_str(const char *tag, size_t n, NodeId owner) {
    css_atom_reserve(1);
    return dom_create_element(css_atom_intern(tag, n), owner);
}

NodeId dom_create_chardata(int kind, const char *s, size_t n, NodeId owner) {
    uint32_t slot = dom_alloc_slot(kind, owner);
    g_dom.text[slot] = dom_str_dup(s, n);
    return dom_id_of(slot);
}

NodeId dom_create_fragment(NodeId owner) {
    return dom_id_of(dom_alloc_slot(DOM_K_FRAGMENT, owner));
}

/* ============================================================================
 * Reads
 * ============================================================================ */

const char *dom_tag_name(NodeId id) {
    if (!dom_valid(id)) return "";
    const char *s = css_atom_str(g_dom.tag[dom_slot(id)]);
    return s ? s : "";
}

const char *dom_text(NodeId id, uint32_t *len) {
    if (!dom_valid(id)) { if (len) *len = 0; return ""; }
    DomStr *t = &g_dom.text[dom_slot(id)];
    if (len) *len = t->len;
    return t->s ? t->s : "";
}

uint32_t dom_tree_root_slot(uint32_t s) {
    for (;;) {
        while (g_dom.parent[s]) s = g_dom.parent[s];
        if (g_dom.host[s]) { s = g_dom.host[s]; continue; }
        return s;
    }
}

NodeId dom_root(NodeId id) {
    if (!dom_valid(id)) return DOM_NULL;
    uint32_t s = dom_slot(id);
    while (g_dom.parent[s]) s = g_dom.parent[s];
    return dom_id_of(s);
}

bool dom_is_connected(NodeId id) {
    if (!dom_valid(id)) return false;
    uint32_t s = dom_slot(id);
    for (;;) {
        while (g_dom.parent[s]) s = g_dom.parent[s];
        if (g_dom.kind[s] == DOM_K_DOCUMENT) return true;
        /* Shadow roots continue to their host; template contents never
         * render, so they are not connected. */
        if ((g_dom.flags[s] & DOMF_SHADOW_ROOT) && g_dom.host[s]) { s = g_dom.host[s]; continue; }
        return false;
    }
}

bool dom_contains(NodeId a, NodeId b) {
    if (!dom_valid(a) || !dom_valid(b)) return false;
    uint32_t sa = dom_slot(a);
    for (uint32_t s = dom_slot(b); s; s = g_dom.parent[s])
        if (s == sa) return true;
    return false;
}

/* ============================================================================
 * Attributes
 * ============================================================================ */

int dom_attr_count(NodeId id) { return dom_valid(id) ? g_dom.attrs[dom_slot(id)].n : 0; }

const DomAttr *dom_attr_at(NodeId id, int i) {
    if (!dom_valid(id)) return NULL;
    DomAttrList *l = &g_dom.attrs[dom_slot(id)];
    return (i >= 0 && i < l->n) ? &l->a[i] : NULL;
}

const char *dom_get_attr(NodeId id, CssAtom name, uint32_t *len) {
    if (!dom_valid(id) || !name) return NULL;
    DomAttrList *l = &g_dom.attrs[dom_slot(id)];
    for (int i = 0; i < l->n; i++)
        if (l->a[i].name == name) {
            if (len) *len = l->a[i].value.len;
            return l->a[i].value.s ? l->a[i].value.s : "";
        }
    return NULL;
}

const char *dom_get_attr_str(NodeId id, const char *name) {
    CssAtom a = css_atom_find(name, strlen(name));
    return a ? dom_get_attr(id, a, NULL) : NULL;
}

static void attrs_make_owned(DomAttrList *l, int need) {
    if (!l->blk && l->cap >= need) return;
    int cap = need < 4 ? 4 : need;
    if (cap < 2 * l->n) cap = 2 * l->n;
    if (cap > 0xFFFF) cap = 0xFFFF;
    DomAttr *a = malloc((size_t)cap * sizeof(DomAttr));
    if (l->n) memcpy(a, l->a, (size_t)l->n * sizeof(DomAttr));
    if (l->blk) dom_block_release(l->blk);
    else free(l->a);
    l->a = a;
    l->cap = (uint16_t)cap;
    l->blk = 0;
}

void dom_set_attr(NodeId id, CssAtom name, const char *value, size_t n) {
    if (!dom_valid(id) || !name) return;
    uint32_t s = dom_slot(id);
    DomAttrList *l = &g_dom.attrs[s];
    int i;
    for (i = 0; i < l->n; i++) if (l->a[i].name == name) break;
    if (i == l->n) {
        if (l->n == 0xFFFF) return;
        attrs_make_owned(l, l->n + 1);
        l->a[l->n].name = name;
        l->a[l->n].value = (DomStr){ NULL, 0, 0 };
        l->n++;
    } else {
        dom_str_release(&l->a[i].value);
    }
    l->a[i].value = dom_str_dup(value, n);
    static CssAtom id_name;
    if (!id_name) { css_atom_reserve(1); id_name = css_atom_intern("id", 2); }
    static CssAtom class_name;
    if (!class_name) { css_atom_reserve(1); class_name = css_atom_intern("class", 5); }
    if (name == id_name) {
        if (n) { css_atom_reserve(1); g_dom.id_atom[s] = css_atom_intern(value, n); dom_id_index_note(s); }
        else g_dom.id_atom[s] = CSS_ATOM_NONE;
    }
    if (name == id_name || name == class_name) g_dom.class_version++;
    dom_note_connected_change(s);
    g_dom.attr_version++;
}

void dom_set_attr_str(NodeId id, const char *name, const char *value) {
    css_atom_reserve(1);
    dom_set_attr(id, css_atom_intern(name, strlen(name)), value ? value : "", value ? strlen(value) : 0);
}

bool dom_remove_attr(NodeId id, CssAtom name) {
    if (!dom_valid(id) || !name) return false;
    uint32_t s = dom_slot(id);
    DomAttrList *l = &g_dom.attrs[s];
    for (int i = 0; i < l->n; i++) {
        if (l->a[i].name != name) continue;
        attrs_make_owned(l, l->n);
        dom_str_release(&l->a[i].value);
        memmove(&l->a[i], &l->a[i + 1], (size_t)(l->n - i - 1) * sizeof(DomAttr));
        l->n--;
        const char *nm = css_atom_str(name);
        if (nm && strcmp(nm, "id") == 0) g_dom.id_atom[s] = CSS_ATOM_NONE;
        if (nm && (strcmp(nm, "id") == 0 || strcmp(nm, "class") == 0)) g_dom.class_version++;
        dom_note_connected_change(s);
        g_dom.attr_version++;
        return true;
    }
    return false;
}

bool dom_has_class(NodeId id, const char *cls, size_t n) {
    const char *v = dom_get_attr_str(id, "class");
    if (!v || !n) return false;
    const char *p = v;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f') p++;
        const char *b = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '\f') p++;
        if ((size_t)(p - b) == n && memcmp(b, cls, n) == 0) return true;
    }
    return false;
}

void dom_set_text(NodeId id, const char *s, size_t n) {
    if (!dom_valid(id)) return;
    uint32_t slot = dom_slot(id);
    dom_str_release(&g_dom.text[slot]);
    g_dom.text[slot] = dom_str_dup(s, n);
    dom_note_connected_change(slot);
    g_dom.text_version++;
}

/* ============================================================================
 * Structure
 * ============================================================================ */

void dom_link_before(NodeId parent, NodeId child, NodeId ref) {
    uint32_t p = dom_slot(parent), c = dom_slot(child);
    uint32_t r = dom_valid(ref) ? dom_slot(ref) : 0;
    if (r && g_dom.parent[r] != p) r = 0;
    /* A root that is linked under a parent stops being a tree root. */
    g_dom.parent[c] = p;
    if (!r) {
        g_dom.prev[c] = g_dom.last[p];
        g_dom.next[c] = 0;
        if (g_dom.last[p]) g_dom.next[g_dom.last[p]] = c;
        else g_dom.first[p] = c;
        g_dom.last[p] = c;
    } else {
        g_dom.next[c] = r;
        g_dom.prev[c] = g_dom.prev[r];
        if (g_dom.prev[r]) g_dom.next[g_dom.prev[r]] = c;
        else g_dom.first[p] = c;
        g_dom.prev[r] = c;
    }
    g_dom.structure_version++;
    dom_note_connected_change(p);
}

void dom_link_append(NodeId parent, NodeId child) {
    dom_link_before(parent, child, DOM_NULL);
}

void dom_unlink(NodeId child) {
    if (!dom_valid(child)) return;
    uint32_t c = dom_slot(child), p = g_dom.parent[c];
    if (!p) return;
    dom_note_connected_change(p);
    if (g_dom.prev[c]) g_dom.next[g_dom.prev[c]] = g_dom.next[c];
    else g_dom.first[p] = g_dom.next[c];
    if (g_dom.next[c]) g_dom.prev[g_dom.next[c]] = g_dom.prev[c];
    else g_dom.last[p] = g_dom.prev[c];
    g_dom.parent[c] = g_dom.next[c] = g_dom.prev[c] = 0;
    g_dom.structure_version++;
}

static void free_subtree_slot(uint32_t root) {
    /* Iterative preorder; collect first, then free (freeing reuses next[]). */
    uint32_t cap = 64, n = 0;
    uint32_t *list = malloc(cap * sizeof(uint32_t));
    uint32_t stack_cap = 16, sp = 0;
    uint32_t *stack = malloc(stack_cap * sizeof(uint32_t));
    stack[sp++] = root;
    while (sp) {
        uint32_t r = stack[--sp];
        for (uint32_t s = r; s; s = dom_next_in_subtree(s, r)) {
            if (n == cap) { cap *= 2; list = realloc(list, cap * sizeof(uint32_t)); }
            list[n++] = s;
            uint32_t extra[2] = { g_dom.shadow[s], g_dom.content[s] };
            for (int k = 0; k < 2; k++) {
                if (!extra[k]) continue;
                if (sp == stack_cap) { stack_cap *= 2; stack = realloc(stack, stack_cap * sizeof(uint32_t)); }
                stack[sp++] = extra[k];
            }
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = list[i];
        g_dom.parent[s] = g_dom.first[s] = g_dom.last[s] = g_dom.prev[s] = 0;
        g_dom.host[s] = g_dom.shadow[s] = g_dom.content[s] = 0;
        dom_free_slot(s);
    }
    free(list);
    free(stack);
    g_dom.structure_version++;
}

void dom_free_subtree(NodeId root) {
    if (!dom_valid(root)) return;
    dom_unlink(root);
    free_subtree_slot(dom_slot(root));
}

uint32_t dom_subtree_size(NodeId root) {
    if (!dom_valid(root)) return 0;
    uint32_t r = dom_slot(root), n = 0;
    for (uint32_t s = r; s; s = dom_next_in_subtree(s, r)) n++;
    return n;
}

/* ============================================================================
 * Trees
 * ============================================================================ */

uint32_t dom_tree_new(NodeId root) {
    uint32_t t = g_dom.tree_free;
    if (t) {
        g_dom.tree_free = g_dom.trees[t].obj;
    } else {
        if (g_dom.ntrees == g_dom.trees_cap) {
            g_dom.trees_cap *= 2;
            g_dom.trees = realloc(g_dom.trees, g_dom.trees_cap * sizeof(DomTreeRec));
        }
        t = g_dom.ntrees++;
    }
    g_dom.trees[t] = (DomTreeRec){ root, 0, 0 };
    if (dom_valid(root)) g_dom.tree[dom_slot(root)] = t;
    return t;
}

void dom_tree_free(uint32_t t) {
    if (!t) return;
    DomTreeRec *r = &g_dom.trees[t];
    if (dom_valid(r->root) && g_dom.tree[dom_slot(r->root)] == t) g_dom.tree[dom_slot(r->root)] = 0;
    r->root = DOM_NULL;
    r->wrappers = 0;
    r->obj = g_dom.tree_free;      /* free list through obj */
    g_dom.tree_free = t;
}

uint32_t dom_tree_of(NodeId id) {
    if (!dom_valid(id)) return 0;
    uint32_t r = dom_tree_root_slot(dom_slot(id));
    if (!g_dom.tree[r]) dom_tree_new(dom_id_of(r));
    return g_dom.tree[r];
}

/* ============================================================================
 * Dirty bits
 * ============================================================================ */

void dom_mark_dirty(NodeId id, uint16_t bits) {
    if (!dom_valid(id)) return;
    uint32_t s = dom_slot(id);
    g_dom.flags[s] |= bits;
    g_dom.dirty_version++;
    for (uint32_t p = g_dom.parent[s] ? g_dom.parent[s] : g_dom.host[s]; p;
         p = g_dom.parent[p] ? g_dom.parent[p] : g_dom.host[p]) {
        if (g_dom.flags[p] & DOMF_DIRTY_DESCENDANT) break;
        g_dom.flags[p] |= DOMF_DIRTY_DESCENDANT;
    }
}

void dom_clear_dirty(NodeId root) {
    if (!dom_valid(root)) return;
    uint32_t r = dom_slot(root);
    for (uint32_t s = r; s; ) {
        /* Also below a node with bits of its own: children linked to it may
         * carry bits set while they were detached (unpropagated), and a
         * stale DESCENDANT bit would stop later marks from reaching here. */
        bool down = (g_dom.flags[s] & DOMF_DIRTY_MASK) != 0;
        g_dom.flags[s] &= (uint16_t)~DOMF_DIRTY_MASK;
        if (g_dom.shadow[s]) dom_clear_dirty(dom_id_of(g_dom.shadow[s]));
        if (down && g_dom.first[s]) { s = g_dom.first[s]; continue; }
        while (s && s != r && !g_dom.next[s]) s = g_dom.parent[s];
        if (!s || s == r) break;
        s = g_dom.next[s];
    }
}

/* ============================================================================
 * Id index: atom -> slots, append-only per atom, validated by readers
 * ============================================================================ */

typedef struct IdBucket { CssAtom atom; uint32_t n, cap; uint32_t *slots; } IdBucket;
static IdBucket *g_idx;
static uint32_t g_idx_cap, g_idx_used;

static IdBucket *id_bucket(CssAtom a, bool create) {
    if (!g_idx_cap) {
        if (!create) return NULL;
        g_idx_cap = 1024;
        g_idx = calloc(g_idx_cap, sizeof(IdBucket));
    }
    if (create && (g_idx_used + 1) * 2 > g_idx_cap) {
        uint32_t cap = g_idx_cap * 2;
        IdBucket *nb = calloc(cap, sizeof(IdBucket));
        for (uint32_t i = 0; i < g_idx_cap; i++) {
            if (!g_idx[i].atom) continue;
            uint32_t j = (g_idx[i].atom * 2654435761u) & (cap - 1);
            while (nb[j].atom) j = (j + 1) & (cap - 1);
            nb[j] = g_idx[i];
        }
        free(g_idx);
        g_idx = nb;
        g_idx_cap = cap;
    }
    uint32_t j = (a * 2654435761u) & (g_idx_cap - 1);
    while (g_idx[j].atom) {
        if (g_idx[j].atom == a) return &g_idx[j];
        j = (j + 1) & (g_idx_cap - 1);
    }
    if (!create) return NULL;
    g_idx[j].atom = a;
    g_idx_used++;
    return &g_idx[j];
}

void dom_id_index_note(uint32_t s) {
    CssAtom a = g_dom.id_atom[s];
    if (!a) return;
    IdBucket *b = id_bucket(a, true);
    for (uint32_t i = 0; i < b->n; i++) if (b->slots[i] == s) return;
    /* Drop entries that no longer carry this id before growing. */
    if (b->n == b->cap) {
        uint32_t w = 0;
        for (uint32_t i = 0; i < b->n; i++) {
            uint32_t x = b->slots[i];
            if (x < g_dom.count && g_dom.kind[x] != DOM_K_FREE && g_dom.id_atom[x] == a) b->slots[w++] = x;
        }
        b->n = w;
        if (b->n == b->cap) {
            b->cap = b->cap ? b->cap * 2 : 2;
            b->slots = realloc(b->slots, b->cap * sizeof(uint32_t));
        }
    }
    b->slots[b->n++] = s;
}

const uint32_t *dom_id_index_lookup(CssAtom a, uint32_t *n) {
    IdBucket *b = a ? id_bucket(a, false) : NULL;
    *n = b ? b->n : 0;
    return b ? b->slots : NULL;
}

extern NodeId g_dom_document;

void dom_note_connected_change(uint32_t s) {
    /* Every mutation passes here with the node whose content changed (the
     * parent for (un)linking): mark it dirty, so snapshots that keep
     * clean subtrees (html_dom.c) see the change. */
    if (s && g_dom.kind[s] != DOM_K_FREE) dom_mark_dirty(dom_id_of(s), DOMF_DIRTY_CHILDREN);
    if (!s || !g_dom_document || !dom_valid(g_dom_document)) return;
    if (dom_tree_root_slot(s) == dom_slot(g_dom_document)) g_dom.connected_version++;
}

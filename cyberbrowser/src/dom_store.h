/*
 * Node store (PARALLEL_DOM.md §4).
 *
 * The document lives in flat arrays in plain memory, one entry per node slot,
 * owned by the engine thread.  JS objects are lazy wrappers created when a
 * script first touches a node (dom_wrap.h); layout reads a snapshot built
 * from the store by a C walk (dom_snapshot.c).
 *
 *   NodeId      slot | generation << 24.  A freed slot bumps its generation,
 *               so a stale id never resolves to the slot's next occupant.
 *   names       CssAtoms (lower-cased, shared with the style system).
 *   strings     text, comment data and attribute values are DomStr: either
 *               a span in a reference-counted arena block (what the parser
 *               produces, no malloc per string) or a malloc'd copy (what
 *               mutations produce).  A block is freed when its last string
 *               is released.
 *   trees       every node belongs to the tree of its root.  A root records
 *               its tree id; shadow roots and template contents continue to
 *               their host, so a host and its shadow tree share one tree.
 *               Trees are the unit of wrapper lifetime (dom_wrap.h).
 *
 * Only the engine thread mutates the store.  Parse and post-processing jobs
 * write slots that were reserved for them, before the store is published.
 */
#ifndef DOM_STORE_H
#define DOM_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "css_style.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t NodeId;
#define DOM_NULL 0u
#define DOM_SLOT_BITS 24u
#define DOM_SLOT_MASK ((1u << DOM_SLOT_BITS) - 1u)

enum {
    DOM_K_FREE      = 0,
    DOM_K_ELEMENT   = 1,
    DOM_K_TEXT      = 3,
    DOM_K_COMMENT   = 8,
    DOM_K_DOCUMENT  = 9,
    DOM_K_DOCTYPE   = 10,
    DOM_K_FRAGMENT  = 11,
};

enum {
    DOMF_SHADOW_ROOT      = 1u << 0,   /* fragment attached as a shadow root (host = element) */
    DOMF_TEMPLATE_CONTENT = 1u << 1,   /* fragment that is a template's content (host = template) */
    DOMF_CE_CANDIDATE     = 1u << 2,   /* element whose local name contains '-' */
    DOMF_PARSER_SCRIPT    = 1u << 3,   /* script inserted by the parser */
    DOMF_RAW_TEXT         = 1u << 4,   /* script/style/textarea/title element */
    DOMF_VOID             = 1u << 5,   /* void element (br, img, ...) */
    DOMF_SELF_CLOSED      = 1u << 6,   /* parsed as <x/> (a raw-text element then has no content) */
    DOMF_CE_DESC          = 1u << 7,   /* the subtree contains a CE candidate (conservative) */
    /* Dirty bits (§7): set by mutations, read and cleared by rendering. */
    DOMF_DIRTY_CHILDREN   = 1u << 8,
    DOMF_DIRTY_ATTR       = 1u << 9,
    DOMF_DIRTY_TEXT       = 1u << 10,
    DOMF_DIRTY_DESCENDANT = 1u << 11,  /* some descendant has a dirty bit */
};
#define DOMF_DIRTY_MASK (DOMF_DIRTY_CHILDREN | DOMF_DIRTY_ATTR | DOMF_DIRTY_TEXT | DOMF_DIRTY_DESCENDANT)

/* A stored string: NUL-terminated, `len` bytes.  blk != 0: a span of arena
 * block `blk`; blk == 0: malloc'd (or NULL for the empty string). */
typedef struct DomStr {
    char *s;
    uint32_t len;
    uint32_t blk;
} DomStr;

typedef struct DomAttr {
    CssAtom name;
    DomStr value;
} DomAttr;

/* A node's attribute list.  `blk` != 0: the array lives in an arena block
 * (as parsed); a mutation copies it to malloc'd memory first (blk = 0). */
typedef struct DomAttrList {
    DomAttr *a;
    uint16_t n, cap;
    uint32_t blk;
} DomAttrList;

typedef struct DomTreeRec {
    NodeId root;            /* DOM_NULL: free record */
    uint32_t obj;           /* GC handle of the tree object (dom_wrap.c), 0 = none */
    uint32_t wrappers;      /* nodes of this tree that have a wrapper */
} DomTreeRec;

typedef struct DomStore {
    uint32_t cap;           /* slots allocated (slot 0 is never used) */
    uint32_t count;         /* high-water mark: slots [1, count) were used */
    uint32_t free_head;     /* free slot list through next[] */
    uint32_t live;          /* live nodes */

    uint8_t  *kind;
    uint8_t  *gen;
    uint16_t *flags;
    CssAtom  *tag;          /* element local name (lower case) */
    uint32_t *parent, *first, *last, *next, *prev;   /* slots, 0 = none */
    uint32_t *host;         /* shadow root / template content -> host slot */
    uint32_t *shadow;       /* element -> its shadow root slot */
    uint32_t *content;      /* template element -> content fragment slot */
    uint32_t *owner;        /* owner document slot */
    uint32_t *tree;         /* roots only: tree record index (0 = none yet) */
    uint32_t *wrapper;      /* GC handle of the node's wrapper, 0 = none */
    uint32_t *wrap_idx;     /* index of the wrapper in its tree object */
    DomStr   *text;         /* text / comment data */
    DomAttrList *attrs;
    CssAtom  *id_atom;      /* atom of the id attribute (lower-cased), 0 = none */
    uint32_t *order;        /* creation sequence, for document-order ties */

    DomTreeRec *trees;
    uint32_t ntrees, trees_cap, tree_free;

    uint64_t structure_version;   /* bumped by every tree mutation */
    uint64_t attr_version;        /* bumped by every attribute mutation */
    uint64_t text_version;        /* bumped by every text mutation */
    uint64_t class_version;       /* bumped when an id or class attribute changes */
    uint64_t connected_version;   /* bumped by tree, attribute and text mutations
                                     in the document's tree (not detached ones) */
    uint32_t seq;
} DomStore;

extern DomStore g_dom;

/* ---- ids ---- */
static inline uint32_t dom_slot(NodeId id) { return id & DOM_SLOT_MASK; }
static inline NodeId dom_id_of(uint32_t slot) {
    return slot ? (slot | ((uint32_t)g_dom.gen[slot] << DOM_SLOT_BITS)) : DOM_NULL;
}
static inline bool dom_valid(NodeId id) {
    uint32_t s = id & DOM_SLOT_MASK;
    return s && s < g_dom.count && g_dom.kind[s] != DOM_K_FREE &&
           g_dom.gen[s] == (uint8_t)(id >> DOM_SLOT_BITS);
}

/* ---- life cycle ---- */
void dom_store_init(void);
void dom_store_reset(void);              /* free every node (tests, shutdown) */
/* Make room for `extra` more slots (so jobs can fill reserved slots). */
void dom_store_reserve(uint32_t extra);
/* Allocate a slot (engine thread).  The node is detached, has no children. */
uint32_t dom_alloc_slot(int kind, NodeId owner);
/* Free one slot's own data (strings, attributes) and its id.  The slot must be
 * detached from any parent; children are not touched. */
void dom_free_slot(uint32_t slot);

/* ---- strings and arena ---- */
/* Copy `n` bytes into a new malloc'd DomStr. */
DomStr dom_str_dup(const char *s, size_t n);
void dom_str_release(DomStr *str);
/* Arena used by parsers: allocations are counted in their block. */
typedef struct DomArena {
    uint32_t blk;           /* current block (0 = none) */
    char *base;
    size_t used, cap;
    size_t next_size;       /* size of the next block (0 = default); grows to 64 KB */
} DomArena;
void *dom_arena_alloc(DomArena *ar, size_t n, uint32_t *blk_out);
/* Copy `n` bytes into the arena as a NUL-terminated string. */
DomStr dom_arena_str(DomArena *ar, const char *s, size_t n);
/* Drop the arena's hold on its current block (the strings keep it alive). */
void dom_arena_finish(DomArena *ar);
void dom_block_retain(uint32_t blk);
void dom_block_release(uint32_t blk);
size_t dom_blocks_live(void);           /* statistics / tests */

/* ---- node creation ---- */
NodeId dom_create_document(void);
NodeId dom_create_element(CssAtom tag, NodeId owner);
NodeId dom_create_element_str(const char *tag, size_t n, NodeId owner);
NodeId dom_create_chardata(int kind, const char *s, size_t n, NodeId owner);
NodeId dom_create_fragment(NodeId owner);
/* Flags derived from the tag name (CE candidate, raw text, void). */
uint16_t dom_tag_flags(CssAtom tag);

/* ---- reads ---- */
static inline int dom_kind(NodeId id) { return dom_valid(id) ? g_dom.kind[dom_slot(id)] : DOM_K_FREE; }
static inline CssAtom dom_tag(NodeId id) { return dom_valid(id) ? g_dom.tag[dom_slot(id)] : CSS_ATOM_NONE; }
static inline uint16_t dom_flags(NodeId id) { return dom_valid(id) ? g_dom.flags[dom_slot(id)] : 0; }
static inline NodeId dom_parent(NodeId id) { return dom_valid(id) ? dom_id_of(g_dom.parent[dom_slot(id)]) : DOM_NULL; }
static inline NodeId dom_first_child(NodeId id) { return dom_valid(id) ? dom_id_of(g_dom.first[dom_slot(id)]) : DOM_NULL; }
static inline NodeId dom_last_child(NodeId id) { return dom_valid(id) ? dom_id_of(g_dom.last[dom_slot(id)]) : DOM_NULL; }
static inline NodeId dom_next_sibling(NodeId id) { return dom_valid(id) ? dom_id_of(g_dom.next[dom_slot(id)]) : DOM_NULL; }
static inline NodeId dom_prev_sibling(NodeId id) { return dom_valid(id) ? dom_id_of(g_dom.prev[dom_slot(id)]) : DOM_NULL; }
static inline NodeId dom_host(NodeId id) { return dom_valid(id) ? dom_id_of(g_dom.host[dom_slot(id)]) : DOM_NULL; }
static inline NodeId dom_shadow_root(NodeId id) { return dom_valid(id) ? dom_id_of(g_dom.shadow[dom_slot(id)]) : DOM_NULL; }
static inline NodeId dom_template_content(NodeId id) { return dom_valid(id) ? dom_id_of(g_dom.content[dom_slot(id)]) : DOM_NULL; }
static inline NodeId dom_owner(NodeId id) { return dom_valid(id) ? dom_id_of(g_dom.owner[dom_slot(id)]) : DOM_NULL; }
/* Tag name as a string ("" for non-elements). */
const char *dom_tag_name(NodeId id);
/* Text / comment data ("" when none). */
const char *dom_text(NodeId id, uint32_t *len);

/* Root of the node's tree, following shadow-root and template-content hosts. */
uint32_t dom_tree_root_slot(uint32_t slot);
/* Root of the node's own tree, not following hosts (getRootNode()). */
NodeId dom_root(NodeId id);
/* Is the node in a document (following shadow hosts)? */
bool dom_is_connected(NodeId id);
/* Is `a` an inclusive ancestor of `b` (not following hosts)? */
bool dom_contains(NodeId a, NodeId b);
/* Preorder successor of `n` within the subtree of `root` (not entering shadow
 * roots or template contents). */
static inline uint32_t dom_next_in_subtree(uint32_t n, uint32_t root) {
    if (g_dom.first[n]) return g_dom.first[n];
    while (n && n != root) {
        if (g_dom.next[n]) return g_dom.next[n];
        n = g_dom.parent[n];
    }
    return 0;
}

/* ---- attributes ---- */
int dom_attr_count(NodeId id);
const DomAttr *dom_attr_at(NodeId id, int i);
/* Value of attribute `name` (atom), or NULL. */
const char *dom_get_attr(NodeId id, CssAtom name, uint32_t *len);
const char *dom_get_attr_str(NodeId id, const char *name);
/* Set (or add, at the end) an attribute.  Updates the id atom. */
void dom_set_attr(NodeId id, CssAtom name, const char *value, size_t n);
void dom_set_attr_str(NodeId id, const char *name, const char *value);
bool dom_remove_attr(NodeId id, CssAtom name);
/* Does the class attribute contain `cls` (case-sensitive token match)? */
bool dom_has_class(NodeId id, const char *cls, size_t n);

/* ---- text ---- */
void dom_set_text(NodeId id, const char *s, size_t n);

/* ---- structure (no JS, no wrappers: dom_wrap.c layers those on top) ---- */
/* Link a detached `child` as the last child of `parent` / before `ref`
 * (ref == DOM_NULL appends).  Both must be valid; `child` must be detached. */
void dom_link_append(NodeId parent, NodeId child);
void dom_link_before(NodeId parent, NodeId child, NodeId ref);
/* Unlink `child` from its parent (no-op when detached). */
void dom_unlink(NodeId child);
/* Free a detached subtree (and the shadow roots and template contents that
 * hang off it).  The caller guarantees no wrapper refers to any of it. */
void dom_free_subtree(NodeId root);
/* Number of nodes in a subtree (debug / tests). */
uint32_t dom_subtree_size(NodeId root);

/* ---- trees ---- */
/* Tree record of the node's tree (creating one for its root if needed). */
uint32_t dom_tree_of(NodeId id);
static inline DomTreeRec *dom_tree_rec(uint32_t t) { return &g_dom.trees[t]; }
uint32_t dom_tree_new(NodeId root);
void dom_tree_free(uint32_t t);

/* ---- id index (maintained incrementally; entries validated on lookup) ---- */
/* Record that slot `s` now has id atom g_dom.id_atom[s]. */
void dom_id_index_note(uint32_t s);
/* Slots that had id atom `a` at some point (callers re-check each). */
const uint32_t *dom_id_index_lookup(CssAtom a, uint32_t *n);

/* ---- dirty bits (§7) ---- */
/* A mutation at slot s: count it in connected_version when s is in the
 * document's tree. */
void dom_note_connected_change(uint32_t s);
void dom_mark_dirty(NodeId id, uint16_t bits);
void dom_clear_dirty(NodeId root);

#ifdef __cplusplus
}
#endif

#endif /* DOM_STORE_H */

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
    DOM_K_CDATA     = 4,    /* CDATASection (a Text) */
    DOM_K_PI        = 7,    /* ProcessingInstruction: target as lname, data as text */
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
    CssAtom name;       /* qualified name, lower-cased (matching, CSS) */
    DomStr value;
    uint32_t ns;        /* namespace: its URI's dom_name id; 0 = none */
    uint32_t qname;     /* qualified name as written (dom_name id) if it is not name's; 0 = name's */
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

/* An element's first class atoms, inline in its slot. */
typedef struct DomCls4 { CssAtom a[4]; } DomCls4;

/* An element's states, as selectors see them (dom_element_state).  Below 1 << 16:
 * what the browser sets (DomStore.state); from there: read off its attributes. */
enum {
    DOM_ST_HOVER = 1 << 0, DOM_ST_ACTIVE = 1 << 1, DOM_ST_FOCUS = 1 << 2, DOM_ST_FOCUS_VISIBLE = 1 << 3,
    DOM_ST_FOCUS_WITHIN = 1 << 4, DOM_ST_TARGET = 1 << 5, DOM_ST_CHECKED = 1 << 6,
    DOM_ST_CHECK_SET = 1 << 7,      /* its checkedness is its own, not the attribute's */
    DOM_ST_INDETERMINATE = 1 << 8, DOM_ST_PLAYING = 1 << 9, DOM_ST_MUTED = 1 << 10, DOM_ST_SEEKING = 1 << 11,
    DOM_ST_BUFFERING = 1 << 12, DOM_ST_STALLED = 1 << 13,
    DOM_ST_MUTE_SET = 1 << 14,      /* its mutedness is its own, not the attribute's */
    DOM_ST_TARGET_CURRENT = 1 << 15, /* its ::scroll-marker is its group's active one */
    DOM_ST_PAUSED = 1 << 16, DOM_ST_ENABLED = 1 << 17, DOM_ST_DISABLED = 1 << 18, DOM_ST_READ_WRITE = 1 << 19,
    DOM_ST_READ_ONLY = 1 << 20, DOM_ST_REQUIRED = 1 << 21, DOM_ST_OPTIONAL = 1 << 22,
    DOM_ST_VALID = 1 << 23, DOM_ST_INVALID = 1 << 24,   /* (as dom_validity_refresh last found them) */
    DOM_ST_PLACEHOLDER_SHOWN = 1 << 25, DOM_ST_DEFAULT = 1 << 26, DOM_ST_IN_RANGE = 1 << 27, DOM_ST_OUT_OF_RANGE = 1 << 28,
    DOM_ST_MODAL = 1 << 29, DOM_ST_POPOVER_OPEN = 1 << 30,   /* (g_dom_top: what the page's script showed) */
};

/* Per slot: a dialog shown modally (1), a popover showing (2).  Cleared when a
 * slot is given out again. */
extern uint8_t *g_dom_top;
extern uint32_t g_dom_top_cap;
void dom_top_state_set(uint32_t slot, uint8_t bits, bool on);

typedef struct DomStore {
    uint32_t cap;           /* slots allocated (slot 0 is never used) */
    uint32_t count;         /* high-water mark: slots [1, count) were used */
    uint32_t free_head;     /* free slot list through next[] */
    uint32_t live;          /* live nodes */

    uint8_t  *kind;
    uint8_t  *gen;
    uint16_t *flags;
    uint16_t *state;        /* DOM_ST_* the browser set */
    CssAtom  *tag;          /* element local name (lower case) */
    uint32_t *parent, *first, *last, *next, *prev;   /* slots, 0 = none */
    uint32_t *host;         /* shadow root / template content -> host slot */
    uint32_t *shadow;       /* element -> its shadow root slot */
    uint32_t *content;      /* template element -> content fragment slot */
    uint32_t *owner;        /* owner document slot */
    uint32_t *tree;         /* roots only: tree record index (0 = none yet) */
    uint32_t *wrapper;      /* GC handle of the node's wrapper, 0 = none */
    uint32_t *wrap_idx;     /* index of the wrapper in its tree object */
    uint16_t *realm;        /* the realm it was created in (dom_wrap.c's table), its document's; 0 = unknown */
    DomStr   *text;         /* text / comment data */
    DomAttrList *attrs;
    CssAtom  *id_atom;      /* atom of the id attribute (lower-cased), 0 = none */
    /* The class attribute's tokens as atoms (lower-cased), made when it is
     * set: ncls low 7 bits their count (at most 127), bit 7 set when a token
     * has an upper-case letter (an exact match then checks the string);
     * cls the first 4, cls_more all of them when there are more (malloc'd);
     * cls_bloom one bit per token (DOM_CLS_BIT). */
    uint8_t  *ncls;
    DomCls4  *cls;
    CssAtom **cls_more;
    uint64_t *cls_bloom;
    uint32_t *ns;           /* element: namespace (DOM_NS_*, else a dom_name id); 0 = HTML's */
    uint32_t *prefix;       /* element: namespace prefix (dom_name id), 0 = none */
    uint32_t *lname;        /* element: local name as written (dom_name id) when it
                               is not tag's (upper case in it), 0 = tag's */
    uint32_t *order;        /* creation sequence, for document-order ties */

    DomTreeRec *trees;
    uint32_t ntrees, trees_cap, tree_free;

    uint64_t structure_version;   /* bumped by every tree mutation */
    uint64_t dirty_version;       /* bumped by every dom_mark_dirty */
    uint64_t attr_version;        /* bumped by every attribute mutation */
    uint64_t text_version;        /* bumped by every text mutation */
    uint64_t class_version;       /* bumped when an id or class attribute changes */
    uint64_t connected_version;   /* bumped by tree, attribute and text mutations
                                     in the document's tree (not detached ones) */
    uint32_t seq;
} DomStore;

extern DomStore g_dom;

/* ---- ids ---- */
/* Scroll offsets (CSSOM View), flat: the rows of an element's scrolling box, or
 * a document's for its viewport.  Few, searched in order; no row means 0, 0.
 * paint: the display list is out of date; fixed: the last one had a fixed box
 * (it moves against the viewport's scroll). */
typedef struct DomScrolls {
    NodeId *id;
    float *x, *y;
    uint32_t n, cap;
    bool paint, fixed;
} DomScrolls;
extern DomScrolls g_dom_scrolls;
bool dom_scroll_get(NodeId id, float *x, float *y);
void dom_scroll_set(NodeId id, float x, float y);

static inline uint32_t dom_slot(NodeId id) { return id & DOM_SLOT_MASK; }
static inline NodeId dom_id_of(uint32_t slot) {
    return slot ? (slot | ((uint32_t)g_dom.gen[slot] << DOM_SLOT_BITS)) : DOM_NULL;
}
static inline bool dom_valid(NodeId id) {
    uint32_t s = id & DOM_SLOT_MASK;
    return s && s < g_dom.count && g_dom.kind[s] != DOM_K_FREE &&
           g_dom.gen[s] == (uint8_t)(id >> DOM_SLOT_BITS);
}

/* ---- class tokens (atoms) ---- */
#define DOM_CLS_BIT(a) (1ull << (((uint32_t)(a) * 0x9E3779B1u) >> 26))
static inline uint32_t dom_cls_count(uint32_t s) { return g_dom.ncls[s] & 127u; }
static inline const CssAtom *dom_cls_atoms(uint32_t s) {
    return (g_dom.ncls[s] & 127u) > 4 ? g_dom.cls_more[s] : g_dom.cls[s].a;
}
static inline bool dom_cls_upper(uint32_t s) { return (g_dom.ncls[s] & 128u) != 0; }
/* Set slot s's class tokens from the class attribute's value (NULL: none). */
void dom_cls_note(uint32_t s, const char *v, size_t n);

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
const char *dom_name_str(uint32_t id);
/* A DocumentType (its name, publicId and systemId: lname, prefix and ns as
 * dom_name ids) and a ProcessingInstruction (its target: lname). */
NodeId dom_create_doctype(const char *name, size_t nn, const char *pub, size_t pn, const char *sys, size_t sn,
                          NodeId owner);
static inline const char *dom_doctype_public_id(NodeId id) { return dom_name_str(dom_valid(id) ? g_dom.prefix[dom_slot(id)] : 0); }
static inline const char *dom_doctype_system_id(NodeId id) { return dom_name_str(dom_valid(id) ? g_dom.ns[dom_slot(id)] : 0); }
NodeId dom_create_pi(const char *target, size_t tn, const char *data, size_t dn, NodeId owner);
/* Flags derived from the tag name (CE candidate, raw text, void). */
uint16_t dom_tag_flags(CssAtom tag);
/* The media elements created so far (a parsed one has no script to start its
 * load: browser/media_source_api.c looks them over), and forgetting them. */
const NodeId *dom_media_elements(uint32_t *n);
void dom_media_note(uint32_t slot);
void dom_media_forget(void);

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
/* Is the kind a CharacterData's (Text, CDATASection, ProcessingInstruction, Comment)? */
static inline bool dom_kind_is_chardata(int k) {
    return k == DOM_K_TEXT || k == DOM_K_CDATA || k == DOM_K_PI || k == DOM_K_COMMENT;
}
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

/* ---- names and namespaces ---- */
/* Case-sensitive interned strings (namespace URIs, prefixes, local names
 * with upper case), by id; ids are never 0.  Engine thread only. */
uint32_t dom_name_intern(const char *s, size_t n);
const char *dom_name_str(uint32_t id);
/* Element namespaces (g_dom.ns): HTML's is 0, so elements are HTML elements
 * unless set; the others are the dom_name ids of their URIs. */
enum { DOM_NS_HTML = 0, DOM_NS_NULL = 1, DOM_NS_SVG = 2, DOM_NS_MATHML = 3, DOM_NS_XML = 4,
       DOM_NS_XMLNS = 5, DOM_NS_XLINK = 6 };
/* The namespace for a URI (NULL or "": DOM_NS_NULL); its URI (NULL for null). */
uint32_t dom_ns_of(const char *uri, size_t n);
const char *dom_ns_uri(uint32_t ns);
static inline uint32_t dom_ns(NodeId id) { return dom_valid(id) ? g_dom.ns[dom_slot(id)] : DOM_NS_HTML; }
/* An element's local name as written ("" for non-elements) and prefix (NULL: none). */
const char *dom_local_name(NodeId id);
const char *dom_prefix(NodeId id);
/* Give an element a namespace, prefix (NULL: none) and local name (as written). */
void dom_set_element_name(NodeId id, uint32_t ns, const char *prefix, size_t pn, const char *local, size_t ln);
/* Is the document an XML document (not an HTML document)? */
#define DOMF_XML_DOCUMENT (1u << 12)   /* (on document nodes) */
#define DOMF_MEDIA (1u << 12)          /* (on elements) video, audio */
#define DOMF_SHADOW_CLOSED (1u << 13)  /* (on shadow roots) mode "closed" */
#define DOMF_INERT_DOC (1u << 13)      /* (on document nodes) a template contents owner */
#define DOMF_DOC_CHANGED (1u << 15)    /* (on document nodes) something in it marked dirty since the frame tick looked */
#define DOMF_META_PENDING (1u << 14)   /* (on metas) inserted, its post-insertion steps to come */
static inline bool dom_is_html_document(NodeId doc) { return !(dom_flags(doc) & DOMF_XML_DOCUMENT); }

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
/* An attribute's qualified name as written, its local name and prefix
 * (NULL: none; only a namespaced attribute has one). */
const char *dom_attr_qualified_name(const DomAttr *a);
const char *dom_attr_local_name(const DomAttr *a);
const char *dom_attr_prefix(const DomAttr *a, size_t *len);
/* The index of the first attribute whose qualified name (as written) is
 * qn, or whose namespace and local name are these; -1 if none. */
int dom_attr_index_qualified(NodeId id, const char *qn, size_t n);
int dom_attr_index_ns(NodeId id, uint32_t ns, const char *local, size_t n);
/* Set the value of attribute i, or (i < 0) add one named qn, in namespace
 * ns (0: none).  Keeps the id atom; returns the attribute's index. */
int dom_set_attr_at(NodeId id, int i, uint32_t ns, const char *qn, size_t qnl, const char *value, size_t n);
void dom_remove_attr_at(NodeId id, int i);
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
/* Called with a node about to be unlinked from its parent (NodeIterator's
 * pre-removing steps: traversal.c sets it while there are iterators). */
extern void (*g_dom_pre_remove)(NodeId child);
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
/* The same, marking s dirty with `bits` (dom_note_connected_change marks
 * DOMF_DIRTY_CHILDREN: a structural change at s). */
void dom_note_mutation(uint32_t s, uint16_t bits);
void dom_mark_dirty(NodeId id, uint16_t bits);
void dom_clear_dirty(NodeId root);

#ifdef __cplusplus
}
#endif

#endif /* DOM_STORE_H */

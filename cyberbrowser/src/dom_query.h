/*
 * Queries over the node store (PARALLEL_DOM.md §4.2, §7 "Queries").
 *
 *   Selectors   querySelector/All, matches and closest compile their
 *               selector once (cached by text) into compounds of atoms and
 *               match right to left on the store.  Supported: type, *, #id,
 *               .class, attribute selectors (= ~= |= ^= $= *=, i flag), the
 *               descendant, child, next-sibling and subsequent-sibling
 *               combinators, selector lists, :not/:is/:where, structural
 *               pseudo-classes (first/last/only-child, nth-child,
 *               nth-last-child, first/last-of-type, empty, root, scope) and
 *               :checked/:disabled/:enabled.  Other pseudo-classes match
 *               (as the legacy matcher did); pseudo-elements never match.
 *               An unparsable selector matches nothing.
 *   Indexes     id -> nodes, class -> nodes and tag -> nodes for the main
 *               document, as sorted lists in document order, built by
 *               counting sort (range jobs on large documents) and rebuilt
 *               lazily when the store's versions moved.  Queries take their
 *               candidates from an index when the rightmost compound has an
 *               id, class or tag, and restrict them to the scope by preorder
 *               range; otherwise they scan the scope's subtree.
 */
#ifndef DOM_QUERY_H
#define DOM_QUERY_H

#include <stdbool.h>
#include <stddef.h>
#include "dom_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DomNodeList {
    NodeId *ids;
    uint32_t n, cap;
} DomNodeList;
void dom_node_list_free(DomNodeList *l);

/* Selector queries.  `include_self` also tests the scope node itself. */
NodeId dom_query_first(NodeId scope, const char *sel, size_t len, bool include_self);
DomNodeList dom_query_all(NodeId scope, const char *sel, size_t len, bool include_self);
bool dom_query_matches(NodeId el, const char *sel, size_t len);
/* Is the selector parsable? */
bool dom_query_valid(const char *sel, size_t len);

/* Index lookups (connected elements of document `doc`, document order). */
NodeId dom_query_by_id(NodeId doc, const char *id, size_t len);
DomNodeList dom_query_by_class(NodeId doc, const char *cls, size_t len);
DomNodeList dom_query_by_tag(NodeId doc, const char *tag, size_t len);
/* The same restricted to the subtree of `root` (excluding root unless
 * include_self); `root` may be any node, in or out of the document. */
DomNodeList dom_query_by_tag_in(NodeId root, const char *tag, size_t len, bool include_self);
/* Class list in the subtree of `root`; `cls` may hold several
 * space-separated names (all must be present). */
DomNodeList dom_query_by_class_in(NodeId root, const char *cls, size_t len);

/* An attribute of `n` changed (keeps index invalidation precise). */
void dom_query_note_attr(NodeId n, const char *name);

/* Statistics: index builds, queries answered from an index. */
typedef struct DomQueryStats {
    uint64_t index_builds, index_hits, scans, cache_hits, compiles;
    double last_build_ms;
} DomQueryStats;
void dom_query_stats(DomQueryStats *out);

#ifdef __cplusplus
}
#endif

#endif /* DOM_QUERY_H */

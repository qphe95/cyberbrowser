/*
 * dom_range.h - the boundary points of every Range and StaticRange (DOM
 * 5.3-5.5), in one store of flat arrays (dom_range.c).
 *
 * A row is a range: its start (node, offset) and end.  A live one (a Range)
 * follows the tree: the store's own insert and remove (dom_store.c), and
 * CharacterData's replace data and split (browser/character_data.c), call
 * the hooks below, which move the boundary points as DOM 5.3 says.  The
 * hooks cost nothing while no live range exists.  Offsets of character data
 * count UTF-16 code units.
 */
#ifndef DOM_RANGE_H
#define DOM_RANGE_H

#include <stdbool.h>
#include <stdint.h>
#include "dom_store.h"

/* A new range collapsed at (n, off); live: one the hooks move.  Returns its
 * row, or -1 when out of memory. */
int dom_range_new(NodeId n, uint32_t off, bool live);
void dom_range_free(int r);
void dom_range_get(int r, NodeId *sn, uint32_t *so, NodeId *en, uint32_t *eo);
void dom_range_set(int r, NodeId sn, uint32_t so, NodeId en, uint32_t eo);
/* Whether row r is a live one (a Range, not a StaticRange). */
bool dom_range_is_live(int r);

/* The length of UTF-8 text s[0..n) in UTF-16 code units. */
uint32_t dom_utf16_len(const char *s, size_t n);

/* ---- The hooks ---- */
extern uint32_t g_dom_ranges_live;      /* live rows: 0 skips every hook */

/* count nodes were inserted into parent at index (they are at index ..). */
void dom_range_inserted(NodeId parent, uint32_t index, uint32_t count);
/* node, which was at index in parent, was removed (already unlinked). */
void dom_range_removed(NodeId parent, uint32_t index, NodeId node);
/* node's data [off, off + count) was replaced by len code units. */
void dom_range_replaced(NodeId node, uint32_t off, uint32_t count, uint32_t len);
/* node was split at off: new_node (inserted after it, at index in parent,
 * when it has one) holds the rest; called after the insertion and before
 * node's data is cut. */
void dom_range_split(NodeId node, uint32_t off, NodeId new_node, NodeId parent, uint32_t index);
/* normalize() merging Text next (at index in parent) into node, whose data
 * was len code units: points in next, or at next in parent, move to node. */
void dom_range_merged(NodeId node, uint32_t len, NodeId next, NodeId parent, uint32_t index);

#endif

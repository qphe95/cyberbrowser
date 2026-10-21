/*
 * PreOrderCompactionArray - A reusable tree stored as a pre-order array.
 *
 * Nodes are laid out contiguously in a single buffer. Each node must begin
 * with a PreOrderCompactionArrayNode header (placed as the first member of the
 * user's node struct). The header stores tree links and a state flag; the bytes
 * that follow are the caller-defined payload.
 *
 * Deleted nodes are marked with a tombstone and removed during compaction,
 * which rewrites active nodes back into pre-order.
 */

#ifndef PREORDER_COMPACTION_ARRAY_H
#define PREORDER_COMPACTION_ARRAY_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PO_ARRAY_NODE_ACTIVE    1
#define PO_ARRAY_NODE_TOMBSTONE 0

/* Array item header. Must be the first member of any node stored in the array. */
typedef struct PreOrderCompactionArrayNode {
    int parent;
    int first_child;
    int last_child;
    int next_sibling;
    int prev_sibling;
    int state; /* PO_ARRAY_NODE_ACTIVE or PO_ARRAY_NODE_TOMBSTONE */
} PreOrderCompactionArrayNode;

typedef struct PreOrderCompactionArray {
    char *buffer;      /* array of full node structs (header + payload) */
    size_t node_size;  /* total bytes per node */
    size_t capacity;
    size_t count;      /* total nodes (active + tombstone) */
    size_t active_count;
    size_t tombstone_count;
    int first_root;
    int last_root;
} PreOrderCompactionArray;

/*
 * Initialize an array for nodes of `node_size` bytes.
 * The node struct must begin with PreOrderCompactionArrayNode.
 */
bool po_array_init(PreOrderCompactionArray *array, size_t node_size, size_t initial_capacity);

/* Free the underlying buffer. The array struct itself is owned by the caller. */
void po_array_free(PreOrderCompactionArray *array);

/*
 * Add a new active node. It is appended at the end of the array and linked as
 * the last child of `parent`. Pass -1 for `parent` to create a root.
 * Returns the node index, or -1 on failure.
 */
int po_array_add(PreOrderCompactionArray *array, int parent);

/*
 * Delete a node and its entire subtree. Nodes are marked tombstone; the array
 * is not rewritten until po_array_compact() is called.
 */
void po_array_delete(PreOrderCompactionArray *array, int index);

/* Parent value of a node taken out of the tree with po_array_detach. */
#define PO_ARRAY_DETACHED (-2)

/*
 * Take a node (and its subtree, which stays active) out of the tree without
 * deleting it, to be linked again with po_array_append.  A detached node is
 * in no sibling list (and not a root).
 */
void po_array_detach(PreOrderCompactionArray *array, int index);

/* Link a detached node as the last child of `parent`.  False when the node
 * is not detached or an index is invalid. */
bool po_array_append(PreOrderCompactionArray *array, int parent, int index);

/*
 * Rewrite the buffer so active nodes are stored in pre-order. All existing
 * indices may change. Returns the number of active nodes.
 */
size_t po_array_compact(PreOrderCompactionArray *array);

/* The same even without tombstones: after detach/append the array order
 * is no longer pre-order. */
size_t po_array_reorder(PreOrderCompactionArray *array);

/* Access the header for a node. Returns NULL for invalid indices. */
PreOrderCompactionArrayNode* po_array_node(PreOrderCompactionArray *array, int index);

/* Access the full node (header + payload). Same address as po_array_node. */
void* po_array_payload(PreOrderCompactionArray *array, int index);

/* Convert a node/payload pointer obtained from this array back into an index. */
int po_array_index_from_payload(const PreOrderCompactionArray *array, const void *payload);

/* Tree navigation (returns -1 if none). */
int po_array_parent(PreOrderCompactionArray *array, int index);
int po_array_first_child(PreOrderCompactionArray *array, int index);
int po_array_last_child(PreOrderCompactionArray *array, int index);
int po_array_next_sibling(PreOrderCompactionArray *array, int index);
int po_array_prev_sibling(PreOrderCompactionArray *array, int index);

/* Iteration over active roots and their subtrees in pre-order. */
typedef void (*PoArrayForeachCallback)(int index, void *payload, void *user_data);
void po_array_preorder_foreach(PreOrderCompactionArray *array, PoArrayForeachCallback callback, void *user_data);

/* Diagnostics. */
size_t po_array_count(const PreOrderCompactionArray *array);
size_t po_array_active_count(const PreOrderCompactionArray *array);
bool po_array_is_active(const PreOrderCompactionArray *array, int index);

#ifdef __cplusplus
}
#endif

#endif /* PREORDER_COMPACTION_ARRAY_H */

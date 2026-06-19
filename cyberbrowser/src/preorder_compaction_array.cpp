/*
 * PreOrderCompactionArray - Implementation
 *
 * Nodes are stored as whole structs in a contiguous buffer. The first bytes of
 * each node are a PreOrderCompactionArrayNode header; the rest is the caller's
 * payload.
 */

#include "preorder_compaction_array.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static inline PreOrderCompactionArrayNode* po_array_node_fast(PreOrderCompactionArray *array, int index) {
    return (PreOrderCompactionArrayNode*)(array->buffer + (size_t)index * array->node_size);
}

bool po_array_init(PreOrderCompactionArray *array, size_t node_size, size_t initial_capacity) {
    if (!array || node_size < sizeof(PreOrderCompactionArrayNode)) return false;

    memset(array, 0, sizeof(*array));
    array->node_size = node_size;
    array->first_root = -1;
    array->last_root = -1;

    if (initial_capacity < 16) initial_capacity = 16;
    array->buffer = (char*)malloc(initial_capacity * array->node_size);
    if (!array->buffer) return false;

    array->capacity = initial_capacity;
    return true;
}

void po_array_free(PreOrderCompactionArray *array) {
    if (!array) return;
    free(array->buffer);
    memset(array, 0, sizeof(*array));
    array->first_root = -1;
    array->last_root = -1;
}

int po_array_add(PreOrderCompactionArray *array, int parent) {
    if (!array) return -1;
    if (parent >= 0 && (size_t)parent >= array->count) return -1;

    if (array->count >= array->capacity) {
        size_t new_capacity = array->capacity * 2;
        char *new_buffer = (char*)realloc(array->buffer, new_capacity * array->node_size);
        if (!new_buffer) return -1;
        array->buffer = new_buffer;
        array->capacity = new_capacity;
    }

    int idx = (int)array->count;
    PreOrderCompactionArrayNode *node = po_array_node_fast(array, idx);
    memset(node, 0, array->node_size);

    node->state = PO_ARRAY_NODE_ACTIVE;
    node->parent = parent;
    node->first_child = -1;
    node->last_child = -1;
    node->next_sibling = -1;
    node->prev_sibling = -1;

    /* Link into the active tree. */
    if (parent < 0) {
        if (array->first_root < 0) {
            array->first_root = idx;
        } else {
            node->prev_sibling = array->last_root;
            po_array_node_fast(array, array->last_root)->next_sibling = idx;
        }
        array->last_root = idx;
    } else {
        PreOrderCompactionArrayNode *parent_node = po_array_node_fast(array, parent);
        if (parent_node->last_child >= 0) {
            PreOrderCompactionArrayNode *last = po_array_node_fast(array, parent_node->last_child);
            node->prev_sibling = parent_node->last_child;
            last->next_sibling = idx;
        } else {
            parent_node->first_child = idx;
        }
        parent_node->last_child = idx;
    }

    array->count++;
    array->active_count++;
    return idx;
}

static void po_array_unlink(PreOrderCompactionArray *array, int index) {
    PreOrderCompactionArrayNode *node = po_array_node_fast(array, index);

    if (node->parent < 0) {
        if (node->prev_sibling >= 0) {
            po_array_node_fast(array, node->prev_sibling)->next_sibling = node->next_sibling;
        } else {
            array->first_root = node->next_sibling;
        }
        if (node->next_sibling >= 0) {
            po_array_node_fast(array, node->next_sibling)->prev_sibling = node->prev_sibling;
        } else {
            array->last_root = node->prev_sibling;
        }
    } else {
        PreOrderCompactionArrayNode *parent_node = po_array_node_fast(array, node->parent);
        if (parent_node->first_child == index) parent_node->first_child = node->next_sibling;
        if (parent_node->last_child == index) parent_node->last_child = node->prev_sibling;
        if (node->prev_sibling >= 0) {
            po_array_node_fast(array, node->prev_sibling)->next_sibling = node->next_sibling;
        }
        if (node->next_sibling >= 0) {
            po_array_node_fast(array, node->next_sibling)->prev_sibling = node->prev_sibling;
        }
    }
}

void po_array_delete(PreOrderCompactionArray *array, int index) {
    if (!array || index < 0 || (size_t)index >= array->count) return;

    PreOrderCompactionArrayNode *root_node = po_array_node_fast(array, index);
    if (root_node->state == PO_ARRAY_NODE_TOMBSTONE) return;

    /* Mark the whole subtree as tombstone. */
    int *stack = (int*)malloc(array->count * sizeof(int));
    if (!stack) return;

    int top = 0;
    stack[top++] = index;

    while (top > 0) {
        int idx = stack[--top];
        PreOrderCompactionArrayNode *node = po_array_node_fast(array, idx);
        if (node->state == PO_ARRAY_NODE_TOMBSTONE) continue;

        int child = node->first_child;
        while (child >= 0) {
            stack[top++] = child;
            child = po_array_node_fast(array, child)->next_sibling;
        }

        node->state = PO_ARRAY_NODE_TOMBSTONE;
        array->active_count--;
        array->tombstone_count++;
    }

    free(stack);

    /* Unlink the root of the deleted subtree from the active tree.
     * The links in the now-tombstoned node are still intact. */
    po_array_unlink(array, index);
}

size_t po_array_compact(PreOrderCompactionArray *array) {
    if (!array) return 0;
    if (array->tombstone_count == 0) return array->active_count;

    size_t new_capacity = array->active_count;
    if (new_capacity < 16) new_capacity = 16;

    char *new_buffer = (char*)malloc(new_capacity * array->node_size);
    if (!new_buffer) return 0;

    int *stack = (int*)malloc(array->count * sizeof(int));
    int *old_to_new = (int*)malloc(array->count * sizeof(int));
    if (!stack || !old_to_new) {
        free(new_buffer);
        free(stack);
        free(old_to_new);
        return 0;
    }

    for (size_t i = 0; i < array->count; i++) old_to_new[i] = -1;

    /* Push roots in reverse order so first root is popped first. */
    int top = 0;
    for (int i = (int)array->count - 1; i >= 0; i--) {
        PreOrderCompactionArrayNode *node = po_array_node_fast(array, i);
        if (node->state == PO_ARRAY_NODE_ACTIVE && node->parent < 0) {
            stack[top++] = i;
        }
    }

    size_t new_count = 0;
    while (top > 0) {
        int old_idx = stack[--top];
        PreOrderCompactionArrayNode *old_node = po_array_node_fast(array, old_idx);
        if (old_node->state != PO_ARRAY_NODE_ACTIVE) continue;

        char *dest = new_buffer + new_count * array->node_size;
        memcpy(dest, old_node, array->node_size);

        old_to_new[old_idx] = (int)new_count;
        new_count++;

        /* Push children in reverse order. */
        int child = old_node->last_child;
        while (child >= 0) {
            stack[top++] = child;
            child = po_array_node_fast(array, child)->prev_sibling;
        }
    }

    /* Remap links. */
    for (size_t i = 0; i < new_count; i++) {
        PreOrderCompactionArrayNode *node = (PreOrderCompactionArrayNode*)(new_buffer + i * array->node_size);
#define PO_REMAP(field) if (node->field >= 0) node->field = old_to_new[node->field]
        PO_REMAP(parent);
        PO_REMAP(first_child);
        PO_REMAP(last_child);
        PO_REMAP(next_sibling);
        PO_REMAP(prev_sibling);
#undef PO_REMAP
    }

    free(array->buffer);
    array->buffer = new_buffer;
    array->capacity = new_capacity;
    array->count = new_count;
    array->active_count = new_count;
    array->tombstone_count = 0;

    /* Recompute root list from new ordering. */
    array->first_root = -1;
    array->last_root = -1;
    for (size_t i = 0; i < new_count; i++) {
        PreOrderCompactionArrayNode *node = (PreOrderCompactionArrayNode*)(new_buffer + i * array->node_size);
        if (node->parent < 0) {
            if (array->first_root < 0) array->first_root = (int)i;
            array->last_root = (int)i;
        }
    }

    free(stack);
    free(old_to_new);
    return new_count;
}

PreOrderCompactionArrayNode* po_array_node(PreOrderCompactionArray *array, int index) {
    if (!array || !array->buffer || index < 0 || (size_t)index >= array->count) return NULL;
    return po_array_node_fast(array, index);
}

void* po_array_payload(PreOrderCompactionArray *array, int index) {
    if (!array || !array->buffer || index < 0 || (size_t)index >= array->count) return NULL;
    return array->buffer + (size_t)index * array->node_size;
}

int po_array_index_from_payload(const PreOrderCompactionArray *array, const void *payload) {
    if (!array || !array->buffer || !payload) return -1;
    const char *p = (const char*)payload;
    ptrdiff_t offset = p - array->buffer;
    if (offset < 0 || array->node_size == 0 || offset % (ptrdiff_t)array->node_size != 0) return -1;
    int idx = (int)(offset / (ptrdiff_t)array->node_size);
    if (idx < 0 || (size_t)idx >= array->count) return -1;
    return idx;
}

int po_array_parent(PreOrderCompactionArray *array, int index) {
    PreOrderCompactionArrayNode *node = po_array_node(array, index);
    return node ? node->parent : -1;
}

int po_array_first_child(PreOrderCompactionArray *array, int index) {
    PreOrderCompactionArrayNode *node = po_array_node(array, index);
    return node ? node->first_child : -1;
}

int po_array_last_child(PreOrderCompactionArray *array, int index) {
    PreOrderCompactionArrayNode *node = po_array_node(array, index);
    return node ? node->last_child : -1;
}

int po_array_next_sibling(PreOrderCompactionArray *array, int index) {
    PreOrderCompactionArrayNode *node = po_array_node(array, index);
    return node ? node->next_sibling : -1;
}

int po_array_prev_sibling(PreOrderCompactionArray *array, int index) {
    PreOrderCompactionArrayNode *node = po_array_node(array, index);
    return node ? node->prev_sibling : -1;
}

void po_array_preorder_foreach(PreOrderCompactionArray *array, PoArrayForeachCallback callback, void *user_data) {
    if (!array || !callback || !array->buffer) return;

    int *stack = (int*)malloc(array->count * sizeof(int));
    if (!stack) return;

    int top = 0;
    for (int i = (int)array->count - 1; i >= 0; i--) {
        PreOrderCompactionArrayNode *node = po_array_node_fast(array, i);
        if (node->state == PO_ARRAY_NODE_ACTIVE && node->parent < 0) {
            stack[top++] = i;
        }
    }

    while (top > 0) {
        int idx = stack[--top];
        PreOrderCompactionArrayNode *node = po_array_node_fast(array, idx);
        if (node->state != PO_ARRAY_NODE_ACTIVE) continue;

        callback(idx, array->buffer + (size_t)idx * array->node_size, user_data);

        int child = node->last_child;
        while (child >= 0) {
            stack[top++] = child;
            child = po_array_node_fast(array, child)->prev_sibling;
        }
    }

    free(stack);
}

size_t po_array_count(const PreOrderCompactionArray *array) {
    return array ? array->count : 0;
}

size_t po_array_active_count(const PreOrderCompactionArray *array) {
    return array ? array->active_count : 0;
}

bool po_array_is_active(const PreOrderCompactionArray *array, int index) {
    if (!array || index < 0 || (size_t)index >= array->count) return false;
    PreOrderCompactionArrayNode *node = po_array_node_fast((PreOrderCompactionArray*)array, index);
    return node && node->state == PO_ARRAY_NODE_ACTIVE;
}

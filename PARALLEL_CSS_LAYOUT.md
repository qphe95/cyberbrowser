# Parallel CSS Layout Plan

This document describes how to parallelize CSS layout — the phase that turns
computed style declarations into concrete layout values (sizes, positions,
margins, etc.) — using the existing DOM tree and computed-style table.

Layout naturally splits into two directional passes:

* **Top-down (pre-order)** properties: values flow from parent to child
  (inheritance, containing-block width, x/y position, etc.).
* **Bottom-up (post-order)** properties: values flow from children to parent
  (auto height, intrinsic sizes, scroll extents, etc.).

Because we can lay the DOM out into arrays that already respect these
orderings, each worker only needs to process its assigned subtree in array
order. Synchronization is reduced to a single boundary per subtree instead of
per-node spinlocks.

---

## 1. Property categories

### 1.1 Top-down / pre-order

These depend only on the parent (or ancestor) and on the element's own
computed style. They can be evaluated as soon as the parent is processed.

Examples:

* `color`, `font-size`, `line-height`, `text-align`, `visibility`, `cursor`
* containing-block width (used for percentage resolution)
* `x`, `y` / offsetLeft/offsetTop for normal flow
* resolved `position` context (static/relative/absolute/fixed)
* text direction / writing mode propagation

### 1.2 Bottom-up / post-order

These require all descendants to be resolved first.

Examples:

* `height: auto` (sum of children's heights + padding/border)
* `min-height`, `max-height` when based on content
* intrinsic width/height for replaced elements and blocks
* `scrollHeight`, `scrollWidth`
* line-box height and baseline position

### 1.3 Independent / either order

Many used values can be resolved from computed style alone, e.g. explicit
`width: 100px`, `margin-left: 10px`, `display: block`. These are resolved
lazily on first read or during whichever pass needs them.

---

## 2. Layout tree representation

The layout pass works directly on the existing DOM tree (`DOMNode` handles and
their `parent_node`, `first_child`, `next_sibling` links). To avoid contention
and expensive tree walks, we build two flat auxiliary arrays once, on the main
thread, before dispatching workers:

```c
typedef struct LayoutNodeRef {
    GCHandle dom_node_handle;   /* handle to DOMNode data */
    int parent_idx;             /* index in layout array, or -1 for root */
    int first_child_idx;
    int next_sibling_idx;
    int child_count;
    int subtree_size;           /* number of nodes in this subtree */
} LayoutNodeRef;

typedef struct LayoutTree {
    LayoutNodeRef *nodes;
    int count;
    int root_idx;
} LayoutTree;
```

The arrays are built in two orders:

* `preorder[]`  — root first, then recursively children left-to-right.
* `postorder[]` — all descendants before their parent (true post-order).

In both arrays the nodes of any single subtree occupy one contiguous range,
which makes subtree-aligned chunking trivial:

* In `preorder[]`, subtree of node `i` occupies `[i, i + subtree_size(i) - 1]`.
* In `postorder[]`, subtree of node `i` occupies `[i - subtree_size(i) + 1, i]`.

---

## 3. Per-subtree synchronization state

Only the boundary between subtrees needs synchronization. Each node keeps a
single flag for correctness checks and incremental layout; the hot loops do not
spin.

```c
typedef struct LayoutNodeState {
    _Atomic(uint32_t) top_down_done;   /* set after top-down processing */
    _Atomic(uint32_t) bottom_up_done;  /* set after bottom-up processing */
    _Atomic(uint32_t) children_remaining;
        /* for bottom-up: number of child subtrees not yet reported */
} LayoutNodeState;
```

A `LayoutNodeState states[]` array parallels `LayoutTree.nodes[]`.

### 3.1 Initialization

```c
for (int i = 0; i < tree.count; i++) {
    atomic_store(&states[i].top_down_done, 0);
    atomic_store(&states[i].bottom_up_done, 0);
    atomic_store(&states[i].children_remaining, tree.nodes[i].child_count);
}
```

---

## 4. Top-down pass (pre-order)

### 4.1 Work distribution

The `preorder[]` array is partitioned into chunks that are aligned to subtree
boundaries. Each chunk is assigned to one worker, and the worker processes the
chunk's nodes strictly in `preorder[]` order.

Because a subtree's nodes are contiguous in `preorder[]` and the parent is the
first node of that range, processing the range in order guarantees that every
child is processed after its own parent *inside the same chunk*. No spinlock is
needed inside the chunk.

The only dependency that crosses chunks is the root of a child subtree waiting
for its parent, which lives in an earlier chunk. We handle that with a single
flag check on the root's parent before the chunk starts.

### 4.2 Per-node algorithm

```c
static void layout_top_down_node(LayoutContext *ctx, int idx)
{
    LayoutNodeRef *node = &ctx->tree.nodes[idx];

    /* Compute top-down properties.  Parent is guaranteed done for every node
       except the root of a chunk; the chunk entry handles that one case. */
    DOMNodeHandle dom(node->dom_node_handle);
    layout_resolve_inherited(ctx, dom);
    layout_resolve_containing_block(ctx, dom);
    layout_resolve_position(ctx, dom);

    atomic_store(&ctx->states[idx].top_down_done, 1);
}
```

### 4.3 Chunk algorithm

```c
static void layout_top_down_chunk(LayoutContext *ctx, int chunk_start, int chunk_end)
{
    /* Wait once for the parent of the subtree root, if it exists. */
    int root_idx = ctx->preorder[chunk_start];
    int parent_idx = ctx->tree.nodes[root_idx].parent_idx;
    if (parent_idx >= 0) {
        while (atomic_load(&ctx->states[parent_idx].top_down_done) == 0) {
            js_thread_yield();
        }
    }

    /* Process the whole subtree in pre-order. */
    for (int i = chunk_start; i < chunk_end; i++) {
        layout_top_down_node(ctx, ctx->preorder[i]);
    }
}
```

### 4.4 Worker job

```c
static void layout_top_down_job(void *arg)
{
    LayoutChunk *chunk = (LayoutChunk*)arg;
    layout_top_down_chunk(chunk->ctx, chunk->start, chunk->end);
}
```

### 4.5 Why no per-node spinlock is needed

Inside a chunk the parent always precedes its children in `preorder[]`, so a
worker never observes an unprocessed parent for a node it is about to process.
The one external dependency — the chunk root's parent — is resolved once before
the chunk's loop begins.

---

## 5. Bottom-up pass (post-order)

### 5.1 Work distribution

The `postorder[]` array is also partitioned into subtree-aligned chunks. Each
worker processes its chunk strictly in `postorder[]` order. Because descendants
appear before ancestors in `postorder[]`, every child inside a chunk is
processed before its parent. No spinlock is needed inside the chunk.

The only cross-chunk dependency is a parent waiting for all of its child
subtrees to report completion. Each child subtree reports once when its root
finishes, so the parent decrements `children_remaining` once per child subtree.

### 5.2 Per-node algorithm

```c
static void layout_bottom_up_node(LayoutContext *ctx, int idx)
{
    LayoutNodeRef *node = &ctx->tree.nodes[idx];

    /* Compute bottom-up properties.  All descendants inside the chunk are
       already done; descendants in other chunks are handled below. */
    DOMNodeHandle dom(node->dom_node_handle);
    layout_resolve_auto_height(ctx, dom);
    layout_resolve_scroll_extents(ctx, dom);

    atomic_store(&ctx->states[idx].bottom_up_done, 1);
}
```

### 5.3 Chunk algorithm

```c
static void layout_bottom_up_chunk(LayoutContext *ctx, int chunk_start, int chunk_end)
{
    /* Wait once for all child subtrees that are not in this chunk. */
    int root_idx = ctx->postorder[chunk_end - 1];
    LayoutNodeState *root_state = &ctx->states[root_idx];
    while (atomic_load(&root_state->children_remaining) > 0) {
        js_thread_yield();
    }

    /* Process the whole subtree in post-order. */
    for (int i = chunk_start; i < chunk_end; i++) {
        layout_bottom_up_node(ctx, ctx->postorder[i]);
    }

    /* Report to the parent that this whole subtree is done. */
    int parent_idx = ctx->tree.nodes[root_idx].parent_idx;
    if (parent_idx >= 0) {
        atomic_fetch_sub(&ctx->states[parent_idx].children_remaining, 1);
    }
}
```

### 5.4 Worker job

```c
static void layout_bottom_up_job(void *arg)
{
    LayoutChunk *chunk = (LayoutChunk*)arg;
    layout_bottom_up_chunk(chunk->ctx, chunk->start, chunk->end);
}
```

### 5.5 Leaf optimization

Leaf subtrees have `children_remaining == 0` immediately, so they never spin.
This produces a large amount of ready work up front.

### 5.6 Why no per-node spinlock is needed

Inside a chunk every descendant precedes its ancestors in `postorder[]`, so a
worker never observes an unfinished child for a node it is about to process.
The one external dependency — waiting for child subtrees assigned to other
workers — is resolved once before the chunk's loop begins, and the chunk
reports completion to its parent once after the loop ends.

---

## 6. Chunking strategy

A simple chunker greedily consumes `preorder[]` from left to right, taking
whole subtrees until a target chunk size is reached:

```c
static int layout_split_chunks(LayoutTree *tree, int *order, int node_count,
                               int target_chunk_size, LayoutChunk *chunks)
{
    int chunk_count = 0;
    int i = 0;
    while (i < node_count) {
        int idx = order[i];
        int size = tree->nodes[idx].subtree_size;
        int end = i + size;

        /* Try to merge following sibling subtrees into the same chunk
           without splitting them. */
        while (end < node_count &&
               (end - i) < target_chunk_size) {
            int next_idx = order[end];
            if (tree->nodes[next_idx].parent_idx != tree->nodes[idx].parent_idx)
                break;   /* not a sibling subtree */
            end += tree->nodes[next_idx].subtree_size;
        }

        chunks[chunk_count].start = i;
        chunks[chunk_count].end   = end;
        chunk_count++;
        i = end;
    }
    return chunk_count;
}
```

The same splitting logic works for both `preorder[]` and `postorder[]` because
both arrays store whole subtrees contiguously.

---

## 7. Layout value storage

A new `LayoutBox` struct is attached to each `DOMNode` (or stored in a parallel
array keyed by DOMNode handle):

```c
typedef struct LayoutBox {
    double x, y;
    double width, height;
    double margin_top, margin_right, margin_bottom, margin_left;
    double padding_top, padding_right, padding_bottom, padding_left;
    double border_top, border_right, border_bottom, border_left;
    double content_width, content_height;
    double baseline;
    uint32_t flags;  /* LAYOUT_NEEDS_LAYOUT, etc. */
} LayoutBox;
```

Add a `GCHandle layout_box_handle` field to `DOMNode` and a `gc_mark` callback
that marks it. The `LayoutBox` values are raw numbers, not GC references, so
the mark callback only needs to keep the `LayoutBox` object alive.

---

## 8. Integration with computed-style table

Both layout passes read from the per-element `CssComputedStyle` table:

```c
GCValue val = css_computed_get_property(ctx, dom_node, atom);
const char *str = JS_ToCString(ctx, val);
/* resolve used value ... */
```

The computed-style table is populated in parallel during CSS application and is
read-only during layout, so no additional synchronization is required.

---

## 9. Handling incremental layout

When a subset of the DOM changes:

1. Mark the changed subtree's root with `LAYOUT_NEEDS_LAYOUT`.
2. Rebuild `LayoutTree` only if the tree structure changed.
3. Run the top-down pass starting from the nearest ancestor whose containing
   block changed.
4. Run the bottom-up pass starting from the changed subtree root.

For the first implementation, always do a full document layout.

---

## 10. Memory ordering

All synchronization variables use `memory_order_seq_cst` (or the default
`atomic_*` operations) for simplicity. The hot path is the property computation
itself; the boundary wait only runs when a chunk root depends on another chunk,
which is one wait per chunk rather than one per node.

If profiling shows contention at the chunk boundaries, switch to:

* `_mm_pause()` / `__builtin_ia32_pause()` in the boundary loops.
* Hybrid spin-then-yield (spin N iterations, then `js_thread_yield()`).
* Work-stealing so a waiting thread can process other ready chunks.

---

## 11. Pseudocode: full layout function

```c
void css_layout_document(JSContextHandle ctx, HtmlDocument *doc)
{
    LayoutContext ctx;
    ctx.rt = JS_GetRuntime(ctx);

    /* 1. Build flat layout tree (serial, main thread). */
    layout_tree_from_dom(ctx, doc, &ctx.tree);

    ctx.states = (LayoutNodeState*)calloc(ctx.tree.count, sizeof(LayoutNodeState));
    for (int i = 0; i < ctx.tree.count; i++) {
        atomic_store(&ctx.states[i].top_down_done, 0);
        atomic_store(&ctx.states[i].bottom_up_done, 0);
        atomic_store(&ctx.states[i].children_remaining, ctx.tree.nodes[i].child_count);
    }

    ctx.preorder  = (int*)malloc(ctx.tree.count * sizeof(int));
    ctx.postorder = (int*)malloc(ctx.tree.count * sizeof(int));
    layout_build_preorder(&ctx.tree, ctx.preorder);
    layout_build_postorder(&ctx.tree, ctx.postorder);

    /* 2. Top-down pass. */
    int td_count = layout_split_chunks(&ctx.tree, ctx.preorder, ctx.tree.count,
                                       TARGET_CHUNK_SIZE, ctx.chunks);
    layout_dispatch_jobs(&ctx, ctx.chunks, td_count, layout_top_down_job);
    gc_thread_pool_wait_empty();

    /* 3. Bottom-up pass. */
    int bu_count = layout_split_chunks(&ctx.tree, ctx.postorder, ctx.tree.count,
                                       TARGET_CHUNK_SIZE, ctx.chunks);
    layout_dispatch_jobs(&ctx, ctx.chunks, bu_count, layout_bottom_up_job);
    gc_thread_pool_wait_empty();

    free(ctx.preorder);
    free(ctx.postorder);
    free(ctx.states);
    layout_tree_free(&ctx.tree);
}
```

---

## 12. Execution order

1. Add `LayoutBox` struct and `layout_box_handle` to `DOMNode`.
2. Add DOMNode `gc_mark` callback updates to mark the layout box.
3. Implement `layout_tree_from_dom`, `layout_build_preorder`,
   `layout_build_postorder`, and per-node `subtree_size` computation.
4. Implement subtree-aligned chunk splitting.
5. Implement top-down pass with one boundary wait per chunk.
6. Implement bottom-up pass with one boundary wait and one completion signal
   per chunk.
7. Resolve simple top-down properties (inheritance, containing-block width).
8. Resolve simple bottom-up properties (`height: auto`, scroll extents).
9. Add tests for both passes.
10. Profile and tune boundary-wait behavior and chunk size.

---

## 13. Success criteria

* A full-document layout runs in two parallel passes.
* Top-down properties produce correct values when children depend on parents.
* Bottom-up properties produce correct values when parents depend on children.
* No per-node spinlocks; synchronization is coarse-grained at subtree boundaries.
* No deadlocks or livelocks under multi-core load.
* Layout results match a serial reference implementation on a diverse set of
  test documents.
* All existing tests still pass.

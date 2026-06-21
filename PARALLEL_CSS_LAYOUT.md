# Parallel CSS Layout Plan

This document describes how to parallelize CSS layout — the phase that turns
computed style declarations into concrete layout values (sizes, positions,
margins, etc.) — using the existing DOM tree and computed-style table.

Layout naturally splits into two directional passes:

* **Top-down (pre-order)** properties: values flow from parent to child
  (inheritance, containing-block width, x/y position, etc.).
* **Bottom-up (post-order)** properties: values flow from children to parent
  (auto height, intrinsic sizes, scroll extents, etc.).

Each pass is parallelized over the DOM tree with fine-grained spin-wait
synchronization on per-node flags/counters.

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

## 2. Required spin-wait dependencies

Because the two passes traverse the same tree in opposite directions, workers
must block on unresolved dependencies. The blocking is implemented as a
short, fine-grained spin-wait on per-node atomic flags/counters:

* **Top-down pass:** each child spin-waits until its parent's `top_down_done`
  flag is set, and until its previous sibling's `top_down_done` flag is set.
  The previous-sibling edge creates a dependency chain that serializes sibling
  layout while still allowing unrelated subtrees to run in parallel.  The root
  has no parent and starts immediately; first children have no previous sibling
  and initialize their parent's line-flow state.
* **Bottom-up pass:** each parent spin-waits until its `children_remaining`
  counter reaches zero (i.e., every child has finished and decremented the
  counter). Leaf nodes start immediately.

These waits are the core synchronization mechanism; they cannot be removed or
replaced by naive scheduling because chunk boundaries cut across parent/child
edges and workers may otherwise overtake their dependencies.

---

## 3. Layout tree representation

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
    int prev_sibling_idx;       /* previous sibling index, or -1 for first child */
    int child_count;
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

Both arrays contain the same `LayoutNodeRef` records; only the traversal order
is different.

---

## 4. Per-node synchronization state

Each `LayoutNodeRef` gets a small synchronization block:

```c
typedef struct LayoutNodeState {
    /* Top-down pass: also reused as the previous-sibling completion flag. */
    _Atomic(uint32_t) top_down_done;   /* 0 = pending, 1 = done */

    /* Bottom-up pass */
    _Atomic(uint32_t) children_remaining;
    _Atomic(uint32_t) bottom_up_done;  /* 0 = pending, 1 = done */
} LayoutNodeState;
```

A single `LayoutNodeState states[]` array parallels the `LayoutTree.nodes[]`
array.

### 4.1 Initialization

```c
for (int i = 0; i < tree.count; i++) {
    atomic_store(&states[i].top_down_done, 0);
    atomic_store(&states[i].bottom_up_done, 0);
    atomic_store(&states[i].children_remaining, tree.nodes[i].child_count);
}
/* The root has no parent; mark it runnable immediately. */
atomic_store(&states[root].top_down_done, 0);
```

---

## 5. Top-down pass (pre-order)

### 5.1 Work distribution

The `preorder[]` array is split into contiguous chunks and submitted to the
`gc_thread_pool`. A node appears in `preorder[]` before its descendants, so
most of the time a worker reaches a node only after its parent has already been
processed by an earlier thread.

### 5.2 Per-node algorithm

The top-down pass resolves inherited/containing-block values **and** performs
block sibling stacking and inline line-flow.  Each node waits for its previous
sibling before reading or updating the parent's temporary line state, so
siblings are laid out in order without a serial post-pass.

```c
static void layout_top_down_node(LayoutContext *ctx, int idx)
{
    LayoutNodeRef *node = &ctx->tree.nodes[idx];
    LayoutNodeState *state = &ctx->states[idx];
    LayoutBox *box = &ctx->boxes[idx];

    if (node->parent_idx >= 0) {
        /* Wait for parent. */
        while (atomic_load(&ctx->states[node->parent_idx].top_down_done) == 0) {
            js_thread_yield();
        }
        LayoutBox *parent = &ctx->boxes[node->parent_idx];

        layout_resolve_inherited(ctx, box, parent);
        layout_resolve_width(ctx, box, parent);

        /* Wait for previous sibling before touching parent's line state. */
        if (node->prev_sibling_idx >= 0) {
            while (atomic_load(&ctx->states[node->prev_sibling_idx].top_down_done) == 0) {
                js_thread_yield();
            }
        } else {
            /* First child initializes the line state. */
            parent->line_x = parent->content_left;
            parent->line_y_offset = 0;
            parent->line_height = 0;
        }

        /* Stack block boxes, flow inline/inline-block boxes on lines. */
        if (is_block_flow(box)) {
            parent->line_y_offset += parent->line_height;
            parent->line_height = 0;
            box->x = parent->content_left + box->margin_left;
            box->y = parent->content_top + parent->line_y_offset + box->margin_top;
            parent->line_y_offset = box->y + box->height + box->margin_bottom
                                    - parent->content_top;
        } else {
            layout_flow_inline_box(ctx, box, parent);
        }
    } else {
        /* Root forms the initial containing block. */
        box->x = 0;
        box->y = 0;
        box->width = ctx->viewport_width;
        box->height = ctx->viewport_height;
    }

    atomic_store(&state->top_down_done, 1);
}
```

### 5.3 Worker job

```c
static void layout_top_down_job(void *arg)
{
    LayoutChunk *chunk = (LayoutChunk*)arg;
    for (int i = chunk->start; i < chunk->end; i++) {
        layout_top_down_node(chunk->ctx, chunk->preorder[i]);
    }
}
```

### 5.4 Scheduling

```c
layout_build_preorder(&ctx->tree, preorder);
layout_dispatch_chunks(ctx, preorder, ctx->tree.count, layout_top_down_job);
gc_thread_pool_wait_empty();
```

### 5.5 Why spin-wait is safe

Every node in `preorder[]` has its parent and its previous sibling somewhere
earlier in the array (pre-order is a valid topological order for both the
parent/child and sibling/sibling edges). If a worker reaches a child before the
parent is done, or reaches a sibling before the previous sibling is done, it
yields and retries. The dependency graph is still a DAG (a tree plus a
left-to-right sibling chain), so no deadlocks are possible.

---

## 6. Bottom-up pass (post-order)

### 6.1 Work distribution

The `postorder[]` array is split into chunks. A node appears in `postorder[]`
after all of its descendants, so most nodes are processed after their children.

### 6.2 Per-node algorithm

```c
static void layout_bottom_up_node(LayoutContext *ctx, int idx)
{
    LayoutNodeRef *node = &ctx->tree.nodes[idx];
    LayoutNodeState *state = &ctx->states[idx];

    /* Wait for all children. */
    while (atomic_load(&state->children_remaining) > 0) {
        js_thread_yield();
    }

    /* Compute bottom-up properties.  Auto height is derived from the
     * bottom-most child's position, which was set during the top-down pass. */
    DOMNodeHandle dom(node->dom_node_handle);
    layout_resolve_auto_height(ctx, dom);
    layout_resolve_scroll_extents(ctx, dom);

    atomic_store(&state->bottom_up_done, 1);

    /* Notify parent that one more child is finished. */
    if (node->parent_idx >= 0) {
        atomic_fetch_sub(&ctx->states[node->parent_idx].children_remaining, 1);
    }
}
```

### 6.3 Worker job

```c
static void layout_bottom_up_job(void *arg)
{
    LayoutChunk *chunk = (LayoutChunk*)arg;
    for (int i = chunk->start; i < chunk->end; i++) {
        layout_bottom_up_node(chunk->ctx, chunk->postorder[i]);
    }
}
```

### 6.4 Leaf optimization

Leaf nodes have `children_remaining == 0` immediately, so they never spin. This
gives the pass a large amount of ready work up front.

### 6.5 Why spin-wait is safe

Every node in `postorder[]` appears after all descendants. If a worker reaches
a parent before all children are done, the parent's counter is still positive
and the worker yields. Children decrement the counter when they finish. The
last child to finish leaves the counter at zero and unblocks the parent.

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

    /* Temporary line-flow state used during the top-down pass.  The parent box
     * tracks the current line; children update it sequentially through the
     * previous-sibling dependency chain. */
    double line_x;
    double line_y_offset;
    double line_height;

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
itself; the spin-wait only runs when a worker gets ahead of its dependency,
which should be rare with reasonable chunk sizes.

If profiling shows contention, switch to:

* `_mm_pause()` / `__builtin_ia32_pause()` in the spin loop.
* Hybrid spin-then-yield (spin N iterations, then `js_thread_yield()`).
* Work-stealing so a waiting thread can process other ready nodes.

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

    int *preorder  = (int*)malloc(ctx.tree.count * sizeof(int));
    int *postorder = (int*)malloc(ctx.tree.count * sizeof(int));
    layout_build_preorder(&ctx.tree, preorder);
    layout_build_postorder(&ctx.tree, postorder);

    /* 2. Top-down pass: inheritance, containing-block widths, block sibling
     *    stacking, and inline line-flow (parallel, with parent + previous-
     *    sibling spin-waits). */
    layout_dispatch_chunks(&ctx, preorder, ctx.tree.count, layout_top_down_job);
    gc_thread_pool_wait_empty();

    /* 3. Bottom-up pass: auto heights from stacked child positions (parallel,
     *    with children_remaining spin-waits). */
    layout_dispatch_chunks(&ctx, postorder, ctx.tree.count, layout_bottom_up_job);
    gc_thread_pool_wait_empty();

    free(preorder);
    free(postorder);
    free(ctx.states);
    layout_tree_free(&ctx.tree);
}
```

---

## 12. Execution order

1. Add `LayoutBox` struct and `layout_box_handle` to `DOMNode`.
2. Add DOMNode `gc_mark` callback updates to mark the layout box.
3. Implement `layout_tree_from_dom`, `layout_build_preorder`,
   `layout_build_postorder`.
4. Implement top-down pass with parent + previous-sibling spin-waits
   (see section 5).
5. Implement block sibling stacking and inline line-flow inside the top-down
   pass.
6. Implement bottom-up pass with children-remaining spin-wait (see section 6).
7. Resolve simple top-down properties (inheritance, containing-block width).
8. Resolve bottom-up properties (`height: auto` from child positions, scroll
   extents).
9. Add tests for both passes.
10. Profile and tune spin-wait behavior.

---

## 13. Success criteria

* A full-document layout runs in two parallel passes.
* Top-down properties produce correct values when children depend on parents
  and siblings depend on previous siblings.
* Bottom-up properties produce correct values when parents depend on children.
* No deadlocks or livelocks under multi-core load.
* Layout results match a serial reference implementation on a diverse set of
  test documents.
* All existing tests still pass.

# Parallel CSS Application V2 Plan

This document is a concrete implementation plan for moving CSS selector matching,
computed-style construction, and inline-style parsing off the main JavaScript
mutator thread and into the existing `gc_thread_pool` workers.

The plan assumes the foundational work from `PARALLEL_CSS_APPLICATION.md` is in
place: publication state (`PUBLISH_GREY`/`PUBLISH_BLACK`), the generic
`LFHashTable`, lock-free atom/shape caches, and the lock-free job queue. The
remaining blockers are addressed below in dependency order.

---

## 1. DOMNode `gc_mark` callback

### Problem

`DOMNode` stores tree links and other GC references as `GCValue` fields, but
`js_dom_node_class_def` only registers a finalizer. The unified GC cannot see
those references, so a worker-constructed grey DOM node is not kept alive by the
GC unless the worker holds a local root.

### Implementation

1. Add a `gc_mark` function in `cyberbrowser/src/browser_api_impl.cpp`:

   ```c
   static void js_dom_node_mark(JSRuntimeHandle rt, GCValue val,
                                JS_MarkFunc *mark_func)
   {
       GCHandle node_handle = JS_GetOpaqueHandle(val, JS_CLASS_DOM_NODE);
       if (node_handle == GC_HANDLE_NULL) return;
       DOMNodeHandle node(node_handle);
       if (!node) return;

       JS_MarkValue(rt, node.parent_node(), mark_func);
       JS_MarkValue(rt, node.first_child(), mark_func);
       JS_MarkValue(rt, node.last_child(), mark_func);
       JS_MarkValue(rt, node.previous_sibling(), mark_func);
       JS_MarkValue(rt, node.next_sibling(), mark_func);
       JS_MarkValue(rt, node.owner_document(), mark_func);
       JS_MarkValue(rt, node.shadow_root(), mark_func);

       if (node.computed_style_handle() != GC_HANDLE_NULL)
           mark_func(rt, node.computed_style_handle());
   }
   ```

2. Register it in `js_dom_node_class_def`:

   ```c
   static JSClassDef js_dom_node_class_def = {
       .class_name = "DOMNode",
       .finalizer = js_dom_node_finalizer,
       .gc_mark   = js_dom_node_mark,
   };
   ```

3. Make sure `DOMNodeHandle` exposes getters for the `GCValue` fields; if the
   handle wrapper only exposes raw pointers, add `parent_node()`, etc., that
   return the embedded `GCValue`.

### Verification

- Run the existing DOM/GC tests.
- Add a targeted test that creates a detached DOM subtree, runs a full GC, and
  verifies the subtree and its `DOMNode` data survive.

---

## 2. Make the GC marker respect `publish_state`

### Problem

`gc_mark_object()` currently checks only `gc_color_state`. The publication-state
array is intended to keep grey objects alive during construction, but the marker
never consults it. A grey object that loses its worker-local root can be
 collected before it is published.

### Implementation

1. In `quickjs_gc_unified.cpp`, update the marking path so that any object whose
   `publish_state` is not `PUBLISH_BLACK` is treated as an opaque root:

   ```c
   static void gc_mark_object(GCHandle handle)
   {
       if (handle == GC_HANDLE_NULL) return;

       GCPublishState pub = gc_publish_state_load(handle);
       if (pub == PUBLISH_GREY) {
           /* Opaque root: keep alive, but do not scan children yet.
            * The constructing thread is responsible for publishing. */
           gc_shade(handle); /* or set BLACK directly */
           return;
       }
       if (pub == PUBLISH_UNBORN) {
           return;
       }

       /* Normal tri-color marking for PUBLISH_BLACK objects. */
       ...
   }
   ```

2. Alternatively, treat `PUBLISH_GREY` like `GC_COLOR_GREY`: shade it and let
   the marker scan its children. This is simpler but requires that grey objects
   are always in a consistent enough state to be scanned. For DOM nodes the
   `gc_mark` callback above only reads already-initialized fields, so scanning
   is safe once the header and `DOMNode` fields are written.

3. Add an assertion in `gc_publish()` that the object is currently `PUBLISH_GREY`.

### Verification

- Add a test that allocates an object with `gc_alloc_grey()`, drops all local
  references, runs a full GC, and verifies the object survives. Then publish it
  and run GC again to verify it is collected if unreachable.

---

## 3. Per-element computed-style table

### Problem

`DOMNode` has no place to store computed CSS properties. `getComputedStyle()` is
a stub. Workers cannot write computed declarations in parallel because there is
no lock-free destination.

### Implementation

1. Add a `computed_style_handle` field to `DOMNode` in
   `browser-api_impl_types.h`:

   ```c
   typedef struct DOMNode {
       ...
       GCHandle computed_style_handle;  /* handle to CssComputedStyle */
   } DOMNode;
   ```

2. Define a small GC-managed computed-style object:

   ```c
   typedef struct CssComputedStyle {
       LFHashTable *properties;  /* atom -> CSSValue handle */
   } CssComputedStyle;
   ```

   For the first version, the table can be a simple flat array if the number of
   computed properties per element is small. A lock-free hash table is the
   scalable version.

3. Allocate the computed-style object lazily when the first worker writes a
   property:

   ```c
   GCHandle css_ensure_computed_style(DOMNodeHandle node)
   {
       GCHandle h = node.computed_style_handle();
       if (h != GC_HANDLE_NULL) return h;

       h = gc_allocz(sizeof(CssComputedStyle), JS_GC_OBJ_TYPE_DATA);
       if (h == GC_HANDLE_NULL) return GC_HANDLE_NULL;

       CssComputedStyleHandle cs(h);
       cs.set_properties(lf_hash_create(CSS_PROPERTY_TABLE_BUCKETS));
       node.set_computed_style_handle(h);
       return h;
   }
   ```

4. Add worker-safe write accessor:

   ```c
   void css_computed_set_property(DOMNodeHandle node, JSAtom prop_atom,
                                  GCHandle value_handle)
   {
       GCHandle cs_handle = css_ensure_computed_style(node);
       if (cs_handle == GC_HANDLE_NULL) return;
       CssComputedStyleHandle cs(cs_handle);
       lf_hash_insert(cs.properties(), prop_atom, value_handle);
   }
   ```

5. Implement `getComputedStyle()` to read from this table instead of returning
   static values:

   ```c
   GCHandle css_computed_get_property(DOMNodeHandle node, JSAtom prop_atom)
   {
       GCHandle cs_handle = node.computed_style_handle();
       if (cs_handle == GC_HANDLE_NULL) return GC_HANDLE_NULL;
       CssComputedStyleHandle cs(cs_handle);
       return lf_hash_lookup(cs.properties(), prop_atom);
   }
   ```

### Verification

- Extend the CSS parser tests to set and read computed properties through the
  new table.
- Run `test_css_apply_parallel` and verify it still passes.

---

## 4. CSS index tables (id / class / tag)

### Problem

Selector matching walks the whole DOM and/or stylesheet. `getElementById`,
`getElementsByClassName`, and `getElementsByTagName` need index tables to be
fast, and those tables must be safe for workers to update while nodes are being
published.

### Implementation

1. Add the tables to `JSRuntime` (or a dedicated `CssDocumentState` object):

   ```c
   typedef struct CssDocumentState {
       LFHashTable *id_table;     /* atom -> DOMNode handle */
       LFHashTable *class_table;  /* atom -> head of DOMNode list */
       LFHashTable *tag_table;    /* atom -> head of DOMNode list */
   } CssDocumentState;
   ```

2. Allocate the state object at runtime/context creation and store its handle
   in `JSRuntime`.

3. Update DOM node publication to insert the node into the index tables:

   ```c
   void dom_publish_node(DOMNodeHandle node)
   {
       JSAtom id = node.id_atom();
       if (id != JS_ATOM_NULL) lf_hash_insert(css_state->id_table, id, node.handle());

       /* class attribute may contain multiple classes */
       for each class atom in node.class_list() {
           css_class_list_insert(css_state->class_table, class_atom, node.handle());
       }

       if (node.tag_atom() != JS_ATOM_NULL) {
           css_class_list_insert(css_state->tag_table, node.tag_atom(), node.handle());
       }

       gc_publish(node.handle());
   }
   ```

4. The class/tag tables need a list-of-handles value. Use the lock-free list
   pattern already present for map records: each bucket value is the head handle,
   and nodes in the same bucket are chained through a sibling field in `DOMNode`
   (e.g., `next_class_sibling`, `next_tag_sibling`).

5. Update reader accessors (`getElementById`, etc.) to check `PUBLISH_BLACK`:

   ```c
   GCHandle dom_get_element_by_id(JSRuntimeHandle rt, JSAtom id)
   {
       GCHandle h = lf_hash_lookup(css_state->id_table, id);
       if (h != GC_HANDLE_NULL && gc_publish_state_load(h) == PUBLISH_BLACK)
           return h;
       return GC_HANDLE_NULL;
   }
   ```

### Verification

- Add tests for `getElementById` / `getElementsByClassName` /
  `getElementsByTagName` correctness.
- Add a concurrent test that publishes nodes from workers while readers query
  the index tables.

---

## 5. Execution order

Recommended implementation order:

1. DOMNode `gc_mark` callback.
2. GC marker respects `publish_state`.
3. Add `computed_style_handle` to `DOMNode` and implement `getComputedStyle()`
   using it.
4. Add CSS index tables and update DOM node publication to populate them.
5. Parallelize inline-style parsing and stylesheet declaration application
   into the computed-style table.
6. Parallelize JS `element.style` writes (safe because each element is owned
   by exactly one worker and the runtime supports concurrent object mutation).
7. Optimize: let the renderer read computed-style tables directly for
   render-only properties.

---

## 6. Success criteria

- `css_apply_node_styles_parallel()` dispatches selector matching,
  computed-style construction, inline-style parsing, and JS `element.style`
  writes to workers.
- `getComputedStyle()` returns real values from the per-element table.
- DOM tree links survive GC when only `DOMNode` references keep a subtree alive.
- Grey DOM nodes under construction are not collected before publication.
- Index-table readers only observe `PUBLISH_BLACK` nodes.
- All existing tests pass, and `test_css_apply_parallel` shows measurable
  speedup on multi-core hardware.

# Missing browser APIs inventory — Polymer / ShadyDOM shadow DOM

This file lists the browser APIs and behaviours that are currently missing, stubbed, or incorrect in `cyberbrowser` and that block YouTube's Polymer (v3.5.0) shadow-DOM / ShadyDOM code path from stamping the real masthead and other custom elements without fallbacks.

## How the inventory was built

1. **Code inspection** of `cyberbrowser/src/browser_api_impl.cpp`, `cyberbrowser/src/browser/dom_api.cpp`, `cyberbrowser/src/browser/shadow_dom_api.cpp`, `cyberbrowser/src/browser/misc_api.cpp`, `cyberbrowser/src/browser/timer_api.cpp`, `cyberbrowser/src/browser/css_api.cpp`, `cyberbrowser/src/browser/console_api.cpp`, and `cyberbrowser/src/js_quickjs.cpp`.
2. **Runtime check** by running the current MinGW build against `https://www.youtube.com/watch?v=dQw4w9WgXcQ` and capturing stderr/stdout to `run_inventory.log`.
3. **Cross-reference** with the minified Polymer/YouTube source in `youtube_data/fetched_external_031.js` to confirm which APIs are actually exercised.

Note: `BROWSER_API_AUDIT.md` marks several of these as "implemented" or "stub", but the code shows they are either non-functional stubs or absent from the real prototype chain.

---

## Critical blockers (must be fixed for real masthead stamping)

| # | API / behaviour | Current state | Why it blocks Polymer / ShadyDOM | Source evidence |
|---|-----------------|---------------|-----------------------------------|-----------------|
| 1 | `Document.prototype.createDocumentFragment()` | ✅ Wired to the real `js_create_document_fragment()` helper in `browser_api_impl.cpp`. | Polymer's template parser creates fragments with `template.content.ownerDocument.createDocumentFragment()` (line 3466). Returning `null` aborted stamping. | `cyberbrowser/src/browser_api_impl.cpp` |
| 2 | `Document.prototype.importNode(node, deep)` | ✅ Deep-clones via `js_node_cloneNode_real()` and reassigns `ownerDocument`. | Polymer stamps a template with `document.importNode(template.content, true)` (line 3497). A shallow clone produced no real shadow-root content. | `cyberbrowser/src/browser/dom_api.cpp` |
| 3 | `Element.prototype.attributes` / `hasAttributes()` / `getAttributeNames()` | ✅ Added to both `Element.prototype` and `HTMLElement.prototype`. `attributes` is a live-like NamedNodeMap with `length`, indexed access, `.name`/`.value`, `.getNamedItem()`, `.item()`, `.setNamedItem()`, `.removeNamedItem()`. | Polymer reads attributes via `this.attributes`, `.length`, `[i]`, `.name`, `.value`, and `.getNamedItem()` (lines 3468, 7734, 8147). | `cyberbrowser/src/browser/dom_api.cpp` |
| 4 | `Document.prototype.getElementsByTagName()` | ✅ Tag index stores lower-case names; lookup normalizes the query and falls back to case-insensitive tree traversal. | Runtime previously showed `[CE-UPGRADE] upgradeAll <tag> found=0` for most definitions because the index was case-sensitive. | `cyberbrowser/src/browser/dom_api.cpp` |
| 5 | Custom element upgrade semantics | ✅ Replaced the global `__cyber_upgrade_target` hack with a per-global upgrade stack. `__cyber_upgradeElement` is now a C function that pushes the element, calls `new ctor()`, and lets the native `HTMLElement` constructor pop the stack and return the existing element as `this`. `connectedCallback` is invoked when the element is already connected. | The registered Polymer constructor for `ytd-masthead` now receives the existing element as `this` and runs `_attachDom`/`connectedCallback`; the final DOM contains real masthead content. | `cyberbrowser/src/browser/misc_api.cpp`; `cyberbrowser/src/browser_api_impl.cpp` |
| 6 | `Element.prototype.matches()` / `Element.prototype.closest()` | ✅ Added to both `Element.prototype` and `HTMLElement.prototype`, delegating to the existing `matches_selector()` helper. | Polymer uses `element.matches("dialog:modal")` and ancestor traversal (line 10025, gesture utilities). | `cyberbrowser/src/browser/dom_api.cpp` |

---

## High-priority gaps

| # | API / behaviour | Current state | Why it blocks / matters | Source evidence |
|---|-----------------|---------------|--------------------------|-----------------|
| 7 | `Element.prototype.getAttribute('style')` | Returns the live `CSSStyleDeclaration` object instead of the inline style string. | Breaks code that parses the inline style attribute as text (common in Polymer style modules and sanitizer). The current workaround is `setAttribute('style', ...)` only. | `cyberbrowser/src/browser/dom_api.cpp:1526` |
| 8 | `Document.prototype.createTreeWalker()` | Returns an object whose traversal methods (`nextNode`, `firstChild`, `parentNode`, …) are no-op dummies. | Used by YouTube's HTML sanitizer and Polymer's DOM scanners. | `cyberbrowser/src/browser/dom_api.cpp:2189-2204` |
| 9 | `MutationObserver.prototype.observe()` | No-op; `takeRecords()` returns empty array; `disconnect()` no-op. | The built-in `ShadyDOM` stub's `observeChildren()` is backed by `MutationObserver` (`browser_api_impl.cpp:3115`). Without real mutation delivery, dynamic slot/child changes are invisible. | `cyberbrowser/src/browser/misc_api.cpp:1500-1512` |
| 10 | `HTMLSlotElement` / `slot.assignedNodes()` / `slot.assignedElements()` | Only a dummy constructor exists; no slot API. | Polymer queries slots and calls `assignedNodes({flatten: true})` (line 3380). Even though the native serializer now flattens slots, JS-side code cannot inspect assignments. | `cyberbrowser/src/browser_api_impl.cpp:1517` |
| 11 | `Node.prototype.getRootNode(options)` | Ignores `{composed: true}` and stops at the shadow root instead of crossing to the document. | Shadow-DOM aware code uses `getRootNode({composed: true})` to reach the document. | `cyberbrowser/src/browser/dom_api.cpp:1100-1117` |
| 12 | `ShadowRoot` completeness | `ShadowRoot` does not inherit from `DocumentFragment`/Node fully; missing `adoptedStyleSheets`, `styleSheets`, `getElementsByTagName`, proper `parentNode`/`ownerDocument`. `attachShadow()` ignores `shadyUpgradeFragment` and other init options. | Polymer's `_attachDom` passes `{mode:'open', shadyUpgradeFragment:F}` and then appends the fragment; it later tries to set `shadowRoot.adoptedStyleSheets` when native adopted sheets are available (line 7830). | `cyberbrowser/src/browser/shadow_dom_api.cpp:570-595`; `cyberbrowser/src/browser_api_impl.cpp:1676-1699`, `2936-2978` |
| 13 | `queueMicrotask()` | Not defined. | Polymer/ShadyDOM use microtasks for flushing; it currently falls back to `Promise.resolve().then(...)`, but explicit `queueMicrotask` is expected by modern bundles. | not present in source |

---

## Medium / low-priority gaps

| # | API / behaviour | Current state | Why it matters | Source evidence |
|---|-----------------|---------------|----------------|-----------------|
| 14 | `window.getComputedStyle()` | Returns an object with hard-coded defaults plus values from the per-element computed-style hash table. No real cascade, inheritance, or pseudo-element support. | Layout/scripts that read computed metrics for stamping or positioning get wrong values. | `cyberbrowser/src/browser/console_api.cpp:468-539` |
| 15 | `CSSStyleSheet` / `adoptedStyleSheets` | `CSSStyleSheet` constructor is a dummy; `replace`/`replaceSync` do not parse; `document.styleSheets` and `ShadowRoot.styleSheets` are missing. | Polymer's adopted-style-sheet fast path (`AGo && aVD && FC$`) cannot work without these. | `cyberbrowser/src/browser/css_api.cpp:48-170`; `cyberbrowser/src/browser_api_impl.cpp:2591-2598` |
| 16 | `document.implementation.createHTMLDocument()` | Returns a minimal inert object with no real DOM methods. | Used for safe style parsing/diffing in mutation observers (line 10025). | `cyberbrowser/src/browser_api_impl.cpp:1980-1993` |
| 17 | `Element.prototype.outerHTML` getter/setter | Getter returns `""`; setter is a no-op. | Polymer and sanitizers occasionally read/write `outerHTML`. | `cyberbrowser/src/browser/dom_api.cpp:2048-2062` |
| 18 | `Document.prototype.createComment()` | Returns an empty string instead of a Comment node. | Minor; can break DOM scanners that expect comment nodes. | `cyberbrowser/src/browser_api_impl.cpp:1958` |
| 19 | `requestIdleCallback()` / `requestAnimationFrame()` | `rIC` fires immediately with a mock `IdleDeadline`; `rAF` is scheduled as a 0-delay timer. | Not faithful to browser scheduling, but usually does not block stamping outright. | `cyberbrowser/src/browser/timer_api.cpp:512-559` |

---

## Observed runtime symptom

After executing all 48 page scripts:

- `[CE-UPGRADE] upgradeAll <element> found=0` for almost every custom element, including `ytd-masthead`.
- Only `ytd-app` is found (`found=1`), yet its shadow root remains empty.
- The rendered screenshot still shows the light-DOM skeleton masthead (no logo, no search box), confirming that Polymer's `connectedCallback` / `_attachDom` / `_stampTemplate` path never ran for the real elements.
- Visual diff MAE stays ~0.098 vs. Chrome.

---

## Recommended order of attack

1. Implement a real `DocumentFragment` and wire `document.createDocumentFragment()` to it.
2. Implement deep `document.importNode()` (clone the full DOM subtree).
3. Add `Element.prototype.attributes` (NamedNodeMap) with `length`, indexed access, `.name`, `.value`, `.getNamedItem`, plus `hasAttributes()` / `getAttributeNames()`.
4. Make `Document.getElementsByTagName()` HTML-case-insensitive (lower-case query atom before index lookup).
5. Fix custom-element upgrade so that parser-created and manually-appended elements are upgraded when definitions arrive, using the existing element as the constructor's `this` (spec construct-a-custom-element semantics).
6. Add/fix `Element.prototype.matches()` and `Element.prototype.closest()`.
7. Fix `Element.prototype.getAttribute('style')` to return the inline style string.
8. Implement a real `MutationObserver.observe()` (or at least childList/subtree delivery).
9. Fill in `HTMLSlotElement` and slot APIs (`assignedNodes`, `assignedElements`, `name`).
10. Harden `ShadowRoot` / `attachShadow` and support `getRootNode({composed:true})`.

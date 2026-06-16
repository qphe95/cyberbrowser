# Polymer/WebComponents Loading Analysis

## Key Insight

**You are correct!** Polymer, ShadyDOM, and ShadyCSS are NOT browser APIs - they are JavaScript LIBRARIES that are loaded from external scripts.

This means we have TWO options:
1. **Execute the actual scripts** that load Polymer (what happens in a real browser)
2. **Stub the Polymer API** to skip execution (faster but may miss initialization side effects)

---

## Script Loading Order

Based on analysis of `youtube_page.html`, here's the actual loading sequence:

### Phase 1: Configuration (Inline Scripts)
```
Script 0: WIZ_global_data setup
Script 1: ytcfg definition  
Script 2: Error handling setup
Script 3: Polymer/ShadyDOM Configuration ← FIRST POLYMER MENTION
```

**youtube_script_003_inline.js:**
```javascript
window.Polymer = window.Polymer || {};  // Creates empty Polymer object
window.Polymer.legacyOptimizations = true;
window.Polymer.setPassiveTouchGestures = true;
window.ShadyDOM = {                       // Creates initial ShadyDOM config
  force: true,
  preferPerformance: true,
  noPatch: true
};
window.polymerSkipLoadingFontRoboto = true;
window.ShadyCSS = {                       // Creates initial ShadyCSS config
  disableRuntime: true
};
```

### Phase 2: External Library Loading
```
Script: web-animations-next-lite.min.js
Script: custom-elements-es5-adapter.js
Script: webcomponents-sd.js  ← LOADS FULL SHADYDOM/SHADYCSS
Script: intersection-observer.min.js
```

**youtube_script_008_external.js** (webcomponents-sd.js) does:
```javascript
// Line 2606: Overwrites window.ShadyDOM with full implementation
window.ShadyDOM = {
  inUse: w.inUse,
  patch: ed,
  isShadyRoot: y,
  enqueue: Ea,
  flush: Fa,
  // ... many more methods
  nativeMethods: Ta,
  nativeTree: Ua,
  patchElementProto: Pc,
  querySelectorImplementation: w.querySelectorImplementation
};

// Line 3745: Overwrites window.ShadyCSS with full implementation  
window.ShadyCSS = {
  ScopingShim: Z,
  prepareTemplate: function(a, b, c) {...},
  prepareTemplateDom: function(a, b) {...},
  prepareTemplateStyles: function(a, b, c) {...},
  // ...
};

// Line 2641-2644: Overrides native Event classes
window.Event = Hb;
window.CustomEvent = Ib;
window.MouseEvent = Jb;
window.ShadowRoot = yc;
```

### Phase 3: Main Application Bundle
```
Script 037: /s/_/ytmainappweb/_/js/k=ytmainappweb.kevlar_base...  ← LOADS POLYMER
```

**youtube_script_037_external.js** (line 64417):
```javascript
// Defines Polymer as a function
var XNi = window.Polymer;
window.Polymer = function(G) {
  return window.Polymer._polymerFn(G)
};
XNi && Object.assign(Polymer, XNi);
Polymer._polymerFn = function() {
  throw Error("Sd");  // Will be overridden later
};
```

---

## What Polymer/ShadyDOM Actually Do

### ShadyDOM
Shadow DOM polyfill - provides scoped DOM trees:
```javascript
// Creates shadow root
const shadow = element.attachShadow({mode: 'open'});

// Patches native methods
window.ShadyDOM.patch(element);  // Wraps element for Shadow DOM

// Event handling
window.ShadyDOM.wrap(element).addEventListener(...);
```

### ShadyCSS
CSS scoping for Shadow DOM:
```javascript
// Prepares template styles
window.ShadyCSS.prepareTemplate(template, elementName);

// Applies scoped styles
window.ShadyCSS.styleElement(element);
```

### Polymer
Web Components framework:
```javascript
// Defines custom element
Polymer({
  is: 'my-element',
  properties: {...},
  ready: function() {...}
});

// Or with class syntax
class MyElement extends Polymer.Element {...}
```

---

## Do We Need Polymer for URL Extraction?

### Analysis: NO - For URL Extraction Only

The video player (`youtube_script_024_external.js` - base.js) does NOT use Polymer:
```javascript
// Player uses native video element:
const video = document.createElement('video');
video.src = url;  // <-- This is what we hook

// Or MediaSource:
const ms = new MediaSource();
video.src = URL.createObjectURL(ms);
```

Polymer is used for the **UI** (buttons, menus, etc.), not the **player core**.

### However: Script Execution May Fail

If we execute all scripts in order:
1. Script 003 sets up Polymer config ✓
2. Script 008 loads full ShadyDOM ✓  
3. Script 037 tries to use Polymer → May fail if ShadyDOM errors

**The risk:** ShadyDOM patches native DOM methods. If our DOM stubs aren't compatible, ShadyDOM may crash, preventing script 024 (the player) from running.

---

## Two Approaches

### Approach 1: Minimal Stubs (Skip Polymer Execution)

Provide just enough for scripts not to crash, but skip Polymer-heavy scripts:

```javascript
// Pre-define Polymer/ShadyDOM as minimal stubs
window.Polymer = function() {};  // No-op
window.ShadyDOM = { 
  inUse: false,
  patch: function(e) { return e; },
  wrap: function(e) { return e; }
};
window.ShadyCSS = {
  prepareTemplate: function() {},
  styleElement: function() {}
};
```

**Pros:**
- Faster execution
- No DOM patching complexity
- Can skip large scripts (008, 037)

**Cons:**
- May miss initialization side effects
- Harder to maintain

### Approach 2: Full Execution (Let Scripts Load)

Execute scripts in order, including webcomponents-sd.js:

**Required for this approach:**
```javascript
// Our DOM stubs must be compatible with ShadyDOM patches:

// ShadyDOM patches these methods:
Element.prototype.attachShadow
Element.prototype.shadowRoot (getter)
document.createElement
document.querySelector/querySelectorAll
addEventListener/removeEventListener

// ShadyDOM expects these to exist:
window.Event, window.CustomEvent, window.MouseEvent
window.ShadowRoot
window.HTMLElement, window.Element, window.Node
```

**Pros:**
- Most accurate to real browser
- Captures all side effects
- Future-proof

**Cons:**
- More complex DOM stubs needed
- Slower (executes more code)
- May encounter edge cases

---

## Recommendation: Hybrid Approach

### Phase 1: URL Extraction (Current Goal)
Use **Approach 1** - skip Polymer-heavy scripts:

**Skip these scripts (UI only):**
- script_008_external.js (webcomponents-sd.js - 110KB of ShadyDOM)
- script_037_external.js (main app - huge Polymer app)
- Any script with heavy DOM manipulation

**Execute these scripts (player-related):**
- script_024_external.js (base.js - THE PLAYER)
- script_026_inline.js (ytplayer bootstrap)
- script_011_inline.js (player config)

**Provide minimal stubs:**
```javascript
// Before executing any scripts:
window.Polymer = function() { return {}; };
window.Polymer.legacyOptimizations = true;
window.ShadyDOM = { inUse: false, wrap: function(e) { return e; } };
window.ShadyCSS = { disableRuntime: true };
```

### Phase 2: Full Emulation (Future)
If needed, implement full ShadyDOM-compatible DOM stubs.

---

## Critical APIs for ShadyDOM Compatibility

If you DO want to execute script_008 (webcomponents-sd.js), you need:

### Must Have
```javascript
// DOM Core (already in browser_api_impl.cpp mostly)
window.Event
document.createElement
Element.prototype
Node.prototype
HTMLElement.prototype

// Shadow DOM
Element.prototype.attachShadow
window.ShadowRoot

// EventTarget (patched by ShadyDOM)
EventTarget.prototype.addEventListener
EventTarget.prototype.removeEventListener
EventTarget.prototype.dispatchEvent
```

### May Need
```javascript
// Tree traversal (patched by ShadyDOM)
Node.prototype.childNodes
Node.prototype.parentNode
Node.prototype.firstChild, lastChild
Node.prototype.nextSibling, previousSibling
Element.prototype.children

// DOM manipulation (patched by ShadyDOM)
Node.prototype.appendChild
Node.prototype.removeChild
Node.prototype.insertBefore
Node.prototype.replaceChild
Element.prototype.innerHTML
Element.prototype.textContent
```

---

## Current Status

Looking at `browser_api_impl.cpp`:

### ✅ Already Compatible
```cpp
// EventTarget with add/removeEventListener
// Element with attachShadow
// ShadowRoot class
// Node with appendChild, removeChild, etc.
// Document with createElement, querySelector
```

### ⚠️ May Need Enhancement
The stubs may need to match ShadyDOM's expectations exactly. For example, ShadyDOM does:
```javascript
const nativeCreateElement = document.createElement;
document.createElement = function(tag) {
  const el = nativeCreateElement.call(document, tag);
  // Wrap for shadow DOM...
  return el;
};
```

If our `document.createElement` isn't a proper function that can be `.call()`-ed, ShadyDOM will fail.

---

## Test Strategy

1. **First try:** Execute scripts WITHOUT script_008 and script_037
   - If player (script_024) loads and sets video.src → SUCCESS

2. **If that fails:** Add minimal Polymer stubs
   - Define `window.Polymer = function() {}`
   - Define minimal `window.ShadyDOM`

3. **If still failing:** Try executing script_008 with enhanced DOM stubs
   - Make sure all DOM methods are proper functions
   - Ensure prototype chain is correct

---

## Summary

| Question | Answer |
|----------|--------|
| Is Polymer a browser API? | **NO** - It's a library loaded from scripts |
| Do we need Polymer for URL extraction? | **NO** - Player doesn't use it |
| Should we execute Polymer scripts? | **NO** (for Phase 1) - Skip them for speed |
| What if scripts fail without Polymer? | Provide minimal stubs |
| What's the risk? | ShadyDOM patches DOM - our stubs must be compatible |

**Bottom line:** For URL extraction, skip the Polymer-heavy scripts (008, 037) and focus on the player script (024). The player works with native video APIs, not Polymer.

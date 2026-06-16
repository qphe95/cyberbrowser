# UPDATED Browser APIs Required (Including ShadyDOM/Polymer Compatibility)

## Summary

From analyzing 51 YouTube scripts and ShadyDOM requirements:

| Category | Count | Notes |
|----------|-------|-------|
| YouTube Internal | 14 | Auto-created by scripts |
| JavaScript Built-ins | ~50 | Already in QuickJS |
| **Browser APIs to Stub** | **~120** | **IMPLEMENT THESE** |

**CRITICAL UPDATE:** ShadyDOM patches 40+ DOM methods. Our stubs must be compatible!

---

## 1. CRITICAL APIs for URL Extraction (Phase 1)

### 1.1 Video & URL Capture (Already Implemented)
```javascript
// ✅ Video element with hooked .src (js_quickjs.cpp:345-357)
HTMLVideoElement                   // Constructor
document.createElement('video')    // Returns video element with:
//   - .src getter/setter (setter captures URL!)
//   - .currentSrc, .currentTime, .duration
//   - .paused, .readyState, .networkState
//   - .load(), .play(), .pause()

// ✅ Network capture (browser_api_impl.cpp)
fetch()                            // Hooked to capture URLs
XMLHttpRequest                     // Constructor with URL capture in open()

// 🟡 URL API (NEEDS IMPLEMENTATION)
URL                                // Constructor
URL.createObjectURL()              // CRITICAL: for MediaSource blob URLs
URL.revokeObjectURL()              // Cleanup
```

### 1.2 MediaSource API (CRITICAL - NEEDED)
```javascript
MediaSource                        // Constructor
window.MediaSource                 // Global
MediaSource.isTypeSupported()      // Static method
ManagedMediaSource                 // iOS variant
WebKitMediaSource                  // Safari variant

// SourceBuffer (returned from mediaSource.addSourceBuffer())
SourceBuffer
sb.appendBuffer()                  // Receives video data
sb.remove()                        // Remove data
```

---

## 2. DOM APIs for ShadyDOM Compatibility (Phase 1.5)

ShadyDOM (in script_008) patches these methods. Our stubs must be **real functions** that can be `.call()`-ed and stored.

### 2.1 Node Tree Traversal (ALL MISSING!)
```javascript
// Parent/Child relationships
node.parentNode                    // Property (returns Node or null)
node.parentElement                 // Property (returns Element or null)
node.childNodes                    // Property (returns NodeList)
element.children                   // Property (returns HTMLCollection)

// Sibling navigation
node.firstChild                    // Property
node.lastChild                     // Property
node.nextSibling                   // Property
node.previousSibling               // Property

// Element-specific
node.firstElementChild             // Property (Element only)
node.lastElementChild              // Property (Element only)
node.nextElementSibling            // Property (Element only)
node.previousElementSibling        // Property (Element only)
```

### 2.2 DOM Manipulation (Some Missing)
```javascript
// ✅ Already in browser_api_impl.cpp:
node.appendChild(child)
node.removeChild(child)

// 🟡 MISSING - Need implementation:
node.insertBefore(newNode, refNode)
node.cloneNode(deep)
document.importNode(node, deep)
node.contains(otherNode)
```

### 2.3 Content Properties (ALL MISSING!)
```javascript
element.innerHTML                  // Getter/setter for HTML content
element.outerHTML                  // Getter/setter
node.textContent                   // Getter/setter for text
```

### 2.4 Attributes (Some Missing)
```javascript
// ✅ Already in browser_api_impl.cpp:
element.getAttribute(name)
element.setAttribute(name, value)
element.removeAttribute(name)

// 🟡 MISSING:
element.hasAttribute(name)         // Returns boolean
element.toggleAttribute(name)      // Add/remove attribute
```

### 2.5 Shadow DOM (ALL MISSING - Critical for ShadyDOM!)
```javascript
element.attachShadow(init)         // Creates ShadowRoot
// Returns: ShadowRoot

element.shadowRoot                 // Property - gets shadow root (readonly)
node.getRootNode(options)          // Gets root (document or shadow root)

// ShadowRoot interface
ShadowRoot                         // Constructor (internal)
shadowRoot.host                    // Property - parent element
shadowRoot.innerHTML               // Same as element
shadowRoot.querySelector           // Scoped query
shadowRoot.querySelectorAll        // Scoped query all
```

### 2.6 Document Methods (Some Missing)
```javascript
// ✅ Already in browser_api_impl.cpp:
document.createElement(tag)
document.createElementNS(ns, tag)
document.createTextNode(text)
document.querySelector(selector)
document.querySelectorAll(selector)

// 🟡 MISSING:
document.importNode(node, deep)
document.elementFromPoint(x, y)    // For hit testing
```

### 2.7 Focus Management (Missing)
```javascript
element.focus()                    // Focus element
element.blur()                     // Remove focus
document.activeElement             // Property - currently focused element
```

---

## 3. Event System (Mostly Implemented)

### 3.1 EventTarget (✅ Already in browser_api_impl.cpp)
```javascript
// EventTarget base class
EventTarget                        // Constructor
EventTarget.prototype.addEventListener(type, listener, options)
EventTarget.prototype.removeEventListener(type, listener, options)
EventTarget.prototype.dispatchEvent(event)

// ✅ Already set up for:
// - window (global)
// - document
// - Element, HTMLElement
// - HTMLVideoElement
```

### 3.2 Event Constructors (✅ Mostly done)
```javascript
Event                              // Constructor
new Event(type, init)              // Create event
CustomEvent                        // Constructor
new CustomEvent(type, init)        // Create custom event
MouseEvent                         // Constructor
new MouseEvent(type, init)         // Create mouse event
```

### 3.3 Event Properties (Some Missing)
```javascript
event.target                       // ✅ Already
event.currentTarget                // ✅ Already
event.type                         // ✅ Already
event.eventPhase                   // 🟡 MISSING (1=capture, 2=target, 3=bubble)
event.composed                     // For shadow DOM events
event.composedPath()               // Event path through shadow DOM
```

---

## 4. Timing & Console (✅ Already Implemented)

```javascript
// ✅ Timing (browser_api_impl.cpp:2804-2807)
setTimeout(fn, delay)
clearTimeout(id)
setInterval(fn, delay)
clearInterval(id)
requestAnimationFrame(fn)
cancelAnimationFrame(id)

// ✅ Console (browser_api_impl.cpp:3037-3045)
console.log()
console.warn()
console.error()
console.info()
console.debug()
console.trace()
console.groupCollapsed()
console.groupEnd()
console.time()
console.timeEnd()
```

---

## 5. Window, Location, Navigator (Mostly Implemented)

### 5.1 Window (✅ Already)
```javascript
window                             // Global
window.document                    // Document reference
window.location                    // Location object
window.navigator                   // Navigator object
window.console                     // Console object
window.customElements              // ✅ Already
window.Polymer                     // 🟡 Should stub as function(){}
window.ShadyDOM                    // 🟡 Should stub
window.ShadyCSS                    // 🟡 Should stub
```

### 5.2 Location (✅ Already in browser_api_impl.cpp:2969-2980)
```javascript
location.href                      // Full URL
location.protocol                  // "https:"
location.host                      // "www.youtube.com"
location.hostname                  // "www.youtube.com"
location.port                      // ""
location.pathname                  // "/watch"
location.search                    // "?v=..."
location.hash                      // "#..."
location.origin                    // "https://www.youtube.com"
location.reload()                  // Reload page
```

### 5.3 Navigator (✅ Already in browser_api_impl.cpp:2985-3002)
```javascript
navigator.userAgent                // Browser string
navigator.language                 // "en-US"
navigator.cookieEnabled            // true
navigator.onLine                   // true
navigator.hardwareConcurrency      // 8
navigator.maxTouchPoints           // 0
navigator.vendor                   // "Google Inc."
navigator.sendBeacon(url, data)    // 🟡 Needs implementation
```

---

## 6. Custom Elements (✅ Already Implemented)

```javascript
customElements                     // Registry object
window.customElements
customElements.define(name, ctor)  // Register element
window.customElements.define
customElements.get(name)           // Get element class
window.customElements.get
customElements.whenDefined(name)   // Returns Promise
window.customElements.whenDefined
```

---

## 7. Storage & Crypto (Missing)

```javascript
// 🟡 localStorage
localStorage
localStorage.getItem(key)          // Returns string or null
localStorage.setItem(key, value)
localStorage.removeItem(key)

// 🟡 sessionStorage
sessionStorage
sessionStorage.getItem(key)
sessionStorage.setItem(key, value)

// 🟡 Crypto (needed for botguard)
crypto
window.crypto
crypto.getRandomValues(typedArray) // Fill with random
```

---

## 8. Performance (Partial)

```javascript
// 🟡 Basic
performance
window.performance
performance.now()                  // High-res timestamp

// 🟡 Timing info
performance.timing                 // NavigationTiming
performance.timing.navigationStart // Page load start

// 🟡 Other (lower priority)
performance.mark(name)
performance.measure(name)
performance.getEntriesByName(name)
performance.clearResourceTimings()
```

---

## 9. Misc APIs (Lower Priority)

```javascript
// 🟡 AbortController
AbortController
window.AbortController
controller.abort()
controller.signal

// 🟡 matchMedia
matchMedia(query)                  // Media query matcher
window.matchMedia

// 🟡 FormData
FormData                           // Form data constructor
window.FormData
formData.append(name, value)

// 🟡 Observers
MutationObserver                   // DOM change observer
window.MutationObserver
IntersectionObserver               // Visibility observer
window.IntersectionObserver
ResizeObserver                     // Size observer
window.ResizeObserver
```

---

## 10. Implementation Priority (Revised)

### 🔴 TIER 1: Critical for URL Extraction (Do First)
1. ✅ Video element `.src` hook - **DONE**
2. 🔴 `URL.createObjectURL()` - **CRITICAL**
3. 🔴 `MediaSource` API - **CRITICAL**
4. 🔴 Enhanced `XMLHttpRequest.open()` - **CRITICAL**
5. 🔴 Enhanced `fetch()` - **IMPORTANT**

### 🟠 TIER 2: DOM Compatibility (For ShadyDOM)
**If we execute script_008 (webcomponents-sd.js), we need:**

**Tree Navigation:**
6. 🔴 `node.parentNode`, `node.parentElement`
7. 🔴 `node.childNodes` (NodeList)
8. 🔴 `element.children` (HTMLCollection)
9. 🔴 `node.firstChild`, `node.lastChild`
10. 🔴 `node.nextSibling`, `node.previousSibling`
11. 🔴 `node.firstElementChild`, etc.

**DOM Manipulation:**
12. 🔴 `node.insertBefore()`
13. 🔴 `node.cloneNode()`
14. 🔴 `document.importNode()`
15. 🔴 `node.contains()`

**Content:**
16. 🔴 `element.innerHTML` (getter/setter)
17. 🔴 `node.textContent` (getter/setter)

**Attributes:**
18. 🔴 `element.hasAttribute()`
19. 🔴 `element.toggleAttribute()`

**Shadow DOM:**
20. 🔴 `element.attachShadow()`
21. 🔴 `element.shadowRoot` (getter)
22. 🔴 `node.getRootNode()`
23. 🔴 `ShadowRoot` interface

### 🟡 TIER 3: Important
24. 🔴 Storage (`localStorage`, `sessionStorage`)
25. 🔴 Crypto (`crypto.getRandomValues`)
26. 🔴 Focus (`element.focus()`, `blur()`, `document.activeElement`)
27. 🔴 `document.elementFromPoint()`

### 🟢 TIER 4: Nice to Have
28. 🔴 `AbortController`
29. 🔴 `matchMedia`
30. 🔴 `FormData`
31. 🔴 `performance.*`

---

## 11. Recommendation

### Option A: Skip ShadyDOM Scripts (Recommended for Phase 1)
1. Don't execute `script_008_external.js` (110KB ShadyDOM)
2. Don't execute `script_037_external.js` (main Polymer app)
3. **Stub these instead:**
   ```javascript
   window.ShadyDOM = { inUse: false, wrap: function(e) { return e; } };
   window.ShadyCSS = { disableRuntime: true };
   window.Polymer = function() { return {}; };
   ```
4. Execute `script_024_external.js` (player) directly

**Pros:** Much simpler, faster  
**Cons:** May miss initialization side effects

### Option B: Full ShadyDOM Support
Implement ALL DOM methods listed in TIER 2 above.

**Pros:** Most accurate  
**Cons:** Significant work (~40 DOM methods)

---

## Summary of MISSING Critical APIs

| API | Where Used | Priority |
|-----|------------|----------|
| `URL.createObjectURL` | MediaSource blob URLs | 🔴 CRITICAL |
| `MediaSource` | DASH streaming | 🔴 CRITICAL |
| `node.parentNode` | ShadyDOM tree traversal | 🔴 If using ShadyDOM |
| `node.childNodes` | ShadyDOM tree traversal | 🔴 If using ShadyDOM |
| `element.innerHTML` | Content setting | 🔴 If using ShadyDOM |
| `element.attachShadow` | Shadow DOM | 🔴 If using ShadyDOM |
| `crypto.getRandomValues` | BotGuard | 🟡 Important |
| `localStorage` | State persistence | 🟡 Important |

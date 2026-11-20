/*
 * HTMLElement basics that pages call constantly and that throw when absent
 * (YouTube's watch page: `el.focus()` in focusOnPlayer, `el.innerText.length`
 * in its snippet checks; an exception in a Polymer observer skips the
 * observers after it, so a missing API can leave unrelated data unbound).
 *
 * Written in script over the native DOM: each member is defined only where
 * Element.prototype (or its chain) has none, so native implementations win.
 *
 * - innerText / outerText: the rendered text, approximated from the tree
 *   (script/style/template/head content skipped, hidden elements skipped,
 *   <br> as a line break, block-level elements on their own lines); the
 *   setter replaces the children with text, line breaks as <br>.
 * - focus() / blur(): document.activeElement and focus/blur (not bubbling)
 *   and focusin/focusout (bubbling) events.
 * - Reflected attributes per HTML: hidden, title, lang, dir, accessKey,
 *   draggable, translate, inert, tabIndex, contentEditable.
 * - insertAdjacentHTML / insertAdjacentElement / insertAdjacentText.
 * - checkVisibility(): false without a box or when visibility is hidden.
 */
#include "html_element_api.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "dom_js.h"
#include "display_list.h"
#include "canvas2d.h"
#include <math.h>
void dom_request_layout(void);
#include "css_style.h"

static const char k_html_element_js[] =
"(function(P){\n"
"  function def(name, desc) {\n"
"    if (name in P) return;\n"
"    desc.configurable = true; desc.enumerable = true;\n"
"    Object.defineProperty(P, name, desc);\n"
"  }\n"
"  function method(name, fn) {\n"
"    if (typeof P[name] === 'function') return;\n"
"    Object.defineProperty(P, name, {configurable:true, writable:true, value:fn});\n"
"  }\n"
"  var renderedText = __cyberInnerText;   /* (inner_text below) */\n"
"  function setText(el, v) {\n"
"    var s = v == null ? '' : String(v);\n"
"    while (el.firstChild) el.removeChild(el.firstChild);\n"
"    var parts = s.split(/\\r\\n|\\r|\\n/);\n"
"    for (var i = 0; i < parts.length; i++) {\n"
"      if (i) el.appendChild(el.ownerDocument.createElement('br'));\n"
"      if (parts[i]) el.appendChild(el.ownerDocument.createTextNode(parts[i]));\n"
"    }\n"
"  }\n"
"  def('innerText', {get: function(){ return renderedText(this); }, set: function(v){ setText(this, v); }});\n"
"  def('outerText', {get: function(){ return renderedText(this); },\n"
"    set: function(v){ var p = this.parentNode; if (!p) throw new DOMException('No parent', 'NoModificationAllowedError');\n"
"      var t = this.ownerDocument.createTextNode(v == null ? '' : String(v)); p.replaceChild(t, this); }});\n"
"\n"
"  function fire(target, type, bubbles, related) {\n"
"    var ev;\n"
"    try { ev = typeof FocusEvent === 'function' ? new FocusEvent(type, {bubbles: bubbles, composed: true, relatedTarget: related || null})\n"
"                                               : new Event(type, {bubbles: bubbles, composed: true}); }\n"
"    catch (e) { ev = new Event(type, {bubbles: bubbles}); }\n"
"    target.dispatchEvent(ev);\n"
"  }\n"
"  var ST_FOCUS = 4, ST_VISIBLE = 8, ST_WITHIN = 16;\n"
"  function focusState(el, on, visible) {\n"
"    __cyberElState(el, ST_FOCUS, on);\n"
"    __cyberElState(el, ST_VISIBLE, !!(on && visible));\n"
"    for (var a = el; a; a = a.assignedSlot || a.parentNode || a.host) if (a.nodeType === 1) __cyberElState(a, ST_WITHIN, on);\n"
"  }\n"
"  function takesText(el) {\n"
"    var t = el.localName;\n"
"    if (el.isContentEditable || t === 'textarea') return true;\n"
"    if (t !== 'input') return false;\n"
"    return !/^(hidden|range|color|checkbox|radio|file|submit|image|reset|button)$/i.test(el.getAttribute('type') || '');\n"
"  }\n"
"  /* (Selectors 4 :focus-visible heuristics) */\n"
"  function focusVisible(el, prev) {\n"
"    if (takesText(el)) return true;\n"
"    if (window.__cyberFocusByMouse) return false;\n"
"    if (window.__cyberLastInput === 'key') return true;\n"
"    if (prev && prev.nodeType === 1 && prev.localName !== 'body' && prev !== el.ownerDocument.documentElement) return __cyberElState(prev, ST_VISIBLE);\n"
"    return window.__cyberLastInput !== 'mouse';\n"
"  }\n"
"  /* (HTML 6.6.2: a focusable area: a control, a link, an editing host, one with a tabindex, ... that is rendered and not inert) */\n"
"  var HTMLNS = 'http://www.w3.org/1999/xhtml', SVGNS = 'http://www.w3.org/2000/svg';\n"
"  function focusableArea(el) {\n"
"    if (!el || el.nodeType !== 1) return false;\n"
"    var ln = el.localName, ns = el.namespaceURI, ti = el.getAttribute('tabindex');\n"
"    var tab = ti !== null && /^\\s*[-+]?\\d+/.test(ti);\n"
"    var own = tab;\n"
"    if (!own && ns === HTMLNS) {\n"
"      if (ln === 'a' || ln === 'area') own = el.hasAttribute('href');\n"
"      else if (ln === 'input') own = String(el.getAttribute('type') || '').toLowerCase() !== 'hidden';\n"
"      else if (/^(button|select|textarea|iframe|embed|object)$/.test(ln)) own = true;\n"
"      else if (ln === 'audio' || ln === 'video') own = el.hasAttribute('controls');\n"
"      else if (ln === 'summary') { var dp = el.parentElement; own = !!dp && dp.localName === 'details' && dp.querySelector(':scope > summary') === el; }\n"
"      else if (ln === 'option') { var os = el.closest('select'); own = !!os && os.hasAttribute('data-cyber-open'); }\n"
"      else if (ln === 'menuitem') own = !el.hasAttribute('disabled');\n"
"      if (!own && el.isContentEditable && (!el.parentElement || !el.parentElement.isContentEditable)) own = true;\n"
"    } else if (!own && ns === SVGNS && ln === 'a') own = el.hasAttribute('href') || el.hasAttributeNS('http://www.w3.org/1999/xlink', 'href');\n"
"    if (!own) return false;\n"
"    try { if (el.matches(':disabled')) return false; } catch (e) {}\n"
"    if (el.closest && el.closest('[inert]')) return false;\n"
"    /* (blocked by a modal dialog: outside the topmost one, and not in a popover shown over it) */\n"
"    try {\n"
"      var md = el.ownerDocument && el.ownerDocument.querySelectorAll(':modal');\n"
"      if (md && md.length) {\n"
"        var mo = window.__cyberModalOrder || [], top = null, inTop = false;\n"
"        for (var mi = mo.length - 1; mi >= 0 && !top; mi--) if (mo[mi].isConnected && mo[mi].matches(':modal')) top = mo[mi];\n"
"        if (!top) top = md[md.length - 1];\n"
"        for (var fp = el; fp && !inTop; fp = fp.assignedSlot || fp.parentNode || (fp.host || null)) { if (fp === top) inTop = true; if (fp.nodeType === 1 && fp !== top && fp.matches && fp.matches(':popover-open')) inTop = true; }\n"
"        if (!inTop) return false;\n"
"      }\n"
"    } catch (e) {}\n"
"    if (ln === 'area') return true;\n"
"    /* (a canvas's fallback content: focusable with no box, while the canvas is rendered and nothing on the way down is display: none) */\n"
"    for (var cv = el.parentElement; cv; cv = cv.parentElement) if (cv.localName === 'canvas' && cv.namespaceURI === HTMLNS) {\n"
"      if (!cv.getClientRects().length) return false;\n"
"      try { for (var up = el; up && up !== cv; up = up.parentElement) { if (up.hasAttribute('hidden') || (up.style && up.style.display === 'none')) return false; } } catch (e) {}\n"
"      return true;\n"
"    }\n"
"    try {\n"
"      var cs = getComputedStyle(el);\n"
"      if (cs.visibility === 'hidden' || cs.visibility === 'collapse') return false;\n"
"      /* (the popover or dialog itself just shown: its style and boxes may be from before, during an event's dispatch) */\n"
"      var justShown = el.matches && (el.matches(':popover-open, dialog[open]') || (cs.display !== 'none' && !!el.closest(':popover-open, dialog[open]')));\n"
"      /* (an SVG element inside an <svg>: rendered when its outermost <svg> is, its own geometry not boxes) */\n"
"      var boxEl = el;\n"
"      if (ns === SVGNS && el.ownerSVGElement) { if (cs.display === 'none') return false; while (boxEl.ownerSVGElement) boxEl = boxEl.ownerSVGElement; }\n"
"      if (cs.display !== 'contents' && !boxEl.getClientRects().length && !justShown) return false;\n"
"    } catch (e) {}\n"
"    return true;\n"
"  }\n"
"  /* (a shadow host that delegates focus: its first focusable area, flat tree order) */\n"
"  function delegateOf(el) {\n"
"    var sr = el.shadowRoot;\n"
"    if (!sr || !sr.delegatesFocus) return null;\n"
"    var stack = Array.prototype.slice.call(sr.childNodes).reverse();\n"
"    while (stack.length) {\n"
"      var n = stack.pop();\n"
"      if (n.nodeType !== 1) continue;\n"
"      if (focusableArea(n)) return n;\n"
"      var d = n.shadowRoot && n.shadowRoot.delegatesFocus ? delegateOf(n) : null;\n"
"      if (d) return d;\n"
"      var ch = Array.prototype.slice.call(n.childNodes);\n"
"      for (var i = ch.length - 1; i >= 0; i--) stack.push(ch[i]);\n"
"    }\n"
"    return null;\n"
"  }\n"
"  Object.defineProperty(window, '__cyberFocusableArea', { configurable: true, value: focusableArea });\n"
"  method('focus', function(options){\n"
"    var doc = this.ownerDocument || document;\n"
"    if (!this.isConnected) return;\n"
"    var self = this;\n"
"    /* (not a focusable area: a host's delegate, else nothing; a dialog: its own focusing steps) */\n"
"    if (!focusableArea(self) && !(self.localName === 'dialog' && self.namespaceURI === HTMLNS)) {\n"
"      var dl = delegateOf(self);\n"
"      if (!dl) return;\n"
"      self = dl;\n"
"    }\n"
"    var prev = doc.activeElement;\n"
"    if (prev === self) return;\n"
"    var visible = focusVisible(self, prev);\n"
"    if (prev && prev !== doc.body && prev.dispatchEvent) { focusState(prev, false); fire(prev, 'blur', false, self); fire(prev, 'focusout', true, self); }\n"
"    if (window.__cyberDocSet) __cyberDocSet(doc, 'activeElement', self); else doc.activeElement = self;\n"
"    window.__cyberFocusGen = (window.__cyberFocusGen | 0) + 1;\n"
"    focusState(self, true, visible);\n"
"    var boxless = false; try { boxless = getComputedStyle(self).display === 'contents'; } catch (e) {}   /* (no box: nothing to scroll to) */\n"
"    if (!(options && options.preventScroll) && !boxless && self.scrollIntoViewIfNeeded) self.scrollIntoViewIfNeeded();\n"
"    fire(self, 'focus', false, prev); fire(self, 'focusin', true, prev);\n"
"  });\n"
"  method('blur', function(){\n"
"    var doc = this.ownerDocument || document;\n"
"    if (doc.activeElement !== this) return;\n"
"    if (window.__cyberDocSet) __cyberDocSet(doc, 'activeElement', doc.body); else doc.activeElement = doc.body;\n"
"    window.__cyberFocusGen = (window.__cyberFocusGen | 0) + 1;\n"
"    focusState(this, false);\n"
"    fire(this, 'blur', false, null); fire(this, 'focusout', true, null);\n"
"  });\n"
"\n"
"  function strAttr(name, attr) {\n"
"    def(name, {get: function(){ var v = this.getAttribute(attr); return v == null ? '' : v; },\n"
"               set: function(v){ this.setAttribute(attr, String(v)); }});\n"
"  }\n"
"  function boolAttr(name, attr) {\n"
"    def(name, {get: function(){ return this.hasAttribute(attr); },\n"
"               set: function(v){ if (v) this.setAttribute(attr, ''); else this.removeAttribute(attr); }});\n"
"  }\n"
"  strAttr('title', 'title'); strAttr('lang', 'lang'); strAttr('accessKey', 'accesskey');\n"
"  boolAttr('hidden', 'hidden'); boolAttr('inert', 'inert'); boolAttr('headingReset', 'headingreset');\n"
"  def('headingOffset', {get: function(){ var v = parseInt(this.getAttribute('headingoffset'), 10); return isNaN(v) || v < 0 ? 0 : v; },\n"
"              set: function(v){ this.setAttribute('headingoffset', String(v >>> 0)); }});\n"
"  def('dir', {get: function(){ var v = String(this.getAttribute('dir') || '').toLowerCase();\n"
"                return v === 'ltr' || v === 'rtl' || v === 'auto' ? v : ''; },\n"
"              set: function(v){ this.setAttribute('dir', String(v)); }});\n"
"  def('draggable', {get: function(){ var v = this.getAttribute('draggable');\n"
"                if (v === 'true') return true; if (v === 'false') return false;\n"
"                var t = String(this.tagName || '').toUpperCase();\n"
"                return t === 'IMG' || (t === 'A' && this.hasAttribute('href')); },\n"
"              set: function(v){ this.setAttribute('draggable', v ? 'true' : 'false'); }});\n"
"  def('translate', {get: function(){ for (var e = this; e && e.nodeType === 1; e = e.parentNode) {\n"
"                var v = e.getAttribute('translate'); if (v === 'no') return false; if (v === 'yes' || v === '') return true; }\n"
"                return true; },\n"
"              set: function(v){ this.setAttribute('translate', v ? 'yes' : 'no'); }});\n"
"  var FOCUSABLE = {A:1, BUTTON:1, INPUT:1, SELECT:1, TEXTAREA:1, IFRAME:1, SUMMARY:1};\n"
"  def('tabIndex', {get: function(){ var v = parseInt(this.getAttribute('tabindex'), 10);\n"
"                if (!isNaN(v) && v >= -2147483648 && v <= 2147483647) return v === 0 ? 0 : v;\n"
"                var t = String(this.tagName || '').toUpperCase();\n"
"                return FOCUSABLE[t] && (t !== 'A' || this.hasAttribute('href')) ? 0 : -1; },\n"
"              set: function(v){ this.setAttribute('tabindex', String(v | 0)); }});\n"
"  def('contentEditable', {get: function(){ var v = this.getAttribute('contenteditable');\n"
"                if (v == null) return 'inherit'; v = v.toLowerCase();\n"
"                return v === '' || v === 'true' ? 'true' : v === 'false' ? 'false' : v === 'plaintext-only' ? v : 'inherit'; },\n"
"              set: function(v){ v = String(v).toLowerCase(); if (v === 'inherit') this.removeAttribute('contenteditable'); else this.setAttribute('contenteditable', v); }});\n"
"  def('isContentEditable', {get: function(){ for (var e = this; e && e.nodeType === 1; e = e.parentNode) {\n"
"                var v = e.getAttribute('contenteditable'); if (v == null) continue; v = v.toLowerCase();\n"
"                return v === '' || v === 'true' || v === 'plaintext-only'; }\n"
"                return false; }});\n"
"\n"
"  function adjacent(el, where, node) {\n"
"    switch (String(where).toLowerCase()) {\n"
"      case 'beforebegin': if (!el.parentNode) return null; el.parentNode.insertBefore(node, el); return node;\n"
"      case 'afterbegin': el.insertBefore(node, el.firstChild); return node;\n"
"      case 'beforeend': el.appendChild(node); return node;\n"
"      case 'afterend': if (!el.parentNode) return null; el.parentNode.insertBefore(node, el.nextSibling); return node;\n"
"    }\n"
"    throw new DOMException('Invalid position ' + where, 'SyntaxError');\n"
"  }\n"
"  function need2(name, n){ if (n < 2) throw new TypeError(\"Failed to execute '\" + name + \"' on 'Element': 2 arguments required.\"); }\n"
"  method('insertAdjacentElement', function insertAdjacentElement(where, el){\n"
"    need2('insertAdjacentElement', arguments.length);\n"
"    if (!(el instanceof Element)) throw new TypeError(\"Failed to execute 'insertAdjacentElement' on 'Element': parameter 2 is not of type 'Element'.\");\n"
"    return adjacent(this, where, el); });\n"
"  method('insertAdjacentText', function insertAdjacentText(where, text){\n"
"    need2('insertAdjacentText', arguments.length);\n"
"    adjacent(this, where, this.ownerDocument.createTextNode(String(text))); });\n"
"  method('insertAdjacentHTML', function(where, html){\n"
"    var t = this.ownerDocument.createElement('template'); t.innerHTML = String(html);\n"
"    var src = t.content || t, frag = this.ownerDocument.createDocumentFragment();\n"
"    while (src.firstChild) frag.appendChild(src.firstChild);\n"
"    adjacent(this, where, frag);\n"
"  });\n"
"\n"
"  method('checkVisibility', function(opts){\n"
"    if (!(this instanceof Element)) throw new TypeError('Illegal invocation');\n"
"    opts = opts || {};\n"
"    if (!this.isConnected) return false;\n"
"    var op = opts.checkOpacity || opts.opacityProperty, vis = opts.checkVisibilityCSS || opts.visibilityProperty;\n"
"    var self = getComputedStyle(this);\n"
"    if (self.display === 'contents' || (vis && self.visibility !== 'visible')) return false;\n"
"    for (var e = this; e && e.nodeType === 1; ) {\n"
"      var cs = getComputedStyle(e);\n"
"      if (cs.display === 'none') return false;\n"
"      if (op && cs.opacity === '0') return false;\n"
"      if (e !== this) {\n"
"        if (cs.contentVisibility === 'hidden') return false;\n"
"        if (cs.contentVisibility === 'auto' && opts.contentVisibilityAuto) {\n"
"          var br = e.getBoundingClientRect();\n"
"          if (br.bottom < 0 || br.right < 0 || br.top > innerHeight || br.left > innerWidth) return false;\n"
"        }\n"
"      }\n"
"      var up = e.assignedSlot || e.parentNode;\n"
"      if (up && up.localName === 'details' && !up.open && e.localName !== 'summary') return false;\n"
"      if (up && up.nodeType === 11 && up.host) up = up.host;\n"
"      e = up;\n"
"    }\n"
"    return true;\n"
"  });\n"
"})";

/* HTMLLinkElement reflected attributes.  Without them `link.href = url`
 * only set a JS property, the element had no href attribute, and its sheet
 * was never loaded (YouTube's player waits for link#player-css's load). */
static const char k_link_js[] =
"(function(P){\n"
"  function def(name, desc) {\n"
"    if (Object.prototype.hasOwnProperty.call(P, name)) return;\n"
"    desc.configurable = true; desc.enumerable = true;\n"
"    Object.defineProperty(P, name, desc);\n"
"  }\n"
"  function strAttr(name, attr) {\n"
"    def(name, {get: function(){ var v = this.getAttribute(attr); return v == null ? '' : v; },\n"
"               set: function(v){ this.setAttribute(attr, String(v)); }});\n"
"  }\n"
"  strAttr('rel', 'rel'); strAttr('media', 'media'); strAttr('type', 'type'); strAttr('as', 'as');\n"
"  strAttr('integrity', 'integrity'); strAttr('hreflang', 'hreflang');\n"
"  def('crossOrigin', {get: function(){ var v = this.getAttribute('crossorigin'); return v == null ? null : (v.toLowerCase() === 'use-credentials' ? 'use-credentials' : 'anonymous'); },\n"
"                      set: function(v){ if (v == null) this.removeAttribute('crossorigin'); else this.setAttribute('crossorigin', String(v)); }});\n"
"  def('referrerPolicy', {get: function(){ return this.getAttribute('referrerpolicy') || ''; },\n"
"                         set: function(v){ this.setAttribute('referrerpolicy', String(v)); }});\n"
"  def('href', {get: function(){ var v = this.getAttribute('href'); if (v == null) return '';\n"
"                 try { return new URL(v, document.baseURI || location.href).href; } catch (e) { return v; } },\n"
"               set: function(v){ this.setAttribute('href', String(v)); }});\n"
"})";

/* HTMLScriptElement: src as a URL ('' for inline scripts; the WebGL suite
 * finds its files by it) and text.  Assigning src stays a plain property, as
 * it always was: setAttribute('src') would run the script at once. */
static const char k_script_js[] =
"(function(P){\n"
"  if (Object.prototype.hasOwnProperty.call(P, 'src')) return;\n"
"  Object.defineProperty(P, 'src', {configurable: true, enumerable: true,\n"
"    get: function(){ var v = this.getAttribute('src'); if (v == null) return '';\n"
"      try { return new URL(v, document.baseURI || location.href).href; } catch (e) { return v; } },\n"
"    set: function(v){ if (!this.isConnected) { this.setAttribute('src', String(v)); return; }\n"
"      Object.defineProperty(this, 'src', {value: v, writable: true, configurable: true, enumerable: true}); }});\n"
"  Object.defineProperty(P, 'text', {configurable: true, enumerable: true,\n"
"    get: function(){ return this.textContent; }, set: function(v){ this.textContent = v; }});\n"
"})";

static void run_installer(JSContextHandle ctx, const char *src, size_t len, const char *name, GCValue proto)
{
    GCValue fn = JS_Eval(ctx, src, len, name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsFunction(ctx, fn)) {
        GCValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, &proto);
        if (JS_IsException(r)) JS_GetException(ctx);
    } else if (JS_IsException(fn)) {
        JS_GetException(ctx);
    }
}

/* ---- innerText's getter, over the node store ----
 * The rendered text approximated from the tree: script/style/template/head
 * content skipped, elements hidden by the hidden attribute or an inline
 * display:none / visibility:hidden skipped, white space runs as one space,
 * <br> a line break, block-level elements on their own lines; then spaces
 * around line breaks, repeated and outer line breaks, and outer spaces go. */

typedef struct TextBuf { char *s; size_t n, cap; } TextBuf;

static void tb_put(TextBuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        while (b->n + n + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 256;
        b->s = realloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
}

static bool tag_in(const char *tag, const char *const *list) {
    for (int i = 0; list[i]; i++) if (!strcmp(tag, list[i])) return true;
    return false;
}

static const char *const k_skip[] = { "script", "style", "template", "noscript", "head", "title", "meta", "link", NULL };
static const char *const k_block[] = { "address", "article", "aside", "blockquote", "dd", "div", "dl", "dt", "fieldset",
    "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "li", "main", "nav",
    "ol", "p", "pre", "section", "table", "tr", "ul", NULL };

/* Does the style attribute say display:none or visibility:hidden? */
static bool inline_hidden(const char *style) {
    for (const char *p = style; p && *p; ) {
        while (*p == ' ' || *p == ';' || *p == '\t' || *p == '\n') p++;
        const char *name = p;
        while (*p && *p != ':' && *p != ';') p++;
        if (*p != ':') continue;
        size_t nl = (size_t)(p - name);
        while (nl && (name[nl - 1] == ' ' || name[nl - 1] == '\t')) nl--;
        const char *v = ++p;
        while (*p && *p != ';') p++;
        while (v < p && (*v == ' ' || *v == '\t')) v++;
        size_t vl = (size_t)(p - v);
        while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
        if ((nl == 7 && !strncasecmp(name, "display", 7) && vl >= 4 && !strncasecmp(v, "none", 4)) ||
            (nl == 10 && !strncasecmp(name, "visibility", 10) && vl >= 6 && !strncasecmp(v, "hidden", 6)))
            return true;
    }
    return false;
}

static void collect(uint32_t node, TextBuf *b) {
    for (uint32_t c = g_dom.first[node]; c; c = g_dom.next[c]) {
        if (g_dom.kind[c] == DOM_K_TEXT || g_dom.kind[c] == DOM_K_CDATA) {
            const char *t = g_dom.text[c].s;
            uint32_t n = g_dom.text[c].len;
            for (uint32_t i = 0; t && i < n; ) {
                if (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r') {
                    while (i < n && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++;
                    tb_put(b, " ", 1);
                } else {
                    uint32_t j = i;
                    while (j < n && !(t[j] == ' ' || t[j] == '\t' || t[j] == '\n' || t[j] == '\r')) j++;
                    tb_put(b, t + i, j - i);
                    i = j;
                }
            }
            continue;
        }
        if (g_dom.kind[c] != DOM_K_ELEMENT) continue;
        const char *tag = css_atom_str(g_dom.tag[c]);
        NodeId id = dom_id_of(c);
        if (!tag || tag_in(tag, k_skip) || dom_attr(id, "hidden") || inline_hidden(dom_attr(id, "style"))) continue;
        if (!strcmp(tag, "br")) { tb_put(b, "\n", 1); continue; }
        bool block = tag_in(tag, k_block);
        if (block) tb_put(b, "\n", 1);
        collect(c, b);
        if (block) tb_put(b, "\n", 1);
    }
}

/* textContent's concatenation (for a skipped root). */
static void all_text(uint32_t node, TextBuf *b) {
    for (uint32_t c = g_dom.first[node]; c; c = g_dom.next[c]) {
        if (g_dom.kind[c] == DOM_K_TEXT || g_dom.kind[c] == DOM_K_CDATA) tb_put(b, g_dom.text[c].s, g_dom.text[c].len);
        else if (g_dom.kind[c] == DOM_K_ELEMENT) all_text(c, b);
    }
}

static GCValue js_inner_text(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    NodeId id = argc && JS_IsObject(argv[0]) ? dom_node_of(argv[0]) : DOM_NULL;
    if (!id || !dom_valid(id)) return JS_NewString(ctx, "");
    uint32_t s = dom_slot(id);
    TextBuf b = { 0 };
    const char *tag = g_dom.kind[s] == DOM_K_ELEMENT ? css_atom_str(g_dom.tag[s]) : NULL;
    if (tag && tag_in(tag, k_skip)) {
        all_text(s, &b);
        GCValue v = JS_NewStringLen(ctx, b.s ? b.s : "", b.n);
        free(b.s);
        return v;
    }
    collect(s, &b);
    /* " *\n *" -> "\n", "\n\n+" -> "\n", then trim line breaks and spaces */
    size_t o = 0;
    for (size_t i = 0; i < b.n; i++) {
        char c = b.s[i];
        if (c == '\n') {
            while (o && b.s[o - 1] == ' ') o--;
            if (o && b.s[o - 1] == '\n') continue;
            b.s[o++] = '\n';
            while (i + 1 < b.n && b.s[i + 1] == ' ') i++;
            continue;
        }
        b.s[o++] = c;
    }
    size_t st = 0, en = o;
    while (st < en && b.s[st] == '\n') st++;
    while (en > st && b.s[en - 1] == '\n') en--;
    while (st < en && b.s[st] == ' ') st++;
    while (en > st && b.s[en - 1] == ' ') en--;
    GCValue v = JS_NewStringLen(ctx, b.s ? b.s + st : "", en - st);
    free(b.s);
    return v;
}

/* __cyberOptions(select): its list of options (HTML 4.10.7), the option
 * children and the option children of its optgroup children, in tree
 * order, as an array (form_controls.js). */
static GCValue js_options(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    NodeId id = argc && JS_IsObject(argv[0]) ? dom_node_of(argv[0]) : DOM_NULL;
    GCValue arr = JS_NewArray(ctx);
    if (!id || !dom_valid(id)) return arr;
    static CssAtom option, optgroup;
    if (!option) {
        css_atom_reserve(2);
        option = css_atom_intern("option", 6);
        optgroup = css_atom_intern("optgroup", 8);
    }
    static CssAtom hr_, select_, datalist_;
    if (!hr_) { css_atom_reserve(3); hr_ = css_atom_intern("hr", 2); select_ = css_atom_intern("select", 6); datalist_ = css_atom_intern("datalist", 8); }
    /* (customizable select: options inside wrappers too; not inside an option, hr, datalist or another select, nor a second optgroup) */
    uint32_t k = 0, sel = dom_slot(id);
    uint32_t stack[64];
    unsigned char og[64];
    int sp = 0;
    uint32_t c = g_dom.first[sel];
    unsigned char in_og = 0;
    while (c || sp > 0) {
        if (!c) { sp--; c = g_dom.next[stack[sp]]; in_og = sp > 0 ? og[sp - 1] : 0; if (sp > 0) in_og = og[sp - 1]; else in_og = 0; continue; }
        if (g_dom.kind[c] == DOM_K_ELEMENT && g_dom.ns[c] == DOM_NS_HTML) {
            CssAtom t = g_dom.tag[c];
            if (t == option) { JS_SetPropertyUint32(ctx, arr, k++, dom_js(ctx, dom_id_of(c))); c = g_dom.next[c]; continue; }
            if (t == hr_ || t == select_ || t == datalist_ || (t == optgroup && in_og)) { c = g_dom.next[c]; continue; }
            if (g_dom.first[c] && sp < 64) {
                unsigned char now = in_og || t == optgroup;
                stack[sp] = c;
                og[sp] = now;
                sp++;
                in_og = now;
                c = g_dom.first[c];
                continue;
            }
        }
        c = g_dom.next[c];
    }
    return arr;
}

/* __cyberCanvasRect(canvas, x, y, w, h, r, g, b, a, clear): a 2D context's solid
 * rectangle (or clearRect), in the bitmap's pixels (display_list_canvas_rect). */
static GCValue js_canvas_rect(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    NodeId el = argc > 0 ? dom_node_of(argv[0]) : 0;
    if (!el || argc < 10) return JS_UNDEFINED;
    double v[9];
    for (int i = 0; i < 9; i++) if (JS_ToFloat64(ctx, &v[i], argv[i + 1])) return JS_UNDEFINED;
    for (int i = 0; i < 8; i++) if (!isfinite(v[i])) return JS_UNDEFINED;
    int ch[4];
    for (int i = 0; i < 3; i++) ch[i] = (int)(v[4 + i] < 0 ? 0 : v[4 + i] > 255 ? 255 : v[4 + i] + 0.5);
    ch[3] = (int)(v[7] < 0 ? 0 : v[7] > 1 ? 255 : v[7] * 255.0 + 0.5);
    uint32_t rgba = (uint32_t)ch[0] << 24 | (uint32_t)ch[1] << 16 | (uint32_t)ch[2] << 8 | (uint32_t)ch[3];
    display_list_canvas_rect(el, (float)v[0], (float)v[1], (float)v[2], (float)v[3], rgba, v[8] == 2.0 ? 2 : v[8] != 0.0);   /* (2: drawn, not cut to the box) */
    dom_mark_dirty(el, DOMF_DIRTY_ATTR);
    dom_request_layout();
    return JS_UNDEFINED;
}

/* __cyberCanvasImage(canvas, url, x, y, w, h): an image drawn whole on a 2D context. */
static GCValue js_canvas_image(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    NodeId el = argc > 0 ? dom_node_of(argv[0]) : 0;
    if (!el || argc < 6) return JS_UNDEFINED;
    const char *url = JS_ToCString(ctx, argv[1]);
    double v[4];
    for (int i = 0; i < 4; i++) if (JS_ToFloat64(ctx, &v[i], argv[i + 2]) || !isfinite(v[i])) return JS_UNDEFINED;
    if (url) {
        display_list_canvas_image(el, url, (float)v[0], (float)v[1], (float)v[2], (float)v[3]);
    }
    dom_mark_dirty(el, DOMF_DIRTY_ATTR);
    dom_request_layout();
    return JS_UNDEFINED;
}

/* __cyberElState(el, mask[, on]): whether el has the state; with on, sets it. */
void dom_live_value_set(NodeId id, const char *v);
/* (__cyberLiveValue(el, v): a form control's value as script set it, to paint; null: none) */
static GCValue js_live_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    NodeId el = argc > 0 ? dom_node_of(argv[0]) : 0;
    if (!el) return JS_UNDEFINED;
    const char *v = argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
    dom_live_value_set(el, v);
    (void)0;   /* (the string is the collector's) */
    dom_mark_dirty(el, DOMF_DIRTY_ATTR);
    dom_request_layout();
    return JS_UNDEFINED;
}
static GCValue js_el_state(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    NodeId el = argc > 0 ? dom_node_of(argv[0]) : 0;
    int32_t mask = 0;
    if (!el || argc < 2 || JS_ToInt32(ctx, &mask, argv[1])) return JS_FALSE;
    uint32_t s = dom_slot(el);
    /* (a dialog shown modally, a popover showing: kept apart) */
    if ((uint32_t)mask == DOM_ST_MODAL || (uint32_t)mask == DOM_ST_POPOVER_OPEN) {
        uint8_t bit = (uint32_t)mask == DOM_ST_MODAL ? 1 : 2;
        bool was = s < g_dom_top_cap && (g_dom_top[s] & bit);
        if (argc > 2 && !JS_IsUndefined(argv[2])) {
            bool on = JS_ToBool(ctx, argv[2]);
            if (on != was) {
                dom_top_state_set(s, bit, on);
                dom_mark_dirty(el, DOMF_DIRTY_ATTR);
                dom_request_layout();
            }
            return JS_NewBool(ctx, on);
        }
        return JS_NewBool(ctx, was);
    }
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        uint16_t was = g_dom.state[s];
        uint16_t now = JS_ToBool(ctx, argv[2]) ? (uint16_t)(was | mask) : (uint16_t)(was & ~mask);
        /* (the focused element, for its removal: dom_api.c) */
        {
            extern NodeId g_dom_focused;
            if (mask & DOM_ST_FOCUS) {
                if (now & DOM_ST_FOCUS) g_dom_focused = el;
                else if (g_dom_focused == el) g_dom_focused = DOM_NULL;
            }
        }
        if (now != was) {
            g_dom.state[s] = now;
            dom_mark_dirty(el, DOMF_DIRTY_ATTR);
            dom_request_layout();
        }
    }
    return JS_NewBool(ctx, (g_dom.state[s] & mask) != 0);
}

/* __cyberHighlightSet(names, priorities, flat): the custom highlights to
 * paint; flat holds 4 per segment: highlight index, text node, UTF-16 start, end. */
static GCValue js_highlight_set(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    if (argc < 3) return JS_UNDEFINED;
    uint32_t nh = 0, nf = 0;
    GCValue v = JS_GetPropertyStr(ctx, argv[0], "length");
    JS_ToUint32(ctx, &nh, v);
    v = JS_GetPropertyStr(ctx, argv[2], "length");
    JS_ToUint32(ctx, &nf, v);
    if (nh > 4096) nh = 4096;
    if (nf > 4 * 65536) nf = 4 * 65536;
    const char **names = (const char **)calloc(nh + 1, sizeof(char *));
    int *prios = (int *)calloc(nh + 1, sizeof(int));
    uint32_t *seg = (uint32_t *)calloc(nf + 4, sizeof(uint32_t));
    if (names && prios && seg) {
        for (uint32_t k = 0; k < nh; k++) {
            GCValue s = JS_GetPropertyUint32(ctx, argv[0], k);
            names[k] = JS_ToCString(ctx, s);
            GCValue p = JS_GetPropertyUint32(ctx, argv[1], k);
            int32_t pv = 0;
            JS_ToInt32(ctx, &pv, p);
            prios[k] = pv;
        }
        for (uint32_t k = 0; k + 3 < nf; k += 4) {
            int32_t h = 0, s = 0, e = 0;
            JS_ToInt32(ctx, &h, JS_GetPropertyUint32(ctx, argv[2], k));
            seg[k] = (uint32_t)h;
            seg[k + 1] = dom_node_of(JS_GetPropertyUint32(ctx, argv[2], k + 1));
            JS_ToInt32(ctx, &s, JS_GetPropertyUint32(ctx, argv[2], k + 2));
            JS_ToInt32(ctx, &e, JS_GetPropertyUint32(ctx, argv[2], k + 3));
            seg[k + 2] = (uint32_t)(s < 0 ? 0 : s);
            seg[k + 3] = (uint32_t)(e < 0 ? 0 : e);
        }
        display_list_highlights_set(names, prios, (int)nh, seg, (int)(nf / 4));
        dom_request_layout();
    }
    free(names); free(prios); free(seg);
    return JS_UNDEFINED;
}

void html_element_api_install(JSContextHandle ctx, GCValue element_proto)
{
    canvas2d_api_install(ctx);
    JS_DefinePropertyValueStr(ctx, JS_GetGlobalObject(ctx), "__cyberHighlightSet",
                              JS_NewCFunction(ctx, js_highlight_set, "__cyberHighlightSet", 3), JS_PROP_CONFIGURABLE);
    if (!JS_IsObject(element_proto)) return;
    JS_DefinePropertyValueStr(ctx, JS_GetGlobalObject(ctx), "__cyberInnerText",
                              JS_NewCFunction(ctx, js_inner_text, "__cyberInnerText", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, JS_GetGlobalObject(ctx), "__cyberElState", JS_NewCFunction(ctx, js_el_state, "__cyberElState", 3), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, JS_GetGlobalObject(ctx), "__cyberLiveValue", JS_NewCFunction(ctx, js_live_value, "__cyberLiveValue", 2), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, JS_GetGlobalObject(ctx), "__cyberCanvasRect", JS_NewCFunction(ctx, js_canvas_rect, "__cyberCanvasRect", 10), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, JS_GetGlobalObject(ctx), "__cyberCanvasImage", JS_NewCFunction(ctx, js_canvas_image, "__cyberCanvasImage", 6), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, JS_GetGlobalObject(ctx), "__cyberOptions",
                              JS_NewCFunction(ctx, js_options, "__cyberOptions", 1), JS_PROP_CONFIGURABLE);
    run_installer(ctx, k_html_element_js, sizeof(k_html_element_js) - 1, "<html_element_api>", element_proto);
}

void html_link_api_install(JSContextHandle ctx, GCValue link_proto)
{
    if (!JS_IsObject(link_proto)) return;
    run_installer(ctx, k_link_js, sizeof(k_link_js) - 1, "<link_api>", link_proto);
}

void html_script_api_install(JSContextHandle ctx, GCValue script_proto)
{
    if (!JS_IsObject(script_proto)) return;
    run_installer(ctx, k_script_js, sizeof(k_script_js) - 1, "<script_api>", script_proto);
}


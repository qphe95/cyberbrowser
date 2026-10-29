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
 * - getClientRects(): the border box as a one-entry list (empty with no box).
 * - checkVisibility(): false without a box or when visibility is hidden.
 * - scrollIntoView / scroll / scrollTo / scrollBy: no-ops (no scrolling).
 */
#include "html_element_api.h"

#include <string.h>

#include <quickjs.h>
#include <quickjs_gc_unified.h>

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
"  var SKIP = {SCRIPT:1, STYLE:1, TEMPLATE:1, NOSCRIPT:1, HEAD:1, TITLE:1, META:1, LINK:1};\n"
"  var BLOCK = {ADDRESS:1, ARTICLE:1, ASIDE:1, BLOCKQUOTE:1, DD:1, DIV:1, DL:1, DT:1,\n"
"    FIELDSET:1, FIGCAPTION:1, FIGURE:1, FOOTER:1, FORM:1, H1:1, H2:1, H3:1, H4:1, H5:1, H6:1,\n"
"    HEADER:1, HR:1, LI:1, MAIN:1, NAV:1, OL:1, P:1, PRE:1, SECTION:1, TABLE:1, TR:1, UL:1};\n"
"  function hiddenEl(el) {\n"
"    if (el.hasAttribute && el.hasAttribute('hidden')) return true;\n"
"    var s = el.style;\n"
"    return !!s && (s.display === 'none' || s.visibility === 'hidden');\n"
"  }\n"
"  function collect(node, out) {\n"
"    for (var c = node.firstChild; c; c = c.nextSibling) {\n"
"      if (c.nodeType === 3) { out.push(String(c.data == null ? c.textContent : c.data).replace(/[ \\t\\n\\r]+/g, ' ')); continue; }\n"
"      if (c.nodeType !== 1) continue;\n"
"      var tag = String(c.tagName || '').toUpperCase();\n"
"      if (SKIP[tag] || hiddenEl(c)) continue;\n"
"      if (tag === 'BR') { out.push('\\n'); continue; }\n"
"      var block = !!BLOCK[tag];\n"
"      if (block) out.push('\\n');\n"
"      collect(c, out);\n"
"      if (block) out.push('\\n');\n"
"    }\n"
"  }\n"
"  function renderedText(el) {\n"
"    if (SKIP[String(el.tagName || '').toUpperCase()]) return el.textContent || '';\n"
"    var out = []; collect(el, out);\n"
"    return out.join('').replace(/ *\\n */g, '\\n').replace(/\\n{2,}/g, '\\n').replace(/^\\n+|\\n+$/g, '').replace(/^ +| +$/g, '');\n"
"  }\n"
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
"  method('focus', function(){\n"
"    var doc = this.ownerDocument || document;\n"
"    if (!this.isConnected) return;\n"
"    var prev = doc.activeElement;\n"
"    if (prev === this) return;\n"
"    if (prev && prev !== doc.body && prev.dispatchEvent) { fire(prev, 'blur', false, this); fire(prev, 'focusout', true, this); }\n"
"    doc.activeElement = this;\n"
"    fire(this, 'focus', false, prev); fire(this, 'focusin', true, prev);\n"
"  });\n"
"  method('blur', function(){\n"
"    var doc = this.ownerDocument || document;\n"
"    if (doc.activeElement !== this) return;\n"
"    doc.activeElement = doc.body;\n"
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
"  boolAttr('hidden', 'hidden'); boolAttr('inert', 'inert');\n"
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
"                if (!isNaN(v)) return v;\n"
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
"  method('insertAdjacentElement', function(where, el){ return adjacent(this, where, el); });\n"
"  method('insertAdjacentText', function(where, text){ adjacent(this, where, this.ownerDocument.createTextNode(String(text))); });\n"
"  method('insertAdjacentHTML', function(where, html){\n"
"    var t = this.ownerDocument.createElement('template'); t.innerHTML = String(html);\n"
"    var src = t.content || t, frag = this.ownerDocument.createDocumentFragment();\n"
"    while (src.firstChild) frag.appendChild(src.firstChild);\n"
"    adjacent(this, where, frag);\n"
"  });\n"
"\n"
"  method('getClientRects', function(){\n"
"    var r = this.getBoundingClientRect(), list = [];\n"
"    if (r && (r.width > 0 || r.height > 0)) list.push(r);\n"
"    list.item = function(i){ return this[i] || null; };\n"
"    return list;\n"
"  });\n"
"  method('checkVisibility', function(opts){\n"
"    var r = this.getBoundingClientRect();\n"
"    if (!r || (r.width === 0 && r.height === 0 && this.getClientRects().length === 0 && !this.firstChild)) return false;\n"
"    for (var e = this; e && e.nodeType === 1; e = e.parentNode) { if (hiddenEl(e)) return false; }\n"
"    return true;\n"
"  });\n"
"  ['scrollIntoView', 'scrollIntoViewIfNeeded', 'scroll', 'scrollTo', 'scrollBy'].forEach(function(n){\n"
"    method(n, function(){});\n"
"  });\n"
"})";

/* CharacterData (Text and Comment share the prototype here): length and the
 * string editing methods over `data`; Text's splitText() and wholeText. */
static const char k_character_data_js[] =
"(function(P){\n"
"  function method(name, fn) {\n"
"    if (typeof P[name] === 'function') return;\n"
"    Object.defineProperty(P, name, {configurable:true, writable:true, value:fn});\n"
"  }\n"
"  function check(node, off) {\n"
"    off = off >>> 0;\n"
"    if (off > node.data.length) throw new DOMException('Offset out of range', 'IndexSizeError');\n"
"    return off;\n"
"  }\n"
"  if (!('length' in P)) Object.defineProperty(P, 'length', {configurable:true, enumerable:true,\n"
"    get: function(){ return String(this.data).length; }});\n"
"  method('appendData', function(s){ this.data = this.data + String(s); });\n"
"  method('substringData', function(off, count){ off = check(this, off); return this.data.substr(off, count >>> 0); });\n"
"  method('replaceData', function(off, count, s){ off = check(this, off); var d = this.data;\n"
"    this.data = d.slice(0, off) + String(s) + d.slice(off + (count >>> 0)); });\n"
"  method('insertData', function(off, s){ this.replaceData(off, 0, s); });\n"
"  method('deleteData', function(off, count){ this.replaceData(off, count, ''); });\n"
"  method('splitText', function(off){\n"
"    off = check(this, off);\n"
"    var tail = this.ownerDocument.createTextNode(this.data.slice(off));\n"
"    this.data = this.data.slice(0, off);\n"
"    if (this.parentNode) this.parentNode.insertBefore(tail, this.nextSibling);\n"
"    return tail;\n"
"  });\n"
"  if (!('wholeText' in P)) Object.defineProperty(P, 'wholeText', {configurable:true, enumerable:true,\n"
"    get: function(){ var s = this; while (s.previousSibling && s.previousSibling.nodeType === 3) s = s.previousSibling;\n"
"      var out = ''; for (; s && s.nodeType === 3; s = s.nextSibling) out += s.data; return out; }});\n"
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
"    set: function(v){ Object.defineProperty(this, 'src', {value: v, writable: true, configurable: true, enumerable: true}); }});\n"
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

void html_element_api_install(JSContextHandle ctx, GCValue element_proto)
{
    if (!JS_IsObject(element_proto)) return;
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

void html_character_data_api_install(JSContextHandle ctx, GCValue character_data_proto)
{
    if (!JS_IsObject(character_data_proto)) return;
    run_installer(ctx, k_character_data_js, sizeof(k_character_data_js) - 1, "<character_data_api>",
                  character_data_proto);
}

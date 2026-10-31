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

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "dom_js.h"
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
        if (g_dom.kind[c] == DOM_K_TEXT) {
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
        if (g_dom.kind[c] == DOM_K_TEXT) tb_put(b, g_dom.text[c].s, g_dom.text[c].len);
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
    uint32_t k = 0, sel = dom_slot(id);
    for (uint32_t c = g_dom.first[sel]; c; c = g_dom.next[c]) {
        if (g_dom.kind[c] != DOM_K_ELEMENT) continue;
        if (g_dom.tag[c] == option) JS_SetPropertyUint32(ctx, arr, k++, dom_js(ctx, dom_id_of(c)));
        else if (g_dom.tag[c] == optgroup)
            for (uint32_t o = g_dom.first[c]; o; o = g_dom.next[o])
                if (g_dom.kind[o] == DOM_K_ELEMENT && g_dom.tag[o] == option)
                    JS_SetPropertyUint32(ctx, arr, k++, dom_js(ctx, dom_id_of(o)));
    }
    return arr;
}

void html_element_api_install(JSContextHandle ctx, GCValue element_proto)
{
    if (!JS_IsObject(element_proto)) return;
    JS_DefinePropertyValueStr(ctx, JS_GetGlobalObject(ctx), "__cyberInnerText",
                              JS_NewCFunction(ctx, js_inner_text, "__cyberInnerText", 1), JS_PROP_CONFIGURABLE);
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


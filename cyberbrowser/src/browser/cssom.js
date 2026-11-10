/*
 * cssom.js - the CSS Object Model's style sheets (CSSOM §6), installed in
 * each realm by platform_api_install (browser/platform_api.c):
 *   - CSSStyleSheet (constructable too), CSSRuleList, MediaList,
 *     StyleSheetList, and the rules: CSSStyleRule, CSSMediaRule,
 *     CSSSupportsRule, CSSContainerRule, CSSLayerBlockRule,
 *     CSSLayerStatementRule, CSSImportRule, CSSNamespaceRule,
 *     CSSFontFaceRule, CSSPageRule, CSSKeyframesRule, CSSKeyframeRule,
 *     CSSPropertyRule, CSSScopeRule, CSSStartingStyleRule;
 *   - <style>.sheet and <link rel=stylesheet>.sheet, document.styleSheets;
 *   - a selector parser (Selectors 4) that checks and serializes selectors
 *     ("serialize a selector"), which querySelector shares;
 *   - CSS rules parsed from a sheet's text (CSS Syntax 3 "consume a list of
 *     rules"); declarations are the native CSSStyleDeclaration's
 *     (__cyberStyleDecl), which checks them against the property table.
 * A sheet changed through these objects renders from its serialization:
 * layout reads element.__cyberSheet.__cyberText() once it is modified
 * (css_layout.c).
 * Called as fn(global, window).
 */
(function (global, win) {
    'use strict';
    var SHEET = Symbol('cyber.sheet'), RULES = Symbol('cyber.rules');

    function define(obj, name, value) {
        Object.defineProperty(obj, name, { value: value, writable: true, configurable: true });
    }
    function expose(name, value) {
        define(global, name, value);
        if (win && win !== global) define(win, name, value);
    }
    function getter(proto, name, get, set) {
        /* (as an interface's attribute: named so, and only for what it's an interface of) */
        var own = function (self) { return self !== proto; };
        var g = get && function () { if (!own(this)) throw new TypeError('Illegal invocation'); return get.call(this); };
        var s = set && function (v) { if (!own(this)) throw new TypeError('Illegal invocation'); return set.call(this, v); };
        if (g) Object.defineProperty(g, 'name', { configurable: true, value: 'get ' + name });
        if (s) Object.defineProperty(s, 'name', { configurable: true, value: 'set ' + name });
        Object.defineProperty(proto, name, { get: g, set: s, configurable: true, enumerable: true });
    }
    function domException(msg, name) {
        if (typeof global.DOMException === 'function') return new global.DOMException(msg, name);
        var e = new Error(msg); e.name = name; return e;
    }
    function illegal(name) {
        return function () { throw new TypeError("Failed to construct '" + name + "': Illegal constructor"); };
    }

    /* ==================================================================
     * Tokens (enough of CSS Syntax 3 for rules and selectors)
     * ================================================================== */

    function isNameStart(c) { return /[A-Za-z_\u0080-￿]/.test(c); }
    function isName(c) { return /[A-Za-z0-9_\-\u0080-￿]/.test(c); }
    function isWS(c) { return c === ' ' || c === '\t' || c === '\n' || c === '\r' || c === '\f'; }

    /* Tokens of s: {t: 'ident'|'function'|'hash'|'string'|'number'|'percentage'|
     * 'dimension'|'delim'|'ws'|'('|')'|'['|']'|'{'|'}'|','|':'|';'|'at'|'bad', v, raw} */
    function tokenize(s) {
        var out = [], i = 0, n = s.length;
        function escape() {
            /* at s[i] == '\\' */
            i++;
            var h = /^[0-9a-fA-F]{1,6}/.exec(s.slice(i, i + 6));
            if (h) {
                i += h[0].length;
                if (i < n && isWS(s[i])) i++;
                var cp = parseInt(h[0], 16);
                if (cp === 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
                return String.fromCodePoint(cp);
            }
            if (i >= n) return '�';
            return s[i++];
        }
        function validEscape(k) { return s[k] === '\\' && k + 1 < n && s[k + 1] !== '\n'; }
        function startsIdent(k) {
            var c = s[k];
            if (c === '-') return k + 1 < n && (isNameStart(s[k + 1]) || s[k + 1] === '-' || validEscape(k + 1));
            if (c !== undefined && isNameStart(c)) return true;
            return validEscape(k);
        }
        function name() {
            var r = '';
            while (i < n) {
                if (isName(s[i])) r += s[i++];
                else if (validEscape(i)) r += escape();
                else break;
            }
            return r;
        }
        function startsNumber(k) {
            var c = s[k];
            if (c === '+' || c === '-') { k++; c = s[k]; }
            if (c >= '0' && c <= '9') return true;
            return c === '.' && s[k + 1] >= '0' && s[k + 1] <= '9';
        }
        while (i < n) {
            var c = s[i], st = i;
            if (c === '/' && s[i + 1] === '*') {
                var e = s.indexOf('*/', i + 2);
                i = e < 0 ? n : e + 2;
                continue;
            }
            if (isWS(c)) { while (i < n && isWS(s[i])) i++; out.push({ t: 'ws', raw: s.slice(st, i) }); continue; }
            if (c === '"' || c === "'") {
                i++;
                var v = '', bad = false;
                while (i < n && s[i] !== c) {
                    if (s[i] === '\n') { bad = true; break; }
                    if (s[i] === '\\') {
                        if (s[i + 1] === '\n') { i += 2; continue; }
                        v += escape();
                        continue;
                    }
                    v += s[i++];
                }
                if (s[i] === c) i++;
                out.push({ t: bad ? 'bad' : 'string', v: v, raw: s.slice(st, i) });
                continue;
            }
            if (startsNumber(i)) {
                var m = /^[+-]?(\d*\.\d+|\d+)([eE][+-]?\d+)?/.exec(s.slice(i));
                i += m[0].length;
                var num = parseFloat(m[0]);
                if (s[i] === '%') { i++; out.push({ t: 'percentage', v: num, raw: s.slice(st, i) }); continue; }
                if (startsIdent(i)) { var u = name(); out.push({ t: 'dimension', v: num, unit: u, raw: s.slice(st, i) }); continue; }
                out.push({ t: 'number', v: num, int: !/[.eE]/.test(m[0]), raw: s.slice(st, i) });
                continue;
            }
            if (startsIdent(i)) {
                var nm = name();
                if (s[i] === '(') { i++; out.push({ t: 'function', v: nm, raw: s.slice(st, i) }); continue; }
                out.push({ t: 'ident', v: nm, raw: s.slice(st, i) });
                continue;
            }
            if (c === '#' && i + 1 < n && (isName(s[i + 1]) || validEscape(i + 1))) {
                i++;
                var id = startsIdent(i);
                out.push({ t: 'hash', v: name(), id: id, raw: s.slice(st, i) });
                continue;
            }
            if (c === '@' && startsIdent(i + 1)) { i++; out.push({ t: 'at', v: name(), raw: s.slice(st, i) }); continue; }
            if ('()[]{},:;'.indexOf(c) >= 0) { i++; out.push({ t: c, raw: c }); continue; }
            i++;
            out.push({ t: 'delim', v: c, raw: c });
        }
        return out;
    }

    /* CSSOM "serialize an identifier" / "serialize a string" */
    function serIdent(s) {
        var r = '';
        for (var k = 0; k < s.length; k++) {
            var c = s[k], cp = s.charCodeAt(k);
            if (cp === 0) r += '�';
            else if ((cp >= 1 && cp <= 0x1f) || cp === 0x7f) r += '\\' + cp.toString(16) + ' ';
            else if (k === 0 && c >= '0' && c <= '9') r += '\\' + cp.toString(16) + ' ';
            else if (k === 1 && c >= '0' && c <= '9' && s[0] === '-') r += '\\' + cp.toString(16) + ' ';
            else if (k === 0 && c === '-' && s.length === 1) r += '\\-';
            else if (cp >= 0x80 || c === '-' || c === '_' || /[0-9A-Za-z]/.test(c)) r += c;
            else r += '\\' + c;
        }
        return r;
    }
    function serString(s) {
        var r = '"';
        for (var k = 0; k < s.length; k++) {
            var c = s[k], cp = s.charCodeAt(k);
            if (cp === 0) r += '�';
            else if ((cp >= 1 && cp <= 0x1f) || cp === 0x7f) r += '\\' + cp.toString(16) + ' ';
            else if (c === '"' || c === '\\') r += '\\' + c;
            else r += c;
        }
        return r + '"';
    }

    /* ==================================================================
     * Selectors (Selectors 4): parse, check, serialize
     * ================================================================== */

    var PSEUDO_CLASSES = ('active active-view-transition any-link autofill blank checked current default defined disabled empty enabled ' +
        'first first-child first-of-type focus focus-visible focus-within fullscreen future heading host hover ' +
        'in-range indeterminate invalid last-child last-of-type left link local-link modal muted only-child ' +
        'only-of-type open optional out-of-range past paused picture-in-picture placeholder-shown playing ' +
        'popover-open read-only read-write required right root scope seeking stalled target target-within ' +
        'user-invalid user-valid valid visited volume-locked buffering closed xr-overlay -webkit-autofill ' +
        '-webkit-any-link -webkit-full-screen has-slotted').split(' ');
    var PSEUDO_CLASS_FNS = 'has-slotted dir has host host-context is lang not nth-child nth-last-child nth-last-of-type nth-of-type state where heading active-view-transition-type'.split(' ');
    var PSEUDO_ELEMENTS = ('after backdrop before cue file-selector-button first-letter first-line grammar-error marker ' +
        'placeholder selection spelling-error target-text view-transition details-content checkmark picker-icon ' +
        'scroll-marker scroll-marker-group column search-text -webkit-scrollbar -webkit-scrollbar-thumb ' +
        '-webkit-scrollbar-track -webkit-scrollbar-button -webkit-scrollbar-corner -webkit-scrollbar-track-piece ' +
        '-webkit-resizer -webkit-input-placeholder -webkit-file-upload-button -webkit-inner-spin-button ' +
        '-webkit-outer-spin-button -webkit-search-cancel-button -webkit-details-marker').split(' ');
    var PSEUDO_ELEMENT_FNS = 'part slotted highlight cue view-transition-group view-transition-group-children view-transition-image-pair view-transition-old view-transition-new picker scroll-button'.split(' ');
    var LEGACY_PSEUDO_ELEMENTS = ['before', 'after', 'first-line', 'first-letter'];

    /* What may follow a pseudo-element in a compound. */
    var HIGHLIGHTS = /^(selection|target-text|spelling-error|grammar-error|highlight|search-text)$/;
    var NOT_AFTER_PART = /^(first|left|right|empty|root|scope|host|first-child|last-child|only-child|first-of-type|last-of-type|only-of-type|nth-child|nth-last-child|nth-of-type|nth-last-of-type|host-context|has)$/;
    function pseudoChainOk(a, b) {
        if (a === 'part') return !HIGHLIGHTS.test(b) ? b !== 'part' && b !== 'slotted' : true;
        if (a === 'slotted') return /^(before|after|marker|placeholder|file-selector-button)$/.test(b);
        if (a === 'before' || a === 'after') return b === 'marker';
        if (a === 'column') return b === 'scroll-marker';
        return /^-webkit-/.test(a) || /^-webkit-/.test(b);
    }
    function checkCompound(c, last) {
        var pe = null;
        for (var i = 0; i < c.items.length; i++) {
            var it = c.items[i];
            if (it.k === 'pelem' || it.k === 'pelemfn') {
                if (pe && !pseudoChainOk(pe, it.v)) throw new SyntaxError('pseudo-element here');
                pe = it.v;
            } else if (pe) {
                if (it.k !== 'pclass' && it.k !== 'pclassfn') throw new SyntaxError('after a pseudo-element');
                if (HIGHLIGHTS.test(pe) && !(pe === 'search-text' && (it.v === 'current' || it.v === 'not'))) throw new SyntaxError('after a highlight');
                if (it.v === 'state' && pe !== 'part') throw new SyntaxError(':state() after a pseudo-element');
                if (pe === 'part' && NOT_AFTER_PART.test(it.v)) throw new SyntaxError('after ::part()');
                if (pe === 'slotted' && !/^(is|where|not)$/.test(it.v)) throw new SyntaxError('after ::slotted()');
            }
        }
        if (pe && !last) throw new SyntaxError('combinator after a pseudo-element');
    }

    function SelectorParser(toks, nested) {
        this.toks = toks;
        this.i = 0;
        this.nested = nested;
    }
    SelectorParser.prototype = {
        peek: function (k) { return this.toks[this.i + (k || 0)]; },
        skipWS: function () { var had = false; while (this.peek() && this.peek().t === 'ws') { this.i++; had = true; } return had; },
        /* <complex-selector-list> (or <relative-...> for :has) */
        list: function (relative, forgiving) {
            var out = [], raws = [], failed = false;
            for (;;) {
                this.skipWS();
                var start = this.i, sel = null;
                try { sel = this.complex(relative); } catch (e) { if (!forgiving) throw e; }
                /* (what a forgiving list can't have where it is: a pseudo-element; more than a compound in :host()) */
                if (forgiving && sel && (JSON.stringify(sel).indexOf('"pelem') >= 0 || (this.inHost && sel.length > 1))) sel = null;
                this.skipWS();
                var t = this.peek();
                if (sel === null || (t && t.t !== ',' && t.t !== ')')) {
                    failed = true;
                    if (!forgiving) throw new SyntaxError('bad selector');
                    /* forgiving: skip to the next top-level comma */
                    this.i = start;
                    var depth = 0;
                    while (this.peek()) {
                        var u = this.peek();
                        if (u.t === '(' || u.t === 'function' || u.t === '[') depth++;
                        else if (u.t === ')' || u.t === ']') { if (!depth) break; depth--; }
                        else if (u.t === ',' && !depth) break;
                        this.i++;
                    }
                    sel = null;
                }
                if (sel) out.push(sel);
                if (forgiving) {
                    var rt = '';
                    for (var ri = start; ri < this.i; ri++) rt += this.toks[ri].raw || '';
                    raws.push(rt.trim());
                }
                t = this.peek();
                if (t && t.t === ',') { this.i++; continue; }
                break;
            }
            /* (an item it had to forgive: the list as it was written, CSSOM's way) */
            if (forgiving && failed) out.raw = raws.join(', ');
            return out;
        },
        complex: function (relative) {
            var parts = [];
            this.skipWS();
            var t = this.peek();
            if (relative && t && t.t === 'delim' && (t.v === '>' || t.v === '+' || t.v === '~')) {
                this.i++;
                parts.push({ comb: t.v });
                this.skipWS();
            } else if (relative) parts.push({ comb: ' ' });
            var first = this.compound();
            if (!first) throw new SyntaxError('expected a selector');
            parts.push(first);
            var prevCompound = first;
            for (;;) {
                var ws = this.skipWS();
                t = this.peek();
                if (!t || t.t === ',' || t.t === ')' || t.t === '{') break;
                checkCompound(prevCompound, false);
                var comb = ws ? ' ' : null;
                if (t.t === 'delim' && (t.v === '>' || t.v === '+' || t.v === '~')) {
                    comb = t.v;
                    this.i++;
                    this.skipWS();
                } else if (t.t === 'delim' && t.v === '|' && this.peek(1) && this.peek(1).t === 'delim' && this.peek(1).v === '|') {
                    throw new SyntaxError('column combinator');
                }
                if (!comb) throw new SyntaxError('unexpected token');
                var c = this.compound();
                if (!c) throw new SyntaxError('expected a compound selector');
                parts.push({ comb: comb });
                parts.push(c);
                prevCompound = c;
            }
            checkCompound(prevCompound, true);
            return parts;
        },
        /* type? subclass* (pseudo-element pseudo-class*)*  */
        compound: function () {
            var items = [], t = this.peek();
            var type = this.typeSelector();
            if (type) items.push(type);
            for (;;) {
                t = this.peek();
                if (!t) break;
                if (t.t === 'hash') {
                    if (!t.id) throw new SyntaxError('bad id');
                    this.i++;
                    items.push({ k: 'id', v: t.v });
                } else if (t.t === 'delim' && t.v === '.') {
                    var n = this.peek(1);
                    if (!n || n.t !== 'ident') throw new SyntaxError('bad class');
                    this.i += 2;
                    /* (.foo-*: a class prefix, the asterisk right after the dash) */
                    var star = this.toks[this.i];
                    if (star && star.t === 'delim' && star.v === '*' && /-$/.test(n.v) && n.v !== '-') {
                        this.i++;
                        items.push({ k: 'classprefix', v: n.v });
                        var after = this.toks[this.i];
                        if (after && after.t === 'delim' && after.v === '*') throw new SyntaxError('bad class prefix');
                        continue;
                    }
                    items.push({ k: 'class', v: n.v });
                } else if (t.t === '[') {
                    items.push(this.attribute());
                } else if (t.t === ':') {
                    items.push(this.pseudo());
                } else if (t.t === 'delim' && t.v === '&') {
                    this.i++;
                    items.push({ k: 'nest' });
                } else break;
            }
            return items.length ? { k: 'compound', items: items } : null;
        },
        typeSelector: function () {
            var t = this.peek(), ns = null, n1 = this.peek(1), n2 = this.peek(2);
            var isNameTok = function (x) { return x && (x.t === 'ident' || (x.t === 'delim' && x.v === '*')); };
            var nameOf = function (x) { return x.t === 'ident' ? x.v : '*'; };
            if (t && t.t === 'delim' && t.v === '|' && isNameTok(n1)) {
                this.i += 2;
                return { k: 'type', ns: '', v: nameOf(n1) };
            }
            if (isNameTok(t) && n1 && n1.t === 'delim' && n1.v === '|' && isNameTok(n2)) {
                this.i += 3;
                return { k: 'type', ns: nameOf(t), v: nameOf(n2) };
            }
            if (isNameTok(t)) { this.i++; return { k: 'type', ns: null, v: nameOf(t) }; }
            return null;
        },
        attribute: function () {
            var b = this.i;
            this.i++;   /* [ */
            this.skipWS();
            var t = this.peek(), ns = null, name;
            if (t && t.t === 'delim' && t.v === '|') { this.i++; ns = ''; t = this.peek(); }
            if (t && t.t === 'delim' && t.v === '*' && this.peek(1) && this.peek(1).t === 'delim' && this.peek(1).v === '|') {
                this.i += 2; ns = '*'; t = this.peek();
            }
            if (!t || t.t !== 'ident') throw new SyntaxError('bad attribute');
            this.i++;
            if (ns === null && this.peek() && this.peek().t === 'delim' && this.peek().v === '|' && this.peek(1) && this.peek(1).t === 'ident') {
                ns = t.v;
                t = this.peek(1);
                this.i += 2;
            }
            name = t.v;
            this.skipWS();
            var op = null, value = null, flag = null;
            t = this.peek();
            if (t && t.t === 'delim' && '~|^$*'.indexOf(t.v) >= 0 && this.peek(1) && this.peek(1).t === 'delim' && this.peek(1).v === '=') {
                op = t.v + '=';
                this.i += 2;
            } else if (t && t.t === 'delim' && t.v === '=') { op = '='; this.i++; }
            if (op) {
                this.skipWS();
                t = this.peek();
                if (!t || (t.t !== 'ident' && t.t !== 'string')) throw new SyntaxError('bad attribute value');
                value = t.v;
                this.i++;
                this.skipWS();
                t = this.peek();
                if (t && t.t === 'ident' && (t.v.toLowerCase() === 'i' || t.v.toLowerCase() === 's')) {
                    flag = t.v.toLowerCase();
                    this.i++;
                    this.skipWS();
                }
            }
            t = this.peek();
            if (!t || t.t !== ']') throw new SyntaxError('bad attribute');
            this.i++;
            return { k: 'attr', ns: ns, name: name, op: op, value: value, flag: flag };
        },
        pseudo: function () {
            this.i++;   /* : */
            var element = false, t = this.peek();
            if (t && t.t === ':') { element = true; this.i++; t = this.peek(); }
            if (!t) throw new SyntaxError('bad pseudo');
            if (t.t === 'ident') {
                var name = t.v.toLowerCase();
                this.i++;
                if (element) {
                    if (PSEUDO_ELEMENTS.indexOf(name) < 0) throw new SyntaxError('unknown pseudo-element');
                    return { k: 'pelem', v: name };
                }
                if (LEGACY_PSEUDO_ELEMENTS.indexOf(name) >= 0) return { k: 'pelem', v: name };
                if (PSEUDO_CLASSES.indexOf(name) < 0) throw new SyntaxError('unknown pseudo-class');
                return { k: 'pclass', v: name };
            }
            if (t.t !== 'function') throw new SyntaxError('bad pseudo');
            var fn = t.v.toLowerCase();
            this.i++;
            var known = element ? PSEUDO_ELEMENT_FNS : PSEUDO_CLASS_FNS;
            if (known.indexOf(fn) < 0) throw new SyntaxError('unknown functional pseudo');
            var arg = this.pseudoArgs(fn, element);
            t = this.peek();
            if (!t || t.t !== ')') throw new SyntaxError('unclosed pseudo');
            this.i++;
            return { k: element ? 'pelemfn' : 'pclassfn', v: fn, arg: arg };
        },
        pseudoArgs: function (fn, element) {
            this.skipWS();
            if (!element && (fn === 'is' || fn === 'where')) return { list: this.list(false, true) };
            if (!element && fn === 'not') {
                var l = this.list(false, false);
                if (!l.length) throw new SyntaxError('empty :not');
                if (JSON.stringify(l).indexOf('"pelem') >= 0) throw new SyntaxError('pseudo-element in :not');
                return { list: l };
            }
            if (!element && fn === 'has') {
                if (this.inHas) throw new SyntaxError(':has() in :has()');
                this.inHas = (this.inHas || 0) + 1;
                var h;
                try { h = this.list(true, false); } finally { this.inHas--; }
                if (!h.length) throw new SyntaxError('empty :has');
                return { list: h, relative: true };
            }
            if (!element && fn === 'has-slotted') {
                /* (one selector of what's slotted, no child combinator) */
                var hs = this.list(false, false);
                if (hs.length !== 1 || JSON.stringify(hs).indexOf('">"') >= 0) throw new SyntaxError('bad :has-slotted()');
                return { list: hs };
            }
            if (!element && (fn === 'host' || fn === 'host-context')) {
                this.inHost = (this.inHost || 0) + 1;
                var c;
                try { c = this.compound(); } finally { this.inHost--; }
                if (!c) throw new SyntaxError('bad :host()');
                this.skipWS();
                return { compound: c };
            }
            if (element && fn === 'slotted') {
                var sc = this.compound();
                if (!sc) throw new SyntaxError('bad ::slotted()');
                this.skipWS();
                return { compound: sc };
            }
            if (!element && fn.indexOf('nth-') === 0) {
                var ab = this.anPlusB();
                this.skipWS();
                var t = this.peek(), of = null;
                if (t && t.t === 'ident' && t.v.toLowerCase() === 'of' && (fn === 'nth-child' || fn === 'nth-last-child')) {
                    this.i++;
                    of = this.list(false, false);
                    if (!of.length) throw new SyntaxError('empty of');
                }
                return { ab: ab, of: of };
            }
            /* anything else: the tokens to the closing parenthesis */
            var raw = '', depth = 0, count = 0;
            while (this.peek()) {
                var u = this.peek();
                if (u.t === '(' || u.t === 'function') depth++;
                else if (u.t === ')') { if (!depth) break; depth--; }
                raw += u.raw;
                this.i++;
                count++;
            }
            raw = raw.trim();
            if (!raw) throw new SyntaxError('empty argument');
            if (fn === 'state' && !/^(?:[a-zA-Z_-]|[^\x00-\x7f]|\\.)(?:[\w-]|[^\x00-\x7f]|\\.)*$/.test(raw)) throw new SyntaxError('bad :state');
            if (element && fn === 'scroll-button' && !/^(\*|up|down|left|right|block-start|block-end|inline-start|inline-end|prev|next)$/i.test(raw)) throw new SyntaxError('bad ::scroll-button');
            if (element && fn === 'picker' && !/^select$/i.test(raw)) throw new SyntaxError('bad ::picker');
            if (element && fn === 'highlight' && !/^(?:\*|-?(?:[a-zA-Z_]|[^\x00-\x7f]|\\.)(?:[\w-]|[^\x00-\x7f]|\\.)*)$/.test(raw)) throw new SyntaxError('bad ::highlight');
            if (fn === 'dir' && !/^(ltr|rtl)$/i.test(raw)) throw new SyntaxError('bad :dir');
            if (fn === 'heading' && raw && !/^\s*[-+]?\d+(\s*,\s*[-+]?\d+)*\s*$/.test(raw)) throw new SyntaxError('bad :heading');
            return { raw: raw };
        },
        /* <an+b>: returns [a, b] */
        anPlusB: function () {
            var s = '';
            while (this.peek() && this.peek().t !== ')' && !(this.peek().t === 'ident' && this.peek().v.toLowerCase() === 'of')) {
                s += this.peek().raw;
                this.i++;
            }
            var str = s.trim().toLowerCase();
            if (str === 'odd') return [2, 1];
            if (str === 'even') return [2, 0];
            var m = /^([+-]?)(\d*)n\s*(?:([+-])\s*(\d+))?$/.exec(str);
            if (m) {
                if (/^[+-]?\d*n\s*[+-]\s*\d/.test(str) && /n\s*[+-]\d/.test(str) && !/n\s*[+-]\s+\d/.test(str) && /n[+-]\d/.test(str.replace(/\s+/g, ''))) {
                    /* (fine) */
                }
                var a = m[2] === '' ? 1 : parseInt(m[2], 10);
                if (m[1] === '-') a = -a;
                var b = m[4] ? parseInt(m[4], 10) * (m[3] === '-' ? -1 : 1) : 0;
                /* whitespace is not allowed between a sign and its number, nor inside "n-" forms with a space before the sign only */
                if (/[+-]\s+n/.test(str) || /^[+-]\s/.test(str)) throw new SyntaxError('bad an+b');
                return [a, b];
            }
            if (/^[+-]?\d+$/.test(str)) return [0, parseInt(str, 10)];
            throw new SyntaxError('bad an+b');
        },
    };

    function serAnB(ab) {
        var a = ab[0], b = ab[1];
        if (a === 0) return String(b);
        var s = a === 1 ? 'n' : a === -1 ? '-n' : a + 'n';
        if (b > 0) s += '+' + b;
        else if (b < 0) s += '-' + (-b);
        return s;
    }

    /* (the namespaces the sheet being read declares: its default and its prefixes) */
    var NS_CTX = { dflt: null, map: Object.create(null) };
    function serCompound(c) {
        var s = '';
        for (var k = 0; k < c.items.length; k++) {
            var it = c.items[k];
            switch (it.k) {
            case 'type':
                /* (a prefix that says nothing more: any namespace with no default
                 * one, or the default's own) */
                var ns = it.ns;
                if (ns === '*' && NS_CTX.dflt === null) ns = null;
                else if (ns !== null && ns !== '*' && ns !== '' && NS_CTX.dflt !== null && NS_CTX.map[ns] === NS_CTX.dflt) ns = null;
                /* (an implied * before what follows it goes without saying) */
                if (it.v === '*' && ns === null && c.items[k + 1]) break;
                if (ns !== null) s += (ns === '*' ? '*' : serIdent(ns)) + '|';
                s += it.v === '*' ? '*' : serIdent(it.v);
                break;
            case 'id': s += '#' + serIdent(it.v); break;
            case 'class': s += '.' + serIdent(it.v); break;
            case 'classprefix': s += '.' + serIdent(it.v) + '*'; break;
            case 'nest': s += '&'; break;
            case 'attr':
                s += '[';
                if (it.ns !== null) s += (it.ns === '*' ? '*' : serIdent(it.ns)) + '|';
                s += serIdent(it.name);
                if (it.op) { s += it.op + serString(it.value); if (it.flag) s += ' ' + it.flag; }
                s += ']';
                break;
            case 'pclass': s += ':' + it.v; break;
            case 'pelem': s += '::' + it.v; break;
            case 'pclassfn':
            case 'pelemfn':
                s += (it.k === 'pelemfn' ? '::' : ':') + it.v + '(';
                var a = it.arg;
                if (a.list && a.list.raw !== undefined) s += a.list.raw;
                else if (a.list) s += serList(a.list);
                else if (a.compound) s += serCompound(a.compound);
                else if (a.ab) s += serAnB(a.ab) + (a.of ? ' of ' + serList(a.of) : '');
                else s += a.raw;
                s += ')';
                break;
            }
        }
        return s;
    }
    function serComplex(parts) {
        var s = '';
        for (var k = 0; k < parts.length; k++) {
            var p = parts[k];
            if (p.comb !== undefined) {
                if (k === 0) { if (p.comb !== ' ') s += p.comb + ' '; }
                else s += p.comb === ' ' ? ' ' : ' ' + p.comb + ' ';
            } else s += serCompound(p);
        }
        return s;
    }
    function serList(list) { return list.map(serComplex).join(', '); }

    /* The selector list of text, serialized; throws SyntaxError when invalid. */
    function parseSelectorList(text) {
        var toks = tokenize(text);
        var p = new SelectorParser(toks);
        var list = p.list(false, false);
        p.skipWS();
        if (p.peek() || !list.length) throw new SyntaxError('bad selector');
        /* (view transition pseudo-elements: querySelector's stricter grammar) */
        if (/::view-transition/i.test(text)) global.document.createDocumentFragment().querySelector(text.replace(/:active-view-transition(-type\([^)]*\))?(?![-\w(])/gi, ''));
        return serList(list);
    }
    /* (the end of the input closes what's open) */
    function selectorClosed(text) {
        var open = [], q = '';
        for (var i = 0; i < text.length; i++) {
            var c = text[i];
            if (c === '\\') { i++; continue; }
            if (q) { if (c === q) q = ''; continue; }
            if (c === '"' || c === "'") q = c;
            else if (c === '(') open.push(')');
            else if (c === '[') open.push(']');
            else if (c === ')' || c === ']') { if (open.pop() !== c) return text; }
        }
        return text + q + open.reverse().join('');
    }
    function selectorValid(text) {
        try { parseSelectorList(selectorClosed(text)); return true; } catch (e) { return false; }
    }
    Object.defineProperty(global, '__cyberSelectorValid', { configurable: true, value: selectorValid });

    /* ==================================================================
     * MediaList
     * ================================================================== */

    function normalizeMedia(q) {
        return q.replace(/\s+/g, ' ').trim().replace(/\(\s+/g, '(').replace(/\s+\)/g, ')')
            .replace(/\s*:\s*/g, ': ').replace(/^(only|not)\s+/i, function (m) { return m.toLowerCase(); })
            .replace(/(^|\s)(and|or)(\s|\()/gi, function (m, a, w, b) { return a + w.toLowerCase() + b; })
            .replace(/(^|[\s(,])(all|screen|print|speech)\b/gi, function (m, a, w) { return a + w.toLowerCase(); });
    }
    /* Whether q is a <media-query> (mediaqueries-5 2.); what it means is
     * for the matching. */
    function mediaValid(q) {
        var s = q.trim(), i = 0;
        function ws() { while (i < s.length && /\s/.test(s[i])) i++; }
        function word(w) {
            ws();
            var t = s.substr(i, w.length);
            if (t.toLowerCase() !== w) return false;
            var c = s[i + w.length];
            if (c !== undefined && !/[\s(]/.test(c)) return false;
            i += w.length;
            return true;
        }
        function blockEnd() {
            var depth = 0;
            for (; i < s.length; i++) {
                if (s[i] === '(') depth++;
                else if (s[i] === ')' && --depth === 0) { i++; return true; }
                else if (s[i] === '[' || s[i] === ']' || s[i] === '{' || s[i] === '}') return false;
            }
            return true;       /* (the end of the input closes it) */
        }
        function inParens() {
            ws();
            if (s[i] === '(') return blockEnd();
            /* (<general-enclosed>: a function) */
            var m = /^-?[a-zA-Z_][\w-]*\(/.exec(s.slice(i));
            if (!m) return false;
            i += m[0].length - 1;
            return blockEnd();
        }
        function condition(orOk) {
            if (word('not')) return inParens();
            if (!inParens()) return false;
            if (word('and')) { do { if (!inParens()) return false; } while (word('and')); return true; }
            if (orOk && word('or')) { do { if (!inParens()) return false; } while (word('or')); return true; }
            return true;
        }
        function done() { ws(); return i >= s.length; }
        ws();
        var save = i;
        if (s[i] === '(' || /^-?[a-zA-Z_][\w-]*\(/.test(s) || /^not[\s(]+[^\s]/i.test(s) && (function () {
            word('not'); ws(); var c = s[i] === '(' || /^-?[a-zA-Z_][\w-]*\(/.test(s.slice(i)); i = save; return c; })()) {
            return condition(true) && done();
        }
        if (!word('not')) word('only');
        ws();
        var m = /^-?[a-zA-Z_][\w-]*/.exec(s.slice(i));
        if (!m || /^(and|or|not|only|layer)$/i.test(m[0])) return false;
        i += m[0].length;
        if (done()) return true;
        return word('and') && condition(false) && done();
    }
    function splitMedia(text) {
        var out = [], depth = 0, cur = '';
        for (var k = 0; k < text.length; k++) {
            var c = text[k];
            if (c === '(') depth++;
            else if (c === ')') depth--;
            if (c === ',' && !depth) { out.push(cur); cur = ''; } else cur += c;
        }
        out.push(cur);
        if (out.length === 1 && !out[0].trim()) return [];
        return out.map(function (q) { return mediaValid(q) ? normalizeMedia(q) : 'not all'; });
    }
    function MediaList() { throw new TypeError('Illegal constructor'); }
    function newMediaList(text, onChange) {
        var m = Object.create(MediaList.prototype);
        Object.defineProperty(m, '__items', { value: splitMedia(text || ''), writable: true });
        Object.defineProperty(m, '__onChange', { value: onChange || null });
        return m;
    }
    getter(MediaList.prototype, 'mediaText', function () { return this.__items.join(', '); },
        function (v) {
            if (!this || !this.__items) throw new TypeError('Illegal invocation');
            this.__items = splitMedia(v == null ? '' : String(v)); if (this.__onChange) this.__onChange();
        });
    getter(MediaList.prototype, 'length', function () { return this.__items.length; });
    define(MediaList.prototype, 'item', function (i) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'item' on 'MediaList': 1 argument required");
        i = i >>> 0; return i < this.__items.length ? this.__items[i] : null; });
    define(MediaList.prototype, 'appendMedium', function (m) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'appendMedium' on 'MediaList': 1 argument required");
        m = normalizeMedia(String(m));
        if (m && this.__items.indexOf(m) < 0) { this.__items.push(m); if (this.__onChange) this.__onChange(); }
    });
    define(MediaList.prototype, 'deleteMedium', function (m) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'deleteMedium' on 'MediaList': 1 argument required");
        m = normalizeMedia(String(m));
        var k = this.__items.indexOf(m);
        if (k < 0) throw domException("Failed to execute 'deleteMedium': the medium is not in the list.", 'NotFoundError');
        this.__items.splice(k, 1);
        if (this.__onChange) this.__onChange();
    });
    define(MediaList.prototype, 'toString', function () {
        if (!this || !this.__items) throw new TypeError('Illegal invocation');
        return this.mediaText;
    });
    /* (a stringifier is enumerable) */
    Object.defineProperty(MediaList.prototype, 'toString', { enumerable: true });
    Object.defineProperty(MediaList.prototype, Symbol.toStringTag, { value: 'MediaList', configurable: true });
    MediaList.prototype[Symbol.iterator] = Array.prototype[Symbol.iterator];
    /* (a declaration block is a list of its property names) */
    /* (CSS Parser API: the namespace's operations, each a parse of its input; nothing is
     * built from them yet) */
    /* ---- CSS Paint API, a small one: registerPaint classes run here; each
     * element whose background-image is paint(name, args) has its class's
     * paint() draw on a recording context (canvas_api.js), its solid
     * rectangles painted as that background ---- */
    if (global.CSS && !global.CSS.paintWorklet) (function () {
        var defs = Object.create(null);
        /* (registerPaint as CSS Painting API 1 has it: the name, then the class's
         * inputProperties, inputArguments (syntax strings), contextOptions, that it
         * constructs, its prototype and the prototype's paint, in that order) */
        function seqOfStrings(v, what) {
            if (v === undefined) return [];
            if (v === null || typeof v !== 'object' || typeof v[Symbol.iterator] !== 'function')
                throw new TypeError("Failed to execute 'registerPaint': The '" + what + "' property is not a sequence.");
            return Array.from(v, String);
        }
        var TYPE = '(?:<(?:length|number|percentage|length-percentage|color|image|url|integer|angle|time|resolution|transform-function|custom-ident|transform-list|string)>|[-_a-zA-Z][-_a-zA-Z0-9]*)[+#]?';
        var SYNTAX = new RegExp('^\\s*(?:\\*|' + TYPE + '(?:\\s*\\|\\s*' + TYPE + ')*)\\s*$');
        function registerPaint(name, cls) {
            name = String(name);
            if (!name) throw new TypeError("Failed to execute 'registerPaint': The empty string is not a valid name.");
            if (defs[name]) throw new global.DOMException("A class with name:'" + name + "' is already registered.", 'InvalidModificationError');
            var c = cls !== null && (typeof cls === 'object' || typeof cls === 'function') ? cls : {};
            var ip = seqOfStrings(c.inputProperties, 'inputProperties');
            seqOfStrings(c.inputArguments, 'inputArguments').forEach(function (t) {
                if (!SYNTAX.test(t)) throw new TypeError("Failed to execute 'registerPaint': Invalid argument types.");
            });
            void c.contextOptions;
            if (typeof cls !== 'function') throw new TypeError("Failed to execute 'registerPaint': The provided callback is not a function.");
            var proto = cls.prototype;
            if (proto === null || typeof proto !== 'object') throw new TypeError("Failed to execute 'registerPaint': The 'prototype' object on the class does not exist.");
            if (typeof proto.paint !== 'function') throw new TypeError("Failed to execute 'registerPaint': The 'paint' function on the prototype does not exist.");
            defs[name] = cls;
            inprops[name] = ip;
        }
        var inprops = Object.create(null);
        /* (the paint's styleMap: its inputProperties' computed values, Typed OM's) */
        function inputMap(tom, ip) {
            function one(n) {
                var v = null;
                try { v = tom.get(n); } catch (e) { v = null; }
                if (v == null && /^--/.test(n) && typeof global.CSSUnparsedValue === 'function') v = new global.CSSUnparsedValue([]);
                return v;
            }
            return {
                get: function (n) { return ip.indexOf(String(n)) >= 0 ? one(String(n)) : undefined; },
                getAll: function (n) { n = String(n); if (ip.indexOf(n) < 0) return []; try { var a = tom.getAll(n); if (a && a.length) return a; } catch (e) { } var v = one(n); return v ? [v] : []; },
                has: function (n) { return ip.indexOf(String(n)) >= 0; },
                keys: function () { return ip.slice()[Symbol.iterator](); },
                get size() { return ip.length; },
                forEach: function (cb, t) { var self = this; ip.forEach(function (n) { cb.call(t, self.getAll(n), n, self); }); }
            };
        }
        function splitArgs(s) {
            var out = [], d = 0, st = 0;
            for (var i = 0; i < s.length; i++) {
                var c = s[i];
                if (c === '(') d++;
                else if (c === ')') d--;
                else if (c === ',' && !d) { out.push(s.slice(st, i)); st = i + 1; }
            }
            out.push(s.slice(st));
            return out;
        }
        /* (layer k's tile size and the origins of its tiles in a W x H area, as the
         * background-size, -position and -repeat lists say; no natural size: auto, cover
         * and contain the area) */
        function listAt(v, k) { var l = splitArgs(String(v || '')); return (l[k % l.length] || '').trim(); }
        function lenOf(t, ref) {
            if (/%$/.test(t)) return parseFloat(t) / 100 * ref;
            var n = parseFloat(t);
            return isFinite(n) ? n : null;
        }
        function tileOf(cs, k, W, H) {
            var sz = listAt(cs.backgroundSize, k).split(/\s+/), w = W, h = H;
            if (sz[0] && sz[0] !== 'auto' && sz[0] !== 'cover' && sz[0] !== 'contain') { var lw = lenOf(sz[0], W); if (lw !== null) w = lw; }
            if (sz[1] && sz[1] !== 'auto') { var lh = lenOf(sz[1], H); if (lh !== null) h = lh; }
            if (!(w > 0) || !(h > 0)) return { w: 0, h: 0, origins: [] };
            var pos = listAt(cs.backgroundPosition, k).split(/\s+/);
            function off(t, free) {
                if (!t || t === 'left' || t === 'top') return 0;
                if (t === 'center') return free / 2;
                if (t === 'right' || t === 'bottom') return free;
                if (/%$/.test(t)) return parseFloat(t) / 100 * free;
                var n = parseFloat(t); return isFinite(n) ? n : 0;
            }
            var ox = off(pos[0], W - w), oy = off(pos[1] || pos[0] === 'top' || pos[0] === 'bottom' ? pos[1] : '50%', H - h);
            if (pos.length < 2 && (pos[0] === 'top' || pos[0] === 'bottom')) { oy = off(pos[0], H - h); ox = (W - w) / 2; }
            var rep = listAt(cs.backgroundRepeat, k).split(/\s+/), rx = rep[0], ry = rep[1] || rep[0];
            if (rx === 'repeat-x') { rx = 'repeat'; ry = 'no-repeat'; }
            else if (rx === 'repeat-y') { rx = 'no-repeat'; ry = 'repeat'; }
            function starts(o, t, len, on) {
                if (!on) return [o];
                var s = o - Math.ceil(o / t) * t, out = [];
                for (var x = s; x < len && out.length < 400; x += t) out.push(x);
                return out;
            }
            var xs = starts(ox, w, W, rx !== 'no-repeat'), ys = starts(oy, h, H, ry !== 'no-repeat'), origins = [];
            for (var i = 0; i < ys.length; i++) for (var j = 0; j < xs.length; j++) origins.push([xs[j], ys[i]]);
            return { w: w, h: h, origins: origins };
        }
        function paintAll() {
            var doc = global.document;
            if (!doc || typeof global.__cyberPaintContext !== 'function') return;
            var els = doc.querySelectorAll('*');
            for (var i = 0; i < els.length; i++) {
                var el = els[i], cs = global.getComputedStyle(el), bi = cs.backgroundImage;
                if (!bi || bi.indexOf('paint(') < 0) continue;
                /* (each layer's paint(name, args): the last first, as layers stack) */
                var re = /paint\(\s*([-\w]+)\s*(?:,((?:[^()]|\((?:[^()]|\([^()]*\))*\))*))?\)/g, layers = [], mm;
                while ((mm = re.exec(bi))) layers.push(mm);
                if (!layers.length || !global.__cyberPaintContext(el)) continue;
                /* (the element's computed values as Typed OM has them, else as strings) */
                var tom = null;
                try { tom = typeof el.computedStyleMap === 'function' ? el.computedStyleMap() : null; } catch (e) { tom = null; }
                var props = tom ? null : { get: function (n) { var v = String(cs.getPropertyValue(n)).trim(); return { toString: function () { return v; }, value: v }; },
                                          getAll: function (n) { return [this.get(n)]; }, has: function () { return true; } };
                var nat = global.__cyberCanvasRect;
                for (var li = layers.length - 1; li >= 0; li--) {
                    var m = layers[li];
                    if (!defs[m[1]]) continue;
                    var args = m[2] ? splitArgs(m[2]).map(function (a) { a = a.trim(); return { toString: function () { return a; } }; }) : [];
                    var lp = tom ? inputMap(tom, inprops[m[1]] || []) : props;
                    /* (its tile: background-size, -position and -repeat of this layer, in the padding box) */
                    var W = el.clientWidth, H = el.clientHeight, tl = tileOf(cs, layers.length - 1 - li, W, H);
                    /* (what it draws held back until paint() returns: one that throws is an invalid image, nothing) */
                    var buf = [], ok = true;
                    Object.defineProperty(global, '__cyberCanvasRect', { configurable: true, writable: true,
                        value: function () { buf.push(Array.prototype.slice.call(arguments)); } });
                    try {
                        var ctx = global.__cyberPaintContext(el, true);
                        new defs[m[1]]().paint(ctx, { width: tl.w, height: tl.h }, lp, args);
                    } catch (e) { ok = false; }
                    Object.defineProperty(global, '__cyberCanvasRect', { configurable: true, writable: true, value: nat });
                    if (ok) tl.origins.forEach(function (o) {
                        buf.forEach(function (a) {
                            var b = a.slice();
                            b[0] = el;
                            if (!b[9]) {
                                /* (clipped to the tile, then put at its place) */
                                var x0 = Math.max(b[1], 0), y0 = Math.max(b[2], 0), x1 = Math.min(b[1] + b[3], tl.w), y1 = Math.min(b[2] + b[4], tl.h);
                                if (x1 <= x0 || y1 <= y0) return;
                                b[1] = x0 + o[0]; b[2] = y0 + o[1]; b[3] = x1 - x0; b[4] = y1 - y0;
                            }
                            nat.apply(null, b);
                        });
                    });
                }
            }
        }
        var worklet = { addModule: function addModule(url) {
            url = String(url);
            var reg = global.__blobRegistry, b = reg && reg[url];
            var p = b && typeof b.text === 'function' ? b.text() : global.fetch(url).then(function (r) { return r.text(); });
            return p.then(function (code) {
                (new Function('registerPaint', code))(registerPaint);
                paintAll();
                /* (painted again each frame for a while: animations and later style changes) */
                var left = 120;
                function again() { paintAll(); if (--left > 0 && typeof global.requestAnimationFrame === 'function') global.requestAnimationFrame(again); }
                if (typeof global.requestAnimationFrame === 'function') global.requestAnimationFrame(again);
            });
        } };
        Object.defineProperty(global.CSS, 'paintWorklet', { configurable: true, enumerable: true, get: function () { return worklet; } });
    })();
    /* ---- CSS Layout API, emulated: registerLayout classes run here; an element
     * whose display is layout(name) is laid out by the engine as flow-root, its
     * children measured by the engine (layoutNextFragment), then placed where the
     * class's layout() puts them, as absolutely positioned boxes of the sizes it
     * got; its block size the autoBlockSize when its own is not fixed ---- */
    if (global.CSS && !global.CSS.layoutWorklet) (function () {
        var defs = Object.create(null);
        function seqOf(v) { return v && typeof v[Symbol.iterator] === 'function' ? Array.from(v, String) : []; }
        function registerLayout(name, cls) {
            name = String(name);
            if (!name) throw new TypeError("Failed to execute 'registerLayout': The empty string is not a valid name.");
            if (defs[name]) throw new global.DOMException("A class with name:'" + name + "' is already registered.", 'InvalidModificationError');
            if (typeof cls !== 'function') throw new TypeError("Failed to execute 'registerLayout': The provided callback is not a function.");
            var proto = cls.prototype;
            if (!proto || typeof proto.layout !== 'function') throw new TypeError("Failed to execute 'registerLayout': The 'layout' function on the prototype does not exist.");
            defs[name] = { cls: cls, ip: seqOf(cls.inputProperties), cip: seqOf(cls.childInputProperties) };
        }
        /* (a styleMap of only these properties, Typed OM values, unparsed for custom ones) */
        function mapOf(el, ip) {
            var tom = null;
            try { tom = typeof el.computedStyleMap === 'function' ? el.computedStyleMap() : null; } catch (e) { tom = null; }
            var cs = global.getComputedStyle(el);
            function one(n) {
                var v = null;
                try { v = tom ? tom.get(n) : null; } catch (e) { v = null; }
                if (v == null) {
                    var t = String(cs.getPropertyValue(n));
                    v = typeof global.CSSUnparsedValue === 'function' && /^--/.test(n) ? new global.CSSUnparsedValue(t ? [t] : []) : { toString: function () { return t; } };
                }
                return v;
            }
            return {
                get: function (n) { n = String(n); return ip.indexOf(n) >= 0 ? one(n) : undefined; },
                getAll: function (n) { n = String(n); return ip.indexOf(n) >= 0 ? [one(n)] : []; },
                has: function (n) { return ip.indexOf(String(n)) >= 0; },
                keys: function () { return ip.slice()[Symbol.iterator](); },
                get size() { return ip.length; },
                forEach: function (cb, t) { var self = this; ip.forEach(function (n) { cb.call(t, self.getAll(n), n, self); }); }
            };
        }
        /* (the inline style properties set here, each with what it was: put back
         * before each pass, leaving the page's own changes alone) */
        var touched = [];
        function put(el, css) {
            if (!el.__cbLay) { el.__cbLay = []; touched.push(el); }
            for (var k in css) {
                el.__cbLay.push([k, el.style.getPropertyValue(k), el.style.getPropertyPriority(k)]);
                el.style.setProperty(k, css[k], 'important');
            }
        }
        function restore(el) {
            var rec = el.__cbLay;
            if (!rec) return;
            for (var i = rec.length - 1; i >= 0; i--) {
                if (rec[i][1]) el.style.setProperty(rec[i][0], rec[i][1], rec[i][2]);
                else el.style.removeProperty(rec[i][0]);
            }
            delete el.__cbLay;
        }
        function restoreAll() {
            var dn = global.document ? global.document.querySelectorAll('*') : [];
            for (var di = 0; di < dn.length; di++) if (dn[di].__cbLayDone) delete dn[di].__cbLayDone;
            touched.forEach(restore);
            touched = [];
            var doc = global.document;
            if (doc) Array.prototype.slice.call(doc.querySelectorAll('span[data-cyber-anon]')).forEach(function (sp) {
                var p = sp.parentNode;
                if (!p) return;
                while (sp.firstChild) p.insertBefore(sp.firstChild, sp);
                p.removeChild(sp);
            });
        }
        /* (a size given as a border-box one, in the element's own box-sizing) */
        function boxSize(cs, sz, edge) { return px(cs.boxSizing === 'border-box' ? sz : Math.max(0, sz - edge)); }
        function px(n) { return n + 'px'; }
        function num(v, d) { v = Number(v); return v === v && v !== undefined && isFinite(v) ? v : d; }
        function modeOf(cs) {
            var wm = String(cs.writingMode || 'horizontal-tb');
            return { vert: /^(vertical|sideways)/.test(wm), lr: /-lr$/.test(wm), rtl: cs.direction === 'rtl' };
        }
        /* (the children: in-flow elements and text runs, a run wrapped in a span) */
        function childrenOf(el) {
            var out = [];
            Array.prototype.slice.call(el.childNodes).forEach(function (n) {
                if (n.nodeType === 3) {
                    if (!/\S/.test(n.data)) return;
                    var sp = el.ownerDocument.createElement('span');
                    sp.setAttribute('data-cyber-anon', '');
                    el.insertBefore(sp, n);
                    sp.appendChild(n);
                    out.push(sp);
                } else if (n.nodeType === 1) {
                    var ncs = global.getComputedStyle(n);
                    /* (out-of-flow boxes are the containing block's, not children) */
                    if (ncs.display === 'none' || ncs.position === 'absolute' || ncs.position === 'fixed') return;
                    out.push(n);
                }
            });
            return out;
        }
        function unwrap(el) {
            Array.prototype.slice.call(el.querySelectorAll('span[data-cyber-anon]')).forEach(function (sp) {
                while (sp.firstChild) sp.parentNode.insertBefore(sp.firstChild, sp);
                sp.parentNode.removeChild(sp);
            });
        }
        function edgesOf(el, cs, m) {
            function f(p) { return parseFloat(cs.getPropertyValue(p)) || 0; }
            var t = f('border-top-width') + f('padding-top'), b = f('border-bottom-width') + f('padding-bottom');
            var l = f('border-left-width') + f('padding-left'), r = f('border-right-width') + f('padding-right');
            var sbw = Math.max(0, el.offsetWidth - el.clientWidth - f('border-left-width') - f('border-right-width'));
            var sbh = Math.max(0, el.offsetHeight - el.clientHeight - f('border-top-width') - f('border-bottom-width'));
            /* (scrollbars only where it scrolls) */
            if (!/scroll|auto/.test(cs.overflowY + cs.overflow)) sbw = 0;
            if (!/scroll|auto/.test(cs.overflowX + cs.overflow)) sbh = 0;
            r += sbw; b += sbh;
            var is, ie, bs, be;
            if (!m.vert) { is = m.rtl ? r : l; ie = m.rtl ? l : r; bs = t; be = b; }
            else { is = m.rtl ? b : t; ie = m.rtl ? t : b; bs = m.lr ? l : r; be = m.lr ? r : l; }
            return { inlineStart: is, inlineEnd: ie, blockStart: bs, blockEnd: be, inline: is + ie, block: bs + be };
        }
        /* (the inline and block size of a box, as its parent's writing mode has them) */
        function sizes(el, m) { var w = el.offsetWidth, h = el.offsetHeight; return m.vert ? [h, w] : [w, h]; }
        /* (what the box's sizes depend on: its children hidden, then a small box in it) */
        function probe(el, kids, m, cs) {
            kids.forEach(function (k) { put(k, { display: 'none' }); });
            var doc = el.ownerDocument, p = doc.createElement('div');
            p.style.cssText = 'display:block;margin:0;padding:0;border:0;width:0;height:0;float:none;position:static';
            el.appendChild(p);
            var a = sizes(el, m);
            p.style.width = '7px'; p.style.height = '7px';
            var b = sizes(el, m), fill = 0;
            if (a[0] !== b[0]) {
                /* (shrink-to-fit: the inline space it has, which many 1px pieces fill) */
                p.style.cssText = 'display:block;margin:0;padding:0;border:0;font-size:0;line-height:0;position:static;float:none;' +
                    (m.vert ? 'writing-mode:' + cs.writingMode : '');
                p.innerHTML = new Array(1601).join('<i style="display:inline-block;width:1px;height:1px"></i> ');
                fill = sizes(el, m)[0];
            }
            el.removeChild(p);
            kids.forEach(restore);
            touched = touched.filter(function (t) { return kids.indexOf(t) < 0; });
            return { a: a, inlDep: a[0] !== b[0], blkDep: a[1] !== b[1], fill: fill };
        }
        /* (layoutNextFragment: the child absolutely positioned in its parent, made
         * the percentage size; its inline size fitted to the available one) */
        function measure(el, k, o, m) {
            var ai = Math.max(0, num(o.availableInlineSize, 0)), ab = Math.max(0, num(o.availableBlockSize, 0));
            var pi = num(o.percentageInlineSize, ai), pb = num(o.percentageBlockSize, ab);
            if (pi < 0) pi = ai;
            if (pb < 0) pb = ab;
            var fi = o.fixedInlineSize != null ? num(o.fixedInlineSize, null) : null, fb = o.fixedBlockSize != null ? num(o.fixedBlockSize, null) : null;
            var fw = m.vert ? fb : fi, fh = m.vert ? fi : fb, aw = m.vert ? ab : ai, ah = m.vert ? ai : ab;
            var cw = m.vert ? pb : pi, ch = m.vert ? pi : pb;
            var kept = el.getAttribute('style'), kk = k.getAttribute('style');
            function box(w, h) {
                el.style.setProperty('padding', '0', 'important');
                el.style.setProperty('border-width', '0', 'important');
                el.style.setProperty('box-sizing', 'content-box', 'important');
                el.style.setProperty('min-width', '0', 'important'); el.style.setProperty('max-width', 'none', 'important');
                el.style.setProperty('min-height', '0', 'important'); el.style.setProperty('max-height', 'none', 'important');
                el.style.setProperty('width', px(w), 'important'); el.style.setProperty('height', px(h), 'important');
                el.style.setProperty('overflow', 'visible', 'important');
                if (global.getComputedStyle(el).position === 'static') el.style.setProperty('position', 'relative', 'important');
            }
            k.style.setProperty('position', 'absolute', 'important');
            k.style.setProperty('left', '0', 'important'); k.style.setProperty('top', '0', 'important');
            k.style.setProperty('right', 'auto', 'important'); k.style.setProperty('bottom', 'auto', 'important');
            k.style.setProperty('float', 'none', 'important');
            if (fw != null) { k.style.setProperty('width', px(fw), 'important'); k.style.setProperty('min-width', '0', 'important'); k.style.setProperty('max-width', 'none', 'important'); }
            if (fh != null) { k.style.setProperty('height', px(fh), 'important'); k.style.setProperty('min-height', '0', 'important'); k.style.setProperty('max-height', 'none', 'important'); }
            if (fw != null || fh != null) k.style.setProperty('box-sizing', 'border-box', 'important');
            var kv = modeOf(global.getComputedStyle(k)).vert, ix = kv ? 1 : 0;
            box(cw, ch);
            var s1 = [k.offsetWidth, k.offsetHeight];
            box(cw + 100, ch + 100);
            var s2 = [k.offsetWidth, k.offsetHeight];
            var r = s1;
            if (s1[ix] === s2[ix] && (ix ? fh : fw) == null) {
                /* (not a percentage: shrink-to-fit against the available size) */
                box(aw, ah);
                r = [k.offsetWidth, k.offsetHeight];
                /* (the block size as the percentage size has it) */
                box(cw, ch);
                k.style.setProperty(ix ? 'height' : 'width', px(r[ix]), 'important');
                k.style.setProperty('box-sizing', 'border-box', 'important');
                r = [k.offsetWidth, k.offsetHeight];
            }
            if (kept === null) el.removeAttribute('style'); else el.setAttribute('style', kept);
            if (kk === null) k.removeAttribute('style'); else k.setAttribute('style', kk);
            return m.vert ? { inlineSize: r[1], blockSize: r[0] } : { inlineSize: r[0], blockSize: r[1] };
        }
        function Fragment(sz, data) {
            this.inlineSize = sz.inlineSize; this.blockSize = sz.blockSize;
            this.inlineOffset = 0; this.blockOffset = 0;
            this.data = data; this.breakToken = null;
        }
        function cloneData(d) {
            if (d === undefined) return undefined;
            if (typeof d === 'function' || (typeof global.SharedArrayBuffer === 'function' && d instanceof global.SharedArrayBuffer))
                throw new global.DOMException('The data could not be cloned.', 'DataCloneError');
            return typeof global.structuredClone === 'function' ? global.structuredClone(d) : JSON.parse(JSON.stringify(d));
        }
        /* (a pass's children and fragments are good in that pass only) */
        var passNo = 0;
        function bad() { throw new TypeError('invalid custom layout'); }
        /* (a promise that a callback must return; one still pending after the
         * microtasks have run is a failure) */
        function settle(p) {
            if (!p || typeof p.then !== 'function') bad();
            return Promise.race([p, new Promise(function (r, j) { global.setTimeout(function () { j(new TypeError('pending')); }, 0); })]);
        }
        /* (a custom layout's name, for an element whose display is layout(name)) */
        function customOf(el) {
            var mm = /^(?:inline-)?layout\(\s*([-\w]+)\s*\)$/.exec(String(global.getComputedStyle(el).display));
            return mm && defs[mm[1]] ? defs[mm[1]] : null;
        }
        /* (outer: a parent custom layout's options for this one, when it lays it out) */
        async function layoutOne(el, def, outer) {
            var cs = global.getComputedStyle(el), m = modeOf(cs);
            var kids = childrenOf(el);
            var pr = probe(el, kids, m, cs);
            var edges = edgesOf(el, cs, m);
            var inst = def.inst || (def.inst = new def.cls());
            function childrenFor(pass) {
                return kids.map(function (k) {
                    return {
                        __el: k, __pass: pass,
                        styleMap: mapOf(k, def.cip),
                        intrinsicSizes: async function () {
                            if (this.__pass !== passNo) bad();
                            var cdef = customOf(k);
                            if (cdef) {
                                /* (a child with a custom layout: its class's intrinsic sizes, with its edges) */
                                var kcs = global.getComputedStyle(k), ke = edgesOf(k, kcs, modeOf(kcs));
                                var ci = cdef.inst || (cdef.inst = new cdef.cls());
                                var cr = await settle(ci.intrinsicSizes([], ke, mapOf(k, cdef.ip)));
                                return { minContentSize: Math.max(0, num(cr && cr.minContentSize, 0)) + ke.inline, maxContentSize: Math.max(0, num(cr && cr.maxContentSize, 0)) + ke.inline };
                            }
                            var w = measure(el, k, { availableInlineSize: 0 }, m), x = measure(el, k, { availableInlineSize: 1e6 }, m);
                            return { minContentSize: w.inlineSize, maxContentSize: x.inlineSize };
                        },
                        layoutNextFragment: async function (o) {
                            if (this.__pass !== passNo) bad();
                            o = o || {};
                            var d = cloneData(o.data);
                            /* (a child with a custom layout of its own: laid out by it, its data the fragment's) */
                            var cdef = customOf(k), inner = null;
                            if (cdef) { k.__cbLayDone = true; inner = await layoutOne(k, cdef, { data: d, fixedInlineSize: o.fixedInlineSize, fixedBlockSize: o.fixedBlockSize }); }
                            var f = new Fragment(measure(el, k, o, m), inner ? cloneData(inner.data) : undefined);
                            if (inner && f.data && typeof f.data === 'object') Object.freeze(f.data);
                            Object.defineProperty(f, '__el', { value: k });
                            Object.defineProperty(f, '__pass', { value: passNo });
                            return f;
                        }
                    };
                });
            }
            var styleMap = mapOf(el, def.ip);
            var fixedInline = pr.a[0];
            if (pr.inlDep) {
                /* (shrink-to-fit: the class's intrinsic sizes fitted to the space there is) */
                var mn = 0, mx = 0;
                passNo++;
                var is = await settle(inst.intrinsicSizes(childrenFor(passNo), edges, styleMap));
                if (is !== undefined && (!is || typeof is !== 'object')) bad();
                if (is) { mn = Math.max(0, num(is.minContentSize, 0)); mx = Math.max(0, num(is.maxContentSize, 0)); }
                var avail = Math.max(0, pr.fill - edges.inline);
                fixedInline = Math.max(mn, Math.min(mx, avail)) + edges.inline;
                var o = {};
                o[m.vert ? 'height' : 'width'] = boxSize(cs, fixedInline, edges.inline);
                put(el, o);
                fixedInline = sizes(el, m)[0];
            }
            passNo++;
            var children = childrenFor(passNo);
            var fixedBlock = pr.blkDep ? null : pr.a[1];
            if (outer && outer.fixedInlineSize != null) fixedInline = num(outer.fixedInlineSize, fixedInline);
            if (outer && outer.fixedBlockSize != null) fixedBlock = num(outer.fixedBlockSize, fixedBlock);
            var constraints = {
                fixedInlineSize: fixedInline, fixedBlockSize: fixedBlock,
                availableInlineSize: Math.max(0, fixedInline - edges.inline),
                availableBlockSize: fixedBlock == null ? null : Math.max(0, fixedBlock - edges.block),
                percentageInlineSize: Math.max(0, fixedInline - edges.inline),
                percentageBlockSize: fixedBlock == null ? null : Math.max(0, fixedBlock - edges.block),
                blockFragmentationOffset: null, blockFragmentationType: 'none', data: outer ? outer.data : undefined
            };
            var res = await settle(inst.layout(children, edges, constraints, styleMap, null));
            if (res === undefined || res === null) res = {};
            if (typeof res !== 'object') bad();
            var frags = res.childFragments === undefined ? [] : Array.from(res.childFragments);
            frags.forEach(function (f) { if (!f || f.__pass !== passNo || kids.indexOf(f.__el) < 0) bad(); });
            cloneData(res.data);
            /* (its block size, then its children where they go) */
            if (fixedBlock == null) {
                var ob = {};
                ob[m.vert ? 'width' : 'height'] = boxSize(cs, Math.max(0, num(res.autoBlockSize, 0)), edges.block);
                put(el, ob);
            }
            if (cs.position === 'static') put(el, { position: 'relative' });
            var W = el.offsetWidth, H = el.offsetHeight;
            var bl = parseFloat(cs.borderLeftWidth) || 0, bt = parseFloat(cs.borderTopWidth) || 0;
            var placed = [];
            frags.forEach(function (f) {
                if (!f || !f.__el) return;
                var k = f.__el, io = num(f.inlineOffset, 0), bo = num(f.blockOffset, 0), inl = f.inlineSize, blk = f.blockSize;
                var x, y, w = m.vert ? blk : inl, h = m.vert ? inl : blk;
                if (!m.vert) { x = m.rtl ? W - io - inl : io; y = bo; }
                else { x = m.lr ? bo : W - bo - blk; y = m.rtl ? H - io - inl : io; }
                placed.push(k);
                put(k, { position: 'absolute', margin: '0', left: px(x - bl), top: px(y - bt), right: 'auto', bottom: 'auto',
                         width: px(w), height: px(h), 'box-sizing': 'border-box', 'min-width': '0', 'max-width': 'none',
                         'min-height': '0', 'max-height': 'none', float: 'none' });
            });
            kids.forEach(function (k) { if (placed.indexOf(k) < 0) put(k, { display: 'none' }); });
            return res;
        }
        var busy = false, mo = null, queued = false;
        async function layoutAll() {
            var doc = global.document;
            if (!doc || busy) return;
            busy = true;
            try { await layoutAll1(doc); } finally { if (mo) mo.takeRecords(); busy = false; }
        }
        async function layoutAll1(doc) {
            restoreAll();
            var els = doc.querySelectorAll('*');
            for (var i = 0; i < els.length; i++) {
                var el = els[i], d = String(global.getComputedStyle(el).display);
                var mm = /^(?:inline-)?layout\(\s*([-\w]+)\s*\)$/.exec(d);
                if (!mm || !defs[mm[1]] || el.__cbLayDone) continue;
                var before = touched.length;
                try { await layoutOne(el, defs[mm[1]]); }
                catch (e) {
                    /* (a layout that fails: the box as flow layout has it) */
                    touched.slice(before).forEach(restore);
                    touched = touched.slice(0, before);
                    unwrap(el);
                }
            }
        }
        /* (module text: its imports' exports put before it) */
        function fetchText(url) {
            var reg = global.__blobRegistry, b = reg && reg[url];
            return b && typeof b.text === 'function' ? b.text() : global.fetch(url).then(function (r) { return r.text(); });
        }
        async function moduleCode(code, url) {
            var re = /^\s*import\s+(?:\{[^}]*\}|\*\s+as\s+\w+|\w+)\s+from\s+['"]([^'"]+)['"]\s*;?/gm, mm, pre = '';
            var deps = [];
            while ((mm = re.exec(code))) deps.push(mm[1]);
            for (var i = 0; i < deps.length; i++) {
                var u = deps[i];
                try { u = new global.URL(u, url || global.location.href).href; } catch (e) { }
                var t = await fetchText(u);
                pre += String(t).replace(/^\s*export\s+default\s+/gm, '').replace(/^\s*export\s+(?=function|const|let|var|class|async)/gm, '')
                                .replace(/^\s*export\s*\{[^}]*\}\s*;?/gm, '') + '\n';
            }
            return pre + code.replace(re, '');
        }
        var worklet = { addModule: function addModule(url) {
            url = String(url);
            var abs = url;
            try { abs = new global.URL(url, global.location.href).href; } catch (e) { }
            return fetchText(url).then(function (code) { return moduleCode(String(code), abs); }).then(function (code) {
                (new Function('registerLayout', code))(registerLayout);
                /* (laid out again on the page's own DOM and style changes) */
                if (!mo && typeof global.MutationObserver === 'function' && global.document) {
                    mo = new global.MutationObserver(function () {
                        if (busy || queued) return;
                        queued = true;
                        global.requestAnimationFrame(function () { queued = false; layoutAll(); });
                    });
                    mo.observe(global.document, { childList: true, subtree: true, attributes: true, characterData: true });
                }
                return layoutAll();
            }).then(function () { return undefined; });
        } };
        Object.defineProperty(global.CSS, 'layoutWorklet', { configurable: true, enumerable: true, get: function () { return worklet; } });
    })();
    if (global.CSS && !global.CSS.parseStylesheet) {
        [['parseStylesheet', true], ['parseRuleList', true], ['parseRule', true], ['parseDeclarationList', true],
         ['parseDeclaration', false], ['parseValue', false], ['parseValueList', false], ['parseCommaValueList', false]].forEach(function (p) {
            var fn = function (input) {
                if (arguments.length < 1) {
                    var e = new TypeError("Failed to execute '" + p[0] + "' on 'CSS': 1 argument required, but only 0 present.");
                    if (p[1]) return Promise.reject(e);
                    throw e;
                }
                if (p[1]) return Promise.resolve([]);
                return p[0] === 'parseValue' || p[0] === 'parseDeclaration' ? null : [];
            };
            Object.defineProperty(fn, 'name', { configurable: true, value: p[0] });
            Object.defineProperty(global.CSS, p[0], { configurable: true, enumerable: true, writable: true, value: fn });
        });
    }
    /* (an @page rule's declarations: CSSPageDescriptors, with the page's descriptors its own) */
    var PAGE_DESC = ['margin', 'margin-top', 'margin-right', 'margin-bottom', 'margin-left', 'size', 'page-orientation', 'marks', 'bleed'];
    function pageDescriptorsOf(style) {
        var PD = global.CSSPageDescriptors, base = Object.getPrototypeOf(style);
        if (typeof PD !== 'function' || !PD.prototype || !style || typeof style !== 'object') return;
        var P = PD.prototype;
        if (!P.__cyberAdopted) {
            var SD = global.CSSStyleDeclaration && global.CSSStyleDeclaration.prototype;
            Object.getOwnPropertyNames(P).forEach(function (k) { if (k !== 'constructor') { try { delete P[k]; } catch (e) { /* (kept) */ } } });
            if (SD) Object.setPrototypeOf(P, SD);
            PAGE_DESC.forEach(function (n) {
                [n, n.replace(/-([a-z])/g, function (m, c) { return c.toUpperCase(); })].forEach(function (k) {
                    var d = null;
                    for (var o = base; o && !d; o = Object.getPrototypeOf(o)) d = Object.getOwnPropertyDescriptor(o, k);
                    /* (a page's own descriptor: through its declarations by name) */
                    if (!d) {
                        var gf = function () { return this.getPropertyValue(n); }, sf = function (v) { this.setProperty(n, String(v)); };
                        Object.defineProperty(gf, 'name', { value: 'get ' + k });
                        Object.defineProperty(sf, 'name', { value: 'set ' + k });
                        d = { get: gf, set: sf, configurable: true, enumerable: true };
                    }
                    Object.defineProperty(P, k, d);
                });
            });
            Object.defineProperty(P, Symbol.toStringTag, { value: 'CSSPageDescriptors', configurable: true });
            Object.defineProperty(P, '__cyberAdopted', { value: true });
        }
        Object.setPrototypeOf(style, P);
    }
    /* (CSSStyleProperties: the declarations of elements and style rules, a CSSStyleDeclaration
     * with the properties; idl_shape.c calls this once its interfaces exist) */
    Object.defineProperty(global, '__cyberStylePropsAdopt', { configurable: true, value: function () {
        var SP = global.CSSStyleProperties, D = global.__CSSStyleDeclarationProto, SD = global.CSSStyleDeclaration;
        if (typeof SP !== 'function' || !SP.prototype || !D || SP.prototype === D || D.__cyberAdopted) return;
        var P = SP.prototype;
        Object.getOwnPropertyNames(P).forEach(function (k) { if (k !== 'constructor') { try { delete P[k]; } catch (e) { /* (kept) */ } } });
        Object.getOwnPropertySymbols(P).forEach(function (k) { try { delete P[k]; } catch (e) { /* (kept) */ } });
        Object.setPrototypeOf(P, D);
        if (typeof SD === 'function') Object.setPrototypeOf(SP, SD);
        Object.defineProperty(P, Symbol.toStringTag, { value: 'CSSStyleProperties', configurable: true });
        Object.defineProperty(P, '__cyberAdopted', { value: true });
        global.__CSSStyleDeclarationProto = P;
    } });
    if (global.__CSSStyleDeclarationProto && !global.__CSSStyleDeclarationProto[Symbol.iterator])
        Object.defineProperty(global.__CSSStyleDeclarationProto, Symbol.iterator, { value: Array.prototype.values, writable: true, configurable: true });

    /* ==================================================================
     * Rules
     * ================================================================== */

    function CSSRule() { throw new TypeError('Illegal constructor'); }
    var RULE_TYPES = { style: 1, charset: 2, import: 3, media: 4, 'font-face': 5, page: 6, keyframes: 7,
                       keyframe: 8, margin: 9, namespace: 10, 'counter-style': 11, supports: 12, 'font-feature-values': 14 };
    ['STYLE_RULE', 'CHARSET_RULE', 'IMPORT_RULE', 'MEDIA_RULE', 'FONT_FACE_RULE', 'PAGE_RULE', 'KEYFRAMES_RULE',
     'KEYFRAME_RULE', 'MARGIN_RULE', 'NAMESPACE_RULE', 'COUNTER_STYLE_RULE', 'SUPPORTS_RULE', 'DOCUMENT_RULE',
     'FONT_FEATURE_VALUES_RULE'].forEach(function (n, k) {
        var v = k + 1;
        if (n === 'MARGIN_RULE') v = 9;
        Object.defineProperty(CSSRule, n, { value: v, enumerable: true });
        Object.defineProperty(CSSRule.prototype, n, { value: v, enumerable: true });
    });
    getter(CSSRule.prototype, 'parentRule', function () { return this.__parentRule || null; });
    /* (a rule taken out: no sheet and no parent any more) */
    function detachRule(r) {
        if (!r || typeof r !== 'object') return;
        try { r.__sheet = null; r.__parentRule = null; } catch (e) { /* (kept) */ }
    }
    getter(CSSRule.prototype, 'parentStyleSheet', function () {
        for (var r = this; r; r = r.__parentRule) if (r.__sheet) return r.__sheet;
        return null;
    });
    getter(CSSRule.prototype, 'type', function () { return RULE_TYPES[this.__kind] || 0; });
    getter(CSSRule.prototype, 'cssText', function () { return this.__text(); }, function () { /* (no effect) */ });

    function subclass(name, parent) {
        var C = illegal(name);
        Object.defineProperty(C, 'name', { configurable: true, value: name });
        Object.defineProperty(C, 'prototype', { value: Object.create(parent.prototype), writable: false, enumerable: false, configurable: false });
        define(C.prototype, 'constructor', C);
        Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true });
        Object.setPrototypeOf(C, parent);
        expose(name, C);
        return C;
    }
    var CSSGroupingRule = subclass('CSSGroupingRule', CSSRule);
    /* (a style rule groups the rules nested in it) */
    var CSSStyleRule = subclass('CSSStyleRule', CSSGroupingRule);
    var CSSConditionRule = subclass('CSSConditionRule', CSSGroupingRule);
    var CSSMediaRule = subclass('CSSMediaRule', CSSConditionRule);
    var CSSSupportsRule = subclass('CSSSupportsRule', CSSConditionRule);
    var CSSContainerRule = subclass('CSSContainerRule', CSSConditionRule);
    var CSSLayerBlockRule = subclass('CSSLayerBlockRule', CSSGroupingRule);
    var CSSLayerStatementRule = subclass('CSSLayerStatementRule', CSSRule);
    var CSSViewTransitionRule = subclass('CSSViewTransitionRule', CSSRule);
    getter(CSSViewTransitionRule.prototype, 'navigation', function () { return this.__navigation; });
    getter(CSSViewTransitionRule.prototype, 'types', function () { return Object.freeze(this.__types.slice()); });
    CSSViewTransitionRule.prototype.__text = function () {
        return '@view-transition { ' + (this.__navigation ? 'navigation: ' + this.__navigation + '; ' : '') +
            (this.__types.length ? 'types: ' + this.__types.join(' ') + '; ' : '') + '}';
    };
    var CSSScopeRule = subclass('CSSScopeRule', CSSGroupingRule);
    var CSSStartingStyleRule = subclass('CSSStartingStyleRule', CSSGroupingRule);
    var CSSImportRule = subclass('CSSImportRule', CSSRule);
    var CSSNamespaceRule = subclass('CSSNamespaceRule', CSSRule);
    var CSSFontFaceRule = subclass('CSSFontFaceRule', CSSRule);
    var CSSPageRule = subclass('CSSPageRule', CSSGroupingRule);
    var CSSMarginRule = subclass('CSSMarginRule', CSSRule);
    /* @container's prelude: names and conditions; what's in parentheses may be anything. */
    function containerTerms(text) {
        var out = [], i = 0, n = text.length;
        while (i < n) {
            while (i < n && /\s/.test(text[i])) i++;
            if (i >= n) break;
            var st = i, d = 0;
            while (i < n && (d || !/\s/.test(text[i]))) {
                if (text[i] === '(') d++;
                else if (text[i] === ')') { d--; if (d < 0) return null; if (!d) { i++; break; } }
                i++;
            }
            if (d) return null;
            out.push(text.slice(st, i));
        }
        return out;
    }
    function containerIsTerm(w) { return /^(?:[a-zA-Z_-][\w-]*)?\(/.test(w) && /\)$/.test(w); }
    function containerConditionValid(w) {
        if (!w.length) return false;
        var i = 0;
        if (/^not$/i.test(w[0])) return w.length === 2 && containerIsTerm(w[1]);
        if (!containerIsTerm(w[0])) return false;
        i = 1;
        var op = null;
        while (i < w.length) {
            if (!/^(and|or)$/i.test(w[i])) return false;
            var o = w[i].toLowerCase();
            if (op && op !== o) return false;
            op = o;
            if (i + 1 >= w.length || !containerIsTerm(w[i + 1])) return false;
            i += 2;
        }
        return true;
    }
    /* A @container prelude as it serializes. */
    /* A style() query's text: custom property tests as name: value, ranges spaced,
     * and/or/not in lower case, groups without their padding; anything else as written. */
    function styleQueryText(s) {
        var t = s.replace(/^\s+|\s+$/g, '');
        var close = function (str, k) {
            for (var d = 0, q = k; q < str.length; q++) { if (str[q] === '(') d++; else if (str[q] === ')' && --d === 0) return q; }
            return -1;
        };
        /* (top-level parts: groups, words) */
        var parts = [], k = 0;
        while (k < t.length) {
            while (k < t.length && /\s/.test(t[k])) k++;
            if (k >= t.length) break;
            if (t[k] === '(') {
                var e = close(t, k);
                if (e < 0) return t;
                parts.push({ g: t.slice(k + 1, e) });
                k = e + 1;
            } else {
                var w = k;
                while (w < t.length && !/\s/.test(t[w]) && t[w] !== '(') w++;
                parts.push({ w: t.slice(k, w) });
                k = w;
            }
        }
        var logic = parts.length > 1 && parts.every(function (p) { return p.g !== undefined || /^(and|or|not)$/i.test(p.w); });
        if (logic || (parts.length === 1 && parts[0].g !== undefined)) {
            return parts.map(function (p) {
                if (p.w !== undefined) return p.w.toLowerCase();
                var inner = p.g.replace(/^\s+|\s+$/g, '');
                /* (a test of something not a custom property: as written) */
                if (/^[A-Za-z]/.test(inner) && !/^(not)\b/i.test(inner) && inner.indexOf('(') < 0) return '(' + p.g + ')';
                return '(' + styleQueryText(p.g) + ')';
            }).join(' ');
        }
        /* (a range: its parts and operators, one space apart) */
        if (/[<>=]/.test(t) && !/^--[^:]*:/.test(t)) {
            var toks = t.split(/\s*(<=|>=|<|>|=)\s*/);
            return toks.map(function (x) { return x.replace(/^\s+|\s+$/g, ''); }).join(' ');
        }
        /* (a declaration: name: value, the value's own spaces kept within it) */
        var c = -1;
        for (var q = 0, d = 0; q < t.length; q++) {
            if (t[q] === '\\') { q++; continue; }
            if (t[q] === '(') d++; else if (t[q] === ')') d--;
            else if (t[q] === ':' && !d) { c = q; break; }
        }
        if (c >= 0 && /^--/.test(t)) return t.slice(0, c).replace(/\s+$/, '') + ': ' + t.slice(c + 1).replace(/^\s+/, '');
        return t;
    }
    function containerCondText(text) {
        var parts = [], d = 0, cur = '';
        for (var k = 0; k < text.length; k++) {
            var c = text[k];
            if (c === '(') d++; else if (c === ')') d--;
            if (c === ',' && !d) { parts.push(cur); cur = ''; } else cur += c;
        }
        parts.push(cur);
        var low = function (m) { return m.toLowerCase(); };
        var one = function (seg) {
            return seg.replace(/\s+/g, ' ').replace(/\(\s+/g, '(').replace(/\s+\)/g, ')')
                .replace(/\s*(<=|>=|<|>|=)\s*/g, ' $1 ').replace(/\s*:\s*/g, ': ')
                .replace(/\([A-Za-z-]+(?=\)|:| [<>=])/g, low)
                .replace(/[<>=] [A-Za-z-]+(?=\)| [<>=])/g, low);
        };
        return parts.map(function (part) {
            /* (what's in style() goes as written) */
            var out = '', t = part.trim(), i = 0;
            while (i < t.length) {
                var rest = t.slice(i), at = rest.search(/(^|[^\w-])(style|scroll-state)\(/i);
                if (at < 0) { out += one(rest); break; }
                if (!/^(style|scroll-state)\(/i.test(rest.slice(at))) at++;
                var ss = /^scroll-state\(/i.test(rest.slice(at)), fl = ss ? 13 : 6;
                out += one(t.slice(i, i + at));
                var j = i + at + fl, depth = 1;
                while (j < t.length && depth) { if (t[j] === '(') depth++; else if (t[j] === ')') depth--; j++; }
                var inner = t.slice(i + at + fl, j - 1);
                if (ss) {
                    /* (scroll-state(): keywords all, lower case and spaced; one that isn't a query, as written) */
                    var n = one(inner).trim().replace(/\(\s+/g, '(').replace(/\s+\)/g, ')');
                    var feat = /^\(?\s*[A-Za-z-]+\s*(:\s*[A-Za-z-]+)?\s*\)?$/;
                    var ok = n.split(/\s+(?:and|or)\s+/i).every(function (x) { return feat.test(x.replace(/^not\s+/i, '')); });
                    out += ok ? 'scroll-state(' + n.toLowerCase() + ')' : t.slice(i + at, j);
                } else {
                    /* (the function's name in lower case; its query normalized) */
                    out += 'style(' + styleQueryText(inner) + ')';
                }
                i = j;
            }
            return out;
        }).join(', ');
    }
    /* (Element.matchContainer's query: its serialization, '' when it isn't one) */
    Object.defineProperty(global, '__cyberContainerQueryText', { configurable: true, value: function (q) {
        q = String(q);
        if (!q.trim() || !containerPreludeValid(q)) return '';
        return containerCondText(q);
    } });
    function containerPreludeValid(prelude) {
        var parts = [], d = 0, cur = '';
        for (var k = 0; k < prelude.length; k++) {
            var c = prelude[k];
            if (c === '(') d++; else if (c === ')') d--;
            if (c === ',' && !d) { parts.push(cur); cur = ''; } else cur += c;
        }
        parts.push(cur);
        for (var q = 0; q < parts.length; q++) {
            var w = containerTerms(parts[q].replace(/\/\*[\s\S]*?\*\//g, ' '));
            if (!w || !w.length) return false;
            /* (a name, if the first word isn't a term or not) */
            if (!containerIsTerm(w[0]) && !/^not$/i.test(w[0])) {
                if (!/^(?:[a-zA-Z_-]|[^\x00-\x7f]|\\.)(?:[\w-]|[^\x00-\x7f]|\\.)*$/.test(w[0])) return false;
                if (/^(and|or|none|default|initial|inherit|unset|revert|revert-layer)$/i.test(w[0])) return false;
                w.shift();
                if (!w.length) continue;
            }
            if (!containerConditionValid(w)) return false;
        }
        return true;
    }
    /* @font-feature-values: the names of feature values, by kind */
    var FFV_KINDS = { 'stylistic': 1, 'historical-forms': 1, 'styleset': 0, 'character-variant': 2, 'swash': 1, 'ornaments': 1, 'annotation': 1 };
    var CSSFontFeatureValuesRule = subclass('CSSFontFeatureValuesRule', CSSRule);
    getter(CSSFontFeatureValuesRule.prototype, 'fontFamily', function () { return this.__family; },
        function (v) { var f = ffvFamily(String(v)); if (f !== null) { this.__family = f; touched(this); } });
    /* (each kind's names: a CSSFontFeatureValuesMap, live over the rule's own list) */
    var FFV_MAP_OF = new WeakMap();
    var CSSFontFeatureValuesMap = global.CSSFontFeatureValuesMap || function CSSFontFeatureValuesMap() { throw new TypeError('Illegal constructor'); };
    var ffvList = function (m) {
        var s = FFV_MAP_OF.get(m);
        if (!s) throw new TypeError('Illegal invocation');
        var maps = s.rule.__maps;
        if (!maps[s.kind]) maps[s.kind] = [];
        return maps[s.kind];
    };
    var ffvAt = function (list, name) { for (var i = 0; i < list.length; i++) if (list[i][0] === name) return i; return -1; };
    (function (P) {
        var m = function (name, fn) { Object.defineProperty(P, name, { configurable: true, enumerable: true, writable: true, value: fn }); };
        Object.defineProperty(P, 'size', { configurable: true, enumerable: true, get: function () { return ffvList(this).length; } });
        m('get', function get(k) { var l = ffvList(this), i = ffvAt(l, String(k)); return i < 0 ? undefined : l[i][1].map(Number); });
        m('has', function has(k) { return ffvAt(ffvList(this), String(k)) >= 0; });
        m('set', function set(k, v) {
            var l = ffvList(this), nums = (Array.isArray(v) ? v : [v]).map(function (x) { return Math.floor(Number(x)); });
            var i = ffvAt(l, String(k));
            if (i < 0) l.push([String(k), nums]); else l[i][1] = nums;
            touched(FFV_MAP_OF.get(this).rule);
            return this;
        });
        m('delete', function (k) {
            var l = ffvList(this), i = ffvAt(l, String(k));
            if (i < 0) return false;
            l.splice(i, 1);
            touched(FFV_MAP_OF.get(this).rule);
            return true;
        });
        m('clear', function clear() { ffvList(this).length = 0; touched(FFV_MAP_OF.get(this).rule); });
        m('forEach', function forEach(cb, thisArg) { var self = this; ffvList(this).slice().forEach(function (e) { cb.call(thisArg, e[1].map(Number), e[0], self); }); });
        m('keys', function keys() { return ffvList(this).map(function (e) { return e[0]; })[Symbol.iterator](); });
        m('values', function values() { return ffvList(this).map(function (e) { return e[1].map(Number); })[Symbol.iterator](); });
        m('entries', function entries() { return ffvList(this).map(function (e) { return [e[0], e[1].map(Number)]; })[Symbol.iterator](); });
        Object.defineProperty(P, Symbol.iterator, { configurable: true, writable: true, value: P.entries });
    })(CSSFontFeatureValuesMap.prototype);
    [['annotation', 'annotation'], ['ornaments', 'ornaments'], ['stylistic', 'stylistic'], ['swash', 'swash'],
     ['characterVariant', 'character-variant'], ['styleset', 'styleset'], ['historicalForms', 'historical-forms']].forEach(function (p) {
        getter(CSSFontFeatureValuesRule.prototype, p[0], function () {
            var cache = this.__ffvMaps || (Object.defineProperty(this, '__ffvMaps', { value: {} }), this.__ffvMaps);
            if (!cache[p[1]]) {
                var o = Object.create(CSSFontFeatureValuesMap.prototype);
                FFV_MAP_OF.set(o, { rule: this, kind: p[1] });
                cache[p[1]] = o;
            }
            return cache[p[1]];
        });
    });
    CSSFontFeatureValuesRule.prototype.__text = function () {
        var maps = this.__maps, out = '@font-feature-values ' + this.__family + ' {';
        /* (each kind where it first came) */
        Object.keys(maps).forEach(function (k) {
            if (!maps[k] || !maps[k].length) return;
            out += ' @' + k + ' {' + maps[k].map(function (e) { return ' ' + e[0] + ': ' + e[1].join(' ') + ';'; }).join('') + ' }';
        });
        return out + ' }';
    };
    function ffvFamily(prelude) {
        var parts = prelude.split(','), out = [];
        for (var i = 0; i < parts.length; i++) {
            var t = parts[i].trim();
            if (/^"[^"]*"$|^'[^']*'$/.test(t)) { out.push(t[0] === "'" ? '"' + t.slice(1, -1) + '"' : t); continue; }
            if (!t || !/^(?:(?:[a-zA-Z_]|[^\x00-\x7f]|\\.)(?:[\w-]|[^\x00-\x7f]|\\.)*\s*)+$/.test(t)) return null;
            if (/^(serif|sans-serif|monospace|cursive|fantasy|system-ui|math|emoji|fangsong|ui-serif|ui-sans-serif|ui-monospace|ui-rounded|initial|inherit|unset|revert|revert-layer|default)$/i.test(t)) return null;
            out.push(t.replace(/\s+/g, ' '));
        }
        return out.length ? out.join(', ') : null;
    }
    function ffvIdent(s) {
        s = s.trim().replace(/\\([0-9a-fA-F]{1,6})\s?/g, function (m, h) { return String.fromCodePoint(parseInt(h, 16)); });
        s = s.replace(/\\([^:\s])/g, '$1');
        if (!/^(?:[a-zA-Z_-]|[^\x00-\x7f]|\\.)(?:[\w-]|[^\x00-\x7f]|\\.)*$/.test(s) || /^--/.test(s)) return null;
        return s;
    }
    function ffvBlock(block, maps) {
        var i = 0, n = block.length;
        while (i < n) {
            while (i < n && /\s/.test(block[i])) i++;
            if (i >= n) break;
            if (block[i] !== '@') {
                var semi = block.indexOf(';', i);
                var brace = block.indexOf('{', i);
                if (brace >= 0 && (semi < 0 || brace < semi)) {
                    var dq = 1, eq = brace + 1;
                    for (; eq < n && dq; eq++) { if (block[eq] === '{') dq++; else if (block[eq] === '}') dq--; }
                    i = eq;
                    continue;
                }
                i = semi < 0 ? n : semi + 1;
                continue;
            }
            var o = i;
            while (o < n && block[o] !== '{' && block[o] !== ';') o++;
            var head = block.slice(i + 1, o).trim().toLowerCase();
            if (o >= n || block[o] === ';') { i = o + 1; continue; }
            var d = 1, e = o + 1;
            for (; e < n && d; e++) { if (block[e] === '{') d++; else if (block[e] === '}') d--; }
            var body = block.slice(o + 1, d ? e : e - 1);
            i = e;
            if (!Object.prototype.hasOwnProperty.call(FFV_KINDS, head)) continue;
            var list = maps[head] || (maps[head] = []), max = FFV_KINDS[head];
            body.split(';').forEach(function (decl) {
                var c = decl.search(/(^|[^\\]):/);
                if (c < 0) return;
                if (decl[c] !== ':') c++;
                var name = ffvIdent(decl.slice(0, c));
                var vals = decl.slice(c + 1).trim();
                if (!name || !/^\d+(\s+\d+)*$/.test(vals)) return;
                var nums = vals.split(/\s+/);
                if (max && nums.length > max) return;
                var was = list.filter(function (x) { return x[0] === name; })[0];
                if (was) was[1] = nums; else list.push([name, nums]);
            });
        }
    }
    /* @font-palette-values */
    var CSSFontPaletteValuesRule = subclass('CSSFontPaletteValuesRule', CSSRule);
    getter(CSSFontPaletteValuesRule.prototype, 'name', function () { return this.__name; });
    getter(CSSFontPaletteValuesRule.prototype, 'fontFamily', function () { return this.__family; });
    getter(CSSFontPaletteValuesRule.prototype, 'basePalette', function () { return this.__base; });
    getter(CSSFontPaletteValuesRule.prototype, 'overrideColors', function () { return this.__colors; });
    CSSFontPaletteValuesRule.prototype.__text = function () {
        var s = '@font-palette-values ' + this.__name + ' {';
        if (this.__family) s += ' font-family: ' + this.__family + ';';
        if (this.__base) s += ' base-palette: ' + this.__base + ';';
        if (this.__colors) s += ' override-colors: ' + this.__colors + ';';
        return s + ' }';
    };
    function paletteColor(t) {
        var probe = global.document && global.document.createElement('div');
        if (!probe) return null;
        probe.style.color = t;
        return probe.style.color || null;
    }
    function paletteBlock(r, block) {
        block.split(';').forEach(function (decl) {
            var c = decl.indexOf(':');
            if (c < 0) return;
            var name = decl.slice(0, c).trim().toLowerCase(), val = decl.slice(c + 1).trim();
            if (/!\s*important\s*$/i.test(val)) return;
            if (name === 'font-family') {
                if (!val || /^(serif|sans-serif|monospace|cursive|fantasy|system-ui|initial|inherit|unset|revert|revert-layer|default)$/i.test(val)) return;
                if (/^(?:"[^"]*"|'[^']*'|[a-zA-Z_][\w-]*(?:\s+[a-zA-Z_][\w-]*)*)(?:\s*,\s*(?:"[^"]*"|'[^']*'|[a-zA-Z_][\w-]*(?:\s+[a-zA-Z_][\w-]*)*))*$/.test(val)) r.__family = val.replace(/'([^']*)'/g, '"$1"');
            } else if (name === 'base-palette') {
                if (/^(light|dark)$/i.test(val)) r.__base = val.toLowerCase();
                else if (/^\+?\d+$/.test(val)) r.__base = String(parseInt(val, 10));
            } else if (name === 'override-colors') {
                var depth = 0, cur = '', parts = [];
                for (var i = 0; i < val.length; i++) {
                    var ch = val[i];
                    if (ch === '(') depth++; else if (ch === ')') depth--;
                    if (ch === ',' && !depth) { parts.push(cur); cur = ''; } else cur += ch;
                }
                parts.push(cur);
                var out = [];
                for (var k = 0; k < parts.length; k++) {
                    var m = /^\s*(\d+)\s+([\s\S]+?)\s*$/.exec(parts[k]);
                    if (!m) return;
                    /* (nothing that depends on where it's used) */
                    if (/currentcolor|^(?:Canvas|ButtonText)$/i.test(m[2])) return;
                    var col = paletteColor(m[2]);
                    if (!col) return;
                    out.push(m[1] + ' ' + col);
                }
                r.__colors = out.join(', ');
            }
        });
    }
    /* unicode-range: its text in the short form, or null */
    function urangeOne(t) {
        t = t.replace(/\/\*[\s\S]*?\*\//g, '').trim();
        var m = /^u\+([0-9a-f?]{1,6})(?:-([0-9a-f]{1,6}))?$/i.exec(t);
        if (!m) return null;
        var lo, hi;
        if (/\?/.test(m[1])) {
            if (m[2] || !/^[0-9a-f]*\?+$/i.test(m[1])) return null;
            lo = parseInt(m[1].replace(/\?/g, '0'), 16);
            hi = parseInt(m[1].replace(/\?/g, 'F'), 16);
        } else {
            lo = parseInt(m[1], 16);
            hi = m[2] ? parseInt(m[2], 16) : lo;
        }
        if (hi > 0x10FFFF || lo > hi) return null;
        return 'U+' + lo.toString(16).toUpperCase() + (hi !== lo ? '-' + hi.toString(16).toUpperCase() : '');
    }
    function urangeCanon(text) {
        var parts = String(text).split(','), out = [];
        for (var i = 0; i < parts.length; i++) {
            var u = urangeOne(parts[i]);
            if (u === null) return null;
            out.push(u);
        }
        return out.join(', ');
    }
    /* A @font-face src list's parts that parse: each a url with its format and tech, or a local name. */
    var SRC_FORMATS = /^(collection|embedded-opentype|opentype|svg|truetype|woff|woff2)$/i;
    var SRC_TECHS = /^(features-opentype|features-aat|features-graphite|color-colrv0|color-colrv1|color-svg|color-sbix|color-cbdt|variations|palettes|incremental)$/i;
    var WIDE_NAME = /^(default|inherit|initial|revert|revert-layer|unset)$/i;
    function fontSrcParts(text) {
        var s = String(text).replace(/\/\*[\s\S]*?\*\//g, ' '), out = [], d = 0, q = '', cur = '', parts = [];
        for (var i = 0; i < s.length; i++) {
            var c = s[i];
            if (q) { if (c === q) q = ''; }
            else if (c === '"' || c === "'") q = c;
            else if (c === '(') d++;
            else if (c === ')') d--;
            if (c === ',' && !d && !q) { parts.push(cur); cur = ''; } else cur += c;
        }
        parts.push(cur);
        parts.forEach(function (p) {
            p = p.trim();
            var lm = /^local\(\s*("[^"]*"|'[^']*'|[^()"']*?)\s*\)$/i.exec(p);
            if (lm) {
                var nm = lm[1];
                if (!nm) return;
                if (nm[0] !== '"' && nm[0] !== "'") {
                    var ids = nm.split(/\s+/);
                    if (!ids.every(function (x) { return /^-?[a-zA-Z_][\w-]*$/.test(x); })) return;
                    if (ids.length === 1 && WIDE_NAME.test(ids[0])) return;
                }
                out.push({ text: p, local: true });
                return;
            }
            var um = /^url\(\s*("[^"]*"|'[^']*'|[^\s"'()]*)\s*\)\s*/i.exec(p);
            if (!um) return;
            var rest = p.slice(um[0].length), fmt = null;
            var fm = /^format\(\s*("[^"]*"|'[^']*'|[-\w]+)\s*\)\s*/i.exec(rest);
            if (fm) {
                fmt = fm[1].replace(/^["']|["']$/g, '');
                if (fm[1][0] !== '"' && fm[1][0] !== "'" && !SRC_FORMATS.test(fm[1])) return;
                rest = rest.slice(fm[0].length);
            }
            var tm = /^tech\(([^()]*)\)\s*/i.exec(rest);
            if (tm) {
                if (!tm[1].split(',').every(function (x) { return SRC_TECHS.test(x.trim()); })) return;
                p = p.replace(tm[0].trim(), tm[0].trim().toLowerCase());
                rest = rest.slice(tm[0].length);
            }
            if (rest) return;
            out.push({ text: p, format: fmt });
        });
        return out;
    }
    Object.defineProperty(global, '__cyberFontSrcParts', { value: fontSrcParts, configurable: true, writable: true, enumerable: false });
    /* (what isn't a property: its own check, and it's kept here) */
    var FACE_DESC = {
        'src': function (v) { var p = fontSrcParts(v); return p.length ? p.map(function (x) { return x.text; }).join(', ') : null; },
        'ascent-override': facePct, 'descent-override': facePct, 'line-gap-override': facePct,
        'size-adjust': function (v) { v = String(v).trim().toLowerCase(); return /^\+?[\d.]+(e[-+]?\d+)?%$/.test(v) ? v.replace(/^\+/, '') : null; }
    };
    /* (one value as the property has it; null if the property won't take it) */
    var faceScratch = null;
    function faceOne(prop, v) {
        var d = global.document;
        if (!d || /^(inherit|initial|unset|revert|revert-layer|default)$/i.test(v)) return null;
        if (!faceScratch) faceScratch = d.createElement('div').style;
        faceScratch.setProperty(prop, '');
        faceScratch.setProperty(prop, v);
        var r = faceScratch.getPropertyValue(prop);
        faceScratch.setProperty(prop, '');
        return r || null;
    }
    function faceWords(v) {
        var out = [], d = 0, cur = '';
        for (var i = 0; i < v.length; i++) {
            var c = v[i];
            if (c === '(') d++;
            else if (c === ')') d--;
            if (/\s/.test(c) && !d) { if (cur) out.push(cur); cur = ''; } else cur += c;
        }
        if (cur) out.push(cur);
        return out;
    }
    /* font-weight and font-stretch in @font-face: auto, or one value or a range of two */
    function faceRange(prop, bad) {
        return function (v) {
            v = String(v).trim();
            if (/^auto$/i.test(v)) return 'auto';
            var w = faceWords(v);
            if (!w.length || w.length > 2) return null;
            var c = w.map(function (x) { return bad.test(x) ? null : faceOne(prop, x); });
            if (c.some(function (x) { return x === null || /\s/.test(x) && !/\(/.test(x); })) return null;
            return c.length === 2 && c[0] === c[1] ? c[0] : c.join(' ');
        };
    }
    function faceStyle(v) {
        v = String(v).trim();
        if (/^(auto|normal|italic|oblique)$/i.test(v)) return v.toLowerCase();
        var w = faceWords(v);
        if (w.length < 2 || w.length > 3 || !/^oblique$/i.test(w[0])) return null;
        var a = w.slice(1).map(function (x) { var c = faceOne('font-style', 'oblique ' + x); return c && /^oblique\s/.test(c) ? c.slice(8).trim() : null; });
        if (a.some(function (x) { return x === null; })) return null;
        if (a.length === 2 && a[0] === a[1]) a.pop();
        if (a.every(function (x) { return parseFloat(x) === 0 && !/\(/.test(x); })) return 'normal';
        return 'oblique ' + a.join(' ');
    }
    FACE_DESC['font-weight'] = faceRange('font-weight', /^(lighter|bolder)$/i);
    FACE_DESC['font-stretch'] = faceRange('font-stretch', /^$/);
    FACE_DESC['font-width'] = FACE_DESC['font-stretch'];
    FACE_DESC['font-style'] = faceStyle;
    function facePct(v) {
        v = String(v).trim().toLowerCase();
        if (v === 'normal') return v;
        return /^\+?[\d.]+(e[-+]?\d+)?%$/.test(v) ? v.replace(/^\+/, '') : null;
    }
    /* A @font-face rule's style, with its unicode-range. */
    function fontFaceStyle(style, block) {
        var range = null, desc = {};
        var m = /(?:^|;)\s*unicode-range\s*:\s*([^;]*)/i.exec(block);
        if (m) range = urangeCanon(m[1]);
        Object.keys(FACE_DESC).forEach(function (k) {
            var re = new RegExp('(?:^|;)\\s*' + k + '\\s*:\\s*((?:[^;"\'()]|"[^"]*"|\'[^\']*\'|\\([^)]*\\))*)', 'gi'), dm;
            while ((dm = re.exec(block))) {
                var ok = FACE_DESC[k](dm[1]);
                if (ok !== null) desc[k] = ok;
            }
        });
        var set = style.setProperty, get = style.getPropertyValue, remove = style.removeProperty;
        /* (font-width is font-stretch, as a descriptor too) */
        var low = function (n) { n = String(n).trim().toLowerCase(); return n === 'font-width' ? 'font-stretch' : n; };
        if (desc['font-width'] !== undefined) { desc['font-stretch'] = desc['font-width']; delete desc['font-width']; }
        ['fontWeight', 'fontStretch', 'fontWidth', 'fontStyle'].forEach(function (k) {
            var n = k.replace(/[A-Z]/g, function (c) { return '-' + c.toLowerCase(); });
            [k, n].forEach(function (key) {
                Object.defineProperty(style, key, { configurable: true, enumerable: true,
                    get: function () { return this.getPropertyValue(n); }, set: function (v) { this.setProperty(n, String(v)); } });
            });
        });
        var is = function (n) { return low(n) === 'unicode-range'; };
        Object.defineProperty(style, 'setProperty', { configurable: true, writable: true, value: function setProperty(n, v, pri) {
            if (low(n) in FACE_DESC) {
                if (String(v).trim() === '') { delete desc[low(n)]; return; }
                var dv = FACE_DESC[low(n)](v);
                if (dv !== null) desc[low(n)] = dv;
                return;
            }
            if (!is(n)) return set.call(this, n, v, pri);
            var c = urangeCanon(v);
            if (c !== null) range = c;
        } });
        Object.defineProperty(style, 'getPropertyValue', { configurable: true, writable: true, value: function getPropertyValue(n) {
            if (low(n) in FACE_DESC) return desc[low(n)] || '';
            return is(n) ? (range === null ? '' : range) : get.call(this, n);
        } });
        Object.defineProperty(style, 'removeProperty', { configurable: true, writable: true, value: function removeProperty(n) {
            if (low(n) in FACE_DESC) { var dw = desc[low(n)] || ''; delete desc[low(n)]; return dw; }
            if (!is(n)) return remove.call(this, n);
            var was = range === null ? '' : range;
            range = null;
            return was;
        } });
        Object.defineProperty(style, '__faceText', { configurable: true, value: function () {
            return Object.keys(desc).map(function (k) { return k + ': ' + desc[k] + ';'; }).join(' ');
        } });
        return style;
    }
    /* @function (css-mixins) */
    var CSSFunctionRule = subclass('CSSFunctionRule', CSSGroupingRule);
    getter(CSSFunctionRule.prototype, 'name', function () { return fnUnescape(this.__name); });
    function fnUnescape(s) {
        return s.replace(/\\([0-9a-fA-F]{1,6})\s?|\\(.)/g, function (m, h, c) { return h ? String.fromCodePoint(parseInt(h, 16)) : c; });
    }
    /* A type as text: type(...) by what's in it. */
    function fnTypeText(t) {
        if (!t) return '*';
        var m = /^type\(([\s\S]*)\)$/i.exec(t.trim());
        return m ? m[1].trim() : t.trim();
    }
    /* What a function's declarations are: its result and locals, in order. */
    var CSSFunctionDeclarations = subclass('CSSFunctionDeclarations', CSSRule);
    getter(CSSFunctionDeclarations.prototype, 'style', function () { return this.__style; });
    CSSFunctionDeclarations.prototype.__text = function () { return this.__style.cssText; };
    function CSSFunctionDescriptors() { throw new TypeError('Illegal constructor'); }
    if (global.__CSSStyleDeclarationProto) {
        CSSFunctionDescriptors.prototype = Object.create(global.__CSSStyleDeclarationProto);
        Object.setPrototypeOf(CSSFunctionDescriptors, global.CSSStyleDeclaration || Function.prototype);
    }
    Object.defineProperty(CSSFunctionDescriptors.prototype, 'constructor', { value: CSSFunctionDescriptors, writable: true, configurable: true });
    Object.defineProperty(CSSFunctionDescriptors.prototype, Symbol.toStringTag, { value: 'CSSFunctionDescriptors', configurable: true });
    Object.defineProperty(global, 'CSSFunctionDescriptors', { value: CSSFunctionDescriptors, writable: true, configurable: true });
    function fnDescSplit(text) {
        var out = [], d = 0, cur = '';
        for (var i = 0; i < text.length; i++) {
            var c = text[i];
            if (c === '(' || c === '{' || c === '[') d++;
            else if (c === ')' || c === '}' || c === ']') d--;
            if (c === ';' && !d) { out.push(cur); cur = ''; } else cur += c;
        }
        out.push(cur);
        return out;
    }
    /* (the order they're first set in; a later one of a name replaces its value) */
    function fnDescSet(o, text) {
        var n = o.__n, v = o.__v;
        fnDescSplit(text).forEach(function (d) {
            var c = d.indexOf(':');
            if (c < 0) return;
            var name = fnUnescape(d.slice(0, c).trim()), val = d.slice(c + 1).trim();
            if (!val || /!\s*important\s*$/i.test(val)) return;
            if (name !== 'result' && !/^--./.test(name)) return;
            var at = n.indexOf(name);
            if (at < 0) { n.push(name); v.push(val); } else v[at] = val;
        });
        fnDescIndex(o);
    }
    function fnDescIndex(o) {
        for (var k = 0; k < o.__n.length; k++) Object.defineProperty(o, k, { value: o.__n[k], enumerable: true, configurable: true });
        for (var q = o.__n.length; Object.prototype.hasOwnProperty.call(o, q); q++) delete o[q];
    }
    function fnDescMake(text, rule) {
        var o = Object.create(CSSFunctionDescriptors.prototype);
        Object.defineProperty(o, '__n', { value: [], writable: true });
        Object.defineProperty(o, '__v', { value: [], writable: true });
        Object.defineProperty(o, '__rule', { value: rule });
        fnDescSet(o, text);
        return o;
    }
    (function (P) {
        function def(name, f) { Object.defineProperty(P, name, { value: f, writable: true, configurable: true, enumerable: true }); }
        function acc(name, g, s) { Object.defineProperty(P, name, { get: g, set: s, configurable: true, enumerable: true }); }
        acc('length', function () { return this.__n.length; });
        def('item', function item(i) { return this.__n[i >>> 0] || ''; });
        def('getPropertyValue', function getPropertyValue(n) { var at = this.__n.indexOf(String(n)); return at < 0 ? '' : this.__v[at]; });
        def('getPropertyPriority', function getPropertyPriority() { return ''; });
        def('setProperty', function setProperty(n, val, pri) { if (!pri) fnDescSet(this, n + ':' + val); });
        def('removeProperty', function removeProperty(n) {
            var at = this.__n.indexOf(String(n));
            if (at < 0) return '';
            var old = this.__v[at];
            this.__n.splice(at, 1); this.__v.splice(at, 1);
            fnDescIndex(this);
            return old;
        });
        acc('cssText', function () {
            var v = this.__v;
            return this.__n.map(function (x, k) { return x + ': ' + v[k] + ';'; }).join(' ');
        }, function (t) { this.__n = []; this.__v = []; fnDescSet(this, String(t)); });
        acc('result', function () { return this.getPropertyValue('result'); }, function (x) { this.setProperty('result', x); });
        def(Symbol.iterator, Array.prototype[Symbol.iterator]);
    })(CSSFunctionDescriptors.prototype);
    /* A function's body: runs of declarations, and conditions holding more. */
    function fnBody(block, parent) {
        var out = [], i = 0, n = block.length, run = '';
        function flush() {
            if (!run.trim()) { run = ''; return; }
            var r = makeRule(CSSFunctionDeclarations, 'function-declarations', { __style: null });
            r.__style = fnDescMake(run, r);
            r.__parent = parent;
            out.push(r);
            run = '';
        }
        while (i < n) {
            if (block[i] === '@' && !run.replace(/[^;]*;/g, '').trim()) {
                flush();
                var o = block.indexOf('{', i);
                if (o < 0) break;
                var d = 1, e = o + 1;
                for (; e < n && d; e++) { if (block[e] === '{') d++; else if (block[e] === '}') d--; }
                var m = /^@([\w-]+)\s*([\s\S]*)$/.exec(block.slice(i, o).trim());
                var inner = block.slice(o + 1, e - 1), kid = null;
                if (m && m[1].toLowerCase() === 'media') kid = makeRule(CSSMediaRule, 'media', { __mediaText: m[2], __media: null, __rules: [] });
                else if (m && m[1].toLowerCase() === 'supports') kid = makeRule(CSSSupportsRule, 'supports', { __cond: m[2].replace(/\s+/g, ' ').trim(), __rules: [] });
                if (kid) { kid.__rules = fnBody(inner, kid); out.push(kid); }
                i = e;
                continue;
            }
            run += block[i++];
        }
        flush();
        return out;
    }
    getter(CSSFunctionRule.prototype, 'returnType', function () { return fnTypeText(this.__returns); });
    define(CSSFunctionRule.prototype, 'getParameters', function () {
        return this.__params.map(function (x) {
            var o = { name: fnUnescape(x.name), type: fnTypeText(x.type) };
            if (x.def !== undefined) o.defaultValue = x.def;
            return o;
        });
    });
    CSSFunctionRule.prototype.__text = function () {
        var ps = this.__params.map(function (x) { return x.name + (x.type ? ' ' + x.type : '') + (x.def !== undefined ? ': ' + x.def : ''); }).join(', ');
        var body = this.__body.replace(/\s+/g, ' ').trim();
        return '@function ' + this.__name + '(' + ps + ')' + (this.__returns && fnTypeText(this.__returns) !== '*' ? ' returns ' + this.__returns : '') + ' {' + (body ? ' ' + body + ' ' : ' ') + '}';
    };
    /* @mixin and what it holds (css-mixins): @layer goes wherever it is */
    function dropLayers(b) {
        var out = '', i = 0, q = 0;
        while (i < b.length) {
            var c = b[i];
            if (q) { out += c; if (c === '\\') { out += b[i + 1] || ''; i++; } else if (c === q) q = 0; i++; continue; }
            if (c === '"' || c === "'") { q = c; out += c; i++; continue; }
            if (c === '@' && /^@layer\b/i.test(b.slice(i, i + 7))) {
                var j = i, d = 0;
                for (; j < b.length; j++) {
                    if (b[j] === '{') d++;
                    else if (b[j] === '}') { if (--d <= 0) { j++; break; } }
                    else if (b[j] === ';' && !d) { j++; break; }
                }
                i = j;
                continue;
            }
            out += c;
            i++;
        }
        return out;
    }
    var CSSMixinRule = subclass('CSSMixinRule', CSSGroupingRule);
    getter(CSSMixinRule.prototype, 'name', function () { return fnUnescape(this.__name); });
    getter(CSSMixinRule.prototype, 'contents', function () { return !!this.__contents; });
    define(CSSMixinRule.prototype, 'getParameters', function () {
        return this.__params.map(function (x) {
            var o = { name: x.name, type: x.type ? fnTypeText(x.type) : '*' };
            if (x.def !== undefined) o.defaultValue = x.def;
            return o;
        });
    });
    CSSMixinRule.prototype.__text = function () {
        var ps = this.__params.map(function (x) { return x.name + (x.type ? ' ' + x.type : '') + (x.def !== undefined ? ': ' + x.def : ''); }).join(', ');
        return '@mixin ' + this.__name + '(' + ps + ') { ' + this.__body.replace(/\s+/g, ' ').trim() + ' }';
    };
    var CSSResultRule = subclass('CSSResultRule', CSSGroupingRule);
    CSSResultRule.prototype.__text = function () { return '@result { ' + this.__body.replace(/\s+/g, ' ').trim() + ' }'; };
    /* @private: variables of the rule it's in */
    var CSSPrivateRule = subclass('CSSPrivateRule', CSSRule);
    CSSPrivateRule.prototype.__text = function () {
        var d = this.__vars;
        return '@private {' + (d.length ? ' ' + d.map(function (v) { return v.name + (v.type ? ' ' + v.type : '') + ': ' + v.value + ';'; }).join(' ') + ' ' : ' ') + '}';
    };
    function privateVars(block) {
        var out = [], parts = [], q = 0, d = 0, at = 0;
        for (var i = 0; i < block.length; i++) {
            var c = block[i];
            if (c === '\\') { i++; continue; }
            if (q) { if (c === q) q = 0; continue; }
            if (c === '"' || c === "'") q = c;
            else if (c === '(' || c === '{' || c === '[') d++;
            else if (c === ')' || c === '}' || c === ']') d--;
            else if (c === ';' && !d) { parts.push(block.slice(at, i)); at = i + 1; }
        }
        parts.push(block.slice(at));
        parts.forEach(function (t) {
            t = t.trim();
            var m = /^(--(?:[^\s:\\]|\\[^\n])+)\s*([^:]*?)\s*:([\s\S]*)$/.exec(t);
            if (!m || /!\s*important\s*$/i.test(m[3])) return;
            var type = m[2].trim(), one = /^type\(\s*(<[^|>]+>)\s*\)$/i.exec(type), value = m[3].trim();
            if (one) type = one[1];
            if (!value) return;
            for (var k = 0; k < out.length; k++) if (out[k].name === m[1]) out.splice(k--, 1);
            out.push({ name: m[1], type: type, value: value });
        });
        return out;
    }
    /* A function's type: one component of a syntax, or type() of any syntax. */
    function fnTypeValid(t) {
        t = t.trim();
        var reg = global.__cyberCssSyntax;
        var m = /^type\(([\s\S]*)\)$/i.exec(t);
        if (m) return !reg || m[1].trim() === '*' || reg.__syntaxValid(m[1].trim());
        return /^(<[a-z-]+>[+#]?|[a-zA-Z_-][\w-]*[+#]?)$/.test(t) && (!reg || reg.__syntaxValid(t));
    }
    /* A default has to be of its type, unless it's only known later. */
    function fnDefaultValid(type, def) {
        var reg = global.__cyberCssSyntax;
        if (!type || !reg || /\b(var|env|if|attr)\(|--[\w-]+\(/i.test(def) || /^(initial|inherit|unset|revert|revert-layer|revert-rule)$/i.test(def)) return true;
        var syn = fnTypeText(type);
        return syn === '*' || reg.__syntaxParse(syn, def) !== null;
    }
    function fnParse(prelude) {
        var m = /^(--(?:[\w-]|[^\x00-\x7f]|\\[0-9a-fA-F]{1,6}\s?|\\.)+)\(([\s\S]*)$/.exec(prelude.trim());
        if (!m) return null;
        var d = 1, i = 0, s = m[2];
        for (; i < s.length && d; i++) { if (s[i] === '(') d++; else if (s[i] === ')') d--; }
        if (d) return null;
        var inside = s.slice(0, i - 1), rest = s.slice(i).trim(), ret = null;
        if (rest) {
            var r = /^returns\s+([\s\S]+)$/i.exec(rest);
            if (!r || !fnTypeValid(r[1])) return null;
            ret = r[1].trim();
        }
        var params = [], depth = 0, cur = '', parts = [];
        for (var k = 0; k < inside.length; k++) {
            var c = inside[k];
            if (c === '(' || c === '{' || c === '[') depth++;
            else if (c === ')' || c === '}' || c === ']') depth--;
            if (c === ',' && !depth) { parts.push(cur); cur = ''; } else cur += c;
        }
        if (inside.trim() || parts.length) parts.push(cur);
        for (var q = 0; q < parts.length; q++) {
            var pm = /^\s*(--(?:[\w-]|[^\x00-\x7f]|\\[0-9a-fA-F]{1,6}\s?|\\.)+)\s*([^:]*?)\s*(?::([\s\S]*))?$/.exec(parts[q]);
            if (!pm) return null;
            if (pm[2] && !fnTypeValid(pm[2])) return null;
            if (pm[3] !== undefined && (/!/.test(pm[3]) || !fnDefaultValid(pm[2], pm[3].trim()))) return null;
            if (params.some(function (x) { return x.name === pm[1]; })) return null;
            params.push({ name: pm[1], type: pm[2] || null, def: pm[3] === undefined ? undefined : pm[3].trim() });
        }
        return { name: m[1], params: params, returns: ret };
    }
    /* @position-try: its name, and the declarations it may have */
    var CSSPositionTryRule = subclass('CSSPositionTryRule', CSSRule);
    var TRY_ALLOWED = /^(inset|top|right|bottom|left|margin|width|height|min-|max-|block-size|inline-size|align-self|justify-self|place-self|position-anchor|position-area)/;
    function tryAllowed(p) { return TRY_ALLOWED.test(String(p).toLowerCase()) && !/^(margin-trim)$/.test(p); }
    function CSSPositionTryDescriptors() { throw new TypeError('Illegal constructor'); }
    var DECL_PROTO = global.__CSSStyleDeclarationProto;
    if (DECL_PROTO) {
        CSSPositionTryDescriptors.prototype = Object.create(DECL_PROTO, { constructor: { value: CSSPositionTryDescriptors, writable: true, configurable: true } });
        Object.defineProperty(CSSPositionTryDescriptors.prototype, Symbol.toStringTag, { value: 'CSSPositionTryDescriptors', configurable: true });
        Object.setPrototypeOf(CSSPositionTryDescriptors, global.CSSStyleDeclaration || Function.prototype);
        Object.defineProperty(CSSPositionTryDescriptors.prototype, 'setProperty', { writable: true, configurable: true, enumerable: true, value: function setProperty(name, value, priority) {
            if (!tryAllowed(name) || priority) return;
            return DECL_PROTO.setProperty.call(this, name, value);
        } });
        /* (what it may not have isn't there: no attribute, and setting one does nothing; made with
         * the first @position-try's descriptors, not with every realm, as it's every property) */
        var hyphen = function (n) { return n.replace(/[A-Z]/g, function (c) { return '-' + c.toLowerCase(); }).replace(/^webkit-/, '-webkit-'); };
        var tryShadowed = false;
        var tryShadow = function () {
            if (tryShadowed) return;
            tryShadowed = true;
            var none = { configurable: true, enumerable: false, get: function () { return undefined; }, set: function () {} };
            Object.getOwnPropertyNames(DECL_PROTO).forEach(function (n) {
                var d = Object.getOwnPropertyDescriptor(DECL_PROTO, n);
                if (!d || !d.get || /^(cssText|length|parentRule|cssFloat)$/.test(n)) return;
                if (tryAllowed(n.indexOf('-') >= 0 ? n : hyphen(n))) return;
                if (!Object.prototype.hasOwnProperty.call(CSSPositionTryDescriptors.prototype, n))
                    Object.defineProperty(CSSPositionTryDescriptors.prototype, n, none);
            });
        };
        var textDesc = Object.getOwnPropertyDescriptor(DECL_PROTO, 'cssText');
        if (textDesc && textDesc.set) Object.defineProperty(CSSPositionTryDescriptors.prototype, 'cssText', { configurable: true, enumerable: true,
            get: textDesc.get,
            set: function (v) {
                var kept = String(v).split(';').filter(function (d) {
                    var c = d.indexOf(':');
                    return c > 0 && tryAllowed(d.slice(0, c).trim()) && !/!\s*important\s*$/i.test(d);
                }).join(';');
                textDesc.set.call(this, kept);
            } });
        Object.defineProperty(global, 'CSSPositionTryDescriptors', { value: CSSPositionTryDescriptors, writable: true, configurable: true });
    }
    /* A dashed name as written (escapes and all). */
    getter(CSSPositionTryRule.prototype, 'name', function () { return this.__name; });
    getter(CSSPositionTryRule.prototype, 'style', function () { return this.__style; });
    CSSPositionTryRule.prototype.__text = function () {
        var d = this.__style.cssText;
        return '@position-try ' + this.__name + ' {' + (d ? ' ' + d + ' ' : ' ') + '}';
    };
    var CSSKeyframesRule = subclass('CSSKeyframesRule', CSSRule);
    var CSSKeyframeRule = subclass('CSSKeyframeRule', CSSRule);
    var CSSPropertyRule = subclass('CSSPropertyRule', CSSRule);
    var CSSCounterStyleRule = subclass('CSSCounterStyleRule', CSSRule);
    var CSSNestedDeclarations = subclass('CSSNestedDeclarations', CSSRule);
    expose('CSSRule', CSSRule);

    function makeRule(C, kind, props) {
        var r = Object.create(C.prototype);
        Object.defineProperty(r, '__kind', { value: kind });
        Object.defineProperty(r, '__parentRule', { value: null, writable: true });
        Object.defineProperty(r, '__sheet', { value: null, writable: true });
        for (var k in props) Object.defineProperty(r, k, { value: props[k], writable: true });
        return r;
    }
    /* A rule's serialization, kept until it (or a rule inside it) changes:
     * layout re-reads a modified sheet's text, mostly unchanged (CSS-in-JS
     * inserts rules one at a time). */
    var RULE_TEXT = Symbol('ruleText');
    function ruleText(r) {
        var t = r[RULE_TEXT];
        if (t === undefined) { t = r.__text(); Object.defineProperty(r, RULE_TEXT, { value: t, configurable: true, writable: true }); }
        return t;
    }
    function touched(r) {
        for (var x = r; x; x = x.__parentRule) {
            if (x[RULE_TEXT] !== undefined) x[RULE_TEXT] = undefined;
            if (x.__sheet) { x.__sheet[SHEET].modified = true; x.__sheet.__changed(); return; }
        }
    }

    /* declarations: the native CSSStyleDeclaration */
    function decls(text, rule) {
        /* (writes through rule.style mark the rule's sheet changed) */
        return global.__cyberStyleDecl(text || '', rule ? function () { touched(rule); } : undefined);
    }
    function declText(d) { return d.cssText; }

    function blockText(prefix, rules, indent) {
        var s = prefix + ' {';
        if (!rules.length) return s + '\n}';
        for (var k = 0; k < rules.length; k++) s += '\n  ' + rules[k].__text().replace(/\n/g, '\n  ');
        return s + '\n}';
    }

    /* the style rule */
    getter(CSSStyleRule.prototype, 'selectorText', function () { return this.__selector; }, function (v) {
        try { this.__selector = parseSelectorList(String(v)); touched(this); } catch (e) { /* (ignored) */ }
    });
    getter(CSSStyleRule.prototype, 'style', function () { return this.__style; }, function (v) {
        this.__style.cssText = String(v);
        touched(this);
    });
    getter(CSSStyleRule.prototype, 'cssRules', function () { return ruleList(this.__rules || (this.__rules = []), this); });
    CSSStyleRule.prototype.__text = function () {
        var d = declText(this.__style);
        var nested = this.__rules && this.__rules.length;
        if (!nested) return this.__selector + ' {' + (d ? ' ' + d + ' ' : ' ') + '}';
        var s = this.__selector + ' {';
        if (d) s += '\n  ' + d;
        for (var k = 0; k < this.__rules.length; k++) s += '\n  ' + this.__rules[k].__text().replace(/\n/g, '\n  ');
        return s + '\n}';
    };
    /* (a style rule holds rules too) */
    define(CSSStyleRule.prototype, 'insertRule', function (rule, index) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'insertRule': 1 argument required");
        var k = insertInto(this.__rules || (this.__rules = []), String(rule), index === undefined ? 0 : index >>> 0, true, this);
        changedList(this);
        touched(this);
        return k;
    });
    define(CSSStyleRule.prototype, 'deleteRule', function (index) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'deleteRule' on 'CSSStyleRule': 1 argument required");
        index = index >>> 0;
        var rules = this.__rules || [];
        if (index >= rules.length) throw domException("Failed to execute 'deleteRule': index out of range.", 'IndexSizeError');
        detachRule(rules.splice(index, 1)[0]);
        changedList(this);
        touched(this);
    });
    /* grouping rules: cssRules, insertRule, deleteRule */
    getter(CSSGroupingRule.prototype, 'cssRules', function () { return ruleList(this.__rules, this); });
    define(CSSGroupingRule.prototype, 'insertRule', function (rule, index) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'insertRule': 1 argument required");
        var k = insertInto(this.__rules, String(rule), index === undefined ? 0 : index >>> 0, true, this);
        changedList(this);
        touched(this);
        return k;
    });
    define(CSSGroupingRule.prototype, 'deleteRule', function (index) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'deleteRule' on 'CSSGroupingRule': 1 argument required");
        index = index >>> 0;
        if (index >= this.__rules.length) throw domException("Failed to execute 'deleteRule': index out of range.", 'IndexSizeError');
        detachRule(this.__rules.splice(index, 1)[0]);
        changedList(this);
        touched(this);
    });
    getter(CSSConditionRule.prototype, 'conditionText', function () { return this.__condition(); });
    getter(CSSMediaRule.prototype, 'media', function () {
        var self = this;
        return this.__media || (this.__media = newMediaList(this.__mediaText, function () { self.__mediaText = self.__media.mediaText; touched(self); }));
    }, function (v) { this.media.mediaText = v; });
    CSSMediaRule.prototype.__condition = function () { return this.__media ? this.__media.mediaText : splitMedia(this.__mediaText).join(', '); };
    CSSMediaRule.prototype.__text = function () { return blockText('@media ' + this.__condition(), this.__rules); };
    CSSSupportsRule.prototype.__condition = function () { return this.__cond; };
    /* (whether its condition holds, now) */
    getter(CSSMediaRule.prototype, 'matches', function () {
        try { return !!global.matchMedia(this.__condition()).matches; } catch (e) { return false; }
    });
    getter(CSSSupportsRule.prototype, 'matches', function () {
        try { return !!global.CSS.supports(this.__cond); } catch (e) { return false; }
    });
    CSSSupportsRule.prototype.__text = function () { return blockText('@supports ' + this.__cond, this.__rules); };
    /* (each of its comma-separated conditions: a name and a query) */
    getter(CSSContainerRule.prototype, 'conditions', function () {
        return Object.freeze([{ name: this.containerName, query: this.containerQuery }]);
    });
    getter(CSSContainerRule.prototype, 'containerName', function () { return this.__cname || ''; });
    getter(CSSContainerRule.prototype, 'containerQuery', function () { return this.__cquery || ''; });
    CSSContainerRule.prototype.__condition = function () { return this.__cond; };
    CSSContainerRule.prototype.__text = function () { return blockText('@container ' + this.__cond, this.__rules); };
    getter(CSSLayerBlockRule.prototype, 'name', function () { return this.__name; });
    CSSLayerBlockRule.prototype.__text = function () { return blockText('@layer' + (this.__name ? ' ' + this.__name : ''), this.__rules); };
    getter(CSSLayerStatementRule.prototype, 'nameList', function () { return Object.freeze(this.__names.slice()); });
    CSSLayerStatementRule.prototype.__text = function () { return '@layer ' + this.__names.join(', ') + ';'; };
    CSSStartingStyleRule.prototype.__text = function () { return blockText('@starting-style', this.__rules); };
    getter(CSSScopeRule.prototype, 'start', function () { return this.__start || null; });
    getter(CSSScopeRule.prototype, 'end', function () { return this.__end || null; });
    CSSScopeRule.prototype.__text = function () {
        var p = '@scope';
        if (this.__start) p += ' (' + this.__start + ')';
        if (this.__end) p += ' to (' + this.__end + ')';
        return blockText(p, this.__rules);
    };
    getter(CSSImportRule.prototype, 'href', function () { return this.__href; });
    getter(CSSImportRule.prototype, 'media', function () { return this.__media || (this.__media = newMediaList(this.__mediaText)); },
        function (v) { this.media.mediaText = v; });
    /* (the sheet it imports: fetched when first asked for; its parent, its rule and its media are the rule's) */
    getter(CSSImportRule.prototype, 'styleSheet', function () {
        if (this.__imported !== undefined) return this.__imported;
        var parent = this.__sheet || null, base = (parent && parent[SHEET] && parent[SHEET].href) || document.baseURI, href;
        if (parent && parent[SHEET] && parent[SHEET].constructed) return (this.__imported = null);
        try { href = new URL(this.__href, base).href; } catch (e) { return (this.__imported = null); }
        var sh = Object.create(CSSStyleSheet.prototype);
        initSheet(sh, null, '', { title: null, href: href });
        sh[SHEET].parent = parent;
        sh[SHEET].ownerRule = this;
        try {
            var x = new XMLHttpRequest();
            x.open('GET', href, false);
            x.send();
            sh[SHEET].text = x.status >= 200 && x.status < 300 ? x.responseText : '';
        } catch (e) { sh[SHEET].text = ''; }
        try { sh[SHEET].origin_clean = new URL(href).origin === location.origin; } catch (e) { /* (kept) */ }
        Object.defineProperty(this, '__imported', { value: sh, writable: true, configurable: true });
        return sh;
    });
    getter(CSSImportRule.prototype, 'layerName', function () { return this.__layer === undefined ? null : this.__layer; });
    getter(CSSImportRule.prototype, 'supportsText', function () { return this.__supports || null; });
    CSSImportRule.prototype.__text = function () {
        var s = '@import url(' + serString(this.__href) + ')';
        if (this.__layer !== undefined) s += this.__layer === '' ? ' layer' : ' layer(' + this.__layer + ')';
        if (this.__supports) s += ' supports(' + this.__supports + ')';
        var m = this.__media ? this.__media.mediaText : splitMedia(this.__mediaText).join(', ');
        if (m) s += ' ' + m;
        return s + ';';
    };
    getter(CSSNamespaceRule.prototype, 'namespaceURI', function () { return this.__uri; });
    getter(CSSNamespaceRule.prototype, 'prefix', function () { return this.__prefix; });
    CSSNamespaceRule.prototype.__text = function () {
        return '@namespace ' + (this.__prefix ? serIdent(this.__prefix) + ' ' : '') + 'url(' + serString(this.__uri) + ');';
    };
    getter(CSSFontFaceRule.prototype, 'style', function () { return this.__style; });
    CSSFontFaceRule.prototype.__text = function () { var d = declText(this.__style), fx = this.__style.__faceText ? this.__style.__faceText() : ''; if (fx) d = d ? d + ' ' + fx : fx; return '@font-face {' + (d ? ' ' + d + ' ' : ' ') + '}'; };
    /* (a page selector list: names and :first, :left, :right, :blank) */
    /* (the pseudo-pages in lower case) */
    function pageSelectorCanon(t) {
        var parts = String(t).trim().split(':');
        for (var k = 1; k < parts.length; k++) parts[k] = parts[k].toLowerCase();
        return parts.join(':');
    }
    function pageSelectorOk(t) {
        t = String(t).trim();
        if (!t) return true;
        return t.split(',').every(function (p) {
            p = p.trim();
            return !!p && /^(?:-?[a-zA-Z_\u0080-\uffff][\w\u0080-\uffff-]*)?(?::(?:first|left|right|blank))*$/i.test(p) && !/^--/.test(p);
        });
    }
    getter(CSSPageRule.prototype, 'selectorText', function () { return this.__selector; }, function (v) {
        if (!pageSelectorOk(v)) return;
        this.__selector = pageSelectorCanon(String(v).trim()); touched(this);
    });
    getter(CSSPageRule.prototype, 'style', function () { return this.__style; }, function (v) { this.__style.cssText = v; });
    CSSPageRule.prototype.__text = function () {
        var d = declText(this.__style);
        /* (its margin rules after its declarations) */
        var m = (this.__rules || []).map(function (x) { return x.__text(); }).join(' ');
        if (m) d = d ? d + ' ' + m : m;
        return '@page' + (this.__selector ? ' ' + this.__selector : '') + ' {' + (d ? ' ' + d + ' ' : ' ') + '}';
    };
    getter(CSSMarginRule.prototype, 'name', function () { return this.__name; });
    getter(CSSMarginRule.prototype, 'style', function () { return this.__style; }, function (v) { this.__style.cssText = v; });
    CSSMarginRule.prototype.__text = function () {
        var d = declText(this.__style);
        return '@' + this.__name + ' {' + (d ? ' ' + d + ' ' : ' ') + '}';
    };
    getter(CSSKeyframesRule.prototype, 'name', function () { return this.__name; }, function (v) { this.__name = String(v); touched(this); });
    getter(CSSKeyframesRule.prototype, 'cssRules', function () { return ruleList(this.__rules, this); });
    getter(CSSKeyframesRule.prototype, 'length', function () { return this.__rules.length; });
    /* (its keyframes by index: rule[i]) */
    for (var kfi = 0; kfi < 256; kfi++) (function (i) {
        Object.defineProperty(CSSKeyframesRule.prototype, i, { configurable: true, get: function () { return this.__rules ? this.__rules[i] : undefined; } });
    })(kfi);
    CSSKeyframesRule.prototype.__text = function () {
        var nm = /^[A-Za-z_-][\w-]*$/.test(this.__name) && !/^(none|initial|inherit|unset|default)$/i.test(this.__name) ? this.__name : serString(this.__name);
        return blockText('@keyframes ' + nm, this.__rules);
    };
    function keyText(t) {
        var parts = t.split(',').map(function (x) {
            x = x.trim().toLowerCase();
            if (x === 'from') return '0%';
            if (x === 'to') return '100%';
            /* (a range of a view timeline's before it; a calc() of a percentage as it is) */
            var rm = /^(normal|cover|contain|entry|exit|entry-crossing|exit-crossing|scroll)\s+(.*)$/.exec(x);
            var range = rm ? rm[1] + ' ' : '';
            if (rm) x = rm[2];
            if (/^calc\(.*%.*\)$/.test(x)) {
                var dd = 0, okc = true;
                for (var ci = 0; ci < x.length; ci++) { if (x[ci] === '(') dd++; else if (x[ci] === ')') { dd--; if (dd < 0) okc = false; } }
                return okc && !dd ? range + x : null;
            }
            if (!/^[+-]?(\d*\.)?\d+%$/.test(x)) return null;
            if (range) { var rv = parseFloat(x); return rv < 0 || rv > 100 ? null : range + rv + '%'; }
            var v = parseFloat(x);
            if (v < 0 || v > 100) return null;
            return v + '%';
        });
        return parts.indexOf(null) >= 0 ? null : parts.join(', ');
    }
    define(CSSKeyframesRule.prototype, 'appendRule', function (text) {
        if (!(this instanceof CSSKeyframesRule)) throw new TypeError('Illegal invocation');
        if (arguments.length < 1) throw new TypeError("Failed to execute 'appendRule' on 'CSSKeyframesRule': 1 argument required");
        var r = parseRules(String(text), 'keyframes', this)[0];
        if (r) { this.__rules.push(r); changedList(this); touched(this); }
    });
    define(CSSKeyframesRule.prototype, 'deleteRule', function (key) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'deleteRule' on 'CSSKeyframesRule': 1 argument required");
        var k = keyText(String(key));
        for (var i = this.__rules.length - 1; i >= 0; i--) if (this.__rules[i].__key === k) { this.__rules.splice(i, 1); changedList(this); touched(this); return; }
    });
    define(CSSKeyframesRule.prototype, 'findRule', function (key) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'findRule' on 'CSSKeyframesRule': 1 argument required");
        var k = keyText(String(key));
        for (var i = this.__rules.length - 1; i >= 0; i--) if (this.__rules[i].__key === k) return this.__rules[i];
        return null;
    });
    getter(CSSKeyframeRule.prototype, 'keyText', function () { return this.__key; }, function (v) {
        var k = keyText(String(v));
        if (k === null) throw new SyntaxError("Failed to set the 'keyText' property on 'CSSKeyframeRule': The key '" + v + "' is invalid and cannot be parsed");
        this.__key = k;
        touched(this);
    });
    getter(CSSKeyframeRule.prototype, 'style', function () { return this.__style; }, function (v) { this.__style.cssText = v; });
    CSSKeyframeRule.prototype.__text = function () { var d = declText(this.__style); return this.__key + ' {' + (d ? ' ' + d + ' ' : ' ') + '}'; };
    getter(CSSPropertyRule.prototype, 'name', function () { return this.__name; });
    getter(CSSPropertyRule.prototype, 'syntax', function () { return this.__descs.syntax || ''; });
    getter(CSSPropertyRule.prototype, 'inherits', function () { return this.__descs.inherits === 'true'; });
    getter(CSSPropertyRule.prototype, 'initialValue', function () { return this.__descs['initial-value'] === undefined ? null : this.__descs['initial-value']; });
    CSSPropertyRule.prototype.__text = function () {
        var d = this.__descs, s = '@property ' + this.__name + ' {';
        ['syntax', 'inherits', 'initial-value'].forEach(function (k) { if (d[k] !== undefined) s += ' ' + k + ': ' + (k === 'syntax' ? serString(d[k]) : d[k]) + ';'; });
        return s + ' }';
    };
    /* @counter-style's descriptors: each checked and put as it's written out */
    function csWords(v) {
        var o = [], m, re = /"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'|[^\s"'(]+\((?:[^()]|\([^()]*\))*\)|[^\s,]+|,/g;
        while ((m = re.exec(v))) o.push(m[0]);
        return o;
    }
    var CS_WIDE = /^(?:initial|inherit|unset|revert|revert-layer|default)$/i;
    function csIdent(w) { return /^-?(?:[a-zA-Z_\u0080-\uffff]|--)[-\w\u0080-\uffff]*$/.test(w) && !CS_WIDE.test(w); }
    function csSymbol(w) { return /^["']/.test(w) || /^(?:url|image-set|-webkit-image-set|linear-gradient|radial-gradient)\(/i.test(w) || csIdent(w); }
    function csInt(w) { return /^[-+]?\d+$/.test(w); }
    function csPair(a, b) {
        /* (<integer> and <symbol>, either way round) */
        var n = csInt(a) ? a : csInt(b) ? b : null, y = n === a ? b : a;
        if (n === null || +n < 0 || !csSymbol(y) || csInt(y)) return null;
        return String(+n) + ' ' + y;
    }
    var CS_DESC = {
        system: function (w) {
            if (w.length === 1 && /^(?:cyclic|numeric|alphabetic|symbolic|additive|fixed)$/i.test(w[0])) return w[0].toLowerCase();
            if (w.length === 2 && /^fixed$/i.test(w[0]) && csInt(w[1])) return 'fixed ' + w[1];
            if (w.length === 2 && /^extends$/i.test(w[0]) && csIdent(w[1]) && !/^none$/i.test(w[1])) return 'extends ' + w[1];
            return null;
        },
        negative: function (w) { return (w.length === 1 || w.length === 2) && w.every(csSymbol) ? w.join(' ') : null; },
        prefix: function (w) { return w.length === 1 && csSymbol(w[0]) ? w[0] : null; },
        suffix: function (w) { return w.length === 1 && csSymbol(w[0]) ? w[0] : null; },
        range: function (w) {
            if (w.length === 1 && /^auto$/i.test(w[0])) return 'auto';
            var out = [], i = 0;
            for (;;) {
                var a = w[i], b = w[i + 1], ok = function (x) { return x !== undefined && (csInt(x) || /^infinite$/i.test(x)); };
                if (!ok(a) || !ok(b)) return null;
                if (csInt(a) && csInt(b) && +a > +b) return null;
                out.push(a.toLowerCase() + ' ' + b.toLowerCase());
                i += 2;
                if (i === w.length) return out.join(', ');
                if (w[i] !== ',') return null;
                i++;
            }
        },
        pad: function (w) { return w.length === 2 ? csPair(w[0], w[1]) : null; },
        fallback: function (w) { return w.length === 1 && csIdent(w[0]) ? w[0] : null; },
        symbols: function (w) { return w.length && w.every(csSymbol) ? w.join(' ') : null; },
        'additive-symbols': function (w) {
            var out = [], last = Infinity;
            for (var i = 0; i < w.length; i += 3) {
                var p = csPair(w[i], w[i + 1]);
                if (!p) return null;
                var n = parseInt(p, 10);
                if (n >= last) return null;
                last = n;
                out.push(p);
                if (i + 2 < w.length && w[i + 2] !== ',') return null;
                if (i + 2 === w.length - 1) return null;
            }
            return out.length ? out.join(', ') : null;
        },
        'speak-as': function (w) { return w.length === 1 && (/^(?:auto|bullets|numbers|words|spell-out)$/i.test(w[0]) || (csIdent(w[0]) && !/^none$/i.test(w[0]))) ? w[0] : null; },
    };
    var CS_ORDER = ['system', 'symbols', 'additive-symbols', 'negative', 'prefix', 'suffix', 'range', 'pad', 'speak-as', 'fallback'];
    function csParse(body) {
        var o = {}, depth = 0, q = 0, at = 0, parts = [];
        for (var i = 0; i < body.length; i++) {
            var c = body[i];
            if (q) { if (c === '\\') i++; else if (c === q) q = 0; continue; }
            if (c === '"' || c === "'") q = c;
            else if (c === '(') depth++;
            else if (c === ')') depth--;
            else if (c === ';' && !depth) { parts.push(body.slice(at, i)); at = i + 1; }
        }
        parts.push(body.slice(at));
        parts.forEach(function (d) {
            var k = d.indexOf(':');
            if (k < 0) return;
            var n = d.slice(0, k).trim().toLowerCase(), f = CS_DESC[n];
            if (!f) return;
            var v = f(csWords(d.slice(k + 1).trim()));
            if (v !== null) o[n] = v;
        });
        return o;
    }
    function csName(v) {
        v = String(v).trim();
        return csIdent(v) && !/^(?:none|decimal|disc|square|circle|disclosure-open|disclosure-closed)$/i.test(v);
    }
    getter(CSSCounterStyleRule.prototype, 'name', function () { return this.__name; }, function (v) {
        if (csName(v)) { this.__name = String(v).trim(); touched(this); }
    });
    [['system', 'system'], ['symbols', 'symbols'], ['additiveSymbols', 'additive-symbols'], ['negative', 'negative'], ['prefix', 'prefix'],
     ['suffix', 'suffix'], ['range', 'range'], ['pad', 'pad'], ['speakAs', 'speak-as'], ['fallback', 'fallback']].forEach(function (m) {
        getter(CSSCounterStyleRule.prototype, m[0], function () { return this.__descs[m[1]] || ''; }, function (v) {
            var r = CS_DESC[m[1]](csWords(String(v).trim()));
            if (r !== null) { this.__descs[m[1]] = r; touched(this); }
        });
    });
    CSSCounterStyleRule.prototype.__text = function () {
        var d = this.__descs, s = '@counter-style ' + this.__name + ' {';
        CS_ORDER.forEach(function (k) { if (d[k] !== undefined) s += ' ' + k + ': ' + d[k] + ';'; });
        return s + ' }';
    };
    CSSCounterStyleRule.prototype.__unused = function () { return '@counter-style ' + this.__name + ' {' + (this.__body ? ' ' + this.__body + ' ' : ' ') + '}'; };
    getter(CSSNestedDeclarations.prototype, 'style', function () { return this.__style; });
    CSSNestedDeclarations.prototype.__text = function () { return declText(this.__style); };

    /* ==================================================================
     * Parsing rules (CSS Syntax 3 "consume a list of rules")
     * ================================================================== */

    /* The top-level parts of text: [{at, prelude, block|null}] (block: the
     * text between the braces, or null for a statement). */
    function splitRules(text) {
        var out = [], i = 0, n = text.length;
        function skipComment() {
            if (text[i] === '/' && text[i + 1] === '*') {
                var e = text.indexOf('*/', i + 2);
                i = e < 0 ? n : e + 2;
                return true;
            }
            return false;
        }
        while (i < n) {
            while (i < n && (isWS(text[i]) || skipComment())) if (isWS(text[i])) i++;
            if (i >= n) break;
            /* <!-- and --> at the top level are ignored */
            if (text.substr(i, 4) === '<!--') { i += 4; continue; }
            if (text.substr(i, 3) === '-->') { i += 3; continue; }
            var start = i, depth = 0, q = null, block = null, prelude = null;
            while (i < n) {
                var c = text[i];
                if (q) {
                    if (c === '\\') { i += 2; continue; }
                    if (c === q || c === '\n') q = null;
                    i++;
                    continue;
                }
                if (skipComment()) continue;
                if (c === '\\') { i += 2; continue; }
                if (c === '"' || c === "'") { q = c; i++; continue; }
                if (c === '\\') { i += 2; continue; }
                if (c === '(' || c === '[') depth++;
                else if ((c === ')' || c === ']') && depth) depth--;
                else if (!depth && c === ';' && text[start] === '@') { prelude = text.slice(start, i); i++; break; }
                else if (!depth && c === '{') {
                    prelude = text.slice(start, i);
                    var bstart = ++i, bd = 1, bq = null;
                    while (i < n && bd) {
                        var d = text[i];
                        if (bq) { if (d === '\\') { i += 2; continue; } if (d === bq || d === '\n') bq = null; i++; continue; }
                        if (d === '/' && text[i + 1] === '*') { var e2 = text.indexOf('*/', i + 2); i = e2 < 0 ? n : e2 + 2; continue; }
                        if (d === '\\') { i += 2; continue; }
                        if (d === '"' || d === "'") bq = d;
                        else if (d === '{') bd++;
                        else if (d === '}') bd--;
                        i++;
                    }
                    block = text.slice(bstart, bd ? i : i - 1);
                    break;
                }
                i++;
            }
            if (prelude === null) { prelude = text.slice(start); if (text[start] !== '@') break; }
            prelude = prelude.trim();
            var m = /^@([A-Za-z-]+)/.exec(prelude);
            out.push({ at: m ? m[1].toLowerCase() : null, prelude: m ? prelude.slice(m[0].length).trim() : prelude, block: block });
        }
        return out;
    }

    /* A style rule's block: its declarations, and nested rules (CSS
     * Nesting): [declText, [rules]]. */
    function splitBlock(block) {
        var decl = '', rules = [], i = 0, n = block.length, start = 0;
        while (i < n) {
            /* a declaration (to ';') or a nested rule (to its block) */
            var j = i, depth = 0, q = null, kind = null;
            while (j < n) {
                var c = block[j];
                if (q) { if (c === '\\') { j += 2; continue; } if (c === q) q = null; j++; continue; }
                if (c === '\\') { j += 2; continue; }
                if (c === '"' || c === "'") { q = c; j++; continue; }
                if (c === '(' || c === '[') depth++;
                else if ((c === ')' || c === ']') && depth) depth--;
                else if (!depth && c === ';') { kind = 'decl'; break; }
                else if (!depth && c === '{') { kind = 'rule'; break; }
                j++;
            }
            if (kind === 'rule') {
                var bd = 1, k = j + 1;
                while (k < n && bd) { if (block[k] === '{') bd++; else if (block[k] === '}') bd--; k++; }
                rules.push(block.slice(i, k));
                i = k;
            } else {
                decl += block.slice(i, kind ? j + 1 : n);
                i = kind ? j + 1 : n;
            }
        }
        return [decl, rules];
    }

    /* rules of text for a context: 'top', 'group', 'keyframes', 'style' */
    function parseRules(text, ctx, parent) {
        /* (a whole sheet: its namespaces from nothing) */
        if (ctx === 'top' && (/@namespace/i.test(text) || text.length > 40)) NS_CTX = { dflt: null, map: Object.create(null) };
        var out = [];
        var parts = splitRules(text);
        for (var k = 0; k < parts.length; k++) {
            var r = buildRule(parts[k], ctx, parent);
            if (r) out.push(r);
        }
        return out;
    }

    /* (rules under a style rule or an @scope may start with a combinator) */
    var relativeOk = 0;
    function nestedUnder(rule) {
        for (var q = rule; q; q = q.__parentRule) if (q.__kind === 'scope' || q.__kind === 'style') return true;
        return false;
    }
    function topCommas(text) {
        var out = [], d = 0, cur = '';
        for (var i = 0; i < text.length; i++) {
            var c = text[i];
            if (c === '\\') { cur += c + (text[++i] || ''); continue; }
            if (c === '(' || c === '[') d++;
            else if (c === ')' || c === ']') d--;
            if (c === ',' && !d) { out.push(cur.trim()); cur = ''; } else cur += c;
        }
        out.push(cur.trim());
        return out;
    }
    function relativeList(text, amp) {
        return topCommas(text).map(function (one) {
            var m = /^([>+~])\s*([\s\S]*)$/.exec(one);
            if (m) return (amp ? '& ' : '') + m[1] + ' ' + parseSelectorList(m[2]);
            /* (under a style rule, what doesn't say & is under it) */
            var text1 = parseSelectorList(one);
            return amp && one.indexOf('&') < 0 ? '& ' + text1 : text1;
        }).join(', ');
    }
    /* @scope's [(start)]? [to (end)]?: [start, end], null if not one. */
    function scopeSelector(text) {
        if (!text.trim() || text.replace(/\[[^\]]*\]/g, '').indexOf('::') >= 0) return false;
        try { relativeList(text.replace(/&/g, '*')); return true; } catch (e) { return false; }
    }
    function scopePrelude(text) {
        var t = text.trim(), i = 0, out = ['', ''];
        var part = function () {
            if (t[i] !== '(') return null;
            var d = 0, j = i;
            for (; j < t.length; j++) {
                if (t[j] === '(') d++;
                else if (t[j] === ')' && --d === 0) break;
            }
            if (j >= t.length) return null;
            var v = t.slice(i + 1, j).trim();
            i = j + 1;
            while (/\s/.test(t[i] || '')) i++;
            return scopeSelector(v) ? v : null;
        };
        if (t[i] === '(') { out[0] = part(); if (out[0] === null) return null; }
        if (/^to(\s|\()/i.test(t.slice(i))) {
            i += 2;
            while (/\s/.test(t[i] || '')) i++;
            out[1] = part();
            if (out[1] === null) return null;
        }
        return i === t.length ? out : null;
    }
    /* A block's declarations up to its first rule, and the rest. */
    function leadingDecls(block) {
        var i = 0, n = block.length, end = 0, kept = "", from = 0;
        while (i < n) {
            var it = blockItem(block, i);
            /* (an at-rule that means nothing here is passed over) */
            if (it.rule && /^\s*@/.test(block.slice(i, it.end)) && !/^\s*@(private|result|media|supports|container|layer|scope|starting-style|font-face|keyframes|-webkit-keyframes|page|property|counter-style|position-try|view-transition|document|-moz-document|mixin|function|contents|apply|font-feature-values|font-palette-values|namespace|import|charset|custom-media)\b/i.test(block.slice(i, it.end))) {
                kept += block.slice(from, i);
                i = end = from = it.end;
                continue;
            }
            if (it.rule) break;
            i = end = it.end;
        }
        return [kept + block.slice(from, end), block.slice(end)];
    }
    /* The declaration or rule at i in a block: where it ends, and which. The block of
     * a custom property's value is its value; strings hold what they hold. */
    function blockItem(block, i) {
        var n = block.length, j = i, depth = 0, q = null;
        while (j < n) {
            var c = block[j];
            if (q) { if (c === '\\') { j += 2; continue; } if (c === q) q = null; j++; continue; }
            if (c === '\\') { j += 2; continue; }
                if (c === '"' || c === "'") { q = c; j++; continue; }
            if (c === '(' || c === '[') depth++;
            else if ((c === ')' || c === ']') && depth) depth--;
            else if (!depth && c === ';') return { rule: false, end: j + 1 };
            else if (!depth && c === '{') {
                var e = braceEnd(block, j);
                var before = block.slice(i, j);
                /* (a value that is or holds a block, not a selector) */
                if (/^\s*--[^\s:{]*\s*:/.test(before) || /^\s*[-\w]+\s*:\s*$/.test(before) || /^\s*[-\w]+\s*:.*var\(/.test(before)) { j = e; continue; }
                return { rule: true, end: e };
            }
            j++;
        }
        return { rule: false, end: n };
    }
    /* text without its comments (those in strings stay) */
    function noComments(t) {
        if (t.indexOf('/*') < 0) return t;
        var o = '', i = 0, n = t.length, q = null;
        while (i < n) {
            var c = t[i];
            if (q) { if (c === '\\' && i + 1 < n) { o += c + t[i + 1]; i += 2; continue; } if (c === q || c === '\n') q = null; o += c; i++; continue; }
            if (c === '/' && t[i + 1] === '*') { var e = t.indexOf('*/', i + 2); i = e < 0 ? n : e + 2; o += ' '; continue; }
            if (c === '\\' && i + 1 < n) { o += c + t[i + 1]; i += 2; continue; }
            if (c === '"' || c === "'") q = c;
            o += c; i++;
        }
        return o;
    }
    function braceEnd(block, open) {
        var d = 1, k = open + 1, q = null, n = block.length;
        while (k < n && d) {
            var c = block[k];
            if (q) { if (c === '\\') { k += 2; continue; } if (c === q) q = null; k++; continue; }
            if (c === '\\') { k += 2; continue; }
            if (c === '"' || c === "'") q = c;
            else if (c === '{') d++;
            else if (c === '}') d--;
            k++;
        }
        return k;
    }
    /* An @scope's block in order: runs of declarations are rules of their own. */
    function nestedDeclarations(text, parent) {
        var r = makeRule(CSSNestedDeclarations, 'nested-declarations', { __style: null });
        r.__style = decls(text, r);
        r.__parentRule = parent;
        return r.__style.length ? r : null;
    }
    /* A group rule's block: as a rule list, or under a style rule or @scope in order. */
    function groupBody(block, r, ctx) {
        if (ctx === 'style') return scopeBody(block, r, 'style');
        if (relativeOk > 0) return scopeBody(block, r, 'group');
        return parseRules(block, 'group', r);
    }
    function scopeBody(block, parent, ctx) {
        var out = [], i = 0, n = block.length, run = '';
        var flush = function () {
            var d = run.trim() ? nestedDeclarations(run, parent) : null;
            if (d) out.push(d);
            run = '';
        };
        while (i < n) {
            var it = blockItem(block, i);
            if (it.rule) {
                flush();
                parseRules(block.slice(i, it.end), ctx || 'group', parent).forEach(function (x) { out.push(x); });
            } else {
                run += block.slice(i, it.end);
            }
            i = it.end;
        }
        flush();
        return out;
    }

    function buildRule(p, ctx, parent) {
        var r = null;
        if (ctx === 'keyframes') {
            if (p.at || p.block === null) return null;
            var key = keyText(p.prelude);
            if (key === null) return null;
            r = makeRule(CSSKeyframeRule, 'keyframe', { __key: key, __style: null });
            r.__style = decls(p.block, r);
        } else if (!p.at) {
            if (p.block === null) return null;
            var sel;
            try { sel = ctx === 'style' || relativeOk > 0 || nestedUnder(parent) ? relativeList(p.prelude, ctx === 'style') : parseSelectorList(p.prelude); } catch (e) { return null; }
            r = makeRule(CSSStyleRule, 'style', { __selector: sel, __style: null, __rules: [] });
            /* (what comes before the first rule inside is its own; later ones are rules of their own) */
            var lead = leadingDecls(p.block);
            r.__style = decls(lead[0], r);
            r.__rules = scopeBody(lead[1], r, 'style');
        } else {
            switch (p.at) {
            case 'media':
                if (p.block === null) return null;
                r = makeRule(CSSMediaRule, 'media', { __mediaText: p.prelude, __media: null, __rules: [] });
                r.__rules = groupBody(p.block, r, ctx);
                break;
            case 'supports':
                if (p.block === null) return null;
                r = makeRule(CSSSupportsRule, 'supports', { __cond: p.prelude.replace(/\s+/g, ' ').trim(), __rules: [] });
                r.__rules = groupBody(p.block, r, ctx);
                break;
            case 'container':
                if (p.block === null || !containerPreludeValid(p.prelude)) return null;
                var cm = /^([A-Za-z_-][\w-]*)\s+(.*)$/.exec(p.prelude);
                var cname = cm && !/^(not|style|scroll-state)$/i.test(cm[1]) && !/^\(/.test(cm[1]) ? cm[1] : '';
                r = makeRule(CSSContainerRule, 'container', { __cond: containerCondText(p.prelude), __cname: cname,
                    __cquery: containerCondText(cname ? cm[2] : p.prelude), __rules: [] });
                r.__rules = groupBody(p.block, r, ctx);
                break;
            case 'layer':
                var names = p.prelude ? p.prelude.split(',').map(function (x) { return x.trim(); }) : [];
                if (names.some(function (x) { return !/^[A-Za-z_-][\w-]*(\.[A-Za-z_-][\w-]*)*$/.test(x); })) return null;
                if (p.block === null) {
                    if (!names.length) return null;
                    r = makeRule(CSSLayerStatementRule, 'layer', { __names: names });
                } else {
                    if (names.length > 1) return null;
                    r = makeRule(CSSLayerBlockRule, 'layer', { __name: names[0] || '', __rules: [] });
                    r.__rules = parseRules(p.block, ctx === 'style' ? 'style' : 'group', r);
                }
                break;
            case 'view-transition':
                if (p.block === null || p.prelude) return null;
                r = makeRule(CSSViewTransitionRule, 'view-transition', { __navigation: '', __types: [] });
                p.block.split(';').forEach(function (d) {
                    var at = d.indexOf(':');
                    if (at < 0) return;
                    var k = d.slice(0, at).trim().toLowerCase(), v = d.slice(at + 1).trim();
                    if (k === 'navigation' && /^(auto|none)$/i.test(v)) r.__navigation = v.toLowerCase();
                    else if (k === 'types') r.__types = /^none$/i.test(v) ? [] : v.split(/\s+/);
                });
                break;
            case 'starting-style':
                if (p.block === null) return null;
                r = makeRule(CSSStartingStyleRule, 'starting-style', { __rules: [] });
                r.__rules = groupBody(p.block, r, ctx);
                break;
            case 'scope':
                if (p.block === null) return null;
                var sm = scopePrelude(p.prelude);
                if (!sm) return null;
                r = makeRule(CSSScopeRule, 'scope', { __start: sm[0], __end: sm[1], __rules: [] });
                relativeOk++;
                r.__rules = scopeBody(p.block, r);
                relativeOk--;
                break;
            case 'import':
                if (ctx !== 'top' || p.block !== null) return null;
                var im = /^(?:url\(\s*(?:"([^"]*)"|'([^']*)'|([^)\s]*))\s*\)|"([^"]*)"|'([^']*)')\s*(.*)$/i.exec(p.prelude);
                if (!im) return null;
                var href = im[1] !== undefined ? im[1] : im[2] !== undefined ? im[2] : im[3] !== undefined ? im[3] : im[4] !== undefined ? im[4] : im[5];
                var rest = im[6] || '', layer, supports;
                var lm = /^layer(?:\(([^)]*)\))?\s*/i.exec(rest);
                if (lm) { layer = lm[1] ? lm[1].trim() : ''; rest = rest.slice(lm[0].length); }
                /* (supports(...): to its own closing parenthesis, however deep) */
                if (/^supports\(/i.test(rest)) {
                    var sd = 0, se = -1;
                    for (var si = 8; si < rest.length; si++) {
                        var sc = rest[si];
                        if (sc === '"' || sc === "'") { var sq = rest.indexOf(sc, si + 1); if (sq < 0) break; si = sq; continue; }
                        if (sc === '(') sd++;
                        else if (sc === ')' && --sd === 0) { se = si; break; }
                    }
                    if (se > 0) { supports = rest.slice(9, se).trim(); rest = rest.slice(se + 1).replace(/^\s+/, ''); }
                }
                r = makeRule(CSSImportRule, 'import', { __href: href, __mediaText: rest.trim(), __media: null, __layer: layer, __supports: supports });
                break;
            case 'namespace':
                if (ctx !== 'top' || p.block !== null) return null;
                var nm = /^(?:([A-Za-z_-][\w-]*)\s+)?(?:url\(\s*(?:"([^"]*)"|'([^']*)'|([^)\s]*))\s*\)|"([^"]*)"|'([^']*)')$/i.exec(p.prelude);
                if (!nm) return null;
                r = makeRule(CSSNamespaceRule, 'namespace', { __prefix: nm[1] || '',
                    __uri: nm[2] !== undefined ? nm[2] : nm[3] !== undefined ? nm[3] : nm[4] !== undefined ? nm[4] : nm[5] !== undefined ? nm[5] : nm[6] });
                if (r.__prefix) NS_CTX.map[r.__prefix] = r.__uri; else NS_CTX.dflt = r.__uri;
                break;
            case 'font-face':
                if (p.block === null || p.prelude) return null;
                r = makeRule(CSSFontFaceRule, 'font-face', { __style: null });
                r.__style = fontFaceStyle(decls(p.block, r), p.block);
                break;
            case 'font-feature-values':
                if (p.block === null) return null;
                var ffam = ffvFamily(p.prelude);
                if (ffam === null) return null;
                r = makeRule(CSSFontFeatureValuesRule, 'font-feature-values', { __family: ffam, __maps: {} });
                ffvBlock(p.block, r.__maps);
                break;
            case 'font-palette-values':
                if (p.block === null || !/^--(?:[\w-]|[^\x00-\x7f]|\\.)*$/.test(p.prelude)) return null;
                r = makeRule(CSSFontPaletteValuesRule, 'font-palette-values', { __name: p.prelude, __family: '', __base: '', __colors: '' });
                paletteBlock(r, p.block);
                break;
            case 'function':
                if (p.block === null) return null;
                var fnp = fnParse(p.prelude);
                if (!fnp) return null;
                r = makeRule(CSSFunctionRule, 'function', { __name: fnp.name, __params: fnp.params, __returns: fnp.returns, __body: p.block, __rules: [] });
                r.__rules = fnBody(p.block, r);
                break;
            case 'mixin':
                if (p.block === null) return null;
                var mxp = fnParse(p.prelude);
                if (!mxp) return null;
                var mxb = dropLayers(p.block);
                r = makeRule(CSSMixinRule, 'mixin', { __name: mxp.name, __params: mxp.params, __body: mxb, __rules: [], __contents: /@contents\b/i.test(mxb) });
                r.__rules = parseRules(mxb, 'mixin', r);
                break;
            case 'result':
                if (p.block === null || ctx !== 'mixin') return null;
                r = makeRule(CSSResultRule, 'result', { __body: p.block, __rules: [] });
                r.__rules = groupBody(p.block, r, ctx);
                break;
            case 'private':
                if (p.block === null || !parent || parent.__kind !== 'style') return null;
                r = makeRule(CSSPrivateRule, 'private', { __vars: privateVars(p.block) });
                break;
            case 'position-try':
                if (p.block === null || !/^--(?:[\w-]|[^\x00-\x7f]|\\.)+$/.test(p.prelude)) return null;
                r = makeRule(CSSPositionTryRule, 'position-try', { __name: p.prelude, __style: null });
                {
                    /* (only what it may have; nothing !important) */
                    var kept = p.block.split(';').filter(function (d) {
                        var c = d.indexOf(':');
                        return c > 0 && tryAllowed(d.slice(0, c).trim()) && !/!\s*important\s*$/i.test(d);
                    }).join(';');
                    r.__style = decls(kept, r);
                    if (DECL_PROTO) { tryShadow(); Object.setPrototypeOf(r.__style, CSSPositionTryDescriptors.prototype); }
                }
                break;
            case 'page':
                if (p.block === null) return null;
                if (!pageSelectorOk(p.prelude)) return null;
                r = makeRule(CSSPageRule, 'page', { __selector: pageSelectorCanon(p.prelude), __style: null, __rules: [] });
                /* (the margin at-rules in it: rules of their own; the rest its declarations) */
                var pdecl = '', pi2 = 0, pb = noComments(p.block);
                while (pi2 < pb.length) {
                    var pit = blockItem(pb, pi2), seg = pb.slice(pi2, pit.end);
                    var mm = pit.rule && /^\s*@([a-z-]+)\s*\{([\s\S]*)\}\s*$/i.exec(seg);
                    if (mm && /^(top|bottom)-(left-corner|left|center|right|right-corner)$|^(left|right)-(top|middle|bottom)$/i.test(mm[1])) {
                        var mr = makeRule(CSSMarginRule, 'margin', { __name: mm[1].toLowerCase(), __style: null });
                        mr.__style = decls(mm[2], mr);
                        mr.__parentRule = r;
                        r.__rules.push(mr);
                    } else if (!pit.rule) pdecl += seg;
                    pi2 = pit.end;
                }
                r.__style = decls(pdecl, r);
                pageDescriptorsOf(r.__style);
                break;
            case 'keyframes':
            case '-webkit-keyframes':
                if (p.block === null) return null;
                var kn = p.prelude;
                var km = /^"([^"]*)"$|^'([^']*)'$/.exec(kn);
                if (km) { kn = km[1] !== undefined ? km[1] : km[2]; if (!kn) return null; }
                else if (!/^[A-Za-z_-][\w-]*$/.test(kn) || /^-\d/.test(kn) || /^(none|initial|inherit|unset|default|revert|revert-layer)$/i.test(kn)) return null;
                r = makeRule(CSSKeyframesRule, 'keyframes', { __name: kn, __rules: [] });
                r.__rules = parseRules(p.block, 'keyframes', r);
                break;
            case 'property':
                if (p.block === null || !/^--[\w-]+$/.test(p.prelude)) return null;
                var descs = {};
                p.block.split(';').forEach(function (d) {
                    var c = d.indexOf(':');
                    if (c < 0) return;
                    var key = d.slice(0, c).trim().toLowerCase(), val = d.slice(c + 1).trim();
                    if (key === 'syntax') { var sv = /^"(.*)"$|^'(.*)'$/.exec(val); if (sv) val = sv[1] !== undefined ? sv[1] : sv[2]; }
                    if (key === 'syntax' && !sv) val = null;
                    descs[key] = val;
                });
                /* (syntax and inherits it must have; an initial value of that syntax) */
                var reg = global.__cyberCssSyntax;
                if (reg) {
                    if (typeof descs.syntax !== 'string' || !reg.__syntaxValid(descs.syntax)) return null;
                    if (descs.inherits !== 'true' && descs.inherits !== 'false') return null;
                    if (!/^\s*\*\s*$/.test(descs.syntax)) {
                        var iv = descs['initial-value'];
                        if (iv === undefined || reg.__syntaxParse(descs.syntax, iv) === null || reg.__dependsOnContext(iv)) return null;
                    } else if (/var\(/.test(descs['initial-value'] || '')) return null;
                }
                r = makeRule(CSSPropertyRule, 'property', { __name: p.prelude, __descs: descs });
                break;
            case 'counter-style':
                if (p.block === null) return null;
                if (!csName(p.prelude)) return null;
                r = makeRule(CSSCounterStyleRule, 'counter-style', { __name: p.prelude.trim(), __descs: csParse(p.block) });
                break;
            default:
                return null;    /* unknown at-rules are dropped */
            }
        }
        if (r) r.__parentRule = parent && parent instanceof CSSRule ? parent : null;
        return r;
    }

    /* insertRule's steps: index k of rules (CSSOM "insert a CSS rule") */
    function insertInto(rules, text, index, nested, parent) {
        if (index > rules.length) throw domException("Failed to execute 'insertRule': The index provided (" + index + ") is larger than the maximum index (" + rules.length + ").", 'IndexSizeError');
        /* (bare declarations go in a style rule or an @scope) */
        var bare = nested && nestedUnder(parent) && text.indexOf('{') < 0 ? nestedDeclarations(text, parent) : null;
        /* (@import and @namespace belong to a sheet, not a grouping rule) */
        if (nested && /^\s*@(import|namespace)\b/i.test(text))
            throw domException("Failed to execute 'insertRule': '" + text + "' is not allowed here.", 'HierarchyRequestError');
        /* (one rule and nothing after it but space and comments) */
        if (!bare) {
            var depth = 0, q = '', end = -1;
            for (var ti = 0; ti < text.length && end < 0; ti++) {
                var ch = text[ti];
                if (q) { if (ch === '\\') ti++; else if (ch === q) q = ''; continue; }
                if (ch === '\\') { ti++; continue; }
                if (ch === '"' || ch === "'") q = ch;
                else if (ch === '/' && text[ti + 1] === '*') { var ce = text.indexOf('*/', ti + 2); ti = ce < 0 ? text.length : ce + 1; }
                else if (ch === '{') depth++;
                else if (ch === '}') { depth--; if (!depth) end = ti; }
                else if (ch === ';' && !depth) end = ti;
            }
            if (end >= 0 && text.slice(end + 1).replace(/\/\*[\s\S]*?\*\//g, '').trim())
                throw domException("Failed to execute 'insertRule': Failed to parse the rule '" + text + "'.", 'SyntaxError');
        }
        var parts = bare ? [null] : splitRules(text);
        if (parts.length !== 1) throw domException("Failed to execute 'insertRule': Failed to parse the rule '" + text + "'.", 'SyntaxError');
        var r = bare || buildRule(parts[0], nested ? (parent && parent.__kind === 'style' ? 'style' : 'group') : 'top', parent);
        if (!r) throw domException("Failed to execute 'insertRule': Failed to parse the rule '" + text + "'.", 'SyntaxError');
        /* @import / @namespace only before other rules */
        if (r.__kind === 'import' && rules.slice(0, index).some(function (x) { return x.__kind !== 'import' && x.__kind !== 'layer'; }))
            throw domException("Failed to execute 'insertRule': @import rules must precede all rules aside from @charset and @layer statements.", 'HierarchyRequestError');
        if (r.__kind !== 'import' && r.__kind !== 'namespace' && r.__kind !== 'layer' && rules.slice(index).some(function (x) { return x.__kind === 'import'; }))
            throw domException("Failed to execute 'insertRule': Failed to insert the rule.", 'HierarchyRequestError');
        rules.splice(index, 0, r);
        if (!nested) r.__sheet = parent;
        return index;
    }

    /* CSSRuleList over an array of rules: live (its indices re-synced
     * after every change, which all go through this file) */
    function CSSRuleList() { throw new TypeError('Illegal constructor'); }
    var LISTS = [];
    function syncList(l) {
        var arr = l.__arr, had = l.__n;
        for (var i = arr.length; i < had; i++) delete l[i];
        for (var j = had; j < arr.length; j++)
            (function (k) { Object.defineProperty(l, k, { get: function () { return this.__arr[k]; }, enumerable: true, configurable: true }); })(j);
        l.__n = arr.length;
    }
    function ruleList(arr, owner) {
        var l = owner && owner.__ruleList;
        if (!l || l.__arr !== arr) {
            l = Object.create(CSSRuleList.prototype);
            Object.defineProperty(l, '__arr', { value: arr });
            Object.defineProperty(l, '__n', { value: 0, writable: true });
            if (owner) Object.defineProperty(owner, '__ruleList', { value: l, writable: true, configurable: true });
        }
        syncList(l);
        return l;
    }
    /* after a change to arr: its list, if any, follows */
    function changedList(owner) { if (owner && owner.__ruleList) syncList(owner.__ruleList); }
    getter(CSSRuleList.prototype, 'length', function () { return this.__arr.length; });
    define(CSSRuleList.prototype, 'item', function (i) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'item' on 'CSSRuleList': 1 argument required");
        i = i >>> 0; var a = this.__arr; return i < a.length ? a[i] : null; });
    Object.defineProperty(CSSRuleList.prototype, Symbol.toStringTag, { value: 'CSSRuleList', configurable: true });
    CSSRuleList.prototype[Symbol.iterator] = function* () { var a = this.__arr; for (var i = 0; i < a.length; i++) yield a[i]; };
    expose('CSSRuleList', CSSRuleList);

    /* ==================================================================
     * Style sheets
     * ================================================================== */

    function StyleSheet() { throw new TypeError('Illegal constructor'); }
    Object.defineProperty(StyleSheet.prototype, Symbol.toStringTag, { value: 'StyleSheet', configurable: true });

    function CSSStyleSheet(options) {
        if (!new.target) throw new TypeError("Failed to construct 'CSSStyleSheet': Please use the 'new' operator");
        options = options || {};
        initSheet(this, null, '', { media: options.media, title: null, constructed: true, disabled: !!options.disabled,
                                    baseURL: options.baseURL });
    }
    CSSStyleSheet.prototype = Object.create(StyleSheet.prototype);
    define(CSSStyleSheet.prototype, 'constructor', CSSStyleSheet);
    Object.defineProperty(CSSStyleSheet.prototype, Symbol.toStringTag, { value: 'CSSStyleSheet', configurable: true });
    Object.setPrototypeOf(CSSStyleSheet, StyleSheet);

    function initSheet(sh, owner, text, o) {
        Object.defineProperty(sh, SHEET, { value: {
            owner: owner, text: text, rules: null, media: null, mediaText: o.media === undefined ? '' :
                (typeof o.media === 'string' ? o.media : (o.media && o.media.mediaText) || ''),
            title: o.title, constructed: !!o.constructed, disabled: !!o.disabled, modified: false, href: o.href || null,
            origin_clean: o.originClean !== false,
        } });
    }
    function sheetRules(sh) {
        var st = sh[SHEET];
        if (!st.rules) {
            st.rules = parseRules(st.text || '', 'top', sh);
            for (var k = 0; k < st.rules.length; k++) st.rules[k].__sheet = sh;
        }
        return st.rules;
    }
    function checkOrigin(sh) {
        if (!sh[SHEET].origin_clean) throw domException("Failed to read the 'cssRules' property from 'CSSStyleSheet': Cannot access rules", 'SecurityError');
    }
    getter(CSSStyleSheet.prototype, 'cssRules', function () { checkOrigin(this); return ruleList(sheetRules(this), this); });
    getter(CSSStyleSheet.prototype, 'rules', function () { checkOrigin(this); return ruleList(sheetRules(this), this); });
    getter(CSSStyleSheet.prototype, 'ownerRule', function () { return this[SHEET].ownerRule || null; });
    getter(StyleSheet.prototype, 'type', function () { return 'text/css'; });
    getter(StyleSheet.prototype, 'href', function () { return this[SHEET].href; });
    /* (an element taken out of the document lets go of its sheet) */
    getter(StyleSheet.prototype, 'ownerNode', function () {
        var st = this[SHEET], o = st.owner;
        if (o && !o.isConnected) {
            st.owner = null;
            if (o.__cyberSheet === this) o.__cyberSheet = null;
        }
        return st.owner;
    });
    getter(StyleSheet.prototype, 'parentStyleSheet', function () { return this[SHEET].parent || null; });
    getter(StyleSheet.prototype, 'title', function () { var t = this[SHEET].title; return t === undefined || t === '' ? null : t; });
    getter(StyleSheet.prototype, 'media', function () {
        var st = this[SHEET], self = this;
        if (st.ownerRule) return st.ownerRule.media;
        return st.media || (st.media = newMediaList(st.mediaText, function () { self.__changed(); }));
    }, function (v) { this.media.mediaText = v; });
    getter(StyleSheet.prototype, 'disabled', function () { return this[SHEET].disabled; },
        function (v) { this[SHEET].disabled = !!v; this.__changed(); });
    define(CSSStyleSheet.prototype, '__changed', function () {
        this[SHEET].textCache = undefined;
        if (typeof global.__cyberRelayout === 'function') global.__cyberRelayout();
    });
    /* the text layout renders: the serialization once modified */
    define(CSSStyleSheet.prototype, '__cyberText', function () {
        var st = this[SHEET];
        if (st.disabled) return '';
        if (!st.modified && !st.constructed) return st.text;
        if (st.textCache === undefined) st.textCache = sheetRules(this).map(ruleText).join('\n');
        return st.textCache;
    });
    Object.defineProperty(CSSStyleSheet.prototype, '__cyberModified', { get: function () { return this[SHEET].modified || this[SHEET].disabled; } });
    /* (layout reads constructed sheets\x27 cssText: their rules\x27 text) */
    Object.defineProperty(CSSStyleSheet.prototype, 'cssText', { get: function () { return this.__cyberText(); }, configurable: true });
    define(CSSStyleSheet.prototype, 'insertRule', function (rule, index) {
        checkOrigin(this);
        if (arguments.length < 1) throw new TypeError("Failed to execute 'insertRule': 1 argument required");
        var k = insertInto(sheetRules(this), String(rule), index === undefined ? 0 : index >>> 0, false, this);
        changedList(this);
        this[SHEET].modified = true;
        this.__changed();
        return k;
    });
    define(CSSStyleSheet.prototype, 'deleteRule', function (index) {
        checkOrigin(this);
        if (arguments.length < 1) throw new TypeError("Failed to execute 'deleteRule': 1 argument required");
        var rules = sheetRules(this);
        index = index >>> 0;
        if (index >= rules.length) throw domException("Failed to execute 'deleteRule': The index provided (" + index + ") is larger than the maximum index (" + (rules.length - 1) + ").", 'IndexSizeError');
        detachRule(rules.splice(index, 1)[0]);
        changedList(this);
        this[SHEET].modified = true;
        this.__changed();
    });
    define(CSSStyleSheet.prototype, 'addRule', function (selector, style, index) {
        var rules = sheetRules(this);
        var text = String(selector === undefined ? 'undefined' : selector) + ' { ' + (style === undefined ? '' : String(style)) + ' }';
        this.insertRule(text, index === undefined ? rules.length : index);
        return -1;
    });
    define(CSSStyleSheet.prototype, 'removeRule', function (index) { this.deleteRule(index === undefined ? 0 : index); });
    define(CSSStyleSheet.prototype, 'replaceSync', function (text) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'replaceSync' on 'CSSStyleSheet': 1 argument required");
        if (!this[SHEET].constructed) throw domException("Failed to execute 'replaceSync': Can't call replaceSync on non-constructed CSSStyleSheets.", 'NotAllowedError');
        var rules = parseRules(String(text), 'top', this).filter(function (r) { return r.__kind !== 'import'; });
        for (var k = 0; k < rules.length; k++) rules[k].__sheet = this;
        this[SHEET].rules = rules;
        if (this.__ruleList) this.__ruleList = null;
        this[SHEET].modified = true;
        this.__changed();
    });
    define(CSSStyleSheet.prototype, 'replace', function (text) {
        var self = this;
        return new Promise(function (resolve, reject) {
            try { self.replaceSync(text); resolve(self); } catch (e) { reject(e); }
        });
    });
    expose('StyleSheet', StyleSheet);
    expose('CSSStyleSheet', CSSStyleSheet);
    expose('MediaList', MediaList);

    /* <style>.sheet, <link rel=stylesheet>.sheet */
    function elementSheet(el) {
        if (!el.isConnected) return null;
        var tag = (el.localName || '').toLowerCase(), text, href = null, origin = true;
        if (tag === 'style') {
            var type = el.getAttribute('type');
            if (type !== null && type !== '' && type.toLowerCase() !== 'text/css') return null;
            text = el.textContent || '';
        } else if (tag === 'link') {
            var rel = (el.getAttribute('rel') || '').toLowerCase().split(/\s+/);
            if (rel.indexOf('stylesheet') < 0 || !el.getAttribute('href')) return null;
            href = el.href;
            text = null;
        } else return null;
        var sh = el.__cyberSheet;
        /* (the same text: the same sheet, changed through the CSSOM or not; new text: a new sheet) */
        if (sh && (tag === 'link' ? sh[SHEET].href === href : sh[SHEET].text === text)) return sh;
        sh = Object.create(CSSStyleSheet.prototype);
        initSheet(sh, el, text, { media: el.getAttribute('media') || '', title: el.getAttribute('title'), href: href });
        if (tag === 'link') {
            /* the sheet's text, when it can be read (same origin) */
            try {
                var x = new XMLHttpRequest();
                x.open('GET', href, false);
                x.send();
                sh[SHEET].text = x.status >= 200 && x.status < 300 ? x.responseText : '';
            } catch (e) { sh[SHEET].text = ''; }
            try { sh[SHEET].origin_clean = new URL(href, document.baseURI).origin === location.origin; } catch (e) { /* (kept) */ }
        }
        Object.defineProperty(el, '__cyberSheet', { value: sh, writable: true, configurable: true });
        return sh;
    }
    ['HTMLStyleElement', 'HTMLLinkElement'].forEach(function (n) {
        var C = global[n];
        var P = C && C.prototype;
        if (!P) return;
        Object.defineProperty(P, 'sheet', { get: function sheet() {
            if (!P.isPrototypeOf(this)) throw new TypeError('Illegal invocation');
            return elementSheet(this);
        }, configurable: true, enumerable: true });
        /* (disabled: its sheet's) */
        Object.defineProperty(P, 'disabled', {
            get: function () { var s = elementSheet(this); return s ? s.disabled : false; },
            set: function (v) { var s = elementSheet(this); if (s) s.disabled = v; },
            configurable: true, enumerable: true });
    });
    /* (elements made before the interfaces had their own prototypes) */
    var EP = global.Element && global.Element.prototype;
    if (EP && !Object.getOwnPropertyDescriptor(EP, 'sheet')) {
        Object.defineProperty(EP, 'sheet', { get: function () {
            var t = (this.localName || '').toLowerCase();
            return t === 'style' || t === 'link' ? elementSheet(this) : undefined;
        }, configurable: true });
    }

    /* document.styleSheets */
    function StyleSheetList() { throw new TypeError('Illegal constructor'); }
    function sheetList(doc) {
        var els = doc.querySelectorAll ? doc.querySelectorAll('style, link') : [];
        var arr = [];
        for (var k = 0; k < els.length; k++) { var s = elementSheet(els[k]); if (s) arr.push(s); }
        var l = Object.create(StyleSheetList.prototype);
        for (var j = 0; j < arr.length; j++) Object.defineProperty(l, j, { value: arr[j], enumerable: true });
        Object.defineProperty(l, '__n', { value: arr.length });
        return l;
    }
    function isSheetList(l) { return l && typeof l === 'object' && Object.prototype.hasOwnProperty.call(l, '__n'); }
    getter(StyleSheetList.prototype, 'length', function () {
        if (!isSheetList(this)) throw new TypeError('Illegal invocation');
        return this.__n;
    });
    define(StyleSheetList.prototype, 'item', function (i) {
        if (!isSheetList(this)) throw new TypeError('Illegal invocation');
        if (arguments.length < 1) throw new TypeError("Failed to execute 'item' on 'StyleSheetList': 1 argument required");
        i = i >>> 0; return i < this.__n ? this[i] : null;
    });
    StyleSheetList.prototype[Symbol.iterator] = Array.prototype[Symbol.iterator];
    Object.defineProperty(StyleSheetList.prototype, Symbol.toStringTag, { value: 'StyleSheetList', configurable: true });
    expose('StyleSheetList', StyleSheetList);
    /* CSS.escape (CSSOM 2.1): an identifier's escaped form */
    if (global.CSS) Object.defineProperty(global.CSS, 'escape', { configurable: true, enumerable: true, writable: true,
        value: function escape(ident) {
            if (arguments.length < 1) throw new TypeError("Failed to execute 'escape' on 'CSS': 1 argument required");
            var s = String(ident), n = s.length, out = '', first = n ? s.charCodeAt(0) : 0;
            if (n === 1 && first === 0x2d) return '\\' + s;
            for (var i = 0; i < n; i++) {
                var c = s.charCodeAt(i);
                if (c === 0) { out += '\uFFFD'; continue; }
                if ((c >= 0x1 && c <= 0x1f) || c === 0x7f || (i === 0 && c >= 0x30 && c <= 0x39) ||
                    (i === 1 && c >= 0x30 && c <= 0x39 && first === 0x2d)) { out += '\\' + c.toString(16) + ' '; continue; }
                if (c >= 0x80 || c === 0x2d || c === 0x5f || (c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x5a) || (c >= 0x61 && c <= 0x7a)) { out += s.charAt(i); continue; }
                out += '\\' + s.charAt(i);
            }
            return out;
        } });
    /* (a namespace object isn't enumerable) */
    if (Object.getOwnPropertyDescriptor(global, 'CSS')) Object.defineProperty(global, 'CSS', { enumerable: false });
    var DP = global.Document && global.Document.prototype;
    var docTarget = DP || (global.document && Object.getPrototypeOf(global.document));
    if (docTarget) Object.defineProperty(docTarget, 'styleSheets', { get: function styleSheets() {
        if (!this || this.nodeType !== 9) throw new TypeError('Illegal invocation');
        return sheetList(this);
    }, configurable: true, enumerable: true });
    if (global.document && Object.prototype.hasOwnProperty.call(global.document, 'styleSheets')) {
        try { delete global.document.styleSheets; } catch (e) { /* (kept) */ }
    }

    /* for querySelector and CSS.supports(selector()) */
    Object.defineProperty(global, '__cyberSelectorValid', { value: selectorValid, configurable: true });
    Object.defineProperty(global, '__cyberMediaText', { value: function (q) { return newMediaList(q).mediaText; }, configurable: true });
    Object.defineProperty(global, '__cyberSelectorSerialize', { value: parseSelectorList, configurable: true });
})

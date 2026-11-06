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
        Object.defineProperty(proto, name, { get: get, set: set, configurable: true, enumerable: true });
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

    var PSEUDO_CLASSES = ('active any-link autofill blank checked current default defined disabled empty enabled ' +
        'first first-child first-of-type focus focus-visible focus-within fullscreen future heading host hover ' +
        'in-range indeterminate invalid last-child last-of-type left link local-link modal muted only-child ' +
        'only-of-type open optional out-of-range past paused picture-in-picture placeholder-shown playing ' +
        'popover-open read-only read-write required right root scope seeking stalled target target-within ' +
        'user-invalid user-valid valid visited volume-locked buffering closed xr-overlay -webkit-autofill ' +
        '-webkit-any-link -webkit-full-screen').split(' ');
    var PSEUDO_CLASS_FNS = 'dir has host host-context is lang not nth-child nth-last-child nth-last-of-type nth-of-type state where heading active-view-transition-type'.split(' ');
    var PSEUDO_ELEMENTS = ('after backdrop before cue file-selector-button first-letter first-line grammar-error marker ' +
        'placeholder selection spelling-error target-text view-transition details-content checkmark picker-icon ' +
        'scroll-marker scroll-marker-group column search-text -webkit-scrollbar -webkit-scrollbar-thumb ' +
        '-webkit-scrollbar-track -webkit-scrollbar-button -webkit-scrollbar-corner -webkit-scrollbar-track-piece ' +
        '-webkit-resizer -webkit-input-placeholder -webkit-file-upload-button -webkit-inner-spin-button ' +
        '-webkit-outer-spin-button -webkit-search-cancel-button -webkit-details-marker').split(' ');
    var PSEUDO_ELEMENT_FNS = 'part slotted highlight cue view-transition-group view-transition-image-pair view-transition-old view-transition-new picker scroll-button'.split(' ');
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
            var out = [];
            for (;;) {
                this.skipWS();
                var start = this.i, sel = null;
                try { sel = this.complex(relative); } catch (e) { if (!forgiving) throw e; }
                this.skipWS();
                var t = this.peek();
                if (sel === null || (t && t.t !== ',' && t.t !== ')')) {
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
                t = this.peek();
                if (t && t.t === ',') { this.i++; continue; }
                break;
            }
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
                var h = this.list(true, false);
                if (!h.length) throw new SyntaxError('empty :has');
                return { list: h, relative: true };
            }
            if (!element && (fn === 'host' || fn === 'host-context')) {
                var c = this.compound();
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

    function serCompound(c) {
        var s = '';
        for (var k = 0; k < c.items.length; k++) {
            var it = c.items[k];
            switch (it.k) {
            case 'type':
                /* (an implied * before a pseudo-element goes without saying) */
                if (it.v === '*' && it.ns === null && c.items[k + 1] && (c.items[k + 1].k === 'pelem' || c.items[k + 1].k === 'pelemfn')) break;
                if (it.ns !== null) s += (it.ns === '*' ? '*' : serIdent(it.ns)) + '|';
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
                if (a.list) s += serList(a.list);
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
        if (/::view-transition/i.test(text)) global.document.createDocumentFragment().querySelector(text);
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
    getter(CSSRule.prototype, 'parentStyleSheet', function () {
        for (var r = this; r; r = r.__parentRule) if (r.__sheet) return r.__sheet;
        return null;
    });
    getter(CSSRule.prototype, 'type', function () { return RULE_TYPES[this.__kind] || 0; });
    getter(CSSRule.prototype, 'cssText', function () { return this.__text(); }, function () { /* (no effect) */ });

    function subclass(name, parent) {
        var C = illegal(name);
        Object.defineProperty(C, 'name', { configurable: true, value: name });
        C.prototype = Object.create(parent.prototype);
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
                var at = t.slice(i).search(/style\(/i);
                if (at < 0) { out += one(t.slice(i)); break; }
                out += one(t.slice(i, i + at));
                var j = i + at + 6, depth = 1;
                while (j < t.length && depth) { if (t[j] === '(') depth++; else if (t[j] === ')') depth--; j++; }
                out += t.slice(i + at, j);
                i = j;
            }
            return out;
        }).join(', ');
    }
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
    getter(CSSFontFeatureValuesRule.prototype, 'fontFamily', function () { return this.__family; });
    CSSFontFeatureValuesRule.prototype.__text = function () {
        var maps = this.__maps, out = '@font-feature-values ' + this.__family + ' {';
        Object.keys(FFV_KINDS).forEach(function (k) {
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
        var low = function (n) { return String(n).trim().toLowerCase(); };
        var is = function (n) { return low(n) === 'unicode-range'; };
        Object.defineProperty(style, 'setProperty', { configurable: true, writable: true, value: function setProperty(n, v, pri) {
            if (low(n) in FACE_DESC) {
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
        /* (what it may not have isn't there: no attribute, and setting one does nothing) */
        var hyphen = function (n) { return n.replace(/[A-Z]/g, function (c) { return '-' + c.toLowerCase(); }).replace(/^webkit-/, '-webkit-'); };
        Object.getOwnPropertyNames(DECL_PROTO).forEach(function (n) {
            var d = Object.getOwnPropertyDescriptor(DECL_PROTO, n);
            if (!d || !d.get || /^(cssText|length|parentRule|cssFloat)$/.test(n)) return;
            if (tryAllowed(n.indexOf('-') >= 0 ? n : hyphen(n))) return;
            Object.defineProperty(CSSPositionTryDescriptors.prototype, n, { configurable: true, enumerable: false,
                get: function () { return undefined; }, set: function () {} });
        });
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
        rules.splice(index, 1);
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
        this.__rules.splice(index, 1);
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
    CSSSupportsRule.prototype.__text = function () { return blockText('@supports ' + this.__cond, this.__rules); };
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
    getter(CSSImportRule.prototype, 'styleSheet', function () { return null; });
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
        this.__selector = String(v).trim(); touched(this);
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
    CSSKeyframesRule.prototype.__text = function () {
        var nm = /^[A-Za-z_-][\w-]*$/.test(this.__name) && !/^(none|initial|inherit|unset|default)$/i.test(this.__name) ? this.__name : serString(this.__name);
        return blockText('@keyframes ' + nm, this.__rules);
    };
    function keyText(t) {
        var parts = t.split(',').map(function (x) {
            x = x.trim().toLowerCase();
            if (x === 'from') return '0%';
            if (x === 'to') return '100%';
            if (!/^[+-]?(\d*\.)?\d+%$/.test(x)) return null;
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
    function braceEnd(block, open) {
        var d = 1, k = open + 1, q = null, n = block.length;
        while (k < n && d) {
            var c = block[k];
            if (q) { if (c === '\\') { k += 2; continue; } if (c === q) q = null; k++; continue; }
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
                    __cquery: cname ? cm[2].trim() : p.prelude.trim(), __rules: [] });
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
                if (p.block === null || !/^--(?:[\w-]|[^\x00-\x7f]|\\.)+$/.test(p.prelude)) return null;
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
                    if (DECL_PROTO) Object.setPrototypeOf(r.__style, CSSPositionTryDescriptors.prototype);
                }
                break;
            case 'page':
                if (p.block === null) return null;
                if (!pageSelectorOk(p.prelude)) return null;
                r = makeRule(CSSPageRule, 'page', { __selector: p.prelude, __style: null, __rules: [] });
                /* (the margin at-rules in it: rules of their own; the rest its declarations) */
                var pdecl = '', pi2 = 0, pb = p.block;
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
                break;
            case 'keyframes':
            case '-webkit-keyframes':
                if (p.block === null) return null;
                var kn = p.prelude;
                var km = /^"([^"]*)"$|^'([^']*)'$/.exec(kn);
                if (km) kn = km[1] !== undefined ? km[1] : km[2];
                else if (!/^[A-Za-z_-][\w-]*$/.test(kn) || /^(none|initial|inherit|unset|default|revert|revert-layer)$/i.test(kn)) return null;
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
    getter(CSSStyleSheet.prototype, 'ownerRule', function () { return null; });
    getter(StyleSheet.prototype, 'type', function () { return 'text/css'; });
    getter(StyleSheet.prototype, 'href', function () { return this[SHEET].href; });
    getter(StyleSheet.prototype, 'ownerNode', function () { return this[SHEET].owner; });
    getter(StyleSheet.prototype, 'parentStyleSheet', function () { return null; });
    getter(StyleSheet.prototype, 'title', function () { var t = this[SHEET].title; return t === undefined || t === '' ? null : t; });
    getter(StyleSheet.prototype, 'media', function () {
        var st = this[SHEET], self = this;
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
        rules.splice(index, 1);
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
        if (sh && (tag === 'link' ? sh[SHEET].href === href : sh[SHEET].text === text || sh[SHEET].modified)) return sh;
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

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
        'scroll-marker scroll-marker-group column scroll-button -webkit-scrollbar -webkit-scrollbar-thumb ' +
        '-webkit-scrollbar-track -webkit-scrollbar-button -webkit-scrollbar-corner -webkit-scrollbar-track-piece ' +
        '-webkit-resizer -webkit-input-placeholder -webkit-file-upload-button -webkit-inner-spin-button ' +
        '-webkit-outer-spin-button -webkit-search-cancel-button -webkit-details-marker').split(' ');
    var PSEUDO_ELEMENT_FNS = 'part slotted highlight cue view-transition-group view-transition-image-pair view-transition-old view-transition-new picker scroll-button'.split(' ');
    var LEGACY_PSEUDO_ELEMENTS = ['before', 'after', 'first-line', 'first-letter'];

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
            for (;;) {
                var ws = this.skipWS();
                t = this.peek();
                if (!t || t.t === ',' || t.t === ')' || t.t === '{') break;
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
            }
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
            if (!raw && fn !== 'heading') throw new SyntaxError('empty argument');
            if (fn === 'dir' && !/^(ltr|rtl)$/i.test(raw)) throw new SyntaxError('bad :dir');
            if (fn === 'heading' && raw && !/^\s*\d+(\s*,\s*\d+)*\s*$/.test(raw)) throw new SyntaxError('bad :heading');
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
                if (it.ns !== null) s += (it.ns === '*' ? '*' : serIdent(it.ns)) + '|';
                s += it.v === '*' ? '*' : serIdent(it.v);
                break;
            case 'id': s += '#' + serIdent(it.v); break;
            case 'class': s += '.' + serIdent(it.v); break;
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
        return serList(list);
    }
    function selectorValid(text) {
        try { parseSelectorList(text); return true; } catch (e) { return false; }
    }

    /* ==================================================================
     * MediaList
     * ================================================================== */

    function normalizeMedia(q) {
        return q.replace(/\s+/g, ' ').trim().replace(/\(\s+/g, '(').replace(/\s+\)/g, ')')
            .replace(/\s*:\s*/g, ': ').replace(/^(only|not)\s+/i, function (m) { return m.toLowerCase(); })
            .replace(/(^|\s)(and|or)(\s|\()/gi, function (m, a, w, b) { return a + w.toLowerCase() + b; })
            .replace(/(^|[\s(,])(all|screen|print|speech)\b/gi, function (m, a, w) { return a + w.toLowerCase(); });
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
        return out.map(function (q) { return normalizeMedia(q); }).filter(function (q) { return q.length; });
    }
    function MediaList() { throw new TypeError('Illegal constructor'); }
    function newMediaList(text, onChange) {
        var m = Object.create(MediaList.prototype);
        Object.defineProperty(m, '__items', { value: splitMedia(text || ''), writable: true });
        Object.defineProperty(m, '__onChange', { value: onChange || null });
        return m;
    }
    getter(MediaList.prototype, 'mediaText', function () { return this.__items.join(', '); },
        function (v) { this.__items = splitMedia(v == null ? '' : String(v)); if (this.__onChange) this.__onChange(); });
    getter(MediaList.prototype, 'length', function () { return this.__items.length; });
    define(MediaList.prototype, 'item', function (i) { i = i >>> 0; return i < this.__items.length ? this.__items[i] : null; });
    define(MediaList.prototype, 'appendMedium', function (m) {
        m = normalizeMedia(String(m));
        if (m && this.__items.indexOf(m) < 0) { this.__items.push(m); if (this.__onChange) this.__onChange(); }
    });
    define(MediaList.prototype, 'deleteMedium', function (m) {
        m = normalizeMedia(String(m));
        var k = this.__items.indexOf(m);
        if (k < 0) throw domException("Failed to execute 'deleteMedium': the medium is not in the list.", 'NotFoundError');
        this.__items.splice(k, 1);
        if (this.__onChange) this.__onChange();
    });
    define(MediaList.prototype, 'toString', function () { return this.mediaText; });
    Object.defineProperty(MediaList.prototype, Symbol.toStringTag, { value: 'MediaList', configurable: true });
    MediaList.prototype[Symbol.iterator] = Array.prototype[Symbol.iterator];

    /* ==================================================================
     * Rules
     * ================================================================== */

    function CSSRule() { throw new TypeError('Illegal constructor'); }
    var RULE_TYPES = { style: 1, charset: 2, import: 3, media: 4, 'font-face': 5, page: 6, keyframes: 7,
                       keyframe: 8, namespace: 10, 'counter-style': 11, supports: 12, 'font-feature-values': 14 };
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
        C.prototype = Object.create(parent.prototype);
        define(C.prototype, 'constructor', C);
        Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true });
        Object.setPrototypeOf(C, parent);
        expose(name, C);
        return C;
    }
    var CSSStyleRule = subclass('CSSStyleRule', CSSRule);
    var CSSGroupingRule = subclass('CSSGroupingRule', CSSRule);
    var CSSConditionRule = subclass('CSSConditionRule', CSSGroupingRule);
    var CSSMediaRule = subclass('CSSMediaRule', CSSConditionRule);
    var CSSSupportsRule = subclass('CSSSupportsRule', CSSConditionRule);
    var CSSContainerRule = subclass('CSSContainerRule', CSSConditionRule);
    var CSSLayerBlockRule = subclass('CSSLayerBlockRule', CSSGroupingRule);
    var CSSLayerStatementRule = subclass('CSSLayerStatementRule', CSSRule);
    var CSSScopeRule = subclass('CSSScopeRule', CSSGroupingRule);
    var CSSStartingStyleRule = subclass('CSSStartingStyleRule', CSSGroupingRule);
    var CSSImportRule = subclass('CSSImportRule', CSSRule);
    var CSSNamespaceRule = subclass('CSSNamespaceRule', CSSRule);
    var CSSFontFaceRule = subclass('CSSFontFaceRule', CSSRule);
    var CSSPageRule = subclass('CSSPageRule', CSSGroupingRule);
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
    function touched(r) {
        for (var x = r; x; x = x.__parentRule) if (x.__sheet) { x.__sheet[SHEET].modified = true; x.__sheet.__changed(); return; }
    }

    /* declarations: the native CSSStyleDeclaration */
    function decls(text) { return global.__cyberStyleDecl(text || ''); }
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
    getter(CSSStyleRule.prototype, 'style', function () { touched(this); return this.__style; }, function (v) {
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
    });
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
    getter(CSSImportRule.prototype, 'media', function () { return this.__media || (this.__media = newMediaList(this.__mediaText)); });
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
    getter(CSSFontFaceRule.prototype, 'style', function () { touched(this); return this.__style; });
    CSSFontFaceRule.prototype.__text = function () { var d = declText(this.__style); return '@font-face {' + (d ? ' ' + d + ' ' : ' ') + '}'; };
    getter(CSSPageRule.prototype, 'selectorText', function () { return this.__selector; }, function (v) { this.__selector = String(v).trim(); touched(this); });
    getter(CSSPageRule.prototype, 'style', function () { touched(this); return this.__style; });
    CSSPageRule.prototype.__text = function () {
        var d = declText(this.__style);
        return '@page' + (this.__selector ? ' ' + this.__selector : '') + ' {' + (d ? ' ' + d + ' ' : ' ') + '}';
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
        var r = parseRules(String(text), 'keyframes', this)[0];
        if (r) { this.__rules.push(r); changedList(this); touched(this); }
    });
    define(CSSKeyframesRule.prototype, 'deleteRule', function (key) {
        var k = keyText(String(key));
        for (var i = this.__rules.length - 1; i >= 0; i--) if (this.__rules[i].__key === k) { this.__rules.splice(i, 1); changedList(this); touched(this); return; }
    });
    define(CSSKeyframesRule.prototype, 'findRule', function (key) {
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
    getter(CSSKeyframeRule.prototype, 'style', function () { touched(this); return this.__style; });
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
    getter(CSSCounterStyleRule.prototype, 'name', function () { return this.__name; });
    CSSCounterStyleRule.prototype.__text = function () { return '@counter-style ' + this.__name + ' {' + (this.__body ? ' ' + this.__body + ' ' : ' ') + '}'; };
    getter(CSSNestedDeclarations.prototype, 'style', function () { touched(this); return this.__style; });
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

    function buildRule(p, ctx, parent) {
        var r = null;
        if (ctx === 'keyframes') {
            if (p.at || p.block === null) return null;
            var key = keyText(p.prelude);
            if (key === null) return null;
            r = makeRule(CSSKeyframeRule, 'keyframe', { __key: key, __style: decls(p.block) });
        } else if (!p.at) {
            if (p.block === null) return null;
            var sel;
            try { sel = parseSelectorList(p.prelude); } catch (e) { return null; }
            var b = splitBlock(p.block);
            r = makeRule(CSSStyleRule, 'style', { __selector: sel, __style: decls(b[0]), __rules: [] });
            for (var k = 0; k < b[1].length; k++) {
                var nested = parseRules(b[1][k], 'style', r);
                for (var m = 0; m < nested.length; m++) r.__rules.push(nested[m]);
            }
        } else {
            switch (p.at) {
            case 'media':
                if (p.block === null) return null;
                r = makeRule(CSSMediaRule, 'media', { __mediaText: p.prelude, __media: null, __rules: [] });
                r.__rules = parseRules(p.block, 'group', r);
                break;
            case 'supports':
                if (p.block === null) return null;
                r = makeRule(CSSSupportsRule, 'supports', { __cond: p.prelude.replace(/\s+/g, ' ').trim(), __rules: [] });
                r.__rules = parseRules(p.block, 'group', r);
                break;
            case 'container':
                if (p.block === null) return null;
                var cm = /^([A-Za-z_-][\w-]*)\s+(.*)$/.exec(p.prelude);
                var cname = cm && !/^(not|style|scroll-state)$/i.test(cm[1]) && !/^\(/.test(cm[1]) ? cm[1] : '';
                r = makeRule(CSSContainerRule, 'container', { __cond: p.prelude.replace(/\s+/g, ' ').trim(), __cname: cname,
                    __cquery: cname ? cm[2].trim() : p.prelude.trim(), __rules: [] });
                r.__rules = parseRules(p.block, 'group', r);
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
            case 'starting-style':
                if (p.block === null) return null;
                r = makeRule(CSSStartingStyleRule, 'starting-style', { __rules: [] });
                r.__rules = parseRules(p.block, 'group', r);
                break;
            case 'scope':
                if (p.block === null) return null;
                var sm = /^(?:\(([^)]*)\))?\s*(?:to\s*\(([^)]*)\))?$/.exec(p.prelude);
                if (!sm) return null;
                r = makeRule(CSSScopeRule, 'scope', { __start: sm[1] ? sm[1].trim() : '', __end: sm[2] ? sm[2].trim() : '', __rules: [] });
                r.__rules = parseRules(p.block, 'group', r);
                break;
            case 'import':
                if (ctx !== 'top' || p.block !== null) return null;
                var im = /^(?:url\(\s*(?:"([^"]*)"|'([^']*)'|([^)\s]*))\s*\)|"([^"]*)"|'([^']*)')\s*(.*)$/i.exec(p.prelude);
                if (!im) return null;
                var href = im[1] !== undefined ? im[1] : im[2] !== undefined ? im[2] : im[3] !== undefined ? im[3] : im[4] !== undefined ? im[4] : im[5];
                var rest = im[6] || '', layer, supports;
                var lm = /^layer(?:\(([^)]*)\))?\s*/i.exec(rest);
                if (lm) { layer = lm[1] ? lm[1].trim() : ''; rest = rest.slice(lm[0].length); }
                var spm = /^supports\(([^)]*(?:\([^)]*\)[^)]*)*)\)\s*/i.exec(rest);
                if (spm) { supports = spm[1].trim(); rest = rest.slice(spm[0].length); }
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
                r = makeRule(CSSFontFaceRule, 'font-face', { __style: decls(p.block) });
                break;
            case 'page':
                if (p.block === null) return null;
                r = makeRule(CSSPageRule, 'page', { __selector: p.prelude, __style: decls(p.block), __rules: [] });
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
                    descs[key] = val;
                });
                r = makeRule(CSSPropertyRule, 'property', { __name: p.prelude, __descs: descs });
                break;
            case 'counter-style':
                if (p.block === null) return null;
                r = makeRule(CSSCounterStyleRule, 'counter-style', { __name: p.prelude, __body: p.block.trim().replace(/\s+/g, ' ') });
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
        var parts = splitRules(text);
        if (parts.length !== 1) throw domException("Failed to execute 'insertRule': Failed to parse the rule '" + text + "'.", 'SyntaxError');
        var r = buildRule(parts[0], nested ? (parent && parent.__kind === 'style' ? 'style' : 'group') : 'top', parent);
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
    define(CSSRuleList.prototype, 'item', function (i) { i = i >>> 0; var a = this.__arr; return i < a.length ? a[i] : null; });
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
    });
    getter(StyleSheet.prototype, 'disabled', function () { return this[SHEET].disabled; },
        function (v) { this[SHEET].disabled = !!v; this.__changed(); });
    define(CSSStyleSheet.prototype, '__changed', function () {
        if (typeof global.__cyberRelayout === 'function') global.__cyberRelayout();
    });
    /* the text layout renders: the serialization once modified */
    define(CSSStyleSheet.prototype, '__cyberText', function () {
        var st = this[SHEET];
        if (st.disabled) return '';
        if (!st.modified && !st.constructed) return st.text;
        return sheetRules(this).map(function (r) { return r.__text(); }).join('\n');
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
        Object.defineProperty(P, 'sheet', { get: function () { return elementSheet(this); }, configurable: true, enumerable: true });
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
        Object.defineProperty(l, 'length', { value: arr.length });
        return l;
    }
    define(StyleSheetList.prototype, 'item', function (i) { i = i >>> 0; return i < this.length ? this[i] : null; });
    StyleSheetList.prototype[Symbol.iterator] = Array.prototype[Symbol.iterator];
    Object.defineProperty(StyleSheetList.prototype, Symbol.toStringTag, { value: 'StyleSheetList', configurable: true });
    expose('StyleSheetList', StyleSheetList);
    var DP = global.Document && global.Document.prototype;
    var docTarget = DP || (global.document && Object.getPrototypeOf(global.document));
    if (docTarget) Object.defineProperty(docTarget, 'styleSheets', { get: function () { return sheetList(this); }, configurable: true });
    if (global.document && Object.prototype.hasOwnProperty.call(global.document, 'styleSheets')) {
        try { delete global.document.styleSheets; } catch (e) { /* (kept) */ }
    }

    /* for querySelector and CSS.supports(selector()) */
    Object.defineProperty(global, '__cyberSelectorValid', { value: selectorValid, configurable: true });
    Object.defineProperty(global, '__cyberSelectorSerialize', { value: parseSelectorList, configurable: true });
})

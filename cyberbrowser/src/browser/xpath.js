/*
 * xpath.js - XPath 1.0 over the DOM (DOM Standard 8: XPathResult,
 * XPathExpression, XPathEvaluator, and document.evaluate,
 * createExpression and createNSResolver).
 *
 * An expression is tokenized and parsed once into a tree of plain objects
 * ({ t: kind, ... }); evaluating it walks the tree over the DOM with the
 * node's own accessors.  Node-sets are arrays kept in document order
 * (compareDocumentPosition); attribute nodes are the elements' Attr
 * objects.  HTML elements in an HTML document match name tests
 * ASCII-case-insensitively, and a name with no prefix matches them in the
 * HTML namespace (HTML 3.4 "Interactions with XPath").
 * Called as fn(global, window).
 */
(function (global, win) {
    'use strict';
    var HTML_NS = 'http://www.w3.org/1999/xhtml';
    var STATE = Symbol('xpath');

    function define(obj, name, value, enumerable) {
        Object.defineProperty(obj, name, { value: value, writable: true, configurable: true, enumerable: !!enumerable });
    }
    function expose(name, value) {
        define(global, name, value);
        if (win && win !== global) define(win, name, value);
    }
    function domError(message, name) {
        return new (global.DOMException || Error)(message, name);
    }
    function syntaxError(expr) {
        return domError("The string '" + expr + "' is not a valid XPath expression.", 'SyntaxError');
    }

    /* ---- tokens ---- */
    var OPERATOR_NAMES = { and: 1, or: 1, mod: 1, div: 1 };
    var NODE_TYPES = { comment: 1, text: 1, 'processing-instruction': 1, node: 1 };
    var AXES = { ancestor: 1, 'ancestor-or-self': 1, attribute: 1, child: 1, descendant: 1, 'descendant-or-self': 1,
                 following: 1, 'following-sibling': 1, namespace: 1, parent: 1, preceding: 1, 'preceding-sibling': 1,
                 self: 1 };
    var NAME_START = /[A-Za-z_\u00C0-\u00D6\u00D8-\u00F6\u00F8-\u02FF\u0370-\u037D\u037F-\u1FFF\u200C-\u200D\u2070-\u218F\u2C00-\u2FEF\u3001-\uD7FF\uF900-\uFDCF\uFDF0-\uFFFD]/;
    var NAME_CHAR = /[-.0-9A-Za-z_\u00B7\u00C0-\u00D6\u00D8-\u00F6\u00F8-\u037D\u037F-\u1FFF\u200C-\u200D\u203F\u2040\u2070-\u218F\u2C00-\u2FEF\u3001-\uD7FF\uF900-\uFDCF\uFDF0-\uFFFD]/;

    /* tokens: { k: 'num'|'str'|'name'|'op'|'var'|'star'..., v } */
    function tokenize(s) {
        var out = [], i = 0, n = s.length;
        function prevAllowsOperator() {
            /* (XPath 3.7: * and an operator name are operators unless they follow
             * nothing, @, ::, (, [, , or an operator) */
            var p = out[out.length - 1];
            if (!p) return false;
            if (p.k === 'op') return false;
            if (p.k === 'punct' && (p.v === '@' || p.v === '::' || p.v === '(' || p.v === '[' || p.v === ',' || p.v === '/' || p.v === '//')) return false;
            return true;
        }
        while (i < n) {
            var c = s[i];
            if (c === ' ' || c === '\t' || c === '\n' || c === '\r') { i++; continue; }
            if (c === '"' || c === "'") {
                var e = s.indexOf(c, i + 1);
                if (e < 0) throw syntaxError(s);
                out.push({ k: 'str', v: s.slice(i + 1, e) });
                i = e + 1;
                continue;
            }
            if ((c >= '0' && c <= '9') || (c === '.' && s[i + 1] >= '0' && s[i + 1] <= '9')) {
                var j = i;
                while (j < n && s[j] >= '0' && s[j] <= '9') j++;
                if (s[j] === '.') { j++; while (j < n && s[j] >= '0' && s[j] <= '9') j++; }
                out.push({ k: 'num', v: Number(s.slice(i, j)) });
                i = j;
                continue;
            }
            var two = s.substr(i, 2);
            if (two === '//' || two === '::' || two === '..' || two === '!=' || two === '<=' || two === '>=') {
                out.push({ k: two === '!=' || two === '<=' || two === '>=' ? 'op' : 'punct', v: two });
                i += 2;
                continue;
            }
            if (c === '*') {
                if (prevAllowsOperator()) out.push({ k: 'op', v: '*' });
                else out.push({ k: 'name', v: '*' });
                i++;
                continue;
            }
            if ('=<>+-|'.indexOf(c) >= 0) { out.push({ k: 'op', v: c }); i++; continue; }
            if ('()[]@,/.'.indexOf(c) >= 0) { out.push({ k: 'punct', v: c }); i++; continue; }
            if (c === '$') {
                i++;
                var vs = i;
                while (i < n && (NAME_CHAR.test(s[i]) || s[i] === ':')) i++;
                if (i === vs) throw syntaxError(s);
                out.push({ k: 'var', v: s.slice(vs, i) });
                continue;
            }
            if (NAME_START.test(c)) {
                var st = i;
                while (i < n && NAME_CHAR.test(s[i])) i++;
                var name = s.slice(st, i);
                /* (prefix:local or prefix:*) */
                if (s[i] === ':' && s[i + 1] !== ':') {
                    if (s[i + 1] === '*') { name += ':*'; i += 2; }
                    else if (NAME_START.test(s[i + 1] || '')) {
                        i++;
                        while (i < n && NAME_CHAR.test(s[i])) i++;
                        name = s.slice(st, i);
                    } else {
                        throw syntaxError(s);
                    }
                }
                if (OPERATOR_NAMES[name] && prevAllowsOperator()) out.push({ k: 'op', v: name });
                else out.push({ k: 'name', v: name });
                continue;
            }
            throw syntaxError(s);
        }
        return out;
    }

    /* ---- parser (XPath 1.0 grammar, recursive descent) ---- */
    function parse(src, resolve) {
        var toks = tokenize(src), p = 0;
        function peek(o) { return toks[p + (o || 0)]; }
        function is(k, v, o) { var t = peek(o); return !!t && t.k === k && (v === undefined || t.v === v); }
        function take(k, v) {
            if (!is(k, v)) throw syntaxError(src);
            return toks[p++];
        }
        function binary(next, ops) {
            var left = next();
            while (peek() && peek().k === 'op' && ops.indexOf(peek().v) >= 0) {
                var op = toks[p++].v;
                left = { t: 'bin', op: op, a: left, b: next() };
            }
            return left;
        }
        function orExpr() { return binary(andExpr, ['or']); }
        function andExpr() { return binary(eqExpr, ['and']); }
        function eqExpr() { return binary(relExpr, ['=', '!=']); }
        function relExpr() { return binary(addExpr, ['<', '>', '<=', '>=']); }
        function addExpr() { return binary(mulExpr, ['+', '-']); }
        function mulExpr() { return binary(unaryExpr, ['*', 'div', 'mod']); }
        function unaryExpr() {
            if (is('op', '-')) { p++; return { t: 'neg', a: unaryExpr() }; }
            return unionExpr();
        }
        function unionExpr() { return binary(pathExpr, ['|']); }
        function startsFilter() {
            var t = peek();
            if (!t) return false;
            if (t.k === 'var' || t.k === 'str' || t.k === 'num' || (t.k === 'punct' && t.v === '(')) return true;
            /* (a function call: a name followed by '(' that is not a node type) */
            return t.k === 'name' && t.v !== '*' && is('punct', '(', 1) && !NODE_TYPES[t.v];
        }
        function pathExpr() {
            if (startsFilter()) {
                var f = filterExpr();
                if (is('punct', '/') || is('punct', '//')) {
                    var steps = [];
                    relPath(steps);
                    return { t: 'path', filter: f, abs: false, steps: steps };
                }
                return f;
            }
            return locationPath();
        }
        function filterExpr() {
            var e = primary(), preds = predicates();
            return preds.length ? { t: 'filter', e: e, preds: preds } : e;
        }
        function primary() {
            var t = toks[p++];
            if (!t) throw syntaxError(src);
            if (t.k === 'var') return { t: 'var', name: t.v };
            if (t.k === 'str') return { t: 'lit', v: t.v };
            if (t.k === 'num') return { t: 'lit', v: t.v };
            if (t.k === 'punct' && t.v === '(') { var e = orExpr(); take('punct', ')'); return e; }
            if (t.k === 'name') {
                take('punct', '(');
                var args = [];
                if (!is('punct', ')')) {
                    args.push(orExpr());
                    while (is('punct', ',')) { p++; args.push(orExpr()); }
                }
                take('punct', ')');
                if (!FUNCTIONS[t.v]) throw syntaxError(src);
                return { t: 'call', name: t.v, args: args };
            }
            throw syntaxError(src);
        }
        function predicates() {
            var out = [];
            while (is('punct', '[')) { p++; out.push(orExpr()); take('punct', ']'); }
            return out;
        }
        function locationPath() {
            var steps = [];
            if (is('punct', '/')) {
                p++;
                if (startsStep()) relPath(steps, true);
                return { t: 'path', abs: true, steps: steps };
            }
            if (is('punct', '//')) {
                p++;
                steps.push({ axis: 'descendant-or-self', test: { k: 'node' }, preds: [] });
                relPath(steps, true);
                return { t: 'path', abs: true, steps: steps };
            }
            relPath(steps, true);
            return { t: 'path', abs: false, steps: steps };
        }
        function startsStep() {
            var t = peek();
            return !!t && (t.k === 'name' || (t.k === 'punct' && (t.v === '@' || t.v === '.' || t.v === '..')));
        }
        /* steps separated by / and //; first: no leading separator */
        function relPath(steps, first) {
            if (first) steps.push(step());
            while (is('punct', '/') || is('punct', '//')) {
                if (toks[p++].v === '//') steps.push({ axis: 'descendant-or-self', test: { k: 'node' }, preds: [] });
                steps.push(step());
            }
        }
        function step() {
            if (is('punct', '.')) { p++; return { axis: 'self', test: { k: 'node' }, preds: [] }; }
            if (is('punct', '..')) { p++; return { axis: 'parent', test: { k: 'node' }, preds: [] }; }
            var axis = 'child';
            if (is('punct', '@')) { p++; axis = 'attribute'; }
            else if (is('name') && is('punct', '::', 1)) {
                axis = toks[p].v;
                if (!AXES[axis]) throw syntaxError(src);
                p += 2;
            }
            return { axis: axis, test: nodeTest(), preds: predicates() };
        }
        function nodeTest() {
            var t = take('name');
            if (NODE_TYPES[t.v] && is('punct', '(')) {
                p++;
                var lit = null;
                if (t.v === 'processing-instruction' && is('str')) lit = toks[p++].v;
                take('punct', ')');
                return { k: t.v === 'processing-instruction' ? 'pi' : t.v, lit: lit };
            }
            if (t.v === '*') return { k: 'any' };
            var colon = t.v.indexOf(':');
            if (colon < 0) return { k: 'name', ns: null, local: t.v };
            var prefix = t.v.slice(0, colon), local = t.v.slice(colon + 1);
            var ns = resolve(prefix);
            if (ns === null || ns === undefined || ns === '')
                throw domError("The prefix '" + prefix + "' cannot be resolved.", 'NamespaceError');
            return local === '*' ? { k: 'nsany', ns: ns } : { k: 'name', ns: ns, local: local, prefixed: true };
        }
        var tree = orExpr();
        if (p !== toks.length) throw syntaxError(src);
        return tree;
    }

    /* ---- values ---- */
    function isNodeSet(v) { return Array.isArray(v); }
    function nodeOwner(n) { return n.nodeType === 9 ? n : n.ownerDocument; }
    function stringValue(n) {
        switch (n.nodeType) {
        case 1: case 11: case 9: {
            var root = n.nodeType === 9 ? n.documentElement : n;
            if (!root) return '';
            var s = '';
            for (var c = root.firstChild; c; c = next(c, root))
                if (c.nodeType === 3 || c.nodeType === 4) s += c.data;
            return s;
        }
        case 2: return n.value;
        default: return n.nodeValue == null ? '' : String(n.nodeValue);
        }
    }
    /* the node after n in tree order, within root (n's descendants first) */
    function next(n, root) {
        if (n.firstChild) return n.firstChild;
        while (n && n !== root) {
            if (n.nextSibling) return n.nextSibling;
            n = n.parentNode;
        }
        return null;
    }
    function toStr(v) {
        if (isNodeSet(v)) return v.length ? stringValue(v[0]) : '';
        if (typeof v === 'number') {
            if (v !== v) return 'NaN';
            if (v === 0) return '0';
            if (!isFinite(v)) return v > 0 ? 'Infinity' : '-Infinity';
            if (Math.abs(v) >= 1e21 || Math.abs(v) < 1e-6) {
                /* (no exponent form in XPath) */
                var neg = v < 0, a = Math.abs(v), s = a.toFixed(20).replace(/\.?0+$/, '');
                if (a >= 1e21) s = BigInt(Math.round(a)).toString();
                return (neg ? '-' : '') + s;
            }
            return String(v);
        }
        return String(v);
    }
    function toNum(v) {
        if (typeof v === 'number') return v;
        if (typeof v === 'boolean') return v ? 1 : 0;
        var s = toStr(v).replace(/^[\x20\x09\x0D\x0A]+|[\x20\x09\x0D\x0A]+$/g, '');
        return /^-?(\d+(\.\d*)?|\.\d+)$/.test(s) ? Number(s) : NaN;
    }
    function toBool(v) {
        if (isNodeSet(v)) return v.length > 0;
        if (typeof v === 'number') return v !== 0 && v === v;
        if (typeof v === 'string') return v.length > 0;
        return !!v;
    }

    /* ---- document order ---- */
    function attrOwner(n) { return n.nodeType === 2 ? n.ownerElement : null; }
    function compareOrder(a, b) {
        if (a === b) return 0;
        var ae = attrOwner(a), be = attrOwner(b);
        /* (an attribute comes after its element and before its children) */
        if (ae || be) {
            var ak = ae || a, bk = be || b;
            if (ak === bk) {
                if (!ae) return -1;
                if (!be) return 1;
                var at = ak.attributes;
                for (var i = 0; i < at.length; i++) {
                    if (at[i] === a) return -1;
                    if (at[i] === b) return 1;
                }
                return 0;
            }
            a = ak;
            b = bk;
        }
        var pos = a.compareDocumentPosition(b);
        if (pos & 4) return -1;     /* b follows a */
        if (pos & 2) return 1;
        return 0;
    }
    function sortUnique(nodes) {
        if (nodes.length < 2) return nodes;
        nodes.sort(compareOrder);
        var out = [nodes[0]];
        for (var i = 1; i < nodes.length; i++) if (nodes[i] !== out[out.length - 1]) out.push(nodes[i]);
        return out;
    }

    /* ---- node tests ---- */
    function htmlCaseless(n) {
        var d = nodeOwner(n);
        return n.nodeType === 1 && n.namespaceURI === HTML_NS && d && d.contentType === 'text/html';
    }
    function principalType(axis) { return axis === 'attribute' ? 2 : 1; }
    function matches(n, test, axis) {
        switch (test.k) {
        case 'node': return true;
        case 'text': return n.nodeType === 3 || n.nodeType === 4;
        case 'comment': return n.nodeType === 8;
        case 'pi': return n.nodeType === 7 && (test.lit === null || n.target === test.lit);
        case 'any': return n.nodeType === principalType(axis);
        case 'nsany': return n.nodeType === principalType(axis) && n.namespaceURI === test.ns;
        case 'name': {
            if (n.nodeType !== principalType(axis)) return false;
            if (n.nodeType === 1 && htmlCaseless(n) && !test.prefixed)
                return n.localName === test.local.toLowerCase();
            if (n.localName !== test.local) return false;
            return (n.namespaceURI || null) === (test.ns || null);
        }
        }
        return false;
    }

    /* ---- axes: nodes in axis order (reverse axes nearest first) ---- */
    function axisNodes(n, axis) {
        var out = [], c;
        switch (axis) {
        case 'self': out.push(n); break;
        case 'child':
            if (n.nodeType !== 2) for (c = n.firstChild; c; c = c.nextSibling) out.push(c);
            break;
        case 'descendant-or-self':
            out.push(n);
            /* falls through */
        case 'descendant':
            if (n.nodeType !== 2) for (c = n.firstChild; c; c = next(c, n)) out.push(c);
            break;
        case 'parent':
            c = n.nodeType === 2 ? n.ownerElement : n.parentNode;
            if (c) out.push(c);
            break;
        case 'ancestor-or-self':
            out.push(n);
            /* falls through */
        case 'ancestor':
            for (c = n.nodeType === 2 ? n.ownerElement : n.parentNode; c; c = c.parentNode) out.push(c);
            break;
        case 'following-sibling':
            if (n.nodeType !== 2) for (c = n.nextSibling; c; c = c.nextSibling) out.push(c);
            break;
        case 'preceding-sibling':
            if (n.nodeType !== 2) for (c = n.previousSibling; c; c = c.previousSibling) out.push(c);
            break;
        case 'following': {
            var start = n.nodeType === 2 ? n.ownerElement : n;
            if (!start) break;
            /* (after n's subtree; an attribute's: its element's descendants too) */
            var root = start;
            while (root.parentNode) root = root.parentNode;
            if (n.nodeType === 2) c = next(start, root);
            else {
                c = start;
                while (c && !c.nextSibling) c = c.parentNode;
                c = c ? c.nextSibling : null;
            }
            for (; c; c = next(c, root)) out.push(c);
            break;
        }
        case 'preceding': {
            var s0 = n.nodeType === 2 ? n.ownerElement : n;
            if (!s0) break;
            var anc = [];
            for (c = s0.parentNode; c; c = c.parentNode) anc.push(c);
            var r = anc.length ? anc[anc.length - 1] : s0, all = [];
            for (c = r; c && c !== s0; c = next(c, r)) if (anc.indexOf(c) < 0) all.push(c);
            for (var i = all.length - 1; i >= 0; i--) out.push(all[i]);
            break;
        }
        case 'attribute':
            if (n.nodeType === 1) {
                var at = n.attributes;
                for (var k = 0; k < at.length; k++)
                    if (at[k].namespaceURI !== 'http://www.w3.org/2000/xmlns/') out.push(at[k]);
            }
            break;
        case 'namespace': break;
        }
        return out;
    }
    var REVERSE = { ancestor: 1, 'ancestor-or-self': 1, preceding: 1, 'preceding-sibling': 1 };

    /* ---- evaluation ---- */
    function evalStep(nodes, st, ctx) {
        var out = [];
        for (var i = 0; i < nodes.length; i++) {
            var cand = axisNodes(nodes[i], st.axis), sel = [];
            for (var j = 0; j < cand.length; j++) if (matches(cand[j], st.test, st.axis)) sel.push(cand[j]);
            /* (predicates count positions in axis order) */
            for (var k = 0; k < st.preds.length; k++) sel = filterBy(sel, st.preds[k], ctx);
            for (j = 0; j < sel.length; j++) out.push(sel[j]);
        }
        return nodes.length > 1 || REVERSE[st.axis] ? sortUnique(out) : out;
    }
    function filterBy(nodes, pred, ctx) {
        var out = [], size = nodes.length;
        for (var i = 0; i < size; i++) {
            var v = evaluate(pred, { node: nodes[i], pos: i + 1, size: size, vars: ctx.vars });
            if (typeof v === 'number' ? v === i + 1 : toBool(v)) out.push(nodes[i]);
        }
        return out;
    }
    function rootOf(n) {
        if (n.nodeType === 2) n = n.ownerElement || n;
        while (n.parentNode) n = n.parentNode;
        return n;
    }
    function compare(op, a, b) {
        /* (XPath 3.4: node-sets compare by their nodes' string values) */
        if (isNodeSet(a) || isNodeSet(b)) {
            if (isNodeSet(a) && isNodeSet(b)) {
                for (var i = 0; i < a.length; i++) {
                    var sa = stringValue(a[i]);
                    for (var j = 0; j < b.length; j++) if (cmpAtoms(op, sa, stringValue(b[j]))) return true;
                }
                return false;
            }
            var set = isNodeSet(a) ? a : b, other = isNodeSet(a) ? b : a, swap = !isNodeSet(a);
            if (typeof other === 'boolean') return cmpAtoms(op, swap ? other : toBool(set), swap ? toBool(set) : other);
            for (var k = 0; k < set.length; k++) {
                var sv = typeof other === 'number' ? toNum(stringValue(set[k])) : stringValue(set[k]);
                if (cmpAtoms(op, swap ? other : sv, swap ? sv : other)) return true;
            }
            return false;
        }
        return cmpAtoms(op, a, b);
    }
    function cmpAtoms(op, a, b) {
        if (op === '=' || op === '!=') {
            var eq;
            if (typeof a === 'boolean' || typeof b === 'boolean') eq = toBool(a) === toBool(b);
            else if (typeof a === 'number' || typeof b === 'number') eq = toNum(a) === toNum(b);
            else eq = toStr(a) === toStr(b);
            return op === '=' ? eq : !eq;
        }
        var x = toNum(a), y = toNum(b);
        return op === '<' ? x < y : op === '>' ? x > y : op === '<=' ? x <= y : x >= y;
    }
    function evaluate(e, ctx) {
        switch (e.t) {
        case 'lit': return e.v;
        case 'var': throw domError('Variables are not supported.', 'NamespaceError');
        case 'neg': return -toNum(evaluate(e.a, ctx));
        case 'bin': {
            if (e.op === 'or') return toBool(evaluate(e.a, ctx)) || toBool(evaluate(e.b, ctx));
            if (e.op === 'and') return toBool(evaluate(e.a, ctx)) && toBool(evaluate(e.b, ctx));
            var a = evaluate(e.a, ctx), b = evaluate(e.b, ctx);
            switch (e.op) {
            case '|':
                if (!isNodeSet(a) || !isNodeSet(b)) throw new TypeError('The operands of | must be node-sets.');
                return sortUnique(a.concat(b));
            case '+': return toNum(a) + toNum(b);
            case '-': return toNum(a) - toNum(b);
            case '*': return toNum(a) * toNum(b);
            case 'div': return toNum(a) / toNum(b);
            case 'mod': return toNum(a) % toNum(b);
            default: return compare(e.op, a, b);
            }
        }
        case 'call': return FUNCTIONS[e.name](ctx, e.args);
        case 'filter': {
            var v = evaluate(e.e, ctx);
            if (!isNodeSet(v)) throw new TypeError('A predicate needs a node-set.');
            for (var i = 0; i < e.preds.length; i++) v = filterBy(v, e.preds[i], ctx);
            return v;
        }
        case 'path': {
            var nodes;
            if (e.filter) {
                nodes = evaluate(e.filter, ctx);
                if (!isNodeSet(nodes)) throw new TypeError('A path needs a node-set.');
            } else {
                nodes = [e.abs ? rootOf(ctx.node) : ctx.node];
            }
            for (var s = 0; s < e.steps.length; s++) nodes = evalStep(nodes, e.steps[s], ctx);
            return nodes;
        }
        }
        throw new TypeError('bad expression');
    }

    /* ---- the core function library (XPath 4) ---- */
    function arg(ctx, args, i) { return evaluate(args[i], ctx); }
    function argStr(ctx, args, i) { return i < args.length ? toStr(arg(ctx, args, i)) : stringValue(ctx.node); }
    function argSet(ctx, args, i) {
        if (i >= args.length) return [ctx.node];
        var v = arg(ctx, args, i);
        if (!isNodeSet(v)) throw new TypeError('A node-set was expected.');
        return v;
    }
    function need(args, lo, hi) {
        if (args.length < lo || args.length > hi) throw new TypeError('Wrong number of arguments.');
    }
    var FUNCTIONS = {
        last: function (ctx, a) { need(a, 0, 0); return ctx.size; },
        position: function (ctx, a) { need(a, 0, 0); return ctx.pos; },
        count: function (ctx, a) { need(a, 1, 1); return argSet(ctx, a, 0).length; },
        id: function (ctx, a) {
            need(a, 1, 1);
            var v = arg(ctx, a, 0), words = [];
            if (isNodeSet(v)) for (var i = 0; i < v.length; i++) words = words.concat(stringValue(v[i]).split(/[\x20\x09\x0D\x0A]+/));
            else words = toStr(v).split(/[\x20\x09\x0D\x0A]+/);
            var root = rootOf(ctx.node), out = [];
            for (var k = 0; k < words.length; k++) {
                if (!words[k]) continue;
                var el = root.getElementById ? root.getElementById(words[k]) : null;
                if (!el && root.querySelector) {
                    try { el = root.querySelector('[id="' + words[k].replace(/["\\]/g, '\\$&') + '"]'); } catch (e) { el = null; }
                }
                if (el) out.push(el);
            }
            return sortUnique(out);
        },
        'local-name': function (ctx, a) {
            need(a, 0, 1);
            var s = argSet(ctx, a, 0);
            if (!s.length) return '';
            var n = sortUnique(s.slice())[0];
            return n.nodeType === 1 || n.nodeType === 2 ? n.localName : n.nodeType === 7 ? n.target : '';
        },
        'namespace-uri': function (ctx, a) {
            need(a, 0, 1);
            var s = argSet(ctx, a, 0);
            if (!s.length) return '';
            var n = sortUnique(s.slice())[0];
            return (n.nodeType === 1 || n.nodeType === 2) && n.namespaceURI ? n.namespaceURI : '';
        },
        name: function (ctx, a) {
            need(a, 0, 1);
            var s = argSet(ctx, a, 0);
            if (!s.length) return '';
            var n = sortUnique(s.slice())[0];
            if (n.nodeType === 1) return n.prefix ? n.prefix + ':' + n.localName : n.localName;
            return n.nodeType === 2 ? n.name : n.nodeType === 7 ? n.target : '';
        },
        string: function (ctx, a) { need(a, 0, 1); return argStr(ctx, a, 0); },
        concat: function (ctx, a) {
            if (a.length < 2) throw new TypeError('Wrong number of arguments.');
            var s = '';
            for (var i = 0; i < a.length; i++) s += toStr(arg(ctx, a, i));
            return s;
        },
        'starts-with': function (ctx, a) { need(a, 2, 2); return argStr(ctx, a, 0).indexOf(argStr(ctx, a, 1)) === 0; },
        contains: function (ctx, a) { need(a, 2, 2); return argStr(ctx, a, 0).indexOf(argStr(ctx, a, 1)) >= 0; },
        'substring-before': function (ctx, a) {
            need(a, 2, 2);
            var s = argStr(ctx, a, 0), i = s.indexOf(argStr(ctx, a, 1));
            return i < 0 ? '' : s.slice(0, i);
        },
        'substring-after': function (ctx, a) {
            need(a, 2, 2);
            var s = argStr(ctx, a, 0), t = argStr(ctx, a, 1), i = s.indexOf(t);
            return i < 0 ? '' : s.slice(i + t.length);
        },
        substring: function (ctx, a) {
            need(a, 2, 3);
            var s = argStr(ctx, a, 0), chars = Array.from(s);
            var start = Math.round(toNum(arg(ctx, a, 1)));
            var end = a.length > 2 ? start + Math.round(toNum(arg(ctx, a, 2))) : Infinity;
            var out = '';
            for (var i = 0; i < chars.length; i++) if (i + 1 >= start && i + 1 < end) out += chars[i];
            return out;
        },
        'string-length': function (ctx, a) { need(a, 0, 1); return Array.from(argStr(ctx, a, 0)).length; },
        'normalize-space': function (ctx, a) {
            need(a, 0, 1);
            return argStr(ctx, a, 0).replace(/[\x20\x09\x0D\x0A]+/g, ' ').replace(/^ | $/g, '');
        },
        translate: function (ctx, a) {
            need(a, 3, 3);
            var s = Array.from(argStr(ctx, a, 0)), from = Array.from(argStr(ctx, a, 1)), to = Array.from(argStr(ctx, a, 2));
            var out = '';
            for (var i = 0; i < s.length; i++) {
                var k = from.indexOf(s[i]);
                out += k < 0 ? s[i] : k < to.length ? to[k] : '';
            }
            return out;
        },
        boolean: function (ctx, a) { need(a, 1, 1); return toBool(arg(ctx, a, 0)); },
        not: function (ctx, a) { need(a, 1, 1); return !toBool(arg(ctx, a, 0)); },
        'true': function (ctx, a) { need(a, 0, 0); return true; },
        'false': function (ctx, a) { need(a, 0, 0); return false; },
        lang: function (ctx, a) {
            need(a, 1, 1);
            var want = argStr(ctx, a, 0).toLowerCase();
            for (var n = ctx.node.nodeType === 2 ? ctx.node.ownerElement : ctx.node; n; n = n.parentNode) {
                if (n.nodeType !== 1) continue;
                var l = n.getAttributeNS('http://www.w3.org/XML/1998/namespace', 'lang');
                if (l === null && htmlCaseless(n)) l = n.getAttribute('lang');
                if (l === null) continue;
                l = l.toLowerCase();
                return l === want || l.indexOf(want + '-') === 0;
            }
            return false;
        },
        number: function (ctx, a) { need(a, 0, 1); return a.length ? toNum(arg(ctx, a, 0)) : toNum(stringValue(ctx.node)); },
        sum: function (ctx, a) {
            need(a, 1, 1);
            var s = argSet(ctx, a, 0), t = 0;
            for (var i = 0; i < s.length; i++) t += toNum(stringValue(s[i]));
            return t;
        },
        floor: function (ctx, a) { need(a, 1, 1); return Math.floor(toNum(arg(ctx, a, 0))); },
        ceiling: function (ctx, a) { need(a, 1, 1); return Math.ceil(toNum(arg(ctx, a, 0))); },
        round: function (ctx, a) {
            need(a, 1, 1);
            var v = toNum(arg(ctx, a, 0));
            if (v !== v || !isFinite(v)) return v;
            if (v < 0 && v >= -0.5) return -0;
            return Math.floor(v + 0.5);
        }
    };

    /* ---- the interfaces ---- */
    function illegal() { throw new TypeError('Illegal constructor'); }
    function isNode(v) { return v !== null && typeof v === 'object' && typeof v.nodeType === 'number' && global.Node && v instanceof global.Node; }
    function needNode(v, what) {
        if (!isNode(v)) throw new TypeError("Failed to execute '" + what + "': parameter is not of type 'Node'.");
        return v;
    }

    var XPathResult = ({ XPathResult() { illegal(); } }).XPathResult;
    XPathResult.prototype = Object.create(Object.prototype);
    var RP = XPathResult.prototype;
    var TYPES = ['ANY_TYPE', 'NUMBER_TYPE', 'STRING_TYPE', 'BOOLEAN_TYPE', 'UNORDERED_NODE_ITERATOR_TYPE',
                 'ORDERED_NODE_ITERATOR_TYPE', 'UNORDERED_NODE_SNAPSHOT_TYPE', 'ORDERED_NODE_SNAPSHOT_TYPE',
                 'ANY_UNORDERED_NODE_TYPE', 'FIRST_ORDERED_NODE_TYPE'];
    for (var ti = 0; ti < TYPES.length; ti++) {
        Object.defineProperty(XPathResult, TYPES[ti], { value: ti, enumerable: true });
        Object.defineProperty(RP, TYPES[ti], { value: ti, enumerable: true });
    }
    function rs(r) {
        var s = r !== null && typeof r === 'object' ? r[STATE] : undefined;
        if (!s || s.kind !== 'result') throw new TypeError('Illegal invocation');
        return s;
    }
    function getter(P, name, fn) {
        Object.defineProperty(fn, 'name', { value: 'get ' + name });
        Object.defineProperty(P, name, { get: fn, configurable: true, enumerable: true });
    }
    getter(RP, 'resultType', function () { return rs(this).type; });
    getter(RP, 'numberValue', function () {
        var s = rs(this);
        if (s.type !== 1) throw new TypeError('The result is not a number.');
        return s.value;
    });
    getter(RP, 'stringValue', function () {
        var s = rs(this);
        if (s.type !== 2) throw new TypeError('The result is not a string.');
        return s.value;
    });
    getter(RP, 'booleanValue', function () {
        var s = rs(this);
        if (s.type !== 3) throw new TypeError('The result is not a boolean.');
        return s.value;
    });
    getter(RP, 'singleNodeValue', function () {
        var s = rs(this);
        if (s.type !== 8 && s.type !== 9) throw new TypeError('The result is not a single node.');
        return s.nodes.length ? s.nodes[0] : null;
    });
    /* (an iterator is not invalidated by later changes to the document: the
     * nodes it returns are the ones the evaluation found) */
    getter(RP, 'invalidIteratorState', function () { rs(this); return false; });
    getter(RP, 'snapshotLength', function () {
        var s = rs(this);
        if (s.type !== 6 && s.type !== 7) throw new TypeError('The result is not a snapshot.');
        return s.nodes.length;
    });
    define(RP, 'iterateNext', function iterateNext() {
        var s = rs(this);
        if (s.type !== 4 && s.type !== 5) throw new TypeError('The result is not an iterator.');
        return s.at < s.nodes.length ? s.nodes[s.at++] : null;
    }, true);
    define(RP, 'snapshotItem', function snapshotItem(index) {
        var s = rs(this);
        if (arguments.length < 1) throw new TypeError("Failed to execute 'snapshotItem' on 'XPathResult': 1 argument required.");
        if (s.type !== 6 && s.type !== 7) throw new TypeError('The result is not a snapshot.');
        index = index >>> 0;
        return index < s.nodes.length ? s.nodes[index] : null;
    }, true);
    Object.defineProperty(RP, Symbol.toStringTag, { value: 'XPathResult', configurable: true });
    define(RP, 'constructor', XPathResult);

    function makeResult(value, type) {
        if (type === 0) type = isNodeSet(value) ? 4 : typeof value === 'number' ? 1 : typeof value === 'string' ? 2 : 3;
        var st = { kind: 'result', type: type, value: null, nodes: [], at: 0 };
        switch (type) {
        case 1: st.value = toNum(value); break;
        case 2: st.value = toStr(value); break;
        case 3: st.value = toBool(value); break;
        case 4: case 5: case 6: case 7: case 8: case 9:
            if (!isNodeSet(value)) throw new TypeError('The expression does not give a node-set.');
            st.nodes = type === 8 || type === 9 ? value.slice(0, 1) : value.slice();
            break;
        default:
            throw domError('The result type is not supported.', 'NotSupportedError');
        }
        var r = Object.create(RP);
        Object.defineProperty(r, STATE, { value: st });
        return r;
    }

    function resolverFn(resolver) {
        if (resolver === null || resolver === undefined) return function () { return null; };
        return function (prefix) {
            var r;
            if (typeof resolver === 'function') r = resolver.call(undefined, prefix);
            else {
                var f = resolver.lookupNamespaceURI;
                if (typeof f !== 'function') throw new TypeError('The resolver has no lookupNamespaceURI.');
                r = f.call(resolver, prefix);
            }
            return r === null || r === undefined ? null : String(r);
        };
    }

    var XPathExpression = ({ XPathExpression() { illegal(); } }).XPathExpression;
    XPathExpression.prototype = Object.create(Object.prototype);
    var EXP = XPathExpression.prototype;
    function compile(expression, resolver) {
        var src = String(expression);
        var tree = parse(src, resolverFn(resolver));
        var x = Object.create(EXP);
        Object.defineProperty(x, STATE, { value: { kind: 'expression', tree: tree } });
        return x;
    }
    function run(x, contextNode, type, result) {
        var s = x !== null && typeof x === 'object' ? x[STATE] : undefined;
        if (!s || s.kind !== 'expression') throw new TypeError('Illegal invocation');
        needNode(contextNode, 'evaluate');
        type = type === undefined ? 0 : Number(type) & 0xFFFF;
        var v = evaluate(s.tree, { node: contextNode, pos: 1, size: 1, vars: null });
        if (isNodeSet(v) && (type === 5 || type === 7 || type === 9 || type === 0 || type === 4 || type === 6 || type === 8))
            v = sortUnique(v.slice());
        return makeResult(v, type);
    }
    define(EXP, 'evaluate', function evaluate(contextNode, type, result) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'evaluate' on 'XPathExpression': 1 argument required.");
        return run(this, contextNode, type, result);
    }, true);
    Object.defineProperty(EXP, Symbol.toStringTag, { value: 'XPathExpression', configurable: true });
    define(EXP, 'constructor', XPathExpression);

    /* XPathEvaluatorBase: on XPathEvaluator.prototype and Document.prototype */
    var base = {
        createExpression: function createExpression(expression, resolver) {
            if (arguments.length < 1) throw new TypeError("Failed to execute 'createExpression': 1 argument required.");
            return compile(expression, resolver);
        },
        createNSResolver: function createNSResolver(nodeResolver) {
            if (arguments.length < 1) throw new TypeError("Failed to execute 'createNSResolver': 1 argument required.");
            return needNode(nodeResolver, 'createNSResolver');
        },
        evaluate: function evaluate(expression, contextNode, resolver, type, result) {
            if (arguments.length < 2) throw new TypeError("Failed to execute 'evaluate': 2 arguments required.");
            needNode(contextNode, 'evaluate');
            return run(compile(expression, resolver), contextNode, type, result);
        }
    };
    var XPathEvaluator = function XPathEvaluator() {
        if (!new.target) throw new TypeError("Failed to construct 'XPathEvaluator': Please use the 'new' operator.");
    };
    var EVP = XPathEvaluator.prototype;
    Object.defineProperty(EVP, Symbol.toStringTag, { value: 'XPathEvaluator', configurable: true });
    var DocP = global.Document && global.Document.prototype;
    for (var bk in base) {
        define(EVP, bk, base[bk], true);
        if (DocP) define(DocP, bk, base[bk], true);
    }
    expose('XPathResult', XPathResult);
    expose('XPathExpression', XPathExpression);
    expose('XPathEvaluator', XPathEvaluator);
})

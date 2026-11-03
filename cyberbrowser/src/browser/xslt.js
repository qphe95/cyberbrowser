/*
 * xslt.js - XSLTProcessor (DOM Standard 9) and XSLT 1.0 over the DOM, on
 * xpath.js's evaluator (__cyberXPath).
 *
 * The stylesheet's templates are read once by importStylesheet.  A transform
 * walks the source: apply-templates picks the best match (import precedence
 * aside: the highest priority, then the last), and the instructions build
 * their output in a tree of the output document.  Not here: xsl:import and
 * xsl:include, xsl:key, xsl:number, document(), format-number(), xsl:output
 * (the output is what the processor's caller asks for).
 * Called as fn(global, window).
 */
(function (global, win) {
    'use strict';
    var XSL = 'http://www.w3.org/1999/XSL/Transform';
    var XMLNS = 'http://www.w3.org/2000/xmlns/';
    var STATE = Symbol('xslt');
    var xpath = global.__cyberXPath;

    function st(p) {
        var s = p !== null && typeof p === 'object' ? p[STATE] : undefined;
        if (!s) throw new TypeError('Illegal invocation');
        return s;
    }
    function isXsl(n, name) { return n.nodeType === 1 && n.namespaceURI === XSL && (!name || n.localName === name); }
    function attr(el, name) { return el.getAttributeNS(null, name); }
    function resolverOf(el) { return function (prefix) { return el.lookupNamespaceURI(prefix); }; }

    /* An expression in the context (node, position, size) with the variables. */
    function ev(expr, el, c) {
        return xpath(expr, resolverOf(el), { node: c.node, pos: c.pos, size: c.size, vars: c.vars });
    }
    function toStr(v) { return xpath.string(v); }
    function toBool(v) { return xpath.boolean(v); }
    function evStr(expr, el, c) { return toStr(ev(expr, el, c)); }
    /* An attribute value template. */
    function avt(text, el, c) {
        var out = '', i = 0;
        while (i < text.length) {
            var ch = text[i];
            if (ch === '{') {
                if (text[i + 1] === '{') { out += '{'; i += 2; continue; }
                var e = text.indexOf('}', i);
                if (e < 0) throw new Error('XSLT: unterminated attribute value template');
                out += evStr(text.slice(i + 1, e), el, c);
                i = e + 1;
                continue;
            }
            if (ch === '}' && text[i + 1] === '}') i++;
            out += text[i++];
        }
        return out;
    }

    /* ---- the stylesheet ---- */
    function defaultPriority(pattern) {
        if (pattern.indexOf('|') >= 0) return 0.5;
        if (/^(@?[\w-]+:?[\w-]*)$/.test(pattern) && pattern !== '*' ) return 0;
        if (/^(\*|@\*|node\(\)|text\(\)|comment\(\)|processing-instruction\(\))$/.test(pattern)) return -0.5;
        if (/^[\w-]+:\*$/.test(pattern)) return -0.25;
        return 0.5;
    }
    function readStylesheet(root) {
        var sheet = { templates: [], named: {}, globals: [], root: root };
        for (var n = root.firstChild; n; n = n.nextSibling) {
            if (!isXsl(n)) continue;
            if (n.localName === 'template') {
                var name = attr(n, 'name'), match = attr(n, 'match');
                if (name) sheet.named[name] = n;
                if (match) match.split('|').forEach(function (pat) {
                    pat = pat.trim();
                    var pr = attr(n, 'priority');
                    sheet.templates.push({ el: n, pattern: pat, mode: attr(n, 'mode') || '',
                                           priority: pr !== null ? Number(pr) : defaultPriority(pat) });
                });
            } else if (n.localName === 'variable' || n.localName === 'param') {
                sheet.globals.push(n);
            }
        }
        return sheet;
    }
    /* Does the node match the pattern: is it in the pattern's nodes from
     * itself or an ancestor? */
    function matches(pattern, el, node, vars) {
        if (pattern === '/') return node.nodeType === 9;
        var from = node.nodeType === 2 ? node.ownerElement : node;
        for (var a = from; a; a = a.parentNode) {
            var v;
            try { v = xpath(pattern, resolverOf(el), { node: a, pos: 1, size: 1, vars: vars }); }
            catch (e) { return false; }
            if (Array.isArray(v) && v.indexOf(node) >= 0) return true;
            /* (a step's children: from the parent, what it selects) */
        }
        return false;
    }
    function bestTemplate(tx, node, mode) {
        var best = null;
        tx.sheet.templates.forEach(function (t) {
            if (t.mode !== mode) return;
            if (best && t.priority < best.priority) return;
            if (matches(t.pattern, t.el, node, tx.globals)) best = t;
        });
        return best;
    }

    /* ---- the output ---- */
    function append(out, node) { out.appendChild(node); }
    function text(tx, out, s) { if (s !== '') append(out, tx.doc.createTextNode(s)); }
    function copyNode(tx, node, out) {
        switch (node.nodeType) {
        case 9: case 11:
            for (var c = node.firstChild; c; c = c.nextSibling) copyNode(tx, c, out);
            break;
        case 2:
            if (out.nodeType === 1) out.setAttributeNS(node.namespaceURI, node.name, node.value);
            break;
        default:
            append(out, tx.doc.importNode(node, true));
        }
    }

    /* ---- the instructions ---- */
    function body(tx, el, c, out) {
        var vars = Object.create(c.vars);
        var inner = { node: c.node, pos: c.pos, size: c.size, vars: vars };
        for (var n = el.firstChild; n; n = n.nextSibling) instruction(tx, n, inner, out);
    }
    function valueOf(tx, el, c) {
        var sel = attr(el, 'select');
        if (sel !== null) return ev(sel, el, c);
        var frag = tx.doc.createDocumentFragment();
        body(tx, el, c, frag);
        return frag.textContent;
    }
    function paramsFrom(tx, el, c) {
        var p = Object.create(null);
        for (var n = el.firstChild; n; n = n.nextSibling)
            if (isXsl(n, 'with-param')) p[attr(n, 'name')] = valueOf(tx, n, c);
        return p;
    }
    function sorted(tx, el, nodes, c) {
        var keys = [];
        for (var n = el.firstChild; n; n = n.nextSibling) if (isXsl(n, 'sort')) keys.push(n);
        if (!keys.length) return nodes;
        var size = nodes.length;
        var rows = nodes.map(function (node, i) {
            var ctx = { node: node, pos: i + 1, size: size, vars: c.vars };
            return { node: node, i: i, k: keys.map(function (s) { return evStr(attr(s, 'select') || '.', s, ctx); }) };
        });
        rows.sort(function (a, b) {
            for (var j = 0; j < keys.length; j++) {
                var numeric = (attr(keys[j], 'data-type') || 'text') === 'number';
                var r = numeric ? Number(a.k[j]) - Number(b.k[j]) : (a.k[j] < b.k[j] ? -1 : a.k[j] > b.k[j] ? 1 : 0);
                if (attr(keys[j], 'order') === 'descending') r = -r;
                if (r) return r;
            }
            return a.i - b.i;
        });
        return rows.map(function (r) { return r.node; });
    }
    function applyTo(tx, nodes, mode, params, out, vars) {
        var size = nodes.length;
        nodes.forEach(function (node, i) { applyOne(tx, node, i + 1, size, mode, params, out, vars); });
    }
    function childrenOf(node) {
        var out = [];
        for (var c = node.firstChild; c; c = c.nextSibling) out.push(c);
        return out;
    }
    function applyOne(tx, node, pos, size, mode, params, out, vars) {
        var t = bestTemplate(tx, node, mode);
        var c = { node: node, pos: pos, size: size, vars: tx.globals };
        if (t) { runTemplate(tx, t.el, c, params, out); return; }
        /* (the built-in rules) */
        switch (node.nodeType) {
        case 1: case 9: case 11: applyTo(tx, childrenOf(node), mode, Object.create(null), out, vars); break;
        case 2: case 3: case 4: text(tx, out, node.nodeType === 2 ? node.value : node.data); break;
        }
    }
    function runTemplate(tx, el, c, params, out) {
        if (++tx.depth > 3000) throw new Error('XSLT: templates nested too deep');
        var vars = Object.create(tx.globals);
        var inner = { node: c.node, pos: c.pos, size: c.size, vars: vars };
        for (var n = el.firstChild; n; n = n.nextSibling) {
            if (isXsl(n, 'param')) {
                var name = attr(n, 'name');
                vars[name] = params && name in params ? params[name] : valueOf(tx, n, inner);
            } else {
                instruction(tx, n, inner, out);
            }
        }
        tx.depth--;
    }
    function literalElement(tx, el, c, out) {
        var e = tx.doc.createElementNS(el.namespaceURI, el.prefix ? el.prefix + ':' + el.localName : el.localName);
        for (var i = 0; i < el.attributes.length; i++) {
            var a = el.attributes[i];
            if (a.namespaceURI === XMLNS || a.namespaceURI === XSL) continue;
            e.setAttributeNS(a.namespaceURI, a.name, avt(a.value, el, c));
        }
        
        append(out, e);
        body(tx, el, c, e);
    }
    function instruction(tx, n, c, out) {
        if (n.nodeType === 3 || n.nodeType === 4) {
            /* (white space alone in a stylesheet goes) */
            if (/\S/.test(n.data)) text(tx, out, n.data);
            return;
        }
        if (n.nodeType !== 1) return;
        if (n.namespaceURI !== XSL) { literalElement(tx, n, c, out); return; }
        var sel = attr(n, 'select');
        switch (n.localName) {
        case 'value-of': text(tx, out, evStr(sel, n, c)); break;
        case 'text': text(tx, out, n.textContent); break;
        case 'apply-templates': {
            var nodes = sel !== null ? ev(sel, n, c) : childrenOf(c.node);
            if (!Array.isArray(nodes)) throw new TypeError('XSLT: apply-templates needs a node-set');
            applyTo(tx, sorted(tx, n, nodes, c), attr(n, 'mode') || '', paramsFrom(tx, n, c), out, c.vars);
            break;
        }
        case 'call-template': {
            var t = tx.sheet.named[attr(n, 'name')];
            if (!t) throw new Error('XSLT: no template named ' + attr(n, 'name'));
            runTemplate(tx, t, c, paramsFrom(tx, n, c), out);
            break;
        }
        case 'for-each': {
            var set = ev(sel, n, c);
            if (!Array.isArray(set)) throw new TypeError('XSLT: for-each needs a node-set');
            var list = sorted(tx, n, set, c);
            list.forEach(function (node, i) {
                var inner = { node: node, pos: i + 1, size: list.length, vars: c.vars };
                for (var k = n.firstChild; k; k = k.nextSibling)
                    if (!isXsl(k, 'sort')) instruction(tx, k, inner, out);
            });
            break;
        }
        case 'if':
            if (toBool(ev(attr(n, 'test'), n, c))) body(tx, n, c, out);
            break;
        case 'choose':
            for (var w = n.firstChild; w; w = w.nextSibling) {
                if (isXsl(w, 'otherwise') || (isXsl(w, 'when') && toBool(ev(attr(w, 'test'), w, c)))) {
                    body(tx, w, c, out);
                    break;
                }
            }
            break;
        case 'copy': {
            var src = c.node;
            if (src.nodeType === 1) {
                var e = tx.doc.createElementNS(src.namespaceURI, src.prefix ? src.prefix + ':' + src.localName : src.localName);
                append(out, e);
                body(tx, n, c, e);
            } else if (src.nodeType === 9 || src.nodeType === 11) {
                body(tx, n, c, out);
            } else {
                copyNode(tx, src, out);
            }
            break;
        }
        case 'copy-of': {
            var v = ev(sel, n, c);
            if (Array.isArray(v)) v.forEach(function (x) { copyNode(tx, x, out); });
            else if (v && v.nodeType === 11) copyNode(tx, v, out);
            else text(tx, out, toStr(v));
            break;
        }
        case 'element': {
            var name = avt(attr(n, 'name'), n, c);
            var ns = attr(n, 'namespace');
            var made = tx.doc.createElementNS(ns !== null ? avt(ns, n, c) : (name.indexOf(':') >= 0 ? n.lookupNamespaceURI(name.split(':')[0]) : n.lookupNamespaceURI(null)), name);
            append(out, made);
            body(tx, n, c, made);
            break;
        }
        case 'attribute': {
            var an = avt(attr(n, 'name'), n, c);
            var ans = attr(n, 'namespace');
            var frag = tx.doc.createDocumentFragment();
            body(tx, n, c, frag);
            if (out.nodeType === 1) out.setAttributeNS(ans !== null ? avt(ans, n, c) : null, an, frag.textContent);
            break;
        }
        case 'comment': {
            var cf = tx.doc.createDocumentFragment();
            body(tx, n, c, cf);
            append(out, tx.doc.createComment(cf.textContent));
            break;
        }
        case 'processing-instruction': {
            var pf = tx.doc.createDocumentFragment();
            body(tx, n, c, pf);
            append(out, tx.doc.createProcessingInstruction(avt(attr(n, 'name'), n, c), pf.textContent));
            break;
        }
        case 'variable': case 'param':
            c.vars[attr(n, 'name')] = valueOf(tx, n, c);
            break;
        }
    }

    function transform(p, source, doc, out) {
        var s = st(p);
        if (!s.sheet) throw new (global.DOMException || Error)('No stylesheet was imported.', 'InvalidStateError');
        var tx = { sheet: s.sheet, doc: doc, depth: 0, globals: Object.create(null) };
        var gc = { node: source, pos: 1, size: 1, vars: tx.globals };
        s.sheet.globals.forEach(function (g) {
            var name = attr(g, 'name');
            var given = s.params[':' + name];
            tx.globals[name] = given !== undefined && g.localName === 'param' ? given : valueOf(tx, g, gc);
        });
        applyOne(tx, source, 1, 1, '', Object.create(null), out, tx.globals);
    }

    function needNode(v, method) {
        if (v === null || typeof v !== 'object' || typeof v.nodeType !== 'number')
            throw new TypeError("Failed to execute '" + method + "' on 'XSLTProcessor': parameter is not of type 'Node'.");
    }
    function need(n, count, method) {
        if (n < count) throw new TypeError("Failed to execute '" + method + "' on 'XSLTProcessor': " + count + ' argument(s) required.');
    }
    function XSLTProcessor() {
        if (!new.target) throw new TypeError("Failed to construct 'XSLTProcessor': Please use the 'new' operator.");
        Object.defineProperty(this, STATE, { value: { sheet: null, params: Object.create(null) } });
    }
    var methods = {
        importStylesheet: function importStylesheet(style) {
            need(arguments.length, 1, 'importStylesheet');
            needNode(style, 'importStylesheet');
            var s = st(this);
            var root = style.nodeType === 9 ? style.documentElement : style;
            if (root && root.namespaceURI === XSL && (root.localName === 'stylesheet' || root.localName === 'transform')) {
                s.sheet = readStylesheet(root);
            } else {
                /* (a literal result element as the stylesheet) */
                s.sheet = { templates: [{ el: { firstChild: root, lookupNamespaceURI: function () { return null; } }, pattern: '/', mode: '', priority: 0 }],
                            named: {}, globals: [], root: root };
            }
        },
        transformToFragment: function transformToFragment(source, output) {
            need(arguments.length, 2, 'transformToFragment');
            needNode(source, 'transformToFragment');
            needNode(output, 'transformToFragment');
            var frag = output.createDocumentFragment();
            transform(this, source, output, frag);
            return frag;
        },
        transformToDocument: function transformToDocument(source) {
            need(arguments.length, 1, 'transformToDocument');
            needNode(source, 'transformToDocument');
            var impl = (source.ownerDocument || source).implementation;
            var doc = impl.createDocument(null, '', null);
            var holder = doc.createDocumentFragment();
            transform(this, source, doc, holder);
            doc.appendChild(holder);
            return doc;
        },
        setParameter: function setParameter(ns, name, value) {
            need(arguments.length, 3, 'setParameter');
            st(this).params[(ns === null ? '' : String(ns)) + ':' + String(name)] = value;
        },
        getParameter: function getParameter(ns, name) {
            need(arguments.length, 2, 'getParameter');
            var v = st(this).params[(ns === null ? '' : String(ns)) + ':' + String(name)];
            return v === undefined ? null : v;
        },
        removeParameter: function removeParameter(ns, name) {
            need(arguments.length, 2, 'removeParameter');
            delete st(this).params[(ns === null ? '' : String(ns)) + ':' + String(name)];
        },
        clearParameters: function clearParameters() { st(this).params = Object.create(null); },
        reset: function reset() { var s = st(this); s.sheet = null; s.params = Object.create(null); }
    };
    Object.keys(methods).forEach(function (k) {
        Object.defineProperty(XSLTProcessor.prototype, k, { value: methods[k], writable: true, configurable: true, enumerable: true });
    });
    Object.defineProperty(XSLTProcessor.prototype, Symbol.toStringTag, { value: 'XSLTProcessor', configurable: true });
    Object.defineProperty(XSLTProcessor, 'prototype', { writable: false });
    [global, win].forEach(function (o) {
        if (o) Object.defineProperty(o, 'XSLTProcessor', { value: XSLTProcessor, writable: true, configurable: true, enumerable: false });
    });
})

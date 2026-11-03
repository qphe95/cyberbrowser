/*
 * platform_api.js - pieces of the web platform kept in JS, installed in each
 * realm by platform_api_install (browser/platform_api.c):
 *   - NodeList and HTMLCollection (DOM Standard section 4.2.10): the
 *     prototypes of the node lists the DOM returns (arrays underneath);
 *   - NodeFilter (DOM Standard section 6; the traversals are in traversal.c);
 *   - the HTML element interfaces the browser does not define natively
 *     (HTMLParagraphElement, HTMLLIElement, ...), and the table API
 *     (HTML 4.9: rows, cells, insertRow, insertCell, ...);
 *   - the User Timing marks and measures (User Timing Level 3) of
 *     performance, over performance.now().
 * Called as fn(global, window).
 */
(function (global, win) {
    'use strict';
    function define(obj, name, value) {
        Object.defineProperty(obj, name, { value: value, writable: true, configurable: true });
    }
    function expose(name, value) {
        define(global, name, value);
        if (win && win !== global) define(win, name, value);
    }
    function illegal() { throw new TypeError('Illegal constructor'); }

    /* ---- NodeList / HTMLCollection ---- */
    function list(name, methods) {
        var C = function () { illegal(); };
        Object.defineProperty(C, 'name', { value: name });
        var P = C.prototype, A = Array.prototype;
        define(P, 'item', function item(i) { i = i >>> 0; return i < this.length ? this[i] : null; });
        for (var i = 0; i < methods.length; i++) define(P, methods[i], A[methods[i]]);
        define(P, Symbol.iterator, A.values);
        Object.defineProperty(P, Symbol.toStringTag, { value: name, configurable: true });
        expose(name, C);
        return C;
    }
    list('NodeList', ['forEach', 'entries', 'keys', 'values']);
    var HTMLCollection = list('HTMLCollection', []);
    define(HTMLCollection.prototype, 'namedItem', function namedItem(n) {
        n = String(n);
        for (var i = 0; i < this.length; i++) {
            var e = this[i];
            if (e.id === n || (e.getAttribute && e.getAttribute('name') === n)) return e;
        }
        return null;
    });

    /* ---- element interfaces ---- */
    var HE = global.HTMLElement;
    if (HE) [
        'HTMLAnchorElement', 'HTMLAreaElement', 'HTMLBaseElement', 'HTMLQuoteElement', 'HTMLBodyElement', 'HTMLBRElement', 'HTMLTableCaptionElement',
        'HTMLTableColElement', 'HTMLDataElement', 'HTMLDataListElement', 'HTMLModElement',
        'HTMLDetailsElement', 'HTMLDialogElement', 'HTMLDListElement', 'HTMLEmbedElement',
        'HTMLFieldSetElement', 'HTMLHeadingElement', 'HTMLHeadElement', 'HTMLHRElement', 'HTMLHtmlElement',
        'HTMLLabelElement', 'HTMLLegendElement', 'HTMLLIElement', 'HTMLMapElement', 'HTMLMenuElement',
        'HTMLMetaElement', 'HTMLMeterElement', 'HTMLObjectElement', 'HTMLOListElement', 'HTMLOptGroupElement',
        'HTMLOutputElement', 'HTMLParagraphElement', 'HTMLPictureElement', 'HTMLPreElement',
        'HTMLProgressElement', 'HTMLSourceElement', 'HTMLSpanElement', 'HTMLTableElement',
        'HTMLTableSectionElement', 'HTMLTableCellElement', 'HTMLTimeElement', 'HTMLTitleElement',
        'HTMLTableRowElement', 'HTMLTrackElement', 'HTMLUListElement', 'HTMLUnknownElement',
        'HTMLDirectoryElement', 'HTMLFontElement', 'HTMLFrameElement', 'HTMLFrameSetElement', 'HTMLParamElement',
        'HTMLMarqueeElement'
    ].forEach(function (name) {
        if (typeof global[name] === 'function') return;
        var C = function () { illegal(); };
        Object.defineProperty(C, 'name', { value: name });
        C.prototype = Object.create(HE.prototype);
        define(C.prototype, 'constructor', C);
        Object.setPrototypeOf(C, HE);
        expose(name, C);
    });

    /* (HTMLAudioElement: an <audio> is the native media object, with the
     * media members of HTMLVideoElement.prototype but not video's own) */
    var VE = global.HTMLVideoElement;
    if (HE && VE && VE.prototype) {
        if (typeof global.HTMLAudioElement !== 'function') {
            var AE = function () { illegal(); };
            Object.defineProperty(AE, 'name', { value: 'HTMLAudioElement' });
            AE.prototype = Object.create(HE.prototype);
            var videoOnly = ['constructor', 'width', 'height', 'videoWidth', 'videoHeight', 'poster', 'playsInline',
                             'getVideoPlaybackQuality', 'requestPictureInPicture', 'disablePictureInPicture'];
            Object.getOwnPropertyNames(VE.prototype).forEach(function (n) {
                if (videoOnly.indexOf(n) < 0) Object.defineProperty(AE.prototype, n, Object.getOwnPropertyDescriptor(VE.prototype, n));
            });
            define(AE.prototype, 'constructor', AE);
            Object.setPrototypeOf(AE, HE);
            expose('HTMLAudioElement', AE);
        }
    }

    /* ---- traversal (DOM 6): NodeFilter, NodeIterator, TreeWalker ---- */
    /* (a legacy callback interface object: a function that throws, with the constants) */
    var NodeFilter = ({ NodeFilter() { illegal(); } }).NodeFilter;
    var nfConsts = { FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3,
        SHOW_ALL: 0xFFFFFFFF, SHOW_ELEMENT: 0x1, SHOW_ATTRIBUTE: 0x2, SHOW_TEXT: 0x4, SHOW_CDATA_SECTION: 0x8,
        SHOW_ENTITY_REFERENCE: 0x10, SHOW_ENTITY: 0x20, SHOW_PROCESSING_INSTRUCTION: 0x40, SHOW_COMMENT: 0x80,
        SHOW_DOCUMENT: 0x100, SHOW_DOCUMENT_TYPE: 0x200, SHOW_DOCUMENT_FRAGMENT: 0x400, SHOW_NOTATION: 0x800 };
    for (var nfk in nfConsts) Object.defineProperty(NodeFilter, nfk, { value: nfConsts[nfk], enumerable: true });
    if (!global.NodeFilter || global.NodeFilter.FILTER_SKIP !== 3) expose('NodeFilter', NodeFilter);
    /* (NodeIterator and TreeWalker: browser/traversal.c) */

    /* ---- Node.isEqualNode / isSameNode (DOM 4.4) ---- */
    var NP = global.Node && global.Node.prototype;
    function equalNodes(a, b) {
        if (a === b) return true;
        if (!a || !b || a.nodeType !== b.nodeType) return false;
        switch (a.nodeType) {
        case 1: {
            if (a.localName !== b.localName || a.namespaceURI !== b.namespaceURI || a.prefix !== b.prefix) return false;
            /* (attributes by namespace and local name: their prefixes don't matter) */
            var aa = a.attributes, ba = b.attributes;
            if (aa.length !== ba.length) return false;
            for (var i = 0; i < aa.length; i++) {
                var bv = b.getAttributeNS(aa[i].namespaceURI, aa[i].localName);
                if (bv === null || bv !== aa[i].value) return false;
            }
            break;
        }
        case 2:
            if (a.namespaceURI !== b.namespaceURI || a.localName !== b.localName || a.value !== b.value) return false;
            break;
        case 3: case 4: case 8:
            if (a.data !== b.data) return false;
            break;
        case 7:
            if (a.target !== b.target || a.data !== b.data) return false;
            break;
        case 10:
            if (a.name !== b.name || a.publicId !== b.publicId || a.systemId !== b.systemId) return false;
            break;
        }
        var ca = a.childNodes, cb = b.childNodes;
        if (ca.length !== cb.length) return false;
        for (var j = 0; j < ca.length; j++) if (!equalNodes(ca[j], cb[j])) return false;
        return true;
    }
    /* (WebIDL: this a Node, and the one argument given) */
    function nodeCall(t, method, argc) {
        if (t === null || typeof t !== 'object' || !NP.isPrototypeOf(t))
            throw new TypeError("Failed to execute '" + method + "' on 'Node': Illegal invocation");
        if (argc < 1) throw new TypeError("Failed to execute '" + method + "' on 'Node': 1 argument required, but only 0 present.");
    }
    if (NP && typeof NP.isEqualNode !== 'function')
        define(NP, 'isEqualNode', function isEqualNode(other) {
            nodeCall(this, 'isEqualNode', arguments.length);
            return other != null && equalNodes(this, other);
        });
    if (NP && typeof NP.isSameNode !== 'function')
        define(NP, 'isSameNode', function isSameNode(other) {
            nodeCall(this, 'isSameNode', arguments.length);
            return this === other;
        });

    /* ---- ChildNode on character data (DOM 4.2.8): text, comments ---- */
    function nodesOf(doc, args) {
        var out = [];
        for (var i = 0; i < args.length; i++) {
            var a = args[i];
            out.push(a !== null && typeof a === 'object' && typeof a.nodeType === 'number' ? a : doc.createTextNode(String(a)));
        }
        return out;
    }
    var CDP = global.CharacterData && global.CharacterData.prototype;
    [CDP, global.Text && global.Text.prototype, global.Comment && global.Comment.prototype].forEach(function (P) {
        if (!P) return;
        if (typeof P.remove !== 'function')
            define(P, 'remove', function remove() {
                if (this === null || typeof this !== 'object' || !NP.isPrototypeOf(this))
                    throw new TypeError("Failed to execute 'remove': Illegal invocation");
                var p = this.parentNode;
                if (p) p.removeChild(this);
            });
        if (typeof P.before !== 'function')
            define(P, 'before', function before() {
                var p = this.parentNode, self = this;
                if (p) nodesOf(this.ownerDocument, arguments).forEach(function (n) { p.insertBefore(n, self); });
            });
        if (typeof P.after !== 'function')
            define(P, 'after', function after() {
                var p = this.parentNode, ref = this.nextSibling;
                if (p) nodesOf(this.ownerDocument, arguments).forEach(function (n) { p.insertBefore(n, ref); });
            });
        if (typeof P.replaceWith !== 'function')
            define(P, 'replaceWith', function replaceWith() {
                var p = this.parentNode, ref = this.nextSibling;
                if (!p) return;
                var ns = nodesOf(this.ownerDocument, arguments);
                p.removeChild(this);
                ns.forEach(function (n) { p.insertBefore(n, ref); });
            });
    });

    /* (ChildNode on DocumentType too; NonDocumentTypeChildNode's element
     * siblings on CharacterData, Element's getters) */
    var DTP = global.DocumentType && global.DocumentType.prototype, ELP = global.Element && global.Element.prototype;
    if (DTP && CDP) ['before', 'after', 'replaceWith', 'remove'].forEach(function (m) {
        if (!Object.prototype.hasOwnProperty.call(DTP, m) && typeof CDP[m] === 'function')
            Object.defineProperty(DTP, m, { value: CDP[m], writable: true, configurable: true, enumerable: true });
    });
    if (CDP && ELP) ['previousElementSibling', 'nextElementSibling'].forEach(function (m) {
        var d = Object.getOwnPropertyDescriptor(ELP, m);
        if (d && d.get && !Object.prototype.hasOwnProperty.call(CDP, m))
            Object.defineProperty(CDP, m, { get: d.get, configurable: true, enumerable: true });
    });
    /* (Slottable: element.slot reflects slot=""; assignedSlot, the open
     * shadow root's slot the node is assigned to) */
    if (ELP && !Object.prototype.hasOwnProperty.call(ELP, 'slot'))
        Object.defineProperty(ELP, 'slot', { configurable: true, enumerable: true,
            get: function () { var v = this.getAttributeNS(null, 'slot'); return v === null ? '' : v; },
            set: function (v) { this.setAttribute('slot', String(v)); } });
    function assignedSlot() {
        if (this === null || typeof this !== 'object' || !NP.isPrototypeOf(this)) throw new TypeError('Illegal invocation');
        var host = this.parentNode;
        var root = host && host.nodeType === 1 ? host.shadowRoot : null;
        if (!root || root.mode !== 'open') return null;
        var want = this.nodeType === 1 ? (this.getAttributeNS(null, 'slot') || '') : '';
        var slots = root.querySelectorAll('slot');
        for (var i = 0; i < slots.length; i++) if ((slots[i].getAttribute('name') || '') === want) return slots[i];
        return null;
    }
    [ELP, global.Text && global.Text.prototype].forEach(function (P) {
        if (P && !Object.prototype.hasOwnProperty.call(P, 'assignedSlot'))
            Object.defineProperty(P, 'assignedSlot', { get: assignedSlot, configurable: true, enumerable: true });
    });

    /* ---- attribute nodes (DOM 4.9.2): getAttributeNode and the rest ---- */
    var EP = global.Element && global.Element.prototype;
    function attrNode(el, name) {
        var v = el.getAttribute(name);
        if (v === null) return null;
        var attrs = el.attributes;
        for (var i = 0; attrs && i < attrs.length; i++) if (attrs[i] && attrs[i].name === name) return attrs[i];
        return { name: name, localName: name, value: v, ownerElement: el, namespaceURI: null, prefix: null, specified: true };
    }
    if (EP) {
        if (typeof EP.getAttributeNode !== 'function')
            define(EP, 'getAttributeNode', function getAttributeNode(name) { return attrNode(this, String(name).toLowerCase()); });
        if (typeof EP.getAttributeNodeNS !== 'function')
            define(EP, 'getAttributeNodeNS', function getAttributeNodeNS(ns, name) { return attrNode(this, String(name)); });
        if (typeof EP.removeAttributeNode !== 'function')
            define(EP, 'removeAttributeNode', function removeAttributeNode(attr) {
                if (!attr || !this.hasAttribute(attr.name))
                    throw new (global.DOMException || Error)('The attribute was not found.', 'NotFoundError');
                this.removeAttribute(attr.name);
                return attr;
            });
        if (typeof EP.setAttributeNode !== 'function')
            define(EP, 'setAttributeNode', function setAttributeNode(attr) {
                var old = attrNode(this, attr.name);
                this.setAttribute(attr.name, attr.value);
                return old;
            });
    }

    /* (CSSOM View: the element whose scroll is the viewport's) */
    var DOCP = global.Document && global.Document.prototype;
    if (DOCP && !('scrollingElement' in DOCP))
        Object.defineProperty(DOCP, 'scrollingElement', { configurable: true, enumerable: true, get: function () {
            return this.compatMode === 'BackCompat' ? this.body : this.documentElement;
        } });

    /* Range.expand(unit): to the word or sentence around its ends, in their
     * text; anything longer, to its container's contents.  (Never out of the
     * ends' trees: they stay where they are.) */
    var RP = global.Range && global.Range.prototype;
    if (RP && !RP.expand)
        Object.defineProperty(RP, 'expand', { configurable: true, enumerable: true, writable: true, value: function (unit) {
            unit = unit === undefined ? 'word' : String(unit);
            var isWord = unit === 'word', isSentence = unit === 'sentence';
            if (!isWord && !isSentence) {
                if (unit === 'block' || unit === 'document' || unit === 'documentboundary') {
                    var c = this.commonAncestorContainer;
                    if (c.nodeType !== 1 && c.nodeType !== 11 && c.nodeType !== 9) c = c.parentNode;
                    if (c) this.selectNodeContents(c);
                }
                return;
            }
            var inside = isWord ? /[\p{L}\p{N}_']/u : /[^.!?]/;
            function back(n, o) {
                var t = n.data;
                while (o > 0 && inside.test(t.charAt(o - 1))) o--;
                if (isSentence) while (o < t.length && /\s/.test(t.charAt(o))) o++;
                return o;
            }
            function ahead(n, o) {
                var t = n.data;
                while (o < t.length && inside.test(t.charAt(o))) o++;
                if (isSentence && o < t.length) o++;
                return o;
            }
            var sc = this.startContainer, ec = this.endContainer, so = this.startOffset, eo = this.endOffset;
            if (sc.nodeType === 3) this.setStart(sc, back(sc, so));
            if (ec.nodeType === 3) this.setEnd(ec, ahead(ec, eo));
        } });

    /* (fullscreen isn't here, but [LegacyLenientSetter]: a setter that does nothing) */
    if (DOCP) ['fullscreenEnabled', 'fullscreen', 'fullscreenElement'].forEach(function (name) {
        var d = Object.getOwnPropertyDescriptor(DOCP, name);
        if (!d || !d.get || d.set) return;
        var set = function () {};
        Object.defineProperty(set, 'name', { value: 'set ' + name });
        Object.defineProperty(DOCP, name, { configurable: true, enumerable: true, get: d.get, set: set });
    });
    var SRP = global.ShadowRoot && global.ShadowRoot.prototype;
    if (SRP && !('fullscreenElement' in SRP))
        Object.defineProperty(SRP, 'fullscreenElement', { configurable: true, enumerable: true,
            get: Object.getOwnPropertyDescriptor({ get fullscreenElement() { return null; } }, 'fullscreenElement').get,
            set: Object.getOwnPropertyDescriptor({ set fullscreenElement(v) {} }, 'fullscreenElement').set });

    /* (HTML 3.1.5: the document's collections) */
    if (DOCP) [['scripts', 'script'], ['embeds', 'embed'], ['plugins', 'embed']].forEach(function (m) {
        if (!(m[0] in DOCP))
            Object.defineProperty(DOCP, m[0], { configurable: true, enumerable: true,
                get: function () { return this.getElementsByTagName(m[1]); } });
    });
    /* (DOM parsing: as innerHTML and DOMParser here) */
    if (global.Document && !global.Document.parseHTMLUnsafe)
        Object.defineProperty(global.Document, 'parseHTMLUnsafe', { configurable: true, enumerable: true, writable: true,
            value: function parseHTMLUnsafe(html) { return new global.DOMParser().parseFromString(String(html), 'text/html'); } });
    if (global.Element && !global.Element.prototype.setHTMLUnsafe)
        Object.defineProperty(global.Element.prototype, 'setHTMLUnsafe', { configurable: true, enumerable: true, writable: true,
            value: function setHTMLUnsafe(html) { this.innerHTML = String(html); } });
    if (global.ShadowRoot && !global.ShadowRoot.prototype.setHTMLUnsafe)
        Object.defineProperty(global.ShadowRoot.prototype, 'setHTMLUnsafe', { configurable: true, enumerable: true, writable: true,
            value: function setHTMLUnsafe(html) { this.innerHTML = String(html); } });
    /* (XMLHttpRequest's response, by its responseType) */
    var XHRP = global.XMLHttpRequest && global.XMLHttpRequest.prototype;
    if (XHRP) {
        var parsedDoc = function (x) {
            var ct = (x.getResponseHeader('content-type') || 'application/xml').split(';')[0].trim();
            if (ct !== 'text/html' && ct !== 'application/xml' && ct !== 'text/xml' && ct !== 'application/xhtml+xml' && !/\+xml$/.test(ct))
                ct = 'application/xml';
            return new global.DOMParser().parseFromString(x.responseText, ct);
        };
        Object.defineProperty(XHRP, 'response', { configurable: true, enumerable: true, get: function () {
            if (this.readyState !== 4) return this.responseType === '' || this.responseType === 'text' ? '' : null;
            switch (this.responseType) {
            case 'document': return parsedDoc(this);
            case 'json': try { return JSON.parse(this.responseText); } catch (e) { return null; }
            default: return this.responseText;
            }
        } });
        Object.defineProperty(XHRP, 'responseXML', { configurable: true, enumerable: true, get: function () {
            if (this.readyState !== 4 || (this.responseType !== '' && this.responseType !== 'document')) return null;
            return parsedDoc(this);
        } });
    }

    /* (Performance is an EventTarget) */
    var PERF = global.performance;
    if (PERF && global.EventTarget && !PERF.addEventListener) {
        var PERFP = Object.getPrototypeOf(PERF);
        if (PERFP === Object.prototype) Object.setPrototypeOf(PERF, global.EventTarget.prototype);
        else Object.setPrototypeOf(PERFP, global.EventTarget.prototype);
    }

    /* (HTML 4.11.1) */
    var DETP = global.HTMLDetailsElement && global.HTMLDetailsElement.prototype;
    if (DETP && !('open' in DETP))
        Object.defineProperty(DETP, 'open', { configurable: true, enumerable: true,
            get: function () { return this.hasAttributeNS(null, 'open'); },
            set: function (v) { if (v) this.setAttribute('open', ''); else this.removeAttribute('open'); } });

    /* ---- scrolling (CSSOM View), over scrollTop and scrollLeft ---- */
    /* (smooth scrolling is an instant one) */
    var ELP = global.Element && global.Element.prototype;
    function scrollEnum(name, owner, v, allowed) {
        if (allowed.indexOf(v = String(v)) < 0)
            throw new TypeError("Failed to execute '" + name + "' on '" + owner + "': '" + v + "' is not a valid enum value.");
        return v;
    }
    function scrollNumber(v) { v = Number(v); return isFinite(v) ? v : 0; }
    function scrollArgs(name, owner, args) {
        if (args.length >= 2) return { left: scrollNumber(args[0]), top: scrollNumber(args[1]) };
        var a = args[0], o = {};
        if (a === undefined || a === null) return o;
        if (typeof a !== 'object' && typeof a !== 'function')
            throw new TypeError("Failed to execute '" + name + "' on '" + owner + "': parameter 1 is not of type 'ScrollToOptions'.");
        if (a.behavior !== undefined) scrollEnum(name, owner, a.behavior, ['auto', 'instant', 'smooth']);
        if (a.left !== undefined) o.left = scrollNumber(a.left);
        if (a.top !== undefined) o.top = scrollNumber(a.top);
        return o;
    }
    function scrollApply(el, o, by) {
        if (o.left !== undefined) el.scrollLeft = by ? el.scrollLeft + o.left : o.left;
        if (o.top !== undefined) el.scrollTop = by ? el.scrollTop + o.top : o.top;
    }
    function scrollViewportElement() {
        var d = global.document;
        return d && d.scrollingElement;
    }
    [['scroll', false], ['scrollTo', false], ['scrollBy', true]].forEach(function (m) {
        var name = m[0], by = m[1];
        if (ELP) Object.defineProperty(ELP, name, { configurable: true, enumerable: true, writable: true, value: function () {
            scrollApply(this, scrollArgs(name, 'Element', arguments), by);
        } });
        Object.defineProperty(global, name, { configurable: true, enumerable: true, writable: true, value: function () {
            var o = scrollArgs(name, 'Window', arguments), el = scrollViewportElement();
            if (el) scrollApply(el, o, by);
        } });
    });
    [['scrollX', 'scrollLeft'], ['pageXOffset', 'scrollLeft'], ['scrollY', 'scrollTop'], ['pageYOffset', 'scrollTop']].forEach(function (m) {
        Object.defineProperty(global, m[0], { configurable: true, enumerable: true,
            get: function () { var el = scrollViewportElement(); return el ? el[m[1]] : 0; },
            set: function (v) { Object.defineProperty(global, m[0], { configurable: true, enumerable: true, writable: true, value: v }); } });
    });
    /* The offset that brings [lo, hi] into [vlo, vhi], by how it's asked. */
    function scrollAlign(how, lo, hi, vlo, vhi) {
        if (how === 'start') return lo - vlo;
        if (how === 'end') return hi - vhi;
        if (how === 'center') return (lo + hi) / 2 - (vlo + vhi) / 2;
        if (lo < vlo && hi > vhi) return 0;
        if (lo < vlo || hi > vhi) return hi - lo > vhi - vlo === (lo < vlo) ? hi - vhi : lo - vlo;
        return 0;
    }
    function scrollIntoViewOf(el, block, inline) {
        var d = el.ownerDocument, root = d.documentElement;
        for (var a = el.assignedSlot || el.parentNode || el.host; a && a !== d; a = a.assignedSlot || a.parentNode || a.host) {
            if (a.nodeType !== 1 || a === root || a === d.body) continue;
            if (a.scrollHeight <= a.clientHeight && a.scrollWidth <= a.clientWidth) continue;
            var r = el.getBoundingClientRect(), b = a.getBoundingClientRect();
            var l = b.left + a.clientLeft, t = b.top + a.clientTop;
            a.scrollLeft += scrollAlign(inline, r.left, r.right, l, l + a.clientWidth);
            a.scrollTop += scrollAlign(block, r.top, r.bottom, t, t + a.clientHeight);
        }
        var se = d.scrollingElement;
        if (!se) return;
        var v = el.getBoundingClientRect();
        se.scrollLeft += scrollAlign(inline, v.left, v.right, 0, global.innerWidth);
        se.scrollTop += scrollAlign(block, v.top, v.bottom, 0, global.innerHeight);
    }
    if (ELP) {
        Object.defineProperty(ELP, 'scrollIntoView', { configurable: true, enumerable: true, writable: true, value: function () {
            var arg = arguments[0], block = 'start', inline = 'nearest', LOGICAL = ['start', 'center', 'end', 'nearest'];
            if (typeof arg === 'object' && arg !== null || typeof arg === 'function') {
                if (arg.behavior !== undefined) scrollEnum('scrollIntoView', 'Element', arg.behavior, ['auto', 'instant', 'smooth']);
                if (arg.block !== undefined) block = scrollEnum('scrollIntoView', 'Element', arg.block, LOGICAL);
                if (arg.inline !== undefined) inline = scrollEnum('scrollIntoView', 'Element', arg.inline, LOGICAL);
            } else if (arg !== undefined && arg !== null && !arg) {
                block = 'end';
            }
            scrollIntoViewOf(this, block, inline);
        } });
        Object.defineProperty(ELP, 'scrollIntoViewIfNeeded', { configurable: true, enumerable: true, writable: true, value: function () {
            scrollIntoViewOf(this, 'nearest', 'nearest');
        } });
    }
    if (DOCP && !('elementsFromPoint' in DOCP))
        Object.defineProperty(DOCP, 'elementsFromPoint', { configurable: true, enumerable: true, writable: true, value: function (x, y) {
            var out = [];
            for (var e = this.elementFromPoint(x, y); e; e = e.parentElement) out.push(e);
            return out;
        } });

    /* ---- DocumentFragment (and ShadowRoot).getElementById (DOM 4.2.4) ---- */
    var DFP = global.DocumentFragment && global.DocumentFragment.prototype;
    if (DFP && typeof DFP.getElementById !== 'function')
        define(DFP, 'getElementById', function getElementById(id) {
            if (arguments.length < 1) throw new TypeError("Failed to execute 'getElementById' on 'DocumentFragment': 1 argument required.");
            id = String(id);
            if (!id) return null;
            var all = this.querySelectorAll('[id]');
            for (var i = 0; i < all.length; i++) if (all[i].id === id) return all[i];
            return null;
        });

    /* ---- selection (Selection API): document.getSelection() and the
     * members the window's Selection lacks ---- */
    var winGetSelection = global.getSelection;
    function completeSelection(s) {
        if (!s || typeof s !== 'object' || typeof s.setBaseAndExtent === 'function') return s;
        define(s, 'setBaseAndExtent', function setBaseAndExtent(an, ao, fn, fo) {
            var d = (an && an.ownerDocument) || global.document, r = d.createRange();
            r.setStart(an, ao >>> 0);
            r.setEnd(fn, fo >>> 0);
            if (r.collapsed && (an !== fn || ao !== fo)) { r.setStart(fn, fo >>> 0); r.setEnd(an, ao >>> 0); }
            this.removeAllRanges();
            this.addRange(r);
        });
        define(s, 'collapseToStart', function collapseToStart() {
            if (this.rangeCount) { var r = this.getRangeAt(0).cloneRange(); r.collapse(true); this.removeAllRanges(); this.addRange(r); }
        });
        define(s, 'collapseToEnd', function collapseToEnd() {
            if (this.rangeCount) { var r = this.getRangeAt(0).cloneRange(); r.collapse(false); this.removeAllRanges(); this.addRange(r); }
        });
        define(s, 'empty', function empty() { this.removeAllRanges(); });
        if (typeof s.modify !== 'function') define(s, 'modify', function modify() {});
        return s;
    }
    if (typeof winGetSelection === 'function') {
        var windowGetSelection = function getSelection() { return completeSelection(winGetSelection.call(global)); };
        define(global, 'getSelection', windowGetSelection);
        var DocP = global.Document && global.Document.prototype;
        var docGetSelection = function getSelection() {
            var w = (this && this.defaultView) || global;
            return w === global ? windowGetSelection() : w.getSelection();
        };
        if (DocP && typeof DocP.getSelection !== 'function') define(DocP, 'getSelection', docGetSelection);
        if (global.document && typeof global.document.getSelection !== 'function')
            define(global.document, 'getSelection', docGetSelection);
    }

    /* ---- tables ---- */
    function elementChildren(el, names) {
        var out = [];
        for (var c = el.firstChild; c; c = c.nextSibling)
            if (c.nodeType === 1 && names.indexOf(c.localName) >= 0) out.push(c);
        return out;
    }
    function collection(arr) {
        Object.setPrototypeOf(arr, global.HTMLCollection.prototype);
        return arr;
    }
    function make(el, tag) { return el.ownerDocument.createElement(tag); }
    function indexError() {
        return new (global.DOMException || RangeError)('The index is not in the allowed range.', 'IndexSizeError');
    }
    function accessor(P, name, get) {
        Object.defineProperty(P, name, { get: get, configurable: true, enumerable: true });
    }
    function tableRows(t) {
        var out = [], heads = elementChildren(t, ['thead']), foots = elementChildren(t, ['tfoot']);
        heads.forEach(function (s) { out.push.apply(out, elementChildren(s, ['tr'])); });
        for (var c = t.firstChild; c; c = c.nextSibling) {
            if (c.nodeType !== 1) continue;
            if (c.localName === 'tr') out.push(c);
            else if (c.localName === 'tbody') out.push.apply(out, elementChildren(c, ['tr']));
        }
        foots.forEach(function (s) { out.push.apply(out, elementChildren(s, ['tr'])); });
        return out;
    }
    var TP = global.HTMLTableElement && global.HTMLTableElement.prototype;
    if (TP) {
        accessor(TP, 'caption', function () { return elementChildren(this, ['caption'])[0] || null; });
        accessor(TP, 'tHead', function () { return elementChildren(this, ['thead'])[0] || null; });
        accessor(TP, 'tFoot', function () { return elementChildren(this, ['tfoot'])[0] || null; });
        accessor(TP, 'tBodies', function () { return collection(elementChildren(this, ['tbody'])); });
        accessor(TP, 'rows', function () { return collection(tableRows(this)); });
        define(TP, 'createCaption', function () {
            return this.caption || this.insertBefore(make(this, 'caption'), this.firstChild);
        });
        define(TP, 'deleteCaption', function () { var c = this.caption; if (c) this.removeChild(c); });
        define(TP, 'createTHead', function () {
            if (this.tHead) return this.tHead;
            var ref = null;
            for (var c = this.firstChild; c; c = c.nextSibling)
                if (c.nodeType === 1 && c.localName !== 'caption' && c.localName !== 'colgroup') { ref = c; break; }
            return this.insertBefore(make(this, 'thead'), ref);
        });
        define(TP, 'deleteTHead', function () { var h = this.tHead; if (h) this.removeChild(h); });
        define(TP, 'createTFoot', function () { return this.tFoot || this.appendChild(make(this, 'tfoot')); });
        define(TP, 'deleteTFoot', function () { var f = this.tFoot; if (f) this.removeChild(f); });
        define(TP, 'createTBody', function () {
            var bodies = elementChildren(this, ['tbody']), last = bodies[bodies.length - 1];
            return this.insertBefore(make(this, 'tbody'), last ? last.nextSibling : null);
        });
        define(TP, 'insertRow', function (index) {
            index = index === undefined ? -1 : index | 0;
            var rows = tableRows(this);
            if (index < -1 || index > rows.length) throw indexError();
            var tr = make(this, 'tr');
            if (!rows.length) {
                var bodies = elementChildren(this, ['tbody']);
                (bodies.length ? bodies[bodies.length - 1] : this.appendChild(make(this, 'tbody'))).appendChild(tr);
            } else if (index === -1 || index === rows.length) {
                rows[rows.length - 1].parentNode.appendChild(tr);
            } else {
                rows[index].parentNode.insertBefore(tr, rows[index]);
            }
            return tr;
        });
        define(TP, 'deleteRow', function (index) {
            var rows = tableRows(this);
            index = index | 0;
            if (index === -1) index = rows.length - 1;
            if (index < 0 || index >= rows.length) { if (index === -1) return; throw indexError(); }
            rows[index].parentNode.removeChild(rows[index]);
        });
    }
    var SP = global.HTMLTableSectionElement && global.HTMLTableSectionElement.prototype;
    if (SP) {
        accessor(SP, 'rows', function () { return collection(elementChildren(this, ['tr'])); });
        define(SP, 'insertRow', function (index) {
            index = index === undefined ? -1 : index | 0;
            var rows = elementChildren(this, ['tr']);
            if (index < -1 || index > rows.length) throw indexError();
            var tr = make(this, 'tr');
            return this.insertBefore(tr, index === -1 || index === rows.length ? null : rows[index]);
        });
        define(SP, 'deleteRow', function (index) {
            var rows = elementChildren(this, ['tr']);
            index = index | 0;
            if (index === -1) index = rows.length - 1;
            if (index < 0 || index >= rows.length) { if (index === -1) return; throw indexError(); }
            this.removeChild(rows[index]);
        });
    }
    var RP = global.HTMLTableRowElement && global.HTMLTableRowElement.prototype;
    if (RP) {
        accessor(RP, 'cells', function () { return collection(elementChildren(this, ['td', 'th'])); });
        accessor(RP, 'rowIndex', function () {
            for (var t = this.parentNode; t; t = t.parentNode)
                if (t.localName === 'table') return tableRows(t).indexOf(this);
            return -1;
        });
        accessor(RP, 'sectionRowIndex', function () {
            var p = this.parentNode;
            return p ? elementChildren(p, ['tr']).indexOf(this) : -1;
        });
        define(RP, 'insertCell', function (index) {
            index = index === undefined ? -1 : index | 0;
            var cells = elementChildren(this, ['td', 'th']);
            if (index < -1 || index > cells.length) throw indexError();
            var td = make(this, 'td');
            return this.insertBefore(td, index === -1 || index === cells.length ? null : cells[index]);
        });
        define(RP, 'deleteCell', function (index) {
            var cells = elementChildren(this, ['td', 'th']);
            index = index | 0;
            if (index === -1) index = cells.length - 1;
            if (index < 0 || index >= cells.length) { if (index === -1) return; throw indexError(); }
            this.removeChild(cells[index]);
        });
    }
    var CP = global.HTMLTableCellElement && global.HTMLTableCellElement.prototype;
    if (CP) {
        accessor(CP, 'cellIndex', function () {
            var p = this.parentNode;
            return p && p.localName === 'tr' ? elementChildren(p, ['td', 'th']).indexOf(this) : -1;
        });
        [['colSpan', 'colspan', 1], ['rowSpan', 'rowspan', 1]].forEach(function (a) {
            Object.defineProperty(CP, a[0], { configurable: true, enumerable: true,
                get: function () { var v = parseInt(this.getAttribute(a[1]), 10); return v > 0 ? v : a[2]; },
                set: function (v) { this.setAttribute(a[1], String(v >>> 0)); } });
        });
    }

    /* ---- User Timing ---- */
    var perf = global.performance;
    if (!perf) return;
    var PerfProto = Object.getPrototypeOf(perf);
    var nativeNow = perf.now.bind(perf);
    var nativeByType = PerfProto.getEntriesByType, nativeAll = PerfProto.getEntries;
    var marks = [], measures = [];

    function PerformanceEntry() { illegal(); }
    function entry(proto, name, type, start, duration, detail) {
        var e = Object.create(proto);
        Object.defineProperty(e, '_d', { value: { name: name, entryType: type, startTime: start,
                                                   duration: duration, detail: detail } });
        return e;
    }
    ['name', 'entryType', 'startTime', 'duration'].forEach(function (k) {
        Object.defineProperty(PerformanceEntry.prototype, k, {
            get: function () { return this._d[k]; }, configurable: true, enumerable: true });
    });
    define(PerformanceEntry.prototype, 'toJSON', function toJSON() {
        var d = this._d;
        return { name: d.name, entryType: d.entryType, startTime: d.startTime, duration: d.duration };
    });
    function PerformanceMark(name, options) {
        if (!new.target) throw new TypeError("Constructor PerformanceMark requires 'new'");
        name = String(name);
        var start = options && options.startTime !== undefined ? Number(options.startTime) : nativeNow();
        if (start < 0) throw new TypeError('startTime must not be negative');
        var detail = options && options.detail !== undefined ? structuredCloneOr(options.detail) : null;
        return entry(PerformanceMark.prototype, name, 'mark', start, 0, detail);
    }
    PerformanceMark.prototype = Object.create(PerformanceEntry.prototype);
    define(PerformanceMark.prototype, 'constructor', PerformanceMark);
    function PerformanceMeasure() { illegal(); }
    PerformanceMeasure.prototype = Object.create(PerformanceEntry.prototype);
    define(PerformanceMeasure.prototype, 'constructor', PerformanceMeasure);
    [PerformanceMark, PerformanceMeasure].forEach(function (C) {
        Object.defineProperty(C.prototype, 'detail', {
            get: function () { return this._d.detail; }, configurable: true, enumerable: true });
    });
    function structuredCloneOr(v) {
        return typeof structuredClone === 'function' ? structuredClone(v) : v;
    }

    var TIMING = ['navigationStart', 'unloadEventStart', 'unloadEventEnd', 'redirectStart', 'redirectEnd',
        'fetchStart', 'domainLookupStart', 'domainLookupEnd', 'connectStart', 'connectEnd',
        'secureConnectionStart', 'requestStart', 'responseStart', 'responseEnd', 'domLoading',
        'domInteractive', 'domContentLoadedEventStart', 'domContentLoadedEventEnd', 'domComplete',
        'loadEventStart', 'loadEventEnd'];
    /* A mark's time by name (the latest such mark), or a time given as a number. */
    function markTime(m) {
        if (typeof m === 'number') {
            if (m < 0) throw new TypeError('negative timestamp');
            return m;
        }
        m = String(m);
        for (var i = marks.length - 1; i >= 0; i--)
            if (marks[i]._d.name === m) return marks[i]._d.startTime;
        /* a PerformanceTiming attribute's name: its time since navigationStart */
        if (TIMING.indexOf(m) >= 0) {
            var t = perf.timing;
            if (m === 'navigationStart' || !t) return 0;
            var v = Number(t[m]);
            if (!v) throw new (global.DOMException || Error)("The '" + m + "' timing is not available.", 'InvalidAccessError');
            return v - Number(t.navigationStart);
        }
        throw new SyntaxError("The mark '" + m + "' does not exist.");
    }

    define(PerfProto, 'mark', function mark(name, options) {
        var e = new PerformanceMark(name, options);
        marks.push(e);
        return e;
    });
    define(PerfProto, 'measure', function measure(name, start, end) {
        name = String(name);
        var s, e, detail = null;
        /* (an options dictionary with none of its members counts as no start) */
        var nonEmpty = start !== null && typeof start === 'object' &&
            (start.start !== undefined || start.end !== undefined || start.duration !== undefined || start.detail !== undefined);
        if (start !== null && typeof start === 'object' && !nonEmpty) start = undefined;
        if (nonEmpty) {
            var o = start;
            if (end !== undefined) throw new TypeError('measure: options with an end mark');
            if (o.start !== undefined && o.end !== undefined && o.duration !== undefined)
                throw new TypeError('measure: start, end and duration');
            if (o.end !== undefined) e = markTime(o.end);
            else if (o.start !== undefined && o.duration !== undefined) e = markTime(o.start) + Number(o.duration);
            else e = nativeNow();
            if (o.start !== undefined) s = markTime(o.start);
            else if (o.duration !== undefined && o.end !== undefined) s = e - Number(o.duration);
            else s = 0;
            if (o.detail !== undefined) detail = structuredCloneOr(o.detail);
        } else {
            e = end !== undefined ? markTime(end) : nativeNow();
            s = start !== undefined ? markTime(start) : 0;
        }
        var m = entry(PerformanceMeasure.prototype, name, 'measure', s, e - s, detail);
        measures.push(m);
        return m;
    });
    function named(list, name) {
        name = String(name);
        return list.filter(function (x) { return x._d.name !== name; });
    }
    define(PerfProto, 'clearMarks', function clearMarks(name) {
        marks = name === undefined ? [] : named(marks, name);
    });
    define(PerfProto, 'clearMeasures', function clearMeasures(name) {
        measures = name === undefined ? [] : named(measures, name);
    });
    function byTime(a, b) { return a.startTime - b.startTime; }
    define(PerfProto, 'getEntriesByType', function getEntriesByType(type) {
        type = String(type);
        if (type === 'mark') return marks.slice();
        if (type === 'measure') return measures.slice();
        return nativeByType ? nativeByType.call(this, type) : [];
    });
    define(PerfProto, 'getEntriesByName', function getEntriesByName(name, type) {
        name = String(name);
        var all = type === undefined ? this.getEntries() : this.getEntriesByType(type);
        return all.filter(function (x) { return x.name === name; });
    });
    define(PerfProto, 'getEntries', function getEntries() {
        var other = nativeAll ? Array.prototype.filter.call(nativeAll.call(this), function (x) {
            return x.entryType !== 'mark' && x.entryType !== 'measure';
        }) : [];
        return other.concat(marks, measures).sort(byTime);
    });
    expose('PerformanceEntry', PerformanceEntry);
    expose('PerformanceMark', PerformanceMark);
    expose('PerformanceMeasure', PerformanceMeasure);
})

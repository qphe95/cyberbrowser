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

    /* ---- Blob, File (File API): the bytes, in a Uint8Array ---- */
    var blobBytes = new WeakMap();
    function bytesOfBlob(b) {
        var by = b !== null && typeof b === 'object' ? blobBytes.get(b) : undefined;
        if (!by) throw new TypeError('Illegal invocation');
        return by;
    }
    function Blob(parts, options) {
        if (!new.target) throw new TypeError("Failed to construct 'Blob': Please use the 'new' operator");
        var chunks = [], size = 0, text = [];
        /* (strings next to each other encode as one; each is well formed on
         * its own first: half a surrogate pair in a part is a replacement) */
        var flush = function () {
            if (!text.length) return;
            var c = new global.TextEncoder().encode(text.join(''));
            text = [];
            chunks.push(c);
            size += c.length;
        };
        if (parts !== undefined) {
            if (parts === null || typeof parts !== 'object' && typeof parts !== 'function' || typeof parts[Symbol.iterator] !== 'function')
                throw new TypeError("Failed to construct 'Blob': The provided value cannot be converted to a sequence.");
            for (var p of parts) {
                var c;
                if (p !== null && typeof p === 'object' && blobBytes.has(p)) c = blobBytes.get(p);
                else if (p instanceof ArrayBuffer) c = new Uint8Array(p);
                else if (ArrayBuffer.isView(p)) c = new Uint8Array(p.buffer, p.byteOffset, p.byteLength);
                else { text.push(String(p).toWellFormed()); continue; }
                flush();
                chunks.push(c);
                size += c.length;
            }
            flush();
        }
        var bytes = new Uint8Array(size), at = 0;
        chunks.forEach(function (c) { bytes.set(c, at); at += c.length; });
        blobBytes.set(this, bytes);
        var type = options !== undefined && options !== null && options.type !== undefined ? String(options.type) : '';
        Object.defineProperty(this, '__type', { value: /[^\x20-\x7e]/.test(type) ? '' : type.toLowerCase() });
    }
    Object.defineProperty(Blob.prototype, 'size', { configurable: true, enumerable: true, get: function () { return bytesOfBlob(this).length; } });
    Object.defineProperty(Blob.prototype, 'type', { configurable: true, enumerable: true, get: function () { bytesOfBlob(this); return this.__type; } });
    [['slice', function slice(start, end, contentType) {
        var by = bytesOfBlob(this), n = by.length;
        var rel = function (v, dflt) {
            if (v === undefined) return dflt;
            v = Math.trunc(Number(v)) || 0;
            return v < 0 ? Math.max(n + v, 0) : Math.min(v, n);
        };
        var from = rel(start, 0), to = rel(end, n);
        return new Blob([by.subarray(from, Math.max(from, to))], { type: contentType === undefined ? '' : contentType });
    }], ['text', function text() {
        try { return Promise.resolve(new global.TextDecoder().decode(bytesOfBlob(this))); } catch (e) { return Promise.reject(e); }
    }], ['arrayBuffer', function arrayBuffer() {
        try { return Promise.resolve(bytesOfBlob(this).slice().buffer); } catch (e) { return Promise.reject(e); }
    }], ['bytes', function bytes() {
        try { return Promise.resolve(bytesOfBlob(this).slice()); } catch (e) { return Promise.reject(e); }
    }], ['stream', function stream() {
        var by = bytesOfBlob(this);
        return new global.ReadableStream({ start: function (c) { if (by.length) c.enqueue(by.slice()); c.close(); } });
    }]].forEach(function (m) { Object.defineProperty(Blob.prototype, m[0], { configurable: true, enumerable: true, writable: true, value: m[1] }); });
    Object.defineProperty(Blob.prototype, Symbol.toStringTag, { value: 'Blob', configurable: true });
    function File(bits, name, options) {
        if (!new.target) throw new TypeError("Failed to construct 'File': Please use the 'new' operator");
        if (arguments.length < 2) throw new TypeError("Failed to construct 'File': 2 arguments required.");
        var self = Reflect.construct(Blob, [bits, options], new.target);
        Object.defineProperty(self, '__name', { value: String(name) });
        Object.defineProperty(self, '__lastModified', { value: options && options.lastModified !== undefined ? Number(options.lastModified) : Date.now() });
        return self;
    }
    File.prototype = Object.create(Blob.prototype, { constructor: { value: File, writable: true, configurable: true } });
    Object.setPrototypeOf(File, Blob);
    Object.defineProperty(File.prototype, 'name', { configurable: true, enumerable: true, get: function () { return this.__name; } });
    Object.defineProperty(File.prototype, 'lastModified', { configurable: true, enumerable: true, get: function () { return this.__lastModified; } });
    Object.defineProperty(File.prototype, Symbol.toStringTag, { value: 'File', configurable: true });
    expose('Blob', Blob);

    /* ---- structured clone (HTML 2.7): a value as JSON text can carry, tagged,
     * and back in this realm; what crosses to a worker goes this way ---- */
    function cloneError(v) {
        return new global.DOMException(String(v) + ' could not be cloned.', 'DataCloneError');
    }
    var ERROR_NAMES = ['Error', 'EvalError', 'RangeError', 'ReferenceError', 'SyntaxError', 'TypeError', 'URIError'];
    var VIEWS = ['Int8Array', 'Uint8Array', 'Uint8ClampedArray', 'Int16Array', 'Uint16Array', 'Int32Array', 'Uint32Array',
                 'Float32Array', 'Float64Array', 'BigInt64Array', 'BigUint64Array', 'DataView'];
    function wire(v, seen) {
        switch (typeof v) {
        case 'undefined': return { u: 1 };
        case 'boolean': case 'string': return v;
        case 'number':
            if (v !== v) return { n: 'NaN' };
            if (v === Infinity) return { n: 'Infinity' };
            if (v === -Infinity) return { n: '-Infinity' };
            if (v === 0 && 1 / v < 0) return { n: '-0' };
            return v;
        case 'bigint': return { b: String(v) };
        case 'symbol': case 'function': throw cloneError(typeof v === 'symbol' ? v.toString() : v);
        }
        if (v === null) return null;
        if (seen.has(v)) return { r: seen.get(v) };
        var id = seen.size, kind = Object.prototype.toString.call(v).slice(8, -1);
        seen.set(v, id);
        if (global.DOMException && v instanceof global.DOMException) kind = 'DOMException';
        if (Array.isArray(v)) {
            var items = [], own = Object.keys(v);
            own.forEach(function (k) { items.push([/^\d+$/.test(k) ? +k : k, wire(v[k], seen)]); });
            return { i: id, a: v.length, p: items };
        }
        switch (kind) {
        case 'Date': return { i: id, d: v.getTime() };
        case 'RegExp': return { i: id, re: v.source, f: v.flags };
        case 'Boolean': case 'Number': case 'String': case 'BigInt':
            return { i: id, w: kind, v: wire(v.valueOf(), new Map()) };
        case 'ArrayBuffer': return { i: id, ab: Array.from(new Uint8Array(v)) };
        case 'Map': {
            var m = [];
            v.forEach(function (x, k) { m.push([wire(k, seen), wire(x, seen)]); });
            return { i: id, m: m };
        }
        case 'Set': {
            var e = [];
            v.forEach(function (x) { e.push(wire(x, seen)); });
            return { i: id, s: e };
        }
        case 'DOMException': return { i: id, x: v.name, msg: v.message };
        case 'Error': {
            var name = ERROR_NAMES.indexOf(v.name) >= 0 ? v.name : 'Error';
            return { i: id, e: name, msg: v.message === undefined ? '' : String(v.message), st: v.stack === undefined ? '' : String(v.stack) };
        }
        case 'File': case 'Blob': {
            var w = { i: id, bl: Array.from(bytesOfBlob(v)), t: v.type };
            if (kind === 'File') { w.nm = v.name; w.lm = v.lastModified; }
            return w;
        }
        }
        if (VIEWS.indexOf(kind) >= 0)
            return { i: id, vw: kind, buf: wire(v.buffer, seen), o: v.byteOffset, l: kind === 'DataView' ? v.byteLength : v.length };
        /* (what has no serialization: a platform object, a promise, a weak collection) */
        if (kind !== 'Object' && kind !== 'Arguments') throw cloneError(kind);
        if (global.Node && v instanceof global.Node) throw cloneError(v);
        var props = [];
        Object.keys(v).forEach(function (k) { props.push([k, wire(v[k], seen)]); });
        return { i: id, o: props };
    }
    function unwire(w, made) {
        if (w === null || typeof w !== 'object') return w;
        if (w.u) return undefined;
        if (w.n !== undefined) return w.n === 'NaN' ? NaN : w.n === '-0' ? -0 : w.n === 'Infinity' ? Infinity : -Infinity;
        if (w.b !== undefined) return BigInt(w.b);
        if (w.r !== undefined) return made[w.r];
        var v;
        if (w.a !== undefined) {
            v = new Array(w.a);
            made[w.i] = v;
            w.p.forEach(function (e) { v[e[0]] = unwire(e[1], made); });
        } else if (w.o !== undefined && w.vw === undefined) {
            v = {};
            made[w.i] = v;
            w.o.forEach(function (e) { Object.defineProperty(v, e[0], { value: unwire(e[1], made), writable: true, enumerable: true, configurable: true }); });
        } else if (w.d !== undefined) {
            v = made[w.i] = new Date(w.d);
        } else if (w.re !== undefined) {
            v = made[w.i] = new RegExp(w.re, w.f);
        } else if (w.w !== undefined) {
            v = made[w.i] = Object(unwire(w.v, []));
        } else if (w.ab !== undefined) {
            v = made[w.i] = new Uint8Array(w.ab).buffer;
        } else if (w.m !== undefined) {
            v = made[w.i] = new Map();
            w.m.forEach(function (e) { v.set(unwire(e[0], made), unwire(e[1], made)); });
        } else if (w.s !== undefined) {
            v = made[w.i] = new Set();
            w.s.forEach(function (e) { v.add(unwire(e, made)); });
        } else if (w.x !== undefined) {
            v = made[w.i] = new global.DOMException(w.msg, w.x);
        } else if (w.e !== undefined) {
            v = made[w.i] = new global[w.e](w.msg);
            if (w.st) Object.defineProperty(v, 'stack', { value: w.st, writable: true, configurable: true });
        } else if (w.bl !== undefined) {
            var bytes = new Uint8Array(w.bl);
            v = made[w.i] = w.nm !== undefined ? new global.File([bytes], w.nm, { type: w.t, lastModified: w.lm }) : new Blob([bytes], { type: w.t });
        } else if (w.vw !== undefined) {
            var buf = unwire(w.buf, made);
            v = made[w.i] = new global[w.vw](buf, w.o, w.l);
        }
        return v;
    }
    Object.defineProperty(global, '__cyberWire', { configurable: true, value: function (v) { return wire(v, new Map()); } });
    Object.defineProperty(global, '__cyberUnwire', { configurable: true, value: function (w) { return unwire(w, []); } });
    Object.defineProperty(global, 'structuredClone', { configurable: true, enumerable: true, writable: true, value: function structuredClone(value) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'structuredClone': 1 argument required, but only 0 present.");
        return unwire(wire(value, new Map()), []);
    } });
    /* (a blob: URL's script, for a Worker) */
    Object.defineProperty(global, '__cyberBlobText', { configurable: true, value: function (url) {
        var reg = global.__blobRegistry, b = reg && reg[url];
        return b && blobBytes.has(b) ? new global.TextDecoder().decode(blobBytes.get(b)) : null;
    } });
    expose('File', File);

    /* FileReader: a read ends in a task of its own, with the events File API 8 says. */
    if (global.EventTarget) {
        var readers = new WeakMap();
        var readerOf = function (r) {
            var s = r !== null && typeof r === 'object' ? readers.get(r) : undefined;
            if (!s) throw new TypeError('Illegal invocation');
            return s;
        };
        var FileReader = function FileReader() {
            if (!new.target) throw new TypeError("Failed to construct 'FileReader': Please use the 'new' operator");
            var self = Reflect.construct(global.EventTarget, [], new.target);
            readers.set(self, { state: 0, result: null, error: null, run: 0, on: {} });
            return self;
        };
        FileReader.prototype = Object.create(global.EventTarget.prototype, { constructor: { value: FileReader, writable: true, configurable: true } });
        [['EMPTY', 0], ['LOADING', 1], ['DONE', 2]].forEach(function (k) {
            [FileReader, FileReader.prototype].forEach(function (o) { Object.defineProperty(o, k[0], { value: k[1], enumerable: true }); });
        });
        var fireReader = function (r, type) {
            var PE = global.ProgressEvent || global.Event;
            r.dispatchEvent(new PE(type));
        };
        var startRead = function (r, blob, convert) {
            var s = readerOf(r);
            if (blob === null || typeof blob !== 'object' || !blobBytes.has(blob))
                throw new TypeError("Failed to execute 'read' on 'FileReader': parameter 1 is not of type 'Blob'.");
            if (s.state === 1) throw new global.DOMException('The object is already busy reading.', 'InvalidStateError');
            s.state = 1; s.result = null; s.error = null;
            var run = ++s.run, by = blobBytes.get(blob);
            global.setTimeout(function () {
                if (s.run !== run) return;
                fireReader(r, 'loadstart');
                if (s.run !== run) return;
                global.setTimeout(function () {
                    if (s.run !== run) return;
                    s.state = 2;
                    s.result = convert(by, blob);
                    fireReader(r, 'progress');
                    fireReader(r, 'load');
                    if (s.state === 2) fireReader(r, 'loadend');
                }, 0);
            }, 0);
        };
        var binaryOf = function (by) {
            var parts = [];
            for (var i = 0; i < by.length; i += 8192) parts.push(String.fromCharCode.apply(null, by.subarray(i, i + 8192)));
            return parts.join('');
        };
        var readerMethods = {
            readAsArrayBuffer: function readAsArrayBuffer(blob) { startRead(this, blob, function (by) { return by.slice().buffer; }); },
            readAsBinaryString: function readAsBinaryString(blob) { startRead(this, blob, binaryOf); },
            readAsDataURL: function readAsDataURL(blob) {
                startRead(this, blob, function (by, b) { return 'data:' + (b.type || 'application/octet-stream') + ';base64,' + global.btoa(binaryOf(by)); });
            },
            readAsText: function readAsText(blob, encoding) {
                startRead(this, blob, function (by) {
                    try { return new global.TextDecoder(encoding === undefined ? 'utf-8' : String(encoding)).decode(by); }
                    catch (e) { return new global.TextDecoder().decode(by); }
                });
            },
            abort: function abort() {
                var s = readerOf(this);
                if (s.state !== 1) return;
                s.run++; s.state = 2; s.result = null;
                s.error = new global.DOMException('The read was aborted.', 'AbortError');
                fireReader(this, 'abort');
                if (s.state === 2) fireReader(this, 'loadend');
            }
        };
        Object.keys(readerMethods).forEach(function (k) {
            Object.defineProperty(FileReader.prototype, k, { configurable: true, enumerable: true, writable: true, value: readerMethods[k] });
        });
        ['readyState', 'result', 'error'].forEach(function (k, i) {
            var field = ['state', 'result', 'error'][i];
            Object.defineProperty(FileReader.prototype, k, { configurable: true, enumerable: true, get: function () { return readerOf(this)[field]; } });
        });
        ['loadstart', 'progress', 'load', 'abort', 'error', 'loadend'].forEach(function (type) {
            Object.defineProperty(FileReader.prototype, 'on' + type, { configurable: true, enumerable: true,
                get: function () { return readerOf(this).on[type] || null; },
                set: function (v) { readerOf(this).on[type] = typeof v === 'function' || (v !== null && typeof v === 'object') ? v : null; } });
        });
        Object.defineProperty(FileReader.prototype, Symbol.toStringTag, { value: 'FileReader', configurable: true });
        expose('FileReader', FileReader);
    }

    /* A <?xml-stylesheet?> in a document's prolog: its sheet (LinkStyle, its
     * pseudo-attributes parsed; its rules aren't fetched or applied here). */
    if (global.ProcessingInstruction) {
        var piSheets = new WeakMap();
        var piRefs = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'" };
        var piDecode = function (v) {
            return v.replace(/&(?:#x([0-9a-fA-F]+)|#([0-9]+)|(amp|lt|gt|quot|apos));/g, function (m, hex, dec, name) {
                return name ? piRefs[name] : String.fromCodePoint(hex ? parseInt(hex, 16) : parseInt(dec, 10));
            });
        };
        Object.defineProperty(global.ProcessingInstruction.prototype, 'sheet', { configurable: true, enumerable: true, get: function () {
            if (this.target !== 'xml-stylesheet' || this.parentNode !== this.ownerDocument) return null;
            if (piSheets.has(this)) return piSheets.get(this);
            var attrs = {}, m, re = /([^\s=]+)\s*=\s*(?:"([^"]*)"|'([^']*)')/g;
            while ((m = re.exec(this.data))) attrs[m[1]] = piDecode(m[2] !== undefined ? m[2] : m[3]);
            if (attrs.href === undefined || (attrs.type !== undefined && attrs.type !== 'text/css') || !global.CSSStyleSheet) return null;
            var sheet = new global.CSSStyleSheet();
            Object.defineProperty(sheet, 'href', { value: attrs.href, configurable: true });
            Object.defineProperty(sheet, 'ownerNode', { value: this, configurable: true });
            piSheets.set(this, sheet);
            return sheet;
        } });
    }

    /* Interfaces with nothing behind them yet but what scripts can see of them. */
    function plainInterfaces(list) {
        list.forEach(function (d) {
            if (global[d[0]]) return;
            var C = { f: function () { throw new TypeError('Illegal constructor'); } }.f;
            Object.defineProperty(C, 'name', { value: d[0], configurable: true });
            Object.defineProperty(C, 'length', { value: 0, configurable: true });
            var parent = d[1] && global[d[1]];
            if (parent) Object.setPrototypeOf(C, parent);
            var proto = Object.create(parent ? parent.prototype : Object.prototype);
            Object.defineProperty(C, 'prototype', { value: proto, writable: false, enumerable: false, configurable: false });
            Object.defineProperty(proto, 'constructor', { value: C, writable: true, enumerable: false, configurable: true });
            Object.defineProperty(proto, Symbol.toStringTag, { value: d[0], configurable: true });
            d[2].forEach(function (name) {
                var get = { get: function () { if (!(this instanceof C)) throw new TypeError('Illegal invocation'); return null; } };
                var g = get.get;
                Object.defineProperty(g, 'name', { value: 'get ' + name, configurable: true });
                Object.defineProperty(proto, name, { get: g, set: undefined, enumerable: true, configurable: true });
            });
            d[3].forEach(function (k) {
                Object.defineProperty(C, k[0], { value: k[1], enumerable: true });
                Object.defineProperty(proto, k[0], { value: k[1], enumerable: true });
            });
            d[4].forEach(function (op) {
                var fn = { m: function () {
                    if (!(this instanceof C)) throw new TypeError('Illegal invocation');
                    if (arguments.length < op[1]) throw new TypeError(op[0] + ': ' + op[1] + ' arguments required');
                } }.m;
                Object.defineProperty(fn, 'name', { value: op[0], configurable: true });
                Object.defineProperty(fn, 'length', { value: op[1], configurable: true });
                Object.defineProperty(proto, op[0], { value: fn, writable: true, enumerable: true, configurable: true });
            });
            Object.defineProperty(global, d[0], { value: C, writable: true, enumerable: false, configurable: true });
        });
    }
    /* (for idl_shape.c: an interface nothing behind it yet, its members all there) */
    Object.defineProperty(global, '__cyberStubInterface', { configurable: true, value: function (name, parent, ro, rw, ops) {
        var had = !!global[name];
        if (!had) plainInterfaces([[name, parent, [], [], ops]]);
        var C = global[name];
        if (typeof C !== 'function') return;
        var proto = C.prototype;
        var dash = function (a) { return a.replace(/[A-Z]/g, function (m) { return '-' + m.toLowerCase(); }); };
        /* (a style declaration's: what it holds; the rest, nothing yet) */
        var isDecl = global.CSSStyleDeclaration && proto instanceof global.CSSStyleDeclaration, held = new WeakMap();
        ro.concat(rw).forEach(function (a, i) {
            if (Object.getOwnPropertyDescriptor(proto, a)) return;
            var get = { get: function () {
                if (!(this instanceof C)) throw new TypeError('Illegal invocation');
                if (isDecl) return this.getPropertyValue(a.indexOf('-') >= 0 ? a : dash(a));
                var kept = held.get(this);
                return kept ? kept[a] : undefined;
            } };
            var g = Object.getOwnPropertyDescriptor(get, 'get').value, s;
            if (i >= ro.length) {
                var set = { set: function (v) {
                    if (!(this instanceof C)) throw new TypeError('Illegal invocation');
                    if (isDecl) { this.setProperty(a.indexOf('-') >= 0 ? a : dash(a), v); return; }
                    /* (what's put in, kept for reading back) */
                    var kept = held.get(this);
                    if (!kept) held.set(this, kept = {});
                    kept[a] = v;
                } };
                s = Object.getOwnPropertyDescriptor(set, 'set').value;
            }
            Object.defineProperty(proto, a, { get: g, set: s, enumerable: true, configurable: true });
        });
    } });
    /* (for idl_shape.c: a script-made accessor checks its this, as a native one does) */
    var fnText = Function.prototype.toString;
    Object.defineProperty(global, '__cyberBrandAccessors', { configurable: true, value: function (C) {
        var proto = C.prototype;
        Object.getOwnPropertyNames(proto).forEach(function (k) {
            var d = Object.getOwnPropertyDescriptor(proto, k);
            if (!d.get || fnText.call(d.get).indexOf('[native code]') >= 0) return;
            var g = d.get, s = d.set;
            var get = function () { if (!(this instanceof C)) throw new TypeError('Illegal invocation'); return g.call(this); };
            var set = s && function (v) { if (!(this instanceof C)) throw new TypeError('Illegal invocation'); return s.call(this, v); };
            Object.defineProperty(proto, k, { get: get, set: set, enumerable: d.enumerable, configurable: true });
        });
    } });
    /* (FontFace: its descriptors, checked as @font-face checks them) */
    if (global.FontFace && global.FontFace.prototype) {
        var ffWeight = function (w) { return w === 'normal' || w === 'bold' || (/^[+-]?[\d.]+(e[-+]?\d+)?$/i.test(w) && +w >= 1 && +w <= 1000); };
        var ffStretch = function (w) {
            if (/^(normal|ultra-condensed|extra-condensed|condensed|semi-condensed|semi-expanded|expanded|extra-expanded|ultra-expanded)$/.test(w)) return true;
            return /^[+-]?[\d.]+%$/.test(w) && parseFloat(w) >= 0;
        };
        var ffAngle = function (w) { return /^[+-]?[\d.]+deg$/.test(w) && Math.abs(parseFloat(w)) <= 90; };
        var ffWords = function (v) { return String(v).trim().split(/\s+(?![^(]*\))/); };
        var ffCalc = function (v) {
            /* (a sum of percentages: worked out, in calc()) */
            var m = /^calc\(\s*([-+]?[\d.]+)%\s*([-+])\s*([\d.]+)%\s*\)$/.exec(v);
            return m ? 'calc(' + (parseFloat(m[1]) + (m[2] === '-' ? -1 : 1) * parseFloat(m[3])) + '%)' : v;
        };
        var ffDescs = {
            weight: function (v) { var w = ffWords(v); return w.length <= 2 && w.every(ffWeight) && (w.length < 2 || !/[a-z]/.test(v)) ? v.trim() : null; },
            stretch: function (v) {
                var t = ffCalc(String(v).trim());
                if (/^calc\(/.test(t)) return /^calc\([-+]?[\d.]+%\)$/.test(t) ? t : null;
                var w = ffWords(t);
                return w.length <= 2 && w.every(ffStretch) && (w.length < 2 || /^[\d.]+% [\d.]+%$/.test(t)) ? t : null;
            },
            style: function (v) {
                var t = String(v).trim(), w = ffWords(t);
                if (t === 'normal' || t === 'italic' || t === 'oblique') return t;
                return w[0] === 'oblique' && w.length <= 3 && w.length > 1 && w.slice(1).every(ffAngle) ? t : null;
            },
        };
        Object.keys(ffDescs).forEach(function (k) {
            var check = ffDescs[k], held = new WeakMap();
            Object.defineProperty(global.FontFace.prototype, k, { enumerable: true, configurable: true,
                get: function () { if (!(this instanceof global.FontFace)) throw new TypeError('Illegal invocation'); var v = held.get(this); return v === undefined ? 'normal' : v; },
                set: function (v) {
                    if (!(this instanceof global.FontFace)) throw new TypeError('Illegal invocation');
                    var ok = check(String(v));
                    if (ok === null) throw new global.DOMException("'" + v + "' is not a valid value for " + k, 'SyntaxError');
                    held.set(this, ok);
                } });
        });
    }
    /* (a screen, what the media features' device says) */
    function liveInterface(name, parent, ro) {
        var C = { f: function () { throw new TypeError('Illegal constructor'); } }.f;
        Object.defineProperty(C, 'name', { value: name, configurable: true });
        Object.defineProperty(C, 'length', { value: 0, configurable: true });
        var pc = parent && global[parent];
        if (pc) Object.setPrototypeOf(C, pc);
        var proto = Object.create(pc ? pc.prototype : Object.prototype);
        Object.defineProperty(C, 'prototype', { value: proto, writable: false, enumerable: false, configurable: false });
        Object.defineProperty(proto, 'constructor', { value: C, writable: true, enumerable: false, configurable: true });
        Object.defineProperty(proto, Symbol.toStringTag, { value: name, configurable: true });
        Object.keys(ro).forEach(function (k) {
            var read = ro[k];
            Object.defineProperty(proto, k, { get: function () { if (!(this instanceof C)) throw new TypeError('Illegal invocation'); return read.call(this); }, enumerable: true, configurable: true });
        });
        Object.defineProperty(global, name, { value: C, writable: true, enumerable: false, configurable: true });
        return C;
    }
    if (global.document && !global.Screen) {
        var ScreenC = liveInterface('Screen', null, {
            availWidth: function () { return 1920; }, availHeight: function () { return 1040; }, width: function () { return 1920; },
            height: function () { return 1080; }, colorDepth: function () { return 24; }, pixelDepth: function () { return 24; }
        });
        var theScreen = Object.create(ScreenC.prototype);
        Object.defineProperty(global, 'screen', { get: function screen() { return theScreen; }, set: function (v) { Object.defineProperty(global, 'screen', { value: v, writable: true, configurable: true, enumerable: true }); }, enumerable: true, configurable: true });
        var VV = liveInterface('VisualViewport', 'EventTarget', {
            offsetLeft: function () { return 0; }, offsetTop: function () { return 0; },
            pageLeft: function () { return global.scrollX || 0; }, pageTop: function () { return global.scrollY || 0; },
            width: function () { return global.innerWidth; }, height: function () { return global.innerHeight; }, scale: function () { return 1; }
        });
        ['onresize', 'onscroll', 'onscrollend'].forEach(function (k) {
            var held = new WeakMap();
            Object.defineProperty(VV.prototype, k, { enumerable: true, configurable: true,
                get: function () { if (!(this instanceof VV)) throw new TypeError('Illegal invocation'); return held.get(this) || null; },
                set: function (f) {
                    if (!(this instanceof VV)) throw new TypeError('Illegal invocation');
                    var was = held.get(this);
                    if (was) this.removeEventListener(k.slice(2), was);
                    if (typeof f === 'function') { held.set(this, f); this.addEventListener(k.slice(2), f); } else held.delete(this);
                } });
        });
        var theViewport = null;
        Object.defineProperty(global, 'visualViewport', { enumerable: true, configurable: true, get: function visualViewport() {
            if (!theViewport) {
                theViewport = new EventTarget();
                Object.setPrototypeOf(theViewport, VV.prototype);
            }
            return theViewport;
        }, set: function (v) { Object.defineProperty(global, 'visualViewport', { value: v, writable: true, configurable: true, enumerable: true }); } });
        var CaretP = liveInterface('CaretPosition', null, {
            offsetNode: function () { return this.__node; }, offset: function () { return this.__offset; }
        });
        Object.defineProperty(CaretP.prototype, 'getClientRect', { writable: true, enumerable: true, configurable: true, value: function getClientRect() {
            if (!(this instanceof CaretP)) throw new TypeError('Illegal invocation');
            var r = global.document.createRange();
            r.setStart(this.__node, this.__offset);
            r.collapse(true);
            return r.getBoundingClientRect();
        } });
        var DocP = global.Document && global.Document.prototype;
        if (DocP && !DocP.caretPositionFromPoint) Object.defineProperty(DocP, 'caretPositionFromPoint', { writable: true, enumerable: true, configurable: true,
            value: function caretPositionFromPoint(x, y) {
                if (arguments.length < 2) throw new TypeError('caretPositionFromPoint: 2 arguments required');
                if (x < 0 || y < 0 || x > global.innerWidth || y > global.innerHeight) return null;
                var el = this.elementFromPoint(x, y);
                if (!el) return null;
                var p = Object.create(CaretP.prototype), n = el.firstChild && el.firstChild.nodeType === 3 ? el.firstChild : el;
                Object.defineProperty(p, '__node', { value: n });
                Object.defineProperty(p, '__offset', { value: 0 });
                return p;
            } });
        var ElP = global.Element && global.Element.prototype;
        if (ElP && !('currentCSSZoom' in ElP)) Object.defineProperty(ElP, 'currentCSSZoom', { enumerable: true, configurable: true,
            get: function () {
                if (!(this instanceof Element)) throw new TypeError('Illegal invocation');
                var z = 1;
                for (var e = this; e; e = e.parentElement) {
                    var v = parseFloat(global.getComputedStyle(e).zoom);
                    if (v > 0) z *= v;
                }
                return z;
            } });
    }

    /* (filter-effects: its elements; a window's only) */
    if (global.document) plainInterfaces([["SVGFilterElement","SVGElement",["filterUnits","primitiveUnits","x","y","width","height"],[],[]],["SVGFEBlendElement","SVGElement",["in1","in2","mode","x","y","width","height","result"],[["SVG_FEBLEND_MODE_UNKNOWN",0],["SVG_FEBLEND_MODE_NORMAL",1],["SVG_FEBLEND_MODE_MULTIPLY",2],["SVG_FEBLEND_MODE_SCREEN",3],["SVG_FEBLEND_MODE_DARKEN",4],["SVG_FEBLEND_MODE_LIGHTEN",5],["SVG_FEBLEND_MODE_OVERLAY",6],["SVG_FEBLEND_MODE_COLOR_DODGE",7],["SVG_FEBLEND_MODE_COLOR_BURN",8],["SVG_FEBLEND_MODE_HARD_LIGHT",9],["SVG_FEBLEND_MODE_SOFT_LIGHT",10],["SVG_FEBLEND_MODE_DIFFERENCE",11],["SVG_FEBLEND_MODE_EXCLUSION",12],["SVG_FEBLEND_MODE_HUE",13],["SVG_FEBLEND_MODE_SATURATION",14],["SVG_FEBLEND_MODE_COLOR",15],["SVG_FEBLEND_MODE_LUMINOSITY",16]],[]],["SVGFEColorMatrixElement","SVGElement",["in1","type","values","x","y","width","height","result"],[["SVG_FECOLORMATRIX_TYPE_UNKNOWN",0],["SVG_FECOLORMATRIX_TYPE_MATRIX",1],["SVG_FECOLORMATRIX_TYPE_SATURATE",2],["SVG_FECOLORMATRIX_TYPE_HUEROTATE",3],["SVG_FECOLORMATRIX_TYPE_LUMINANCETOALPHA",4]],[]],["SVGFEComponentTransferElement","SVGElement",["in1","x","y","width","height","result"],[],[]],["SVGComponentTransferFunctionElement","SVGElement",["type","tableValues","slope","intercept","amplitude","exponent","offset"],[["SVG_FECOMPONENTTRANSFER_TYPE_UNKNOWN",0],["SVG_FECOMPONENTTRANSFER_TYPE_IDENTITY",1],["SVG_FECOMPONENTTRANSFER_TYPE_TABLE",2],["SVG_FECOMPONENTTRANSFER_TYPE_DISCRETE",3],["SVG_FECOMPONENTTRANSFER_TYPE_LINEAR",4],["SVG_FECOMPONENTTRANSFER_TYPE_GAMMA",5]],[]],["SVGFEFuncRElement","SVGComponentTransferFunctionElement",[],[],[]],["SVGFEFuncGElement","SVGComponentTransferFunctionElement",[],[],[]],["SVGFEFuncBElement","SVGComponentTransferFunctionElement",[],[],[]],["SVGFEFuncAElement","SVGComponentTransferFunctionElement",[],[],[]],["SVGFECompositeElement","SVGElement",["in1","in2","operator","k1","k2","k3","k4","x","y","width","height","result"],[["SVG_FECOMPOSITE_OPERATOR_UNKNOWN",0],["SVG_FECOMPOSITE_OPERATOR_OVER",1],["SVG_FECOMPOSITE_OPERATOR_IN",2],["SVG_FECOMPOSITE_OPERATOR_OUT",3],["SVG_FECOMPOSITE_OPERATOR_ATOP",4],["SVG_FECOMPOSITE_OPERATOR_XOR",5],["SVG_FECOMPOSITE_OPERATOR_ARITHMETIC",6]],[]],["SVGFEConvolveMatrixElement","SVGElement",["in1","orderX","orderY","kernelMatrix","divisor","bias","targetX","targetY","edgeMode","kernelUnitLengthX","kernelUnitLengthY","preserveAlpha","x","y","width","height","result"],[["SVG_EDGEMODE_UNKNOWN",0],["SVG_EDGEMODE_DUPLICATE",1],["SVG_EDGEMODE_WRAP",2],["SVG_EDGEMODE_NONE",3]],[]],["SVGFEDiffuseLightingElement","SVGElement",["in1","surfaceScale","diffuseConstant","kernelUnitLengthX","kernelUnitLengthY","x","y","width","height","result"],[],[]],["SVGFEDistantLightElement","SVGElement",["azimuth","elevation"],[],[]],["SVGFEPointLightElement","SVGElement",["x","y","z"],[],[]],["SVGFESpotLightElement","SVGElement",["x","y","z","pointsAtX","pointsAtY","pointsAtZ","specularExponent","limitingConeAngle"],[],[]],["SVGFEDisplacementMapElement","SVGElement",["in1","in2","scale","xChannelSelector","yChannelSelector","x","y","width","height","result"],[["SVG_CHANNEL_UNKNOWN",0],["SVG_CHANNEL_R",1],["SVG_CHANNEL_G",2],["SVG_CHANNEL_B",3],["SVG_CHANNEL_A",4]],[]],["SVGFEDropShadowElement","SVGElement",["in1","dx","dy","stdDeviationX","stdDeviationY","x","y","width","height","result"],[],[["setStdDeviation",2]]],["SVGFEFloodElement","SVGElement",["x","y","width","height","result"],[],[]],["SVGFEGaussianBlurElement","SVGElement",["in1","stdDeviationX","stdDeviationY","edgeMode","x","y","width","height","result"],[["SVG_EDGEMODE_UNKNOWN",0],["SVG_EDGEMODE_DUPLICATE",1],["SVG_EDGEMODE_WRAP",2],["SVG_EDGEMODE_NONE",3]],[["setStdDeviation",2]]],["SVGFEImageElement","SVGElement",["preserveAspectRatio","crossOrigin","x","y","width","height","result","href"],[],[]],["SVGFEMergeElement","SVGElement",["x","y","width","height","result"],[],[]],["SVGFEMergeNodeElement","SVGElement",["in1"],[],[]],["SVGFEMorphologyElement","SVGElement",["in1","operator","radiusX","radiusY","x","y","width","height","result"],[["SVG_MORPHOLOGY_OPERATOR_UNKNOWN",0],["SVG_MORPHOLOGY_OPERATOR_ERODE",1],["SVG_MORPHOLOGY_OPERATOR_DILATE",2]],[]],["SVGFEOffsetElement","SVGElement",["in1","dx","dy","x","y","width","height","result"],[],[]],["SVGFESpecularLightingElement","SVGElement",["in1","surfaceScale","specularConstant","specularExponent","kernelUnitLengthX","kernelUnitLengthY","x","y","width","height","result"],[],[]],["SVGFETileElement","SVGElement",["in1","x","y","width","height","result"],[],[]],["SVGFETurbulenceElement","SVGElement",["baseFrequencyX","baseFrequencyY","numOctaves","seed","stitchTiles","type","x","y","width","height","result"],[["SVG_TURBULENCE_TYPE_UNKNOWN",0],["SVG_TURBULENCE_TYPE_FRACTALNOISE",1],["SVG_TURBULENCE_TYPE_TURBULENCE",2],["SVG_STITCHTYPE_UNKNOWN",0],["SVG_STITCHTYPE_STITCH",1],["SVG_STITCHTYPE_NOSTITCH",2]],[]]]);

    /* (css-font-loading: a font list load() and check() can parse) */
    Object.defineProperty(global, '__cyberFontSpecError', { configurable: true, value: function (spec) {
        spec = String(spec);
        var bad = /var\(/i.test(spec) || /^\s*(initial|inherit|unset|revert|revert-layer|default)\s*$/i.test(spec);
        return bad ? new global.DOMException("Could not parse the font list '" + spec + "'", 'SyntaxError') : null;
    } });
    /* (document.fonts' events: the faces the sheets declare have loaded by the
     * time the page has, for a page that listens; dom_events.js calls this) */
    Object.defineProperty(global, '__cyberFontsLoaded', { configurable: true, value: function () {
        var d = global.document, set = d && d.fonts;
        if (!set || typeof set.onloadingdone !== 'function') return;
        var faces = [], sheets = d.styleSheets;
        for (var i = 0; i < sheets.length; i++) {
            var rules = null;
            try { rules = sheets[i].cssRules; } catch (e) { /* (not readable) */ }
            for (var k = 0; rules && k < rules.length; k++)
                if (rules[k].type === 5) faces.push(rules[k]);
        }
        if (!faces.length) return;
        var e = new global.Event('loadingdone');
        Object.defineProperty(e, 'fontfaces', { value: faces, enumerable: true });
        global.setTimeout(function () { set.onloadingdone(e); }, 0);
    } });

    /* (the parent in the flat tree) */
    function flatParent(n) { return n.assignedSlot || n.parentNode || n.host || null; }
    function isScrollBox(cs) { return /auto|scroll|hidden/.test(cs.overflowX + ' ' + cs.overflowY); }

    /* Scroll snap (CSS Scroll Snap 1 and 2): for each box, the targets it is
     * snapped to on each axis (those at the same position, in flat tree order)
     * and the offset they were for. */
    var snapStates = new WeakMap(), snapReported = new WeakMap(), snapChanging = new WeakMap(), snapIntents = new WeakMap();
    function snapAxes(cs) {
        var type = cs.scrollSnapType;
        if (!type || type === 'none') return null;
        return { x: /\b(x|inline|both|pair)\b/.test(type), y: /\b(y|block|both|pair)\b/.test(type), proximity: /proximity/.test(type) };
    }
    /* Each axis's snap positions, { t: the target, pos: the offset }. */
    function snapLists(el, axes) {
        /* (the root's scrollport is the viewport, which its box moves in) */
        var isRoot = el === el.ownerDocument.documentElement;
        var r = el.getBoundingClientRect(), left = isRoot ? 0 : r.left + el.clientLeft, top = isRoot ? 0 : r.top + el.clientTop;
        var sx = el.scrollLeft, sy = el.scrollTop, cw = el.clientWidth, ch = el.clientHeight;
        var maxX = el.scrollWidth - cw, maxY = el.scrollHeight - ch, out = { x: [], y: [] };
        var at = function (align, lo, hi, view) { return align === 'start' ? lo : align === 'end' ? hi - view : (lo + hi - view) / 2; };
        /* (what the cascade says has a scroll-snap-align, in tree order; a
         * nested scroller's targets are its own) */
        var aligned = global.__cyberRenderElements(16);
        for (var i = 0; i < aligned.length; i++) {
            var c = aligned[i], up = flatParent(c);
            while (up && up !== el && !(up.nodeType === 1 && isScrollBox(global.getComputedStyle(up)))) up = flatParent(up);
            if (up !== el || !c.getClientRects().length) continue;
            var align = global.getComputedStyle(c).scrollSnapAlign;
            if (!align || align === 'none') continue;
            var parts = align.split(' '), block = parts[0], inline = parts[1] || parts[0], b = c.getBoundingClientRect();
            if (axes.x && inline !== 'none')
                out.x.push({ t: c, pos: Math.max(0, Math.min(maxX, at(inline, b.left - left + sx, b.right - left + sx, cw))) });
            if (axes.y && block !== 'none')
                out.y.push({ t: c, pos: Math.max(0, Math.min(maxY, at(block, b.top - top + sy, b.bottom - top + sy, ch))) });
        }
        return out;
    }
    function snapNearest(list, cur) {
        var best = null;
        for (var i = 0; i < list.length; i++) if (best === null || Math.abs(list[i].pos - cur) < Math.abs(best - cur)) best = list[i].pos;
        return best;
    }
    function snapAt(list, pos) {
        var out = [];
        for (var i = 0; pos !== null && i < list.length; i++) if (Math.abs(list[i].pos - pos) < 1) out.push(list[i].t);
        return out;
    }
    /* (the one it follows: the focused one's, or the first) */
    function snapPreferred(targets) {
        var active = global.document.activeElement;
        for (var i = 0; active && active !== global.document.body && i < targets.length; i++)
            if (targets[i] === active || targets[i].contains(active)) return targets[i];
        return targets[0] || null;
    }
    /* (the root's go to its document, and bubble to the window) */
    function snapFire(el, type, block, inline) {
        var d = el.ownerDocument, root = el === d.documentElement;
        global.__cyberDispatchTrusted(root ? d : el, new global.SnapEvent(type, { bubbles: root, snapTargetBlock: block, snapTargetInline: inline }));
    }
    function snapReport(el, st) {
        var block = snapPreferred(st.y), inline = snapPreferred(st.x), was = snapReported.get(el);
        if (was && was.block === block && was.inline === inline) return;
        var now = { block: block, inline: inline };
        snapReported.set(el, now);
        snapChanging.set(el, now);
        if (block || inline) snapFire(el, 'scrollsnapchange', block, inline);
    }
    /* What a scroll is going to snap to, when that isn't what it was going to. */
    function snapAnnounce(el, st) {
        var block = snapPreferred(st.y), inline = snapPreferred(st.x), was = snapChanging.get(el) || snapReported.get(el);
        if (was ? was.block === block && was.inline === inline : !block && !inline) return;
        snapChanging.set(el, { block: block, inline: inline });
        snapFire(el, 'scrollsnapchanging', block, inline);
    }
    function snapRemember(el, lists, x, y) {
        var st = { x: snapAt(lists.x, x), y: snapAt(lists.y, y), sx: el.scrollLeft, sy: el.scrollTop };
        snapStates.set(el, st);
        return st;
    }
    if (!global.SnapEvent) {
        var SnapEvent = function SnapEvent(type, init) {
            var e = new global.Event(type, init);
            Object.setPrototypeOf(e, (new.target || SnapEvent).prototype);
            Object.defineProperty(e, 'snapTargetBlock', { value: init && init.snapTargetBlock || null, enumerable: true, configurable: true });
            Object.defineProperty(e, 'snapTargetInline', { value: init && init.snapTargetInline || null, enumerable: true, configurable: true });
            return e;
        };
        SnapEvent.prototype = Object.create(global.Event.prototype, { constructor: { value: SnapEvent, writable: true, configurable: true } });
        Object.defineProperty(global, 'SnapEvent', { value: SnapEvent, writable: true, configurable: true });
    }
    /* As a scroll ends: to the nearest snap position on each axis; true if it moved. */
    Object.defineProperty(global, '__cyberScrollSnap', { configurable: true, value: function (el) {
        if (el.nodeType === 9) el = el.documentElement;
        if (!el) return false;
        var axes = snapAxes(global.getComputedStyle(el));
        if (!axes) return false;
        var sx = el.scrollLeft, sy = el.scrollTop, lists = snapLists(el, axes);
        /* (a key press goes on to a position that way, however short its step) */
        var intent = snapIntents.get(el);
        snapIntents.delete(el);
        var ahead = function (list, from, sign) {
            if (!sign) return list;
            var on = list.filter(function (p) { return (p.pos - from) * sign >= 1; });
            return on.length ? on : list;
        };
        var bestX = snapNearest(intent ? ahead(lists.x, intent.x, intent.dx) : lists.x, sx);
        var bestY = snapNearest(intent ? ahead(lists.y, intent.y, intent.dy) : lists.y, sy);
        if (axes.proximity) {
            if (bestX !== null && Math.abs(bestX - sx) > el.clientWidth / 3) bestX = null;
            if (bestY !== null && Math.abs(bestY - sy) > el.clientHeight / 3) bestY = null;
        }
        var moved = false;
        snapAnnounce(el, { x: snapAt(lists.x, bestX), y: snapAt(lists.y, bestY) });
        if (bestX !== null && Math.abs(bestX - sx) >= 1) { el.scrollLeft = bestX; moved = true; }
        if (bestY !== null && Math.abs(bestY - sy) >= 1) { el.scrollTop = bestY; moved = true; }
        var st = snapRemember(el, lists, bestX, bestY);
        if (!moved) snapReport(el, st);
        return moved;
    } });
    Object.defineProperty(global, '__cyberSnapIntent', { configurable: true, value: function (el, x, y, dx, dy) {
        snapIntents.set(el, { x: x, y: y, dx: dx, dy: dy });
    } });
    /* A scroll under way: where it would snap now. */
    Object.defineProperty(global, '__cyberSnapProgress', { configurable: true, value: function (el) {
        if (el.nodeType === 9) el = el.documentElement;
        if (!el) return;
        var axes = snapAxes(global.getComputedStyle(el));
        if (!axes) return;
        var lists = snapLists(el, axes), sx = el.scrollLeft, sy = el.scrollTop;
        var bestX = snapNearest(lists.x, sx), bestY = snapNearest(lists.y, sy);
        if (axes.proximity) {
            if (bestX !== null && Math.abs(bestX - sx) > el.clientWidth / 3) bestX = null;
            if (bestY !== null && Math.abs(bestY - sy) > el.clientHeight / 3) bestY = null;
        }
        snapAnnounce(el, { x: snapAt(lists.x, bestX), y: snapAt(lists.y, bestY) });
    } });
    Object.defineProperty(global, '__cyberSnapCapture', { configurable: true, value: function (el) {
        if (!el || el.nodeType !== 1 || snapStates.has(el)) return;
        var axes = snapAxes(global.getComputedStyle(el));
        if (!axes) return;
        var lists = snapLists(el, axes);
        snapRemember(el, lists, snapNearest(lists.x, el.scrollLeft), snapNearest(lists.y, el.scrollTop));
    } });
    /* Layout changed: it stays with what it is snapped to (Scroll Snap 1, "re-snapping"). */
    function snapFollow(el, axes) {
        var st = snapStates.get(el);
        if (!st) { global.__cyberScrollSnap(el); return; }
        /* (scrolled since: its scroll's end decides) */
        if (st.sx !== el.scrollLeft || st.sy !== el.scrollTop) return;
        var lists = snapLists(el, axes);
        var follow = function (list, targets, cur) {
            var t = snapPreferred(targets);
            for (var i = 0; t && i < list.length; i++) if (list[i].t === t) return list[i].pos;
            return snapNearest(list, cur);
        };
        var x = follow(lists.x, st.x, st.sx), y = follow(lists.y, st.y, st.sy);
        if (x !== null && Math.abs(x - st.sx) >= 1) el.scrollLeft = x;
        if (y !== null && Math.abs(y - st.sy) >= 1) el.scrollTop = y;
        snapReport(el, snapRemember(el, lists, x, y));
    }

    /* content-visibility: auto (CSS Contain 2): whether its contents are
     * relevant to the user, and the event when that changes. */
    var cvSkipped = new WeakMap(), cvActivated = new WeakSet();
    if (!global.ContentVisibilityAutoStateChangeEvent) {
        var CVEvent = function ContentVisibilityAutoStateChangeEvent(type, init) {
            var e = new global.Event(type, init);
            Object.setPrototypeOf(e, (new.target || CVEvent).prototype);
            Object.defineProperty(e, 'skipped', { value: !!(init && init.skipped), enumerable: true, configurable: true });
            return e;
        };
        CVEvent.prototype = Object.create(global.Event.prototype, { constructor: { value: CVEvent, writable: true, configurable: true } });
        Object.defineProperty(global, 'ContentVisibilityAutoStateChangeEvent', { value: CVEvent, writable: true, configurable: true });
    }
    function cvRelevant(el) {
        var r = el.getBoundingClientRect(), mw = global.innerWidth / 2, mh = global.innerHeight / 2;
        if (r.bottom >= -mh && r.top <= global.innerHeight + mh && r.right >= -mw && r.left <= global.innerWidth + mw) return true;
        var active = global.document.activeElement;
        return !!active && active !== global.document.body && el.contains(active);
    }

    /* Scroll markers (CSS Overflow 5): which is its group's active one, and
     * what activating a marker or a button does. */
    var ST_TARGET_CURRENT = 32768;
    function scrollerSubject(el) {
        var d = el.ownerDocument;
        return el === d.documentElement ? d.scrollingElement || el : el;
    }
    function scrollOffsetOf(sc, t) {
        var isRoot = sc === sc.ownerDocument.scrollingElement || sc === sc.ownerDocument.documentElement;
        var b = sc.getBoundingClientRect(), r = t.getBoundingClientRect();
        var left = isRoot ? 0 : b.left + sc.clientLeft, top = isRoot ? 0 : b.top + sc.clientTop;
        return { x: r.left - left + sc.scrollLeft, y: r.top - top + sc.scrollTop };
    }
    function markersUpdate(el) {
        var sc = scrollerSubject(el), list = global.__cyberScrollMarkerTargets(el);
        var maxX = sc.scrollWidth - sc.clientWidth, maxY = sc.scrollHeight - sc.clientHeight, best = null, bestD = Infinity;
        for (var i = 0; i < list.length; i++) {
            var o = scrollOffsetOf(sc, list[i]);
            var d = Math.abs(Math.max(0, Math.min(maxX, o.x)) - sc.scrollLeft) + Math.abs(Math.max(0, Math.min(maxY, o.y)) - sc.scrollTop);
            if (d < bestD) { bestD = d; best = list[i]; }
        }
        for (var k = 0; k < list.length; k++) global.__cyberElState(list[k], ST_TARGET_CURRENT, list[k] === best);
    }
    function scrollerAround(el) {
        var d = el.ownerDocument;
        for (var a = flatParent(el); a && a !== d; a = flatParent(a))
            if (a.nodeType === 1 && a !== d.documentElement && a !== d.body && isScrollBox(global.getComputedStyle(a))) return a;
        return d.scrollingElement || d.documentElement;
    }
    /* A click at a point: true if on a marker or button, which it activates. */
    Object.defineProperty(global, '__cyberScrollPseudoClick', { configurable: true, value: function (x, y) {
        var hit = global.__cyberScrollPseudoAt(x, y);
        if (!hit) return false;
        var el = hit[0], kind = hit[1];
        if (kind === 5) {
            var sc = scrollerAround(el), o = scrollOffsetOf(sc, el);
            if (sc.scrollWidth > sc.clientWidth) sc.scrollLeft = o.x;
            if (sc.scrollHeight > sc.clientHeight) sc.scrollTop = o.y;
            return true;
        }
        /* (a page: what most browsers take one to be) */
        var s = scrollerSubject(el), page = 0.85;
        if (kind === 9) s.scrollTop -= s.clientHeight * page;
        else if (kind === 10) s.scrollTop += s.clientHeight * page;
        else if (kind === 11) s.scrollLeft -= s.clientWidth * page;
        else if (kind === 12) s.scrollLeft += s.clientWidth * page;
        return true;
    } });

    /* A rendering opportunity on a page with these features (layout_render_tick). */
    var initialDone = new WeakSet();
    function scrollToInitialTarget(el) {
        var a = el.assignedSlot || el.parentNode || el.host, d = el.ownerDocument;
        for (; a && a !== d; a = a.assignedSlot || a.parentNode || a.host)
            if (a.nodeType === 1 && a !== d.documentElement && a !== d.body && /auto|scroll|hidden/.test(global.getComputedStyle(a).overflowY + global.getComputedStyle(a).overflowX)) break;
        var r = el.getBoundingClientRect();
        if (a && a !== d) {
            var b = a.getBoundingClientRect();
            a.scrollLeft += r.left - b.left - a.clientLeft;
            a.scrollTop += r.top - b.top - a.clientTop;
        } else {
            global.scrollBy(r.left, r.top);
        }
    }
    Object.defineProperty(global, '__cyberRenderTick', { configurable: true, value: function (features) {
        var snappers = [], changed = [], again = false;
        if ((features & 8) && global.__cyberMediaCheck) global.__cyberMediaCheck();
        var list = global.__cyberRenderElements(7);
        for (var e = 0; e < list.length; e++) {
            var c = list[e], cs = global.getComputedStyle(c);
            var axes = snapAxes(cs);
            if (axes) snappers.push(c, axes);
            else if (snapStates.has(c)) snapStates.delete(c);
            if (cs.contentVisibility === 'auto' && c.getClientRects().length) {
                var skipped = !cvRelevant(c);
                if (cvActivated.delete(c)) { skipped = false; again = true; }
                if (cvSkipped.get(c) !== skipped) { cvSkipped.set(c, skipped); changed.push(c, skipped); }
            }
            if (cs.scrollInitialTarget === 'nearest' && !initialDone.has(c) && c.getClientRects().length) {
                initialDone.add(c);
                scrollToInitialTarget(c);
            }
        }
        for (var i = 0; i < snappers.length; i += 2) snapFollow(snappers[i], snappers[i + 1]);
        if (features & 32) {
            var groups = global.__cyberRenderElements(32);
            for (var g = 0; g < groups.length; g++) markersUpdate(groups[g]);
        }
        for (var k = 0; k < changed.length; k += 2)
            global.__cyberDispatchTrusted(changed[k], new global.ContentVisibilityAutoStateChangeEvent('contentvisibilityautostatechange', { skipped: changed[k + 1] }));
        /* (true: the next rendering looks again) */
        if (again) global.requestAnimationFrame(function () {});
        return again;
    } });

    /* (the transition running in a document) */
    if (global.Document && !('activeViewTransition' in global.Document.prototype))
        Object.defineProperty(global.Document.prototype, 'activeViewTransition', { configurable: true, enumerable: true, get: function () {
            return this.__cyberViewTransition || null;
        } });
    /* (a window that isn't a popup can't be moved or sized by script) */
    ['resizeTo', 'resizeBy', 'moveTo', 'moveBy'].forEach(function (n) {
        if (!(n in global)) Object.defineProperty(global, n, { configurable: true, enumerable: true, writable: true, value: function () {} });
    });

    /* (an iframe's size is its attributes) */
    if (global.HTMLIFrameElement) ['width', 'height'].forEach(function (a) {
        Object.defineProperty(global.HTMLIFrameElement.prototype, a, { configurable: true, enumerable: true,
            get: function () { var v = this.getAttribute(a); return v === null ? '' : v; },
            set: function (v) { this.setAttribute(a, String(v)); } });
    });

    /* (HTMLMetaElement reflects its attributes) */
    if (global.HTMLMetaElement) [['name', 'name'], ['content', 'content'], ['httpEquiv', 'http-equiv'], ['media', 'media'], ['scheme', 'scheme']].forEach(function (m) {
        Object.defineProperty(global.HTMLMetaElement.prototype, m[0], { configurable: true, enumerable: true,
            get: function () { var v = this.getAttribute(m[1]); return v === null ? '' : v; },
            set: function (v) { this.setAttribute(m[1], String(v)); } });
    });

    /* ---- View transitions: the API and its promises; nothing is captured
     * or animated, the update simply happens ---- */
    function ViewTransition() { throw new TypeError('Illegal constructor'); }
    /* What runs on each pseudo-element of a transition (the user agent's sheet,
     * then the page's rules): animationstart on the root for each.  Nothing is
     * captured or drawn. */
    var viewTransitionEvents = function (doc) {
        var root = doc.documentElement, names = [], seen = {};
        if (!root || !global.AnimationEvent) return;
        var all = doc.querySelectorAll('*');
        for (var i = 0; i < all.length; i++) {
            var n = global.getComputedStyle(all[i]).viewTransitionName;
            if (all[i] === root && (!n || n === 'auto')) n = 'root';
            if (n && n !== 'none' && n !== 'auto' && !seen[n]) { seen[n] = true; names.push(n); }
        }
        if (!names.length) return;
        var rules = [];
        var walk = function (list) {
            for (var i = 0; i < list.length; i++) {
                var r = list[i];
                if (r.selectorText) {
                    var m = /::view-transition-(group|image-pair|old|new)\(([^)]*)\)/.exec(r.selectorText);
                    if (m && r.style.animationName) rules.push({ kind: m[1], name: m[2].trim(), anim: r.style.animationName });
                } else if (r.cssRules) walk(r.cssRules);
            }
        };
        for (var s = 0; s < doc.styleSheets.length; s++) {
            try { walk(doc.styleSheets[s].cssRules); } catch (e) { /* (cross-origin) */ }
        }
        var ua = { group: function (n) { return ['-ua-view-transition-group-anim-' + n]; }, 'image-pair': function () { return []; },
                   old: function () { return ['-ua-view-transition-fade-out', '-ua-mix-blend-mode-plus-lighter']; },
                   new: function () { return ['-ua-view-transition-fade-in', '-ua-mix-blend-mode-plus-lighter']; } };
        names.forEach(function (name) {
            ['group', 'image-pair', 'old', 'new'].forEach(function (kind) {
                var anims = ua[kind](name);
                rules.forEach(function (r) {
                    if (r.kind === kind && (r.name === '*' || r.name === name))
                        anims = r.anim === 'none' ? [] : r.anim.split(',').map(function (x) { return x.trim(); });
                });
                anims.forEach(function (a) {
                    root.dispatchEvent(new global.AnimationEvent('animationstart', { bubbles: true, animationName: a, elapsedTime: 0,
                        pseudoElement: '::view-transition-' + kind + '(' + name + ')' }));
                });
            });
        });
    };
    var newViewTransition = function (update, types, doc) {
        var vt = Object.create(ViewTransition.prototype), resolveReady, rejectReady, resolveFinished, rejectFinished,
            resolveDone, rejectDone, skipped = false, timedOut = false;
        var ready = new Promise(function (a, b) { resolveReady = a; rejectReady = b; });
        var finished = new Promise(function (a, b) { resolveFinished = a; rejectFinished = b; });
        var done = new Promise(function (a, b) { resolveDone = a; rejectDone = b; });
        /* (the page's three promises have no handlers of ours: a rejection
         * nobody takes is reported as unhandled) */
        var timer = global.setTimeout(function () {
            timedOut = true;
            rejectReady(new global.DOMException('Transition update callback timed out', 'TimeoutError'));
        }, 4000);
        Promise.resolve().then(update).then(function (v) {
            global.clearTimeout(timer);
            if (timedOut) return;
            resolveDone(v);
            if (skipped) rejectReady(new global.DOMException('Transition was skipped', 'AbortError'));
            else {
                resolveReady();
                if (doc) global.setTimeout(function () { if (!skipped) viewTransitionEvents(doc); }, 0);
            }
            resolveFinished();
        }, function (reason) {
            global.clearTimeout(timer);
            if (timedOut) return;
            rejectDone(reason);
            rejectReady(reason);
            rejectFinished(reason);
        });
        Object.defineProperties(vt, {
            updateCallbackDone: { value: done, enumerable: true }, ready: { value: ready, enumerable: true },
            finished: { value: finished, enumerable: true },
            types: { value: new Set(types || []), enumerable: true },
            __skipped: { get: function () { return skipped; } },
            skipTransition: { value: function skipTransition() { skipped = true; }, enumerable: true }
        });
        return vt;
    };
    if (global.document && !global.Document.prototype.startViewTransition) {
        expose('ViewTransition', ViewTransition);
        Object.defineProperty(global.Document.prototype, 'startViewTransition', { configurable: true, enumerable: true, writable: true,
            value: function startViewTransition(arg) {
                var update = typeof arg === 'function' ? arg : arg && arg.update, types = arg && typeof arg === 'object' ? arg.types : null;
                /* (a new one skips the one running) */
                if (this.__cyberViewTransition) this.__cyberViewTransition.skipTransition();
                var vt = newViewTransition(function () { return update ? update() : undefined; }, types, this);
                Object.defineProperty(this, '__cyberViewTransition', { value: vt, configurable: true, writable: true });
                return vt;
            } });
    }
    /* The last @view-transition in force in the document's sheets. */
    var viewTransitionRule = function () {
        var found = null;
        var walk = function (rules) {
            for (var i = 0; i < rules.length; i++) {
                var r = rules[i], n = r.__kind;
                if (n === 'view-transition') { if (r.navigation) found = r; }
                else if (n === 'layer' || (n === 'media' && global.matchMedia(r.media.mediaText).matches) ||
                         (n === 'supports' && global.CSS.supports(r.conditionText))) walk(r.cssRules);
            }
        };
        var sheets = global.document ? global.document.styleSheets : [];
        for (var i = 0; i < sheets.length; i++) {
            try { walk(sheets[i].cssRules); } catch (e) { /* (cross-origin) */ }
        }
        return found;
    };
    var pageTransitionEvent = function (type) {
        var C = function (t, init) {
            var e = new global.Event(t, init);
            Object.defineProperty(e, 'viewTransition', { value: init && init.viewTransition || null, enumerable: true });
            if (type === 'PageSwapEvent') Object.defineProperty(e, 'activation', { value: init && init.activation || null, enumerable: true });
            return e;
        };
        Object.defineProperty(C, 'name', { value: type });
        C.prototype = Object.create(global.Event.prototype, { constructor: { value: C, writable: true, configurable: true } });
        expose(type, C);
        return C;
    };
    if (global.Event && global.addEventListener) {
        var PageSwapEvent = pageTransitionEvent('PageSwapEvent'), PageRevealEvent = pageTransitionEvent('PageRevealEvent');
        var nothing = function () {};
        Object.defineProperty(global, '__cyberPageSwap', { configurable: true, value: function (url, same, how) {
            var rule = viewTransitionRule();
            var vt = same && rule && rule.navigation === 'auto' ? newViewTransition(nothing, rule.types) : null;
            var activation = { entry: { url: url }, from: { url: global.location.href }, navigationType: how || 'replace' };
            global.__cyberOutboundVT = vt;
            global.__cyberDispatchTrusted(global, new PageSwapEvent('pageswap', { viewTransition: vt, activation: activation }));
            return !!vt && !vt.__skipped;
        } });
        /* (before the load event: this engine has no first frame to wait for) */
        Object.defineProperty(global, '__cyberPageReveal', { configurable: true, value: function () {
            /* (only after a document that opted in: reading every sheet's rules isn't free) */
            var rule = global.__cyberOldOptIn ? viewTransitionRule() : null;
            var vt = rule && rule.navigation === 'auto' ? newViewTransition(nothing, rule.types) : null;
            global.__cyberDispatchTrusted(global, new PageRevealEvent('pagereveal', { viewTransition: vt }));
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

    /* (window.location and document.location: [PutForwards=href]) */
    var LOCATION = global.location;
    if (LOCATION && typeof LOCATION === 'object') {
        var locationAccessor = { configurable: true, enumerable: true, get: function () { return LOCATION; },
                                 set: function (v) { LOCATION.href = String(v); } };
        Object.defineProperty(global, 'location', locationAccessor);
        if (global.document) { try { Object.defineProperty(global.document, 'location', locationAccessor); } catch (e) { /* (fixed) */ } }
    }

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
            return Promise.resolve({ interrupted: false });
        } });
        Object.defineProperty(global, name, { configurable: true, enumerable: true, writable: true, value: function () {
            var o = scrollArgs(name, 'Window', arguments), el = scrollViewportElement();
            if (el) scrollApply(el, o, by);
            return Promise.resolve({ interrupted: false });
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
    /* (inner: where in el, a frame, what is scrolled to is) */
    function scrollIntoViewOf(el, block, inline, inner) {
        var d = el.ownerDocument, root = d.documentElement;
        var rectOf = function () {
            var r = el.getBoundingClientRect();
            if (!inner) return r;
            var left = r.left + el.clientLeft + inner.left, top = r.top + el.clientTop + inner.top;
            return { left: left, top: top, right: left + inner.right - inner.left, bottom: top + inner.bottom - inner.top };
        };
        /* (a skipped box a descendant is scrolled to shows it, whatever it does then) */
        var marked = global.__cyberRenderElements(7);
        for (var mi = 0; mi < marked.length; mi++)
            if (marked[mi] !== el && marked[mi].contains(el) && global.getComputedStyle(marked[mi]).contentVisibility === 'auto') cvActivated.add(marked[mi]);
        for (var a = el.assignedSlot || el.parentNode || el.host; a && a !== d; a = a.assignedSlot || a.parentNode || a.host) {
            if (a.nodeType !== 1 || a === root || a === d.body) continue;
            if (a.scrollHeight <= a.clientHeight && a.scrollWidth <= a.clientWidth) continue;
            /* (overflow: clip and visible: no scrolling box) */
            if (!isScrollBox(global.getComputedStyle(a))) continue;
            var r = rectOf(), b = a.getBoundingClientRect();
            var l = b.left + a.clientLeft, t = b.top + a.clientTop;
            a.scrollLeft += scrollAlign(inline, r.left, r.right, l, l + a.clientWidth);
            a.scrollTop += scrollAlign(block, r.top, r.bottom, t, t + a.clientHeight);
        }
        var se = d.scrollingElement;
        if (!se) return;
        var v = rectOf();
        se.scrollLeft += scrollAlign(inline, v.left, v.right, 0, global.innerWidth);
        se.scrollTop += scrollAlign(block, v.top, v.bottom, 0, global.innerHeight);
        /* (the parent's too, if this window may reach it) */
        var fe = global.frameElement, view = fe && fe.ownerDocument && fe.ownerDocument.defaultView;
        if (view && view.__cyberScrollIntoView) view.__cyberScrollIntoView(fe, block, inline, rectOf());
    }
    Object.defineProperty(global, '__cyberScrollIntoView', { configurable: true, value: scrollIntoViewOf });
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
            return Promise.resolve({ interrupted: false });
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
    /* (small, full, ...) */
    var FULL_KANA = [
                       0x3041, 0x3042, 0x3043, 0x3044, 0x3045, 0x3046, 0x3047, 0x3048, 0x3049, 0x304a, 0x3063, 0x3064, 0x3083, 0x3084, 0x3085, 0x3086, 0x3087, 0x3088,
                       0x308e, 0x308f, 0x3095, 0x304b, 0x3096, 0x3051, 0x30a1, 0x30a2, 0x30a3, 0x30a4, 0x30a5, 0x30a6, 0x30a7, 0x30a8, 0x30a9, 0x30aa, 0x30c3, 0x30c4,
                       0x30e3, 0x30e4, 0x30e5, 0x30e6, 0x30e7, 0x30e8, 0x30ee, 0x30ef, 0x30f5, 0x30ab, 0x30f6, 0x30b1, 0x31f0, 0x30af, 0x31f1, 0x30b7, 0x31f2, 0x30b9,
                       0x31f3, 0x30c8, 0x31f4, 0x30cc, 0x31f5, 0x30cf, 0x31f6, 0x30d2, 0x31f7, 0x30d5, 0x31f8, 0x30d8, 0x31f9, 0x30db, 0x31fa, 0x30e0, 0x31fb, 0x30e9,
                       0x31fc, 0x30ea, 0x31fd, 0x30eb, 0x31fe, 0x30ec, 0x31ff, 0x30ed, 0x1b132, 0x3053, 0x1b150, 0x3090, 0x1b151, 0x3091, 0x1b152, 0x3092, 0x1b155, 0x30b3,
                       0x1b164, 0x30f0, 0x1b165, 0x30f1, 0x1b166, 0x30f2, 0x1b167, 0x30f3, 0xff67, 0xff71, 0xff68, 0xff72, 0xff69, 0xff73, 0xff6a, 0xff74, 0xff6b, 0xff75,
                       0xff6c, 0xff94, 0xff6d, 0xff95, 0xff6e, 0xff96, 0xff6f, 0xff82];
    /* (from, to, how many) */
    var MATH_ITALIC = [0x41, 0x1d434, 26, 0x61, 0x1d44e, 7, 0x68, 0x210e, 1, 0x69, 0x1d456, 18, 0x131, 0x1d6a4, 1, 0x237, 0x1d6a5, 1, 0x391, 0x1d6e2, 17, 0x3f4, 0x1d6f3, 1,
                       0x3a3, 0x1d6f4, 7, 0x2207, 0x1d6fb, 1, 0x3b1, 0x1d6fc, 25, 0x2202, 0x1d715, 1, 0x3f5, 0x1d716, 1, 0x3d1, 0x1d717, 1, 0x3f0, 0x1d718, 1, 0x3d5, 0x1d719, 1,
                       0x3f1, 0x1d71a, 1, 0x3d6, 0x1d71b, 1];
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
        /* (math-auto shows one letter in its italic form, and that's what's selected) */
        var plainText = s.toString;
        define(s, 'toString', function toString() {
            var t = plainText.call(this), r = this.rangeCount ? this.getRangeAt(0) : null;
            var n = r && r.startContainer, el = n && (n.nodeType === 1 ? n : n.parentElement);
            if (!el || !t || Array.from(t).length !== 1) return t;
            var c = t.codePointAt(0), tt = global.getComputedStyle(el).textTransform, o = null;
            /* (a small kana, full size) */
            if (tt === 'full-size-kana') {
                var ki = FULL_KANA.indexOf(c);
                return ki >= 0 && !(ki & 1) ? String.fromCodePoint(FULL_KANA[ki + 1]) : t;
            }
            if (tt !== 'math-auto') return t;
            for (var k = 0; k < MATH_ITALIC.length; k += 3) if (c >= MATH_ITALIC[k] && c < MATH_ITALIC[k] + MATH_ITALIC[k + 2]) o = MATH_ITALIC[k + 1] + c - MATH_ITALIC[k];
            return o === null ? t : String.fromCodePoint(o);
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

    /* Observers (Performance Timeline 2): each with what it has queued. */
    /* (this realm's start: the process has served other pages) */
    var observers = [], resources = [], resourcesSeen = global.__cyberResourceEntries ? global.__cyberResourceEntries() : 0;
    function queueFor(o) {
        if (o.scheduled) return;
        o.scheduled = true;
        global.setTimeout(function () {
            o.scheduled = false;
            if (!o.queue.length || !o.active) return;
            var list = new PerformanceObserverEntryList(o.queue);
            o.queue = [];
            o.callback.call(o.self, list, o.self, { droppedEntriesCount: 0 });
        }, 0);
    }
    function deliver(e) {
        for (var i = 0; i < observers.length; i++) {
            var o = observers[i];
            if (o.active && o.types.indexOf(e.entryType) >= 0) { o.queue.push(e); queueFor(o); }
        }
    }
    function PerformanceResourceTiming() { illegal(); }
    PerformanceResourceTiming.prototype = Object.create(PerformanceEntry.prototype, { constructor: { value: PerformanceResourceTiming, writable: true, configurable: true } });
    ['fetchStart', 'requestStart', 'responseStart', 'domainLookupStart', 'domainLookupEnd', 'connectStart', 'connectEnd', 'redirectStart', 'redirectEnd', 'secureConnectionStart', 'workerStart'].forEach(function (k, i) {
        Object.defineProperty(PerformanceResourceTiming.prototype, k, { configurable: true, enumerable: true, get: function () {
            return i < 3 ? this._d.startTime : 0; } });
    });
    Object.defineProperty(PerformanceResourceTiming.prototype, 'responseEnd', { configurable: true, enumerable: true, get: function () { return this._d.startTime + this._d.duration; } });
    Object.defineProperty(PerformanceResourceTiming.prototype, 'initiatorType', { configurable: true, enumerable: true, get: function () { return 'other'; } });
    Object.defineProperty(PerformanceResourceTiming.prototype, 'nextHopProtocol', { configurable: true, enumerable: true, get: function () { return 'http/1.1'; } });
    ['transferSize', 'encodedBodySize', 'decodedBodySize'].forEach(function (k) {
        Object.defineProperty(PerformanceResourceTiming.prototype, k, { configurable: true, enumerable: true, get: function () { return 0; } });
    });
    /* What has been fetched since the last look. */
    function pollResources() {
        if (!global.__cyberResourceEntries) return;
        var got = global.__cyberResourceEntries(resourcesSeen), origin = perf.timeOrigin || 0;
        for (var i = 0; i + 2 < got.length; i += 3) {
            var e = entry(PerformanceResourceTiming.prototype, got[i], 'resource', got[i + 1] - origin, got[i + 2] - got[i + 1]);
            resources.push(e);
            deliver(e);
        }
        resourcesSeen += got.length / 3;
    }
    function resourcePoll(o) {
        if (!o.active || o.types.indexOf('resource') < 0) return;
        pollResources();
        global.setTimeout(function () { resourcePoll(o); }, 50);
    }
    function PerformanceObserverEntryList(entries) {
        Object.defineProperty(this, '_e', { value: entries.slice().sort(byTime) });
    }
    define(PerformanceObserverEntryList.prototype, 'getEntries', function getEntries() { return this._e.slice(); });
    define(PerformanceObserverEntryList.prototype, 'getEntriesByType', function getEntriesByType(type) {
        type = String(type);
        return this._e.filter(function (x) { return x.entryType === type; });
    });
    define(PerformanceObserverEntryList.prototype, 'getEntriesByName', function getEntriesByName(name, type) {
        name = String(name);
        return this._e.filter(function (x) { return x.name === name && (type === undefined || x.entryType === String(type)); });
    });
    var SUPPORTED_TYPES = ['mark', 'measure', 'resource'];
    var PO_STATE = new WeakMap();
    function PerformanceObserver(callback) {
        if (!(this instanceof PerformanceObserver)) throw new TypeError("Failed to construct 'PerformanceObserver': Please use the 'new' operator");
        if (typeof callback !== 'function') throw new TypeError("Failed to construct 'PerformanceObserver': parameter 1 is not a function");
        PO_STATE.set(this, { callback: callback, self: this, types: [], queue: [], active: false, scheduled: false, mode: null });
    }
    define(PerformanceObserver.prototype, 'observe', function observe(options) {
        var o = PO_STATE.get(this), opt = options || {};
        var hasList = opt.entryTypes !== undefined, hasType = opt.type !== undefined;
        if (hasList === hasType) throw new TypeError("Failed to execute 'observe' on 'PerformanceObserver': exactly one of entryTypes and type");
        if (hasList && opt.buffered !== undefined) { /* (ignored with entryTypes) */ }
        var mode = hasList ? 'list' : 'single';
        if (o.mode && o.mode !== mode) throw new (global.DOMException)('Cannot change the observer type', 'InvalidModificationError');
        o.mode = mode;
        var was = o.types;
        if (hasList) {
            o.types = Array.prototype.slice.call(opt.entryTypes).map(String).filter(function (t) { return SUPPORTED_TYPES.indexOf(t) >= 0; });
            if (!o.types.length) return;
        } else {
            var t = String(opt.type);
            if (SUPPORTED_TYPES.indexOf(t) < 0) return;
            if (o.types.indexOf(t) < 0) o.types = o.types.concat([t]);
        }
        if (!o.active) { o.active = true; observers.push(o); }
        if (hasType && opt.buffered) {
            pollResources();
            var old = opt.type === 'mark' ? marks : opt.type === 'measure' ? measures : resources;
            for (var i = 0; i < old.length; i++) if (o.queue.indexOf(old[i]) < 0) o.queue.push(old[i]);
            if (o.queue.length) queueFor(o);
        }
        if (o.types.indexOf('resource') >= 0 && was.indexOf('resource') < 0) resourcePoll(o);
    });
    define(PerformanceObserver.prototype, 'disconnect', function disconnect() {
        var o = PO_STATE.get(this);
        o.active = false;
        o.queue = [];
        o.types = [];
        o.mode = null;
        var at = observers.indexOf(o);
        if (at >= 0) observers.splice(at, 1);
    });
    define(PerformanceObserver.prototype, 'takeRecords', function takeRecords() {
        var o = PO_STATE.get(this), q = o.queue;
        o.queue = [];
        return q;
    });
    Object.defineProperty(PerformanceObserver, 'supportedEntryTypes', { configurable: true, enumerable: true,
        get: function () { return Object.freeze(SUPPORTED_TYPES.slice()); } });

    define(PerfProto, 'mark', function mark(name, options) {
        var e = new PerformanceMark(name, options);
        marks.push(e);
        deliver(e);
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
        deliver(m);
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
        if (type === 'resource') { pollResources(); return resources.slice(); }
        return nativeByType ? nativeByType.call(this, type) : [];
    });
    define(PerfProto, 'getEntriesByName', function getEntriesByName(name, type) {
        name = String(name);
        var all = type === undefined ? this.getEntries() : this.getEntriesByType(type);
        return all.filter(function (x) { return x.name === name; });
    });
    define(PerfProto, 'getEntries', function getEntries() {
        var other = nativeAll ? Array.prototype.filter.call(nativeAll.call(this), function (x) {
            return x.entryType !== 'mark' && x.entryType !== 'measure' && x.entryType !== 'resource';
        }) : [];
        pollResources();
        return other.concat(marks, measures, resources).sort(byTime);
    });
    expose('PerformanceObserver', PerformanceObserver);
    expose('PerformanceObserverEntryList', PerformanceObserverEntryList);
    expose('PerformanceResourceTiming', PerformanceResourceTiming);
    expose('PerformanceEntry', PerformanceEntry);
    expose('PerformanceMark', PerformanceMark);
    expose('PerformanceMeasure', PerformanceMeasure);
})

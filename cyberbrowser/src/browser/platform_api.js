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
        case 'DOMPoint': case 'DOMPointReadOnly': case 'DOMRect': case 'DOMRectReadOnly':
        case 'DOMQuad': case 'DOMMatrix': case 'DOMMatrixReadOnly': {
            /* (by the interface's own getters: an instance's may be anything) */
            var own = function (o, k) {
                for (var p = Object.getPrototypeOf(o); p; p = Object.getPrototypeOf(p)) {
                    var d = Object.getOwnPropertyDescriptor(p, k);
                    if (d && d.get) return d.get.call(o);
                }
                return o[k];
            };
            var pt = function (o) { return [own(o, 'x'), own(o, 'y'), own(o, 'z'), own(o, 'w')]; };
            if (/Point/.test(kind)) return { i: id, g: kind, gv: pt(v) };
            if (/Rect/.test(kind)) return { i: id, g: kind, gv: [own(v, 'x'), own(v, 'y'), own(v, 'width'), own(v, 'height')] };
            if (kind === 'DOMQuad') return { i: id, g: kind, gv: [pt(own(v, 'p1')), pt(own(v, 'p2')), pt(own(v, 'p3')), pt(own(v, 'p4'))] };
            var two = own(v, 'is2D');
            var names = two ? ['a', 'b', 'c', 'd', 'e', 'f'] : ['m11', 'm12', 'm13', 'm14', 'm21', 'm22', 'm23', 'm24', 'm31', 'm32', 'm33', 'm34', 'm41', 'm42', 'm43', 'm44'];
            return { i: id, g: kind, gv: names.map(function (k) { return own(v, k); }) };
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
        } else if (w.g !== undefined) {
            var C = global[w.g], gv = w.gv;
            if (/Point/.test(w.g)) v = new C(gv[0], gv[1], gv[2], gv[3]);
            else if (/Rect/.test(w.g)) v = new C(gv[0], gv[1], gv[2], gv[3]);
            else if (w.g === "DOMQuad") v = new C(new global.DOMPoint(gv[0][0], gv[0][1], gv[0][2], gv[0][3]), new global.DOMPoint(gv[1][0], gv[1][1], gv[1][2], gv[1][3]),
                                                        new global.DOMPoint(gv[2][0], gv[2][1], gv[2][2], gv[2][3]), new global.DOMPoint(gv[3][0], gv[3][1], gv[3][2], gv[3][3]));
            else v = new C(gv);
            made[w.i] = v;
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
        /* (the rest of its descriptors: held here, nothing in them reaches the font yet) */
        var ffOverride = function (v) { v = String(v).trim(); return v === 'normal' || /^[+]?[\d.]+%$/.test(v) ? v : null; };
        var ffSettings = function (v) {
            v = String(v).trim();
            if (v === 'normal') return v;
            return global.CSS && global.CSS.supports('font-feature-settings', v) ? v : null;
        };
        var ffVariations = function (v) {
            v = String(v).trim();
            if (v === 'normal') return v;
            return global.CSS && global.CSS.supports('font-variation-settings', v) ? v : null;
        };
        var ffMore = {
            unicodeRange: ['U+0-10FFFF', function (v) {
                v = String(v).trim();
                if (!/^[uU]\+[0-9a-fA-F?]{1,6}(-[0-9a-fA-F]{1,6})?(\s*,\s*[uU]\+[0-9a-fA-F?]{1,6}(-[0-9a-fA-F]{1,6})?)*$/.test(v)) return null;
                /* (a range with ? in it: its first and last code points) */
                return v.split(/\s*,\s*/).map(function (r) {
                    var m = /^[uU]\+([0-9a-fA-F]*)(\?+)$/.exec(r);
                    if (!m) return r.toUpperCase();
                    var lo = (m[1] + m[2].replace(/\?/g, '0')).replace(/^0+(?=.)/, ''), hi = (m[1] + m[2].replace(/\?/g, 'F')).replace(/^0+(?=.)/, '');
                    return ('U+' + lo + '-' + hi).toUpperCase();
                }).join(', ');
            }],
            variant: ['normal', function (v) { return global.CSS && global.CSS.supports('font-variant', String(v)) ? String(v).trim() : null; }],
            featureSettings: ['normal', ffSettings],
            variationSettings: ['normal', ffVariations],
            display: ['auto', function (v) { v = String(v).trim(); return /^(auto|block|swap|fallback|optional)$/.test(v) ? v : null; }],
            ascentOverride: ['normal', ffOverride],
            descentOverride: ['normal', ffOverride],
            lineGapOverride: ['normal', ffOverride],
        };
        var ffHeld = new WeakMap(), ffOf = function (f) {
            if (!(f instanceof global.FontFace)) throw new TypeError('Illegal invocation');
            var h = ffHeld.get(f);
            if (!h) ffHeld.set(f, h = {});
            return h;
        };
        Object.keys(ffMore).forEach(function (k) {
            var def = ffMore[k][0], check = ffMore[k][1];
            Object.defineProperty(global.FontFace.prototype, k, { enumerable: true, configurable: true,
                get: function () { var h = ffOf(this); return k in h ? h[k] : def; },
                set: function (v) {
                    var h = ffOf(this), ok = check(v);
                    if (ok === null) throw new global.DOMException("'" + v + "' is not a valid value for " + k, 'SyntaxError');
                    h[k] = ok;
                } });
        });
        /* (a name that isn't one, as a string) */
        var ffFamily = function (v) {
            v = String(v).trim();
            if (/^["'].*["']$/.test(v)) return v;
            if (/^-?(?:[a-zA-Z_\u0080-\uffff]|--)[-\w\u0080-\uffff]*$/.test(v) && !/^(?:initial|inherit|unset|revert|revert-layer|default)$/i.test(v)) return v;
            return '"' + v.replace(/(["\\])/g, '\\$1') + '"';
        };
        var ffFam = Object.getOwnPropertyDescriptor(global.FontFace.prototype, 'family');
        if (ffFam && ffFam.get) Object.defineProperty(global.FontFace.prototype, 'family', { enumerable: true, configurable: true,
            get: function () { var h = ffOf(this); return 'family' in h ? h.family : ffFamily(ffFam.get.call(this)); },
            set: function (v) { ffOf(this).family = ffFamily(v); if (ffFam.set) ffFam.set.call(this, v); } });
        var ffOrig = global.FontFace, ffCtor = function FontFace(family, source) {
            if (!new.target) throw new TypeError("Failed to construct 'FontFace': Please use the 'new' operator");
            if (arguments.length < 2) throw new TypeError("Failed to construct 'FontFace': 2 arguments required");
            /* (a list with nothing in it to load: load() says so) */
            var srcBad = false;
            if (typeof source === 'string' && global.__cyberFontSrcParts) {
                srcBad = !global.__cyberFontSrcParts(source).some(function (p) { return p.local || p.format === null || /^(collection|opentype|truetype|woff|woff2)(-variations)?$/i.test(p.format); });
            }
            var f = Reflect.construct(ffOrig, arguments, new.target === ffCtor ? ffOrig : new.target), init = arguments[2];
            if (srcBad) ffOf(f).badSrc = true;
            if (init && typeof init === 'object') Object.keys(ffMore).forEach(function (k) {
                if (init[k] === undefined) return;
                var ok = ffMore[k][1](init[k]);
                if (ok !== null) ffOf(f)[k] = ok;
            });
            /* (weight, style and stretch too; width is stretch, and goes first) */
            if (init && typeof init === 'object') ['weight', 'style', 'stretch'].forEach(function (k) {
                var v = k === 'stretch' && init.width !== undefined ? init.width : init[k];
                if (v === undefined) return;
                try { f[k] = v; } catch (e) { /* (invalid: as if not given) */ }
            });
            return f;
        };
        var ffLoad = ffOrig.prototype.load;
        if (ffLoad) Object.defineProperty(ffOrig.prototype, 'load', { configurable: true, enumerable: true, writable: true, value: function load() {
            if (ffOf(this).badSrc) return Promise.reject(new global.DOMException('The source provided could not be parsed as a value list.', 'SyntaxError'));
            return ffLoad.call(this);
        } });
        /* (width: stretch's other name) */
        Object.defineProperty(ffOrig.prototype, 'width', { enumerable: true, configurable: true,
            get: function () { return this.stretch; }, set: function (v) { this.stretch = v; } });
        /* (features, variations and palettes: the font's, empty until it says) */
        [['features', 'FontFaceFeatures'], ['variations', 'FontFaceVariations'], ['palettes', 'FontFacePalettes']].forEach(function (p) {
            if (Object.getOwnPropertyDescriptor(ffOrig.prototype, p[0])) return;
            Object.defineProperty(ffOrig.prototype, p[0], { enumerable: true, configurable: true, get: function () {
                var h = ffOf(this), key = '__' + p[0];
                if (!h[key]) { var I = global[p[1]]; h[key] = I && I.prototype ? Object.create(I.prototype) : {}; }
                return h[key];
            } });
        });
        Object.defineProperty(ffCtor, 'length', { configurable: true, value: 2 });
        Object.defineProperty(ffCtor, 'prototype', { value: ffOrig.prototype, writable: false });
        Object.defineProperty(ffOrig.prototype, 'constructor', { configurable: true, writable: true, value: ffCtor });
        Object.defineProperty(global, 'FontFace', { configurable: true, writable: true, value: ffCtor });
    }
    /* (FontFaceSet: load() and check() of a font that isn't one, a SyntaxError;
     * size; no constructor) */
    (function () {
        var FS = global.FontFaceSet, FP = FS && FS.prototype;
        if (!FP && global.document && global.document.fonts) FP = Object.getPrototypeOf(global.document.fonts);
        if (!FP || FP === Object.prototype) return;
        /* (an interface with that prototype, no constructor of its own) */
        if (!FS || !Object.prototype.hasOwnProperty.call(FS, 'prototype')) {
            var C = { FontFaceSet: function () { throw new TypeError('Illegal constructor'); } }.FontFaceSet;
            Object.defineProperty(C, 'length', { configurable: true, value: 0 });
            Object.defineProperty(C, 'prototype', { value: FP, writable: false });
            Object.defineProperty(FP, 'constructor', { configurable: true, writable: true, value: C });
            Object.defineProperty(global, 'FontFaceSet', { configurable: true, writable: true, value: C });
        }
        var bad = function (spec) {
            spec = String(spec);
            var words = spec.trim().split(/\s+/);
            var wide = /^(initial|inherit|unset|default|revert|revert-layer)$/i;
            return words.some(function (w) { return wide.test(w); }) || !spec.trim();
        };
        ['load', 'check'].forEach(function (name) {
            var orig = FP[name];
            if (typeof orig !== 'function') return;
            var fn = function (font) {
                if (bad(font)) {
                    var err = new global.DOMException("'" + font + "' is not a valid font", 'SyntaxError');
                    if (name === 'check') throw err;
                    return Promise.reject(err);
                }
                return orig.apply(this, arguments);
            };
            Object.defineProperty(fn, 'name', { configurable: true, value: name });
            Object.defineProperty(fn, 'length', { configurable: true, value: 1 });
            Object.defineProperty(FP, name, { configurable: true, enumerable: true, writable: true, value: fn });
        });
        if (!('size' in FP)) Object.defineProperty(FP, 'size', { configurable: true, enumerable: true, get: function () {
            var n = 0;
            if (typeof this.forEach === 'function') this.forEach(function () { n++; });
            return n;
        } });
        /* (an EventTarget; its operations and attributes check their this, a promise's rejecting) */
        if (global.EventTarget && Object.getPrototypeOf(FP) === Object.prototype) Object.setPrototypeOf(FP, global.EventTarget.prototype);
        var mine = function (o) { return o !== null && o !== undefined && (o === FP || FP.isPrototypeOf(o)) && o !== FP; };
        ['add', 'delete', 'clear', 'check', 'has', 'forEach', 'entries', 'keys', 'values', 'load'].forEach(function (name) {
            var d = Object.getOwnPropertyDescriptor(FP, name);
            if (!d || typeof d.value !== 'function') return;
            var orig = d.value, promised = name === 'load';
            var need = { add: 1, delete: 1, check: 1, has: 1, forEach: 1, load: 1 }[name] || 0;
            var fn = function () {
                var e = !mine(this) ? new TypeError('Illegal invocation') :
                        arguments.length < need ? new TypeError("Failed to execute '" + name + "' on 'FontFaceSet': " + need + ' argument required, but only 0 present.') : null;
                if (e) {
                    if (promised) return Promise.reject(e);
                    throw e;
                }
                return orig.apply(this, arguments);
            };
            Object.defineProperty(fn, 'name', { configurable: true, value: name });
            Object.defineProperty(fn, 'length', { configurable: true, value: orig.length });
            Object.defineProperty(FP, name, { configurable: true, enumerable: true, writable: true, value: fn });
        });
        /* (the document's: its @font-face rules' faces too, CSS-connected) */
        var cssFaceOf = new WeakMap();
        var cssFaces = function (set) {
            var doc = global.document, out = [];
            if (!doc || set !== doc.fonts || !global.FontFace) return out;
            var walk = function (rules) {
                for (var i = 0; rules && i < rules.length; i++) {
                    var r = rules[i];
                    if (r.type === 5) {
                        var f = cssFaceOf.get(r);
                        if (!f) {
                            var st = r.style, get = function (n) { return st ? st.getPropertyValue(n) : ''; };
                            var fam = get('font-family').trim().replace(/^(["'])(.*)\1$/, '$2');
                            var init = {}, map = { style: 'font-style', weight: 'font-weight', stretch: 'font-stretch', width: 'font-width',
                                unicodeRange: 'unicode-range', featureSettings: 'font-feature-settings', variationSettings: 'font-variation-settings',
                                display: 'font-display', ascentOverride: 'ascent-override', descentOverride: 'descent-override', lineGapOverride: 'line-gap-override' };
                            Object.keys(map).forEach(function (k) { var v = get(map[k]); if (v) init[k] = v; });
                            try { f = new global.FontFace(fam, get('src') || 'local(none)', init); } catch (e) { f = null; }
                            if (f) cssFaceOf.set(r, f);
                        }
                        if (f) out.push(f);
                    } else if (r.cssRules && (r.type === 4 || r.type === 12 || r.__kind === 'layer')) walk(r.cssRules);
                }
            };
            var sheets = doc.styleSheets;
            for (var s = 0; s < sheets.length; s++) { try { walk(sheets[s].cssRules); } catch (e) { /* (not readable) */ } }
            return out;
        };
        var nativeOf = function (set, name) { var d = Object.getOwnPropertyDescriptor(FP, name); return d && d.value ? Array.from(d.value.call(set)) : []; };
        var baseValues = FP.values, baseEntries = FP.entries, baseForEach = FP.forEach;
        var all = function (set) {
            var mine = baseValues ? Array.from(baseValues.call(set)) : [];
            cssFaces(set).forEach(function (f) { if (mine.indexOf(f) < 0) mine.unshift(f); });
            return mine;
        };
        if (baseValues) {
            var mk = function (name, fn) {
                Object.defineProperty(fn, 'name', { configurable: true, value: name });
                Object.defineProperty(FP, name, { configurable: true, enumerable: true, writable: true, value: fn });
            };
            mk('values', function values() { if (!mine(this)) throw new TypeError('Illegal invocation'); return all(this)[Symbol.iterator](); });
            mk('keys', FP.values);
            mk('entries', function entries() { if (!mine(this)) throw new TypeError('Illegal invocation'); return all(this).map(function (f) { return [f, f]; })[Symbol.iterator](); });
            mk('forEach', function forEach(cb) {
                if (!mine(this)) throw new TypeError('Illegal invocation');
                if (typeof cb !== 'function') throw new TypeError("Failed to execute 'forEach' on 'FontFaceSet': 1 argument required");
                var self = this, that = arguments[1];
                all(this).forEach(function (f) { cb.call(that, f, f, self); });
            });
        }
        void nativeOf; void baseEntries; void baseForEach;
        if (typeof FP.values === 'function') Object.defineProperty(FP, Symbol.iterator, { configurable: true, writable: true, value: FP.values });
        ['status', 'ready'].forEach(function (name) {
            var d = Object.getOwnPropertyDescriptor(FP, name);
            if (!d || typeof d.get !== 'function') return;
            var g = d.get, promised = name === 'ready';
            var get = function () {
                if (!mine(this)) {
                    var e = new TypeError('Illegal invocation');
                    if (promised) return Promise.reject(e);
                    throw e;
                }
                return g.call(this);
            };
            Object.defineProperty(get, 'name', { configurable: true, value: 'get ' + name });
            Object.defineProperty(FP, name, { configurable: true, enumerable: true, get: get, set: d.set });
        });
        /* (fonts: Document.prototype's) */
        var DPf = global.Document && global.Document.prototype, doc = global.document;
        if (DPf && doc && Object.prototype.hasOwnProperty.call(doc, 'fonts') && !Object.prototype.hasOwnProperty.call(DPf, 'fonts')) {
            var dd = Object.getOwnPropertyDescriptor(doc, 'fonts'), theSet = dd.get ? dd.get.call(doc) : dd.value;
            Object.defineProperty(DPf, 'fonts', { configurable: true, enumerable: true, get: function fonts() {
                if (!(this instanceof global.Document)) throw new TypeError('Illegal invocation');
                return this === doc ? theSet : null;
            } });
            delete doc.fonts;
        }
    })();
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
        /* (the caret at a point: the text there and the character boundary nearest it; [node, offset] or null) */
        var caretAt = function (doc, x, y) {
            if (!(doc instanceof global.Document)) throw new TypeError('Illegal invocation');
            if (!doc.defaultView) return null;
            if (x < 0 || y < 0 || x > global.innerWidth || y > global.innerHeight) return null;
            var el = doc.elementFromPoint(x, y);
            if (!el) return null;
            var ln = (el.localName || '').toLowerCase();
            /* (a textarea: the place in its value, by its lines and its font) */
            if (ln === 'textarea') {
                var cs = global.getComputedStyle(el), box = el.getBoundingClientRect(), val = String(el.value);
                var fs = parseFloat(cs.fontSize) || 13, lh = parseFloat(cs.lineHeight) || 1.2;
                if (!/px$/.test(cs.lineHeight)) lh *= fs;
                var top = box.top + (parseFloat(cs.borderTopWidth) || 0) + (parseFloat(cs.paddingTop) || 0);
                var left = box.left + (parseFloat(cs.borderLeftWidth) || 0) + (parseFloat(cs.paddingLeft) || 0);
                var lines = val.split('\n'), row = Math.floor((y - top) / lh);
                if (row < 0) return [el, 0];
                if (row >= lines.length) return [el, val.length];
                var start = 0;
                for (var q = 0; q < row; q++) start += lines[q].length + 1;
                var cx = null;
                try { cx = doc.createElement('canvas').getContext('2d'); cx.font = cs.font || (fs + 'px ' + cs.fontFamily); } catch (e) { cx = null; }
                var line = lines[row], col = line.length;
                if (cx && line && !(cx.measureText(line).width > 0)) cx = null;
                for (var c = 0; c < line.length; c++) {
                    var w0 = cx ? cx.measureText(line.slice(0, c)).width : c * fs * 0.6;
                    var w1 = cx ? cx.measureText(line.slice(0, c + 1)).width : (c + 1) * fs * 0.6;
                    if (x - left < (w0 + w1) / 2) { col = c; break; }
                }
                return [el, start + col];
            }
            /* (audio and video: the place before them in their parent) */
            if ((ln === 'audio' || ln === 'video') && el.parentNode) return [el.parentNode, Array.prototype.indexOf.call(el.parentNode.childNodes, el)];
            /* (a control or a replaced element: the element itself) */
            if (/^(input|select|button|canvas|img|iframe|embed|object)$/.test(ln)) return [el, 0];
            var texts = [], walk = function (n) {
                for (var c = n.firstChild; c && texts.length < 64; c = c.nextSibling) {
                    if (c.nodeType === 3) texts.push(c);
                    else if (c.nodeType === 1) walk(c.shadowRoot || c);
                }
            };
            walk(el.shadowRoot || el);
            var best = null, bestD = Infinity, budget = 4000, r = doc.createRange();
            for (var k = 0; k < texts.length && budget > 0; k++) {
                var t = texts[k], n = t.data.length;
                for (var i = 0; i < n && budget > 0; i++, budget--) {
                    r.setStart(t, i);
                    r.setEnd(t, i + 1);
                    var rs = r.getClientRects();
                    for (var j = 0; j < rs.length; j++) {
                        var b = rs[j];
                        if (!b.width && !b.height) continue;
                        var dy = y < b.top ? b.top - y : y >= b.bottom ? y - b.bottom : 0;
                        var dx = x < b.left ? b.left - x : x >= b.right ? x - b.right : 0;
                        var d = dy * 1e4 + dx;
                        if (d < bestD) { bestD = d; best = [t, x < b.left + b.width / 2 ? i : i + 1]; }
                        if (!d) break;
                    }
                    if (!bestD) break;
                }
                if (!bestD) break;
            }
            return best || [el.firstChild && el.firstChild.nodeType === 3 ? el.firstChild : el, 0];
        };
        /* (elementFromPoint: Document.prototype's, as cssom-view has it, not the document's own) */
        if (DocP && global.document && Object.prototype.hasOwnProperty.call(global.document, 'elementFromPoint')) {
            var efp = global.document.elementFromPoint;
            Object.defineProperty(DocP, 'elementFromPoint', { writable: true, enumerable: true, configurable: true,
                value: function elementFromPoint(x, y) {
                    if (!(this instanceof global.Document)) throw new TypeError('Illegal invocation');
                    if (arguments.length < 2) throw new TypeError("Failed to execute 'elementFromPoint' on 'Document': 2 arguments required, but only " + arguments.length + ' present.');
                    if (!isFinite(+x) || !isFinite(+y)) throw new TypeError("Failed to execute 'elementFromPoint' on 'Document': The provided double value is non-finite.");
                    return efp.call(this, x, y);
                } });
            delete global.document.elementFromPoint;
        }
        var coord = function (v, name) {
            v = +v;
            if (!isFinite(v)) throw new TypeError("Failed to execute '" + name + "' on 'Document': The provided double value is non-finite.");
            return v;
        };
        if (DocP && !DocP.caretPositionFromPoint) Object.defineProperty(DocP, 'caretPositionFromPoint', { writable: true, enumerable: true, configurable: true,
            value: function caretPositionFromPoint(x, y) {
                if (!(this instanceof global.Document)) throw new TypeError('Illegal invocation');
                if (arguments.length < 2) throw new TypeError('caretPositionFromPoint: 2 arguments required');
                var at = caretAt(this, coord(x, 'caretPositionFromPoint'), coord(y, 'caretPositionFromPoint'));
                if (!at) return null;
                var p = Object.create(CaretP.prototype);
                Object.defineProperty(p, '__node', { value: at[0] });
                Object.defineProperty(p, '__offset', { value: at[1] });
                return p;
            } });
        if (DocP && !DocP.caretRangeFromPoint) Object.defineProperty(DocP, 'caretRangeFromPoint', { writable: true, enumerable: true, configurable: true,
            value: function caretRangeFromPoint(x, y) {
                var at = caretAt(this, x === undefined ? 0 : coord(x, 'caretRangeFromPoint'), y === undefined ? 0 : coord(y, 'caretRangeFromPoint'));
                if (!at) return null;
                var r = this.createRange();
                r.setStart(at[0], at[1]);
                r.collapse(true);
                return r;
            } });
        /* ---- SVG geometry: shapes as polylines (user units), their lengths,
         * points, hit tests and boxes; text by its font's measure ---- */
        var SvgP = global.SVGElement && global.SVGElement.prototype;
        if (SvgP && !SvgP.getBBox) (function () {
            var num = function (el, a, d) { var v = parseFloat(el.getAttribute(a)); return isFinite(v) ? v : d; };
            /* (a path's d: its subpaths, each { p: [[x, y]...], z: closed }) */
            var pathPolys = function (d) {
                var toks = String(d || '').match(/[MmLlHhVvCcSsQqTtAaZz]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?/g) || [];
                var out = [], cur = null, x = 0, y = 0, sx = 0, sy = 0, cmd = '', i = 0, lcx = 0, lcy = 0, lq = null;
                var n = function () { return +toks[i++]; };
                var to = function (nx, ny) { if (!cur) { cur = { p: [[x, y]], z: false }; out.push(cur); } cur.p.push([nx, ny]); x = nx; y = ny; };
                var curve = function (pts) {   /* (cubic or quadratic: sampled) */
                    var x0 = x, y0 = y, N = 16;
                    for (var s = 1; s <= N; s++) {
                        var t = s / N, u = 1 - t, px, py;
                        if (pts.length === 4) { px = u * u * x0 + 2 * u * t * pts[0] + t * t * pts[2]; py = u * u * y0 + 2 * u * t * pts[1] + t * t * pts[3]; }
                        else { px = u * u * u * x0 + 3 * u * u * t * pts[0] + 3 * u * t * t * pts[2] + t * t * t * pts[4]; py = u * u * u * y0 + 3 * u * u * t * pts[1] + 3 * u * t * t * pts[3] + t * t * t * pts[5]; }
                        to(px, py);
                    }
                };
                var arc = function (rx, ry, phi, fa, fs, x2, y2) {
                    var x1 = x, y1 = y;
                    rx = Math.abs(rx); ry = Math.abs(ry);
                    if (!rx || !ry) { to(x2, y2); return; }
                    var c = Math.cos(phi * Math.PI / 180), s = Math.sin(phi * Math.PI / 180);
                    var dx = (x1 - x2) / 2, dy = (y1 - y2) / 2, xp = c * dx + s * dy, yp = -s * dx + c * dy;
                    var lam = xp * xp / (rx * rx) + yp * yp / (ry * ry);
                    if (lam > 1) { rx *= Math.sqrt(lam); ry *= Math.sqrt(lam); }
                    var num2 = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp, den = rx * rx * yp * yp + ry * ry * xp * xp;
                    var k = (fa === fs ? -1 : 1) * Math.sqrt(Math.max(0, num2 / den));
                    var cxp = k * rx * yp / ry, cyp = -k * ry * xp / rx;
                    var cx = c * cxp - s * cyp + (x1 + x2) / 2, cy = s * cxp + c * cyp + (y1 + y2) / 2;
                    var ang = function (ux, uy, vx, vy) { var a = Math.atan2(ux * vy - uy * vx, ux * vx + uy * vy); return a; };
                    var t1 = ang(1, 0, (xp - cxp) / rx, (yp - cyp) / ry), dt = ang((xp - cxp) / rx, (yp - cyp) / ry, (-xp - cxp) / rx, (-yp - cyp) / ry);
                    if (!fs && dt > 0) dt -= 2 * Math.PI; else if (fs && dt < 0) dt += 2 * Math.PI;
                    var N = Math.max(4, Math.ceil(Math.abs(dt) / (Math.PI / 16)));
                    for (var q = 1; q <= N; q++) {
                        var t = t1 + dt * q / N;
                        to(q === N ? x2 : cx + rx * Math.cos(t) * c - ry * Math.sin(t) * s, q === N ? y2 : cy + rx * Math.cos(t) * s + ry * Math.sin(t) * c);
                    }
                };
                while (i < toks.length) {
                    if (/[A-Za-z]/.test(toks[i])) cmd = toks[i++];
                    else if (!cmd) break;
                    var rel = cmd === cmd.toLowerCase() && cmd !== 'z' ? 1 : 0, ox = rel ? x : 0, oy = rel ? y : 0, C = cmd.toUpperCase();
                    if (C === 'Z') { if (cur) { cur.z = true; x = sx; y = sy; cur = null; } lq = null; continue; }
                    if (C === 'M') { var mx = n() + ox, my = n() + oy; cur = { p: [[mx, my]], z: false }; out.push(cur); x = sx = mx; y = sy = my; cmd = rel ? 'l' : 'L'; lq = null; continue; }
                    if (C === 'L') to(n() + ox, n() + oy);
                    else if (C === 'H') to(n() + ox, y);
                    else if (C === 'V') to(x, n() + oy);
                    else if (C === 'C') { var c6 = [n() + ox, n() + oy, n() + ox, n() + oy, n() + ox, n() + oy]; curve(c6); lcx = c6[2]; lcy = c6[3]; lq = 'c'; continue; }
                    else if (C === 'S') { var r1 = lq === 'c' ? [2 * x - lcx, 2 * y - lcy] : [x, y], s4 = [n() + ox, n() + oy, n() + ox, n() + oy]; curve([r1[0], r1[1], s4[0], s4[1], s4[2], s4[3]]); lcx = s4[0]; lcy = s4[1]; lq = 'c'; continue; }
                    else if (C === 'Q') { var q4 = [n() + ox, n() + oy, n() + ox, n() + oy]; curve(q4); lcx = q4[0]; lcy = q4[1]; lq = 'q'; continue; }
                    else if (C === 'T') { var r2 = lq === 'q' ? [2 * x - lcx, 2 * y - lcy] : [x, y], t2 = [n() + ox, n() + oy]; curve([r2[0], r2[1], t2[0], t2[1]]); lcx = r2[0]; lcy = r2[1]; lq = 'q'; continue; }
                    else if (C === 'A') { var a5 = [n(), n(), n(), n(), n()]; arc(a5[0], a5[1], a5[2], a5[3], a5[4], n() + ox, n() + oy); }
                    else break;
                    lq = null;
                }
                return out.filter(function (sp) { return sp.p.every(function (pt) { return isFinite(pt[0]) && isFinite(pt[1]); }); });
            };
            var shapePolys = function (el) {
                var ln = el.localName, k, pts;
                if (ln === 'path') return pathPolys(el.getAttribute('d'));
                if (ln === 'rect') {
                    var x = num(el, 'x', 0), y = num(el, 'y', 0), w = num(el, 'width', 0), h = num(el, 'height', 0);
                    return w > 0 && h > 0 ? [{ p: [[x, y], [x + w, y], [x + w, y + h], [x, y + h], [x, y]], z: true }] : [];
                }
                if (ln === 'circle' || ln === 'ellipse') {
                    var cx = num(el, 'cx', 0), cy = num(el, 'cy', 0), rx = ln === 'circle' ? num(el, 'r', 0) : num(el, 'rx', 0), ry = ln === 'circle' ? rx : num(el, 'ry', 0);
                    if (!(rx > 0 && ry > 0)) return [];
                    pts = [];
                    for (k = 0; k <= 64; k++) pts.push([cx + rx * Math.cos(k * Math.PI / 32), cy + ry * Math.sin(k * Math.PI / 32)]);
                    return [{ p: pts, z: true }];
                }
                if (ln === 'line') return [{ p: [[num(el, 'x1', 0), num(el, 'y1', 0)], [num(el, 'x2', 0), num(el, 'y2', 0)]], z: false }];
                if (ln === 'polyline' || ln === 'polygon') {
                    var v = String(el.getAttribute('points') || '').match(/[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?/g) || [];
                    pts = [];
                    for (k = 0; k + 1 < v.length; k += 2) pts.push([+v[k], +v[k + 1]]);
                    if (ln === 'polygon' && pts.length) pts.push(pts[0].slice());
                    return pts.length ? [{ p: pts, z: ln === 'polygon' }] : [];
                }
                return null;
            };
            var segLen = function (a, b) { return Math.hypot(b[0] - a[0], b[1] - a[1]); };
            var polysLength = function (ps) {
                var L = 0;
                ps.forEach(function (sp) { for (var k = 1; k < sp.p.length; k++) L += segLen(sp.p[k - 1], sp.p[k]); if (sp.z && sp.p.length) L += segLen(sp.p[sp.p.length - 1], sp.p[0]); });
                return L;
            };
            /* (a transform attribute: [a, b, c, d, e, f]) */
            var mul = function (m, n2) {
                return [m[0] * n2[0] + m[2] * n2[1], m[1] * n2[0] + m[3] * n2[1], m[0] * n2[2] + m[2] * n2[3], m[1] * n2[2] + m[3] * n2[3],
                        m[0] * n2[4] + m[2] * n2[5] + m[4], m[1] * n2[4] + m[3] * n2[5] + m[5]];
            };
            var parseTransform = function (t) {
                var m = [1, 0, 0, 1, 0, 0], re = /(matrix|translate|scale|rotate|skewX|skewY)\s*\(([^)]*)\)/g, g;
                while ((g = re.exec(String(t || '')))) {
                    var a = (g[2].match(/[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?/g) || []).map(Number), f = g[1], o;
                    if (f === 'matrix' && a.length === 6) o = a;
                    else if (f === 'translate') o = [1, 0, 0, 1, a[0] || 0, a[1] || 0];
                    else if (f === 'scale') o = [a[0], 0, 0, a.length > 1 ? a[1] : a[0], 0, 0];
                    else if (f === 'rotate') {
                        var r = (a[0] || 0) * Math.PI / 180, cs2 = Math.cos(r), sn = Math.sin(r);
                        o = [cs2, sn, -sn, cs2, 0, 0];
                        if (a.length === 3) o = mul(mul([1, 0, 0, 1, a[1], a[2]], o), [1, 0, 0, 1, -a[1], -a[2]]);
                    }
                    else if (f === 'skewX') o = [1, 0, Math.tan((a[0] || 0) * Math.PI / 180), 1, 0, 0];
                    else if (f === 'skewY') o = [1, Math.tan((a[0] || 0) * Math.PI / 180), 0, 1, 0, 0];
                    if (o && o.every(isFinite)) m = mul(m, o);
                }
                return m;
            };
            var apply = function (m, p) { return [m[0] * p[0] + m[2] * p[1] + m[4], m[1] * p[0] + m[3] * p[1] + m[5]]; };
            var nearestSvg = function (el) { for (var a = el.parentNode; a && a.nodeType === 1; a = a.parentNode) if (a.localName === 'svg' && a.namespaceURI === el.namespaceURI) return a; return null; };
            /* (to the nearest <svg>'s user space: the element's own and its ancestors' transforms) */
            var ctmOf = function (el) {
                var m = [1, 0, 0, 1, 0, 0];
                for (var a = el; a && a.nodeType === 1 && !(a !== el && a.localName === 'svg'); a = a.parentNode)
                    if (a.localName !== 'svg') m = mul(parseTransform(a.getAttribute('transform')), m);
                return m;
            };
            var fontOf = function (el) {
                var cs = global.getComputedStyle(el), fs = parseFloat(cs.fontSize) || 16;
                return { fs: fs, font: (cs.fontStyle && cs.fontStyle !== 'normal' ? cs.fontStyle + ' ' : '') + (cs.fontWeight && cs.fontWeight !== '400' ? cs.fontWeight + ' ' : '') + fs + 'px ' + (cs.fontFamily || 'sans-serif') };
            };
            var measureCtx = null;
            var textWidth = function (el, s) {
                var f = fontOf(el);
                try {
                    if (!measureCtx) measureCtx = global.document.createElement('canvas').getContext('2d');
                    measureCtx.font = f.font;
                    var w = measureCtx.measureText(s).width;
                    if (w > 0 || !s) return w;
                } catch (e) { /* (by estimate) */ }
                return s.length * f.fs * 0.6;
            };
            var isText = function (el) { return /^(text|tspan|textPath)$/.test(el.localName); };
            var textOrigin = function (el) {
                for (var a = el; a && a.nodeType === 1 && isText(a); a = a.parentNode) {
                    var xs = parseFloat(String(a.getAttribute('x') || '').trim().split(/[\s,]+/)[0]), ys = parseFloat(String(a.getAttribute('y') || '').trim().split(/[\s,]+/)[0]);
                    if (isFinite(xs) || isFinite(ys)) return [isFinite(xs) ? xs : 0, isFinite(ys) ? ys : 0];
                }
                return [0, 0];
            };
            /* (the box of el in its own user space; null: nothing) */
            var ownBox = function (el) {
                var ps = shapePolys(el), b = null, add = function (p) {
                    if (!b) b = [p[0], p[1], p[0], p[1]];
                    else { b[0] = Math.min(b[0], p[0]); b[1] = Math.min(b[1], p[1]); b[2] = Math.max(b[2], p[0]); b[3] = Math.max(b[3], p[1]); }
                };
                if (ps) { ps.forEach(function (sp) { sp.p.forEach(add); }); return b; }
                if (isText(el)) {
                    var s = el.textContent || '', o = textOrigin(el), f = fontOf(el);
                    if (!s) return null;
                    return [o[0], o[1] - f.fs * 0.8, o[0] + textWidth(el, s), o[1] + f.fs * 0.2];
                }
                /* (a group: its children's, each by its transform) */
                for (var c = el.firstElementChild; c; c = c.nextElementSibling) {
                    if (c.localName === 'defs' || c.localName === 'style' || c.localName === 'title' || c.localName === 'desc') continue;
                    var cb = ownBox(c);
                    if (!cb) continue;
                    var m = parseTransform(c.getAttribute('transform'));
                    [[cb[0], cb[1]], [cb[2], cb[1]], [cb[0], cb[3]], [cb[2], cb[3]]].forEach(function (p) { add(apply(m, p)); });
                }
                return b;
            };
            var svgBox = function (el) {   /* (in the nearest <svg>'s user space) */
                var b = ownBox(el);
                if (!b) return null;
                var m = mul(ctmOf(el), [1, 0, 0, 1, 0, 0]), q = null;
                /* (ctmOf includes el's own transform: its box is before it) */
                [[b[0], b[1]], [b[2], b[1]], [b[0], b[3]], [b[2], b[3]]].forEach(function (p) {
                    var r = apply(m, p);
                    if (!q) q = [r[0], r[1], r[0], r[1]];
                    else { q[0] = Math.min(q[0], r[0]); q[1] = Math.min(q[1], r[1]); q[2] = Math.max(q[2], r[0]); q[3] = Math.max(q[3], r[1]); }
                });
                return q;
            };
            var def = function (name, fn) { Object.defineProperty(SvgP, name, { writable: true, enumerable: true, configurable: true, value: fn }); };
            var geom = function (el, name) {
                var ps = shapePolys(el);
                if (!ps) throw new TypeError("Failed to execute '" + name + "': not a geometry element");
                return ps;
            };
            def('getTotalLength', function getTotalLength() { return polysLength(geom(this, 'getTotalLength')); });
            def('getPointAtLength', function getPointAtLength(d) {
                var ps = geom(this, 'getPointAtLength'), left = Math.max(0, +d || 0), last = null;
                for (var s = 0; s < ps.length; s++) {
                    var p = ps[s].p.slice();
                    if (ps[s].z && p.length) p.push(p[0]);
                    for (var k = 1; k < p.length; k++) {
                        var L = segLen(p[k - 1], p[k]);
                        if (left <= L && L > 0) return new global.DOMPoint(p[k - 1][0] + (p[k][0] - p[k - 1][0]) * left / L, p[k - 1][1] + (p[k][1] - p[k - 1][1]) * left / L);
                        left -= L;
                        last = p[k];
                    }
                    if (!last && p.length) last = p[0];
                }
                return new global.DOMPoint(last ? last[0] : 0, last ? last[1] : 0);
            });
            def('isPointInFill', function isPointInFill(pt) {
                var ps = geom(this, 'isPointInFill'), x = +(pt && pt.x) || 0, y = +(pt && pt.y) || 0, wn = 0, odd = false;
                ps.forEach(function (sp) {
                    var p = sp.p.slice();
                    if (p.length) p.push(p[0]);
                    for (var k = 1; k < p.length; k++) {
                        var a = p[k - 1], b = p[k];
                        if ((a[1] <= y) !== (b[1] <= y)) {
                            var xi = a[0] + (y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]);
                            if (x < xi) { odd = !odd; wn += b[1] > a[1] ? 1 : -1; }
                        }
                    }
                });
                return global.getComputedStyle(this).fillRule === 'evenodd' ? odd : wn !== 0;
            });
            def('isPointInStroke', function isPointInStroke(pt) {
                var ps = geom(this, 'isPointInStroke'), x = +(pt && pt.x) || 0, y = +(pt && pt.y) || 0;
                var sw = parseFloat(global.getComputedStyle(this).strokeWidth);
                var hw = (isFinite(sw) ? sw : 1) / 2;
                return ps.some(function (sp) {
                    var p = sp.p.slice();
                    if (sp.z && p.length) p.push(p[0]);
                    for (var k = 1; k < p.length; k++) {
                        var a = p[k - 1], b = p[k], dx = b[0] - a[0], dy = b[1] - a[1], l2 = dx * dx + dy * dy;
                        var t = l2 ? Math.max(0, Math.min(1, ((x - a[0]) * dx + (y - a[1]) * dy) / l2)) : 0;
                        if (Math.hypot(x - a[0] - t * dx, y - a[1] - t * dy) <= hw + 1e-9) return true;
                    }
                    return false;
                });
            });
            def('getBBox', function getBBox() {
                var b = ownBox(this);
                return b ? new global.DOMRect(b[0], b[1], b[2] - b[0], b[3] - b[1]) : new global.DOMRect(0, 0, 0, 0);
            });
            def('getCTM', function getCTM() { var m = ctmOf(this); return new global.DOMMatrix(m); });
            def('getScreenCTM', function getScreenCTM() {
                var m = ctmOf(this), svg = this.localName === 'svg' ? this : nearestSvg(this);
                if (svg && svg.getBoundingClientRect) {
                    var r = svg.getBoundingClientRect(), vb = String(svg.getAttribute('viewBox') || '').trim().split(/[\s,]+/).map(Number), base = [1, 0, 0, 1, r.left, r.top];
                    if (vb.length === 4 && vb[2] > 0 && vb[3] > 0 && r.width && r.height) {
                        var s = Math.min(r.width / vb[2], r.height / vb[3]);
                        base = mul(base, [s, 0, 0, s, (r.width - vb[2] * s) / 2 - vb[0] * s, (r.height - vb[3] * s) / 2 - vb[1] * s]);
                    }
                    m = mul(base, m);
                }
                return new global.DOMMatrix(m);
            });
            Object.defineProperty(SvgP, 'ownerSVGElement', { enumerable: true, configurable: true, get: function () { return nearestSvg(this); } });
            Object.defineProperty(SvgP, 'viewportElement', { enumerable: true, configurable: true, get: function () { return nearestSvg(this); } });
            /* (text content) */
            def('getNumberOfChars', function getNumberOfChars() { return (this.textContent || '').length; });
            def('getComputedTextLength', function getComputedTextLength() { return textWidth(this, this.textContent || ''); });
            def('getSubStringLength', function getSubStringLength(i, n) {
                var s = this.textContent || '';
                i = i >>> 0;
                if (i >= s.length) throw new global.DOMException('Index out of range', 'IndexSizeError');
                return textWidth(this, s.substr(i, n >>> 0));
            });
            def('getCharNumAtPosition', function getCharNumAtPosition(pt) {
                var s = this.textContent || '', o = textOrigin(this), f = fontOf(this), x = +(pt && pt.x) || 0, y = +(pt && pt.y) || 0;
                if (y < o[1] - f.fs || y > o[1] + f.fs * 0.3) return -1;
                for (var k = 0; k < s.length; k++) {
                    var x0 = o[0] + textWidth(this, s.slice(0, k)), x1 = o[0] + textWidth(this, s.slice(0, k + 1));
                    if (x >= x0 && x < x1) return k;
                }
                return -1;
            });
            def('getStartPositionOfChar', function getStartPositionOfChar(i) {
                var s = this.textContent || '', o = textOrigin(this);
                i = i >>> 0;
                if (i >= s.length) throw new global.DOMException('Index out of range', 'IndexSizeError');
                return new global.DOMPoint(o[0] + textWidth(this, s.slice(0, i)), o[1]);
            });
            def('getEndPositionOfChar', function getEndPositionOfChar(i) {
                var s = this.textContent || '', o = textOrigin(this);
                i = i >>> 0;
                if (i >= s.length) throw new global.DOMException('Index out of range', 'IndexSizeError');
                return new global.DOMPoint(o[0] + textWidth(this, s.slice(0, i + 1)), o[1]);
            });
            def('getExtentOfChar', function getExtentOfChar(i) {
                var s = this.textContent || '', o = textOrigin(this), f = fontOf(this);
                i = i >>> 0;
                if (i >= s.length) throw new global.DOMException('Index out of range', 'IndexSizeError');
                var x0 = o[0] + textWidth(this, s.slice(0, i));
                return new global.DOMRect(x0, o[1] - f.fs * 0.8, textWidth(this, s.charAt(i)), f.fs);
            });
            /* (<svg>'s own) */
            def('createSVGRect', function createSVGRect() { return new global.DOMRect(); });
            def('createSVGPoint', function createSVGPoint() { return new global.DOMPoint(); });
            def('createSVGMatrix', function createSVGMatrix() { return new global.DOMMatrix(); });
            var meets = function (b, r, enclose) {
                if (!b) return false;
                var rx = +r.x || 0, ry = +r.y || 0, rr = rx + (+r.width || 0), rb = ry + (+r.height || 0);
                return enclose ? b[0] >= rx && b[1] >= ry && b[2] <= rr && b[3] <= rb : b[0] <= rr && b[2] >= rx && b[1] <= rb && b[3] >= ry;
            };
            def('checkIntersection', function checkIntersection(el, r) { return meets(svgBox(el), r, false); });
            def('checkEnclosure', function checkEnclosure(el, r) { return meets(svgBox(el), r, true); });
            var listOf = function (svg, r, ref, enclose) {
                var out = [], all = svg.querySelectorAll('path, rect, circle, ellipse, line, polyline, polygon, text, image, use');
                for (var k = 0; k < all.length; k++) {
                    if (ref && !ref.contains(all[k])) continue;
                    if (meets(svgBox(all[k]), r, enclose)) out.push(all[k]);
                }
                return out;
            };
            def('getIntersectionList', function getIntersectionList(r, ref) { return listOf(this, r, ref, false); });
            /* (SVGAnimatedEnumeration: an attribute's keyword as its number; 0 is unknown) */
            var enumAttr = function (el, attr, words, dflt) {
                var o = Object.create(global.SVGAnimatedEnumeration ? global.SVGAnimatedEnumeration.prototype : Object.prototype);
                var read = function () { var k = words.indexOf(el.getAttribute(attr)); return k > 0 ? k : el.hasAttribute(attr) ? (words.indexOf(dflt) > 0 ? words.indexOf(dflt) : 0) : words.indexOf(dflt); };
                Object.defineProperty(o, 'baseVal', { enumerable: true, configurable: true, get: read, set: function (v) {
                    v = Number(v);
                    if (!(v > 0 && v < words.length && Math.floor(v) === v)) throw new TypeError("Failed to set the 'baseVal' property on 'SVGAnimatedEnumeration': The enumeration value provided (" + v + ') is invalid.');
                    el.setAttribute(attr, words[v]);
                } });
                Object.defineProperty(o, 'animVal', { enumerable: true, configurable: true, get: read });
                return o;
            };
            var BLEND = ['', 'normal', 'multiply', 'screen', 'darken', 'lighten', 'overlay', 'color-dodge', 'color-burn', 'hard-light',
                         'soft-light', 'difference', 'exclusion', 'hue', 'saturation', 'color', 'luminosity'];
            var enums = new WeakMap();
            Object.defineProperty(SvgP, 'mode', { enumerable: true, configurable: true, get: function () {
                if (this.localName !== 'feBlend') return undefined;
                var e = enums.get(this);
                if (!e) { e = enumAttr(this, 'mode', BLEND, 'normal'); enums.set(this, e); }
                return e;
            } });
            def('getEnclosureList', function getEnclosureList(r, ref) { return listOf(this, r, ref, true); });
        })();
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
        return { x: /\b(x|inline|both|pair)\b/.test(type), y: /\b(y|block|both|pair)\b/.test(type), proximity: !/mandatory/.test(type) };
    }
    /* Each axis's snap positions, { t: the target, pos: the offset }. */
    function snapLists(el, axes) {
        /* (the root's scrollport is the viewport, which its box moves in) */
        var isRoot = el === el.ownerDocument.documentElement;
        var rz = el.currentCSSZoom || 1, r = el.getBoundingClientRect(), left = isRoot ? 0 : r.left + el.clientLeft * rz, top = isRoot ? 0 : r.top + el.clientTop * rz;
        /* (sizes zoomed: layout px, as the scroll offsets are) */
        var ez = el.currentCSSZoom || 1;
        var sx = el.scrollLeft, sy = el.scrollTop, cw = el.clientWidth * ez, ch = el.clientHeight * ez;
        var maxX = el.scrollWidth * ez - cw, maxY = el.scrollHeight * ez - ch, out = { x: [], y: [] };
        /* (the snapport is the scrollport less scroll-padding; a snap area is the box and its scroll-margin) */
        /* (lengths zoomed as layout has them) */
        var ecs = global.getComputedStyle(el), elPx = function (v, of, z) {
            var m = /^(-?[\d.]+)(px|%)$/.exec(v || '');
            return !m ? 0 : m[2] === '%' ? +m[1] * of / 100 : +m[1] * (z || 1);
        };
        var padL = elPx(ecs.scrollPaddingLeft, cw, ez), padR = elPx(ecs.scrollPaddingRight, cw, ez), padT = elPx(ecs.scrollPaddingTop, ch, ez), padB = elPx(ecs.scrollPaddingBottom, ch, ez);
        var at = function (align, lo, hi, view, pa, pb) {
            return align === 'start' ? lo - pa : align === 'end' ? hi - view + pb : (lo + hi - view - pa + pb) / 2 ;
        };
        /* (what the cascade says has a scroll-snap-align, in tree order; a
         * nested scroller's targets are its own) */
        var aligned = global.__cyberRenderElements(16);
        for (var i = 0; i < aligned.length; i++) {
            var c = aligned[i], up = flatParent(c);
            /* (out of flow: only what it's positioned in holds it) */
            var outOf = /^(absolute|fixed)$/.test(global.getComputedStyle(c).position);
            while (up && up !== el) {
                if (up.nodeType === 1) {
                    var ups = global.getComputedStyle(up);
                    if (!outOf || ups.position !== 'static') {
                        if (isScrollBox(ups)) break;
                        outOf = /^(absolute|fixed)$/.test(ups.position);
                    }
                }
                up = flatParent(up);
            }
            if (up !== el || !c.getClientRects().length) continue;
            var align = global.getComputedStyle(c).scrollSnapAlign;
            if (!align || align === 'none') continue;
            var parts = align.split(' '), block = parts[0], inline = parts[1] || parts[0], b = c.getBoundingClientRect();
            var tcs = global.getComputedStyle(c), tz = c.currentCSSZoom || 1;
            if (axes.x && inline !== 'none') {
                var xlo = b.left - left + sx - elPx(tcs.scrollMarginLeft, 0, tz), xhi = b.right - left + sx + elPx(tcs.scrollMarginRight, 0, tz);
                out.x.push({ t: c, pos: Math.max(0, Math.min(maxX, at(inline, xlo, xhi, cw, padL, padR))), lo: xlo - padL, hi: xhi + padR });
            }
            if (axes.y && block !== 'none') {
                var ylo = b.top - top + sy - elPx(tcs.scrollMarginTop, 0, tz), yhi = b.bottom - top + sy + elPx(tcs.scrollMarginBottom, 0, tz);
                out.y.push({ t: c, pos: Math.max(0, Math.min(maxY, at(block, ylo, yhi, ch, padT, padB))), lo: ylo - padT, hi: yhi + padB });
            }
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
    /* (a programmatic scroll lands on a snap position; in an area bigger than
     * the snapport, anywhere that it still fills is one) */
    var snapHeld = 0;
    Object.defineProperty(global, '__cyberSnapHold', { configurable: true, value: function (d) { snapHeld += d; } });
    function snapGoal(el, left, top) {
        if (snapHeld || !global.__cyberHasSnap || !global.__cyberHasSnap() || !el.isConnected) return null;
        var axes = snapAxes(global.getComputedStyle(el));
        if (!axes) return null;
        var lz = el.currentCSSZoom || 1, lists = snapLists(el, axes), cw = el.clientWidth * lz, ch = el.clientHeight * lz;
        var pick = function (list, cur, view) {
            for (var i = 0; i < list.length; i++) if (list[i].hi - list[i].lo > view && list[i].lo <= cur && list[i].hi >= cur + view) return cur;
            var best = snapNearest(list, cur);
            if (best !== null && axes.proximity && Math.abs(best - cur) > view / 3) return cur;
            return best === null ? cur : best;
        };
        var maxX = el.scrollWidth - cw, maxY = el.scrollHeight - ch;
        var l = left === undefined ? rawLeft.get.call(el) : Math.max(0, Math.min(maxX, left));
        var t = top === undefined ? rawTop.get.call(el) : Math.max(0, Math.min(maxY, top));
        return [axes.x ? pick(lists.x, l, cw) : left, axes.y ? pick(lists.y, t, ch) : top];
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
        var follow = function (list, targets, cur, range) {
            var t = snapPreferred(targets);
            for (var i = 0; t && i < list.length; i++) if (list[i].t === t) return list[i].pos;
            /* (snapped to nothing: a proximity scroller goes only to a position near it, as when it first snaps) */
            var near = snapNearest(list, cur);
            return near !== null && axes.proximity && Math.abs(near - cur) > range ? null : near;
        };
        var x = follow(lists.x, st.x, st.sx, el.clientWidth / 3), y = follow(lists.y, st.y, st.sy, el.clientHeight / 3);
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
    /* (scroll-target-group: auto: its anchors to elements, the one whose target
     * is nearest where its scroller is scrolled to :target-current) */
    function targetGroupUpdate(g) {
        if (global.getComputedStyle(g).scrollTargetGroup !== 'auto') return;
        var d = g.ownerDocument, as = g.querySelectorAll('a[href]'), pairs = [];
        for (var i = 0; i < as.length; i++) {
            var h = as[i].getAttribute('href'), k = h.indexOf('#');
            if (k < 0 || k === h.length - 1) continue;
            var id = h.slice(k + 1), t = d.getElementById(id);
            if (!t) { try { t = d.getElementById(decodeURIComponent(id)); } catch (e) { t = null; } }
            if (t) pairs.push(as[i], t);
        }
        var best = null, bestD = Infinity;
        for (var p = 0; p < pairs.length; p += 2) {
            var sc = scrollerSubject(scrollerAround(pairs[p + 1])), o = scrollOffsetOf(sc, pairs[p + 1]);
            var maxX = sc.scrollWidth - sc.clientWidth, maxY = sc.scrollHeight - sc.clientHeight;
            var dd = Math.abs(Math.max(0, Math.min(maxX, o.x)) - sc.scrollLeft) + Math.abs(Math.max(0, Math.min(maxY, o.y)) - sc.scrollTop);
            if (dd < bestD) { bestD = dd; best = pairs[p]; }
        }
        for (var q = 0; q < as.length; q++) global.__cyberElState(as[q], ST_TARGET_CURRENT, as[q] === best);
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
        if (features & 64) {
            var tgs = global.__cyberRenderElements(64);
            for (var t = 0; t < tgs.length; t++) targetGroupUpdate(tgs[t]);
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
    /* ---- View transitions, painted: the old state's named elements cloned
     * (computed styles inlined) before the update, the new state's after it,
     * and the ::view-transition pseudo tree built as a fixed overlay of
     * those clones, styled by the page's ::view-transition-* rules.  The
     * default animations are taken at their first frame while they run long
     * (or paused); a transition whose animations end soon shows nothing ---- */
    /* (auto and match-element: a name of the element's own; auto its id when it has one) */
    var VT_AUTO = new WeakMap(), vtAutoN = 0;
    /* (an identifier without its escapes) */
    var vtUnesc = function (s) {
        if (s.indexOf('\\') < 0) return s;
        return s.replace(/\\([0-9a-fA-F]{1,6})[ \t\n]?|\\([^])/g, function (m, h, c) { return h ? String.fromCodePoint(parseInt(h, 16) || 0xFFFD) : c; });
    };
    var vtName = function (el) {
        var n = '';
        try { n = String(global.getComputedStyle(el).getPropertyValue('view-transition-name') || '').trim(); }
        catch (e) { return ''; }
        if (!n || n === 'none') return '';
        if (n === 'auto' || n === 'match-element') {
            if (n === 'auto' && el.id) return '-cyber-vt-id-' + el.id;
            if (!VT_AUTO.has(el)) VT_AUTO.set(el, '-cyber-vt-' + (++vtAutoN));
            return VT_AUTO.get(el);
        }
        return vtUnesc(n);
    };
    var vtClasses = function (el) {
        try { var v = String(global.getComputedStyle(el).getPropertyValue('view-transition-class') || '').trim(); return v && v !== 'none' ? v.split(/\s+/) : []; }
        catch (e) { return []; }
    };
    var vtInline = function (from, to) {
        var cs = global.getComputedStyle(from), parts = [];
        for (var i = 0; i < cs.length; i++) {
            var p = cs[i];
            /* (physical properties only: logical ones and prefixed aliases say the same) */
            if (!p || p.indexOf('view-transition') >= 0 || /^(animation|transition|-webkit-|inset|contain-intrinsic)/.test(p) ||
                /(^|-)(block|inline)(-|$)/.test(p)) continue;
            var v = cs.getPropertyValue(p);
            if (v !== '' && v != null) parts.push(p + ':' + v);
        }
        /* (the used sizes last: nothing before them resets them) */
        ['width', 'height'].forEach(function (p) { var v = cs.getPropertyValue(p); if (v && v !== 'auto') parts.push(p + ':' + v); });
        to.setAttribute('style', parts.join(';'));
        to.removeAttribute('id');
    };
    /* (a deep clone of el as it looks now, named descendants left out) */
    var vtSnap = function (el, isRoot) {
        var c = el.cloneNode(true);
        var walk = function (a, b, top) {
            if (a.nodeType !== 1 || b.nodeType !== 1) return;
            vtInline(a, b);
            if (!top && vtName(a)) { b.style.setProperty('visibility', 'hidden'); }
            /* (a fixed descendant: where it is on screen, from the nearest positioned box of the clone) */
            var apos = !top && !isRoot ? global.getComputedStyle(a).position : '';
            if (apos === 'fixed' || apos === 'absolute') {
                var cb = a.parentNode;
                while (cb && cb !== el && cb.nodeType === 1 && global.getComputedStyle(cb).position === 'static') cb = cb.parentNode;
                /* (an absolute one: only when its containing block is outside, the clone standing in for it) */
                if (apos === 'absolute' && !(cb === el && global.getComputedStyle(el).position === 'static')) cb = null;
                if (cb && cb.nodeType === 1) {
                    var ar = a.getBoundingClientRect(), cr = cb.getBoundingClientRect();
                    [['position', 'absolute'], ['left', (ar.left - cr.left - (cb.clientLeft || 0)) + 'px'], ['top', (ar.top - cr.top - (cb.clientTop || 0)) + 'px'],
                     ['right', 'auto'], ['bottom', 'auto'], ['margin', '0']].forEach(function (d) { b.style.setProperty(d[0], d[1]); });
                }
            }
            var ak = a.childNodes, bk = b.childNodes;
            for (var i = 0; i < ak.length && i < bk.length; i++) walk(ak[i], bk[i], false);
        };
        walk(el, c, true);
        ['position:absolute', 'left:0', 'top:0', 'right:auto', 'bottom:auto', 'margin:0', 'transform:none', 'translate:none',
         'rotate:none', 'scale:none', 'float:none', 'visibility:visible'].forEach(function (d) {
            var k = d.indexOf(':');
            c.style.setProperty(d.slice(0, k), d.slice(k + 1));
        });
        return c;
    };
    /* (the page as the root's image: the canvas color and a clone of the body where it is) */
    var vtRootSnap = function (doc) {
        var b = doc.body;
        if (!b) return null;
        var bg = global.getComputedStyle(doc.documentElement).backgroundColor;
        if (!bg || /rgba\(0, 0, 0, 0\)|transparent/.test(bg)) bg = global.getComputedStyle(b).backgroundColor;
        if (!bg || /rgba\(0, 0, 0, 0\)|transparent/.test(bg)) bg = 'white';
        var r = b.getBoundingClientRect();
        var bcs = global.getComputedStyle(b);
        return { bg: bg, x: r.left, y: r.top, snap: vtSnap(b, true), margin: [bcs.marginTop, bcs.marginRight, bcs.marginBottom, bcs.marginLeft].join(' ') };
    };
    var vtCaptureState = function (doc, scope) {
        scope = scope || doc.documentElement;
        var out = [], seen = {};
        /* (its descendants in tree order, into shadow trees too) */
        var els = [];
        (function walk(n) {
            for (var c = n.firstElementChild; c; c = c.nextElementSibling) {
                els.push(c);
                if (c.shadowRoot) walk(c.shadowRoot);
                walk(c);
            }
        })(scope);
        for (var i = 0; i < els.length; i++) {
            var el = els[i];
            if (el === scope || el.__cyberVtOverlay) continue;
            var rn = el.getRootNode ? el.getRootNode() : null;
            if (rn && rn.host && rn !== scope.getRootNode() && !el.hasAttribute('part')) continue;
            var n = vtName(el);
            if (!n || seen[n]) continue;
            var cs = global.getComputedStyle(el);
            if (cs.display === 'none' || cs.display === 'contents') continue;
            seen[n] = true;
            var r = el.getBoundingClientRect();
            out.push({ name: n, el: el, cls: vtClasses(el), x: r.left, y: r.top, w: r.width, h: r.height, snap: vtSnap(el) });
        }
        return out;
    };
    /* (the page's declarations for one pseudo: the * rules first, then the named) */
    var vtRules = function (doc, scope) {
        scope = scope || doc.documentElement;
        var list = [];
        var walk = function (rules) {
            for (var i = 0; i < rules.length; i++) {
                var r = rules[i];
                if (r.cssRules && !r.selectorText) {
                    var k = r.__kind;
                    if (k === 'media' && !(r.media && global.matchMedia(r.media.mediaText).matches)) continue;
                    if (k === 'supports' && !(global.CSS && global.CSS.supports(r.conditionText))) continue;
                    walk(r.cssRules);
                    continue;
                }
                var sel = String(r.selectorText || '');
                if (sel.indexOf('::view-transition') < 0 || !r.style) continue;
                sel.split(',').forEach(function (one) {
                    var m = /::view-transition(?:-(group-children|group|image-pair|old|new))?(?:\(\s*([^)]*)\s*\))?\s*$/.exec(one.trim());
                    if (!m) return;
                    var pre = one.trim().slice(0, m.index).trim();
                    /* (:active-view-transition(-type()) as the attribute that stands for them) */
                    if (pre) pre = pre.replace(/:active-view-transition-type\(([^)]*)\)/gi, function (m, args) {
                        return ':is(' + args.split(',').map(function (t) { return '[data-cyber-avt~="' + t.trim() + '"]'; }).join(',') + ')';
                    }).replace(/:active-view-transition(?![-\w])/gi, '[data-cyber-avt]');
                    if (pre) { var okp = false; try { okp = scope.matches(pre); } catch (e) { okp = false; } if (!okp) return; }
                    list.push({ kind: m[1] || 'root', name: (m[2] || '*').trim(), style: r.style });
                });
            }
        };
        /* (the sheets of the scope's tree: a shadow root's own and adopted ones too) */
        var tr = scope.getRootNode ? scope.getRootNode() : doc, sheets = [];
        var addAll = function (l) { if (l) for (var i = 0; i < l.length; i++) sheets.push(l[i]); };
        addAll(tr && tr.styleSheets ? tr.styleSheets : doc.styleSheets);
        try { addAll(tr && tr.adoptedStyleSheets); } catch (e) { }
        for (var i = 0; i < sheets.length; i++) { try { walk(sheets[i].cssRules); } catch (e) { } }
        return list;
    };
    var vtDecls = function (rules, kind, name, cls) {
        var d = {};
        var take = function (r) {
            for (var i = 0; i < r.style.length; i++) {
                var p = r.style[i];
                d[p] = r.style.getPropertyValue(p);
            }
        };
        /* (the argument: a name or *, then .classes; * first, then classes, then names) */
        var parse = function (a) {
            var m = /^([^.]*)((?:\.[^.]+)*)$/.exec(a) || ['', a, ''];
            return { n: vtUnesc(m[1] || '*'), u: (m[1] || '*') === '*', c: m[2] ? m[2].slice(1).split('.').map(vtUnesc) : [] };
        };
        var hit = function (r, level) {
            if (r.kind !== kind) return false;
            var p = parse(r.name);
            if (!p.u && p.n !== name) return false;
            for (var i = 0; i < p.c.length; i++) if (!cls || cls.indexOf(p.c[i]) < 0) return false;
            var lv = !p.u || p.c.length ? 1 : 0;
            return lv === level;
        };
        [0, 1].forEach(function (lv) { rules.forEach(function (r) { if (hit(r, lv)) take(r); }); });
        return d;
    };
    var vtSecs = function (v) {
        v = String(v || '').trim().split(/[\s,]+/)[0];
        var n = parseFloat(v);
        if (!isFinite(n)) return null;
        return /ms$/.test(v) ? n / 1000 : n;
    };
    /* (its animation: 'none' (none or unset), else its duration and play state) */
    var vtAnim = function (d, inheritDur) {
        var a = String(d['animation'] || '').trim(), nm = String(d['animation-name'] || '').trim();
        if (/^(unset|none|initial|revert|revert-layer)$/.test(a) || /^(unset|none|initial)$/.test(nm)) return { none: true };
        var dur = d['animation-duration'] != null ? vtSecs(d['animation-duration']) : null;
        if (dur == null && a) { var mm = /(-?[\d.]+m?s)/.exec(a); if (mm) dur = vtSecs(mm[1]); }
        if (dur == null) dur = inheritDur;
        /* (a long delay: its start state held, as long as a long animation) */
        var del = d['animation-delay'] != null ? vtSecs(d['animation-delay']) : null;
        if (del == null && a) { var tm = a.match(/-?[\d.]+m?s\b/g); if (tm && tm.length > 1) del = vtSecs(tm[1]); }
        if (del != null && del >= 2) dur = Math.max(dur, del);
        var paused = /paused/.test(String(d['animation-play-state'] || '') + ' ' + a);
        /* (where it is at its start: steps(n, start) has jumped once) */
        var tf = String(d['animation-timing-function'] || '') + ' ' + a, p0 = 0;
        var st = /steps\(\s*(\d+)\s*,\s*(start|jump-start|jump-both)\s*\)/.exec(tf);
        if (st) p0 = (1 / (+st[1] + (st[2] === 'jump-both' ? 1 : 0)));
        else if (/step-start/.test(tf)) p0 = 1;
        return { none: false, dur: dur, long: paused || dur >= 2, custom: !!nm && nm !== 'none', p0: p0 };
    };
    var VT_SKIP = /^(animation|transition)/;
    var vtApply = function (div, d) {
        for (var p in d) if (!VT_SKIP.test(p)) {
            try { div.style.setProperty(p, d[p]); } catch (e) { }
            /* (a shorthand's CSS-wide keyword: its longhands too) */
            if (p === 'background' && /^\s*(inherit|initial|unset)\s*$/.test(d[p]))
                ['background-color', 'background-image'].forEach(function (q) { try { div.style.setProperty(q, d[p].trim()); } catch (e) { } });
        }
    };
    /* (the root's view-transition-name: root unless a rule or its style says none) */
    var vtRootNamed = function (doc) {
        var n = vtName(doc.documentElement);
        if (n) return true;
        var none = false;
        var walk = function (rules) {
            for (var i = 0; i < rules.length; i++) {
                var r = rules[i];
                if (r.cssRules && !r.selectorText) { walk(r.cssRules); continue; }
                var sel = String(r.selectorText || '').trim();
                if (!/^(:root|html)$/i.test(sel) || !r.style) continue;
                var v = String(r.style.getPropertyValue('view-transition-name') || '').trim();
                if (v) none = v === 'none';
            }
        };
        var sh = doc.styleSheets;
        for (var i = 0; i < sh.length; i++) { try { walk(sh[i].cssRules); } catch (e) { } }
        var inl = doc.documentElement.style.getPropertyValue('view-transition-name');
        if (inl) none = String(inl).trim() === 'none';
        return !none;
    };
    var vtShow = function (doc, old, onDone, scope) {
        scope = scope || doc.documentElement;
        var scoped = scope !== doc.documentElement;
        var nu = vtCaptureState(doc, scope), rules = vtRules(doc, scope);
        var byName = {}, order = [];
        /* (the old capture's names first, in its paint order, the new ones after: the groups' order, CSS View Transitions 1) */
        old.forEach(function (o) { byName[o.name] = { nu: null, old: o }; order.push(o.name); });
        nu.forEach(function (n) { if (!byName[n.name]) { byName[n.name] = { nu: n, old: null }; order.push(n.name); } else byName[n.name].nu = n; });
        /* (the animations: the group's duration the others inherit) */
        var hold = false, info = {};
        /* (the root takes part unless its view-transition-name is none) */
        var rootIn = scoped ? !!vtName(scope) : vtRootNamed(doc);
        /* (::view-transition itself: an animation of its own keeps it up) */
        var va = vtAnim(vtDecls(rules, 'root', '*'), 0.25);
        if (!va.none && va.long && (va.custom || va.dur >= 2)) hold = true;
        if (!rootIn && !order.length && !hold) { onDone(); return null; }
        /* (the root's rules by its own view-transition-name) */
        var rootNm = (scoped ? '' : vtName(doc.documentElement)) || 'root';
        order.concat(order.indexOf('root') < 0 ? ['root'] : []).forEach(function (n) {
            var e = byName[n] || { nu: null, old: null };
            var cl = (e.nu || e.old || {}).cls || [], ln = n === 'root' ? rootNm : n;
            var g = vtDecls(rules, 'group', ln, cl), ip = vtDecls(rules, 'image-pair', ln, cl), od = vtDecls(rules, 'old', ln, cl), nd = vtDecls(rules, 'new', ln, cl);
            var ga = vtAnim(g, 0.25), gd = ga.none ? 0.25 : ga.dur;
            var oa = vtAnim(od, gd), na = vtAnim(nd, gd);
            var both = (n === 'root' && rootIn) || (e.nu && e.old);
            if (!both) ga = { none: true };
            if ((n !== 'root' || !rootIn) && !e.old) oa = { none: true };
            if ((n !== 'root' || !rootIn) && !e.nu) na = { none: true };
            [ga, oa, na].forEach(function (a) { if (!a.none && a.long) hold = true; });
            info[n] = { g: g, ip: ip, od: od, nd: nd, ga: ga, oa: oa, na: na };
        });
        var brief = !hold;
        if (brief) for (var bn in info) [info[bn].ga, info[bn].oa, info[bn].na].forEach(function (a) { if (!a.none && a.dur !== 0) a.long = true; });   /* (a 0s one: at its end already) */
        /* (nested groups, view-transition-group: each name's parent group, by its
         * element's value: nearest (the closest named ancestor), a name (the
         * closest ancestor of that name), normal (the closest named ancestor
         * when that one contains: contain, or chosen by another) */
        var parentOf = {}, elOf = {}, gval = {};
        order.forEach(function (n) { var e = byName[n]; elOf[n] = (e.nu || e.old).el; });
        var vtGroupVal = function (el) {
            try { var v = String(global.getComputedStyle(el).getPropertyValue('view-transition-group') || '').trim(); return v || 'normal'; }
            catch (e) { return 'normal'; }
        };
        var namedAnc = function (el) {
            var out = [];
            for (var a = el && (el.parentElement || (el.parentNode && el.parentNode.host)); a; a = a.parentElement || (a.parentNode && a.parentNode.host)) {
                var an = vtName(a);
                if (an && byName[an] && elOf[an] === a) out.push(an);
            }
            return out;
        };
        var contains = {};
        order.forEach(function (n) { gval[n] = vtGroupVal(elOf[n]); if (gval[n] === 'contain') contains[n] = true; });
        order.forEach(function (n) {
            var anc = namedAnc(elOf[n]), v = gval[n];
            if (v === 'nearest' && anc.length) { parentOf[n] = anc[0]; contains[anc[0]] = true; }
            else if (v !== 'normal' && v !== 'contain' && v !== 'nearest') { if (anc.indexOf(v) >= 0) { parentOf[n] = v; contains[v] = true; } }
        });
        order.forEach(function (n) {
            if (parentOf[n] || (gval[n] !== 'normal' && gval[n] !== 'contain')) return;
            var anc = namedAnc(elOf[n]);
            for (var i = 0; i < anc.length; i++) if (contains[anc[i]]) { parentOf[n] = anc[i]; break; }
        });
        var gdivs = {};
        /* (the overlay's elements by their pseudo-elements: what animate() with pseudoElement drives) */
        var vtEls = {}, vtAnims = [];
        Object.defineProperty(global, '__cyberVtHold', { configurable: true, value: function (a) { vtAnims.push(a); } });
        Object.defineProperty(global, '__cyberVtEl', { configurable: true, value: function (p) {
            var m = /^::view-transition-(group|image-pair|old|new)\(\s*([^)\s]*)\s*\)$/.exec(String(p || ''));
            return m ? vtEls[m[1] + ':' + m[2]] || null : null;
        } });
        var root = doc.documentElement;
        var ov = doc.createElement('div');
        Object.defineProperty(ov, '__cyberVtOverlay', { value: true });
        /* (a scoped one over its element's padding box; its groups placed from there) */
        var sr = { left: 0, top: 0 };
        if (scoped) {
            var sb = scope.getBoundingClientRect(), scs = getComputedStyle(scope);
            var bl = parseFloat(scs.borderLeftWidth) || 0, bt = parseFloat(scs.borderTopWidth) || 0;
            var br = parseFloat(scs.borderRightWidth) || 0, bb = parseFloat(scs.borderBottomWidth) || 0;
            sr = { left: sb.left + bl, top: sb.top + bt, width: Math.max(0, sb.width - bl - br), height: Math.max(0, sb.height - bt - bb) };
        }
        ov.setAttribute('style', 'position:fixed;z-index:2147483647;pointer-events:none;' +
                                 (scoped ? 'left:' + sr.left + 'px;top:' + sr.top + 'px;width:' + sr.width + 'px;height:' + sr.height + 'px;'
                                         : 'left:0;top:0;width:100vw;height:100vh;') +
                                 'margin:0;padding:0;border:0;visibility:visible;display:block;overflow:visible');
        vtApply(ov, vtDecls(rules, 'root', '*'));
        /* (the root's new image: the live page under the overlay; hidden when its group or new is) */
        var ri = info.root, hidden = [];
        var rootOp = function (d, a, start) {
            if (d.display === 'none') return 0;
            var o = d.opacity != null && d.opacity !== '' ? parseFloat(d.opacity) : 1;
            if (!a.none && a.long && !a.custom && start !== undefined) o = start;
            return o;
        };
        var gOp = rootOp(ri.g, ri.ga), oOp = rootOp(ri.od, ri.oa, 1), nOp = rootOp(ri.nd, ri.na, 0);
        /* (the old page drawn only when the page's rules set the root's images still; mid crossfade the live page stands in) */
        var oldRoot = rootIn && old.root && ((ri.oa.none && ri.na.none) || (ri.oa.custom && ri.oa.long && oOp >= 0.99)) && gOp * oOp > 0.01 && (ri.od.visibility || ri.ip.visibility || ri.g.visibility) !== 'hidden';
        var rootShown = !rootIn || gOp * nOp > 0.01 || (!oldRoot && gOp * oOp > 0.01);
        if (oldRoot) {
            /* (the old page, as captured, under the groups) */
            var rg = doc.createElement('div');
            rg.setAttribute('style', 'position:absolute;left:0;top:0;width:100vw;height:100vh;overflow:hidden;background:' + old.root.bg + ';opacity:' + (gOp * oOp));
            var bs = old.root.snap.cloneNode(true);
            /* (in flow, with its margins: they collapse as on the page) */
            bs.style.setProperty('position', 'static');
            bs.style.setProperty('margin', old.root.margin);
            rg.appendChild(bs);
            ov.appendChild(rg);
        }
        if (!rootShown) {
            for (var c = scoped ? scope.firstChild : doc.body ? doc.body.firstChild : null; c; c = c.nextSibling)
                if (c.nodeType === 1) { hidden.push([c, c.style.getPropertyValue('visibility'), c.style.getPropertyPriority('visibility')]); c.style.setProperty('visibility', 'hidden', 'important'); }
        }
        order.forEach(function (n) {
            var e = byName[n], f = info[n];
            /* (the live element: painted in its group instead) */
            if (e.nu && rootShown) { hidden.push([e.nu.el, e.nu.el.style.getPropertyValue('visibility'), e.nu.el.style.getPropertyPriority('visibility')]); e.nu.el.style.setProperty('visibility', 'hidden', 'important'); }
            var start = !f.ga.none && f.ga.long && !f.ga.custom;
            var geo = start && e.old ? e.old : (e.nu || e.old);
            if (start && e.old && e.nu && f.ga.p0 > 0) {
                /* (its first frame part of the way: the geometry between the old and the new) */
                var q = f.ga.p0, mix = function (a, b) { return a + (b - a) * q; };
                geo = { x: mix(e.old.x, e.nu.x), y: mix(e.old.y, e.nu.y), w: mix(e.old.w, e.nu.w), h: mix(e.old.h, e.nu.h) };
            }
            var g = doc.createElement('div');
            g.setAttribute('style', 'position:absolute;left:0;top:0;margin:0;padding:0;border:0;box-sizing:content-box;width:' + geo.w + 'px;height:' + geo.h +
                                    'px;transform:translate(' + (geo.x - sr.left) + 'px,' + (geo.y - sr.top) + 'px);transform-origin:0 0');
            vtApply(g, f.g);
            /* (the group's width after its rules: what the images span) */
            var gw = /px\s*$/.test(g.style.width) ? parseFloat(g.style.width) : geo.w;
            if (!(gw >= 0)) gw = geo.w;
            var ip = doc.createElement('div');
            ip.setAttribute('style', 'position:absolute;left:0;top:0;width:100%;height:100%');
            vtApply(ip, f.ip);
            g.appendChild(ip);
            vtEls['group:' + n] = g;
            vtEls['image-pair:' + n] = ip;
            var put = function (s, d, a, op0, kind) {
                if (!s) return;
                var w = doc.createElement('div');
                vtEls[kind + ':' + n] = w;
                w.setAttribute('style', 'position:absolute;left:0;top:0;width:100%;height:100%;overflow:visible');
                var o = d.opacity != null && d.opacity !== '' ? parseFloat(d.opacity) : 1;
                if (!a.none && a.long && !a.custom) o = op0;
                vtApply(w, d);
                w.style.setProperty('opacity', String(o));
                var snap = s.snap.cloneNode(true), sc = s.w > 0 ? gw / s.w : 1;
                /* (hidden with its group, image pair or itself) */
                var hid = [f.g, f.ip, d].some(function (x) { return x && String(x.visibility || '').trim() === 'hidden'; });
                if (hid) snap.style.setProperty('visibility', 'hidden');
                var fit = d['object-fit'] ? String(d['object-fit']).trim() : '';
                if (fit === 'none' && s.w > 0) {
                    /* (its own size, centred in the box its width gives it) */
                    var bh = s.h * gw / s.w, opx = [0.5, 0], opy = [0.5, 0];
                    var ops = d['object-position'] ? String(d['object-position']).trim().split(/\s+/) : [];
                    var opv = function (t, hz) {
                        if (t === 'left' || t === 'top') return [0, 0];
                        if (t === 'right' || t === 'bottom') return [1, 0];
                        if (t === 'center') return [0.5, 0];
                        if (/%$/.test(t)) return [parseFloat(t) / 100, 0];
                        var v = parseFloat(t);
                        return isNaN(v) ? [0.5, 0] : [0, v];
                    };
                    if (ops.length >= 1) opx = opv(ops[0]);
                    if (ops.length >= 2) opy = opv(ops[1]);
                    snap.style.setProperty('transform', 'translate(' + (opx[0] * (gw - s.w) + opx[1]) + 'px,' + (opy[0] * (bh - s.h) + opy[1]) + 'px)');
                } else if (Math.abs(sc - 1) > 0.001) { snap.style.setProperty('transform', 'scale(' + sc + ')'); snap.style.setProperty('transform-origin', '0 0'); }
                w.appendChild(snap);
                ip.appendChild(w);
            };
            put(e.old, f.od, f.oa, 1, 'old');
            put(e.nu, f.nd, f.na, 0, 'new');
            gdivs[n] = { g: g, geo: geo, kids: null };
            var pn = parentOf[n], pg = pn ? gdivs[pn] : null;
            if (pg) {
                /* (inside its parent group's ::view-transition-group-children, placed from that group) */
                if (!pg.kids) {
                    pg.kids = doc.createElement('div');
                    pg.kids.setAttribute('style', 'position:absolute;left:0;top:0;width:100%;height:100%');
                    vtApply(pg.kids, vtDecls(rules, 'group-children', pn, (byName[pn].nu || byName[pn].old).cls || []));
                    pg.g.appendChild(pg.kids);
                }
                g.style.setProperty('transform', 'translate(' + (geo.x - pg.geo.x) + 'px,' + (geo.y - pg.geo.y) + 'px)');
                pg.kids.appendChild(g);
            } else ov.appendChild(g);
        });
        /* (in a closed shadow tree of its own: the page's rules don't reach it) */
        var host = doc.createElement('cyber-view-transition');
        Object.defineProperty(host, '__cyberVtOverlay', { value: true });
        host.setAttribute('style', 'display:contents;visibility:visible');
        var hostOk = false;
        try { host.attachShadow({ mode: 'closed' }).appendChild(ov); hostOk = true; } catch (e) { hostOk = false; }
        root.appendChild(hostOk ? host : ov);
        var undo = function () {
            if (host.parentNode) host.parentNode.removeChild(host);
            if (ov.parentNode) ov.parentNode.removeChild(ov);
            hidden.forEach(function (h) { if (h[1]) h[0].style.setProperty('visibility', h[1], h[2]); else h[0].style.removeProperty('visibility'); });
            hidden = [];
        };
        /* (while animations started on its pseudo-elements run: held) */
        var vtLive = function () { return vtAnims.some(function (a) { try { return a.playState === 'running' || a.playState === 'paused'; } catch (e) { return false; } }); };
        if (brief) { var bleft = 6, btick = function () { if (vtLive()) { global.requestAnimationFrame(btick); return; } if (--bleft > 0) global.requestAnimationFrame(btick); else { undo(); onDone(); } }; global.requestAnimationFrame(btick); }
        return undo;
    };
    var VT_ACTIVE = { cur: null };
    var newViewTransition = function (update, types, doc, root) {
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
        /* (the old state, captured now: painted once the update is done) */
        var vtOld = null, vtUndo = null;
        /* (the root matches :active-view-transition(-type) while it runs: its types in an attribute) */
        var avtSet = function (list) { if (doc && doc.documentElement) doc.documentElement.setAttribute('data-cyber-avt', list.filter(function (t) { return !/^(none$|-ua-)/i.test(t); }).join(' ')); };
        if (doc) { var tl = []; try { if (types) for (var ti of types) tl.push(String(ti)); } catch (e) { } avtSet(tl); }
        var vdoc = root && root.ownerDocument;
        var vtCapOld = function () {
            if (skipped) return;
            if (vdoc && (!doc || root === doc.documentElement)) {
                try { vtOld = vtCaptureState(vdoc, root); } catch (e) { vtOld = null; }
                if (vtOld && doc) { try { vtOld.root = vtRootSnap(doc); } catch (e) { vtOld.root = null; } }
            }
        };
        Promise.resolve().then(function () { vtCapOld(); return update(); }).then(function (v) {
            global.clearTimeout(timer);
            if (timedOut) return;
            resolveDone(v);
            if (skipped) rejectReady(new global.DOMException('Transition was skipped', 'AbortError'));
            else {
                resolveReady();
                if (doc) global.setTimeout(function () { if (!skipped) viewTransitionEvents(doc); }, 0);
            }
            if (!skipped && vtOld) {
                var fin = false, finish = function () { if (!fin) { fin = true; resolveFinished(); } };
                try { vtUndo = vtShow(vdoc, vtOld, finish, root); } catch (e) { finish(); }
            } else resolveFinished();
        }, function (reason) {
            global.clearTimeout(timer);
            if (timedOut) return;
            rejectDone(reason);
            rejectReady(reason);
            rejectFinished(reason);
        });
        VT_STATE.set(vt, { done: done, ready: ready, finished: finished, types: newTypeSet(types),
                           root: root || (doc && doc.documentElement) || null,
                           skip: function () { skipped = true; if (vtUndo) { vtUndo(); vtUndo = null; resolveFinished(); } } });
        Object.defineProperty(vt, '__skipped', { get: function () { return skipped; } });
        if (doc) {
            var ts = VT_STATE.get(vt).types;
            try {
                Object.defineProperty(ts, '__onChange', { configurable: true, value: function () {
                    var a = []; ts.forEach(function (t) { a.push(t); }); if (VT_ACTIVE.cur === vt) avtSet(a);
                } });
            } catch (e) { }
            VT_ACTIVE.cur = vt;
            var off = function () { if (VT_ACTIVE.cur === vt) { VT_ACTIVE.cur = null; if (doc.documentElement) doc.documentElement.removeAttribute('data-cyber-avt'); } };
            finished.then(off, off);
        }
        return vt;
    };
    /* (ViewTransition's attributes and operations, on its prototype) */
    var VT_STATE = new WeakMap();
    var vtOf = function (o) {
        var s = o && typeof o === 'object' ? VT_STATE.get(o) : undefined;
        if (!s) throw new TypeError('Illegal invocation');
        return s;
    };
    [['updateCallbackDone', 'done'], ['ready', 'ready'], ['finished', 'finished'], ['types', 'types'], ['transitionRoot', 'root']].forEach(function (p) {
        var get = { get: function () { return vtOf(this)[p[1]]; } };
        var g = Object.getOwnPropertyDescriptor(get, 'get').value;
        Object.defineProperty(g, 'name', { configurable: true, value: 'get ' + p[0] });
        Object.defineProperty(ViewTransition.prototype, p[0], { configurable: true, enumerable: true, get: g });
    });
    Object.defineProperty(ViewTransition.prototype, 'skipTransition', { configurable: true, enumerable: true, writable: true,
        value: function skipTransition() { vtOf(this).skip(); } });
    Object.defineProperty(ViewTransition.prototype, 'waitUntil', { configurable: true, enumerable: true, writable: true,
        value: function waitUntil(p) {
            vtOf(this);
            if (arguments.length < 1) throw new TypeError("Failed to execute 'waitUntil' on 'ViewTransition': 1 argument required, but only 0 present.");
            return undefined;
        } });
    /* (ViewTransitionTypeSet: a setlike of strings) */
    var VTTS_SET = new WeakMap();
    var ViewTransitionTypeSet = function ViewTransitionTypeSet() { throw new TypeError('Illegal constructor'); };
    var ttsOf = function (o) {
        var s = o && typeof o === 'object' ? VTTS_SET.get(o) : undefined;
        if (!s) throw new TypeError('Illegal invocation');
        return s;
    };
    (function (P) {
        var m = function (name, len, fn) {
            Object.defineProperty(fn, 'name', { configurable: true, value: name });
            Object.defineProperty(fn, 'length', { configurable: true, value: len });
            Object.defineProperty(P, name, { configurable: true, enumerable: true, writable: true, value: fn });
        };
        Object.defineProperty(P, 'size', { configurable: true, enumerable: true, get: function size() { return ttsOf(this).size; } });
        m('has', 1, function (v) { return ttsOf(this).has(String(v)); });
        m('add', 1, function (v) { ttsOf(this).add(String(v)); if (this.__onChange) this.__onChange(); return this; });
        m('delete', 1, function (v) { var r = ttsOf(this).delete(String(v)); if (this.__onChange) this.__onChange(); return r; });
        m('clear', 0, function () { ttsOf(this).clear(); if (this.__onChange) this.__onChange(); });
        m('forEach', 1, function (cb, thisArg) { var self = this; ttsOf(this).forEach(function (v) { cb.call(thisArg, v, v, self); }); });
        m('entries', 0, function () { return ttsOf(this).entries(); });
        m('values', 0, function () { return ttsOf(this).values(); });
        Object.defineProperty(P, 'keys', { configurable: true, enumerable: true, writable: true, value: P.values });
        Object.defineProperty(P, Symbol.iterator, { configurable: true, writable: true, value: P.values });
        Object.defineProperty(P, Symbol.toStringTag, { configurable: true, value: 'ViewTransitionTypeSet' });
    })(ViewTransitionTypeSet.prototype);
    var newTypeSet = function (types) {
        var o = Object.create(ViewTransitionTypeSet.prototype);
        VTTS_SET.set(o, new Set(Array.from(types || [], String)));
        return o;
    };
    if (global.document && !global.Document.prototype.startViewTransition) {
        expose('ViewTransition', ViewTransition);
        expose('ViewTransitionTypeSet', ViewTransitionTypeSet);
        /* (a document's, or an element's for a transition scoped to it) */
        var startOn = function (self, arg, doc, root) {
            var update = typeof arg === 'function' ? arg : arg && arg.update, types = arg && typeof arg === 'object' ? arg.types : null;
            /* (a new one skips the one running) */
            if (self.__cyberViewTransition) self.__cyberViewTransition.skipTransition();
            var vt = newViewTransition(function () { return update ? update() : undefined; }, types, doc, root);
            Object.defineProperty(self, '__cyberViewTransition', { value: vt, configurable: true, writable: true });
            return vt;
        };
        Object.defineProperty(global.Document.prototype, 'startViewTransition', { configurable: true, enumerable: true, writable: true,
            value: function startViewTransition() {
                if (!(this instanceof global.Document)) throw new TypeError('Illegal invocation');
                return startOn(this, arguments[0], this, this.documentElement);
            } });
        if (global.Element) Object.defineProperty(global.Element.prototype, 'startViewTransition', { configurable: true, enumerable: true, writable: true,
            value: function startViewTransition() {
                if (!(this instanceof global.Element)) throw new TypeError('Illegal invocation');
                return startOn(this, arguments[0], null, this);
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

    /* ---- navigator.preferences (Web Preferences API): overrides of the user's
     * preference media features ---- */
    if (global.navigator && global.EventTarget && !('preferences' in global.navigator)) (function () {
        var PREFS = [
            ['colorScheme', 'prefers-color-scheme', ['light', 'dark']],
            ['contrast', 'prefers-contrast', ['more', 'less', 'no-preference']],
            ['reducedMotion', 'prefers-reduced-motion', ['reduce', 'no-preference']],
            ['reducedTransparency', 'prefers-reduced-transparency', ['reduce', 'no-preference']],
            ['reducedData', 'prefers-reduced-data', ['reduce', 'no-preference']],
        ];
        var STATE = new WeakMap();
        var st = function (o) { var s = STATE.get(o); if (!s) throw new TypeError('Illegal invocation'); return s; };
        var PreferenceObject = function PreferenceObject() { throw new TypeError('Illegal constructor'); };
        PreferenceObject.prototype = Object.create(global.EventTarget.prototype);
        Object.defineProperty(PreferenceObject.prototype, 'constructor', { value: PreferenceObject, writable: true, configurable: true });
        Object.defineProperty(PreferenceObject.prototype, Symbol.toStringTag, { value: 'PreferenceObject', configurable: true });
        /* (at once: each request is told before the next one's handler is in place) */
        var fire = function (o) { o.dispatchEvent(new global.Event('change')); };
        var set = function (o, v) {
            var s = st(o);
            s.override = v;
            if (typeof global.__cyberMediaPref === 'function') global.__cyberMediaPref(s.feature, v);
            fire(o);
        };
        Object.defineProperty(PreferenceObject.prototype, 'override', { configurable: true, enumerable: true, get: function () { return st(this).override; } });
        Object.defineProperty(PreferenceObject.prototype, 'value', { configurable: true, enumerable: true, get: function () {
            var s = st(this);
            if (s.override !== null) return s.override;
            for (var i = 0; i < s.valid.length; i++)
                if (global.matchMedia('(' + s.feature + ': ' + s.valid[i] + ')').matches) return s.valid[i];
            return s.valid[s.valid.length - 1];
        } });
        Object.defineProperty(PreferenceObject.prototype, 'validValues', { configurable: true, enumerable: true, get: function () { return st(this).valid.slice(); } });
        Object.defineProperty(PreferenceObject.prototype, 'requestOverride', { configurable: true, enumerable: true, writable: true,
            value: function requestOverride(v) {
                var self = this, s;
                try { s = st(this); } catch (e) { return Promise.reject(e); }
                if (v === null || v === undefined || v === '') { set(self, null); return Promise.resolve(); }
                v = String(v);
                if (s.valid.indexOf(v) < 0) return Promise.reject(new global.DOMException('Not a valid value for this preference: ' + v, 'TypeError'));
                set(self, v);
                return Promise.resolve();
            } });
        Object.defineProperty(PreferenceObject.prototype, 'clearOverride', { configurable: true, enumerable: true, writable: true,
            value: function clearOverride() { set(this, null); } });
        var handlers = new WeakMap();
        Object.defineProperty(PreferenceObject.prototype, 'onchange', { configurable: true, enumerable: true,
            get: function () { st(this); return handlers.get(this) || null; },
            set: function (f) {
                st(this);
                if (typeof f === 'function') handlers.set(this, f); else handlers.delete(this);
            } });
        var PreferenceManager = function PreferenceManager() { throw new TypeError('Illegal constructor'); };
        Object.defineProperty(PreferenceManager.prototype, Symbol.toStringTag, { value: 'PreferenceManager', configurable: true });
        var manager = Object.create(PreferenceManager.prototype);
        /* (made when first asked for: an EventTarget can't be made this early) */
        PREFS.forEach(function (p) {
            var o = null;
            Object.defineProperty(PreferenceManager.prototype, p[0], { configurable: true, enumerable: true, get: function () {
                if (!o) {
                    o = new global.EventTarget();
                    Object.setPrototypeOf(o, PreferenceObject.prototype);
                    STATE.set(o, { feature: p[1], valid: p[2], override: null });
                }
                return o;
            } });
        });
        Object.defineProperty(global, 'PreferenceManager', { value: PreferenceManager, writable: true, configurable: true });
        Object.defineProperty(global, 'PreferenceObject', { value: PreferenceObject, writable: true, configurable: true });
        var NP = Object.getPrototypeOf(global.navigator);
        Object.defineProperty(NP && NP !== Object.prototype ? NP : global.navigator, 'preferences', { configurable: true, enumerable: true,
            get: function () { return manager; } });
    })();

    /* (geometry's legacy window aliases: SVGPoint, SVGRect, SVGMatrix) */
    if (global.document && global.DOMPoint && !global.SVGPoint) {
        [['SVGPoint', 'DOMPoint'], ['SVGRect', 'DOMRect'], ['SVGMatrix', 'DOMMatrix']].forEach(function (p) {
            if (global[p[1]] && !global[p[0]]) Object.defineProperty(global, p[0], { value: global[p[1]], writable: true, configurable: true });
        });
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
        o.behavior = a.behavior;
        return o;
    }
    /* ---- Scrolling, at once or smoothly: what's running, per element ---- */
    var SCROLL_MS = 250;
    var scrollRuns = new Map();
    var rawTop = ELP && Object.getOwnPropertyDescriptor(ELP, 'scrollTop');
    var rawLeft = ELP && Object.getOwnPropertyDescriptor(ELP, 'scrollLeft');
    function scrollInterrupt(el) {
        var run = scrollRuns.get(el);
        if (!run) return;
        scrollRuns.delete(el);
        if (!scrollRuns.size) global.__cyberScrollHold(false);
        run.resolve({ interrupted: true });
    }
    /* (where it can go: some flows run to the negative side) */
    function scrollClamp(el, cs, left, top) {
        var vert = cs.writingMode !== 'horizontal-tb', rtl = cs.direction === 'rtl';
        var mx = Math.max(0, el.scrollWidth - el.clientWidth), my = Math.max(0, el.scrollHeight - el.clientHeight);
        var negL = vert ? /-rl$/.test(cs.writingMode) : rtl, negT = vert && rtl;
        /* (a control's text says nothing here: what's written clamps it) */
        return [mx ? Math.max(negL ? -mx : 0, Math.min(negL ? 0 : mx, left)) : left, my ? Math.max(negT ? -my : 0, Math.min(negT ? 0 : my, top)) : top];
    }
    function scrollSmoothly(el, behavior) {
        if (behavior === 'instant') return false;
        if (behavior === 'smooth') return !!el.isConnected;
        return !!el.isConnected && global.getComputedStyle(el).scrollBehavior === 'smooth';
    }
    function scrollGo(el, left, top, behavior) {
        var goal = snapGoal(el, left, top);
        if (goal) { left = goal[0]; top = goal[1]; }
        if (!scrollSmoothly(el, behavior)) {
            scrollInterrupt(el);
            if (left !== undefined) rawLeft.set.call(el, left);
            if (top !== undefined) rawTop.set.call(el, top);
            return Promise.resolve({ interrupted: false });
        }
        var l0 = rawLeft.get.call(el), t0 = rawTop.get.call(el), was = scrollRuns.get(el);
        var to = scrollClamp(el, global.getComputedStyle(el), left === undefined ? (was ? was.l1 : l0) : left, top === undefined ? (was ? was.t1 : t0) : top);
        var l1 = to[0], t1 = to[1];
        /* (going where it's going already: nothing changes) */
        if (was && was.l1 === l1 && was.t1 === t1) return was.promise;
        scrollInterrupt(el);
        if (l0 === l1 && t0 === t1) return Promise.resolve({ interrupted: false });
        var made = new Promise(function (resolve) {
            var run = { resolve: resolve, l1: l1, t1: t1 }, start = null;
            scrollRuns.set(el, run);
            global.__cyberScrollHold(true);
            var tick = function (now) {
                if (scrollRuns.get(el) !== run) return;
                if (start === null) start = now;
                var p = (now - start) / SCROLL_MS;
                if (p >= 1) {
                    scrollRuns.delete(el);
                    rawLeft.set.call(el, l1);
                    rawTop.set.call(el, t1);
                    if (!scrollRuns.size) global.__cyberScrollHold(false);
                    resolve({ interrupted: false });
                    return;
                }
                var e = p * p * (3 - 2 * p);
                rawLeft.set.call(el, l0 + (l1 - l0) * e);
                rawTop.set.call(el, t0 + (t1 - t0) * e);
                global.requestAnimationFrame(tick);
            };
            global.requestAnimationFrame(tick);
        });
        scrollRuns.get(el).promise = made;
        return made;
    }
    function scrollApply(el, o, by) {
        var l = o.left === undefined ? undefined : by ? rawLeft.get.call(el) + o.left : o.left;
        var t = o.top === undefined ? undefined : by ? rawTop.get.call(el) + o.top : o.top;
        return scrollGo(el, l, t, o.behavior);
    }
    if (rawTop && rawLeft) [['scrollTop', rawTop], ['scrollLeft', rawLeft]].forEach(function (d) {
        var top = d[0] === 'scrollTop', raw = d[1];
        Object.defineProperty(ELP, d[0], { configurable: true, enumerable: raw.enumerable, get: raw.get, set: function (v) {
            if (!(this instanceof global.Element)) throw new TypeError('Illegal invocation');
            scrollGo(this, top ? undefined : +v, top ? +v : undefined);
        } });
    });
    function scrollViewportElement() {
        var d = global.document;
        return d && d.scrollingElement;
    }
    [['scroll', false], ['scrollTo', false], ['scrollBy', true]].forEach(function (m) {
        var name = m[0], by = m[1];
        var named = function (f) { Object.defineProperty(f, 'name', { configurable: true, value: name }); return f; };
        /* (a promise that rejects for what isn't one: as an operation returning one does) */
        if (ELP) Object.defineProperty(ELP, name, { configurable: true, enumerable: true, writable: true, value: named(function () {
            if (!(this instanceof global.Element)) return Promise.reject(new TypeError('Illegal invocation'));
            return scrollApply(this, scrollArgs(name, 'Element', arguments), by);
        }) });
        Object.defineProperty(global, name, { configurable: true, enumerable: true, writable: true, value: named(function () {
            if (this !== undefined && this !== null && this !== global && !(global.Window && this instanceof global.Window))
                return Promise.reject(new TypeError('Illegal invocation'));
            var o = scrollArgs(name, 'Window', arguments), el = scrollViewportElement();
            return el ? scrollApply(el, o, by) : Promise.resolve({ interrupted: false });
        }) });
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
    var scrollWith, scrollLast;
    function scrollIntoViewOf(el, block, inline, inner) {
        var d = el.ownerDocument, root = d.documentElement;
        /* (inside a content-visibility: hidden subtree: nothing to scroll to) */
        try {
            for (var a0 = el.parentElement; a0; a0 = a0.parentElement)
                if (global.getComputedStyle(a0).contentVisibility === "hidden") return;
        } catch (e) {}
        /* (what smooth scrolls begun here will still take off it) */
        var pendX = 0, pendY = 0;
        var pend = function (a) {
            var run = scrollRuns.get(a);
            if (!run) return;
            pendX += run.l1 - rawLeft.get.call(a);
            pendY += run.t1 - rawTop.get.call(a);
        };
        /* (its scroll-margin: the area scrolled to) */
        var sm = [0, 0, 0, 0];
        try {
            var ecs = global.getComputedStyle(el);
            var smz = el.currentCSSZoom || 1;
            sm = [ecs.scrollMarginTop, ecs.scrollMarginRight, ecs.scrollMarginBottom, ecs.scrollMarginLeft].map(function (v) { return (parseFloat(v) || 0) * smz; });
        } catch (e) { /* (none) */ }
        /* (a scroller's scroll-padding: in from its view) */
        var spOf = function (a) {
            try {
                var c = global.getComputedStyle(a), z = a.currentCSSZoom || 1;
                return [c.scrollPaddingTop, c.scrollPaddingRight, c.scrollPaddingBottom, c.scrollPaddingLeft].map(function (v) { return /%$/.test(v) ? 0 : (parseFloat(v) || 0) * z; });
            } catch (e) { return [0, 0, 0, 0]; }
        };
        var rectOf = function () {
            var r0 = el.getBoundingClientRect();
            var r = { left: r0.left - sm[3], top: r0.top - sm[0], right: r0.right + sm[1], bottom: r0.bottom + sm[2] };
            if (!inner) return pendX || pendY ? { left: r.left - pendX, top: r.top - pendY, right: r.right - pendX, bottom: r.bottom - pendY } : r;
            var left = r.left + el.clientLeft + inner.left - pendX, top = r.top + el.clientTop + inner.top - pendY;
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
            var r = rectOf(), b = a.getBoundingClientRect(), sp = spOf(a);
            var bz = a.currentCSSZoom || 1, l = b.left + a.clientLeft * bz, t = b.top + a.clientTop * bz;
            var az = a.currentCSSZoom || 1;
            scrollLast = scrollGo(a, rawLeft.get.call(a) + scrollAlign(inline, r.left, r.right, l + sp[3], l + a.clientWidth * az - sp[1]),
                                  rawTop.get.call(a) + scrollAlign(block, r.top, r.bottom, t + sp[0], t + a.clientHeight * az - sp[2]), scrollWith);
            pend(a);
        }
        var se = d.scrollingElement;
        if (!se) return;
        var v = rectOf(), vp = spOf(root);
        scrollLast = scrollGo(se, rawLeft.get.call(se) + scrollAlign(inline, v.left, v.right, vp[3], global.innerWidth - vp[1]),
                              rawTop.get.call(se) + scrollAlign(block, v.top, v.bottom, vp[0], global.innerHeight - vp[2]), scrollWith);
        pend(se);
        /* (the parent's too, if this window may reach it) */
        var fe = global.frameElement, view = fe && fe.ownerDocument && fe.ownerDocument.defaultView;
        if (view && view.__cyberScrollIntoView) view.__cyberScrollIntoView(fe, block, inline, rectOf());
    }
    Object.defineProperty(global, '__cyberScrollIntoView', { configurable: true, value: scrollIntoViewOf });
    if (ELP) {
        Object.defineProperty(ELP, 'scrollIntoView', { configurable: true, enumerable: true, writable: true, value: function scrollIntoView() {
            if (!(this instanceof global.Element)) return Promise.reject(new TypeError('Illegal invocation'));
            scrollWith = typeof arguments[0] === 'object' && arguments[0] ? arguments[0].behavior : undefined;
            scrollLast = null;
            var arg = arguments[0], block = 'start', inline = 'nearest', LOGICAL = ['start', 'center', 'end', 'nearest'];
            if (typeof arg === 'object' && arg !== null || typeof arg === 'function') {
                if (arg.behavior !== undefined) scrollEnum('scrollIntoView', 'Element', arg.behavior, ['auto', 'instant', 'smooth']);
                if (arg.block !== undefined) block = scrollEnum('scrollIntoView', 'Element', arg.block, LOGICAL);
                if (arg.inline !== undefined) inline = scrollEnum('scrollIntoView', 'Element', arg.inline, LOGICAL);
            } else if (arg !== undefined && arg !== null && !arg) {
                block = 'end';
            }
            scrollIntoViewOf(this, block, inline);
            scrollWith = undefined;
            return scrollLast || Promise.resolve({ interrupted: false });
        } });
        /* ---- CSSPseudoElement: one object per element and type ---- */
        var PSEUDO = new WeakMap(), pseudoOf = new WeakMap();
        var PSEUDO_TYPES = /^::(?:before|after|marker|backdrop|scroll-marker|scroll-marker-group|column|view-transition|check|picker-icon)$/;
        var PS_BUTTON = /^::scroll-button\((block-start|block-end|inline-start|inline-end|up|down|left|right|prev|next)\)$/;
        var CSSPseudoElement = function CSSPseudoElement() { throw new TypeError('Illegal constructor'); };
        var pseudoProto = CSSPseudoElement.prototype;
        Object.defineProperty(pseudoProto, Symbol.toStringTag, { configurable: true, value: 'CSSPseudoElement' });
        var psGet = function (self) { var s = PSEUDO.get(self); if (!s) throw new TypeError('Illegal invocation'); return s; };
        ['type', 'element', 'parent', 'selectorText'].forEach(function (k) {
            Object.defineProperty(pseudoProto, k, { configurable: true, enumerable: true, get: function () { return psGet(this)[k]; } });
        });
        var makePseudo = function (owner, type) {
            if (arguments.length < 2) throw new TypeError("Failed to execute 'pseudo': 1 argument required");
            type = String(type).toLowerCase();
            var btn = PS_BUTTON.exec(type);
            if (!btn && !PSEUDO_TYPES.test(type)) return null;
            var map = pseudoOf.get(owner);
            if (!map) pseudoOf.set(owner, map = new Map());
            var p = map.get(type);
            if (p) return p;
            p = Object.create(pseudoProto);
            var st = PSEUDO.get(owner);
            PSEUDO.set(p, { type: btn ? '::scroll-button' : type, element: st ? st.element : owner, parent: owner, selectorText: type });
            map.set(type, p);
            return p;
        };
        Object.defineProperty(ELP, 'pseudo', { configurable: true, enumerable: true, writable: true, value: function pseudo(type) {
            if (!(this instanceof global.Element)) throw new TypeError('Illegal invocation');
            return makePseudo.apply(null, [this].concat([].slice.call(arguments)));
        } });
        Object.defineProperty(pseudoProto, 'pseudo', { configurable: true, enumerable: true, writable: true, value: function pseudo(type) {
            psGet(this);
            return makePseudo.apply(null, [this].concat([].slice.call(arguments)));
        } });
        Object.defineProperty(global, 'CSSPseudoElement', { configurable: true, writable: true, value: CSSPseudoElement });
        /* (getPseudoElements(element, type): a list of the element's pseudo-elements of that type) */
        var PSLIST = new WeakMap();
        var CSSPseudoElementList = function CSSPseudoElementList() { throw new TypeError('Illegal constructor'); };
        var psl = function (o) { var l = PSLIST.get(o); if (!l) throw new TypeError('Illegal invocation'); return l; };
        Object.defineProperty(CSSPseudoElementList.prototype, Symbol.toStringTag, { configurable: true, value: 'CSSPseudoElementList' });
        Object.defineProperty(CSSPseudoElementList.prototype, 'length', { configurable: true, enumerable: true, get: function () { return psl(this).length; } });
        Object.defineProperty(CSSPseudoElementList.prototype, 'item', { configurable: true, enumerable: true, writable: true, value: function item(i) {
            var l = psl(this);
            if (arguments.length < 1) throw new TypeError("Failed to execute 'item': 1 argument required");
            return l[i >>> 0] || null;
        } });
        Object.defineProperty(CSSPseudoElementList.prototype, 'getByType', { configurable: true, enumerable: true, writable: true, value: function getByType(t) {
            var l = psl(this);
            if (arguments.length < 1) throw new TypeError("Failed to execute 'getByType': 1 argument required");
            t = String(t);
            for (var i = 0; i < l.length; i++) if (PSEUDO.get(l[i]).type === t || PSEUDO.get(l[i]).type === '::' + t) return l[i];
            return null;
        } });
        Object.defineProperty(global, 'CSSPseudoElementList', { configurable: true, writable: true, value: CSSPseudoElementList });
        Object.defineProperty(global, 'getPseudoElements', { configurable: true, enumerable: true, writable: true, value: function getPseudoElements(el, type) {
            if (arguments.length < 2) throw new TypeError("Failed to execute 'getPseudoElements': 2 arguments required");
            if (!(el instanceof global.Element)) throw new TypeError("Failed to execute 'getPseudoElements': parameter 1 is not of type 'Element'.");
            var o = Object.create(CSSPseudoElementList.prototype), p = makePseudo(el, '::' + String(type).replace(/^::/, ''));
            PSLIST.set(o, p ? [p] : []);
            return o;
        } });
        /* ---- GeometryUtils (CSSOM View): a box's corners, through the transforms ---- */
        var geoPx = function (v) { return parseFloat(v) || 0; };
        /* (a node as [matrix to the viewport, its box's rect there before that]) */
        var geoBox = function (node, box, fname) {
            box = box === undefined ? 'border' : String(box);
            if (['margin', 'border', 'padding', 'content'].indexOf(box) < 0)
                throw new TypeError("Failed to execute '" + fname + "': invalid box");
            if (node === document || (node && node.nodeType === 9)) {
                var sx = global.scrollX || 0, sy = global.scrollY || 0;
                return { m: [1, 0, 0, 1, 0, 0], x: -sx, y: -sy, w: global.innerWidth, h: global.innerHeight };
            }
            if (global.CSSPseudoElement && node instanceof global.CSSPseudoElement) {
                /* (its own box from its style; where it starts, the element's content) */
                var pe = node.element, pc = global.getComputedStyle(pe, node.type);
                var ev = pe.isConnected ? global.__cyberBoxMap(pe) : null;
                if (!ev || pc.display === 'none' || pc.content === 'none') throw new DOMException('The node has no box.', 'InvalidStateError');
                var ec = global.getComputedStyle(pe);
                var side = function (s, k) { return geoPx(s[k]); };
                var bl = side(pc, 'borderLeftWidth'), bt0 = side(pc, 'borderTopWidth'), pl = side(pc, 'paddingLeft'), pt = side(pc, 'paddingTop');
                var ww = side(pc, 'width') + pl + side(pc, 'paddingRight') + bl + side(pc, 'borderRightWidth');
                var hh = side(pc, 'height') + pt + side(pc, 'paddingBottom') + bt0 + side(pc, 'borderBottomWidth');
                var px0 = ev[6] + side(ec, 'borderLeftWidth') + side(ec, 'paddingLeft') + side(pc, 'marginLeft');
                var py0 = ev[7] + side(ec, 'borderTopWidth') + side(ec, 'paddingTop') + side(pc, 'marginTop');
                if (pc.position === 'absolute') { px0 += geoPx(pc.left); py0 += geoPx(pc.top); }
                var o0 = box === 'margin' ? -side(pc, 'marginLeft') : box === 'border' ? 0 : box === 'padding' ? bl : bl + pl;
                var o1 = box === 'margin' ? -side(pc, 'marginTop') : box === 'border' ? 0 : box === 'padding' ? bt0 : bt0 + pt;
                var e0 = box === 'margin' ? -side(pc, 'marginRight') : box === 'border' ? 0 : box === 'padding' ? side(pc, 'borderRightWidth') : side(pc, 'borderRightWidth') + side(pc, 'paddingRight');
                var e1 = box === 'margin' ? -side(pc, 'marginBottom') : box === 'border' ? 0 : box === 'padding' ? side(pc, 'borderBottomWidth') : side(pc, 'borderBottomWidth') + side(pc, 'paddingBottom');
                return { m: ev.slice(0, 6), x: px0 + o0, y: py0 + o1, w: ww - o0 - e0, h: hh - o1 - e1 };
            }
            if (node && node.nodeType === 3) {
                var rg = document.createRange();
                rg.selectNodeContents(node);
                var tr = rg.getBoundingClientRect();
                return { m: [1, 0, 0, 1, 0, 0], x: tr.left, y: tr.top, w: tr.width, h: tr.height };
            }
            if (!node || node.nodeType !== 1) throw new TypeError("Failed to execute '" + fname + "': not a geometry node");
            var v = node.isConnected ? global.__cyberBoxMap(node) : null;
            if (!v) throw new DOMException('The node has no box.', 'InvalidStateError');
            var cs = global.getComputedStyle(node);
            var off = function (side) {
                if (box === 'margin') return -geoPx(cs['margin' + side]);
                if (box === 'border') return 0;
                var bw = geoPx(cs['border' + side + 'Width']);
                return box === 'padding' ? bw : bw + geoPx(cs['padding' + side]);
            };
            var l = off('Left'), t = off('Top'), r = off('Right'), bt = off('Bottom');
            return { m: v.slice(0, 6), x: v[6] + l, y: v[7] + t, w: v[8] - l - r, h: v[9] - t - bt };
        };
        var geoTo = function (m, p) { return { x: m[0] * p.x + m[2] * p.y + m[4], y: m[1] * p.x + m[3] * p.y + m[5] }; };
        var geoFrom = function (m, p) {
            var det = m[0] * m[3] - m[1] * m[2], dx = p.x - m[4], dy = p.y - m[5];
            return { x: (m[3] * dx - m[2] * dy) / det, y: (m[0] * dy - m[1] * dx) / det };
        };
        /* (from a point in one's box to one in another's) */
        var geoPoint = function (from, to, p) {
            var v = geoTo(from.m, { x: p.x + from.x, y: p.y + from.y }), o = geoFrom(to.m, v);
            return { x: o.x - to.x, y: o.y - to.y };
        };
        var geoQuads = function getBoxQuads() {
            var o = arguments[0] || {};
            /* (no generated box: none to give) */
            if (global.CSSPseudoElement && this instanceof global.CSSPseudoElement) {
                var ps0 = global.getComputedStyle(this.element, this.type);
                if (!this.element.isConnected || ps0.display === 'none' || ps0.content === 'none') return [];
            }
            var b = geoBox(this, o.box, 'getBoxQuads');
            var to = o.relativeTo !== undefined ? geoBox(o.relativeTo, 'border', 'getBoxQuads') : { m: [1, 0, 0, 1, 0, 0], x: 0, y: 0 };
            var c = [[0, 0], [b.w, 0], [b.w, b.h], [0, b.h]].map(function (k) { return geoPoint(b, to, { x: k[0], y: k[1] }); });
            return [new global.DOMQuad(c[0], c[1], c[2], c[3])];
        };
        var geoArgs = function (self, name, a, min) {
            if (a.length < min) throw new TypeError("Failed to execute '" + name + "': 2 arguments required");
            var o = a[2] || {};
            return { from: geoBox(a[1], o.fromBox, name), to: geoBox(self, o.toBox, name) };
        };
        var geoMethods = {
            getBoxQuads: geoQuads,
            convertQuadFromNode: function convertQuadFromNode(quad, from) {
                var g = geoArgs(this, 'convertQuadFromNode', arguments, 2), q = Object(quad);
                var c = ['p1', 'p2', 'p3', 'p4'].map(function (k) { var p = q[k] || {}; return geoPoint(g.from, g.to, { x: +p.x || 0, y: +p.y || 0 }); });
                return new global.DOMQuad(c[0], c[1], c[2], c[3]);
            },
            convertRectFromNode: function convertRectFromNode(rect, from) {
                var g = geoArgs(this, 'convertRectFromNode', arguments, 2), r = Object(rect), x = +r.x || 0, y = +r.y || 0, w = +r.width || 0, h = +r.height || 0;
                var c = [[x, y], [x + w, y], [x + w, y + h], [x, y + h]].map(function (k) { return geoPoint(g.from, g.to, { x: k[0], y: k[1] }); });
                return new global.DOMQuad(c[0], c[1], c[2], c[3]);
            },
            convertPointFromNode: function convertPointFromNode(point, from) {
                var g = geoArgs(this, 'convertPointFromNode', arguments, 2), p = Object(point);
                var o = geoPoint(g.from, g.to, { x: +p.x || 0, y: +p.y || 0 });
                return new global.DOMPoint(o.x, o.y, +p.z || 0, p.w === undefined ? 1 : +p.w);
            },
        };
        Object.keys(geoMethods).forEach(function (k) {
            [ELP, DOCP, global.Text && global.Text.prototype, pseudoProto].forEach(function (P) {
                if (P) Object.defineProperty(P, k, { configurable: true, enumerable: true, writable: true, value: geoMethods[k] });
            });
        });
        Object.defineProperty(ELP, 'scrollIntoViewIfNeeded', { configurable: true, enumerable: true, writable: true, value: function () {
            scrollIntoViewOf(this, 'nearest', 'nearest');
        } });
    }
    /* ---- CSS Custom Highlight API: the sets and their registry, sent to be
     * painted (__cyberHighlightSet) after each change ---- */
    var HL = new WeakMap();
    var hlDirty = false, theRegistry = null;
    var hlRange = function (r, hi, flat) {
        var sc = r.startContainer, ec = r.endContainer, so = r.startOffset, eo = r.endOffset;
        if (!sc || !ec) return;
        if (sc === ec && sc.nodeType === 3) { if (eo > so) flat.push(hi, sc, so, eo); return; }
        var doc = sc.nodeType === 9 ? sc : sc.ownerDocument;
        if (!doc || !doc.createRange) return;
        var lr = doc.createRange();
        try { lr.setStart(sc, so); lr.setEnd(ec, eo); } catch (e) { return; }
        if (lr.collapsed) return;
        var root = lr.commonAncestorContainer;
        if (root.nodeType === 3) { flat.push(hi, root, so, eo); return; }
        var w = doc.createTreeWalker(root, 4);
        for (var t = w.nextNode(); t; t = w.nextNode()) {
            if (!lr.intersectsNode(t)) continue;
            var s = t === sc ? so : 0, e = t === ec ? eo : t.length;
            if (e > s) flat.push(hi, t, s, e);
        }
    };
    /* (text directives, #:~:text=[prefix-,]start[,end][,-suffix]: the first
     * match of each in the document's text, case-insensitively) */
    var hlTargetText = function (names, prios, flat) {
        var href = String(global.location && global.location.href || ''), at = href.indexOf(':~:');
        if (at < 0 || !global.document || !global.document.body) return;
        var dirs = href.slice(at + 3).split('&'), nodes = [], starts = [], text = '';
        var w = global.document.createTreeWalker(global.document.body, 4);
        for (var t = w.nextNode(); t; t = w.nextNode()) {
            var p = t.parentNode && t.parentNode.localName;
            if (p === 'script' || p === 'style') continue;
            nodes.push(t); starts.push(text.length); text += t.data;
        }
        var low = text.toLowerCase(), ti = -1;
        for (var d = 0; d < dirs.length; d++) {
            if (dirs[d].slice(0, 5) !== 'text=') continue;
            var parts = dirs[d].slice(5).split(',').map(function (x) { try { return decodeURIComponent(x); } catch (e) { return x; } });
            if (parts.length && /-$/.test(parts[0])) parts.shift();
            if (parts.length && /^-/.test(parts[parts.length - 1])) parts.pop();
            if (!parts.length || !parts[0]) continue;
            var a = low.indexOf(parts[0].toLowerCase());
            if (a < 0) continue;
            var b = a + parts[0].length;
            if (parts.length > 1 && parts[1]) {
                var eb = low.indexOf(parts[1].toLowerCase(), b);
                if (eb < 0) continue;
                b = eb + parts[1].length;
            }
            if (ti < 0) { ti = names.length; names.push('\u0001target-text'); prios.push(0x3ffffff0); }
            for (var k = 0; k < nodes.length; k++) {
                var ns = starts[k], ne = ns + nodes[k].data.length;
                if (ne <= a || ns >= b) continue;
                flat.push(ti, nodes[k], Math.max(a, ns) - ns, Math.min(b, ne) - ns);
            }
        }
    };
    var hlLastKey = null, hlIds = new WeakMap(), hlNextId = 1;
    var hlNodeId = function (n) {
        if (!n) return 0;
        var v = hlIds.get(n);
        if (!v) { v = hlNextId++; hlIds.set(n, v); }
        return v;
    };
    var hlInFlush = false;
    var hlFlush = function () {
        if (hlInFlush) return;
        hlInFlush = true;
        try { hlFlush0(); } finally { hlInFlush = false; }
    };
    var hlFlush0 = function () {
        hlDirty = false;
        if (typeof global.__cyberHighlightSet !== 'function') return;
        var names = [], prios = [], flat = [];
        /* (no registry yet: the text fragment and the selection still) */
        if (theRegistry) HL.get(theRegistry).forEach(function (h, name) {
            var hi = names.length;
            names.push(name);
            /* (one made in another window: through its own methods) */
            var own = hlState.get(h);
            prios.push(own ? own.priority : (Number(h.priority) | 0));
            (HL.get(h) || h).forEach(function (r) { try { hlRange(r, hi, flat); } catch (e) {} });
        });
        /* (the URL's text fragment: ::target-text, under the selection) */
        try { hlTargetText(names, prios, flat); } catch (e) {}
        /* (the selection: last, over all of them, its name marked) */
        var sel = global.__cyberSelection, sr = sel && sel.__range;
        if (sr && !sr.collapsed) {
            var si = names.length;
            names.push('\u0001selection');
            prios.push(0x3fffffff);
            var f0 = flat.length;
            try { hlRange(sr, si, flat); } catch (e) {}
            /* (text of user-select: none is not selected) */
            for (var fq = flat.length - 4; fq >= f0; fq -= 4) {
                var tn = flat[fq + 1], pe = tn && tn.parentElement;
                var us = '';
                try { us = pe ? global.getComputedStyle(pe).getPropertyValue('user-select') : ''; } catch (e) { us = ''; }
                if (us === 'none') flat.splice(fq, 4);
            }
        } else if (sr) {
            /* (collapsed: where the caret is, at a text node's offset) */
            try {
                var cn = sr.startContainer, co = sr.startOffset;
                if (cn && cn.nodeType === 1) {
                    var bef = co > 0 ? cn.childNodes[co - 1] : null, aft = cn.childNodes[co] || null;
                    if (bef && bef.nodeType === 3) { cn = bef; co = bef.data.length; }
                    else if (aft && aft.nodeType === 3) { cn = aft; co = 0; }
                }
                var host = cn && cn.parentNode;
                while (host && host.nodeType === 1 && !(host.getAttribute && host.hasAttribute('contenteditable') && host.getAttribute('contenteditable') !== 'false')) host = host.parentNode;
                if (cn && cn.nodeType === 3 && host && host.nodeType === 1) {
                    var ci = names.length;
                    names.push('\u0001caret');
                    prios.push(0);
                    flat.push(ci, cn, co, co);
                }
            } catch (e) {}
        }
        /* (one highlight's ranges over the same text: painted once, as their union) */
        var segs = [];
        for (var q = 0; q + 3 < flat.length; q += 4) segs.push([flat[q], flat[q + 1], flat[q + 2], flat[q + 3], q]);
        var merged = [];
        segs.forEach(function (g) {
            for (var m = 0; m < merged.length; m++) {
                var o = merged[m];
                if (o[0] === g[0] && o[1] === g[1] && g[2] <= o[3] && g[3] >= o[2]) {
                    o[2] = Math.min(o[2], g[2]); o[3] = Math.max(o[3], g[3]);
                    return;
                }
            }
            merged.push(g.slice());
        });
        if (merged.length !== segs.length) {
            flat = [];
            merged.forEach(function (o) { flat.push(o[0], o[1], o[2], o[3]); });
        }
        /* (the same as last time: nothing to paint again) */
        var key = names.join('\u0002') + '|' + prios.join(',') + '|' + flat.map(function (x) { return typeof x === 'object' ? (hlNodeId(x)) : x; }).join(',');
        if (key === hlLastKey) return;
        hlLastKey = key;
        global.__cyberHighlightSet(names, prios, flat);
    };
    if (global.addEventListener) {
        global.addEventListener('hashchange', function () { hlTouch(); });
        global.addEventListener('load', function () { if (String(global.location && global.location.href).indexOf(':~:') >= 0) hlTouch(); });
    }
    Object.defineProperty(global, '__cyberSelectionChanged', { configurable: true, value: function () { hlTouch(); } });
    var hlTouch = function () {
        if (hlDirty) return;
        hlDirty = true;
        Promise.resolve().then(hlFlush);
    };
    var hlOf = function (h) {
        var s = HL.get(h);
        if (!s) throw new TypeError('Illegal invocation');
        return s;
    };
    var hlMethods = function (P, kind) {
        var entries = function () { return hlOf(this).entries(); };
        var keys = function () { return hlOf(this).keys(); };
        var values = function () { return hlOf(this).values(); };
        var hm = function (o) {
            Object.keys(o).forEach(function (k) {
                Object.defineProperty(o[k], 'name', { configurable: true, value: k });
                Object.defineProperty(P, k, { configurable: true, enumerable: true, writable: true, value: o[k] });
            });
        };
        hm({ entries: entries, values: values,
            forEach: function (cb) {
                if (typeof cb !== 'function') throw new TypeError('The callback provided as parameter 1 is not a function.');
                var self = this, that = arguments[1];
                hlOf(this).forEach(function (v, k) { cb.call(that, v, k, self); });
            },
            has: function (k) { if (!arguments.length) throw new TypeError('1 argument required'); return hlOf(this).has(k); },
            delete: function (k) { if (!arguments.length) throw new TypeError('1 argument required'); hlTouch(); return hlOf(this).delete(k); },
            clear: function () { hlOf(this).clear(); hlTouch(); } });
        /* (keys: its own function, named so, a set's being its values) */
        var keysFn = kind === 'set' ? function () { return hlOf(this).values(); } : keys;
        Object.defineProperty(keysFn, 'name', { configurable: true, value: 'keys' });
        Object.defineProperty(P, 'keys', { configurable: true, enumerable: true, writable: true, value: keysFn });
        Object.defineProperty(P, Symbol.iterator, { configurable: true, writable: true, value: kind === 'set' ? P.values : P.entries });
        var sizeGet = function () { return hlOf(this).size; };
        Object.defineProperty(sizeGet, 'name', { configurable: true, value: 'get size' });
        Object.defineProperty(P, 'size', { configurable: true, enumerable: true, get: sizeGet });
    };
    var HIGHLIGHT_TYPES = ['highlight', 'spelling-error', 'grammar-error'];
    var Highlight = function Highlight() {
        if (!new.target) throw new TypeError("Failed to construct 'Highlight': Please use the 'new' operator");
        var s = new Set();
        for (var i = 0; i < arguments.length; i++) {
            if (!(arguments[i] instanceof global.AbstractRange)) throw new TypeError("Failed to construct 'Highlight': not an AbstractRange");
            s.add(arguments[i]);
        }
        HL.set(this, s);
        hlState.set(this, { priority: 0, type: 'highlight' });
        hlTouch();
    };
    var hlState = new WeakMap();
    Object.defineProperty(Highlight, 'length', { configurable: true, value: 0 });
    Object.defineProperty(Highlight.prototype, Symbol.toStringTag, { configurable: true, value: 'Highlight' });
    hlMethods(Highlight.prototype, 'set');
    Object.defineProperty(Highlight.prototype, 'add', { configurable: true, enumerable: true, writable: true, value: function add(r) {
        if (!arguments.length) throw new TypeError('1 argument required');
        if (!(r instanceof global.AbstractRange)) throw new TypeError("Failed to execute 'add' on 'Highlight': not an AbstractRange");
        hlOf(this).add(r);
        hlTouch();
        return this;
    } });
    Object.defineProperty(Highlight.prototype, 'priority', { configurable: true, enumerable: true,
        get: function () { hlOf(this); return hlState.get(this).priority; },
        set: function (v) { hlOf(this); hlState.get(this).priority = (Number(v) | 0); hlTouch(); } });
    Object.defineProperty(Highlight.prototype, 'type', { configurable: true, enumerable: true,
        get: function () { hlOf(this); return hlState.get(this).type; },
        set: function (v) { hlOf(this); v = String(v); if (HIGHLIGHT_TYPES.indexOf(v) >= 0) hlState.get(this).type = v; } });
    Object.defineProperty(global, 'Highlight', { configurable: true, writable: true, value: Highlight });
    var HighlightRegistry = function HighlightRegistry() { throw new TypeError('Illegal constructor'); };
    Object.defineProperty(HighlightRegistry.prototype, Symbol.toStringTag, { configurable: true, value: 'HighlightRegistry' });
    hlMethods(HighlightRegistry.prototype, 'map');
    Object.defineProperty(HighlightRegistry.prototype, 'get', { configurable: true, enumerable: true, writable: true, value: function get(k) {
        if (!arguments.length) throw new TypeError('1 argument required');
        return hlOf(this).get(String(k));
    } });
    Object.defineProperty(HighlightRegistry.prototype, 'set', { configurable: true, enumerable: true, writable: true, value: function set(k, v) {
        if (arguments.length < 2) throw new TypeError('2 arguments required');
        if (!(v instanceof Highlight) && Object.prototype.toString.call(v) !== '[object Highlight]') throw new TypeError("Failed to execute 'set' on 'HighlightRegistry': not a Highlight");
        hlOf(this).set(String(k), v);
        hlTouch();
        return this;
    } });
    Object.defineProperty(HighlightRegistry.prototype, 'highlightsFromPoint', { configurable: true, enumerable: true, writable: true,
        value: function highlightsFromPoint(x, y) {
            if (!(this instanceof HighlightRegistry)) throw new TypeError('Illegal invocation');
            if (arguments.length < 2) throw new TypeError('2 arguments required');
            return [];
        } });
    Object.defineProperty(global, 'HighlightRegistry', { configurable: true, writable: true, value: HighlightRegistry });
    theRegistry = Object.create(HighlightRegistry.prototype);
    /* (a new page: nothing painted from the last one's) */
    if (typeof global.__cyberHighlightSet === 'function') global.__cyberHighlightSet([], [], []);
    HL.set(theRegistry, new Map());
    /* (a range in a highlight changed: painted again) */
    if (global.Range && global.Range.prototype) ['setStart', 'setEnd', 'setStartBefore', 'setStartAfter', 'setEndBefore', 'setEndAfter',
                                                'collapse', 'selectNode', 'selectNodeContents'].forEach(function (m) {
        var d = Object.getOwnPropertyDescriptor(global.Range.prototype, m);
        if (!d || typeof d.value !== 'function') return;
        var orig = d.value;
        var w = function () {
            var r = orig.apply(this, arguments);
            try { if (!hlInFlush && !hlDirty && HL.get(theRegistry).size) hlTouch(); } catch (e) { /* (none) */ }
            return r;
        };
        Object.defineProperty(w, 'name', { configurable: true, value: orig.name });
        Object.defineProperty(w, 'length', { configurable: true, value: orig.length });
        d.value = w;
        Object.defineProperty(global.Range.prototype, m, d);
    });
    Object.defineProperty(global, '__cyberHighlights', { configurable: true, value: theRegistry });
    if (global.CSS) Object.defineProperty(global.CSS, 'highlights', { configurable: true, enumerable: true, get: function () { return theRegistry; } });

    /* ---- dialogs and popovers: shown, hidden, modal (as far as style and events go) ---- */
    (function () {
        var ST_MODAL = 1 << 29, ST_POPOVER = 1 << 30;
        var elState = global.__cyberElState;
        if (typeof elState !== 'function') return;
        function method(P, name, fn, len) {
            Object.defineProperty(fn, 'name', { configurable: true, value: name });
            Object.defineProperty(P, name, { configurable: true, enumerable: true, writable: true, value: fn });
        }
        function invalidState(msg) { return new global.DOMException(msg, 'InvalidStateError'); }
        function toggleEvent(el, type, oldState, newState, cancelable) {
            var C = global.ToggleEvent || global.Event;
            return el.dispatchEvent(new C(type, { cancelable: !!cancelable, oldState: oldState, newState: newState }));
        }
        /* (a toggle event, in a task: one pending per element, its first old state kept) */
        var pendingToggle = new WeakMap();
        function queueToggle(el, oldState, newState) {
            var p = pendingToggle.get(el);
            if (p) { p.newState = newState; return; }
            p = { oldState: oldState, newState: newState };
            pendingToggle.set(el, p);
            global.setTimeout(function () {
                pendingToggle.delete(el);
                if (p.oldState !== p.newState) toggleEvent(el, 'toggle', p.oldState, p.newState, false);
            }, 0);
        }

        var D = global.HTMLDialogElement, DP = D && D.prototype;
        if (DP && typeof DP.showModal !== 'function') {
            var RV = new WeakMap();
            var isDialog = function (el) { return el instanceof D; };
            Object.defineProperty(DP, 'open', { configurable: true, enumerable: true,
                get: function () { if (!isDialog(this)) throw new TypeError('Illegal invocation'); return this.hasAttribute('open'); },
                set: function (v) { if (!isDialog(this)) throw new TypeError('Illegal invocation'); if (v) this.setAttribute('open', ''); else this.removeAttribute('open'); } });
            Object.defineProperty(DP, 'returnValue', { configurable: true, enumerable: true,
                get: function () { if (!isDialog(this)) throw new TypeError('Illegal invocation'); return RV.has(this) ? RV.get(this) : ''; },
                set: function (v) { if (!isDialog(this)) throw new TypeError('Illegal invocation'); RV.set(this, String(v)); } });
            method(DP, 'show', function () {
                if (!isDialog(this)) throw new TypeError('Illegal invocation');
                if (this.hasAttribute('open')) {
                    if (elState(this, ST_MODAL)) throw invalidState('The dialog is already open as a modal dialog.');
                    return;
                }
                if (!toggleEvent(this, 'beforetoggle', 'closed', 'open', true) || this.hasAttribute('open')) return;
                this.setAttribute('open', '');
                queueToggle(this, 'closed', 'open');
            });
            method(DP, 'showModal', function () {
                if (!isDialog(this)) throw new TypeError('Illegal invocation');
                if (this.hasAttribute('open')) {
                    if (elState(this, ST_MODAL)) return;
                    throw invalidState('The dialog is already open as a non-modal dialog.');
                }
                if (!this.isConnected) throw invalidState('The dialog is not connected.');
                if (elState(this, ST_POPOVER)) throw invalidState('The dialog is already open as a popover.');
                if (!toggleEvent(this, 'beforetoggle', 'closed', 'open', true) || this.hasAttribute('open')) return;
                this.setAttribute('open', '');
                elState(this, ST_MODAL, true);
                queueToggle(this, 'closed', 'open');
                /* (its focus: the autofocus one, or the dialog itself) */
                var f = this.querySelector('[autofocus]') || this;
                try { if (typeof f.focus === 'function') f.focus(); } catch (e) { /* (not focusable) */ }
            });
            var closeDialog = function (el, rv) {
                if (!el.hasAttribute('open')) return;
                toggleEvent(el, 'beforetoggle', 'open', 'closed', false);
                if (!el.hasAttribute('open')) return;
                el.removeAttribute('open');
                elState(el, ST_MODAL, false);
                if (rv !== undefined && rv !== null) RV.set(el, String(rv));
                queueToggle(el, 'open', 'closed');
                global.setTimeout(function () { el.dispatchEvent(new global.Event('close')); }, 0);
            };
            method(DP, 'close', function (rv) {
                if (!isDialog(this)) throw new TypeError('Illegal invocation');
                closeDialog(this, rv);
            });
            method(DP, 'requestClose', function (rv) {
                if (!isDialog(this)) throw new TypeError('Illegal invocation');
                if (!this.hasAttribute('open')) return;
                if (!this.dispatchEvent(new global.Event('cancel', { cancelable: true }))) return;
                closeDialog(this, rv);
            });
        }

        /* popovers: the popover attribute's element, shown and hidden */
        var HP = global.HTMLElement && global.HTMLElement.prototype;
        if (!HP || typeof HP.showPopover === 'function') return;
        function popoverKind(el) {
            var v = el.getAttribute('popover');
            if (v === null) return null;
            v = v.toLowerCase();
            return v === 'manual' ? 'manual' : v === 'hint' ? 'hint' : 'auto';
        }
        var isHTML = function (el) { return el instanceof global.HTMLElement; };
        function showing(el) { return !!elState(el, ST_POPOVER); }
        function check(el, open) {
            if (popoverKind(el) === null) throw new global.DOMException('Not a popover.', 'NotSupportedError');
            if (!el.isConnected) throw invalidState('The popover is not connected.');
            if (el.localName === 'dialog' && el.hasAttribute('open') && !showing(el)) throw invalidState('The dialog is already open.');
        }
        function hide(el, events) {
            if (!showing(el)) return;
            if (events) toggleEvent(el, 'beforetoggle', 'open', 'closed', false);
            if (!showing(el)) return;
            elState(el, ST_POPOVER, false);
            if (events) queueToggle(el, 'open', 'closed');
        }
        /* (the auto ones showing, oldest first) */
        var autoStack = [];
        function show(el) {
            if (showing(el)) return;
            check(el);
            if (!toggleEvent(el, 'beforetoggle', 'closed', 'open', true) || showing(el)) return;
            if (popoverKind(el) === 'auto') {
                /* (the others shown with it close, but for its ancestors) */
                for (var i = autoStack.length - 1; i >= 0; i--) {
                    var o = autoStack[i];
                    if (o === el || !showing(o) || o.contains(el)) continue;
                    autoStack.splice(i, 1);
                    hide(o, true);
                }
                autoStack.push(el);
            }
            elState(el, ST_POPOVER, true);
            queueToggle(el, 'closed', 'open');
        }
        method(HP, 'showPopover', function (options) {
            if (!isHTML(this)) throw new TypeError('Illegal invocation');
            show(this);
        });
        method(HP, 'hidePopover', function () {
            if (!isHTML(this)) throw new TypeError('Illegal invocation');
            if (!showing(this)) return;
            check(this);
            var k = autoStack.indexOf(this);
            if (k >= 0) autoStack.splice(k, 1);
            hide(this, true);
        });
        method(HP, 'togglePopover', function (force) {
            if (!isHTML(this)) throw new TypeError('Illegal invocation');
            var want = typeof force === 'boolean' ? force : force && typeof force === 'object' && typeof force.force === 'boolean' ? force.force : !showing(this);
            if (want) show(this); else if (showing(this)) this.hidePopover();
            else check(this);
            return showing(this);
        });
        Object.defineProperty(HP, 'popover', { configurable: true, enumerable: true,
            get: function () { if (!isHTML(this)) throw new TypeError('Illegal invocation'); return popoverKind(this); },
            set: function (v) {
                if (!isHTML(this)) throw new TypeError('Illegal invocation');
                if (v === null) this.removeAttribute('popover'); else this.setAttribute('popover', String(v));
            } });
        /* (a popover that stops being one: hidden) */
        var setA = global.Element.prototype.removeAttribute;
        if (global.MutationObserver) {
            var mo = new global.MutationObserver(function (recs) {
                recs.forEach(function (r) {
                    var el = r.target;
                    if (r.attributeName === 'popover' && showing(el) && popoverKind(el) !== r.oldValue) hide(el, false);
                    if (r.attributeName === 'open' && el.localName === 'dialog' && !el.hasAttribute('open')) elState(el, ST_MODAL, false);
                });
            });
            var watching = new WeakSet();
            global.__cyberWatchPopover = function (doc) {
                if (!doc || watching.has(doc)) return;
                watching.add(doc);
                mo.observe(doc, { attributes: true, attributeFilter: ['popover', 'open'], attributeOldValue: true, subtree: true });
            };
        }
        var origShow = show;
        show = function (el) { if (global.__cyberWatchPopover) global.__cyberWatchPopover(el.ownerDocument); origShow(el); };
        (void setA);
    })();

    /* ---- Window's sizes and offsets: read through getters that know their object ---- */
    ['innerWidth', 'innerHeight', 'outerWidth', 'outerHeight', 'screenX', 'screenY', 'screenLeft', 'screenTop', 'devicePixelRatio',
     'scrollX', 'scrollY', 'pageXOffset', 'pageYOffset', 'screen', 'visualViewport'].forEach(function (k) {
        var d = Object.getOwnPropertyDescriptor(global, k), val;
        if (!d) return;
        var mine = function () { return this === undefined || this === null || this === global; };
        var get = d.get ? function () { if (!mine.call(this)) throw new TypeError('Illegal invocation'); return d.get.call(global); }
                        : function () { if (!mine.call(this)) throw new TypeError('Illegal invocation'); return val; };
        if (!d.get) val = d.value;
        var set = d.get ? d.set : function (v) { if (!mine.call(this)) throw new TypeError('Illegal invocation'); val = v; };
        /* (named as an attribute's accessors are) */
        Object.defineProperty(get, 'name', { configurable: true, value: 'get ' + k });
        if (set && !d.get) Object.defineProperty(set, 'name', { configurable: true, value: 'set ' + k });
        Object.defineProperty(global, k, { configurable: true, enumerable: true, get: get, set: set });
    });
    ['moveTo', 'moveBy', 'resizeTo', 'resizeBy'].forEach(function (n) {
        var was = global[n];
        var f = function (a, b) {
            if (this !== undefined && this !== null && this !== global) throw new TypeError('Illegal invocation');
            if (arguments.length < 2) throw new TypeError("Failed to execute '" + n + "' on 'Window': 2 arguments required");
            if (typeof was === 'function') return was.call(this, a, b);
        };
        Object.defineProperty(f, 'name', { configurable: true, value: n });
        Object.defineProperty(global, n, { configurable: true, enumerable: true, writable: true, value: f });
    });
    /* (the box its scrolling is by, up the containing blocks) */
    if (global.Element) Object.defineProperty(global.Element.prototype, 'scrollParent', { configurable: true, enumerable: true, writable: true,
        value: function scrollParent() {
            if (!(this instanceof global.Element)) throw new TypeError('Illegal invocation');
            var d = this.ownerDocument, cs = global.getComputedStyle(this);
            if (!this.isConnected || cs.display === 'none' || this === d.documentElement) return null;
            var pos = cs.position;
            for (var a = this.assignedSlot || this.parentNode || this.host; a; a = a.assignedSlot || a.parentNode || a.host) {
                if (a.nodeType !== 1) continue;
                var s = global.getComputedStyle(a);
                var box = s.transform !== 'none' || s.perspective !== 'none' || s.filter !== 'none' || /paint|layout|strict|content/.test(s.contain || '');
                /* (what isn't its containing block doesn't scroll it) */
                if ((pos === 'absolute' && s.position === 'static' && !box) || (pos === 'fixed' && !box)) continue;
                if (a === d.documentElement) return d.scrollingElement;
                if (isScrollBox(s) && a !== d.body) return a;
                pos = s.position;
            }
            return pos === 'fixed' ? null : d.scrollingElement;
        } });
    if (global.HTMLImageElement) [['x', 6], ['y', 7]].forEach(function (m) {
        if (m[0] in global.HTMLImageElement.prototype) return;
        Object.defineProperty(global.HTMLImageElement.prototype, m[0], { configurable: true, enumerable: true,
            get: function () { var v = global.__cyberBoxMap(this); return v ? Math.round(v[m[1]]) : 0; } });
    });
    if (DOCP && !('elementsFromPoint' in DOCP))
        Object.defineProperty(DOCP, 'elementsFromPoint', { configurable: true, enumerable: true, writable: true, value: function elementsFromPoint(x, y) {
            if (arguments.length < 2) throw new TypeError("Failed to execute 'elementsFromPoint' on 'Document': 2 arguments required");
            if (!isFinite(+x) || !isFinite(+y)) throw new TypeError("Failed to execute 'elementsFromPoint' on 'Document': The provided double value is non-finite.");
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

    /* ---- the accessibility tree's role and name of an element (HTML-AAM, accname 1.2) ---- */
    var ARIA_ROLES = ('alert alertdialog application article banner blockquote button caption cell checkbox code columnheader ' +
        'combobox complementary contentinfo definition deletion dialog directory document emphasis feed figure form generic grid ' +
        'gridcell group heading img insertion link list listbox listitem log main mark marquee math menu menubar menuitem ' +
        'menuitemcheckbox menuitemradio meter navigation none note option paragraph presentation progressbar radio radiogroup ' +
        'region row rowgroup rowheader scrollbar search searchbox separator slider spinbutton status strong subscript ' +
        'superscript switch tab table tablist tabpanel term textbox time timer toolbar tooltip tree treegrid treeitem').split(' ');
    function axHidden(el) {
        for (var n = el; n && n.nodeType === 1; n = n.parentElement) {
            if (n.hasAttribute('hidden') || n.getAttribute('aria-hidden') === 'true') return true;
            var cs = global.getComputedStyle ? global.getComputedStyle(n) : null;
            if (cs && (cs.display === 'none' || (n === el && cs.visibility === 'hidden'))) return true;
        }
        return false;
    }
    function axTableRole(el) {
        for (var t = el.parentElement; t; t = t.parentElement) {
            if (t.localName === 'table' || /^(grid|treegrid|table)$/.test(t.getAttribute('role') || '')) return axRole(t);
        }
        return '';
    }
    function axSectioning(el) {
        for (var p = el.parentElement; p; p = p.parentElement)
            if (/^(article|aside|main|nav|section)$/.test(p.localName)) return true;
        return false;
    }
    function axRole(el) {
        var explicit = (el.getAttribute('role') || '').trim().toLowerCase().split(/\s+/);
        for (var i = 0; i < explicit.length; i++) if (ARIA_ROLES.indexOf(explicit[i]) >= 0) return explicit[i];
        var tag = el.localName, type = (el.getAttribute('type') || '').toLowerCase();
        if (el.namespaceURI === 'http://www.w3.org/2000/svg') {
            if (tag === 'svg') return 'graphics-document';
            if (tag === 'g') return axName(el) ? 'group' : 'generic';
            return '';
        }
        switch (tag) {
        case 'a': case 'area': return el.hasAttribute('href') ? 'link' : 'generic';
        case 'button': return 'button';
        case 'h1': case 'h2': case 'h3': case 'h4': case 'h5': case 'h6': return 'heading';
        case 'nav': return 'navigation';
        case 'main': return 'main';
        case 'header': return axSectioning(el) ? 'generic' : 'banner';
        case 'footer': return axSectioning(el) ? 'generic' : 'contentinfo';
        case 'aside': return 'complementary';
        case 'section': return axName(el) ? 'region' : 'generic';
        case 'form': return 'form';
        case 'search': return 'search';
        case 'article': return 'article';
        case 'ul': case 'ol': case 'menu': return 'list';
        case 'li': return 'listitem';
        case 'table': return 'table';
        case 'caption': return 'caption';
        case 'thead': case 'tbody': case 'tfoot': return 'rowgroup';
        case 'tr': return 'row';
        case 'td': return /^(grid|treegrid)$/.test(axTableRole(el)) ? 'gridcell' : 'cell';
        case 'th': return el.getAttribute('scope') === 'row' ? 'rowheader' : 'columnheader';
        case 'dialog': return 'dialog';
        case 'fieldset': case 'details': case 'optgroup': return 'group';
        case 'figure': return 'figure';
        case 'img': return el.getAttribute('alt') === '' ? 'presentation' : 'img';
        case 'hr': return 'separator';
        case 'p': return 'paragraph';
        case 'progress': return 'progressbar';
        case 'meter': return 'meter';
        case 'output': return 'status';
        case 'option': return 'option';
        case 'select': return el.multiple || el.size > 1 ? 'listbox' : 'combobox';
        case 'textarea': return 'textbox';
        case 'dl': return 'list';
        case 'dt': return 'term';
        case 'dd': return 'definition';
        case 'blockquote': return 'blockquote';
        case 'input':
            if (/^(button|submit|reset|image)$/.test(type)) return 'button';
            if (type === 'checkbox') return 'checkbox';
            if (type === 'radio') return 'radio';
            if (type === 'range') return 'slider';
            if (type === 'number') return 'spinbutton';
            if (type === 'search') return el.hasAttribute('list') ? 'combobox' : 'searchbox';
            if (type === 'hidden') return '';
            return el.hasAttribute('list') ? 'combobox' : 'textbox';
        case 'div': case 'span': return 'generic';
        }
        return '';
    }
    var NAME_FROM_CONTENT = /^(button|cell|checkbox|columnheader|gridcell|heading|link|menuitem|menuitemcheckbox|menuitemradio|option|radio|row|rowheader|switch|tab|tooltip|treeitem|caption|term|listitem)$/;
    function axText(node, referenced) {
        var out = '';
        for (var c = node.firstChild; c; c = c.nextSibling) {
            if (c.nodeType === 3) out += c.data;
            else if (c.nodeType === 1) {
                if (!referenced && axHidden(c)) continue;
                if (c.localName === 'title' && c.namespaceURI === 'http://www.w3.org/2000/svg') continue;
                var sub = c.getAttribute('aria-label') || axText(c, referenced);
                var cs = global.getComputedStyle ? global.getComputedStyle(c) : null;
                var block = cs && /^(block|flex|grid|list-item|table)/.test(cs.display);
                out += block ? ' ' + sub + ' ' : sub;
            }
        }
        return out;
    }
    function axClean(t) { return String(t || '').replace(/\s+/g, ' ').trim(); }
    function axName(el) {
        var doc = el.ownerDocument;
        var by = (el.getAttribute('aria-labelledby') || '').trim();
        if (by) {
            var parts = by.split(/\s+/).map(function (id) {
                var r = doc.getElementById(id);
                return r ? axClean(r.getAttribute('aria-label') || axText(r, true)) : '';
            }).filter(Boolean);
            if (parts.length) return parts.join(' ');
        }
        var al = axClean(el.getAttribute('aria-label'));
        if (al) return al;
        var tag = el.localName;
        if (/^(input|button|textarea|select|meter|progress|output)$/.test(tag)) {
            var labels = el.labels ? Array.prototype.slice.call(el.labels) : [];
            var lt = axClean(labels.map(function (l) { return axText(l, false); }).join(' '));
            if (lt) return lt;
            if (tag === 'input' && /^(button|submit|reset)$/.test((el.type || '').toLowerCase())) return axClean(el.value);
        }
        if (tag === 'img' || (tag === 'input' && el.type === 'image')) { var alt = el.getAttribute('alt'); if (alt) return axClean(alt); }
        if (el.namespaceURI === 'http://www.w3.org/2000/svg') {
            for (var c = el.firstElementChild; c; c = c.nextElementSibling) if (c.localName === 'title') return axClean(c.textContent);
        }
        if (tag === 'table') { var cap = el.querySelector('caption'); if (cap) return axClean(axText(cap, false)); }
        if (tag === 'fieldset') { var lg = el.querySelector('legend'); if (lg) return axClean(axText(lg, false)); }
        if (tag === 'figure') { var fc = el.querySelector('figcaption'); if (fc) return axClean(axText(fc, false)); }
        var role = axRoleOnly(el);
        if (NAME_FROM_CONTENT.test(role)) { var tc = axClean(axText(el, false)); if (tc) return tc; }
        return axClean(el.getAttribute('title'));
    }
    var axDepth = 0;
    function axRoleOnly(el) {
        /* (a role that asks for the name asks again: once is enough) */
        if (axDepth > 2) return '';
        axDepth++;
        try { return axRole(el); } finally { axDepth--; }
    }
    /* ---- print mode (CSS Page 3): pages as a strip, one under the other: the
     * engine paginates the root (__cyberPrintSet); here the @page rules, the page
     * boxes and their margin boxes.  Default page: 5in x 3in, margins 0.5in, as
     * WPT's print reftests expect. */
    /* (cascade layers by first mention, dotted paths; an @page rule's layer as __printLayer) */
    var printLayers = [];
    function printLayerRank(path) {
        var k = printLayers.indexOf(path);
        if (k < 0) { printLayers.push(path); k = printLayers.length - 1; }
        return k;
    }
    var printAnon = 0;
    function printRules(list, out, layer) {
        for (var i = 0; list && i < list.length; i++) {
            var r = list[i];
            if (global.CSSPageRule && r instanceof global.CSSPageRule) { r.__printLayer = layer === undefined ? null : layer; out.push(r); }
            else if (global.CSSLayerStatementRule && r instanceof global.CSSLayerStatementRule) {
                (r.nameList || []).forEach(function (n) { printLayerRank(layer ? layer + '.' + n : n); });
            }
            else if (global.CSSLayerBlockRule && r instanceof global.CSSLayerBlockRule) {
                var nm = r.name ? (layer ? layer + '.' + r.name : r.name) : (layer ? layer + '.' : '') + '\u0000' + (printAnon++);
                printLayerRank(nm);
                try { printRules(r.cssRules, out, nm); } catch (e) { /* (none) */ }
            }
            else if (r.cssRules) { try { printRules(r.cssRules, out, layer); } catch (e) { /* (none) */ } }
            else if (r.styleSheet) { try { printRules(r.styleSheet.cssRules, out, layer); } catch (e) { /* (cross origin) */ } }
        }
    }
    var PAGE_SIZES = { a5: [148, 210], a4: [210, 297], a3: [297, 420], b5: [176, 250], b4: [250, 353], 'jis-b5': [182, 257], 'jis-b4': [257, 364] };
    var PAGE_IN = { letter: [8.5, 11], legal: [8.5, 14], ledger: [11, 17] };
    function printLen(v, base) {
        v = String(v).trim();
        var m = /^(-?[\d.]+)%$/.exec(v);
        if (m) return base * parseFloat(m[1]) / 100;
        var e = document.createElement('div');
        e.style.cssText = 'position:absolute;visibility:hidden;width:0';
        e.style.width = v;
        (document.documentElement || document).appendChild(e);
        var w = parseFloat(getComputedStyle(e).width);
        e.remove();
        return isNaN(w) ? 0 : w;
    }
    function printSpec(sel, name, k) {
        /* (specificity: (name, first/blank, left/right)); -1: doesn't match) */
        var best = -1;
        (sel || '').split(',').forEach(function (p) {
            p = p.trim();
            var m = /^([^:]*)((?::[a-z]+)*)$/i.exec(p);
            if (!m) return;
            var sp = 0, ok = true;
            if (m[1]) { if (m[1] !== name) ok = false; sp += 65536; }
            (m[2].match(/:[a-z]+/gi) || []).forEach(function (c) {
                c = c.toLowerCase();
                if (c === ':first') { if (k !== 0) ok = false; sp += 256; }
                else if (c === ':blank') { ok = false; }
                else if (c === ':left') { if (k % 2 === 0) ok = false; sp += 1; }
                else if (c === ':right') { if (k % 2 === 1) ok = false; sp += 1; }
                else ok = false;
            });
            if (ok && sp > best) best = sp;
        });
        return best;
    }
    /* (the rules of page k, least specific first; later of the same specificity after) */
    function printPageRules(rules, name, k) {
        var o = [];
        rules.forEach(function (r, i) {
            var s = printSpec(r.selectorText, name, k);
            var L = r.__printLayer == null ? 1e9 : printLayerRank(r.__printLayer);
            if (s >= 0) o.push([s, i, r, L]);
        });
        o.sort(function (a, b) { return a[3] - b[3] || a[0] - b[0] || a[1] - b[1]; });
        return o.map(function (x) { return x[2]; });
    }
    function printDecl(rs, prop) {
        var v = '';
        rs.forEach(function (r) {
            var w = r.style.getPropertyValue(prop);
            if (!w && prop.indexOf('margin-') === 0) { var m = r.style.getPropertyValue('margin'); if (m) w = marginSide(m, prop); }
            if (w) v = w;
        });
        return v;
    }
    function marginSide(m, prop) {
        var p = m.trim().split(/\s+/), t = p[0], r = p[1] || t, b = p[2] || t, l = p[3] || r;
        return prop === 'margin-top' ? t : prop === 'margin-right' ? r : prop === 'margin-bottom' ? b : l;
    }
    function printSize(rs) {
        var w = 480, h = 288, v = (printDecl(rs, 'size') || '').toLowerCase().trim();
        if (!v || v === 'auto') return [w, h];
        var land = false, port = false, nums = [];
        v.split(/\s+/).forEach(function (t) {
            if (t === 'landscape') land = true;
            else if (t === 'portrait') port = true;
            else if (PAGE_SIZES[t]) nums = [PAGE_SIZES[t][0] * 96 / 25.4, PAGE_SIZES[t][1] * 96 / 25.4];
            else if (PAGE_IN[t]) nums = [PAGE_IN[t][0] * 96, PAGE_IN[t][1] * 96];
            else if (t !== 'auto') nums.push(printLen(t, 0));
        });
        if (nums.length === 1) { w = h = nums[0]; }
        else if (nums.length >= 2) { w = nums[0]; h = nums[1]; }
        if ((land && h > w) || (port && w > h)) { var q = w; w = h; h = q; }
        return [w, h];
    }
    function printMetrics(rs, pw, ph) {
        var side = function (p, base) { var v = printDecl(rs, p); return v === '' ? 48 : v === 'auto' ? 0 : printLen(v, base); };
        return { top: side('margin-top', ph), right: side('margin-right', pw), bottom: side('margin-bottom', ph), left: side('margin-left', pw) };
    }
    var MB_ROWS = {
        top: ['top-left', 'top-center', 'top-right'], bottom: ['bottom-left', 'bottom-center', 'bottom-right'],
        left: ['left-top', 'left-middle', 'left-bottom'], right: ['right-top', 'right-middle', 'right-bottom'],
    };
    var MB_DEFAULT = {
        'top-left-corner': ['right', 'middle'], 'top-left': ['left', 'middle'], 'top-center': ['center', 'middle'], 'top-right': ['right', 'middle'], 'top-right-corner': ['left', 'middle'],
        'bottom-left-corner': ['right', 'middle'], 'bottom-left': ['left', 'middle'], 'bottom-center': ['center', 'middle'], 'bottom-right': ['right', 'middle'], 'bottom-right-corner': ['left', 'middle'],
        'left-top': ['center', 'top'], 'left-middle': ['center', 'middle'], 'left-bottom': ['center', 'bottom'],
        'right-top': ['center', 'top'], 'right-middle': ['center', 'middle'], 'right-bottom': ['center', 'bottom'],
    };
    /* (a margin box's declarations from page k's rules) */
    function printMarginDecls(rs) {
        var by = {};
        rs.forEach(function (r) {
            (r.__rules || []).forEach(function (m) {
                var o = by[m.__name] = by[m.__name] || {};
                var st = m.style;
                for (var i = 0; i < st.length; i++) o[st[i]] = st.getPropertyValue(st[i]);
            });
        });
        return by;
    }
    function printContent(c, k, n) {
        /* (strings, counter(page), counter(pages), as text) */
        var out = '', re = /"((?:[^"\\]|\\.)*)"|'((?:[^'\\]|\\.)*)'|counter\(\s*(page|pages)\s*(?:,[^)]*)?\)/g, m;
        while ((m = re.exec(c))) {
            if (m[3]) out += m[3] === 'page' ? (printPageNo >= 0 ? printPageNo : k + 1) : n;
            else out += (m[1] !== undefined ? m[1] : m[2]).replace(/\\(.)/g, '$1');
        }
        return out;
    }
    function printHasBox(d) {
        if (!d || d.content === undefined) return false;
        var c = String(d.content).trim().toLowerCase();
        return c !== 'none' && c !== 'normal';
    }
    /* (one margin box's element holding its text; for measuring and showing) */
    var printSeq = 0, printSheet = null, printPageCtr = '', printPageNo = -1, printCtrMap = {};
    /* (counter-reset/-increment/-set text: [name, value] pairs, each value defaulting to dflt) */
    function printCtrList(v, dflt) {
        var o = [], t = String(v || '').trim().split(/\s+/);
        if (!t[0] || t[0] === 'none') return o;
        for (var i = 0; i < t.length; i++) {
            var name = t[i];
            if (!/^-?[A-Za-z_][\w-]*$/.test(name)) continue;
            var nv = dflt;
            if (i + 1 < t.length && /^-?\d+$/.test(t[i + 1])) { nv = parseInt(t[i + 1], 10); i++; }
            o.push([name, nv]);
        }
        return o;
    }
    function printBoxEl(name, d, k, n) {
        var cell = document.createElement('div'), def = MB_DEFAULT[name];
        var skip = { content: 1, width: 1, height: 1, 'min-width': 1, 'max-width': 1, 'min-height': 1, 'max-height': 1,
                     'counter-reset': 1, 'counter-increment': 1, 'counter-set': 1 };
        cell.style.cssText = 'display:table-cell;box-sizing:border-box;text-align:' + def[0] + ';vertical-align:' + def[1] + ';white-space:pre-wrap;';
        for (var p in d) {
            if (skip[p] || /^margin/.test(p)) continue;
            try { cell.style.setProperty(p, d[p]); } catch (e) { /* (not for it) */ }
        }
        /* (its content as a generated box's; the page counters as counters) */
        var cn = 'cyber-mb-' + (printSeq++);
        cell.className = cn;
        cell.style.counterReset = 'page ' + (printPageNo >= 0 ? printPageNo : k + 1) + ' pages ' + n + printPageCtr;
        if (!printSheet) { printSheet = document.createElement('style'); (document.head || document.documentElement).appendChild(printSheet); }
        /* (the page context's counters, after its own counter properties, as their values) */
        var loc = {};
        for (var q0 in printCtrMap) loc[q0] = printCtrMap[q0];
        printCtrList(d['counter-reset'], 0).forEach(function (p) { loc[p[0]] = p[1]; });
        printCtrList(d['counter-increment'], 1).forEach(function (p) { loc[p[0]] = (loc[p[0]] || 0) + p[1]; });
        printCtrList(d['counter-set'], 0).forEach(function (p) { loc[p[0]] = p[1]; });
        var content = String(d.content).replace(/\bcounter\(\s*([\w-]+)\s*\)|\bcounters\(\s*([\w-]+)\s*,\s*(?:"[^"]*"|'[^']*')\s*\)/g, function (m0, a, b) {
            var nm = a || b;
            return nm !== 'page' && nm !== 'pages' && Object.prototype.hasOwnProperty.call(loc, nm) ? '"' + loc[nm] + '"' : m0;
        });
        printSheet.appendChild(document.createTextNode('.' + cn + '::before{content:' + content + '}\n'));
        return cell;
    }
    function printMeasure(host, name, d, k, n) {
        /* (min and max content widths and heights of its border box) */
        var mk = function (width) {
            var c = printBoxEl(name, d, k, n);
            c.style.display = 'block';
            c.style.position = 'absolute';
            c.style.visibility = 'hidden';
            c.style.width = width;
            host.appendChild(c);
            var r = c.getBoundingClientRect();
            c.remove();
            return r;
        };
        var mn = mk('min-content'), mx = mk('max-content');
        return { minW: mn.width, maxW: mx.width, minH: mn.height, maxH: mx.height };
    }
    function printMarginSum(d, a, b, base) {
        var one = function (p, q) { var v = d[p]; if (v === undefined && d.margin !== undefined) v = marginSide(d.margin, p); return !v || v === 'auto' ? 0 : printLen(v, base); };
        return one(a) + one(b);
    }
    function printAutos(d, a, b) {
        var one = function (p) { var v = d[p]; if (v === undefined && d.margin !== undefined) v = marginSide(d.margin, p); return v === 'auto'; };
        return [one(a), one(b)];
    }
    /* (CSS Page 3 5.3.1: sizes of two groups along the available length) */
    function printDistribute(av, aMin, aMax, bMin, bMax) {
        var sumMax = aMax + bMax, sumMin = aMin + bMin;
        if (sumMax <= av) {
            var fl = av - sumMax, t = sumMax > 0 ? aMax / sumMax : 0.5;
            return aMax + fl * t;
        }
        if (sumMin < av) {
            var room = av - sumMin, dA = aMax - aMin, dB = bMax - bMin, dT = dA + dB;
            return aMin + (dT > 0 ? room * dA / dT : room / 2);
        }
        var over = sumMin - av, tm = sumMin > 0 ? aMin / sumMin : 0.5;
        return aMin - over * tm;
    }
    function printRow(host, names, decl, avail, horizontal, k, n, base) {
        /* (the used main size of each of the three, 0: no box) */
        var sz = [0, 0, 0], fix = [null, null, null], ms = [], outer = [0, 0, 0];
        for (var i = 0; i < 3; i++) {
            var d = decl[names[i]];
            if (!printHasBox(d)) { ms[i] = null; continue; }
            var pr = horizontal ? 'width' : 'height';
            var mar = horizontal ? printMarginSum(d, 'margin-left', 'margin-right', base) : printMarginSum(d, 'margin-top', 'margin-bottom', base);
            outer[i] = mar;
            var m = printMeasure(host, names[i], d, k, n);
            ms[i] = horizontal ? { mn: m.minW + mar, mx: m.maxW + mar } : { mn: m.minH + mar, mx: m.maxH + mar };
            if (!horizontal) ms[i].mn = ms[i].mx;   /* (heights: the content's) */
            if (d[pr] !== undefined && d[pr] !== 'auto' && !/content/.test(d[pr])) {
                var fv = printLen(d[pr], base);
                var mnv = d['min-' + pr], mxv = d['max-' + pr];
                if (mxv !== undefined && mxv !== 'none') fv = Math.min(fv, printLen(mxv, base));
                if (mnv !== undefined) fv = Math.max(fv, printLen(mnv, base));
                if (d['box-sizing'] !== 'border-box') fv += (horizontal ? m.maxW : m.maxH) - printContentSize(host, names[i], d, k, n, horizontal);
                fix[i] = fv + mar;
                ms[i].mn = ms[i].mx = fix[i];
            }
        }
        var A = ms[0], C = ms[1], B = ms[2];
        if (C) {
            if (fix[1] !== null) sz[1] = fix[1];
            else {
                var sideMin = 2 * Math.max(A ? A.mn : 0, B ? B.mn : 0), sideMax = 2 * Math.max(A ? A.mx : 0, B ? B.mx : 0);
                /* (sides all sized: the centre has the rest between them) */
                if ((A || B) && (!A || fix[0] !== null) && (!B || fix[2] !== null)) sz[1] = Math.max(0, avail - sideMax);
                else sz[1] = printDistribute(avail, C.mn, C.mx, sideMin, sideMax);
            }
            var rest = (avail - sz[1]) / 2;
            if (A) sz[0] = fix[0] !== null ? fix[0] : rest;
            if (B) sz[2] = fix[2] !== null ? fix[2] : rest;
        } else if (A && B) {
            if (fix[0] !== null && fix[2] !== null) { sz[0] = fix[0]; sz[2] = fix[2]; }
            else if (fix[0] !== null) { sz[0] = fix[0]; sz[2] = avail - sz[0]; }
            else if (fix[2] !== null) { sz[2] = fix[2]; sz[0] = avail - sz[2]; }
            else { sz[0] = printDistribute(avail, A.mn, A.mx, B.mn, B.mx); sz[2] = avail - sz[0]; }
        } else if (A) { sz[0] = fix[0] !== null ? fix[0] : avail; }
        else if (B) { sz[2] = fix[2] !== null ? fix[2] : avail; }
        return { sz: sz, has: [!!A, !!C, !!B] };
    }
    function printContentSize(host, name, d, k, n, horizontal) {
        var c = printBoxEl(name, d, k, n);
        c.style.display = 'block';
        c.style.position = 'absolute';
        c.style.visibility = 'hidden';
        c.style.width = 'max-content';
        host.appendChild(c);
        var cs = getComputedStyle(c);
        var v = horizontal ? parseFloat(cs.borderLeftWidth) + parseFloat(cs.borderRightWidth) + parseFloat(cs.paddingLeft) + parseFloat(cs.paddingRight)
                           : parseFloat(cs.borderTopWidth) + parseFloat(cs.borderBottomWidth) + parseFloat(cs.paddingTop) + parseFloat(cs.paddingBottom);
        var r = horizontal ? c.getBoundingClientRect().width : c.getBoundingClientRect().height;
        c.remove();
        return r - v;
    }
    /* (a margin box placed in its slot (x, y, w, h of its margin box)) */
    function printPlace(host, name, d, x, y, w, h, k, n, base) {
        var ml = printMarginSum(d, 'margin-left', 'margin-left', base) / 2, mr = printMarginSum(d, 'margin-right', 'margin-right', base) / 2;
        var mt = printMarginSum(d, 'margin-top', 'margin-top', base) / 2, mb = printMarginSum(d, 'margin-bottom', 'margin-bottom', base) / 2;
        var aw = printAutos(d, 'margin-left', 'margin-right'), ah = printAutos(d, 'margin-top', 'margin-bottom');
        var bw = w - ml - mr, bh = h - mt - mb;
        var wrap = document.createElement('div'), cell = printBoxEl(name, d, k, n);
        var kw = function (v) { return /content/.test(v); };
        var cw = d.width !== undefined && d.width !== 'auto' && /corner|^left|^right/.test(name) ? (kw(d.width) ? printMeasure(host, name, d, k, n).maxW - printContentSize(host, name, d, k, n, true) : printLen(d.width, w)) : null;
        var ch = d.height !== undefined && d.height !== 'auto' && /corner|^top|^bottom/.test(name) ? (kw(d.height) ? printMeasure(host, name, d, k, n).maxH - printContentSize(host, name, d, k, n, false) : printLen(d.height, h)) : null;
        if (cw !== null && d['box-sizing'] !== 'border-box') cw += printMeasure(host, name, d, k, n).maxW - printContentSize(host, name, d, k, n, true);
        if (ch !== null && d['box-sizing'] !== 'border-box') ch += printMeasure(host, name, d, k, n).maxH - printContentSize(host, name, d, k, n, false);
        var bx = x + ml, by = y + mt;
        if (cw !== null) {
            var fw = w - ml - mr - cw;
            if (aw[0] && aw[1]) bx = x + fw / 2;
            else if (aw[0] || (!aw[1] && /^left|left-corner$/.test(name))) bx = x + fw + ml;   /* (overconstrained: against the page area) */
            bw = cw;
        }
        if (ch !== null) {
            var fh = h - mt - mb - ch;
            if (ah[0] && ah[1]) by = y + fh / 2;
            else if (ah[0] || (!ah[1] && /^top/.test(name))) by = y + fh + mt;
            bh = ch;
        }
        wrap.style.cssText = 'position:absolute;display:table;table-layout:fixed;left:' + bx + 'px;top:' + by + 'px;width:' + bw + 'px;height:' + bh + 'px;';
        cell.style.width = bw + 'px';
        cell.style.height = bh + 'px';
        wrap.appendChild(cell);
        host.appendChild(wrap);
    }
    function printBuild() {
        var rules = [];
        for (var i = 0; i < document.styleSheets.length; i++) { try { printRules(document.styleSheets[i].cssRules, rules); } catch (e) { /* (cross origin) */ } }
        var firstEl = document.body && document.body.firstElementChild, firstName = '';
        for (var fe = firstEl; fe; fe = fe.firstElementChild) { var pn = getComputedStyle(fe).page; if (pn && pn !== 'auto') { firstName = pn; break; } }
        var first = printPageRules(rules, firstName, 0), size = printSize(first), m0 = printMetrics(first, size[0], size[1]);
        var pw = size[0], ph = size[1], de = document.documentElement;
        /* (the page box's own border and padding take room from the content) */
        var extra = function (p) { var o = 0; ['border-' + p + '-width', 'padding-' + p].forEach(function (q) { var v = printDecl(first, q); if (v) o += printLen(v, pw); }); return o; };
        var probe = document.createElement('div');
        var bt = 0, br = 0, bb = 0, bl = 0;
        first.forEach(function (r) {
            probe.style.cssText = r.style.cssText.replace(/\bsize\s*:[^;]*;?/g, '').replace(/\bmargin[^:]*:[^;]*;?/g, '');
        });
        probe.style.position = 'absolute';
        probe.style.visibility = 'hidden';
        de.appendChild(probe);
        var pcs = getComputedStyle(probe);
        bt = parseFloat(pcs.borderTopWidth) + parseFloat(pcs.paddingTop);
        br = parseFloat(pcs.borderRightWidth) + parseFloat(pcs.paddingRight);
        bb = parseFloat(pcs.borderBottomWidth) + parseFloat(pcs.paddingBottom);
        bl = parseFloat(pcs.borderLeftWidth) + parseFloat(pcs.paddingLeft);
        probe.remove();
        var cw = Math.max(1, pw - m0.left - m0.right - bl - br), chh = Math.max(1, ph - m0.top - m0.bottom - bt - bb);
        /* (a width or height of the page box: its area that big; the overconstrained margin gives) */
        var wd = printDecl(first, 'width'), hd = printDecl(first, 'height');
        if (wd && wd !== 'auto') { var wv = printLen(wd, pw); m0.right += cw - wv; cw = wv; }
        if (hd && hd !== 'auto') { var hv = printLen(hd, ph); m0.bottom += chh - hv; chh = hv; }
        __cyberPrintSet(cw, chh, m0.top + m0.bottom + bt + bb, m0.left + bl, m0.top + bt, pw);
        var tries = 0;
        function pages() {
            /* (from the root's height, once laid out as pages) */
            var n = Math.max(1, __cyberPrintPages());
            if (n > 40) n = 40;
            var hostOf = function (z) {
                var hh = document.createElement('div');
                hh.setAttribute('data-cyber-print', '');
                hh.style.cssText = 'all:initial;position:fixed;left:0;top:0;width:0;height:0;z-index:' + z + ';';
                var inh = {};
                ['font', 'font-family', 'font-size', 'font-weight', 'font-style', 'line-height', 'color', 'white-space', 'text-align', 'direction', 'letter-spacing', 'word-spacing', 'quotes'].forEach(function (q) {
                    var v = printDecl(first, q);
                    if (v) hh.style.setProperty(q, v);
                });
                de.appendChild(hh);
                return hh;
            };
            var host = hostOf(999999000), under = hostOf(-999999000), sheet = hostOf(-999999001);
            /* (the page context's counters: from the root's counter-reset, then each page's) */
            var ctr = {}, pno = 0;
            try { printCtrList(getComputedStyle(de).counterReset, 0).forEach(function (p) { if (p[0] !== 'page' && p[0] !== 'pages') ctr[p[0]] = p[1]; }); } catch (e) { /* (none) */ }
            for (var k = 0; k < n; k++) {
                var rs = printPageRules(rules, '', k), m = printMetrics(rs, pw, ph);
                var pinc = 1;
                printCtrList(printDecl(rs, 'counter-reset'), 0).forEach(function (p) { if (p[0] === 'page') pno = p[1]; else ctr[p[0]] = p[1]; });
                printCtrList(printDecl(rs, 'counter-increment'), 1).forEach(function (p) { if (p[0] === 'page') pinc = p[1]; else ctr[p[0]] = (ctr[p[0]] || 0) + p[1]; });
                pno += pinc;
                printCtrList(printDecl(rs, 'counter-set'), 0).forEach(function (p) { if (p[0] === 'page') pno = p[1]; else ctr[p[0]] = p[1]; });
                printPageNo = pno;
                printCtrMap = ctr;
                printPageCtr = '';
                for (var cn0 in ctr) if (cn0 !== 'page' && cn0 !== 'pages') printPageCtr += ' ' + cn0 + ' ' + ctr[cn0];
                var px = -(m0.left + bl), py = k * ph - (m0.top + bt);
                var box = document.createElement('div');
                /* (the page box's decorations, under nothing else of it) */
                var deco = '';
                rs.forEach(function (r) { deco += r.style.cssText.replace(/\bsize\s*:[^;]*;?/g, '').replace(/\bmargin[^:]*:[^;]*;?/g, '').replace(/\b(page-orientation|marks|bleed|width|height)\s*:[^;]*;?/g, '') + ';'; });
                if (deco.replace(/;/g, '').trim()) {
                    box.style.cssText = deco + ';position:absolute;box-sizing:border-box;left:' + (px + m.left) + 'px;top:' + (py + m.top) + 'px;width:' + (pw - m.left - m.right) + 'px;height:' + (ph - m.top - m.bottom) + 'px;';
                    under.appendChild(box);
                    /* (its colour: the whole sheet's, under the canvas's; the engine paints it from this hidden box) */
                    var bgc = box.style.backgroundColor;
                    if (bgc && bgc !== 'transparent') {
                        box.style.backgroundColor = 'transparent';
                        var sh = document.createElement('div');
                        sh.style.cssText = 'position:absolute;visibility:hidden;left:' + px + 'px;top:' + py + 'px;width:' + pw + 'px;height:' + ph + 'px;';
                        sh.style.backgroundColor = bgc;
                        sheet.appendChild(sh);
                    }
                }
                var d = printMarginDecls(rs);
                var cwm = pw - m.left - m.right, chm = ph - m.top - m.bottom;
                [['top-left-corner', 0, 0, m.left, m.top], ['top-right-corner', pw - m.right, 0, m.right, m.top],
                 ['bottom-left-corner', 0, ph - m.bottom, m.left, m.bottom], ['bottom-right-corner', pw - m.right, ph - m.bottom, m.right, m.bottom]].forEach(function (c) {
                    if (printHasBox(d[c[0]])) printPlace(host, c[0], d[c[0]], px + c[1], py + c[2], c[3], c[4], k, n, pw);
                });
                [['top', px + m.left, py, cwm, m.top, true], ['bottom', px + m.left, py + ph - m.bottom, cwm, m.bottom, true],
                 ['left', px, py + m.top, m.left, chm, false], ['right', px + pw - m.right, py + m.top, m.right, chm, false]].forEach(function (e) {
                    var names = MB_ROWS[e[0]], horiz = e[5];
                    var res = printRow(host, names, d, horiz ? e[3] : e[4], horiz, k, n, horiz ? cwm : chm);
                    var at = horiz ? e[1] : e[2];
                    for (var q = 0; q < 3; q++) {
                        if (!res.has[q]) continue;
                        var s = res.sz[q], pos;
                        if (q === 0) pos = at;
                        else if (q === 2) pos = at + (horiz ? e[3] : e[4]) - s;
                        else pos = at + ((horiz ? e[3] : e[4]) - s) / 2;
                        if (horiz) printPlace(host, names[q], d[names[q]], pos, e[2], s, e[4], k, n, cwm);
                        else printPlace(host, names[q], d[names[q]], e[1], pos, e[3], s, k, n, chm);
                    }
                });
            }
        }
        requestAnimationFrame(function () { requestAnimationFrame(function () { requestAnimationFrame(function () {
            try { pages(); } catch (e) { console.error('print: ' + e); }
            global.__cyberPrintPending = false;
        }); }); });
    }
    Object.defineProperty(global, '__cyberPrintBegin', { configurable: true, writable: true, value: function () {
        global.__cyberPrintPending = true;
        if (document.readyState === 'complete') printBuild();
        else addEventListener('load', printBuild);
    } });

    Object.defineProperty(global, '__cyberAccessibility', { configurable: true, value: Object.freeze({
        role: function (el) { return el && el.nodeType === 1 ? axRoleOnly(el) : ''; },
        label: function (el) { return el && el.nodeType === 1 ? axName(el) : ''; },
    }) });
})

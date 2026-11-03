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

    /* (css-font-loading: a font list load() and check() can parse) */
    Object.defineProperty(global, '__cyberFontSpecError', { configurable: true, value: function (spec) {
        spec = String(spec);
        var bad = /var\(/i.test(spec) || /^\s*(initial|inherit|unset|revert|revert-layer|default)\s*$/i.test(spec);
        return bad ? new global.DOMException("Could not parse the font list '" + spec + "'", 'SyntaxError') : null;
    } });
    /* (document.fonts' events: the faces the sheets declare have loaded by the
     * time the page has, for a page that listens) */
    if (global.addEventListener) global.addEventListener('load', function () {
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
    });

    /* The flat tree, in order (Selectors: the snap targets in a shadow tree or
     * slotted count where they show). */
    function flatKids(n) {
        if (n.shadowRoot) return n.shadowRoot.children;
        if (n.localName === 'slot' && n.assignedElements) {
            var as = n.assignedElements();
            if (as.length) return as;
        }
        return n.children;
    }
    function flatWalk(n, fn) {
        var k = flatKids(n);
        for (var i = 0; i < k.length; i++) if (fn(k[i]) !== false) flatWalk(k[i], fn);
    }
    function isScrollBox(cs) { return /auto|scroll|hidden/.test(cs.overflowX + ' ' + cs.overflowY); }

    /* Scroll snap (CSS Scroll Snap 1 and 2): for each box, the targets it is
     * snapped to on each axis (those at the same position, in flat tree order)
     * and the offset they were for. */
    var snapStates = new WeakMap(), snapReported = new WeakMap();
    function snapAxes(cs) {
        var type = cs.scrollSnapType;
        if (!type || type === 'none') return null;
        return { x: /\b(x|inline|both|pair)\b/.test(type), y: /\b(y|block|both|pair)\b/.test(type), proximity: /proximity/.test(type) };
    }
    /* Each axis's snap positions, { t: the target, pos: the offset }. */
    function snapLists(el, axes) {
        var r = el.getBoundingClientRect(), left = r.left + el.clientLeft, top = r.top + el.clientTop;
        var sx = el.scrollLeft, sy = el.scrollTop, cw = el.clientWidth, ch = el.clientHeight;
        var maxX = el.scrollWidth - cw, maxY = el.scrollHeight - ch, out = { x: [], y: [] };
        var at = function (align, lo, hi, view) { return align === 'start' ? lo : align === 'end' ? hi - view : (lo + hi - view) / 2; };
        flatWalk(el, function (c) {
            var cs = global.getComputedStyle(c), align = cs.scrollSnapAlign;
            if (align && align !== 'none' && c.getClientRects().length) {
                var parts = align.split(' '), block = parts[0], inline = parts[1] || parts[0], b = c.getBoundingClientRect();
                if (axes.x && inline !== 'none')
                    out.x.push({ t: c, pos: Math.max(0, Math.min(maxX, at(inline, b.left - left + sx, b.right - left + sx, cw))) });
                if (axes.y && block !== 'none')
                    out.y.push({ t: c, pos: Math.max(0, Math.min(maxY, at(block, b.top - top + sy, b.bottom - top + sy, ch))) });
            }
            /* (a nested scroller's targets are its own) */
            return !isScrollBox(cs);
        });
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
    function snapReport(el, st) {
        var block = snapPreferred(st.y), inline = snapPreferred(st.x), was = snapReported.get(el);
        if (was && was.block === block && was.inline === inline) return;
        snapReported.set(el, { block: block, inline: inline });
        if (block || inline)
            global.__cyberDispatchTrusted(el, new global.SnapEvent('scrollsnapchange', { snapTargetBlock: block, snapTargetInline: inline }));
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
        var axes = snapAxes(global.getComputedStyle(el));
        if (!axes) return false;
        var sx = el.scrollLeft, sy = el.scrollTop, lists = snapLists(el, axes);
        var bestX = snapNearest(lists.x, sx), bestY = snapNearest(lists.y, sy);
        if (axes.proximity) {
            if (bestX !== null && Math.abs(bestX - sx) > el.clientWidth / 3) bestX = null;
            if (bestY !== null && Math.abs(bestY - sy) > el.clientHeight / 3) bestY = null;
        }
        var moved = false;
        if (bestX !== null && Math.abs(bestX - sx) >= 1) { el.scrollLeft = bestX; moved = true; }
        if (bestY !== null && Math.abs(bestY - sy) >= 1) { el.scrollTop = bestY; moved = true; }
        var st = snapRemember(el, lists, bestX, bestY);
        if (!moved) snapReport(el, st);
        return moved;
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
    var cvSkipped = new WeakMap();
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
    Object.defineProperty(global, '__cyberRenderTick', { configurable: true, value: function (features, scrollers) {
        var root = global.document && global.document.documentElement, snappers = [], changed = [];
        if (!root) return;
        if (scrollers) {
            for (var s = 0; s < scrollers.length; s++) {
                var ax = snapAxes(global.getComputedStyle(scrollers[s]));
                if (ax) snapFollow(scrollers[s], ax);
            }
            return;
        }
        flatWalk(root, function (c) {
            var cs = global.getComputedStyle(c);
            if (features & 1) {
                var axes = snapAxes(cs);
                if (axes) snappers.push(c, axes);
                else if (snapStates.has(c)) snapStates.delete(c);
            }
            if ((features & 2) && cs.contentVisibility === 'auto' && c.getClientRects().length) {
                var skipped = !cvRelevant(c);
                if (cvSkipped.get(c) !== skipped) { cvSkipped.set(c, skipped); changed.push(c, skipped); }
            }
            if ((features & 4) && cs.scrollInitialTarget === 'nearest' && !initialDone.has(c) && c.getClientRects().length) {
                initialDone.add(c);
                scrollToInitialTarget(c);
            }
        });
        for (var i = 0; i < snappers.length; i += 2) snapFollow(snappers[i], snappers[i + 1]);
        for (var k = 0; k < changed.length; k += 2)
            global.__cyberDispatchTrusted(changed[k], new global.ContentVisibilityAutoStateChangeEvent('contentvisibilityautostatechange', { skipped: changed[k + 1] }));
    } });

    /* (HTMLMetaElement reflects its attributes) */
    if (global.HTMLMetaElement) [['name', 'name'], ['content', 'content'], ['httpEquiv', 'http-equiv'], ['media', 'media'], ['scheme', 'scheme']].forEach(function (m) {
        Object.defineProperty(global.HTMLMetaElement.prototype, m[0], { configurable: true, enumerable: true,
            get: function () { var v = this.getAttribute(m[1]); return v === null ? '' : v; },
            set: function (v) { this.setAttribute(m[1], String(v)); } });
    });

    /* ---- View transitions: the API and its promises; nothing is captured
     * or animated, the update simply happens ---- */
    function ViewTransition() { throw new TypeError('Illegal constructor'); }
    var newViewTransition = function (update, types) {
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
            else resolveReady();
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
                var vt = newViewTransition(function () { return update ? update() : undefined; }, types);
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
        Object.defineProperty(global, '__cyberPageSwap', { configurable: true, value: function (url, same) {
            var rule = viewTransitionRule();
            var vt = same && rule && rule.navigation === 'auto' ? newViewTransition(nothing, rule.types) : null;
            var activation = { entry: { url: url }, from: { url: global.location.href }, navigationType: 'replace' };
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

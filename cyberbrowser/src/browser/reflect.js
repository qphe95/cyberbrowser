/* HTML's reflecting IDL attributes (HTML 2.6.1), from one table (reflect_table.js): each
 * getter and setter here is the standard's rule for its type over the content attribute. */
(function (global) {
    'use strict';
    var TABLE = global.__cyberReflectTable;
    global.__cyberReflectTable = undefined;
    if (!TABLE) return;
    var IFACE = {
        a: 'HTMLAnchorElement', area: 'HTMLAreaElement', audio: 'HTMLAudioElement', base: 'HTMLBaseElement', blockquote: 'HTMLQuoteElement',
        body: 'HTMLBodyElement', br: 'HTMLBRElement', button: 'HTMLButtonElement', canvas: 'HTMLCanvasElement', caption: 'HTMLTableCaptionElement',
        col: 'HTMLTableColElement', colgroup: 'HTMLTableColElement', data: 'HTMLDataElement', datalist: 'HTMLDataListElement', del: 'HTMLModElement',
        details: 'HTMLDetailsElement', dialog: 'HTMLDialogElement', dir: 'HTMLDirectoryElement', div: 'HTMLDivElement', dl: 'HTMLDListElement',
        embed: 'HTMLEmbedElement', fieldset: 'HTMLFieldSetElement', font: 'HTMLFontElement', form: 'HTMLFormElement', frame: 'HTMLFrameElement',
        frameset: 'HTMLFrameSetElement', h1: 'HTMLHeadingElement', h2: 'HTMLHeadingElement', h3: 'HTMLHeadingElement', h4: 'HTMLHeadingElement',
        h5: 'HTMLHeadingElement', h6: 'HTMLHeadingElement', head: 'HTMLHeadElement', hr: 'HTMLHRElement', html: 'HTMLHtmlElement',
        iframe: 'HTMLIFrameElement', img: 'HTMLImageElement', input: 'HTMLInputElement', ins: 'HTMLModElement', label: 'HTMLLabelElement',
        legend: 'HTMLLegendElement', li: 'HTMLLIElement', link: 'HTMLLinkElement', map: 'HTMLMapElement', marquee: 'HTMLMarqueeElement',
        usermedia: 'HTMLUserMediaElement', geolocation: 'HTMLGeolocationElement', camera: 'HTMLCameraElement', microphone: 'HTMLMicrophoneElement',
        install: 'HTMLInstallElement', menu: 'HTMLMenuElement', menubar: 'HTMLMenuBarElement', menulist: 'HTMLMenuListElement', submenu: 'HTMLSubMenuElement',
        menuitem: 'HTMLMenuItemElement', menuitemcheckbox: 'HTMLMenuItemCheckboxElement', menuitemradio: 'HTMLMenuItemRadioElement', meta: 'HTMLMetaElement', meter: 'HTMLMeterElement', object: 'HTMLObjectElement', ol: 'HTMLOListElement',
        optgroup: 'HTMLOptGroupElement', option: 'HTMLOptionElement', output: 'HTMLOutputElement', p: 'HTMLParagraphElement', param: 'HTMLParamElement',
        picture: 'HTMLPictureElement', pre: 'HTMLPreElement', progress: 'HTMLProgressElement', q: 'HTMLQuoteElement', script: 'HTMLScriptElement',
        select: 'HTMLSelectElement', slot: 'HTMLSlotElement', source: 'HTMLSourceElement', span: 'HTMLSpanElement', style: 'HTMLStyleElement',
        table: 'HTMLTableElement', tbody: 'HTMLTableSectionElement', td: 'HTMLTableCellElement', template: 'HTMLTemplateElement', textarea: 'HTMLTextAreaElement',
        tfoot: 'HTMLTableSectionElement', th: 'HTMLTableCellElement', thead: 'HTMLTableSectionElement', time: 'HTMLTimeElement', title: 'HTMLTitleElement',
        tr: 'HTMLTableRowElement', track: 'HTMLTrackElement', ul: 'HTMLUListElement', video: 'HTMLVideoElement'
    };
    /* (the element's own idea of these: not plain reflection) */
    var SKIP = { 'button.type': 1, 'img.width': 1, 'img.height': 1, 'canvas.width': 1, 'canvas.height': 1, 'input.width': 1, 'input.height': 1,
                 'option.value': 1, 'option.label': 1, 'title.text': 1, 'script.text': 1, 'link.nonce': 1, 'style.nonce': 1, 'input.type': 1 };
    /* (setting these starts something the element's own setter knows about) */
    var KEEP_SET = { src: 1, srcset: 1, srcdoc: 1, data: 1, href: 1, type: 1 };
    var MAX = 2147483647;
    var WS = /^[ \t\n\f\r]*/;
    function parseInteger(s) {
        var m = /^[ \t\n\f\r]*([-+]?)(\d+)/.exec(s);
        if (!m) return null;
        var v = parseFloat(m[2]);
        return m[1] === '-' && v ? -v : v;   /* (never -0) */
    }
    function parseNonneg(s) {
        var v = parseInteger(s);
        return v === null || v < 0 ? (v === 0 ? 0 : null) : v;
    }
    function parseDouble(s) {
        /* (rules for parsing floating-point number values: '1.' and '1.e2' are numbers too) */
        var m = /^[ \t\n\f\r]*(-?)(\d+\.?\d*|\.\d+)(?:([eE][-+]?\d+))?/.exec(s);
        if (!m) return null;
        var v = parseFloat(m[2].replace(/\.$/, '') + (m[3] || ''));
        if (m[1] && v) v = -v;
        return isFinite(v) ? v : null;
    }
    function indexError(name) { return new global.DOMException("The value provided for '" + name + "' is out of range.", 'IndexSizeError'); }
    function lower(s) { return s.replace(/[A-Z]/g, function (c) { return c.toLowerCase(); }); }
    var get = Element.prototype.getAttribute, set = Element.prototype.setAttribute, remove = Element.prototype.removeAttribute;
    function accessor(prop, d, attr, native) {
        var dflt = Array.isArray(d.defaultVal) ? d.defaultVal[d.defaultVal.length - 1] : d.defaultVal;   /* (an implementation-defined default from a list: one of them) */
        switch (d.type) {
        case 'string':
            return { get: function () { var v = get.call(this, attr); return v === null ? '' : v; },
                     set: function (v) { set.call(this, attr, v === null && d.treatNullAsEmptyString ? '' : String(v)); } };
        case 'url':
            return { get: function () {
                         var v = get.call(this, attr);
                         if (v === null) return attr === 'action' || attr === 'formaction' ? this.ownerDocument.URL : '';
                         /* (non-ASCII in its query: in the document's encoding, URL 4.4's encoding override) */
                         if (/[^\x00-\x7f]/.test(v) && typeof global.__cyberLegacyQuery === 'function') {
                             var od = this.ownerDocument, cs = od && od.characterSet;
                             var lq = cs && cs !== 'UTF-8' ? global.__cyberLegacyQuery(v, cs) : null;
                             if (lq) v = lq;
                         }
                         try { return new global.URL(v, this.baseURI).href; } catch (e) { return v; }
                     },
                     set: function (v) { set.call(this, attr, String(v)); } };
        case 'boolean':
            return { get: function () { return get.call(this, attr) !== null; },
                     set: function (v) { if (v) set.call(this, attr, ''); else remove.call(this, attr); } };
        case 'enum': {
            var missing = dflt === undefined ? (d.isNullable ? null : '') : dflt, invalid = d.invalidVal === undefined ? missing : d.invalidVal, kw = {};
            /* (ARIA's, nullable: absent, null; its defaults are for a value there) */
            if (d.isNullable && /^aria-/.test(attr)) missing = null;
            d.keywords.forEach(function (k) { kw[lower(k)] = k; });
            var canon = d.nonCanon || {};
            return { get: function () {
                         var v = get.call(this, attr);
                         if (v === null) return missing;
                         var low = lower(v), out = Object.prototype.hasOwnProperty.call(kw, low) ? kw[low] : invalid;
                         return out !== null && typeof canon[out] !== 'undefined' ? canon[out] : out;
                     },
                     set: function (v) {
                         if ((v === null || v === undefined) && d.isNullable) remove.call(this, attr);
                         else set.call(this, attr, String(v));
                     } };
        }
        case 'long': {
            var dl = dflt === undefined || dflt === null ? 0 : dflt;
            return { get: function () {
                         var s = get.call(this, attr), v = s === null ? null : parseInteger(s);
                         return v === null || v > MAX || v < -MAX - 1 ? dl : v;
                     },
                     set: function (v) { set.call(this, attr, String(v | 0)); } };
        }
        case 'limited long': {
            var dn = dflt === undefined ? -1 : dflt;
            return { get: function () {
                         var s = get.call(this, attr), v = s === null ? null : parseNonneg(s);
                         return v === null || v > MAX ? dn : v;
                     },
                     set: function (v) {
                         var n = v | 0;
                         if (n < 0) throw indexError(prop);
                         set.call(this, attr, String(n));
                     } };
        }
        case 'unsigned long': {
            var du = dflt === undefined ? 0 : dflt;
            return { get: function () {
                         var s = get.call(this, attr), v = s === null ? null : parseNonneg(s);
                         return v === null || v > MAX ? du : v;
                     },
                     set: function (v) { var n = v >>> 0; set.call(this, attr, String(n > MAX ? du : n)); } };
        }
        case 'limited unsigned long':
        case 'limited unsigned long with fallback': {
            var fall = d.type !== 'limited unsigned long', dp = dflt === undefined ? 1 : dflt;
            return { get: function () {
                         var s = get.call(this, attr), v = s === null ? null : parseNonneg(s);
                         return v === null || v > MAX || v === 0 ? dp : v;
                     },
                     set: function (v) {
                         var n = v >>> 0;
                         if (n === 0 && !fall) throw indexError(prop);
                         set.call(this, attr, String(n === 0 || n > MAX ? dp : n));
                     } };
        }
        case 'clamped unsigned long': {
            var dc = dflt === undefined ? 0 : dflt, lo = d.min, hi = d.max;
            return { get: function () {
                         var s = get.call(this, attr), v = s === null ? null : parseNonneg(s);
                         if (v === null) return dc;
                         return v < lo ? lo : v > hi ? hi : v;
                     },
                     set: function (v) { var n = v >>> 0; set.call(this, attr, String(n > MAX ? dc : n)); } };
        }
        case 'double':
        case 'limited double': {
            var lim = d.type !== 'double', dd = dflt === undefined ? (lim ? 1 : 0) : dflt;
            return { get: function () {
                         var s = get.call(this, attr), v = s === null ? null : parseDouble(s);
                         return v === null || (lim && v <= 0) ? dd : v;
                     },
                     set: function (v) {
                         var n = Number(v);
                         if (!isFinite(n)) throw new TypeError("The value provided for '" + prop + "' is not a finite number.");
                         if (lim && n <= 0) return;
                         set.call(this, attr, String(n));
                     } };
        }
        }
        return null;
    }
    function install(proto, tag, prop, d) {
        if (typeof d.type === 'object') { var inner = d.type; d = Object.assign({}, d, inner); }
        if (d.customGetter || SKIP[tag + '.' + prop]) return;
        var attr = d.domAttrName === undefined ? lower(prop) : d.domAttrName;
        var native = Object.getOwnPropertyDescriptor(proto, prop);
        var a = accessor(prop, d, attr, native);
        if (!a) return;
        if (KEEP_SET[prop] && native && native.set) a.set = native.set;
        Object.defineProperty(proto, prop, { configurable: true, enumerable: true, get: a.get, set: a.set });
    }
    var done = {};
    Object.keys(TABLE).forEach(function (tag) {
        var name = tag === '__aria' ? 'Element' : IFACE[tag] || 'HTMLElement', C = global[name];
        if (typeof C !== 'function' || !C.prototype) return;
        Object.keys(TABLE[tag]).forEach(function (prop) {
            var key = name + '.' + prop;
            if (done[key]) return;
            done[key] = 1;
            install(C.prototype, tag, prop, TABLE[tag][prop]);
        });
    });
    /* (ARIA's strings: null when absent) */
    ['ariaBrailleLabel', 'ariaBrailleRoleDescription', 'ariaColCount', 'ariaColIndex', 'ariaColIndexText', 'ariaColSpan', 'ariaDescription',
     'ariaKeyShortcuts', 'ariaLabel', 'ariaLevel', 'ariaPlaceholder', 'ariaPosInSet', 'ariaRoleDescription', 'ariaRowCount', 'ariaRowIndex',
     'ariaRowIndexText', 'ariaRowSpan', 'ariaSetSize', 'ariaValueMax', 'ariaValueMin', 'ariaValueNow', 'ariaValueText', 'role'].forEach(function (p) {
        var at = p === 'role' ? 'role' : 'aria-' + p.slice(4).toLowerCase();
        Object.defineProperty(Element.prototype, p, { configurable: true, enumerable: true,
            get: function () { return get.call(this, at); },
            set: function (v) { if (v === null || v === undefined) remove.call(this, at); else set.call(this, at, String(v)); } });
    });
    /* popovertarget: the element, by id or as set; what the button does to it */
    var targets = new WeakMap();
    [global.HTMLButtonElement, global.HTMLInputElement].forEach(function (C) {
        if (!C) return;
        install(C.prototype, 'button', 'popoverTargetAction', { type: 'enum', keywords: ['toggle', 'show', 'hide'], defaultVal: 'toggle', invalidVal: 'toggle', domAttrName: 'popovertargetaction' });
        Object.defineProperty(C.prototype, 'popoverTargetElement', { configurable: true, enumerable: true,
            get: function () {
                var set_ = targets.get(this);
                if (set_) {
                    /* (only one in this tree, or a tree that contains it) */
                    if (!this.isConnected || !set_.isConnected) return null;
                    for (var r = this.getRootNode(); r; r = r.host ? r.host.getRootNode() : null) if (set_.getRootNode() === r) return set_;
                    return null;
                }
                var id = get.call(this, 'popovertarget');
                if (id === null || !this.isConnected) return null;
                var rt = this.getRootNode();
                return rt.getElementById ? rt.getElementById(id) : null;
            },
            set: function (v) {
                if (v === null || v === undefined) { targets.delete(this); remove.call(this, 'popovertarget'); return; }
                if (!(v instanceof global.Element)) throw new TypeError("Failed to set 'popoverTargetElement': not an Element.");
                targets.set(this, v);
                set.call(this, 'popovertarget', '');
            } });
    });
    /* (on every HTML element, the title, lang, dir, class, autofocus, hidden, inert and accesskey) */
    var HP = global.HTMLElement && global.HTMLElement.prototype;
    if (HP) {
        var G = { title: { type: 'string' }, lang: { type: 'string' }, accessKey: { type: 'string' },
                  dir: { type: 'enum', keywords: ['ltr', 'rtl', 'auto'] }, autofocus: { type: 'boolean' }, inert: { type: 'boolean' } };
        Object.keys(G).forEach(function (p) { install(HP, 'html', p, G[p]); });
        install(global.Element.prototype, 'element', 'className', { type: 'string', domAttrName: 'class' });
    }
    /* <meter>'s numbers (HTML 4.10.15): each attribute's value, kept in the range the others make */
    var MP_ = global.HTMLMeterElement && global.HTMLMeterElement.prototype;
    if (MP_) {
        /* (a leading "+" allowed, as engines do) */
        var num = function (el, a, d) { var s = get.call(el, a); if (s !== null && /^\+[0-9.]/.test(s)) s = s.slice(1); var v = s === null ? null : parseDouble(s); return v === null ? d : v; };
        var mmin = function (el) { return num(el, 'min', 0); };
        var mmax = function (el) { var lo = mmin(el), v = num(el, 'max', 1); return v < lo ? lo : v; };
        var clampTo = function (v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; };
        var M = {
            min: mmin, max: mmax,
            value: function (el) { return clampTo(num(el, 'value', 0), mmin(el), mmax(el)); },
            low: function (el) { var lo = mmin(el), hi = mmax(el); return clampTo(num(el, 'low', lo), lo, hi); },
            high: function (el) { var lo = mmin(el), hi = mmax(el), low = clampTo(num(el, 'low', lo), lo, hi); return clampTo(num(el, 'high', hi), low, hi); },
            optimum: function (el) { var lo = mmin(el), hi = mmax(el); return clampTo(num(el, 'optimum', (lo + hi) / 2), lo, hi); },
        };
        Object.keys(M).forEach(function (k) {
            Object.defineProperty(MP_, k, { configurable: true, enumerable: true,
                get: function () { return M[k](this); },
                set: function (v) {
                    var n = Number(v);
                    if (!isFinite(n)) throw new TypeError("Failed to set the '" + k + "' property on 'HTMLMeterElement': The provided double value is non-finite.");
                    set.call(this, k, String(n));
                } });
        });
    }
    /* <progress>'s numbers (HTML 4.10.14): max a positive number, else 1; value within 0..max, 0 when not one; position -1 when indeterminate */
    var PP_ = global.HTMLProgressElement && global.HTMLProgressElement.prototype;
    if (PP_) {
        var pnum = function (el, a) { var s = get.call(el, a); if (s !== null && /^\+[0-9.]/.test(s)) s = s.slice(1); return s === null ? null : parseDouble(s); };
        var pmax = function (el) { var v = pnum(el, 'max'); return v !== null && v > 0 ? v : 1; };
        var pval = function (el) { var v = pnum(el, 'value'), mx = pmax(el); if (v === null || v <= 0) return 0; return v > mx ? mx : v; };
        var pset = function (k, pos) {
            return function (v) {
                var n = Number(v);
                if (!isFinite(n)) throw new TypeError("Failed to set the '" + k + "' property on 'HTMLProgressElement': The provided double value is non-finite.");
                if (pos && !(n > 0)) return;
                set.call(this, k, String(n));
            };
        };
        Object.defineProperty(PP_, 'value', { configurable: true, enumerable: true, get: function () { return pval(this); }, set: pset('value', false) });
        Object.defineProperty(PP_, 'max', { configurable: true, enumerable: true, get: function () { return pmax(this); }, set: pset('max', true) });
        Object.defineProperty(PP_, 'position', { configurable: true, enumerable: true, get: function () {
            if (pnum(this, 'value') === null) return -1;
            return pval(this) / pmax(this);
        } });
        if (!Object.getOwnPropertyDescriptor(PP_, 'labels') && global.HTMLMeterElement) {
            var ld = Object.getOwnPropertyDescriptor(global.HTMLMeterElement.prototype, 'labels');
            if (ld) Object.defineProperty(PP_, 'labels', ld);
        }
    }
    /* <base href>: the frozen base URL's source, against the document's fallback base URL */
    var BSP = global.HTMLBaseElement && global.HTMLBaseElement.prototype;
    if (BSP) Object.defineProperty(BSP, 'href', { configurable: true, enumerable: true,
        get: function () {
            var d = this.ownerDocument, url = d ? d.URL : '';
            var v = get.call(this, 'href');
            if (v === null) return url;
            if (/[^\x00-\x7f]/.test(v) && typeof global.__cyberLegacyQuery === 'function' && d && d.characterSet !== 'UTF-8') { var lq = global.__cyberLegacyQuery(v, d.characterSet); if (lq) v = lq; }
            try { return new global.URL(v, url === 'about:blank' && d.defaultView && d.defaultView.parent !== d.defaultView ? d.defaultView.parent.document.baseURI : url).href; } catch (e) { return v; }
        },
        set: function (v) { set.call(this, 'href', String(v)); } });
    /* document.fgColor and the rest: <body>'s text, link, vlink, alink and bgcolor */
    var DP = global.Document && global.Document.prototype;
    if (DP) [['fgColor', 'text'], ['linkColor', 'link'], ['vlinkColor', 'vlink'], ['alinkColor', 'alink'], ['bgColor', 'bgcolor']].forEach(function (p) {
        if (Object.getOwnPropertyDescriptor(DP, p[0])) return;
        Object.defineProperty(DP, p[0], { configurable: true, enumerable: true,
            get: function () {
                var b = this.body;
                if (!b || b.localName !== 'body') return '';
                var v = get.call(b, p[1]);
                return v === null ? '' : v;
            },
            set: function (v) {
                var b = this.body;
                if (b && b.localName === 'body') set.call(b, p[1], v === null ? '' : String(v));
            } });
    });
    /* nonce (HTML 3.2.6.x): the element's cryptographic nonce, set from the attribute as it changes; the IDL setter sets it alone */
    var nonces = new WeakMap();
    [global.HTMLElement, global.SVGElement, global.MathMLElement].forEach(function (C) {
        if (!C || !C.prototype) return;
        Object.defineProperty(C.prototype, 'nonce', { configurable: true, enumerable: true,
            get: function () {
                var a = get.call(this, 'nonce'), r = nonces.get(this);
                if (r && r.attr === a) return r.v;
                return a === null ? (r ? r.v : '') : a;
            },
            set: function (v) { nonces.set(this, { v: String(v), attr: get.call(this, 'nonce') }); } });
    });
})(typeof globalThis !== 'undefined' ? globalThis : this);

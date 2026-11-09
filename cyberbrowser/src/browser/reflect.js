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
        menu: 'HTMLMenuElement', meta: 'HTMLMetaElement', meter: 'HTMLMeterElement', object: 'HTMLObjectElement', ol: 'HTMLOListElement',
        optgroup: 'HTMLOptGroupElement', option: 'HTMLOptionElement', output: 'HTMLOutputElement', p: 'HTMLParagraphElement', param: 'HTMLParamElement',
        picture: 'HTMLPictureElement', pre: 'HTMLPreElement', progress: 'HTMLProgressElement', q: 'HTMLQuoteElement', script: 'HTMLScriptElement',
        select: 'HTMLSelectElement', slot: 'HTMLSlotElement', source: 'HTMLSourceElement', span: 'HTMLSpanElement', style: 'HTMLStyleElement',
        table: 'HTMLTableElement', tbody: 'HTMLTableSectionElement', td: 'HTMLTableCellElement', template: 'HTMLTemplateElement', textarea: 'HTMLTextAreaElement',
        tfoot: 'HTMLTableSectionElement', th: 'HTMLTableCellElement', thead: 'HTMLTableSectionElement', time: 'HTMLTimeElement', title: 'HTMLTitleElement',
        tr: 'HTMLTableRowElement', track: 'HTMLTrackElement', ul: 'HTMLUListElement', video: 'HTMLVideoElement'
    };
    /* (the element's own idea of these: not plain reflection) */
    var SKIP = { 'img.width': 1, 'img.height': 1, 'canvas.width': 1, 'canvas.height': 1, 'input.width': 1, 'input.height': 1,
                 'option.value': 1, 'option.label': 1, 'title.text': 1, 'script.text': 1 };
    /* (setting these starts something the element's own setter knows about) */
    var KEEP_SET = { src: 1, srcset: 1, srcdoc: 1, data: 1, href: 1, type: 1 };
    var MAX = 2147483647;
    var WS = /^[ \t\n\f\r]*/;
    function parseInteger(s) {
        var m = /^[ \t\n\f\r]*([-+]?)(\d+)/.exec(s);
        if (!m) return null;
        var v = parseFloat(m[2]);
        return m[1] === '-' ? -v : v;
    }
    function parseNonneg(s) {
        var v = parseInteger(s);
        return v === null || v < 0 ? (v === 0 ? 0 : null) : v;
    }
    function parseDouble(s) {
        var m = /^[ \t\n\f\r]*([-+]?(?:\d+(?:\.\d+)?|\.\d+)(?:[eE][-+]?\d+)?)/.exec(s);
        if (!m) return null;
        var v = parseFloat(m[1]);
        return isFinite(v) ? v : null;
    }
    function indexError(name) { return new global.DOMException("The value provided for '" + name + "' is out of range.", 'IndexSizeError'); }
    function lower(s) { return s.replace(/[A-Z]/g, function (c) { return c.toLowerCase(); }); }
    var get = Element.prototype.getAttribute, set = Element.prototype.setAttribute, remove = Element.prototype.removeAttribute;
    function accessor(prop, d, attr, native) {
        var dflt = d.defaultVal;
        switch (d.type) {
        case 'string':
            return { get: function () { var v = get.call(this, attr); return v === null ? '' : v; },
                     set: function (v) { set.call(this, attr, v === null && d.treatNullAsEmptyString ? '' : String(v)); } };
        case 'url':
            return { get: function () {
                         var v = get.call(this, attr);
                         if (v === null) return attr === 'action' || attr === 'formaction' ? this.ownerDocument.URL : '';
                         try { return new global.URL(v, this.baseURI).href; } catch (e) { return v; }
                     },
                     set: function (v) { set.call(this, attr, String(v)); } };
        case 'boolean':
            return { get: function () { return get.call(this, attr) !== null; },
                     set: function (v) { if (v) set.call(this, attr, ''); else remove.call(this, attr); } };
        case 'enum': {
            var missing = dflt === undefined ? (d.isNullable ? null : '') : dflt, invalid = d.invalidVal === undefined ? missing : d.invalidVal, kw = {};
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
})(typeof globalThis !== 'undefined' ? globalThis : this);

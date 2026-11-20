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
    /* (window listeners added while this runs wait for the DOM event system, dom_events.js, which comes after it:
     * form_controls.js adds them; later ones go straight in) */
    var afterEvents = [], eventsIn = false;
    function onWin(type, fn, cap) {
        if (eventsIn) global.addEventListener(type, fn, cap);
        else afterEvents.push(function () { global.addEventListener(type, fn, cap); });
    }
    Object.defineProperty(global, '__cyberAfterEvents', { configurable: true, value: function () {
        eventsIn = true;
        afterEvents.splice(0).forEach(function (f) { try { f(); } catch (e) { } });
    } });
    /* User activation (HTML 6.4.3): trusted input events activate; transient for 5 s */
    (function () {
        if (!global.addEventListener) return;
        var last = -Infinity, ever = false;
        var now = function () { return global.performance && global.performance.now ? global.performance.now() : Date.now(); };
        /* (the listeners: once the DOM event system is in, dom_events.js runs after this; form_controls.js calls it) */
        Object.defineProperty(global, '__cyberActivationListen', { configurable: true, value: function () {
            ['keydown', 'mousedown', 'pointerdown', 'pointerup', 'mouseup', 'touchend'].forEach(function (t) {
                global.addEventListener(t, function (e) { if (e.isTrusted && !(t === 'keydown' && e.key === 'Escape')) { last = now(); ever = true; } }, true);
            });
        } });
        Object.defineProperty(global, '__cyberTransientActivation', { configurable: true, value: function () { return now() - last < 5000; } });
        Object.defineProperty(global, '__cyberConsumeActivation', { configurable: true, value: function () { last = -Infinity; } });
        function UserActivation() { throw new TypeError('Illegal constructor'); }
        var UP = UserActivation.prototype;
        Object.defineProperty(UP, 'hasBeenActive', { configurable: true, enumerable: true, get: function hasBeenActive() { return ever; } });
        Object.defineProperty(UP, 'isActive', { configurable: true, enumerable: true, get: function isActive() { return now() - last < 5000; } });
        Object.defineProperty(UP, Symbol.toStringTag, { configurable: true, value: 'UserActivation' });
        Object.defineProperty(global, 'UserActivation', { configurable: true, writable: true, value: UserActivation });
        var ua = Object.create(UP);
        var nav = global.navigator, NP = nav && Object.getPrototypeOf(nav);
        var tgt = NP && NP !== Object.prototype ? NP : nav;   /* (a plain-object navigator: on it, never on Object.prototype) */
        if (tgt && !Object.prototype.hasOwnProperty.call(tgt, 'userActivation')) Object.defineProperty(tgt, 'userActivation', { configurable: true, enumerable: true, get: function userActivation() { return ua; } });
    })();
    /* btoa / atob (HTML 8.3): binary strings, forgiving-base64 decoding */
    (function () {
        var B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
        function bad(msg) { return new global.DOMException(msg, 'InvalidCharacterError'); }
        var btoa = function btoa(data) {
            if (arguments.length < 1) throw new TypeError("Failed to execute 'btoa': 1 argument required, but only 0 present.");
            var s = String(data), out = '', i;
            for (i = 0; i < s.length; i++) if (s.charCodeAt(i) > 255) throw bad("Failed to execute 'btoa': The string to be encoded contains characters outside of the Latin1 range.");
            for (i = 0; i + 2 < s.length; i += 3) {
                var n = (s.charCodeAt(i) << 16) | (s.charCodeAt(i + 1) << 8) | s.charCodeAt(i + 2);
                out += B64[n >> 18] + B64[(n >> 12) & 63] + B64[(n >> 6) & 63] + B64[n & 63];
            }
            var r = s.length - i;
            if (r === 1) { var a = s.charCodeAt(i) << 16; out += B64[a >> 18] + B64[(a >> 12) & 63] + '=='; }
            else if (r === 2) { var b = (s.charCodeAt(i) << 16) | (s.charCodeAt(i + 1) << 8); out += B64[b >> 18] + B64[(b >> 12) & 63] + B64[(b >> 6) & 63] + '='; }
            return out;
        };
        var atob = function atob(data) {
            if (arguments.length < 1) throw new TypeError("Failed to execute 'atob': 1 argument required, but only 0 present.");
            var s = String(data).replace(/[\t\n\f\r ]/g, '');
            if (s.length % 4 === 0) s = s.replace(/={1,2}$/, '');
            if (s.length % 4 === 1 || /[^A-Za-z0-9+/]/.test(s)) throw bad("Failed to execute 'atob': The string to be decoded is not correctly encoded.");
            var out = '', buf = 0, bits = 0;
            for (var i = 0; i < s.length; i++) {
                buf = (buf << 6) | B64.indexOf(s[i]); bits += 6;
                if (bits >= 8) { bits -= 8; out += String.fromCharCode((buf >> bits) & 255); }
            }
            return out;
        };
        [global, win].forEach(function (g) {
            if (!g) return;
            Object.defineProperty(g, 'btoa', { value: btoa, writable: true, configurable: true, enumerable: true });
            Object.defineProperty(g, 'atob', { value: atob, writable: true, configurable: true, enumerable: true });
        });
    })();
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

    /* (the menu elements, whatwg/html#12011 (tentative): interfaces as the IDL has them) */
    if (HE) [['HTMLMenuBarElement', 0], ['HTMLMenuListElement', 0], ['HTMLSubMenuElement', 0], ['HTMLMenuItemElement', 1],
             ['HTMLMenuItemCheckboxElement', 2], ['HTMLMenuItemRadioElement', 2]].forEach(function (p) {
        var name = p[0];
        if (typeof global[name] === 'function') return;
        var C = function () { throw new TypeError('Illegal constructor'); };
        Object.defineProperty(C, 'name', { value: name, configurable: true });
        Object.defineProperty(C, 'length', { value: 0, configurable: true });
        var P = Object.create(HE.prototype);
        Object.defineProperty(P, 'constructor', { value: C, writable: true, configurable: true });
        Object.defineProperty(P, Symbol.toStringTag, { value: name, configurable: true });
        Object.defineProperty(C, 'prototype', { value: P, writable: false, enumerable: false, configurable: false });
        Object.setPrototypeOf(C, HE);
        var acc = function (k, get, set) {
            var g = { get: function () { if (!(this instanceof C)) throw new TypeError('Illegal invocation'); return get.call(this); } }.get;
            var st = { set: function (v) { if (!(this instanceof C)) throw new TypeError('Illegal invocation'); set.call(this, v); } }.set;
            Object.defineProperty(g, 'name', { value: 'get ' + k });
            Object.defineProperty(st, 'name', { value: 'set ' + k });
            Object.defineProperty(P, k, { get: g, set: st, enumerable: true, configurable: true });
        };
        var bool = function (k, attr) {
            acc(k, function () { return this.hasAttribute(attr); }, function (v) { if (v) this.setAttribute(attr, ''); else this.removeAttribute(attr); });
        };
        if (p[1] >= 1) bool('disabled', 'disabled');
        if (p[1] === 2) {
            var CHK = Symbol('checked');
            /* (its checkedness: the defaultchecked attribute's until set; :checked with it; a radio's set unchecks the rest of its fieldset's) */
            var setChk = function (el, v) {
                el[CHK] = !!v;
                if (typeof global.__cyberElState === 'function') { global.__cyberElState(el, 128, true); global.__cyberElState(el, 64, !!v); }
            };
            var radio = name === 'HTMLMenuItemRadioElement';
            acc('checked', function () { return CHK in this ? this[CHK] : this.hasAttribute('defaultchecked'); }, function (v) {
                setChk(this, v);
                if (radio && v) {
                    var grp = this.parentElement;
                    while (grp && grp.localName !== 'fieldset' && grp.localName !== 'menulist' && grp.localName !== 'menubar') grp = grp.parentElement;
                    if (grp) {
                        var others = grp.querySelectorAll('menuitemradio');
                        for (var oi = 0; oi < others.length; oi++) if (others[oi] !== this && (CHK in others[oi] ? others[oi][CHK] : others[oi].hasAttribute('defaultchecked'))) setChk(others[oi], false);
                    }
                }
            });
            bool('defaultChecked', 'defaultchecked');
        }
        Object.defineProperty(global, name, { value: C, writable: true, configurable: true, enumerable: false });
    });

    /* (the permission elements, tentative: their interfaces, members as the IDL has them, at rest) */
    if (HE) (function () {
        var mk = function (name, attrs, ops) {
            if (typeof global[name] === 'function') return;
            var C = function () { throw new TypeError('Illegal constructor'); };
            Object.defineProperty(C, 'name', { value: name, configurable: true });
            Object.defineProperty(C, 'length', { value: 0, configurable: true });
            var P = Object.create(HE.prototype);
            Object.defineProperty(P, 'constructor', { value: C, writable: true, configurable: true });
            Object.defineProperty(P, Symbol.toStringTag, { value: name, configurable: true });
            Object.defineProperty(C, 'prototype', { value: P, writable: false, enumerable: false, configurable: false });
            Object.setPrototypeOf(C, HE);
            var chk = function (o) { if (!(o instanceof C)) throw new TypeError('Illegal invocation'); };
            /* (why it can't be used now: its display one of those a permission element may have, PEPC's locked style) */
            var reasonOf = function (el) {
                if (!el.isConnected) return 'unsuccessful_registration';
                var d = '';
                try { d = global.getComputedStyle(el).display; } catch (e) { }
                if (!(/^(block|inline-block|flex|inline-flex|flow-root)$/.test(d) || d === 'block flow' || d === 'inline flow-root')) return 'style_invalid';
                /* (an ancestor masking it: not clearly shown) */
                for (var a = el.parentElement; a; a = a.parentElement) {
                    var mi = 'none';
                    try { var ac = global.getComputedStyle(a); mi = ac.maskImage || ac.webkitMaskImage || 'none'; } catch (e) { }
                    if (mi && mi !== 'none') return 'style_invalid';
                }
                return '';
            };
            var named = function (f, n) { Object.defineProperty(f, 'name', { value: n }); return f; };
            (attrs || []).forEach(function (a) {
                var k = a[0], kind = a[1], g, st;
                if (kind === 'ro') { var dv = a[2]; g = function () { chk(this); return dv; }; }
                else if (kind === 'reason') g = function () { chk(this); return reasonOf(this); };
                else if (kind === 'valid') g = function () { chk(this); return reasonOf(this) === ''; };
                else if (kind === 'str') { var sn = k.toLowerCase(); g = function () { chk(this); var v = this.getAttribute(sn); return v === null ? '' : v; };
                    st = function (v) { chk(this); this.setAttribute(sn, String(v)); }; }
                else if (kind === 'bool') { var an = k.toLowerCase(); g = function () { chk(this); return this.hasAttribute(an); };
                    st = function (v) { chk(this); if (v) this.setAttribute(an, ''); else this.removeAttribute(an); }; }
                else if (kind === 'url') { var un = a[2] || k.toLowerCase();
                    g = function () { chk(this); var v = this.getAttribute(un); if (v === null) return ''; try { return new global.URL(v, this.baseURI).href; } catch (e) { return v; } };
                    st = function (v) { chk(this); this.setAttribute(un, String(v)); }; }
                else if (kind === 'h') {
                    var HK = Symbol(k), ty = k.slice(2);
                    g = function () { chk(this); var h = this[HK]; return h ? h.fn : null; };
                    st = function (v) {
                        chk(this);
                        var h = this[HK];
                        if (h) { this.removeEventListener(ty, h.wrap); this[HK] = null; }
                        if (typeof v === 'function' || (v !== null && typeof v === 'object')) {
                            var fn = v, wrap = function (e) { return typeof fn === 'function' ? fn.call(this, e) : undefined; };
                            this[HK] = { fn: fn, wrap: wrap };
                            this.addEventListener(ty, wrap);
                            /* (its validation status, told once it is known) */
                            if (ty === 'validationstatuschange') {
                                var el = this;
                                global.setTimeout(function () { try { el.dispatchEvent(new global.Event('validationstatuschange')); } catch (e) { } }, 0);
                            }
                        }
                    };
                }
                var d = { get: named(g, 'get ' + k), enumerable: true, configurable: true };
                if (st) d.set = named(st, 'set ' + k);
                Object.defineProperty(P, k, d);
            });
            (ops || []).forEach(function (o) {
                var f = named(function () { chk(this); return undefined; }, o[0]);
                Object.defineProperty(f, 'length', { value: o[1] });
                Object.defineProperty(P, o[0], { value: f, writable: true, enumerable: true, configurable: true });
            });
            Object.defineProperty(global, name, { value: C, writable: true, configurable: true, enumerable: false });
        };
        var device = [['track', 'ro', null], ['error', 'ro', null], ['ontrack', 'h'], ['oncancel', 'h'], ['onerror', 'h']];
        mk('HTMLUserMediaElement', [['onstream', 'h'], ['oncancel', 'h'], ['onerror', 'h'], ['stream', 'ro', null], ['error', 'ro', null]], [['setConstraints', 0]]);
        mk('HTMLGeolocationElement', [['position', 'ro', null], ['error', 'ro', null], ['autolocate', 'bool'], ['watch', 'bool'], ['onlocation', 'h'],
            ['isValid', 'valid'], ['invalidReason', 'reason'], ['initialPermissionStatus', 'ro', 'prompt'], ['permissionStatus', 'ro', 'prompt'],
            ['onpromptaction', 'h'], ['onpromptdismiss', 'h'], ['onvalidationstatuschange', 'h']]);
        mk('HTMLCameraElement', device);
        mk('HTMLMicrophoneElement', device);
        mk('HTMLInstallElement', [['manifest', 'url'], ['manifestId', 'url', 'manifestid'], ['isValid', 'valid'], ['invalidReason', 'reason'],
            ['onvalidationstatuschange', 'h']]);
        /* (persistent widgets, tentative: the element, its opener's interface, window.persistentWidgetOpener) */
        mk('HTMLPersistentWidgetElement', [['src', 'url'], ['name', 'str']], [['postMessage', 1]]);
        if (typeof global.PersistentWidgetOpener !== 'function' && global.EventTarget) {
            var PWO = function PersistentWidgetOpener() { throw new TypeError('Illegal constructor'); };
            PWO.prototype = Object.create(global.EventTarget.prototype);
            Object.defineProperty(PWO.prototype, 'constructor', { value: PWO, writable: true, configurable: true });
            Object.defineProperty(PWO.prototype, Symbol.toStringTag, { value: 'PersistentWidgetOpener', configurable: true });
            var pwm = { postMessage: function (message) {
                if (!(this instanceof PWO)) throw new TypeError('Illegal invocation');
                if (arguments.length < 1) throw new TypeError("Failed to execute 'postMessage' on 'PersistentWidgetOpener': 1 argument required, but only 0 present.");
            } }.postMessage;
            Object.defineProperty(pwm, 'length', { value: 1 });
            Object.defineProperty(PWO.prototype, 'postMessage', { value: pwm, writable: true, enumerable: true, configurable: true });
            Object.setPrototypeOf(PWO, global.EventTarget);
            Object.defineProperty(global, 'PersistentWidgetOpener', { value: PWO, writable: true, configurable: true });
            if (!('persistentWidgetOpener' in global))
                Object.defineProperty(global, 'persistentWidgetOpener', { enumerable: true, configurable: true, get: { get: function persistentWidgetOpener() { return null; } }.get });
        }
        /* (InstallResultEvent: an Event with its result) */
        var EV = global.Event;
        if (EV && typeof global.InstallResultEvent !== 'function') {
            var RES = { success: 1, aborted: 1, 'invalid-data': 1 };
            var IRE = function InstallResultEvent(type, init) {
                if (!new.target) throw new TypeError("Failed to construct 'InstallResultEvent': Please use the 'new' operator");
                if (arguments.length < 1) throw new TypeError("Failed to construct 'InstallResultEvent': 1 argument required, but only 0 present.");
                var e = Reflect.construct(EV, [type, init], new.target);
                var r = init && init.result !== undefined ? String(init.result) : 'aborted';
                if (!RES[r]) throw new TypeError("Failed to construct 'InstallResultEvent': The provided value '" + r + "' is not a valid enum value of type InstallResult.");
                Object.defineProperty(e, '__result', { value: r });
                return e;
            };
            IRE.prototype = Object.create(EV.prototype);
            Object.defineProperty(IRE.prototype, 'constructor', { value: IRE, writable: true, configurable: true });
            Object.defineProperty(IRE.prototype, Symbol.toStringTag, { value: 'InstallResultEvent', configurable: true });
            Object.defineProperty(IRE.prototype, 'result', { enumerable: true, configurable: true,
                get: { get: function () { if (!(this instanceof IRE)) throw new TypeError('Illegal invocation'); return this.__result; } }.get });
            Object.setPrototypeOf(IRE, EV);
            Object.defineProperty(IRE, 'length', { value: 1 });
            Object.defineProperty(global, 'InstallResultEvent', { value: IRE, writable: true, configurable: true, enumerable: false });
        }
    })();

    /* (SVG 2 SVGURIReference: href, an SVGAnimatedString of the href attribute, else xlink:href) */
    (function () {
        var XL = 'http://www.w3.org/1999/xlink';
        var SAS = global.SVGAnimatedString;
        if (typeof SAS !== 'function') {
            SAS = function SVGAnimatedString() { throw new TypeError('Illegal constructor'); };
            Object.defineProperty(SAS.prototype, Symbol.toStringTag, { value: 'SVGAnimatedString', configurable: true });
            Object.defineProperty(global, 'SVGAnimatedString', { value: SAS, writable: true, configurable: true, enumerable: false });
        }
        var EL = Symbol('svgHrefElement');
        if (!Object.getOwnPropertyDescriptor(SAS.prototype, 'baseVal')) {
            var cur = function (el) { var v = el.getAttribute('href'); if (v === null) v = el.getAttributeNS(XL, 'href'); return v === null ? '' : v; };
            Object.defineProperty(SAS.prototype, 'baseVal', { enumerable: true, configurable: true,
                get: { get: function baseVal() { var el = this[EL]; if (!el) throw new TypeError('Illegal invocation'); return cur(el); } }.get,
                set: { set: function baseVal(v) {
                    var el = this[EL]; if (!el) throw new TypeError('Illegal invocation');
                    if (!el.hasAttribute('href') && el.hasAttributeNS(XL, 'href')) el.setAttributeNS(XL, 'xlink:href', String(v));
                    else el.setAttribute('href', String(v));
                } }.set });
            Object.defineProperty(SAS.prototype, 'animVal', { enumerable: true, configurable: true,
                get: { get: function animVal() { var el = this[EL]; if (!el) throw new TypeError('Illegal invocation'); return cur(el); } }.get });
        }
    /* (SVG 2's element interfaces, bare: what they have is SVGElement's for now) */
    if (global.document && global.SVGElement) plainInterfaces([
        ['SVGGraphicsElement', 'SVGElement'], ['SVGGeometryElement', 'SVGGraphicsElement'], ['SVGSVGElement', 'SVGGraphicsElement'],
        ['SVGGElement', 'SVGGraphicsElement'], ['SVGDefsElement', 'SVGGraphicsElement'], ['SVGSymbolElement', 'SVGElement'],
        ['SVGUseElement', 'SVGGraphicsElement'], ['SVGImageElement', 'SVGGraphicsElement'], ['SVGSwitchElement', 'SVGGraphicsElement'],
        ['SVGForeignObjectElement', 'SVGGraphicsElement'], ['SVGRectElement', 'SVGGeometryElement'], ['SVGCircleElement', 'SVGGeometryElement'],
        ['SVGEllipseElement', 'SVGGeometryElement'], ['SVGLineElement', 'SVGGeometryElement'], ['SVGPolylineElement', 'SVGGeometryElement'],
        ['SVGPolygonElement', 'SVGGeometryElement'], ['SVGPathElement', 'SVGGeometryElement'], ['SVGTextContentElement', 'SVGGraphicsElement'],
        ['SVGTextPositioningElement', 'SVGTextContentElement'], ['SVGTextElement', 'SVGTextPositioningElement'],
        ['SVGTSpanElement', 'SVGTextPositioningElement'], ['SVGTextPathElement', 'SVGTextContentElement'], ['SVGTitleElement', 'SVGElement'],
        ['SVGDescElement', 'SVGElement'], ['SVGMetadataElement', 'SVGElement'], ['SVGStyleElement', 'SVGElement'], ['SVGScriptElement', 'SVGElement'],
        ['SVGMarkerElement', 'SVGElement'], ['SVGPatternElement', 'SVGElement'], ['SVGGradientElement', 'SVGElement'],
        ['SVGLinearGradientElement', 'SVGGradientElement'], ['SVGRadialGradientElement', 'SVGGradientElement'], ['SVGStopElement', 'SVGElement'],
        ['SVGClipPathElement', 'SVGElement'], ['SVGMaskElement', 'SVGElement'], ['SVGViewElement', 'SVGElement'],
        ['SVGAnimationElement', 'SVGElement'], ['SVGAnimateElement', 'SVGAnimationElement'], ['SVGSetElement', 'SVGAnimationElement'],
        ['SVGAnimateMotionElement', 'SVGAnimationElement'], ['SVGAnimateTransformElement', 'SVGAnimationElement'], ['SVGMPathElement', 'SVGElement'],
    ].map(function (d) { return [d[0], d[1], [], [], []]; }));
        var HREFS = Symbol('svgHref');
        ['SVGAElement', 'SVGUseElement', 'SVGImageElement', 'SVGScriptElement', 'SVGPatternElement', 'SVGLinearGradientElement',
         'SVGRadialGradientElement', 'SVGGradientElement', 'SVGFilterElement', 'SVGTextPathElement', 'SVGFEImageElement', 'SVGMPathElement'].forEach(function (n) {
            var C = global[n];
            if (typeof C !== 'function' || !C.prototype) return;
            var d = Object.getOwnPropertyDescriptor(C.prototype, 'href');
            if (d && d.get) return;   /* (one there already) */
            Object.defineProperty(C.prototype, 'href', { enumerable: true, configurable: true,
                get: { get: function href() {
                    if (!(this instanceof C)) throw new TypeError('Illegal invocation');
                    if (!this[HREFS]) { var o = Object.create(SAS.prototype); Object.defineProperty(o, EL, { value: this }); Object.defineProperty(this, HREFS, { value: o }); }
                    return this[HREFS];
                } }.get });
        });
    })();

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
    /* (runScripts: the scripts put in run, in tree order, shadow trees too, each as if just inserted; one a script took out does not) */
    var runInsertedScripts = function (root) {
        var list = [];
        var walk = function (n) {
            for (var c = n.firstChild; c; c = c.nextSibling) {
                if (c.nodeType !== 1) continue;
                if (c.localName === 'script' && c.namespaceURI === 'http://www.w3.org/1999/xhtml') list.push(c);
                if (c.shadowRoot) walk(c.shadowRoot);
                walk(c.localName === 'template' && c.content ? c.content : c);
            }
        };
        walk(root);
        list.forEach(function (s) {
            if (!s.isConnected || !s.parentNode) return;
            var n = s.ownerDocument.createElement('script');
            for (var i = 0; i < s.attributes.length; i++) n.setAttribute(s.attributes[i].name, s.attributes[i].value);
            n.text = s.text;
            s.parentNode.replaceChild(n, s);
        });
    };
    var setUnsafe = function setHTMLUnsafe(html, options) {
        this.innerHTML = String(html);
        if (options && typeof options === 'object' && options.runScripts) runInsertedScripts(this);
    };
    if (global.Element && !global.Element.prototype.setHTMLUnsafe)
        Object.defineProperty(global.Element.prototype, 'setHTMLUnsafe', { configurable: true, enumerable: true, writable: true, value: setUnsafe });
    if (global.ShadowRoot && !global.ShadowRoot.prototype.setHTMLUnsafe)
        Object.defineProperty(global.ShadowRoot.prototype, 'setHTMLUnsafe', { configurable: true, enumerable: true, writable: true, value: setUnsafe });
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
            /* (the bytes: the same object each time) */
            case 'arraybuffer': case 'blob': {
                var by = this.__cyberXhrBytes;
                if (!by) return null;
                if (this.responseType === 'arraybuffer') return by;
                if (!this.__cyberXhrBlob || this.__cyberXhrBlob[0] !== by) {
                    var ct = (this.getResponseHeader('content-type') || '').split(';')[0].trim().toLowerCase();
                    Object.defineProperty(this, '__cyberXhrBlob', { value: [by, new global.Blob([by], { type: ct })], configurable: true, writable: true });
                }
                return this.__cyberXhrBlob[1];
            }
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
                else if (p instanceof ArrayBuffer || (p !== null && typeof p === 'object' && Object.prototype.toString.call(p) === '[object ArrayBuffer]')) c = new Uint8Array(p);   /* (another realm's too: a native's) */
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
    /* (a Blob's bytes for the engine: blob: URLs of images; undefined for anything else) */
    Object.defineProperty(global, '__cyberBlobBytes', { configurable: true, value: function (b) { return b !== null && typeof b === 'object' ? blobBytes.get(b) : undefined; } });
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
    /* (the MessagePorts a transfer moves: passed through as they are) */
    var curPorts = null;
    function wire(v, seen) {
        if (curPorts && v && typeof v === 'object') { var pi = curPorts.indexOf(v); if (pi >= 0) return { tp: pi }; }
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
        case 'ArrayBuffer': return v.resizable ? { i: id, ab: Array.from(new Uint8Array(v)), abmax: v.maxByteLength } : { i: id, ab: Array.from(new Uint8Array(v)) };
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
            /* (its message only if its own; its cause too, cloned) */
            var md = Object.getOwnPropertyDescriptor(v, 'message'), cd = Object.getOwnPropertyDescriptor(v, 'cause');
            var ew = { i: id, e: name, st: v.stack === undefined ? '' : String(v.stack) };
            if (md && 'value' in md) ew.msg = String(md.value);
            if (cd && 'value' in cd) ew.cause = wire(cd.value, seen);
            return ew;
        }
        case 'ImageData': return { i: id, idt: Array.from(v.data), iw: v.width, ih: v.height, ics: v.colorSpace || 'srgb' };
        case 'FileList': {
            var fl = [];
            for (var fi = 0; fi < v.length; fi++) fl.push(wire(v[fi], seen));
            return { i: id, flist: fl };
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
        case 'OffscreenCanvas': {
            var ow = typeof global.__cyberOffscreenWire === 'function' ? global.__cyberOffscreenWire(v) : null;
            if (!ow) throw cloneError(kind);
            return { i: id, ocv: ow };
        }
        case 'ElementImage': {
            var ew = typeof global.__cyberElementImageWire === 'function' ? global.__cyberElementImageWire(v) : null;
            if (!ew) throw cloneError(kind);
            return { i: id, ei: ew };
        }
        case 'ImageBitmap': {
            /* (a bitmap: its id in canvas2d.c, shared by the realms of this thread) */
            var bw = typeof global.__cyberBitmapWire === 'function' ? global.__cyberBitmapWire(v) : null;
            if (!bw) throw cloneError(kind);
            return { i: id, ib: bw };
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
        if ((global.Response && v instanceof global.Response) || (global.Request && v instanceof global.Request) ||
            (global.Headers && v instanceof global.Headers)) throw cloneError(v);
        var props = [];
        Object.keys(v).forEach(function (k) { props.push([k, wire(v[k], seen)]); });
        return { i: id, o: props };
    }
    function unwire(w, made) {
        if (w === null || typeof w !== 'object') return w;
        if (w.tp !== undefined && curPorts) return curPorts[w.tp];
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
            if (w.abmax !== undefined) { v = made[w.i] = new ArrayBuffer(w.ab.length, { maxByteLength: w.abmax }); new Uint8Array(v).set(w.ab); }
            else v = made[w.i] = new Uint8Array(w.ab).buffer;
        } else if (w.m !== undefined) {
            v = made[w.i] = new Map();
            w.m.forEach(function (e) { v.set(unwire(e[0], made), unwire(e[1], made)); });
        } else if (w.s !== undefined) {
            v = made[w.i] = new Set();
            w.s.forEach(function (e) { v.add(unwire(e, made)); });
        } else if (w.x !== undefined) {
            v = made[w.i] = new global.DOMException(w.msg, w.x);
        } else if (w.e !== undefined) {
            v = made[w.i] = w.msg === undefined ? new global[w.e]() : new global[w.e](w.msg);
            if (w.st) Object.defineProperty(v, 'stack', { value: w.st, writable: true, configurable: true });
            if (w.cause !== undefined) Object.defineProperty(v, 'cause', { value: unwire(w.cause, made), writable: true, configurable: true });
        } else if (w.idt !== undefined) {
            v = made[w.i] = new global.ImageData(new Uint8ClampedArray(w.idt), w.iw, w.ih, { colorSpace: w.ics });
        } else if (w.flist !== undefined) {
            var files = w.flist.map(function (x) { return unwire(x, made); });
            v = made[w.i] = global.__cyberFileList ? global.__cyberFileList(files) : files;
        } else if (w.g !== undefined) {
            var C = global[w.g], gv = w.gv;
            if (/Point/.test(w.g)) v = new C(gv[0], gv[1], gv[2], gv[3]);
            else if (/Rect/.test(w.g)) v = new C(gv[0], gv[1], gv[2], gv[3]);
            else if (w.g === "DOMQuad") v = new C(new global.DOMPoint(gv[0][0], gv[0][1], gv[0][2], gv[0][3]), new global.DOMPoint(gv[1][0], gv[1][1], gv[1][2], gv[1][3]),
                                                        new global.DOMPoint(gv[2][0], gv[2][1], gv[2][2], gv[2][3]), new global.DOMPoint(gv[3][0], gv[3][1], gv[3][2], gv[3][3]));
            else v = new C(gv);
            made[w.i] = v;
        } else if (w.ocv !== undefined) {
            v = made[w.i] = global.__cyberOffscreenMake(w.ocv);
        } else if (w.ib !== undefined) {
            v = made[w.i] = global.__cyberBitmapMake(w.ib);
        } else if (w.ei !== undefined && typeof global.__cyberElementImageMake === 'function') {
            v = made[w.i] = global.__cyberElementImageMake(w.ei);
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
        /* (options.transfer: ArrayBuffers detached after, their contents moved; anything else not transferable) */
        var opts = arguments[1], list = [];
        if (opts !== undefined && opts !== null) {
            var tr = opts.transfer;
            if (tr !== undefined) {
                if (tr === null || typeof tr !== 'object' || typeof tr[Symbol.iterator] !== 'function') throw new TypeError("Failed to execute 'structuredClone': transfer is not iterable.");
                list = Array.from(tr);
            }
        }
        var seenT = new Set();
        list.forEach(function (t) {
            if (seenT.has(t)) throw new global.DOMException("Failed to execute 'structuredClone': ArrayBuffer at index is a duplicate of an earlier ArrayBuffer.", 'DataCloneError');
            seenT.add(t);
            var tag = Object.prototype.toString.call(t), isAB = tag === '[object ArrayBuffer]', isPort = tag === '[object MessagePort]';
            if (!isAB && !isPort) throw new global.DOMException("Failed to execute 'structuredClone': Value at index is not a transferable type.", 'DataCloneError');
            if (isAB && t.detached === true) throw new global.DOMException("Failed to execute 'structuredClone': An ArrayBuffer is detached and could not be cloned.", 'DataCloneError');
            if (isPort && t.__cyberDetached) throw new global.DOMException("Failed to execute 'structuredClone': A MessagePort is detached and could not be transferred.", 'DataCloneError');
        });
        var ports = list.filter(function (t) { return Object.prototype.toString.call(t) === '[object MessagePort]'; });
        var wasPorts = curPorts;
        curPorts = ports;
        var out;
        try { out = unwire(wire(value, new Map()), []); } finally { curPorts = wasPorts; }
        list.forEach(function (t) { try { if (typeof t.transfer === 'function') t.transfer(); } catch (e) { } });
        return out;
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
    var BRANDED = typeof Symbol === 'function' ? Symbol.for('cyber.branded') : '__cyberBranded';
    Object.defineProperty(global, '__cyberBrandAccessors', { configurable: true, value: function (C, req, ops) {
        var proto = C.prototype;
        if (!proto || typeof proto !== 'object') return;
        /* (with the IDL's members given: only those looked at) */
        (ops && req ? Object.keys(req) : Object.getOwnPropertyNames(proto)).forEach(function (k) {
            var d = Object.getOwnPropertyDescriptor(proto, k);
            if (!d || !d.configurable) return;
            /* (an operation made in script: its this an instance; its required arguments there) */
            if (false && ops && req && typeof d.value === 'function' && k !== 'constructor' && Object.prototype.hasOwnProperty.call(req, k) && req[k] >= 0 &&
                !d.value[BRANDED] && fnText.call(d.value).indexOf('[native code]') < 0) {
                var f = d.value, need = req[k] | 0;
                var w = function () {
                    if (!(this instanceof C) && this != null && this !== global) throw new TypeError("Failed to execute '" + k + "' on '" + C.name + "': Illegal invocation");
                    if (arguments.length < need) throw new TypeError("Failed to execute '" + k + "' on '" + C.name + "': " + need + " argument" + (need > 1 ? 's' : '') + " required, but only " + arguments.length + " present.");
                    return f.apply(this, arguments);
                };
                Object.defineProperty(w, 'name', { value: k, configurable: true });
                Object.defineProperty(w, 'length', { value: f.length, configurable: true });
                Object.defineProperty(w, BRANDED, { value: true });
                Object.defineProperty(proto, k, { value: w, writable: d.writable, enumerable: d.enumerable, configurable: true });
                return;
            }
            /* (the cheap tests first: a realm runs this over every interface) */
            if (!d.get || d.get[BRANDED]) return;
            if (ops && req && !Object.prototype.hasOwnProperty.call(req, k)) return;
            if (fnText.call(d.get).indexOf('[native code]') >= 0) return;
            var g = d.get, s = d.set;
            var get = function () { if (!(this instanceof C)) throw new TypeError('Illegal invocation'); return g.call(this); };
            var set = s && function (v) { if (!(this instanceof C)) throw new TypeError('Illegal invocation'); return s.call(this, v); };
            Object.defineProperty(get, 'name', { value: 'get ' + k, configurable: true });
            Object.defineProperty(get, BRANDED, { value: true });
            if (set) { Object.defineProperty(set, 'name', { value: 'set ' + k, configurable: true }); Object.defineProperty(set, 'length', { value: 1, configurable: true }); }
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
            sizeAdjust: ['100%', function (v) { v = String(v).trim(); return /^\d*\.?\d+%$/.test(v) ? v : null; }],
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
                    if (h.refresh) h.refresh();
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
            set: function (v) { var h = ffOf(this); h.family = ffFamily(v); if (ffFam.set) ffFam.set.call(this, v); if (h.refresh) h.refresh(); } });
        /* (weight, style and stretch set after it was added: its rule made again) */
        ['weight', 'style', 'stretch'].forEach(function (k) {
            var pd = Object.getOwnPropertyDescriptor(global.FontFace.prototype, k);
            if (pd && pd.set) Object.defineProperty(global.FontFace.prototype, k, { enumerable: true, configurable: true, get: pd.get,
                set: function (v) { pd.set.call(this, v); var h = ffOf(this); if (h.refresh) h.refresh(); } });
            else if (!pd) Object.defineProperty(global.FontFace.prototype, k, { enumerable: true, configurable: true,
                get: function () { var h = ffOf(this); return '_' + k in h ? h['_' + k] : 'normal'; },
                set: function (v) { var h = ffOf(this); h['_' + k] = String(v); if (h.refresh) h.refresh(); } });
        });
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
            if (typeof source === 'string') ffOf(f).src = source;   /* (its sources as written: an @font-face of it once added) */
            if (typeof source === 'string') Object.defineProperty(f, Symbol.for('cyber.ffsrc'), { value: source });   /* (a worker's face set: the same) */
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
        /* (a face added by script: an @font-face of its sources and descriptors, in a sheet of the document's the page doesn't see) */
        var ffSheet = function (f) {
            if (typeof ffOf !== 'function') return;
            var h = ffOf(f), doc = global.document;
            if (!h.src || !doc || !global.CSSStyleSheet) return;
            var d = ['font-family: ' + f.family, 'src: ' + h.src];
            var put = function (n, v, def) { if (v !== undefined && v !== null && String(v) !== '' && String(v) !== def) d.push(n + ': ' + v); };
            put('font-weight', f.weight, 'normal');
            put('font-style', f.style, 'normal');
            put('font-stretch', f.stretch, 'normal');
            var names = { unicodeRange: 'unicode-range', featureSettings: 'font-feature-settings', variationSettings: 'font-variation-settings',
                          display: 'font-display', ascentOverride: 'ascent-override', descentOverride: 'descent-override',
                          lineGapOverride: 'line-gap-override', sizeAdjust: 'size-adjust' };
            Object.keys(names).forEach(function (k) { if (k in h) put(names[k], h[k], ''); });
            var text = '@font-face { ' + d.join('; ') + ' }';
            /* (its descriptors changed after: the rule made again) */
            if (h.sheet) {
                if (h.sheetText !== text) { try { h.sheet.replaceSync(text); h.sheetText = text; doc.adoptedStyleSheets = doc.adoptedStyleSheets; } catch (e) { } }
                return;
            }
            try { if (typeof global.__c2dFontScan === 'function') global.__c2dFontScan(text, doc.baseURI || global.location.href); } catch (e) { }
            var sh;
            try { sh = new global.CSSStyleSheet(); sh.replaceSync(text); } catch (e) { return; }
            h.sheetText = text;
            h.refresh = function () { ffSheet(f); };
            var arr = doc.__cyberFontSheets;
            if (!arr) Object.defineProperty(doc, '__cyberFontSheets', { configurable: true, value: arr = [] });
            arr.push(sh);
            h.sheet = sh;
            try { doc.adoptedStyleSheets = doc.adoptedStyleSheets; } catch (e) { }   /* (laid out again with it) */
        };
        var ffAdd = FP.add;
        if (typeof ffAdd === 'function') Object.defineProperty(FP, 'add', { configurable: true, enumerable: true, writable: true, value: function add(font) {
            var r = ffAdd.apply(this, arguments);
            try { if (global.document && this === global.document.fonts && font instanceof global.FontFace) ffSheet(font); } catch (e) { }
            return r;
        } });
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
                var t = texts[k], n = t.data.length, lvs = null;
                /* (a right-to-left character: its right half before it, logically) */
                if (typeof global.__c2dBidi === 'function' && t.parentNode && t.parentNode.nodeType === 1) {
                    var tdir = 0;
                    try { tdir = global.getComputedStyle(t.parentNode).direction === 'rtl' ? 1 : 0; } catch (e) { }
                    if (tdir || /[\u0590-\u08ff\ufb1d-\ufdff\ufe70-\ufefc]/.test(t.data)) { try { lvs = global.__c2dBidi(t.data, tdir); } catch (e) { lvs = null; } }
                }
                for (var i = 0, i2; i < n && budget > 0; i = i2, budget--) {
                    /* (a character at a time: a surrogate pair is one) */
                    var cu = t.data.charCodeAt(i), cu2 = t.data.charCodeAt(i + 1);
                    i2 = cu >= 0xd800 && cu < 0xdc00 && cu2 >= 0xdc00 && cu2 < 0xe000 ? i + 2 : i + 1;
                    r.setStart(t, i);
                    r.setEnd(t, i2);
                    var rs = r.getClientRects();
                    for (var j = 0; j < rs.length; j++) {
                        var b = rs[j];
                        if (!b.width && !b.height) continue;
                        var dy = y < b.top ? b.top - y : y >= b.bottom ? y - b.bottom : 0;
                        var dx = x < b.left ? b.left - x : x >= b.right ? x - b.right : 0;
                        var d = dy * 1e4 + dx;
                        if (d < bestD) { bestD = d; var lh0 = x < b.left + b.width / 2; best = [t, (lvs && lvs[i] & 1) ? (lh0 ? i2 : i) : (lh0 ? i : i2)]; }
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
            /* (SVGAnimatedTransformList: the transform attribute's functions, each its name and text; the CSS property's not among them) */
            if (!Object.prototype.hasOwnProperty.call(SvgP, 'transform')) {   /* (over HTMLElement's legacy transform() helper) */
                var TYPES = { matrix: 1, translate: 2, scale: 3, rotate: 4, skewx: 5, skewy: 6 };
                var tfList = function (el) {
                    var items = [], src = el.getAttribute('transform') || '', re = /([a-zA-Z]+)\s*\(([^)]*)\)/g, m;
                    while ((m = re.exec(src))) {
                        var nums = m[2].split(/[\s,]+/).filter(function (x) { return x !== ''; }).map(Number);
                        items.push({ type: TYPES[m[1].toLowerCase()] || 0, angle: m[1].toLowerCase() === 'rotate' ? nums[0] || 0 : 0, args: nums });
                    }
                    var list = { get length() { return items.length; }, get numberOfItems() { return items.length; },
                                 getItem: function (i) { if (i < 0 || i >= items.length) throw new global.DOMException('Index out of range', 'IndexSizeError'); return items[i]; } };
                    return list;
                };
                Object.defineProperty(SvgP, 'transform', { enumerable: true, configurable: true, get: function () {
                    var el = this, o = {};
                    Object.defineProperty(o, 'baseVal', { enumerable: true, get: function () { return tfList(el); } });
                    Object.defineProperty(o, 'animVal', { enumerable: true, get: function () { return tfList(el); } });
                    return o;
                } });
            }
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
    /* (a scroller's columns as snap targets: one token each, the same from call to call) */
    var colTokens = new WeakMap();
    function columnToken(el, q) {
        var a = colTokens.get(el);
        if (!a) { a = []; colTokens.set(el, a); }
        while (a.length <= q) a.push({ column: a.length, contains: function () { return false; } });
        return a[q];
    }
    /* (the scroll-snap-align its ::column rules give it, in sheet order; '' none) */
    function columnSnapAlign(el) {
        var align = '', doc = el.ownerDocument;
        var walk = function (rules) {
            for (var i = 0; rules && i < rules.length; i++) {
                var r = rules[i];
                if (r.cssRules && !r.selectorText) { try { walk(r.cssRules); } catch (e) {} continue; }
                var sel = r.selectorText;
                if (!sel || sel.indexOf('::column') < 0 || !r.style) continue;
                var sa = r.style.scrollSnapAlign || r.style.getPropertyValue('scroll-snap-align');
                if (!sa) continue;
                var parts = sel.split(',');
                for (var k = 0; k < parts.length; k++) {
                    var p = parts[k].trim();
                    if (!/::column$/.test(p)) continue;
                    var base = p.slice(0, -8).trim() || '*';
                    try { if (el.matches(base)) align = sa; } catch (e) {}
                }
            }
        };
        var sheets = doc.styleSheets;
        for (var s = 0; sheets && s < sheets.length; s++) { try { walk(sheets[s].cssRules); } catch (e) {} }
        return align === 'none' ? '' : align;
    }
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
        /* (its writing mode: which axis is the block one, which run from the far side) */
        var wm_ = String(ecs.writingMode || ''), vert_ = /^(vertical|sideways)/.test(wm_), rtl_ = ecs.direction === 'rtl';
        var flipX = vert_ ? /-rl$/.test(wm_) : rtl_, flipY = vert_ ? rtl_ : false;
        var flipA = function (a, f) { return !f ? a : a === 'start' ? 'end' : a === 'end' ? 'start' : a; };
        var minX = flipX ? -maxX : 0, hiX = flipX ? 0 : maxX, minY = flipY ? -maxY : 0, hiY = flipY ? 0 : maxY;
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
            var ax_ = flipA(vert_ ? block : inline, flipX), ay_ = flipA(vert_ ? inline : block, flipY);
            var tcs = global.getComputedStyle(c), tz = c.currentCSSZoom || 1;
            if (axes.x && ax_ !== 'none') {
                var xlo = b.left - left + sx - elPx(tcs.scrollMarginLeft, 0, tz), xhi = b.right - left + sx + elPx(tcs.scrollMarginRight, 0, tz);
                out.x.push({ t: c, pos: Math.max(minX, Math.min(hiX, at(ax_, xlo, xhi, cw, padL, padR))), lo: xlo - padL, hi: xhi + padR });
            }
            if (axes.y && ay_ !== 'none') {
                var ylo = b.top - top + sy - elPx(tcs.scrollMarginTop, 0, tz), yhi = b.bottom - top + sy + elPx(tcs.scrollMarginBottom, 0, tz);
                out.y.push({ t: c, pos: Math.max(minY, Math.min(hiY, at(ay_, ylo, yhi, ch, padT, padB))), lo: ylo - padT, hi: yhi + padB });
            }
        }
        /* (its columns, as ::column says) */
        var cAlign = axes.x && !vert_ && !rtl_ && (ecs.columnCount !== 'auto' || ecs.columnWidth !== 'auto') ? columnSnapAlign(el) : '';
        if (cAlign) {
            var cps = cAlign.split(' '), cInline = cps[1] || cps[0];
            var pL = elPx(ecs.paddingLeft, 0, ez), pR = elPx(ecs.paddingRight, 0, ez);
            var W = cw - pL - pR, gap = ecs.columnGap === 'normal' ? elPx(ecs.fontSize, 0, ez) : elPx(ecs.columnGap, W, ez);
            var cc = parseInt(ecs.columnCount, 10), cwd = elPx(ecs.columnWidth, 0, ez), n = 1;
            if (cwd > 0) { n = Math.max(1, Math.floor((W + gap) / (cwd + gap))); if (cc > 0 && cc < n) n = cc; }
            else if (cc > 0) n = cc;
            var colw = (W - (n - 1) * gap) / n, stride = colw + gap;
            var total = stride > 0 ? Math.ceil((el.scrollWidth * ez - pL - pR + gap) / stride - 1e-6) : 0;
            for (var q = 0; cInline !== 'none' && q < total && q < 1000; q++) {
                var clo = pL + q * stride, chi = clo + colw;
                out.x.push({ t: columnToken(el, q), pos: Math.max(minX, Math.min(hiX, at(cInline, clo, chi, cw, padL, padR))), lo: clo - padL, hi: chi + padR });
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
    /* (where scrolling to t leaves it: its scroll-snap-align, start when none) */
    function alignedOffsetOf(sc, t) {
        var o = scrollOffsetOf(sc, t), a = String(global.getComputedStyle(t).scrollSnapAlign || 'none').split(/\s+/);
        var block = a[0], inline = a[1] || a[0], r = t.getBoundingClientRect();
        var at = function (v, size, view) { return v === 'center' ? (size - view) / 2 : v === 'end' ? size - view : 0; };
        return { x: o.x + at(inline, r.width, sc.clientWidth), y: o.y + at(block, r.height, sc.clientHeight) };
    }
    function markersUpdate(el) {
        var sc = scrollerSubject(el), list = global.__cyberScrollMarkerTargets(el);
        var maxX = sc.scrollWidth - sc.clientWidth, maxY = sc.scrollHeight - sc.clientHeight, best = null, bestD = Infinity, bestI = Infinity;
        /* (by the block axis first, then the inline one) */
        var vert_ = /^(vertical|sideways)/.test(global.getComputedStyle(sc).writingMode || '');
        for (var i = 0; i < list.length; i++) {
            var o = alignedOffsetOf(sc, list[i]);
            var dx = Math.abs(Math.max(0, Math.min(maxX, o.x)) - sc.scrollLeft), dy = Math.abs(Math.max(0, Math.min(maxY, o.y)) - sc.scrollTop);
            var db = vert_ ? dx : dy, di = vert_ ? dy : dx;
            if (db < bestD - 0.5 || (Math.abs(db - bestD) <= 0.5 && di < bestI)) { bestD = db; bestI = di; best = list[i]; }
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
        if (kind >= 38) {
            /* (a column's: the scroller to that column, as its ::column's scroll-snap-align has it) */
            var cs = global.getComputedStyle(el), n = parseInt(cs.columnCount, 10), gap = parseFloat(cs.columnGap) || 0;
            var pl = parseFloat(cs.paddingLeft) || 0, pr = parseFloat(cs.paddingRight) || 0, inner = el.clientWidth - pl - pr;
            if (!(n > 0)) { var cw0 = parseFloat(cs.columnWidth); n = cw0 > 0 ? Math.max(1, Math.floor((inner + gap) / (cw0 + gap))) : 1; }
            var colW = (inner - (n - 1) * gap) / n, at = hit[2] * (colW + gap), al = 'none';
            /* (its ::column rules' scroll-snap-align: the last that matches it) */
            var colRules = function (list) {
                for (var ri = 0; list && ri < list.length; ri++) {
                    var rr = list[ri];
                    if (rr.cssRules && !rr.selectorText) colRules(rr.cssRules);
                    if (!rr.selectorText || !rr.style) continue;
                    rr.selectorText.split(',').forEach(function (sel) {
                        var m = /^(.*)::column\s*$/.exec(sel.trim());
                        try { if (m && el.matches(m[1] || '*')) { var v = rr.style.getPropertyValue('scroll-snap-align'); if (v) al = v.trim(); } } catch (e) { }
                    });
                }
            };
            try { for (var si = 0; si < el.ownerDocument.styleSheets.length; si++) colRules(el.ownerDocument.styleSheets[si].cssRules); } catch (e) { }
            al = String(al).split(/\s+/); al = al[1] || al[0];
            var x = al === 'center' ? at + colW / 2 - inner / 2 : al === 'end' ? at + colW - inner : at;
            el.scrollLeft = Math.max(0, Math.min(el.scrollWidth - el.clientWidth, x));
            return true;
        }
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

    /* (document.dir: the root element's dir attribute, a known value or "") */
    if (global.Document && !('dir' in global.Document.prototype))
        Object.defineProperty(global.Document.prototype, 'dir', { configurable: true, enumerable: true,
            get: function () { var e = this.documentElement; var v = e && e.getAttribute ? String(e.getAttribute('dir') || '').toLowerCase() : ''; return v === 'ltr' || v === 'rtl' || v === 'auto' ? v : ''; },
            set: function (v) { var e = this.documentElement; if (e && e.setAttribute) e.setAttribute('dir', String(v)); } });
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
        try {
            var root = el.getRootNode ? el.getRootNode() : null;
            var v;
            if (root && root !== el.ownerDocument && root.host) {
                /* (in a shadow tree: its own classes are its tree's, not the document's; the document's ::part() rules') */
                v = '';
                var parts = String(el.getAttribute('part') || '').split(/\s+/);
                var walk = function (rules) {
                    for (var i = 0; i < rules.length; i++) {
                        var r = rules[i];
                        if (r.cssRules && !r.selectorText) { walk(r.cssRules); continue; }
                        var sel = r.selectorText, st = r.style;
                        if (!sel || !st) continue;
                        var cv = st.getPropertyValue('view-transition-class');
                        if (!cv) continue;
                        var list = sel.split(',');
                        for (var j = 0; j < list.length; j++) {
                            var m = /^(.*)::part\(([^)]*)\)\s*$/.exec(list[j].trim());
                            if (!m || parts.indexOf(m[2].trim()) < 0) continue;
                            try { if (m[1] && !root.host.matches(m[1])) continue; } catch (e2) { continue; }
                            v = cv;
                        }
                    }
                };
                var sheets = el.ownerDocument.styleSheets;
                for (var k = 0; k < sheets.length; k++) { try { walk(sheets[k].cssRules); } catch (e3) {} }
                /* (the shadow tree's own !important wins over the document's) */
                if (el.style && el.style.getPropertyPriority('view-transition-class') === 'important') v = el.style.getPropertyValue('view-transition-class');
                v = String(v).trim();
            } else v = String(global.getComputedStyle(el).getPropertyValue('view-transition-class') || '').trim();
            return v && v !== 'none' ? v.split(/\s+/) : [];
        }
        catch (e) { return []; }
    };
    var vtInline = function (from, to) {
        to.setAttribute('style', vtStyleText(from));
        to.removeAttribute('id');
    };
    var vtStyleText = function (from) {
        var cs = global.getComputedStyle(from), parts = [];
        for (var i = 0; i < cs.length; i++) {
            var p = cs[i];
            /* (physical properties only: logical ones and prefixed aliases say the same) */
            if (!p || p.indexOf('view-transition') >= 0 || /^(animation|transition|-webkit-|inset|contain-intrinsic)/.test(p) ||
                /(^|-)(block|inline)(-|$)/.test(p) || /-(start|end)-(start|end)-/.test(p)) continue;
            var v = cs.getPropertyValue(p);
            if (v !== '' && v != null) parts.push(p + ':' + v);
        }
        /* (the used sizes last: nothing before them resets them) */
        ['width', 'height'].forEach(function (p) { var v = cs.getPropertyValue(p); if (v && v !== 'auto') parts.push(p + ':' + v); });
        return parts.join(';');
    };
    /* (a deep clone of el as it looks now, named descendants left out) */
    var vtSnap = function (el, isRoot, keepNamed) {
        var c = el.cloneNode(true);
        /* (everything read first, then written: a write between reads would have each element's style worked out afresh) */
        var writes = [];
        /* (a large subtree: its descendants keep their classes and the sheets' rules, only the top's values written in;
         * every value of every element read is too slow there) */
        var big = el.getElementsByTagName('*').length > 400;
        var walk = function (a, b, top) {
            if (a.nodeType !== 1 || b.nodeType !== 1) return;
            var w = { b: b, text: top || !big ? vtStyleText(a) : null, hide: !top && !keepNamed && !!vtName(a), pos: null };
            writes.push(w);
            /* (a fixed descendant: where it is on screen, from the nearest positioned box of the clone) */
            var apos = !top && !isRoot ? global.getComputedStyle(a).position : '';
            if (apos === 'fixed' || apos === 'absolute') {
                var cb = a.parentNode;
                while (cb && cb !== el && cb.nodeType === 1 && global.getComputedStyle(cb).position === 'static') cb = cb.parentNode;
                /* (an absolute one: only when its containing block is outside, the clone standing in for it) */
                if (apos === 'absolute' && !(cb === el && global.getComputedStyle(el).position === 'static')) cb = null;
                if (cb && cb.nodeType === 1) {
                    var ar = a.getBoundingClientRect(), cr = cb.getBoundingClientRect();
                    w.pos = [['position', 'absolute'], ['left', (ar.left - cr.left - (cb.clientLeft || 0)) + 'px'], ['top', (ar.top - cr.top - (cb.clientTop || 0)) + 'px'],
                     ['right', 'auto'], ['bottom', 'auto'], ['margin', '0']];
                }
            }
            var ak = a.childNodes, bk = b.childNodes;
            for (var i = 0; i < ak.length && i < bk.length; i++) walk(ak[i], bk[i], false);
        };
        /* (the values as they are: no transition worked out again for each one read, the tree over) */
        var heldSnap = global.__cyberVtSnapping;
        global.__cyberVtSnapping = true;
        try { walk(el, c, true); } finally { global.__cyberVtSnapping = heldSnap; }
        writes.forEach(function (w) {
            if (w.text !== null) w.b.setAttribute('style', w.text);
            w.b.removeAttribute('id');
            if (w.hide) w.b.style.setProperty('visibility', 'hidden');
            if (w.pos) w.pos.forEach(function (d) { w.b.style.setProperty(d[0], d[1]); });
        });
        ['position:absolute', 'left:0', 'top:0', 'right:auto', 'bottom:auto', 'margin:0', 'transform:none', 'translate:none',
         'rotate:none', 'scale:none', 'float:none', 'visibility:visible', 'transform-style:flat'].forEach(function (d) {
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
            var ent = { name: n, el: el, cls: vtClasses(el), x: r.left, y: r.top, w: r.width, h: r.height, snap: vtSnap(el), vert: /^(vertical|sideways)/.test(String(cs.writingMode || '')),
                        bw: [cs.borderTopWidth, cs.borderRightWidth, cs.borderBottomWidth, cs.borderLeftWidth].map(function (v) { return parseFloat(v) || 0; }),
                        /* (its corners: what its group-children clip keeps) */
                        rad: ['top-left', 'top-right', 'bottom-right', 'bottom-left'].map(function (k) {
                            return 'border-' + k + '-radius:' + cs.getPropertyValue('border-' + k + '-radius') + ';corner-' + k + '-shape:' + (cs.getPropertyValue('corner-' + k + '-shape') || 'round');
                        }).join(';') };
            /* (transformed: the border box's size, placed so its transform takes it where it shows) */
            var tm = /^matrix\(([^)]*)\)$/.exec(String(cs.transform || '').trim());
            if (tm) {
                /* (a translation only, a transformed ancestor: as if none, the rect where it shows) */
                var tv = tm[1].split(',').map(parseFloat);
                if (tv.length === 6 && tv[0] === 1 && tv[1] === 0 && tv[2] === 0 && tv[3] === 1 &&
                    (function () { for (var a = el.parentElement; a; a = a.parentElement) { var t = global.getComputedStyle(a).transform; if (t && t !== 'none') return true; } return false; })()) tm = null;
            }
            if (tm && typeof el.offsetWidth === 'number') {
                var mv = tm[1].split(',').map(parseFloat), bw = el.offsetWidth, bh = el.offsetHeight;
                var to = String(cs.transformOrigin || '').split(/\s+/).map(parseFloat), ox = to[0] || 0, oy = to[1] || 0;
                if (mv.length === 6 && mv.every(isFinite) && bw > 0 && bh > 0) {
                    var ccx = r.left + r.width / 2, ccy = r.top + r.height / 2, qx = bw / 2 - ox, qy = bh / 2 - oy;
                    ent.x = ccx - ox - (mv[0] * qx + mv[2] * qy) - mv[4];
                    ent.y = ccy - oy - (mv[1] * qx + mv[3] * qy) - mv[5];
                    ent.w = bw; ent.h = bh;
                    /* (about the group's centre, its transform-origin) */
                    ent.m = 'translate(' + (ox - bw / 2) + 'px,' + (oy - bh / 2) + 'px) matrix(' + mv.join(',') + ') translate(' + (bw / 2 - ox) + 'px,' + (bh / 2 - oy) + 'px)';
                }
            } else if (!tm && typeof el.offsetWidth === 'number' && el.offsetWidth > 0 && el.offsetHeight > 0 &&
                       (Math.abs(r.width - el.offsetWidth) > 0.5 || Math.abs(r.height - el.offsetHeight) > 0.5) &&
                       (function () { for (var a = el.parentElement; a; a = a.parentElement) { var t = global.getComputedStyle(a).transform; if (t && t !== 'none') return true; } return false; })()) {
                /* (an ancestor's scale: its border box, scaled about its centre to where it shows) */
                var bw2 = el.offsetWidth, bh2 = el.offsetHeight, sx2 = r.width / bw2, sy2 = r.height / bh2;
                ent.x = r.left + r.width / 2 - bw2 / 2;
                ent.y = r.top + r.height / 2 - bh2 / 2;
                ent.w = bw2; ent.h = bh2;
                ent.m = 'matrix(' + sx2 + ',0,0,' + sy2 + ',0,0)';
            }
            out.push(ent);
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
        var vtNewLive = [];
        scope = scope || doc.documentElement;
        var scoped = scope !== doc.documentElement;
        var nu = vtCaptureState(doc, scope), rules = vtRules(doc, scope);
        var byName = {}, order = [];
        /* (the root in the old state only: the old page, the viewport's size, a group of its own) */
        if (!scoped && old.root && old.rootIn && !vtRootNamed(doc)) {
            var vw0 = global.innerWidth, vh0 = global.innerHeight, sd = doc.createElement('div');
            sd.setAttribute('style', 'position:relative;margin:0;width:' + vw0 + 'px;height:' + vh0 + 'px;overflow:hidden;background:' + old.root.bg);
            var bs0 = old.root.snap.cloneNode(true);
            bs0.style.setProperty('position', 'static');
            bs0.style.setProperty('margin', old.root.margin);
            sd.appendChild(bs0);
            var old2 = [{ name: old.rootName, el: doc.documentElement, cls: [], x: 0, y: 0, w: vw0, h: vh0, snap: sd }].concat(old.filter(function (o) { return o.name !== old.rootName; }));
            old2.root = null;
            old = old2;
        }
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
        order.forEach(function (n) { gval[n] = vtGroupVal(elOf[n]); if (gval[n] !== 'normal') contains[n] = true;   /* (any value but normal: it contains) */ });
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
        var vtEls = {}, vtAnims = [], vtFades = [], vtRunEls = [], vtTracked = [];
        /* (each pseudo's stand-in: its style before the page's rules, and the rules it took) */
        var vtTrack = function (el, kind, n, d) {
            var base = {};
            for (var i = 0; i < el.style.length; i++) { var p = el.style[i]; base[p] = el.style.getPropertyValue(p); }
            var last = {};
            for (var q in d) last[q] = d[q];
            vtTracked.push({ el: el, kind: kind, n: n, base: base, last: last });
        };
        /* (the images' UA fades as animations a page can seek: currentTime the opacity along it) */
        Object.defineProperty(global, '__cyberVtFades', { configurable: true, value: function () {
            var out = vtFades.filter(function (f) { return f._live(); });
            /* (and the keyframes run on the images, in the overlay's closed shadow root) */
            vtRunEls.forEach(function (el) { if (el.isConnected) try { out = out.concat(Array.from(el.getAnimations())); } catch (e) { } });
            return out;
        } });
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
        /* (a root image the rules place and size: its box from them) */
        var VT_GEO = ['position', 'left', 'top', 'right', 'bottom', 'width', 'height'];
        var geoOf = function (d) { return VT_GEO.some(function (k) { return d[k] != null && d[k] !== ''; }); };
        var geoSet = function (el, d) { VT_GEO.forEach(function (k) { if (d[k]) { try { el.style.setProperty(k, d[k]); } catch (e) { } } }); };
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
            /* (the old root's image: its transform from the rules, followed as they change) */
            vtEls['old:root'] = rg;
            if (ri.od.transform) rg.style.setProperty('transform', ri.od.transform);
            vtTrack(rg, 'old', 'root', ri.od);
            if (geoOf(ri.od)) geoSet(rg, ri.od);
        }
        /* (the new root placed elsewhere: the page as it is now, as an image there, the live page hidden) */
        var newRootSnap = rootIn && !scoped && oldRoot && geoOf(ri.nd) && gOp * nOp > 0.01 && (ri.nd.visibility || ri.ip.visibility || ri.g.visibility) !== 'hidden' ? vtRootSnap(doc) : null;
        if (newRootSnap) {
            var ng = doc.createElement('div');
            ng.setAttribute('style', 'position:absolute;left:0;top:0;width:100vw;height:100vh;overflow:hidden;background:' + newRootSnap.bg + ';opacity:' + (gOp * nOp));
            var nbs = newRootSnap.snap;
            nbs.style.setProperty('position', 'static');
            nbs.style.setProperty('margin', newRootSnap.margin);
            ng.appendChild(nbs);
            ov.appendChild(ng);
            geoSet(ng, ri.nd);
            rootShown = false;
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
                                    'px;transform:translate(' + (geo.x - sr.left) + 'px,' + (geo.y - sr.top) + 'px)' + (geo.m ? ' ' + geo.m : ''));
            vtTrack(g, 'group', n, f.g);
            vtApply(g, f.g);
            /* (the group's UA animation, old geometry to new, as one a page can seek: currentTime puts it there) */
            if (start && e.old && e.nu && f.ga.dur > 0 && !/width|height|transform/.test(Object.keys(f.g).join(' '))) {
                (function (g, eo, en, durMs, tf, name) {
                    var cur = 0;
                    var ease = function (x) {
                        var m = /steps\(\s*(\d+)\s*(?:,\s*([a-z-]+))?\s*\)/.exec(tf);
                        if (m) {
                            var n = +m[1], pos = m[2] || 'end';
                            if (pos === 'jump-none') return n > 1 ? Math.min(1, Math.floor(x * n) / (n - 1)) : (x >= 1 ? 1 : 0);
                            if (pos === 'start' || pos === 'jump-start') return Math.min(1, Math.ceil(x * n) / n);
                            if (pos === 'jump-both') return Math.min(1, (Math.floor(x * n) + 1) / (n + 1));
                            return Math.min(1, Math.floor(x * n) / n);
                        }
                        if (/step-start/.test(tf)) return 1;
                        if (/step-end/.test(tf)) return x >= 1 ? 1 : 0;
                        if (/linear/.test(tf)) return x;
                        var b = /cubic-bezier\(\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\)/.exec(tf);
                        var p = b ? [+b[1], +b[2], +b[3], +b[4]] : /ease-in-out/.test(tf) ? [0.42, 0, 0.58, 1] : /ease-in/.test(tf) ? [0.42, 0, 1, 1] : /ease-out/.test(tf) ? [0, 0, 0.58, 1] : [0.25, 0.1, 0.25, 1];
                        var lo = 0, hi = 1, t = x;
                        for (var it = 0; it < 40; it++) {
                            t = (lo + hi) / 2;
                            var bx = 3 * p[0] * t * (1 - t) * (1 - t) + 3 * p[2] * t * t * (1 - t) + t * t * t;
                            if (bx < x) lo = t; else hi = t;
                        }
                        return 3 * p[1] * t * (1 - t) * (1 - t) + 3 * p[3] * t * t * (1 - t) + t * t * t;
                    };
                    var put = function (k) {
                        var mx = function (a, b) { return a + (b - a) * k; };
                        g.style.setProperty('width', mx(eo.w, en.w) + 'px');
                        g.style.setProperty('height', mx(eo.h, en.h) + 'px');
                        var mm = k >= 1 ? en.m : eo.m;
                        g.style.setProperty('transform', 'translate(' + (mx(eo.x, en.x) - sr.left) + 'px,' + (mx(eo.y, en.y) - sr.top) + 'px)' + (mm ? ' ' + mm : ''));
                        /* (the images in it: to its width) */
                        var gwk = mx(eo.w, en.w);
                        g.querySelectorAll('*').forEach(function (sn) {
                            if (!sn.__cyberSnapW) return;
                            var sk = gwk / sn.__cyberSnapW;
                            if (Math.abs(sk - 1) > 0.001) { sn.style.setProperty('transform', 'scale(' + sk + ')'); sn.style.setProperty('transform-origin', '0 0'); }
                            else sn.style.removeProperty('transform');
                        });
                    };
                    vtFades.push({
                        _live: function () { return !!g.isConnected; },
                        get currentTime() { return cur; },
                        set currentTime(t) { cur = Math.max(0, Math.min(durMs, +t || 0)); put(ease(cur / durMs)); },
                        playState: 'paused',
                        pause: function () { this.playState = 'paused'; },
                        play: function () { this.playState = 'running'; },
                        finish: function () { this.currentTime = durMs; this.playState = 'finished'; },
                        cancel: function () { },
                        effect: { pseudoElement: '::view-transition-group(' + name + ')', getTiming: function () { return { duration: durMs }; },
                                  getComputedTiming: function () { return { duration: durMs, progress: cur / durMs }; } }
                    });
                })(g, e.old, e.nu, f.ga.dur * 1000, String(f.g['animation-timing-function'] || '') + ' ' + String(f.g['animation'] || ''), n);
            }
            /* (the group's width after its rules: what the images span) */
            var gw = /px\s*$/.test(g.style.width) ? parseFloat(g.style.width) : geo.w;
            if (!(gw >= 0)) gw = geo.w;
            var gh = /px\s*$/.test(g.style.height) ? parseFloat(g.style.height) : geo.h;
            if (!(gh >= 0)) gh = geo.h;
            var ip = doc.createElement('div');
            ip.setAttribute('style', 'position:absolute;left:0;top:0;width:100%;height:100%');
            vtTrack(ip, 'image-pair', n, f.ip);
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
                /* (the UA fade already over, the transition going on for others: it fills forwards, the old image out) */
                else if (!a.none && !a.custom && !a.long && kind === 'old' && !(d.opacity != null && d.opacity !== '')) o = 0;
                vtTrack(w, kind, n, d);
                vtApply(w, d);
                w.style.setProperty('opacity', String(o));
                /* (its own keyframes, paused: run on it, for the page to seek) */
                if (!a.none && a.custom && /paused/.test(String(d['animation-play-state'] || '') + ' ' + String(d.animation || '')))
                { for (var ap in d) if (/^animation/.test(ap)) { try { w.style.setProperty(ap, d[ap]); } catch (e) { } } vtRunEls.push(w); }
                if (!a.none && a.long && !a.custom && a.dur > 0) {
                    var durMs = a.dur * 1000, cur = 0, full = o;
                    var fade = {
                        _live: function () { return !!w.isConnected; },
                        get currentTime() { return cur; },
                        set currentTime(t) {
                            cur = Math.max(0, Math.min(durMs, +t || 0));
                            var k = cur / durMs;
                            w.style.setProperty('opacity', String(op0 === 0 ? k : 1 - k));
                        },
                        playState: 'paused',
                        pause: function () { this.playState = 'paused'; },
                        play: function () { this.playState = 'running'; },
                        finish: function () { this.currentTime = durMs; this.playState = 'finished'; },
                        cancel: function () { w.style.setProperty('opacity', String(full)); },
                        effect: { pseudoElement: '::view-transition-' + kind + '(' + n + ')', getTiming: function () { return { duration: durMs }; },
                                  getComputedTiming: function () { return { duration: durMs, progress: cur / durMs }; } }
                    };
                    vtFades.push(fade);
                }
                /* (its inline size the group's, in its own writing mode: a vertical one by the height) */
                var snap = s.snap.cloneNode(true), sc = s.vert && s.h > 0 ? gh / s.h : s.w > 0 ? gw / s.w : 1;
                /* (hidden with its group, image pair or itself) */
                var hid = [f.g, f.ip, d].some(function (x) { return x && String(x.visibility || '').trim() === 'hidden'; });
                if (hid) snap.style.setProperty('visibility', 'hidden');
                var fit = d['object-fit'] ? String(d['object-fit']).trim() : '';
                /* (object-view-box: inset(): the part of the snapshot taken as the image, its size the natural one) */
                var ovbm = d['object-view-box'] ? /^inset\(([^)]*)\)$/.exec(String(d['object-view-box']).trim()) : null;
                if (ovbm && s.w > 0 && s.h > 0) {
                    var iv = ovbm[1].trim().split(/\s+/).map(function (t, k) { var v = parseFloat(t); return isNaN(v) ? 0 : /%$/.test(t) ? v / 100 : v; });
                    var raw = ovbm[1].trim().split(/\s+/);
                    if (iv.length === 1) iv = [iv[0], iv[0], iv[0], iv[0]], raw = [raw[0], raw[0], raw[0], raw[0]];
                    else if (iv.length === 2) iv = [iv[0], iv[1], iv[0], iv[1]], raw = [raw[0], raw[1], raw[0], raw[1]];
                    else if (iv.length === 3) iv = [iv[0], iv[1], iv[2], iv[1]], raw = [raw[0], raw[1], raw[2], raw[1]];
                    var px = function (k) { return /%$/.test(raw[k]) ? iv[k] * (k % 2 ? s.w : s.h) : iv[k]; };
                    var vt = px(0), vr = px(1), vb = px(2), vl = px(3), vw = s.w - vl - vr, vh = s.h - vt - vb;
                    if (vw > 0 && vh > 0) {
                        var bw = gw, bh2 = w.offsetHeight > 0 ? w.offsetHeight : s.h * gw / s.w;
                        var ox = 0, oy = 0, kx = 1, ky = 1;
                        if (fit === 'none') {
                            var ops2 = d['object-position'] ? String(d['object-position']).trim().split(/\s+/) : [];
                            var pv = function (t, room) { if (t === 'left' || t === 'top') return 0; if (t === 'right' || t === 'bottom') return room; if (!t || t === 'center') return room / 2; if (/%$/.test(t)) return parseFloat(t) / 100 * room; var v = parseFloat(t); return isNaN(v) ? room / 2 : v; };
                            ox = pv(ops2[0], bw - vw); oy = pv(ops2[1], bh2 - vh);
                        } else { kx = bw / vw; ky = bh2 / vh; }
                        snap.style.setProperty('transform', 'translate(' + (ox - vl * kx) + 'px,' + (oy - vt * ky) + 'px) scale(' + kx + ',' + ky + ')');
                        snap.style.setProperty('transform-origin', '0 0');
                        /* (cut to its box only by its own paint containment or overflow) */
                        if (/paint|strict|content/.test(String(d.contain || '')) || /hidden|clip|scroll|auto/.test(String(d.overflow || ''))) w.style.setProperty('overflow', 'clip');
                        fit = 'ovb';
                    }
                }
                if (fit === 'ovb') { }
                else if (fit === 'none' && s.w > 0) {
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
                } else if ((fit === 'contain' || fit === 'cover') && s.w > 0 && s.h > 0 && gw > 0 && gh > 0 && Math.abs(gw / s.w - gh / s.h) > 0.001) {
                    /* (the whole of it in the box, or the box all covered; centred) */
                    var kc = fit === 'contain' ? Math.min(gw / s.w, gh / s.h) : Math.max(gw / s.w, gh / s.h);
                    snap.style.setProperty('transform', 'translate(' + (gw - s.w * kc) / 2 + 'px,' + (gh - s.h * kc) / 2 + 'px) scale(' + kc + ')');
                    snap.style.setProperty('transform-origin', '0 0');
                    if (fit === 'cover') w.style.setProperty('overflow', 'clip');
                    fit = 'ovb';
                } else if (Math.abs(sc - 1) > 0.001) { snap.style.setProperty('transform', 'scale(' + sc + ')'); snap.style.setProperty('transform-origin', '0 0'); }
                /* (its own width: scaled again when a seek changes its group's) */
                if (fit !== 'none' && fit !== 'ovb' && s.w > 0) Object.defineProperty(snap, '__cyberSnapW', { value: s.w, configurable: true });
                w.appendChild(snap);
                if (kind === 'new' && s.el && n !== 'root') vtNewLive.push({ el: s.el, snap: snap, sig: null, g: !start && !parentOf[n] ? g : null, n: n, s: s });
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
                    /* (over the group, with the element's border widths, solid and transparent: its children placed from the padding box) */
                    var pbw = ((byName[pn].nu || byName[pn].old) || {}).bw || [0, 0, 0, 0];
                    pg.kids.setAttribute('style', 'position:absolute;left:0;top:0;width:100%;height:100%;box-sizing:border-box;border-style:solid;border-color:transparent;border-width:' + pbw.map(function (v) { return v + 'px'; }).join(' ') + ';' + (((byName[pn].nu || byName[pn].old) || {}).rad || ''));
                    vtApply(pg.kids, vtDecls(rules, 'group-children', pn, (byName[pn].nu || byName[pn].old).cls || []));
                    pg.g.appendChild(pg.kids);
                }
                var pbw2 = ((byName[pn].nu || byName[pn].old) || {}).bw || [0, 0, 0, 0];
                g.style.setProperty('transform', 'translate(' + (geo.x - pg.geo.x - pbw2[3]) + 'px,' + (geo.y - pg.geo.y - pbw2[0]) + 'px)' + (geo.m ? ' ' + geo.m : ''));
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
        /* (the page's ::view-transition-* rules changing while it shows, by a class on the root: each pseudo's styles follow) */
        var vtOn = true, vtMo = null;
        /* (the new images: the live elements, taken again when they change) */
        var vtSig = function (el) { var r = el.getBoundingClientRect(); return el.outerHTML.replace(/visibility:\s*hidden\s*!important;?/g, '') + '|' + r.width + 'x' + r.height; };
        vtNewLive.forEach(function (t) { try { t.sig = vtSig(t.el); } catch (e) { t.sig = null; } });
        var vtNewCheck = function () {
            vtNewLive.forEach(function (t) {
                if (!t.el.isConnected || !t.snap.parentNode || t.sig === null) return;
                var sg;
                try { sg = vtSig(t.el); } catch (e) { return; }
                if (sg === t.sig) return;
                t.sig = sg;
                try {
                    var hv = t.el.style.getPropertyValue('visibility'), hp = t.el.style.getPropertyPriority('visibility');
                    var hold = null;
                    for (var hk = 0; hk < hidden.length; hk++) if (hidden[hk][0] === t.el) hold = hidden[hk];
                    t.el.style.setProperty('visibility', hold && hold[1] && !/hidden/.test(hold[1]) ? hold[1] : 'visible', 'important');
                    var ns;
                    try { ns = vtSnap(t.el); } finally { if (hv) t.el.style.setProperty('visibility', hv, hp); else t.el.style.removeProperty('visibility'); }
                    ['transform', 'transform-origin', 'visibility'].forEach(function (p) { var v = t.snap.style.getPropertyValue(p); if (v) ns.style.setProperty(p, v); });
                    if (t.snap.__cyberSnapW) Object.defineProperty(ns, '__cyberSnapW', { value: t.snap.__cyberSnapW, configurable: true });
                    t.snap.parentNode.replaceChild(ns, t.snap);
                    t.snap = ns;
                    if (t.g) {
                        /* (its group where the element now is) */
                        var cs2 = vtCaptureState(doc, scope).filter(function (x) { return x.name === t.n; })[0];
                        if (cs2 && Math.abs(cs2.w - t.s.w) < 0.5 && Math.abs(cs2.h - t.s.h) < 0.5) {
                            t.g.style.setProperty('transform', 'translate(' + (cs2.x - sr.left) + 'px,' + (cs2.y - sr.top) + 'px)' + (cs2.m ? ' ' + cs2.m : ''));
                        }
                    }
                } catch (e) { }
            });
        };
        (function () {
            var sig = function (rl) { return rl.map(function (r) { return r.kind + '|' + r.name + '|' + r.style.cssText; }).join('\n'); };
            var sig0 = sig(rules);
            var check = function () {
                if (!vtOn) return;
                var r2;
                try { r2 = vtRules(doc, scope); } catch (e) { return; }
                var s2 = sig(r2);
                if (s2 === sig0) return;
                sig0 = s2;
                vtTracked.forEach(function (t) {
                    var e = byName[t.n] || { nu: null, old: null };
                    var cl = (e.nu || e.old || {}).cls || [], ln = t.n === 'root' ? rootNm : t.n;
                    var d2 = vtDecls(r2, t.kind, ln, cl);
                    for (var p in t.last) if (!(p in d2) && !VT_SKIP.test(p)) {
                        if (p in t.base) t.el.style.setProperty(p, t.base[p]); else t.el.style.removeProperty(p);
                    }
                    var ch = {};
                    for (var q in d2) if (t.last[q] !== d2[q]) ch[q] = d2[q];
                    vtApply(t.el, ch);
                    /* (visibility: the snapshots under it carry their own, set the same) */
                    if ('visibility' in ch || ('visibility' in t.last && !('visibility' in d2))) {
                        var vv = d2.visibility || 'visible';
                        t.el.querySelectorAll('*').forEach(function (x) { if (x.style.getPropertyValue('visibility')) x.style.setProperty('visibility', vv); });
                    }
                    t.last = d2;
                });
            };
            try {
                vtMo = new global.MutationObserver(check);
                vtMo.observe(doc.documentElement, { attributes: true, attributeFilter: ['class', 'style'] });
            } catch (e) { vtMo = null; }
            /* (and each frame: a sheet changed through the CSSOM, adopted ones) */
            var fr = function () { if (!vtOn) return; check(); vtNewCheck(); global.requestAnimationFrame(fr); };
            global.requestAnimationFrame(fr);
        })();
        var undo = function () {
            vtOn = false;
            if (vtMo) try { vtMo.disconnect(); } catch (e) { }
            if (host.parentNode) host.parentNode.removeChild(host);
            if (ov.parentNode) ov.parentNode.removeChild(ov);
            hidden.forEach(function (h) { if (h[1]) h[0].style.setProperty('visibility', h[1], h[2]); else h[0].style.removeProperty('visibility'); });
            hidden = [];
        };
        /* (while animations started on its pseudo-elements run: held) */
        var vtLive = function () { return vtAnims.some(function (a) { try { return a.playState === 'running' || a.playState === 'paused'; } catch (e) { return false; } }); };
        if (brief) { var bleft = 6, btick = function () { if (vtLive() || (onDone.__held && onDone.__held())) { global.requestAnimationFrame(btick); return; } if (--bleft > 0) global.requestAnimationFrame(btick); else { undo(); onDone(); } }; global.requestAnimationFrame(btick); }
        return undo;
    };
    var VT_ACTIVE = { cur: null };
    var newViewTransition = function (update, types, doc, root, pre) {
        var vt = Object.create(ViewTransition.prototype), resolveReady, rejectReady, resolveFinished, rejectFinished,
            resolveDone, rejectDone, skipped = false, timedOut = false, vtWaits = 0;
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
        var vtFrozen = null;
        var vtThaw = function () {
            if (!vtFrozen) return;
            try { if (vtFrozen.c.parentNode) vtFrozen.c.parentNode.removeChild(vtFrozen.c); } catch (e) { }
            try { if (vtFrozen.op) root.style.setProperty('opacity', vtFrozen.op, vtFrozen.pr); else root.style.removeProperty('opacity'); } catch (e) { }
            vtFrozen = null;
        };
        var vtFreeze = function () {
            /* (scoped: the scope as it is now, over it, while the update runs) */
            if (doc || !root || !root.parentNode || !root.getBoundingClientRect) return;
            try {
                var r = root.getBoundingClientRect();
                var c = vtSnap(root, false, true);
                c.removeAttribute('id');
                Object.defineProperty(c, '__cyberVtOverlay', { value: true });
                [['position', 'fixed'], ['left', r.left + 'px'], ['top', r.top + 'px'], ['pointer-events', 'none']].forEach(function (d) { c.style.setProperty(d[0], d[1]); });
                var zi = global.getComputedStyle(root).zIndex;
                if (zi && zi !== 'auto') c.style.setProperty('z-index', zi);
                vtFrozen = { c: c, op: root.style.getPropertyValue('opacity'), pr: root.style.getPropertyPriority('opacity') };
                root.parentNode.insertBefore(c, root.nextSibling);
                root.style.setProperty('opacity', '0', 'important');
            } catch (e) { vtThaw(); }
        };
        var vtCapOld = function () {
            if (skipped) return;
            if (pre) { vtOld = pre; return; }   /* (a navigation's: the old document's, carried) */
            if (vdoc && (!doc || root === doc.documentElement)) {
                try { vtOld = vtCaptureState(vdoc, root); } catch (e) { vtOld = null; }
                if (vtOld && doc) { try { vtOld.root = vtRootSnap(doc); } catch (e) { vtOld.root = null; } }
                if (vtOld && doc) { try { vtOld.rootIn = vtRootNamed(doc); vtOld.rootName = vtName(doc.documentElement) || 'root'; } catch (e) { vtOld.rootIn = false; } }
            }
        };
        Promise.resolve().then(function () { vtCapOld(); if (!skipped) vtFreeze(); return update(); }).then(function (v) {
            vtThaw();
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
                finish.__held = function () { return vtWaits > 0; };   /* (waitUntil promises not settled) */
                try { vtUndo = vtShow(vdoc, vtOld, finish, root); } catch (e) { finish(); }
            } else resolveFinished();
        }, function (reason) {
            vtThaw();
            global.clearTimeout(timer);
            if (timedOut) return;
            rejectDone(reason);
            rejectReady(reason);
            rejectFinished(reason);
        });
        VT_STATE.set(vt, { done: done, ready: ready, finished: finished, types: newTypeSet(types),
                           root: root || (doc && doc.documentElement) || null,
                           wait: function (p) { vtWaits++; var dec = function () { vtWaits--; }; Promise.resolve(p).then(dec, dec); },
                           skip: function () { skipped = true; vtThaw(); if (vtUndo) { vtUndo(); vtUndo = null; resolveFinished(); } } });
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
            var st = vtOf(this);
            if (arguments.length < 1) throw new TypeError("Failed to execute 'waitUntil' on 'ViewTransition': 1 argument required, but only 0 present.");
            if (st.wait) st.wait(p);
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
        /* (the old state as text, for the next document: a snapshot is its tag, attributes and markup) */
        var vtSer = function (el) {
            var a = [];
            for (var i = 0; i < el.attributes.length; i++) a.push([el.attributes[i].name, el.attributes[i].value]);
            return { t: el.localName, ns: el.namespaceURI, a: a, h: el.innerHTML };
        };
        var vtDeser = function (d, s) {
            if (!s) return null;
            var html = s.ns === 'http://www.w3.org/1999/xhtml';
            /* (body and html: a div stands in, as a body in a div isn't parsed) */
            var el = html ? d.createElement(/^(body|html|head)$/.test(s.t) ? 'div' : s.t) : d.createElementNS(s.ns, s.t);
            s.a.forEach(function (p) { try { el.setAttribute(p[0], p[1]); } catch (e) { } });
            el.innerHTML = s.h;
            return el;
        };
        var vtCarryOut = function () {
            var d = global.document, st = vtCaptureState(d, d.documentElement);
            var o = { ents: st.map(function (e) {
                var c = {};
                for (var k in e) if (k !== 'el' && k !== 'snap') c[k] = e[k];
                c.snap = vtSer(e.snap);
                return c;
            }) };
            var rs = vtRootSnap(d);
            if (rs) o.root = { bg: rs.bg, x: rs.x, y: rs.y, margin: rs.margin, snap: vtSer(rs.snap) };
            o.rootIn = vtRootNamed(d);
            o.rootName = vtName(d.documentElement) || 'root';
            return JSON.stringify(o);
        };
        var vtCarryIn = function (json) {
            var o = JSON.parse(json), d = global.document;
            var old = o.ents.map(function (c) { c.snap = vtDeser(d, c.snap); c.el = d.documentElement; return c; }).filter(function (c) { return c.snap; });
            if (o.root) o.root.snap = vtDeser(d, o.root.snap);
            old.root = o.root && o.root.snap ? o.root : null;
            old.rootIn = o.rootIn;
            old.rootName = o.rootName;
            return old;
        };
        Object.defineProperty(global, '__cyberPageSwap', { configurable: true, value: function (url, same, how) {
            var rule = viewTransitionRule();
            var vt = same && rule && rule.navigation === 'auto' ? newViewTransition(nothing, rule.types) : null;
            var activation = { entry: { url: url }, from: { url: global.location.href }, navigationType: how || 'replace' };
            global.__cyberOutboundVT = vt;
            global.__cyberDispatchTrusted(global, new PageSwapEvent('pageswap', { viewTransition: vt, activation: activation }));
            if (!vt || vt.__skipped) return false;
            try { return vtCarryOut(); } catch (e) { return true; }
        } });
        /* (before the load event: this engine has no first frame to wait for) */
        Object.defineProperty(global, '__cyberPageReveal', { configurable: true, value: function () {
            /* (only after a document that opted in: reading every sheet's rules isn't free) */
            var rule = global.__cyberOldOptIn ? viewTransitionRule() : null;
            var carry = global.__cyberVtCarry, pre = null;
            global.__cyberVtCarry = null;
            if (carry && rule && rule.navigation === 'auto') { try { pre = vtCarryIn(carry); } catch (e) { pre = null; } }
            var d = global.document;
            var vt = rule && rule.navigation === 'auto' ? (pre ? newViewTransition(nothing, rule.types, d, d.documentElement, pre) : newViewTransition(nothing, rule.types)) : null;
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
        /* (no box of its own: nothing to scroll to) */
        try { if (!inner && global.getComputedStyle(el).display === "contents") return; } catch (e) {}
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
            if (a.nodeType !== 1 || a === root) continue;
            /* (the body: the viewport's overflow, unless containment on it or the root keeps its own) */
            if (a === d.body) { try { var cb0 = global.getComputedStyle(a).contain, cr0 = global.getComputedStyle(root).contain; if ((!cb0 || cb0 === "none") && (!cr0 || cr0 === "none")) continue; } catch (e) { continue; } }
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
            /* (no alignment asked for: its scroll-snap-align's, where it has one) */
            if (arg === undefined || arg === true) {
                try {
                    var sa = String(global.getComputedStyle(this).scrollSnapAlign || '').trim().split(/\s+/);
                    if (/^(start|end|center)$/.test(sa[0])) block = sa[0];
                    var si = sa.length > 1 ? sa[1] : sa[0];
                    if (/^(start|end|center)$/.test(si)) inline = si;
                } catch (e) { /* (as asked) */ }
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
            /* (a scroll-snap-align of its own: where a targeted scroll puts it) */
            var b0 = 'nearest', i0 = 'nearest';
            try {
                var sa0 = String(global.getComputedStyle(this).scrollSnapAlign || '').trim().split(/\s+/);
                if (/^(start|end|center)$/.test(sa0[0])) b0 = sa0[0];
                var si0 = sa0.length > 1 ? sa0[1] : sa0[0];
                if (/^(start|end|center)$/.test(si0)) i0 = si0;
            } catch (e) { /* (nearest) */ }
            scrollIntoViewOf(this, b0, i0);
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
        var w = doc.createTreeWalker(root, 5);
        for (var t = w.nextNode(); t; t = w.nextNode()) {
            if (!lr.intersectsNode(t)) continue;
            if (t.nodeType === 1) {
                /* (a replaced element the range holds whole: a segment of its own) */
                if (/^(img|video|canvas|svg|object|embed|iframe)$/i.test(t.localName || '') && t.parentNode) {
                    var ix = Array.prototype.indexOf.call(t.parentNode.childNodes, t);
                    if (lr.comparePoint(t.parentNode, ix) >= 0 && lr.comparePoint(t.parentNode, ix + 1) <= 0) flat.push(hi, t, 0, 1);
                }
                continue;
            }
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
        var low = text.toLowerCase().replace(/\u00a0/g, ' '), ti = -1;
        /* (a word character: a match starts and ends between words) */
        var wordy = function (c) { return !!c && /[0-9A-Za-z\u00c0-\u024f\u0370-\u03ff\u0400-\u04ff]/.test(c); };
        var findw = function (q, from) {
            for (var i = low.indexOf(q, from); i >= 0; i = low.indexOf(q, i + 1))
                if (!(wordy(q.charAt(0)) && wordy(low.charAt(i - 1))) && !(wordy(q.charAt(q.length - 1)) && wordy(low.charAt(i + q.length)))) return i;
            return -1;
        };
        var norm = function (x) { return x.toLowerCase().replace(/\u00a0/g, ' '); };
        for (var d = 0; d < dirs.length; d++) {
            if (dirs[d].slice(0, 5) !== 'text=') continue;
            var parts = dirs[d].slice(5).split(',').map(function (x) { try { return decodeURIComponent(x); } catch (e) { return x; } });
            var pre = parts.length && /-$/.test(parts[0]) ? norm(parts.shift().slice(0, -1)) : '';
            var suf = parts.length && /^-/.test(parts[parts.length - 1]) ? norm(parts.pop().slice(1)) : '';
            if (!parts.length || !parts[0]) continue;
            var q0 = norm(parts[0]), a = -1, b = -1;
            for (var from = 0; ; ) {
                var at0 = findw(q0, from);
                if (at0 < 0) break;
                from = at0 + 1;
                /* (a prefix: just before it, white space between) */
                if (pre) {
                    var pe = at0;
                    while (pe > 0 && /\s/.test(low.charAt(pe - 1))) pe--;
                    if (low.slice(pe - pre.length, pe) !== pre || wordy(low.charAt(pe - pre.length - 1))) continue;
                }
                var e0 = at0 + q0.length;
                if (parts.length > 1 && parts[1]) {
                    var eb = findw(norm(parts[1]), e0);
                    if (eb < 0) break;
                    e0 = eb + parts[1].length;
                }
                /* (a suffix: just after it) */
                if (suf) {
                    var ss = e0;
                    while (ss < low.length && /\s/.test(low.charAt(ss))) ss++;
                    if (low.slice(ss, ss + suf.length) !== suf || wordy(low.charAt(ss + suf.length))) continue;
                }
                a = at0; b = e0;
                break;
            }
            if (a < 0) continue;
            if (ti < 0) { ti = names.length; names.push('\u0001target-text'); prios.push(0x3ffffff0); }
            for (var k = 0; k < nodes.length; k++) {
                var ns = starts[k], ne = ns + nodes[k].data.length;
                if (ne <= a || ns >= b) continue;
                /* (in a canvas laid out for drawElementImage: not painted, what it matched no one's to see in the drawing) */
                var inCv = false;
                for (var an = nodes[k].parentNode, g = 0; an && an.nodeType === 1 && g < 256 && !inCv; an = an.parentNode, g++)
                    inCv = an.localName === 'canvas' && (an.hasAttribute('layoutsubtree') || an.hasAttribute('content'));
                if (inCv) continue;
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
    var hlInFlush = false, hlMo = null;
    var hlFlush = function () {
        if (hlInFlush) return;
        hlInFlush = true;
        try { hlFlush0(); } finally { hlInFlush = false; }
    };
    var hlFlush0 = function () {
        hlDirty = false;
        if (typeof global.__cyberHighlightSet !== 'function') return;
        var names = [], prios = [], flat = [];
        /* (highlights registered: the DOM's changes move their ranges, painted again then) */
        if (!hlMo && theRegistry && HL.get(theRegistry).size && typeof global.MutationObserver === 'function' && global.document) {
            hlMo = new global.MutationObserver(function () { try { if (!hlInFlush && HL.get(theRegistry).size) hlTouch(); } catch (e) { } });
            try { hlMo.observe(global.document, { childList: true, characterData: true, subtree: true }); } catch (e) { }
        }
        /* (no registry yet: the text fragment and the selection still) */
        if (theRegistry) HL.get(theRegistry).forEach(function (h, name) {
            var hi = names.length;
            names.push(name);
            /* (one made in another window: through its own methods) */
            var own = hlState.get(h);
            prios.push(own ? own.priority : (Number(h.priority) | 0));
            (HL.get(h) || h).forEach(function (r) {
                try {
                    /* (a range in another document: not this registry's to paint) */
                    var sc = r.startContainer, od = sc && (sc.nodeType === 9 ? sc : sc.ownerDocument);
                    if (od && global.document && od !== global.document) {
                        var ow = od.defaultView, oreg = ow && ow.CSS && ow.CSS.highlights;   /* (unless that document registers it too) */
                        if (!oreg || typeof oreg.get !== "function" || oreg.get(name) !== h) return;
                    }
                    hlRange(r, hi, flat);
                } catch (e) {}
            });
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
            /* (text of user-select: none is not selected; nor inert text, outside the topmost modal dialog or under inert) */
            var mdl = null;
            try { var mds = global.document.querySelectorAll(':modal'); mdl = mds.length ? mds[mds.length - 1] : null; } catch (e) { mdl = null; }
            for (var fq = flat.length - 4; fq >= f0; fq -= 4) {
                var tn = flat[fq + 1], pe = tn && tn.parentElement;
                var us = '';
                try { us = pe ? global.getComputedStyle(pe).getPropertyValue('user-select') : ''; } catch (e) { us = ''; }
                var inert_ = false;
                try { inert_ = !!(pe && ((mdl && !mdl.contains(tn)) || pe.closest('[inert]'))); } catch (e) { inert_ = false; }
                if (us === 'none' || inert_) flat.splice(fq, 4);
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
        onWin('hashchange', function () { hlTouch(); });
        onWin('load', function () { if (String(global.location && global.location.href).indexOf(':~:') >= 0) hlTouch(); });
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
        /* (fullscreen: the element in the top layer over the whole viewport, as a modal is shown, its style the UA sheet's
         * [data-cyber-fs] rules; document.fullscreenElement, fullscreenchange) */
        (function () {
            var EP = global.Element && global.Element.prototype, DocP = global.Document && global.Document.prototype;
            if (!EP || !DocP) return;
            var FS = null;
            var fsOwn = function (d) { try { var od = Object.getOwnPropertyDescriptor(d, 'fullscreenElement'); if (od && od.configurable) delete d.fullscreenElement; } catch (e) { } };
            Object.defineProperty(DocP, 'fullscreenElement', { configurable: true, enumerable: true, get: function () { return FS && FS.ownerDocument === this && FS.isConnected ? FS : null; } });
            Object.defineProperty(DocP, 'fullscreenEnabled', { configurable: true, enumerable: true, get: function () { return true; } });
            var fire = function (el) {
                var d = el.ownerDocument;
                global.setTimeout(function () {
                    try { (el.isConnected ? el : d).dispatchEvent(new global.Event('fullscreenchange', { bubbles: true, composed: true })); } catch (e) { }
                }, 0);
            };
            var leave = function () {
                var el = FS;
                if (!el) return null;
                FS = null;
                el.removeAttribute('data-cyber-fs');
                if (el.localName !== 'dialog' || !el.hasAttribute('open')) elState(el, ST_MODAL, false);
                return el;
            };
            method(EP, 'requestFullscreen', function requestFullscreen() {
                var el = this;
                return new global.Promise(function (res, rej) {
                    if (!el.isConnected || !el.ownerDocument || !el.ownerDocument.defaultView) { rej(new TypeError('The element is not connected.')); return; }
                    /* (a showing popover, a modal dialog: not made fullscreen) */
                    if (elState(el, ST_POPOVER) || (el.localName === 'dialog' && elState(el, ST_MODAL))) { rej(new TypeError('The element is already in the top layer.')); return; }
                    fsOwn(el.ownerDocument);
                    /* (the popovers shown: hidden, up to its topmost popover ancestor, as for a modal dialog) */
                    if (typeof global.__cyberHidePopoversFor === 'function') { try { global.__cyberHidePopoversFor(el); } catch (e) { } }
                    if (FS && FS !== el) leave();
                    FS = el;
                    el.setAttribute('data-cyber-fs', '');
                    elState(el, ST_MODAL, true);
                    fire(el);
                    global.setTimeout(res, 0);
                });
            });
            if (!EP.webkitRequestFullscreen) method(EP, 'webkitRequestFullscreen', function webkitRequestFullscreen() { return this.requestFullscreen(); });
            method(DocP, 'exitFullscreen', function exitFullscreen() {
                var d = this;
                return new global.Promise(function (res, rej) {
                    fsOwn(d);
                    var el = leave();
                    if (!el) { rej(new TypeError('Not in fullscreen.')); return; }
                    fire(el);
                    global.setTimeout(res, 0);
                });
            });
        })();
        function toggleEvent(el, type, oldState, newState, cancelable, source) {
            var C = global.ToggleEvent || global.Event;
            /* (a dialog's: the command button that opened or closed it, its source) */
            var src = source !== undefined ? source : (el.localName === 'dialog' ? global.__cyberDialogSource || null : undefined);
            var init = { cancelable: !!cancelable, oldState: oldState, newState: newState };
            if (src) init.source = src;
            return el.dispatchEvent(new C(type, init));
        }
        /* (a toggle event, in a task: one pending per element, its first old state kept) */
        var pendingToggleD = new WeakMap(), pendingPopToggle = new WeakMap();
        function queueToggle(el, oldState, newState, pop) {
            var pendingToggle = pop ? pendingPopToggle : pendingToggleD;   /* (a dialog's and a popover's toggle task trackers: apart) */
            var p = pendingToggle.get(el);
            if (p) { p.newState = newState; return; }
            p = { oldState: oldState, newState: newState, source: !pop && el.localName === 'dialog' ? global.__cyberDialogSource || null : undefined };
            pendingToggle.set(el, p);
            global.setTimeout(function () {
                pendingToggle.delete(el);
                if (p.oldState !== p.newState || (!pop && el.localName === 'dialog')) toggleEvent(el, 'toggle', p.oldState, p.newState, false, p.source === undefined ? null : p.source);   /* (a dialog's: even when it ends as it began) */
            }, 0);
        }

        var D = global.HTMLDialogElement, DP = D && D.prototype;
        if (DP && typeof DP.showModal !== 'function') {
            var RV = new WeakMap();
            var isDialog = function (el) { return el instanceof D; };
            Object.defineProperty(DP, 'open', { configurable: true, enumerable: true,
                get: function () { if (!isDialog(this)) throw new TypeError('Illegal invocation'); return this.hasAttribute('open'); },
                set: function (v) {
                    if (!isDialog(this)) throw new TypeError('Illegal invocation');
                    if (v) this.setAttribute('open', ''); else this.removeAttribute('open');
                    /* (opened so, or closed so: its close watcher too, the dialog setup and cleanup steps) */
                    if (v && this.isConnected && typeof global.__cyberCloseWatchAdd === 'function') global.__cyberCloseWatchAdd(this);
                    else if (!v && typeof global.__cyberCloseWatchRemove === 'function') global.__cyberCloseWatchRemove(this);
                } });
            Object.defineProperty(DP, 'returnValue', { configurable: true, enumerable: true,
                get: function () { if (!isDialog(this)) throw new TypeError('Illegal invocation'); return RV.has(this) ? RV.get(this) : ''; },
                set: function (v) { if (!isDialog(this)) throw new TypeError('Illegal invocation'); RV.set(this, String(v)); } });
            /* (closedBy: the attribute's state; auto, as the dialog is: close requests if modal, nothing if not) */
            Object.defineProperty(DP, 'closedBy', { configurable: true, enumerable: true,
                get: function () {
                    if (!isDialog(this)) throw new TypeError('Illegal invocation');
                    var v = String(this.getAttribute('closedby') || '').toLowerCase();
                    if (v === 'any' || v === 'closerequest' || v === 'none') return v;
                    return elState(this, ST_MODAL) ? 'closerequest' : 'none';
                },
                set: function (v) { if (!isDialog(this)) throw new TypeError('Illegal invocation'); this.setAttribute('closedby', String(v)); } });
            /* the focus delegate (HTML 6.6.3): an autofocus one, else the first focusable one in the flat tree */
            var NATIVE_FOCUS = /^(button|select|textarea|iframe|object|embed)$/;
            var focusableArea = function (el) {
                if (!el || el.nodeType !== 1) return false;
                var ln = el.localName, html = el.namespaceURI === 'http://www.w3.org/1999/xhtml';
                var native = html && (NATIVE_FOCUS.test(ln) || (ln === 'input' && String(el.getAttribute('type') || '').toLowerCase() !== 'hidden') ||
                    ((ln === 'a' || ln === 'area') && el.hasAttribute('href')) ||
                    (ln === 'summary' && el.parentElement && el.parentElement.localName === 'details' && el.parentElement.querySelector('summary') === el));
                if (!native && !el.hasAttribute('tabindex') && !(el.isContentEditable && (!el.parentElement || !el.parentElement.isContentEditable))) return false;
                if (el.disabled === true && html && /^(button|input|select|textarea)$/.test(ln)) return false;
                if (el.closest && el.closest('[inert]')) return false;
                try { if (!el.getClientRects().length) return false; } catch (e) { }
                return true;
            };
            /* (descendants in tree order: a shadow tree only through a host that delegates focus) */
            var kids = function (n) { return Array.prototype.slice.call(n.childNodes); };
            var autofocusIn = function (root) {
                var stack = kids(root).reverse();
                while (stack.length) {
                    var n = stack.pop();
                    if (n.nodeType !== 1) continue;
                    if (n.hasAttribute('autofocus') && focusableArea(n)) return n;
                    stack.push.apply(stack, kids(n).reverse());
                }
                return null;
            };
            var focusDelegate = function (target, keyboard) {
                var a = autofocusIn(target);
                if (a) return a;
                var stack = kids(target).reverse();
                while (stack.length) {
                    var n = stack.pop();
                    if (n.nodeType !== 1) continue;
                    /* (a dialog's: keyboard focusable, sequentially) */
                    if (focusableArea(n) && (!keyboard || n.tabIndex >= 0)) return n;
                    if (n.shadowRoot && n.shadowRoot.delegatesFocus) { var d = focusDelegate(n.shadowRoot, keyboard); if (d) return d; }
                    stack.push.apply(stack, kids(n).reverse());
                }
                return null;
            };
            var PREV = new WeakMap();
            var dialogFocus = function (dlg) {
                /* (the dialog focusing steps: the document's autofocus processed flag set) */
                try { global.__cyberAutofocusDone = true; } catch (e) { }
                var doc = dlg.ownerDocument, prev = doc && doc.activeElement;
                PREV.set(dlg, prev && prev !== doc.body ? prev : null);
                /* (an autofocus dialog: itself) */
                var control = dlg.hasAttribute('autofocus') ? dlg : focusDelegate(dlg, true) || dlg;
                try { control.focus(); } catch (e) { /* (not focusable) */ }
            };
            /* (the close watchers: open dialogs and auto/hint popovers, newest last; Escape asks the newest to close,
             * a click outside a closedby=any dialog closes it, HTML 6.x close watchers and dialog light dismiss) */
            var CW = [];
            var cwRemove = function (el) { var i = CW.indexOf(el); if (i >= 0) CW.splice(i, 1); };
            var cwWatched = new WeakSet();
            var cwAdd = function (el) {
                cwRemove(el);
                CW.push(el);
                var doc = el.ownerDocument;
                if (!doc || cwWatched.has(doc) || typeof doc.addEventListener !== 'function') return;
                cwWatched.add(doc);
                var downIn = null, lastUp = 0;
                /* (dialogs opened by their open attribute: watched too, the dialog setup steps) */
                var adopt = function () {
                    var ods = doc.querySelectorAll('dialog[open]');
                    for (var oi = 0; oi < ods.length; oi++) if (CW.indexOf(ods[oi]) < 0) CW.push(ods[oi]);
                };
                doc.addEventListener('keydown', function (e) {
                    if ((e.key !== 'Escape' && e.key !== 'Esc') || e.defaultPrevented) return;
                    adopt();
                    for (var i = CW.length - 1; i >= 0; i--) {
                        var w = CW[i];
                        if (w.ownerDocument !== doc) continue;
                        if (w.localName === 'dialog') {
                            if (!w.hasAttribute('open')) { CW.splice(i, 1); continue; }
                            if (w.closedBy !== 'none') { try { w.requestClose(); } catch (x) { } }
                            return;
                        }
                        if (global.CloseWatcher && w instanceof global.CloseWatcher) { if (w.__cyberActive && w.__cyberActive()) { w.requestClose(); return; } CW.splice(i, 1); continue; }
                        if (typeof global.__cyberPopoverEscape === 'function' && global.__cyberPopoverEscape(w)) return;
                        CW.splice(i, 1);
                    }
                }, false);
                var topDialog = function () {
                    adopt();
                    for (var i = CW.length - 1; i >= 0; i--) {
                        var w = CW[i];
                        if (w.ownerDocument === doc && w.localName === 'dialog' && w.hasAttribute('open')) return w;
                        if (w.ownerDocument === doc && w.localName !== 'dialog') return null;
                    }
                    return null;
                };
                /* (a click in the dialog: in its subtree; the dialog itself (its ::backdrop) only inside its box) */
                var inDlg = function (d, e) {
                    if (!d.contains(e.target)) return false;
                    if (e.target !== d || typeof e.clientX !== 'number') return true;
                    var r = d.getBoundingClientRect();
                    return e.clientX >= r.left && e.clientX < r.right && e.clientY >= r.top && e.clientY < r.bottom;
                };
                /* (HTML "light dismiss open dialogs": the topmost open dialog, unless the click was in it (its nearest clicked
                 * dialog, through the flat tree; a modal one's backdrop is outside), down and up in the same one) */
                var topOpenDialog = function () {
                    adopt();
                    for (var i = CW.length - 1; i >= 0; i--) {
                        var w = CW[i];
                        if (w.ownerDocument === doc && w.localName === 'dialog' && w.hasAttribute('open')) return w;
                    }
                    return null;
                };
                var nearestClicked = function (e) {
                    var cp = typeof e.composedPath === 'function' ? e.composedPath() : null, t = cp && cp.length ? cp[0] : e.target;
                    if (t && t.localName === 'dialog' && t.hasAttribute('open') && typeof e.clientX === 'number' && t.matches(':modal')) {
                        var r = t.getBoundingClientRect();
                        if (!(e.clientX >= r.left && e.clientX < r.right && e.clientY >= r.top && e.clientY < r.bottom)) return null;
                    }
                    for (var n = t, g = 0; n && g < 4096; g++) {
                        if (n.nodeType === 1 && n.localName === 'dialog' && n.hasAttribute('open')) return n;
                        n = n.assignedSlot || n.parentNode || n.host || null;
                    }
                    return null;
                };
                var NONE = {};
                downIn = NONE;
                doc.addEventListener('pointerdown', function (e) { downIn = topOpenDialog() ? nearestClicked(e) : NONE; }, true);
                doc.addEventListener('mousedown', function (e) { if (downIn === NONE) downIn = topOpenDialog() ? nearestClicked(e) : NONE; }, true);
                var up = function (e) {
                    var now = Date.now();
                    if (e.type === 'mouseup' && now - lastUp < 50) return;
                    lastUp = now;
                    var top = topOpenDialog(), was = downIn;
                    downIn = NONE;
                    if (!top || was === NONE) return;
                    var anc = nearestClicked(e);
                    if (anc !== was || anc === top || top.closedBy !== 'any') return;
                    try { top.requestClose(); } catch (x) { }
                };
                doc.addEventListener('pointerup', up, true);
                doc.addEventListener('mouseup', up, true);
            };
            Object.defineProperty(global, '__cyberCloseWatchAdd', { configurable: true, value: cwAdd });
            Object.defineProperty(global, '__cyberCloseWatchRemove', { configurable: true, value: cwRemove });
            method(DP, 'show', function () {
                if (!isDialog(this)) throw new TypeError('Illegal invocation');
                if (this.hasAttribute('open')) {
                    if (elState(this, ST_MODAL)) throw invalidState('The dialog is already open as a modal dialog.');
                    return;
                }
                if (!toggleEvent(this, 'beforetoggle', 'closed', 'open', true) || this.hasAttribute('open')) return;
                this.setAttribute('open', '');
                queueToggle(this, 'closed', 'open');
                cwAdd(this);
                dialogFocus(this);
            });
            method(DP, 'showModal', function () {
                if (!isDialog(this)) throw new TypeError('Illegal invocation');
                if (this.hasAttribute('open')) {
                    if (elState(this, ST_MODAL)) return;
                    throw invalidState('The dialog is already open as a non-modal dialog.');
                }
                if (!this.isConnected) throw invalidState('The dialog is not connected.');
                /* (its document not fully active: one with no browsing context) */
                if (!this.ownerDocument || !this.ownerDocument.defaultView) throw invalidState('The dialog\'s document is not fully active.');
                if (elState(this, ST_POPOVER)) throw invalidState('The dialog is already open as a popover.');
                if (!toggleEvent(this, 'beforetoggle', 'closed', 'open', true) || this.hasAttribute('open') || !this.isConnected || elState(this, ST_POPOVER)) return;
                this.setAttribute('open', '');
                elState(this, ST_MODAL, true);
                /* (the modal dialogs in the order shown: the topmost last, html_element_api.c's focusable check) */
                var mord = global.__cyberModalOrder || (global.__cyberModalOrder = []), mix = mord.indexOf(this);
                if (mix >= 0) mord.splice(mix, 1);
                mord.push(this);
                /* (the popovers shown: hidden, up to the dialog's topmost popover ancestor, else all of them) */
                if (typeof global.__cyberHidePopoversFor === 'function') { try { global.__cyberHidePopoversFor(this); } catch (e) { } }
                queueToggle(this, 'closed', 'open');
                cwAdd(this);
                dialogFocus(this);
            });
            var closeDialog = function (el, rv) {
                cwRemove(el);
                if (!el.hasAttribute('open')) return;
                toggleEvent(el, 'beforetoggle', 'open', 'closed', false);
                if (!el.hasAttribute('open')) return;
                var doc = el.ownerDocument, act = doc && doc.activeElement;
                var hadFocus = act && (act === el || el.contains(act) || (act.shadowRoot && el.contains(act)));
                el.removeAttribute('open');
                elState(el, ST_MODAL, false);
                var mord2 = global.__cyberModalOrder, mix2 = mord2 ? mord2.indexOf(el) : -1;
                if (mix2 >= 0) mord2.splice(mix2, 1);
                if (rv !== undefined && rv !== null) RV.set(el, String(rv));
                queueToggle(el, 'open', 'closed');
                /* (focus back where it was, if it was in the dialog) */
                var prev = PREV.get(el);
                PREV.delete(el);
                if (hadFocus || (act && act === doc.body)) {
                    if (prev && prev.isConnected) { try { prev.focus({ preventScroll: true }); } catch (e) { } }
                    else if (act && act.blur && act !== doc.body) { try { act.blur(); } catch (e) { } }
                }
                global.setTimeout(function () {
                    var ev = new global.Event('close');
                    if (global.__cyberDispatchTrusted) global.__cyberDispatchTrusted(el, ev); else el.dispatchEvent(ev);
                }, 0);
            };
            method(DP, 'close', function (rv) {
                if (!isDialog(this)) throw new TypeError('Illegal invocation');
                closeDialog(this, rv);
            });
            method(DP, 'requestClose', function (rv) {
                if (!isDialog(this)) throw new TypeError('Illegal invocation');
                if (!this.hasAttribute('open')) return;
                /* (no close watcher in a document not fully active) */
                if (!this.ownerDocument || !this.ownerDocument.defaultView) return;
                if (!this.dispatchEvent(new global.Event('cancel', { cancelable: true }))) return;
                closeDialog(this, rv);
            });
        }

        /* popovers: the popover attribute's element, shown and hidden */
        var HP = global.HTMLElement && global.HTMLElement.prototype;
        if (!HP || typeof HP.showPopover === 'function') return;
        function popoverKind(el) {
            var v = el.getAttribute('popover');
            if (v === null) return el.localName === 'menulist' && el.namespaceURI === 'http://www.w3.org/1999/xhtml' ? 'auto' : null;   /* (a menulist: a popover of its own) */
            v = v.toLowerCase();
            /* (auto: the keyword or empty; hint; anything else, the invalid value default, manual) */
            return v === '' || v === 'auto' ? 'auto' : v === 'hint' ? 'hint' : 'manual';
        }
        var isHTML = function (el) { return el instanceof global.HTMLElement; };
        function showing(el) { return !!elState(el, ST_POPOVER); }
        function check(el, open) {
            if (popoverKind(el) === null) throw new global.DOMException('Not a popover.', 'NotSupportedError');
            if (!el.isConnected) throw invalidState('The popover is not connected.');
            if (el.localName === 'dialog' && el.hasAttribute('open') && !showing(el)) throw invalidState('The dialog is already open.');
        }
        /* (those showing: one removed from its document, or whose popover attribute changes state, hides at once, dom_api.c) */
        var shownSet = new Set();
        var noteShown = function (el, on) {
            if (on) shownSet.add(el); else shownSet.delete(el);
            if (shownSet.size) {
                if (!global.__cyberPopoverCheck) Object.defineProperty(global, '__cyberPopoverCheck', { configurable: true, writable: true, value: function () {
                    shownSet.forEach(function (p) { if (!p.isConnected) hide(p, false); });
                } });
            } else if (global.__cyberPopoverCheck) delete global.__cyberPopoverCheck;
        };
        var kindOfValue = function (v) {
            if (v === null || v === undefined) return null;
            v = String(v).toLowerCase();
            return v === '' || v === 'auto' ? 'auto' : v === 'hint' ? 'hint' : 'manual';
        };
        Object.defineProperty(global, '__cyberPopoverAttr', { configurable: true, value: function (el, old) {
            if (showing(el) && kindOfValue(old) !== popoverKind(el)) hide(el, true, true);
        } });
        function hide(el, events) {
            if (!showing(el)) return;
            if (events) toggleEvent(el, 'beforetoggle', 'open', 'closed', false);
            if (!showing(el)) return;
            elState(el, ST_POPOVER, false);
            noteShown(el, false);
            if (events) queueToggle(el, 'open', 'closed', true);
        }
        /* (HTML 6.12: each document's showing auto and hint popover lists, oldest first; who is being shown or hidden) */
        var POPLISTS = new WeakMap(), busy = new WeakSet();
        var lists = function (doc) { var l = POPLISTS.get(doc); if (!l) { l = { auto: [], hint: [], down: null }; POPLISTS.set(doc, l); } return l; };
        var unlist = function (el) {
            var l = POPLISTS.get(el.ownerDocument);
            if (!l) return;
            var k = l.auto.indexOf(el); if (k >= 0) l.auto.splice(k, 1);
            k = l.hint.indexOf(el); if (k >= 0) l.hint.splice(k, 1);
        };
        function checkValidity(el, expectShowing, throwEx, expectedDoc) {
            if (popoverKind(el) === null) { if (throwEx) throw new global.DOMException('Not a popover.', 'NotSupportedError'); return false; }
            /* (the state asked for first: a popover hidden is hidden, connected or not) */
            if (expectShowing !== showing(el)) return false;
            if (!el.isConnected || (expectedDoc && el.ownerDocument !== expectedDoc) || (el.localName === 'dialog' && elState(el, ST_MODAL)) ||
                (!expectShowing && el.localName === 'dialog' && el.hasAttribute('open') && !showing(el))) {
                if (throwEx) throw invalidState(!el.isConnected ? 'The popover is not connected.' : 'The popover is in a state it cannot be toggled from.');
                return false;
            }
            return true;
        }
        /* (the flat tree's parent) */
        var flatParent = function (n) {
            if (n.assignedSlot) return n.assignedSlot;
            var p = n.parentNode;
            if (p && p.nodeType === 11 && p.host) return p.host;
            return p;
        };
        var openPopover = function (n) {
            for (; n; n = flatParent(n))
                if (n.nodeType === 1 && isHTML(n) && (popoverKind(n) === 'auto' || popoverKind(n) === 'hint') && showing(n)) return n;
            return null;
        };
        /* (an invoker's target popover, for the nearest invoker at or above n, when it is open) */
        var invokerTarget = function (n) {
            for (; n; n = flatParent(n)) {
                if (n.nodeType !== 1 || !isHTML(n)) continue;
                var t = null;
                try { t = 'popoverTargetElement' in n ? n.popoverTargetElement : null; } catch (e) { t = null; }
                /* (a button whose command is a popover's: its commandfor element) */
                if (!t && n.localName === 'button') {
                    try {
                        var cmd = String(n.getAttribute('command') || '').toLowerCase();
                        if (cmd === 'toggle-popover' || cmd === 'show-popover' || cmd === 'hide-popover') t = n.commandForElement || null;
                    } catch (e) { t = null; }
                }
                /* (an interest invoker: the popover it shows interest in, its ::interest-button too) */
                if (!t && n.hasAttribute('interestfor')) {
                    try { var rt_ = n.getRootNode && n.getRootNode(); t = rt_ && rt_.getElementById ? rt_.getElementById(n.getAttribute('interestfor')) : null; } catch (e) { t = null; }
                }
                if (t && (popoverKind(t) === 'auto' || popoverKind(t) === 'hint') && showing(t)) return t;
            }
            return null;
        };
        function topmostAncestor(np, list, invoker, isPopover) {
            var pos = new Map();
            list.forEach(function (p, i) { pos.set(p, i); });
            if (isPopover) pos.set(np, list.length);
            var top = null;
            var chk = function (c) {
                if (!c) return;
                var a = openPopover(c);
                if (!a || a === np || !pos.has(a)) return;
                if (isPopover && pos.get(a) >= pos.get(np)) return;
                if (top === null || pos.get(top) < pos.get(a)) top = a;
            };
            chk(flatParent(np));
            chk(invoker);
            return top;
        }
        function closeEntire(list, focusPrev, fireEvents) {
            for (var guard = 0; list.length && guard < 1000; guard++) {
                var last = list[list.length - 1];
                hidePopoverAlg(last, focusPrev, fireEvents, false);
                if (list.length && list[list.length - 1] === last) { list.pop(); elState(last, ST_POPOVER, false); noteShown(last, false); }
            }
        }
        function hideStackUntil(endpoint, list, focusPrev, fireEvents) {
            var repeating = false;
            for (var guard = 0; guard < 100; guard++) {
                var lastToHide = null;
                for (var i = list.length - 1; i >= 0; i--) { if (list[i] === endpoint) break; lastToHide = list[i]; }
                if (!lastToHide) return;
                for (var g2 = 0; showing(lastToHide) && list.length && g2 < 1000; g2++) {
                    var last = list[list.length - 1];
                    hidePopoverAlg(last, focusPrev, fireEvents, false);
                    if (list.length && list[list.length - 1] === last) { list.pop(); elState(last, ST_POPOVER, false); noteShown(last, false); }
                }
                repeating = list.indexOf(endpoint) >= 0 && list[list.length - 1] !== endpoint;
                if (!repeating) return;
                fireEvents = false;
            }
        }
        function hideAllUntil(endpoint, focusPrev, fireEvents) {
            var isDoc = endpoint && endpoint.nodeType === 9;
            if (!isDoc && !showing(endpoint)) return;
            var l = lists(isDoc ? endpoint : endpoint.ownerDocument);
            if (isDoc) { closeEntire(l.hint, focusPrev, fireEvents); closeEntire(l.auto, focusPrev, fireEvents); return; }
            if (l.hint.indexOf(endpoint) >= 0) { hideStackUntil(endpoint, l.hint, focusPrev, fireEvents); return; }
            /* (an auto one: the autos above it; hints opened apart from it stay, as engines have it) */
            if (l.auto.indexOf(endpoint) < 0) return;
            hideStackUntil(endpoint, l.auto, focusPrev, fireEvents);
        }
        /* (each popover's previously focused element, for its hiding to go back to) */
        var popPrevFocus = new WeakMap();
        var tryFocus = function (c) {
            var d = c.ownerDocument;
            try { global.HTMLElement.prototype.focus.call(c, { preventScroll: true }); } catch (e) {
                try { c.focus({ preventScroll: true }); } catch (e2) { return false; } }
            var a = d.activeElement;
            for (var g = 0; a && a.shadowRoot && a.shadowRoot.activeElement && g < 32; g++) a = a.shadowRoot.activeElement;
            return a === c || d.activeElement === c;
        };
        /* (HTML "popover focusing steps": a dialog's own initial focus; else the popover, with autofocus, or its autofocus delegate) */
        var popoverFocusingSteps = function (el) {
            var isDialog = el.localName === 'dialog' && el.namespaceURI === 'http://www.w3.org/1999/xhtml';
            /* (a shadow host that delegates focus: its autofocus delegate in there) */
            var sr = el.shadowRoot;
            if (el.hasAttribute('autofocus') && sr && sr.delegatesFocus) {
                var inner = sr.querySelectorAll('[autofocus]');
                for (var s2 = 0; s2 < inner.length; s2++) if (tryFocus(inner[s2])) return;
            }
            if (el.hasAttribute('autofocus')) { if (tryFocus(el)) return; if (!isDialog) return; }
            var auto = el.querySelectorAll('[autofocus]');
            for (var i = 0; i < auto.length; i++) {
                /* (not inside a nested popover or dialog of its own) */
                if (auto[i].closest('[popover], dialog') !== el && auto[i].closest('[popover], dialog') !== auto[i]) continue;
                if (tryFocus(auto[i])) return;
            }
            if (!isDialog) return;
            var all = el.querySelectorAll('*');
            for (var k = 0; k < all.length; k++) if (all[k].tabIndex >= 0 && !all[k].hasAttribute('disabled') && tryFocus(all[k])) return;
        };
        Object.defineProperty(global, '__cyberHidePopoversFor', { configurable: true, value: function (dlg) {
            var d = dlg.ownerDocument, ls = lists(d);
            if (!ls.auto.length && !ls.hint.length) return;
            var until = topmostAncestor(dlg, ls.auto, null, false) || d;
            hideAllUntil(until, false, true);
        } });
        function showPopoverAlg(el, throwEx, invoker) {
            if (!checkValidity(el, false, throwEx, null)) return;
            var doc = el.ownerDocument, nested = busy.has(el), fire = !nested;
            busy.add(el);
            var cleanup = function () { if (!nested) busy.delete(el); };
            try {
                if (!toggleEvent(el, 'beforetoggle', 'closed', 'open', true)) return;
                if (!checkValidity(el, false, throwEx, doc)) return;
                watchDoc(doc);
                if (global.__cyberWatchPopover) global.__cyberWatchPopover(doc);
                var orig = popoverKind(el), stack = null, l = lists(doc);
                var autoA = topmostAncestor(el, l.auto, invoker, true), hintA = topmostAncestor(el, l.hint, invoker, true);
                if (orig === 'auto') { closeEntire(l.hint, false, fire); hideAllUntil(autoA || doc, false, fire); stack = 'auto'; }
                else if (orig === 'hint') {
                    if (hintA) { hideAllUntil(hintA, false, fire); stack = 'hint'; }
                    else { closeEntire(l.hint, false, fire); if (autoA) { hideAllUntil(autoA, false, fire); stack = 'auto'; } else stack = 'hint'; }
                }
                if (orig === 'auto' || orig === 'hint') {
                    if (orig !== popoverKind(el)) { if (throwEx) throw invalidState('The popover type changed while it was being shown.'); return; }
                    if (!checkValidity(el, false, throwEx, doc)) return;
                }
                elState(el, ST_POPOVER, true);
                noteShown(el, true);
                if (stack) l[stack].push(el);
                if (stack && typeof global.__cyberCloseWatchAdd === 'function') global.__cyberCloseWatchAdd(el);
                /* (focus: the popover's focusing steps; an auto one's hiding goes back to what had it) */
                popPrevFocus.delete(el);
                var origFocus = doc.activeElement;
                try { popoverFocusingSteps(el); } catch (e) { }
                if (orig === 'auto' && popoverKind(el) !== null && origFocus) popPrevFocus.set(el, origFocus);
                queueToggle(el, 'closed', 'open', true);
            } finally { cleanup(); }
        }
        function hidePopoverAlg(el, focusPrev, fireEvents, throwEx) {
            if (!checkValidity(el, true, throwEx, null)) return;
            var doc = el.ownerDocument, l = lists(doc), nested = busy.has(el);
            busy.add(el);
            if (nested) fireEvents = false;
            try {
                var k = popoverKind(el);
                if (k === 'auto' || k === 'hint') {
                    hideAllUntil(el, focusPrev, fireEvents);
                    if (!checkValidity(el, true, throwEx, null)) return;
                }
                var autoTop = l.auto.length && l.auto[l.auto.length - 1] === el;
                if (fireEvents) {
                    toggleEvent(el, 'beforetoggle', 'open', 'closed', false);
                    if (autoTop && l.auto[l.auto.length - 1] !== el) hideAllUntil(el, focusPrev, false);
                    if (!checkValidity(el, true, throwEx, null)) return;
                }
                unlist(el);
                elState(el, ST_POPOVER, false);
                noteShown(el, false);
                if (typeof global.__cyberCloseWatchRemove === 'function') global.__cyberCloseWatchRemove(el);
                if (fireEvents) queueToggle(el, 'open', 'closed', true);
                /* (the previously focused element: focus back to it, when focus was inside the popover) */
                var prevF = popPrevFocus.get(el);
                popPrevFocus.delete(el);
                if (focusPrev && prevF && prevF.isConnected) {
                    var af = doc.activeElement;
                    var inside = false;
                    for (var n = af, g2 = 0; n && g2 < 256; g2++) { if (n === el) { inside = true; break; } n = n.parentNode || n.host; }
                    if (inside) tryFocus(prevF);
                }
            } finally { if (!nested) busy.delete(el); }
        }
        /* (hidden as it stops being one, or leaves its document: at once, no events) */
        function hide(el, events, focusPrev) {
            if (!showing(el)) return;
            if (el.isConnected && popoverKind(el) !== null) { hidePopoverAlg(el, !!focusPrev, events, false); if (!showing(el)) return; }
            /* (one that stopped being a popover: beforetoggle first all the same) */
            if (events && el.isConnected) { toggleEvent(el, 'beforetoggle', 'open', 'closed', false); if (!showing(el)) return; }
            /* (out of its document: the popovers shown above it in its list go first, no events) */
            var L = POPLISTS.get(el.ownerDocument);
            if (L && L.auto.indexOf(el) >= 0) for (var gh = 0; L.hint.length && gh < 1000; gh++) { var th = L.hint[L.hint.length - 1]; hide(th, false); if (L.hint[L.hint.length - 1] === th) L.hint.pop(); }
            if (L) [L.hint, L.auto].forEach(function (list) {
                var at = list.indexOf(el);
                for (var g = 0; at >= 0 && list.length > at + 1 && g < 1000; g++) {
                    var top = list[list.length - 1];
                    hide(top, false);
                    if (list[list.length - 1] === top) list.pop();
                    at = list.indexOf(el);
                }
            });
            unlist(el);
            elState(el, ST_POPOVER, false);
            noteShown(el, false);
            if (events) queueToggle(el, 'open', 'closed', true);
        }
        function show(el) { showPopoverAlg(el, true, null); }
        /* (light dismiss, HTML 6.12.x: a click outside the topmost popovers; Escape: the topmost one's close watcher) */
        var clickedPopover = function (n, l) {
            var a = openPopover(n), b = invokerTarget(n);
            var ia = a ? Math.max(l.auto.indexOf(a), l.hint.indexOf(a) + 10000) : -1, ib = b ? Math.max(l.auto.indexOf(b), l.hint.indexOf(b) + 10000) : -1;
            return ia >= ib ? a : b;
        };
        var watched = new WeakSet();
        function watchDoc(doc) {
            if (!doc || watched.has(doc) || typeof doc.addEventListener !== 'function') return;
            watched.add(doc);
            var lastDown = 0;
            var down = function (e) {
                var now = Date.now();
                if (e.type === 'mousedown' && now - lastDown < 50) return;
                lastDown = now;
                var l = lists(doc);
                if (!l.auto.length && !l.hint.length) return;
                l.down = clickedPopover(e.target, l);
                l.downSet = true;
                l.downNode = e.target;
            };
            var lastUp = 0;
            var up = function (e) {
                var now = Date.now();
                if (e.type === 'mouseup' && now - lastUp < 50) return;
                lastUp = now;
                var l = lists(doc);
                if (!l.downSet) return;
                var a = clickedPopover(e.target, l), same = a === l.down;
                /* (what was pressed on gone from the document since: not a click outside) */
                if (l.downNode && l.downNode.nodeType === 1 && !l.downNode.isConnected) same = false;
                l.down = null; l.downSet = false; l.downNode = null;
                if (!l.auto.length && !l.hint.length) return;
                if (same) hideAllUntil(a || doc, false, true);
            };
            doc.addEventListener('pointerdown', down, true);
            doc.addEventListener('mousedown', down, true);
            doc.addEventListener('pointerup', up, true);
            doc.addEventListener('mouseup', up, true);
            /* (Escape: through the close watchers, with the dialogs) */
            if (!global.__cyberPopoverEscape) Object.defineProperty(global, '__cyberPopoverEscape', { configurable: true, value: function (el) {
                if (!showing(el)) return false;
                hidePopoverAlg(el, true, true, false);
                return true;
            } });
        }
        /* (a popovertarget invoker's: with it as the invoker, dom_events.js) */
        Object.defineProperty(global, '__cyberPopoverInvoke', { configurable: true, value: function (pt, invoker, action) {
            if (action === 'show') { if (!showing(pt)) showPopoverAlg(pt, false, invoker); }
            else if (action === 'hide') { if (showing(pt)) hidePopoverAlg(pt, true, true, false); }
            else if (showing(pt)) hidePopoverAlg(pt, true, true, false);
            else showPopoverAlg(pt, false, invoker);
        } });
        /* (options.source: the popover's implicit anchor, as an invoker's) */
        function sourceAnchor(el, options) {
            var src = options && typeof options === 'object' ? options.source : undefined;
            if (src === undefined || src === null) return null;
            if (!(src instanceof global.HTMLElement)) throw new TypeError("Failed to read the 'source' property: not an HTMLElement.");
            var ik = src.getAttribute('data-cyber-iasrc') || String(++global.__cyberIaSeq || (global.__cyberIaSeq = 1));
            if (src.getAttribute('data-cyber-iasrc') !== ik) src.setAttribute('data-cyber-iasrc', ik);
            if (el.getAttribute('data-cyber-ia') !== ik) el.setAttribute('data-cyber-ia', ik);
            return src;
        }
        method(HP, 'showPopover', function (options) {
            if (!isHTML(this)) throw new TypeError('Illegal invocation');
            var src = sourceAnchor(this, options);
            showPopoverAlg(this, true, src);
        });
        method(HP, 'hidePopover', function () {
            if (!isHTML(this)) throw new TypeError('Illegal invocation');
            hidePopoverAlg(this, true, true, true);
        });
        method(HP, 'togglePopover', function (options) {
            if (!isHTML(this)) throw new TypeError('Illegal invocation');
            var force = null, src = null;
            if (typeof options === 'boolean') force = options;
            else if (options && typeof options === 'object') { if (typeof options.force === 'boolean') force = options.force; src = sourceAnchor(this, options); }
            if (showing(this) && force !== true) hidePopoverAlg(this, true, true, true);
            else if (force === null || force === true) showPopoverAlg(this, true, src);
            else checkValidity(this, showing(this), true, null);
            return showing(this);
        });
        Object.defineProperty(HP, 'popover', { configurable: true, enumerable: true,
            get: function () { if (!isHTML(this)) throw new TypeError('Illegal invocation'); return this.getAttribute('popover') === null ? null : popoverKind(this); },   /* (a menulist's own popover-ness: not the attribute's) */
            set: function (v) {
                if (!isHTML(this)) throw new TypeError('Illegal invocation');
                if (v === null || v === undefined) this.removeAttribute('popover'); else this.setAttribute('popover', String(v));
            } });
        /* (a popover that stops being one: hidden) */
        var setA = global.Element.prototype.removeAttribute;
        if (global.MutationObserver) {
            var mo = new global.MutationObserver(function (recs) {
                recs.forEach(function (r) {
                    var el = r.target;
                    /* (a popover attribute's change: __cyberPopoverAttr, at once) */
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
        /* (execCommand: selectAll only, the editing host focused or else the body) */
        var execCommand = function execCommand(cmd) {
            if (String(cmd).toLowerCase() !== 'selectall') return false;
            var doc = this && this.nodeType === 9 ? this : global.document;
            var host = doc.activeElement;
            while (host && host.nodeType === 1 && !host.isContentEditable) host = null;
            var root = host || doc.body || doc.documentElement;
            if (!root) return false;
            var sel = (doc.defaultView || global).getSelection();
            if (sel && sel.selectAllChildren) sel.selectAllChildren(root);
            return true;
        };
        if (DocP && typeof DocP.execCommand !== 'function') define(DocP, 'execCommand', execCommand);
        if (global.document && typeof global.document.execCommand !== 'function') define(global.document, 'execCommand', execCommand);
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
    /* (the table interfaces' operations: this one of theirs, their required arguments; caption, tHead, tFoot settable) */
    [[global.HTMLTableElement, { createCaption: 0, deleteCaption: 0, createTHead: 0, deleteTHead: 0, createTFoot: 0, deleteTFoot: 0, createTBody: 0, insertRow: 0, deleteRow: 1 }],
     [global.HTMLTableSectionElement, { insertRow: 0, deleteRow: 1 }],
     [global.HTMLTableRowElement, { insertCell: 0, deleteCell: 1 }]].forEach(function (e) {
        var C = e[0];
        if (typeof C !== 'function') return;
        Object.keys(e[1]).forEach(function (k) {
            var f = C.prototype[k], need = e[1][k];
            if (typeof f !== 'function') return;
            var w = { [k]: function () {
                if (!(this instanceof C)) throw new TypeError("Failed to execute '" + k + "' on '" + C.name + "': Illegal invocation");
                if (arguments.length < need) throw new TypeError("Failed to execute '" + k + "' on '" + C.name + "': " + need + ' argument required, but only ' + arguments.length + ' present.');
                return f.apply(this, arguments);
            } }[k];
            Object.defineProperty(w, 'length', { value: need });
            define(C.prototype, k, w);
        });
    });
    if (TP) [['caption', 'caption', 'deleteCaption', function (t) { return t.firstChild; }],
             ['tHead', 'thead', 'deleteTHead', function (t) {
                 for (var c = t.firstChild; c; c = c.nextSibling)
                     if (c.nodeType === 1 && c.localName !== 'caption' && c.localName !== 'colgroup') return c;
                 return null; }],
             ['tFoot', 'tfoot', 'deleteTFoot', function () { return null; }]].forEach(function (a) {
        var d = Object.getOwnPropertyDescriptor(TP, a[0]);
        if (!d || !d.get || d.set) return;
        var g = d.get;
        var gg = { [a[0]]: function () { if (!(this instanceof global.HTMLTableElement)) throw new TypeError('Illegal invocation'); return g.call(this); } }[a[0]];
        Object.defineProperty(gg, 'name', { value: 'get ' + a[0] });
        var ss = { [a[0]]: function (v) {
            if (!(this instanceof global.HTMLTableElement)) throw new TypeError('Illegal invocation');
            if (v !== null && !(v instanceof global.HTMLTableCaptionElement || v instanceof global.HTMLTableSectionElement || (v && v.nodeType === 1)))
                throw new TypeError("Failed to set the '" + a[0] + "' property on 'HTMLTableElement': The provided value is not of the right type.");
            if (v !== null && v.localName !== a[1]) throw new global.DOMException('Not a ' + a[1] + ' element.', 'HierarchyRequestError');
            this[a[2]]();
            if (v !== null) this.insertBefore(v, a[3](this));
        } }[a[0]];
        Object.defineProperty(ss, 'name', { value: 'set ' + a[0] });
        Object.defineProperty(TP, a[0], { get: gg, set: ss, enumerable: true, configurable: true });
    });
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
    /* (a menu tree, its menulists and menubars nested through submenus: one with anything its content model has not
     * (text outside an item or legend, an element other than its own kinds, a focusable container) is lists, all of it) */
    var MENU_ITEMS = { menuitem: 1, menuitemradio: 1, menuitemcheckbox: 1, legend: 1 };
    /* (a submenu's trigger, its first menuitem, activated: its menulist shown, anchored to it, or hidden) */
    onWin('click', function (e) {
        var mi = e.target;
        while (mi && mi.nodeType === 1 && mi.localName !== 'menuitem') mi = mi.parentElement;
        if (!mi || mi.nodeType !== 1 || mi.namespaceURI !== 'http://www.w3.org/1999/xhtml' || e.defaultPrevented) return;
        var sm = mi.parentElement;
        if (!sm || sm.localName !== 'submenu' || sm.firstElementChild !== mi) return;
        var ml = null;
        for (var c = mi.nextElementSibling; c && !ml; c = c.nextElementSibling) if (c.localName === 'menulist') ml = c;
        if (!ml || mi.hasAttribute('disabled')) return;
        try {
            if (ml.matches(':popover-open')) ml.hidePopover();
            else ml.showPopover({ source: mi });
        } catch (x) { }
    });
    var MENU_PARTS = { fieldset: 1, div: 1, submenu: 1, menulist: 1, menubar: 1, hr: 1, template: 1, script: 1, style: 1 };
    function menuTreeBad(el) {
        var root = el;
        for (var p = el.parentElement; p; p = p.parentElement)
            if ((p.localName === 'menulist' || p.localName === 'menubar') && p.namespaceURI === el.namespaceURI) root = p;
        var bad = function (n) {
            for (var c = n.firstChild; c; c = c.nextSibling) {
                if (c.nodeType === 3) { if (/[^\t\n\f\r ]/.test(c.data)) return true; continue; }
                if (c.nodeType !== 1) continue;
                var ln = c.localName;
                if (c.namespaceURI !== 'http://www.w3.org/1999/xhtml') return true;
                if (MENU_ITEMS[ln]) continue;
                if (!MENU_PARTS[ln] || (ln !== 'template' && c.hasAttribute('tabindex'))) return true;
                if (ln !== 'template' && ln !== 'script' && ln !== 'style' && bad(c)) return true;
            }
            return false;
        };
        return bad(root);
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
        case 'menulist': return menuTreeBad(el) ? 'list' : 'menu';
        case 'menubar': return menuTreeBad(el) ? 'list' : 'menubar';
        case 'menuitem': case 'menuitemradio': case 'menuitemcheckbox': {
            /* (in a menu tree that is lists: a list item, a radio, a checkbox) */
            var mo = el.parentElement;
            while (mo && mo.localName !== 'menulist' && mo.localName !== 'menubar') mo = mo.parentElement;
            var lst = mo && menuTreeBad(mo);
            return tag === 'menuitem' ? (lst ? 'listitem' : 'menuitem') : tag === 'menuitemradio' ? (lst ? 'radio' : 'menuitemradio') : (lst ? 'checkbox' : 'menuitemcheckbox');
        }
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
    /* (scoped focusgroup's implied roles, Open UI: a generic owner its behavior's role; its generic or button items the
     * behavior's child role; explicit roles and other native ones kept) */
    var FG_OWNER = { tablist: 'tablist', radiogroup: 'radiogroup', listbox: 'listbox', menu: 'menu', menubar: 'menubar', toolbar: 'toolbar', feed: 'feed' };
    var FG_CHILD = { tablist: 'tab', radiogroup: 'radio', listbox: 'option', menu: 'menuitem', menubar: 'menuitem', feed: 'article' };
    var fgBehavior = function (n) {
        var v = n.getAttribute && n.getAttribute('focusgroup');
        if (v === null || v === undefined) return null;
        var t = String(v).trim().toLowerCase().split(/\s+/);
        for (var i = 0; i < t.length; i++) if (FG_OWNER[t[i]]) return t[i];
        return '';
    };
    var fgImplied = function (el, r) {
        var explicit = (el.getAttribute('role') || '').trim();
        if (explicit) return r;
        var own = fgBehavior(el);
        if (own && r === 'generic') return FG_OWNER[own];
        if ((r !== 'generic' && r !== 'button') || !(el.tabIndex >= 0)) return r;
        for (var n = el.assignedSlot || el.parentNode || el.host, g = 0; n && g < 256; n = n.assignedSlot || n.parentNode || n.host, g++) {
            if (n.nodeType !== 1) continue;
            var b = fgBehavior(n);
            if (b === null) continue;
            return b && FG_CHILD[b] ? FG_CHILD[b] : r;
        }
        return r;
    };
    function axRoleOnly(el) {
        /* (a role that asks for the name asks again: once is enough) */
        if (axDepth > 2) return '';
        axDepth++;
        try { return fgImplied(el, axRole(el)); } finally { axDepth--; }
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
                else if (c === ':left') { if (k % 2 === printFirstSide()) ok = false; sp += 1; }
                else if (c === ':right') { if (k % 2 === 1 - printFirstSide()) ok = false; sp += 1; }
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
        if (!(w > 0) || !(h > 0)) return [480, 288];   /* (a page of no size: invalid, the default one) */
        return [w, h];
    }
    /* (a width or height with auto margins: they take the rest, half each when both; m.fw / m.fh: so resolved) */
    function printAutoMargins(rs, m, pw, ph, bl, br, bt, bb) {
        var wd = printDecl(rs, 'width'), hd = printDecl(rs, 'height');
        var la = printDecl(rs, 'margin-left') === 'auto', ra = printDecl(rs, 'margin-right') === 'auto';
        var ta = printDecl(rs, 'margin-top') === 'auto', ba = printDecl(rs, 'margin-bottom') === 'auto';
        if (wd && wd !== 'auto' && (la || ra)) {
            var rw = pw - printLen(wd, pw) - bl - br - (la ? 0 : m.left) - (ra ? 0 : m.right);
            if (la && ra) m.left = m.right = rw / 2; else if (la) m.left = rw; else m.right = rw;
            m.fw = true;
        }
        if (hd && hd !== 'auto' && (ta || ba)) {
            var rh = ph - printLen(hd, ph) - bt - bb - (ta ? 0 : m.top) - (ba ? 0 : m.bottom);
            if (ta && ba) m.top = m.bottom = rh / 2; else if (ta) m.top = rh; else m.bottom = rh;
            m.fh = true;
        }
        return m;
    }
    /* (page progression right to left (an rtl root across, or vertical-rl): the first page a left one; 0: a right one) */
    function printFirstSide() {
        try {
            var rcs = getComputedStyle(document.documentElement), wm = rcs.writingMode;
            return (wm === 'vertical-rl' || wm === 'sideways-rl') || (/^horizontal/.test(wm || 'horizontal-tb') && rcs.direction === 'rtl') ? 1 : 0;
        } catch (e) { return 0; }
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
            if (name === 'pages') { if (i + 1 < t.length && /^-?\d+$/.test(t[i + 1])) i++; continue; }   /* (pages: not to be changed) */
            var nv = dflt;
            if (i + 1 < t.length && /^-?\d+$/.test(t[i + 1])) { nv = parseInt(t[i + 1], 10); i++; }
            o.push([name, nv]);
        }
        return o;
    }
    function printBoxEl(name, d, k, n) {
        var cell = document.createElement('cyber-pm'), def = MB_DEFAULT[name];
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
            if (nm === 'pages') return '"' + n + '"';
            if (nm === 'page') return '"' + (printPageNo >= 0 ? printPageNo : k + 1) + '"';
            return Object.prototype.hasOwnProperty.call(loc, nm) ? '"' + loc[nm] + '"' : m0;
        });
        printSheet.appendChild(document.createTextNode('.' + cn + '::before{content:' + content + '}\n'));
        return cell;
    }
    function printMeasure(host, name, d, k, n, vert) {
        /* (min and max content widths and heights of its border box; vert: its lines down, the heights by content) */
        var mk = function (v) {
            var c = printBoxEl(name, d, k, n);
            c.style.display = 'block';
            c.style.position = 'absolute';
            c.style.visibility = 'hidden';
            if (vert) c.style.height = v; else c.style.width = v;
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
    function printDistribute(av, aMin, aMax, bMin, bMax, t0) {
        var sumMax = aMax + bMax, sumMin = aMin + bMin;
        if (sumMax <= av) {
            var fl = av - sumMax, t = sumMax > 0 ? aMax / sumMax : (t0 === undefined ? 0.5 : t0);   /* (none wanting any: by count, t0) */
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
            var vt = !horizontal && /vertical|sideways/.test(d['writing-mode'] || '');
            var m = printMeasure(host, names[i], d, k, n, vt);
            ms[i] = horizontal ? { mn: m.minW + mar, mx: m.maxW + mar } : { mn: m.minH + mar, mx: m.maxH + mar };
            if (!horizontal && !vt) ms[i].mn = ms[i].mx;   /* (heights of lines across: the content's) */
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
                else {
                    /* (against each side doubled: the one that takes more room) */
                    var dl_ = A ? avail - printDistribute(avail, C.mn, C.mx, 2 * A.mn, 2 * A.mx, 1 / 3) : -1;
                    var dr_ = B ? avail - printDistribute(avail, C.mn, C.mx, 2 * B.mn, 2 * B.mx, 1 / 3) : -1;
                    var S_ = dl_ >= dr_ ? A : B, Sf_ = dl_ >= dr_ ? fix[0] : fix[2];
                    if (S_ && Sf_ !== null) sz[1] = Math.max(0, avail - 2 * Sf_);   /* (that side of a fixed size: the rest) */
                    else sz[1] = S_ ? printDistribute(avail, C.mn, C.mx, 2 * S_.mn, 2 * S_.mx, 1 / 3) : printDistribute(avail, C.mn, C.mx, sideMin, sideMax, 1 / 3);
                }
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
    var printClip = null, printOrg = [0, 0];   /* (the page's clipping box the margin boxes go in, and where it is) */
    function printPlace(host, name, d, x, y, w, h, k, n, base) {
        /* (along its row: of the row's length; across: of its slot) */
        var rowH = /^(top|bottom)-/.test(name) && !/corner/.test(name), rowV = /^(left|right)-/.test(name) && !/corner/.test(name);
        var hb = rowH ? base : w, vb = rowV ? base : h;
        var ml = printMarginSum(d, 'margin-left', 'margin-left', hb) / 2, mr = printMarginSum(d, 'margin-right', 'margin-right', hb) / 2;
        var mt = printMarginSum(d, 'margin-top', 'margin-top', vb) / 2, mb = printMarginSum(d, 'margin-bottom', 'margin-bottom', vb) / 2;
        var aw = printAutos(d, 'margin-left', 'margin-right'), ah = printAutos(d, 'margin-top', 'margin-bottom');
        var bw = w - ml - mr, bh = h - mt - mb;
        var wrap = document.createElement('cyber-pm'), cell = printBoxEl(name, d, k, n);
        var kw = function (v) { return /content/.test(v); };
        var cw = d.width !== undefined && d.width !== 'auto' && /corner|^left|^right/.test(name) ? (kw(d.width) ? printContentSize(host, name, d, k, n, true) : printLen(d.width, w)) : null;
        var ch = d.height !== undefined && d.height !== 'auto' && /corner|^top|^bottom/.test(name) ? (kw(d.height) ? printContentSize(host, name, d, k, n, false) : printLen(d.height, h)) : null;
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
        wrap.style.cssText = 'position:absolute;display:table;table-layout:fixed;left:' + (bx - printOrg[0]) + 'px;top:' + (by - printOrg[1]) + 'px;width:' + bw + 'px;height:' + bh + 'px;';
        cell.style.width = bw + 'px';
        cell.style.height = bh + 'px';
        wrap.appendChild(cell);
        (printClip || host).appendChild(wrap);
        /* (content taller than the slot: the box the slot's height, the content past it) */
        if (cell.getBoundingClientRect().height > bh + 0.5) {
            wrap.style.display = 'block';
            cell.style.display = 'block';
        }
    }
    function printBuild() {
        var rules = [];
        for (var i = 0; i < document.styleSheets.length; i++) { try { printRules(document.styleSheets[i].cssRules, rules); } catch (e) { /* (cross origin) */ } }
        var firstEl = document.body && document.body.firstElementChild, firstName = '';
        for (var fe = firstEl; fe; fe = fe.firstElementChild) { var pn = getComputedStyle(fe).page; if (pn && pn !== 'auto') { firstName = pn; break; } }
        var first = printPageRules(rules, firstName, 0), size = printSize(first), m0 = printMetrics(first, size[0], size[1]);
        var pw = size[0], ph = size[1], opw = pw, oph = ph, de = document.documentElement;   /* (opw, oph: what percentages are of) */
        /* (the page box's own border and padding take room from the content) */
        var extra = function (p) { var o = 0; ['border-' + p + '-width', 'padding-' + p].forEach(function (q) { var v = printDecl(first, q); if (v) o += printLen(v, pw); }); return o; };
        var probe = document.createElement('cyber-pm');
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
        /* (percentage padding: of the page box's height for top and bottom, its width across) */
        var ppct = function (p, base, got) { var v = printDecl(first, 'padding-' + p); return v && /%/.test(v) ? printLen(v, base) - (parseFloat(got) || 0) : 0; };
        bt += ppct('top', ph, pcs.paddingTop); bb += ppct('bottom', ph, pcs.paddingBottom);
        bl += ppct('left', pw, pcs.paddingLeft); br += ppct('right', pw, pcs.paddingRight);
        /* (the padding alone: the canvas's background over it too) */
        var padT = parseFloat(pcs.paddingTop) + ppct('top', ph, pcs.paddingTop), padR = parseFloat(pcs.paddingRight) + ppct('right', pw, pcs.paddingRight);
        var padB = parseFloat(pcs.paddingBottom) + ppct('bottom', ph, pcs.paddingBottom), padL = parseFloat(pcs.paddingLeft) + ppct('left', pw, pcs.paddingLeft);
        probe.remove();
        printAutoMargins(first, m0, pw, ph, bl, br, bt, bb);
        var cw = Math.max(1, pw - m0.left - m0.right - bl - br), chh = Math.max(1, ph - m0.top - m0.bottom - bt - bb);
        var wd = printDecl(first, 'width'), hd = printDecl(first, 'height');
        /* (overconstrained: the page box resized to fit it, its margins kept) */
        if (wd && wd !== 'auto' && !m0.fw) { var wv = printLen(wd, pw); cw = wv; pw = wv + m0.left + m0.right + bl + br; }
        if (hd && hd !== 'auto' && !m0.fh) { var hv = printLen(hd, ph); chh = hv; ph = hv + m0.top + m0.bottom + bt + bb; }
        /* (reftest-pages: the first page named shown first) */
        var rpm = document.querySelector('meta[name="reftest-pages"]'), skip = 0;
        if (rpm) { var rpv = /^\s*(\d+)/.exec(rpm.getAttribute('content') || ''); if (rpv && +rpv[1] > 1) skip = +rpv[1] - 1; }
        __cyberPrintSet(cw, chh, m0.top + m0.bottom + bt + bb, m0.left + bl, m0.top + bt, pw, skip, padT || 0, padR || 0, padB || 0, padL || 0);
        var tries = 0;
        function pages() {
            /* (from the root's height, once laid out as pages) */
            var n = Math.max(1, __cyberPrintPages());
            if (n > 40) n = 40;
            var hostOf = function (z) {
                var hh = document.createElement('cyber-pm');
                hh.setAttribute('data-cyber-print', '');
                hh.style.cssText = 'position:fixed;left:0;top:0;width:0;height:0;z-index:' + z + ';';
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
                var pnm = k > 0 && typeof __cyberPrintPageName === 'function' ? __cyberPrintPageName(k) : firstName;
                var rs = printPageRules(rules, pnm, k), m = printMetrics(rs, opw, oph);
                /* (a width or height of the page box: the area that big, the end margin the rest) */
                var wdk = printDecl(rs, 'width'), hdk = printDecl(rs, 'height');
                printAutoMargins(rs, m, pw, ph, bl, br, bt, bb);
                if (wdk && wdk !== 'auto' && !m.fw) m.right = pw - m.left - printLen(wdk, opw) - bl - br;
                if (hdk && hdk !== 'auto' && !m.fh) m.bottom = ph - m.top - printLen(hdk, oph) - bt - bb;
                var pinc = 1;
                /* (a page's counter-reset: an instance of its own, the next page the document's again) */
                var lc = {};
                printCtrList(printDecl(rs, 'counter-reset'), 0).forEach(function (p) { if (p[0] === 'page') pno = p[1]; else lc[p[0]] = p[1]; });
                printCtrList(printDecl(rs, 'counter-increment'), 1).forEach(function (p) { if (p[0] === 'page') pinc = p[1]; else if (Object.prototype.hasOwnProperty.call(lc, p[0])) lc[p[0]] += p[1]; else ctr[p[0]] = (ctr[p[0]] || 0) + p[1]; });
                pno += pinc;
                printCtrList(printDecl(rs, 'counter-set'), 0).forEach(function (p) { if (p[0] === 'page') pno = p[1]; else if (Object.prototype.hasOwnProperty.call(lc, p[0])) lc[p[0]] = p[1]; else ctr[p[0]] = p[1]; });
                printPageNo = pno;
                printCtrMap = {};
                for (var cq in ctr) printCtrMap[cq] = ctr[cq];
                for (var cq2 in lc) printCtrMap[cq2] = lc[cq2];
                printPageCtr = '';
                for (var cn0 in printCtrMap) if (cn0 !== 'page' && cn0 !== 'pages') printPageCtr += ' ' + cn0 + ' ' + printCtrMap[cn0];
                var px = -(m0.left + bl), py = k * ph - (m0.top + bt);
                if (typeof __cyberPrintPageOff === 'function') __cyberPrintPageOff(k, m.left - m0.left, m.top - m0.top);
                /* (the page shown: page-orientation turns it) */
                if (k === skip && typeof __cyberPrintRotate === 'function') {
                    var po = (printDecl(rs, 'page-orientation') || '').trim();
                    __cyberPrintRotate(po === 'rotate-right' ? 1 : po === 'rotate-left' ? -1 : 0);
                }
                var box = document.createElement('cyber-pm');
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
                        var sh = document.createElement('cyber-pm');
                        sh.style.cssText = 'position:absolute;visibility:hidden;left:' + px + 'px;top:' + py + 'px;width:' + pw + 'px;height:' + ph + 'px;';
                        sh.style.backgroundColor = bgc;
                        sheet.appendChild(sh);
                    }
                }
                var d = printMarginDecls(rs);
                var cwm = pw - m.left - m.right, chm = ph - m.top - m.bottom;
                printClip = document.createElement('cyber-pm');
                printClip.style.cssText = 'position:absolute;overflow:hidden;left:' + px + 'px;top:' + py + 'px;width:' + pw + 'px;height:' + ph + 'px;';
                host.appendChild(printClip);
                printOrg = [px, py];
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
                printClip = null;
                printOrg = [0, 0];
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

    /* ---- interfaces for objects made plain elsewhere (HTML 4.8.11 media, 7.2 window parts) ---- */
    (function () {
        if (!global.document || global.document.nodeType !== 9) return;   /* (a window's realm) */
        var ET = global.EventTarget;
        function iface(name, Parent) {
            var C = function () { illegal(); };
            Object.defineProperty(C, 'name', { value: name });
            C.prototype = Object.create(Parent ? Parent.prototype : Object.prototype);
            define(C.prototype, 'constructor', C);
            Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true });
            if (Parent) Object.setPrototypeOf(C, Parent);
            expose(name, C);
            return C;
        }
        function getter(P, name, brand, get, set) {
            var g = { get: function () { if (!this || !this[brand]) throw new TypeError('Illegal invocation'); return get.call(this, this[brand]); } };
            var d = { get: Object.getOwnPropertyDescriptor(g, 'get').value, enumerable: true, configurable: true };
            if (set) {
                var s = { set: function (v) { if (!this || !this[brand]) throw new TypeError('Illegal invocation'); set.call(this, this[brand], v); } };
                d.set = Object.getOwnPropertyDescriptor(s, 'set').value;
            }
            Object.defineProperty(P, name, d);
        }
        function op(P, name, brand, len, fn) {
            var o = { f: function () {
                if (!this || !this[brand]) throw new TypeError('Illegal invocation');
                if (arguments.length < len) throw new TypeError("Failed to execute '" + name + "': " + len + ' argument' + (len > 1 ? 's' : '') + ' required, but only ' + arguments.length + ' present.');
                return fn.apply(this, [this[brand]].concat([].slice.call(arguments)));
            } };
            var f = o.f;
            Object.defineProperty(f, 'name', { value: name, configurable: true });
            Object.defineProperty(f, 'length', { value: len, configurable: true });
            Object.defineProperty(P, name, { value: f, writable: true, enumerable: true, configurable: true });
        }
        /* (an event handler IDL attribute: the listener it adds) */
        function handler(P, name) {
            var K = Symbol(name), L = Symbol(name + 'L'), type = name.slice(2);
            Object.defineProperty(P, name, { enumerable: true, configurable: true,
                get: function () { return this[K] || null; },
                set: function (v) {
                    if (this[L]) { this.removeEventListener(type, this[L]); this[L] = null; }
                    var f = typeof v === 'function' || (v && typeof v === 'object') ? v : null;
                    Object.defineProperty(this, K, { value: f, writable: true, configurable: true });
                    if (f) {
                        var self = this, w = function (e) { var r = typeof f === 'function' ? f.call(self, e) : null; if (r === false) e.preventDefault(); return r; };
                        Object.defineProperty(this, L, { value: w, writable: true, configurable: true });
                        this.addEventListener(type, w);
                    }
                } });
        }
        function brandNew(C, brand, st) { var o = Object.create(C.prototype); Object.defineProperty(o, brand, { value: st }); return o; }
        function fire(t, type, init) { var E = global.Event; try { t.dispatchEvent(init && global.TrackEvent ? new global.TrackEvent(type, init) : new E(type)); } catch (e) { } }

        /* TimeRanges */
        var TRB = Symbol('timeRanges');
        var TimeRanges = iface('TimeRanges');
        getter(TimeRanges.prototype, 'length', TRB, function (st) { return st.s.length; });
        ['start', 'end'].forEach(function (k) {
            op(TimeRanges.prototype, k, TRB, 1, function (st, i) {
                i = Number(i); i = isFinite(i) ? Math.trunc(i) >>> 0 : 0;
                if (i >= st.s.length) throw new global.DOMException("Failed to execute '" + k + "' on 'TimeRanges': The index provided (" + i + ") is greater than or equal to the maximum bound (" + st.s.length + ').', 'IndexSizeError');
                return (k === 'start' ? st.s : st.e)[i];
            });
        });
        function ranges(nat) {
            var s = [], e = [];
            if (nat && typeof nat === 'object') {
                var n = nat.length >>> 0;
                for (var i = 0; i < n; i++) { try { s.push(+nat.start(i)); e.push(+nat.end(i)); } catch (x) { } }
            }
            return brandNew(TimeRanges, TRB, { s: s, e: e });
        }
        define(global, '__cyberTimeRanges', function (s, e) { return brandNew(TimeRanges, TRB, { s: s || [], e: e || [] }); });

        /* MediaError */
        var MEB = Symbol('mediaError');
        var MediaError = iface('MediaError');
        getter(MediaError.prototype, 'code', MEB, function (st) { return +st.code || 0; });
        getter(MediaError.prototype, 'message', MEB, function (st) { return st.message == null ? '' : String(st.message); });
        var errs = new WeakMap();
        function mediaError(nat) {
            if (!nat || typeof nat !== 'object') return nat === undefined ? null : nat;
            if (nat instanceof MediaError) return nat;
            var w = errs.get(nat);
            if (!w) { w = brandNew(MediaError, MEB, nat); errs.set(nat, w); }
            return w;
        }

        /* the track lists, the tracks, the cues */
        var LB = Symbol('trackList'), TB = Symbol('track'), CB = Symbol('cueList'), QB = Symbol('cue');
        var TextTrackList = iface('TextTrackList', ET), AudioTrackList = iface('AudioTrackList', ET), VideoTrackList = iface('VideoTrackList', ET);
        var AudioTrack = iface('AudioTrack'), VideoTrack = iface('VideoTrack');
        var TextTrack = iface('TextTrack', ET), TextTrackCueList = iface('TextTrackCueList'), TextTrackCue = iface('TextTrackCue', ET);
        [TextTrackList, AudioTrackList, VideoTrackList].forEach(function (L) {
            var P = L.prototype;
            getter(P, 'length', LB, function (st) { return st.items.length; });
            op(P, 'getTrackById', LB, 1, function (st, id) {
                id = String(id);
                for (var i = 0; i < st.items.length; i++) if (st.items[i].id === id) return st.items[i];
                return null;
            });
            if (L === VideoTrackList) getter(P, 'selectedIndex', LB, function (st) {
                for (var i = 0; i < st.items.length; i++) if (st.items[i].selected) return i;
                return -1;
            });
            ['onchange', 'onaddtrack', 'onremovetrack'].forEach(function (h) { handler(P, h); });
        });
        function listSync(list, items) {
            var st = list[LB], old = st.items.length;
            st.items = items;
            for (var i = 0; i < Math.max(old, items.length); i++) {
                if (i < items.length) Object.defineProperty(list, i, { value: items[i], enumerable: true, configurable: true });
                else delete list[i];
            }
        }
        function newList(L) { return brandNew(L, LB, { items: [] }); }
        [AudioTrack, VideoTrack].forEach(function (T) {
            var P = T.prototype;
            ['id', 'kind', 'label', 'language'].forEach(function (k) { getter(P, k, TB, function (st) { return st[k] || ''; }); });
            if (T === AudioTrack) getter(P, 'enabled', TB, function (st) { return !!st.enabled; }, function (st, v) { st.enabled = !!v; });
            else getter(P, 'selected', TB, function (st) { return !!st.selected; }, function (st, v) { st.selected = !!v; });
        });
        /* TextTrack: what addTextTrack or a <track> makes */
        var KINDS = ['subtitles', 'captions', 'descriptions', 'chapters', 'metadata'], MODES = ['disabled', 'hidden', 'showing'];
        var TTP = TextTrack.prototype;
        /* (a <track>'s: its attributes as they are now) */
        var TRACK_ATTR = { label: 'label', language: 'srclang', id: 'id' };
        ['kind', 'label', 'language', 'id', 'inBandMetadataTrackDispatchType'].forEach(function (k) {
            getter(TTP, k, TB, function (st) {
                if (st.el && TRACK_ATTR[k]) { var av = st.el.getAttribute(TRACK_ATTR[k]); return av === null ? '' : av; }
                return st[k] || '';
            });
        });
        getter(TTP, 'mode', TB, function (st) { return st.mode; }, function (st, v) {
            v = String(v);
            if (MODES.indexOf(v) < 0 || v === st.mode) return;
            st.mode = v;
            if (st.el && v !== 'disabled' && typeof global.__cyberTrackLoad === 'function') global.__cyberTrackLoad(st.el);
            if (st.media) { var self = this; setTimeout(function () { var l = st.media.textTracks; if (l) fire(l, 'change'); }, 0); }
        });
        function cueList(st) {
            if (!st.cueList) st.cueList = brandNew(TextTrackCueList, CB, { items: st.cues });
            var cl = st.cueList, items = st.cues;
            for (var i = 0; i < Math.max(items.length, cl[CB].n || 0); i++) {
                if (i < items.length) Object.defineProperty(cl, i, { value: items[i], enumerable: true, configurable: true });
                else delete cl[i];
            }
            cl[CB].n = items.length; cl[CB].items = items;
            return cl;
        }
        getter(TTP, 'cues', TB, function (st) { return st.mode === 'disabled' ? null : cueList(st); });
        getter(TTP, 'activeCues', TB, function (st) {
            if (st.mode === 'disabled') return null;
            if (!st.active) st.active = brandNew(TextTrackCueList, CB, { items: [], n: 0 });
            return st.active;
        });
        function cueSort(a, b) { var x = a[QB], y = b[QB]; return x.startTime - y.startTime || y.endTime - x.endTime; }
        op(TTP, 'addCue', TB, 1, function (st, cue) {
            if (!(cue instanceof TextTrackCue)) throw new TypeError("Failed to execute 'addCue' on 'TextTrack': parameter 1 is not of type 'TextTrackCue'.");
            var q = cue[QB];
            if (q.track && q.track !== this) q.track.removeCue(cue);
            var i = st.cues.indexOf(cue);
            if (i >= 0) st.cues.splice(i, 1);
            q.track = this;
            st.cues.push(cue);
            st.cues.sort(cueSort);
            if (st.cueList) cueList(st);   /* (the list: live) */
        });
        op(TTP, 'removeCue', TB, 1, function (st, cue) {
            if (!(cue instanceof TextTrackCue)) throw new TypeError("Failed to execute 'removeCue' on 'TextTrack': parameter 1 is not of type 'TextTrackCue'.");
            var i = st.cues.indexOf(cue);
            if (i < 0) throw new global.DOMException("Failed to execute 'removeCue' on 'TextTrack': The specified cue is not listed in the TextTrack's list of cues.", 'NotFoundError');
            st.cues.splice(i, 1);
            cue[QB].track = null;
            if (st.cueList) cueList(st);
        });
        handler(TTP, 'oncuechange');
        var TCLP = TextTrackCueList.prototype;
        getter(TCLP, 'length', CB, function (st) { return st.items.length; });
        op(TCLP, 'getCueById', CB, 1, function (st, id) {
            id = String(id);
            if (!id) return null;
            for (var i = 0; i < st.items.length; i++) if (st.items[i].id === id) return st.items[i];
            return null;
        });
        /* TextTrackCue: abstract; VTTCue makes them */
        var TCP = TextTrackCue.prototype;
        getter(TCP, 'track', QB, function (st) { return st.track || null; });
        getter(TCP, 'id', QB, function (st) { return st.id; }, function (st, v) { st.id = String(v); });
        ['startTime', 'endTime'].forEach(function (k) {
            getter(TCP, k, QB, function (st) { return st[k]; }, function (st, v) {
                v = Number(v);
                if (!isFinite(v)) throw new TypeError("Failed to set the '" + k + "' property on 'TextTrackCue': The provided double value is non-finite.");
                st[k] = v;
                if (st.track) st.track[TB].cues.sort(cueSort);
            });
        });
        getter(TCP, 'pauseOnExit', QB, function (st) { return !!st.pauseOnExit; }, function (st, v) { st.pauseOnExit = !!v; });
        ['onenter', 'onexit'].forEach(function (h) { handler(TCP, h); });
        define(global, '__cyberNewCue', function (cue, start, end) {
            Object.defineProperty(cue, QB, { value: { id: '', startTime: +start, endTime: +end, pauseOnExit: false, track: null } });
            return cue;
        });
        function newTextTrack(kind, label, lang, media) {
            return brandNew(TextTrack, TB, { kind: kind, label: label, language: lang, id: '', inBandMetadataTrackDispatchType: '',
                                            mode: 'hidden', cues: [], media: media || null });
        }

        /* the media element's: lists kept per element, a <track> child's track among them */
        var MP = null;
        for (var p = global.HTMLVideoElement && global.HTMLVideoElement.prototype; p && !MP; p = Object.getPrototypeOf(p))
            if (Object.getOwnPropertyDescriptor(p, 'buffered')) MP = p;
        var lists = new WeakMap();
        function listsOf(el) {
            var l = lists.get(el);
            if (!l) { l = { text: newList(TextTrackList), audio: newList(AudioTrackList), video: newList(VideoTrackList), added: [] }; lists.set(el, l); }
            return l;
        }
        var TRP = global.HTMLTrackElement && global.HTMLTrackElement.prototype;
        var trackOf = new WeakMap();
        function elementTrack(t) {
            var tt = trackOf.get(t);
            if (!tt) {
                tt = newTextTrack('', '', '', null);
                tt[TB].mode = 'disabled';
                tt[TB].el = t;
                trackOf.set(t, tt);
            }
            var st = tt[TB], k = (t.getAttribute('kind') || '').toLowerCase();
            st.kind = !t.hasAttribute('kind') || k === '' ? 'subtitles' : KINDS.indexOf(k) >= 0 ? k : 'metadata';
            st.label = t.getAttribute('label') || '';
            st.language = t.getAttribute('srclang') || '';
            st.id = t.id || '';
            return tt;
        }
        function textTracksOf(el) {
            var l = listsOf(el), items = [];
            for (var c = el.firstElementChild; c; c = c.nextElementSibling)
                if (c.localName === 'track' && c.namespaceURI === 'http://www.w3.org/1999/xhtml') { var tt = elementTrack(c); tt[TB].media = el; items.push(tt); }
            listSync(l.text, items.concat(l.added));
            return l.text;
        }
        function wrapGet(name, conv) {
            var d = MP && Object.getOwnPropertyDescriptor(MP, name);
            var og = d && d.get;
            var g = { get: function () { return conv.call(this, og ? og.call(this) : undefined); } };
            Object.defineProperty(MP, name, { get: Object.getOwnPropertyDescriptor(g, 'get').value, set: d ? d.set : undefined, enumerable: true, configurable: true });
        }
        if (MP) {
            ['buffered', 'played', 'seekable'].forEach(function (k) { wrapGet(k, function (nat) { return nat instanceof TimeRanges ? nat : ranges(nat); }); });
            wrapGet('error', function (nat) { return mediaError(nat); });
            wrapGet('textTracks', function () { return textTracksOf(this); });
            wrapGet('audioTracks', function () { return listsOf(this).audio; });
            wrapGet('videoTracks', function () { return listsOf(this).video; });
            Object.defineProperty(MP, 'addTextTrack', { writable: true, enumerable: true, configurable: true, value: function addTextTrack(kind) {
                if (arguments.length < 1) throw new TypeError("Failed to execute 'addTextTrack' on 'HTMLMediaElement': 1 argument required, but only 0 present.");
                kind = String(kind);
                if (KINDS.indexOf(kind) < 0) throw new TypeError("Failed to execute 'addTextTrack' on 'HTMLMediaElement': The provided value '" + kind + "' is not a valid enum value of type TextTrackKind.");
                var label = arguments.length > 1 && arguments[1] !== undefined ? String(arguments[1]) : '';
                var lang = arguments.length > 2 && arguments[2] !== undefined ? String(arguments[2]) : '';
                var tt = newTextTrack(kind, label, lang, this), l = listsOf(this);
                l.added.push(tt);
                var list = textTracksOf(this);
                setTimeout(function () { fire(list, 'addtrack', { track: tt }); }, 0);
                return tt;
            } });
        }
        if (TRP) {
            Object.defineProperty(TRP, 'track', { enumerable: true, configurable: true, get: function track() { return elementTrack(this); } });
            Object.defineProperty(TRP, 'readyState', { enumerable: true, configurable: true, get: function readyState() { var tt = trackOf.get(this); return tt && tt[TB].ready || 0; } });
            /* ---- the text track processing model (HTML 4.8.11.11.x): a <track>'s cues fetched and parsed, WebVTT ---- */
            var vttTime = function (s) {
                var m = /^(?:(\d{2,}):)?([0-5]\d):([0-5]\d)\.(\d{3})$/.exec(s);
                return m ? (m[1] ? +m[1] * 3600 : 0) + +m[2] * 60 + +m[3] + +m[4] / 1000 : null;
            };
            var vttSettings = function (cue, s) {
                s.split(/[ \t]+/).forEach(function (tok) {
                    var i = tok.indexOf(':');
                    if (i <= 0 || i === tok.length - 1) return;
                    var k = tok.slice(0, i), v = tok.slice(i + 1), m;
                    try {
                        if (k === 'vertical') { if (v === 'rl' || v === 'lr') cue.vertical = v; }
                        else if (k === 'line') {
                            var parts = v.split(','), lv = parts[0], la = parts[1];
                            if (la !== undefined && !/^(start|center|end)$/.test(la)) return;
                            if ((m = /^(\d+(?:\.\d+)?)%$/.exec(lv)) && +m[1] <= 100) { cue.line = +m[1]; cue.snapToLines = false; }
                            else if (/^-?\d+$/.test(lv)) { cue.line = +lv; cue.snapToLines = true; }
                            else return;
                            if (la) cue.lineAlign = la;
                        } else if (k === 'position') {
                            var pp = v.split(','), pa = pp[1];
                            if (!(m = /^(\d+(?:\.\d+)?)%$/.exec(pp[0])) || +m[1] > 100) return;
                            if (pa !== undefined && !/^(line-left|center|line-right)$/.test(pa)) return;
                            cue.position = +m[1];
                            if (pa) cue.positionAlign = pa;
                        } else if (k === 'size') {
                            if ((m = /^(\d+(?:\.\d+)?)%$/.exec(v)) && +m[1] <= 100) cue.size = +m[1];
                        } else if (k === 'align') {
                            if (/^(start|center|end|left|right)$/.test(v)) cue.align = v;
                        }
                    } catch (e) { }
                });
            };
            var parseVTT = function (text) {
                if (text.charCodeAt(0) === 0xFEFF) text = text.slice(1);
                text = text.replace(/\0/g, '\uFFFD');
                var lines = text.split(/\r\n|\r|\n/), i = 0, cues = [];
                if (!/^WEBVTT(?:[ \t]|$)/.test(lines[0] || '')) return null;
                i = 1;
                /* (the header: to a blank line, or to a line with "-->") */
                while (i < lines.length && lines[i] !== '' && lines[i].indexOf('-->') < 0) i++;
                /* (to the end of a block: a blank line, or (not its first) a line with "-->", which starts the next) */
                var skipBlock = function () { while (i < lines.length && lines[i] !== '' && lines[i].indexOf('-->') < 0) i++; };
                /* (cue timings: a time, "-->" with white space around it or not, a time, then the settings) */
                var timings = function (l) {
                    var k = l.indexOf('-->');
                    if (k < 0) return null;
                    var left = l.slice(0, k).replace(/^[ \t\f]+|[ \t\f]+$/g, ''), rest = l.slice(k + 3).replace(/^[ \t\f]+/, '');
                    var m2 = /^([^ \t\f]+)(?:[ \t\f]+([\s\S]*))?$/.exec(rest);
                    if (!left || /[ \t\f]/.test(left) || !m2) return null;
                    return [l, left, m2[1], m2[2]];
                };
                while (i < lines.length) {
                    while (i < lines.length && lines[i] === '') i++;
                    if (i >= lines.length) break;
                    var id = '', line = lines[i];
                    if (line.indexOf('-->') < 0) {
                        /* (NOTE, STYLE, REGION blocks, or an id line) */
                        if (/^(NOTE|STYLE|REGION)(?:[ \t]|$)/.test(line)) { i++; skipBlock(); continue; }
                        id = line; i++;
                        if (i >= lines.length || lines[i] === '' || lines[i].indexOf('-->') < 0) { skipBlock(); continue; }
                        line = lines[i];
                    }
                    i++;
                    var tm = timings(line);
                    var st0 = tm && vttTime(tm[1]), en0 = tm && vttTime(tm[2]);
                    if (!tm || st0 === null || en0 === null) { skipBlock(); continue; }
                    var body = [];
                    while (i < lines.length && lines[i] !== '' && lines[i].indexOf('-->') < 0) body.push(lines[i++]);
                    var cue;
                    try { cue = new global.VTTCue(st0, en0, body.join('\n')); } catch (e) { continue; }
                    cue.id = id;
                    if (tm[3]) vttSettings(cue, tm[3]);
                    cues.push(cue);
                }
                return cues;
            };
            var fireSimple = function (t, type) { try { t.dispatchEvent(new global.Event(type)); } catch (e) { } };
            var mediaParent = function (t) { var p = t.parentNode; return p && p.nodeType === 1 && (p.localName === 'video' || p.localName === 'audio') ? p : null; };
            Object.defineProperty(global, '__cyberTrackLoad', { configurable: true, value: function (t) {
                var tt = elementTrack(t), st = tt[TB];
                if (st.mode === 'disabled' || !mediaParent(t)) return;
                var src = t.getAttribute('src');
                var key = src === null ? null : String(src);
                if (st.loadingFor === key && st.ready) return;
                st.loadingFor = key;
                st.cues.length = 0;
                st.ready = 1;
                var gen = st.gen = (st.gen || 0) + 1;
                var fail = function () { if (st.gen !== gen) return; st.ready = 3; fireSimple(t, 'error'); };
                if (!key) { global.setTimeout(fail, 0); return; }
                var url;
                /* (its query in the document's encoding) */
                var tdc = t.ownerDocument && t.ownerDocument.characterSet, tkey = key;
                if (tdc && !/^utf-8$/i.test(tdc) && /[^\x00-\x7f]/.test(key) && typeof global.__cyberLegacyQuery === 'function') { var tlq = global.__cyberLegacyQuery(key, tdc); if (tlq) tkey = tlq; }
                try { url = new global.URL(tkey, t.baseURI || global.location.href).href; } catch (e) { global.setTimeout(fail, 0); return; }
                /* (the media element's CORS setting: none a no-cors fetch, which another origin's response fails) */
                var ca = mediaParent(t).getAttribute('crossorigin'), self0 = global.location.origin;
                var opts = ca === null ? { mode: 'no-cors', credentials: 'include' } :
                           String(ca).toLowerCase() === 'use-credentials' ? { mode: 'cors', credentials: 'include' } : { mode: 'cors', credentials: 'same-origin' };
                var crossAt = function (u) { try { var o = new global.URL(u).origin; return o !== self0 && !/^(data|blob):/.test(u); } catch (e) { return false; } };
                global.fetch(url, opts).then(function (r) {
                    if (!r.ok && r.status !== 0) throw new Error('status');
                    if (ca === null && (crossAt(url) || (r.url && crossAt(r.url)) || r.type === 'opaque')) throw new Error('opaque');
                    return r.text();
                }).then(function (txt) {
                    if (st.gen !== gen) return;
                    var cues = parseVTT(txt);
                    if (!cues) { fail(); return; }
                    cues.forEach(function (c) { c[QB].track = tt; st.cues.push(c); });
                    st.cues.sort(cueSort);
                    st.ready = 2;
                    fireSimple(t, 'load');
                }).catch(fail);
            } });
            /* (a <track> in a media element: a default one showing (hidden for metadata), the first of its kind; then loading) */
            var autoSelect = function (t) {
                var m = mediaParent(t);
                if (!m) return;
                var tt = elementTrack(t), st = tt[TB];
                if (t.hasAttribute('default') && st.mode === 'disabled' && !st.autoDone) {
                    st.autoDone = true;
                    var others = m.querySelectorAll('track'), taken = false;
                    for (var i = 0; i < others.length; i++) {
                        if (others[i] === t) break;
                        var ot = trackOf.get(others[i]);
                        if (ot && ot[TB].kind === st.kind && ot[TB].mode !== 'disabled' && others[i].hasAttribute('default')) taken = true;
                    }
                    if (!taken) { st.mode = st.kind === 'metadata' ? 'hidden' : 'showing'; }
                }
                if (st.mode !== 'disabled') global.__cyberTrackLoad(t);
            };
            Object.defineProperty(global, '__cyberTrackInserted', { configurable: true, value: function (t) { global.setTimeout(function () { autoSelect(t); }, 0); } });
            Object.defineProperty(global, '__cyberTrackSrcChanged', { configurable: true, value: function (t) {
                var tt = trackOf.get(t);
                if (tt && tt[TB].mode !== 'disabled') { tt[TB].loadingFor = undefined; tt[TB].ready = 0; global.__cyberTrackLoad(t); }
            } });
            onWin('DOMContentLoaded', function () {
                var l = global.document.querySelectorAll('video > track, audio > track');
                for (var i = 0; i < l.length; i++) global.__cyberTrackInserted(l[i]);
            });
            if (!Object.getOwnPropertyDescriptor(TRP, 'kind')) Object.defineProperty(TRP, 'kind', { enumerable: true, configurable: true,
                get: function kind() { var k = (this.getAttribute('kind') || '').toLowerCase(); return !this.hasAttribute('kind') || !k ? 'subtitles' : KINDS.indexOf(k) >= 0 ? k : 'metadata'; },
                set: function kind(v) { this.setAttribute('kind', String(v)); } });
        }

        /* the window's one Navigator, History and bars: their members moved to an interface's prototype */
        function adopt(obj, name, keepOwn) {
            if (!obj || typeof obj !== 'object') return;
            var C = typeof global[name] === 'function' ? global[name] : iface(name);
            var P = C.prototype;
            if (Object.getPrototypeOf(obj) === P) return;
            if (!keepOwn) Object.getOwnPropertyNames(obj).forEach(function (k) {
                var d = Object.getOwnPropertyDescriptor(obj, k);
                if (!d.configurable || Object.getOwnPropertyDescriptor(P, k)) return;
                var nd;
                if (d.get || d.set) {
                    var og = d.get, os = d.set;
                    var g = { get: function () { if (this !== obj) throw new TypeError('Illegal invocation'); return og ? og.call(obj) : undefined; } };
                    var s = os && { set: function (v) { if (this !== obj) throw new TypeError('Illegal invocation'); os.call(obj, v); } };
                    nd = { get: Object.getOwnPropertyDescriptor(g, 'get').value, set: s ? Object.getOwnPropertyDescriptor(s, 'set').value : undefined, enumerable: true, configurable: true };
                } else if (typeof d.value === 'function') {
                    var of = d.value;
                    var o = { f: function () { if (this !== obj) throw new TypeError('Illegal invocation'); return of.apply(obj, arguments); } };
                    Object.defineProperty(o.f, 'name', { value: k, configurable: true });
                    Object.defineProperty(o.f, 'length', { value: of.length, configurable: true });
                    nd = { value: o.f, writable: true, enumerable: true, configurable: true };
                } else {
                    var held = d.value;
                    var g2 = { get: function () { if (this !== obj) throw new TypeError('Illegal invocation'); return held; } };
                    var s2 = { set: function (v) { if (this !== obj) throw new TypeError('Illegal invocation'); held = v; } };
                    nd = { get: Object.getOwnPropertyDescriptor(g2, 'get').value, set: Object.getOwnPropertyDescriptor(s2, 'set').value, enumerable: true, configurable: true };
                }
                try { Object.defineProperty(P, k, nd); delete obj[k]; } catch (e) { }
            });
            try { Object.setPrototypeOf(obj, P); } catch (e) { }
        }
        try { adopt(global.navigator, 'Navigator'); } catch (e) { }
        /* registerProtocolHandler / unregisterProtocolHandler (HTML 8.9.1.3): checked, nothing registered */
        (function () {
            var NP = global.Navigator && global.Navigator.prototype;
            if (!NP || NP.registerProtocolHandler) return;
            var SAFE = ['bitcoin', 'cabal', 'dat', 'did', 'dweb', 'ethereum', 'ftp', 'ftps', 'geo', 'im', 'ipfs', 'ipns', 'irc', 'ircs',
                        'matrix', 'magnet', 'mailto', 'mms', 'news', 'nntp', 'openpgp4fpr', 'sftp', 'sip', 'sms', 'smsto', 'ssb', 'ssh',
                        'tel', 'urn', 'webcal', 'wtai', 'xmpp'];
            function normalize(name, scheme, url) {
                scheme = String(scheme).replace(/[A-Z]/g, function (c) { return c.toLowerCase(); });
                if (SAFE.indexOf(scheme) < 0 && !/^web\+[a-z]+$/.test(scheme))
                    throw new global.DOMException("Failed to execute '" + name + "' on 'Navigator': The scheme '" + scheme + "' doesn't belong to the scheme allowlist.", 'SecurityError');
                url = String(url);
                if (url.indexOf('%s') < 0)
                    throw new global.DOMException("Failed to execute '" + name + "' on 'Navigator': The url provided ('" + url + "') does not contain '%s'.", 'SyntaxError');
                var u;
                try { u = new global.URL(url, global.document.baseURI); } catch (e) {
                    throw new global.DOMException("Failed to execute '" + name + "' on 'Navigator': The custom handler URL created by removing '%s' and prepending the base URL is not valid.", 'SyntaxError');
                }
                if ((u.protocol !== 'http:' && u.protocol !== 'https:') || u.origin !== global.location.origin)
                    throw new global.DOMException("Failed to execute '" + name + "' on 'Navigator': Can only register custom handler in the document's origin.", 'SecurityError');
            }
            Object.defineProperty(NP, 'registerProtocolHandler', { configurable: true, writable: true, enumerable: true, value: function registerProtocolHandler(scheme, url) {
                if (this !== global.navigator) throw new TypeError('Illegal invocation');
                if (arguments.length < 2) throw new TypeError("Failed to execute 'registerProtocolHandler' on 'Navigator': 2 arguments required, but only " + arguments.length + ' present.');
                normalize('registerProtocolHandler', scheme, url);
            } });
            Object.defineProperty(NP, 'unregisterProtocolHandler', { configurable: true, writable: true, enumerable: true, value: function unregisterProtocolHandler(scheme, url) {
                if (this !== global.navigator) throw new TypeError('Illegal invocation');
                if (arguments.length < 2) throw new TypeError("Failed to execute 'unregisterProtocolHandler' on 'Navigator': 2 arguments required, but only " + arguments.length + ' present.');
                normalize('unregisterProtocolHandler', scheme, url);
            } });
            /* (window.clientInformation: the navigator, replaceable) */
            if (!Object.getOwnPropertyDescriptor(global, 'clientInformation')) Object.defineProperty(global, 'clientInformation', { configurable: true, enumerable: true,
                get: function () { return global.navigator; },
                set: function (v) { Object.defineProperty(global, 'clientInformation', { value: v, writable: true, enumerable: true, configurable: true }); } });
        })();
        try { adopt(global.history, 'History'); } catch (e) { }
        try { adopt(global.location, 'Location', true); } catch (e) { }
        /* Storage: localStorage's and sessionStorage's methods (this-based natives) on its prototype */
        (function () {
            var ls = global.localStorage, ss = global.sessionStorage;
            if (!ls || typeof ls !== 'object' || typeof global.Storage === 'function') return;
            var St = iface('Storage'), SP_ = St.prototype, both = ss && typeof ss === 'object' ? [ls, ss] : [ls];
            ['key', 'getItem', 'setItem', 'removeItem', 'clear'].forEach(function (k) {
                var d = Object.getOwnPropertyDescriptor(ls, k);
                /* (this a Storage) */
                if (d && typeof d.value === 'function') {
                    var f0 = d.value;
                    var w = { [k]: function () {
                        if (!(this instanceof St)) throw new TypeError("Failed to execute '" + k + "' on 'Storage': Illegal invocation");
                        var need = { key: 1, getItem: 1, setItem: 2, removeItem: 1, clear: 0 }[k];
                        if (arguments.length < need) throw new TypeError("Failed to execute '" + k + "' on 'Storage': " + need + ' argument' + (need > 1 ? 's' : '') + ' required, but only ' + arguments.length + ' present.');
                        return f0.apply(this, arguments);
                    } }[k];
                    Object.defineProperty(w, 'length', { value: f0.length });
                    Object.defineProperty(SP_, k, { value: w, writable: true, enumerable: true, configurable: true });
                }
                both.forEach(function (o) { try { delete o[k]; } catch (e) { } });
            });
            var ld = Object.getOwnPropertyDescriptor(ls, 'length');
            if (ld && ld.get) {
                var lg0 = ld.get, lg = function () { if (!(this instanceof St)) throw new TypeError('Illegal invocation'); return lg0.call(this); };
                Object.defineProperty(lg, 'name', { value: 'get length' });
                Object.defineProperty(SP_, 'length', { get: lg, enumerable: true, configurable: true });
                both.forEach(function (o) { try { delete o.length; } catch (e) { } });
            }
            both.forEach(function (o) { try { Object.setPrototypeOf(o, SP_); } catch (e) { } });
        })();
        /* BarProp: locationbar, menubar, personalbar, scrollbars, statusbar, toolbar */
        var BB = Symbol('barProp'), BarProp = iface('BarProp');
        getter(BarProp.prototype, 'visible', BB, function () { return !global.__cyberIsPopup; });
        ['locationbar', 'menubar', 'personalbar', 'scrollbars', 'statusbar', 'toolbar'].forEach(function (k) {
            if (k in global) return;
            var b = brandNew(BarProp, BB, {});
            Object.defineProperty(global, k, { enumerable: true, configurable: true, get: function () { return b; }, set: function (v) { Object.defineProperty(global, k, { value: v, writable: true, enumerable: true, configurable: true }); } });
        });
    })();

    /* ---- BroadcastChannel (HTML 9.6): messages to the same-named channels of the same origin, in this page's realms ---- */
    (function () {
        var ETc = global.EventTarget;
        if (typeof global.BroadcastChannel === 'function' || !ETc || typeof global.__cyberWire !== 'function') return;
        var BC = Symbol.for('cyber.broadcastChannel');   /* (one key for every realm's channels) */
        var localHub = {};
        var hub = function () {
            try { if (typeof global.__cyberSharedHub === 'function') return global.__cyberSharedHub(); } catch (e) { }
            return localHub;
        };
        var originOf = function () { try { return global.origin || (global.location && global.location.origin) || ''; } catch (e) { return ''; } };
        var BroadcastChannel = function BroadcastChannel(name) {
            if (!new.target) throw new TypeError("Failed to construct 'BroadcastChannel': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
            if (arguments.length < 1) throw new TypeError("Failed to construct 'BroadcastChannel': 1 argument required, but only 0 present.");
            var self = this;
            var st = { name: String(name), closed: false, origin: originOf(), g: global };
            /* (a message for it: in a task of its own realm's) */
            st.deliver = function (wire, origin) {
                global.setTimeout(function () {
                    if (st.closed) return;
                    var data, ME = global.MessageEvent || global.Event;
                    try { data = global.__cyberUnwire(wire); }
                    catch (e) { var ee = new global.Event('messageerror'); if (global.__cyberDispatchTrusted) global.__cyberDispatchTrusted(self, ee); else self.dispatchEvent(ee); return; }
                    var ev = new ME('message', { data: data, origin: origin });
                    if (global.__cyberDispatchTrusted) global.__cyberDispatchTrusted(self, ev); else self.dispatchEvent(ev);
                }, 0);
            };
            Object.defineProperty(self, BC, { value: st });
            var h = hub(), key = 'bc:' + st.name;
            (h[key] || (h[key] = [])).push(self);
            return self;
        };
        BroadcastChannel.prototype = Object.create(ETc.prototype);
        Object.defineProperty(BroadcastChannel.prototype, 'constructor', { value: BroadcastChannel, writable: true, configurable: true });
        Object.setPrototypeOf(BroadcastChannel, ETc);
        var stOf = function (x) { var s = x && x[BC]; if (!s) throw new TypeError('Illegal invocation'); return s; };
        Object.defineProperty(BroadcastChannel.prototype, 'name', { configurable: true, enumerable: true, get: function name() { return stOf(this).name; } });
        Object.defineProperty(BroadcastChannel.prototype, 'postMessage', { configurable: true, enumerable: true, writable: true, value: function postMessage(message) {
            var st = stOf(this);
            if (arguments.length < 1) throw new TypeError("Failed to execute 'postMessage' on 'BroadcastChannel': 1 argument required, but only 0 present.");
            if (st.closed) throw new global.DOMException("Failed to execute 'postMessage' on 'BroadcastChannel': Channel is closed", 'InvalidStateError');
            var w = global.__cyberWire(message), self = this;
            var list = (hub()['bc:' + st.name] || []).slice();
            list.forEach(function (ch) {
                var cs = ch[BC];
                if (ch === self || !cs || cs.closed || cs.origin !== st.origin) return;
                try { cs.deliver(w, st.origin); } catch (e) { cs.closed = true; }
            });
        } });
        Object.defineProperty(BroadcastChannel.prototype, 'close', { configurable: true, enumerable: true, writable: true, value: function close() {
            var st = stOf(this);
            if (st.closed) return;
            st.closed = true;
            var list = hub()['bc:' + st.name];
            if (list) { var i = list.indexOf(this); if (i >= 0) list.splice(i, 1); }
        } });
        ['onmessage', 'onmessageerror'].forEach(function (h) {
            var K = Symbol(h), type = h.slice(2);
            Object.defineProperty(BroadcastChannel.prototype, h, { configurable: true, enumerable: true,
                get: function () { stOf(this); return this[K] || null; },
                set: function (v) {
                    stOf(this);
                    if (this[K]) this.removeEventListener(type, this[K]);
                    var f = typeof v === 'function' ? v : null;
                    Object.defineProperty(this, K, { value: f, writable: true, configurable: true });
                    if (f) this.addEventListener(type, f);
                } });
        });
        Object.defineProperty(BroadcastChannel.prototype, Symbol.toStringTag, { configurable: true, value: 'BroadcastChannel' });
        Object.defineProperty(global, 'BroadcastChannel', { configurable: true, writable: true, value: BroadcastChannel });
    })();

    /* ---- Origin (HTML 7.1.1.x): an origin as an object, from what has one ---- */
    (function () {
        if (typeof global.Origin === 'function' || !global.URL) return;
        var OB = Symbol('origin');
        function Origin() {
            if (!new.target) throw new TypeError("Failed to construct 'Origin': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
            Object.defineProperty(this, OB, { value: { opaque: true, id: {} } });
        }
        function make(o) { var x = Object.create(Origin.prototype); Object.defineProperty(x, OB, { value: o }); return x; }
        function opaque() { return { opaque: true, id: {} }; }
        var TUPLE = { 'http:': 1, 'https:': 1, 'ws:': 1, 'wss:': 1, 'ftp:': 1 };
        function ofURL(u) {
            if (u.protocol === 'blob:') {
                try { var inner = new global.URL(u.pathname); if (inner.protocol === 'http:' || inner.protocol === 'https:') return ofURL(inner); } catch (e) { }
                return opaque();
            }
            if (!TUPLE[u.protocol]) return opaque();
            return { opaque: false, scheme: u.protocol.slice(0, -1), host: u.hostname, port: u.port };
        }
        function ofString(s) {
            var u;
            try { u = new global.URL(s); } catch (e) { throw new TypeError("Failed to execute 'from' on 'Origin': The URL '" + s + "' is invalid."); }
            return ofURL(u);
        }
        function ofSerialized(s) {
            if (s === 'null' || !s) return null;
            try { return ofURL(new global.URL(s)); } catch (e) { return null; }
        }
        function bad() { return new TypeError("Failed to execute 'from' on 'Origin': The value has no origin."); }
        Object.defineProperty(Origin, 'from', { configurable: true, writable: true, enumerable: true, value: function from(value) {
            if (arguments.length < 1) throw new TypeError("Failed to execute 'from' on 'Origin': 1 argument required, but only 0 present.");
            if (typeof value === 'string') return make(ofString(value));
            if (!value || (typeof value !== 'object' && typeof value !== 'function')) throw bad();
            if (value[OB]) return make(value[OB]);
            if (global.URL && value instanceof global.URL) return make(ofURL(value));
            if (global.Location && value instanceof global.Location) throw bad();
            if (global.MessageEvent && value instanceof global.MessageEvent) {
                if (!value.isTrusted) throw bad();
                return make(ofSerialized(value.origin) || opaque());
            }
            /* (an element with a URL to follow: <a>, <area>, SVG's <a>) */
            if (value.nodeType === 1) {
                var ns = value.namespaceURI, ln = value.localName;
                var html = ns === 'http://www.w3.org/1999/xhtml' && (ln === 'a' || ln === 'area');
                var svg = ns === 'http://www.w3.org/2000/svg' && ln === 'a';
                var mml = ns === 'http://www.w3.org/1998/Math/MathML' && ln === 'a';
                if (!html && !svg && !mml) throw bad();
                var h = value.getAttribute('href');
                if (h === null && svg) h = value.getAttributeNS('http://www.w3.org/1999/xlink', 'href');
                /* (MathML's: its href, as set on it) */
                if (h === null && mml && Object.prototype.hasOwnProperty.call(value, 'href') && typeof value.href === 'string') h = value.href;
                if (h === null) throw bad();
                var u;
                try { u = new global.URL(h, value.baseURI); } catch (e) { throw bad(); }
                return make(ofURL(u));
            }
            /* (a window, or a worker's global: its document's or its own origin) */
            var loc;
            try {
                if (value === global || (value.window === value && value.document) || (value.self === value && value.location)) {
                    loc = value === global ? global.origin : value.origin;
                    if (loc === undefined && value.location) loc = value.location.origin;
                } else if (global.Worker && value instanceof global.Worker) {
                    loc = value.__cyberOrigin;
                }
            } catch (e) { throw bad(); }
            if (loc === undefined) throw bad();
            var o = ofSerialized(String(loc));
            if (!o) throw bad();
            return o.opaque ? make(o) : make(o);
        } });
        var P = Origin.prototype;
        function brand(x) { if (!x || !x[OB]) throw new TypeError('Illegal invocation'); return x[OB]; }
        Object.defineProperty(P, 'opaque', { configurable: true, enumerable: true, get: function opaque() { return !!brand(this).opaque; } });
        function site(host) {
            if (/^\[|^\d+\.\d+\.\d+\.\d+$/.test(host)) return host;
            var parts = host.split('.');
            return parts.length <= 2 ? host : parts.slice(-2).join('.');
        }
        function other(o, name) {
            if (!o || !o[OB]) throw new TypeError("Failed to execute '" + name + "' on 'Origin': parameter 1 is not of type 'Origin'.");
            return o[OB];
        }
        Object.defineProperty(P, 'isSameOrigin', { configurable: true, writable: true, enumerable: true, value: function isSameOrigin(o) {
            var a = brand(this), b = other(o, 'isSameOrigin');
            if (a.opaque || b.opaque) return a === b || (a.opaque && b.opaque && a.id === b.id);
            return a.scheme === b.scheme && a.host === b.host && a.port === b.port;
        } });
        Object.defineProperty(P, 'isSameSite', { configurable: true, writable: true, enumerable: true, value: function isSameSite(o) {
            var a = brand(this), b = other(o, 'isSameSite');
            if (a.opaque || b.opaque) return a === b || (a.opaque && b.opaque && a.id === b.id);
            return a.scheme === b.scheme && site(a.host) === site(b.host);
        } });
        Object.defineProperty(P, Symbol.toStringTag, { configurable: true, value: 'Origin' });
        Object.defineProperty(global, 'Origin', { configurable: true, writable: true, value: Origin });
    })();

    /* ---- <meta http-equiv=refresh> (HTML 4.2.5.3): the shared declarative refresh steps, once per document ---- */
    (function () {
        var doc = global.document;
        if (!doc || doc.nodeType !== 9 || typeof global.MutationObserver !== 'function') return;
        var WSP = /[\t\n\f\r ]/;
        function parseRefresh(input) {
            var p = 0, n = input.length;
            while (p < n && WSP.test(input[p])) p++;
            var t = '';
            while (p < n && input[p] >= '0' && input[p] <= '9') t += input[p++];
            var time = 0;
            if (t === '') { if (input[p] !== '.') return null; }
            else time = parseInt(t, 10);
            while (p < n && ((input[p] >= '0' && input[p] <= '9') || input[p] === '.')) p++;
            var url = null;
            if (p < n) {
                if (input[p] !== ';' && input[p] !== ',' && !WSP.test(input[p])) return null;
                while (p < n && WSP.test(input[p])) p++;
                if (input[p] === ';' || input[p] === ',') p++;
                while (p < n && WSP.test(input[p])) p++;
            }
            if (p < n) {
                var urlString = input.slice(p), skip = false;
                if (input[p] === 'U' || input[p] === 'u') {
                    p++;
                    if (input[p] === 'R' || input[p] === 'r') {
                        p++;
                        if (input[p] === 'L' || input[p] === 'l') {
                            p++;
                            while (p < n && WSP.test(input[p])) p++;
                            if (input[p] === '=') {
                                p++;
                                while (p < n && WSP.test(input[p])) p++;
                                skip = true;
                            }
                        }
                    }
                } else skip = true;
                if (skip) {
                    var q = input[p] === '"' || input[p] === "'" ? input[p] : null;
                    if (q) p++;
                    urlString = input.slice(p);
                    if (q) { var e = urlString.indexOf(q); if (e >= 0) urlString = urlString.slice(0, e); }
                }
                url = urlString;
            }
            return { time: time, url: url };
        }
        var armed = false;
        function consider(m) {
            if (armed || !m || m.localName !== 'meta' || m.namespaceURI !== 'http://www.w3.org/1999/xhtml') return;
            if (!m.isConnected || m.getRootNode() !== doc) return;
            if (String(m.getAttribute('http-equiv') || '').replace(/^[\t\n\f\r ]+|[\t\n\f\r ]+$/g, '').toLowerCase() !== 'refresh') return;
            var c = m.getAttribute('content');
            if (c === null) return;
            arm(c);
        }
        function arm(c) {
            if (armed) return;
            var r = parseRefresh(c);
            if (!r) return;
            var target;
            try { target = r.url === null ? doc.URL : new global.URL(r.url, doc.baseURI).href; } catch (e) { return; }
            armed = true;
            var go = function () {
                global.setTimeout(function () {
                    if (doc.defaultView !== global) return;
                    try { global.location.replace(target); } catch (e) { }
                }, r.time * 1000);
            };
            if (doc.readyState === 'complete') go(); else onWin('load', go);
        }
        function scan(root) {
            if (armed || !root || root.nodeType !== 1 && root.nodeType !== 9 && root.nodeType !== 11) return;
            if (root.localName === 'meta') consider(root);
            if (root.firstElementChild && root.querySelectorAll) { var l = root.querySelectorAll('meta[http-equiv]'); for (var i = 0; i < l.length && !armed; i++) consider(l[i]); }
        }
        var mo = new global.MutationObserver(function (recs) {
            for (var i = 0; i < recs.length && !armed; i++) {
                var r = recs[i];
                if (r.type === 'attributes') consider(r.target);
                else for (var k = 0; k < r.addedNodes.length; k++) scan(r.addedNodes[k]);
            }
            if (armed) mo.disconnect();
        });
        mo.observe(doc, { childList: true, subtree: true, attributes: true, attributeFilter: ['http-equiv', 'content'] });
        scan(doc);
        onWin('DOMContentLoaded', function () {
            /* (the response's Refresh header, first) */
            if (!armed && typeof doc.__cyber_refresh === 'string') arm(doc.__cyber_refresh);
            scan(doc);
        });
        /* (once loaded, only the head watched: a page's churn in the body costs nothing) */
        onWin('load', function () {
            if (armed || !doc.head) return;
            mo.disconnect();
            mo.observe(doc.head, { childList: true, subtree: true, attributes: true, attributeFilter: ['http-equiv', 'content'] });
            if (doc.body) mo.observe(doc.body, { childList: true });   /* (and the body's own children: cheap, and what script appends there) */
        });
    })();

    /* ---- img.decode() (HTML 4.8.4.3.x): settles with the image's load, rejects if it breaks or its source changes first ---- */
    (function () {
        var IP = global.HTMLImageElement && global.HTMLImageElement.prototype;
        if (!IP || typeof global.MutationObserver !== 'function') return;
        function encodingError() { return new global.DOMException('The source image cannot be decoded.', 'EncodingError'); }
        Object.defineProperty(IP, 'decode', { configurable: true, writable: true, enumerable: true, value: function decode() {
            var img = this;
            if (!(img instanceof global.HTMLImageElement)) return Promise.reject(new TypeError('Illegal invocation'));
            return new Promise(function (resolve, reject) {
                var d = img.ownerDocument;
                if (!d || !d.defaultView) { reject(encodingError()); return; }
                var done = false, mo = null;
                function finish(ok) {
                    if (done) return;
                    done = true;
                    img.removeEventListener('load', onload);
                    img.removeEventListener('error', onerror);
                    if (mo) mo.disconnect();
                    if (ok) resolve(); else reject(encodingError());
                }
                function onload() { finish(img.naturalWidth > 0 || img.naturalHeight > 0 || /\.svg(\?|#|$)/i.test(img.currentSrc || '') || true); }
                function onerror() { finish(false); }
                var hasSrc = img.hasAttribute('src') || img.hasAttribute('srcset') || (img.parentNode && img.parentNode.localName === 'picture');
                if (!hasSrc) { global.setTimeout(function () { finish(false); }, 0); return; }
                img.addEventListener('load', onload);
                img.addEventListener('error', onerror);
                /* (its source changed while waiting: this decode fails) */
                mo = new global.MutationObserver(function () { finish(false); });
                mo.observe(img, { attributes: true, attributeFilter: ['src', 'srcset', 'sizes', 'crossorigin', 'referrerpolicy'] });
                /* (no load or error event in a while: settled by what it has) */
                global.setTimeout(function () { if (!done) finish(img.naturalWidth > 0); }, 8000);
                /* (already there: settled now, by what it is) */
                if (img.complete) global.setTimeout(function () {
                    if (done) return;
                    if (img.naturalWidth > 0 || img.naturalHeight > 0) finish(true);
                    else if (img.complete) finish(false);
                }, 0);
            });
        } });
    })();

    /* ---- the document's collections (HTML 3.1.5) and named properties (3.1.6) ---- */
    (function () {
        var DocP = global.Document && global.Document.prototype, HC = global.HTMLCollection;
        if (!DocP || !HC || typeof Proxy !== 'function') return;
        var XHTML = 'http://www.w3.org/1999/xhtml';
        function isIndex(k) { return typeof k === 'string' && /^(0|[1-9]\d*)$/.test(k); }
        /* (a live HTMLCollection over what items() finds each time) */
        function liveColl(items) {
            var t = Object.create(HC.prototype);
            Object.defineProperty(t, 'length', { configurable: true, get: function () { return items().length; } });
            Object.defineProperty(t, 'item', { configurable: true, value: function item(i) { return items()[i >>> 0] || null; } });
            Object.defineProperty(t, 'namedItem', { configurable: true, value: function namedItem(n) {
                n = String(n);
                if (!n) return null;
                var l = items();
                for (var i = 0; i < l.length; i++) if (l[i].id === n || (l[i].namespaceURI === XHTML && l[i].getAttribute('name') === n)) return l[i];
                return null;
            } });
            return new Proxy(t, {
                get: function (o, k, r) { if (isIndex(k)) return items()[+k]; return Reflect.get(o, k, r); },
                has: function (o, k) { if (isIndex(k)) return +k < items().length; return k in o; },
                ownKeys: function (o) { var n = items().length, ks = []; for (var i = 0; i < n; i++) ks.push(String(i)); return ks.concat(Reflect.ownKeys(o)); },
                getOwnPropertyDescriptor: function (o, k) {
                    if (isIndex(k)) { var e = items()[+k]; return e ? { value: e, enumerable: true, configurable: true, writable: false } : undefined; }
                    return Reflect.getOwnPropertyDescriptor(o, k);
                },
            });
        }
        var cache = new WeakMap();
        function coll(doc, key, make) {
            var c = cache.get(doc);
            if (!c) { c = {}; cache.set(doc, c); }
            return c[key] || (c[key] = make(doc));
        }
        function byTag(tag) { return function (doc) { return doc.getElementsByTagNameNS(XHTML, tag); }; }
        function filtered(sel, test) {
            return function (doc) {
                return liveColl(function () {
                    var l = doc.querySelectorAll(sel), out = [];
                    for (var i = 0; i < l.length; i++) if (l[i].namespaceURI === XHTML && (!test || test(l[i]))) out.push(l[i]);
                    return out;
                });
            };
        }
        var DEFS = {
            images: byTag('img'), embeds: byTag('embed'), plugins: byTag('embed'), forms: byTag('form'), scripts: byTag('script'),
            links: filtered('a[href], area[href]'), anchors: filtered('a[name]'),
            applets: function () { return liveColl(function () { return []; }); },
        };
        Object.keys(DEFS).forEach(function (k) {
            var d = Object.getOwnPropertyDescriptor(DocP, k);
            if (d && d.get) return;
            var g = { get: function () {
                if (!(this instanceof global.Document)) throw new TypeError('Illegal invocation');
                return coll(this, k === 'plugins' ? 'embeds' : k, DEFS[k]);
            } };
            Object.defineProperty(DocP, k, { configurable: true, enumerable: true, get: Object.getOwnPropertyDescriptor(g, 'get').value });
        });

        /* getElementsByName: a live NodeList of the HTML elements so named */
        var NLP = global.NodeList && global.NodeList.prototype;
        function liveNodeList(items) {
            var t = Object.create(NLP || HC.prototype);
            Object.defineProperty(t, 'length', { configurable: true, get: function () { return items().length; } });
            Object.defineProperty(t, 'item', { configurable: true, value: function item(i) { return items()[i >>> 0] || null; } });
            return new Proxy(t, {
                get: function (o, k, r) { if (isIndex(k)) return items()[+k]; return Reflect.get(o, k, r); },
                has: function (o, k) { if (isIndex(k)) return +k < items().length; return k in o; },
                ownKeys: function (o) { var n = items().length, ks = []; for (var i = 0; i < n; i++) ks.push(String(i)); return ks.concat(Reflect.ownKeys(o)); },
                getOwnPropertyDescriptor: function (o, k) {
                    if (isIndex(k)) { var e = items()[+k]; return e ? { value: e, enumerable: true, configurable: true, writable: false } : undefined; }
                    return Reflect.getOwnPropertyDescriptor(o, k);
                },
            });
        }
        Object.defineProperty(DocP, 'getElementsByName', { configurable: true, writable: true, enumerable: true, value: function getElementsByName(name) {
            if (arguments.length < 1) throw new TypeError("Failed to execute 'getElementsByName' on 'Document': 1 argument required, but only 0 present.");
            var d = this;
            name = String(name);
            return liveNodeList(function () {
                var l = d.querySelectorAll('[name]'), out = [];
                for (var i = 0; i < l.length; i++) if (l[i].namespaceURI === XHTML && l[i].getAttribute('name') === name) out.push(l[i]);
                return out;
            });
        } });
        if (global.document && Object.prototype.hasOwnProperty.call(global.document, 'getElementsByName')) { try { delete global.document.getElementsByName; } catch (e) { } }
        /* HTMLDocument: the document's interface, Document's own as the standard keeps it */
        if (typeof global.HTMLDocument !== 'function') {
            var HD = function HTMLDocument() { throw new TypeError('Illegal constructor'); };
            HD.prototype = Object.create(DocP);
            Object.defineProperty(HD.prototype, 'constructor', { value: HD, writable: true, configurable: true });
            Object.defineProperty(HD.prototype, Symbol.toStringTag, { value: 'HTMLDocument', configurable: true });
            Object.setPrototypeOf(HD, global.Document);
            Object.defineProperty(global, 'HTMLDocument', { value: HD, writable: true, configurable: true });
        }

        /* the document's named properties: embed, form, iframe, img and object elements by name (and object, img by id) */
        var doc = global.document;
        if (!doc || doc.nodeType !== 9 || (doc.contentType && doc.contentType !== 'text/html')) return;
        var NAMED_TAGS = { embed: 1, form: 1, iframe: 1, img: 1, object: 1 };
        function namedFor(d, k) {
            if (!k) return [];
            var out = [], l = d.querySelectorAll('embed[name], form[name], iframe[name], img[name], object[name]');
            for (var i = 0; i < l.length; i++) if (l[i].namespaceURI === XHTML && l[i].getAttribute('name') === k) out.push(l[i]);
            /* (an object by its id, an img by its id when it has a name too) */
            var byId = d.getElementById(k);
            if (byId && out.indexOf(byId) < 0 && byId.namespaceURI === XHTML && (byId.localName === 'object' || (byId.localName === 'img' && byId.getAttribute('name')))) {
                var all = d.querySelectorAll('object[id], img[id][name]');
                for (var j = 0; j < all.length; j++) if (all[j].id === k && out.indexOf(all[j]) < 0) out.push(all[j]);
                out.sort(function (x, y) { return x === y ? 0 : x.compareDocumentPosition(y) & 4 ? -1 : 1; });
            }
            return out;
        }
        function namedValue(d, k) {
            var l = namedFor(d, k);
            if (!l.length) return undefined;
            if (l.length === 1) return l[0].localName === 'iframe' ? l[0].contentWindow : l[0];
            return liveColl(function () { return namedFor(d, k); });
        }
        var base = Object.getPrototypeOf(doc);
        if (base !== DocP && base !== global.HTMLDocument.prototype) return;
        /* (between the document and HTMLDocument.prototype: a names object, the builtins first) */
        var named = new Proxy(Object.create(global.HTMLDocument.prototype), {
            get: function (o, k, r) {
                if (typeof k === 'string' && !(k in o)) { var v = namedValue(doc, k); if (v !== undefined) return v; }
                return Reflect.get(o, k, r);
            },
            has: function (o, k) {
                if (k in o) return true;
                return typeof k === 'string' && namedFor(doc, k).length > 0;
            },
            setPrototypeOf: function () { return false; },
        });
        try { Object.setPrototypeOf(doc, named); } catch (e) { }
    })();

    /* ---- WebSocket (WebSockets Standard): the object, its URL and checks; no connection is made: it fails, error then close ---- */
    (function () {
        if (typeof global.WebSocket === 'function' || typeof global.EventTarget !== 'function') return;
        var WSK = Symbol('webSocket');
        var WebSocket = function WebSocket(url, protocols) {
            if (!new.target) throw new TypeError("Failed to construct 'WebSocket': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
            if (arguments.length < 1) throw new TypeError("Failed to construct 'WebSocket': 1 argument required, but only 0 present.");
            var base = (global.document && global.document.baseURI) || (global.location && global.location.href), u;
            try { u = new global.URL(String(url), base); } catch (e) { throw new global.DOMException("Failed to construct 'WebSocket': The URL '" + url + "' is invalid.", 'SyntaxError'); }
            if (u.protocol === 'http:') u.protocol = 'ws:';
            else if (u.protocol === 'https:') u.protocol = 'wss:';
            if (u.protocol !== 'ws:' && u.protocol !== 'wss:') throw new global.DOMException("Failed to construct 'WebSocket': The URL's scheme must be either 'http', 'https', 'ws', or 'wss'. '" + u.protocol + "' is not allowed.", 'SyntaxError');
            if (u.hash) throw new global.DOMException("Failed to construct 'WebSocket': The URL contains a fragment identifier ('" + u.hash + "'). Fragment identifiers are not allowed in WebSocket URLs.", 'SyntaxError');
            var ps = protocols === undefined ? [] : typeof protocols === 'string' ? [protocols] : Array.from(protocols, String);
            var seen = {};
            ps.forEach(function (p) {
                if (!/^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/.test(p) || seen[p]) throw new global.DOMException("Failed to construct 'WebSocket': The subprotocol '" + p + "' is invalid.", 'SyntaxError');
                seen[p] = 1;
            });
            var self = Reflect.construct(global.EventTarget, [], new.target);
            var st = { url: u.href, readyState: 0, binaryType: 'blob' };
            Object.defineProperty(self, WSK, { value: st });
            global.setTimeout(function () {
                if (st.readyState === 3) return;
                st.readyState = 3;
                try { self.dispatchEvent(new global.Event('error')); } catch (e) { }
                var CE = global.CloseEvent || global.Event;
                try { self.dispatchEvent(new CE('close', { wasClean: false, code: 1006, reason: '' })); } catch (e) { }
            }, 0);
            return self;
        };
        WebSocket.prototype = Object.create(global.EventTarget.prototype);
        Object.defineProperty(WebSocket.prototype, 'constructor', { value: WebSocket, writable: true, configurable: true });
        Object.setPrototypeOf(WebSocket, global.EventTarget);
        ['CONNECTING', 'OPEN', 'CLOSING', 'CLOSED'].forEach(function (k, i) {
            Object.defineProperty(WebSocket, k, { value: i, enumerable: true });
            Object.defineProperty(WebSocket.prototype, k, { value: i, enumerable: true });
        });
        var wst = function (x) { var s = x && x[WSK]; if (!s) throw new TypeError('Illegal invocation'); return s; };
        var wget = function (k, f) {
            var g = { [k]: function () { return f(wst(this)); } }[k];
            Object.defineProperty(g, 'name', { value: 'get ' + k });
            Object.defineProperty(WebSocket.prototype, k, { configurable: true, enumerable: true, get: g });
        };
        wget('url', function (s) { return s.url; });
        wget('readyState', function (s) { return s.readyState; });
        wget('bufferedAmount', function () { return 0; });
        wget('extensions', function () { return ''; });
        wget('protocol', function () { return ''; });
        Object.defineProperty(WebSocket.prototype, 'binaryType', { configurable: true, enumerable: true,
            get: function () { return wst(this).binaryType; },
            set: function (v) { var s = wst(this); if (v === 'blob' || v === 'arraybuffer') s.binaryType = v; } });
        ['onopen', 'onerror', 'onclose', 'onmessage'].forEach(function (h) {
            var K = Symbol(h), type = h.slice(2);
            Object.defineProperty(WebSocket.prototype, h, { configurable: true, enumerable: true,
                get: function () { wst(this); return this[K] || null; },
                set: function (v) {
                    wst(this);
                    if (this[K]) this.removeEventListener(type, this[K]);
                    var f = typeof v === 'function' ? v : null;
                    Object.defineProperty(this, K, { value: f, writable: true, configurable: true });
                    if (f) this.addEventListener(type, f);
                } });
        });
        Object.defineProperty(WebSocket.prototype, 'send', { configurable: true, enumerable: true, writable: true, value: function send(data) {
            var s = wst(this);
            if (arguments.length < 1) throw new TypeError("Failed to execute 'send' on 'WebSocket': 1 argument required, but only 0 present.");
            if (s.readyState === 0) throw new global.DOMException("Failed to execute 'send' on 'WebSocket': Still in CONNECTING state.", 'InvalidStateError');
        } });
        Object.defineProperty(WebSocket.prototype, 'close', { configurable: true, enumerable: true, writable: true, value: function close(code, reason) {
            var s = wst(this);
            if (code !== undefined && code !== 1000 && !(code >= 3000 && code <= 4999))
                throw new global.DOMException("Failed to execute 'close' on 'WebSocket': The close code must be either 1000, or between 3000 and 4999. " + code + ' is neither.', 'InvalidAccessError');
            if (s.readyState < 2) s.readyState = 2;
        } });
        Object.defineProperty(WebSocket.prototype, Symbol.toStringTag, { configurable: true, value: 'WebSocket' });
        Object.defineProperty(global, 'WebSocket', { configurable: true, writable: true, value: WebSocket });
    })();

    /* ---- EventSource (HTML 9.2): server-sent events over fetch; DOMStringList (HTML 2.6.x) ---- */
    (function () {
        var ETc = global.EventTarget;
        if (typeof global.EventSource !== 'function' && ETc && typeof global.fetch === 'function') {
            var ES = Symbol('eventSource');
            var EventSource = function EventSource(url, init) {
                if (!new.target) throw new TypeError("Failed to construct 'EventSource': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
                if (arguments.length < 1) throw new TypeError("Failed to construct 'EventSource': 1 argument required, but only 0 present.");
                var base = (global.document && global.document.baseURI) || (global.location && global.location.href);
                var u;
                try { u = new global.URL(String(url), base); } catch (e) { throw new global.DOMException("Failed to construct 'EventSource': Cannot open an EventSource to '" + url + "'. The URL is invalid.", 'SyntaxError'); }
                /* (the realm's EventTarget now: the one there when this was set up may have been replaced since) */
                var self = Reflect.construct(global.EventTarget || ETc, [], new.target);
                var st = { url: u.href, withCredentials: !!(init && init.withCredentials), readyState: 0, lastId: '', retry: 3000, ctrl: null, timer: 0 };
                Object.defineProperty(self, ES, { value: st });
                connect(self, st);
                return self;
            };
            EventSource.prototype = Object.create(ETc.prototype);
            Object.defineProperty(EventSource.prototype, 'constructor', { value: EventSource, writable: true, configurable: true });
            Object.setPrototypeOf(EventSource, ETc);
            ['CONNECTING', 'OPEN', 'CLOSED'].forEach(function (k, i) {
                Object.defineProperty(EventSource, k, { value: i, enumerable: true });
                Object.defineProperty(EventSource.prototype, k, { value: i, enumerable: true });
            });
            var stOf = function (x) { var s = x && x[ES]; if (!s) throw new TypeError('Illegal invocation'); return s; };
            ['url', 'withCredentials', 'readyState'].forEach(function (k) {
                var g = { get: function () { return stOf(this)[k]; } };
                Object.defineProperty(EventSource.prototype, k, { configurable: true, enumerable: true, get: Object.getOwnPropertyDescriptor(g, 'get').value });
            });
            ['onopen', 'onmessage', 'onerror'].forEach(function (h) {
                var K = Symbol(h), type = h.slice(2);
                Object.defineProperty(EventSource.prototype, h, { configurable: true, enumerable: true,
                    get: function () { stOf(this); return this[K] || null; },
                    set: function (v) {
                        stOf(this);
                        if (this[K]) this.removeEventListener(type, this[K]);
                        var f = typeof v === 'function' ? v : null;
                        Object.defineProperty(this, K, { value: f, writable: true, configurable: true });
                        if (f) this.addEventListener(type, f);
                    } });
            });
            Object.defineProperty(EventSource.prototype, 'close', { configurable: true, enumerable: true, writable: true, value: function close() {
                var st = stOf(this);
                st.readyState = 2;
                if (st.timer) { global.clearTimeout(st.timer); st.timer = 0; }
                if (st.ctrl) { try { st.ctrl.abort(); } catch (e) { } st.ctrl = null; }
            } });
            Object.defineProperty(EventSource.prototype, Symbol.toStringTag, { configurable: true, value: 'EventSource' });
            var fire = function (t, ev) { if (global.__cyberDispatchTrusted) global.__cyberDispatchTrusted(t, ev); else t.dispatchEvent(ev); };
            var reconnect = function (es, st) {
                if (st.readyState === 2) return;
                st.readyState = 0;
                fire(es, new global.Event('error'));
                if (st.readyState === 2) return;
                st.timer = global.setTimeout(function () { st.timer = 0; if (st.readyState !== 2) connect(es, st); }, st.retry);
            };
            var fail = function (es, st) {
                if (st.readyState === 2) return;
                st.readyState = 2;
                fire(es, new global.Event('error'));
            };
            var connect = function (es, st) {
                var ctrl = global.AbortController ? new global.AbortController() : null;
                st.ctrl = ctrl;
                var headers = { 'Accept': 'text/event-stream', 'Cache-Control': 'no-store' };
                if (st.lastId) headers['Last-Event-ID'] = st.lastId;
                var buf = '', data = '', type = '', origin = '';
                try { origin = new global.URL(st.url).origin; } catch (e) { }
                var line = function (l) {
                    if (l === '') {
                        if (data !== '') {
                            if (data.charAt(data.length - 1) === '\n') data = data.slice(0, -1);
                            var ME = global.MessageEvent || global.Event;
                            var ev = new ME(type || 'message', { data: data, origin: origin, lastEventId: st.lastId });
                            if (st.readyState !== 2) fire(es, ev);
                        }
                        data = ''; type = '';
                        return;
                    }
                    if (l.charAt(0) === ':') return;
                    var c = l.indexOf(':'), f = c < 0 ? l : l.slice(0, c), v = c < 0 ? '' : l.slice(c + 1);
                    if (v.charAt(0) === ' ') v = v.slice(1);
                    if (f === 'data') data += v + '\n';
                    else if (f === 'event') type = v;
                    else if (f === 'id') { if (v.indexOf('\0') < 0) st.lastId = v; }
                    else if (f === 'retry') { if (/^\d+$/.test(v)) st.retry = +v; }
                };
                var feed = function (chunk) {
                    buf += chunk;
                    var m;
                    while ((m = /\r\n|\r|\n/.exec(buf))) {
                        /* (a CR at the end might be half of a CRLF still coming) */
                        if (m[0] === '\r' && m.index === buf.length - 1) break;
                        line(buf.slice(0, m.index));
                        buf = buf.slice(m.index + m[0].length);
                    }
                };
                global.fetch(st.url, { headers: headers, credentials: st.withCredentials ? 'include' : 'same-origin', cache: 'no-store', signal: ctrl ? ctrl.signal : undefined })
                    .then(function (r) {
                        if (st.readyState === 2) return;
                        var ct = String(r.headers.get('content-type') || '').split(';')[0].trim().toLowerCase();
                        if (r.status !== 200 || ct !== 'text/event-stream') { fail(es, st); return; }
                        st.readyState = 1;
                        fire(es, new global.Event('open'));
                        var dec = global.TextDecoder ? new global.TextDecoder() : null;
                        var reader = r.body && r.body.getReader ? r.body.getReader() : null;
                        if (reader && dec) {
                            var pump = function () {
                                return reader.read().then(function (x) {
                                    if (st.readyState === 2) return;
                                    if (x.done) { feed(dec.decode()); reconnect(es, st); return; }
                                    feed(dec.decode(x.value, { stream: true }));
                                    return pump();
                                });
                            };
                            return pump();
                        }
                        return r.text().then(function (t) { if (st.readyState === 2) return; feed(t); reconnect(es, st); });
                    })
                    .catch(function () { if (st.readyState !== 2 && !(ctrl && ctrl.signal.aborted)) reconnect(es, st); });
            };
            Object.defineProperty(global, 'EventSource', { configurable: true, writable: true, value: EventSource });
        }

        /* DOMStringList: a list of strings (location.ancestorOrigins and the like) */
        if (typeof global.DOMStringList !== 'function') {
            var DSB = Symbol('stringList');
            var DOMStringList = function DOMStringList() { throw new TypeError('Illegal constructor'); };
            var DP = DOMStringList.prototype;
            Object.defineProperty(DP, 'length', { configurable: true, enumerable: true, get: function length() { var l = this[DSB]; if (!l) throw new TypeError('Illegal invocation'); return l.length; } });
            Object.defineProperty(DP, 'item', { configurable: true, enumerable: true, writable: true, value: function item(i) {
                var l = this[DSB]; if (!l) throw new TypeError('Illegal invocation');
                if (arguments.length < 1) throw new TypeError("Failed to execute 'item' on 'DOMStringList': 1 argument required, but only 0 present.");
                i = Number(i); i = isFinite(i) ? Math.trunc(i) >>> 0 : 0;
                return i < l.length ? l[i] : null;
            } });
            Object.defineProperty(DP, 'contains', { configurable: true, enumerable: true, writable: true, value: function contains(s) {
                var l = this[DSB]; if (!l) throw new TypeError('Illegal invocation');
                if (arguments.length < 1) throw new TypeError("Failed to execute 'contains' on 'DOMStringList': 1 argument required, but only 0 present.");
                return l.indexOf(String(s)) >= 0;
            } });
            Object.defineProperty(DP, Symbol.toStringTag, { configurable: true, value: 'DOMStringList' });
            Object.defineProperty(global, 'DOMStringList', { configurable: true, writable: true, value: DOMStringList });
            var makeList = function (arr) {
                var o = Object.create(DP);
                Object.defineProperty(o, DSB, { value: arr.slice() });
                arr.forEach(function (v, i) { Object.defineProperty(o, i, { value: v, enumerable: true }); });
                return o;
            };
            Object.defineProperty(global, '__cyberStringList', { configurable: true, value: makeList });
            /* (location.ancestorOrigins: the origins of the documents it is nested in) */
            try {
                var loc = global.location;
                if (loc && Object.getOwnPropertyDescriptor(loc, 'ancestorOrigins')) {
                    var anc = [];
                    try { for (var w = global; w.parent && w.parent !== w; w = w.parent) anc.push(w.parent.location.origin); } catch (e) { }
                    var ao = makeList(anc);
                    Object.defineProperty(loc, 'ancestorOrigins', { configurable: true, enumerable: true, get: function ancestorOrigins() { return ao; } });
                }
            } catch (e) { }
        }
    })();

    /* ---- VTTCue (WebVTT): a TextTrackCue with its settings and text ---- */
    (function () {
        var TTC = global.TextTrackCue;
        if (typeof TTC !== 'function' || typeof global.VTTCue === 'function' || typeof global.__cyberNewCue !== 'function') return;
        var VB = Symbol('vttCue');
        var VTTCue = function VTTCue(startTime, endTime, text) {
            if (!new.target) throw new TypeError("Failed to construct 'VTTCue': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
            if (arguments.length < 3) throw new TypeError("Failed to construct 'VTTCue': 3 arguments required, but only " + arguments.length + " present.");
            var s = Number(startTime), e = Number(endTime);
            if (!isFinite(s) || !isFinite(e)) throw new TypeError("Failed to construct 'VTTCue': The provided double value is non-finite.");
            var self = Object.create(new.target.prototype);
            global.__cyberNewCue(self, s, e);
            Object.defineProperty(self, VB, { value: { region: null, vertical: '', snapToLines: true, line: 'auto', lineAlign: 'start',
                position: 'auto', positionAlign: 'auto', size: 100, align: 'center', text: String(text) } });
            return self;
        };
        VTTCue.prototype = Object.create(TTC.prototype);
        Object.defineProperty(VTTCue.prototype, 'constructor', { value: VTTCue, writable: true, configurable: true });
        Object.setPrototypeOf(VTTCue, TTC);
        var st = function (x) { var v = x && x[VB]; if (!v) throw new TypeError('Illegal invocation'); return v; };
        var acc = function (k, set) {
            Object.defineProperty(VTTCue.prototype, k, { configurable: true, enumerable: true,
                get: function () { return st(this)[k]; },
                set: function (v) { var s = st(this); set(s, v); } });
        };
        var enumSet = function (k, list) { return function (s, v) { v = String(v); if (list.indexOf(v) >= 0) s[k] = v; }; };
        acc('region', function (s, v) { s.region = v === undefined ? null : v; });
        acc('vertical', enumSet('vertical', ['', 'rl', 'lr']));
        acc('snapToLines', function (s, v) { s.snapToLines = !!v; });
        acc('line', function (s, v) { if (v === 'auto') s.line = 'auto'; else { var n = Number(v); if (!isFinite(n)) throw new TypeError("Failed to set the 'line' property on 'VTTCue': The provided double value is non-finite."); s.line = n; } });
        acc('lineAlign', enumSet('lineAlign', ['start', 'center', 'end']));
        acc('position', function (s, v) {
            if (v === 'auto') { s.position = 'auto'; return; }
            var n = Number(v);
            if (n < 0 || n > 100 || !isFinite(n)) throw new global.DOMException("Failed to set the 'position' property on 'VTTCue': The value provided (" + v + ") is outside the range [0, 100].", 'IndexSizeError');
            s.position = n;
        });
        acc('positionAlign', enumSet('positionAlign', ['line-left', 'center', 'line-right', 'auto']));
        acc('size', function (s, v) {
            var n = Number(v);
            if (n < 0 || n > 100 || !isFinite(n)) throw new global.DOMException("Failed to set the 'size' property on 'VTTCue': The value provided (" + v + ") is outside the range [0, 100].", 'IndexSizeError');
            s.size = n;
        });
        acc('align', enumSet('align', ['start', 'center', 'end', 'left', 'right']));
        acc('text', function (s, v) { s.text = String(v); });
        /* (WebVTT cue text parsing: c, i, b, u, ruby, rt, v and lang, their classes; entities; timestamps as PIs) */
        var XH = 'http://www.w3.org/1999/xhtml';
        var decode = function (t) {
            if (t.indexOf('&') < 0) return t;
            var ta = global.document.createElement('textarea');
            ta.innerHTML = t.replace(/</g, '&lt;');
            return ta.value;
        };
        Object.defineProperty(VTTCue.prototype, 'getCueAsHTML', { configurable: true, enumerable: true, writable: true, value: function getCueAsHTML() {
            var s = st(this), d = global.document, f = d.createDocumentFragment(), text = s.text;
            var stack = [{ node: f, tag: '' }], buf = '';
            var flush = function () { if (buf) { stack[stack.length - 1].node.appendChild(d.createTextNode(decode(buf))); buf = ''; } };
            var i = 0;
            while (i < text.length) {
                var ch = text[i];
                if (ch !== '<') { buf += ch; i++; continue; }
                var e = text.indexOf('>', i);
                var tagText = e < 0 ? text.slice(i + 1) : text.slice(i + 1, e);
                i = e < 0 ? text.length : e + 1;
                if (/^\d/.test(tagText)) {
                    flush();
                    if (/^(?:\d{2,}:)?[0-5]\d:[0-5]\d\.\d{3}$/.test(tagText)) stack[stack.length - 1].node.appendChild(d.createProcessingInstruction('timestamp', tagText));
                    continue;
                }
                if (tagText[0] === '/') {
                    var nm = tagText.slice(1).replace(/[\s.].*$/, '');
                    var cur = stack[stack.length - 1];
                    if (nm === cur.tag) { flush(); stack.pop(); }
                    else if (nm === 'ruby' && cur.tag === 'rt' && stack.length > 2 && stack[stack.length - 2].tag === 'ruby') { flush(); stack.pop(); stack.pop(); }
                    continue;
                }
                var m = /^([^\s.]*)((?:\.[^\s.]*)*)(?:\s+([\s\S]*))?$/.exec(tagText);
                if (!m) continue;
                var tag = m[1], classes = m[2] ? m[2].split('.').filter(Boolean) : [], ann = m[3] ? m[3].replace(/^\s+|\s+$/g, '').replace(/\s+/g, ' ') : '';
                if (!/^(c|i|b|u|ruby|rt|v|lang)$/.test(tag)) continue;
                if (tag === 'rt' && stack[stack.length - 1].tag !== 'ruby') continue;
                flush();
                var el = d.createElementNS(XH, tag === 'c' || tag === 'v' || tag === 'lang' ? 'span' : tag);
                if (classes.length) el.className = classes.join(' ');
                if (tag === 'v') el.title = decode(ann);
                if (tag === 'lang') el.lang = ann;
                stack[stack.length - 1].node.appendChild(el);
                stack.push({ node: el, tag: tag });
            }
            flush();
            return f;
        } });
        Object.defineProperty(VTTCue.prototype, Symbol.toStringTag, { configurable: true, value: 'VTTCue' });
        Object.defineProperty(global, 'VTTCue', { configurable: true, writable: true, value: VTTCue });

        /* media: preservesPitch; volume a finite number in [0, 1] */
        var MP2 = null;
        for (var p = global.HTMLVideoElement && global.HTMLVideoElement.prototype; p && !MP2; p = Object.getPrototypeOf(p))
            if (Object.getOwnPropertyDescriptor(p, 'buffered')) MP2 = p;
        if (MP2) {
            if (!('preservesPitch' in MP2)) {
                var PP = new WeakMap();
                Object.defineProperty(MP2, 'preservesPitch', { configurable: true, enumerable: true,
                    get: function () { return PP.has(this) ? PP.get(this) : true; },
                    set: function (v) { PP.set(this, !!v); } });
            }
            var vd = Object.getOwnPropertyDescriptor(MP2, 'volume');
            if (vd && vd.set) Object.defineProperty(MP2, 'volume', { configurable: true, enumerable: true, get: vd.get,
                set: function (v) {
                    var n = Number(v);
                    if (!isFinite(n)) throw new TypeError("Failed to set the 'volume' property on 'HTMLMediaElement': The provided double value is non-finite.");
                    if (n < 0 || n > 1) throw new global.DOMException("Failed to set the 'volume' property on 'HTMLMediaElement': The volume provided (" + n + ") is outside the range [0, 1].", 'IndexSizeError');
                    if (vd.get && vd.get.call(this) === n) return;   /* (the same volume: no volumechange) */
                    vd.set.call(this, n);
                } });
            var md = Object.getOwnPropertyDescriptor(MP2, 'muted');
            /* (muted: the attribute's until the setter is used; from then on, what was set) */
            var LATCH = new WeakMap();
            if (md && md.set && md.get) Object.defineProperty(MP2, 'muted', { configurable: true, enumerable: true,
                get: { muted: function () { return LATCH.has(this) ? LATCH.get(this) : md.get.call(this); } }.muted,
                set: { muted: function (v) { v = !!v; LATCH.set(this, v); if (md.get.call(this) !== v) md.set.call(this, v); } }.muted });
        }
    })();

    /* ---- XML serialization (DOM Parsing 3.2): XMLSerializer, and innerHTML / outerHTML in XML documents ---- */
    (function () {
        var XML_NS = 'http://www.w3.org/XML/1998/namespace', XMLNS_NS = 'http://www.w3.org/2000/xmlns/', HTML_NS = 'http://www.w3.org/1999/xhtml';
        var VOID = { area: 1, base: 1, basefont: 1, bgsound: 1, br: 1, col: 1, embed: 1, frame: 1, hr: 1, img: 1, input: 1, keygen: 1,
                     link: 1, menuitem: 1, meta: 1, param: 1, source: 1, track: 1, wbr: 1 };
        var esc = function (v, attr) {
            v = String(v).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
            return attr ? v.replace(/"/g, '&quot;') : v;
        };
        var copyMap = function (m) { var n = new Map(); m.forEach(function (v, k) { n.set(k, v.slice()); }); return n; };
        var addPrefix = function (m, prefix, ns) { var l = m.get(ns); if (l) l.push(prefix); else m.set(ns, [prefix]); };
        var preferred = function (m, ns, pref) {
            var l = m.get(ns);
            if (!l || !l.length) return null;
            return l.indexOf(pref) >= 0 ? pref : l[l.length - 1];
        };
        var recordNs = function (el, m, local) {
            var def = null, at = el.attributes;
            for (var i = 0; i < at.length; i++) {
                var a = at[i];
                if (a.namespaceURI !== XMLNS_NS) continue;
                if (a.prefix === null) { def = a.value; continue; }
                var p = a.localName, ns = a.value;
                if (ns === XML_NS) continue;
                if (ns === '') ns = null;
                var l = m.get(ns);
                if (l && l.indexOf(p) >= 0) continue;
                addPrefix(m, p, ns);
                local[p] = ns;
            }
            return def;
        };
        var ser = function (node, ns, m, ctr, out) {
            switch (node.nodeType) {
            case 1: {
                var local = {}, ignoreDef = false, mp = copyMap(m), qn, markup = '<';
                var defNs = recordNs(node, mp, local), inherited = ns, nsu = node.namespaceURI, ln = node.localName;
                if (inherited === nsu) {
                    if (defNs !== null) ignoreDef = true;
                    qn = nsu === XML_NS ? 'xml:' + ln : ln;
                    markup += qn;
                } else {
                    var prefix = node.prefix, cand = preferred(mp, nsu, prefix);
                    if (prefix === 'xmlns') cand = prefix;
                    if (cand !== null) {
                        qn = cand + ':' + ln;
                        if (defNs !== null && defNs !== XML_NS) inherited = defNs === '' ? null : defNs;
                        markup += qn;
                    } else if (prefix !== null) {
                        if (Object.prototype.hasOwnProperty.call(local, prefix)) prefix = 'ns' + (ctr.i++);
                        addPrefix(mp, prefix, nsu);
                        qn = prefix + ':' + ln;
                        markup += qn + ' xmlns:' + prefix + '="' + esc(nsu, true) + '"';
                        if (defNs !== null) inherited = defNs === '' ? null : defNs;
                    } else if (defNs === null || defNs !== nsu) {
                        ignoreDef = true;
                        qn = ln;
                        inherited = nsu;
                        markup += qn + ' xmlns="' + esc(nsu === null ? '' : nsu, true) + '"';
                    } else {
                        qn = ln;
                        inherited = nsu;
                        markup += qn;
                    }
                }
                var at = node.attributes;
                for (var i = 0; i < at.length; i++) {
                    var a = at[i], ans = a.namespaceURI, cp = null;
                    if (ans !== null) {
                        cp = preferred(mp, ans, a.prefix);
                        if (ans === XMLNS_NS) {
                            if (a.value === XML_NS || (a.prefix === null && ignoreDef) ||
                                (a.prefix !== null && (!Object.prototype.hasOwnProperty.call(local, a.localName) || local[a.localName] !== a.value))) continue;
                            cp = 'xmlns';
                        } else if (cp === null) {
                            cp = 'ns' + (ctr.i++);
                            addPrefix(mp, cp, ans);
                            markup += ' xmlns:' + cp + '="' + esc(ans, true) + '"';
                        }
                    }
                    markup += ' ' + (cp !== null ? cp + ':' : '') + a.localName + '="' + esc(a.value, true) + '"';
                }
                var kids = nsu === HTML_NS && ln === 'template' && node.content ? node.content.childNodes : node.childNodes;
                var skipEnd = false;
                if (nsu === HTML_NS && !kids.length && VOID[ln]) { markup += ' /'; skipEnd = true; }
                else if (nsu !== HTML_NS && !kids.length) { markup += '/'; skipEnd = true; }
                markup += '>';
                out.push(markup);
                if (skipEnd) return;
                for (var k = 0; k < kids.length; k++) ser(kids[k], inherited, mp, ctr, out);
                out.push('</' + qn + '>');
                return;
            }
            case 3: out.push(esc(node.data, false)); return;
            case 4: out.push('<![CDATA[' + node.data + ']]>'); return;
            case 7: out.push('<?' + node.target + ' ' + node.data + '?>'); return;
            case 8: out.push('<!--' + node.data + '-->'); return;
            case 9: case 11: {
                var c = node.childNodes;
                for (var j = 0; j < c.length; j++) ser(c[j], ns, m, ctr, out);
                return;
            }
            case 10: {
                var d = '<!DOCTYPE ' + node.name;
                if (node.publicId) d += ' PUBLIC "' + node.publicId + '"';
                if (node.systemId && !node.publicId) d += ' SYSTEM';
                if (node.systemId) d += ' "' + node.systemId + '"';
                out.push(d + '>');
                return;
            }
            default: return;
            }
        };
        var xmlSerialize = function (node, kidsOnly) {
            var m = new Map(); m.set(XML_NS, ['xml']);
            var out = [], ctr = { i: 1 };
            if (kidsOnly) {
                var c = node.nodeType === 1 && node.namespaceURI === HTML_NS && node.localName === 'template' && node.content ? node.content.childNodes : node.childNodes;
                for (var i = 0; i < c.length; i++) ser(c[i], null, m, ctr, out);
            } else ser(node, null, m, ctr, out);
            return out.join('');
        };
        var isXmlDoc = function (n) {
            var d = n && (n.nodeType === 9 ? n : n.ownerDocument);
            try { return !!d && d.contentType !== 'text/html' && !(global.HTMLDocument && d instanceof global.HTMLDocument); } catch (e) { return false; }
        };
        var Node_ = global.Node;
        var XMLSerializer = function XMLSerializer() {
            if (!new.target) throw new TypeError("Failed to construct 'XMLSerializer': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
        };
        Object.defineProperty(XMLSerializer.prototype, 'serializeToString', { configurable: true, enumerable: true, writable: true, value: function serializeToString(root) {
            if (arguments.length < 1) throw new TypeError("Failed to execute 'serializeToString' on 'XMLSerializer': 1 argument required, but only 0 present.");
            if (!Node_ || !(root instanceof Node_)) throw new TypeError("Failed to execute 'serializeToString' on 'XMLSerializer': parameter 1 is not of type 'Node'.");
            return xmlSerialize(root, false);
        } });
        Object.defineProperty(XMLSerializer.prototype, Symbol.toStringTag, { configurable: true, value: 'XMLSerializer' });
        Object.defineProperty(global, 'XMLSerializer', { configurable: true, writable: true, value: XMLSerializer });
        var EP = global.Element && global.Element.prototype;
        ['innerHTML', 'outerHTML'].forEach(function (n) {
            var p = EP;
            while (p && !Object.getOwnPropertyDescriptor(p, n)) p = Object.getPrototypeOf(p);
            var d = p && Object.getOwnPropertyDescriptor(p, n);
            if (!d || !d.get) return;
            var g = d.get;
            Object.defineProperty(p, n, { configurable: true, enumerable: d.enumerable, set: d.set,
                get: function () { return this && this.nodeType === 1 && isXmlDoc(this) ? xmlSerialize(this, n === 'innerHTML') : g.call(this); } });
        });
    })();

    /* ---- Range.createContextualFragment (DOM Parsing), Element.streamAppendHTMLUnsafe (WICG partial updates) ---- */
    (function () {
        var HTML_NS = 'http://www.w3.org/1999/xhtml';
        var RP = global.Range && global.Range.prototype;
        var fragmentFor = function (n, html) {
            var doc = n.nodeType === 9 ? n : n.ownerDocument;
            var el = n.nodeType === 1 ? n : (n.nodeType === 3 || n.nodeType === 8 || n.nodeType === 7) ? n.parentElement : null;
            var htmlDoc = doc.contentType === 'text/html';
            if (!el || (htmlDoc && el.localName === 'html' && el.namespaceURI === HTML_NS)) el = doc.createElement('body');
            var tmp = doc.createElementNS(el.namespaceURI, el.localName);
            tmp.innerHTML = String(html);
            var src = tmp.namespaceURI === HTML_NS && tmp.localName === 'template' && tmp.content ? tmp.content : tmp;
            var frag = doc.createDocumentFragment();
            while (src.firstChild) frag.appendChild(src.firstChild);
            return frag;
        };
        if (RP && typeof RP.createContextualFragment !== 'function')
            Object.defineProperty(RP, 'createContextualFragment', { configurable: true, enumerable: true, writable: true, value: function createContextualFragment(fragment) {
                if (arguments.length < 1) throw new TypeError("Failed to execute 'createContextualFragment' on 'Range': 1 argument required, but only 0 present.");
                return fragmentFor(this.startContainer, fragment);
            } });
        /* (template.htmlFor reflects for) */
        var TP = global.HTMLTemplateElement && global.HTMLTemplateElement.prototype;
        if (TP && !('htmlFor' in TP))
            Object.defineProperty(TP, 'htmlFor', { configurable: true, enumerable: true,
                get: function () { var v = this.getAttribute('for'); return v === null ? '' : v; },
                set: function (v) { this.setAttribute('for', String(v)); } });
        /* (output (HTML 4.10.12): its type, value and defaultValue, by its value mode and default value override; a form reset puts it back) */
        var OUTP = global.HTMLOutputElement && global.HTMLOutputElement.prototype;
        if (OUTP && !Object.prototype.hasOwnProperty.call(OUTP, 'value')) {
            var OST = new WeakMap();
            var ochk = function (t) { if (!(t instanceof global.HTMLOutputElement)) throw new TypeError('Illegal invocation'); };
            var ost = function (t) { var s = OST.get(t); if (!s) OST.set(t, s = { value: false, over: '' }); return s; };
            var odef = function (k, get, set) {
                var g0 = { [k]: get }[k];
                Object.defineProperty(g0, 'name', { value: 'get ' + k });
                var d = { configurable: true, enumerable: true, get: g0 };
                if (set) { var s0 = { [k]: set }[k]; Object.defineProperty(s0, 'name', { value: 'set ' + k }); d.set = s0; }
                Object.defineProperty(OUTP, k, d);
            };
            odef('type', function () { ochk(this); return 'output'; });
            odef('value', function () { ochk(this); return this.textContent; }, function (v) {
                ochk(this);
                var s = ost(this);
                if (!s.value) { s.over = this.textContent; s.value = true; }
                this.textContent = String(v);
            });
            odef('defaultValue', function () { ochk(this); var s = ost(this); return s.value ? s.over : this.textContent; }, function (v) {
                ochk(this);
                var s = ost(this);
                if (!s.value) this.textContent = String(v); else s.over = String(v);
            });
            Object.defineProperty(global, '__cyberOutputReset', { configurable: true, value: function (o) {
                var s = OST.get(o);
                if (!s || !s.value) return;
                s.value = false;
                o.textContent = s.over;
                s.over = '';
            } });
        }
        /* (marquee: loop, a positive integer or -1; start and stop) */
        var MQP = global.HTMLMarqueeElement && global.HTMLMarqueeElement.prototype;
        if (MQP && !Object.prototype.hasOwnProperty.call(MQP, 'start')) {
            var mqchk = function (t) { if (!(t instanceof global.HTMLMarqueeElement)) throw new TypeError('Illegal invocation'); };
            ['start', 'stop'].forEach(function (k) {
                Object.defineProperty(MQP, k, { configurable: true, enumerable: true, writable: true, value: { [k]: function () { mqchk(this); } }[k] });
            });
            Object.defineProperty(MQP, 'loop', { configurable: true, enumerable: true,
                get: { get: function () {
                    mqchk(this);
                    var v = this.getAttribute('loop'), m = v === null ? null : /^[\t\n\f\r ]*([-+]?\d+)/.exec(v);
                    var n = m ? parseInt(m[1], 10) : -1;
                    return n > 0 ? n : -1;
                } }.get,
                set: { set: function (v) {
                    mqchk(this);
                    v = Math.trunc(Number(v)) | 0;
                    if (v > 0 || v === -1) this.setAttribute('loop', String(v));
                    else throw new global.DOMException('The loop value must be positive or -1.', 'IndexSizeError');
                } }.set });
        }
        if (MQP) { var mld = Object.getOwnPropertyDescriptor(MQP, 'loop'); if (mld && mld.get) { Object.defineProperty(mld.get, 'name', { value: 'get loop' }); Object.defineProperty(mld.set, 'name', { value: 'set loop' }); } }
        /* (script: async, by its force async flag (set for those not the parser's) or its attribute; referrerPolicy, an enumerated one) */
        var SCP = global.HTMLScriptElement && global.HTMLScriptElement.prototype;
        if (SCP && !Object.prototype.hasOwnProperty.call(SCP, 'async')) {
            var FA_OFF = new WeakSet();
            var scchk = function (t) { if (!(t instanceof global.HTMLScriptElement)) throw new TypeError('Illegal invocation'); };
            Object.defineProperty(SCP, 'async', { configurable: true, enumerable: true,
                get: Object.defineProperty(function () {
                    scchk(this);
                    if (this.hasAttribute('async')) return true;
                    return !FA_OFF.has(this) && !this.__cyber_parser_script;
                }, 'name', { value: 'get async' }),
                set: Object.defineProperty(function (v) {
                    scchk(this);
                    FA_OFF.add(this);
                    if (v) this.setAttribute('async', ''); else this.removeAttribute('async');
                }, 'name', { value: 'set async' }) });
        }
        if (SCP && !Object.prototype.hasOwnProperty.call(SCP, 'referrerPolicy')) {
            var RPK = ['', 'no-referrer', 'no-referrer-when-downgrade', 'same-origin', 'origin', 'strict-origin', 'origin-when-cross-origin', 'strict-origin-when-cross-origin', 'unsafe-url'];
            Object.defineProperty(SCP, 'referrerPolicy', { configurable: true, enumerable: true,
                get: Object.defineProperty(function () {
                    if (!(this instanceof global.HTMLScriptElement)) throw new TypeError('Illegal invocation');
                    var v = this.getAttribute('referrerpolicy');
                    v = v === null ? '' : v.toLowerCase();
                    return RPK.indexOf(v) >= 0 ? v : '';
                }, 'name', { value: 'get referrerPolicy' }),
                set: Object.defineProperty(function (v) {
                    if (!(this instanceof global.HTMLScriptElement)) throw new TypeError('Illegal invocation');
                    this.setAttribute('referrerpolicy', String(v));
                }, 'name', { value: 'set referrerPolicy' }) });
        }
        /* (a template's declarative shadow root attributes, reflected) */
        if (TP && !('shadowRootMode' in TP)) {
            var tchk = function (t) { if (!(t instanceof global.HTMLTemplateElement)) throw new TypeError('Illegal invocation'); };
            var tdef = function (k, get, set) {
                var g0 = { [k]: get }[k], s0 = { [k]: set }[k];
                Object.defineProperty(g0, 'name', { value: 'get ' + k });
                Object.defineProperty(s0, 'name', { value: 'set ' + k });
                Object.defineProperty(TP, k, { configurable: true, enumerable: true, get: g0, set: s0 });
            };
            var tenum = function (k, attr, kws, dflt) {
                tdef(k, function () { tchk(this); var v = this.getAttribute(attr); v = v === null ? null : v.toLowerCase(); return v !== null && kws.indexOf(v) >= 0 ? v : dflt; },
                     function (v) { tchk(this); this.setAttribute(attr, String(v)); });
            };
            var tbool = function (k, attr) {
                tdef(k, function () { tchk(this); return this.hasAttribute(attr); },
                     function (v) { tchk(this); if (v) this.setAttribute(attr, ''); else this.removeAttribute(attr); });
            };
            tenum('shadowRootMode', 'shadowrootmode', ['open', 'closed'], '');
            tenum('shadowRootSlotAssignment', 'shadowrootslotassignment', ['named', 'manual'], 'named');
            tbool('shadowRootDelegatesFocus', 'shadowrootdelegatesfocus');
            tbool('shadowRootSerializable', 'shadowrootserializable');
            tbool('shadowRootClonable', 'shadowrootclonable');
            tdef('shadowRootCustomElementRegistry', function () { tchk(this); var v = this.getAttribute('shadowrootcustomelementregistry'); return v === null ? '' : v; },
                 function (v) { tchk(this); this.setAttribute('shadowrootcustomelementregistry', String(v)); });
        }
        /* (appendHTMLUnsafe and the like: markup parsed in the element's context, or its parent's, put in place) */
        var EP0 = global.Element && global.Element.prototype;
        var ins = function (name, where) {
            if (!EP0 || typeof EP0[name] === 'function') return;
            var insf = { [name]: function (html) {
                if (!(this instanceof global.Node)) throw new TypeError('Illegal invocation');
                if (arguments.length < 1) throw new TypeError("Failed to execute '" + name + "' on 'Element': 1 argument required, but only 0 present.");
                var inner = where === 'append' || where === 'prepend';
                var ctx = inner ? this : this.parentNode;
                if (!ctx) return;
                var frag = fragmentFor(ctx.nodeType === 11 ? (this.ownerDocument || this) : ctx, html);
                if (where === 'append') this.append(frag);
                else if (where === 'prepend') this.prepend(frag);
                else if (where === 'before') this.before(frag);
                else if (where === 'after') this.after(frag);
                else this.replaceWith(frag);
            } }[name];
            Object.defineProperty(insf, 'length', { value: 1 });
            Object.defineProperty(EP0, name, { configurable: true, enumerable: true, writable: true, value: insf });
            /* (before, after, replaceWith: CharacterData's too, as ChildNode's) */
            var CDP0 = global.CharacterData && global.CharacterData.prototype;
            if (CDP0 && !inner0(where) && typeof CDP0[name] !== 'function')
                Object.defineProperty(CDP0, name, { configurable: true, enumerable: true, writable: true, value: insf });
        };
        var inner0 = function (w) { return w === 'append' || w === 'prepend'; };
        ins('appendHTMLUnsafe', 'append');
        ins('prependHTMLUnsafe', 'prepend');
        ins('beforeHTMLUnsafe', 'before');
        ins('afterHTMLUnsafe', 'after');
        ins('replaceWithHTMLUnsafe', 'replace');
        /* (a WritableStream, where the realm has none: its sink's write and close in order, through one writer) */
        if (typeof global.WritableStream !== 'function') {
            var WS = Symbol('writableStream');
            var WritableStream = function WritableStream(sink) {
                if (!new.target) throw new TypeError("Failed to construct 'WritableStream': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
                sink = sink || {};
                var st = { sink: sink, q: Promise.resolve(), locked: false, state: 'writable', err: undefined, ctrl: { error: function (e) { st.state = 'errored'; st.err = e; } } };
                Object.defineProperty(this, WS, { value: st });
                if (typeof sink.start === 'function') { var sr = sink.start.call(sink, st.ctrl); st.q = Promise.resolve(sr); }
            };
            var run = function (st, name, arg) {
                st.q = st.q.then(function () {
                    if (st.state === 'errored') throw st.err;
                    var fn = st.sink[name];
                    return typeof fn === 'function' ? fn.call(st.sink, arg, st.ctrl) : undefined;
                }).then(function () { if (name === 'close') st.state = 'closed'; }, function (e) { st.state = 'errored'; st.err = e; throw e; });
                var p = st.q;
                st.q = st.q.catch(function () { });
                return p;
            };
            var Writer = function (stream) {
                var st = stream[WS];
                if (st.locked) throw new TypeError('WritableStream is locked');
                st.locked = true;
                var closedR, closed = new Promise(function (r) { closedR = r; });
                this.write = function (chunk) { return run(st, 'write', chunk); };
                this.close = function () { var p = run(st, 'close'); p.then(closedR, function () { }); return p; };
                this.abort = function (r) { st.state = 'errored'; st.err = r; var fn = st.sink.abort; return Promise.resolve(typeof fn === 'function' ? fn.call(st.sink, r) : undefined); };
                this.releaseLock = function () { st.locked = false; };
                this.ready = Promise.resolve();
                this.closed = closed;
                Object.defineProperty(this, 'desiredSize', { get: function () { return st.state === 'writable' ? 1 : 0; } });
            };
            WritableStream.prototype.getWriter = function getWriter() { return new Writer(this); };
            Object.defineProperty(WritableStream.prototype, 'locked', { configurable: true, enumerable: true, get: function () { return !!(this[WS] && this[WS].locked); } });
            WritableStream.prototype.abort = function abort(r) { var st = this[WS]; st.state = 'errored'; st.err = r; return Promise.resolve(); };
            WritableStream.prototype.close = function close() { return run(this[WS], 'close'); };
            Object.defineProperty(WritableStream.prototype, Symbol.toStringTag, { configurable: true, value: 'WritableStream' });
            Object.defineProperty(global, 'WritableStream', { configurable: true, writable: true, value: WritableStream });
        }
        var EP = global.Element && global.Element.prototype;
        if (EP && typeof EP.streamAppendHTMLUnsafe !== 'function' && typeof global.WritableStream === 'function')
            Object.defineProperty(EP, 'streamAppendHTMLUnsafe', { configurable: true, enumerable: true, writable: true, value: function streamAppendHTMLUnsafe() {
                var el = this, buf = [];
                return new global.WritableStream({
                    write: function (c) { buf.push(String(c)); },
                    close: function () { el.appendChild(fragmentFor(el, buf.join(''))); }
                });
            } });
    })();

    /* ---- Location's protocol, host, hostname and port setters (HTML 7.2.4): the URL changed so, navigated to ---- */
    (function () {
        var loc = global.location;
        if (!loc || typeof global.URL !== 'function') return;
        var setPart = function (part, v) {
            var u;
            try { u = new global.URL(String(this.href)); } catch (e) { return; }
            v = String(v);
            if (part === 'protocol') {
                var sch = v.replace(/[\t\n\r]/g, '');
                var c = sch.indexOf(':');
                if (c >= 0) sch = sch.slice(0, c);
                if (!/^[A-Za-z][A-Za-z0-9+.\-]*$/.test(sch))
                    throw new global.DOMException("Failed to set the 'protocol' property on 'Location': '" + v + "' is an invalid protocol.", 'SyntaxError');
                var before = u.href;
                u.protocol = sch;
                if (!/^https?:$/.test(u.protocol) || u.href === before) return;
                this.href = u.href;
                return;
            }
            var was = u.href;
            u[part] = v;
            if (u.href !== was) this.href = u.href;
        };
        ['protocol', 'host', 'hostname', 'port'].forEach(function (part) {
            var o = loc, d = null;
            while (o && !(d = Object.getOwnPropertyDescriptor(o, part))) o = Object.getPrototypeOf(o);
            if (!d || !d.get || d.set || !d.configurable) return;
            Object.defineProperty(o, part, { configurable: true, enumerable: d.enumerable, get: d.get,
                set: function (v) { setPart.call(this, part, v); } });
        });
    })();

    /* ---- document.body's setter (HTML 3.1.5): a body or frameset, in place of the one there or into the root ---- */
    (function () {
        var DP = global.Document && global.Document.prototype;
        var bd = DP && Object.getOwnPropertyDescriptor(DP, 'body');
        if (!bd || !bd.get || bd.set) return;
        var XH = 'http://www.w3.org/1999/xhtml';
        Object.defineProperty(DP, 'body', { configurable: true, enumerable: true, get: bd.get, set: function (v) {
            if (v !== null && !(global.HTMLElement && v instanceof global.HTMLElement) && !(v && v.nodeType === 1 && v.namespaceURI === XH))
                throw new TypeError("Failed to set the 'body' property on 'Document': The provided value is not of type 'HTMLElement'.");
            if (v === null || v.namespaceURI !== XH || (v.localName !== 'body' && v.localName !== 'frameset'))
                throw new global.DOMException("Failed to set the 'body' property on 'Document': The new body element is of type '" + (v ? v.tagName : 'null') + "'. It must be either a 'BODY' or 'FRAMESET' element.", 'HierarchyRequestError');
            var cur = bd.get.call(this);
            if (v === cur) return;
            if (cur && cur.parentNode) { cur.parentNode.replaceChild(v, cur); return; }
            var de = this.documentElement;
            if (!de) throw new global.DOMException("Failed to set the 'body' property on 'Document': No document element exists.", 'HierarchyRequestError');
            de.appendChild(v);
        } });
    })();

    /* ---- writingSuggestions (HTML 6.8.x): the nearest writingsuggestions attribute's state, false only for "false" ---- */
    (function () {
        var HP = global.HTMLElement && global.HTMLElement.prototype;
        if (!HP || 'writingSuggestions' in HP) return;
        Object.defineProperty(HP, 'writingSuggestions', { configurable: true, enumerable: true,
            get: function writingSuggestions() {
                for (var e = this; e && e.nodeType === 1; e = e.parentElement) {
                    var v = e.getAttribute('writingsuggestions');
                    if (v !== null) return String(v).toLowerCase() === 'false' ? 'false' : 'true';
                }
                return 'true';
            },
            set: function writingSuggestions(v) { this.setAttribute('writingsuggestions', String(v)); } });
    })();

    /* ---- image maps in hit testing (HTML 4.8.14.x): an img's usemap map's area at the point ---- */
    (function () {
        var DP = global.Document && global.Document.prototype;
        var holders = [DP, global.document].filter(function (o) { return o && Object.prototype.hasOwnProperty.call(o, 'elementFromPoint'); });
        var nums = function (v) { return String(v || '').split(/[\s,;]+/).filter(Boolean).map(parseFloat).map(function (n) { return isFinite(n) ? n : 0; }); };
        var hitArea = function (area, x, y) {
            var shape = String(area.getAttribute('shape') || 'rect').toLowerCase(), c = nums(area.getAttribute('coords'));
            if (shape === 'default') return true;
            if (shape === 'circle' || shape === 'circ') { if (c.length < 3) return false; var dx = x - c[0], dy = y - c[1]; return dx * dx + dy * dy <= c[2] * c[2]; }
            if (shape === 'poly' || shape === 'polygon') {
                if (c.length < 6) return false;
                var inside = false;
                for (var i = 0, j = c.length - 2; i + 1 < c.length; j = i, i += 2) {
                    var xi = c[i], yi = c[i + 1], xj = c[j], yj = c[j + 1];
                    if ((yi > y) !== (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) inside = !inside;
                }
                return inside;
            }
            if (c.length < 4) return false;
            var x1 = Math.min(c[0], c[2]), x2 = Math.max(c[0], c[2]), y1 = Math.min(c[1], c[3]), y2 = Math.max(c[1], c[3]);
            return x >= x1 && x < x2 && y >= y1 && y < y2;
        };
        holders.forEach(function (H) {
        var d = Object.getOwnPropertyDescriptor(H, 'elementFromPoint');
        if (!d || typeof d.value !== 'function' || !d.configurable) return;
        var orig = d.value;
        Object.defineProperty(H, 'elementFromPoint', { configurable: true, enumerable: d.enumerable, writable: true, value: function elementFromPoint(x, y) {
            var el = orig.apply(this, arguments);
            if (!el || el.localName !== 'img' || el.namespaceURI !== 'http://www.w3.org/1999/xhtml') return el;
            var um = el.getAttribute('usemap');
            if (um === null || um.charAt(0) !== '#' || um.length < 2) return el;
            var name = um.slice(1), maps = this.getElementsByTagName('map'), map = null;
            for (var i = 0; i < maps.length && !map; i++) if (maps[i].getAttribute('name') === name || maps[i].id === name) map = maps[i];
            if (!map) return el;
            var r = el.getBoundingClientRect(), px = Number(x) - r.left, py = Number(y) - r.top;
            var areas = map.querySelectorAll('area');
            for (var k = 0; k < areas.length; k++) if (hitArea(areas[k], px, py)) return areas[k];
            return el;
        } });
        });
    })();

    /* ---- getClientRects of an empty inline: one empty box where it is (CSSOM View 6), as long as it is rendered ---- */
    (function () {
        var EP = global.Element && global.Element.prototype;
        var d = EP && Object.getOwnPropertyDescriptor(EP, 'getClientRects');
        if (!d || typeof d.value !== 'function') return;
        var orig = d.value;
        Object.defineProperty(EP, 'getClientRects', { configurable: true, enumerable: d.enumerable, writable: true, value: function getClientRects() {
            var l = orig.apply(this, arguments);
            if (l && l.length === 0 && this.isConnected && this.namespaceURI === 'http://www.w3.org/1999/xhtml' && !this.firstChild) {
                var cs;
                try { cs = global.getComputedStyle(this); } catch (e) { return l; }
                if (!cs || !/^inline$|^inline flow$/.test(cs.display)) return l;
                for (var a = this.parentElement; a; a = a.parentElement) {
                    var ac = global.getComputedStyle(a);
                    if (!ac || ac.display === 'none') return l;
                }
                var b = this.getBoundingClientRect();
                l[0] = new global.DOMRect(b.x, b.y, 0, b.height);
                try { Object.defineProperty(l, 'length', { value: 1 }); } catch (e) { l.length = 1; }
            }
            return l;
        } });
    })();

    /* ---- document.all (HTML 2.6.2.1): HTMLAllCollection, callable; its elements in tree order, by index, id or name ---- */
    (function () {
        if (typeof global.HTMLAllCollection === 'function') return;
        var DP = global.Document && global.Document.prototype;
        if (!DP) return;
        var HTMLAllCollection = function HTMLAllCollection() { throw new TypeError('Illegal constructor'); };
        var P = HTMLAllCollection.prototype;
        var OWN = Symbol('allOwner');
        var NAMED = /^(a|button|embed|form|frame|frameset|iframe|img|input|map|meta|object|select|textarea)$/;
        var XH = 'http://www.w3.org/1999/xhtml';
        var items = function (doc) { return Array.prototype.slice.call(doc.getElementsByTagName('*')); };
        var byName = function (doc, n) {
            n = String(n);
            if (!n) return [];
            return items(doc).filter(function (e) { return e.id === n || (e.namespaceURI === XH && NAMED.test(e.localName) && e.getAttribute('name') === n); });
        };
        var isIndex = function (k) { return typeof k === 'string' && /^(0|[1-9]\d*)$/.test(k) && +k < 4294967295; };
        var named = function (doc, n) {
            var l = byName(doc, n);
            if (!l.length) return null;
            if (l.length === 1) return l[0];
            var hc = doc.createElement('div'), r = Object.create(global.HTMLCollection ? global.HTMLCollection.prototype : Object.prototype);
            l.forEach(function (e, i) { Object.defineProperty(r, i, { value: e, enumerable: true }); });
            Object.defineProperty(r, 'length', { value: l.length });
            (void hc);
            return r;
        };
        /* (document.all is falsy, [[IsHTMLDDA]]: looked at as an object, not by truth) */
        var docOf = function (t) { var d = t === undefined || t === null ? undefined : t[OWN]; if (!d) throw new TypeError('Illegal invocation'); return d; };
        var itemImpl = function (doc, a) {
            if (a === undefined || a === null && arguments.length < 2) return null;
            var k = String(a);
            if (isIndex(k)) return items(doc)[+k] || null;
            return named(doc, k);
        };
        Object.defineProperty(P, 'length', { configurable: true, enumerable: true, get: function () { return items(docOf(this)).length; } });
        Object.defineProperty(P, 'item', { configurable: true, enumerable: true, writable: true, value: function item(n) { var d = docOf(this); return arguments.length ? itemImpl(d, n) : null; } });
        Object.defineProperty(P, 'namedItem', { configurable: true, enumerable: true, writable: true, value: function namedItem(n) {
            if (arguments.length < 1) throw new TypeError("Failed to execute 'namedItem' on 'HTMLAllCollection': 1 argument required, but only 0 present.");
            return named(docOf(this), n);
        } });
        Object.defineProperty(P, Symbol.iterator, { configurable: true, writable: true, value: function () { return items(docOf(this))[Symbol.iterator](); } });
        Object.defineProperty(P, Symbol.toStringTag, { configurable: true, value: 'HTMLAllCollection' });
        Object.defineProperty(global, 'HTMLAllCollection', { configurable: true, writable: true, value: HTMLAllCollection });
        var made = new WeakMap();
        var allOf = function (doc) {
            var a = made.get(doc);
            if (a) return a;
            /* (its legacy caller: callable, not a constructor) */
            var fn = (...args) => args.length ? itemImpl(doc, args[0]) : null;
            Object.setPrototypeOf(fn, P);
            Object.defineProperty(fn, OWN, { value: doc });
            a = new Proxy(fn, {
                get: function (t, k, r) {
                    if (k === 'length') return items(doc).length;
                    if (typeof k === 'string' && !(k in P)) {
                        if (isIndex(k)) return items(doc)[+k];
                        var v = named(doc, k);
                        if (v) return v;
                    }
                    return Reflect.get(t, k, r === a ? t : r);
                },
                has: function (t, k) {
                    if (typeof k === 'string') { if (isIndex(k)) return +k < items(doc).length; if (byName(doc, k).length) return true; }
                    return k in t;
                },
                getOwnPropertyDescriptor: function (t, k) {
                    if (typeof k === 'string' && isIndex(k)) { var e = items(doc)[+k]; return e ? { value: e, writable: false, enumerable: true, configurable: true } : undefined; }
                    return Reflect.getOwnPropertyDescriptor(t, k);
                },
                ownKeys: function (t) { return items(doc).map(function (_, i) { return String(i); }).concat(Reflect.ownKeys(t)); }
            });
            if (typeof global.__cyberMarkHTMLDDA === 'function') global.__cyberMarkHTMLDDA(a);
            made.set(doc, a);
            return a;
        };
        Object.defineProperty(DP, 'all', { configurable: true, enumerable: true, get: function all() {
            if (!(this instanceof global.Document)) throw new TypeError('Illegal invocation');
            return allOf(this);
        } });
        try {
            if (Object.prototype.hasOwnProperty.call(global.document, 'all')) delete global.document.all;
            var dp0 = Object.getPrototypeOf(global.document);
            if (dp0 && dp0 !== DP && Object.prototype.hasOwnProperty.call(dp0, 'all')) delete dp0.all;
        } catch (e) { }
        if (typeof global.__cyberMarkHTMLDDA !== 'function') delete DP.all;   /* (no way to make it falsy: the old one stays) */
    })();

    /* ---- Window's small members (HTML 7.2.2): stop(), status, external, origin ---- */
    (function () {
        if (!global.document) return;
        if (!('stop' in global)) Object.defineProperty(global, 'stop', { configurable: true, enumerable: true, writable: true, value: function stop() { if (typeof global.__cyberStopFrames === 'function') global.__cyberStopFrames(); } });
        if (!('status' in global)) {
            var st = '';
            Object.defineProperty(global, 'status', { configurable: true, enumerable: true, get: function status() { return st; }, set: function status(v) { st = String(v); } });
        }
        if (!('external' in global)) {
            var External = function External() { throw new TypeError('Illegal constructor'); };
            var extOf = function (t) { if (!(t instanceof External)) throw new TypeError('Illegal invocation'); };
            Object.defineProperty(External, 'prototype', { writable: false });
            Object.defineProperty(External.prototype, 'AddSearchProvider', { configurable: true, enumerable: true, writable: true, value: function AddSearchProvider() { extOf(this); } });
            Object.defineProperty(External.prototype, 'IsSearchProviderInstalled', { configurable: true, enumerable: true, writable: true, value: function IsSearchProviderInstalled() { extOf(this); } });
            Object.defineProperty(External.prototype, Symbol.toStringTag, { configurable: true, value: 'External' });
            Object.defineProperty(global, 'External', { configurable: true, writable: true, value: External });
            var ext = Object.create(External.prototype);
            Object.defineProperty(global, 'external', { configurable: true, enumerable: true, get: function external() { return ext; }, set: function (v) { Object.defineProperty(global, 'external', { value: v, writable: true, configurable: true, enumerable: true }); } });
        }
        if (!('origin' in global)) {
            Object.defineProperty(global, 'origin', { configurable: true, enumerable: true,
                get: function origin() {
                    try {
                        if (global.document && global.document.origin) return global.document.origin;
                        var o = global.location.origin;
                        /* (an about:blank or srcdoc document: its creator's origin, as its base URL comes from it; not a sandboxed one's) */
                        if ((!o || o === 'null') && global.location.protocol === 'about:' && global.document && global.document.baseURI &&
                            !/^about:/.test(global.document.baseURI) && !(global.frameElement && global.frameElement.hasAttribute && global.frameElement.hasAttribute('sandbox'))) {
                            try { o = new global.URL(global.document.baseURI).origin; } catch (e) { }
                        }
                        return o && o !== 'null' ? o : 'null';
                    } catch (e) { return 'null'; }
                },
                set: function (v) { Object.defineProperty(global, 'origin', { value: v, writable: true, configurable: true, enumerable: true }); } });
        }
    })();

    /* ---- focusgroup (Open UI scoped focusgroup, tentative): arrow keys move focus among a group's items ---- */
    (function () {
        if (!global.document) return;
        var BEHAVIORS = { toolbar: 1, tablist: 1, radiogroup: 1, listbox: 1, menu: 1, menubar: 1 };
        var tokens = function (el) {
            var v = el.getAttribute && el.getAttribute('focusgroup');
            if (v === null || v === undefined) return null;
            return String(v).toLowerCase().split(/[\t\n\f\r ]+/).filter(Boolean);
        };
        var isOwner = function (el) { var t = tokens(el); return !!(t && t.length && BEHAVIORS[t[0]]); };
        var optedOut = function (el) { var t = tokens(el); return !!(t && t[0] === 'none'); };
        var focusable = function (el) {
            if (!el || el.nodeType !== 1) return false;
            if (el.hasAttribute('disabled') && /^(button|input|select|textarea)$/.test(el.localName)) return false;
            var ti = el.getAttribute('tabindex');
            if (ti !== null && !isNaN(parseInt(ti, 10))) return true;
            if (/^(button|select|textarea|iframe)$/.test(el.localName)) return true;
            if (el.localName === 'input') return String(el.getAttribute('type') || '').toLowerCase() !== 'hidden';
            if ((el.localName === 'a' || el.localName === 'area') && el.hasAttribute('href')) return true;
            return false;
        };
        var ownerOf = function (el) {
            for (var a = el.parentElement; a; a = a.parentElement) {
                if (optedOut(a)) return null;
                if (isOwner(a)) return a;
            }
            return null;
        };
        var itemsOf = function (owner) {
            var out = [];
            (function walk(n) {
                for (var c = n.firstElementChild; c; c = c.nextElementSibling) {
                    if (optedOut(c)) continue;
                    var rendered = true;
                    try { var cs = global.getComputedStyle(c); rendered = cs.display !== 'none' && cs.visibility !== 'hidden'; } catch (e) { }
                    if (!rendered) continue;
                    if (focusable(c)) out.push(c);
                    if (isOwner(c)) continue;   /* (a nested group: an item itself, its items its own) */
                    walk(c);
                }
            })(owner);
            return out;
        };
        onWin('keydown', function (e) {
            if (e.defaultPrevented || e.altKey || e.ctrlKey || e.metaKey) return;
            var k = e.key;
            if (!/^(ArrowLeft|ArrowRight|ArrowUp|ArrowDown|Home|End)$/.test(k)) return;
            var d = global.document, cur = d.activeElement;
            if (!cur || cur === d.body) return;
            var owner = ownerOf(cur);
            if (!owner) return;
            var t = tokens(owner), inline = t.indexOf('inline') >= 0, block = t.indexOf('block') >= 0;
            if (!inline && !block) inline = block = true;
            var items = itemsOf(owner), i = items.indexOf(cur);
            if (i < 0 || !items.length) return;
            var rtl = false;
            try { rtl = global.getComputedStyle(owner).direction === 'rtl'; } catch (e2) { }
            var step = 0;
            if (k === 'ArrowRight' && inline) step = rtl ? -1 : 1;
            else if (k === 'ArrowLeft' && inline) step = rtl ? 1 : -1;
            else if (k === 'ArrowDown' && block) step = 1;
            else if (k === 'ArrowUp' && block) step = -1;
            var to = -1;
            if (k === 'Home') to = 0;
            else if (k === 'End') to = items.length - 1;
            else if (step) {
                to = i + step;
                if (to < 0 || to >= items.length) to = t.indexOf('wrap') >= 0 ? (to + items.length) % items.length : -1;
            }
            if (to < 0 || to === i) return;
            e.preventDefault();
            try { items[to].focus(); } catch (e3) { }
        });
    })();

    /* ---- HTML members the interfaces lacked (HTML's IDL): reflections, text, areas, media odds and ends ---- */
    (function () {
        var def = function (C, name, d) {
            var P = global[C] && global[C].prototype;
            if (!P || Object.getOwnPropertyDescriptor(P, name)) return;
            d.configurable = true; d.enumerable = true;
            Object.defineProperty(P, name, d);
        };
        var str = function (attr) { return { get: function () { var v = this.getAttribute(attr); return v === null ? '' : v; }, set: function (v) { this.setAttribute(attr, String(v)); } }; };
        var enm = function (attr, kw, missing, invalid) {
            return { get: function () {
                var v = this.getAttribute(attr);
                if (v === null) return missing;
                v = v.toLowerCase();
                return kw.indexOf(v) >= 0 ? v : invalid;
            }, set: function (v) { this.setAttribute(attr, String(v)); } };
        };
        var ulong = function (attr, dflt) {
            return { get: function () {
                var m = /^[\t\n\f\r ]*\+?(\d+)/.exec(this.getAttribute(attr) || '');
                var n = m ? parseInt(m[1], 10) : NaN;
                return isFinite(n) && n >= 0 && n <= 2147483647 ? n : dflt;
            }, set: function (v) { var n = Number(v) >>> 0; this.setAttribute(attr, String(n > 2147483647 ? dflt : n)); } };
        };
        def('HTMLImageElement', 'sizes', str('sizes'));
        def('HTMLImageElement', 'loading', enm('loading', ['eager', 'lazy'], 'eager', 'eager'));
        def('HTMLImageElement', 'fetchPriority', enm('fetchpriority', ['high', 'low', 'auto'], 'auto', 'auto'));
        def('HTMLIFrameElement', 'allow', str('allow'));
        def('HTMLIFrameElement', 'loading', enm('loading', ['eager', 'lazy'], 'eager', 'eager'));
        def('HTMLLinkElement', 'fetchPriority', enm('fetchpriority', ['high', 'low', 'auto'], 'auto', 'auto'));
        def('HTMLLinkElement', 'imageSizes', str('imagesizes'));
        def('HTMLLinkElement', 'imageSrcset', str('imagesrcset'));
        def('HTMLScriptElement', 'fetchPriority', enm('fetchpriority', ['high', 'low', 'auto'], 'auto', 'auto'));
        def('HTMLSourceElement', 'width', ulong('width', 0));
        def('HTMLSourceElement', 'height', ulong('height', 0));
        def('HTMLFormElement', 'rel', str('rel'));
        def('HTMLElement', 'autocapitalize', { get: function () {
            var v = (this.getAttribute('autocapitalize') || '').toLowerCase();
            if (v === 'off' || v === 'none') return 'none';
            if (v === 'on' || v === 'sentences') return 'sentences';
            if (v === 'words' || v === 'characters') return v;
            return '';
        }, set: function (v) { this.setAttribute('autocapitalize', String(v)); } });
        def('HTMLElement', 'autocorrect', { get: function () { var v = this.getAttribute('autocorrect'); return !(v !== null && v.toLowerCase() === 'off'); },
            set: function (v) { this.setAttribute('autocorrect', v ? 'on' : 'off'); } });
        def('HTMLElement', 'spellcheck', { get: function () {
            for (var e = this; e && e.nodeType === 1; e = e.parentElement) {
                var v = e.getAttribute('spellcheck');
                if (v !== null) { v = v.toLowerCase(); if (v === '' || v === 'true') return true; if (v === 'false') return false; }
            }
            return true;
        }, set: function (v) { this.setAttribute('spellcheck', v ? 'true' : 'false'); } });
        def('HTMLElement', 'accessKeyLabel', { get: function () { return ''; } });
        def('HTMLAnchorElement', 'text', { get: function () { return this.textContent; }, set: function (v) { this.textContent = String(v); } });
        def('HTMLTitleElement', 'text', { get: function () {
            var t = '';
            for (var c = this.firstChild; c; c = c.nextSibling) if (c.nodeType === 3) t += c.data;
            return t;
        }, set: function (v) { this.textContent = String(v); } });
        var areasOf = new WeakMap();
        def('HTMLMapElement', 'areas', { get: function () {
            var self = this, c = areasOf.get(this);
            if (!c) {
                c = self.getElementsByTagName('area');
                areasOf.set(this, c);
            }
            return c;
        } });
        def('HTMLMediaElement', 'srcObject', { get: function () { return this.__cyberSrcObject || null; },
            set: function (v) { Object.defineProperty(this, '__cyberSrcObject', { value: v === undefined ? null : v, writable: true, configurable: true }); } });
        var MP = global.HTMLMediaElement && global.HTMLMediaElement.prototype;
        if (MP && typeof MP.fastSeek !== 'function') Object.defineProperty(MP, 'fastSeek', { configurable: true, enumerable: true, writable: true, value: function fastSeek(time) {
            if (arguments.length < 1) throw new TypeError("Failed to execute 'fastSeek' on 'HTMLMediaElement': 1 argument required, but only 0 present.");
            var t = Number(time);
            if (!isFinite(t)) throw new TypeError("Failed to execute 'fastSeek' on 'HTMLMediaElement': The provided double value is non-finite.");
            this.currentTime = t;
        } });
        if (MP && typeof MP.getStartDate !== 'function') Object.defineProperty(MP, 'getStartDate', { configurable: true, enumerable: true, writable: true, value: function getStartDate() { return new Date(NaN); } });
        ['HTMLEmbedElement', 'HTMLIFrameElement', 'HTMLObjectElement'].forEach(function (C) {
            var P = global[C] && global[C].prototype;
            if (!P || typeof P.getSVGDocument === 'function') return;
            Object.defineProperty(P, 'getSVGDocument', { configurable: true, enumerable: true, writable: true, value: function getSVGDocument() {
                var d = null;
                try { d = this.contentDocument || null; } catch (e) { d = null; }
                return d && d.documentElement && d.documentElement.namespaceURI === 'http://www.w3.org/2000/svg' ? d : null;
            } });
        });
    })();

    /* ---- document.queryCommand* (HTML 8.x, editing): nothing enabled, nothing on ---- */
    (function () {
        var DP = global.Document && global.Document.prototype;
        if (!DP) return;
        var SUPPORTED = /^(bold|italic|underline|strikethrough|subscript|superscript|createlink|unlink|delete|forwarddelete|inserttext|inserthtml|insertparagraph|insertlinebreak|insertorderedlist|insertunorderedlist|indent|outdent|justifyleft|justifyright|justifycenter|justifyfull|removeformat|selectall|undo|redo|copy|cut|paste|forecolor|backcolor|hilitecolor|fontname|fontsize|formatblock|inserthorizontalrule|insertimage|stylewithcss|defaultparagraphseparator|contentreadonly|usecss)$/;
        var op = function (name, impl) {
            if (typeof DP[name] === 'function') return;
            var fn = function (cmd) {
                if (arguments.length < 1) throw new TypeError("Failed to execute '" + name + "' on 'Document': 1 argument required, but only 0 present.");
                if (!(this instanceof global.Document)) throw new TypeError('Illegal invocation');
                return impl(String(cmd).toLowerCase());
            };
            Object.defineProperty(fn, 'name', { value: name });
            Object.defineProperty(DP, name, { configurable: true, enumerable: true, writable: true, value: fn });
        };
        op('queryCommandEnabled', function () { return false; });
        op('queryCommandIndeterm', function () { return false; });
        op('queryCommandState', function () { return false; });
        op('queryCommandSupported', function (c) { return SUPPORTED.test(c); });
        op('queryCommandValue', function () { return ''; });
    })();

    /* ---- a form's named properties (HTML 4.10.3): its listed elements by name or id, where nothing else is named so ---- */
    /* (after the IDL shaping, which sets each prototype's parent: idl_shape.c calls this) */
    /* (focusreset: the focused element removed, the event at its document from a microtask, HTML PR 13050) */
    (function () {
        var DPf = global.Document && global.Document.prototype;
        if (!DPf || Object.getOwnPropertyDescriptor(DPf, 'onfocusreset')) return;
        var FR = Symbol('onfocusreset'), FRW = Symbol('onfocusresetW');
        var g = function () { if (!(this instanceof global.Document)) throw new TypeError('Illegal invocation'); return this[FR] || null; };
        var s = function (f) {
            if (!(this instanceof global.Document)) throw new TypeError('Illegal invocation');
            if (this[FRW]) this.removeEventListener('focusreset', this[FRW]);
            if (typeof f !== 'function' && (typeof f !== 'object' || f === null)) { this[FR] = null; this[FRW] = null; return; }
            var self = this, w = function (e) { return typeof f === 'function' ? f.call(self, e) : undefined; };
            this[FR] = f; this[FRW] = w;
            this.addEventListener('focusreset', w);
        };
        Object.defineProperty(g, 'name', { value: 'get onfocusreset' });
        Object.defineProperty(s, 'name', { value: 'set onfocusreset' });
        Object.defineProperty(DPf, 'onfocusreset', { get: g, set: s, enumerable: true, configurable: true });
        Object.defineProperty(global, '__cyberFocusReset', { configurable: true, value: function (doc) {
            /* (the viewport focused now; a focus update before the microtask runs: no event) */
            try { if (typeof global.__cyberDocSet === 'function') global.__cyberDocSet(doc, 'activeElement', doc.body); } catch (e) { }
            var gen = global.__cyberFocusGen | 0;
            global.queueMicrotask(function () {
                if ((global.__cyberFocusGen | 0) !== gen) return;
                var ev = new global.Event('focusreset');
                if (typeof global.__cyberDispatchTrusted === 'function') global.__cyberDispatchTrusted(doc, ev); else doc.dispatchEvent(ev);
            });
        } });
    })();
    Object.defineProperty(global, '__cyberAfterShape', { configurable: true, value: function () {
        try { mediaPolicy(); } catch (e) { }
        /* (HTMLElement's own members the engine put higher up: its own, as the IDL has them) */
        try {
            [['HTMLElement', ['translate', 'draggable', 'isContentEditable', 'dataset', 'tabIndex', 'focus', 'blur', 'nonce', 'autofocus']],
             ['SVGElement', ['dataset', 'tabIndex', 'focus', 'blur', 'nonce', 'autofocus']],
             ['MathMLElement', ['dataset', 'tabIndex', 'focus', 'blur', 'nonce', 'autofocus']]].forEach(function (ent) {
            var HEo = global[ent[0]] && global[ent[0]].prototype;
            if (HEo) ent[1].forEach(function (k) {
                for (var p = HEo; p; p = Object.getPrototypeOf(p)) {
                    var d = Object.getOwnPropertyDescriptor(p, k);
                    if (d) {
                        /* (enumerable, its accessors named as WebIDL names them) */
                        d.enumerable = true;
                        try {
                            if (d.get && (d.get.name === 'get' || d.get.name === '')) Object.defineProperty(d.get, 'name', { value: 'get ' + k });
                            if (d.set && (d.set.name === 'set' || d.set.name === '')) Object.defineProperty(d.set, 'name', { value: 'set ' + k });
                        } catch (e2) { }
                        if (d.configurable !== false) Object.defineProperty(HEo, k, d);
                        break;
                    }
                }
            });
            });
        } catch (e) { }
        /* (the media element's members wrapped on the video prototype: up on HTMLMediaElement's, an audio element's too) */
        try {
            var MEp = global.HTMLMediaElement && global.HTMLMediaElement.prototype, VEp = global.HTMLVideoElement && global.HTMLVideoElement.prototype;
            var AEp = global.HTMLAudioElement && global.HTMLAudioElement.prototype;
            if (MEp && VEp && MEp !== VEp) ['buffered', 'played', 'seekable', 'error', 'textTracks', 'audioTracks', 'videoTracks', 'addTextTrack', 'volume', 'muted', 'preservesPitch'].forEach(function (k) {
                var dv = Object.getOwnPropertyDescriptor(VEp, k);
                if (!dv || !dv.configurable) return;
                Object.defineProperty(MEp, k, dv);   /* (kept on the video prototype too: deleting from a prototype here was suspect in a GC crash) */
                /* (an audio prototype with its own: the same, wrapped one) */
                var da = AEp && AEp !== MEp && Object.getOwnPropertyDescriptor(AEp, k);
                if (da && da.configurable) Object.defineProperty(AEp, k, dv);
            });
            /* (focus and blur: their IDL names and lengths) */
            var HEf = global.HTMLElement && global.HTMLElement.prototype;
            [['focus', 0], ['blur', 0]].forEach(function (p) {
                var d = HEf && Object.getOwnPropertyDescriptor(HEf, p[0]);
                if (!d || typeof d.value !== 'function') return;
                try { Object.defineProperty(d.value, 'name', { value: p[0] }); Object.defineProperty(d.value, 'length', { value: p[1] }); } catch (e2) { }
            });
        } catch (e) { }
        /* (progress: labels as meter has them; datalist: its options) */
        try {
            var INp = global.HTMLInputElement && global.HTMLInputElement.prototype;
            var ldi = INp && Object.getOwnPropertyDescriptor(INp, 'labels');
            if (ldi && ldi.get) ['HTMLProgressElement', 'HTMLMeterElement'].forEach(function (nm) {
                var P = global[nm] && global[nm].prototype;
                if (!P || Object.getOwnPropertyDescriptor(P, 'labels')) return;
                var C = global[nm], g0 = ldi.get;
                var g = function () { if (!(this instanceof C)) throw new TypeError('Illegal invocation'); return g0.call(this); };
                Object.defineProperty(g, 'name', { value: 'get labels' });
                Object.defineProperty(P, 'labels', { get: g, enumerable: true, configurable: true });
            });
            var DLp = global.HTMLDataListElement && global.HTMLDataListElement.prototype;
            if (DLp && !Object.getOwnPropertyDescriptor(DLp, 'options')) {
                var dlOpts = function options() {
                    if (!(this instanceof global.HTMLDataListElement)) throw new TypeError('Illegal invocation');
                    if (!this.__cyberDlOpts) {
                        var self = this;
                        var hc = self.getElementsByTagName('option');
                        Object.defineProperty(self, '__cyberDlOpts', { value: hc });
                    }
                    return this.__cyberDlOpts;
                };
                Object.defineProperty(dlOpts, 'name', { value: 'get options' });
                Object.defineProperty(DLp, 'options', { get: dlOpts, enumerable: true, configurable: true });
            }
        } catch (e) { }
        /* (operations made in script on element interfaces: this one of theirs, their required arguments) */
        try {
            [['HTMLSelectElement', { item: 1, namedItem: 1, add: 1, remove: 0, showPicker: 0, checkValidity: 0, reportValidity: 0, setCustomValidity: 1 }],
             ['HTMLCanvasElement', { transferControlToOffscreen: 0 }],
             ['HTMLSlotElement', { assignedNodes: 0, assignedElements: 0, assign: 0 }],
             ['HTMLIFrameElement', { getSVGDocument: 0 }], ['HTMLEmbedElement', { getSVGDocument: 0 }], ['HTMLObjectElement', { getSVGDocument: 0 }],
             ['XMLSerializer', { serializeToString: 1 }], ['ImageBitmap', { close: 0 }], ['OffscreenCanvas', { transferToImageBitmap: 0 }],
             ['DataTransfer', { setDragImage: 3 }], ['HTMLMediaElement', { fastSeek: 1, getStartDate: 0 }]].forEach(function (e) {
                var C = global[e[0]];
                if (typeof C !== 'function' || !C.prototype) return;
                Object.keys(e[1]).forEach(function (k) {
                    var d = Object.getOwnPropertyDescriptor(C.prototype, k);
                    if (!d || typeof d.value !== 'function' || d.value.__cyberOp || !d.configurable) return;
                    var f = d.value, need = e[1][k];
                    var w = { [k]: function () {
                        if (!(this instanceof C)) throw new TypeError("Failed to execute '" + k + "' on '" + e[0] + "': Illegal invocation");
                        if (arguments.length < need) throw new TypeError("Failed to execute '" + k + "' on '" + e[0] + "': " + need + ' argument required, but only ' + arguments.length + ' present.');
                        return f.apply(this, arguments);
                    } }[k];
                    Object.defineProperty(w, 'length', { value: need });
                    Object.defineProperty(w, '__cyberOp', { value: true });
                    Object.defineProperty(C.prototype, k, { value: w, writable: true, enumerable: true, configurable: true });
                });
            });
        } catch (e) { }
        /* (the registry: an instance of CustomElementRegistry, its operations on the prototype) */
        try {
            var reg = global.customElements, CER = global.CustomElementRegistry;
            if (reg && typeof reg === 'object' && typeof CER === 'function' && CER.prototype && !(reg instanceof CER)) {
                ['define', 'get', 'getName', 'whenDefined', 'upgrade', 'initialize'].forEach(function (k) {
                    var own = Object.getOwnPropertyDescriptor(reg, k);
                    if (!own || typeof own.value !== 'function' || Object.getOwnPropertyDescriptor(CER.prototype, k)) return;
                    var f = own.value;
                    var w = { [k]: function () { if (!(this instanceof CER)) throw new TypeError("Failed to execute '" + k + "' on 'CustomElementRegistry': Illegal invocation"); return f.apply(this, arguments); } }[k];
                    Object.defineProperty(w, 'length', { value: f.length });
                    Object.defineProperty(CER.prototype, k, { value: w, writable: true, enumerable: true, configurable: true });
                });
                var rp = Object.getPrototypeOf(reg);
                if (rp === Object.prototype || rp === null) {
                    Object.setPrototypeOf(reg, CER.prototype);
                    ['define', 'get', 'getName', 'whenDefined', 'upgrade', 'initialize'].forEach(function (k) {
                        var own = Object.getOwnPropertyDescriptor(reg, k);
                        if (own && own.configurable && Object.getOwnPropertyDescriptor(CER.prototype, k)) delete reg[k];
                    });
                }
            }
        } catch (e) { }
        /* (fieldset elements, legend form, details name) */
        try {
            var FSp = global.HTMLFieldSetElement && global.HTMLFieldSetElement.prototype;
            if (FSp && !Object.getOwnPropertyDescriptor(FSp, 'elements')) {
                var LISTED = { button: 1, fieldset: 1, input: 1, object: 1, output: 1, select: 1, textarea: 1 };
                var fse = function () {
                    if (!(this instanceof global.HTMLFieldSetElement)) throw new TypeError('Illegal invocation');
                    var out = [];
                    this.querySelectorAll('*').forEach(function (e) { if (LISTED[e.localName] && e.namespaceURI === 'http://www.w3.org/1999/xhtml') out.push(e); });
                    if (global.HTMLCollection) Object.setPrototypeOf(out, global.HTMLCollection.prototype);
                    return out;
                };
                Object.defineProperty(fse, 'name', { value: 'get elements' });
                Object.defineProperty(FSp, 'elements', { get: fse, enumerable: true, configurable: true });
            }
            var LGp = global.HTMLLegendElement && global.HTMLLegendElement.prototype;
            if (LGp && !Object.getOwnPropertyDescriptor(LGp, 'form')) {
                var lgf = function () {
                    if (!(this instanceof global.HTMLLegendElement)) throw new TypeError('Illegal invocation');
                    var p = this.parentNode;
                    return p && p.localName === 'fieldset' && p.namespaceURI === 'http://www.w3.org/1999/xhtml' ? p.form : null;
                };
                Object.defineProperty(lgf, 'name', { value: 'get form' });
                Object.defineProperty(LGp, 'form', { get: lgf, enumerable: true, configurable: true });
            }
            var DTp = global.HTMLDetailsElement && global.HTMLDetailsElement.prototype;
            if (DTp && !Object.getOwnPropertyDescriptor(DTp, 'name') && !('name' in DTp)) {
                var dg = function () { if (!(this instanceof global.HTMLDetailsElement)) throw new TypeError('Illegal invocation'); var v = this.getAttribute('name'); return v === null ? '' : v; };
                var ds = function (v) { if (!(this instanceof global.HTMLDetailsElement)) throw new TypeError('Illegal invocation'); this.setAttribute('name', String(v)); };
                Object.defineProperty(dg, 'name', { value: 'get name' });
                Object.defineProperty(ds, 'name', { value: 'set name' });
                Object.defineProperty(DTp, 'name', { get: dg, set: ds, enumerable: true, configurable: true });
            }
        } catch (e) { }
        /* (OffscreenCanvas: its context loss handlers) */
        try {
            var OCp = global.OffscreenCanvas && global.OffscreenCanvas.prototype;
            if (OCp) ['contextlost', 'contextrestored'].forEach(function (t) {
                if (Object.getOwnPropertyDescriptor(OCp, 'on' + t)) return;
                var slot = Symbol('on' + t), OC = global.OffscreenCanvas;
                var g = function () { if (!(this instanceof OC)) throw new TypeError('Illegal invocation'); return this[slot] || null; };
                var s = function (f) {
                    if (!(this instanceof OC)) throw new TypeError('Illegal invocation');
                    if (this[slot]) this.removeEventListener(t, this[slot]);
                    this[slot] = typeof f === 'function' ? f : null;
                    if (this[slot]) this.addEventListener(t, this[slot]);
                };
                Object.defineProperty(g, 'name', { value: 'get on' + t });
                Object.defineProperty(s, 'name', { value: 'set on' + t });
                Object.defineProperty(OCp, 'on' + t, { get: g, set: s, enumerable: true, configurable: true });
            });
        } catch (e) { }
        /* (the legacy factory functions Image, Audio, Option: new only, length 0, the element interface's prototype) */
        try {
            [['Image', 'HTMLImageElement'], ['Audio', 'HTMLAudioElement'], ['Option', 'HTMLOptionElement']].forEach(function (p) {
                var orig = global[p[0]], E = global[p[1]];
                if (typeof orig !== 'function' || typeof E !== 'function' || orig.__cyberFactory) return;
                var N = { [p[0]]: function () {
                    if (!new.target) throw new TypeError("Failed to construct '" + p[0] + "': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
                    var el = Reflect.construct(orig, arguments);
                    if (new.target !== N && new.target.prototype && typeof new.target.prototype === 'object') Object.setPrototypeOf(el, new.target.prototype);
                    return el;
                } }[p[0]];
                Object.defineProperty(N, 'length', { value: 0 });
                Object.defineProperty(N, 'prototype', { value: E.prototype, writable: false, enumerable: false, configurable: false });
                Object.defineProperty(N, '__cyberFactory', { value: true });
                Object.defineProperty(global, p[0], { value: N, writable: true, enumerable: false, configurable: true });
            });
        } catch (e) { }
        /* (SVGElement's HTMLOrSVGElement members, its own; SVGSVGElement's WindowEventHandlers, as body's) */
        try {
            var SEp = global.SVGElement && global.SVGElement.prototype, HEp = global.HTMLElement && global.HTMLElement.prototype;
            /* (MathMLElement's the same) */
            var MEp = global.MathMLElement && global.MathMLElement.prototype;
            [SEp, MEp].forEach(function (XP) {
                if (XP && HEp) ['dataset', 'nonce', 'autofocus', 'tabIndex', 'focus', 'blur'].forEach(function (k) {
                    if (Object.prototype.hasOwnProperty.call(XP, k)) return;
                    var d = Object.getOwnPropertyDescriptor(HEp, k);
                    if (d) Object.defineProperty(XP, k, d);
                });
            });
            /* (MathML's <a>: MathMLAnchorElement, its href's setter and target reflecting; HyperlinkElementUtils' URL parts, url_classes.c) */
            if (MEp && typeof global.MathMLAnchorElement !== 'function') {
                var MAE = function MathMLAnchorElement() { throw new TypeError('Illegal constructor'); };
                Object.defineProperty(MAE, 'prototype', { value: Object.create(MEp), writable: false });
                Object.defineProperty(MAE.prototype, 'constructor', { value: MAE, writable: true, configurable: true });
                Object.defineProperty(MAE.prototype, Symbol.toStringTag, { value: 'MathMLAnchorElement', configurable: true });
                Object.setPrototypeOf(MAE, global.MathMLElement);
                var mchk = function (t) { if (!(t instanceof MAE)) throw new TypeError('Illegal invocation'); };
                Object.defineProperty(MAE.prototype, 'target', { configurable: true, enumerable: true,
                    get: { get: function target() { mchk(this); var v = this.getAttribute('target'); return v === null ? '' : v; } }.get,
                    set: { set: function target(v) { mchk(this); this.setAttribute('target', String(v)); } }.set });
                Object.defineProperty(global, 'MathMLAnchorElement', { value: MAE, writable: true, configurable: true });
                var HAp = global.HTMLAnchorElement && global.HTMLAnchorElement.prototype;
                if (HAp) ['href', 'origin', 'protocol', 'username', 'password', 'host', 'hostname', 'port', 'pathname', 'search', 'hash'].forEach(function (k) {
                    var d = Object.getOwnPropertyDescriptor(HAp, k);
                    if (d) Object.defineProperty(MAE.prototype, k, d);
                });
            }
            /* (HyperlinkElementUtils' hreflang and type, reflected, on SVG's and MathML's <a> too) */
            [global.SVGAElement, global.MathMLAnchorElement].forEach(function (C) {
                if (!C || !C.prototype) return;
                ['hreflang', 'type'].forEach(function (k) {
                    if (Object.prototype.hasOwnProperty.call(C.prototype, k)) return;
                    var g0 = { [k]: function () { if (!(this instanceof C)) throw new TypeError('Illegal invocation'); var v = this.getAttribute(k); return v === null ? '' : v; } }[k];
                    var s0 = { [k]: function (v) { if (!(this instanceof C)) throw new TypeError('Illegal invocation'); this.setAttribute(k, String(v)); } }[k];
                    Object.defineProperty(g0, 'name', { value: 'get ' + k });
                    Object.defineProperty(s0, 'name', { value: 'set ' + k });
                    Object.defineProperty(C.prototype, k, { configurable: true, enumerable: true, get: g0, set: s0 });
                });
            });
            var SSp = global.SVGSVGElement && global.SVGSVGElement.prototype, BOp = global.HTMLBodyElement && global.HTMLBodyElement.prototype;
            if (SSp && BOp) ['onafterprint', 'onbeforeprint', 'onbeforeunload', 'onhashchange', 'onlanguagechange', 'onmessage', 'onmessageerror',
                'onoffline', 'ononline', 'onpagehide', 'onpagereveal', 'onpageshow', 'onpageswap', 'onpopstate', 'onrejectionhandled', 'onstorage',
                'onunhandledrejection', 'onunload'].forEach(function (k) {
                if (Object.prototype.hasOwnProperty.call(SSp, k)) return;
                var d = Object.getOwnPropertyDescriptor(BOp, k);
                if (d) Object.defineProperty(SSp, k, d);
            });
        } catch (e) { }
        /* (Navigator: NavigatorID's legacy members; languages a frozen array, the same each time) */
        try {
            var NV = global.Navigator, NP = NV && NV.prototype;
            if (NP) {
                var nvOp = function (k, v) {
                    if (Object.prototype.hasOwnProperty.call(NP, k)) return;
                    Object.defineProperty(NP, k, { writable: true, enumerable: true, configurable: true, value: Object.defineProperty({ m: function () {
                        if (!(this instanceof NV)) throw new TypeError('Illegal invocation');
                        return v;
                    } }.m, 'name', { value: k }) });
                };
                nvOp('javaEnabled', false);
                nvOp('taintEnabled', false);
                var ld = Object.getOwnPropertyDescriptor(NP, 'languages');
                if (ld && ld.get) {
                    var lg = ld.get, frozen = null, last = '';
                    Object.defineProperty(NP, 'languages', { enumerable: true, configurable: true, get: Object.defineProperty({ get: function () {
                        var v = lg.call(this);
                        if (!Array.isArray(v) || Object.isFrozen(v)) return v;
                        var key = v.join(',');
                        if (!frozen || key !== last) { frozen = Object.freeze(v.slice()); last = key; }
                        return frozen;
                    } }.get, 'name', { value: 'get languages' }) });
                }
            }
        } catch (e) { }
        /* (Window's members on the global: operations that check their this, attributes as accessors over what the engine set) */
        try {
            if (global.document && typeof global.Window === 'function') {
                var isWin = function (t) { return t === undefined || t === null || t === global || (t && typeof t === 'object' && t.window === t && t.self === t); };
                var WOPS = { close: 0, stop: 0, focus: 0, blur: 0, open: 0, alert: 0, confirm: 0, prompt: 0, print: 0 };   /* (not postMessage: a wrapper here would be the incumbent, the message's source) */
                Object.keys(WOPS).forEach(function (k) {
                    var d = Object.getOwnPropertyDescriptor(global, k);
                    if (!d || typeof d.value !== 'function' || !d.configurable || d.value.__cyberWinChecked) return;
                    var f = d.value;
                    var w = { m: function () {
                        if (!isWin(this)) throw new TypeError("Failed to execute '" + k + "' on 'Window': Illegal invocation");
                        if (arguments.length < WOPS[k]) throw new TypeError("Failed to execute '" + k + "' on 'Window': " + WOPS[k] + ' argument required, but only ' + arguments.length + ' present.');
                        /* (open's URL: its query in the document's encoding) */
                        if (k === 'open' && typeof arguments[0] === 'string' && /[^\x00-\x7f]/.test(arguments[0]) && typeof global.__cyberLegacyQuery === 'function') {
                            var dcs = global.document && global.document.characterSet;
                            if (dcs && !/^utf-8$/i.test(dcs)) {
                                var lqo = global.__cyberLegacyQuery(arguments[0], dcs), args = Array.prototype.slice.call(arguments);
                                if (lqo) { args[0] = lqo; return f.apply(this === undefined || this === null ? global : this, args); }
                            }
                        }
                        return f.apply(this === undefined || this === null ? global : this, arguments);
                    } }.m;
                    Object.defineProperty(w, 'name', { value: k });
                    Object.defineProperty(w, 'length', { value: WOPS[k] });
                    Object.defineProperty(w, '__cyberWinChecked', { value: true });
                    Object.defineProperty(global, k, { value: w, writable: d.writable, enumerable: d.enumerable, configurable: true });
                });
                /* (the interface object: not enumerable, its prototype fixed, constructor not enumerable) */
                try {
                    Object.defineProperty(global, 'Window', { value: global.Window, writable: true, enumerable: false, configurable: true });
                    Object.defineProperty(global.Window, 'prototype', { writable: false });
                    Object.defineProperty(global.Window.prototype, 'constructor', { value: global.Window, writable: true, enumerable: false, configurable: true });
                } catch (e) { }
                /* (legacy no-ops; originAgentCluster; frameElement, null at the top) */
                ['captureEvents', 'releaseEvents'].forEach(function (k) {
                    if (Object.prototype.hasOwnProperty.call(global, k)) return;
                    var m = { [k]: function () { if (!isWin(this)) throw new TypeError('Illegal invocation'); } }[k];
                    Object.defineProperty(global, k, { value: m, writable: true, enumerable: true, configurable: true });
                });
                [['originAgentCluster', function () { return false; }], ['frameElement', function () { return null; }]].forEach(function (p) {
                    if (Object.prototype.hasOwnProperty.call(global, p[0])) return;
                    var g1 = { [p[0]]: function () { if (!isWin(this)) throw new TypeError('Illegal invocation'); return p[1](); } }[p[0]];
                    Object.defineProperty(g1, 'name', { value: 'get ' + p[0] });
                    Object.defineProperty(global, p[0], { get: g1, enumerable: true, configurable: true });
                });
                ['locationbar', 'menubar', 'personalbar', 'scrollbars', 'statusbar', 'toolbar', 'status', 'length', 'navigation', 'clientInformation', 'external'].forEach(function (k) {
                    var d = Object.getOwnPropertyDescriptor(global, k);
                    if (!d || !d.get || !d.configurable || d.get.__cyberWinChecked) return;
                    var g0 = d.get, s0 = d.set;
                    var g = { get: function () { if (!isWin(this)) throw new TypeError('Illegal invocation'); return g0.call(this === undefined || this === null ? global : this); } }.get;
                    Object.defineProperty(g, 'name', { value: 'get ' + k });
                    Object.defineProperty(g, '__cyberWinChecked', { value: true });
                    var s = s0 && { set: function (v) { if (!isWin(this)) throw new TypeError('Illegal invocation'); return s0.call(this === undefined || this === null ? global : this, v); } }.set;
                    /* ([Replaceable] navigation: a set puts a value in its place) */
                    if (!s && k === 'navigation') s = { set: function (v) { if (!isWin(this)) throw new TypeError('Illegal invocation'); Object.defineProperty(global, k, { value: v, writable: true, enumerable: true, configurable: true }); } }.set;
                    if (s) Object.defineProperty(s, 'name', { value: 'set ' + k });
                    Object.defineProperty(global, k, { get: g, set: s, enumerable: d.enumerable, configurable: true });
                });
                /* (values: replaceable ones a setter that puts a value in their place; name and opener kept; the rest only read) */
                var WATTR = { self: 'r', frames: 'r', parent: 'r', name: 'w', opener: 'w', history: 'o', navigator: 'o', customElements: 'o', closed: 'o' };
                Object.keys(WATTR).forEach(function (k) {
                    var d = Object.getOwnPropertyDescriptor(global, k);
                    if (!d || !('value' in d) || !d.configurable) return;
                    var val = d.value, mode = WATTR[k];
                    var g = { get: function () { if (!isWin(this)) throw new TypeError('Illegal invocation'); return val; } }.get;
                    Object.defineProperty(g, 'name', { value: 'get ' + k });
                    var s;
                    if (mode === 'w') s = { set: function (v) { if (!isWin(this)) throw new TypeError('Illegal invocation'); val = k === 'name' ? String(v) : v; } }.set;
                    else if (mode === 'r') s = { set: function (v) {
                        if (!isWin(this)) throw new TypeError('Illegal invocation');
                        Object.defineProperty(global, k, { value: v, writable: true, enumerable: true, configurable: true });
                    } }.set;
                    if (s) Object.defineProperty(s, 'name', { value: 'set ' + k });
                    Object.defineProperty(global, k, { get: g, set: s, enumerable: d.enumerable !== false, configurable: true });
                });
            }
        } catch (e) { }
        /* (the document's attributes the engine keeps as values on it: Document.prototype's accessors over what it set, per document) */
        try {
            var DPd = global.Document && global.Document.prototype, doc0 = global.document;
            if (DPd && doc0 && !DPd.__cyberDocVals) {
                var DV = new WeakMap();
                Object.defineProperty(DPd, '__cyberDocVals', { value: DV });
                var dflt = { referrer: '', cookie: '', lastModified: '', readyState: 'complete', currentScript: null, defaultView: null,
                             hidden: false, visibilityState: 'visible', designMode: 'off', activeElement: null };
                /* (the focused element retargeted against a root: the host it is inside of there, DOM "retarget") */
                var retarget = function (el, root) {
                    for (var d = 0; el && d < 64; d++) {
                        var r = el.getRootNode ? el.getRootNode() : null;
                        if (!r || r === root || r.nodeType !== 11 || !r.host) return r === root || !root || root.nodeType === 9 ? el : null;
                        el = r.host;
                    }
                    return el;
                };
                /* (the engine's writes: here, past the accessors, which have no setters for read-only attributes) */
                var WRITABLE_DV = { cookie: 1, designMode: 1 };
                Object.defineProperty(global, '__cyberDocSet', { configurable: true, value: function (d, k, v) {
                    if (!d || (typeof d !== 'object' && typeof d !== 'function')) return;
                    var m = DV.get(d);
                    if (!m) DV.set(d, m = {});
                    m[k] = v;
                    /* (a value of its own an earlier write left: gone, the accessor's to answer) */
                    var own = Object.getOwnPropertyDescriptor(d, k);
                    if (own && 'value' in own && own.configurable) delete d[k];
                } });
                if (!('lastModified' in dflt)) dflt.lastModified = '';
                Object.keys(dflt).forEach(function (k) {
                    var od = Object.getOwnPropertyDescriptor(doc0, k);
                    var pd = Object.getOwnPropertyDescriptor(DPd, k);
                    if (pd && pd.get) return;
                    /* (an accessor of the page document's own: moved to the prototype as it is) */
                    if (od && (od.get || od.set) && od.configurable) {
                        delete doc0[k];
                        Object.defineProperty(DPd, k, { get: od.get, set: od.set, enumerable: true, configurable: true });
                        return;
                    }
                    if (od && (!('value' in od) || !od.configurable)) return;
                    var vals = DV.get(doc0);
                    if (!vals) DV.set(doc0, vals = {});
                    if (od) { vals[k] = od.value; delete doc0[k]; }
                    var g = { get: function () {
                        if (!(this instanceof global.Document)) throw new TypeError('Illegal invocation');
                        var m = DV.get(this);
                        var v = m && Object.prototype.hasOwnProperty.call(m, k) ? m[k] : dflt[k];
                        /* (no Last-Modified: now, local time, MM/DD/YYYY hh:mm:ss) */
                        if (k === 'lastModified' && !v) {
                            var dt = new Date(), p2 = function (x) { return (x < 10 ? '0' : '') + x; };
                            v = p2(dt.getMonth() + 1) + '/' + p2(dt.getDate()) + '/' + dt.getFullYear() + ' ' + p2(dt.getHours()) + ':' + p2(dt.getMinutes()) + ':' + p2(dt.getSeconds());
                        }
                        if (k === 'activeElement') {
                            if (v && (!v.isConnected || v.ownerDocument !== this)) v = null;
                            v = v ? retarget(v, this) : null;
                            if (!v) v = this.body || this.documentElement || null;
                        }
                        return v;
                    } }.get;
                    Object.defineProperty(g, 'name', { value: 'get ' + k });
                    var st = { set: function (v) {
                        if (!(this instanceof global.Document)) throw new TypeError('Illegal invocation');
                        var m = DV.get(this);
                        if (!m) DV.set(this, m = {});
                        m[k] = k === 'designMode' ? (/^on$/i.test(String(v)) ? 'on' : /^off$/i.test(String(v)) ? 'off' : (m[k] || 'off')) : k === 'cookie' ? String(v) : v;
                    } }.set;
                    Object.defineProperty(st, 'name', { value: 'set ' + k });
                    Object.defineProperty(DPd, k, { get: g, set: WRITABLE_DV[k] ? st : undefined, enumerable: true, configurable: true });
                });
                /* (a shadow root's activeElement: the focused element retargeted to it, if it is in it) */
                var SRo = global.ShadowRoot && global.ShadowRoot.prototype;
                if (SRo && !Object.getOwnPropertyDescriptor(SRo, 'activeElement') && Object.getOwnPropertyDescriptor(DPd, 'activeElement'))
                    Object.defineProperty(SRo, 'activeElement', { enumerable: true, configurable: true, get: { get: function activeElement() {
                        if (!(this instanceof global.ShadowRoot)) throw new TypeError('Illegal invocation');
                        var doc = this.ownerDocument, m = doc && DV.get(doc), v = m ? m.activeElement : null;
                        if (!v || !v.isConnected) return null;
                        for (var e = v, d = 0; e && d < 64; d++) {
                            var r = e.getRootNode();
                            if (r === this) return e;
                            if (!r || r.nodeType !== 11 || !r.host) return null;
                            e = r.host;
                        }
                        return null;
                    } }.get });
            }
        } catch (e) { }
        /* (Worker, SharedWorker: interface objects as WebIDL has them: not enumerable, length 1, operations enumerable, handlers brand checked) */
        try {
            ['Worker', 'SharedWorker'].forEach(function (wn) {
                var WC = global[wn];
                if (typeof WC !== 'function' || !WC.prototype) return;
                var gd = Object.getOwnPropertyDescriptor(global, wn);
                if (gd && gd.enumerable) Object.defineProperty(global, wn, { value: WC, writable: true, enumerable: false, configurable: true });
                try { Object.defineProperty(WC, 'length', { value: 1, configurable: true }); } catch (e) { }
                var WP = WC.prototype;
                Object.getOwnPropertyNames(WP).forEach(function (k) {
                    if (k === 'constructor') return;
                    var d = Object.getOwnPropertyDescriptor(WP, k);
                    if (!d.enumerable && d.configurable) { d.enumerable = true; Object.defineProperty(WP, k, d); }
                    if (/^on/.test(k) && d.get && d.configurable) {
                        var g0 = d.get, s0 = d.set;
                        Object.defineProperty(WP, k, { enumerable: true, configurable: true,
                            get: Object.defineProperty(function () { if (!(this instanceof WC)) throw new TypeError('Illegal invocation'); return g0.call(this); }, 'name', { value: 'get ' + k }),
                            set: s0 && Object.defineProperty(function (v) { if (!(this instanceof WC)) throw new TypeError('Illegal invocation'); return s0.call(this, v); }, 'name', { value: 'set ' + k }) });
                    }
                });
            });
        } catch (e) { }
        /* (document.body's setter, HTML 3.1.x: a body or frameset put in place of the body, or appended to the root) */
        try {
            var DPb = global.Document && global.Document.prototype;
            var bd = DPb && Object.getOwnPropertyDescriptor(DPb, 'body');
            if (bd && bd.get && !bd.set) {
                var bget = bd.get;
                var bset = { body: function (v) {
                    if (!(this instanceof global.Document)) throw new TypeError('Illegal invocation');
                    if (v !== null && !(global.HTMLElement && v instanceof global.HTMLElement))
                        throw new TypeError("Failed to set the 'body' property on 'Document': The provided value is not of type 'HTMLElement'.");
                    if (v === null || (v.localName !== 'body' && v.localName !== 'frameset'))
                        throw new global.DOMException("Failed to set the 'body' property on 'Document': The new body element is of type '" + (v ? v.localName.toUpperCase() : 'null') + "'. It must be either a 'BODY' or 'FRAMESET' element.", 'HierarchyRequestError');
                    var cur = bget.call(this);
                    if (cur === v) return;
                    if (cur) { cur.parentNode.replaceChild(v, cur); return; }
                    var root = this.documentElement;
                    if (!root) throw new global.DOMException("Failed to set the 'body' property on 'Document': No document element exists.", 'HierarchyRequestError');
                    root.appendChild(v);
                } }.body;
                Object.defineProperty(bset, 'name', { value: 'set body' });
                Object.defineProperty(DPb, 'body', { get: bget, set: bset, enumerable: true, configurable: true });
            }
        } catch (e) { }
        /* (document.domain: the prototype's accessor, not a value set on the document) */
        try {
            var dd = global.document && Object.getOwnPropertyDescriptor(global.document, 'domain');
            if (dd && 'value' in dd && dd.configurable) delete global.document.domain;
        } catch (e) { }
        /* (Location's members: [LegacyUnforgeable], own and fixed; assign/replace need their URL) */
        try {
            var loc = global.location, LC = global.Location;
            if (loc && typeof loc === 'object' && !loc.__cyberLocked) {
                ['assign', 'replace'].forEach(function (k) {
                    var f = loc[k];
                    if (typeof f !== 'function') return;
                    var w = Object.defineProperty({ m: function (url) {
                        if (arguments.length < 1) throw new TypeError("Failed to execute '" + k + "' on 'Location': 1 argument required, but only 0 present.");
                        return f.apply(this, arguments);
                    } }.m, 'name', { value: k });
                    Object.defineProperty(loc, k, { value: w, writable: true, enumerable: true, configurable: true });
                });
                /* (its members check their this: a Location) */
                var isLoc = function (t) {
                    if (t === loc) return true;
                    if (!t || typeof t !== 'object') return false;
                    try { return Object.prototype.toString.call(t) === '[object Location]'; } catch (e) { return false; }
                };
                ['href', 'origin', 'protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'ancestorOrigins', 'assign', 'replace', 'reload', 'toString'].forEach(function (k) {
                    var d = Object.getOwnPropertyDescriptor(loc, k);
                    if (!d || !d.configurable) return;
                    if (typeof d.value === 'function') {
                        var f = d.value, n0 = f.length;
                        var w = { m: function () { if (!isLoc(this)) throw new TypeError('Illegal invocation'); return f.apply(this, arguments); } }.m;
                        Object.defineProperty(w, 'name', { value: k });
                        Object.defineProperty(w, 'length', { value: n0 });
                        d.value = w;
                    } else if (d.get) {
                        var g0 = d.get, s0 = d.set;
                        d.get = Object.defineProperty({ get: function () { if (!isLoc(this)) throw new TypeError('Illegal invocation'); return g0.call(this); } }.get, 'name', { value: 'get ' + k });
                        if (s0) d.set = Object.defineProperty({ set: function (v) { if (!isLoc(this)) throw new TypeError('Illegal invocation'); return s0.call(this, v); } }.set, 'name', { value: 'set ' + k });
                    } else return;
                    Object.defineProperty(loc, k, d);
                });
                Object.defineProperty(loc, '__cyberLocked', { value: true });
            }
        } catch (e) { }
        /* (Document.parseHTMLUnsafe, a static operation the shaping passed over) */
        var DC = global.Document;
        if (DC && typeof DC.parseHTMLUnsafe !== 'function' && typeof global.DOMParser === 'function')
            Object.defineProperty(DC, 'parseHTMLUnsafe', { configurable: true, enumerable: true, writable: true, value: Object.defineProperty(function parseHTMLUnsafe(html) {
                if (arguments.length < 1) throw new TypeError("Failed to execute 'parseHTMLUnsafe' on 'Document': 1 argument required, but only 0 present.");
                return new global.DOMParser().parseFromString(String(html), 'text/html');
            }, 'length', { value: 1 }) });
        var FC = global.HTMLFormElement, HE = global.HTMLElement;
        if (!FC || !HE || Object.getPrototypeOf(FC.prototype) !== HE.prototype) return;
        var named = function (form, k) {
            if (typeof k !== 'string' || !k || k === 'then') return undefined;
            var els;
            try { els = form.elements; } catch (e) { return undefined; }
            if (!els || typeof els.namedItem !== 'function') return undefined;
            var v = els.namedItem(k);
            return v === null ? undefined : v;
        };
        var isForm = function (r) { return r && r.nodeType === 1 && r.localName === 'form' && r.namespaceURI === 'http://www.w3.org/1999/xhtml'; };
        var px = new Proxy(HE.prototype, {
            get: function (t, k, r) {
                if (typeof k === 'string' && !(k in t) && isForm(r)) { var v = named(r, k); if (v !== undefined) return v; }
                return Reflect.get(t, k, r);
            },
            has: function (t, k) { return k in t; },
            /* (the chain as it was: HTMLElement.prototype next, for instanceof and brand checks) */
            getPrototypeOf: function () { return HE.prototype; }
        });
        try { Object.setPrototypeOf(FC.prototype, px); } catch (e) { }
    } });

    /* ---- navigator.plugins and navigator.mimeTypes (HTML 8.9.1.6): with a PDF viewer, its five plugins and two types ---- */
    (function () {
        var NAV = global.navigator;
        if (!NAV || typeof global.PluginArray === 'function') return;
        var mkIface = function (name) {
            var C = function () { throw new TypeError('Illegal constructor'); };
            Object.defineProperty(C, 'name', { value: name, configurable: true });
            Object.defineProperty(C, 'length', { value: 0, configurable: true });
            Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true });
            Object.defineProperty(global, name, { value: C, writable: true, configurable: true, enumerable: false });
            return C;
        };
        var S = Symbol('pluginData');
        var PA = mkIface('PluginArray'), MTA = mkIface('MimeTypeArray'), PL = mkIface('Plugin'), MT = mkIface('MimeType');
        var chk = function (o, C) { if (!(o instanceof C) || !o[S]) throw new TypeError('Illegal invocation'); return o[S]; };
        var getter = function (C, k, f) {
            var g = { get: function () { return f(chk(this, C)); } }.get;
            Object.defineProperty(g, 'name', { value: 'get ' + k });
            Object.defineProperty(C.prototype, k, { get: g, enumerable: true, configurable: true });
        };
        var op = function (C, k, n, f) {
            var fn = { f: function () { var d = chk(this, C); if (arguments.length < n) throw new TypeError("Failed to execute '" + k + "': " + n + ' argument required, but only 0 present.'); return f(d, arguments[0]); } }.f;
            Object.defineProperty(fn, 'name', { value: k });
            Object.defineProperty(fn, 'length', { value: n });
            Object.defineProperty(C.prototype, k, { value: fn, writable: true, enumerable: true, configurable: true });
        };
        var make = function (C, data) { var o = Object.create(C.prototype); Object.defineProperty(o, S, { value: data }); return o; };
        /* (a list's own index and name properties: the items, by position and by name) */
        var fill = function (o, items, nameOf) {
            items.forEach(function (it, i) { Object.defineProperty(o, i, { value: it, enumerable: true, configurable: true }); });
            items.forEach(function (it) { var n = nameOf(it); if (!(n in o)) Object.defineProperty(o, n, { value: it, enumerable: false, configurable: true }); });
        };
        var names = ['PDF Viewer', 'Chrome PDF Viewer', 'Chromium PDF Viewer', 'Microsoft Edge PDF Viewer', 'WebKit built-in PDF'];
        var on = NAV.pdfViewerEnabled !== false;
        var plugins = on ? names.map(function (n) { return make(PL, { name: n, types: [] }); }) : [];
        var types = on ? ['application/pdf', 'text/pdf'].map(function (t) { return make(MT, { type: t, plugin: plugins[0] }); }) : [];
        plugins.forEach(function (p) {
            var d = p[S];
            d.types = ['application/pdf', 'text/pdf'].map(function (t) { return make(MT, { type: t, plugin: p }); });
            fill(p, d.types, function (m) { return m[S].type; });
        });
        getter(PL, 'name', function (d) { return d.name; });
        getter(PL, 'description', function () { return 'Portable Document Format'; });
        getter(PL, 'filename', function () { return 'internal-pdf-viewer'; });
        getter(PL, 'length', function (d) { return d.types.length; });
        op(PL, 'item', 1, function (d, i) { i = Number(i) >>> 0; return i < d.types.length ? d.types[i] : null; });
        op(PL, 'namedItem', 1, function (d, n) { n = String(n); for (var i = 0; i < d.types.length; i++) if (d.types[i][S].type === n) return d.types[i]; return null; });
        getter(MT, 'type', function (d) { return d.type; });
        getter(MT, 'description', function () { return 'Portable Document Format'; });
        getter(MT, 'suffixes', function () { return 'pdf'; });
        getter(MT, 'enabledPlugin', function (d) { return d.plugin; });
        var pa = make(PA, { list: plugins }), mta = make(MTA, { list: types });
        fill(pa, plugins, function (p) { return p[S].name; });
        fill(mta, types, function (m) { return m[S].type; });
        op(PA, 'refresh', 0, function () { });
        getter(PA, 'length', function (d) { return d.list.length; });
        op(PA, 'item', 1, function (d, i) { i = Number(i) >>> 0; return i < d.list.length ? d.list[i] : null; });
        op(PA, 'namedItem', 1, function (d, n) { n = String(n); for (var i = 0; i < d.list.length; i++) if (d.list[i][S].name === n) return d.list[i]; return null; });
        getter(MTA, 'length', function (d) { return d.list.length; });
        op(MTA, 'item', 1, function (d, i) { i = Number(i) >>> 0; return i < d.list.length ? d.list[i] : null; });
        op(MTA, 'namedItem', 1, function (d, n) { n = String(n); for (var i = 0; i < d.list.length; i++) if (d.list[i][S].type === n) return d.list[i]; return null; });
        [PA, MTA, PL].forEach(function (C) { Object.defineProperty(C.prototype, Symbol.iterator, { value: Array.prototype.values, writable: true, configurable: true }); });
        var NP = Object.getPrototypeOf(NAV);
        var put = function (k, v) {
            var tgt = NP && NP !== Object.prototype ? NP : NAV;
            var g = { get: function () { return v; } }.get;
            Object.defineProperty(g, 'name', { value: 'get ' + k });
            try { if (Object.getOwnPropertyDescriptor(NAV, k)) delete NAV[k]; } catch (e) { }
            Object.defineProperty(tgt, k, { get: g, enumerable: true, configurable: true });
        };
        put('plugins', pa);
        put('mimeTypes', mta);
    })();

    /* ---- getHTML (HTML 8.5): serialized with the shadow roots asked for, as declarative ones; setHTML and the like:
     * the markup sanitized (Sanitizer API, its safe default), and Sanitizer ---- */
    (function () {
        var EP = global.Element && global.Element.prototype, SRP = global.ShadowRoot && global.ShadowRoot.prototype;
        if (!EP || typeof EP.getHTML === 'function') return;
        var included = function (sr, opts) {
            if (!sr) return false;
            if (opts.serializableShadowRoots && sr.serializable) return true;
            return opts.shadowRoots.indexOf(sr) >= 0;
        };
        var hasIncluded = function (n, opts) {
            if (n.nodeType === 1 && included(n.shadowRoot, opts)) return true;
            var kids = n.nodeType === 1 && n.localName === 'template' && n.content ? n.content.childNodes : n.childNodes;
            for (var i = 0; i < kids.length; i++) if (hasIncluded(kids[i], opts)) return true;
            return false;
        };
        var templateOf = function (sr, opts) {
            var a = ' shadowrootmode="' + sr.mode + '"';
            if (sr.delegatesFocus) a += ' shadowrootdelegatesfocus=""';
            if (sr.serializable) a += ' shadowrootserializable=""';
            if (sr.clonable) a += ' shadowrootclonable=""';
            return '<template' + a + '>' + children(sr, opts) + '</template>';
        };
        var outer = function (n, opts) {
            if (n.nodeType !== 1 || !hasIncluded(n, opts)) {
                if (n.nodeType === 1) return n.outerHTML;
                var d = n.ownerDocument.createElement('div');
                d.appendChild(n.cloneNode(false));
                return d.innerHTML;
            }
            var oh = n.cloneNode(false).outerHTML;
            var close = '</' + (n.namespaceURI === 'http://www.w3.org/1999/xhtml' ? n.localName : n.tagName) + '>';
            var open = oh.endsWith(close) ? oh.slice(0, oh.length - close.length) : oh;
            var body = (included(n.shadowRoot, opts) ? templateOf(n.shadowRoot, opts) : '') +
                       children(n.localName === 'template' && n.content ? n.content : n, opts);
            return open + body + close;
        };
        var children = function (n, opts) {
            var s = '', kids = n.childNodes;
            for (var i = 0; i < kids.length; i++) s += outer(kids[i], opts);
            return s;
        };
        var optsOf = function (o) {
            var r = { serializableShadowRoots: !!(o && o.serializableShadowRoots), shadowRoots: [] };
            if (o && o.shadowRoots !== undefined) {
                if (o.shadowRoots === null || typeof o.shadowRoots !== 'object' || typeof o.shadowRoots[Symbol.iterator] !== 'function')
                    throw new TypeError("Failed to execute 'getHTML': The provided value cannot be converted to a sequence.");
                for (var x of o.shadowRoots) {
                    if (!(global.ShadowRoot && x instanceof global.ShadowRoot)) throw new TypeError("Failed to execute 'getHTML': Failed to convert value to 'ShadowRoot'.");
                    r.shadowRoots.push(x);
                }
            }
            return r;
        };
        var getHTML = function getHTML(options) {
            var opts = optsOf(options);
            var self = this;
            var tmpl = self.nodeType === 1 && self.localName === 'template' && self.content ? self.content : self;
            var pre = self.nodeType === 1 && included(self.shadowRoot, opts) ? templateOf(self.shadowRoot, opts) : '';
            return pre + children(tmpl, opts);
        };
        Object.defineProperty(EP, 'getHTML', { value: getHTML, writable: true, enumerable: true, configurable: true });
        if (SRP) Object.defineProperty(SRP, 'getHTML', { value: getHTML, writable: true, enumerable: true, configurable: true });
        /* (the safe default: no script, no event handler attributes, no javascript: URLs, none of the elements that run or embed) */
        var UNSAFE_EL = { script: 1, iframe: 1, frame: 1, frameset: 1, object: 1, embed: 1, noscript: 1, noembed: 1, noframes: 1, base: 1, meta: 1, link: 1, style: 0, template: 0, portal: 1, applet: 1, use: 0 };
        var URL_ATTR = { href: 1, src: 1, action: 1, formaction: 1, 'xlink:href': 1, data: 1, poster: 1, codebase: 1, background: 1 };
        var scrub = function (root) {
            var walk = function (n) {
                for (var c = n.firstChild; c; ) {
                    var next = c.nextSibling;
                    if (c.nodeType === 1) {
                        var ln = c.localName;
                        if (UNSAFE_EL[ln] === 1) { c.parentNode.removeChild(c); c = next; continue; }
                        for (var i = c.attributes.length - 1; i >= 0; i--) {
                            var a = c.attributes[i], an = a.name.toLowerCase();
                            if (/^on/.test(an) || (URL_ATTR[an] && /^\s*javascript:/i.test(a.value))) c.removeAttributeNode(a);
                        }
                        walk(c.localName === 'template' && c.content ? c.content : c);
                    } else if (c.nodeType === 7) c.parentNode.removeChild(c);
                    c = next;
                }
            };
            walk(root);
            return root;
        };
        var parseSafe = function (ctxEl, html) {
            var doc = ctxEl.ownerDocument || global.document;
            var t = doc.createElement('template');
            t.innerHTML = String(html);
            return scrub(t.content);
        };
        /* (unsafe: parsed as is, its scripts left inert as a template's) */
        var parseUnsafe = function (ctxEl, html) {
            var doc = ctxEl.ownerDocument || global.document;
            var t = doc.createElement('template');
            t.innerHTML = String(html);
            return t.content;
        };
        var put = function (name, how, unsafe, elementOnly) {
            var f = { f: function (html, options) {
                if (!(this instanceof global.Node)) throw new TypeError('Illegal invocation');
                if (arguments.length < 1) throw new TypeError("Failed to execute '" + name + "': 1 argument required, but only 0 present.");
                var outer = how === 'before' || how === 'after' || how === 'replace';
                var ctxEl = this.nodeType === 11 ? (this.host || global.document.body) : outer && this.parentNode && this.parentNode.nodeType === 1 ? this.parentNode : this;
                var frag = unsafe ? parseUnsafe(ctxEl, html) : parseSafe(ctxEl, html);
                if (how === 'set') { while (this.firstChild) this.removeChild(this.firstChild); this.appendChild(frag); }
                else if (how === 'append') this.appendChild(frag);
                else if (how === 'prepend') this.insertBefore(frag, this.firstChild);
                else {
                    var p = this.parentNode;
                    if (!p) return;
                    if (how === 'before') p.insertBefore(frag, this);
                    else if (how === 'after') p.insertBefore(frag, this.nextSibling);
                    else p.replaceChild(frag, this);
                }
            } }.f;
            Object.defineProperty(f, 'name', { value: name });
            Object.defineProperty(f, 'length', { value: 1 });
            /* (before, after, replaceWith: CharacterData's too, as ChildNode's) */
            var CDP = elementOnly && global.CharacterData ? global.CharacterData.prototype : null;
            [EP, elementOnly ? null : SRP, CDP].forEach(function (P) { if (P && typeof P[name] !== 'function') Object.defineProperty(P, name, { value: f, writable: true, enumerable: true, configurable: true }); });
        };
        put('setHTML', 'set'); put('appendHTML', 'append'); put('prependHTML', 'prepend');
        put('beforeHTML', 'before', false, true); put('afterHTML', 'after', false, true); put('replaceWithHTML', 'replace', false, true);
        put('appendHTMLUnsafe', 'append', true); put('prependHTMLUnsafe', 'prepend', true);
        put('beforeHTMLUnsafe', 'before', true, true); put('afterHTMLUnsafe', 'after', true, true); put('replaceWithHTMLUnsafe', 'replace', true, true);
        if (SRP && typeof SRP.setHTMLUnsafe !== 'function' && typeof EP.setHTMLUnsafe === 'function')
            Object.defineProperty(SRP, 'setHTMLUnsafe', { value: EP.setHTMLUnsafe, writable: true, enumerable: true, configurable: true });
        /* (Sanitizer: its configuration, as given, and the lists' edits) */
        if (typeof global.Sanitizer !== 'function') {
            var CFG = Symbol('sanitizerConfig');
            var SZ = function Sanitizer(configuration) {
                if (!new.target) throw new TypeError("Failed to construct 'Sanitizer': Please use the 'new' operator");
                var c = configuration === undefined || configuration === 'default' ? {} : configuration;
                if (typeof c === 'string') { if (c !== 'default') throw new TypeError("Failed to construct 'Sanitizer': The provided value '" + c + "' is not a valid enum value of type SanitizerPresets."); c = {}; }
                if (c === null || typeof c !== 'object') throw new TypeError("Failed to construct 'Sanitizer': The provided value is not of type 'SanitizerConfig'.");
                var conf = {};
                ['elements', 'removeElements', 'replaceWithChildrenElements', 'processingInstructions', 'removeProcessingInstructions', 'attributes', 'removeAttributes'].forEach(function (k) {
                    if (c[k] !== undefined) conf[k] = Array.prototype.slice.call(c[k]).map(function (x) { return typeof x === 'string' ? { name: x, namespace: k.indexOf('ttribute') >= 0 ? null : 'http://www.w3.org/1999/xhtml' } : x; });
                });
                conf.comments = c.comments === undefined ? false : !!c.comments;
                conf.dataAttributes = c.dataAttributes === undefined ? false : !!c.dataAttributes;
                Object.defineProperty(this, CFG, { value: conf });
            };
            var cf = function (o) { if (!o || !o[CFG]) throw new TypeError('Illegal invocation'); return o[CFG]; };
            var nm = function (x) { return typeof x === 'string' ? x : x && x.name; };
            var addTo = function (list, x) { var n = nm(x); if (list.some(function (y) { return nm(y) === n; })) return false; list.push(typeof x === 'string' ? { name: x } : x); return true; };
            var dropFrom = function (list, x) { var n = nm(x), i = list.findIndex(function (y) { return nm(y) === n; }); if (i < 0) return false; list.splice(i, 1); return true; };
            var mm = function (k, n, f) {
                var fn = { f: function () { var c = cf(this); if (arguments.length < n) throw new TypeError("Failed to execute '" + k + "' on 'Sanitizer': " + n + ' argument required, but only 0 present.'); return f(c, arguments[0]); } }.f;
                Object.defineProperty(fn, 'name', { value: k });
                Object.defineProperty(fn, 'length', { value: n });
                Object.defineProperty(SZ.prototype, k, { value: fn, writable: true, enumerable: true, configurable: true });
            };
            var L = function (c, k) { return c[k] || (c[k] = []); };
            mm('get', 0, function (c) { return JSON.parse(JSON.stringify(c)); });
            mm('allowElement', 1, function (c, x) { dropFrom(L(c, 'removeElements'), x); return addTo(L(c, 'elements'), x); });
            mm('removeElement', 1, function (c, x) { dropFrom(L(c, 'elements'), x); return addTo(L(c, 'removeElements'), x); });
            mm('replaceElementWithChildren', 1, function (c, x) { return addTo(L(c, 'replaceWithChildrenElements'), x); });
            mm('allowProcessingInstruction', 1, function (c, x) { dropFrom(L(c, 'removeProcessingInstructions'), x); return addTo(L(c, 'processingInstructions'), x); });
            mm('removeProcessingInstruction', 1, function (c, x) { dropFrom(L(c, 'processingInstructions'), x); return addTo(L(c, 'removeProcessingInstructions'), x); });
            mm('allowAttribute', 1, function (c, x) { dropFrom(L(c, 'removeAttributes'), x); return addTo(L(c, 'attributes'), x); });
            mm('removeAttribute', 1, function (c, x) { dropFrom(L(c, 'attributes'), x); return addTo(L(c, 'removeAttributes'), x); });
            mm('setComments', 1, function (c, v) { v = !!v; var ch = c.comments !== v; c.comments = v; return ch; });
            mm('setDataAttributes', 1, function (c, v) { v = !!v; var ch = c.dataAttributes !== v; c.dataAttributes = v; return ch; });
            mm('setJavascriptURLs', 1, function (c, v) { v = !!v; var ch = c.javascriptURLs !== v; c.javascriptURLs = v; return ch; });
            mm('removeUnsafe', 0, function (c) { var ch = false; ['script', 'iframe', 'object', 'embed', 'frame'].forEach(function (e) { dropFrom(L(c, 'elements'), e); if (addTo(L(c, 'removeElements'), e)) ch = true; }); return ch; });
            Object.defineProperty(SZ.prototype, Symbol.toStringTag, { value: 'Sanitizer', configurable: true });
            Object.defineProperty(global, 'Sanitizer', { value: SZ, writable: true, configurable: true, enumerable: false });
        }
    })();

    /* ---- TrackEvent; ElementInternals and CustomStateSet (attachInternals); DOMParser's prototype; the registry's operations on
     * CustomElementRegistry.prototype ---- */
    (function () {
        var EV = global.Event;
        if (EV && typeof global.TrackEvent !== 'function') {
            var TE = function TrackEvent(type, init) {
                if (!new.target) throw new TypeError("Failed to construct 'TrackEvent': Please use the 'new' operator");
                if (arguments.length < 1) throw new TypeError("Failed to construct 'TrackEvent': 1 argument required, but only 0 present.");
                var e = Reflect.construct(EV, [type, init], new.target);
                if (Object.getPrototypeOf(e) !== new.target.prototype) Object.setPrototypeOf(e, new.target.prototype);   /* (the native Event's new.target unheeded) */
                var t = init && init.track !== undefined ? init.track : null;
                if (t !== null && !((global.VideoTrack && t instanceof global.VideoTrack) || (global.AudioTrack && t instanceof global.AudioTrack) || (global.TextTrack && t instanceof global.TextTrack)))
                    throw new TypeError("Failed to construct 'TrackEvent': The provided value is not of type '(VideoTrack or AudioTrack or TextTrack)'.");
                Object.defineProperty(e, '__track', { value: t });
                return e;
            };
            TE.prototype = Object.create(EV.prototype);
            Object.defineProperty(TE.prototype, 'constructor', { value: TE, writable: true, configurable: true });
            Object.defineProperty(TE.prototype, Symbol.toStringTag, { value: 'TrackEvent', configurable: true });
            Object.defineProperty(TE.prototype, 'track', { enumerable: true, configurable: true,
                get: { get: function track() { if (!(this instanceof TE)) throw new TypeError('Illegal invocation'); return this.__track; } }.get });
            Object.setPrototypeOf(TE, EV);
            Object.defineProperty(TE, 'length', { value: 1 });
            Object.defineProperty(global, 'TrackEvent', { value: TE, writable: true, configurable: true, enumerable: false });
        }
        /* (CustomStateSet: a set of strings, setlike) */
        if (typeof global.CustomStateSet !== 'function') {
            var SET = Symbol('states');
            var CSS_ = function CustomStateSet() { throw new TypeError('Illegal constructor'); };
            var csp = CSS_.prototype;
            var st = function (o) { if (!o || !o[SET]) throw new TypeError('Illegal invocation'); return o[SET]; };
            var dm = function (k, n, f) { var fn = { f: function () { return f.apply(this, arguments); } }.f; Object.defineProperty(fn, 'name', { value: k }); Object.defineProperty(fn, 'length', { value: n }); Object.defineProperty(csp, k, { value: fn, writable: true, enumerable: true, configurable: true }); };
            dm('add', 1, function (v) { var s = st(this); s.set.add(String(v)); if (s.changed) s.changed(); return this; });
            dm('delete', 1, function (v) { var s = st(this), r = s.set.delete(String(v)); if (r && s.changed) s.changed(); return r; });
            dm('clear', 0, function () { var s = st(this); s.set.clear(); if (s.changed) s.changed(); });
            dm('has', 1, function (v) { return st(this).set.has(String(v)); });
            dm('forEach', 1, function (cb, t) { var self = this; st(this).set.forEach(function (v) { cb.call(t, v, v, self); }); });
            dm('values', 0, function () { return st(this).set.values(); });
            dm('keys', 0, function () { return st(this).set.values(); });
            dm('entries', 0, function () { return st(this).set.entries(); });
            Object.defineProperty(csp, Symbol.iterator, { value: csp.values, writable: true, configurable: true });
            Object.defineProperty(csp, 'size', { enumerable: true, configurable: true, get: { get: function size() { return st(this).set.size; } }.get });
            Object.defineProperty(csp, Symbol.toStringTag, { value: 'CustomStateSet', configurable: true });
            Object.defineProperty(global, 'CustomStateSet', { value: CSS_, writable: true, configurable: true, enumerable: false });
            global.__cyberNewStateSet = function (changed) { var o = Object.create(csp); Object.defineProperty(o, SET, { value: { set: new Set(), changed: changed } }); return o; };
            Object.defineProperty(global, '__cyberNewStateSet', { enumerable: false });
        }
        /* (ElementInternals: an autonomous custom element's, from attachInternals once) */
        var HE = global.HTMLElement;
        if (HE && typeof HE.prototype.attachInternals !== 'function' && typeof global.ElementInternals !== 'function') {
            var EL = Symbol('internalsOf'), ATT = Symbol('internals');
            var EI = function ElementInternals() { throw new TypeError('Illegal constructor'); };
            var eip = EI.prototype;
            var me = function (o) { var el = o && o[EL]; if (!el) throw new TypeError('Illegal invocation'); return el; };
            var isFACE = function (el) { var C = global.customElements && global.customElements.get(el.localName); return !!(C && C.formAssociated); };
            var ga = function (k, f) { Object.defineProperty(eip, k, { enumerable: true, configurable: true, get: Object.defineProperty({ get: function () { return f.call(this, me(this)); } }.get, 'name', { value: 'get ' + k }) }); };
            var op = function (k, n, f) { var fn = { f: function () { var el = me(this); return f.apply(this, [el].concat(Array.prototype.slice.call(arguments))); } }.f; Object.defineProperty(fn, 'name', { value: k }); Object.defineProperty(fn, 'length', { value: n }); Object.defineProperty(eip, k, { value: fn, writable: true, enumerable: true, configurable: true }); };
            var notFACE = function () { throw new global.DOMException("The target element is not a form-associated custom element.", 'NotSupportedError'); };
            ga('shadowRoot', function (el) { var sr = el.shadowRoot; return sr || (this.__closedRoot || null); });
            ga('form', function (el) { if (!isFACE(el)) notFACE(); return el.closest ? el.closest('form') : null; });
            ga('willValidate', function (el) { if (!isFACE(el)) notFACE(); return !el.hasAttribute('readonly') && !el.matches(':disabled') && !(el.closest && el.closest('datalist')); });
            ga('validity', function (el) { if (!isFACE(el)) notFACE(); return this.__validity || (this.__validity = { valid: true }); });
            ga('validationMessage', function (el) { if (!isFACE(el)) notFACE(); return this.__vmsg || ''; });
            ga('labels', function (el) { if (!isFACE(el)) notFACE(); var d = el.ownerDocument, out = []; if (d) d.querySelectorAll('label').forEach(function (l) { if (l.htmlFor ? l.htmlFor === el.id : l.contains(el)) out.push(l); }); if (global.NodeList) Object.setPrototypeOf(out, global.NodeList.prototype); return out; });
            ga('states', function (el) { return this.__states || (this.__states = global.__cyberNewStateSet ? global.__cyberNewStateSet() : new Set()); });
            op('setFormValue', 1, function (el, v, state) { if (!isFACE(el)) notFACE(); this.__value = v; this.__state = state; });
            op('setValidity', 0, function (el, flags, msg) { if (!isFACE(el)) notFACE(); var v = { valid: true }; if (flags) for (var k in flags) if (flags[k]) { v[k] = true; v.valid = false; } this.__validity = v; this.__vmsg = v.valid ? '' : String(msg === undefined ? '' : msg); });
            op('checkValidity', 0, function (el) { if (!isFACE(el)) notFACE(); var ok = !this.__validity || this.__validity.valid; if (!ok) el.dispatchEvent(new global.Event('invalid', { cancelable: true })); return ok; });
            op('reportValidity', 0, function (el) { return eip.checkValidity.call(this); });
            Object.defineProperty(eip, Symbol.toStringTag, { value: 'ElementInternals', configurable: true });
            Object.defineProperty(global, 'ElementInternals', { value: EI, writable: true, configurable: true, enumerable: false });
            Object.defineProperty(HE.prototype, 'attachInternals', { writable: true, enumerable: true, configurable: true, value: function attachInternals() {
                if (!(this instanceof HE)) throw new TypeError('Illegal invocation');
                if (this.getAttribute('is') !== null) throw new global.DOMException("Unable to attach ElementInternals to a customized built-in element.", 'NotSupportedError');
                var C = global.customElements && global.customElements.get(this.localName);
                if (!C) throw new global.DOMException("Unable to attach ElementInternals to non-custom elements.", 'NotSupportedError');
                if (C.disabledFeatures && Array.prototype.indexOf.call(C.disabledFeatures, 'internals') >= 0)
                    throw new global.DOMException("ElementInternals is disabled by disabledFeature static field.", 'NotSupportedError');
                if (this[ATT]) throw new global.DOMException("ElementInternals for the specified element was already attached.", 'NotSupportedError');
                var i = Object.create(eip);
                Object.defineProperty(i, EL, { value: this });
                Object.defineProperty(this, ATT, { value: i });
                return i;
            } });
        }
        /* (DOMParser: an interface with a prototype, its operation there) */
        var NDP = global.DOMParser;
        if (typeof NDP === 'function' && !(NDP.prototype && typeof NDP.prototype.parseFromString === 'function')) {
            var DPW = function DOMParser() {
                if (!new.target) throw new TypeError("Failed to construct 'DOMParser': Please use the 'new' operator");
                return Object.create(new.target.prototype);
            };
            Object.defineProperty(DPW.prototype, 'parseFromString', { writable: true, enumerable: true, configurable: true, value: function parseFromString(str, type) {
                if (!(this instanceof DPW)) throw new TypeError('Illegal invocation');
                if (arguments.length < 2) throw new TypeError("Failed to execute 'parseFromString' on 'DOMParser': 2 arguments required, but only " + arguments.length + ' present.');
                return new NDP().parseFromString(str, type);
            } });
            Object.defineProperty(DPW.prototype, Symbol.toStringTag, { value: 'DOMParser', configurable: true });
            Object.defineProperty(global, 'DOMParser', { value: DPW, writable: true, configurable: true, enumerable: false });
        }
        /* (the registry's operations: on its prototype, the instance's own ones moved there) */
        var reg = global.customElements, CER = global.CustomElementRegistry;
        if (reg && typeof CER === 'function' && CER.prototype && Object.getPrototypeOf(reg) === CER.prototype) {
            ['define', 'get', 'getName', 'whenDefined', 'upgrade', 'initialize'].forEach(function (k) {
                var own = Object.getOwnPropertyDescriptor(reg, k);
                if (!own || typeof own.value !== 'function' || Object.getOwnPropertyDescriptor(CER.prototype, k)) return;
                var f = own.value;
                var w = { w: function () { if (!(this instanceof CER)) throw new TypeError('Illegal invocation'); return f.apply(this, arguments); } }.w;
                Object.defineProperty(w, 'name', { value: k });
                Object.defineProperty(w, 'length', { value: f.length });
                Object.defineProperty(CER.prototype, k, { value: w, writable: true, enumerable: true, configurable: true });
                try { delete reg[k]; } catch (e) { }
            });
        }
    })();

    /* ---- SVGImageElement.decode(): its href's image fetched and decoded, else an EncodingError ---- */
    (function () {
        var SI = global.SVGImageElement;
        if (!SI || !SI.prototype || typeof SI.prototype.decode === 'function' || typeof global.Image !== 'function') return;
        Object.defineProperty(SI.prototype, 'decode', { writable: true, enumerable: true, configurable: true, value: function decode() {
            if (!(this instanceof SI)) return Promise.reject(new TypeError('Illegal invocation'));
            var h = this.getAttribute('href');
            if (h === null) h = this.getAttributeNS('http://www.w3.org/1999/xlink', 'href');
            var bad = function () { return new global.DOMException('The source image cannot be decoded.', 'EncodingError'); };
            if (!h) return Promise.reject(bad());
            var url;
            try { url = new global.URL(h, this.baseURI).href; } catch (e) { return Promise.reject(bad()); }
            return new Promise(function (resolve, reject) {
                var im = new global.Image();
                im.onload = function () { resolve(undefined); };
                im.onerror = function () { reject(bad()); };
                im.src = url;
            });
        } });
    })();

    /* ---- document.domain (HTML 7.1.2): its origin's effective domain; set, a suffix of its host, for same origin-domain access ---- */
    (function () {
        var DC = global.Document, DP = DC && DC.prototype;
        if (!DP || !global.document) return;
        var op = function (v) { var f = global.__cyberDomain; if (typeof f !== 'function') return null; return arguments.length ? f(v) : f(); };
        var sec = function (m) { return new global.DOMException("Failed to set the 'domain' property on 'Document': " + m, 'SecurityError'); };
        var host = function () { try { return String(global.location.hostname || ''); } catch (e) { return ''; } };
        var opaque = function () { try { return global.origin === 'null'; } catch (e) { return true; } };
        var isIP = function (h) { return /^\d+\.\d+\.\d+\.\d+$/.test(h) || /^\[.*\]$/.test(h) || h.indexOf(':') >= 0; };
        var d0 = Object.getOwnPropertyDescriptor(global.document, 'domain');
        if (d0 && d0.configurable) delete global.document.domain;
        Object.defineProperty(DP, 'domain', { configurable: true, enumerable: true,
            get: { get: function () {
                if (!(this instanceof DC)) throw new TypeError('Illegal invocation');
                if (opaque()) return '';
                var d = this === global.document ? op() : null;
                return d !== null && d !== undefined ? d : host();
            } }.get,
            set: { set: function (v) {
                if (!(this instanceof DC)) throw new TypeError('Illegal invocation');
                v = String(v);
                if (this !== global.document || !this.defaultView) throw sec('The document has no browsing context.');
                if (opaque()) throw sec("Assignment is forbidden for sandboxed iframes.");
                var h = host(), cur = op(), eff = cur !== null && cur !== undefined ? cur : h;
                if (!eff) throw sec("'" + v + "' is not a suffix of ''.");
                var lv = v.toLowerCase();
                var ok = isIP(eff) ? lv === eff : (lv === eff || (lv && lv.charAt(0) !== '.' && eff.length > lv.length && eff.slice(-lv.length - 1) === '.' + lv && lv.indexOf('.') > 0));
                if (!ok) throw sec("'" + v + "' is not a suffix of '" + eff + "'.");
                /* (an origin-keyed agent cluster: nothing changes) */
                if (global.originAgentCluster === true) return;
                op(lv);
            } }.set });
    })();

    /* ---- media-playback-while-not-visible (permissions policy): a frame allowed 'none' can't play while not rendered, and pauses when hidden ---- */
    var mediaPolicy = function () {
        var MP = global.HTMLMediaElement && global.HTMLMediaElement.prototype;
        if (!MP || !global.document) return;
        var blockedHere = function () {
            if (typeof global.__cyberFrameAllow !== 'function') return false;
            var v = global.__cyberFrameAllow('media-playback-while-not-visible');
            return v !== null && v !== undefined && /^'none'$|^\(\)$/.test(String(v).trim());
        };
        var hidden = function () { try { return !!global.__cyberFrameChainHidden(); } catch (e) { return false; } };
        var watching = false;
        var watch = function () {
            if (watching) return;
            watching = true;
            var tick = function () {
                if (hidden()) {
                    var ms = global.document.querySelectorAll('video, audio');
                    for (var i = 0; i < ms.length; i++) if (!ms[i].paused) { try { ms[i].pause(); } catch (e) { } }
                }
                global.setTimeout(tick, 100);
            };
            global.setTimeout(tick, 100);
        };
        /* (each prototype that has its own play: the media element's, the video's, the audio's) */
        [global.HTMLMediaElement, global.HTMLVideoElement, global.HTMLAudioElement].forEach(function (C) {
            var P = C && C.prototype;
            if (!P || !Object.prototype.hasOwnProperty.call(P, 'play')) return;
            var origPlay = P.play;
            if (typeof origPlay !== 'function') return;
            var w = { play: function () {
                if (blockedHere()) {
                    watch();
                    if (hidden()) return Promise.reject(new global.DOMException('Playback is not allowed while the frame is not visible.', 'NotAllowedError'));
                }
                return origPlay.apply(this, arguments);
            } }.play;
            Object.defineProperty(w, 'length', { value: 0 });
            Object.defineProperty(P, 'play', { configurable: true, enumerable: true, writable: true, value: w });
        });
        if (blockedHere()) watch();
    };

    /* ---- an SVG <image>'s href set: its image fetched, load or error at the element ---- */
    (function () {
        var tok = typeof WeakMap === 'function' ? new WeakMap() : null;
        if (!tok || typeof global.Image !== 'function') return;
        Object.defineProperty(global, '__cyberSvgImageHref', { configurable: true, value: function (el) {
            var h = el.getAttribute('href');
            if (h === null) h = el.getAttributeNS('http://www.w3.org/1999/xlink', 'href');
            var im = new global.Image();
            tok.set(el, im);
            var fire = function (t) {
                if (tok.get(el) !== im) return;
                tok.delete(el);
                el.dispatchEvent(new global.Event(t));
            };
            if (!h) { global.setTimeout(function () { fire('error'); }, 0); return; }
            var url;
            try { url = new global.URL(h, el.baseURI).href; } catch (e) { global.setTimeout(function () { fire('error'); }, 0); return; }
            im.onload = function () { fire('load'); };
            im.onerror = function () { fire('error'); };
            im.src = url;
        } });
    })();

    /* ---- interfaces nothing makes here yet: NotRestoredReasons (bfcache reasons), VisibilityStateEntry ---- */
    (function () {
        var shell = function (name, parent, attrs, json) {
            if (typeof global[name] === 'function') return;
            var P0 = parent && global[parent];
            if (parent && typeof P0 !== 'function') return;
            var C = function () { throw new TypeError('Illegal constructor'); };
            Object.defineProperty(C, 'name', { value: name, configurable: true });
            Object.defineProperty(C, 'length', { value: 0, configurable: true });
            var P = parent ? Object.create(P0.prototype) : C.prototype;
            if (parent) { Object.defineProperty(C, 'prototype', { value: P, writable: false }); Object.setPrototypeOf(C, P0); }
            Object.defineProperty(P, 'constructor', { value: C, writable: true, configurable: true });
            Object.defineProperty(P, Symbol.toStringTag, { value: name, configurable: true });
            var D = Symbol(name);
            attrs.forEach(function (a) {
                var g = { get: function () { if (!(this instanceof C) || !this[D]) throw new TypeError('Illegal invocation'); return this[D][a]; } }.get;
                Object.defineProperty(g, 'name', { value: 'get ' + a });
                Object.defineProperty(P, a, { get: g, enumerable: true, configurable: true });
            });
            if (json) Object.defineProperty(P, 'toJSON', { writable: true, enumerable: true, configurable: true, value: function toJSON() {
                if (!(this instanceof C) || !this[D]) throw new TypeError('Illegal invocation');
                var o = {}, self = this;
                attrs.forEach(function (a) { o[a] = self[a]; });
                return o;
            } });
            Object.defineProperty(global, name, { value: C, writable: true, configurable: true, enumerable: false });
        };
        shell('NotRestoredReasonDetails', null, ['reason'], true);
        shell('NotRestoredReasons', null, ['src', 'id', 'name', 'url', 'reasons', 'children'], true);
        shell('VisibilityStateEntry', 'PerformanceEntry', ['name', 'entryType', 'startTime', 'duration'], false);
    })();

    /* ---- CloseWatcher (HTML 6.x close requests): requestClose, close, destroy; Escape asks the newest one ---- */
    (function () {
        var ET = global.EventTarget;
        if (!ET || typeof global.CloseWatcher === 'function') return;
        var ACT = Symbol('cwActive');
        var CWC = function CloseWatcher(options) {
            if (!new.target) throw new TypeError("Failed to construct 'CloseWatcher': Please use the 'new' operator");
            var w = Reflect.construct(ET, [], new.target);
            var doc = global.document;
            if (!doc || !doc.defaultView) throw new global.DOMException('The document is not fully active.', 'InvalidStateError');
            Object.defineProperty(w, ACT, { value: { on: true, running: false }, writable: true });
            Object.defineProperty(w, 'ownerDocument', { value: doc, configurable: true });
            Object.defineProperty(w, '__cyberActive', { value: function () { return w[ACT].on; }, configurable: true });
            var sig = options && options.signal;
            if (sig) {
                if (sig.aborted) w[ACT].on = false;
                else sig.addEventListener('abort', function () { w.destroy(); });
            }
            if (w[ACT].on && typeof global.__cyberCloseWatchAdd === 'function') global.__cyberCloseWatchAdd(w);
            return w;
        };
        CWC.prototype = Object.create(ET.prototype);
        Object.defineProperty(CWC.prototype, 'constructor', { value: CWC, writable: true, configurable: true });
        Object.defineProperty(CWC.prototype, Symbol.toStringTag, { value: 'CloseWatcher', configurable: true });
        Object.setPrototypeOf(CWC, ET);
        var st = function (o) { if (!(o instanceof CWC) || !o[ACT]) throw new TypeError('Illegal invocation'); return o[ACT]; };
        var m = function (k, f) { Object.defineProperty(CWC.prototype, k, { value: f, writable: true, enumerable: true, configurable: true }); };
        m('requestClose', function requestClose() {
            var s = st(this);
            if (!s.on || s.running) return;
            s.running = true;
            var go = this.dispatchEvent(new global.Event('cancel', { cancelable: false }));
            s.running = false;
            if (!go || !s.on) return;
            this.close();
        });
        m('close', function close() {
            var s = st(this);
            if (!s.on) return;
            s.on = false;
            if (typeof global.__cyberCloseWatchRemove === 'function') global.__cyberCloseWatchRemove(this);
            this.dispatchEvent(new global.Event('close'));
        });
        m('destroy', function destroy() {
            var s = st(this);
            s.on = false;
            if (typeof global.__cyberCloseWatchRemove === 'function') global.__cyberCloseWatchRemove(this);
        });
        ['cancel', 'close'].forEach(function (t) {
            var HK = Symbol(t);
            Object.defineProperty(CWC.prototype, 'on' + t, { enumerable: true, configurable: true,
                get: { get: function () { st(this); var h = this[HK]; return h ? h.fn : null; } }.get,
                set: { set: function (v) {
                    st(this);
                    var h = this[HK];
                    if (h) { this.removeEventListener(t, h.wrap); this[HK] = null; }
                    if (typeof v === 'function') { var fn = v, wrap = function (e) { return fn.call(this, e); }; this[HK] = { fn: fn, wrap: wrap }; this.addEventListener(t, wrap); }
                } }.set });
        });
        Object.defineProperty(global, 'CloseWatcher', { value: CWC, writable: true, configurable: true, enumerable: false });
    })();

    /* ---- Document.parseHTMLUnsafe (HTML 8.5): a new HTML document from markup; MessageEvent's initMessageEvent ---- */
    (function () {
        var D = global.Document;
        if (D && typeof D.parseHTMLUnsafe !== 'function' && typeof global.DOMParser === 'function') {
            Object.defineProperty(D, 'parseHTMLUnsafe', { configurable: true, enumerable: true, writable: true, value: function parseHTMLUnsafe(html) {
                if (arguments.length < 1) throw new TypeError("Failed to execute 'parseHTMLUnsafe' on 'Document': 1 argument required, but only 0 present.");
                return new global.DOMParser().parseFromString(String(html), 'text/html');
            } });
        }
        var ME = global.MessageEvent;
        if (ME && ME.prototype && typeof ME.prototype.initMessageEvent !== 'function') {
            Object.defineProperty(ME.prototype, 'initMessageEvent', { configurable: true, enumerable: true, writable: true, value: function initMessageEvent(type, bubbles, cancelable, data, origin, lastEventId, source, ports) {
                if (arguments.length < 1) throw new TypeError("Failed to execute 'initMessageEvent' on 'MessageEvent': 1 argument required, but only 0 present.");
                if (typeof this.initEvent === 'function') this.initEvent(String(type), !!bubbles, !!cancelable);
                var set = function (o, k, v) { try { Object.defineProperty(o, k, { value: v, configurable: true, enumerable: true }); } catch (e) { } };
                set(this, 'data', data === undefined ? null : data);
                set(this, 'origin', origin === undefined ? '' : String(origin));
                set(this, 'lastEventId', lastEventId === undefined ? '' : String(lastEventId));
                set(this, 'source', source === undefined ? null : source);
                set(this, 'ports', Object.freeze(Array.isArray(ports) ? ports.slice() : []));
            } });
            Object.defineProperty(ME.prototype.initMessageEvent, 'length', { value: 1 });
        }
    })();
    /* ---- a URL's query in the document's encoding (HTML's encoding-parse a URL): XMLHttpRequest's open, EventSource,
     * history's pushState and replaceState ---- */
    (function () {
        function legacy(u) {
            if (typeof u !== 'string' || !/[^\x00-\x7f]/.test(u) || typeof global.__cyberLegacyQuery !== 'function') return u;
            var d = global.document, cs = d && d.characterSet;
            if (!cs || /^utf-8$/i.test(cs)) return u;
            var r = global.__cyberLegacyQuery(u, cs);
            return r || u;
        }
        function wrapArg(proto, name, at) {
            var d = proto && Object.getOwnPropertyDescriptor(proto, name);
            if (!d || typeof d.value !== 'function' || !d.configurable) return;
            var f = d.value;
            var w = { m: function () {
                if (arguments.length > at && typeof arguments[at] === 'string') {
                    var lu = legacy(arguments[at]);
                    if (lu !== arguments[at]) { var args = Array.prototype.slice.call(arguments); args[at] = lu; return f.apply(this, args); }
                }
                return f.apply(this, arguments);
            } }.m;
            try { Object.defineProperty(w, 'name', { value: f.name }); Object.defineProperty(w, 'length', { value: f.length }); } catch (e) { }
            Object.defineProperty(proto, name, { value: w, writable: d.writable, enumerable: d.enumerable, configurable: true });
        }
        try { wrapArg(global.XMLHttpRequest && global.XMLHttpRequest.prototype, 'open', 1); } catch (e) { }
        try { wrapArg(global.History && global.History.prototype, 'pushState', 2); } catch (e) { }
        try { wrapArg(global.History && global.History.prototype, 'replaceState', 2); } catch (e) { }
    })();
    /* ---- autofocus (HTML 6.6.7): at each rendering update while the document loads, the first [autofocus] focusable
     * element focused, unless something has focus already; done once one is focused (the autofocus processed flag) ---- */
    (function () {
        var processed = false, loaded = false, after = 0;
        function flush() {
            if (processed || global.__cyberAutofocusDone) return;
            /* (the top document's and its children's, not deeper frames') */
            try { if (global !== global.top && global.parent !== global.top) return; } catch (e) { return; }
            var d = global.document;
            if (!d || typeof global.requestAnimationFrame !== 'function') return;
            var ae = d.activeElement;
            if (ae && ae !== d.body && ae !== d.documentElement) { processed = true; return; }
            var c = d.querySelector('[autofocus]');
            var fa = global.__cyberFocusableArea;
            if (c && (typeof fa !== 'function' || fa(c))) {
                processed = true;
                try { c.focus(); } catch (e) { }
                return;
            }
            /* (none yet: tried again at the next update, until a little past the load event) */
            if (loaded && ++after > 2) return;
            global.requestAnimationFrame(flush);
        }
        onWin('DOMContentLoaded', function () { if (typeof global.requestAnimationFrame === 'function') global.requestAnimationFrame(flush); });
        onWin('load', function () { loaded = true; });
    })();
    /* ---- focusGroupStart (focusgroup, tentative): the focusgroupstart attribute's presence, on HTML, SVG and MathML elements ---- */
    ['HTMLElement', 'SVGElement', 'MathMLElement'].forEach(function (n) {
        var P = global[n] && global[n].prototype;
        if (!P || Object.getOwnPropertyDescriptor(P, 'focusGroupStart')) return;
        Object.defineProperty(P, 'focusGroupStart', { configurable: true, enumerable: true,
            get: function focusGroupStart() { return this.hasAttribute('focusgroupstart'); },
            set: function focusGroupStart(v) { if (v) this.setAttribute('focusgroupstart', ''); else this.removeAttribute('focusgroupstart'); } });
    });
    /* ---- interest invokers (interestfor, tentative): interestForElement, InterestEvent, interest gained on focus or hover
     * and lost on blur, leaving or Escape after the interest-delay times; :interest-source / :interest-target by
     * data-cyber-interest-source / -target, which the selector engines read ---- */
    (function () {
        var EL = global.Element;
        if (!EL) return;
        var REF = Symbol.for('cyber.interestForElement');
        function rootChainHas(el, root) {
            for (var r = el.getRootNode(); r; r = r.host ? r.host.getRootNode() : null) { if (r === root) return true; if (!r.host) break; }
            return false;
        }
        function targetOf(b) {
            var id = b.getAttribute('interestfor');
            if (id === null) return null;
            var t = b[REF];
            if (t !== undefined && id === '') return t && t.isConnected && rootChainHas(b, t.getRootNode()) ? t : null;
            if (!id) return null;
            var root = b.getRootNode();
            return root && root.getElementById ? root.getElementById(id) : null;
        }
        ['HTMLButtonElement', 'HTMLAnchorElement', 'HTMLAreaElement', 'SVGAElement'].forEach(function (n) {
            var P = global[n] && global[n].prototype;
            if (!P || Object.getOwnPropertyDescriptor(P, 'interestForElement')) return;
            Object.defineProperty(P, 'interestForElement', { configurable: true, enumerable: true,
                get: function interestForElement() { return targetOf(this); },
                set: function interestForElement(v) {
                    if (v !== null && v !== undefined && !(v instanceof EL)) throw new TypeError("Failed to set the 'interestForElement' property: Failed to convert value to 'Element'.");
                    Object.defineProperty(this, REF, { value: v || null, configurable: true, writable: true });
                    if (!v) this.removeAttribute('interestfor'); else this.setAttribute('interestfor', '');
                } });
        });
        var INV = 'interestfor';
        function invokerOf(n) {
            for (var e = n, d = 0; e && d < 64; d++) {
                if (e.nodeType === 1 && e.hasAttribute && e.hasAttribute(INV)) {
                    var ln = e.localName;
                    if (ln === 'button' || ln === 'a' || ln === 'area' || ln === 'menuitem') return e;
                }
                e = e.parentNode || (e.host || null);
            }
            return null;
        }
        function delayOf(el, which) {
            var v = '';
            try { v = global.getComputedStyle(el).getPropertyValue('interest-delay-' + which).trim(); } catch (e) { }
            var m = /^(-?[\d.]+)(ms|s)$/.exec(v);
            if (!m) return which === 'start' ? 500 : 250;
            return Math.max(0, Number(m[1]) * (m[2] === 's' ? 1000 : 1));
        }
        var current = [];   /* ([invoker, target]) with interest now */
        var timers = new Map();
        function setTimer(el, kind, ms, fn) {
            var k = timers.get(el);
            if (k) global.clearTimeout(k.id);
            var id = global.setTimeout(function () { timers.delete(el); fn(); }, ms);
            timers.set(el, { id: id, kind: kind });
        }
        function clearTimer(el, kind) { var k = timers.get(el); if (k && (!kind || k.kind === kind)) { global.clearTimeout(k.id); timers.delete(el); } }
        function hasInterest(inv) { for (var i = 0; i < current.length; i++) if (current[i][0] === inv) return current[i]; return null; }
        function fireI(t, type, inv, notCancelable) {
            var ev;
            try { ev = new global.InterestEvent(type, { cancelable: !notCancelable, source: inv }); } catch (e) { return true; }
            return t.dispatchEvent(ev);
        }
        function gain(inv) {
            if (hasInterest(inv)) return;
            var t = targetOf(inv);
            if (!t || !t.isConnected) return;
            if (!fireI(t, 'interest', inv)) return;
            current.push([inv, t]);
            inv.setAttribute('data-cyber-interest-source', '');
            t.setAttribute('data-cyber-interest-target', '');
            if (t.hasAttribute('popover') && !t.matches(':popover-open')) { try { t.showPopover({ source: inv }); } catch (e) { } }
        }
        function lose(inv, force, notCancelable) {
            var c = hasInterest(inv);
            if (!c) return;
            if (!fireI(c[1], 'loseinterest', inv, notCancelable) && !force) return;
            current.splice(current.indexOf(c), 1);
            inv.removeAttribute('data-cyber-interest-source');
            var still = false;
            for (var i = 0; i < current.length; i++) if (current[i][1] === c[1]) still = true;
            if (!still) { c[1].removeAttribute('data-cyber-interest-target'); c[1].removeAttribute('data-cyber-interest-pseudo'); }
            if (c[1].hasAttribute('popover') && c[1].matches(':popover-open')) { try { c[1].hidePopover(); } catch (e) { } }
        }
        function within(n, c) {
            for (var e = n, d = 0; e && d < 64; d++) { if (e === c[0] || e === c[1]) return true; e = e.parentNode || (e.host || null); }
            return false;
        }
        onWin('focusin', function (e) {
            var inv = invokerOf(e.target);
            for (var i = 0; i < current.length; i++) if (within(e.target, current[i])) clearTimer(current[i][0], 'lose');
            if (!inv) return;
            if (hasInterest(inv)) { clearTimer(inv, 'lose'); return; }
            var ms = delayOf(inv, 'start');
            setTimer(inv, 'gain', ms, function () { gain(inv); });
        }, true);
        onWin('focusout', function (e) {
            var inv = invokerOf(e.target);
            if (inv) clearTimer(inv, 'gain');
            current.slice().forEach(function (c) {
                if (!within(e.target, c) || (e.relatedTarget && within(e.relatedTarget, c))) return;
                var ms = delayOf(c[0], 'end');
                setTimer(c[0], 'lose', ms, function () { lose(c[0]); });
            });
        }, true);
        onWin('mouseover', function (e) {
            var inv = invokerOf(e.target);
            for (var i = 0; i < current.length; i++) if (within(e.target, current[i])) clearTimer(current[i][0], 'lose');
            if (!inv || hasInterest(inv)) return;
            setTimer(inv, 'gain', delayOf(inv, 'start'), function () { gain(inv); });
        }, true);
        onWin('mouseout', function (e) {
            var inv = invokerOf(e.target);
            if (inv && !(e.relatedTarget && within(e.relatedTarget, [inv, inv]))) clearTimer(inv, 'gain');
            current.slice().forEach(function (c) {
                if (!within(e.target, c) || (e.relatedTarget && within(e.relatedTarget, c))) return;
                setTimer(c[0], 'lose', delayOf(c[0], 'end'), function () { lose(c[0]); });
            });
        }, true);
        /* (its ::interest-button activated, a click or a tap on it: interest shown now) */
        onWin('click', function (e) {
            if (typeof global.__cyberScrollPseudoAt !== 'function' || typeof e.clientX !== 'number') return;
            /* (only a page with interest invokers: the hit test lays the page out) */
            try { if (!global.document || !global.document.querySelector('[interestfor]')) return; } catch (x) { return; }
            var hit = null;
            try { hit = global.__cyberScrollPseudoAt(e.clientX, e.clientY); } catch (x) { }
            if (!hit || hit[1] !== 43 || !hit[0]) return;
            var inv = invokerOf(hit[0]);
            if (!inv) return;
            clearTimer(inv);
            /* (anchored to the ::interest-button from now on, shown already or not) */
            var tg = targetOf(inv);
            if (tg) tg.setAttribute('data-cyber-interest-pseudo', '');
            gain(inv);
        }, true);
        /* (a target popover closing: its invokers lose interest, unless that is canceled) */
        onWin('beforetoggle', function (e) {
            if (e.newState !== 'closed') return;
            var t = e.target;
            current.slice().forEach(function (c) {
                if (c[1] !== t) return;
                clearTimer(c[0]);
                if (!fireI(t, 'loseinterest', c[0])) return;
                var ci = current.indexOf(c);
                if (ci >= 0) current.splice(ci, 1);
                c[0].removeAttribute('data-cyber-interest-source');
                var still = false;
                for (var k = 0; k < current.length; k++) if (current[k][1] === t) still = true;
                if (!still) t.removeAttribute('data-cyber-interest-target');
            });
        }, true);
        onWin('keydown', function (e) {
            if (e.key !== 'Escape' || e.defaultPrevented) return;
            current.slice().reverse().forEach(function (c) { clearTimer(c[0]); lose(c[0], true, true); });
        }, true);
    })();
    /* ---- ImageBitmaps in a postMessage's transfer list: detached once sent (width and height 0) ---- */
    (function () {
        function detachAfter(proto) {
            var d = proto && Object.getOwnPropertyDescriptor(proto, 'postMessage');
            if (!d || typeof d.value !== 'function' || !d.configurable) return;
            var f = d.value;
            var w = { postMessage: function (msg, opts) {
                var r = f.apply(this, arguments);
                var list = Array.isArray(opts) ? opts : (opts && typeof opts === 'object' && Array.isArray(opts.transfer) ? opts.transfer : null);
                if (list && global.ImageBitmap) for (var i = 0; i < list.length; i++) {
                    if (list[i] instanceof global.ImageBitmap && typeof list[i].close === 'function') { try { list[i].close(); } catch (e) { } }
                }
                return r;
            } }.postMessage;
            try { Object.defineProperty(w, 'length', { value: f.length }); } catch (e) { }
            Object.defineProperty(proto, 'postMessage', { value: w, writable: d.writable, enumerable: d.enumerable, configurable: true });
        }
        try { detachAfter(global.Worker && global.Worker.prototype); } catch (e) { }
        try { detachAfter(global.MessagePort && global.MessagePort.prototype); } catch (e) { }
    })();
})

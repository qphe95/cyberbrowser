/*
 * form_controls.js - the IDL attributes of the form controls (HTML section
 * 4.10): input, textarea, select, option, button, form, label, fieldset.
 * Installed in each realm by platform_api_install (browser/platform_api.c).
 *
 * A control's value and checkedness are state of their own, initialized
 * from (and, until script or the user changes them, following) the content
 * attributes: the "dirty value flag" and "dirty checkedness flag".  That
 * state is kept under a symbol shared by all realms.
 * Called as fn(global, window).
 */
(function (global, win) {
    'use strict';
    var STATE = Symbol.for('cyber.formState');
    function proto(name) { var C = global[name]; return C && C.prototype; }
    function accessor(P, name, get, set) {
        if (!P) return;
        /* (no setter given: a readonly attribute, as WebIDL has it) */
        Object.defineProperty(P, name, { get: get, set: set, configurable: true, enumerable: true });
    }
    function method(P, name, fn) {
        if (P) Object.defineProperty(P, name, { value: fn, writable: true, configurable: true });
    }
    function state(el) {
        var s = el[STATE];
        if (!s) {
            s = { dirtyValue: false, value: '', dirtyChecked: false, checked: false,
                  dirtySelected: false, selected: false, selStart: 0, selEnd: 0, selDir: 'none' };
            Object.defineProperty(el, STATE, { value: s, configurable: true });
            if (el.localName === 'input') s.lastType = inputType(el);
        } else if (s.lastType !== undefined && el.localName === 'input') typeChanged(el, s);
        return s;
    }
    /* (an input's type attribute changed since last seen: HTML 4.10.5's steps for it, done now) */
    function typeChanged(el, s) {
        var p = s.lastType, n = inputType(el);
        if (p === n) return;
        s.lastType = n;
        var pm = valueMode(p), nm = valueMode(n);
        if (pm === 'value' && (nm === 'default' || nm === 'default/on')) {
            var v = s.dirtyValue ? s.value : sanitize(p, el.getAttributeNS(null, 'value') || '', el);
            if (v !== '') el.setAttribute('value', v);
            s.dirtyValue = false;
        } else if (pm !== 'value' && nm === 'value') {
            s.value = el.getAttributeNS(null, 'value') || ''; s.dirtyValue = false;
        } else if (pm !== 'filename' && nm === 'filename') {
            s.value = ''; s.dirtyValue = false;
        }
        if (nm === 'value' && s.dirtyValue) s.value = sanitize(n, s.value, el);
        var sel = /^(text|search|tel|url|password)$/;
        if (!sel.test(p) && sel.test(n)) { s.selStart = s.selEnd = 0; s.selDir = 'none'; }
    }
    function reflectString(P, prop, attr) {
        attr = attr || prop.toLowerCase();
        accessor(P, prop, function () { var v = this.getAttribute(attr); return v === null ? '' : v; },
                 function (v) { this.setAttribute(attr, String(v)); });
    }
    function reflectBool(P, prop, attr) {
        attr = attr || prop.toLowerCase();
        accessor(P, prop, function () { return this.hasAttribute(attr); },
                 function (v) { if (v) this.setAttribute(attr, ''); else this.removeAttribute(attr); });
    }
    function reflectInt(P, prop, attr, dflt) {
        attr = attr || prop.toLowerCase();
        accessor(P, prop, function () {
            var v = parseInt(this.getAttribute(attr), 10);
            return isNaN(v) ? dflt : v;
        }, function (v) { this.setAttribute(attr, String(v | 0)); });
    }
    function enumerated(P, prop, attr, values, missing, invalid) {
        accessor(P, prop, function () {
            var v = this.getAttribute(attr);
            if (v === null) return missing;
            v = v.toLowerCase();
            return values.indexOf(v) >= 0 ? v : invalid;
        }, function (v) { this.setAttribute(attr, String(v)); });
    }
    function textOf(el) {
        var t = el.textContent;
        return t == null ? '' : String(t);
    }
    function stripNewlines(v) { return v.replace(/[\r\n]/g, ''); }
    function formOwner(el) {
        var id = el.getAttribute && el.getAttribute('form');
        /* (form="": the element so identified in its tree, when connected; a disconnected one, its ancestor) */
        if (id !== null && id !== undefined && el.isConnected) {
            var r = el.getRootNode(), f = r && r.getElementById ? r.getElementById(id) : null;
            return f && f.localName === 'form' ? f : null;
        }
        for (var n = el.parentNode; n; n = n.parentNode) if (n.localName === 'form') return n;
        return null;
    }
    function rootOf(n) { while (n.parentNode) n = n.parentNode; return n; }
    /* (cloning steps, HTML 4.10.5 / 4.10.11 / 4.10.10.x: a copy takes a form control's value, checkedness
     * and their dirty flags; an option its selectedness) */
    (function () {
        var NP = global.Node && global.Node.prototype;
        if (!NP || !NP.cloneNode) return;
        function copyState(src, dst) {
            if (src.nodeType !== 1) return;
            var s = src[STATE];
            if (s && (src.localName === 'input' || src.localName === 'textarea' || src.localName === 'option')) {
                var d = state(dst);
                d.dirtyValue = s.dirtyValue; d.value = s.value; d.dirtyChecked = s.dirtyChecked; d.checked = s.checked;
                d.indeterminate = s.indeterminate; d.dirtySelected = s.dirtySelected; d.selected = s.selected;
                if (src.localName === 'input' && s.dirtyChecked) syncChecked(dst, d);
                if (s.dirtyValue && global.__cyberLiveValue && src.localName !== 'option') global.__cyberLiveValue(dst, d.value);
            }
        }
        function walk(a, b) {
            copyState(a, b);
            for (var x = a.firstChild, y = b.firstChild; x && y; x = x.nextSibling, y = y.nextSibling) walk(x, y);
        }
        var clone = NP.cloneNode;
        Object.defineProperty(NP, 'cloneNode', { configurable: true, writable: true, enumerable: true, value: function cloneNode() {
            var c = clone.apply(this, arguments);
            try { if (c && this.nodeType === 1 || this.nodeType === 11) walk(this, c); } catch (e) { }
            return c;
        } });
        var DP = global.Document && global.Document.prototype, imp = DP && DP.importNode;
        if (imp) Object.defineProperty(DP, 'importNode', { configurable: true, writable: true, enumerable: true, value: function importNode(n) {
            var c = imp.apply(this, arguments);
            try { if (n && c) walk(n, c); } catch (e) { }
            return c;
        } });
    })();

    /* ---- input ---- */
    var IP = proto('HTMLInputElement');
    var TYPES = ['hidden', 'text', 'search', 'tel', 'url', 'email', 'password', 'date', 'month', 'week',
                 'time', 'datetime-local', 'number', 'range', 'color', 'checkbox', 'radio', 'file',
                 'submit', 'image', 'reset', 'button'];
    function inputType(el) {
        var v = el.getAttributeNS(null, 'type');
        if (v === null) return 'text';
        v = v.replace(/[A-Z]/g, function (c) { return c.toLowerCase(); });   /* (ASCII only: the Kelvin sign is no k) */
        return TYPES.indexOf(v) >= 0 ? v : 'text';
    }
    /* the value mode of a type (HTML 4.10.5.1) */
    function valueMode(type) {
        if (type === 'hidden' || type === 'submit' || type === 'image' || type === 'reset' || type === 'button') return 'default';
        if (type === 'checkbox' || type === 'radio') return 'default/on';
        if (type === 'file') return 'filename';
        return 'value';
    }
    /* ---- dates and times (HTML 2.3.5): parsed to parts, or null ---- */
    function daysIn(y, m) { return m === 2 ? ((y % 4 === 0 && y % 100 !== 0) || y % 400 === 0 ? 29 : 28) : [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][m - 1]; }
    function weeksIn(y) {
        var d = new Date(Date.UTC(2000, 0, 1)); d.setUTCFullYear(y); var dow = d.getUTCDay();
        var leap = (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0;
        return dow === 4 || (leap && dow === 3) ? 53 : 52;
    }
    function pDate(s) {
        var m = /^(\d{4,})-(\d\d)-(\d\d)$/.exec(s); if (!m) return null;
        var y = +m[1], mo = +m[2], d = +m[3];
        return y > 0 && mo >= 1 && mo <= 12 && d >= 1 && d <= daysIn(y, mo) ? { y: y, m: mo, d: d } : null;
    }
    function pMonth(s) { var m = /^(\d{4,})-(\d\d)$/.exec(s); if (!m) return null; var y = +m[1], mo = +m[2]; return y > 0 && mo >= 1 && mo <= 12 ? { y: y, m: mo } : null; }
    function pWeek(s) { var m = /^(\d{4,})-W(\d\d)$/.exec(s); if (!m) return null; var y = +m[1], w = +m[2]; return y > 0 && w >= 1 && w <= weeksIn(y) ? { y: y, w: w } : null; }
    function pTime(s) {
        var m = /^(\d\d):(\d\d)(?::(\d\d)(?:\.(\d{1,3}))?)?$/.exec(s); if (!m) return null;
        var h = +m[1], mi = +m[2], se = m[3] ? +m[3] : 0, ms = m[4] ? +(m[4] + '00').slice(0, 3) : 0;
        return h < 24 && mi < 60 && se < 60 ? { ms: ((h * 60 + mi) * 60 + se) * 1000 + ms } : null;
    }
    function pDateTime(s) { var m = /^(.+?)[T ](.+)$/.exec(s); if (!m) return null; var d = pDate(m[1]), t = pTime(m[2]); return d && t ? { d: d, t: t } : null; }
    function pad(n, w) { var s = String(n); while (s.length < w) s = '0' + s; return s; }
    function fTime(ms) {
        var h = Math.floor(ms / 3600000), mi = Math.floor(ms / 60000) % 60, se = Math.floor(ms / 1000) % 60, f = ms % 1000;
        var s = pad(h, 2) + ':' + pad(mi, 2);
        if (se || f) s += ':' + pad(se, 2);
        if (f) s += ('.' + pad(f, 3)).replace(/0+$/, '');
        return s;
    }
    function fDate(y, m, d) { return pad(y, 4) + '-' + pad(m, 2) + '-' + pad(d, 2); }
    function utcDays(d) { var x = new Date(Date.UTC(2000, d.m - 1, d.d)); x.setUTCFullYear(d.y); return x.getTime(); }
    /* (a type's value as a number: ms, months, or the number; NaN none) */
    function toNumber(type, v) {
        var p;
        switch (type) {
        case 'date': p = pDate(v); return p ? utcDays(p) : NaN;
        case 'month': p = pMonth(v); return p ? (p.y - 1970) * 12 + (p.m - 1) : NaN;
        case 'week': {
            p = pWeek(v); if (!p) return NaN;
            var j4 = new Date(Date.UTC(2000, 0, 4)); j4.setUTCFullYear(p.y);
            var mon = j4.getTime() - ((j4.getUTCDay() + 6) % 7) * 86400000;
            return mon + (p.w - 1) * 7 * 86400000;
        }
        case 'time': p = pTime(v); return p ? p.ms : NaN;
        case 'datetime-local': p = pDateTime(v); return p ? utcDays(p.d) + p.t.ms : NaN;
        case 'number': case 'range': return v === '' ? NaN : Number(v);
        }
        return NaN;
    }
    function fromNumber(type, n) {
        var d;
        switch (type) {
        case 'date': d = new Date(n); return fDate(d.getUTCFullYear(), d.getUTCMonth() + 1, d.getUTCDate());
        case 'month': { var y = 1970 + Math.floor(n / 12), m = ((n % 12) + 12) % 12 + 1; return pad(y, 4) + '-' + pad(m, 2); }
        case 'week': {
            d = new Date(n); var t = new Date(Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate()));
            var day = (t.getUTCDay() + 6) % 7; t.setUTCDate(t.getUTCDate() - day + 3);
            var wy = t.getUTCFullYear(), f = new Date(Date.UTC(wy, 0, 4));
            var wk = 1 + Math.round(((t - f) / 86400000 - 3 + ((f.getUTCDay() + 6) % 7)) / 7);
            return pad(wy, 4) + '-W' + pad(wk, 2);
        }
        case 'time': return fTime(((n % 86400000) + 86400000) % 86400000);
        case 'datetime-local': d = new Date(n); return fDate(d.getUTCFullYear(), d.getUTCMonth() + 1, d.getUTCDate()) + 'T' + fTime(((n % 86400000) + 86400000) % 86400000);
        }
        return String(n);
    }
    var DATEY = { date: 1, month: 1, week: 1, time: 1, 'datetime-local': 1 };
    /* ---- colors (HTML 4.10.5.1.15): a CSS color, in the input's color space, with alpha or not ---- */
    var colorCx = null;
    function srgbDec(c) { var a = Math.abs(c), s = c < 0 ? -1 : 1; return a <= 0.04045 ? c / 12.92 : s * Math.pow((a + 0.055) / 1.055, 2.4); }
    function srgbEnc(c) { var a = Math.abs(c), s = c < 0 ? -1 : 1; return a <= 0.0031308 ? c * 12.92 : s * (1.055 * Math.pow(a, 1 / 2.4) - 0.055); }
    function mat(m, v) { return [m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2]]; }
    var S2P = [0.8224621, 0.1775380, 0, 0.0331941, 0.9668058, 0, 0.0170827, 0.0723974, 0.9105199];
    var P2S = [1.2249401, -0.2249404, 0, -0.0420569, 1.0420571, 0, -0.0196376, -0.0786361, 1.0982735];
    function convertRGB(c, from, to) {
        if (from === to) return c;
        var lin = c.map(srgbDec);
        return mat(from === 'srgb' ? S2P : P2S, lin).map(srgbEnc);
    }
    /* (a color: { space, c: [r, g, b] in 0..1, a }, or null) */
    function parseCssColor(v) {
        v = String(v).replace(/^[\t\n\f\r ]+|[\t\n\f\r ]+$/g, '');
        var m = /^color\(\s*(srgb|display-p3)\s+([^\s/)]+)\s+([^\s/)]+)\s+([^\s/)]+)\s*(?:\/\s*([^\s)]+)\s*)?\)$/i.exec(v);
        if (m) {
            var num = function (x, pct) {
                if (/^none$/i.test(x)) return 0;
                var p = /^([-+]?(?:\d+\.?\d*|\.\d+)(?:e[-+]?\d+)?)(%?)$/i.exec(x);
                if (!p) return NaN;
                return p[2] ? +p[1] / 100 : +p[1];
            };
            var c = [num(m[2]), num(m[3]), num(m[4])], a = m[5] === undefined ? 1 : num(m[5]);
            if (c.some(isNaN) || isNaN(a)) return null;
            return { space: m[1].toLowerCase(), c: c, a: Math.min(1, Math.max(0, a)) };
        }
        if (/^(inherit|initial|unset|revert|revert-layer)$/i.test(v) || !v) return null;
        try {
            if (!colorCx) colorCx = global.document.createElement('canvas').getContext('2d');
            colorCx.fillStyle = '#010203'; colorCx.fillStyle = v; var x = colorCx.fillStyle;
            colorCx.fillStyle = '#030201'; colorCx.fillStyle = v; var y = colorCx.fillStyle;
            if (x !== y) return null;
            var h = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(x);
            if (h) return { space: 'srgb', c: [parseInt(h[1], 16) / 255, parseInt(h[2], 16) / 255, parseInt(h[3], 16) / 255], a: 1 };
            var r = /^rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*(?:,\s*([\d.e-]+)\s*)?\)$/i.exec(x);
            if (r) return { space: 'srgb', c: [r[1] / 255, r[2] / 255, r[3] / 255], a: r[4] === undefined ? 1 : Math.round(+r[4] * 255) / 255 };
        } catch (e) { }
        return null;
    }
    function fmtNum(n) { var s = (Math.round(n * 1e6) / 1e6).toFixed(6).replace(/\.?0+$/, ''); return s === '-0' ? '0' : s; }
    function sanitizeColor(v, el) {
        var space = el && String(el.getAttributeNS(null, 'colorspace') || '').toLowerCase() === 'display-p3' ? 'display-p3' : 'limited-srgb';
        var alpha = !!(el && el.hasAttributeNS(null, 'alpha'));
        var col = parseCssColor(v) || { space: 'srgb', c: [0, 0, 0], a: 1 };
        var a = alpha ? col.a : 1, c;
        if (space === 'limited-srgb') {
            c = convertRGB(col.c, col.space, 'srgb').map(function (x) { return Math.round(Math.min(1, Math.max(0, x)) * 255); });
            a = Math.round(a * 255) / 255;
            if (!alpha) return '#' + c.map(function (x) { return (x < 16 ? '0' : '') + x.toString(16); }).join('');
            return 'color(srgb ' + c.map(function (x) { return fmtNum(x / 255); }).join(' ') + (a < 1 ? ' / ' + fmtNum(a) : '') + ')';
        }
        c = convertRGB(col.c, col.space, 'display-p3');
        return 'color(display-p3 ' + c.map(fmtNum).join(' ') + (a < 1 ? ' / ' + fmtNum(a) : '') + ')';
    }
    function sanitize(type, v, el) {
        switch (type) {
        case 'text': case 'search': case 'tel': case 'password': return stripNewlines(v);
        case 'url': case 'email': return stripNewlines(v).replace(/^[\t\n\f\r ]+|[\t\n\f\r ]+$/g, '');
        case 'date': return pDate(v) ? v : '';
        case 'month': return pMonth(v) ? v : '';
        case 'week': return pWeek(v) ? v : '';
        case 'time': return pTime(v) ? v : '';
        case 'datetime-local': { var dt = pDateTime(v); return dt ? fDate(dt.d.y, dt.d.m, dt.d.d) + 'T' + fTime(dt.t.ms) : ''; }
        case 'number': return /^-?(\d+(\.\d+)?|\.\d+)([eE][-+]?\d+)?$/.test(v) && isFinite(Number(v)) ? v : '';
        case 'range': {
            /* (a number in [min, max], on a step: the middle when none is given) */
            var pf = function (x) { var m = /^[\t\n\f\r ]*(-?(?:\d+(?:\.\d+)?|\.\d+)(?:[eE][-+]?\d+)?)/.exec(x || ''); return m && isFinite(+m[1]) ? +m[1] : null; };
            var mn = el ? pf(el.getAttribute('min')) : null, mx = el ? pf(el.getAttribute('max')) : null;
            if (mn === null) mn = 0;
            if (mx === null) mx = 100;
            if (mx < mn) mx = mn;
            var n = /^-?(\d+(\.\d+)?|\.\d+)([eE][-+]?\d+)?$/.test(v) ? Number(v) : NaN;
            if (!isFinite(n)) n = mx < mn ? mn : mn + (mx - mn) / 2;
            if (n < mn) n = mn;
            if (n > mx) n = mx;
            var sa = el ? el.getAttribute('step') : null, step = 1;
            if (sa !== null && String(sa).toLowerCase() === 'any') step = 0;
            else { var sp = pf(sa); if (sp !== null && sp > 0) step = sp; }
            if (step) {
                var base = el && pf(el.getAttribute('min')) !== null ? mn : (el && pf(el.getAttribute('value')) !== null && false ? 0 : mn);
                var k = Math.round((n - base) / step);
                var c = base + k * step;
                if (c > mx) c = base + Math.floor((mx - base) / step) * step;
                if (c < mn) c = base + Math.ceil((mn - base) / step) * step;
                n = +c.toPrecision(15);
            }
            return String(n);
        }
        case 'color': return sanitizeColor(v, el);
        default: return v;
        }
    }
    if (IP) {
        accessor(IP, 'type', function () { return inputType(this); },
                 function (v) { this.setAttribute('type', String(v)); });
        accessor(IP, 'value', function () {
            var t = inputType(this), mode = valueMode(t), s = state(this);
            if (mode === 'value') return s.dirtyValue ? (t === 'color' ? sanitize(t, s.value, this) : s.value) : sanitize(t, this.getAttribute('value') || '', this);
            if (mode === 'default/on') { var a = this.getAttribute('value'); return a === null ? 'on' : a; }
            if (mode === 'filename') return '';
            var d = this.getAttribute('value'); return d === null ? '' : d;
        }, function (v) {
            v = String(v);
            var t = inputType(this), mode = valueMode(t), s = state(this);
            if (mode === 'value') {
                var old = this.value;
                s.value = sanitize(t, v, this); s.dirtyValue = true;
                /* (a new value: the cursor at its end; the same one, the selection kept) */
                if (s.value !== old) { s.selStart = s.selEnd = s.value.length; s.selDir = 'none'; }
                if (global.__cyberLiveValue) global.__cyberLiveValue(this, s.value);   /* (painted as set) */
            } else if (mode === 'filename') {
                if (v !== '') throw new global.DOMException("Failed to set the 'value' property on 'HTMLInputElement': This input element accepts a filename, which may only be programmatically set to the empty string.", 'InvalidStateError');
            } else {
                this.setAttribute('value', v);
            }
        });
        reflectString(IP, 'defaultValue', 'value');
        accessor(IP, 'checked', function () {
            var s = state(this);
            return s.dirtyChecked ? s.checked : this.hasAttribute('checked');
        }, function (v) {
            var s = state(this);
            s.checked = !!v; s.dirtyChecked = true;
            syncChecked(this, s);
            if (s.checked && inputType(this) === 'radio') uncheckGroup(this);
        });
        reflectBool(IP, 'defaultChecked', 'checked');
        accessor(IP, 'indeterminate', function () { return !!state(this).indeterminate; },
                 function (v) { state(this).indeterminate = !!v; global.__cyberElState(this, 256, !!v); });
        ['name', 'placeholder', 'autocomplete', 'min', 'max', 'step', 'pattern', 'accept', 'alt', 'src',
         'inputMode', 'enterKeyHint', 'dirName', 'formAction', 'formMethod', 'formTarget'].forEach(function (p) {
            reflectString(IP, p);
        });
        ['disabled', 'required', 'multiple', 'autofocus', 'formNoValidate'].forEach(function (p) { reflectBool(IP, p); });
        reflectBool(IP, 'readOnly', 'readonly');
        reflectBool(IP, 'alpha', 'alpha');
        accessor(IP, 'colorSpace', function () {
            return String(this.getAttributeNS(null, 'colorspace') || '').toLowerCase() === 'display-p3' ? 'display-p3' : 'limited-srgb';
        }, function (v) { this.setAttribute('colorspace', String(v)); });
        reflectInt(IP, 'maxLength', 'maxlength', -1);
        reflectInt(IP, 'minLength', 'minlength', -1);
        reflectInt(IP, 'size', 'size', 20);
        accessor(IP, 'form', function () { return formOwner(this); });
        /* (type=image: its image, as an img element's: load or error at it) */
        /* (an SVG data: URL's own size: its width, height and viewBox, CSS px; nulls for what it lacks) */
        var svgSize = function (u) {
            var m = /^data:image\/svg\+xml(;[^,]*)?,(.*)$/i.exec(u);
            if (!m) return null;
            var t = m[2];
            try { t = /;base64/i.test(m[1] || '') ? global.atob(t) : decodeURIComponent(t); } catch (e) { return null; }
            var root = /<svg\b([^>]*)>/i.exec(t);
            if (!root) return null;
            var attr = function (n) { var r = new RegExp('\\b' + n + '\\s*=\\s*["\']([^"\']*)["\']', 'i').exec(root[1]); return r ? r[1] : null; };
            var len = function (v) { if (v === null || /%/.test(v)) return null; var x = parseFloat(v); return isFinite(x) && x >= 0 ? x : null; };
            var vb = attr('viewBox'), ratio = null;
            if (vb) { var p = vb.trim().split(/[\s,]+/).map(Number); if (p.length === 4 && p[2] > 0 && p[3] > 0) ratio = p[2] / p[3]; }
            return { w: len(attr('width')), h: len(attr('height')), ratio: ratio };
        };
        /* (type=image: its image, as an img element's: load or error at it, its natural size kept) */
        var inputImage = function (el) {
            var s = state(el);
            if (inputType(el) !== 'image' || !global.Image) { s.image = null; s.nat = null; return; }
            var src = el.getAttribute('src');
            if (!src) { s.image = null; s.nat = null; return; }
            var img = new global.Image();
            s.image = img; s.nat = null;
            var E = global.Event;
            img.onload = function () {
                if (s.image !== img) return;
                var sv = svgSize(img.src), w = img.naturalWidth, h = img.naturalHeight, ratio = w && h ? w / h : null;
                if (sv) { w = sv.w; h = sv.h; ratio = w && h ? w / h : sv.ratio; }
                /* (the default sizing algorithm, a 300 x 150 default object size: what it lacks filled in) */
                if (w === null && h === null) { if (ratio) { w = 300; h = 300 / ratio; } else { w = 300; h = 150; } }
                else if (w === null) w = ratio ? h * ratio : 300;
                else if (h === null) h = ratio ? w / ratio : 150;
                s.nat = { w: w, h: h, ratio: ratio };
                el.dispatchEvent(new E('load'));
            };
            img.onerror = function () { if (s.image === img) el.dispatchEvent(new E('error')); };
            img.src = src;
        };
        Object.defineProperty(global, '__cyberInputImage', { configurable: true, value: inputImage });
        accessor(IP, 'src', function () {
            var v = this.getAttribute('src');
            if (v === null) return '';
            try { return new global.URL(v, this.baseURI).href; } catch (e) { return v; }
        }, function (v) { this.setAttribute('src', String(v)); });
        /* (HTML 2.3.4.2: rules for parsing dimension values, truncated to an integer; null an error) */
        var dimAttr = function (el, n) {
            var v = el.getAttribute(n);
            if (v === null) return null;
            var m = /^[\t\n\f\r ]*([+-]?)(\d+(?:\.\d*)?|\.\d+)/.exec(v);
            if (!m) return null;
            /* (a minus sign an error only when the integer is negative) */
            var x = Math.trunc((m[1] === '-' ? -1 : 1) * Number(m[2]));
            return x < 0 ? null : x + 0;
        };
        /* (rendered: its box's size, the attributes as the width and height properties with the natural ratio; else the
         * attributes, or the natural size) */
        var inputDims = function (el) {
            if (inputType(el) !== 'image') return [0, 0];
            var s = state(el), nat = s.nat, aw = dimAttr(el, 'width'), ah = dimAttr(el, 'height');
            var rendered = false;
            try { rendered = el.isConnected && el.getClientRects().length > 0; } catch (e) { }
            if (rendered && nat) {
                if (aw !== null && ah !== null) return [aw, ah];
                if (aw !== null) return [aw, Math.round(nat.ratio ? aw / nat.ratio : nat.h)];
                if (ah !== null) return [Math.round(nat.ratio ? ah * nat.ratio : nat.w), ah];
                return [Math.round(nat.w), Math.round(nat.h)];
            }
            return [aw !== null ? aw : nat ? Math.round(nat.w) : 0, ah !== null ? ah : nat ? Math.round(nat.h) : 0];
        };
        accessor(IP, 'width', function () { return inputDims(this)[0]; }, function (v) { this.setAttribute('width', String(v >>> 0)); });
        /* (hspace and vspace: unsigned longs reflected, HTML 16.3 obsolete features) */
        ['hspace', 'vspace'].forEach(function (k) {
            if (Object.getOwnPropertyDescriptor(IP, k)) return;
            accessor(IP, k, function () { var m = /^[\t\n\f\r ]*\+?(\d+)/.exec(this.getAttribute(k) || ''); var n = m ? Number(m[1]) : 0; return n <= 2147483647 ? n : 0; },
                     function (v) { var n = Number(v) >>> 0; this.setAttribute(k, String(n > 2147483647 ? 0 : n)); });
        });
        accessor(IP, 'height', function () { return inputDims(this)[1]; }, function (v) { this.setAttribute('height', String(v >>> 0)); });
        accessor(IP, 'valueAsNumber', function () {
            var t = inputType(this);
            return DATEY[t] || t === 'number' || t === 'range' ? toNumber(t, this.value) : NaN;
        }, function (n) {
            var t = inputType(this);
            if (!(DATEY[t] || t === 'number' || t === 'range')) throw new global.DOMException("Failed to set the 'valueAsNumber' property on 'HTMLInputElement': This input element does not support Number values.", 'InvalidStateError');
            n = Number(n);
            if (n === Infinity || n === -Infinity) throw new TypeError("Failed to set the 'valueAsNumber' property on 'HTMLInputElement': The value provided is infinite.");
            this.value = isNaN(n) ? '' : fromNumber(t, n);
        });
        accessor(IP, 'valueAsDate', function () {
            var t = inputType(this);
            if (!DATEY[t] || t === 'datetime-local') return null;
            var n = toNumber(t, this.value);
            if (isNaN(n)) return null;
            if (t === 'month') { var y = 1970 + Math.floor(n / 12), m = ((n % 12) + 12) % 12; var d = new Date(Date.UTC(2000, m, 1)); d.setUTCFullYear(y); return d; }
            return new Date(n);
        }, function (v) {
            var t = inputType(this);
            if (!DATEY[t] || t === 'datetime-local') throw new global.DOMException("Failed to set the 'valueAsDate' property on 'HTMLInputElement': This input element does not support Date values.", 'InvalidStateError');
            if (v !== null && !(v instanceof Date)) throw new TypeError("Failed to set the 'valueAsDate' property on 'HTMLInputElement': The provided value is not a Date.");
            if (v === null || isNaN(v.getTime())) { this.value = ''; return; }
            var ms = v.getTime();
            if (t === 'month') { this.value = pad(v.getUTCFullYear(), 4) + '-' + pad(v.getUTCMonth() + 1, 2); return; }
            this.value = fromNumber(t, ms);
        });
        accessor(IP, 'files', function () { return inputType(this) === 'file' ? [] : null; });
        accessor(IP, 'list', function () {
            var id = this.getAttribute('list'), d = this.ownerDocument;
            var l = id && d ? d.getElementById(id) : null;
            return l && l.localName === 'datalist' ? l : null;
        });
        textSelection(IP, function (el) { return /^(text|search|tel|url|password)$/.test(inputType(el)) ? el.value : null; });
        /* showPicker (HTML 4.10.5.1.x): a mutable element, with transient activation; the picker itself nothing here */
        Object.defineProperty(IP, 'showPicker', { configurable: true, writable: true, enumerable: true, value: function showPicker() {
            var t = inputType(this);
            var ro = this.hasAttribute('readonly') && /^(text|search|url|tel|email|password|date|month|week|time|datetime-local|number)$/.test(t);
            if (this.matches(':disabled') || ro) throw new global.DOMException("Failed to execute 'showPicker' on 'HTMLInputElement': HTMLInputElement::showPicker() cannot be used on immutable controls.", 'InvalidStateError');
            if (!(global.__cyberTransientActivation && global.__cyberTransientActivation())) throw new global.DOMException("Failed to execute 'showPicker' on 'HTMLInputElement': HTMLInputElement::showPicker() requires a user gesture.", 'NotAllowedError');
        } });
        method(IP, 'stepUp', function (n) { stepBy(this, n === undefined ? 1 : n); });
        method(IP, 'stepDown', function (n) { stepBy(this, -(n === undefined ? 1 : n)); });
    }
    /* stepUp / stepDown (HTML 4.10.5.4) */
    function stepBy(el, n) {
        var t = inputType(el);
        if (!STEP[t]) throw new global.DOMException("Failed to execute 'stepUp' on 'HTMLInputElement': This form element is not steppable.", 'InvalidStateError');
        var s = stepOf(el);
        if (!s) throw new global.DOMException("Failed to execute 'stepUp' on 'HTMLInputElement': This form element does not have an allowed value step.", 'InvalidStateError');
        n = Number(n);
        n = isFinite(n) ? Math.trunc(n) : 0;
        var mn = limitOf(el, 'min'), mx = limitOf(el, 'max');
        if (!isNaN(mn) && !isNaN(mx) && mn > mx) return;
        var cur = el.value === '' ? NaN : toNumber(t, el.value);
        var value = isNaN(cur) ? 0 : cur, before = value;
        var q = (value - s.base) / s.step;
        if (Math.abs(q - Math.round(q)) > 1e-9) value = s.base + (n < 0 ? Math.floor(q) : Math.ceil(q)) * s.step;
        else value += s.step * n;
        if (!isNaN(mn) && value < mn) value = s.base + Math.ceil((mn - s.base) / s.step) * s.step;
        if (!isNaN(mx) && value > mx) value = s.base + Math.floor((mx - s.base) / s.step) * s.step;
        if ((n > 0 && value < before) || (n < 0 && value > before)) return;
        value = +value.toPrecision(15);
        el.value = t === 'number' || t === 'range' ? String(value) : fromNumber(t, value);
    }
    /* (what selectors see: :checked) */
    function syncChecked(el, s) {
        global.__cyberElState(el, 128, s.dirtyChecked);
        global.__cyberElState(el, 64, s.dirtyChecked && s.checked);
    }
    function uncheckGroup(el) {
        var name = el.getAttribute('name');
        if (!name) return;
        var f = formOwner(el), root = f || rootOf(el);
        if (!root.querySelectorAll) return;
        var all = root.querySelectorAll('input');
        for (var i = 0; i < all.length; i++) {
            var r = all[i];
            if (r !== el && inputType(r) === 'radio' && r.getAttribute('name') === name && formOwner(r) === f) {
                var s = state(r); s.checked = false; s.dirtyChecked = true;
                syncChecked(r, s);
            }
        }
    }
    /* select(), selectionStart/End/Direction, setSelectionRange, setRangeText */
    /* Text field selection (HTML 4.10.18.6): where it applies, a start, an end and a direction; a change of it
     * queues a select event at the element. */
    function u32(v) { v = Number(v); return isFinite(v) ? (Math.trunc(v) % 4294967296 + 4294967296) % 4294967296 : 0; }
    function invalidState(what) { return new (global.DOMException || Error)("Failed to " + what + ": The input element's type does not support selection.", 'InvalidStateError'); }
    function queueSelect(el) {
        var s = state(el);
        if (s.selectQueued) return;
        s.selectQueued = true;
        setTimeout(function () {
            s.selectQueued = false;
            var E = global.Event, ev = new E('select', { bubbles: true, cancelable: false });
            if (global.__cyberDispatchTrusted) global.__cyberDispatchTrusted(el, ev); else el.dispatchEvent(ev);
        }, 0);
    }
    function setRange(el, s, n, start, end, dir) {
        start = start === null ? 0 : u32(start); end = end === null ? 0 : u32(end);
        if (end > n) end = n;
        if (start > end) start = end;
        dir = dir === 'forward' || dir === 'backward' ? dir : 'none';
        var changed = s.selStart !== start || s.selEnd !== end || s.selDir !== dir;
        s.selStart = start; s.selEnd = end; s.selDir = dir;
        if (changed) queueSelect(el);
    }
    function textSelection(P, textFn) {
        function clamp(el, s) {
            var t = textFn(el);
            if (t === null) return null;
            var n = t.length;
            if (s.selStart > n) s.selStart = n;
            if (s.selEnd > n) s.selEnd = n;
            return t;
        }
        accessor(P, 'selectionStart', function () {
            var s = state(this); return clamp(this, s) === null ? null : s.selStart;
        }, function (v) {
            var s = state(this), t = clamp(this, s);
            if (t === null) throw invalidState("set the 'selectionStart' property on '" + (this.localName === 'textarea' ? 'HTMLTextAreaElement' : 'HTMLInputElement') + "'");
            var st = v === null ? 0 : u32(v), en = s.selEnd;
            if (en < st) en = st;
            setRange(this, s, t.length, st, en, s.selDir);
        });
        accessor(P, 'selectionEnd', function () {
            var s = state(this); return clamp(this, s) === null ? null : s.selEnd;
        }, function (v) {
            var s = state(this), t = clamp(this, s);
            if (t === null) throw invalidState("set the 'selectionEnd' property on '" + (this.localName === 'textarea' ? 'HTMLTextAreaElement' : 'HTMLInputElement') + "'");
            setRange(this, s, t.length, s.selStart, v === null ? 0 : v, s.selDir);
        });
        accessor(P, 'selectionDirection', function () {
            var s = state(this); return clamp(this, s) === null ? null : s.selDir;
        }, function (v) {
            var s = state(this), t = clamp(this, s);
            if (t === null) throw invalidState("set the 'selectionDirection' property on '" + (this.localName === 'textarea' ? 'HTMLTextAreaElement' : 'HTMLInputElement') + "'");
            setRange(this, s, t.length, s.selStart, s.selEnd, String(v));
        });
        method(P, 'setSelectionRange', function setSelectionRange(start, end, dir) {
            if (arguments.length < 2) throw new TypeError("Failed to execute 'setSelectionRange': 2 arguments required, but only " + arguments.length + " present.");
            var s = state(this), t = clamp(this, s);
            if (t === null) throw invalidState("execute 'setSelectionRange'");
            setRange(this, s, t.length, start, end, dir === undefined ? 'none' : String(dir));
        });
        method(P, 'select', function select() {
            var s = state(this), t = clamp(this, s);
            if (t !== null) setRange(this, s, t.length, 0, t.length, 'none');
        });
        method(P, 'setRangeText', function setRangeText(text, start, end, mode) {
            if (arguments.length < 1) throw new TypeError("Failed to execute 'setRangeText': 1 argument required, but only 0 present.");
            var s = state(this), v = clamp(this, s);
            if (v === null) throw invalidState("execute 'setRangeText'");
            if (arguments.length === 2) throw new TypeError("Failed to execute 'setRangeText': Valid arities are: [1, 3, 4], but 2 arguments provided.");
            text = String(text);
            if (arguments.length < 3) { start = s.selStart; end = s.selEnd; }
            else {
                start = u32(start); end = u32(end);
                if (start > end) throw new (global.DOMException || Error)("Failed to execute 'setRangeText': The provided start value (" + start + ") is larger than the provided end value (" + end + ").", 'IndexSizeError');
            }
            var n = v.length;
            if (start > n) start = n;
            if (end > n) end = n;
            var ss = s.selStart, se = s.selEnd, dir = s.selDir, ps = ss, pe = se;
            var nv = v.slice(0, start) + text + v.slice(end);
            this.value = nv;
            var nn = this.value.length, newEnd = start + text.length;
            mode = mode === undefined ? 'preserve' : String(mode);
            if (['select', 'start', 'end', 'preserve'].indexOf(mode) < 0) throw new TypeError("Failed to execute 'setRangeText': The provided value '" + mode + "' is not a valid enum value of type SelectionMode.");
            if (mode === 'select') { ss = start; se = newEnd; }
            else if (mode === 'start') { ss = se = start; }
            else if (mode === 'end') { ss = se = newEnd; }
            else {
                var delta = text.length - (end - start);
                if (ss > end) ss += delta; else if (ss > start) ss = start;
                if (se > end) se += delta; else if (se > start) se = newEnd;
            }
            /* (the value's own move to its end, undone: set as the mode says, and the change seen) */
            s.selStart = ps; s.selEnd = pe; s.selDir = dir;
            setRange(this, s, nn, ss, se, dir);
        });
    }

    /* ---- textarea ---- */
    var TP = proto('HTMLTextAreaElement');
    if (TP) {
        accessor(TP, 'value', function () {
            var s = state(this);
            return s.dirtyValue ? s.value : textOf(this).replace(/\r\n?/g, '\n');
        }, function (v) {
            var s = state(this), old = this.value;
            s.value = String(v).replace(/\r\n?/g, '\n'); s.dirtyValue = true;
            if (s.value !== old) { s.selStart = s.selEnd = s.value.length; s.selDir = 'none'; }
            if (global.__cyberLiveValue) global.__cyberLiveValue(this, s.value);   /* (painted as set) */
        });
        accessor(TP, 'defaultValue', function () { return textOf(this); },
                 function (v) { this.textContent = String(v); });
        accessor(TP, 'textLength', function () { return this.value.length; });
        accessor(TP, 'type', function () { return 'textarea'; });
        accessor(TP, 'form', function () { return formOwner(this); });
        ['name', 'placeholder', 'autocomplete', 'wrap', 'dirName'].forEach(function (p) { reflectString(TP, p); });
        ['disabled', 'required', 'autofocus'].forEach(function (p) { reflectBool(TP, p); });
        reflectBool(TP, 'readOnly', 'readonly');
        reflectInt(TP, 'maxLength', 'maxlength', -1);
        reflectInt(TP, 'minLength', 'minlength', -1);
        reflectInt(TP, 'rows', 'rows', 2);
        reflectInt(TP, 'cols', 'cols', 20);
        textSelection(TP, function (el) { return el.value; });
    }

    /* ---- select / option ---- */
    var SP = proto('HTMLSelectElement'), OP = proto('HTMLOptionElement');
    /* the list of options: the select's option children and those of its
     * optgroup children, in tree order (one native walk of the store) */
    var nativeOptions = global.__cyberOptions;
    function optionsOf(sel) {
        if (nativeOptions) return nativeOptions(sel);
        var out = [];
        (function walk(n) {
            for (var c = n.firstChild; c; c = c.nextSibling) {
                if (c.localName === 'option') out.push(c);
                else if (c.localName === 'optgroup' && n === sel) walk(c);
            }
        })(sel);
        return out;
    }
    /* (an option's select: its nearest ancestor select, through wrappers; none through an option, hr, datalist, or a second optgroup) */
    function selectOf(opt) {
        var seenOG = false;
        for (var p = opt.parentNode, d = 0; p && p.nodeType === 1 && d < 64; p = p.parentNode, d++) {
            if (p.namespaceURI !== 'http://www.w3.org/1999/xhtml') continue;
            var ln = p.localName;
            if (ln === 'select') return p;
            if (ln === 'option' || ln === 'hr' || ln === 'datalist') return null;
            if (ln === 'optgroup') { if (seenOG) return null; seenOG = true; }
        }
        return null;
    }
    function optionSelected(o) {
        var s = state(o);
        return s.dirtySelected ? s.selected : o.hasAttributeNS(null, 'selected');
    }
    function setSelected(o, v) {
        var s = state(o); s.selected = !!v; s.dirtySelected = true;
        global.__cyberElState(o, 128, true); global.__cyberElState(o, 64, s.selected);
    }
    /* the selectedness setting algorithm, for a single select: the last
     * selected option, else the first non-disabled one (display size 1) */
    function selectedIndexOf(sel, opts) {
        opts = opts || optionsOf(sel);
        var idx = -1;
        for (var i = 0; i < opts.length; i++) if (optionSelected(opts[i])) idx = sel.multiple ? (idx < 0 ? i : idx) : i;
        /* (a script asked for none: it stays so) */
        if (idx < 0 && !state(sel).noneAsked && !sel.multiple && !(Number(sel.getAttributeNS(null, 'size')) > 1)) {
            for (var j = 0; j < opts.length; j++) if (!opts[j].hasAttribute('disabled')) return j;
        }
        return idx;
    }
    /* (a select's <selectedcontent>s, those not in an option: a copy of the selected option's content, made when the
     * selection changes, not as the option's content does) */
    var LASTSC = Symbol('selectedcontentOf');
    function updateSelectedContent(sel, force) {
        if (!sel || sel.multiple) return;
        var opts = optionsOf(sel), o = opts[selectedIndexOf(sel, opts)] || null;
        if (!force && sel[LASTSC] === o) return;
        Object.defineProperty(sel, LASTSC, { value: o, configurable: true, writable: true });
        var scs = sel.getElementsByTagName('selectedcontent');
        for (var q = 0; q < scs.length; q++) {
            var inOpt = false;
            for (var up = scs[q].parentNode; up && up !== sel && !inOpt; up = up.parentNode) inOpt = up.localName === 'option';
            if (inOpt) continue;
            var want = o ? o.innerHTML : '';
            if (scs[q].innerHTML !== want) scs[q].innerHTML = want;
        }
    }
    global.__cyberSelectedContent = updateSelectedContent;
    global.__cyberBeforeParserScript = function () {
        var d = global.document;
        if (!d) return;
        var sc = d.getElementsByTagName('selectedcontent');
        for (var i = 0; i < sc.length; i++) {
            var sel = sc[i].closest ? sc[i].closest('select') : null;
            if (sel) updateSelectedContent(sel);
        }
    };
    global.__cyberSelectAttr = function (el) {
        var sel = el.localName === 'select' ? el : selectOf(el);
        if (sel) updateSelectedContent(sel);
    };
    /* (an option inserted: when selected, in a select of one, the others not) */
    global.__cyberOptionInserted = function (root) {
        /* (what went in: its options, the last selected of them the one kept) */
        var list = root.localName === 'option' ? [root] : Array.prototype.slice.call(root.getElementsByTagName('option'));
        var sel = null, win = null;
        for (var i = 0; i < list.length; i++) {
            var s = selectOf(list[i]);
            if (!s) continue;
            sel = s;
            if (optionSelected(list[i])) win = list[i];
        }
        if (!sel) return;
        if (!sel.multiple && win) optionsOf(sel).forEach(function (x) { if (x !== win && optionSelected(x)) setSelected(x, false); });
        updateSelectedContent(sel);
    };
    if (OP) {
        accessor(OP, 'value', function () { var v = this.getAttributeNS(null, 'value'); return v === null ? this.text : v; },
                 function (v) { this.setAttribute('value', String(v)); });
        accessor(OP, 'text', function () { return textOf(this).replace(/\s+/g, ' ').trim(); },
                 function (v) { this.textContent = String(v); });
        /* (label: the attribute (no namespace) when there, else text) */
        accessor(OP, 'label', function () { var v = this.getAttributeNS(null, 'label'); return v === null ? this.text : v; },
                 function (v) { this.setAttribute('label', String(v)); });
        reflectBool(OP, 'disabled');
        reflectBool(OP, 'defaultSelected', 'selected');
        accessor(OP, 'selected', function () {
            var sel = selectOf(this);
            if (sel && !sel.multiple) { var opts = optionsOf(sel); return opts[selectedIndexOf(sel, opts)] === this; }
            return optionSelected(this);
        }, function (v) {
            var sel = selectOf(this);
            if (sel && !sel.multiple && v) optionsOf(sel).forEach(function (o) { setSelected(o, false); });
            setSelected(this, v);
            if (sel) updateSelectedContent(sel, true);
        });
        accessor(OP, 'index', function () { var sel = selectOf(this); return sel ? optionsOf(sel).indexOf(this) : 0; });
        accessor(OP, 'form', function () { var sel = selectOf(this); return sel ? formOwner(sel) : null; });
    }
    if (SP) {
        accessor(SP, 'options', function () {
            var l = optionsOf(this);
            var HC = global.HTMLCollection;
            if (HC) Object.setPrototypeOf(l, HC.prototype);
            return l;
        });
        accessor(SP, 'length', function () { return optionsOf(this).length; }, function (v) {
            /* (more: blank options appended; fewer: the last ones gone) */
            var n = Number(v) >>> 0, opts = optionsOf(this);
            if (n > 100000) return;
            for (var k = opts.length; k < n; k++) this.appendChild(this.ownerDocument.createElement('option'));
            for (var j = opts.length - 1; j >= n; j--) if (opts[j].parentNode) opts[j].parentNode.removeChild(opts[j]);
        });
        accessor(SP, 'selectedIndex', function () { return selectedIndexOf(this); }, function (i) {
            var opts = optionsOf(this);
            opts.forEach(function (o, k) { setSelected(o, k === (i | 0)); });
            state(this).noneAsked = !opts[i | 0];
            updateSelectedContent(this, true);
        });
        accessor(SP, 'value', function () {
            var opts = optionsOf(this), o = opts[selectedIndexOf(this, opts)];
            return o ? o.value : '';
        }, function (v) {
            v = String(v);
            var found = false;
            optionsOf(this).forEach(function (o) {
                var hit = !found && o.value === v;
                if (hit) found = true;
                setSelected(o, hit);
            });
            updateSelectedContent(this, true);
        });
        accessor(SP, 'selectedOptions', function () {
            var self = this;
            return optionsOf(this).filter(function (o, i) { return self.multiple ? optionSelected(o) : i === selectedIndexOf(self); });
        });
        accessor(SP, 'type', function () { return this.multiple ? 'select-multiple' : 'select-one'; });
        accessor(SP, 'form', function () { return formOwner(this); });
        reflectString(SP, 'name');
        reflectString(SP, 'autocomplete');
        ['disabled', 'required', 'multiple', 'autofocus'].forEach(function (p) { reflectBool(SP, p); });
        reflectInt(SP, 'size', 'size', 0);
        method(SP, 'item', function (i) { return optionsOf(this)[i >>> 0] || null; });
        method(SP, 'add', function (el, before) {
            var ref = typeof before === 'number' ? optionsOf(this)[before] : before;
            this.insertBefore(el, ref || null);
        });
        method(SP, 'remove', function (i) {
            if (arguments.length === 0) { if (this.parentNode) this.parentNode.removeChild(this); return; }
            var o = optionsOf(this)[i | 0];
            if (o && o.parentNode) o.parentNode.removeChild(o);
        });
    }

    /* ---- button ---- */
    var BP = proto('HTMLButtonElement');
    function buttonType(el) {
        var v = String(el.getAttributeNS(null, 'type') || '').toLowerCase();
        if (v === 'submit' || v === 'reset' || v === 'button') return v;
        return el.hasAttributeNS(null, 'command') || el.hasAttributeNS(null, 'commandfor') ? 'button' : 'submit';
    }
    if (BP) {
        /* (the Auto state, missing or invalid: a button with command or commandfor is a Button, else Submit) */
        accessor(BP, 'type', function () { return buttonType(this); }, function (v) { this.setAttribute('type', String(v)); });
        reflectString(BP, 'value');
        reflectString(BP, 'name');
        ['disabled', 'autofocus', 'formNoValidate'].forEach(function (p) { reflectBool(BP, p); });
        accessor(BP, 'form', function () { return formOwner(this); });
    }

    /* ---- form ---- */
    var FP = proto('HTMLFormElement');
    var LISTED = ['button', 'fieldset', 'input', 'object', 'output', 'select', 'textarea'];
    function elementsOf(form) {
        var out = [];
        var doc = form.ownerDocument, root = doc && rootOf(form) === doc ? doc : rootOf(form);
        var all = root.querySelectorAll ? root.querySelectorAll(LISTED.join(',')) : [];
        for (var i = 0; i < all.length; i++) if (formOwner(all[i]) === form) out.push(all[i]);
        return out;
    }
    if (FP) {
        accessor(FP, 'elements', function () {
            var l = elementsOf(this), HC = global.HTMLCollection;
            l.forEach(function (e) { var n = e.getAttribute('name') || e.id; if (n && !(n in l)) l[n] = e; });
            if (HC) Object.setPrototypeOf(l, HC.prototype);
            return l;
        });
        accessor(FP, 'length', function () { return elementsOf(this).length; });
        ['name', 'action', 'target', 'acceptCharset', 'autocomplete'].forEach(function (p) {
            reflectString(FP, p, p === 'acceptCharset' ? 'accept-charset' : undefined);
        });
        enumerated(FP, 'method', 'method', ['get', 'post', 'dialog'], 'get', 'get');
        enumerated(FP, 'enctype', 'enctype', ['application/x-www-form-urlencoded', 'multipart/form-data', 'text/plain'],
                   'application/x-www-form-urlencoded', 'application/x-www-form-urlencoded');
        reflectBool(FP, 'noValidate', 'novalidate');
        method(FP, 'submit', function () {
            if (typeof this.__cyberSubmit === 'function') this.__cyberSubmit(null);
        });
        method(FP, 'reset', function () {
            var E = global.Event;
            if (!this.dispatchEvent(new E('reset', { bubbles: true, cancelable: true }))) return;
            elementsOf(this).forEach(function (el) {
                var s = el[STATE];
                if (s) { s.dirtyValue = false; s.dirtyChecked = false; syncChecked(el, s); }
                if (el.localName === 'select') optionsOf(el).forEach(function (o) {
                    if (o[STATE]) { o[STATE].dirtySelected = false; global.__cyberElState(o, 128, false); }
                });
                if (el.localName === 'output' && global.__cyberOutputReset) global.__cyberOutputReset(el);
            });
        });
        /* (each control's, all of them: each invalid one fires its event) */
        function formValid() {
            var ok = true;
            elementsOf(this).forEach(function (el) { if (!el.checkValidity()) ok = false; });
            return ok;
        }
        method(FP, 'checkValidity', function checkValidity() { return formValid.call(this); });
        method(FP, 'reportValidity', function reportValidity() { return formValid.call(this); });
        /* (a form's operations: this a form) */
        var firingSubmit = new WeakSet();
        ['submit', 'reset', 'checkValidity', 'reportValidity', 'requestSubmit'].forEach(function (k) {
            var f = FP[k];
            if (typeof f !== 'function' || !global.HTMLFormElement) return;
            var w = { [k]: function () {
                if (!(this instanceof global.HTMLFormElement)) throw new TypeError("Failed to execute '" + k + "' on 'HTMLFormElement': Illegal invocation");
                /* (requestSubmit while its submission events are firing: nothing, HTML's firing submission events flag) */
                if (k === 'requestSubmit') {
                    if (firingSubmit.has(this)) return;
                    firingSubmit.add(this);
                    try { return f.apply(this, arguments); } finally { firingSubmit.delete(this); }
                }
                return f.apply(this, arguments);
            } }[k];
            Object.defineProperty(w, 'length', { value: 0 });
            method(FP, k, w);
        });
    }

    /* ---- label / fieldset / output ---- */
    var LABELABLE = ['button', 'input', 'meter', 'output', 'progress', 'select', 'textarea'];
    var isLabelable = function (el) {
        if (!el || el.nodeType !== 1 || el.namespaceURI !== 'http://www.w3.org/1999/xhtml') return false;
        if (el.localName === 'input') return String(el.getAttribute('type') || '').toLowerCase() !== 'hidden';
        return LABELABLE.indexOf(el.localName) >= 0;
    };
    var LP = proto('HTMLLabelElement');
    if (LP) {
        reflectString(LP, 'htmlFor', 'for');
        /* (HTML 4.10.4: with for, the first element of that ID in the label's tree, if labelable; else its first labelable descendant) */
        var labelControl = function (lab) {
            var f = lab.getAttribute('for');
            if (f !== null) {
                var root = lab.getRootNode ? lab.getRootNode() : lab.ownerDocument, el = null;
                if (root && typeof root.getElementById === 'function') el = root.getElementById(f);
                else if (root && root.querySelectorAll && f) {
                    var it = root.querySelectorAll('[id]');
                    for (var q = 0; q < it.length && !el; q++) if (it[q].id === f) el = it[q];
                }
                return el && isLabelable(el) ? el : null;
            }
            var all = lab.querySelectorAll(LABELABLE.join(','));
            for (var i = 0; i < all.length; i++) if (isLabelable(all[i])) return all[i];
            return null;
        };
        accessor(LP, 'control', function () { return labelControl(this); });
        accessor(LP, 'form', function () { var c = this.control; return c ? formOwner(c) : null; });
    }
    var FSP = proto('HTMLFieldSetElement');
    if (FSP) {
        reflectBool(FSP, 'disabled');
        reflectString(FSP, 'name');
        accessor(FSP, 'type', function () { return 'fieldset'; });
        accessor(FSP, 'form', function () { return formOwner(this); });
    }

    /* Checkedness as the user agent changes it (activation behavior,
     * browser/dom_events.js): the state itself, not through the `checked`
     * setter, which pages may wrap (React tracks values that way). */
    if (!global[Symbol.for('cyber.formInternals')])
        Object.defineProperty(global, Symbol.for('cyber.formInternals'), { value: Object.freeze({
            getChecked: function (el) {
                var s = el[STATE];
                return s && s.dirtyChecked ? s.checked : el.hasAttribute('checked');
            },
            setChecked: function (el, v) {
                var s = state(el);
                s.checked = !!v; s.dirtyChecked = true;
                syncChecked(el, s);
                if (s.checked && el.localName === 'input' && inputType(el) === 'radio') uncheckGroup(el);
            }
        }) });

    /* ---- FormData (XHR 5): the entries, a form's as HTML "constructing the
     * entry list" has them (without files or the _charset_ name) ---- */
    var FD_ENTRIES = Symbol('entries');
    /* (HTML "constructing the entry list", 4.10.21.4) */
    function formEntries(form, submitter) {
        var out = [];
        elementsOf(form).forEach(function (el) {
            if (el.matches('datalist *')) return;
            for (var a = el; a && a.nodeType === 1; a = a.parentNode)
                if (a.hasAttributeNS(null, 'disabled') && (a === el || a.localName === 'fieldset')) return;
            var ln = el.localName, name = el.getAttributeNS(null, 'name');
            if (ln === 'input' && inputType(el) === 'image') {
                if (el !== submitter) return;
                var pre = name ? name + '.' : '';
                out.push([pre + 'x', '0'], [pre + 'y', '0']);
                return;
            }
            if (!name) return;
            if (ln === 'select') {
                optionsOf(el).forEach(function (o) { if (o.selected && !o.disabled) out.push([name, o.value]); });
            } else if (ln === 'textarea') {
                out.push([name, el.value]);
            } else if (ln === 'button' || ln === 'input') {
                var t = ln === 'input' ? inputType(el) : buttonType(el);
                if ((t === 'submit' || t === 'button' || t === 'reset') && el !== submitter) return;
                if (ln === 'button' && t !== 'submit' && el === submitter) { }
                if ((t === 'checkbox' || t === 'radio') && !el.checked) return;
                if (t === 'file') {
                    var fl = el.files;
                    if (!fl || !fl.length) out.push([name, new global.File([], '', { type: 'application/octet-stream' })]);
                    else for (var k = 0; k < fl.length; k++) out.push([name, fl[k]]);
                    return;
                }
                if (ln === 'input' && t === 'hidden' && name.toLowerCase() === '_charset_') { out.push([name, 'UTF-8']); return; }
                out.push([name, el.value]);
            } else return;
            var dn = el.getAttributeNS(null, 'dirname');
            if (dn && (ln === 'textarea' || (ln === 'input' && /^(text|search|tel|url|email|password|submit|reset|hidden)$/.test(inputType(el)))))
                out.push([dn, 'ltr']);
        });
        return out;
    }
    var constructing = new WeakSet();
    function FormData(form, submitter) {
        if (!new.target) throw new TypeError("Failed to construct 'FormData': Please use the 'new' operator.");
        if (form !== undefined && !(FP && FP.isPrototypeOf(form)))
            throw new TypeError("Failed to construct 'FormData': parameter 1 is not of type 'HTMLFormElement'.");
        if (form && constructing.has(form)) throw new global.DOMException("Failed to construct 'FormData': The form is already constructing its entry list.", 'InvalidStateError');
        this[FD_ENTRIES] = [];
        if (form) {
            constructing.add(form);
            try {
                this[FD_ENTRIES] = formEntries(form, submitter);
                var FDE = global.FormDataEvent;
                if (FDE) form.dispatchEvent(new FDE('formdata', { bubbles: true, formData: this }));
            } finally { constructing.delete(form); }
        }
    }
    /* (USVString: lone surrogates as U+FFFD) */
    function usv(s) { s = String(s); return typeof s.toWellFormed === 'function' ? s.toWellFormed() : s.replace(/[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(^|[^\uD800-\uDBFF])[\uDC00-\uDFFF]/g, function (m, p) { return (p || '') + '\uFFFD'; }); }
    function fdValue(v, filename) {
        if (global.Blob && v instanceof global.Blob) {
            if (global.File && (!(v instanceof global.File) || filename !== undefined))
                return new global.File([v], filename !== undefined ? usv(filename) : (v instanceof global.File ? v.name : 'blob'), { type: v.type, lastModified: v.lastModified });
            return v;
        }
        return usv(v);
    }
    var FDP = FormData.prototype;
    method(FDP, 'append', function (n, v, filename) {
        if (arguments.length < 2) throw new TypeError("Failed to execute 'append' on 'FormData': 2 arguments required, but only " + arguments.length + " present.");
        this[FD_ENTRIES].push([usv(n), fdValue(v, arguments.length > 2 ? filename : undefined)]);
    });
    method(FDP, 'delete', function (n) { n = String(n); this[FD_ENTRIES] = this[FD_ENTRIES].filter(function (e) { return e[0] !== n; }); });
    method(FDP, 'get', function (n) {
        n = String(n);
        var e = this[FD_ENTRIES].find(function (x) { return x[0] === n; });
        return e ? e[1] : null;
    });
    method(FDP, 'getAll', function (n) {
        n = String(n);
        return this[FD_ENTRIES].filter(function (x) { return x[0] === n; }).map(function (x) { return x[1]; });
    });
    method(FDP, 'has', function (n) { n = String(n); return this[FD_ENTRIES].some(function (x) { return x[0] === n; }); });
    method(FDP, 'set', function (n, v) {
        n = String(n);
        var es = this[FD_ENTRIES], at = es.findIndex(function (x) { return x[0] === n; });
        var kept = es.filter(function (x, i) { return x[0] !== n || i === at; });
        var fv = fdValue(v, arguments.length > 2 ? arguments[2] : undefined);
        if (at < 0) kept.push([n, fv]);
        else kept[kept.findIndex(function (x) { return x[0] === n; })] = [n, fv];
        this[FD_ENTRIES] = kept;
    });
    method(FDP, 'forEach', function (cb, thisArg) {
        var es = this[FD_ENTRIES];
        for (var i = 0; i < es.length; i++) cb.call(thisArg, es[i][1], es[i][0], this);
    });
    method(FDP, 'entries', function () { return this[FD_ENTRIES].map(function (e) { return [e[0], e[1]]; })[Symbol.iterator](); });
    method(FDP, 'keys', function () { return this[FD_ENTRIES].map(function (e) { return e[0]; })[Symbol.iterator](); });
    method(FDP, 'values', function () { return this[FD_ENTRIES].map(function (e) { return e[1]; })[Symbol.iterator](); });
    Object.defineProperty(FDP, Symbol.iterator, { configurable: true, writable: true, value: FDP.entries });
    Object.defineProperty(FDP, Symbol.toStringTag, { configurable: true, value: 'FormData' });
    Object.defineProperty(global, 'FormData', { configurable: true, writable: true, value: FormData });

    /* Constraint validation (HTML 4.10.20): who's a candidate, and what's wrong
     * with its value.  (No tooLong, tooShort, stepMismatch or badInput: those
     * want user edits or parsing that isn't here.) */
    function candidate(el) {
        if (el.localName === 'fieldset' || el.localName === 'output' || el.localName === 'object' || el.hasAttributeNS(null, 'disabled')) return false;
        for (var a = el.parentNode; a && a.nodeType === 1; a = a.parentNode)
            if (a.localName === 'datalist' || (a.localName === 'fieldset' && a.hasAttributeNS(null, 'disabled'))) return false;
        if (el.localName === 'button') return buttonType(el) === 'submit';
        if (el.localName === 'textarea') return !el.hasAttributeNS(null, 'readonly');
        if (el.localName === 'input') {
            var t = inputType(el);
            return t !== 'hidden' && t !== 'reset' && t !== 'button' && !el.hasAttributeNS(null, 'readonly');
        }
        return true;
    }
    /* (mutable: not disabled, and not readonly where readonly applies) */
    function immutable(el) {
        if (el.matches && el.matches(':disabled')) return true;
        return (el.localName === 'input' || el.localName === 'textarea') && el.hasAttributeNS(null, 'readonly') &&
            !(el.localName === 'input' && /^(hidden|range|color|checkbox|radio|file|submit|image|reset|button)$/.test(inputType(el)));
    }
    function valueMissing(el) {
        var req = el.hasAttributeNS(null, 'required');
        if (el.localName === 'input' && inputType(el) === 'radio') {
            /* (a group's: required if any of it is, missing if none of it is checked) */
            var gname = el.getAttributeNS(null, 'name');
            if (!gname) return false;   /* (no group: as browsers have it) */
            var grp = radioGroup(el, gname), anyReq = false;
            for (var g = 0; g < grp.length; g++) { if (grp[g].checked) return false; if (grp[g].hasAttributeNS(null, 'required')) anyReq = true; }
            return anyReq;
        }
        if (!req) return false;
        if (el.localName === 'select') {
            /* (no option selected, or only the placeholder label option) */
            var opts = optionsOf(el), sel = null;
            for (var i = 0; i < opts.length && !sel; i++) if (opts[i].selected) sel = opts[i];
            if (!sel) return true;
            var multi = el.hasAttributeNS(null, 'multiple'), size = parseInt(el.getAttributeNS(null, 'size'), 10);
            if (!multi && !(size > 1) && sel === opts[0] && sel.value === '' && sel.parentNode === el) return true;
            return false;
        }
        if (el.localName === 'input') {
            var t = inputType(el);
            if (t === 'checkbox') return !el.checked;
            if (t === 'file') return !(el.files && el.files.length);
            if (/^(hidden|range|color|submit|image|reset|button)$/.test(t)) return false;
        }
        if (el.localName !== 'input' && el.localName !== 'textarea') return false;
        if (immutable(el)) return false;
        return !el.value;
    }
    function radioGroup(el, name) {
        var form = el.form, root = rootOf(el), out = [];
        var all = root.querySelectorAll ? root.querySelectorAll('input') : [el];
        for (var j = 0; j < all.length; j++)
            if (inputType(all[j]) === 'radio' && all[j].form === form && all[j].getAttributeNS(null, 'name') === name) out.push(all[j]);
        if (out.indexOf(el) < 0) out.push(el);
        return out;
    }
    var EMAIL = /^[a-zA-Z0-9.!#$%&'*+\/=?^_`{|}~-]+@[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(?:\.[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*$/;
    function emailValues(el) {
        var v = el.value;
        return el.hasAttributeNS(null, 'multiple') ? v.split(',').map(function (s) { return s.replace(/^[\t\n\f\r ]+|[\t\n\f\r ]+$/g, ''); }) : [v];
    }
    function typeMismatch(el) {
        if (el.localName !== 'input' || !el.value) return false;
        var t = inputType(el);
        if (t === 'email') return emailValues(el).some(function (s) { return !EMAIL.test(s); });
        if (t === 'url') { try { new global.URL(el.value); return false; } catch (e) { return true; } }
        return false;
    }
    function patternMismatch(el) {
        if (el.localName !== 'input' || !/^(text|search|tel|url|email|password)$/.test(inputType(el))) return false;
        var p = el.getAttributeNS(null, 'pattern');
        if (p === null || !el.value) return false;
        /* (a bad pattern, alone or wrapped: none; one that gives up, too much backtracking: no match) */
        var re;
        try { new RegExp(p, 'v'); re = new RegExp('^(?:' + p + ')$', 'v'); } catch (e) {
            if (p.indexOf('[') >= 0) return false;   /* (v differs from u in classes only: a class v refuses, none) */
            try { new RegExp(p, 'u'); re = new RegExp('^(?:' + p + ')$', 'u'); } catch (e2) { return false; }
        }
        var vals = inputType(el) === 'email' ? emailValues(el) : [el.value];
        try { return vals.some(function (s) { return !re.test(s); }); } catch (e) { return true; }
    }
    var RANGED = { date: 1, month: 1, week: 1, time: 1, 'datetime-local': 1, number: 1, range: 1 };
    function limitOf(el, attr) {
        var t = inputType(el), a = el.getAttributeNS(null, attr);
        if (a === null) return NaN;
        if (t === 'number' || t === 'range') { var n = parseFloatValue(a); return n === null ? NaN : n; }
        return toNumber(t, a);
    }
    function parseFloatValue(s) {
        var m = /^[\t\n\f\r ]*(-?(?:\d+(?:\.\d+)?|\.\d+)(?:[eE][-+]?\d+)?)/.exec(s);
        if (!m) return null;
        var v = Number(m[1]);
        return isFinite(v) ? v : null;
    }
    function numValue(el) {
        var t = inputType(el);
        if (!RANGED[t] || !el.value) return NaN;
        return toNumber(t, el.value);
    }
    function rangeMismatch(el, attr, under) {
        if (el.localName !== 'input' || !RANGED[inputType(el)]) return false;
        var v = numValue(el);
        if (isNaN(v)) return false;
        var lim = limitOf(el, attr);
        if (inputType(el) === 'time') {
            var mn = limitOf(el, 'min'), mx = limitOf(el, 'max');
            /* (a reversed range: out of it, both at once) */
            if (!isNaN(mn) && !isNaN(mx) && mn > mx) return v > mx && v < mn;
        }
        return !isNaN(lim) && (under ? v < lim : v > lim);
    }
    /* (allowed value step: a step and its base, in the type's units; null any) */
    var STEP = { date: [1, 86400000, 0], month: [1, 1, 0], week: [1, 604800000, -259200000], time: [60, 1000, 0],
                 'datetime-local': [60, 1000, 0], number: [1, 1, 0], range: [1, 1, 0] };
    function stepOf(el) {
        var t = inputType(el), d = STEP[t];
        if (!d) return null;
        var a = el.getAttributeNS(null, 'step'), step = d[0];
        if (a !== null) {
            if (a.toLowerCase() === 'any') return null;
            var n = parseFloatValue(a);
            if (n !== null && n > 0) {
                step = n;
                if ((t === 'date' || t === 'month' || t === 'week') && step !== Math.round(step)) step = Math.max(1, Math.round(step));
            }
        }
        var base = limitOf(el, 'min');
        if (isNaN(base)) {
            var dv = el.getAttributeNS(null, 'value');
            base = dv === null ? NaN : (t === 'number' || t === 'range' ? (parseFloatValue(dv) === null ? NaN : parseFloatValue(dv)) : toNumber(t, dv));
        }
        if (isNaN(base)) base = d[2];
        return { step: step * d[1], base: base };
    }
    function stepMismatch(el) {
        if (el.localName !== 'input') return false;
        var v = numValue(el);
        if (isNaN(v)) return false;
        var s = stepOf(el);
        if (!s) return false;
        var q = (v - s.base) / s.step, r = Math.round(q);
        return Math.abs(q - r) > 1e-9 * Math.max(1, Math.abs(q));
    }
    function customError(el) { return !!state(el).customValidity; }
    var FLAWS = { valueMissing: valueMissing, typeMismatch: typeMismatch, patternMismatch: patternMismatch,
        tooLong: function () { return false; }, tooShort: function () { return false; },
        rangeUnderflow: function (el) { return rangeMismatch(el, 'min', true); },
        rangeOverflow: function (el) { return rangeMismatch(el, 'max', false); },
        stepMismatch: stepMismatch, badInput: function () { return false; },
        customError: customError };
    function suffering(el) {
        for (var k in FLAWS) if (FLAWS[k](el)) return true;
        return false;
    }
    function isValid(el) { return !candidate(el) || !suffering(el); }
    /* ValidityState (HTML 4.10.20.3): the element's, read as it is now */
    var VS_EL = Symbol('validityElement');
    function ValidityState() { throw new TypeError('Illegal constructor'); }
    var VSP = ValidityState.prototype;
    Object.keys(FLAWS).concat(['valid']).forEach(function (k) {
        var get = { get: function () {
            var el = this && this[VS_EL];
            if (!el) throw new TypeError('Illegal invocation');
            return k === 'valid' ? !suffering(el) : !!FLAWS[k](el);
        } };
        Object.defineProperty(VSP, k, { configurable: true, enumerable: true, get: Object.getOwnPropertyDescriptor(get, 'get').value });
    });
    Object.defineProperty(VSP, Symbol.toStringTag, { configurable: true, value: 'ValidityState' });
    Object.defineProperty(global, 'ValidityState', { configurable: true, writable: true, value: ValidityState });
    function validityOf(el) {
        var s = state(el);
        if (s.validity) return s.validity;
        var v = Object.create(VSP);
        Object.defineProperty(v, VS_EL, { value: el });
        return s.validity = v;
    }
    function checkValidity() {
        if (isValid(this)) return true;
        this.dispatchEvent(new global.Event('invalid', { cancelable: true }));
        return false;
    }
    [IP, TP, SP, BP, FSP, proto('HTMLOutputElement'), proto('HTMLObjectElement')].forEach(function (P) {
        if (!P) return;
        if (!Object.getOwnPropertyDescriptor(P, 'form')) accessor(P, 'form', function () { return formOwner(this); });
        accessor(P, 'validity', function () { return validityOf(this); });
        if (!global.__cyberFormState) Object.defineProperty(global, '__cyberFormState', { configurable: true, value: function (el) {
            /* (1 placeholder shown, 2 default, 4 in range, 8 out of range) */
            var tag = el.localName, type = tag === 'input' ? String(el.type || 'text').toLowerCase() : '', bits = 0;
            var texty = tag === 'textarea' || (tag === 'input' && /^(text|search|url|tel|email|password|number)$/.test(type));
            if (texty && el.hasAttribute('placeholder') && el.value === '') bits |= 1;
            if ((type === 'checkbox' || type === 'radio') && el.hasAttribute('checked')) bits |= 2;
            if (tag === 'option' && el.hasAttribute('selected')) bits |= 2;
            if (tag === 'input' && /^(number|range|date|month|week|time|datetime-local)$/.test(type) &&
                (el.hasAttribute('min') || el.hasAttribute('max')) && !el.disabled && !el.readOnly) {
                var vy = validityOf(el);
                bits |= vy && (vy.rangeUnderflow || vy.rangeOverflow) ? 8 : 4;
            }
            return bits;
        } });
        accessor(P, 'validationMessage', function () {
            return isValid(this) ? '' : state(this).customValidity || 'Invalid value.';
        });
        accessor(P, 'willValidate', function () { return candidate(this); });
        method(P, 'checkValidity', checkValidity);
        method(P, 'reportValidity', function reportValidity() { return checkValidity.call(this); });
        method(P, 'setCustomValidity', function setCustomValidity(m) {
            if (!(this instanceof global.Element)) throw new TypeError('Illegal invocation');
            if (arguments.length < 1) throw new TypeError("Failed to execute 'setCustomValidity': 1 argument required, but only 0 present.");
            state(this).customValidity = String(m);
        });
        if (P !== FSP && P !== proto('HTMLObjectElement')) accessor(P, 'labels', function () {
            /* (not labelable: a hidden input's is null) */
            if (this.localName === 'input' && !isLabelable(this)) return null;
            /* (the same NodeList each time, live: worked out at each look) */
            var s = state(this);
            if (s.labelsList) return s.labelsList;
            var el = this, now = function () { return labelsNow(el); };
            var base = global.NodeList ? Object.create(global.NodeList.prototype) : {};
            var own = {
                item: function (i) { var l = now(); i = i >>> 0; return i < l.length ? l[i] : null; },
                forEach: function (cb, t) { now().forEach(function (x, i) { cb.call(t, x, i, s.labelsList); }); },
                entries: function () { return now().entries(); }, keys: function () { return now().keys(); },
                values: function () { return now().values(); }
            };
            s.labelsList = new Proxy(base, {
                get: function (t, k, r) {
                    if (k === 'length') return now().length;
                    if (typeof k === 'string' && /^(0|[1-9]\d*)$/.test(k)) return now()[+k];
                    if (k === Symbol.iterator) return own.values;
                    if (typeof k === 'string' && Object.prototype.hasOwnProperty.call(own, k)) return own[k];
                    return Reflect.get(t, k, r);
                },
                has: function (t, k) {
                    if (typeof k === 'string' && /^(0|[1-9]\d*)$/.test(k)) return +k < now().length;
                    return k === 'length' || Reflect.has(t, k);
                },
                ownKeys: function (t) { return Object.keys(now()).concat(Reflect.ownKeys(t)); },
                getOwnPropertyDescriptor: function (t, k) {
                    if (typeof k === 'string' && /^(0|[1-9]\d*)$/.test(k)) { var l = now(); return +k < l.length ? { value: l[+k], enumerable: true, configurable: true, writable: false } : undefined; }
                    return Reflect.getOwnPropertyDescriptor(t, k);
                }
            });
            return s.labelsList;
        });
        var labelsNow = function (el) {
            var root = el.getRootNode ? el.getRootNode() : el.ownerDocument, out = [];
            if (root && root.querySelectorAll) {
                var ls = root.querySelectorAll('label');
                for (var i = 0; i < ls.length; i++) if (ls[i].control === el) out.push(ls[i]);
            }
            if (root && root !== el && root.nodeType === 1 && root.localName === 'label' && root.control === el) out.unshift(root);
            return out;
        };
    });

    /* ---- <select> rendering (HTML 15.5.15 / customizable select): the browser's
     * own shadow tree on each select, made again when the document changes.
     * A drop-down box shows its selected option's label and an arrow (every
     * label laid out with no height inside it, for its width); a list box
     * shows its options; appearance: base-select shows the first <button>
     * child (or the label) and a picker icon, and its picker (a popover)
     * the other children. ---- */
    (function () {
        var doc0 = global.document;
        if (!doc0 || !SP || typeof global.MutationObserver !== 'function' || !global.getComputedStyle) return;
        var UA = Symbol('selectUA'), UAKEY = Symbol('selectUAkey'), UAOPEN = Symbol('selectUAopen'), LBLFS = Symbol('selectLabelSize');
        function esc(s) { return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;'); }
        function labelOf(o) {
            var l = o.getAttributeNS(null, 'label');
            if (l !== null && l !== '') return l;
            return o.text;
        }
        var DROP = ':host { white-space: nowrap !important; text-indent: 0 !important; text-align: start; align-content: center; }' +
            '.l { white-space: pre; }' +
            '.a { display: inline-block; width: 0; height: 0; border-inline: 4px solid transparent; border-top: 5px solid currentColor; margin-inline: 5px 3px; vertical-align: middle; }' +
            '.w { display: block; height: 0; overflow: hidden; visibility: hidden; white-space: pre; padding-inline-end: 17px; }';
        var LIST = ':host { display: inline-block; overflow-x: hidden; overflow-y: auto; white-space: nowrap; vertical-align: text-bottom; line-height: normal; }' +
            '::slotted(option) { display: block; white-space: nowrap; padding-inline: 2px; min-block-size: 1.2em; min-inline-size: 0; gap: 0; }' +
            '::slotted(option)::before { content: none; }' +
            '::slotted(optgroup) { display: block; font-weight: bold; }' +
            '::slotted(option:checked) { background-color: #cecece; } :host(:focus) ::slotted(option:checked) { background-color: #cecece !important; color: FieldText !important; }';
        var BASE = ':host { color: inherit; font: inherit; background-color: transparent; padding-block: 0.25em; padding-inline: 0.5em; border: 1px solid currentColor; border-radius: 0.5em; display: inline-flex; gap: 0.5em; min-inline-size: calc-size(auto, max(size, 24px)); min-block-size: max(24px, 1lh); box-sizing: border-box; text-align: inherit; cursor: default; }' +
            ':host(:disabled) { color: color-mix(currentColor 50%, transparent); }' +
            ':host(:enabled:not([data-cyber-open]):hover) { background-color: color-mix(currentColor 10%, transparent); }' +
            ':host(:enabled:active) { background-color: color-mix(currentColor 20%, transparent); }' +
            '::slotted(button[data-cyber-selbtn]) { display: contents; border: 0; padding: 0; margin: 0; background: none; color: inherit; font: inherit; }' +
            '.icon { margin-inline-start: auto; display: block; order: 2147483646; }' +
            '.picker { position: fixed; width: fit-content; height: fit-content; box-sizing: border-box; border: 1px solid; padding: 0; color: CanvasText; background-color: Canvas; margin: 0; inset: auto; min-inline-size: anchor-size(self-inline); min-block-size: 1lh; overflow: auto; position-area: block-end span-inline-end; position-try-fallbacks: block-start span-inline-end, block-end span-inline-start, block-start span-inline-start; }' +
            '::slotted(option) { min-inline-size: 24px; min-block-size: max(24px, 1lh); padding-inline: 0.5em; padding-block-end: 0; display: flex; align-items: center; gap: 0.5em; white-space: nowrap; }' +

            ':host(:not(:disabled)) ::slotted(option:disabled) { color: color-mix(currentColor 50%, transparent); }' +
            '::slotted(optgroup) { display: block; }' +
            '::slotted(hr) { margin-inline: 0.5em; }';
        /* (a base-select list box, multiple or of a size over 1: its options in it, that many rows tall) */
        function baseListbox(rows) {
            return ':host { display: inline-block; overflow: auto; border: 1px solid; padding: 0; color: inherit; font: inherit; background-color: transparent; box-sizing: content-box; text-align: inherit; cursor: default; block-size: calc(max(24px, 1lh) * ' + rows + '); }' +
                ':host(:disabled) { color: color-mix(currentColor 50%, transparent); }' +
                '::slotted(option) { min-inline-size: 24px; min-block-size: max(24px, 1lh); padding-inline: 0.5em; padding-block-end: 0; display: flex; align-items: center; gap: 0.5em; white-space: nowrap; }' +

                ':host(:not(:disabled)) ::slotted(option:disabled) { color: color-mix(currentColor 50%, transparent); }' +
                '::slotted(optgroup) { display: block; }';   /* (an hr in it: as wide as the box, its own margins) */
        }
        function selectedOf(sel) {
            var opts = optionsOf(sel), i = selectedIndexOf(sel, opts);
            return { opts: opts, idx: i, opt: i >= 0 ? opts[i] : null };
        }
        function render(sel) {
            var root = sel[UA];
            if (!sel.isConnected) return;
            var cs;
            try { cs = global.getComputedStyle(sel); } catch (e) { return; }
            var base = cs.appearance === 'base-select' || cs.appearance === 'base';   /* (base: the same opt-in, its newer name) */
            /* (the UA sheet's options-in-a-picker rules go by this) */
            if (base !== sel.hasAttribute('data-cyber-base')) { if (base) sel.setAttribute('data-cyber-base', ''); else sel.removeAttribute('data-cyber-base'); }
            var size = parseInt(sel.getAttribute('size'), 10);
            var listbox = !base && ((sel.multiple && size !== 1) || size > 1);   /* (multiple with a size of 1: a drop-down, whatwg/html 8189) */
            var so = selectedOf(sel);
            /* (:checked follows what is selected) */
            for (var i = 0; i < so.opts.length; i++) {
                global.__cyberElState(so.opts[i], 128, true);
                global.__cyberElState(so.opts[i], 64, sel.multiple ? optionSelected(so.opts[i]) : i === so.idx);
            }
            /* (<selectedcontent>: the selected option's content, when that changed) */
            updateSelectedContent(sel);
            var html, key;
            if (base && ((sel.multiple && size !== 1) || size > 1)) {
                var lbRows = size > 1 ? size : 4;
                key = 'blb|' + lbRows;
                html = '<style>' + baseListbox(lbRows) + '</style><slot name="-cb-rest"></slot>';
            } else if (base) {
                var btn = null;
                /* (only a first element child <button>: one later is the picker's content) */
                var c0 = sel.firstElementChild;
                if (c0 && c0.localName === 'button' && c0.namespaceURI === 'http://www.w3.org/1999/xhtml') btn = c0;
                /* (the UA sheet's rule for it goes by this; a button that no longer is it, unmarked) */
                for (var bc = sel.firstElementChild; bc; bc = bc.nextElementSibling) {
                    if (bc === btn) { if (!bc.hasAttribute('data-cyber-selbtn')) bc.setAttribute('data-cyber-selbtn', ''); }
                    else if (bc.localName === 'button' && bc.hasAttribute('data-cyber-selbtn')) bc.removeAttribute('data-cyber-selbtn');
                }
                /* (an option's label attribute: what the picker shows of it, its text hidden) */
                var bfs = 0, anyLbl = false;
                so.opts.forEach(function (o) {
                    var lv = o.getAttributeNS(null, 'label');
                    /* (an empty one shows its label already) */
                    if (lv !== null && lv !== '' && /\S/.test(o.textContent)) {
                        anyLbl = true;
                        /* (its own font size, read before the mark takes it to 0) */
                        /* (the UA's 13.333px is :host { font: inherit } in the tree: the parent's then) */
                        if (!o.hasAttribute('data-cyber-lbl')) {
                            try {
                                var ofs = parseFloat(global.getComputedStyle(o).fontSize);
                                if (Math.abs(ofs - 13.333) < 0.01 && sel.parentElement) ofs = parseFloat(global.getComputedStyle(sel.parentElement).fontSize) || ofs;
                                if (ofs > 0) bfs = bfs || ofs;
                            } catch (e) { }
                        }
                        if (o.getAttribute('data-cyber-lbl') !== lv) o.setAttribute('data-cyber-lbl', lv);
                    }
                    else if (o.hasAttribute('data-cyber-lbl')) o.removeAttribute('data-cyber-lbl');
                });
                if (anyLbl) { if (bfs) Object.defineProperty(sel, LBLFS, { value: bfs, configurable: true, writable: true }); else bfs = sel[LBLFS] || parseFloat(cs.fontSize) || 13.333; }
                /* (its em lengths in px: the font size of 0 would take them) */
                /* (its hidden text an item of its own still: the gap only before the label) */
                var BLBL = anyLbl ? '::slotted(option[data-cyber-lbl]) { font-size: 0 !important; padding-inline: ' + (bfs / 2) + 'px; gap: 0; min-block-size: max(24px, ' + Math.round(bfs * 1.15) + 'px); }' +
                    '::slotted(option[data-cyber-lbl])::checkmark { font-size: ' + bfs + 'px; }' +
                    '::slotted(option[data-cyber-lbl])::after { content: attr(data-cyber-lbl); font-size: ' + bfs + 'px; margin-inline-start: ' + (bfs / 2) + 'px; }' : '';
                /* (the icon disclosure-open's: pointing along the block axis, upright in vertical text) */
                var wmv = '';
                try { wmv = cs.getPropertyValue('writing-mode') || cs.writingMode || ''; } catch (e) { }
                var icon = /vertical-lr|sideways-lr/.test(wmv) ? '\u25b8' : /vertical-rl|sideways-rl/.test(wmv) ? '\u25c2' : '\u25be';
                /* (multiple, a drop-down: the one selected option's label, else how many are) */
                var blbl = so.opt ? labelOf(so.opt) : '';
                if (sel.multiple) {
                    var sels_ = so.opts.filter(function (o) { return optionSelected(o); });
                    blbl = sels_.length > 1 ? sels_.length + ' selected' : sels_.length ? labelOf(sels_[0]) : '';
                }
                key = 'b|' + (btn ? 1 : 0) + '|' + blbl + '|' + !!sel[UAOPEN] + '|' + (anyLbl ? bfs : 0) + '|' + icon;
                /* (no label: no line of it, the box its minimum sizes) */
                var EMPTY = ':host([data-cyber-nolabel]) { min-inline-size: 24px; } :host([data-cyber-nolabel]) .l { display: none; }';
                html = '<style>' + BASE + BLBL + EMPTY + '</style>' + (btn ? '<slot name="-cb-button"></slot>' : '<span class="l" part="cbselectedcontent">' + esc(blbl) + '</span>') +
                    '<span class="icon" part="cbpickericon">' + icon + '</span><div class="picker" part="cbpicker" popover="manual"><slot name="-cb-rest"></slot></div>';

            } else if (listbox) {
                var rows = size > 1 ? size : 4;
                var fs = parseFloat(cs.fontSize) || 13.333;
                var rowH = Math.round(fs * 1.15 + 0.5) + 0;
                var bpb = (parseFloat(cs.borderTopWidth) || 0) + (parseFloat(cs.borderBottomWidth) || 0) + (parseFloat(cs.paddingTop) || 0) + (parseFloat(cs.paddingBottom) || 0);
                var hset = sel.style.height || '';
                key = 'l|' + rows + '|' + rowH + '|' + bpb + '|' + fs;
                /* (an option with a label: the label shown in place of its text) */
                /* (the label attribute, no namespace, not empty: marked for the sheet, the row its height still) */
                so.opts.forEach(function (o) {
                    var lv = o.getAttributeNS(null, 'label');
                    /* (no label, an image in it: its text with the image's alt text, shown the same way) */
                    if ((lv === null || lv === '') && o.querySelector && o.querySelector('img')) {
                        var out = '';
                        (function walk(n) {
                            for (var c = n.firstChild; c; c = c.nextSibling) {
                                if (c.nodeType === 3) out += c.data;
                                else if (c.nodeType === 1) {
                                    var ln = c.localName;
                                    if (ln === 'script' || ln === 'style' || ln === 'template') continue;
                                    if (ln === 'img') { out += c.getAttribute('alt') || ''; continue; }
                                    walk(c);
                                }
                            }
                        })(o);
                        lv = out.replace(/[ \t\n\f\r]+/g, ' ').trim();
                    }
                    if (lv !== null && lv !== '') { if (o.getAttribute('data-cyber-lbl') !== lv) o.setAttribute('data-cyber-lbl', lv); }
                    else if (o.hasAttribute('data-cyber-lbl')) o.removeAttribute('data-cyber-lbl');
                });
                /* (an optgroup's label: its first legend's text, else its label attribute, a row of its own) */
                Array.prototype.forEach.call(sel.querySelectorAll('optgroup'), function (g) {
                    var lg = null;
                    for (var c = g.firstElementChild; c && !lg; c = c.nextElementSibling) if (c.localName === 'legend') lg = c;
                    var gl = lg ? lg.textContent.replace(/[ \t\n\f\r]+/g, ' ').trim() : (g.getAttribute('label') || '');
                    if (g.getAttribute('data-cyber-oglbl') !== gl) g.setAttribute('data-cyber-oglbl', gl);
                });
                var LBL = '::slotted(option[data-cyber-lbl]) { font-size: 0 !important; min-block-size: ' + rowH + 'px !important; }' +
                    '::slotted(option[data-cyber-lbl])::before { content: attr(data-cyber-lbl); font-size: ' + fs + 'px; }';
                html = '<style>' + LIST + LBL + (cs.boxSizing === 'border-box' ? ':host { height: ' + (rows * rowH + bpb) + 'px; }' : ':host { height: ' + (rows * rowH) + 'px; }') + '</style><slot></slot>';
                void hset;
            } else {
                /* (as shown: its label, else its text with its images' alt text in it) */
                var dispLabel = function (o) {
                    var l = o.getAttributeNS(null, 'label');
                    if (l !== null && l !== '') return l;
                    if (!o.querySelector || !o.querySelector('img')) return labelOf(o);
                    var out = '';
                    (function walk(n) {
                        for (var c = n.firstChild; c; c = c.nextSibling) {
                            if (c.nodeType === 3) out += c.data;
                            else if (c.nodeType === 1) {
                                var ln = c.localName;
                                if (ln === 'script' || ln === 'style' || ln === 'template') continue;
                                if (ln === 'img') { out += c.getAttribute('alt') || ''; continue; }
                                walk(c);
                            }
                        }
                    })(o);
                    return out.replace(/[ \t\n\f\r]+/g, ' ').trim();
                };
                var labels = so.opts.map(dispLabel);
                var shown = so.opt ? dispLabel(so.opt) : '';
                /* (a first child <button>: its text the label, <selectedcontent> the option's; the widest of those the width,
                 * whatwg/html#12474) */
                var fb = sel.firstElementChild;
                if (fb && fb.localName === 'button' && fb.namespaceURI === 'http://www.w3.org/1999/xhtml') {
                    var btnText = function (o) {
                        var out = '';
                        (function walk(n) {
                            for (var c = n.firstChild; c; c = c.nextSibling) {
                                if (c.nodeType === 3) out += c.data;
                                else if (c.nodeType === 1) {
                                    var ln = c.localName;
                                    if (ln === 'script' || ln === 'style' || ln === 'template') continue;
                                    /* (the selected option's: what the selectedcontent holds now, script may have changed it) */
                                    if (ln === 'selectedcontent') { if (o && o === so.opt && c.firstChild) walk(c); else out += o ? labelOf(o) : ''; continue; }
                                    if (ln === 'img') { out += c.getAttribute('alt') || ''; continue; }
                                    walk(c);
                                }
                            }
                        })(fb);
                        return out.replace(/[ \t\n\f\r]+/g, ' ').trim();
                    };
                    shown = btnText(so.opt);
                    labels = so.opts.length ? so.opts.map(btnText) : [shown];
                }
                /* (size containment: sized as an empty one, its arrow kept) */
                var sizeContained = /\b(size|strict)\b/.test(cs.contain || '');
                if (sizeContained) { labels = []; }
                key = 'd|' + shown + '|' + labels.join('\u0001');
                /* (as wide as its widest label) */
                var mw = 0;
                try {
                    if (!measureCtx) measureCtx = global.document.createElement('canvas').getContext('2d');
                    measureCtx.font = cs.fontStyle + ' ' + cs.fontWeight + ' ' + cs.fontSize + ' ' + cs.fontFamily;
                    var tt = cs.textTransform;
                    labels.forEach(function (t) {
                        if (tt === 'uppercase') t = t.toUpperCase();
                        else if (tt === 'lowercase') t = t.toLowerCase();
                        else if (tt === 'capitalize') t = t.replace(/(^|\s)(\S)/g, function (m, a, b) { return a + b.toUpperCase(); });
                        var w = measureCtx.measureText(t).width; if (w > mw) mw = w;
                    });
                } catch (e) { }
                key += '|' + mw;
                key += "|" + sizeContained;
                var plain = cs.appearance === 'none';   /* (appearance: none: no arrow, its line-height its own) */
                key += '|' + plain;
                html = '<style>' + DROP + (plain ? '' : ':host { line-height: normal !important; }') + '.l { display: inline-block; min-inline-size: ' + Math.ceil(mw) + 'px; }' +
                    /* (its label the whole line, whatever the select's own flex or grid alignment says) */
                    (plain ? '.l { flex: 1 0 auto; justify-self: stretch; }' : '') + (sizeContained ? ':host { min-inline-size: 18px; }' : '') + '</style><span class="l">' + (shown === '' ? '​' : esc(shown)) + '</span>' + (plain ? '' : '<span class="a"></span>');
            }
            if (!root) {
                try { root = sel.attachShadow({ mode: 'closed', __cyberUA: true }); } catch (e) { return; }
                Object.defineProperty(sel, UA, { value: root, configurable: true });
            }
            if (sel[UAKEY] !== key) {
                Object.defineProperty(sel, UAKEY, { value: key, configurable: true, writable: true });
                root.innerHTML = html;
                if (sel[ICONSTY]) iconStyle(sel);
                if (base && sel[UAOPEN]) {
                    var pk = root.querySelector('.picker');
                    try { pk.showPopover({ source: sel }); } catch (e) { try { pk.showPopover(); } catch (e2) { } }
                    /* (a picker not opted in by ::picker(select) { appearance: base-select }: the platform's own, not in the page) */
                    var pap = '';
                    try { pap = global.getComputedStyle(pk).appearance; } catch (e) { }
                    if (pap !== 'base-select' && pap !== 'base') pk.style.setProperty('visibility', 'hidden', 'important');
                }
            }
            /* (::picker-icon's content: none, no icon; a string, that in its place) */
            if (base) {
                var ic = root.querySelector('.icon');
                if (ic) {
                    var icv = '';
                    try { icv = global.getComputedStyle(ic).content || ''; } catch (e) { }
                    var want = typeof icon === 'string' && icon ? icon : '\u25be', hide = false, sm = /^"((?:[^"\\]|\\.)*)"$/.exec(icv);
                    if (icv === 'none') hide = true;
                    else if (sm) want = sm[1].replace(/\\(.)/g, '$1');
                    if (ic.textContent !== want) ic.textContent = want;
                    if (hide !== (ic.style.display === 'none')) ic.style.display = hide ? 'none' : '';
                }
                /* (no label and no icon shown: nothing of a line in it, layout sizes it by its minimums alone) */
                var nolbl_ = false;
                if (!root.querySelector('slot[name="-cb-button"]')) {
                    var lb_ = root.querySelector('.l');
                    var icd_ = '';
                    try { icd_ = ic ? global.getComputedStyle(ic).display : 'none'; } catch (e) { }
                    nolbl_ = (!lb_ || lb_.textContent === '') && (!ic || icd_ === 'none');
                }
                if (nolbl_ !== sel.hasAttribute('data-cyber-nolabel')) { if (nolbl_) sel.setAttribute('data-cyber-nolabel', ''); else sel.removeAttribute('data-cyber-nolabel'); }
            }
            /* (its picker open: :open, through an attribute the UA sheet reads) */
            var op_ = !!(base && sel[UAOPEN]);
            if (op_ !== sel.hasAttribute('data-cyber-open')) { if (op_) sel.setAttribute('data-cyber-open', ''); else sel.removeAttribute('data-cyber-open'); }
        }
        var measureCtx = null;
        var pending = false;
        var mo = null;
        function renderAll() {
            pending = false;
            var doc = global.document;
            if (!doc) return;
            /* (the observer made now, at run time: setup is restored from a snapshot for each page) */
            if (!mo) {
                try {
                    mo = new global.MutationObserver(schedule);
                    mo.observe(doc, { childList: true, subtree: true, attributes: true, characterData: true });
                } catch (e) { }
            }
            var sels = doc.getElementsByTagName('select');
            for (var i = 0; i < sels.length; i++) { if (sels[i].namespaceURI !== 'http://www.w3.org/1999/xhtml') continue; try { render(sels[i]); } catch (e) { } }
            /* (headings whose level headingoffset moves: that level, for :heading() and the UA sheet) */
            if (doc.querySelector('[headingoffset], [headingreset]') || doc.querySelector('[data-cyber-hl]')) {
                var hs = doc.querySelectorAll('h1, h2, h3, h4, h5, h6');
                for (var hi = 0; hi < hs.length; hi++) {
                    var hel = hs[hi], lvl = +hel.localName.charAt(1), off = 0;
                    for (var up = hel; up && up.nodeType === 1; up = up.parentElement) {
                        var ov = up.getAttribute('headingoffset');
                        if (ov !== null) { var on = parseInt(ov, 10); if (on > 0) off += on; }
                        if (up !== hel && up.hasAttribute('headingreset')) break;
                        if (up === hel && hel.hasAttribute('headingreset')) break;
                    }
                    var eff = Math.min(9, lvl + off);
                    if (eff !== lvl) { if (hel.getAttribute('data-cyber-hl') !== String(eff)) hel.setAttribute('data-cyber-hl', String(eff)); }
                    else if (hel.hasAttribute('data-cyber-hl')) hel.removeAttribute('data-cyber-hl');
                }
            }
            /* (a details element: its UA tree, its summary and the rest in ::details-content (::part(cbdetailscontent)); made
             * only where a sheet names that, other details as they were) */
            /* (or where its summary is inline-level: the rest a block after it, not on its line) */
            var dcu = detailsContentUsed(doc);
            {
                var dts = doc.getElementsByTagName('details');
                for (var di = 0; di < dts.length; di++) {
                    var de = dts[di];
                    if (de.namespaceURI !== 'http://www.w3.org/1999/xhtml' || de[DETUA] || de.shadowRoot) continue;
                    if (!dcu) {
                        var sm_ = null;
                        for (var dc_ = de.firstElementChild; dc_ && !sm_; dc_ = dc_.nextElementSibling) if (dc_.localName === 'summary') sm_ = dc_;
                        var sd_ = '';
                        try { sd_ = sm_ ? global.getComputedStyle(sm_).display : ''; } catch (e) { }
                        if (!/^inline/.test(sd_)) continue;
                    }
                    try {
                        var dr = de.attachShadow({ mode: 'closed', __cyberUA: true });
                        Object.defineProperty(de, DETUA, { value: dr, configurable: true });
                        de.setAttribute('data-cyber-dua', '');   /* (its closed state the UA tree's, not the light-tree rule's) */
                        dr.innerHTML = '<style>[part~=cbdetailscontent] { display: block; } :host(:not([open])) [part~=cbdetailscontent] { content-visibility: hidden; } ::slotted(summary) { display: list-item; counter-increment: list-item 0; list-style: disclosure-closed inside; } :host([open]) ::slotted(summary) { list-style-type: disclosure-open; }</style><slot name="-cb-summary"></slot><div part="cbdetailscontent"><slot name="-cb-rest"></slot></div>';
                    } catch (e) { }
                }
            }
            for (var pt = 0; pt < PERM_TAGS.length; pt++) {
                var pe = doc.getElementsByTagName(PERM_TAGS[pt]);
                for (var j = 0; j < pe.length; j++) { if (pe[j].namespaceURI === 'http://www.w3.org/1999/xhtml') { try { permRender(pe[j]); } catch (e) { } } }
            }
        }
        /* (the permission elements (WICG PEPC): a label and an icon, the icon ::permission-icon (::part(cbpermicon)),
         * only some of its properties the page's) */
        var PERM_TAGS = ['usermedia', 'geolocation', 'permission', 'camera', 'microphone'];
        /* (a layout query while the document loads: the permission elements parsed so far rendered first) */
        Object.defineProperty(global, '__cyberUAEarly', { configurable: true, value: function () {
            var doc = global.document;
            if (!doc) return;
            for (var pt = 0; pt < PERM_TAGS.length; pt++) {
                var pe = doc.getElementsByTagName(PERM_TAGS[pt]);
                for (var j = 0; j < pe.length; j++) if (pe[j].namespaceURI === 'http://www.w3.org/1999/xhtml' && !pe[j][PERMUA]) { try { permRender(pe[j]); } catch (e) { } }
            }
        } });
        var DETUA = Symbol('detailsUA');
        function detailsContentUsed(doc) {
            var ss = doc.styleSheets;
            for (var i = 0; ss && i < ss.length; i++) {
                var o = ss[i].ownerNode;
                if (o && o.localName === 'style' && /details-content/.test(o.textContent || '')) return true;
            }
            return false;
        }
        var PERMUA = Symbol('permUA'), PERMKEY = Symbol('permUAkey');
        var PERM_ICON = {
            camera: 'M2 6h13v12H2z M16 10l6-4v12l-6-4z',
            /* (a capsule with a slot in it, the same way round: fill-rule shows) */
            microphone: 'M9 2h6v11H9z M10.5 4h3v7h-3z M5 10h2c0 3 2 5 5 5s5-2 5-5h2c0 4-3 6.5-6 7v3h-2v-3c-3-.5-6-3-6-7z',
            camera_microphone: 'M1 7h10v10H1z M12 10l4-3v10l-4-3z M18 4h3v8h-3z',
            location: 'M12 2c-4 0-7 3-7 7 0 5 7 13 7 13s7-8 7-13c0-4-3-7-7-7z M12 6.5a2.5 2.5 0 1 1 0 5a2.5 2.5 0 1 1 0-5z',
            precise: 'M11 1h2v3h-2z M11 20h2v3h-2z M1 11h3v2H1z M20 11h3v2h-3z M12 5a7 7 0 1 1 0 14a7 7 0 1 1 0-14z M12 8a4 4 0 1 1 0 8a4 4 0 1 1 0-8z'
        };
        var PERM_LABEL = { camera: 'Use camera', microphone: 'Use microphone', camera_microphone: 'Use camera and microphone', location: 'Use location', precise: 'Use precise location' };
        var PERM_CSS = ':host { display: inline-flex; align-items: center; white-space: nowrap; vertical-align: middle; }' +
            '*::before, *::after { content: none !important; display: none !important; }' +
            
            '.i { margin-inline-start: 0 !important; margin-block: 0 !important; padding: 0 !important; float: none !important; position: static !important; opacity: 1 !important; stroke-opacity: 1 !important; fill-opacity: 1 !important; max-height: 1.5em !important; max-width: 1.5em !important; transform: none !important; }';
        function permKind(el) {
            var ln = el.localName, ty = String(el.getAttribute('type') || '').toLowerCase().split(/\s+/).filter(Boolean);
            if (ln === 'geolocation') return /^precise$/i.test(el.getAttribute('accuracymode') || '') ? 'precise' : 'location';
            if (ln === 'camera') return 'camera';
            if (ln === 'microphone') return 'microphone';
            var cam = ty.indexOf('camera') >= 0, mic = ty.indexOf('microphone') >= 0;
            if (ln === 'permission' && ty.indexOf('geolocation') >= 0) return 'location';
            if (cam && mic) return 'camera_microphone';
            if (mic) return 'microphone';
            return 'camera';
        }
        function permRender(el) {
            if (!el.isConnected) return;
            var k = permKind(el);
            var root = el[PERMUA];
            if (!root) {
                if (el.shadowRoot) return;
                try { root = el.attachShadow({ mode: 'closed', __cyberUA: true }); } catch (e) { return; }
                Object.defineProperty(el, PERMUA, { value: root, configurable: true });
            }
            if (el[PERMKEY] !== k) {
                Object.defineProperty(el, PERMKEY, { value: k, configurable: true, writable: true });
                root.innerHTML = '<style>' + PERM_CSS + '</style><style class="b"></style><svg class="i" part="cbpermicon" viewBox="0 0 24 24" height="1em" fill="currentColor" fill-rule="evenodd" aria-hidden="true"><path d="' + PERM_ICON[k] + '"/></svg>';
            }
            permBounds(el, root);
        }
        /* (its size bounds, as the permission element's: a min-height of at most 3em, a max-height of at least 1em,
         * a min-width of at most three times its fit-content width, a max-width of at least that width) */
        function permBounds(el, root) {
            var bs = root.querySelector('style.b'), ic = root.querySelector('svg');
            if (!bs || !ic) return;
            var rules = '', icr = '';
            try {
                bs.textContent = '';
                var cs = global.getComputedStyle(el), fs = parseFloat(cs.fontSize) || 16;
                var px = function (v) { var n = parseFloat(v); return isFinite(n) ? n : NaN; };
                var bx = px(cs.paddingLeft) + px(cs.paddingRight) + px(cs.borderLeftWidth) + px(cs.borderRightWidth);
                /* (the icon's end margin: at most 3em) */
                var ics = global.getComputedStyle(ic), ime = px(ics.marginInlineEnd || ics.marginRight);
                if (ime > 3 * fs) { icr = '.i { margin-inline-end: ' + (3 * fs) + 'px !important; }'; ime = 3 * fs; }
                var fc = Math.max(ic.getBoundingClientRect().width, 1.5 * fs) + (isFinite(ime) ? ime : 0) + (isFinite(bx) ? bx : 0);   /* (the icon: no width of its own, as wide as its 1.5em limit) */
                /* (each within [1em, 3em] tall and [its fit-content width, three times that] wide, the author's where it is inside) */
                var clamp = function (v, lo, hi, d) { if (!isFinite(v)) v = d; return v < lo ? lo : v > hi ? hi : v; };
                /* (heights: only an author's out of those bounds pulled in) */
                var mnh0 = px(cs.minHeight), mxh0 = px(cs.maxHeight);
                if (mnh0 > 3 * fs) rules += 'min-height: ' + (3 * fs) + 'px !important;';
                if (mxh0 < fs) rules += 'max-height: ' + fs + 'px !important;';
                if (fc > 0) {
                    var mnw = clamp(px(cs.minWidth), fc, 3 * fc, 0), mxw = clamp(px(cs.maxWidth), fc, 3 * fc, Infinity);
                    rules += 'min-width: ' + mnw + 'px !important; max-width: ' + mxw + 'px !important;';
                }
            } catch (e) { }
            if (rules || icr) bs.textContent = (rules ? ':host { ' + rules + ' }' : '') + icr;
        }
        function schedule() {
            if (pending) return;
            pending = true;
            Promise.resolve().then(renderAll);
        }
        global.addEventListener('DOMContentLoaded', renderAll);
        /* (a customizable combobox, input[list] and datalist of appearance base: the input's own look, and the down arrow
         * shows the list below it) */
        var comboBase = function (inp) {
            try { return global.getComputedStyle(inp).appearance === 'base'; } catch (e) { return false; }
        };
        global.addEventListener('keydown', function (e) {
            var t = e.target;
            /* (a filterable select's input, input[filter]: the down arrow makes the select's next option the active one) */
            if (t && t.localName === 'input' && t.hasAttribute('filter') && e.key === 'ArrowDown' && comboBase(t)) {
                var fsel = null;
                try { var rt_ = t.getRootNode(); fsel = rt_ && rt_.getElementById ? rt_.getElementById(t.getAttribute('filter')) : null; } catch (x) { fsel = null; }
                if (fsel && fsel.localName === 'select') {
                    if (!t.hasAttribute('data-cyber-base')) t.setAttribute('data-cyber-base', '');
                    var fo = Array.prototype.filter.call(fsel.getElementsByTagName('option'), function (o) { return !o.disabled; });
                    var fc = 0;
                    for (var fi = 0; fi < fo.length; fi++) if (fo[fi].hasAttribute('data-cyber-active')) fc = fi;
                    var fn = Math.min(fc + 1, fo.length - 1);
                    for (var fj = 0; fj < fo.length; fj++) if (fj !== fn && fo[fj].hasAttribute('data-cyber-active')) fo[fj].removeAttribute('data-cyber-active');
                    if (fo.length && !fo[fn].hasAttribute('data-cyber-active')) fo[fn].setAttribute('data-cyber-active', '');
                    e.preventDefault();
                }
                return;
            }
            if (!t || t.localName !== 'input' || !t.hasAttribute('list') || e.key !== 'ArrowDown') return;
            var dl = null;
            try { dl = t.list; } catch (x) { dl = null; }
            if (!dl || !comboBase(t) || !comboBase(dl)) return;
            if (!t.hasAttribute('data-cyber-base')) t.setAttribute('data-cyber-base', '');
            var ik = t.getAttribute('data-cyber-iasrc') || String(++global.__cyberIaSeq || (global.__cyberIaSeq = 1));
            if (t.getAttribute('data-cyber-iasrc') !== ik) t.setAttribute('data-cyber-iasrc', ik);
            if (dl.getAttribute('data-cyber-ia') !== ik) dl.setAttribute('data-cyber-ia', ik);
            /* (the next option the active one, the first to begin with) */
            var opts_ = Array.prototype.filter.call(dl.getElementsByTagName('option'), function (o) { return !o.disabled; });
            var cur_ = -1;
            for (var oi_ = 0; oi_ < opts_.length; oi_++) if (opts_[oi_].hasAttribute('data-cyber-active')) cur_ = oi_;
            var opened_ = dl.hasAttribute('data-cyber-open');
            var nx_ = !opened_ ? 0 : Math.min(cur_ + 1, opts_.length - 1);
            if (opts_.length && nx_ !== cur_) {
                if (cur_ >= 0) opts_[cur_].removeAttribute('data-cyber-active');
                opts_[nx_].setAttribute('data-cyber-active', '');
            }
            if (!opened_) dl.setAttribute('data-cyber-open', '');
            e.preventDefault();
        }, true);
        global.addEventListener('focusin', function (e) {
            var t = e.target;
            if (t && t.localName === 'input' && (t.hasAttribute('list') || t.hasAttribute('filter')) && comboBase(t) && !t.hasAttribute('data-cyber-base')) t.setAttribute('data-cyber-base', '');
        }, true);
        /* (style sheet links the parser made: their load events, once each, those script inserted fire natively) */
        var linkFired = typeof WeakSet === 'function' ? new WeakSet() : null;
        global.addEventListener('load', function (e) { var t = e.target; if (linkFired && t && t.localName === 'link') linkFired.add(t); }, true);
        global.addEventListener('DOMContentLoaded', function () {
            var doc = global.document;
            if (!doc || !linkFired) return;
            var links = Array.prototype.slice.call(doc.querySelectorAll('link[rel~="stylesheet" i][href]'));
            global.setTimeout(function () {
                links.forEach(function (l) {
                    if (linkFired.has(l) || !l.isConnected) return;
                    linkFired.add(l);
                    try { l.dispatchEvent(new global.Event('load')); } catch (e) { }
                });
            }, 0);
        });
        global.addEventListener('load', renderAll);
        global.addEventListener('change', schedule, true);
        global.addEventListener('input', schedule, true);
        /* (selectedIndex / value / selected set by script change no attribute) */
        ['selectedIndex', 'value'].forEach(function (k) {
            var d = Object.getOwnPropertyDescriptor(SP, k);
            if (d && d.set) Object.defineProperty(SP, k, { get: d.get, set: function (v) { d.set.call(this, v); schedule(); }, configurable: true, enumerable: true });
        });
        var od = OP && Object.getOwnPropertyDescriptor(OP, 'selected');
        if (od && od.set) Object.defineProperty(OP, 'selected', { get: od.get, set: function (v) { od.set.call(this, v); schedule(); }, configurable: true, enumerable: true });
        /* showPicker: a base-select's picker opens */
        Object.defineProperty(SP, 'showPicker', { configurable: true, writable: true, value: function showPicker() {
            if (this.disabled) throw new global.DOMException('The select is disabled.', 'InvalidStateError');
            setOpen(this, true);
        } });
        function setOpen(sel, on) {
            if (!!sel[UAOPEN] === on) return;
            Object.defineProperty(sel, UAOPEN, { value: on, configurable: true, writable: true });
            Object.defineProperty(sel, UAKEY, { value: '', configurable: true, writable: true });
            render(sel);
        }
        /* (a base-select by the pointer: pressed, its picker opens or closes; an option in it clicked, chosen and the picker
         * closed; pressed anywhere else, an open one closes) */
        /* (a base-select: as its renderer marks it, made now when it has not run yet, before the document's loaded) */
        function isBase(sel) {
            if (!sel.hasAttribute('data-cyber-base') && !sel[UA]) { try { render(sel); } catch (x) { } }
            return sel.hasAttribute('data-cyber-base');
        }
        function baseSelectOf(path) {
            for (var i = 0; i < path.length; i++) {
                var n = path[i];
                if (n && n.localName === 'select' && n.namespaceURI === 'http://www.w3.org/1999/xhtml') return isBase(n) ? { sel: n, at: i } : null;
            }
            /* (a target in its UA tree, the path not reaching it: up through the shadow root to its host) */
            for (var u = path[0], d = 0; u && d < 64; d++) {
                if (u.nodeType === 1 && u.localName === 'select' && u.namespaceURI === 'http://www.w3.org/1999/xhtml')
                    return isBase(u) ? { sel: u, at: path.length } : null;
                u = u.parentNode || (u.nodeType === 11 ? u.host : null);
            }
            return null;
        }
        function optionIn(path, upto) {
            for (var i = 0; i < upto; i++) if (path[i] && path[i].localName === 'option') return path[i];
            return null;
        }
        global.addEventListener('mousedown', function (e) {
            if (e.button !== 0) return;
            var path = e.composedPath ? e.composedPath() : [e.target];
            var hit = baseSelectOf(path);
            var doc = global.document, open = doc ? doc.querySelectorAll('select[data-cyber-open]') : [];
            for (var i = 0; i < open.length; i++) if (!hit || open[i] !== hit.sel) setOpen(open[i], false);
            if (!hit || hit.sel.disabled || hit.sel.matches(':disabled')) return;
            if (e.defaultPrevented) return;   /* (the page's: no picker opened or closed) */
            if (hit.sel.multiple || parseInt(hit.sel.getAttribute('size'), 10) > 1) return;   /* (a list box: no picker) */
            if (hit.sel[UAOPEN] && optionIn(path, hit.at)) return;   /* (closed: an option the hit test found is the select's, its picker not shown) */
            /* (in its picker but on no option: nothing) */
            for (var k = 0; k < hit.at; k++) if (path[k] && path[k].getAttribute && path[k].getAttribute('part') === 'cbpicker') return;
            setOpen(hit.sel, !hit.sel[UAOPEN]);
            /* (the click this press ends in: not a choice in the picker it opened) */
            Object.defineProperty(hit.sel, JUSTOPENED, { value: !!hit.sel[UAOPEN], configurable: true, writable: true });
            if (hit.sel[UAOPEN]) focusPickerOption(hit.sel);
        }, false);
        /* (the option the picker's focus goes to: the selected one, else the first enabled) */
        function focusPickerOption(sel, which) {
            var opts = optionsOf(sel).filter(function (o) { return !o.disabled && !(o.matches && o.matches(':disabled')); });
            if (!opts.length) return;
            var cur = opts.indexOf(global.document.activeElement), so = selectedOf(sel).opt, t;
            if (which === undefined) t = so && opts.indexOf(so) >= 0 ? so : opts[0];
            else if (which === 'first') t = opts[0];
            else if (which === 'last') t = opts[opts.length - 1];
            else { var base = cur >= 0 ? cur : (so ? opts.indexOf(so) : 0); var ni = base + which; if (ni < 0) ni = 0; if (ni >= opts.length) ni = opts.length - 1; t = opts[ni]; }
            try { t.focus(); } catch (x) { }
        }
        function chooseOption(sel, o) {
            if (o && !o.selected) {
                o.selected = true;
                try { sel.dispatchEvent(new global.Event('input', { bubbles: true, composed: true })); } catch (x) { }
                try { sel.dispatchEvent(new global.Event('change', { bubbles: true })); } catch (x) { }
            }
        }
        /* (a base-select drop-down by the keyboard: closed, space, enter and the arrows open it; open, the arrows, Home and
         * End move among its options, enter and space choose, escape and tab close) */
        global.addEventListener('keydown', function (e) {
            if (e.defaultPrevented) return;
            var path = e.composedPath ? e.composedPath() : [e.target];
            var hit = baseSelectOf(path);
            if (!hit) return;
            var sel = hit.sel;
            if (sel.disabled || sel.multiple || parseInt(sel.getAttribute('size'), 10) > 1) return;
            var k = e.key;
            if (!sel[UAOPEN]) {
                if (hit.at !== 0 && !(path[hit.at - 1] && path[hit.at - 1].localName === 'button')) return;
                if (k === ' ' || k === 'Enter' || k === 'ArrowDown' || k === 'ArrowUp' || k === 'ArrowLeft' || k === 'ArrowRight') {
                    e.preventDefault();
                    setOpen(sel, true);
                    focusPickerOption(sel);
                }
                return;
            }
            var o = optionIn(path, hit.at);
            if (k === 'ArrowDown') { e.preventDefault(); focusPickerOption(sel, 1); }
            else if (k === 'ArrowUp') { e.preventDefault(); focusPickerOption(sel, -1); }
            else if (k === 'Home' || k === 'PageUp') { e.preventDefault(); focusPickerOption(sel, 'first'); }
            else if (k === 'End' || k === 'PageDown') { e.preventDefault(); focusPickerOption(sel, 'last'); }
            else if (k === 'Enter' || k === ' ') {
                e.preventDefault();
                if (o && !o.disabled) chooseOption(sel, o);
                setOpen(sel, false);
                try { sel.focus(); } catch (x) { }
            } else if (k === 'Escape') { e.preventDefault(); setOpen(sel, false); try { sel.focus(); } catch (x) { } }
            else if (k === 'Tab') { setOpen(sel, false); }
        }, false);
        /* (a list box, multiple or of a size over 1, by the pointer: the option pressed chosen, input and change after) */
        global.addEventListener('mousedown', function (e) {
            if (e.button !== 0 || e.defaultPrevented) return;
            var path = e.composedPath ? e.composedPath() : [e.target];
            var sel = null, opt = null;
            for (var i = 0; i < path.length && !sel; i++) {
                var n = path[i];
                if (!n || n.nodeType !== 1) continue;
                if (n.localName === 'option' && !opt) opt = n;
                if (n.localName === 'select' && n.namespaceURI === 'http://www.w3.org/1999/xhtml') sel = n;
            }
            if (!sel || !opt || !(sel.multiple || parseInt(sel.getAttribute('size'), 10) > 1)) return;
            if (sel.disabled || opt.disabled || (opt.matches && opt.matches(':disabled'))) return;
            var before = optionsOf(sel).map(function (o) { return optionSelected(o); }).join();
            if (sel.multiple && (e.ctrlKey || e.metaKey)) setSelected(opt, !optionSelected(opt));
            else {
                optionsOf(sel).forEach(function (o) { if (o !== opt) setSelected(o, false); });
                setSelected(opt, true);
            }
            var after = optionsOf(sel).map(function (o) { return optionSelected(o); }).join();
            if (before === after) return;
            var tr = typeof global.__cyberDispatchTrusted === 'function' ? global.__cyberDispatchTrusted : null;
            var ie = new global.Event('input', { bubbles: true, composed: true }), ce = new global.Event('change', { bubbles: true });
            if (tr) tr(sel, ie); else sel.dispatchEvent(ie);
            if (tr) tr(sel, ce); else sel.dispatchEvent(ce);
            schedule();
        }, false);
        /* (type-ahead: a printable key on a focused select, its picker closed, chooses the next option its label starts with) */
        global.addEventListener('keydown', function (e) {
            if (e.defaultPrevented || e.ctrlKey || e.metaKey || e.altKey || !e.key || e.key.length !== 1 || e.key === ' ') return;
            var sel = e.target;
            if (!sel || sel.localName !== 'select' || sel.namespaceURI !== 'http://www.w3.org/1999/xhtml' || sel.disabled || sel.multiple || sel[UAOPEN]) return;
            var opts = optionsOf(sel), cur = selectedIndexOf(sel, opts), k = e.key.toLowerCase();
            for (var step = 1; step <= opts.length; step++) {
                var o = opts[(cur + step + opts.length) % opts.length];
                if (o.disabled || (o.matches && o.matches(':disabled'))) continue;
                if (labelOf(o).trim().toLowerCase().charAt(0) !== k) continue;
                if (o === opts[cur]) return;
                opts.forEach(function (x) { setSelected(x, x === o); });
                var tr = typeof global.__cyberDispatchTrusted === 'function' ? global.__cyberDispatchTrusted : null;
                var ie = new global.Event('input', { bubbles: true, composed: true }), ce = new global.Event('change', { bubbles: true });
                if (tr) tr(sel, ie); else sel.dispatchEvent(ie);
                if (tr) tr(sel, ce); else sel.dispatchEvent(ce);
                updateSelectedContent(sel, true);
                schedule();
                return;
            }
        }, false);
        /* (::picker-icon animated by script (Element.animate's pseudoElement): its style on the UA tree's icon, kept across renders) */
        var ICONSTY = Symbol('pickerIconStyle');
        function iconStyle(sel) {
            var root = sel[UA], ic = root && root.querySelector('.icon');
            if (!ic) return;
            if (sel[ICONSTY]) ic.setAttribute('style', sel[ICONSTY]); else ic.removeAttribute('style');
        }
        Object.defineProperty(global, '__cyberPickerIconStyle', { configurable: true, value: function (sel, text) {
            if (!sel || sel.localName !== 'select') return;
            Object.defineProperty(sel, ICONSTY, { value: text || '', configurable: true, writable: true });
            iconStyle(sel);
        } });
        var JUSTOPENED = Symbol('selectJustOpened');
        global.addEventListener('click', function (e) {
            if (e.button !== 0) return;
            var path = e.composedPath ? e.composedPath() : [e.target];
            var hit = baseSelectOf(path);
            if (hit && hit.sel[JUSTOPENED]) { hit.sel[JUSTOPENED] = false; return; }
            if (!hit || !hit.sel[UAOPEN]) return;
            var o = optionIn(path, hit.at);
            if (!o || o.disabled || (o.matches && o.matches(':disabled'))) return;
            var sel = hit.sel;
            if (!o.selected) {
                o.selected = true;
                try { sel.dispatchEvent(new global.Event('input', { bubbles: true, composed: true })); } catch (x) { }
                try { sel.dispatchEvent(new global.Event('change', { bubbles: true })); } catch (x) { }
            }
            if (!sel.multiple) setOpen(sel, false);
        }, true);
    })();
    /* ---- Invoker commands (HTML 4.10.6: button command / commandfor, CommandEvent) ---- */
    (function () {
        var BP = proto('HTMLButtonElement');
        if (!BP || !global.Event) return;
        var Ev = global.Event;
        /* CommandEvent */
        var CSRC = Symbol('cmdSource'), CCMD = Symbol('cmdCommand');
        function CommandEvent(type, init) {
            if (!new.target) throw new TypeError("Failed to construct 'CommandEvent': Please use the 'new' operator.");
            if (arguments.length < 1) throw new TypeError("Failed to construct 'CommandEvent': 1 argument required, but only 0 present.");
            var e = Reflect.construct(Ev, [type, init], new.target);
            init = init || {};
            var src = init.source === undefined ? null : init.source;
            if (src !== null && !(src instanceof global.Element)) throw new TypeError("Failed to construct 'CommandEvent': member source is not of type Element.");
            Object.defineProperty(e, CSRC, { value: src, writable: true });
            Object.defineProperty(e, CCMD, { value: init.command === undefined ? '' : String(init.command), writable: true });
            return e;
        }
        CommandEvent.prototype = Object.create(Ev.prototype, { constructor: { value: CommandEvent, writable: true, configurable: true } });
        Object.setPrototypeOf(CommandEvent, Ev);
        Object.defineProperty(CommandEvent.prototype, 'source', { configurable: true, enumerable: true, get: function source() {
            var s = this[CSRC];
            if (!s) return null;
            /* (retargeted against the current target) */
            var ct = this.currentTarget;
            if (ct && ct.getRootNode && s.getRootNode) {
                var r = s.getRootNode();
                while (r && r.host && !(ct === r || (r.contains && r.contains(ct)))) { s = r.host; r = s.getRootNode(); }
            }
            return s;
        } });
        Object.defineProperty(CommandEvent.prototype, 'command', { configurable: true, enumerable: true, get: function command() { return this[CCMD] || ''; } });
        Object.defineProperty(CommandEvent.prototype, Symbol.toStringTag, { configurable: true, value: 'CommandEvent' });
        Object.defineProperty(global, 'CommandEvent', { configurable: true, writable: true, value: CommandEvent });

        var BUILTIN = ['toggle-popover', 'show-popover', 'hide-popover', 'show-modal', 'close', 'request-close', 'toggle', 'open',
                       'step-up', 'step-down', 'play-pause', 'play', 'pause', 'toggle-muted', 'toggle-fullscreen', 'request-fullscreen', 'exit-fullscreen',
                       'page-up', 'page-down', 'page-left', 'page-right', 'page-block-start', 'page-block-end', 'page-inline-start', 'page-inline-end'];
        function commandOf(b) {
            var v = b.getAttribute('command');
            if (v === null) return '';
            if (/^--/.test(v)) return v;
            var low = v.toLowerCase();
            return BUILTIN.indexOf(low) >= 0 ? low : '';
        }
        Object.defineProperty(BP, 'command', { configurable: true, enumerable: true,
            get: function command() { return commandOf(this); },
            set: function command(v) { this.setAttribute('command', String(v)); } });
        function targetOf(b) {
            var t = b[Symbol.for('cyber.commandForElement')];
            if (t !== undefined) return t && t.isConnected && t.getRootNode() === b.getRootNode() ? t : null;
            var id = b.getAttribute('commandfor');
            if (!id) return null;
            var root = b.getRootNode();
            return root && root.getElementById ? root.getElementById(id) : null;
        }
        Object.defineProperty(BP, 'commandForElement', { configurable: true, enumerable: true,
            get: function commandForElement() { return targetOf(this); },
            set: function commandForElement(v) {
                if (v !== null && !(v instanceof global.Element)) throw new TypeError("Failed to set the 'commandForElement' property on 'HTMLButtonElement': Failed to convert value to 'Element'.");
                Object.defineProperty(this, Symbol.for('cyber.commandForElement'), { value: v, configurable: true, writable: true });
                if (v === null) this.removeAttribute('commandfor'); else this.setAttribute('commandfor', '');
            } });
        /* (a button with commandfor and no valid type: a button, not a submit) */
        var td = null;
        for (var tp = BP; tp && !td; tp = Object.getPrototypeOf(tp)) td = Object.getOwnPropertyDescriptor(tp, 'type') || null;
        if (!td || td.get) Object.defineProperty(BP, 'type', { configurable: true, enumerable: true,
            get: function type() {
                var v = (this.getAttribute('type') || '').toLowerCase();
                if (v === 'submit' || v === 'reset' || v === 'button') return v;
                return this.hasAttribute('commandfor') || this.hasAttribute('command') ? 'button' : 'submit';
            }, set: td && td.set || function type(v) { this.setAttribute('type', String(v)); } });
        /* activation: the command fired at its target, then the built-in one done */
        global.addEventListener('click', function (e) {
            if (e.defaultPrevented) return;
            var path = e.composedPath ? e.composedPath() : [e.target], b = null;
            for (var i = 0; i < path.length && !b; i++) if (path[i] && path[i].localName === 'button' && path[i].namespaceURI === 'http://www.w3.org/1999/xhtml') b = path[i];
            if (!b || b.disabled || (b.matches && b.matches(':disabled'))) return;
            var target = targetOf(b);
            if (!target) return;
            var form = b.form, ty = b.type;
            if (form && (ty === 'submit' || ty === 'reset')) return;
            /* (the Auto state in a form: neither a submit nor a command) */
            var tyA = String(b.getAttribute('type') || '').toLowerCase();
            if (form && tyA !== 'button') return;
            var cmd = commandOf(b);
            if (!cmd) return;
            var isDialog = target.localName === 'dialog', isPopover = target.hasAttribute && target.hasAttribute('popover');
            /* (HTML's command steps: an element not HTML takes custom commands only; the dialog ones only a dialog) */
            var custom = /^--/.test(cmd);
            if (!custom && target.namespaceURI !== 'http://www.w3.org/1999/xhtml') return;
            if ((cmd === 'show-modal' || cmd === 'request-close') && !isDialog) return;
            if (cmd === 'close' && !isDialog && target.localName !== 'details') return;
            var ev = new CommandEvent('command', { source: b, command: cmd, cancelable: true, composed: true });
            if (!(global.__cyberDispatchTrusted ? global.__cyberDispatchTrusted(target, ev) : target.dispatchEvent(ev))) return;
            if (cmd === 'toggle-popover' && isPopover) { try { target.togglePopover({ source: b }); } catch (x) { } }
            else if (cmd === 'show-popover' && isPopover) { try { if (!target.matches(':popover-open')) target.showPopover({ source: b }); } catch (x) { } }
            else if (cmd === 'hide-popover' && isPopover) { try { if (target.matches(':popover-open')) target.hidePopover(); } catch (x) { } }
            else if (isDialog && (cmd === 'show-modal' || cmd === 'close' || cmd === 'request-close')) {
                /* (the button: the toggle events' source) */
                var heldSrc = global.__cyberDialogSource;
                global.__cyberDialogSource = b;
                try {
                    if (cmd === 'show-modal') { try { if (!target.open && target.isConnected) target.showModal(); } catch (x) { } }
                    else if (cmd === 'close') { if (target.open) target.close(b.value); }
                    else if (target.open && target.requestClose) target.requestClose(b.value); else if (target.open) target.close(b.value);
                } finally { global.__cyberDialogSource = heldSrc; }
            }
            /* (scroll commands: a page along the axis asked, logical ones by the target's writing mode and direction) */
            else if (/^page-/.test(cmd)) {
                var cs = null;
                try { cs = global.getComputedStyle(target); } catch (x) { }
                var vert = cs && /^vertical|^sideways/.test(String(cs.writingMode || '')), rtl = cs && cs.direction === 'rtl';
                var dx = 0, dy = 0, ph = (target.clientHeight || 0) * 0.875, pw = (target.clientWidth || 0) * 0.875;
                var lg = { 'page-block-start': vert ? 'page-right' : 'page-up', 'page-block-end': vert ? 'page-left' : 'page-down',
                           'page-inline-start': vert ? 'page-up' : (rtl ? 'page-right' : 'page-left'), 'page-inline-end': vert ? 'page-down' : (rtl ? 'page-left' : 'page-right') };
                var phys = lg[cmd] || cmd;
                if (vert && (cmd === 'page-block-start' || cmd === 'page-block-end') && /vertical-rl|sideways-rl/.test(String(cs.writingMode))) phys = cmd === 'page-block-start' ? 'page-right' : 'page-left';
                else if (vert && (cmd === 'page-block-start' || cmd === 'page-block-end')) phys = cmd === 'page-block-start' ? 'page-left' : 'page-right';
                if (phys === 'page-up') dy = -ph; else if (phys === 'page-down') dy = ph; else if (phys === 'page-left') dx = -pw; else if (phys === 'page-right') dx = pw;
                try { target.scrollBy({ left: dx, top: dy, behavior: 'instant' }); } catch (x) { target.scrollLeft += dx; target.scrollTop += dy; }
            }
            /* (the built-in commands of details, number inputs and media) */
            else if (target.localName === 'details' && (cmd === 'toggle' || cmd === 'open' || cmd === 'close')) {
                if (cmd === 'toggle') target.open = !target.open; else target.open = cmd === 'open';
            } else if (target.localName === 'input' && /^(step-up|step-down)$/.test(cmd) && inputType(target) === 'number') {
                try { if (cmd === 'step-up') target.stepUp(); else target.stepDown(); } catch (x) { }
            } else if ((target.localName === 'video' || target.localName === 'audio') && /^(play-pause|play|pause|toggle-muted)$/.test(cmd)) {
                try {
                    if (cmd === 'toggle-muted') target.muted = !target.muted;
                    else if (cmd === 'pause' || (cmd === 'play-pause' && !target.paused)) target.pause();
                    else { var pr = target.play(); if (pr && pr.catch) pr.catch(function () { }); }
                } catch (x) { }
            }
        }, false);
    })();
    /* ---- innerText / outerText getters (HTML "rendered text collection steps"), from computed styles ---- */
    (function () {
        var HE = global.HTMLElement && global.HTMLElement.prototype;
        if (!HE) return;
        var BLOCKISH = /^(block|flow-root|flex|grid|list-item|table|table-caption|ruby-text|grid-lanes|-webkit-box|block flow|block flow-root|block flex|block grid|block table|block list-item|flow list-item)$/;
        var SKIP = { template: 1, head: 1, title: 1, meta: 1, link: 1, base: 1 };
        var REPLACED = { canvas: 1, img: 1, video: 1, audio: 1, iframe: 1, embed: 1, object: 1, input: 1, textarea: 1, math: 1, meter: 1, progress: 1 };
        var EMPTY_ROOT = { canvas: 1, img: 1, video: 1, audio: 1, iframe: 1, textarea: 1, input: 1 };
        function csOf(el) { try { return global.getComputedStyle(el); } catch (e) { return null; } }
        function transform(s, tt) {
            if (!tt || tt === 'none') return s;
            if (tt === 'uppercase') return s.toUpperCase();
            if (tt === 'lowercase') return s.toLowerCase();
            if (tt === 'capitalize') return s.replace(/(^|[\s])(\S)/g, function (m, a, b) { return a + b.toUpperCase(); });
            return s;
        }
        function collect(root) {
            var out = [];   /* strings, {cs:1} collapsible spaces, {req:n}, {ls:1} line starts */
            var lineStart = true;
            function popSpaces() { while (out.length && out[out.length - 1].cs) out.pop(); }
            function req(n) { popSpaces(); out.push({ req: n }); lineStart = true; }
            function nl(s) { popSpaces(); out.push(s); lineStart = true; }
            function text(s, ws, tt) {
                s = s.replace(/\r\n?/g, '\n');
                var collapse = ws !== 'pre' && ws !== 'pre-wrap' && ws !== 'break-spaces';
                var keepNl = !collapse || ws === 'pre-line';
                if (!collapse) {
                    var parts = s.split('\n');
                    for (var i = 0; i < parts.length; i++) {
                        if (i) nl('\n');
                        if (parts[i]) { out.push(transform(parts[i], tt)); lineStart = false; }
                    }
                    return;
                }
                var lines = keepNl ? s.split('\n') : [s];
                for (var li = 0; li < lines.length; li++) {
                    if (li) nl('\n');
                    var re = /[ \t\n\f\r]+|[^ \t\n\f\r]+/g, m;
                    while ((m = re.exec(lines[li]))) {
                        var w = m[0];
                        if (/^[ \t\n\f\r]/.test(w)) {
                            if (lineStart) continue;
                            var last = out[out.length - 1];
                            if (last && last.cs) continue;
                            out.push({ cs: 1 });
                        } else { out.push(transform(w, tt)); lineStart = false; }
                    }
                }
            }
            function walkOne(el, ws, tt, vis) {
                var cs1 = csOf(el);
                if (!cs1 || cs1.display === 'none') return;
                var b1 = BLOCKISH.test(cs1.display);
                if (b1) req(1);
                walk(el, cs1.whiteSpace || ws, cs1.textTransform, cs1.visibility === 'visible');
                if (b1) req(1);
            }
            function walk(node, ws, tt, vis) {
                for (var c = node.firstChild; c; c = c.nextSibling) {
                    if (c.nodeType === 3 || c.nodeType === 4) { if (vis) text(c.data, ws, tt); continue; }
                    if (c.nodeType !== 1) continue;
                    var tag = c.localName;
                    if (SKIP[tag] && c.namespaceURI === 'http://www.w3.org/1999/xhtml') continue;
                    var cs = csOf(c);
                    if (!cs) continue;
                    var disp = cs.display;
                    if (disp === 'none') continue;
                    var cvis = cs.visibility === 'visible';
                    var cws = cs.whiteSpace || (cs.whiteSpaceCollapse === 'preserve' ? 'pre' : 'normal');
                    var ctt = cs.textTransform;
                    if (disp === 'contents') { walk(c, cws, ctt, cvis); continue; }
                    if (tag === 'br') { if (cvis) nl('\n'); continue; }
                    var isP = tag === 'p';
                    /* (an option and an optgroup each a line; a select's options its text) */
                    var optLine = tag === 'option' || tag === 'optgroup';
                    var blockish = BLOCKISH.test(disp) || optLine;
                    var replaced = REPLACED[tag] && c.namespaceURI === 'http://www.w3.org/1999/xhtml';
                    var atomic = !replaced && tag !== 'select' && (/^inline-(block|flex|grid|table)$/.test(disp) || /^inline (flow-root|flex|grid|table)$/.test(disp));
                    if (isP) req(2); else if (blockish) req(1);
                    /* (atomic inlines and replaced content: what is around them stays, a space either side) */
                    if (atomic) { out.push(''); lineStart = true; }
                    if (replaced) { out.push(''); lineStart = false; }
                    else if (tag === 'select') { walk(c, cws, ctt, cvis); }
                    else if (tag === 'details' && !c.hasAttribute('open')) {
                        for (var sm = c.firstElementChild; sm; sm = sm.nextElementSibling) if (sm.localName === 'summary') { walkOne(sm, cws, ctt, cvis); break; }
                    }
                    else walk(c, cws, ctt, cvis);
                    if (atomic) { popSpaces(); out.push(''); lineStart = false; }
                    if (disp === 'table-cell') {
                        var nextCell = c.nextElementSibling;
                        while (nextCell && csOf(nextCell) && csOf(nextCell).display !== 'table-cell') nextCell = nextCell.nextElementSibling;
                        if (nextCell) { popSpaces(); out.push('\t'); lineStart = true; }
                    } else if (disp === 'table-row') {
                        var nextRow = c.nextElementSibling, p = c.parentElement;
                        if (!nextRow && p) { var ps = p.nextElementSibling; while (ps && !nextRow) { nextRow = ps.querySelector ? ps.querySelector('tr') : null; ps = ps.nextElementSibling; } }
                        if (nextRow) nl('\n');
                    }
                    if (isP) req(2); else if (blockish) req(1);
                }
            }
            var rcs = csOf(root);
            walk(root, rcs ? (rcs.whiteSpace || 'normal') : 'normal', rcs ? rcs.textTransform : 'none', !rcs || rcs.visibility === 'visible');
            /* (required line breaks: none at the ends, the largest of a run) */
            var s = '', pending = 0, started = false;
            popSpaces();
            for (var i = 0; i < out.length; i++) {
                var x = out[i];
                if (typeof x === 'object' && x.req) { if (started) pending = Math.max(pending, x.req); continue; }
                var str = typeof x === 'string' ? x : ' ';
                if (!started && x.cs) continue;
                if (pending) { s += '\n\n'.slice(0, pending); pending = 0; }
                s += str; started = true;
            }
            return s;
        }
        function rendered(el) {
            if (!el.isConnected) return el.textContent;
            if (EMPTY_ROOT[el.localName] && el.namespaceURI === 'http://www.w3.org/1999/xhtml') {
                var rcs0 = csOf(el);
                if (rcs0 && rcs0.display !== 'none') return '';
            }
            for (var a = el; a && a.nodeType === 1; a = a.parentElement || (a.parentNode && a.parentNode.host) || null) {
                var cs = csOf(a);
                if (!cs || cs.display === 'none') return el.textContent;
            }
            return collect(el);
        }
        /* (the rendered text fragment: text nodes, a <br> for each line break) */
        function fragment(el, v) {
            var doc = el.ownerDocument, f = doc.createDocumentFragment(), s = v == null ? '' : String(v);
            var parts = s.split(/\r\n|\r|\n/);
            for (var i = 0; i < parts.length; i++) {
                if (i) f.appendChild(doc.createElement('br'));
                if (parts[i]) f.appendChild(doc.createTextNode(parts[i]));
            }
            return f;
        }
        /* (HTMLElement's alone: not an SVG or MathML element's) */
        var EPi = global.Element && global.Element.prototype;
        if (EPi && EPi !== HE) { delete EPi.innerText; delete EPi.outerText; }
        Object.defineProperty(HE, 'innerText', { configurable: true, enumerable: true,
            get: function innerText() { return rendered(this); },
            set: function innerText(v) { var f = fragment(this, v); while (this.firstChild) this.removeChild(this.firstChild); this.appendChild(f); } });
        Object.defineProperty(HE, 'outerText', { configurable: true, enumerable: true,
            get: function outerText() { return rendered(this); },
            set: function outerText(v) {
                var p = this.parentNode;
                if (!p) throw new global.DOMException("Failed to set the 'outerText' property on 'HTMLElement': The element has no parent.", 'NoModificationAllowedError');
                var next = this.nextSibling, prev = this.previousSibling, f = fragment(this, v);
                if (!f.firstChild) f.appendChild(this.ownerDocument.createTextNode(''));
                p.replaceChild(f, this);
                if (next && next.nodeType === 3 && next.previousSibling && next.previousSibling.nodeType === 3) {
                    var t = next.previousSibling; t.data += next.data; p.removeChild(next);
                }
                if (prev && prev.nodeType === 3 && prev.nextSibling && prev.nextSibling.nodeType === 3) {
                    var n2 = prev.nextSibling; prev.data += n2.data; p.removeChild(n2);
                }
            } });
    })();

    /* new Option(text, value, defaultSelected, selected) (HTML 4.10.10) */
    (function () {
        var OP = proto('HTMLOptionElement');
        if (!OP || global.Option) return;
        var Option = function Option(text, value, defaultSelected, selected) {
            if (!new.target) throw new TypeError("Failed to construct 'Option': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
            var o = global.document.createElement('option');
            text = text === undefined ? '' : String(text);
            if (text !== '') o.appendChild(global.document.createTextNode(text));
            if (value !== undefined) o.setAttribute('value', String(value));
            if (defaultSelected) o.setAttribute('selected', '');
            if (!!selected !== !!defaultSelected) o.selected = !!selected;
            if (new.target !== Option && new.target.prototype) Object.setPrototypeOf(o, new.target.prototype);
            return o;
        };
        Object.defineProperty(Option, 'prototype', { value: OP, writable: false });
        Object.defineProperty(global, 'Option', { configurable: true, writable: true, value: Option });
    })();


    /* ---- the form controls' live collections (HTML 2.6.2-2.6.3): HTMLOptionsCollection, HTMLFormControlsCollection, RadioNodeList ---- */
    (function () {
        var HC = global.HTMLCollection, NL = global.NodeList;
        if (!HC || typeof Proxy !== 'function') return;
        function iface(name, Parent) {
            var C = function () { throw new TypeError('Illegal constructor'); };
            Object.defineProperty(C, 'name', { value: name });
            C.prototype = Object.create(Parent.prototype);
            Object.defineProperty(C.prototype, 'constructor', { value: C, writable: true, configurable: true });
            Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true });
            Object.setPrototypeOf(C, Parent);
            Object.defineProperty(global, name, { value: C, writable: true, configurable: true });
            return C;
        }
        var OWNER = Symbol('collectionOwner'), ITEMS = Symbol('collectionItems');
        function isIndex(k) { return typeof k === 'string' && /^(0|[1-9]\d*)$/.test(k) && +k < 4294967295; }
        function nameOf(e) { return e.id || (e.getAttribute && e.namespaceURI === 'http://www.w3.org/1999/xhtml' ? e.getAttribute('name') : '') || ''; }
        /* (a live collection: indices and names read through to items() each time) */
        function live(P, owner, items, named) {
            var t = Object.create(P);
            Object.defineProperty(t, OWNER, { value: owner });
            Object.defineProperty(t, ITEMS, { value: items });
            function names() {
                var out = [], seen = {};
                items().forEach(function (e) {
                    [e.id, e.getAttribute && e.getAttribute('name')].forEach(function (n) {
                        if (n && !seen[n]) { seen[n] = 1; out.push(n); }
                    });
                });
                return out;
            }
            return new Proxy(t, {
                get: function (o, k, r) {
                    if (isIndex(k)) return items()[+k];
                    if (typeof k === 'string' && !(k in o) && k) { var v = named(k); if (v != null) return v; }
                    return Reflect.get(o, k, r);
                },
                set: function (o, k, v, r) {
                    if (isIndex(k) && P.__indexSet) { P.__indexSet.call(r, +k, v); return true; }
                    if (isIndex(k)) return true;
                    return Reflect.set(o, k, v, r);
                },
                has: function (o, k) {
                    if (isIndex(k)) return +k < items().length;
                    if (typeof k === 'string' && k && !(k in o) && named(k) != null) return true;
                    return k in o;
                },
                ownKeys: function (o) {
                    var n = items().length, keys = [];
                    for (var i = 0; i < n; i++) keys.push(String(i));
                    names().forEach(function (x) { if (!(x in o)) keys.push(x); });
                    return keys.concat(Reflect.ownKeys(o));
                },
                getOwnPropertyDescriptor: function (o, k) {
                    if (isIndex(k)) { var e = items()[+k]; return e ? { value: e, writable: false, enumerable: true, configurable: true } : undefined; }
                    if (typeof k === 'string' && k && !(k in o)) { var v = named(k); if (v != null) return { value: v, writable: false, enumerable: false, configurable: true }; }
                    return Reflect.getOwnPropertyDescriptor(o, k);
                },
                defineProperty: function (o, k, d) {
                    if (isIndex(k)) return false;
                    return Reflect.defineProperty(o, k, d);
                },
                deleteProperty: function (o, k) {
                    if (isIndex(k)) return !(+k < items().length);
                    return Reflect.deleteProperty(o, k);
                },
            });
        }
        function ownerOf(c, name) {
            var o = c && c[OWNER];
            if (!o) throw new TypeError("Failed to execute '" + name + "': Illegal invocation");
            return o;
        }
        function acc(P, name, get, set) {
            var d = { configurable: true, enumerable: true };
            var g = { get: function () { return get.call(this, ownerOf(this, name)); } };
            d.get = Object.getOwnPropertyDescriptor(g, 'get').value;
            if (set) { var s = { set: function (v) { set.call(this, ownerOf(this, name), v); } }; d.set = Object.getOwnPropertyDescriptor(s, 'set').value; }
            Object.defineProperty(P, name, d);
        }
        function op(P, name, len, fn) {
            var o = { f: function () {
                var own = ownerOf(this, name);
                if (arguments.length < len) throw new TypeError("Failed to execute '" + name + "': " + len + ' argument' + (len > 1 ? 's' : '') + ' required, but only ' + arguments.length + ' present.');
                return fn.apply(this, [own].concat([].slice.call(arguments)));
            } };
            Object.defineProperty(o.f, 'name', { value: name, configurable: true });
            Object.defineProperty(o.f, 'length', { value: len, configurable: true });
            Object.defineProperty(P, name, { value: o.f, writable: true, enumerable: true, configurable: true });
        }

        /* RadioNodeList: a NodeList of the form's controls of one name, with a value */
        var RNL = NL ? iface('RadioNodeList', NL) : null;
        function radioNodeList(form, name) {
            var l = elementsOf(form).filter(function (e) { return e.localName !== 'input' || inputType(e) !== 'image'; })
                .filter(function (e) { return e.id === name || e.getAttribute('name') === name; });
            Object.setPrototypeOf(l, RNL.prototype);
            return l;
        }
        if (RNL) Object.defineProperty(RNL.prototype, 'value', { configurable: true, enumerable: true,
            get: function value() {
                for (var i = 0; i < this.length; i++) {
                    var e = this[i];
                    if (e.localName === 'input' && inputType(e) === 'radio' && e.checked) { var v = e.getAttribute('value'); return v === null ? 'on' : v; }
                }
                return '';
            },
            set: function value(v) {
                v = String(v);
                for (var i = 0; i < this.length; i++) {
                    var e = this[i];
                    if (e.localName !== 'input' || inputType(e) !== 'radio') continue;
                    var ev = e.getAttribute('value');
                    if ((ev === null ? 'on' : ev) === v) { e.checked = true; return; }
                }
            } });

        /* HTMLFormControlsCollection: form.elements */
        var HFCC = iface('HTMLFormControlsCollection', HC), FCP = HFCC.prototype;
        function controls(form) { return elementsOf(form).filter(function (e) { return e.localName !== 'input' || inputType(e) !== 'image'; }); }
        function formNamed(form, name) {
            if (!name) return null;
            var hits = controls(form).filter(function (e) { return e.id === name || e.getAttribute('name') === name; });
            if (!hits.length) return null;
            return hits.length === 1 ? hits[0] : RNL ? radioNodeList(form, name) : hits[0];
        }
        acc(FCP, 'length', function (form) { return controls(form).length; });
        op(FCP, 'item', 1, function (form, i) { return controls(form)[i >>> 0] || null; });
        op(FCP, 'namedItem', 1, function (form, n) { return formNamed(form, String(n)); });
        var formLists = new WeakMap();
        if (FP) Object.defineProperty(FP, 'elements', { configurable: true, enumerable: true, get: function elements() {
            var c = formLists.get(this), self = this;
            if (!c) { c = live(FCP, this, function () { return controls(self); }, function (n) { return formNamed(self, n); }); formLists.set(this, c); }
            return c;
        } });

        /* HTMLOptionsCollection: select.options */
        var HOC = iface('HTMLOptionsCollection', HC), OCP = HOC.prototype;
        function optNamed(sel, name) {
            if (!name) return null;
            var l = optionsOf(sel);
            for (var i = 0; i < l.length; i++) if (l[i].id === name || l[i].getAttribute('name') === name) return l[i];
            return null;
        }
        acc(OCP, 'length', function (sel) { return optionsOf(sel).length; }, function (sel, n) {
            n = Number(n); n = isFinite(n) ? Math.trunc(n) >>> 0 : 0;
            var l = optionsOf(sel);
            if (n > 100000) return;
            if (n > l.length) {
                var d = sel.ownerDocument, f = d.createDocumentFragment();
                for (var i = l.length; i < n; i++) f.appendChild(d.createElement('option'));
                sel.appendChild(f);
            } else for (var j = l.length - 1; j >= n; j--) l[j].remove();
        });
        acc(OCP, 'selectedIndex', function (sel) { return sel.selectedIndex; }, function (sel, v) { sel.selectedIndex = v; });
        op(OCP, 'item', 1, function (sel, i) { return optionsOf(sel)[i >>> 0] || null; });
        op(OCP, 'namedItem', 1, function (sel, n) { return optNamed(sel, String(n)); });
        op(OCP, 'add', 1, function (sel, el, before) { return SP.add.call(sel, el, before); });
        op(OCP, 'remove', 1, function (sel, i) { var o = optionsOf(sel)[Number(i) | 0]; if (o) o.remove(); });
        /* (options[i] = option: put there, or the list grown to it; = null, the one there removed) */
        Object.defineProperty(OCP, '__indexSet', { value: function (i, v) {
            var sel = this[OWNER], l = optionsOf(sel);
            if (v === null || v === undefined) { if (l[i]) l[i].remove(); return; }
            if (!(v instanceof global.HTMLOptionElement)) throw new TypeError("Failed to set an indexed property on 'HTMLOptionsCollection': parameter 2 is not of type 'HTMLOptionElement'.");
            if (i > l.length) { var d = sel.ownerDocument; for (var k = l.length; k < i; k++) sel.appendChild(d.createElement('option')); l = optionsOf(sel); }
            if (i < l.length) l[i].parentNode.replaceChild(v, l[i]);
            else sel.appendChild(v);
        } });
        var selLists = new WeakMap();
        if (SP) Object.defineProperty(SP, 'options', { configurable: true, enumerable: true, get: function options() {
            var c = selLists.get(this), self = this;
            if (!c) { c = live(OCP, this, function () { return optionsOf(self); }, function (n) { return optNamed(self, n); }); selLists.set(this, c); }
            return c;
        } });
        /* (selectedOptions: a live HTMLCollection, the same one each time) */
        var selSel = new WeakMap(), SOP = Object.create(HC.prototype);
        Object.defineProperty(SOP, 'length', { configurable: true, get: function () { return this[ITEMS]().length; } });
        Object.defineProperty(SOP, 'item', { configurable: true, writable: true, value: function item(i) { return this[ITEMS]()[i >>> 0] || null; } });
        Object.defineProperty(SOP, 'namedItem', { configurable: true, writable: true, value: function namedItem(n) { var l = this[ITEMS](); n = String(n); for (var i = 0; i < l.length; i++) if (n && (l[i].id === n || l[i].getAttribute('name') === n)) return l[i]; return null; } });
        Object.defineProperty(SOP, Symbol.iterator, { configurable: true, writable: true, value: function () { return this[ITEMS]()[Symbol.iterator](); } });
        if (SP) Object.defineProperty(SP, 'selectedOptions', { configurable: true, enumerable: true, get: function selectedOptions() {
            if (!(this instanceof global.HTMLSelectElement)) throw new TypeError('Illegal invocation');
            var c = selSel.get(this), self = this;
            if (!c) {
                var items = function () { return optionsOf(self).filter(function (o) { return o.selected; }); };
                c = live(SOP, this, items, function (n) {
                    var l = items();
                    for (var i = 0; i < l.length; i++) if (n && (l[i].id === n || l[i].getAttribute('name') === n)) return l[i];
                    return null;
                });
                selSel.set(this, c);
            }
            return c;
        } });
        /* (select.namedItem: its options' by id or name) */
        if (SP && typeof SP.namedItem !== 'function')
            Object.defineProperty(SP, 'namedItem', { configurable: true, enumerable: true, writable: true, value: function namedItem(name) {
                if (!(this instanceof global.HTMLSelectElement)) throw new TypeError('Illegal invocation');
                if (arguments.length < 1) throw new TypeError("Failed to execute 'namedItem' on 'HTMLSelectElement': 1 argument required, but only 0 present.");
                return optNamed(this, String(name));
            } });
    })();

    /* FileList (File API): an input's chosen files */
    (function () {
        if (typeof global.FileList === 'function') return;
        var FB = Symbol('fileList');
        var FileList = function FileList() { throw new TypeError('Illegal constructor'); };
        var FP_ = FileList.prototype;
        Object.defineProperty(FP_, 'length', { configurable: true, enumerable: true, get: function length() { var l = this[FB]; if (!l) throw new TypeError('Illegal invocation'); return l.length; } });
        Object.defineProperty(FP_, 'item', { configurable: true, enumerable: true, writable: true, value: function item(i) {
            var l = this[FB]; if (!l) throw new TypeError('Illegal invocation');
            if (arguments.length < 1) throw new TypeError("Failed to execute 'item' on 'FileList': 1 argument required, but only 0 present.");
            i = Number(i); i = isFinite(i) ? Math.trunc(i) >>> 0 : 0;
            return i < l.length ? l[i] : null;
        } });
        Object.defineProperty(FP_, Symbol.iterator, { configurable: true, writable: true, value: function () { return this[FB].slice()[Symbol.iterator](); } });
        Object.defineProperty(FP_, Symbol.toStringTag, { configurable: true, value: 'FileList' });
        Object.defineProperty(global, 'FileList', { configurable: true, writable: true, value: FileList });
        var make = function (files) {
            var o = Object.create(FP_);
            Object.defineProperty(o, FB, { value: files.slice() });
            files.forEach(function (f, i) { Object.defineProperty(o, i, { value: f, enumerable: true }); });
            return o;
        };
        Object.defineProperty(global, '__cyberFileList', { configurable: true, value: make });
        /* DataTransfer (HTML 6.11.3): what a drag carries, made by script: its items, their strings and files */
        if (typeof global.DataTransfer !== 'function') {
            var DT = Symbol('dataTransfer');
            var dtOf = function (x) { var s = x && x[DT]; if (!s) throw new TypeError('Illegal invocation'); return s; };
            var DataTransferItem = function DataTransferItem() { throw new TypeError('Illegal constructor'); };
            var DTIP = DataTransferItem.prototype;
            /* (an item: the same object while it is in the list; removed, in the disabled mode) */
            var newItem = function (e) { if (e.obj) return e.obj; var o = Object.create(DTIP); Object.defineProperty(o, DT, { value: e }); e.obj = o; return o; };
            var disable = function (list) { list.forEach(function (e) { e.disabled = true; }); };
            Object.defineProperty(DTIP, 'kind', { configurable: true, enumerable: true, get: function () { var e = dtOf(this); return e.disabled ? '' : e.file ? 'file' : 'string'; } });
            Object.defineProperty(DTIP, 'type', { configurable: true, enumerable: true, get: function () { var e = dtOf(this); return e.disabled ? '' : e.file ? e.file.type : e.type; } });
            DTIP.getAsFile = function getAsFile() { var e = dtOf(this); return e.disabled ? null : e.file || null; };
            DTIP.getAsString = function getAsString(cb) { var e = dtOf(this); if (!e.disabled && !e.file && typeof cb === 'function') global.setTimeout(function () { cb(e.data); }, 0); };
            Object.defineProperty(DTIP, Symbol.toStringTag, { configurable: true, value: 'DataTransferItem' });
            var DataTransferItemList = function DataTransferItemList() { throw new TypeError('Illegal constructor'); };
            var DTLP = DataTransferItemList.prototype;
            var newList = function (st) {
                var o = Object.create(DTLP);
                Object.defineProperty(o, DT, { value: st });
                return new Proxy(o, {
                    get: function (t, k, r) { if (typeof k === 'string' && /^\d+$/.test(k)) { var e = st.items[+k]; return e ? newItem(e) : undefined; } return Reflect.get(t, k, t); },
                    has: function (t, k) { if (typeof k === 'string' && /^\d+$/.test(k)) return +k < st.items.length; return k in t; },
                    getOwnPropertyDescriptor: function (t, k) {
                        if (typeof k === 'string' && /^\d+$/.test(k)) { var e = st.items[+k]; return e ? { value: newItem(e), writable: false, enumerable: true, configurable: true } : undefined; }
                        return Reflect.getOwnPropertyDescriptor(t, k);
                    },
                    ownKeys: function (t) { var ks = st.items.map(function (_, i) { return String(i); }); return ks.concat(Reflect.ownKeys(t)); }
                });
            };
            Object.defineProperty(DTLP, 'length', { configurable: true, enumerable: true, get: function () { return dtOf(this).items.length; } });
            DTLP.add = function add(data, type) {
                var st = dtOf(this);
                if (st.mode && st.mode !== 'rw') return null;
                if (global.File && data instanceof global.File) { st.items.push({ file: data }); st.filesObj = null; return newItem(st.items[st.items.length - 1]); }
                if (arguments.length < 2) throw new TypeError("Failed to execute 'add' on 'DataTransferItemList': parameter 2 is required.");
                type = String(type).toLowerCase();
                for (var i = 0; i < st.items.length; i++) if (!st.items[i].file && st.items[i].type === type)
                    throw new global.DOMException("Failed to execute 'add' on 'DataTransferItemList': An item already exists for type '" + type + "'.", 'NotSupportedError');
                st.items.push({ type: type, data: String(data) });
                return newItem(st.items[st.items.length - 1]);
            };
            DTLP.remove = function remove(i) { var st = dtOf(this); if (st.mode && st.mode !== 'rw') { if (st.mode === 'protected' || st.mode === 'ro') throw new global.DOMException('The data store is not in read/write mode.', 'InvalidStateError'); } i = Number(i) >>> 0; if (i < st.items.length) { disable(st.items.splice(i, 1)); st.filesObj = null; } };
            DTLP.clear = function clear() { var st = dtOf(this); if (st.mode && st.mode !== 'rw') return; disable(st.items); st.items.length = 0; st.filesObj = null; };
            Object.defineProperty(DTLP, Symbol.toStringTag, { configurable: true, value: 'DataTransferItemList' });
            var DataTransfer = function DataTransfer() {
                if (!new.target) throw new TypeError("Failed to construct 'DataTransfer': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
                var st = { items: [], dropEffect: 'none', effectAllowed: 'none', filesObj: null };
                Object.defineProperty(this, DT, { value: st });
                st.list = newList(st);
            };
            var DTP = DataTransfer.prototype;
            var norm = function (f) { f = String(f).toLowerCase(); return f === 'text' ? 'text/plain' : f === 'url' ? 'text/uri-list' : f; };
            Object.defineProperty(DTP, 'dropEffect', { configurable: true, enumerable: true, get: function () { return dtOf(this).dropEffect; },
                set: function (v) { v = String(v); if (/^(none|copy|link|move)$/.test(v)) dtOf(this).dropEffect = v; } });
            Object.defineProperty(DTP, 'effectAllowed', { configurable: true, enumerable: true, get: function () { return dtOf(this).effectAllowed; },
                set: function (v) { var st = dtOf(this); if (st.mode && st.mode !== 'rw') return; v = String(v); if (/^(none|copy|copyLink|copyMove|link|linkMove|move|all|uninitialized)$/.test(v)) st.effectAllowed = v; } });
            Object.defineProperty(DTP, 'items', { configurable: true, enumerable: true, get: function () { return dtOf(this).list; } });
            Object.defineProperty(DTP, 'types', { configurable: true, enumerable: true, get: function () {
                var st = dtOf(this), t = [], files = false;
                st.items.forEach(function (e) { if (e.file) files = true; else t.push(e.type); });
                if (files) t.push('Files');
                /* (the same frozen array while the list stays as it is) */
                var key = t.join('\u0000');
                if (st.typesObj && st.typesKey === key) return st.typesObj;
                st.typesKey = key;
                return (st.typesObj = Object.freeze(t));
            } });
            Object.defineProperty(DTP, 'files', { configurable: true, enumerable: true, get: function () {
                var st = dtOf(this);
                if (!st.filesObj) st.filesObj = make(st.items.filter(function (e) { return e.file; }).map(function (e) { return e.file; }));
                return st.filesObj;
            } });
            DTP.getData = function getData(format) {
                var st = dtOf(this), f = norm(format);
                if (st.mode === 'protected' || st.mode === 'gone') return '';
                for (var i = 0; i < st.items.length; i++) if (!st.items[i].file && st.items[i].type === f) return st.items[i].data;
                return '';
            };
            DTP.setData = function setData(format, data) {
                var st = dtOf(this), f = norm(format);
                if (st.mode && st.mode !== 'rw') return;
                disable(st.items.filter(function (e) { return !e.file && e.type === f; }));
                st.items = st.items.filter(function (e) { return e.file || e.type !== f; });
                st.items.push({ type: f, data: String(data) });
            };
            DTP.clearData = function clearData(format) {
                var st = dtOf(this);
                if (st.mode && st.mode !== 'rw') return;
                if (format === undefined) st.items = st.items.filter(function (e) { return e.file; });
                else { var f = norm(format); st.items = st.items.filter(function (e) { return e.file || e.type !== f; }); }
            };
            DTP.setDragImage = function setDragImage() { };
            Object.defineProperty(DTP, Symbol.toStringTag, { configurable: true, value: 'DataTransfer' });
            /* (a drag's: its data store, its mode the drag sets, dom_events.js) */
            Object.defineProperty(global, '__cyberDragData', { configurable: true, value: function () {
                var dt = new DataTransfer(), st = dt[DT];
                st.effectAllowed = 'uninitialized';
                st.mode = 'rw';
                return [dt, st];
            } });
            [['DataTransfer', DataTransfer], ['DataTransferItem', DataTransferItem], ['DataTransferItemList', DataTransferItemList]].forEach(function (p) {
                Object.defineProperty(global, p[0], { configurable: true, writable: true, value: p[1] });
            });
        }
        /* (a file input's: its list, the same object until it changes) */
        if (IP) {
            var lists = new WeakMap();
            Object.defineProperty(IP, 'files', { configurable: true, enumerable: true,
                get: function files() {
                    if (inputType(this) !== 'file') return null;
                    var l = lists.get(this);
                    if (!l) { l = make([]); lists.set(this, l); }
                    return l;
                },
                set: function files(v) {
                    if (inputType(this) !== 'file' || v === null || v === undefined) return;
                    if (!(v instanceof FileList)) throw new TypeError("Failed to set the 'files' property on 'HTMLInputElement': The provided value is not of type 'FileList'.");
                    lists.set(this, v);
                } });
        }
    })();

    /* (a picker input's click() and showPicker(): the picker opens, which takes the user's activation) */
    (function () {
        if (!IP) return;
        var PICK = /^(color|date|datetime-local|file|month|time|week)$/;
        var hc = global.HTMLElement && Object.getOwnPropertyDescriptor(global.HTMLElement.prototype, 'click');
        if (hc && typeof hc.value === 'function') Object.defineProperty(IP, 'click', { configurable: true, writable: true, enumerable: true, value: function click() {
            var r = hc.value.apply(this, arguments);
            if (PICK.test(inputType(this)) && global.__cyberTransientActivation && global.__cyberTransientActivation() && global.__cyberConsumeActivation) global.__cyberConsumeActivation();
            return r;
        } });
        var sp = Object.getOwnPropertyDescriptor(IP, 'showPicker');
        if (sp && typeof sp.value === 'function') Object.defineProperty(IP, 'showPicker', { configurable: true, writable: true, enumerable: true, value: function showPicker() {
            var r = sp.value.apply(this, arguments);
            if (global.__cyberConsumeActivation) global.__cyberConsumeActivation();
            return r;
        } });
    })();

    /* (user activation: its listeners, now that events are in) */
    /* autocomplete (HTML 4.10.18.7.1): a control's autofill detail tokens, serialized; a form's on or off */
    (function () {
        var NORMAL = 'name honorific-prefix given-name additional-name family-name honorific-suffix nickname username new-password current-password one-time-code organization-title organization street-address address-line1 address-line2 address-line3 address-level4 address-level3 address-level2 address-level1 country country-name postal-code cc-name cc-given-name cc-additional-name cc-family-name cc-number cc-exp cc-exp-month cc-exp-year cc-csc cc-type transaction-currency transaction-amount language bday bday-day bday-month bday-year sex url photo'.split(' ');
        var CONTACT = 'tel tel-country-code tel-national tel-area-code tel-local tel-local-prefix tel-local-suffix tel-extension email impp'.split(' ');
        var detail = function (el) {
            var v = el.getAttribute('autocomplete');
            if (v === null) return '';
            var t = String(v).toLowerCase().split(/[\t\n\f\r ]+/).filter(Boolean);
            if (!t.length) return '';
            var hidden = el.localName === 'input' && String(el.getAttribute('type') || '').toLowerCase() === 'hidden';
            var cred = false;
            if (t.length > 1 && t[t.length - 1] === 'webauthn') { cred = true; t.pop(); }
            else if (t.length === 1 && t[0] === 'webauthn') return 'webauthn';
            var field = t[t.length - 1];
            if (field === 'on' || field === 'off') return t.length === 1 && !cred && !hidden ? field : '';
            var contactField = CONTACT.indexOf(field) >= 0;
            if (!contactField && NORMAL.indexOf(field) < 0) return '';
            var max = (contactField ? 4 : 3) + (cred ? 1 : 0);
            if (t.length + (cred ? 1 : 0) > max) return '';
            var out = [field], i = t.length - 2;
            if (contactField && i >= 0 && /^(home|work|mobile|fax|pager)$/.test(t[i])) out.unshift(t[i--]);
            if (i >= 0 && /^(shipping|billing)$/.test(t[i])) out.unshift(t[i--]);
            if (i >= 0 && /^section-/.test(t[i])) out.unshift(t[i--]);
            if (i >= 0) return '';
            if (cred) out.push('webauthn');
            return out.join(' ');
        };
        ['HTMLInputElement', 'HTMLSelectElement', 'HTMLTextAreaElement'].forEach(function (n) {
            var P = global[n] && global[n].prototype;
            if (!P) return;
            Object.defineProperty(P, 'autocomplete', { configurable: true, enumerable: true,
                get: function autocomplete() { return detail(this); },
                set: function autocomplete(v) { this.setAttribute('autocomplete', String(v)); } });
        });
        var FP0 = global.HTMLFormElement && global.HTMLFormElement.prototype;
        if (FP0) Object.defineProperty(FP0, 'autocomplete', { configurable: true, enumerable: true,
            get: function autocomplete() { var v = this.getAttribute('autocomplete'); return v !== null && String(v).toLowerCase() === 'off' ? 'off' : 'on'; },
            set: function autocomplete(v) { this.setAttribute('autocomplete', String(v)); } });
    })();
    if (typeof global.__cyberActivationListen === 'function') global.__cyberActivationListen();
    if (typeof global.__cyberAfterEvents === 'function') global.__cyberAfterEvents();
})

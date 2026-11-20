/*
 * canvas_api.js - <canvas>, OffscreenCanvas, ImageBitmap and the 2D context
 * (HTML 4.12.5), installed in each realm by platform_api_install
 * (browser/platform_api.c).
 *
 * The context keeps the whole drawing state as the standard defines it
 * (the state stack, the current transform, styles with colors serialized
 * as browsers do, line and text settings, the current path in device
 * pixels).  Drawing goes to the bitmaps of browser/canvas2d.c: each
 * operation hands over its path (already transformed), its paint and the
 * state it needs (__c2dDraw, __c2dText, ...).
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
    function num(v, d) { v = Number(v); return isFinite(v) ? v : d; }
    function domErr(msg, name) { var E = global.DOMException; return E ? new E(msg, name) : new Error(msg); }
    var N = {
        reset: global.__c2dReset, draw: global.__c2dDraw, save: global.__c2dSave, restore: global.__c2dRestore,
        get: global.__c2dGet, put: global.__c2dPut, copy: global.__c2dCopy, size: global.__c2dSize, png: global.__c2dPng,
        measure: global.__c2dMeasure, text: global.__c2dText, hit: global.__c2dHit, newId: global.__c2dNew,
        free: global.__c2dFree, beginLayer: global.__c2dBeginLayer, endLayer: global.__c2dEndLayer
    };
    var HAVE = typeof N.draw === 'function';

    /* ---- colors (CSS Color 4 parsing; HTML's serialization of a canvas
     * color: "#rrggbb" when opaque, else "rgba(r, g, b, a)") ---- */
    var NAMED = {
        aliceblue: 'f0f8ff', antiquewhite: 'faebd7', aqua: '00ffff', aquamarine: '7fffd4', azure: 'f0ffff', beige: 'f5f5dc',
        bisque: 'ffe4c4', black: '000000', blanchedalmond: 'ffebcd', blue: '0000ff', blueviolet: '8a2be2', brown: 'a52a2a',
        burlywood: 'deb887', cadetblue: '5f9ea0', chartreuse: '7fff00', chocolate: 'd2691e', coral: 'ff7f50',
        cornflowerblue: '6495ed', cornsilk: 'fff8dc', crimson: 'dc143c', cyan: '00ffff', darkblue: '00008b', darkcyan: '008b8b',
        darkgoldenrod: 'b8860b', darkgray: 'a9a9a9', darkgreen: '006400', darkgrey: 'a9a9a9', darkkhaki: 'bdb76b',
        darkmagenta: '8b008b', darkolivegreen: '556b2f', darkorange: 'ff8c00', darkorchid: '9932cc', darkred: '8b0000',
        darksalmon: 'e9967a', darkseagreen: '8fbc8f', darkslateblue: '483d8b', darkslategray: '2f4f4f', darkslategrey: '2f4f4f',
        darkturquoise: '00ced1', darkviolet: '9400d3', deeppink: 'ff1493', deepskyblue: '00bfff', dimgray: '696969',
        dimgrey: '696969', dodgerblue: '1e90ff', firebrick: 'b22222', floralwhite: 'fffaf0', forestgreen: '228b22',
        fuchsia: 'ff00ff', gainsboro: 'dcdcdc', ghostwhite: 'f8f8ff', gold: 'ffd700', goldenrod: 'daa520', gray: '808080',
        green: '008000', greenyellow: 'adff2f', grey: '808080', honeydew: 'f0fff0', hotpink: 'ff69b4', indianred: 'cd5c5c',
        indigo: '4b0082', ivory: 'fffff0', khaki: 'f0e68c', lavender: 'e6e6fa', lavenderblush: 'fff0f5', lawngreen: '7cfc00',
        lemonchiffon: 'fffacd', lightblue: 'add8e6', lightcoral: 'f08080', lightcyan: 'e0ffff', lightgoldenrodyellow: 'fafad2',
        lightgray: 'd3d3d3', lightgreen: '90ee90', lightgrey: 'd3d3d3', lightpink: 'ffb6c1', lightsalmon: 'ffa07a',
        lightseagreen: '20b2aa', lightskyblue: '87cefa', lightslategray: '778899', lightslategrey: '778899',
        lightsteelblue: 'b0c4de', lightyellow: 'ffffe0', lime: '00ff00', limegreen: '32cd32', linen: 'faf0e6', magenta: 'ff00ff',
        maroon: '800000', mediumaquamarine: '66cdaa', mediumblue: '0000cd', mediumorchid: 'ba55d3', mediumpurple: '9370db',
        mediumseagreen: '3cb371', mediumslateblue: '7b68ee', mediumspringgreen: '00fa9a', mediumturquoise: '48d1cc',
        mediumvioletred: 'c71585', midnightblue: '191970', mintcream: 'f5fffa', mistyrose: 'ffe4e1', moccasin: 'ffe4b5',
        navajowhite: 'ffdead', navy: '000080', oldlace: 'fdf5e6', olive: '808000', olivedrab: '6b8e23', orange: 'ffa500',
        orangered: 'ff4500', orchid: 'da70d6', palegoldenrod: 'eee8aa', palegreen: '98fb98', paleturquoise: 'afeeee',
        palevioletred: 'db7093', papayawhip: 'ffefd5', peachpuff: 'ffdab9', peru: 'cd853f', pink: 'ffc0cb', plum: 'dda0dd',
        powderblue: 'b0e0e6', purple: '800080', rebeccapurple: '663399', red: 'ff0000', rosybrown: 'bc8f8f',
        royalblue: '4169e1', saddlebrown: '8b4513', salmon: 'fa8072', sandybrown: 'f4a460', seagreen: '2e8b57',
        seashell: 'fff5ee', sienna: 'a0522d', silver: 'c0c0c0', skyblue: '87ceeb', slateblue: '6a5acd', slategray: '708090',
        slategrey: '708090', snow: 'fffafa', springgreen: '00ff7f', steelblue: '4682b4', tan: 'd2b48c', teal: '008080',
        thistle: 'd8bfd8', tomato: 'ff6347', turquoise: '40e0d0', violet: 'ee82ee', wheat: 'f5deb3', white: 'ffffff',
        whitesmoke: 'f5f5f5', yellow: 'ffff00', yellowgreen: '9acd32', transparent: null
    };
    function hex2(n) { n = Math.max(0, Math.min(255, Math.round(n))); return (n < 16 ? '0' : '') + n.toString(16); }
    function channel(s) {
        s = s.trim();
        if (s === 'none') return 0;
        if (s.slice(-1) === '%') return num(s.slice(0, -1), NaN) * 2.55;
        return num(s, NaN);
    }
    function alpha(s) {
        if (s === undefined) return 1;
        s = s.trim();
        if (s === 'none') return 0;
        var a = s.slice(-1) === '%' ? num(s.slice(0, -1), NaN) / 100 : num(s, NaN);
        return Math.max(0, Math.min(1, a));
    }
    function hsl(h, s, l) {
        h = ((h % 360) + 360) % 360 / 360; s /= 100; l /= 100;
        function f(n) { var k = (n + h * 12) % 12, a = s * Math.min(l, 1 - l); return l - a * Math.max(-1, Math.min(k - 3, 9 - k, 1)); }
        return [f(0) * 255, f(8) * 255, f(4) * 255];
    }
    function hue(s) {
        s = s.trim();
        var m = /^(-?[\d.]+(?:e[+-]?\d+)?)(deg|rad|grad|turn)?$/.exec(s);
        if (!m) return NaN;
        var v = Number(m[1]);
        return m[2] === 'rad' ? v * 180 / Math.PI : m[2] === 'grad' ? v * 0.9 : m[2] === 'turn' ? v * 360 : v;
    }
    function serialize(r, g, b, a) {
        if (a >= 1) return '#' + hex2(r) + hex2(g) + hex2(b);
        var as = String(Math.round(a * 1000) / 1000);
        if (Math.abs(Number(as) - a) > 1e-9) { var a2 = Math.round(a * 255) / 255; as = String(Number(a2.toFixed(3))); }
        return 'rgba(' + Math.round(r) + ', ' + Math.round(g) + ', ' + Math.round(b) + ', ' + as + ')';
    }
    var g_current = null;   /* (the element currentcolor resolves against) */
    /* The serialized color, or null when value is not a color. */
    /* (CSS Color 4's rgb() and hsl() syntaxes: legacy all commas, one kind of channel, no none; modern spaces, then / alpha) */
    var NUMTOK = '[+-]?(?:\\d+(?:\\.\\d+)?|\\.\\d+)(?:e[+-]?\\d+)?';
    var RE_NUM = new RegExp('^' + NUMTOK + '$'), RE_PCT = new RegExp('^' + NUMTOK + '%$'), RE_HUE = new RegExp('^' + NUMTOK + '(?:deg|rad|grad|turn)?$');
    function colorArgsOk(fn, args) {
        args = args.trim();
        var hslf = fn.charAt(0) === 'h';
        if (args.indexOf(',') >= 0) {
            if (args.indexOf('/') >= 0) return false;
            var p = args.split(/\s*,\s*/);
            if (p.length !== 3 && p.length !== 4) return false;
            if (p.some(function (t) { return !t || /\s/.test(t) || t === 'none'; })) return false;
            if (hslf) {
                if (!RE_HUE.test(p[0]) || !RE_PCT.test(p[1]) || !RE_PCT.test(p[2])) return false;
            } else {
                var allP = p.slice(0, 3).every(function (t) { return RE_PCT.test(t); }), allN = p.slice(0, 3).every(function (t) { return RE_NUM.test(t); });
                if (!allP && !allN) return false;
            }
            if (p.length === 4 && !RE_NUM.test(p[3]) && !RE_PCT.test(p[3])) return false;
            return true;
        }
        var parts = args.split(/\s*\/\s*/);
        if (parts.length > 2) return false;
        var c = parts[0].split(/\s+/).filter(Boolean);
        if (c.length !== 3) return false;
        var okc = function (t, i) {
            if (t === 'none') return true;
            if (hslf) return i === 0 ? RE_HUE.test(t) : RE_PCT.test(t) || RE_NUM.test(t);
            return RE_NUM.test(t) || RE_PCT.test(t);
        };
        if (!c.every(okc)) return false;
        if (parts.length === 2) { var a = parts[1].trim(); if (!a || /\s/.test(a) || !(a === 'none' || RE_NUM.test(a) || RE_PCT.test(a))) return false; }
        return true;
    }
    function parseColor(value) {
        var s = String(value).trim().toLowerCase(), m;
        if ((m = /^(rgba?|hsla?)\((.*)\)$/.exec(s)) && m[2].indexOf('(') < 0 && !colorArgsOk(m[1], m[2])) return null;
        if (s in NAMED) return NAMED[s] === null ? 'rgba(0, 0, 0, 0)' : '#' + NAMED[s];
        if ((m = /^#([0-9a-f]{3,8})$/.exec(s))) {
            var h = m[1];
            if (h.length === 3 || h.length === 4) h = h.replace(/./g, '$&$&');
            if (h.length !== 6 && h.length !== 8) return null;
            return serialize(parseInt(h.slice(0, 2), 16), parseInt(h.slice(2, 4), 16), parseInt(h.slice(4, 6), 16),
                             h.length === 8 ? parseInt(h.slice(6, 8), 16) / 255 : 1);
        }
        if ((m = /^rgba?\((.*)\)$/.exec(s))) {
            var p = m[1].split(/\s*[,\/]\s*|\s+/).filter(Boolean);
            if (p.length < 3 || p.length > 4) return null;
            var r = channel(p[0]), g = channel(p[1]), b = channel(p[2]), a = alpha(p[3]);
            if (![r, g, b, a].every(isFinite)) return null;
            return serialize(r, g, b, a);
        }
        if ((m = /^hsla?\((.*)\)$/.exec(s))) {
            var q = m[1].split(/\s*[,\/]\s*|\s+/).filter(Boolean);
            if (q.length < 3 || q.length > 4) return null;
            var hh = hue(q[0]), ss = num(q[1].replace('%', ''), NaN), ll = num(q[2].replace('%', ''), NaN);
            if (!isFinite(hh + ss + ll)) return null;
            var c = hsl(hh, Math.max(0, ss), Math.max(0, ll));
            return serialize(c[0], c[1], c[2], alpha(q[3]));
        }
        /* (color(display-p3 r g b [/ a]): kept as it is, for a display-p3 bitmap; sRGB from it for the rest) */
        if ((m = /^color\(\s*display-p3\s+([^)]*)\)$/.exec(s))) {
            var pp = m[1].split(/\s*\/\s*|\s+/).filter(Boolean);
            if (pp.length < 3 || pp.length > 4) return null;
            var pc = pp.slice(0, 3).map(function (t) { return t === 'none' ? 0 : t.slice(-1) === '%' ? Number(t.slice(0, -1)) / 100 : Number(t); });
            if (!pc.every(isFinite)) return null;
            var pa = alpha(pp[3]);
            if (!isFinite(pa)) return null;
            return 'color(display-p3 ' + pc.join(' ') + (pa < 1 ? ' / ' + pa : '') + ')';
        }
        if ((m = /^color\(\s*(srgb|srgb-linear)\s+([^)]*)\)$/.exec(s))) {
            /* (color(srgb r g b [/ a]): channels 0..1 or percentages; srgb-linear encoded to srgb) */
            var cp = m[2].split(/\s*\/\s*|\s+/).filter(Boolean);
            if (cp.length < 3 || cp.length > 4) return null;
            var ch3 = cp.slice(0, 3).map(function (t) { return t === 'none' ? 0 : t.slice(-1) === '%' ? Number(t.slice(0, -1)) / 100 : Number(t); });
            if (!ch3.every(isFinite)) return null;
            if (m[1] === 'srgb-linear') ch3 = ch3.map(function (v) { var a = Math.abs(v), e = a <= 0.0031308 ? 12.92 * a : 1.055 * Math.pow(a, 1 / 2.4) - 0.055; return v < 0 ? -e : e; });
            return serialize(ch3[0] * 255, ch3[1] * 255, ch3[2] * 255, alpha(cp[3]));
        }
        if (s === 'currentcolor') {
            var el = g_current, cs = null;
            try { if (el && el.nodeType === 1 && el.isConnected) cs = global.getComputedStyle(el).color; } catch (e) { cs = null; }
            if (cs && cs !== 'currentcolor') { var v = parseColor(cs); if (v) return v; }
            return '#000000';
        }
        /* (other CSS colors: as the style system computes them) */
        if (/^[a-z-]+\(/.test(s) || /^[a-z]+$/.test(s)) {
            try {
                var doc = global.document, probe = doc && doc.documentElement;
                if (probe && global.getComputedStyle) {
                    var d = doc.createElement('span');
                    d.style.color = 'rgb(1, 2, 3)';
                    d.style.color = String(value);
                    if (d.style.color && d.style.color !== 'rgb(1, 2, 3)') {
                        probe.appendChild(d);
                        var got = global.getComputedStyle(d).color;
                        d.remove();
                        if (got && /^rgba?\(/.test(got)) return parseColor(got);
                    }
                }
            } catch (e) { }
        }
        return null;
    }
    function rgbaOf(c) {
        if (typeof c !== 'string') return null;
        var pm3 = /^color\(display-p3 ([-\d.e]+) ([-\d.e]+) ([-\d.e]+)(?: \/ ([\d.e-]+))?\)$/.exec(c);
        if (pm3) {
            var p3px = new global.Uint8ClampedArray([Math.round(Math.min(1, Math.max(0, +pm3[1])) * 255), Math.round(Math.min(1, Math.max(0, +pm3[2])) * 255), Math.round(Math.min(1, Math.max(0, +pm3[3])) * 255), 255]);
            convertPixels(p3px, 'display-p3', 'srgb');
            return [p3px[0], p3px[1], p3px[2], pm3[4] === undefined ? 1 : +pm3[4]];
        }
        var m = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(c);
        if (m) return [parseInt(m[1], 16), parseInt(m[2], 16), parseInt(m[3], 16), 1];
        m = /^rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*(?:,\s*([\d.e-]+)\s*)?\)$/i.exec(c);
        if (m) return [+m[1], +m[2], +m[3], m[4] === undefined ? 1 : +m[4]];
        return null;
    }

    /* (for others: a color's serialization, as canvas reads it) */
    if (!global.__cyberParseColor) Object.defineProperty(global, '__cyberParseColor', { configurable: true, value: parseColor });

    /* ---- gradients, patterns, ImageData ---- */
    function CanvasGradient() { illegal(); }
    define(CanvasGradient.prototype, 'addColorStop', function addColorStop(offset, color) {
        if (!(this instanceof CanvasGradient) || !this._stops) throw new TypeError('Illegal invocation');
        if (arguments.length < 2) throw new TypeError("Failed to execute 'addColorStop': 2 arguments required.");
        offset = Number(offset);
        if (!isFinite(offset)) throw new TypeError("The provided double value is non-finite.");
        if (!(offset >= 0 && offset <= 1))
            throw domErr('The provided value is outside [0, 1].', 'IndexSizeError');
        var c = parseColor(color);
        if (c === null) throw domErr("The value provided ('" + color + "') could not be parsed as a color.", 'SyntaxError');
        this._stops.push([offset, c]);
    });
    function gradient(kind, args) {
        var g = Object.create(CanvasGradient.prototype);
        define(g, '_kind', kind); define(g, '_args', args); define(g, '_stops', []);
        define(g, '_cim', 'srgb'); define(g, '_him', 'shorter');
        return g;
    }
    var CIM = ['srgb', 'srgb-linear', 'display-p3', 'display-p3-linear', 'a98-rgb', 'prophoto-rgb', 'rec2020', 'lab', 'oklab', 'xyz', 'xyz-d50', 'xyz-d65', 'hsl', 'hwb', 'lch', 'oklch'];
    var HIM = ['shorter', 'longer', 'increasing', 'decreasing'];
    Object.defineProperty(CanvasGradient.prototype, 'colorInterpolationMethod', { configurable: true, enumerable: true,
        get: function () { return this._cim; }, set: function (v) { v = String(v); if (CIM.indexOf(v) >= 0) define(this, '_cim', v); } });
    Object.defineProperty(CanvasGradient.prototype, 'hueInterpolationMethod', { configurable: true, enumerable: true,
        get: function () { return this._him; }, set: function (v) { v = String(v); if (HIM.indexOf(v) >= 0) define(this, '_him', v); } });
    /* (a linear gradient in another color space: the CSS gradient it is, over the bitmap, as an image) */
    function cssGradientPaint(c, g) {
        if (g._kind !== 'linear' || (g._cim === 'srgb' && g._him === 'shorter') || typeof global.__c2dGrad !== 'function' || !g._stops.length) return null;
        var dm = dims(c), W = dm[0], H = dm[1];
        if (!W || !H) return null;
        var m = c._m, a = g._args;
        var x0 = m[0] * a[0] + m[2] * a[1] + m[4], y0 = m[1] * a[0] + m[3] * a[1] + m[5];
        var x1 = m[0] * a[2] + m[2] * a[3] + m[4], y1 = m[1] * a[2] + m[3] * a[3] + m[5];
        var dx = x1 - x0, dy = y1 - y0, len = Math.sqrt(dx * dx + dy * dy);
        if (!(len > 0)) return null;
        var ux = dx / len, uy = dy / len, L = Math.abs(W * ux) + Math.abs(H * uy);
        var k = (W / 2 - x0) * ux + (H / 2 - y0) * uy;
        var st = g._stops.map(function (p, i) { return [p[0], p[1], i]; }).sort(function (p, q) { return p[0] - q[0] || p[2] - q[2]; });
        var parts = st.map(function (p) { return p[1] + ' ' + (len * p[0] - k + L / 2).toFixed(3) + 'px'; });
        var polar = /^(hsl|hwb|lch|oklch)$/.test(g._cim);
        var css = 'linear-gradient(' + (Math.atan2(dx, -dy) * 180 / Math.PI).toFixed(4) + 'deg in ' + g._cim + (polar ? ' ' + g._him + ' hue' : '') + ', ' + parts.join(', ') + ')';
        var key = global.__c2dGrad(css, W, H);
        if (!key) return null;
        return [new Float64Array([4, 1, 0, 0, 1, 0, 0, 4, 0, 0, W, H, 1]), key];
    }
    function CanvasPattern() { illegal(); }
    define(CanvasPattern.prototype, 'setTransform', function setTransform(mt) {
        if (!(this instanceof CanvasPattern) || !this._mt) throw new TypeError('Illegal invocation');
        var a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
        if (mt && typeof mt === 'object') {
            check2d(mt);
            a = num(mt.a !== undefined ? mt.a : mt.m11, 1); b = num(mt.b !== undefined ? mt.b : mt.m12, 0);
            c = num(mt.c !== undefined ? mt.c : mt.m21, 0); d = num(mt.d !== undefined ? mt.d : mt.m22, 1);
            e = num(mt.e !== undefined ? mt.e : mt.m41, 0); f = num(mt.f !== undefined ? mt.f : mt.m42, 0);
        }
        this._mt = [a, b, c, d, e, f];
    });

    var IMGDATA = Symbol('imagedata');
    function ImageData(a, b, c, d) {
        if (!new.target) throw new TypeError("Failed to construct 'ImageData': Please use the 'new' operator");
        var data, w, h, settings;
        if (a instanceof global.Uint8ClampedArray) {
            data = a; w = b >>> 0;
            if (!data.length || data.length % 4) throw domErr('The input data length is not a multiple of 4.', 'InvalidStateError');
            if (!w) throw domErr('The source width is zero.', 'IndexSizeError');
            if ((data.length / 4) % w) throw domErr('The input data length is not a multiple of (4 * width).', 'IndexSizeError');
            h = c === undefined ? data.length / 4 / w : c >>> 0;
            if (data.length !== w * h * 4) throw domErr('The input data length is not equal to (4 * width * height).', 'IndexSizeError');
            settings = d;
        } else {
            w = Number(a); h = Number(b);
            if (arguments.length < 2) throw new TypeError("Failed to construct 'ImageData': 2 arguments required.");
            w = w >>> 0; h = h >>> 0;
            if (!w || !h) throw domErr('The source width/height is zero.', 'IndexSizeError');
            data = new global.Uint8ClampedArray(w * h * 4);
            settings = c;
        }
        var cs = colorSpaceOf(settings, 'srgb');
        Object.defineProperty(this, IDS, { value: { w: w, h: h, data: data, cs: cs } });
    }
    /* (its attributes: the prototype's getters over what it holds) */
    var IDS = Symbol('imageData');
    ['width', 'height', 'data', 'colorSpace', 'pixelFormat'].forEach(function (k) {
        var g = { get: function () {
            var s = this != null ? this[IDS] : undefined;
            if (!s) throw new TypeError('Illegal invocation');
            return k === 'width' ? s.w : k === 'height' ? s.h : k === 'data' ? s.data : k === 'colorSpace' ? s.cs : 'rgba-unorm8';
        } }.get;
        Object.defineProperty(g, 'name', { value: 'get ' + k });
        Object.defineProperty(ImageData.prototype, k, { get: g, enumerable: true, configurable: true });
    });
    /* ---- predefined color spaces: what an ImageData's bytes are in; the canvas keeps sRGB ---- */
    var CSPACES = { 'srgb': 1, 'display-p3': 1, 'srgb-linear': 1, 'display-p3-linear': 1 };
    function colorSpaceOf(settings, dflt) {
        var v = settings == null ? undefined : settings.colorSpace;
        if (v === undefined) return dflt;
        v = String(v);
        if (!CSPACES[v]) throw new TypeError("Failed to read the 'colorSpace' property: The provided value '" + v + "' is not a valid enum value of type PredefinedColorSpace.");
        return v;
    }
    function srgbDecode(c) { return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4); }
    function srgbEncode(c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * Math.pow(c, 1 / 2.4) - 0.055; }
    var M_S2P = [0.8224621, 0.1775380, 0, 0.0331941, 0.9668058, 0, 0.0170827, 0.0723974, 0.9105199];
    var M_P2S = [1.2249401, -0.2249404, 0, -0.0420569, 1.0420571, 0, -0.0196376, -0.0786361, 1.0982735];
    var DECODE_LUT = null;
    /* (bytes in one space to another, in place: through linear light) */
    function convertPixels(d, from, to) {
        if (from === to || !d || !d.length) return d;
        if (!DECODE_LUT) { DECODE_LUT = new Float64Array(256); for (var k = 0; k < 256; k++) DECODE_LUT[k] = srgbDecode(k / 255); }
        var fromLin = from === 'srgb-linear' || from === 'display-p3-linear', toLin = to === 'srgb-linear' || to === 'display-p3-linear';
        var fromP3 = from.indexOf('display-p3') === 0, toP3 = to.indexOf('display-p3') === 0;
        var M = fromP3 === toP3 ? null : fromP3 ? M_P2S : M_S2P;
        for (var i = 0; i < d.length; i += 4) {
            var r = fromLin ? d[i] / 255 : DECODE_LUT[d[i]], g = fromLin ? d[i + 1] / 255 : DECODE_LUT[d[i + 1]], b = fromLin ? d[i + 2] / 255 : DECODE_LUT[d[i + 2]];
            if (M) { var r2 = M[0] * r + M[1] * g + M[2] * b, g2 = M[3] * r + M[4] * g + M[5] * b, b2 = M[6] * r + M[7] * g + M[8] * b; r = r2; g = g2; b = b2; }
            r = Math.min(1, Math.max(0, r)); g = Math.min(1, Math.max(0, g)); b = Math.min(1, Math.max(0, b));
            if (!toLin) { r = srgbEncode(r); g = srgbEncode(g); b = srgbEncode(b); }
            d[i] = Math.round(r * 255); d[i + 1] = Math.round(g * 255); d[i + 2] = Math.round(b * 255);
        }
        return d;
    }
    function ctxColorSpace(c) { return (c && c._attrs && c._attrs.colorSpace) || 'srgb'; }
    /* (a display-p3 context's bitmap holds Display P3 values: what goes in is converted to it, what comes out from it) */
    /* (a float16 one keeps sRGB values: 8 bits of P3 would lose what came in) */
    function storeSpace(c) { return ctxColorSpace(c) === 'display-p3' && !(c && c._attrs && c._attrs.colorType === 'float16') ? 'display-p3' : 'srgb'; }
    function p3Key(c, src) {
        if (typeof src !== 'string' || storeSpace(c) !== 'display-p3' || /^(p3|cyber|cbfilter):/.test(src)) return src;
        return 'p3:' + src;
    }
    /* (an [r, g, b, a] in sRGB bytes, as a display-p3 bitmap holds it; color(display-p3 ...) its own values) */
    function p3Color(c, col, str) {
        if (storeSpace(c) !== 'display-p3' || !col) return col;
        var m = typeof str === 'string' ? /^color\(\s*display-p3\s+([^)]*)\)$/i.exec(str.trim()) : null;
        if (m) {
            var cp = m[1].split(/\s*\/\s*|\s+/).filter(Boolean);
            var ch = cp.slice(0, 3).map(function (t) { return t === 'none' ? 0 : t.slice(-1) === '%' ? Number(t.slice(0, -1)) / 100 : Number(t); });
            if (ch.every(isFinite)) return [Math.round(Math.min(1, Math.max(0, ch[0])) * 255), Math.round(Math.min(1, Math.max(0, ch[1])) * 255), Math.round(Math.min(1, Math.max(0, ch[2])) * 255), col[3]];
        }
        var px = new global.Uint8ClampedArray([col[0], col[1], col[2], 255]);
        convertPixels(px, 'srgb', 'display-p3');
        return [px[0], px[1], px[2], col[3]];
    }
    function makeImageData(w, h, buf) {
        var d = buf ? new global.Uint8ClampedArray(buf) : new global.Uint8ClampedArray(w * h * 4);
        return new ImageData(d, w, h);
    }

    /* ---- paths: points in device pixels for a context (through its transform
     * when added), in its own space for a Path2D.  ops: 0 M x y, 1 L x y,
     * 2 C x1 y1 x2 y2 x y, 3 Z ---- */
    function PathData() { this.d = []; this.has = false; this.cx = 0; this.cy = 0; this.sx = 0; this.sy = 0; this.open = false; }
    var PATH = Symbol('path');
    function pathOf(o) { return o[PATH]; }
    function mapPt(o, x, y) {
        var m = o._m;
        return m ? [m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]] : [x, y];
    }
    function unmapPt(o, x, y) {
        var m = o._m;
        if (!m) return [x, y];
        var det = m[0] * m[3] - m[1] * m[2];
        if (!det) return [x, y];
        var dx = x - m[4], dy = y - m[5];
        return [(m[3] * dx - m[2] * dy) / det, (-m[1] * dx + m[0] * dy) / det];
    }
    function pMove(o, x, y) {
        var p = pathOf(o), q = mapPt(o, x, y);
        p.d.push(0, q[0], q[1]); p.has = true; p.cx = p.sx = q[0]; p.cy = p.sy = q[1]; p.open = true;
    }
    function pLine(o, x, y) {
        var p = pathOf(o);
        if (!p.has) { pMove(o, x, y); return; }
        var q = mapPt(o, x, y);
        p.d.push(1, q[0], q[1]); p.cx = q[0]; p.cy = q[1];
    }
    function pCubic(o, a, b, c, d, x, y) {
        var p = pathOf(o);
        if (!p.has) pMove(o, a, b);
        var q1 = mapPt(o, a, b), q2 = mapPt(o, c, d), q3 = mapPt(o, x, y);
        p.d.push(2, q1[0], q1[1], q2[0], q2[1], q3[0], q3[1]); p.cx = q3[0]; p.cy = q3[1];
    }
    function finite() { for (var i = 0; i < arguments.length; i++) if (!isFinite(arguments[i])) return false; return true; }
    /* an elliptical arc from angle s through sweep (radians), as cubic pieces */
    function pArc(o, x, y, rx, ry, rot, s, sweep, join) {
        var cr = Math.cos(rot), sr = Math.sin(rot);
        function pt(t) { var ex = rx * Math.cos(t), ey = ry * Math.sin(t); return [x + ex * cr - ey * sr, y + ex * sr + ey * cr]; }
        var p0 = pt(s);
        if (join) pLine(o, p0[0], p0[1]); else pMove(o, p0[0], p0[1]);
        if (!sweep) return;
        var n = Math.max(1, Math.ceil(Math.abs(sweep) / (Math.PI / 2) - 1e-9)), step = sweep / n, k = 4 / 3 * Math.tan(step / 4);
        for (var i = 0; i < n; i++) {
            var a0 = s + step * i, a1 = a0 + step;
            var c0 = Math.cos(a0), s0 = Math.sin(a0), c1 = Math.cos(a1), s1 = Math.sin(a1);
            var e1x = rx * (c0 - k * s0), e1y = ry * (s0 + k * c0), e2x = rx * (c1 + k * s1), e2y = ry * (s1 - k * c1);
            var p3 = pt(a1);
            pCubic(o, x + e1x * cr - e1y * sr, y + e1x * sr + e1y * cr, x + e2x * cr - e2y * sr, y + e2x * sr + e2y * cr, p3[0], p3[1]);
        }
    }
    function sweepOf(s, e, ccw) {
        var tau = 2 * Math.PI;
        if (!ccw && e - s >= tau) return tau;
        if (ccw && s - e >= tau) return -tau;
        var d = ((e - s) % tau + tau) % tau;
        if (!ccw) return d;
        return d === 0 ? 0 : d - tau;
    }
    function negErr() { return domErr('The radius provided is negative.', 'IndexSizeError'); }
    var PATH_METHODS = {
        moveTo: function moveTo(x, y) { x = +x; y = +y; if (finite(x, y)) pMove(this, x, y); },
        lineTo: function lineTo(x, y) { x = +x; y = +y; if (finite(x, y)) pLine(this, x, y); },
        closePath: function closePath() {
            var p = pathOf(this);
            if (!p.has) return;
            p.d.push(3); p.cx = p.sx; p.cy = p.sy;
            p.d.push(0, p.sx, p.sy);
        },
        bezierCurveTo: function bezierCurveTo(a, b, c, d, x, y) {
            a = +a; b = +b; c = +c; d = +d; x = +x; y = +y;
            if (finite(a, b, c, d, x, y)) pCubic(this, a, b, c, d, x, y);
        },
        quadraticCurveTo: function quadraticCurveTo(a, b, x, y) {
            a = +a; b = +b; x = +x; y = +y;
            if (!finite(a, b, x, y)) return;
            var p = pathOf(this);
            if (!p.has) pMove(this, a, b);
            var c0 = unmapPt(this, p.cx, p.cy);
            pCubic(this, c0[0] + 2 / 3 * (a - c0[0]), c0[1] + 2 / 3 * (b - c0[1]), x + 2 / 3 * (a - x), y + 2 / 3 * (b - y), x, y);
        },
        arc: function arc(x, y, r, s, e, ccw) {
            x = +x; y = +y; r = +r; s = +s; e = +e;
            if (!finite(x, y, r, s, e)) return;
            if (r < 0) throw negErr();
            pArc(this, x, y, r, r, 0, s, sweepOf(s, e, !!ccw), pathOf(this).has);
        },
        ellipse: function ellipse(x, y, rx, ry, rot, s, e, ccw) {
            x = +x; y = +y; rx = +rx; ry = +ry; rot = +rot; s = +s; e = +e;
            if (!finite(x, y, rx, ry, rot, s, e)) return;
            if (rx < 0 || ry < 0) throw negErr();
            pArc(this, x, y, rx, ry, rot, s, sweepOf(s, e, !!ccw), pathOf(this).has);
        },
        arcTo: function arcTo(x1, y1, x2, y2, r) {
            x1 = +x1; y1 = +y1; x2 = +x2; y2 = +y2; r = +r;
            if (!finite(x1, y1, x2, y2, r)) return;
            if (r < 0) throw negErr();
            var p = pathOf(this);
            if (!p.has) pMove(this, x1, y1);
            var c0 = unmapPt(this, p.cx, p.cy), x0 = c0[0], y0 = c0[1];
            if ((x0 === x1 && y0 === y1) || (x1 === x2 && y1 === y2) || r === 0) { pLine(this, x1, y1); return; }
            var ux = x0 - x1, uy = y0 - y1, vx = x2 - x1, vy = y2 - y1;
            var lu = Math.hypot(ux, uy), lv = Math.hypot(vx, vy);
            var cross = ux * vy - uy * vx;
            if (Math.abs(cross) < 1e-12 * lu * lv) { pLine(this, x1, y1); return; }
            ux /= lu; uy /= lu; vx /= lv; vy /= lv;
            var cosT = ux * vx + uy * vy, theta = Math.acos(Math.max(-1, Math.min(1, cosT)));
            var dist = r / Math.tan(theta / 2);
            var t1x = x1 + ux * dist, t1y = y1 + uy * dist, t2x = x1 + vx * dist, t2y = y1 + vy * dist;
            var bx = ux + vx, by = uy + vy, bl = Math.hypot(bx, by);
            var cd = r / Math.sin(theta / 2), cx = x1 + bx / bl * cd, cy = y1 + by / bl * cd;
            var a0 = Math.atan2(t1y - cy, t1x - cx), a1 = Math.atan2(t2y - cy, t2x - cx);
            var ccw = cross > 0;
            var sw = a1 - a0;
            if (ccw) { while (sw > 0) sw -= 2 * Math.PI; } else { while (sw < 0) sw += 2 * Math.PI; }
            if (Math.abs(sw) > Math.PI) sw = ccw ? sw + 2 * Math.PI : sw - 2 * Math.PI;
            pLine(this, t1x, t1y);
            pArc(this, cx, cy, r, r, 0, a0, sw, true);
        },
        rect: function rect(x, y, w, h) {
            x = +x; y = +y; w = +w; h = +h;
            if (!finite(x, y, w, h)) return;
            pMove(this, x, y); pLine(this, x + w, y); pLine(this, x + w, y + h); pLine(this, x, y + h);
            PATH_METHODS.closePath.call(this);
        },
        roundRect: function roundRect(x, y, w, h, radii) {
            x = +x; y = +y; w = +w; h = +h;
            var list = radii === undefined ? [0] : (radii !== null && typeof radii === 'object' && typeof radii[Symbol.iterator] === 'function') ? Array.from(radii) : [radii];
            if (list.length < 1 || list.length > 4) throw new RangeError(list.length + ' radii provided. Between one and four radii are necessary.');
            var rs = [], bad = false;
            list.forEach(function (r) {
                if (r !== null && typeof r === 'object') {
                    var rx = Number(r.x === undefined ? 0 : r.x), ry = Number(r.y === undefined ? 0 : r.y);
                    if (!finite(rx, ry)) bad = true;
                    else if (rx < 0 || ry < 0) throw new RangeError('X and Y radii must be positive.');
                    rs.push([rx, ry]);
                } else {
                    var v = Number(r);
                    if (!isFinite(v)) bad = true;
                    else if (v < 0) throw new RangeError('Radius value must be positive.');
                    rs.push([v, v]);
                }
            });
            if (bad || !finite(x, y, w, h)) return;
            var ul, ur, lr, ll;
            if (rs.length === 4) { ul = rs[0]; ur = rs[1]; lr = rs[2]; ll = rs[3]; }
            else if (rs.length === 3) { ul = rs[0]; ur = ll = rs[1]; lr = rs[2]; }
            else if (rs.length === 2) { ul = lr = rs[0]; ur = ll = rs[1]; }
            else { ul = ur = lr = ll = rs[0]; }
            ul = ul.slice(); ur = ur.slice(); lr = lr.slice(); ll = ll.slice();
            /* (a negative width or height: the rectangle flipped, the corners with it) */
            var t;
            if (w < 0) { x += w; w = -w; t = ul; ul = ur; ur = t; t = ll; ll = lr; lr = t; }
            if (h < 0) { y += h; h = -h; t = ul; ul = ll; ll = t; t = ur; ur = lr; lr = t; }
            var top = ul[0] + ur[0], right = ur[1] + lr[1], bottom = lr[0] + ll[0], left = ul[1] + ll[1];
            var sc = Math.min(top > 0 ? w / top : Infinity, right > 0 ? h / right : Infinity, bottom > 0 ? w / bottom : Infinity, left > 0 ? h / left : Infinity);
            if (sc < 1) [ul, ur, lr, ll].forEach(function (c) { c[0] *= sc; c[1] *= sc; });
            pMove(this, x + ul[0], y);
            pLine(this, x + w - ur[0], y);
            if (ur[0] || ur[1]) pArc(this, x + w - ur[0], y + ur[1], ur[0], ur[1], 0, -Math.PI / 2, Math.PI / 2, true);
            pLine(this, x + w, y + h - lr[1]);
            if (lr[0] || lr[1]) pArc(this, x + w - lr[0], y + h - lr[1], lr[0], lr[1], 0, 0, Math.PI / 2, true);
            pLine(this, x + ll[0], y + h);
            if (ll[0] || ll[1]) pArc(this, x + ll[0], y + h - ll[1], ll[0], ll[1], 0, Math.PI / 2, Math.PI / 2, true);
            pLine(this, x, y + ul[1]);
            if (ul[0] || ul[1]) pArc(this, x + ul[0], y + ul[1], ul[0], ul[1], 0, Math.PI, Math.PI / 2, true);
            PATH_METHODS.closePath.call(this);
        }
    };
    /* (path d's points through matrix m) */
    function mapPath(d, m) {
        var o = new Array(d.length), i = 0;
        while (i < d.length) {
            var op = d[i]; o[i] = op; i++;
            var n = op === 2 ? 3 : op === 3 ? 0 : 1;
            for (var k = 0; k < n; k++, i += 2) {
                var x = d[i], y = d[i + 1];
                o[i] = m[0] * x + m[2] * y + m[4]; o[i + 1] = m[1] * x + m[3] * y + m[5];
            }
        }
        return o;
    }
    /* SVG path data (Path2D(string)) */
    function parseSvgPath(o, s) {
        var toks = String(s).match(/[MmLlHhVvCcSsQqTtAaZz]|[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?/g) || [];
        var i = 0, cmd = '', cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0, lq = null, last = '';
        function n() { return Number(toks[i++]); }
        function isNum() { return i < toks.length && /^[+-.\d]/.test(toks[i]); }
        while (i < toks.length) {
            if (!isNum()) cmd = toks[i++];
            else if (!cmd) return;
            var rel = cmd === cmd.toLowerCase(), C = cmd.toUpperCase(), ox = rel ? cx : 0, oy = rel ? cy : 0;
            if (C === 'Z') { PATH_METHODS.closePath.call(o); cx = sx; cy = sy; last = 'Z'; continue; }
            if (!isNum()) return;
            if (C === 'M') { cx = ox + n(); cy = oy + n(); sx = cx; sy = cy; pMove(o, cx, cy); cmd = rel ? 'l' : 'L'; }
            else if (C === 'L') { cx = ox + n(); cy = oy + n(); pLine(o, cx, cy); }
            else if (C === 'H') { cx = ox + n(); pLine(o, cx, cy); }
            else if (C === 'V') { cy = (rel ? cy : 0) + n(); pLine(o, cx, cy); }
            else if (C === 'C') { var a = ox + n(), b = oy + n(), c = ox + n(), d = oy + n(); cx = ox + n(); cy = oy + n(); pCubic(o, a, b, c, d, cx, cy); lcx = c; lcy = d; }
            else if (C === 'S') { var r1x = /[CS]/.test(last) ? 2 * cx - lcx : cx, r1y = /[CS]/.test(last) ? 2 * cy - lcy : cy, c2 = ox + n(), d2 = oy + n(); cx = ox + n(); cy = oy + n(); pCubic(o, r1x, r1y, c2, d2, cx, cy); lcx = c2; lcy = d2; }
            else if (C === 'Q') { var qa = ox + n(), qb = oy + n(); var nx = ox + n(), ny = oy + n(); pCubic(o, cx + 2 / 3 * (qa - cx), cy + 2 / 3 * (qb - cy), nx + 2 / 3 * (qa - nx), ny + 2 / 3 * (qb - ny), nx, ny); cx = nx; cy = ny; lq = [qa, qb]; }
            else if (C === 'T') { var tq = /[QT]/.test(last) && lq ? [2 * cx - lq[0], 2 * cy - lq[1]] : [cx, cy]; var tx = ox + n(), ty = oy + n(); pCubic(o, cx + 2 / 3 * (tq[0] - cx), cy + 2 / 3 * (tq[1] - cy), tx + 2 / 3 * (tq[0] - tx), ty + 2 / 3 * (tq[1] - ty), tx, ty); cx = tx; cy = ty; lq = tq; }
            else if (C === 'A') {
                var arx = Math.abs(n()), ary = Math.abs(n()), phi = n() * Math.PI / 180, fa = n(), fs = n(), ex = ox + n(), ey = oy + n();
                svgArc(o, cx, cy, arx, ary, phi, !!fa, !!fs, ex, ey); cx = ex; cy = ey;
            } else return;
            last = C;
        }
    }
    function svgArc(o, x1, y1, rx, ry, phi, fa, fs, x2, y2) {
        if (!rx || !ry) { pLine(o, x2, y2); return; }
        var c = Math.cos(phi), s = Math.sin(phi), dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
        var x1p = c * dx + s * dy, y1p = -s * dx + c * dy;
        var l = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
        if (l > 1) { rx *= Math.sqrt(l); ry *= Math.sqrt(l); }
        var num2 = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p, den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
        var k = Math.sqrt(Math.max(0, num2 / den)) * (fa === fs ? -1 : 1);
        var cxp = k * rx * y1p / ry, cyp = -k * ry * x1p / rx;
        var cx = c * cxp - s * cyp + (x1 + x2) / 2, cy = s * cxp + c * cyp + (y1 + y2) / 2;
        function ang(ux, uy, vx, vy) { var a = Math.atan2(ux * vy - uy * vx, ux * vx + uy * vy); return a; }
        var t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry), dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
        if (!fs && dt > 0) dt -= 2 * Math.PI; else if (fs && dt < 0) dt += 2 * Math.PI;
        pArc(o, cx, cy, rx, ry, phi, t1, dt, true);
    }
    function Path2D(src) {
        if (!new.target) throw new TypeError("Failed to construct 'Path2D': Please use the 'new' operator");
        define(this, PATH, new PathData());
        if (src instanceof Path2D) {
            var a = src[PATH], b = this[PATH];
            b.d = a.d.slice(); b.has = a.has; b.cx = a.cx; b.cy = a.cy; b.sx = a.sx; b.sy = a.sy;
        } else if (src !== undefined) {
            parseSvgPath(this, src);
        }
    }
    for (var k in PATH_METHODS) define(Path2D.prototype, k, PATH_METHODS[k]);
    define(Path2D.prototype, 'addPath', function addPath(p, mt) {
        if (!(p instanceof Path2D)) throw new TypeError("Failed to execute 'addPath' on 'Path2D': parameter 1 is not of type 'Path2D'.");
        var m = [1, 0, 0, 1, 0, 0];
        if (mt !== undefined && mt !== null) {
            if (typeof mt !== 'object') throw new TypeError("Failed to execute 'addPath': parameter 2 is not of type 'DOMMatrix2DInit'.");
            check2d(mt);
            var g = function (x, y, d) { return x !== undefined ? Number(x) : y !== undefined ? Number(y) : d; };
            m = [g(mt.a, mt.m11, 1), g(mt.b, mt.m12, 0), g(mt.c, mt.m21, 0), g(mt.d, mt.m22, 1), g(mt.e, mt.m41, 0), g(mt.f, mt.m42, 0)];
            if (!m.every(isFinite)) return;
        }
        var src = p[PATH], dst = this[PATH], d = mapPath(src.d, m);
        for (var i = 0; i < d.length; i++) dst.d.push(d[i]);
        if (src.has) {
            dst.has = true;
            dst.cx = m[0] * src.cx + m[2] * src.cy + m[4]; dst.cy = m[1] * src.cx + m[3] * src.cy + m[5];
            dst.sx = m[0] * src.sx + m[2] * src.sy + m[4]; dst.sy = m[1] * src.sx + m[3] * src.sy + m[5];
        }
    });

    /* ---- the context ---- */
    var DEFAULTS = {
        fillStyle: '#000000', strokeStyle: '#000000', globalAlpha: 1, globalCompositeOperation: 'source-over',
        lineWidth: 1, lineCap: 'butt', lineJoin: 'miter', miterLimit: 10, lineDashOffset: 0,
        shadowOffsetX: 0, shadowOffsetY: 0, shadowBlur: 0, shadowColor: 'rgba(0, 0, 0, 0)',
        font: '10px sans-serif', textAlign: 'start', textBaseline: 'alphabetic', direction: 'inherit',
        imageSmoothingEnabled: true, imageSmoothingQuality: 'low', filter: 'none',
        letterSpacing: '0px', wordSpacing: '0px', fontKerning: 'auto', fontStretch: 'normal',
        fontVariantCaps: 'normal', textRendering: 'auto', lang: 'inherit'
    };
    var ENUMS = {
        globalCompositeOperation: ['source-over', 'source-in', 'source-out', 'source-atop', 'destination-over',
            'destination-in', 'destination-out', 'destination-atop', 'lighter', 'copy', 'xor', 'multiply', 'screen',
            'overlay', 'darken', 'lighten', 'color-dodge', 'color-burn', 'hard-light', 'soft-light', 'difference',
            'exclusion', 'hue', 'saturation', 'color', 'luminosity'],
        lineCap: ['butt', 'round', 'square'], lineJoin: ['miter', 'round', 'bevel'],
        textAlign: ['start', 'end', 'left', 'right', 'center'],
        textBaseline: ['top', 'hanging', 'middle', 'alphabetic', 'ideographic', 'bottom'],
        direction: ['ltr', 'rtl', 'inherit'], imageSmoothingQuality: ['low', 'medium', 'high'],
        fontKerning: ['auto', 'normal', 'none'],
        fontStretch: ['ultra-condensed', 'extra-condensed', 'condensed', 'semi-condensed', 'normal', 'semi-expanded', 'expanded', 'extra-expanded', 'ultra-expanded'],
        fontVariantCaps: ['normal', 'small-caps', 'all-small-caps', 'petite-caps', 'all-petite-caps', 'unicase', 'titling-caps'],
        textRendering: ['auto', 'optimizeSpeed', 'optimizeLegibility', 'geometricPrecision']
    };
    function CanvasRenderingContext2D() { illegal(); }
    var CP = CanvasRenderingContext2D.prototype;
    function OffscreenCanvasRenderingContext2D() { illegal(); }
    var OCP = OffscreenCanvasRenderingContext2D.prototype;
    function newContext(canvas, proto, attrs) {
        var c = Object.create(proto || CP);
        define(c, '_canvas', canvas);
        define(c, '_attrs', attrs || { alpha: true, colorSpace: 'srgb', desynchronized: false, willReadFrequently: false });
        reset(c);
        return c;
    }
    function reset(c) {
        /* (a bitmap made anew is clean again) */
        if (c && c._canvas && typeof UNCLEAN !== 'undefined' && c._canvas[UNCLEAN]) define(c._canvas, UNCLEAN, false);
        define(c, '_s', Object.assign({ dash: [], fparsed: [] }, DEFAULTS));
        define(c, '_m', [1, 0, 0, 1, 0, 0]);
        define(c, '_stack', []);
        define(c, '_layers', 0);
        define(c, PATH, new PathData());
    }
    /* (the bitmap's id for the natives, and its size) */
    function sid(c) { var cv = c._canvas; return cv && cv[OFFID] !== undefined ? cv[OFFID] : cv; }
    function dims(c) {
        var cv = c._canvas;
        if (cv && cv[OFFID] !== undefined) return [cv[OFFW], cv[OFFH]];
        return [cv.width, cv.height];
    }
    function opaque(c) { return c._attrs && c._attrs.alpha === false; }
    function canvasGetter(name) {
        Object.defineProperty(CP, name, { get: function () { return this._canvas; }, configurable: true, enumerable: true });
    }
    canvasGetter('canvas');
    Object.defineProperty(OCP, 'canvas', { get: function () { return this._canvas; }, configurable: true, enumerable: true });

    /* ---- filters: CSS filter text or CanvasFilter input, as (kind, n, values...) lists ---- */
    function lenPx(s) {
        s = String(s).trim();
        var m = /^(-?[\d.]+(?:e[+-]?\d+)?)(px|em|rem|pt|pc|in|cm|mm|q)?$/i.exec(s);
        if (!m) return NaN;
        var v = Number(m[1]), u = (m[2] || '').toLowerCase();
        if (!u && v !== 0) return NaN;
        return u === 'em' || u === 'rem' ? v * 16 : u === 'pt' ? v * 4 / 3 : u === 'pc' ? v * 16 : u === 'in' ? v * 96 : u === 'cm' ? v * 96 / 2.54 : u === 'mm' ? v * 96 / 25.4 : u === 'q' ? v * 96 / 101.6 : v;
    }
    function amount(s, d) {
        s = String(s === undefined ? '' : s).trim();
        if (s === '') return d;
        if (s.slice(-1) === '%') return Number(s.slice(0, -1)) / 100;
        return Number(s);
    }
    function splitTop(s) {
        var out = [], depth = 0, cur = '';
        for (var i = 0; i < s.length; i++) {
            var ch = s.charAt(i);
            if (ch === '(') depth++;
            if (ch === ')') depth--;
            if (/\s/.test(ch) && !depth) { if (cur) out.push(cur); cur = ''; continue; }
            cur += ch;
        }
        if (cur) out.push(cur);
        return out;
    }
    function parseCssFilter(text) {
        var s = String(text).trim(), out = [];
        if (s === 'none' || s === '') return out;
        var re = /([a-z-]+)\(((?:[^()]|\([^()]*\))*)\)/gi, m, last = 0;
        while ((m = re.exec(s))) {
            if (s.slice(last, m.index).trim()) return null;
            last = re.lastIndex;
            var f = m[1].toLowerCase(), a = m[2].trim(), v;
            if (f === 'blur') { v = a ? lenPx(a) : 0; if (!(v >= 0)) return null; out.push(1, 1, v); }
            else if (f === 'drop-shadow') {
                var parts = splitTop(a), lens = [], col = null;
                parts.forEach(function (p) { var L = lenPx(p); if (isFinite(L)) lens.push(L); else col = parseColor(p); });
                if (lens.length < 2) return null;
                var c = rgbaOf(col || (g_current ? parseColor('currentcolor') : '#000000')) || [0, 0, 0, 1];
                out.push(10, 7, lens[0], lens[1], lens[2] || 0, c[0], c[1], c[2], c[3]);
            } else {
                var idx = { brightness: 2, contrast: 3, grayscale: 4, 'hue-rotate': 5, invert: 6, opacity: 7, saturate: 8, sepia: 9 }[f];
                if (!idx) return null;
                if (idx === 5) { v = a ? hue(a) : 0; } else v = amount(a, 1);
                if (!isFinite(v) || (idx !== 5 && v < 0)) return null;
                out.push(idx, 1, v);
            }
        }
        if (s.slice(last).trim()) return null;
        return out;
    }
    function filterFromObject(f) {
        var list = Array.isArray(f) ? f : [f], out = [];
        for (var i = 0; i < list.length; i++) {
            var o = list[i];
            if (!o || typeof o !== 'object') continue;
            var name = String(o.name || '');
            if (name === 'gaussianBlur') {
                var sd = o.stdDeviation, sx = 0, sy = 0;
                if (Array.isArray(sd)) { sx = num(sd[0], 0); sy = num(sd.length > 1 ? sd[1] : sd[0], 0); }
                else sx = sy = num(sd, 0);
                out.push(12, 2, Math.max(0, sx), Math.max(0, sy));
            } else if (name === 'dropShadow') {
                var c = rgbaOf(parseColor(o.floodColor === undefined ? 'black' : o.floodColor)) || [0, 0, 0, 1];
                var op = o.floodOpacity === undefined ? 1 : num(o.floodOpacity, 1);
                var sd0 = o.stdDeviation === undefined ? 2 : o.stdDeviation, dsx, dsy;
                if (sd0 !== null && typeof sd0 === 'object' && typeof sd0[Symbol.iterator] === 'function') { var sa = Array.from(sd0); dsx = Number(sa[0]); dsy = Number(sa.length > 1 ? sa[1] : sa[0]); }
                else dsx = dsy = Number(sd0);
                if (!isFinite(dsx)) dsx = 0;
                if (!isFinite(dsy)) dsy = 0;
                /* (a negative deviation: none, a sharp shadow) */
                var fa = c[3] * Math.max(0, Math.min(1, op));
                out.push(10, 8, num(o.dx === undefined ? 2 : o.dx, 0), num(o.dy === undefined ? 2 : o.dy, 0), Math.max(0, dsx), Math.max(0, dsy), c[0], c[1], c[2], fa);
            } else if (name === 'componentTransfer') {
                var parts = [];
                ['funcR', 'funcG', 'funcB', 'funcA'].forEach(function (key) {
                    var fo = o[key], t = fo && fo.type, vals = [];
                    var ty = t === 'table' ? 1 : t === 'discrete' ? 2 : t === 'linear' ? 3 : t === 'gamma' ? 4 : 0;
                    if (ty === 1 || ty === 2) { vals = Array.from(fo.tableValues || []).map(Number); if (!vals.length || !vals.every(isFinite)) { ty = 0; vals = []; } }
                    else if (ty === 3) vals = [num(fo.slope === undefined ? 1 : fo.slope, 1), num(fo.intercept, 0)];
                    else if (ty === 4) vals = [num(fo.amplitude === undefined ? 1 : fo.amplitude, 1), num(fo.exponent === undefined ? 1 : fo.exponent, 1), num(fo.offset, 0)];
                    parts.push(ty, vals.length); vals.forEach(function (x) { parts.push(x); });
                });
                out.push(13, parts.length); parts.forEach(function (x) { out.push(x); });
            } else if (name === 'colorMatrix') {
                var t = o.type || 'matrix', vals = o.values;
                if (t === 'matrix' && vals && vals.length === 20) { out.push(11, 20); for (var q = 0; q < 20; q++) out.push(num(vals[q], 0)); }
                else if (t === 'saturate') out.push(8, 1, num(vals, 1));
                else if (t === 'hueRotate') out.push(5, 1, num(vals, 0));
                else if (t === 'luminanceToAlpha') out.push(11, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.2125, 0.7154, 0.0721, 0, 0);
            }
        }
        return out;
    }
    function CanvasFilter(init) {
        if (!new.target) throw new TypeError("Failed to construct 'CanvasFilter': Please use the 'new' operator");
        define(this, '_list', filterFromObject(init));
    }

    function setStyle(c, name, v) {
        var s = c._s;
        if (name === 'fillStyle' || name === 'strokeStyle') {
            if (v instanceof CanvasGradient || v instanceof CanvasPattern) { if (v[UNCLEAN]) taint(c); s[name] = v; return; }
            g_current = c._canvas && c._canvas.nodeType === 1 ? c._canvas : null;
            var col = parseColor(v);
            g_current = null;
            if (col !== null) s[name] = col;
        } else if (name === 'shadowColor') {
            g_current = c._canvas && c._canvas.nodeType === 1 ? c._canvas : null;
            var sc = parseColor(v);
            g_current = null;
            if (sc !== null) s[name] = sc;
        } else if (typeof DEFAULTS[name] === 'number') {
            v = Number(v);
            if (!isFinite(v)) return;
            if (name === 'globalAlpha' && (v < 0 || v > 1)) return;
            if ((name === 'lineWidth' || name === 'miterLimit') && v <= 0) return;
            if (name === 'shadowBlur' && v < 0) return;
            s[name] = v;
        } else if (typeof DEFAULTS[name] === 'boolean') {
            s[name] = !!v;
        } else if (ENUMS[name]) {
            if (ENUMS[name].indexOf(String(v)) >= 0) s[name] = String(v);
        } else if (name === 'font') {
            var f = normalizeFont(String(v));
            if (f) s.font = f;
        } else if (name === 'filter') {
            if (v && typeof v === 'object' && v instanceof CanvasFilter) { s.filter = v; s.fparsed = v._list; return; }
            var fl = parseCssFilter(String(v));
            if (fl === null) return;
            s.filter = String(v); s.fparsed = fl;
        } else if (name === 'letterSpacing' || name === 'wordSpacing') {
            var L = lenPx(String(v));
            if (!isFinite(L)) return;
            s[name] = String(v).trim().replace(/^(-?)0+(\d)/, '$1$2');
        } else {
            s[name] = String(v);
        }
    }
    Object.keys(DEFAULTS).forEach(function (name) {
        var d = { configurable: true, enumerable: true,
            get: function () { return this._s[name]; },
            set: function (v) { setStyle(this, name, v); } };
        Object.defineProperty(CP, name, d);
        Object.defineProperty(OCP, name, d);
    });
    var STRETCH = { 'ultra-condensed': 50, 'extra-condensed': 62.5, condensed: 75, 'semi-condensed': 87.5, normal: 100, 'semi-expanded': 112.5, expanded: 125, 'extra-expanded': 150, 'ultra-expanded': 200 };
    var SIZES = { math: 16, 'xx-small': 9, 'x-small': 10, small: 13, medium: 16, large: 18, 'x-large': 24, 'xx-large': 32, 'xxx-large': 48 };
    function normalizeFont(f) {
        f = f.trim();
        var m = /(?:^|\s)(\d*\.?\d+(?:e[+-]?\d+)?)(px|pt|em|rem|%|pc|in|cm|mm|q|ex|ch|vw|vh)(?:\s*\/\s*[^\s]+)?\s+(.+)$/i.exec(f);
        var size, prefix, fam;
        if (m) {
            size = Number(m[1]);
            var unit = m[2].toLowerCase();
            if (unit === 'pt') size = size * 4 / 3;
            else if (unit === 'pc') size *= 16;
            else if (unit === 'in') size *= 96;
            else if (unit === 'cm') size = size * 96 / 2.54;
            else if (unit === 'mm') size = size * 96 / 25.4;
            else if (unit === 'q') size = size * 96 / 101.6;
            else if (unit === 'em' || unit === 'rem') size *= 10;
            else if (unit === '%') size = size * 10 / 100;
            else if (unit === 'ex' || unit === 'ch') size *= 5;
            prefix = f.slice(0, m.index + (/^\s/.test(m[0]) ? 1 : 0)).trim();
            fam = m[3].trim();
        } else {
            var k = /(?:^|\s)(xx-small|x-small|small|medium|large|x-large|xx-large|xxx-large|math)(?:\s*\/\s*[^\s]+)?\s+(.+)$/i.exec(f);
            if (!k) return null;
            size = SIZES[k[1].toLowerCase()];
            prefix = f.slice(0, k.index + (/^\s/.test(k[0]) ? 1 : 0)).trim();
            fam = k[2].trim();
        }
        if (/^(inherit|initial|unset|revert)$/i.test(fam)) return null;
        var toks = prefix ? prefix.split(/\s+/) : [];
        for (var i = 0; i < toks.length; i++)
            if (!/^(normal|italic|oblique|small-caps|bold|bolder|lighter|[1-9]00|condensed|expanded|semi-condensed|semi-expanded|ultra-condensed|extra-condensed|ultra-expanded|extra-expanded)$/i.test(toks[i])) return null;
        var keep = toks.filter(function (t) { return t.toLowerCase() !== 'normal'; }).map(function (t) { return t.toLowerCase(); });
        return (keep.length ? keep.join(' ') + ' ' : '') + (Math.round(size * 1000) / 1000) + 'px ' + fam;
    }
    function fontParts(c) {
        var f = c._s.font, m = /(?:^|\s)(\d*\.?\d+(?:e[+-]?\d+)?)px\s+(.+)$/.exec(f);
        var size = m ? Number(m[1]) : 10, fam = m ? m[2] : 'sans-serif', pre = m ? f.slice(0, m.index) : '';
        var weight = 400, italic = 0, stretch = STRETCH[c._s.fontStretch] || 100;
        pre.split(/\s+/).forEach(function (t) {
            if (t === 'italic') italic = 1;
            else if (t === 'oblique') italic = 2;
            else if (STRETCH[t]) stretch = STRETCH[t];
            else if (t === 'bold' || t === 'bolder') weight = 700;
            else if (t === 'lighter') weight = 100;
            else if (/^[1-9]00$/.test(t)) weight = Number(t);
        });
        return { size: size, family: fam, weight: weight + 1000 * Math.round(stretch * 10), italic: italic };
    }

    function saveState(c) {
        c._stack.push({ s: Object.assign({}, c._s, { dash: c._s.dash.slice() }), m: c._m.slice(), layer: false });
        if (HAVE) N.save(sid(c));
    }
    function restoreState(c) {
        var t = c._stack[c._stack.length - 1];
        if (!t || t.layer) return;
        c._stack.pop();
        define(c, '_s', t.s);
        define(c, '_m', t.m);
        if (HAVE) N.restore(sid(c));
    }
    /* (the 2D contexts' operations: on a context only, an Illegal invocation else) */
    /* (each operation's IDL required argument count, where its function declares more) */
    var BOTH_REQ = { beginLayer: 0, setTransform: 0, fill: 0, stroke: 0, clip: 0, isPointInPath: 2, isPointInStroke: 2,
        fillText: 3, strokeText: 3, fillTextCluster: 3, strokeTextCluster: 3, createImageData: 1, getImageData: 4, putImageData: 3,
        arc: 5, ellipse: 7, roundRect: 4, drawFocusIfNeeded: 1 };
    function both(name, fn) {
        var req = BOTH_REQ[name] !== undefined ? BOTH_REQ[name] : fn.length, w = function () { if (this == null || !this._s) throw new TypeError('Illegal invocation'); if (arguments.length < req) throw new TypeError("Failed to execute '" + name + "': " + req + ' argument' + (req > 1 ? 's' : '') + ' required, but only ' + arguments.length + ' present.'); return fn.apply(this, arguments); };
        Object.defineProperty(w, 'name', { value: fn.name || name });
        Object.defineProperty(w, 'length', { value: req });
        define(CP, name, w); define(OCP, name, w);
    }
    both('save', function save() { saveState(this); });
    both('restore', function restore() { restoreState(this); });
    both('reset', function () {
        var d = dims(this);
        reset(this);
        if (HAVE) N.reset(sid(this), d[0], d[1], opaque(this));
    });
    both('isContextLost', function () { return false; });
    both('getContextAttributes', function () {
        var a = this._attrs;
        return { alpha: a.alpha !== false, colorSpace: a.colorSpace || 'srgb', colorType: a.colorType || 'unorm8',
                 desynchronized: !!a.desynchronized, willReadFrequently: !!a.willReadFrequently };
    });
    /* layers */
    both('beginLayer', function beginLayer(opts) {
        var s = this._s, fl = s.fparsed || [], own = false;
        if (opts !== undefined && opts !== null) {
            if (typeof opts !== 'object') throw new TypeError("Failed to execute 'beginLayer': The provided value is not of type 'BeginLayerOptions'.");
            if (opts.filter !== undefined && opts.filter !== null) {
                var f = opts.filter;
                fl = f instanceof CanvasFilter ? f._list : typeof f === 'string' ? (parseCssFilter(f) || []) : filterFromObject(f);
                own = true;
            }
        }
        var sc = rgbaOf(s.shadowColor) || [0, 0, 0, 0];
        var sh = new Float64Array([sc[0], sc[1], sc[2], sc[3], s.shadowOffsetX, s.shadowOffsetY, s.shadowBlur]);
        /* (a layer's filter in its own space: its drop shadows' offsets through the transform) */
        var m_ = this._m;
        if (m_ && fl.length && (m_[0] !== 1 || m_[1] !== 0 || m_[2] !== 0 || m_[3] !== 1)) {
            fl = fl.slice();
            for (var i_ = 0; i_ + 1 < fl.length; i_ += 2 + fl[i_ + 1]) {
                if (fl[i_] === 10 && fl[i_ + 1] >= 8) {   /* (the object form; a CSS one is turned natively) */
                    var dx_ = fl[i_ + 2], dy_ = fl[i_ + 3];
                    fl[i_ + 2] = m_[0] * dx_ + m_[2] * dy_;
                    fl[i_ + 3] = m_[1] * dx_ + m_[3] * dy_;
                }
            }
        }
        if (HAVE) N.beginLayer(sid(this), s.globalAlpha, ENUMS.globalCompositeOperation.indexOf(s.globalCompositeOperation), sh, new Float64Array(fl), singular(this), own);
        /* (the state is saved, then the layer's own rendering state starts over) */
        this._stack.push({ s: Object.assign({}, s, { dash: s.dash.slice() }), m: this._m.slice(), layer: true });
        this._layers++;
        s.globalAlpha = 1; s.globalCompositeOperation = 'source-over';
        s.shadowOffsetX = 0; s.shadowOffsetY = 0; s.shadowBlur = 0; s.shadowColor = 'rgba(0, 0, 0, 0)';
        s.filter = 'none'; s.fparsed = [];
    });
    both('endLayer', function endLayer() {
        if (!this._layers) throw domErr('endLayer() called without a matching beginLayer().', 'InvalidStateError');
        while (this._stack.length && !this._stack[this._stack.length - 1].layer) {
            var t0 = this._stack.pop();
            define(this, '_s', t0.s); define(this, '_m', t0.m);
        }
        var t = this._stack.pop();
        if (t) { define(this, '_s', t.s); define(this, '_m', t.m); }
        this._layers--;
        if (HAVE) N.endLayer(sid(this));
    });
    /* transforms: m = [a, b, c, d, e, f] */
    function mul(m, a, b, c, d, e, f) {
        return [m[0] * a + m[2] * b, m[1] * a + m[3] * b, m[0] * c + m[2] * d, m[1] * c + m[3] * d,
                m[0] * e + m[2] * f + m[4], m[1] * e + m[3] * f + m[5]];
    }
    function setM(c, m) { if (m.every(isFinite)) define(c, '_m', m); }
    /* (a DOMMatrix2DInit: a and m11, b and m12, ... each the same when both are given) */
    function check2d(o) {
        var same = function (x, y) { return x === y || (x !== x && y !== y) || (x === 0 && y === 0); };
        [['a', 'm11'], ['b', 'm12'], ['c', 'm21'], ['d', 'm22'], ['e', 'm41'], ['f', 'm42']].forEach(function (p) {
            if (o[p[0]] !== undefined && o[p[1]] !== undefined && !same(Number(o[p[0]]), Number(o[p[1]])))
                throw new TypeError(p[0] + ' and ' + p[1] + ' must be the same');
        });
        ['m13', 'm14', 'm23', 'm24', 'm31', 'm32', 'm34', 'm43'].forEach(function (k) {
            if (o[k] !== undefined && Number(o[k]) !== 0) throw new TypeError('The matrix is not 2D.');
        });
        ['m33', 'm44'].forEach(function (k) {
            if (o[k] !== undefined && Number(o[k]) !== 1) throw new TypeError('The matrix is not 2D.');
        });
        if (o.is2D === false) throw new TypeError('The matrix is not 2D.');
    }
    both('transform', function transform(a, b, c, d, e, f) { setM(this, mul(this._m, +a, +b, +c, +d, +e, +f)); });
    both('setTransform', function setTransform(a, b, c, d, e, f) {
        if (arguments.length === 0 || a === undefined || (a !== null && typeof a === 'object')) {
            var o = a || {};
            check2d(o);
            var g = function (x, y, d) { return x !== undefined ? Number(x) : y !== undefined ? Number(y) : d; };
            setM(this, [g(o.a, o.m11, 1), g(o.b, o.m12, 0), g(o.c, o.m21, 0), g(o.d, o.m22, 1), g(o.e, o.m41, 0), g(o.f, o.m42, 0)]);
        } else {
            setM(this, [+a, +b, +c, +d, +e, +f]);
        }
    });
    both('resetTransform', function resetTransform() { define(this, '_m', [1, 0, 0, 1, 0, 0]); });
    both('getTransform', function getTransform() {
        var m = this._m;
        if (typeof global.DOMMatrix === 'function') {
            try { return new global.DOMMatrix([m[0], m[1], m[2], m[3], m[4], m[5]]); } catch (e) { /* below */ }
        }
        return { a: m[0], b: m[1], c: m[2], d: m[3], e: m[4], f: m[5], is2D: true,
                 isIdentity: m[0] === 1 && m[1] === 0 && m[2] === 0 && m[3] === 1 && m[4] === 0 && m[5] === 0 };
    });
    both('translate', function translate(x, y) { setM(this, mul(this._m, 1, 0, 0, 1, +x, +y)); });
    both('scale', function scale(x, y) { setM(this, mul(this._m, +x, 0, 0, +y, 0, 0)); });
    both('rotate', function rotate(a) { a = +a; if (!isFinite(a)) return; var c = Math.cos(a), s = Math.sin(a); setM(this, mul(this._m, c, s, -s, c, 0, 0)); });
    /* line dashes */
    both('setLineDash', function setLineDash(segments) {
        if (segments === null || typeof segments !== 'object') throw new TypeError("Failed to execute 'setLineDash': The provided value cannot be converted to a sequence.");
        var d = Array.prototype.map.call(Array.from(segments), Number);
        if (d.some(function (x) { return !isFinite(x) || x < 0; })) return;
        if (d.length % 2) d = d.concat(d);
        this._s.dash = d;
    });
    both('getLineDash', function getLineDash() { return this._s.dash.slice(); });
    /* paths */
    both('beginPath', function beginPath() { define(this, PATH, new PathData()); });
    for (var pm in PATH_METHODS) both(pm, PATH_METHODS[pm]);

    /* ---- drawing ---- */
    function stateArr(c, extra) {
        var s = c._s, m = c._m, sc = rgbaOf(s.shadowColor) || [0, 0, 0, 0];
        var a = [s.globalAlpha, ENUMS.globalCompositeOperation.indexOf(s.globalCompositeOperation), s.lineWidth,
                 ENUMS.lineCap.indexOf(s.lineCap), ENUMS.lineJoin.indexOf(s.lineJoin), s.miterLimit, s.lineDashOffset, s.dash.length];
        for (var i = 0; i < s.dash.length; i++) a.push(s.dash[i]);
        a.push(sc[0], sc[1], sc[2], sc[3], s.shadowOffsetX, s.shadowOffsetY, s.shadowBlur, extra && extra.rule ? 1 : 0,
               s.imageSmoothingEnabled ? 1 : 0, m[0], m[1], m[2], m[3], m[4], m[5]);
        var fl = s.fparsed || [];
        a.push(fl.length);
        for (var j = 0; j < fl.length; j++) a.push(fl[j]);
        a.push(ENUMS.imageSmoothingQuality.indexOf(s.imageSmoothingQuality));
        return new Float64Array(a);
    }
    function stopsOf(g) {
        var st = g._stops.map(function (p, i) { return [p[0], p[1], i]; }).sort(function (p, q) { return p[0] - q[0] || p[2] - q[2]; });
        var out = [st.length];
        st.forEach(function (p) { var c = rgbaOf(p[1]) || [0, 0, 0, 0]; out.push(p[0], c[0], c[1], c[2], c[3]); });
        return out;
    }
    var REPS = { repeat: 0, 'repeat-x': 1, 'repeat-y': 2, 'no-repeat': 3 };
    /* [paint array, image source] for a style */
    function paintOf(c, style) {
        var m = c._m;
        if (typeof style === 'string') {
            var col = p3Color(c, rgbaOf(style) || [0, 0, 0, 1], style);
            return [new Float64Array([0, col[0], col[1], col[2], col[3]]), undefined];
        }
        if (style instanceof CanvasGradient) {
            var cg = cssGradientPaint(c, style);
            if (cg) return cg;
            var a = style._args.map(Number), kind = style._kind === 'linear' ? 1 : style._kind === 'radial' ? 2 : 3;
            return [new Float64Array([kind].concat(m, a, stopsOf(style))), undefined];
        }
        if (style instanceof CanvasPattern) {
            var pm = mul(m, style._mt[0], style._mt[1], style._mt[2], style._mt[3], style._mt[4], style._mt[5]);
            var sz = style._size || [0, 0];
            return [new Float64Array([4].concat(pm, [REPS[style._rep], 0, 0, sz[0], sz[1], c._s.imageSmoothingEnabled ? 1 : 0])), p3Key(c, style._src)];
        }
        return [new Float64Array([0, 0, 0, 0, 1]), undefined];
    }
    /* (a transform that can't be inverted: nothing is drawn) */
    function singular(c) { var m = c._m, det = m[0] * m[3] - m[1] * m[2]; return !(det !== 0 && isFinite(det)); }
    function doDraw(c, mode, d, style, extra) {
        if (!HAVE || !c._canvas || singular(c)) return;
        var dm = dims(c);
        if (!dm[0] || !dm[1]) return;
        var p = style !== undefined ? paintOf(c, style) : [new Float64Array([0, 0, 0, 0, 1]), undefined];
        N.draw(sid(c), dm[0], dm[1], mode, new Float64Array(d), p[0], stateArr(c, extra), p[1]);
    }
    function ruleOf(r) {
        if (r === undefined) return 0;
        r = String(r);
        if (r !== 'nonzero' && r !== 'evenodd') throw new TypeError("The provided value '" + r + "' is not a valid enum value of type CanvasFillRule.");
        return r === 'evenodd' ? 1 : 0;
    }
    function pathArg(c, a) {
        if (a instanceof Path2D) return mapPath(a[PATH].d, c._m);
        return c[PATH].d;
    }
    both('fill', function fill(a, b) {
        var rule = a instanceof Path2D ? ruleOf(b) : ruleOf(a);
        if (a !== undefined && !(a instanceof Path2D) && typeof a !== 'string') throw new TypeError("Failed to execute 'fill': parameter 1 is not of type 'Path2D'.");
        doDraw(this, 0, pathArg(this, a), this._s.fillStyle, { rule: rule });
    });
    both('stroke', function stroke(a) {
        if (a !== undefined && !(a instanceof Path2D)) throw new TypeError("Failed to execute 'stroke': parameter 1 is not of type 'Path2D'.");
        doDraw(this, 1, pathArg(this, a), this._s.strokeStyle);
    });
    both('clip', function clip(a, b) {
        var rule = a instanceof Path2D ? ruleOf(b) : ruleOf(a);
        if (!HAVE || !this._canvas) return;
        var dm = dims(this);
        N.draw(sid(this), dm[0], dm[1], 2, new Float64Array(pathArg(this, a)), new Float64Array([0, 0, 0, 0, 1]), stateArr(this, { rule: rule }));
    });
    function hitTest(c, d, rule, x, y, stroke) {
        x = Number(x); y = Number(y);
        if (!isFinite(x) || !isFinite(y) || !HAVE) return false;
        var dm = dims(c);
        return !!N.hit(dm[0], dm[1], new Float64Array(d), rule, x, y, stroke ? 1 : 0, stateArr(c));
    }
    both('isPointInPath', function isPointInPath(a, b, c, d) {
        if (a instanceof Path2D) return hitTest(this, mapPath(a[PATH].d, this._m), ruleOf(d), b, c, false);
        return hitTest(this, this[PATH].d, ruleOf(c), a, b, false);
    });
    both('isPointInStroke', function isPointInStroke(a, b, c) {
        if (a instanceof Path2D) return hitTest(this, mapPath(a[PATH].d, this._m), 0, b, c, true);
        return hitTest(this, this[PATH].d, 0, a, b, true);
    });
    both('drawFocusIfNeeded', function () {});
    both('scrollPathIntoView', function () {});
    /* rectangles */
    function rectPath(c, x, y, w, h) {
        var m = c._m;
        function P(px, py) { return [m[0] * px + m[2] * py + m[4], m[1] * px + m[3] * py + m[5]]; }
        var a = P(x, y), b = P(x + w, y), d = P(x + w, y + h), e = P(x, y + h);
        return [0, a[0], a[1], 1, b[0], b[1], 1, d[0], d[1], 1, e[0], e[1], 3];
    }
    both('fillRect', function fillRect(x, y, w, h) {
        if (this._legacy) { putRect(this, x, y, w, h, false); return; }
        x = +x; y = +y; w = +w; h = +h;
        if (!finite(x, y, w, h) || !w || !h) return;
        doDraw(this, 0, rectPath(this, x, y, w, h), this._s.fillStyle);
    });
    both('strokeRect', function strokeRect(x, y, w, h) {
        x = +x; y = +y; w = +w; h = +h;
        if (!finite(x, y, w, h)) return;
        if (!w && !h) return;
        if (!w || !h) {
            /* (a line: stroked as one, with butt ends) */
            var m = this._m;
            var a = [m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]], b = [m[0] * (x + w) + m[2] * (y + h) + m[4], m[1] * (x + w) + m[3] * (y + h) + m[5]];
            doDraw(this, 1, [0, a[0], a[1], 1, b[0], b[1]], this._s.strokeStyle);
            return;
        }
        doDraw(this, 1, rectPath(this, x, y, w, h), this._s.strokeStyle);
    });
    both('clearRect', function clearRect(x, y, w, h) {
        if (this._legacy) { putRect(this, x, y, w, h, true); return; }
        x = +x; y = +y; w = +w; h = +h;
        if (!finite(x, y, w, h) || !w || !h || !HAVE) return;
        var dm = dims(this);
        N.draw(sid(this), dm[0], dm[1], 3, new Float64Array(rectPath(this, x, y, w, h)), new Float64Array([0, 0, 0, 0, 1]), stateArr(this));
    });

    /* (a paint worklet's context: solid rectangles kept as display-list rectangles on its element) */
    function putRect(ctx, x, y, w, h, clear) {
        var f = global.__cyberCanvasRect, m = ctx._m, cv = ctx._canvas;
        if (typeof f !== 'function' || !cv || m[1] !== 0 || m[2] !== 0) return;
        x = num(x, NaN); y = num(y, NaN); w = num(w, NaN); h = num(h, NaN);
        if (!isFinite(x + y + w + h)) return;
        var X = m[0] * x + m[4], Y = m[3] * y + m[5], W = m[0] * w, H = m[3] * h;
        if (clear) { f(cv, X, Y, W, H, 0, 0, 0, 0, 1); return; }
        var s = ctx._s, c = rgbaOf(s.fillStyle);
        var gimg = global.__cyberCanvasImage, fg = s.fillStyle;
        if (!c && fg instanceof CanvasGradient && fg._kind === 'linear' && fg._stops.length && typeof gimg === 'function' &&
            s.globalCompositeOperation === 'source-over' && W > 0 && H > 0) {
            var a = fg._args.map(Number);
            var x0 = m[0] * a[0] + m[4], y0 = m[3] * a[1] + m[5], x1 = m[0] * a[2] + m[4], y1 = m[3] * a[3] + m[5];
            var dx = x1 - x0, dy = y1 - y0, len = Math.sqrt(dx * dx + dy * dy);
            if (!(len > 0) || !isFinite(len)) return;
            var ux = dx / len, uy = dy / len, L = Math.abs(W * ux) + Math.abs(H * uy);
            var cx = X + W / 2, cy = Y + H / 2, k = (cx - x0) * ux + (cy - y0) * uy;
            var st = fg._stops.slice().map(function (p, i) { return [p[0], p[1], i]; }).sort(function (p, q) { return p[0] - q[0] || p[2] - q[2]; });
            var parts = st.map(function (p) { return p[1] + ' ' + (len * p[0] - k + L / 2).toFixed(3) + 'px'; });
            var deg = Math.atan2(dx, -dy) * 180 / Math.PI;
            var css = 'linear-gradient(' + deg.toFixed(4) + 'deg, ' + parts.join(', ') + ')';
            if (s.globalAlpha < 1) return;
            gimg(cv, 'cyber-cvgrad:' + css, X, Y, W, H);
            return;
        }
        if (!c || s.globalCompositeOperation !== 'source-over') return;
        f(cv, X, Y, W, H, c[0], c[1], c[2], c[3] * s.globalAlpha, 0);
    }
    /* (a paint worklet's drawing: a real bitmap w x h, and how to get its image cache key) */
    var paintBitmaps = new WeakMap();
    define(global, '__cyberPaintBitmap', function (w, h, el, layer) {
        if (!HAVE || typeof global.__c2dKey !== 'function') return null;
        var W = Math.max(1, Math.ceil(w)), H = Math.max(1, Math.ceil(h));
        /* (one per element and layer, cleared each time: its image cache key stays) */
        var per = el && typeof el === 'object' ? paintBitmaps.get(el) : null;
        if (el && typeof el === 'object' && !per) { per = []; paintBitmaps.set(el, per); }
        var oc = per && per[layer | 0];
        if (!oc) { oc = new OffscreenCanvas(W, H); if (per) per[layer | 0] = oc; }
        if (oc[OFFW] !== W || oc[OFFH] !== H) { oc.width = W; oc.height = H; }
        var c = oc.getContext('2d');
        c.reset();
        return { ctx: c, key: function () { return global.__c2dKey(oc[OFFID]); } };
    });
    define(global, '__cyberPaintContext', function (el, keep) {
        var f = global.__cyberCanvasRect;
        if (typeof f !== 'function' || !el) return null;
        if (!keep) f(el, -1e7, -1e7, 2e7, 2e7, 0, 0, 0, 0, 1);
        var c = newContext(el);
        define(c, '_legacy', true);
        return c;
    });

    /* ---- text ---- */
    function textDir(c) {
        var d = c._s.direction;
        if (d !== 'inherit') return d;
        var cv = c._canvas;
        try { if (cv && cv.nodeType === 1 && cv.isConnected) return global.getComputedStyle(cv).direction === 'rtl' ? 'rtl' : 'ltr'; } catch (e) { }
        try { if (cv && cv.nodeType === 1 && cv.dir) return cv.dir === 'rtl' ? 'rtl' : 'ltr'; } catch (e) { }
        return 'ltr';
    }
    function alignOf(c) {
        var a = c._s.textAlign, rtl = textDir(c) === 'rtl';
        if (a === 'start') a = rtl ? 'right' : 'left';
        else if (a === 'end') a = rtl ? 'left' : 'right';
        return a === 'right' ? 1 : a === 'center' ? 2 : 0;
    }
    /* (font-kerning: none for what is drawn or measured next) */
    function kernOff(on) { if (typeof global.__c2dNoKern === 'function') global.__c2dNoKern(!!on); }
    function drawText(c, text, x, y, maxw, stroke) {
        if (!HAVE || !c._canvas) return;
        x = +x; y = +y;
        if (!finite(x, y)) return;
        var mw = -1;
        if (maxw !== undefined) { mw = +maxw; if (!isFinite(mw)) return; if (mw <= 0) return; }
        var dm = dims(c);
        if (!dm[0] || !dm[1] || singular(c)) return;
        var f = fontParts(c), p = paintOf(c, stroke ? c._s.strokeStyle : c._s.fillStyle);
        var str = String(text);
        kernOff(c._s.fontKerning === 'none');
        N.text(sid(c), dm[0], dm[1], str, f.family, f.weight, f.italic, f.size, x, y, alignOf(c),
               ENUMS.textBaseline.indexOf(c._s.textBaseline) === 3 ? 0 : [1, 2, 3, 0, 4, 5][ENUMS.textBaseline.indexOf(c._s.textBaseline)],
               mw, stroke ? 1 : 0, p[0], stateArr(c), p[1], textDir(c) === 'rtl' ? 1 : 0);
    }
    both('fillText', function fillText(text, x, y, maxw) {
        if (arguments.length < 3) throw new TypeError("Failed to execute 'fillText': 3 arguments required.");
        drawText(this, text, x, y, maxw, false);
    });
    both('strokeText', function strokeText(text, x, y, maxw) {
        if (arguments.length < 3) throw new TypeError("Failed to execute 'strokeText': 3 arguments required.");
        drawText(this, text, x, y, maxw, true);
    });
    function TextMetrics() { illegal(); }
    var TMV = Symbol('textMetrics');
    both('measureText', function measureText(text) {
        text = String(text).replace(/[\t\n\f\r]/g, ' ');
        kernOff(this._s.fontKerning === 'none');
        var f = fontParts(this), size = f.size, w = 0, r = HAVE ? N.measure(text, f.family, f.weight, f.italic, size, textDir(this) === 'rtl' ? 1 : 0) : null;
        kernOff(false);
        var asc = size * 0.8, desc = size * 0.2, bx0 = 0, by0 = -size * 0.72, bx1 = 0, by1 = size * 0.2;
        if (r) { w = r[0]; asc = r[1]; desc = r[2]; bx0 = r[3]; by0 = r[4]; bx1 = r[5]; by1 = r[6]; }
        else {
            for (var i = 0; i < text.length; i++) {
                var ch = text.charCodeAt(i);
                w += ch === 32 ? 0.28 : ch >= 0x2e80 ? 1 : (ch >= 65 && ch <= 90) ? 0.66 : 'iljt.,:;!|\''.indexOf(text.charAt(i)) >= 0 ? 0.28 : 0.55;
            }
            w *= size; bx1 = w;
        }
        var al = alignOf(this), left = al === 2 ? w / 2 : al === 1 ? w : 0;
        var tot = asc + desc, na = tot > 0 ? asc / tot * size : asc, nd = tot > 0 ? desc / tot * size : desc;
        var bl = this._s.textBaseline;
        var oy = bl === 'top' ? na : bl === 'hanging' ? asc * 0.8 : bl === 'middle' ? (na - nd) / 2 : bl === 'ideographic' ? -desc : bl === 'bottom' ? -nd : 0;
        var m = Object.create(TextMetrics.prototype);
        var vals = { width: w, actualBoundingBoxLeft: left - bx0, actualBoundingBoxRight: bx1 - left,
            actualBoundingBoxAscent: -by0 - oy, actualBoundingBoxDescent: by1 + oy,
            fontBoundingBoxAscent: asc - oy, fontBoundingBoxDescent: desc + oy,
            emHeightAscent: na - oy, emHeightDescent: nd + oy,
            hangingBaseline: asc * 0.8 - oy, alphabeticBaseline: -oy, ideographicBaseline: -desc - oy };
        define(m, TMV, vals);
        define(m, '_tc', { text: text, font: this._s.font, align: al, w: w, baseline: bl, direction: textDir(this), ctx: this, nokern: this._s.fontKerning === 'none' });
        return m;
    });
    /* (TextMetrics' attributes: getters on its prototype, the measure in a slot of each) */
    ['width', 'actualBoundingBoxLeft', 'actualBoundingBoxRight', 'fontBoundingBoxAscent', 'fontBoundingBoxDescent',
     'actualBoundingBoxAscent', 'actualBoundingBoxDescent', 'emHeightAscent', 'emHeightDescent', 'hangingBaseline',
     'alphabeticBaseline', 'ideographicBaseline'].forEach(function (k) {
        var g = { g: function () { var v = this && this[TMV]; if (!v) throw new TypeError('Illegal invocation'); return v[k]; } }.g;
        Object.defineProperty(g, 'name', { value: 'get ' + k });
        Object.defineProperty(TextMetrics.prototype, k, { get: g, enumerable: true, configurable: true });
    });
    /* ---- text clusters: grapheme clusters, each where fillText would put it ---- */
    function graphemes(t) {
        var out = [];
        if (global.Intl && typeof global.Intl.Segmenter === 'function') {
            try { var it = new global.Intl.Segmenter(undefined, { granularity: 'grapheme' }).segment(t); for (var s of it) out.push([s.index, s.index + s.segment.length]); return out; } catch (e) { out = []; }
        }
        for (var i = 0; i < t.length;) {
            var j = i + (t.codePointAt(i) > 0xffff ? 2 : 1);
            while (j < t.length) {
                var c = t.codePointAt(j);
                if ((c >= 0x300 && c <= 0x36f) || (c >= 0xfe00 && c <= 0xfe0f) || c === 0x200d || (c >= 0x1f3fb && c <= 0x1f3ff) || (c >= 0xe0100 && c <= 0xe01ef) || (c >= 0x20d0 && c <= 0x20ff)) {
                    j += c > 0xffff ? 2 : 1;
                    if (c === 0x200d && j < t.length) j += t.codePointAt(j) > 0xffff ? 2 : 1;
                } else break;
            }
            out.push([i, j]); i = j;
        }
        return out;
    }
    function TextCluster() { illegal(); }
    define(TextMetrics.prototype, 'getTextClusters', function getTextClusters(a, b, opts) {
        var tc = this._tc;
        if (!tc) return [];
        if (a !== null && typeof a === 'object') { opts = a; a = undefined; }
        var start = 0, end = tc.text.length;
        if (a !== undefined || b !== undefined) {
            var ab = indexArgs(tc, a === undefined ? 0 : a, b === undefined ? tc.text.length : b, 'getTextClusters', true);
            if (ab[0] >= tc.text.length && tc.text.length) throw domErr("Failed to execute 'getTextClusters' on 'TextMetrics': The range is out of bounds.", 'IndexSizeError');
            start = ab[0]; end = ab[1];
        }
        var c = tc.ctx, saved = c._s.font;
        c._s.font = tc.font;
        var f = fontParts(c);
        c._s.font = saved;
        var off = tc.align === 2 ? -tc.w / 2 : tc.align === 1 ? -tc.w : 0;
        var alignName = tc.align === 2 ? 'center' : tc.align === 1 ? 'right' : 'left';
        var out = [];
        graphemes(tc.text).forEach(function (g) {
            if (g[0] < start || g[1] > end) return;
            /* (its pen position: the text through it less its own advance, kerning with what is before kept) */
            var rd = tc.direction === 'rtl' ? 1 : 0;
            kernOff(tc.nokern);
            var upto = g[0] && HAVE ? N.measure(tc.text.slice(0, g[1]), f.family, f.weight, f.italic, f.size, rd) : null;
            var own = g[0] && HAVE ? N.measure(tc.text.slice(g[0], g[1]), f.family, f.weight, f.italic, f.size, rd) : null;
            kernOff(false);
            var px = upto && own ? upto[0] - own[0] : 0;
            var cl = Object.create(TextCluster.prototype);
            var v = { x: off + px, y: 0, begin: g[0], end: g[1], align: 'left', baseline: tc.baseline };
            for (var k in v) Object.defineProperty(cl, k, { value: v[k], enumerable: true });
            define(cl, '_tc', tc);
            define(cl, '_alignAt', alignName);
            out.push(cl);
        });
        return out;
    });
    /* (the text's clusters laid out left to right, as fillText puts them: bidi levels, visual order; each [begin, end, level, x0, x1] from the alignment point) */
    function visualClusters(tc) {
        if (tc._vis) return tc._vis;
        var c = tc.ctx, saved = c._s.font;
        c._s.font = tc.font;
        var f = fontParts(c);
        c._s.font = saved;
        var rtl = tc.direction === 'rtl' ? 1 : 0;
        var lv = typeof global.__c2dBidi === 'function' ? global.__c2dBidi(tc.text, rtl) : null;
        var gr = graphemes(tc.text), sw = null;
        /* (each one's advance in the text, what is beside it counted, as the page's text has it) */
        if (HAVE && typeof global.__c2dSpanW === 'function') {
            var pairs = [];
            gr.forEach(function (g) { pairs.push(g[0], g[1]); });
            try { sw = global.__c2dSpanW(tc.text, f.family, f.weight, f.italic, f.size, new Float64Array(pairs)); } catch (e) { sw = null; }
        }
        var gs = gr.map(function (g, gi) {
            var wv = sw && sw.length > gi && sw[gi] > 0 ? sw[gi] : null;
            var r = wv === null && HAVE ? N.measure(tc.text.slice(g[0], g[1]), f.family, f.weight, f.italic, f.size, rtl) : null;
            return [g[0], g[1], lv && lv.length > g[0] ? lv[g[0]] : rtl, 0, wv !== null ? wv : r ? r[0] : f.size * 0.5];
        });
        /* (L2: from the highest level down to the lowest odd one, each run at least that high reversed) */
        var order = gs.slice(), hi = 0, lo = 255;
        gs.forEach(function (g) { if (g[2] > hi) hi = g[2]; if (g[2] & 1 && g[2] < lo) lo = g[2]; });
        for (var L = hi; L >= lo && L > 0; L--) {
            for (var i = 0; i < order.length;) {
                if (order[i][2] < L) { i++; continue; }
                var j = i;
                while (j < order.length && order[j][2] >= L) j++;
                var run = order.slice(i, j).reverse();
                for (var q = 0; q < run.length; q++) order[i + q] = run[q];
                i = j;
            }
        }
        var sum = 0;
        order.forEach(function (g) { sum += g[4]; });
        var k = sum > 0 ? tc.w / sum : 1, x = tc.align === 2 ? -tc.w / 2 : tc.align === 1 ? -tc.w : 0;
        order.forEach(function (g) { var wd = g[4] * k; g[3] = x; g[4] = x + wd; x += wd; });
        define(tc, '_vis', order);
        return order;
    }
    var BOXH = {};
    function inlineBoxHeight(font) {
        if (font in BOXH) return BOXH[font];
        var h = 0, doc = global.document;
        try {
            if (doc && doc.body && doc.defaultView) {
                var sp = doc.createElement('span');
                sp.style.font = font;
                sp.style.position = 'absolute';
                sp.style.visibility = 'hidden';
                sp.textContent = 'x';
                doc.body.appendChild(sp);
                h = sp.getBoundingClientRect().height;
                sp.remove();
            }
        } catch (e) { h = 0; }
        BOXH[font] = h;
        return h;
    }
    /* (unsigned longs, [EnforceRange]: negative a TypeError; past the text, or start past end where that is wrong, an IndexSizeError) */
    function indexArgs(tc, a, b, what, ordered) {
        a = Number(a); b = Number(b);
        if (!isFinite(a) || !isFinite(b) || Math.trunc(a) < 0 || Math.trunc(b) < 0)
            throw new TypeError("Failed to execute '" + what + "' on 'TextMetrics': Value is outside the 'unsigned long' value range.");
        a = Math.trunc(a); b = Math.trunc(b);
        var n = tc.text.length;
        if (a > n || b > n || (ordered && a > b))
            throw domErr("Failed to execute '" + what + "' on 'TextMetrics': The range is out of bounds.", 'IndexSizeError');
        return [a, b];
    }
    define(TextMetrics.prototype, 'getIndexFromOffset', function getIndexFromOffset(offset) {
        var tc = this._tc;
        if (!tc) return 0;
        offset = Number(offset);
        var vis = visualClusters(tc), n = tc.text.length, rtl = tc.direction === 'rtl';
        if (!vis.length) return 0;
        var x0 = vis[0][3], x1 = vis[vis.length - 1][4];
        if (offset <= x0) return rtl ? n : 0;
        if (offset >= x1) return rtl ? 0 : n;
        for (var i = 0; i < vis.length; i++) {
            var g = vis[i];
            if (offset >= g[3] && offset < g[4]) {
                var leftHalf = offset < (g[3] + g[4]) / 2;
                return (g[2] & 1) ? (leftHalf ? g[1] : g[0]) : (leftHalf ? g[0] : g[1]);
            }
        }
        return rtl ? 0 : n;
    });
    define(TextMetrics.prototype, 'getSelectionRects', function getSelectionRects(start, end) {
        var tc = this._tc;
        if (!tc) return [];
        var r = indexArgs(tc, start, end, 'getSelectionRects');
        /* (an offset inside a surrogate pair: past it, as a DOM range's ends are taken) */
        var snapCP = function (o) { var c0 = tc.text.charCodeAt(o); return o > 0 && o < tc.text.length && c0 >= 0xDC00 && c0 < 0xE000 ? o + 1 : o; };
        var rev = r[0] > r[1];
        r = [snapCP(r[0]), snapCP(r[1])];
        /* (start past end: a collapsed range at end, as a DOM range's is: one empty rect at that caret; so too one made empty) */
        var collapsed = rev || r[0] >= r[1];
        if (collapsed) r = rev ? [r[1], r[1]] : [r[0], r[0]];
        var vis = visualClusters(tc), out = [], cur = null;
        var top = -this.fontBoundingBoxAscent, ht = this.fontBoundingBoxAscent + this.fontBoundingBoxDescent;
        /* (the height of an inline box of the font, as the page's text has: a line's content area) */
        var bh = inlineBoxHeight(tc.font);
        if (bh > 0) ht = bh;
        vis.forEach(function (g) {
            var sel = g[0] < r[1] && g[1] > r[0];
            if (sel && cur && cur.lv === g[2] && Math.abs(cur.x1 - g[3]) < 1e-6) { cur.x1 = g[4]; return; }
            if (cur) { out.push(cur); cur = null; }
            if (sel) cur = { x0: g[3], x1: g[4], lv: g[2] };
        });
        if (cur) out.push(cur);
        if (collapsed && vis.length) {
            var cx = null;
            vis.forEach(function (g) {
                if (cx !== null) return;
                if (g[0] === r[0]) cx = (g[2] & 1) ? g[4] : g[3];
                else if (g[1] === r[0]) cx = (g[2] & 1) ? g[3] : g[4];
            });
            out = [{ x0: cx === null ? 0 : cx, x1: cx === null ? 0 : cx }];
        }
        return out.map(function (p) { return new global.DOMRect(p.x0, top, p.x1 - p.x0, ht); });
    });
    define(TextMetrics.prototype, 'getActualBoundingBox', function getActualBoundingBox(start, end) {
        var tc = this._tc;
        if (!tc) return new global.DOMRect(0, 0, 0, 0);
        var r = indexArgs(tc, start, end, 'getActualBoundingBox');
        var t = '';
        for (var i = 0; i < tc.text.length; i++) t += i >= r[0] && i < r[1] ? tc.text.charAt(i) : ' ';
        var c = tc.ctx, s = c._s, keep = [s.font, s.textAlign, s.textBaseline, s.direction];
        s.font = tc.font; s.textBaseline = tc.baseline; s.direction = tc.direction;
        s.textAlign = tc.align === 2 ? 'center' : tc.align === 1 ? 'right' : 'left';
        var m;
        try { m = c.measureText(t); } finally { s.font = keep[0]; s.textAlign = keep[1]; s.textBaseline = keep[2]; s.direction = keep[3]; }
        return new global.DOMRect(-m.actualBoundingBoxLeft, -m.actualBoundingBoxAscent, m.actualBoundingBoxLeft + m.actualBoundingBoxRight, m.actualBoundingBoxAscent + m.actualBoundingBoxDescent);
    });
    function drawCluster(c, cl, x, y, opts, stroke) {
        if (!(cl instanceof TextCluster)) throw new TypeError("Failed to execute 'fillTextCluster': parameter 1 is not of type 'TextCluster'.");
        var tc = cl._tc, s = c._s;
        var keep = [s.font, s.textAlign, s.textBaseline, s.direction];
        s.font = tc.font; s.textAlign = 'left'; s.textBaseline = cl.baseline; s.direction = tc.direction;
        var cx = opts && opts.x !== undefined ? Number(opts.x) : cl.x, cy = opts && opts.y !== undefined ? Number(opts.y) : cl.y;
        try { drawText(c, tc.text.slice(cl.begin, cl.end), Number(x) + cx, Number(y) + cy, undefined, stroke); }
        finally { s.font = keep[0]; s.textAlign = keep[1]; s.textBaseline = keep[2]; s.direction = keep[3]; }
    }
    both('fillTextCluster', function fillTextCluster(cl, x, y, opts) { drawCluster(this, cl, x, y, opts, false); });
    both('strokeTextCluster', function strokeTextCluster(cl, x, y, opts) { drawCluster(this, cl, x, y, opts, true); });
    expose('TextCluster', TextCluster);

    /* ---- image sources ---- */
    var OFFID = Symbol('offid'), OFFW = Symbol('offw'), OFFH = Symbol('offh');
    var BMID = Symbol('bitmap');
    function isImg(o) { return global.HTMLImageElement && o instanceof global.HTMLImageElement; }
    function isSvgImg(o) { return global.SVGImageElement && o instanceof global.SVGImageElement; }
    function isCanvasEl(o) { return global.HTMLCanvasElement && o instanceof global.HTMLCanvasElement; }
    function isVideo(o) { return global.HTMLVideoElement && o instanceof global.HTMLVideoElement; }
    function absUrl(u) { try { return new URL(u, global.document && global.document.baseURI).href; } catch (e) { return u; } }
    /* {src, w, h} for drawImage/createPattern: src a url or a bitmap id; null: nothing to draw;
     * throws for a broken or unusable image */
    function sourceOf(image, what) {
        if (image === null || typeof image !== 'object')
            throw new TypeError("Failed to execute '" + what + "': The provided value is not of type '(CSSImageValue or HTMLCanvasElement or HTMLImageElement or HTMLVideoElement or ImageBitmap or OffscreenCanvas or SVGImageElement or VideoFrame)'.");
        if (image[BMID] !== undefined) {
            if (image[BMID] === null) throw domErr('The image source is detached.', 'InvalidStateError');
            return { src: image[BMID], w: image.width, h: image.height };
        }
        if (image[OFFID] !== undefined) {
            if (!image[OFFW] || !image[OFFH]) throw domErr('The image argument is an OffscreenCanvas element with a width or height of 0.', 'InvalidStateError');
            return { src: image[OFFID], w: image[OFFW], h: image[OFFH] };
        }
        if (isCanvasEl(image)) {
            if (!image.width || !image.height) throw domErr('The image argument is a canvas element with a width or height of 0.', 'InvalidStateError');
            return { src: image, w: image.width, h: image.height };
        }
        if (isImg(image)) {
            var u = image.currentSrc || image.src;
            if (!u) return null;
            var nw = image.naturalWidth, nh = image.naturalHeight;
            if (image.complete && !nw && !nh) {
                var sz0 = HAVE ? N.size(absUrl(u)) : null;
                if (!sz0) throw domErr('The HTMLImageElement provided is in the \'broken\' state.', 'InvalidStateError');
                nw = sz0[0]; nh = sz0[1];
            }
            if (!nw || !nh) { var sz = HAVE ? N.size(absUrl(u)) : null; if (!sz) return null; nw = sz[0]; nh = sz[1]; }
            var io = '';
            try { if (image.isConnected) io = global.getComputedStyle(image).imageOrientation; } catch (e) { }
            var key = absUrl(u);
            /* (image-orientation: none on a same-origin image: its pixels as stored, unturned) */
            try {
                if (io === 'none' && new URL(key, global.location.href).origin === global.location.origin) {
                    var nk = 'noexif:' + key, ns = HAVE ? N.size(nk) : null;
                    if (ns) return { src: nk, w: ns[0], h: ns[1] };
                }
            } catch (e) { }
            return { src: key, w: nw, h: nh };
        }
        if (isSvgImg(image)) {
            var h = image.href && image.href.baseVal !== undefined ? image.href.baseVal : image.getAttribute('href') || image.getAttributeNS('http://www.w3.org/1999/xlink', 'href');
            if (!h) return null;
            var sz2 = HAVE ? N.size(absUrl(h)) : null;
            if (!sz2) return null;
            var wv = 0, hv = 0;
            try { wv = image.width.baseVal.value; hv = image.height.baseVal.value; } catch (e) { }
            return { src: absUrl(h), w: wv || sz2[0], h: hv || sz2[1], nw: sz2[0], nh: sz2[1] };
        }
        if (isVideo(image)) {
            /* (HAVE_CURRENT_DATA or more: its frame now, at its natural size) */
            if (image.readyState < 2 || !image.videoWidth || !image.videoHeight) return null;
            return { src: image, w: image.videoWidth, h: image.videoHeight };
        }
        if (image.__text) return null;
        if (image instanceof ImageData) throw new TypeError("Failed to execute '" + what + "': The provided value is not of type 'CanvasImageSource'.");
        throw new TypeError("Failed to execute '" + what + "': The provided value is not of type '(CSSImageValue or HTMLCanvasElement or HTMLImageElement or HTMLVideoElement or ImageBitmap or OffscreenCanvas or SVGImageElement or VideoFrame)'.");
    }

    both('drawImage', function drawImage(image) {
        var n = arguments.length;
        if (n !== 3 && n !== 5 && n !== 9)
            throw new TypeError("Failed to execute 'drawImage': Valid arities are: [3, 5, 9], but " + n + " arguments provided.");
        if (this._legacy) { legacyDrawImage.apply(this, arguments); return; }
        var info = sourceOf(image, 'drawImage');
        if (!info || !HAVE) return;
        if (sourceTainted(image)) taint(this);
        var a = [];
        for (var q = 1; q < n; q++) a.push(Number(arguments[q]));
        if (!a.every(isFinite)) return;
        var iw = info.nw || info.w, ih = info.nh || info.h, kx = info.nw ? info.nw / info.w : 1, ky = info.nh ? info.nh / info.h : 1;
        var sx, sy, sw, sh, dx, dy, dw, dh;
        if (n === 3) { sx = 0; sy = 0; sw = iw; sh = ih; dx = a[0]; dy = a[1]; dw = info.w; dh = info.h; }
        else if (n === 5) { sx = 0; sy = 0; sw = iw; sh = ih; dx = a[0]; dy = a[1]; dw = a[2]; dh = a[3]; }
        else { sx = a[0] * kx; sy = a[1] * ky; sw = a[2] * kx; sh = a[3] * ky; dx = a[4]; dy = a[5]; dw = a[6]; dh = a[7]; }
        if (sw < 0) { sx += sw; sw = -sw; }
        if (sh < 0) { sy += sh; sh = -sh; }
        if (dw < 0) { dx += dw; dw = -dw; }
        if (dh < 0) { dy += dh; dh = -dh; }
        if (!sw || !sh || !dw || !dh) return;
        /* (the source rectangle cut to the image, the destination with it) */
        var fx = dw / sw, fy = dh / sh;
        if (sx < 0) { dx -= sx * fx; dw += sx * fx; sw += sx; sx = 0; }
        if (sy < 0) { dy -= sy * fy; dh += sy * fy; sh += sy; sy = 0; }
        if (sx + sw > iw) { dw -= (sx + sw - iw) * fx; sw = iw - sx; }
        if (sy + sh > ih) { dh -= (sy + sh - ih) * fy; sh = ih - sy; }
        if (!(sw > 0) || !(sh > 0) || !(dw > 0) || !(dh > 0)) return;
        var dm = dims(this);
        if (!dm[0] || !dm[1] || singular(this)) return;
        var pm = mul(this._m, fx, 0, 0, fy, dx - sx * fx, dy - sy * fy);
        var paint = new Float64Array([4].concat(pm, [4, sx, sy, sw, sh, this._s.imageSmoothingEnabled ? 1 : 0]));
        /* (into a display-p3 bitmap: an image decoded to P3; another bitmap's pixels converted to the context's space first) */
        var cs0 = spaceSource(this, image, info);
        N.draw(sid(this), dm[0], dm[1], 0, new Float64Array(rectPath(this, dx, dy, dw, dh)), paint, stateArr(this), cs0.src);
        if (cs0.tmp && N.free) N.free(cs0.src);
    });
    /* (a source for context c: a url keyed for its space; a bitmap in another space than c's, or a video into a P3 one, its pixels converted into a temporary one) */
    function spaceSource(c, image, info) {
        if (typeof info.src === 'string') return { src: p3Key(c, info.src) };
        /* (a bitmap of an image, into a display-p3 bitmap: the image decoded to P3 again, cut and scaled the same) */
        if (image && image[BP3SRC] && storeSpace(c) === 'display-p3') {
            if (image[BP3ID] === undefined) {
                var r3 = image[BP3SRC], b3 = null;
                try { b3 = bitmapFrom('p3:' + r3.url, r3.w, r3.h, r3.crop, r3.opts); } catch (e) { b3 = null; }
                define(image, BP3ID, b3 ? b3[BMID] : null);
            }
            if (image[BP3ID] !== null) return { src: image[BP3ID] };
        }
        var to = storeSpace(c), from = isVideo(image) ? 'srgb' : srcSpace(image);
        if (from === to) return { src: info.src };
        var w0 = info.nw || info.w, h0 = info.nh || info.h, key = info.src, tmpv = null;
        if (!(w0 > 0) || !(h0 > 0)) return { src: info.src };
        if (isVideo(image)) {
            tmpv = new OffscreenCanvas(w0, h0);
            tmpv.getContext('2d').drawImage(image, 0, 0);
            key = tmpv[OFFID];
        }
        var buf0 = N.get(key, 0, 0, w0, h0);
        if (tmpv && N.free) N.free(tmpv[OFFID]);
        if (!buf0) return { src: info.src };
        var px0 = new global.Uint8ClampedArray(buf0);
        convertPixels(px0, from, to);
        var oc0 = new OffscreenCanvas(w0, h0);
        N.put(oc0[OFFID], w0, h0, px0, w0, h0, 0, 0, 0, 0, w0, h0);
        return { src: oc0[OFFID], tmp: true };
    }
    var BSPACE = Symbol('bitmapSpace'), BP3SRC = Symbol('bitmapImage'), BP3ID = Symbol('bitmapP3');
    /* ---- origin-clean (HTML 4.12.5.1.x): a bitmap that has had another origin's pixels drawn in can't be read back ---- */
    var UNCLEAN = Symbol('originUnclean');
    function docOrigin() { try { return (global.document && global.document.origin) || global.location.origin; } catch (e) { return ''; } }
    /* (a resource's URL another origin's: it, or a hop of a redirect.py chain to it, unless asked for with CORS) */
    function urlTainted(u, el) {
        if (!u) return false;
        var url;
        try { url = new global.URL(String(u), (global.document && global.document.baseURI) || global.location.href); } catch (e) { return false; }
        if (/^(data|blob|about|javascript):$/.test(url.protocol)) return false;
        if (el && el.getAttribute && el.getAttribute('crossorigin') !== null) return false;
        var mine = docOrigin(), hop = url;
        for (var i = 0; i < 5 && hop; i++) {
            if (hop.origin !== mine) return true;
            if (!/\/common\/redirect\.py$/.test(hop.pathname)) break;
            var loc = hop.searchParams.get('location');
            try { hop = loc ? new global.URL(loc, hop.href) : null; } catch (e) { hop = null; }
        }
        return false;
    }
    function sourceTainted(image) {
        if (!image || (typeof image !== 'object' && typeof image !== 'function')) return false;
        try {
            if (image[UNCLEAN]) return true;
            if (isVideo(image)) return urlTainted(image.currentSrc || image.getAttribute('src'), image);
            if (isSvgImg(image)) {
                var h = image.href && image.href.baseVal !== undefined ? image.href.baseVal : image.getAttribute('href') || image.getAttributeNS('http://www.w3.org/1999/xlink', 'href');
                return urlTainted(h, image);
            }
            if (isImg(image)) return urlTainted(image.currentSrc || image.src, image);
        } catch (e) { }
        return false;
    }
    function taint(c) { var cv = c && c._canvas; if (cv && !cv[UNCLEAN]) define(cv, UNCLEAN, true); }
    function checkClean(cv, what) {
        if (cv && cv[UNCLEAN]) throw domErr("Failed to execute '" + what + "': Tainted canvases may not be exported.", 'SecurityError');
    }
    /* (a bitmap of an image's url: what it was made from kept, for a display-p3 context's own decode of it) */
    function urlBitmap(url, w, h, crop, opts) {
        var b = bitmapFrom(url, w, h, crop, opts);
        if (typeof url === 'string' && !/^(p3|cyber|cbfilter):/.test(url)) define(b, BP3SRC, { url: url, w: w, h: h, crop: crop, opts: opts });
        return b;
    }
    /* (the space a canvas's pixels are in: its context's; a bitmap's, the one its source had) */
    function srcSpace(image) {
        try {
            if (image && image[BMID] !== undefined) return image[BSPACE] || 'srgb';
            if (isCanvasEl(image) && image[CONTEXT]) return storeSpace(image[CONTEXT]);
            if (image && image[OFFID] !== undefined && image[OCTX]) return storeSpace(image[OCTX]);
        } catch (e) { }
        return 'srgb';
    }
    function legacyDrawImage(image) {
        var f = global.__cyberCanvasImage, m = this._m, n = arguments.length;
        if (image && image.__text && typeof image.src !== 'string' && (n === 5 || n === 3)) {
            var um = /^url\(\s*(['"]?)(.*?)\1\s*\)$/.exec(String(image.__text).trim());
            if (!um) return;
            var abs = absUrl(um[2]);
            var xw = num(arguments[1], NaN), yw = num(arguments[2], NaN), ww = n === 5 ? num(arguments[3], NaN) : 0, hw = n === 5 ? num(arguments[4], NaN) : 0;
            if (typeof f !== 'function' || m[1] !== 0 || m[2] !== 0 || !isFinite(xw + yw + ww + hw) || (n === 5 && (!ww || !hw))) return;
            f(this._canvas, abs, m[0] * xw + m[4], m[3] * yw + m[5], m[0] * ww, m[3] * hw);
            return;
        }
        if (typeof f !== 'function' || n === 9 || m[1] !== 0 || m[2] !== 0 || !image || typeof image.src !== 'string' || !image.src) return;
        var w = n === 5 ? num(arguments[3], NaN) : (image.naturalWidth || image.width || 0);
        var h = n === 5 ? num(arguments[4], NaN) : (image.naturalHeight || image.height || 0);
        var x = num(arguments[1], NaN), y = num(arguments[2], NaN);
        if (!isFinite(x + y + w + h) || !w || !h) return;
        f(this._canvas, image.src, m[0] * x + m[4], m[3] * y + m[5], m[0] * w, m[3] * h);
    }
    function idArgs(name, vals) {
        for (var i = 0; i < vals.length; i++) if (!isFinite(vals[i])) throw new TypeError("Failed to execute '" + name + "': The provided double value is non-finite.");
    }
    both('createImageData', function createImageData(a, b, settings) {
        if (a instanceof ImageData) return new ImageData(a.width, a.height, { colorSpace: a.colorSpace });
        if (arguments.length < 2) throw new TypeError("Failed to execute 'createImageData': 2 arguments required.");
        var w = Number(a), h = Number(b);
        idArgs('createImageData', [w, h]);
        w = Math.abs(Math.trunc(w)); h = Math.abs(Math.trunc(h));
        if (!w || !h) throw domErr('The source width/height is 0.', 'IndexSizeError');
        return new ImageData(w, h, { colorSpace: colorSpaceOf(settings, ctxColorSpace(this)) });
    });
    both('getImageData', function getImageData(x, y, w, h, settings) {
        if (arguments.length < 4) throw new TypeError("Failed to execute 'getImageData': 4 arguments required.");
        checkClean(this._canvas, 'getImageData');
        var outCs = colorSpaceOf(settings, ctxColorSpace(this));
        x = Number(x); y = Number(y); w = Number(w); h = Number(h);
        idArgs('getImageData', [x, y, w, h]);
        x = Math.trunc(x); y = Math.trunc(y); w = Math.trunc(w); h = Math.trunc(h);
        if (!w || !h) throw domErr('The source width/height is 0.', 'IndexSizeError');
        if (w < 0) { x += w; w = -w; }
        if (h < 0) { y += h; h = -h; }
        var buf = HAVE ? N.get(sid(this), x, y, w, h) : null;
        var px = buf ? new global.Uint8ClampedArray(buf) : new global.Uint8ClampedArray(w * h * 4);
        if (outCs !== storeSpace(this)) convertPixels(px, storeSpace(this), outCs);
        return new ImageData(px, w, h, { colorSpace: outCs });
    });
    both('putImageData', function putImageData(data, dx, dy, x0, y0, w0, h0) {
        if (!(data instanceof ImageData)) throw new TypeError("Failed to execute 'putImageData': parameter 1 is not of type 'ImageData'.");
        if (arguments.length !== 3 && arguments.length !== 7) throw new TypeError("Failed to execute 'putImageData': Valid arities are: [3, 7].");
        dx = Number(dx); dy = Number(dy);
        var full = arguments.length === 3;
        var rx = full ? 0 : Number(x0), ry = full ? 0 : Number(y0), rw = full ? data.width : Number(w0), rh = full ? data.height : Number(h0);
        idArgs('putImageData', [dx, dy, rx, ry, rw, rh]);
        dx = Math.trunc(dx); dy = Math.trunc(dy); rx = Math.trunc(rx); ry = Math.trunc(ry); rw = Math.trunc(rw); rh = Math.trunc(rh);
        if (rw < 0) { rx += rw; rw = -rw; }
        if (rh < 0) { ry += rh; rh = -rh; }
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rx + rw > data.width) rw = data.width - rx;
        if (ry + rh > data.height) rh = data.height - ry;
        if (rw <= 0 || rh <= 0 || !HAVE) return;
        var dm = dims(this);
        if (!dm[0] || !dm[1]) return;
        var src = data.data;
        if ((data.colorSpace || 'srgb') !== storeSpace(this)) src = convertPixels(new global.Uint8ClampedArray(src), data.colorSpace || 'srgb', storeSpace(this));
        N.put(sid(this), dm[0], dm[1], src, data.width, data.height, dx, dy, rx, ry, rx + rw, ry + rh);
    });
    both('createLinearGradient', function (x0, y0, x1, y1) {
        if (arguments.length < 4) throw new TypeError("Failed to execute 'createLinearGradient': 4 arguments required.");
        var a = [x0, y0, x1, y1].map(Number);
        idArgs('createLinearGradient', a);
        return gradient('linear', a);
    });
    both('createRadialGradient', function (x0, y0, r0, x1, y1, r1) {
        if (arguments.length < 6) throw new TypeError("Failed to execute 'createRadialGradient': 6 arguments required.");
        var a = [x0, y0, r0, x1, y1, r1].map(Number);
        idArgs('createRadialGradient', a);
        if (a[2] < 0 || a[5] < 0) throw domErr('The radius provided is negative.', 'IndexSizeError');
        return gradient('radial', a);
    });
    both('createConicGradient', function (a, x, y) {
        if (arguments.length < 3) throw new TypeError("Failed to execute 'createConicGradient': 3 arguments required.");
        var v = [a, x, y].map(Number);
        idArgs('createConicGradient', v);
        return gradient('conic', v);
    });
    both('createPattern', function (image, repetition) {
        if (arguments.length < 2) throw new TypeError("Failed to execute 'createPattern': 2 arguments required.");
        var info = sourceOf(image, 'createPattern');
        var rep = repetition === null || repetition === '' ? 'repeat' : String(repetition);
        if (!(rep in REPS)) throw domErr("The provided type ('" + rep + "') is not one of 'repeat', 'no-repeat', 'repeat-x', or 'repeat-y'.", 'SyntaxError');
        if (!info) return null;
        var p = Object.create(CanvasPattern.prototype);
        define(p, '_rep', rep);
        define(p, '_img', image);
        if (sourceTainted(image)) define(p, UNCLEAN, true);
        /* (a canvas: its pixels now, as the pattern keeps them, in this context's space) */
        var src = info.src;
        if (HAVE && typeof src !== 'string') {
            var cps = spaceSource(this, image, info);
            var id = N.newId();
            if (N.copy(id, cps.src)) src = id;
            if (cps.tmp && N.free) N.free(cps.src);
        }
        define(p, '_src', src);
        define(p, '_size', [info.w, info.h]);
        p._mt = [1, 0, 0, 1, 0, 0];
        return p;
    });
    var CP_TAG = 'CanvasRenderingContext2D';
    Object.defineProperty(CP, Symbol.toStringTag, { value: CP_TAG, configurable: true });
    Object.defineProperty(OCP, Symbol.toStringTag, { value: 'OffscreenCanvasRenderingContext2D', configurable: true });

    /* ---- encoding ---- */
    function b64(buf) {
        var u = new Uint8Array(buf), s = '', CH = 0x8000;
        for (var i = 0; i < u.length; i += CH) s += String.fromCharCode.apply(null, u.subarray(i, i + CH));
        return global.btoa(s);
    }
    function encode(id, type, quality) {
        type = String(type === undefined ? 'image/png' : type).toLowerCase();
        var jpeg = type === 'image/jpeg';
        var q = typeof quality === 'number' && quality >= 0 && quality <= 1 ? quality : 0.92;
        var buf = HAVE ? N.png(id, jpeg ? q : undefined) : null;
        return buf ? { buf: buf, type: jpeg ? 'image/jpeg' : 'image/png' } : null;
    }
    function ensureBitmap(el) {
        /* (a canvas without a context: a blank bitmap for encoding) */
        if (HAVE && !el[CONTEXT]) N.reset(el, el.width, el.height, false);
    }

    /* ---- HTMLCanvasElement ---- */
    var CONTEXT = Symbol('context');
    var HCE = global.HTMLCanvasElement;
    if (!HCE) {
        HCE = function HTMLCanvasElement() { illegal(); };
        if (global.HTMLElement) {
            HCE.prototype = Object.create(global.HTMLElement.prototype);
            define(HCE.prototype, 'constructor', HCE);
        }
        expose('HTMLCanvasElement', HCE);
    }
    var HP = HCE.prototype;
    [['width', 300], ['height', 150]].forEach(function (d) {
        Object.defineProperty(HP, d[0], { configurable: true, enumerable: true,
            get: function () {
                var a = this.getAttribute(d[0]);
                if (a === null) return d[1];
                /* (rules for non-negative integers: -0 is 0, a negative one or one past 2^31 - 1 the default) */
                var m = /^[\t\n\f\r ]*([+-]?)(\d+)/.exec(a);
                if (!m) return d[1];
                var n = Number(m[2]);
                return (m[1] === '-' && n !== 0) || n > 2147483647 ? d[1] : n;
            },
            set: function (v) {
                v = Number(v);
                v = v >= 0 && v <= 2147483647 ? Math.floor(v) : d[1];
                this.setAttribute(d[0], String(v));
                /* setting either resets the bitmap and the context's state */
                var c = this[CONTEXT];
                if (c && !c._legacy) {
                    reset(c);
                    if (HAVE) N.reset(this, this.width, this.height, opaque(c));
                }
            } });
    });
    define(HP, 'getContext', function getContext(type, attrs) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'getContext': 1 argument required.");
        type = String(type);
        if (this[TRANSFERRED]) throw domErr('Cannot get context from a canvas that has transferred its control to offscreen.', 'InvalidStateError');
        if (type === 'bitmaprenderer') {
            if (this[CONTEXT]) return null;
            if (!this[BRCTX]) define(this, BRCTX, newBitmapRenderer(this));
            return this[BRCTX];
        }
        if (type !== '2d' || this[BRCTX]) return null;
        var c = this[CONTEXT];
        if (!c) {
            var a = attrs && typeof attrs === 'object' ? attrs : {};
            c = newContext(this, CP, { alpha: a.alpha === undefined ? true : !!a.alpha, colorSpace: colorSpaceOf(a, 'srgb'),
                colorType: a.colorType || 'unorm8', desynchronized: !!a.desynchronized, willReadFrequently: !!a.willReadFrequently });
            define(this, CONTEXT, c);
            if (HAVE) N.reset(this, this.width, this.height, c._attrs.alpha === false);
        }
        return c;
    });
    define(HP, 'toDataURL', function toDataURL(type, quality) {
        checkClean(this, 'toDataURL');
        if (!this.width || !this.height) return 'data:,';
        ensureBitmap(this);
        var e = encode(this, type, quality);
        return e ? 'data:' + e.type + ';base64,' + b64(e.buf) : 'data:,';
    });
    define(HP, 'toBlob', function toBlob(cb, type, quality) {
        if (typeof cb !== 'function') throw new TypeError("Failed to execute 'toBlob': parameter 1 is not of type 'BlobCallback'.");
        checkClean(this, 'toBlob');
        var blob = null;
        if (this.width && this.height) {
            ensureBitmap(this);
            var e = encode(this, type, quality);
            if (e && global.Blob) blob = new global.Blob([e.buf], { type: e.type });
        }
        global.setTimeout(function () { cb(blob); }, 0);
    });
    define(HP, 'captureStream', function captureStream() { return null; });

    /* ---- HTML in canvas (WICG html-in-canvas): a canvas with layoutsubtree / content=drawable
     * lays its children out; drawElementImage paints one of them into it (browser/canvas2d.c
     * records where, display_list.c paints the child's own display there) ---- */
    var ELIMG = Symbol('elementImage');
    function ElementImage() { illegal(); }
    Object.defineProperty(ElementImage.prototype, 'width', { configurable: true, enumerable: true, get: function () { var e = this[ELIMG]; return e ? Math.round(e.getBoundingClientRect().width) : 0; } });
    Object.defineProperty(ElementImage.prototype, 'height', { configurable: true, enumerable: true, get: function () { var e = this[ELIMG]; return e ? Math.round(e.getBoundingClientRect().height) : 0; } });
    define(ElementImage.prototype, 'close', function () { define(this, ELIMG, null); });
    expose('ElementImage', ElementImage);
    function drawableCanvas(cv) {
        return cv && cv.nodeType === 1 && (cv.hasAttribute('layoutsubtree') || cv.hasAttribute('content'));
    }
    function elementTarget(c, el, what) {
        var t = el && el[ELIMG] !== undefined ? el[ELIMG] : el;
        if (!t || !(global.Element && t instanceof global.Element)) throw new TypeError("Failed to execute '" + what + "': parameter 1 is not of type 'Element'.");
        var cv = c._canvas;
        if (!drawableCanvas(cv) || t.parentNode !== cv) throw domErr('The element is not a child of a canvas laid out for drawing.', 'InvalidStateError');
        return t;
    }
    /* (the context's filter then its shadow, for a drawn element: CSS filter's kinds, "t:v,...|dx,dy,blur,r,g,b,a") */
    var CANVAS_TO_CSS_FILTER = { 1: 9, 2: 7, 3: 8, 4: 1, 5: 4, 6: 5, 7: 6, 8: 3, 9: 2 };
    function elemFx(s) {
        var fl = s.fparsed || [], parts = [], ds = null;
        for (var i = 0; i + 1 < fl.length; i += 2 + fl[i + 1]) {
            if (fl[i] === 10) { if (fl[i + 1] === 7 && !ds) { ds = fl.slice(i + 2, i + 9); parts.push('10:0'); } continue; }
            var k = CANVAS_TO_CSS_FILTER[fl[i]];
            if (k && fl[i + 1] >= 1) parts.push(k + ':' + fl[i + 2]);
        }
        var sc = rgbaOf(s.shadowColor);
        if (!ds && sc && sc[3] > 0 && (s.shadowBlur || s.shadowOffsetX || s.shadowOffsetY)) {
            ds = [s.shadowOffsetX, s.shadowOffsetY, s.shadowBlur / 2, sc[0], sc[1], sc[2], sc[3]];
            parts.push('10:0');
        }
        if (!parts.length) return '';
        return parts.join(',') + (ds ? '|' + ds.join(',') : '');
    }
    both('drawElementImage', function drawElementImage(el) {
        var n = arguments.length, t = elementTarget(this, el, 'drawElementImage');
        var a = [];
        for (var q = 1; q < n; q++) a.push(Number(arguments[q]));
        if (!a.every(isFinite)) return null;
        var dx = 0, dy = 0, dw = 0, dh = 0, src = [0, 0, 0, 0];
        if (n === 3) { dx = a[0]; dy = a[1]; }
        else if (n === 5) { dx = a[0]; dy = a[1]; dw = a[2]; dh = a[3]; }
        else if (n === 7) {
            /* (a part of it at its own size: placed so the part's corner lands there) */
            if (!(a[2] > 0) || !(a[3] > 0)) return null;
            dx = a[4] - a[0]; dy = a[5] - a[1]; src = [a[0], a[1], a[2], a[3]];
        }
        else if (n === 9) {
            /* (a part of it: the whole scaled as the part is, placed so the part lands there) */
            if (!a[2] || !a[3]) return null;
            var kx9 = a[6] / a[2], ky9 = a[7] / a[3];
            dx = a[4] - a[0] * kx9; dy = a[5] - a[1] * ky9;
            dw = (t.offsetWidth || 0) * kx9; dh = (t.offsetHeight || 0) * ky9;
            if (!dw || !dh) { dw = 0; dh = 0; }
            src = [a[0], a[1], a[2], a[3]];
        }
        else if (n !== 1) throw new TypeError("Failed to execute 'drawElementImage': Valid arities are: [1, 3, 5, 7, 9].");
        var m = this._m, X = m[0] * dx + m[2] * dy + m[4], Y = m[1] * dx + m[3] * dy + m[5];
        var dm = dims(this);
        if (HAVE && typeof global.__c2dElem === 'function') global.__c2dElem(sid(this), dm[0], dm[1], t, X, Y, dw, dh, this._s.globalAlpha, src[0], src[1], src[2], src[3], elemFx(this._s));
        try { return new global.DOMMatrix([1, 0, 0, 1, X, Y]); } catch (e) { return null; }
    });
    both('getElementTransform', function getElementTransform(el) {
        elementTarget(this, el, 'getElementTransform');
        var m = this._m;
        try { return new global.DOMMatrix([m[0], m[1], m[2], m[3], m[4], m[5]]); } catch (e) { return null; }
    });
    define(HP, 'captureElementImage', function captureElementImage(el) {
        if (!(global.Element && el instanceof global.Element)) throw new TypeError("Failed to execute 'captureElementImage': parameter 1 is not of type 'Element'.");
        if (!drawableCanvas(this) || el.parentNode !== this) throw domErr('The element is not a child of this canvas.', 'InvalidStateError');
        var img = Object.create(ElementImage.prototype);
        define(img, ELIMG, el);
        return img;
    });
    define(HP, 'getElementTransform', function getElementTransform(el, m) {
        try { return m ? global.DOMMatrix.fromMatrix(m) : new global.DOMMatrix(); } catch (e) { return null; }
    });
    /* (the paint event: after a frame, when asked for or when its handler is set) */
    var PAINTQ = Symbol('paintQueued'), ONPAINT = Symbol('onpaint');
    function queuePaint(cv) {
        if (cv[PAINTQ]) return;
        define(cv, PAINTQ, true);
        var fire = function () {
            define(cv, PAINTQ, false);
            var ev;
            try { ev = new global.Event('paint'); } catch (e) { return; }
            try { Object.defineProperty(ev, 'changedElements', { value: Array.prototype.slice.call(cv.children), enumerable: true }); } catch (e) { }
            cv.dispatchEvent(ev);
        };
        if (typeof global.requestAnimationFrame === 'function') global.requestAnimationFrame(function () { global.setTimeout(fire, 0); });
        else global.setTimeout(fire, 0);
    }
    define(HP, 'requestPaint', function requestPaint() { queuePaint(this); });
    Object.defineProperty(HP, 'onpaint', { configurable: true, enumerable: true,
        get: function () { return this[ONPAINT] || null; },
        set: function (f) {
            if (this[ONPAINT]) this.removeEventListener('paint', this[ONPAINT]);
            define(this, ONPAINT, typeof f === 'function' ? f : null);
            if (typeof f === 'function') { this.addEventListener('paint', f); queuePaint(this); }
        } });
    var TRANSFERRED = Symbol('transferred');
    define(HP, 'transferControlToOffscreen', function () {
        if (this[CONTEXT]) throw domErr('Cannot transfer control from a canvas that has a rendering context.', 'InvalidStateError');
        if (this[TRANSFERRED]) throw domErr('Cannot transfer control from a canvas for more than one time.', 'InvalidStateError');
        define(this, TRANSFERRED, true);
        var oc = new OffscreenCanvas(this.width, this.height);
        /* (its drawing shown in this canvas: the same bitmap) */
        var el = this;
        define(oc, OFFID, null);
        Object.defineProperty(oc, OFFID, { configurable: true, get: function () { return el; } });
        define(oc, 'placeholder', el);
        return oc;
    });

    /* ---- ImageBitmapRenderingContext ---- */
    var BRCTX = Symbol('brctx');
    function ImageBitmapRenderingContext() { illegal(); }
    var BRP = ImageBitmapRenderingContext.prototype;
    function newBitmapRenderer(canvas) { var r = Object.create(BRP); define(r, '_canvas', canvas); return r; }
    Object.defineProperty(BRP, 'canvas', { configurable: true, enumerable: true, get: function () { return this._canvas; } });
    define(BRP, 'transferFromImageBitmap', function transferFromImageBitmap(b) {
        var cv = this._canvas, id = cv[OFFID] !== undefined ? cv[OFFID] : cv;
        if (b === null) { if (HAVE) N.reset(id, cv.width, cv.height, false); return; }
        if (!b || b[BMID] === undefined) throw new TypeError("Failed to execute 'transferFromImageBitmap': parameter 1 is not of type 'ImageBitmap'.");
        if (b[BMID] === null) throw domErr('The input ImageBitmap has been detached', 'InvalidStateError');
        if (HAVE) N.copy(id, b[BMID]);
        if (b[UNCLEAN] && !cv[UNCLEAN]) define(cv, UNCLEAN, true);
        b.close();
    });
    Object.defineProperty(BRP, Symbol.toStringTag, { value: 'ImageBitmapRenderingContext', configurable: true });
    expose('ImageBitmapRenderingContext', ImageBitmapRenderingContext);

    /* ---- OffscreenCanvas ---- */
    var OCTX = Symbol('octx');
    function OffscreenCanvas(w, h) {
        if (!new.target) throw new TypeError("Failed to construct 'OffscreenCanvas': Please use the 'new' operator");
        if (arguments.length < 2) throw new TypeError("Failed to construct 'OffscreenCanvas': 2 arguments required.");
        define(this, OFFID, HAVE ? N.newId() : 0);
        define(this, OFFW, Number(w) >>> 0);
        define(this, OFFH, Number(h) >>> 0);
    }
    var OP = OffscreenCanvas.prototype;
    [['width', OFFW], ['height', OFFH]].forEach(function (d) {
        Object.defineProperty(OP, d[0], { configurable: true, enumerable: true,
            get: function () { return this[d[1]]; },
            set: function (v) {
                define(this, d[1], Number(v) >>> 0);
                var c = this[OCTX];
                if (c) { reset(c); if (HAVE) N.reset(this[OFFID], this[OFFW], this[OFFH], opaque(c)); }
                if (this.placeholder) this.placeholder.setAttribute(d[0], String(this[d[1]]));
            } });
    });
    define(OP, 'getContext', function getContext(type, attrs) {
        if (arguments.length < 1) throw new TypeError("Failed to execute 'getContext': 1 argument required.");
        type = String(type);
        if (type !== '2d') {
            if (['webgl', 'webgl2', 'bitmaprenderer', 'webgpu'].indexOf(type) < 0)
                throw new TypeError("Failed to execute 'getContext': The provided value '" + type + "' is not a valid enum value of type OffscreenRenderingContextType.");
            return null;
        }
        var c = this[OCTX];
        if (!c) {
            var a = attrs && typeof attrs === 'object' ? attrs : {};
            c = newContext(this, OCP, { alpha: a.alpha === undefined ? true : !!a.alpha, colorSpace: colorSpaceOf(a, 'srgb'),
                colorType: a.colorType || 'unorm8', desynchronized: !!a.desynchronized, willReadFrequently: !!a.willReadFrequently });
            define(this, OCTX, c);
            if (HAVE) N.reset(this[OFFID], this[OFFW], this[OFFH], c._attrs.alpha === false);
        }
        return c;
    });
    define(OP, 'transferToImageBitmap', function () {
        var c = this[OCTX];
        if (!c) throw domErr('Cannot transfer an ImageBitmap from an OffscreenCanvas with no context', 'InvalidStateError');
        var b = newBitmap(this[OFFW], this[OFFH]);
        if (HAVE) N.copy(b[BMID], this[OFFID]);
        if (HAVE) N.reset(this[OFFID], this[OFFW], this[OFFH], opaque(c));
        if (this[UNCLEAN]) define(b, UNCLEAN, true);
        return b;
    });
    define(OP, 'convertToBlob', function (opts) {
        var self = this;
        return new Promise(function (resolve, reject) {
            if (self[UNCLEAN]) { reject(domErr("Failed to execute 'convertToBlob': Tainted OffscreenCanvas may not be exported.", 'SecurityError')); return; }
            if (!self[OFFW] || !self[OFFH]) { reject(domErr('The canvas has no pixels.', 'IndexSizeError')); return; }
            if (HAVE && !self[OCTX]) N.reset(self[OFFID], self[OFFW], self[OFFH], false);
            var e = encode(self[OFFID], opts && opts.type, opts && opts.quality);
            if (!e) { reject(domErr('Encoding failed.', 'EncodingError')); return; }
            resolve(new global.Blob([e.buf], { type: e.type }));
        });
    });
    Object.defineProperty(OP, Symbol.toStringTag, { value: 'OffscreenCanvas', configurable: true });
    if (global.EventTarget) Object.setPrototypeOf(OP, global.EventTarget.prototype);

    /* ---- ImageBitmap ---- */
    function ImageBitmap() { illegal(); }
    var IBP = ImageBitmap.prototype;
    function newBitmap(w, h) {
        var b = Object.create(IBP);
        define(b, BMID, HAVE ? N.newId() : 0);
        define(b, '_w', w); define(b, '_h', h);
        return b;
    }
    Object.defineProperty(IBP, 'width', { configurable: true, enumerable: true, get: function () { return this[BMID] === null ? 0 : this._w; } });
    Object.defineProperty(IBP, 'height', { configurable: true, enumerable: true, get: function () { return this[BMID] === null ? 0 : this._h; } });
    /* (transferred: detached, its pixels left to the copy the message took) */
    define(global, '__cyberDetachBitmap', function (b) { if (b instanceof ImageBitmap) define(b, BMID, null); });
    define(IBP, 'close', function close() { if (this[BMID] !== null && HAVE) N.free(this[BMID]); define(this, BMID, null); });
    Object.defineProperty(IBP, Symbol.toStringTag, { value: 'ImageBitmap', configurable: true });
    /* (structured clone: a bitmap as [id, w, h] and back, in any realm of this thread) */
    define(global, '__cyberBitmapWire', function (b) {
        if (!b || b[BMID] === undefined) return null;
        if (b[BMID] === null) throw domErr('An ImageBitmap is detached and could not be cloned.', 'DataCloneError');
        return [b[BMID], b._w, b._h];
    });
    /* (an OffscreenCanvas sent to another realm of this thread: its bitmap's id and size; a placeholder's, its element's) */
    define(global, '__cyberOffscreenWire', function (o) {
        if (!o || o[OFFID] === undefined || typeof o.getContext !== 'function') return null;
        var id = o[OFFID];
        if (id && typeof id === 'object') id = typeof global.__c2dNodeId === 'function' ? global.__c2dNodeId(id) : 0;
        return [id, o.width, o.height];
    });
    define(global, '__cyberOffscreenMake', function (a) {
        var o = Object.create(OP);
        define(o, OFFID, a[0]); define(o, OFFW, a[1]); define(o, OFFH, a[2]);
        return o;
    });
    define(global, '__cyberBitmapMake', function (a) {
        var b = Object.create(IBP);
        define(b, BMID, a[0]); define(b, '_w', a[1]); define(b, '_h', a[2]);
        return b;
    });

    /* (the realm's document, kept: a worker's realm loses its name, and still decodes images) */
    var DOC0 = global.document;
    function loadImage(url) {
        return new Promise(function (resolve, reject) {
            var im = typeof global.Image === 'function' && global.document ? new global.Image() : DOC0.createElement('img');
            im.onload = function () { resolve(im); };
            im.onerror = function () { reject(domErr('The source image could not be decoded.', 'InvalidStateError')); };
            im.src = url;
        });
    }
    function bitmapFrom(srcKey, w, h, crop, opts) {
        var sx = 0, sy = 0, sw = w, sh = h;
        if (crop) { sx = crop[0]; sy = crop[1]; sw = crop[2]; sh = crop[3]; if (sw < 0) { sx += sw; sw = -sw; } if (sh < 0) { sy += sh; sh = -sh; } }
        var ow = sw, oh = sh;
        if (opts && opts.resizeWidth) ow = opts.resizeWidth >>> 0;
        if (opts && opts.resizeHeight) oh = opts.resizeHeight >>> 0;
        if (opts && opts.resizeWidth && !opts.resizeHeight) oh = Math.ceil(sh * ow / sw);
        if (opts && opts.resizeHeight && !opts.resizeWidth) ow = Math.ceil(sw * oh / sh);
        var b = newBitmap(ow, oh);
        if (!HAVE) return b;
        if (ow === sw && oh === sh && !(opts && opts.imageOrientation === 'flipY')) {
            N.copy(b[BMID], srcKey, sx, sy, sw, sh);
            return b;
        }
        /* (scaled or flipped: drawn through a context) */
        var oc = new OffscreenCanvas(ow, oh), c = oc.getContext('2d');
        if (opts && opts.resizeQuality === 'pixelated') c.imageSmoothingEnabled = false;
        if (opts && opts.imageOrientation === 'flipY') { c.translate(0, oh); c.scale(1, -1); }
        var dm = dims(c);
        var fx = ow / sw, fy = oh / sh, pm = mul(c._m, fx, 0, 0, fy, -sx * fx, -sy * fy);
        N.draw(oc[OFFID], dm[0], dm[1], 0, new Float64Array(rectPath(c, 0, 0, ow, oh)), new Float64Array([4].concat(pm, [4, sx, sy, sw, sh, c._s.imageSmoothingEnabled ? 1 : 0])), stateArr(c), srcKey);
        N.copy(b[BMID], oc[OFFID]);
        N.free(oc[OFFID]);
        return b;
    }
    function createImageBitmap(image) {
        var args = arguments;
        return new Promise(function (resolve, reject) {
            try {
                var crop = null, opts = null;
                if (args.length >= 5) {
                    crop = [Number(args[1]), Number(args[2]), Number(args[3]), Number(args[4])];
                    if (!crop[2] || !crop[3]) throw new RangeError('The crop rect width/height is 0.');
                    opts = args[5];
                } else opts = args[1];
                if (opts && ((opts.resizeWidth !== undefined && !opts.resizeWidth) || (opts.resizeHeight !== undefined && !opts.resizeHeight)))
                    throw domErr('The resize width or height is 0.', 'InvalidStateError');
                if (image instanceof ImageData) {
                    var oc = new OffscreenCanvas(image.width, image.height);
                    oc.getContext('2d').putImageData(image, 0, 0);
                    later(bitmapFrom(oc[OFFID], image.width, image.height, crop, opts));
                    return;
                }
                if (global.Blob && image instanceof global.Blob && !global.document && HAVE) {
                    /* (a worker: no image element; the bytes as a data: URL, decoded by the image cache) */
                    image.arrayBuffer().then(function (ab) {
                        var by = new Uint8Array(ab), bin = '';
                        for (var q = 0; q < by.length; q += 0x8000) bin += String.fromCharCode.apply(null, by.subarray(q, q + 0x8000));
                        var du = 'data:' + (image.type || 'application/octet-stream') + ';base64,' + global.btoa(bin);
                        var sz = N.size(du);
                        if (!sz) { reject(domErr('The source image could not be decoded.', 'InvalidStateError')); return; }
                        resolve(bitmapFrom(du, sz[0], sz[1], crop, opts));
                    }, reject);
                    return;
                }
                if (global.Blob && image instanceof global.Blob) {
                    var u = global.URL.createObjectURL(image);
                    loadImage(u).then(function (im) {
                        var info = sourceOf(im, 'createImageBitmap');
                        if (!info) { reject(domErr('The source image could not be decoded.', 'InvalidStateError')); return; }
                        resolve(urlBitmap(info.src, info.w, info.h, crop, opts));
                    }, reject);
                    return;
                }
                if (isImg(image) && !image.complete) {
                    reject(domErr('The image is not fully loaded.', 'InvalidStateError'));
                    return;
                }
                if (isVideo(image) && image.readyState < 2) {
                    reject(domErr('The provided element has not retrieved data.', 'InvalidStateError'));
                    return;
                }
                var info = sourceOf(image, 'createImageBitmap');
                if (!info) { reject(domErr('The source image could not be decoded.', 'InvalidStateError')); return; }
                var bm0 = typeof info.src === 'string' ? urlBitmap(info.src, info.nw || info.w, info.nh || info.h, crop, opts) : bitmapFrom(info.src, info.nw || info.w, info.nh || info.h, crop, opts);
                define(bm0, BSPACE, isVideo(image) ? 'srgb' : srcSpace(image));
                if (sourceTainted(image)) define(bm0, UNCLEAN, true);
                later(bm0);
            } catch (e) { reject(e); }
            /* (the bitmap taken now, the promise resolved in a later task) */
            function later(b) { global.setTimeout(function () { resolve(b); }, 0); }
        });
    }

    expose('CanvasRenderingContext2D', CanvasRenderingContext2D);
    expose('OffscreenCanvasRenderingContext2D', OffscreenCanvasRenderingContext2D);
    expose('OffscreenCanvas', OffscreenCanvas);
    expose('ImageBitmap', ImageBitmap);
    expose('createImageBitmap', createImageBitmap);
    expose('CanvasGradient', CanvasGradient);
    expose('CanvasPattern', CanvasPattern);
    expose('CanvasFilter', CanvasFilter);
    expose('ImageData', ImageData);
    expose('Path2D', Path2D);
    expose('TextMetrics', TextMetrics);
})

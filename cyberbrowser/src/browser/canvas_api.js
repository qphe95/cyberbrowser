/*
 * canvas_api.js - <canvas> and its 2D context (HTML 4.12.5), installed in each
 * realm by platform_api_install (browser/platform_api.c).
 *
 * The context keeps the whole drawing state as the standard defines it
 * (the state stack, the current transform, styles with colors serialized
 * as browsers do, line and text settings, the current path), so scripts
 * that read state back, build paths, measure text or move image data see
 * a coherent canvas.  Nothing is rasterized: drawing operations update no
 * pixels, getImageData returns transparent black, and measureText
 * estimates widths from the font size.
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

    /* ---- colors (CSS Color 4 parsing; HTML's serialization of a canvas
     * color: "#rrggbb" when opaque, else "rgba(r, g, b, a)") ---- */
    var NAMED = {
        black: '000000', white: 'ffffff', red: 'ff0000', green: '008000', blue: '0000ff', yellow: 'ffff00',
        cyan: '00ffff', aqua: '00ffff', magenta: 'ff00ff', fuchsia: 'ff00ff', gray: '808080', grey: '808080',
        silver: 'c0c0c0', maroon: '800000', olive: '808000', lime: '00ff00', navy: '000080', purple: '800080',
        teal: '008080', orange: 'ffa500', pink: 'ffc0cb', brown: 'a52a2a', gold: 'ffd700', indigo: '4b0082',
        violet: 'ee82ee', coral: 'ff7f50', crimson: 'dc143c', darkblue: '00008b', darkgray: 'a9a9a9',
        darkgrey: 'a9a9a9', darkgreen: '006400', darkred: '8b0000', lightgray: 'd3d3d3', lightgrey: 'd3d3d3',
        lightblue: 'add8e6', lightgreen: '90ee90', steelblue: '4682b4', tomato: 'ff6347', salmon: 'fa8072',
        skyblue: '87ceeb', slategray: '708090', slategrey: '708090', whitesmoke: 'f5f5f5', beige: 'f5f5dc',
        khaki: 'f0e68c', orchid: 'da70d6', plum: 'dda0dd', tan: 'd2b48c', turquoise: '40e0d0',
        chocolate: 'd2691e', firebrick: 'b22222', forestgreen: '228b22', goldenrod: 'daa520',
        hotpink: 'ff69b4', lavender: 'e6e6fa', linen: 'faf0e6', midnightblue: '191970', royalblue: '4169e1',
        seagreen: '2e8b57', sienna: 'a0522d', springgreen: '00ff7f', dodgerblue: '1e90ff', darkorange: 'ff8c00',
        dimgray: '696969', dimgrey: '696969', gainsboro: 'dcdcdc', ivory: 'fffff0', mintcream: 'f5fffa',
        rebeccapurple: '663399', transparent: null
    };
    function hex2(n) { n = Math.max(0, Math.min(255, Math.round(n))); return (n < 16 ? '0' : '') + n.toString(16); }
    function channel(s) {
        s = s.trim();
        if (s.slice(-1) === '%') return num(s.slice(0, -1), NaN) * 2.55;
        return num(s, NaN);
    }
    function alpha(s) {
        if (s === undefined) return 1;
        s = s.trim();
        var a = s.slice(-1) === '%' ? num(s.slice(0, -1), NaN) / 100 : num(s, NaN);
        return Math.max(0, Math.min(1, a));
    }
    function hsl(h, s, l) {
        h = ((h % 360) + 360) % 360 / 360; s /= 100; l /= 100;
        function f(n) { var k = (n + h * 12) % 12, a = s * Math.min(l, 1 - l); return l - a * Math.max(-1, Math.min(k - 3, 9 - k, 1)); }
        return [f(0) * 255, f(8) * 255, f(4) * 255];
    }
    function serialize(r, g, b, a) {
        if (a >= 1) return '#' + hex2(r) + hex2(g) + hex2(b);
        var as = String(Math.round(a * 1000) / 1000);
        return 'rgba(' + Math.round(r) + ', ' + Math.round(g) + ', ' + Math.round(b) + ', ' + as + ')';
    }
    /* The serialized color, or null when value is not a color. */
    function parseColor(value) {
        var s = String(value).trim().toLowerCase(), m;
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
            if (p.length < 3) return null;
            var r = channel(p[0]), g = channel(p[1]), b = channel(p[2]), a = alpha(p[3]);
            if (![r, g, b, a].every(isFinite)) return null;
            return serialize(r, g, b, a);
        }
        if ((m = /^hsla?\((.*)\)$/.exec(s))) {
            var q = m[1].split(/\s*[,\/]\s*|\s+/).filter(Boolean);
            if (q.length < 3) return null;
            var c = hsl(num(q[0].replace(/deg$/, ''), 0), num(q[1].replace('%', ''), 0), num(q[2].replace('%', ''), 0));
            return serialize(c[0], c[1], c[2], alpha(q[3]));
        }
        if (s === 'currentcolor') return '#000000';
        return null;
    }

    /* ---- gradients, patterns, ImageData, Path2D ---- */
    function CanvasGradient() { illegal(); }
    define(CanvasGradient.prototype, 'addColorStop', function addColorStop(offset, color) {
        offset = Number(offset);
        if (!(offset >= 0 && offset <= 1))
            throw new (global.DOMException || RangeError)('The provided value is outside [0, 1].', 'IndexSizeError');
        var c = parseColor(color);
        if (c === null) throw new SyntaxError("The value provided ('" + color + "') could not be parsed as a color.");
        this._stops.push([offset, c]);
    });
    function gradient(kind, args) {
        var g = Object.create(CanvasGradient.prototype);
        define(g, '_kind', kind); define(g, '_args', args); define(g, '_stops', []);
        return g;
    }
    function CanvasPattern() { illegal(); }
    define(CanvasPattern.prototype, 'setTransform', function setTransform() {});

    function ImageData(a, b, c) {
        if (!new.target) throw new TypeError("Failed to construct 'ImageData': Please use the 'new' operator");
        var data, w, h;
        if (a instanceof global.Uint8ClampedArray) {
            data = a; w = b >>> 0; h = c === undefined ? data.length / 4 / w : c >>> 0;
            if (!w || data.length !== w * h * 4)
                throw new (global.DOMException || RangeError)('The input data length is not a multiple of (4 * width).', 'IndexSizeError');
        } else {
            w = a >>> 0; h = b >>> 0;
            if (!w || !h) throw new (global.DOMException || RangeError)('The source width/height is zero.', 'IndexSizeError');
            data = new global.Uint8ClampedArray(w * h * 4);
        }
        Object.defineProperty(this, 'width', { value: w, enumerable: true });
        Object.defineProperty(this, 'height', { value: h, enumerable: true });
        Object.defineProperty(this, 'data', { value: data, enumerable: true });
        Object.defineProperty(this, 'colorSpace', { value: 'srgb', enumerable: true });
    }

    function PathData() { this.ops = []; this.bounds = null; }
    PathData.prototype.point = function (x, y) {
        var b = this.bounds;
        if (!b) this.bounds = { x0: x, y0: y, x1: x, y1: y };
        else { if (x < b.x0) b.x0 = x; if (x > b.x1) b.x1 = x; if (y < b.y0) b.y0 = y; if (y > b.y1) b.y1 = y; }
    };
    /* the path-building methods shared by the context and Path2D: points
     * go through the current transform (identity for a Path2D) */
    var PATH_METHODS = {
        moveTo: function (x, y) { pathOf(this).ops.push(['M', +x, +y]); point(this, x, y); },
        lineTo: function (x, y) { pathOf(this).ops.push(['L', +x, +y]); point(this, x, y); },
        closePath: function () { pathOf(this).ops.push(['Z']); },
        bezierCurveTo: function (a, b, c, d, x, y) { pathOf(this).ops.push(['C', +a, +b, +c, +d, +x, +y]); point(this, a, b); point(this, c, d); point(this, x, y); },
        quadraticCurveTo: function (a, b, x, y) { pathOf(this).ops.push(['Q', +a, +b, +x, +y]); point(this, a, b); point(this, x, y); },
        arc: function (x, y, r, s, e, ccw) {
            if (Number(r) < 0) throw new (global.DOMException || RangeError)('The radius provided is negative.', 'IndexSizeError');
            pathOf(this).ops.push(['A', +x, +y, +r, +s, +e, !!ccw]); point(this, x - r, y - r); point(this, x + r, y + r);
        },
        arcTo: function (x1, y1, x2, y2, r) {
            if (Number(r) < 0) throw new (global.DOMException || RangeError)('The radius provided is negative.', 'IndexSizeError');
            pathOf(this).ops.push(['T', +x1, +y1, +x2, +y2, +r]); point(this, x1, y1); point(this, x2, y2);
        },
        ellipse: function (x, y, rx, ry, rot, s, e, ccw) {
            if (Number(rx) < 0 || Number(ry) < 0) throw new (global.DOMException || RangeError)('The radius provided is negative.', 'IndexSizeError');
            pathOf(this).ops.push(['E', +x, +y, +rx, +ry, +rot, +s, +e, !!ccw]); point(this, x - rx, y - ry); point(this, x + rx, y + ry);
        },
        rect: function (x, y, w, h) { pathOf(this).ops.push(['R', +x, +y, +w, +h]); point(this, x, y); point(this, +x + +w, +y + +h); },
        roundRect: function (x, y, w, h) { pathOf(this).ops.push(['R', +x, +y, +w, +h]); point(this, x, y); point(this, +x + +w, +y + +h); }
    };
    var PATH = Symbol('path');
    function pathOf(o) { return o[PATH]; }
    function point(o, x, y) {
        var m = o._m;
        x = num(x, 0); y = num(y, 0);
        if (m) o[PATH].point(m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]);
        else o[PATH].point(x, y);
    }
    function Path2D(src) {
        if (!new.target) throw new TypeError("Failed to construct 'Path2D': Please use the 'new' operator");
        define(this, PATH, new PathData());
        if (src instanceof Path2D) {
            this[PATH].ops = src[PATH].ops.slice();
            this[PATH].bounds = src[PATH].bounds && Object.assign({}, src[PATH].bounds);
        } else if (typeof src === 'string') {
            this[PATH].ops.push(['SVG', src]);
        }
    }
    for (var k in PATH_METHODS) define(Path2D.prototype, k, PATH_METHODS[k]);
    define(Path2D.prototype, 'addPath', function addPath(p) {
        if (!(p instanceof Path2D)) throw new TypeError("parameter 1 is not of type 'Path2D'.");
        var self = this;
        p[PATH].ops.forEach(function (op) { self[PATH].ops.push(op); });
        var b = p[PATH].bounds;
        if (b) { this[PATH].point(b.x0, b.y0); this[PATH].point(b.x1, b.y1); }
    });

    /* ---- the context ---- */
    var DEFAULTS = {
        fillStyle: '#000000', strokeStyle: '#000000', globalAlpha: 1, globalCompositeOperation: 'source-over',
        lineWidth: 1, lineCap: 'butt', lineJoin: 'miter', miterLimit: 10, lineDashOffset: 0,
        shadowOffsetX: 0, shadowOffsetY: 0, shadowBlur: 0, shadowColor: 'rgba(0, 0, 0, 0)',
        font: '10px sans-serif', textAlign: 'start', textBaseline: 'alphabetic', direction: 'inherit',
        imageSmoothingEnabled: true, imageSmoothingQuality: 'low', filter: 'none',
        letterSpacing: '0px', wordSpacing: '0px', fontKerning: 'auto', fontStretch: 'normal',
        fontVariantCaps: 'normal', textRendering: 'auto'
    };
    var ENUMS = {
        globalCompositeOperation: ['source-over', 'source-in', 'source-out', 'source-atop', 'destination-over',
            'destination-in', 'destination-out', 'destination-atop', 'lighter', 'copy', 'xor', 'multiply', 'screen',
            'overlay', 'darken', 'lighten', 'color-dodge', 'color-burn', 'hard-light', 'soft-light', 'difference',
            'exclusion', 'hue', 'saturation', 'color', 'luminosity'],
        lineCap: ['butt', 'round', 'square'], lineJoin: ['round', 'bevel', 'miter'],
        textAlign: ['start', 'end', 'left', 'right', 'center'],
        textBaseline: ['top', 'hanging', 'middle', 'alphabetic', 'ideographic', 'bottom'],
        direction: ['ltr', 'rtl', 'inherit'], imageSmoothingQuality: ['low', 'medium', 'high']
    };
    function CanvasRenderingContext2D() { illegal(); }
    var CP = CanvasRenderingContext2D.prototype;
    function newContext(canvas) {
        var c = Object.create(CP);
        define(c, '_canvas', canvas);
        reset(c);
        return c;
    }
    function reset(c) {
        define(c, '_s', Object.assign({ dash: [] }, DEFAULTS));
        define(c, '_m', [1, 0, 0, 1, 0, 0]);
        define(c, '_stack', []);
        define(c, PATH, new PathData());
    }
    Object.defineProperty(CP, 'canvas', { get: function () { return this._canvas; }, configurable: true, enumerable: true });
    Object.keys(DEFAULTS).forEach(function (name) {
        Object.defineProperty(CP, name, { configurable: true, enumerable: true,
            get: function () { return this._s[name]; },
            set: function (v) {
                var s = this._s;
                if (name === 'fillStyle' || name === 'strokeStyle') {
                    if (v instanceof CanvasGradient || v instanceof CanvasPattern) { s[name] = v; return; }
                    var c = parseColor(v);
                    if (c !== null) s[name] = c;
                } else if (name === 'shadowColor') {
                    var sc = parseColor(v);
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
                } else {
                    s[name] = String(v);
                }
            } });
    });
    function normalizeFont(f) {
        var m = /(?:^|\s)(\d*\.?\d+)(px|pt|em|rem|%)(?:\s*\/\s*[^\s]+)?\s+(.+)$/.exec(f);
        if (!m) return null;
        var size = Number(m[1]), unit = m[2];
        if (unit === 'pt') size = size * 4 / 3;
        else if (unit === 'em' || unit === 'rem') size *= 16;
        else if (unit === '%') size = size * 16 / 100;
        var prefix = f.slice(0, m.index + (m[0].charAt(0) === ' ' ? 1 : 0)).trim();
        return (prefix ? prefix + ' ' : '') + (Math.round(size * 100) / 100) + 'px ' + m[3].trim();
    }
    function fontSize(c) { var m = /(\d*\.?\d+)px/.exec(c._s.font); return m ? Number(m[1]) : 10; }

    define(CP, 'save', function save() {
        this._stack.push({ s: Object.assign({}, this._s, { dash: this._s.dash.slice() }), m: this._m.slice() });
    });
    define(CP, 'restore', function restore() {
        var t = this._stack.pop();
        if (!t) return;
        define(this, '_s', t.s);
        define(this, '_m', t.m);
    });
    define(CP, 'reset', function () { reset(this); });
    define(CP, 'isContextLost', function () { return false; });
    define(CP, 'getContextAttributes', function () {
        return { alpha: true, colorSpace: 'srgb', desynchronized: false, willReadFrequently: false };
    });
    /* transforms: m = [a, b, c, d, e, f] */
    function mul(m, a, b, c, d, e, f) {
        return [m[0] * a + m[2] * b, m[1] * a + m[3] * b, m[0] * c + m[2] * d, m[1] * c + m[3] * d,
                m[0] * e + m[2] * f + m[4], m[1] * e + m[3] * f + m[5]];
    }
    function setM(c, m) { if (m.every(isFinite)) define(c, '_m', m); }
    define(CP, 'transform', function transform(a, b, c, d, e, f) { setM(this, mul(this._m, +a, +b, +c, +d, +e, +f)); });
    define(CP, 'setTransform', function setTransform(a, b, c, d, e, f) {
        if (a !== null && typeof a === 'object') {
            var o = a;
            setM(this, [num(o.a !== undefined ? o.a : o.m11, 1), num(o.b !== undefined ? o.b : o.m12, 0),
                        num(o.c !== undefined ? o.c : o.m21, 0), num(o.d !== undefined ? o.d : o.m22, 1),
                        num(o.e !== undefined ? o.e : o.m41, 0), num(o.f !== undefined ? o.f : o.m42, 0)]);
        } else if (arguments.length === 0) {
            setM(this, [1, 0, 0, 1, 0, 0]);
        } else {
            setM(this, [+a, +b, +c, +d, +e, +f]);
        }
    });
    define(CP, 'resetTransform', function resetTransform() { define(this, '_m', [1, 0, 0, 1, 0, 0]); });
    define(CP, 'getTransform', function getTransform() {
        var m = this._m;
        if (typeof global.DOMMatrix === 'function') {
            try { return new global.DOMMatrix([m[0], m[1], m[2], m[3], m[4], m[5]]); } catch (e) { /* below */ }
        }
        return { a: m[0], b: m[1], c: m[2], d: m[3], e: m[4], f: m[5], is2D: true,
                 isIdentity: m[0] === 1 && m[1] === 0 && m[2] === 0 && m[3] === 1 && m[4] === 0 && m[5] === 0 };
    });
    define(CP, 'translate', function translate(x, y) { setM(this, mul(this._m, 1, 0, 0, 1, +x, +y)); });
    define(CP, 'scale', function scale(x, y) { setM(this, mul(this._m, +x, 0, 0, +y, 0, 0)); });
    define(CP, 'rotate', function rotate(a) { var c = Math.cos(a), s = Math.sin(a); setM(this, mul(this._m, c, s, -s, c, 0, 0)); });
    /* line dashes */
    define(CP, 'setLineDash', function setLineDash(segments) {
        var d = Array.prototype.map.call(segments, Number);
        if (d.some(function (x) { return !isFinite(x) || x < 0; })) return;
        if (d.length % 2) d = d.concat(d);
        this._s.dash = d;
    });
    define(CP, 'getLineDash', function getLineDash() { return this._s.dash.slice(); });
    /* paths */
    define(CP, 'beginPath', function beginPath() { define(this, PATH, new PathData()); });
    for (var pm in PATH_METHODS) define(CP, pm, PATH_METHODS[pm]);
    function inBounds(b, x, y) { return !!b && x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1; }
    define(CP, 'fill', function fill() {});
    define(CP, 'stroke', function stroke() {});
    define(CP, 'clip', function clip() {});
    define(CP, 'isPointInPath', function isPointInPath(a, b, c) {
        if (a instanceof Path2D) return inBounds(a[PATH].bounds, +b, +c);
        return inBounds(this[PATH].bounds, +a, +b);
    });
    define(CP, 'isPointInStroke', function isPointInStroke() { return false; });
    define(CP, 'drawFocusIfNeeded', function () {});
    define(CP, 'scrollPathIntoView', function () {});
    /* rectangles */
    define(CP, 'fillRect', function fillRect() {});
    define(CP, 'strokeRect', function strokeRect() {});
    define(CP, 'clearRect', function clearRect() {});
    /* text */
    define(CP, 'fillText', function fillText() {});
    define(CP, 'strokeText', function strokeText() {});
    function TextMetrics() { illegal(); }
    define(CP, 'measureText', function measureText(text) {
        text = String(text);
        var size = fontSize(this), w = 0;
        for (var i = 0; i < text.length; i++) {
            var ch = text.charCodeAt(i);
            w += ch === 32 ? 0.28 : ch >= 0x2e80 ? 1 : (ch >= 65 && ch <= 90) ? 0.66 :
                 'iljt.,:;!|\''.indexOf(text.charAt(i)) >= 0 ? 0.28 : 0.55;
        }
        w *= size;
        var align = this._s.textAlign, left = align === 'center' ? w / 2 : (align === 'right' || align === 'end') ? w : 0;
        var m = Object.create(TextMetrics.prototype);
        var vals = { width: w, actualBoundingBoxLeft: left, actualBoundingBoxRight: w - left,
            actualBoundingBoxAscent: size * 0.72, actualBoundingBoxDescent: size * 0.2,
            fontBoundingBoxAscent: size * 0.91, fontBoundingBoxDescent: size * 0.21,
            emHeightAscent: size * 0.8, emHeightDescent: size * 0.2,
            hangingBaseline: size * 0.72, alphabeticBaseline: 0, ideographicBaseline: -size * 0.2 };
        for (var key in vals) Object.defineProperty(m, key, { value: vals[key], enumerable: true });
        return m;
    });
    /* images */
    define(CP, 'drawImage', function drawImage(image) {
        if (arguments.length !== 3 && arguments.length !== 5 && arguments.length !== 9)
            throw new TypeError("Failed to execute 'drawImage': Valid arities are: [3, 5, 9].");
        if (image === null || typeof image !== 'object')
            throw new TypeError("Failed to execute 'drawImage': The provided value is not of type '(CanvasImageSource)'.");
    });
    define(CP, 'createImageData', function createImageData(a, b) {
        if (a instanceof ImageData) return new ImageData(a.width, a.height);
        return new ImageData(Math.abs(a | 0) || 1, Math.abs(b | 0) || 1);
    });
    define(CP, 'getImageData', function getImageData(x, y, w, h) {
        w = Math.abs(w | 0); h = Math.abs(h | 0);
        if (!w || !h) throw new (global.DOMException || RangeError)('The source width/height is 0.', 'IndexSizeError');
        return new ImageData(w, h);
    });
    define(CP, 'putImageData', function putImageData(data) {
        if (!(data instanceof ImageData)) throw new TypeError("parameter 1 is not of type 'ImageData'.");
    });
    define(CP, 'createLinearGradient', function (x0, y0, x1, y1) { return gradient('linear', [x0, y0, x1, y1]); });
    define(CP, 'createRadialGradient', function (x0, y0, r0, x1, y1, r1) {
        if (r0 < 0 || r1 < 0) throw new (global.DOMException || RangeError)('The radius provided is negative.', 'IndexSizeError');
        return gradient('radial', [x0, y0, r0, x1, y1, r1]);
    });
    define(CP, 'createConicGradient', function (a, x, y) { return gradient('conic', [a, x, y]); });
    define(CP, 'createPattern', function (image, repetition) {
        if (image === null || typeof image !== 'object') throw new TypeError("parameter 1 is not of type 'CanvasImageSource'.");
        var p = Object.create(CanvasPattern.prototype);
        define(p, '_rep', repetition == null || repetition === '' ? 'repeat' : String(repetition));
        return p;
    });
    Object.defineProperty(CP, Symbol.toStringTag, { value: 'CanvasRenderingContext2D', configurable: true });

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
                var v = parseInt(this.getAttribute(d[0]), 10);
                return v >= 0 ? v : d[1];
            },
            set: function (v) {
                this.setAttribute(d[0], String(v >>> 0));
                /* setting either resets the bitmap and the context's state */
                var c = this[CONTEXT];
                if (c) reset(c);
            } });
    });
    define(HP, 'getContext', function getContext(type) {
        type = String(type);
        if (type !== '2d') return null;
        var c = this[CONTEXT];
        if (!c) { c = newContext(this); define(this, CONTEXT, c); }
        return c;
    });
    define(HP, 'toDataURL', function toDataURL() { return 'data:,'; });
    define(HP, 'toBlob', function toBlob(cb) {
        if (typeof cb !== 'function') throw new TypeError("parameter 1 is not of type 'BlobCallback'.");
        global.setTimeout(function () { cb(null); }, 0);
    });
    define(HP, 'captureStream', function captureStream() { return null; });
    define(HP, 'transferControlToOffscreen', function () {
        throw new (global.DOMException || Error)('Not supported', 'NotSupportedError');
    });

    expose('CanvasRenderingContext2D', CanvasRenderingContext2D);
    expose('CanvasGradient', CanvasGradient);
    expose('CanvasPattern', CanvasPattern);
    expose('ImageData', ImageData);
    expose('Path2D', Path2D);
    expose('TextMetrics', TextMetrics);
})

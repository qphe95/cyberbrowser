/*
 * web_animations.js - Web Animations (Level 1): Element.animate, Animation,
 * KeyframeEffect, DocumentTimeline and document.getAnimations, installed in
 * each realm by platform_api_install.  Called as fn(global, window).
 *
 * The timing model is here.  What an element's effects come to is a list of
 * declarations given to the engine (__cyberAnimStyle, browser/anim_api.c),
 * which the cascade takes after the element's inline style; the value of a
 * property between two keyframes is worked out from their text, number by
 * number (a value with something relative in it is first read back from
 * getComputedStyle).  The CSS animations and transitions the page's sheets
 * start are css_anim.c's.
 */
(function (global, window) {
    'use strict';
    var ELP = global.Element && global.Element.prototype;
    if (!ELP || !global.__cyberAnimStyle) return;
    var setStyle = global.__cyberAnimStyle, wake = global.__cyberAnimWake;
    function hide(o, name, value) { Object.defineProperty(o, name, { value: value, writable: true, configurable: true }); }
    function expose(name, value) { hide(global, name, value); }
    function noop() {}

    /* ---- easing ---- */
    function cubicBezier(x1, y1, x2, y2) {
        function at(a, b, t) { var u = 1 - t; return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t; }
        /* (past the ends: the tangent at the end) */
        var startSlope = x1 > 0 ? y1 / x1 : !y1 && x2 > 0 ? y2 / x2 : 0;
        var endSlope = x2 < 1 ? (y2 - 1) / (x2 - 1) : x2 === 1 && x1 < 1 ? (y1 - 1) / (x1 - 1) : 0;
        return function (x) {
            if (x <= 0) return x * startSlope;
            if (x >= 1) return 1 + (x - 1) * endSlope;
            var lo = 0, hi = 1, t = x;
            for (var i = 0; i < 40; i++) {
                var v = at(x1, x2, t);
                if (Math.abs(v - x) < 1e-7) break;
                if (v < x) lo = t; else hi = t;
                t = (lo + hi) / 2;
            }
            return at(y1, y2, t);
        };
    }
    function stepsEasing(n, pos) {
        return function (x, before) {
            var cur = Math.floor(x * n);
            if (pos === 'start' || pos === 'jump-start' || pos === 'jump-both') cur++;
            if (before && Math.floor(x * n) === x * n) cur--;
            if (x >= 0 && cur < 0) cur = 0;
            var jumps = pos === 'jump-both' ? n + 1 : pos === 'jump-none' ? n - 1 : n;
            if (x <= 1 && cur > jumps) cur = jumps;
            return cur / jumps;
        };
    }
    function linearEasing(arg) {
        var pts = [];
        arg.split(',').forEach(function (part) {
            var nums = part.trim().split(/\s+/), y = parseFloat(nums[0]), xs = nums.slice(1).map(parseFloat);
            if (isNaN(y)) throw new TypeError('Invalid easing');
            if (!xs.length) pts.push({ y: y, x: null });
            xs.forEach(function (x) { pts.push({ y: y, x: x / 100 }); });
        });
        if (pts.length < 2) throw new TypeError('Invalid easing');
        if (pts[0].x === null) pts[0].x = 0;
        if (pts[pts.length - 1].x === null) pts[pts.length - 1].x = 1;
        for (var i = 1; i < pts.length; i++) {
            if (pts[i].x !== null) { if (pts[i].x < pts[i - 1].x) pts[i].x = pts[i - 1].x; continue; }
            var k = i;
            while (pts[k].x === null) k++;
            for (var j = i; j < k; j++) pts[j].x = pts[i - 1].x + (pts[k].x - pts[i - 1].x) * (j - i + 1) / (k - i + 1);
        }
        return function (x) {
            var i = 1;
            while (i < pts.length - 1 && x > pts[i].x) i++;
            var a = pts[i - 1], b = pts[i];
            return b.x === a.x ? b.y : a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x);
        };
    }
    var NAMED = { linear: function (x) { return x; }, ease: cubicBezier(0.25, 0.1, 0.25, 1), 'ease-in': cubicBezier(0.42, 0, 1, 1),
                  'ease-out': cubicBezier(0, 0, 0.58, 1), 'ease-in-out': cubicBezier(0.42, 0, 0.58, 1),
                  'step-start': stepsEasing(1, 'start'), 'step-end': stepsEasing(1, 'end') };
    function parseEasing(text) {
        var s = String(text).trim().toLowerCase(), m;
        if (Object.prototype.hasOwnProperty.call(NAMED, s)) return { text: s, fn: NAMED[s] };
        if ((m = /^cubic-bezier\(([^)]*)\)$/.exec(s))) {
            var v = m[1].split(',').map(Number);
            if (v.length !== 4 || v.some(isNaN) || v[0] < 0 || v[0] > 1 || v[2] < 0 || v[2] > 1) throw new TypeError("'" + text + "' is not a valid easing");
            return { text: 'cubic-bezier(' + v.join(', ') + ')', fn: cubicBezier(v[0], v[1], v[2], v[3]) };
        }
        if ((m = /^steps\(\s*(\d+)\s*(?:,\s*([a-z-]+)\s*)?\)$/.exec(s))) {
            var n = +m[1], pos = m[2] || 'end';
            if (['start', 'end', 'jump-start', 'jump-end', 'jump-both', 'jump-none'].indexOf(pos) < 0 || n < (pos === 'jump-none' ? 2 : 1))
                throw new TypeError("'" + text + "' is not a valid easing");
            return { text: pos === 'end' || pos === 'jump-end' ? 'steps(' + n + ')' : 'steps(' + n + ', ' + pos + ')', fn: stepsEasing(n, pos) };
        }
        if ((m = /^linear\((.*)\)$/.exec(s))) return { text: s, fn: linearEasing(m[1]) };
        throw new TypeError("'" + text + "' is not a valid easing");
    }
    var LINEAR = parseEasing('linear');

    /* ---- interpolating values, from their text ---- */
    var NUM = /calc\(\s*([-+]?[\d.eE+-]*\d)(%|px)\s*([-+])\s*([\d.eE+-]*\d)(%|px)\s*\)|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?(%|[a-zA-Z]+)?/g;
    function tokens(text) {
        var lits = [], nums = [], at = 0, m;
        NUM.lastIndex = 0;
        while ((m = NUM.exec(text))) {
            if (m[1] !== undefined) {
                var c1 = parseFloat(m[1]), c2 = (m[3] === '-' ? -1 : 1) * parseFloat(m[4]);
                lits.push(text.slice(at, m.index));
                nums.push({ v: 0, u: 'lp', pct: (m[2] === '%' ? c1 : 0) + (m[5] === '%' ? c2 : 0), px: (m[2] === 'px' ? c1 : 0) + (m[5] === 'px' ? c2 : 0) });
                at = m.index + m[0].length;
                continue;
            }
            /* (inside a name: translate3d, h1, a color's #123) */
            var c = m.index ? text[m.index - 1] : '';
            if (/[a-zA-Z_#-]/.test(c) || (/[-+]/.test(m[0][0]) && /[\w)]/.test(c))) continue;
            lits.push(text.slice(at, m.index));
            nums.push({ v: parseFloat(m[0]), u: (m[6] || '').toLowerCase() });
            at = m.index + m[0].length;
        }
        lits.push(text.slice(at));
        return { lits: lits, nums: nums };
    }
    function fmt(v) { return String(Math.round(v * 1e5) / 1e5); }
    var NONNEG = /^(background-size|mask-size|border-image-(slice|width|outset)|mask-border-(slice|width|outset)|font-stretch|font-width|tab-size|hyphenate-limit-|width|height|min-|max-|padding|border(-[a-z]+)*-(width|radius)|font-size|line-height|flex-grow|flex-shrink|flex-basis|outline-width|column-width|column-gap|row-gap|gap$|perspective$|border-spacing|shape-margin|letter-spacing-none)/;
    var INTEGER = /^(z-index|order|orphans|widows|column-count|flood-none)$/;
    function finish(prop, v) {
        /* (a gap rule's width snaps as a border's does) */
        if (/^(column|row)-rule-width$/.test(prop)) return v <= 0 ? 0 : v < 1 ? 1 : Math.floor(v + 0.02);
        /* (no size at all is the size it was) */
        if (prop === 'zoom') return v <= 0 ? 1 : v;
        if (prop === 'opacity' || /-opacity$/.test(prop) || prop === 'shape-image-threshold') v = Math.max(0, Math.min(1, v));
        else if (INTEGER.test(prop)) v = Math.round(v);
        else if (NONNEG.test(prop) && v < 0) v = 0;
        return v;
    }
    /* (in a color function the channels are whole numbers and alpha at most 1) */
    function tidyColors(s) {
        return s.replace(/rgba?\(([^()]*)\)/g, function (all, body) {
            var v = body.split(',').map(parseFloat);
            if (v.length < 3 || v.some(isNaN)) return all;
            var rgb = v.slice(0, 3).map(function (c) { return Math.max(0, Math.min(255, Math.round(c + 0.002))); });
            if (v.length === 4 && v[3] < 1) return 'rgba(' + rgb.join(', ') + ', ' + fmt(Math.max(0, v[3])) + ')';
            return 'rgb(' + rgb.join(', ') + ')';
        });
    }
    function combine(prop, a, b, how) {
        /* (a radius by keyword has nothing to interpolate) */
        if (SHAPE_PROP.test(prop) && /(circle|ellipse)\([^()]*(closest|farthest)-/.test(String(a) + String(b))) return null;
        /* (a polygon without rounding has it at nothing) */
        if (SHAPE_PROP.test(prop) && /^\s*polygon\(/i.test(String(a)) && /^\s*polygon\(/i.test(String(b))) {
            var hasR = /polygon\(\s*((?:nonzero|evenodd)\s*,?\s*)?round\b/i;
            var addR = function (s) { return s.replace(/polygon\(\s*((?:nonzero|evenodd)\s*,\s*)?/i, 'polygon($1round 0px, '); };
            if (hasR.test(a) !== hasR.test(b)) { if (hasR.test(a)) b = addR(b); else a = addR(a); }
        }
        var ta = tokens(a), tb = tokens(b);
        if (ta.nums.length !== tb.nums.length || !ta.nums.length) return null;
        for (var i = 0; i < ta.lits.length; i++) if (ta.lits[i].toLowerCase() !== tb.lits[i].toLowerCase()) return null;
        var out = ta.lits[0];
        for (var k = 0; k < ta.nums.length; k++) {
            var x = ta.nums[k], y = tb.nums[k], ux = x.u || (x.v === 0 ? y.u : ''), uy = y.u || (y.v === 0 ? x.u : '');
            /* (lengths and percentages: each part by itself, the percentage first) */
            if ((ux === 'lp' || uy === 'lp' || (ux === '%' && uy === 'px') || (ux === 'px' && uy === '%')) &&
                /^(lp|%|px)$/.test(ux) && /^(lp|%|px)$/.test(uy)) {
                var px0 = x.u === 'lp' ? x.px : ux === 'px' ? x.v : 0, pc0 = x.u === 'lp' ? x.pct : ux === '%' ? x.v : 0;
                var px1 = y.u === 'lp' ? y.px : uy === 'px' ? y.v : 0, pc1 = y.u === 'lp' ? y.pct : uy === '%' ? y.v : 0;
                var pxv = how(px0, px1), pcv = how(pc0, pc1);
                if (!pcv && !pxv) out += '0%';
                else if (!pcv) out += fmt(finish(prop, pxv)) + 'px';
                else if (!pxv) out += fmt(finish(prop, pcv)) + '%';
                else out += 'calc(' + fmt(pcv) + '%' + (pxv < 0 ? ' - ' : ' + ') + fmt(Math.abs(pxv)) + 'px)';
            } else if (ux === uy) {
                out += fmt(finish(prop, how(x.v, y.v))) + ux;
            } else if (/^(%|px|em|rem|vw|vh|ch|ex)$/.test(ux) && /^(%|px|em|rem|vw|vh|ch|ex)$/.test(uy)) {
                /* (px and %: a calc(), the percentage first) */
                var p = how(x.v, 0), q = how(0, y.v);
                var first = uy === '%' ? fmt(q) + uy : fmt(p) + ux, second = uy === '%' ? p : q, unit = uy === '%' ? ux : uy;
                /* (a part that comes to nothing goes, as a calc()'s does in computing it) */
                if (!second) out += first;
                else if (!(uy === '%' ? q : p)) out += fmt(second) + unit;
                else out += 'calc(' + first + (second < 0 ? ' - ' : ' + ') + fmt(Math.abs(second)) + unit + ')';
            } else {
                return null;
            }
            out += ta.lits[k + 1];
        }
        out = out.replace(/\b(circle|ellipse)\(-[\d.]+(px|%)/g, function (all, fn, u) { return fn + '(0' + u; })
                 .replace(/\bellipse\(([^ ]+) -[\d.]+(px|%)/g, function (all, r, u) { return 'ellipse(' + r + ' 0' + u; })
                 .replace(/\bround -[\d.]+(px|%)/g, function (all, u) { return 'round 0' + u; });
        return tidyColors(out);
    }
    /* (a transform's none: the functions of the other, doing nothing) */
    function neutralFunctions(list) {
        return list.replace(/([a-zA-Z0-9]+)\(([^()]*)\)/g, function (all, name, args) {
            var n = name.toLowerCase(), parts = args.split(',');
            var neutral = n.indexOf('scale') === 0 ? '1' : '0';
            if (n === 'matrix') return 'matrix(1, 0, 0, 1, 0, 0)';
            if (n === 'matrix3d') return 'matrix3d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)';
            return name + '(' + parts.map(function (p) {
                var m = /[a-zA-Z%]+\s*$/.exec(p.trim());
                return neutral + (m && neutral === '0' ? m[0] : '');
            }).join(', ') + ')';
        });
    }
    /* ---- transform lists: composing and interpolating (CSS Transforms 1, 2).
     * Every function as its primitive; what doesn't pair up, as matrices.
     * Matrices are column-major, as matrix3d's arguments are. ---- */
    var tfEl = null;
    function tfBox() {
        var w = 0, h = 0;
        if (tfEl) { try { w = tfEl.offsetWidth || 0; h = tfEl.offsetHeight || 0; } catch (e) { /* (not laid out) */ } }
        return { w: w, h: h };
    }
    var DEG = Math.PI / 180;
    function tfAng(t) {
        var v = parseFloat(t), m = /[a-zA-Z]+\s*$/.exec(t), u = m ? m[0].trim().toLowerCase() : 'deg';
        return u === 'rad' ? v / DEG : u === 'grad' ? v * 0.9 : u === 'turn' ? v * 360 : v;
    }
    function tfLen(t) {
        var out = { px: 0, pct: 0 }, re = /([-+]?\s*(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)(%|[a-zA-Z]*)/g, m, s = String(t);
        while ((m = re.exec(s))) {
            var v = parseFloat(m[1].replace(/\s+/g, ''));
            if (m[2] === '%') { out.pct += v; out.has = true; } else out.px += v;
        }
        return out;
    }
    function tfNum(t) { var v = parseFloat(t); return /%\s*$/.test(t) ? v / 100 : v; }
    function tfArgs(text) {
        var out = [], depth = 0, cur = '';
        for (var i = 0; i < text.length; i++) {
            var c = text[i];
            if (c === '(') depth++;
            if (c === ')') depth--;
            if (!depth && (c === ',' || c === ' ')) { if (cur) out.push(cur); cur = ''; } else cur += c;
        }
        if (cur) out.push(cur);
        return out;
    }
    function tfAxis(x, y, z, deg) {
        var l = Math.sqrt(x * x + y * y + z * z);
        if (!l) return { k: 'rotate', a: [0, 0, 1, 0] };
        return { k: 'rotate', a: [x / l, y / l, z / l, deg] };
    }
    function tfParse(text) {
        var out = [], re = /([a-zA-Z][a-zA-Z0-9]*)\s*\(((?:[^()]|\([^()]*\))*)\)/g, m, Z = { px: 0, pct: 0 };
        while ((m = re.exec(text))) {
            var n = m[1].toLowerCase(), a = tfArgs(m[2]);
            switch (n) {
            case 'translate': out.push({ k: 'translate', a: [tfLen(a[0]), a.length > 1 ? tfLen(a[1]) : Z, Z] }); break;
            case 'translatex': out.push({ k: 'translate', a: [tfLen(a[0]), Z, Z] }); break;
            case 'translatey': out.push({ k: 'translate', a: [Z, tfLen(a[0]), Z] }); break;
            case 'translatez': out.push({ k: 'translate', a: [Z, Z, tfLen(a[0])] }); break;
            case 'translate3d': out.push({ k: 'translate', a: [tfLen(a[0]), tfLen(a[1]), tfLen(a[2])] }); break;
            case 'scale': out.push({ k: 'scale', a: [tfNum(a[0]), a.length > 1 ? tfNum(a[1]) : tfNum(a[0]), 1] }); break;
            case 'scalex': out.push({ k: 'scale', a: [tfNum(a[0]), 1, 1] }); break;
            case 'scaley': out.push({ k: 'scale', a: [1, tfNum(a[0]), 1] }); break;
            case 'scalez': out.push({ k: 'scale', a: [1, 1, tfNum(a[0])] }); break;
            case 'scale3d': out.push({ k: 'scale', a: [tfNum(a[0]), tfNum(a[1]), tfNum(a[2])] }); break;
            case 'rotate': case 'rotatez': out.push(tfAxis(0, 0, 1, tfAng(a[0]))); break;
            case 'rotatex': out.push(tfAxis(1, 0, 0, tfAng(a[0]))); break;
            case 'rotatey': out.push(tfAxis(0, 1, 0, tfAng(a[0]))); break;
            case 'rotate3d': out.push(tfAxis(parseFloat(a[0]), parseFloat(a[1]), parseFloat(a[2]), tfAng(a[3]))); break;
            case 'skew': out.push({ k: 'skew', a: [tfAng(a[0]), a.length > 1 ? tfAng(a[1]) : 0] }); break;
            case 'skewx': out.push({ k: 'skew', a: [tfAng(a[0]), 0] }); break;
            case 'skewy': out.push({ k: 'skew', a: [0, tfAng(a[0])] }); break;
            case 'perspective': out.push({ k: 'perspective', a: [a[0] === 'none' ? Infinity : parseFloat(a[0])] }); break;
            case 'matrix': out.push({ k: 'matrix', m: [+a[0], +a[1], 0, 0, +a[2], +a[3], 0, 0, 0, 0, 1, 0, +a[4], +a[5], 0, 1] }); break;
            case 'matrix3d': out.push({ k: 'matrix', m: a.map(Number) }); break;
            default: return null;
            }
        }
        return out;
    }
    function tfIdentity(f) {
        var Z = { px: 0, pct: 0 };
        switch (f.k) {
        case 'translate': return { k: 'translate', a: [Z, Z, Z] };
        case 'scale': return { k: 'scale', a: [1, 1, 1] };
        case 'rotate': return { k: 'rotate', a: [f.a[0], f.a[1], f.a[2], 0] };
        case 'skew': return { k: 'skew', a: [0, 0] };
        case 'perspective': return { k: 'perspective', a: [Infinity] };
        default: return { k: 'matrix', m: M4_ID.slice() };
        }
    }
    var M4_ID = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
    function m4Mul(a, b) {
        var r = new Array(16);
        for (var c = 0; c < 4; c++) for (var w = 0; w < 4; w++) {
            var v = 0;
            for (var k = 0; k < 4; k++) v += a[k * 4 + w] * b[c * 4 + k];
            r[c * 4 + w] = v;
        }
        return r;
    }
    function tfMatrix(f, box) {
        var m = M4_ID.slice(), a = f.a, r;
        switch (f.k) {
        case 'translate':
            m[12] = a[0].px + a[0].pct * box.w / 100;
            m[13] = a[1].px + a[1].pct * box.h / 100;
            m[14] = a[2].px;
            return m;
        case 'scale': m[0] = a[0]; m[5] = a[1]; m[10] = a[2]; return m;
        case 'rotate':
            r = a[3] * DEG;
            var c = Math.cos(r), s = Math.sin(r), t = 1 - c, x = a[0], y = a[1], z = a[2];
            m[0] = t * x * x + c; m[1] = t * x * y + s * z; m[2] = t * x * z - s * y;
            m[4] = t * x * y - s * z; m[5] = t * y * y + c; m[6] = t * y * z + s * x;
            m[8] = t * x * z + s * y; m[9] = t * y * z - s * x; m[10] = t * z * z + c;
            return m;
        case 'skew': m[4] = Math.tan(a[0] * DEG); m[1] = Math.tan(a[1] * DEG); return m;
        case 'perspective': if (isFinite(a[0])) m[11] = -1 / Math.max(a[0], 1); return m;
        default: return f.m;
        }
    }
    function tfListMatrix(list, box) {
        var m = M4_ID.slice();
        for (var i = 0; i < list.length; i++) m = m4Mul(m, tfMatrix(list[i], box));
        return m;
    }
    function fine(v) { return String(Math.round(v * 1e8) / 1e8); }
    function tfText(f, box) {
        var a = f.a;
        switch (f.k) {
        case 'translate':
            return 'translate3d(' + fine(a[0].px + a[0].pct * box.w / 100) + 'px, ' + fine(a[1].px + a[1].pct * box.h / 100) + 'px, ' + fine(a[2].px) + 'px)';
        case 'scale': return 'scale3d(' + fine(a[0]) + ', ' + fine(a[1]) + ', ' + fine(a[2]) + ')';
        case 'rotate': return 'rotate3d(' + fine(a[0]) + ', ' + fine(a[1]) + ', ' + fine(a[2]) + ', ' + fine(a[3]) + 'deg)';
        case 'skew': return 'skew(' + fine(a[0]) + 'deg, ' + fine(a[1]) + 'deg)';
        case 'perspective': return isFinite(a[0]) ? 'perspective(' + fine(a[0]) + 'px)' : 'perspective(none)';
        default: return 'matrix3d(' + f.m.map(fine).join(', ') + ')';
        }
    }
    function is2D(m) {
        return m[2] === 0 && m[3] === 0 && m[6] === 0 && m[7] === 0 && m[8] === 0 && m[9] === 0 && m[10] === 1 && m[11] === 0 && m[14] === 0 && m[15] === 1;
    }
    /* (CSS Transforms 1: decomposing a 2D matrix) */
    function dec2(m) {
        var r0x = m[0], r0y = m[1], r1x = m[4], r1y = m[5];
        var s0 = Math.sqrt(r0x * r0x + r0y * r0y), s1 = Math.sqrt(r1x * r1x + r1y * r1y);
        if (r0x * r1y - r0y * r1x < 0) { if (r0x < r1y) s0 = -s0; else s1 = -s1; }
        if (s0) { r0x /= s0; r0y /= s0; }
        if (s1) { r1x /= s1; r1y /= s1; }
        var angle = Math.atan2(r0y, r0x), sn = -r0y, cs = r0x, a = r0x, b = r0y, c = r1x, d = r1y;
        if (angle) {
            r0x = cs * a + sn * c; r0y = cs * b + sn * d;
            r1x = -sn * a + cs * c; r1y = -sn * b + cs * d;
        }
        return { t: [m[12], m[13]], s: [s0, s1], ang: angle / DEG, r: [r0x, r0y, r1x, r1y] };
    }
    function rec2(tx, ty, s0, s1, ang, r) {
        var rd = ang * DEG, c = Math.cos(rd), s = Math.sin(rd);
        /* rotation * remainder * scale */
        var a = c * r[0] - s * r[1], b = s * r[0] + c * r[1], d = c * r[2] - s * r[3], e = s * r[2] + c * r[3];
        return [a * s0, b * s0, 0, 0, d * s1, e * s1, 0, 0, 0, 0, 1, 0, tx, ty, 0, 1];
    }
    function lerp2(A, B, t) {
        var a = dec2(A), b = dec2(B);
        if ((a.s[0] < 0 && b.s[1] < 0) || (a.s[1] < 0 && b.s[0] < 0)) {
            a.s[0] = -a.s[0]; a.s[1] = -a.s[1];
            a.ang += a.ang < 0 ? 180 : -180;
        }
        if (!a.ang) a.ang = 360;
        if (!b.ang) b.ang = 360;
        if (Math.abs(a.ang - b.ang) > 180) { if (a.ang > b.ang) a.ang -= 360; else b.ang -= 360; }
        var l = function (x, y) { return x + (y - x) * t; };
        return rec2(l(a.t[0], b.t[0]), l(a.t[1], b.t[1]), l(a.s[0], b.s[0]), l(a.s[1], b.s[1]), l(a.ang, b.ang),
                    [l(a.r[0], b.r[0]), l(a.r[1], b.r[1]), l(a.r[2], b.r[2]), l(a.r[3], b.r[3])]);
    }
    function det3(a, b, c, d, e, f, g, h, i) { return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g); }
    function m4Det(m) {
        var d = 0;
        for (var c = 0; c < 4; c++) {
            var rows = [];
            for (var k = 0; k < 4; k++) if (k !== c) rows.push(k);
            d += (c % 2 ? -1 : 1) * m[c * 4] * det3(m[rows[0] * 4 + 1], m[rows[0] * 4 + 2], m[rows[0] * 4 + 3],
                                                    m[rows[1] * 4 + 1], m[rows[1] * 4 + 2], m[rows[1] * 4 + 3],
                                                    m[rows[2] * 4 + 1], m[rows[2] * 4 + 2], m[rows[2] * 4 + 3]);
        }
        return d;
    }
    function m4Inverse(m) {
        var d = m4Det(m), out = new Array(16);
        for (var c = 0; c < 4; c++) for (var r = 0; r < 4; r++) {
            var cols = [0, 1, 2, 3].filter(function (x) { return x !== c; }), rws = [0, 1, 2, 3].filter(function (x) { return x !== r; });
            var v = det3(m[cols[0] * 4 + rws[0]], m[cols[0] * 4 + rws[1]], m[cols[0] * 4 + rws[2]],
                         m[cols[1] * 4 + rws[0]], m[cols[1] * 4 + rws[1]], m[cols[1] * 4 + rws[2]],
                         m[cols[2] * 4 + rws[0]], m[cols[2] * 4 + rws[1]], m[cols[2] * 4 + rws[2]]);
            out[r * 4 + c] = ((c + r) % 2 ? -v : v) / d;
        }
        return out;
    }
    /* (CSS Transforms 2: decomposing a 3D matrix) */
    function dec3(src) {
        if (!src[15]) return null;
        var m = src.map(function (v) { return v / src[15]; }), pm = m.slice();
        pm[3] = pm[7] = pm[11] = 0; pm[15] = 1;
        if (Math.abs(m4Det(pm)) < 1e-12) return null;
        var persp = [0, 0, 0, 1];
        if (m[3] || m[7] || m[11]) {
            var inv = m4Inverse(pm), rhs = [m[3], m[7], m[11], m[15]];
            /* (the inverse's transpose, on the right hand side) */
            for (var i = 0; i < 4; i++) persp[i] = rhs[0] * inv[i * 4] + rhs[1] * inv[i * 4 + 1] + rhs[2] * inv[i * 4 + 2] + rhs[3] * inv[i * 4 + 3];
        }
        var row = [[m[0], m[1], m[2]], [m[4], m[5], m[6]], [m[8], m[9], m[10]]];
        var len = function (v) { return Math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
        var dot = function (a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        var comb = function (a, b, x, y) { return [a[0] * x + b[0] * y, a[1] * x + b[1] * y, a[2] * x + b[2] * y]; };
        var scale = [0, 0, 0], skew = [0, 0, 0];
        scale[0] = len(row[0]);
        if (scale[0]) row[0] = comb(row[0], row[0], 1 / scale[0], 0);
        skew[0] = dot(row[0], row[1]);
        row[1] = comb(row[1], row[0], 1, -skew[0]);
        scale[1] = len(row[1]);
        if (scale[1]) { row[1] = comb(row[1], row[1], 1 / scale[1], 0); skew[0] /= scale[1]; }
        skew[1] = dot(row[0], row[2]);
        row[2] = comb(row[2], row[0], 1, -skew[1]);
        skew[2] = dot(row[1], row[2]);
        row[2] = comb(row[2], row[1], 1, -skew[2]);
        scale[2] = len(row[2]);
        if (scale[2]) { row[2] = comb(row[2], row[2], 1 / scale[2], 0); skew[1] /= scale[2]; skew[2] /= scale[2]; }
        var cross = [row[1][1] * row[2][2] - row[1][2] * row[2][1], row[1][2] * row[2][0] - row[1][0] * row[2][2], row[1][0] * row[2][1] - row[1][1] * row[2][0]];
        if (dot(row[0], cross) < 0) {
            for (var k = 0; k < 3; k++) { scale[k] = -scale[k]; row[k] = comb(row[k], row[k], -1, 0); }
        }
        var q = [0.5 * Math.sqrt(Math.max(1 + row[0][0] - row[1][1] - row[2][2], 0)),
                 0.5 * Math.sqrt(Math.max(1 - row[0][0] + row[1][1] - row[2][2], 0)),
                 0.5 * Math.sqrt(Math.max(1 - row[0][0] - row[1][1] + row[2][2], 0)),
                 0.5 * Math.sqrt(Math.max(1 + row[0][0] + row[1][1] + row[2][2], 0))];
        if (row[2][1] > row[1][2]) q[0] = -q[0];
        if (row[0][2] > row[2][0]) q[1] = -q[1];
        if (row[1][0] > row[0][1]) q[2] = -q[2];
        return { t: [m[12], m[13], m[14]], s: scale, k: skew, q: q, p: persp };
    }
    function rec3(d) {
        var m = M4_ID.slice(), i;
        for (i = 0; i < 4; i++) m[i * 4 + 3] = d.p[i];
        for (i = 0; i < 3; i++) for (var j = 0; j < 4; j++) m[12 + j] += d.t[i] * m[i * 4 + j];
        var q = d.q, x = q[0], y = q[1], z = q[2], w = q[3];
        var rot = M4_ID.slice();
        rot[0] = 1 - 2 * (y * y + z * z); rot[1] = 2 * (x * y + z * w); rot[2] = 2 * (x * z - y * w);
        rot[4] = 2 * (x * y - z * w); rot[5] = 1 - 2 * (x * x + z * z); rot[6] = 2 * (y * z + x * w);
        rot[8] = 2 * (x * z + y * w); rot[9] = 2 * (y * z - x * w); rot[10] = 1 - 2 * (x * x + y * y);
        m = m4Mul(m, rot);
        var skew = M4_ID.slice();
        if (d.k[2]) { skew = M4_ID.slice(); skew[9] = d.k[2]; m = m4Mul(m, skew); }
        if (d.k[1]) { skew = M4_ID.slice(); skew[8] = d.k[1]; m = m4Mul(m, skew); }
        if (d.k[0]) { skew = M4_ID.slice(); skew[4] = d.k[0]; m = m4Mul(m, skew); }
        for (i = 0; i < 3; i++) for (var c = 0; c < 4; c++) m[i * 4 + c] *= d.s[i];
        return m;
    }
    function slerp(a, b, t) {
        var dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], sign = 1;
        if (dot < 0) { dot = -dot; sign = -1; }
        if (dot >= 1 - 1e-12) return a.slice();
        var th = Math.acos(dot), w = Math.sin(t * th) / Math.sqrt(1 - dot * dot);
        var ka = Math.cos(t * th) - dot * w, kb = sign * w;
        return [0, 1, 2, 3].map(function (i) { return a[i] * ka + b[i] * kb; });
    }
    function tfLerpMatrix(A, B, t) {
        var a = dec3(A), b = dec3(B);
        if (!a || !b) return t < 0.5 ? A : B;
        var l = function (x, y) { return x.map(function (v, i) { return v + (y[i] - v) * t; }); };
        return rec3({ t: l(a.t, b.t), s: l(a.s, b.s), k: l(a.k, b.k), p: l(a.p, b.p), q: slerp(a.q, b.q, t) });
    }
    function tfAccumMatrix(A, B) {
        var a = dec3(A), b = dec3(B);
        if (!a || !b) return B;
        var sum = function (x, y) { return x.map(function (v, i) { return v + y[i]; }); };
        var x = a.q, y = b.q;
        var q = [x[3] * y[0] + x[0] * y[3] + x[1] * y[2] - x[2] * y[1],
                 x[3] * y[1] - x[0] * y[2] + x[1] * y[3] + x[2] * y[0],
                 x[3] * y[2] + x[0] * y[1] - x[1] * y[0] + x[2] * y[3],
                 x[3] * y[3] - x[0] * y[0] - x[1] * y[1] - x[2] * y[2]];
        return rec3({ t: sum(a.t, b.t), k: sum(a.k, b.k), q: q,
                      s: a.s.map(function (v, i) { return v + b.s[i] - 1; }),
                      p: [a.p[0] + b.p[0], a.p[1] + b.p[1], a.p[2] + b.p[2], a.p[3] + b.p[3] - 1] });
    }
    /* A pair of one primitive, combined by how; null when they don't pair. */
    function tfSameAxis(a, b) { return !a.a[3] || !b.a[3] || (a.a[0] === b.a[0] && a.a[1] === b.a[1] && a.a[2] === b.a[2]); }
    function tfPair(a, b, how, box, matrixHow) {
        if (a.k !== b.k) return null;
        switch (a.k) {
        case 'translate':
            return { k: a.k, a: a.a.map(function (v, i) { return { px: how(v.px, b.a[i].px), pct: how(v.pct, b.a[i].pct) }; }) };
        case 'skew':
            return { k: a.k, a: [how(a.a[0], b.a[0]), how(a.a[1], b.a[1])] };
        case 'rotate':
            if (tfSameAxis(a, b)) {
                var ax = a.a[3] ? a : b;
                return { k: a.k, a: [ax.a[0], ax.a[1], ax.a[2], how(a.a[3], b.a[3])] };
            }
            return { k: 'matrix', m: matrixHow(tfMatrix(a, box), tfMatrix(b, box)) };
        default: return undefined;
        }
    }
    function tfInterpolate(ta, tb, t) {
        var la = tfParse(ta === 'none' ? '' : ta), lb = tfParse(tb === 'none' ? '' : tb);
        if (!la || !lb) return null;
        var box = tfBox(), out = [], n = Math.max(la.length, lb.length);
        var l = function (x, y) { return x + (y - x) * t; };
        var ml = function (A, B) { return tfLerpMatrix(A, B, t); };
        for (var i = 0; i < n; i++) {
            var a = i < la.length ? la[i] : tfIdentity(lb[i]), b = i < lb.length ? lb[i] : tfIdentity(la[i]);
            var r = tfPair(a, b, l, box, ml);
            if (r === undefined) {
                if (a.k === 'scale') r = { k: a.k, a: [l(a.a[0], b.a[0]), l(a.a[1], b.a[1]), l(a.a[2], b.a[2])] };
                else if (a.k === 'perspective') {
                    var ia = isFinite(a.a[0]) ? 1 / Math.max(a.a[0], 1) : 0, ib = isFinite(b.a[0]) ? 1 / Math.max(b.a[0], 1) : 0, iv = l(ia, ib);
                    r = { k: a.k, a: [iv > 0 ? 1 / iv : Infinity] };
                } else r = { k: 'matrix', m: ml(a.m, b.m) };
            }
            if (!r) {
                /* (the rest, as matrices) */
                var ra = la.slice(i), rb = lb.slice(i);
                out.push('matrix3d(' + ml(tfListMatrix(ra, box), tfListMatrix(rb, box)).map(fine).join(', ') + ')');
                return out.join(' ');
            }
            out.push(tfText(r, box));
        }
        return out.join(' ') || 'none';
    }
    function tfAccumulate(ta, tb) {
        var la = tfParse(ta === 'none' ? '' : ta), lb = tfParse(tb === 'none' ? '' : tb);
        if (!la || !lb) return tb;
        if (!la.length) return tb;
        if (!lb.length) return ta;
        var box = tfBox(), out = [], paired = la.length === lb.length;
        var sum = function (x, y) { return x + y; };
        for (var i = 0; paired && i < la.length; i++) {
            var a = la[i], b = lb[i];
            var r = tfPair(a, b, sum, box, tfAccumMatrix);
            if (r === undefined) {
                if (a.k === 'scale') r = { k: a.k, a: [a.a[0] + b.a[0] - 1, a.a[1] + b.a[1] - 1, a.a[2] + b.a[2] - 1] };
                else if (a.k === 'perspective') {
                    var inv = (isFinite(a.a[0]) ? 1 / a.a[0] : 0) + (isFinite(b.a[0]) ? 1 / b.a[0] : 0);
                    r = { k: a.k, a: [inv > 0 ? 1 / inv : Infinity] };
                } else r = { k: 'matrix', m: tfAccumMatrix(a.m, b.m) };
            }
            if (!r) paired = false; else out.push(tfText(r, box));
        }
        if (paired) return out.join(' ');
        return 'matrix3d(' + tfAccumMatrix(tfListMatrix(la, box), tfListMatrix(lb, box)).map(fine).join(', ') + ')';
    }
    /* (for DOMMatrix: a transform list without relative lengths, or null) */
    Object.defineProperty(global, '__cyberTransformList', { configurable: true, value: function (text) {
        text = String(text).trim();
        if (text === 'none') return { m: M4_ID.slice(), is2D: true };
        if (!global.CSS.supports('transform', text) || /%|[0-9.](?:[a-z]*em|ex|ch|ic|cap|r?lh|[dsl]?v[whib]|[dsl]?vmin|[dsl]?vmax|c[qQ][a-zA-Z]+)\b/.test(text)) return null;
        var list = tfParse(text);
        if (!list || !list.length) return null;
        var m = tfListMatrix(list, { w: 0, h: 0 }), flat = !/3d|translateZ|rotateX|rotateY|scaleZ|perspective/i.test(text);
        return { m: m, is2D: flat };
    } });
    var CORNER_K = { round: 1, squircle: 2, bevel: 0, scoop: -1 };
    function cornerShape(v) { return v in CORNER_K ? 'superellipse(' + CORNER_K[v] + ')' : v; }
    /* ---- translate, rotate, scale ---- */
    function pad3(list, fill) { return [list[0] || fill[0], list[1] || fill[1], list[2] || fill[2]]; }
    function translateParts(v) { return pad3(v === 'none' ? [] : tfArgs(v), ['0px', '0px', '0px']); }
    function scaleParts(v) {
        var a = v === 'none' ? [] : tfArgs(v).map(tfNum);
        return [a.length ? a[0] : 1, a.length > 1 ? a[1] : a.length ? a[0] : 1, a.length > 2 ? a[2] : 1];
    }
    function rotateParts(v) {
        if (v === 'none') return { x: 0, y: 0, z: 1, deg: 0, text: '' };
        var a = tfArgs(v), angle = 0, axis = [];
        a.forEach(function (t) {
            if (/^(x|y|z)$/i.test(t)) axis = t.toLowerCase() === 'x' ? [1, 0, 0] : t.toLowerCase() === 'y' ? [0, 1, 0] : [0, 0, 1];
            else if (/[a-zA-Z]\s*$/.test(t)) angle = tfAng(t);
            else axis.push(parseFloat(t));
        });
        if (axis.length !== 3) axis = [0, 0, 1];
        var l = Math.sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]) || 1;
        var text = a.filter(function (t) { return !/[a-zA-Z]\s*$/.test(t) || /^(x|y|z)$/i.test(t); }).join(' ');
        return { x: axis[0] / l, y: axis[1] / l, z: axis[2] / l, deg: angle, text: text };
    }
    function individualInterpolate(prop, a, b, t) {
        var out, i;
        if (prop === 'translate') {
            var ta = translateParts(a), tb = translateParts(b);
            out = [];
            for (i = 0; i < 3; i++) {
                var c = combine(prop, ta[i], tb[i], function (x, y) { return x + (y - x) * t; });
                if (c === null) return t < 0.5 ? a : b;
                out.push(c);
            }
            var zero = function (s) { return /^-?0(\.0*)?(px)?$/.test(s); };
            if (zero(out[2])) { out.pop(); if (zero(out[1])) out.pop(); }
            return out.join(' ');
        }
        if (prop === 'scale') {
            var sa = scaleParts(a), sb = scaleParts(b);
            out = [0, 1, 2].map(function (k) { return sa[k] + (sb[k] - sa[k]) * t; });
            if (Math.abs(out[2] - 1) < 1e-9) { out.pop(); if (Math.abs(out[0] - out[1]) < 1e-9) out.pop(); }
            return out.map(fmt).join(' ');
        }
        var ra = rotateParts(a), rb = rotateParts(b);
        var same = !ra.deg || !rb.deg || (Math.abs(ra.x - rb.x) < 1e-6 && Math.abs(ra.y - rb.y) < 1e-6 && Math.abs(ra.z - rb.z) < 1e-6);
        if (same) {
            var ax = ra.deg || a === 'none' && !rb.deg ? ra : rb, deg = ra.deg + (rb.deg - ra.deg) * t;
            if (!ra.deg && a !== 'none') ax = rb;
            /* (the z axis goes without saying; others as unit vectors) */
            var axis = !ax.text || (Math.abs(ax.z - 1) < 1e-9) ? '' : fine(ax.x) + ' ' + fine(ax.y) + ' ' + fine(ax.z) + ' ';
            return axis + fmt(deg) + 'deg';
        }
        var quat = function (r) {
            var h = r.deg * DEG / 2, s = Math.sin(h);
            return [r.x * s, r.y * s, r.z * s, Math.cos(h)];
        };
        var q = slerp(quat(ra), quat(rb), t), w = Math.max(-1, Math.min(1, q[3])), an = 2 * Math.acos(w), sn = Math.sin(an / 2);
        if (Math.abs(sn) < 1e-9) return '0 0 1 0deg';
        return fine(q[0] / sn) + ' ' + fine(q[1] / sn) + ' ' + fine(q[2] / sn) + ' ' + fmt(an / DEG) + 'deg';
    }
    /* ---- lists ---- */
    function splitTop(text) {
        var out = [], d = 0, cur = '';
        for (var i = 0; i < text.length; i++) {
            var c = text[i];
            if (c === '(') d++;
            if (c === ')') d--;
            if (c === ',' && !d) { out.push(cur.trim()); cur = ''; } else cur += c;
        }
        out.push(cur.trim());
        return out;
    }
    function gcd(a, b) { return b ? gcd(b, a % b) : a; }
    var REPEATABLE = /^(background|mask)-(position(-[xy])?|size)$/;
    function repeatableInterpolate(prop, a, b, how) {
        var la = splitTop(a), lb = splitTop(b), n = la.length * lb.length / gcd(la.length, lb.length), out = [];
        for (var i = 0; i < n; i++) {
            var v = combine(prop, la[i % la.length], lb[i % lb.length], how);
            if (v === null) return null;
            out.push(v);
        }
        return out.join(', ');
    }
    var CLEAR_SHADOW = 'rgba(0, 0, 0, 0) 0px 0px 0px 0px';
    function shadowInterpolate(prop, a, b, how) {
        var la = a === 'none' ? [] : splitTop(a), lb = b === 'none' ? [] : splitTop(b), n = Math.max(la.length, lb.length), out = [];
        for (var i = 0; i < n; i++) {
            var x = la[i], y = lb[i];
            var inset = /\binset\b/;
            if (x === undefined) x = CLEAR_SHADOW + (inset.test(y) ? ' inset' : '');
            if (y === undefined) y = CLEAR_SHADOW + (inset.test(x) ? ' inset' : '');
            /* (text-shadow has no spread) */
            /* (rgb() against rgba(): the same count of numbers) */
            var withAlpha = function (s) { return s.replace(/rgb\(([^()]*)\)/, 'rgba($1, 1)'); };
            if (/rgba\(/.test(x) !== /rgba\(/.test(y)) { x = withAlpha(x); y = withAlpha(y); }
            if (prop === 'text-shadow') { x = x.replace(' 0px 0px 0px 0px', ' 0px 0px 0px'); y = y.replace(' 0px 0px 0px 0px', ' 0px 0px 0px'); }
            var v = combine(prop, x, y, how);
            if (v === null) return null;
            /* (a blur is never negative) */
            out.push(v.replace(/^((?:rgba?\([^()]*\)\s+)?-?[\d.e-]+px\s+-?[\d.e-]+px\s+)-[\d.e-]+px/, '$10px'));
        }
        return out.join(', ') || 'none';
    }
    /* filters: function by function, the shorter padded with what does nothing */
    var FILTER_ZERO = { blur: '0px', grayscale: '0', sepia: '0', invert: '0', 'hue-rotate': '0deg',
                        brightness: '1', contrast: '1', saturate: '1', opacity: '1', 'drop-shadow': 'rgba(0, 0, 0, 0) 0px 0px 0px' };
    function filterList(text) {
        var out = [], re = /([a-zA-Z-]+)\s*\(((?:[^()]|\((?:[^()]|\([^()]*\))*\))*)\)/g, m;
        if (text === 'none') return out;
        while ((m = re.exec(text))) {
            var name = m[1].toLowerCase(), arg = m[2].trim();
            if (name !== 'drop-shadow' && name !== 'hue-rotate' && name !== 'blur' && name !== 'url') {
                if (!arg) arg = name in FILTER_ZERO ? (FILTER_ZERO[name] === '0' ? '1' : FILTER_ZERO[name]) : arg;
                else if (/%$/.test(arg)) arg = fmt(parseFloat(arg) / 100);
            } else if (!arg && (name === 'blur' || name === 'hue-rotate')) {
                arg = FILTER_ZERO[name];
            }
            /* (a shadow with no blur: of none, to go against one with) */
            if (name === 'drop-shadow') {
                var lens = arg.match(/-?[\d.]+(?:e[-+]?\d+)?px/g);
                if (lens && lens.length === 2) arg = arg.replace(lens[1], lens[1] + ' 0px');
            }
            out.push({ n: name, a: arg });
        }
        return out;
    }
    function filterInterpolate(prop, a, b, how) {
        if (/url\(/.test(a) || /url\(/.test(b)) return null;
        var la = filterList(a), lb = filterList(b), n = Math.max(la.length, lb.length), out = [];
        for (var i = 0; i < n; i++) {
            var x = la[i], y = lb[i];
            if (x && y && x.n !== y.n) return null;
            var name = (x || y).n;
            var v = combine(prop, x ? x.a : FILTER_ZERO[name], y ? y.a : FILTER_ZERO[name], how);
            if (v === null) return null;
            if (name !== 'hue-rotate' && name !== 'drop-shadow') {
                var fn = parseFloat(v);
                if (fn < 0) v = v.replace(/^-?[\d.]+/, '0');
                else if (fn > 1 && (name === 'sepia' || name === 'grayscale' || name === 'opacity' || name === 'invert')) v = '1';
            }
            out.push(name + '(' + v + ')');
        }
        return out.join(' ') || 'none';
    }
    /* ---- path(): as absolute commands, number by number ---- */
    var PATH_ARGS = { M: 2, L: 2, H: 1, V: 1, C: 6, S: 4, Q: 4, T: 2, A: 7, Z: 0 };
    function pathAbsolute(d) {
        var re = /([a-zA-Z])|([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)/g, m, toks = [];
        while ((m = re.exec(d))) toks.push(m[1] ? m[1] : parseFloat(m[2]));
        var out = [], i = 0, cx = 0, cy = 0, sx = 0, sy = 0;
        while (i < toks.length) {
            var c = toks[i++];
            if (typeof c !== 'string') return null;
            var up = c.toUpperCase(), rel = c !== up, nargs = PATH_ARGS[up];
            if (nargs === undefined) return null;
            if (!nargs) { out.push({ c: 'Z', a: [] }); cx = sx; cy = sy; continue; }
            var first = true;
            do {
                var a = toks.slice(i, i + nargs);
                if (a.length < nargs || a.some(function (x) { return typeof x !== 'number'; })) return null;
                i += nargs;
                var cmd = first ? up : (up === 'M' ? 'L' : up);
                first = false;
                if (rel) {
                    if (cmd === 'H') a[0] += cx;
                    else if (cmd === 'V') a[0] += cy;
                    else if (cmd === 'A') { a[5] += cx; a[6] += cy; }
                    else for (var k = 0; k < a.length; k += 2) { a[k] += cx; a[k + 1] += cy; }
                }
                if (cmd === 'H') cx = a[0];
                else if (cmd === 'V') cy = a[0];
                else { cx = a[a.length - 2]; cy = a[a.length - 1]; }
                if (cmd === 'M') { sx = cx; sy = cy; }
                out.push({ c: cmd, a: a });
            } while (i < toks.length && typeof toks[i] === 'number');
        }
        return out;
    }
    var PATH_FN = /^\s*path\(\s*(?:(nonzero|evenodd)\s*,\s*)?["']([^"']*)["']\s*\)\s*$/i;
    function pathInterpolate(a, b, t) {
        var ma = PATH_FN.exec(a), mb = PATH_FN.exec(b);
        if (!ma || !mb) return null;
        if ((ma[1] || 'nonzero').toLowerCase() !== (mb[1] || 'nonzero').toLowerCase()) return null;
        var pa = pathAbsolute(ma[2]), pb = pathAbsolute(mb[2]);
        if (!pa || !pb || pa.length !== pb.length) return null;
        var parts = [];
        for (var i = 0; i < pa.length; i++) {
            if (pa[i].c !== pb[i].c) return null;
            var nums = pa[i].a.map(function (x, k) {
                var v = x + (pb[i].a[k] - x) * t;
                /* (an arc's flags: set where they aren't both clear) */
                if (pa[i].c === 'A' && (k === 3 || k === 4)) return v !== 0 ? 1 : 0;
                return fmt(v);
            });
            parts.push(pa[i].c + (nums.length ? ' ' + nums.join(' ') : ''));
        }
        return 'path(' + (ma[1] ? ma[1].toLowerCase() + ', ' : '') + '"' + parts.join(' ') + '")';
    }
    /* ---- inset(), rect() and xywh(): as inset() ---- */
    function lpParse(t) {
        var out = { px: 0, pct: 0 };
        var re = /([-+]?\s*(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)(%|px)?/g, m, s = String(t);
        while ((m = re.exec(s))) {
            var v = parseFloat(m[1].replace(/\s+/g, ''));
            if (m[2] === '%') { out.pct += v; out.has = true; } else out.px += v;
        }
        return out;
    }
    /* (a percentage's zero where one was written) */
    function lpText(v) {
        if (!v.pct && !v.px && v.zeroPct) return '0%';
        if (!v.pct) return fmt(v.px) + 'px';
        if (!v.px) return fmt(v.pct) + '%';
        return 'calc(' + fmt(v.pct) + '% ' + (v.px < 0 ? '- ' : '+ ') + fmt(Math.abs(v.px)) + 'px)';
    }
    function from100(v) { return { px: -v.px, pct: 100 - v.pct }; }
    var BOX_AFTER = /\s+(border-box|padding-box|content-box|margin-box|fill-box|stroke-box|view-box)\s*$/;
    function insetParts(text) {
        var m = /^\s*(inset|rect|xywh)\(((?:[^()]|\([^()]*\))*)\)\s*$/i.exec(text);
        if (!m) return null;
        var fn = m[1].toLowerCase(), body = m[2], round = '';
        var ri = body.search(/\bround\b/);
        if (ri >= 0) { round = body.slice(ri).trim(); body = body.slice(0, ri); }
        var a = tfArgs(body.trim());
        var auto = function (x, dflt) { return /^auto$/i.test(x) ? dflt : lpParse(x); };
        var box;
        if (fn === 'inset') {
            var p = a.map(lpParse);
            box = [p[0], p.length > 1 ? p[1] : p[0], p.length > 2 ? p[2] : p[0], p.length > 3 ? p[3] : (p.length > 1 ? p[1] : p[0])];
        } else if (fn === 'rect') {
            var Z = { px: 0, pct: 0, zeroPct: true }, F = { px: 0, pct: 100 };
            box = [auto(a[0], Z), from100(auto(a[1], F)), from100(auto(a[2], F)), auto(a[3], Z)];
        } else {
            var x = lpParse(a[0]), y = lpParse(a[1]), w = lpParse(a[2]), h = lpParse(a[3]);
            box = [y, { px: -x.px - w.px, pct: 100 - x.pct - w.pct }, { px: -y.px - h.px, pct: 100 - y.pct - h.pct }, x];
        }
        return { box: box, round: round };
    }
    function insetInterpolate(a, b, t, prop) {
        var boxA = BOX_AFTER.exec(a), boxB = BOX_AFTER.exec(b);
        if (boxA || boxB) {
            if (!boxA || !boxB || boxA[1] !== boxB[1]) return null;
            var inner = insetInterpolate(a.slice(0, boxA.index), b.slice(0, boxB.index), t, prop);
            return inner === null ? null : inner + ' ' + boxA[1];
        }
        var pa = insetParts(a), pb = insetParts(b);
        if (!pa || !pb) return null;
        var l = function (x, y) { return { px: x.px + (y.px - x.px) * t, pct: x.pct + (y.pct - x.pct) * t }; };
        /* (rect() and xywh() stay what they are, against their own kind) */
        var fa = /^\s*(rect|xywh)\(/i.exec(a), fb = /^\s*(rect|xywh)\(/i.exec(b);
        if (prop !== 'shape-outside' && fa && fb && fa[1].toLowerCase() === fb[1].toLowerCase() && !/\bauto\b/i.test(a + b)) {
            var args = function (s) { return tfArgs(s.slice(s.indexOf('(') + 1, s.lastIndexOf(')')).split(/\bround\b/)[0].trim()); };
            var xa = args(a), xb = args(b);
            var rr = '';
            if (pa.round || pb.round) {
                var r0 = combine('clip-path', pa.round || 'round 0px', pb.round || 'round 0px', function (x, y) { return finish('clip-path', x + (y - x) * t); });
                if (r0 === null) return null;
                if (!/^round -?0(px)?$/.test(r0)) rr = ' ' + r0;
            }
            return fa[1].toLowerCase() + '(' + xa.map(function (x, i) { return lpMixText(lpMix(lpParse(x), lpParse(xb[i]), t)); }).join(' ') + rr + ')';
        }
        var v = pa.box.map(function (x, i) { return lpText(l(x, pb.box[i])); });
        var txt = v[1] === v[0] && v[2] === v[0] && v[3] === v[0] ? v[0] : v.join(' ');
        var rv = '';
        if (pa.round || pb.round) {
            var ra = pa.round || 'round 0px', rb = pb.round || 'round 0px', c = combine('clip-path', ra, rb, function (x, y) { return x + (y - x) * t; });
            if (c === null) return null;
            if (!/^round -?0(px)?$/.test(c)) rv = ' ' + c;
        }
        return 'inset(' + txt + rv + ')';
    }
    /* ---- shape(): a list of commands, each number by number ---- */
    function shapeWords(t) {
        var out = [], d = 0, cur = '';
        for (var i = 0; i < t.length; i++) {
            var c = t[i];
            if (c === '(') d++; else if (c === ')') d--;
            if (!d && /\s/.test(c)) { if (cur) out.push(cur); cur = ''; } else cur += c;
        }
        if (cur) out.push(cur);
        return out;
    }
    function shapeParts(text) {
        var m = /^\s*shape\(([\s\S]*)\)\s*$/i.exec(text);
        if (!m) return null;
        var parts = splitTop(m[1]), out = { rule: '', from: null, cmds: [] };
        var head = shapeWords(parts[0]);
        if (head[0] === 'nonzero' || head[0] === 'evenodd') out.rule = head.shift();
        if (head[0] !== 'from' || head.length !== 3) return null;
        out.from = [lpParse(head[1]), lpParse(head[2])];
        for (var i = 1; i < parts.length; i++) {
            var w = shapeWords(parts[i]), c = { op: w[0], by: w[1] === 'by', pts: [] };
            if (c.op === 'close') { out.cmds.push(c); continue; }
            var k = 2;
            if (c.op === 'hline' || c.op === 'vline') c.pts.push(lpParse(w[k]));
            else if (c.op === 'line') c.pts.push(lpParse(w[k]), lpParse(w[k + 1]));
            else if (c.op === 'curve' || c.op === 'smooth') {
                c.pts.push(lpParse(w[k]), lpParse(w[k + 1]));
                k += 2;
                c.anchors = [];
                c.ctrl = 0;
                while (k < w.length) {
                    if (w[k] === 'with' || w[k] === '/') {
                        c.pts.push(lpParse(w[k + 1]), lpParse(w[k + 2]));
                        c.ctrl++;
                        k += 3;
                        var anchor = c.by ? 'start' : 'origin';
                        if (w[k] === 'from') { anchor = w[k + 1]; k += 2; }
                        c.anchors.push(anchor);
                    } else return null;
                }
            } else if (c.op === 'arc') {
                c.pts.push(lpParse(w[k]), lpParse(w[k + 1]));
                k += 2;
                if (w[k] !== 'of') return null;
                k++;
                c.pts.push(lpParse(w[k]));
                c.one = true;
                k++;
                if (k < w.length && !/^(cw|ccw|large|small|rotate)$/.test(w[k])) { c.pts.push(lpParse(w[k])); c.one = false; k++; }
                else c.pts.push(c.pts[c.pts.length - 1]);
                c.cw = 0; c.large = 0; c.rot = 0;
                while (k < w.length) {
                    if (w[k] === 'cw') c.cw = 1;
                    else if (w[k] === 'large') c.large = 1;
                    else if (w[k] === 'rotate') { k++; c.rot = parseFloat(w[k]); if (/rad$/.test(w[k])) c.rot *= 180 / Math.PI; else if (/turn$/.test(w[k])) c.rot *= 360; else if (/grad$/.test(w[k])) c.rot *= 0.9; }
                    k++;
                }
            } else return null;
            out.cmds.push(c);
        }
        return out;
    }
    /* A path as the commands it comes to, relative ones staying so. */
    function pathAsShape(text) {
        var m = PATH_FN.exec(text);
        if (!m) return null;
        var re = /([a-zA-Z])|([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)/g, g, toks = [];
        while ((g = re.exec(m[2]))) toks.push(g[1] ? g[1] : parseFloat(g[2]));
        var out = { rule: m[1] ? m[1].toLowerCase() : '', from: null, cmds: [] }, i = 0, letter = '';
        var px = function (v) { return { px: v, pct: 0 }; };
        var take = function (n) { var a = toks.slice(i, i + n); i += n; return a.map(px); };
        while (i < toks.length) {
            if (typeof toks[i] === 'string') letter = toks[i++];
            var L = letter.toUpperCase(), by = letter !== L, p;
            if (L === 'Z') { out.cmds.push({ op: 'close', by: false, pts: [] }); continue; }
            if (L === 'M' && !out.from) { p = take(2); out.from = p; letter = by ? 'l' : 'L'; continue; }
            if (L === 'L' || L === 'M') out.cmds.push({ op: 'line', by: by, pts: take(2) });
            else if (L === 'H') out.cmds.push({ op: 'hline', by: by, pts: take(1) });
            else if (L === 'V') out.cmds.push({ op: 'vline', by: by, pts: take(1) });
            else if (L === 'C') { p = take(6); out.cmds.push({ op: 'curve', by: by, ctrl: 2, anchors: by ? ['start', 'start'] : ['origin', 'origin'], pts: [p[4], p[5], p[0], p[1], p[2], p[3]] }); }
            else if (L === 'Q') { p = take(4); out.cmds.push({ op: 'curve', by: by, ctrl: 1, anchors: [by ? 'start' : 'origin'], pts: [p[2], p[3], p[0], p[1]] }); }
            else if (L === 'S') { p = take(4); out.cmds.push({ op: 'smooth', by: by, ctrl: 1, anchors: [by ? 'start' : 'origin'], pts: [p[2], p[3], p[0], p[1]] }); }
            else if (L === 'T') { p = take(2); out.cmds.push({ op: 'smooth', by: by, ctrl: 0, anchors: [], pts: [p[0], p[1]] }); }
            else if (L === 'A') {
                var a = toks.slice(i, i + 7);
                i += 7;
                out.cmds.push({ op: 'arc', by: by, one: false, cw: a[4] ? 1 : 0, large: a[3] ? 1 : 0, rot: a[2], pts: [px(a[5]), px(a[6]), px(a[0]), px(a[1])] });
            } else return null;
        }
        return out.from ? out : null;
    }
    function lpMix(x, y, t) { return { px: x.px + (y.px - x.px) * t, pct: x.pct + (y.pct - x.pct) * t, has: x.has || y.has }; }
    /* (a zero stays a percentage where one was in it) */
    function lpMixText(v) { return !v.pct && !v.px && v.has ? '0%' : lpText(v); }
    function shapeText(s, pts) {
        return 'shape(' + (s.rule && s.rule !== 'nonzero' ? s.rule + ' ' : '') + 'from ' + lpMixText(s.from[0]) + ' ' + lpMixText(s.from[1]) + s.cmds.map(function (c) {
            var o = ', ' + c.op;
            if (c.op === 'close') return o;
            o += c.by ? ' by ' : ' to ';
            var p = c.pts.map(lpMixText);
            if (c.op === 'hline' || c.op === 'vline') return o + p[0];
            o += p[0] + ' ' + p[1];
            if (c.op === 'line') return o;
            if (c.op === 'arc') {
                o += ' of ' + p[2];
                if (!(c.one && p[2] === p[3])) o += ' ' + p[3];
                if (c.cw) o += ' cw';
                if (c.large) o += ' large';
                if (c.rot) o += ' rotate ' + fmt(c.rot) + 'deg';
                return o;
            }
            var dflt = c.by ? 'start' : 'origin';
            for (var q = 0; q < c.ctrl; q++) {
                o += (q ? ' / ' : ' with ') + p[2 + 2 * q] + ' ' + p[3 + 2 * q];
                if (c.anchors[q] !== dflt) o += ' from ' + c.anchors[q];
            }
            return o;
        }).join('') + ')';
    }
    function shapeInterpolate(a, b, t) {
        var isA = /^\s*shape\(/i.test(a), isB = /^\s*shape\(/i.test(b);
        if (!isA && !isB) return null;
        var x = isA ? shapeParts(a) : pathAsShape(a), y = isB ? shapeParts(b) : pathAsShape(b);
        if (!x || !y || (x.rule || 'nonzero') !== (y.rule || 'nonzero') || x.cmds.length !== y.cmds.length) return null;
        var out = { rule: x.rule, from: [lpMix(x.from[0], y.from[0], t), lpMix(x.from[1], y.from[1], t)], cmds: [] };
        for (var i = 0; i < x.cmds.length; i++) {
            var c = x.cmds[i], d = y.cmds[i];
            if (c.op !== d.op || c.by !== d.by || c.pts.length !== d.pts.length) return null;
            if (c.anchors && c.anchors.join() !== d.anchors.join()) return null;
            var m = { op: c.op, by: c.by, pts: c.pts.map(function (p, k) { return lpMix(p, d.pts[k], t); }), anchors: c.anchors, ctrl: c.ctrl };
            if (c.op === 'arc') {
                m.one = c.one && d.one;
                m.cw = c.cw + (d.cw - c.cw) * t !== 0 ? 1 : 0;
                m.large = c.large + (d.large - c.large) * t !== 0 ? 1 : 0;
                m.rot = c.rot + (d.rot - c.rot) * t;
            }
            out.cmds.push(m);
        }
        return shapeText(out);
    }
    /* ---- gap rules: a list, its repeat()s written out; repeat(auto) stays in its place ---- */
    function ruleExpand(text) {
        var items = [], auto = null;
        splitTop(text).forEach(function (w) {
            var m = /^repeat\(\s*(\d+|auto)\s*,([\s\S]*)\)$/i.exec(w.trim());
            if (!m) { items.push(w.trim()); return; }
            var kids = splitTop(m[2]).map(function (x) { return x.trim(); });
            if (m[1].toLowerCase() === 'auto') { auto = { at: items.length, kids: kids }; return; }
            for (var i = 0; i < parseInt(m[1], 10); i++) items.push.apply(items, kids);
        });
        return { items: items, auto: auto };
    }
    function ruleInterpolate(prop, a, b, t) {
        var x = ruleExpand(a), y = ruleExpand(b), one = function (p, q) { return p === q ? p : interpolate(prop, p, q, t); };
        if (!x.auto && !y.auto) {
            if (x.items.length === 1 && y.items.length === 1) return null;
            var n = x.items.length * y.items.length / gcd(x.items.length, y.items.length), out = [];
            for (var i = 0; i < n; i++) out.push(one(x.items[i % x.items.length], y.items[i % y.items.length]));
            return out.join(', ');
        }
        if (!x.auto || !y.auto || x.auto.at !== y.auto.at || x.items.length !== y.items.length) return null;
        var res = x.items.map(function (p, k) { return one(p, y.items[k]); });
        var nk = x.auto.kids.length * y.auto.kids.length / gcd(x.auto.kids.length, y.auto.kids.length), kids = [];
        for (var q = 0; q < nk; q++) kids.push(one(x.auto.kids[q % x.auto.kids.length], y.auto.kids[q % y.auto.kids.length]));
        res.splice(x.auto.at, 0, 'repeat(auto, ' + kids.join(', ') + ')');
        return res.join(', ');
    }
    var RULE_LIST = /^(column|row)-rule-(width|color)$/;
    var SHAPE_PROP = /^(clip-path|shape-outside|offset-path)$/;
    /* border-shape: one or two shapes, each with its box. */
    function borderShapes(v) {
        var out = [], s = String(v).trim(), i = 0;
        while (i < s.length) {
            while (s[i] === ' ') i++;
            var o = s.indexOf('(', i);
            if (o < 0) return null;
            var d = 0, j = o;
            for (; j < s.length; j++) { if (s[j] === '(') d++; else if (s[j] === ')' && !--d) break; }
            var shape = s.slice(i, j + 1), rest = /^\s+([a-z-]+-box)\b/.exec(s.slice(j + 1));
            out.push({ fn: s.slice(i, o).toLowerCase(), shape: shape, box: rest ? rest[1] : '' });
            i = j + 1 + (rest ? rest[0].length : 0);
        }
        return out;
    }
    function borderShapeInterpolate(a, b, t) {
        if (a === 'none' || b === 'none') return null;
        var x = borderShapes(a), y = borderShapes(b);
        if (!x || !y) return null;
        var one = function (l) { return l.length === 1 ? [{ fn: l[0].fn, shape: l[0].shape, box: l[0].box || 'half-border-box' }, { fn: l[0].fn, shape: l[0].shape, box: l[0].box || 'half-border-box' }] : l; };
        if (x.length !== y.length) { x = one(x); y = one(y); }
        var out = [];
        for (var k = 0; k < x.length; k++) {
            if (x[k].fn !== y[k].fn || x[k].box !== y[k].box) return null;
            var m = pathInterpolate(x[k].shape, y[k].shape, t);
            if (m === null) m = shapeInterpolate(x[k].shape, y[k].shape, t);
            if (m === null) m = insetInterpolate(x[k].shape, y[k].shape, t, 'clip-path');
            if (m === null) m = combine('clip-path', x[k].shape, y[k].shape, function (p, q) { return p + (q - p) * t; });
            if (m === null) return null;
            out.push(m + (x[k].box ? ' ' + x[k].box : ''));
        }
        return out.join(' ');
    }
    /* (four numbers that are one, or two, as box values come to) */
    function boxCollapse(v) {
        /* (fill, after the numbers) */
        var fill = /\s+fill$/.test(v);
        if (fill) return boxCollapse(v.replace(/\s+fill$/, '')) + ' fill';
        var p = v.split(' ');
        if (p.length !== 4 || /\//.test(v)) return v;
        if (p[3] !== p[1]) return v;
        if (p[2] !== p[0]) return p.slice(0, 3).join(' ');
        if (p[1] !== p[0]) return p.slice(0, 2).join(' ');
        return p[0];
    }
    /* (a z of nothing, when one has it) */
    function originParts(v) { var a = tfArgs(v); if (a.length < 3) a.push('0px'); return a.join(' '); }
    function originTrim(v) { return v.replace(/ -?0(\.0*)?px$/, ''); }
    function ratioParts(v) {
        var m = /^(auto\s+)?\s*([\d.eE+-]+)(?:\s*\/\s*([\d.eE+-]+))?(\s+auto)?\s*$/.exec(v);
        if (!m) return null;
        return { auto: !!(m[1] || m[4]), w: parseFloat(m[2]), h: m[3] === undefined ? 1 : parseFloat(m[3]) };
    }
    /* (rect() without commas, as with) */
    function clipRect(v) {
        return /^rect\([^,]*\)$/.test(v) ? v.replace(/\s+(?=[^\s)])/g, ', ') : v;
    }
    /* grid-template-rows/columns: a track list, as a tree; null if it isn't one. */
    function trackList(s) {
        var i = 0, out = [];
        function ws() { while (i < s.length && /\s/.test(s[i])) i++; }
        function arg() {
            var d = 0, b = i;
            for (; i < s.length; i++) {
                if (s[i] === '(') d++;
                else if (s[i] === ')') { if (!d) break; d--; }
                else if (s[i] === ',' && !d) break;
            }
            return s.slice(b, i).trim();
        }
        for (ws(); i < s.length; ws()) {
            if (s[i] === '[') {
                var e = s.indexOf(']', i);
                if (e < 0) return null;
                out.push({ k: 'names', v: s.slice(i, e + 1).replace(/\s+/g, ' ') });
                i = e + 1;
                continue;
            }
            var m = /^(repeat|minmax|fit-content)\(/i.exec(s.slice(i));
            if (m) {
                i += m[0].length;
                var a = arg(), rest = [];
                while (s[i] === ',') { i++; rest.push(arg()); }
                if (s[i] !== ')') return null;
                i++;
                var fn = m[1].toLowerCase();
                if (fn === 'repeat') {
                    var inner = trackList(rest[0] || '');
                    if (!inner) return null;
                    out.push({ k: 'repeat', n: a, v: inner });
                } else out.push({ k: fn, v: [a].concat(rest) });
                continue;
            }
            var w = /^[^\s()\[\]]+/.exec(s.slice(i));
            if (!w) return null;
            out.push({ k: 'size', v: w[0] });
            i += w[0].length;
        }
        return out;
    }
    function trackSize(a, b, t) {
        var ma = /^(-?[\d.]+(?:e[-+]?\d+)?)(px|%|fr)$/i.exec(a), mb = /^(-?[\d.]+(?:e[-+]?\d+)?)(px|%|fr)$/i.exec(b);
        if (ma && mb && ma[2] === mb[2]) {
            var v = +ma[1] + (+mb[1] - +ma[1]) * t;
            return +Math.max(0, v).toFixed(6) + ma[2];
        }
        /* (a length and a percentage: both, in a calc()) */
        if (ma && mb && ma[2] !== 'fr' && mb[2] !== 'fr') {
            var px = (ma[2] === 'px' ? +ma[1] * (1 - t) : 0) + (mb[2] === 'px' ? +mb[1] * t : 0);
            var pc = (ma[2] === '%' ? +ma[1] * (1 - t) : 0) + (mb[2] === '%' ? +mb[1] * t : 0);
            return 'calc(' + +px.toFixed(6) + 'px + ' + +pc.toFixed(6) + '%)';
        }
        return t < 0.5 ? a : b;
    }
    function trackMix(x, y, t) {
        var out = [];
        for (var i = 0; i < x.length; i++) {
            var p = x[i], q = y[i];
            if (p.k === 'names') { out.push(t < 0.5 ? p.v : q.v); continue; }
            if (p.k === 'repeat') { out.push('repeat(' + p.n + ', ' + trackMix(p.v, q.v, t) + ')'); continue; }
            if (p.k === 'size' && q.k === 'size') { out.push(trackSize(p.v, q.v, t)); continue; }
            if (p.k === q.k) {
                out.push(p.k + '(' + p.v.map(function (u, j) { return trackSize(u, q.v[j], t); }).join(', ') + ')');
                continue;
            }
            var from = t < 0.5 ? p : q;
            out.push(from.k === 'size' ? from.v : from.k + '(' + from.v.join(', ') + ')');
        }
        return out.join(' ');
    }
    /* Same shape: as many tracks, repeats of the same count.  (Line names and
     * each track's kind may differ.) */
    function trackShape(x, y) {
        var tracks = function (l) { return l.filter(function (e) { return e.k !== 'names'; }); };
        var xt = tracks(x), yt = tracks(y);
        if (xt.length !== yt.length) return false;
        for (var i = 0; i < xt.length; i++) {
            if ((xt[i].k === 'repeat') !== (yt[i].k === 'repeat')) return false;
            if (xt[i].k === 'repeat' && (/auto/i.test(xt[i].n) || xt[i].n !== yt[i].n || !trackShape(xt[i].v, yt[i].v))) return false;
        }
        return true;
    }
    function trackAligned(l) {
        var o = [], pend = null;
        l.forEach(function (e) { if (e.k === 'names') pend = e; else { o.push(pend || { k: 'names', v: '' }); o.push(e); pend = null; } });
        o.push(pend || { k: 'names', v: '' });
        return o;
    }
    function trackInterpolate(a, b, t) {
        if (/\b(none|subgrid|masonry)\b/.test(a + ' ' + b)) return null;
        var x = trackList(a), y = trackList(b);
        if (!x || !y || !trackShape(x, y)) return null;
        /* (names in between tracks line up by position: both written or left out) */
        var align = function (l) {
            var o = [], pend = null;
            l.forEach(function (e) { if (e.k === 'names') pend = e; else { o.push(pend || { k: 'names', v: '' }); o.push(e); pend = null; } });
            o.push(pend || { k: 'names', v: '' });
            return o;
        };
        return trackMix(align(x), align(y), t).replace(/\s+/g, ' ').trim();
    }
    /* Added track by track; what won't add stays as the later one says. */
    function trackSizeAdd(a, b) {
        var ma = /^(-?[\d.]+(?:e[-+]?\d+)?)(px|%|fr)$/i.exec(a), mb = /^(-?[\d.]+(?:e[-+]?\d+)?)(px|%|fr)$/i.exec(b);
        if (ma && mb && ma[2] === mb[2]) return +(+ma[1] + +mb[1]).toFixed(6) + ma[2];
        if (ma && mb && ma[2] !== 'fr' && mb[2] !== 'fr') {
            var px = ma[2] === 'px' ? ma : mb, pc = ma[2] === 'px' ? mb : ma;
            return 'calc(' + px[1] + 'px + ' + pc[1] + '%)';
        }
        return b;
    }
    function trackAddList(x, y) {
        var out = [];
        for (var i = 0; i < x.length; i++) {
            var p = x[i], q = y[i];
            if (p.k === 'names') { out.push(q.v || p.v); continue; }
            if (p.k === 'repeat') { out.push('repeat(' + p.n + ', ' + trackAddList(p.v, q.v) + ')'); continue; }
            if (p.k === 'size' && q.k === 'size') { out.push(trackSizeAdd(p.v, q.v)); continue; }
            if (p.k === q.k) { out.push(p.k + '(' + p.v.map(function (u, j) { return trackSizeAdd(u, q.v[j]); }).join(', ') + ')'); continue; }
            out.push(q.k === 'size' ? q.v : q.k + '(' + q.v.join(', ') + ')');
        }
        return out.join(' ');
    }
    function trackAdd(a, b) {
        if (/\b(none|subgrid|masonry)\b/.test(a + ' ' + b)) return null;
        var x = trackList(a), y = trackList(b);
        if (!x || !y || !trackShape(x, y)) return null;
        return trackAddList(trackAligned(x), trackAligned(y)).replace(/\s+/g, ' ').trim();
    }
    var TRACK_PROP = /^grid-template-(rows|columns)$/;
    /* Colors other than sRGB's old forms go in oklab, premultiplied, and stay
     * oklab() (css-color-4 12.1). */
    var COLOR_FN = /\b(?:rgba?|color|oklab|oklch|lab|lch|hsla?|hwb)\((?:[^()]|\([^()]*\))*\)/g;
    function oklabOf(el, c) {
        if (!global.__cyberComputeLength) return null;
        var got = global.__cyberComputeLength(el, 'color-mix(in oklab, ' + c + ')', 'color');
        var m = got && /^oklab\(([-\d.e]+|none) ([-\d.e]+|none) ([-\d.e]+|none)(?: \/ ([\d.e]+|none))?\)/.exec(got);
        if (!m) return null;
        return { l: parseFloat(m[1]) || 0, a: parseFloat(m[2]) || 0, b: parseFloat(m[3]) || 0, al: m[4] === undefined ? 1 : parseFloat(m[4]) || 0 };
    }
    function wideColorPairs(a, b) {
        var x = String(a).match(COLOR_FN), y = String(b).match(COLOR_FN);
        if (!x || !y || x.length !== y.length) return null;
        var wide = function (c) { return !/^(rgba?|hsla?|hwb)\(/.test(c); };
        return x.some(wide) || y.some(wide) ? { x: x, y: y } : null;
    }
    function colorMixOklab(el, c0, c1, t) {
        var p = oklabOf(el, c0), q = oklabOf(el, c1);
        if (!p || !q) return t < 0.5 ? c0 : c1;
        var al = p.al + (q.al - p.al) * t, A = Math.max(0, Math.min(1, al));
        var ch = function (k) { var v = p[k] * p.al + (q[k] * q.al - p[k] * p.al) * t; return al > 0 ? v / al : 0; };
        var L = Math.max(0, Math.min(1, ch('l')));
        var n = function (v) { return +v.toFixed(6); };
        return 'oklab(' + n(L) + ' ' + n(ch('a')) + ' ' + n(ch('b')) + (A < 1 ? ' / ' + n(A) : '') + ')';
    }
    /* superellipse(s) by where its curve crosses the corner's diagonal (css-borders-4). */
    function superHalf(s) {
        if (s === Infinity) return 1;
        if (s === -Infinity) return 0;
        return s >= 0 ? Math.pow(0.5, Math.pow(2, -s)) : 1 - Math.pow(0.5, Math.pow(2, s));
    }
    function superFrom(h) {
        if (h >= 1) return Infinity;
        if (h <= 0) return -Infinity;
        var up = h >= 0.5, v = -Math.log2(Math.log(up ? h : 1 - h) / Math.log(0.5));
        return up ? v : -v;
    }
    function superParam(v) {
        var m = /superellipse\(\s*(-?infinity|-?[\d.e-]+)\s*\)/i.exec(v);
        if (!m) return null;
        return /infinity/i.test(m[1]) ? (m[1][0] === '-' ? -Infinity : Infinity) : parseFloat(m[1]);
    }
    function rgbaOf(v) {
        var m = /^\s*rgba?\(\s*([-\d.]+),\s*([-\d.]+),\s*([-\d.]+)(?:,\s*([-\d.]+))?\s*\)\s*$/.exec(String(v));
        return m ? [+m[1], +m[2], +m[3], m[4] === undefined ? 1 : +m[4]] : null;
    }
    /* offset-rotate: whether it follows the path, and the angle over that. */
    var IMAGE_PROP = /^(background-image|list-style-image)$/;
    function offsetRotateParts(v) {
        var m = /^(?:(auto|reverse)\s*)?([^\s]*?)(?:\s*(auto|reverse))?$/i.exec(String(v).trim());
        if (!m) return null;
        var kw = (m[1] || m[3] || '').toLowerCase(), deg = 0;
        if (m[2]) {
            var n = /^(-?[\d.]+(?:e[-+]?\d+)?)(deg|grad|rad|turn)$/i.exec(m[2]);
            if (!n) return null;
            var u = n[2].toLowerCase();
            deg = parseFloat(n[1]) * (u === 'deg' ? 1 : u === 'grad' ? 0.9 : u === 'rad' ? 180 / Math.PI : 360);
        }
        return { flag: !!kw, deg: deg + (kw === 'reverse' ? 180 : 0) };
    }
    function interpolate(prop, a, b, t) {
        /* (axis by axis, in the order of their tags; the last of a tag counts) */
        if (prop === 'font-variation-settings') {
            var axes = function (v) {
                var o = {}, m, re = /["']([^"']{4})["']\s+(-?[\d.]+(?:e[-+]?\d+)?)/g;
                while ((m = re.exec(String(v)))) o[m[1]] = +m[2];
                return o;
            };
            var ax = axes(a), bx = axes(b), ka = Object.keys(ax).sort(), kb = Object.keys(bx).sort();
            if (!ka.length || ka.join() !== kb.join()) return t < 0.5 ? a : b;
            return ka.map(function (k) { return '"' + k + '" ' + fmt(+(ax[k] + (bx[k] - ax[k]) * t).toFixed(6)); }).join(', ');
        }
        /* (normal is none at all, but at its own end) */
        if (prop === 'letter-spacing' || prop === 'word-spacing') {
            if (a === 'normal' && t === 0) return a;
            if (b === 'normal' && t === 1) return b;
            if (a === 'normal' && b === 'normal') return a;
            if (a === 'normal') a = '0px';
            if (b === 'normal') b = '0px';
        }
        /* (each of its numbers, whole; auto doesn't go between) */
        if (prop === 'hyphenate-limit-chars') {
            var hp = String(a).trim().split(/\s+/), hq = String(b).trim().split(/\s+/), hr = [];
            for (var hi = 0; hi < 3; hi++) {
                var hx = hp[hi] === undefined ? 'auto' : hp[hi], hy = hq[hi] === undefined ? 'auto' : hq[hi];
                if (hx === 'auto' || hy === 'auto') {
                    if (hx !== hy) return t < 0.5 ? a : b;
                    hr.push('auto');
                } else hr.push(String(Math.round(parseFloat(hx) + (parseFloat(hy) - parseFloat(hx)) * t)));
            }
            while (hr.length > 2 && hr[hr.length - 1] === 'auto') hr.pop();
            if (hr.length === 2 && hr[1] === 'auto' && hr[0] === 'auto') return 'auto';
            return hr.join(' ');
        }
        /* (visible all the way, but for the very end of hidden) */
        if (prop === 'visibility') {
            if (a === b) return a;
            if (a !== 'visible' && b !== 'visible') return t < 0.5 ? a : b;
            var vp = a === 'visible' ? 1 - t : t;
            return vp > 0 ? 'visible' : (a === 'visible' ? b : a);
        }
        /* (counters don't go between) */
        if (/^counter-/.test(prop)) return t < 0.5 ? a : b;
        if (prop === 'border-shape') {
            var bs = borderShapeInterpolate(a, b, t);
            return bs === null ? (t < 0.5 ? a : b) : bs;
        }
        if (prop === 'offset-rotate') {
            var r0 = offsetRotateParts(a), r1 = offsetRotateParts(b);
            if (!r0 || !r1 || r0.flag !== r1.flag) return t < 0.5 ? a : b;
            return (r0.flag ? 'auto ' : '') + fmt(+(r0.deg + (r1.deg - r0.deg) * t).toFixed(4)) + 'deg';
        }
        /* (normal is no slant at all; italic doesn't go between) */
        if (prop === 'font-style') {
            var slant = function (v) {
                v = String(v).trim();
                if (v === 'normal') return 0;
                var m = /^oblique(?:\s+(-?[\d.]+)deg)?$/.exec(v);
                return m ? (m[1] === undefined ? 14 : parseFloat(m[1])) : null;
            };
            var s0 = slant(a), s1 = slant(b);
            if (s0 === null || s1 === null || a === b) return t < 0.5 ? a : b;
            var d = s0 + (s1 - s0) * t;
            d = d < -90 ? -90 : d > 90 ? 90 : d;
            return Math.abs(d) < 1e-4 ? 'normal' : 'oblique ' + fmt(+d.toFixed(4)) + 'deg';
        }
        /* (always the pair, as the reference reads) */
        if (prop === 'border-spacing') {
            var two = function (v) { var p = String(v).trim().split(/\s+/); return p.length === 1 ? p[0] + ' ' + p[0] : v; };
            a = two(a);
            b = two(b);
        }
        if (TRACK_PROP.test(prop)) {
            var tl = lpListMix(a, b, function (x, y) { return x + (y - x) * t; });
            if (tl !== null) return tl;
        }
        /* (a box's values: all four against all four; ex-height goes without saying) */
        if (/^(border-image|mask-border)-(slice|width|outset)$/.test(prop)) {
            var fa = boxFour(a), fb = boxFour(b);
            if (String(a).split(/\s+/).length !== String(b).split(/\s+/).length) {
                var four = combine(prop, fa, fb, function (x, y) { return finish(prop, x + (y - x) * t); });
                if (four !== null) return boxCollapse(four);
            }
        }
        if (prop === 'font-size-adjust') {
            a = String(a).replace(/^ex-height\s+/, '');
            b = String(b).replace(/^ex-height\s+/, '');
        }
        if (RULE_LIST.test(prop) && /,|repeat\(/i.test(String(a) + String(b))) {
            var rl = ruleInterpolate(prop, String(a), String(b), t);
            if (rl !== null) return rl;
        }
        if (a === b) return a;
        /* (one color and another, premultiplied; alpha then kept to what alpha can be) */
        if (/(^|-)color$/.test(prop)) {
            var ca = rgbaOf(a), cb = rgbaOf(b);
            if (ca && cb) {
                var alpha = ca[3] + (cb[3] - ca[3]) * t, kept = alpha > 1 ? 1 : alpha;
                if (kept <= 0) return 'rgba(0, 0, 0, 0)';
                var ch = [0, 1, 2].map(function (i) {
                    var pv = ca[i] * ca[3] + (cb[i] * cb[3] - ca[i] * ca[3]) * t;
                    return Math.max(0, Math.min(255, Math.round(pv / kept + 0.002)));
                });
                return kept >= 1 ? 'rgb(' + ch.join(', ') + ')' : 'rgba(' + ch.join(', ') + ', ' + fmt(+kept.toFixed(6)) + ')';
            }
        }
        if (/color|shadow|background|border|outline|fill|stroke|caret|accent/.test(prop) && tfEl) {
            var pairs = wideColorPairs(a, b);
            if (pairs) {
                /* (everything but the colors as usual, the colors put back after) */
                var ia = 0, ib = 0;
                var ra = String(a).replace(COLOR_FN, function () { return 'black'; });
                var rb = String(b).replace(COLOR_FN, function () { return 'black'; });
                var mixed = pairs.x.map(function (c, i) { return colorMixOklab(tfEl, c, pairs.y[i], t); });
                var rest = ra === rb ? ra : interpolate(prop, ra, rb, t), k = 0;
                return rest.replace(/\bblack\b|rgb\(0, 0, 0\)/g, function () { return mixed[k++] || 'black'; });
            }
        }
        if (TRACK_PROP.test(prop)) {
            var tr = trackInterpolate(a, b, t);
            return tr === null ? (t < 0.5 ? a : b) : tr;
        }
        if (prop === 'clip') { a = clipRect(a); b = clipRect(b); }
        if (prop === 'translate' || prop === 'rotate' || prop === 'scale') return individualInterpolate(prop, a, b, t);
        var lerpHow = function (x, y) { return x + (y - x) * t; };
        if (SHAPE_PROP.test(prop)) {
            var sh = pathInterpolate(a, b, t);
            if (sh === null) sh = shapeInterpolate(a, b, t);
            if (sh === null) sh = insetInterpolate(a, b, t, prop);
            /* (no rounding is no round at all) */
            if (sh === null && /^\s*polygon\(/i.test(a)) {
                var pg = combine(prop, a, b, lerpHow);
                if (pg !== null) sh = pg.replace(/polygon\(((?:nonzero|evenodd),\s*)?round 0(px)?,\s*/i, 'polygon($1');
            }
            if (sh !== null) return sh;
        }
        if (/^(border-image|mask-border)-(slice|width|outset)$/.test(prop)) {
            var bc = combine(prop, a, b, lerpHow);
            return bc === null ? (t < 0.5 ? a : b) : boxCollapse(bc);
        }
        if (prop === 'transform-origin') {
            var ov = combine(prop, originParts(a), originParts(b), lerpHow);
            return ov === null ? (t < 0.5 ? a : b) : originTrim(ov);
        }
        if (prop === 'aspect-ratio') {
            var q0 = ratioParts(a), q1 = ratioParts(b);
            /* (degenerate ratios and auto on one side alone: no interpolating) */
            if (!q0 || !q1 || q0.auto !== q1.auto || !q0.w || !q0.h || !q1.w || !q1.h) return t < 0.5 ? a : b;
            var la0 = Math.log(q0.w / q0.h), la1 = Math.log(q1.w / q1.h);
            return (q0.auto ? 'auto ' : '') + fmt(Math.exp(la0 + (la1 - la0) * t)) + ' / 1';
        }
        if (REPEATABLE.test(prop)) {
            var rv = repeatableInterpolate(prop, a, b, lerpHow);
            return rv === null ? (t < 0.5 ? a : b) : rv;
        }
        if (prop === 'box-shadow' || prop === 'text-shadow') {
            var sv = shadowInterpolate(prop, a, b, lerpHow);
            return sv === null ? (t < 0.5 ? a : b) : sv;
        }
        if (prop === 'filter' || prop === 'backdrop-filter') {
            var fv = filterInterpolate(prop, a, b, lerpHow);
            return fv === null ? (t < 0.5 ? a : b) : fv;
        }
        if (/^corner-.*-shape$/.test(prop)) {
            a = cornerShape(a);
            b = cornerShape(b);
            var s0 = superParam(a), s1 = superParam(b);
            if (s0 !== null && s1 !== null) {
                var hc = superHalf(s0) + (superHalf(s1) - superHalf(s0)) * t;
                var sv1 = superFrom(hc);
                return 'superellipse(' + (sv1 === Infinity ? 'infinity' : sv1 === -Infinity ? '-infinity' : fmt(+sv1.toFixed(6))) + ')';
            }
        }
        if (prop === 'transform') {
            var tv = tfInterpolate(a, b, t);
            if (tv !== null) return tv;
        }
        if (/^(transform|filter|backdrop-filter)$/.test(prop)) {
            if (a === 'none' && b !== 'none') a = neutralFunctions(b);
            else if (b === 'none' && a !== 'none') b = neutralFunctions(a);
        }
        var v = combine(prop, a, b, function (x, y) { return x + (y - x) * t; });
        return v === null ? (t < 0.5 ? a : b) : v;
    }
    /* (a box's one to four values, all four) */
    function boxFour(v) {
        var fill = /\bfill\b/.test(v), p = String(v).replace(/\bfill\b/, '').trim().split(/\s+/);
        if (p.length === 1) p = [p[0], p[0], p[0], p[0]];
        else if (p.length === 2) p = [p[0], p[1], p[0], p[1]];
        else if (p.length === 3) p = [p[0], p[1], p[2], p[1]];
        return p.join(' ') + (fill ? ' fill' : '');
    }
    /* Track lists of plain lengths and percentages, number by number. */
    function lpListMix(a, b, how) {
        var la = shapeWords(String(a)), lb = shapeWords(String(b));
        if (la.length !== lb.length || !la.length) return null;
        var LP = /^(?:-?[\d.]+(?:e-?\d+)?(?:px|%)|calc\(.*\))$/;
        for (var i = 0; i < la.length; i++) if (!LP.test(la[i]) || !LP.test(lb[i])) return null;
        return la.map(function (x, k) {
            var p = lpParse(x), q = lpParse(lb[k]);
            return lpMixText({ px: how(p.px, q.px), pct: how(p.pct, q.pct), has: p.has || q.has });
        }).join(' ');
    }
    /* accumulate on filters: function by function; what starts at 1 counts from it. */
    function filterAccumulate(prop, under, value) {
        var x = filterList(under === 'none' ? '' : under), y = filterList(value === 'none' ? '' : value), out = [];
        var n = Math.max(x.length, y.length);
        for (var i = 0; i < n; i++) {
            var p = x[i], q = y[i];
            var txt = function (f) { return f.n + '(' + f.a + ')'; };
            if (!p || !q) { out.push(txt(p || q)); continue; }
            if (p.n !== q.n) return value;
            var one = /^(brightness|contrast|opacity|saturate)$/.test(p.n);
            var r = combine(prop, txt(p), txt(q), function (u, v) { return one ? u + v - 1 : u + v; });
            if (r === null) return value;
            out.push(r);
        }
        return filterTidy(out.join(' '));
    }
    /* (amounts in their ranges) */
    function filterTidy(s) {
        return String(s).replace(/\b(brightness|contrast|saturate|grayscale|invert|opacity|sepia)\(\s*(-?[\d.e+-]+)\s*\)/g, function (all, fn, v) {
            var n = parseFloat(v);
            if (n < 0) n = 0;
            if (/^(grayscale|invert|opacity|sepia)$/.test(fn) && n > 1) n = 1;
            return fn + '(' + fmt(n) + ')';
        });
    }
    function addValues(prop, under, value, how) {
        if (prop === 'clip') { under = clipRect(under); value = clipRect(value); }
        if (TRACK_PROP.test(prop)) {
            var ts = trackAdd(under, value);
            return ts === null ? value : ts;
        }
        if (prop === 'font-size-adjust') {
            var sa = combine(prop, under, value, function (x, y) { return x + y; });
            if (sa !== null) return sa.replace(/^ex-height\s+/, '');
        }
        if ((prop === 'filter' || prop === 'backdrop-filter') && how === 'accumulate') return filterAccumulate(prop, under, value);
        if (/^(border-image|mask-border)-(slice|width|outset)$/.test(prop)) {
            under = boxFour(under);
            value = boxFour(value);
            var ab = combine(prop, under, value, function (x, y) { return x + y; });
            return boxCollapse(ab === null ? value : ab);
        }
        /* (a corner's two radii, the same: one) */
        if (/^border-.*-radius$/.test(prop)) {
            var rr = combine(prop, under, value, function (x, y) { return x + y; });
            if (rr === null) return value;
            var rp = rr.split(' ');
            return rp.length === 2 && rp[0] === rp[1] ? rp[0] : rr;
        }
        if (prop === 'transform-origin') {
            var oa = combine(prop, originParts(under), originParts(value), function (x, y) { return x + y; });
            return oa === null ? value : originTrim(oa);
        }
        if (prop === 'translate' && under !== 'none' && value !== 'none') {
            var ua = translateParts(under), va = translateParts(value), added = [];
            for (var ti = 0; ti < 3; ti++) added.push(combine(prop, ua[ti], va[ti], function (x, y) { return x + y; }));
            if (added.indexOf(null) < 0) return individualInterpolate(prop, added.join(' '), added.join(' ') + ' ', 0.5);
            return value;
        }
        if (prop === 'translate' || prop === 'rotate' || prop === 'scale') {
            if (under === 'none') return value;
            if (value === 'none') return under;
        }
        if (prop === 'scale') {
            var ss = scaleParts(under), sv = scaleParts(value);
            return individualInterpolate(prop, 'none', ss.map(function (x, k) { return fmt(how === 'add' ? x * sv[k] : x + sv[k] - 1); }).join(' '), 0.9999999);
        }
        if (prop === 'rotate') {
            var ru = rotateParts(under), rv = rotateParts(value);
            if (Math.abs(ru.x - rv.x) < 1e-6 && Math.abs(ru.y - rv.y) < 1e-6 && Math.abs(ru.z - rv.z) < 1e-6) return (rv.text ? rv.text + ' ' : '') + fmt(ru.deg + rv.deg) + 'deg';
            return value;
        }
        if (prop === 'transform' || prop === 'filter' || prop === 'backdrop-filter') {
            if (prop === 'transform' && how === 'accumulate') return tfAccumulate(under, value);
            if (how === 'add' || prop === 'transform') {
                if (under === 'none') return value;
                return value === 'none' ? under : under + ' ' + value;
            }
        }
        if (how === 'add' && (prop === 'box-shadow' || prop === 'text-shadow')) {
            if (under === 'none') return value;
            return value === 'none' ? under : under + ', ' + value;
        }
        var v = combine(prop, under, value, function (x, y) { return x + y; });
        return v === null ? value : v;
    }

    /* ---- what a keyframe's value comes to on an element ---- */
    var COLORISH = /color|background|border|outline|shadow|fill|stroke|caret|accent/;
    var RELATIVE = /\b(inherit|initial|unset|revert|revert-layer|currentcolor)\b|var\(|calc\(|min\(|max\(|clamp\(|[\d.](em|rem|ex|ch|vw|vh|vmin|vmax|lh|rlh|cap|ic|dvh|dvw|svh|svw|lvh|lvw)\b/i;
    function needsResolving(prop, value) {
        if (/^\s*(unset|initial|inherit|revert|revert-layer)\s*$/i.test(value)) return true;
        /* (what weighs by name, or sizes by name) */
        if (prop === 'font-weight' && /[a-z]/i.test(value)) return true;
        if (prop === 'font-size' && /^\s*[a-z-]+\s*$/i.test(value)) return true;
        /* (what a keyword or a left-out color comes to, in the computed form) */
        if (/^(box-shadow|text-shadow|font-stretch|font-width|corner-.*-shape)$/.test(prop) && value !== 'none') return true;
        if (/^(clip-path|shape-outside|offset-path)$/.test(prop) && /\bat\b|closest-|farthest-|shape\(/.test(value)) return true;
        if (/^(column|row)-rule-width$/.test(prop) && /thin|medium|thick/.test(value)) return true;
        if (prop === 'zoom' && !/^\s*[\d.]+\s*$/.test(value)) return true;
        if ((prop === 'filter' || prop === 'backdrop-filter') && /%|\(\s*\)/.test(value)) return true;
        if (/^(min-|max-)?(width|height)$/.test(prop) && /calc-size\(|^\s*(auto|min-content|max-content|fit-content|stretch)\s*$/i.test(value)) return true;
        if (RELATIVE.test(value)) return true;
        if (COLORISH.test(prop) && !/^\s*[-+\d.]/.test(value) && value !== 'none') return true;
        return false;
    }
    /* The computed value of prop with this declaration in force on el. */
    function readWith(el, prop, value) {
        /* (calc-size(): its length, worked out from its basis here) */
        if (value && /calc-size\(/i.test(value) && SIZE_PROP.test(prop)) {
            var sized = calcSizePx(el, /width/.test(prop) ? 'width' : 'height', value);
            setStyle(el, prop + ': ' + value);
            if (sized !== null) return sized;
        }
        setStyle(el, value === null ? null : prop + ': ' + value);
        return global.getComputedStyle(el).getPropertyValue(prop);
    }

    /* ---- CSS animations and transitions of what the engine doesn't animate
     * itself: values for getComputedStyle, from the rules and the same
     * interpolation.  (Layout and paint don't see them.) ---- */
    var keyframesCache = new Map();
    function keyframesOf(doc, name) {
        var found = keyframesCache.get(name);
        if (found) return found;
        var walk = function (rules) {
            for (var i = 0; i < rules.length; i++) {
                var r = rules[i];
                if (r.keyRules || (r.cssRules && r.name === name && r.type === 7)) found = r.name === name ? r : found;
                else if (r.cssRules && !r.selectorText) walk(r.cssRules);
            }
        };
        for (var s = 0; s < doc.styleSheets.length; s++) {
            try { walk(doc.styleSheets[s].cssRules); } catch (e) { /* (cross-origin) */ }
        }
        if (!found) return null;
        var frames = [];
        for (var k = 0; k < found.cssRules.length; k++) {
            var fr = found.cssRules[k];
            String(fr.keyText).split(',').forEach(function (key) {
                key = key.trim().toLowerCase();
                frames.push({ at: key === 'from' ? 0 : key === 'to' ? 1 : parseFloat(key) / 100, style: fr.style });
            });
        }
        frames.sort(function (a, b) { return a.at - b.at; });
        keyframesCache.set(name, frames);
        return frames;
    }
    function computedInContext(el, prop, value, base) {
        if (!needsResolving(prop, value)) return value;
        var v = readWith(el, prop, value);
        setStyle(el, null);
        applyTarget(el);
        return v || value;
    }
    /* (a physical size's logical name, on this box) */
    function sizeLogical(el, prop) {
        var m = /^(min-|max-|)(width|height)$/.exec(prop);
        if (!m) return null;
        var vert = !/^horizontal/.test(global.getComputedStyle(el).writingMode || 'horizontal-tb');
        return m[1] + ((m[2] === 'width') !== vert ? 'inline-size' : 'block-size');
    }
    function cssAnimationsOver(el, prop, base, specBase) {
        var running = global.__cyberAnimRunning(el), v = base;
        for (var i = 0; i + 1 < running.length; i += 2) {
            var frames = keyframesOf(el.ownerDocument, running[i]), p = running[i + 1];
            var comp = listItem(global.getComputedStyle(el).animationComposition || 'replace', i / 2);
            if (!frames) continue;
            var pts = [];
            var alias = sizeLogical(el, prop);
            for (var f = 0; f < frames.length; f++) {
                var val = frames[f].style.getPropertyValue(prop);
                /* (by a logical name: a keyword or calc-size(); a plain length is the engine's) */
                if (!val && alias) {
                    var byLogical = frames[f].style.getPropertyValue(alias);
                    if (byLogical && !/^s*[-+d.]/.test(byLogical)) val = byLogical;
                }
                if (val) pts.push({ at: frames[f].at, value: val, composite: frames[f].style.animationComposition || comp });
            }
            if (!pts.length) continue;
            if (pts[0].at !== 0) pts.unshift({ at: 0, value: null });
            if (pts[pts.length - 1].at !== 1) pts.push({ at: 1, value: null });
            var k = 1;
            while (k < pts.length - 1 && p >= pts[k].at) k++;
            var a = pts[k - 1], b = pts[k], span = b.at - a.at, local = span > 0 ? (p - a.at) / span : 1;
            var under = v;
            /* (a keyword without allow-keywords: it doesn't interpolate) */
            if (SIZE_PROP.test(prop)) {
                if (!sizesInterpolate(el, prop, a.value === null ? specBase : a.value, b.value === null ? specBase : b.value)) { a = local < 0.5 ? a : b; b = a; local = 0; }
            }
            var at = function (pt) {
                if (pt.value === null) return under;
                var u = computedInContext(el, prop, pt.value, under);
                return pt.composite === 'add' || pt.composite === 'accumulate' ? addValues(prop, under, u, pt.composite) : u;
            };
            var atA = at(a), atB = at(b);
            /* (a keyword, between sizes: what it comes to) */
            if (atA !== atB) { atA = sizeResolved(el, prop, atA); atB = sizeResolved(el, prop, atB); }
            v = interpolate(prop, atA, atB, local);
        }
        return v;
    }
    /* calc-size(basis, expression), on a box: its length. */
    function firstComma(s) {
        for (var i = 0, d = 0; i < s.length; i++) {
            if (s[i] === '(') d++;
            else if (s[i] === ')') d--;
            else if (s[i] === ',' && !d) return i;
        }
        return -1;
    }
    function calcSizePx(el, axis, v) {
        var m = /^\s*calc-size\(([\s\S]*)\)\s*$/i.exec(v);
        /* (a plain length needs no layout) */
        var plain = function (t) {
            if (t.indexOf('%') >= 0 || SIZE_KEYWORD.test(t)) return readWith(el, axis, t);
            var got = global.__cyberComputeLength(el, t);
            return got === null ? readWith(el, axis, t) : got;
        };
        if (!m) return plain(v);
        var c = firstComma(m[1]);
        if (c < 0) return null;
        var basis = m[1].slice(0, c).trim(), expr = m[1].slice(c + 1).trim();
        var px = basis.toLowerCase() === 'any' ? '0px' : calcSizePx(el, axis, basis);
        if (px === null) return null;
        return plain('calc(' + expr.replace(/\bsize\b/g, px) + ')');
    }
    var calcMemo = new WeakMap(), calcMemoAt = -1;
    Object.defineProperty(global, '__cyberCalcSize', { configurable: true, value: function (el, prop, spec) {
        var axis = /width/.test(prop) ? 'width' : 'height';
        /* (reading it lays out; a task's pages of them need only once each) */
        var now = global.__cyberAnimNow();
        if (now !== calcMemoAt) { calcMemo = new WeakMap(); calcMemoAt = now; }
        var memo = calcMemo.get(el);
        if (!memo) calcMemo.set(el, memo = {});
        var key = axis + '|' + spec + '|' + (el.getAttribute('style') || '');
        /* (only for a box that is animating: others can change other ways) */
        var moving = global.__cyberAnimRunning(el).length > 0 || parseFloat(global.getComputedStyle(el).transitionDuration) > 0;
        if (moving && key in memo) return memo[key];
        var v = calcSizePx(el, axis, spec);
        /* (while the effects are being sampled, what was being read stays) */
        if (!applying) {
            setStyle(el, null);
            applyTarget(el);
        } else setStyle(el, prop + ': ' + spec);
        memo[key] = v;
        return v;
    } });
    /* What a size's calc-size() builds on: a keyword, 'pct', or 'any'. */
    function sizeBasis(v) {
        v = v.trim();
        var m = /^calc-size\(([\s\S]*)\)$/i.exec(v);
        if (m) {
            var c = firstComma(m[1]);
            if (c < 0) return 'any';
            var b = m[1].slice(0, c).trim();
            if (/^calc-size\(/i.test(b)) return sizeBasis(b);
            if (/^any$/i.test(b)) return 'any';
            if (SIZE_KEYWORD.test(b)) return b.toLowerCase();
            return b.indexOf('%') >= 0 ? 'pct' : 'any';
        }
        return SIZE_KEYWORD.test(v) ? v.toLowerCase() : 'any';
    }
    /* Whether two declared sizes interpolate (css-values-5 calc-size, css-sizing-4
     * interpolate-size); null: what the property starts as. */
    function sizesInterpolate(el, prop, a, b) {
        var dflt = /^max-/.test(prop) ? 'none' : 'auto';
        var named = function (x) { return x === null || x === undefined || /^\s*(initial|unset|revert|revert-layer)\s*$/i.test(x) ? dflt : x; };
        a = named(a);
        b = named(b);
        if (a === 'none' || b === 'none') return false;
        var calc = /calc-size\(/i.test(a) || /calc-size\(/i.test(b);
        var ka = SIZE_KEYWORD.test(a), kb = SIZE_KEYWORD.test(b);
        if (!calc && (ka || kb)) {
            if (ka && kb) return false;
            if (global.getComputedStyle(el).interpolateSize !== 'allow-keywords') return false;
        }
        var ba = sizeBasis(a), bb = sizeBasis(b);
        return ba === bb || ba === 'any' || bb === 'any';
    }
    var SIZE_PROP = /^(min-|max-)?(width|height)$/, SIZE_KEYWORD = /^\s*(auto|min-content|max-content|fit-content|stretch|none)\s*$/i;
    var transitionsOf = new WeakMap();
    function listItem(text, i) {
        var parts = String(text).split(/,(?![^(]*\))/).map(function (x) { return x.trim(); });
        return parts[i % parts.length];
    }
    function seconds(t) {
        var v = parseFloat(t);
        return /ms\s*$/.test(t) ? v : v * 1000;
    }
    /* A size's keyword as a length, what the box would have by it (a minimum's
     * or maximum's too: as width or height have it).  'none' has none. */
    function sizeResolved(el, prop, v) {
        if (!SIZE_PROP.test(prop) || typeof v !== 'string' || !SIZE_KEYWORD.test(v) || /^\s*none\s*$/i.test(v)) return v;
        var got = readWith(el, /width/.test(prop) ? 'width' : 'height', v);
        setStyle(el, null);
        if (!applying) applyTarget(el);
        return got || v;
    }
    /* (the last that covers it: by its name, by a shorthand's, or all) */
    var longhandsMemo = new Map();
    function transitionItem(names, prop, alias) {
        for (var i = names.length - 1; i >= 0; i--) {
            var nm = names[i];
            if (nm === prop || nm === alias) return i;
            if (nm === 'none' || nm === 'all') continue;
            var ls = longhandsMemo.get(nm);
            if (ls === undefined) longhandsMemo.set(nm, ls = global.__cyberCssLonghands(nm));
            if (ls && ls.indexOf(prop) >= 0) return i;
        }
        return names.lastIndexOf('all');
    }
    function cssTransitionOver(el, prop, base, before, specNow, specBefore) {
        var cs = global.getComputedStyle(el), names = String(cs.transitionProperty).split(',').map(function (x) { return x.trim(); });
        var at = transitionItem(names, prop, sizeLogical(el, prop));
        var state = transitionsOf.get(el);
        if (!state) transitionsOf.set(el, state = new Map());
        var rec = state.get(prop);
        var fresh = !rec;
        if (!rec) { state.set(prop, rec = { last: before === undefined ? base : before, lastSpec: specBefore, run: null }); }
        var now = global.__cyberAnimNow();
        var fromSpec = rec.lastSpec;
        if (base === rec.last) rec.lastSpec = specNow;
        /* (what comes down from a parent that is transitioning it doesn't start one) */
        var pe = el.parentElement, up = null, prec = null;
        /* (a parent with transitions of its own, only: reading one lays out and runs its transitions) */
        if (pe && global.__cyberHasAnimSpec(pe) && (base !== rec.last || fresh || transitionsOf.get(pe))) {
            var heldEl = tfEl, pcs = global.getComputedStyle(pe);
            if ((base !== rec.last || fresh) && pcs.transitionDuration !== '0s') pcs.getPropertyValue(prop);
            var pst = transitionsOf.get(pe);
            prec = pst && pst.get(prop);
            /* (it may have got what the parent's at, or where it's going) */
            if (prec && prec.run && !transitionDone(prec.run, now) &&
                (prec.run.to === base || transitionValue(prop, prec.run, now) === base || pcs.getPropertyValue(prop) === base)) up = prec.run;
            tfEl = heldEl;
        }
        if (base !== rec.last) {
            var from = rec.last;
            /* (one running: from where it has got to) */
            if (rec.run) {
                from = transitionValue(prop, rec.run, now);
                transitionCancel(el, prop, rec.run, now);
            }
            rec.last = base;
            rec.lastSpec = specNow;
            rec.run = null;
            if (at >= 0 && !up) {
                var dur = seconds(listItem(cs.transitionDuration, at)), delay = seconds(listItem(cs.transitionDelay, at));
                var discrete = listItem(cs.transitionBehavior || 'normal', at) === 'allow-discrete';
                var id = function (x) { return x; };
                var can = /^(transform|filter|backdrop-filter|translate|rotate|scale|transform-origin|font-palette)$/.test(prop) ||
                          (SHAPE_PROP.test(prop) && (pathInterpolate(from, base, 0) !== null || shapeInterpolate(from, base, 0) !== null || insetInterpolate(from, base, 0, prop) !== null)) ||
                          (RULE_LIST.test(prop) && /repeat\(/i.test(from + base) && ruleInterpolate(prop, from, base, 0) !== null) ||
                          (REPEATABLE.test(prop) ? repeatableInterpolate(prop, from, base, id) !== null :
                           prop === 'box-shadow' || prop === 'text-shadow' ? shadowInterpolate(prop, from, base, id) !== null :
                           prop === 'aspect-ratio' ? (function () { var p0 = ratioParts(from), p1 = ratioParts(base); return !!(p0 && p1 && p0.auto === p1.auto && p0.w && p0.h && p1.w && p1.h); })() :
                           combine(prop, from, base, id) !== null);
                if (SIZE_PROP.test(prop)) can = sizesInterpolate(el, prop, fromSpec, specNow);
                if (TRACK_PROP.test(prop)) can = trackInterpolate(from, base, 0) !== null;
                if (wideColorPairs(from, base)) can = true;
                if (/^corner-.*-shape$/.test(prop)) can = true;
                if (prop === 'offset-rotate') { var rf = offsetRotateParts(from), rt = offsetRotateParts(base); can = !!(rf && rt && rf.flag === rt.flag); }
                if (IMAGE_PROP.test(prop)) can = true;
                if (prop === 'border-shape') can = borderShapeInterpolate(from, base, 0) !== null;
                if (prop === 'font-style') can = true;
                if (prop === 'visibility') can = true;
                if (/(^|-)color$/.test(prop) && rgbaOf(from) && rgbaOf(base)) can = true;
                if (prop === 'filter' || prop === 'backdrop-filter') can = filterInterpolate(prop, from, base, id) !== null;
                if (dur + delay > 0 && (can || discrete)) {
                    var easing;
                    try { easing = parseEasing(listItem(cs.transitionTimingFunction, at)); } catch (e) { easing = LINEAR; }
                    rec.run = { from: can ? sizeResolved(el, prop, from) : from, to: can ? sizeResolved(el, prop, base) : base, start: now, dur: dur, delay: delay, easing: easing, flip: !can };
                    /* (one the engine runs too has its events and its animation there) */
                    if (ENGINE_RUNS.test(prop) && !/%/.test(String(specNow) + String(fromSpec)) && !wideColorPairs(from, base) && !(SIZE_PROP.test(prop) && /calc-size\(|^\s*(auto|none|min-content|max-content|fit-content|stretch)\s*$/i.test(String(specNow) + ' ' === ' ' ? '' : String(specNow)))) rec.run.quiet = true;
                    else transitionEvents(el, prop, rec.run);
                }
            }
        }
        if (up && !rec.run) return prec.run.to === base ? transitionValue(prop, up, now) : base;
        return rec.run ? transitionValue(prop, rec.run, now) : base;
    }
    var ENGINE_RUNS = /^(left|top|right|bottom|opacity|transform|width|height|color|background-color|(padding|margin)-(top|right|bottom|left)|border-(top|right|bottom|left)-width|font-size|letter-spacing|word-spacing|font-weight)$/;
    function transitionDone(run, now) { return now - run.start - run.delay >= run.dur; }
    function transitionFire(el, type, prop, secs) {
        if (el.isConnected) el.dispatchEvent(new global.TransitionEvent(type, { bubbles: true, propertyName: prop, elapsedTime: secs, pseudoElement: '' }));
    }
    function transitionCancel(el, prop, run, now) {
        if (run.over || run.quiet) return;
        run.over = true;
        if (run.endTimer) global.clearTimeout(run.endTimer);
        var t = Math.max(0, now - run.start - Math.max(run.delay, 0)) / 1000;
        global.setTimeout(function () { transitionFire(el, 'transitioncancel', prop, t); }, 0);
    }
    /* (the engine's events for one of those it runs itself; these from timers) */
    function transitionEvents(el, prop, run) {
        var d = run.delay, wait = Math.max(d, 0), late = Math.max(-d, 0) / 1000;
        global.setTimeout(function () {
            if (run.over) return;
            transitionFire(el, 'transitionrun', prop, late);
            if (wait === 0) transitionFire(el, 'transitionstart', prop, late);
        }, 0);
        if (wait > 0) global.setTimeout(function () { if (!run.over) transitionFire(el, 'transitionstart', prop, late); }, wait);
        transitionArm(el, prop, run, global.__cyberAnimNow());
    }
    function transitionArm(el, prop, run, now) {
        if (run.endTimer) global.clearTimeout(run.endTimer);
        run.endTimer = global.setTimeout(function () {
            if (run.over) return;
            run.over = true;
            transitionFire(el, 'transitionend', prop, run.dur / 1000);
            if (run.done) run.done();
        }, Math.max(run.start + run.delay + run.dur - now, 0));
    }
    /* What getAnimations() has for one of them. */
    function transitionAnimation(el, prop, run) {
        if (run.anim) return run.anim;
        var a = Object.create(CSSTransition.prototype);
        var own = function (name, get, set) { Object.defineProperty(a, name, { configurable: true, enumerable: true, get: get, set: set }); };
        var total = function () { return run.delay + run.dur; };
        var val = function (name, v) { Object.defineProperty(a, name, { configurable: true, enumerable: true, writable: true, value: v }); };
        val('id', '');
        val('transitionProperty', prop);
        val('onfinish', null);
        val('oncancel', null);
        val('onremove', null);
        val('finished', new Promise(function (ok) { run.done = function () { ok(a); }; }));
        own('currentTime', function () { return run.over ? total() : global.__cyberAnimNow() - run.start; }, function (t) {
            var now = global.__cyberAnimNow();
            run.start = now - Number(t);
            if (!run.over) transitionArm(el, prop, run, now);
        });
        own('startTime', function () { return run.start; }, function (t) {
            run.start = Number(t);
            if (!run.over) transitionArm(el, prop, run, global.__cyberAnimNow());
        });
        own('playState', function () { return run.over ? 'finished' : 'running'; });
        own('pending', function () { return false; });
        own('playbackRate', function () { return 1; }, function () {});
        own('timeline', function () { return defaultTimeline; });
        own('replaceState', function () { return 'active'; });
        var timing = function () { return { delay: run.delay, endDelay: 0, fill: 'backwards', iterationStart: 0, iterations: 1, duration: run.dur, direction: 'normal', easing: 'ease' }; };
        val('effect', { target: el, pseudoElement: null, getTiming: timing, getKeyframes: function () { return []; },
                        getComputedTiming: function () {
                            var t = timing(), c = a.currentTime;
                            t.endTime = total(); t.activeDuration = run.dur; t.localTime = c;
                            t.progress = run.dur > 0 ? Math.max(0, Math.min(1, (c - run.delay) / run.dur)) : 1;
                            t.currentIteration = 0;
                            return t;
                        } });
        val('cancel', function () { if (!run.over) transitionCancel(el, prop, run, global.__cyberAnimNow()); });
        val('finish', function () { a.currentTime = total(); });
        ['pause', 'play', 'reverse', 'persist', 'commitStyles', 'updatePlaybackRate', 'addEventListener', 'removeEventListener'].forEach(function (n) { val(n, function () {}); });
        run.anim = a;
        return a;
    }
    function transitionValue(prop, run, now) {
        var x = run.dur > 0 ? (now - run.start - run.delay) / run.dur : 1;
        /* (colors in oklab are in it at their ends too) */
        if (x <= 0) return wideColorPairs(run.from, run.to) ? interpolate(prop, run.from, run.to, 0) : run.from;
        if (x >= 1) return run.to;
        if (run.flip) return run.easing.fn(x) < 0.5 ? run.from : run.to;
        return interpolate(prop, run.from, run.to, run.easing.fn(x));
    }
    Object.defineProperty(global, '__cyberGenericAnim', { configurable: true, value: function (el, prop, base, before, keyframesOnly, specNow, specBefore) {
        tfEl = el;
        if (keyframesOnly) {
            /* (only keyframes with calc-size(): the rest the engine has dealt with) */
            var names = global.__cyberAnimRunning(el), some = (global.getComputedStyle(el).animationComposition || 'replace') !== 'replace';
            for (var ni = 0; ni + 1 < names.length && !some; ni += 2) {
                var fs = keyframesOf(el.ownerDocument, names[ni]);
                for (var fi = 0; fs && fi < fs.length && !some; fi++) some = (/color/.test(prop) && /\b(?:color|lab|lch|oklab|oklch|color-mix)\(/.test(fs[fi].style.getPropertyValue(prop))) || !!fs[fi].style.animationComposition || /calc-size\(|^\s*(initial|unset)\s*$/.test(fs[fi].style.getPropertyValue(prop) || '') || SIZE_KEYWORD.test(fs[fi].style.getPropertyValue(prop) || '');
            }
            if (!some) return undefined;
        }
        var v = keyframesOnly ? base : cssTransitionOver(el, prop, base, before, specNow, specBefore);
        v = cssAnimationsOver(el, prop, v, specNow);
        /* (a size part percentage: the length it comes to on this box) */
        if (SIZE_PROP.test(prop) && typeof v === 'string' && v !== base && v.indexOf('%') >= 0 && /^(width|height)$/.test(prop)) {
            var got = readWith(el, prop, v);
            setStyle(el, null);
            if (!applying) applyTarget(el);
            if (got) v = got;
        }
        return v;
    } });

    /* ---- timing (Web Animations 4.5-4.8) ---- */
    function timingOf(input) {
        var t = { delay: 0, endDelay: 0, fill: 'auto', iterationStart: 0, iterations: 1, duration: 'auto', direction: 'normal', easing: 'linear', fn: LINEAR.fn };
        return updateTiming(t, input);
    }
    function finiteNumber(v, what) {
        v = Number(v);
        if (!isFinite(v)) throw new TypeError(what + ' must be finite');
        return v;
    }
    function updateTiming(t, input) {
        if (input === undefined || input === null) return t;
        if (typeof input === 'number') input = { duration: input };
        if (typeof input !== 'object' && typeof input !== 'function') throw new TypeError('Invalid timing');
        var r = Object.assign({}, t);
        if (input.delay !== undefined) r.delay = finiteNumber(input.delay, 'delay');
        if (input.endDelay !== undefined) r.endDelay = finiteNumber(input.endDelay, 'endDelay');
        if (input.fill !== undefined) {
            r.fill = String(input.fill);
            if (['none', 'forwards', 'backwards', 'both', 'auto'].indexOf(r.fill) < 0) throw new TypeError('Invalid fill');
        }
        if (input.iterationStart !== undefined) {
            r.iterationStart = finiteNumber(input.iterationStart, 'iterationStart');
            if (r.iterationStart < 0) throw new TypeError('iterationStart must be non-negative');
        }
        if (input.iterations !== undefined) {
            r.iterations = Number(input.iterations);
            if (isNaN(r.iterations) || r.iterations < 0) throw new TypeError('iterations must be non-negative');
        }
        if (input.duration !== undefined) {
            if (input.duration === 'auto') r.duration = 'auto';
            else if (typeof input.duration === 'string') throw new TypeError('Invalid duration');
            else {
                r.duration = Number(input.duration);
                if (isNaN(r.duration) || r.duration < 0) throw new TypeError('duration must be non-negative');
            }
        }
        if (input.direction !== undefined) {
            r.direction = String(input.direction);
            if (['normal', 'reverse', 'alternate', 'alternate-reverse'].indexOf(r.direction) < 0) throw new TypeError('Invalid direction');
        }
        if (input.easing !== undefined) {
            var e = parseEasing(input.easing);
            r.easing = e.text;
            r.fn = e.fn;
        }
        return r;
    }
    function computedTiming(effect) {
        var t = effect._t, anim = effect._anim;
        var dur = t.duration === 'auto' ? 0 : t.duration;
        var active = t.iterations === 0 || dur === 0 ? 0 : dur * t.iterations;
        var end = Math.max(t.delay + active + t.endDelay, 0);
        var local = anim ? anim.currentTime : null;
        var rate = anim ? anim._rate : 1;
        var c = { delay: t.delay, endDelay: t.endDelay, fill: t.fill, iterationStart: t.iterationStart, iterations: t.iterations,
                  duration: t.duration === 'auto' ? 0 : t.duration, direction: t.direction, easing: t.easing,
                  activeDuration: active, endTime: end, localTime: local === undefined ? null : local,
                  progress: null, currentIteration: null };
        if (local === null || local === undefined) { c.phase = 'idle'; return c; }
        var beforeEnd = Math.max(Math.min(t.delay, end), 0), activeEnd = Math.max(Math.min(t.delay + active, end), 0);
        var phase = local < beforeEnd || (rate < 0 && local === beforeEnd) ? 'before'
                  : local > activeEnd || (rate >= 0 && local === activeEnd) ? 'after' : 'active';
        c.phase = phase;
        var fill = t.fill === 'auto' ? 'none' : t.fill, activeTime = null;
        if (phase === 'active') activeTime = local - t.delay;
        else if (phase === 'before' && (fill === 'backwards' || fill === 'both')) activeTime = Math.max(local - t.delay, 0);
        else if (phase === 'after' && (fill === 'forwards' || fill === 'both')) activeTime = Math.max(Math.min(local - t.delay, active), 0);
        if (activeTime === null) return c;
        var overall = dur === 0 ? (phase === 'before' ? 0 : t.iterations) : activeTime / dur;
        overall += t.iterationStart;
        var simple = !isFinite(overall) ? t.iterationStart % 1 : overall % 1;
        if (simple === 0 && (phase === 'active' || phase === 'after') && (activeTime === active) && t.iterations !== 0 && overall !== 0) simple = 1;
        var iter = phase === 'after' && !isFinite(t.iterations) ? Infinity
                 : simple === 1 ? Math.floor(overall) - 1 : Math.floor(overall);
        var dir = t.direction, forwards = dir === 'normal' || (dir === 'alternate' && iter % 2 === 0) || (dir === 'alternate-reverse' && iter % 2 !== 0);
        if (dir === 'alternate' && iter === Infinity) forwards = true;
        var directed = forwards ? simple : 1 - simple;
        var before = (phase === 'before') !== !forwards && false;
        c.currentIteration = iter;
        c.progress = t.fn(directed, before);
        return c;
    }

    /* ---- keyframes ---- */
    function cssName(name) {
        if (name === 'cssFloat') return 'float';
        if (name === 'cssOffset') return 'offset';
        if (name.indexOf('--') === 0) return name;
        return name.replace(/[A-Z]/g, function (c) { return '-' + c.toLowerCase(); });
    }
    function idlName(name) {
        if (name === 'float') return 'cssFloat';
        if (name === 'offset') return 'cssOffset';
        if (name.indexOf('--') === 0) return name;
        return name.replace(/-([a-z])/g, function (all, c) { return c.toUpperCase(); });
    }
    var RESERVED = { offset: 1, easing: 1, composite: 1 };
    function frameOffset(v) {
        if (v === null || v === undefined) return null;
        v = Number(v);
        if (isNaN(v) || !isFinite(v) || v < 0 || v > 1) throw new TypeError('Offsets must be null or in the range [0,1]');
        return v;
    }
    function frameComposite(v) {
        if (v === undefined || v === null || v === 'auto') return 'auto';
        v = String(v);
        if (['replace', 'add', 'accumulate'].indexOf(v) < 0) throw new TypeError('Invalid composite');
        return v;
    }
    function distribute(frames) {
        if (!frames.length) return frames;
        var last = -Infinity;
        frames.forEach(function (f) {
            if (f.offset === null) return;
            if (f.offset < last) throw new TypeError('Offsets must be loosely sorted');
            last = f.offset;
        });
        frames.forEach(function (f) { f.computed = f.offset; });
        if (frames.length === 1 && frames[0].computed === null) frames[0].computed = 1;
        if (frames[0].computed === null) frames[0].computed = 0;
        if (frames[frames.length - 1].computed === null) frames[frames.length - 1].computed = 1;
        for (var i = 1; i < frames.length; i++) {
            if (frames[i].computed !== null) continue;
            var k = i;
            while (frames[k].computed === null) k++;
            for (var j = i; j < k; j++)
                frames[j].computed = frames[i - 1].computed + (frames[k].computed - frames[i - 1].computed) * (j - i + 1) / (k - i + 1);
        }
        return frames;
    }
    function parseKeyframes(input) {
        if (input === null || input === undefined) return [];
        if (typeof input !== 'object' && typeof input !== 'function') throw new TypeError('Keyframes must be an object');
        var frames = [], iter = input[Symbol.iterator];
        if (iter !== undefined && iter !== null) {
            if (typeof iter !== 'function') throw new TypeError('Invalid keyframes');
            Array.prototype.slice.call(Array.from(input)).forEach(function (k) {
                if (k === null || k === undefined) k = {};
                if (typeof k !== 'object' && typeof k !== 'function') throw new TypeError('Keyframes must be objects');
                var f = { offset: frameOffset(k.offset), easing: parseEasing(k.easing === undefined ? 'linear' : k.easing),
                          composite: frameComposite(k.composite), props: {} };
                Object.keys(k).sort().forEach(function (name) {
                    if (RESERVED[name] || (name.indexOf('-') >= 0 && name.indexOf('--') !== 0)) return;
                    f.props[cssName(name)] = String(k[name]);
                });
                frames.push(f);
            });
            return distribute(frames);
        }
        /* a property-indexed keyframe */
        var byOffset = [];
        var offsets = input.offset, easings = input.easing, composites = input.composite;
        Object.keys(input).sort().forEach(function (name) {
            if (RESERVED[name] || (name.indexOf('-') >= 0 && name.indexOf('--') !== 0)) return;
            var values = input[name];
            values = Array.isArray(values) ? values : [values];
            values.forEach(function (v, i) {
                byOffset.push({ prop: cssName(name), value: String(v), at: values.length === 1 ? 1 : i / (values.length - 1) });
            });
        });
        byOffset.sort(function (a, b) { return a.at - b.at; });
        byOffset.forEach(function (e) {
            var f = frames.length && frames[frames.length - 1]._at === e.at ? frames[frames.length - 1] : null;
            if (!f) { f = { _at: e.at, offset: null, easing: LINEAR, composite: 'auto', props: {} }; frames.push(f); }
            f.props[e.prop] = e.value;
        });
        frames.forEach(function (f, i) {
            if (Array.isArray(offsets)) { if (i < offsets.length) f.offset = frameOffset(offsets[i]); }
            else if (offsets !== undefined && offsets !== null) { if (i === 0) f.offset = frameOffset(offsets); }
            else f.offset = f._at;
            var e = Array.isArray(easings) ? easings[easings.length ? i % easings.length : 0] : easings;
            f.easing = parseEasing(e === undefined ? 'linear' : e);
            var c = Array.isArray(composites) ? composites[composites.length ? i % composites.length : 0] : composites;
            f.composite = frameComposite(c);
        });
        return distribute(frames);
    }
    function pseudoOf(v) {
        if (v === null || v === undefined) return null;
        v = String(v);
        if (!/^::?(before|after|marker|first-line|first-letter|selection|view-transition)/.test(v))
            throw new global.DOMException('Invalid pseudo-element: ' + v, 'SyntaxError');
        return v === ':before' ? '::before' : v === ':after' ? '::after' : v;
    }

    /* ---- AnimationEffect, KeyframeEffect ---- */
    function AnimationEffect() { throw new TypeError('Illegal constructor'); }
    function setMethods(proto, methods) {
        Object.keys(methods).forEach(function (n) {
            Object.defineProperty(proto, n, { value: methods[n], writable: true, enumerable: true, configurable: true });
        });
    }
    function setAccessors(proto, list) {
        Object.keys(list).forEach(function (n) {
            Object.defineProperty(proto, n, { get: list[n][0], set: list[n][1], enumerable: true, configurable: true });
        });
    }
    setMethods(AnimationEffect.prototype, {
        getTiming: function () {
            var t = this._t;
            return { delay: t.delay, endDelay: t.endDelay, fill: t.fill, iterationStart: t.iterationStart, iterations: t.iterations,
                     duration: t.duration, direction: t.direction, easing: t.easing };
        },
        getComputedTiming: function () {
            var c = computedTiming(this);
            delete c.phase;
            if (c.fill === 'auto') c.fill = 'none';
            return c;
        },
        updateTiming: function (input) {
            this._t = updateTiming(this._t, input);
            if (this._anim) changed(this._anim);
        }
    });
    function KeyframeEffect(target, keyframes, options) {
        if (!new.target) throw new TypeError("Constructor KeyframeEffect requires 'new'");
        if (target instanceof KeyframeEffect && arguments.length === 1) {
            hide(this, '_t', Object.assign({}, target._t));
            hide(this, '_frames', target._frames);
            hide(this, '_target', target._target);
            hide(this, '_pseudo', target._pseudo);
            hide(this, '_composite', target._composite);
        } else {
            if (target !== null && !(target instanceof global.Element)) throw new TypeError('The target must be an Element or null');
            hide(this, '_target', target);
            var opts = typeof options === 'object' && options !== null ? options : null;
            hide(this, '_pseudo', opts ? pseudoOf(opts.pseudoElement) : null);
            hide(this, '_composite', opts && opts.composite !== undefined ? frameComposite(opts.composite) : 'replace');
            hide(this, '_t', timingOf(options));
            hide(this, '_frames', parseKeyframes(keyframes));
        }
        hide(this, '_anim', null);
        hide(this, '_values', null);
    }
    KeyframeEffect.prototype = Object.create(AnimationEffect.prototype, { constructor: { value: KeyframeEffect, writable: true, configurable: true } });
    setMethods(KeyframeEffect.prototype, {
        getKeyframes: function () {
            var comp = this._composite;
            return this._frames.map(function (f) {
                var o = { offset: f.offset, computedOffset: f.computed, easing: f.easing.text, composite: f.composite };
                Object.keys(f.props).forEach(function (p) { o[idlName(p)] = f.props[p]; });
                return o;
            });
        },
        setKeyframes: function (keyframes) {
            this._frames = parseKeyframes(keyframes);
            this._values = null;
            if (this._anim) changed(this._anim);
        }
    });
    setAccessors(KeyframeEffect.prototype, {
        target: [function () { return this._target; }, function (v) {
            if (v !== null && !(v instanceof global.Element)) throw new TypeError('The target must be an Element or null');
            var was = this._target;
            this._target = v;
            this._values = null;
            if (was) dirtyTargets.add(was);
            if (this._anim) changed(this._anim);
        }],
        pseudoElement: [function () { return this._pseudo; }, function (v) {
            this._pseudo = pseudoOf(v);
            if (this._anim) changed(this._anim);
        }],
        composite: [function () { return this._composite; }, function (v) {
            v = String(v);
            if (['replace', 'add', 'accumulate'].indexOf(v) < 0) throw new TypeError('Invalid composite');
            this._composite = v;
            if (this._anim) changed(this._anim);
        }]
    });

    /* The value effect gives each property at its progress: added to out. */
    /* (a frame's values by longhand: a longhand given outright over a shorthand's) */
    function frameLonghands(f) {
        if (f.long) return f.long;
        var out = {}, ks = Object.keys(f.props), rest = [];
        ks.forEach(function (k) {
            if (k.indexOf('--') === 0 || k === 'all' || !global.__cyberCssIsShorthand(k)) { rest.push(k); return; }
            var e = global.__cyberCssExpand(k, f.props[k]);
            if (e) Object.keys(e).forEach(function (l) { out[l] = e[l]; });
            else out[k] = f.props[k];
        });
        rest.forEach(function (k) { out[k] = f.props[k]; });
        return f.long = out;
    }
    function sampleEffect(effect, progress, out, el) {
        var frames = effect._frames, props = {};
        frames.forEach(function (f) { Object.keys(frameLonghands(f)).forEach(function (p) { props[p] = true; }); });
        Object.keys(props).forEach(function (prop) {
            var list = frames.filter(function (f) { return prop in f.long; });
            var first = list[0], last = list[list.length - 1];
            /* (the missing ends: the underlying value) */
            var pts = list.map(function (f) {
                return { at: f.computed, value: f.long[prop], easing: f.easing, composite: f.composite === 'auto' ? effect._composite : f.composite };
            });
            if (first.computed !== 0) pts.unshift({ at: 0, value: null, easing: LINEAR, composite: 'add' });
            if (last.computed !== 1) pts.push({ at: 1, value: null, easing: LINEAR, composite: 'add' });
            var i = 1;
            if (progress >= 1 && pts[pts.length - 1].at === 1) i = pts.length - 1;
            else while (i < pts.length - 1 && progress >= pts[i].at) i++;
            /* (keyframes at one offset: the last one before it, the first after) */
            var a = pts[i - 1], b = pts[i];
            var span = b.at - a.at, local = span > 0 ? (progress - a.at) / span : 1;
            var eased = a.easing.fn(local);
            var under = function () {
                if (out.has(prop)) return out.get(prop);
                return baseValue(effect, el, prop);
            };
            var resolve = function (p) {
                var u = p.value === null ? under() : resolved(effect, el, prop, p.value);
                if (p.value !== null && (p.composite === 'add' || p.composite === 'accumulate')) u = addValues(prop, under(), u, p.composite);
                return u;
            };
            var ends = /color/.test(prop) && !!wideColorPairs(a.value === null ? under() : resolve(a), b.value === null ? under() : resolve(b)) || (prop === 'transform' && a.value !== b.value) || prop === 'translate' || prop === 'rotate' || prop === 'scale' || prop === 'offset-rotate' || prop === 'border-shape' || /^(border-image|mask-border)-(slice|width|outset)$/.test(prop) || prop === 'filter' || prop === 'backdrop-filter' || REPEATABLE.test(prop) || prop === 'box-shadow' || prop === 'text-shadow' || SHAPE_PROP.test(prop);
            /* (an inset that's auto doesn't go between) */
            var autoish = function (v) { return v !== null && /^\s*(auto|initial|unset|revert|revert-layer)\s*$/i.test(v); };
            if (/^(top|right|bottom|left)$/.test(prop) && (autoish(a.value) || autoish(b.value))) {
                out.set(prop, eased < 0.5 ? resolve(a) : resolve(b));
                return;
            }
            /* (sizes that don't interpolate: the nearer keyframe) */
            if (SIZE_PROP.test(prop) && !sizesInterpolate(el, prop, a.value === null ? under() : a.value,
                                                           b.value === null ? under() : b.value)) {
                out.set(prop, eased < 0.5 ? resolve(a) : resolve(b));
                return;
            }
            out.set(prop, eased === 0 && !ends ? resolve(a) : eased === 1 && !ends ? resolve(b) : interpolate(prop, sizeResolved(el, prop, resolve(a)), sizeResolved(el, prop, resolve(b)), eased));
        });
    }
    function baseValue(effect, el, prop) {
        var base = effect._values && effect._values.base;
        if (!base) return global.getComputedStyle(el).getPropertyValue(prop);
        if (!(prop in base)) base[prop] = readWith(el, prop, null);
        return base[prop];
    }
    function resolved(effect, el, prop, value) {
        if (!effect._values) effect._values = { base: {}, read: {} };
        if (!needsResolving(prop, value)) return value;
        var key = prop + ':' + value, read = effect._values.read;
        if (!(key in read)) {
            var v = readWith(el, prop, value);
            read[key] = v || value;
        }
        return read[key];
    }

    /* ---- applying: each target's effects, in order ---- */
    var animations = new Set(), dirtyTargets = new Set(), byTarget = new Map(), sequence = 0;
    var running = new Set(), events = [], applying = 0;
    function relevantOf(anim) {
        var e = anim._effect;
        return !!(e && e._target && !e._pseudo);
    }
    function applyTarget(el) {
        applying++;
        try { applyTargetNow(el); } finally { applying--; }
    }
    function applyTargetNow(el) {
        tfEl = el;
        var list = [];
        animations.forEach(function (a) { if (a._effect && a._effect._target === el && !a._effect._pseudo) list.push(a); });
        list.sort(function (x, y) { return x._seq - y._seq; });
        var values = new Map();
        /* (what is underneath is read without what is on top) */
        list.forEach(function (a) { if (!a._effect._values) a._effect._values = { base: {}, read: {} }; });
        list.forEach(function (a) {
            var c = computedTiming(a._effect);
            if (c.progress === null) return;
            sampleEffect(a._effect, c.progress, values, el);
        });
        var text = '';
        values.forEach(function (v, p) { text += p + ': ' + v + '; '; });
        setStyle(el, text || null);
    }
    function applyDirty() {
        if (!dirtyTargets.size) return;
        var targets = Array.from(dirtyTargets);
        dirtyTargets.clear();
        targets.forEach(applyTarget);
    }
    function changed(anim) {
        if (anim._effect && anim._effect._target) dirtyTargets.add(anim._effect._target);
        applyDirty();
        schedule(anim);
    }
    function schedule(anim) {
        var state = anim.playState;
        if (anim._pendingPlay || anim._pendingPause || state === 'running') {
            running.add(anim);
            wake();
        } else {
            running.delete(anim);
        }
    }
    function resolveWith(holder, name, value) {
        var d = holder[name];
        d.settled = true;
        d.resolve(value);
    }
    function deferred() {
        var d = { settled: false };
        d.promise = new Promise(function (res, rej) { d.resolve = res; d.reject = rej; });
        d.promise.catch(noop);
        return d;
    }

    /* ---- timelines ---- */
    function AnimationTimeline() { throw new TypeError('Illegal constructor'); }
    setAccessors(AnimationTimeline.prototype, {
        currentTime: [function () { return global.performance.now() - (this._origin || 0); }, undefined]
    });
    function DocumentTimeline(options) {
        if (!new.target) throw new TypeError("Constructor DocumentTimeline requires 'new'");
        hide(this, '_origin', options && options.originTime !== undefined ? Number(options.originTime) : 0);
    }
    DocumentTimeline.prototype = Object.create(AnimationTimeline.prototype, { constructor: { value: DocumentTimeline, writable: true, configurable: true } });
    var defaultTimeline = new DocumentTimeline();

    /* ---- Animation ---- */
    var ET = global.EventTarget;
    function Animation(effect, timeline) {
        if (!new.target) throw new TypeError("Constructor Animation requires 'new'");
        var self = this;
        if (effect !== null && effect !== undefined && !(effect instanceof AnimationEffect)) throw new TypeError('Invalid effect');
        hide(self, '_listeners', {});
        hide(self, '_effect', null);
        hide(self, '_timeline', timeline === undefined ? defaultTimeline : timeline);
        hide(self, '_start', null);
        hide(self, '_hold', null);
        hide(self, '_rate', 1);
        hide(self, '_pendingRate', null);
        hide(self, '_pendingPlay', false);
        hide(self, '_pendingPause', false);
        hide(self, '_seq', ++sequence);
        hide(self, '_ready', deferred());
        self._ready.settled = true;
        self._ready.resolve(self);
        hide(self, '_finished', deferred());
        hide(self, '_finishedSeen', false);
        hide(self, '_replace', 'active');
        self.id = '';
        self.onfinish = null;
        self.oncancel = null;
        self.onremove = null;
        animations.add(self);
        if (effect) attachEffect(self, effect);
    }
    Animation.prototype = Object.create(ET ? ET.prototype : Object.prototype, { constructor: { value: Animation, writable: true, configurable: true } });
    /* (the events of an Animation, on a plain object rather than a native target) */
    setMethods(Animation.prototype, {
        addEventListener: function (type, fn, opts) {
            if (!fn) return;
            var l = this._listeners[type] = this._listeners[type] || [];
            var once = !!(opts && typeof opts === 'object' && opts.once);
            if (!l.some(function (e) { return e.fn === fn; })) l.push({ fn: fn, once: once });
        },
        removeEventListener: function (type, fn) {
            var l = this._listeners[type];
            if (l) this._listeners[type] = l.filter(function (e) { return e.fn !== fn; });
        },
        dispatchEvent: function (ev) {
            var type = ev.type, self = this, on = self['on' + type];
            Object.defineProperty(ev, 'target', { value: self, configurable: true });
            Object.defineProperty(ev, 'currentTarget', { value: self, configurable: true });
            if (typeof on === 'function') on.call(self, ev);
            (self._listeners[type] || []).slice().forEach(function (e) {
                if (e.once) self.removeEventListener(type, e.fn);
                if (typeof e.fn === 'function') e.fn.call(self, ev);
                else if (e.fn && typeof e.fn.handleEvent === 'function') e.fn.handleEvent(ev);
            });
            return !ev.defaultPrevented;
        }
    });
    function attachEffect(anim, effect) {
        if (effect._anim && effect._anim !== anim) effect._anim._effect = null;
        if (anim._effect) anim._effect._anim = null;
        anim._effect = effect;
        effect._anim = anim;
    }
    function timelineTime(anim) {
        var t = anim._timeline;
        return t ? t.currentTime : null;
    }
    function currentOf(anim) {
        if (anim._hold !== null) return anim._hold;
        var now = timelineTime(anim);
        if (anim._start === null || now === null) return null;
        return (now - anim._start) * anim._rate;
    }
    function endOf(anim) { return anim._effect ? computedEnd(anim._effect) : 0; }
    function computedEnd(effect) {
        var t = effect._t, dur = t.duration === 'auto' ? 0 : t.duration;
        var active = t.iterations === 0 || dur === 0 ? 0 : dur * t.iterations;
        return Math.max(t.delay + active + t.endDelay, 0);
    }
    function pending(anim) { return anim._pendingPlay || anim._pendingPause; }
    function newReady(anim) {
        if (anim._ready.settled) anim._ready = deferred();
    }
    /* "update the finished state" */
    function updateFinished(anim, didSeek) {
        var cur = currentOf(anim), now = timelineTime(anim);
        if (cur !== null && anim._start !== null && !pending(anim)) {
            var end = endOf(anim);
            if (anim._rate > 0 && cur >= end) {
                var was = anim._prevTime;
                anim._hold = didSeek ? cur : (was === undefined || was === null ? end : Math.max(was, end));
            } else if (anim._rate < 0 && cur <= 0) {
                var w = anim._prevTime;
                anim._hold = didSeek ? cur : (w === undefined || w === null ? 0 : Math.min(w, 0));
            } else if (anim._rate !== 0 && now !== null) {
                if (didSeek && anim._hold !== null) anim._start = now - anim._hold / anim._rate;
                anim._hold = null;
            }
        }
        anim._prevTime = currentOf(anim);
        if (anim.playState === 'finished') {
            if (!anim._finishedSeen && !pending(anim)) {
                anim._finishedSeen = true;
                var d = anim._finished;
                events.push(function () {
                    if (anim._finished !== d) return;
                    if (!d.settled) resolveWith(anim, '_finished', anim);
                    fire(anim, 'finish', anim.currentTime);
                });
                wake();
            }
        } else if (anim._finishedSeen || anim._finished.settled) {
            anim._finishedSeen = false;
            if (anim._finished.settled) anim._finished = deferred();
        }
    }
    function fire(anim, type, current) {
        var e = new global.Event(type);
        Object.defineProperty(e, 'currentTime', { value: current === undefined ? null : current, enumerable: true });
        Object.defineProperty(e, 'timelineTime', { value: timelineTime(anim), enumerable: true });
        anim.dispatchEvent(e);
    }
    /* A turn: what was pending starts or pauses. */
    function settlePending(anim) {
        var now = timelineTime(anim);
        if (now === null) return;
        if (anim._pendingPlay) {
            if (anim._pendingRate !== null) { anim._rate = anim._pendingRate; anim._pendingRate = null; }
            if (anim._hold !== null) {
                if (anim._rate !== 0) { anim._start = now - anim._hold / anim._rate; anim._hold = null; }
            } else if (anim._start !== null && anim._rate !== 0) {
                /* (the start time set by script stays) */
            }
            anim._pendingPlay = false;
        } else if (anim._pendingPause) {
            if (anim._start !== null && anim._hold === null) anim._hold = (now - anim._start) * anim._rate;
            if (anim._pendingRate !== null) { anim._rate = anim._pendingRate; anim._pendingRate = null; }
            anim._start = null;
            anim._pendingPause = false;
        } else {
            return;
        }
        if (!anim._ready.settled) resolveWith(anim, '_ready', anim);
        updateFinished(anim, false);
    }
    function silentlySetTime(anim, t) {
        if (t === null) {
            if (anim._start !== null || anim._hold !== null) throw new TypeError('Cannot set the current time to null');
            return;
        }
        var now = timelineTime(anim);
        if (anim._hold !== null || anim._start === null || now === null || anim._rate === 0) anim._hold = t;
        else anim._start = now - t / anim._rate;
        if (now === null) anim._start = null;
        anim._prevTime = null;
    }
    setAccessors(Animation.prototype, {
        effect: [function () { return this._effect; }, function (e) {
            if (e === this._effect) return;
            if (e !== null && !(e instanceof AnimationEffect)) throw new TypeError('Invalid effect');
            var was = this._effect;
            if (was && was._target) dirtyTargets.add(was._target);
            if (was) was._anim = null;
            this._effect = null;
            if (e) attachEffect(this, e);
            changed(this);
            updateFinished(this, false);
        }],
        timeline: [function () { return this._timeline; }, function (t) {
            var wasTime = currentOf(this);
            this._timeline = t;
            if (wasTime !== null && this._start !== null) silentlySetTime(this, wasTime);
            changed(this);
        }],
        startTime: [function () { return this._start; }, function (v) {
            v = v === null || v === undefined ? null : Number(v);
            var now = timelineTime(this), was = currentOf(this);
            if (now === null && v !== null) this._hold = null;
            this._start = v;
            if (v !== null) this._hold = this._rate === 0 ? this._hold : null;
            else this._hold = was;
            var wasPending = pending(this);
            this._pendingPlay = this._pendingPause = false;
            if (this._pendingRate !== null) { this._rate = this._pendingRate; this._pendingRate = null; }
            if (wasPending && !this._ready.settled) resolveWith(this, '_ready', this);
            updateFinished(this, true);
            changed(this);
        }],
        currentTime: [function () { return currentOf(this); }, function (v) {
            v = v === null || v === undefined ? null : Number(v);
            if (v !== null && !isFinite(v)) throw new TypeError('Invalid current time');
            silentlySetTime(this, v);
            if (this._pendingPause) {
                this._hold = v;
                if (this._pendingRate !== null) { this._rate = this._pendingRate; this._pendingRate = null; }
                this._start = null;
                this._pendingPause = false;
                if (!this._ready.settled) resolveWith(this, '_ready', this);
            }
            updateFinished(this, true);
            changed(this);
        }],
        playbackRate: [function () { return this._rate; }, function (v) {
            this._pendingRate = null;
            var was = currentOf(this);
            this._rate = Number(v);
            if (was !== null) silentlySetTime(this, was);
            updateFinished(this, false);
            changed(this);
        }],
        playState: [function () {
            var cur = currentOf(this);
            if (cur === null && !pending(this)) return 'idle';
            if (this._pendingPause || (this._start === null && !this._pendingPlay)) return 'paused';
            var rate = this._pendingRate !== null ? this._pendingRate : this._rate;
            if (cur !== null && ((rate > 0 && cur >= endOf(this)) || (rate < 0 && cur <= 0))) return 'finished';
            return 'running';
        }, undefined],
        replaceState: [function () { return this._replace; }, undefined],
        pending: [function () { return pending(this); }, undefined],
        ready: [function () { return this._ready.promise; }, undefined],
        finished: [function () { return this._finished.promise; }, undefined]
    });
    function playAnimation(anim, autoRewind) {
        var cur = currentOf(anim), rate = anim._pendingRate !== null ? anim._pendingRate : anim._rate, seek = false, end = endOf(anim), abortPause = false;
        if (rate > 0 && autoRewind && (cur === null || cur < 0 || cur >= end)) { anim._hold = 0; seek = true; }
        else if (rate < 0 && autoRewind && (cur === null || cur <= 0 || cur > end)) {
            if (end === Infinity) throw new global.DOMException('Cannot play a reversed infinite animation', 'InvalidStateError');
            anim._hold = end;
            seek = true;
        } else if (rate === 0 && cur === null) { anim._hold = 0; seek = true; }
        if (anim._pendingPause) { anim._pendingPause = false; abortPause = true; }
        if (anim._pendingPlay && !seek) { /* (already on its way) */ }
        if (anim._hold === null && !abortPause && anim._pendingRate === null) {
            if (!anim._pendingPlay && anim._start !== null) { updateFinished(anim, false); changed(anim); return; }
        }
        if (anim._hold !== null) anim._start = null;
        if (!anim._pendingPlay) newReady(anim);
        anim._pendingPlay = true;
        updateFinished(anim, false);
        changed(anim);
    }
    setMethods(Animation.prototype, {
        play: function () { playAnimation(this, true); },
        pause: function () {
            if (this._pendingPause) return;
            var cur = currentOf(this);
            if (cur === null) {
                if (this._rate >= 0) this._hold = 0;
                else {
                    if (endOf(this) === Infinity) throw new global.DOMException('Cannot pause a reversed infinite animation', 'InvalidStateError');
                    this._hold = endOf(this);
                }
            }
            if (!this._pendingPlay) newReady(this);
            this._pendingPlay = false;
            this._pendingPause = true;
            updateFinished(this, false);
            changed(this);
        },
        finish: function () {
            var rate = this._pendingRate !== null ? this._pendingRate : this._rate;
            if (rate === 0) throw new global.DOMException('Cannot finish an animation with a playback rate of 0', 'InvalidStateError');
            var end = endOf(this);
            if (rate > 0 && end === Infinity) throw new global.DOMException('Cannot finish an infinite animation', 'InvalidStateError');
            if (this._pendingRate !== null) { this._rate = this._pendingRate; this._pendingRate = null; }
            var limit = rate < 0 ? 0 : end;
            silentlySetTime(this, limit);
            var now = timelineTime(this);
            if (this._start === null && now !== null) { this._start = now - limit / this._rate; this._hold = null; }
            var hadPending = pending(this);
            this._pendingPause = false;
            if (this._pendingPlay && this._start !== null) this._pendingPlay = false;
            if (hadPending && !pending(this) && !this._ready.settled) resolveWith(this, '_ready', this);
            /* (sync: the promise and event don't wait for a turn's check) */
            updateFinished(this, true);
            changed(this);
        },
        cancel: function () {
            var self = this;
            if (self.playState !== 'idle') {
                if (!self._ready.settled) {
                    self._ready.settled = true;
                    self._ready.reject(new global.DOMException('The animation was canceled', 'AbortError'));
                }
                self._ready = deferred();
                self._ready.settled = true;
                self._ready.resolve(self);
                self._pendingPlay = self._pendingPause = false;
                if (!self._finished.settled) {
                    self._finished.settled = true;
                    self._finished.reject(new global.DOMException('The animation was canceled', 'AbortError'));
                }
                self._finished = deferred();
                self._finishedSeen = false;
                events.push(function () { fire(self, 'cancel', null); });
                wake();
            }
            self._hold = null;
            self._start = null;
            self._prevTime = null;
            changed(self);
        },
        reverse: function () {
            if (timelineTime(this) === null) throw new global.DOMException('The animation has no active timeline', 'InvalidStateError');
            var was = this._pendingRate !== null ? this._pendingRate : this._rate;
            this._pendingRate = -was;
            try { playAnimation(this, true); }
            catch (e) { this._pendingRate = null; throw e; }
        },
        updatePlaybackRate: function (v) {
            this._pendingRate = Number(v);
            var state = this.playState;
            if (pending(this)) return;
            if (state === 'idle' || state === 'paused') {
                this._rate = this._pendingRate;
                this._pendingRate = null;
                return;
            }
            if (state === 'finished') {
                var now = timelineTime(this), cur = currentOf(this);
                if (this._pendingRate === 0) this._start = now;
                else if (now !== null) this._start = now - cur / this._pendingRate;
                this._rate = this._pendingRate;
                this._pendingRate = null;
                updateFinished(this, false);
                changed(this);
                return;
            }
            playAnimation(this, false);
        },
        persist: function () { this._replace = 'persisted'; },
        commitStyles: function () {
            var e = this._effect;
            if (!e || !e._target) return;
            if (e._pseudo) throw new global.DOMException('The target is a pseudo-element', 'NoModificationAllowedError');
            var c = computedTiming(e);
            if (c.progress === null) return;
            var values = new Map();
            sampleEffect(e, c.progress, values, e._target);
            values.forEach(function (v, p) { e._target.style.setProperty(p, v); });
        }
    });

    /* ---- CSSAnimation, CSSTransition: Animation objects over css_anim.c's rows ---- */
    var rowOp = global.__cyberAnimRow, wrappers = new Map(), awaitingEnd = new Set();
    function rowTiming(a) { return a._gone ? null : rowOp(0, a._rec.row, a._rec.gen); }
    /* (the store's clock, on the timeline's) */
    function clockDelta(v) { return global.performance.now() - v[9]; }
    function CSSAnimation() { throw new TypeError('Illegal constructor'); }
    function CSSTransition() { throw new TypeError('Illegal constructor'); }
    CSSAnimation.prototype = Object.create(Animation.prototype, { constructor: { value: CSSAnimation, writable: true, configurable: true } });
    CSSTransition.prototype = Object.create(Animation.prototype, { constructor: { value: CSSTransition, writable: true, configurable: true } });
    /* What's left once its row has gone. */
    function settleRow(a) {
        if (a._gone) return;
        var v = rowTiming(a);
        if (v) { if (v[6]) hide(a, '_ended', true); return; }
        a._gone = true;
        /* (gone before it ended: cancelled, by a change of style) */
        a._state = a._ended ? 'finished' : 'idle';
    }
    [CSSAnimation.prototype, CSSTransition.prototype].forEach(function (P) {
        setAccessors(P, {
            effect: [function () { return this._cssEffect; }, noop],
            timeline: [function () { return defaultTimeline; }, noop],
            pending: [function () { return false; }, undefined],
            ready: [function () { return Promise.resolve(this); }, undefined],
            finished: [function () {
                settleRow(this);
                if (this._gone) return this._state === 'idle' ? new Promise(noop) : Promise.resolve(this);
                awaitingEnd.add(this);
                wake();
                return this._finished.promise;
            }, undefined],
            playbackRate: [function () { return 1; }, noop],
            playState: [function () {
                settleRow(this);
                if (this._gone) return this._state;
                var v = rowTiming(this);
                if (v[5]) return 'paused';
                return v[6] ? 'finished' : 'running';
            }, undefined],
            currentTime: [function () {
                settleRow(this);
                if (this._gone) return this._state === 'idle' ? null : this._lastEnd;
                var v = rowTiming(this);
                this._lastEnd = v[1] + v[2] * v[3];
                return v[4];
            }, function (t) {
                settleRow(this);
                if (this._gone || t === null || t === undefined) return;
                rowOp(1, this._rec.row, this._rec.gen, Number(t));
            }],
            startTime: [function () {
                settleRow(this);
                var v = rowTiming(this);
                return v && !v[5] ? v[0] + clockDelta(v) : null;
            }, function (t) {
                settleRow(this);
                var v = rowTiming(this);
                if (v && t !== null && t !== undefined) rowOp(2, this._rec.row, this._rec.gen, Number(t) - clockDelta(v));
            }]
        });
        setMethods(P, {
            play: function () { settleRow(this); if (!this._gone) rowOp(4, this._rec.row, this._rec.gen); },
            pause: function () { settleRow(this); if (!this._gone) rowOp(3, this._rec.row, this._rec.gen); },
            cancel: function () {
                settleRow(this);
                if (!this._gone) { this._rec.cancel(); this._gone = true; }
                this._state = 'idle';
                awaitingEnd.delete(this);
            },
            finish: function () {
                settleRow(this);
                var v = rowTiming(this);
                if (!v) return;
                if (!isFinite(v[3])) throw new global.DOMException('Cannot finish an infinite animation', 'InvalidStateError');
                rowOp(1, this._rec.row, this._rec.gen, v[1] + v[2] * v[3]);
            },
            reverse: noop,
            updatePlaybackRate: noop,
            commitStyles: function () { if (!this._gone) this._rec.commitStyles(); }
        });
    });
    var DIRECTIONS = ['normal', 'reverse', 'alternate', 'alternate-reverse'], FILLS = ['none', 'forwards', 'backwards', 'both'];
    function wrapRow(rec, el) {
        var key = rec.row + ':' + rec.gen, a = wrappers.get(key);
        if (a) return a;
        var isTransition = rec.transitionProperty !== undefined;
        a = Object.create(isTransition ? CSSTransition.prototype : CSSAnimation.prototype);
        hide(a, '_rec', rec);
        hide(a, '_gone', false);
        hide(a, '_state', 'finished');
        hide(a, '_lastEnd', 0);
        hide(a, '_finished', deferred());
        hide(a, '_rate', 1);
        hide(a, '_seq', ++sequence);
        hide(a, '_listeners', {});
        a.id = '';
        a.onfinish = a.oncancel = a.onremove = null;
        if (isTransition) a.transitionProperty = rec.transitionProperty;
        else a.animationName = rec.animationName;
        var effect = Object.create(KeyframeEffect.prototype);
        hide(effect, '_target', el);
        hide(effect, '_pseudo', rec.pseudo || null);
        hide(effect, '_composite', 'replace');
        hide(effect, '_frames', []);
        hide(effect, '_values', null);
        hide(effect, '_anim', a);
        var kept = timingOf(undefined);
        Object.defineProperty(effect, '_t', { configurable: true, get: function () {
            var v = rowTiming(a);
            if (!v) return kept;
            kept = timingOf({ delay: v[1], duration: v[2], iterations: v[3] });
            kept.fill = FILLS[v[7]] || 'none';
            kept.direction = DIRECTIONS[v[8]] || 'normal';
            return kept;
        }, set: function (t) { kept = t; } });
        hide(a, '_cssEffect', effect);
        wrappers.set(key, a);
        return a;
    }

    /* The event loop's turn (browser/anim_api.c): true while something still runs. */
    Object.defineProperty(global, '__cyberWebAnimTick', { configurable: true, value: function () {
        awaitingEnd.forEach(function (a) {
            var state = a.playState;
            if (state === 'finished') {
                awaitingEnd.delete(a);
                if (!a._finished.settled) resolveWith(a, '_finished', a);
            } else if (state === 'idle') {
                awaitingEnd.delete(a);
                /* (cancelled: what waited for its end hears so) */
                if (!a._finished.settled) {
                    a._finished.settled = true;
                    a._finished.reject(new global.DOMException('The animation was canceled', 'AbortError'));
                    a._finished = deferred();
                }
            }
        });
        running.forEach(function (a) { settlePending(a); });
        running.forEach(function (a) {
            if (a.playState === 'running' || a.playState === 'finished') {
                updateFinished(a, false);
                if (a._effect && a._effect._target) dirtyTargets.add(a._effect._target);
            }
            if (!pending(a) && a.playState !== 'running') running.delete(a);
        });
        applyDirty();
        var due = events;
        events = [];
        due.forEach(function (f) { try { f(); } catch (e) { global.setTimeout(function () { throw e; }, 0); } });
        return running.size > 0 || events.length > 0 || awaitingEnd.size > 0;
    } });

    /* ---- Element.animate, getAnimations ---- */
    Object.defineProperty(ELP, 'animate', { configurable: true, enumerable: true, writable: true, value: function animate(keyframes, options) {
        var effect = new KeyframeEffect(this, keyframes, options);
        var anim = new Animation(effect, options && options.timeline !== undefined ? options.timeline : undefined);
        if (options && typeof options === 'object' && options.id !== undefined) anim.id = String(options.id);
        anim.play();
        return anim;
    } });
    function relevant(anim) {
        var e = anim._effect;
        if (!e) return false;
        var c = computedTiming(e);
        if (c.phase === 'idle') return false;
        var state = anim.playState;
        /* current: before or running; in effect: it gives a value */
        return c.phase === 'before' || c.phase === 'active' || (state !== 'finished' && c.phase === 'active') || c.progress !== null;
    }
    var rowsOf = ELP.getAnimations;
    function nativeGetOf(el) {
        /* (style is up to date first: that's what starts them) */
        if (el.isConnected) global.getComputedStyle(el).getPropertyValue('opacity');
        var got = Array.from(rowsOf.call(el)).map(function (rec) { return wrapRow(rec, el); });
        var st = transitionsOf.get(el);
        if (st) st.forEach(function (rec, prop) { if (rec.run && !rec.run.over && !rec.run.quiet) got.push(transitionAnimation(el, prop, rec.run)); });
        return got;
    }
    var nativeGet = rowsOf ? { call: nativeGetOf_ } : null;
    function nativeGetOf_(el) { return nativeGetOf(el); }
    Object.defineProperty(ELP, 'getAnimations', { configurable: true, enumerable: true, writable: true, value: function getAnimations(options) {
        var el = this, out = nativeGet ? Array.from(nativeGet.call(el)) : [], mine = [];
        var subtree = !!(options && options.subtree);
        animations.forEach(function (a) {
            var t = a._effect && a._effect._target;
            if (t && (t === el || (subtree && el.contains(t))) && relevant(a)) mine.push(a);
        });
        if (subtree) {
            var kids = el.querySelectorAll('*');
            for (var i = 0; nativeGet && i < kids.length; i++) out = out.concat(Array.from(nativeGet.call(kids[i])));
        }
        mine.sort(function (x, y) { return x._seq - y._seq; });
        return out.concat(mine);
    } });
    var DP = global.Document && global.Document.prototype;
    if (DP) {
        Object.defineProperty(DP, 'timeline', { configurable: true, enumerable: true, get: function () { return defaultTimeline; } });
        Object.defineProperty(DP, 'getAnimations', { configurable: true, enumerable: true, writable: true, value: function getAnimations() {
            var root = this.documentElement;
            return root ? [root].concat(Array.from(root.querySelectorAll('*'))).reduce(function (all, el) {
                return nativeGet ? all.concat(Array.from(nativeGet.call(el))) : all;
            }, []).concat(Array.from(animations).filter(relevant).sort(function (x, y) { return x._seq - y._seq; })) : [];
        } });
    }
    var SRP = global.ShadowRoot && global.ShadowRoot.prototype;
    if (SRP) Object.defineProperty(SRP, 'getAnimations', { configurable: true, enumerable: true, writable: true, value: function getAnimations() {
        var root = this;
        return Array.from(animations).filter(function (a) {
            var t = a._effect && a._effect._target;
            return t && relevant(a) && t.getRootNode() === root;
        });
    } });

    function AnimationPlaybackEvent(type, init) {
        var e = new global.Event(type, init);
        Object.setPrototypeOf(e, (new.target || AnimationPlaybackEvent).prototype);
        Object.defineProperty(e, 'currentTime', { value: init && init.currentTime !== undefined ? init.currentTime : null, enumerable: true, configurable: true });
        Object.defineProperty(e, 'timelineTime', { value: init && init.timelineTime !== undefined ? init.timelineTime : null, enumerable: true, configurable: true });
        return e;
    }
    AnimationPlaybackEvent.prototype = Object.create(global.Event.prototype, { constructor: { value: AnimationPlaybackEvent, writable: true, configurable: true } });

    expose('AnimationEffect', AnimationEffect);
    expose('KeyframeEffect', KeyframeEffect);
    expose('AnimationTimeline', AnimationTimeline);
    expose('DocumentTimeline', DocumentTimeline);
    expose('Animation', Animation);
    expose('CSSAnimation', CSSAnimation);
    expose('CSSTransition', CSSTransition);
    expose('AnimationPlaybackEvent', AnimationPlaybackEvent);
})

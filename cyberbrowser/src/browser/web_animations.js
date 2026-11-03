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
    var NUM = /[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?(%|[a-zA-Z]+)?/g;
    function tokens(text) {
        var lits = [], nums = [], at = 0, m;
        NUM.lastIndex = 0;
        while ((m = NUM.exec(text))) {
            /* (inside a name: translate3d, h1, a color's #123) */
            var c = m.index ? text[m.index - 1] : '';
            if (/[a-zA-Z_#-]/.test(c) || (/[-+]/.test(m[0][0]) && /[\w)]/.test(c))) continue;
            lits.push(text.slice(at, m.index));
            nums.push({ v: parseFloat(m[0]), u: (m[1] || '').toLowerCase() });
            at = m.index + m[0].length;
        }
        lits.push(text.slice(at));
        return { lits: lits, nums: nums };
    }
    function fmt(v) { return String(Math.round(v * 1e5) / 1e5); }
    var NONNEG = /^(width|height|min-|max-|padding|border(-[a-z]+)*-(width|radius)|font-size|line-height|flex-grow|flex-shrink|flex-basis|outline-width|column-width|column-gap|row-gap|gap$|perspective$|border-spacing|shape-margin|letter-spacing-none)/;
    var INTEGER = /^(z-index|order|orphans|widows|column-count|flood-none)$/;
    function finish(prop, v) {
        if (prop === 'opacity' || /-opacity$/.test(prop)) v = Math.max(0, Math.min(1, v));
        else if (INTEGER.test(prop)) v = Math.round(v);
        else if (NONNEG.test(prop) && v < 0) v = 0;
        return v;
    }
    /* (in a color function the channels are whole numbers and alpha at most 1) */
    function tidyColors(s) {
        return s.replace(/rgba?\(([^()]*)\)/g, function (all, body) {
            var v = body.split(',').map(parseFloat);
            if (v.length < 3 || v.some(isNaN)) return all;
            var rgb = v.slice(0, 3).map(function (c) { return Math.max(0, Math.min(255, Math.round(c))); });
            if (v.length === 4 && v[3] < 1) return 'rgba(' + rgb.join(', ') + ', ' + fmt(Math.max(0, v[3])) + ')';
            return 'rgb(' + rgb.join(', ') + ')';
        });
    }
    function combine(prop, a, b, how) {
        var ta = tokens(a), tb = tokens(b);
        if (ta.nums.length !== tb.nums.length || !ta.nums.length) return null;
        for (var i = 0; i < ta.lits.length; i++) if (ta.lits[i].toLowerCase() !== tb.lits[i].toLowerCase()) return null;
        var out = ta.lits[0];
        for (var k = 0; k < ta.nums.length; k++) {
            var x = ta.nums[k], y = tb.nums[k], ux = x.u || (x.v === 0 ? y.u : ''), uy = y.u || (y.v === 0 ? x.u : '');
            if (ux === uy) {
                out += fmt(finish(prop, how(x.v, y.v))) + ux;
            } else if (/^(%|px|em|rem|vw|vh|ch|ex)$/.test(ux) && /^(%|px|em|rem|vw|vh|ch|ex)$/.test(uy)) {
                /* (px and %: a calc(), the percentage first) */
                var p = how(x.v, 0), q = how(0, y.v);
                var first = uy === '%' ? fmt(q) + uy : fmt(p) + ux, second = uy === '%' ? p : q, unit = uy === '%' ? ux : uy;
                out += 'calc(' + first + (second < 0 ? ' - ' : ' + ') + fmt(Math.abs(second)) + unit + ')';
            } else {
                return null;
            }
            out += ta.lits[k + 1];
        }
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
    function interpolate(prop, a, b, t) {
        if (a === b) return a;
        if (/^(transform|filter|backdrop-filter)$/.test(prop)) {
            if (a === 'none' && b !== 'none') a = neutralFunctions(b);
            else if (b === 'none' && a !== 'none') b = neutralFunctions(a);
        }
        var v = combine(prop, a, b, function (x, y) { return x + (y - x) * t; });
        return v === null ? (t < 0.5 ? a : b) : v;
    }
    function addValues(prop, under, value) {
        var v = combine(prop, under, value, function (x, y) { return x + y; });
        return v === null ? value : v;
    }

    /* ---- what a keyframe's value comes to on an element ---- */
    var COLORISH = /color|background|border|outline|shadow|fill|stroke|caret|accent/;
    var RELATIVE = /\b(inherit|initial|unset|revert|revert-layer|currentcolor)\b|var\(|calc\(|min\(|max\(|clamp\(|[\d.](em|rem|ex|ch|vw|vh|vmin|vmax|lh|rlh|cap|ic|dvh|dvw|svh|svw|lvh|lvw)\b/i;
    function needsResolving(prop, value) {
        if (RELATIVE.test(value)) return true;
        if (COLORISH.test(prop) && !/^\s*[-+\d.]/.test(value) && value !== 'none') return true;
        return false;
    }
    /* The computed value of prop with this declaration in force on el. */
    function readWith(el, prop, value) {
        setStyle(el, value === null ? null : prop + ': ' + value);
        return global.getComputedStyle(el).getPropertyValue(prop);
    }

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
    function sampleEffect(effect, progress, out, el) {
        var frames = effect._frames, props = {};
        frames.forEach(function (f) { Object.keys(f.props).forEach(function (p) { props[p] = true; }); });
        Object.keys(props).forEach(function (prop) {
            var list = frames.filter(function (f) { return prop in f.props; });
            var first = list[0], last = list[list.length - 1];
            /* (the missing ends: the underlying value) */
            var pts = list.map(function (f) {
                return { at: f.computed, value: f.props[prop], easing: f.easing, composite: f.composite === 'auto' ? effect._composite : f.composite };
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
                if (p.value !== null && (p.composite === 'add' || p.composite === 'accumulate')) u = addValues(prop, under(), u);
                return u;
            };
            out.set(prop, eased === 0 ? resolve(a) : eased === 1 ? resolve(b) : interpolate(prop, resolve(a), resolve(b), eased));
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
    var running = new Set(), events = [];
    function relevantOf(anim) {
        var e = anim._effect;
        return !!(e && e._target && !e._pseudo);
    }
    function applyTarget(el) {
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
        if (a._gone || rowTiming(a)) return;
        a._gone = true;
        if (a._state !== 'idle') a._state = 'finished';
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
        return Array.from(rowsOf.call(el)).map(function (rec) { return wrapRow(rec, el); });
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

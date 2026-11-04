/*
 * typed_om.js - CSS Typed OM 1: style values, numeric math, transform values and
 * style property maps, over the engine's text values.
 * Run as fn(global, window).
 */
(function (global, win) {
    'use strict';
    function expose(name, value) {
        var d = { configurable: true, enumerable: false, writable: true, value: value };
        Object.defineProperty(global, name, d);
        if (win && win !== global) Object.defineProperty(win, name, d);
    }
    function idl(cls, name) {
        [cls.prototype, cls].forEach(function (o) {
            Object.getOwnPropertyNames(o).forEach(function (k) {
                if (k === 'constructor' || k === 'prototype' || k === 'name' || k === 'length' || k.slice(0, 2) === '__') return;
                var d = Object.getOwnPropertyDescriptor(o, k);
                d.enumerable = true;
                Object.defineProperty(o, k, d);
            });
        });
        Object.defineProperty(cls.prototype, Symbol.toStringTag, { configurable: true, value: name });
        Object.defineProperty(cls, 'prototype', { writable: false });
        expose(name, cls);
    }
    var ILLEGAL = {};
    function syntaxError(msg) {
        return global.DOMException ? new global.DOMException(msg, 'SyntaxError') : new SyntaxError(msg);
    }

    /* ---- units ---- */
    var UNITS = {
        number: null, percent: 'percent',
        em: 'length', rem: 'length', ex: 'length', rex: 'length', cap: 'length', rcap: 'length', ch: 'length', rch: 'length', ic: 'length', ric: 'length',
        lh: 'length', rlh: 'length', vw: 'length', vh: 'length', vi: 'length', vb: 'length', vmin: 'length', vmax: 'length',
        svw: 'length', svh: 'length', svi: 'length', svb: 'length', svmin: 'length', svmax: 'length',
        lvw: 'length', lvh: 'length', lvi: 'length', lvb: 'length', lvmin: 'length', lvmax: 'length',
        dvw: 'length', dvh: 'length', dvi: 'length', dvb: 'length', dvmin: 'length', dvmax: 'length',
        cqw: 'length', cqh: 'length', cqi: 'length', cqb: 'length', cqmin: 'length', cqmax: 'length',
        cm: 'length', mm: 'length', q: 'length', in: 'length', pt: 'length', pc: 'length', px: 'length',
        deg: 'angle', grad: 'angle', rad: 'angle', turn: 'angle', s: 'time', ms: 'time', hz: 'frequency', khz: 'frequency',
        dpi: 'resolution', dpcm: 'resolution', dppx: 'resolution', fr: 'flex',
    };
    var CANON = { px: 1, cm: 96 / 2.54, mm: 96 / 25.4, q: 96 / 101.6, in: 96, pt: 96 / 72, pc: 16,
                  deg: 1, grad: 0.9, rad: 180 / Math.PI, turn: 360, s: 1000, ms: 1, hz: 1, khz: 1000, dppx: 96, dpi: 1, dpcm: 2.54 };
    function canonUnit(u) {
        var l = String(u).toLowerCase();
        return l === '%' ? 'percent' : l;
    }
    var BASES = ['length', 'angle', 'time', 'frequency', 'resolution', 'flex', 'percent'];
    function emptyType() { return { percentHint: null }; }
    function typeOfUnit(u) {
        var t = emptyType(), c = UNITS[u];
        if (c) t[c] = 1;
        return t;
    }
    /* (Typed OM 4.1: add two types; null when they don't go) */
    function typeAdd(a, b) {
        var x = Object.assign({}, a), y = Object.assign({}, b);
        if (x.percentHint && y.percentHint && x.percentHint !== y.percentHint) return null;
        var hint = x.percentHint || y.percentHint;
        var eq = function (p, q) { return BASES.every(function (k) { return (p[k] || 0) === (q[k] || 0); }); };
        if (!eq(x, y)) {
            if (x.percent || y.percent) {
                var other = null;
                BASES.forEach(function (k) { if (k !== 'percent' && ((x[k] || 0) || (y[k] || 0))) other = other || k; });
                if (!other) return null;
                /* (percentages hint to the other's kind) */
                [x, y].forEach(function (t) { if (t.percent) { t[other] = (t[other] || 0) + t.percent; delete t.percent; } });
                hint = other;
                if (!eq(x, y)) return null;
            } else return null;
        }
        x.percentHint = hint;
        return x;
    }
    function typeMul(a, b) {
        if (a.percentHint && b.percentHint && a.percentHint !== b.percentHint) return null;
        var t = { percentHint: a.percentHint || b.percentHint };
        BASES.forEach(function (k) {
            var v = (a[k] || 0) + (b[k] || 0);
            if (a[k] !== undefined || b[k] !== undefined) t[k] = v;
        });
        return t;
    }
    function typeInvert(a) {
        var t = { percentHint: a.percentHint };
        BASES.forEach(function (k) { if (a[k] !== undefined) t[k] = -a[k]; });
        return t;
    }
    function typeOut(t) {
        /* (only the base types it has) */
        var o = {};
        BASES.forEach(function (k) { if (t[k]) o[k] = t[k]; });
        if (t.percentHint) o.percentHint = t.percentHint;
        return o;
    }
    function typesEqual(a, b) {
        return BASES.every(function (k) { return (a[k] || 0) === (b[k] || 0); }) && (a.percentHint || null) === (b.percentHint || null);
    }

    /* ---- values ---- */
    class CSSStyleValue {
        constructor(key) {
            if (key !== ILLEGAL && new.target === CSSStyleValue) throw new TypeError('Illegal constructor');
        }
        toString() { return this.__text === undefined ? '' : this.__text; }
        static parse(property, cssText) {
            return parseFor(String(property), String(cssText), false);
        }
        static parseAll(property, cssText) {
            return parseFor(String(property), String(cssText), true);
        }
    }
    function generic(text, prop) {
        var v = new CSSStyleValue(ILLEGAL);
        Object.defineProperty(v, '__text', { value: text, writable: true });
        if (prop) Object.defineProperty(v, '__prop', { value: prop });
        return v;
    }
    class CSSKeywordValue extends CSSStyleValue {
        constructor(value) {
            super(ILLEGAL);
            value = String(value);
            if (!value) throw new TypeError('A keyword must not be empty');
            Object.defineProperty(this, '__v', { value: value, writable: true });
        }
        get value() { return this.__v; }
        set value(v) {
            v = String(v);
            if (!v) throw new TypeError('A keyword must not be empty');
            this.__v = v;
        }
        toString() { return serIdent(this.__v); }
    }
    function serIdent(s) {
        if (/^-?[a-zA-Z_\u0080-￿][\w\u0080-￿-]*$|^--/.test(s)) return s;
        return global.CSS && global.CSS.escape ? global.CSS.escape(s) : s;
    }
    function rectify(v) {
        if (typeof v === 'number') return new CSSUnitValue(v, 'number');
        if (v instanceof CSSNumericValue) return v;
        throw new TypeError('A CSSNumericValue or number is required');
    }
    function rectifyKw(v) { return typeof v === 'string' ? new CSSKeywordValue(v) : v; }
    class CSSNumericValue extends CSSStyleValue {
        constructor(key) {
            super(ILLEGAL);
            if (key !== ILLEGAL) throw new TypeError('Illegal constructor');
        }
        add() {
            var a = [this].concat(Array.prototype.map.call(arguments, rectify));
            if (a.every(function (x) { return x instanceof CSSUnitValue && x.unit === a[0].unit; }))
                return new CSSUnitValue(a.reduce(function (s, x) { return s + x.value; }, 0), a[0].unit);
            var flat = [];
            a.forEach(function (x) { if (x instanceof CSSMathSum) flat = flat.concat(x.__args); else flat.push(x); });
            return makeMath(CSSMathSum, flat);
        }
        sub() {
            var a = Array.prototype.map.call(arguments, function (v) { return negate(rectify(v)); });
            return CSSNumericValue.prototype.add.apply(this, a);
        }
        mul() {
            var a = [this].concat(Array.prototype.map.call(arguments, rectify));
            var nums = a.filter(function (x) { return x instanceof CSSUnitValue && x.unit === 'number'; });
            var units = a.filter(function (x) { return x instanceof CSSUnitValue && x.unit !== 'number'; });
            if (nums.length + units.length === a.length && units.length <= 1) {
                var p = 1;
                a.forEach(function (x) { p *= x.value; });
                return new CSSUnitValue(p, units.length ? units[0].unit : 'number');
            }
            var flat = [];
            a.forEach(function (x) { if (x instanceof CSSMathProduct) flat = flat.concat(x.__args); else flat.push(x); });
            return makeMath(CSSMathProduct, flat);
        }
        div() {
            var a = Array.prototype.map.call(arguments, function (v) {
                var x = rectify(v);
                if (x instanceof CSSUnitValue && x.unit === 'number') {
                    if (x.value === 0) throw new RangeError('Division by zero');
                    return new CSSUnitValue(1 / x.value, 'number');
                }
                return new CSSMathInvert(x);
            });
            return CSSNumericValue.prototype.mul.apply(this, a);
        }
        min() { return minMax(this, arguments, CSSMathMin, Math.min); }
        max() { return minMax(this, arguments, CSSMathMax, Math.max); }
        equals() {
            var t = this;
            return Array.prototype.every.call(arguments, function (o) { return numEqual(t, rectify(o)); });
        }
        to(unit) {
            unit = canonUnit(unit);
            if (!(unit in UNITS)) throw new SyntaxError('Unknown unit');
            var sum = sumOf(this);
            if (!sum || sum.length !== 1) throw new TypeError('Cannot convert to ' + unit);
            var k = Object.keys(sum[0].units);
            if (k.length === 0) {
                if (unit !== 'number') throw new TypeError('Cannot convert a number to ' + unit);
                return new CSSUnitValue(sum[0].value, 'number');
            }
            if (k.length !== 1 || sum[0].units[k[0]] !== 1) throw new TypeError('Cannot convert to ' + unit);
            var c = convert(sum[0].value, k[0], unit);
            if (c === null) throw new TypeError('Cannot convert to ' + unit);
            return new CSSUnitValue(c, unit);
        }
        toSum() {
            var units = Array.prototype.map.call(arguments, function (u) {
                var c = canonUnit(u);
                if (!(c in UNITS)) throw new SyntaxError('Unknown unit ' + u);
                return c;
            });
            var sum = sumOf(this);
            if (!sum) throw new TypeError('Cannot make a sum of it');
            var vals = [];
            sum.forEach(function (term) {
                var k = Object.keys(term.units);
                if (k.length > 1 || (k.length === 1 && term.units[k[0]] !== 1)) throw new TypeError('Cannot make a sum of it');
                vals.push(new CSSUnitValue(term.value, k.length ? k[0] : 'number'));
            });
            if (!units.length) {
                vals.sort(function (a, b) { return a.unit < b.unit ? -1 : a.unit > b.unit ? 1 : 0; });
                return makeMath(CSSMathSum, vals);
            }
            var out = [];
            units.forEach(function (u) {
                var total = 0, rest = [];
                vals.forEach(function (v) {
                    var c = v.unit === u ? v.value : convert(v.value, v.unit, u);
                    if (c === null) rest.push(v); else total += c;
                });
                vals = rest;
                out.push(new CSSUnitValue(total, u));
            });
            if (vals.length) throw new TypeError('Not all of it converts');
            return makeMath(CSSMathSum, out);
        }
        type() { return typeOut(this.__type()); }
        static parse(cssText) {
            var v = parseNumeric(String(cssText).trim());
            if (!v) throw syntaxError('Not a numeric value: ' + cssText);
            return v;
        }
    }
    function convert(v, from, to) {
        if (from === to) return v;
        if (UNITS[from] !== UNITS[to] || !(from in CANON) || !(to in CANON)) return null;
        return v * CANON[from] / CANON[to];
    }
    /* (Typed OM 4.1.3: create a sum value) */
    function sumOf(v) {
        if (v instanceof CSSUnitValue) {
            var u = {};
            if (v.unit !== 'number') {
                var base = v.unit, val = v.value;
                if (v.unit in CANON) {
                    var cat = UNITS[v.unit];
                    base = cat === 'length' ? 'px' : cat === 'angle' ? 'deg' : cat === 'time' ? 'ms' : cat === 'frequency' ? 'hz' : cat === 'resolution' ? 'dpi' : v.unit;
                    val = v.value * CANON[v.unit] / CANON[base];
                }
                u[base] = 1;
                return [{ value: val, units: u }];
            }
            return [{ value: v.value, units: u }];
        }
        var a, out = [];
        if (v instanceof CSSMathSum) {
            for (a = 0; a < v.__args.length; a++) {
                var s = sumOf(v.__args[a]);
                if (!s) return null;
                s.forEach(function (t) {
                    var hit = out.find(function (o) { return sameUnits(o.units, t.units); });
                    if (hit) hit.value += t.value; else out.push(t);
                });
            }
            return out;
        }
        if (v instanceof CSSMathNegate) {
            var n = sumOf(v.__arg);
            return n && n.map(function (t) { return { value: -t.value, units: t.units }; });
        }
        if (v instanceof CSSMathProduct) {
            out = [{ value: 1, units: {} }];
            for (a = 0; a < v.__args.length; a++) {
                var f = sumOf(v.__args[a]);
                if (!f) return null;
                var next = [];
                out.forEach(function (x) {
                    f.forEach(function (y) {
                        var u2 = Object.assign({}, x.units);
                        Object.keys(y.units).forEach(function (k) {
                            u2[k] = (u2[k] || 0) + y.units[k];
                            if (!u2[k]) delete u2[k];
                        });
                        next.push({ value: x.value * y.value, units: u2 });
                    });
                });
                out = next;
            }
            return out;
        }
        if (v instanceof CSSMathInvert) {
            var iv = sumOf(v.__arg);
            if (!iv || iv.length !== 1 || !iv[0].value) return null;
            var u3 = {};
            Object.keys(iv[0].units).forEach(function (k) { u3[k] = -iv[0].units[k]; });
            return [{ value: 1 / iv[0].value, units: u3 }];
        }
        if (v instanceof CSSMathMin || v instanceof CSSMathMax) {
            var fn = v instanceof CSSMathMin ? Math.min : Math.max, sums = v.__args.map(sumOf);
            if (sums.some(function (x) { return !x || x.length !== 1 || !sameUnits(x[0].units, sums[0][0].units); })) return null;
            return [{ value: fn.apply(null, sums.map(function (x) { return x[0].value; })), units: sums[0][0].units }];
        }
        return null;
    }
    function sameUnits(a, b) {
        var ka = Object.keys(a), kb = Object.keys(b);
        return ka.length === kb.length && ka.every(function (k) { return a[k] === b[k]; });
    }
    function numEqual(a, b) {
        if (a === b) return true;
        if (a.constructor !== b.constructor) return false;
        if (a instanceof CSSUnitValue) return a.value === b.value && a.unit === b.unit;
        if (a.__args) return a.__args.length === b.__args.length && a.__args.every(function (x, i) { return numEqual(x, b.__args[i]); });
        if (a.__arg) return numEqual(a.__arg, b.__arg);
        if (a.__clamp) return a.__clamp.every(function (x, i) { return numEqual(x, b.__clamp[i]); });
        return false;
    }
    function negate(v) {
        if (v instanceof CSSUnitValue) return new CSSUnitValue(-v.value, v.unit);
        if (v instanceof CSSMathNegate) return v.__arg;
        return new CSSMathNegate(v);
    }
    function minMax(self, args, Cls, fn) {
        var a = [self].concat(Array.prototype.map.call(args, rectify));
        if (a.every(function (x) { return x instanceof CSSUnitValue && x.unit === a[0].unit; }))
            return new CSSUnitValue(fn.apply(null, a.map(function (x) { return x.value; })), a[0].unit);
        return makeMath(Cls, a);
    }
    function makeMath(Cls, args) {
        return new (Function.prototype.bind.apply(Cls, [null].concat(args)))();
    }
    class CSSUnitValue extends CSSNumericValue {
        constructor(value, unit) {
            super(ILLEGAL);
            if (typeof value !== 'number' && !(value instanceof Number)) value = Number(value);
            if (!isFinite(value) && value === value && false) throw new TypeError('not finite');
            var u = canonUnit(unit);
            if (!(u in UNITS)) throw new TypeError('Unknown unit: ' + unit);
            Object.defineProperty(this, '__v', { value: Number(value), writable: true });
            Object.defineProperty(this, '__u', { value: u });
        }
        get value() { return this.__v; }
        set value(v) {
            var n = Number(v);
            this.__v = n;
        }
        get unit() { return this.__u; }
        __type() { return typeOfUnit(this.__u); }
        toString() {
            var s = String(this.__v);
            if (this.__u === 'number') return s;
            if (this.__u === 'percent') return s + '%';
            return s + this.__u;
        }
    }
    /* math */
    class CSSNumericArray {
        constructor(key, list) {
            if (key !== ILLEGAL) throw new TypeError('Illegal constructor');
            Object.defineProperty(this, '__l', { value: list });
            for (var i = 0; i < list.length; i++) Object.defineProperty(this, i, { enumerable: true, value: list[i] });
        }
        get length() { return this.__l.length; }
        forEach(cb, thisArg) { this.__l.forEach(cb, thisArg); }
        keys() { return this.__l.keys(); }
        values() { return this.__l.values(); }
        entries() { return this.__l.entries(); }
        [Symbol.iterator]() { return this.__l[Symbol.iterator](); }
    }
    class CSSMathValue extends CSSNumericValue {
        constructor(key) {
            super(ILLEGAL);
            if (key !== ILLEGAL) throw new TypeError('Illegal constructor');
        }
        get operator() { return this.__op; }
        toString() { return serMath(this, false); }
    }
    function serArg(v, nested) { return v instanceof CSSMathValue ? serMath(v, nested) : String(v); }
    function serMath(v, nested) {
        var inner;
        if (v instanceof CSSMathMin || v instanceof CSSMathMax)
            return (v instanceof CSSMathMin ? 'min(' : 'max(') + v.__args.map(function (a) { return serArg(a, false); }).join(', ') + ')';
        if (v instanceof CSSMathClamp) return 'clamp(' + v.__clamp.map(function (a) { return serArg(a, false); }).join(', ') + ')';
        if (v instanceof CSSMathSum) {
            inner = serArg(v.__args[0], true);
            v.__args.slice(1).forEach(function (a) {
                if (a instanceof CSSMathNegate) inner += ' - ' + serArg(a.__arg, true);
                else if (a instanceof CSSUnitValue && a.value < 0) inner += ' - ' + String(new CSSUnitValue(-a.value, a.unit));
                else inner += ' + ' + serArg(a, true);
            });
        } else if (v instanceof CSSMathProduct) {
            inner = serArg(v.__args[0], true);
            v.__args.slice(1).forEach(function (a) {
                if (a instanceof CSSMathInvert) inner += ' / ' + serArg(a.__arg, true);
                else inner += ' * ' + serArg(a, true);
            });
        } else if (v instanceof CSSMathNegate) {
            inner = '-1 * ' + serArg(v.__arg, true);
            inner = '-' + serArg(v.__arg, true);
        } else if (v instanceof CSSMathInvert) {
            inner = '1 / ' + serArg(v.__arg, true);
        }
        return nested ? '(' + inner + ')' : 'calc(' + inner + ')';
    }
    function variadic(cls, op, typeFn) {
        Object.defineProperty(cls.prototype, '__op', { configurable: true, value: op });
        Object.defineProperty(cls.prototype, '__type', { configurable: true, value: typeFn });
    }
    function argsOf(args) {
        var a = Array.prototype.map.call(args, rectify);
        if (!a.length) throw new SyntaxError('No arguments');
        return a;
    }
    function checkSumType(a) {
        var t = a[0].__type();
        for (var i = 1; i < a.length; i++) {
            t = typeAdd(t, a[i].__type());
            if (!t) throw new TypeError('Incompatible types');
        }
        return t;
    }
    function checkProductType(a) {
        var t = a[0].__type();
        for (var i = 1; i < a.length; i++) {
            t = typeMul(t, a[i].__type());
            if (!t) throw new TypeError('Incompatible types');
        }
        return t;
    }
    class CSSMathSum extends CSSMathValue {
        constructor() {
            super(ILLEGAL);
            var a = argsOf(arguments);
            checkSumType(a);
            Object.defineProperty(this, '__args', { value: a });
        }
        get values() { return new CSSNumericArray(ILLEGAL, this.__args); }
    }
    variadic(CSSMathSum, 'sum', function () { return checkSumType(this.__args); });
    class CSSMathProduct extends CSSMathValue {
        constructor() {
            super(ILLEGAL);
            var a = argsOf(arguments);
            checkProductType(a);
            Object.defineProperty(this, '__args', { value: a });
        }
        get values() { return new CSSNumericArray(ILLEGAL, this.__args); }
    }
    variadic(CSSMathProduct, 'product', function () { return checkProductType(this.__args); });
    class CSSMathMin extends CSSMathValue {
        constructor() {
            super(ILLEGAL);
            var a = argsOf(arguments);
            checkSumType(a);
            Object.defineProperty(this, '__args', { value: a });
        }
        get values() { return new CSSNumericArray(ILLEGAL, this.__args); }
    }
    variadic(CSSMathMin, 'min', function () { return checkSumType(this.__args); });
    class CSSMathMax extends CSSMathValue {
        constructor() {
            super(ILLEGAL);
            var a = argsOf(arguments);
            checkSumType(a);
            Object.defineProperty(this, '__args', { value: a });
        }
        get values() { return new CSSNumericArray(ILLEGAL, this.__args); }
    }
    variadic(CSSMathMax, 'max', function () { return checkSumType(this.__args); });
    class CSSMathClamp extends CSSMathValue {
        constructor(lower, value, upper) {
            super(ILLEGAL);
            var a = [rectify(lower), rectify(value), rectify(upper)];
            checkSumType(a);
            Object.defineProperty(this, '__clamp', { value: a });
        }
        get lower() { return this.__clamp[0]; }
        get value() { return this.__clamp[1]; }
        get upper() { return this.__clamp[2]; }
    }
    variadic(CSSMathClamp, 'clamp', function () { return checkSumType(this.__clamp); });
    class CSSMathNegate extends CSSMathValue {
        constructor(arg) {
            super(ILLEGAL);
            Object.defineProperty(this, '__arg', { value: rectify(arg) });
        }
        get value() { return this.__arg; }
    }
    variadic(CSSMathNegate, 'negate', function () { return this.__arg.__type(); });
    class CSSMathInvert extends CSSMathValue {
        constructor(arg) {
            super(ILLEGAL);
            var a = rectify(arg);
            if (a instanceof CSSUnitValue && a.unit === 'number' && a.value === 0) throw new RangeError('Division by zero');
            Object.defineProperty(this, '__arg', { value: a });
        }
        get value() { return this.__arg; }
    }
    variadic(CSSMathInvert, 'invert', function () { return typeInvert(this.__arg.__type()); });

    /* CSS.registerProperty (css-properties-values-api 3.) */
    if (global.CSS && global.CSS.__register) {
        var hidden = {};
        ['__syntaxValid', '__syntaxParse', '__dependsOnContext', '__register'].forEach(function (k) {
            hidden[k] = global.CSS[k];
            delete global.CSS[k];
        });
        Object.defineProperty(global, '__cyberCssSyntax', { value: hidden, configurable: true });
        Object.defineProperty(global.CSS, 'registerProperty', { configurable: true, enumerable: true, writable: true, value: function registerProperty(def) {
            if (arguments.length < 1) throw new TypeError('registerProperty: 1 argument required');
            if (def === null || typeof def !== 'object') throw new TypeError('registerProperty: not a dictionary');
            var has = {};
            ['name', 'syntax', 'inherits', 'initialValue'].forEach(function (k) { has[k] = def[k]; });
            if (has.name === undefined) throw new TypeError("registerProperty: 'name' is required");
            if (has.inherits === undefined) throw new TypeError("registerProperty: 'inherits' is required");
            var name = String(has.name), syntax = has.syntax === undefined ? '*' : String(has.syntax), inherits = !!has.inherits;
            var initial = has.initialValue === undefined ? null : String(has.initialValue);
            var err = function (m) { return new global.DOMException(m, 'SyntaxError'); };
            if (name.slice(0, 2) !== '--' || name.length < 3) throw err("'" + name + "' is not a custom property name");
            if (!hidden.__syntaxValid(syntax)) throw err("'" + syntax + "' is not a syntax");
            var universal = /^\s*\*\s*$/.test(syntax);
            if (initial === null) {
                if (!universal) throw err('An initial value is required');
            } else if (!universal) {
                var parsed = hidden.__syntaxParse(syntax, initial);
                if (parsed === null) throw err("'" + initial + "' is not a value of '" + syntax + "'");
                if (hidden.__dependsOnContext(initial)) throw err("'" + initial + "' is not computationally independent");
                initial = parsed;
            } else if (/var\(/.test(initial)) throw err('An initial value cannot use var()');
            if (!hidden.__register(name, syntax, inherits, initial))
                throw new global.DOMException("'" + name + "' is already registered", 'InvalidModificationError');
        } });
    }

    /* factories */
    if (global.CSS) {
        Object.keys(UNITS).forEach(function (u) {
            var name = u === 'percent' ? 'percent' : u;
            if (Object.prototype.hasOwnProperty.call(global.CSS, name)) return;
            Object.defineProperty(global.CSS, name, { configurable: true, enumerable: true, writable: true, value: function (v) { return new CSSUnitValue(Number(v), u); } });
        });
    }

    /* ---- parsing numeric text ---- */
    function tokenizeCalc(s) {
        var out = [], re = /\s*(?:([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)(%|[a-zA-Z]+)?|([a-zA-Z-]+)\(|([()+\-*\/,]))/y, m;
        var i = 0;
        for (;;) {
            re.lastIndex = i;
            m = re.exec(s);
            if (!m) break;
            i = re.lastIndex;
            if (m[1] !== undefined) out.push({ num: parseFloat(m[1]), unit: m[2] || '' });
            else if (m[3]) out.push({ fn: m[3].toLowerCase() });
            else out.push({ op: m[4] });
        }
        if (s.slice(i).trim()) return null;
        return out;
    }
    function parseNumeric(s) {
        var toks = tokenizeCalc(s);
        if (!toks || !toks.length) return null;
        var i = 0, peek = function () { return toks[i]; };
        function unit(t) {
            var u = canonUnit(t.unit || 'number');
            if (!(u in UNITS)) throw syntaxError('Unknown unit');
            return new CSSUnitValue(t.num, u);
        }
        function args() {
            var list = [expr()];
            while (peek() && peek().op === ',') { i++; list.push(expr()); }
            if (!peek() || peek().op !== ')') throw syntaxError('Expected )');
            i++;
            return list;
        }
        function value() {
            var t = peek();
            if (!t) throw syntaxError('Unexpected end');
            if (t.num !== undefined) { i++; return unit(t); }
            if (t.op === '(') { i++; var v = expr(); if (!peek() || peek().op !== ')') throw syntaxError('Expected )'); i++; return v; }
            if (t.fn) {
                i++;
                var a = args();
                if (t.fn === 'calc') { if (a.length !== 1) throw syntaxError('calc'); return a[0]; }
                if (t.fn === 'min') return simplify(makeMath(CSSMathMin, a));
                if (t.fn === 'max') return simplify(makeMath(CSSMathMax, a));
                if (t.fn === 'clamp' && a.length === 3) return new CSSMathClamp(a[0], a[1], a[2]);
                throw syntaxError('Unknown function ' + t.fn);
            }
            throw syntaxError('Unexpected token');
        }
        function term() {
            var list = [value()];
            while (peek() && (peek().op === '*' || peek().op === '/')) {
                var o = toks[i++].op, v = value();
                list.push(o === '*' ? v : new CSSMathInvert(v));
            }
            return list.length === 1 ? list[0] : simplifyProduct(list);
        }
        function expr() {
            var list = [term()];
            while (peek() && ((peek().op === '+' || peek().op === '-') || (peek().num !== undefined && peek().num < 0))) {
                if (peek().num !== undefined) { list.push(term()); continue; }
                var o = toks[i++].op, v = term();
                list.push(o === '+' ? v : negate(v));
            }
            return list.length === 1 ? list[0] : simplifySum(list);
        }
        var r;
        try { r = value(); } catch (e) { if (e instanceof TypeError) return null; throw e; }
        if (i !== toks.length) return null;
        return r;
    }
    function simplifySum(list) {
        var out = [];
        list.forEach(function (v) {
            var hit = v instanceof CSSUnitValue && out.find(function (o) { return o instanceof CSSUnitValue && o.unit === v.unit; });
            if (hit) hit.value += v.value; else out.push(v);
        });
        return out.length === 1 ? out[0] : makeMath(CSSMathSum, out);
    }
    function simplifyProduct(list) {
        var p = 1, rest = [];
        list.forEach(function (v) {
            if (v instanceof CSSUnitValue && v.unit === 'number') p *= v.value;
            else if (v instanceof CSSMathInvert && v.__arg instanceof CSSUnitValue && v.__arg.unit === 'number') p /= v.__arg.value;
            else rest.push(v);
        });
        if (rest.length === 1 && rest[0] instanceof CSSUnitValue) return new CSSUnitValue(p * rest[0].value, rest[0].unit);
        if (!rest.length) return new CSSUnitValue(p, 'number');
        if (p !== 1) rest.unshift(new CSSUnitValue(p, 'number'));
        return rest.length === 1 ? rest[0] : makeMath(CSSMathProduct, rest);
    }
    function simplify(v) { return v; }

    /* ---- unparsed values ---- */
    class CSSVariableReferenceValue {
        constructor(variable, fallback) {
            variable = String(variable);
            if (variable.slice(0, 2) !== '--') throw new TypeError('A custom property name is required');
            if (fallback !== undefined && fallback !== null && !(fallback instanceof CSSUnparsedValue)) throw new TypeError('fallback must be a CSSUnparsedValue');
            Object.defineProperty(this, '__n', { value: variable, writable: true });
            Object.defineProperty(this, '__f', { value: fallback === undefined ? null : fallback, writable: true });
        }
        get variable() { return this.__n; }
        set variable(v) {
            v = String(v);
            if (v.slice(0, 2) !== '--') throw new TypeError('A custom property name is required');
            this.__n = v;
        }
        get fallback() { return this.__f; }
    }
    function indexed(self, list, check) {
        return new Proxy(self, {
            get: function (t, k) {
                if (typeof k === 'string' && /^\d+$/.test(k)) return list[+k];
                return Reflect.get(t, k, t);
            },
            set: function (t, k, v) {
                if (typeof k === 'string' && /^\d+$/.test(k)) {
                    var i = +k;
                    if (i > list.length) throw new RangeError('Index out of range');
                    list[i] = check(v);
                    return true;
                }
                return Reflect.set(t, k, v, t);
            },
            has: function (t, k) { return typeof k === 'string' && /^\d+$/.test(k) ? +k < list.length : k in t; },
            ownKeys: function (t) { return Reflect.ownKeys(t).concat(list.map(function (x, i) { return String(i); })); },
            getOwnPropertyDescriptor: function (t, k) {
                if (typeof k === 'string' && /^\d+$/.test(k) && +k < list.length) return { configurable: true, enumerable: true, writable: true, value: list[+k] };
                return Reflect.getOwnPropertyDescriptor(t, k);
            },
        });
    }
    function unparsedSegment(v) {
        if (typeof v === 'string' || v instanceof CSSVariableReferenceValue) return v;
        return String(v);
    }
    class CSSUnparsedValue extends CSSStyleValue {
        constructor(members) {
            super(ILLEGAL);
            if (!members || typeof members[Symbol.iterator] !== 'function') throw new TypeError('A sequence is required');
            var list = Array.from(members).map(unparsedSegment);
            Object.defineProperty(this, '__l', { value: list });
            return indexed(this, list, unparsedSegment);
        }
        get length() { return this.__l.length; }
        forEach(cb, thisArg) { this.__l.forEach(cb, thisArg); }
        keys() { return this.__l.keys(); }
        values() { return this.__l.values(); }
        entries() { return this.__l.entries(); }
        [Symbol.iterator]() { return this.__l[Symbol.iterator](); }
        toString() {
            return this.__l.map(function (x) {
                if (typeof x === 'string') return x;
                return 'var(' + x.variable + (x.fallback ? ',' + x.fallback : '') + ')';
            }).join('');
        }
    }
    function parseUnparsed(text) {
        var segs = [], at = 0, i;
        while ((i = text.indexOf('var(', at)) >= 0) {
            if (i > at) segs.push(text.slice(at, i));
            var d = 1, j = i + 4;
            for (; j < text.length && d; j++) { if (text[j] === '(') d++; else if (text[j] === ')') d--; }
            var body = text.slice(i + 4, j - 1), c = body.indexOf(',');
            var name = (c < 0 ? body : body.slice(0, c)).trim();
            segs.push(new CSSVariableReferenceValue(name, c < 0 ? null : parseUnparsed(body.slice(c + 1))));
            at = j;
        }
        if (at < text.length) segs.push(text.slice(at));
        return new CSSUnparsedValue(segs);
    }

    /* ---- images ---- */
    class CSSImageValue extends CSSStyleValue {
        constructor(key) {
            super(ILLEGAL);
            if (key !== ILLEGAL) throw new TypeError('Illegal constructor');
        }
    }

    /* ---- transforms ---- */
    class CSSTransformComponent {
        constructor(key) {
            if (key !== ILLEGAL) throw new TypeError('Illegal constructor');
            Object.defineProperty(this, '__2d', { value: true, writable: true });
        }
        get is2D() { return this.__2d; }
        set is2D(v) { this.__2d = !!v; }
        toMatrix() {
            var D = global.DOMMatrix, m = new D();
            return this.__apply(m);
        }
    }
    function lengthish(v, what) {
        var n = rectify(v), t = n.__type();
        if (!typesEqual(t, typeOfUnit('px')) && !typesEqual(t, typeOfUnit('percent')) && !(t.length === 1 && t.percentHint === 'length')) throw new TypeError(what + ' must be a length or percentage');
        return n;
    }
    function lengthOnly(v, what) {
        var n = rectify(v);
        if (!typesEqual(n.__type(), typeOfUnit('px'))) throw new TypeError(what + ' must be a length');
        return n;
    }
    function angleOf(v, what) {
        if (typeof v === 'number') throw new TypeError(what + ' must be an angle');
        var n = rectify(v);
        if (!typesEqual(n.__type(), typeOfUnit('deg'))) throw new TypeError(what + ' must be an angle');
        return n;
    }
    function numberOf(v, what) {
        var n = rectify(v);
        if (!typesEqual(n.__type(), typeOfUnit('number'))) throw new TypeError(what + ' must be a number');
        return n;
    }
    function pxOf(n) {
        try { return n.to('px').value; } catch (e) { throw new TypeError('Cannot convert to a matrix'); }
    }
    function degOf(n) {
        try { return n.to('deg').value; } catch (e) { throw new TypeError('Cannot convert to a matrix'); }
    }
    function numOf(n) {
        try { return n.to('number').value; } catch (e) { throw new TypeError('Cannot convert to a matrix'); }
    }
    class CSSTranslate extends CSSTransformComponent {
        constructor(x, y, z) {
            super(ILLEGAL);
            this.__x = lengthish(x, 'x');
            this.__y = lengthish(y, 'y');
            this.__z = z === undefined ? new CSSUnitValue(0, 'px') : lengthOnly(z, 'z');
            this.__2d = z === undefined;
        }
        get x() { return this.__x; }
        set x(v) { this.__x = lengthish(v, 'x'); }
        get y() { return this.__y; }
        set y(v) { this.__y = lengthish(v, 'y'); }
        get z() { return this.__z; }
        set z(v) { this.__z = lengthOnly(v, 'z'); }
        __apply(m) { return m.translateSelf(pxOf(this.__x), pxOf(this.__y), this.__2d ? 0 : pxOf(this.__z)); }
        toString() { return this.__2d ? 'translate(' + this.__x + ', ' + this.__y + ')' : 'translate3d(' + this.__x + ', ' + this.__y + ', ' + this.__z + ')'; }
    }
    class CSSRotate extends CSSTransformComponent {
        constructor(a, b, c, d) {
            super(ILLEGAL);
            if (b === undefined) {
                this.__x = new CSSUnitValue(0, 'number');
                this.__y = new CSSUnitValue(0, 'number');
                this.__z = new CSSUnitValue(1, 'number');
                this.__a = angleOf(a, 'angle');
                this.__2d = true;
            } else {
                this.__x = numberOf(a, 'x');
                this.__y = numberOf(b, 'y');
                this.__z = numberOf(c, 'z');
                this.__a = angleOf(d, 'angle');
                this.__2d = false;
            }
        }
        get x() { return this.__x; }
        set x(v) { this.__x = numberOf(v, 'x'); }
        get y() { return this.__y; }
        set y(v) { this.__y = numberOf(v, 'y'); }
        get z() { return this.__z; }
        set z(v) { this.__z = numberOf(v, 'z'); }
        get angle() { return this.__a; }
        set angle(v) { this.__a = angleOf(v, 'angle'); }
        __apply(m) {
            if (this.__2d) return m.rotateAxisAngleSelf(0, 0, 1, degOf(this.__a));
            return m.rotateAxisAngleSelf(numOf(this.__x), numOf(this.__y), numOf(this.__z), degOf(this.__a));
        }
        toString() {
            return this.__2d ? 'rotate(' + this.__a + ')' : 'rotate3d(' + this.__x + ', ' + this.__y + ', ' + this.__z + ', ' + this.__a + ')';
        }
    }
    class CSSScale extends CSSTransformComponent {
        constructor(x, y, z) {
            super(ILLEGAL);
            this.__x = numberOf(x, 'x');
            this.__y = numberOf(y, 'y');
            this.__z = z === undefined ? new CSSUnitValue(1, 'number') : numberOf(z, 'z');
            this.__2d = z === undefined;
        }
        get x() { return this.__x; }
        set x(v) { this.__x = numberOf(v, 'x'); }
        get y() { return this.__y; }
        set y(v) { this.__y = numberOf(v, 'y'); }
        get z() { return this.__z; }
        set z(v) { this.__z = numberOf(v, 'z'); }
        __apply(m) { return m.scaleSelf(numOf(this.__x), numOf(this.__y), this.__2d ? 1 : numOf(this.__z)); }
        toString() { return this.__2d ? 'scale(' + this.__x + ', ' + this.__y + ')' : 'scale3d(' + this.__x + ', ' + this.__y + ', ' + this.__z + ')'; }
    }
    class CSSSkew extends CSSTransformComponent {
        constructor(ax, ay) {
            super(ILLEGAL);
            this.__x = angleOf(ax, 'ax');
            this.__y = ay === undefined ? new CSSUnitValue(0, 'deg') : angleOf(ay, 'ay');
        }
        get ax() { return this.__x; }
        set ax(v) { this.__x = angleOf(v, 'ax'); }
        get ay() { return this.__y; }
        set ay(v) { this.__y = angleOf(v, 'ay'); }
        get is2D() { return true; }
        set is2D(v) { /* (always) */ }
        __apply(m) {
            var t = new global.DOMMatrix([1, Math.tan(degOf(this.__y) * Math.PI / 180), Math.tan(degOf(this.__x) * Math.PI / 180), 1, 0, 0]);
            return m.multiplySelf(t);
        }
        toString() {
            var zero = this.__y instanceof CSSUnitValue && this.__y.value === 0;
            return 'skew(' + this.__x + (zero ? '' : ', ' + this.__y) + ')';
        }
    }
    class CSSSkewX extends CSSTransformComponent {
        constructor(ax) {
            super(ILLEGAL);
            this.__x = angleOf(ax, 'ax');
        }
        get ax() { return this.__x; }
        set ax(v) { this.__x = angleOf(v, 'ax'); }
        get is2D() { return true; }
        set is2D(v) { /* (always) */ }
        __apply(m) { return m.skewXSelf(degOf(this.__x)); }
        toString() { return 'skewX(' + this.__x + ')'; }
    }
    class CSSSkewY extends CSSTransformComponent {
        constructor(ay) {
            super(ILLEGAL);
            this.__y = angleOf(ay, 'ay');
        }
        get ay() { return this.__y; }
        set ay(v) { this.__y = angleOf(v, 'ay'); }
        get is2D() { return true; }
        set is2D(v) { /* (always) */ }
        __apply(m) { return m.skewYSelf(degOf(this.__y)); }
        toString() { return 'skewY(' + this.__y + ')'; }
    }
    function perspectiveLength(v) {
        if (typeof v === 'string') return rectifyKw(v);
        if (v instanceof CSSKeywordValue) return v;
        return lengthOnly(v, 'length');
    }
    class CSSPerspective extends CSSTransformComponent {
        constructor(length) {
            super(ILLEGAL);
            this.__l = perspectiveLength(length);
            if (this.__l instanceof CSSKeywordValue && this.__l.value !== 'none') throw new TypeError('length must be none or a length');
            this.__2d = false;
        }
        get length() { return this.__l; }
        set length(v) { this.__l = perspectiveLength(v); }
        get is2D() { return false; }
        set is2D(v) { /* (never) */ }
        __apply(m) {
            if (this.__l instanceof CSSKeywordValue) return m;
            var d = pxOf(this.__l), t = new global.DOMMatrix();
            t.m34 = -1 / Math.max(d, 1);
            return m.multiplySelf(t);
        }
        toString() { return 'perspective(' + this.__l + ')'; }
    }
    class CSSMatrixComponent extends CSSTransformComponent {
        constructor(matrix, options) {
            super(ILLEGAL);
            if (!(matrix instanceof global.DOMMatrixReadOnly)) throw new TypeError('A DOMMatrixReadOnly is required');
            this.__m = matrix;
            this.__2d = options && options.is2D !== undefined ? !!options.is2D : matrix.is2D;
        }
        get matrix() { return this.__m; }
        set matrix(v) {
            if (!(v instanceof global.DOMMatrixReadOnly)) throw new TypeError('A DOMMatrixReadOnly is required');
            this.__m = v;
        }
        __apply(m) { return m.multiplySelf(this.__m); }
        toString() {
            var t = this.__m;
            if (this.__2d) return 'matrix(' + [t.a, t.b, t.c, t.d, t.e, t.f].join(', ') + ')';
            return 'matrix3d(' + [t.m11, t.m12, t.m13, t.m14, t.m21, t.m22, t.m23, t.m24, t.m31, t.m32, t.m33, t.m34, t.m41, t.m42, t.m43, t.m44].join(', ') + ')';
        }
    }
    function transformMember(v) {
        if (!(v instanceof CSSTransformComponent)) throw new TypeError('A CSSTransformComponent is required');
        return v;
    }
    class CSSTransformValue extends CSSStyleValue {
        constructor(transforms) {
            super(ILLEGAL);
            if (!transforms || typeof transforms[Symbol.iterator] !== 'function') throw new TypeError('A sequence is required');
            var list = Array.from(transforms).map(transformMember);
            if (!list.length) throw new TypeError('At least one component is required');
            Object.defineProperty(this, '__l', { value: list });
            return indexed(this, list, transformMember);
        }
        get length() { return this.__l.length; }
        get is2D() { return this.__l.every(function (c) { return c.is2D; }); }
        toMatrix() {
            var m = new global.DOMMatrix();
            this.__l.forEach(function (c) { c.__apply(m); });
            return m;
        }
        forEach(cb, thisArg) { this.__l.forEach(cb, thisArg); }
        keys() { return this.__l.keys(); }
        values() { return this.__l.values(); }
        entries() { return this.__l.entries(); }
        [Symbol.iterator]() { return this.__l[Symbol.iterator](); }
        toString() { return this.__l.join(' '); }
    }
    function parseTransform(text) {
        var re = /([a-zA-Z0-9]+)\(((?:[^()]|\([^()]*\))*)\)/g, m, out = [];
        var left = text.replace(re, '').trim();
        if (left) return null;
        while ((m = re.exec(text))) {
            var n = m[1].toLowerCase(), raw = m[2].split(/,(?![^(]*\))/).map(function (x) { return x.trim(); });
            var a = raw.map(function (x) { return /^none$/i.test(x) ? 'none' : parseNumeric(x); });
            if (a.some(function (x) { return x === null; })) return null;
            var U = function (x) { return x === 0 || (x instanceof CSSUnitValue && x.value === 0 && x.unit === 'number') ? new CSSUnitValue(0, 'px') : x; };
            var A = function (x) { return x instanceof CSSUnitValue && x.value === 0 && x.unit === 'number' ? new CSSUnitValue(0, 'deg') : x; };
            switch (n) {
            case 'translate': out.push(new CSSTranslate(U(a[0]), a.length > 1 ? U(a[1]) : new CSSUnitValue(0, 'px'))); break;
            case 'translatex': out.push(new CSSTranslate(U(a[0]), new CSSUnitValue(0, 'px'))); break;
            case 'translatey': out.push(new CSSTranslate(new CSSUnitValue(0, 'px'), U(a[0]))); break;
            case 'translatez': out.push(new CSSTranslate(new CSSUnitValue(0, 'px'), new CSSUnitValue(0, 'px'), U(a[0]))); break;
            case 'translate3d': out.push(new CSSTranslate(U(a[0]), U(a[1]), U(a[2]))); break;
            case 'scale': out.push(new CSSScale(a[0], a.length > 1 ? a[1] : a[0])); break;
            case 'scalex': out.push(new CSSScale(a[0], 1)); break;
            case 'scaley': out.push(new CSSScale(1, a[0])); break;
            case 'scalez': out.push(new CSSScale(1, 1, a[0])); break;
            case 'scale3d': out.push(new CSSScale(a[0], a[1], a[2])); break;
            case 'rotate': out.push(new CSSRotate(A(a[0]))); break;
            case 'rotatex': out.push(new CSSRotate(1, 0, 0, A(a[0]))); break;
            case 'rotatey': out.push(new CSSRotate(0, 1, 0, A(a[0]))); break;
            case 'rotatez': out.push(new CSSRotate(0, 0, 1, A(a[0]))); break;
            case 'rotate3d': out.push(new CSSRotate(a[0], a[1], a[2], A(a[3]))); break;
            case 'skew': out.push(a.length > 1 ? new CSSSkew(A(a[0]), A(a[1])) : new CSSSkew(A(a[0]))); break;
            case 'skewx': out.push(new CSSSkewX(A(a[0]))); break;
            case 'skewy': out.push(new CSSSkewY(A(a[0]))); break;
            case 'perspective': out.push(new CSSPerspective(a[0] === 'none' ? 'none' : U(a[0]))); break;
            case 'matrix': case 'matrix3d':
                out.push(new CSSMatrixComponent(new global.DOMMatrixReadOnly(a.map(function (x) { return x.value; }))));
                break;
            default: return null;
            }
        }
        return out.length ? new CSSTransformValue(out) : null;
    }

    /* ---- text to a typed value, for a property ---- */
    function splitCommas(text) {
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
    function reify(prop, text) {
        text = String(text).trim();
        if (/\bvar\(/i.test(text) || prop.slice(0, 2) === '--') return parseUnparsed(text);
        if (/^(initial|inherit|unset|revert|revert-layer)$/i.test(text)) return new CSSKeywordValue(text.toLowerCase());
        if (global.__cyberCssIsShorthand && global.__cyberCssIsShorthand(prop)) return generic(text, prop);
        /* (colors: not keywords of their own, but for these) */
        if (/(^|-)color$/.test(prop) && !/^(currentcolor|transparent)$/i.test(text)) return generic(text, prop);
        if (/^-?[a-zA-Z_][\w-]*$/.test(text) && !/^(?:e|pi|infinity)$/i.test(text)) return new CSSKeywordValue(text);
        if ((prop === 'transform') && text !== 'none') {
            var tv;
            try { tv = parseTransform(text); } catch (e) { tv = null; }
            if (tv) return tv;
        }
        if (/^url\(/i.test(text) && /image|src/.test(prop)) {
            var iv = new CSSImageValue(ILLEGAL);
            Object.defineProperty(iv, '__text', { value: text });
            return iv;
        }
        if (/^[-+]?[\d.]|^(?:calc|min|max|clamp)\(/i.test(text) && !/\s(?![-+*/(])/.test(text.replace(/\([^)]*\)/g, ''))) {
            var n = null;
            try { n = parseNumeric(text); } catch (e) { n = null; }
            if (n) return n;
        }
        return generic(text, prop);
    }
    function parseFor(prop, text, all) {
        if (prop.slice(0, 2) !== '--') {
            prop = prop.toLowerCase();
            if (!global.CSS.supports(prop, 'inherit')) throw new TypeError('Unknown property: ' + prop);
        }
        if (prop.slice(0, 2) !== '--' && !global.CSS.supports(prop, text)) throw new TypeError('Invalid value for ' + prop + ': ' + text);
        var parts = LIST_VALUED.test(prop) ? splitCommas(text) : [text];
        var vals = parts.map(function (t) { return reify(prop, t); });
        return all ? vals : vals[0];
    }
    var LIST_VALUED = /^(transition|animation|background|mask|box-shadow|text-shadow|will-change|font-family|grid-template-areas)(-|$)/;

    /* ---- style property maps ---- */
    var MAP = new WeakMap();
    function mapOf(self) {
        var m = MAP.get(self);
        if (!m) throw new TypeError('Illegal invocation');
        return m;
    }
    function propName(p) {
        p = String(p);
        return p.slice(0, 2) === '--' ? p : p.toLowerCase();
    }
    function knownProp(p) { return p.slice(0, 2) === '--' || global.CSS.supports(p, 'inherit'); }
    class StylePropertyMapReadOnly {
        constructor(key) {
            if (key !== ILLEGAL) throw new TypeError('Illegal constructor');
        }
        get(property) {
            var p = propName(property);
            if (!knownProp(p)) throw new TypeError('Unknown property: ' + property);
            var t = mapOf(this).read(p);
            if (!t) return null;
            return (LIST_VALUED.test(p) ? splitCommas(t) : [t]).map(function (x) { return reify(p, x); })[0];
        }
        getAll(property) {
            var p = propName(property);
            if (!knownProp(p)) throw new TypeError('Unknown property: ' + property);
            var t = mapOf(this).read(p);
            if (!t) return [];
            return (LIST_VALUED.test(p) ? splitCommas(t) : [t]).map(function (x) { return reify(p, x); });
        }
        has(property) {
            var p = propName(property);
            if (!knownProp(p)) throw new TypeError('Unknown property: ' + property);
            return !!mapOf(this).read(p);
        }
        get size() { return mapOf(this).names().length; }
        forEach(cb, thisArg) {
            var t = this;
            mapOf(this).names().forEach(function (n) { cb.call(thisArg, t.getAll(n), n, t); });
        }
        keys() { return mapOf(this).names()[Symbol.iterator](); }
        values() { var t = this; return mapOf(this).names().map(function (n) { return t.getAll(n); })[Symbol.iterator](); }
        entries() { var t = this; return mapOf(this).names().map(function (n) { return [n, t.getAll(n)]; })[Symbol.iterator](); }
        [Symbol.iterator]() { return this.entries(); }
    }
    function valueText(p, v) {
        if (typeof v === 'string') return v;
        if (!(v instanceof CSSStyleValue)) throw new TypeError('A CSSStyleValue or string is required');
        if (v.__prop && v.__prop !== p) throw new TypeError('The value is for ' + v.__prop);
        return v.toString();
    }
    class StylePropertyMap extends StylePropertyMapReadOnly {
        constructor(key) {
            super(key);
        }
        set(property) {
            var p = propName(property), m = mapOf(this);
            if (!knownProp(p)) throw new TypeError('Unknown property: ' + property);
            var vals = Array.prototype.slice.call(arguments, 1);
            if (vals.length > 1 && !LIST_VALUED.test(p)) throw new TypeError(p + ' is not list-valued');
            /* (a var() in a list: only alone) */
            if (vals.length > 1 && vals.some(function (v) { return /\bvar\(/i.test(String(v)); })) throw new TypeError('var() in a list of values');
            var text = vals.map(function (v) { return valueText(p, v); }).join(', ');
            if (p.slice(0, 2) !== '--' && !global.CSS.supports(p, text)) throw new TypeError('Invalid value for ' + p + ': ' + text);
            m.write(p, text);
        }
        append(property) {
            var p = propName(property), m = mapOf(this);
            if (!knownProp(p)) throw new TypeError('Unknown property: ' + property);
            if (!LIST_VALUED.test(p)) throw new TypeError(p + ' is not list-valued');
            var vals = Array.prototype.slice.call(arguments, 1).map(function (v) {
                var t = valueText(p, v);
                if (!global.CSS.supports(p, t)) throw new TypeError('Invalid value for ' + p + ': ' + t);
                if (/\bvar\(/i.test(t)) throw new TypeError('var() in a list of values');
                return t;
            });
            var old = m.read(p);
            m.write(p, (old ? [old] : []).concat(vals).join(', '));
        }
        delete(property) {
            var p = propName(property);
            if (!knownProp(p)) throw new TypeError('Unknown property: ' + property);
            mapOf(this).write(p, null);
        }
        clear() {
            var m = mapOf(this);
            m.names().forEach(function (n) { m.write(n, null); });
        }
    }
    function declMap(style) {
        var map = new StylePropertyMap(ILLEGAL);
        MAP.set(map, {
            read: function (p) { return style.getPropertyValue(p); },
            write: function (p, t) { if (t === null) style.removeProperty(p); else style.setProperty(p, t); },
            names: function () {
                var out = [];
                for (var i = 0; i < style.length; i++) out.push(style[i]);
                return out.sort(function (a, b) {
                    var ca = a.slice(0, 2) === '--', cb = b.slice(0, 2) === '--';
                    return ca !== cb ? (ca ? 1 : -1) : a < b ? -1 : a > b ? 1 : 0;
                });
            },
        });
        return map;
    }
    function computedMap(el, pseudo) {
        var map = new StylePropertyMapReadOnly(ILLEGAL);
        var cs = function () { return global.getComputedStyle(el, pseudo); };
        MAP.set(map, {
            read: function (p) { return cs().getPropertyValue(p); },
            write: function () { throw new TypeError('read only'); },
            names: function () {
                var s = cs(), out = [];
                for (var i = 0; i < s.length; i++) out.push(s[i]);
                return out.sort();
            },
        });
        return map;
    }
    var attrMaps = new WeakMap();
    function install(proto, name, get) {
        if (proto && !(name in proto)) Object.defineProperty(proto, name, { configurable: true, enumerable: true, get: get });
    }
    if (global.Element) {
        if (!('computedStyleMap' in global.Element.prototype))
            Object.defineProperty(global.Element.prototype, 'computedStyleMap', { configurable: true, enumerable: true, writable: true,
                value: function computedStyleMap() { return computedMap(this); } });
        [global.HTMLElement, global.SVGElement, global.MathMLElement].forEach(function (C) {
            if (!C) return;
            install(C.prototype, 'attributeStyleMap', function () {
                var m = attrMaps.get(this);
                if (!m) { m = declMap(this.style); attrMaps.set(this, m); }
                return m;
            });
        });
        if (!('attributeStyleMap' in global.Element.prototype))
            install(global.Element.prototype, 'attributeStyleMap', function () {
                if (!this.style) return undefined;
                var m = attrMaps.get(this);
                if (!m) { m = declMap(this.style); attrMaps.set(this, m); }
                return m;
            });
    }
    if (global.CSSStyleRule) {
        install(global.CSSStyleRule.prototype, 'styleMap', function () {
            var m = attrMaps.get(this);
            if (!m) { m = declMap(this.style); attrMaps.set(this, m); }
            return m;
        });
    }

    /* ---- colors ---- */
    function colorPct(v, name) {
        if (v instanceof CSSKeywordValue) { if (v.value !== 'none') throw syntaxError(name + ': only none'); return v; }
        if (typeof v === 'string') { if (v.toLowerCase() !== 'none') throw syntaxError(name + ': only none'); return new CSSKeywordValue('none'); }
        if (typeof v === 'number') return new CSSUnitValue(v * 100, 'percent');
        if (v instanceof CSSUnitValue && v.unit === 'percent') return v;
        if (v instanceof CSSUnitValue && v.unit === 'number') return new CSSUnitValue(v.value * 100, 'percent');
        if (v instanceof CSSMathValue && typesEqual(v.__type(), typeOfUnit('percent'))) return v;
        throw syntaxError(name + ' must be a percentage');
    }
    function colorNum(v, name) {
        if (v instanceof CSSKeywordValue) { if (v.value !== 'none') throw syntaxError(name + ': only none'); return v; }
        if (typeof v === 'string') { if (v.toLowerCase() !== 'none') throw syntaxError(name + ': only none'); return new CSSKeywordValue('none'); }
        if (typeof v === 'number') return new CSSUnitValue(v, 'number');
        if (v instanceof CSSUnitValue && (v.unit === 'number' || v.unit === 'percent')) return v;
        if (v instanceof CSSMathValue) return v;
        throw syntaxError(name + ' must be a number or percentage');
    }
    function colorAngle(v, name) {
        if (v instanceof CSSKeywordValue) { if (v.value !== 'none') throw syntaxError(name + ': only none'); return v; }
        if (typeof v === 'string') { if (v.toLowerCase() !== 'none') throw syntaxError(name + ': only none'); return new CSSKeywordValue('none'); }
        if (typeof v === 'number') return new CSSUnitValue(v, 'deg');
        if (v instanceof CSSUnitValue && typesEqual(v.__type(), typeOfUnit('deg'))) return v;
        if (v instanceof CSSMathValue && typesEqual(v.__type(), typeOfUnit('deg'))) return v;
        throw syntaxError(name + ' must be an angle');
    }
    class CSSColorValue extends CSSStyleValue {
        constructor(key) {
            super(ILLEGAL);
            if (key !== ILLEGAL) throw new TypeError('Illegal constructor');
        }
        static parse(cssText) {
            if (!global.CSS.supports('color', String(cssText))) throw syntaxError('Not a color: ' + cssText);
            return generic(String(cssText));
        }
    }
    /* members: [name, conversion]; the last, alpha, is optional and 100% */
    function colorClass(name, fn, members) {
        var cls = class extends CSSColorValue {
            constructor() {
                super(ILLEGAL);
                var a = arguments, self = this;
                if (a.length < members.length) throw new TypeError('Not enough arguments');
                members.forEach(function (m, i) { Object.defineProperty(self, '__' + m[0], { value: m[1](a[i], m[0]), writable: true }); });
                Object.defineProperty(self, '__alpha', { value: a[members.length] === undefined ? new CSSUnitValue(100, 'percent') : colorPct(a[members.length], 'alpha'), writable: true });
            }
            toString() {
                var self = this;
                var body = members.map(function (m) { return String(self['__' + m[0]]); }).join(' ');
                var al = this.__alpha;
                return fn + '(' + body + (al instanceof CSSUnitValue && al.value === 100 ? '' : ' / ' + al) + ')';
            }
        };
        members.concat([['alpha', colorPct]]).forEach(function (m) {
            Object.defineProperty(cls.prototype, m[0], {
                configurable: true,
                enumerable: true,
                get: function () { return this['__' + m[0]]; },
                set: function (v) { this['__' + m[0]] = m[1](v, m[0]); },
            });
        });
        Object.defineProperty(cls, 'length', { configurable: true, value: members.length });
        Object.defineProperty(cls, 'name', { configurable: true, value: name });
        idl(cls, name);
    }
    idl(CSSColorValue, 'CSSColorValue');
    colorClass('CSSRGB', 'rgb', [['r', colorNum], ['g', colorNum], ['b', colorNum]]);
    colorClass('CSSHSL', 'hsl', [['h', colorAngle], ['s', colorPct], ['l', colorPct]]);
    colorClass('CSSHWB', 'hwb', [['h', colorAngle], ['w', colorPct], ['b', colorPct]]);
    colorClass('CSSLab', 'lab', [['l', colorPct], ['a', colorNum], ['b', colorNum]]);
    colorClass('CSSLCH', 'lch', [['l', colorPct], ['c', colorNum], ['h', colorAngle]]);
    colorClass('CSSOKLab', 'oklab', [['l', colorPct], ['a', colorNum], ['b', colorNum]]);
    colorClass('CSSOKLCH', 'oklch', [['l', colorPct], ['c', colorNum], ['h', colorAngle]]);

    [[CSSKeywordValue, 1], [CSSUnitValue, 2], [CSSVariableReferenceValue, 1], [CSSUnparsedValue, 1], [CSSMathNegate, 1], [CSSMathInvert, 1], [CSSMathClamp, 3],
     [CSSTransformValue, 1], [CSSTranslate, 2], [CSSRotate, 1], [CSSScale, 2], [CSSSkew, 1], [CSSSkewX, 1], [CSSSkewY, 1], [CSSPerspective, 1], [CSSMatrixComponent, 1],
     [CSSStyleValue, 0], [CSSNumericValue, 0], [CSSMathValue, 0], [CSSNumericArray, 0], [CSSImageValue, 0], [CSSTransformComponent, 0],
     [StylePropertyMapReadOnly, 0], [StylePropertyMap, 0], [CSSMathSum, 0], [CSSMathProduct, 0], [CSSMathMin, 0], [CSSMathMax, 0]].forEach(function (p) {
        Object.defineProperty(p[0], 'length', { configurable: true, value: p[1] });
    });

    [[CSSStyleValue, 'CSSStyleValue'], [CSSKeywordValue, 'CSSKeywordValue'], [CSSNumericValue, 'CSSNumericValue'], [CSSUnitValue, 'CSSUnitValue'],
     [CSSNumericArray, 'CSSNumericArray'], [CSSMathValue, 'CSSMathValue'], [CSSMathSum, 'CSSMathSum'], [CSSMathProduct, 'CSSMathProduct'],
     [CSSMathMin, 'CSSMathMin'], [CSSMathMax, 'CSSMathMax'], [CSSMathClamp, 'CSSMathClamp'], [CSSMathNegate, 'CSSMathNegate'], [CSSMathInvert, 'CSSMathInvert'],
     [CSSVariableReferenceValue, 'CSSVariableReferenceValue'], [CSSUnparsedValue, 'CSSUnparsedValue'], [CSSImageValue, 'CSSImageValue'],
     [CSSTransformComponent, 'CSSTransformComponent'], [CSSTranslate, 'CSSTranslate'], [CSSRotate, 'CSSRotate'], [CSSScale, 'CSSScale'], [CSSSkew, 'CSSSkew'],
     [CSSSkewX, 'CSSSkewX'], [CSSSkewY, 'CSSSkewY'], [CSSPerspective, 'CSSPerspective'], [CSSMatrixComponent, 'CSSMatrixComponent'], [CSSTransformValue, 'CSSTransformValue'],
     [StylePropertyMapReadOnly, 'StylePropertyMapReadOnly'], [StylePropertyMap, 'StylePropertyMap']].forEach(function (p) { idl(p[0], p[1]); });
})

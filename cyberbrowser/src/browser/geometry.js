/*
 * geometry.js - Geometry Interfaces 1: DOMPoint, DOMQuad, DOMMatrix (and their
 * read-only forms).  DOMRect is the native one (misc_api.c).
 * Run as fn(global, window).
 */
(function (global, win) {
    'use strict';
    function expose(name, value) {
        var d = { configurable: true, enumerable: false, writable: true, value: value };
        Object.defineProperty(global, name, d);
        if (win && win !== global) Object.defineProperty(win, name, d);
    }
    /* (Web IDL: operations and attributes are enumerable) */
    function idl(cls, name) {
        [cls.prototype, cls].forEach(function (o) {
            Object.getOwnPropertyNames(o).forEach(function (k) {
                if (k === 'constructor' || k === 'prototype' || k === 'name' || k === 'length') return;
                var d = Object.getOwnPropertyDescriptor(o, k);
                d.enumerable = true;
                Object.defineProperty(o, k, d);
                /* (their arguments are optional, but for these) */
                if (typeof d.value === 'function' && !/^(setMatrixValue|fromFloat32Array|fromFloat64Array)$/.test(k))
                    Object.defineProperty(d.value, 'length', { configurable: true, value: 0 });
            });
        });
        Object.defineProperty(cls.prototype, Symbol.toStringTag, { configurable: true, value: name });
        Object.defineProperty(cls, 'prototype', { writable: false });
        expose(name, cls);
    }
    function num(v, what) {
        var n = Number(v);
        return n;
    }
    function finite(v, what) {
        var n = Number(v);
        if (!isFinite(n)) throw new TypeError(what + ' is not a finite number');
        return n;
    }
    function dictOf(o, what) {
        if (o === undefined || o === null) return {};
        if (typeof o !== 'object' && typeof o !== 'function') throw new TypeError(what + ' is not an object');
        return o;
    }

    /* ---- DOMPoint ---- */
    var PT = new WeakMap();
    function pt(self) {
        var p = PT.get(self);
        if (!p) throw new TypeError('Illegal invocation');
        return p;
    }
    class DOMPointReadOnly {
        constructor(x, y, z, w) {
            PT.set(this, [x === undefined ? 0 : num(x), y === undefined ? 0 : num(y), z === undefined ? 0 : num(z), w === undefined ? 1 : num(w)]);
        }
        static fromPoint(other) {
            var o = dictOf(other, 'other');
            return new DOMPointReadOnly(o.x, o.y, o.z, o.w);
        }
        get x() { return pt(this)[0]; }
        get y() { return pt(this)[1]; }
        get z() { return pt(this)[2]; }
        get w() { return pt(this)[3]; }
        matrixTransform(matrix) {
            var m = matrixOf(fixup(dictOf(matrix, 'matrix'))), p = pt(this);
            return new DOMPoint(p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + p[3] * m[12],
                                p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + p[3] * m[13],
                                p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + p[3] * m[14],
                                p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + p[3] * m[15]);
        }
        toJSON() {
            if (!(this instanceof DOMPointReadOnly)) throw new TypeError('Illegal invocation');
            return { x: this.x, y: this.y, z: this.z, w: this.w };
        }
    }
    class DOMPoint extends DOMPointReadOnly {
        constructor(x, y, z, w) { super(x, y, z, w); }
        static fromPoint(other) {
            var o = dictOf(other, 'other');
            return new DOMPoint(o.x, o.y, o.z, o.w);
        }
        get x() { return pt(this)[0]; }
        set x(v) { pt(this)[0] = num(v); }
        get y() { return pt(this)[1]; }
        set y(v) { pt(this)[1] = num(v); }
        get z() { return pt(this)[2]; }
        set z(v) { pt(this)[2] = num(v); }
        get w() { return pt(this)[3]; }
        set w(v) { pt(this)[3] = num(v); }
    }

    /* ---- DOMMatrix ---- */
    var MX = new WeakMap();     /* { m: 16 doubles, column-major; is2D } */
    function mx(self) {
        var v = MX.get(self);
        if (!v) throw new TypeError('Illegal invocation');
        return v;
    }
    function identity() { return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]; }
    function mul(a, b) {
        var r = new Array(16);
        for (var c = 0; c < 4; c++) for (var w = 0; w < 4; w++) {
            var v = 0;
            for (var k = 0; k < 4; k++) v += a[k * 4 + w] * b[c * 4 + k];
            r[c * 4 + w] = v;
        }
        return r;
    }
    function same(a, b) { return a === b || (a !== a && b !== b) || (a === 0 && b === 0); }
    /* validate and fixup (Geometry 6.4) */
    function fixup2d(o) {
        var pairs = [['a', 'm11'], ['b', 'm12'], ['c', 'm21'], ['d', 'm22'], ['e', 'm41'], ['f', 'm42']];
        var out = {};
        pairs.forEach(function (p) {
            var x = o[p[0]], y = o[p[1]];
            if (x !== undefined) x = finiteOrNan(x);
            if (y !== undefined) y = finiteOrNan(y);
            if (x !== undefined && y !== undefined && !same(x, y)) throw new TypeError(p[0] + ' and ' + p[1] + ' must be the same');
            out[p[1]] = y !== undefined ? y : x !== undefined ? x : (p[1] === 'm11' || p[1] === 'm22' ? 1 : 0);
        });
        return out;
    }
    function finiteOrNan(v) { return Number(v); }
    function fixup(o) {
        var d = fixup2d(o);
        var names3 = ['m13', 'm14', 'm23', 'm24', 'm31', 'm32', 'm33', 'm34', 'm43', 'm44'];
        var zero = { m13: 1, m14: 1, m23: 1, m24: 1, m31: 1, m32: 1, m34: 1, m43: 1 };
        var is2D = o.is2D === undefined ? undefined : !!o.is2D;
        names3.forEach(function (k) {
            d[k] = o[k] === undefined ? (k === 'm33' || k === 'm44' ? 1 : 0) : Number(o[k]);
        });
        var flat = true;
        names3.forEach(function (k) { if (zero[k] ? !(d[k] === 0) : !(d[k] === 1)) flat = false; });
        if (is2D === true && !flat) throw new TypeError('is2D is true but the matrix is not 2D');
        d.is2D = is2D === undefined ? flat : is2D;
        return d;
    }
    function matrixOf(d) {
        return [d.m11, d.m12, d.m13, d.m14, d.m21, d.m22, d.m23, d.m24, d.m31, d.m32, d.m33, d.m34, d.m41, d.m42, d.m43, d.m44];
    }
    function fromSeq(seq) {
        var a = Array.prototype.map.call(seq, Number);
        if (a.length === 6) return { m: [a[0], a[1], 0, 0, a[2], a[3], 0, 0, 0, 0, 1, 0, a[4], a[5], 0, 1], is2D: true };
        if (a.length === 16) return { m: a, is2D: false };
        throw new TypeError('The sequence must have 6 or 16 elements');
    }
    function fromString(text) {
        if (!win && !global.document) throw new TypeError('String arguments need a Window');
        text = String(text);
        if (!global.document) throw new TypeError('Failed to construct: strings are only for a Window');
        if (!text.trim()) return { m: identity(), is2D: true };
        var parse = global.__cyberTransformList;
        var r = parse ? parse(text) : null;
        if (!r) {
            var e = global.DOMException ? new global.DOMException('Failed to parse: ' + text, 'SyntaxError') : new SyntaxError(text);
            throw e;
        }
        return r;
    }
    function setup(self, init) {
        var v;
        if (init === undefined) v = { m: identity(), is2D: true };
        else if (typeof init === 'string') v = fromString(init);
        else if (init !== null && typeof init === 'object' && typeof init[Symbol.iterator] === 'function') v = fromSeq(Array.from(init));
        else if (init === null) v = fromString('null');
        else v = fromString(String(init));
        MX.set(self, v);
    }
    function clone(self, Cls) {
        var r = new Cls();
        var v = mx(self);
        MX.set(r, { m: v.m.slice(), is2D: v.is2D });
        return r;
    }
    function not2D(v) { v.is2D = false; }
    var ops = {
        multiply: function (v, other) {
            var d = fixup(dictOf(other, 'other')), o = matrixOf(d);
            v.m = mul(v.m, o);
            if (!d.is2D) not2D(v);
        },
        preMultiply: function (v, other) {
            var d = fixup(dictOf(other, 'other')), o = matrixOf(d);
            v.m = mul(o, v.m);
            if (!d.is2D) not2D(v);
        },
        translate: function (v, tx, ty, tz) {
            tx = tx === undefined ? 0 : Number(tx); ty = ty === undefined ? 0 : Number(ty); tz = tz === undefined ? 0 : Number(tz);
            var t = identity();
            t[12] = tx; t[13] = ty; t[14] = tz;
            v.m = mul(v.m, t);
            if (tz !== 0 || !(tz === tz)) not2D(v);
        },
        scale: function (v, sx, sy, sz, ox, oy, oz) {
            sx = sx === undefined ? 1 : Number(sx);
            sy = sy === undefined ? sx : Number(sy);
            sz = sz === undefined ? 1 : Number(sz);
            ox = ox === undefined ? 0 : Number(ox); oy = oy === undefined ? 0 : Number(oy); oz = oz === undefined ? 0 : Number(oz);
            ops.translate(v, ox, oy, oz);
            var t = identity();
            t[0] = sx; t[5] = sy; t[10] = sz;
            v.m = mul(v.m, t);
            if (sz !== 1 || oz !== 0) not2D(v);
            ops.translate(v, -ox, -oy, -oz);
        },
        scale3d: function (v, s, ox, oy, oz) {
            s = s === undefined ? 1 : Number(s);
            ops.scale(v, s, s, s, ox, oy, oz);
        },
        rotate: function (v, rx, ry, rz) {
            rx = rx === undefined ? 0 : Number(rx);
            if (ry === undefined && rz === undefined) { rz = rx; rx = 0; ry = 0; }
            ry = ry === undefined ? 0 : Number(ry);
            rz = rz === undefined ? 0 : Number(rz);
            if (rx !== 0 || ry !== 0) not2D(v);
            if (rz) ops.axis(v, 0, 0, 1, rz);
            if (ry) ops.axis(v, 0, 1, 0, ry);
            if (rx) ops.axis(v, 1, 0, 0, rx);
        },
        axis: function (v, x, y, z, deg) {
            var l = Math.sqrt(x * x + y * y + z * z);
            var t = identity();
            if (l) {
                x /= l; y /= l; z /= l;
                var r = deg * Math.PI / 180, c = Math.cos(r), s = Math.sin(r), k = 1 - c;
                t[0] = c + x * x * k; t[1] = x * y * k + z * s; t[2] = x * z * k - y * s;
                t[4] = x * y * k - z * s; t[5] = c + y * y * k; t[6] = y * z * k + x * s;
                t[8] = x * z * k + y * s; t[9] = y * z * k - x * s; t[10] = c + z * z * k;
            }
            v.m = mul(v.m, t);
        },
        rotateFromVector: function (v, x, y) {
            x = x === undefined ? 0 : Number(x); y = y === undefined ? 0 : Number(y);
            var deg = x === 0 && y === 0 ? 0 : Math.atan2(y, x) * 180 / Math.PI;
            ops.axis(v, 0, 0, 1, deg);
        },
        rotateAxisAngle: function (v, x, y, z, a) {
            x = x === undefined ? 0 : Number(x); y = y === undefined ? 0 : Number(y); z = z === undefined ? 0 : Number(z);
            a = a === undefined ? 0 : Number(a);
            if (x !== 0 || y !== 0) not2D(v);
            ops.axis(v, x, y, z, a);
        },
        skewX: function (v, s) {
            var t = identity();
            t[4] = Math.tan((s === undefined ? 0 : Number(s)) * Math.PI / 180);
            v.m = mul(v.m, t);
        },
        skewY: function (v, s) {
            var t = identity();
            t[1] = Math.tan((s === undefined ? 0 : Number(s)) * Math.PI / 180);
            v.m = mul(v.m, t);
        },
        invert: function (v) {
            var m = v.m, inv = inverse(m);
            if (!inv) { v.m = m.map(function () { return NaN; }); v.is2D = false; return; }
            v.m = inv;
        },
    };
    function det3(a, b, c, d, e, f, g, h, i) { return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g); }
    function inverse(m) {
        for (var i = 0; i < 16; i++) if (!isFinite(m[i])) return null;
        var out = new Array(16), det = 0;
        for (var c = 0; c < 4; c++) {
            var cols = [0, 1, 2, 3].filter(function (x) { return x !== c; });
            var minor = det3(m[cols[0] * 4 + 1], m[cols[0] * 4 + 2], m[cols[0] * 4 + 3], m[cols[1] * 4 + 1], m[cols[1] * 4 + 2], m[cols[1] * 4 + 3], m[cols[2] * 4 + 1], m[cols[2] * 4 + 2], m[cols[2] * 4 + 3]);
            det += (c % 2 ? -1 : 1) * m[c * 4] * minor;
        }
        if (det === 0 || !isFinite(det)) return null;
        for (var col = 0; col < 4; col++) for (var row = 0; row < 4; row++) {
            var cs = [0, 1, 2, 3].filter(function (x) { return x !== col; }), rs = [0, 1, 2, 3].filter(function (x) { return x !== row; });
            var v = det3(m[cs[0] * 4 + rs[0]], m[cs[0] * 4 + rs[1]], m[cs[0] * 4 + rs[2]], m[cs[1] * 4 + rs[0]], m[cs[1] * 4 + rs[1]], m[cs[1] * 4 + rs[2]], m[cs[2] * 4 + rs[0]], m[cs[2] * 4 + rs[1]], m[cs[2] * 4 + rs[2]]);
            out[row * 4 + col] = ((col + row) % 2 ? -v : v) / det;
        }
        return out;
    }
    function ser(v) { return String(v); }
    class DOMMatrixReadOnly {
        constructor(init) { setup(this, init); }
        static fromMatrix(other) {
            var d = fixup(dictOf(other, 'other')), r = new DOMMatrixReadOnly();
            MX.set(r, { m: matrixOf(d), is2D: d.is2D });
            return r;
        }
        static fromFloat32Array(a) {
            if (!(a instanceof Float32Array)) throw new TypeError('not a Float32Array');
            return arrayMatrix(DOMMatrixReadOnly, a);
        }
        static fromFloat64Array(a) {
            if (!(a instanceof Float64Array)) throw new TypeError('not a Float64Array');
            return arrayMatrix(DOMMatrixReadOnly, a);
        }
        get a() { return mx(this).m[0]; }
        get b() { return mx(this).m[1]; }
        get c() { return mx(this).m[4]; }
        get d() { return mx(this).m[5]; }
        get e() { return mx(this).m[12]; }
        get f() { return mx(this).m[13]; }
        get m11() { return mx(this).m[0]; }
        get m12() { return mx(this).m[1]; }
        get m13() { return mx(this).m[2]; }
        get m14() { return mx(this).m[3]; }
        get m21() { return mx(this).m[4]; }
        get m22() { return mx(this).m[5]; }
        get m23() { return mx(this).m[6]; }
        get m24() { return mx(this).m[7]; }
        get m31() { return mx(this).m[8]; }
        get m32() { return mx(this).m[9]; }
        get m33() { return mx(this).m[10]; }
        get m34() { return mx(this).m[11]; }
        get m41() { return mx(this).m[12]; }
        get m42() { return mx(this).m[13]; }
        get m43() { return mx(this).m[14]; }
        get m44() { return mx(this).m[15]; }
        get is2D() { return mx(this).is2D; }
        get isIdentity() {
            var m = mx(this).m, id = identity();
            for (var i = 0; i < 16; i++) if (!(m[i] === id[i])) return false;
            return true;
        }
        translate(x, y, z) { var r = clone(this, DOMMatrix); ops.translate(mx(r), x, y, z); return r; }
        scale(a, b, c, d, e, f) { var r = clone(this, DOMMatrix); ops.scale(mx(r), a, b, c, d, e, f); return r; }
        scaleNonUniform(a, b) { var r = clone(this, DOMMatrix); ops.scale(mx(r), a === undefined ? 1 : a, b === undefined ? 1 : b, 1, 0, 0, 0); return r; }
        scale3d(s, x, y, z) { var r = clone(this, DOMMatrix); ops.scale3d(mx(r), s, x, y, z); return r; }
        rotate(x, y, z) { var r = clone(this, DOMMatrix); ops.rotate(mx(r), x, y, z); return r; }
        rotateFromVector(x, y) { var r = clone(this, DOMMatrix); ops.rotateFromVector(mx(r), x, y); return r; }
        rotateAxisAngle(x, y, z, a) { var r = clone(this, DOMMatrix); ops.rotateAxisAngle(mx(r), x, y, z, a); return r; }
        skewX(s) { var r = clone(this, DOMMatrix); ops.skewX(mx(r), s); return r; }
        skewY(s) { var r = clone(this, DOMMatrix); ops.skewY(mx(r), s); return r; }
        multiply(o) { var r = clone(this, DOMMatrix); ops.multiply(mx(r), o); return r; }
        flipX() { var r = clone(this, DOMMatrix); var t = identity(); t[0] = -1; mx(r).m = mul(mx(r).m, t); return r; }
        flipY() { var r = clone(this, DOMMatrix); var t = identity(); t[5] = -1; mx(r).m = mul(mx(r).m, t); return r; }
        inverse() { var r = clone(this, DOMMatrix); ops.invert(mx(r)); return r; }
        transformPoint(p) {
            if (!(this instanceof DOMMatrixReadOnly)) throw new TypeError('Illegal invocation');
            var o = dictOf(p, 'point');
            return new DOMPoint(o.x, o.y, o.z, o.w).matrixTransform(this);
        }
        toFloat32Array() { return new Float32Array(mx(this).m); }
        toFloat64Array() { return new Float64Array(mx(this).m); }
        toString() {
            var v = mx(this), m = v.m;
            for (var i = 0; i < 16; i++) if (!isFinite(m[i])) {
                var e = global.DOMException ? new global.DOMException('A non-finite value cannot be serialized', 'InvalidStateError') : new Error('non-finite');
                throw e;
            }
            if (v.is2D) return 'matrix(' + [m[0], m[1], m[4], m[5], m[12], m[13]].map(ser).join(', ') + ')';
            return 'matrix3d(' + m.map(ser).join(', ') + ')';
        }
        toJSON() {
            if (!(this instanceof DOMMatrixReadOnly)) throw new TypeError('Illegal invocation');
            var o = {}, t = this;
            ['a', 'b', 'c', 'd', 'e', 'f', 'm11', 'm12', 'm13', 'm14', 'm21', 'm22', 'm23', 'm24', 'm31', 'm32', 'm33', 'm34', 'm41', 'm42', 'm43', 'm44', 'is2D', 'isIdentity'].forEach(function (k) { o[k] = t[k]; });
            return o;
        }
    }
    function arrayMatrix(Cls, a) {
        var r = new Cls();
        MX.set(r, fromSeq(a));
        return r;
    }
    class DOMMatrix extends DOMMatrixReadOnly {
        constructor(init) { super(init); }
        static fromMatrix(other) {
            var d = fixup(dictOf(other, 'other')), r = new DOMMatrix();
            MX.set(r, { m: matrixOf(d), is2D: d.is2D });
            return r;
        }
        static fromFloat32Array(a) {
            if (!(a instanceof Float32Array)) throw new TypeError('not a Float32Array');
            return arrayMatrix(DOMMatrix, a);
        }
        static fromFloat64Array(a) {
            if (!(a instanceof Float64Array)) throw new TypeError('not a Float64Array');
            return arrayMatrix(DOMMatrix, a);
        }
        multiplySelf(o) { ops.multiply(mx(this), o); return this; }
        preMultiplySelf(o) { ops.preMultiply(mx(this), o); return this; }
        translateSelf(x, y, z) { ops.translate(mx(this), x, y, z); return this; }
        scaleSelf(a, b, c, d, e, f) { ops.scale(mx(this), a, b, c, d, e, f); return this; }
        scale3dSelf(s, x, y, z) { ops.scale3d(mx(this), s, x, y, z); return this; }
        rotateSelf(x, y, z) { ops.rotate(mx(this), x, y, z); return this; }
        rotateFromVectorSelf(x, y) { ops.rotateFromVector(mx(this), x, y); return this; }
        rotateAxisAngleSelf(x, y, z, a) { ops.rotateAxisAngle(mx(this), x, y, z, a); return this; }
        skewXSelf(s) { ops.skewX(mx(this), s); return this; }
        skewYSelf(s) { ops.skewY(mx(this), s); return this; }
        invertSelf() { ops.invert(mx(this)); return this; }
        setMatrixValue(text) {
            var r = fromString(text);
            MX.set(this, r);
            return this;
        }
    }
    /* (its mutable attributes: a to f, m11 to m44) */
    [['a', 0], ['b', 1], ['c', 4], ['d', 5], ['e', 12], ['f', 13], ['m11', 0], ['m12', 1], ['m13', 2], ['m14', 3], ['m21', 4], ['m22', 5], ['m23', 6], ['m24', 7],
     ['m31', 8], ['m32', 9], ['m33', 10], ['m34', 11], ['m41', 12], ['m42', 13], ['m43', 14], ['m44', 15]].forEach(function (p) {
        var name = p[0], i = p[1], d3 = name[0] === 'm' && !/^m(11|12|21|22|41|42)$/.test(name);
        var one = name === 'm33' || name === 'm44';
        var getter = function () { return mx(this).m[i]; };
        var setter = function (v) {
            var e = mx(this), n = Number(v);
            e.m[i] = n;
            if (d3 && !(n === (one ? 1 : 0))) e.is2D = false;
        };
        Object.defineProperty(getter, 'name', { configurable: true, value: 'get ' + name });
        Object.defineProperty(setter, 'name', { configurable: true, value: 'set ' + name });
        Object.defineProperty(DOMMatrix.prototype, name, { configurable: true, enumerable: true, get: getter, set: setter });
    });

    /* ---- DOMQuad ---- */
    var QD = new WeakMap();
    function qd(self) {
        var q = QD.get(self);
        if (!q) throw new TypeError('Illegal invocation');
        return q;
    }
    function ptIn(o) {
        o = dictOf(o, 'point');
        return new DOMPoint(o.x, o.y, o.z, o.w);
    }
    class DOMQuad {
        constructor(p1, p2, p3, p4) { QD.set(this, [ptIn(p1), ptIn(p2), ptIn(p3), ptIn(p4)]); }
        static fromRect(other) {
            var o = dictOf(other, 'other');
            var x = o.x === undefined ? 0 : Number(o.x), y = o.y === undefined ? 0 : Number(o.y);
            var w = o.width === undefined ? 0 : Number(o.width), h = o.height === undefined ? 0 : Number(o.height);
            return new DOMQuad({ x: x, y: y }, { x: x + w, y: y }, { x: x + w, y: y + h }, { x: x, y: y + h });
        }
        static fromQuad(other) {
            var o = dictOf(other, 'other');
            return new DOMQuad(o.p1, o.p2, o.p3, o.p4);
        }
        get p1() { return qd(this)[0]; }
        get p2() { return qd(this)[1]; }
        get p3() { return qd(this)[2]; }
        get p4() { return qd(this)[3]; }
        getBounds() {
            var q = qd(this);
            var xs = q.map(function (p) { return p.x; }), ys = q.map(function (p) { return p.y; });
            var nan = function (a) { return a.some(function (v) { return v !== v; }); };
            var x0 = nan(xs) ? NaN : Math.min.apply(null, xs), x1 = nan(xs) ? NaN : Math.max.apply(null, xs);
            var y0 = nan(ys) ? NaN : Math.min.apply(null, ys), y1 = nan(ys) ? NaN : Math.max.apply(null, ys);
            return new global.DOMRect(x0, y0, x1 - x0, y1 - y0);
        }
        toJSON() {
            if (!(this instanceof DOMQuad)) throw new TypeError('Illegal invocation');
            return { p1: this.p1, p2: this.p2, p3: this.p3, p4: this.p4 };
        }
    }
    idl(DOMPointReadOnly, 'DOMPointReadOnly');
    idl(DOMPoint, 'DOMPoint');
    idl(DOMMatrixReadOnly, 'DOMMatrixReadOnly');
    idl(DOMMatrix, 'DOMMatrix');
    idl(DOMQuad, 'DOMQuad');
    /* (the window's legacy names; a worker has none, nor setMatrixValue) */
    if (global.document) {
        expose('SVGMatrix', DOMMatrix);
        expose('WebKitCSSMatrix', DOMMatrix);
        expose('SVGPoint', DOMPoint);
        if (global.DOMRect && !Object.prototype.hasOwnProperty.call(global, 'SVGRect')) expose('SVGRect', global.DOMRect);
    } else {
        delete DOMMatrix.prototype.setMatrixValue;
    }
})

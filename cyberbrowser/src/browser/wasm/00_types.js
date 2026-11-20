/*
 * wasm.js - WebAssembly (the core specification 3.0 and its JavaScript and
 * Web APIs).  Installed in each realm by platform_api_install
 * (browser/platform_api.c), called as fn(global, window).  Made by
 * tools/gen_wasm_js.js from the parts in browser/wasm/, in their order.
 *
 * A module is decoded and validated in one pass over its bytes, and each
 * function body compiled while it is validated into JavaScript source: one
 * JS function per Wasm function, its locals and operand stack slots as JS
 * variables, block / loop / if as labelled statements, br as break or
 * continue, Wasm exceptions as JS ones.  Values in the generated code:
 *   i32   a number (|0)
 *   i64   a BigInt (asIntN 64)
 *   f32   a number (Math.fround), or an F32NaN box for a NaN other than
 *         the canonical one: the bits are kept by the operations that
 *         must keep them (neg, abs, copysign, reinterpret, load, store)
 *   f64   a number, or an F64NaN box likewise
 *   v128  an Int32Array of 4
 *   refs  null, a FuncInst, a WasmStruct / WasmArray, an i31 as a number,
 *         a HostRef (a JS value made internal), an ExnRef, or for
 *         externref any JS value
 * A box's valueOf is NaN, so arithmetic on it needs no test.
 */
(function (global, win) {
'use strict';

/* ---- errors ---- */
function makeError(name) {
    function E(message) {
        if (!new.target) return new E(message);
        var e = Reflect.construct(Error, message === undefined ? [] : [message], new.target);
        return e;
    }
    Object.setPrototypeOf(E, Error);
    E.prototype = Object.create(Error.prototype, {
        constructor: { value: E, writable: true, configurable: true },
        name: { value: name, writable: true, configurable: true },
        message: { value: '', writable: true, configurable: true }
    });
    Object.defineProperty(E, 'name', { value: name });
    Object.defineProperty(E, 'prototype', { writable: false, enumerable: false, configurable: false });
    return E;
}
var CompileError = makeError('CompileError');
var LinkError = makeError('LinkError');
var RuntimeError = makeError('RuntimeError');
function CE(msg) { return new CompileError(msg); }
function trap(msg) { throw new RuntimeError(msg); }

/* ---- float bits ---- */
var F64B = new Float64Array(1), F64U = new Uint32Array(F64B.buffer), F32B = new Float32Array(1), F32U = new Uint32Array(F32B.buffer);
var BIG64 = new BigInt64Array(F64B.buffer);
/* (a NaN other than the canonical one, its bits kept) */
function F32NaN(bits) { this.b = bits | 0; }
F32NaN.prototype.valueOf = function () { return NaN; };
F32NaN.prototype.toString = function () { return 'NaN'; };
function F64NaN(hi, lo) { this.hi = hi | 0; this.lo = lo | 0; }
F64NaN.prototype.valueOf = function () { return NaN; };
F64NaN.prototype.toString = function () { return 'NaN'; };
var CANON32 = 0x7fc00000, CANON64HI = 0x7ff80000;
/* (from bits: the value) */
function f32FromBits(b) {
    b |= 0;
    if ((b & 0x7f800000) === 0x7f800000 && (b & 0x7fffff) !== 0) return b === CANON32 ? NaN : new F32NaN(b);
    F32U[0] = b;
    return F32B[0];
}
function f32Bits(v) {
    if (typeof v !== 'number') return v.b;
    if (v !== v) return CANON32;
    F32B[0] = v;
    return F32U[0] | 0;
}
function f64FromBits(hi, lo) {
    hi |= 0; lo |= 0;
    if ((hi & 0x7ff00000) === 0x7ff00000 && ((hi & 0xfffff) !== 0 || lo !== 0)) return hi === CANON64HI && lo === 0 ? NaN : new F64NaN(hi, lo);
    F64U[0] = lo; F64U[1] = hi;
    return F64B[0];
}
function f64Hi(v) {
    if (typeof v !== 'number') return v.hi;
    if (v !== v) return CANON64HI;
    F64B[0] = v;
    return F64U[1] | 0;
}
function f64Lo(v) {
    if (typeof v !== 'number') return v.lo;
    if (v !== v) return 0;
    F64B[0] = v;
    return F64U[0] | 0;
}
function f64FromI64(b) {
    BIG64[0] = b;
    return f64FromBits(F64U[1], F64U[0]);
}
function i64FromF64(v) {
    F64U[0] = f64Lo(v); F64U[1] = f64Hi(v);
    return BIG64[0];
}

/* ---- the binary reader ---- */
function Reader(bytes, pos, end) {
    this.b = bytes;
    this.p = pos || 0;
    this.e = end === undefined ? bytes.length : end;
}
Reader.prototype = {
    eof: function () { return this.p >= this.e; },
    u8: function () {
        if (this.p >= this.e) throw CE('unexpected end');
        return this.b[this.p++];
    },
    peek: function () {
        if (this.p >= this.e) throw CE('unexpected end');
        return this.b[this.p];
    },
    /* (an unsigned LEB128 of n bits: at most ceil(n / 7) bytes, unused bits of the last zero) */
    uN: function (n) {
        var r = 0, shift = 0, maxb = Math.ceil(n / 7);
        for (var i = 0; ; i++) {
            if (i >= maxb) throw CE('integer representation too long');
            var x = this.u8();
            if (i === maxb - 1) {
                var used = n - 7 * i;
                if (x & 0x80) throw CE('integer representation too long');
                if (x >> used) throw CE('integer too large');
            }
            r += (x & 0x7f) * Math.pow(2, shift);
            shift += 7;
            if (!(x & 0x80)) return r;
        }
    },
    u32: function () { return this.uN(32); },
    /* (a signed LEB128 of n <= 33 bits, as a number) */
    sN: function (n) {
        var r = 0, shift = 0, maxb = Math.ceil(n / 7), x;
        for (var i = 0; ; i++) {
            if (i >= maxb) throw CE('integer representation too long');
            x = this.u8();
            if (i === maxb - 1) {
                if (x & 0x80) throw CE('integer representation too long');
                var used = n - 7 * i;   /* (bits of the value in this byte) */
                var rest = x >> (used - 1);   /* (the sign bit and those above it: all the same) */
                if (rest !== 0 && rest !== (0x7f >> (used - 1))) throw CE('integer too large');
            }
            r += (x & 0x7f) * Math.pow(2, shift);
            shift += 7;
            if (!(x & 0x80)) break;
        }
        if (shift < 64 && (x & 0x40)) r -= Math.pow(2, shift);
        return r;
    },
    s32: function () { return this.sN(32) | 0; },
    s33: function () { return this.sN(33); },
    /* (64 bits: a BigInt) */
    s64: function () {
        var r = 0n, shift = 0n, x;
        for (var i = 0; ; i++) {
            if (i >= 10) throw CE('integer representation too long');
            x = this.u8();
            if (i === 9) {
                if (x & 0x80) throw CE('integer representation too long');
                if (x !== 0 && x !== 0x7f) throw CE('integer too large');
            }
            r |= BigInt(x & 0x7f) << shift;
            shift += 7n;
            if (!(x & 0x80)) break;
        }
        if (shift < 64n && (x & 0x40)) r -= 1n << shift;
        return BigInt.asIntN(64, r);
    },
    u64: function () {
        var r = 0n, shift = 0n, x;
        for (var i = 0; ; i++) {
            if (i >= 10) throw CE('integer representation too long');
            x = this.u8();
            if (i === 9) {
                if (x & 0x80) throw CE('integer representation too long');
                if (x > 1) throw CE('integer too large');
            }
            r |= BigInt(x & 0x7f) << shift;
            shift += 7n;
            if (!(x & 0x80)) break;
        }
        return r;
    },
    f32: function () {
        if (this.p + 4 > this.e) throw CE('unexpected end');
        var b = this.b, p = this.p;
        this.p += 4;
        return f32FromBits(b[p] | (b[p + 1] << 8) | (b[p + 2] << 16) | (b[p + 3] << 24));
    },
    f64: function () {
        if (this.p + 8 > this.e) throw CE('unexpected end');
        var b = this.b, p = this.p;
        this.p += 8;
        return f64FromBits(b[p + 4] | (b[p + 5] << 8) | (b[p + 6] << 16) | (b[p + 7] << 24), b[p] | (b[p + 1] << 8) | (b[p + 2] << 16) | (b[p + 3] << 24));
    },
    bytes: function (n) {
        if (n > this.e - this.p) throw CE('unexpected end');
        var r = this.b.subarray(this.p, this.p + n);
        this.p += n;
        return r;
    },
    name: function () {
        var n = this.u32();
        return utf8Decode(this.bytes(n));
    },
    vec: function (f) {
        var n = this.u32(), out = [];
        if (n > this.e - this.p) throw CE('length out of bounds');
        for (var i = 0; i < n; i++) out.push(f.call(this, i));
        return out;
    }
};
/* (UTF-8, well formed or a CompileError: "malformed UTF-8 encoding") */
function utf8Decode(u) {
    var s = '', i = 0, n = u.length;
    while (i < n) {
        var c = u[i++], cp, need, min;
        if (c < 0x80) { s += String.fromCharCode(c); continue; }
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 0x1f; need = 1; min = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 0x0f; need = 2; min = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 0x07; need = 3; min = 0x10000; }
        else throw CE('malformed UTF-8 encoding');
        if (i + need > n) throw CE('malformed UTF-8 encoding');
        for (var k = 0; k < need; k++) {
            var d = u[i++];
            if ((d & 0xc0) !== 0x80) throw CE('malformed UTF-8 encoding');
            cp = (cp << 6) | (d & 0x3f);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) throw CE('malformed UTF-8 encoding');
        s += String.fromCodePoint(cp);
    }
    return s;
}

/* ---- types ----
 * A value type: 'i32' 'i64' 'f32' 'f64' 'v128', or a reference type
 * {n: nullable, h: heap type}; a heap type: an abstract one (a negative
 * number, the byte - 0x80) or a defined one by its canonical id (>= 0).
 * BOT: the type of a value on an unreachable stack, any type. */
var H_FUNC = -0x10, H_EXTERN = -0x11, H_ANY = -0x12, H_EQ = -0x13, H_I31 = -0x14, H_STRUCT = -0x15, H_ARRAY = -0x16,
    H_EXN = -0x17, H_NONE = -0x0f, H_NOEXTERN = -0x0e, H_NOFUNC = -0x0d, H_NOEXN = -0x0c;
var BOT = 'bot';
var ABSTRACT_NAMES = {};
ABSTRACT_NAMES[H_FUNC] = 'func'; ABSTRACT_NAMES[H_EXTERN] = 'extern'; ABSTRACT_NAMES[H_ANY] = 'any'; ABSTRACT_NAMES[H_EQ] = 'eq';
ABSTRACT_NAMES[H_I31] = 'i31'; ABSTRACT_NAMES[H_STRUCT] = 'struct'; ABSTRACT_NAMES[H_ARRAY] = 'array'; ABSTRACT_NAMES[H_EXN] = 'exn';
ABSTRACT_NAMES[H_NONE] = 'none'; ABSTRACT_NAMES[H_NOEXTERN] = 'noextern'; ABSTRACT_NAMES[H_NOFUNC] = 'nofunc'; ABSTRACT_NAMES[H_NOEXN] = 'noexn';
function isAbstractByte(b) { return b >= 0x69 && b <= 0x74; }
function ref(n, h) { return { n: n, h: h }; }
var FUNCREF = ref(true, H_FUNC), EXTERNREF = ref(true, H_EXTERN), ANYREF = ref(true, H_ANY), EXNREF = ref(true, H_EXN);
function isRef(t) { return typeof t === 'object' && t !== null; }
function isNum(t) { return t === 'i32' || t === 'i64' || t === 'f32' || t === 'f64'; }

/* Canonical types (iso-recursive, shared by the modules of this realm):
 * CT[id] = {kind: 'func'|'struct'|'array', params, results | fields,
 * final, sup: id or -1, depth, group: first id of its rec group, gsize} */
var CT = [];
var CANON = new Map();

/* (the top of a heap type's hierarchy) */
function topOf(h) {
    if (h >= 0) { var k = CT[h].kind; return k === 'func' ? H_FUNC : H_ANY; }
    switch (h) {
    case H_FUNC: case H_NOFUNC: return H_FUNC;
    case H_EXTERN: case H_NOEXTERN: return H_EXTERN;
    case H_EXN: case H_NOEXN: return H_EXN;
    default: return H_ANY;
    }
}
function bottomOf(h) {
    var t = topOf(h);
    return t === H_FUNC ? H_NOFUNC : t === H_EXTERN ? H_NOEXTERN : t === H_EXN ? H_NOEXN : H_NONE;
}
function heapSub(a, b) {
    if (a === b) return true;
    if (a >= 0) {
        if (b >= 0) {
            for (var x = CT[a].sup; x >= 0; x = CT[x].sup) if (x === b) return true;
            return false;
        }
        var k = CT[a].kind;
        if (k === 'func') return b === H_FUNC;
        if (k === 'struct') return b === H_STRUCT || b === H_EQ || b === H_ANY;
        return b === H_ARRAY || b === H_EQ || b === H_ANY;
    }
    switch (a) {
    case H_NONE: return b === H_NONE || topOf(b) === H_ANY;
    case H_NOFUNC: return topOf(b) === H_FUNC;
    case H_NOEXTERN: return topOf(b) === H_EXTERN;
    case H_NOEXN: return topOf(b) === H_EXN;
    case H_I31: case H_STRUCT: case H_ARRAY: return b === H_EQ || b === H_ANY;
    case H_EQ: return b === H_ANY;
    }
    return false;
}
function valSub(a, b) {
    if (a === BOT) return true;
    if (b === BOT) return false;
    if (isRef(a)) {
        if (!isRef(b)) return false;
        if (a.n && !b.n) return false;
        return heapSub(a.h, b.h);
    }
    return a === b;
}
function valEq(a, b) {
    if (isRef(a)) return isRef(b) && a.n === b.n && a.h === b.h;
    return a === b;
}
function typesSub(as, bs) {
    if (as.length !== bs.length) return false;
    for (var i = 0; i < as.length; i++) if (!valSub(as[i], bs[i])) return false;
    return true;
}
function storageUnpack(t) { return t === 'i8' || t === 'i16' ? 'i32' : t; }
function fieldSub(a, b) {
    if (a.mut !== b.mut) return false;
    if (a.mut) return a.t === b.t || (isRef(a.t) && isRef(b.t) && valEq(a.t, b.t));
    if (a.t === 'i8' || a.t === 'i16' || b.t === 'i8' || b.t === 'i16') return a.t === b.t;
    return valSub(a.t, b.t);
}
/* (comptype a matches b: the subtyping a declared supertype must allow) */
function compSub(a, b) {
    if (a.kind !== b.kind) return false;
    if (a.kind === 'func') return typesSub(b.params, a.params) && typesSub(a.results, b.results);
    if (a.kind === 'array') return fieldSub(a.field, b.field);
    if (a.fields.length < b.fields.length) return false;
    for (var i = 0; i < b.fields.length; i++) if (!fieldSub(a.fields[i], b.fields[i])) return false;
    return true;
}
function defaultable(t) { return !isRef(t) || t.n; }

/* (a rec group read with module type indices, made canonical: its first canonical id; types are mapped through
 * canonOf for the indices before the group, relative within it) */
function vtKey(t, g0, n, canonOf) {
    if (!isRef(t)) return t;
    var h = t.h;
    var hk = h < 0 ? 'a' + h : h >= g0 && h < g0 + n ? 'r' + (h - g0) : 'c' + canonOf[h];
    return (t.n ? 'N' : 'R') + hk;
}
function compKey(c, g0, n, canonOf) {
    var f = function (t) { return vtKey(t, g0, n, canonOf); };
    if (c.kind === 'func') return 'F(' + c.params.map(f).join(',') + '|' + c.results.map(f).join(',') + ')';
    if (c.kind === 'array') return 'A(' + (c.field.mut ? 'm' : 'c') + f(c.field.t) + ')';
    return 'S(' + c.fields.map(function (x) { return (x.mut ? 'm' : 'c') + f(x.t); }).join(',') + ')';
}
function canonicalize(group, g0, canonOf) {
    var n = group.length;
    var key = group.map(function (st) {
        var sk = st.sup < 0 ? '-' : st.sup >= g0 ? 'r' + (st.sup - g0) : 'c' + canonOf[st.sup];
        return (st.final ? 'f' : 'o') + sk + compKey(st, g0, n, canonOf);
    }).join(';');
    var base = CANON.get(key);
    if (base !== undefined) return base;
    base = CT.length;
    CANON.set(key, base);
    var mapT = function (t) {
        if (!isRef(t) || t.h < 0) return t;
        return ref(t.n, t.h >= g0 ? base + (t.h - g0) : canonOf[t.h]);
    };
    for (var i = 0; i < n; i++) {
        var st = group[i], c = { kind: st.kind, final: st.final, group: base, gsize: n, sup: -1, depth: 0 };
        if (st.kind === 'func') { c.params = st.params.map(mapT); c.results = st.results.map(mapT); }
        else if (st.kind === 'array') c.field = { t: mapT(st.field.t), mut: st.field.mut };
        else c.fields = st.fields.map(function (x) { return { t: mapT(x.t), mut: x.mut }; });
        c.sup = st.sup < 0 ? -1 : st.sup >= g0 ? base + (st.sup - g0) : canonOf[st.sup];
        CT.push(c);
    }
    for (var j = 0; j < n; j++) { var cc = CT[base + j]; cc.depth = cc.sup < 0 ? 0 : CT[cc.sup].depth + 1; }
    return base;
}

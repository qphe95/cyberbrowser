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


/* ---- modules: decoded, validated and compiled ----
 * M: {types (CT entries), typeCanon, funcs (canonical type id of each
 * function, imports first), nFuncImports, tables [{type, min, max, at,
 * init}], mems [{min, max, at, shared}], tags [{type}], globals [{type,
 * mut, init}], imports, exports, start, elems, datas, dataCount, refs,
 * customs, js (the source of the instance factory)} */
var SECTION_ORDER = { 1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 13: 6, 6: 7, 7: 8, 8: 9, 9: 10, 12: 11, 10: 12, 11: 13 };

function readLimits(r, kind) {
    var flags = r.u8();
    if (kind === 'table' ? flags > 5 || flags === 2 || flags === 3 : flags > 7) throw CE('malformed limits flags');
    var at = flags & 4 ? 'i64' : 'i32', shared = !!(flags & 2), hasMax = !!(flags & 1);
    var rd = at === 'i64' ? function () { return r.u64(); } : function () { return BigInt(r.u32()); };
    var min = rd(), max = hasMax ? rd() : null;
    return { min: min, max: max, at: at, shared: shared };
}
function checkLimits(l, kind) {
    var cap = kind === 'mem' ? (l.at === 'i64' ? 1n << 48n : 65536n) : (l.at === 'i64' ? (1n << 64n) - 1n : 0xffffffffn);
    if (l.min > cap) throw CE(kind === 'mem' ? 'memory size must be at most 65536 pages (4GiB)' : 'table size');
    if (l.max !== null && l.max > cap) throw CE(kind === 'mem' ? 'memory size must be at most 65536 pages (4GiB)' : 'table size');
    if (l.max !== null && l.max < l.min) throw CE('size minimum must not be greater than maximum');
    if (l.shared && l.max === null) throw CE('shared memory must have maximum');
}
function readTableType(r, M) {
    var t = readValType(r, M);
    if (!isRef(t)) throw CE('malformed reference type');
    var l = readLimits(r, 'table');
    checkLimits(l, 'table');
    return { type: t, min: l.min, max: l.max, at: l.at };
}
function readMemType(r) {
    var l = readLimits(r, 'mem');
    checkLimits(l, 'mem');
    return { min: l.min, max: l.max, at: l.at, shared: l.shared };
}
function readGlobalType(r, M) {
    var t = readValType(r, M), m = r.u8();
    if (m > 1) throw CE('malformed mutability');
    return { type: t, mut: m === 1 };
}
function readTagType(r, M) {
    if (r.u8() !== 0) throw CE('malformed tag attribute');
    var x = r.u32();
    if (x >= M.types.length) throw CE('unknown type');
    var c = M.typeCanon[x];
    if (CT[c].kind !== 'func') throw CE('type mismatch: tag type must be a function type');
    if (CT[c].results.length) throw CE('non-empty tag result type');
    return { type: c };
}

/* (a constant expression of type t: compiled to the source of a function of the instance's parts) */
function constExpr(r, M, t, nglobals) {
    var c = new FnCompiler(M, r, { konst: true, locals: [], nparams: 0, results: [t], nglobals: nglobals });
    c.pushFrame('func', [], [t]);
    while (true) {
        var op = r.u8();
        if (op === 0x0b && c.cs.length === 1) { c.popFrame(); break; }
        c.instr(op);
    }
    return '(function(){let ' + slotDecl(c) + 'r,ea,dv;' + c.out.join('') + 'return s0;})';
}
function slotDecl(c) {
    var a = [];
    for (var i = 0; i < Math.max(c.maxs, 1); i++) a.push('s' + i);
    for (var j = 0; j < c.ntmp; j++) a.push('t' + j);
    for (var k = 0; k < (c.needQ || 0); k++) a.push('q' + k);
    return a.join(',') + ',';
}

function decodeModule(bytes) {
    var r = new Reader(bytes);
    if (bytes.length < 4 || bytes[0] !== 0 || bytes[1] !== 0x61 || bytes[2] !== 0x73 || bytes[3] !== 0x6d) throw CE('magic header not detected');
    if (bytes.length < 8) throw CE('unexpected end');
    if (bytes[4] !== 1 || bytes[5] !== 0 || bytes[6] !== 0 || bytes[7] !== 0) throw CE('unknown binary version');
    r.p = 8;
    var M = { types: [], typeCanon: [], funcs: [], nFuncImports: 0, tables: [], mems: [], tags: [], globals: [], imports: [], exports: [],
              start: null, elems: [], datas: [], dataCount: undefined, refs: new Set(), customs: [], codes: [], nDefinedFuncs: 0,
              nTableImports: 0, nMemImports: 0, nGlobalImports: 0, nTagImports: 0 };
    var last = 0, funcDecls = null, codeCount = undefined;
    while (!r.eof()) {
        var id = r.u8(), size = r.u32();
        if (size > r.e - r.p) throw CE('section size mismatch: unexpected end');
        var sr = new Reader(bytes, r.p, r.p + size);
        r.p += size;
        if (id === 0) {
            var nm = sr.name();
            M.customs.push({ name: nm, bytes: bytes.slice(sr.p, sr.e) });
            continue;
        }
        var ord = SECTION_ORDER[id];
        if (!ord) throw CE('malformed section id');
        if (ord <= last) throw CE('unexpected content after last section');
        last = ord;
        switch (id) {
        case 1: readTypes(sr, M); break;
        case 2: readImports(sr, M); break;
        case 3:
            funcDecls = sr.vec(function () {
                var x = this.u32();
                if (x >= M.types.length) throw CE('unknown type');
                var c = M.typeCanon[x];
                if (CT[c].kind !== 'func') throw CE('type mismatch: function type expected');
                return c;
            });
            for (var i = 0; i < funcDecls.length; i++) M.funcs.push(funcDecls[i]);
            M.nDefinedFuncs = funcDecls.length;
            break;
        case 4:
            sr.vec(function () {
                var b = this.peek(), tt;
                if (b === 0x40) {
                    this.u8();
                    if (this.u8() !== 0) throw CE('malformed table');
                    tt = readTableType(this, M);
                    tt.init = constExpr(this, M, tt.type, M.globals.length);
                } else {
                    tt = readTableType(this, M);
                    if (!tt.type.n) throw CE('type mismatch: non-nullable table needs an initializer');
                }
                M.tables.push(tt);
            });
            break;
        case 5: sr.vec(function () { M.mems.push(readMemType(this)); }); break;
        case 13: sr.vec(function () { M.tags.push(readTagType(this, M)); }); break;
        case 6:
            sr.vec(function () {
                var gt = readGlobalType(this, M);
                gt.init = constExpr(this, M, gt.type, M.globals.length);
                M.globals.push(gt);
            });
            break;
        case 7: readExports(sr, M); break;
        case 8: {
            var sx = sr.u32();
            if (sx >= M.funcs.length) throw CE('unknown function');
            var st = CT[M.funcs[sx]];
            if (st.params.length || st.results.length) throw CE('start function');
            M.start = sx;
            break;
        }
        case 9: readElems(sr, M); break;
        case 12: M.dataCount = sr.u32(); break;
        case 10: {
            if ((funcDecls ? funcDecls.length : 0) !== (codeCount = sr.u32())) throw CE('function and code section have inconsistent lengths');
            if (codeCount > sr.e - sr.p) throw CE('unexpected end');
            for (var k = 0; k < codeCount; k++) {
                var sz = sr.u32();
                if (sz > sr.e - sr.p) throw CE('unexpected end');
                var cr = new Reader(bytes, sr.p, sr.p + sz);
                sr.p += sz;
                M.codes.push(compileFunction(M, M.nFuncImports + k, cr));
            }
            break;
        }
        case 11: readDatas(sr, M); break;
        }
        if (!sr.eof()) throw CE('section size mismatch');
    }
    if (funcDecls && funcDecls.length && codeCount === undefined) throw CE('function and code section have inconsistent lengths');
    if (M.dataCount !== undefined && M.dataCount !== M.datas.length) throw CE('data count and data section have inconsistent lengths');
    if (M.mems.length > 1 && false) throw CE('multiple memories');
    M.js = buildFactory(M);
    return M;
}

function readTypes(r, M) {
    var n = r.u32();
    for (var gi = 0; gi < n; gi++) {
        var group;
        var g0 = M.types.length;
        if (r.peek() === 0x4e) {
            r.u8();
            var cnt = r.u32();
            group = [];
            for (var i = 0; i < cnt; i++) group.push(readSubType(r, M, g0, g0 + cnt, g0 + i));
        } else group = [readSubType(r, M, g0, g0 + 1, g0)];
        var base = canonicalize(group, g0, M.typeCanon);
        for (var j = 0; j < group.length; j++) {
            M.typeCanon.push(base + j);
            M.types.push(CT[base + j]);
        }
        /* (declared supertypes: not final, the definitions matching) */
        for (j = 0; j < group.length; j++) {
            var c = CT[base + j];
            if (c.sup >= 0) {
                var sc = CT[c.sup];
                if (sc.final) throw CE('sub type: supertype is final');
                if (!compSub(c, sc)) throw CE('sub type: does not match its supertype');
            }
        }
    }
}
/* (a subtype, its type indices as module indices below limit; self is its own index) */
function readSubType(r, M, g0, limit, self) {
    var b = r.u8(), fin = true, sup = -1;
    if (b === 0x50 || b === 0x4f) {
        fin = b === 0x4f;
        var sups = r.vec(function () { return this.u32(); });
        if (sups.length > 1) throw CE('multiple supertypes');
        if (sups.length) {
            sup = sups[0];
            if (sup >= self) throw CE('unknown type: supertype must be defined before');
        }
        b = r.u8();
    }
    var rt = function () {
        var t = readValTypeRaw(r, limit);
        return t;
    };
    var st;
    if (b === 0x60) {
        var ps = r.vec(rt), rs = r.vec(rt);
        st = { kind: 'func', params: ps, results: rs };
    } else if (b === 0x5f) {
        st = { kind: 'struct', fields: r.vec(function () { return readFieldRaw(r, limit); }) };
    } else if (b === 0x5e) {
        st = { kind: 'array', field: readFieldRaw(r, limit) };
    } else throw CE('malformed type');
    st.final = fin;
    st.sup = sup;
    return st;
}
function readFieldRaw(r, limit) {
    var b = r.peek(), t;
    if (b === 0x78) { r.u8(); t = 'i8'; }
    else if (b === 0x77) { r.u8(); t = 'i16'; }
    else t = readValTypeRaw(r, limit);
    var m = r.u8();
    if (m > 1) throw CE('malformed mutability');
    return { t: t, mut: m === 1 };
}
/* (in the type section: heap types as module indices) */
function readValTypeRaw(r, limit) {
    var b = r.u8();
    switch (b) {
    case 0x7f: return 'i32';
    case 0x7e: return 'i64';
    case 0x7d: return 'f32';
    case 0x7c: return 'f64';
    case 0x7b: return 'v128';
    case 0x63: case 0x64: {
        var p = r.peek(), h;
        if (isAbstractByte(p)) { r.u8(); h = p - 0x80; }
        else {
            h = r.s33();
            if (h < 0) throw CE('malformed heap type');
            if (h >= limit) throw CE('unknown type');
        }
        return ref(b === 0x63, h);
    }
    }
    if (isAbstractByte(b)) return ref(true, b - 0x80);
    throw CE('malformed value type');
}

function readImports(r, M) {
    r.vec(function () {
        var mod = this.name(), nm = this.name(), kind = this.u8(), desc;
        switch (kind) {
        case 0: {
            var x = this.u32();
            if (x >= M.types.length) throw CE('unknown type');
            var c = M.typeCanon[x];
            if (CT[c].kind !== 'func') throw CE('type mismatch: function type expected');
            M.funcs.push(c); M.nFuncImports++;
            desc = { type: c };
            break;
        }
        case 1: desc = readTableType(this, M); M.tables.push(desc); M.nTableImports++; break;
        case 2: desc = readMemType(this); M.mems.push(desc); M.nMemImports++; break;
        case 3: desc = readGlobalType(this, M); desc.imported = true; M.globals.push(desc); M.nGlobalImports++; break;
        case 4: desc = readTagType(this, M); M.tags.push(desc); M.nTagImports++; break;
        default: throw CE('malformed import kind');
        }
        M.imports.push({ module: mod, name: nm, kind: ['function', 'table', 'memory', 'global', 'tag'][kind], desc: desc });
    });
}
function readExports(r, M) {
    var seen = new Set();
    r.vec(function () {
        var nm = this.name(), kind = this.u8(), x = this.u32(), lim;
        if (seen.has(nm)) throw CE('duplicate export name');
        seen.add(nm);
        switch (kind) {
        case 0: lim = M.funcs.length; M.refs.add(x); break;
        case 1: lim = M.tables.length; break;
        case 2: lim = M.mems.length; break;
        case 3: lim = M.globals.length; break;
        case 4: lim = M.tags.length; break;
        default: throw CE('malformed export kind');
        }
        if (x >= lim) throw CE('unknown ' + ['function', 'table', 'memory', 'global', 'tag'][kind]);
        M.exports.push({ name: nm, kind: ['function', 'table', 'memory', 'global', 'tag'][kind], index: x });
    });
}
function readElems(r, M) {
    r.vec(function () {
        var flags = this.u32(), seg = { mode: 'passive', table: 0, offset: null, type: null, items: [] };
        if (flags > 7) throw CE('malformed elements segment kind');
        var active = !(flags & 1), explicitTable = !!(flags & 2), exprs = !!(flags & 4);
        if (active) {
            seg.mode = 'active';
            if (explicitTable) seg.table = this.u32();
            if (seg.table >= M.tables.length) throw CE('unknown table ' + seg.table);
            seg.offset = constExpr(this, M, M.tables[seg.table].at, M.globals.length);
        } else seg.mode = explicitTable ? 'declarative' : 'passive';
        if (flags === 0 || flags === 4) seg.type = flags === 4 ? FUNCREF : ref(false, H_FUNC);
        else if (!exprs) {
            if (this.u8() !== 0) throw CE('malformed element kind');
            seg.type = ref(false, H_FUNC);
        } else {
            seg.type = readValType(this, M);
            if (!isRef(seg.type)) throw CE('malformed reference type');
        }
        if (!exprs) {
            seg.items = this.vec(function () {
                var x = this.u32();
                if (x >= M.funcs.length) throw CE('unknown function ' + x);
                M.refs.add(x);
                return '(function(){return F[' + x + '];})';
            });
        } else {
            seg.items = this.vec(function () { return constExpr(this, M, seg.type, M.globals.length); });
        }
        if (seg.mode === 'active' && !valSub(seg.type, M.tables[seg.table].type)) throw CE('type mismatch: element segment');
        M.elems.push(seg);
    });
}
function readDatas(r, M) {
    r.vec(function () {
        var flags = this.u32(), seg = { mode: 'passive', mem: 0, offset: null, bytes: null };
        if (flags > 2) throw CE('malformed data segment kind');
        if (flags !== 1) {
            seg.mode = 'active';
            if (flags === 2) seg.mem = this.u32();
            if (seg.mem >= M.mems.length) throw CE('unknown memory ' + seg.mem);
            seg.offset = constExpr(this, M, M.mems[seg.mem].at, M.globals.length);
        }
        var n = this.u32();
        seg.bytes = this.bytes(n).slice();
        M.datas.push(seg);
    });
}

function compileFunction(M, fi, r) {
    var ft = CT[M.funcs[fi]];
    var locals = ft.params.slice();
    var groups = r.vec(function () {
        var n = this.u32(), t = readValType(this, M);
        return [n, t];
    });
    var total = 0;
    for (var i = 0; i < groups.length; i++) {
        total += groups[i][0];
        if (total > 50000) throw CE('too many locals');
        for (var k = 0; k < groups[i][0]; k++) locals.push(groups[i][1]);
    }
    var c = new FnCompiler(M, r, { konst: false, locals: locals, nparams: ft.params.length, results: ft.results });
    c.pushFrame('func', [], ft.results);
    while (true) {
        var op = r.u8();
        if (op === 0x0b && c.cs.length === 1) {
            var dead = c.deadNow();
            c.popFrame();
            if (!dead) {
                var rs = ft.results.length;
                c.out.push(rs === 0 ? 'return;' : rs === 1 ? 'return s0;' : 'return [' + ft.results.map(function (_, j) { return 's' + j; }).join(',') + '];');
            }
            break;
        }
        c.instr(op);
    }
    if (!r.eof()) throw CE('section size mismatch: operators remaining after end of function');
    var params = [];
    for (i = 0; i < ft.params.length; i++) params.push('l' + i);
    var decl = 'let ' + slotDecl(c) + 'r,ea,dv';
    for (i = ft.params.length; i < locals.length; i++) decl += ',l' + i + '=' + vtDefault(locals[i]);
    return 'function f' + fi + '(' + params.join(',') + '){' + decl + ';' + c.out.join('') + '}';
}

/* (the instance factory's source: (R, I) => the instance's functions and constant evaluators) */
function buildFactory(M) {
    var src = '"use strict";const F=I.funcs,G=I.globals,TG=I.tags,D=I.datas,E=I.elems,TC=R.TC,AI=BigInt.asIntN,AU=BigInt.asUintN;';
    for (var t = 0; t < M.tables.length; t++) src += 'const T' + t + '=I.tables[' + t + '];';
    for (var m = 0; m < M.mems.length; m++) src += 'const MEM' + m + '=I.mems[' + m + '];';
    src += M.codes.join('\n');
    var fl = [];
    for (var i = M.nFuncImports; i < M.funcs.length; i++) fl.push('f' + i);
    src += '\nreturn {funcs:[' + fl.join(',') + '],';
    src += 'globals:[' + M.globals.map(function (g) { return g.init || 'null'; }).join(',') + '],';
    src += 'tables:[' + M.tables.map(function (tb) { return tb.init || 'null'; }).join(',') + '],';
    src += 'elems:[' + M.elems.map(function (e) { return '{offset:' + (e.offset || 'null') + ',items:[' + e.items.join(',') + ']}'; }).join(',') + '],';
    src += 'datas:[' + M.datas.map(function (d) { return d.offset || 'null'; }).join(',') + ']};';
    return src;
}


/* ---- function bodies: validated and compiled to JS in one pass ----
 * The operand stack's slot i is the JS variable s<i>; local i is l<i>.  A
 * frame of the control stack: {op, params, results, h (height at entry,
 * below its params), dead (unreachable from here), deadIn (entered while
 * unreachable: nothing of it emitted), label, inits (locals initialized at
 * entry)}.  Code is emitted only where reachable. */
var NUMOPS = {};
(function () {
    function d(op, ins, out, js) { NUMOPS[op] = { ins: ins, out: out, js: js }; }
    var I = 'i32', L = 'i64', F = 'f32', D = 'f64';
    /* i32 tests and comparisons */
    d(0x45, [I], I, '($0===0?1:0)');
    d(0x46, [I, I], I, '($0===$1?1:0)'); d(0x47, [I, I], I, '($0!==$1?1:0)');
    d(0x48, [I, I], I, '($0<$1?1:0)'); d(0x49, [I, I], I, '(($0>>>0)<($1>>>0)?1:0)');
    d(0x4a, [I, I], I, '($0>$1?1:0)'); d(0x4b, [I, I], I, '(($0>>>0)>($1>>>0)?1:0)');
    d(0x4c, [I, I], I, '($0<=$1?1:0)'); d(0x4d, [I, I], I, '(($0>>>0)<=($1>>>0)?1:0)');
    d(0x4e, [I, I], I, '($0>=$1?1:0)'); d(0x4f, [I, I], I, '(($0>>>0)>=($1>>>0)?1:0)');
    /* i64 */
    d(0x50, [L], I, '($0===0n?1:0)');
    d(0x51, [L, L], I, '($0===$1?1:0)'); d(0x52, [L, L], I, '($0!==$1?1:0)');
    d(0x53, [L, L], I, '($0<$1?1:0)'); d(0x54, [L, L], I, '(AU(64,$0)<AU(64,$1)?1:0)');
    d(0x55, [L, L], I, '($0>$1?1:0)'); d(0x56, [L, L], I, '(AU(64,$0)>AU(64,$1)?1:0)');
    d(0x57, [L, L], I, '($0<=$1?1:0)'); d(0x58, [L, L], I, '(AU(64,$0)<=AU(64,$1)?1:0)');
    d(0x59, [L, L], I, '($0>=$1?1:0)'); d(0x5a, [L, L], I, '(AU(64,$0)>=AU(64,$1)?1:0)');
    /* f32 / f64 comparisons */
    var cmp = ['===', '!==', '<', '>', '<=', '>='];
    for (var k = 0; k < 6; k++) {
        d(0x5b + k, [F, F], I, '((+$0)' + cmp[k] + '(+$1)?1:0)');
        d(0x61 + k, [D, D], I, '((+$0)' + cmp[k] + '(+$1)?1:0)');
    }
    /* i32 arithmetic */
    d(0x67, [I], I, 'Math.clz32($0)'); d(0x68, [I], I, 'R.ctz32($0)'); d(0x69, [I], I, 'R.popcnt32($0)');
    d(0x6a, [I, I], I, '(($0+$1)|0)'); d(0x6b, [I, I], I, '(($0-$1)|0)'); d(0x6c, [I, I], I, 'Math.imul($0,$1)');
    d(0x6d, [I, I], I, 'R.divS32($0,$1)'); d(0x6e, [I, I], I, 'R.divU32($0,$1)');
    d(0x6f, [I, I], I, 'R.remS32($0,$1)'); d(0x70, [I, I], I, 'R.remU32($0,$1)');
    d(0x71, [I, I], I, '($0&$1)'); d(0x72, [I, I], I, '($0|$1)'); d(0x73, [I, I], I, '($0^$1)');
    d(0x74, [I, I], I, '($0<<$1)'); d(0x75, [I, I], I, '($0>>$1)'); d(0x76, [I, I], I, '(($0>>>$1)|0)');
    d(0x77, [I, I], I, '(($0<<($1&31))|($0>>>((32-($1&31))&31)))'); d(0x78, [I, I], I, '(($0>>>($1&31))|($0<<((32-($1&31))&31)))');
    /* i64 arithmetic */
    d(0x79, [L], L, 'R.clz64($0)'); d(0x7a, [L], L, 'R.ctz64($0)'); d(0x7b, [L], L, 'R.popcnt64($0)');
    d(0x7c, [L, L], L, 'AI(64,$0+$1)'); d(0x7d, [L, L], L, 'AI(64,$0-$1)'); d(0x7e, [L, L], L, 'AI(64,$0*$1)');
    d(0x7f, [L, L], L, 'R.divS64($0,$1)'); d(0x80, [L, L], L, 'R.divU64($0,$1)');
    d(0x81, [L, L], L, 'R.remS64($0,$1)'); d(0x82, [L, L], L, 'R.remU64($0,$1)');
    d(0x83, [L, L], L, '($0&$1)'); d(0x84, [L, L], L, '($0|$1)'); d(0x85, [L, L], L, '($0^$1)');
    d(0x86, [L, L], L, 'AI(64,$0<<($1&63n))'); d(0x87, [L, L], L, '($0>>($1&63n))'); d(0x88, [L, L], L, 'AI(64,AU(64,$0)>>($1&63n))');
    d(0x89, [L, L], L, 'R.rotl64($0,$1)'); d(0x8a, [L, L], L, 'R.rotr64($0,$1)');
    /* f32 arithmetic */
    d(0x8b, [F], F, 'R.fabs32($0)'); d(0x8c, [F], F, 'R.fneg32($0)');
    d(0x8d, [F], F, 'Math.fround(Math.ceil(+$0))'); d(0x8e, [F], F, 'Math.fround(Math.floor(+$0))');
    d(0x8f, [F], F, 'Math.fround(Math.trunc(+$0))'); d(0x90, [F], F, 'Math.fround(R.nearest(+$0))');
    d(0x91, [F], F, 'Math.fround(Math.sqrt(+$0))');
    d(0x92, [F, F], F, 'Math.fround((+$0)+(+$1))'); d(0x93, [F, F], F, 'Math.fround((+$0)-(+$1))');
    d(0x94, [F, F], F, 'Math.fround((+$0)*(+$1))'); d(0x95, [F, F], F, 'Math.fround((+$0)/(+$1))');
    d(0x96, [F, F], F, 'Math.fround(Math.min(+$0,+$1))'); d(0x97, [F, F], F, 'Math.fround(Math.max(+$0,+$1))');
    d(0x98, [F, F], F, 'R.fcopysign32($0,$1)');
    /* f64 arithmetic */
    d(0x99, [D], D, 'R.fabs64($0)'); d(0x9a, [D], D, 'R.fneg64($0)');
    d(0x9b, [D], D, 'Math.ceil(+$0)'); d(0x9c, [D], D, 'Math.floor(+$0)'); d(0x9d, [D], D, 'Math.trunc(+$0)');
    d(0x9e, [D], D, 'R.nearest(+$0)'); d(0x9f, [D], D, 'Math.sqrt(+$0)');
    d(0xa0, [D, D], D, '((+$0)+(+$1))'); d(0xa1, [D, D], D, '((+$0)-(+$1))'); d(0xa2, [D, D], D, '((+$0)*(+$1))');
    d(0xa3, [D, D], D, '((+$0)/(+$1))'); d(0xa4, [D, D], D, 'Math.min(+$0,+$1)'); d(0xa5, [D, D], D, 'Math.max(+$0,+$1)');
    d(0xa6, [D, D], D, 'R.fcopysign64($0,$1)');
    /* conversions */
    d(0xa7, [L], I, 'Number(AI(32,$0))');
    d(0xa8, [F], I, 'R.truncS32(+$0)'); d(0xa9, [F], I, 'R.truncU32(+$0)');
    d(0xaa, [D], I, 'R.truncS32(+$0)'); d(0xab, [D], I, 'R.truncU32(+$0)');
    d(0xac, [I], L, 'BigInt($0)'); d(0xad, [I], L, 'BigInt($0>>>0)');
    d(0xae, [F], L, 'R.truncS64(+$0)'); d(0xaf, [F], L, 'R.truncU64(+$0)');
    d(0xb0, [D], L, 'R.truncS64(+$0)'); d(0xb1, [D], L, 'R.truncU64(+$0)');
    d(0xb2, [I], F, 'Math.fround($0)'); d(0xb3, [I], F, 'Math.fround($0>>>0)');
    d(0xb4, [L], F, 'R.i64ToF32($0)'); d(0xb5, [L], F, 'R.i64ToF32(AU(64,$0))');
    d(0xb6, [D], F, 'Math.fround(+$0)');
    d(0xb7, [I], D, '($0)'); d(0xb8, [I], D, '($0>>>0)');
    d(0xb9, [L], D, 'Number($0)'); d(0xba, [L], D, 'Number(AU(64,$0))');
    d(0xbb, [F], D, '(+$0)');
    d(0xbc, [F], I, 'R.f32Bits($0)'); d(0xbd, [D], L, 'R.i64FromF64($0)');
    d(0xbe, [I], F, 'R.f32FromBits($0)'); d(0xbf, [L], D, 'R.f64FromI64($0)');
    /* sign extension */
    d(0xc0, [I], I, '(($0<<24)>>24)'); d(0xc1, [I], I, '(($0<<16)>>16)');
    d(0xc2, [L], L, 'AI(8,$0)'); d(0xc3, [L], L, 'AI(16,$0)'); d(0xc4, [L], L, 'AI(32,$0)');
})();
/* (0xFC 0-7: saturating truncation) */
var SATOPS = [
    ['f32', 'i32', 'R.satS32(+$0)'], ['f32', 'i32', 'R.satU32(+$0)'], ['f64', 'i32', 'R.satS32(+$0)'], ['f64', 'i32', 'R.satU32(+$0)'],
    ['f32', 'i64', 'R.satS64(+$0)'], ['f32', 'i64', 'R.satU64(+$0)'], ['f64', 'i64', 'R.satS64(+$0)'], ['f64', 'i64', 'R.satU64(+$0)']
];
/* (loads and stores: opcode -> [value type, natural alignment log2, JS kind]) */
var MEMOPS = {
    0x28: ['i32', 2, 'i32'], 0x29: ['i64', 3, 'i64'], 0x2a: ['f32', 2, 'f32'], 0x2b: ['f64', 3, 'f64'],
    0x2c: ['i32', 0, 'i8s'], 0x2d: ['i32', 0, 'i8u'], 0x2e: ['i32', 1, 'i16s'], 0x2f: ['i32', 1, 'i16u'],
    0x30: ['i64', 0, 'l8s'], 0x31: ['i64', 0, 'l8u'], 0x32: ['i64', 1, 'l16s'], 0x33: ['i64', 1, 'l16u'],
    0x34: ['i64', 2, 'l32s'], 0x35: ['i64', 2, 'l32u'],
    0x36: ['i32', 2, 'si32'], 0x37: ['i64', 3, 'si64'], 0x38: ['f32', 2, 'sf32'], 0x39: ['f64', 3, 'sf64'],
    0x3a: ['i32', 0, 'si8'], 0x3b: ['i32', 1, 'si16'], 0x3c: ['i64', 0, 'sl8'], 0x3d: ['i64', 1, 'sl16'], 0x3e: ['i64', 2, 'sl32']
};
var LOADJS = {
    i32: ['dv.getInt32(#,true)', 4], i64: ['dv.getBigInt64(#,true)', 8], f32: ['R.f32FromBits(dv.getInt32(#,true))', 4],
    f64: ['R.f64FromBits(dv.getInt32(#+4,true),dv.getInt32(#,true))', 8],
    i8s: ['dv.getInt8(#)', 1], i8u: ['dv.getUint8(#)', 1], i16s: ['dv.getInt16(#,true)', 2], i16u: ['dv.getUint16(#,true)', 2],
    l8s: ['BigInt(dv.getInt8(#))', 1], l8u: ['BigInt(dv.getUint8(#))', 1], l16s: ['BigInt(dv.getInt16(#,true))', 2],
    l16u: ['BigInt(dv.getUint16(#,true))', 2], l32s: ['BigInt(dv.getInt32(#,true))', 4], l32u: ['BigInt(dv.getUint32(#,true))', 4]
};
var STOREJS = {
    si32: ['dv.setInt32(#,$,true)', 4], si64: ['dv.setBigInt64(#,$,true)', 8], sf32: ['dv.setInt32(#,R.f32Bits($),true)', 4],
    sf64: ['dv.setInt32(#,R.f64Lo($),true);dv.setInt32(#+4,R.f64Hi($),true)', 8],
    si8: ['dv.setInt8(#,$)', 1], si16: ['dv.setInt16(#,$,true)', 2],
    sl8: ['dv.setInt8(#,Number(AI(8,$)))', 1], sl16: ['dv.setInt16(#,Number(AI(16,$)),true)', 2], sl32: ['dv.setInt32(#,Number(AI(32,$)),true)', 4]
};

function vtDefault(t) {
    if (t === 'i32' || t === 'f32' || t === 'f64' || t === 'i8' || t === 'i16') return '0';
    if (t === 'i64') return '0n';
    if (t === 'v128') return 'R.V0';
    return 'null';
}

/* The compiler of one function body (or, with cx.konst, a constant expression). */
function FnCompiler(M, r, cx) {
    this.M = M; this.r = r; this.cx = cx;
    this.vs = []; this.cs = [];
    this.out = [];
    this.maxs = 0; this.ntmp = 0; this.nlab = 0;
    this.locals = cx.locals;          /* (types) */
    this.inits = cx.locals.map(function (t, i) { return i < cx.nparams || defaultable(t); });
    this.initLog = [];                /* (locals set since: undone at a frame's end) */
}
FnCompiler.prototype = {
    err: function (m) { throw CE(m); },
    emit: function (s) { if (!this.deadNow()) this.out.push(s); },
    deadNow: function () { var f = this.cs[this.cs.length - 1]; return f ? (f.dead || f.deadIn) : false; },
    tmp: function () { return 't' + (this.ntmp++); },
    S: function (i) { return 's' + i; },
    push: function (t) {
        this.vs.push(t);
        if (this.vs.length > this.maxs) this.maxs = this.vs.length;
        return 's' + (this.vs.length - 1);
    },
    /* (pop one, of type t (a subtype of it) or any when t is undefined: its slot name in .slot) */
    pop: function (t) {
        var f = this.cs[this.cs.length - 1];
        if (this.vs.length <= f.h + 0 && this.vs.length === f.h) {
            if (f.dead) { this.lastSlot = 's' + this.vs.length; return BOT; }
            this.err('type mismatch: stack underflow');
        }
        var a = this.vs.pop();
        if (t !== undefined && !valSub(a, t)) this.err('type mismatch');
        this.lastSlot = 's' + this.vs.length;
        return a;
    },
    popS: function (t) { this.pop(t); return this.lastSlot; },
    popRef: function () {
        var a = this.pop();
        if (a !== BOT && !isRef(a)) this.err('type mismatch: reference expected');
        return a;
    },
    pops: function (ts) { for (var i = ts.length - 1; i >= 0; i--) this.pop(ts[i]); },
    pushes: function (ts) { for (var i = 0; i < ts.length; i++) this.push(ts[i]); },
    pushFrame: function (op, params, results) {
        var deadIn = this.deadNow();
        var f = { op: op, params: params, results: results, h: this.vs.length - params.length, dead: false, deadIn: deadIn,
                  label: 'L' + (this.nlab++), initMark: this.initLog.length };
        this.cs.push(f);
        return f;
    },
    popFrame: function () {
        var f = this.cs[this.cs.length - 1];
        this.pops(f.results);
        if (this.vs.length !== f.h) this.err('type mismatch: values remaining on the stack at end of block');
        this.cs.pop();
        /* (locals initialized inside: not after) */
        while (this.initLog.length > f.initMark) this.inits[this.initLog.pop()] = false;
        return f;
    },
    unreachable: function () {
        var f = this.cs[this.cs.length - 1];
        this.vs.length = f.h;
        f.dead = true;
    },
    labelTypes: function (f) { return f.op === 'loop' ? f.params : f.results; },
    frameAt: function (depth) {
        if (depth >= this.cs.length) this.err('unknown label');
        return this.cs[this.cs.length - 1 - depth];
    },
    /* (the jump to frame f with its label's values from the top n slots) */
    jumpCode: function (f, n) {
        var top = this.vs.length, code = '';
        if (f.op === 'func') return this.retCode(n);
        for (var i = 0; i < n; i++) {
            var src = top - n + i, dst = f.h + i;
            if (src !== dst) code += 's' + dst + '=s' + src + ';';
        }
        return code + (f.op === 'loop' ? 'continue ' : 'break ') + f.label + ';';
    },
    retCode: function (n) {
        var top = this.vs.length, rs = this.cx.results;
        if (rs.length === 0) return 'return;';
        if (rs.length === 1) return 'return s' + (top - 1) + ';';
        var a = [];
        for (var i = 0; i < rs.length; i++) a.push('s' + (top - rs.length + i));
        return 'return [' + a.join(',') + '];';
    },
    /* (a branch's operands: the label's types checked, left on the stack for the caller to decide) */
    checkBranch: function (f) {
        var ts = this.labelTypes(f), saved = [];
        for (var i = ts.length - 1; i >= 0; i--) saved.unshift(this.pop(ts[i]));
        /* (left on the stack with the label's types) */
        for (var j = 0; j < ts.length; j++) this.push(ts[j]);
        return ts.length;
    },
    blockType: function () {
        var r = this.r, b = r.peek();
        if (b === 0x40) { r.u8(); return { params: [], results: [] }; }
        if (b === 0x7f || b === 0x7e || b === 0x7d || b === 0x7c || b === 0x7b || b === 0x63 || b === 0x64 || isAbstractByte(b)) {
            return { params: [], results: [readValType(r, this.M)] };
        }
        var x = r.s33();
        if (x < 0 || x >= this.M.types.length) this.err('unknown type');
        var t = this.M.types[x];
        if (t.kind !== 'func') this.err('type mismatch: block type is not a function type');
        return { params: t.params, results: t.results };
    },
    local: function (x) {
        if (x >= this.locals.length) this.err('unknown local');
        return this.locals[x];
    },
    funcType: function (x) {
        if (x >= this.M.funcs.length) this.err('unknown function');
        return CT[this.M.funcs[x]];
    },
    typeAt: function (x) {
        if (x >= this.M.types.length) this.err('unknown type');
        return x;
    },
    memAt: function (x) {
        if (x >= this.M.mems.length) this.err('unknown memory ' + x);
        return this.M.mems[x];
    },
    tableAt: function (x) {
        if (x >= this.M.tables.length) this.err('unknown table ' + x);
        return this.M.tables[x];
    },
    /* (a memarg: {mem, off (a number, or a BigInt for the big ones), at}) */
    memarg: function (natural) {
        var r = this.r, flags = r.u32(), mem = 0;
        if (flags & 0x40) { flags &= ~0x40; mem = r.u32(); }
        if (flags >= 64) this.err('malformed memop flags');
        var m = this.memAt(mem);
        var off = r.u64();
        if (m.at === 'i32' && off > 0xffffffffn) this.err('offset out of range');
        if (natural !== undefined && flags > natural) this.err('alignment must not be larger than natural');
        return { mem: mem, off: off, at: m.at, align: flags };
    },
    /* (the effective address of slot a with offset off, n bytes: JS checking it, the address in variable ea) */
    eaCode: function (ma, a, n) {
        var m = 'MEM' + ma.mem;
        if (ma.at === 'i32') {
            var offn = Number(ma.off);
            return 'ea=(' + a + '>>>0)+' + offn + ';if(ea+' + n + '>' + m + '.len)R.oob();';
        }
        return 'ea=R.ea64(' + a + ',' + ma.off + 'n,' + n + ',' + m + '.len);';
    },
    compileBody: function () {
        var cx = this.cx, r = this.r;
        this.pushFrame('func', [], cx.results);
        while (true) {
            var op = r.u8();
            if (op === 0x0b && this.cs.length === 1) {
                this.popFrame();
                /* (the fall-through return) */
                break;
            }
            this.instr(op);
        }
        if (!r.eof() && !cx.konst) this.err('section size mismatch: junk after end of function');
    },
    instr: function (op) {
        var r = this.r, M = this.M, cx = this.cx, t, f, a, b, c, x, y, n, i, s, ts;
        if (cx.konst && !KONST_OK[op] && !(op === 0xfb || op === 0xfd)) this.err('constant expression required');
        switch (op) {
        case 0x00: this.emit('R.unreachable();'); this.unreachable(); return;
        case 0x01: return;
        case 0x02: case 0x03: {   /* block, loop */
            var bt = this.blockType();
            this.pops(bt.params);
            this.pushes(bt.params);
            f = this.pushFrame(op === 0x02 ? 'block' : 'loop', bt.params, bt.results);
            if (op === 0x02) this.emit(f.label + ':{');
            else this.emit(f.label + ':while(true){');
            return;
        }
        case 0x04: {   /* if */
            var bt2 = this.blockType();
            s = this.popS('i32');
            this.pops(bt2.params);
            this.pushes(bt2.params);
            var saves = [];
            if (bt2.params.length && !this.deadNow()) {
                var h0 = this.vs.length - bt2.params.length;
                for (i = 0; i < bt2.params.length; i++) { var tv = this.tmp(); saves.push(tv); this.emit(tv + '=s' + (h0 + i) + ';'); }
            }
            f = this.pushFrame('if', bt2.params, bt2.results);
            f.saves = saves;
            this.emit(f.label + ':if(' + s + '){');
            return;
        }
        case 0x05: {   /* else */
            f = this.cs[this.cs.length - 1];
            if (f.op !== 'if' || f.elseSeen) this.err('else without if');
            this.pops(f.results);
            if (this.vs.length !== f.h) this.err('type mismatch: values remaining at else');
            while (this.initLog.length > f.initMark) this.inits[this.initLog.pop()] = false;
            f.elseSeen = true;
            f.dead = false;
            this.pushes(f.params);
            this.emit('}else{');
            for (i = 0; i < f.saves.length; i++) this.emit('s' + (f.h + i) + '=' + f.saves[i] + ';');
            return;
        }
        case 0x0b: {   /* end */
            f = this.cs[this.cs.length - 1];
            if (f.op === 'if' && !f.elseSeen) {
                /* (no else: the params pass through, they must match the results) */
                if (!typesSub(f.params, f.results)) {
                    if (f.params.length !== f.results.length) this.err('type mismatch: if without else must have matching param and result types');
                    for (i = 0; i < f.params.length; i++) if (!valSub(f.params[i], f.results[i])) this.err('type mismatch');
                }
            }
            this.popFrame();
            var wasDeadIn = f.deadIn;
            if (!wasDeadIn) {
                this.cs.length && this.out.push(f.op === 'loop' ? 'break;}' : f.op === 'try' ? '' : '}');
                if (f.op === 'try') this.out.push(f.tail);
            }
            this.pushes(f.results);
            return;
        }
        case 0x0c: {   /* br */
            f = this.frameAt(r.u32());
            n = this.checkBranch(f);
            this.emit(this.jumpCode(f, n));
            this.unreachable();
            return;
        }
        case 0x0d: {   /* br_if */
            f = this.frameAt(r.u32());
            s = this.popS('i32');
            n = this.checkBranch(f);
            this.emit('if(' + s + '){' + this.jumpCode(f, n) + '}');
            return;
        }
        case 0x0e: {   /* br_table */
            var labels = r.vec(function () { return this.u32(); }), dflt = r.u32();
            s = this.popS('i32');
            var fd = this.frameAt(dflt), arity = this.labelTypes(fd).length;
            /* (each label: the same arity, the operands of each of its types) */
            var ops = [];
            for (i = arity - 1; i >= 0; i--) ops.unshift(this.pop());
            var top0 = this.vs.length;
            var cases = [];
            var all = labels.concat([dflt]);
            for (i = 0; i < all.length; i++) {
                var fl = this.frameAt(all[i]), lt = this.labelTypes(fl);
                if (lt.length !== arity) this.err('type mismatch: br_table arity');
                for (var k = 0; k < arity; k++) if (!valSub(ops[k], lt[k])) this.err('type mismatch: br_table operand');
            }
            this.pushes(ops.map(function (o) { return o === BOT ? BOT : o; }));
            if (!this.deadNow()) {
                var code = 'switch(' + s + '){';
                for (i = 0; i < labels.length; i++) code += 'case ' + i + ':' + this.jumpCode(this.frameAt(labels[i]), arity);
                code += 'default:' + this.jumpCode(fd, arity) + '}';
                this.emit(code);
            }
            void top0;
            this.unreachable();
            return;
        }
        case 0x0f:   /* return */
            ts = this.cx.results;
            for (i = ts.length - 1; i >= 0; i--) this.pop(ts[i]);
            this.pushes(ts);
            this.emit(this.retCode(ts.length));
            this.unreachable();
            return;
        case 0x10: case 0x12: {   /* call, return_call */
            x = r.u32();
            var ft = this.funcType(x);
            if (op === 0x12 && !typesSub(ft.results, this.cx.results)) this.err('type mismatch: return_call results');
            this.callCode(ft, x < M.nFuncImports ? 'F[' + x + '].f' : 'f' + x, op === 0x12, null);
            return;
        }
        case 0x11: case 0x13: {   /* call_indirect, return_call_indirect */
            var ty = this.typeAt(r.u32()), tb = r.u32();
            var tab = this.tableAt(tb);
            if (!heapSub(tab.type.h, H_FUNC)) this.err('type mismatch: call_indirect on a non-function table');
            var cty = CT[M.typeCanon[ty]];
            if (cty.kind !== 'func') this.err('type mismatch: not a function type');
            if (op === 0x13 && !typesSub(cty.results, this.cx.results)) this.err('type mismatch: return_call_indirect results');
            s = this.popS(tab.at);
            this.callCode(cty, 'R.callInd(T' + tb + ',' + s + ',' + M.typeCanon[ty] + ')', op === 0x13, null);
            return;
        }
        case 0x14: case 0x15: {   /* call_ref, return_call_ref */
            var tyx = this.typeAt(r.u32()), cid = M.typeCanon[tyx];
            if (CT[cid].kind !== 'func') this.err('type mismatch: call_ref type');
            if (op === 0x15 && !typesSub(CT[cid].results, this.cx.results)) this.err('type mismatch: return_call_ref results');
            s = this.popS(ref(true, cid));
            this.callCode(CT[cid], 'R.callRef(' + s + ')', op === 0x15, null);
            return;
        }
        case 0x08: {   /* throw */
            x = r.u32();
            if (x >= M.tags.length) this.err('unknown tag');
            var tt = CT[M.tags[x].type];
            var args = [];
            for (i = tt.params.length - 1; i >= 0; i--) args.unshift(this.popS(tt.params[i]));
            this.emit('throw R.mkExn(TG[' + x + '],[' + args.join(',') + ']);');
            this.unreachable();
            return;
        }
        case 0x0a:   /* throw_ref */
            s = this.popS(EXNREF);
            this.emit('throw R.throwRef(' + s + ');');
            this.unreachable();
            return;
        case 0x1f: return this.tryTable();
        case 0x1a: this.pop(); return;   /* drop */
        case 0x1b: case 0x1c: {   /* select */
            var st = null;
            if (op === 0x1c) {
                var tsel = r.vec(function () { return readValType(this, M); });
                if (tsel.length !== 1) this.err('invalid result arity');
                st = tsel[0];
            }
            c = this.popS('i32');
            if (st) {
                b = this.popS(st); this.pop(st);
                a = this.lastSlot;
                this.emit(a + '=' + c + '?' + a + ':' + b + ';');
                this.push(st);
                return;
            }
            var tb2 = this.pop(); b = this.lastSlot;
            var ta2 = this.pop(); a = this.lastSlot;
            if ((ta2 !== BOT && !isNum(ta2) && ta2 !== 'v128') || (tb2 !== BOT && !isNum(tb2) && tb2 !== 'v128')) this.err('type mismatch: select without type on references');
            if (ta2 !== BOT && tb2 !== BOT && ta2 !== tb2) this.err('type mismatch: select operands');
            this.emit(a + '=' + c + '?' + a + ':' + b + ';');
            this.push(ta2 === BOT ? tb2 : ta2);
            return;
        }
        case 0x20:   /* local.get */
            x = r.u32();
            t = this.local(x);
            if (!this.inits[x]) this.err('uninitialized local');
            s = this.push(t);
            this.emit(s + '=l' + x + ';');
            return;
        case 0x21: case 0x22:   /* local.set, local.tee */
            x = r.u32();
            t = this.local(x);
            s = this.popS(t);
            this.emit('l' + x + '=' + s + ';');
            if (!this.inits[x]) { this.inits[x] = true; this.initLog.push(x); }
            if (op === 0x22) this.push(t);
            return;
        case 0x23: case 0x24: {   /* global.get, global.set */
            x = r.u32();
            if (x >= (cx.konst ? cx.nglobals : M.globals.length)) this.err('unknown global');
            var g = M.globals[x];
            if (op === 0x23) {
                if (cx.konst && g.mut) this.err('constant expression required');
                s = this.push(g.type);
                this.emit(s + '=G[' + x + '].v;');
            } else {
                if (!g.mut) this.err('global is immutable');
                s = this.popS(g.type);
                this.emit('G[' + x + '].v=' + s + ';');
            }
            return;
        }
        case 0x25: case 0x26: {   /* table.get, table.set */
            x = r.u32();
            var tbl = this.tableAt(x);
            if (op === 0x25) {
                s = this.popS(tbl.at);
                this.push(tbl.type);
                this.emit(s + '=R.tget(T' + x + ',' + s + ');');
            } else {
                b = this.popS(tbl.type);
                a = this.popS(tbl.at);
                this.emit('R.tset(T' + x + ',' + a + ',' + b + ');');
            }
            return;
        }
        case 0x3f: case 0x40: {   /* memory.size, memory.grow */
            x = r.u32();
            var mm = this.memAt(x);
            if (op === 0x3f) { s = this.push(mm.at); this.emit(s + '=' + (mm.at === 'i64' ? 'BigInt(MEM' + x + '.pages)' : 'MEM' + x + '.pages') + ';'); }
            else { s = this.popS(mm.at); this.push(mm.at); this.emit(s + '=R.memGrow(MEM' + x + ',' + s + ');'); }
            return;
        }
        case 0x41: s = this.push('i32'); this.emit(s + '=' + r.s32() + ';'); return;
        case 0x42: s = this.push('i64'); this.emit(s + '=' + r.s64() + 'n;'); return;
        case 0x43: s = this.push('f32'); this.emit(s + '=' + this.fconst(r.f32(), 32) + ';'); return;
        case 0x44: s = this.push('f64'); this.emit(s + '=' + this.fconst(r.f64(), 64) + ';'); return;
        case 0xd0: {   /* ref.null */
            var ht = readHeapType(r, M);
            s = this.push(ref(true, ht));
            this.emit(s + '=null;');
            return;
        }
        case 0xd1:   /* ref.is_null */
            this.popRef(); s = this.lastSlot;
            this.push('i32');
            this.emit(s + '=(' + s + '===null?1:0);');
            return;
        case 0xd2:   /* ref.func */
            x = r.u32();
            if (x >= M.funcs.length) this.err('unknown function ' + x);
            if (!cx.konst && !M.refs.has(x)) this.err('undeclared function reference');
            if (cx.konst) M.refs.add(x);
            s = this.push(ref(false, M.funcs[x]));
            this.emit(s + '=F[' + x + '];');
            return;
        case 0xd3:   /* ref.eq */
            b = this.popS(ref(true, H_EQ)); a = this.popS(ref(true, H_EQ));
            this.push('i32');
            this.emit(a + '=(' + a + '===' + b + '?1:0);');
            return;
        case 0xd4:   /* ref.as_non_null */
            t = this.popRef(); s = this.lastSlot;
            this.push(t === BOT ? BOT : ref(false, t.h));
            this.emit('if(' + s + '===null)R.nullRef();');
            return;
        case 0xd5: case 0xd6: {   /* br_on_null, br_on_non_null */
            f = this.frameAt(r.u32());
            t = this.popRef(); s = this.lastSlot;
            if (op === 0xd5) {
                n = this.checkBranch(f);
                this.emit('if(' + s + '===null){' + this.jumpCode(f, n) + '}');
                this.push(t === BOT ? BOT : ref(false, t.h));
            } else {
                var lt2 = this.labelTypes(f);
                if (!lt2.length || !valSub(t === BOT ? BOT : ref(false, t.h), lt2[lt2.length - 1])) this.err('type mismatch: br_on_non_null');
                for (i = lt2.length - 2; i >= 0; i--) this.pop(lt2[i]);
                this.pushes(lt2.slice(0, -1));
                var sl = this.push(t === BOT ? BOT : ref(false, t.h));
                this.emit('if(' + sl + '!==null){' + this.jumpCode(f, lt2.length) + '}');
                this.vs.pop();
            }
            return;
        }
        case 0xfc: return this.fcOp(r.u32());
        case 0xfb: return this.gcOp(r.u32());
        case 0xfd: return this.simdOp(r.u32());
        }
        if (op >= 0x28 && op <= 0x3e) {
            var mo = MEMOPS[op];
            var ma = this.memarg(mo[1]);
            if (op <= 0x35) {
                a = this.popS(ma.at);
                var lj = LOADJS[mo[2]];
                this.push(mo[0]);
                this.emit(this.eaCode(ma, a, lj[1]) + 'dv=MEM' + ma.mem + '.dv;' + a + '=' + lj[0].replace(/#/g, 'ea') + ';');
            } else {
                b = this.popS(mo[0]); a = this.popS(ma.at);
                var sj = STOREJS[mo[2]];
                this.emit(this.eaCode(ma, a, sj[1]) + 'dv=MEM' + ma.mem + '.dv;' + sj[0].replace(/#/g, 'ea').replace(/\$/g, b) + ';');
            }
            return;
        }
        var no = NUMOPS[op];
        if (no) {
            var sl2 = [];
            for (i = no.ins.length - 1; i >= 0; i--) sl2.unshift(this.popS(no.ins[i]));
            s = this.push(no.out);
            var js = no.js.replace(/\$(\d)/g, function (_, k) { return sl2[+k]; });
            this.emit(s + '=' + js + ';');
            return;
        }
        this.err('illegal opcode ' + op.toString(16));
    },
    fconst: function (v, bits) {
        if (typeof v !== 'number') return bits === 32 ? 'R.f32FromBits(' + v.b + ')' : 'R.f64FromBits(' + v.hi + ',' + v.lo + ')';
        if (v !== v) return 'NaN';
        if (v === 0 && 1 / v < 0) return '-0';
        if (v === Infinity) return 'Infinity';
        if (v === -Infinity) return '-Infinity';
        return String(v);
    },
    /* (a call of a function of type ft by expression fn; tail: return it) */
    callCode: function (ft, fn, tail, extraArgs) {
        var args = [];
        for (var i = ft.params.length - 1; i >= 0; i--) args.unshift(this.popS(ft.params[i]));
        var call = fn + '(' + args.join(',') + ')';
        if (tail) {
            this.pushes(ft.results);
            this.emit('return R.tail(' + fn.replace(/^R\.callInd\(/, 'R.indFn(').replace(/^R\.callRef\(/, 'R.refFn(') + ',[' + args.join(',') + ']);');
            this.unreachable();
            return;
        }
        var h = this.vs.length;
        this.pushes(ft.results);
        var rs = ft.results.length;
        if (rs === 0) this.emit('r=' + call + ';if(r instanceof TC)R.tcRun(r);');
        else if (rs === 1) this.emit('s' + h + '=' + call + ';if(s' + h + ' instanceof TC)s' + h + '=R.tcRun(s' + h + ');');
        else {
            var code = 'r=' + call + ';if(r instanceof TC)r=R.tcRun(r);';
            for (var j = 0; j < rs; j++) code += 's' + (h + j) + '=r[' + j + '];';
            this.emit(code);
        }
    },
    tryTable: function () {
        var r = this.r, M = this.M;
        var bt = this.blockType();
        var catches = r.vec(function () {
            var kind = this.u8();
            if (kind > 3) throw CE('malformed catch clause');
            var tag = kind < 2 ? this.u32() : -1;
            return { kind: kind, tag: tag, label: this.u32() };
        });
        this.pops(bt.params);
        this.pushes(bt.params);
        /* (each clause: its label (outside the try_table) takes the payload, then an exnref for the _ref kinds) */
        var h = this.vs.length - bt.params.length;
        var tail = '}catch(e){if(R.uncatchable(e))throw e;';
        for (var i = 0; i < catches.length; i++) {
            var cc = catches[i], payload = [];
            if (cc.kind < 2) {
                if (cc.tag >= M.tags.length) this.err('unknown tag');
                payload = CT[M.tags[cc.tag].type].params.slice();
            }
            if (cc.kind === 1 || cc.kind === 3) payload.push(ref(false, H_EXN));
            var fl = this.frameAt(cc.label), lt = this.labelTypes(fl);
            if (lt.length !== payload.length) this.err('type mismatch: catch label arity');
            for (var k = 0; k < payload.length; k++) if (!valSub(payload[k], lt[k])) this.err('type mismatch: catch label');
            /* (as if the payload were pushed at the try's height) */
            var code = '';
            for (k = 0; k < payload.length; k++) {
                var dst = fl.op === 'func' ? null : fl.h + k;
                var src = cc.kind < 2 && k < payload.length - (cc.kind === 1 ? 1 : 0) ? 'R.exnArgs(e)[' + k + ']' : 'R.exnRef(e)';
                if (fl.op === 'func') code += 'q' + k + '=' + src + ';';
                else code += 's' + dst + '=' + src + ';';
            }
            var jump;
            if (fl.op === 'func') {
                var rs = this.cx.results;
                jump = rs.length === 0 ? 'return;' : rs.length === 1 ? 'return q0;' : 'return [' + rs.map(function (_, j) { return 'q' + j; }).join(',') + '];';
                this.needQ = Math.max(this.needQ || 0, payload.length);
            } else jump = (fl.op === 'loop' ? 'continue ' : 'break ') + fl.label + ';';
            var cond = cc.kind < 2 ? 'R.exnIs(e,TG[' + cc.tag + '])' : 'true';
            tail += 'if(' + cond + '){' + code + jump + '}';
        }
        tail += 'throw e;}}';
        var f = this.pushFrame('try', bt.params, bt.results);
        f.tail = tail;
        void h;
        this.emit(f.label + ':{try{');
    },
    fcOp: function (sub) {
        var r = this.r, M = this.M, s, a, b, c, x, y, t;
        if (this.cx.konst) this.err('constant expression required');
        if (sub <= 7) {
            var so = SATOPS[sub];
            s = this.popS(so[0]);
            this.push(so[1]);
            this.emit(s + '=' + so[2].replace('$0', s) + ';');
            return;
        }
        switch (sub) {
        case 8: {   /* memory.init */
            x = r.u32();
            if (M.dataCount === undefined) this.err('data count section required');
            if (x >= M.dataCount) this.err('unknown data segment');
            y = r.u32();
            var mm = this.memAt(y);
            c = this.popS('i32'); b = this.popS('i32'); a = this.popS(mm.at);
            this.emit('R.memInit(MEM' + y + ',D,' + x + ',' + a + ',' + b + ',' + c + ');');
            return;
        }
        case 9:
            x = r.u32();
            if (M.dataCount === undefined) this.err('data count section required');
            if (x >= M.dataCount) this.err('unknown data segment');
            this.emit('D[' + x + ']=R.EMPTY;');
            return;
        case 10: {   /* memory.copy */
            x = r.u32(); y = r.u32();
            var md = this.memAt(x), ms = this.memAt(y);
            var lt = md.at === 'i64' && ms.at === 'i64' ? 'i64' : 'i32';
            c = this.popS(lt); b = this.popS(ms.at); a = this.popS(md.at);
            this.emit('R.memCopy(MEM' + x + ',MEM' + y + ',' + a + ',' + b + ',' + c + ');');
            return;
        }
        case 11: {   /* memory.fill */
            x = r.u32();
            var mf = this.memAt(x);
            c = this.popS(mf.at); b = this.popS('i32'); a = this.popS(mf.at);
            this.emit('R.memFill(MEM' + x + ',' + a + ',' + b + ',' + c + ');');
            return;
        }
        case 12: {   /* table.init */
            x = r.u32(); y = r.u32();
            if (x >= M.elems.length) this.err('unknown elem segment ' + x);
            var ti = this.tableAt(y);
            if (!valSub(M.elems[x].type, ti.type)) this.err('type mismatch: table.init');
            c = this.popS('i32'); b = this.popS('i32'); a = this.popS(ti.at);
            this.emit('R.tableInit(T' + y + ',E,' + x + ',' + a + ',' + b + ',' + c + ');');
            return;
        }
        case 13:
            x = r.u32();
            if (x >= M.elems.length) this.err('unknown elem segment ' + x);
            this.emit('E[' + x + ']=R.EMPTYA;');
            return;
        case 14: {   /* table.copy */
            x = r.u32(); y = r.u32();
            var td = this.tableAt(x), tsrc = this.tableAt(y);
            if (!valSub(tsrc.type, td.type)) this.err('type mismatch: table.copy');
            var lt2 = td.at === 'i64' && tsrc.at === 'i64' ? 'i64' : 'i32';
            c = this.popS(lt2); b = this.popS(tsrc.at); a = this.popS(td.at);
            this.emit('R.tableCopy(T' + x + ',T' + y + ',' + a + ',' + b + ',' + c + ');');
            return;
        }
        case 15: {   /* table.grow */
            x = r.u32();
            var tg = this.tableAt(x);
            b = this.popS(tg.at); a = this.popS(tg.type);
            s = this.push(tg.at);
            this.emit(s + '=R.tableGrow(T' + x + ',' + a + ',' + b + ');');
            return;
        }
        case 16: {   /* table.size */
            x = r.u32();
            var tz = this.tableAt(x);
            s = this.push(tz.at);
            this.emit(s + '=' + (tz.at === 'i64' ? 'BigInt(T' + x + '.a.length)' : 'T' + x + '.a.length') + ';');
            return;
        }
        case 17: {   /* table.fill */
            x = r.u32();
            var tf = this.tableAt(x);
            c = this.popS(tf.at); b = this.popS(tf.type); a = this.popS(tf.at);
            this.emit('R.tableFill(T' + x + ',' + a + ',' + b + ',' + c + ');');
            return;
        }
        case 19: case 20: {   /* i64.add128, i64.sub128 */
            var d2 = this.popS('i64'), c2 = this.popS('i64'), b2 = this.popS('i64'), a2 = this.popS('i64');
            this.pushes(['i64', 'i64']);
            this.emit('r=R.' + (sub === 19 ? 'add128' : 'sub128') + '(' + a2 + ',' + b2 + ',' + c2 + ',' + d2 + ');' + a2 + '=r[0];' + b2 + '=r[1];');
            return;
        }
        case 21: case 22: {   /* i64.mul_wide_s, i64.mul_wide_u */
            b = this.popS('i64'); a = this.popS('i64');
            this.pushes(['i64', 'i64']);
            this.emit('r=R.mulWide(' + a + ',' + b + ',' + (sub === 21) + ');' + a + '=r[0];' + b + '=r[1];');
            return;
        }
        }
        this.err('illegal opcode 0xfc ' + sub);
    }
};
/* (opcodes allowed in a constant expression) */
var KONST_OK = { 0x0b: 1, 0x23: 1, 0x41: 1, 0x42: 1, 0x43: 1, 0x44: 1, 0xd0: 1, 0xd2: 1, 0x6a: 1, 0x6b: 1, 0x6c: 1, 0x7c: 1, 0x7d: 1, 0x7e: 1 };

/* (a value type from the reader: concrete heap types by canonical id) */
function readHeapType(r, M) {
    var b = r.peek();
    if (isAbstractByte(b)) { r.u8(); return b - 0x80; }
    var x = r.s33();
    if (x < 0) throw CE('malformed heap type');
    if (x >= M.types.length) throw CE('unknown type');
    return M.typeCanon[x];
}
function readValType(r, M) {
    var b = r.u8();
    switch (b) {
    case 0x7f: return 'i32';
    case 0x7e: return 'i64';
    case 0x7d: return 'f32';
    case 0x7c: return 'f64';
    case 0x7b: return 'v128';
    case 0x63: return ref(true, readHeapType(r, M));
    case 0x64: return ref(false, readHeapType(r, M));
    }
    if (isAbstractByte(b)) return ref(true, b - 0x80);
    throw CE('malformed value type');
}


/* ---- the runtime the generated code calls (R) ---- */
var AI = BigInt.asIntN, AU = BigInt.asUintN;
var I64MIN = -(1n << 63n);
/* (a tail call: the callee and its arguments, run by the caller's loop) */
function TC(f, a) { this.f = f; this.a = a; }
/* (Wasm stores: memories, tables, globals, tags) */
function MemObj(pages, max, at, shared) {
    this.pages = pages; this.max = max; this.at = at; this.shared = !!shared;
    this.buf = shared && typeof SharedArrayBuffer === 'function' ? new SharedArrayBuffer(pages * 65536) : new ArrayBuffer(pages * 65536);
    this.len = pages * 65536;
    this.dv = new DataView(this.buf);
    this.u8 = new Uint8Array(this.buf);
    this.js = null;   /* (its WebAssembly.Memory) */
    this.bufObj = null;
}
function TableObj(type, n, max, at, init) {
    this.type = type; this.max = max; this.at = at;
    this.a = new Array(n);
    for (var i = 0; i < n; i++) this.a[i] = init;
    this.js = null;
}
function GlobalCell(type, mut, v) { this.type = type; this.mut = mut; this.v = v; this.js = null; }
function TagObj(type) { this.type = type; this.js = null; }
/* (a function: its canonical type, its raw callable (Wasm values in and out), its JS function once exported) */
function FuncInst(type, f, host) { this.type = type; this.f = f; this.host = host || null; this.js = null; }
/* (a JS value made an anyref) */
function HostRef(v) { this.v = v; }
function WasmStruct(t, f) { this.t = t; this.f = f; }
function WasmArray(t, a) { this.t = t; this.a = a; }

var R = {
    TC: TC,
    V0: new Int32Array(4),
    EMPTY: new Uint8Array(0),
    EMPTYA: [],
    unreachable: function () { trap('unreachable'); },
    oob: function () { trap('out of bounds memory access'); },
    nullRef: function () { trap('null reference'); },
    f32Bits: f32Bits, f32FromBits: f32FromBits, f64FromBits: f64FromBits, f64Hi: f64Hi, f64Lo: f64Lo,
    f64FromI64: f64FromI64, i64FromF64: i64FromF64,
    ctz32: function (x) { if (x === 0) return 32; return 31 - Math.clz32(x & -x); },
    popcnt32: function (x) { x = x - ((x >>> 1) & 0x55555555); x = (x & 0x33333333) + ((x >>> 2) & 0x33333333); return (Math.imul((x + (x >>> 4)) & 0x0f0f0f0f, 0x01010101) >>> 24); },
    divS32: function (a, b) { if (b === 0) trap('integer divide by zero'); if (a === -0x80000000 && b === -1) trap('integer overflow'); return (a / b) | 0; },
    divU32: function (a, b) { if (b === 0) trap('integer divide by zero'); return ((a >>> 0) / (b >>> 0)) | 0; },
    remS32: function (a, b) { if (b === 0) trap('integer divide by zero'); return (a % b) | 0; },
    remU32: function (a, b) { if (b === 0) trap('integer divide by zero'); return ((a >>> 0) % (b >>> 0)) | 0; },
    clz64: function (x) { var h = Number(AU(32, x >> 32n)); if (h) return BigInt(Math.clz32(h)); return BigInt(32 + Math.clz32(Number(AU(32, x)))); },
    ctz64: function (x) { var l = Number(AU(32, x)); if (l) return BigInt(R.ctz32(l)); var h = Number(AU(32, x >> 32n)); return BigInt(h ? 32 + R.ctz32(h) : 64); },
    popcnt64: function (x) { return BigInt(R.popcnt32(Number(AU(32, x)) | 0) + R.popcnt32(Number(AU(32, x >> 32n)) | 0)); },
    divS64: function (a, b) { if (b === 0n) trap('integer divide by zero'); if (a === I64MIN && b === -1n) trap('integer overflow'); return a / b; },
    divU64: function (a, b) { if (b === 0n) trap('integer divide by zero'); return AI(64, AU(64, a) / AU(64, b)); },
    remS64: function (a, b) { if (b === 0n) trap('integer divide by zero'); return a % b; },
    remU64: function (a, b) { if (b === 0n) trap('integer divide by zero'); return AI(64, AU(64, a) % AU(64, b)); },
    rotl64: function (a, b) { b &= 63n; var u = AU(64, a); return AI(64, (u << b) | (u >> ((64n - b) & 63n))); },
    rotr64: function (a, b) { b &= 63n; var u = AU(64, a); return AI(64, (u >> b) | (u << ((64n - b) & 63n))); },
    fneg32: function (a) { return f32FromBits(f32Bits(a) ^ 0x80000000); },
    fabs32: function (a) { return f32FromBits(f32Bits(a) & 0x7fffffff); },
    fcopysign32: function (a, b) { return f32FromBits((f32Bits(a) & 0x7fffffff) | (f32Bits(b) & 0x80000000)); },
    fneg64: function (a) { if (typeof a === 'number' && a === a) return -a; return f64FromBits(f64Hi(a) ^ 0x80000000, f64Lo(a)); },
    fabs64: function (a) { if (typeof a === 'number' && a === a) return Math.abs(a); return f64FromBits(f64Hi(a) & 0x7fffffff, f64Lo(a)); },
    fcopysign64: function (a, b) { return f64FromBits((f64Hi(a) & 0x7fffffff) | (f64Hi(b) & 0x80000000), f64Lo(a)); },
    /* (round half to even) */
    nearest: function (x) {
        if (x !== x || x === Infinity || x === -Infinity || x === 0) return x;
        var r = Math.round(x);
        if (Math.abs(x - Math.trunc(x)) === 0.5) r = 2 * Math.round(x / 2);
        if (r === 0) return x < 0 ? -0 : 0;
        return r;
    },
    truncS32: function (x) {
        if (x !== x) trap('invalid conversion to integer');
        var t = Math.trunc(x);
        if (!(t >= -2147483648 && t <= 2147483647)) trap('integer overflow');
        return t | 0;
    },
    truncU32: function (x) {
        if (x !== x) trap('invalid conversion to integer');
        var t = Math.trunc(x);
        if (!(t >= 0 && t <= 4294967295)) { if (t === 0) return 0; trap('integer overflow'); }
        return t | 0;
    },
    truncS64: function (x) {
        if (x !== x) trap('invalid conversion to integer');
        var t = Math.trunc(x);
        if (!(t >= -9223372036854775808 && t < 9223372036854775808)) trap('integer overflow');
        return BigInt(t);
    },
    truncU64: function (x) {
        if (x !== x) trap('invalid conversion to integer');
        var t = Math.trunc(x);
        if (!(t > -1 && t < 18446744073709551616)) trap('integer overflow');
        return AI(64, BigInt(t));
    },
    satS32: function (x) { if (x !== x) return 0; if (x <= -2147483648) return -2147483648; if (x >= 2147483647) return 2147483647; return Math.trunc(x) | 0; },
    satU32: function (x) { if (x !== x || x <= 0) return 0; if (x >= 4294967295) return -1; return Math.trunc(x) | 0; },
    satS64: function (x) { if (x !== x) return 0n; if (x <= -9223372036854775808) return I64MIN; if (x >= 9223372036854775808) return (1n << 63n) - 1n; return BigInt(Math.trunc(x)); },
    satU64: function (x) { if (x !== x || x <= 0) return 0n; if (x >= 18446744073709551616) return -1n; return AI(64, BigInt(Math.trunc(x))); },
    /* (an integer to f32 with one rounding: the bits below the 26th kept as a sticky one) */
    i64ToF32: function (v) {
        var neg = v < 0n, a = neg ? -v : v;
        var n = a.toString(2).length;
        var d;
        if (n <= 53) d = Number(a);
        else {
            var sh = BigInt(n - 26), m = a >> sh;
            if ((a & ((1n << sh) - 1n)) !== 0n) m |= 1n;
            d = Number(m) * Math.pow(2, n - 26);
        }
        return Math.fround(neg ? -d : d);
    },
    ea64: function (a, off, n, len) {
        var e = AU(64, a) + off;
        if (e + BigInt(n) > BigInt(len)) trap('out of bounds memory access');
        return Number(e);
    },
    memGrow: function (m, d) {
        var big = typeof d === 'bigint';
        var delta = big ? AU(64, d) : d >>> 0;
        var old = m.pages;
        if (memGrowBy(m, Number(delta), delta) < 0) return big ? -1n : -1;
        return big ? BigInt(old) : old;
    },
    memFill: function (m, a, v, n) {
        var d = idx(a), c = idx(n);
        if (d + c > m.len) trap('out of bounds memory access');
        m.u8.fill(v & 0xff, d, d + c);
    },
    memCopy: function (md, ms, a, b, n) {
        var d = idx(a), s = idx(b), c = idx(n);
        if (d + c > md.len || s + c > ms.len) trap('out of bounds memory access');
        if (md === ms) md.u8.copyWithin(d, s, s + c);
        else md.u8.set(ms.u8.subarray(s, s + c), d);
    },
    memInit: function (m, D, x, a, b, n) {
        var seg = D[x], d = idx(a), s = b >>> 0, c = n >>> 0;
        if (s + c > seg.length || d + c > m.len) trap('out of bounds memory access');
        m.u8.set(seg.subarray(s, s + c), d);
    },
    tget: function (T, i) {
        var k = idx(i);
        if (k >= T.a.length) trap('out of bounds table access');
        return T.a[k];
    },
    tset: function (T, i, v) {
        var k = idx(i);
        if (k >= T.a.length) trap('out of bounds table access');
        T.a[k] = v;
    },
    tableGrow: function (T, v, n) {
        var big = typeof n === 'bigint';
        var c = big ? AU(64, n) : n >>> 0, old = T.a.length;
        if (tableGrowBy(T, Number(c), v) < 0) return big ? -1n : -1;
        return big ? BigInt(old) : old;
    },
    tableFill: function (T, i, v, n) {
        var d = idx(i), c = idx(n);
        if (d + c > T.a.length) trap('out of bounds table access');
        for (var k = 0; k < c; k++) T.a[d + k] = v;
    },
    tableCopy: function (Td, Ts, a, b, n) {
        var d = idx(a), s = idx(b), c = idx(n);
        if (d + c > Td.a.length || s + c > Ts.a.length) trap('out of bounds table access');
        if (Td === Ts && d > s) for (var k = c - 1; k >= 0; k--) Td.a[d + k] = Ts.a[s + k];
        else for (k = 0; k < c; k++) Td.a[d + k] = Ts.a[s + k];
    },
    tableInit: function (T, E, x, a, b, n) {
        var seg = E[x], d = idx(a), s = b >>> 0, c = n >>> 0;
        if (s + c > seg.length || d + c > T.a.length) trap('out of bounds table access');
        for (var k = 0; k < c; k++) T.a[d + k] = seg[s + k];
    },
    indFn: function (T, i, ty) {
        var k = idx(i);
        if (k >= T.a.length) trap('undefined element');
        var f = T.a[k];
        if (f === null || f === undefined) trap('uninitialized element');
        if (f.type !== ty && !heapSub(f.type, ty)) trap('indirect call type mismatch');
        return f.f;
    },
    callInd: function (T, i, ty) { return R.indFn(T, i, ty); },
    refFn: function (f) { if (f === null) trap('null function reference'); return f.f; },
    callRef: function (f) { return R.refFn(f); },
    tail: function (f, a) { return new TC(f, a); },
    tcRun: function (r) {
        while (r instanceof TC) r = r.f.apply(null, r.a);
        return r;
    },
    /* ---- exceptions ---- */
    mkExn: function (tag, args) { return makeExn(tag, args); },
    throwRef: function (x) {
        if (x === null) trap('null exception reference');
        return x instanceof JSExn ? x.v : x;
    },
    exnRef: function (e) { return e instanceof WasmExn ? e : new JSExn(e); },
    exnIs: function (e, tag) {
        if (e instanceof WasmExn) return e.__tag === tag;
        return tag === JSTAG_OBJ;
    },
    exnArgs: function (e) { return e instanceof WasmExn ? e.__args : [e]; },
    uncatchable: function (e) { return e instanceof RuntimeError || isStackOverflow(e); },
    /* ---- 128-bit arithmetic (wide arithmetic) ---- */
    add128: function (al, ah, bl, bh) {
        var a = (AU(64, ah) << 64n) | AU(64, al), b = (AU(64, bh) << 64n) | AU(64, bl), s = AU(128, a + b);
        return [AI(64, s), AI(64, s >> 64n)];
    },
    sub128: function (al, ah, bl, bh) {
        var a = (AU(64, ah) << 64n) | AU(64, al), b = (AU(64, bh) << 64n) | AU(64, bl), s = AU(128, a - b);
        return [AI(64, s), AI(64, s >> 64n)];
    },
    mulWide: function (a, b, signed) {
        var p = signed ? a * b : AU(64, a) * AU(64, b);
        p = AU(128, p);
        return [AI(64, p), AI(64, p >> 64n)];
    }
};
/* (an address or length: an i32 as unsigned, an i64 as a number; past 2^53 as huge) */
function idx(v) {
    if (typeof v === 'bigint') { var u = AU(64, v); return u > 9007199254740991n ? Infinity : Number(u); }
    return v >>> 0;
}
/* (the error a JS stack overflow throws here: found the first time it is needed, not at setup) */
var STACK_OVERFLOW_CTOR = null;
function isStackOverflow(e) {
    if (e === null || typeof e !== 'object' || !/stack|recursion/i.test(String(e.message))) return false;
    if (!STACK_OVERFLOW_CTOR) {
        try { (function f() { return 1 + f(); })(); } catch (x) { STACK_OVERFLOW_CTOR = x && x.constructor; }
        if (!STACK_OVERFLOW_CTOR) STACK_OVERFLOW_CTOR = RangeError;
    }
    return e instanceof STACK_OVERFLOW_CTOR;
}
/* (a JS exception caught as an exnref) */
function JSExn(v) { this.v = v; }
var JSTAG_OBJ = new TagObj(-1);

/* (the memory grown by n pages: the old size, or -1) */
function memGrowBy(m, n, nBig) {
    var old = m.pages, nw = old + n;
    var cap = m.at === 'i64' ? 65536 * 4 : 65536;
    var lim = m.max !== null && m.max !== undefined ? Math.min(Number(m.max), cap) : cap;
    if (nBig !== undefined && typeof nBig === 'bigint' && nBig > BigInt(lim)) return -1;
    if (nw > lim) return -1;
    if (n === 0) return old;
    var nb;
    try { nb = m.shared ? new SharedArrayBuffer(nw * 65536) : new ArrayBuffer(nw * 65536); } catch (e) { return -1; }
    new Uint8Array(nb).set(m.u8);
    var oldBuf = m.buf;
    m.buf = nb; m.pages = nw; m.len = nw * 65536;
    m.dv = new DataView(nb); m.u8 = new Uint8Array(nb);
    /* (the old ArrayBuffer the JS side holds: detached) */
    if (m.bufObj && !m.shared) detachBuffer(m.bufObj);
    m.bufObj = null;
    void oldBuf;
    return old;
}
function detachBuffer(b) {
    try {
        if (typeof b.transfer === 'function') { b.transfer(); return; }
    } catch (e) { }
    try { if (typeof structuredClone === 'function') structuredClone(b, { transfer: [b] }); } catch (e) { }
}
function tableGrowBy(T, n, v) {
    var old = T.a.length, nw = old + n;
    var cap = T.at === 'i64' ? 1e7 : 1e7;
    var lim = T.max !== null && T.max !== undefined ? Math.min(Number(T.max), cap) : cap;
    if (nw > lim) return -1;
    for (var k = 0; k < n; k++) T.a.push(v);
    return old;
}


/* ---- SIMD (0xFD): v128 as an Int32Array of 4, each operation on views of the lane type ---- */
var V = (function () {
    var I8 = Int8Array, U8 = Uint8Array, I16 = Int16Array, U16 = Uint16Array, I32 = Int32Array, U32 = Uint32Array,
        F32 = Float32Array, F64 = Float64Array, I64 = BigInt64Array, U64 = BigUint64Array;
    function nv() { return new Int32Array(4); }
    var FL = function (T) { return T === Float32Array || T === Float64Array; };
    function un(T, f) {
        var fl = FL(T);
        return function (a) {
            var r = nv(), x = new T(a.buffer), z = new T(r.buffer);
            for (var i = 0; i < x.length; i++) { var v = f(x[i]); z[i] = fl && v !== v ? NaN : v; }
            return r;
        };
    }
    /* (pmin / pmax: the chosen lane's bits as they were) */
    function psel(T, I, pick) {
        return function (a, b) {
            var r = nv(), x = new T(a.buffer), y = new T(b.buffer), xi = new I(a.buffer), yi = new I(b.buffer), z = new I(r.buffer);
            for (var i = 0; i < x.length; i++) z[i] = pick(x[i], y[i]) ? yi[i] : xi[i];
            return r;
        };
    }
    function bin(T, f) {
        var fl = FL(T);
        return function (a, b) {
            var r = nv(), x = new T(a.buffer), y = new T(b.buffer), z = new T(r.buffer);
            for (var i = 0; i < x.length; i++) { var v = f(x[i], y[i]); z[i] = fl && v !== v ? NaN : v; }
            return r;
        };
    }
    /* (a comparison: all ones or zeros in lanes of the same width) */
    function cmp(T, f) {
        var R_ = T === F32 ? I32 : T === F64 ? I64 : T === U8 ? I8 : T === U16 ? I16 : T === U32 ? I32 : T === U64 ? I64 : T;
        var one = R_ === I64 ? -1n : -1, zero = R_ === I64 ? 0n : 0;
        return function (a, b) {
            var r = nv(), x = new T(a.buffer), y = new T(b.buffer), z = new R_(r.buffer);
            for (var i = 0; i < x.length; i++) z[i] = f(x[i], y[i]) ? one : zero;
            return r;
        };
    }
    function shift(T, bits, f) {
        return function (a, s) {
            var r = nv(), x = new T(a.buffer), z = new T(r.buffer), n = s & (bits - 1);
            for (var i = 0; i < x.length; i++) z[i] = f(x[i], n);
            return r;
        };
    }
    function shift64(f) {
        return function (a, s) {
            var r = nv(), x = new I64(a.buffer), z = new I64(r.buffer), n = BigInt(s & 63);
            for (var i = 0; i < 2; i++) z[i] = f(x[i], n);
            return r;
        };
    }
    function splat(T) {
        return function (v) {
            var r = nv(), z = new T(r.buffer);
            for (var i = 0; i < z.length; i++) z[i] = v;
            return r;
        };
    }
    function sat(lo, hi) { return function (v) { return v < lo ? lo : v > hi ? hi : v; }; }
    var satI8 = sat(-128, 127), satU8 = sat(0, 255), satI16 = sat(-32768, 32767), satU16 = sat(0, 65535);
    function allTrue(T) { return function (a) { var x = new T(a.buffer); for (var i = 0; i < x.length; i++) if (x[i] == 0) return 0; return 1; }; }
    function bitmask(T, sh) {
        return function (a) {
            var x = new T(a.buffer), m = 0;
            for (var i = 0; i < x.length; i++) if (x[i] < 0) m |= 1 << i;
            void sh;
            return m;
        };
    }
    function narrow(S, D, s) {
        return function (a, b) {
            var r = nv(), x = new S(a.buffer), y = new S(b.buffer), z = new D(r.buffer), n = x.length;
            for (var i = 0; i < n; i++) { z[i] = s(x[i]); z[n + i] = s(y[i]); }
            return r;
        };
    }
    function extend(S, D, high) {
        return function (a) {
            var r = nv(), x = new S(a.buffer), z = new D(r.buffer), n = z.length, o = high ? n : 0;
            for (var i = 0; i < n; i++) z[i] = D === I64 || D === U64 ? BigInt(x[o + i]) : x[o + i];
            return r;
        };
    }
    function extmul(S, D, high) {
        return function (a, b) {
            var r = nv(), x = new S(a.buffer), y = new S(b.buffer), z = new D(r.buffer), n = z.length, o = high ? n : 0;
            for (var i = 0; i < n; i++) z[i] = D === I64 || D === U64 ? BigInt(x[o + i]) * BigInt(y[o + i]) : D === I32 || D === U32 ? Math.imul(x[o + i], y[o + i]) : x[o + i] * y[o + i];
            return r;
        };
    }
    function extadd(S, D) {
        return function (a) {
            var r = nv(), x = new S(a.buffer), z = new D(r.buffer);
            for (var i = 0; i < z.length; i++) z[i] = x[2 * i] + x[2 * i + 1];
            return r;
        };
    }
    function bits32(T) {
        /* (neg / abs of float lanes: the sign bit alone) */
        return T;
    }
    void bits32;
    var V = {
        load: function (m, ea) { var r = nv(); new U8(r.buffer).set(m.u8.subarray(ea, ea + 16)); return r; },
        loadExt: function (m, ea, S, D) {
            var r = nv(), src = new S(m.buf, 0, 0), z = new D(r.buffer), n = z.length, sz = S.BYTES_PER_ELEMENT;
            var tmp = new S(m.u8.slice(ea, ea + n * sz).buffer);
            for (var i = 0; i < n; i++) z[i] = D === I64 || D === U64 ? BigInt(tmp[i]) : tmp[i];
            void src;
            return r;
        },
        loadSplat: function (m, ea, n) {
            var r = nv(), u = new U8(r.buffer);
            for (var i = 0; i < 16; i += n) for (var k = 0; k < n; k++) u[i + k] = m.u8[ea + k];
            return r;
        },
        loadZero: function (m, ea, n) { var r = nv(); new U8(r.buffer).set(m.u8.subarray(ea, ea + n)); return r; },
        loadLane: function (m, ea, v, n, lane) { var r = new I32(v); new U8(r.buffer).set(m.u8.subarray(ea, ea + n), lane * n); return r; },
        store: function (m, ea, v) { m.u8.set(new U8(v.buffer), ea); },
        storeLane: function (m, ea, v, n, lane) { m.u8.set(new U8(v.buffer, lane * n, n), ea); },
        konst: function (a, b, c, d) { var r = nv(); r[0] = a; r[1] = b; r[2] = c; r[3] = d; return r; },
        shuffle: function (a, b, l) {
            var r = nv(), x = new U8(a.buffer), y = new U8(b.buffer), z = new U8(r.buffer);
            for (var i = 0; i < 16; i++) z[i] = l[i] < 16 ? x[l[i]] : y[l[i] - 16];
            return r;
        },
        swizzle: function (a, b) {
            var r = nv(), x = new U8(a.buffer), y = new U8(b.buffer), z = new U8(r.buffer);
            for (var i = 0; i < 16; i++) z[i] = y[i] < 16 ? x[y[i]] : 0;
            return r;
        },
        splat8: splat(I8), splat16: splat(I16), splat32: splat(I32), splat64: splat(I64),
        splatF32: function (v) { return splat(I32)(f32Bits(v)); },
        splatF64: function (v) { var r = nv(); r[0] = r[2] = f64Lo(v); r[1] = r[3] = f64Hi(v); return r; },
        ext8s: function (a, l) { return new I8(a.buffer)[l]; }, ext8u: function (a, l) { return new U8(a.buffer)[l]; },
        ext16s: function (a, l) { return new I16(a.buffer)[l]; }, ext16u: function (a, l) { return new U16(a.buffer)[l]; },
        ext32: function (a, l) { return a[l]; }, ext64: function (a, l) { return new I64(a.buffer)[l]; },
        extF32: function (a, l) { return f32FromBits(a[l]); }, extF64: function (a, l) { return f64FromBits(a[2 * l + 1], a[2 * l]); },
        rep8: function (a, l, v) { var r = new I32(a); new I8(r.buffer)[l] = v; return r; },
        rep16: function (a, l, v) { var r = new I32(a); new I16(r.buffer)[l] = v; return r; },
        rep32: function (a, l, v) { var r = new I32(a); r[l] = v; return r; },
        rep64: function (a, l, v) { var r = new I32(a); new I64(r.buffer)[l] = v; return r; },
        repF32: function (a, l, v) { var r = new I32(a); r[l] = f32Bits(v); return r; },
        repF64: function (a, l, v) { var r = new I32(a); r[2 * l] = f64Lo(v); r[2 * l + 1] = f64Hi(v); return r; },
        not: un(I32, function (x) { return ~x; }),
        and: bin(I32, function (x, y) { return x & y; }), andnot: bin(I32, function (x, y) { return x & ~y; }),
        or: bin(I32, function (x, y) { return x | y; }), xor: bin(I32, function (x, y) { return x ^ y; }),
        bitselect: function (a, b, c) { var r = nv(); for (var i = 0; i < 4; i++) r[i] = (a[i] & c[i]) | (b[i] & ~c[i]); return r; },
        anyTrue: function (a) { return (a[0] | a[1] | a[2] | a[3]) !== 0 ? 1 : 0; }
    };
    var ops = {
        35: ['vv', cmp(I8, function (x, y) { return x === y; })], 36: ['vv', cmp(I8, function (x, y) { return x !== y; })],
        37: ['vv', cmp(I8, function (x, y) { return x < y; })], 38: ['vv', cmp(U8, function (x, y) { return x < y; })],
        39: ['vv', cmp(I8, function (x, y) { return x > y; })], 40: ['vv', cmp(U8, function (x, y) { return x > y; })],
        41: ['vv', cmp(I8, function (x, y) { return x <= y; })], 42: ['vv', cmp(U8, function (x, y) { return x <= y; })],
        43: ['vv', cmp(I8, function (x, y) { return x >= y; })], 44: ['vv', cmp(U8, function (x, y) { return x >= y; })],
        45: ['vv', cmp(I16, function (x, y) { return x === y; })], 46: ['vv', cmp(I16, function (x, y) { return x !== y; })],
        47: ['vv', cmp(I16, function (x, y) { return x < y; })], 48: ['vv', cmp(U16, function (x, y) { return x < y; })],
        49: ['vv', cmp(I16, function (x, y) { return x > y; })], 50: ['vv', cmp(U16, function (x, y) { return x > y; })],
        51: ['vv', cmp(I16, function (x, y) { return x <= y; })], 52: ['vv', cmp(U16, function (x, y) { return x <= y; })],
        53: ['vv', cmp(I16, function (x, y) { return x >= y; })], 54: ['vv', cmp(U16, function (x, y) { return x >= y; })],
        55: ['vv', cmp(I32, function (x, y) { return x === y; })], 56: ['vv', cmp(I32, function (x, y) { return x !== y; })],
        57: ['vv', cmp(I32, function (x, y) { return x < y; })], 58: ['vv', cmp(U32, function (x, y) { return x < y; })],
        59: ['vv', cmp(I32, function (x, y) { return x > y; })], 60: ['vv', cmp(U32, function (x, y) { return x > y; })],
        61: ['vv', cmp(I32, function (x, y) { return x <= y; })], 62: ['vv', cmp(U32, function (x, y) { return x <= y; })],
        63: ['vv', cmp(I32, function (x, y) { return x >= y; })], 64: ['vv', cmp(U32, function (x, y) { return x >= y; })],
        65: ['vv', cmp(F32, function (x, y) { return x === y; })], 66: ['vv', cmp(F32, function (x, y) { return x !== y; })],
        67: ['vv', cmp(F32, function (x, y) { return x < y; })], 68: ['vv', cmp(F32, function (x, y) { return x > y; })],
        69: ['vv', cmp(F32, function (x, y) { return x <= y; })], 70: ['vv', cmp(F32, function (x, y) { return x >= y; })],
        71: ['vv', cmp(F64, function (x, y) { return x === y; })], 72: ['vv', cmp(F64, function (x, y) { return x !== y; })],
        73: ['vv', cmp(F64, function (x, y) { return x < y; })], 74: ['vv', cmp(F64, function (x, y) { return x > y; })],
        75: ['vv', cmp(F64, function (x, y) { return x <= y; })], 76: ['vv', cmp(F64, function (x, y) { return x >= y; })],
        77: ['v', V.not], 78: ['vv', V.and], 79: ['vv', V.andnot], 80: ['vv', V.or], 81: ['vv', V.xor], 82: ['vvv', V.bitselect], 83: ['vi', V.anyTrue],
        94: ['v', function (a) { var r = nv(), x = new F64(a.buffer), z = new F32(r.buffer); z[0] = x[0]; z[1] = x[1]; return r; }],
        95: ['v', function (a) { var r = nv(), x = new F32(a.buffer), z = new F64(r.buffer); z[0] = x[0]; z[1] = x[1]; return r; }],
        96: ['v', un(I8, function (x) { return x < 0 ? -x : x; })], 97: ['v', un(I8, function (x) { return -x; })],
        98: ['v', un(U8, function (x) { return R.popcnt32(x); })], 99: ['vi', allTrue(I8)], 100: ['vi', bitmask(I8)],
        101: ['vv', narrow(I16, I8, satI8)], 102: ['vv', narrow(I16, U8, satU8)],
        103: ['v', un(F32, Math.ceil)], 104: ['v', un(F32, Math.floor)], 105: ['v', un(F32, Math.trunc)], 106: ['v', un(F32, function (x) { return R.nearest(x); })],
        107: ['vs', shift(I8, 8, function (x, n) { return x << n; })], 108: ['vs', shift(I8, 8, function (x, n) { return x >> n; })],
        109: ['vs', shift(U8, 8, function (x, n) { return x >>> n; })],
        110: ['vv', bin(I8, function (x, y) { return x + y; })], 111: ['vv', bin(I8, function (x, y) { return satI8(x + y); })],
        112: ['vv', bin(U8, function (x, y) { return satU8(x + y); })], 113: ['vv', bin(I8, function (x, y) { return x - y; })],
        114: ['vv', bin(I8, function (x, y) { return satI8(x - y); })], 115: ['vv', bin(U8, function (x, y) { return satU8(x - y); })],
        116: ['v', un(F64, Math.ceil)], 117: ['v', un(F64, Math.floor)],
        118: ['vv', bin(I8, Math.min)], 119: ['vv', bin(U8, Math.min)], 120: ['vv', bin(I8, Math.max)], 121: ['vv', bin(U8, Math.max)],
        122: ['v', un(F64, Math.trunc)], 123: ['vv', bin(U8, function (x, y) { return (x + y + 1) >>> 1; })],
        124: ['v', extadd(I8, I16)], 125: ['v', extadd(U8, U16)], 126: ['v', extadd(I16, I32)], 127: ['v', extadd(U16, U32)],
        128: ['v', un(I16, function (x) { return x < 0 ? -x : x; })], 129: ['v', un(I16, function (x) { return -x; })],
        130: ['vv', bin(I16, function (x, y) { return satI16((x * y + 0x4000) >> 15); })], 131: ['vi', allTrue(I16)], 132: ['vi', bitmask(I16)],
        133: ['vv', narrow(I32, I16, satI16)], 134: ['vv', narrow(I32, U16, satU16)],
        135: ['v', extend(I8, I16, false)], 136: ['v', extend(I8, I16, true)], 137: ['v', extend(U8, U16, false)], 138: ['v', extend(U8, U16, true)],
        139: ['vs', shift(I16, 16, function (x, n) { return x << n; })], 140: ['vs', shift(I16, 16, function (x, n) { return x >> n; })],
        141: ['vs', shift(U16, 16, function (x, n) { return x >>> n; })],
        142: ['vv', bin(I16, function (x, y) { return x + y; })], 143: ['vv', bin(I16, function (x, y) { return satI16(x + y); })],
        144: ['vv', bin(U16, function (x, y) { return satU16(x + y); })], 145: ['vv', bin(I16, function (x, y) { return x - y; })],
        146: ['vv', bin(I16, function (x, y) { return satI16(x - y); })], 147: ['vv', bin(U16, function (x, y) { return satU16(x - y); })],
        148: ['v', un(F64, function (x) { return R.nearest(x); })], 149: ['vv', bin(I16, function (x, y) { return Math.imul(x, y); })],
        150: ['vv', bin(I16, Math.min)], 151: ['vv', bin(U16, Math.min)], 152: ['vv', bin(I16, Math.max)], 153: ['vv', bin(U16, Math.max)],
        155: ['vv', bin(U16, function (x, y) { return (x + y + 1) >>> 1; })],
        156: ['vv', extmul(I8, I16, false)], 157: ['vv', extmul(I8, I16, true)], 158: ['vv', extmul(U8, U16, false)], 159: ['vv', extmul(U8, U16, true)],
        160: ['v', un(I32, function (x) { return x < 0 ? -x : x; })], 161: ['v', un(I32, function (x) { return -x; })],
        163: ['vi', allTrue(I32)], 164: ['vi', bitmask(I32)],
        167: ['v', extend(I16, I32, false)], 168: ['v', extend(I16, I32, true)], 169: ['v', extend(U16, U32, false)], 170: ['v', extend(U16, U32, true)],
        171: ['vs', shift(I32, 32, function (x, n) { return x << n; })], 172: ['vs', shift(I32, 32, function (x, n) { return x >> n; })],
        173: ['vs', shift(U32, 32, function (x, n) { return x >>> n; })],
        174: ['vv', bin(I32, function (x, y) { return x + y; })], 177: ['vv', bin(I32, function (x, y) { return x - y; })],
        181: ['vv', bin(I32, Math.imul)],
        182: ['vv', bin(I32, Math.min)], 183: ['vv', bin(U32, Math.min)], 184: ['vv', bin(I32, Math.max)], 185: ['vv', bin(U32, Math.max)],
        186: ['vv', function (a, b) {
            var r = nv(), x = new I16(a.buffer), y = new I16(b.buffer);
            for (var i = 0; i < 4; i++) r[i] = (x[2 * i] * y[2 * i] + x[2 * i + 1] * y[2 * i + 1]) | 0;
            return r;
        }],
        188: ['vv', extmul(I16, I32, false)], 189: ['vv', extmul(I16, I32, true)], 190: ['vv', extmul(U16, U32, false)], 191: ['vv', extmul(U16, U32, true)],
        192: ['v', un(I64, function (x) { return x < 0n ? -x : x; })], 193: ['v', un(I64, function (x) { return -x; })],
        195: ['vi', allTrue(I64)], 196: ['vi', bitmask(I64)],
        199: ['v', extend(I32, I64, false)], 200: ['v', extend(I32, I64, true)], 201: ['v', extend(U32, U64, false)], 202: ['v', extend(U32, U64, true)],
        203: ['vs', shift64(function (x, n) { return x << n; })], 204: ['vs', shift64(function (x, n) { return x >> n; })],
        205: ['vs', function (a, s) { var r = nv(), x = new U64(a.buffer), z = new U64(r.buffer), n = BigInt(s & 63); z[0] = x[0] >> n; z[1] = x[1] >> n; return r; }],
        206: ['vv', bin(I64, function (x, y) { return x + y; })], 209: ['vv', bin(I64, function (x, y) { return x - y; })],
        213: ['vv', bin(I64, function (x, y) { return x * y; })],
        214: ['vv', cmp(I64, function (x, y) { return x === y; })], 215: ['vv', cmp(I64, function (x, y) { return x !== y; })],
        216: ['vv', cmp(I64, function (x, y) { return x < y; })], 217: ['vv', cmp(I64, function (x, y) { return x > y; })],
        218: ['vv', cmp(I64, function (x, y) { return x <= y; })], 219: ['vv', cmp(I64, function (x, y) { return x >= y; })],
        220: ['vv', extmul(I32, I64, false)], 221: ['vv', extmul(I32, I64, true)], 222: ['vv', extmul(U32, U64, false)], 223: ['vv', extmul(U32, U64, true)],
        224: ['v', un(I32, function (x) { return x & 0x7fffffff; })], 225: ['v', un(I32, function (x) { return x ^ 0x80000000; })],
        227: ['v', un(F32, Math.sqrt)],
        228: ['vv', bin(F32, function (x, y) { return x + y; })], 229: ['vv', bin(F32, function (x, y) { return x - y; })],
        230: ['vv', bin(F32, function (x, y) { return x * y; })], 231: ['vv', bin(F32, function (x, y) { return x / y; })],
        232: ['vv', bin(F32, Math.min)], 233: ['vv', bin(F32, Math.max)],
        234: ['vv', psel(F32, I32, function (x, y) { return y < x; })], 235: ['vv', psel(F32, I32, function (x, y) { return x < y; })],
        236: ['v', function (a) { var r = new I32(a); r[1] &= 0x7fffffff; r[3] &= 0x7fffffff; return r; }],
        237: ['v', function (a) { var r = new I32(a); r[1] ^= 0x80000000; r[3] ^= 0x80000000; return r; }],
        239: ['v', un(F64, Math.sqrt)],
        240: ['vv', bin(F64, function (x, y) { return x + y; })], 241: ['vv', bin(F64, function (x, y) { return x - y; })],
        242: ['vv', bin(F64, function (x, y) { return x * y; })], 243: ['vv', bin(F64, function (x, y) { return x / y; })],
        244: ['vv', bin(F64, Math.min)], 245: ['vv', bin(F64, Math.max)],
        246: ['vv', psel(F64, I64, function (x, y) { return y < x; })], 247: ['vv', psel(F64, I64, function (x, y) { return x < y; })],
        248: ['v', function (a) { var r = nv(), x = new F32(a.buffer); for (var i = 0; i < 4; i++) r[i] = R.satS32(x[i]); return r; }],
        249: ['v', function (a) { var r = nv(), x = new F32(a.buffer); for (var i = 0; i < 4; i++) r[i] = R.satU32(x[i]); return r; }],
        250: ['v', function (a) { var r = nv(), z = new F32(r.buffer); for (var i = 0; i < 4; i++) z[i] = a[i]; return r; }],
        251: ['v', function (a) { var r = nv(), z = new F32(r.buffer); for (var i = 0; i < 4; i++) z[i] = a[i] >>> 0; return r; }],
        252: ['v', function (a) { var r = nv(), x = new F64(a.buffer); r[0] = R.satS32(x[0]); r[1] = R.satS32(x[1]); return r; }],
        253: ['v', function (a) { var r = nv(), x = new F64(a.buffer); r[0] = R.satU32(x[0]); r[1] = R.satU32(x[1]); return r; }],
        254: ['v', function (a) { var r = nv(), z = new F64(r.buffer); z[0] = a[0]; z[1] = a[1]; return r; }],
        255: ['v', function (a) { var r = nv(), z = new F64(r.buffer); z[0] = a[0] >>> 0; z[1] = a[1] >>> 0; return r; }],
        /* relaxed SIMD: the deterministic choices */
        0x100: ['vv', V.swizzle],
        0x101: ['v', function (a) { return ops[248][1](a); }], 0x102: ['v', function (a) { return ops[249][1](a); }],
        0x103: ['v', function (a) { return ops[252][1](a); }], 0x104: ['v', function (a) { return ops[253][1](a); }],
        0x105: ['vvv', function (a, b, c) { var r = nv(), x = new F32(a.buffer), y = new F32(b.buffer), w = new F32(c.buffer), z = new F32(r.buffer); for (var i = 0; i < 4; i++) z[i] = Math.fround(Math.fround(x[i] * y[i]) + w[i]); return r; }],
        0x106: ['vvv', function (a, b, c) { var r = nv(), x = new F32(a.buffer), y = new F32(b.buffer), w = new F32(c.buffer), z = new F32(r.buffer); for (var i = 0; i < 4; i++) z[i] = Math.fround(-Math.fround(x[i] * y[i]) + w[i]); return r; }],
        0x107: ['vvv', function (a, b, c) { var r = nv(), x = new F64(a.buffer), y = new F64(b.buffer), w = new F64(c.buffer), z = new F64(r.buffer); for (var i = 0; i < 2; i++) z[i] = x[i] * y[i] + w[i]; return r; }],
        0x108: ['vvv', function (a, b, c) { var r = nv(), x = new F64(a.buffer), y = new F64(b.buffer), w = new F64(c.buffer), z = new F64(r.buffer); for (var i = 0; i < 2; i++) z[i] = -(x[i] * y[i]) + w[i]; return r; }],
        0x109: ['vvv', V.bitselect], 0x10a: ['vvv', V.bitselect], 0x10b: ['vvv', V.bitselect], 0x10c: ['vvv', V.bitselect],
        0x10d: ['vv', bin(F32, Math.min)], 0x10e: ['vv', bin(F32, Math.max)], 0x10f: ['vv', bin(F64, Math.min)], 0x110: ['vv', bin(F64, Math.max)],
        0x111: ['vv', bin(I16, function (x, y) { return satI16((x * y + 0x4000) >> 15); })],
        0x112: ['vv', function (a, b) { var r = nv(), x = new I8(a.buffer), y = new I8(b.buffer), z = new I16(r.buffer); for (var i = 0; i < 8; i++) z[i] = satI16(x[2 * i] * y[2 * i] + x[2 * i + 1] * y[2 * i + 1]); return r; }],
        0x113: ['vvv', function (a, b, c) {
            var r = nv(), x = new I8(a.buffer), y = new I8(b.buffer);
            for (var i = 0; i < 4; i++) {
                var s = 0;
                for (var k = 0; k < 4; k++) s += x[4 * i + k] * y[4 * i + k];
                r[i] = (s + c[i]) | 0;
            }
            return r;
        }]
    };
    V.ops = ops;
    V.T = { I8: I8, U8: U8, I16: I16, U16: U16, I32: I32, U32: U32, I64: I64, U64: U64 };
    return V;
})();
R.V = V;
R.V0 = new Int32Array(4);

FnCompiler.prototype.simdOp = function (sub) {
    var r = this.r, s, a, b, c, ma, lane, i;
    var cx = this.cx;
    if (cx.konst && sub !== 12) this.err('constant expression required');
    var self = this;
    function ld(n, code) {
        ma = self.memarg(Math.log2(n) | 0);
        a = self.popS(ma.at);
        self.push('v128');
        self.emit(self.eaCode(ma, a, n) + a + '=' + code + ';');
    }
    function laneImm(n) { var l = r.u8(); if (l >= n) self.err('invalid lane index'); return l; }
    switch (sub) {
    case 0: ma = this.memarg(4); a = this.popS(ma.at); this.push('v128'); this.emit(this.eaCode(ma, a, 16) + a + '=R.V.load(MEM' + ma.mem + ',ea);'); return;
    case 1: case 2: case 3: case 4: case 5: case 6: {
        var pairs = [['I8', 'I16'], ['U8', 'U16'], ['I16', 'I32'], ['U16', 'U32'], ['I32', 'I64'], ['U32', 'U64']][sub - 1];
        ma = this.memarg(3); a = this.popS(ma.at); this.push('v128');
        this.emit(this.eaCode(ma, a, 8) + a + '=R.V.loadExt(MEM' + ma.mem + ',ea,R.V.T.' + pairs[0] + ',R.V.T.' + pairs[1] + ');');
        return;
    }
    case 7: case 8: case 9: case 10: {
        var n = 1 << (sub - 7);
        ma = this.memarg(sub - 7); a = this.popS(ma.at); this.push('v128');
        this.emit(this.eaCode(ma, a, n) + a + '=R.V.loadSplat(MEM' + ma.mem + ',ea,' + n + ');');
        return;
    }
    case 11:
        ma = this.memarg(4); b = this.popS('v128'); a = this.popS(ma.at);
        this.emit(this.eaCode(ma, a, 16) + 'R.V.store(MEM' + ma.mem + ',ea,' + b + ');');
        return;
    case 12: {
        var w = [];
        for (i = 0; i < 4; i++) w.push(r.u8() | (r.u8() << 8) | (r.u8() << 16) | (r.u8() << 24));
        s = this.push('v128');
        this.emit(s + '=R.V.konst(' + w.join(',') + ');');
        return;
    }
    case 13: {
        var ls = [];
        for (i = 0; i < 16; i++) { var l = r.u8(); if (l >= 32) this.err('invalid lane index'); ls.push(l); }
        b = this.popS('v128'); a = this.popS('v128'); this.push('v128');
        this.emit(a + '=R.V.shuffle(' + a + ',' + b + ',[' + ls.join(',') + ']);');
        return;
    }
    case 14: b = this.popS('v128'); a = this.popS('v128'); this.push('v128'); this.emit(a + '=R.V.swizzle(' + a + ',' + b + ');'); return;
    case 15: case 16: case 17: case 18: case 19: case 20: {
        var st = ['i32', 'i32', 'i32', 'i64', 'f32', 'f64'][sub - 15], fn = ['splat8', 'splat16', 'splat32', 'splat64', 'splatF32', 'splatF64'][sub - 15];
        a = this.popS(st); this.push('v128'); this.emit(a + '=R.V.' + fn + '(' + a + ');');
        return;
    }
    case 21: case 22: case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31: case 32: case 33: case 34: {
        var spec = {
            21: [16, 'ext8s', 'i32', 0], 22: [16, 'ext8u', 'i32', 0], 23: [16, 'rep8', 'i32', 1],
            24: [8, 'ext16s', 'i32', 0], 25: [8, 'ext16u', 'i32', 0], 26: [8, 'rep16', 'i32', 1],
            27: [4, 'ext32', 'i32', 0], 28: [4, 'rep32', 'i32', 1], 29: [2, 'ext64', 'i64', 0], 30: [2, 'rep64', 'i64', 1],
            31: [4, 'extF32', 'f32', 0], 32: [4, 'repF32', 'f32', 1], 33: [2, 'extF64', 'f64', 0], 34: [2, 'repF64', 'f64', 1]
        }[sub];
        lane = laneImm(spec[0]);
        if (spec[3]) {
            b = this.popS(spec[2]); a = this.popS('v128'); this.push('v128');
            this.emit(a + '=R.V.' + spec[1] + '(' + a + ',' + lane + ',' + b + ');');
        } else {
            a = this.popS('v128'); this.push(spec[2]);
            this.emit(a + '=R.V.' + spec[1] + '(' + a + ',' + lane + ');');
        }
        return;
    }
    case 84: case 85: case 86: case 87: case 88: case 89: case 90: case 91: {
        var nb = 1 << ((sub - 84) & 3);
        ma = this.memarg((sub - 84) & 3);
        lane = laneImm(16 / nb);
        b = this.popS('v128'); a = this.popS(ma.at);
        if (sub <= 87) {
            this.push('v128');
            this.emit(this.eaCode(ma, a, nb) + a + '=R.V.loadLane(MEM' + ma.mem + ',ea,' + b + ',' + nb + ',' + lane + ');');
        } else this.emit(this.eaCode(ma, a, nb) + 'R.V.storeLane(MEM' + ma.mem + ',ea,' + b + ',' + nb + ',' + lane + ');');
        return;
    }
    case 92: case 93: {
        var nz = sub === 92 ? 4 : 8;
        ma = this.memarg(sub === 92 ? 2 : 3); a = this.popS(ma.at); this.push('v128');
        this.emit(this.eaCode(ma, a, nz) + a + '=R.V.loadZero(MEM' + ma.mem + ',ea,' + nz + ');');
        return;
    }
    }
    var op = V.ops[sub];
    if (!op) this.err('illegal opcode 0xfd ' + sub);
    var sig = op[0];
    switch (sig) {
    case 'v': a = this.popS('v128'); this.push('v128'); this.emit(a + '=R.V.ops[' + sub + '][1](' + a + ');'); return;
    case 'vv': b = this.popS('v128'); a = this.popS('v128'); this.push('v128'); this.emit(a + '=R.V.ops[' + sub + '][1](' + a + ',' + b + ');'); return;
    case 'vvv': c = this.popS('v128'); b = this.popS('v128'); a = this.popS('v128'); this.push('v128'); this.emit(a + '=R.V.ops[' + sub + '][1](' + a + ',' + b + ',' + c + ');'); return;
    case 'vi': a = this.popS('v128'); this.push('i32'); this.emit(a + '=R.V.ops[' + sub + '][1](' + a + ');'); return;
    case 'vs': b = this.popS('i32'); a = this.popS('v128'); this.push('v128'); this.emit(a + '=R.V.ops[' + sub + '][1](' + a + ',' + b + ');'); return;
    }
    this.err('illegal opcode 0xfd ' + sub);
};


/* ---- GC (0xFB): structs and arrays as JS objects, i31 as numbers ---- */
var GC_KONST = { 0: 1, 1: 1, 6: 1, 7: 1, 8: 1, 26: 1, 27: 1, 28: 1 };
function packIn(st, v) { return st === 'i8' ? v & 0xff : st === 'i16' ? v & 0xffff : v; }
R.packIn = packIn;
R.struct = function (t, f) { return new WasmStruct(t, f); };
R.sget = function (o, i) { if (o === null) trap('null structure reference'); return o.f[i]; };
R.sset = function (o, i, v) { if (o === null) trap('null structure reference'); o.f[i] = v; };
R.arrNew = function (t, v, n) {
    n >>>= 0;
    if (n > 100000000) trap('out of bounds array access');
    var a = new Array(n);
    for (var i = 0; i < n; i++) a[i] = v;
    return new WasmArray(t, a);
};
R.arrFixed = function (t, vals) { return new WasmArray(t, vals); };
var ELEM_BYTES = { i8: 1, i16: 2, i32: 4, i64: 8, f32: 4, f64: 8, v128: 16 };
function readElem(bytes, at, st) {
    var dv = new DataView(bytes.buffer, bytes.byteOffset + at);
    switch (st) {
    case 'i8': return dv.getUint8(0);
    case 'i16': return dv.getUint16(0, true);
    case 'i32': return dv.getInt32(0, true);
    case 'i64': return dv.getBigInt64(0, true);
    case 'f32': return f32FromBits(dv.getInt32(0, true));
    case 'f64': return f64FromBits(dv.getInt32(4, true), dv.getInt32(0, true));
    case 'v128': { var r = new Int32Array(4); for (var k = 0; k < 4; k++) r[k] = dv.getInt32(4 * k, true); return r; }
    }
}
R.arrNewData = function (t, D, d, s, n) {
    var st = CT[t].field.t, sz = ELEM_BYTES[st], seg = D[d];
    s >>>= 0; n >>>= 0;
    if (s + n * sz > seg.length) trap('out of bounds memory access');
    var a = new Array(n);
    for (var i = 0; i < n; i++) a[i] = readElem(seg, s + i * sz, st);
    return new WasmArray(t, a);
};
R.arrNewElem = function (t, E, e, s, n) {
    var seg = E[e];
    s >>>= 0; n >>>= 0;
    if (s + n > seg.length) trap('out of bounds table access');
    return new WasmArray(t, seg.slice(s, s + n));
};
R.aget = function (o, i) {
    if (o === null) trap('null array reference');
    i >>>= 0;
    if (i >= o.a.length) trap('out of bounds array access');
    return o.a[i];
};
R.aset = function (o, i, v) {
    if (o === null) trap('null array reference');
    i >>>= 0;
    if (i >= o.a.length) trap('out of bounds array access');
    o.a[i] = v;
};
R.alen = function (o) { if (o === null) trap('null array reference'); return o.a.length; };
R.afill = function (o, d, v, n) {
    if (o === null) trap('null array reference');
    d >>>= 0; n >>>= 0;
    if (d + n > o.a.length) trap('out of bounds array access');
    for (var i = 0; i < n; i++) o.a[d + i] = v;
};
R.acopy = function (od, d, os, s, n) {
    if (od === null || os === null) trap('null array reference');
    d >>>= 0; s >>>= 0; n >>>= 0;
    if (d + n > od.a.length || s + n > os.a.length) trap('out of bounds array access');
    if (od === os && d > s) for (var i = n - 1; i >= 0; i--) od.a[d + i] = os.a[s + i];
    else for (i = 0; i < n; i++) od.a[d + i] = os.a[s + i];
};
R.ainitData = function (o, D, x, d, s, n) {
    if (o === null) trap('null array reference');
    var st = CT[o.t].field.t, sz = ELEM_BYTES[st], seg = D[x];
    d >>>= 0; s >>>= 0; n >>>= 0;
    if (d + n > o.a.length) trap('out of bounds array access');
    if (s + n * sz > seg.length) trap('out of bounds memory access');
    for (var i = 0; i < n; i++) o.a[d + i] = readElem(seg, s + i * sz, st);
};
R.ainitElem = function (o, E, x, d, s, n) {
    if (o === null) trap('null array reference');
    var seg = E[x];
    d >>>= 0; s >>>= 0; n >>>= 0;
    if (d + n > o.a.length) trap('out of bounds array access');
    if (s + n > seg.length) trap('out of bounds table access');
    for (var i = 0; i < n; i++) o.a[d + i] = seg[s + i];
};
R.test = function (v, h, nullable) { return v === null ? nullable : refTest(v, h); };
R.cast = function (v, h, nullable) {
    if (v === null ? !nullable : !refTest(v, h)) trap('cast failure');
    return v;
};
R.i31get = function (v, signed) {
    if (v === null) trap('null i31 reference');
    return signed ? v : v & 0x7fffffff;
};
R.internalize = function (v) { return internalize(v); };
R.externalize = function (v) { return externalize(v); };

FnCompiler.prototype.gcOp = function (sub) {
    var r = this.r, M = this.M, cx = this.cx, s, a, b, c, d, x, y, t, i;
    if (cx.konst && !GC_KONST[sub]) this.err('constant expression required');
    var self = this;
    function structT(x) { self.typeAt(x); var c = M.typeCanon[x]; if (CT[c].kind !== 'struct') self.err('type mismatch: struct type expected'); return c; }
    function arrayT(x) { self.typeAt(x); var c = M.typeCanon[x]; if (CT[c].kind !== 'array') self.err('type mismatch: array type expected'); return c; }
    function needData(dx) { if (M.dataCount === undefined) self.err('data count section required'); if (dx >= M.dataCount) self.err('unknown data segment ' + dx); }
    function needElem(ex) { if (ex >= M.elems.length) self.err('unknown elem segment ' + ex); return M.elems[ex]; }
    function readRefType(nullable) { return ref(nullable, readHeapType(r, M)); }
    switch (sub) {
    case 0: case 1: {   /* struct.new, struct.new_default */
        x = structT(r.u32());
        var fs = CT[x].fields, args = [];
        if (sub === 0) {
            for (i = fs.length - 1; i >= 0; i--) args.unshift(this.popS(storageUnpack(fs[i].t)));
            args = args.map(function (sl, k) { return fs[k].t === 'i8' || fs[k].t === 'i16' ? 'R.packIn("' + fs[k].t + '",' + sl + ')' : sl; });
        } else {
            for (i = 0; i < fs.length; i++) { if (!defaultable(fs[i].t)) this.err('type mismatch: field is not defaultable'); args.push(vtDefault(fs[i].t)); }
        }
        s = this.push(ref(false, x));
        this.emit(s + '=R.struct(' + x + ',[' + args.join(',') + ']);');
        return;
    }
    case 2: case 3: case 4: {   /* struct.get(_s/_u) */
        x = structT(r.u32());
        var fi = r.u32(), fl = CT[x].fields;
        if (fi >= fl.length) this.err('unknown field');
        var ft = fl[fi].t, packed = ft === 'i8' || ft === 'i16';
        if (packed !== (sub !== 2)) this.err('type mismatch: packed field access');
        a = this.popS(ref(true, x));
        this.push(storageUnpack(ft));
        var get = 'R.sget(' + a + ',' + fi + ')';
        if (sub === 3) get = ft === 'i8' ? '((' + get + '<<24)>>24)' : '((' + get + '<<16)>>16)';
        this.emit(a + '=' + get + ';');
        return;
    }
    case 5: {   /* struct.set */
        x = structT(r.u32());
        var fj = r.u32(), fl2 = CT[x].fields;
        if (fj >= fl2.length) this.err('unknown field');
        if (!fl2[fj].mut) this.err('field is immutable');
        b = this.popS(storageUnpack(fl2[fj].t));
        a = this.popS(ref(true, x));
        this.emit('R.sset(' + a + ',' + fj + ',' + (fl2[fj].t === 'i8' || fl2[fj].t === 'i16' ? 'R.packIn("' + fl2[fj].t + '",' + b + ')' : b) + ');');
        return;
    }
    case 6: case 7: {   /* array.new, array.new_default */
        x = arrayT(r.u32());
        var et = CT[x].field.t;
        b = this.popS('i32');
        if (sub === 6) {
            a = this.popS(storageUnpack(et));
            if (et === 'i8' || et === 'i16') a = 'R.packIn("' + et + '",' + a + ')';
            s = this.push(ref(false, x));
            this.emit(s + '=R.arrNew(' + x + ',' + a + ',' + b + ');');
        } else {
            if (!defaultable(et)) this.err('type mismatch: array element is not defaultable');
            s = this.push(ref(false, x));
            this.emit(s + '=R.arrNew(' + x + ',' + vtDefault(et) + ',' + b + ');');
        }
        return;
    }
    case 8: {   /* array.new_fixed */
        x = arrayT(r.u32());
        var n = r.u32(), et2 = CT[x].field.t, vals = [];
        if (n > 10000) this.err('array.new_fixed too large');
        for (i = 0; i < n; i++) vals.unshift(this.popS(storageUnpack(et2)));
        if (et2 === 'i8' || et2 === 'i16') vals = vals.map(function (v) { return 'R.packIn("' + et2 + '",' + v + ')'; });
        s = this.push(ref(false, x));
        this.emit(s + '=R.arrFixed(' + x + ',[' + vals.join(',') + ']);');
        return;
    }
    case 9: {   /* array.new_data */
        x = arrayT(r.u32()); d = r.u32();
        needData(d);
        if (isRef(CT[x].field.t)) this.err('type mismatch: array.new_data of references');
        b = this.popS('i32'); a = this.popS('i32');
        s = this.push(ref(false, x));
        this.emit(s + '=R.arrNewData(' + x + ',D,' + d + ',' + a + ',' + b + ');');
        return;
    }
    case 10: {   /* array.new_elem */
        x = arrayT(r.u32()); d = r.u32();
        var seg = needElem(d);
        if (!valSub(seg.type, CT[x].field.t)) this.err('type mismatch: array.new_elem');
        b = this.popS('i32'); a = this.popS('i32');
        s = this.push(ref(false, x));
        this.emit(s + '=R.arrNewElem(' + x + ',E,' + d + ',' + a + ',' + b + ');');
        return;
    }
    case 11: case 12: case 13: {   /* array.get(_s/_u) */
        x = arrayT(r.u32());
        var at = CT[x].field.t, pk = at === 'i8' || at === 'i16';
        if (pk !== (sub !== 11)) this.err('type mismatch: packed array access');
        b = this.popS('i32'); a = this.popS(ref(true, x));
        this.push(storageUnpack(at));
        var g2 = 'R.aget(' + a + ',' + b + ')';
        if (sub === 12) g2 = at === 'i8' ? '((' + g2 + '<<24)>>24)' : '((' + g2 + '<<16)>>16)';
        this.emit(a + '=' + g2 + ';');
        return;
    }
    case 14: {   /* array.set */
        x = arrayT(r.u32());
        var fd = CT[x].field;
        if (!fd.mut) this.err('array is immutable');
        c = this.popS(storageUnpack(fd.t)); b = this.popS('i32'); a = this.popS(ref(true, x));
        this.emit('R.aset(' + a + ',' + b + ',' + (fd.t === 'i8' || fd.t === 'i16' ? 'R.packIn("' + fd.t + '",' + c + ')' : c) + ');');
        return;
    }
    case 15:   /* array.len */
        a = this.popS(ref(true, H_ARRAY));
        this.push('i32');
        this.emit(a + '=R.alen(' + a + ');');
        return;
    case 16: {   /* array.fill */
        x = arrayT(r.u32());
        var ff = CT[x].field;
        if (!ff.mut) this.err('array is immutable');
        d = this.popS('i32'); c = this.popS(storageUnpack(ff.t)); b = this.popS('i32'); a = this.popS(ref(true, x));
        this.emit('R.afill(' + a + ',' + b + ',' + (ff.t === 'i8' || ff.t === 'i16' ? 'R.packIn("' + ff.t + '",' + c + ')' : c) + ',' + d + ');');
        return;
    }
    case 17: {   /* array.copy */
        x = arrayT(r.u32()); y = arrayT(r.u32());
        var dfd = CT[x].field, sfd = CT[y].field;
        if (!dfd.mut) this.err('array is immutable');
        var okc = (dfd.t === 'i8' || dfd.t === 'i16' || sfd.t === 'i8' || sfd.t === 'i16') ? dfd.t === sfd.t : valSub(sfd.t, dfd.t);
        if (!okc) this.err('type mismatch: array.copy');
        var e5 = this.popS('i32'), e4 = this.popS('i32'), e3 = this.popS(ref(true, y)), e2 = this.popS('i32'), e1 = this.popS(ref(true, x));
        this.emit('R.acopy(' + e1 + ',' + e2 + ',' + e3 + ',' + e4 + ',' + e5 + ');');
        return;
    }
    case 18: case 19: {   /* array.init_data, array.init_elem */
        x = arrayT(r.u32()); d = r.u32();
        var fi2 = CT[x].field;
        if (!fi2.mut) this.err('array is immutable');
        if (sub === 18) { needData(d); if (isRef(fi2.t)) this.err('type mismatch: array.init_data of references'); }
        else { var sg = needElem(d); if (!valSub(sg.type, fi2.t)) this.err('type mismatch: array.init_elem'); }
        var g4 = this.popS('i32'), g3 = this.popS('i32'), g2b = this.popS('i32'), g1 = this.popS(ref(true, x));
        this.emit('R.' + (sub === 18 ? 'ainitData' : 'ainitElem') + '(' + g1 + ',' + (sub === 18 ? 'D' : 'E') + ',' + d + ',' + g2b + ',' + g3 + ',' + g4 + ');');
        return;
    }
    case 20: case 21: case 22: case 23: {   /* ref.test, ref.cast */
        var rt = readRefType(sub === 21 || sub === 23);
        t = this.popRef(); a = this.lastSlot;
        if (t !== BOT && topOf(t.h) !== topOf(rt.h)) this.err('type mismatch: ref.test / ref.cast of another hierarchy');
        if (sub <= 21) { this.push('i32'); this.emit(a + '=R.test(' + a + ',' + rt.h + ',' + rt.n + ')?1:0;'); }
        else { this.push(rt); this.emit(a + '=R.cast(' + a + ',' + rt.h + ',' + rt.n + ');'); }
        return;
    }
    case 24: case 25: {   /* br_on_cast, br_on_cast_fail */
        var flags = r.u8();
        if (flags > 3) this.err('malformed br_on_cast flags');
        var depth = r.u32();
        var rt1 = readRefType(!!(flags & 1)), rt2 = readRefType(!!(flags & 2));
        if (!valSub(rt2, rt1)) this.err('type mismatch: br_on_cast types');
        var f = this.frameAt(depth), lt = this.labelTypes(f);
        t = this.popRef(); a = this.lastSlot;
        if (t !== BOT && !valSub(t, rt1)) this.err('type mismatch: br_on_cast operand');
        var diff = ref(rt1.n && !rt2.n, rt1.h);
        var onBranch = sub === 24 ? rt2 : diff, onFall = sub === 24 ? diff : rt2;
        if (!lt.length || !valSub(onBranch, lt[lt.length - 1])) this.err('type mismatch: br_on_cast label');
        for (i = lt.length - 2; i >= 0; i--) this.pop(lt[i]);
        this.pushes(lt.slice(0, -1));
        var sl = this.push(onBranch);
        var cond = 'R.test(' + sl + ',' + rt2.h + ',' + rt2.n + ')';
        if (sub === 25) cond = '!' + cond;
        this.emit('if(' + cond + '){' + this.jumpCode(f, lt.length) + '}');
        this.vs.pop();
        this.push(onFall);
        void a;
        return;
    }
    case 26: case 27: {   /* any.convert_extern, extern.convert_any */
        t = this.popRef(); a = this.lastSlot;
        var from = sub === 26 ? H_EXTERN : H_ANY;
        if (t !== BOT && topOf(t.h) !== from) this.err('type mismatch: convert');
        this.push(ref(t === BOT ? false : t.n, sub === 26 ? H_ANY : H_EXTERN));
        this.emit(a + '=' + (sub === 26 ? 'R.internalize(' : 'R.externalize(') + a + ');');
        return;
    }
    case 28:   /* ref.i31 */
        a = this.popS('i32');
        this.push(ref(false, H_I31));
        this.emit(a + '=((' + a + '<<1)>>1);');
        return;
    case 29: case 30:
        a = this.popS(ref(true, H_I31));
        this.push('i32');
        this.emit(a + '=R.i31get(' + a + ',' + (sub === 29) + ');');
        return;
    }
    this.err('illegal opcode 0xfb ' + sub);
};


/* ---- the JavaScript API (WebAssembly JS Interface) ---- */
var S_MOD = Symbol('wasm.module'), S_INST = Symbol('wasm.instance'), S_MEM = Symbol('wasm.memory'), S_TAB = Symbol('wasm.table'),
    S_GLB = Symbol('wasm.global'), S_TAG = Symbol('wasm.tag'), S_EXN_TAG = Symbol('wasm.exn.tag'), S_EXN_ARGS = Symbol('wasm.exn.args');
var FUNC_OF = new WeakMap();   /* (an exported function -> its FuncInst) */
function slot(o, s, what) {
    if (o === null || (typeof o !== 'object' && typeof o !== 'function') || !Object.prototype.hasOwnProperty.call(o, s)) throw new TypeError('Illegal invocation: not a ' + what);
    return o[s];
}
function hide(o, s, v) { Object.defineProperty(o, s, { value: v, writable: false, enumerable: false, configurable: false }); }
function defMethods(P, ms) {
    for (var k in ms) Object.defineProperty(P, k, { value: ms[k], writable: true, enumerable: true, configurable: true });
}
function defGetter(P, name, get, set) {
    Object.defineProperty(P, name, { get: get, set: set, enumerable: true, configurable: true });
}
function toTag(o, name) { Object.defineProperty(o, Symbol.toStringTag, { value: name, configurable: true }); }
function fnName(f, n) { Object.defineProperty(f, 'name', { value: n, configurable: true }); return f; }

/* (the bytes of a BufferSource, copied) */
function bufferBytes(b) {
    if (b instanceof ArrayBuffer || (typeof SharedArrayBuffer === 'function' && b instanceof SharedArrayBuffer)) return new Uint8Array(b).slice();
    if (ArrayBuffer.isView(b)) return new Uint8Array(b.buffer, b.byteOffset, b.byteLength).slice();
    throw new TypeError('first argument must be an ArrayBuffer or typed array object');
}

/* ---- values across the boundary ---- */
function toJS(v, t) {
    if (t === 'i32' || t === 'i64') return v;
    if (t === 'f32' || t === 'f64') return +v;
    if (t === 'v128') throw new TypeError('type incompatibility when transforming from/to JS');
    if (v === null) return null;
    var top = topOf(t.h);
    if (top === H_FUNC) return funcToJS(v);
    if (top === H_EXN) throw new TypeError('type incompatibility when transforming from/to JS');
    if (top === H_EXTERN) return v;
    if (v instanceof HostRef) return v.v;
    return v;
}
function funcToJS(fi) {
    if (fi.js) return fi.js;
    var ty = CT[fi.type];
    var f = function () {
        var args = arguments;
        for (var i = 0; i < ty.params.length; i++) if (ty.params[i] === 'v128' || (isRef(ty.params[i]) && topOf(ty.params[i].h) === H_EXN)) throw new TypeError('type incompatibility when transforming from/to JS');
        for (i = 0; i < ty.results.length; i++) if (ty.results[i] === 'v128' || (isRef(ty.results[i]) && topOf(ty.results[i].h) === H_EXN)) throw new TypeError('type incompatibility when transforming from/to JS');
        var wa = new Array(ty.params.length);
        for (i = 0; i < ty.params.length; i++) wa[i] = toWasm(args[i], ty.params[i]);
        var r = fi.f.apply(null, wa);
        if (r instanceof TC) r = R.tcRun(r);
        if (ty.results.length === 0) return undefined;
        if (ty.results.length === 1) return toJS(r, ty.results[0]);
        var out = [];
        for (i = 0; i < ty.results.length; i++) out.push(toJS(r[i], ty.results[i]));
        return out;
    };
    /* (a function object, no constructor, its name the function index) */
    var nm = String(fi.index === undefined ? 0 : fi.index);
    var exported = { [nm]: (...a) => f.apply(undefined, a) }[nm];
    Object.defineProperty(exported, 'length', { value: ty.params.length, configurable: true });
    FUNC_OF.set(exported, fi);
    fi.js = exported;
    return exported;
}
function toWasm(v, t) {
    switch (t) {
    case 'i32': return ToInt32(v);
    case 'i64': return BigInt.asIntN(64, BigInt(toBigIntStrict(v)));
    case 'f32': return Math.fround(+v);
    case 'f64': return +v;
    case 'v128': throw new TypeError('type incompatibility when transforming from/to JS');
    }
    if (v === null) {
        if (!t.n) throw new TypeError('type incompatibility: null for a non-nullable reference');
        return null;
    }
    var top = topOf(t.h);
    if (top === H_EXN) throw new TypeError('type incompatibility when transforming from/to JS');
    if (top === H_EXTERN) {
        if (t.h === H_NOEXTERN) throw new TypeError('type incompatibility');
        return v;
    }
    if (top === H_FUNC) {
        var fi = typeof v === 'function' ? FUNC_OF.get(v) : undefined;
        if (!fi) throw new TypeError('type incompatibility: not a WebAssembly function');
        if (t.h !== H_FUNC && !heapSub(fi.type, t.h)) throw new TypeError('type incompatibility: function type');
        return fi;
    }
    /* (any hierarchy: i31, a Wasm object, or a host value) */
    var w = internalize(v);
    if (!refTest(w, t.h)) throw new TypeError('type incompatibility');
    return w;
}
function ToInt32(v) { return (+v) | 0; }
function toBigIntStrict(v) {
    if (typeof v === 'bigint') return v;
    if (typeof v === 'boolean') return v ? 1n : 0n;
    if (typeof v === 'string') { try { return BigInt(v); } catch (e) { throw new SyntaxError('Cannot convert ' + v + ' to a BigInt'); } }
    if (typeof v === 'object' && v !== null) return toBigIntStrict(typeof v[Symbol.toPrimitive] === 'function' ? v[Symbol.toPrimitive]('number') : v.valueOf !== Object.prototype.valueOf ? v.valueOf() : String(v));
    throw new TypeError('Cannot convert ' + String(v) + ' to a BigInt');
}
function internalize(v) {
    if (v === null) return null;
    if (typeof v === 'number' && Number.isInteger(v) && v >= -0x40000000 && v <= 0x3fffffff && !(v === 0 && 1 / v < 0)) return v;
    if (v instanceof WasmStruct || v instanceof WasmArray) return v;
    return new HostRef(v);
}
function externalize(v) {
    if (v instanceof HostRef) return v.v;
    return v;
}
/* (a runtime value of the any / func / extern hierarchies against a heap type) */
function refTest(v, h) {
    if (v === null) return false;
    if (h >= 0) {
        if (v instanceof WasmStruct || v instanceof WasmArray) return v.t === h || heapSub(v.t, h);
        if (v instanceof FuncInst) return v.type === h || heapSub(v.type, h);
        return false;
    }
    switch (h) {
    case H_ANY: return !(v instanceof FuncInst);
    case H_EQ: return typeof v === 'number' || v instanceof WasmStruct || v instanceof WasmArray;
    case H_I31: return typeof v === 'number';
    case H_STRUCT: return v instanceof WasmStruct;
    case H_ARRAY: return v instanceof WasmArray;
    case H_FUNC: return v instanceof FuncInst;
    case H_EXTERN: return true;
    case H_EXN: return true;
    }
    return false;
}

/* ---- WebAssembly.Exception ---- */
function WasmExn(tag, payload, options) {
    if (!new.target) throw new TypeError("Constructor WebAssembly.Exception requires 'new'");
    var to = slot(tag, S_TAG, 'WebAssembly.Tag');
    if (to === JSTAG_OBJ) throw new TypeError('WebAssembly.JSTag cannot be used to create an Exception');
    if (payload === null || payload === undefined || typeof payload[Symbol.iterator] !== 'function') throw new TypeError('Exception payload must be a sequence');
    var vals = Array.from(payload), ty = CT[to.type];
    if (vals.length !== ty.params.length) throw new TypeError('Exception payload length does not match the tag');
    var args = vals.map(function (v, i) { return toWasm(v, ty.params[i]); });
    hide(this, S_EXN_TAG, to);
    hide(this, S_EXN_ARGS, args);
    if (options !== undefined && options !== null && typeof options === 'object' && options.traceStack) {
        Object.defineProperty(this, 'stack', { value: new Error().stack || '', writable: true, configurable: true });
    }
}
function makeExn(tag, args) {
    var e = Object.create(WasmExn.prototype);
    hide(e, S_EXN_TAG, tag);
    hide(e, S_EXN_ARGS, args);
    return e;
}
defMethods(WasmExn.prototype, {
    getArg: function getArg(tag, index) {
        var tg = slot(this, S_EXN_TAG, 'WebAssembly.Exception');
        if (arguments.length < 2) throw new TypeError('getArg requires 2 arguments');
        var to = slot(tag, S_TAG, 'WebAssembly.Tag');
        if (to !== tg) throw new TypeError('First argument does not match the exception tag');
        var i = Number(index);
        if (!(i >= 0 && i < 4294967296)) i = ToInt32(index) >>> 0;
        var args = this[S_EXN_ARGS];
        if (i >= args.length) throw new RangeError('Index out of range');
        return toJS(args[i], CT[tg.type].params[i]);
    },
    is: function is(tag) {
        var tg = slot(this, S_EXN_TAG, 'WebAssembly.Exception');
        if (arguments.length < 1) throw new TypeError('is requires 1 argument');
        return slot(tag, S_TAG, 'WebAssembly.Tag') === tg;
    }
});
toTag(WasmExn.prototype, 'WebAssembly.Exception');
/* (the generated code's view of an exception) */
R.exnIs = function (e, tag) {
    if (e !== null && typeof e === 'object' && Object.prototype.hasOwnProperty.call(e, S_EXN_TAG)) return e[S_EXN_TAG] === tag;
    return tag === JSTAG_OBJ;
};
R.exnArgs = function (e) {
    if (e !== null && typeof e === 'object' && Object.prototype.hasOwnProperty.call(e, S_EXN_TAG)) return e[S_EXN_ARGS];
    return [e];
};
R.exnRef = function (e) {
    if (e !== null && typeof e === 'object' && Object.prototype.hasOwnProperty.call(e, S_EXN_TAG)) return e;
    return new JSExn(e);
};

/* ---- WebAssembly.Module ---- */
function Module(bytes) {
    if (!new.target) throw new TypeError("Constructor WebAssembly.Module requires 'new'");
    if (arguments.length < 1) throw new TypeError('WebAssembly.Module requires 1 argument');
    var M = decodeModule(bufferBytes(bytes));
    hide(this, S_MOD, M);
}
function moduleOf(m) { return slot(m, S_MOD, 'WebAssembly.Module'); }
var KIND_OF = ['function', 'table', 'memory', 'global', 'tag'];
defMethods(Module, {
    exports: function exports(m) {
        var M = moduleOf(m);
        return M.exports.map(function (e) { return { name: e.name, kind: e.kind }; });
    },
    imports: function imports(m) {
        var M = moduleOf(m);
        return M.imports.map(function (i) { return { module: i.module, name: i.name, kind: i.kind }; });
    },
    customSections: function customSections(m, name) {
        var M = moduleOf(m);
        if (arguments.length < 2) throw new TypeError('customSections requires 2 arguments');
        var nm = String(name), out = [];
        for (var i = 0; i < M.customs.length; i++) if (M.customs[i].name === nm) out.push(M.customs[i].bytes.slice().buffer);
        return out;
    }
});
toTag(Module.prototype, 'WebAssembly.Module');

/* ---- instantiation ---- */
function linkErr(msg) { return new LinkError(msg); }
function limitsMatch(have, want) {
    if (have.min < Number(want.min)) return false;
    if (want.max !== null && want.max !== undefined) {
        if (have.max === null || have.max === undefined || have.max > Number(want.max)) return false;
    }
    return true;
}
function instantiateModule(M, importObject) {
    if (M.imports.length && (importObject === undefined || importObject === null || (typeof importObject !== 'object' && typeof importObject !== 'function')))
        throw new TypeError('imports argument must be present and must be an object');
    var I = { funcs: [], globals: [], tables: [], mems: [], tags: [], datas: [], elems: [] };
    for (var k = 0; k < M.imports.length; k++) {
        var im = M.imports[k];
        var o = importObject[im.module];
        if (o === null || (typeof o !== 'object' && typeof o !== 'function')) throw new TypeError('Import #' + k + ' "' + im.module + '": module is not an object or function');
        var v = o[im.name], d = im.desc;
        switch (im.kind) {
        case 'function': {
            if (typeof v !== 'function') throw linkErr('Import #' + k + ' "' + im.module + '" "' + im.name + '": function import requires a callable');
            var wfi = FUNC_OF.get(v);
            if (wfi) {
                if (wfi.type !== d.type && !heapSub(wfi.type, d.type)) throw linkErr('imported function does not match the expected type');
                I.funcs.push(wfi);
            } else I.funcs.push(hostFunc(v, d.type));
            break;
        }
        case 'global': {
            var g;
            if (v !== null && typeof v === 'object' && Object.prototype.hasOwnProperty.call(v, S_GLB)) {
                g = v[S_GLB];
                if (g.mut !== d.mut) throw linkErr('imported global mutability mismatch');
                if (d.mut ? !valEq(g.type, d.type) : !valSub(g.type, d.type)) throw linkErr('imported global type mismatch');
            } else {
                if (d.mut) throw linkErr('imported mutable global must be a WebAssembly.Global object');
                if (d.type === 'i64' && typeof v !== 'bigint') throw linkErr('global import must be a BigInt');
                if ((d.type === 'i32' || d.type === 'f32' || d.type === 'f64') && typeof v !== 'number') throw linkErr('global import must be a number');
                if (d.type === 'v128') throw linkErr('v128 global import');
                g = new GlobalCell(d.type, false, toWasm(v, d.type));
            }
            I.globals.push(g);
            break;
        }
        case 'memory': {
            if (v === null || typeof v !== 'object' || !Object.prototype.hasOwnProperty.call(v, S_MEM)) throw linkErr('memory import must be a WebAssembly.Memory object');
            var mo = v[S_MEM];
            if (mo.at !== d.at || mo.shared !== d.shared) throw linkErr('memory import type mismatch');
            if (!limitsMatch({ min: mo.pages, max: mo.max === null ? null : Number(mo.max) }, d)) throw linkErr('memory import has incompatible limits');
            I.mems.push(mo);
            break;
        }
        case 'table': {
            if (v === null || typeof v !== 'object' || !Object.prototype.hasOwnProperty.call(v, S_TAB)) throw linkErr('table import must be a WebAssembly.Table object');
            var to = v[S_TAB];
            if (!valEq(to.type, d.type) || to.at !== d.at) throw linkErr('table import type mismatch');
            if (!limitsMatch({ min: to.a.length, max: to.max === null ? null : Number(to.max) }, d)) throw linkErr('table import has incompatible limits');
            I.tables.push(to);
            break;
        }
        case 'tag': {
            if (v === null || typeof v !== 'object' || !Object.prototype.hasOwnProperty.call(v, S_TAG)) throw linkErr('tag import must be a WebAssembly.Tag object');
            var tg = v[S_TAG];
            if (tg === JSTAG_OBJ ? !(CT[d.type].params.length === 1 && valEq(CT[d.type].params[0], EXTERNREF)) : tg.type !== d.type) throw linkErr('tag import type mismatch');
            I.tags.push(tg);
            break;
        }
        }
    }
    /* (the module's own: functions, then globals by their initializers, tables, memories, tags) */
    var factory = M.factory || (M.factory = new Function('R', 'I', M.js));
    var made = factory(R, I);
    for (var fi = 0; fi < made.funcs.length; fi++) {
        var fx = new FuncInst(M.funcs[M.nFuncImports + fi], made.funcs[fi]);
        fx.index = M.nFuncImports + fi;
        I.funcs.push(fx);
    }
    for (k = 0; k < M.funcs.length; k++) if (I.funcs[k].index === undefined && k < M.nFuncImports) { /* (an import keeps its own) */ }
    for (k = M.nTableImports; k < M.tables.length; k++) {
        var td = M.tables[k];
        var init = made.tables[k] ? made.tables[k]() : null;
        I.tables.push(new TableObj(td.type, Number(td.min), td.max === null ? null : Number(td.max), td.at, init));
    }
    for (k = M.nMemImports; k < M.mems.length; k++) {
        var md = M.mems[k];
        I.mems.push(new MemObj(Number(md.min), md.max === null ? null : Number(md.max), md.at, md.shared));
    }
    for (k = M.nTagImports; k < M.tags.length; k++) I.tags.push(new TagObj(M.tags[k].type));
    for (k = M.nGlobalImports; k < M.globals.length; k++) {
        var gd = M.globals[k];
        I.globals.push(new GlobalCell(gd.type, gd.mut, made.globals[k]()));
    }
    /* (the tables and memories again: the factory captured them before they were made) */
    made = factory(R, I);
    for (fi = 0; fi < made.funcs.length; fi++) I.funcs[M.nFuncImports + fi].f = made.funcs[fi];
    /* (segments: passive ones kept, active ones written, then dropped) */
    for (k = 0; k < M.elems.length; k++) {
        var es = M.elems[k];
        I.elems.push(made.elems[k].items.map(function (f) { return f(); }));
    }
    for (k = 0; k < M.datas.length; k++) I.datas.push(M.datas[k].bytes);
    var exportsObj = makeExports(M, I);
    for (k = 0; k < M.elems.length; k++) {
        es = M.elems[k];
        if (es.mode === 'active') {
            var T = I.tables[es.table], off = made.elems[k].offset(), items = I.elems[k];
            var o0 = idx(off);
            if (o0 + items.length > T.a.length) trap('out of bounds table access');
            for (var j = 0; j < items.length; j++) T.a[o0 + j] = items[j];
            I.elems[k] = R.EMPTYA;
        } else if (es.mode === 'declarative') I.elems[k] = R.EMPTYA;
    }
    for (k = 0; k < M.datas.length; k++) {
        var ds = M.datas[k];
        if (ds.mode === 'active') {
            var mem = I.mems[ds.mem], a0 = idx(made.datas[k]());
            if (a0 + ds.bytes.length > mem.len) trap('out of bounds memory access');
            mem.u8.set(ds.bytes, a0);
            I.datas[k] = R.EMPTY;
        }
    }
    if (M.start !== null) {
        var sr = I.funcs[M.start].f();
        if (sr instanceof TC) R.tcRun(sr);
    }
    return exportsObj;
}
function hostFunc(fn, type) {
    var ty = CT[type];
    var fi = new FuncInst(type, function () {
        var a = new Array(ty.params.length);
        for (var i = 0; i < ty.params.length; i++) a[i] = toJS(arguments[i], ty.params[i]);
        var r = fn.apply(undefined, a);
        if (ty.results.length === 0) return undefined;
        if (ty.results.length === 1) return toWasm(r, ty.results[0]);
        if (r === null || r === undefined || typeof r[Symbol.iterator] !== 'function') throw new TypeError('multi-value result must be iterable');
        var vals = Array.from(r);
        if (vals.length !== ty.results.length) throw new TypeError('multi-value result length mismatch');
        return vals.map(function (x, k) { return toWasm(x, ty.results[k]); });
    }, fn);
    return fi;
}
function makeExports(M, I) {
    var ex = Object.create(null);
    for (var k = 0; k < M.exports.length; k++) {
        var e = M.exports[k], v;
        switch (e.kind) {
        case 'function': {
            var fi = I.funcs[e.index];
            if (fi.index === undefined) fi.index = e.index;
            v = funcToJS(fi);
            break;
        }
        case 'table': v = tableJS(I.tables[e.index]); break;
        case 'memory': v = memoryJS(I.mems[e.index]); break;
        case 'global': v = globalJS(I.globals[e.index]); break;
        case 'tag': v = tagJS(I.tags[e.index]); break;
        }
        ex[e.name] = v;
    }
    return Object.freeze(ex);
}

/* ---- WebAssembly.Instance ---- */
function Instance(module, importObject) {
    if (!new.target) throw new TypeError("Constructor WebAssembly.Instance requires 'new'");
    var M = moduleOf(module);
    var ex = instantiateModule(M, importObject);
    hide(this, S_INST, ex);
}
defGetter(Instance.prototype, 'exports', function () { return slot(this, S_INST, 'WebAssembly.Instance'); });
toTag(Instance.prototype, 'WebAssembly.Instance');
function makeInstance(M, importObject) {
    var ex = instantiateModule(M, importObject);
    var inst = Object.create(Instance.prototype);
    hide(inst, S_INST, ex);
    return inst;
}

/* ---- descriptors ---- */
function getIndexDesc(o, name, at, req) {
    var v = o[name];
    if (v === undefined) { if (req) throw new TypeError('descriptor property "' + name + '" is required'); return undefined; }
    if (at === 'i64') {
        var b = toBigIntStrict(v);
        if (b < 0n || b > 0xffffffffffffffffn) throw new TypeError('"' + name + '" out of range');
        return Number(b);
    }
    var n = Number(v);
    if (n !== n || n === Infinity || n === -Infinity) throw new TypeError('"' + name + '" must be a finite number');
    n = Math.trunc(n);
    if (n < 0 || n > 0xffffffff) throw new TypeError('"' + name + '" out of range');
    return n;
}
function addressOf(o) {
    var a = o.address === undefined ? o.index : o.address;
    if (a === undefined) return 'i32';
    a = String(a);
    if (a !== 'i32' && a !== 'i64') throw new TypeError('invalid address type');
    return a;
}

/* ---- WebAssembly.Memory ---- */
function Memory(desc) {
    if (!new.target) throw new TypeError("Constructor WebAssembly.Memory requires 'new'");
    if (desc === null || (typeof desc !== 'object' && typeof desc !== 'function')) throw new TypeError('WebAssembly.Memory(): Argument 0 must be a memory descriptor');
    var at = addressOf(desc);
    var initial = getIndexDesc(desc, 'initial', at, true);
    var maximum = getIndexDesc(desc, 'maximum', at, false);
    var shared = !!desc.shared;
    var cap = at === 'i64' ? 262144 : 65536;
    if (initial > cap) throw new RangeError('WebAssembly.Memory(): initial size is too large');
    if (maximum !== undefined && maximum < initial) throw new RangeError('WebAssembly.Memory(): maximum must not be less than initial');
    if (maximum !== undefined && at === 'i32' && maximum > 65536) throw new RangeError('WebAssembly.Memory(): maximum too large');
    if (shared && maximum === undefined) throw new TypeError('WebAssembly.Memory(): shared memory must have a maximum');
    var m;
    try { m = new MemObj(initial, maximum === undefined ? null : maximum, at, shared); } catch (e) { throw new RangeError('WebAssembly.Memory(): could not allocate memory'); }
    hide(this, S_MEM, m);
    m.js = this;
}
function memoryJS(m) {
    if (m.js) return m.js;
    var o = Object.create(Memory.prototype);
    hide(o, S_MEM, m);
    m.js = o;
    return o;
}
defMethods(Memory.prototype, {
    grow: function grow(delta) {
        var m = slot(this, S_MEM, 'WebAssembly.Memory');
        var d = m.at === 'i64' ? Number(toBigIntStrict(delta)) : Number(delta);
        if (d !== d || d === Infinity || d === -Infinity) throw new TypeError('WebAssembly.Memory.grow(): delta must be a number');
        d = Math.trunc(d);
        if (d < 0 || d > 0xffffffff) throw new TypeError('WebAssembly.Memory.grow(): delta out of range');
        var oldBuf = m.bufObj;
        var old = memGrowBy(m, d);
        if (old < 0) throw new RangeError('WebAssembly.Memory.grow(): Maximum memory size exceeded');
        if (d === 0 && oldBuf && !m.shared) { detachBuffer(oldBuf); m.bufObj = null; }
        return m.at === 'i64' ? BigInt(old) : old;
    }
});
defGetter(Memory.prototype, 'buffer', function () {
    var m = slot(this, S_MEM, 'WebAssembly.Memory');
    if (!m.bufObj) {
        m.bufObj = m.buf;
        if (m.shared) Object.freeze(m.bufObj);
    }
    return m.bufObj;
});
toTag(Memory.prototype, 'WebAssembly.Memory');

/* ---- WebAssembly.Table ---- */
function elemType(s) {
    switch (String(s)) {
    case 'anyfunc': case 'funcref': return FUNCREF;
    case 'externref': return EXTERNREF;
    case 'anyref': return ANYREF;
    case 'eqref': return ref(true, H_EQ);
    case 'i31ref': return ref(true, H_I31);
    case 'structref': return ref(true, H_STRUCT);
    case 'arrayref': return ref(true, H_ARRAY);
    case 'nullref': return ref(true, H_NONE);
    case 'nullfuncref': return ref(true, H_NOFUNC);
    case 'nullexternref': return ref(true, H_NOEXTERN);
    case 'exnref': return EXNREF;
    }
    throw new TypeError('invalid element type');
}
function Table(desc, value) {
    if (!new.target) throw new TypeError("Constructor WebAssembly.Table requires 'new'");
    if (desc === null || (typeof desc !== 'object' && typeof desc !== 'function')) throw new TypeError('WebAssembly.Table(): Argument 0 must be a table descriptor');
    var at = addressOf(desc);
    var t = elemType(desc.element);
    var initial = getIndexDesc(desc, 'initial', at, true);
    var maximum = getIndexDesc(desc, 'maximum', at, false);
    if (maximum !== undefined && maximum < initial) throw new RangeError('WebAssembly.Table(): maximum must not be less than initial');
    if (initial > 10000000) throw new RangeError('WebAssembly.Table(): initial size too large');
    var init = arguments.length < 2 || value === undefined ? defaultRef(t) : toWasm(value, t);
    var to = new TableObj(t, initial, maximum === undefined ? null : maximum, at, init);
    hide(this, S_TAB, to);
    to.js = this;
}
function defaultRef(t) { return topOf(t.h) === H_EXTERN ? undefined : null; }
function tableJS(t) {
    if (t.js) return t.js;
    var o = Object.create(Table.prototype);
    hide(o, S_TAB, t);
    t.js = o;
    return o;
}
function tableIndex(t, v) {
    if (t.at === 'i64') {
        var b = toBigIntStrict(v);
        return b < 0n || b > 0xffffffffffffffffn ? -1 : Number(b);
    }
    var n = Number(v);
    if (n !== n || n === Infinity || n === -Infinity) throw new TypeError('index must be a number');
    n = Math.trunc(n);
    if (n < 0 || n > 0xffffffff) throw new TypeError('index out of range');
    return n;
}
defMethods(Table.prototype, {
    grow: function grow(delta, value) {
        var t = slot(this, S_TAB, 'WebAssembly.Table');
        var d = tableIndex(t, delta);
        if (d < 0) throw new TypeError('delta out of range');
        var v = arguments.length < 2 || value === undefined ? defaultRef(t.type) : toWasm(value, t.type);
        if (v === null && !t.type.n) throw new TypeError('a non-nullable table needs a value');
        var old = tableGrowBy(t, d, v);
        if (old < 0) throw new RangeError('WebAssembly.Table.grow(): failed to grow');
        return t.at === 'i64' ? BigInt(old) : old;
    },
    get: function get(index) {
        var t = slot(this, S_TAB, 'WebAssembly.Table');
        var i = tableIndex(t, index);
        if (i < 0 || i >= t.a.length) throw new RangeError('WebAssembly.Table.get(): index out of bounds');
        var v = t.a[i];
        return toJSTable(v, t.type);
    },
    set: function set(index, value) {
        var t = slot(this, S_TAB, 'WebAssembly.Table');
        if (arguments.length < 1) throw new TypeError('WebAssembly.Table.set(): Argument 0 is required');
        var i = tableIndex(t, index);
        if (i < 0 || i >= t.a.length) throw new RangeError('WebAssembly.Table.set(): index out of bounds');
        var v = arguments.length < 2 || value === undefined ? defaultRef(t.type) : toWasm(value, t.type);
        t.a[i] = v;
    }
});
function toJSTable(v, t) {
    if (v === undefined) return undefined;
    return toJS(v, t);
}
defGetter(Table.prototype, 'length', function () {
    var t = slot(this, S_TAB, 'WebAssembly.Table');
    return t.at === 'i64' ? BigInt(t.a.length) : t.a.length;
});
toTag(Table.prototype, 'WebAssembly.Table');

/* ---- WebAssembly.Global ---- */
function valueType(s) {
    switch (String(s)) {
    case 'i32': return 'i32';
    case 'i64': return 'i64';
    case 'f32': return 'f32';
    case 'f64': return 'f64';
    case 'v128': return 'v128';
    }
    return elemType(s);
}
function Global(desc, v) {
    if (!new.target) throw new TypeError("Constructor WebAssembly.Global requires 'new'");
    if (desc === null || (typeof desc !== 'object' && typeof desc !== 'function')) throw new TypeError('WebAssembly.Global(): Argument 0 must be a global descriptor');
    var mut = !!desc.mutable;
    var t = valueType(desc.value);
    if (t === 'v128') throw new TypeError('WebAssembly.Global(): v128 is not allowed');
    var val;
    if (arguments.length < 2 || v === undefined) {
        val = t === 'i64' ? 0n : t === 'i32' || t === 'f32' || t === 'f64' ? 0 : defaultRef(t);
        if (val === null && !t.n) throw new TypeError('a non-nullable global needs a value');
    } else val = toWasm(v, t);
    var g = new GlobalCell(t, mut, val);
    hide(this, S_GLB, g);
    g.js = this;
}
function globalJS(g) {
    if (g.js) return g.js;
    var o = Object.create(Global.prototype);
    hide(o, S_GLB, g);
    g.js = o;
    return o;
}
function globalGet(g) {
    if (g.type === 'v128') throw new TypeError('WebAssembly.Global: v128 value');
    if (g.v === undefined) return undefined;
    return toJS(g.v, g.type);
}
defMethods(Global.prototype, {
    valueOf: function valueOf() { return globalGet(slot(this, S_GLB, 'WebAssembly.Global')); }
});
defGetter(Global.prototype, 'value', function () { return globalGet(slot(this, S_GLB, 'WebAssembly.Global')); }, function (v) {
    var g = slot(this, S_GLB, 'WebAssembly.Global');
    if (!g.mut) throw new TypeError("Can't set the value of an immutable global.");
    if (arguments.length < 1) throw new TypeError('value is required');
    g.v = toWasm(v, g.type);
});
toTag(Global.prototype, 'WebAssembly.Global');

/* ---- WebAssembly.Tag ---- */
function canonFuncType(params, results) {
    var key = 'F(' + params.map(function (t) { return vtKey(t, 0, 0, []); }).join(',') + '|' + results.map(function (t) { return vtKey(t, 0, 0, []); }).join(',') + ')';
    return canonicalize([{ kind: 'func', params: params, results: results, final: true, sup: -1 }], 1e9, []);
}
function Tag(desc) {
    if (!new.target) throw new TypeError("Constructor WebAssembly.Tag requires 'new'");
    if (desc === null || (typeof desc !== 'object' && typeof desc !== 'function')) throw new TypeError('WebAssembly.Tag(): Argument 0 must be a tag descriptor');
    var ps = desc.parameters;
    if (ps === null || ps === undefined || typeof ps[Symbol.iterator] !== 'function') throw new TypeError('WebAssembly.Tag(): parameters must be a sequence');
    var types = Array.from(ps).map(valueType);
    var to = new TagObj(canonFuncType(types, []));
    hide(this, S_TAG, to);
    to.js = this;
}
function tagJS(t) {
    if (t.js) return t.js;
    var o = Object.create(Tag.prototype);
    hide(o, S_TAG, t);
    t.js = o;
    return o;
}
toTag(Tag.prototype, 'WebAssembly.Tag');
JSTAG_OBJ.type = canonFuncType([EXTERNREF], []);

/* ---- the namespace ---- */
function asyncDo(f) {
    return new Promise(function (res, rej) {
        Promise.resolve().then(function () {
            try { res(f()); } catch (e) { rej(e); }
        });
    });
}
var WA = {};
defMethods(WA, {
    validate: function validate(bytes) {
        if (arguments.length < 1) throw new TypeError('WebAssembly.validate requires 1 argument');
        var b = bufferBytes(bytes);
        try { decodeModule(b); return true; } catch (e) { if (e instanceof CompileError) return false; throw e; }
    },
    compile: function compile(bytes) {
        var b;
        try { b = bufferBytes(bytes); } catch (e) { return Promise.reject(e); }
        return asyncDo(function () {
            var m = Object.create(Module.prototype);
            hide(m, S_MOD, decodeModule(b));
            return m;
        });
    },
    instantiate: function instantiate(source, importObject) {
        if (source !== null && typeof source === 'object' && Object.prototype.hasOwnProperty.call(source, S_MOD)) {
            var M0 = source[S_MOD];
            if (importObject !== undefined && importObject !== null && typeof importObject !== 'object' && typeof importObject !== 'function')
                return Promise.reject(new TypeError('imports argument must be an object'));
            return asyncDo(function () { return makeInstance(M0, importObject); });
        }
        var b;
        try { b = bufferBytes(source); } catch (e) { return Promise.reject(e); }
        if (importObject !== undefined && importObject !== null && typeof importObject !== 'object' && typeof importObject !== 'function')
            return Promise.reject(new TypeError('imports argument must be an object'));
        return asyncDo(function () {
            var M = decodeModule(b);
            var m = Object.create(Module.prototype);
            hide(m, S_MOD, M);
            return m;
        }).then(function (m) {
            return asyncDo(function () { return { module: m, instance: makeInstance(m[S_MOD], importObject) }; });
        });
    },
    compileStreaming: function compileStreaming(source) {
        return streamBytes(source).then(function (b) {
            var m = Object.create(Module.prototype);
            hide(m, S_MOD, decodeModule(b));
            return m;
        });
    },
    instantiateStreaming: function instantiateStreaming(source, importObject) {
        return streamBytes(source).then(function (b) {
            var M = decodeModule(b);
            var m = Object.create(Module.prototype);
            hide(m, S_MOD, M);
            return { module: m, instance: makeInstance(M, importObject) };
        });
    }
});
/* (a Response, or a promise of one: its body if it is a Wasm one) */
function streamBytes(source) {
    return Promise.resolve(source).then(function (resp) {
        var Resp = global.Response;
        if (!Resp || !(resp instanceof Resp)) throw new TypeError('WebAssembly: argument must be a Response or a promise of one');
        var ct = resp.headers && resp.headers.get('Content-Type');
        if (ct === null || ct === undefined || String(ct).trim().toLowerCase() !== 'application/wasm') throw new TypeError('WebAssembly: response has an incorrect MIME type, expected application/wasm');
        if (resp.type !== 'basic' && resp.type !== 'cors' && resp.type !== 'default') throw new TypeError('WebAssembly: response type is not allowed');
        if (!resp.ok) throw new TypeError('WebAssembly: HTTP status is not ok');
        if (resp.bodyUsed) throw new TypeError('WebAssembly: response body already used');
        return resp.arrayBuffer().then(function (ab) { return new Uint8Array(ab); });
    });
}
function defCtor(name, C, len) {
    Object.defineProperty(C, 'name', { value: name, configurable: true });
    Object.defineProperty(C, 'length', { value: len, configurable: true });
    Object.defineProperty(WA, name, { value: C, writable: true, enumerable: false, configurable: true });
}
defCtor('Module', Module, 1);
defCtor('Instance', Instance, 1);
defCtor('Memory', Memory, 1);
defCtor('Table', Table, 1);
defCtor('Global', Global, 1);
defCtor('Tag', Tag, 1);
defCtor('Exception', WasmExn, 1);
Object.defineProperty(WA, 'CompileError', { value: CompileError, writable: true, enumerable: false, configurable: true });
Object.defineProperty(WA, 'LinkError', { value: LinkError, writable: true, enumerable: false, configurable: true });
Object.defineProperty(WA, 'RuntimeError', { value: RuntimeError, writable: true, enumerable: false, configurable: true });
Object.defineProperty(WA, 'JSTag', { value: tagJS(JSTAG_OBJ), writable: false, enumerable: true, configurable: false });
Object.defineProperty(WA, Symbol.toStringTag, { value: 'WebAssembly', configurable: true });
for (var k_ in { Module: 1, Instance: 1, Memory: 1, Table: 1, Global: 1, Tag: 1, Exception: 1 }) {
    Object.defineProperty(WA[k_], 'prototype', { writable: false, enumerable: false, configurable: false });
}
Object.defineProperty(global, 'WebAssembly', { value: WA, writable: true, enumerable: false, configurable: true });
if (win && win !== global) { try { Object.defineProperty(win, 'WebAssembly', { value: WA, writable: true, enumerable: false, configurable: true }); } catch (e) { } }


})


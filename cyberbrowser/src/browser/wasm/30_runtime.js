
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

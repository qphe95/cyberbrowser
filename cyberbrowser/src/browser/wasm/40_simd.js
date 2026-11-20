
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

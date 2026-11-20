
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

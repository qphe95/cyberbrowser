
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

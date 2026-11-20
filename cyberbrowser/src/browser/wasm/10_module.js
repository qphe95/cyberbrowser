
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

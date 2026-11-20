
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

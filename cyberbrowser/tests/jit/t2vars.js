// T2's closure and global variable accesses (t2_var_get, t2_var_put) and
// inline property adds: each function is warmed until T2 compiles it, then
// meets the cases its fast paths leave to the interpreter.  Output must
// match the interpreter's.
var out = [];
function T(name, f) {
    var r;
    try { r = f(); } catch (e) { r = 'threw ' + e.name + ': ' + e.message; }
    out.push(name + ': ' + JSON.stringify(r));
}
var N = 3000;

T('closure-reads-and-writes', function () {
    function make() {
        var count = 0, step = 2;
        return { inc: function () { count += step; return count; }, get: function () { return count; },
                 setStep: function (s) { step = s; } };
    }
    var c = make(), s = 0;
    for (var i = 0; i < N; i++) s += c.inc();
    c.setStep('x');
    var r = c.inc();
    return [s, r, c.get()];
});

T('closure-tdz', function () {
    function outer(early) {
        function read() { return v; }
        function write(x) { v = x; return v; }
        var r = [];
        if (early) {
            try { r.push(read()); } catch (e) { r.push(e.name); }
            try { r.push(write(1)); } catch (e) { r.push(e.name); }
        }
        let v = 5;
        for (var i = 0; i < 10; i++) r.push(read() + write(i));
        return r.length + ':' + r[0] + ':' + r[r.length - 1];
    }
    var s;
    for (var k = 0; k < 400; k++) s = outer(false);
    return [s, outer(true)];
});

var gCounter = 0;
let gLet = 1;
const gConst = 7;
T('globals-declared-and-builtins', function () {
    function f(x) { gCounter += x; return gCounter + gLet + gConst + Math.max(x, 2) + Object.keys({ a: 1 }).length; }
    var s = 0;
    for (var i = 0; i < N; i++) s += f(1);
    gLet = 'L';
    return [s, f(2), gCounter];
});

T('globals-undefined-getter-const', function () {
    function readMissing() { return typeof notDefinedAnywhere === 'undefined' ? 'undef' : 'def'; }
    function readThrows() { return notDefinedEither; }
    function readG() { return lateGlobal; }
    function writeConst() { gConst = 1; }
    globalThis.lateGlobal = 1;
    var s = 0, r = [];
    for (var i = 0; i < N; i++) { s += readG(); readMissing(); }
    r.push(s, readMissing());
    try { readThrows(); r.push('no throw'); } catch (e) { r.push(e.name); }
    Object.defineProperty(globalThis, 'lateGlobal', { get: function () { return 'getter'; }, configurable: true });
    r.push(readG());
    try { writeConst(); r.push('wrote'); } catch (e) { r.push(e.name); }
    delete globalThis.lateGlobal;
    try { r.push(readG()); } catch (e) { r.push(e.name); }
    return r;
});

T('implicit-global-writes', function () {
    function w(v) { implicitG = v; return implicitG; }
    var s = 0;
    for (var i = 0; i < N; i++) s += w(i);
    return [s, typeof implicitG, delete globalThis.implicitG, w('again')];
});

T('adds-in-constructors', function () {
    function P(x, y) { this.x = x; this.y = y; this.z = x + y; }
    function Big() { for (var i = 0; i < 20; i++) this['p' + i] = i; this.last = 1; }
    function addLater(o) { o.extra = 1; return o; }
    var s = 0;
    for (var i = 0; i < N; i++) { var p = new P(i, 1); s += p.z; if (i % 50 === 0) s += Object.keys(new Big()).length; }
    var objs = [], c = 0;
    for (var j = 0; j < N; j++) objs.push(addLater({ a: j }));
    for (var j = 0; j < objs.length; j++) c += objs[j].extra;
    return [s, c, Object.keys(addLater({ a: 1, b: 2 }))];
});

T('adds-guarded', function () {
    'use strict';
    function C() { this.a = 1; }
    function add(o, v) { o.b = v; return o; }
    var r = [];
    for (var i = 0; i < N; i++) add(new C(), i);
    var ne = Object.preventExtensions(new C());
    try { add(ne, 1); r.push('added'); } catch (e) { r.push(e.name); }
    Object.defineProperty(C.prototype, 'b', { set: function (v) { r.push('setter ' + v); }, configurable: true });
    add(new C(), 5);
    delete C.prototype.b;
    Object.defineProperty(C.prototype, 'b', { value: 0, writable: false, configurable: true });
    try { add(new C(), 6); r.push('added'); } catch (e) { r.push(e.name); }
    delete C.prototype.b;
    var arr = [1, 2];
    add(arr, 9);
    r.push(arr.b, Object.keys(add(new C(), 3)));
    return r;
});

console.error('JITTEST-BEGIN');
for (var i = 0; i < out.length; i++) console.error(out[i]);
console.error('JITTEST-END');

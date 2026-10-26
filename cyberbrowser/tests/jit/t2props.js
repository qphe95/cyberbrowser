// T2's inline property and array access (JS_PERF_DESIGN.md section 6.7,
// step 5d): each function is warmed until T2 compiles its sites from the
// feedback caches, then sent objects its shape guards, prototype epochs and
// array guards must reject.  Output must match the interpreter's.
var out = [];
function T(name, f) {
    var r;
    try { r = f(); } catch (e) { r = 'threw ' + e.name + ': ' + e.message; }
    out.push(name + ': ' + JSON.stringify(r));
}
var N = 3000;

T('own-fields-mono-poly', function () {
    function get(o) { return o.x + o.y; }
    function P(x, y) { this.x = x; this.y = y; }
    var s = 0, shapes = [new P(1, 2), { x: 3, y: 4 }, { y: 5, x: 6 }, { z: 0, x: 7, y: 8 }];
    for (var i = 0; i < N; i++) s += get(shapes[i < N / 2 ? 0 : i % 4]);
    var o5 = { a: 1, b: 2, c: 3, x: 10, y: 20 };   // a fifth shape, after compiling
    return [s, get(o5), get({ x: 'a', y: 'b' })];
});

T('proto-method-then-change', function () {
    function A() { this.v = 2; }
    A.prototype.f = function () { return this.v; };
    function call(o) { return o.f(); }
    var a = new A(), s = 0;
    for (var i = 0; i < N; i++) s += call(a);
    A.prototype.f = function () { return 100; };       // the prototype's property changes
    var r1 = call(a);
    a.f = function () { return 7; };                     // shadowed on the receiver
    var r2 = call(a);
    Object.setPrototypeOf(a, { f: function () { return 9; } });
    return [s, r1, r2, call(a), call(new A())];
});

T('absent-then-defined', function () {
    function get(o) { return o.missing; }
    var o = { q: 1 }, n = 0;
    for (var i = 0; i < N; i++) if (get(o) === undefined) n++;
    Object.prototype.missing = 'late';
    var r = get(o);
    delete Object.prototype.missing;
    return [n, r, get(o)];
});

T('getter-added-after-compile', function () {
    function P() { this.k = 3; }
    function get(o) { return o.w; }
    P.prototype.w = 1;
    var p = new P(), s = 0;
    for (var i = 0; i < N; i++) s += get(p);
    Object.defineProperty(P.prototype, 'w', { get: function () { return this.k * 10; }, configurable: true });
    return [s, get(p)];
});

T('put-own-then-readonly-frozen-watched', function () {
    'use strict';
    function set(o, v) { o.x = v; }
    function C() { this.x = 0; }
    var c = new C();
    for (var i = 0; i < N; i++) set(c, i);
    var r = [c.x];
    var ro = new C();
    Object.defineProperty(ro, 'x', { writable: false });
    try { set(ro, 1); r.push('wrote'); } catch (e) { r.push(e.name); }
    var fr = Object.freeze(new C());
    try { set(fr, 1); r.push('wrote'); } catch (e) { r.push(e.name); }
    var acc = { get x() { return 1; }, set x(v) { r.push('setter ' + v); } };
    set(acc, 5);
    r.push(ro.x, fr.x);
    return r;
});

T('put-adds-and-deletes', function () {
    function set(o, v) { o.n = v; }
    var objs = [], s = 0;
    for (var i = 0; i < N; i++) { var o = { m: i }; if (i & 1) o.n = 0; set(o, i); s += o.n; objs.push(o); }
    var d = { m: 1, n: 2 };
    delete d.n;
    set(d, 9);
    return [s, d.n, Object.keys(d)];
});

T('array-get-in-and-out-of-bounds', function () {
    function at(a, i) { return a[i]; }
    var a = [], s = 0;
    for (var i = 0; i < 100; i++) a.push(i * 2);
    for (var k = 0; k < N; k++) s += at(a, k % 100);
    return [s, at(a, 100), at(a, -1), at(a, 1.5), at(a, '3'), at([1, , 3], 1), at('str', 1),
            at({ 0: 'obj' }, 0), at(new Int32Array([5, 6]), 1), at(a, 2147483647)];
});

T('array-put-and-conversions', function () {
    'use strict';
    function put(a, i, v) { a[i] = v; }
    var a = [], r = [];
    for (var i = 0; i < 50; i++) a.push(0);
    for (var k = 0; k < N; k++) put(a, k % 50, k);
    r.push(a[0], a[49], a.length);
    put(a, 50, 'append');               // out of bounds: the length grows
    r.push(a.length, a[50]);
    var fr = Object.freeze([1, 2, 3]);
    try { put(fr, 0, 9); r.push('wrote'); } catch (e) { r.push(e.name); }
    r.push(fr[0]);
    var sp = [1, 2, 3];
    delete sp[1];                       // no longer a fast array
    put(sp, 1, 'x');
    r.push(sp.join());
    var ta = new Float64Array(2);
    put(ta, 1, 2.5);
    r.push(ta[1]);
    return r;
});

T('array-sum-loops', function () {
    function sum(a) { var t = 0; for (var i = 0; i < a.length; i++) t += a[i]; return t; }
    function fill(a, n) { for (var i = 0; i < n; i++) a[i] = i * i; return a; }
    var s = 0;
    for (var k = 0; k < 200; k++) s += sum(fill(new Array(100).fill(0), 100));
    return [s, sum([1.5, 2.5]), sum(['a', 'b']), sum([1, , 3])];
});

T('gc-churn-shapes-inline', function () {
    function get(o) { return o.k; }
    var s = 0;
    for (var round = 0; round < 30; round++) {
        var objs = [];
        for (var i = 0; i < 300; i++) { var o = {}; o['f' + (round % 3)] = 1; o.k = i; objs.push(o); }
        for (var j = 0; j < objs.length; j++) s += get(objs[j]);
        var junk = [];
        for (var g = 0; g < 3000; g++) junk.push({ a: g, b: [g] });
    }
    return s;
});

console.error('JITTEST-BEGIN');
for (var i = 0; i < out.length; i++) console.error(out[i]);
console.error('JITTEST-END');

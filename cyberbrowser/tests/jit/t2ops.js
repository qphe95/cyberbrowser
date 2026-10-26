// Instructions T2 calls C for directly rather than through their helpers
// (JS_PERF_DESIGN.md section 6.7, N5): add of strings and objects, loose
// equality, in, instanceof, object literals and their fields, closures,
// element accesses that are not a fast array's, and the stack shuffles of
// compound assignments; with valueOf, getters, proxies and traps that run
// JavaScript or throw in the middle.  Output must match the interpreter's.
var out = [];
function T(name, f) {
    var r;
    try { r = f(); } catch (e) { r = 'threw ' + e.name + ': ' + e.message; }
    out.push(name + ': ' + JSON.stringify(r));
}
var N = 3000;

T('add-mixed', function () {
    function f(a, b) { return a + b; }
    var r = [];
    for (var i = 0; i < N; i++) r.length = 0, r.push(f('s' + (i & 3), i), f(i, 'x'));
    var n = 0, captured = 1;
    var o = { valueOf: function () { n++; captured += 10; return 5; } };
    function g(a) { var k = captured; return a + o + k + captured; }
    for (var j = 0; j < N; j++) g(j);
    r.push(f(o, 1), f('a', o), f(null, 1), f(undefined, 'u'), f([1, 2], {}), n, g(1), captured);
    var bad = { valueOf: function () { throw new Error('vo'); } };
    try { f(bad, 1); } catch (e) { r.push(e.message); }
    return r;
});

T('loose-equality', function () {
    function eq(a, b) { return a == b; }
    function ne(a, b) { return a != b; }
    var s = 0;
    for (var i = 0; i < N; i++) s += eq(i, '' + i) + ne(i & 1, '0') + eq(null, undefined) + eq({}, 'x');
    var calls = 0;
    var o = { valueOf: function () { calls++; return 7; } };
    var bad = { toString: function () { throw new TypeError('ts'); }, valueOf: undefined };
    var r = [s, eq(o, 7), ne(o, '7'), eq(o, o), calls, eq(1n, 1), eq(Symbol.iterator, Symbol.iterator)];
    try { eq(bad, 'x'); } catch (e) { r.push(e.message); }
    return r;
});

T('in-and-instanceof', function () {
    function has(k, o) { return k in o; }
    function isA(o, C) { return o instanceof C; }
    function A() {}
    var s = 0;
    for (var i = 0; i < N; i++) s += has('x', { x: i }) + has(i & 3, [1, 2]) + isA(new A(), A) + isA({}, A);
    var log = [];
    var px = new Proxy({}, { has: function (t, k) { log.push(k); return k === 'y'; } });
    var weird = { [Symbol.hasInstance]: function (v) { log.push('hi'); return v === 1; } };
    var r = [s, has('y', px), has('z', px), isA(1, weird), log];
    try { has('x', 5); } catch (e) { r.push(e.name); }
    try { isA({}, {}); } catch (e) { r.push(e.name); }
    return r;
});

T('object-literals-and-closures', function () {
    function mk(i) { var c = i * 2; return { a: i, b: 'v' + i, get c() { return c; }, f: function () { return c + this.a; } }; }
    var s = 0;
    for (var i = 0; i < N; i++) { var o = mk(i); s += o.a + o.c + o.f(); }
    function counter() { var n = 0; return { inc: function () { return ++n; }, get: function () { return n; } }; }
    var cs = [];
    for (var j = 0; j < N; j++) { var c = counter(); c.inc(); c.inc(); cs.push(c.get()); }
    return [s, cs.length, cs[0], cs[N - 1], Object.keys(mk(3))];
});

T('elements-off-the-fast-path', function () {
    function get(o, k) { return o[k]; }
    function put(o, k, v) { o[k] = v; return o[k]; }
    var s = 0, ta = new Int32Array(8), str = 'abcdef';
    for (var i = 0; i < N; i++) {
        s += get(ta, i & 7) + (get(str, i % 6) === 'c') + get({ k1: 1, k2: 2 }, 'k' + (1 + (i & 1)));
        put(ta, i & 7, i);
        put({}, 'p' + (i & 3), i);
    }
    var holey = [1, , 3];
    var log = [];
    var px = new Proxy([], { get: function (t, k) { log.push('g' + String(k)); return 9; },
                            set: function (t, k, v) { log.push('s' + String(k)); return true; } });
    var r = [s, get(holey, 1), get(holey, 5), get(px, 2), put(px, 1, 4), get(arguments, 0), log];
    try { get(null, 1); } catch (e) { r.push(e.name + ': ' + e.message); }
    try { put(undefined, 'a', 1); } catch (e) { r.push(e.name + ': ' + e.message); }
    var frozen = Object.freeze([1, 2]);
    r.push(put(frozen, 0, 9));
    (function () { 'use strict'; try { frozen[0] = 3; } catch (e) { r.push(e.name); } })();
    return r;
});

T('compound-assignments', function () {
    function f(o, a, i) {
        o.x += 1;
        o.y = o.y * 2 + 1;
        a[i] += 2;
        a[i + 1] = a[i] || 5;
        o.n = (o.n || 0) + 1;
        var t = o.z = a[i] = i;
        return o.x + o.y + a[i] + a[i + 1] + o.n + t;
    }
    var s = 0, o = { x: 0, y: 0 }, a = [0, 0, 0, 0];
    for (var i = 0; i < N; i++) s = (s + f(o, a, i & 1)) | 0;
    var px = new Proxy({ x: 1, y: 1 }, { set: function (t, k, v) { t[k] = v * 10; return true; } });
    return [s, o.x, o.y, a, f(px, [1, 2, 3], 0), px.x];
});

T('destructuring', function () {
    function f(o) { var { a, b: [c, d] = [1, 2], ...rest } = o; return a + c + d + Object.keys(rest).length; }
    var s = 0;
    for (var i = 0; i < N; i++) s += f({ a: i, b: [i, 1], e: 1, g: 2 });
    var x = 1, y = 2;
    for (var j = 0; j < N; j++) [x, y] = [y, x + y & 1023];
    return [s, f({ a: 1 }), x, y];
});

console.error('JITTEST-BEGIN');
for (var i = 0; i < out.length; i++) console.error(out[i]);
console.error('JITTEST-END');

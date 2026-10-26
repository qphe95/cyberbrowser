// T2's direct calls, push_this, let/const checks and equality by identity
// (JS_PERF_DESIGN.md section 6.7, step 5e): each function is warmed until
// T2 compiles it and its callees, then called in the ways direct calls
// must still get right.  Output must match the interpreter's.
var out = [];
function T(name, f) {
    var r;
    try { r = f(); } catch (e) { r = 'threw ' + e.name + ': ' + e.message; }
    out.push(name + ': ' + JSON.stringify(r));
}
var N = 3000;

T('recursion-and-overflow', function () {
    function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
    function deep(n) { return n === 0 ? 0 : 1 + deep(n - 1); }
    var s = 0;
    for (var i = 0; i < 30; i++) s += fib(15);
    var r;
    try { deep(1e7); r = 'no overflow'; } catch (e) { r = e.name; }
    return [s, deep(1000), r];
});

T('exceptions-through-frames', function () {
    function thrower(x) { if (x > 5) throw new RangeError('big ' + x); return x; }
    function mid(x) { return thrower(x) * 2; }
    function outer(x) { try { return mid(x); } catch (e) { return e.message; } }
    var r = [], s = 0;
    for (var i = 0; i < N; i++) s += outer(i % 5);
    r.push(s, outer(9), outer(3));
    try { mid(7); } catch (e) { r.push(e.name); }
    return r;
});

T('arguments-rest-defaults', function () {
    function a(x, y) { return arguments.length + ':' + x + ':' + y; }
    function rest(x, ...more) { return x + more.length; }
    function def(x, y = 10) { return x + y; }
    var s = 0;
    for (var i = 0; i < N; i++) s += rest(i, 1, 2) + def(i);
    return [s, a(1), a(1, 2, 3), rest(5), def(1, undefined), def(1, null)];
});

T('constructors', function () {
    function P(x) { this.x = x; }
    function Q(x) { this.x = x; return { y: x }; }
    function R(x) { this.x = x; return 5; }
    class C { constructor(v) { this.v = v; } get dbl() { return this.v * 2; } }
    class D extends C { constructor(v) { super(v + 1); } }
    var s = 0;
    for (var i = 0; i < N; i++) s += new P(i).x + new C(i).dbl + new D(i).v;
    return [s, new Q(3).y, new Q(3).x, new R(4).x, new D(1) instanceof C];
});

T('this-sloppy-strict', function () {
    function sloppy() { return typeof this; }
    function strict() { 'use strict'; return this === undefined ? 'undef' : typeof this; }
    var o = { m: function () { return this.k; }, k: 42 };
    var r = [], c = 0;
    for (var i = 0; i < N; i++) c += sloppy().length + strict().length + o.m();
    r.push(c, sloppy(), strict(), sloppy.call(5), strict.call(5), o.m.call({ k: 1 }), o.m.apply(o));
    var bound = o.m.bind({ k: 'b' });
    r.push(bound());
    return r;
});

T('natives-and-tail', function () {
    function mx(a, b) { return Math.max(a, b); }
    function push(arr, v) { return arr.push(v); }
    function tail(n, acc) { if (n === 0) return acc; return tail(n - 1, acc + n); }
    var a = [], s = 0;
    for (var i = 0; i < N; i++) s += mx(i, 100) + push(a, i);
    return [s, a.length, tail(1000, 0), mx('a', 3), mx()];
});

T('closures-and-non-functions', function () {
    function counter() { var c = 0; return function () { return ++c; }; }
    function callIt(f) { return f(); }
    var k = counter(), s = 0;
    for (var i = 0; i < N; i++) s += callIt(k);
    var r = [s];
    try { callIt(5); r.push('called'); } catch (e) { r.push(e.name); }
    try { callIt(undefined); r.push('called'); } catch (e) { r.push(e.name); }
    try { new (function () {}.bind())(); r.push('ok'); } catch (e) { r.push(e.name); }
    return r;
});

T('equality-identity', function () {
    function isNull(x) { return x == null; }
    function same(a, b) { return a === b; }
    function differ(a, b) { return a !== b; }
    function loose(a, b) { return a == b; }
    var o = {}, p = {}, sym = Symbol('s'), c = 0;
    for (var i = 0; i < N; i++)
        c += isNull(i & 1 ? o : null) + same(o, i & 1 ? o : p) + differ(sym, i & 2 ? sym : o) + loose(null, undefined);
    return [c, isNull(undefined), isNull(0), isNull(''), same(1, 1), same('a', 'a'), same(NaN, NaN),
            differ(1.5, 1.5), loose(1, '1'), loose(true, 1), loose(o, '[object Object]'), loose(sym, sym)];
});

T('let-before-init', function () {
    function f(flag) {
        if (flag) { try { return g(); } catch (e) { return e.name; } }
        let v = 3;
        function g() { return v; }
        return g();
    }
    var s = 0;
    for (var i = 0; i < N; i++) s += f(false);
    return [s, f(true)];
});

console.error('JITTEST-BEGIN');
for (var i = 0; i < out.length; i++) console.error(out[i]);
console.error('JITTEST-END');

// Differential test for frames on the JS stack (JS_PERF_DESIGN.md section
// 5.3; see basic.js): calls of functions nothing refers to after the call
// get their frames there, mixed with heap frames (closures, generators,
// mapped arguments, eval), through exceptions, natives and deep recursion.
var log = [];
function out(x) { log.push(String(x)); }
function t(name, f) {
    try { out(name + ': ' + f()); }
    catch (e) { out(name + ' threw: ' + (e && e.name) + ': ' + (e && e.message)); }
}

function leaf(a, b) { return a * 2 + (b | 0); }
function mid(a) { var s = 0; for (var i = 0; i < 3; i++) s += leaf(a, i); return s; }
function top(n) { var s = 0; for (var i = 0; i < n; i++) s += mid(i); return s; }

t('nested', function () { return top(50); });
t('backtrace', function () {
    function inner() { return new Error('x').stack; }
    function outer() { return inner(); }
    var st = outer();
    // Function names in order, not file positions.
    return st.split('\n').map(function (l) { var m = l.match(/at (\w+)/); return m ? m[1] : ''; })
        .filter(Boolean).slice(0, 3).join(',');
});
t('throw-through', function () {
    function thrower(x) { if (x > 3) throw new RangeError('deep ' + x); return thrower(x + 1) + 1; }
    function catcher() { try { return thrower(0); } catch (e) { return e.message + '/' + leaf(1, 2); } }
    var r = [];
    for (var i = 0; i < 5; i++) r.push(catcher());
    return r.join(' ');
});
t('finally', function () {
    var log2 = [];
    function f(x) { try { if (x) throw 1; return 'ok'; } finally { log2.push('f' + x); } }
    function g(x) { try { return f(x); } catch (e) { return 'caught'; } }
    return [g(0), g(1), g(0), log2.join('')].join(' ');
});
t('deep-recursion', function () {
    function depth(n) { return n == 0 ? 0 : 1 + depth(n - 1); }
    var r = [depth(500), depth(1500)];
    function forever(n) { return forever(n + 1) + 1; }
    try { forever(0); r.push('no overflow'); } catch (e) { r.push(e.name); }
    r.push(depth(100));      // the stacks unwound properly
    return r.join(' ');
});
t('arguments', function () {
    function strictArgs() { 'use strict'; return arguments.length + ':' + Array.prototype.join.call(arguments, ''); }
    function sloppyArgs(a) { arguments[0] = 'changed'; return a; }       // mapped: a heap frame
    function restArgs(...r) { return r.length; }
    var r = [];
    for (var i = 0; i < 3; i++) r.push(strictArgs(1, 2, i), sloppyArgs('orig'), restArgs(1, 2, 3));
    return r.join(' ');
});
t('closures-around', function () {
    // A heap frame (it makes a closure) calling stack frames and back.
    function maker(k) { var c = 0; return function (x) { c += leaf(x, k); return c; }; }
    var fns = [maker(1), maker(2)], s = 0;
    for (var i = 0; i < 10; i++) s += fns[i & 1](i);
    return s;
});
t('generators', function () {
    function* gen(n) { for (var i = 0; i < n; i++) yield leaf(i, 1); }
    function sum(it) { var s = 0; for (var v of it) s += mid(v); return s; }
    return [sum(gen(5)), sum(gen(10))].join(' ');
});
t('natives-reenter', function () {
    var arr = [];
    for (var i = 0; i < 30; i++) arr.push(i);
    var mapped = arr.map(function (x) { return leaf(x, 1); });
    var sorted = arr.slice().sort(function (a, b) { return leaf(b, 0) - leaf(a, 0); });
    var reduced = arr.reduce(function (acc, x) { return acc + mid(x); }, 0);
    return [mapped.slice(0, 5).join(','), sorted.slice(0, 3).join(','), reduced,
            JSON.stringify({ a: 1, b: [2, 3] }, function (k, v) { return typeof v == 'number' ? leaf(v, 0) : v; })].join(' ');
});
t('eval-and-with', function () {
    function viaEval(x) { return eval('x + 1'); }
    function caller() { var s = 0; for (var i = 0; i < 3; i++) s += viaEval(i) + leaf(i, 0); return s; }
    var o = { q: 5 };
    function withUser() { with (o) { return q + leaf(1, 1); } }
    return [caller(), withUser()].join(' ');
});
t('args-in-place', function () {
    // A callee's arguments can be its caller's operand-stack slots.
    function mutate(a, b, c) { a = a + 100; b = 'b' + b; c = c === undefined ? 'pad' : c; return a + ':' + b + ':' + c; }
    function extra(a) { return a + '/' + arguments.length + '/' + Array.prototype.slice.call(arguments, 1).join(''); }
    function strictExtra(a) { 'use strict'; a = 0; return arguments[0] + ',' + arguments.length; }
    var o = { k: 3, m: function (x, y) { x = x * this.k; return x + (y | 0); } };
    function C(a, b) { a = a + 1; this.v = a + (b || 0); }
    var r = [], v = 7;
    for (var i = 0; i < 4; i++) {
        var before = v;
        r.push(mutate(v, i), extra(i, 'x', 'y'), strictExtra(i + 1, 2), o.m(v, i), new C(i, v).v,
               mutate(mutate(i, i), extra(v)), Math.max(leaf(i, 1), mutate.length), v === before);
        r.push(mutate.apply(null, [i]), mutate.call(null, i, i, i), mutate(...[1, 2]));
    }
    return r.join(' ');
});
t('zero-params', function () {
    // Functions without parameters, called at the top of their caller's
    // operand stack, whose own callees take arguments in place: their
    // frames must still be marked (they once were not), so what they hold
    // survives collections.
    function make(i, s) { return { i: i, s: s + i, a: [i, i + 1] }; }
    function keep(o, p) { return o.i + p.a[1] + o.s.length; }
    function z() { var o = make(1, 'z'), p = make(2, 'q'); var t = 0; for (var k = 0; k < 200; k++) t += keep(make(k, 'x'), p); return t + keep(o, p); }
    function y() { return z() + z(); }
    function x() { var s = 0; for (var i = 0; i < 40; i++) s += y(); return s; }
    var r = 0;
    for (var round = 0; round < 10; round++) r += x();
    return r;
});
t('gc-churn', function () {
    // Allocation in deep stack frames, so collections run while they do.
    function build(n) { var a = []; for (var i = 0; i < n; i++) a.push({ v: i, s: 'x' + i }); return a; }
    function sumv(a) { var s = 0; for (var i = 0; i < a.length; i++) s += a[i].v + a[i].s.length; return s; }
    function rec(n) { return n == 0 ? sumv(build(2000)) : rec(n - 1) + sumv(build(50)); }
    var r = 0;
    for (var k = 0; k < 20; k++) r += rec(30);
    return r;
});

console.error('JITTEST-BEGIN');
log.forEach(function (l) { console.error(l); });
console.error('JITTEST-END');

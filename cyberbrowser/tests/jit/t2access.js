// T2's property accesses that miss their inline ways (JS_PERF_DESIGN.md
// section 6.7, N5): getters and setters in JavaScript and in C called from
// compiled code, with the caller's captured locals stored around them;
// getters and setters that throw; primitives, proxies, null and undefined
// receivers; sites whose shapes keep changing.  Each function is warmed
// until T2 compiles it, then given the receivers that miss.  Output must
// match the interpreter's.
var out = [];
function T(name, f) {
    var r;
    try { r = f(); } catch (e) { r = 'threw ' + e.name + ': ' + e.message; }
    out.push(name + ': ' + JSON.stringify(r));
}
var N = 3000;

T('getter-sees-captured-local', function () {
    function f(o) {
        var x = 1;
        var g = { get v() { return x; } };
        x = 2;
        var a = g.v;            // the getter reads x: 2
        x = 3;
        return a + o.k + g.v;   // 2 + k + 3
    }
    var s = 0;
    for (var i = 0; i < N; i++) s += f({ k: i & 3 });
    return [s, f({ k: 10 }), f({ get k() { return 100; } })];
});

T('getter-writes-captured-local', function () {
    function f(o) {
        var n = 0;
        var g = { get bump() { n += 10; return n; } };
        var a = o.v;
        var b = g.bump;
        return a + b + n;       // n is 10 after the getter
    }
    var s = 0;
    for (var i = 0; i < N; i++) s += f({ v: i & 7 });
    return [s, f({ get v() { return -1; } })];
});

T('setter-sees-and-writes', function () {
    function f(o, v) {
        var seen = 0;
        var t = { set w(x) { seen = x * 2; } };
        o.p = v;
        t.w = v;
        return seen + o.p;
    }
    var s = 0;
    for (var i = 0; i < N; i++) s += f({ p: 0 }, i & 15);
    var log = [];
    var odd = { set p(x) { log.push(x); } };
    return [s, f(odd, 4), log];
});

T('getter-throws', function () {
    function f(o) {
        var r = 0;
        try { r = o.x + 1; } catch (e) { r = 'caught ' + e.message; }
        return r;
    }
    var s = 0;
    for (var i = 0; i < N; i++) s += f({ x: i & 3 });
    var calls = 0;
    var bad = { get x() { calls++; throw new Error('boom'); } };
    return [s, f(bad), calls, f({ x: 5 })];
});

T('setter-throws', function () {
    function f(o) {
        try { o.y = 7; return o.y; } catch (e) { return e.message; }
    }
    var s = 0;
    for (var i = 0; i < N; i++) s += f({ y: 0 });
    var calls = 0;
    return [s, f({ set y(v) { calls++; throw new TypeError('no y'); } }), calls,
            f(Object.freeze({ y: 1 })), f(Object.defineProperty({}, 'y', { value: 3 }))];
});

T('strict-readonly-throws', function () {
    function f(o) { 'use strict'; try { o.y = 7; return o.y; } catch (e) { return e.name; } }
    var s = 0;
    for (var i = 0; i < N; i++) s += f({ y: 0 });
    return [s, f(Object.freeze({ y: 1 })), f({ get y() { return 9; } })];
});

T('primitives', function () {
    function f(v) { return v.length + ':' + (v.toString ? 'm' : '-'); }
    function g(v) { return v.constructor.name; }
    var r = [];
    for (var i = 0; i < N; i++) { f([1, 2]); g({}); }
    r.push(f('abcd'), f([1]), g(5), g(true), g('s'), g(Symbol('q')), g(10n));
    String.prototype.extra = 42;
    function h(v) { return v.extra; }
    for (var j = 0; j < N; j++) h({ extra: j });
    r.push(h('x'), h(3));
    Object.defineProperty(Number.prototype, 'twice', { get: function () { return this * 2; }, configurable: true });
    r.push(h({ extra: 1 }), (function (n) { return n.twice; })(21));
    delete String.prototype.extra;
    delete Number.prototype.twice;
    return r;
});

T('null-and-undefined', function () {
    function f(o) { return o.x; }
    function p(o) { o.x = 1; return 1; }
    for (var i = 0; i < N; i++) { f({ x: i }); p({ x: 0 }); }
    var r = [];
    try { f(null); } catch (e) { r.push(e.name + ': ' + e.message); }
    try { f(undefined); } catch (e) { r.push(e.name + ': ' + e.message); }
    try { p(null); } catch (e) { r.push(e.name + ': ' + e.message); }
    return r;
});

T('proxies-and-exotics', function () {
    function f(o) { return o.x; }
    function p(o, v) { o.x = v; return o.x; }
    for (var i = 0; i < N; i++) { f({ x: i }); p({ x: 0 }, i); }
    var log = [];
    var px = new Proxy({}, { get: function (t, k) { log.push('get ' + String(k)); return 7; },
                            set: function (t, k, v) { log.push('set ' + String(k) + '=' + v); return true; } });
    var arr = [1, 2, 3];
    arr.x = 'ax';
    return [f(px), p(px, 3), f(arr), p(arr, 'b'), f(function () {}), log];
});

T('shapes-churn', function () {
    function f(o) { return o.a + o.b; }
    function p(o) { o.c = o.a; return o.c; }
    var s = 0;
    for (var i = 0; i < N; i++) {
        var o = {};
        if (i & 1) o.z = 1;
        if (i & 2) o.y = 2;
        if (i & 4) o.w = 3;
        if (i & 8) o.v = 4;
        if (i & 16) o.u = 5;
        o.a = i & 7;
        o.b = 1;
        s += f(o) + p(o);
    }
    return s;
});

T('prototype-changes', function () {
    function A() {}
    A.prototype.m = function () { return 1; };
    function f(o) { return o.m(); }
    var s = 0, a = new A();
    for (var i = 0; i < N; i++) {
        s += f(a);
        if (i === 1000) A.prototype.m = function () { return 2; };
        if (i === 2000) Object.defineProperty(A.prototype, 'm', { get: function () { return function () { return 3; }; } });
    }
    return s;
});

console.error('JITTEST-BEGIN');
for (var i = 0; i < out.length; i++) console.error(out[i]);
console.error('JITTEST-END');

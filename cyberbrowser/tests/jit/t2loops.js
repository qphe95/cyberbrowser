// T2's carried locals (JS_PERF_DESIGN.md section 6.7, step 5f): arguments
// and locals kept in the native frame across blocks and helper calls, in
// one representation.  Each case changes a carried local's type mid-loop,
// leaves through an exit or an exception with carried values live, or
// gives the frame a reason to be observed.  Output must match the
// interpreter's.
var out = [];
function T(name, f) {
    var r;
    try { r = f(); } catch (e) { r = 'threw ' + e.name + ': ' + e.message; }
    out.push(name + ': ' + JSON.stringify(r));
}
var N = 2000;

T('int-counters-and-sums', function () {
    function f(n) { var s = 0; for (var i = 0; i < n; i++) s += i; return s; }
    function g(n) { var c = 0, i = n; while (i-- > 0) c++; return c; }
    var t = 0;
    for (var k = 0; k < 200; k++) t += f(100) + g(50);
    return [t, f(70000), g(0)];
});

T('overflow-mid-loop', function () {
    function f(start, n) { var s = start; for (var i = 0; i < n; i++) s += 1000000; return s; }
    var t = 0;
    for (var k = 0; k < 200; k++) t += f(0, 10);
    return [t, f(2147000000, 5), f(0.5, 3), f(-2147483648, 2)];
});

T('type-changes-mid-loop', function () {
    function f(n, add) { var s = 0; for (var i = 0; i < n; i++) { s += add; if (i === 5) add = 'x'; } return s; }
    function g(arr) { var m = arr[0]; for (var i = 1; i < arr.length; i++) if (arr[i] > m) m = arr[i]; return m; }
    var t = 0;
    for (var k = 0; k < 200; k++) t += g([3, 9, 2, 7]);
    return [t, f(10, 1), g([1.5, 0.5, 2.5]), g(['b', 'a', 'c']), g([1, 'z', 3])];
});

T('exception-reads-locals', function () {
    function f(n) {
        var i = 0, s = 0;
        try {
            for (i = 0; i < n; i++) { s += i; if (i === 7) null.boom; }
        } catch (e) { return 'caught at ' + i + ' s=' + s; }
        return 'done ' + s;
    }
    var r;
    for (var k = 0; k < 300; k++) r = f(5);
    return [r, f(20), f(3)];
});

T('closures-arguments-eval', function () {
    function cl(n) { var fs = []; for (var i = 0; i < n; i++) fs.push(function () { return i; }); return fs[0]() + fs[n - 1](); }
    function lets(n) { var fs = []; for (let i = 0; i < n; i++) fs.push(function () { return i; }); return fs[0]() + fs[n - 1](); }
    function args(a, b) { var s = 0; for (var i = 0; i < 3; i++) { a++; s += arguments[0]; } return s; }
    function ev(n) { var x = 1; for (var i = 0; i < n; i++) eval('x += i'); return x; }
    var t = 0;
    for (var k = 0; k < 300; k++) t += cl(4) + lets(4) + args(1, 2) + ev(3);
    return [t, cl(2), lets(2), args(10), ev(5)];
});

T('loop-at-function-start', function () {
    function f(n) { while (n > 0) { n -= 2; } return n; }
    function g(a) { for (;;) { if (a.length > 3) return a.length; a.push(0); } }
    var t = 0;
    for (var k = 0; k < 300; k++) t += f(11) + g([1]);
    return [t, f(4), f(-1)];
});

T('unassigned-and-tdz', function () {
    function f(c) { var x; if (c) x = 1; for (var i = 0; i < 3; i++) x = x + 1; return x; }
    function g(n) { let s = 0; for (let i = 0; i < n; i++) s += i; return s; }
    var t = 0;
    for (var k = 0; k < 300; k++) t += f(true) + g(5);
    return [t, f(false), f(true), g(0)];
});

T('doubles-and-mixed', function () {
    function f(n) { var x = 0.5; for (var i = 0; i < n; i++) x = x * 1.5 + i; return x; }
    function g(n) { var a = 1; for (var i = 0; i < n; i++) a = i % 2 ? a / 2 : a * 3; return a; }
    var t = 0;
    for (var k = 0; k < 300; k++) t += f(10) + g(10);
    return [t, f(30), g(7)];
});

T('string-builder-and-nested', function () {
    function f(n) { var s = ''; for (var i = 0; i < n; i++) s += i; return s; }
    function g(n) { var c = 0; for (var i = 0; i < n; i++) for (var j = 0; j < i; j++) c += j & 1; return c; }
    var t = 0;
    for (var k = 0; k < 300; k++) t += f(5).length + g(10);
    return [t, f(12), g(30)];
});

console.error('JITTEST-BEGIN');
for (var i = 0; i < out.length; i++) console.error(out[i]);
console.error('JITTEST-END');

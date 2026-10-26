// T2's speculation on arithmetic (JS_PERF_DESIGN.md section 6.7, step 5c):
// each function is warmed on int32s, then sent values its guards must
// reject (overflow, -0, doubles, NaN, strings, objects), so every exit and
// every recompilation with a widened site runs.  Output must match the
// interpreter's.
var out = [];
function T(name, f) {
    var r;
    try { r = f(); } catch (e) { r = 'threw ' + e.name + ': ' + e.message; }
    out.push(name + ': ' + (typeof r === 'number' && Object.is(r, -0) ? '-0' : JSON.stringify(r)));
}
var N = 2000;
function show(v) { return Object.is(v, -0) ? '-0' : typeof v === 'number' && v !== v ? 'NaN' : v; }

T('add-int-then-overflow-then-double-then-string', function () {
    function add(a, b) { return a + b; }
    var s = 0;
    for (var i = 0; i < N; i++) s = add(s, i) | 0;
    return [s, add(2147483647, 1), add(-2147483648, -1), add(0.5, 0.25), add(1, 2.5),
            add('a', 1), add(1, 'b'), add(null, 1), add(true, 1), add([1], 2), add({}, 1)];
});

T('sub-mul-neg-zero', function () {
    function sub(a, b) { return a - b; }
    function mul(a, b) { return a * b; }
    function neg(a) { return -a; }
    var s = 0;
    for (var i = 0; i < N; i++) s += sub(i, 3) + mul(i, 2) + neg(i);
    return [s, show(sub(0, 0)), show(mul(0, -5)), show(mul(-3, 0)), show(neg(0)), neg(-2147483648),
            mul(65536, 65536), mul(-65536, 65536), sub(-2147483648, 1), show(mul(1.5, 0)), neg('3')];
});

T('div', function () {
    function div(a, b) { return a / b; }
    var s = 0;
    for (var i = 1; i < N; i++) s += div(i, 4);
    return [s, div(1, 0), div(-1, 0), show(div(0, 0)), show(div(0, -3)), div(7, 2), div('9', 3)];
});

T('compare-int-double-nan-string', function () {
    function lt(a, b) { return a < b; }
    function le(a, b) { return a <= b; }
    function gt(a, b) { return a > b; }
    function ge(a, b) { return a >= b; }
    var c = 0, nan = 0 / 0;
    for (var i = 0; i < N; i++) c += lt(i, 1000) + le(i, 1000) + gt(i, 1000) + ge(i, 1000);
    return [c, lt(1.5, 2), le(2, 2.0), gt(nan, 1), lt(nan, 1), le(nan, nan), ge(1, nan),
            lt('a', 'b'), gt('10', 9), le(undefined, 1), lt(null, 1), ge(-0, 0)];
});

T('equality', function () {
    function eq(a, b) { return a == b; }
    function seq(a, b) { return a === b; }
    function ne(a, b) { return a != b; }
    function sne(a, b) { return a !== b; }
    var c = 0, nan = 0 / 0;
    for (var i = 0; i < N; i++) c += eq(i, 7) + seq(i, 7) + ne(i, 7) + sne(i, 7);
    return [c, seq(1, 1.0), seq(nan, nan), sne(nan, nan), eq(nan, nan), seq(0, -0), eq('1', 1),
            seq('1', 1), eq(null, undefined), seq(null, undefined), ne(2.5, 2.5), seq({}, {})];
});

T('bitwise-and-shifts', function () {
    function and(a, b) { return a & b; }
    function or(a, b) { return a | b; }
    function xor(a, b) { return a ^ b; }
    function shl(a, b) { return a << b; }
    function sar(a, b) { return a >> b; }
    function shr(a, b) { return a >>> b; }
    var s = 0;
    for (var i = 0; i < N; i++) s += and(i, 255) + or(i, 1) + xor(i, 3) + shl(i, 2) + sar(i, 1) + shr(i, 1);
    return [s, shl(1, 31), shl(1, 32), shl(1, 33), sar(-8, 33), shr(-1, 0), shr(-1, 1), shr(-8, 28),
            and(1.9, 3), or(2.5, 0), xor('5', 1), shl(2147483648, 0), shr(4294967295, 0)];
});

T('inc-dec-forms', function () {
    function inc(x) { x++; return x; }
    function dec(x) { x--; return x; }
    function pre(x) { return ++x; }
    function post(x) { var y = x++; return [y, x]; }
    function loop(n) { var c = 0; for (var i = 0; i < n; i++) c++; return c; }
    var s = 0;
    for (var i = 0; i < N; i++) s += inc(i) + dec(i) + pre(i) + post(i)[1] + loop(3);
    return [s, inc(2147483647), dec(-2147483648), pre(2147483647), post(2147483647), inc(1.5),
            dec('5'), post('7'), inc(null), post(undefined), loop(10)];
});

T('add-loc-int-double-string', function () {
    function sum(a) { var t = 0; for (var i = 0; i < a.length; i++) t += a[i]; return t; }
    var ints = [], s = 0;
    for (var i = 0; i < 100; i++) ints.push(i);
    for (var k = 0; k < 50; k++) s += sum(ints);
    return [s, sum([2147483647, 1]), sum([0.5, 0.25, 1]), sum(['a', 'b']), sum([1, 'x', 2]), sum([])];
});

T('loops-with-doubles', function () {
    function f(n) { var x = 0; for (var i = 0; i < n; i++) x = x + i * 0.5; return x; }
    function g(n) { var x = 1; for (var i = 0; i < n; i++) x = x * 1.0001; return Math.round(x * 1e6); }
    var s = 0;
    for (var k = 0; k < 200; k++) s += f(100) + g(10);
    return [s, f(1000), g(1000)];
});

T('branches-on-compares', function () {
    function cls(x) { if (x < 0) return 'neg'; else if (x === 0) return 'zero'; else if (!(x < 100)) return 'big'; return 'small'; }
    var r = {};
    for (var i = -50; i < N; i++) { var c = cls(i); r[c] = (r[c] || 0) + 1; }
    return [r, cls(-0), cls(0.5), cls(0 / 0), cls('5'), cls(1e10)];
});

T('bool-arith', function () {
    function f(a, b) { return (a < b) + (a > b); }
    function g(a) { return !(a < 3); }
    var s = 0;
    for (var i = 0; i < N; i++) s += f(i, 50) + g(i);
    return [s, f(1, 1), g(5), g(1)];
});

T('exits-keep-locals', function () {
    // a guard failing mid-expression must leave every local as it was
    function f(a, b, c) {
        var x = a + 1, y = x * 2, z = y - c;
        var w = b + z;      // b a string on the last call: exits here
        return [x, y, z, w];
    }
    var last;
    for (var i = 0; i < N; i++) last = f(i, i, 3);
    return [last, f(10, 'q', 3), f(2147483647, 1, 0), f(1.5, 2, 1)];
});

console.error('JITTEST-BEGIN');
for (var i = 0; i < out.length; i++) console.error(out[i]);
console.error('JITTEST-END');

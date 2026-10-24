// JIT micro-benchmarks: deterministic JS (no DOM) of the kinds YouTube's
// code runs, timed by tests/jit/bench/bench.sh.  BENCH selects one
// (all by default); the printed checksum must not depend on QJS_JIT.
var BENCH = typeof globalThis.__bench === 'string' ? globalThis.__bench : 'all';
var results = [], times = [];
var now = typeof performance === 'object' ? function () { return performance.now(); } : Date.now;

function run(name, f) {
    if (BENCH !== 'all' && BENCH !== name) return;
    var t0 = now();
    results.push(name + '=' + f());
    times.push(name + '=' + Math.round(now() - t0));
}

// Small functions called a lot: calls, arguments, returns.
run('calls', function () {
    function add(a, b) { return a + b; }
    function mix(x) { return add(x, 1) * 2 - add(x, -1); }
    var s = 0;
    for (var i = 0; i < 3000000; i++) s = (s + mix(i)) | 0;
    return s;
});

// Methods and property access on objects with a few shapes.
run('props', function () {
    function P(x, y) { this.x = x; this.y = y; }
    P.prototype.len2 = function () { return this.x * this.x + this.y * this.y; };
    P.prototype.add = function (o) { return new P(this.x + o.x, this.y + o.y); };
    var a = new P(1, 2), s = 0;
    for (var i = 0; i < 1000000; i++) {
        var b = new P(i & 7, 3);
        a = a.add(b);
        s = (s + b.len2() + a.x) | 0;
        if (a.x > 100000) a = new P(0, 0);
    }
    return s;
});

// Closures over outer variables (get_var_ref / put_var_ref).
run('closures', function () {
    var count = 0, total = 0;
    function makeAcc(k) { return function (v) { count++; total += v * k; return total; }; }
    var accs = [makeAcc(1), makeAcc(2), makeAcc(3)];
    for (var i = 0; i < 2000000; i++) accs[i % 3](i & 15);
    return count + ':' + (total | 0);
});

// Arrays: push, index reads, a callback.
run('arrays', function () {
    var a = [];
    for (var i = 0; i < 200000; i++) a.push(i * 7 % 1000);
    var s = 0;
    for (var r = 0; r < 10; r++)
        for (var i = 0; i < a.length; i++) s = (s + a[i]) | 0;
    s += a.filter(function (x) { return x & 1; }).length;
    return s;
});

// Branchy code over strings and objects (truthiness, ===, typeof).
run('branches', function () {
    var vals = [null, undefined, 0, 1, '', 'x', {}, [], true, false];
    var n = 0;
    for (var i = 0; i < 3000000; i++) {
        var v = vals[i % 10];
        if (v) n++;
        if (v === null || v === undefined) n += 2;
        if (typeof v === 'string' && v.length) n += 3;
        if (!v) n--;
    }
    return n;
});

// Strings: concatenation and character codes.
run('strings', function () {
    var h = 0;
    for (var i = 0; i < 200000; i++) {
        var s = 'k' + (i % 97) + '_' + (i & 255);
        for (var j = 0; j < s.length; j++) h = (h * 31 + s.charCodeAt(j)) | 0;
    }
    return h;
});

console.error('BENCH ' + results.join(' '));
console.error('BENCHTIME ' + times.join(' '));

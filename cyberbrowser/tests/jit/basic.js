// Differential test for the baseline JIT (JIT_DESIGN.md section 10): run
// with QJS_JIT=0 and with QJS_JIT=stress (or threaded) through
// CYBER_PROBE_JS; the printed log must be identical.  tests/jit/run.sh
// does both and compares.
var log = [];
function out(x) { log.push(String(x)); }
function t(name, f) {
    try { out(name + ': ' + f()); }
    catch (e) { out(name + ' threw: ' + (e && e.name) + ': ' + (e && e.message)); }
}

// Arithmetic, comparisons, conversions.
t('int', function () { var s = 0; for (var i = 0; i < 1000; i++) s += i * 3 - (i >> 1) + (i % 7); return s; });
t('overflow', function () { var x = 2147483647; return [x + 1, -x - 2, x * 2, -(-2147483648), 0 * -1, 1 / (0 * -1)].join(); });
t('float', function () { var s = 0.5; for (var i = 0; i < 100; i++) s = s * 1.01 + i / 3; return s.toFixed(6); });
t('mixed', function () { return [1 + '2', '3' * '4', true + 1, null + 1, undefined + 1, [] + {}, 7 / 2, -7 % 3, 2 ** 10, 5 & 3, 5 | 3, 5 ^ 3, ~5, -5 >> 1, -5 >>> 28, 1 << 31].join(); });
t('compare', function () { return [1 < 2, '10' < '9', 1 == '1', 1 === '1', null == undefined, null === undefined, NaN == NaN, 2 >= 2, 'a' > 'B', [1] == 1].join(); });
t('incdec', function () { var a = 1, b = a++, c = ++a, d = a--, e = --a; var o = { x: 1 }; o.x++; ++o.x; var arr = [5]; arr[0]--; return [a, b, c, d, e, o.x, arr[0]].join(); });
t('bigint', function () { var x = 2n ** 64n; return [x, x + 1n, -x, x / 3n, x % 7n, typeof x, 5n > 3].join(); });
t('unary', function () { var s = '5'; return [+s, -s, !s, !!'', typeof s, void 0, +true, -null, +'x'].join(); });
t('strings', function () { var s = ''; for (var i = 0; i < 50; i++) s += String.fromCharCode(65 + i % 26); return s + s.length + s.slice(3, 9) + s.indexOf('XYZ'); });

// Control flow.
t('switch', function () { var r = []; for (var i = 0; i < 6; i++) { switch (i) { case 0: r.push('a'); case 1: r.push('b'); break; case 'x': r.push('no'); break; default: r.push('d' + i); } } return r.join(''); });
t('labels', function () { var n = 0; outer: for (var i = 0; i < 10; i++) { for (var j = 0; j < 10; j++) { if (j == 5) continue outer; if (i == 7) break outer; n += j; } } return n; });
t('dowhile', function () { var i = 0, s = 0; do { s += i; } while (++i < 10); while (i > 0) { i -= 3; s -= i; } return s; });
t('logical', function () { var a = null, b = 0, c = 'x'; return [a || 'd', b || 'e', c && 'f', a ?? 'g', b ?? 'h', a?.y, ({ y: { z: 1 } })?.y?.z].join(); });
t('ternary', function () { var r = []; for (var i = -2; i < 3; i++) r.push(i < 0 ? 'neg' : i > 0 ? 'pos' : 'zero'); return r.join(); });

// Functions, closures, arguments.
t('closure', function () {
    function counter() { var c = 0; return { inc: function () { return ++c; }, get: () => c }; }
    var a = counter(), b = counter(); a.inc(); a.inc(); b.inc();
    return [a.get(), b.get()].join();
});
t('closure-loop', function () { var fs = []; for (let i = 0; i < 5; i++) fs.push(() => i * i); return fs.map(f => f()).join(); });
t('arguments', function () { function f(a, b) { arguments[0] = 9; return [a, b, arguments.length, Array.prototype.slice.call(arguments).join('|')].join(); } return f(1, 2, 3); });
t('strict-args', function () { 'use strict'; function f(a) { arguments[0] = 9; return a + arguments.length; } return f(1, 2); });
t('rest-default', function () { function f(a = 10, ...r) { return a + ':' + r.length + ':' + r.join(); } return f() + ' ' + f(1, 2, 3); });
t('recursion', function () { function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); } return fib(20); });
t('apply-call-bind', function () { function f(x, y) { return this.k + x + y; } var o = { k: 1 }; return [f.call(o, 2, 3), f.apply(o, [4, 5]), f.bind(o, 6)(7), Math.max.apply(null, [3, 9, 2])].join(); });
t('spread', function () { function f() { return arguments.length; } var a = [1, 2, 3]; return [f(...a, ...a), [...a, 4].length, Math.min(...a)].join(); });
t('tail', function () { function loop(n, acc) { if (n == 0) return acc; return loop(n - 1, acc + n); } return loop(500, 0); });
t('new-target', function () { function F() { return new.target === F; } return [new F() instanceof F, F()].join(); });
t('this-sloppy', function () { function f() { return typeof this; } return [f(), f.call(5), (function () { 'use strict'; return typeof this; })()].join(); });

// Objects, properties, prototypes.
t('objects', function () {
    var o = { a: 1, b: 'two', ['c' + 1]: 3, get d() { return this.a + 10; }, set e(v) { this.a = v; } };
    o.e = 5; delete o.b; o.f = [1, 2];
    return JSON.stringify(o) + o.d + ('b' in o) + Object.keys(o).join();
});
t('proto', function () {
    function A(x) { this.x = x; } A.prototype.get = function () { return this.x; };
    function B(x) { A.call(this, x * 2); } B.prototype = Object.create(A.prototype); B.prototype.constructor = B;
    var b = new B(4); return [b.get(), b instanceof A, Object.getPrototypeOf(b) === B.prototype, b.hasOwnProperty('get')].join();
});
t('classes', function () {
    class P { #s = 3; static k = 'K'; constructor(n) { this.n = n; } get twice() { return this.n * 2; } m() { return this.#s + this.n; } static sm() { return P.k; } #priv() { return 1; } usePriv() { return this.#priv() + (#s in this); } }
    class Q extends P { constructor() { super(7); } m() { return super.m() * 10; } }
    var q = new Q(); return [q.twice, q.m(), P.sm(), q.usePriv(), q instanceof P].join();
});
t('array-el', function () { var a = []; for (var i = 0; i < 20; i++) a[i] = i * i; a[30] = 1; var s = 0; for (var i = 0; i < a.length; i++) s += a[i] | 0; return s + ':' + a.length; });
t('computed', function () { var o = {}; var k = ['x', 'y', 1, Symbol.for('z')]; for (var i = 0; i < k.length; i++) o[k[i]] = i; return [o.x, o.y, o[1], o[Symbol.for('z')], Object.getOwnPropertySymbols(o).length].join(); });
t('freeze', function () { 'use strict'; var o = Object.freeze({ a: 1 }); o.a = 2; return o.a; });
t('getter-chain', function () { var base = { get v() { return 42; } }; var o = Object.create(Object.create(base)); return o.v; });

// Destructuring, iterators, generators.
t('destructure', function () { var { a, b: [c, , d = 4] = [], ...rest } = { a: 1, b: [2, 3], e: 5, f: 6 }; var [x, , ...ys] = 'hello'; return [a, c, d, JSON.stringify(rest), x, ys.join('')].join(); });
t('for-of', function () { var s = 0; for (var v of [1, 2, 3]) s += v; for (var [k, v] of new Map([['a', 1], ['b', 2]])) s += k.length + v; for (var ch of 'abc') s += ch.charCodeAt(0); return s; });
t('for-in', function () { var o = { a: 1, b: 2 }; var p = Object.create(o); p.c = 3; var r = []; for (var k in p) r.push(k); return r.join(); });
t('generator', function () {
    function* g(n) { var x = yield 1; for (var i = 0; i < n; i++) { if (i == 2) continue; yield i * x; } return 'end'; }
    var it = g(5), r = [it.next().value]; var s; while (!(s = it.next(10)).done) r.push(s.value); r.push(s.value);
    function* d() { yield* [7, 8]; yield* g(1); }
    return r.join() + '|' + [...d()].join();
});
t('iter-close', function () { var closed = 0; var it = { [Symbol.iterator]() { var i = 0; return { next() { return { value: i++, done: false }; }, return() { closed++; return {}; } }; } }; for (var v of it) if (v > 3) break; try { for (var v of it) throw 1; } catch (e) { } return closed; });

// Exceptions.
t('try', function () { var r = []; try { r.push(1); throw new Error('x'); } catch (e) { r.push(e.message); } finally { r.push('f'); } return r.join(); });
t('finally-return', function () { function f() { try { return 'try'; } finally { log.push('fin'); } } return f(); });
t('nested-throw', function () { function a() { b(); } function b() { null.x; } try { a(); } catch (e) { return e instanceof TypeError; } });
t('rethrow', function () { try { try { throw 1; } catch (e) { throw e + 1; } } catch (e) { return e; } });
t('throw-in-loop', function () { var n = 0; for (var i = 0; i < 100; i++) { try { if (i % 3 == 0) throw i; n++; } catch (e) { n += 100; } } return n; });
t('tdz', function () { try { x; let x = 1; } catch (e) { return e.name; } });
t('const', function () { const c = 1; try { c = 2; } catch (e) { return e.name; } });
t('stack-depth', function () { function r(n) { return n ? 1 + r(n - 1) : 0; } return r(1000); });

// Scopes: with, eval, globals.
t('with', function () { var o = { p: 1 }; var r; with (o) { p = 2; r = p + typeof q; } return r + o.p; });
t('eval', function () { var a = 5; var r = eval('a * 2'); var g = (0, eval)('typeof a'); return r + g; });
t('global', function () { globalThis.__jit_g = 1; __jit_g++; return __jit_g + typeof __jit_undefined; });
t('typeof-undeclared', function () { return typeof someUndeclaredThing; });

// Builtins with callbacks (native -> JS calls).
t('array-methods', function () { var a = [5, 1, 4, 2, 3]; return [a.map(x => x * 2).join(), a.filter(x => x & 1).join(), a.reduce((s, x) => s + x, 0), a.slice().sort((x, y) => x - y).join(), a.some(x => x > 4), a.findIndex(x => x == 4)].join(';'); });
t('regexp', function () { var m = /(\d+)-(\w+)/g.exec('x 12-ab 34-cd'); return [m[1], m[2], 'a1b22c333'.replace(/\d+/g, d => d.length), /^\s*$/.test('  ')].join(); });
t('json', function () { return JSON.stringify(JSON.parse('{"a":[1,{"b":null}],"c":"\\u0041"}'), null, 0); });
t('template', function () { function tag(s, ...v) { return s.raw.join('|') + v.join(','); } var x = 3; return `a${x}b${x * 2}` + tag`p${1}q${2}r`; });
t('symbols', function () { var s = Symbol('d'); var o = { [s]: 1, [Symbol.toPrimitive]() { return 42; } }; return [o[s], +o, `${o}`, s.toString()].join(); });
t('proxy', function () { var p = new Proxy({}, { get(t, k) { return k + '!'; }, has() { return true; } }); return [p.foo, 'x' in p].join(); });
t('date-math', function () { return [Math.floor(7.9), Math.round(-2.5), Math.abs(-3), Math.sqrt(16), new Date(0).toISOString()].join(); });

// Async (settled after the promise jobs run).
var asyncLog = [];
(async function () {
    async function f(x) { await null; return x * 2; }
    var r = await f(3);
    try { await Promise.reject(new Error('rej')); } catch (e) { r += e.message; }
    for await (var v of (async function* () { yield 1; yield 2; })()) r += v;
    asyncLog.push(r);
})();
Promise.resolve().then(function () { asyncLog.push('then'); });

// A hot loop, long enough to be compiled at the normal threshold.
t('hot', function () {
    function step(o, i) { o.a += i & 3; o.b = o.a ^ i; return o.b > 100 ? o.b - 100 : o.b; }
    var o = { a: 0, b: 0 }, s = 0;
    for (var i = 0; i < 20000; i++) s = (s + step(o, i)) | 0;
    return s + ':' + o.a;
});

setTimeout(function () {
    out('async: ' + asyncLog.join());
    console.error('JITTEST-BEGIN\n' + log.join('\n') + '\nJITTEST-END');
}, 0);

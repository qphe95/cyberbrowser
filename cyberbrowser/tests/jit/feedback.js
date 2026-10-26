// The interpreter's feedback-vector caches (JS_PERF_DESIGN.md section 5.4):
// each case warms a site past the feedback threshold, then changes what the
// cached ways assumed.  Output must match QJS_FEEDBACK=0 and every JIT mode.
var out = [];
function T(name, f) {
    var r;
    try { r = f(); } catch (e) { r = 'threw ' + e.name; }
    out.push(name + ': ' + JSON.stringify(r));
}
var N = 3000;

T('get-own-poly-mega', function () {
    function get(o) { return o.x; }
    var shapes = [], s = 0;
    for (var k = 0; k < 8; k++) { var o = {}; o['p' + k] = k; o.x = k; shapes.push(o); }
    for (var i = 0; i < N; i++) s += get(shapes[i % (1 + (i >> 9))]);
    return s;
});

T('get-proto-then-shadow', function () {
    function A() {}
    A.prototype.m = 1;
    function get(o) { return o.m; }
    var a = new A(), s = 0;
    for (var i = 0; i < N; i++) s += get(a);
    A.prototype.m = 2;              // the prototype's property changes
    s += get(a);
    a.m = 10;                       // shadowed: the receiver's shape changes
    s += get(a);
    delete A.prototype.m;
    s += get(new A()) === undefined ? 100 : 0;
    return s;
});

T('get-becomes-getter', function () {
    function P() {}
    function get(o) { return o.v; }
    var p = new P(), s = 0;
    P.prototype.v = 1;
    for (var i = 0; i < N; i++) s += get(p);
    Object.defineProperty(P.prototype, 'v', { get: function () { return 1000; }, configurable: true });
    s += get(p);
    return s;
});

T('getters', function () {
    class P { constructor(v) { this._v = v; } get v() { return this._v * 2; } }
    function get(o) { return o.v; }
    var a = new P(1), s = 0, r = [];
    for (var i = 0; i < N; i++) s += get(a);
    r.push(s);
    Object.defineProperty(P.prototype, 'v', { get: function () { return -1; }, configurable: true });
    r.push(get(a));                 // the getter redefined
    Object.defineProperty(P.prototype, 'v', { value: 7, configurable: true, writable: true });
    r.push(get(a));                 // now a data property
    var own = { get w() { return this.k; }, k: 5 }, t = 0;
    function getw(o) { return o.w; }
    for (var j = 0; j < N; j++) t += getw(own);
    Object.defineProperty(own, 'w', { get: function () { return 100; }, configurable: true });
    r.push(t, getw(own));
    var thrower = { get x() { throw new RangeError('g'); } };
    function getx(o) { return o.x; }
    for (var k = 0; k < N; k++) try { getx(thrower); } catch (e) {}
    try { getx(thrower); r.push('no throw'); } catch (e) { r.push(e.name); }
    return r;
});

T('get-absent', function () {
    function get(o) { return o.nope; }
    var o = { a: 1 }, n = 0;
    for (var i = 0; i < N; i++) if (get(o) === undefined) n++;
    Object.prototype.nope = 'late';
    var r = get(o);
    delete Object.prototype.nope;
    return [n, r];
});

T('put-own-and-add', function () {
    function C() {}
    function set(o, v) { o.y = v; }
    var s = 0;
    for (var i = 0; i < N; i++) { var o = new C(); set(o, i); set(o, i + 1); s += o.y; }
    return s;
});

T('put-add-then-setter-on-proto', function () {
    function C() {}
    function set(o, v) { o.z = v; }
    for (var i = 0; i < N; i++) set(new C(), i);
    var seen = 0;
    Object.defineProperty(C.prototype, 'z', { set: function (v) { seen = v; }, configurable: true });
    var o = new C();
    set(o, 42);
    return [seen, Object.keys(o).length];
});

T('put-add-then-readonly-proto', function () {
    'use strict';
    function C() {}
    function set(o, v) { o.w = v; }
    for (var i = 0; i < N; i++) set(new C(), i);
    Object.defineProperty(C.prototype, 'w', { value: 1, writable: false, configurable: true });
    try { set(new C(), 2); return 'no throw'; } catch (e) { return e.name; }
});

T('put-non-extensible-and-frozen', function () {
    'use strict';
    function C() { this.a = 0; }
    function setb(o, v) { o.b = v; }
    function seta(o, v) { o.a = v; }
    for (var i = 0; i < N; i++) { var o = new C(); setb(o, i); seta(o, i); }
    var r = [];
    var ne = Object.preventExtensions(new C());
    try { setb(ne, 1); r.push('added'); } catch (e) { r.push(e.name); }
    var fr = Object.freeze(new C());
    try { seta(fr, 1); r.push('wrote'); } catch (e) { r.push(e.name); }
    return r;
});

T('put-array-length', function () {
    function set(o, v) { o.length = v; }
    var a = [1, 2, 3, 4, 5], s = 0;
    for (var i = 0; i < N; i++) { var b = [1, 2, 3]; set(b, 1); s += b.length; }
    set(a, 2);
    return [s, a.length, a[3]];
});

T('private-fields', function () {
    class A {
        #x = 1;
        getX() { return this.#x; }
        setX(v) { this.#x = v; }
        static has(o) { try { o.getX(); return true; } catch (e) { return e.name; } }
    }
    var a = new A(), s = 0;
    for (var i = 0; i < N; i++) { a.setX(i); s += a.getX(); }
    var fake = Object.create(A.prototype);
    return [s, A.has(fake)];
});

T('private-same-site-two-classes', function () {
    function mk(k) {
        return class { #v = k; get() { return this.#v; } static read(o) { return o.#v; } };
    }
    var C1 = mk(1), C2 = mk(2), s = 0;
    var o1 = new C1(), o2 = new C2();
    for (var i = 0; i < N; i++) s += C1.read(o1) + C2.read(o2);
    var r;
    try { C1.read(o2); r = 'read'; } catch (e) { r = e.name; }
    return [s, r];
});

T('gc-churn-shapes', function () {
    function get(o) { return o.k; }
    var s = 0;
    for (var round = 0; round < 40; round++) {
        var objs = [];
        for (var i = 0; i < 200; i++) { var o = {}; o['f' + round + '_' + (i % 3)] = 1; o.k = i; objs.push(o); }
        for (var j = 0; j < objs.length; j++) s += get(objs[j]);
        var junk = [];
        for (var g = 0; g < 2000; g++) junk.push({ a: g, b: [g] });
    }
    return s;
});

console.error('JITTEST-BEGIN');
for (var i = 0; i < out.length; i++) console.error(out[i]);
console.error('JITTEST-END');

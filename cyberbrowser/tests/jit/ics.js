// Differential test for the JIT's inline caches (see basic.js): get_field
// sites that see a shape, cache it, and then see the objects or their
// prototypes change in every way that must invalidate (or must not affect)
// a cached way.
var log = [];
function out(x) { log.push(String(x)); }
function t(name, f) {
    try { out(name + ': ' + f()); }
    catch (e) { out(name + ' threw: ' + (e && e.name) + ': ' + (e && e.message)); }
}

// One site per property name, called on many objects.
function getA(o) { return o.a; }
function getM(o) { return o.m; }
function getX(o) { return o.x; }
function callM(o) { return o.m(); }

t('own-then-change', function () {
    var r = [], o = { a: 1, b: 2 }, p = { a: 3, b: 4 };
    for (var i = 0; i < 5; i++) r.push(getA(o), getA(p));
    o.a = 10;                          // value change: same shape
    r.push(getA(o));
    delete o.a;                        // own property gone
    r.push(getA(o));
    o.a = 20;                          // back, at another slot
    r.push(getA(o), getA(p));
    Object.defineProperty(p, 'a', { get: function () { return 'getter'; } });
    r.push(getA(p), getA({ a: 5, b: 6 }));
    return r.join();
});

t('proto-changes', function () {
    function C() { this.own = 1; }
    C.prototype.m = 'proto-m';
    var a = new C(), b = new C(), r = [];
    for (var i = 0; i < 5; i++) r.push(getM(a));
    C.prototype.m = 'replaced';        // value change on the holder
    r.push(getM(a));
    Object.getPrototypeOf(C.prototype).m = 'deeper';   // Object.prototype: shadowed by C.prototype
    r.push(getM(b));
    delete C.prototype.m;              // now the deeper one shows
    r.push(getM(a));
    delete Object.prototype.m;
    r.push(getM(a));                   // absent
    C.prototype.m = 'again';
    r.push(getM(b));
    a.m = 'own';                       // own shadows the prototype
    r.push(getM(a), getM(b));
    Object.defineProperty(C.prototype, 'm', { get: function () { return 'proto-getter'; }, configurable: true });
    r.push(getM(b));
    return r.join();
});

t('absent-then-present', function () {
    var base = {}, o = Object.create(base), r = [];
    for (var i = 0; i < 5; i++) r.push(getX(o));
    base.x = 'on-base';
    r.push(getX(o));
    Object.prototype.x = 'on-object-prototype';
    r.push(getX({}), getX(o));
    delete base.x;
    r.push(getX(o));
    delete Object.prototype.x;
    r.push(getX(o), getX({}));
    return r.join();
});

t('set-prototype', function () {
    var p1 = { m: 'p1' }, p2 = { m: 'p2' }, o = Object.create(p1), r = [];
    for (var i = 0; i < 5; i++) r.push(getM(o));
    Object.setPrototypeOf(o, p2);
    r.push(getM(o));
    var twin = Object.create(p1);      // same shape as o had
    r.push(getM(twin));
    Object.setPrototypeOf(p2, { m: 'under' });
    delete p2.m;
    r.push(getM(o));
    return r.join();
});

t('polymorphic', function () {
    var objs = [], r = 0;
    for (var k = 0; k < 9; k++) {      // more shapes than ways
        var o = {};
        for (var j = 0; j < k; j++) o['f' + j] = j;
        o.a = k;
        objs.push(o);
    }
    for (var i = 0; i < 200; i++) r += getA(objs[i % 9]) * (i + 1);
    objs[3].a = 'changed';
    return r + ' ' + getA(objs[3]) + ' ' + getA(objs[8]);
});

t('methods', function () {
    function P(v) { this.v = v; }
    P.prototype.m = function () { return 'P' + this.v; };
    function Q(v) { this.v = v; }
    Q.prototype = Object.create(P.prototype);
    var r = [], ps = [new P(1), new Q(2)];
    for (var i = 0; i < 6; i++) r.push(callM(ps[i & 1]));
    Q.prototype.m = function () { return 'Q' + this.v; };
    r.push(callM(ps[0]), callM(ps[1]));
    P.prototype.m = function () { return 'P2' + this.v; };
    r.push(callM(ps[0]), callM(ps[1]));
    return r.join();
});

t('exotic-share', function () {
    // A String object and a plain object with the same prototype may share
    // a shape; the String object answers length and indexes itself.
    var plain = Object.create(String.prototype), str = new String('abc'), r = [];
    function get0(o) { return o[0]; }
    function getLen(o) { return o.length; }
    function getNaN(o) { return o.NaN; }
    for (var i = 0; i < 5; i++) r.push(getLen(plain), getLen(str), get0(str), get0(plain));
    var ta = new Uint8Array(2), plainTa = Object.create(Uint8Array.prototype);
    Object.prototype.NaN = 'proto-NaN';
    for (var i = 0; i < 5; i++) r.push(getNaN(plainTa), getNaN(ta), getNaN({}));
    delete Object.prototype.NaN;
    var arr = [1, 2], obj = Object.create(Array.prototype);
    for (var i = 0; i < 5; i++) r.push(getLen(arr), getLen(obj));
    return r.join();
});

t('dictionary', function () {
    var d = {}, r = [];
    for (var i = 0; i < 60; i++) d['k' + i] = i;   // past the private-shape threshold
    d.a = 'dict-a';
    for (var i = 0; i < 5; i++) r.push(getA(d));
    delete d.k3;
    d.a = 'dict-a2';
    r.push(getA(d));
    delete d.a;
    r.push(getA(d));
    d.a = 'dict-a3';
    r.push(getA(d));
    // Deletes enough to compact the slots, with ways cached on the shape.
    var e = {};
    for (var i = 0; i < 80; i++) e['k' + i] = i;
    e.a = 'e-a';
    e.x = 'e-x';
    for (var i = 0; i < 5; i++) r.push(getA(e), getX(e));
    for (var i = 0; i < 70; i++) delete e['k' + i];
    r.push(getA(e), getX(e));
    for (var i = 0; i < 40; i++) e['n' + i] = i;   // appends keep the shape
    r.push(getA(e), getX(e));
    Object.defineProperty(e, 'x', { get: function () { return 'e-x-getter'; } });
    r.push(getA(e), getX(e));
    Object.setPrototypeOf(e, { a: 'proto-a' });
    delete e.a;
    r.push(getA(e), getX(e));
    return r.join();
});

t('churn', function () {
    // Many short-lived shapes and objects, so collections run and freed
    // shapes' handles get reused while sites hold ways.
    var s = 0;
    for (var round = 0; round < 40; round++) {
        var objs = [];
        for (var i = 0; i < 400; i++) {
            var o = {};
            o['r' + round + '_' + (i % 7)] = i;
            o.a = i;
            objs.push(o);
        }
        for (var i = 0; i < objs.length; i++) s = (s + getA(objs[i])) | 0;
        var big = [];
        for (var i = 0; i < 2000; i++) big.push({ x: i, y: [i] });
        s = (s + getX(big[round])) | 0;
    }
    return s;
});

t('getters-and-frozen', function () {
    var o = { get a() { return 'g'; } }, f = Object.freeze({ a: 'frozen' }), r = [];
    for (var i = 0; i < 5; i++) r.push(getA(o), getA(f));
    var s = Object.seal({ a: 'sealed' });
    s.a = 'sealed2';
    r.push(getA(s));
    return r.join();
});

// put_field sites: an own way and an add way per shape, then each thing
// that must stop an add way (a setter or read-only property appearing on
// the prototype, the receiver made non-extensible or frozen) or an own one.
function setQ(o, v) { o.q = v; return o; }
function setQStrict(o, v) { 'use strict'; o.q = v; return o; }
function desc(o) { return Object.keys(o).join('') + '=' + o.q; }
t('put-add-ways', function () {
    var r = [], proto = { base: 1 }, seen = [];
    function make() { var o = Object.create(proto); o.k = 1; return o; }
    for (var i = 0; i < 6; i++) r.push(desc(setQ(make(), i)));          // add way
    for (var i = 0; i < 3; i++) { var o = setQ(make(), 'a'); r.push(desc(setQ(o, 'b'))); }  // own way
    Object.defineProperty(proto, 'q', { set: function (v) { seen.push(v); }, get: function () { return 'got'; }, configurable: true });
    for (var i = 0; i < 3; i++) r.push(desc(setQ(make(), 's' + i)));    // the setter now
    delete proto.q;
    for (var i = 0; i < 3; i++) r.push(desc(setQ(make(), 'd' + i)));    // added again
    Object.defineProperty(proto, 'q', { value: 'ro', writable: false, configurable: true });
    for (var i = 0; i < 2; i++) r.push(desc(setQ(make(), 'w' + i)));
    try { setQStrict(make(), 'strict'); r.push('no throw'); } catch (e) { r.push(e.name); }
    delete proto.q;
    var ne = Object.preventExtensions(make()), fr = Object.freeze(setQ(make(), 'f'));
    try { setQStrict(ne, 1); r.push('no throw'); } catch (e) { r.push(e.name); }
    try { setQStrict(fr, 2); r.push('no throw'); } catch (e) { r.push(e.name); }
    setQ(ne, 3); setQ(fr, 4);
    r.push(desc(ne), desc(fr), desc(setQ(make(), 'end')));
    return r.join(' ') + ' / ' + seen.join();
});
t('put-polymorphic', function () {
    // More shapes than ways, adds and writes mixed, receivers that are
    // prototypes of others.
    var r = [], objs = [];
    for (var i = 0; i < 10; i++) { var o = {}; o['p' + i] = i; objs.push(o); }
    for (var round = 0; round < 3; round++)
        for (var i = 0; i < objs.length; i++) setQ(objs[i], round * 10 + i);
    var parent = {}, child = Object.create(parent);
    for (var i = 0; i < 3; i++) { setQ({}, 0); }
    setQ(parent, 'P');
    r.push(objs.map(desc).join(','), child.q, desc(setQ(child, 'C')), parent.q);
    return r.join(' ');
});

console.error('JITTEST-BEGIN');
log.forEach(function (l) { console.error(l); });
console.error('JITTEST-END');

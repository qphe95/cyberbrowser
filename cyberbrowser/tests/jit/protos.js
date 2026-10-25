// Differential test for lazy function prototypes (JS_PERF_DESIGN.md
// section 5.6; see basic.js): a function's `prototype` is made on first use,
// and every way of using it must see what an eager one would.
var log = [];
function out(x) { log.push(String(x)); }
function t(name, f) {
    try { out(name + ': ' + f()); }
    catch (e) { out(name + ' threw: ' + (e && e.name) + ': ' + (e && e.message)); }
}
function make() { return function F(a, b) { this.a = a; }; }
function desc(o, k) {
    var d = Object.getOwnPropertyDescriptor(o, k);
    return d ? [typeof d.value, d.writable, d.enumerable, d.configurable].join('/') : 'none';
}

t('shape', function () {
    var F = make();
    return [Object.getOwnPropertyNames(F).join(','), F.hasOwnProperty('prototype'), 'prototype' in F,
            desc(F, 'prototype'), Object.keys(F).length].join(' ');
});
t('identity', function () {
    var F = make(), p = F.prototype, o = new F(1);
    return [p === F.prototype, Object.getPrototypeOf(o) === p, p.constructor === F, o.constructor === F,
            Object.getOwnPropertyNames(p).join(','), desc(p, 'constructor')].join(' ');
});
t('new-first', function () {
    // new and instanceof before anything reads prototype
    var F = make(), o = new F(2), G = make();
    return [o instanceof F, o instanceof G, Object.getPrototypeOf(o) === F.prototype, o.a].join(' ');
});
t('assign-first', function () {
    var F = make(), p = { m: function () { return 'm' + this.a; } };
    F.prototype = p;
    var o = new F(3);
    return [F.prototype === p, o.m(), desc(F, 'prototype'), o.constructor === Object].join(' ');
});
t('assign-strict-and-loops', function () {
    'use strict';
    var r = [];
    for (var i = 0; i < 20; i++) {
        var F = make();
        if (i % 3 == 0) F.prototype.k = i;
        else if (i % 3 == 1) F.prototype = { k: -i };
        r.push(new F(0).k === undefined ? 'u' : new F(0).k);
    }
    return r.join(',');
});
t('define', function () {
    var F = make(), G = make(), H = make();
    Object.defineProperty(F, 'prototype', { value: 42 });
    Object.defineProperty(G, 'prototype', { writable: false });
    var gp = G.prototype;
    try { Object.defineProperty(H, 'prototype', { enumerable: true }); } catch (e) { var he = e.name; }
    return [F.prototype, desc(G, 'prototype'), gp.constructor === G, he, desc(H, 'prototype')].join(' ');
});
t('freeze-seal-delete', function () {
    var F = make(), G = make(), H = make();
    Object.freeze(F); Object.seal(G);
    var del = delete H.prototype;
    try { 'use strict'; (function () { 'use strict'; F.prototype = 1; })(); } catch (e) { var fe = e.name; }
    return [desc(F, 'prototype'), Object.isFrozen(F), desc(G, 'prototype'), del, typeof H.prototype, fe,
            F.prototype.constructor === F].join(' ');
});
t('via-chain', function () {
    // A function on the prototype chain of another object.
    var F = make(), o = Object.create(F), G = make(), q = Object.create(G);
    var fromChain = o.prototype;
    q.prototype = 'own';
    return [fromChain === F.prototype, fromChain.constructor === F, q.prototype, typeof G.prototype,
            G.hasOwnProperty('prototype'), q.hasOwnProperty('prototype')].join(' ');
});
t('kinds', function () {
    var arrow = () => 1, method = ({ m() {} }).m, gen = function* () {}, agen = async function* () {},
        afn = async function () {}, bound = make().bind(null);
    return [arrow, method, gen, agen, afn, bound].map(function (f) {
        return f.hasOwnProperty('prototype') + ':' + desc(f, 'prototype');
    }).join(' ');
});
t('reflect-and-json', function () {
    var F = make(), G = make();
    var r = [Reflect.has(F, 'prototype'), Reflect.ownKeys(F).join(','), typeof Reflect.get(G, 'prototype'),
             JSON.stringify(Object.getOwnPropertyDescriptors(make()).prototype.writable)];
    var s = Reflect.set(F, 'prototype', 'x');
    r.push(s, F.prototype);
    return r.join(' ');
});

console.error('JITTEST-BEGIN');
log.forEach(function (l) { console.error(l); });
console.error('JITTEST-END');

// Differential test for the JIT's property fast paths (see basic.js):
// adding properties with put_field past every case JS_SetPropertyInternal
// treats specially, and gets through prototypes and primitives.
var log = [];
function out(x) { log.push(String(x)); }
function t(name, f) {
    try { out(name + ': ' + f()); }
    catch (e) { out(name + ' threw: ' + (e && e.name) + ': ' + (e && e.message)); }
}
function keys(o) { return Object.getOwnPropertyNames(o).join('|'); }

// The same put_field site over objects of every kind, sloppy and strict.
function setX(o, v) { o.x = v; return o; }
function setXStrict(o, v) { 'use strict'; o.x = v; return o; }
function kinds() {
    var setterLog = [];
    var withSetter = Object.create({ set x(v) { setterLog.push(v); } });
    var readOnly = Object.create(Object.defineProperty({}, 'x', { value: 'ro', writable: false }));
    var getterOnly = Object.create({ get x() { return 'g'; } });
    var protoData = Object.create({ x: 'proto' });
    var deep = Object.create(Object.create(Object.create({ set x(v) { setterLog.push('deep' + v); } })));
    var frozen = Object.freeze({ y: 1 });
    var sealed = Object.seal({ x: 1 });
    var nonExt = Object.preventExtensions({ y: 1 });
    var arr = [1, 2];
    var proxied = Object.create(new Proxy({}, { set: function (t, k, v, r) { setterLog.push('proxy' + v); return true; } }));
    var arrProto = Object.create([]);
    var fn = function () {};
    var nullProto = Object.create(null);
    return { list: [{}, withSetter, readOnly, getterOnly, protoData, deep, frozen, sealed, nonExt, arr, proxied, arrProto, fn,
                    nullProto, new Date(0), /re/, new Map()],
             setterLog: setterLog };
}
t('put-sloppy', function () {
    var k = kinds(), r = [];
    for (var i = 0; i < k.list.length; i++) {
        try { var o = setX(k.list[i], i); r.push(keys(o) + '=' + o.x); } catch (e) { r.push(e.name); }
    }
    return r.join(' ') + ' / ' + k.setterLog.join();
});
t('put-strict', function () {
    var k = kinds(), r = [];
    for (var i = 0; i < k.list.length; i++) {
        try { var o = setXStrict(k.list[i], i); r.push(keys(o) + '=' + o.x); } catch (e) { r.push(e.name); }
    }
    return r.join(' ') + ' / ' + k.setterLog.join();
});
t('put-primitive', function () {
    var r = [];
    [1, 'a', true, null, undefined, 2n].forEach(function (p) { try { setXStrict(p, 1); r.push('ok'); } catch (e) { r.push(e.name); } });
    [1, 'a', true].forEach(function (p) { try { setX(p, 1); r.push('ok'); } catch (e) { r.push(e.name); } });
    return r.join();
});

// Many objects through one site: shape transitions, order of keys.
t('constructors', function () {
    function P(x, y) { this.x = x; this.y = y; if (x & 1) this.odd = true; }
    var s = 0, last;
    for (var i = 0; i < 2000; i++) { last = new P(i, i * 2); s += last.x + last.y + (last.odd ? 1 : 0); }
    var q = new P(3, 4); delete q.y; q.y = 5; q.z = 6;
    return s + ' ' + keys(last) + ' ' + keys(q) + ' ' + JSON.stringify(q);
});
t('proto-change', function () {
    var o = {}, r = [];
    for (var i = 0; i < 4; i++) {
        if (i == 2) Object.setPrototypeOf(o, { set w(v) { r.push('set' + v); } });
        o['k' + i] = i;
        setX(o, i);
        o.w = i;
    }
    return r.join() + ' ' + keys(o) + ' ' + o.x;
});
t('global', function () {
    var g = typeof globalThis === 'object' ? globalThis : this;
    setX(g, 'gx');
    var r = [typeof g.x, g.x];
    delete g.x;
    return r.join();
});
t('get-proto', function () {
    function A() { this.own = 1; }
    A.prototype.inherited = 2;
    Object.defineProperty(A.prototype, 'acc', { get: function () { return this.own + 10; } });
    var a = new A(), s = 0;
    for (var i = 0; i < 100; i++) s += a.own + a.inherited + a.acc + (a.missing === undefined ? 1 : 0);
    return s;
});

console.error('JITTEST-BEGIN');
log.forEach(function (l) { console.error(l); });
console.error('JITTEST-END');

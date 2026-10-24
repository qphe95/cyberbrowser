// Differential test for the JIT's number and string fast paths (see
// basic.js): every operator over a table of operands, so the edges
// (overflow, -0, NaN, ToInt32 of doubles, unsigned shifts, string lengths
// and methods) run through the compiled fast paths and their slow paths.
var log = [];
function out(x) { log.push(String(x)); }
function t(name, f) {
    try { out(name + ': ' + f()); }
    catch (e) { out(name + ' threw: ' + (e && e.name) + ': ' + (e && e.message)); }
}
// -0 prints as 0: show it.
function show(v) { return v === 0 && 1 / v < 0 ? '-0' : typeof v === 'number' ? String(v) : typeof v + ':' + String(v); }

var vals = [0, -0, 1, -1, 7, -7, 3.5, -3.5, 0.5, 2147483647, -2147483648, 2147483648, -2147483649, 4294967295,
            4294967296, 1e10, -1e10, 1e300, NaN, Infinity, -Infinity, true, false, null, undefined, '3', 'x', '', 5n];

function table(name, f) {
    t(name, function () {
        var rows = [];
        for (var i = 0; i < vals.length; i++) {
            var r = [];
            for (var j = 0; j < vals.length; j++) {
                var v;
                try { v = show(f(vals[i], vals[j])); } catch (e) { v = 'E'; }
                r.push(v);
            }
            rows.push(r.join(' '));
        }
        return '\n' + rows.join('\n');
    });
}
table('add', function (a, b) { return a + b; });
table('sub', function (a, b) { return a - b; });
table('mul', function (a, b) { return a * b; });
table('mod', function (a, b) { return a % b; });
table('and', function (a, b) { return a & b; });
table('or', function (a, b) { return a | b; });
table('xor', function (a, b) { return a ^ b; });
table('shl', function (a, b) { return a << b; });
table('sar', function (a, b) { return a >> b; });
table('shr', function (a, b) { return a >>> b; });
table('lt', function (a, b) { return a < b; });
table('lte', function (a, b) { return a <= b; });
table('gt', function (a, b) { return a > b; });
table('gte', function (a, b) { return a >= b; });
table('eq', function (a, b) { return a == b; });
table('strict_eq', function (a, b) { return a === b; });
t('neg', function () { return vals.map(function (v) { try { return show(-v); } catch (e) { return 'E'; } }).join(' '); });

// Overflow inside loops (the compiled int paths, then their float results).
t('overflow-loop', function () {
    var a = 2147483600, b = -2147483600, m = 46000, r = [];
    for (var i = 0; i < 100; i++) { a = a + 1; b = b - 1; m = m * 1 + 1; }
    r.push(a, b, m, m * m, a * 2, -a - a);
    var h = 0;
    for (var i = 0; i < 1000; i++) h = (h * 31 + i) | 0;
    r.push(h);
    var z = 0;
    for (var i = -3; i < 3; i++) r.push(show(i * z), show(z * i), show(-i));
    return r.join();
});
t('shr-loop', function () { var r = []; for (var i = -4; i < 4; i++) r.push(i >>> 0, i >>> 1, (i * 1e9) >>> 3); return r.join(); });
t('float-bits', function () {
    var r = [], x = 0.75;
    for (var i = 0; i < 40; i++) { r.push((x | 0), (x & 255), (x ^ 3), (x << 1), (x >> 1)); x = x * -2.5; }
    return r.join();
});

// Strings: length, and String.prototype methods on primitives.
t('string-length', function () {
    var s = 'hello', n = 0, o = new String('wrapped'), a = [1, 2, 3], f = function (x, y) {};
    for (var i = 0; i < 10; i++) n += s.length + ''.length + o.length + a.length + f.length + ('x' + i).length;
    var plain = { length: 42 }, getter = { get length() { return 7; } };
    return [n, plain.length, getter.length, 'abc'.length].join();
});
t('string-methods', function () {
    var s = 'k12_34', h = 0;
    for (var j = 0; j < s.length; j++) h = (h * 31 + s.charCodeAt(j)) | 0;
    String.prototype.extra = function () { return 'extra:' + this.length; };
    var r = [h, s.toUpperCase(), s.indexOf('_'), s.slice(1, 3), s.extra(), s.constructor === String];
    delete String.prototype.extra;
    try { s.extra(); } catch (e) { r.push(e.name); }
    Object.defineProperty(String.prototype, 'acc', { get: function () { return 'acc' + this; }, configurable: true });
    r.push(s.acc);
    delete String.prototype.acc;
    r.push(s[0], s[5], s[6], s['1']);
    return r.join();
});
t('primitive-fields', function () {
    var r = [], vs = [1, 1.5, true, null, undefined, 5n, Symbol.iterator];
    for (var i = 0; i < vs.length; i++) { try { r.push(String(vs[i].toString)); } catch (e) { r.push(e.name); } }
    return r.map(function (x) { return x.length; }).join();
});

// One line per call: the console cuts long messages.
console.error('JITTEST-BEGIN');
log.join('\n').split('\n').forEach(function (l) { console.error(l); });
console.error('JITTEST-END');

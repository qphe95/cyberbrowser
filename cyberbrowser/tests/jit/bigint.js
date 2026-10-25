// Differential test for BigInts across the short/heap boundary (see
// basic.js): short big ints are 64-bit with 16-byte values and 32-bit with
// 8-byte ones (JS_PERF_DESIGN.md section 5.1), so every operation is tried
// on values on both sides of 2^31 and 2^63.  Each line is checked against
// the known answer, so a wrong result in every build still fails.
var log = [];
function out(x) { log.push(String(x)); }
function t(name, f) {
    try { out(name + ': ' + f()); }
    catch (e) { out(name + ' threw: ' + (e && e.name) + ': ' + (e && e.message)); }
}
function eq(name, got, want) { out(name + ': ' + (got === want ? 'ok' : 'WRONG ' + got + ' (want ' + want + ')')); }

eq('2^31', String(2n ** 31n), '2147483648');
eq('2^32', String(2n ** 32n), '4294967296');
eq('0+2^31', String(0n + 2n ** 31n), '2147483648');
eq('2^31-1 < 2^31', String(2n ** 31n - 1n < 2n ** 31n), 'true');
eq('-(2^31)-1', String(-(2n ** 31n) - 1n), '-2147483649');
eq('asUintN(33,-1)', String(BigInt.asUintN(33, -1n)), '8589934591');
eq('asIntN(32,2^31)', String(BigInt.asIntN(32, 2n ** 31n)), '-2147483648');
eq('asUintN(64,-1)', String(BigInt.asUintN(64, -1n)), '18446744073709551615');
eq('-(2^70)>>3', String((-(2n ** 70n)) >> 3n), '-147573952589676412928');
eq('(-(2^70)-5)>>100', String((-(2n ** 70n) - 5n) >> 100n), '-1');
eq('(-(2^31)-1)>>3', String((-(2n ** 31n) - 1n) >> 3n), '-268435457');
eq('(-(2^31)-1)>>40', String((-(2n ** 31n) - 1n) >> 40n), '-1');
eq('2^63 hex', (2n ** 63n).toString(16), '8000000000000000');
eq('Number(2^53+1)', String(Number(2n ** 53n + 1n)), '9007199254740992');

t('grid', function () {
    // Every operation on values around the boundaries: printed, compared
    // between builds and JIT modes.
    var xs = [0n, 1n, -1n, 2n ** 31n - 1n, 2n ** 31n, -(2n ** 31n), -(2n ** 31n) - 1n, 2n ** 32n,
              2n ** 63n - 1n, -(2n ** 63n), 2n ** 64n + 5n, 123456789012345678901234567890n];
    var h = 0;
    for (var i = 0; i < xs.length; i++) for (var j = 0; j < xs.length; j++) {
        var a = xs[i], b = xs[j];
        var r = [a + b, a - b, a * b, a & b, a | b, a ^ b, a < b, a == b, a << 3n, a >> 3n,
                 BigInt.asIntN(32, a), BigInt.asUintN(33, a), -a, ~a];
        if (b != 0n) r.push(a / b, a % b);
        var s = r.join(',');
        for (var k = 0; k < s.length; k++) h = (h * 31 + s.charCodeAt(k)) | 0;
    }
    return h;
});
t('map-keys', function () {
    var m = new Map();
    m.set(2n ** 31n, 'a'); m.set(-(2n ** 31n) - 1n, 'b'); m.set(2n ** 64n, 'c');
    return [m.get(2n ** 31n), m.get(-(2n ** 31n) - 1n), m.get(2n ** 64n), m.has(2n ** 31n - 1n)].join(',');
});

console.error('JITTEST-BEGIN');
log.forEach(function (l) { console.error(l); });
console.error('JITTEST-END');

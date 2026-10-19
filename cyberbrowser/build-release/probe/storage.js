var ok = true, fails = []; function check(c, m) { if (!c) { ok = false; fails.push(m); } }
localStorage.setItem('a', '1'); localStorage.setItem('b', '2'); sessionStorage.setItem('s', 'x');
check(localStorage.length === 2, 'length 2'); check(sessionStorage.length === 1, 'session separate');
check(localStorage.key(0) === 'a' && localStorage.key(1) === 'b' && localStorage.key(2) === null, 'key order');
var big = new Array(30001).join('x'); localStorage.setItem('big', big);
check(localStorage.getItem('big').length === 30000, 'big value round-trips');
var longKey = new Array(1001).join('k'); localStorage.setItem(longKey, 'v'); check(localStorage.getItem(longKey) === 'v', 'long key');
localStorage.setItem('a', 'updated'); check(localStorage.getItem('a') === 'updated' && localStorage.length === 4, 'update in place');
localStorage.setItem('nul', 'x\u0000y'); check(localStorage.getItem('nul') === 'x\u0000y', 'embedded NUL');
localStorage.setItem('u', 'é中'); check(localStorage.getItem('u') === 'é中', 'unicode');
localStorage.removeItem('b'); check(localStorage.getItem('b') === null && localStorage.key(1) === 'big', 'remove keeps order');
var threw = null; try { localStorage.setItem('huge', new Array(6 * 1024 * 1024).join('z')); } catch (e) { threw = e; }
check(threw && threw.name === 'QuotaExceededError' && threw.code === 22, 'quota throws QuotaExceededError: ' + (threw && threw.name));
check(localStorage.getItem('huge') === null, 'failed write not stored');
for (var i = 0; i < 5000; i++) localStorage.setItem('k' + i, 'v' + i);
check(localStorage.getItem('k4999') === 'v4999', 'more than 1024 items');
localStorage.clear(); check(localStorage.length === 0 && localStorage.getItem('a') === null, 'clear');
localStorage.setItem('after', 'clear'); check(localStorage.getItem('after') === 'clear', 'usable after clear');
if (!ok) throw new Error('STORAGE_PROBE_FAIL: ' + fails.join(' | '));

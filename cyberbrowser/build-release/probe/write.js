var fails = []; function check(c, m) { if (!c) fails.push(m); }
localStorage.clear();
localStorage.setItem('persist', 'yes');
localStorage.setItem('u', 'é中😀');
localStorage.setItem('nul', 'x\u0000y');
var longKey = new Array(2001).join('k'); localStorage.setItem(longKey, 'long');
localStorage.setItem('gone', 'should be removed');
var huge = new Array(6 * 1024 * 1024 + 1).join('z');
try { localStorage.setItem('huge', huge); } catch (e) { fails.push('localStorage 6MB threw ' + e.name); }
var threw = null; try { sessionStorage.setItem('huge', huge); } catch (e) { threw = e; }
check(threw && threw.name === 'QuotaExceededError', 'sessionStorage quota still enforced');
sessionStorage.setItem('sess', 'temp');
for (var i = 0; i < 3000; i++) localStorage.setItem('k' + i, 'v' + i);
localStorage.setItem('k0', 'updated');
localStorage.removeItem('gone');
if (fails.length) throw new Error('WRITE_FAIL: ' + fails.join(' | '));

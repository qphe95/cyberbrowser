var fails = []; function check(c, m) { if (!c) fails.push(m); }
check(localStorage.getItem('persist') === 'yes', 'persist');
check(localStorage.getItem('u') === 'é中😀', 'unicode');
check(localStorage.getItem('nul') === 'x\u0000y', 'embedded NUL');
check(localStorage.getItem(new Array(2001).join('k')) === 'long', 'long key');
check(localStorage.getItem('gone') === null, 'removed key stays removed');
var h = localStorage.getItem('huge'); check(h && h.length === 6 * 1024 * 1024, 'huge value: ' + (h && h.length));
check(localStorage.getItem('k0') === 'updated' && localStorage.getItem('k2999') === 'v2999', 'many items + last write');
check(localStorage.length === 3005, 'length ' + localStorage.length);
check(localStorage.key(0) === 'persist', 'order preserved: ' + localStorage.key(0));
check(sessionStorage.getItem('sess') === null && sessionStorage.length === 0, 'sessionStorage not persisted');
if (fails.length) throw new Error('READ_FAIL: ' + fails.join(' | '));

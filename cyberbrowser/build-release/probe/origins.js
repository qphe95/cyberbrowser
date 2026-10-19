var fails = []; function check(c, m) { if (!c) fails.push(m); }
check(localStorage.getItem('persist') === 'yes', 'localhost data visible');
location.href = 'https://example.com/page';
check(location.origin === 'https://example.com', 'origin switched: ' + location.origin);
check(localStorage.getItem('persist') === null && localStorage.length === 0, 'example.com isolated, length ' + localStorage.length);
localStorage.setItem('ex', 'ample');
location.href = 'https://localhost/';
check(localStorage.getItem('persist') === 'yes' && localStorage.getItem('ex') === null, 'back to localhost');
location.href = 'https://example.com:8443/';
check(localStorage.getItem('ex') === null, 'port is part of origin');
if (fails.length) throw new Error('ORIGIN_FAIL: ' + fails.join(' | '));

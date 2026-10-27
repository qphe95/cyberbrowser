// The escape tests' server (escape_test.sh): a page that keeps a counter in
// localStorage and sets cookies (one HttpOnly), and /echo, which answers
// with the request's Cookie header.
//   node server.js PORT LOGFILE
const http = require('http');
const fs = require('fs');
const port = +process.argv[2];
const logfile = process.argv[3];
const page = `<!doctype html><html><head><title>storage test</title></head><body>
<h1 id="out">running</h1>
<script>
  var n = +(localStorage.getItem('count') || 0) + 1;
  localStorage.setItem('count', String(n));
  document.cookie = 'fromjs=' + n + '; max-age=3600; path=/';
  var text = 'count=' + n + ' cookie=[' + document.cookie + ']';
  document.getElementById('out').textContent = text;
  fetch('/ping').then(function (r) { return r.text(); }).then(function (t) {
    document.getElementById('out').textContent = text + ' ping=' + t;
  });
</script></body></html>`;
http.createServer((req, res) => {
  const cookie = req.headers.cookie || '';
  fs.appendFileSync(logfile, `${req.headers.host} ${req.method} ${req.url} cookie=[${cookie}]\n`);
  if (req.url === '/echo') {
    res.writeHead(200, { 'content-type': 'text/plain' });
    res.end(cookie);
  } else if (req.url === '/ping') {
    res.writeHead(200, { 'content-type': 'text/plain' });
    res.end('pong');
  } else {
    res.writeHead(200, {
      'content-type': 'text/html',
      'set-cookie': ['server=1; Max-Age=3600; Path=/', 'secret=2; Max-Age=3600; Path=/; HttpOnly'],
    });
    res.end(page);
  }
}).listen(port, '127.0.0.1');

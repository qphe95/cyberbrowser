// The window tests' server (window_test.sh).
//   /busy: 60 colored bands, 100 px each; 3 s after loading the page's JS
//          is busy for 4 s, then makes every band gray.
//   /anim: a counter that goes up every 200 ms (frames that keep coming).
//   node server.js PORT
const http = require('http');
const port = +process.argv[2];
const busy = `<!doctype html><html><head><title>busy</title>
<style>body { margin: 0 } .b { height: 100px }</style></head><body><div id="bands"></div>
<script>
  function bands(colors) {
    var h = '';
    for (var i = 0; i < 60; i++) h += '<div class="b" style="background:' + colors[i % colors.length] + '"></div>';
    document.getElementById('bands').innerHTML = h;
  }
  bands(['#e6194b', '#3cb44b', '#ffe119', '#4363d8', '#f58231', '#911eb4', '#46f0f0', '#f032e6', '#bcf60c', '#fabebe']);
  setTimeout(function () {
    console.error('[busy] from ' + Date.now());
    var t = Date.now();
    while (Date.now() - t < 4000) {}
    bands(['#808080']);
    console.error('[busy] to ' + Date.now());
  }, 3000);
</script></body></html>`;
const anim = `<!doctype html><html><head><title>anim</title>
<style>body { margin: 0 } #n { font-size: 200px; height: 300px; background: #ffe119 }</style></head><body>
<div id="n">0</div>
<script>
  var n = 0;
  setInterval(function () { document.getElementById('n').textContent = String(++n); }, 200);
</script></body></html>`;
http.createServer((req, res) => {
  res.writeHead(200, { 'content-type': 'text/html' });
  res.end(req.url === '/anim' ? anim : busy);
}).listen(port, '127.0.0.1');

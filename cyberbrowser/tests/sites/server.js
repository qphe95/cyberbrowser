// The site-isolation tests' server (sites_test.sh), on two ports.  The pages
// report with console.error lines starting "[sites]", which land in the tab's
// log.  127.0.0.1 and localhost are different sites.
//   node server.js PORT_A PORT_B
const http = require('http');
const [A, B] = process.argv.slice(2).map(Number);

const page = (title, body) => `<!doctype html><html><head><title>${title}</title></head><body>${body}</body></html>`;
const say = `function say(s) { console.error('[sites] ' + s); }`;

const routes = {
  // A page with a frame from another site: the frame keeps its storage and
  // cookies apart, and the two talk only through postMessage.
  '/top': () => page('top', `<iframe id="f" src="http://localhost:${A}/frame"></iframe><script>
    ${say}
    localStorage.setItem('who', 'top');
    window.addEventListener('message', function (e) {
      say('top got ' + JSON.stringify(e.data) + ' from ' + e.origin + ' source-is-frame=' + (e.source === f.contentWindow));
      if (e.data && e.data.hello) f.contentWindow.postMessage({ reply: 'hi frame' }, 'http://localhost:${A}');
    });
    var f = document.getElementById('f');
    f.addEventListener('load', function () {
      var w = f.contentWindow;
      say('top sees contentDocument=' + f.contentDocument + ' document=' + (w && w.document) + ' frame-ls=' + (function () { try { return w.localStorage; } catch (e) { return 'threw'; } })());
      w.postMessage({ wrong: 'origin' }, 'http://example.com');
    });
    // An about:blank frame made by script: the page's own origin.
    var b = document.createElement('iframe');
    b.src = 'about:blank';
    document.body.appendChild(b);
    setTimeout(function () { say('blank frame document=' + (b.contentDocument ? 'reachable' : 'null')); }, 1000);
  </script>`),
  '/frame': () => page('frame', `<script>
    ${say}
    localStorage.setItem('who', 'frame');
    document.cookie = 'framecookie=1; path=/';
    var p = (function () { try { return parent.document; } catch (e) { return 'threw ' + e.name; } })();
    say('frame sees parent.document=' + p + ' frameElement=' + window.frameElement + ' ls=' + localStorage.getItem('who') + ' cookie=' + document.cookie);
    window.addEventListener('message', function (e) {
      say('frame got ' + JSON.stringify(e.data) + ' from ' + e.origin);
    });
    parent.postMessage({ hello: 'from frame', ls: localStorage.getItem('who') }, '*');
  </script>`),
  // The frame's site as a page of its own: none of what it kept as a frame.
  '/alone': () => page('alone', `<script>
    ${say}
    say('alone ls=' + localStorage.getItem('who') + ' cookie=' + document.cookie);
  </script>`),
  // ORB and CORS: reads across sites.
  '/orb': () => page('orb', `<script>
    ${say}
    function t(name, url, opts) {
      fetch(url, opts).then(function (r) { return r.text(); })
        .then(function (s) { say(name + ' read ' + s.length); }, function (e) { say(name + ' failed'); });
    }
    var o = 'http://localhost:${A}';
    t('json-no-cors', o + '/data.json', { mode: 'no-cors' });
    t('html-no-cors', o + '/alone', { mode: 'no-cors' });
    t('script-no-cors', o + '/script.js', { mode: 'no-cors' });
    t('cors-without-acao', o + '/data.json');
    t('cors-with-acao', o + '/acao');
  </script>`),
  // Private Network Access, from a page on port B (made public).
  '/pna': () => page('pna', `<iframe id="f" src="http://127.0.0.1:${A}/frame"></iframe><script>
    ${say}
    function t(name, url) {
      fetch(url).then(function (r) { return r.text(); })
        .then(function (s) { say(name + ' read ' + s.length); }, function () { say(name + ' failed'); });
    }
    t('public-to-local', 'http://127.0.0.1:${A}/acao');
    t('public-to-public', 'http://127.0.0.1:${B}/acao');
  </script>`),
  '/data.json': () => ['application/json', '{"secret": "across sites"}'],
  '/script.js': () => ['text/javascript', 'var fromScript = 1;'],
  '/acao': () => ['text/plain', 'shared', { 'access-control-allow-origin': '*' }],
};

for (const port of [A, B]) {
  http.createServer((req, res) => {
    const r = routes[req.url.split('?')[0]];
    if (!r) { res.writeHead(404); res.end(); return; }
    const out = r();
    if (typeof out === 'string') {
      res.writeHead(200, { 'content-type': 'text/html' });
      res.end(out);
    } else {
      res.writeHead(200, Object.assign({ 'content-type': out[0] }, out[2] || {}));
      res.end(out[1]);
    }
  }).listen(port, '127.0.0.1');
}

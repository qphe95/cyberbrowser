// The Khronos WebGL conformance suite and the WebGPU CTS, one page after
// another in one tab, for conformance_test.sh.
//
//   node conformance_server.js <webgl1|webgl2|webgpu> <suite dir> <port> <results dir> [filter]
//
// pages.txt lists the pages without a result in results.json ("<k> <url>"),
// so a run picks up where the last one stopped. The UI script opens each by
// its own URL (not through a redirect: the browser keeps the URL from before
// one, and the page's relative URLs would miss) and waits for step-<k>.flag
// (page k answered) or the page's time to be up. A page that never answered
// is recorded as a crash when the next one, or /end, is opened.
// Pages report by posting their counts to /result:
//  - WebGL: each page is served with window.webglTestHarness in front of its
//    scripts, the object js-test-pre.js reports to when the suite's own
//    frame harness runs it;
//  - WebGPU: /cyber-cts.html runs one spec file's cases with the CTS's own
//    loader (out/common), each with a time limit.
// The first line out says how many pages are left to run.
// (tests/conformance/failures.js lists what failed.)
const http = require('http'), fs = require('fs'), path = require('path');
const [suite, rootArg, port, out, filter] = process.argv.slice(2);
const origin = 'http://127.0.0.1:' + port;
/* WebGL pages that pass take a few seconds; one that hangs reports what it
 * has 5 s before its time is up. */
const PAGE_MS = suite === 'webgpu' ? 300000 : 30000, CASE_MS = 30000;

function cmpVersion(a, b) {
    const x = a.split('.').map(Number), y = b.split('.').map(Number);
    for (let i = 0; i < 3; i++) if ((x[i] || 0) !== (y[i] || 0)) return (x[i] || 0) - (y[i] || 0);
    return 0;
}
/* The WebGL suite's lists: 00_test_list.txt files, with --min-version and --max-version. */
function webglList(root, version, file) {
    const dir = path.posix.dirname(file), tests = [];
    for (let line of fs.readFileSync(path.join(root, file), 'utf8').split(/\r?\n/)) {
        line = line.trim();
        if (!line || line.startsWith('//') || line.startsWith('#')) continue;
        const words = line.split(/\s+/);
        let ok = true;
        while (words.length > 1 && words[0].startsWith('--')) {
            const opt = words.shift();
            if (opt === '--slow') continue;
            const v = words.shift();
            if (opt === '--min-version' && cmpVersion(version, v) < 0) ok = false;
            if (opt === '--max-version' && cmpVersion(version, v) > 0) ok = false;
        }
        if (!ok) continue;
        const name = dir === '.' ? words[0] : dir + '/' + words[0];
        if (name.endsWith('.txt')) tests.push(...webglList(root, version, name));
        else tests.push(name);
    }
    return tests;
}

let root, pages;
if (suite === 'webgpu') {
    const cts = path.resolve(rootArg);
    root = path.join(cts, 'out');
    /* One page per spec file, from the WPT variants (one per test). */
    const html = fs.readFileSync(path.join(cts, 'out-wpt', 'cts.https.html'), 'utf8');
    const files = new Set();
    for (const m of html.matchAll(/q=(webgpu:[^:']*):/g)) files.add(m[1] + ':*');
    pages = [...files].map(id => ({ id, url: '/cyber-cts.html?q=' + encodeURIComponent(id) }));
} else {
    root = path.join(path.resolve(rootArg), 'sdk', 'tests');
    const version = suite === 'webgl2' ? '2.0.1' : '1.0.4';
    pages = webglList(root, version, '00_test_list.txt')
        .map(id => ({ id, url: '/' + id + '?webglVersion=' + version[0] + '&quiet=1' }));
}
if (filter) pages = pages.filter(p => p.id.includes(filter));

const resultsFile = path.join(out, 'results.json');
const results = fs.existsSync(resultsFile) ? JSON.parse(fs.readFileSync(resultsFile, 'utf8')) : {};
let current = -1;
const left = pages.map((p, i) => i).filter(i => !results[pages[i].id]);
fs.writeFileSync(path.join(out, 'pages.txt'), left.map(i => `${i + 1} ${origin}${pages[i].url}\n`).join(''));
console.log(`${left.length} of ${pages.length} pages to run`);

function save() { fs.writeFileSync(resultsFile, JSON.stringify(results, null, 1)); }
function record(r) {
    results[r.page] = r;
    console.log(`${r.status} ${r.page} ${r.pass}/${r.pass + r.fail}` + (r.skip ? ` skip ${r.skip}` : ''));
    save();
}

function send() {
    return `function send(status, r) {
    if (window.__cyberSent) return; window.__cyberSent = true;
    r.status = status; r.ms = Date.now() - window.__cyberStart;
    fetch('/result', { method: 'POST', body: JSON.stringify(r) });
}`;
}
/* In front of a WebGL page's own scripts. */
function webglHarness(id) {
    return `<script>
(function () {
  window.__cyberStart = Date.now();
  ${send()}
  var r = { page: ${JSON.stringify(id)}, pass: 0, fail: 0, skip: 0, fails: [] };
  window.webglTestHarness = {
    reportResults: function (url, success, msg, skipped) {
      if (skipped) r.skip++; else if (success) r.pass++;
      else { r.fail++; if (r.fails.length < 20) r.fails.push(String(msg).slice(0, 300)); }
    },
    notifyFinished: function () { setTimeout(function () { send('done', r); }, 0); }
  };
  window.addEventListener('error', function (e) {
    if (r.fails.length < 20) r.fails.push('uncaught: ' + (e.message || e));
  });
  setTimeout(function () { send('timeout', r); }, ${PAGE_MS - 5000});
})();
</script>`;
}
/* A spec file of the CTS: its cases one after another, warnings counted as passes (as in WPT). */
function ctsPage(id) {
    return `<!doctype html><meta charset=utf-8><title>CTS ${id}</title><body>
<script type=module>
import { DefaultTestFileLoader } from '/common/internal/file_loader.js';
import { Logger } from '/common/internal/logging/logger.js';
import { parseQuery } from '/common/internal/query/parseQuery.js';
import { prettyPrintLog } from '/common/internal/logging/log_message.js';
window.__cyberStart = Date.now();
${send()}
const r = { page: ${JSON.stringify(id)}, pass: 0, fail: 0, skip: 0, timeouts: 0, fails: [] };
setTimeout(() => send('timeout', r), ${PAGE_MS - 15000});
const note = s => { if (r.fails.length < 30) r.fails.push(String(s).slice(0, 400)); };
try {
  const cases = await new DefaultTestFileLoader().loadCases(parseQuery(${JSON.stringify(id)}));
  const log = new Logger();
  for (const tc of cases) {
    const name = tc.query.toString();
    const [rec, res] = log.record(name);
    const timedOut = await Promise.race([tc.run(rec, []).then(() => false),
                                         new Promise(ok => setTimeout(() => ok(true), ${CASE_MS}))]);
    if (timedOut) { r.fail++; r.timeouts++; note(name + ': timed out'); continue; }
    if (res.status === 'pass' || res.status === 'warn') r.pass++;
    else if (res.status === 'skip') r.skip++;
    else { r.fail++; note(name + ': ' + (res.logs || []).map(prettyPrintLog).join(' | ')); }
  }
  send('done', r);
} catch (e) {
  note('error: ' + (e && e.stack || e));
  send('error', r);
}
</script>`;
}

const types = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.png': 'image/png',
    '.jpg': 'image/jpeg', '.gif': 'image/gif', '.webm': 'video/webm', '.mp4': 'video/mp4', '.ogv': 'video/ogg',
    '.json': 'application/json', '.txt': 'text/plain', '.svg': 'image/svg+xml', '.wav': 'audio/wav',
    '.mp3': 'audio/mpeg', '.webp': 'image/webp', '.bmp': 'image/bmp', '.ico': 'image/x-icon',
    '.wgsl': 'text/plain', '.wasm': 'application/wasm' };

http.createServer((req, res) => {
    if (process.env.TRACE) console.log(req.method + ' ' + req.url);
    const url = new URL(req.url, origin);
    /* Page i is opened: the one before it is done, answered or not. */
    const opened = i => {
        if (current >= 0 && current !== i && !results[pages[current].id])
            record({ page: pages[current].id, status: 'crash', pass: 0, fail: 0, skip: 0 });
        current = i;
    };
    if (url.pathname === '/end') {
        opened(-1);
        res.writeHead(200, { 'content-type': 'text/html' });
        res.end('<p>done</p>');
        setTimeout(() => process.exit(0), 1000);
        return;
    }
    const page = pages.findIndex(p => p.url.split('?')[0] === url.pathname &&
        (suite !== 'webgpu' || p.id === url.searchParams.get('q')));
    if (page >= 0) opened(page);
    if (url.pathname === '/result' && req.method === 'POST') {
        let body = '';
        req.on('data', d => body += d);
        req.on('end', () => {
            try {
                const r = JSON.parse(body);
                if (current >= 0 && r.page === pages[current].id && !results[r.page]) {
                    record(r);
                    fs.writeFileSync(path.join(out, `step-${current + 1}.flag`), r.status);
                }
            } catch (e) { console.log('bad result ' + e); }
            res.writeHead(204); res.end();
        });
        return;
    }
    if (url.pathname === '/cyber-cts.html') {
        res.writeHead(200, { 'content-type': 'text/html' });
        return res.end(ctsPage(url.searchParams.get('q')));
    }
    const file = path.join(root, decodeURIComponent(url.pathname));
    if (!file.startsWith(root) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
        res.writeHead(404); return res.end();
    }
    let data = fs.readFileSync(file);
    const ext = path.extname(file).toLowerCase();
    if (suite !== 'webgpu' && ext === '.html' && page >= 0) {
        let s = data.toString('utf8');
        const m = /<head[^>]*>/i.exec(s);
        const h = webglHarness(pages[current].id);
        s = m ? s.slice(0, m.index + m[0].length) + h + s.slice(m.index + m[0].length) : h + s;
        data = Buffer.from(s, 'utf8');
    }
    res.writeHead(200, { 'content-type': types[ext] || 'application/octet-stream' });
    res.end(data);
}).listen(+port, '127.0.0.1');

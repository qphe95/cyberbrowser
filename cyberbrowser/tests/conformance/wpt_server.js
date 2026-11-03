// web-platform-tests' testharness.js tests, one page after another in one
// tab, for wpt_test.sh.
//
//   node wpt_server.js <wpt dir> <port> <results dir> <dir or file under wpt>...
//
// pages.txt lists the pages without a result in results.json
// ("<k> <url> <ms>"), so a run picks up where the last one stopped.  The UI
// script opens each and waits for step-<k>.flag (page k reported) or its
// time to be up; a page that never reported is recorded as a crash when the
// next one, or /end, is opened.
//
// What `wpt serve` does that this does too: /resources/testharnessreport.js
// (here: it posts the page's subtests to /result), .any.html and
// .window.html wrappers for .any.js and .window.js tests (window scope only),
// <meta name=variant> variants, .headers files, and the common {{...}}
// substitutions in .sub. files.  Python handlers (.py) are not run, but for
// the few emulated in PY.
// PORTS=n (default 1): it listens on port, port + 1, ... port + n - 1, each
// with its own current page, for n browsers running pages at once (each from
// its own port).
// Reftests (rel=match / rel=mismatch) are counted in reftests.txt; with
// REFTEST=1 they are what runs instead: each test page and reference (once,
// however many tests share it) is a step whose content the UI script saves
// (pages.txt's fourth column), a
// script injected into them saying when they are ready (load, no
// reftest-wait, fonts, two frames), and reftest_compare.js compares them.
// (tests/conformance/failures.js lists what failed.)
const http = require('http'), fs = require('fs'), path = require('path');
const [rootArg, port, out, ...dirs] = process.argv.slice(2);
const root = path.resolve(rootArg);
const origin = 'http://127.0.0.1:' + port;
/* testharness's own time limits (10 s, long 60 s), scaled by WPT_TIMEOUT_MULT
 * (testharness's timeout_multiplier; default 1), and some room. */
const MULT = +(process.env.WPT_TIMEOUT_MULT || 1);
const SHORT_MS = 10000 * MULT, LONG_MS = 60000 * MULT, SLACK_MS = 2000 + 3000 * MULT;

/* ---- the tests ---- */

const SKIP_DIR = /^(support|resources|reference|references|tools|crashtests|\.|_)/;
function walk(rel, list) {
    const abs = path.join(root, rel);
    if (!fs.existsSync(abs)) { console.log('no such path: ' + rel); return; }
    if (fs.statSync(abs).isFile()) { list.push(rel.split(path.sep).join('/')); return; }
    for (const e of fs.readdirSync(abs, { withFileTypes: true }).sort((a, b) => a.name < b.name ? -1 : 1)) {
        if (e.isDirectory()) { if (!SKIP_DIR.test(e.name)) walk(path.join(rel, e.name), list); }
        else list.push(path.join(rel, e.name).split(path.sep).join('/'));
    }
}

const pages = [], reftests = [];
function addPage(id, file, long) {
    pages.push({ id, url: '/' + id, ms: (long ? LONG_MS : SHORT_MS) + SLACK_MS, file });
}
for (const d of dirs) {
    const files = [];
    walk(d, files);
    for (const f of files) {
        const base = path.posix.basename(f);
        if (/-(ref|notref)\.|-manual\.|^ref-|-ref$/.test(base) || /\/(reference|support)\//.test(f)) continue;
        const m = /^(.*)\.(any|window|worker)\.js$/.exec(f);
        if (m) {
            const src = fs.readFileSync(path.join(root, f), 'utf8');
            const meta = scriptMeta(src);
            const vs = meta.variants.length ? meta.variants : [''];
            /* (.any.js: in a window and a dedicated worker unless its globals
             * say otherwise; .worker.js: in a dedicated worker) */
            const globals = m[2] === 'any' ? (meta.global || 'window,dedicatedworker') : m[2];
            const has = g => new RegExp('(^|,)\\s*(' + g + ')\\s*(,|$)').test(globals);
            if (m[2] === 'window' || (m[2] === 'any' && has('window|default')))
                for (const v of vs) addPage(`${m[1]}.${m[2]}.html${v}`, f, meta.long);
            if (m[2] === 'worker' || (m[2] === 'any' && has('dedicatedworker|worker|default')))
                for (const v of vs) addPage(`${m[1]}.${m[2] === 'any' ? 'any.worker' : 'worker'}.html${v}`, f, meta.long);
            /* (shared workers too; service workers are not run) */
            if (m[2] === 'any' && has('sharedworker|worker'))
                for (const v of vs) addPage(`${m[1]}.any.sharedworker.html${v}`, f, meta.long);
            continue;
        }
        if (!/\.(html?|xht|xhtml|xml|svg)$/.test(base)) continue;
        const src = fs.readFileSync(path.join(root, f), 'utf8');
        if (/rel\s*=\s*["']?(match|mismatch)\b/i.test(src)) { reftests.push(f); continue; }
        if (!/\/resources\/testharness\.js/.test(src)) continue;
        const long = /<meta\s+name=["']?timeout["']?\s+content=["']?long/i.test(src);
        const variants = [...src.matchAll(/<meta\s+name=["']?variant["']?\s+content=["']([^"']*)["']/gi)].map(v => v[1]);
        for (const v of variants.length ? variants : ['']) addPage(f + v, f, long);
    }
}

/* "// META: key=value" lines of a .any.js / .window.js test. */
function scriptMeta(src) {
    const meta = { scripts: [], variants: [], long: false, global: null, title: null };
    for (const line of src.split(/\r?\n/)) {
        const m = /^\/\/\s*META:\s*(\w+)=(.*)$/.exec(line.trim());
        if (!m) { if (line.trim() && !line.trim().startsWith('//')) break; continue; }
        const [, k, v] = m;
        if (k === 'script') meta.scripts.push(v.trim());
        else if (k === 'variant') meta.variants.push(v.trim());
        else if (k === 'timeout') meta.long = v.trim() === 'long';
        else if (k === 'global') meta.global = v.trim();
        else if (k === 'title') meta.title = v.trim();
    }
    return meta;
}

function wrapper(jsPath, kind) {
    const src = fs.readFileSync(path.join(root, jsPath), 'utf8');
    const meta = scriptMeta(src);
    const esc = s => s.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;');
    return `<!doctype html>
<meta charset=utf-8>
${meta.long ? '<meta name=timeout content=long>' : ''}
${meta.title ? `<title>${esc(meta.title)}</title>` : ''}
${kind === 'any' ? `<script>self.GLOBAL = { isWindow: () => true, isWorker: () => false, isShadowRealm: () => false };</script>` : ''}
<script src="/resources/testharness.js"></script>
<script src="/resources/testharnessreport.js"></script>
${meta.scripts.map(s => `<script src="${esc(s)}"></script>`).join('\n')}
<div id=log></div>
<script src="/${esc(jsPath)}"></script>
`;
}

/* A dedicated-worker test's page: its results come from the worker. */
function workerPage(base, search, shared) {
    return `<!doctype html>
<meta charset=utf-8>
<script src="/resources/testharness.js"></script>
<script src="/resources/testharnessreport.js"></script>
<div id=log></div>
<script>fetch_tests_from_worker(new ${shared ? 'SharedWorker' : 'Worker'}("/${base}.js${search || ''}"));</script>
`;
}

/* What wpt serve's .any.worker.js is: testharness, the META scripts, the
 * test, done(). */
function workerScript(jsPath) {
    const meta = scriptMeta(fs.readFileSync(path.join(root, jsPath), 'utf8'));
    const q = s => JSON.stringify(s);
    return ['self.GLOBAL = { isWindow: () => false, isWorker: () => true, isShadowRealm: () => false };',
            'importScripts("/resources/testharness.js");',
            ...meta.scripts.map(sc => `importScripts(${q(sc)});`),
            `importScripts(${q('/' + jsPath)});`,
            'done();', ''].join('\n');
}

/* ---- reftests ---- */

const REFTEST = !!process.env.REFTEST;
const BLANK = '__blank__.html';
/* A reftest's references (rel=match / mismatch, resolved) and fuzziness. */
function refsOf(f) {
    const src = fs.readFileSync(path.join(root, f), 'utf8');
    const refs = [];
    for (const m of src.matchAll(/<link\b[^>]*>/gi)) {
        const tag = m[0];
        const rel = /rel\s*=\s*["']?(match|mismatch)\b/i.exec(tag);
        const href = /href\s*=\s*["']([^"']+)["']|href\s*=\s*([^\s>]+)/i.exec(tag);
        if (!rel || !href) continue;
        const h = href[1] || href[2];
        const abs = new URL(h, origin + '/' + f);
        /* (about:blank: a blank page of the server's, BLANK, that can say it is ready) */
        const url = abs.protocol === 'about:' ? BLANK : abs.pathname.slice(1) + abs.search;
        refs.push({ kind: rel[1].toLowerCase(), url });
    }
    const fuzzy = [...src.matchAll(/<meta\s+name=["']?fuzzy["']?\s+content=["']([^"']*)["']/gi)].map(m => m[1]);
    return { refs, fuzzy };
}
const steps = [];                 /* { k, url, shot }, one per page */
const refInfo = {};
if (REFTEST) {
    fs.mkdirSync(path.join(out, 'shots'), { recursive: true });
    const seen = new Set();
    for (const t of reftests) {
        const info = refsOf(t);
        if (!info.refs.length) continue;
        refInfo[t] = info;
        for (const u of [t, ...info.refs.map(r => r.url)]) {
            if (seen.has(u)) continue;
            seen.add(u);
            steps.push({ k: steps.length + 1, url: u, shot: path.join(out, 'shots', `${steps.length + 1}.rle`) });
        }
    }
    fs.writeFileSync(path.join(out, 'reftests.json'), JSON.stringify({ refInfo, steps }, null, 1));
}
/* Says when a reftest page is ready to be compared (posted to /refready). */
const REFREADY = `<script style="display: none !important">//<![CDATA[
(function () {
  function ready() {
    var x = new XMLHttpRequest();
    x.open('POST', '/refready'); x.send(location.pathname + location.search);
  }
  function check() {
    var de = document.documentElement;
    if (de && de.classList && de.classList.contains('reftest-wait')) { setTimeout(check, 50); return; }
    /* (two frames, as wptrunner waits: the second callback runs once the
     * first frame is with the GPU process, and an image that frame was
     * still waiting for is in the one built after it) */
    var go = function () { requestAnimationFrame(function () { requestAnimationFrame(function () { setTimeout(ready, 0); }); }); };
    if (document.fonts && document.fonts.ready) document.fonts.ready.then(go, go); else go();
  }
  if (document.readyState === 'complete') setTimeout(check, 0); else addEventListener('load', function () { setTimeout(check, 0); });
})();
//]]></script>`; /* (CDATA: well-formed in XHTML too) */

/* ---- results ---- */

fs.mkdirSync(out, { recursive: true });
fs.writeFileSync(path.join(out, 'reftests.txt'), reftests.join('\n') + '\n');
const resultsFile = path.join(out, 'results.json');
const results = fs.existsSync(resultsFile) ? JSON.parse(fs.readFileSync(resultsFile, 'utf8')) : {};
/* Per port: the page its browser has open (-1: none), and the ports whose
 * browser is done (/end). */
const NPORTS = Math.max(1, +(process.env.PORTS || 1));
const currentOf = {}, ended = new Set();
const left = REFTEST ? [] : pages.map((p, i) => i).filter(i => !results[pages[i].id]);
if (REFTEST) {
    const todo = steps.filter(st => !fs.existsSync(st.shot));
    fs.writeFileSync(path.join(out, 'pages.txt'),
        todo.map(st => `${st.k} ${origin}/${st.url} 4000 ${st.shot.split(path.sep).join('/')}\n`).join(''));
    console.log(`${todo.length} of ${steps.length} reftest steps to run (${Object.keys(refInfo).length} reftests)`);
} else {
    fs.writeFileSync(path.join(out, 'pages.txt'),
        left.map(i => `${i + 1} ${origin}${pages[i].url} ${pages[i].ms}\n`).join(''));
    console.log(`${left.length} of ${pages.length} pages to run (${reftests.length} reftests not run)`);
}

function save() { fs.writeFileSync(resultsFile, JSON.stringify(results, null, 1)); }
function record(r) {
    results[r.page] = r;
    console.log(`${r.status} ${r.page} ${r.pass}/${r.pass + r.fail}` + (r.skip ? ` skip ${r.skip}` : ''));
    save();
}

/* Stands in for /resources/testharnessreport.js. */
const REPORT = `(function () {
  if (typeof setup === 'function' && ${MULT} !== 1) setup({ timeout_multiplier: ${MULT} });
  var start = Date.now();
  var TEST = ['PASS', 'FAIL', 'TIMEOUT', 'NOTRUN', 'PRECONDITION_FAILED'];
  var HARNESS = ['OK', 'ERROR', 'TIMEOUT', 'PRECONDITION_FAILED'];
  add_completion_callback(function (tests, status) {
    var r = { page: (location.pathname + location.search).slice(1), status: HARNESS[status.status] || 'ERROR',
              pass: 0, fail: 0, skip: 0, fails: [], ms: Date.now() - start };
    if (status.status !== 0 && status.message) r.fails.push('harness: ' + String(status.message).slice(0, 300));
    for (var i = 0; i < tests.length; i++) {
      var t = tests[i];
      if (t.status === 0) r.pass++;
      else if (t.status === 4) r.skip++;
      else { r.fail++; if (r.fails.length < ${+(process.env.WPT_FAILS_MAX || 200)}) r.fails.push(t.name + ': ' + TEST[t.status] + (t.message ? ' ' + String(t.message).slice(0, 300) : '')); }
    }
    var x = new XMLHttpRequest();
    x.open('POST', '/result');
    x.send(JSON.stringify(r));
  });
})();
`;

/* {{...}} in .sub. files, for the one host and port there is. */
function substitute(s, url) {
    const port = url.port, origin = url.origin;
    return s.replace(/\{\{([^}]*)\}\}/g, (all, expr) => {
        expr = expr.trim();
        if (/^host$|^domains\[[^\]]*\]$|^hosts\[[^\]]*\]\[[^\]]*\]$/.test(expr)) return '127.0.0.1';
        if (/^ports\[[^\]]*\]\[\d+\]$/.test(expr)) return port;
        if (expr === 'location[host]') return '127.0.0.1:' + port;
        if (expr === 'location[hostname]') return '127.0.0.1';
        if (expr === 'location[port]') return port;
        if (expr === 'location[scheme]') return 'http';
        if (expr === 'location[path]') return url.pathname;
        if (expr === 'location[query]') return url.search;
        if (expr === 'location[server]') return origin;
        let m = /^GET\[([^\]]*)\]$/.exec(expr);
        if (m) return url.searchParams.get(m[1]) || '';
        m = /^uuid\(\)$/.exec(expr);
        if (m) return require('crypto').randomUUID();
        return all;
    });
}

const types = { '.html': 'text/html', '.htm': 'text/html', '.xht': 'application/xhtml+xml',
    '.xhtml': 'application/xhtml+xml', '.xml': 'application/xml', '.js': 'text/javascript',
    '.mjs': 'text/javascript', '.css': 'text/css', '.png': 'image/png', '.jpg': 'image/jpeg',
    '.gif': 'image/gif', '.json': 'application/json', '.txt': 'text/plain', '.svg': 'image/svg+xml',
    '.webp': 'image/webp', '.bmp': 'image/bmp', '.ico': 'image/x-icon', '.woff': 'font/woff',
    '.woff2': 'font/woff2', '.ttf': 'font/ttf', '.otf': 'font/otf', '.webm': 'video/webm',
    '.mp4': 'video/mp4', '.wav': 'audio/wav', '.mp3': 'audio/mpeg', '.wasm': 'application/wasm' };

/* Python handlers emulated: path -> (url) -> [content type, body]. */
const escapeHtml = s => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;')
    .replace(/'/g, '&#x27;');
const PY = {
    /* a document whose <meta charset> is the label */
    '/dom/nodes/encoding.py': url => ['text/html', `<!doctype html><meta charset="${escapeHtml(url.searchParams.get('label') || '')}">`],
};

const server = http.createServer((req, res) => {
    if (process.env.TRACE) console.log(req.method + ' ' + req.url);
    const lport = req.socket.localPort;
    const url = new URL(req.url, 'http://127.0.0.1:' + lport);
    const current = currentOf[lport] === undefined ? -1 : currentOf[lport];
    /* Page i is opened: the one before it is done, answered or not. */
    const opened = i => {
        if (current >= 0 && current !== i && !results[pages[current].id])
            record({ page: pages[current].id, status: 'crash', pass: 0, fail: 0, skip: 0 });
        currentOf[lport] = i;
    };
    if (url.pathname === '/end') {
        opened(-1);
        ended.add(lport);
        res.writeHead(200, { 'content-type': 'text/html' });
        res.end('<p>done</p>');
        if (ended.size >= NPORTS) setTimeout(() => process.exit(0), 1000);
        return;
    }
    const id = (url.pathname + url.search).slice(1);
    const page = pages.findIndex(p => p.id === id);
    if (page >= 0) opened(page);
    if (url.pathname === '/result' && req.method === 'POST') {
        let body = '';
        req.on('data', d => body += d);
        req.on('end', () => {
            try {
                const r = JSON.parse(body);
                const cur = currentOf[lport] === undefined ? -1 : currentOf[lport];
                if (cur >= 0 && r.page === pages[cur].id && !results[r.page]) {
                    record(r);
                    fs.writeFileSync(path.join(out, `step-${cur + 1}.flag`), r.status);
                }
            } catch (e) { console.log('bad result ' + e); }
            res.writeHead(204); res.end();
        });
        return;
    }
    if (url.pathname === '/refready' && req.method === 'POST') {
        const t_req = process.env.CYBER_TRACE_STEPS ? ((Date.now() * 10000) % 1e9 / 1e4).toFixed(1) : null;
        let body = '';
        req.on('data', d => body += d);
        req.on('end', () => {
            const st = steps.find(x => '/' + x.url === body && !fs.existsSync(path.join(out, `step-${x.k}.flag`)));
            if (st) fs.writeFileSync(path.join(out, `step-${st.k}.flag`), 'ready');
            if (t_req) console.log(`[trace] ${t_req} refready request, ${((Date.now() * 10000) % 1e9 / 1e4).toFixed(1)} flag written`);
            res.writeHead(204); res.end();
        });
        return;
    }
    /* (wpt serve's alias for the WebIDL parser idlharness.js loads) */
    if (url.pathname === '/resources/WebIDLParser.js') url.pathname = '/resources/webidl2/lib/webidl2.js';
    if (url.pathname === '/resources/testharnessreport.js') {
        res.writeHead(200, { 'content-type': 'text/javascript' });
        return res.end(REPORT);
    }
    /* a worker test's page, and the script its worker runs */
    const wk = /^\/(.*?)\.(any\.worker|worker)\.(html|js)$/.exec(url.pathname);
    if (wk && fs.existsSync(path.join(root, `${wk[1]}.${wk[2] === 'worker' ? 'worker' : 'any'}.js`))) {
        if (wk[3] === 'html') {
            res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
            return res.end(workerPage(`${wk[1]}.${wk[2]}`, url.search));
        }
        if (wk[2] === 'any.worker') {
            res.writeHead(200, { 'content-type': 'text/javascript' });
            return res.end(workerScript(`${wk[1]}.any.js`));
        }
    }
    const sw = /^\/(.*?)\.any\.sharedworker\.html$/.exec(url.pathname);
    if (sw && fs.existsSync(path.join(root, `${sw[1]}.any.js`))) {
        res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
        return res.end(workerPage(`${sw[1]}.any.worker`, url.search, true));
    }
    const w = /^\/(.*)\.(any|window)\.html$/.exec(url.pathname);
    if (w && fs.existsSync(path.join(root, `${w[1]}.${w[2]}.js`))) {
        res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
        return res.end(wrapper(`${w[1]}.${w[2]}.js`, w[2]));
    }
    if (url.pathname === '/' + BLANK) {
        res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
        return res.end('<!DOCTYPE html><html><head></head><body>' + (REFTEST ? REFREADY : '') + '</body></html>');
    }
    if (PY[url.pathname]) {
        const [type, body] = PY[url.pathname](url);
        res.writeHead(200, { 'content-type': type });
        return res.end(body);
    }
    const file = path.join(root, decodeURIComponent(url.pathname));
    if (!file.startsWith(root) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
        res.writeHead(404); return res.end();
    }
    let data = fs.readFileSync(file);
    const ext = path.extname(file).toLowerCase();
    if (/\.sub\.[^/]*$/.test(file)) data = Buffer.from(substitute(data.toString('utf8'), url), 'utf8');
    const headers = { 'content-type': types[ext] || 'application/octet-stream' };
    for (const hf of [file + '.headers', path.join(path.dirname(file), '__dir__.headers')]) {
        if (!fs.existsSync(hf)) continue;
        for (const line of fs.readFileSync(hf, 'utf8').split(/\r?\n/)) {
            const i = line.indexOf(':');
            if (i > 0) headers[line.slice(0, i).trim().toLowerCase()] = line.slice(i + 1).trim();
        }
        break;
    }
    if (REFTEST && /^(text\/html|application\/xhtml\+xml)/.test(headers['content-type']) &&
        steps.some(st => '/' + st.url.split('?')[0] === url.pathname)) {
        /* (inside the root element, so XHTML stays well-formed) */
        /* (found in the text itself: lower-casing can change its length, 'İ' -> 'i̇') */
        const text = data.toString('utf8'), ends = [...text.matchAll(/<\/html>/gi)];
        const end = ends.length ? ends[ends.length - 1].index : -1;
        data = Buffer.from(end < 0 ? text + REFREADY : text.slice(0, end) + REFREADY + text.slice(end), 'utf8');
    }
    res.writeHead(200, headers);
    res.end(data);
});
for (let i = 0; i < NPORTS; i++) {
    const s = i === 0 ? server : http.createServer(server.listeners('request')[0]);
    s.listen(+port + i, '127.0.0.1');
}

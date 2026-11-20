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
/* WPT_SERVER_PROF=<file>: a CPU profile of this server, seconds 3 to 15 of a
 * run (it is killed at the end, so it cannot write one as it exits). */
if (process.env.WPT_SERVER_PROF) {
    const s = new (require('inspector').Session)();
    s.connect();
    s.post('Profiler.enable');
    setTimeout(() => s.post('Profiler.start'), 3000);
    setTimeout(() => s.post('Profiler.stop', (e, r) => { if (!e) fs.writeFileSync(process.env.WPT_SERVER_PROF, JSON.stringify(r.profile)); }), 15000);
}
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

/* What a test file's text says about it, kept from run to run (next to the
 * results) while its size and time stay: reading every file of css/ takes
 * longer than running most of them. */
const classifyFile = path.join(path.dirname(path.resolve(out)), '.wpt-classify.json');
let classified = {};
try { classified = JSON.parse(fs.readFileSync(classifyFile, 'utf8')); } catch (e) { /* (none yet) */ }
let classifyChanged = false;
function classify(f) {
    const abs = path.join(root, f), st = fs.statSync(abs), had = classified[f];
    if (had && had.t === st.mtimeMs && had.n === st.size) return had;
    const src = fs.readFileSync(abs, 'utf8');
    const c = { t: st.mtimeMs, n: st.size, kind: 'none', long: false, variants: [] };
    /* (flags=interact: it asks a person to act, as -manual does) */
    if (/<meta\s[^>]*name\s*=\s*["']?flags\b[^>]*\binteract\b/i.test(src)) c.kind = 'none';
    else if (/rel\s*=\s*["']?(match|mismatch)\b/i.test(src)) c.kind = 'ref';
    else if (/\/resources\/testharness\.js/.test(src)) {
        c.kind = 'page';
        c.long = /<meta\s+name=["']?timeout["']?\s+content=["']?long/i.test(src);
        /* (content quoted or not, as the manifest's HTML parser takes it) */
        c.variants = [...src.matchAll(/<meta\s+name=["']?variant["']?\s+content=(?:["']([^"']*)["']|([^\s"'>]+))/gi)].map(v => v[1] !== undefined ? v[1] : v[2]);
    }
    classified[f] = c;
    classifyChanged = true;
    return c;
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
        const c = classify(f);
        if (c.kind === 'ref') reftests.push(f);
        else if (c.kind === 'page') for (const v of c.variants.length ? c.variants : ['']) addPage(f + v, f, c.long);
    }
}
if (classifyChanged) fs.writeFileSync(classifyFile, JSON.stringify(classified));

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
    /* (variants: the first one's query, on the test and its references: wptrunner never runs one without) */
    const vm_ = /<meta\s+name=["']?variant["']?\s+content=(?:["']([^"']*)["']|([^\s"'>]+))/i.exec(src);
    const variant = vm_ ? (vm_[1] !== undefined ? vm_[1] : vm_[2]) : undefined;
    if (variant && variant.startsWith('?')) {
        for (const r of refs) if (r.url !== BLANK) r.url += (r.url.includes('?') ? '&' : '?') + variant.slice(1);
        return { refs, fuzzy, url: f + variant };
    }
    return { refs, fuzzy };
}
const steps = [];                 /* { k, url, shot }, one per page */
const refInfo = {};
const PRINT_URLS = new Set();
if (REFTEST) {
    fs.mkdirSync(path.join(out, 'shots'), { recursive: true });
    const seen = new Set();
    for (const t of reftests) {
        const info = refsOf(t);
        if (!info.refs.length) continue;
        refInfo[t] = info;
        /* (wptrunner's print reftests: "-print" in the name's flags, or a print directory; their
         * references printed too, as steps of their own: a reference can be shared with screen tests) */
        if (/-print(\.[\w-]+)*\.[a-z]+$/.test(t.split('?')[0]) || /\/print\//.test(t)) {
            PRINT_URLS.add('/' + t.split('?')[0]);
            for (const r of info.refs) r.url += (r.url.includes('?') ? '&' : '?') + 'cyberprint=1';
        }
        for (const u of [info.url || t, ...info.refs.map(r => r.url)]) {
            if (seen.has(u)) continue;
            seen.add(u);
            steps.push({ k: steps.length + 1, url: u, shot: path.join(out, 'shots', `${steps.length + 1}.rle`) });
        }
    }
    fs.writeFileSync(path.join(out, 'reftests.json'), JSON.stringify({ refInfo, steps }, null, 1));
}
/* (a step by its URL, and the paths that are steps: asked on every request, and
 * a whole run has tens of thousands of steps to look through) */
const STEP_BY_URL = new Map(steps.map(st => ['/' + st.url, st]));
const STEP_PATHS = new Set(steps.map(st => '/' + st.url.split('?')[0]));
/* A step's page is ready (or will never say so): its flag, once. */
function stepReady(pathAndSearch) {
    const st = STEP_BY_URL.get(pathAndSearch);
    if (!st || st.flagged) return;
    st.flagged = true;
    /* (not waited for: making a file takes near a millisecond here, and every
     * shard's requests would wait behind it) */
    fs.writeFile(path.join(out, `step-${st.k}.flag`), 'ready', () => {});
}
/* Files as read, with their headers (a .headers file's among them), by path:
 * a reference many tests share, a font, a support script are asked for
 * hundreds of times, and each asking was five file system calls.  (the tree
 * does not change during a run) */
const FILE_CACHE = new Map();
let fileCacheBytes = 0;
/* Says when a reftest page is ready to be compared (posted to /refready). */
const REFREADY = `<script style="display: none !important">//<![CDATA[
(function () {
  /*PRINT*/
  function ready() {
    var x = new XMLHttpRequest();
    x.open('POST', '/refready'); x.send(location.pathname + location.search);
  }
  var rendered = false;
  function check() {
    var de = document.documentElement;
    /* (print: its pages being set up) */
    if (window.__cyberPrintPending) { setTimeout(check, 20); return; }
    if (de && de.classList && de.classList.contains('reftest-wait')) {
      /* (as wptrunner: TestRendered once, after the first frames, for the page to make its change) */
      if (!rendered) {
        rendered = true;
        requestAnimationFrame(function () { requestAnimationFrame(function () {
          try { de.dispatchEvent(new Event('TestRendered', { bubbles: true })); } catch (e) {}
        }); });
      }
      setTimeout(check, 50);
      return;
    }
    /* (two frames, as wptrunner waits: the second callback runs once the
     * first frame is with the GPU process, and an image that frame was
     * still waiting for is in the one built after it) */
    /* (and, when what was laid out since has not gone to the GPU process yet (a web
     * font's relayout, say), a frame more: __cyberFramePending, the tab's in a reftest run) */
    /* (at most 8 frames more: a running animation lays out every frame, and would keep it pending to the time limit) */
    var extra = 0, held = false, refonts = 0;
    var settled = function () {
      /* (a web face first asked for by the frames since (text laid out late, its fallback held invisible): its load waited for too, as before them) */
      if (document.fonts && document.fonts.status === 'loading' && refonts++ < 3) { document.fonts.ready.then(go, go); return; }
      if (window.__cyberFramePending && window.__cyberFramePending() && extra++ < 8) requestAnimationFrame(function () { setTimeout(settled, 0); });
      else if (extra > 8 && !held && document.getAnimations) {
        /* (still animating, without end: each such held 50 ms in (a few frames, as a browser shot after load sees it), so a test and its
         * reference are shot at the same moment of it, not whenever each got here) */
        held = true;
        try {
          document.getAnimations().forEach(function (a) {
            var t = a.effect && a.effect.getComputedTiming ? a.effect.getComputedTiming() : null;
            if (a.playState === 'running' && t && t.iterations === Infinity && !t.delay) { a.pause(); a.currentTime = 50; }
          });
        } catch (e) {}
        requestAnimationFrame(function () { requestAnimationFrame(function () { setTimeout(ready, 0); }); });
      }
      else ready();
    };
    /* (two frames more than wptrunner: under a full run's load the GPU process may present the frame a test settled in late) */
    var go = function () { requestAnimationFrame(function () { requestAnimationFrame(function () { requestAnimationFrame(function () { requestAnimationFrame(function () { setTimeout(settled, 0); }); }); }); }); };
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
/* Each result goes on the end of a journal as it comes, and into results.json
 * when the run ends (wpt_test.sh takes in what a run stopped short of): writing
 * all of them for each one blocks every browser for the time it takes, which
 * grows through the run. */
const journalFile = path.join(out, 'results.journal');
if (fs.existsSync(journalFile)) {
    for (const line of fs.readFileSync(journalFile, 'utf8').split('\n')) {
        if (!line) continue;
        try { const r = JSON.parse(line); results[r.page] = r; } catch (e) { /* (the last line, cut short) */ }
    }
}
/* Per port: the page its browser has open (-1: none), and the ports whose
 * browser is done (/end). */
const NPORTS = Math.max(1, +(process.env.PORTS || 1));
const pageIndex = new Map(pages.map((p, i) => [p.id, i]));
const currentOf = {}, ended = new Set();
const openedAt = {};
const left = REFTEST ? [] : pages.map((p, i) => i).filter(i => !results[pages[i].id]);
if (REFTEST) {
    const todo = steps.filter(st => !fs.existsSync(st.shot));
    fs.writeFileSync(path.join(out, 'pages.txt'),
        todo.map(st => `${st.k} ${origin}/${st.url} ${+process.env.REFTEST_MS || 4000} ${st.shot.split(path.sep).join('/')}\n`).join(''));
    console.log(`${todo.length} of ${steps.length} reftest steps to run (${Object.keys(refInfo).length} reftests)`);
} else {
    fs.writeFileSync(path.join(out, 'pages.txt'),
        left.map(i => `${i + 1} ${origin}${pages[i].url} ${pages[i].ms}\n`).join(''));
    console.log(`${left.length} of ${pages.length} pages to run (${reftests.length} reftests not run)`);
}

function save() {
    fs.writeFileSync(resultsFile, JSON.stringify(results, null, 1));
    fs.rmSync(journalFile, { force: true });
    /* (how long each took: the next run's shards are shared out by it) */
    const timesFile = path.join(path.dirname(path.resolve(out)), '.wpt-page-ms.json');
    let times = {};
    try { times = JSON.parse(fs.readFileSync(timesFile, 'utf8')); } catch (e) { /* (none yet) */ }
    for (const p of pages) {
        const r = results[p.id];
        if (r) times[p.id] = r.ms === undefined ? p.ms : r.ms;
    }
    fs.writeFileSync(timesFile, JSON.stringify(times));
}
function record(r) {
    results[r.page] = r;
    console.log(`${r.status} ${r.page} ${r.pass}/${r.pass + r.fail}` + (r.skip ? ` skip ${r.skip}` : ''));
    fs.appendFileSync(journalFile, JSON.stringify(r) + '\n');
}

/* Stands in for /resources/testharnessreport.js. */
const REPORT = `(function () {
  /* (as wptrunner: no results table, which a page's own body.innerHTML would break) */
  if (typeof setup === 'function') setup({ output: false, timeout_multiplier: ${MULT} });
  var start = Date.now();
  /* (the page as it was opened: it may change its URL) */
  var opened = (location.pathname + location.search).slice(1);
  var TEST = ['PASS', 'FAIL', 'TIMEOUT', 'NOTRUN', 'PRECONDITION_FAILED'];
  var HARNESS = ['OK', 'ERROR', 'TIMEOUT', 'PRECONDITION_FAILED'];
  add_completion_callback(function (tests, status) {
    var r = { page: opened, status: HARNESS[status.status] || 'ERROR',
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
const stalledResumes = new Set();
const echoHeaders = new Map();
const renderBlockKeys = new Set();
const dispatcherQueues = new Map();
const REMOTE_PORT_OFFSET = 200;
function substitute(s, url) {
    const port = url.port, origin = url.origin;
    /* (a page on the second origin's port: the first is the one below it) */
    const base = +port >= +process.argv[3] + REMOTE_PORT_OFFSET ? +port - REMOTE_PORT_OFFSET : +port;
    /* (get-host-info's REMOTE_HOST is www1.<host>: no name for an address.
     * Its origin is this host on a second port; no TLS here, so https is http) */
    if (/function get_host_info/.test(s)) {
        s = s.replace(/'www1\.' \+ ORIGINAL_HOST/g, 'ORIGINAL_HOST')
             .replace(/'https:\/\/(foo:bar@)?' \+/g, "'http://$1' +")
             .replace(/(\+ (?:REMOTE_HOST|NOTSAMESITE_HOST|OTHER_NOTSAMESITE_HOST|OTHER_HOST) \+ )[A-Z_]*PORT[A-Z_0-9]*ELIDED/g, "$1':" + (base + REMOTE_PORT_OFFSET) + "'");
    } else {
        s = s.replace(/'www1\.' \+ ORIGINAL_HOST/g, 'ORIGINAL_HOST');
    }
    return s.replace(/\{\{([^}]*)\}\}/g, (all, expr) => {
        expr = expr.trim();
        if (/^host$|^domains\[[^\]]*\]$|^hosts\[[^\]]*\]\[[^\]]*\]$/.test(expr)) return '127.0.0.1';
        if (/^ports\[[^\]]*\]\[0\]$/.test(expr)) return String(base);
        if (/^ports\[[^\]]*\]\[\d+\]$/.test(expr)) return String(base + REMOTE_PORT_OFFSET);
        if (expr === 'location[host]') return '127.0.0.1:' + port;
        if (expr === 'location[hostname]') return '127.0.0.1';
        if (expr === 'location[port]') return port;
        if (expr === 'location[scheme]') return 'http';
        if (expr === 'location[path]' || expr === 'location[pathname]') return url.pathname;
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
const queryStash = new Map();
/* (the script element's load-error-events.py: a script that notes it ran, a 404 for _error; the module kinds import their resource) */
function loadErrorEventsPy(kind) {
    return url => {
        const t = url.searchParams.get('test') || '';
        if (!/^[a-zA-Z0-9_]+$/.test(t)) return [400, 'text/plain', ''];
        const ok = t.indexOf('_load') >= 0;
        const fail = t + '.test.step(function() { assert_unreached("404 script should not be executed"); });';
        if (!kind) return ok ? ['text/javascript', '"use strict"; ' + t + '.executed = true;'] : [404, 'text/javascript', '"use strict"; ' + fail];
        const res = { json: ['./module.json', './not_found.json'], text: ['./file.txt', './not_found.txt'], css: ['./basic.css', './not_found.css'] }[kind];
        return ['text/javascript', 'import "' + (ok ? res[0] : res[1]) + '" with { type: "' + kind + '" }; ' + (ok ? t + '.executed = true;' : fail)];
    };
}
/* (serve-text-then-js.py and its reverse: the first answer for a key one kind, the rest the other) */
const thenCounts = new Map();
function servesThen(firstText) {
    return url => {
        const k = url.searchParams.get('key');
        if (!k) return [400, 'text/plain', 'Not enough parameters'];
        const n = thenCounts.get(k) || 0;
        thenCounts.set(k, n + 1);
        const text = (n === 0) === firstText;
        return text ? ['text/plain', 'hello'] : ['application/javascript', "export default 'world';"];
    };
}
const cookieOf = (req, name) => { const m = new RegExp('(?:^|;\\s*)' + name + '=([^;]*)').exec(req.headers.cookie || ''); return m ? m[1] : null; };
const TM = '/html/semantics/scripting-1/the-script-element/';
const PY = {
    /* (navigating-across-documents/refresh: a Refresh header with bytes past ASCII, as they are) */
    '/html/browsers/browsing-the-web/navigating-across-documents/refresh/resources/refresh.py': () =>
        [200, 'text/plain', 'Not refreshed.\n', { refresh: '0;./refreshed.txt?\u0080\u00FF' }],
    '/html/browsers/browsing-the-web/navigating-across-documents/refresh/resources/refresh-with-svg.py': () =>
        [200, 'image/svg+xml', '<svg version="1.1" xmlns="http://www.w3.org/2000/svg"><text y="14" text-anchor="left" >Not refreshed.</text></svg>\n', { refresh: '0;./refreshed.txt' }],
    [TM + 'text-module/serve-text-then-js.py']: servesThen(true),
    [TM + 'text-module/serve-js-then-text.py']: servesThen(false),
    [TM + 'text-module/http-accept-header-checker.py']: (url, req) => [200, 'text/plain', req.headers.accept || '', { 'access-control-allow-origin': '*' }],
    [TM + 'json-module/http-accept-header-checker.py']: (url, req) => [200, 'application/json', '{"accept": "' + (req.headers.accept || '') + '"}', { 'access-control-allow-origin': '*' }],
    [TM + 'text-module/referrer-checker.py']: (url, req) => [200, 'text/plain', req.headers.referer || '', { 'access-control-allow-origin': '*' }],
    [TM + 'json-module/referrer-checker.py']: (url, req) => [200, 'application/json', '{"referrer": "' + (req.headers.referer || '') + '"}', { 'access-control-allow-origin': '*' }],
    [TM + 'text-module/cross-origin.py']: (url, req) => [200, 'text/plain', cookieOf(req, 'milk') === '1' ? 'request had cookies' : 'request did not have cookies',
        { 'access-control-allow-origin': url.searchParams.get('origin') || '', 'access-control-allow-credentials': 'true' }],
    [TM + 'json-module/cross-origin.py']: (url, req) => [200, 'application/json', '{"requestHadCookies": ' + (cookieOf(req, 'milk') === '1') + '}',
        { 'access-control-allow-origin': url.searchParams.get('origin') || '', 'access-control-allow-credentials': 'true' }],
    '/html/semantics/scripting-1/the-script-element/resources/load-error-events.py': loadErrorEventsPy(null),
    '/html/semantics/scripting-1/the-script-element/json-module/load-error-events.py': loadErrorEventsPy('json'),
    '/html/semantics/scripting-1/the-script-element/text-module/load-error-events.py': loadErrorEventsPy('text'),
    '/html/semantics/scripting-1/the-script-element/css-module/resources/load-error-events.py': loadErrorEventsPy('css'),
    /* a document whose <meta charset> is the label */
    /* a document of this content type, its meta saying another */
    '/dom/nodes/Document-contentType/support/contenttype_setter.py': url => {
        const mime = url.searchParams.get('mime') || url.searchParams.get('mimeHead');
        return [url.searchParams.get('type') + '/' + url.searchParams.get('subtype'),
                '<head>' + (mime ? `<meta http-equiv="Content-Type" content="${escapeHtml(mime)}; charset=utf-8"/>` : '') + '</head>'];
    },
    '/html/infrastructure/urls/terminology-0/resources/echo-referrer-text.py': (url, req) => ['text/plain', req.headers.referer || ''],
    '/dom/nodes/encoding.py': url => ['text/html', `<!doctype html><meta charset="${escapeHtml(url.searchParams.get('label') || '')}">`],
    /* (query-encoding's resources: the raw q echoed as the type asked for) */
    '/html/infrastructure/urls/resolving-urls/query-encoding/resources/resource.py': url => {
        const raw = url.search.slice(1), m = /(?:^|&)q=([^&]+)/.exec(raw), q = m ? m[1] : '';
        const type = url.searchParams.get('type'), enc = url.searchParams.get('encoding') || 'utf-8';
        switch (type) {
        case 'html': return ['text/html; charset=utf-8', q];
        case 'css': return ['text/css; charset=utf-8', "#test::before { content:'" + q + "' }"];
        case 'js': return ['text/javascript; charset=utf-8', url.searchParams.get('var') + " = '" + q + "';"];
        case 'worker': return ['text/javascript', "postMessage('" + q + "'); close();"];
        case 'sharedworker': return ['text/javascript', "onconnect = function(e) { e.source.postMessage('" + q + "'); close(); };"];
        case 'worker_importScripts': return ['text/javascript; charset=' + enc, "try { var x = 'importScripts failed to run'; importScripts('?q=\\u00E5&type=js&var=x&encoding=" + enc + "'); postMessage(x); close(); } catch(ex) { postMessage(String(ex)); }"];
        case 'worker_worker': return ['text/javascript; charset=' + enc, "try { var worker = new Worker('?q=\\u00E5&type=worker&encoding=" + enc + "'); worker.onmessage = function(e) { postMessage(e.data); close(); }; } catch(ex) { postMessage(String(ex)); }"];
        case 'eventstream': return ['text/event-stream', 'data: ' + q + '\n\n'];
        case 'svg': return ['image/svg+xml', "<svg xmlns='http://www.w3.org/2000/svg'>" + q + '</svg>'];
        case 'webvtt': return ['text/vtt', 'WEBVTT\n\n00:00:00.000 --> 00:00:01.000\n' + q];
        case 'png': {
            const img = q === '%E5' || q === '%26%23229%3B' ? 'green-1x1.png' : q === '%C3%A5' ? 'green-2x2.png' : q === '%3F' ? 'green-16x16.png' : 'green-256x256.png';
            try { return ['image/png', fs.readFileSync(path.join(root, 'images', img))]; } catch (e) { return [404, 'text/plain', '']; }
        }
        case 'video': {
            const ext = url.searchParams.get('ext') || 'webm';
            const v = q === '%E5' || q === '%26%23229%3B' ? 'A4' : q === '%C3%A5' ? 'movie_5' : q === '%3F' ? 'green-at-15' : 'movie_300';
            try { return ['video/' + (ext === 'ogv' ? 'ogg' : ext), fs.readFileSync(path.join(root, 'media', v + '.' + ext))]; } catch (e) { return [404, 'text/plain', '']; }
        }
        }
        return [200, 'text/plain', ''];
    },
    '/html/infrastructure/urls/resolving-urls/query-encoding/resources/blank.py': url => ['text/html; charset=' + (url.searchParams.get('encoding') || 'utf-8'), ''],
    '/html/infrastructure/urls/resolving-urls/query-encoding/resources/css-tmpl.py': url =>
        ['text/css; charset=' + (url.searchParams.get('encoding') || 'utf-8'), (url.searchParams.get('tmpl') || '').replace(/%s/g, '\\0000E5')],
    '/html/infrastructure/urls/resolving-urls/query-encoding/resources/stash.py': url => {
        const key = url.searchParams.get('id'), raw = url.search.slice(1), m = /(?:^|&)q=([^&]+)/.exec(raw);
        if (url.searchParams.get('action') === 'put') { queryStash.set(key, m ? m[1] : ''); return ['text/html', 'Put ' + (m ? m[1] : '')]; }
        const v = queryStash.get(key);
        queryStash.delete(key);
        return ['text/html', v === undefined ? '' : v];
    },
    /* (a page with the Refresh header the URL asks for) */
    '/html/semantics/document-metadata/the-meta-element/pragma-directives/attr-meta-http-equiv-refresh/support/refresh.py': url =>
        [200, 'text/html', '<!doctype html>refresh.py\n', { refresh: url.searchParams.get('input') || '' }],
    /* (a file next to the handler, served as the content type asked for; none, a 400) */
    '/html/semantics/scripting-1/the-script-element/serve-with-content-type.py': url => {
        const fn = url.searchParams.get('fn'), ct = url.searchParams.get('ct');
        const dir = path.join(root, 'html/semantics/scripting-1/the-script-element');
        const f = fn && path.resolve(dir, fn);
        if (!fn || ct === null || !f.startsWith(dir) || !fs.existsSync(f)) return [400, 'text/plain', 'Not enough parameters or file not found'];
        /* (its bytes as they are, which Node would refuse: UTF-8 as Latin-1, a control character as '?', still not a token) */
        return [Buffer.from(ct, 'utf8').toString('latin1').replace(/[\x00-\x08\x0a-\x1f\x7f]/g, '?'), fs.readFileSync(f)];
    },
};

const server = http.createServer((req, res) => {
    if (process.env.TRACE) console.log(req.method + ' ' + req.url);
    /* (a page on a port's second origin is that port's browser's) */
    const sock = req.socket.localPort, lport = sock >= +port + REMOTE_PORT_OFFSET ? sock - REMOTE_PORT_OFFSET : sock;
    const url = new URL(req.url, 'http://127.0.0.1:' + sock);
    const current = currentOf[lport] === undefined ? -1 : currentOf[lport];
    /* Page i is opened: the one before it is done, answered or not. */
    const opened = i => {
        if (current >= 0 && current !== i && !results[pages[current].id])
            record({ page: pages[current].id, status: 'crash', pass: 0, fail: 0, skip: 0 });
        currentOf[lport] = i;
        openedAt[lport] = Date.now();
    };
    if (url.pathname === '/end') {
        opened(-1);
        ended.add(lport);
        res.writeHead(200, { 'content-type': 'text/html' });
        res.end('<p>done</p>');
        /* (saved first: the flag lets this browser quit, and then the run ends) */
        if (ended.size >= NPORTS) save();
        fs.writeFileSync(path.join(out, `end-${lport}.flag`), 'end');
        if (ended.size >= NPORTS) setTimeout(() => process.exit(0), 50);
        return;
    }
    const id = (url.pathname + url.search).slice(1);
    const page = pageIndex.has(id) ? pageIndex.get(id) : -1;
    if (page >= 0) opened(page);
    /* (STEP_TIMES=1: each request and when, for where a step's time goes) */
    if (process.env.STEP_TIMES) console.log(`[st] ${Date.now()} ${lport} ${req.method} ${url.pathname}`);
    if (url.pathname === '/result' && req.method === 'POST') {
        let body = '';
        req.on('data', d => body += d);
        req.on('end', () => {
            try {
                const r = JSON.parse(body);
                const cur = currentOf[lport] === undefined ? -1 : currentOf[lport];
                if (cur >= 0 && r.page === pages[cur].id && !results[r.page]) {
                    /* (real time: the page's clock skips what it only waits for) */
                    if (openedAt[lport]) r.ms = Date.now() - openedAt[lport];
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
            stepReady(body);
            if (t_req) console.log(`[trace] ${t_req} refready request, ${((Date.now() * 10000) % 1e9 / 1e4).toFixed(1)} flag written`);
            res.writeHead(204); res.end();
        });
        return;
    }
    /* (wpt serve's alias for the WebIDL parser idlharness.js loads) */
    if (url.pathname === '/resources/WebIDLParser.js') url.pathname = '/resources/webidl2/lib/webidl2.js';
    /* (WPT's is empty: an implementation brings its own) */
    if (url.pathname === '/resources/testdriver-vendor.js') {
        res.writeHead(200, { 'content-type': 'text/javascript' });
        return res.end(fs.readFileSync(path.join(__dirname, 'testdriver-vendor.js')));
    }
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
    /* (remote-context-helper's window: its scripts, then its executor) */
    if (url.pathname === '/html/browsers/browsing-the-web/remote-context-helper/resources/executor-window.py') {
        const q = url.searchParams, esc = v => String(v).replace(/&/g, '&amp;').replace(/'/g, '&#x27;').replace(/"/g, '&quot;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
        const scripts = q.getAll('script').map(v => "<script src='" + esc(v) + "'></script>").join('\n');
        const startOn = q.get('startOn');
        const heads = Object.entries(req.headers).map(([k, v]) => 'window.__requestHeaders.append(' + JSON.stringify(k) + ', ' + JSON.stringify(String(v)) + ');').join('\n');
        const href = 'http://' + req.headers.host + req.url;
        res.writeHead(+(q.get('status') || 200), { 'content-type': 'text/html' });
        return res.end('<!DOCTYPE HTML>\n<base href="' + esc(href) + '">\n<script src="/common/dispatcher/dispatcher.js"></script>\n' +
            '<script src="./executor-common.js"></script>\n<script src="./executor-window.js"></script>\n' + scripts + '\n<body>\n<script>\n' +
            'window.__requestHeaders = new Headers();\n' + heads + '\nrequestExecutor("' + q.get('uuid') + '", ' + (startOn ? "'" + startOn + "'" : 'null') + ');\n</script>\n');
    }
    /* (a GET that waits for the POST with its key) */
    if (url.pathname.endsWith('/resources/render-blocking-stylesheet.py')) {
        const key = url.searchParams.get('key');
        if (req.method === 'POST') {
            renderBlockKeys.add(key);
            res.writeHead(200, { 'content-type': 'text/plain' });
            return res.end('done');
        }
        const check = () => {
            if (!renderBlockKeys.delete(key)) return void setTimeout(check, 50);
            res.writeHead(200, { 'content-type': 'text/css' });
            res.end('');
        };
        return check();
    }
    /* (css/fetching: what a request's headers were, by token) */
    if (url.pathname === '/css/fetching/support/echo-headers.py') {
        const token = url.searchParams.get('token'), location = url.searchParams.get('location');
        const store = echoHeaders.get(token) || {};
        if (location === 'echo') {
            res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-store' });
            return res.end(JSON.stringify(store));
        }
        store[location] = req.headers;
        echoHeaders.set(token, store);
        res.writeHead(302, { location, 'cache-control': 'no-store' });
        return res.end();
    }
    /* (the link element's resources/css.py: a sheet, with the Content-Type asked for (none by default), nosniff and CORS as asked) */
    if (url.pathname === '/html/semantics/document-metadata/the-link-element/resources/css.py') {
        const h = {};
        if (url.searchParams.has('content_type')) h['content-type'] = url.searchParams.get('content_type');
        if (url.searchParams.has('nosniff')) h['x-content-type-options'] = 'nosniff';
        if (url.searchParams.has('cors')) h['access-control-allow-origin'] = '*';
        try { res.writeHead(200, h); } catch (e) { res.writeHead(200); }
        return res.end('body { background:red }');
    }
    if (url.pathname === '/css/fetching/support/echo-css.py') {
        res.writeHead(200, { 'content-type': 'text/css' });
        return res.end(url.searchParams.get('content') || '');
    }
    if (url.pathname === '/common/redirect.py') {
        res.writeHead(+(url.searchParams.get('status') || 302), { location: url.searchParams.get('location') });
        return res.end();
    }
    /* (common/dispatcher/dispatcher.py: queues by uuid; POST adds, GET takes) */
    if (url.pathname === '/common/dispatcher/dispatcher.py') {
        const uuid = url.searchParams.get('uuid');
        const cors = { 'access-control-allow-origin': req.headers.origin || '*', 'access-control-allow-credentials': 'true',
                       'access-control-allow-methods': 'OPTIONS, GET, POST', 'access-control-allow-headers': 'Content-Type',
                       'cache-control': url.searchParams.has('cacheable') ? 'max-age=31536000' : 'no-cache, no-store, must-revalidate' };
        if (req.method === 'OPTIONS') { res.writeHead(200, cors); return res.end(); }
        const queue = dispatcherQueues.get(uuid) || [];
        dispatcherQueues.set(uuid, queue);
        if (req.method === 'POST') {
            let body = '';
            req.on('data', d => body += d);
            req.on('end', () => { queue.push(body); res.writeHead(200, cors); res.end('done'); });
            return;
        }
        res.writeHead(200, cors);
        return res.end(queue.length ? queue.shift() : 'not ready');
    }
    /* (the script element's serve-custom-response.py: by key, a response set ahead (status, type, network error), counted) */
    if (url.pathname.endsWith('/the-script-element/serve-custom-response.py')) {
        global.__customResp = global.__customResp || new Map();
        const st = global.__customResp, key = url.searchParams.get('key') || '', action = url.searchParams.get('action');
        const c = st.get(key) || { net: false, count: 0, code: 200, type: 'application/javascript' };
        st.set(key, c);
        if (action === 'stat') { res.writeHead(200, { 'content-type': 'text/plain' }); return res.end(String(c.count)); }
        if (action === 'reset-stat') { c.count = 0; res.writeHead(200, { 'content-type': 'text/plain' }); return res.end('OK'); }
        if (action === 'set') {
            if (url.searchParams.has('code')) c.code = +url.searchParams.get('code');
            if (url.searchParams.has('content-type')) c.type = url.searchParams.get('content-type');
            if (url.searchParams.has('network-error')) c.net = url.searchParams.get('network-error').toLowerCase() === 'true';
            res.writeHead(200, { 'content-type': 'text/plain' }); return res.end('OK');
        }
        c.count++;
        if (c.net) { req.socket.destroy(); return; }
        const body = c.type === 'application/javascript' ? "export default 'hello';" : c.type === 'application/json' ? '{"hello": "world"}' :
                     c.type === 'text/css' ? '#test { background-color: rgb(0, 255, 0); }' : '42';
        res.writeHead(c.code, { 'content-type': c.type, 'cache-control': 'no-store' });
        return res.end(body);
    }
    /* (script-element moving-between-documents: the iframe page in two chunks, a second apart; the slow script and sheet) */
    if (url.pathname.endsWith('/moving-between-documents/resources/moving-between-documents-iframe.py')) {
        const q = k => url.searchParams.get(k);
        const ioe = q('inlineOrExternal') || 'null', block = (q('hasBlockingStylesheet') || 'true') === 'true';
        const result = q('result') || 'success', type = (q('type') || 'classic') === 'classic' ? 'text/javascript' : 'module';
        res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
        let body = '<!DOCTYPE html>\n<head>\n<script>\nparent.postMessage("fox", "*");\n</script>\n';
        if (block) body += '<link rel="stylesheet" href="slow-flag-setter.py?result=css&cache=' + Math.random() + '">\n';
        body += '</head>\n<body>\n';
        if (ioe === 'inline' || ioe === 'external' || ioe === 'empty-src') body += '<streaming-element>\n';
        body += 'A'.repeat(100000);
        res.write(body);
        setTimeout(() => {
            let b2 = '';
            if (ioe === 'inline') {
                b2 += '<script id="s1" type="' + type + '" onload="scriptOnLoad()" onerror="scriptOnError(event)">\n' +
                      'if (!window.readyToEvaluate) { window.didExecute = "executed too early"; } else { window.didExecute = "executed"; }\n';
                if (result === 'parse-error') b2 += '1=2 parse error\n';
                b2 += '</script>\n</streaming-element>\n';
            } else if (ioe === 'external') {
                b2 += '<script id="s1" type="' + type + '" src="slow-flag-setter.py?result=' + result + '&cache=' + Math.random() +
                      '" onload="scriptOnLoad()" onerror="scriptOnError(event)"></script>\n</streaming-element>\n';
            } else if (ioe === 'empty-src') {
                b2 += '<script id="s1" type="' + type + '" src="" onload="scriptOnLoad()" onerror="scriptOnError(event)"></script>\n</streaming-element>\n';
            }
            b2 += 'B'.repeat(100000);
            res.end(b2);
        }, ioe === 'null' ? 0 : 1000);
        return;
    }
    /* (the script element's log.py: log('<id>') after sec seconds; script-not-found-not-executed*.py, cacheable-script-throw.py) */
    if (url.pathname.endsWith('/scripting-1/the-script-element/log.py')) {
        const id = parseInt(url.searchParams.get('id'), 10), sec = parseInt(url.searchParams.get('sec'), 10);
        if (!isFinite(id) || !isFinite(sec)) { res.writeHead(400, { 'content-type': 'text/plain' }); return res.end('Invalid parameter'); }
        setTimeout(() => { res.writeHead(200, { 'content-type': 'text/javascript' }); res.end("log('" + id + "')"); }, sec * 1000);
        return;
    }
    if (/\/scripting-1\/the-script-element\/script-not-found-not-executed(-2)?\.py$/.test(url.pathname)) {
        const two = /-2\.py$/.test(url.pathname);
        res.writeHead(two ? 200 : 404, { 'content-type': 'text/javascript' });
        return res.end(two ? 'test2_token = "script executed";' : 'test1_token = "script executed";');
    }
    if (url.pathname.endsWith('/scripting-1/the-script-element/cacheable-script-throw.py')) {
        res.writeHead(200, { 'content-type': 'text/javascript', 'cache-control': 'public, max-age=100' });
        return res.end("throw('fox');");
    }
    if (url.pathname.endsWith('/moving-between-documents/resources/slow-flag-setter.py')) {
        const result = url.searchParams.get('result') || 'success';
        if (result === 'css') { setTimeout(() => { res.writeHead(200, { 'content-type': 'text/css' }); res.end(''); }, 3000); return; }
        setTimeout(() => {
            if (result === 'fetch-error') { res.writeHead(404, { 'content-type': 'text/plain' }); return res.end('window.didExecute = "fetch error";'); }
            res.writeHead(200, { 'content-type': 'text/javascript' });
            res.end(result === 'parse-error' ? '1=2 parse error;' :
                "fetch('exec');\nconsole.log('exec');\nif (!window.readyToEvaluate) { window.didExecute = \"executed too early\"; } else { window.didExecute = \"executed\"; }\n");
        }, 2000);
        return;
    }
    /* (track CORS's cors-tester.py: each request's Origin and cookie noted by id; CORS headers, redirects; read hands them back) */
    if (url.pathname.endsWith('/track-element/cors/support/cors-tester.py')) {
        global.__corsStash = global.__corsStash || new Map();
        const st = global.__corsStash, q = k => url.searchParams.get(k), id = q('id');
        if (req.method !== 'GET' || !id) { res.writeHead(400); return res.end('bad'); }
        if (url.searchParams.has('read')) {
            const d = st.get(id); st.delete(id);
            if (d === undefined) { res.writeHead(404, { 'content-type': 'text/plain' }); return res.end('Tried to read data not yet set'); }
            res.writeHead(200, { 'content-type': 'text/plain' }); return res.end(d);
        }
        if (url.searchParams.has('cleanup')) { st.delete(id); res.writeHead(200); return res.end('OK'); }
        if (url.searchParams.has('delete-cookie')) { res.writeHead(200, { 'content-type': 'text/plain', 'set-cookie': id + '=; Max-Age=0; Path=/' }); return res.end('OK'); }
        const h = { 'content-type': 'text/vtt' };
        if (q('origin') !== null) { h['access-control-allow-origin'] = q('origin'); h['access-control-allow-credentials'] = 'true'; }
        const cors = req.headers.origin || 'no';
        let cookie = 'no';
        (req.headers.cookie || '').split(/;\s*/).forEach(c => { const i = c.indexOf('='); if (i > 0 && c.slice(0, i) === id) cookie = c.slice(i + 1); });
        let line = 'cors = ' + cors + ' | cookie = ' + cookie;
        if (st.has(id)) line = st.get(id) + '\n' + line;
        st.set(id, line);
        if (q('redirect') !== null) { h.location = q('redirect'); res.writeHead(302, h); return res.end(); }
        res.writeHead(200, h);
        return res.end('WEBVTT\n\n00:00:00.000 --> 00:00:10.000\nTest');
    }
    /* (common/slow.py: nothing, after ?delay= ms, 2000 by default) */
    if (url.pathname === '/common/slow.py') {
        const ms = parseFloat(url.searchParams.get('delay') || '2000');
        setTimeout(() => { res.writeHead(200); res.end(); }, isFinite(ms) ? ms : 2000);
        return;
    }
    /* (the iframe tests' stash.py: a POST puts its body under id, a GET takes it, once) */
    if (url.pathname === '/html/semantics/embedded-content/the-iframe-element/stash.py') {
        global.__ifStash = global.__ifStash || new Map();
        const st = global.__ifStash, id = url.searchParams.get('id') || '';
        if (req.method === 'POST') {
            const chunks = [];
            req.on('data', d => chunks.push(d));
            req.on('end', () => { st.set(id, Buffer.concat(chunks)); res.writeHead(200, { 'content-type': 'text/plain', 'access-control-allow-origin': '*' }); res.end(''); });
            return;
        }
        const v = st.get(id);
        st.delete(id);
        res.writeHead(200, { 'content-type': 'text/plain', 'cache-control': 'no-store', 'access-control-allow-origin': '*' });
        return res.end(v || '');
    }
    /* (speculative-parsing's stash.py: put keeps the request's Origin, Accept and Referer lines, take hands them back once) */
    if (url.pathname === '/html/syntax/speculative-parsing/resources/stash.py') {
        global.__specStash = global.__specStash || new Map();
        const st = global.__specStash, uuid = url.searchParams.get('uuid') || '';
        res.writeHead(200, { 'content-type': 'text/plain', 'cache-control': 'no-store' });
        if (url.searchParams.get('action') === 'put') {
            const q = url.search.slice(1), at = q.indexOf('&encodingcheck=');
            let v = 'param-encodingcheck: ' + (at >= 0 ? q.slice(at + 15) : '') + '\r\n';
            const lines = [];
            for (let i = 0; i + 1 < req.rawHeaders.length; i += 2)
                if (['Origin', 'Accept', 'Referer'].includes(req.rawHeaders[i])) lines.push(req.rawHeaders[i] + ': ' + req.rawHeaders[i + 1]);
            st.set(uuid, v + lines.join('\r\n'));
            return res.end('');
        }
        const v = st.get(uuid) || '';
        st.delete(uuid);
        return res.end(v);
    }
    /* (forms' form-submission.py: OK when the body is what the query says it should be) */
    if (url.pathname.endsWith('/form-submission-0/resources/form-submission.py')) {
        const chunks = [];
        req.on('data', d => chunks.push(d));
        req.on('end', () => {
            const b = Buffer.concat(chunks), ct = String(req.headers['content-type'] || '');
            let ok = false;
            if (url.searchParams.get('query') === '1') {
                if (ct === 'application/x-www-form-urlencoded') ok = b.toString('latin1') === 'foo=bara';
                else if (ct === 'text/plain') ok = b.toString('latin1') === 'qux=baz\r\n';
                else {
                    const m = /boundary=("?)([^";]+)\1/.exec(ct);
                    if (m) {
                        for (const part of b.toString('latin1').split('--' + m[2])) {
                            const i = part.indexOf('\r\n\r\n');
                            if (i < 0) continue;
                            if (/name="foo"/.test(part.slice(0, i))) { ok = part.slice(i + 4).replace(/\r\n$/, '') === 'bar'; break; }
                        }
                    }
                }
            } else if (url.searchParams.get('expected_body') !== null) ok = b.toString('utf8') === url.searchParams.get('expected_body');
            res.writeHead(200, { 'content-type': 'text/plain' });
            res.end(ok ? 'OK' : 'FAIL');
        });
        return;
    }
    /* (forms' form-echo.py: the request body's bytes in hex, space-separated, as text/plain) */
    if (url.pathname.endsWith('/form-submission-0/form-echo.py')) {
        const chunks = [];
        req.on('data', d => chunks.push(d));
        req.on('end', () => {
            const b = Buffer.concat(chunks);
            res.writeHead(200, { 'content-type': 'text/plain' });
            res.end(Array.from(b, c => c.toString(16).padStart(2, '0')).join(' '));
        });
        return;
    }
    /* (FileAPI's echo-content-escaped.py: the request body, its control bytes and backslashes escaped) */
    if (url.pathname === '/FileAPI/file/resources/echo-content-escaped.py') {
        const chunks = [];
        req.on('data', d => chunks.push(d));
        req.on('end', () => {
            const b = Buffer.concat(chunks);
            let out = '';
            for (const c of b) out += c <= 0x1f || c >= 0x7f ? '\\x' + c.toString(16).padStart(2, '0') : c === 0x5c ? '\\\\' : String.fromCharCode(c);
            out = out.split('\\x0d\\x0a').join('\r\n');
            res.writeHead(200, { 'content-type': 'text/plain; charset=utf-8', 'x-request-method': req.method,
                                 'x-request-content-length': req.headers['content-length'] || 'NO',
                                 'x-request-content-type': req.headers['content-type'] || 'NO' });
            res.end(Buffer.from(out, 'latin1'));
        });
        return;
    }
    /* (support/stall-resume.py: the start of a video, then the rest once a POST says so) */
    if (url.pathname === '/css/selectors/media/support/stall-resume.py') {
        const key = url.searchParams.get('key');
        if (req.method === 'POST') {
            stalledResumes.add(key);
            setTimeout(() => stalledResumes.delete(key), 5000);
            return res.end('put ' + key + ' into stash');
        }
        const video = fs.readFileSync(path.join(root, 'media/movie_300.webm'));
        res.writeHead(200, { 'content-type': 'video/webm', 'content-length': video.length });
        res.write(video.subarray(0, 4096));
        const deadline = Date.now() + 30000;
        const wait = setInterval(() => {
            if (Date.now() > deadline) { clearInterval(wait); return res.destroy(); }
            if (!stalledResumes.has(key)) return;
            clearInterval(wait);
            res.end(video.subarray(4096));
        }, 100);
        res.on('close', () => clearInterval(wait));
        return;
    }
    /* (css-values/urls/support/*-referrer-policy.py: one thing when the Referer is the expected one, another when not) */
    const rp = /^\/css\/css-values\/urls\/support\/(image|font|stylesheet|svg-filter)-referrer-policy\.py$/.exec(url.pathname);
    if (rp) {
        const q = url.searchParams, want = q.get('expected_referrer'), got = req.headers.referer || '';
        const match = want === 'none' ? got === '' : want === 'origin' ? got === q.get('origin') : want === 'url' ? got === q.get('url') : null;
        const h = { 'cache-control': 'no-cache; must-revalidate' };
        let body;
        if (rp[1] === 'image') {
            body = fs.readFileSync(path.join(root, 'css/css-values/urls/support', match === null ? '1x1-navy.png' : match ? '1x1-green.png' : '1x1-red.png'));
            h['content-type'] = 'image/png';
            if (q.has('corp')) h['cross-origin-resource-policy'] = q.get('corp');
            if (q.has('acao')) h['access-control-allow-origin'] = q.get('acao');
        } else {
            h['access-control-allow-origin'] = '*';
            if (rp[1] === 'font') {
                body = match ? fs.readFileSync(path.join(root, 'fonts/Ahem.ttf')) : Buffer.alloc(0);
                h['content-type'] = match ? 'font/truetype' : 'text/plain';
            } else if (rp[1] === 'stylesheet') {
                body = Buffer.from(match ? '#test { color: green; }' : '#test { color: red; }');
                h['content-type'] = 'text/css';
            } else {
                body = Buffer.from(match ? '<svg width="0" height="0" xmlns="http://www.w3.org/2000/svg" version="1.1"><defs><filter id="MyFilter" color-interpolation-filters="sRGB"><feColorMatrix type="hueRotate" values="90"/></filter></defs></svg>' : '');
                h['content-type'] = 'image/svg+xml';
            }
        }
        h['content-length'] = body.length;
        res.writeHead(200, h);
        return res.end(body);
    }
    if (PY[url.pathname]) {
        const r = PY[url.pathname](url, req);
        const [status, type, body, extra] = r.length >= 3 ? r : [200, r[0], r[1]];
        try { res.writeHead(status, Object.assign({ 'content-type': type }, extra || {})); } catch (e) { res.writeHead(500); }
        return res.end(body);
    }
    let decoded_path;
    try { decoded_path = decodeURIComponent(url.pathname); } catch (e) { res.writeHead(400); return res.end(); }
    const file = path.join(root, decoded_path);
    const cached = FILE_CACHE.get(file);
    if (cached) return serveFile(res, url, file, cached);
    /* (read off this thread, as the flag is written; a directory, or nothing there: the read fails) */
    fs.readFile(file.startsWith(root) ? file : path.join(root, '\0'), (err, raw) => {
        if (err) {
            /* (a reftest's page that is not there will never say it is ready) */
            if (REFTEST) stepReady(url.pathname + url.search);
            res.writeHead(404); return res.end();
        }
        const h = { 'content-type': types[path.extname(file).toLowerCase()] || 'application/octet-stream' };
        for (const hf of [file + '.headers', path.join(path.dirname(file), '__dir__.headers')]) {
            if (!fs.existsSync(hf)) continue;
            for (const line of fs.readFileSync(hf, 'utf8').split(/\r?\n/)) {
                const i = line.indexOf(':');
                if (i > 0) h[line.slice(0, i).trim().toLowerCase()] = line.slice(i + 1).trim();
            }
            break;
        }
        const hit = { raw, h };
        if (raw.length <= (1 << 20) && fileCacheBytes < (768 << 20) && !FILE_CACHE.has(file)) { FILE_CACHE.set(file, hit); fileCacheBytes += raw.length; }
        try { serveFile(res, url, file, hit); } catch (e) { console.log('serving ' + file + ': ' + e); res.destroy(); }
    });
});
function serveFile(res, url, file, hit) {
    let data = hit.raw;
    if (/\.sub\.[^/]*$/.test(file)) data = Buffer.from(substitute(data.toString('utf8'), url), 'utf8');
    /* (?pipe=sub: substituted as a .sub file is) */
    else if ((url.searchParams.get('pipe') || '').split('|').some(p => /^sub(\([^)]*\))?$/.test(p.trim()))) data = Buffer.from(substitute(data.toString('utf8'), url), 'utf8');
    const headers = Object.assign({}, hit.h);
    if (REFTEST && /^(text\/html|application\/xhtml\+xml)/.test(headers['content-type']) && STEP_PATHS.has(url.pathname)) {
        /* (inside the root element, so XHTML stays well-formed) */
        /* (found in the text itself: lower-casing can change its length, 'İ' -> 'i̇') */
        const text = data.toString('latin1'), ends = [...text.matchAll(/<\/html>/gi)];   /* (bytes as latin1 both ways: another encoding's kept) */
        let end = ends.length ? ends[ends.length - 1].index : -1;
        /* (in the head when it has one: the body's children as the page wrote them, :last-child and the like) */
        /* (a tag of the page's own: outside any quoted attribute value, a data: URL's markup not counted) */
        const tagAt = re => {
            let q = '', inTag = false;
            for (let k = 0; k < text.length; k++) {
                const ch = text[k];
                if (q) { if (ch === q) q = ''; continue; }
                if (inTag) { if (ch === '"' || ch === "'") q = ch; else if (ch === '>') inTag = false; continue; }
                if (ch === '<') {
                    if (re.test(text.slice(k, k + 8))) return k;
                    if (text.startsWith('<script', k) || text.startsWith('<style', k)) {   /* (their content: text, not tags) */
                        const close = text.toLowerCase().indexOf(text.startsWith('<script', k) ? '</script' : '</style', k + 1);
                        if (close < 0) return -1;
                        k = close;
                        continue;
                    }
                    if (text.startsWith('<!--', k)) { const c = text.indexOf('-->', k + 4); if (c < 0) return -1; k = c + 2; continue; }
                    inTag = true;
                }
            }
            return -1;
        };
        const hd = tagAt(/^<\/head\s*>/i), bd = hd >= 0 ? -1 : tagAt(/^<body[\s>]/i);
        if (hd >= 0) end = hd;
        else if (bd >= 0) end = bd;
        /* (no end tag, the file ending in a tag left open ('</html' and nothing after): before that) */
        if (end < 0 && text.lastIndexOf('<') > text.lastIndexOf('>')) end = text.lastIndexOf('<');
        /* (a raw-text element left open to the end, an <iframe> with no end tag: before it, not inside its text) */
        if (end < 0) {
            const lo = text.toLowerCase();
            for (const t of ['iframe', 'textarea', 'xmp', 'noembed', 'noframes', 'title', 'style']) {
                const o = lo.lastIndexOf('<' + t);
                if (o >= 0 && /[\s>\/]/.test(lo[o + t.length + 1] || '') && lo.indexOf('</' + t, o) < 0 && (end < 0 || o < end)) end = o;
            }
        }
        /* (a <meta> after it within the first kilobyte: the script at the end, not pushing that meta out of the
         * encoding prescan's reach) */
        if (end >= 0 && end < 1024 && /<meta[\s>]/i.test(text.slice(end, 1030))) end = -1;
        /* (a print reftest and its references: laid out as pages) */
        const rdy = PRINT_URLS.has(url.pathname) || url.searchParams.has('cyberprint') ? REFREADY.replace('/*PRINT*/', 'if (window.__cyberPrintBegin) window.__cyberPrintBegin();') : REFREADY;
        data = Buffer.from(end < 0 ? text + rdy : text.slice(0, end) + rdy + text.slice(end), 'latin1');
    } else if (REFTEST && /^(application|text)\/(xml|[\w.+-]*\+xml)/.test(headers['content-type']) && STEP_PATHS.has(url.pathname)) {
        /* (other XML, SVG among it: an XHTML script before the root element's end tag says when it's ready) */
        const text = data.toString('latin1'), ends = [...text.matchAll(/<\/[\w:.-]+\s*>/g)];
        const end = ends.length ? ends[ends.length - 1].index : -1;
        const tag = REFREADY.replace('<script ', '<script xmlns="http://www.w3.org/1999/xhtml" ');
        if (end >= 0) data = Buffer.from(text.slice(0, end) + tag + text.slice(end), 'latin1');
    }
    /* (?pipe=header(Name,Value)|header(...): response headers the URL asks for) */
    const pipe = url.searchParams.get('pipe') || '';
    let rest = pipe;
    /* (wptserve's pipe syntax: name(arg,arg), a backslash escaping the next character, so ',' and ')' can be in a value) */
    {
        let i = 0, kept = '';
        while (i < pipe.length) {
            const m = /^\s*header\(/.exec(pipe.slice(i));
            if (!m) { kept += pipe[i]; i++; continue; }
            let j = i + m[0].length, args = [''], ok = false;
            for (; j < pipe.length; j++) {
                const c = pipe[j];
                if (c === '\\' && j + 1 < pipe.length) { args[args.length - 1] += pipe[++j]; continue; }
                if (c === ',' && args.length < 3) { args.push(''); continue; }
                if (c === ')') { ok = true; j++; break; }
                args[args.length - 1] += c;
            }
            if (!ok || args.length < 2) { kept += pipe.slice(i); break; }
            headers[args[0].trim().toLowerCase()] = args[1].trim();
            i = j;
        }
        rest = kept;
    }
    /* (a pipe that isn't all header()s, or a header value Node won't send: a server error, as wptserve fails on it) */
    if (/header\(/.test(rest)) { res.writeHead(500); res.end(); return; }
    try { res.writeHead(200, headers); }
    catch (e) {
        /* (a value with a line break in it: what comes before it, as the raw header line wptserve writes would be read) */
        const clean = {};
        let broke = false;
        for (const k in headers) { const v = String(headers[k]); if (/[\r\n]/.test(v)) broke = true; clean[k] = v.split(/[\r\n]/)[0]; }
        /* (what Node won't send: other characters as their UTF-8 bytes, as wptserve writes them; a control byte a '"', which
         * keeps a MIME type as unparsable as the byte made it) */
        if (!broke) for (const k in clean) clean[k] = Buffer.from(clean[k], 'utf8').toString('latin1').replace(/[^\t\x20-\x7e\x80-\xff]/g, '"');
        try { res.writeHead(200, clean); }
        catch (e3) { try { res.writeHead(500); } catch (e2) { } res.end(); return; }
    }
    /* (?pipe=trickle(...): its d<seconds> delays, all of them before the body, as a slow response) */
    const tm = /(?:^|\|)\s*trickle\(([^)]*)\)/.exec(pipe);
    if (tm) {
        let secs = 0;
        for (const part of tm[1].split(':')) { const d = /^d([\d.]+)$/.exec(part.trim()); if (d) secs += Number(d[1]); }
        if (secs > 0) { setTimeout(() => { try { res.end(data); } catch (e) { } }, Math.min(secs, 30) * 1000); return; }
    }
    res.end(data);
}
/* (one request's handler throwing: logged, the server keeps serving the rest of the run) */
process.on('uncaughtException', (e) => { console.error('wpt_server: request failed:', e && e.stack || e); });
for (let i = 0; i < NPORTS; i++) {
    const s = i === 0 ? server : http.createServer(server.listeners('request')[0]);
    s.listen(+port + i, '127.0.0.1');
    /* (each port's second origin) */
    http.createServer(server.listeners('request')[0]).listen(+port + i + REMOTE_PORT_OFFSET, '127.0.0.1');
}

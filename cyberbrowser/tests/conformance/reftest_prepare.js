// Makes the browsers of a REFTEST=1 run (wpt_test.sh) ready: each of n
// takes every n-th page of pages.txt, from <build>/shard-N (kept from run to
// run: its copy of the browser's programs is made again only when the
// build's is newer; not hard links, whose one set of permissions the
// sandboxes of all the shards would share), with its own browser_data and
// logs, and its UI script (script-N.txt) and pages (pages-N.txt).
// With a port (testharness pages, wpt_server.js on PORTS=n ports): shard N's
// pages from port + N, each waited for until its answer (or its time and
// 5 s more), and /end on that port at the end.
//
//   node reftest_prepare.js <build dir> <results dir> <results dir, Windows form> <n> [port]
const fs = require('fs'), path = require('path');
const [build, out, W, nArg, portArg] = process.argv.slice(2);
const n = +nArg, js = !!portArg;
const lines = fs.readFileSync(path.join(out, 'pages.txt'), 'utf8').split('\n').filter(Boolean);
const exes = fs.readdirSync(build).filter(f => /^cb_.*\.exe$/.test(f) && !/^cb_(harness|webgpu_fuzz)\.exe$/.test(f));
/* Shard of each page: every n-th, or with times from an earlier run (testharness
 * pages), the longest first to the shard with the least so far: a run lasts as
 * long as its slowest shard. */
let times = null;
if (js) {
    try { times = JSON.parse(fs.readFileSync(path.join(path.dirname(path.resolve(out)), '.wpt-page-ms.json'), 'utf8')); }
    catch (e) { /* (none yet) */ }
}
const shardOf = new Map();
if (times) {
    const load = new Array(n).fill(0);
    const est = l => { const id = l.split(' ')[1].replace(/^http:\/\/[^/]+\//, ''); return (times[id] || 0) + 25; };
    lines.map(l => [l, est(l)]).sort((a, b) => b[1] - a[1]).forEach(([l, t]) => {
        let best = 0;
        for (let i = 1; i < n; i++) if (load[i] < load[best]) best = i;
        load[best] += t;
        shardOf.set(l, best);
    });
} else {
    lines.forEach((l, j) => shardOf.set(l, (j + 1) % n));
}
for (let i = 0; i < n; i++) {
    const mine = lines.filter(l => shardOf.get(l) === i);
    if (!mine.length) continue;
    const d = path.join(build, `shard-${i}`);
    fs.mkdirSync(d, { recursive: true });
    for (const e of exes) {
        const from = path.join(build, e), to = path.join(d, e), st = fs.statSync(from);
        let have = null;
        try { have = fs.statSync(to); } catch (x) { /* not there yet */ }
        if (!have || st.mtimeMs > have.mtimeMs) {
            fs.copyFileSync(from, to);
            fs.utimesSync(to, st.atime, st.mtime);
        }
    }
    fs.rmSync(path.join(d, 'browser_data'), { recursive: true, force: true });
    fs.rmSync(path.join(d, 'logs'), { recursive: true, force: true });
    fs.mkdirSync(path.join(d, 'logs'));
    let script = js ? 'open about:blank\n' : 'open about:blank\nviewport 800 600\n';
    const origin = js ? `http://127.0.0.1:${+portArg + i}` : null;
    for (const l of mine) {
        const [k, url, ms, shot] = l.split(' ');
        if (js) script += `go ${url.replace(/^http:\/\/[^/]+/, origin)}\nwaitfile ${W}/step-${k}.flag ${+ms + 5000}\n`;
        else script += `go ${url}\nwaitfile ${W}/step-${k}.flag ${ms}\nshotc ${shot}\n`;
    }
    if (js) script += `go ${origin}/end\nwait 1500\n`;
    fs.writeFileSync(path.join(out, `script-${i}.txt`), script + 'quit\n');
    fs.writeFileSync(path.join(out, `pages-${i}.txt`), mine.join('\n') + '\n');
}

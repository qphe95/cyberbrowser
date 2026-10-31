// Makes the browsers of a REFTEST=1 run (wpt_test.sh) ready: each of n
// takes every n-th page of pages.txt, from <build>/shard-N (kept from run to
// run: its copy of the browser's programs is made again only when the
// build's is newer; not hard links, whose one set of permissions the
// sandboxes of all the shards would share), with its own browser_data and
// logs, and its UI script (script-N.txt) and pages (pages-N.txt).
//
//   node reftest_prepare.js <build dir> <results dir> <results dir, Windows form> <n>
const fs = require('fs'), path = require('path');
const [build, out, W, nArg] = process.argv.slice(2);
const n = +nArg;
const lines = fs.readFileSync(path.join(out, 'pages.txt'), 'utf8').split('\n').filter(Boolean);
const exes = fs.readdirSync(build).filter(f => /^cb_.*\.exe$/.test(f) && !/^cb_(harness|webgpu_fuzz)\.exe$/.test(f));
for (let i = 0; i < n; i++) {
    const mine = lines.filter((l, j) => (j + 1) % n === i);
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
    let script = 'open about:blank\nviewport 800 600\n';
    for (const l of mine) {
        const [k, url, ms, shot] = l.split(' ');
        script += `go ${url}\nwaitfile ${W}/step-${k}.flag ${ms}\nshotc ${shot}\n`;
    }
    fs.writeFileSync(path.join(out, `script-${i}.txt`), script + 'quit\n');
    fs.writeFileSync(path.join(out, `pages-${i}.txt`), mine.join('\n') + '\n');
}

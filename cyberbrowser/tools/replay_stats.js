#!/usr/bin/env node
// Summarize tests/jit/replay.sh's results (JS_PERF_DESIGN.md section 4).
//   node tools/replay_stats.js <results> <setting>...
// Per recording: each setting's median JS-thread cycles (and CPU time), the
// spread of its runs, and its change from the first setting with a 95%
// bootstrap confidence interval for the ratio of medians.  An interval that
// excludes 0 is a change the runs show.  Under the deterministic clock each
// run also reports the calls it made (the work it did): only runs at the
// recording's most common count are compared, and a run whose page took
// another path is set aside and counted.  Counter lines, when present, are
// printed side by side.
'use strict';
const fs = require('fs');

const [resultsPath, ...settings] = process.argv.slice(2);
const runs = new Map(), counters = new Map();
for (const line of fs.readFileSync(resultsPath, 'utf8').split(/\r?\n/)) {
    const f = line.trim().split(/\s+/);
    if (f[0] === 'run' && f[4] !== 'NA') {
        const key = f[1] + '\t' + f[2];
        if (!runs.has(key)) runs.set(key, []);
        runs.get(key).push({ cycles: +f[4], cpu: +f[5], nodes: f[6], calls: f[7] || 'NA' });
    } else if (f[0] === 'counter') {   // counter <recording> <setting> <name> <value>
        const key = f[1] + '\t' + f[2];
        if (!counters.has(key)) counters.set(key, new Map());
        counters.get(key).set(f[3], f[4]);
    }
}

const median = a => {
    const s = [...a].sort((x, y) => x - y), n = s.length;
    return n % 2 ? s[(n - 1) / 2] : (s[n / 2 - 1] + s[n / 2]) / 2;
};
// A deterministic generator, so the intervals do not change between calls.
let seed = 12345;
const rand = () => ((seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296);
const resample = a => a.map(() => a[Math.floor(rand() * a.length)]);

// 95% interval of median(b) / median(a) - 1, by resampling each side.
function ratioInterval(a, b, n = 2000) {
    const r = [];
    for (let i = 0; i < n; i++) r.push(median(resample(b)) / median(resample(a)) - 1);
    r.sort((x, y) => x - y);
    return [r[Math.floor(0.025 * n)], r[Math.floor(0.975 * n)]];
}
const pct = x => (x >= 0 ? '+' : '') + (100 * x).toFixed(1) + '%';

const recordings = [...new Set([...runs.keys(), ...counters.keys()].map(k => k.split('\t')[0]))];
for (const rec of recordings) {
    // The most common call count over all of the recording's runs: the
    // runs compared did the same work.
    const tally = new Map();
    for (const [key, rs] of runs)
        if (key.startsWith(rec + '\t'))
            for (const r of rs) tally.set(r.calls, (tally.get(r.calls) || 0) + 1);
    let mode = 'NA', best = 0;
    for (const [c, n] of tally) if (n > best) { mode = c; best = n; }
    let setAside = 0;
    if (mode !== 'NA') {
        for (const [key, rs] of runs) {
            if (!key.startsWith(rec + '\t')) continue;
            const kept = rs.filter(r => r.calls === mode);
            setAside += rs.length - kept.length;
            runs.set(key, kept);
        }
    }
    console.log(rec + (mode === 'NA' ? '' : `  (${mode} calls; ${setAside} run${setAside === 1 ? '' : 's'} at other counts set aside)`));
    const base = (runs.get(rec + '\t' + settings[0]) || []).map(r => r.cycles);
    for (const s of settings) {
        const rs = runs.get(rec + '\t' + s);
        if (!rs || !rs.length) continue;
        const c = rs.map(r => r.cycles), m = median(c);
        const spread = median(c.map(x => Math.abs(x - m))) / m;   // median absolute deviation
        let line = `  ${s.padEnd(28)} ${m.toFixed(0).padStart(7)} Mcycles  cpu ${median(rs.map(r => r.cpu)).toFixed(0).padStart(5)} ms` +
                   `  spread ${(100 * spread).toFixed(1)}%  n=${c.length}`;
        if (s !== settings[0] && base.length) {
            const [lo, hi] = ratioInterval(base, c);
            line += `  change ${pct(m / median(base) - 1)} [${pct(lo)}, ${pct(hi)}]${lo > 0 || hi < 0 ? ' *' : ''}`;
        }
        const nodes = [...new Set(rs.map(r => r.nodes))];
        line += `  nodes ${nodes.length <= 3 ? nodes.join('/') : nodes.length + ' values'}`;
        console.log(line);
    }
}

if (counters.size) {
    for (const rec of recordings) {
        const cols = settings.filter(s => counters.has(rec + '\t' + s));
        if (!cols.length) continue;
        console.log(`\ncounters, ${rec}: ` + cols.join(' | '));
        const names = [...counters.get(rec + '\t' + cols[0]).keys()];
        for (const name of names)
            console.log('  ' + name.padEnd(32) + cols.map(s => String(counters.get(rec + '\t' + s).get(name) || 0).padStart(14)).join(''));
    }
}

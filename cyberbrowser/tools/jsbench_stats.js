#!/usr/bin/env node
// Summarize tests/jit/jsbench.sh's results (JS_PERF_DESIGN.md section 4.3).
//   node tools/jsbench_stats.js <results> <setting>...
// Per test: each setting's median first iteration, steady-state iteration
// (mean of the last half) and total, in ms, and each one's change from the
// first setting with a 95% bootstrap interval for the ratio of medians (*:
// it excludes 0).  Then, over the tests every setting completed, the
// geometric mean of the ratios.  A run that failed or timed out is NA and
// counted.
'use strict';
const fs = require('fs');

const [resultsPath, ...settings] = process.argv.slice(2);
const METRICS = ['first', 'steady', 'total'];
const runs = new Map(), failed = new Map();
for (const line of fs.readFileSync(resultsPath, 'utf8').split(/\r?\n/)) {
    const f = line.trim().split(/\s+/);
    if (f[0] !== 'bench') continue;
    const key = f[1] + '\t' + f[2];
    if (f[4] === 'NA') { failed.set(key, (failed.get(key) || 0) + 1); continue; }
    if (!runs.has(key)) runs.set(key, []);
    runs.get(key).push({ first: +f[4], steady: +f[5], total: +f[6] });
}

const median = a => {
    const s = [...a].sort((x, y) => x - y), n = s.length;
    return n % 2 ? s[(n - 1) / 2] : (s[n / 2 - 1] + s[n / 2]) / 2;
};
let seed = 12345;
const rand = () => ((seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296);
const resample = a => a.map(() => a[Math.floor(rand() * a.length)]);
function ratioInterval(a, b, n = 2000) {
    const r = [];
    for (let i = 0; i < n; i++) r.push(median(resample(b)) / median(resample(a)) - 1);
    r.sort((x, y) => x - y);
    return [r[Math.floor(0.025 * n)], r[Math.floor(0.975 * n)]];
}
const pct = x => (x >= 0 ? '+' : '') + (100 * x).toFixed(1) + '%';
const ms = x => x >= 100 ? x.toFixed(0) : x.toFixed(1);

const tests = [...new Set([...runs.keys(), ...failed.keys()].map(k => k.split('\t')[0]))].sort();
const ratios = new Map(settings.map(s => [s, { first: [], steady: [], total: [] }]));
for (const t of tests) {
    console.log(t);
    const base = runs.get(t + '\t' + settings[0]) || [];
    for (const s of settings) {
        const rs = runs.get(t + '\t' + s) || [];
        const nf = failed.get(t + '\t' + s) || 0;
        let line = '  ' + s.padEnd(24);
        if (!rs.length) { console.log(line + `failed (${nf})`); continue; }
        for (const m of METRICS) {
            const v = median(rs.map(r => r[m]));
            line += `  ${m} ${ms(v).padStart(7)}`;
            if (s !== settings[0] && base.length) {
                const ch = v / median(base.map(r => r[m])) - 1;
                const [lo, hi] = ratioInterval(base.map(r => r[m]), rs.map(r => r[m]));
                line += ` ${pct(ch)}${lo > 0 || hi < 0 ? '*' : ' '}`;
                ratios.get(s)[m].push({ t, r: v / median(base.map(r => r[m])) });
            }
        }
        line += `  n=${rs.length}` + (nf ? ` (${nf} failed)` : '');
        console.log(line);
    }
}
// Geometric means over the tests every setting completed.
const complete = tests.filter(t => settings.every(s => (runs.get(t + '\t' + s) || []).length));
console.log(`\ngeometric mean over ${complete.length} tests, against ${settings[0]}:`);
for (const s of settings.slice(1)) {
    let line = '  ' + s.padEnd(24);
    for (const m of METRICS) {
        const rs = ratios.get(s)[m].filter(x => complete.includes(x.t)).map(x => x.r);
        const g = Math.exp(rs.reduce((a, r) => a + Math.log(r), 0) / rs.length);
        line += `  ${m} ${pct(g - 1)}`;
    }
    console.log(line);
}

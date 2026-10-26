#!/usr/bin/env node
// Summarize tests/jit/speedometer.sh's results (JS_PERF_DESIGN.md section 4.3).
//   node tools/speedometer_stats.js <results> <setting>...
// A results line is "<setting> <round> <SPEEDOMETER-DONE json>".  For each
// setting: the median over the rounds of the score and of each suite's mean
// time, with the range over the rounds, and the change from the first
// setting.
'use strict';
const fs = require('fs');
const [file, ...settings] = process.argv.slice(2);
if (!file || !settings.length) {
    console.error('usage: speedometer_stats.js <results> <setting>...');
    process.exit(2);
}
const runs = {};
for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
    const m = /^(\S+) (\d+) (\{.*\})$/.exec(line);
    if (!m) continue;
    (runs[m[1]] = runs[m[1]] || []).push(JSON.parse(m[3]));
}
function median(xs) {
    const s = xs.slice().sort((a, b) => a - b), n = s.length;
    return n ? (n % 2 ? s[(n - 1) / 2] : (s[n / 2 - 1] + s[n / 2]) / 2) : NaN;
}
function stat(setting, name) {
    const xs = (runs[setting] || []).map(r => r[name] && r[name].mean).filter(x => typeof x === 'number');
    return { med: median(xs), min: Math.min(...xs), max: Math.max(...xs), n: xs.length };
}
const names = [];
for (const s of settings)
    for (const r of runs[s] || [])
        for (const k of Object.keys(r)) if (!names.includes(k)) names.push(k);
const suites = names.filter(k => k !== 'Geomean' && k !== 'Score');
const base = settings[0];
const pad = (s, n) => String(s).padEnd(n);
const lpad = (s, n) => String(s).padStart(n);
const fmt = x => isFinite(x) ? x.toFixed(1) : '-';
const change = (x, b) => isFinite(x) && isFinite(b) && b ? ((x / b - 1) * 100).toFixed(1) + '%' : '-';

const w = Math.max(10, ...names.map(n => n.length)) + 2;
let header = pad('', w);
for (const s of settings) header += lpad(s + ' (' + (runs[s] || []).length + ')', 22) + (s === base ? '' : lpad('vs ' + base, 12));
console.log(header);
for (const name of ['Score', 'Geomean', ...suites]) {
    let row = pad(name, w);
    const b = stat(base, name);
    for (const s of settings) {
        const st = stat(s, name);
        row += lpad(st.n ? fmt(st.med) + ' [' + fmt(st.min) + '-' + fmt(st.max) + ']' : '-', 22);
        if (s !== base) row += lpad(change(st.med, b.med), 12);
    }
    console.log(row);
}
console.log('\nScore: higher is better (Speedometer\'s 1000 / geomean); the others are ms per iteration, lower is better.');

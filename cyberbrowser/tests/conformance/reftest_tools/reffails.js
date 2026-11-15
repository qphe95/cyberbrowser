// node reffails.js <run dir> [depth] [prefix]: reftest failures by directory (depth path parts), with pass counts,
// and how badly they fail (pixels differing: small <500, mid, whole-page >100k)
const fs = require('fs'), path = require('path');
const [dir, depthArg, prefix] = process.argv.slice(2);
const depth = +(depthArg || 2);
const j = JSON.parse(fs.readFileSync(path.join(dir, 'results.json'), 'utf8'));
const ok = x => x && x.pass > 0 && x.fail === 0;
const by = {};
for (const k in j) {
    if (prefix && !k.startsWith(prefix)) continue;
    const d = k.split('/').slice(0, depth).join('/');
    const e = by[d] = by[d] || { n: 0, pass: 0, small: 0, mid: 0, big: 0, other: 0 };
    e.n++;
    if (ok(j[k])) { e.pass++; continue; }
    const f = (j[k].fails || []).join(' ');
    const m = /(\d+) pixels differ/.exec(f);
    if (!m) e.other++;
    else if (+m[1] < 500) e.small++;
    else if (+m[1] > 100000) e.big++;
    else e.mid++;
}
const rows = Object.entries(by).sort((a, b) => (b[1].n - b[1].pass) - (a[1].n - a[1].pass));
let tn = 0, tp = 0;
for (const [, e] of rows) { tn += e.n; tp += e.pass; }
console.log(`total ${tp}/${tn} (${(100 * tp / tn).toFixed(1)}%), need ${Math.ceil(0.95 * tn) - tp} more for 95%`);
for (const [d, e] of rows.slice(0, +(process.argv[5] || 40)))
    console.log(`${d.padEnd(48)} fail ${String(e.n - e.pass).padStart(5)} / ${String(e.n).padStart(5)}  small ${e.small} mid ${e.mid} big ${e.big} other ${e.other}`);

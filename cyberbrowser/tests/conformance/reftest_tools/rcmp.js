// node rcmp.js <base run> <new run> [prefix]: reftest pass changes of the new run's pages against the base run
const fs = require('fs'), path = require('path');
const [a, b, prefix] = process.argv.slice(2);
const load = d => JSON.parse(fs.readFileSync(path.join(d, 'results.json'), 'utf8'));
const A = load(a), B = load(b);
const ok = x => x && x.pass > 0 && x.fail === 0;
let won = [], lost = [], same = 0, pass = 0, n = 0;
for (const k in B) {
    if (prefix && !k.startsWith(prefix)) continue;
    n++;
    if (ok(B[k])) pass++;
    if (ok(B[k]) && !ok(A[k])) won.push(k);
    else if (!ok(B[k]) && ok(A[k])) lost.push(k);
    else same++;
}
console.log(`pages ${n}, pass ${pass}; won ${won.length}, lost ${lost.length}`);
for (const k of lost) console.log('  -', k, (B[k].fails || []).join(' ').slice(0, 100));
if (process.env.WON) for (const k of won) console.log('  +', k);

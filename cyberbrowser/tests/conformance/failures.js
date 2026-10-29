// The pages of a conformance run that didn't pass cleanly, with their first
// failure messages: node failures.js <results dir> [max messages per page=3]
const fs = require('fs'), path = require('path');
const r = JSON.parse(fs.readFileSync(path.join(process.argv[2], 'results.json'), 'utf8'));
const max = +(process.argv[3] || 3);
for (const [page, x] of Object.entries(r)) {
    if (x.status === 'done' && !x.fail) continue;
    console.log(`${x.status.padEnd(7)} ${x.pass}/${x.pass + x.fail}  ${page}`);
    for (const f of (x.fails || []).slice(0, max)) console.log('        ' + f.replace(/\s+/g, ' ').slice(0, 240));
}

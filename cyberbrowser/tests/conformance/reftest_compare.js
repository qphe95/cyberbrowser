// Compares a REFTEST=1 run's screenshots (wpt_test.sh): each reftest's
// page against its references, as WPT does: every rel=match reference must
// look the same (within its <meta name=fuzzy>), every rel=mismatch one
// different.  Writes results.json (status OK when it passes, FAIL, or
// MISSING without a screenshot) for the summary and failures.js.
//
//   node reftest_compare.js <results dir>
const fs = require('fs'), path = require('path');
const out = process.argv[2];
const { refInfo, steps } = JSON.parse(fs.readFileSync(path.join(out, 'reftests.json'), 'utf8'));

/* A 24-bit bottom-up BMP (shotc's) as { w, h, px: RGB top-down }. */
function readBmp(file) {
    if (!fs.existsSync(file)) return null;
    const b = fs.readFileSync(file);
    const off = b.readUInt32LE(10), w = b.readInt32LE(18), h = b.readInt32LE(22), bpp = b.readUInt16LE(28);
    if (bpp !== 24 || w <= 0 || h <= 0) return null;
    const stride = (w * 3 + 3) & ~3, px = Buffer.alloc(w * h * 3);
    for (let y = 0; y < h; y++) b.copy(px, y * w * 3, off + (h - 1 - y) * stride, off + (h - 1 - y) * stride + w * 3);
    return { w, h, px };
}

/* The largest channel difference and the number of pixels that differ. */
function diff(a, b) {
    if (a.w !== b.w || a.h !== b.h) return { max: 255, count: Infinity };
    let max = 0, count = 0;
    for (let i = 0; i < a.px.length; i += 3) {
        const d = Math.max(Math.abs(a.px[i] - b.px[i]), Math.abs(a.px[i + 1] - b.px[i + 1]), Math.abs(a.px[i + 2] - b.px[i + 2]));
        if (d) { count++; if (d > max) max = d; }
    }
    return { max, count };
}

/* <meta name=fuzzy content="[url:]maxDifference=A-B;totalPixels=C-D"> (or "A-B;C-D"). */
function fuzzRange(spec) {
    const body = spec.replace(/^[^=;]*:(?=\s*(maxDifference|\d))/, '');
    const r = { max: [0, 0], count: [0, 0] };
    const parts = body.split(';').map(p => p.trim()).filter(Boolean);
    parts.forEach((p, i) => {
        const m = /^(?:(maxDifference|totalPixels)\s*=\s*)?(\d+)(?:\s*-\s*(\d+))?$/.exec(p);
        if (!m) return;
        const key = m[1] === 'totalPixels' || (!m[1] && i === 1) ? 'count' : 'max';
        const lo = +m[2], hi = m[3] !== undefined ? +m[3] : lo;
        r[key] = [m[3] !== undefined ? lo : 0, hi];
    });
    return r;
}

function same(d, fuzzy) {
    if (d.count === 0) return true;
    return fuzzy.some(f => {
        const r = fuzzRange(f);
        return d.max >= r.max[0] && d.max <= r.max[1] && d.count >= r.count[0] && d.count <= r.count[1];
    });
}

const results = {};
let k = 0;
for (const [test, info] of Object.entries(refInfo)) {
    const mine = steps.filter(s => s.test === test);
    const shot = readBmp(mine[0].shot);
    const r = { page: test, status: 'OK', pass: 1, fail: 0, skip: 0, fails: [] };
    if (!shot) {
        Object.assign(r, { status: 'MISSING', pass: 0, fail: 1 });
        r.fails.push('no screenshot of the test');
    }
    info.refs.forEach((ref, i) => {
        if (!shot) return;
        const rs = readBmp(mine[i + 1].shot);
        if (!rs) { Object.assign(r, { status: 'MISSING', pass: 0, fail: 1 }); r.fails.push('no screenshot of ' + ref.url); return; }
        const d = diff(shot, rs);
        const ok = ref.kind === 'match' ? same(d, info.fuzzy) : d.count > 0;
        if (!ok) {
            Object.assign(r, { status: 'FAIL', pass: 0, fail: 1 });
            r.fails.push(`${ref.kind} ${ref.url}: ${d.count === Infinity ? 'sizes differ' : `${d.count} pixels differ, by up to ${d.max}`}`);
        }
    });
    results[test] = r;
    k++;
}
fs.writeFileSync(path.join(out, 'results.json'), JSON.stringify(results, null, 1));
console.log(`compared ${k} reftests`);

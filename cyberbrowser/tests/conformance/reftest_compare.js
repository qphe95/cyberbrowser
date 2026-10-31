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

/* A screenshot file's bytes (read once: references are shared), or null. */
const files = new Map();
function readFile(file) {
    if (!files.has(file)) files.set(file, fs.existsSync(file) ? fs.readFileSync(file) : null);
    return files.get(file);
}

/* A screenshot (shotc's: run-length encoded, ui_main.c write_rle, or a
 * 24-bit bottom-up BMP) as { w, h, px: RGB top-down }. */
function readBmp(file) {
    const b = readFile(file);
    if (!b) return null;
    if (b.length >= 12 && b.toString('latin1', 0, 4) === 'CBRL') {
        const w = b.readUInt32LE(4), h = b.readUInt32LE(8), px = Buffer.alloc(w * h * 3);
        for (let o = 12, p = 0; o + 5 <= b.length; o += 5) {
            const n = b.readUInt16LE(o), bl = b[o + 2], g = b[o + 3], r = b[o + 4];
            for (let k = 0; k < n && p < px.length; k++, p += 3) { px[p] = r; px[p + 1] = g; px[p + 2] = bl; }
        }
        return { w, h, px };
    }
    const off = b.readUInt32LE(10), w = b.readInt32LE(18), h = b.readInt32LE(22), bpp = b.readUInt16LE(28);
    if (bpp !== 24 || w <= 0 || h <= 0) return null;
    const stride = (w * 3 + 3) & ~3, px = Buffer.alloc(w * h * 3);
    for (let y = 0; y < h; y++) b.copy(px, y * w * 3, off + (h - 1 - y) * stride, off + (h - 1 - y) * stride + w * 3);
    return { w, h, px };
}

/* Two run-length encoded screenshots' difference, run by run (nothing
 * decoded), or null when either is not one. */
function diffRle(a, b) {
    const isRle = x => x.length >= 12 && x.toString('latin1', 0, 4) === 'CBRL';
    if (!isRle(a) || !isRle(b)) return null;
    if (a.readUInt32LE(4) !== b.readUInt32LE(4) || a.readUInt32LE(8) !== b.readUInt32LE(8)) return { max: 255, count: Infinity };
    let max = 0, count = 0, i = 12, j = 12, ra = 0, rb = 0;
    while (i + 5 <= a.length && j + 5 <= b.length) {
        if (!ra) ra = a.readUInt16LE(i);
        if (!rb) rb = b.readUInt16LE(j);
        const n = Math.min(ra, rb);
        const d = Math.max(Math.abs(a[i + 2] - b[j + 2]), Math.abs(a[i + 3] - b[j + 3]), Math.abs(a[i + 4] - b[j + 4]));
        if (d) { count += n; if (d > max) max = d; }
        ra -= n; rb -= n;
        if (!ra) i += 5;
        if (!rb) j += 5;
    }
    return { max, count };
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
const byUrl = new Map(steps.map(s => [s.url, s]));
let k = 0;
for (const [test, info] of Object.entries(refInfo)) {
    const mine = [test, ...info.refs.map(r => r.url)].map(u => byUrl.get(u));
    const raw = readFile(mine[0].shot);
    let decoded;
    const shot = raw && { get v() { return decoded || (decoded = readBmp(mine[0].shot)); } };
    const r = { page: test, status: 'OK', pass: 1, fail: 0, skip: 0, fails: [] };
    if (!shot) {
        Object.assign(r, { status: 'MISSING', pass: 0, fail: 1 });
        r.fails.push('no screenshot of the test');
    }
    info.refs.forEach((ref, i) => {
        if (!shot) return;
        const rraw = readFile(mine[i + 1].shot);
        if (!rraw) { Object.assign(r, { status: 'MISSING', pass: 0, fail: 1 }); r.fails.push('no screenshot of ' + ref.url); return; }
        /* (the same bytes: the same pixels, without decoding either) */
        const d = raw.equals(rraw) ? { max: 0, count: 0 }
                : diffRle(raw, rraw) || diff(shot.v, readBmp(mine[i + 1].shot));
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

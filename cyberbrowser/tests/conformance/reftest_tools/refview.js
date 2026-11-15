// node refview.js <results dir> <test path> [out.png]: test | reference | difference side by side
// (CROP=x,y,w,h crops each; SCALE=2 doubles).  The other tools eval readBmp..crop from here.
const fs = require('fs'), path = require('path'), zlib = require('zlib');
const [out, test, png] = process.argv.slice(2);

function readBmp(file) {
    if (!fs.existsSync(file)) return null;
    const b = fs.readFileSync(file);
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

function crop(img, x, y, w, h) {
    const px = Buffer.alloc(w * h * 3, 255);
    for (let r = 0; r < h; r++) for (let c = 0; c < w; c++) {
        const sx = x + c, sy = y + r;
        if (sx < 0 || sy < 0 || sx >= img.w || sy >= img.h) continue;
        img.px.copy(px, (r * w + c) * 3, (sy * img.w + sx) * 3, (sy * img.w + sx) * 3 + 3);
    }
    return { w, h, px };
}

function writePng(file, img) {
    const raw = Buffer.alloc((img.w * 3 + 1) * img.h);
    for (let y = 0; y < img.h; y++) img.px.copy(raw, y * (img.w * 3 + 1) + 1, y * img.w * 3, (y + 1) * img.w * 3);
    const crcT = []; for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; crcT[n] = c >>> 0; }
    const crc = b => { let c = 0xFFFFFFFF; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
    const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t, 'latin1'), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
    const ih = Buffer.alloc(13); ih.writeUInt32BE(img.w, 0); ih.writeUInt32BE(img.h, 4); ih[8] = 8; ih[9] = 2;
    fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
}

if (require.main === module) {
    const { refInfo, steps } = JSON.parse(fs.readFileSync(path.join(out, 'reftests.json'), 'utf8'));
    const byUrl = new Map(steps.map(s => [s.url, s]));
    const info = refInfo[test];
    if (!info) { console.log('not a reftest of this run:', test); process.exit(1); }
    const shotOf = u => { const s = byUrl.get(u); return s && readBmp(s.shot); };
    let a = shotOf(info.url || test), b = shotOf(info.refs[0].url);
    if (!a || !b) { console.log('no shot', !!a, !!b); process.exit(1); }
    const [cx, cy, cw, ch] = (process.env.CROP || `0,0,${a.w},${a.h}`).split(',').map(Number);
    a = crop(a, cx, cy, cw, ch); b = crop(b, cx, cy, cw, ch);
    const d = { w: cw, h: ch, px: Buffer.alloc(cw * ch * 3) };
    let n = 0, x0 = 1e9, y0 = 1e9, x1 = -1, y1 = -1;
    for (let i = 0; i < cw * ch; i++) {
        const diff = Math.abs(a.px[i * 3] - b.px[i * 3]) + Math.abs(a.px[i * 3 + 1] - b.px[i * 3 + 1]) + Math.abs(a.px[i * 3 + 2] - b.px[i * 3 + 2]);
        if (diff) { n++; d.px[i * 3] = 255; const x = i % cw, y = (i / cw) | 0; x0 = Math.min(x0, x); y0 = Math.min(y0, y); x1 = Math.max(x1, x); y1 = Math.max(y1, y); }
        else { d.px[i * 3] = d.px[i * 3 + 1] = d.px[i * 3 + 2] = (a.px[i * 3] + a.px[i * 3 + 1] + a.px[i * 3 + 2]) / 9 + 170; }
    }
    const sc = +(process.env.SCALE || 1), W = cw * 3 + 8, all = { w: W * sc, h: ch * sc, px: Buffer.alloc(W * sc * ch * sc * 3, 128) };
    [a, b, d].forEach((im, k) => {
        for (let y = 0; y < ch * sc; y++) for (let x = 0; x < cw * sc; x++)
            im.px.copy(all.px, (y * all.w + k * (cw + 4) * sc + x) * 3, (((y / sc) | 0) * cw + ((x / sc) | 0)) * 3, (((y / sc) | 0) * cw + ((x / sc) | 0)) * 3 + 3);
    });
    const file = png || path.join(__dirname, 'view.png');
    writePng(file, all);
    console.log(`${info.refs[0].kind} ${info.refs[0].url}: ${n} differ` + (n ? `, box ${x0 + cx},${y0 + cy} to ${x1 + cx},${y1 + cy}` : '') + ` -> ${file}`);
}

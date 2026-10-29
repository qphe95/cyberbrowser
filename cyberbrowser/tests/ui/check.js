// Checks on the window tests' screenshots (24-bit BMPs from the UI's "shot").
//   node check.js moved A B DY [X0 X1]  B shows A's content moved up DY px:
//                                      the share of B's content rows that do
//   node check.js changed A B X0 Y0 X1 Y1  the share of that box that differs
// Prints the share (0..1).  The content starts at y 68 (below the strip and
// the address bar).
const fs = require('fs');
const CONTENT_Y = 68;
function load(path) {
  const b = fs.readFileSync(path);
  const off = b.readUInt32LE(10), w = b.readInt32LE(18), h = b.readInt32LE(22);
  const stride = (w * 3 + 3) & ~3;
  return { w, h, px(x, y) { const o = off + (h - 1 - y) * stride + x * 3; return [b[o + 2], b[o + 1], b[o]]; } };
}
const near = (p, q) => Math.abs(p[0] - q[0]) + Math.abs(p[1] - q[1]) + Math.abs(p[2] - q[2]) <= 30;
const [what, a, b, ...rest] = process.argv.slice(2);
const A = load(a), B = load(b);
let hit = 0, n = 0;
if (what === 'moved') {
  const dy = +rest[0], x0 = rest.length > 1 ? +rest[1] : 0, x1 = rest.length > 2 ? +rest[2] : A.w;
  for (let y = CONTENT_Y; y + dy < A.h; y += 2)
    for (let x = x0; x < x1; x += 2, n++) if (near(A.px(x, y + dy), B.px(x, y))) hit++;
} else {
  const [x0, y0, x1, y1] = rest.map(Number);
  for (let y = y0; y < y1; y += 2)
    for (let x = x0; x < x1; x += 2, n++) if (!near(A.px(x, y), B.px(x, y))) hit++;
}
console.log((hit / n).toFixed(4));

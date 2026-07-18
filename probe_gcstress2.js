var keep = [];
// phase 1: 3000 objects same shape
for (var i = 0; i < 3000; i++) keep.push({pageData: {page: "watch", idx: i}, other: "x" + i, third: [i, i + 1]});
// phase 2: churn trash
for (var r = 0; r < 50; r++) {
  var trash = [];
  for (var j = 0; j < 5000; j++) trash.push({a: j, b: "s" + j + "_" + r, c: [j]});
  trash = null;
  // interleave keep-creation at a few points only
  if (r % 10 === 0) keep.push({pageData: {page: "later", idx: 100000 + r}, other: "y" + r, third: [r, r + 1]});
}
var bad = 0;
for (var k = 0; k < keep.length; k++) {
  if (Object.keys(keep[k]).length !== 3) { bad++; if (bad < 6) console.error("BAD " + k + " keys=" + Object.keys(keep[k]).length + " idx=" + (keep[k].pageData && keep[k].pageData.idx)); }
}
console.error("RESULT bad=" + bad + "/" + keep.length);

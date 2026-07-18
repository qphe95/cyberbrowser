// Stress: many live objects with properties, heavy churn to force GC + compaction.
var keep = [];
for (var i = 0; i < 3000; i++) {
  keep.push({pageData: {page: "watch", idx: i}, other: "x" + i, third: [i, i + 1]});
}
function deepChurn() {
  for (var r = 0; r < 50; r++) {
    var trash = [];
    for (var j = 0; j < 5000; j++) trash.push({a: j, b: "s" + j + "_" + r, c: [j]});
    trash = null;
    // also create new live objects during churn
    keep.push({pageData: {page: "later", idx: 100000 + r}, other: "y" + r});
  }
}
deepChurn();
var bad = 0, checked = 0;
for (var k = 0; k < keep.length; k++) {
  var o = keep[k];
  checked++;
  var keys = Object.keys(o);
  if (keys.length !== 3) { bad++; if (bad < 5) console.error("BAD obj " + k + " keys=" + keys.length + " str=" + JSON.stringify(o).slice(0, 60)); }
  else if (!o.pageData || o.pageData.page === undefined) { bad++; if (bad < 5) console.error("BAD2 obj " + k + " pd=" + JSON.stringify(o.pageData)); }
}
console.error("RESULT checked=" + checked + " bad=" + bad);

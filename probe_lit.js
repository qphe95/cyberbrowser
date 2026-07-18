// Mimic publishPageData: build {pageData: V} in a function, churn-heavy.
function pub(V) {
  return {pageData: V};
}
var V = {page: "watch", endpoint: {a: 1}, response: {b: 2}, playerResponse: {c: 3}, url: "/watch", rootVe: 3854};
var bad = 0;
for (var r = 0; r < 30000; r++) {
  // churn to force GC cycles
  var trash = [];
  for (var j = 0; j < 200; j++) trash.push({a: j, b: "s" + j});
  var lit = pub(V);
  var k = Object.keys(lit).length;
  if (k !== 1) {
    bad++;
    if (bad < 5) console.error("BAD r=" + r + " keys=" + k + " str=" + JSON.stringify(lit).slice(0, 50));
  }
}
console.error("RESULT bad=" + bad + "/30000");

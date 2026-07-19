// Trigger GC via 500+ element constructions, then test {pageData:V} literals.
var V = {page: "watch", endpoint: {a: 1}, response: {b: 2}, playerResponse: {c: 3}, url: "/watch", rootVe: 3854};

function makeElements(n) {
  var arr = [];
  for (var i = 0; i < n; i++) arr.push(document.createElement("div"));
  return arr;
}

var bad = 0, total = 0;
for (var round = 0; round < 40; round++) {
  var els = makeElements(300); // churn elements + GC pressure
  var lit = {pageData: V};
  total++;
  var k = Object.keys(lit).length;
  var inres = ('pageData' in lit);
  if (k !== 1 || !inres) {
    bad++;
    if (bad <= 6) console.error("BAD round=" + round + " keys=" + k + " in=" + inres + " own=" + JSON.stringify(Object.getOwnPropertyNames(lit)));
  }
}
console.error("RESULT bad=" + bad + "/" + total);

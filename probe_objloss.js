function sink(evName, detail) {
  // allocate inside the call (like `new CustomEvent`)
  var junk = [];
  for (var i = 0; i < 5000; i++) junk.push({a: i, b: "s" + i});
  return JSON.stringify(detail);
}

var failures = 0, total = 0;
for (var round = 0; round < 2000; round++) {
  var V = { page: "watch", n: round };
  var out = sink("yt-page-data-fetched", { pageData: V });
  total++;
  if (out === "{}") failures++;
  if (round < 3 || failures > 0 && failures < 4) console.error("round " + round + " -> " + out.slice(0, 60));
}
console.error("RESULT failures=" + failures + "/" + total);

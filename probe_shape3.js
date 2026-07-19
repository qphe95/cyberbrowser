// Stress shape hash: many distinct object shapes + element-GC + literal checks.
var keep = [];
var names = [];
for (var i = 0; i < 300; i++) names.push("prop_" + i);

function churn() {
  for (var r = 0; r < 30; r++) {
    var trash = [];
    for (var j = 0; j < 2000; j++) {
      var o = {};
      o[names[j % names.length]] = j;
      o[names[(j + 7) % names.length]] = "s" + j;
      o[names[(j + 13) % names.length]] = [j];
      trash.push(o);
    }
    trash = null;
    // elements to trigger the 500-ctor GC
    var els = [];
    for (var e = 0; e < 100; e++) els.push(document.createElement("div"));
  }
}

var bad = 0;
for (var round = 0; round < 60; round++) {
  // build objects with distinct shape sets
  var a = {pageData: {page: "watch", i: round}, other: "x"};
  var b = {pageData: {page: "watch", i: round}};
  var c = {};
  c.alpha = 1; c.beta = 2; c.gamma = 3; c.delta = 4; c.eps = 5; c.zeta = 6;
  keep.push(a, b, c);
  churn();
}
for (var k = 0; k < keep.length; k++) {
  var o = keep[k];
  var keys = Object.keys(o);
  if (o.pageData === undefined && keys.length === 0) {
    bad++;
    if (bad <= 6) console.error("LOST obj " + k + " keys=" + keys.length);
  }
  for (var kk = 0; kk < keys.length; kk++) {
    if (o[keys[kk]] === undefined && keys[kk] !== "other") {
      // tolerate undefined values only for 'other' which has a string
    }
  }
}
// verify {pageData} objects specifically
var pdBad = 0;
for (var m = 0; m < keep.length; m++) {
  if (keep[m].pageData !== undefined && Object.keys(keep[m]).length === 0) pdBad++;
}
console.error("RESULT lost=" + bad + " pdBad=" + pdBad + " total=" + keep.length);

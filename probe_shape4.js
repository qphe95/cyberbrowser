var keep = [];
function triggerGC() {
  // 500+ element constructions trigger JS_RunGC in the page's element ctor path
  var els = [];
  for (var e = 0; e < 600; e++) els.push(document.createElement("div"));
  return els.length;
}
var bad = 0;
for (var round = 0; round < 8; round++) {
  var a = {pageData: {page: "watch", i: round}, other: "x"};
  keep.push(a);
  triggerGC();
  // immediately after GC, check all kept objects
  for (var k = 0; k < keep.length; k++) {
    var o = keep[k];
    var keys = Object.keys(o);
    if (keys.length === 0 || !('pageData' in o)) {
      bad++;
      console.error("LOST round=" + round + " obj=" + k + " keys=" + keys.length + " in=" + ('pageData' in o) + " own=" + JSON.stringify(Object.getOwnPropertyNames(o)));
    }
  }
}
console.error("RESULT bad=" + bad + " kept=" + keep.length);

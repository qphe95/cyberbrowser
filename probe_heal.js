var n = 0;
for (var i = 0; i < 10000; i++) {
  var o = {};
  o.pageData = {i: i};
  o.other = "x" + i;
  if (!('pageData' in o)) n++;
}
console.error("pageData define failures: " + n + "/10000");

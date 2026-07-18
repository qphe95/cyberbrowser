function makeSet(n, kind) {
  var arr = [];
  for (var i = 0; i < n; i++) {
    if (kind === "scalar") arr.push({a: i, b: "x" + i, c: i * 2});
    else if (kind === "array") arr.push({a: i, b: "x" + i, c: [i, i + 1]});
    else arr.push({a: i, b: "x" + i, c: {v: i}});
  }
  return arr;
}
function churn() {
  for (var r = 0; r < 50; r++) {
    var trash = [];
    for (var j = 0; j < 5000; j++) trash.push({a: j, b: "s" + j + "_" + r, c: [j]});
    trash = null;
  }
}
function check(label, arr, expect) {
  var bad = 0;
  for (var k = 0; k < arr.length; k++) if (Object.keys(arr[k]).length !== expect) bad++;
  console.error(label + ": bad=" + bad + "/" + arr.length);
}
var A = makeSet(500, "scalar");
var B = makeSet(500, "array");
var C = makeSet(500, "object");
churn();
check("scalar", A, 3);
check("array", B, 3);
check("object", C, 3);

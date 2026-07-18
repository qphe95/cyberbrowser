// Reproduce gkY's closure capture: loop creating closures via IIFE,
// run them later after heavy allocation/GC.
function makeJobSet(payloads) {
  var jobs = [];
  for (var V = {i: 0}; V.i < payloads.length; V = {payload: void 0, i: V.i}, V.i++) {
    V.payload = payloads[V.i];
    var Q = function(J) {
      return function() {
        return J.payload.job() + " @i=" + J.i;
      };
    }(V);
    jobs.push(Q);
  }
  return jobs;
}

var payloads = [];
for (var k = 0; k < 5; k++) {
  (function(n){ payloads.push({job: function(){ return "payload" + n; }}); })(k);
}

var jobs = makeJobSet(payloads);

// heavy allocation to churn GC/compaction
for (var r = 0; r < 300; r++) {
  var trash = [];
  for (var j = 0; j < 3000; j++) trash.push({s: "x" + j + "_" + r, a: [j, r]});
  trash = null;
}

var ok = 0, bad = 0;
for (var t = 0; t < jobs.length; t++) {
  try {
    var res = jobs[t]();
    console.error("JOB[" + t + "] -> " + res);
    ok++;
  } catch (e) {
    console.error("JOB[" + t + "] ERROR: " + e.message);
    bad++;
  }
}
console.error("RESULT ok=" + ok + " bad=" + bad);

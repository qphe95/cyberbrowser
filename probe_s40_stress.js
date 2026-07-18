// Stress atoms + GC, then run script_40 verbatim with a mock ytsignals.
function junk(n) {
  var src = [];
  for (var i = 0; i < n; i++) src.push("var jv" + i + "={a" + i + ":" + i + "};");
  return src.join("\n");
}
// thousands of unique atoms through eval (global scope)
for (var round = 0; round < 30; round++) {
  (1, eval)(junk(400) + "\n0");
}
// allocate garbage to push GC/compaction
for (var k = 0; k < 200; k++) {
  var trash = [];
  for (var j = 0; j < 2000; j++) trash.push({s: "str" + j + "_" + k, a: [j, k, j * k]});
  trash = null;
}

var __captured = [];
window.ytsignals = { getInstance: function(){ return {
  onAny: function(x){ return 'SIG:'+x.join('|'); },
  parkOrScheduleJob: function(V,y,Q){ __captured.push(V && V.name); }
}; } };
window.removeEventListener = function(){};
window.addEventListener = function(){};

var scheduleAppLoad=function(e){window.removeEventListener("script-load-dpj",scheduleAppLoad);if(window["ytsignals"]&&window["ytsignals"]["getInstance"]){var ytSignalsInstance=window["ytsignals"]["getInstance"]();var signal=ytSignalsInstance["onAny"](["eoir","eor"]);ytSignalsInstance["parkOrScheduleJob"](appLoad,3,signal)}else{appLoad();}};var appLoad=function(){var ytcsi=window.ytcsi;if(ytcsi)ytcsi.tick("apa_b");};var ytSignals=window["ytsignals"];if(ytSignals)scheduleAppLoad();else window.addEventListener("script-load-dpj",scheduleAppLoad);

console.error('CAPTURED: ' + __captured.join(','));
console.error('window.appLoad.name=' + window.appLoad.name + ' appLoad===scheduleAppLoad:' + (appLoad===scheduleAppLoad));

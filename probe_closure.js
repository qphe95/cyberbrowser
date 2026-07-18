var scheduleAppLoad = function(e) {
  console.error("inside: appLoad.name=" + appLoad.name +
              " appLoad===scheduleAppLoad:" + (appLoad === scheduleAppLoad) +
              " typeof ytSignals=" + typeof ytSignals);
};
var appLoad = function() { return 42; };
var ytSignals = 1;
if (ytSignals) scheduleAppLoad();
console.error("global: appLoad.name=" + appLoad.name);

// also test via a second function referencing both
var probe2 = function() {
  console.error("probe2: sal.name=" + scheduleAppLoad.name + " appLoad.name=" + appLoad.name);
};
probe2();

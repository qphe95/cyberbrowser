var scheduleAppLoad = function(e) {
  window.removeEventListener("script-load-dpj", scheduleAppLoad);
  if (window["ytsignals"] && window["ytsignals"]["getInstance"]) {
    var ytSignalsInstance = window["ytsignals"]["getInstance"]();
    var signal = ytSignalsInstance["onAny"](["eoir", "eor"]);
    ytSignalsInstance["parkOrScheduleJob"](appLoad, 3, signal)
  } else {
    appLoad();
    var ua = window["navigator"]["userAgent"];
    var shouldLog = ua.indexOf("msnbot") === -1 && ua.indexOf("BingPreview") === -1 && ua.indexOf("bingbot") === -1;
    if (shouldLog) window.onerror("ytsignals missing",
      "async_attach_app_loader.js", 0, 0, new Error("ytsignals missing"))
  }
};
var appLoad = function() {
  var ytcsi = window.ytcsi;
  if (ytcsi) ytcsi.tick("apa_b");
  var appEl = document.querySelector("ytd-app");
  var mastheadEl = appEl && appEl.querySelector("ytd-masthead");
  if (mastheadEl) mastheadEl.removeAttribute("disable-upgrade");
  if (appEl) appEl.removeAttribute("disable-upgrade");
  if (ytcsi) ytcsi.tick("apa_a")
};
var ytSignals = window["ytsignals"];
if (ytSignals) scheduleAppLoad();
else window.addEventListener("script-load-dpj", scheduleAppLoad);
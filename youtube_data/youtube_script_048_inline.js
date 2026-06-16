(function serverContract() {
  window['ytPageType'] = "watch";
  window['ytCommand'] = {
    "clickTrackingParams": "IhMI6t2XtfqqkwMV_wYvCB0GvBXJMghleHRlcm5hbMoBBPbWbds=",
    "commandMetadata": {
      "webCommandMetadata": {
        "url": "/watch?v=dQw4w9WgXcQ",
        "webPageType": "WEB_PAGE_TYPE_WATCH",
        "rootVe": 3832
      }
    },
    "watchEndpoint": {
      "videoId": "dQw4w9WgXcQ"
    }
  };
  window['ytUrl'] = '\/watch?v\x3ddQw4w9WgXcQ';
  var a = window;
  (function(e) {
    var c = window;
    c.getInitialCommand = function() {
      return e
    };
    c.loadInitialCommand && c.loadInitialCommand(c.getInitialCommand())
  })(a.ytCommand);
  (function(e, c, l, f, g, h, k) {
    var d = window;
    d.getInitialData = function() {
      var b = window;
      b.ytcsi && b.ytcsi.tick("pr", null, "");
      b = {
        page: e,
        endpoint: c,
        response: l
      };
      f && (b.playerResponse = f);
      g && (b.reelWatchSequenceResponse = g);
      k && (b.url = k);
      h && (b.previousCsn = h);
      return b
    };
    d.loadInitialData && d.loadInitialData(d.getInitialData())
  })(a.ytPageType, a.ytCommand, a.ytInitialData, a.ytInitialPlayerResponse, a.ytInitialReelWatchSequenceResponse, a.ytPreviousCsn, a.ytUrl);
})();
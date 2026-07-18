var o = {};
o.rawResponse = 1; o.autoplayRenderer = 2; o.hasRelatedVideoData = 3; o.playingVideo = 4; o.playingVideoId = 5; o.playlistPanelRenderer = 6; o.playlistId = 7; o.playlistIndex = 8; o.relatedVideoArgs = 9; o.endScreenRenderer = 10;
console.error("keys=" + Object.keys(o).length + " rawResponse=" + o.rawResponse + " endScreen=" + o.endScreenRenderer);
var o2 = {};
for (var i = 0; i < 40; i++) o2["prop" + i] = i;
console.error("o2 keys=" + Object.keys(o2).length + " p39=" + o2.prop39 + " p20=" + o2.prop20);

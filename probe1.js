// Mimics base.js structure: IIFE with `var window=this`, anonymous class expr,
// constructor referencing global Math.
var _yt_player = {};
(function(g){
  var window = this;
  function IL(){ return false; }
  var vg = new class {
    constructor(W,l){
      this.events = [];
      var n = null;
      this.G = IL() || (n != null ? n : Math.random() < W);
    }
    disable(){ this.G = false; }
  }(1, window);
  console.log("vg.G=" + vg.G);
})(_yt_player);
console.log("done typeof Math=" + typeof Math);

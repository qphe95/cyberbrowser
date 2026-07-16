// IIFE + var window=this + class, but named class expression assigned first
(function(g){
  var window = this;
  var C = class { constructor(W){ this.G = Math.random() < W; } };
  var vg = new C(1);
})({});

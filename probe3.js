// IIFE but no `var window=this`
(function(g){
  var vg = new class { constructor(W){ this.G = Math.random() < W; } }(1);
})({});

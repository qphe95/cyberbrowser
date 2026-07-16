// IIFE + var window=this, plain function instead of class
(function(g){
  var window = this;
  function f(W){ return Math.random() < W; }
  f(1);
})({});

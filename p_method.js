(function(g){
  var C = class { m(){ return Math.floor(1.5); } };
  var c = new C();
  if (c.m() !== 1) throw new Error("method Math wrong");
})({});

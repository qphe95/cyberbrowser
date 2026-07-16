(function(g){
  var outerLocal = 42;
  var C = class { constructor(){ this.a = outerLocal + 1; } };
  var c = new C();
  if (c.a !== 43) throw new Error("outerLocal wrong: " + c.a);
})({});

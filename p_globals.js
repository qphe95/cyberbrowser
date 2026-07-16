(function(g){
  var C = class { constructor(){ this.a = isNaN(0) ? 1 : 2; this.b = Object.keys({x:1}).length; } };
  var c = new C();
})({});

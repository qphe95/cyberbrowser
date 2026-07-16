(function(g){
  var B = class { constructor(){ this.x = 1; } };
  var D = class extends B { constructor(){ super(); this.y = Math.ceil(0.5); } };
  var d = new D();
})({});

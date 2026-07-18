var obj = { n: 0, T: function() { this.n++; console.error("T fired n=" + this.n); } };
var bound = obj.T.bind(obj);
console.error("bound type=" + typeof bound + " isFunc=" + (bound instanceof Function));
requestAnimationFrame(bound);
setTimeout(function(){ console.error("after: n=" + obj.n); }, 100);

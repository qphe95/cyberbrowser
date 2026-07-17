// Mimic es5-adapter derived class construction
var captured = [];
var baseProto = { createElement: function(){ return 'elem-made'; } };
class MyEl extends HTMLElement {
  constructor() {
    super();
    Object.setPrototypeOf(this, baseProto);
    captured.push('ctor-ran');
  }
}
var e = new MyEl();
console.error('proto has createElement: ' + (typeof e.createElement));
console.error('createElement works: ' + (e.createElement ? e.createElement() : 'NO'));
console.error('captured: ' + captured.join(','));

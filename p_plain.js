// Plain function constructor (no class TDZ) - the kevlar N pattern
function N() {
  var b = HTMLElement.call(this) || this;
  b.is = 'my-el';
  b.createElement = N.prototype.createElement;
  return b;
}
N.prototype = Object.create(HTMLElement.prototype);
N.prototype.constructor = N;
N.prototype.createElement = function(){ return 'elem-made'; };
try {
  var e = new N();
  console.error('plain N: is=' + e.is + ' createElement=' + typeof e.createElement + ' result=' + (e.createElement ? e.createElement() : 'NO'));
} catch(e) { console.error('plain N threw: ' + e.message); }

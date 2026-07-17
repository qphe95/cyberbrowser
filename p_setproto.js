var N = function(){};
N.prototype.createElement = function(){ return 'made'; };
var el = document.createElement('div');
console.error('before: ' + typeof el.createElement);
Object.setPrototypeOf(el, N.prototype);
console.error('after setProto: ' + typeof el.createElement + ' val=' + (el.createElement ? el.createElement() : 'NO'));
console.error('proto is N.prototype: ' + (Object.getPrototypeOf(el) === N.prototype));

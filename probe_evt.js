var ev = new CustomEvent('attached', {bubbles: true, composed: true});
console.error('type=' + ev.type + ' bubbles=' + ev.bubbles + ' composed=' + ev.composed);
var el = document.createElement('div');
el.addEventListener('attached', function(){ console.error('listener fired'); });
console.error('listeners prop=' + (el['__listeners_attached'] ? el['__listeners_attached'].length : 'none'));
var r = el.dispatchEvent(ev);
console.error('dispatch returned ' + r);
console.error('target=' + (ev.target && ev.target.tagName));

var v = document.createElement('video');
var s = 'tag=' + v.tagName + ' ctor=' + (v.constructor && v.constructor.name);
var d = Object.getOwnPropertyDescriptor(v, 'style');
s += ' ownStyle=' + (d ? (d.get ? 'getter' : typeof d.value) : 'NONE');
s += ' typeofStyle=' + typeof v.style;
throw new Error(s);

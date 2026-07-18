var parent = document.createElement('div');
var child = document.createElement('span');
parent.appendChild(child);
document.body.appendChild(parent);

var got = [];
parent.addEventListener('attached', function(e){
  got.push('parent heard attached, target=' + (e.target && e.target.tagName) + ' currentTarget=' + (e.currentTarget && e.currentTarget.tagName));
});
child.addEventListener('attached', function(e){
  got.push('child heard attached');
});

var ev = new CustomEvent('attached', {bubbles: true, composed: true});
child.dispatchEvent(ev);
console.error('RESULT: ' + got.length + ' | ' + got.join(' ; '));

// also test document-level listener
var got2 = 0;
document.addEventListener('attached', function(){ got2++; });
child.dispatchEvent(new CustomEvent('attached', {bubbles: true, composed: true}));
console.error('DOC RESULT: ' + got2);

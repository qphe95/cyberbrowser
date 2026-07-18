var de = document.documentElement;
console.error('de.parentNode===document: ' + (de.parentNode === document));
console.error('de.parentNode type: ' + (de.parentNode && de.parentNode.nodeType));
var p = window.__cyber_eventComposedPath(de);
console.error('path last type=' + (p[p.length-1] && p[p.length-1].nodeType) + ' last===document: ' + (p[p.length-1] === document));

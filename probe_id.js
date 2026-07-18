var d = document.createElement('div');
d.setAttribute('id', 'page-manager');
console.error('via setAttribute: id=' + d.id + ' getAttribute=' + d.getAttribute('id'));
d.id = 'foo';
console.error('via prop set: id=' + d.id + ' getAttribute=' + d.getAttribute('id'));
var p = document.querySelectorAll('ytd-page-manager')[0];
console.error('page-manager id=' + (p && p.id));

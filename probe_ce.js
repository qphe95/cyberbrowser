var e = new CustomEvent('x', {detail: {pageData: 42, page: 'watch'}});
console.error('detail=' + JSON.stringify(e.detail));
var el = document.createElement('div');
el.addEventListener('x', function(ev){ console.error('listener detail=' + JSON.stringify(ev.detail)); });
el.dispatchEvent(e);

try {
  function MyEl() {
    var b = HTMLElement.call(this) || this;
    b.is = 'my-el';
    if (typeof b.createElement !== 'function') throw new Error('createElement missing on b');
    b.createElement();
    return b;
  }
  MyEl.prototype = Object.create(HTMLElement.prototype);
  MyEl.prototype.constructor = MyEl;
  MyEl.prototype.createElement = function(){ console.error('createElement ran ok'); };
  customElements.define('my-el', MyEl);
  var c = document.createElement('div');
  c.innerHTML = '<my-el></my-el>';
  console.error('innerHTML path: children=' + c.childNodes.length + ' first.createElement=' + (c.firstChild ? typeof c.firstChild.createElement : 'n/a'));
} catch(e) { console.error('innerHTML path threw: ' + e.message); }

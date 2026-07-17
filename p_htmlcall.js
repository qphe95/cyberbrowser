try {
  var b = HTMLElement.call({});
  console.error('HTMLElement.call({}) ok: ' + typeof b);
} catch(e) { console.error('HTMLElement.call threw: ' + e.message); }
class MyEl extends HTMLElement {
  constructor() {
    var b = HTMLElement.call(this) || this;
    b.is = 'my-el';
    this.result = b.is;
  }
}
try { var e = new MyEl(); console.error('derived HTMLElement.call(this) ok: ' + e.result); }
catch(e) { console.error('derived threw: ' + e.message); }

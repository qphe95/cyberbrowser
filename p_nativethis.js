class MyEl extends HTMLElement {
  constructor() {
    super();
    console.error('this after native super: typeof=' + typeof this + ' isObj=' + (this !== null && typeof this === 'object'));
    this.marker = 'set';
  }
}
try { var e = new MyEl(); console.error('marker=' + e.marker); }
catch(e) { console.error('threw: ' + e.message); }

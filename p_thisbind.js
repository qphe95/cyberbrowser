class Base { constructor(){ this.baseProp = 'base-val'; } }
class Derived extends Base {
  constructor() {
    super();
    console.error('this after super: baseProp=' + this.baseProp + ' thisUndef=' + (typeof this));
  }
}
try { new Derived(); } catch(e) { console.error('derived threw: ' + e.message); }

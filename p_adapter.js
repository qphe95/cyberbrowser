'use strict';/*

 Copyright (c) 2016 The Polymer Project Authors. All rights reserved.
 This code may only be used under the BSD style license found at http://polymer.github.io/LICENSE.txt
 The complete set of authors may be found at http://polymer.github.io/AUTHORS.txt
 The complete set of contributors may be found at http://polymer.github.io/CONTRIBUTORS.txt
 Code distributed by Google as part of the polymer project is also
 subject to an additional IP rights grant found at http://polymer.github.io/PATENTS.txt
*/
(()=>{if(window.customElements){var h=window.HTMLElement,m=window.customElements.define,n=window.customElements.get,k=new Map,l=new Map,e=!1,f=!1;window.HTMLElement=function(){if(!e){var a=k.get(this.constructor);a=n.call(window.customElements,a);f=!0;return new a}e=!1};window.HTMLElement.prototype=h.prototype;window.HTMLElement.es5Shimmed=!0;Object.defineProperty(window,"customElements",{value:window.customElements,configurable:!0,writable:!0});Object.defineProperty(window.customElements,"define",
{value:(a,b)=>{var c=b.prototype,g=class extends h{constructor(){super();Object.setPrototypeOf(this,c);if(!f){e=!0;try{b.call(this)}catch(p){throw Error("Constructing ".concat(a,": ").concat(p));}}f=!1}},d=g.prototype;g.observedAttributes=b.observedAttributes;d.connectedCallback=c.connectedCallback;d.disconnectedCallback=c.disconnectedCallback;d.attributeChangedCallback=c.attributeChangedCallback;d.adoptedCallback=c.adoptedCallback;k.set(b,a);l.set(a,b);m.call(window.customElements,a,g)},configurable:!0,
writable:!0});Object.defineProperty(window.customElements,"get",{value:a=>l.get(a),configurable:!0,writable:!0});if(navigator.userAgent.match(/Version\/(10\..*|11\.0\..*)Safari/)){let a=HTMLElement.prototype.constructor;Object.defineProperty(HTMLElement.prototype,"constructor",{configurable:!0,get(){return a},set(b){Object.defineProperty(this,"constructor",{value:b,configurable:!0,writable:!0})}})}}})();
//# sourceMappingURL=blaze-out/k8-opt/bin/third_party/javascript/custom_elements/fast-shim.js.sourcemap

// The custom class (like KvR's N)
function MyEl() {
  var b = HTMLElement.call(this) || this;
  b.is = 'my-el';
  b.createElement();
  return b;
}
MyEl.prototype = Object.create(HTMLElement.prototype);
MyEl.prototype.constructor = MyEl;
MyEl.prototype.createElement = function(){ console.error('createElement ran'); };
try {
  customElements.define('my-el', MyEl);
  var el = document.createElement('my-el');
  console.error('upgrade: proto-chain ok');
  if (window.customElements.upgrade) window.customElements.upgrade(el);
  console.error('post-upgrade createElement=' + typeof el.createElement);
} catch(e) { console.error('adapter flow threw: ' + e.message); }

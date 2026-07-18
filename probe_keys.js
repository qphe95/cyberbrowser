var V = {page: "watch"};
var lit = {pageData: V};
console.error("keys=" + JSON.stringify(Object.keys(lit)));
console.error("ownNames=" + JSON.stringify(Object.getOwnPropertyNames(lit)));
console.error("in=" + ('pageData' in lit) + " val=" + (lit.pageData ? 'y' : 'n'));
console.error("desc=" + JSON.stringify(Object.getOwnPropertyDescriptor(lit, 'pageData')));

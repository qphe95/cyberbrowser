function foo(a) { return a + 1; }
var arrow = (x) => x * 2;
var f2 = new Function('a', 'return a');
throw new Error('A=' + JSON.stringify(foo.toString()) + ' B=' + JSON.stringify(arrow.toString()) + ' C=' + JSON.stringify(f2.toString()) + ' NOW=' + new Date().toISOString() + ' DATENOW=' + Date.now());

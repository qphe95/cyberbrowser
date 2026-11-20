// node tools/gen_wasm_js.js: concatenates src/browser/wasm/*.js (in name order) into src/browser/wasm.js
const fs = require('fs'), path = require('path');
const dir = path.join(__dirname, '..', 'src', 'browser', 'wasm');
const parts = fs.readdirSync(dir).filter(f => /\.js$/.test(f)).sort();
let out = '';
for (const p of parts) out += fs.readFileSync(path.join(dir, p), 'utf8').replace(/\r\n/g, '\n') + '\n';
fs.writeFileSync(path.join(dir, '..', 'wasm.js'), out);
console.log('wasm.js from', parts.join(' '), out.split('\n').length, 'lines');

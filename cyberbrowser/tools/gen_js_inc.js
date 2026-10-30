// node gen_js_inc.js in.js out.inc : the same output as tools/gen_js_inc.py
// (a JS source file as a C string literal for #include), where there is no
// Python.
const fs = require('fs');
const [inp, out] = process.argv.slice(2);
const src = fs.readFileSync(inp, 'utf8').replace(/\r\n?/g, '\n');
const lines = ['/* Generated from ' + inp.split(/[\\/]/).pop() + ' by tools/gen_js_inc.py - do not edit. */'];
const hex = n => '\\u' + n.toString(16).padStart(4, '0');
for (const line of src.split('\n')) {
    let esc = line.replace(/\\/g, '\\\\').replace(/"/g, '\\"');
    /* (Python escapes each UTF-16 unit of a non-ASCII character) */
    let s = '';
    for (let i = 0; i < esc.length; i++) {
        const u = esc.charCodeAt(i);
        s += u < 128 ? esc[i] : hex(u);
    }
    lines.push('"' + s + '\\n"');
}
fs.writeFileSync(out, lines.join('\n') + '\n');

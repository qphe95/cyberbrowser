#!/usr/bin/env node
// Symbolize and summarize CYBER_SAMPLE output.
//   node tools/sample_report.js <samples.txt> <cyberbrowser.exe> [top=40]
// Prints self time by (innermost inlined) function, inclusive time by
// function, and a coarse breakdown by subsystem.  Needs addr2line/objdump
// (MinGW) on PATH and an executable built with -g.
'use strict';
const fs = require('fs');
const { execFileSync } = require('child_process');

const [samplesPath, exe, topArg] = process.argv.slice(2);
if (!samplesPath || !exe) {
    console.error('usage: sample_report.js <samples.txt> <exe> [top]');
    process.exit(1);
}
const TOP = Number(topArg) || 40;

const lines = fs.readFileSync(samplesPath, 'utf8').split('\n');
const header = lines[0].startsWith('#') ? lines.shift() : '';
const intervalUs = Number((header.match(/interval_us (\d+)/) || [])[1]) || 1000;
const samples = lines.filter(l => l.trim()).map(l => l.split(' ').map(x => BigInt('0x' + x)));

const objdump = execFileSync('objdump', ['-p', exe], { encoding: 'utf8', maxBuffer: 1 << 26 });
const base = BigInt('0x' + objdump.match(/ImageBase\s+([0-9a-fA-F]+)/)[1]);

// Unique addresses -> inline chains (innermost first).  Return addresses
// point after the call: look up addr-1 for callers.
const uniq = new Map();
for (const s of samples)
    s.forEach((rva, d) => { if (rva) uniq.set(d ? rva - 1n : rva, null); });
const addrs = [...uniq.keys()];
const CHUNK = 20000;
for (let i = 0; i < addrs.length; i += CHUNK) {
    const part = addrs.slice(i, i + CHUNK);
    const out = execFileSync('addr2line', ['-f', '-i', '-a', '-e', exe],
        { input: part.map(a => '0x' + (a + base).toString(16)).join('\n') + '\n', encoding: 'utf8', maxBuffer: 1 << 28 });
    const rows = out.split(/\r?\n/);
    let k = -1, cur = null;
    for (let r = 0; r < rows.length; r++) {
        const row = rows[r];
        if (/^0x[0-9a-f]+$/i.test(row)) { k++; cur = []; uniq.set(part[k], cur); continue; }
        if (!cur || !row) continue;
        const loc = rows[r + 1] || '';
        cur.push({ fn: row, loc: loc.replace(/^.*[\\/]src[\\/]/, '').replace(/ \(discriminator \d+\)/, '') });
        r++;
    }
}
const frames = (rva, d) => (rva ? uniq.get(d ? rva - 1n : rva) : null) || [{ fn: '[outside image]', loc: '' }];

const self = new Map(), selfLine = new Map(), incl = new Map();
const add = (m, k, v = 1) => m.set(k, (m.get(k) || 0) + v);
for (const s of samples) {
    const leaf = frames(s[0], 0);
    add(self, leaf[0].fn);
    add(selfLine, leaf[0].fn + '  ' + leaf[0].loc);
    const seen = new Set();
    s.forEach((rva, d) => { for (const f of frames(rva, d)) seen.add(f.fn); });
    for (const fn of seen) add(incl, fn);
}

// Subsystem of a sample: the first matching rule over its frames, leaf to
// root (so "GC inside a DOM call" counts as GC).
const rules = [
    ['GC', /^(gc_|GCCompact|compact|mark_|sweep|js_gc|JS_RunGC|gc$)/],
    ['regexp', /^(lre_|js_regexp)/],
    ['string/number builtins', /^(js_string|JS_ConcatString|js_array_buffer|js_dtoa|js_number|js_atof|StringBuffer|string_buffer|js_sub_string|JS_NewAtom|__JS_NewAtom|JS_AtomGetStr|js_json|JS_JSONStringify|json_|js_std|js_os)/],
    ['parse/compile', /^(js_parse|next_token|js_create_function|resolve_|__JS_EvalInternal|JS_EvalFunction|js_compile|ident_realloc|emit_|skip_spaces)/],
    ['DOM/layout (native)', /^(dom_|js_node_|js_element|js_document|html_|css_|layout_|lq_|cp_|style_|snapshot_|ser_|js_dom|DomWrap|image_)/],
    ['property access/shapes', /^(find_own_property|JS_GetPropertyInternal|JS_SetPropertyInternal|add_property|js_shape|resize_properties|JS_DefineProperty|delete_property|js_get_length|JS_GetOwnPropertyNames|convert_fast_array|expand_fast_array|js_create_from_ctor|JS_NewObjectFromShape|js_closure)/],
    ['interpreter', /^(JS_CallInternal|js_call_c_function|js_call_bound|async_func|js_async|js_generator|js_function_apply|JS_Call$|js_array_iterator|js_build_arguments)/],
];
const sys = new Map();
for (const s of samples) {
    let cat = 'other';
    outer: for (let d = 0; d < s.length; d++)
        for (const f of frames(s[d], d)) {
            for (const [name, re] of rules) if (re.test(f.fn)) { cat = name; break outer; }
        }
    add(sys, cat);
}

// Samples whose leaf is in the OS (waits, system calls): the first frame
// back in the image says who is waiting.
const osCaller = new Map(), osCallerLine = new Map();
for (const s of samples) {
    if (s[0]) continue;
    let d = 1;
    while (d < s.length && !s[d]) d++;
    if (d >= s.length) { add(osCaller, '[no frame in image]'); continue; }
    const f = frames(s[d], d);
    add(osCaller, f[0].fn);
    add(osCallerLine, f[0].fn + '  ' + f[0].loc);
}

const N = samples.length;
const pct = v => (100 * v / N).toFixed(1).padStart(5) + '%';
const ms = v => (v * intervalUs / 1000).toFixed(0).padStart(7) + ' ms';
const show = (title, m, n = TOP) => {
    console.log(`\n== ${title} ==`);
    [...m.entries()].sort((a, b) => b[1] - a[1]).slice(0, n)
        .forEach(([k, v]) => console.log(pct(v), ms(v), ' ', k));
};
console.log(`${N} samples, ${intervalUs} us apart (~${(N * intervalUs / 1e6).toFixed(1)} s on the main thread)`);
show('subsystem (first match from the leaf)', sys, 20);
show('in the OS, called from', osCaller, 25);
show('in the OS, called from (line)', osCallerLine, 15);
show('self, by function (innermost inline)', self);
show('self, by line', selfLine, 25);
show('inclusive, by function', incl);

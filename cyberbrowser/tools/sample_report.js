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
// Frames in other modules (bit 61: an address) are named by the sampler's
// "# ext <address> <module>!<symbol>+<offset>" lines.
const EXT_FLAG = 1n << 61n;
const extNames = new Map();
for (const l of lines) {
    const m = l.match(/^# ext ([0-9a-f]+) (\S+)/);
    if (m) extNames.set(BigInt('0x' + m[1]), m[2]);
}
const samples = lines.filter(l => l.trim() && !l.startsWith('#')).map(l => l.split(' ').map(x => BigInt('0x' + x)));

const objdump = execFileSync('objdump', ['-p', exe], { encoding: 'utf8', maxBuffer: 1 << 26 });
const base = BigInt('0x' + objdump.match(/ImageBase\s+([0-9a-fA-F]+)/)[1]);

// Frames in the JIT's code (bit 62 set: an offset in its region) are named
// from <samples>.jit, the JIT's map: jit:<op> for the instruction's code
// ("(slow)": its out-of-line generic copy), with the compiled function as
// the caller frame jit:<function>.
const JIT_FLAG = 1n << 62n;
const jitFns = [], jitInsns = [];
if (fs.existsSync(samplesPath + '.jit')) {
    for (const line of fs.readFileSync(samplesPath + '.jit', 'utf8').split(/\r?\n/)) {
        const p = line.split(' ');
        if (p[0] === 'F' || p[0] === 'X') jitFns.push({ off: parseInt(p[1], 16), size: parseInt(p[2], 16), name: p.slice(3).join(' ') });
        else if (p[0] === 'I' || p[0] === 'S') jitInsns.push({ off: parseInt(p[1], 16), op: p[2] + (p[0] === 'S' ? ' (slow)' : '') });
        else if (p[0] === 'L') jitInsns.push({ off: parseInt(p[1], 16), op: '(literals)' });
    }
    jitFns.sort((a, b) => a.off - b.off);
    jitInsns.sort((a, b) => a.off - b.off);
}
const lastAtOrBelow = (arr, off) => {
    let lo = 0, hi = arr.length - 1, best = null;
    while (lo <= hi) { const m = (lo + hi) >> 1; if (arr[m].off <= off) { best = arr[m]; lo = m + 1; } else hi = m - 1; }
    return best;
};
const jitCache = new Map();
function jitFrames(off) {
    if (jitCache.has(off)) return jitCache.get(off);
    const fn = lastAtOrBelow(jitFns, off);
    let r;
    if (!fn || off >= fn.off + fn.size) r = [{ fn: 'jit:?', loc: '' }];
    else {
        const ins = lastAtOrBelow(jitInsns, off);
        const op = ins && ins.off >= fn.off ? ins.op : fn.name;
        r = [{ fn: 'jit:' + op, loc: fn.name }, { fn: 'jit:' + fn.name, loc: '' }];
    }
    jitCache.set(off, r);
    return r;
}

// Unique addresses -> inline chains (innermost first).  Return addresses
// point after the call: look up addr-1 for callers.
const uniq = new Map();
for (const s of samples)
    s.forEach((rva, d) => { if (rva && !(rva & (JIT_FLAG | EXT_FLAG))) uniq.set(d ? rva - 1n : rva, null); });
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
        const where = loc.replace(/^.*[\\/]src[\\/]/, '').replace(/ \(discriminator \d+\)/, '');
        // No symbol (third-party code built without names): its file.
        const file = where.replace(/:.*$/, '').replace(/^.*[\\/]/, '');
        cur.push({ fn: row === '??' && file && file !== '??' ? '[' + file + ']' : row, loc: where });
        r++;
    }
}
// An external frame: module!symbol without the offset (the offset goes in
// loc), and "[outside image]" when the sampler could not name it.
function extFrame(rva) {
    const name = extNames.get(rva & ~EXT_FLAG);
    if (!name) return [{ fn: '[outside image]', loc: '' }];
    const m = name.match(/^(.*?)(\+[0-9a-f]+)?$/);
    return [{ fn: m[1], loc: name }];
}
const isExt = rva => !rva || (rva & EXT_FLAG) !== 0n;
const frames = (rva, d) => (rva & JIT_FLAG) ? jitFrames(Number((rva & ~JIT_FLAG) - (d ? 1n : 0n)))
    : (rva & EXT_FLAG) ? extFrame(rva)
    : (rva ? uniq.get(d ? rva - 1n : rva) : null) || [{ fn: '[outside image]', loc: '' }];

const self = new Map(), selfLine = new Map(), selfCaller = new Map(), incl = new Map();
const add = (m, k, v = 1) => m.set(k, (m.get(k) || 0) + v);
for (const s of samples) {
    const leaf = frames(s[0], 0);
    add(self, leaf[0].fn);
    add(selfLine, leaf[0].fn + '  ' + leaf[0].loc);
    // The leaf's caller: the function it is inlined into, else the next frame.
    const caller = leaf[1] || (s.length > 1 ? frames(s[1], 1)[0] : null);
    add(selfCaller, leaf[0].fn + '  <-  ' + (caller ? caller.fn + '  ' + caller.loc : '?'));
    const seen = new Set();
    s.forEach((rva, d) => { for (const f of frames(rva, d)) seen.add(f.fn); });
    for (const fn of seen) add(incl, fn);
}

// Subsystem of a sample: the first matching rule over its frames, leaf to
// root (so "GC inside a DOM call" counts as GC).
const rules = [
    // The test harness, not the page: replay file reads, logging, the timeline.
    ['harness (replay reads, logs)', /^(replay_|record_|tl_mark|loop_log|platform_log|print_body_snippet|__pformat|__mingw_pformat|vfprintf|printf|fprintf)/],
    ['fonts and text', /^(load_sys|read_at|index_file|index_dir|font_|stbtt|text_shape|shape_run|hb_)/],
    ['GC', /^(gc_|GCCompact|compact|mark_|sweep|js_gc|JS_RunGC|gc$)/],
    ['regexp', /^(lre_|js_regexp)/],
    ['string/number builtins', /^(js_string|JS_ConcatString|js_array_buffer|js_dtoa|js_number|js_atof|StringBuffer|string_buffer|js_sub_string|JS_NewAtom|__JS_NewAtom|JS_AtomGetStr|js_json|JS_JSONStringify|json_|js_std|js_os)/],
    ['parse/compile', /^(js_parse|next_token|js_create_function|resolve_|__JS_EvalInternal|JS_EvalFunction|js_compile|ident_realloc|emit_|skip_spaces)/],
    ['DOM/layout (native)', /^(dom_|js_node_|js_element|js_document|html_|css_|layout_|lq_|cp_|style_|snapshot_|ser_|js_dom|DomWrap|image_)/],
    ['property access/shapes', /^(find_own_property|JS_GetPropertyInternal|JS_SetPropertyInternal|add_property|js_shape|resize_properties|JS_DefineProperty|delete_property|js_get_length|JS_GetOwnPropertyNames|convert_fast_array|expand_fast_array|js_create_from_ctor|JS_NewObjectFromShape|js_closure)/],
    ['JIT code', /^jit:/],
    ['interpreter', /^(JS_CallInternal|jsop_|js_call_c_function|js_call_bound|async_func|js_async|js_generator|js_function_apply|JS_Call$|js_array_iterator|js_build_arguments)/],
];
const sys = new Map(), sysLeaf = new Map();   // sysLeaf: subsystem -> leaf -> samples
for (const s of samples) {
    let cat = 'other';
    outer: for (let d = 0; d < s.length; d++)
        for (const f of frames(s[d], d)) {
            for (const [name, re] of rules) if (re.test(f.fn)) { cat = name; break outer; }
        }
    add(sys, cat);
    if (!sysLeaf.has(cat)) sysLeaf.set(cat, new Map());
    add(sysLeaf.get(cat), frames(s[0], 0)[0].fn);
}

// Samples whose leaf is in the OS (waits, system calls): the first frame
// back in the image says who is waiting.
const osCaller = new Map(), osCallerLine = new Map(), osBoth = new Map();
for (const s of samples) {
    if (!isExt(s[0])) continue;
    let d = 1;
    while (d < s.length && isExt(s[d])) d++;
    if (d >= s.length) { add(osCaller, '[no frame in image]'); continue; }
    const f = frames(s[d], d);
    add(osCaller, f[0].fn);
    add(osCallerLine, f[0].fn + '  ' + f[0].loc);
    add(osBoth, frames(s[0], 0)[0].fn + '  <-  ' + f[0].fn);
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
show('in the OS, by function, called from', osBoth, 25);
console.log('\n== subsystems, by leaf ==');
for (const [cat] of [...sys.entries()].sort((a, b) => b[1] - a[1])) {
    console.log(`  ${cat}: ${pct(sys.get(cat)).trim()}`);
    [...sysLeaf.get(cat).entries()].sort((a, b) => b[1] - a[1]).slice(0, 8)
        .forEach(([k, v]) => console.log('    ' + pct(v), ms(v), ' ', k));
}
show('self, by function (innermost inline)', self);
show('self, by line', selfLine, 25);
show('self, with caller', selfCaller, +process.env.CALLERS || 25);   // CALLERS=<n>: more rows
show('inclusive, by function', incl);

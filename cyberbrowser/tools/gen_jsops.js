#!/usr/bin/env node
// Generate src/quickjs/quickjs_jsops.inc: one helper per opcode, made from
// the interpreter's own case bodies in JS_CallInternal (quickjs.c).
//
//   node tools/gen_jsops.js            write the file
//   node tools/gen_jsops.js --check    fail if the file is not current
//
// Each helper runs one instruction over a JSInterpFrame (the interpreter's
// locals): jsop_<op>(F) with F->pc just past the opcode byte.  It returns
// JSOP_NEXT, JSOP_EXCEPTION, JSOP_DONE or JSOP_DONE_GENERATOR, with F->sp,
// F->pc and F->ret_val updated, exactly as the interpreter's BREAK, `goto
// exception`, `goto done` and `goto done_generator` leave them.  The
// baseline JIT calls these for every instruction it does not inline, so
// the two tiers share one implementation of every opcode (JIT_DESIGN.md M1).
//
// A case body may jump to a label in another case (binary_arith_slow,
// set_true...) or fall through into the next one: the helper then includes
// those cases too, inside a switch on its own (constant) opcode.
'use strict';
const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..', 'src', 'quickjs');
const srcPath = path.join(root, 'quickjs.c');
const outPath = path.join(root, 'quickjs_jsops.inc');
const src = fs.readFileSync(srcPath, 'utf8').replace(/\r\n/g, '\n');

// --- Locate the interpreter's switch -----------------------------------
const fnStart = src.indexOf('static GCValue JS_CallInternal(JSContextHandle caller_ctx,');
if (fnStart < 0) throw new Error('JS_CallInternal definition not found');
const swStart = src.indexOf('SWITCH(pc) {', fnStart);
if (swStart < 0) throw new Error('SWITCH(pc) not found');
const open = src.indexOf('{', swStart);

// Index of the brace closing the one at `i`, skipping strings, character
// constants and comments.
function matchBrace(s, i) {
    let depth = 0;
    for (; i < s.length; i++) {
        const c = s[i];
        if (c === '/' && s[i + 1] === '*') { i = s.indexOf('*/', i + 2) + 1; continue; }
        if (c === '/' && s[i + 1] === '/') { i = s.indexOf('\n', i); continue; }
        if (c === '"' || c === "'") {
            for (i++; s[i] !== c; i++) if (s[i] === '\\') i++;
            continue;
        }
        if (c === '{') depth++;
        else if (c === '}' && --depth === 0) return i;
    }
    throw new Error('unbalanced braces');
}
const close = matchBrace(src, open);
const body = src.slice(open + 1, close);

// --- Preprocess: drop disabled blocks and macro definitions ------------
// SHORT_OPCODES is 0 in this build (quickjs.c) and `#if 0` is dead; the
// macros the switch defines (GET_FIELD_INLINE, OP_CMP...) stay defined
// after JS_CallInternal, where the generated file is included.
function preprocess(text) {
    const lines = text.split('\n');
    const out = [];
    const stack = [];   // per #if: { live, parentLive }
    let live = true;
    for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        const t = line.trim();
        let m;
        if ((m = t.match(/^#\s*if(n?def)?\s+(.*)$/))) {
            const cond = m[2].trim();
            let val;
            if (m[1] === undefined && (cond === 'SHORT_OPCODES' || cond === '0')) val = false;
            else if (m[1] === undefined && cond === 'EXEC_TRACE_ENABLED') val = false;
            else throw new Error(`unexpected preprocessor condition in the switch: ${t}`);
            stack.push({ live, val });
            live = live && val;
            continue;
        }
        if (/^#\s*else\b/.test(t)) {
            const top = stack[stack.length - 1];
            live = top.live && !top.val;
            continue;
        }
        if (/^#\s*endif\b/.test(t)) {
            live = stack.pop().live;
            continue;
        }
        if (!live) continue;
        if (/^#\s*(define|undef)\b/.test(t)) {
            if (/^#\s*undef\b/.test(t)) throw new Error(`#undef in the switch: ${t}`);
            while (lines[i].trimEnd().endsWith('\\')) i++;
            continue;
        }
        out.push(line);
    }
    if (stack.length) throw new Error('unterminated #if in the switch');
    return out;
}
const lines = preprocess(body);

// --- Split into case groups ---------------------------------------------
// A group starts at a run of case labels (CASE(OP_x):, DEFAULT:) or at an
// OP_CMP(OP_x, ...) macro use, and runs to the next one.
const caseRe = /^\s*CASE\((OP_\w+)\):\s*(\/\*.*\*\/)?\s*$/;
const cmpRe = /^\s*OP_CMP\((OP_\w+),/;
const defaultRe = /^\s*DEFAULT:\s*(\/\*.*\*\/)?\s*$/;
const groups = [];
let cur = null, prevWasLabel = false;
for (const line of lines) {
    let m, isLabel = false, ops = null;
    if ((m = line.match(caseRe))) { ops = [m[1]]; isLabel = true; }
    else if (defaultRe.test(line)) { ops = ['DEFAULT']; isLabel = true; }
    else if ((m = line.match(cmpRe))) { ops = [m[1]]; }
    if (ops) {
        if (!(isLabel && prevWasLabel && cur)) {
            cur = { ops: [], lines: [] };
            groups.push(cur);
        }
        cur.ops.push(...ops);
        cur.lines.push(line);
        prevWasLabel = isLabel;
        continue;
    }
    prevWasLabel = false;
    if (!cur) {
        if (line.trim()) throw new Error(`code before the first case: ${line}`);
        continue;
    }
    cur.lines.push(line);
}

// Labels a group defines and jumps it makes; whether it can fall through.
const EXITS = new Set(['exception', 'done', 'done_generator']);
const labelOwner = new Map();
groups.forEach((g, gi) => {
    // The interpreter's back-edge counting (JS_JIT_BACKEDGE: on-stack
    // replacement through restart) has no use in compiled code.
    g.lines = g.lines.map(l => l.replace(/JS_JIT_BACKEDGE\([^;]*\);/g, ';'));
    g.text = g.lines.join('\n');
    g.labels = [];
    for (const line of g.lines) {
        const m = line.match(/^\s*([A-Za-z_]\w*):(?!:)/);
        if (m && m[1] !== 'default' && m[1] !== 'case' && !/^\s*(CASE|DEFAULT)\b/.test(line)) {
            g.labels.push(m[1]);
            if (labelOwner.has(m[1])) throw new Error(`label ${m[1]} defined twice`);
            labelOwner.set(m[1], gi);
        }
    }
    g.gotos = [...new Set([...g.text.matchAll(/\bgoto\s+(\w+)\s*;/g)].map(m => m[1]))];
    // The last statement: BREAK, a goto or an OP_CMP use (which ends in
    // BREAK) cannot fall through.
    const code = g.lines.map(l => l.replace(/\/\/.*$/, '')).join('\n')
        .replace(/\/\*[\s\S]*?\*\//g, '').trim();
    g.fallsThrough = !(/\bBREAK\s*;$/.test(code) || /\bgoto\s+\w+\s*;$/.test(code) || cmpRe.test(g.lines[0]));
});
for (const g of groups)
    for (const l of g.gotos)
        if (!EXITS.has(l) && !labelOwner.has(l))
            throw new Error(`goto ${l}: label not found in the switch`);
if (groups[groups.length - 1].fallsThrough)
    throw new Error('the last case falls out of the switch');

// --- The frame state -----------------------------------------------------
// Locals of JS_CallInternal a case body may use.  The first three are
// written back to the frame on every exit; a body assigning any other is
// an error (the generator would lose the store).
const WRITTEN = [
    ['GCValue *', 'sp'],
    ['const uint8_t *', 'pc'],
    ['GCValue', 'ret_val'],
];
const READ = [
    ['JSContextHandle', 'caller_ctx'],
    ['JSContextHandle', 'ctx'],
    ['JSRuntimeHandle', 'rt'],
    ['JSObjectHandle', 'p'],
    ['JSFunctionBytecodeHandle', 'b'],
    ['JSStackFrame *', 'sf'],
    ['GCValue *', 'stack_buf'],
    ['GCValue *', 'var_buf'],
    ['GCValue *', 'arg_buf'],
    ['JSVarRefHandle *', 'var_refs'],
    ['GCHandle', 'var_refs_handle'],
    ['GCHandle *', 'sf_var_refs'],
    ['GCValue', 'func_obj'],
    ['GCValue', 'this_obj'],
    ['GCValue', 'new_target'],
    ['int', 'argc'],
    ['GCValue *', 'argv'],
    ['int', 'flags'],
];
const readNames = READ.map(r => r[1]);

// Assignments to a read-only frame local, other than in a declaration that
// shadows it (`JSObjectHandle p = ...`).
function checkAssignments(op, text) {
    // Blank out strings, character constants and comments first.
    text = text.replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*|"(\\.|[^"\\\n])*"|'(\\.|[^'\\\n])*'/g,
                        s => s.replace(/[^\n]/g, ' '));
    // Names the body declares itself (block locals shadowing the frame's).
    const shadowed = new Set();
    const declRe = /^\s*(?:const\s+)?(?:int|BOOL|uint32_t|int32_t|GCValue|JSAtom|JSObjectHandle|JSFunctionBytecodeHandle|JSContextHandle|GCHandle)\s+([^;]*);/gm;
    for (const d of text.matchAll(declRe))
        for (const part of d[1].replace(/\([^)]*\)|\{[^}]*\}/g, '').split(','))
            shadowed.add(part.replace(/=.*$/, '').replace(/[*\s]|\[.*\]/g, ''));
    const re = new RegExp(`(^|[^\\w.>])(${readNames.join('|')})\\s*(=(?!=)|\\+=|-=|\\+\\+|--)`, 'gm');
    for (const m of text.matchAll(re)) {
        if (shadowed.has(m[2])) continue;
        const lineStart = text.lastIndexOf('\n', m.index) + 1;
        const before = text.slice(lineStart, m.index + m[1].length);
        // A declaration: a type name before the variable on the line.
        if (/\b(JSObjectHandle|JSFunctionBytecodeHandle|JSContextHandle|GCValue|int|GCHandle)\s*\**\s*$/.test(before))
            continue;
        throw new Error(`${op}: the body assigns frame local '${m[2]}': ${text.slice(lineStart, text.indexOf('\n', m.index))}`);
    }
}

// --- Emit ------------------------------------------------------------------
const allOps = [];
for (const g of groups) for (const op of g.ops) if (op !== 'DEFAULT') allOps.push(op);

function closureFor(gi) {
    const set = new Set([gi]);
    const work = [gi];
    while (work.length) {
        const i = work.pop();
        const add = j => { if (!set.has(j)) { set.add(j); work.push(j); } };
        for (const l of groups[i].gotos) if (!EXITS.has(l)) add(labelOwner.get(l));
        if (groups[i].fallsThrough) add(i + 1);
    }
    return [...set].sort((a, b) => a - b);
}

const out = [];
out.push(`/* Generated by tools/gen_jsops.js from JS_CallInternal in quickjs.c.
 * Do not edit: change the interpreter and run \`node tools/gen_jsops.js\`.
 *
 * One helper per opcode, each running one instruction over a JSInterpFrame
 * with the interpreter's own code (see the generator for how). */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-label"
#pragma GCC diagnostic ignored "-Woverride-init"

#undef CASE
#undef DEFAULT
#undef BREAK
#define CASE(op)  case op
#define DEFAULT   default
#define BREAK     goto jsop_next
`);

for (const op of allOps) {
    const gi = groups.findIndex(g => g.ops.includes(op));
    const members = closureFor(gi);
    const text = members.map(i => groups[i].text).join('\n');
    checkAssignments(op, text);
    const name = op.replace(/^OP_/, '');
    const fn = [];
    fn.push(`static JSOP_HELPER int jsop_${name}(JSInterpFrame *F)`);
    fn.push('{');
    for (const [t, n] of WRITTEN) fn.push(`    ${t}${t.endsWith('*') ? '' : ' '}${n} = F->${n};`);
    for (const [t, n] of READ) fn.push(`    ${t}${t.endsWith('*') ? '' : ' '}const ${n} = F->${n};`.replace('* const', '*const'));
    fn.push(`    int opcode = ${op};`);
    fn.push('    int i, call_argc;');
    fn.push('    GCValue *call_argv;');
    fn.push('    (void)i; (void)call_argc; (void)call_argv;');
    fn.push('    ' + READ.map(r => `(void)${r[1]};`).join(' '));
    fn.push('    switch (opcode) {');
    fn.push(text);
    fn.push('    }');
    fn.push(' jsop_next: __attribute__((unused));');
    fn.push('    F->sp = sp; F->pc = pc; F->ret_val = ret_val;');
    fn.push('    return JSOP_NEXT;');
    fn.push(' exception: __attribute__((unused));');
    fn.push('    F->sp = sp; F->pc = pc; F->ret_val = ret_val;');
    fn.push('    return JSOP_EXCEPTION;');
    fn.push(' done: __attribute__((unused));');
    fn.push('    F->sp = sp; F->pc = pc; F->ret_val = ret_val;');
    fn.push('    return JSOP_DONE;');
    fn.push(' done_generator: __attribute__((unused));');
    fn.push('    F->sp = sp; F->pc = pc; F->ret_val = ret_val;');
    fn.push('    return JSOP_DONE_GENERATOR;');
    fn.push('}');
    out.push(fn.join('\n') + '\n');
}

// Helpers that can leave pc anywhere but after their operands (a jump):
// a body that sets pc, or adds to it anything but a constant.
const jumps = allOps.filter(op => {
    const gi = groups.findIndex(g => g.ops.includes(op));
    const text = closureFor(gi).map(i => groups[i].text).join('\n')
        .replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*/g, '');
    return /(^|[^\w.>])pc\s*=[^=]/.test(text) || /(^|[^\w.>])pc\s*\+=(?!\s*\d+\s*;)/.test(text) ||
           /(^|[^\w.>])pc\s*-=/.test(text);
});
out.push('/* Opcodes whose helper may jump (leave pc other than past its operands). */');
out.push('static const uint8_t jsop_jumps[256] = {');
for (const op of jumps) out.push(`    [${op}] = 1,`);
out.push('};\n');

out.push('/* By opcode; opcodes this build does not have run OP_invalid. */');
out.push('static int (*const jsop_table[256])(JSInterpFrame *F) = {');
out.push('    [0 ... 255] = jsop_invalid,');
for (const op of allOps) out.push(`    [${op}] = jsop_${op.replace(/^OP_/, '')},`);
out.push('};\n');
out.push('#undef CASE\n#undef DEFAULT\n#undef BREAK\n\n#pragma GCC diagnostic pop\n');

const generated = out.join('\n');
if (process.argv.includes('--check')) {
    const have = fs.existsSync(outPath) ? fs.readFileSync(outPath, 'utf8').replace(/\r\n/g, '\n') : '';
    if (have !== generated) {
        console.error(`${path.relative(process.cwd(), outPath)} is out of date: run node tools/gen_jsops.js`);
        process.exit(1);
    }
} else {
    fs.writeFileSync(outPath, generated);
    console.log(`wrote ${outPath}: ${allOps.length} helpers from ${groups.length} case groups`);
}

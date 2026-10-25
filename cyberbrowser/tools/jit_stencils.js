#!/usr/bin/env node
// Build the baseline JIT's stencils (JIT_DESIGN.md section 3.2): compile
// src/quickjs/quickjs_jit_ops.c with clang, read the COFF object's
// functions, relocations and unwind data, and write
// src/quickjs/jit_stencils_x86_64_windows.h.
//
//   node tools/jit_stencils.js            regenerate (needs clang on PATH)
//   node tools/jit_stencils.js --check    fail if the header is not current
//   ... --value8                           the same for 8-byte values (QJS_VALUE8):
//                                          jit_stencils_x86_64_windows_value8.h
//
// Each function jit_op_<name> becomes stencil <name>: its bytes, its holes
// (every relocation, all rip-relative 32-bit) and its unwind ranges.  The
// header also records clang's layout of the structures the stencils read,
// which quickjs_jit.c checks against gcc's.
'use strict';
const fs = require('fs');
const path = require('path');
const os = require('os');
const { execFileSync } = require('child_process');

const root = path.join(__dirname, '..', 'src', 'quickjs');
const srcPath = path.join(root, 'quickjs_jit_ops.c');
const VALUE8 = process.argv.includes('--value8');
const outPath = path.join(root, VALUE8 ? 'jit_stencils_x86_64_windows_value8.h' : 'jit_stencils_x86_64_windows.h');
const TARGET = 'x86_64-w64-windows-gnu';
const CFLAGS = ['-target', TARGET, '-O2', '-std=gnu11', '-fno-stack-protector', '-ffunction-sections',
                '-fno-jump-tables', '-fno-builtin', '-fno-asynchronous-unwind-tables', '-funwind-tables',
                '-mno-red-zone', '-I', root, '-I', path.join(root, '..'), '-w',
                ...(VALUE8 ? ['-DQJS_VALUE8'] : [])];

// The structure fields stencils use, whose offsets gcc must agree on.
const LAYOUT = [
    'sizeof(GCValue)', 'sizeof(JSInterpFrame)',
    ...['sp', 'pc', 'ret_val', 'caller_ctx', 'ctx', 'rt', 'p', 'b', 'sf', 'stack_buf', 'var_buf', 'arg_buf',
        'var_refs', 'var_refs_handle', 'sf_var_refs', 'func_obj', 'this_obj', 'new_target', 'argc', 'argv',
        'flags', 'code', 'byte_code', 'jit_exit'].map(f => `offsetof(JSInterpFrame, ${f})`),
    'sizeof(JSStackFrame)', 'offsetof(JSStackFrame, arg_buf_offset)', 'offsetof(JSStackFrame, var_buf_offset)',
    'offsetof(JSStackFrame, pc_offset)',
    'sizeof(JSVarRef)', 'offsetof(JSVarRef, is_detached)', 'offsetof(JSVarRef, is_arg)', 'offsetof(JSVarRef, var_idx)',
    'offsetof(JSVarRef, frame_ref)',
    'offsetof(GCState, heap_base)', 'offsetof(GCState, heap_reserved)', 'offsetof(GCState, cards)',
];

function compile() {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'jitst-'));
    const probe = path.join(tmp, 'layout.c');
    fs.writeFileSync(probe, `#include "${srcPath.replace(/\\/g, '/')}"
#include <stddef.h>
const long long jit_layout_values[] = { ${LAYOUT.join(', ')} };
`);
    const obj = path.join(tmp, 'ops.o');
    execFileSync('clang', [...CFLAGS, '-c', probe, '-o', obj], { stdio: ['ignore', 'inherit', 'inherit'] });
    const buf = fs.readFileSync(obj);
    const dis = execFileSync('llvm-objdump', ['-d', '--no-show-raw-insn', obj], { encoding: 'utf8', maxBuffer: 1 << 26 });
    fs.rmSync(tmp, { recursive: true, force: true });
    return { buf, starts: insnStarts(dis) };
}

// Instruction start offsets of each function, from the disassembly: the
// JIT may rewrite an instruction that reads a literal only knowing where
// it begins.
function insnStarts(dis) {
    const starts = new Map();
    let cur = null;
    for (const line of dis.split(/\r?\n/)) {
        let m = line.match(/^[0-9a-f]+ <(\w+)>:$/);
        if (m) { cur = []; starts.set(m[1], cur); continue; }
        m = line.match(/^\s+([0-9a-f]+):\s/);
        if (m && cur) cur.push(parseInt(m[1], 16));
    }
    return starts;
}

// --- COFF ------------------------------------------------------------------
function readCoff(buf) {
    const nsec = buf.readUInt16LE(2), symPtr = buf.readUInt32LE(8), nsym = buf.readUInt32LE(12);
    const optSize = buf.readUInt16LE(16);
    const strtab = symPtr + nsym * 18;
    const str = off => { const e = buf.indexOf(0, strtab + off); return buf.toString('latin1', strtab + off, e); };
    const name8 = (o) => {
        if (buf.readUInt32LE(o) === 0) return str(buf.readUInt32LE(o + 4));
        const raw = buf.toString('latin1', o, o + 8);
        const z = raw.indexOf('\0');
        return z < 0 ? raw : raw.slice(0, z);
    };
    const sections = [];
    for (let i = 0; i < nsec; i++) {
        const o = 20 + optSize + i * 40;
        let name = name8(o);
        if (name.startsWith('/')) name = str(Number(name.slice(1)));
        const size = buf.readUInt32LE(o + 16), ptr = buf.readUInt32LE(o + 20);
        const relPtr = buf.readUInt32LE(o + 24), nrel = buf.readUInt16LE(o + 32);
        const relocs = [];
        for (let r = 0; r < nrel; r++) {
            const ro = relPtr + r * 10;
            relocs.push({ offset: buf.readUInt32LE(ro), sym: buf.readUInt32LE(ro + 4), type: buf.readUInt16LE(ro + 8) });
        }
        sections.push({ index: i + 1, name, data: ptr ? buf.subarray(ptr, ptr + size) : Buffer.alloc(size), relocs });
    }
    const symbols = [];
    for (let i = 0; i < nsym; i++) {
        const o = symPtr + i * 18;
        symbols[i] = { name: name8(o), value: buf.readUInt32LE(o + 8), section: buf.readInt16LE(o + 12),
                       storage: buf.readUInt8(o + 16) };
        i += buf.readUInt8(o + 17);
    }
    return { sections, symbols };
}

const IMAGE_REL_AMD64_ADDR64 = 1, IMAGE_REL_AMD64_ADDR32NB = 3, IMAGE_REL_AMD64_REL32 = 4;
const BRANCHES = ['continue', 'target', 'slow', 'exit', 'dyn'];

function build() {
    const { buf, starts } = compile();
    const { sections, symbols } = readCoff(buf);
    const secByName = new Map(sections.map(s => [s.name, s]));
    const extSyms = [];          // symbols resolved at run time, by name
    const extIndex = name => { let i = extSyms.indexOf(name); if (i < 0) { i = extSyms.length; extSyms.push(name); } return i; };
    const datas = [];            // copied read-only data sections
    const stencils = [];

    // Layout values.
    const layoutSym = symbols.find(s => s && s.name === 'jit_layout_values');
    const layoutSec = sections[layoutSym.section - 1];
    const layout = LAYOUT.map((e, i) => Number(layoutSec.data.readBigInt64LE(layoutSym.value + i * 8)));

    for (const sec of sections) {
        const m = sec.name.match(/^\.text\$jit_(?:op_)?(\w+)$/);
        if (!m) continue;
        const name = sec.name === '.text$jit_trampoline' ? 'trampoline' : m[1];
        const code = Buffer.from(sec.data);
        const fnStarts = starts.get(sec.name.slice('.text$'.length)) || [];
        const holes = [];
        for (const r of sec.relocs) {
            const sym = symbols[r.sym];
            if (r.type < IMAGE_REL_AMD64_REL32 || r.type > 9)
                throw new Error(`${name}: relocation type ${r.type} at ${r.offset} (${sym.name})`);
            // REL32_k: the field ends k bytes before the instruction does.
            const extra = r.type - IMAGE_REL_AMD64_REL32;
            const addend = code.readInt32LE(r.offset);
            code.writeInt32LE(0, r.offset);
            let hole;
            let mm;
            if ((mm = sym.name.match(/^\.refptr\.__jit_imm(\d)$/))) hole = { kind: 'IMM', arg: Number(mm[1]) };
            else if ((mm = sym.name.match(/^\.refptr\.(.+)$/))) hole = { kind: 'ADDR_LIT', arg: extIndex(mm[1]) };
            else if ((mm = sym.name.match(/^__jit_(\w+)$/)) && BRANCHES.includes(mm[1])) hole = { kind: 'BRANCH', arg: BRANCHES.indexOf(mm[1]) };
            else if (sym.name === '__jit_helper') hole = { kind: 'HELPER', arg: 0 };
            else if (sym.section === 0) hole = { kind: 'CALL', arg: extIndex(sym.name) };
            else {
                // A local data section (constants).
                const ds = sections[sym.section - 1];
                if (ds.relocs.length) throw new Error(`${name}: data section ${ds.name} has relocations`);
                let di = datas.findIndex(d => d.name === ds.name);
                if (di < 0) { di = datas.length; datas.push({ name: ds.name, data: ds.data }); }
                hole = { kind: 'DATA', arg: di };
            }
            hole.offset = r.offset;
            hole.addend = addend + (sym.section > 0 && hole.kind === 'DATA' ? sym.value : 0);
            hole.extra = extra;
            // Bytes of its instruction before the field, when the field
            // ends the instruction (0: unknown, never rewritten).
            const start = fnStarts.filter(s => s <= r.offset).pop();
            const next = fnStarts.find(s => s > r.offset);
            hole.pre = start !== undefined && (next === undefined ? code.length : next) === r.offset + 4 ? r.offset - start : 0;
            holes.push(hole);
        }
        holes.sort((a, b) => a.offset - b.offset);
        // A final `jmp continue` (E9 rel32) the JIT can cut when the next
        // stencil follows.
        const last = holes[holes.length - 1];
        const tailContinue = last && last.kind === 'BRANCH' && last.arg === 0 && last.offset === code.length - 4 &&
            code[code.length - 5] === 0xE9;
        // Unwind ranges: .pdata entries for this function.
        const unwind = [];
        const pdata = secByName.get(sec.name.replace('.text$', '.pdata$'));
        let xdata = null;
        if (pdata) {
            const xsec = secByName.get(sec.name.replace('.text$', '.xdata$'));
            xdata = xsec.data;
            if (xsec.relocs.length) throw new Error(`${name}: unwind info with relocations (a handler?)`);
            for (let e = 0; e < pdata.data.length; e += 12) {
                const field = k => {
                    const rel = pdata.relocs.find(r => r.offset === e + k);
                    if (!rel || rel.type !== IMAGE_REL_AMD64_ADDR32NB) throw new Error(`${name}: odd .pdata`);
                    const s = symbols[rel.sym];
                    return { sec: sections[s.section - 1].name, off: pdata.data.readUInt32LE(e + k) + s.value };
                };
                const b = field(0), en = field(4), u = field(8);
                if (b.sec !== sec.name || en.sec !== sec.name) throw new Error(`${name}: .pdata for another function`);
                unwind.push({ begin: b.off, end: en.off, info: u.off });
            }
        }
        stencils.push({ name, code, holes, tailContinue, unwind, xdata });
    }
    stencils.sort((a, b) => (a.name === 'trampoline' ? -1 : b.name === 'trampoline' ? 1 : a.name < b.name ? -1 : 1));
    return { stencils, extSyms, datas, layout };
}

const hex = b => [...b].map(x => '0x' + x.toString(16).padStart(2, '0')).join(',');
function wrap(s, w = 100) {
    const out = [];
    for (let i = 0; i < s.length; i += w) {
        let j = Math.min(s.length, i + w);
        while (j < s.length && s[j - 1] !== ',') j++;
        out.push('    ' + s.slice(i, j));
        i = j - w;
    }
    return out.join('\n');
}

function emit({ stencils, extSyms, datas, layout }) {
    const L = [];
    L.push(`/* Generated by tools/jit_stencils.js from quickjs_jit_ops.c (clang, ${TARGET}).
 * Do not edit: change the stencils and run \`node tools/jit_stencils.js\`. */
#ifndef JIT_STENCILS_X86_64_WINDOWS_H
#define JIT_STENCILS_X86_64_WINDOWS_H

/* Symbols the stencils use, resolved by name at run time. */
#define JIT_SYMBOLS(X) ${extSyms.map(s => `X(${s})`).join(' ')}
enum { ${extSyms.map(s => `JIT_SYM_${s}`).join(', ')}, JIT_SYM_COUNT };

/* clang's layout of what the stencils read (quickjs_jit.c checks gcc's). */
#define JIT_LAYOUT(X) \\
${LAYOUT.map((e, i) => `    X(${e}, ${layout[i]})`).join(' \\\n')}
`);
    datas.forEach((d, i) => L.push(`static const uint8_t jit_data_${i}[${d.data.length}] __attribute__((aligned(16))) = {\n${wrap(hex(d.data))}\n};`));
    L.push(`static const JitData jit_datas[] = { ${datas.map((d, i) => `{ jit_data_${i}, ${d.data.length} }`).join(', ')} };`);
    L.push('');
    L.push(`enum { ${stencils.map(s => `JIT_ST_${s.name}`).join(', ')}, JIT_ST_COUNT };`);
    for (const s of stencils) {
        L.push(`static const uint8_t jit_code_${s.name}[${s.code.length}] = {\n${wrap(hex(s.code))}\n};`);
        L.push(`static const JitHole jit_holes_${s.name}[] = {\n${s.holes.map(h =>
            `    { ${h.offset}, JIT_HOLE_${h.kind}, ${h.arg}, ${h.addend}, ${h.extra}, ${h.pre} },`).join('\n')}\n    { 0, JIT_HOLE_END, 0, 0, 0, 0 }\n};`);
        if (s.unwind.length) {
            L.push(`static const uint8_t jit_xdata_${s.name}[${s.xdata.length}] = { ${hex(s.xdata)} };`);
            L.push(`static const JitUnwind jit_unwind_${s.name}[] = { ${s.unwind.map(u => `{ ${u.begin}, ${u.end}, ${u.info} }`).join(', ')} };`);
        }
    }
    L.push('');
    L.push('static const JitStencil jit_stencils[JIT_ST_COUNT] = {');
    for (const s of stencils) {
        const u = s.unwind.length ? `jit_xdata_${s.name}, ${s.xdata.length}, jit_unwind_${s.name}, ${s.unwind.length}` : 'NULL, 0, NULL, 0';
        L.push(`    [JIT_ST_${s.name}] = { "${s.name}", jit_code_${s.name}, ${s.code.length}, jit_holes_${s.name}, ${s.tailContinue ? 1 : 0}, ${u} },`);
    }
    L.push('};\n\n#endif\n');
    return L.join('\n');
}

const text = emit(build());
if (process.argv.includes('--check')) {
    const have = fs.existsSync(outPath) ? fs.readFileSync(outPath, 'utf8').replace(/\r\n/g, '\n') : '';
    if (have !== text) { console.error(`${outPath} is out of date: run node tools/jit_stencils.js`); process.exit(1); }
} else {
    fs.writeFileSync(outPath, text);
    console.log(`wrote ${outPath}`);
}

// gen_webgl.js: WebGL's calls, from one table, for both ends of a tab's gl
// channel (SANDBOX_DESIGN.md section 5.7):
//   src/proc/webgl_gen.h, src/proc/webgl_gen.c   the web-GPU process: the
//       GLES entry points it loads, and a decoder that checks each record's
//       arguments and makes the call;
//   src/browser/webgl_gen.js                      the tab: a method per call
//       that encodes it as a record (browser/webgl_api.js adds the rest).
// Calls with overloads, WebGL-only behavior or results are hand-written in
// webgl_api.js and webgl_backend.c; this table is the plain ones.
//
//   node tools/gen_webgl.js
//
// Argument kinds: e enum, i int, u uint, f float, b boolean, z size (int),
// p intptr (64-bit), and o<kind> an object id the tab gave out (b buffer,
// f framebuffer, r renderbuffer, t texture, s shader, p program, l uniform
// location, v vertex array, q query, m sampler, y sync, x transform
// feedback).  Ids are the tab's own and mapped per context: an id it never
// made reaches GLES as a name it never generated, which ANGLE's WebGL
// compatibility mode refuses.
const fs = require('fs');
const path = require('path');
const root = path.join(__dirname, '..');

// [JS name, WebGL version (1 or 2), GLES function, argument kinds]
const CALLS = [
    ['activeTexture', 1, 'glActiveTexture', 'e'],
    ['attachShader', 1, 'glAttachShader', 'op os'],
    ['bindBuffer', 1, 'glBindBuffer', 'e ob'],
    ['bindFramebuffer', 1, 'glBindFramebuffer', 'e of'],
    ['bindRenderbuffer', 1, 'glBindRenderbuffer', 'e or'],
    ['bindTexture', 1, 'glBindTexture', 'e ot'],
    ['blendColor', 1, 'glBlendColor', 'f f f f'],
    ['blendEquation', 1, 'glBlendEquation', 'e'],
    ['blendEquationSeparate', 1, 'glBlendEquationSeparate', 'e e'],
    ['blendFunc', 1, 'glBlendFunc', 'e e'],
    ['blendFuncSeparate', 1, 'glBlendFuncSeparate', 'e e e e'],
    ['clear', 1, 'glClear', 'u'],
    ['clearColor', 1, 'glClearColor', 'f f f f'],
    ['clearDepth', 1, 'glClearDepthf', 'f'],
    ['clearStencil', 1, 'glClearStencil', 'i'],
    ['colorMask', 1, 'glColorMask', 'b b b b'],
    ['compileShader', 1, 'glCompileShader', 'os'],
    ['copyTexImage2D', 1, 'glCopyTexImage2D', 'e i e i i z z i'],
    ['copyTexSubImage2D', 1, 'glCopyTexSubImage2D', 'e i i i i i z z'],
    ['cullFace', 1, 'glCullFace', 'e'],
    ['depthFunc', 1, 'glDepthFunc', 'e'],
    ['depthMask', 1, 'glDepthMask', 'b'],
    ['depthRange', 1, 'glDepthRangef', 'f f'],
    ['detachShader', 1, 'glDetachShader', 'op os'],
    ['disable', 1, 'glDisable', 'e'],
    ['disableVertexAttribArray', 1, 'glDisableVertexAttribArray', 'u'],
    ['drawArrays', 1, 'glDrawArrays', 'e i z'],
    ['drawElements', 1, 'glDrawElements', 'e z e p'],
    ['enable', 1, 'glEnable', 'e'],
    ['enableVertexAttribArray', 1, 'glEnableVertexAttribArray', 'u'],
    ['finish', 1, 'glFinish', ''],
    ['flush', 1, 'glFlush', ''],
    ['framebufferRenderbuffer', 1, 'glFramebufferRenderbuffer', 'e e e or'],
    ['framebufferTexture2D', 1, 'glFramebufferTexture2D', 'e e e ot i'],
    ['frontFace', 1, 'glFrontFace', 'e'],
    ['generateMipmap', 1, 'glGenerateMipmap', 'e'],
    ['hint', 1, 'glHint', 'e e'],
    ['lineWidth', 1, 'glLineWidth', 'f'],
    ['linkProgram', 1, 'glLinkProgram', 'op'],
    ['polygonOffset', 1, 'glPolygonOffset', 'f f'],
    ['renderbufferStorage', 1, 'glRenderbufferStorage', 'e e z z'],
    ['sampleCoverage', 1, 'glSampleCoverage', 'f b'],
    ['scissor', 1, 'glScissor', 'i i z z'],
    ['stencilFunc', 1, 'glStencilFunc', 'e i u'],
    ['stencilFuncSeparate', 1, 'glStencilFuncSeparate', 'e e i u'],
    ['stencilMask', 1, 'glStencilMask', 'u'],
    ['stencilMaskSeparate', 1, 'glStencilMaskSeparate', 'e u'],
    ['stencilOp', 1, 'glStencilOp', 'e e e'],
    ['stencilOpSeparate', 1, 'glStencilOpSeparate', 'e e e e'],
    ['texParameterf', 1, 'glTexParameterf', 'e e f'],
    ['texParameteri', 1, 'glTexParameteri', 'e e i'],
    ['uniform1f', 1, 'glUniform1f', 'ol f'],
    ['uniform2f', 1, 'glUniform2f', 'ol f f'],
    ['uniform3f', 1, 'glUniform3f', 'ol f f f'],
    ['uniform4f', 1, 'glUniform4f', 'ol f f f f'],
    ['uniform1i', 1, 'glUniform1i', 'ol i'],
    ['uniform2i', 1, 'glUniform2i', 'ol i i'],
    ['uniform3i', 1, 'glUniform3i', 'ol i i i'],
    ['uniform4i', 1, 'glUniform4i', 'ol i i i i'],
    ['useProgram', 1, 'glUseProgram', 'op'],
    ['validateProgram', 1, 'glValidateProgram', 'op'],
    ['vertexAttrib1f', 1, 'glVertexAttrib1f', 'u f'],
    ['vertexAttrib2f', 1, 'glVertexAttrib2f', 'u f f'],
    ['vertexAttrib3f', 1, 'glVertexAttrib3f', 'u f f f'],
    ['vertexAttrib4f', 1, 'glVertexAttrib4f', 'u f f f f'],
    ['vertexAttribPointer', 1, 'glVertexAttribPointer', 'u i e b z p'],
    ['viewport', 1, 'glViewport', 'i i z z'],
    // WebGL 2
    ['beginQuery', 2, 'glBeginQuery', 'e oq'],
    ['beginTransformFeedback', 2, 'glBeginTransformFeedback', 'e'],
    ['bindBufferBase', 2, 'glBindBufferBase', 'e u ob'],
    ['bindBufferRange', 2, 'glBindBufferRange', 'e u ob p p'],
    ['bindSampler', 2, 'glBindSampler', 'u om'],
    ['bindTransformFeedback', 2, 'glBindTransformFeedback', 'e ox'],
    ['bindVertexArray', 2, 'glBindVertexArray', 'ov'],
    ['blitFramebuffer', 2, 'glBlitFramebuffer', 'i i i i i i i i u e'],
    ['clearBufferfi', 2, 'glClearBufferfi', 'e i f i'],
    ['copyBufferSubData', 2, 'glCopyBufferSubData', 'e e p p p'],
    ['copyTexSubImage3D', 2, 'glCopyTexSubImage3D', 'e i i i i i i z z'],
    ['drawArraysInstanced', 2, 'glDrawArraysInstanced', 'e i z z'],
    ['drawElementsInstanced', 2, 'glDrawElementsInstanced', 'e z e p z'],
    ['drawRangeElements', 2, 'glDrawRangeElements', 'e u u z e p'],
    ['endQuery', 2, 'glEndQuery', 'e'],
    ['endTransformFeedback', 2, 'glEndTransformFeedback', ''],
    ['framebufferTextureLayer', 2, 'glFramebufferTextureLayer', 'e e ot i i'],
    ['pauseTransformFeedback', 2, 'glPauseTransformFeedback', ''],
    ['readBuffer', 2, 'glReadBuffer', 'e'],
    ['renderbufferStorageMultisample', 2, 'glRenderbufferStorageMultisample', 'e z e z z'],
    ['resumeTransformFeedback', 2, 'glResumeTransformFeedback', ''],
    ['samplerParameterf', 2, 'glSamplerParameterf', 'om e f'],
    ['samplerParameteri', 2, 'glSamplerParameteri', 'om e i'],
    ['texStorage2D', 2, 'glTexStorage2D', 'e z e z z'],
    ['texStorage3D', 2, 'glTexStorage3D', 'e z e z z z'],
    ['uniform1ui', 2, 'glUniform1ui', 'ol u'],
    ['uniform2ui', 2, 'glUniform2ui', 'ol u u'],
    ['uniform3ui', 2, 'glUniform3ui', 'ol u u u'],
    ['uniform4ui', 2, 'glUniform4ui', 'ol u u u u'],
    ['uniformBlockBinding', 2, 'glUniformBlockBinding', 'op u u'],
    ['vertexAttribDivisor', 2, 'glVertexAttribDivisor', 'u u'],
    ['vertexAttribI4i', 2, 'glVertexAttribI4i', 'u i i i i'],
    ['vertexAttribI4ui', 2, 'glVertexAttribI4ui', 'u u u u u'],
    ['vertexAttribIPointer', 2, 'glVertexAttribIPointer', 'u i e z p'],
];

const CTYPE = { e: 'uint32_t', i: 'int32_t', u: 'uint32_t', f: 'float', b: 'uint32_t', z: 'int32_t', p: 'int64_t' };
const GLTYPE = { e: 'GLenum', i: 'GLint', u: 'GLuint', f: 'GLfloat', b: 'GLboolean', z: 'GLsizei', p: 'GLintptr' };
const SIZE = { e: 4, i: 4, u: 4, f: 4, b: 4, z: 4, p: 8 };
const OBJ = { b: 'BUFFER', f: 'FRAMEBUFFER', r: 'RENDERBUFFER', t: 'TEXTURE', s: 'SHADER', p: 'PROGRAM',
              l: 'LOCATION', v: 'VERTEX_ARRAY', q: 'QUERY', m: 'SAMPLER', y: 'SYNC', x: 'TRANSFORM_FEEDBACK' };
const FIRST_OP = 1;

const args = c => c[3] ? c[3].split(' ') : [];
const size = c => args(c).reduce((n, a) => n + (a[0] === 'o' ? 4 : SIZE[a]), 0);
const head = '/* Generated by tools/gen_webgl.js from its table: do not edit. */\n';

// ---- the web-GPU process -------------------------------------------------------
let h = head + `#ifndef WEBGL_GEN_H
#define WEBGL_GEN_H

/* The plain WebGL calls: record ops ${FIRST_OP}..${FIRST_OP + CALLS.length - 1} (webgl_backend.c has the
 * rest, from WEBGL_OP_SPECIAL). */
enum {
`;
CALLS.forEach((c, i) => { h += `    WEBGL_OP_${c[0].toUpperCase()} = ${FIRST_OP + i},\n`; });
h += `    WEBGL_OP_SPECIAL = 256,
};

/* Their arguments' lengths, by op (0: not a plain call): the grammar the
 * proved walker (webgl_decode.c) checks each record against. */
#define WEBGL_PLAIN_OPS ${FIRST_OP + CALLS.length - 1}
#define WEBGL_PLAIN_LENS { ${[...Array(FIRST_OP).fill(0), ...CALLS.map(size)].join(', ')} }

/* The GLES entry points the plain calls use (X(name, return, parameters)). */
#define WEBGL_GEN_PROCS(X) \\
`;
const procs = [];
for (const c of CALLS) {
    if (procs.some(p => p[0] === c[2])) continue;
    const ps = args(c).map(a => a[0] === 'o' ? (a[1] === 'l' ? 'GLint' : 'GLuint') : GLTYPE[a]);
    procs.push([c[2], ps]);
}
h += procs.map(p => `    X(${p[0]}, void, (${p[1].join(', ') || 'void'}))`).join(' \\\n') + '\n\n#endif\n';

let c = head + `#include "webgl_backend.h"

/* One plain call's record (its arguments, little-endian, n bytes): false if
 * the op isn't a plain one or its length is wrong. */
bool webgl_exec_plain(WebglCtx *c, uint32_t op, const uint8_t *p, uint32_t n) {
    switch (op) {
`;
CALLS.forEach((call, i) => {
    const as = args(call);
    c += `    case ${FIRST_OP + i}: {    /* ${call[0]} */\n`;
    c += `        if (n != ${size(call)}u) return false;\n`;
    let off = 0;
    const names = [];
    as.forEach((a, k) => {
        const v = `a${k}`;
        if (a[0] === 'o') {
            if (a[1] === 'l') c += `        GLint ${v} = webgl_location(c, rd_u32(p + ${off}));\n`;
            else c += `        GLuint ${v} = webgl_name(c, WEBGL_${OBJ[a[1]]}, rd_u32(p + ${off}));\n`;
            off += 4;
        } else {
            const rd = a === 'f' ? 'rd_f32' : a === 'p' ? 'rd_i64' : a === 'i' || a === 'z' ? 'rd_i32' : 'rd_u32';
            c += `        ${CTYPE[a]} ${v} = ${rd}(p + ${off});\n`;
            off += SIZE[a];
        }
        names.push(a === 'b' ? `(GLboolean)(${v} != 0)` : a === 'p' ? `(GLintptr)${v}` : v);
    });
    if (as.some(a => a === 'ol')) c += `        if (!webgl_location_ok(c, rd_u32(p))) return true;   /* INVALID_OPERATION noted */\n`;
    c += `        gl.${call[2]}(${names.join(', ')});\n        return true;\n    }\n`;
});
c += `    default:
        return false;
    }
}
`;

// ---- the tab -----------------------------------------------------------------
let js = `/* Generated by tools/gen_webgl.js from its table: do not edit.
 * The plain WebGL calls, as methods that encode a record; installed by
 * browser/webgl_api.js as gen(P1, P2, rec, id), where P1 and P2 are the
 * WebGL 1 and 2 prototypes, rec(ctx, op, kinds, args) encodes, and id(ctx,
 * kind, object) checks an object and gives its id. */
(function gen(P1, P2, rec, id) {
`;
CALLS.forEach((call, i) => {
    const as = args(call);
    const params = as.map((a, k) => `a${k}`);
    const kinds = as.map(a => a[0] === 'o' ? 'u' : a).join('');
    const vals = as.map((a, k) => a[0] === 'o' ? `id(this, '${a[1]}', a${k})` : `a${k}`);
    js += `    (${call[1] === 1 ? 'P1' : 'P2'}).${call[0]} = function ${call[0]}(${params.join(', ')}) { rec(this, ${FIRST_OP + i}, '${kinds}', [${vals.join(', ')}]); };\n`;
});
js += `})`;

fs.writeFileSync(path.join(root, 'src/proc/webgl_gen.h'), h);
fs.writeFileSync(path.join(root, 'src/proc/webgl_gen.c'), c);
fs.writeFileSync(path.join(root, 'src/browser/webgl_gen.js'), js);
console.log(`${CALLS.length} plain calls, ${procs.length} GLES entry points`);

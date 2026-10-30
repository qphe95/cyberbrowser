/* webgpu_fuzz.c - the web-GPU process's record decoders under random input
 * (SANDBOX_DESIGN.md section 5.7, M8): WebGL records into webgl_run on a
 * real ANGLE context, WebGPU records into wgpu_run on a real wgpu device,
 * in this process (the libraries from webgpu\ next to it), as a tab may send
 * them.  Records are random but shaped: real ops, lengths that are right or
 * lie, words drawn from the enums, ids and edge values the decoders meet,
 * value trees of the keys and enum strings WebGPU uses.  After each batch
 * the context must still work: a clear reads back as the color cleared.
 *
 *   cb_webgpu_fuzz gl|gpu|gpuseq [batches=2000] [seed=1]
 *   cb_webgpu_fuzz replay <file> [skip from] [skip to]
 *
 * gpuseq makes well-formed calls on real objects, used right and wrong (see
 * below).  When a fresh client there fails, the client before it is written
 * to fuzz_prev.bin; replay runs it again, REPLAY_TIMES rounds, only the
 * batches REPLAY_ONLY=a-b, printing them with REPLAY_DUMP=1.  FUZZ_TRACE=1
 * prints each batch's ops before it runs (to find what a crash was).
 *
 * Exits 0 if every check passed; a crash is the failure it hunts. */
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "proc/webgl_backend.h"
#include "proc/webgl_gen.h"
#include "proc/webgpu_wgpu.h"
#include "proc/webgpu_wire.h"

static uint64_t g_rand;
static uint32_t rnd(void) {
    g_rand ^= g_rand << 13;
    g_rand ^= g_rand >> 7;
    g_rand ^= g_rand << 17;
    return (uint32_t)(g_rand >> 11);
}
static uint32_t below(uint32_t n) { return n ? rnd() % n : 0; }

/* ---- a record buffer ---- */
static uint8_t *g_buf;
static uint32_t g_len, g_cap;
static void put(const void *p, uint32_t n) {
    if (g_len + n > g_cap) {
        g_cap = (g_len + n) * 2 + 4096;
        g_buf = (uint8_t *)realloc(g_buf, g_cap);
    }
    memcpy(g_buf + g_len, p, n);
    g_len += n;
}
static void u32(uint32_t v) { put(&v, 4); }
static void f64(double v) { put(&v, 8); }
static void u8(uint8_t v) { put(&v, 1); }

static const uint32_t EDGES[] = { 0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 255, 256, 1023, 1024, 4096, 16384,
                                  32767, 65535, 65536, 0x7fff0000u, 0x7fffffffu, 0x80000000u, 0xfffffff0u,
                                  0xfffffffeu, 0xffffffffu };
/* Enums WebGL calls take (targets, formats, types, pnames, caps). */
static const uint32_t ENUMS[] = {
    0x0DE1, 0x8513, 0x8515, 0x806F, 0x8C1A, 0x8892, 0x8893, 0x8A11, 0x8C8E, 0x88EB, 0x88EC, 0x8F36, 0x8F37, 0x8D40,
    0x8CA8, 0x8CA9, 0x8D41, 0x8CE0, 0x8CE1, 0x8D00, 0x8D20, 0x821A, 0x1908, 0x1907, 0x1906, 0x1909, 0x190A, 0x1903,
    0x8227, 0x8058, 0x8051, 0x8D9F, 0x8814, 0x881A, 0x822E, 0x8D70, 0x8D7C, 0x81A5, 0x81A6, 0x8CAC, 0x88F0, 0x8CAD,
    0x8D48, 0x1401, 0x1400, 0x1403, 0x1402, 0x1405, 0x1404, 0x1406, 0x140B, 0x8D61, 0x8033, 0x8034, 0x8363, 0x84FA,
    0x8368, 0x8C3B, 0x8C3E, 0x8DAD, 0x0BE2, 0x0B44, 0x0B71, 0x0BD0, 0x8037, 0x809E, 0x80A0, 0x0C11, 0x0B90, 0x8C89,
    0x2800, 0x2801, 0x2802, 0x2803, 0x8072, 0x813A, 0x813B, 0x813C, 0x813D, 0x884C, 0x884D, 0x84FE, 0x2600, 0x2601,
    0x2700, 0x2703, 0x2901, 0x812F, 0x8370, 0x0000, 0x0001, 0x0004, 0x0005, 0x0006, 0x0002, 0x0003, 0x0300, 0x0302,
    0x0303, 0x8006, 0x800A, 0x8007, 0x8008, 0x0200, 0x0201, 0x0203, 0x0207, 0x1E00, 0x1E01, 0x150A, 0x0404, 0x0405,
    0x0408, 0x0900, 0x0901, 0x8B30, 0x8B31, 0x8B81, 0x8B82, 0x8B86, 0x8B89, 0x8B8D, 0x88E4, 0x88E8, 0x88E0, 0x88E9,
    0x0CF5, 0x0D05, 0x0CF2, 0x0CF3, 0x0CF4, 0x806E, 0x806D, 0x0D02, 0x0D03, 0x0D04, 0x8CA6, 0x0D33, 0x8869, 0x1F00,
    0x1F01, 0x1F02, 0x1F03, 0x8B8C, 0x9240, 0x9241, 0x9243, 0x8C2F, 0x8D6A, 0x8C88, 0x8E22, 0x8C8C, 0x8C8D, 0x9117,
    0x83F0, 0x83F1, 0x83F2, 0x83F3, 0x9274, 0x9278, 0x8E8C, 0x8DBB, 0x93B0, 0x1800, 0x1801, 0x1802, 0x0C02, 0x0405,
    0x4000, 0x0100, 0x0400, 0x4500, 0x8B5A, 0x8B5B };
static uint32_t word(void) {
    switch (below(10)) {
    case 0: case 1: case 2: return EDGES[below(sizeof(EDGES) / 4)];
    case 3: case 4: case 5: case 6: return ENUMS[below(sizeof(ENUMS) / 4)];
    case 7: return 0x84C0 + below(40);      /* TEXTURE0 + i */
    case 8: return rnd();
    default: return below(12);              /* small: ids, indices, counts, kinds */
    }
}
static float flt(void) {
    static const float F[] = { 0.0f, 1.0f, -1.0f, 0.5f, 1e30f, -1e30f, 1e-30f, 3.4e38f };
    if (below(4) == 0) { uint32_t b = rnd(); float f; memcpy(&f, &b, 4); return f; }   /* NaN and inf too */
    return F[below(8)];
}

/* ---- WebGL ---- */

/* The plain ops' lengths, found by asking webgl_exec_plain (with zeros). */
static uint32_t g_arity[102];
static void gl_arity(WebglCtx *c) {
    static const uint8_t zeros[64];
    for (uint32_t op = 1; op <= 101; op++)
        for (uint32_t n = 0; n <= 64; n += 4)
            if (webgl_exec_plain(c, op, zeros, n)) { g_arity[op] = n / 4; break; }
}

/* A special op or query as its grammar (webgl_forms) allows, so that the
 * executor behind the walker is reached: random words in its head, with
 * the count, flag or kind its form reads in range, and a tail of the
 * form's shape. */
static void gl_grammatical(uint32_t op) {
    WebglForm f = webgl_forms[op - WEBGL_OP_SPECIAL];
    uint32_t head[16] = { 0 }, tail = 0, cap = f.cap > 4096 ? 4096 : f.cap;
    for (uint32_t i = 0; i < f.head / 4 && i < 16; i++) head[i] = word();
    uint32_t *at = &head[f.at / 4];
    switch (f.form) {
    case F_DATA: tail = below(8) ? below(64) : below(9000); break;
    case F_STR: case F_NAMES: tail = below(48); break;
    case F_WORDS: tail = 4 * below(cap + 1); break;
    case F_COUNTED: *at = below(cap + 1); break;
    case F_HAS: case F_BUFDATA:
        *at = below(2);
        tail = *at ? below(64) : 0;
        if (f.form == F_BUFDATA) { if (*at) { head[2] = tail; head[3] = 0; } }
        break;
    case F_UNIFORM: *at = 1 + below(21); tail = 4 * webgl_uniform_per[*at] * below(4); break;
    case F_ATTRIB: *at = 1 + below(6); tail = 4 * webgl_attrib_per[*at]; break;
    default: break;
    }
    u32(op);
    u32(f.head + tail);
    put(head, f.head);
    if (f.form == F_STR || f.form == F_NAMES) {
        static const char *SRC[] = { "void main(){gl_Position=vec4(0);}", "a_position", "u_color", "GL_OES_texture_float",
                                     "#version 300 es\nin vec4 p;void main(){gl_Position=p;}" };
        const char *s = SRC[below(5)];
        uint32_t l = (uint32_t)strlen(s) < tail ? (uint32_t)strlen(s) : tail;
        put(s, l);
        for (uint32_t i = l; i < tail; i++) u8(f.form == F_NAMES && below(6) == 0 ? 0 : (uint8_t)('a' + below(26)));
    } else {
        for (uint32_t i = 0; i < tail; i++) u8((uint8_t)(below(3) ? rnd() : 0));
    }
}

/* Every special op and query, and a plain op at random (of its length,
 * mostly); now and then a length that lies, or an op that isn't one. */
static void gl_record(void) {
    uint32_t op, words, extra;
    if (below(3) == 0) {
        op = below(2) ? 256 + below(21) : 300 + below(41);
        gl_grammatical(op);
        return;
    }
    switch (below(8)) {
    case 0: case 1: case 2: case 3: case 4:
        op = 1 + below(101);
        words = below(20) ? g_arity[op] : below(14);
        extra = 0;
        break;
    case 5: op = 256 + below(21); words = below(14); extra = below(6) == 0 ? below(9000) : below(64); break;
    case 6: op = 300 + below(41); words = below(9); extra = below(4) ? 0 : below(64); break;
    default:
        op = below(20) ? 256 + below(21) : below(2) ? 1 + below(400) : rnd();
        words = below(14);
        extra = below(64);
        break;
    }
    uint32_t n = words * 4 + extra;
    uint32_t claim = below(60) == 0 ? (below(2) ? n + 1 + below(64) : rnd()) : n;
    if (claim < n && below(2)) n = claim;               /* a short body */
    u32(op);
    u32(claim > n && below(3) ? n : claim);              /* mostly honest */
    for (uint32_t i = 0; i < words && i * 4 < n; i++) {
        if (below(5) == 0) { float f = flt(); put(&f, 4); } else u32(word());
    }
    uint32_t done = words * 4 < n ? words * 4 : n;
    if (n > done) {
        /* Shader source sometimes, else bytes. */
        if (below(4) == 0) {
            static const char *SRC[] = {
                "void main(){gl_Position=vec4(0);}", "precision mediump float;void main(){gl_FragColor=vec4(1);}",
                "#version 300 es\nin vec4 p;void main(){gl_Position=p;}", "#version 300 es\nprecision highp float;"
                "out vec4 o;uniform sampler2D s;void main(){o=texture(s,vec2(0));}",
                "void main(){ for(int i=0;i<1000000;i++){} }", "#extension GL_EXT_frag_depth : enable\nvoid main(){}" };
            const char *s = SRC[below(6)];
            uint32_t l = (uint32_t)strlen(s);
            if (l > n - done) l = n - done;
            put(s, l);
            done += l;
        }
        for (; done < n; done++) u8((uint8_t)(below(3) ? rnd() : 0));
    }
}

/* One record per call: a WebGL 2 one on a WebGL 1 context is only an error. */
static void gl_reset(WebglCtx *c, uint8_t **out, uint32_t *len, uint32_t *cap) {
    static const uint32_t R[][4] = {
        { WEBGL_OP_ENDTRANSFORMFEEDBACK, 0 }, { WEBGL_OP_BINDTRANSFORMFEEDBACK, 2, 0x8E22, 0 },
        { WEBGL_OP_BINDBUFFER, 2, 0x88EB, 0 }, { WEBGL_OP_READBUFFER, 1, 0x0405 }, { 270, 1, 0x0405 },
        { 274, 2, 0x0D02, 0 }, { 274, 2, 0x0D03, 0 }, { 274, 2, 0x0D04, 0 }, { WEBGL_OP_DISABLE, 1, 0x8C89 },
    };
    for (size_t i = 0; i < sizeof(R) / sizeof(R[0]); i++) {
        g_len = 0;
        u32(R[i][0]);
        u32(R[i][1] * 4);
        for (uint32_t k = 0; k < R[i][1]; k++) u32(R[i][2 + k]);
        *len = 0;
        webgl_run(c, g_buf, g_len, out, len, cap);
    }
}

static bool gl_check(WebglCtx *c, uint8_t **out, uint32_t *len, uint32_t *cap) {
    /* Back to the default framebuffer and the state a clear and a read
     * depend on, then clear to a color and read it back. */
    gl_reset(c, out, len, cap);
    g_len = 0;
    u32(WEBGL_OP_BINDFRAMEBUFFER); u32(8); u32(0x8D40); u32(0);
    u32(WEBGL_OP_DISABLE); u32(4); u32(0x0C11);                         /* scissor */
    u32(WEBGL_OP_COLORMASK); u32(16); u32(1); u32(1); u32(1); u32(1);
    u32(WEBGL_OP_VIEWPORT); u32(16); u32(0); u32(0); u32(8); u32(8);
    float col[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    u32(WEBGL_OP_CLEARCOLOR); u32(16); put(col, 16);
    u32(WEBGL_OP_CLEAR); u32(4); u32(0x4000);
    u32(274); u32(8); u32(0x0D05); u32(4);                               /* PACK_ALIGNMENT 4 */
    u32(313); u32(28); u32(0); u32(0); u32(1); u32(1); u32(0x1908); u32(0x1401); u32(4);
    u32(300); u32(0);                                                    /* getError */
    *len = 0;
    if (!webgl_run(c, g_buf, g_len, out, len, cap) || *len < 12) { printf("check: not run (%u)\n", *len); return false; }
    uint32_t n, err;
    memcpy(&n, *out, 4);
    memcpy(&err, *out + 8, 4);
    bool ok = n == 4 && (*out)[4] == 0 && (*out)[5] == 255 && (*out)[6] == 0;
    if (!ok) printf("check: read %u bytes %u,%u,%u,%u, error 0x%x\n", n, (*out)[4], (*out)[5], (*out)[6], (*out)[7], err);
    return ok;
}

static int fuzz_gl(const wchar_t *dir, int batches) {
    char why[256];
    if (!webgl_init(dir, why, sizeof(why))) { printf("no WebGL: %s\n", why); return 2; }
    uint8_t *out = NULL;
    uint32_t out_len = 0, out_cap = 0;
    int rejected = 0, failed = 0, lost = 0;
    WebglCtx *c = NULL;
    for (int b = 0; b < batches; b++) {
        if (!c || below(50) == 0) {
            if (c) webgl_destroy(c);
            c = webgl_create(1, 1 + (int)below(2), 1 + below(300), 1 + below(300), below(32));
            if (!c) { printf("FAIL: no context at batch %d\n", b); return 1; }
            /* Objects of each kind, for the records to name. */
            g_len = 0;
            for (uint32_t k = 0; k < 12; k++)
                for (uint32_t id = 1; id <= 4; id++) { u32(256); u32(12); u32(k); u32(id); u32(k == 4 ? 0x8B30 + (id & 1) : 0); }
            out_len = 0;
            webgl_run(c, g_buf, g_len, &out, &out_len, &out_cap);
            if (!g_arity[1]) gl_arity(c);
        }
        g_len = 0;
        int nrec = 1 + (int)below(12);
        for (int r = 0; r < nrec; r++) gl_record();
        out_len = 0;
        if (!webgl_run(c, g_buf, g_len, &out, &out_len, &out_cap)) rejected++;
        if (below(10) == 0) webgl_resize(c, below(2000), below(2000));
        if (!gl_check(c, &out, &out_len, &out_cap)) {
            /* A context lost (a reset) is allowed; any other failure isn't. */
            g_len = 0;
            u32(340); u32(0);
            out_len = 0;
            uint32_t status = 0;
            if (webgl_run(c, g_buf, g_len, &out, &out_len, &out_cap) && out_len >= 4) memcpy(&status, out, 4);
            if (status) { lost++; webgl_destroy(c); c = NULL; continue; }
            printf("FAIL: the context stopped working after batch %d (seed state %llu)\n", b,
                   (unsigned long long)g_rand);
            failed++;
            webgl_destroy(c);
            c = NULL;
        }
        if (b % 200 == 0) { printf("gl batch %d: %d rejected, %d failed, %d lost\n", b, rejected, failed, lost); fflush(stdout); }
    }
    if (c) webgl_destroy(c);
    printf("gl: %d batches, %d rejected as malformed, %d contexts lost, %d failed\n", batches, rejected, lost, failed);
    return failed ? 1 : 0;
}

/* ---- WebGPU ---- */

static const char *KEYS[] = { "size", "usage", "mappedAtCreation", "format", "dimension", "mipLevelCount",
    "sampleCount", "width", "height", "depthOrArrayLayers", "code", "layout", "vertex", "fragment", "module",
    "entryPoint", "buffers", "attributes", "arrayStride", "stepMode", "shaderLocation", "offset", "targets", "blend",
    "color", "alpha", "operation", "srcFactor", "dstFactor", "writeMask", "primitive", "topology", "cullMode",
    "depthStencil", "depthCompare", "depthWriteEnabled", "multisample", "count", "entries", "binding", "visibility",
    "buffer", "sampler", "texture", "storageTexture", "type", "resource", "bindGroupLayouts", "colorAttachments",
    "view", "loadOp", "storeOp", "clearValue", "r", "g", "b", "a", "depthStencilAttachment", "compute", "constants",
    "destination", "origin", "x", "y", "z", "mipLevel", "aspect", "bytesPerRow", "rowsPerImage", "filter",
    "requiredFeatures", "powerPreference", "alphaMode", "addressModeU", "magFilter", "lodMaxClamp", "maxAnisotropy",
    "compare", "baseMipLevel", "arrayLayerCount", "resolveTarget", "stencilFront", "failOp", "hasDynamicOffset",
    "minBindingSize", "sampleType", "viewDimension", "access", "unclippedDepth", "stripIndexFormat" };
static const char *STRS[] = { "rgba8unorm", "bgra8unorm", "depth24plus", "depth32float-stencil8", "r32float",
    "rgba16float", "2d", "3d", "1d", "cube", "2d-array", "auto", "triangle-list", "point-list", "line-strip",
    "uint16", "uint32", "float32x2", "float32x4", "unorm8x4-bgra", "clear", "load", "store", "discard", "uniform",
    "storage", "read-only-storage", "filtering", "comparison", "float", "depth", "sint", "write-only",
    "read-write", "vertex", "instance", "validation", "out-of-memory", "less", "always", "back", "src-alpha",
    "one-minus-src-alpha", "add", "max", "repeat", "linear", "all", "depth-only", "premultiplied", "opaque",
    "main", "vs", "fs", "high-performance", "shader-f16", "timestamp-query", "",
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f { return vec4f(0); }"
    "@fragment fn fs() -> @location(0) vec4f { return vec4f(1); }"
    "@compute @workgroup_size(1) fn main() {}",
    "@group(0) @binding(0) var<storage, read_write> d: array<u32>; @compute @workgroup_size(64) fn main("
    "@builtin(global_invocation_id) i: vec3u) { d[i.x] = d[i.x + 1000000u]; }",
    "fn f( {", "@compute @workgroup_size(1) fn main() { loop {} }" };
static const char *CMDS[] = { "setPipeline", "setBindGroup", "setVertexBuffer", "setIndexBuffer", "draw",
    "drawIndexed", "drawIndirect", "drawIndexedIndirect", "setViewport", "setScissorRect", "setBlendConstant",
    "setStencilReference", "dispatchWorkgroups", "dispatchWorkgroupsIndirect", "copyBufferToBuffer",
    "copyBufferToTexture", "copyTextureToBuffer", "copyTextureToTexture", "clearBuffer", "bogus" };

static void str(const char *s) { uint32_t l = (uint32_t)strlen(s); u8(V_STRING); u32(l); put(s, l); }
static void value(int depth);
static void obj_of(int depth, int keys) {
    u8(V_OBJECT);
    u32((uint32_t)keys);
    for (int i = 0; i < keys; i++) {
        const char *k = KEYS[below(sizeof(KEYS) / sizeof(KEYS[0]))];
        u32((uint32_t)strlen(k));
        put(k, (uint32_t)strlen(k));
        value(depth + 1);
    }
}
static void value(int depth) {
    uint32_t pick = depth > 6 ? below(5) : below(10);
    switch (pick) {
    case 0: u8(V_NULL); break;
    case 1: u8(below(2) ? V_TRUE : V_FALSE); break;
    case 2: {
        u8(V_NUMBER);
        double d;
        switch (below(4)) {
        case 0: d = (double)EDGES[below(sizeof(EDGES) / 4)]; break;
        case 1: d = -(double)below(100); break;
        case 2: { uint64_t b = ((uint64_t)rnd() << 32) | rnd(); memcpy(&d, &b, 8); break; }   /* NaN, inf, ... */
        default: d = (double)below(512); break;
        }
        f64(d);
        break;
    }
    case 3: str(STRS[below(sizeof(STRS) / sizeof(STRS[0]))]); break;
    case 4: u8(V_ID); u32(below(5) ? below(64) : word()); break;
    case 5: case 6: obj_of(depth, (int)below(8)); break;
    case 7: case 8: {
        uint32_t n = below(6);
        u8(V_ARRAY);
        u32(n);
        if (n && below(2)) { str(CMDS[below(sizeof(CMDS) / sizeof(CMDS[0]))]); n--; }
        for (uint32_t i = 0; i < n; i++) value(depth + 1);
        break;
    }
    default: {
        uint32_t n = below(4) ? below(256) : below(70000);
        u8(V_BYTES);
        u32(n);
        for (uint32_t i = 0; i < n; i++) u8((uint8_t)rnd());
        break;
    }
    }
}

static void gpu_record(void) {
    uint32_t op = below(10) ? 3 + below(G_OPS - 3) : (below(2) ? rnd() : below(40));
    if (op == G_REQUEST_DEVICE || op == G_REQUEST_ADAPTER) op = G_CREATE_BUFFER;   /* (made once, below) */
    uint32_t at = g_len;
    u32(op);
    u32(0);
    uint32_t words = below(4);
    for (uint32_t i = 0; i < words; i++) u32(below(4) ? below(64) : word());
    if (op == G_BUFFER_MAP || op == G_BUFFER_UNMAP || op == G_WRITE_BUFFER) {
        f64(below(3) ? (double)(below(8) * 4) : (double)rnd() * 1e6);
        if (op == G_BUFFER_MAP) f64(below(3) ? (double)(below(8) * 4) : -1.0);
    }
    if (below(5)) value(0);
    if (below(10) == 0) {                       /* trailing junk */
        uint32_t n = below(64);
        for (uint32_t i = 0; i < n; i++) u8((uint8_t)rnd());
    }
    uint32_t n = g_len - at - 8;
    if (below(60) == 0) n = below(2) ? n + 1 + below(16) : rnd();          /* a length that lies */
    memcpy(g_buf + at + 4, &n, 4);
}

/* A canvas cleared to green and presented: it reads back green. */
static bool gpu_check(WgpuClient *c, uint8_t **out, uint32_t *len, uint32_t *cap) {
    static uint8_t *px;
    static size_t px_cap;
    g_len = 0;
    /* canvas 900 on device 2; texture 901, view 902, encoder 903, pass 904, buffer 905 */
    u32(G_CONFIGURE); u32(0); u32(900); u32(2);
    uint32_t at = g_len;
    u8(V_OBJECT); u32(3);
    u32(6); put("format", 6); str("rgba8unorm");
    u32(5); put("width", 5); u8(V_NUMBER); f64(4);
    u32(6); put("height", 6); u8(V_NUMBER); f64(4);
    uint32_t n = g_len - at + 8;
    memcpy(g_buf + at - 12, &n, 4);
    for (uint32_t id = 901; id <= 905; id++) { u32(G_RELEASE); u32(4); u32(id); }
    u32(G_CURRENT_TEXTURE); u32(8); u32(901); u32(900);
    at = g_len;
    u32(G_CREATE_VIEW); u32(0); u32(902); u32(901); u8(V_NULL);
    memcpy(g_buf + at + 4, &(uint32_t){ g_len - at - 8 }, 4);
    u32(G_CREATE_COMMAND_ENCODER); u32(8); u32(903); u32(2);
    at = g_len;
    u32(G_BEGIN_RENDER_PASS); u32(0); u32(904); u32(903);
    u8(V_OBJECT); u32(1); u32(16); put("colorAttachments", 16);
    u8(V_ARRAY); u32(1); u8(V_OBJECT); u32(4);
    u32(4); put("view", 4); u8(V_ID); u32(902);
    u32(6); put("loadOp", 6); str("clear");
    u32(7); put("storeOp", 7); str("store");
    u32(10); put("clearValue", 10); u8(V_ARRAY); u32(4);
    u8(V_NUMBER); f64(0); u8(V_NUMBER); f64(1); u8(V_NUMBER); f64(0); u8(V_NUMBER); f64(1);
    memcpy(g_buf + at + 4, &(uint32_t){ g_len - at - 8 }, 4);
    u32(G_END_PASS); u32(4); u32(904);
    u32(G_FINISH); u32(8); u32(905); u32(903);
    at = g_len;
    u32(G_SUBMIT); u32(0); u32(3); u8(V_ARRAY); u32(1); u8(V_ID); u32(905);
    memcpy(g_buf + at + 4, &(uint32_t){ g_len - at - 8 }, 4);
    *len = 0;
    if (!wgpu_run(c, g_buf, g_len, out, len, cap)) return false;
    uint32_t w = 0, h = 0;
    wgpu_present(c, 900, &px, &px_cap, &w, &h);
    return w == 4 && h == 4 && px[0] == 0 && px[1] == 255 && px[2] == 0;
}

static int fuzz_gpu(const wchar_t *dir, int batches) {
    uint8_t *out = NULL;
    uint32_t out_len = 0, out_cap = 0;
    int rejected = 0, failed = 0, fresh = 0;
    WgpuClient *c = NULL;
    for (int b = 0; b < batches; b++) {
        if (!c || below(100) == 0) {
            if (c) wgpu_client_free(c);
            c = wgpu_client_new(dir);
            if (!c) { printf("no WebGPU\n"); return 2; }
            fresh++;
            g_len = 0;
            u32(G_REQUEST_ADAPTER); u32(5); u32(1); u8(V_NULL);
            u32(G_REQUEST_DEVICE); u32(13); u32(2); u32(3); u32(1); u8(V_NULL);
            out_len = 0;
            if (!wgpu_run(c, g_buf, g_len, &out, &out_len, &out_cap) || out_len < 2 || out[0] == V_NULL ||
                !gpu_check(c, &out, &out_len, &out_cap)) {
                printf("FAIL: no working device at batch %d\n", b);
                return 1;
            }
        }
        g_len = 0;
        int nrec = 1 + (int)below(8);
        for (int r = 0; r < nrec; r++) gpu_record();
        if (getenv("FUZZ_TRACE")) {         /* the batch's ops and words, to find what a crash was */
            for (uint32_t at = 0; at + 8 <= g_len; ) {
                uint32_t op, n;
                memcpy(&op, g_buf + at, 4);
                memcpy(&n, g_buf + at + 4, 4);
                printf("  op %u n %u:", op, n);
                for (uint32_t k = 0; k < 3 && at + 8 + 4 * k + 4 <= g_len && 4 * k + 4 <= n; k++) {
                    uint32_t v;
                    memcpy(&v, g_buf + at + 8 + 4 * k, 4);
                    printf(" %u", v);
                }
                printf("\n");
                if (n > g_len - at - 8) break;
                at += 8 + n;
            }
        }
        out_len = 0;
        if (!wgpu_run(c, g_buf, g_len, &out, &out_len, &out_cap)) rejected++;
        if (!gpu_check(c, &out, &out_len, &out_cap)) {
            /* The page may destroy its own device (G_DESTROY on id 2): then a
             * fresh client must work. */
            wgpu_client_free(c);
            c = NULL;
            continue;
        }
        if (b % 200 == 0) { printf("gpu batch %d: %d rejected, %d clients\n", b, rejected, fresh); fflush(stdout); }
    }
    if (c) wgpu_client_free(c);
    printf("gpu: %d batches, %d rejected as malformed, %d clients, %d failed\n", batches, rejected, fresh, failed);
    return failed ? 1 : 0;
}

/* ---- WebGPU, in sequences: real objects, used right and wrong ----
 *
 * Random records seldom make a command buffer that is valid in shape and
 * wrong in use, which is what wgpu-native aborts on at submit.  Here each
 * batch is a sequence of well-formed calls on a pool of real objects
 * (buffers, textures, views, bind groups, encoders, passes, command
 * buffers), in any order: mapped buffers used, destroyed ones written,
 * command buffers submitted twice or after their device, passes left open,
 * scopes popped that were never pushed. */

/* A record: op, words, and a tree built by the callback. */
static void rec(uint32_t op, const uint32_t *words, int nw, void (*tree)(void *), void *arg) {
    uint32_t at = g_len;
    u32(op);
    u32(0);
    for (int i = 0; i < nw; i++) u32(words[i]);
    if (tree) tree(arg);
    uint32_t n = g_len - at - 8;
    memcpy(g_buf + at + 4, &n, 4);
}
static void key(const char *k) { u32((uint32_t)strlen(k)); put(k, (uint32_t)strlen(k)); }
static void num(double v) { u8(V_NUMBER); f64(v); }
static void idv(uint32_t id) { u8(V_ID); u32(id); }

#define POOL 48                         /* ids 16 .. 16 + POOL */
static uint32_t pick(void) { return 16 + below(POOL); }

static const char *SEQ_WGSL =
    "@group(0) @binding(0) var<storage, read_write> d: array<u32>;"
    "@compute @workgroup_size(4) fn main(@builtin(global_invocation_id) i: vec3u) { d[i.x] = d[i.x] * 2u; }"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {"
    "  var p = array<vec2f, 3>(vec2f(-1, -1), vec2f(1, -1), vec2f(-1, 1)); return vec4f(p[i], 0, 1); }"
    "@fragment fn fs() -> @location(0) vec4f { return vec4f(1, 0.5, 0, 1); }";

static void t_null(void *a) { (void)a; u8(V_NULL); }
static void t_code(void *a) { (void)a; u8(V_OBJECT); u32(1); key("code"); str(SEQ_WGSL); }
static void t_cpipe(void *a) {
    (void)a;
    u8(V_OBJECT); u32(2); key("layout"); str("auto");
    key("compute"); u8(V_OBJECT); u32(2); key("module"); idv(4); key("entryPoint"); str("main");
}
static void t_rpipe(void *a) {
    (void)a;
    u8(V_OBJECT); u32(3); key("layout"); str("auto");
    key("vertex"); u8(V_OBJECT); u32(2); key("module"); idv(4); key("entryPoint"); str("vs");
    key("fragment"); u8(V_OBJECT); u32(3); key("module"); idv(4); key("entryPoint"); str("fs");
    key("targets"); u8(V_ARRAY); u32(1); u8(V_OBJECT); u32(1); key("format"); str("rgba8unorm");
}
static void t_buffer(void *a) {
    (void)a;
    static const uint32_t U[] = { 1 | 8, 2 | 4, 128 | 4 | 8, 128, 8 | 4, 32 | 8, 64 | 8, 256 | 128, 1 | 2 };
    uint32_t size = below(4) ? 16 * (1 + below(16)) : below(300);
    u8(V_OBJECT); u32(3);
    key("size"); num(size);
    key("usage"); num(U[below(sizeof(U) / 4)]);
    key("mappedAtCreation"); u8(below(4) ? V_FALSE : V_TRUE);
}
static void t_texture(void *a) {
    (void)a;
    u8(V_OBJECT); u32(3);
    key("size"); u8(V_ARRAY); u32(2); num(1 + below(64)); num(1 + below(64));
    key("format"); str("rgba8unorm");
    key("usage"); num((below(2) ? 16 : 0) | (below(2) ? 1 : 0) | (below(2) ? 2 : 0) | (below(2) ? 4 : 0));
}
static void t_group(void *a) {
    uint32_t buf = *(uint32_t *)a;
    u8(V_OBJECT); u32(2); key("layout"); idv(7);
    key("entries"); u8(V_ARRAY); u32(1); u8(V_OBJECT); u32(2); key("binding"); num(0);
    key("resource"); u8(V_OBJECT); u32(1); key("buffer"); idv(buf);
}
static void t_rpass(void *a) {
    uint32_t view = *(uint32_t *)a;
    u8(V_OBJECT); u32(1); key("colorAttachments"); u8(V_ARRAY); u32(1); u8(V_OBJECT); u32(3);
    key("view"); idv(view); key("loadOp"); str(below(2) ? "clear" : "load"); key("storeOp"); str("store");
}
static void t_compute_cmds(void *a) {
    uint32_t group = *(uint32_t *)a;
    u8(V_ARRAY); u32(3);
    u8(V_ARRAY); u32(2); str("setPipeline"); idv(below(8) ? 5 : 6);
    u8(V_ARRAY); u32(3); str("setBindGroup"); num(0); idv(group);
    u8(V_ARRAY); u32(2); str("dispatchWorkgroups"); num(below(4));
}
static void t_render_cmds(void *a) {
    (void)a;
    u8(V_ARRAY); u32(2);
    u8(V_ARRAY); u32(2); str("setPipeline"); idv(below(8) ? 6 : 5);
    u8(V_ARRAY); u32(2); str("draw"); num(below(5));
}
static void t_copy(void *a) {
    uint32_t *ab = (uint32_t *)a;
    u8(V_ARRAY); u32(1); u8(V_ARRAY); u32(6); str("copyBufferToBuffer");
    idv(ab[0]); num(below(3) * 4); idv(ab[1]); num(below(3) * 4); num(4 * below(8));
}
static void t_submit(void *a) {
    uint32_t *ids = (uint32_t *)a;
    u8(V_ARRAY); u32(ids[0]);
    for (uint32_t i = 0; i < ids[0]; i++) idv(ids[1 + i]);
}
static void t_scope(void *a) { (void)a; u8(V_OBJECT); u32(1); key("filter"); str("validation"); }

static void seq_step(void) {
    uint32_t a = pick(), b = pick(), w[4];
    switch (below(17)) {
    case 0: case 1: w[0] = a; w[1] = 2; rec(G_CREATE_BUFFER, w, 2, t_buffer, NULL); break;
    case 2: w[0] = a; w[1] = 2; rec(G_CREATE_TEXTURE, w, 2, t_texture, NULL); break;
    case 3: w[0] = a; w[1] = b; rec(G_CREATE_VIEW, w, 2, t_null, NULL); break;
    case 4: w[0] = a; w[1] = 2; rec(G_CREATE_BIND_GROUP, w, 2, t_group, &b); break;
    case 5: w[0] = a; w[1] = 2; rec(G_CREATE_COMMAND_ENCODER, w, 2, NULL, NULL); break;
    case 6: w[0] = a; w[1] = b; rec(G_BEGIN_COMPUTE_PASS, w, 2, NULL, NULL); break;
    case 7: { uint32_t v = pick(); w[0] = a; w[1] = b; rec(G_BEGIN_RENDER_PASS, w, 2, t_rpass, &v); break; }
    case 8: { uint32_t g = pick(); w[0] = a; rec(G_PASS, w, 1, below(2) ? t_compute_cmds : t_render_cmds, &g); break; }
    case 9: w[0] = a; rec(G_END_PASS, w, 1, NULL, NULL); break;
    case 10: { uint32_t ab[2] = { b, pick() }; w[0] = a; rec(G_ENCODE, w, 1, t_copy, ab); break; }
    case 11: w[0] = a; w[1] = b; rec(G_FINISH, w, 2, NULL, NULL); break;
    case 12: {
        uint32_t ids[5] = { 1 + below(3), a, below(4) ? pick() : a, pick(), 0 };
        w[0] = below(20) ? 3 : 2;
        rec(G_SUBMIT, w, 1, t_submit, ids);
        break;
    }
    case 13: {                          /* map, maybe unmap with bytes */
        uint32_t at = g_len;
        u32(G_BUFFER_MAP); u32(24); u32(a); u32(1 + below(2)); f64(below(3) * 8); f64(below(4) * 4);
        (void)at;
        if (below(2)) {
            uint32_t len = 4 * below(4);
            u32(G_BUFFER_UNMAP); u32(12 + len); u32(a); f64(below(2) * 8);
            for (uint32_t i = 0; i < len; i++) u8((uint8_t)rnd());
        }
        break;
    }
    case 14: {                          /* write, destroy, release */
        uint32_t len = 4 * below(5);
        switch (below(4)) {
        case 0: u32(G_WRITE_BUFFER); u32(16 + len); u32(3); u32(a); f64(below(3) * 4);
                for (uint32_t i = 0; i < len; i++) u8((uint8_t)rnd());
                break;
        case 1: u32(G_DESTROY); u32(4); u32(a); break;
        default: u32(G_RELEASE); u32(4); u32(a); break;
        }
        break;
    }
    case 15:
        w[0] = 2;
        if (below(2)) rec(G_PUSH_ERROR_SCOPE, w, 1, t_scope, NULL); else rec(G_POP_ERROR_SCOPE, w, 1, NULL, NULL);
        break;
    default:
        if (below(60) == 0) { u32(G_DESTROY); u32(4); u32(2); }          /* the device itself */
        else { w[0] = 3; rec(G_WORK_DONE, w, 1, NULL, NULL); }
        break;
    }
}

/* Each client's batches, kept to replay (cb_webgpu_fuzz replay <file>):
 * when a fresh client fails, the one before it is written out. */
static uint8_t *g_log, *g_prev;
static size_t g_log_len, g_log_cap, g_prev_len;
static void log_batch(void) {
    if (g_log_len + g_len + 4 > g_log_cap) {
        g_log_cap = (g_log_len + g_len + 4) * 2;
        g_log = (uint8_t *)realloc(g_log, g_log_cap);
    }
    memcpy(g_log + g_log_len, &g_len, 4);
    memcpy(g_log + g_log_len + 4, g_buf, g_len);
    g_log_len += 4 + g_len;
}
static void log_next_client(void) {
    free(g_prev);
    g_prev = g_log;
    g_prev_len = g_log_len;
    g_log = NULL;
    g_log_len = g_log_cap = 0;
}

/* A fresh client's adapter, device, module, pipelines and layout. */
static void seq_setup(void) {
    g_len = 0;
    uint32_t w[4] = { 1 };
    rec(G_REQUEST_ADAPTER, w, 1, t_null, NULL);
    w[0] = 2; w[1] = 3; w[2] = 1;
    rec(G_REQUEST_DEVICE, w, 3, t_null, NULL);
    w[0] = 4; w[1] = 2;
    rec(G_CREATE_SHADER_MODULE, w, 2, t_code, NULL);
    w[0] = 5;
    rec(G_CREATE_COMPUTE_PIPELINE, w, 2, t_cpipe, NULL);
    w[0] = 6;
    rec(G_CREATE_RENDER_PIPELINE, w, 2, t_rpipe, NULL);
    w[0] = 7; w[1] = 5; w[2] = 0;
    rec(G_GET_BIND_GROUP_LAYOUT, w, 3, NULL, NULL);
}

/* replay <file> [from to]: a client runs the file's batches (but those
 * from..to), is freed, and then a new one must get a working device. */
static int replay(const wchar_t *dir, const char *file, int from, int to) {
    FILE *f = fopen(file, "rb");
    if (!f) { printf("no %s\n", file); return 2; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *log = (uint8_t *)malloc(size > 0 ? (size_t)size : 1);
    size_t got = log ? fread(log, 1, (size_t)size, f) : 0;
    fclose(f);
    uint8_t *out = NULL;
    uint32_t out_len = 0, out_cap = 0;
    /* REPLAY_TIMES rounds of it (what builds up across clients). */
    int times = getenv("REPLAY_TIMES") ? atoi(getenv("REPLAY_TIMES")) : 1, i = 0;
    for (int round = 0; round < times; round++) {
        WgpuClient *c = wgpu_client_new(dir);
        seq_setup();
        out_len = 0;
        wgpu_run(c, g_buf, g_len, &out, &out_len, &out_cap);
        i = 0;
        for (size_t at = 0; at + 4 <= got; i++) {
            uint32_t n;
            memcpy(&n, log + at, 4);
            if (at + 4 + n > got) break;
            const char *only = getenv("REPLAY_ONLY"), *dash = only ? strchr(only, '-') : NULL;
            bool only_ok = !only || (i >= atoi(only) && i <= (dash ? atoi(dash + 1) : atoi(only)));
            if (only_ok && getenv("REPLAY_DUMP") && round == 0)
                for (uint32_t r = 0; r + 8 <= n; ) {
                    uint32_t op, rn, v[3] = { 0 };
                    memcpy(&op, log + at + 4 + r, 4);
                    memcpy(&rn, log + at + 8 + r, 4);
                    memcpy(v, log + at + 12 + r, rn < 12 ? rn : 12);
                    printf("batch %d: op %u n %u: %u %u %u\n", i, op, rn, v[0], v[1], v[2]);
                    r += 8 + rn;
                }
            if ((i < from || i > to) && only_ok) { out_len = 0; wgpu_run(c, log + at + 4, n, &out, &out_len, &out_cap); }
            at += 4 + n;
        }
        wgpu_client_free(c);
        c = wgpu_client_new(dir);
        seq_setup();
        out_len = 0;
        bool ok = wgpu_run(c, g_buf, g_len, &out, &out_len, &out_cap) && gpu_check(c, &out, &out_len, &out_cap);
        wgpu_client_free(c);
        if (!ok) {
            printf("replay of %d batches (skipping %d..%d): the next client FAILS after %d rounds\n", i, from, to, round + 1);
            return 1;
        }
    }
    printf("replay of %d batches (skipping %d..%d), %d rounds: the next client works\n", i, from, to, times);
    return 0;
}

static int fuzz_gpu_seq(const wchar_t *dir, int batches) {
    uint8_t *out = NULL;
    uint32_t out_len = 0, out_cap = 0;
    int fresh = 0;
    WgpuClient *c = NULL;
    for (int b = 0; b < batches; b++) {
        if (!c || below(100) == 0) {
            if (c) wgpu_client_free(c);
            c = wgpu_client_new(dir);
            if (!c) { printf("no WebGPU\n"); return 2; }
            fresh++;
            log_next_client();
            g_len = 0;
            uint32_t w[4] = { 1 };
            rec(G_REQUEST_ADAPTER, w, 1, t_null, NULL);
            w[0] = 2; w[1] = 3; w[2] = 1;
            rec(G_REQUEST_DEVICE, w, 3, t_null, NULL);
            w[0] = 4; w[1] = 2;
            rec(G_CREATE_SHADER_MODULE, w, 2, t_code, NULL);
            w[0] = 5;
            rec(G_CREATE_COMPUTE_PIPELINE, w, 2, t_cpipe, NULL);
            w[0] = 6;
            rec(G_CREATE_RENDER_PIPELINE, w, 2, t_rpipe, NULL);
            w[0] = 7; w[1] = 5; w[2] = 0;
            rec(G_GET_BIND_GROUP_LAYOUT, w, 3, NULL, NULL);
            out_len = 0;
            bool ran = wgpu_run(c, g_buf, g_len, &out, &out_len, &out_cap);
            uint32_t got = out_len;
            int tags[2] = { got > 0 ? out[0] : -1, -1 };
            /* (the adapter's info tree, then the device's limits) */
            if (ran && got > 1 && out[0] == V_OBJECT) tags[1] = out[got - 1 - 8] == V_NUMBER ? V_OBJECT : out[got - 1];
            if (ran && tags[0] == V_OBJECT && tags[1] != V_OBJECT && fresh > 40) {
                /* wgpu-native's leaks (command buffers never submitted) used
                 * up device memory: the web-GPU process is recycled when
                 * idle for that.  Not a failure of the decoder. */
                printf("note: no more devices after %d clients (wgpu-native's leaks): stopping at batch %d\n", fresh, b);
                batches = b;
                break;
            }
            if (!ran || !gpu_check(c, &out, &out_len, &out_cap)) {
                printf("FAIL: no working device at batch %d (ran %d, %u bytes, adapter %d, device %s)\n", b, ran, got,
                       tags[0], tags[1] == V_OBJECT ? "made" : "none");
                FILE *f = fopen("fuzz_prev.bin", "wb");
                if (f) { fwrite(g_prev, 1, g_prev_len, f); fclose(f); printf("the client before: fuzz_prev.bin\n"); }
                return 1;
            }
        }
        g_len = 0;
        int steps = 1 + (int)below(30);
        for (int s = 0; s < steps; s++) seq_step();
        if (getenv("FUZZ_TRACE")) {
            for (uint32_t at = 0; at + 8 <= g_len; ) {
                uint32_t op, n, v0 = 0, v1 = 0;
                memcpy(&op, g_buf + at, 4);
                memcpy(&n, g_buf + at + 4, 4);
                if (n >= 4) memcpy(&v0, g_buf + at + 8, 4);
                if (n >= 8) memcpy(&v1, g_buf + at + 12, 4);
                printf("  op %u n %u: %u %u\n", op, n, v0, v1);
                at += 8 + n;
            }
        }
        log_batch();
        out_len = 0;
        if (!wgpu_run(c, g_buf, g_len, &out, &out_len, &out_cap)) { printf("FAIL: a well-formed batch refused\n"); return 1; }
        if (!gpu_check(c, &out, &out_len, &out_cap)) {   /* the device destroyed: a fresh client */
            wgpu_client_free(c);
            c = NULL;
        }
        if (b % 200 == 0) printf("gpuseq batch %d: %d clients\n", b, fresh);
    }
    if (c) wgpu_client_free(c);
    printf("gpuseq: %d batches, %d clients, 0 failed\n", batches, fresh);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || (strcmp(argv[1], "gl") && strcmp(argv[1], "gpu") && strcmp(argv[1], "gpuseq") &&
                     strcmp(argv[1], "replay"))) {
        fprintf(stderr, "usage: cb_webgpu_fuzz gl|gpu|gpuseq [batches] [seed]\n"
                        "       cb_webgpu_fuzz replay <file> [skip from] [skip to]\n");
        return 2;
    }
    int batches = argc > 2 ? atoi(argv[2]) : 2000;
    g_rand = argc > 3 ? strtoull(argv[3], NULL, 10) * 0x9E3779B97F4A7C15ull + 1 : 0x9E3779B97F4A7C15ull;
    wchar_t exe[MAX_PATH], dir[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *slash = wcsrchr(exe, L'\\');
    if (slash) slash[1] = 0;
    _snwprintf(dir, MAX_PATH, L"%lswebgpu", exe);
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!strcmp(argv[1], "replay"))
        return argc > 2 ? replay(dir, argv[2], argc > 3 ? atoi(argv[3]) : -1, argc > 4 ? atoi(argv[4]) : -1) : 2;
    return !strcmp(argv[1], "gl") ? fuzz_gl(dir, batches) : !strcmp(argv[1], "gpu") ? fuzz_gpu(dir, batches)
                                                                                    : fuzz_gpu_seq(dir, batches);
}

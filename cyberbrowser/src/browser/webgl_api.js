/*
 * webgl_api.js - WebGL 1 and 2 (the Khronos WebGL specifications) in a tab,
 * installed in each realm by webgpu_api_install (browser/webgpu_api.c) as
 * fn(global, window, native, gen).
 *
 * A context's calls are recorded as [u32 op][u32 n][args] (webgl_backend.h)
 * and sent to the site's web-GPU process, where ANGLE runs them in a WebGL
 * compatibility context: it does WebGL's checks, so this side only shapes
 * calls and keeps what WebGL answers from here (bindings, the context's
 * attributes, extensions).  Records are sent when something must be
 * answered, and at the end of the task with the canvas presented (its
 * pixels come back to be drawn with the page).
 *
 * gen (browser/webgl_gen.js, made by tools/gen_webgl.js) installs the plain
 * calls; the rest are here.  native: available, glCreate, glRun, resize,
 * destroy, present, lost, imageSource.
 */
(function (global, win, native, gen) {
    'use strict';
    function define(obj, name, value) {
        Object.defineProperty(obj, name, { value: value, writable: true, configurable: true });
    }
    function expose(name, value) {
        define(global, name, value);
        if (win && win !== global) define(win, name, value);
    }
    function illegal() { throw new TypeError('Illegal constructor'); }

    /* ---- constants ---- */
    var C1 = 'DEPTH_BUFFER_BIT:100 STENCIL_BUFFER_BIT:400 COLOR_BUFFER_BIT:4000 POINTS:0 LINES:1 LINE_LOOP:2 ' +
        'LINE_STRIP:3 TRIANGLES:4 TRIANGLE_STRIP:5 TRIANGLE_FAN:6 ZERO:0 ONE:1 SRC_COLOR:300 ONE_MINUS_SRC_COLOR:301 ' +
        'SRC_ALPHA:302 ONE_MINUS_SRC_ALPHA:303 DST_ALPHA:304 ONE_MINUS_DST_ALPHA:305 DST_COLOR:306 ' +
        'ONE_MINUS_DST_COLOR:307 SRC_ALPHA_SATURATE:308 FUNC_ADD:8006 BLEND_EQUATION:8009 BLEND_EQUATION_RGB:8009 ' +
        'BLEND_EQUATION_ALPHA:883D FUNC_SUBTRACT:800A FUNC_REVERSE_SUBTRACT:800B BLEND_DST_RGB:80C8 BLEND_SRC_RGB:80C9 ' +
        'BLEND_DST_ALPHA:80CA BLEND_SRC_ALPHA:80CB CONSTANT_COLOR:8001 ONE_MINUS_CONSTANT_COLOR:8002 ' +
        'CONSTANT_ALPHA:8003 ONE_MINUS_CONSTANT_ALPHA:8004 BLEND_COLOR:8005 ARRAY_BUFFER:8892 ' +
        'ELEMENT_ARRAY_BUFFER:8893 ARRAY_BUFFER_BINDING:8894 ELEMENT_ARRAY_BUFFER_BINDING:8895 STREAM_DRAW:88E0 ' +
        'STATIC_DRAW:88E4 DYNAMIC_DRAW:88E8 BUFFER_SIZE:8764 BUFFER_USAGE:8765 CURRENT_VERTEX_ATTRIB:8626 FRONT:404 ' +
        'BACK:405 FRONT_AND_BACK:408 CULL_FACE:B44 BLEND:BE2 DITHER:BD0 STENCIL_TEST:B90 DEPTH_TEST:B71 ' +
        'SCISSOR_TEST:C11 POLYGON_OFFSET_FILL:8037 SAMPLE_ALPHA_TO_COVERAGE:809E SAMPLE_COVERAGE:80A0 NO_ERROR:0 ' +
        'INVALID_ENUM:500 INVALID_VALUE:501 INVALID_OPERATION:502 OUT_OF_MEMORY:505 CW:900 CCW:901 LINE_WIDTH:B21 ' +
        'ALIASED_POINT_SIZE_RANGE:846D ALIASED_LINE_WIDTH_RANGE:846E CULL_FACE_MODE:B45 FRONT_FACE:B46 ' +
        'DEPTH_RANGE:B70 DEPTH_WRITEMASK:B72 DEPTH_CLEAR_VALUE:B73 DEPTH_FUNC:B74 STENCIL_CLEAR_VALUE:B91 ' +
        'STENCIL_FUNC:B92 STENCIL_FAIL:B94 STENCIL_PASS_DEPTH_FAIL:B95 STENCIL_PASS_DEPTH_PASS:B96 STENCIL_REF:B97 ' +
        'STENCIL_VALUE_MASK:B93 STENCIL_WRITEMASK:B98 STENCIL_BACK_FUNC:8800 STENCIL_BACK_FAIL:8801 ' +
        'STENCIL_BACK_PASS_DEPTH_FAIL:8802 STENCIL_BACK_PASS_DEPTH_PASS:8803 STENCIL_BACK_REF:8CA3 ' +
        'STENCIL_BACK_VALUE_MASK:8CA4 STENCIL_BACK_WRITEMASK:8CA5 VIEWPORT:BA2 SCISSOR_BOX:C10 COLOR_CLEAR_VALUE:C22 ' +
        'COLOR_WRITEMASK:C23 UNPACK_ALIGNMENT:CF5 PACK_ALIGNMENT:D05 MAX_TEXTURE_SIZE:D33 MAX_VIEWPORT_DIMS:D3A ' +
        'SUBPIXEL_BITS:D50 RED_BITS:D52 GREEN_BITS:D53 BLUE_BITS:D54 ALPHA_BITS:D55 DEPTH_BITS:D56 STENCIL_BITS:D57 ' +
        'POLYGON_OFFSET_UNITS:2A00 POLYGON_OFFSET_FACTOR:8038 TEXTURE_BINDING_2D:8069 SAMPLE_BUFFERS:80A8 ' +
        'SAMPLES:80A9 SAMPLE_COVERAGE_VALUE:80AA SAMPLE_COVERAGE_INVERT:80AB COMPRESSED_TEXTURE_FORMATS:86A3 ' +
        'DONT_CARE:1100 FASTEST:1101 NICEST:1102 GENERATE_MIPMAP_HINT:8192 BYTE:1400 UNSIGNED_BYTE:1401 SHORT:1402 ' +
        'UNSIGNED_SHORT:1403 INT:1404 UNSIGNED_INT:1405 FLOAT:1406 DEPTH_COMPONENT:1902 ALPHA:1906 RGB:1907 RGBA:1908 ' +
        'LUMINANCE:1909 LUMINANCE_ALPHA:190A UNSIGNED_SHORT_4_4_4_4:8033 UNSIGNED_SHORT_5_5_5_1:8034 ' +
        'UNSIGNED_SHORT_5_6_5:8363 FRAGMENT_SHADER:8B30 VERTEX_SHADER:8B31 MAX_VERTEX_ATTRIBS:8869 ' +
        'MAX_VERTEX_UNIFORM_VECTORS:8DFB MAX_VARYING_VECTORS:8DFC MAX_COMBINED_TEXTURE_IMAGE_UNITS:8B4D ' +
        'MAX_VERTEX_TEXTURE_IMAGE_UNITS:8B4C MAX_TEXTURE_IMAGE_UNITS:8872 MAX_FRAGMENT_UNIFORM_VECTORS:8DFD ' +
        'SHADER_TYPE:8B4F DELETE_STATUS:8B80 LINK_STATUS:8B82 VALIDATE_STATUS:8B83 ATTACHED_SHADERS:8B85 ' +
        'ACTIVE_UNIFORMS:8B86 ACTIVE_ATTRIBUTES:8B89 SHADING_LANGUAGE_VERSION:8B8C CURRENT_PROGRAM:8B8D NEVER:200 ' +
        'LESS:201 EQUAL:202 LEQUAL:203 GREATER:204 NOTEQUAL:205 GEQUAL:206 ALWAYS:207 KEEP:1E00 REPLACE:1E01 ' +
        'INCR:1E02 DECR:1E03 INVERT:150A INCR_WRAP:8507 DECR_WRAP:8508 VENDOR:1F00 RENDERER:1F01 VERSION:1F02 ' +
        'NEAREST:2600 LINEAR:2601 NEAREST_MIPMAP_NEAREST:2700 LINEAR_MIPMAP_NEAREST:2701 NEAREST_MIPMAP_LINEAR:2702 ' +
        'LINEAR_MIPMAP_LINEAR:2703 TEXTURE_MAG_FILTER:2800 TEXTURE_MIN_FILTER:2801 TEXTURE_WRAP_S:2802 ' +
        'TEXTURE_WRAP_T:2803 TEXTURE_2D:DE1 TEXTURE:1702 TEXTURE_CUBE_MAP:8513 TEXTURE_BINDING_CUBE_MAP:8514 ' +
        'TEXTURE_CUBE_MAP_POSITIVE_X:8515 TEXTURE_CUBE_MAP_NEGATIVE_X:8516 TEXTURE_CUBE_MAP_POSITIVE_Y:8517 ' +
        'TEXTURE_CUBE_MAP_NEGATIVE_Y:8518 TEXTURE_CUBE_MAP_POSITIVE_Z:8519 TEXTURE_CUBE_MAP_NEGATIVE_Z:851A ' +
        'MAX_CUBE_MAP_TEXTURE_SIZE:851C ACTIVE_TEXTURE:84E0 REPEAT:2901 CLAMP_TO_EDGE:812F MIRRORED_REPEAT:8370 ' +
        'FLOAT_VEC2:8B50 FLOAT_VEC3:8B51 FLOAT_VEC4:8B52 INT_VEC2:8B53 INT_VEC3:8B54 INT_VEC4:8B55 BOOL:8B56 ' +
        'BOOL_VEC2:8B57 BOOL_VEC3:8B58 BOOL_VEC4:8B59 FLOAT_MAT2:8B5A FLOAT_MAT3:8B5B FLOAT_MAT4:8B5C SAMPLER_2D:8B5E ' +
        'SAMPLER_CUBE:8B60 VERTEX_ATTRIB_ARRAY_ENABLED:8622 VERTEX_ATTRIB_ARRAY_SIZE:8623 ' +
        'VERTEX_ATTRIB_ARRAY_STRIDE:8624 VERTEX_ATTRIB_ARRAY_TYPE:8625 VERTEX_ATTRIB_ARRAY_NORMALIZED:886A ' +
        'VERTEX_ATTRIB_ARRAY_POINTER:8645 VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:889F IMPLEMENTATION_COLOR_READ_TYPE:8B9A ' +
        'IMPLEMENTATION_COLOR_READ_FORMAT:8B9B COMPILE_STATUS:8B81 LOW_FLOAT:8DF0 MEDIUM_FLOAT:8DF1 HIGH_FLOAT:8DF2 ' +
        'LOW_INT:8DF3 MEDIUM_INT:8DF4 HIGH_INT:8DF5 FRAMEBUFFER:8D40 RENDERBUFFER:8D41 RGBA4:8056 RGB5_A1:8057 ' +
        'RGB565:8D62 DEPTH_COMPONENT16:81A5 STENCIL_INDEX8:8D48 DEPTH_STENCIL:84F9 RENDERBUFFER_WIDTH:8D42 ' +
        'RENDERBUFFER_HEIGHT:8D43 RENDERBUFFER_INTERNAL_FORMAT:8D44 RENDERBUFFER_RED_SIZE:8D50 ' +
        'RENDERBUFFER_GREEN_SIZE:8D51 RENDERBUFFER_BLUE_SIZE:8D52 RENDERBUFFER_ALPHA_SIZE:8D53 ' +
        'RENDERBUFFER_DEPTH_SIZE:8D54 RENDERBUFFER_STENCIL_SIZE:8D55 FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:8CD0 ' +
        'FRAMEBUFFER_ATTACHMENT_OBJECT_NAME:8CD1 FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:8CD2 ' +
        'FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:8CD3 COLOR_ATTACHMENT0:8CE0 DEPTH_ATTACHMENT:8D00 ' +
        'STENCIL_ATTACHMENT:8D20 DEPTH_STENCIL_ATTACHMENT:821A NONE:0 FRAMEBUFFER_COMPLETE:8CD5 ' +
        'FRAMEBUFFER_INCOMPLETE_ATTACHMENT:8CD6 FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:8CD7 ' +
        'FRAMEBUFFER_INCOMPLETE_DIMENSIONS:8CD9 FRAMEBUFFER_UNSUPPORTED:8CDD FRAMEBUFFER_BINDING:8CA6 ' +
        'RENDERBUFFER_BINDING:8CA7 MAX_RENDERBUFFER_SIZE:84E8 INVALID_FRAMEBUFFER_OPERATION:506 ' +
        'UNPACK_FLIP_Y_WEBGL:9240 UNPACK_PREMULTIPLY_ALPHA_WEBGL:9241 CONTEXT_LOST_WEBGL:9242 ' +
        'UNPACK_COLORSPACE_CONVERSION_WEBGL:9243 BROWSER_DEFAULT_WEBGL:9244';
    var C2 = 'READ_BUFFER:C02 UNPACK_ROW_LENGTH:CF2 UNPACK_SKIP_ROWS:CF3 UNPACK_SKIP_PIXELS:CF4 PACK_ROW_LENGTH:D02 ' +
        'PACK_SKIP_ROWS:D03 PACK_SKIP_PIXELS:D04 COLOR:1800 DEPTH:1801 STENCIL:1802 RED:1903 RGB8:8051 RGBA8:8058 ' +
        'RGB10_A2:8059 TEXTURE_BINDING_3D:806A UNPACK_SKIP_IMAGES:806D UNPACK_IMAGE_HEIGHT:806E TEXTURE_3D:806F ' +
        'TEXTURE_WRAP_R:8072 MAX_3D_TEXTURE_SIZE:8073 UNSIGNED_INT_2_10_10_10_REV:8368 MAX_ELEMENTS_VERTICES:80E8 ' +
        'MAX_ELEMENTS_INDICES:80E9 TEXTURE_MIN_LOD:813A TEXTURE_MAX_LOD:813B TEXTURE_BASE_LEVEL:813C ' +
        'TEXTURE_MAX_LEVEL:813D MIN:8007 MAX:8008 DEPTH_COMPONENT24:81A6 MAX_TEXTURE_LOD_BIAS:84FD ' +
        'TEXTURE_COMPARE_MODE:884C TEXTURE_COMPARE_FUNC:884D CURRENT_QUERY:8865 QUERY_RESULT:8866 ' +
        'QUERY_RESULT_AVAILABLE:8867 STREAM_READ:88E1 STREAM_COPY:88E2 STATIC_READ:88E5 STATIC_COPY:88E6 ' +
        'DYNAMIC_READ:88E9 DYNAMIC_COPY:88EA MAX_DRAW_BUFFERS:8824 MAX_FRAGMENT_UNIFORM_COMPONENTS:8B49 ' +
        'MAX_VERTEX_UNIFORM_COMPONENTS:8B4A SAMPLER_3D:8B5F SAMPLER_2D_SHADOW:8B62 FRAGMENT_SHADER_DERIVATIVE_HINT:8B8B ' +
        'PIXEL_PACK_BUFFER:88EB PIXEL_UNPACK_BUFFER:88EC PIXEL_PACK_BUFFER_BINDING:88ED PIXEL_UNPACK_BUFFER_BINDING:88EF ' +
        'FLOAT_MAT2x3:8B65 FLOAT_MAT2x4:8B66 FLOAT_MAT3x2:8B67 FLOAT_MAT3x4:8B68 FLOAT_MAT4x2:8B69 FLOAT_MAT4x3:8B6A ' +
        'SRGB:8C40 SRGB8:8C41 SRGB8_ALPHA8:8C43 COMPARE_REF_TO_TEXTURE:884E RGBA32F:8814 RGB32F:8815 RGBA16F:881A ' +
        'RGB16F:881B VERTEX_ATTRIB_ARRAY_INTEGER:88FD MAX_ARRAY_TEXTURE_LAYERS:88FF MIN_PROGRAM_TEXEL_OFFSET:8904 ' +
        'MAX_PROGRAM_TEXEL_OFFSET:8905 MAX_VARYING_COMPONENTS:8B4B TEXTURE_2D_ARRAY:8C1A TEXTURE_BINDING_2D_ARRAY:8C1D ' +
        'R11F_G11F_B10F:8C3A UNSIGNED_INT_10F_11F_11F_REV:8C3B RGB9_E5:8C3D UNSIGNED_INT_5_9_9_9_REV:8C3E ' +
        'TRANSFORM_FEEDBACK_BUFFER_MODE:8C7F MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS:8C80 ' +
        'TRANSFORM_FEEDBACK_VARYINGS:8C83 TRANSFORM_FEEDBACK_BUFFER_START:8C84 TRANSFORM_FEEDBACK_BUFFER_SIZE:8C85 ' +
        'TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN:8C88 RASTERIZER_DISCARD:8C89 ' +
        'MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS:8C8A MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS:8C8B ' +
        'INTERLEAVED_ATTRIBS:8C8C SEPARATE_ATTRIBS:8C8D TRANSFORM_FEEDBACK_BUFFER:8C8E ' +
        'TRANSFORM_FEEDBACK_BUFFER_BINDING:8C8F RGBA32UI:8D70 RGB32UI:8D71 RGBA16UI:8D76 RGB16UI:8D77 RGBA8UI:8D7C ' +
        'RGB8UI:8D7D RGBA32I:8D82 RGB32I:8D83 RGBA16I:8D88 RGB16I:8D89 RGBA8I:8D8E RGB8I:8D8F RED_INTEGER:8D94 ' +
        'RGB_INTEGER:8D98 RGBA_INTEGER:8D99 SAMPLER_2D_ARRAY:8DC1 SAMPLER_2D_ARRAY_SHADOW:8DC4 SAMPLER_CUBE_SHADOW:8DC5 ' +
        'UNSIGNED_INT_VEC2:8DC6 UNSIGNED_INT_VEC3:8DC7 UNSIGNED_INT_VEC4:8DC8 INT_SAMPLER_2D:8DCA INT_SAMPLER_3D:8DCB ' +
        'INT_SAMPLER_CUBE:8DCC INT_SAMPLER_2D_ARRAY:8DCF UNSIGNED_INT_SAMPLER_2D:8DD2 UNSIGNED_INT_SAMPLER_3D:8DD3 ' +
        'UNSIGNED_INT_SAMPLER_CUBE:8DD4 UNSIGNED_INT_SAMPLER_2D_ARRAY:8DD7 DEPTH_COMPONENT32F:8CAC ' +
        'DEPTH32F_STENCIL8:8CAD FLOAT_32_UNSIGNED_INT_24_8_REV:8DAD FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING:8210 ' +
        'FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE:8211 FRAMEBUFFER_ATTACHMENT_RED_SIZE:8212 ' +
        'FRAMEBUFFER_ATTACHMENT_GREEN_SIZE:8213 FRAMEBUFFER_ATTACHMENT_BLUE_SIZE:8214 ' +
        'FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE:8215 FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE:8216 ' +
        'FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE:8217 FRAMEBUFFER_DEFAULT:8218 UNSIGNED_INT_24_8:84FA ' +
        'DEPTH24_STENCIL8:88F0 UNSIGNED_NORMALIZED:8C17 DRAW_FRAMEBUFFER_BINDING:8CA6 READ_FRAMEBUFFER:8CA8 ' +
        'DRAW_FRAMEBUFFER:8CA9 READ_FRAMEBUFFER_BINDING:8CAA RENDERBUFFER_SAMPLES:8CAB ' +
        'FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER:8CD4 MAX_COLOR_ATTACHMENTS:8CDF FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:8D56 ' +
        'MAX_SAMPLES:8D57 HALF_FLOAT:140B RG:8227 RG_INTEGER:8228 R8:8229 RG8:822B R16F:822D R32F:822E RG16F:822F ' +
        'RG32F:8230 R8I:8231 R8UI:8232 R16I:8233 R16UI:8234 R32I:8235 R32UI:8236 RG8I:8237 RG8UI:8238 RG16I:8239 ' +
        'RG16UI:823A RG32I:823B RG32UI:823C VERTEX_ARRAY_BINDING:85B5 R8_SNORM:8F94 RG8_SNORM:8F95 RGB8_SNORM:8F96 ' +
        'RGBA8_SNORM:8F97 SIGNED_NORMALIZED:8F9C COPY_READ_BUFFER:8F36 COPY_WRITE_BUFFER:8F37 ' +
        'COPY_READ_BUFFER_BINDING:8F36 COPY_WRITE_BUFFER_BINDING:8F37 UNIFORM_BUFFER:8A11 UNIFORM_BUFFER_BINDING:8A28 ' +
        'UNIFORM_BUFFER_START:8A29 UNIFORM_BUFFER_SIZE:8A2A MAX_VERTEX_UNIFORM_BLOCKS:8A2B ' +
        'MAX_FRAGMENT_UNIFORM_BLOCKS:8A2D MAX_COMBINED_UNIFORM_BLOCKS:8A2E MAX_UNIFORM_BUFFER_BINDINGS:8A2F ' +
        'MAX_UNIFORM_BLOCK_SIZE:8A30 MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS:8A31 ' +
        'MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS:8A33 UNIFORM_BUFFER_OFFSET_ALIGNMENT:8A34 ACTIVE_UNIFORM_BLOCKS:8A36 ' +
        'UNIFORM_TYPE:8A37 UNIFORM_SIZE:8A38 UNIFORM_BLOCK_INDEX:8A3A UNIFORM_OFFSET:8A3B UNIFORM_ARRAY_STRIDE:8A3C ' +
        'UNIFORM_MATRIX_STRIDE:8A3D UNIFORM_IS_ROW_MAJOR:8A3E UNIFORM_BLOCK_BINDING:8A3F UNIFORM_BLOCK_DATA_SIZE:8A40 ' +
        'UNIFORM_BLOCK_ACTIVE_UNIFORMS:8A42 UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES:8A43 ' +
        'UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER:8A44 UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER:8A46 ' +
        'INVALID_INDEX:FFFFFFFF MAX_VERTEX_OUTPUT_COMPONENTS:9122 MAX_FRAGMENT_INPUT_COMPONENTS:9125 ' +
        'MAX_SERVER_WAIT_TIMEOUT:9111 OBJECT_TYPE:9112 SYNC_CONDITION:9113 SYNC_STATUS:9114 SYNC_FLAGS:9115 ' +
        'SYNC_FENCE:9116 SYNC_GPU_COMMANDS_COMPLETE:9117 UNSIGNALED:9118 SIGNALED:9119 ALREADY_SIGNALED:911A ' +
        'TIMEOUT_EXPIRED:911B CONDITION_SATISFIED:911C WAIT_FAILED:911D SYNC_FLUSH_COMMANDS_BIT:1 ' +
        'VERTEX_ATTRIB_ARRAY_DIVISOR:88FE ANY_SAMPLES_PASSED:8C2F ANY_SAMPLES_PASSED_CONSERVATIVE:8D6A ' +
        'SAMPLER_BINDING:8919 RGB10_A2UI:906F INT_2_10_10_10_REV:8D9F TRANSFORM_FEEDBACK:8E22 ' +
        'TRANSFORM_FEEDBACK_PAUSED:8E23 TRANSFORM_FEEDBACK_ACTIVE:8E24 TRANSFORM_FEEDBACK_BINDING:8E25 ' +
        'TEXTURE_IMMUTABLE_FORMAT:912F MAX_ELEMENT_INDEX:8D6B TEXTURE_IMMUTABLE_LEVELS:82DF ' +
        'MAX_CLIENT_WAIT_TIMEOUT_WEBGL:9247';
    function constants(list, onto) {
        list.split(' ').forEach(function (kv) {
            var i = kv.indexOf(':'), v = parseInt(kv.slice(i + 1), 16) >>> 0;
            onto.forEach(function (o) {
                Object.defineProperty(o, kv.slice(0, i), { value: v, enumerable: true });
            });
        });
    }

    /* ---- the objects a context gives out ---- */
    var KIND = { b: 0, f: 1, r: 2, t: 3, s: 4, p: 5, l: 6, v: 7, q: 8, m: 9, y: 10, x: 11 };
    function objectClass(name) {
        var F = function () { illegal(); };
        Object.defineProperty(F, 'name', { value: name });
        Object.defineProperty(F.prototype, Symbol.toStringTag, { value: name, configurable: true });
        expose(name, F);
        return F;
    }
    var GLObject = objectClass('WebGLObject');
    var CLASS = {
        b: objectClass('WebGLBuffer'), f: objectClass('WebGLFramebuffer'), r: objectClass('WebGLRenderbuffer'),
        t: objectClass('WebGLTexture'), s: objectClass('WebGLShader'), p: objectClass('WebGLProgram'),
        l: objectClass('WebGLUniformLocation'), v: objectClass('WebGLVertexArrayObject'), q: objectClass('WebGLQuery'),
        m: objectClass('WebGLSampler'), y: objectClass('WebGLSync'), x: objectClass('WebGLTransformFeedback')
    };
    Object.keys(CLASS).forEach(function (k) {
        if (k !== 'l') Object.setPrototypeOf(CLASS[k].prototype, GLObject.prototype);
    });
    var ActiveInfo = objectClass('WebGLActiveInfo');
    var PrecisionFormat = objectClass('WebGLShaderPrecisionFormat');
    function make(F, props) {
        var o = Object.create(F.prototype);
        for (var k in props) Object.defineProperty(o, k, { value: props[k], enumerable: true });
        return o;
    }

    var S = Symbol('webgl');        /* a context's state, and an object's */
    function obj(ctx, kind, extra) {
        var st = ctx[S];
        var o = Object.create(CLASS[kind].prototype);
        define(o, S, { ctx: ctx, kind: kind, id: ++st.next[kind], deleted: false });
        if (extra) for (var k in extra) o[S][k] = extra[k];
        return o;
    }
    /* The id of an object passed to a call (0 for null); a TypeError if it
     * isn't the right kind (WebGL's IDL); another context's, or a deleted
     * one, goes as an id the web-GPU process doesn't know, and ANGLE
     * refuses it. */
    function id(ctx, kind, o) {
        if (o === null || o === undefined) return 0;
        if (!(o instanceof CLASS[kind]) || !o[S]) throw new TypeError('parameter is not of type ' + CLASS[kind].name);
        var s = o[S];
        return s.ctx === ctx && !s.deleted ? s.id : 0x7fffffff;
    }

    /* ---- recording ---- */
    function ensure(st, n) {
        if (st.len + n <= st.buf.length) return;
        var cap = st.buf.length * 2;
        while (cap < st.len + n) cap *= 2;
        var b = new Uint8Array(cap);
        b.set(st.buf.subarray(0, st.len));
        st.buf = b;
        st.dv = new DataView(b.buffer);
    }
    function head(st, op, n) {
        ensure(st, 8 + n);
        st.dv.setUint32(st.len, op, true);
        st.dv.setUint32(st.len + 4, n, true);
        st.len += 8;
    }
    function u32(st, v) { st.dv.setUint32(st.len, v >>> 0, true); st.len += 4; }
    function i32(st, v) { st.dv.setInt32(st.len, v | 0, true); st.len += 4; }
    function f32(st, v) { st.dv.setFloat32(st.len, +v, true); st.len += 4; }
    function i64(st, v) {
        v = Number(v) || 0;
        var lo = v >>> 0, hi = Math.floor(v / 4294967296);
        st.dv.setUint32(st.len, lo, true);
        st.dv.setInt32(st.len + 4, hi | 0, true);
        st.len += 8;
    }
    function bytes(st, b) { ensure(st, b.length); st.buf.set(b, st.len); st.len += b.length; }
    function rec(ctx, op, kinds, args) {
        var st = ctx[S];
        if (st.lost) return;
        var n = 0;
        for (var i = 0; i < kinds.length; i++) n += kinds[i] === 'p' ? 8 : 4;
        head(st, op, n);
        for (i = 0; i < kinds.length; i++) {
            var k = kinds[i], v = args[i];
            if (k === 'f') f32(st, v);
            else if (k === 'p') i64(st, v);
            else if (k === 'i' || k === 'z') i32(st, Number(v));
            else if (k === 'b') u32(st, v ? 1 : 0);
            else u32(st, Number(v));
        }
        soon(ctx);
    }
    /* A special op: u32 words ({v} for a 64-bit one), then bytes. */
    function special(ctx, op, words, data) {
        var st = ctx[S];
        if (st.lost) return;
        var n = (data ? data.length : 0);
        words.forEach(function (w) { n += typeof w === 'object' ? 8 : 4; });
        head(st, op, n);
        words.forEach(function (w) {
            if (typeof w === 'object') i64(st, w.v);
            else { st.dv.setUint32(st.len, w >>> 0, true); st.len += 4; }
        });
        if (data) bytes(st, data);
        soon(ctx);
    }
    function utf8(s) {
        s = String(s);
        if (typeof TextEncoder === 'function') return new TextEncoder().encode(s);
        var out = [];
        for (var i = 0; i < s.length; i++) {
            var c = s.charCodeAt(i);
            if (c < 0x80) out.push(c);
            else if (c < 0x800) out.push(0xc0 | c >> 6, 0x80 | c & 63);
            else out.push(0xe0 | c >> 12, 0x80 | c >> 6 & 63, 0x80 | c & 63);
        }
        return new Uint8Array(out);
    }
    /* UTF-8 bytes as a string (by hand: the engine's TextDecoder misreads views). */
    function fromUtf8(b, at, n) {
        var s = '', end = at + n;
        for (var i = at; i < end; ) {
            var c = b[i++];
            if (c >= 0xf0 && i + 2 < end) {
                c = (c & 7) << 18 | (b[i] & 63) << 12 | (b[i + 1] & 63) << 6 | b[i + 2] & 63; i += 3;
                c -= 0x10000; s += String.fromCharCode(0xd800 + (c >> 10), 0xdc00 + (c & 1023)); continue;
            }
            if (c >= 0xe0) { c = (c & 15) << 12 | (b[i] & 63) << 6 | b[i + 1] & 63; i += 2; }
            else if (c >= 0xc0) { c = (c & 31) << 6 | b[i] & 63; i += 1; }
            s += String.fromCharCode(c);
        }
        return s;
    }

    /* Send what is recorded; with a reply, the results of its queries. */
    function flush(ctx, reply) {
        var st = ctx[S];
        if (st.lost) return null;
        if (!st.len && !reply) return null;
        var r = native.glRun(st.cid, st.buf.subarray(0, st.len), !!reply);
        st.len = 0;
        checkLost();
        if (reply && r === null) return null;
        return r;
    }
    /* A query: recorded, sent with what came before, answered. */
    function query(ctx, op, words, data) {
        special(ctx, op, words, data);
        var r = flush(ctx, true);
        return r ? new Reader(r) : null;
    }
    function Reader(ab) { this.b = new Uint8Array(ab); this.dv = new DataView(ab); this.at = 0; }
    Reader.prototype.u32 = function () { var v = this.dv.getUint32(this.at, true); this.at += 4; return v; };
    Reader.prototype.i32 = function () { var v = this.dv.getInt32(this.at, true); this.at += 4; return v; };
    Reader.prototype.f32 = function () { var v = this.dv.getFloat32(this.at, true); this.at += 4; return v; };
    Reader.prototype.str = function () { var n = this.u32(); var s = fromUtf8(this.b, this.at, n); this.at += n; return s; };
    Reader.prototype.bytes = function () { var n = this.u32(); var s = this.b.slice(this.at, this.at + n); this.at += n; return s; };

    /* At the end of the task: what is recorded goes, and a canvas drawn to
     * is presented. */
    var pending = [];
    function soon(ctx) {
        var st = ctx[S];
        if (st.queued) return;
        st.queued = true;
        pending.push(ctx);
        if (pending.length === 1) Promise.resolve().then(endOfTask);
    }
    function endOfTask() {
        var list = pending;
        pending = [];
        list.forEach(function (ctx) {
            var st = ctx[S];
            st.queued = false;
            flush(ctx, false);
            if (st.dirty && !st.lost) {
                st.dirty = false;
                if (!native.present(st.cid, st.canvas)) checkLost(true);
            }
        });
    }
    function dirty(ctx) { ctx[S].dirty = true; }

    /* Contexts the web-GPU process lost (a GPU reset), or all of them (it
     * is gone): lost, with webglcontextlost at their canvases. */
    var contexts = [];
    function checkLost(all) {
        var lost = native.lost();
        if (!lost.length && !all) return;
        contexts.forEach(function (ctx) {
            var st = ctx[S];
            if (st.lost || !(all || lost.indexOf(-1) >= 0 || lost.indexOf(st.cid) >= 0)) return;
            st.lost = true;
            st.lostError = true;
            var ev;
            try { ev = new (global.WebGLContextEvent || global.Event)('webglcontextlost', { cancelable: true, statusMessage: '' }); }
            catch (e) { ev = null; }
            if (ev && st.canvas.dispatchEvent) global.setTimeout(function () { st.canvas.dispatchEvent(ev); }, 0);
        });
    }

    /* ---- the contexts ---- */
    function WebGLRenderingContext() { illegal(); }
    function WebGL2RenderingContext() { illegal(); }
    var B = {};                 /* what both versions have */
    var P1 = WebGLRenderingContext.prototype = Object.create(B);
    var P2 = WebGL2RenderingContext.prototype = Object.create(B);
    define(P1, 'constructor', WebGLRenderingContext);
    define(P2, 'constructor', WebGL2RenderingContext);
    Object.defineProperty(P1, Symbol.toStringTag, { value: 'WebGLRenderingContext', configurable: true });
    Object.defineProperty(P2, Symbol.toStringTag, { value: 'WebGL2RenderingContext', configurable: true });
    constants(C1, [B, WebGLRenderingContext, WebGL2RenderingContext]);
    constants(C2, [P2, WebGL2RenderingContext]);
    for (var t = 0; t < 32; t++) {
        [B, WebGLRenderingContext, WebGL2RenderingContext].forEach(function (o) {
            Object.defineProperty(o, 'TEXTURE' + t, { value: 0x84C0 + t, enumerable: true });
        });
        if (t < 16) [P2, WebGL2RenderingContext].forEach(function (o) {
            Object.defineProperty(o, 'COLOR_ATTACHMENT' + t, { value: 0x8CE0 + t, enumerable: true });
            Object.defineProperty(o, 'DRAW_BUFFER' + t, { value: 0x8825 + t, enumerable: true });
        });
    }
    Object.defineProperty(P2, 'TIMEOUT_IGNORED', { value: -1, enumerable: true });
    Object.defineProperty(WebGL2RenderingContext, 'TIMEOUT_IGNORED', { value: -1, enumerable: true });
    gen(B, P2, rec, id);

    function create(canvas, version, attrs) {
        if (!native.available()) return null;
        attrs = attrs || {};
        var a = {
            alpha: attrs.alpha !== false, depth: attrs.depth !== false, stencil: !!attrs.stencil,
            antialias: attrs.antialias !== false, premultipliedAlpha: attrs.premultipliedAlpha !== false,
            preserveDrawingBuffer: !!attrs.preserveDrawingBuffer, powerPreference: attrs.powerPreference || 'default',
            failIfMajorPerformanceCaveat: !!attrs.failIfMajorPerformanceCaveat, desynchronized: false
        };
        var cid = native.newId();
        if (!cid) return null;
        var flags = (a.alpha ? 1 : 0) | (a.depth ? 2 : 0) | (a.stencil ? 4 : 0) | (a.antialias ? 8 : 0) |
                    (a.preserveDrawingBuffer ? 16 : 0);
        var ok = native.glCreate(cid, version, canvas.width, canvas.height, flags);
        if (ok !== true) {
            var ev;
            try { ev = new (global.WebGLContextEvent || global.Event)('webglcontextcreationerror', { statusMessage: String(ok) }); }
            catch (e) { ev = null; }
            if (ev && canvas.dispatchEvent) canvas.dispatchEvent(ev);
            return null;
        }
        var ctx = Object.create(version === 2 ? P2 : P1);
        var buf = new Uint8Array(65536);
        define(ctx, S, {
            cid: cid, version: version, canvas: canvas, attrs: a, buf: buf, dv: new DataView(buf.buffer), len: 0,
            next: {}, lost: false, lostError: false, queued: false, dirty: false,
            width: canvas.width, height: canvas.height, ext: null, enabled: {},
            buffers: {}, program: null, framebuffer: null, readFramebuffer: null, renderbuffer: null,
            unit: 0, textures: [], vao: null, defaultElements: null, samplers: [], tf: null,
            unpack: { flipY: false, premultiply: false, colorspace: 0x9244, alignment: 4 }, shaders: new WeakMap()
        });
        'bfrtsplvqmyx'.split('').forEach(function (k) { ctx[S].next[k] = 0; });
        contexts.push(ctx);
        return ctx;
    }

    /* Draw calls past the plain ones also make a frame. */
    function markDraws(names) {
        names.forEach(function (n) {
            var f = B[n] || P2[n];
            if (!f) return;
            var host = B[n] ? B : P2;
            define(host, n, function () { dirty(this); return f.apply(this, arguments); });
        });
    }
    markDraws(['clear', 'drawArrays', 'drawElements', 'drawArraysInstanced', 'drawElementsInstanced',
               'drawRangeElements', 'blitFramebuffer', 'clearBufferfi']);

    function st(ctx) {
        var s = ctx && ctx[S];
        if (!s) throw new TypeError('Illegal invocation');
        return s;
    }

    /* Bindings, kept here to answer getParameter. */
    function wrap(host, name, after) {
        var f = host[name];
        define(host, name, function () {
            var r = f.apply(this, arguments);
            after.apply(this, [st(this)].concat(Array.prototype.slice.call(arguments)));
            return r;
        });
    }
    wrap(B, 'bindBuffer', function (s, target, b) {
        if (target === 0x8893) { if (s.vao) s.vao[S].elements = b; else s.defaultElements = b; }
        else s.buffers[target] = b;
    });
    wrap(B, 'bindFramebuffer', function (s, target, f) {
        if (target === 0x8D40 || target === 0x8CA9) s.framebuffer = f;
        if (target === 0x8D40 || target === 0x8CA8) s.readFramebuffer = f;
    });
    wrap(B, 'bindRenderbuffer', function (s, target, r) { s.renderbuffer = r; });
    wrap(B, 'activeTexture', function (s, unit) { s.unit = (unit - 0x84C0) >>> 0; });
    wrap(B, 'bindTexture', function (s, target, tx) {
        (s.textures[s.unit] = s.textures[s.unit] || {})[target] = tx;
        if (tx && tx[S] && !tx[S].target) tx[S].target = target;
    });
    wrap(B, 'useProgram', function (s, p) { s.program = p; });
    wrap(P2, 'bindVertexArray', function (s, v) { s.vao = v; });
    wrap(P2, 'bindSampler', function (s, unit, m) { s.samplers[unit] = m; });
    wrap(P2, 'bindTransformFeedback', function (s, target, x) { s.tf = x; });
    wrap(P2, 'bindBufferBase', function (s, target, index, b) { s.buffers[target] = b; });
    wrap(P2, 'bindBufferRange', function (s, target, index, b) { s.buffers[target] = b; });
    /* A buffer is for indices or for everything else, from its first binding
     * (WebGL 1 section 6.1, WebGL 2 section 5.1; the copy targets take
     * either). A binding against that is INVALID_OPERATION, kept here for
     * getError. */
    function kindCheck(name, targetArg, bufferArg) {
        var f = B[name] || P2[name];
        if (!f) return;
        define(B[name] ? B : P2, name, function () {
            var target = arguments[targetArg] >>> 0, b = arguments[bufferArg], o = b && b[S];
            if (o && o.kind === 'b' && o.ctx === this && target !== 0x8F36 && target !== 0x8F37) {
                var kind = target === 0x8893 ? 'e' : 'o';
                if (o.use && o.use !== kind) { synthError(this, 0x0502); return; }
                o.use = kind;
            }
            return f.apply(this, arguments);
        });
    }
    kindCheck('bindBuffer', 0, 1);
    kindCheck('bindBufferBase', 0, 2);
    kindCheck('bindBufferRange', 0, 2);
    function synthError(ctx, code) {
        var s = st(ctx);
        if (!s.synthError) s.synthError = code;
    }
    wrap(B, 'enable', function (s, cap) { s.enabled[cap] = true; });
    wrap(B, 'disable', function (s, cap) { s.enabled[cap] = false; });

    /* ---- objects ---- */
    var CREATE = { createBuffer: 'b', createFramebuffer: 'f', createRenderbuffer: 'r', createTexture: 't',
                   createProgram: 'p' };
    var CREATE2 = { createVertexArray: 'v', createQuery: 'q', createSampler: 'm', createTransformFeedback: 'x' };
    function creator(host, map) {
        Object.keys(map).forEach(function (name) {
            define(host, name, function () {
                var k = map[name], s = st(this);
                if (s.lost) return null;
                var o = obj(this, k);
                special(this, 256, [KIND[k], o[S].id, 0]);
                return o;
            });
        });
    }
    creator(B, CREATE);
    creator(P2, CREATE2);
    define(B, 'createShader', function (type) {
        var s = st(this);
        if (s.lost) return null;
        var o = obj(this, 's', { type: type >>> 0, source: '' });
        special(this, 256, [KIND.s, o[S].id, type >>> 0]);
        return o;
    });
    function deleter(host, name, k, unbind) {
        define(host, name, function (o) {
            if (o === null || o === undefined) return;
            var i = id(this, k, o);
            if (i === 0x7fffffff) return;
            o[S].deleted = true;
            special(this, 257, [KIND[k], i]);
            if (unbind) unbind(st(this), o);
        });
    }
    deleter(B, 'deleteBuffer', 'b', function (s, o) {
        for (var t in s.buffers) if (s.buffers[t] === o) s.buffers[t] = null;
        if (s.defaultElements === o) s.defaultElements = null;
    });
    deleter(B, 'deleteFramebuffer', 'f', function (s, o) {
        if (s.framebuffer === o) s.framebuffer = null;
        if (s.readFramebuffer === o) s.readFramebuffer = null;
    });
    deleter(B, 'deleteRenderbuffer', 'r', function (s, o) { if (s.renderbuffer === o) s.renderbuffer = null; });
    deleter(B, 'deleteTexture', 't', function (s, o) {
        s.textures.forEach(function (u) { for (var t in u) if (u[t] === o) u[t] = null; });
    });
    deleter(B, 'deleteShader', 's');
    deleter(B, 'deleteProgram', 'p', function (s, o) { if (s.program === o) s.program = null; });
    deleter(P2, 'deleteVertexArray', 'v', function (s, o) { if (s.vao === o) s.vao = null; });
    deleter(P2, 'deleteQuery', 'q');
    deleter(P2, 'deleteSampler', 'm');
    deleter(P2, 'deleteTransformFeedback', 'x');
    function is(host, name, k) {
        define(host, name, function (o) {
            if (!o || !(o instanceof CLASS[k]) || o[S].ctx !== this || o[S].deleted || st(this).lost) return false;
            var r = query(this, 315, [KIND[k], o[S].id]);
            return !!(r && r.u32());
        });
    }
    is(B, 'isBuffer', 'b'); is(B, 'isFramebuffer', 'f'); is(B, 'isRenderbuffer', 'r'); is(B, 'isTexture', 't');
    is(B, 'isShader', 's'); is(B, 'isProgram', 'p'); is(P2, 'isVertexArray', 'v'); is(P2, 'isQuery', 'q');
    is(P2, 'isSampler', 'm'); is(P2, 'isTransformFeedback', 'x'); is(P2, 'isSync', 'y');

    /* ---- shaders and programs ---- */
    define(B, 'shaderSource', function (sh, src) {
        var i = id(this, 's', sh);
        if (sh) sh[S].source = String(src);
        special(this, 258, [i], utf8(src));
    });
    define(B, 'getShaderSource', function (sh) { id(this, 's', sh); return sh ? sh[S].source : null; });
    define(B, 'bindAttribLocation', function (p, index, name) {
        special(this, 259, [id(this, 'p', p), index >>> 0], utf8(name));
    });
    function param(op, k) {
        return function (o, pname) {
            if (st(this).lost) return null;
            var r = query(this, op, [id(this, k, o), pname >>> 0]);
            if (!r) return null;
            var v = r.i32();
            pname >>>= 0;
            /* booleans */
            if (pname === 0x8B81 || pname === 0x8B80 || pname === 0x8B82 || pname === 0x8B83) return !!v;
            return pname === 0x8B4F || pname === 0x8C7F ? v >>> 0 : v;
        };
    }
    define(B, 'getShaderParameter', param(305, 's'));
    define(B, 'getProgramParameter', param(307, 'p'));
    define(B, 'getShaderInfoLog', function (sh) {
        var r = st(this).lost ? null : query(this, 306, [id(this, 's', sh)]);
        return r ? r.str() : null;
    });
    define(B, 'getProgramInfoLog', function (p) {
        var r = st(this).lost ? null : query(this, 308, [id(this, 'p', p)]);
        return r ? r.str() : null;
    });
    define(B, 'getUniformLocation', function (p, name) {
        if (st(this).lost) return null;
        var pid = id(this, 'p', p);
        var loc = obj(this, 'l', { program: p });
        var r = query(this, 309, [pid, loc[S].id], utf8(name));
        return r && r.u32() ? loc : null;
    });
    function located(op, nameKind) {
        return function (p, name) {
            if (st(this).lost) return -1;
            var r = query(this, op, [id(this, 'p', p)], utf8(name));
            return r ? (nameKind ? r.u32() : r.i32()) : -1;
        };
    }
    define(B, 'getAttribLocation', located(310));
    define(P2, 'getFragDataLocation', located(330));
    define(P2, 'getUniformBlockIndex', located(331, true));
    function active(op) {
        return function (p, index) {
            if (st(this).lost) return null;
            var r = query(this, op, [id(this, 'p', p), index >>> 0]);
            if (!r) return null;
            var size = r.i32(), type = r.u32(), name = r.str();
            return type ? make(ActiveInfo, { size: size, type: type, name: name }) : null;
        };
    }
    define(B, 'getActiveAttrib', active(311));
    define(B, 'getActiveUniform', active(312));
    define(P2, 'getTransformFeedbackVarying', active(336));
    define(B, 'getAttachedShaders', function (p) {
        id(this, 'p', p);
        var out = [];
        if (!p) return null;
        (p[S].shaders || []).forEach(function (sh) { if (!sh[S].deleted) out.push(sh); });
        return out;
    });
    wrap(B, 'attachShader', function (s, p, sh) { if (p && sh) (p[S].shaders = p[S].shaders || []).push(sh); });
    wrap(B, 'detachShader', function (s, p, sh) {
        if (p && p[S].shaders) p[S].shaders = p[S].shaders.filter(function (x) { return x !== sh; });
    });
    define(B, 'getShaderPrecisionFormat', function (stype, ptype) {
        var r = st(this).lost ? null : query(this, 320, [stype >>> 0, ptype >>> 0]);
        if (!r) return null;
        return make(PrecisionFormat, { rangeMin: r.i32(), rangeMax: r.i32(), precision: r.i32() });
    });

    /* ---- buffers ---- */
    function view(v) {
        if (v instanceof ArrayBuffer || (typeof SharedArrayBuffer === 'function' && v instanceof SharedArrayBuffer))
            return new Uint8Array(v);
        if (ArrayBuffer.isView(v)) return new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
        return null;
    }
    function sub(v, srcOffset, length) {
        if (!ArrayBuffer.isView(v) || srcOffset === undefined) return view(v);
        var per = v.BYTES_PER_ELEMENT || 1;
        var start = (srcOffset >>> 0) * per;
        var len = length ? (length >>> 0) * per : v.byteLength - start;
        return new Uint8Array(v.buffer, v.byteOffset + start, Math.max(0, Math.min(len, v.byteLength - start)));
    }
    define(B, 'bufferData', function (target, data, usage, srcOffset, length) {
        if (typeof data === 'number') {
            special(this, 260, [target >>> 0, usage >>> 0, { v: data }, 0]);
            return;
        }
        /* BufferSource? data: null (or undefined) is INVALID_VALUE, as a negative size is. */
        if (data === null || data === undefined) {
            special(this, 260, [target >>> 0, usage >>> 0, { v: -1 }, 0]);
            return;
        }
        var b = sub(data, srcOffset, length);
        if (!b) {
            /* Anything else picks the size overload: "42" is 42, NaN is 0. */
            var n = Math.trunc(+data);
            special(this, 260, [target >>> 0, usage >>> 0, { v: isFinite(n) ? n : 0 }, 0]);
            return;
        }
        special(this, 260, [target >>> 0, usage >>> 0, { v: b.length }, 1], b);
    });
    define(B, 'bufferSubData', function (target, offset, data, srcOffset, length) {
        var b = sub(data, srcOffset, length);
        if (!b) throw new TypeError("bufferSubData: parameter 3 is not of type 'ArrayBuffer'.");
        special(this, 261, [target >>> 0, { v: offset }], b);
    });
    define(P2, 'getBufferSubData', function (target, srcByteOffset, dst, dstOffset, length) {
        var d = sub(dst, dstOffset, length);
        if (!d || st(this).lost) return;
        var r = query(this, 328, [target >>> 0, { v: srcByteOffset }, d.length]);
        if (r) d.set(r.bytes().subarray(0, d.length));
    });

    /* ---- textures ---- */
    define(B, 'pixelStorei', function (pname, v) {
        var s = st(this);
        pname >>>= 0;
        if (pname === 0x9240) { s.unpack.flipY = !!v; return; }
        if (pname === 0x9241) { s.unpack.premultiply = !!v; return; }
        if (pname === 0x9243) { s.unpack.colorspace = v >>> 0; return; }
        if (pname === 0x0CF5) s.unpack.alignment = v | 0;
        special(this, 274, [pname, v | 0]);
    });
    /* A TexImageSource as RGBA bytes, after FLIP_Y and PREMULTIPLY_ALPHA. */
    function sourcePixels(s, src) {
        var w, h, data;
        if (src && src.data && typeof src.width === 'number' && typeof src.height === 'number') {
            w = src.width; h = src.height; data = new Uint8Array(src.data.buffer || src.data, src.data.byteOffset || 0, w * h * 4);
        } else if (global.HTMLCanvasElement && src instanceof global.HTMLCanvasElement) {
            var cv = canvasPixels(src);
            w = cv.w; h = cv.h; data = cv.data;
        } else {
            var img = src ? native.imageSource(src) : null;
            if (img) { w = img.width; h = img.height; data = new Uint8Array(img.data); }
            else {
                w = src && src.width >>> 0 || 0; h = src && src.height >>> 0 || 0;
                data = new Uint8Array(w * h * 4);
            }
        }
        var out = new Uint8Array(data.length);
        for (var y = 0; y < h; y++) {
            var from = (s.unpack.flipY ? h - 1 - y : y) * w * 4;
            out.set(data.subarray(from, from + w * 4), y * w * 4);
        }
        if (s.unpack.premultiply)
            for (var i = 0; i < out.length; i += 4) {
                var a = out[i + 3] / 255;
                out[i] = Math.round(out[i] * a); out[i + 1] = Math.round(out[i + 1] * a); out[i + 2] = Math.round(out[i + 2] * a);
            }
        return { w: w, h: h, data: out };
    }
    /* A canvas as RGBA bytes, top row first: a WebGL canvas's drawing buffer,
     * a 2D canvas's pixels, or transparent black (a WebGPU canvas too). */
    function canvasPixels(cv) {
        var w = cv.width >>> 0, h = cv.height >>> 0, c = cv[GLCTX];
        if (c && c.kind !== 'gpu') {
            var g = c.ctx, gs = g[S];
            if (!gs.lost) {
                w = gs.width; h = gs.height;
                var px = new Uint8Array(w * h * 4);
                /* WebGL 1 has one framebuffer binding; WebGL 2 reads through READ_FRAMEBUFFER. */
                var target = c.kind === 'gl2' ? 0x8CA8 : 0x8D40;
                var fb = gs.readFramebuffer, align = g.getParameter(0x0D05);
                g.bindFramebuffer(target, null);
                g.pixelStorei(0x0D05, 4);
                g.readPixels(0, 0, w, h, 0x1908, 0x1401, px);
                g.pixelStorei(0x0D05, align);
                g.bindFramebuffer(target, fb || null);
                var out = new Uint8Array(px.length);
                for (var y = 0; y < h; y++) out.set(px.subarray((h - 1 - y) * w * 4, (h - y) * w * 4), y * w * 4);
                return { w: w, h: h, data: out };
            }
        } else if (cv[OTHER] && w && h) {
            var d = cv.getContext('2d').getImageData(0, 0, w, h);
            return { w: w, h: h, data: new Uint8Array(d.data.buffer, d.data.byteOffset, w * h * 4) };
        }
        return { w: w, h: h, data: new Uint8Array(w * h * 4) };
    }
    /* RGBA bytes as format (UNSIGNED_BYTE): what a source becomes. */
    function convert(px, format) {
        var n = px.w * px.h, d = px.data, out, i;
        switch (format) {
        case 0x1907: case 0x8D98: out = new Uint8Array(n * 3); for (i = 0; i < n; i++) { out[i * 3] = d[i * 4]; out[i * 3 + 1] = d[i * 4 + 1]; out[i * 3 + 2] = d[i * 4 + 2]; } return out;
        case 0x1906: case 0x1909: case 0x1903: case 0x8D94:
            out = new Uint8Array(n); for (i = 0; i < n; i++) out[i] = d[i * 4 + (format === 0x1906 ? 3 : 0)]; return out;
        case 0x190A: case 0x8227: case 0x8228:
            out = new Uint8Array(n * 2); for (i = 0; i < n; i++) { out[i * 2] = d[i * 4]; out[i * 2 + 1] = d[i * 4 + (format === 0x190A ? 3 : 1)]; } return out;
        default: return d;
        }
    }
    function isSource(v) { return v !== null && typeof v === 'object' && !ArrayBuffer.isView(v) && !(v instanceof ArrayBuffer); }
    define(B, 'texImage2D', function (target, level, internalformat, a, b, c, d, e, f, g) {
        var s = st(this);
        if (arguments.length === 6) {
            /* (target, level, internalformat, format, type, source) */
            var px = sourcePixels(s, c);
            var data = convert(px, a >>> 0);
            special(this, 262, [target >>> 0, level | 0, internalformat | 0, px.w, px.h, 0, a >>> 0, b >>> 0, 1], data);
            return;
        }
        /* (target, level, internalformat, width, height, border, format, type, pixels[, srcOffset]) */
        if (isSource(f)) {
            var px2 = sourcePixels(s, f);
            special(this, 262, [target >>> 0, level | 0, internalformat | 0, a | 0, b | 0, c | 0, d >>> 0, e >>> 0, 1], convert(px2, d >>> 0));
            return;
        }
        if (typeof f === 'number') {                /* from the bound PIXEL_UNPACK_BUFFER at offset f */
            special(this, 262, [target >>> 0, level | 0, internalformat | 0, a | 0, b | 0, c | 0, d >>> 0, e >>> 0, 0]);
            return;
        }
        var pixels = f === null || f === undefined ? null : sub(f, g);
        special(this, 262, [target >>> 0, level | 0, internalformat | 0, a | 0, b | 0, c | 0, d >>> 0, e >>> 0, pixels ? 1 : 0],
                pixels ? flipView(s, pixels, a | 0, b | 0, d >>> 0, e >>> 0) : null);
    });
    define(B, 'texSubImage2D', function (target, level, x, y, a, b, c, d, e, f) {
        var s = st(this);
        if (arguments.length === 7) {
            /* (target, level, x, y, format, type, source) */
            var px = sourcePixels(s, c);
            special(this, 263, [target >>> 0, level | 0, x | 0, y | 0, px.w, px.h, a >>> 0, b >>> 0], convert(px, a >>> 0));
            return;
        }
        if (isSource(e)) {
            var px2 = sourcePixels(s, e);
            special(this, 263, [target >>> 0, level | 0, x | 0, y | 0, a | 0, b | 0, c >>> 0, d >>> 0], convert(px2, c >>> 0));
            return;
        }
        var pixels = typeof e === 'number' ? new Uint8Array(0) : sub(e, f);
        if (!pixels) throw new TypeError('texSubImage2D: no pixels');
        special(this, 263, [target >>> 0, level | 0, x | 0, y | 0, a | 0, b | 0, c >>> 0, d >>> 0],
                flipView(s, pixels, a | 0, b | 0, c >>> 0, d >>> 0));
    });
    /* FLIP_Y applies to typed arrays too: rows of RGBA/RGB/... bytes. */
    function flipView(s, px, w, h, format, type) {
        if (!s.unpack.flipY || h < 2) return px;
        var per = { 0x1908: 4, 0x1907: 3, 0x1906: 1, 0x1909: 1, 0x190A: 2, 0x1903: 1, 0x8227: 2 }[format] || 4;
        if (type === 0x8363 || type === 0x8033 || type === 0x8034) per = 2;     /* packed 16-bit */
        else if (type !== 0x1401) per *= type === 0x1406 ? 4 : 2;
        /* Rows are padded to UNPACK_ALIGNMENT, but the last needs only its pixels. */
        var align = s.unpack.alignment || 4, used = w * per, row = Math.ceil(used / align) * align;
        if ((h - 1) * row + used > px.length) return px;
        var out = new Uint8Array(px.length);
        for (var y = 0; y < h; y++) out.set(px.subarray((h - 1 - y) * row, (h - 1 - y) * row + used), y * row);
        return out;
    }
    define(B, 'compressedTexImage2D', function (target, level, internalformat, w, h, border, data, srcOffset, length) {
        var b = sub(data, srcOffset, length) || new Uint8Array(0);
        special(this, 264, [target >>> 0, level | 0, internalformat >>> 0, w | 0, h | 0, border | 0], b);
    });
    define(B, 'compressedTexSubImage2D', function (target, level, x, y, w, h, format, data, srcOffset, length) {
        var b = sub(data, srcOffset, length) || new Uint8Array(0);
        special(this, 265, [target >>> 0, level | 0, x | 0, y | 0, w | 0, h | 0, format >>> 0], b);
    });
    define(P2, 'texImage3D', function (target, level, internalformat, w, h, depth, border, format, type, src, srcOffset) {
        var b = src === null || src === undefined || typeof src === 'number' ? null : sub(src, srcOffset);
        special(this, 268, [target >>> 0, level | 0, internalformat | 0, w | 0, h | 0, depth | 0, border | 0, format >>> 0,
                            type >>> 0, b ? 1 : 0], b);
    });
    define(P2, 'texSubImage3D', function (target, level, x, y, z, w, h, depth, format, type, src, srcOffset) {
        var b = src === null || src === undefined || typeof src === 'number' ? new Uint8Array(0) : sub(src, srcOffset);
        special(this, 269, [target >>> 0, level | 0, x | 0, y | 0, z | 0, w | 0, h | 0, depth | 0, format >>> 0, type >>> 0], b);
    });

    /* ---- uniforms and attributes ---- */
    function locId(ctx, loc) {
        if (loc === null || loc === undefined) return 0;
        if (!(loc instanceof CLASS.l)) throw new TypeError('parameter 1 is not of type WebGLUniformLocation.');
        return loc[S].ctx === ctx ? loc[S].id : 0x7fffffff;
    }
    function values(v, Kind, srcOffset, srcLength) {
        var a;
        if (v instanceof Kind) a = v;
        else if (ArrayBuffer.isView(v)) a = new Kind(v);
        else if (v && typeof v.length === 'number') a = new Kind(Array.prototype.slice.call(v));
        else throw new TypeError('not a sequence');
        if (srcOffset !== undefined) a = a.subarray(srcOffset >>> 0, srcLength ? (srcOffset >>> 0) + (srcLength >>> 0) : undefined);
        return new Uint8Array(a.buffer, a.byteOffset, a.byteLength);
    }
    [['1f', 1, Float32Array], ['2f', 2, Float32Array], ['3f', 3, Float32Array], ['4f', 4, Float32Array],
     ['1i', 5, Int32Array], ['2i', 6, Int32Array], ['3i', 7, Int32Array], ['4i', 8, Int32Array],
     ['1ui', 9, Uint32Array], ['2ui', 10, Uint32Array], ['3ui', 11, Uint32Array], ['4ui', 12, Uint32Array]]
        .forEach(function (u) {
            define(u[1] > 8 ? P2 : B, 'uniform' + u[0] + 'v', function (loc, v, srcOffset, srcLength) {
                special(this, 266, [locId(this, loc), u[1], 0], values(v, u[2], srcOffset, srcLength));
            });
        });
    [['2', 13], ['3', 14], ['4', 15], ['2x3', 16], ['3x2', 17], ['2x4', 18], ['4x2', 19], ['3x4', 20], ['4x3', 21]]
        .forEach(function (u) {
            define(u[1] > 15 ? P2 : B, 'uniformMatrix' + u[0] + 'fv', function (loc, transpose, v, srcOffset, srcLength) {
                special(this, 266, [locId(this, loc), u[1], transpose ? 1 : 0], values(v, Float32Array, srcOffset, srcLength));
            });
        });
    [['1', 1], ['2', 2], ['3', 3], ['4', 4]].forEach(function (u) {
        define(B, 'vertexAttrib' + u[0] + 'fv', function (index, v) {
            special(this, 267, [index >>> 0, u[1]], values(v, Float32Array).subarray(0, 4 * u[1]));
        });
    });
    define(P2, 'vertexAttribI4iv', function (index, v) { special(this, 267, [index >>> 0, 5], values(v, Int32Array).subarray(0, 16)); });
    define(P2, 'vertexAttribI4uiv', function (index, v) { special(this, 267, [index >>> 0, 6], values(v, Uint32Array).subarray(0, 16)); });
    define(B, 'getUniform', function (p, loc) {
        if (st(this).lost) return null;
        var pid = id(this, 'p', p), lid = locId(this, loc);
        /* Its type, from the program's active uniforms. */
        var info = null, n = this.getProgramParameter(p, 0x8B86) | 0;
        var name = loc && loc[S].name;
        for (var i = 0; i < n && !info; i++) {
            var a = this.getActiveUniform(p, i);
            if (a && (name === undefined || a.name === name || a.name === name + '[0]')) info = a;
        }
        var t = info ? info.type : 0x1406;
        var spec = UNIFORM_TYPES[t] || [0, 1];
        var r = query(this, 321, [pid, lid, spec[0] === 3 ? 1 : spec[0], spec[1]]);
        if (!r) return null;
        var out = [];
        for (i = 0; i < spec[1]; i++) out.push(spec[0] === 0 ? r.f32() : spec[0] === 2 ? r.u32() : r.i32());
        if (spec[0] === 3) out = out.map(function (v) { return !!v; });
        if (spec[1] === 1) return out[0];
        return spec[0] === 0 ? new Float32Array(out) : spec[0] === 1 ? new Int32Array(out) :
               spec[0] === 2 ? new Uint32Array(out) : out;
    });
    /* type: [as (0 float, 1 int, 2 uint, 3 bool), count] */
    var UNIFORM_TYPES = { 0x1406: [0, 1], 0x8B50: [0, 2], 0x8B51: [0, 3], 0x8B52: [0, 4], 0x1404: [1, 1],
        0x8B53: [1, 2], 0x8B54: [1, 3], 0x8B55: [1, 4], 0x8B56: [3, 1], 0x8B57: [3, 2], 0x8B58: [3, 3], 0x8B59: [3, 4],
        0x8B5A: [0, 4], 0x8B5B: [0, 9], 0x8B5C: [0, 16], 0x8B5E: [1, 1], 0x8B60: [1, 1], 0x1405: [2, 1],
        0x8DC6: [2, 2], 0x8DC7: [2, 3], 0x8DC8: [2, 4], 0x8B65: [0, 6], 0x8B66: [0, 8], 0x8B67: [0, 6],
        0x8B68: [0, 12], 0x8B69: [0, 8], 0x8B6A: [0, 12] };
    (function () {
        var f = B.getUniformLocation;
        define(B, 'getUniformLocation', function (p, name) {
            var loc = f.call(this, p, name);
            if (loc) loc[S].name = String(name).replace(/\[0\]$/, '');
            return loc;
        });
    })();
    define(B, 'getVertexAttrib', function (index, pname) {
        if (st(this).lost) return null;
        pname >>>= 0;
        if (pname === 0x889F) return null;          /* its buffer: not tracked per attribute */
        if (pname === 0x8626) {
            var r4 = query(this, 322, [index >>> 0, pname, 0, 4]);
            return r4 ? new Float32Array([r4.f32(), r4.f32(), r4.f32(), r4.f32()]) : null;
        }
        var r = query(this, 322, [index >>> 0, pname, 1, 1]);
        if (!r) return null;
        var v = r.i32();
        return pname === 0x8622 || pname === 0x886A || pname === 0x88FD ? !!v : pname === 0x8625 ? v >>> 0 : v;
    });
    define(B, 'getVertexAttribOffset', function (index, pname) {
        var r = st(this).lost ? null : query(this, 323, [index >>> 0, pname >>> 0]);
        return r ? r.dv.getUint32(0, true) + r.dv.getUint32(4, true) * 4294967296 : 0;
    });

    /* ---- reading back ---- */
    define(B, 'readPixels', function (x, y, w, h, format, type, dst, dstOffset) {
        if (st(this).lost) return;
        if (typeof dst === 'number') {              /* into the bound PIXEL_PACK_BUFFER (WebGL 2) */
            special(this, 313, [x | 0, y | 0, w | 0, h | 0, format >>> 0, type >>> 0, 0]);
            return;
        }
        var d = sub(dst, dstOffset);
        if (!d) throw new TypeError("readPixels: parameter 7 is not of type 'ArrayBufferView'.");
        var r = query(this, 313, [x | 0, y | 0, w | 0, h | 0, format >>> 0, type >>> 0, d.length]);
        if (r) d.set(r.bytes().subarray(0, d.length));
    });
    define(B, 'checkFramebufferStatus', function (target) {
        var r = st(this).lost ? null : query(this, 314, [target >>> 0]);
        return r ? r.u32() : 0;
    });
    define(B, 'getError', function () {
        var s = st(this);
        if (s.lostError) { s.lostError = false; return 0x9242; }
        if (s.lost) return 0;
        if (s.synthError) { var e = s.synthError; s.synthError = 0; return e; }
        var r = query(this, 300, []);
        return r ? r.u32() : 0;
    });
    define(B, 'isContextLost', function () { return st(this).lost; });
    define(B, 'getContextAttributes', function () {
        var s = st(this);
        return s.lost ? null : Object.assign({}, s.attrs);
    });
    Object.defineProperty(B, 'canvas', { get: function () { return st(this).canvas; }, configurable: true });
    Object.defineProperty(B, 'drawingBufferWidth', { get: function () { return st(this).width; }, configurable: true });
    Object.defineProperty(B, 'drawingBufferHeight', { get: function () { return st(this).height; }, configurable: true });
    define(B, 'drawingBufferColorSpace', 'srgb');
    define(B, 'makeXRCompatible', function () { return Promise.reject(new Error('WebXR is not supported')); });

    /* ---- parameters ---- */
    var BOOL = [0x0BE2, 0x0B44, 0x0B71, 0x0BD0, 0x8037, 0x809E, 0x80A0, 0x0C11, 0x0B90, 0x0B72, 0x80AB, 0x8C89, 0x8E24, 0x8E23];
    var FLOAT = [0x0B21, 0x8038, 0x2A00, 0x80AA, 0x0B73, 0x84FD];
    var F2 = [0x846E, 0x846D, 0x0B70], F4 = [0x8005, 0x0C22], I2 = [0x0D3A], I4 = [0x0C10, 0x0BA2], B4 = [0x0C23];
    define(B, 'getParameter', function (pname) {
        var s = st(this);
        if (s.lost) return null;
        pname >>>= 0;
        switch (pname) {
        case 0x1F00: return 'WebKit';
        case 0x1F01: return 'WebKit WebGL';
        case 0x1F02: return s.version === 2 ? 'WebGL 2.0' : 'WebGL 1.0';
        case 0x8B8C: return s.version === 2 ? 'WebGL GLSL ES 3.00' : 'WebGL GLSL ES 1.0';
        case 0x9240: return s.unpack.flipY;
        case 0x9241: return s.unpack.premultiply;
        case 0x9243: return s.unpack.colorspace;
        case 0x8894: return s.buffers[0x8892] || null;
        case 0x8895: return (s.vao ? s.vao[S].elements : s.defaultElements) || null;
        case 0x8B8D: return s.program;
        case 0x8CA6: return s.framebuffer;
        case 0x8CAA: return s.version === 2 ? s.readFramebuffer : undefined;
        case 0x8CA7: return s.renderbuffer;
        case 0x8069: return (s.textures[s.unit] || {})[0x0DE1] || null;
        case 0x8514: return (s.textures[s.unit] || {})[0x8513] || null;
        case 0x806A: return (s.textures[s.unit] || {})[0x806F] || null;
        case 0x8C1D: return (s.textures[s.unit] || {})[0x8C1A] || null;
        case 0x85B5: return s.vao;
        case 0x8F36: return s.buffers[0x8F36] || null;
        case 0x8F37: return s.buffers[0x8F37] || null;
        case 0x88ED: return s.buffers[0x88EB] || null;
        case 0x88EF: return s.buffers[0x88EC] || null;
        case 0x8A28: return s.buffers[0x8A11] || null;
        case 0x8C8F: return s.buffers[0x8C8E] || null;
        case 0x8E25: return s.tf;
        case 0x8919: return s.samplers[s.unit] || null;
        case 0x86A3: return new Uint32Array(0);
        }
        var kind = BOOL.indexOf(pname) >= 0 ? 'b' : FLOAT.indexOf(pname) >= 0 ? 'f' : F2.indexOf(pname) >= 0 ? 'f2' :
                   F4.indexOf(pname) >= 0 ? 'f4' : I2.indexOf(pname) >= 0 ? 'i2' : I4.indexOf(pname) >= 0 ? 'i4' :
                   B4.indexOf(pname) >= 0 ? 'b4' : 'i';
        var n = kind.length > 1 ? +kind[1] : 1;
        var r = query(this, kind[0] === 'f' ? 302 : kind[0] === 'b' ? 303 : 301, [pname, n]);
        if (!r) return null;
        var out = [];
        for (var i = 0; i < n; i++) out.push(kind[0] === 'f' ? r.f32() : r.i32());
        if (kind === 'b') return !!out[0];
        if (kind === 'b4') return out.map(function (v) { return !!v; });
        if (kind === 'f') return out[0];
        if (kind === 'f2' || kind === 'f4') return new Float32Array(out);
        if (kind === 'i2' || kind === 'i4') return new Int32Array(out);
        /* enums come back unsigned */
        return out[0] < 0 && pname !== 0x8D6B ? out[0] >>> 0 : out[0];
    });
    function objParam(op, nArgs, flt) {
        return function () {
            if (st(this).lost) return null;
            var words = Array.prototype.slice.call(arguments, 0, nArgs).map(function (v) { return v >>> 0; });
            if (flt) words.push(flt(arguments[1] >>> 0) ? 1 : 0);
            var r = query(this, op, words);
            if (!r) return null;
            return flt && flt(arguments[1] >>> 0) ? r.f32() : r.i32();
        };
    }
    define(B, 'getBufferParameter', objParam(317, 2, function () { return false; }));
    define(B, 'getRenderbufferParameter', objParam(318, 2, function () { return false; }));
    define(B, 'getTexParameter', objParam(316, 2, function (p) { return p === 0x813A || p === 0x813B || p === 0x84FE; }));
    define(B, 'getFramebufferAttachmentParameter', function (target, attachment, pname) {
        if (st(this).lost) return null;
        var r = query(this, 319, [target >>> 0, attachment >>> 0, pname >>> 0]);
        return r ? r.i32() : null;
    });
    define(P2, 'getQueryParameter', function (q, pname) {
        var r = st(this).lost ? null : query(this, 324, [id(this, 'q', q), pname >>> 0]);
        if (!r) return null;
        var v = r.u32();
        return (pname >>> 0) === 0x8867 ? !!v : v;
    });
    define(P2, 'getQuery', function () { return null; });   /* (the current query: not tracked) */
    define(P2, 'getSamplerParameter', function (m, pname) {
        var flt = (pname >>> 0) === 0x813A || (pname >>> 0) === 0x813B;
        var r = st(this).lost ? null : query(this, 325, [id(this, 'm', m), pname >>> 0, flt ? 1 : 0]);
        return r ? (flt ? r.f32() : r.i32()) : null;
    });
    define(P2, 'getIndexedParameter', function (target, index) {
        var r = st(this).lost ? null : query(this, 329, [target >>> 0, index >>> 0, 0]);
        return r ? r.i32() : null;
    });
    define(P2, 'getActiveUniformBlockParameter', function (p, index, pname) {
        pname >>>= 0;
        if (pname === 0x8A43) {
            var cnt = this.getActiveUniformBlockParameter(p, index, 0x8A42) | 0;
            var rr = query(this, 332, [id(this, 'p', p), index >>> 0, pname, cnt]);
            var a = [];
            for (var i = 0; rr && i < cnt; i++) a.push(rr.u32());
            return new Uint32Array(a);
        }
        var r = st(this).lost ? null : query(this, 332, [id(this, 'p', p), index >>> 0, pname, 1]);
        if (!r) return null;
        var v = r.i32();
        return pname === 0x8A44 || pname === 0x8A46 ? !!v : v;
    });
    define(P2, 'getActiveUniformBlockName', function (p, index) {
        var r = st(this).lost ? null : query(this, 333, [id(this, 'p', p), index >>> 0]);
        return r ? r.str() : null;
    });
    define(P2, 'getUniformIndices', function (p, names) {
        var b = utf8(Array.prototype.join.call(names, '\0'));
        var r = st(this).lost ? null : query(this, 334, [id(this, 'p', p)], b);
        if (!r) return null;
        var n = r.u32(), out = [];
        for (var i = 0; i < n; i++) out.push(r.u32());
        return out;
    });
    define(P2, 'getActiveUniforms', function (p, indices, pname) {
        var idx = new Uint32Array(Array.prototype.slice.call(indices));
        var r = st(this).lost ? null : query(this, 335, [id(this, 'p', p), pname >>> 0], new Uint8Array(idx.buffer));
        if (!r) return null;
        var out = [];
        for (var i = 0; i < idx.length; i++) out.push(r.i32());
        pname >>>= 0;
        if (pname === 0x8A3E) return out.map(function (v) { return !!v; });
        if (pname === 0x8A37) return out.map(function (v) { return v >>> 0; });
        return out;
    });
    define(P2, 'getInternalformatParameter', function (target, internalformat, pname) {
        if (st(this).lost) return null;
        var r = query(this, 337, [target >>> 0, internalformat >>> 0, 0x80A9, 16]);
        if (!r) return null;
        var out = [];
        for (var i = 0; i < 16; i++) { var v = r.i32(); if (v) out.push(v); }
        return new Int32Array(out);
    });

    /* ---- WebGL 2 odds and ends ---- */
    define(P2, 'drawBuffers', function (bufs) {
        dirty(this);
        special(this, 270, [], new Uint8Array(new Uint32Array(Array.prototype.slice.call(bufs)).buffer));
    });
    function clearBuffer(kind, Kind) {
        return function (buffer, drawbuffer, values, srcOffset) {
            dirty(this);
            var v = new Kind(4);
            var src = Array.prototype.slice.call(values, srcOffset >>> 0 || 0, (srcOffset >>> 0 || 0) + 4);
            v.set(src.slice(0, 4));
            special(this, 271, [buffer >>> 0, drawbuffer | 0, kind], new Uint8Array(v.buffer));
        };
    }
    define(P2, 'clearBufferfv', clearBuffer(0, Float32Array));
    define(P2, 'clearBufferiv', clearBuffer(1, Int32Array));
    define(P2, 'clearBufferuiv', clearBuffer(2, Uint32Array));
    function invalidate(ctx, target, attachments) {
        special(ctx, 272, [target >>> 0], new Uint8Array(new Uint32Array(Array.prototype.slice.call(attachments)).buffer));
    }
    define(P2, 'invalidateFramebuffer', function (target, attachments) { invalidate(this, target, attachments); });
    define(P2, 'invalidateSubFramebuffer', function (target, attachments) { invalidate(this, target, attachments); });
    define(P2, 'transformFeedbackVaryings', function (p, varyings, mode) {
        special(this, 273, [id(this, 'p', p), mode >>> 0], utf8(Array.prototype.join.call(varyings, '\0')));
    });
    define(P2, 'fenceSync', function (condition, flags) {
        if (st(this).lost) return null;
        var o = obj(this, 'y');
        special(this, 275, [o[S].id, condition >>> 0, flags >>> 0]);
        return o;
    });
    deleter(P2, 'deleteSync', 'y');
    define(P2, 'clientWaitSync', function (sync, flags, timeout) {
        var r = st(this).lost ? null : query(this, 327, [id(this, 'y', sync), flags >>> 0, { v: 0 }]);
        return r ? r.u32() : 0x911D;
    });
    define(P2, 'waitSync', function (sync, flags, timeout) {
        special(this, 276, [id(this, 'y', sync), flags >>> 0, { v: -1 }]);
    });
    define(P2, 'getSyncParameter', function (sync, pname) {
        var r = st(this).lost ? null : query(this, 326, [id(this, 'y', sync), pname >>> 0]);
        return r ? r.u32() : null;
    });

    /* ---- extensions: what ANGLE has and may turn on, as WebGL names them ---- */
    var EXT = {
        ANGLE_instanced_arrays: ['GL_ANGLE_instanced_arrays'], EXT_blend_minmax: ['GL_EXT_blend_minmax'],
        EXT_color_buffer_float: ['GL_EXT_color_buffer_float'], EXT_color_buffer_half_float: ['GL_EXT_color_buffer_half_float'],
        EXT_float_blend: ['GL_EXT_float_blend'], EXT_frag_depth: ['GL_EXT_frag_depth'],
        EXT_shader_texture_lod: ['GL_EXT_shader_texture_lod'], EXT_sRGB: ['GL_EXT_sRGB'],
        EXT_texture_compression_bptc: ['GL_EXT_texture_compression_bptc'],
        EXT_texture_compression_rgtc: ['GL_EXT_texture_compression_rgtc'],
        EXT_texture_filter_anisotropic: ['GL_EXT_texture_filter_anisotropic'],
        EXT_texture_norm16: ['GL_EXT_texture_norm16'], KHR_parallel_shader_compile: ['GL_KHR_parallel_shader_compile'],
        OES_element_index_uint: ['GL_OES_element_index_uint'], OES_fbo_render_mipmap: ['GL_OES_fbo_render_mipmap'],
        OES_standard_derivatives: ['GL_OES_standard_derivatives'], OES_texture_float: ['GL_OES_texture_float'],
        OES_texture_float_linear: ['GL_OES_texture_float_linear'], OES_texture_half_float: ['GL_OES_texture_half_float'],
        OES_texture_half_float_linear: ['GL_OES_texture_half_float_linear'],
        OES_vertex_array_object: ['GL_OES_vertex_array_object'], WEBGL_color_buffer_float: ['GL_EXT_color_buffer_half_float'],
        WEBGL_compressed_texture_s3tc: ['GL_EXT_texture_compression_dxt1', 'GL_ANGLE_texture_compression_dxt3', 'GL_ANGLE_texture_compression_dxt5'],
        WEBGL_compressed_texture_s3tc_srgb: ['GL_EXT_texture_compression_s3tc_srgb'],
        WEBGL_debug_shaders: [], WEBGL_depth_texture: ['GL_ANGLE_depth_texture'],
        WEBGL_draw_buffers: ['GL_EXT_draw_buffers'], WEBGL_lose_context: [], WEBGL_multi_draw: ['GL_ANGLE_multi_draw']
    };
    /* (No EXT_disjoint_timer_query: GPU timers stay off.) */
    function extInfo(ctx) {
        var s = st(ctx);
        if (s.ext) return s.ext;
        var r = query(ctx, 339, []);
        var have = r ? r.str().split(' ') : [], req = r ? r.str().split(' ') : [];
        s.ext = { have: have, req: req, on: {} };
        return s.ext;
    }
    function offered(ctx, name) {
        var e = EXT[name];
        if (!e) return false;
        if (name === 'WEBGL_lose_context') return true;
        if (name === 'WEBGL_debug_shaders') return false;
        var info = extInfo(ctx);
        return e.length > 0 && e.every(function (g) { return info.have.indexOf(g) >= 0 || info.req.indexOf(g) >= 0; });
    }
    define(B, 'getSupportedExtensions', function () {
        if (st(this).lost) return null;
        var self = this;
        return Object.keys(EXT).filter(function (n) { return offered(self, n); });
    });
    define(B, 'getExtension', function (name) {
        var s = st(this);
        name = String(name);
        if (s.lost) return null;
        var info = extInfo(this);
        if (info.on[name]) return info.on[name];
        if (!offered(this, name)) return null;
        var self = this;
        EXT[name].forEach(function (g) { if (info.have.indexOf(g) < 0) query(self, 338, [], utf8(g)); });
        info.on[name] = extension(this, name);
        return info.on[name];
    });
    function extension(ctx, name) {
        var e = { };
        Object.defineProperty(e, Symbol.toStringTag, { value: name, configurable: true });
        switch (name) {
        case 'WEBGL_lose_context':
            e.loseContext = function () { var s = st(ctx); s.lost = true; s.lostError = true; checkLost(false); fire(ctx, 'webglcontextlost'); };
            e.restoreContext = function () {};
            break;
        case 'OES_vertex_array_object':
            e.VERTEX_ARRAY_BINDING_OES = 0x85B5;
            e.createVertexArrayOES = function () { return P2.createVertexArray.call(ctx); };
            e.deleteVertexArrayOES = function (v) { return P2.deleteVertexArray.call(ctx, v); };
            e.isVertexArrayOES = function (v) { return P2.isVertexArray.call(ctx, v); };
            e.bindVertexArrayOES = function (v) { return P2.bindVertexArray.call(ctx, v); };
            break;
        case 'ANGLE_instanced_arrays':
            e.VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE = 0x88FE;
            e.drawArraysInstancedANGLE = function (m, f, c, n) { return P2.drawArraysInstanced.call(ctx, m, f, c, n); };
            e.drawElementsInstancedANGLE = function (m, c, t, o, n) { return P2.drawElementsInstanced.call(ctx, m, c, t, o, n); };
            e.vertexAttribDivisorANGLE = function (i, d) { return P2.vertexAttribDivisor.call(ctx, i, d); };
            break;
        case 'WEBGL_draw_buffers':
            for (var i = 0; i < 16; i++) { e['COLOR_ATTACHMENT' + i + '_WEBGL'] = 0x8CE0 + i; e['DRAW_BUFFER' + i + '_WEBGL'] = 0x8825 + i; }
            e.MAX_COLOR_ATTACHMENTS_WEBGL = 0x8CDF; e.MAX_DRAW_BUFFERS_WEBGL = 0x8824;
            e.drawBuffersWEBGL = function (b) { return P2.drawBuffers.call(ctx, b); };
            break;
        case 'EXT_texture_filter_anisotropic':
            e.TEXTURE_MAX_ANISOTROPY_EXT = 0x84FE; e.MAX_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FF;
            break;
        case 'OES_standard_derivatives': e.FRAGMENT_SHADER_DERIVATIVE_HINT_OES = 0x8B8B; break;
        case 'OES_texture_half_float': e.HALF_FLOAT_OES = 0x8D61; break;
        case 'WEBGL_depth_texture': e.UNSIGNED_INT_24_8_WEBGL = 0x84FA; break;
        case 'EXT_blend_minmax': e.MIN_EXT = 0x8007; e.MAX_EXT = 0x8008; break;
        case 'EXT_sRGB': e.SRGB_EXT = 0x8C40; e.SRGB_ALPHA_EXT = 0x8C42; e.SRGB8_ALPHA8_EXT = 0x8C43;
            e.FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING_EXT = 0x8210; break;
        case 'WEBGL_compressed_texture_s3tc':
            e.COMPRESSED_RGB_S3TC_DXT1_EXT = 0x83F0; e.COMPRESSED_RGBA_S3TC_DXT1_EXT = 0x83F1;
            e.COMPRESSED_RGBA_S3TC_DXT3_EXT = 0x83F2; e.COMPRESSED_RGBA_S3TC_DXT5_EXT = 0x83F3; break;
        case 'KHR_parallel_shader_compile': e.COMPLETION_STATUS_KHR = 0x91B1; break;
        }
        return e;
    }
    function fire(ctx, type) {
        var c = st(ctx).canvas, ev;
        try { ev = new (global.WebGLContextEvent || global.Event)(type, { cancelable: true, statusMessage: '' }); } catch (e) { return; }
        global.setTimeout(function () { if (c.dispatchEvent) c.dispatchEvent(ev); }, 0);
    }

    expose('WebGLRenderingContext', WebGLRenderingContext);
    expose('WebGL2RenderingContext', WebGL2RenderingContext);
    if (!global.WebGLContextEvent && global.Event) {
        var WCE = function WebGLContextEvent(type, init) {
            var e = new global.Event(type, init);
            Object.defineProperty(e, 'statusMessage', { value: init && init.statusMessage || '', enumerable: true });
            return e;
        };
        expose('WebGLContextEvent', WCE);
    }

    /* ---- the canvas ---- */
    var HCE = global.HTMLCanvasElement;
    if (!HCE) return;
    var HP = HCE.prototype;
    var GLCTX = Symbol('glcontext'), OTHER = native.otherContext || Symbol('2d');
    var getContext2 = HP.getContext;
    define(HP, 'getContext', function getContext(type, attrs) {
        type = String(type);
        var c = this[GLCTX];
        if (type === 'webgl' || type === 'experimental-webgl' || type === 'webgl2') {
            var version = type === 'webgl2' ? 2 : 1;
            if (c) return c.kind === 'gl' + version ? c.ctx : null;
            if (this[OTHER]) return null;
            var ctx = create(this, version, attrs);
            if (ctx) define(this, GLCTX, { kind: 'gl' + version, ctx: ctx });
            return ctx;
        }
        if (type === 'webgpu') {
            if (c) return c.kind === 'gpu' ? c.ctx : null;
            if (this[OTHER] || !native.gpuCanvas) return null;
            var g = native.gpuCanvas(this);
            if (g) define(this, GLCTX, { kind: 'gpu', ctx: g });
            return g;
        }
        if (c) return null;
        var r = getContext2.call(this, type, attrs);
        if (r) define(this, OTHER, true);
        return r;
    });
    /* A new size: the drawing buffer follows (and is cleared). */
    ['width', 'height'].forEach(function (d) {
        var desc = Object.getOwnPropertyDescriptor(HP, d);
        if (!desc || !desc.set) return;
        Object.defineProperty(HP, d, { configurable: true, enumerable: true, get: desc.get,
            set: function (v) {
                desc.set.call(this, v);
                var c = this[GLCTX];
                if (!c) return;
                if (c.kind === 'gpu') { if (c.ctx.__resize) c.ctx.__resize(this.width, this.height); return; }
                var s = c.ctx[S];
                flush(c.ctx, false);
                s.width = this.width;
                s.height = this.height;
                native.resize(s.cid, s.width, s.height);
                dirty(c.ctx);
                soon(c.ctx);
            } });
    });
})

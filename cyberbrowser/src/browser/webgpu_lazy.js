/*
 * webgpu_lazy.js - WebGL and WebGPU, there when first used: installed in
 * each realm by webgpu_api_install (browser/webgpu_api.c) as
 * fn(global, window, native, install).
 *
 * webgl_api.js and webgpu_api.js are large, and most pages never use them,
 * so a realm starts with stand-ins: its global names (WebGLRenderingContext,
 * GPUBufferUsage, ...) as getters, canvas.getContext for the new context
 * types, and navigator.gpu.  The first touch of any of them puts the stand-
 * ins away and calls install(global, window, native), which evaluates the
 * two scripts for this realm; then the touch goes on as if they had always
 * been there.
 */
(function (global, win, native, install) {
    'use strict';
    var NAMES = ('WebGLObject WebGLBuffer WebGLFramebuffer WebGLRenderbuffer WebGLTexture WebGLShader WebGLProgram ' +
        'WebGLUniformLocation WebGLVertexArrayObject WebGLQuery WebGLSampler WebGLSync WebGLTransformFeedback ' +
        'WebGLActiveInfo WebGLShaderPrecisionFormat WebGLRenderingContext WebGL2RenderingContext WebGLContextEvent ' +
        'GPUBufferUsage GPUTextureUsage GPUShaderStage GPUMapMode GPUColorWrite GPUError GPUValidationError ' +
        'GPUOutOfMemoryError GPUInternalError GPUPipelineError GPU GPUAdapter GPUAdapterInfo GPUSupportedLimits ' +
        'GPUSupportedFeatures WGSLLanguageFeatures GPUDevice GPUQueue GPUBuffer GPUTexture GPUTextureView GPUSampler ' +
        'GPUShaderModule GPUCompilationInfo GPUBindGroupLayout GPUPipelineLayout GPUBindGroup GPURenderPipeline ' +
        'GPUComputePipeline GPUCommandEncoder GPURenderPassEncoder GPUComputePassEncoder GPUCommandBuffer ' +
        'GPUCanvasContext GPUDeviceLostInfo').split(' ');
    var targets = win && win !== global ? [global, win] : [global];
    var HCE = global.HTMLCanvasElement, HP = HCE && HCE.prototype, original = HP && HP.getContext;
    var NP = global.Navigator && global.Navigator.prototype;
    var done = false;
    /* A canvas that has a 2D context already gets no other (webgl_api.js
     * marks canvases with this too). */
    native.otherContext = Symbol('2d');

    function go() {
        if (done) return;
        done = true;
        NAMES.forEach(function (n) {
            targets.forEach(function (o) {
                var d = Object.getOwnPropertyDescriptor(o, n);
                if (d && d.get === lazy[n]) delete o[n];
            });
        });
        if (HP) Object.defineProperty(HP, 'getContext', { value: original, writable: true, configurable: true });
        if (NP) delete NP.gpu;
        else if (global.navigator) delete global.navigator.gpu;
        install(global, win, native);
    }

    var lazy = {};
    NAMES.forEach(function (n) {
        lazy[n] = function () { go(); return global[n]; };
        targets.forEach(function (o) {
            if (Object.prototype.hasOwnProperty.call(o, n)) return;
            Object.defineProperty(o, n, { configurable: true, enumerable: false, get: lazy[n],
                set: function (v) { Object.defineProperty(o, n, { value: v, writable: true, configurable: true }); } });
        });
    });
    if (HP && original) {
        var stand = function getContext(type) {
            var t = String(type);
            if (t === 'webgl' || t === 'webgl2' || t === 'experimental-webgl' || t === 'webgpu') go();
            var f = HP.getContext === stand ? original : HP.getContext;
            var r = f.apply(this, arguments);
            if (r && !done) Object.defineProperty(this, native.otherContext, { value: true, configurable: true });
            return r;
        };
        Object.defineProperty(HP, 'getContext', { value: stand, writable: true, configurable: true });
    }
    /* navigator.gpu: on Navigator.prototype, or (without one) navigator. */
    var nav = NP || global.navigator;
    if (nav) Object.defineProperty(nav, 'gpu', { configurable: true, enumerable: true,
        get: function () {
            go();
            var d = Object.getOwnPropertyDescriptor(nav, 'gpu');
            return d ? (d.get ? d.get.call(this) : d.value) : undefined;
        } });
})

/*
 * webgpu_api.js - WebGPU (the W3C WebGPU specification) in a tab, installed
 * in each realm by webgpu_api_install (browser/webgpu_api.c) as
 * fn(global, window, native).
 *
 * Calls are recorded as [u32 op][u32 n][args] (proc/webgpu_wire.h, the same
 * numbers) and sent to the site's web-GPU process, where wgpu-native runs
 * them and does WebGPU's validation; descriptors go as value trees, shaped
 * here as the page passed them.  Records are sent when something must be
 * answered (an adapter, a mapping, an error scope), and at the end of the
 * task, with the canvases drawn to presented.  The process answers
 * synchronously, so the promises here resolve as soon as their reply comes.
 *
 * Object ids come from native.newId (one space for the tab, shared with
 * WebGL's contexts) and go back when the page is done with an object: at
 * once for what WebGPU makes unusable (a finished encoder, an ended pass, a
 * submitted command buffer, a presented canvas texture), else when it is
 * collected.
 */
(function (global, win, native) {
    'use strict';
    function define(obj, name, value) {
        Object.defineProperty(obj, name, { value: value, writable: true, configurable: true });
    }
    function expose(name, value) {
        define(global, name, value);
        if (win && win !== global) define(win, name, value);
    }
    function illegal() { throw new TypeError('Illegal constructor'); }
    function domError(msg, name) {
        var D = global.DOMException;
        return D ? new D(msg, name) : new Error(name + ': ' + msg);
    }

    var G = { REQUEST_ADAPTER: 1, REQUEST_DEVICE: 2, CREATE_BUFFER: 3, BUFFER_MAP: 4, BUFFER_UNMAP: 5, DESTROY: 6,
              CREATE_TEXTURE: 7, CREATE_VIEW: 8, CREATE_SAMPLER: 9, CREATE_SHADER_MODULE: 10, COMPILATION_INFO: 11,
              CREATE_BIND_GROUP_LAYOUT: 12, CREATE_PIPELINE_LAYOUT: 13, CREATE_BIND_GROUP: 14,
              CREATE_RENDER_PIPELINE: 15, CREATE_COMPUTE_PIPELINE: 16, GET_BIND_GROUP_LAYOUT: 17,
              CREATE_COMMAND_ENCODER: 18, BEGIN_RENDER_PASS: 19, BEGIN_COMPUTE_PASS: 20, PASS: 21, END_PASS: 22,
              ENCODE: 23, FINISH: 24, SUBMIT: 25, WRITE_BUFFER: 26, WRITE_TEXTURE: 27, WORK_DONE: 28, CONFIGURE: 29,
              CURRENT_TEXTURE: 30, PUSH_ERROR_SCOPE: 31, POP_ERROR_SCOPE: 32, RELEASE: 33 };
    var V_NULL = 0, V_FALSE = 1, V_TRUE = 2, V_NUMBER = 3, V_STRING = 4, V_ARRAY = 5, V_OBJECT = 6, V_ID = 7, V_BYTES = 8;

    /* ---- recording ---- */
    var buf = new Uint8Array(65536), dv = new DataView(buf.buffer), len = 0;
    function ensure(n) {
        if (len + n <= buf.length) return;
        var cap = buf.length * 2;
        while (cap < len + n) cap *= 2;
        var b = new Uint8Array(cap);
        b.set(buf.subarray(0, len));
        buf = b;
        dv = new DataView(b.buffer);
    }
    function u8(v) { ensure(1); buf[len++] = v; }
    function u32(v) { ensure(4); dv.setUint32(len, v >>> 0, true); len += 4; }
    function f64(v) { ensure(8); dv.setFloat64(len, +v, true); len += 8; }
    function raw(b) { ensure(b.length); buf.set(b, len); len += b.length; }
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
    function bytesOf(v) {
        if (v instanceof ArrayBuffer) return new Uint8Array(v);
        if (ArrayBuffer.isView(v)) return new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
        return null;
    }
    var S = Symbol('webgpu');       /* an object's state */
    /* A value tree: what the page passed, as the wire says (GPU objects as
     * their ids; undefined members left out). */
    function tree(v, depth) {
        if (depth > 11) throw new TypeError('descriptor nested too deeply');
        if (v === null || v === undefined) { u8(V_NULL); return; }
        if (v === false || v === true) { u8(v ? V_TRUE : V_FALSE); return; }
        if (typeof v === 'number' || typeof v === 'bigint') { u8(V_NUMBER); f64(Number(v)); return; }
        if (typeof v === 'string') { var s = utf8(v); u8(V_STRING); u32(s.length); raw(s); return; }
        if (typeof v !== 'object') { u8(V_NULL); return; }
        if (v[S]) { u8(V_ID); u32(idOf(v)); return; }
        var b = bytesOf(v);
        if (b) { u8(V_BYTES); u32(b.length); raw(b); return; }
        if (Array.isArray(v) || typeof v[Symbol.iterator] === 'function') {
            var a = Array.from(v);
            u8(V_ARRAY); u32(a.length);
            for (var i = 0; i < a.length; i++) tree(a[i], depth + 1);
            return;
        }
        var keys = [];
        for (var k in v) if (v[k] !== undefined && typeof v[k] !== 'function') keys.push(k);
        u8(V_OBJECT); u32(keys.length);
        keys.forEach(function (key) {
            var kb = utf8(key);
            u32(kb.length); raw(kb);
            tree(v[key], depth + 1);
        });
    }
    /* A record: op, u32 words, then a tree (when `t` is given), or bytes. */
    function rec(op, words, t, hasTree, more) {
        if (gone) return;
        var at = len;
        u32(op); u32(0);
        for (var i = 0; i < words.length; i++) u32(words[i]);
        if (hasTree) tree(t, 0);
        if (more) more();
        dv.setUint32(at + 4, len - at - 8, true);
        soon();
    }
    var gone = false;
    function flush(reply) {
        if (gone) return null;
        if (!len && !reply) return null;
        var r = native.gpuRun(buf.subarray(0, len), !!reply);
        len = 0;
        if (r === null) lostAll();
        return r;
    }
    function query(op, words, t, hasTree, more) {
        rec(op, words, t, hasTree, more);
        var r = flush(true);
        return r ? new Reader(r) : null;
    }
    function Reader(ab) { this.b = new Uint8Array(ab); this.dv = new DataView(ab); this.at = 0; }
    Reader.prototype.u32 = function () { var v = this.dv.getUint32(this.at, true); this.at += 4; return v; };
    Reader.prototype.left = function () { return this.b.length - this.at; };
    Reader.prototype.str = function (n) {
        var s = fromUtf8(this.b, this.at, n);
        this.at += n;
        return s;
    };
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
    Reader.prototype.value = function () {
        var tag = this.b[this.at++], n, i, out;
        switch (tag) {
        case V_FALSE: return false;
        case V_TRUE: return true;
        case V_NUMBER: out = this.dv.getFloat64(this.at, true); this.at += 8; return out;
        case V_STRING: return this.str(this.u32());
        case V_ARRAY: n = this.u32(); out = []; for (i = 0; i < n; i++) out.push(this.value()); return out;
        case V_OBJECT:
            n = this.u32(); out = {};
            for (i = 0; i < n; i++) { var k = this.str(this.u32()); out[k] = this.value(); }
            return out;
        case V_ID: return this.u32();
        case V_BYTES: n = this.u32(); out = this.b.slice(this.at, this.at + n); this.at += n; return out.buffer;
        default: return null;
        }
    };

    /* At the end of the task: what is recorded goes; canvases drawn to are
     * presented. */
    var queued = false, toPresent = [];
    function soon() {
        if (queued) return;
        queued = true;
        Promise.resolve().then(endOfTask);
    }
    function endOfTask() {
        queued = false;
        flush(false);
        var list = toPresent;
        toPresent = [];
        list.forEach(function (cx) { present(cx); });
    }

    /* ---- objects and their ids ---- */
    var collected = typeof FinalizationRegistry === 'function'
        ? new FinalizationRegistry(function (id) { release(id); }) : null;
    function release(id) {
        if (!id) return;
        if (!gone) rec(G.RELEASE, [id]);
        native.freeId(id);
    }
    function idOf(o) {
        var s = o[S];
        return s && !s.released ? s.id : 0;
    }
    /* A new object of class F with a fresh id (null if there are none left). */
    function make(F, device, label, extra) {
        var id = native.newId();
        var o = Object.create(F.prototype);
        var s = { id: id, device: device, released: false };
        if (extra) for (var k in extra) s[k] = extra[k];
        define(o, S, s);
        define(o, '_label', label === undefined ? '' : String(label));
        if (collected && id) collected.register(o, id, s);
        return o;
    }
    /* Done with it now: its id goes back. */
    function done(o) {
        var s = o[S];
        if (s.released) return;
        s.released = true;
        if (collected) collected.unregister(s);
        release(s.id);
    }
    function st(o, F) {
        if (!o || !(o instanceof F) || !o[S]) throw new TypeError('Illegal invocation');
        return o[S];
    }
    function klass(name, parent) {
        var F = function () { illegal(); };
        Object.defineProperty(F, 'name', { value: name });
        if (parent) Object.setPrototypeOf(F.prototype, parent.prototype);
        Object.defineProperty(F.prototype, Symbol.toStringTag, { value: name, configurable: true });
        Object.defineProperty(F.prototype, 'label', { configurable: true, enumerable: true,
            get: function () { return this._label || ''; }, set: function (v) { this._label = String(v); } });
        expose(name, F);
        return F;
    }
    function flags(name, map) {
        var o = {};
        for (var k in map) Object.defineProperty(o, k, { value: map[k], enumerable: true });
        Object.defineProperty(o, Symbol.toStringTag, { value: name });
        expose(name, Object.freeze(o));
    }
    flags('GPUBufferUsage', { MAP_READ: 1, MAP_WRITE: 2, COPY_SRC: 4, COPY_DST: 8, INDEX: 16, VERTEX: 32, UNIFORM: 64,
                              STORAGE: 128, INDIRECT: 256, QUERY_RESOLVE: 512 });
    flags('GPUTextureUsage', { COPY_SRC: 1, COPY_DST: 2, TEXTURE_BINDING: 4, STORAGE_BINDING: 8, RENDER_ATTACHMENT: 16 });
    flags('GPUShaderStage', { VERTEX: 1, FRAGMENT: 2, COMPUTE: 4 });
    flags('GPUMapMode', { READ: 1, WRITE: 2 });
    flags('GPUColorWrite', { RED: 1, GREEN: 2, BLUE: 4, ALPHA: 8, ALL: 15 });

    /* ---- errors ---- */
    function errorClass(name, parent) {
        var F = function (message) {
            if (!new.target) throw new TypeError("Constructor requires 'new'");
            define(this, 'message', message === undefined ? '' : String(message));
        };
        Object.defineProperty(F, 'name', { value: name });
        if (parent) Object.setPrototypeOf(F.prototype, parent.prototype);
        Object.defineProperty(F.prototype, Symbol.toStringTag, { value: name, configurable: true });
        expose(name, F);
        return F;
    }
    var GPUError = errorClass('GPUError');
    var GPUValidationError = errorClass('GPUValidationError', GPUError);
    var GPUOutOfMemoryError = errorClass('GPUOutOfMemoryError', GPUError);
    var GPUInternalError = errorClass('GPUInternalError', GPUError);
    var GPUPipelineError = errorClass('GPUPipelineError');

    /* ---- the classes ---- */
    var GPU = klass('GPU');
    var GPUAdapter = klass('GPUAdapter');
    var GPUAdapterInfo = klass('GPUAdapterInfo');
    var GPUSupportedLimits = klass('GPUSupportedLimits');
    var GPUSupportedFeatures = klass('GPUSupportedFeatures');
    var WGSLLanguageFeatures = klass('WGSLLanguageFeatures');
    var ET = global.EventTarget;
    var GPUDevice = klass('GPUDevice', ET);
    var GPUQueue = klass('GPUQueue');
    var GPUBuffer = klass('GPUBuffer');
    var GPUTexture = klass('GPUTexture');
    var GPUTextureView = klass('GPUTextureView');
    var GPUSampler = klass('GPUSampler');
    var GPUShaderModule = klass('GPUShaderModule');
    var GPUCompilationInfo = klass('GPUCompilationInfo');
    var GPUBindGroupLayout = klass('GPUBindGroupLayout');
    var GPUPipelineLayout = klass('GPUPipelineLayout');
    var GPUBindGroup = klass('GPUBindGroup');
    var GPURenderPipeline = klass('GPURenderPipeline');
    var GPUComputePipeline = klass('GPUComputePipeline');
    var GPUCommandEncoder = klass('GPUCommandEncoder');
    var GPURenderPassEncoder = klass('GPURenderPassEncoder');
    var GPUComputePassEncoder = klass('GPUComputePassEncoder');
    var GPUCommandBuffer = klass('GPUCommandBuffer');
    var GPUCanvasContext = klass('GPUCanvasContext');
    var GPUDeviceLostInfo = klass('GPUDeviceLostInfo');
    function readonly(F, props) {
        var o = Object.create(F.prototype);
        for (var k in props) Object.defineProperty(o, k, { value: props[k], enumerable: true });
        return o;
    }
    /* setlike, read-only */
    function setlike(F, names) {
        var o = Object.create(F.prototype);
        var set = new Set(names);
        define(o, '_set', set);
        return o;
    }
    [GPUSupportedFeatures, WGSLLanguageFeatures].forEach(function (F) {
        var P = F.prototype;
        Object.defineProperty(P, 'size', { get: function () { return this._set.size; }, configurable: true });
        define(P, 'has', function (k) { return this._set.has(k); });
        define(P, 'keys', function () { return this._set.values(); });
        define(P, 'values', function () { return this._set.values(); });
        define(P, 'entries', function () { return this._set.entries(); });
        define(P, 'forEach', function (cb, that) { var self = this; this._set.forEach(function (v) { cb.call(that, v, v, self); }); });
        define(P, Symbol.iterator, function () { return this._set.values(); });
    });

    /* The limits a device gets: WebGPU's defaults (requestDevice asks for
     * no more). */
    var DEFAULT_LIMITS = {
        maxTextureDimension1D: 8192, maxTextureDimension2D: 8192, maxTextureDimension3D: 2048,
        maxTextureArrayLayers: 256, maxBindGroups: 4, maxBindGroupsPlusVertexBuffers: 24, maxBindingsPerBindGroup: 1000,
        maxDynamicUniformBuffersPerPipelineLayout: 8, maxDynamicStorageBuffersPerPipelineLayout: 4,
        maxSampledTexturesPerShaderStage: 16, maxSamplersPerShaderStage: 16, maxStorageBuffersPerShaderStage: 8,
        maxStorageTexturesPerShaderStage: 4, maxUniformBuffersPerShaderStage: 12, maxUniformBufferBindingSize: 65536,
        maxStorageBufferBindingSize: 134217728, minUniformBufferOffsetAlignment: 256,
        minStorageBufferOffsetAlignment: 256, maxVertexBuffers: 8, maxBufferSize: 268435456, maxVertexAttributes: 16,
        maxVertexBufferArrayStride: 2048, maxInterStageShaderVariables: 16, maxColorAttachments: 8,
        maxColorAttachmentBytesPerSample: 32, maxComputeWorkgroupStorageSize: 16384,
        maxComputeInvocationsPerWorkgroup: 256, maxComputeWorkgroupSizeX: 256, maxComputeWorkgroupSizeY: 256,
        maxComputeWorkgroupSizeZ: 64, maxComputeWorkgroupsPerDimension: 65535
    };
    function limits(values) {
        var o = Object.create(GPUSupportedLimits.prototype);
        for (var k in DEFAULT_LIMITS) {
            var v = values && typeof values[k] === 'number' ? values[k] : DEFAULT_LIMITS[k];
            Object.defineProperty(o, k, { value: v, enumerable: true });
        }
        return o;
    }

    /* ---- navigator.gpu ---- */
    var gpu = Object.create(GPU.prototype);
    var wgslFeatures = setlike(WGSLLanguageFeatures, []);
    Object.defineProperty(GPU.prototype, 'wgslLanguageFeatures', { get: function () { return wgslFeatures; }, configurable: true });
    define(GPU.prototype, 'getPreferredCanvasFormat', function () { return 'bgra8unorm'; });
    define(GPU.prototype, 'requestAdapter', function (options) {
        return new Promise(function (resolve) {
            if (gone || !native.available()) { resolve(null); return; }
            var id = native.newId();
            if (!id) { resolve(null); return; }
            var r = query(G.REQUEST_ADAPTER, [id], options || {}, true);
            var info = r ? r.value() : null;
            if (!info) { native.freeId(id); resolve(null); return; }
            var a = Object.create(GPUAdapter.prototype);
            define(a, S, { id: id, used: false });
            define(a, '_features', setlike(GPUSupportedFeatures, info.features || []));
            define(a, '_limits', limits(null));
            define(a, '_info', readonly(GPUAdapterInfo, { vendor: String(info.vendor || '').toLowerCase(),
                architecture: String(info.architecture || '').toLowerCase(), device: '', description: '',
                subgroupMinSize: 4, subgroupMaxSize: 128, isFallbackAdapter: false }));
            resolve(a);
        });
    });
    var AP = GPUAdapter.prototype;
    Object.defineProperty(AP, 'features', { get: function () { return st(this, GPUAdapter) && this._features; }, configurable: true });
    Object.defineProperty(AP, 'limits', { get: function () { return st(this, GPUAdapter) && this._limits; }, configurable: true });
    Object.defineProperty(AP, 'info', { get: function () { return st(this, GPUAdapter) && this._info; }, configurable: true });
    Object.defineProperty(AP, 'isFallbackAdapter', { get: function () { return false; }, configurable: true });
    define(AP, 'requestAdapterInfo', function () { return Promise.resolve(this._info); });
    define(AP, 'requestDevice', function (desc) {
        var a = st(this, GPUAdapter), self = this;
        desc = desc || {};
        return new Promise(function (resolve, reject) {
            var feats = Array.from(desc.requiredFeatures || []).map(String);
            for (var i = 0; i < feats.length; i++)
                if (!self._features.has(feats[i])) { reject(new TypeError("feature '" + feats[i] + "' is not supported")); return; }
            var req = desc.requiredLimits || {};
            for (var k in req) {
                if (req[k] === undefined) continue;
                if (!(k in DEFAULT_LIMITS)) { reject(domError("unknown limit '" + k + "'", 'OperationError')); return; }
                var better = /^min/.test(k) ? req[k] >= DEFAULT_LIMITS[k] : req[k] <= DEFAULT_LIMITS[k];
                if (!better) { reject(domError("limit '" + k + "' exceeds what the adapter has", 'OperationError')); return; }
            }
            if (a.used) { reject(domError('the adapter was already used', 'OperationError')); return; }
            a.used = true;
            var id = native.newId(), qid = native.newId();
            var r = id && qid ? query(G.REQUEST_DEVICE, [id, qid, a.id], { requiredFeatures: feats }, true) : null;
            var lim = r ? r.value() : null;
            var device = deviceObject(id, qid, desc, feats, lim);
            if (!lim) {
                /* A device that is lost already, as WebGPU has it. */
                device[S].lost = true;
                device[S].resolveLost(readonly(GPUDeviceLostInfo, { reason: 'unknown', message: 'no device' }));
            }
            resolve(device);
        });
    });

    /* ---- the device ---- */
    var devices = [];
    function deviceObject(id, qid, desc, feats, lim) {
        var d;
        try { d = ET ? Reflect.construct(ET, [], GPUDevice) : Object.create(GPUDevice.prototype); }
        catch (e) { d = Object.create(GPUDevice.prototype); }
        var resolveLost;
        var lostPromise = new Promise(function (r) { resolveLost = r; });
        var s = { id: id, device: null, lost: false, resolveLost: resolveLost, lostPromise: lostPromise, scopes: 0 };
        define(d, S, s);
        define(d, '_label', desc.label === undefined ? '' : String(desc.label));
        define(d, '_features', setlike(GPUSupportedFeatures, feats));
        define(d, '_limits', limits(lim));
        var q = Object.create(GPUQueue.prototype);
        define(q, S, { id: qid, device: d, released: false });
        define(q, '_label', '');
        define(d, '_queue', q);
        define(d, '_onuncapturederror', null);
        s.device = d;
        devices.push(d);
        return d;
    }
    function lostAll() {
        gone = true;
        devices.forEach(function (d) {
            var s = d[S];
            if (s.lost) return;
            s.lost = true;
            s.resolveLost(readonly(GPUDeviceLostInfo, { reason: 'unknown', message: 'the GPU process ended' }));
        });
    }
    var DP = GPUDevice.prototype;
    Object.defineProperty(DP, 'features', { get: function () { st(this, GPUDevice); return this._features; }, configurable: true });
    Object.defineProperty(DP, 'limits', { get: function () { st(this, GPUDevice); return this._limits; }, configurable: true });
    Object.defineProperty(DP, 'queue', { get: function () { st(this, GPUDevice); return this._queue; }, configurable: true });
    Object.defineProperty(DP, 'lost', { get: function () { return st(this, GPUDevice).lostPromise; }, configurable: true });
    Object.defineProperty(DP, 'adapterInfo', { get: function () { return readonly(GPUAdapterInfo, { vendor: '', architecture: '', device: '', description: '' }); }, configurable: true });
    Object.defineProperty(DP, 'onuncapturederror', { configurable: true, enumerable: true,
        get: function () { return this._onuncapturederror; },
        set: function (f) { this._onuncapturederror = typeof f === 'function' ? f : null; } });
    define(DP, 'destroy', function () {
        var s = st(this, GPUDevice);
        if (s.lost) return;
        rec(G.DESTROY, [s.id]);
        s.lost = true;
        s.resolveLost(readonly(GPUDeviceLostInfo, { reason: 'destroyed', message: '' }));
    });
    function creator(name, op, F, shape, after) {
        define(DP, name, function (desc) {
            var s = st(this, GPUDevice);
            desc = desc || {};
            var o = make(F, this, desc.label, shape ? shape(desc) : null);
            rec(op, [idOf(o), s.id], desc, true);
            if (after) after(o, desc);
            return o;
        });
    }
    function extent(sz) {
        if (sz === undefined || sz === null) throw new TypeError('size is required');
        var a = Array.isArray(sz) || typeof sz[Symbol.iterator] === 'function' ? Array.from(sz) : null;
        return a ? { width: a[0] >>> 0, height: a.length > 1 ? a[1] >>> 0 : 1, depthOrArrayLayers: a.length > 2 ? a[2] >>> 0 : 1 }
                 : { width: sz.width >>> 0, height: sz.height === undefined ? 1 : sz.height >>> 0,
                     depthOrArrayLayers: sz.depthOrArrayLayers === undefined ? 1 : sz.depthOrArrayLayers >>> 0 };
    }
    creator('createBuffer', G.CREATE_BUFFER, GPUBuffer, function (d) {
        var size = Number(d.size) || 0, usage = d.usage >>> 0;
        return { size: size, usage: usage, mapState: d.mappedAtCreation ? 'mapped' : 'unmapped',
                 map: d.mappedAtCreation ? { mode: 2, offset: 0, size: size, data: null, ranges: [] } : null };
    });
    creator('createTexture', G.CREATE_TEXTURE, GPUTexture, function (d) {
        var e = extent(d.size);
        return { width: e.width, height: e.height, depthOrArrayLayers: e.depthOrArrayLayers,
                 mipLevelCount: d.mipLevelCount === undefined ? 1 : d.mipLevelCount >>> 0,
                 sampleCount: d.sampleCount === undefined ? 1 : d.sampleCount >>> 0,
                 dimension: d.dimension || '2d', format: String(d.format), usage: d.usage >>> 0 };
    });
    creator('createSampler', G.CREATE_SAMPLER, GPUSampler);
    creator('createShaderModule', G.CREATE_SHADER_MODULE, GPUShaderModule);
    creator('createBindGroupLayout', G.CREATE_BIND_GROUP_LAYOUT, GPUBindGroupLayout);
    creator('createPipelineLayout', G.CREATE_PIPELINE_LAYOUT, GPUPipelineLayout);
    creator('createRenderPipeline', G.CREATE_RENDER_PIPELINE, GPURenderPipeline);
    creator('createComputePipeline', G.CREATE_COMPUTE_PIPELINE, GPUComputePipeline);
    /* Bind groups: a buffer given as itself is { buffer }. */
    define(DP, 'createBindGroup', function (desc) {
        var s = st(this, GPUDevice);
        desc = desc || {};
        var entries = Array.from(desc.entries || []).map(function (e) {
            var r = e.resource;
            if (r instanceof GPUBuffer) r = { buffer: r };
            return { binding: e.binding, resource: r };
        });
        var o = make(GPUBindGroup, this, desc.label);
        rec(G.CREATE_BIND_GROUP, [idOf(o), s.id], { layout: desc.layout, entries: entries }, true);
        return o;
    });
    define(DP, 'createRenderPipelineAsync', function (desc) {
        var self = this;
        return new Promise(function (resolve) { resolve(self.createRenderPipeline(desc)); });
    });
    define(DP, 'createComputePipelineAsync', function (desc) {
        var self = this;
        return new Promise(function (resolve) { resolve(self.createComputePipeline(desc)); });
    });
    define(DP, 'createCommandEncoder', function (desc) {
        var s = st(this, GPUDevice);
        var o = make(GPUCommandEncoder, this, desc && desc.label);
        rec(G.CREATE_COMMAND_ENCODER, [idOf(o), s.id]);
        return o;
    });
    define(DP, 'createQuerySet', function () {
        throw domError('query sets are not supported', 'NotSupportedError');
    });
    define(DP, 'importExternalTexture', function () {
        throw domError('external textures are not supported', 'NotSupportedError');
    });
    define(DP, 'pushErrorScope', function (filter) {
        var s = st(this, GPUDevice);
        filter = String(filter);
        if (['validation', 'out-of-memory', 'internal'].indexOf(filter) < 0)
            throw new TypeError("'" + filter + "' is not a valid GPUErrorFilter");
        s.scopes++;
        rec(G.PUSH_ERROR_SCOPE, [s.id], { filter: filter }, true);
    });
    define(DP, 'popErrorScope', function () {
        var s = st(this, GPUDevice);
        return new Promise(function (resolve, reject) {
            if (s.lost) { resolve(null); return; }
            if (s.scopes <= 0) { reject(domError('no error scope to pop', 'OperationError')); return; }
            s.scopes--;
            var r = query(G.POP_ERROR_SCOPE, [s.id]);
            var e = r ? r.value() : null;
            if (!e) { resolve(null); return; }
            var F = e.type === 'validation' ? GPUValidationError : e.type === 'out-of-memory' ? GPUOutOfMemoryError
                                                                                              : GPUInternalError;
            resolve(new F(e.message));
        });
    });

    /* ---- buffers ---- */
    var BP = GPUBuffer.prototype;
    ['size', 'usage', 'mapState'].forEach(function (k) {
        Object.defineProperty(BP, k, { get: function () { return st(this, GPUBuffer)[k]; }, configurable: true, enumerable: true });
    });
    define(BP, 'mapAsync', function (mode, offset, size) {
        var s = st(this, GPUBuffer);
        mode >>>= 0;
        offset = offset === undefined ? 0 : Number(offset);
        size = size === undefined ? Math.max(0, s.size - offset) : Number(size);
        return new Promise(function (resolve, reject) {
            if (s.mapState !== 'unmapped') { reject(domError('the buffer is mapped or being mapped', 'OperationError')); return; }
            if ((mode !== 1 && mode !== 2) || offset % 8 || size % 4 || offset + size > s.size ||
                !(s.usage & mode) || s.destroyed) {
                reject(domError('mapAsync: invalid mapping', 'OperationError'));
                return;
            }
            s.mapState = 'pending';
            var r = query(G.BUFFER_MAP, [s.id, mode], null, false, function () { f64(offset); f64(size); });
            if (!r || !r.u32() || s.mapState !== 'pending') {
                s.mapState = 'unmapped';
                reject(domError('mapAsync failed', 'OperationError'));
                return;
            }
            var data = null;
            if (mode === 1) { var n = r.u32(); data = r.b.slice(r.at, r.at + n); }
            s.map = { mode: mode, offset: offset, size: size, data: data, ranges: [] };
            s.mapState = 'mapped';
            resolve(undefined);
        });
    });
    define(BP, 'getMappedRange', function (offset, size) {
        var s = st(this, GPUBuffer), m = s.map;
        if (s.mapState !== 'mapped' || !m) throw domError('the buffer is not mapped', 'OperationError');
        offset = offset === undefined ? 0 : Number(offset);
        size = size === undefined ? Math.max(0, m.offset + m.size - offset) : Number(size);
        if (offset % 8 || size % 4 || offset < m.offset || offset + size > m.offset + m.size)
            throw domError('getMappedRange: out of the mapped range', 'OperationError');
        for (var i = 0; i < m.ranges.length; i++) {
            var x = m.ranges[i];
            if (offset < x.offset + x.size && x.offset < offset + size) throw domError('getMappedRange: overlaps', 'OperationError');
        }
        var ab = new ArrayBuffer(size);
        if (m.data) new Uint8Array(ab).set(m.data.subarray(offset - m.offset, offset - m.offset + size));
        m.ranges.push({ offset: offset, size: size, ab: ab });
        return ab;
    });
    define(BP, 'unmap', function () {
        var s = st(this, GPUBuffer), m = s.map;
        if (s.mapState === 'pending') { s.mapState = 'unmapped'; return; }
        if (s.mapState !== 'mapped' || !m) return;
        var bytes = null, at = 0;
        if (m.mode === 2 && m.ranges.length) {
            /* What the page wrote, over the span of its ranges. */
            var lo = Infinity, hi = 0;
            m.ranges.forEach(function (x) { lo = Math.min(lo, x.offset); hi = Math.max(hi, x.offset + x.size); });
            bytes = new Uint8Array(hi - lo);
            m.ranges.forEach(function (x) { if (x.ab.byteLength) bytes.set(new Uint8Array(x.ab), x.offset - lo); });
            at = lo;
        }
        rec(G.BUFFER_UNMAP, [s.id], null, false, function () { f64(at); if (bytes) raw(bytes); });
        m.ranges.forEach(function (x) { try { if (x.ab.transfer) x.ab.transfer(); } catch (e) {} });
        s.map = null;
        s.mapState = 'unmapped';
    });
    define(BP, 'destroy', function () {
        var s = st(this, GPUBuffer);
        if (s.mapState !== 'unmapped') this.unmap();
        s.destroyed = true;
        rec(G.DESTROY, [s.id]);
    });

    /* ---- textures ---- */
    var TP = GPUTexture.prototype;
    ['width', 'height', 'depthOrArrayLayers', 'mipLevelCount', 'sampleCount', 'dimension', 'format', 'usage']
        .forEach(function (k) {
            Object.defineProperty(TP, k, { get: function () { return st(this, GPUTexture)[k]; }, configurable: true, enumerable: true });
        });
    define(TP, 'createView', function (desc) {
        var s = st(this, GPUTexture);
        desc = desc || {};
        var v = make(GPUTextureView, s.device, desc.label, { texture: this });
        rec(G.CREATE_VIEW, [idOf(v), s.id], desc, true);
        return v;
    });
    define(TP, 'destroy', function () {
        var s = st(this, GPUTexture);
        if (s.canvas) return;           /* a canvas's own: it goes when presented */
        rec(G.DESTROY, [s.id]);
    });

    /* ---- shader modules and pipelines ---- */
    define(GPUShaderModule.prototype, 'getCompilationInfo', function () {
        var s = st(this, GPUShaderModule);
        return new Promise(function (resolve) {
            var r = query(G.COMPILATION_INFO, [s.id]);
            var msgs = r ? r.value() || [] : [];
            resolve(readonly(GPUCompilationInfo, { messages: Object.freeze(msgs) }));
        });
    });
    [GPURenderPipeline, GPUComputePipeline].forEach(function (F) {
        define(F.prototype, 'getBindGroupLayout', function (index) {
            var s = st(this, F);
            var o = make(GPUBindGroupLayout, s.device, '');
            rec(G.GET_BIND_GROUP_LAYOUT, [idOf(o), s.id, index >>> 0]);
            return o;
        });
    });

    /* ---- command encoders and passes ---- */
    var EP = GPUCommandEncoder.prototype;
    function encoder(o) {
        var s = st(o, GPUCommandEncoder);
        if (s.finished) throw domError('the encoder is finished', 'OperationError');
        return s;
    }
    define(EP, 'beginRenderPass', function (desc) {
        var s = encoder(this);
        var p = make(GPURenderPassEncoder, s.device, desc && desc.label, { encoder: this, cmds: [] });
        rec(G.BEGIN_RENDER_PASS, [idOf(p), s.id], desc || {}, true);
        return p;
    });
    define(EP, 'beginComputePass', function (desc) {
        var s = encoder(this);
        var p = make(GPUComputePassEncoder, s.device, desc && desc.label, { encoder: this, cmds: [] });
        rec(G.BEGIN_COMPUTE_PASS, [idOf(p), s.id]);
        return p;
    });
    function encodeCmd(name) {
        return function () {
            var s = encoder(this);
            rec(G.ENCODE, [s.id], [[name].concat(Array.prototype.slice.call(arguments))], true);
        };
    }
    define(EP, 'copyBufferToBuffer', function (src, a, b, c, d) {
        /* (source, sourceOffset, destination, destinationOffset, size) or (source, destination[, size]) */
        if (a instanceof GPUBuffer)
            return encodeCmd('copyBufferToBuffer').call(this, src, 0, a, 0, b === undefined ? src[S].size : b);
        return encodeCmd('copyBufferToBuffer').call(this, src, a, b, c, d === undefined ? src[S].size - a : d);
    });
    /* Buffer copies carry the buffer and its layout in one dictionary. */
    define(EP, 'copyBufferToTexture', function (src, dst, size) {
        encodeCmd('copyBufferToTexture').call(this, src, dst, extent(size));
    });
    define(EP, 'copyTextureToBuffer', function (src, dst, size) {
        encodeCmd('copyTextureToBuffer').call(this, src, dst, extent(size));
    });
    define(EP, 'copyTextureToTexture', function (src, dst, size) {
        encodeCmd('copyTextureToTexture').call(this, src, dst, extent(size));
    });
    define(EP, 'clearBuffer', function (b, offset, size) {
        encodeCmd('clearBuffer').call(this, b, offset === undefined ? 0 : offset, size);
    });
    define(EP, 'pushDebugGroup', function () {});
    define(EP, 'popDebugGroup', function () {});
    define(EP, 'insertDebugMarker', function () {});
    define(EP, 'resolveQuerySet', function () {});
    define(EP, 'finish', function (desc) {
        var s = encoder(this);
        s.finished = true;
        var cb = make(GPUCommandBuffer, s.device, desc && desc.label);
        rec(G.FINISH, [idOf(cb), s.id]);
        done(this);
        return cb;
    });
    function passMethods(F, names) {
        var P = F.prototype;
        names.forEach(function (n) {
            define(P, n, function () {
                var s = st(this, F);
                if (s.ended) throw domError('the pass has ended', 'OperationError');
                var args = Array.prototype.slice.call(arguments);
                if (n === 'setBindGroup' && args.length > 3) {
                    /* (index, group, Uint32Array, start, length) */
                    var data = args[2], start = args[3] >>> 0, count = args[4] >>> 0;
                    args = [args[0], args[1], Array.from(data.subarray(start, start + count))];
                } else if (n === 'setBlendConstant') {
                    var c = args[0];
                    args = Array.isArray(c) || (c && typeof c[Symbol.iterator] === 'function') ? Array.from(c) : [c.r, c.g, c.b, c.a];
                }
                s.cmds.push([n].concat(args));
            });
        });
        define(P, 'end', function () {
            var s = st(this, F);
            if (s.ended) throw domError('the pass has ended', 'OperationError');
            s.ended = true;
            if (s.cmds.length) rec(G.PASS, [s.id], s.cmds, true);
            s.cmds = [];
            rec(G.END_PASS, [s.id]);
            done(this);
        });
        ['pushDebugGroup', 'popDebugGroup', 'insertDebugMarker'].forEach(function (n) { define(P, n, function () {}); });
    }
    passMethods(GPURenderPassEncoder, ['setPipeline', 'setBindGroup', 'setVertexBuffer', 'setIndexBuffer', 'draw',
        'drawIndexed', 'drawIndirect', 'drawIndexedIndirect', 'setViewport', 'setScissorRect', 'setBlendConstant',
        'setStencilReference']);
    passMethods(GPUComputePassEncoder, ['setPipeline', 'setBindGroup', 'dispatchWorkgroups', 'dispatchWorkgroupsIndirect']);
    define(GPURenderPassEncoder.prototype, 'executeBundles', function () {
        throw domError('render bundles are not supported', 'NotSupportedError');
    });
    define(GPURenderPassEncoder.prototype, 'beginOcclusionQuery', function () {});
    define(GPURenderPassEncoder.prototype, 'endOcclusionQuery', function () {});
    /* (Commands past the pass's ids come as numbers: big offsets stay f64.) */

    /* ---- the queue ---- */
    var QP = GPUQueue.prototype;
    define(QP, 'submit', function (buffers) {
        var s = st(this, GPUQueue);
        var list = Array.from(buffers || []);
        rec(G.SUBMIT, [s.id], list, true);
        list.forEach(function (b) { if (b instanceof GPUCommandBuffer) done(b); });
    });
    define(QP, 'onSubmittedWorkDone', function () {
        var s = st(this, GPUQueue);
        return new Promise(function (resolve) {
            query(G.WORK_DONE, [s.id]);
            resolve(undefined);
        });
    });
    define(QP, 'writeBuffer', function (buffer, offset, data, dataOffset, size) {
        var s = st(this, GPUQueue);
        var b = bytesOf(data);
        if (!b) throw new TypeError("writeBuffer: parameter 3 is not of type 'BufferSource'.");
        var per = ArrayBuffer.isView(data) && data.BYTES_PER_ELEMENT ? data.BYTES_PER_ELEMENT : 1;
        var start = (dataOffset === undefined ? 0 : Number(dataOffset)) * per;
        var n = size === undefined ? b.length - start : Number(size) * per;
        if (start < 0 || n < 0 || start + n > b.length) throw domError('writeBuffer: out of the data', 'OperationError');
        var part = b.subarray(start, start + n);
        rec(G.WRITE_BUFFER, [s.id, idOf(buffer)], null, false, function () { f64(Number(offset) || 0); raw(part); });
    });
    define(QP, 'writeTexture', function (destination, data, layout, size) {
        var s = st(this, GPUQueue);
        var b = bytesOf(data);
        if (!b) throw new TypeError("writeTexture: parameter 2 is not of type 'BufferSource'.");
        rec(G.WRITE_TEXTURE, [s.id], { destination: destination, layout: layout || {}, size: extent(size) }, true,
            function () { u8(V_BYTES); u32(b.length); raw(b); });
    });
    /* An image, a video, an ImageBitmap or ImageData, or a canvas: its
     * pixels written to the texture. */
    define(QP, 'copyExternalImageToTexture', function (source, destination, size) {
        var src = source && source.source !== undefined ? source.source : source;
        var flipY = !!(source && source.flipY);
        var w, h, px;
        if (src && src.data && typeof src.width === 'number') {
            w = src.width; h = src.height; px = new Uint8Array(src.data.buffer || src.data, src.data.byteOffset || 0, w * h * 4);
        } else {
            var img = src ? native.imageSource(src) : null;
            if (!img) throw domError('the source has no pixels to copy', 'OperationError');
            w = img.width; h = img.height; px = new Uint8Array(img.data);
        }
        var e = extent(size);
        var tex = destination.texture, fmt = tex && tex[S] ? tex[S].format : 'rgba8unorm';
        var ox = source && source.origin ? (source.origin.x !== undefined ? source.origin.x : source.origin[0] || 0) : 0;
        var oy = source && source.origin ? (source.origin.y !== undefined ? source.origin.y : source.origin[1] || 0) : 0;
        if (ox + e.width > w || oy + e.height > h) throw domError('the copy is outside the source', 'OperationError');
        var out = new Uint8Array(e.width * e.height * 4), bgra = /^bgra/.test(fmt);
        var premult = !!destination.premultipliedAlpha;
        for (var y = 0; y < e.height; y++) {
            var sy = flipY ? oy + e.height - 1 - y : oy + y;
            for (var x = 0; x < e.width; x++) {
                var si = (sy * w + ox + x) * 4, di = (y * e.width + x) * 4;
                var a = px[si + 3], m = premult ? a / 255 : 1;
                out[di] = Math.round(px[si + (bgra ? 2 : 0)] * m);
                out[di + 1] = Math.round(px[si + 1] * m);
                out[di + 2] = Math.round(px[si + (bgra ? 0 : 2)] * m);
                out[di + 3] = a;
            }
        }
        this.writeTexture({ texture: tex, mipLevel: destination.mipLevel || 0, origin: destination.origin || [0, 0, 0],
                            aspect: 'all' }, out, { offset: 0, bytesPerRow: e.width * 4, rowsPerImage: e.height },
                          [e.width, e.height, 1]);
    });

    /* ---- the canvas ---- */
    var CP = GPUCanvasContext.prototype;
    function canvasContext(canvas) {
        if (gone || !native.available()) return null;
        var id = native.newId();
        if (!id) return null;
        var cx = Object.create(GPUCanvasContext.prototype);
        define(cx, S, { id: id, canvas: canvas, config: null, current: null, queued: false });
        define(cx, '__resize', function (w, h) {
            var s = cx[S];
            if (!s.config) return;
            native.resize(s.id, w, h);
        });
        return cx;
    }
    Object.defineProperty(CP, 'canvas', { get: function () { return st(this, GPUCanvasContext).canvas; }, configurable: true });
    define(CP, 'configure', function (config) {
        var s = st(this, GPUCanvasContext);
        if (!config || !(config.device instanceof GPUDevice)) throw new TypeError("configure: 'device' is required");
        var fmt = String(config.format);
        if (['bgra8unorm', 'rgba8unorm', 'rgba16float'].indexOf(fmt) < 0)
            throw new TypeError("configure: '" + fmt + "' is not a canvas format");
        s.config = { device: config.device, format: fmt, usage: config.usage === undefined ? 16 : config.usage >>> 0,
                     alphaMode: config.alphaMode || 'opaque', viewFormats: Array.from(config.viewFormats || []),
                     colorSpace: config.colorSpace || 'srgb', toneMapping: config.toneMapping || { mode: 'standard' } };
        if (s.current) { done(s.current); s.current = null; }
        rec(G.CONFIGURE, [s.id, config.device[S].id], { format: fmt, alphaMode: s.config.alphaMode,
                                                         width: s.canvas.width, height: s.canvas.height }, true);
    });
    define(CP, 'unconfigure', function () {
        var s = st(this, GPUCanvasContext);
        if (s.current) { done(s.current); s.current = null; }
        s.config = null;
    });
    define(CP, 'getConfiguration', function () {
        var c = st(this, GPUCanvasContext).config;
        return c ? Object.assign({}, c) : null;
    });
    define(CP, 'getCurrentTexture', function () {
        var s = st(this, GPUCanvasContext);
        if (!s.config) throw domError('the canvas context is not configured', 'InvalidStateError');
        if (s.current) return s.current;
        var c = s.canvas;
        var t = make(GPUTexture, s.config.device, '', { width: c.width, height: c.height, depthOrArrayLayers: 1,
            mipLevelCount: 1, sampleCount: 1, dimension: '2d', format: s.config.format, usage: s.config.usage, canvas: true });
        rec(G.CURRENT_TEXTURE, [idOf(t), s.id]);
        s.current = t;
        if (!s.queued) { s.queued = true; toPresent.push(this); soon(); }
        return t;
    });
    /* Its frame goes to the page; its texture for this frame is done. */
    function present(cx) {
        var s = cx[S];
        s.queued = false;
        if (s.current) { done(s.current); s.current = null; }
        flush(false);
        if (!gone && !native.present(s.id, s.canvas)) {
            var lost = native.lost();
            if (lost.indexOf(-1) >= 0) lostAll();
        }
    }
    native.gpuCanvas = canvasContext;

    /* ---- navigator.gpu ---- */
    var NP = global.Navigator && global.Navigator.prototype;
    if (NP) Object.defineProperty(NP, 'gpu', { get: function () { return gpu; }, configurable: true, enumerable: true });
    else if (global.navigator) define(global.navigator, 'gpu', gpu);
})

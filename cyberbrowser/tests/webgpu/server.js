// The web-GPU tests' server (webgpu_test.sh).  The pages report with
// console.error lines starting "[gpu]", which land in the tab's log.
//   node server.js PORT
const http = require('http');
const PORT = Number(process.argv[2]);

const page = (title, body) =>
  `<!doctype html><html><head><title>${title}</title></head><body style="margin:0">${body}</body></html>`;
const say = `function say(s) { console.error('[gpu] ' + s); }
  window.onerror = function (m, f, l) { say('error ' + m + ' at ' + l); };
  window.addEventListener('unhandledrejection', function (e) { say('rejected ' + (e.reason && (e.reason.stack || e.reason))); });`;

// A WebGL triangle (version 1 or 2): its pixels read back, the errors and
// what the context says of itself.
const glPage = (version) => page('gl' + version, `<canvas id="c" width="64" height="64"></canvas><script>
  ${say}
  var c = document.getElementById('c');
  var gl = c.getContext(${version === 2 ? "'webgl2'" : "'webgl'"}, { preserveDrawingBuffer: true });
  say('context ' + (gl ? Object.prototype.toString.call(gl) : 'null'));
  if (gl) {
    say('version ' + gl.getParameter(gl.VERSION) + ' / ' + gl.getParameter(gl.SHADING_LANGUAGE_VERSION));
    say('max texture ' + gl.getParameter(gl.MAX_TEXTURE_SIZE) + ' viewport ' + Array.from(gl.getParameter(gl.MAX_VIEWPORT_DIMS)).join('x'));
    function shader(type, src) {
      var s = gl.createShader(type);
      gl.shaderSource(s, src);
      gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) say('compile failed: ' + gl.getShaderInfoLog(s));
      return s;
    }
    var p = gl.createProgram();
    gl.attachShader(p, shader(gl.VERTEX_SHADER, 'attribute vec2 pos; void main() { gl_Position = vec4(pos, 0.0, 1.0); }'));
    gl.attachShader(p, shader(gl.FRAGMENT_SHADER, 'precision mediump float; uniform vec4 color; void main() { gl_FragColor = color; }'));
    gl.linkProgram(p);
    say('linked ' + gl.getProgramParameter(p, gl.LINK_STATUS) + ' attributes ' + gl.getProgramParameter(p, gl.ACTIVE_ATTRIBUTES));
    gl.useProgram(p);
    var b = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, b);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1]), gl.STATIC_DRAW);
    var loc = gl.getAttribLocation(p, 'pos');
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
    gl.uniform4fv(gl.getUniformLocation(p, 'color'), [1, 0.5, 0, 1]);
    say('uniform ' + Array.from(gl.getUniform(p, gl.getUniformLocation(p, 'color'))).join(','));
    gl.clearColor(0, 0, 1, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    var px = new Uint8Array(4), px2 = new Uint8Array(4);
    gl.readPixels(8, 8, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
    gl.readPixels(56, 56, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px2);
    say('inside ' + Array.from(px).join(',') + ' outside ' + Array.from(px2).join(','));
    say('error ' + gl.getError());
    gl.bindBuffer(gl.ARRAY_BUFFER, b);
    gl.drawArrays(gl.TRIANGLES, 0, 300000);
    say('out of range draw error ' + gl.getError());
    gl.bufferSubData(gl.ARRAY_BUFFER, 1000, new Uint8Array(4));
    say('out of range write error ' + gl.getError());
    say('bound buffer ' + (gl.getParameter(gl.ARRAY_BUFFER_BINDING) === b) + ' program ' + (gl.getParameter(gl.CURRENT_PROGRAM) === p));
    say('extensions ' + gl.getSupportedExtensions().length + ' oes_vao ' + !!gl.getExtension('OES_vertex_array_object') + ' timer ' + !!gl.getExtension('EXT_disjoint_timer_query'));
    var t = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 2, 2, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(16));
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    say('texture error ' + gl.getError() + ' filter ' + gl.getTexParameter(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER));
    ${version === 2 ? `
    var vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    say('vao ' + gl.isVertexArray(vao) + ' bound ' + (gl.getParameter(gl.VERTEX_ARRAY_BINDING) === vao));
    gl.bindVertexArray(null);
    var ub = gl.createBuffer();
    gl.bindBuffer(gl.UNIFORM_BUFFER, ub);
    gl.bufferData(gl.UNIFORM_BUFFER, new Float32Array([1, 2, 3, 4]), gl.DYNAMIC_READ);
    var back = new Float32Array(4);
    gl.getBufferSubData(gl.UNIFORM_BUFFER, 0, back);
    say('buffer read back ' + Array.from(back).join(','));
    var sync = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
    say('sync ' + (gl.clientWaitSync(sync, 0, 0) !== gl.WAIT_FAILED));
    say('webgl2 error ' + gl.getError());` : ''}
    var other = document.createElement('canvas').getContext('webgl');
    var ob = other && other.createBuffer();
    if (other) {
      gl.bindBuffer(gl.ARRAY_BUFFER, ob);
      say('another context\\'s buffer error ' + gl.getError());
    }
    gl.drawArrays(gl.TRIANGLES, 0, 3);
  }
  say('done');
</script>`);

// WebGPU: a compute shader doubling numbers, a triangle drawn into a
// texture and read back, and one drawn to a canvas.
const gpuPage = page('gpu', `<canvas id="c" width="64" height="64"></canvas><script>
  ${say}
  say('navigator.gpu ' + !!navigator.gpu + ' format ' + (navigator.gpu && navigator.gpu.getPreferredCanvasFormat()));
  (async function () {
    var adapter = await navigator.gpu.requestAdapter();
    say('adapter ' + !!adapter);
    if (!adapter) { say('done'); return; }
    say('features ' + Array.from(adapter.features).length + ' timestamp ' + adapter.features.has('timestamp-query'));
    var device = await adapter.requestDevice();
    say('device ' + !!device + ' maxBindGroups ' + device.limits.maxBindGroups);
    // compute
    var module = device.createShaderModule({ code: '@group(0) @binding(0) var<storage, read_write> d: array<u32>;' +
      '@compute @workgroup_size(4) fn main(@builtin(global_invocation_id) id: vec3u) { d[id.x] = d[id.x] * 2u; }' });
    var data = device.createBuffer({ size: 16, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST });
    device.queue.writeBuffer(data, 0, new Uint32Array([1, 2, 3, 21]));
    var read = device.createBuffer({ size: 16, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
    var pipeline = device.createComputePipeline({ layout: 'auto', compute: { module: module, entryPoint: 'main' } });
    var group = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: data } }] });
    var enc = device.createCommandEncoder();
    var pass = enc.beginComputePass();
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, group);
    pass.dispatchWorkgroups(1);
    pass.end();
    enc.copyBufferToBuffer(data, 0, read, 0, 16);
    device.queue.submit([enc.finish()]);
    await read.mapAsync(GPUMapMode.READ);
    say('compute ' + Array.from(new Uint32Array(read.getMappedRange())).join(','));
    read.unmap();
    // a triangle into a texture
    var tri = device.createShaderModule({ code:
      '@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {' +
      '  var p = array<vec2f, 3>(vec2f(-1, -1), vec2f(1, -1), vec2f(-1, 1)); return vec4f(p[i], 0, 1); }' +
      '@fragment fn fs() -> @location(0) vec4f { return vec4f(1, 0.5, 0, 1); }' });
    var rp = device.createRenderPipeline({ layout: 'auto', vertex: { module: tri, entryPoint: 'vs' },
      fragment: { module: tri, entryPoint: 'fs', targets: [{ format: 'rgba8unorm' }] } });
    var tex = device.createTexture({ size: [64, 64], format: 'rgba8unorm',
      usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
    var out = device.createBuffer({ size: 256 * 64, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
    enc = device.createCommandEncoder();
    var rpass = enc.beginRenderPass({ colorAttachments: [{ view: tex.createView(), loadOp: 'clear', storeOp: 'store',
      clearValue: { r: 0, g: 0, b: 1, a: 1 } }] });
    rpass.setPipeline(rp);
    rpass.draw(3);
    rpass.end();
    enc.copyTextureToBuffer({ texture: tex }, { buffer: out, bytesPerRow: 256 }, [64, 64]);
    device.queue.submit([enc.finish()]);
    await out.mapAsync(GPUMapMode.READ);
    var px = new Uint8Array(out.getMappedRange());
    say('inside ' + Array.from(px.subarray(56 * 256 + 8 * 4, 56 * 256 + 8 * 4 + 4)).join(',') +
        ' outside ' + Array.from(px.subarray(8 * 256 + 56 * 4, 8 * 256 + 56 * 4 + 4)).join(','));
    out.unmap();
    // validation errors come back through error scopes
    device.pushErrorScope('validation');
    device.createBuffer({ size: 16, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.STORAGE });
    var err = await device.popErrorScope();
    say('scope ' + (err ? err.constructor.name : 'none'));
    // to the canvas
    var ctx = document.getElementById('c').getContext('webgpu');
    var format = navigator.gpu.getPreferredCanvasFormat();
    ctx.configure({ device: device, format: format });
    var cp = device.createRenderPipeline({ layout: 'auto', vertex: { module: tri, entryPoint: 'vs' },
      fragment: { module: tri, entryPoint: 'fs', targets: [{ format: format }] } });
    var frames = 0;
    function frame() {
      var e = device.createCommandEncoder();
      var p = e.beginRenderPass({ colorAttachments: [{ view: ctx.getCurrentTexture().createView(), loadOp: 'clear',
        storeOp: 'store', clearValue: { r: 0, g: 0, b: 1, a: 1 } }] });
      p.setPipeline(cp);
      p.draw(3);
      p.end();
      device.queue.submit([e.finish()]);
      if (++frames < 30) requestAnimationFrame(frame); else say('canvas frames ' + frames);
    }
    requestAnimationFrame(frame);
    device.lost.then(function (i) { say('device lost ' + i.reason); });
    say('done');
  })();
</script>`);

// Frames drawn until the web-GPU process goes: the canvas's context is
// lost, the page lives on.
const crashPage = page('crash', `<canvas id="c" width="32" height="32"></canvas><script>
  ${say}
  var c = document.getElementById('c');
  var gl = c.getContext('webgl');
  c.addEventListener('webglcontextlost', function (e) { say('context lost'); });
  var n = 0;
  function frame() {
    gl.clearColor(n % 2, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    n++;
    if (gl.isContextLost()) { say('lost after ' + n + ' frames'); setTimeout(function () { say('page still runs'); }, 500); return; }
    if (n < 200) setTimeout(frame, 20); else say('never lost');
  }
  frame();
</script>`);

// What a page sees when there is no web-GPU process (a blocked driver).
const availPage = page('avail', `<canvas id="c"></canvas><script>
  ${say}
  var c = document.getElementById('c');
  say('webgl ' + (c.getContext('webgl') ? 'context' : 'null'));
  navigator.gpu.requestAdapter().then(function (a) { say('adapter ' + (a ? 'some' : 'null')); say('done'); });
</script>`);

const routes = { '/gl1': () => glPage(1), '/gl2': () => glPage(2), '/gpu': () => gpuPage,
                 '/crash': () => crashPage, '/avail': () => availPage };

http.createServer((req, res) => {
  const r = routes[req.url.split('?')[0]];
  if (!r) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { 'Content-Type': 'text/html' });
  res.end(r());
}).listen(PORT, '127.0.0.1');

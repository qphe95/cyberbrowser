#ifndef WEBGPU_WIRE_H
#define WEBGPU_WIRE_H

/* WebGPU calls from a tab to its web-GPU process (TW_GPU_RUN), as records:
 * [u32 op][u32 n][n bytes], little-endian.  Most carry a value tree: a
 * descriptor as the page passed it, after the tab's API checked its shape
 * (browser/webgpu_api.js keeps the same numbers):
 *
 *   value := tag:u8 ...
 *     0 null                1 false              2 true
 *     3 number: f64         4 string: u32 n, n bytes (UTF-8)
 *     5 array: u32 count, values
 *     6 object: u32 count, (string key: u32 n, n bytes; value) pairs
 *     7 object id: u32 (a GPU object of this tab)
 *     8 bytes: u32 n, n bytes
 *
 * Trees are parsed with limits (WIRE_MAX_*); a record whose tree breaks
 * one is malformed. */

#define WIRE_MAX_DEPTH 12
#define WIRE_MAX_NODES 16384
#define WIRE_MAX_STRING (8u << 20)      /* a shader's source */

enum { V_NULL = 0, V_FALSE, V_TRUE, V_NUMBER, V_STRING, V_ARRAY, V_OBJECT, V_ID, V_BYTES };

/* Ops: [u32 id of what it makes or acts on] then a value tree, unless said. */
enum {
    G_REQUEST_ADAPTER = 1,      /* tree(options) -> tree(info) or null */
    G_REQUEST_DEVICE,           /* device id, queue id, tree(descriptor) -> tree(features, limits) or null + why */
    G_CREATE_BUFFER,            /* id, device, tree(descriptor) */
    G_BUFFER_MAP,               /* buffer, mode, offset (f64), size (f64) -> u32 ok + bytes (reading) */
    G_BUFFER_UNMAP,             /* buffer, offset (f64), bytes written (or none) */
    G_DESTROY,                  /* id: a buffer, texture, or device */
    G_CREATE_TEXTURE,           /* id, device, tree */
    G_CREATE_VIEW,              /* id, texture, tree */
    G_CREATE_SAMPLER,           /* id, device, tree */
    G_CREATE_SHADER_MODULE,     /* id, device, tree(code) */
    G_COMPILATION_INFO,         /* module -> tree(messages) */
    G_CREATE_BIND_GROUP_LAYOUT, /* id, device, tree */
    G_CREATE_PIPELINE_LAYOUT,   /* id, device, tree */
    G_CREATE_BIND_GROUP,        /* id, device, tree */
    G_CREATE_RENDER_PIPELINE,   /* id, device, tree */
    G_CREATE_COMPUTE_PIPELINE,  /* id, device, tree */
    G_GET_BIND_GROUP_LAYOUT,    /* id, pipeline, index */
    G_CREATE_COMMAND_ENCODER,   /* id, device */
    G_BEGIN_RENDER_PASS,        /* id, encoder, tree */
    G_BEGIN_COMPUTE_PASS,       /* id, encoder */
    G_PASS,                     /* pass, tree([command, args...]): set pipeline, draw, ... */
    G_END_PASS,                 /* pass */
    G_ENCODE,                   /* encoder, tree([command, args...]): copies, clears */
    G_FINISH,                   /* id (the command buffer), encoder */
    G_SUBMIT,                   /* queue, tree([command buffer ids]) */
    G_WRITE_BUFFER,             /* queue, buffer, offset (f64), bytes */
    G_WRITE_TEXTURE,            /* queue, tree(destination, layout, size), bytes */
    G_WORK_DONE,                /* queue -> u32 1, once its work is done */
    G_CONFIGURE,                /* canvas, device, tree(format, alphaMode, width, height) */
    G_CURRENT_TEXTURE,          /* id (the texture), canvas */
    G_PUSH_ERROR_SCOPE,         /* device, tree(filter) */
    G_POP_ERROR_SCOPE,          /* device -> tree(error) or null */
    G_RELEASE,                  /* id: the page let go of it */
    G_OPS
};

#endif

/* webgpu_wgpu.c - WebGPU on wgpu-native in the web-GPU process (webgpu_wgpu.h). */
#include "webgpu_wgpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define WGPU_SKIP_DECLARATIONS
#include "webgpu.h"
#include "webgpu_wire.h"
#include "gpu_blocklist.h"

/* ---- wgpu's entry points --------------------------------------------------------- */

#define WGPU_FUNCS(X) \
    X(CreateInstance) X(InstanceRequestAdapter) X(InstanceProcessEvents) X(InstanceRelease) \
    X(AdapterRequestDevice) X(AdapterGetInfo) X(AdapterInfoFreeMembers) X(AdapterGetLimits) X(AdapterHasFeature) \
    X(AdapterRelease) X(DeviceGetQueue) X(DeviceDestroy) X(DeviceRelease) X(DeviceCreateBuffer) \
    X(DeviceCreateTexture) X(DeviceCreateSampler) X(DeviceCreateShaderModule) X(DeviceCreateBindGroupLayout) \
    X(DeviceCreatePipelineLayout) X(DeviceCreateBindGroup) X(DeviceCreateRenderPipeline) \
    X(DeviceCreateComputePipeline) X(DeviceCreateCommandEncoder) X(DevicePushErrorScope) X(DevicePopErrorScope) \
    X(DeviceGetLimits) X(BufferMapAsync) X(BufferGetConstMappedRange) X(BufferGetMappedRange) X(BufferUnmap) \
    X(BufferDestroy) X(BufferRelease) X(BufferGetSize) X(TextureCreateView) X(TextureDestroy) X(TextureRelease) \
    X(TextureViewRelease) X(SamplerRelease) X(ShaderModuleRelease) X(ShaderModuleGetCompilationInfo) \
    X(BindGroupLayoutRelease) X(PipelineLayoutRelease) X(BindGroupRelease) X(RenderPipelineRelease) \
    X(ComputePipelineRelease) X(RenderPipelineGetBindGroupLayout) X(ComputePipelineGetBindGroupLayout) \
    X(CommandEncoderBeginRenderPass) X(CommandEncoderBeginComputePass) X(CommandEncoderCopyBufferToBuffer) \
    X(CommandEncoderCopyBufferToTexture) X(CommandEncoderCopyTextureToBuffer) X(CommandEncoderCopyTextureToTexture) \
    X(CommandEncoderClearBuffer) X(CommandEncoderFinish) X(CommandEncoderRelease) X(CommandBufferRelease) \
    X(RenderPassEncoderSetPipeline) X(RenderPassEncoderSetBindGroup) X(RenderPassEncoderSetVertexBuffer) \
    X(RenderPassEncoderSetIndexBuffer) X(RenderPassEncoderDraw) X(RenderPassEncoderDrawIndexed) \
    X(RenderPassEncoderDrawIndirect) X(RenderPassEncoderDrawIndexedIndirect) X(RenderPassEncoderSetViewport) \
    X(RenderPassEncoderSetScissorRect) X(RenderPassEncoderSetBlendConstant) X(RenderPassEncoderSetStencilReference) \
    X(RenderPassEncoderEnd) X(RenderPassEncoderRelease) X(ComputePassEncoderSetPipeline) \
    X(ComputePassEncoderSetBindGroup) X(ComputePassEncoderDispatchWorkgroups) \
    X(ComputePassEncoderDispatchWorkgroupsIndirect) X(ComputePassEncoderEnd) X(ComputePassEncoderRelease) \
    X(QueueSubmit) X(QueueWriteBuffer) X(QueueWriteTexture) X(QueueOnSubmittedWorkDone) X(QueueRelease)

#define WGPU_FIELD(n) WGPUProc##n n;
static struct { WGPU_FUNCS(WGPU_FIELD) } w;
static HMODULE g_dll;
static WGPUInstance g_instance;

static bool load(const wchar_t *dir) {
    if (g_instance) return true;
    if (g_dll) return false;                    /* tried, and failed */
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls\\wgpu_native.dll", dir);
    g_dll = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_dll) {
        fprintf(stderr, "[webgpu] could not load wgpu_native.dll (%lu)\n", GetLastError());
        return false;
    }
#define WGPU_LOAD(n) \
    w.n = (WGPUProc##n)(void *)GetProcAddress(g_dll, "wgpu" #n); \
    if (!w.n) { fprintf(stderr, "[webgpu] wgpu_native.dll has no wgpu%s\n", #n); return false; }
    WGPU_FUNCS(WGPU_LOAD)
    g_instance = w.CreateInstance(NULL);
    return g_instance != NULL;
}

/* Wait (process the instance's events) until *flag is set, or 10 s. */
static void wait_for(volatile int *flag) {
    for (int i = 0; i < 5000 && !*flag; i++) {
        w.InstanceProcessEvents(g_instance);
        if (!*flag) Sleep(i < 50 ? 0 : 2);
    }
}

static WGPUStringView sv(const char *s) {
    WGPUStringView v = { s, s ? strlen(s) : 0 };
    return v;
}

/* ---- value trees --------------------------------------------------------------------- */

typedef struct Node {
    uint8_t tag;
    uint32_t n;                     /* array/object: children; string/bytes: length */
    double num;                     /* number; id */
    const uint8_t *data;            /* string/bytes */
    const uint8_t *key;             /* in an object: its key */
    uint32_t key_len;
    int first, next;                /* children, siblings: node indices, -1 none */
} Node;

typedef struct Tree {
    Node nodes[WIRE_MAX_NODES];
    int n;
} Tree;

static Tree g_tree;

static uint32_t take_u32(const uint8_t **p, const uint8_t *end, bool *ok) {
    if (end - *p < 4) { *ok = false; return 0; }
    uint32_t v;
    memcpy(&v, *p, 4);
    *p += 4;
    return v;
}

/* One value at *p (and what it holds); its node index, -1 if malformed. */
static int parse(Tree *t, const uint8_t **p, const uint8_t *end, int depth) {
    if (depth > WIRE_MAX_DEPTH || *p >= end || t->n >= WIRE_MAX_NODES) return -1;
    int i = t->n++;
    Node *node = &t->nodes[i];
    memset(node, 0, sizeof(*node));
    node->first = node->next = -1;
    node->tag = *(*p)++;
    bool ok = true;
    switch (node->tag) {
    case V_NULL: case V_FALSE: case V_TRUE: break;
    case V_NUMBER:
        if (end - *p < 8) return -1;
        memcpy(&node->num, *p, 8);
        *p += 8;
        break;
    case V_ID: node->num = take_u32(p, end, &ok); break;
    case V_STRING: case V_BYTES:
        node->n = take_u32(p, end, &ok);
        if (!ok || node->n > (uint32_t)(end - *p) || node->n > WIRE_MAX_STRING) return -1;
        node->data = *p;
        *p += node->n;
        break;
    case V_ARRAY: case V_OBJECT: {
        node->n = take_u32(p, end, &ok);
        if (!ok || node->n > WIRE_MAX_NODES) return -1;
        int last = -1;
        for (uint32_t k = 0; k < node->n; k++) {
            const uint8_t *key = NULL;
            uint32_t key_len = 0;
            if (node->tag == V_OBJECT) {
                key_len = take_u32(p, end, &ok);
                if (!ok || key_len > 256 || key_len > (uint32_t)(end - *p)) return -1;
                key = *p;
                *p += key_len;
            }
            int c = parse(t, p, end, depth + 1);
            if (c < 0) return -1;
            t->nodes[c].key = key;
            t->nodes[c].key_len = key_len;
            if (last < 0) t->nodes[i].first = c;
            else t->nodes[last].next = c;
            last = c;
        }
        break;
    }
    default: return -1;
    }
    return ok ? i : -1;
}

static const Node *nd(int i) { return i >= 0 ? &g_tree.nodes[i] : NULL; }

/* A member of object o, or -1. */
static int get(int o, const char *key) {
    const Node *n = nd(o);
    if (!n || n->tag != V_OBJECT) return -1;
    size_t kl = strlen(key);
    for (int c = n->first; c >= 0; c = g_tree.nodes[c].next)
        if (g_tree.nodes[c].key_len == kl && !memcmp(g_tree.nodes[c].key, key, kl)) return c;
    return -1;
}

static int item(int a, uint32_t k) {
    const Node *n = nd(a);
    if (!n || n->tag != V_ARRAY || k >= n->n) return -1;
    int c = n->first;
    while (k-- && c >= 0) c = g_tree.nodes[c].next;
    return c;
}

static uint32_t count(int a) { const Node *n = nd(a); return n && n->tag == V_ARRAY ? n->n : 0; }

static double num(int i, double dflt) {
    const Node *n = nd(i);
    return n && n->tag == V_NUMBER ? n->num : dflt;
}

static uint32_t u32(int i, uint32_t dflt) {
    double v = num(i, dflt);
    return v >= 0 && v <= 4294967295.0 ? (uint32_t)v : dflt;
}

static uint64_t u64(int i, uint64_t dflt) {
    double v = num(i, (double)dflt);
    return v >= 0 && v <= 9007199254740992.0 ? (uint64_t)v : dflt;
}

static bool flag(int i, bool dflt) {
    const Node *n = nd(i);
    return n && n->tag == V_TRUE ? true : n && n->tag == V_FALSE ? false : dflt;
}

/* A string member, NUL-terminated into buf, or NULL. */
static const char *string(int i, char *buf, size_t cap) {
    const Node *n = nd(i);
    if (!n || n->tag != V_STRING || n->n >= cap) return NULL;
    memcpy(buf, n->data, n->n);
    buf[n->n] = 0;
    return buf;
}

/* ---- enums: WebGPU's names to wgpu's values ---------------------------------------------- */

typedef struct Name { const char *s; uint32_t v; } Name;

/* Why the call at hand can't be made.  wgpu-native aborts the process on
 * much of what WebGPU calls a validation error (an enum it can't map, a
 * missing module or view), so those never reach it: the call is skipped,
 * what it would make stays invalid (its id empty), and the page sees a
 * validation error (G_POP_ERROR_SCOPE). */
static const char *g_bad;

static uint32_t lookup(const Name *t, int i, uint32_t dflt) {
    char buf[64];
    const char *s = string(i, buf, sizeof(buf));
    if (!s) return dflt;
    for (; t->s; t++)
        if (!strcmp(t->s, s)) return t->v;
    g_bad = "an unknown enum value";
    return dflt;
}

/* An enum the descriptor must have. */
static uint32_t required(const Name *t, int i) {
    char buf[64];
    if (!string(i, buf, sizeof(buf))) g_bad = "a required enum is missing";
    return lookup(t, i, 0);
}

#define F(n) WGPUTextureFormat_##n
static const Name FORMATS[] = {
    { "r8unorm", F(R8Unorm) }, { "r8snorm", F(R8Snorm) }, { "r8uint", F(R8Uint) }, { "r8sint", F(R8Sint) },
    { "r16uint", F(R16Uint) }, { "r16sint", F(R16Sint) }, { "r16float", F(R16Float) },
    { "rg8unorm", F(RG8Unorm) }, { "rg8snorm", F(RG8Snorm) }, { "rg8uint", F(RG8Uint) }, { "rg8sint", F(RG8Sint) },
    { "r32uint", F(R32Uint) }, { "r32sint", F(R32Sint) }, { "r32float", F(R32Float) },
    { "rg16uint", F(RG16Uint) }, { "rg16sint", F(RG16Sint) }, { "rg16float", F(RG16Float) },
    { "rgba8unorm", F(RGBA8Unorm) }, { "rgba8unorm-srgb", F(RGBA8UnormSrgb) }, { "rgba8snorm", F(RGBA8Snorm) },
    { "rgba8uint", F(RGBA8Uint) }, { "rgba8sint", F(RGBA8Sint) }, { "bgra8unorm", F(BGRA8Unorm) },
    { "bgra8unorm-srgb", F(BGRA8UnormSrgb) }, { "rgb9e5ufloat", F(RGB9E5Ufloat) }, { "rgb10a2uint", F(RGB10A2Uint) },
    { "rgb10a2unorm", F(RGB10A2Unorm) }, { "rg11b10ufloat", F(RG11B10Ufloat) }, { "rg32uint", F(RG32Uint) },
    { "rg32sint", F(RG32Sint) }, { "rg32float", F(RG32Float) }, { "rgba16uint", F(RGBA16Uint) },
    { "rgba16sint", F(RGBA16Sint) }, { "rgba16float", F(RGBA16Float) }, { "rgba32uint", F(RGBA32Uint) },
    { "rgba32sint", F(RGBA32Sint) }, { "rgba32float", F(RGBA32Float) }, { "stencil8", F(Stencil8) },
    { "depth16unorm", F(Depth16Unorm) }, { "depth24plus", F(Depth24Plus) },
    { "depth24plus-stencil8", F(Depth24PlusStencil8) }, { "depth32float", F(Depth32Float) },
    { "depth32float-stencil8", F(Depth32FloatStencil8) }, { NULL, 0 } };
#undef F
#define V(n) WGPUVertexFormat_##n
static const Name VERTEX_FORMATS[] = {
    { "uint8", V(Uint8) }, { "uint8x2", V(Uint8x2) }, { "uint8x4", V(Uint8x4) }, { "sint8", V(Sint8) },
    { "sint8x2", V(Sint8x2) }, { "sint8x4", V(Sint8x4) }, { "unorm8", V(Unorm8) }, { "unorm8x2", V(Unorm8x2) },
    { "unorm8x4", V(Unorm8x4) }, { "snorm8", V(Snorm8) }, { "snorm8x2", V(Snorm8x2) }, { "snorm8x4", V(Snorm8x4) },
    { "uint16", V(Uint16) }, { "uint16x2", V(Uint16x2) }, { "uint16x4", V(Uint16x4) }, { "sint16", V(Sint16) },
    { "sint16x2", V(Sint16x2) }, { "sint16x4", V(Sint16x4) }, { "unorm16", V(Unorm16) },
    { "unorm16x2", V(Unorm16x2) }, { "unorm16x4", V(Unorm16x4) }, { "snorm16", V(Snorm16) },
    { "snorm16x2", V(Snorm16x2) }, { "snorm16x4", V(Snorm16x4) }, { "float16", V(Float16) },
    { "float16x2", V(Float16x2) }, { "float16x4", V(Float16x4) }, { "float32", V(Float32) },
    { "float32x2", V(Float32x2) }, { "float32x3", V(Float32x3) }, { "float32x4", V(Float32x4) },
    { "uint32", V(Uint32) }, { "uint32x2", V(Uint32x2) }, { "uint32x3", V(Uint32x3) }, { "uint32x4", V(Uint32x4) },
    { "sint32", V(Sint32) }, { "sint32x2", V(Sint32x2) }, { "sint32x3", V(Sint32x3) }, { "sint32x4", V(Sint32x4) },
    { "unorm10-10-10-2", V(Unorm10_10_10_2) }, { "unorm8x4-bgra", V(Unorm8x4BGRA) }, { NULL, 0 } };
#undef V
static const Name TOPOLOGY[] = { { "point-list", WGPUPrimitiveTopology_PointList },
    { "line-list", WGPUPrimitiveTopology_LineList }, { "line-strip", WGPUPrimitiveTopology_LineStrip },
    { "triangle-list", WGPUPrimitiveTopology_TriangleList }, { "triangle-strip", WGPUPrimitiveTopology_TriangleStrip },
    { NULL, 0 } };
static const Name INDEX_FORMAT[] = { { "uint16", WGPUIndexFormat_Uint16 }, { "uint32", WGPUIndexFormat_Uint32 }, { NULL, 0 } };
static const Name FRONT_FACE[] = { { "ccw", WGPUFrontFace_CCW }, { "cw", WGPUFrontFace_CW }, { NULL, 0 } };
static const Name CULL[] = { { "none", WGPUCullMode_None }, { "front", WGPUCullMode_Front }, { "back", WGPUCullMode_Back },
    { NULL, 0 } };
static const Name COMPARE[] = { { "never", WGPUCompareFunction_Never }, { "less", WGPUCompareFunction_Less },
    { "equal", WGPUCompareFunction_Equal }, { "less-equal", WGPUCompareFunction_LessEqual },
    { "greater", WGPUCompareFunction_Greater }, { "not-equal", WGPUCompareFunction_NotEqual },
    { "greater-equal", WGPUCompareFunction_GreaterEqual }, { "always", WGPUCompareFunction_Always }, { NULL, 0 } };
static const Name STENCIL_OP[] = { { "keep", WGPUStencilOperation_Keep }, { "zero", WGPUStencilOperation_Zero },
    { "replace", WGPUStencilOperation_Replace }, { "invert", WGPUStencilOperation_Invert },
    { "increment-clamp", WGPUStencilOperation_IncrementClamp }, { "decrement-clamp", WGPUStencilOperation_DecrementClamp },
    { "increment-wrap", WGPUStencilOperation_IncrementWrap }, { "decrement-wrap", WGPUStencilOperation_DecrementWrap },
    { NULL, 0 } };
#define B(n) WGPUBlendFactor_##n
static const Name BLEND_FACTOR[] = { { "zero", B(Zero) }, { "one", B(One) }, { "src", B(Src) },
    { "one-minus-src", B(OneMinusSrc) }, { "src-alpha", B(SrcAlpha) }, { "one-minus-src-alpha", B(OneMinusSrcAlpha) },
    { "dst", B(Dst) }, { "one-minus-dst", B(OneMinusDst) }, { "dst-alpha", B(DstAlpha) },
    { "one-minus-dst-alpha", B(OneMinusDstAlpha) }, { "src-alpha-saturated", B(SrcAlphaSaturated) },
    { "constant", B(Constant) }, { "one-minus-constant", B(OneMinusConstant) }, { NULL, 0 } };
#undef B
static const Name BLEND_OP[] = { { "add", WGPUBlendOperation_Add }, { "subtract", WGPUBlendOperation_Subtract },
    { "reverse-subtract", WGPUBlendOperation_ReverseSubtract }, { "min", WGPUBlendOperation_Min },
    { "max", WGPUBlendOperation_Max }, { NULL, 0 } };
static const Name LOAD_OP[] = { { "load", WGPULoadOp_Load }, { "clear", WGPULoadOp_Clear }, { NULL, 0 } };
static const Name STORE_OP[] = { { "store", WGPUStoreOp_Store }, { "discard", WGPUStoreOp_Discard }, { NULL, 0 } };
static const Name ADDRESS[] = { { "clamp-to-edge", WGPUAddressMode_ClampToEdge }, { "repeat", WGPUAddressMode_Repeat },
    { "mirror-repeat", WGPUAddressMode_MirrorRepeat }, { NULL, 0 } };
static const Name FILTER[] = { { "nearest", WGPUFilterMode_Nearest }, { "linear", WGPUFilterMode_Linear }, { NULL, 0 } };
static const Name MIP_FILTER[] = { { "nearest", WGPUMipmapFilterMode_Nearest }, { "linear", WGPUMipmapFilterMode_Linear },
    { NULL, 0 } };
static const Name DIMENSION[] = { { "1d", WGPUTextureDimension_1D }, { "2d", WGPUTextureDimension_2D },
    { "3d", WGPUTextureDimension_3D }, { NULL, 0 } };
static const Name VIEW_DIMENSION[] = { { "1d", WGPUTextureViewDimension_1D }, { "2d", WGPUTextureViewDimension_2D },
    { "2d-array", WGPUTextureViewDimension_2DArray }, { "cube", WGPUTextureViewDimension_Cube },
    { "cube-array", WGPUTextureViewDimension_CubeArray }, { "3d", WGPUTextureViewDimension_3D }, { NULL, 0 } };
static const Name ASPECT[] = { { "all", WGPUTextureAspect_All }, { "stencil-only", WGPUTextureAspect_StencilOnly },
    { "depth-only", WGPUTextureAspect_DepthOnly }, { NULL, 0 } };
static const Name BUFFER_BINDING[] = { { "uniform", WGPUBufferBindingType_Uniform },
    { "storage", WGPUBufferBindingType_Storage }, { "read-only-storage", WGPUBufferBindingType_ReadOnlyStorage },
    { NULL, 0 } };
static const Name SAMPLER_BINDING[] = { { "filtering", WGPUSamplerBindingType_Filtering },
    { "non-filtering", WGPUSamplerBindingType_NonFiltering }, { "comparison", WGPUSamplerBindingType_Comparison },
    { NULL, 0 } };
static const Name SAMPLE_TYPE[] = { { "float", WGPUTextureSampleType_Float },
    { "unfilterable-float", WGPUTextureSampleType_UnfilterableFloat }, { "depth", WGPUTextureSampleType_Depth },
    { "sint", WGPUTextureSampleType_Sint }, { "uint", WGPUTextureSampleType_Uint }, { NULL, 0 } };
static const Name STORAGE_ACCESS[] = { { "write-only", WGPUStorageTextureAccess_WriteOnly },
    { "read-only", WGPUStorageTextureAccess_ReadOnly }, { "read-write", WGPUStorageTextureAccess_ReadWrite },
    { NULL, 0 } };
static const Name STEP_MODE[] = { { "vertex", WGPUVertexStepMode_Vertex }, { "instance", WGPUVertexStepMode_Instance },
    { NULL, 0 } };
static const Name ERROR_FILTER[] = { { "validation", WGPUErrorFilter_Validation },
    { "out-of-memory", WGPUErrorFilter_OutOfMemory }, { "internal", WGPUErrorFilter_Internal }, { NULL, 0 } };
/* The features a page may ask for.  No timestamp queries (GPU timers). */
static const Name FEATURES[] = { { "depth-clip-control", WGPUFeatureName_DepthClipControl },
    { "depth32float-stencil8", WGPUFeatureName_Depth32FloatStencil8 },
    { "texture-compression-bc", WGPUFeatureName_TextureCompressionBC },
    { "texture-compression-etc2", WGPUFeatureName_TextureCompressionETC2 },
    { "texture-compression-astc", WGPUFeatureName_TextureCompressionASTC },
    { "indirect-first-instance", WGPUFeatureName_IndirectFirstInstance }, { "shader-f16", WGPUFeatureName_ShaderF16 },
    { "rg11b10ufloat-renderable", WGPUFeatureName_RG11B10UfloatRenderable },
    { "bgra8unorm-storage", WGPUFeatureName_BGRA8UnormStorage }, { "float32-filterable", WGPUFeatureName_Float32Filterable },
    { NULL, 0 } };

/* ---- a tab's objects ------------------------------------------------------------------ */

enum { O_NONE = 0, O_ADAPTER, O_DEVICE, O_QUEUE, O_BUFFER, O_TEXTURE, O_VIEW, O_SAMPLER, O_MODULE, O_BGL, O_PL, O_BG,
       O_RPIPE, O_CPIPE, O_ENCODER, O_RPASS, O_CPASS, O_CMDBUF };

#define MAX_IDS 65536
#define MAX_CANVASES 64

/* A buffer's or texture's state, shared by its id and by what uses it
 * (views, bind groups, encoders, command buffers).  wgpu-native aborts the
 * process on a submit that fails WebGPU's checks (a buffer still mapped, a
 * destroyed one, a command buffer encoded wrong or submitted twice), so
 * submits are checked here first, and refused as a validation error. */
typedef struct Res { int refs; bool destroyed, mapped; } Res;

typedef struct Obj {
    uint8_t kind;
    void *h;
    uint32_t device;                /* the device it was made by (buffers: for waiting) */
    uint32_t scopes;                /* a device's error scopes pushed (wgpu-native aborts on a pop of none) */
    bool lost;                      /* a device destroyed: nothing of it reaches wgpu again (much of what
                                     * wgpu-native does on a lost device aborts) */
    bool ended;                     /* a pass ended */
    bool bad;                       /* uses more than can be tracked: never submitted */
    uint32_t parent;                /* a pass's encoder */
    Res *res;                       /* a buffer's or a texture's (a view's: its texture's) */
    Res **uses;                     /* what a bind group, an encoder or a command buffer uses */
    uint32_t nuses, cap_uses;
    uint64_t size, map_off, map_size;   /* a buffer's size, and the range mapped */
} Obj;

typedef struct Canvas {
    uint32_t id, device;
    WGPUTexture texture;
    WGPUTextureFormat format;
    uint32_t width, height;
    bool opaque;
} Canvas;

struct WgpuClient {
    Obj *objs;
    Canvas canvases[MAX_CANVASES];
    uint32_t denied;                /* GPU_FEATURE_* the block list took away */
    bool blocked;
    const char *error;              /* a call refused here (g_bad), for the next error scope popped */
    char error_buf[256];
    Obj *unsubmitted;               /* command buffers let go of unsubmitted (see wgpu_client_free) */
    uint32_t nunsubmitted;
};

static Res *res_hold(Res *r) { if (r) r->refs++; return r; }
static void res_drop(Res *r) { if (r && --r->refs == 0) free(r); }

static void uses_add(Obj *holder, Res *r) {
    if (!holder || !r) return;
    for (uint32_t i = holder->nuses > 64 ? holder->nuses - 64 : 0; i < holder->nuses; i++)
        if (holder->uses[i] == r) return;
    if (holder->nuses == holder->cap_uses) {
        uint32_t cap = holder->cap_uses ? holder->cap_uses * 2 : 16;
        Res **u = cap <= 65536 ? (Res **)realloc(holder->uses, cap * sizeof(*u)) : NULL;
        if (!u) { holder->bad = true; return; }
        holder->uses = u;
        holder->cap_uses = cap;
    }
    holder->uses[holder->nuses++] = res_hold(r);
}

/* While an encoder's commands or a bind group are decoded: what records the
 * buffers and textures they name (handle()). */
static Obj *g_holder;

static void use_obj(Obj *o) {
    if (!g_holder || !o || o == g_holder) return;
    if (o->res) uses_add(g_holder, o->res);
    for (uint32_t i = 0; i < o->nuses; i++) uses_add(g_holder, o->uses[i]);
    if (o->bad) g_holder->bad = true;
}

static Obj *obj(WgpuClient *c, uint32_t id, int kind) {
    if (!id || id >= MAX_IDS || c->objs[id].kind != kind || !c->objs[id].h) return NULL;
    Obj *o = &c->objs[id];
    if (o->lost || (kind != O_ADAPTER && o->device && o->device < MAX_IDS && c->objs[o->device].lost)) return NULL;
    return o;
}

static void *handle(WgpuClient *c, int i, int kind) {
    const Node *n = nd(i);
    if (!n || n->tag != V_ID) return NULL;
    Obj *o = obj(c, (uint32_t)n->num, kind);
    use_obj(o);
    return o ? o->h : NULL;
}

/* An object the descriptor must name. */
static void *need(WgpuClient *c, int i, int kind) {
    void *h = handle(c, i, kind);
    if (!h) g_bad = "a required object is missing or invalid";
    return h;
}

static void release(int kind, void *h) {
    if (!h) return;
    switch (kind) {
    case O_ADAPTER: w.AdapterRelease(h); break;
    case O_DEVICE: w.DeviceRelease(h); break;
    case O_QUEUE: w.QueueRelease(h); break;
    case O_BUFFER: w.BufferRelease(h); break;
    case O_TEXTURE: w.TextureRelease(h); break;
    case O_VIEW: w.TextureViewRelease(h); break;
    case O_SAMPLER: w.SamplerRelease(h); break;
    case O_MODULE: w.ShaderModuleRelease(h); break;
    case O_BGL: w.BindGroupLayoutRelease(h); break;
    case O_PL: w.PipelineLayoutRelease(h); break;
    case O_BG: w.BindGroupRelease(h); break;
    case O_RPIPE: w.RenderPipelineRelease(h); break;
    case O_CPIPE: w.ComputePipelineRelease(h); break;
    case O_ENCODER: w.CommandEncoderRelease(h); break;
    case O_RPASS: w.RenderPassEncoderRelease(h); break;
    case O_CPASS: w.ComputePassEncoderRelease(h); break;
    case O_CMDBUF: w.CommandBufferRelease(h); break;
    }
}

/* Give id to a new object (released if the id is taken, or bad). */
static bool put_obj(WgpuClient *c, uint32_t id, int kind, void *h, uint32_t device) {
    if (!h) return false;
    Res *r = NULL;
    if (!id || id >= MAX_IDS || c->objs[id].kind ||
        ((kind == O_BUFFER || kind == O_TEXTURE) && !(r = (Res *)calloc(1, sizeof(Res))))) {
        release(kind, h);
        return false;
    }
    if (r) r->refs = 1;
    c->objs[id].kind = (uint8_t)kind;
    c->objs[id].h = h;
    c->objs[id].device = device;
    c->objs[id].res = r;
    return true;
}

/* An id let go of (or the client gone): a buffer still mapped is unmapped
 * first, as nothing could unmap it later. */
static void free_slot(WgpuClient *c, Obj *o) {
    bool live = o->device && o->device < MAX_IDS && !c->objs[o->device].lost;
    if (o->kind == O_BUFFER && o->res && o->res->mapped && !o->res->destroyed && live) w.BufferUnmap(o->h);
    if (o->res) o->res->mapped = false;
    release(o->kind, o->h);
    res_drop(o->res);
    for (uint32_t i = 0; i < o->nuses; i++) res_drop(o->uses[i]);
    free(o->uses);
    memset(o, 0, sizeof(*o));
}

static void refuse(WgpuClient *c, const char *why) {
    snprintf(c->error_buf, sizeof(c->error_buf), "%.255s", why);
    c->error = c->error_buf;
}

WgpuClient *wgpu_client_new(const wchar_t *dir) {
    if (!load(dir)) return NULL;
    WgpuClient *c = (WgpuClient *)calloc(1, sizeof(*c));
    if (c) c->objs = (Obj *)calloc(MAX_IDS, sizeof(Obj));
    if (!c || !c->objs) { free(c); return NULL; }
    return c;
}

/* A command buffer finished and never submitted keeps its device's memory
 * in wgpu-native for good (some dozens, and no device can be made).  So
 * when the tab is gone, those still valid are submitted (to no visible
 * effect: nothing of the tab is left to see them) before the release. */
static void submit_leftover(WgpuClient *c, Obj *cb) {
    if (!cb->h || cb->device >= MAX_IDS || c->objs[cb->device].lost || cb->bad) return;
    for (uint32_t r = 0; r < cb->nuses; r++)
        if (cb->uses[r]->destroyed || cb->uses[r]->mapped) return;
    for (uint32_t i = 1; i < MAX_IDS; i++)
        if (c->objs[i].kind == O_QUEUE && c->objs[i].device == cb->device) {
            w.QueueSubmit(c->objs[i].h, 1, (WGPUCommandBuffer *)&cb->h);
            return;
        }
}

void wgpu_client_free(WgpuClient *c) {
    if (!c) return;
    for (int i = 0; i < MAX_CANVASES; i++)
        if (c->canvases[i].texture) w.TextureRelease(c->canvases[i].texture);
    /* Buffers unmapped, command buffers left over submitted; passes and
     * encoders released first, devices last, each device's work finished
     * first, as wgpu-native frees a device's resources only when polled. */
    for (uint32_t i = 1; i < MAX_IDS; i++) {
        Obj *y = &c->objs[i];
        if (y->kind == O_BUFFER && y->res->mapped && !y->res->destroyed && y->device < MAX_IDS &&
            !c->objs[y->device].lost) {
            w.BufferUnmap(y->h);
            y->res->mapped = false;
        }
    }
    for (uint32_t i = 1; i < MAX_IDS; i++)
        if (c->objs[i].kind == O_CMDBUF) submit_leftover(c, &c->objs[i]);
    for (uint32_t i = 0; i < c->nunsubmitted; i++) {
        submit_leftover(c, &c->unsubmitted[i]);
        free_slot(c, &c->unsubmitted[i]);
    }
    free(c->unsubmitted);
    typedef WGPUBool (*Poll)(WGPUDevice, WGPUBool, const void *);
    static Poll poll;
    if (!poll) poll = (Poll)(void *)GetProcAddress(g_dll, "wgpuDevicePoll");
    for (uint32_t i = 1; poll && i < MAX_IDS; i++)
        if (c->objs[i].kind == O_DEVICE && c->objs[i].h) poll(c->objs[i].h, 1, NULL);
    for (int k = O_CMDBUF; k >= O_ADAPTER; k--)
        for (uint32_t i = 1; i < MAX_IDS; i++)
            if (c->objs[i].kind == k) free_slot(c, &c->objs[i]);
    w.InstanceProcessEvents(g_instance);
    free(c->objs);
    free(c);
}

/* ---- results ------------------------------------------------------------------------- */

typedef struct Out { uint8_t **buf; uint32_t *len, *cap; bool failed; } Out;

static void put(Out *o, const void *p, uint32_t n) {
    if (o->failed) return;
    if (*o->len + n > *o->cap) {
        uint32_t cap = *o->cap ? *o->cap : 256;
        while (cap < *o->len + n) {
            if (cap > (256u << 20)) { o->failed = true; return; }
            cap *= 2;
        }
        uint8_t *b = (uint8_t *)realloc(*o->buf, cap);
        if (!b) { o->failed = true; return; }
        *o->buf = b;
        *o->cap = cap;
    }
    memcpy(*o->buf + *o->len, p, n);
    *o->len += n;
}
static void put_u8(Out *o, uint8_t v) { put(o, &v, 1); }
static void put_u32(Out *o, uint32_t v) { put(o, &v, 4); }
static void put_num(Out *o, double v) { put_u8(o, V_NUMBER); put(o, &v, 8); }
static void put_str(Out *o, const char *s, size_t n) { put_u8(o, V_STRING); put_u32(o, (uint32_t)n); put(o, s, (uint32_t)n); }
static void put_key(Out *o, const char *k) { put_u32(o, (uint32_t)strlen(k)); put(o, k, (uint32_t)strlen(k)); }

/* ---- the calls ---------------------------------------------------------------------- */

static char g_error[1024];          /* the last uncaptured error, for the log */

static void on_uncaptured(WGPUDevice const *d, WGPUErrorType type, WGPUStringView m, void *u1, void *u2) {
    (void)d; (void)u1; (void)u2;
    snprintf(g_error, sizeof(g_error), "%.*s", (int)(m.length > 900 ? 900 : m.length), m.data ? m.data : "");
    fprintf(stderr, "[webgpu] uncaptured error %d: %s\n", (int)type, g_error);
}

static volatile int g_got;
static WGPUAdapter g_adapter;
static WGPUDevice g_device;
static char g_why[256];

static void on_adapter(WGPURequestAdapterStatus s, WGPUAdapter a, WGPUStringView m, void *u1, void *u2) {
    (void)u1; (void)u2;
    g_adapter = s == WGPURequestAdapterStatus_Success ? a : NULL;
    if (!g_adapter) snprintf(g_why, sizeof(g_why), "%.*s", (int)(m.length > 200 ? 200 : m.length), m.data ? m.data : "");
    g_got = 1;
}

static void on_device(WGPURequestDeviceStatus s, WGPUDevice d, WGPUStringView m, void *u1, void *u2) {
    (void)u1; (void)u2;
    g_device = s == WGPURequestDeviceStatus_Success ? d : NULL;
    if (!g_device) snprintf(g_why, sizeof(g_why), "%.*s", (int)(m.length > 200 ? 200 : m.length), m.data ? m.data : "");
    g_got = 1;
}

static void on_map(WGPUMapAsyncStatus s, WGPUStringView m, void *u1, void *u2) {
    (void)m; (void)u2;
    *(int *)u1 = s == WGPUMapAsyncStatus_Success ? 1 : -1;
}

static void on_done(WGPUQueueWorkDoneStatus s, WGPUStringView m, void *u1, void *u2) {
    (void)s; (void)m; (void)u2;
    *(int *)u1 = 1;
}

static WGPUErrorType g_scope_type;
static void on_pop(WGPUPopErrorScopeStatus s, WGPUErrorType type, WGPUStringView m, void *u1, void *u2) {
    (void)s; (void)u2;
    g_scope_type = type;
    snprintf(g_error, sizeof(g_error), "%.*s", (int)(m.length > 900 ? 900 : m.length), m.data ? m.data : "");
    *(int *)u1 = 1;
}

/* Pop an error scope (waiting for wgpu's answer): its type, the message
 * in g_error. */
static WGPUErrorType pop_scope(Obj *dv) {
    int done = 0;
    g_scope_type = WGPUErrorType_NoError;
    WGPUPopErrorScopeCallbackInfo ci = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    ci.mode = WGPUCallbackMode_AllowProcessEvents;
    ci.callback = on_pop;
    ci.userdata1 = &done;
    w.DevicePopErrorScope(dv->h, ci);
    wait_for((volatile int *)&done);
    return done ? g_scope_type : WGPUErrorType_NoError;
}

/* The process's one adapter, asked for once.  wgpu-native gives each
 * request a handle to the same adapter, and the release of any of them
 * ends it for all (it then aborts on the others), so every id is the one
 * handle with a reference of its own. */
static WGPUAdapter g_shared_adapter;

/* G_REQUEST_ADAPTER: the GPU, if the block list lets it be used. */
static void request_adapter(WgpuClient *c, uint32_t id, int opts, Out *o) {
    typedef void (*AddRef)(WGPUAdapter);
    static AddRef add_ref;
    if (!add_ref) add_ref = (AddRef)(void *)GetProcAddress(g_dll, "wgpuAdapterAddRef");
    if (!add_ref) { put_u8(o, V_NULL); return; }
    WGPURequestAdapterOptions ro = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    char pref[32];
    const char *pp = string(get(opts, "powerPreference"), pref, sizeof(pref));
    if (pp && !strcmp(pp, "low-power")) ro.powerPreference = WGPUPowerPreference_LowPower;
    else if (pp && !strcmp(pp, "high-performance")) ro.powerPreference = WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterCallbackInfo ci = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    ci.mode = WGPUCallbackMode_AllowProcessEvents;
    ci.callback = on_adapter;
    if (!g_shared_adapter) {
        g_got = 0;
        g_adapter = NULL;
        w.InstanceRequestAdapter(g_instance, &ro, ci);
        wait_for(&g_got);
        g_shared_adapter = g_adapter;
    }
    g_adapter = g_shared_adapter;
    if (!g_adapter) { put_u8(o, V_NULL); return; }
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    w.AdapterGetInfo(g_adapter, &info);
    char desc[256], driver[128];
    snprintf(desc, sizeof(desc), "%.*s", (int)info.device.length, info.device.data ? info.device.data : "");
    snprintf(driver, sizeof(driver), "%.*s", (int)info.description.length, info.description.data ? info.description.data : "");
    GpuDevice d = { info.vendorID, info.deviceID, desc, driver };
    GpuVerdict v = gpu_blocklist_check(&d);
    fprintf(stderr, "[webgpu] WebGPU on %s (%04x:%04x, backend %d)%s%s\n", desc, info.vendorID, info.deviceID,
            (int)info.backendType, v.blocked ? ": blocked, " : "", v.blocked ? v.why : "");
    if (v.blocked) {
        w.AdapterInfoFreeMembers(info);
        c->blocked = true;
        put_u8(o, V_NULL);
        return;
    }
    c->denied = v.denied;
    add_ref(g_adapter);
    put_obj(c, id, O_ADAPTER, g_adapter, 0);
    /* What the page learns: vendor and architecture, as browsers give them;
     * the features it may ask for. */
    put_u8(o, V_OBJECT);
    put_u32(o, 3);
    put_key(o, "vendor");
    put_str(o, info.vendor.data ? info.vendor.data : "", info.vendor.length);
    put_key(o, "architecture");
    put_str(o, info.architecture.data ? info.architecture.data : "", info.architecture.length);
    put_key(o, "features");
    uint32_t nf = 0;
    for (const Name *f = FEATURES; f->s; f++) if (w.AdapterHasFeature(g_adapter, (WGPUFeatureName)f->v)) nf++;
    put_u8(o, V_ARRAY);
    put_u32(o, nf);
    for (const Name *f = FEATURES; f->s; f++)
        if (w.AdapterHasFeature(g_adapter, (WGPUFeatureName)f->v)) put_str(o, f->s, strlen(f->s));
    w.AdapterInfoFreeMembers(info);
}

/* The limits a device has, as WebGPU names them. */
static void put_limits(Out *o, const WGPULimits *l) {
#define LIMIT(n) { #n, (double)l->n }
    struct { const char *k; double v; } t[] = {
        LIMIT(maxTextureDimension1D), LIMIT(maxTextureDimension2D), LIMIT(maxTextureDimension3D),
        LIMIT(maxTextureArrayLayers), LIMIT(maxBindGroups), LIMIT(maxBindingsPerBindGroup),
        LIMIT(maxDynamicUniformBuffersPerPipelineLayout), LIMIT(maxDynamicStorageBuffersPerPipelineLayout),
        LIMIT(maxSampledTexturesPerShaderStage), LIMIT(maxSamplersPerShaderStage),
        LIMIT(maxStorageBuffersPerShaderStage), LIMIT(maxStorageTexturesPerShaderStage),
        LIMIT(maxUniformBuffersPerShaderStage), LIMIT(maxUniformBufferBindingSize), LIMIT(maxStorageBufferBindingSize),
        LIMIT(minUniformBufferOffsetAlignment), LIMIT(minStorageBufferOffsetAlignment), LIMIT(maxVertexBuffers),
        LIMIT(maxBufferSize), LIMIT(maxVertexAttributes), LIMIT(maxVertexBufferArrayStride),
        LIMIT(maxInterStageShaderVariables), LIMIT(maxColorAttachments), LIMIT(maxColorAttachmentBytesPerSample),
        LIMIT(maxComputeWorkgroupStorageSize), LIMIT(maxComputeInvocationsPerWorkgroup),
        LIMIT(maxComputeWorkgroupSizeX), LIMIT(maxComputeWorkgroupSizeY), LIMIT(maxComputeWorkgroupSizeZ),
        LIMIT(maxComputeWorkgroupsPerDimension) };
#undef LIMIT
    put_u8(o, V_OBJECT);
    put_u32(o, (uint32_t)(sizeof(t) / sizeof(t[0])));
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) { put_key(o, t[i].k); put_num(o, t[i].v); }
}

/* G_REQUEST_DEVICE: the features asked for (each the adapter has, none
 * the block list took away), the default limits. */
static void request_device(WgpuClient *c, uint32_t id, uint32_t queue_id, uint32_t adapter_id, int desc, Out *o) {
    Obj *a = obj(c, adapter_id, O_ADAPTER);
    WGPUFeatureName feats[16];
    size_t nf = 0;
    int rf = get(desc, "requiredFeatures");
    for (uint32_t k = 0; a && k < count(rf) && nf < 16; k++) {
        uint32_t f = lookup(FEATURES, item(rf, k), 0xffffffffu);
        if (f == 0xffffffffu || !w.AdapterHasFeature(a->h, (WGPUFeatureName)f) ||
            (c->denied & GPU_FEATURE_EXTENSIONS)) {
            put_u8(o, V_NULL);
            return;
        }
        feats[nf++] = (WGPUFeatureName)f;
    }
    if (!a) { put_u8(o, V_NULL); return; }
    WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
    dd.requiredFeatureCount = nf;
    dd.requiredFeatures = feats;
    dd.uncapturedErrorCallbackInfo.callback = on_uncaptured;
    WGPURequestDeviceCallbackInfo ci = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    ci.mode = WGPUCallbackMode_AllowProcessEvents;
    ci.callback = on_device;
    g_got = 0;
    g_device = NULL;
    w.AdapterRequestDevice(a->h, &dd, ci);
    wait_for(&g_got);
    if (!g_device) fprintf(stderr, "[webgpu] no device: %s\n", g_got ? g_why : "no answer");
    if (!g_device || !put_obj(c, id, O_DEVICE, g_device, id)) { put_u8(o, V_NULL); return; }
    put_obj(c, queue_id, O_QUEUE, w.DeviceGetQueue(g_device), id);
    WGPULimits l = WGPU_LIMITS_INIT;
    w.DeviceGetLimits(g_device, &l);
    put_limits(o, &l);
}

static void buffer_map(WgpuClient *c, const uint8_t *p, uint32_t n, Out *o) {
    if (n < 24) { o->failed = true; return; }
    uint32_t id, mode;
    double off, size;
    memcpy(&id, p, 4);
    memcpy(&mode, p + 4, 4);
    memcpy(&off, p + 8, 8);
    memcpy(&size, p + 16, 8);
    Obj *b = obj(c, id, O_BUFFER);
    if (!b || off < 0 || size < 0 || off > 9e15 || size > (256 << 20) || b->res->mapped || b->res->destroyed ||
        (uint64_t)off % 8 || (uint64_t)size % 4 || (uint64_t)off + (uint64_t)size > b->size) {
        put_u32(o, 0);
        return;
    }
    int done = 0;
    WGPUBufferMapCallbackInfo ci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    ci.mode = WGPUCallbackMode_AllowProcessEvents;
    ci.callback = on_map;
    ci.userdata1 = &done;
    w.BufferMapAsync(b->h, mode == 1 ? WGPUMapMode_Read : WGPUMapMode_Write, (size_t)off, (size_t)size, ci);
    wait_for((volatile int *)&done);
    if (done != 1) { put_u32(o, 0); return; }
    b->res->mapped = true;
    b->map_off = (uint64_t)off;
    b->map_size = (uint64_t)size;
    put_u32(o, 1);
    if (mode == 1) {
        const void *m = w.BufferGetConstMappedRange(b->h, (size_t)off, (size_t)size);
        put_u32(o, m ? (uint32_t)size : 0);
        if (m) put(o, m, (uint32_t)size);
    }
}

static void buffer_unmap(WgpuClient *c, const uint8_t *p, uint32_t n) {
    if (n < 12) return;
    uint32_t id;
    double off;
    memcpy(&id, p, 4);
    memcpy(&off, p + 4, 8);
    Obj *b = obj(c, id, O_BUFFER);
    if (!b || !b->res->mapped) return;
    uint32_t len = n - 12;
    if (len && off >= 0 && off < 9e15 && (uint64_t)off % 8 == 0 && len % 4 == 0 && (uint64_t)off >= b->map_off &&
        (uint64_t)off + len <= b->map_off + b->map_size) {
        void *m = w.BufferGetMappedRange(b->h, (size_t)off, len);
        if (m) memcpy(m, p + 12, len);
    }
    w.BufferUnmap(b->h);
    b->res->mapped = false;
}

static WGPUExtent3D extent(int e) {
    WGPUExtent3D x = { 1, 1, 1 };
    if (nd(e) && nd(e)->tag == V_ARRAY) {
        x.width = u32(item(e, 0), 1);
        x.height = u32(item(e, 1), 1);
        x.depthOrArrayLayers = u32(item(e, 2), 1);
    } else {
        x.width = u32(get(e, "width"), 1);
        x.height = u32(get(e, "height"), 1);
        x.depthOrArrayLayers = u32(get(e, "depthOrArrayLayers"), 1);
    }
    return x;
}

static WGPUOrigin3D origin(int e) {
    WGPUOrigin3D x = { 0, 0, 0 };
    if (nd(e) && nd(e)->tag == V_ARRAY) {
        x.x = u32(item(e, 0), 0); x.y = u32(item(e, 1), 0); x.z = u32(item(e, 2), 0);
    } else {
        x.x = u32(get(e, "x"), 0); x.y = u32(get(e, "y"), 0); x.z = u32(get(e, "z"), 0);
    }
    return x;
}

static WGPUTexelCopyTextureInfo copy_texture(WgpuClient *c, int t) {
    WGPUTexelCopyTextureInfo x = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    x.texture = handle(c, get(t, "texture"), O_TEXTURE);
    x.mipLevel = u32(get(t, "mipLevel"), 0);
    x.origin = origin(get(t, "origin"));
    x.aspect = (WGPUTextureAspect)lookup(ASPECT, get(t, "aspect"), WGPUTextureAspect_All);
    return x;
}

static WGPUTexelCopyBufferLayout copy_layout(int l) {
    WGPUTexelCopyBufferLayout x = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    x.offset = u64(get(l, "offset"), 0);
    x.bytesPerRow = u32(get(l, "bytesPerRow"), WGPU_COPY_STRIDE_UNDEFINED);
    x.rowsPerImage = u32(get(l, "rowsPerImage"), WGPU_COPY_STRIDE_UNDEFINED);
    return x;
}

static WGPUBlendComponent blend_component(int b) {
    WGPUBlendComponent x = WGPU_BLEND_COMPONENT_INIT;
    x.operation = (WGPUBlendOperation)lookup(BLEND_OP, get(b, "operation"), WGPUBlendOperation_Add);
    x.srcFactor = (WGPUBlendFactor)lookup(BLEND_FACTOR, get(b, "srcFactor"), WGPUBlendFactor_One);
    x.dstFactor = (WGPUBlendFactor)lookup(BLEND_FACTOR, get(b, "dstFactor"), WGPUBlendFactor_Zero);
    return x;
}

static WGPUStencilFaceState stencil_face(int s) {
    WGPUStencilFaceState x = WGPU_STENCIL_FACE_STATE_INIT;
    x.compare = (WGPUCompareFunction)lookup(COMPARE, get(s, "compare"), WGPUCompareFunction_Always);
    x.failOp = (WGPUStencilOperation)lookup(STENCIL_OP, get(s, "failOp"), WGPUStencilOperation_Keep);
    x.depthFailOp = (WGPUStencilOperation)lookup(STENCIL_OP, get(s, "depthFailOp"), WGPUStencilOperation_Keep);
    x.passOp = (WGPUStencilOperation)lookup(STENCIL_OP, get(s, "passOp"), WGPUStencilOperation_Keep);
    return x;
}

/* Pipeline-overridable constants: { name: number }. */
static size_t constants(int o, WGPUConstantEntry *out, char (*names)[64], size_t cap) {
    const Node *n = nd(o);
    size_t k = 0;
    if (!n || n->tag != V_OBJECT) return 0;
    for (int ch = n->first; ch >= 0 && k < cap; ch = g_tree.nodes[ch].next) {
        const Node *e = &g_tree.nodes[ch];
        if (e->key_len >= 64) continue;
        memcpy(names[k], e->key, e->key_len);
        names[k][e->key_len] = 0;
        out[k] = (WGPUConstantEntry)WGPU_CONSTANT_ENTRY_INIT;
        out[k].key = sv(names[k]);
        out[k].value = num(ch, 0);
        k++;
    }
    return k;
}

static void *render_pipeline(WgpuClient *c, Obj *dev, int d) {
    WGPURenderPipelineDescriptor rp = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    char auto_buf[8], ve[64], fe[64];
    rp.layout = handle(c, get(d, "layout"), O_PL);       /* null ("auto"): wgpu derives it */
    (void)auto_buf;
    int v = get(d, "vertex");
    rp.vertex.module = need(c, get(v, "module"), O_MODULE);
    const char *vep = string(get(v, "entryPoint"), ve, sizeof(ve));
    rp.vertex.entryPoint = vep ? sv(vep) : (WGPUStringView)WGPU_STRING_VIEW_INIT;
    static WGPUConstantEntry vconst[32], fconst[32];
    static char vnames[32][64], fnames[32][64];
    rp.vertex.constantCount = constants(get(v, "constants"), vconst, vnames, 32);
    rp.vertex.constants = vconst;
    static WGPUVertexBufferLayout bufs[16];
    static WGPUVertexAttribute attrs[16][32];
    int vb = get(v, "buffers");
    rp.vertex.bufferCount = count(vb) > 16 ? 16 : count(vb);
    for (uint32_t k = 0; k < rp.vertex.bufferCount; k++) {
        int b = item(vb, k);
        bufs[k] = (WGPUVertexBufferLayout)WGPU_VERTEX_BUFFER_LAYOUT_INIT;
        if (nd(b)->tag == V_NULL) { bufs[k].stepMode = WGPUVertexStepMode_Undefined; continue; }
        bufs[k].arrayStride = u64(get(b, "arrayStride"), 0);
        bufs[k].stepMode = (WGPUVertexStepMode)lookup(STEP_MODE, get(b, "stepMode"), WGPUVertexStepMode_Vertex);
        int as = get(b, "attributes");
        bufs[k].attributeCount = count(as) > 32 ? 32 : count(as);
        for (uint32_t j = 0; j < bufs[k].attributeCount; j++) {
            int a = item(as, j);
            attrs[k][j] = (WGPUVertexAttribute)WGPU_VERTEX_ATTRIBUTE_INIT;
            attrs[k][j].format = (WGPUVertexFormat)required(VERTEX_FORMATS, get(a, "format"));
            attrs[k][j].offset = u64(get(a, "offset"), 0);
            attrs[k][j].shaderLocation = u32(get(a, "shaderLocation"), 0);
        }
        bufs[k].attributes = attrs[k];
    }
    rp.vertex.buffers = bufs;
    int pr = get(d, "primitive");
    rp.primitive.topology = (WGPUPrimitiveTopology)lookup(TOPOLOGY, get(pr, "topology"), WGPUPrimitiveTopology_TriangleList);
    rp.primitive.stripIndexFormat = (WGPUIndexFormat)lookup(INDEX_FORMAT, get(pr, "stripIndexFormat"), WGPUIndexFormat_Undefined);
    rp.primitive.frontFace = (WGPUFrontFace)lookup(FRONT_FACE, get(pr, "frontFace"), WGPUFrontFace_CCW);
    rp.primitive.cullMode = (WGPUCullMode)lookup(CULL, get(pr, "cullMode"), WGPUCullMode_None);
    rp.primitive.unclippedDepth = flag(get(pr, "unclippedDepth"), false);
    static WGPUDepthStencilState ds;
    int dsn = get(d, "depthStencil");
    if (nd(dsn) && nd(dsn)->tag == V_OBJECT) {
        ds = (WGPUDepthStencilState)WGPU_DEPTH_STENCIL_STATE_INIT;
        ds.format = (WGPUTextureFormat)required(FORMATS, get(dsn, "format"));
        int dwe = get(dsn, "depthWriteEnabled");
        ds.depthWriteEnabled = !nd(dwe) ? WGPUOptionalBool_Undefined : flag(dwe, false) ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        ds.depthCompare = (WGPUCompareFunction)lookup(COMPARE, get(dsn, "depthCompare"), WGPUCompareFunction_Undefined);
        ds.stencilFront = stencil_face(get(dsn, "stencilFront"));
        ds.stencilBack = stencil_face(get(dsn, "stencilBack"));
        ds.stencilReadMask = u32(get(dsn, "stencilReadMask"), 0xffffffffu);
        ds.stencilWriteMask = u32(get(dsn, "stencilWriteMask"), 0xffffffffu);
        ds.depthBias = (int32_t)num(get(dsn, "depthBias"), 0);
        ds.depthBiasSlopeScale = (float)num(get(dsn, "depthBiasSlopeScale"), 0);
        ds.depthBiasClamp = (float)num(get(dsn, "depthBiasClamp"), 0);
        rp.depthStencil = &ds;
    }
    int ms = get(d, "multisample");
    rp.multisample.count = u32(get(ms, "count"), 1);
    rp.multisample.mask = u32(get(ms, "mask"), 0xffffffffu);
    rp.multisample.alphaToCoverageEnabled = flag(get(ms, "alphaToCoverageEnabled"), false);
    static WGPUFragmentState fs;
    static WGPUColorTargetState targets[8];
    static WGPUBlendState blends[8];
    int f = get(d, "fragment");
    if (nd(f) && nd(f)->tag == V_OBJECT) {
        fs = (WGPUFragmentState)WGPU_FRAGMENT_STATE_INIT;
        fs.module = need(c, get(f, "module"), O_MODULE);
        const char *fep = string(get(f, "entryPoint"), fe, sizeof(fe));
        fs.entryPoint = fep ? sv(fep) : (WGPUStringView)WGPU_STRING_VIEW_INIT;
        fs.constantCount = constants(get(f, "constants"), fconst, fnames, 32);
        fs.constants = fconst;
        int ts = get(f, "targets");
        fs.targetCount = count(ts) > 8 ? 8 : count(ts);
        for (uint32_t k = 0; k < fs.targetCount; k++) {
            int t = item(ts, k);
            targets[k] = (WGPUColorTargetState)WGPU_COLOR_TARGET_STATE_INIT;
            if (nd(t)->tag == V_NULL) { targets[k].format = WGPUTextureFormat_Undefined; continue; }
            targets[k].format = (WGPUTextureFormat)required(FORMATS, get(t, "format"));
            targets[k].writeMask = u32(get(t, "writeMask"), WGPUColorWriteMask_All);
            int bl = get(t, "blend");
            if (nd(bl) && nd(bl)->tag == V_OBJECT) {
                blends[k].color = blend_component(get(bl, "color"));
                blends[k].alpha = blend_component(get(bl, "alpha"));
                targets[k].blend = &blends[k];
            }
        }
        fs.targets = targets;
        rp.fragment = &fs;
    }
    if (g_bad) return NULL;
    return w.DeviceCreateRenderPipeline(dev->h, &rp);
}

static void *bind_group_layout(WgpuClient *c, Obj *dev, int d) {
    static WGPUBindGroupLayoutEntry es[64];
    int en = get(d, "entries");
    WGPUBindGroupLayoutDescriptor bd = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bd.entryCount = count(en) > 64 ? 64 : count(en);
    for (uint32_t k = 0; k < bd.entryCount; k++) {
        int e = item(en, k);
        es[k] = (WGPUBindGroupLayoutEntry)WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        es[k].binding = u32(get(e, "binding"), 0);
        es[k].visibility = u32(get(e, "visibility"), 0);
        int b = get(e, "buffer"), s = get(e, "sampler"), t = get(e, "texture"), st = get(e, "storageTexture");
        if (nd(b)) {
            es[k].buffer.type = (WGPUBufferBindingType)lookup(BUFFER_BINDING, get(b, "type"), WGPUBufferBindingType_Uniform);
            es[k].buffer.hasDynamicOffset = flag(get(b, "hasDynamicOffset"), false);
            es[k].buffer.minBindingSize = u64(get(b, "minBindingSize"), 0);
        }
        if (nd(s)) es[k].sampler.type = (WGPUSamplerBindingType)lookup(SAMPLER_BINDING, get(s, "type"), WGPUSamplerBindingType_Filtering);
        if (nd(t)) {
            es[k].texture.sampleType = (WGPUTextureSampleType)lookup(SAMPLE_TYPE, get(t, "sampleType"), WGPUTextureSampleType_Float);
            es[k].texture.viewDimension = (WGPUTextureViewDimension)lookup(VIEW_DIMENSION, get(t, "viewDimension"), WGPUTextureViewDimension_2D);
            es[k].texture.multisampled = flag(get(t, "multisampled"), false);
        }
        if (nd(st)) {
            if (c->denied & GPU_FEATURE_STORAGE_TEXTURES) return NULL;
            es[k].storageTexture.access = (WGPUStorageTextureAccess)lookup(STORAGE_ACCESS, get(st, "access"), WGPUStorageTextureAccess_WriteOnly);
            es[k].storageTexture.format = (WGPUTextureFormat)required(FORMATS, get(st, "format"));
            es[k].storageTexture.viewDimension = (WGPUTextureViewDimension)lookup(VIEW_DIMENSION, get(st, "viewDimension"), WGPUTextureViewDimension_2D);
        }
    }
    bd.entries = es;
    if (g_bad) return NULL;
    return w.DeviceCreateBindGroupLayout(dev->h, &bd);
}

static void *bind_group(WgpuClient *c, Obj *dev, int d) {
    static WGPUBindGroupEntry es[64];
    int en = get(d, "entries");
    WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bd.layout = handle(c, get(d, "layout"), O_BGL);
    if (!bd.layout) return NULL;
    bd.entryCount = count(en) > 64 ? 64 : count(en);
    for (uint32_t k = 0; k < bd.entryCount; k++) {
        int e = item(en, k), r = get(e, "resource");
        es[k] = (WGPUBindGroupEntry)WGPU_BIND_GROUP_ENTRY_INIT;
        es[k].binding = u32(get(e, "binding"), 0);
        if (nd(r) && nd(r)->tag == V_OBJECT) {         /* { buffer, offset, size } */
            es[k].buffer = need(c, get(r, "buffer"), O_BUFFER);
            es[k].offset = u64(get(r, "offset"), 0);
            es[k].size = u64(get(r, "size"), WGPU_WHOLE_SIZE);
        } else {
            es[k].sampler = handle(c, r, O_SAMPLER);
            es[k].textureView = handle(c, r, O_VIEW);
            if (!es[k].sampler && !es[k].textureView) g_bad = "a bind group entry names nothing";
        }
    }
    bd.entries = es;
    if (g_bad) return NULL;
    return w.DeviceCreateBindGroup(dev->h, &bd);
}

static void *render_pass(WgpuClient *c, WGPUCommandEncoder enc, int d) {
    static WGPURenderPassColorAttachment cas[8];
    static WGPURenderPassDepthStencilAttachment dsa;
    WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    int ca = get(d, "colorAttachments");
    rp.colorAttachmentCount = count(ca) > 8 ? 8 : count(ca);
    for (uint32_t k = 0; k < rp.colorAttachmentCount; k++) {
        int a = item(ca, k);
        cas[k] = (WGPURenderPassColorAttachment)WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        if (nd(a)->tag == V_NULL) continue;
        cas[k].view = need(c, get(a, "view"), O_VIEW);
        cas[k].resolveTarget = handle(c, get(a, "resolveTarget"), O_VIEW);
        cas[k].loadOp = (WGPULoadOp)required(LOAD_OP, get(a, "loadOp"));
        cas[k].storeOp = (WGPUStoreOp)required(STORE_OP, get(a, "storeOp"));
        int cv = get(a, "clearValue");
        if (nd(cv) && nd(cv)->tag == V_ARRAY) {
            cas[k].clearValue = (WGPUColor){ num(item(cv, 0), 0), num(item(cv, 1), 0), num(item(cv, 2), 0), num(item(cv, 3), 0) };
        } else if (nd(cv)) {
            cas[k].clearValue = (WGPUColor){ num(get(cv, "r"), 0), num(get(cv, "g"), 0), num(get(cv, "b"), 0), num(get(cv, "a"), 0) };
        }
    }
    rp.colorAttachments = cas;
    int ds = get(d, "depthStencilAttachment");
    if (nd(ds) && nd(ds)->tag == V_OBJECT) {
        dsa = (WGPURenderPassDepthStencilAttachment)WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        dsa.view = need(c, get(ds, "view"), O_VIEW);
        dsa.depthLoadOp = (WGPULoadOp)lookup(LOAD_OP, get(ds, "depthLoadOp"), WGPULoadOp_Undefined);
        dsa.depthStoreOp = (WGPUStoreOp)lookup(STORE_OP, get(ds, "depthStoreOp"), WGPUStoreOp_Undefined);
        dsa.depthClearValue = (float)num(get(ds, "depthClearValue"), 0);
        dsa.depthReadOnly = flag(get(ds, "depthReadOnly"), false);
        dsa.stencilLoadOp = (WGPULoadOp)lookup(LOAD_OP, get(ds, "stencilLoadOp"), WGPULoadOp_Undefined);
        dsa.stencilStoreOp = (WGPUStoreOp)lookup(STORE_OP, get(ds, "stencilStoreOp"), WGPUStoreOp_Undefined);
        dsa.stencilClearValue = u32(get(ds, "stencilClearValue"), 0);
        dsa.stencilReadOnly = flag(get(ds, "stencilReadOnly"), false);
        rp.depthStencilAttachment = &dsa;
    }
    if (g_bad) return NULL;
    return w.CommandEncoderBeginRenderPass(enc, &rp);
}

/* G_PASS: [command, args...] on a render or compute pass. */
static void pass_command(WgpuClient *c, uint32_t pass, int cmd) {
    char name[32];
    const char *s = string(item(cmd, 0), name, sizeof(name));
    if (!s) return;
    g_bad = NULL;
    Obj *rp = obj(c, pass, O_RPASS), *cp = obj(c, pass, O_CPASS);
    if ((rp && rp->ended) || (cp && cp->ended)) return;
    g_holder = obj(c, rp ? rp->parent : cp ? cp->parent : 0, O_ENCODER);
    if (rp) {
        WGPURenderPassEncoder e = rp->h;
        if (!strcmp(s, "setPipeline")) {
            WGPURenderPipeline p = handle(c, item(cmd, 1), O_RPIPE);
            if (p) w.RenderPassEncoderSetPipeline(e, p);
        } else if (!strcmp(s, "setBindGroup")) {
            uint32_t offs[16], n = 0;
            int a = item(cmd, 3);
            for (; n < count(a) && n < 16; n++) offs[n] = u32(item(a, n), 0);
            w.RenderPassEncoderSetBindGroup(e, u32(item(cmd, 1), 0), handle(c, item(cmd, 2), O_BG), n, offs);
        } else if (!strcmp(s, "setVertexBuffer")) {
            w.RenderPassEncoderSetVertexBuffer(e, u32(item(cmd, 1), 0), handle(c, item(cmd, 2), O_BUFFER),
                                               u64(item(cmd, 3), 0), u64(item(cmd, 4), WGPU_WHOLE_SIZE));
        } else if (!strcmp(s, "setIndexBuffer")) {
            WGPUBuffer b = handle(c, item(cmd, 1), O_BUFFER);
            WGPUIndexFormat f = (WGPUIndexFormat)required(INDEX_FORMAT, item(cmd, 2));
            if (b && !g_bad) w.RenderPassEncoderSetIndexBuffer(e, b, f, u64(item(cmd, 3), 0), u64(item(cmd, 4), WGPU_WHOLE_SIZE));
        } else if (!strcmp(s, "draw")) {
            w.RenderPassEncoderDraw(e, u32(item(cmd, 1), 0), u32(item(cmd, 2), 1), u32(item(cmd, 3), 0), u32(item(cmd, 4), 0));
        } else if (!strcmp(s, "drawIndexed")) {
            w.RenderPassEncoderDrawIndexed(e, u32(item(cmd, 1), 0), u32(item(cmd, 2), 1), u32(item(cmd, 3), 0),
                                           (int32_t)num(item(cmd, 4), 0), u32(item(cmd, 5), 0));
        } else if (!strcmp(s, "drawIndirect") || !strcmp(s, "drawIndexedIndirect")) {
            WGPUBuffer b = handle(c, item(cmd, 1), O_BUFFER);
            if (b && s[4] == 'I' && s[5] == 'n' && s[6] == 'd' && s[7] == 'e')
                w.RenderPassEncoderDrawIndexedIndirect(e, b, u64(item(cmd, 2), 0));
            else if (b) w.RenderPassEncoderDrawIndirect(e, b, u64(item(cmd, 2), 0));
        } else if (!strcmp(s, "setViewport")) {
            w.RenderPassEncoderSetViewport(e, (float)num(item(cmd, 1), 0), (float)num(item(cmd, 2), 0),
                                           (float)num(item(cmd, 3), 0), (float)num(item(cmd, 4), 0),
                                           (float)num(item(cmd, 5), 0), (float)num(item(cmd, 6), 1));
        } else if (!strcmp(s, "setScissorRect")) {
            w.RenderPassEncoderSetScissorRect(e, u32(item(cmd, 1), 0), u32(item(cmd, 2), 0), u32(item(cmd, 3), 0),
                                              u32(item(cmd, 4), 0));
        } else if (!strcmp(s, "setBlendConstant")) {
            WGPUColor col = { num(item(cmd, 1), 0), num(item(cmd, 2), 0), num(item(cmd, 3), 0), num(item(cmd, 4), 0) };
            w.RenderPassEncoderSetBlendConstant(e, &col);
        } else if (!strcmp(s, "setStencilReference")) {
            w.RenderPassEncoderSetStencilReference(e, u32(item(cmd, 1), 0));
        }
    } else if (cp) {
        WGPUComputePassEncoder e = cp->h;
        if (!strcmp(s, "setPipeline")) {
            WGPUComputePipeline p = handle(c, item(cmd, 1), O_CPIPE);
            if (p) w.ComputePassEncoderSetPipeline(e, p);
        } else if (!strcmp(s, "setBindGroup")) {
            uint32_t offs[16], n = 0;
            int a = item(cmd, 3);
            for (; n < count(a) && n < 16; n++) offs[n] = u32(item(a, n), 0);
            w.ComputePassEncoderSetBindGroup(e, u32(item(cmd, 1), 0), handle(c, item(cmd, 2), O_BG), n, offs);
        } else if (!strcmp(s, "dispatchWorkgroups")) {
            w.ComputePassEncoderDispatchWorkgroups(e, u32(item(cmd, 1), 1), u32(item(cmd, 2), 1), u32(item(cmd, 3), 1));
        } else if (!strcmp(s, "dispatchWorkgroupsIndirect")) {
            WGPUBuffer b = handle(c, item(cmd, 1), O_BUFFER);
            if (b) w.ComputePassEncoderDispatchWorkgroupsIndirect(e, b, u64(item(cmd, 2), 0));
        }
    }
    g_holder = NULL;
}

/* G_ENCODE: [command, args...] on an encoder. */
static void encode_command(WgpuClient *c, WGPUCommandEncoder e, int cmd) {
    char name[32];
    const char *s = string(item(cmd, 0), name, sizeof(name));
    if (!s) return;
    g_bad = NULL;
    if (!strcmp(s, "copyBufferToBuffer")) {
        WGPUBuffer a = handle(c, item(cmd, 1), O_BUFFER), b = handle(c, item(cmd, 3), O_BUFFER);
        if (a && b) w.CommandEncoderCopyBufferToBuffer(e, a, u64(item(cmd, 2), 0), b, u64(item(cmd, 4), 0), u64(item(cmd, 5), 0));
    } else if (!strcmp(s, "copyBufferToTexture")) {
        int src = item(cmd, 1);
        WGPUTexelCopyBufferInfo b = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
        b.buffer = handle(c, get(src, "buffer"), O_BUFFER);
        b.layout = copy_layout(src);
        WGPUTexelCopyTextureInfo t = copy_texture(c, item(cmd, 2));
        WGPUExtent3D x = extent(item(cmd, 3));
        if (b.buffer && t.texture && !g_bad) w.CommandEncoderCopyBufferToTexture(e, &b, &t, &x);
    } else if (!strcmp(s, "copyTextureToBuffer")) {
        WGPUTexelCopyTextureInfo t = copy_texture(c, item(cmd, 1));
        int dst = item(cmd, 2);
        WGPUTexelCopyBufferInfo b = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
        b.buffer = handle(c, get(dst, "buffer"), O_BUFFER);
        b.layout = copy_layout(dst);
        WGPUExtent3D x = extent(item(cmd, 3));
        if (b.buffer && t.texture && !g_bad) w.CommandEncoderCopyTextureToBuffer(e, &t, &b, &x);
    } else if (!strcmp(s, "copyTextureToTexture")) {
        WGPUTexelCopyTextureInfo a = copy_texture(c, item(cmd, 1)), b = copy_texture(c, item(cmd, 2));
        WGPUExtent3D x = extent(item(cmd, 3));
        if (a.texture && b.texture && !g_bad) w.CommandEncoderCopyTextureToTexture(e, &a, &b, &x);
    } else if (!strcmp(s, "clearBuffer")) {
        WGPUBuffer b = handle(c, item(cmd, 1), O_BUFFER);
        if (b) w.CommandEncoderClearBuffer(e, b, u64(item(cmd, 2), 0), u64(item(cmd, 3), WGPU_WHOLE_SIZE));
    }
}

static Canvas *canvas(WgpuClient *c, uint32_t id, bool make) {
    for (int i = 0; i < MAX_CANVASES; i++)
        if (c->canvases[i].id == id && id) return &c->canvases[i];
    if (!make) return NULL;
    for (int i = 0; i < MAX_CANVASES; i++)
        if (!c->canvases[i].id) { memset(&c->canvases[i], 0, sizeof(Canvas)); c->canvases[i].id = id; return &c->canvases[i]; }
    return NULL;
}

/* A canvas's texture at its size (G_CONFIGURE, and a resize). */
static void canvas_texture(WgpuClient *c, Canvas *cv) {
    if (cv->texture) { w.TextureRelease(cv->texture); cv->texture = NULL; }
    Obj *dev = obj(c, cv->device, O_DEVICE);
    if (!dev || !cv->width || !cv->height || cv->width > 8192 || cv->height > 8192) return;
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst |
               WGPUTextureUsage_TextureBinding;
    td.size = (WGPUExtent3D){ cv->width, cv->height, 1 };
    td.format = cv->format;
    cv->texture = w.DeviceCreateTexture(dev->h, &td);
}

static bool run_one(WgpuClient *c, uint32_t op, const uint8_t *p, uint32_t n, Out *o) {
    g_bad = NULL;
    uint32_t a = 0, b = 0, d = 0;
    if (n >= 4) memcpy(&a, p, 4);
    if (n >= 8) memcpy(&b, p + 4, 4);
    if (n >= 12) memcpy(&d, p + 8, 4);
    /* Ops with a tree after k u32s. */
    int k = 0;
    switch (op) {
    case G_REQUEST_ADAPTER: k = 1; break;
    case G_REQUEST_DEVICE: k = 3; break;
    case G_CREATE_BUFFER: case G_CREATE_TEXTURE: case G_CREATE_VIEW: case G_CREATE_SAMPLER: case G_CREATE_SHADER_MODULE:
    case G_CREATE_BIND_GROUP_LAYOUT: case G_CREATE_PIPELINE_LAYOUT: case G_CREATE_BIND_GROUP:
    case G_CREATE_RENDER_PIPELINE: case G_CREATE_COMPUTE_PIPELINE: case G_BEGIN_RENDER_PASS: case G_CONFIGURE: k = 2; break;
    case G_PASS: case G_ENCODE: case G_SUBMIT: case G_PUSH_ERROR_SCOPE: k = 1; break;
    case G_WRITE_TEXTURE: k = 1; break;
    default: k = -1; break;
    }
    int t = -1;
    if (k >= 0) {
        if (n < (uint32_t)k * 4 + 1) return false;
        const uint8_t *q = p + k * 4, *end = p + n;
        g_tree.n = 0;
        t = parse(&g_tree, &q, end, 0);
        if (t < 0) return false;
        if (op == G_WRITE_TEXTURE) {            /* the tree, then the bytes */
            const Node *bytes = NULL;
            int bi = -1;
            if (q < end) { bi = parse(&g_tree, &q, end, 0); bytes = nd(bi); }
            if (!bytes || bytes->tag != V_BYTES) return false;
            Obj *qu = obj(c, a, O_QUEUE);
            if (!qu) return true;
            WGPUTexelCopyTextureInfo dst = copy_texture(c, get(t, "destination"));
            WGPUTexelCopyBufferLayout lay = copy_layout(get(t, "layout"));
            WGPUExtent3D x = extent(get(t, "size"));
            if (dst.texture && !g_bad) w.QueueWriteTexture(qu->h, &dst, bytes->data, bytes->n, &lay, &x);
            else if (g_bad) c->error = g_bad;
            return true;
        }
    }
    Obj *dev = (op >= G_CREATE_BUFFER && op <= G_CREATE_COMMAND_ENCODER && op != G_BUFFER_MAP && op != G_BUFFER_UNMAP &&
                op != G_DESTROY && op != G_CREATE_VIEW && op != G_COMPILATION_INFO && op != G_GET_BIND_GROUP_LAYOUT)
               ? obj(c, b, O_DEVICE) : NULL;
    switch (op) {
    case G_REQUEST_ADAPTER:
        if (c->blocked) { put_u8(o, V_NULL); return true; }
        request_adapter(c, a, t, o);
        return true;
    case G_REQUEST_DEVICE: request_device(c, a, b, d, t, o); return true;
    case G_CREATE_BUFFER: {
        if (!dev) return true;
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.size = u64(get(t, "size"), 0);
        bd.usage = u32(get(t, "usage"), 0);
        bd.mappedAtCreation = flag(get(t, "mappedAtCreation"), false);
        if (bd.mappedAtCreation && bd.size % 4) { g_bad = "mappedAtCreation needs a size that is a multiple of 4"; return true; }
        /* In an error scope of our own: an invalid buffer is never kept
         * (wgpu-native aborts on reading the mapping of one). */
        w.DevicePushErrorScope(dev->h, WGPUErrorFilter_Validation);
        WGPUBuffer buf = w.DeviceCreateBuffer(dev->h, &bd);
        if (pop_scope(dev) != WGPUErrorType_NoError) {
            if (buf) w.BufferRelease(buf);
            refuse(c, g_error);
            return true;
        }
        if (!put_obj(c, a, O_BUFFER, buf, b)) return true;
        c->objs[a].size = bd.size;
        if (bd.mappedAtCreation) {
            c->objs[a].res->mapped = true;
            c->objs[a].map_size = bd.size;
        }
        return true;
    }
    case G_BUFFER_MAP: buffer_map(c, p, n, o); return true;
    case G_BUFFER_UNMAP: buffer_unmap(c, p, n); return true;
    case G_DESTROY: {
        if (!a || a >= MAX_IDS) return true;
        Obj *x = obj(c, a, c->objs[a].kind);
        if (!x) return true;
        if (x->kind == O_BUFFER) { w.BufferDestroy(x->h); x->res->destroyed = true; x->res->mapped = false; }
        else if (x->kind == O_TEXTURE) { w.TextureDestroy(x->h); x->res->destroyed = true; }
        else if (x->kind == O_DEVICE) {
            /* device.destroy(), without wgpuDeviceDestroy: a device destroyed
             * there never gives its memory back (a few dozen and no more
             * devices can be made).  Its buffers and textures are destroyed
             * (their memory freed now), and nothing of it reaches wgpu again;
             * the rest goes as the page lets go of it. */
            for (uint32_t i = 1; i < MAX_IDS; i++) {
                Obj *y = &c->objs[i];
                if (y->device != a || !y->res || y->res->destroyed) continue;
                if (y->kind == O_BUFFER) { w.BufferDestroy(y->h); y->res->destroyed = true; y->res->mapped = false; }
                else if (y->kind == O_TEXTURE) { w.TextureDestroy(y->h); y->res->destroyed = true; }
            }
            x->lost = true;
        }
        return true;
    }
    case G_RELEASE:
        if (!a || a >= MAX_IDS || !c->objs[a].kind || c->objs[a].kind == O_DEVICE || c->objs[a].kind == O_ADAPTER)
            return true;
        if (c->objs[a].kind == O_CMDBUF && c->nunsubmitted < 4096) {
            /* Kept (off its id) for wgpu_client_free. */
            if (c->nunsubmitted % 64 == 0) {
                Obj *u = (Obj *)realloc(c->unsubmitted, (c->nunsubmitted + 64) * sizeof(Obj));
                if (!u) { free_slot(c, &c->objs[a]); return true; }
                c->unsubmitted = u;
            }
            c->unsubmitted[c->nunsubmitted++] = c->objs[a];
            memset(&c->objs[a], 0, sizeof(Obj));
            return true;
        }
        free_slot(c, &c->objs[a]);
        return true;
    case G_CREATE_TEXTURE: {
        if (!dev) return true;
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.size = extent(get(t, "size"));
        td.mipLevelCount = u32(get(t, "mipLevelCount"), 1);
        td.sampleCount = u32(get(t, "sampleCount"), 1);
        td.dimension = (WGPUTextureDimension)lookup(DIMENSION, get(t, "dimension"), WGPUTextureDimension_2D);
        td.format = (WGPUTextureFormat)required(FORMATS, get(t, "format"));
        td.usage = u32(get(t, "usage"), 0);
        if (!g_bad) put_obj(c, a, O_TEXTURE, w.DeviceCreateTexture(dev->h, &td), b);
        return true;
    }
    case G_CREATE_VIEW: {
        Obj *tx = obj(c, b, O_TEXTURE);
        if (!tx) return true;
        WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        vd.format = (WGPUTextureFormat)lookup(FORMATS, get(t, "format"), WGPUTextureFormat_Undefined);
        vd.dimension = (WGPUTextureViewDimension)lookup(VIEW_DIMENSION, get(t, "dimension"), WGPUTextureViewDimension_Undefined);
        vd.aspect = (WGPUTextureAspect)lookup(ASPECT, get(t, "aspect"), WGPUTextureAspect_All);
        vd.baseMipLevel = u32(get(t, "baseMipLevel"), 0);
        vd.mipLevelCount = u32(get(t, "mipLevelCount"), WGPU_MIP_LEVEL_COUNT_UNDEFINED);
        vd.baseArrayLayer = u32(get(t, "baseArrayLayer"), 0);
        vd.arrayLayerCount = u32(get(t, "arrayLayerCount"), WGPU_ARRAY_LAYER_COUNT_UNDEFINED);
        if (!g_bad && put_obj(c, a, O_VIEW, w.TextureCreateView(tx->h, &vd), tx->device))
            c->objs[a].res = res_hold(tx->res);
        return true;
    }
    case G_CREATE_SAMPLER: {
        if (!dev) return true;
        WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
        sd.addressModeU = (WGPUAddressMode)lookup(ADDRESS, get(t, "addressModeU"), WGPUAddressMode_ClampToEdge);
        sd.addressModeV = (WGPUAddressMode)lookup(ADDRESS, get(t, "addressModeV"), WGPUAddressMode_ClampToEdge);
        sd.addressModeW = (WGPUAddressMode)lookup(ADDRESS, get(t, "addressModeW"), WGPUAddressMode_ClampToEdge);
        sd.magFilter = (WGPUFilterMode)lookup(FILTER, get(t, "magFilter"), WGPUFilterMode_Nearest);
        sd.minFilter = (WGPUFilterMode)lookup(FILTER, get(t, "minFilter"), WGPUFilterMode_Nearest);
        sd.mipmapFilter = (WGPUMipmapFilterMode)lookup(MIP_FILTER, get(t, "mipmapFilter"), WGPUMipmapFilterMode_Nearest);
        sd.lodMinClamp = (float)num(get(t, "lodMinClamp"), 0);
        sd.lodMaxClamp = (float)num(get(t, "lodMaxClamp"), 32);
        sd.compare = (WGPUCompareFunction)lookup(COMPARE, get(t, "compare"), WGPUCompareFunction_Undefined);
        sd.maxAnisotropy = (uint16_t)u32(get(t, "maxAnisotropy"), 1);
        if (!g_bad) put_obj(c, a, O_SAMPLER, w.DeviceCreateSampler(dev->h, &sd), b);
        return true;
    }
    case G_CREATE_SHADER_MODULE: {
        const Node *code = nd(get(t, "code"));
        if (!dev || !code || code->tag != V_STRING) return true;
        WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT;
        src.code.data = (const char *)code->data;
        src.code.length = code->n;
        WGPUShaderModuleDescriptor sd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
        sd.nextInChain = &src.chain;
        put_obj(c, a, O_MODULE, w.DeviceCreateShaderModule(dev->h, &sd), b);
        return true;
    }
    case G_COMPILATION_INFO:
        /* (wgpu reports compile errors as validation errors: the page sees
         * them through error scopes; the messages list stays empty.) */
        put_u8(o, V_ARRAY);
        put_u32(o, 0);
        return true;
    case G_CREATE_BIND_GROUP_LAYOUT:
        if (dev) put_obj(c, a, O_BGL, bind_group_layout(c, dev, t), b);
        return true;
    case G_CREATE_PIPELINE_LAYOUT: {
        if (!dev) return true;
        WGPUBindGroupLayout ls[8];
        int bl = get(t, "bindGroupLayouts");
        WGPUPipelineLayoutDescriptor pd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pd.bindGroupLayoutCount = count(bl) > 8 ? 8 : count(bl);
        for (uint32_t k2 = 0; k2 < pd.bindGroupLayoutCount; k2++) {
            ls[k2] = handle(c, item(bl, k2), O_BGL);
            if (!ls[k2]) return true;
        }
        pd.bindGroupLayouts = ls;
        put_obj(c, a, O_PL, w.DeviceCreatePipelineLayout(dev->h, &pd), b);
        return true;
    }
    case G_CREATE_BIND_GROUP: {
        if (!dev) return true;
        Obj uses = { 0 };
        g_holder = &uses;
        void *h = bind_group(c, dev, t);
        g_holder = NULL;
        if (put_obj(c, a, O_BG, h, b)) {
            c->objs[a].uses = uses.uses;
            c->objs[a].nuses = uses.nuses;
            c->objs[a].cap_uses = uses.cap_uses;
            c->objs[a].bad = uses.bad;
        } else {
            for (uint32_t k2 = 0; k2 < uses.nuses; k2++) res_drop(uses.uses[k2]);
            free(uses.uses);
        }
        return true;
    }
    case G_CREATE_RENDER_PIPELINE:
        if (dev) put_obj(c, a, O_RPIPE, render_pipeline(c, dev, t), b);
        return true;
    case G_CREATE_COMPUTE_PIPELINE: {
        if (!dev || (c->denied & GPU_FEATURE_COMPUTE)) return true;
        char ep[64];
        int cs = get(t, "compute");
        WGPUComputePipelineDescriptor cd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        cd.layout = handle(c, get(t, "layout"), O_PL);
        cd.compute.module = need(c, get(cs, "module"), O_MODULE);
        const char *e = string(get(cs, "entryPoint"), ep, sizeof(ep));
        cd.compute.entryPoint = e ? sv(e) : (WGPUStringView)WGPU_STRING_VIEW_INIT;
        static WGPUConstantEntry cc[32];
        static char cn[32][64];
        cd.compute.constantCount = constants(get(cs, "constants"), cc, cn, 32);
        cd.compute.constants = cc;
        if (!g_bad) put_obj(c, a, O_CPIPE, w.DeviceCreateComputePipeline(dev->h, &cd), b);
        return true;
    }
    case G_GET_BIND_GROUP_LAYOUT: {
        Obj *rp = obj(c, b, O_RPIPE), *cp = obj(c, b, O_CPIPE);
        if (rp) put_obj(c, a, O_BGL, w.RenderPipelineGetBindGroupLayout(rp->h, d), rp->device);
        else if (cp) put_obj(c, a, O_BGL, w.ComputePipelineGetBindGroupLayout(cp->h, d), cp->device);
        return true;
    }
    case G_CREATE_COMMAND_ENCODER:
        if (dev) put_obj(c, a, O_ENCODER, w.DeviceCreateCommandEncoder(dev->h, NULL), b);
        return true;
    case G_BEGIN_RENDER_PASS: {
        Obj *e = obj(c, b, O_ENCODER);
        if (!e) return true;
        g_holder = e;
        void *h = render_pass(c, e->h, t);
        g_holder = NULL;
        if (put_obj(c, a, O_RPASS, h, e->device)) c->objs[a].parent = b;
        return true;
    }
    case G_BEGIN_COMPUTE_PASS: {
        Obj *e = obj(c, b, O_ENCODER);
        if (e && put_obj(c, a, O_CPASS, w.CommandEncoderBeginComputePass(e->h, NULL), e->device)) c->objs[a].parent = b;
        return true;
    }
    case G_PASS:
        for (uint32_t k2 = 0; k2 < count(t); k2++) {
            pass_command(c, a, item(t, k2));
            if (g_bad) c->error = g_bad;
        }
        return true;
    case G_END_PASS: {
        Obj *rp = obj(c, a, O_RPASS), *cp = obj(c, a, O_CPASS);
        if (rp && !rp->ended) { w.RenderPassEncoderEnd(rp->h); rp->ended = true; }
        if (cp && !cp->ended) { w.ComputePassEncoderEnd(cp->h); cp->ended = true; }
        return true;
    }
    case G_ENCODE: {
        Obj *e = obj(c, a, O_ENCODER);
        for (uint32_t k2 = 0; e && k2 < count(t); k2++) {
            g_holder = e;
            encode_command(c, e->h, item(t, k2));
            g_holder = NULL;
            if (g_bad) c->error = g_bad;
        }
        return true;
    }
    case G_FINISH: {
        /* In an error scope of our own: a command buffer encoded wrong is
         * never kept (submitting it would abort wgpu-native). */
        Obj *e = obj(c, b, O_ENCODER);
        Obj *dv = e ? obj(c, e->device, O_DEVICE) : NULL;
        if (!e || !dv) return true;
        w.DevicePushErrorScope(dv->h, WGPUErrorFilter_Validation);
        WGPUCommandBuffer cb = w.CommandEncoderFinish(e->h, NULL);
        if (pop_scope(dv) != WGPUErrorType_NoError || e->bad) {
            if (cb) w.CommandBufferRelease(cb);
            refuse(c, e->bad ? "a command buffer uses too much" : g_error);
            return true;
        }
        if (put_obj(c, a, O_CMDBUF, cb, e->device)) {
            Obj *x = &c->objs[a];
            x->uses = e->uses;
            x->nuses = e->nuses;
            x->cap_uses = e->cap_uses;
            e->uses = NULL;
            e->nuses = e->cap_uses = 0;
        }
        return true;
    }
    case G_SUBMIT: {
        /* All of them, or (any wrong) none, and each used up either way. */
        Obj *q = obj(c, a, O_QUEUE);
        if (!q) return true;
        WGPUCommandBuffer bufs[64];
        Obj *used[64];
        size_t nb = 0;
        const char *why = count(t) > 64 ? "more than 64 command buffers" : NULL;
        for (uint32_t k2 = 0; k2 < count(t) && nb < 64; k2++) {
            const Node *x = nd(item(t, k2));
            Obj *cb = x && x->tag == V_ID ? obj(c, (uint32_t)x->num, O_CMDBUF) : NULL;
            if (!cb) { why = "a command buffer is invalid, or was submitted already"; continue; }
            for (size_t j = 0; j < nb; j++) if (used[j] == cb) { why = "a command buffer submitted twice"; cb = NULL; break; }
            if (!cb) continue;
            if (cb->device != q->device) why = "a command buffer of another device";
            for (uint32_t r = 0; r < cb->nuses; r++)
                if (cb->uses[r]->destroyed) why = "a destroyed buffer or texture is used";
                else if (cb->uses[r]->mapped) why = "a mapped buffer is used";
            used[nb] = cb;
            bufs[nb++] = cb->h;
        }
        if (why) refuse(c, why);
        else w.QueueSubmit(q->h, nb, bufs);
        for (size_t j = 0; j < nb; j++) free_slot(c, used[j]);
        return true;
    }
    case G_WRITE_BUFFER: {             /* queue, buffer, offset (f64), bytes */
        if (n < 16) return false;
        double off;
        memcpy(&off, p + 8, 8);
        Obj *q = obj(c, a, O_QUEUE), *bf = obj(c, b, O_BUFFER);
        if (bf && (bf->res->destroyed || bf->res->mapped)) { refuse(c, "writeBuffer to a destroyed or mapped buffer"); return true; }
        if (q && bf && off >= 0 && off < 9e15) w.QueueWriteBuffer(q->h, bf->h, (uint64_t)off, p + 16, n - 16);
        return true;
    }
    case G_WORK_DONE: {
        Obj *q = obj(c, a, O_QUEUE);
        int done = 0;
        if (q) {
            WGPUQueueWorkDoneCallbackInfo ci = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
            ci.mode = WGPUCallbackMode_AllowProcessEvents;
            ci.callback = on_done;
            ci.userdata1 = &done;
            w.QueueOnSubmittedWorkDone(q->h, ci);
            wait_for((volatile int *)&done);
        }
        put_u32(o, (uint32_t)done);
        return true;
    }
    case G_CONFIGURE: {
        Canvas *cv = canvas(c, a, true);
        if (!cv || !obj(c, b, O_DEVICE)) return true;
        cv->device = b;
        cv->format = (WGPUTextureFormat)lookup(FORMATS, get(t, "format"), WGPUTextureFormat_BGRA8Unorm);
        if (cv->format != WGPUTextureFormat_BGRA8Unorm && cv->format != WGPUTextureFormat_RGBA8Unorm &&
            cv->format != WGPUTextureFormat_RGBA16Float)
            cv->format = WGPUTextureFormat_BGRA8Unorm;
        char am[16];
        const char *s = string(get(t, "alphaMode"), am, sizeof(am));
        cv->opaque = !s || strcmp(s, "premultiplied") != 0;
        cv->width = u32(get(t, "width"), 300);
        cv->height = u32(get(t, "height"), 150);
        canvas_texture(c, cv);
        return true;
    }
    case G_CURRENT_TEXTURE: {           /* id, canvas */
        Canvas *cv = canvas(c, b, false);
        if (cv && cv->texture && a && a < MAX_IDS && !c->objs[a].kind) {
            /* The canvas keeps its own reference; the page's id gets another. */
            WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
            (void)vd;
            Res *r = (Res *)calloc(1, sizeof(Res));
            if (!r) return true;
            r->refs = 1;
            c->objs[a].kind = O_TEXTURE;
            c->objs[a].h = cv->texture;
            c->objs[a].device = cv->device;
            c->objs[a].res = r;
            /* A reference of its own, so a release of the page's leaves the canvas's. */
            typedef void (*AddRef)(WGPUTexture);
            static AddRef add_ref;
            if (!add_ref) add_ref = (AddRef)(void *)GetProcAddress(g_dll, "wgpuTextureAddRef");
            if (add_ref) add_ref(cv->texture);
        }
        return true;
    }
    case G_PUSH_ERROR_SCOPE: {
        Obj *dv = obj(c, a, O_DEVICE);
        if (dv && dv->scopes < 1000) {
            w.DevicePushErrorScope(dv->h, (WGPUErrorFilter)lookup(ERROR_FILTER, get(t, "filter"), WGPUErrorFilter_Validation));
            dv->scopes++;
        }
        return true;
    }
    case G_POP_ERROR_SCOPE: {
        Obj *dv = obj(c, a, O_DEVICE);
        int done = 0;
        g_scope_type = WGPUErrorType_NoError;
        if (dv && dv->scopes) {
            dv->scopes--;
            pop_scope(dv);
            done = 1;
        }
        if ((!done || g_scope_type == WGPUErrorType_NoError) && c->error) {
            /* A call refused here (g_bad): reported as wgpu would have. */
            g_scope_type = WGPUErrorType_Validation;
            snprintf(g_error, sizeof(g_error), "%s", c->error);
            done = 1;
        }
        c->error = NULL;
        if (!done || g_scope_type == WGPUErrorType_NoError) { put_u8(o, V_NULL); return true; }
        const char *kind = g_scope_type == WGPUErrorType_Validation ? "validation"
                         : g_scope_type == WGPUErrorType_OutOfMemory ? "out-of-memory" : "internal";
        put_u8(o, V_OBJECT);
        put_u32(o, 2);
        put_key(o, "type");
        put_str(o, kind, strlen(kind));
        put_key(o, "message");
        put_str(o, g_error, strlen(g_error));
        return true;
    }
    }
    return false;
}

bool wgpu_run(WgpuClient *c, const uint8_t *recs, uint32_t len, uint8_t **out, uint32_t *out_len, uint32_t *out_cap) {
    Out o = { out, out_len, out_cap, false };
    for (uint32_t at = 0; at + 8 <= len; ) {
        uint32_t op, n;
        memcpy(&op, recs + at, 4);
        memcpy(&n, recs + at + 4, 4);
        if (n > len - at - 8) return false;
        if (!run_one(c, op, recs + at + 8, n, &o) || o.failed) return false;
        if (g_bad && op != G_PASS && op != G_ENCODE) c->error = g_bad;
        at += 8 + n;
    }
    return true;
}

void wgpu_resize(WgpuClient *c, uint32_t id, uint32_t width, uint32_t height) {
    Canvas *cv = canvas(c, id, false);
    if (!cv || (cv->width == width && cv->height == height)) return;
    cv->width = width;
    cv->height = height;
    canvas_texture(c, cv);
}

void wgpu_present(WgpuClient *c, uint32_t id, uint8_t **px, size_t *cap, uint32_t *wd, uint32_t *ht) {
    *wd = *ht = 0;
    Canvas *cv = canvas(c, id, false);
    Obj *dev = cv ? obj(c, cv->device, O_DEVICE) : NULL;
    if (!cv || !cv->texture || !dev || cv->format == WGPUTextureFormat_RGBA16Float) return;
    uint32_t stride = (cv->width * 4 + 255) & ~255u;
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.size = (uint64_t)stride * cv->height;
    bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    WGPUBuffer buf = w.DeviceCreateBuffer(dev->h, &bd);
    WGPUCommandEncoder enc = w.DeviceCreateCommandEncoder(dev->h, NULL);
    WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    from.texture = cv->texture;
    WGPUTexelCopyBufferInfo to = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    to.buffer = buf;
    to.layout.bytesPerRow = stride;
    to.layout.rowsPerImage = cv->height;
    WGPUExtent3D x = { cv->width, cv->height, 1 };
    w.CommandEncoderCopyTextureToBuffer(enc, &from, &to, &x);
    WGPUCommandBuffer cb = w.CommandEncoderFinish(enc, NULL);
    Obj *q = NULL;
    for (uint32_t i = 1; i < MAX_IDS && !q; i++)
        if (c->objs[i].kind == O_QUEUE && c->objs[i].device == cv->device) q = &c->objs[i];
    if (q) w.QueueSubmit(q->h, 1, &cb);
    int done = 0;
    WGPUBufferMapCallbackInfo ci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    ci.mode = WGPUCallbackMode_AllowProcessEvents;
    ci.callback = on_map;
    ci.userdata1 = &done;
    w.BufferMapAsync(buf, WGPUMapMode_Read, 0, (size_t)bd.size, ci);
    wait_for((volatile int *)&done);
    const uint8_t *m = done == 1 ? w.BufferGetConstMappedRange(buf, 0, (size_t)bd.size) : NULL;
    size_t need = (size_t)cv->width * cv->height * 4;
    if (m && need > *cap) {
        free(*px);
        *px = (uint8_t *)malloc(need);
        *cap = *px ? need : 0;
    }
    if (m && *px) {
        bool bgra = cv->format == WGPUTextureFormat_BGRA8Unorm;
        for (uint32_t y = 0; y < cv->height; y++) {
            const uint8_t *s = m + (size_t)y * stride;
            uint8_t *d = *px + (size_t)y * cv->width * 4;
            for (uint32_t i = 0; i < cv->width; i++, s += 4, d += 4) {
                d[0] = bgra ? s[2] : s[0];
                d[1] = s[1];
                d[2] = bgra ? s[0] : s[2];
                d[3] = cv->opaque ? 255 : s[3];
            }
        }
        *wd = cv->width;
        *ht = cv->height;
    }
    if (m) w.BufferUnmap(buf);
    w.BufferRelease(buf);
    w.CommandBufferRelease(cb);
    w.CommandEncoderRelease(enc);
}

/* lib_webgpu.c - models of what cb_webgpu.exe links outside this step: the
 * WebGL backend over ANGLE (webgl_backend.c, webgl_gen.c) and WebGPU over
 * wgpu-native (webgpu_wgpu.c).  Their record walkers are M9 step 3's work
 * (SANDBOX_DESIGN.md 8.7.3); here they are TRUSTED models of what the shim
 * hands them and gets back: the record bytes they read, the output buffer
 * they grow with realloc (and the length they report), the pixels they
 * write, and the contexts they allocate and free. */
#include <stdlib.h>
#include <windows.h>
#include "webgl_backend.h"
#include "webgpu_wgpu.h"
#include "__fc_builtin.h"

WebglProcs gl;

static char g_renderer[256];

/*@ requires dir: valid_read_wstring(dir);
    requires why: why_len > 0 && \valid(why + (0 .. why_len - 1)); */
bool webgl_init(const wchar_t *dir, char *why, size_t why_len) {
    size_t n = wcslen(dir);
    (void)n;
    Frama_C_make_unknown(g_renderer, sizeof(g_renderer) - 1);
    g_renderer[sizeof(g_renderer) - 1] = 0;
    if (Frama_C_nondet(0, 1)) return true;
    Frama_C_make_unknown(why, why_len - 1);
    why[why_len - 1] = 0;
    return false;
}

/* ANGLE's renderer string (after webgl_init). */
const char *webgl_renderer(void) { return g_renderer; }

WebglCtx *webgl_create(uint32_t id, int version, uint32_t w, uint32_t h, uint32_t flags) {
    (void)flags;
    if (Frama_C_nondet(0, 1)) return NULL;          /* refused, or no memory */
    WebglCtx *c = (WebglCtx *)calloc(1, sizeof(WebglCtx));
    if (!c) return NULL;
    c->id = id;
    c->version = version;
    c->width = w;
    c->height = h;
    return c;
}

/* NULL, or a context webgl_create made. */
/*@ requires c: c == \null || \freeable(c); */
void webgl_destroy(WebglCtx *c) { free(c); }

/*@ requires c: \valid(c); */
bool webgl_resize(WebglCtx *c, uint32_t w, uint32_t h) {
    if (Frama_C_nondet(0, 1)) return false;
    c->width = w;
    c->height = h;
    return true;
}

/* Grows *out (realloc; *out_cap bytes) and appends to it: *out_len stays
 * within *out_cap. */
static bool append_results(uint8_t **out, uint32_t *out_len, uint32_t *out_cap) {
    uint32_t add = Frama_C_unsigned_int_interval(0, 1u << 20);
    uint64_t need = (uint64_t)*out_len + add;
    if (need > 0xffffffffu) return false;
    if (need > *out_cap) {
        uint8_t *nb = (uint8_t *)realloc(*out, (size_t)need);
        if (!nb) return false;
        *out = nb;
        *out_cap = (uint32_t)need;
    }
    if (add) {
        (*out)[*out_len] = 0;
        (*out)[*out_len + add - 1] = 0;
    }
    *out_len += add;
    return true;
}

/* Reads the records, appends query results to *out. */
/*@ requires c: \valid(c);
    requires recs: \valid_read(recs + (0 .. len - 1));
    requires out: \valid(out) && \valid(out_len) && \valid(out_cap);
    requires out_buf: *out == \null || \freeable(*out);
    requires out_len_ok: *out_len <= *out_cap; */
bool webgl_run(WebglCtx *c, const uint8_t *recs, uint32_t len, uint8_t **out, uint32_t *out_len, uint32_t *out_cap) {
    if (len) {
        uint8_t first = recs[0], last = recs[len - 1];
        (void)first;
        (void)last;
    }
    *out_len = 0;
    if (Frama_C_nondet(0, 1)) c->lost = true;
    return append_results(out, out_len, out_cap) && Frama_C_nondet(0, 1);
}

/* The drawing buffer into px: width * height * 4 bytes. */
/*@ requires c: \valid_read(c);
    requires px: \valid(px + (0 .. (size_t)c->width * c->height * 4 - 1)); */
bool webgl_read_frame(WebglCtx *c, uint8_t *px) {
    size_t n = (size_t)c->width * c->height * 4;
    if (n) {
        px[0] = 0;
        px[n - 1] = 0;
    }
    return Frama_C_nondet(0, 1);
}

typedef struct FcWgpu { int unused; } FcWgpu;

/*@ requires dir: valid_read_wstring(dir); */
WgpuClient *wgpu_client_new(const wchar_t *dir) {
    size_t n = wcslen(dir);
    (void)n;
    if (Frama_C_nondet(0, 1)) return NULL;
    return (WgpuClient *)malloc(sizeof(FcWgpu));
}

/*@ requires c: \freeable(c); */
void wgpu_client_free(WgpuClient *c) { free(c); }

/*@ requires c: c != \null;
    requires recs: \valid_read(recs + (0 .. len - 1));
    requires out: \valid(out) && \valid(out_len) && \valid(out_cap);
    requires out_buf: *out == \null || \freeable(*out);
    requires out_len_ok: *out_len <= *out_cap; */
bool wgpu_run(WgpuClient *c, const uint8_t *recs, uint32_t len, uint8_t **out, uint32_t *out_len, uint32_t *out_cap) {
    (void)c;
    if (len) {
        uint8_t first = recs[0], last = recs[len - 1];
        (void)first;
        (void)last;
    }
    *out_len = 0;
    return append_results(out, out_len, out_cap) && Frama_C_nondet(0, 1);
}

/* The canvas's texture into *px, grown (realloc) to *cap >= w * h * 4;
 * *w, *h 0 if there is none. */
/*@ requires c: c != \null;
    requires px: \valid(px) && \valid(cap) && \valid(w) && \valid(h);
    requires px_buf: *px == \null || \freeable(*px); */
void wgpu_present(WgpuClient *c, uint32_t canvas, uint8_t **px, size_t *cap, uint32_t *w, uint32_t *h) {
    (void)c;
    (void)canvas;
    *w = *h = 0;
    if (Frama_C_nondet(0, 1)) return;
    uint32_t tw = Frama_C_unsigned_int_interval(1, 16384), th = Frama_C_unsigned_int_interval(1, 16384);
    size_t need = (size_t)tw * th * 4;
    if (need > *cap) {
        uint8_t *nb = (uint8_t *)realloc(*px, need);
        if (!nb) return;
        *px = nb;
        *cap = need;
    }
    (*px)[0] = 0;
    (*px)[need - 1] = 0;
    *w = tw;
    *h = th;
}

/*@ requires c: c != \null; */
void wgpu_resize(WgpuClient *c, uint32_t canvas, uint32_t w, uint32_t h) {
    (void)c; (void)canvas; (void)w; (void)h;
}

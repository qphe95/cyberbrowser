#ifndef WEBGL_BACKEND_H
#define WEBGL_BACKEND_H

/* WebGL in the web-GPU process (SANDBOX_DESIGN.md section 5.7): ANGLE's EGL
 * and GLES, loaded by full path, with a WebGL compatibility context (ANGLE
 * checks each call as WebGL requires), robust buffer access and robust
 * resource initialization (nothing reads past a buffer, nothing
 * uninitialized is ever read) for each canvas.
 *
 * A tab's calls come as records, [u32 op][u32 n][n bytes of arguments],
 * little-endian, in a TW_GL_RUN message.  Objects are named by ids the tab
 * gives out, mapped per context to the names GLES gave; data reaches GLES
 * only through entry points told its size (ANGLE's robust ones). */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef unsigned GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef char GLchar;
typedef long long GLintptr, GLsizeiptr;
typedef unsigned long long GLuint64;
typedef struct __GLsync *GLsync;

/* The kinds of objects, their ids, the ops, and the proved walk over the
 * records (webgl_decode.c). */
#include "webgl_decode.h"

typedef struct WebglCtx {
    uint32_t id;                    /* the tab's */
    int version;                    /* 1 or 2 */
    void *context, *surface, *config;   /* EGL */
    uint32_t width, height;
    bool alpha, preserve, lost;
    uint32_t *names[WEBGL_KINDS];   /* id -> GLES name (0 = none), made on first use */
    WebglLocation *locations;
    GLsync *syncs;
    GLenum pending_error;           /* one of ours, reported before GLES's */
} WebglCtx;

/* The GLES entry points, loaded once. */
#include "webgl_gen.h"
#define WEBGL_PROC_FIELD(name, ret, params) ret (__stdcall *name) params;
typedef struct WebglProcs {
    WEBGL_GEN_PROCS(WEBGL_PROC_FIELD)
} WebglProcs;
extern WebglProcs gl;

static inline uint32_t rd_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline int32_t rd_i32(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }
static inline int64_t rd_i64(const uint8_t *p) { int64_t v; memcpy(&v, p, 8); return v; }
static inline float rd_f32(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

/* The GLES name for a tab's id (WEBGL_NO_NAME if it names nothing; 0 is
 * 0, "none"). */
GLuint webgl_name(WebglCtx *c, int kind, uint32_t id);
/* A uniform location id: its location, or -1. */
GLint webgl_location(WebglCtx *c, uint32_t id);
/* Whether it belongs to the program in use; if not, INVALID_OPERATION is
 * noted, as WebGL requires, and the call must not be made. */
bool webgl_location_ok(WebglCtx *c, uint32_t id);

/* ANGLE loaded from dir (libEGL.dll, libGLESv2.dll): false if it can't be,
 * with why. */
bool webgl_init(const wchar_t *dir, char *why, size_t why_len);
/* "vendor / renderer" of the device ANGLE uses, for the block list. */
const char *webgl_renderer(void);

/* A new context for canvas id, version 1 or 2, w x h, flags (WEBGL_*). */
enum { WEBGL_ALPHA = 1, WEBGL_DEPTH = 2, WEBGL_STENCIL = 4, WEBGL_ANTIALIAS = 8, WEBGL_PRESERVE = 16 };
WebglCtx *webgl_create(uint32_t id, int version, uint32_t w, uint32_t h, uint32_t flags);
void webgl_destroy(WebglCtx *c);
bool webgl_resize(WebglCtx *c, uint32_t w, uint32_t h);

/* Run records (len bytes), each as webgl_next accepts it; results of
 * queries are appended to *out (grown with realloc; *out_len bytes).  False
 * if a record is malformed: the rest is dropped. */
bool webgl_run(WebglCtx *c, const uint8_t *recs, uint32_t len, uint8_t **out, uint32_t *out_len, uint32_t *out_cap);

/* The drawing buffer, RGBA top-down, into px (w * h * 4). */
bool webgl_read_frame(WebglCtx *c, uint8_t *px);

/* Generated: one plain call. */
bool webgl_exec_plain(WebglCtx *c, uint32_t op, const uint8_t *p, uint32_t n);

#endif

/* webgl_backend.c - WebGL on ANGLE in the web-GPU process (webgl_backend.h). */
#include "webgl_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

WebglProcs gl;

/* ---- EGL and the GLES calls the special records use ---------------------------- */

typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface;
typedef int32_t EGLint;
typedef unsigned EGLBoolean, EGLenum;

#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE 0x3208
#define EGL_NONE 0x3038
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_SAMPLES 0x3031
#define EGL_SAMPLE_BUFFERS 0x3032
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_OPENGL_ROBUST_ACCESS 0x31B2
#define EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY 0x3138
#define EGL_LOSE_CONTEXT_ON_RESET 0x31BF
#define EGL_ROBUST_RESOURCE_INITIALIZATION_ANGLE 0x3453
#define EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE 0x33AC
#define EGL_CONTEXT_BIND_GENERATES_RESOURCE_CHROMIUM 0x33AD
#define EGL_CONTEXT_CLIENT_ARRAYS_ENABLED_ANGLE 0x3452
#define EGL_EXTENSIONS 0x3055

#define GL_NO_ERROR 0
#define GL_INVALID_OPERATION 0x0502
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_PACK_SKIP_ROWS 0x0D03
#define GL_PACK_SKIP_PIXELS 0x0D04
#define GL_READ_BUFFER 0x0C02
#define GL_BACK 0x0405
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_REQUESTABLE_EXTENSIONS_ANGLE 0x93A8
#define GL_EXTENSIONS 0x1F03
#define GL_RENDERER 0x1F01
#define GL_VENDOR 0x1F00
#define GL_MAP_READ_BIT 0x0001
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_GUILTY_CONTEXT_RESET 0x8253

#define EGLF(X) \
    X(EGLDisplay, eglGetPlatformDisplayEXT, (EGLenum, void *, const EGLint *)) \
    X(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *)) \
    X(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *)) \
    X(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *)) \
    X(EGLBoolean, eglDestroyContext, (EGLDisplay, EGLContext)) \
    X(EGLSurface, eglCreatePbufferSurface, (EGLDisplay, EGLConfig, const EGLint *)) \
    X(EGLBoolean, eglDestroySurface, (EGLDisplay, EGLSurface)) \
    X(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext)) \
    X(EGLint, eglGetError, (void)) \
    X(const char *, eglQueryString, (EGLDisplay, EGLint))

#define GLX(X) \
    X(GLenum, glGetError, (void)) \
    X(void, glGenBuffers, (GLsizei, GLuint *)) X(void, glDeleteBuffers, (GLsizei, const GLuint *)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) X(void, glDeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, glGenRenderbuffers, (GLsizei, GLuint *)) X(void, glDeleteRenderbuffers, (GLsizei, const GLuint *)) \
    X(void, glGenTextures, (GLsizei, GLuint *)) X(void, glDeleteTextures, (GLsizei, const GLuint *)) \
    X(GLuint, glCreateShader, (GLenum)) X(void, glDeleteShader, (GLuint)) \
    X(GLuint, glCreateProgram, (void)) X(void, glDeleteProgram, (GLuint)) \
    X(void, glGenVertexArrays, (GLsizei, GLuint *)) X(void, glDeleteVertexArrays, (GLsizei, const GLuint *)) \
    X(void, glGenQueries, (GLsizei, GLuint *)) X(void, glDeleteQueries, (GLsizei, const GLuint *)) \
    X(void, glGenSamplers, (GLsizei, GLuint *)) X(void, glDeleteSamplers, (GLsizei, const GLuint *)) \
    X(void, glGenTransformFeedbacks, (GLsizei, GLuint *)) X(void, glDeleteTransformFeedbacks, (GLsizei, const GLuint *)) \
    X(GLboolean, glIsBuffer, (GLuint)) X(GLboolean, glIsFramebuffer, (GLuint)) X(GLboolean, glIsRenderbuffer, (GLuint)) \
    X(GLboolean, glIsTexture, (GLuint)) X(GLboolean, glIsShader, (GLuint)) X(GLboolean, glIsProgram, (GLuint)) \
    X(GLboolean, glIsVertexArray, (GLuint)) X(GLboolean, glIsQuery, (GLuint)) X(GLboolean, glIsSampler, (GLuint)) \
    X(GLboolean, glIsTransformFeedback, (GLuint)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *)) \
    X(void, glTexImage2DRobustANGLE, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, GLsizei, const void *)) \
    X(void, glTexSubImage2DRobustANGLE, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLsizei, const void *)) \
    X(void, glTexImage3DRobustANGLE, (GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, GLsizei, const void *)) \
    X(void, glTexSubImage3DRobustANGLE, (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, GLsizei, const void *)) \
    X(void, glCompressedTexImage2DRobustANGLE, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, GLsizei, const void *)) \
    X(void, glCompressedTexSubImage2DRobustANGLE, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, GLsizei, const void *)) \
    X(void, glReadPixelsRobustANGLE, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLsizei, GLsizei *, GLsizei *, GLsizei *, void *)) \
    X(void, glPixelStorei, (GLenum, GLint)) \
    X(void, glUniform1fv, (GLint, GLsizei, const GLfloat *)) X(void, glUniform2fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glUniform3fv, (GLint, GLsizei, const GLfloat *)) X(void, glUniform4fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glUniform1iv, (GLint, GLsizei, const GLint *)) X(void, glUniform2iv, (GLint, GLsizei, const GLint *)) \
    X(void, glUniform3iv, (GLint, GLsizei, const GLint *)) X(void, glUniform4iv, (GLint, GLsizei, const GLint *)) \
    X(void, glUniform1uiv, (GLint, GLsizei, const GLuint *)) X(void, glUniform2uiv, (GLint, GLsizei, const GLuint *)) \
    X(void, glUniform3uiv, (GLint, GLsizei, const GLuint *)) X(void, glUniform4uiv, (GLint, GLsizei, const GLuint *)) \
    X(void, glUniformMatrix2fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glUniformMatrix3fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glUniformMatrix2x3fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glUniformMatrix3x2fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glUniformMatrix2x4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glUniformMatrix4x2fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glUniformMatrix3x4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glUniformMatrix4x3fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glVertexAttrib1fv, (GLuint, const GLfloat *)) X(void, glVertexAttrib2fv, (GLuint, const GLfloat *)) \
    X(void, glVertexAttrib3fv, (GLuint, const GLfloat *)) X(void, glVertexAttrib4fv, (GLuint, const GLfloat *)) \
    X(void, glVertexAttribI4iv, (GLuint, const GLint *)) X(void, glVertexAttribI4uiv, (GLuint, const GLuint *)) \
    X(void, glDrawBuffers, (GLsizei, const GLenum *)) \
    X(void, glClearBufferfv, (GLenum, GLint, const GLfloat *)) X(void, glClearBufferiv, (GLenum, GLint, const GLint *)) \
    X(void, glClearBufferuiv, (GLenum, GLint, const GLuint *)) \
    X(void, glInvalidateFramebuffer, (GLenum, GLsizei, const GLenum *)) \
    X(void, glTransformFeedbackVaryings, (GLuint, GLsizei, const GLchar *const *, GLenum)) \
    X(GLsync, glFenceSync, (GLenum, GLbitfield)) X(void, glDeleteSync, (GLsync)) \
    X(GLenum, glClientWaitSync, (GLsync, GLbitfield, GLuint64)) \
    X(void, glWaitSync, (GLsync, GLbitfield, GLuint64)) \
    X(void, glGetSynciv, (GLsync, GLenum, GLsizei, GLsizei *, GLint *)) \
    X(void, glGetIntegervRobustANGLE, (GLenum, GLsizei, GLsizei *, GLint *)) \
    X(void, glGetFloatvRobustANGLE, (GLenum, GLsizei, GLsizei *, GLfloat *)) \
    X(void, glGetBooleanvRobustANGLE, (GLenum, GLsizei, GLsizei *, GLboolean *)) \
    X(void, glGetIntegeri_vRobustANGLE, (GLenum, GLuint, GLsizei, GLsizei *, GLint *)) \
    X(void, glGetInteger64vRobustANGLE, (GLenum, GLsizei, GLsizei *, long long *)) \
    X(const GLchar *, glGetString, (GLenum)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
    X(GLint, glGetAttribLocation, (GLuint, const GLchar *)) \
    X(GLint, glGetFragDataLocation, (GLuint, const GLchar *)) \
    X(void, glGetActiveAttrib, (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *)) \
    X(void, glGetActiveUniform, (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *)) \
    X(void, glGetTransformFeedbackVarying, (GLuint, GLuint, GLsizei, GLsizei *, GLsizei *, GLenum *, GLchar *)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum)) \
    X(void, glGetTexParameterivRobustANGLE, (GLenum, GLenum, GLsizei, GLsizei *, GLint *)) \
    X(void, glGetTexParameterfvRobustANGLE, (GLenum, GLenum, GLsizei, GLsizei *, GLfloat *)) \
    X(void, glGetBufferParameterivRobustANGLE, (GLenum, GLenum, GLsizei, GLsizei *, GLint *)) \
    X(void, glGetRenderbufferParameterivRobustANGLE, (GLenum, GLenum, GLsizei, GLsizei *, GLint *)) \
    X(void, glGetFramebufferAttachmentParameterivRobustANGLE, (GLenum, GLenum, GLenum, GLsizei, GLsizei *, GLint *)) \
    X(void, glGetShaderPrecisionFormat, (GLenum, GLenum, GLint *, GLint *)) \
    X(void, glGetUniformfvRobustANGLE, (GLuint, GLint, GLsizei, GLsizei *, GLfloat *)) \
    X(void, glGetUniformivRobustANGLE, (GLuint, GLint, GLsizei, GLsizei *, GLint *)) \
    X(void, glGetUniformuivRobustANGLE, (GLuint, GLint, GLsizei, GLsizei *, GLuint *)) \
    X(void, glGetVertexAttribfvRobustANGLE, (GLuint, GLenum, GLsizei, GLsizei *, GLfloat *)) \
    X(void, glGetVertexAttribivRobustANGLE, (GLuint, GLenum, GLsizei, GLsizei *, GLint *)) \
    X(void, glGetVertexAttribIuivRobustANGLE, (GLuint, GLenum, GLsizei, GLsizei *, GLuint *)) \
    X(void, glGetVertexAttribPointervRobustANGLE, (GLuint, GLenum, GLsizei, GLsizei *, void **)) \
    X(void, glGetQueryObjectuivRobustANGLE, (GLuint, GLenum, GLsizei, GLsizei *, GLuint *)) \
    X(void, glGetSamplerParameterivRobustANGLE, (GLuint, GLenum, GLsizei, GLsizei *, GLint *)) \
    X(void, glGetSamplerParameterfvRobustANGLE, (GLuint, GLenum, GLsizei, GLsizei *, GLfloat *)) \
    X(void, glGetInternalformativRobustANGLE, (GLenum, GLenum, GLenum, GLsizei, GLsizei *, GLint *)) \
    X(void *, glMapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield)) \
    X(GLboolean, glUnmapBuffer, (GLenum)) \
    X(GLuint, glGetUniformBlockIndex, (GLuint, const GLchar *)) \
    X(void, glGetActiveUniformBlockivRobustANGLE, (GLuint, GLuint, GLenum, GLsizei, GLsizei *, GLint *)) \
    X(void, glGetActiveUniformBlockName, (GLuint, GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glGetUniformIndices, (GLuint, GLsizei, const GLchar *const *, GLuint *)) \
    X(void, glGetActiveUniformsiv, (GLuint, GLsizei, const GLuint *, GLenum, GLint *)) \
    X(void, glRequestExtensionANGLE, (const GLchar *)) \
    X(GLenum, glGetGraphicsResetStatusEXT, (void))

#define DECL(ret, name, params) typedef ret (__stdcall *PFN_##name) params; static PFN_##name p_##name;
EGLF(DECL)
GLX(DECL)

static HMODULE g_egl, g_gles;
static EGLDisplay g_display;
static char g_renderer[512];

#define WEBGL_LOAD_GEN(name, ret, params) \
    gl.name = (ret(__stdcall *) params)(void *)GetProcAddress(g_gles, #name); \
    if (!gl.name) { snprintf(why, why_len, "libGLESv2.dll has no %s", #name); return false; }

bool webgl_init(const wchar_t *dir, char *why, size_t why_len) {
    if (g_display) return true;
    wchar_t path[MAX_PATH];
    /* By full path, and their own dependencies (the MinGW runtime) from
     * their folder: never the DLL search path. */
    _snwprintf(path, MAX_PATH, L"%ls\\libEGL.dll", dir);
    g_egl = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    _snwprintf(path, MAX_PATH, L"%ls\\libGLESv2.dll", dir);
    g_gles = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_egl || !g_gles) {
        snprintf(why, why_len, "could not load ANGLE (%lu)", GetLastError());
        return false;
    }
#define LOAD_EGL(ret, name, params) \
    p_##name = (PFN_##name)(void *)GetProcAddress(g_egl, #name); \
    if (!p_##name && strcmp(#name, "eglGetPlatformDisplayEXT")) { snprintf(why, why_len, "libEGL.dll has no %s", #name); return false; }
    EGLF(LOAD_EGL)
    if (!p_eglGetPlatformDisplayEXT) {
        typedef void *(__stdcall * GPA)(const char *);
        GPA gpa = (GPA)(void *)GetProcAddress(g_egl, "eglGetProcAddress");
        if (gpa) p_eglGetPlatformDisplayEXT = (PFN_eglGetPlatformDisplayEXT)gpa("eglGetPlatformDisplayEXT");
        if (!p_eglGetPlatformDisplayEXT) { snprintf(why, why_len, "no eglGetPlatformDisplayEXT"); return false; }
    }
#define LOAD_GL(ret, name, params) \
    p_##name = (PFN_##name)(void *)GetProcAddress(g_gles, #name); \
    if (!p_##name) { snprintf(why, why_len, "libGLESv2.dll has no %s", #name); return false; }
    GLX(LOAD_GL)
    WEBGL_GEN_PROCS(WEBGL_LOAD_GEN)
    /* D3D11: ANGLE's best-tested back end on Windows. */
    EGLint pa[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE, EGL_NONE };
    EGLDisplay d = p_eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, NULL, pa);
    EGLint maj = 0, min = 0;
    if (!d || !p_eglInitialize(d, &maj, &min)) {
        snprintf(why, why_len, "eglInitialize failed (%x)", p_eglGetError());
        return false;
    }
    const char *ext = p_eglQueryString(d, EGL_EXTENSIONS);
    if (!ext || !strstr(ext, "EGL_ANGLE_create_context_webgl_compatibility") ||
        !strstr(ext, "EGL_ANGLE_robust_resource_initialization") || !strstr(ext, "EGL_EXT_create_context_robustness")) {
        snprintf(why, why_len, "this ANGLE has no WebGL compatibility or robustness");
        return false;
    }
    g_display = d;
    /* The renderer, from a context of its own. */
    WebglCtx *probe = webgl_create(0, 1, 1, 1, 0);
    if (probe) {
        snprintf(g_renderer, sizeof(g_renderer), "%s / %s", p_glGetString(GL_VENDOR), p_glGetString(GL_RENDERER));
        webgl_destroy(probe);
    }
    return true;
}

const char *webgl_renderer(void) { return g_renderer; }

static bool make_current(WebglCtx *c) {
    return p_eglMakeCurrent(g_display, c->surface, c->surface, c->context) != 0;
}

static EGLSurface new_surface(WebglCtx *c, uint32_t w, uint32_t h) {
    EGLint sa[] = { EGL_WIDTH, (EGLint)(w ? w : 1), EGL_HEIGHT, (EGLint)(h ? h : 1), EGL_NONE };
    return p_eglCreatePbufferSurface(g_display, c->config, sa);
}

WebglCtx *webgl_create(uint32_t id, int version, uint32_t w, uint32_t h, uint32_t flags) {
    if (!g_display || (version != 1 && version != 2) || w > 16384 || h > 16384) return NULL;
    WebglCtx *c = (WebglCtx *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->id = id;
    c->version = version;
    c->alpha = (flags & WEBGL_ALPHA) != 0;
    c->preserve = (flags & WEBGL_PRESERVE) != 0;
    EGLint ca[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, (flags & WEBGL_ALPHA) ? 8 : 0,
                    EGL_DEPTH_SIZE, (flags & WEBGL_DEPTH) ? 24 : 0, EGL_STENCIL_SIZE, (flags & WEBGL_STENCIL) ? 8 : 0,
                    EGL_SAMPLE_BUFFERS, (flags & WEBGL_ANTIALIAS) ? 1 : 0, EGL_SAMPLES, (flags & WEBGL_ANTIALIAS) ? 4 : 0,
                    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                    EGL_RENDERABLE_TYPE, version == 2 ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLint n = 0;
    if (!p_eglChooseConfig(g_display, ca, &c->config, 1, &n) || n < 1) {
        /* No multisampled config: without. */
        ca[13] = 0;
        ca[15] = 0;
        if (!p_eglChooseConfig(g_display, ca, &c->config, 1, &n) || n < 1) { free(c); return NULL; }
    }
    /* WebGL's checks done by ANGLE; robust access, resources initialized
     * before first use, and lost on a reset rather than undefined.  A name
     * not made by glGen* (WEBGL_NO_NAME, another context's) is refused, not
     * made on binding; no client-side arrays. */
    EGLint xa[] = { EGL_CONTEXT_MAJOR_VERSION, version == 2 ? 3 : 2, EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE, 1,
                    EGL_CONTEXT_BIND_GENERATES_RESOURCE_CHROMIUM, 0, EGL_CONTEXT_CLIENT_ARRAYS_ENABLED_ANGLE, 0,
                    EGL_CONTEXT_OPENGL_ROBUST_ACCESS, 1, EGL_ROBUST_RESOURCE_INITIALIZATION_ANGLE, 1,
                    EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY, EGL_LOSE_CONTEXT_ON_RESET, EGL_NONE };
    c->context = p_eglCreateContext(g_display, c->config, NULL, xa);
    c->surface = c->context ? new_surface(c, w, h) : NULL;
    if (!c->context || !c->surface || !make_current(c)) {
        webgl_destroy(c);
        return NULL;
    }
    c->width = w ? w : 1;
    c->height = h ? h : 1;
    gl.glViewport(0, 0, (GLsizei)c->width, (GLsizei)c->height);
    return c;
}

void webgl_destroy(WebglCtx *c) {
    if (!c) return;
    p_eglMakeCurrent(g_display, NULL, NULL, NULL);
    if (c->surface) p_eglDestroySurface(g_display, c->surface);
    if (c->context) p_eglDestroyContext(g_display, c->context);
    for (int k = 0; k < WEBGL_KINDS; k++) free(c->names[k]);
    free(c->locations);
    free(c->syncs);
    free(c);
}

bool webgl_resize(WebglCtx *c, uint32_t w, uint32_t h) {
    if (w > 16384 || h > 16384) return false;
    EGLSurface s = new_surface(c, w, h);
    if (!s) return false;
    p_eglMakeCurrent(g_display, s, s, c->context);
    p_eglDestroySurface(g_display, c->surface);
    c->surface = s;
    c->width = w ? w : 1;
    c->height = h ? h : 1;
    return make_current(c);
}

/* ---- ids -------------------------------------------------------------------------------- */

static uint32_t *table(WebglCtx *c, int kind) {
    if (!c->names[kind]) c->names[kind] = (uint32_t *)calloc(WEBGL_MAX_IDS, sizeof(uint32_t));
    return c->names[kind];
}

GLuint webgl_name(WebglCtx *c, int kind, uint32_t id) {
    if (id == 0) return 0;
    uint32_t *t = id < WEBGL_MAX_IDS ? table(c, kind) : NULL;
    return t && t[id] ? t[id] : WEBGL_NO_NAME;
}

GLint webgl_location(WebglCtx *c, uint32_t id) {
    if (!id || id >= WEBGL_MAX_IDS || !c->locations) return -1;
    return c->locations[id].program ? c->locations[id].location : -1;
}

bool webgl_location_ok(WebglCtx *c, uint32_t id) {
    if (!id) return true;                   /* null: the call does nothing */
    GLint cur = 0;
    p_glGetIntegervRobustANGLE(GL_CURRENT_PROGRAM, 1, NULL, &cur);
    if (id < WEBGL_MAX_IDS && c->locations && c->locations[id].program && c->locations[id].program == (GLuint)cur)
        return true;
    if (!c->pending_error) c->pending_error = GL_INVALID_OPERATION;
    return false;
}

/* ---- the special records ---------------------------------------------------------------- */

enum {
    OP_CREATE = 256, OP_DELETE, OP_SHADER_SOURCE, OP_BIND_ATTRIB_LOCATION, OP_BUFFER_DATA, OP_BUFFER_SUB_DATA,
    OP_TEX_IMAGE_2D, OP_TEX_SUB_IMAGE_2D, OP_COMPRESSED_TEX_IMAGE_2D, OP_COMPRESSED_TEX_SUB_IMAGE_2D,
    OP_UNIFORM_V, OP_VERTEX_ATTRIB_V, OP_TEX_IMAGE_3D, OP_TEX_SUB_IMAGE_3D, OP_DRAW_BUFFERS, OP_CLEAR_BUFFER_V,
    OP_INVALIDATE_FRAMEBUFFER, OP_TRANSFORM_FEEDBACK_VARYINGS, OP_PIXEL_STOREI, OP_FENCE_SYNC, OP_WAIT_SYNC,
    /* Queries: each appends its result. */
    OP_GET_ERROR = 300, OP_GET_INTEGER, OP_GET_FLOAT, OP_GET_BOOLEAN, OP_GET_STRING, OP_GET_SHADER_PARAM,
    OP_GET_SHADER_INFO_LOG, OP_GET_PROGRAM_PARAM, OP_GET_PROGRAM_INFO_LOG, OP_GET_UNIFORM_LOCATION,
    OP_GET_ATTRIB_LOCATION, OP_GET_ACTIVE_ATTRIB, OP_GET_ACTIVE_UNIFORM, OP_READ_PIXELS, OP_CHECK_FRAMEBUFFER_STATUS,
    OP_IS_OBJECT, OP_GET_TEX_PARAM, OP_GET_BUFFER_PARAM, OP_GET_RENDERBUFFER_PARAM, OP_GET_FRAMEBUFFER_ATTACHMENT_PARAM,
    OP_GET_SHADER_PRECISION, OP_GET_UNIFORM, OP_GET_VERTEX_ATTRIB, OP_GET_VERTEX_ATTRIB_OFFSET, OP_GET_QUERY_PARAM,
    OP_GET_SAMPLER_PARAM, OP_GET_SYNC_PARAM, OP_CLIENT_WAIT_SYNC, OP_GET_BUFFER_SUB_DATA, OP_GET_INDEXED,
    OP_GET_FRAG_DATA_LOCATION, OP_GET_UNIFORM_BLOCK_INDEX, OP_GET_ACTIVE_UNIFORM_BLOCK_PARAM,
    OP_GET_ACTIVE_UNIFORM_BLOCK_NAME, OP_GET_UNIFORM_INDICES, OP_GET_ACTIVE_UNIFORMS, OP_GET_TRANSFORM_FEEDBACK_VARYING,
    OP_GET_INTERNALFORMAT_PARAM, OP_REQUEST_EXTENSION, OP_GET_EXTENSIONS, OP_GET_RESET_STATUS,
};

/* Results: appended to a growing buffer. */
typedef struct Out {
    uint8_t **buf;
    uint32_t *len, *cap;
    bool failed;
} Out;

static void put(Out *o, const void *p, uint32_t n) {
    if (o->failed) return;
    if (*o->len + n > *o->cap) {
        uint32_t cap = *o->cap ? *o->cap : 256;
        while (cap < *o->len + n) {
            if (cap > (64u << 20)) { o->failed = true; return; }
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
static void put_u32(Out *o, uint32_t v) { put(o, &v, 4); }
static void put_bytes(Out *o, const void *p, uint32_t n) { put_u32(o, n); put(o, p, n); }
static void put_str(Out *o, const char *s) { put_bytes(o, s ? s : "", s ? (uint32_t)strlen(s) : 0); }

/* A NUL-terminated copy of n bytes (a name or a source), or NULL. */
static char *str(const uint8_t *p, uint32_t n) {
    if (n > (4u << 20)) return NULL;
    char *s = (char *)malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, p, n);
    s[n] = 0;
    return memchr(s, 0, n) ? (free(s), (char *)NULL) : s;
}

static void create(WebglCtx *c, uint32_t kind, uint32_t id, uint32_t arg) {
    if (kind >= WEBGL_KINDS || kind == WEBGL_LOCATION || kind == WEBGL_SYNC || !id || id >= WEBGL_MAX_IDS) return;
    uint32_t *t = table(c, (int)kind);
    if (!t || t[id]) return;                    /* each id once */
    GLuint name = 0;
    switch (kind) {
    case WEBGL_BUFFER: p_glGenBuffers(1, &name); break;
    case WEBGL_FRAMEBUFFER: p_glGenFramebuffers(1, &name); break;
    case WEBGL_RENDERBUFFER: p_glGenRenderbuffers(1, &name); break;
    case WEBGL_TEXTURE: p_glGenTextures(1, &name); break;
    case WEBGL_SHADER: name = p_glCreateShader(arg); break;
    case WEBGL_PROGRAM: name = p_glCreateProgram(); break;
    case WEBGL_VERTEX_ARRAY: p_glGenVertexArrays(1, &name); break;
    case WEBGL_QUERY: p_glGenQueries(1, &name); break;
    case WEBGL_SAMPLER: p_glGenSamplers(1, &name); break;
    case WEBGL_TRANSFORM_FEEDBACK: p_glGenTransformFeedbacks(1, &name); break;
    }
    t[id] = name;
}

static void destroy(WebglCtx *c, uint32_t kind, uint32_t id) {
    if (kind >= WEBGL_KINDS || !id || id >= WEBGL_MAX_IDS) return;
    if (kind == WEBGL_SYNC) {
        if (c->syncs && c->syncs[id]) { p_glDeleteSync(c->syncs[id]); c->syncs[id] = NULL; }
        return;
    }
    if (kind == WEBGL_LOCATION) return;
    uint32_t *t = table(c, (int)kind);
    if (!t || !t[id]) return;
    GLuint name = t[id];
    switch (kind) {
    case WEBGL_BUFFER: p_glDeleteBuffers(1, &name); break;
    case WEBGL_FRAMEBUFFER: p_glDeleteFramebuffers(1, &name); break;
    case WEBGL_RENDERBUFFER: p_glDeleteRenderbuffers(1, &name); break;
    case WEBGL_TEXTURE: p_glDeleteTextures(1, &name); break;
    case WEBGL_SHADER: p_glDeleteShader(name); break;
    case WEBGL_PROGRAM: p_glDeleteProgram(name); break;
    case WEBGL_VERTEX_ARRAY: p_glDeleteVertexArrays(1, &name); break;
    case WEBGL_QUERY: p_glDeleteQueries(1, &name); break;
    case WEBGL_SAMPLER: p_glDeleteSamplers(1, &name); break;
    case WEBGL_TRANSFORM_FEEDBACK: p_glDeleteTransformFeedbacks(1, &name); break;
    }
    t[id] = 0;
}

static GLboolean is_object(WebglCtx *c, uint32_t kind, uint32_t id) {
    if (kind == WEBGL_SYNC) return id && id < WEBGL_MAX_IDS && c->syncs && c->syncs[id];
    if (kind >= WEBGL_KINDS || kind == WEBGL_LOCATION) return 0;
    GLuint n = webgl_name(c, (int)kind, id);
    if (!n || n == WEBGL_NO_NAME) return 0;
    switch (kind) {
    case WEBGL_BUFFER: return p_glIsBuffer(n);
    case WEBGL_FRAMEBUFFER: return p_glIsFramebuffer(n);
    case WEBGL_RENDERBUFFER: return p_glIsRenderbuffer(n);
    case WEBGL_TEXTURE: return p_glIsTexture(n);
    case WEBGL_SHADER: return p_glIsShader(n);
    case WEBGL_PROGRAM: return p_glIsProgram(n);
    case WEBGL_VERTEX_ARRAY: return p_glIsVertexArray(n);
    case WEBGL_QUERY: return p_glIsQuery(n);
    case WEBGL_SAMPLER: return p_glIsSampler(n);
    case WEBGL_TRANSFORM_FEEDBACK: return p_glIsTransformFeedback(n);
    }
    return 0;
}

static GLsync sync_of(WebglCtx *c, uint32_t id) {
    return id && id < WEBGL_MAX_IDS && c->syncs ? c->syncs[id] : NULL;
}

/* An info log, whole. */
static void put_log(Out *o, GLuint obj, bool program) {
    GLint n = 0;
    if (program) p_glGetProgramiv(obj, GL_INFO_LOG_LENGTH, &n);
    else p_glGetShaderiv(obj, GL_INFO_LOG_LENGTH, &n);
    if (n < 0 || n > (1 << 20)) n = 0;
    char *log = (char *)calloc(1, (size_t)n + 1);
    if (log && n) {
        if (program) p_glGetProgramInfoLog(obj, n, NULL, log);
        else p_glGetShaderInfoLog(obj, n, NULL, log);
    }
    put_str(o, log);
    free(log);
}

/* glGetActive{Attrib,Uniform} and the varying: size, type, name. */
static void put_active(Out *o, int which, GLuint prog, GLuint index) {
    char name[1024] = "";
    GLsizei len = 0;
    GLint size = 0;
    GLenum type = 0;
    if (which == 0) p_glGetActiveAttrib(prog, index, sizeof(name), &len, &size, &type, name);
    else if (which == 1) p_glGetActiveUniform(prog, index, sizeof(name), &len, &size, &type, name);
    else {
        GLsizei s = 0;
        p_glGetTransformFeedbackVarying(prog, index, sizeof(name), &len, &s, &type, name);
        size = s;
    }
    name[sizeof(name) - 1] = 0;
    put_u32(o, (uint32_t)size);
    put_u32(o, type);
    put_str(o, type ? name : "");
}

/* Bytes a texture upload of w x h x d of format/type may read: the tab
 * sends what it has; ANGLE is told that size, and refuses short data. */
static bool run_special(WebglCtx *c, uint32_t op, const uint8_t *p, uint32_t n, Out *o) {
#define NEED(k) if (n < (k)) return false
    switch (op) {
    case OP_CREATE: NEED(12); create(c, rd_u32(p), rd_u32(p + 4), rd_u32(p + 8)); return true;
    case OP_DELETE: NEED(8); destroy(c, rd_u32(p), rd_u32(p + 4)); return true;
    case OP_SHADER_SOURCE: {
        NEED(4);
        char *s = str(p + 4, n - 4);
        if (!s) return false;
        const GLchar *srcs[1] = { s };
        p_glShaderSource(webgl_name(c, WEBGL_SHADER, rd_u32(p)), 1, srcs, NULL);
        free(s);
        return true;
    }
    case OP_BIND_ATTRIB_LOCATION: {
        NEED(8);
        char *s = str(p + 8, n - 8);
        if (!s) return false;
        p_glBindAttribLocation(webgl_name(c, WEBGL_PROGRAM, rd_u32(p)), rd_u32(p + 4), s);
        free(s);
        return true;
    }
    case OP_BUFFER_DATA: {              /* target, usage, size (i64), has data; then the data */
        NEED(20);
        int64_t size = rd_i64(p + 8);
        bool has = rd_u32(p + 16) != 0;
        if (has && (size < 0 || (uint64_t)size != (uint64_t)(n - 20))) return false;
        p_glBufferData(rd_u32(p), (GLsizeiptr)size, has ? p + 20 : NULL, rd_u32(p + 4));
        return true;
    }
    case OP_BUFFER_SUB_DATA:            /* target, offset (i64); then the data */
        NEED(12);
        p_glBufferSubData(rd_u32(p), (GLintptr)rd_i64(p + 4), (GLsizeiptr)(n - 12), p + 12);
        return true;
    case OP_TEX_IMAGE_2D:               /* target, level, internal, w, h, border, format, type, has; data */
        NEED(36);
        p_glTexImage2DRobustANGLE(rd_u32(p), rd_i32(p + 4), rd_i32(p + 8), rd_i32(p + 12), rd_i32(p + 16),
                                  rd_i32(p + 20), rd_u32(p + 24), rd_u32(p + 28), (GLsizei)(n - 36),
                                  rd_u32(p + 32) ? p + 36 : NULL);
        return true;
    case OP_TEX_SUB_IMAGE_2D:           /* target, level, x, y, w, h, format, type; data */
        NEED(32);
        p_glTexSubImage2DRobustANGLE(rd_u32(p), rd_i32(p + 4), rd_i32(p + 8), rd_i32(p + 12), rd_i32(p + 16),
                                     rd_i32(p + 20), rd_u32(p + 24), rd_u32(p + 28), (GLsizei)(n - 32), p + 32);
        return true;
    case OP_COMPRESSED_TEX_IMAGE_2D:    /* target, level, internal, w, h, border; data */
        NEED(24);
        p_glCompressedTexImage2DRobustANGLE(rd_u32(p), rd_i32(p + 4), rd_u32(p + 8), rd_i32(p + 12), rd_i32(p + 16),
                                            rd_i32(p + 20), (GLsizei)(n - 24), (GLsizei)(n - 24), p + 24);
        return true;
    case OP_COMPRESSED_TEX_SUB_IMAGE_2D:    /* target, level, x, y, w, h, format; data */
        NEED(28);
        p_glCompressedTexSubImage2DRobustANGLE(rd_u32(p), rd_i32(p + 4), rd_i32(p + 8), rd_i32(p + 12), rd_i32(p + 16),
                                               rd_i32(p + 20), rd_u32(p + 24), (GLsizei)(n - 28), (GLsizei)(n - 28),
                                               p + 28);
        return true;
    case OP_TEX_IMAGE_3D:               /* target, level, internal, w, h, d, border, format, type, has; data */
        NEED(40);
        p_glTexImage3DRobustANGLE(rd_u32(p), rd_i32(p + 4), rd_i32(p + 8), rd_i32(p + 12), rd_i32(p + 16),
                                  rd_i32(p + 20), rd_i32(p + 24), rd_u32(p + 28), rd_u32(p + 32), (GLsizei)(n - 40),
                                  rd_u32(p + 36) ? p + 40 : NULL);
        return true;
    case OP_TEX_SUB_IMAGE_3D:           /* target, level, x, y, z, w, h, d, format, type; data */
        NEED(40);
        p_glTexSubImage3DRobustANGLE(rd_u32(p), rd_i32(p + 4), rd_i32(p + 8), rd_i32(p + 12), rd_i32(p + 16),
                                     rd_i32(p + 20), rd_i32(p + 24), rd_i32(p + 28), rd_u32(p + 32), rd_u32(p + 36),
                                     (GLsizei)(n - 40), p + 40);
        return true;
    case OP_UNIFORM_V: {                /* location, kind, transpose; values */
        NEED(12);
        uint32_t loc_id = rd_u32(p), kind = rd_u32(p + 4);
        GLboolean tr = rd_u32(p + 8) != 0;
        const void *v = p + 12;
        uint32_t bytes = n - 12;
        /* kind: 1..4 float vec, 5..8 int, 9..12 uint, 13.. matrices
         * (2, 3, 4, 2x3, 3x2, 2x4, 4x2, 3x4, 4x3). */
        static const uint8_t per[] = { 0, 1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4, 4, 9, 16, 6, 6, 8, 8, 12, 12 };
        if (kind == 0 || kind >= sizeof(per) || bytes % (4u * per[kind])) return false;
        if (!webgl_location_ok(c, loc_id)) return true;
        GLint loc = webgl_location(c, loc_id);
        GLsizei count = (GLsizei)(bytes / (4u * per[kind]));
        switch (kind) {
        case 1: p_glUniform1fv(loc, count, v); break;
        case 2: p_glUniform2fv(loc, count, v); break;
        case 3: p_glUniform3fv(loc, count, v); break;
        case 4: p_glUniform4fv(loc, count, v); break;
        case 5: p_glUniform1iv(loc, count, v); break;
        case 6: p_glUniform2iv(loc, count, v); break;
        case 7: p_glUniform3iv(loc, count, v); break;
        case 8: p_glUniform4iv(loc, count, v); break;
        case 9: p_glUniform1uiv(loc, count, v); break;
        case 10: p_glUniform2uiv(loc, count, v); break;
        case 11: p_glUniform3uiv(loc, count, v); break;
        case 12: p_glUniform4uiv(loc, count, v); break;
        case 13: p_glUniformMatrix2fv(loc, count, tr, v); break;
        case 14: p_glUniformMatrix3fv(loc, count, tr, v); break;
        case 15: p_glUniformMatrix4fv(loc, count, tr, v); break;
        case 16: p_glUniformMatrix2x3fv(loc, count, tr, v); break;
        case 17: p_glUniformMatrix3x2fv(loc, count, tr, v); break;
        case 18: p_glUniformMatrix2x4fv(loc, count, tr, v); break;
        case 19: p_glUniformMatrix4x2fv(loc, count, tr, v); break;
        case 20: p_glUniformMatrix3x4fv(loc, count, tr, v); break;
        case 21: p_glUniformMatrix4x3fv(loc, count, tr, v); break;
        }
        return true;
    }
    case OP_VERTEX_ATTRIB_V: {          /* index, kind (1..4 float, 5 int4, 6 uint4); values */
        NEED(8);
        uint32_t idx = rd_u32(p), kind = rd_u32(p + 4);
        static const uint8_t per[] = { 0, 1, 2, 3, 4, 4, 4 };
        if (kind == 0 || kind > 6 || n - 8 < 4u * per[kind]) return false;
        float v[4];
        memcpy(v, p + 8, 4u * per[kind]);
        switch (kind) {
        case 1: p_glVertexAttrib1fv(idx, v); break;
        case 2: p_glVertexAttrib2fv(idx, v); break;
        case 3: p_glVertexAttrib3fv(idx, v); break;
        case 4: p_glVertexAttrib4fv(idx, v); break;
        case 5: p_glVertexAttribI4iv(idx, (const GLint *)v); break;
        case 6: p_glVertexAttribI4uiv(idx, (const GLuint *)v); break;
        }
        return true;
    }
    case OP_DRAW_BUFFERS:
        if (n % 4 || n > 64) return false;
        p_glDrawBuffers((GLsizei)(n / 4), (const GLenum *)p);
        return true;
    case OP_CLEAR_BUFFER_V:             /* buffer, drawbuffer, kind (0 f, 1 i, 2 ui); 4 values */
        NEED(28);
        if (rd_u32(p + 8) == 0) p_glClearBufferfv(rd_u32(p), rd_i32(p + 4), (const GLfloat *)(p + 12));
        else if (rd_u32(p + 8) == 1) p_glClearBufferiv(rd_u32(p), rd_i32(p + 4), (const GLint *)(p + 12));
        else p_glClearBufferuiv(rd_u32(p), rd_i32(p + 4), (const GLuint *)(p + 12));
        return true;
    case OP_INVALIDATE_FRAMEBUFFER:     /* target; attachments */
        NEED(4);
        if ((n - 4) % 4 || n - 4 > 64) return false;
        p_glInvalidateFramebuffer(rd_u32(p), (GLsizei)((n - 4) / 4), (const GLenum *)(p + 4));
        return true;
    case OP_TRANSFORM_FEEDBACK_VARYINGS: {  /* program, mode; names, NUL-separated */
        NEED(8);
        char *s = (char *)malloc(n - 8 + 1);
        if (!s) return false;
        memcpy(s, p + 8, n - 8);
        s[n - 8] = 0;
        const GLchar *names[64];
        GLsizei k = 0;
        for (char *q = s; q < s + (n - 8) && k < 64; q += strlen(q) + 1) names[k++] = q;
        p_glTransformFeedbackVaryings(webgl_name(c, WEBGL_PROGRAM, rd_u32(p)), k, names, rd_u32(p + 4));
        free(s);
        return true;
    }
    case OP_PIXEL_STOREI: NEED(8); p_glPixelStorei(rd_u32(p), rd_i32(p + 4)); return true;
    case OP_FENCE_SYNC: {               /* id, condition, flags */
        NEED(12);
        uint32_t id = rd_u32(p);
        if (!id || id >= WEBGL_MAX_IDS) return true;
        if (!c->syncs) c->syncs = (GLsync *)calloc(WEBGL_MAX_IDS, sizeof(GLsync));
        if (!c->syncs || c->syncs[id]) return true;
        c->syncs[id] = p_glFenceSync(rd_u32(p + 4), rd_u32(p + 8));
        return true;
    }
    case OP_WAIT_SYNC:                  /* id, flags, timeout (u64) */
        NEED(16);
        if (sync_of(c, rd_u32(p))) p_glWaitSync(sync_of(c, rd_u32(p)), rd_u32(p + 4), (GLuint64)rd_i64(p + 8));
        return true;

    case OP_GET_ERROR: {
        GLenum e = c->pending_error ? c->pending_error : p_glGetError();
        c->pending_error = 0;
        put_u32(o, e);
        return true;
    }
    case OP_GET_INTEGER: case OP_GET_FLOAT: case OP_GET_BOOLEAN: {   /* pname, count */
        NEED(8);
        uint32_t count = rd_u32(p + 4);
        if (count > 16) return false;
        int32_t v[16] = { 0 };
        if (op == OP_GET_INTEGER) p_glGetIntegervRobustANGLE(rd_u32(p), (GLsizei)count, NULL, (GLint *)v);
        else if (op == OP_GET_FLOAT) p_glGetFloatvRobustANGLE(rd_u32(p), (GLsizei)count, NULL, (GLfloat *)v);
        else {
            GLboolean b[16] = { 0 };
            p_glGetBooleanvRobustANGLE(rd_u32(p), (GLsizei)count, NULL, b);
            for (uint32_t k = 0; k < count; k++) v[k] = b[k];
        }
        put(o, v, count * 4);
        return true;
    }
    case OP_GET_STRING: NEED(4); put_str(o, (const char *)p_glGetString(rd_u32(p))); return true;
    case OP_GET_SHADER_PARAM: case OP_GET_PROGRAM_PARAM: {      /* object, pname */
        NEED(8);
        GLint v = 0;
        if (op == OP_GET_SHADER_PARAM) p_glGetShaderiv(webgl_name(c, WEBGL_SHADER, rd_u32(p)), rd_u32(p + 4), &v);
        else p_glGetProgramiv(webgl_name(c, WEBGL_PROGRAM, rd_u32(p)), rd_u32(p + 4), &v);
        put_u32(o, (uint32_t)v);
        return true;
    }
    case OP_GET_SHADER_INFO_LOG: NEED(4); put_log(o, webgl_name(c, WEBGL_SHADER, rd_u32(p)), false); return true;
    case OP_GET_PROGRAM_INFO_LOG: NEED(4); put_log(o, webgl_name(c, WEBGL_PROGRAM, rd_u32(p)), true); return true;
    case OP_GET_UNIFORM_LOCATION: {     /* program, the id to give it; name */
        NEED(8);
        char *s = str(p + 8, n - 8);
        if (!s) return false;
        GLuint prog = webgl_name(c, WEBGL_PROGRAM, rd_u32(p));
        GLint loc = p_glGetUniformLocation(prog, s);
        free(s);
        uint32_t id = rd_u32(p + 4);
        if (loc >= 0 && id && id < WEBGL_MAX_IDS) {
            if (!c->locations) c->locations = (WebglLocation *)calloc(WEBGL_MAX_IDS, sizeof(WebglLocation));
            if (c->locations) { c->locations[id].program = prog; c->locations[id].location = loc; }
        }
        put_u32(o, loc >= 0);
        return true;
    }
    case OP_GET_ATTRIB_LOCATION: case OP_GET_FRAG_DATA_LOCATION: {  /* program; name */
        NEED(4);
        char *s = str(p + 4, n - 4);
        if (!s) return false;
        GLuint prog = webgl_name(c, WEBGL_PROGRAM, rd_u32(p));
        put_u32(o, (uint32_t)(op == OP_GET_ATTRIB_LOCATION ? p_glGetAttribLocation(prog, s)
                                                            : p_glGetFragDataLocation(prog, s)));
        free(s);
        return true;
    }
    case OP_GET_ACTIVE_ATTRIB: case OP_GET_ACTIVE_UNIFORM: case OP_GET_TRANSFORM_FEEDBACK_VARYING:
        NEED(8);
        put_active(o, op == OP_GET_ACTIVE_ATTRIB ? 0 : op == OP_GET_ACTIVE_UNIFORM ? 1 : 2,
                   webgl_name(c, WEBGL_PROGRAM, rd_u32(p)), rd_u32(p + 4));
        return true;
    case OP_READ_PIXELS: {              /* x, y, w, h, format, type, bytes wanted */
        NEED(28);
        uint32_t want = rd_u32(p + 24);
        if (want > (256u << 20)) return false;
        uint8_t *px = (uint8_t *)calloc(1, want ? want : 1);
        if (!px) return false;
        p_glReadPixelsRobustANGLE(rd_i32(p), rd_i32(p + 4), rd_i32(p + 8), rd_i32(p + 12), rd_u32(p + 16),
                                  rd_u32(p + 20), (GLsizei)want, NULL, NULL, NULL, px);
        put_bytes(o, px, want);
        free(px);
        return true;
    }
    case OP_CHECK_FRAMEBUFFER_STATUS: NEED(4); put_u32(o, p_glCheckFramebufferStatus(rd_u32(p))); return true;
    case OP_IS_OBJECT: NEED(8); put_u32(o, is_object(c, rd_u32(p), rd_u32(p + 4))); return true;
    case OP_GET_TEX_PARAM: case OP_GET_BUFFER_PARAM: case OP_GET_RENDERBUFFER_PARAM: {  /* target, pname, float? */
        NEED(12);
        int32_t v = 0;
        if (op == OP_GET_TEX_PARAM && rd_u32(p + 8))
            p_glGetTexParameterfvRobustANGLE(rd_u32(p), rd_u32(p + 4), 1, NULL, (GLfloat *)&v);
        else if (op == OP_GET_TEX_PARAM) p_glGetTexParameterivRobustANGLE(rd_u32(p), rd_u32(p + 4), 1, NULL, &v);
        else if (op == OP_GET_BUFFER_PARAM) p_glGetBufferParameterivRobustANGLE(rd_u32(p), rd_u32(p + 4), 1, NULL, &v);
        else p_glGetRenderbufferParameterivRobustANGLE(rd_u32(p), rd_u32(p + 4), 1, NULL, &v);
        put_u32(o, (uint32_t)v);
        return true;
    }
    case OP_GET_FRAMEBUFFER_ATTACHMENT_PARAM: {     /* target, attachment, pname */
        NEED(12);
        GLint v = 0;
        p_glGetFramebufferAttachmentParameterivRobustANGLE(rd_u32(p), rd_u32(p + 4), rd_u32(p + 8), 1, NULL, &v);
        put_u32(o, (uint32_t)v);
        return true;
    }
    case OP_GET_SHADER_PRECISION: {     /* shader type, precision type */
        NEED(8);
        GLint range[2] = { 0, 0 }, prec = 0;
        p_glGetShaderPrecisionFormat(rd_u32(p), rd_u32(p + 4), range, &prec);
        put_u32(o, (uint32_t)range[0]);
        put_u32(o, (uint32_t)range[1]);
        put_u32(o, (uint32_t)prec);
        return true;
    }
    case OP_GET_UNIFORM: {              /* program, location id, as (0 f, 1 i, 2 ui), count */
        NEED(16);
        uint32_t count = rd_u32(p + 12), as = rd_u32(p + 8);
        if (count > 16) return false;
        int32_t v[16] = { 0 };
        GLuint prog = webgl_name(c, WEBGL_PROGRAM, rd_u32(p));
        GLint loc = webgl_location(c, rd_u32(p + 4));
        if (as == 0) p_glGetUniformfvRobustANGLE(prog, loc, (GLsizei)(count * 4), NULL, (GLfloat *)v);
        else if (as == 1) p_glGetUniformivRobustANGLE(prog, loc, (GLsizei)(count * 4), NULL, v);
        else p_glGetUniformuivRobustANGLE(prog, loc, (GLsizei)(count * 4), NULL, (GLuint *)v);
        put(o, v, count * 4);
        return true;
    }
    case OP_GET_VERTEX_ATTRIB: {        /* index, pname, as (0 f, 1 i, 2 ui), count */
        NEED(16);
        uint32_t count = rd_u32(p + 12), as = rd_u32(p + 8);
        if (count > 4) return false;
        int32_t v[4] = { 0 };
        if (as == 0) p_glGetVertexAttribfvRobustANGLE(rd_u32(p), rd_u32(p + 4), (GLsizei)count, NULL, (GLfloat *)v);
        else if (as == 1) p_glGetVertexAttribivRobustANGLE(rd_u32(p), rd_u32(p + 4), (GLsizei)count, NULL, v);
        else p_glGetVertexAttribIuivRobustANGLE(rd_u32(p), rd_u32(p + 4), (GLsizei)count, NULL, (GLuint *)v);
        put(o, v, count * 4);
        return true;
    }
    case OP_GET_VERTEX_ATTRIB_OFFSET: {     /* index, pname */
        NEED(8);
        void *ptr = NULL;
        p_glGetVertexAttribPointervRobustANGLE(rd_u32(p), rd_u32(p + 4), 1, NULL, &ptr);
        uint64_t off = (uint64_t)(uintptr_t)ptr;
        put(o, &off, 8);
        return true;
    }
    case OP_GET_QUERY_PARAM: {          /* query, pname */
        NEED(8);
        GLuint v = 0;
        p_glGetQueryObjectuivRobustANGLE(webgl_name(c, WEBGL_QUERY, rd_u32(p)), rd_u32(p + 4), 1, NULL, &v);
        put_u32(o, v);
        return true;
    }
    case OP_GET_SAMPLER_PARAM: {        /* sampler, pname, float? */
        NEED(12);
        int32_t v = 0;
        if (rd_u32(p + 8))
            p_glGetSamplerParameterfvRobustANGLE(webgl_name(c, WEBGL_SAMPLER, rd_u32(p)), rd_u32(p + 4), 1, NULL,
                                                 (GLfloat *)&v);
        else p_glGetSamplerParameterivRobustANGLE(webgl_name(c, WEBGL_SAMPLER, rd_u32(p)), rd_u32(p + 4), 1, NULL, &v);
        put_u32(o, (uint32_t)v);
        return true;
    }
    case OP_GET_SYNC_PARAM: {           /* sync, pname */
        NEED(8);
        GLint v = 0;
        GLsync s = sync_of(c, rd_u32(p));
        if (s) p_glGetSynciv(s, rd_u32(p + 4), 1, NULL, &v);
        put_u32(o, (uint32_t)v);
        return true;
    }
    case OP_CLIENT_WAIT_SYNC: {         /* sync, flags, timeout (WebGL caps it at 0 here: no blocking) */
        NEED(16);
        GLsync s = sync_of(c, rd_u32(p));
        put_u32(o, s ? p_glClientWaitSync(s, rd_u32(p + 4), 0) : 0);
        return true;
    }
    case OP_GET_BUFFER_SUB_DATA: {      /* target, offset (i64), length */
        NEED(16);
        uint32_t len = rd_u32(p + 12);
        if (len > (256u << 20)) return false;
        void *m = len ? p_glMapBufferRange(rd_u32(p), (GLintptr)rd_i64(p + 4), (GLsizeiptr)len, GL_MAP_READ_BIT) : NULL;
        if (m) {
            put_bytes(o, m, len);
            p_glUnmapBuffer(rd_u32(p));
        } else {
            put_u32(o, 0);
        }
        return true;
    }
    case OP_GET_INDEXED: {              /* target, index, 64? */
        NEED(12);
        if (rd_u32(p + 8)) {
            long long v = 0;
            GLint tmp[2] = { 0, 0 };
            p_glGetIntegeri_vRobustANGLE(rd_u32(p), rd_u32(p + 4), 2, NULL, tmp);
            v = tmp[0];
            put(o, &v, 8);
        } else {
            GLint v = 0;
            p_glGetIntegeri_vRobustANGLE(rd_u32(p), rd_u32(p + 4), 1, NULL, &v);
            put_u32(o, (uint32_t)v);
        }
        return true;
    }
    case OP_GET_UNIFORM_BLOCK_INDEX: {  /* program; name */
        NEED(4);
        char *s = str(p + 4, n - 4);
        if (!s) return false;
        put_u32(o, p_glGetUniformBlockIndex(webgl_name(c, WEBGL_PROGRAM, rd_u32(p)), s));
        free(s);
        return true;
    }
    case OP_GET_ACTIVE_UNIFORM_BLOCK_PARAM: {   /* program, index, pname, count */
        NEED(16);
        uint32_t count = rd_u32(p + 12);
        if (count > 256) return false;
        GLint v[256];
        memset(v, 0, sizeof(v));
        p_glGetActiveUniformBlockivRobustANGLE(webgl_name(c, WEBGL_PROGRAM, rd_u32(p)), rd_u32(p + 4), rd_u32(p + 8),
                                               (GLsizei)count, NULL, v);
        put(o, v, count * 4);
        return true;
    }
    case OP_GET_ACTIVE_UNIFORM_BLOCK_NAME: {    /* program, index */
        NEED(8);
        char name[1024] = "";
        GLsizei len = 0;
        p_glGetActiveUniformBlockName(webgl_name(c, WEBGL_PROGRAM, rd_u32(p)), rd_u32(p + 4), sizeof(name), &len, name);
        name[sizeof(name) - 1] = 0;
        name[len] = 0;
        put_str(o, name);
        return true;
    }
    case OP_GET_UNIFORM_INDICES: {      /* program; names, NUL-separated */
        NEED(4);
        char *s = (char *)malloc(n - 4 + 1);
        if (!s) return false;
        memcpy(s, p + 4, n - 4);
        s[n - 4] = 0;
        const GLchar *names[256];
        GLuint idx[256];
        GLsizei k = 0;
        for (char *q = s; q < s + (n - 4) && k < 256; q += strlen(q) + 1) names[k++] = q;
        p_glGetUniformIndices(webgl_name(c, WEBGL_PROGRAM, rd_u32(p)), k, names, idx);
        put_u32(o, (uint32_t)k);
        put(o, idx, (uint32_t)k * 4);
        free(s);
        return true;
    }
    case OP_GET_ACTIVE_UNIFORMS: {      /* program, pname; indices */
        NEED(8);
        if ((n - 8) % 4 || n - 8 > 1024) return false;
        GLsizei k = (GLsizei)((n - 8) / 4);
        GLint v[256];
        memset(v, 0, sizeof(v));
        p_glGetActiveUniformsiv(webgl_name(c, WEBGL_PROGRAM, rd_u32(p)), k, (const GLuint *)(p + 8), rd_u32(p + 4), v);
        put(o, v, (uint32_t)k * 4);
        return true;
    }
    case OP_GET_INTERNALFORMAT_PARAM: {     /* target, internal format, pname, count */
        NEED(16);
        uint32_t count = rd_u32(p + 12);
        if (count > 64) return false;
        GLint v[64];
        memset(v, 0, sizeof(v));
        p_glGetInternalformativRobustANGLE(rd_u32(p), rd_u32(p + 4), rd_u32(p + 8), (GLsizei)count, NULL, v);
        put(o, v, count * 4);
        return true;
    }
    case OP_REQUEST_EXTENSION: {        /* name: enabled if ANGLE offers it */
        char *s = str(p, n);
        if (!s) return false;
        const char *req = (const char *)p_glGetString(GL_REQUESTABLE_EXTENSIONS_ANGLE);
        const char *have = (const char *)p_glGetString(GL_EXTENSIONS);
        size_t sl = strlen(s);
        bool ok = false;
        for (const char *list = req; list && *list && !ok; ) {
            const char *e = strchr(list, ' ');
            size_t l = e ? (size_t)(e - list) : strlen(list);
            if (l == sl && !memcmp(list, s, l)) ok = true;
            list = e ? e + 1 : list + l;
        }
        if (ok) p_glRequestExtensionANGLE(s);
        bool on = false;
        have = (const char *)p_glGetString(GL_EXTENSIONS);
        for (const char *list = have; list && *list && !on; ) {
            const char *e = strchr(list, ' ');
            size_t l = e ? (size_t)(e - list) : strlen(list);
            if (l == sl && !memcmp(list, s, l)) on = true;
            list = e ? e + 1 : list + l;
        }
        free(s);
        put_u32(o, on);
        return true;
    }
    case OP_GET_EXTENSIONS: {           /* what is on, and what may be asked for */
        const char *have = (const char *)p_glGetString(GL_EXTENSIONS);
        const char *req = (const char *)p_glGetString(GL_REQUESTABLE_EXTENSIONS_ANGLE);
        put_str(o, have);
        put_str(o, req);
        return true;
    }
    case OP_GET_RESET_STATUS: {
        GLenum s = p_glGetGraphicsResetStatusEXT();
        if (s) c->lost = true;
        put_u32(o, s);
        return true;
    }
    }
    return false;
#undef NEED
}

bool webgl_run(WebglCtx *c, const uint8_t *recs, uint32_t len, uint8_t **out, uint32_t *out_len, uint32_t *out_cap) {
    if (!make_current(c)) return false;
    Out o = { out, out_len, out_cap, false };
    for (uint32_t at = 0; at + 8 <= len; ) {
        uint32_t op = rd_u32(recs + at), n = rd_u32(recs + at + 4);
        if (n > len - at - 8) return false;
        const uint8_t *p = recs + at + 8;
        bool ok = op < WEBGL_OP_SPECIAL ? webgl_exec_plain(c, op, p, n) : run_special(c, op, p, n, &o);
        if (!ok || o.failed) return false;
        at += 8 + n;
    }
    return true;
}

bool webgl_read_frame(WebglCtx *c, uint8_t *px) {
    if (!make_current(c)) return false;
    /* The default framebuffer, leaving the page's bindings and packing as
     * they were. */
    GLint read_fb = 0, pack = 4, pack_buf = 0, row = 0, skip_rows = 0, skip_px = 0, read_buf = 0;
    GLenum fb_target = c->version == 2 ? GL_READ_FRAMEBUFFER : GL_FRAMEBUFFER;
    p_glGetIntegervRobustANGLE(c->version == 2 ? GL_READ_FRAMEBUFFER_BINDING : GL_FRAMEBUFFER_BINDING, 1, NULL, &read_fb);
    p_glGetIntegervRobustANGLE(GL_PACK_ALIGNMENT, 1, NULL, &pack);
    if (c->version == 2) {
        p_glGetIntegervRobustANGLE(GL_PIXEL_PACK_BUFFER_BINDING, 1, NULL, &pack_buf);
        p_glGetIntegervRobustANGLE(GL_PACK_ROW_LENGTH, 1, NULL, &row);
        p_glGetIntegervRobustANGLE(GL_PACK_SKIP_ROWS, 1, NULL, &skip_rows);
        p_glGetIntegervRobustANGLE(GL_PACK_SKIP_PIXELS, 1, NULL, &skip_px);
    }
    gl.glBindFramebuffer(fb_target, 0);
    if (c->version == 2) {
        p_glGetIntegervRobustANGLE(GL_READ_BUFFER, 1, NULL, &read_buf);
        gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        p_glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        p_glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        p_glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    }
    p_glPixelStorei(GL_PACK_ALIGNMENT, 4);
    uint32_t w = c->width, h = c->height, stride = w * 4;
    uint8_t *tmp = (uint8_t *)malloc((size_t)stride * h);
    bool ok = tmp != NULL;
    if (ok) {
        p_glReadPixelsRobustANGLE(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, (GLsizei)(stride * h), NULL,
                                  NULL, NULL, tmp);
        /* GL's rows go up; a frame's go down. */
        for (uint32_t y = 0; y < h; y++) memcpy(px + (size_t)y * stride, tmp + (size_t)(h - 1 - y) * stride, stride);
        if (!c->alpha)
            for (size_t i = 3; i < (size_t)stride * h; i += 4) px[i] = 255;
        free(tmp);
    }
    p_glPixelStorei(GL_PACK_ALIGNMENT, pack);
    if (c->version == 2) {
        gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack_buf);
        p_glPixelStorei(GL_PACK_ROW_LENGTH, row);
        p_glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);
        p_glPixelStorei(GL_PACK_SKIP_PIXELS, skip_px);
    }
    gl.glBindFramebuffer(fb_target, (GLuint)read_fb);
    return ok;
}

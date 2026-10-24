/*
 * quickjs_jit.c - the baseline JIT (JIT_DESIGN.md): copy-and-patch
 * compilation of a function's bytecode into native code built from the
 * stencils in jit_stencils_x86_64_windows.h (made by tools/jit_stencils.js
 * from quickjs_jit_ops.c).
 *
 * Compiled code keeps the interpreter's frame (JSInterpFrame): every
 * bytecode instruction has native code, any of them can be entered (the
 * interpreter does so at a call, after catching an exception, when resuming
 * a generator and on a hot loop's back edge), and compiled code returns to
 * the interpreter at any instruction boundary.  Instructions without a fast
 * path run through their helper (quickjs_jsops.inc), the interpreter's own
 * code.
 *
 * Code lives in one region reserved near the executable image, so stencils
 * reach helpers and globals with 32-bit displacements (through a veneer
 * when they do not).  No page is mapped writable and executable at once:
 * the region is a section mapped twice, executable (where code runs) and
 * writable (where the JIT writes it), so compiling changes no protection.
 * Unwind data for the region is supplied by a callback, so SEH and the
 * sampling profiler walk through compiled frames.
 */
#include "quickjs_gc_unified.h"
#include "quickjs.h"
#include "quickjs-internal.h"
#include "cyber_env.h"
#include "quickjs_jit.h"
#include "quickjs_props_inline.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && defined(__x86_64__)
#define JIT_SUPPORTED 1
#include <windows.h>
#else
#define JIT_SUPPORTED 0
#endif

#if JIT_SUPPORTED

/* --- Stencil tables ------------------------------------------------------ */

enum {
    JIT_HOLE_END,
    JIT_HOLE_IMM,       /* literal: the instruction's immediate n */
    JIT_HOLE_ADDR_LIT,  /* literal: the address of symbol n */
    JIT_HOLE_BRANCH,    /* jump: continue, target, slow, exit, dyn */
    JIT_HOLE_HELPER,    /* call: the instruction's helper */
    JIT_HOLE_CALL,      /* call: symbol n */
    JIT_HOLE_DATA,      /* read-only data n */
};
enum { JIT_BR_CONTINUE, JIT_BR_TARGET, JIT_BR_SLOW, JIT_BR_EXIT, JIT_BR_DYN };

typedef struct JitHole {
    uint16_t offset;
    uint8_t kind;
    uint8_t arg;
    int32_t addend;
    uint8_t extra;      /* REL32_k: bytes between the field's end and the instruction's */
    uint8_t pre;        /* bytes of the instruction before the field (0: unknown) */
} JitHole;

typedef struct JitUnwind {
    uint32_t begin, end, info;
} JitUnwind;

typedef struct JitStencil {
    const char *name;
    const uint8_t *code;
    uint32_t size;
    const JitHole *holes;
    uint8_t tail_continue;  /* ends in `jmp continue`, cut when the next stencil follows */
    const uint8_t *xdata;
    uint32_t xdata_size;
    const JitUnwind *unwind;
    uint32_t n_unwind;
} JitStencil;

typedef struct JitData {
    const uint8_t *data;
    uint32_t size;
} JitData;

#include "jit_stencils_x86_64_windows.h"

typedef int JitFn(JSInterpFrame *F, GCValue *sp, GCValue *var_buf, GCValue *arg_buf);

/* The symbols stencils use.  g_interrupt_countdown is quickjs.c's. */
extern uint32_t g_interrupt_countdown;
void *jit_lookup_pc(JSInterpFrame *F);
#define JIT_SYM_ADDR(s) [JIT_SYM_##s] = (void *)&s,
static void *const jit_sym_addr[JIT_SYM_COUNT] = { JIT_SYMBOLS(JIT_SYM_ADDR) };

/* clang (stencils) and gcc (this file) must agree on these. */
#define JIT_LAYOUT_CHECK(expr, val) _Static_assert((expr) == (val), "stencil layout: " #expr);
JIT_LAYOUT(JIT_LAYOUT_CHECK)

/* --- Opcodes ------------------------------------------------------------- */

typedef struct JitOpInfo {
    uint8_t size;
    uint8_t fmt;
} JitOpInfo;

enum {
#define FMT(f) JIT_FMT_##f,
#define DEF(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef FMT
#undef DEF
};

/* The opcodes, numbered as quickjs.c's OPCodeEnum (which is private to it). */
enum {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f) OP_##id,
#define def(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef FMT
#undef DEF
#undef def
};

static const JitOpInfo jit_op_info[256] = {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f) [OP_##id] = { size, JIT_FMT_##f },
#define def(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef FMT
#undef DEF
#undef def
};

static const char *const jit_op_name[256] = {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f) [OP_##id] = #id,
#define def(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef FMT
#undef DEF
#undef def
};

static inline uint32_t jit_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t jit_u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* --- The code region ----------------------------------------------------- */

#define JIT_REGION_SIZE   ((size_t)256 << 20)
#define JIT_MAX_UNWIND    (1u << 21)     /* RUNTIME_FUNCTION entries */
#define JIT_MAX_FUNC_LEN  32768          /* bytecode bytes compiled per function */
#define JIT_VENEER_AREA   ((size_t)64 << 10)

static struct {
    int state;                  /* 0: not set up, 1: ready, -1: unavailable */
    uint8_t *base;              /* the region, executable view */
    uint8_t *rw;                /* the same memory, writable view */
    size_t used, committed;
    uint8_t *stencil_xdata[JIT_ST_COUNT];  /* each stencil's unwind info, in the region */
    uint8_t *data[sizeof(jit_datas) / sizeof(jit_datas[0]) + 1];
    uint8_t *exit, *dyn, *trampoline;
    RUNTIME_FUNCTION *unwind;   /* sorted by address; JIT_MAX_UNWIND reserved */
    volatile LONG n_unwind;
    size_t unwind_committed;
    /* Veneers: jmp [rip] to an address out of rel32 range. */
    struct { void *target; uint8_t *veneer; } *veneers;
    int n_veneers, cap_veneers;
    uint8_t *veneer_area;       /* pages of their own: flipping them touches nothing else */
    size_t veneer_used;
    uint64_t compiled_bytes, compiled_funcs, failed_funcs;
    uint64_t compile_ticks, bytecode_bytes;
    FILE *map;                  /* CYBER_SAMPLE: <path>.jit, what is where */
} g_jit;

/* For the sampling profiler (cyber_sampler.c): while set, the JIT holds a
 * lock the profiler's unwinding needs. */
volatile LONG g_jit_os_busy;

/* For the sampling profiler (cyber_sampler.c). */
void jit_region_bounds(uintptr_t *base, size_t *size)
{
    *base = (uintptr_t)g_jit.base;
    *size = g_jit.state > 0 ? JIT_REGION_SIZE : 0;
}

static void jit_map_close(void)
{
    if (g_jit.map) fclose(g_jit.map);
    g_jit.map = NULL;
}

/* The map the sample report reads: offsets in the region of each function,
 * and of each instruction's code (I) and out-of-line generic copy (S). */
static void jit_map_open(void)
{
    const char *p = getenv("CYBER_SAMPLE");
    if (!p || !*p) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s.jit", p);
    g_jit.map = fopen(path, "w");
    if (g_jit.map) atexit(jit_map_close);
}

static uint64_t jit_ticks(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

/* QJS_JIT_LOG=1: statistics at exit and failures; 2: every compilation. */
static int jit_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("QJS_JIT_LOG");
        on = e ? atoi(e) : 0;
        if (e && on == 0) on = 1;
    }
    return on;
}

/* Where the JIT writes what runs at p. */
#define JIT_W(p) (g_jit.rw + ((uint8_t *)(p) - g_jit.base))

/* The region: a section reserved (committed as it fills), mapped
 * executable within 2 GB of this image, so rel32 reaches it, and writable
 * anywhere. */
static int jit_map_region(size_t size)
{
    HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE | SEC_RESERVE,
                                    (DWORD)((uint64_t)size >> 32), (DWORD)size, NULL);
    if (!sec) return -1;
    uintptr_t image = (uintptr_t)GetModuleHandleA(NULL);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    /* Try below the image first, then above it. */
    for (int dir = 0; dir < 2 && !g_jit.base; dir++) {
        for (uintptr_t d = (uintptr_t)64 << 20; d < ((uintptr_t)1900 << 20) && !g_jit.base; d += (uintptr_t)32 << 20) {
            uintptr_t want = dir == 0 ? image - d - size : image + d;
            want &= ~(gran - 1);
            g_jit.base = MapViewOfFileEx(sec, FILE_MAP_READ | FILE_MAP_EXECUTE, 0, 0, size, (void *)want);
        }
    }
    if (!g_jit.base)
        g_jit.base = MapViewOfFile(sec, FILE_MAP_READ | FILE_MAP_EXECUTE, 0, 0, size);
    g_jit.rw = MapViewOfFile(sec, FILE_MAP_WRITE, 0, 0, size);
    CloseHandle(sec);   /* the views keep the section */
    return g_jit.base && g_jit.rw ? 0 : -1;
}

static PRUNTIME_FUNCTION CALLBACK jit_unwind_callback(DWORD64 pc, PVOID context)
{
    (void)context;
    LONG n = g_jit.n_unwind;
    MemoryBarrier();
    DWORD rva = (DWORD)(pc - (DWORD64)(uintptr_t)g_jit.base);
    LONG lo = 0, hi = n - 1;
    while (lo <= hi) {
        LONG mid = (lo + hi) / 2;
        RUNTIME_FUNCTION *rf = &g_jit.unwind[mid];
        if (rva < rf->BeginAddress) hi = mid - 1;
        else if (rva >= rf->EndAddress) lo = mid + 1;
        else return rf;
    }
    return NULL;    /* a leaf: the return address is at rsp */
}

static int jit_add_unwind(uint8_t *at, const JitStencil *st, uint32_t emitted_size, int sid)
{
    for (uint32_t i = 0; i < st->n_unwind; i++) {
        const JitUnwind *u = &st->unwind[i];
        uint32_t end = u->end < emitted_size ? u->end : emitted_size;
        if (u->begin >= end) continue;
        LONG n = g_jit.n_unwind;
        if ((uint32_t)n >= JIT_MAX_UNWIND) return -1;
        size_t need = ((size_t)n + 1) * sizeof(RUNTIME_FUNCTION);
        if (need > g_jit.unwind_committed) {
            size_t c = g_jit.unwind_committed + (64 << 10);
            if (!VirtualAlloc(g_jit.unwind, c, MEM_COMMIT, PAGE_READWRITE)) return -1;
            g_jit.unwind_committed = c;
        }
        RUNTIME_FUNCTION *rf = &g_jit.unwind[n];
        rf->BeginAddress = (DWORD)(at - g_jit.base) + u->begin;
        rf->EndAddress = (DWORD)(at - g_jit.base) + end;
        rf->UnwindData = (DWORD)(g_jit.stencil_xdata[sid] - g_jit.base) + u->info;
        MemoryBarrier();
        g_jit.n_unwind = n + 1;
    }
    return 0;
}

/* Commit [p, p+n) of the region (in steps of 1 MB), so it can be written. */
static int jit_commit(uint8_t *p, size_t n)
{
    size_t end = (size_t)(p - g_jit.base) + n;
    if (end > g_jit.committed) {
        size_t c = (end + ((size_t)1 << 20) - 1) & ~(((size_t)1 << 20) - 1);
        if (c > JIT_REGION_SIZE) c = JIT_REGION_SIZE;
        if (!VirtualAlloc(g_jit.rw + g_jit.committed, c - g_jit.committed, MEM_COMMIT, PAGE_READWRITE))
            return -1;
        g_jit.committed = c;
    }
    return 0;
}

/* Code written at [p, p+n) is about to run. */
static void jit_flush(uint8_t *p, size_t n)
{
    FlushInstructionCache(GetCurrentProcess(), p, n);
}

static uint8_t *jit_alloc(size_t n, size_t align)
{
    size_t at = (g_jit.used + align - 1) & ~(align - 1);
    if (at + n > JIT_REGION_SIZE) return NULL;
    g_jit.used = at + n;
    return g_jit.base + at;
}

/* A target a rel32 at `field` (ending `extra` bytes before its
 * instruction) can reach: the address, or a veneer jumping to it.  Only
 * for calls and jumps.  Writes must be enabled where veneers go. */
static uint8_t *jit_reachable(uint8_t *field, uint8_t *target)
{
    intptr_t d = target - (field + 4);
    if (d == (int32_t)d) return target;
    for (int i = 0; i < g_jit.n_veneers; i++)
        if (g_jit.veneers[i].target == target) return g_jit.veneers[i].veneer;
    if (g_jit.n_veneers == g_jit.cap_veneers) {
        int cap = g_jit.cap_veneers ? g_jit.cap_veneers * 2 : 64;
        void *nv = realloc(g_jit.veneers, (size_t)cap * sizeof(*g_jit.veneers));
        if (!nv) return NULL;
        g_jit.veneers = nv;
        g_jit.cap_veneers = cap;
    }
    if (g_jit.veneer_used + 16 > JIT_VENEER_AREA) return NULL;
    uint8_t *v = g_jit.veneer_area + g_jit.veneer_used;
    g_jit.veneer_used += 16;
    uint8_t *w = JIT_W(v);
    w[0] = 0xFF; w[1] = 0x25; memset(w + 2, 0, 4);       /* jmp [rip+0] */
    memcpy(w + 6, &target, 8);
    jit_flush(v, 14);
    g_jit.veneers[g_jit.n_veneers].target = target;
    g_jit.veneers[g_jit.n_veneers].veneer = v;
    g_jit.n_veneers++;
    return v;
}

/* --- Emission ------------------------------------------------------------ */

/* What one stencil copy needs to be patched. */
typedef struct JitPatch {
    uint64_t imm[4];
    uint8_t *branch[5];     /* continue, target, slow, exit, dyn */
    void *helper;
} JitPatch;

typedef struct JitLits {
    uint8_t *base;          /* literal pool */
    uint64_t *vals;
    int n, cap;
    int count_only;
} JitLits;

/* The address of a literal holding v (deduplicated). */
static uint8_t *jit_literal(JitLits *L, uint64_t v)
{
    for (int i = L->n - 1; i >= 0 && i >= L->n - 64; i--)
        if (L->vals[i] == v) return L->base + (size_t)i * 8;
    if (L->n == L->cap) {
        int cap = L->cap ? L->cap * 2 : 256;
        uint64_t *nv = realloc(L->vals, (size_t)cap * sizeof(uint64_t));
        if (!nv) return NULL;
        L->vals = nv;
        L->cap = cap;
    }
    L->vals[L->n] = v;
    return L->base + (size_t)L->n++ * 8;
}

static int jit_patch_rel32(uint8_t *code, const JitHole *h, uint8_t *target)
{
    uint8_t *field = code + h->offset;
    intptr_t d = target + h->addend - (field + 4 + h->extra);
    if (d != (int32_t)d) return -1;
    int32_t d32 = (int32_t)d;
    memcpy(JIT_W(field), &d32, 4);
    return 0;
}

/* An instruction reading a 64-bit literal at [rip+field] (the hole) made to
 * do without it when its form allows, keeping its length: a load of an
 * address within rel32 reach becomes a lea of it, a load of a value that
 * fits 32 bits a mov of an immediate, a compare with one a compare with an
 * immediate.  Returns 1 when rewritten. */
static int jit_rewrite_literal(uint8_t *at, const JitHole *h, uint64_t value)
{
    uint8_t *w = JIT_W(at) + h->offset;     /* the field, where the JIT writes */
    uint8_t *field = at + h->offset;        /* and where it runs */
    if (h->extra || (h->pre != 2 && h->pre != 3))
        return 0;
    int has_rex = h->pre == 3;
    uint8_t rex = has_rex ? w[-3] : 0x40, opc = w[-2], modrm = w[-1];
    if ((modrm & 0xC7) != 0x05 || (rex & 0xF0) != 0x40)
        return 0;                           /* not [rip+disp32], or no REX where one was expected */
    int reg = (modrm >> 3) & 7, rex_r = (rex >> 2) & 1, wide = (rex & 8) != 0;
    int fits = (int64_t)value == (int32_t)value;
    uint32_t imm = (uint32_t)value;
    if (opc == 0x8B && wide) {
        intptr_t d = (intptr_t)value - (intptr_t)(field + 4);
        if (d == (int32_t)d) {
            w[-2] = 0x8D;                   /* lea reg, [rip+d] */
            imm = (uint32_t)(int32_t)d;
        } else if (fits) {
            w[-3] = 0x48 | rex_r;           /* mov reg, simm32: REX.W, reg in r/m */
            w[-2] = 0xC7;
            w[-1] = 0xC0 | reg;
        } else {
            return 0;
        }
    } else if (opc == 0x8B && !wide) {
        if (has_rex)
            w[-3] = 0x40 | rex_r;           /* mov reg32, imm32: the load's low half */
        w[-2] = 0xC7;
        w[-1] = 0xC0 | reg;
    } else if (opc == 0x3B && wide && fits) {
        w[-3] = 0x48 | rex_r;               /* cmp reg, simm32 */
        w[-2] = 0x81;
        w[-1] = 0xF8 | reg;
    } else {
        return 0;
    }
    memcpy(w, &imm, 4);
    return 1;
}

/* Copy stencil sid to `at` (size bytes of it) and patch it. */
static int jit_emit(uint8_t *at, int sid, uint32_t size, const JitPatch *P, JitLits *L)
{
    const JitStencil *st = &jit_stencils[sid];
    if (at) memcpy(JIT_W(at), st->code, size);
    for (const JitHole *h = st->holes; h->kind != JIT_HOLE_END; h++) {
        if (h->offset + 4 > size) continue;     /* the cut jump */
        uint8_t *t = NULL;
        switch (h->kind) {
        case JIT_HOLE_IMM:
        case JIT_HOLE_ADDR_LIT: {
            uint64_t v = h->kind == JIT_HOLE_IMM ? P->imm[h->arg] : (uint64_t)(uintptr_t)jit_sym_addr[h->arg];
            if (at && jit_rewrite_literal(at, h, v))
                continue;
            t = jit_literal(L, v);
            break;
        }
        case JIT_HOLE_BRANCH:
            t = P->branch[h->arg];
            if (!t && at) {
                fprintf(stderr, "[jit] %s: branch %d has no target\n", st->name, h->arg);
                return -1;
            }
            break;
        case JIT_HOLE_HELPER:
            if (at) t = jit_reachable(at + h->offset, (uint8_t *)P->helper);
            break;
        case JIT_HOLE_CALL:
            if (at) t = jit_reachable(at + h->offset, (uint8_t *)jit_sym_addr[h->arg]);
            break;
        case JIT_HOLE_DATA:
            t = g_jit.data[h->arg];
            break;
        }
        if (!at) continue;
        if (!t || jit_patch_rel32(at, h, t)) return -1;
    }
    return 0;
}

/* --- Setup ---------------------------------------------------------------- */

static int jit_setup(void)
{
    if (g_jit.state) return g_jit.state;
    g_jit.state = -1;
    g_jit.unwind = VirtualAlloc(NULL, (size_t)JIT_MAX_UNWIND * sizeof(RUNTIME_FUNCTION), MEM_RESERVE, PAGE_NOACCESS);
    if (!g_jit.unwind || jit_map_region(JIT_REGION_SIZE)) return -1;
    jit_map_open();
    /* The sampler must not stop this thread while it holds the function
     * table lock, which the sampler's unwinding takes too. */
    InterlockedExchange(&g_jit_os_busy, 1);
    BOOL installed = RtlInstallFunctionTableCallback((DWORD64)(uintptr_t)g_jit.base | 3, (DWORD64)(uintptr_t)g_jit.base,
                                                     (DWORD)JIT_REGION_SIZE, jit_unwind_callback, NULL, NULL);
    InterlockedExchange(&g_jit_os_busy, 0);
    if (!installed)
        return -1;
    /* Fixed parts: veneers, stencils' unwind info, read-only data, the
     * trampoline, exit and dyn stubs. */
    g_jit.veneer_area = jit_alloc(JIT_VENEER_AREA, 4096);
    uint8_t *start = g_jit.base + g_jit.used;
    size_t need = 0;
    for (int i = 0; i < JIT_ST_COUNT; i++) need += jit_stencils[i].xdata_size + 8;
    for (size_t i = 0; i < sizeof(jit_datas) / sizeof(jit_datas[0]); i++) need += jit_datas[i].size + 16;
    need += 4096;
    if (jit_commit(g_jit.base, (size_t)(start - g_jit.base) + need)) return -1;
    for (int i = 0; i < JIT_ST_COUNT; i++) {
        if (!jit_stencils[i].xdata_size) continue;
        g_jit.stencil_xdata[i] = jit_alloc(jit_stencils[i].xdata_size, 8);
        memcpy(JIT_W(g_jit.stencil_xdata[i]), jit_stencils[i].xdata, jit_stencils[i].xdata_size);
    }
    for (size_t i = 0; i < sizeof(jit_datas) / sizeof(jit_datas[0]); i++) {
        g_jit.data[i] = jit_alloc(jit_datas[i].size, 16);
        memcpy(JIT_W(g_jit.data[i]), jit_datas[i].data, jit_datas[i].size);
    }
    static const int fixed[] = { JIT_ST_trampoline, JIT_ST_exit, JIT_ST_dyn };
    uint8_t **slot[] = { &g_jit.trampoline, &g_jit.exit, &g_jit.dyn };
    JitLits L = { 0 };
    for (int k = 0; k < 3; k++)
        *slot[k] = jit_alloc(jit_stencils[fixed[k]].size, 16);
    L.base = jit_alloc(64 * 8, 8);
    for (int k = 0; k < 3; k++) {
        JitPatch P = { 0 };
        const JitStencil *st = &jit_stencils[fixed[k]];
        if (g_jit.map)
            fprintf(g_jit.map, "X %zx %x %s\n", (size_t)(*slot[k] - g_jit.base), st->size, st->name);
        if (jit_emit(*slot[k], fixed[k], st->size, &P, &L) ||
            jit_add_unwind(*slot[k], st, st->size, fixed[k]))
            return -1;
    }
    memcpy(JIT_W(L.base), L.vals, (size_t)L.n * 8);
    free(L.vals);
    jit_flush(start, g_jit.used - (size_t)(start - g_jit.base));
    g_jit_trampoline = (int (*)(JSInterpFrame *, void *))(void *)g_jit.trampoline;
    g_jit.state = 1;
    return 1;
}

/* --- Compilation ------------------------------------------------------------ */

/* How an instruction is compiled: its main stencil, and whether it needs
 * an out-of-line generic copy (the fast path's SLOW()). */
typedef struct JitInsn {
    uint32_t off;
    uint8_t op;
    uint8_t main_sid;
    uint8_t slow_sid;       /* JIT_ST_COUNT: none */
    uint8_t cut;            /* its final jump to the next instruction is cut */
    uint32_t main_at, slow_at;
    uint32_t main_size;
    int32_t target;         /* bytecode offset of a static jump target, or -1 */
    uint64_t imm0, imm1;
} JitInsn;

/* imm0[0..1]: the stencil's IMM(0) and IMM(1). */
static int jit_specialized(JSContextHandle ctx, int op, const uint8_t *pc, const GCValue *cpool, uint64_t *imm0,
                           int *target, uint32_t off, int *backward)
{
    *backward = 0;
    switch (op) {
#define LOC_OP(o) case OP_##o: *imm0 = (uint64_t)jit_u16(pc + 1) * sizeof(GCValue); return JIT_ST_##o;
    LOC_OP(get_loc) LOC_OP(put_loc) LOC_OP(set_loc) LOC_OP(get_arg) LOC_OP(put_arg) LOC_OP(set_arg)
    LOC_OP(get_loc_check) LOC_OP(put_loc_check) LOC_OP(set_loc_check) LOC_OP(set_loc_uninitialized)
#undef LOC_OP
#define LOC8_OP(o) case OP_##o: *imm0 = (uint64_t)pc[1] * sizeof(GCValue); return JIT_ST_##o;
    LOC8_OP(inc_loc) LOC8_OP(dec_loc) LOC8_OP(add_loc)
#undef LOC8_OP
    /* Under CYBER_TRACE_GV (fixed for the run) the interpreter traces
     * these: the helper then. */
#define VREF_OP(o) case OP_##o: if (CYBER_ENV(CYBER_T_GV) && op != OP_get_var_ref_check) return -1; \
                                *imm0 = jit_u16(pc + 1); return JIT_ST_##o;
    VREF_OP(get_var_ref) VREF_OP(get_var_ref_check) VREF_OP(put_var_ref) VREF_OP(set_var_ref)
#undef VREF_OP
    case OP_push_i32:
        *imm0 = (uint64_t)(int64_t)(int32_t)jit_u32(pc + 1);
        return JIT_ST_push_i32;
    case OP_push_const:
        *imm0 = (uint64_t)(uintptr_t)&cpool[jit_u32(pc + 1)];
        return JIT_ST_push_const;
    case OP_push_atom_value: {
        GCValue v;
        if (!jit_atom_value(ctx, jit_u32(pc + 1), &v)) return -1;
        memcpy(imm0, &v, 8);
        imm0[1] = (uint64_t)v.tag;
        return JIT_ST_push_value;
    }
    case OP_call:
    case OP_call_method:
    case OP_call_constructor:
        imm0[0] = (uint64_t)(uintptr_t)(pc + 3);
        imm0[1] = jit_u16(pc + 1) | (uint64_t)(op == OP_call ? JIT_CALL_PLAIN :
                                               op == OP_call_method ? JIT_CALL_METHOD : JIT_CALL_CONSTRUCTOR) << 16;
        return JIT_ST_call;
    case OP_tail_call:
    case OP_tail_call_method:
        imm0[0] = (uint64_t)(uintptr_t)(pc + 3);
        imm0[1] = jit_u16(pc + 1) | (uint64_t)(op == OP_tail_call ? JIT_CALL_PLAIN : JIT_CALL_METHOD) << 16;
        return JIT_ST_tail_call;
    case OP_get_field:
    case OP_get_field2:
    case OP_put_field:
        /* Under CYBER_TRACE_NAF the interpreter records the atom. */
        if (CYBER_ENV(CYBER_T_NAF) && op != OP_put_field)
            return -1;
        imm0[0] = jit_u32(pc + 1);
        imm0[1] = (uint64_t)(uintptr_t)(pc + 5);
        if (op == OP_get_field && *imm0 == jit_atom_length)
            return JIT_ST_get_length;
        return op == OP_get_field ? JIT_ST_get_field : op == OP_get_field2 ? JIT_ST_get_field2 : JIT_ST_put_field;
    case OP_goto:
        *target = (int)(off + 1 + (int32_t)jit_u32(pc + 1));
        return JIT_ST_goto;
    case OP_if_true:
    case OP_if_false:
        *target = (int)(off + 1 + (int32_t)jit_u32(pc + 1));
        *backward = *target < (int)(off + 5);
        if (op == OP_if_true) return *backward ? JIT_ST_if_true_back : JIT_ST_if_true;
        return *backward ? JIT_ST_if_false_back : JIT_ST_if_false;
#define PLAIN_OP(o) case OP_##o: return JIT_ST_##o;
    PLAIN_OP(undefined) PLAIN_OP(null) PLAIN_OP(push_false) PLAIN_OP(push_true)
    PLAIN_OP(drop) PLAIN_OP(nip) PLAIN_OP(nip1) PLAIN_OP(dup) PLAIN_OP(dup1) PLAIN_OP(dup2) PLAIN_OP(dup3)
    PLAIN_OP(insert2) PLAIN_OP(insert3) PLAIN_OP(insert4) PLAIN_OP(perm3) PLAIN_OP(perm4) PLAIN_OP(perm5)
    PLAIN_OP(rot3l) PLAIN_OP(rot4l) PLAIN_OP(rot5l) PLAIN_OP(rot3r) PLAIN_OP(swap) PLAIN_OP(swap2)
    PLAIN_OP(add) PLAIN_OP(sub) PLAIN_OP(mul) PLAIN_OP(mod) PLAIN_OP(neg) PLAIN_OP(shr) PLAIN_OP(and) PLAIN_OP(or) PLAIN_OP(xor) PLAIN_OP(shl) PLAIN_OP(sar)
    PLAIN_OP(lt) PLAIN_OP(lte) PLAIN_OP(gt) PLAIN_OP(gte) PLAIN_OP(eq) PLAIN_OP(neq)
    PLAIN_OP(strict_eq) PLAIN_OP(strict_neq)
    PLAIN_OP(inc) PLAIN_OP(dec) PLAIN_OP(post_inc) PLAIN_OP(post_dec)
    PLAIN_OP(lnot) PLAIN_OP(is_undefined_or_null) PLAIN_OP(return) PLAIN_OP(return_undef)
    PLAIN_OP(get_array_el) PLAIN_OP(get_array_el2) PLAIN_OP(push_this)
#undef PLAIN_OP
    default:
        return -1;
    }
}

/* How many literals a copy of a stencil uses. */
static int jit_n_literals(int sid)
{
    int n = 0;
    for (const JitHole *h = jit_stencils[sid].holes; h->kind != JIT_HOLE_END; h++)
        n += h->kind == JIT_HOLE_IMM || h->kind == JIT_HOLE_ADDR_LIT;
    return n;
}

/* Whether a stencil's fast path can take SLOW(). */
static int jit_has_slow(int sid)
{
    for (const JitHole *h = jit_stencils[sid].holes; h->kind != JIT_HOLE_END; h++)
        if (h->kind == JIT_HOLE_BRANCH && h->arg == JIT_BR_SLOW) return 1;
    return 0;
}

/* The static target of a jumping instruction the generic stencil runs, or
 * -1. */
static int jit_generic_target(int op, const uint8_t *pc, uint32_t off)
{
    switch (jit_op_info[op].fmt) {
    case JIT_FMT_label:
        if (op == OP_catch) return -1;      /* pushes an offset, does not jump */
        return (int)(off + 1 + (int32_t)jit_u32(pc + 1));
    case JIT_FMT_atom_label_u8:
    case JIT_FMT_atom_label_u16:
        return (int)(off + 5 + (int32_t)jit_u32(pc + 5));
    default:
        return -1;
    }
}

static void jit_free_code(JitCode *jc)
{
    if (!jc) return;
    free(jc->pc_map);
    free(jc);
}

JitCode *jit_compile(JSContextHandle ctx, GCHandle owner, const uint8_t *bc, uint32_t len,
                     const GCValue *cpool, int (*const *helpers)(JSInterpFrame *F), const uint8_t *jumps,
                     const char *name)
{
    if (jit_setup() <= 0) return NULL;
    if (len == 0 || len > JIT_MAX_FUNC_LEN) return NULL;
    uint64_t t0 = jit_ticks();
    /* Decode. */
    JitInsn *ins = malloc(sizeof(JitInsn) * (len + 1));
    uint32_t *pc_map = malloc(sizeof(uint32_t) * (len + 1));
    JitCode *jc = calloc(1, sizeof(JitCode));
    JitLits L = { 0 };
    int n = 0;
    if (!ins || !pc_map || !jc) goto fail;
    for (uint32_t off = 0; off < len; ) {
        int op = bc[off];
        int size = jit_op_info[op].size;
        if (!size || off + size > len) goto fail;
        JitInsn *I = &ins[n++];
        memset(I, 0, sizeof(*I));
        I->off = off;
        I->op = op;
        I->target = -1;
        int backward;
        uint64_t imms[2] = { 0, 0 };
        int sid = jit_specialized(ctx, op, bc + off, cpool, imms, &I->target, off, &backward);
        I->imm0 = imms[0];
        I->imm1 = imms[1];
        if (sid >= 0) {
            I->main_sid = sid;
            I->slow_sid = !jit_has_slow(sid) ? JIT_ST_COUNT
                : !jumps[op] ? JIT_ST_generic_flat
                : I->target >= 0 ? JIT_ST_generic_branch : JIT_ST_generic;
        } else {
            I->target = jumps[op] ? jit_generic_target(op, bc + off, off) : -1;
            I->main_sid = !jumps[op] ? JIT_ST_generic_flat
                : I->target >= 0 ? JIT_ST_generic_branch : JIT_ST_generic;
            I->slow_sid = JIT_ST_COUNT;
        }
        if (I->target >= (int)len) goto fail;
        off += size;
    }
    /* A last stencil after the code falls back to the interpreter (bytecode
     * never runs off its end). */
    ins[n].off = len;
    ins[n].op = 0;
    ins[n].main_sid = JIT_ST_interp;
    ins[n].slow_sid = JIT_ST_COUNT;
    ins[n].target = -1;

    /* Pass 1: sizes and offsets. */
    uint32_t at = 0;
    for (int i = 0; i <= n; i++) {
        JitInsn *I = &ins[i];
        const JitStencil *st = &jit_stencils[I->main_sid];
        I->cut = i < n && st->tail_continue;
        I->main_size = st->size - (I->cut ? 5 : 0);
        I->main_at = at;
        at += I->main_size;
    }
    for (int i = 0; i < n; i++) {
        JitInsn *I = &ins[i];
        if (I->slow_sid == JIT_ST_COUNT) continue;
        at = (at + 15) & ~15u;
        I->slow_at = at;
        at += jit_stencils[I->slow_sid].size;
    }
    at = (at + 15) & ~15u;
    uint32_t code_size = at;
    uint32_t lit_cap = 0;
    for (int i = 0; i <= n; i++) {
        lit_cap += jit_n_literals(ins[i].main_sid);
        if (ins[i].slow_sid != JIT_ST_COUNT) lit_cap += jit_n_literals(ins[i].slow_sid);
    }
    uint8_t *base = jit_alloc(code_size + (size_t)lit_cap * 8, 16);
    if (!base) {
        if (jit_log_on()) fprintf(stderr, "[jit] code region full\n");
        goto fail;
    }
    L.base = base + code_size;
    if (jit_commit(base, code_size + (size_t)lit_cap * 8)) goto fail;

    for (uint32_t i = 0; i <= len; i++) pc_map[i] = UINT32_MAX;
    for (int i = 0; i < n; i++) pc_map[ins[i].off] = ins[i].main_at;

    /* Pass 2: copy and patch. */
    for (int i = 0; i <= n; i++) {
        JitInsn *I = &ins[i];
        JitPatch P = { 0 };
        uint32_t next_off = i < n ? ins[i + 1].off : len;
        P.imm[0] = I->imm0;
        P.imm[1] = I->imm1;
        P.branch[JIT_BR_CONTINUE] = base + ins[i < n ? i + 1 : n].main_at;
        P.branch[JIT_BR_TARGET] = I->target >= 0 && pc_map[I->target] != UINT32_MAX ? base + pc_map[I->target] : NULL;
        P.branch[JIT_BR_SLOW] = I->slow_sid != JIT_ST_COUNT ? base + I->slow_at : NULL;
        P.branch[JIT_BR_EXIT] = g_jit.exit;
        P.branch[JIT_BR_DYN] = g_jit.dyn;
        P.helper = (void *)helpers[I->op];
        if (I->target >= 0 && !P.branch[JIT_BR_TARGET]) goto fail_written;  /* not an instruction start */
        /* The generic stencils' immediates: the operands' address, the next
         * instruction's and the jump target's. */
        JitPatch G = P;
        G.imm[0] = (uint64_t)(uintptr_t)(bc + I->off + 1);
        G.imm[1] = (uint64_t)(uintptr_t)(bc + next_off);
        G.imm[2] = I->target >= 0 ? (uint64_t)(uintptr_t)(bc + I->target) : 0;
        G.branch[JIT_BR_SLOW] = NULL;
        if (I->main_sid == JIT_ST_interp)
            P.imm[0] = (uint64_t)(uintptr_t)(bc + I->off);
        const JitPatch *M = (I->main_sid == JIT_ST_generic || I->main_sid == JIT_ST_generic_branch ||
                             I->main_sid == JIT_ST_generic_flat) ? &G : &P;
        if (jit_emit(base + I->main_at, I->main_sid, I->main_size, M, &L) ||
            jit_add_unwind(base + I->main_at, &jit_stencils[I->main_sid], I->main_size, I->main_sid))
            goto fail_written;
        if (I->slow_sid != JIT_ST_COUNT &&
            (jit_emit(base + I->slow_at, I->slow_sid, jit_stencils[I->slow_sid].size, &G, &L) ||
             jit_add_unwind(base + I->slow_at, &jit_stencils[I->slow_sid], jit_stencils[I->slow_sid].size, I->slow_sid)))
            goto fail_written;
        if ((uint32_t)L.n > lit_cap) goto fail_written;
    }
    memcpy(JIT_W(L.base), L.vals, (size_t)L.n * 8);
    /* Give back the unused part of the literal pool. */
    if (g_jit.used == (size_t)(L.base - g_jit.base) + (size_t)lit_cap * 8)
        g_jit.used = (size_t)(L.base - g_jit.base) + (size_t)L.n * 8;
    jit_flush(base, code_size + (size_t)L.n * 8);
    free(L.vals);
    jc->base = base;
    jc->size = code_size + (uint32_t)L.n * 8;
    jc->byte_code = bc;
    jc->byte_code_len = len;
    jc->pc_map = pc_map;
    jc->owner = owner;
    if (g_jit.map) {
        fprintf(g_jit.map, "F %zx %x %s\n", (size_t)(base - g_jit.base), jc->size, name ? name : "?");
        for (int i = 0; i < n; i++) {
            fprintf(g_jit.map, "I %zx %s\n", (size_t)(base + ins[i].main_at - g_jit.base), jit_op_name[ins[i].op]);
            if (ins[i].slow_sid != JIT_ST_COUNT)
                fprintf(g_jit.map, "S %zx %s\n", (size_t)(base + ins[i].slow_at - g_jit.base), jit_op_name[ins[i].op]);
        }
        fprintf(g_jit.map, "L %zx\n", (size_t)(L.base - g_jit.base));
    }
    g_jit.compiled_bytes += jc->size;
    g_jit.compiled_funcs++;
    g_jit.bytecode_bytes += len;
    g_jit.compile_ticks += jit_ticks() - t0;
    free(ins);
    if (jit_log_on() > 1)
        fprintf(stderr, "[jit] compiled %u bytecode bytes (%d instructions) into %u bytes at %p\n",
                len, n, jc->size, (void *)base);
    return jc;

 fail_written:
    /* The space stays used; its unwind entries describe code never run. */
 fail:
    g_jit.failed_funcs++;
    g_jit.compile_ticks += jit_ticks() - t0;
    if (jit_log_on()) fprintf(stderr, "[jit] cannot compile (%u bytecode bytes)\n", len);
    free(L.vals);
    free(ins);
    free(pc_map);
    free(jc);
    return NULL;
}

/* --- Running ---------------------------------------------------------------- */

int (*g_jit_trampoline)(JSInterpFrame *F, void *target);

void *jit_lookup_pc(JSInterpFrame *F)
{
    const JitCode *jc = F->code;
    uintptr_t off = (uintptr_t)(F->pc - jc->byte_code);
    if (off < jc->byte_code_len && jc->pc_map[off] != UINT32_MAX)
        return jc->base + jc->pc_map[off];
    /* Not an instruction start (cannot happen): finish in the interpreter. */
    F->jit_exit = JSOP_NEXT;
    return g_jit.exit;
}


void jit_print_stats(void)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    fprintf(stderr, "[jit] %llu functions compiled (%llu bytecode bytes into %llu bytes of code) in %.1f ms, "
            "%llu not compiled, region %zu KB used\n",
            (unsigned long long)g_jit.compiled_funcs, (unsigned long long)g_jit.bytecode_bytes,
            (unsigned long long)g_jit.compiled_bytes, 1e3 * (double)g_jit.compile_ticks / (double)f.QuadPart,
            (unsigned long long)g_jit.failed_funcs, g_jit.used >> 10);
}

int jit_available(void)
{
    return jit_setup() > 0;
}

#else /* !JIT_SUPPORTED */

int (*g_jit_trampoline)(JSInterpFrame *F, void *target);

JitCode *jit_compile(JSContextHandle ctx, GCHandle owner, const uint8_t *bc, uint32_t len,
                     const GCValue *cpool, int (*const *helpers)(JSInterpFrame *F), const uint8_t *jumps,
                     const char *name)
{
    (void)ctx; (void)owner; (void)name; (void)bc; (void)len; (void)cpool; (void)helpers; (void)jumps;
    return NULL;
}

void jit_print_stats(void) {}
int jit_available(void) { return 0; }

#endif

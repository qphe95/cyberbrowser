/*
 * quickjs_t2_x64.h - a small x86-64 assembler for the optimizer
 * (quickjs_t2.c; JS_PERF_DESIGN.md section 6.4): the instructions its nodes
 * need, encoded into a growing buffer, with labels resolved at the end.
 */
#ifndef QUICKJS_T2_X64_H
#define QUICKJS_T2_X64_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    X_RAX, X_RCX, X_RDX, X_RBX, X_RSP, X_RBP, X_RSI, X_RDI,
    X_R8, X_R9, X_R10, X_R11, X_R12, X_R13, X_R14, X_R15,
};

/* Condition codes, as in jcc's low nibble. */
enum {
    X_CC_O = 0, X_CC_NO = 1, X_CC_B = 2, X_CC_AE = 3, X_CC_E = 4, X_CC_NE = 5,
    X_CC_BE = 6, X_CC_A = 7, X_CC_L = 12, X_CC_GE = 13, X_CC_LE = 14, X_CC_G = 15,
};

typedef struct XFixup {
    uint32_t at;        /* the rel32 field */
    int label;
} XFixup;

typedef struct XAsm {
    uint8_t *buf;
    uint32_t len, cap;
    int32_t *labels;    /* label -> offset, -1 while unbound */
    int n_labels, cap_labels;
    XFixup *fixups;
    int n_fixups, cap_fixups;
    int oom;
} XAsm;

static inline void x_grow(XAsm *a, uint32_t n)
{
    if (a->len + n <= a->cap) return;
    uint32_t cap = a->cap ? a->cap * 2 : 4096;
    while (cap < a->len + n) cap *= 2;
    uint8_t *b = realloc(a->buf, cap);
    if (!b) { a->oom = 1; a->len = 0; return; }
    a->buf = b;
    a->cap = cap;
}

static inline void x_byte(XAsm *a, uint8_t v)
{
    x_grow(a, 1);
    if (!a->oom) a->buf[a->len++] = v;
}

static inline void x_u32(XAsm *a, uint32_t v)
{
    x_grow(a, 4);
    if (!a->oom) { memcpy(a->buf + a->len, &v, 4); a->len += 4; }
}

static inline void x_u64(XAsm *a, uint64_t v)
{
    x_grow(a, 8);
    if (!a->oom) { memcpy(a->buf + a->len, &v, 8); a->len += 8; }
}

static inline void x_free(XAsm *a)
{
    free(a->buf);
    free(a->labels);
    free(a->fixups);
    memset(a, 0, sizeof(*a));
}

static inline int x_new_label(XAsm *a)
{
    if (a->n_labels == a->cap_labels) {
        int cap = a->cap_labels ? 2 * a->cap_labels : 64;
        int32_t *l = realloc(a->labels, cap * sizeof(int32_t));
        if (!l) { a->oom = 1; return 0; }
        a->labels = l;
        a->cap_labels = cap;
    }
    a->labels[a->n_labels] = -1;
    return a->n_labels++;
}

static inline void x_bind(XAsm *a, int label)
{
    if (!a->oom) a->labels[label] = (int32_t)a->len;
}

static inline void x_ref_label(XAsm *a, int label)
{
    if (a->n_fixups == a->cap_fixups) {
        int cap = a->cap_fixups ? 2 * a->cap_fixups : 64;
        XFixup *f = realloc(a->fixups, cap * sizeof(XFixup));
        if (!f) { a->oom = 1; return; }
        a->fixups = f;
        a->cap_fixups = cap;
    }
    a->fixups[a->n_fixups].at = a->len;
    a->fixups[a->n_fixups].label = label;
    a->n_fixups++;
    x_u32(a, 0);
}

/* Patch the label references: -1 if a label was never bound. */
static inline int x_resolve(XAsm *a)
{
    if (a->oom) return -1;
    for (int i = 0; i < a->n_fixups; i++) {
        int32_t to = a->labels[a->fixups[i].label];
        if (to < 0) return -1;
        int32_t rel = to - (int32_t)(a->fixups[i].at + 4);
        memcpy(a->buf + a->fixups[i].at, &rel, 4);
    }
    return 0;
}

/* REX with W, and the high bits of reg (r), index (x) and base/rm (b). */
static inline void x_rex(XAsm *a, int w, int r, int x, int b)
{
    uint8_t rex = 0x40 | (w ? 8 : 0) | ((r & 8) ? 4 : 0) | ((x & 8) ? 2 : 0) | ((b & 8) ? 1 : 0);
    if (rex != 0x40 || w) x_byte(a, rex);
}

/* ModRM (and SIB, displacement) for [base + disp] with reg field r. */
static inline void x_mem(XAsm *a, int r, int base, int32_t disp)
{
    int mod = disp == 0 && (base & 7) != X_RBP ? 0 : (disp >= -128 && disp <= 127 ? 1 : 2);
    x_byte(a, (uint8_t)((mod << 6) | ((r & 7) << 3) | (base & 7)));
    if ((base & 7) == X_RSP) x_byte(a, 0x24);     /* SIB: base only */
    if (mod == 1) x_byte(a, (uint8_t)(int8_t)disp);
    else if (mod == 2) x_u32(a, (uint32_t)disp);
}

static inline void x_modrm_rr(XAsm *a, int r, int rm)
{
    x_byte(a, (uint8_t)(0xc0 | ((r & 7) << 3) | (rm & 7)));
}

/* mov dst, src (64-bit) */
static inline void x_mov_rr(XAsm *a, int dst, int src)
{
    x_rex(a, 1, src, 0, dst);
    x_byte(a, 0x89);
    x_modrm_rr(a, src, dst);
}

/* mov dst, [base + disp] (64-bit) */
static inline void x_load(XAsm *a, int dst, int base, int32_t disp)
{
    x_rex(a, 1, dst, 0, base);
    x_byte(a, 0x8b);
    x_mem(a, dst, base, disp);
}

/* mov [base + disp], src (64-bit) */
static inline void x_store(XAsm *a, int base, int32_t disp, int src)
{
    x_rex(a, 1, src, 0, base);
    x_byte(a, 0x89);
    x_mem(a, src, base, disp);
}

/* mov dst32, [base + disp] (zero-extends) */
static inline void x_load32(XAsm *a, int dst, int base, int32_t disp)
{
    x_rex(a, 0, dst, 0, base);
    x_byte(a, 0x8b);
    x_mem(a, dst, base, disp);
}

/* mov dst, imm64 (shorter forms when they fit) */
static inline void x_mov_ri(XAsm *a, int dst, uint64_t imm)
{
    if (imm <= 0xffffffffu) {               /* mov r32, imm32 (zero-extends) */
        x_rex(a, 0, 0, 0, dst);
        x_byte(a, (uint8_t)(0xb8 + (dst & 7)));
        x_u32(a, (uint32_t)imm);
    } else if ((int64_t)imm >= INT32_MIN && (int64_t)imm <= INT32_MAX) {
        x_rex(a, 1, 0, 0, dst);              /* mov r64, simm32 */
        x_byte(a, 0xc7);
        x_modrm_rr(a, 0, dst);
        x_u32(a, (uint32_t)imm);
    } else {
        x_rex(a, 1, 0, 0, dst);              /* movabs */
        x_byte(a, (uint8_t)(0xb8 + (dst & 7)));
        x_u64(a, imm);
    }
}

/* lea dst, [base + disp] */
static inline void x_lea(XAsm *a, int dst, int base, int32_t disp)
{
    x_rex(a, 1, dst, 0, base);
    x_byte(a, 0x8d);
    x_mem(a, dst, base, disp);
}

/* add/sub/cmp r64, imm32: op 0 add, 5 sub, 7 cmp */
static inline void x_alu_ri(XAsm *a, int op, int r, int32_t imm)
{
    x_rex(a, 1, 0, 0, r);
    if (imm >= -128 && imm <= 127) {
        x_byte(a, 0x83);
        x_modrm_rr(a, op, r);
        x_byte(a, (uint8_t)(int8_t)imm);
    } else {
        x_byte(a, 0x81);
        x_modrm_rr(a, op, r);
        x_u32(a, (uint32_t)imm);
    }
}

/* cmp r64, r64 */
static inline void x_cmp_rr(XAsm *a, int r1, int r2)
{
    x_rex(a, 1, r2, 0, r1);
    x_byte(a, 0x39);
    x_modrm_rr(a, r2, r1);
}

/* test r32, r32 */
static inline void x_test32(XAsm *a, int r1, int r2)
{
    x_rex(a, 0, r2, 0, r1);
    x_byte(a, 0x85);
    x_modrm_rr(a, r2, r1);
}

/* cmp dword [base + disp], imm8 */
static inline void x_cmp_mem32_i8(XAsm *a, int base, int32_t disp, int8_t imm)
{
    x_rex(a, 0, 0, 0, base);
    x_byte(a, 0x83);
    x_mem(a, 7, base, disp);
    x_byte(a, (uint8_t)imm);
}

static inline void x_push(XAsm *a, int r)
{
    x_rex(a, 0, 0, 0, r);
    x_byte(a, (uint8_t)(0x50 + (r & 7)));
}

static inline void x_pop(XAsm *a, int r)
{
    x_rex(a, 0, 0, 0, r);
    x_byte(a, (uint8_t)(0x58 + (r & 7)));
}

static inline void x_ret(XAsm *a) { x_byte(a, 0xc3); }

/* call r64 */
static inline void x_call_r(XAsm *a, int r)
{
    x_rex(a, 0, 0, 0, r);
    x_byte(a, 0xff);
    x_modrm_rr(a, 2, r);
}

/* call an absolute address (through rax) */
static inline void x_call_abs(XAsm *a, const void *fn)
{
    x_mov_ri(a, X_RAX, (uint64_t)(uintptr_t)fn);
    x_call_r(a, X_RAX);
}

static inline void x_jmp(XAsm *a, int label)
{
    x_byte(a, 0xe9);
    x_ref_label(a, label);
}

static inline void x_jcc(XAsm *a, int cc, int label)
{
    x_byte(a, 0x0f);
    x_byte(a, (uint8_t)(0x80 | cc));
    x_ref_label(a, label);
}

/* --- Integer arithmetic ------------------------------------------------- */

/* op r32, r32 (dst op= src): x_ADD, x_OR, x_AND, x_SUB, x_XOR, x_CMP are
 * the opcode bytes of the r/m32, r32 forms. */
enum { X_ADD = 0x01, X_OR = 0x09, X_AND = 0x21, X_SUB = 0x29, X_XOR = 0x31, X_CMP = 0x39 };

static inline void x_alu_rr32(XAsm *a, int op, int dst, int src)
{
    x_rex(a, 0, src, 0, dst);
    x_byte(a, (uint8_t)op);
    x_modrm_rr(a, src, dst);
}

static inline void x_alu_rr64(XAsm *a, int op, int dst, int src)
{
    x_rex(a, 1, src, 0, dst);
    x_byte(a, (uint8_t)op);
    x_modrm_rr(a, src, dst);
}

/* imul dst32, src32 */
static inline void x_imul32(XAsm *a, int dst, int src)
{
    x_rex(a, 0, dst, 0, src);
    x_byte(a, 0x0f); x_byte(a, 0xaf);
    x_modrm_rr(a, dst, src);
}

/* neg r32 */
static inline void x_neg32(XAsm *a, int r)
{
    x_rex(a, 0, 0, 0, r);
    x_byte(a, 0xf7);
    x_modrm_rr(a, 3, r);
}

/* shl/shr/sar r32, cl: kind 4 shl, 5 shr, 7 sar */
static inline void x_shift_cl32(XAsm *a, int kind, int r)
{
    x_rex(a, 0, 0, 0, r);
    x_byte(a, 0xd3);
    x_modrm_rr(a, kind, r);
}

/* shr r64, imm8 */
static inline void x_shr64_i(XAsm *a, int r, uint8_t n)
{
    x_rex(a, 1, 0, 0, r);
    x_byte(a, 0xc1);
    x_modrm_rr(a, 5, r);
    x_byte(a, n);
}

/* test r32, imm32 */
static inline void x_test32_i(XAsm *a, int r, uint32_t imm)
{
    x_rex(a, 0, 0, 0, r);
    x_byte(a, 0xf7);
    x_modrm_rr(a, 0, r);
    x_u32(a, imm);
}

/* add/cmp r32, imm8: op 0 add, 7 cmp */
static inline void x_alu_ri32(XAsm *a, int op, int r, int8_t imm)
{
    x_rex(a, 0, 0, 0, r);
    x_byte(a, 0x83);
    x_modrm_rr(a, op, r);
    x_byte(a, (uint8_t)imm);
}

/* setcc al; movzx eax, al */
static inline void x_setcc_eax(XAsm *a, int cc)
{
    x_byte(a, 0x0f); x_byte(a, (uint8_t)(0x90 | cc)); x_byte(a, 0xc0);
    x_byte(a, 0x0f); x_byte(a, 0xb6); x_byte(a, 0xc0);
}

/* setcc r8 for r in rax..rbx (al, cl, dl, bl) */
static inline void x_setcc_r8(XAsm *a, int cc, int r)
{
    x_byte(a, 0x0f); x_byte(a, (uint8_t)(0x90 | cc)); x_modrm_rr(a, 0, r);
}

/* bt[c|r] r64, imm8: kind 6 btr, 7 btc */
static inline void x_bt64_i(XAsm *a, int kind, int r, uint8_t bit)
{
    x_rex(a, 1, 0, 0, r);
    x_byte(a, 0x0f); x_byte(a, 0xba);
    x_modrm_rr(a, kind, r);
    x_byte(a, bit);
}

/* mov dword [base + disp], src32 */
static inline void x_store32(XAsm *a, int base, int32_t disp, int src)
{
    x_rex(a, 0, src, 0, base);
    x_byte(a, 0x89);
    x_mem(a, src, base, disp);
}

/* cmp r32, imm32 */
static inline void x_cmp32_i(XAsm *a, int r, uint32_t imm)
{
    x_rex(a, 0, 0, 0, r);
    x_byte(a, 0x81);
    x_modrm_rr(a, 7, r);
    x_u32(a, imm);
}

/* shl r64, imm8 */
static inline void x_shl64_i(XAsm *a, int r, uint8_t n)
{
    x_rex(a, 1, 0, 0, r);
    x_byte(a, 0xc1);
    x_modrm_rr(a, 4, r);
    x_byte(a, n);
}

/* test byte [base + disp], imm8 */
static inline void x_test_mem8_i(XAsm *a, int base, int32_t disp, uint8_t imm)
{
    x_rex(a, 0, 0, 0, base);
    x_byte(a, 0xf6);
    x_mem(a, 0, base, disp);
    x_byte(a, imm);
}

/* movzx r32, word [base + disp] */
static inline void x_load16(XAsm *a, int dst, int base, int32_t disp)
{
    x_rex(a, 0, dst, 0, base);
    x_byte(a, 0x0f); x_byte(a, 0xb7);
    x_mem(a, dst, base, disp);
}

/* cmp r32, dword [base + disp] */
static inline void x_cmp32_mem(XAsm *a, int r, int base, int32_t disp)
{
    x_rex(a, 0, r, 0, base);
    x_byte(a, 0x3b);
    x_mem(a, r, base, disp);
}

/* ModRM + SIB for [base + index*scale + disp] (index not rsp). */
static void x_mem_sib(XAsm *a, int r, int base, int index, int scale, int32_t disp)
{
    int ss = scale == 8 ? 3 : scale == 4 ? 2 : scale == 2 ? 1 : 0;
    int mod = disp == 0 && (base & 7) != X_RBP ? 0 : (disp >= -128 && disp <= 127 ? 1 : 2);
    x_byte(a, (uint8_t)((mod << 6) | ((r & 7) << 3) | 4));
    x_byte(a, (uint8_t)((ss << 6) | ((index & 7) << 3) | (base & 7)));
    if (mod == 1) x_byte(a, (uint8_t)(int8_t)disp);
    else if (mod == 2) x_u32(a, (uint32_t)disp);
}

/* mov dst, [base + index*8 + disp] */
static inline void x_load_idx8(XAsm *a, int dst, int base, int index, int32_t disp)
{
    x_rex(a, 1, dst, index, base);
    x_byte(a, 0x8b);
    x_mem_sib(a, dst, base, index, 8, disp);
}

/* mov [base + index*8 + disp], src */
static inline void x_store_idx8(XAsm *a, int base, int index, int32_t disp, int src)
{
    x_rex(a, 1, src, index, base);
    x_byte(a, 0x89);
    x_mem_sib(a, src, base, index, 8, disp);
}

/* mov byte [base + index], imm8 */
static inline void x_store8_idx_i(XAsm *a, int base, int index, uint8_t imm)
{
    x_rex(a, 0, 0, index, base);
    x_byte(a, 0xc6);
    x_mem_sib(a, 0, base, index, 1, 0);
    x_byte(a, imm);
}

/* --- SSE2 (xmm0..xmm7 only) ---------------------------------------------- */

/* movq xmm, r64 */
static inline void x_movq_xr(XAsm *a, int x, int r)
{
    x_byte(a, 0x66);
    x_rex(a, 1, x, 0, r);
    x_byte(a, 0x0f); x_byte(a, 0x6e);
    x_modrm_rr(a, x, r);
}

/* movq r64, xmm */
static inline void x_movq_rx(XAsm *a, int r, int x)
{
    x_byte(a, 0x66);
    x_rex(a, 1, x, 0, r);
    x_byte(a, 0x0f); x_byte(a, 0x7e);
    x_modrm_rr(a, x, r);
}

/* addsd/subsd/mulsd/divsd xmm, xmm: 0x58 add, 0x5c sub, 0x59 mul, 0x5e div */
enum { X_ADDSD = 0x58, X_MULSD = 0x59, X_SUBSD = 0x5c, X_DIVSD = 0x5e };
static inline void x_sse_sd(XAsm *a, int op, int dst, int src)
{
    x_byte(a, 0xf2);
    x_byte(a, 0x0f); x_byte(a, (uint8_t)op);
    x_modrm_rr(a, dst, src);
}

/* ucomisd x1, x2 */
static inline void x_ucomisd(XAsm *a, int x1, int x2)
{
    x_byte(a, 0x66);
    x_byte(a, 0x0f); x_byte(a, 0x2e);
    x_modrm_rr(a, x1, x2);
}

/* cvtsi2sd xmm, r32 */
static inline void x_cvtsi2sd32(XAsm *a, int x, int r)
{
    x_byte(a, 0xf2);
    x_rex(a, 0, x, 0, r);
    x_byte(a, 0x0f); x_byte(a, 0x2a);
    x_modrm_rr(a, x, r);
}

#endif /* QUICKJS_T2_X64_H */

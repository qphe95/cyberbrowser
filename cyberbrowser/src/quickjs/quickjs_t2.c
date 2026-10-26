/*
 * quickjs_t2.c - the optimizer, T2 (JS_PERF_DESIGN.md section 6).
 *
 * Step 5b (section 6.7): the skeleton, with no speculation.  A function is
 * built into a graph of nodes, block by block in bytecode order, then
 * emitted as an ordinary Win64 function of the interpreter's frame state.
 * Stack shuffles, local and argument moves, constants and branches are
 * modelled; every other instruction calls its helper (jsop_<op>), the code
 * the interpreter itself runs.  Before a helper, and at the end of every
 * block, the slots the graph holds are stored to the frame, so a helper
 * and every block start see the interpreter's own frame.
 */
#include "quickjs_gc_unified.h"
#include "quickjs.h"
#include "quickjs-internal.h"
#include "quickjs_jit.h"
#include "quickjs_t2.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && defined(__x86_64__) && defined(QJS_VALUE8)
#define T2_SUPPORTED 1
#else
#define T2_SUPPORTED 0
#endif

#if T2_SUPPORTED

#include "quickjs_t2_x64.h"

/* --- Opcodes ------------------------------------------------------------- */

enum {
#define FMT(f) T2_FMT_##f,
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
    T2_OP_COUNT
};

typedef struct T2OpInfo {
    uint8_t size, n_pop, n_push, fmt;
} T2OpInfo;

static const T2OpInfo t2_op_info[256] = {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f) [OP_##id] = { size, n_pop, n_push, T2_FMT_##f },
#define def(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef FMT
#undef DEF
#undef def
};

static const char *const t2_op_name[256] = {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f) [OP_##id] = #id,
#define def(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef FMT
#undef DEF
#undef def
};

static inline uint32_t t2_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t t2_u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static inline uint64_t t2_bits(GCValue v)
{
    uint64_t b;
    _Static_assert(sizeof(GCValue) == 8, "T2 needs 8-byte values");
    memcpy(&b, &v, 8);
    return b;
}

/* --- Stack depths and blocks -------------------------------------------- */

/* The operand-stack depth before each instruction (-1: not reached from
 * the function's start), as compute_stack_size works it out, and where
 * blocks start.  0 when the bytecode cannot be compiled. */
typedef struct T2Shape {
    int16_t *depth;
    uint8_t *block_start;
} T2Shape;

static int t2_npop(const uint8_t *bc, uint32_t pos)
{
    const T2OpInfo *oi = &t2_op_info[bc[pos]];
    int n = oi->n_pop;
    if (oi->fmt == T2_FMT_npop || oi->fmt == T2_FMT_npop_u16)
        n += t2_u16(bc + pos + 1);
    return n;
}

/* The static target of a jump, or -1. */
static int t2_jump_target(const uint8_t *bc, uint32_t pos)
{
    switch (bc[pos]) {
    case OP_goto: case OP_if_true: case OP_if_false: case OP_catch: case OP_gosub:
        return (int)(pos + 1 + (int32_t)t2_u32(bc + pos + 1));
    default:
        return -1;
    }
}

static int t2_ends_block(int op)
{
    switch (op) {
    case OP_goto: case OP_if_true: case OP_if_false:
    case OP_return: case OP_return_undef: case OP_throw: case OP_throw_error:
    case OP_tail_call: case OP_tail_call_method:
        return 1;
    default:
        return 0;
    }
}

static int t2_shape(const T2Source *src, T2Shape *S)
{
    const uint8_t *bc = src->bc;
    uint32_t len = src->len, *work, nwork = 0;
    S->depth = malloc(sizeof(int16_t) * (len + 1));
    S->block_start = calloc(len + 1, 1);
    work = malloc(sizeof(uint32_t) * (len + 1));
    if (!S->depth || !S->block_start || !work) goto fail;
    for (uint32_t i = 0; i <= len; i++) S->depth[i] = -1;
#define T2_REACH(p, d) do {                                                  \
        uint32_t p_ = (p); int d_ = (d);                                     \
        if (p_ >= len || d_ < 0 || d_ > src->stack_size) goto fail;          \
        if (S->depth[p_] < 0) { S->depth[p_] = (int16_t)d_; work[nwork++] = p_; } \
        else if (S->depth[p_] != d_) goto fail;                              \
    } while (0)
    S->block_start[0] = 1;
    T2_REACH(0, 0);
    while (nwork) {
        uint32_t pos = work[--nwork];
        int op = bc[pos], d = S->depth[pos];
        const T2OpInfo *oi = &t2_op_info[op];
        if (!oi->size || pos + oi->size > len) goto fail;
        int npop = t2_npop(bc, pos);
        if (d < npop) goto fail;
        int d2 = d - npop + oi->n_push;
        int target = t2_jump_target(bc, pos);
        if (target >= 0) {
            /* catch: the handler, entered with the catch's offset still
               pushed (compute_stack_size) */
            int dt = op == OP_gosub ? d + 1 : d2;
            T2_REACH((uint32_t)target, dt);
            S->block_start[target] = 1;
        }
        if (t2_ends_block(op) || op == OP_catch || op == OP_gosub) {
            if (pos + oi->size < len)
                S->block_start[pos + oi->size] = 1;
        }
        if (op == OP_goto || op == OP_return || op == OP_return_undef || op == OP_throw ||
            op == OP_throw_error || op == OP_tail_call || op == OP_tail_call_method || op == OP_ret)
            continue;
        T2_REACH(pos + oi->size, d2);
    }
#undef T2_REACH
    free(work);
    return 1;
 fail:
    free(work);
    free(S->depth);
    free(S->block_start);
    S->depth = NULL;
    S->block_start = NULL;
    return 0;
}

/* --- The graph ----------------------------------------------------------- */

enum {
    T2N_CONST,      /* imm: the value's bits */
    T2N_LOAD,       /* slot's frame cell */
    T2N_STORE,      /* slot's frame cell = a */
    T2N_GENERIC,    /* the helper of the instruction at pc, stack depth `depth` */
    T2N_TRUTH,      /* JS_ToBool(a) */
    T2N_BRANCH,     /* to block t if a, else to block e */
    T2N_JUMP,       /* to block t */
    T2N_POLL,       /* JS_POLL_POINT for the jump at pc */
    T2N_RETURN,     /* return a (the instruction at pc) */
};

typedef struct T2Node {
    uint8_t op;
    int32_t a;          /* input node */
    int32_t slot;       /* LOAD, STORE */
    uint32_t pc;        /* the instruction's offset */
    int32_t depth;      /* stack depth before it (GENERIC, POLL, RETURN) */
    int32_t t, e;       /* block targets (offsets) */
    uint64_t imm;
    int32_t last_use;   /* the last node reading it (codegen) */
    int32_t spill;      /* its native stack slot (codegen), -1: none */
} T2Node;

/* What a slot holds while a block is built. */
enum { T2S_FRAME, T2S_ALIAS, T2S_VAL };
typedef struct T2Slot {
    uint8_t kind;
    int32_t n;          /* ALIAS: the slot; VAL: the node */
} T2Slot;

typedef struct T2Builder {
    const T2Source *src;
    T2Shape shape;
    T2Node *nodes;
    int n_nodes, cap_nodes;
    T2Slot *slots;      /* args, then locals, then the operand stack */
    int n_slots;
    int depth;          /* the operand stack's current depth */
    int oom;
    const char *why;    /* why it cannot be compiled */
    int why_op;         /* ... the instruction, or -1 */
} T2Builder;

#define T2_ARG(B, i) (i)
#define T2_LOC(B, i) ((B)->src->arg_count + (i))
#define T2_STK(B, i) ((B)->src->arg_count + (B)->src->var_count + (i))

static int t2_node(T2Builder *B, int op)
{
    if (B->n_nodes == B->cap_nodes) {
        int cap = B->cap_nodes ? 2 * B->cap_nodes : 256;
        T2Node *n = realloc(B->nodes, cap * sizeof(T2Node));
        if (!n) { B->oom = 1; return 0; }
        B->nodes = n;
        B->cap_nodes = cap;
    }
    T2Node *n = &B->nodes[B->n_nodes];
    memset(n, 0, sizeof(*n));
    n->op = (uint8_t)op;
    n->a = n->slot = n->t = n->e = -1;
    n->spill = -1;
    return B->n_nodes++;
}

static int t2_const(T2Builder *B, GCValue v)
{
    int n = t2_node(B, T2N_CONST);
    if (!B->oom) B->nodes[n].imm = t2_bits(v);
    return n;
}

static int t2_load(T2Builder *B, int slot)
{
    int n = t2_node(B, T2N_LOAD);
    if (!B->oom) B->nodes[n].slot = slot;
    return n;
}

/* The node holding slot s's value (loading it if the frame holds it). */
static int t2_value(T2Builder *B, int s)
{
    T2Slot *e = &B->slots[s];
    if (e->kind == T2S_VAL) return e->n;
    int n = t2_load(B, e->kind == T2S_ALIAS ? e->n : s);
    if (!B->oom) { e->kind = T2S_VAL; e->n = n; }
    return n;
}

/* Slot s is about to change: the slots aliasing its frame cell take its
 * current value first. */
static void t2_unalias(T2Builder *B, int s)
{
    int live = T2_STK(B, B->depth);
    for (int k = 0; k < live; k++)
        if (B->slots[k].kind == T2S_ALIAS && B->slots[k].n == s)
            t2_value(B, k);
}

/* Slot dst takes what slot src holds. */
static void t2_copy(T2Builder *B, int dst, int src)
{
    T2Slot v = B->slots[src];
    if (dst == src) return;
    t2_unalias(B, dst);
    if (v.kind == T2S_FRAME) { v.kind = T2S_ALIAS; v.n = src; }
    else if (v.kind == T2S_ALIAS && v.n == dst) v.kind = T2S_FRAME;
    B->slots[dst] = v;
}

static void t2_set_val(T2Builder *B, int s, int n)
{
    t2_unalias(B, s);
    B->slots[s].kind = T2S_VAL;
    B->slots[s].n = n;
}

static inline void t2_push_val(T2Builder *B, int n)
{
    t2_set_val(B, T2_STK(B, B->depth), n);
    B->depth++;
}

static inline void t2_push_copy(T2Builder *B, int src)
{
    t2_copy(B, T2_STK(B, B->depth), src);
    B->depth++;
}

/* Store every slot the graph holds (args, locals, the stack below depth)
 * to its frame cell: loads first, so an alias reads its cell before any
 * store overwrites it.  Afterwards the frame holds every slot. */
static void t2_sync(T2Builder *B)
{
    int live = T2_STK(B, B->depth);
    for (int k = 0; k < live; k++)
        if (B->slots[k].kind == T2S_ALIAS)
            t2_value(B, k);
    for (int k = 0; k < live; k++) {
        if (B->slots[k].kind == T2S_VAL) {
            int n = t2_node(B, T2N_STORE);
            if (B->oom) return;
            B->nodes[n].slot = k;
            B->nodes[n].a = B->slots[k].n;
        }
    }
    for (int k = 0; k < B->n_slots; k++)
        B->slots[k].kind = T2S_FRAME;
}

/* Build the block starting at pos; 0 when the function cannot be
 * compiled. */
static int t2_build_block(T2Builder *B, uint32_t pos)
{
    const T2Source *src = B->src;
    const uint8_t *bc = src->bc;
    for (int k = 0; k < B->n_slots; k++)
        B->slots[k].kind = T2S_FRAME;
    B->depth = B->shape.depth[pos];
    for (;;) {
        int op = bc[pos], n;
        const T2OpInfo *oi = &t2_op_info[op];
        uint32_t next = pos + oi->size;
        if (B->oom) return 0;
        switch (op) {
        case OP_push_i32:
            t2_push_val(B, t2_const(B, JS_MKVAL(JS_TAG_INT, (int32_t)t2_u32(bc + pos + 1))));
            break;
        case OP_push_const:
            t2_push_val(B, t2_const(B, src->cpool[t2_u32(bc + pos + 1)]));
            break;
        case OP_undefined: t2_push_val(B, t2_const(B, JS_UNDEFINED)); break;
        case OP_null: t2_push_val(B, t2_const(B, JS_NULL)); break;
        case OP_push_true: t2_push_val(B, t2_const(B, JS_TRUE)); break;
        case OP_push_false: t2_push_val(B, t2_const(B, JS_FALSE)); break;
        case OP_get_loc: t2_push_copy(B, T2_LOC(B, t2_u16(bc + pos + 1))); break;
        case OP_get_arg: t2_push_copy(B, T2_ARG(B, t2_u16(bc + pos + 1))); break;
        case OP_put_loc:
        case OP_put_arg: {
            int s = op == OP_put_loc ? T2_LOC(B, t2_u16(bc + pos + 1)) : T2_ARG(B, t2_u16(bc + pos + 1));
            t2_copy(B, s, T2_STK(B, B->depth - 1));
            B->depth--;
            break;
        }
        case OP_set_loc:
        case OP_set_arg: {
            int s = op == OP_set_loc ? T2_LOC(B, t2_u16(bc + pos + 1)) : T2_ARG(B, t2_u16(bc + pos + 1));
            t2_copy(B, s, T2_STK(B, B->depth - 1));
            break;
        }
        case OP_drop:
            B->depth--;
            break;
        case OP_nip:        /* a b -> b */
            t2_copy(B, T2_STK(B, B->depth - 2), T2_STK(B, B->depth - 1));
            B->depth--;
            break;
        case OP_dup:
            t2_push_copy(B, T2_STK(B, B->depth - 1));
            break;
        case OP_dup2:       /* a b -> a b a b */
            t2_push_copy(B, T2_STK(B, B->depth - 2));
            t2_push_copy(B, T2_STK(B, B->depth - 2));
            break;
        case OP_swap: {     /* a b -> b a */
            int x = T2_STK(B, B->depth - 2), y = T2_STK(B, B->depth - 1);
            int vx = t2_value(B, x), vy = t2_value(B, y);
            t2_set_val(B, x, vy);
            t2_set_val(B, y, vx);
            break;
        }
        case OP_goto:
        case OP_if_true:
        case OP_if_false: {
            int target = t2_jump_target(bc, pos), cond = -1;
            if (op != OP_goto) {
                cond = t2_value(B, T2_STK(B, B->depth - 1));
                B->depth--;
            }
            t2_sync(B);
            if ((uint32_t)target <= pos) {
                n = t2_node(B, T2N_POLL);
                if (B->oom) return 0;
                B->nodes[n].pc = pos;
                B->nodes[n].depth = B->depth + (op != OP_goto);
            }
            if (op == OP_goto) {
                n = t2_node(B, T2N_JUMP);
                if (B->oom) return 0;
                B->nodes[n].t = target;
            } else {
                int t = t2_node(B, T2N_TRUTH);
                if (B->oom) return 0;
                B->nodes[t].a = cond;
                B->nodes[t].pc = pos;
                n = t2_node(B, T2N_BRANCH);
                if (B->oom) return 0;
                B->nodes[n].a = t;
                B->nodes[n].t = op == OP_if_true ? target : (int)next;
                B->nodes[n].e = op == OP_if_true ? (int)next : target;
            }
            return 1;
        }
        case OP_return:
        case OP_return_undef: {
            int v = op == OP_return ? t2_value(B, T2_STK(B, B->depth - 1)) : t2_const(B, JS_UNDEFINED);
            if (op == OP_return) B->depth--;
            /* the interpreter's return path reads the frame (closing
               captured variables) */
            t2_sync(B);
            n = t2_node(B, T2N_RETURN);
            if (B->oom) return 0;
            B->nodes[n].a = v;
            B->nodes[n].pc = pos;
            B->nodes[n].depth = B->depth;
            return 1;
        }
        default:
            if (src->jumps[op] || op == OP_invalid || src->helpers[op] == src->helpers[OP_invalid]) {
                B->why = "an instruction T2 does not compile";
                B->why_op = op;
                return 0;
            }
            t2_sync(B);
            n = t2_node(B, T2N_GENERIC);
            if (B->oom) return 0;
            B->nodes[n].pc = pos;
            B->nodes[n].depth = B->depth;
            B->nodes[n].imm = op;
            B->depth = B->depth - t2_npop(bc, pos) + oi->n_push;
            if (t2_ends_block(op))
                return 1;   /* throw, tail calls: the helper never continues here */
            break;
        }
        pos = next;
        if (pos >= src->len) {
            B->why = "falls off the end";
            return 0;
        }
        if (B->shape.block_start[pos]) {
            t2_sync(B);
            n = t2_node(B, T2N_JUMP);
            if (B->oom) return 0;
            B->nodes[n].t = pos;
            return 1;
        }
    }
}

/* --- Code generation ----------------------------------------------------- */

/* rbx: F; r12, r13, r14: arg_buf, var_buf, stack_buf. */
#define T2_F    X_RBX
#define T2_ARGS X_R12
#define T2_VARS X_R13
#define T2_STKR X_R14
static const int t2_saved[] = { X_RBX, X_RSI, X_RDI, X_RBP, X_R12, X_R13, X_R14, X_R15 };
#define T2_N_SAVED ((int)(sizeof(t2_saved) / sizeof(t2_saved[0])))
#define T2_MAX_FRAME 3072   /* stays below a page: no stack probe */

typedef struct T2Gen {
    T2Builder *B;
    XAsm a;
    int *block_label;       /* by bytecode offset; -1: none */
    int epilogue;           /* label: eax holds the result */
    uint32_t frame;         /* bytes below the saved registers */
    uint32_t prolog_len;
    uint8_t prolog_at[T2_N_SAVED + 1];  /* offset after each push, then after sub */
} T2Gen;

static void t2_slot_addr(T2Builder *B, int slot, int *base, int32_t *disp)
{
    const T2Source *src = B->src;
    if (slot < src->arg_count) { *base = T2_ARGS; *disp = slot * 8; }
    else if (slot < src->arg_count + src->var_count) { *base = T2_VARS; *disp = (slot - src->arg_count) * 8; }
    else { *base = T2_STKR; *disp = (slot - src->arg_count - src->var_count) * 8; }
}

/* Node n's value into register r. */
static void t2_get(T2Gen *G, int r, int n)
{
    T2Node *N = &G->B->nodes[n];
    if (N->op == T2N_CONST)
        x_mov_ri(&G->a, r, N->imm);
    else
        x_load(&G->a, r, X_RSP, 32 + N->spill * 8);
}

static void t2_put(T2Gen *G, int n, int r)
{
    x_store(&G->a, X_RSP, 32 + G->B->nodes[n].spill * 8, r);
}

/* F->sp and F->pc as the interpreter has them at instruction pc, with
 * stack depth d. */
static void t2_set_frame_pc(T2Gen *G, uint32_t pc, int d)
{
    x_lea(&G->a, X_RAX, T2_STKR, d * 8);
    x_store(&G->a, T2_F, offsetof(JSInterpFrame, sp), X_RAX);
    x_mov_ri(&G->a, X_RAX, (uint64_t)(uintptr_t)(G->B->src->bc + pc + 1));
    x_store(&G->a, T2_F, offsetof(JSInterpFrame, pc), X_RAX);
}

/* Native stack slots: each value gets one at its definition and gives it
 * back after its last use. */
static int t2_assign_spills(T2Builder *B, uint32_t *n_spills)
{
    int *free_list, n_free = 0, top = 0;
    for (int i = 0; i < B->n_nodes; i++) B->nodes[i].last_use = -1;
    for (int i = 0; i < B->n_nodes; i++)
        if (B->nodes[i].a >= 0) B->nodes[B->nodes[i].a].last_use = i;
    free_list = malloc(sizeof(int) * (B->n_nodes + 1));
    if (!free_list) return 0;
    for (int i = 0; i < B->n_nodes; i++) {
        T2Node *N = &B->nodes[i];
        if (N->a >= 0 && B->nodes[N->a].last_use == i && B->nodes[N->a].spill >= 0)
            free_list[n_free++] = B->nodes[N->a].spill;
        if ((N->op == T2N_LOAD || N->op == T2N_TRUTH) && N->last_use >= 0)
            N->spill = n_free ? free_list[--n_free] : top++;
    }
    free(free_list);
    *n_spills = (uint32_t)top;
    return 1;
}

static void t2_emit_node(T2Gen *G, int i)
{
    T2Builder *B = G->B;
    T2Node *N = &B->nodes[i];
    XAsm *a = &G->a;
    int base;
    int32_t disp;
    switch (N->op) {
    case T2N_CONST:
        break;
    case T2N_LOAD:
        if (N->spill < 0) break;
        t2_slot_addr(B, N->slot, &base, &disp);
        x_load(a, X_RAX, base, disp);
        t2_put(G, i, X_RAX);
        break;
    case T2N_STORE:
        t2_get(G, X_RAX, N->a);
        t2_slot_addr(B, N->slot, &base, &disp);
        x_store(a, base, disp, X_RAX);
        break;
    case T2N_GENERIC: {
        int ok = x_new_label(a);
        t2_set_frame_pc(G, N->pc, N->depth);
        x_mov_rr(a, X_RCX, T2_F);
        x_call_abs(a, (const void *)B->src->helpers[N->imm]);
        x_test32(a, X_RAX, X_RAX);
        x_jcc(a, X_CC_E, ok);
        x_jmp(a, G->epilogue);      /* exception, done: the helper set F */
        x_bind(a, ok);
        break;
    }
    case T2N_TRUTH:
        x_mov_rr(a, X_RCX, T2_F);
        t2_get(G, X_RDX, N->a);
        x_call_abs(a, (const void *)t2_to_bool);
        if (N->spill >= 0) t2_put(G, i, X_RAX);
        break;
    case T2N_BRANCH:
        t2_get(G, X_RAX, N->a);
        x_test32(a, X_RAX, X_RAX);
        x_jcc(a, X_CC_NE, G->block_label[N->t]);
        x_jmp(a, G->block_label[N->e]);
        break;
    case T2N_JUMP:
        x_jmp(a, G->block_label[N->t]);
        break;
    case T2N_POLL: {
        int ok = x_new_label(a);
        t2_set_frame_pc(G, N->pc, N->depth);
        x_mov_rr(a, X_RCX, T2_F);
        x_call_abs(a, (const void *)t2_poll);
        x_test32(a, X_RAX, X_RAX);
        x_jcc(a, X_CC_E, ok);
        x_mov_ri(a, X_RAX, JSOP_EXCEPTION);
        x_jmp(a, G->epilogue);
        x_bind(a, ok);
        break;
    }
    case T2N_RETURN:
        t2_get(G, X_RAX, N->a);
        x_store(a, T2_F, offsetof(JSInterpFrame, ret_val), X_RAX);
        t2_set_frame_pc(G, N->pc, N->depth);
        x_mov_ri(a, X_RAX, JSOP_DONE);
        x_jmp(a, G->epilogue);
        break;
    }
}

/* UNWIND_INFO for the prologue: the pushes, then sub rsp. */
static uint32_t t2_unwind_info(T2Gen *G, uint8_t *out)
{
    uint8_t codes[2 * (T2_N_SAVED + 2)];
    int nc = 0;
    /* Codes in reverse order of the prologue. */
    if (G->frame <= 128) {
        codes[nc++] = G->prolog_at[T2_N_SAVED];
        codes[nc++] = (uint8_t)(2 | (((G->frame - 8) / 8) << 4));     /* UWOP_ALLOC_SMALL */
    } else {
        /* UWOP_ALLOC_LARGE, size/8 in the next slot */
        codes[nc++] = G->prolog_at[T2_N_SAVED];
        codes[nc++] = 1;
        codes[nc++] = (uint8_t)((G->frame / 8) & 0xff);
        codes[nc++] = (uint8_t)((G->frame / 8) >> 8);
    }
    for (int k = T2_N_SAVED - 1; k >= 0; k--) {
        codes[nc++] = G->prolog_at[k];
        codes[nc++] = (uint8_t)(0 | ((t2_saved[k] & 15) << 4));      /* UWOP_PUSH_NONVOL */
    }
    int slots = nc / 2;
    out[0] = 1;                         /* version 1, no flags */
    out[1] = (uint8_t)G->prolog_len;
    out[2] = (uint8_t)slots;
    out[3] = 0;                         /* no frame register */
    memcpy(out + 4, codes, nc);
    uint32_t size = 4 + 2 * (uint32_t)((slots + 1) & ~1);
    if (size > 4 + (uint32_t)nc) memset(out + 4 + nc, 0, size - 4 - nc);
    return size;
}

static struct {
    uint64_t compiled, failed, bytes;
} g_t2;

T2Code *t2_compile(const T2Source *src)
{
    T2Builder B = { 0 };
    T2Gen G = { 0 };
    T2Code *tc = NULL;
    uint32_t n_spills = 0;
    B.src = src;
    B.why_op = -1;
    G.B = &B;
    B.n_slots = src->arg_count + src->var_count + src->stack_size + 1;
    B.slots = calloc(B.n_slots, sizeof(T2Slot));
    G.block_label = malloc(sizeof(int) * (src->len + 1));
    if (!B.slots || !G.block_label || !t2_shape(src, &B.shape)) {
        B.why = "its stack depths";
        goto fail;
    }

    /* Build: blocks in bytecode order, each a run of nodes. */
    int *block_first = malloc(sizeof(int) * (src->len + 1));
    if (!block_first) goto fail;
    for (uint32_t p = 0; p <= src->len; p++) {
        block_first[p] = -1;
        G.block_label[p] = -1;
    }
    for (uint32_t p = 0; p < src->len; p++) {
        if (!B.shape.block_start[p] || B.shape.depth[p] < 0) continue;
        block_first[p] = B.n_nodes;
        if (!t2_build_block(&B, p)) { free(block_first); goto fail; }
    }
    if (!t2_assign_spills(&B, &n_spills)) { free(block_first); goto fail; }

    /* Frame: 32 bytes of shadow space and the spills, sized so calls see a
     * 16-byte aligned rsp: the return address and 8 pushes leave rsp at 8
     * mod 16, so the frame is 8 mod 16. */
    G.frame = ((32 + n_spills * 8 + 15) & ~15u) + 8;
    if (G.frame > T2_MAX_FRAME) { B.why = "too many values"; free(block_first); goto fail; }

    XAsm *a = &G.a;
    G.epilogue = x_new_label(a);
    for (uint32_t p = 0; p < src->len; p++)
        if (block_first[p] >= 0) G.block_label[p] = x_new_label(a);

    /* Prologue. */
    for (int k = 0; k < T2_N_SAVED; k++) {
        x_push(a, t2_saved[k]);
        G.prolog_at[k] = (uint8_t)a->len;
    }
    x_byte(a, 0x48); x_byte(a, 0x81); x_byte(a, 0xec); x_u32(a, G.frame);    /* sub rsp, imm32 */
    G.prolog_at[T2_N_SAVED] = (uint8_t)a->len;
    G.prolog_len = a->len;
    x_mov_rr(a, T2_F, X_RCX);
    x_load(a, T2_ARGS, T2_F, offsetof(JSInterpFrame, arg_buf));
    x_load(a, T2_VARS, T2_F, offsetof(JSInterpFrame, var_buf));
    x_load(a, T2_STKR, T2_F, offsetof(JSInterpFrame, stack_buf));

    /* Blocks. */
    for (uint32_t p = 0; p < src->len; p++) {
        if (block_first[p] < 0) continue;
        x_bind(a, G.block_label[p]);
        int end = B.n_nodes;
        for (uint32_t q = p + 1; q < src->len; q++)
            if (block_first[q] >= 0) { end = block_first[q]; break; }
        for (int i = block_first[p]; i < end; i++)
            t2_emit_node(&G, i);
    }
    free(block_first);

    /* Epilogue: eax holds the result. */
    x_bind(a, G.epilogue);
    x_byte(a, 0x48); x_byte(a, 0x81); x_byte(a, 0xc4); x_u32(a, G.frame);    /* add rsp, imm32 */
    for (int k = T2_N_SAVED - 1; k >= 0; k--)
        x_pop(a, t2_saved[k]);
    x_ret(a);
    if (x_resolve(a)) { B.why = "assembling"; goto fail; }

    /* Install: code, then its unwind info. */
    {
        uint8_t xdata[64];
        uint32_t xsize = t2_unwind_info(&G, xdata);
        uint32_t code_len = (a->len + 3) & ~3u;
        uint32_t total = code_len + xsize;
        uint8_t *p = jit_code_alloc(total);
        tc = malloc(sizeof(T2Code));
        if (!p || !tc) { B.why = "code memory"; goto fail; }
        uint8_t *w = jit_code_rw(p);
        memcpy(w, a->buf, a->len);
        memset(w + a->len, 0xcc, code_len - a->len);
        memcpy(w + code_len, xdata, xsize);
        if (jit_code_publish(p, a->len, code_len, total, src->name)) { B.why = "unwind table"; goto fail; }
        tc->entry = (int (*)(JSInterpFrame *))(void *)p;
        tc->size = total;
        g_t2.bytes += total;
    }
    g_t2.compiled++;
    x_free(&G.a);
    free(B.nodes);
    free(B.slots);
    free(B.shape.depth);
    free(B.shape.block_start);
    free(G.block_label);
    if (getenv("QJS_T2_LOG"))
        fprintf(stderr, "[t2] compiled %s: %u bytecode bytes, %d nodes, %u bytes\n",
                src->name ? src->name : "?", src->len, B.n_nodes, tc->size);
    return tc;

 fail:
    g_t2.failed++;
    if (getenv("QJS_T2_LOG"))
        fprintf(stderr, "[t2] not compiled %s: %s%s%s\n", src->name ? src->name : "?",
                B.oom ? "out of memory" : B.why ? B.why : "?",
                B.why_op >= 0 ? ": " : "", B.why_op >= 0 && t2_op_name[B.why_op] ? t2_op_name[B.why_op] : "");
    free(tc);
    x_free(&G.a);
    free(B.nodes);
    free(B.slots);
    free(B.shape.depth);
    free(B.shape.block_start);
    free(G.block_label);
    return NULL;
}

int t2_available(void)
{
    return jit_code_alloc(0) != NULL || jit_available();
}

void t2_print_stats(void)
{
    fprintf(stderr, "[t2] %llu functions compiled (%llu bytes), %llu not compiled\n",
            (unsigned long long)g_t2.compiled, (unsigned long long)g_t2.bytes,
            (unsigned long long)g_t2.failed);
}

#else /* !T2_SUPPORTED */

T2Code *t2_compile(const T2Source *src) { (void)src; return NULL; }
int t2_available(void) { return 0; }
void t2_print_stats(void) {}

#endif

/*
 * quickjs_t2.c - the optimizer, T2 (JS_PERF_DESIGN.md section 6).
 *
 * A function is built into a graph of nodes, block by block in bytecode
 * order, then emitted as an ordinary Win64 function of the interpreter's
 * frame state (section 6.7).  Stack shuffles, local and argument moves,
 * constants and branches are modelled; so is arithmetic, speculated on
 * int32s or float64s (step 5c) behind guards that leave for the
 * interpreter at the instruction's start when they fail.  Every other
 * instruction calls its helper (jsop_<op>), the code the interpreter itself
 * runs.  Before a helper, and at the end of every block, the slots the
 * graph holds are stored to the frame (boxed), so a helper and every block
 * start see the interpreter's own frame.
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
    T2N_CONST,      /* imm: the value (raw bits in its representation) */
    T2N_LOAD,       /* slot's frame cell */
    T2N_STORE,      /* slot's frame cell = a (boxed) */
    T2N_GENERIC,    /* the helper of the instruction at pc, stack depth `depth` */
    T2N_TRUTH,      /* JS_ToBool(a), a tagged */
    T2N_BRANCH,     /* to block t if a (bool or int32) is not 0, else to block e */
    T2N_JUMP,       /* to block t */
    T2N_POLL,       /* JS_POLL_POINT for the jump at pc */
    T2N_RETURN,     /* return a (the instruction at pc) */
    /* Speculation (5c): a node with an exit leaves through it when its
       guard fails. */
    T2N_UNBOX_I32,  /* tagged a, an int32 */
    T2N_UNBOX_F64,  /* tagged a, a number */
    T2N_I32_TO_F64,
    T2N_I32_ADD, T2N_I32_SUB, T2N_I32_MUL,  /* exit on overflow (mul: and -0) */
    T2N_I32_AND, T2N_I32_OR, T2N_I32_XOR,
    T2N_I32_SHL, T2N_I32_SAR, T2N_I32_SHR,  /* shr: exit when the result is not an int32 */
    T2N_I32_NEG,    /* exit on 0 (-0) and INT32_MIN */
    T2N_I32_CMP,    /* bool: a cc b */
    T2N_F64_ADD, T2N_F64_SUB, T2N_F64_MUL, T2N_F64_DIV,
    T2N_F64_NEG,
    T2N_F64_CMP,    /* bool: a cc b (false when unordered, but !=) */
    T2N_BOOL_NOT,
    /* Objects (5d): a guarded object a, from the site's inline cache
       (ic: B->ics) */
    T2N_GET_FIELD,  /* a.atom */
    T2N_PUT_FIELD,  /* a.atom = b (an own writable property) */
    T2N_GET_ELEM,   /* a[b], a a fast array, b an int32 in bounds */
    T2N_PUT_ELEM,   /* a[b] = c */
};

/* A value's representation. */
enum { T2R_TAGGED, T2R_I32, T2R_BOOL, T2R_F64, T2R_NONE };

/* Comparisons (T2N_I32_CMP, T2N_F64_CMP). */
enum { T2C_LT, T2C_LE, T2C_GT, T2C_GE, T2C_EQ, T2C_NE };

typedef struct T2Node {
    uint8_t op;
    uint8_t rep;        /* of its result */
    uint8_t cc;         /* comparisons */
    int32_t a, b, c;    /* inputs */
    int32_t ic;         /* GET_FIELD, PUT_FIELD: its ways (B->ics) */
    JSAtom atom;        /* GET_FIELD, PUT_FIELD */
    int32_t slot;       /* LOAD, STORE */
    uint32_t pc;        /* the instruction's offset */
    int32_t depth;      /* stack depth before it (GENERIC, POLL, RETURN) */
    int32_t t, e;       /* block targets (offsets) */
    int32_t exit;       /* its exit (T2Exit), -1: none */
    int32_t i32_of, f64_of;     /* its unboxed copies in this block, -1: none */
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

/* Where a failed guard leaves: the instruction it belongs to, run again by
 * the interpreter.  The frame then needs the slots the graph held there
 * (entries: slot and value, boxed when stored), and the instruction's
 * operands tell t2_deopt what the site saw. */
typedef struct T2Exit {
    uint32_t pc;
    int32_t depth;
    int32_t first, n;           /* its entries */
    uint8_t *state;             /* the site's arithmetic state */
    int32_t opnd[2];            /* operand slots, -1: none */
    int label;                  /* codegen */
} T2Exit;

typedef struct T2Entry {
    int32_t slot, node;
} T2Entry;

typedef struct T2Builder {
    const T2Source *src;
    T2Shape shape;
    T2Node *nodes;
    int n_nodes, cap_nodes;
    T2Exit *exits;
    int n_exits, cap_exits;
    T2Entry *entries;
    int n_entries, cap_entries;
    T2Slot *slots;      /* args, then locals, then the operand stack */
    int n_slots;
    int depth;          /* the operand stack's current depth */
    int oom;
    const char *why;    /* why it cannot be compiled */
    int why_op;         /* ... the instruction, or -1 */
    int cur_exit;       /* the exit of the instruction being built, -1: none yet */
    uint32_t cur_pc;
    JitIC *ics;         /* the inline caches compiled inline, as they were */
    int n_ics, cap_ics;
} T2Builder;

#define T2_ARG(B, i) (i)
#define T2_LOC(B, i) ((B)->src->arg_count + (i))
#define T2_STK(B, i) ((B)->src->arg_count + (B)->src->var_count + (i))

/* Whether instruction op has a state byte in the feedback vector (the
 * arithmetic sites of js_feedback_op_site_kind, and array accesses). */
static int t2_has_state(int op)
{
    switch (op) {
    case OP_add: case OP_sub: case OP_mul: case OP_div:
    case OP_lt: case OP_lte: case OP_gt: case OP_gte:
    case OP_eq: case OP_neq: case OP_strict_eq: case OP_strict_neq:
    case OP_and: case OP_or: case OP_xor: case OP_shl: case OP_sar: case OP_shr:
    case OP_inc: case OP_dec: case OP_neg: case OP_post_inc: case OP_post_dec:
    case OP_inc_loc: case OP_dec_loc: case OP_add_loc:
    case OP_get_array_el: case OP_get_array_el2: case OP_put_array_el:
        return 1;
    default:
        return 0;
    }
}

static int t2_grow(T2Builder *B, void **p, int *cap, int n, size_t elem)
{
    if (n < *cap) return 1;
    int c = *cap ? 2 * *cap : 256;
    void *q = realloc(*p, (size_t)c * elem);
    if (!q) { B->oom = 1; return 0; }
    *p = q;
    *cap = c;
    return 1;
}

static int t2_node(T2Builder *B, int op, int rep)
{
    if (!t2_grow(B, (void **)&B->nodes, &B->cap_nodes, B->n_nodes, sizeof(T2Node))) return 0;
    T2Node *n = &B->nodes[B->n_nodes];
    memset(n, 0, sizeof(*n));
    n->op = (uint8_t)op;
    n->rep = (uint8_t)rep;
    n->a = n->b = n->c = n->ic = n->slot = n->t = n->e = n->exit = n->i32_of = n->f64_of = -1;
    n->spill = -1;
    return B->n_nodes++;
}

static int t2_node2(T2Builder *B, int op, int rep, int a, int b)
{
    int n = t2_node(B, op, rep);
    if (!B->oom) { B->nodes[n].a = a; B->nodes[n].b = b; }
    return n;
}

static int t2_const_raw(T2Builder *B, int rep, uint64_t bits)
{
    int n = t2_node(B, T2N_CONST, rep);
    if (!B->oom) B->nodes[n].imm = bits;
    return n;
}

static int t2_const(T2Builder *B, GCValue v)
{
    return t2_const_raw(B, T2R_TAGGED, t2_bits(v));
}

static int t2_load(T2Builder *B, int slot)
{
    int n = t2_node(B, T2N_LOAD, T2R_TAGGED);
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
            int n = t2_node(B, T2N_STORE, T2R_NONE);
            if (B->oom) return;
            B->nodes[n].slot = k;
            B->nodes[n].a = B->slots[k].n;
        }
    }
    for (int k = 0; k < B->n_slots; k++)
        B->slots[k].kind = T2S_FRAME;
}

/* The exit of the instruction being built, made the first time a guard
 * needs it: the frame state before the instruction.  Aliases are resolved
 * first, so the exit only stores values. */
static int t2_exit(T2Builder *B, int opnd0, int opnd1)
{
    if (B->cur_exit >= 0) return B->cur_exit;
    int live = T2_STK(B, B->depth);
    for (int k = 0; k < live; k++)
        if (B->slots[k].kind == T2S_ALIAS)
            t2_value(B, k);
    if (!t2_grow(B, (void **)&B->exits, &B->cap_exits, B->n_exits, sizeof(T2Exit))) return -1;
    T2Exit *x = &B->exits[B->n_exits];
    x->pc = B->cur_pc;
    x->depth = B->depth;
    x->first = B->n_entries;
    x->n = 0;
    x->state = NULL;
    if (t2_has_state(B->src->bc[B->cur_pc]) && B->src->site_of && B->src->arith &&
        B->src->site_of[B->cur_pc] != 0xffff)
        x->state = &B->src->arith[B->src->site_of[B->cur_pc]];
    x->opnd[0] = opnd0;
    x->opnd[1] = opnd1;
    x->label = -1;
    for (int k = 0; k < live; k++) {
        if (B->slots[k].kind != T2S_VAL) continue;
        if (!t2_grow(B, (void **)&B->entries, &B->cap_entries, B->n_entries, sizeof(T2Entry))) return -1;
        B->entries[B->n_entries].slot = k;
        B->entries[B->n_entries].node = B->slots[k].n;
        B->n_entries++;
        x->n++;
    }
    B->cur_exit = B->n_exits++;
    return B->cur_exit;
}

/* The arithmetic state of the instruction being built (0: nothing has made
 * speculation fail yet, or no feedback); array accesses have one too. */
static int t2_arith_state(T2Builder *B)
{
    const T2Source *src = B->src;
    if (!src->site_of || !src->arith || src->site_of[B->cur_pc] == 0xffff)
        return T2_ARITH_OTHER;      /* no state to record a failure in */
    return src->arith[src->site_of[B->cur_pc]];
}

/* Node n as an int32 (guarded through the instruction's exit when it is
 * tagged), or -1 when it cannot be one. */
static int t2_as_i32(T2Builder *B, int n, int opnd0, int opnd1)
{
    T2Node *N = &B->nodes[n];
    if (N->rep == T2R_I32) return n;
    if (N->rep != T2R_TAGGED) return -1;
    if (N->op == T2N_CONST) {
        GCValue v;
        memcpy(&v, &N->imm, 8);
        if (GC_VALUE_GET_TAG(v) != JS_TAG_INT) return -1;
        return t2_const_raw(B, T2R_I32, (uint32_t)GC_VALUE_GET_INT(v));
    }
    if (N->i32_of >= 0) return N->i32_of;
    int x = t2_exit(B, opnd0, opnd1);
    if (x < 0) return -1;
    int u = t2_node2(B, T2N_UNBOX_I32, T2R_I32, n, -1);
    if (B->oom) return -1;
    B->nodes[u].exit = x;
    B->nodes[n].i32_of = u;
    return u;
}

/* Node n as a float64, or -1 when it cannot be one. */
static int t2_as_f64(T2Builder *B, int n, int opnd0, int opnd1)
{
    T2Node *N = &B->nodes[n];
    if (N->rep == T2R_F64) return n;
    if (N->rep == T2R_I32) {
        if (N->f64_of >= 0) return N->f64_of;
        int c = t2_node2(B, T2N_I32_TO_F64, T2R_F64, n, -1);
        if (!B->oom) B->nodes[n].f64_of = c;
        return B->oom ? -1 : c;
    }
    if (N->rep != T2R_TAGGED) return -1;
    if (N->op == T2N_CONST) {
        GCValue v;
        double d;
        uint64_t bits;
        memcpy(&v, &N->imm, 8);
        if (GC_VALUE_GET_TAG(v) == JS_TAG_INT) d = GC_VALUE_GET_INT(v);
        else if (GC_IS_FLOAT64(v)) d = GC_VALUE_GET_FLOAT64(v);
        else return -1;
        memcpy(&bits, &d, 8);
        return t2_const_raw(B, T2R_F64, bits);
    }
    if (N->f64_of >= 0) return N->f64_of;
    int x = t2_exit(B, opnd0, opnd1);
    if (x < 0) return -1;
    int u = t2_node2(B, T2N_UNBOX_F64, T2R_F64, n, -1);
    if (B->oom) return -1;
    B->nodes[u].exit = x;
    B->nodes[n].f64_of = u;
    return u;
}

/* Whether node n is a constant that is not a number: speculating on it
 * would only fail. */
static int t2_const_not_number(T2Builder *B, int n)
{
    T2Node *N = &B->nodes[n];
    GCValue v;
    if (N->op != T2N_CONST || N->rep != T2R_TAGGED) return 0;
    memcpy(&v, &N->imm, 8);
    return GC_VALUE_GET_TAG(v) != JS_TAG_INT && !GC_IS_FLOAT64(v);
}

/* A binary arithmetic or comparison instruction on the two stack tops,
 * speculated on int32s or float64s as the site's state says: 1 when built,
 * 0 for the helper. */
static int t2_binary(T2Builder *B, int op)
{
    int sa = T2_STK(B, B->depth - 2), sb = T2_STK(B, B->depth - 1);
    int state = t2_arith_state(B);
    int i32op = -1, f64op = -1, cc = -1, r = -1;
    switch (op) {
    case OP_add: i32op = T2N_I32_ADD; f64op = T2N_F64_ADD; break;
    case OP_sub: i32op = T2N_I32_SUB; f64op = T2N_F64_SUB; break;
    case OP_mul: i32op = T2N_I32_MUL; f64op = T2N_F64_MUL; break;
    case OP_div: f64op = T2N_F64_DIV; break;
    case OP_and: i32op = T2N_I32_AND; break;
    case OP_or: i32op = T2N_I32_OR; break;
    case OP_xor: i32op = T2N_I32_XOR; break;
    case OP_shl: i32op = T2N_I32_SHL; break;
    case OP_sar: i32op = T2N_I32_SAR; break;
    case OP_shr: i32op = T2N_I32_SHR; break;
    case OP_lt: cc = T2C_LT; break;
    case OP_lte: cc = T2C_LE; break;
    case OP_gt: cc = T2C_GT; break;
    case OP_gte: cc = T2C_GE; break;
    case OP_eq: case OP_strict_eq: cc = T2C_EQ; break;
    case OP_neq: case OP_strict_neq: cc = T2C_NE; break;
    default: return 0;
    }
    if (cc >= 0) { i32op = T2N_I32_CMP; f64op = T2N_F64_CMP; }
    if (state & T2_ARITH_OTHER) return 0;
    int na = t2_value(B, sa), nb = t2_value(B, sb);
    if (B->oom || t2_const_not_number(B, na) || t2_const_not_number(B, nb)) return 0;
    int ra = B->nodes[na].rep, rb = B->nodes[nb].rep;
    if (ra == T2R_BOOL || rb == T2R_BOOL) return 0;
    /* an operand already a float64: the float64 path, if there is one */
    if (ra == T2R_F64 || rb == T2R_F64) {
        if (f64op < 0) return 0;
        state |= T2_ARITH_DOUBLE;
    }
    if (!(state & T2_ARITH_DOUBLE) && i32op >= 0) {
        int ia = t2_as_i32(B, na, sa, sb), ib = ia < 0 ? -1 : t2_as_i32(B, nb, sa, sb);
        if (ia < 0 || ib < 0) return 0;
        /* the exit before its guard: making it may load slots */
        int x = i32op == T2N_I32_ADD || i32op == T2N_I32_SUB || i32op == T2N_I32_MUL ||
                i32op == T2N_I32_SHR ? t2_exit(B, sa, sb) : -1;
        r = t2_node2(B, i32op, cc >= 0 ? T2R_BOOL : T2R_I32, ia, ib);
        if (B->oom) return 0;
        B->nodes[r].exit = x;
    } else if (f64op >= 0 && (state & T2_ARITH_DOUBLE || i32op < 0)) {
        int fa = t2_as_f64(B, na, sa, sb), fb = fa < 0 ? -1 : t2_as_f64(B, nb, sa, sb);
        if (fa < 0 || fb < 0) return 0;
        r = t2_node2(B, f64op, cc >= 0 ? T2R_BOOL : T2R_F64, fa, fb);
    } else {
        return 0;
    }
    if (B->oom) return 0;
    B->nodes[r].cc = (uint8_t)(cc < 0 ? 0 : cc);
    B->depth -= 2;
    t2_push_val(B, r);
    return 1;
}

/* x + delta (inc, dec, post_inc, post_dec, inc_loc, dec_loc) on slot s's
 * value: the result node, or -1 for the helper. */
static int t2_add_const(T2Builder *B, int s, int delta)
{
    int state = t2_arith_state(B), n = t2_value(B, s), r;
    if (B->oom || state & T2_ARITH_OTHER) return -1;
    if (!(state & T2_ARITH_DOUBLE)) {
        int ia = t2_as_i32(B, n, s, -1);
        if (ia < 0) return -1;
        int x = t2_exit(B, s, -1), c = t2_const_raw(B, T2R_I32, (uint32_t)delta);
        r = t2_node2(B, T2N_I32_ADD, T2R_I32, ia, c);
        if (B->oom) return -1;
        B->nodes[r].exit = x;
    } else {
        double d = delta;
        uint64_t bits;
        int fa = t2_as_f64(B, n, s, -1);
        if (fa < 0) return -1;
        memcpy(&bits, &d, 8);
        r = t2_node2(B, T2N_F64_ADD, T2R_F64, fa, t2_const_raw(B, T2R_F64, bits));
    }
    return B->oom ? -1 : r;
}

/* The inline cache of the property site at the instruction being built, if
 * its ways can be compiled inline: own data properties, and for gets also
 * prototype data properties and absent properties (not getters, not a
 * megamorphic site); for puts only own writable ones (no adds).  Its shapes
 * and holders are retained: the code names them by handle.  The cache's
 * index in B->ics, or -1. */
static int t2_inline_ic(T2Builder *B, int is_put)
{
    const T2Source *src = B->src;
    if (!src->ics || !src->site_of || src->site_of[B->cur_pc] == 0xffff) return -1;
    const JitIC *ic = &src->ics[src->site_of[B->cur_pc]];
    if (ic->mega || ic->n == 0 || ic->n > JIT_IC_WAYS) return -1;
    for (int k = 0; k < ic->n; k++) {
        uint32_t slot = ic->slot[k];
        if (is_put) {
            if (ic->holder[k] != GC_HANDLE_NULL || slot >= 0x7fffffffu) return -1;   /* adds, getters */
        } else if (slot != JIT_IC_ABSENT && (slot & 0x80000000u)) {
            return -1;                                                          /* getters */
        }
    }
    if (!is_put && ic->epoch != t2_proto_epoch(t2_u32(src->bc + B->cur_pc + 1))) {
        /* prototype ways already stale: compile only the own ones */
        for (int k = 0; k < ic->n; k++)
            if (ic->holder[k] != GC_HANDLE_NULL) return -1;
    }
    for (int k = 0; k < ic->n; k++) {
        if (!t2_retain(ic->shape[k])) return -1;
        if (ic->holder[k] != GC_HANDLE_NULL && ic->slot[k] != JIT_IC_ABSENT && !t2_retain(ic->holder[k]))
            return -1;
    }
    if (!t2_grow(B, (void **)&B->ics, &B->cap_ics, B->n_ics, sizeof(JitIC))) return -1;
    B->ics[B->n_ics] = *ic;
    return B->n_ics++;
}

/* get_field / get_field2 / put_field inline from the site's cache: 1 when
 * built, 0 for the helper. */
static int t2_field(T2Builder *B, int op, JSAtom atom)
{
    int is_put = op == OP_put_field;
    int so = T2_STK(B, B->depth - (is_put ? 2 : 1));
    int ic = t2_inline_ic(B, is_put);
    if (ic < 0) return 0;
    int no = t2_value(B, so);
    if (B->oom || B->nodes[no].rep != T2R_TAGGED) return 0;
    int nv = -1;
    if (is_put) {
        nv = t2_value(B, T2_STK(B, B->depth - 1));
        if (B->oom) return 0;
    }
    int x = t2_exit(B, -1, -1);
    int r = t2_node2(B, is_put ? T2N_PUT_FIELD : T2N_GET_FIELD, is_put ? T2R_NONE : T2R_TAGGED, no, nv);
    if (B->oom || x < 0) return 0;
    B->nodes[r].exit = x;
    B->nodes[r].ic = ic;
    B->nodes[r].atom = atom;
    if (is_put) {
        B->depth -= 2;
    } else if (op == OP_get_field) {
        B->depth--;
        t2_push_val(B, r);
    } else {
        t2_push_val(B, r);  /* get_field2: obj obj.f */
    }
    return 1;
}

/* get_array_el / get_array_el2 / put_array_el on a fast array: 1 when
 * built, 0 for the helper. */
static int t2_elem(T2Builder *B, int op)
{
    int is_put = op == OP_put_array_el;
    int d = B->depth, so = T2_STK(B, d - (is_put ? 3 : 2)), si = T2_STK(B, d - (is_put ? 2 : 1));
    if (t2_arith_state(B) & T2_ARITH_OTHER) return 0;
    int no = t2_value(B, so), ni = t2_value(B, si), nv = -1;
    if (is_put) nv = t2_value(B, T2_STK(B, d - 1));
    if (B->oom || B->nodes[no].rep != T2R_TAGGED || t2_const_not_number(B, ni)) return 0;
    int ii = t2_as_i32(B, ni, -1, -1);
    if (ii < 0) return 0;
    int x = t2_exit(B, -1, -1);
    int r = t2_node2(B, is_put ? T2N_PUT_ELEM : T2N_GET_ELEM, is_put ? T2R_NONE : T2R_TAGGED, no, ii);
    if (B->oom || x < 0) return 0;
    B->nodes[r].c = nv;
    B->nodes[r].exit = x;
    if (is_put) {
        B->depth -= 3;
    } else if (op == OP_get_array_el) {
        B->depth -= 2;
        t2_push_val(B, r);
    } else {
        B->depth--;         /* get_array_el2: obj prop -> obj value */
        t2_push_val(B, r);
    }
    return 1;
}

/* Generic: the instruction at pos through its helper. */
static int t2_generic(T2Builder *B, uint32_t pos)
{
    const uint8_t *bc = B->src->bc;
    int op = bc[pos];
    t2_sync(B);
    int n = t2_node(B, T2N_GENERIC, T2R_NONE);
    if (B->oom) return 0;
    B->nodes[n].pc = pos;
    B->nodes[n].depth = B->depth;
    B->nodes[n].imm = op;
    B->depth = B->depth - t2_npop(bc, pos) + t2_op_info[op].n_push;
    return 1;
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
        B->cur_exit = -1;
        B->cur_pc = pos;
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
        case OP_add: case OP_sub: case OP_mul: case OP_div:
        case OP_and: case OP_or: case OP_xor: case OP_shl: case OP_sar: case OP_shr:
        case OP_lt: case OP_lte: case OP_gt: case OP_gte:
        case OP_eq: case OP_neq: case OP_strict_eq: case OP_strict_neq:
            if (!t2_binary(B, op) && !t2_generic(B, pos))
                return 0;
            break;
        case OP_get_field:
        case OP_get_field2:
        case OP_put_field:
            if (!t2_field(B, op, t2_u32(bc + pos + 1)) && !t2_generic(B, pos))
                return 0;
            break;
        case OP_get_array_el:
        case OP_get_array_el2:
        case OP_put_array_el:
            if (!t2_elem(B, op) && !t2_generic(B, pos))
                return 0;
            break;
        case OP_inc:
        case OP_dec: {
            int r = t2_add_const(B, T2_STK(B, B->depth - 1), op == OP_inc ? 1 : -1);
            if (r < 0) { if (!t2_generic(B, pos)) return 0; break; }
            B->depth--;
            t2_push_val(B, r);
            break;
        }
        case OP_post_inc:
        case OP_post_dec: { /* a -> a a+1: a stays as it is when it is a number */
            int r = t2_add_const(B, T2_STK(B, B->depth - 1), op == OP_post_inc ? 1 : -1);
            if (r < 0) { if (!t2_generic(B, pos)) return 0; break; }
            t2_push_val(B, r);
            break;
        }
        case OP_inc_loc:
        case OP_dec_loc: {
            int s = T2_LOC(B, bc[pos + 1]);
            int r = t2_add_const(B, s, op == OP_inc_loc ? 1 : -1);
            if (r < 0) { if (!t2_generic(B, pos)) return 0; break; }
            t2_set_val(B, s, r);
            break;
        }
        case OP_add_loc: {  /* loc += pop */
            int s = T2_LOC(B, bc[pos + 1]), sb = T2_STK(B, B->depth - 1);
            int state = t2_arith_state(B), r = -1;
            if (!(state & T2_ARITH_OTHER)) {
                int na = t2_value(B, s), nb = t2_value(B, sb);
                if (!t2_const_not_number(B, nb)) {
                    if (!(state & T2_ARITH_DOUBLE)) {
                        int ia = t2_as_i32(B, na, s, sb), ib = ia < 0 ? -1 : t2_as_i32(B, nb, s, sb);
                        if (ia >= 0 && ib >= 0) {
                            int x = t2_exit(B, s, sb);
                            r = t2_node2(B, T2N_I32_ADD, T2R_I32, ia, ib);
                            if (!B->oom) B->nodes[r].exit = x;
                        }
                    } else {
                        int fa = t2_as_f64(B, na, s, sb), fb = fa < 0 ? -1 : t2_as_f64(B, nb, s, sb);
                        if (fa >= 0 && fb >= 0)
                            r = t2_node2(B, T2N_F64_ADD, T2R_F64, fa, fb);
                    }
                }
            }
            if (B->oom) return 0;
            if (r < 0) { if (!t2_generic(B, pos)) return 0; break; }
            B->depth--;
            t2_set_val(B, s, r);
            break;
        }
        case OP_neg: {
            int s = T2_STK(B, B->depth - 1), state = t2_arith_state(B), r = -1;
            if (!(state & T2_ARITH_OTHER)) {
                int na = t2_value(B, s);
                if (!(state & T2_ARITH_DOUBLE)) {
                    int ia = t2_as_i32(B, na, s, -1);
                    if (ia >= 0) {
                        int x = t2_exit(B, s, -1);
                        r = t2_node2(B, T2N_I32_NEG, T2R_I32, ia, -1);
                        if (!B->oom) B->nodes[r].exit = x;
                    }
                } else {
                    int fa = t2_as_f64(B, na, s, -1);
                    if (fa >= 0) r = t2_node2(B, T2N_F64_NEG, T2R_F64, fa, -1);
                }
            }
            if (B->oom) return 0;
            if (r < 0) { if (!t2_generic(B, pos)) return 0; break; }
            B->depth--;
            t2_push_val(B, r);
            break;
        }
        case OP_lnot: {     /* of a known boolean; anything else through the helper */
            int s = T2_STK(B, B->depth - 1);
            if (B->slots[s].kind == T2S_VAL && B->nodes[B->slots[s].n].rep == T2R_BOOL) {
                int r = t2_node2(B, T2N_BOOL_NOT, T2R_BOOL, B->slots[s].n, -1);
                if (B->oom) return 0;
                B->depth--;
                t2_push_val(B, r);
            } else if (!t2_generic(B, pos)) {
                return 0;
            }
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
                n = t2_node(B, T2N_POLL, T2R_NONE);
                if (B->oom) return 0;
                B->nodes[n].pc = pos;
                B->nodes[n].depth = B->depth + (op != OP_goto);
            }
            if (op == OP_goto) {
                n = t2_node(B, T2N_JUMP, T2R_NONE);
                if (B->oom) return 0;
                B->nodes[n].t = target;
            } else {
                int t = cond;
                if (B->nodes[cond].rep != T2R_BOOL && B->nodes[cond].rep != T2R_I32) {
                    t = t2_node2(B, T2N_TRUTH, T2R_BOOL, cond, -1);
                    if (B->oom) return 0;
                    B->nodes[t].pc = pos;
                }
                n = t2_node(B, T2N_BRANCH, T2R_NONE);
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
            n = t2_node(B, T2N_RETURN, T2R_NONE);
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
            if (!t2_generic(B, pos))
                return 0;
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
            n = t2_node(B, T2N_JUMP, T2R_NONE);
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
#define T2_HEAP X_R15   /* g_gc.heap_base: a handle's object is at heap + h*16 + 16 */
static const int t2_saved[] = { X_RBX, X_RSI, X_RDI, X_RBP, X_R12, X_R13, X_R14, X_R15 };
#define T2_N_SAVED ((int)(sizeof(t2_saved) / sizeof(t2_saved[0])))
#define T2_MAX_FRAME 3072   /* stays below a page: no stack probe */
#define T2_F64_ADDEND ((uint64_t)GC_FLOAT64_TAG_ADDEND << 32)

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

/* Node n's raw bits (its representation's) into register r. */
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
    if (G->B->nodes[n].spill >= 0)
        x_store(&G->a, X_RSP, 32 + G->B->nodes[n].spill * 8, r);
}

/* Node n's value as a tagged GCValue into rax (clobbers rcx, rdx). */
static void t2_get_boxed(T2Gen *G, int n)
{
    XAsm *a = &G->a;
    T2Node *N = &G->B->nodes[n];
    if (N->op == T2N_CONST) {
        GCValue v;
        if (N->rep == T2R_I32) v = JS_MKVAL(JS_TAG_INT, (int32_t)(uint32_t)N->imm);
        else if (N->rep == T2R_BOOL) v = JS_MKVAL(JS_TAG_BOOL, N->imm != 0);
        else if (N->rep == T2R_F64) { double d; memcpy(&d, &N->imm, 8); v = GC_NewFloat64(d); }
        else memcpy(&v, &N->imm, 8);
        x_mov_ri(a, X_RAX, t2_bits(v));
        return;
    }
    t2_get(G, X_RAX, n);
    switch (N->rep) {
    case T2R_I32:       /* an int32's value is its zero-extended bits */
        x_byte(a, 0x89); x_byte(a, 0xc0);   /* mov eax, eax */
        break;
    case T2R_BOOL:
        x_byte(a, 0x89); x_byte(a, 0xc0);   /* mov eax, eax */
        x_mov_ri(a, X_RCX, (uint64_t)(uint32_t)JS_TAG_BOOL << 32);
        x_alu_rr64(a, X_OR, X_RAX, X_RCX);
        break;
    case T2R_F64: {     /* NaNs canonical, then the double's encoding */
        int ok = x_new_label(a);
        x_mov_rr(a, X_RCX, X_RAX);
        x_bt64_i(a, 6, X_RCX, 63);          /* btr: |bits| */
        x_mov_ri(a, X_RDX, 0x7ff0000000000000ull);
        x_cmp_rr(a, X_RCX, X_RDX);
        x_jcc(a, X_CC_BE, ok);
        x_mov_ri(a, X_RAX, 0x7ff8000000000000ull);
        x_bind(a, ok);
        x_mov_ri(a, X_RCX, T2_F64_ADDEND);
        x_alu_rr64(a, X_SUB, X_RAX, X_RCX);
        break;
    }
    default:
        break;
    }
}

/* F->sp (stack depth d) and F->pc for the instruction at pc: just past its
 * opcode, as a helper runs it (at > 0), or at its start, where the
 * interpreter continues after an exit (at = 0). */
static void t2_set_frame_at(T2Gen *G, uint32_t pc, int d, int at)
{
    x_lea(&G->a, X_RAX, T2_STKR, d * 8);
    x_store(&G->a, T2_F, offsetof(JSInterpFrame, sp), X_RAX);
    x_mov_ri(&G->a, X_RAX, (uint64_t)(uintptr_t)(G->B->src->bc + pc + at));
    x_store(&G->a, T2_F, offsetof(JSInterpFrame, pc), X_RAX);
}

static inline void t2_set_frame_pc(T2Gen *G, uint32_t pc, int d)
{
    t2_set_frame_at(G, pc, d, 1);
}

static int t2_has_result(int op)
{
    return op != T2N_CONST && op != T2N_STORE && op != T2N_GENERIC && op != T2N_BRANCH &&
           op != T2N_JUMP && op != T2N_POLL && op != T2N_RETURN && op != T2N_PUT_FIELD &&
           op != T2N_PUT_ELEM;
}

/* Native stack slots: each value gets one at its definition and gives it
 * back after its last use (in a block, or at an exit that stores it). */
static int t2_assign_spills(T2Builder *B, uint32_t *n_spills)
{
    int *free_list, n_free = 0, top = 0;
    for (int i = 0; i < B->n_nodes; i++) B->nodes[i].last_use = -1;
    for (int i = 0; i < B->n_nodes; i++) {
        T2Node *N = &B->nodes[i];
        if (N->a >= 0) B->nodes[N->a].last_use = i;
        if (N->b >= 0) B->nodes[N->b].last_use = i;
        if (N->c >= 0) B->nodes[N->c].last_use = i;
        if (N->exit >= 0) {
            T2Exit *x = &B->exits[N->exit];
            for (int k = 0; k < x->n; k++)
                B->nodes[B->entries[x->first + k].node].last_use = i;
        }
    }
    free_list = malloc(sizeof(int) * (B->n_nodes + 1));
    if (!free_list) return 0;
    for (int i = 0; i < B->n_nodes; i++) {
        T2Node *N = &B->nodes[i];
        int ins[3] = { N->a, N->b, N->c };
        for (int k = 0; k < 3; k++) {
            int m = ins[k];
            if (m >= 0 && B->nodes[m].last_use == i && B->nodes[m].spill >= 0) {
                free_list[n_free++] = B->nodes[m].spill;
                B->nodes[m].last_use = -2;  /* given back */
            }
        }
        if (t2_has_result(N->op) && N->last_use >= 0)
            N->spill = n_free ? free_list[--n_free] : top++;
        /* values only an exit read die here too */
        if (N->exit >= 0) {
            T2Exit *x = &B->exits[N->exit];
            for (int k = 0; k < x->n; k++) {
                T2Node *M = &B->nodes[B->entries[x->first + k].node];
                if (M->last_use == i && M->spill >= 0) {
                    free_list[n_free++] = M->spill;
                    M->last_use = -2;
                }
            }
        }
    }
    free(free_list);
    *n_spills = (uint32_t)top;
    return 1;
}

static int t2_exit_label(T2Gen *G, int x)
{
    T2Exit *E = &G->B->exits[x];
    if (E->label < 0) E->label = x_new_label(&G->a);
    return E->label;
}

/* Set al from the flags of a comparison (int: signed cc; f64: after
 * ucomisd with the operands ordered for the cc). */
static void t2_emit_cmp(T2Gen *G, T2Node *N, int is_f64)
{
    XAsm *a = &G->a;
    if (!is_f64) {
        static const uint8_t ccs[] = { X_CC_L, X_CC_LE, X_CC_G, X_CC_GE, X_CC_E, X_CC_NE };
        t2_get(G, X_RAX, N->a);
        t2_get(G, X_RCX, N->b);
        x_alu_rr32(a, X_CMP, X_RAX, X_RCX);
        x_setcc_eax(a, ccs[N->cc]);
        return;
    }
    /* xmm0 = a, xmm1 = b */
    t2_get(G, X_RAX, N->a);
    x_movq_xr(a, 0, X_RAX);
    t2_get(G, X_RAX, N->b);
    x_movq_xr(a, 1, X_RAX);
    switch (N->cc) {
    case T2C_LT: x_ucomisd(a, 1, 0); x_setcc_eax(a, X_CC_A); break;    /* b > a */
    case T2C_LE: x_ucomisd(a, 1, 0); x_setcc_eax(a, X_CC_AE); break;
    case T2C_GT: x_ucomisd(a, 0, 1); x_setcc_eax(a, X_CC_A); break;
    case T2C_GE: x_ucomisd(a, 0, 1); x_setcc_eax(a, X_CC_AE); break;
    case T2C_EQ:    /* ZF and not PF */
        x_ucomisd(a, 0, 1);
        x_setcc_eax(a, X_CC_E);
        x_setcc_r8(a, 0xb, X_RCX);          /* setnp cl */
        x_alu_rr32(a, X_AND, X_RAX, X_RCX);
        break;
    case T2C_NE:    /* not ZF, or PF */
        x_ucomisd(a, 0, 1);
        x_setcc_eax(a, X_CC_NE);
        x_setcc_r8(a, 0xa, X_RCX);          /* setp cl */
        x_alu_rr32(a, X_OR, X_RAX, X_RCX);
        x_alu_ri32(a, 4, X_RAX, 1);         /* and eax, 1 */
        break;
    }
}

/* --- Objects -------------------------------------------------------------- */

/* Where JSObject's flag bits are (bitfields: found once, by setting each). */
static struct {
    int ready;
    int32_t exotic_off, fast_array_off, watched_off;    /* their bytes */
    uint8_t exotic, fast_array, watched;                /* their bits */
} t2_obj;

static int t2_find_bit(const uint8_t *b, size_t n, uint8_t *mask)
{
    for (size_t k = 0; k < n; k++)
        if (b[k]) { *mask = b[k]; return (int)k; }
    return -1;
}

static void t2_obj_layout(void)
{
    JSObject o;
    if (t2_obj.ready) return;
    memset(&o, 0, sizeof(o)); o.is_exotic = 1;
    t2_obj.exotic_off = t2_find_bit((const uint8_t *)&o, sizeof(o), &t2_obj.exotic);
    memset(&o, 0, sizeof(o)); o.fast_array = 1;
    t2_obj.fast_array_off = t2_find_bit((const uint8_t *)&o, sizeof(o), &t2_obj.fast_array);
    memset(&o, 0, sizeof(o)); o.is_watched = 1;
    t2_obj.watched_off = t2_find_bit((const uint8_t *)&o, sizeof(o), &t2_obj.watched);
    t2_obj.ready = 1;
}

#define T2_OBJ(field) ((int32_t)(2 * GC_HEADER_SIZE + offsetof(JSObject, field)))
#define T2_OBJ_BYTE(off) ((int32_t)(2 * GC_HEADER_SIZE) + (off))
#define T2_OBJ_INLINE_PROPS ((int32_t)(2 * GC_HEADER_SIZE + ((sizeof(JSObject) + 7) & ~(size_t)7)))

/* rax: a tagged value; exit unless it is an object.  Then rdx: its handle's
 * base (heap + h*16; the object's fields at T2_OBJ offsets). */
static void t2_emit_object(T2Gen *G, int exit_label)
{
    XAsm *a = &G->a;
    x_mov_rr(a, X_RCX, X_RAX);
    x_shr64_i(a, X_RCX, 32);
    x_cmp32_i(a, X_RCX, (uint32_t)JS_TAG_OBJECT);
    x_jcc(a, X_CC_NE, exit_label);
    x_byte(a, 0x89); x_byte(a, 0xc2);       /* mov edx, eax */
    x_shl64_i(a, X_RDX, 4);
    x_alu_rr64(a, X_ADD, X_RDX, T2_HEAP);
}

/* r8: the property array of the object at base reg (its own array, or its
 * inline slots). */
static void t2_emit_props(T2Gen *G, int reg)
{
    XAsm *a = &G->a;
    int in_obj = x_new_label(a), have = x_new_label(a);
    x_load32(a, X_R8, reg, T2_OBJ(prop_handle));
    x_test32(a, X_R8, X_R8);
    x_jcc(a, X_CC_E, in_obj);
    x_shl64_i(a, X_R8, 4);
    x_alu_rr64(a, X_ADD, X_R8, T2_HEAP);
    x_alu_ri(a, 0, X_R8, 2 * GC_HEADER_SIZE);
    x_jmp(a, have);
    x_bind(a, in_obj);
    x_lea(a, X_R8, reg, T2_OBJ_INLINE_PROPS);
    x_bind(a, have);
}

/* The prototype epoch of atom still the one the ways were filled with
 * (jit_proto_epoch); exit if not.  Clobbers r9, r10. */
static void t2_emit_epoch_check(T2Gen *G, JSAtom atom, uint32_t epoch, int exit_label)
{
    XAsm *a = &G->a;
    x_mov_ri(a, X_R9, (uint64_t)(uintptr_t)t2_proto_epoch_addr());
    x_load32(a, X_R10, X_R9, 0);
    x_mov_ri(a, X_R9, (uint64_t)(uintptr_t)t2_atom_epoch_addr(atom));
    x_rex(a, 0, X_R10, 0, X_R9); x_byte(a, 0x03); x_mem(a, X_R10, X_R9, 0);   /* add r10d, [r9] */
    x_cmp32_i(a, X_R10, epoch);
    x_jcc(a, X_CC_NE, exit_label);
}

/* Mark the card of the heap slot at address reg (gc_dirty_slot); clobbers
 * rax, rcx. */
static void t2_emit_card(T2Gen *G, int reg)
{
    XAsm *a = &G->a;
#if GC_SOFTWARE_DIRTY
    x_mov_rr(a, X_RAX, reg);
    x_alu_rr64(a, X_SUB, X_RAX, T2_HEAP);
    x_shr64_i(a, X_RAX, GC_CARD_SHIFT);
    x_mov_ri(a, X_RCX, (uint64_t)(uintptr_t)&g_gc.cards);
    x_load(a, X_RCX, X_RCX, 0);
    x_store8_idx_i(a, X_RCX, X_RAX, 1);
#else
    (void)a; (void)reg;
#endif
}

static void t2_emit_get_field(T2Gen *G, int i)
{
    T2Builder *B = G->B;
    T2Node *N = &B->nodes[i];
    const JitIC *ic = &B->ics[N->ic];
    XAsm *a = &G->a;
    int ex = t2_exit_label(G, N->exit), done = x_new_label(a), way[JIT_IC_WAYS];
    t2_get(G, X_RAX, N->a);
    t2_emit_object(G, ex);
    x_load32(a, X_RCX, X_RDX, T2_OBJ(shape_handle));
    for (int k = 0; k < ic->n; k++) {
        way[k] = x_new_label(a);
        x_cmp32_i(a, X_RCX, ic->shape[k]);
        x_jcc(a, X_CC_E, way[k]);
    }
    x_jmp(a, ex);
    for (int k = 0; k < ic->n; k++) {
        x_bind(a, way[k]);
        if (ic->holder[k] == GC_HANDLE_NULL) {
            t2_emit_props(G, X_RDX);
            x_load(a, X_RAX, X_R8, (int32_t)ic->slot[k] * 8);
        } else {
            /* on a prototype (or on none): an exotic receiver looks first */
            x_test_mem8_i(a, X_RDX, T2_OBJ_BYTE(t2_obj.exotic_off), t2_obj.exotic);
            x_jcc(a, X_CC_NE, ex);
            t2_emit_epoch_check(G, N->atom, ic->epoch, ex);
            if (ic->slot[k] == JIT_IC_ABSENT) {
                x_mov_ri(a, X_RAX, t2_bits(JS_UNDEFINED));
            } else {
                x_mov_ri(a, X_R9, ic->holder[k]);
                x_shl64_i(a, X_R9, 4);
                x_alu_rr64(a, X_ADD, X_R9, T2_HEAP);
                t2_emit_props(G, X_R9);
                x_load(a, X_RAX, X_R8, (int32_t)ic->slot[k] * 8);
            }
        }
        x_jmp(a, done);
    }
    x_bind(a, done);
    t2_put(G, i, X_RAX);
}

static void t2_emit_put_field(T2Gen *G, int i)
{
    T2Builder *B = G->B;
    T2Node *N = &B->nodes[i];
    const JitIC *ic = &B->ics[N->ic];
    XAsm *a = &G->a;
    int ex = t2_exit_label(G, N->exit), done = x_new_label(a), way[JIT_IC_WAYS];
    t2_get_boxed(G, N->b);
    x_mov_rr(a, X_R11, X_RAX);              /* the value */
    t2_get(G, X_RAX, N->a);
    t2_emit_object(G, ex);
    x_test_mem8_i(a, X_RDX, T2_OBJ_BYTE(t2_obj.watched_off), t2_obj.watched);
    x_jcc(a, X_CC_NE, ex);
    x_load32(a, X_RCX, X_RDX, T2_OBJ(shape_handle));
    for (int k = 0; k < ic->n; k++) {
        way[k] = x_new_label(a);
        x_cmp32_i(a, X_RCX, ic->shape[k]);
        x_jcc(a, X_CC_E, way[k]);
    }
    x_jmp(a, ex);
    for (int k = 0; k < ic->n; k++) {
        x_bind(a, way[k]);
        t2_emit_props(G, X_RDX);
        x_lea(a, X_R8, X_R8, (int32_t)ic->slot[k] * 8);
        x_store(a, X_R8, 0, X_R11);
        t2_emit_card(G, X_R8);
        x_jmp(a, done);
    }
    x_bind(a, done);
}

/* a[b] of a fast array (a[b] = c): r8 the element's address, after the
 * guards. */
static void t2_emit_elem_addr(T2Gen *G, T2Node *N, int ex)
{
    XAsm *a = &G->a;
    t2_get(G, X_RAX, N->a);
    t2_emit_object(G, ex);
    x_load16(a, X_RCX, X_RDX, T2_OBJ(class_id));
    x_cmp32_i(a, X_RCX, t2_class_array);
    x_jcc(a, X_CC_NE, ex);
    x_test_mem8_i(a, X_RDX, T2_OBJ_BYTE(t2_obj.fast_array_off), t2_obj.fast_array);
    x_jcc(a, X_CC_E, ex);
    t2_get(G, X_RCX, N->b);
    x_cmp32_mem(a, X_RCX, X_RDX, T2_OBJ(u.array.count));
    x_jcc(a, X_CC_AE, ex);                  /* unsigned: a negative index too */
    x_byte(a, 0x89); x_byte(a, 0xc9);       /* mov ecx, ecx */
    x_load32(a, X_R8, X_RDX, T2_OBJ(u.array.u.values_handle));
    x_shl64_i(a, X_R8, 4);
    x_alu_rr64(a, X_ADD, X_R8, T2_HEAP);
    x_lea(a, X_R8, X_R8, 2 * GC_HEADER_SIZE);
    x_rex(a, 1, X_R8, X_RCX, X_R8); x_byte(a, 0x8d); x_mem_sib(a, X_R8, X_R8, X_RCX, 8, 0); /* lea r8, [r8+rcx*8] */
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
        t2_get_boxed(G, N->a);
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
        t2_get_boxed(G, N->a);
        x_mov_rr(a, X_RDX, X_RAX);
        x_mov_rr(a, X_RCX, T2_F);
        x_call_abs(a, (const void *)t2_to_bool);
        t2_put(G, i, X_RAX);
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
        t2_get_boxed(G, N->a);
        x_store(a, T2_F, offsetof(JSInterpFrame, ret_val), X_RAX);
        t2_set_frame_pc(G, N->pc, N->depth);
        x_mov_ri(a, X_RAX, JSOP_DONE);
        x_jmp(a, G->epilogue);
        break;
    case T2N_UNBOX_I32:     /* an int32 is its zero-extended bits */
        t2_get(G, X_RAX, N->a);
        x_mov_ri(a, X_RCX, 0xffffffffu);
        x_cmp_rr(a, X_RAX, X_RCX);
        x_jcc(a, X_CC_A, t2_exit_label(G, N->exit));
        t2_put(G, i, X_RAX);
        break;
    case T2N_UNBOX_F64: {
        int is_int = x_new_label(a), done = x_new_label(a);
        t2_get(G, X_RAX, N->a);
        x_mov_rr(a, X_RCX, X_RAX);
        x_shr64_i(a, X_RCX, 32);
        x_test32(a, X_RCX, X_RCX);
        x_jcc(a, X_CC_E, is_int);
        x_alu_ri32(a, 0, X_RCX, (int8_t)-JS_TAG_FIRST);          /* tag - JS_TAG_FIRST */
        x_alu_ri32(a, 7, X_RCX, (int8_t)(JS_TAG_FLOAT64 - JS_TAG_FIRST));
        x_jcc(a, X_CC_B, t2_exit_label(G, N->exit));            /* a tag: not a double */
        x_mov_ri(a, X_RCX, T2_F64_ADDEND);
        x_alu_rr64(a, X_ADD, X_RAX, X_RCX);
        x_jmp(a, done);
        x_bind(a, is_int);
        x_cvtsi2sd32(a, 0, X_RAX);
        x_movq_rx(a, X_RAX, 0);
        x_bind(a, done);
        t2_put(G, i, X_RAX);
        break;
    }
    case T2N_I32_TO_F64:
        t2_get(G, X_RAX, N->a);
        x_cvtsi2sd32(a, 0, X_RAX);
        x_movq_rx(a, X_RAX, 0);
        t2_put(G, i, X_RAX);
        break;
    case T2N_I32_ADD:
    case T2N_I32_SUB:
        t2_get(G, X_RAX, N->a);
        t2_get(G, X_RCX, N->b);
        x_alu_rr32(a, N->op == T2N_I32_ADD ? X_ADD : X_SUB, X_RAX, X_RCX);
        x_jcc(a, X_CC_O, t2_exit_label(G, N->exit));
        t2_put(G, i, X_RAX);
        break;
    case T2N_I32_MUL: {
        int ok = x_new_label(a);
        t2_get(G, X_RAX, N->a);
        t2_get(G, X_RCX, N->b);
        x_mov_rr(a, X_RDX, X_RAX);
        x_imul32(a, X_RAX, X_RCX);
        x_jcc(a, X_CC_O, t2_exit_label(G, N->exit));
        x_test32(a, X_RAX, X_RAX);
        x_jcc(a, X_CC_NE, ok);
        x_alu_rr32(a, X_OR, X_RDX, X_RCX);      /* 0 from a negative operand: -0 */
        x_jcc(a, 8 /* S */, t2_exit_label(G, N->exit));
        x_bind(a, ok);
        t2_put(G, i, X_RAX);
        break;
    }
    case T2N_I32_AND:
    case T2N_I32_OR:
    case T2N_I32_XOR:
        t2_get(G, X_RAX, N->a);
        t2_get(G, X_RCX, N->b);
        x_alu_rr32(a, N->op == T2N_I32_AND ? X_AND : N->op == T2N_I32_OR ? X_OR : X_XOR, X_RAX, X_RCX);
        x_byte(a, 0x89); x_byte(a, 0xc0);       /* mov eax, eax */
        t2_put(G, i, X_RAX);
        break;
    case T2N_I32_SHL:
    case T2N_I32_SAR:
    case T2N_I32_SHR:
        t2_get(G, X_RAX, N->a);
        t2_get(G, X_RCX, N->b);
        x_shift_cl32(a, N->op == T2N_I32_SHL ? 4 : N->op == T2N_I32_SAR ? 7 : 5, X_RAX);
        if (N->op == T2N_I32_SHR) {
            x_test32(a, X_RAX, X_RAX);
            x_jcc(a, 8 /* S */, t2_exit_label(G, N->exit));   /* above INT32_MAX */
        }
        t2_put(G, i, X_RAX);
        break;
    case T2N_I32_NEG:
        t2_get(G, X_RAX, N->a);
        x_test32_i(a, X_RAX, 0x7fffffffu);      /* 0 (-0) or INT32_MIN */
        x_jcc(a, X_CC_E, t2_exit_label(G, N->exit));
        x_neg32(a, X_RAX);
        x_byte(a, 0x89); x_byte(a, 0xc0);       /* mov eax, eax */
        t2_put(G, i, X_RAX);
        break;
    case T2N_I32_CMP:
        t2_emit_cmp(G, N, 0);
        t2_put(G, i, X_RAX);
        break;
    case T2N_F64_ADD:
    case T2N_F64_SUB:
    case T2N_F64_MUL:
    case T2N_F64_DIV: {
        static const uint8_t ops[] = { X_ADDSD, X_SUBSD, X_MULSD, X_DIVSD };
        t2_get(G, X_RAX, N->a);
        x_movq_xr(a, 0, X_RAX);
        t2_get(G, X_RAX, N->b);
        x_movq_xr(a, 1, X_RAX);
        x_sse_sd(a, ops[N->op - T2N_F64_ADD], 0, 1);
        x_movq_rx(a, X_RAX, 0);
        t2_put(G, i, X_RAX);
        break;
    }
    case T2N_F64_NEG:
        t2_get(G, X_RAX, N->a);
        x_bt64_i(a, 7, X_RAX, 63);              /* btc: flip the sign */
        t2_put(G, i, X_RAX);
        break;
    case T2N_F64_CMP:
        t2_emit_cmp(G, N, 1);
        t2_put(G, i, X_RAX);
        break;
    case T2N_GET_FIELD:
        t2_emit_get_field(G, i);
        break;
    case T2N_PUT_FIELD:
        t2_emit_put_field(G, i);
        break;
    case T2N_GET_ELEM:
        t2_emit_elem_addr(G, N, t2_exit_label(G, N->exit));
        x_load(a, X_RAX, X_R8, 0);
        t2_put(G, i, X_RAX);
        break;
    case T2N_PUT_ELEM:
        t2_get_boxed(G, N->c);
        x_mov_rr(a, X_R11, X_RAX);
        t2_emit_elem_addr(G, N, t2_exit_label(G, N->exit));
        x_store(a, X_R8, 0, X_R11);
        t2_emit_card(G, X_R8);
        break;
    case T2N_BOOL_NOT:
        t2_get(G, X_RAX, N->a);
        x_alu_ri32(a, 6, X_RAX, 1);             /* xor eax, 1 */
        x_byte(a, 0x89); x_byte(a, 0xc0);       /* mov eax, eax */
        t2_put(G, i, X_RAX);
        break;
    }
}

/* The exits, out of line: store the slots the graph held, boxed, set
 * F->sp and F->pc to the instruction's start, tell t2_deopt what failed,
 * and continue in the interpreter. */
static void t2_emit_exits(T2Gen *G)
{
    T2Builder *B = G->B;
    XAsm *a = &G->a;
    for (int x = 0; x < B->n_exits; x++) {
        T2Exit *E = &B->exits[x];
        int base;
        int32_t disp;
        if (E->label < 0) continue;     /* no guard uses it */
        x_bind(a, E->label);
        for (int k = 0; k < E->n; k++) {
            T2Entry *en = &B->entries[E->first + k];
            t2_get_boxed(G, en->node);
            t2_slot_addr(B, en->slot, &base, &disp);
            x_store(a, base, disp, X_RAX);
        }
        t2_set_frame_at(G, E->pc, E->depth, 0);
        x_mov_rr(a, X_RCX, T2_F);
        x_mov_ri(a, X_RDX, (uint64_t)(uintptr_t)E->state);
        if (E->opnd[0] >= 0) { t2_slot_addr(B, E->opnd[0], &base, &disp); x_lea(a, X_R8, base, disp); }
        else x_mov_ri(a, X_R8, 0);
        if (E->opnd[1] >= 0) { t2_slot_addr(B, E->opnd[1], &base, &disp); x_lea(a, X_R9, base, disp); }
        else x_mov_ri(a, X_R9, 0);
        x_call_abs(a, (const void *)t2_deopt);
        x_mov_ri(a, X_RAX, JSOP_NEXT);
        x_jmp(a, G->epilogue);
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
    uint64_t inline_get, inline_put, inline_elem, generic;     /* sites, in compiled code */
} g_t2;

static void t2_free_builder(T2Builder *B, T2Gen *G)
{
    x_free(&G->a);
    free(B->nodes);
    free(B->exits);
    free(B->entries);
    free(B->ics);
    free(B->slots);
    free(B->shape.depth);
    free(B->shape.block_start);
    free(G->block_label);
}

T2Code *t2_compile(const T2Source *src)
{
    T2Builder B = { 0 };
    T2Gen G = { 0 };
    T2Code *tc = NULL;
    uint32_t n_spills = 0;
    int *block_first = NULL;
    t2_obj_layout();
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
    block_first = malloc(sizeof(int) * (src->len + 1));
    if (!block_first) goto fail;
    for (uint32_t p = 0; p <= src->len; p++) {
        block_first[p] = -1;
        G.block_label[p] = -1;
    }
    for (uint32_t p = 0; p < src->len; p++) {
        if (!B.shape.block_start[p] || B.shape.depth[p] < 0) continue;
        block_first[p] = B.n_nodes;
        if (!t2_build_block(&B, p)) goto fail;
    }
    /* An exit stores values computed before its guard, never after. */
    for (int i = 0; i < B.n_nodes; i++) {
        if (B.nodes[i].exit < 0) continue;
        T2Exit *x = &B.exits[B.nodes[i].exit];
        for (int k = 0; k < x->n; k++)
            if (B.entries[x->first + k].node >= i) { B.why = "an exit reads a later value (a T2 bug)"; goto fail; }
    }
    if (!t2_assign_spills(&B, &n_spills)) goto fail;

    /* Frame: 32 bytes of shadow space and the spills, sized so calls see a
     * 16-byte aligned rsp: the return address and 8 pushes leave rsp at 8
     * mod 16, so the frame is 8 mod 16. */
    G.frame = ((32 + n_spills * 8 + 15) & ~15u) + 8;
    if (G.frame > T2_MAX_FRAME) { B.why = "too many values"; goto fail; }

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
    x_mov_ri(a, T2_HEAP, (uint64_t)(uintptr_t)&g_gc.heap_base);
    x_load(a, T2_HEAP, T2_HEAP, 0);

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
    t2_emit_exits(&G);

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
    for (int i = 0; i < B.n_nodes; i++) {
        int op = B.nodes[i].op;
        g_t2.inline_get += op == T2N_GET_FIELD;
        g_t2.inline_put += op == T2N_PUT_FIELD;
        g_t2.inline_elem += op == T2N_GET_ELEM || op == T2N_PUT_ELEM;
        g_t2.generic += op == T2N_GENERIC;
    }
    if (getenv("QJS_T2_LOG"))
        fprintf(stderr, "[t2] compiled %s: %u bytecode bytes, %d nodes, %d exits, %u bytes\n",
                src->name ? src->name : "?", src->len, B.n_nodes, B.n_exits, tc->size);
    free(block_first);
    t2_free_builder(&B, &G);
    return tc;

 fail:
    g_t2.failed++;
    if (getenv("QJS_T2_LOG"))
        fprintf(stderr, "[t2] not compiled %s: %s%s%s\n", src->name ? src->name : "?",
                B.oom ? "out of memory" : B.why ? B.why : "?",
                B.why_op >= 0 ? ": " : "", B.why_op >= 0 && t2_op_name[B.why_op] ? t2_op_name[B.why_op] : "");
    free(tc);
    free(block_first);
    t2_free_builder(&B, &G);
    return NULL;
}

int t2_available(void)
{
    return jit_code_alloc(0) != NULL || jit_available();
}

void t2_print_stats(void)
{
    fprintf(stderr, "[t2] %llu functions compiled (%llu bytes), %llu not compiled; "
            "inline sites: %llu gets, %llu puts, %llu array; %llu helper calls\n",
            (unsigned long long)g_t2.compiled, (unsigned long long)g_t2.bytes,
            (unsigned long long)g_t2.failed, (unsigned long long)g_t2.inline_get,
            (unsigned long long)g_t2.inline_put, (unsigned long long)g_t2.inline_elem,
            (unsigned long long)g_t2.generic);
}

#else /* !T2_SUPPORTED */

T2Code *t2_compile(const T2Source *src) { (void)src; return NULL; }
int t2_available(void) { return 0; }
void t2_print_stats(void) {}

#endif

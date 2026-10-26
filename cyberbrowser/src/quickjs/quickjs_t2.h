/*
 * quickjs_t2.h - the optimizer, T2 (JS_PERF_DESIGN.md section 6).
 */
#ifndef QUICKJS_T2_H
#define QUICKJS_T2_H

#include "quickjs_jit.h"

/* A function's T2 code: an ordinary Win64 function of the interpreter's
 * frame state, returning as compiled code does (JSOP_*; JSOP_NEXT:
 * continue in the interpreter at F->pc). */
typedef struct T2Code {
    int (*entry)(JSInterpFrame *F);
    uint32_t size;
} T2Code;

/* JSFunctionBytecode.t2_code of a function T2 cannot compile. */
#define T2_CODE_FAILED ((T2Code *)(uintptr_t)1)

/* What it compiles from (quickjs.c knows the bytecode's layout). */
typedef struct T2Source {
    const uint8_t *bc;
    uint32_t len;
    int arg_count, var_count, stack_size;
    int strict;                                /* a strict-mode function */
    const GCValue *cpool;
    int (*const *helpers)(JSInterpFrame *F);   /* jsop_table */
    const uint8_t *jumps;                      /* jsop_jumps */
    const char *name;                          /* for profiles; may be NULL */
    JSContextHandle ctx;                       /* its realm (atoms' values) */
    int deopts;                                /* how often its T2 code was dropped */
    /* Its feedback vector's map (offset -> site) and arithmetic states;
       NULL when it has none (then nothing is speculated). */
    const uint16_t *site_of;
    uint8_t *arith;
    const JitIC *ics;           /* its property sites' inline caches */
    /* By argument, then local: whether a closure captures it (then it lives
       in the frame, where the closure's reference points). */
    const uint8_t *captured;
} T2Source;

/* An arithmetic site's state: what made T2's speculation on it fail. */
#define T2_ARITH_DOUBLE 1   /* a number outside int32, or an int32 result that was not */
#define T2_ARITH_OTHER  2   /* a value that is not a number */
#define T2_ARITH_GENERIC 4  /* an equality's identity test failed too */

T2Code *t2_compile(const T2Source *src);
/* QJS_T2_LOG=1: what T2 compiles, deopts and why (read once). */
int t2_logging(void);
int t2_available(void);
void t2_print_stats(void);

/* Called by T2 code (quickjs.c). */
int t2_poll(JSInterpFrame *F);
int t2_to_bool(JSInterpFrame *F, GCValue v);
void t2_deopt(JSInterpFrame *F, uint8_t *state, const GCValue *a, const GCValue *b);
int t2_retain(GCHandle h);
int t2_call(JSInterpFrame *F, int argc, int kind);
GCValue t2_this(JSInterpFrame *F);
int t2_eq(GCValue a, GCValue b, int loose);
/* Variable accesses (t2_var_get, t2_var_put): how much they may do. */
enum { T2_VAR_PLAIN, T2_VAR_CHECK, T2_VAR_GLOBAL };
GCValue t2_var_get(JSInterpFrame *F, int idx, int mode);
int t2_var_put(JSInterpFrame *F, int idx, int mode, GCValue v);
int t2_add_field(JSInterpFrame *F, GCValue obj, JSAtom atom, GCValue val);
GCValue t2_get_field_miss(JSInterpFrame *F, GCValue obj, JitIC *ic, JSAtom atom);
int t2_put_field_miss(JSInterpFrame *F, GCValue obj, JitIC *ic, GCValue val);
int t2_strict_eq(JSInterpFrame *F, GCValue a, GCValue b);
GCValue t2_typeof(JSInterpFrame *F, GCValue v);
/* Instructions T2 code calls directly (T2N_CCALL): F->pc past the opcode,
 * F->sp at the instruction's depth, the slots JavaScript could see in the
 * frame; the instruction's operands as values (deepest first).  The result,
 * or JS_EXCEPTION when it threw. */
typedef GCValue T2CCall(JSInterpFrame *F, GCValue a, GCValue b, GCValue c);
T2CCall t2_op_object, t2_op_define_field, t2_op_fclosure, t2_op_in, t2_op_instanceof,
        t2_op_add, t2_op_eq, t2_op_neq, t2_op_get_elem, t2_op_put_elem;
int t2_is_c_function(GCHandle f);
int t2_ic_way_c_getter(const JitIC *ic, int k);
extern uint64_t g_t2_dyn_calls[4];     /* t2_call outcomes (QJS_T2_COUNT) */
extern const uint16_t t2_class_array;   /* JS_CLASS_ARRAY */
const uint32_t *t2_proto_epoch_addr(void);
const uint32_t *t2_atom_epoch_addr(JSAtom atom);
uint32_t t2_proto_epoch(JSAtom atom);

#endif /* QUICKJS_T2_H */

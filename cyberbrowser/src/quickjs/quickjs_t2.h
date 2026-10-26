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
    const GCValue *cpool;
    int (*const *helpers)(JSInterpFrame *F);   /* jsop_table */
    const uint8_t *jumps;                      /* jsop_jumps */
    const char *name;                          /* for profiles; may be NULL */
    /* Its feedback vector's map (offset -> site) and arithmetic states;
       NULL when it has none (then nothing is speculated). */
    const uint16_t *site_of;
    uint8_t *arith;
    const JitIC *ics;           /* its property sites' inline caches */
} T2Source;

/* An arithmetic site's state: what made T2's speculation on it fail. */
#define T2_ARITH_DOUBLE 1   /* a number outside int32, or an int32 result that was not */
#define T2_ARITH_OTHER  2   /* a value that is not a number */

T2Code *t2_compile(const T2Source *src);
int t2_available(void);
void t2_print_stats(void);

/* Called by T2 code (quickjs.c). */
int t2_poll(JSInterpFrame *F);
int t2_to_bool(JSInterpFrame *F, GCValue v);
void t2_deopt(JSInterpFrame *F, uint8_t *state, const GCValue *a, const GCValue *b);
int t2_retain(GCHandle h);
extern const uint16_t t2_class_array;   /* JS_CLASS_ARRAY */
const uint32_t *t2_proto_epoch_addr(void);
const uint32_t *t2_atom_epoch_addr(JSAtom atom);
uint32_t t2_proto_epoch(JSAtom atom);

#endif /* QUICKJS_T2_H */

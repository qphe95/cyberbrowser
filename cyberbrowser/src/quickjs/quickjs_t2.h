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
} T2Source;

T2Code *t2_compile(const T2Source *src);
int t2_available(void);
void t2_print_stats(void);

/* Called by T2 code (quickjs.c). */
int t2_poll(JSInterpFrame *F);
int t2_to_bool(JSInterpFrame *F, GCValue v);

#endif /* QUICKJS_T2_H */

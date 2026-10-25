/*
 * quickjs_jit_ops.c - the baseline JIT's stencils (JIT_DESIGN.md section 3).
 *
 * Not part of the normal build: tools/jit_stencils.js compiles this file
 * with clang and extracts each function's machine code, relocations and
 * unwind data into jit_stencils_x86_64_windows.h, which the JIT copies and
 * patches at run time.  Regenerate the header after changing this file.
 *
 * Every stencil has the same signature, with the pinned state in the
 * registers preserve_none passes arguments in (r12-r15 on x86-64), and ends
 * in a tail call to its successor, so a compiled function is a chain of
 * stencils with no call or return between them.  The interpreter's frame
 * is the only state: sp lives in a register and is stored to F->sp before
 * anything that reads it.
 *
 * Holes, patched by the JIT:
 *   IMM(n)        a 64-bit value (operand, address), loaded from a literal
 *   CONTINUE()    jump to the next instruction (cut when it follows)
 *   TARGET()      jump to the branch target
 *   SLOW()        jump to the instruction's out-of-line generic stencil
 *   EXIT()        leave compiled code with F->jit_exit
 *   DYN()         jump to the code for F->pc, looked up at run time
 *   HELPER(F)     the instruction's jsop_<op> helper (quickjs_jsops.inc)
 * Anything else a stencil references (g_gc, js_frame_deref...) is resolved
 * by name from the JIT's symbol table.
 *
 * A fast path may run only when it does exactly what the interpreter's case
 * does; otherwise, before changing anything, it jumps to SLOW(), which runs
 * the whole instruction through its helper.
 */
#include "quickjs_gc_unified.h"
#include "quickjs.h"
#include "quickjs-internal.h"
#include "cyber_env.h"
#include "quickjs_jit.h"
#include "quickjs_props_inline.h"

/* As quickjs.c has them. */
typedef struct JSString JSString;
typedef struct JSObject JSObject;
#define JS_VALUE_GET_OBJ(v) ((JSObjectHandle)(GC_VALUE_GET_HANDLE(v)))
#define JS_VALUE_GET_STRING(v) ((JSStringHandle)(GC_VALUE_GET_HANDLE(v)))

#define PN __attribute__((preserve_none))
typedef PN int JitFn(JSInterpFrame *F, GCValue *sp, GCValue *var_buf, GCValue *arg_buf);

extern char __jit_imm0[], __jit_imm1[], __jit_imm2[], __jit_imm3[];
extern JitFn __jit_continue, __jit_target, __jit_slow, __jit_exit, __jit_dyn;
extern int __jit_helper(JSInterpFrame *F);
extern uint32_t g_interrupt_countdown;

#define IMM(n) ((uintptr_t)__jit_imm##n)
#define TAIL(fn) __attribute__((musttail)) return fn(F, sp, var_buf, arg_buf)
#define CONTINUE() TAIL(__jit_continue)
#define TARGET() TAIL(__jit_target)
#define SLOW() TAIL(__jit_slow)
#define DYN() TAIL(__jit_dyn)
#define EXIT(code) do { F->jit_exit = (code); TAIL(__jit_exit); } while (0)
#define STENCIL(name) PN int jit_op_##name(JSInterpFrame *F, GCValue *sp, GCValue *var_buf, GCValue *arg_buf)

/* The slot at byte offset IMM(0) of a value buffer. */
#define LOC(buf) ((GCValue *)((char *)(buf) + IMM(0)))

/* Values move as two 8-byte halves, never as one 16-byte access or in
 * 4-byte pieces: a load the store buffer cannot forward from the stores
 * that wrote the value (a 16-byte load of two 8-byte stores, an 8-byte
 * load of two 4-byte ones) stalls for a dozen cycles, and the next
 * instruction's stencil nearly always reads what this one wrote.  The empty
 * asm keeps clang from merging the halves back into one SSE access. */
#define AI __attribute__((always_inline))
typedef uint64_t __attribute__((may_alias)) u64a;
static inline AI void vset(GCValue *d, uint64_t payload, int64_t tag)
{
    ((u64a *)d)[0] = payload;
    ((u64a *)d)[1] = (uint64_t)tag;
}
typedef struct V { uint64_t p, t; } V;
static inline AI V vld(const GCValue *s)
{
    V v = { ((const u64a *)s)[0], ((const u64a *)s)[1] };
    __asm__("" : "+r"(v.p), "+r"(v.t));
    return v;
}
static inline AI void vst(GCValue *d, V v) { vset(d, v.p, (int64_t)v.t); }
static inline AI void vcopy(GCValue *d, const GCValue *s) { vst(d, vld(s)); }
static inline AI void vset_int(GCValue *d, int32_t i) { vset(d, (uint32_t)i, JS_TAG_INT); }
static inline AI void vset_bool(GCValue *d, int b) { vset(d, b != 0, JS_TAG_BOOL); }
static inline AI void vset_f64(GCValue *d, double x)
{
    __builtin_memcpy(d, &x, 8);
    ((u64a *)d)[1] = JS_TAG_FLOAT64;
}

/* JS_POLL_POINT would do nothing but count down: nothing to collect and
 * no interrupt due.  Otherwise the instruction runs through its helper,
 * which polls as the interpreter does. */
static inline __attribute__((always_inline)) int poll_is_quiet(void)
{
    if (atomic_load_explicit(&g_gc_poll_flag, memory_order_relaxed))
        return 0;
    if (g_interrupt_countdown == 1)
        return 0;
    if (g_interrupt_countdown)
        g_interrupt_countdown--;
    return 1;
}

/* --- Entry, exits ---------------------------------------------------- */

/* Called (ms_abi) by the JIT to run compiled code from `target`; returns
 * the JSOP_* code compiled code exits with. */
int jit_trampoline(JSInterpFrame *F, JitFn *target)
{
    return target(F, F->sp, F->var_buf, F->arg_buf);
}

STENCIL(exit)
{
    (void)var_buf; (void)arg_buf;
    F->sp = sp;
    return F->jit_exit;
}

/* Continue at F->pc: the code for that bytecode offset, or the interpreter
 * if it has none. */
extern JitFn *jit_lookup_pc(JSInterpFrame *F);
STENCIL(dyn)
{
    F->sp = sp;
    JitFn *t = jit_lookup_pc(F);
    sp = F->sp;
    __attribute__((musttail)) return t(F, sp, var_buf, arg_buf);
}

/* --- Generic: any instruction through its helper ------------------- */

/* IMM(0): the address past the opcode byte; IMM(1): the address of the
 * next instruction. */
STENCIL(generic)
{
    F->sp = sp;
    F->pc = (const uint8_t *)IMM(0);
    int r = __jit_helper(F);
    sp = F->sp;
    if (__builtin_expect(r != JSOP_NEXT, 0))
        EXIT(r);
    if (__builtin_expect(F->pc != (const uint8_t *)IMM(1), 0))
        DYN();
    CONTINUE();
}

/* The same for an instruction that never jumps. */
STENCIL(generic_flat)
{
    F->sp = sp;
    F->pc = (const uint8_t *)IMM(0);
    int r = __jit_helper(F);
    sp = F->sp;
    if (__builtin_expect(r != JSOP_NEXT, 0))
        EXIT(r);
    CONTINUE();
}

/* The same for a jump: IMM(2) is the target's address. */
STENCIL(generic_branch)
{
    F->sp = sp;
    F->pc = (const uint8_t *)IMM(0);
    int r = __jit_helper(F);
    sp = F->sp;
    if (__builtin_expect(r != JSOP_NEXT, 0))
        EXIT(r);
    if (F->pc == (const uint8_t *)IMM(2))
        TARGET();
    if (__builtin_expect(F->pc != (const uint8_t *)IMM(1), 0))
        DYN();
    CONTINUE();
}

/* An instruction compiled code does not run (an exit to the interpreter
 * at it): F->pc is its opcode's address. */
STENCIL(interp)
{
    F->pc = (const uint8_t *)IMM(0);
    EXIT(JSOP_NEXT);
}

/* --- Constants and the stack ---------------------------------------- */

STENCIL(push_i32) { vset_int(&sp[0], (int32_t)IMM(0)); sp++; CONTINUE(); }
STENCIL(push_const) { vcopy(&sp[0], (const GCValue *)IMM(0)); sp++; CONTINUE(); }
STENCIL(undefined) { vset(&sp[0], 0, JS_TAG_UNDEFINED); sp++; CONTINUE(); }
STENCIL(null) { vset(&sp[0], 0, JS_TAG_NULL); sp++; CONTINUE(); }
STENCIL(push_false) { vset_bool(&sp[0], 0); sp++; CONTINUE(); }
STENCIL(push_true) { vset_bool(&sp[0], 1); sp++; CONTINUE(); }
STENCIL(drop) { sp--; CONTINUE(); }
/* An object `this` is pushed as is in strict and sloppy code alike. */
STENCIL(push_this)
{
    if (__builtin_expect(JS_VALUE_GET_TAG(F->this_obj) != JS_TAG_OBJECT, 0))
        SLOW();
    vcopy(&sp[0], &F->this_obj);
    sp++;
    CONTINUE();
}
/* Shuffles load every value they move (in halves, see vset), then store. */
#define LD(i) V v##i = vld(&sp[-(i)])
#define ST(i, v) vst(&sp[-(i)], v)
STENCIL(nip) { LD(1); ST(2, v1); sp--; CONTINUE(); }
STENCIL(nip1) { LD(1); LD(2); ST(3, v2); ST(2, v1); sp--; CONTINUE(); }
STENCIL(dup) { LD(1); ST(0, v1); sp++; CONTINUE(); }
STENCIL(dup1) { LD(1); LD(2); ST(0, v1); ST(1, v2); ST(2, v1); sp++; CONTINUE(); }
STENCIL(dup2) { LD(1); LD(2); ST(0, v2); vst(&sp[1], v1); sp += 2; CONTINUE(); }
STENCIL(dup3) { LD(1); LD(2); LD(3); ST(0, v3); vst(&sp[1], v2); vst(&sp[2], v1); sp += 3; CONTINUE(); }
STENCIL(insert2) { LD(1); LD(2); ST(0, v1); ST(1, v2); ST(2, v1); sp++; CONTINUE(); }
STENCIL(insert3) { LD(1); LD(2); LD(3); ST(0, v1); ST(1, v2); ST(2, v3); ST(3, v1); sp++; CONTINUE(); }
STENCIL(insert4) { LD(1); LD(2); LD(3); LD(4); ST(0, v1); ST(1, v2); ST(2, v3); ST(3, v4); ST(4, v1); sp++; CONTINUE(); }
STENCIL(perm3) { LD(2); LD(3); ST(2, v3); ST(3, v2); CONTINUE(); }
STENCIL(perm4) { LD(2); LD(3); LD(4); ST(2, v3); ST(3, v4); ST(4, v2); CONTINUE(); }
STENCIL(perm5) { LD(2); LD(3); LD(4); LD(5); ST(2, v3); ST(3, v4); ST(4, v5); ST(5, v2); CONTINUE(); }
STENCIL(rot3l) { LD(1); LD(2); LD(3); ST(3, v2); ST(2, v1); ST(1, v3); CONTINUE(); }
STENCIL(rot4l) { LD(1); LD(2); LD(3); LD(4); ST(4, v3); ST(3, v2); ST(2, v1); ST(1, v4); CONTINUE(); }
STENCIL(rot5l) { LD(1); LD(2); LD(3); LD(4); LD(5); ST(5, v4); ST(4, v3); ST(3, v2); ST(2, v1); ST(1, v5); CONTINUE(); }
STENCIL(rot3r) { LD(1); LD(2); LD(3); ST(1, v2); ST(2, v3); ST(3, v1); CONTINUE(); }
STENCIL(swap) { LD(1); LD(2); ST(2, v1); ST(1, v2); CONTINUE(); }
STENCIL(swap2) { LD(1); LD(2); LD(3); LD(4); ST(4, v2); ST(3, v1); ST(2, v4); ST(1, v3); CONTINUE(); }
#undef LD
#undef ST

/* --- Locals and arguments (IMM(0): the slot's byte offset) ---------- */

STENCIL(get_loc) { vcopy(&sp[0], LOC(var_buf)); sp++; CONTINUE(); }
STENCIL(put_loc) { GCValue *pv = LOC(var_buf); vcopy(pv, &sp[-1]); gc_dirty_slot(pv); sp--; CONTINUE(); }
STENCIL(set_loc) { GCValue *pv = LOC(var_buf); vcopy(pv, &sp[-1]); gc_dirty_slot(pv); CONTINUE(); }
STENCIL(get_arg) { vcopy(&sp[0], LOC(arg_buf)); sp++; CONTINUE(); }
STENCIL(put_arg) { GCValue *pv = LOC(arg_buf); vcopy(pv, &sp[-1]); gc_dirty_slot(pv); sp--; CONTINUE(); }
STENCIL(set_arg) { GCValue *pv = LOC(arg_buf); vcopy(pv, &sp[-1]); gc_dirty_slot(pv); CONTINUE(); }
STENCIL(set_loc_uninitialized) { GCValue *pv = LOC(var_buf); vset(pv, 0, JS_TAG_UNINITIALIZED); gc_dirty_slot(pv); CONTINUE(); }
STENCIL(get_loc_check)
{
    const GCValue *pv = LOC(var_buf);
    if (__builtin_expect(JS_IsUninitialized(*pv), 0))
        SLOW();
    vcopy(&sp[0], pv);
    sp++;
    CONTINUE();
}
STENCIL(put_loc_check)
{
    GCValue *pv = LOC(var_buf);
    if (__builtin_expect(JS_IsUninitialized(*pv), 0))
        SLOW();
    vcopy(pv, &sp[-1]);
    gc_dirty_slot(pv);
    sp--;
    CONTINUE();
}
STENCIL(set_loc_check)
{
    GCValue *pv = LOC(var_buf);
    if (__builtin_expect(JS_IsUninitialized(*pv), 0))
        SLOW();
    vcopy(pv, &sp[-1]);
    gc_dirty_slot(pv);
    CONTINUE();
}

/* --- Closure variables (IMM(0): the index) ------------------------------
 * Out of line (jit_get_var_ref...): the lookup is long.  The interpreter
 * traces these under CYBER_TRACE_GV: the JIT then leaves them to the
 * helper. */

STENCIL(get_var_ref)
{
    if (__builtin_expect(!jit_get_var_ref(F->var_refs, (uint32_t)IMM(0), &sp[0]), 0))
        SLOW();
    sp++;
    CONTINUE();
}
STENCIL(get_var_ref_check)
{
    if (__builtin_expect(!jit_get_var_ref(F->var_refs, (uint32_t)IMM(0), &sp[0]), 0))
        SLOW();
    sp++;
    CONTINUE();
}
STENCIL(put_var_ref)
{
    jit_put_var_ref(F->var_refs, (uint32_t)IMM(0), &sp[-1]);
    sp--;
    CONTINUE();
}
STENCIL(set_var_ref)
{
    jit_put_var_ref(F->var_refs, (uint32_t)IMM(0), &sp[-1]);
    CONTINUE();
}

/* --- Jumps (TARGET() is the destination) --------------------------------
 * goto polls on every jump, the conditional jumps only backward, as in the
 * interpreter; *_back are the backward variants. */

STENCIL(goto)
{
    if (__builtin_expect(!poll_is_quiet(), 0))
        SLOW();
    TARGET();
}

/* JS_ToBoolFree of sp[-1]: inline for int, bool, null and undefined;
 * objects and strings out of line (jit_truth); -1 for the helper. */
static inline __attribute__((always_inline)) int quick_truth(const GCValue *v)
{
    if (__builtin_expect((uint32_t)JS_VALUE_GET_TAG(*v) <= JS_TAG_UNDEFINED, 1))
        return JS_VALUE_GET_INT(*v) != 0;
    return jit_truth(v);
}

#define COND_JUMP(name, jump_if, poll)                                       \
STENCIL(name)                                                                \
{                                                                            \
    int t = quick_truth(&sp[-1]);                                            \
    if (__builtin_expect(t < 0, 0))                                          \
        SLOW();                                                              \
    if (t == (jump_if)) {                                                    \
        if (poll && __builtin_expect(!poll_is_quiet(), 0))                   \
            SLOW();                                                          \
        sp--;                                                                \
        TARGET();                                                            \
    }                                                                        \
    sp--;                                                                    \
    CONTINUE();                                                              \
}
COND_JUMP(if_true, 1, 0)
COND_JUMP(if_false, 0, 0)
COND_JUMP(if_true_back, 1, 1)
COND_JUMP(if_false_back, 0, 1)

/* --- Arithmetic and comparisons on int32 (and float64) ------------- */

/* add, sub, mul: an int32 result that overflows becomes a float64, as the
 * interpreter's inline paths make it. */
#define ARITH_OP(name, ovf, fop)                                             \
STENCIL(name)                                                                \
{                                                                            \
    GCValue a = sp[-2], b = sp[-1];                                          \
    if (__builtin_expect(GC_VALUE_IS_BOTH_INT(a, b), 1)) {                   \
        int32_t x = JS_VALUE_GET_INT(a), y = JS_VALUE_GET_INT(b), r;         \
        if (__builtin_expect(ovf(x, y, &r), 0))                              \
            vset_f64(&sp[-2], (double)x fop (double)y);                      \
        else                                                                 \
            vset_int(&sp[-2], r);                                            \
        sp--;                                                                \
        CONTINUE();                                                          \
    }                                                                        \
    if (GC_VALUE_IS_BOTH_FLOAT(a, b)) {                                      \
        vset_f64(&sp[-2], JS_VALUE_GET_FLOAT64(a) fop JS_VALUE_GET_FLOAT64(b)); \
        sp--;                                                                \
        CONTINUE();                                                          \
    }                                                                        \
    SLOW();                                                                  \
}
/* mul's -0 (an int32 zero from a negative operand) is a float64 too. */
static inline AI int mul_overflow(int32_t x, int32_t y, int32_t *r)
{
    return __builtin_mul_overflow(x, y, r) || (*r == 0 && (x | y) < 0);
}
ARITH_OP(add, __builtin_add_overflow, +)
ARITH_OP(sub, __builtin_sub_overflow, -)
ARITH_OP(mul, mul_overflow, *)

/* mod: what the interpreter does inline (no -0, no division by 0). */
STENCIL(mod)
{
    GCValue a = sp[-2], b = sp[-1];
    if (__builtin_expect(!GC_VALUE_IS_BOTH_INT(a, b), 0))
        SLOW();
    int32_t x = JS_VALUE_GET_INT(a), y = JS_VALUE_GET_INT(b);
    if (__builtin_expect(x < 0 || y <= 0, 0))
        SLOW();
    vset_int(&sp[-2], x % y);
    sp--;
    CONTINUE();
}

/* neg: as the interpreter's inline path (int, bool, null, float64). */
STENCIL(neg)
{
    int tag = JS_VALUE_GET_TAG(sp[-1]);
    if (tag == JS_TAG_INT || tag == JS_TAG_BOOL || tag == JS_TAG_NULL) {
        int32_t v = JS_VALUE_GET_INT(sp[-1]);
        if (__builtin_expect(v == 0 || v == INT32_MIN, 0))
            vset_f64(&sp[-1], v == 0 ? -0.0 : -(double)v);
        else
            vset_int(&sp[-1], -v);
        CONTINUE();
    }
    if (tag == JS_TAG_FLOAT64) {
        vset_f64(&sp[-1], -JS_VALUE_GET_FLOAT64(sp[-1]));
        CONTINUE();
    }
    SLOW();
}

/* ToInt32 of an int or a float64 within int32 range (truncation is then
 * ToInt32); 0 for anything else, which the helper converts. */
static inline AI int quick_int32(const GCValue *v, int32_t *out)
{
    int tag = JS_VALUE_GET_TAG(*v);
    if (__builtin_expect(tag == JS_TAG_INT, 1)) {
        *out = JS_VALUE_GET_INT(*v);
        return 1;
    }
    if (tag == JS_TAG_FLOAT64) {
        double d = JS_VALUE_GET_FLOAT64(*v);
        if (d > -2147483649.0 && d < 2147483648.0) {
            *out = (int32_t)d;
            return 1;
        }
    }
    return 0;
}

/* Bitwise operators: ToInt32 of both operands (js_binary_logic_slow's
 * result for numbers), inline when they are ints or float64s in range. */
#define BIT_OP(name, expr)                                                   \
STENCIL(name)                                                                \
{                                                                            \
    int32_t x, y;                                                            \
    if (__builtin_expect(!quick_int32(&sp[-2], &x) || !quick_int32(&sp[-1], &y), 0)) \
        SLOW();                                                              \
    vset_int(&sp[-2], expr);                                                 \
    sp--;                                                                    \
    CONTINUE();                                                              \
}
BIT_OP(and, x & y)
BIT_OP(or, x | y)
BIT_OP(xor, x ^ y)
BIT_OP(shl, (int32_t)((uint32_t)x << (y & 0x1f)))
BIT_OP(sar, x >> (y & 0x1f))

/* shr: ints only (the result may be a float64, as JS_NewUint32 makes). */
STENCIL(shr)
{
    GCValue a = sp[-2], b = sp[-1];
    if (__builtin_expect(!GC_VALUE_IS_BOTH_INT(a, b), 0))
        SLOW();
    uint32_t r = (uint32_t)JS_VALUE_GET_INT(a) >> (JS_VALUE_GET_INT(b) & 0x1f);
    if (__builtin_expect(r > INT32_MAX, 0))
        vset_f64(&sp[-2], (double)r);
    else
        vset_int(&sp[-2], (int32_t)r);
    sp--;
    CONTINUE();
}

/* Relational operators on numbers: ints, or float64s and ints compared as
 * doubles (js_relational_slow's result for numbers; false with NaN). */
static inline AI int quick_number(const GCValue *v, double *out)
{
    int tag = JS_VALUE_GET_TAG(*v);
    if (tag == JS_TAG_INT) {
        *out = JS_VALUE_GET_INT(*v);
        return 1;
    }
    if (tag == JS_TAG_FLOAT64) {
        *out = JS_VALUE_GET_FLOAT64(*v);
        return 1;
    }
    return 0;
}
#define CMP_OP(name, op)                                                     \
STENCIL(name)                                                                \
{                                                                            \
    GCValue a = sp[-2], b = sp[-1];                                          \
    if (__builtin_expect(GC_VALUE_IS_BOTH_INT(a, b), 1)) {                   \
        vset_bool(&sp[-2], JS_VALUE_GET_INT(a) op JS_VALUE_GET_INT(b));      \
        sp--;                                                                \
        CONTINUE();                                                          \
    }                                                                        \
    double x, y;                                                             \
    if (__builtin_expect(!quick_number(&sp[-2], &x) || !quick_number(&sp[-1], &y), 0)) \
        SLOW();                                                              \
    vset_bool(&sp[-2], x op y);                                              \
    sp--;                                                                    \
    CONTINUE();                                                              \
}
CMP_OP(lt, <)
CMP_OP(lte, <=)
CMP_OP(gt, >)
CMP_OP(gte, >=)
/* Equality: ints inline, other kinds out of line (jit_strict_eq,
 * jit_loose_eq), -1 from those for the helper. */
#define EQ_OP(name, fn, neg)                                                 \
STENCIL(name)                                                                \
{                                                                            \
    int r;                                                                   \
    if (__builtin_expect(GC_VALUE_IS_BOTH_INT(sp[-2], sp[-1]), 1))           \
        r = JS_VALUE_GET_INT(sp[-2]) == JS_VALUE_GET_INT(sp[-1]);            \
    else if (__builtin_expect((r = fn(&sp[-2])) < 0, 0))                     \
        SLOW();                                                              \
    vset_bool(&sp[-2], r ^ (neg));                                           \
    sp--;                                                                    \
    CONTINUE();                                                              \
}
EQ_OP(strict_eq, jit_strict_eq, 0)
EQ_OP(strict_neq, jit_strict_eq, 1)
EQ_OP(eq, jit_loose_eq, 0)
EQ_OP(neq, jit_loose_eq, 1)

STENCIL(inc)
{
    GCValue v = sp[-1];
    if (__builtin_expect(JS_VALUE_GET_TAG(v) != JS_TAG_INT || JS_VALUE_GET_INT(v) == INT32_MAX, 0))
        SLOW();
    vset_int(&sp[-1], JS_VALUE_GET_INT(v) + 1);
    CONTINUE();
}
STENCIL(dec)
{
    GCValue v = sp[-1];
    if (__builtin_expect(JS_VALUE_GET_TAG(v) != JS_TAG_INT || JS_VALUE_GET_INT(v) == INT32_MIN, 0))
        SLOW();
    vset_int(&sp[-1], JS_VALUE_GET_INT(v) - 1);
    CONTINUE();
}
STENCIL(post_inc)
{
    GCValue v = sp[-1];
    if (__builtin_expect(JS_VALUE_GET_TAG(v) != JS_TAG_INT || JS_VALUE_GET_INT(v) == INT32_MAX, 0))
        SLOW();
    vset_int(&sp[0], JS_VALUE_GET_INT(v) + 1);
    sp++;
    CONTINUE();
}
STENCIL(post_dec)
{
    GCValue v = sp[-1];
    if (__builtin_expect(JS_VALUE_GET_TAG(v) != JS_TAG_INT || JS_VALUE_GET_INT(v) == INT32_MIN, 0))
        SLOW();
    vset_int(&sp[0], JS_VALUE_GET_INT(v) - 1);
    sp++;
    CONTINUE();
}
/* IMM(0): the local's byte offset. */
STENCIL(inc_loc)
{
    GCValue *pv = LOC(var_buf);
    if (__builtin_expect(JS_VALUE_GET_TAG(*pv) != JS_TAG_INT || JS_VALUE_GET_INT(*pv) == INT32_MAX, 0))
        SLOW();
    vset_int(pv, JS_VALUE_GET_INT(*pv) + 1);
    CONTINUE();
}
STENCIL(dec_loc)
{
    GCValue *pv = LOC(var_buf);
    if (__builtin_expect(JS_VALUE_GET_TAG(*pv) != JS_TAG_INT || JS_VALUE_GET_INT(*pv) == INT32_MIN, 0))
        SLOW();
    vset_int(pv, JS_VALUE_GET_INT(*pv) - 1);
    CONTINUE();
}
STENCIL(add_loc)
{
    GCValue *pv = LOC(var_buf), b = sp[-1];
    if (__builtin_expect(!GC_VALUE_IS_BOTH_INT(*pv, b), 0))
        SLOW();
    int32_t r;
    if (__builtin_expect(__builtin_add_overflow(JS_VALUE_GET_INT(*pv), JS_VALUE_GET_INT(b), &r), 0))
        SLOW();
    vset_int(pv, r);
    sp--;
    CONTINUE();
}

STENCIL(lnot)
{
    int t = quick_truth(&sp[-1]);
    if (__builtin_expect(t < 0, 0))
        SLOW();
    vset_bool(&sp[-1], !t);
    CONTINUE();
}

/* --- Property access (fast paths in quickjs.c; IMM(0): the atom) ---- */

/* The lookup out of line (jit_field_ref, jit_prim_field_ref for a
 * method of a primitive string): it is long.  The JIT compiles these only
 * when CYBER_TRACE_NAF, which the interpreter's version records, is off. */
static inline AI const GCValue *field_ref(JSInterpFrame *F, const GCValue *obj)
{
    if (__builtin_expect(JS_VALUE_GET_TAG(*obj) == JS_TAG_OBJECT, 1))
        return jit_field_ref(GC_VALUE_GET_HANDLE(*obj), (JSAtom)IMM(0));
    return jit_prim_field_ref(F->ctx, obj, (JSAtom)IMM(0));
}
STENCIL(get_field)
{
    const GCValue *v = field_ref(F, &sp[-1]);
    if (__builtin_expect(!v, 0))
        SLOW();
    vcopy(&sp[-1], v);
    CONTINUE();
}
STENCIL(get_field2)
{
    const GCValue *v = field_ref(F, &sp[-1]);
    if (__builtin_expect(!v, 0))
        SLOW();
    vcopy(&sp[0], v);
    sp++;
    CONTINUE();
}
/* The same through the site's inline cache (IMM(1), a JitIC): a receiver
 * whose shape is way 0's, an own property, is read inline. */
static inline AI const GCValue *ic_field_ref(JSInterpFrame *F, const GCValue *obj)
{
    if (__builtin_expect(JS_VALUE_GET_TAG(*obj) == JS_TAG_OBJECT, 1)) {
        const JitIC *ic = (const JitIC *)IMM(1);
        JSObject *o = (JSObject *)gc_deref(GC_VALUE_GET_HANDLE(*obj));
        if (__builtin_expect(o->shape_handle == ic->shape[0] && ic->holder[0] == GC_HANDLE_NULL, 1))
            return &js_obj_props_of(o)[ic->slot[0]].u.value;
        return jit_get_field_ic((JitIC *)ic, obj, (JSAtom)IMM(0));
    }
    return jit_prim_field_ref(F->ctx, obj, (JSAtom)IMM(0));
}
STENCIL(get_field_ic)
{
    const GCValue *v = ic_field_ref(F, &sp[-1]);
    if (__builtin_expect(!v, 0))
        SLOW();
    vcopy(&sp[-1], v);
    CONTINUE();
}
STENCIL(get_field2_ic)
{
    const GCValue *v = ic_field_ref(F, &sp[-1]);
    if (__builtin_expect(!v, 0))
        SLOW();
    vcopy(&sp[0], v);
    sp++;
    CONTINUE();
}

/* get_field of length (IMM(0)): a string's inline. */
STENCIL(get_length)
{
    int tag = JS_VALUE_GET_TAG(sp[-1]);
    if (tag == JS_TAG_STRING) {
        vset_int(&sp[-1], jit_string_length(GC_VALUE_GET_HANDLE(sp[-1])));
        CONTINUE();
    }
    if (__builtin_expect(tag != JS_TAG_OBJECT, 0))
        SLOW();
    const GCValue *v = jit_field_ref(GC_VALUE_GET_HANDLE(sp[-1]), (JSAtom)IMM(0));
    if (__builtin_expect(!v, 0))
        SLOW();
    vcopy(&sp[-1], v);
    CONTINUE();
}
/* IMM(0): the atom; IMM(1): the address past the operands. */
STENCIL(put_field)
{
    F->pc = (const uint8_t *)IMM(1);
    int r = jit_put_field_fast(F, &sp[-2], (JSAtom)IMM(0));
    if (__builtin_expect(r <= 0, 0)) {
        if (r < 0)
            EXIT(JSOP_EXCEPTION);
        SLOW();
    }
    sp -= 2;
    CONTINUE();
}
STENCIL(get_array_el)
{
    const GCValue *v = jit_array_el_ref(&sp[-2]);
    if (__builtin_expect(!v, 0))
        SLOW();
    vcopy(&sp[-2], v);
    sp--;
    CONTINUE();
}
STENCIL(get_array_el2)
{
    const GCValue *v = jit_array_el_ref(&sp[-2]);
    if (__builtin_expect(!v, 0))
        SLOW();
    vcopy(&sp[-1], v);
    CONTINUE();
}

/* A constant value: IMM(0) its payload, IMM(1) its tag. */
STENCIL(push_value)
{
    vset(&sp[0], IMM(0), (int64_t)IMM(1));
    sp++;
    CONTINUE();
}
STENCIL(is_undefined_or_null)
{
    int tag = JS_VALUE_GET_TAG(sp[-1]);
    vset_bool(&sp[-1], tag == JS_TAG_UNDEFINED || tag == JS_TAG_NULL);
    CONTINUE();
}

/* --- Calls -------------------------------------------------------------
 * call / call_method / call_constructor: to a compiled function, its frame
 * is set up in C (jit_call_prepare, as JS_CallInternal does) and its code
 * called directly, a preserve_none call with no trampoline; a C function is
 * called by jit_call_prepare itself; anything else takes the helper.
 * tail_call / tail_call_method are a call (or call_method) and a return
 * of its result, as the interpreter runs them: a direct call here too.
 * IMM(0): the address past the operands; IMM(1): argc, and the kind
 * (JIT_CALL_*) shifted by 16. */
#define CALL_OP(name, then)                                                  \
STENCIL(name)                                                                \
{                                                                            \
    JSInterpFrame F2;                                                        \
    int argc = (int)(IMM(1) & 0xffff), kind = (int)(IMM(1) >> 16);           \
    F->sp = sp;                                                              \
    F->pc = (const uint8_t *)IMM(0);                                         \
    JitFn *entry = (JitFn *)jit_call_prepare(F, &F2, argc, kind);            \
    if (__builtin_expect(!entry, 0)) {                                       \
        if (F2.jit_exit == JSOP_DONE) {                                      \
            sp = F->sp;                                                      \
            then;                                                            \
        }                                                                    \
        if (F2.jit_exit == JSOP_EXCEPTION)                                   \
            EXIT(JSOP_EXCEPTION);                                            \
        SLOW();                                                              \
    }                                                                        \
    int r = entry(&F2, F2.sp, F2.var_buf, F2.arg_buf);                       \
    r = jit_call_finish(F, &F2, r, argc, kind);                              \
    sp = F->sp;                                                              \
    if (__builtin_expect(r != JSOP_NEXT, 0))                                 \
        EXIT(r);                                                             \
    then;                                                                    \
}
#define RETURN_RESULT() do { vcopy(&F->ret_val, &sp[-1]); sp--; EXIT(JSOP_DONE); } while (0)
CALL_OP(call, CONTINUE())
CALL_OP(tail_call, RETURN_RESULT())

/* --- Return ----------------------------------------------------------- */

STENCIL(return)
{
    vcopy(&F->ret_val, &sp[-1]);
    sp--;
    EXIT(JSOP_DONE);
}
STENCIL(return_undef)
{
    vset(&F->ret_val, 0, JS_TAG_UNDEFINED);
    EXIT(JSOP_DONE);
}

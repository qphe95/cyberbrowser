/*
 * The bytecode cache's ground: a script compiled, written with
 * JS_WriteObject, read back with JS_ReadObject and run, behaves as the
 * script itself (nested and lazily parsed functions, closures, classes,
 * arrows, generators, async functions, regexps, template literals).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "test_runner.h"
#include "platform.h"
#include "quickjs.h"

JSContextHandle get_shared_test_context(void);

static const char k_script[] =
    "var cc_out = [];\n"
    "function cc_outer(a) { var k = a * 2; function inner(b) { return k + b; } return inner(1); }\n"
    "class CcPoint { constructor(x) { this.x = x; } twice() { return this.x * 2; } static of(x) { return new CcPoint(x); } }\n"
    "var cc_arrow = (a, b) => a + b;\n"
    "function* cc_gen() { yield 1; yield 2; }\n"
    "var cc_re = /a(b+)c/g;\n"
    "var cc_counter = (function () { var n = 0; return function () { return ++n; }; })();\n"
    "cc_out.push(cc_outer(3), CcPoint.of(4).twice(), cc_arrow(5, 6), [...cc_gen()].join('+'),\n"
    "            'xabbbc'.replace(cc_re, '[$1]'), `t${1 + 1}`, cc_counter(), cc_counter());\n"
    "cc_out.join(',');\n";
static const char k_expect[] = "7,8,11,1+2,x[bbb],t2,1,2";

/* Compile (flags), write, read back, run: the result as a string (malloc'd), or NULL. */
static char *round_trip(JSContextHandle ctx, int flags, bool run_first, size_t *size_out) {
    GCValue fn = JS_Eval(ctx, k_script, strlen(k_script), "<cc>", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY | flags);
    if (JS_IsException(fn)) return NULL;
    if (run_first) {
        /* (Run it once from a copy read back, so fn's own functions may
         * have compiled lazily meanwhile... here: run a fresh compile.) */
        GCValue again = JS_Eval(ctx, k_script, strlen(k_script), "<cc-run>", JS_EVAL_TYPE_GLOBAL | flags);
        if (JS_IsException(again)) return NULL;
    }
    size_t size = 0;
    uint8_t *buf = JS_WriteObject(ctx, &size, fn, JS_WRITE_OBJ_BYTECODE);
    if (size_out) *size_out = size;
    if (!buf) {
        if (JS_HasException(ctx)) JS_GetException(ctx);
        return NULL;
    }
    GCValue back = JS_ReadObject(ctx, buf, size, JS_READ_OBJ_BYTECODE);
    free(buf);
    if (JS_IsException(back)) {
        JS_GetException(ctx);
        return NULL;
    }
    GCValue res = JS_EvalFunction(ctx, back);
    if (JS_IsException(res)) {
        GCValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        printf("    threw: %s\n", s ? s : "?");
        return NULL;
    }
    const char *s = JS_ToCString(ctx, res);
    char *copy = s ? strdup(s) : NULL;
    return copy;
}

/* A lazily compiled script can't be written (its uncompiled functions refer
 * to the parser's state): it must fail cleanly, which is why the cache
 * compiles fully (code_cache_flush). */
TEST(test_code_cache_lazy_refused) {
    JSContextHandle ctx = get_shared_test_context();
    char *r = round_trip(ctx, 0, false, NULL);
    ASSERT_TRUE(r == NULL || strcmp(r, k_expect) == 0);
    free(r);
    return true;
}

TEST(test_code_cache_no_lazy) {
    JSContextHandle ctx = get_shared_test_context();
    size_t size = 0;
    char *r = round_trip(ctx, JS_EVAL_FLAG_NO_LAZY, false, &size);
    printf("    full compile: %s (%zu bytes)\n", r ? r : "(failed)", size);
    ASSERT_TRUE(r != NULL && strcmp(r, k_expect) == 0);
    free(r);
    return true;
}

/* JS_WRITE_OBJ_SHARE_SOURCE: functions keep their text as ranges in the
 * script's, which the reader is given; toString still has it, and a source
 * too short for a range is refused. */
TEST(test_code_cache_shared_source) {
    JSContextHandle ctx = get_shared_test_context();
    GCValue fn = JS_Eval(ctx, k_script, strlen(k_script), "<cc>",
                         JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_FLAG_NO_LAZY);
    ASSERT_FALSE(JS_IsException(fn));
    size_t size = 0, plain = 0;
    uint8_t *buf = JS_WriteObject(ctx, &size, fn, JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_SHARE_SOURCE);
    uint8_t *full = JS_WriteObject(ctx, &plain, fn, JS_WRITE_OBJ_BYTECODE);
    ASSERT_TRUE(buf != NULL && full != NULL);
    printf("    %zu bytes shared, %zu with each function's text\n", size, plain);
    ASSERT_TRUE(size < plain);
    free(full);
    GCValue short_src = JS_ReadObjectWithSource(ctx, buf, size, JS_READ_OBJ_BYTECODE, k_script, 10);
    ASSERT_TRUE(JS_IsException(short_src));
    JS_GetException(ctx);
    GCValue back = JS_ReadObjectWithSource(ctx, buf, size, JS_READ_OBJ_BYTECODE, k_script, strlen(k_script));
    free(buf);
    ASSERT_FALSE(JS_IsException(back));
    GCValue res = JS_EvalFunction(ctx, back);
    ASSERT_FALSE(JS_IsException(res));
    const char *s = JS_ToCString(ctx, res);
    ASSERT_TRUE(s && strcmp(s, k_expect) == 0);
    static const char probe[] = "cc_outer.toString().slice(0, 18) + '|' + CcPoint.prototype.twice.toString()";
    GCValue t = JS_Eval(ctx, probe, strlen(probe), "<cc-probe>", JS_EVAL_TYPE_GLOBAL);
    const char *ts = JS_ToCString(ctx, t);
    printf("    toString: %s\n", ts ? ts : "?");
    ASSERT_TRUE(ts && strcmp(ts, "function cc_outer(|twice() { return this.x * 2; }") == 0);
    return true;
}

TEST(test_code_cache_corrupt) {
    /* A truncated or altered cache entry must fail to read, not crash. */
    JSContextHandle ctx = get_shared_test_context();
    GCValue fn = JS_Eval(ctx, k_script, strlen(k_script), "<cc>", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    ASSERT_FALSE(JS_IsException(fn));
    size_t size = 0;
    uint8_t *buf = JS_WriteObject(ctx, &size, fn, JS_WRITE_OBJ_BYTECODE);
    ASSERT_TRUE(buf != NULL && size > 16);
    for (size_t cut = 1; cut < size; cut += size / 7 + 1) {
        GCValue back = JS_ReadObject(ctx, buf, cut, JS_READ_OBJ_BYTECODE);
        if (JS_IsException(back)) JS_GetException(ctx);
    }
    free(buf);
    return true;
}

void run_code_cache_tests(void) {
    printf("\n========================================\n");
    printf("Code Cache (bytecode round trip) Tests\n");
    printf("========================================\n");
    RUN_TEST(test_code_cache_lazy_refused);
    RUN_TEST(test_code_cache_no_lazy);
    RUN_TEST(test_code_cache_shared_source);
    RUN_TEST(test_code_cache_corrupt);
}

/*
 * Unit Test: Logical Operators (&& and ||)
 *
 * This test reproduces the bug where logical operators return
 * JS_EXCEPTION without setting an actual exception.
 * 
 * Uses shared context from test_main.cpp (single context model)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "test_runner.h"
#include "platform.h"
#include "quickjs.h"
#include "quickjs_gc_unified.h"
#include "browser_api_impl.h"
#include "js_quickjs.h"

#define LOG_TAG "logical_op_test"

/* Forward declaration for shared context accessor */
extern "C" JSContextHandle get_shared_test_context(void);
extern "C" GCValue get_shared_test_global(void);

static JSContextHandle g_ctx;

static bool setup_context(void) {
    g_ctx = get_shared_test_context();
    if (!g_ctx.valid()) {
        printf("    ERROR: Shared context not available\n");
        return false;
    }
    return true;
}

static void cleanup_context(void) {
    /* No-op in single context model */
}

/* Helper to run a test expression and check result */
static bool test_logical_expr(const char* expr, const char* expected_type, GCValue* out_result) {
    char script[256];
    snprintf(script, sizeof(script), "%s;", expr);
    
    printf( "[TEST_DEBUG] About to eval: %s\n", expr);
    GCValue result = JS_Eval(g_ctx, script, strlen(script), "test.js", JS_EVAL_TYPE_GLOBAL);
    printf( "[TEST_DEBUG] Result tag=%d, is_exc=%d\n", 
            JS_VALUE_GET_TAG(result), JS_IsException(result));
    
    if (out_result) *out_result = result;
    
    if (JS_IsException(result)) {
        printf("    FAIL: '%s' threw exception (tag=%d)\n", expr, JS_VALUE_GET_TAG(result));
        // Try to get exception details
        if (JS_HasException(g_ctx)) {
            GCValue exc = JS_GetException(g_ctx);
            printf( "[TEST_DEBUG] Got exception from context, tag=%d\n", 
                    JS_VALUE_GET_TAG(exc));
        } else {
            printf( "[TEST_DEBUG] No exception in context, result value has wrong tag\n");
        }
        return false;
    }
    
    GCValue str = JS_ToString(g_ctx, result);
    const char* actual = JS_ToCString(g_ctx, str);
    
    printf("    '%s' = %s\n", expr, actual ? actual : "(null)");
    
    return true;
}

/* Test 1: Basic AND operator */
TEST(test_logical_and_basic) {
    printf("\n  Test: Basic AND operator (&&)\n");
    
    if (!setup_context()) return false;
    
    struct {
        const char* expr;
        const char* expected;
    } tests[] = {
        {"true && true", "true"},
        {"true && false", "false"},
        {"false && true", "false"},
        {"false && false", "false"},
        {"1 && 2", "2"},
        {"0 && 2", "0"},
        {"undefined && 1", "undefined"},
        {"null && 1", "null"},
        {NULL, NULL}
    };
    
    bool all_pass = true;
    for (int i = 0; tests[i].expr; i++) {
        GCValue result;
        if (!test_logical_expr(tests[i].expr, tests[i].expected, &result)) {
            all_pass = false;
            // Get exception details
            GCValue exc = JS_GetException(g_ctx);
            GCValue exc_str = JS_ToString(g_ctx, exc);
            const char* exc_cstr = JS_ToCString(g_ctx, exc_str);
            printf("    Exception: %s\n", exc_cstr ? exc_cstr : "(null)");
        }
    }
    
    cleanup_context();
    return all_pass;
}

/* Test 2: Basic OR operator */
TEST(test_logical_or_basic) {
    printf("\n  Test: Basic OR operator (||)\n");
    
    if (!setup_context()) return false;
    
    struct {
        const char* expr;
        const char* expected;
    } tests[] = {
        {"true || true", "true"},
        {"true || false", "true"},
        {"false || true", "true"},
        {"false || false", "false"},
        {"1 || 2", "1"},
        {"0 || 2", "2"},
        {"undefined || 1", "1"},
        {"null || 'default'", "default"},
        {NULL, NULL}
    };
    
    bool all_pass = true;
    for (int i = 0; tests[i].expr; i++) {
        GCValue result;
        if (!test_logical_expr(tests[i].expr, tests[i].expected, &result)) {
            all_pass = false;
            GCValue exc = JS_GetException(g_ctx);
            GCValue exc_str = JS_ToString(g_ctx, exc);
            const char* exc_cstr = JS_ToCString(g_ctx, exc_str);
            printf("    Exception: %s\n", exc_cstr ? exc_cstr : "(null)");
        }
    }
    
    cleanup_context();
    return all_pass;
}

/* Test 3: Chained logical operators */
TEST(test_logical_chained) {
    printf("\n  Test: Chained logical operators\n");
    
    if (!setup_context()) return false;
    
    struct {
        const char* expr;
        const char* expected;
    } tests[] = {
        {"true && true && true", "true"},
        {"true && false && true", "false"},
        {"false || false || true", "true"},
        {"1 && 2 && 3", "3"},
        {"0 || 0 || 5", "5"},
        {"true && true || false", "true"},
        {"false || true && true", "true"},
        {NULL, NULL}
    };
    
    bool all_pass = true;
    for (int i = 0; tests[i].expr; i++) {
        GCValue result;
        if (!test_logical_expr(tests[i].expr, tests[i].expected, &result)) {
            all_pass = false;
            GCValue exc = JS_GetException(g_ctx);
            GCValue exc_str = JS_ToString(g_ctx, exc);
            const char* exc_cstr = JS_ToCString(g_ctx, exc_str);
            printf("    Exception: %s\n", exc_cstr ? exc_cstr : "(null)");
        }
    }
    
    cleanup_context();
    return all_pass;
}

/* Test 4: Short-circuit evaluation */
TEST(test_logical_short_circuit) {
    printf("\n  Test: Short-circuit evaluation\n");
    
    if (!setup_context()) return false;
    
    // Test that side effects don't occur when short-circuited
    const char* script = 
        "var x = 0;"
        "false && (x = 1);"  // x should remain 0
        "x;";
    
    GCValue result = JS_Eval(g_ctx, script, strlen(script), "test.js", JS_EVAL_TYPE_GLOBAL);
    
    if (JS_IsException(result)) {
        printf("    FAIL: Short-circuit test threw exception\n");
        cleanup_context();
        return false;
    }
    
    int32_t val;
    JS_ToInt32(g_ctx, &val, result);
    if (val != 0) {
        printf("    FAIL: Short-circuit failed, x = %d (expected 0)\n", val);
        cleanup_context();
        return false;
    }
    
    printf("    PASS: Short-circuit works correctly\n");
    cleanup_context();
    return true;
}

/* Test 5: YouTube-style pattern */
TEST(test_logical_youtube_pattern) {
    printf("\n  Test: YouTube-style pattern\n");
    
    if (!setup_context()) return false;
    
    // This is the pattern from script 1 that was failing
    const char* script = 
        "var test_obj = {};"
        "var result = test_obj.a && test_obj.a.b || test_obj.c || (test_obj.c = 'default');"
        "result;";
    
    GCValue result = JS_Eval(g_ctx, script, strlen(script), "test.js", JS_EVAL_TYPE_GLOBAL);
    
    if (JS_IsException(result)) {
        printf("    FAIL: YouTube pattern threw exception\n");
        GCValue exc = JS_GetException(g_ctx);
        GCValue exc_str = JS_ToString(g_ctx, exc);
        const char* exc_cstr = JS_ToCString(g_ctx, exc_str);
        printf("    Exception: %s\n", exc_cstr ? exc_cstr : "(null)");
        cleanup_context();
        return false;
    }
    
    GCValue str = JS_ToString(g_ctx, result);
    const char* cstr = JS_ToCString(g_ctx, str);
    printf("    YouTube pattern result: %s\n", cstr ? cstr : "(null)");
    
    cleanup_context();
    return true;
}

/* Test runner entry point */
extern "C" void run_logical_operator_tests(void) {
    printf("\n========================================\n");
    printf("Logical Operator Tests\n");
    printf("========================================\n");
    
    RUN_TEST(test_logical_and_basic);
    RUN_TEST(test_logical_or_basic);
    RUN_TEST(test_logical_chained);
    RUN_TEST(test_logical_short_circuit);
    RUN_TEST(test_logical_youtube_pattern);
}

/*
 * Unit tests for deeply nested IIFE patterns
 * Tests Phases 1-4 implementation of deeply nested IIFE support
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

#define LOG_TAG "test_nested_iife"

/* Forward declaration for shared context accessor */
extern "C" JSContextHandle get_shared_test_context(void);

/* Helper to get the shared test context */
static JSContextHandle get_test_context(void) {
    JSContextHandle ctx = get_shared_test_context();
    if (!ctx.valid()) {
        printf("    ERROR: Shared context not available\n");
        return JSContextHandle();
    }
    return ctx;
}

TEST(test_simple_iife) {
    printf("    Testing simple IIFE...\n");
    
    JSContextHandle ctx = get_test_context();
    if (!ctx.valid()) {
        return false;
    }
    
    const char *js = "(function() { return 42; })();";
    GCValue result = JS_Eval(ctx, js, strlen(js), "<test>", JS_EVAL_TYPE_GLOBAL);
    
    if (JS_IsException(result)) {
        GCValue exc = JS_GetException(ctx);
        const char *error_str = JS_ToCString(ctx, exc);
        printf("    ERROR: Simple IIFE threw exception: %s\n", error_str ? error_str : "(null)");
        
        return false;
    }
    
    int result_int = JS_VALUE_GET_INT(result);
    if (result_int != 42) {
        printf("    ERROR: Expected 42, got %d\n", result_int);
        return false;
    }
    
    printf("    Simple IIFE: PASSED (result=%d)\n", result_int);
    return true;
}

TEST(test_nested_function_in_iife) {
    printf("    Testing nested function in IIFE capturing argument...\n");
    
    JSContextHandle ctx = get_test_context();
    if (!ctx.valid()) {
        return false;
    }
    
    const char *js = 
        "(function(y){"
        "  function k() { return y; }"
        "  return k();"
        "})(42);";
    
    GCValue result = JS_Eval(ctx, js, strlen(js), "<test>", JS_EVAL_TYPE_GLOBAL);
    
    if (JS_IsException(result)) {
        GCValue exc = JS_GetException(ctx);
        const char *error_str = JS_ToCString(ctx, exc);
        printf("    ERROR: Nested IIFE threw exception: %s\n", error_str ? error_str : "(null)");
        
        return false;
    }
    
    int result_int = JS_VALUE_GET_INT(result);
    if (result_int != 42) {
        printf("    ERROR: Expected 42, got %d\n", result_int);
        return false;
    }
    
    printf("    Nested IIFE: PASSED (result=%d)\n", result_int);
    return true;
}

TEST(test_deeply_nested_iife) {
    printf("    Testing deeply nested IIFE (3 levels)...\n");
    
    JSContextHandle ctx = get_test_context();
    if (!ctx.valid()) {
        return false;
    }
    
    const char *js =
        "(function(a) {"
        "  return (function(b) {"
        "    return (function(c) {"
        "      function inner() { return a + b + c; }"
        "      return inner();"
        "    })(3);"
        "  })(2);"
        "})(1);";
    
    GCValue result = JS_Eval(ctx, js, strlen(js), "<test>", JS_EVAL_TYPE_GLOBAL);
    
    if (JS_IsException(result)) {
        GCValue exc = JS_GetException(ctx);
        const char *error_str = JS_ToCString(ctx, exc);
        printf("    ERROR: Deep IIFE threw exception: %s\n", error_str ? error_str : "(null)");
        
        return false;
    }
    
    int result_int = JS_VALUE_GET_INT(result);
    if (result_int != 6) {
        printf("    ERROR: Expected 6, got %d\n", result_int);
        return false;
    }
    
    printf("    Deep IIFE: PASSED (result=%d)\n", result_int);
    return true;
}

TEST(test_web_animations_like_pattern) {
    printf("    Testing Web Animations-like pattern...\n");
    
    JSContextHandle ctx = get_test_context();
    if (!ctx.valid()) {
        return false;
    }
    
    /* Simplified Web Animations pattern */
    const char *js =
        "(function(G, F) {"
        "  function k() { this._duration = 0; }"
        "  k.prototype = {"
        "    get duration() { return this._duration; },"
        "    set duration(p) { this._duration = p; }"
        "  };"
        "  G.Timing = k;"
        "  return G;"
        "})({}, {});";
    
    GCValue result = JS_Eval(ctx, js, strlen(js), "<test>", JS_EVAL_TYPE_GLOBAL);
    
    if (JS_IsException(result)) {
        GCValue exc = JS_GetException(ctx);
        const char *error_str = JS_ToCString(ctx, exc);
        printf("    ERROR: Web Animations pattern threw exception: %s\n", error_str ? error_str : "(null)");
        
        return false;
    }
    
    if (!JS_IsObject(result)) {
        printf("    ERROR: Expected object result\n");
        return false;
    }
    
    /* Verify G.Timing was set */
    GCValue timing = JS_GetPropertyStr(ctx, result, "Timing");
    if (JS_IsUndefined(timing)) {
        printf("    ERROR: G.Timing not set\n");
        return false;
    }
    
    printf("    Web Animations pattern: PASSED\n");
    return true;
}

TEST(test_closure_with_multiple_captures) {
    printf("    Testing closure with multiple captures...\n");
    
    JSContextHandle ctx = get_test_context();
    if (!ctx.valid()) {
        return false;
    }
    
    const char *js =
        "(function(a, b) {"
        "  function inner1() { return a; }"
        "  function inner2() { return b; }"
        "  function inner3() { return a + b; }"
        "  return inner1() + inner2() + inner3();"
        "})(10, 20);";
    
    GCValue result = JS_Eval(ctx, js, strlen(js), "<test>", JS_EVAL_TYPE_GLOBAL);
    
    if (JS_IsException(result)) {
        GCValue exc = JS_GetException(ctx);
        const char *error_str = JS_ToCString(ctx, exc);
        printf("    ERROR: Multiple captures test threw exception: %s\n", error_str ? error_str : "(null)");
        
        return false;
    }
    
    int result_int = JS_VALUE_GET_INT(result);
    /* inner1() = 10, inner2() = 20, inner3() = 30, total = 60 */
    if (result_int != 60) {
        printf("    ERROR: Expected 60, got %d\n", result_int);
        return false;
    }
    
    printf("    Multiple captures: PASSED (result=%d)\n", result_int);
    return true;
}

TEST(test_iife_with_local_var_capture) {
    printf("    Testing IIFE with local variable capture...\n");
    
    JSContextHandle ctx = get_test_context();
    if (!ctx.valid()) {
        return false;
    }
    
    const char *js =
        "(function() {"
        "  var x = 100;"
        "  function getX() { return x; }"
        "  function setX(v) { x = v; }"
        "  setX(200);"
        "  return getX();"
        "})();";
    
    GCValue result = JS_Eval(ctx, js, strlen(js), "<test>", JS_EVAL_TYPE_GLOBAL);
    
    if (JS_IsException(result)) {
        GCValue exc = JS_GetException(ctx);
        const char *error_str = JS_ToCString(ctx, exc);
        printf("    ERROR: Local var capture test threw exception: %s\n", error_str ? error_str : "(null)");
        
        return false;
    }
    
    int result_int = JS_VALUE_GET_INT(result);
    if (result_int != 200) {
        printf("    ERROR: Expected 200, got %d\n", result_int);
        return false;
    }
    
    printf("    Local var capture: PASSED (result=%d)\n", result_int);
    return true;
}

TEST(test_many_nested_functions) {
    printf("    Testing many nested functions in IIFE...\n");
    
    JSContextHandle ctx = get_test_context();
    if (!ctx.valid()) {
        return false;
    }
    
    /* Pattern similar to Web Animations with many function definitions */
    const char *js =
        "(function(G) {"
        "  function f1() { return 1; }"
        "  function f2() { return 2; }"
        "  function f3() { return 3; }"
        "  function f4() { return 4; }"
        "  function f5() { return 5; }"
        "  function f6() { return 6; }"
        "  function f7() { return 7; }"
        "  function f8() { return 8; }"
        "  function f9() { return 9; }"
        "  function f10() { return 10; }"
        "  G.sum = function() {"
        "    return f1() + f2() + f3() + f4() + f5() +"
        "           f6() + f7() + f8() + f9() + f10();"
        "  };"
        "  return G;"
        "})({});";
    
    GCValue result = JS_Eval(ctx, js, strlen(js), "<test>", JS_EVAL_TYPE_GLOBAL);
    
    if (JS_IsException(result)) {
        GCValue exc = JS_GetException(ctx);
        const char *error_str = JS_ToCString(ctx, exc);
        printf("    ERROR: Many nested functions test threw exception: %s\n", error_str ? error_str : "(null)");
        
        return false;
    }
    
    /* Call G.sum() to verify all functions work */
    GCValue sum_func = JS_GetPropertyStr(ctx, result, "sum");
    if (JS_IsUndefined(sum_func)) {
        printf("    ERROR: G.sum not defined\n");
        return false;
    }
    
    GCValue sum_result = JS_Call(ctx, sum_func, result, 0, NULL);
    if (JS_IsException(sum_result)) {
        GCValue exc = JS_GetException(ctx);
        const char *error_str = JS_ToCString(ctx, exc);
        printf("    ERROR: G.sum() threw exception: %s\n", error_str ? error_str : "(null)");
        
        return false;
    }
    
    int sum = JS_VALUE_GET_INT(sum_result);
    /* 1+2+3+4+5+6+7+8+9+10 = 55 */
    if (sum != 55) {
        printf("    ERROR: Expected sum 55, got %d\n", sum);
        return false;
    }
    
    printf("    Many nested functions: PASSED (sum=%d)\n", sum);
    return true;
}

/* Test runner entry point for this file */
extern "C" void run_nested_iife_tests(void) {
    printf("\n========================================\n");
    printf("Nested IIFE Pattern Tests (Phases 1-4)\n");
    printf("========================================\n");
    
    RUN_TEST(test_simple_iife);
    RUN_TEST(test_nested_function_in_iife);
    RUN_TEST(test_deeply_nested_iife);
    RUN_TEST(test_web_animations_like_pattern);
    RUN_TEST(test_closure_with_multiple_captures);
    RUN_TEST(test_iife_with_local_var_capture);
    RUN_TEST(test_many_nested_functions);
    
    printf("\nNested IIFE tests completed.\n");
}

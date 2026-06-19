/*
 * Debug test for exception handling
 * This test verifies that exceptions work correctly end-to-end
 * 
 * Uses shared context from test_main.cpp (single context model)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "test_runner.h"
#include "quickjs.h"
#include "quickjs_gc_unified.h"
#include "browser_api_impl.h"
#include "js_quickjs.h"

/* Forward declaration for shared context accessor */
extern "C" JSContextHandle get_shared_test_context(void);
extern "C" GCValue get_shared_test_global(void);

static JSContextHandle get_test_context(void) {
    return get_shared_test_context();
}

/* Test 1: Simple throw and catch */
TEST(debug_simple_throw_catch) {
    printf("\n  Test 1: Simple throw/catch\n");
    
    JSContextHandle ctx;
    ctx = get_test_context(); 
    if (!ctx.valid()) return false;
    
    const char* script = "throw new Error('test error');";
    GCValue result = JS_Eval(ctx, script, strlen(script), "test.js", JS_EVAL_TYPE_GLOBAL);
    
    if (!JS_IsException(result)) {
        printf("    FAIL: Expected exception, got tag %d\n", JS_VALUE_GET_TAG(result));
        // cleanup removed - shared context
        return false;
    }
    printf("    ✓ Exception detected (tag %d)\n", JS_VALUE_GET_TAG(result));
    
    GCValue exc = JS_GetException(ctx);
    int exc_tag = JS_VALUE_GET_TAG(exc);
    printf("    Exception object tag: %d\n", exc_tag);
    
    if (JS_IsUninitialized(exc)) {
        printf("    FAIL: Exception is UNINITIALIZED!\n");
        // cleanup removed - shared context
        return false;
    }
    printf("    ✓ Exception is not uninitialized\n");
    
    // Try to convert to string
    GCValue str = JS_ToString(ctx, exc);
    if (JS_IsException(str)) {
        printf("    FAIL: JS_ToString threw exception\n");
        // cleanup removed - shared context
        return false;
    }
    printf("    ✓ JS_ToString succeeded\n");
    
    const char* cstr = JS_ToCString(ctx, str);
    printf("    Exception string: %s\n", cstr ? cstr : "(null)");
    
    // cleanup removed - shared context
    return true;
}

/* Test 2: Error prototype methods */
TEST(debug_error_prototypes) {
    printf("\n  Test 2: Error prototype methods\n");
    
    JSContextHandle ctx;
    ctx = get_test_context(); if (!ctx.valid()) return false;
    
    // Test each error type individually
    const char* error_types[] = {
        "Error",
        "TypeError", 
        "ReferenceError",
        "SyntaxError",
        "RangeError",
        "URIError",
        "InternalError",
        "AggregateError"
    };
    
    for (int i = 0; i < 8; i++) {
        char script[256];
        snprintf(script, sizeof(script), 
            "var E = %s; var proto = E.prototype; var e = new E('test'); 'ok';",
            error_types[i]);
        
        GCValue result = JS_Eval(ctx, script, strlen(script), "test.js", JS_EVAL_TYPE_GLOBAL);
        
        if (JS_IsException(result)) {
            GCValue exc = JS_GetException(ctx);
            GCValue name = JS_GetPropertyStr(ctx, exc, "name");
            const char* name_cstr = JS_ToCString(ctx, name);
            GCValue msg = JS_GetPropertyStr(ctx, exc, "message");
            const char* msg_cstr = JS_ToCString(ctx, msg);
            printf("    %s threw: %s: %s\n", error_types[i], 
                   name_cstr ? name_cstr : "?",
                   msg_cstr ? msg_cstr : "(no message)");
        } else {
            const char* cstr = JS_ToCString(ctx, result);
            printf("    %s: %s\n", error_types[i], cstr ? cstr : "(null)");
        }
    }
    
    // cleanup removed - shared context
    return true;  // Show results even if some fail
}

/* Test 3: Undefined variable (ReferenceError) */
TEST(debug_reference_error) {
    printf("\n  Test 3: ReferenceError\n");
    
    JSContextHandle ctx;
    ctx = get_test_context(); if (!ctx.valid()) return false;
    
    const char* script = "undefined_variable_name;";
    GCValue result = JS_Eval(ctx, script, strlen(script), "test.js", JS_EVAL_TYPE_GLOBAL);
    
    if (!JS_IsException(result)) {
        printf("    FAIL: Expected ReferenceError\n");
        // cleanup removed - shared context
        return false;
    }
    
    GCValue exc = JS_GetException(ctx);
    if (JS_IsUninitialized(exc)) {
        printf("    FAIL: Exception is UNINITIALIZED\n");
        // cleanup removed - shared context
        return false;
    }
    
    GCValue str = JS_ToString(ctx, exc);
    if (JS_IsException(str)) {
        printf("    FAIL: JS_ToString threw exception\n");
        // cleanup removed - shared context
        return false;
    }
    const char* cstr = JS_ToCString(ctx, str);
    printf("    ReferenceError string: %s\n", cstr ? cstr : "(null)");
    
    // cleanup removed - shared context
    return true;
}

/* Test 4: TypeError on null */
TEST(debug_type_error) {
    printf("\n  Test 4: TypeError\n");
    
    JSContextHandle ctx;
    ctx = get_test_context(); if (!ctx.valid()) return false;
    
    const char* script = "null.toString();";
    GCValue result = JS_Eval(ctx, script, strlen(script), "test.js", JS_EVAL_TYPE_GLOBAL);
    
    if (!JS_IsException(result)) {
        printf("    FAIL: Expected TypeError\n");
        // cleanup removed - shared context
        return false;
    }
    
    GCValue exc = JS_GetException(ctx);
    if (JS_IsUninitialized(exc)) {
        printf("    FAIL: Exception is UNINITIALIZED\n");
        // cleanup removed - shared context
        return false;
    }
    
    GCValue str = JS_ToString(ctx, exc);
    if (JS_IsException(str)) {
        printf("    FAIL: JS_ToString threw exception\n");
        // cleanup removed - shared context
        return false;
    }
    const char* cstr = JS_ToCString(ctx, str);
    printf("    TypeError string: %s\n", cstr ? cstr : "(null)");
    
    // cleanup removed - shared context
    return true;
}

/* Test 5: InternalError (out of memory simulation) */
TEST(debug_internal_error) {
    printf("\n  Test 5: InternalError\n");
    
    JSContextHandle ctx;
    ctx = get_test_context(); if (!ctx.valid()) return false;
    
    // Try to trigger an internal error by creating a huge string
    const char* script = "'x'.repeat(1e10);";
    GCValue result = JS_Eval(ctx, script, strlen(script), "test.js", JS_EVAL_TYPE_GLOBAL);
    
    // This may or may not throw depending on memory handling
    if (JS_IsException(result)) {
        GCValue exc = JS_GetException(ctx);
        GCValue str = JS_ToString(ctx, exc);
        const char* cstr = JS_ToCString(ctx, str);
        printf("    Exception string: %s\n", cstr ? cstr : "(null)");
    } else {
        printf("    (No exception - may be expected)\n");
    }
    
    // cleanup removed - shared context
    return true;
}

/* Test 6: Exception in API callback */
TEST(debug_api_exception) {
    printf("\n  Test 6: Exception from API callback\n");
    
    JSContextHandle ctx;
    ctx = get_test_context(); if (!ctx.valid()) return false;
    
    // This should trigger our XMLHttpRequest stubs which may have issues
    const char* script = R"(
        try {
            var xhr = new XMLHttpRequest();
            xhr.open('GET', 'http://example.com');
            xhr.send();
        } catch (e) {
            'caught: ' + e.toString();
        }
    )";
    
    GCValue result = JS_Eval(ctx, script, strlen(script), "test.js", JS_EVAL_TYPE_GLOBAL);
    
    if (JS_IsException(result)) {
        GCValue exc = JS_GetException(ctx);
        GCValue str = JS_ToString(ctx, exc);
        const char* cstr = JS_ToCString(ctx, str);
        printf("    Uncaught exception: %s\n", cstr ? cstr : "(null)");
    } else {
        const char* cstr = JS_ToCString(ctx, result);
        printf("    Result: %s\n", cstr ? cstr : "(null)");
    }
    
    // cleanup removed - shared context
    return true;
}

/* Test 7: Minimal YouTube-like script */
TEST(debug_youtube_like) {
    printf("\n  Test 7: YouTube-like script\n");
    
    JSContextHandle ctx;
    ctx = get_test_context(); if (!ctx.valid()) return false;
    
    // Minimal script that mimics what YouTube does
    const char* script = R"(
        (function() {
            var yt = yt || {};
            yt.player = {
                load: function() {
                    var xhr = new XMLHttpRequest();
                    xhr.onload = function() {
                        console.log('loaded');
                    };
                    return 'ok';
                }
            };
            return yt.player.load();
        })();
    )";
    
    GCValue result = JS_Eval(ctx, script, strlen(script), "test.js", JS_EVAL_TYPE_GLOBAL);
    
    if (JS_IsException(result)) {
        GCValue exc = JS_GetException(ctx);
        if (JS_IsUninitialized(exc)) {
            printf("    FAIL: UNINITIALIZED exception from YouTube-like script\n");
            // cleanup removed - shared context
            return false;
        }
        GCValue str = JS_ToString(ctx, exc);
        const char* cstr = JS_ToCString(ctx, str);
        printf("    Exception: %s\n", cstr ? cstr : "(null)");
    } else {
        const char* cstr = JS_ToCString(ctx, result);
        printf("    Result: %s\n", cstr ? cstr : "(null)");
    }
    
    // cleanup removed - shared context
    return true;
}

/* ============================================================================
 * Test Runner
 * ============================================================================ */

extern "C" void run_exception_debug_tests(void) {
    printf("\n========================================\n");
    printf("Exception Debug Tests\n");
    printf("========================================\n\n");
    
    /* Run all debug tests */
    RUN_TEST(debug_simple_throw_catch);
    RUN_TEST(debug_error_prototypes);
    RUN_TEST(debug_reference_error);
    RUN_TEST(debug_type_error);
    RUN_TEST(debug_internal_error);
    RUN_TEST(debug_api_exception);
    RUN_TEST(debug_youtube_like);
}

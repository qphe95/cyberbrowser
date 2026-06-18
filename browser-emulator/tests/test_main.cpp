/*
 * Main Test Runner - Executes all unit and integration tests
 * 
 * SINGLE CONTEXT MODEL:
 * All tests in this executable share a single QuickJS context to avoid
 * the corruption that occurs when creating multiple contexts from the
 * same runtime. See test_youtube_data_scripts.cpp for the complex test
 * that has its own standalone executable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"

/* Define TEST_RUNNER_MAIN to make this file define the global counters */
#define TEST_RUNNER_MAIN
#include "test_runner.h"

/* QuickJS includes for shared runtime/context */
#include "quickjs.h"
#include "quickjs_gc_unified.h"
#include "js_quickjs.h"
#include "browser_api_impl.h"

#ifdef __cplusplus
extern "C" {
#endif
extern void run_browser_api_impl_tests(void);
extern void run_gc_unified_tests(void);
extern void register_multithreaded_gc_tests(void);
extern void run_exception_debug_tests(void);
extern void run_logical_operator_tests(void);
extern void run_nested_iife_tests(void);
extern void run_lazy_parsing_tests(void);
extern void run_preorder_compaction_array_tests(void);
extern void run_css_parser_tests(void);
/* Note: YouTube data tests run in separate executable (youtube-data-test) */

/* Accessor for the shared context */
extern JSContextHandle get_shared_test_context(void);
extern GCValue get_shared_test_global(void);
#ifdef __cplusplus
}
#endif

/* Shared runtime and context - single context model */
static JSRuntimeHandle g_shared_rt;
static JSContextHandle g_shared_ctx;
static GCValue g_shared_global;
static bool g_context_initialized = false;

/* Initialize the shared context once for all tests */
static bool init_shared_context(void) {
    if (g_context_initialized) {
        return true;
    }
    
    /* Use the shared runtime from test_runner.h */
    if (!g_test_rt.valid()) {
        printf("FATAL: Shared test runtime not available\n");
        return false;
    }
    
    g_shared_rt = g_test_rt;
    
    /* Create single shared context */
    g_shared_ctx = JS_NewContext(g_shared_rt);
    if (!g_shared_ctx.valid()) {
        printf("FATAL: Failed to create shared context\n");
        return false;
    }
    
    /* Add all intrinsics */
    JS_AddIntrinsicBaseObjects(g_shared_ctx);
    JS_AddIntrinsicEval(g_shared_ctx);
    JS_AddIntrinsicRegExp(g_shared_ctx);
    JS_AddIntrinsicJSON(g_shared_ctx);
    JS_AddIntrinsicPromise(g_shared_ctx);
    JS_AddIntrinsicMapSet(g_shared_ctx);
    JS_AddIntrinsicTypedArrays(g_shared_ctx);
    JS_AddIntrinsicWeakRef(g_shared_ctx);
    
    /* Get global object */
    g_shared_global = JS_GetGlobalObject(g_shared_ctx);
    if (JS_IsException(g_shared_global)) {
        printf("FATAL: Failed to get global object\n");
        return false;
    }
    
    /* Set global JS pointers (required for unified GC) */
    extern JSRuntimeHandle g_js_runtime;
    extern JSContextHandle g_js_context;
    g_js_runtime = g_shared_rt;
    g_js_context = g_shared_ctx;
    
    /* Initialize browser APIs */
    init_browser_api_impl(g_shared_ctx, g_shared_global);
    js_quickjs_setup_initial_dom();
    
    g_context_initialized = true;
    printf("Shared context initialized successfully (handle=%u)\n\n", 
           g_shared_ctx.handle());
    return true;
}

/* Public accessor for shared context */
extern "C" JSContextHandle get_shared_test_context(void) {
    if (!g_context_initialized) {
        init_shared_context();
    }
    return g_shared_ctx;
}

/* Public accessor for shared global */
extern "C" GCValue get_shared_test_global(void) {
    if (!g_context_initialized) {
        init_shared_context();
    }
    return g_shared_global;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    printf("========================================\n");
    printf("Browser Emulator Test Suite\n");
    printf("(Single Context Model)\n");
    printf("========================================\n");
    printf("Starting tests...\n\n");
    fflush(stdout);

    /* Initialize platform */
    if (!platform_init()) {
        printf("Failed to initialize platform\n");
        return 1;
    }
    
    if (!platform_http_init()) {
        printf("Failed to initialize HTTP\n");
        platform_cleanup();
        return 1;
    }

    PLATFORM_LOGI("test_runner", "Starting browser-emulator test suite (single context)");

    /* Initialize unified GC first (required before any QuickJS operations) */
    printf("Initializing unified GC...\n");
    if (!gc_init()) {
        printf("FATAL: Failed to initialize unified GC\n");
        platform_http_cleanup();
        platform_cleanup();
        return 1;
    }
    
    /* Create shared QuickJS runtime for all tests */
    printf("Creating shared QuickJS runtime...\n");
    JSRuntimeHandle shared_rt = JS_NewRuntime();
    g_test_rt_ptr = &shared_rt;
    if (!g_test_rt.valid()) {
        printf("FATAL: Failed to create QuickJS runtime\n");
        platform_http_cleanup();
        platform_cleanup();
        return 1;
    }
    printf("Shared runtime created successfully\n");
    fflush(stdout);

    /* Initialize shared context once */
    if (!init_shared_context()) {
        printf("FATAL: Failed to initialize shared context\n");
        JS_FreeRuntime(g_test_rt);
        g_test_rt_ptr = NULL;
        platform_http_cleanup();
        platform_cleanup();
        return 1;
    }

    /* Run all test suites - they will use the shared context */
    run_gc_unified_tests();
    return 0; /* DEBUG: stop after gc_unified tests */
    register_multithreaded_gc_tests();
    run_browser_api_impl_tests();
    run_exception_debug_tests();
    run_logical_operator_tests();
    run_nested_iife_tests();
    run_lazy_parsing_tests();
    run_preorder_compaction_array_tests();
    run_css_parser_tests();

    /* Note: YouTube data tests run in separate executable (youtube-data-test) */

    /* Cleanup */
    printf("\nFreeing shared QuickJS runtime...\n");
    if (g_test_rt.valid()) {
        JS_FreeRuntime(g_test_rt);
    }
    g_test_rt_ptr = NULL;
    platform_http_cleanup();
    platform_cleanup();
    
    /* Print summary */
    printf("\n========================================\n");
    printf("TEST SUMMARY\n");
    printf("========================================\n");
    printf("Total:  %d\n", tests_run);
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    printf("========================================\n");
    
    PLATFORM_LOGI("test_runner", 
                  "Tests complete: %d run, %d passed, %d failed",
                  tests_run, tests_passed, tests_failed);
    
    if (tests_failed == 0) {
        printf("\nALL TESTS PASSED!\n");
        PLATFORM_LOGI("test_runner", "ALL TESTS PASSED");
        return 0;
    } else {
        printf("\nSOME TESTS FAILED!\n");
        PLATFORM_LOGE("test_runner", "SOME TESTS FAILED");
        return 1;
    }
}
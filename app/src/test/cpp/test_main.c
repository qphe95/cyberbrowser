/*
 * Main Test Runner - Executes all unit and integration tests
 *
 * Build: ./gradlew externalNativeBuildDebug
 * Run on device: adb shell /data/local/tmp/bgmdwnldr_tests
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>
#include "quickjs_gc_unified.h"

#define LOG_TAG "bgmdwnldr_tests"

/* Define TEST_RUNNER_MAIN to make this file define the global counters */
#define TEST_RUNNER_MAIN
#include "test_runner.h"

/* Test suite declarations */
extern void run_browser_api_impl_tests(void);

int main(int argc, char *argv[]) {
    printf("========================================\n");
    printf("BGMDWLDR Test Suite\n");
    printf("========================================\n");
    printf("Starting tests...\n\n");

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Starting BGMDWLDR test suite");

    /* Run all test suites */
    run_browser_api_impl_tests();

    /* Cleanup GC */
    gc_cleanup();
    
    /* Print summary */
    printf("\n========================================\n");
    printf("TEST SUMMARY\n");
    printf("========================================\n");
    printf("Total:  %d\n", tests_run);
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    printf("========================================\n");
    
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, 
                        "Tests complete: %d run, %d passed, %d failed",
                        tests_run, tests_passed, tests_failed);
    
    if (tests_failed == 0) {
        printf("\nALL TESTS PASSED!\n");
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "ALL TESTS PASSED");
        return 0;
    } else {
        printf("\nSOME TESTS FAILED!\n");
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "SOME TESTS FAILED");
        return 1;
    }
}

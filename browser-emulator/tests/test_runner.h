/*
 * Test Runner - Simple C test framework
 */
#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

/* Global test counters - defined in one compilation unit, extern in others */
#ifdef TEST_RUNNER_MAIN
int tests_run = 0;
int tests_passed = 0;
int tests_failed = 0;
#else
extern int tests_run;
extern int tests_passed;
extern int tests_failed;
#endif

/* Forward declaration for QuickJS runtime handle (C++ class) */
/* The actual type is defined in quickjs_types.h */
class JSRuntimeHandle;

/* Global shared runtime - tests should use this instead of creating their own */
#ifdef TEST_RUNNER_MAIN
JSRuntimeHandle *g_test_rt_ptr = NULL;
#else
extern JSRuntimeHandle *g_test_rt_ptr;
#endif

/* Convenience macro to access the shared runtime */
#define g_test_rt (*g_test_rt_ptr)

#define TEST(name) \
    static bool name(void); \
    static void name##_runner(void) { \
        tests_run++; \
        if (name()) { \
            tests_passed++; \
            printf("  ✓ %s\n", #name); \
        } else { \
            tests_failed++; \
            printf("  ✗ %s\n", #name); \
        } \
    } \
    static bool name(void)

#define ASSERT_TRUE(cond) \
    if (!(cond)) { \
        printf("    ASSERTION FAILED: %s\n", #cond); \
        printf("    at %s:%d\n", __FILE__, __LINE__); \
        return false; \
    }

#define ASSERT_FALSE(cond) \
    if (cond) { \
        printf("    ASSERTION FAILED: %s\n", #cond); \
        printf("    at %s:%d\n", __FILE__, __LINE__); \
        return false; \
    }

#define ASSERT_EQ(expected, actual) \
    if ((expected) != (actual)) { \
        printf("    ASSERTION FAILED: %s == %s\n", #expected, #actual); \
        printf("    Expected: %lld, Got: %lld\n", (long long)(expected), (long long)(actual)); \
        printf("    at %s:%d\n", __FILE__, __LINE__); \
        return false; \
    }

#define ASSERT_STR_EQ(expected, actual) \
    if (strcmp((expected), (actual)) != 0) { \
        printf("    ASSERTION FAILED: %s == %s\n", #expected, #actual); \
        printf("    Expected: \"%s\", Got: \"%s\"\n", (expected), (actual)); \
        printf("    at %s:%d\n", __FILE__, __LINE__); \
        return false; \
    }

#define RUN_TEST(name) name##_runner()

#define PRINT_TEST_SUMMARY() \
    printf("\n========================================\n"); \
    printf("TEST SUMMARY\n"); \
    printf("========================================\n"); \
    printf("Total:  %d\n", tests_run); \
    printf("Passed: %d\n", tests_passed); \
    printf("Failed: %d\n", tests_failed); \
    printf("========================================\n"); \
    if (tests_failed == 0) { \
        printf("ALL TESTS PASSED!\n"); \
    } else { \
        printf("SOME TESTS FAILED!\n"); \
    }

#endif /* TEST_RUNNER_H */

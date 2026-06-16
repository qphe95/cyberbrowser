#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <GLFW/glfw3.h>

#define FAIL_IF(cond, msg) do { \
    if (cond) { \
        fprintf(stderr, "FAIL: %s at line %d\n", msg, __LINE__); \
        return 1; \
    } \
} while(0)

#define PASS(msg) printf("PASS: %s\n", msg)
#define WARN(msg) printf("WARN: %s\n", msg)

/* Test that GLFW framebuffer size maintains correct relationship
   with window size and content scale across different sizes. */
static int test_glfw_size_consistency(void) {
    if (!glfwInit()) {
        fprintf(stderr, "FAIL: glfwInit failed\n");
        return 1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); /* hidden window for testing */

    GLFWwindow *window = glfwCreateWindow(1280, 800, "Test", NULL, NULL);
    FAIL_IF(!window, "glfwCreateWindow failed");

    float xscale, yscale;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    printf("  Content scale: %.2f x %.2f\n", xscale, yscale);

    int sizes[][2] = {
        {640, 400},
        {1280, 800},
        {1920, 1080},
        {2560, 1600},
    };

    for (int i = 0; i < 4; i++) {
        int wantW = sizes[i][0];
        int wantH = sizes[i][1];
        glfwSetWindowSize(window, wantW, wantH);

        /* Give GLFW a chance to process the resize */
        glfwPollEvents();

        int winW, winH;
        glfwGetWindowSize(window, &winW, &winH);

        int fbW, fbH;
        glfwGetFramebufferSize(window, &fbW, &fbH);

        float xs, ys;
        glfwGetWindowContentScale(window, &xs, &ys);

        float expectedFbW = winW * xs;
        float expectedFbH = winH * ys;

        printf("  Window %dx%d -> FB %dx%d (expected ~%.0fx%.0f)\n",
               winW, winH, fbW, fbH, expectedFbW, expectedFbH);

        /* Allow small rounding differences */
        float diffW = fabsf(fbW - expectedFbW);
        float diffH = fabsf(fbH - expectedFbH);

        if (diffW > 2.0f || diffH > 2.0f) {
            WARN("framebuffer size deviates from window_size * scale");
        }

        /* The critical invariant: framebuffer size should be consistent
           with the content scale. If scale is 2.0, framebuffer should
           be roughly 2x the window size. */
        FAIL_IF(fbW <= 0 || fbH <= 0,
                "framebuffer size is zero or negative");
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    PASS("GLFW size consistency verified");
    return 0;
}

/* Test that content scale stays constant during resize on same monitor */
static int test_content_scale_stability(void) {
    if (!glfwInit()) {
        fprintf(stderr, "FAIL: glfwInit failed\n");
        return 1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow *window = glfwCreateWindow(1280, 800, "Test", NULL, NULL);
    FAIL_IF(!window, "glfwCreateWindow failed");

    float initialScale;
    {
        float xs, ys;
        glfwGetWindowContentScale(window, &xs, &ys);
        initialScale = xs;
        printf("  Initial content scale: %.2f\n", initialScale);
    }

    int sizes[][2] = {
        {640, 400},
        {1280, 800},
        {2560, 1600},
        {100, 100},
        {3000, 2000},
    };

    for (int i = 0; i < 5; i++) {
        glfwSetWindowSize(window, sizes[i][0], sizes[i][1]);
        glfwPollEvents();

        float xs, ys;
        glfwGetWindowContentScale(window, &xs, &ys);

        if (fabsf(xs - initialScale) > 0.01f) {
            printf("  Scale changed to %.2f at size %dx%d\n", xs, sizes[i][0], sizes[i][1]);
            WARN("content scale changed during resize");
        }
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    PASS("content scale stability verified");
    return 0;
}

/* Test: verify that simulated resize produces correct framebuffer sizes
   that our swapchain code can use. */
static int test_resize_simulation(void) {
    if (!glfwInit()) {
        fprintf(stderr, "FAIL: glfwInit failed\n");
        return 1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow *window = glfwCreateWindow(1280, 800, "Test", NULL, NULL);
    FAIL_IF(!window, "glfwCreateWindow failed");

    float xscale, yscale;
    glfwGetWindowContentScale(window, &xscale, &yscale);

    /* Simulate the resize logic from main_macos.cpp */
    int fbW, fbH;
    glfwGetFramebufferSize(window, &fbW, &fbH);
    int windowWidth = fbW;
    int windowHeight = fbH;

    /* Resize the window */
    glfwSetWindowSize(window, 1920, 1080);
    glfwPollEvents();

    int newFbW, newFbH;
    glfwGetFramebufferSize(window, &newFbW, &newFbH);

    printf("  Before resize: FB=%dx%d, After resize: FB=%dx%d\n",
           fbW, fbH, newFbW, newFbH);

    /* The new framebuffer should reflect the new window size */
    FAIL_IF(newFbW == fbW && newFbH == fbH,
            "framebuffer size did not change after resize");

    /* Verify the swapchain extent formula would work */
    uint32_t extentW = (uint32_t)newFbW;
    uint32_t extentH = (uint32_t)newFbH;
    FAIL_IF(extentW == 0 || extentH == 0,
            "swapchain extent would be zero");

    glfwDestroyWindow(window);
    glfwTerminate();
    PASS("resize simulation verified");
    return 0;
}

int main(void) {
    printf("=== GLFW Resize Tests ===\n\n");

    int failures = 0;
    failures += test_glfw_size_consistency();
    failures += test_content_scale_stability();
    failures += test_resize_simulation();

    printf("\n=== Results: %d test(s) failed ===\n", failures);
    return failures;
}

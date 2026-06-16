#define VK_USE_PLATFORM_METAL_EXT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include "vulkan_ui.h"

/* Stub platform functions */
extern "C" void platform_show_keyboard(VulkanApp *app, bool clearText) { (void)app; (void)clearText; }
extern "C" void platform_hide_keyboard(VulkanApp *app) { (void)app; }
extern "C" void platform_set_clipboard(const char *text) { (void)text; }
extern "C" const char *platform_get_clipboard(void) { return NULL; }
extern "C" ShaderBlob platform_load_shader(const char *path) {
    ShaderBlob blob = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return blob;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    blob.data = (uint8_t *)malloc(sz);
    if (blob.data) {
        fread(blob.data, 1, sz, f);
        blob.size = (size_t)sz;
    }
    fclose(f);
    return blob;
}

static bool init_instance(VulkanApp *app) {
    uint32_t glfwExtensionCount = 0;
    const char **glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if (!glfwExtensions) return false;

    const char *extensions[16];
    uint32_t extensionCount = 0;
    for (uint32_t i = 0; i < glfwExtensionCount; i++) {
        extensions[extensionCount++] = glfwExtensions[i];
    }
    extensions[extensionCount++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;

    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Test",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "test",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_0
    };
    VkInstanceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledExtensionCount = extensionCount,
        .ppEnabledExtensionNames = extensions,
        .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
    };
    return vkCreateInstance(&createInfo, NULL, &app->instance) == VK_SUCCESS;
}

static bool render_and_capture(VulkanApp *app, uint8_t **pixels, uint32_t *w, uint32_t *h) {
    for (int i = 0; i < 3; i++) {
        draw_frame(app);
    }
    vkDeviceWaitIdle(app->device);
    return capture_swapchain_image(app, pixels, w, h);
}

static float compute_rms_diff(const uint8_t *a, const uint8_t *b,
                               uint32_t w, uint32_t h, uint32_t strideA, uint32_t strideB) {
    double sum = 0.0;
    uint32_t count = 0;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            size_t offA = (size_t)y * strideA + x * 4;
            size_t offB = (size_t)y * strideB + x * 4;
            for (int c = 0; c < 4; c++) {
                int diff = (int)a[offA + c] - (int)b[offB + c];
                sum += diff * diff;
                count++;
            }
        }
    }
    return (float)sqrt(sum / count);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (!glfwInit()) {
        fprintf(stderr, "Failed to init GLFW\n");
        return 1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    GLFWwindow *window = glfwCreateWindow(1280, 800, "TestTextRender", NULL, NULL);
    if (!window) {
        fprintf(stderr, "Failed to create window\n");
        glfwTerminate();
        return 1;
    }

    VulkanApp vk = {};
    g_app = &vk;
    pthread_mutex_init(&vk.uiMutex, NULL);

    float xs, ys;
    glfwGetWindowContentScale(window, &xs, &ys);
    vk.densityScale = xs;
    if (vk.densityScale < 1.0f) vk.densityScale = 1.0f;

    int fbW, fbH;
    glfwGetFramebufferSize(window, &fbW, &fbH);
    vk.windowWidth = fbW;
    vk.windowHeight = fbH;

    if (!init_instance(&vk)) {
        fprintf(stderr, "init_instance failed\n");
        return 1;
    }
    if (glfwCreateWindowSurface(vk.instance, window, NULL, &vk.surface) != VK_SUCCESS) {
        fprintf(stderr, "glfwCreateWindowSurface failed\n");
        return 1;
    }

    ShaderBlob vert = platform_load_shader("app/src/main/assets/triangle.vert.spv");
    ShaderBlob frag = platform_load_shader("app/src/main/assets/triangle.frag.spv");
    if (!vert.data || !frag.data) {
        free_shader(&vert); free_shader(&frag);
        vert = platform_load_shader("triangle.vert.spv");
        frag = platform_load_shader("triangle.frag.spv");
    }
    if (!vert.data || !frag.data) {
        fprintf(stderr, "Failed to load shaders\n");
        return 1;
    }

    if (!vulkan_ui_init(&vk, vk.instance, vk.surface,
                        vert.data, vert.size,
                        frag.data, frag.size)) {
        fprintf(stderr, "vulkan_ui_init failed\n");
        return 1;
    }
    free_shader(&vert);
    free_shader(&frag);

    /* Enable test mode: render a single line of text at fixed position */
    g_testText = "HELLO WORLD";

    printf("=== Text-Line Render Invariance Test ===\n");
    printf("Initial window: %dx%d (framebuffer %dx%d) scale=%.2f\n",
           1280, 800, fbW, fbH, xs);

    /* Capture at initial size */
    uint8_t *pixelsA = NULL;
    uint32_t wA = 0, hA = 0;
    if (!render_and_capture(&vk, &pixelsA, &wA, &hA)) {
        fprintf(stderr, "Failed to capture frame A\n");
        return 1;
    }
    printf("Captured frame A: %dx%d\n", wA, hA);

    /* Resize window */
    glfwSetWindowSize(window, 2560, 1600);
    for (int i = 0; i < 10; i++) {
        glfwPollEvents();
    }
    glfwGetFramebufferSize(window, &fbW, &fbH);
    printf("Resized window: 2560x1600 (framebuffer %dx%d)\n", fbW, fbH);

    if (fbW > 0 && fbH > 0) {
        vk.windowWidth = fbW;
        vk.windowHeight = fbH;
        recreate_swapchain(&vk);
    }

    /* Capture at new size */
    uint8_t *pixelsB = NULL;
    uint32_t wB = 0, hB = 0;
    if (!render_and_capture(&vk, &pixelsB, &wB, &hB)) {
        fprintf(stderr, "Failed to capture frame B\n");
        return 1;
    }
    printf("Captured frame B: %dx%d\n", wB, hB);

    /* The overlapping region should be pixel-identical */
    uint32_t overlapW = wA < wB ? wA : wB;
    uint32_t overlapH = hA < hB ? hA : hB;

    float rms = compute_rms_diff(pixelsA, pixelsB, overlapW, overlapH,
                                  wA * 4, wB * 4);

    printf("\nOverlap region: %dx%d\n", overlapW, overlapH);
    printf("RMS pixel difference across overlap: %.4f\n", rms);

    /* Save both images as PPM for visual inspection */
    FILE *f = fopen("/tmp/frame_text_a.ppm", "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", wA, hA);
        for (uint32_t y = 0; y < hA; y++) {
            for (uint32_t x = 0; x < wA; x++) {
                size_t off = ((size_t)(hA - 1 - y) * wA + x) * 4;
                fputc(pixelsA[off + 2], f);
                fputc(pixelsA[off + 1], f);
                fputc(pixelsA[off + 0], f);
            }
        }
        fclose(f);
        printf("Saved /tmp/frame_text_a.ppm\n");
    }
    f = fopen("/tmp/frame_text_b.ppm", "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", wB, hB);
        for (uint32_t y = 0; y < hB; y++) {
            for (uint32_t x = 0; x < wB; x++) {
                size_t off = ((size_t)(hB - 1 - y) * wB + x) * 4;
                fputc(pixelsB[off + 2], f);
                fputc(pixelsB[off + 1], f);
                fputc(pixelsB[off + 0], f);
            }
        }
        fclose(f);
        printf("Saved /tmp/frame_text_b.ppm\n");
    }

    int pass = (rms < 1.0f) ? 1 : 0;
    printf("\n%s: %s\n",
           pass ? "PASS" : "FAIL",
           pass ? "Text pixels are identical across resize (size invariant)"
                : "Text pixels differ across resize (scaling detected)");

    free(pixelsA);
    free(pixelsB);

    vkDeviceWaitIdle(vk.device);
    cleanup_device(&vk);
    glfwDestroyWindow(window);
    glfwTerminate();
    return pass ? 0 : 1;
}

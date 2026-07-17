/*
 * flamegraph_viewer.cpp - Vulkan window that displays the flame graph PNG
 * produced by CYBER_PROFILE=1 runs of cyberbrowser.
 *
 * The window/instance/surface scaffolding is lifted from the old
 * ft_vulkan_ui vulkan_main.cpp; rendering goes through the shared
 * VulkanRenderer (vulkan_renderer.cpp) with the flamegraph shaders
 * (shaders/flamegraph.vert / .frag, embedded via embedded_shaders.h).
 *
 * Usage:
 *   flamegraph_viewer.exe [path-to-png]     (default: flamegraph.png)
 *
 * ESC or window close quits.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winsock2.h>
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include "vulkan_renderer.h"
#include "stb_image.h"
#include "../shaders/embedded_shaders.h"

typedef struct {
    float pos[2];
    float uv[2];
    float color[3];
} FgVertex;

static int g_running = 1;
static VulkanRenderer *g_renderer = NULL;

static LRESULT CALLBACK fg_window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KEYDOWN:
            if ((int)wParam == VK_ESCAPE) {
                g_running = 0;
                return 0;
            }
            break;
        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            if (g_renderer && width > 0 && height > 0) {
                g_renderer->windowWidth = width;
                g_renderer->windowHeight = height;
                vk_renderer_recreate_swapchain(g_renderer);
            }
            return 0;
        }
        case WM_CLOSE:
            g_running = 0;
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int main(int argc, char *argv[]) {
    const char *png_path = (argc > 1) ? argv[1] : "flamegraph.png";

    /* Load the flame graph image. */
    int img_w = 0, img_h = 0, img_channels = 0;
    unsigned char *pixels = stbi_load(png_path, &img_w, &img_h, &img_channels, 4);
    if (!pixels) {
        fprintf(stderr, "FATAL: cannot load %s (%s)\n", png_path, stbi_failure_reason());
        return 1;
    }
    printf("Loaded %s: %dx%d\n", png_path, img_w, img_h);

    /* Win32 window, sized to the image aspect ratio. */
    HINSTANCE hInstance = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = fg_window_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"CyberFlamegraphWindowClass";
    if (!RegisterClassExW(&wc)) {
        fprintf(stderr, "FATAL: RegisterClassExW failed\n");
        return 1;
    }

    int win_w = 1600;
    int win_h = (int)((double)win_w * (double)img_h / (double)img_w) + 40;
    if (win_h > 1000) {
        win_h = 1000;
        win_w = (int)((double)(win_h - 40) * (double)img_w / (double)img_h);
    }

    HWND hwnd = CreateWindowExW(
        0,
        L"CyberFlamegraphWindowClass",
        L"CyberBrowser Flame Graph",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        win_w, win_h,
        NULL, NULL, hInstance, NULL);
    if (!hwnd) {
        fprintf(stderr, "FATAL: CreateWindowExW failed\n");
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    /* Vulkan instance + window surface (from the ft_vulkan_ui scaffold). */
    VkInstance instance = VK_NULL_HANDLE;
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "CyberFlamegraph";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "NoEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    const char *extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = 2;
    createInfo.ppEnabledExtensionNames = extensions;

    if (vkCreateInstance(&createInfo, NULL, &instance) != VK_SUCCESS) {
        fprintf(stderr, "FATAL: vkCreateInstance failed\n");
        DestroyWindow(hwnd);
        return 1;
    }

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkWin32SurfaceCreateInfoKHR surfaceCreateInfo = {};
    surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceCreateInfo.hinstance = hInstance;
    surfaceCreateInfo.hwnd = hwnd;
    if (vkCreateWin32SurfaceKHR(instance, &surfaceCreateInfo, NULL, &surface) != VK_SUCCESS) {
        fprintf(stderr, "FATAL: vkCreateWin32SurfaceKHR failed\n");
        vkDestroyInstance(instance, NULL);
        DestroyWindow(hwnd);
        return 1;
    }

    VulkanRenderer renderer = {};
    g_renderer = &renderer;
    if (!vk_renderer_init(&renderer, instance, surface,
                          flamegraph_vert_spv, flamegraph_vert_spv_len,
                          flamegraph_frag_spv, flamegraph_frag_spv_len)) {
        fprintf(stderr, "FATAL: vk_renderer_init failed\n");
        /* Renderer took no ownership yet: destroy surface + instance here. */
        vkDestroySurfaceKHR(instance, surface, NULL);
        vkDestroyInstance(instance, NULL);
        DestroyWindow(hwnd);
        return 1;
    }

    if (!vk_renderer_upload_texture_rgba(&renderer, pixels, img_w, img_h)) {
        fprintf(stderr, "FATAL: texture upload failed\n");
        vk_renderer_cleanup(&renderer);
        DestroyWindow(hwnd);
        return 1;
    }
    stbi_image_free(pixels);

    /* Full-window textured quad (two triangles, NDC positions, UV 0..1). */
    static const FgVertex quad[6] = {
        { {-1.0f, -1.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f} },
        { { 1.0f, -1.0f}, {1.0f, 0.0f}, {1.0f, 1.0f, 1.0f} },
        { { 1.0f,  1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f} },
        { {-1.0f, -1.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f} },
        { { 1.0f,  1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f} },
        { {-1.0f,  1.0f}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f} },
    };

    printf("Flame graph viewer ready. ESC or close window to quit.\n");

    /* Headless verification hook: after a few frames, request a framebuffer
     * capture and exit once it has been written.  The renderer writes the
     * BMP (cyberbrowser_capture.bmp) during the draw AFTER the request. */
    const char *capture_path = getenv("CYBER_FG_CAPTURE");
    int frames = 0;
    int capture_requested = 0;

    while (g_running) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = 0;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;

        if (!vk_renderer_draw(&renderer, quad, 6)) break;
        frames++;
        if (capture_path && !capture_requested && frames >= 3) {
            vk_renderer_capture_framebuffer(&renderer, capture_path);
            capture_requested = 1;
        }
        if (capture_requested && frames >= 4) {
            printf("Framebuffer captured to cyberbrowser_capture.bmp\n");
            break;
        }
        Sleep(16);
    }

    vkDeviceWaitIdle(renderer.device);
    /* vk_renderer_cleanup destroys the device, surface, and instance. */
    vk_renderer_cleanup(&renderer);
    DestroyWindow(hwnd);
    return 0;
}

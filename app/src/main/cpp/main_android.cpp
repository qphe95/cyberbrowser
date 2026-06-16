#define VK_USE_PLATFORM_ANDROID_KHR

#include <android/log.h>
#include <android_native_app_glue.h>
#include <android/input.h>
#include <android/configuration.h>
#include <android/native_activity.h>
#include <jni.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include "vulkan_ui.h"
#include "js_quickjs.h"

#define LOG_TAG "minimalvulkan"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static ShaderBlob read_asset(AAssetManager *mgr, const char *path) {
    ShaderBlob blob = {0};
    if (!mgr) {
        LOGE("Cannot read asset %s: asset manager is NULL", path);
        return blob;
    }
    AAsset *asset = AAssetManager_open(mgr, path, AASSET_MODE_STREAMING);
    if (!asset) {
        LOGE("Failed to open asset: %s", path);
        return blob;
    }
    off_t length = AAsset_getLength(asset);
    if (length <= 0) {
        LOGE("Asset length invalid: %s", path);
        AAsset_close(asset);
        return blob;
    }
    uint8_t *buffer = (uint8_t *)malloc((size_t)length);
    if (!buffer) {
        LOGE("Out of memory reading asset: %s", path);
        AAsset_close(asset);
        return blob;
    }
    int64_t readBytes = AAsset_read(asset, buffer, (size_t)length);
    AAsset_close(asset);
    if (readBytes != length) {
        LOGE("Short read for asset: %s", path);
        free(buffer);
        return blob;
    }
    blob.data = buffer;
    blob.size = (size_t)length;
    return blob;
}

static void update_density_scale(struct android_app *app, VulkanApp *vk) {
    if (!app || !app->config) {
        vk->densityScale = 1.0f;
        return;
    }
    int32_t density = AConfiguration_getDensity(app->config);
    if (density <= 0) {
        density = ACONFIGURATION_DENSITY_DEFAULT;
    }
    int32_t baseDensity = ACONFIGURATION_DENSITY_DEFAULT;
    if (baseDensity <= 0) {
        baseDensity = ACONFIGURATION_DENSITY_MEDIUM;
    }
    if (baseDensity <= 0) {
        baseDensity = 160;
    }
    vk->densityScale = (float)density / (float)baseDensity;
}

static void java_show_keyboard(VulkanApp *app, bool clearText) {
    (void)clearText;
    if (!app) return;
    JNIEnv *env = NULL;
    JavaVM *vm = app->javaVm ? app->javaVm : app->androidApp->activity->vm;
    if (!vm) return;
    vm->AttachCurrentThread(&env, NULL);
    if (!env) return;
    jclass cls = env->GetObjectClass(app->androidApp->activity->clazz);
    jmethodID showMethod = env->GetMethodID(cls, "showKeyboard", "()V");
    if (showMethod) {
        env->CallVoidMethod(app->androidApp->activity->clazz, showMethod);
    }
}

static void java_hide_keyboard(VulkanApp *app) {
    if (!app) return;
    JNIEnv *env = NULL;
    JavaVM *vm = app->javaVm ? app->javaVm : app->androidApp->activity->vm;
    if (!vm) return;
    vm->AttachCurrentThread(&env, NULL);
    if (!env) return;
    jclass cls = env->GetObjectClass(app->androidApp->activity->clazz);
    jmethodID hideMethod = env->GetMethodID(cls, "hideKeyboard", "()V");
    if (hideMethod) {
        env->CallVoidMethod(app->androidApp->activity->clazz, hideMethod);
    }
}

/* ============================================================================
 * Platform callbacks (implemented here for Android)
 * ============================================================================ */

void platform_show_keyboard(VulkanApp *app, bool clearText) {
    java_show_keyboard(app, clearText);
}

void platform_hide_keyboard(VulkanApp *app) {
    java_hide_keyboard(app);
}

ShaderBlob platform_load_shader(const char *path) {
    if (!g_app || !g_app->assetManager) {
        ShaderBlob empty = {0};
        return empty;
    }
    return read_asset(g_app->assetManager, path);
}

/* ============================================================================
 * Vulkan instance & surface creation (Android-specific)
 * ============================================================================ */

static bool init_instance(VulkanApp *app) {
    if (app->instance != VK_NULL_HANDLE) {
        return true;
    }
    const char *extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
    };
    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "MinimalVulkan",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "minimal",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_0
    };
    VkInstanceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledExtensionCount = (uint32_t)(sizeof(extensions) / sizeof(extensions[0])),
        .ppEnabledExtensionNames = extensions
    };
    VkResult result = vkCreateInstance(&createInfo, NULL, &app->instance);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateInstance failed: %d", result);
        return false;
    }
    return true;
}

static bool create_surface(VulkanApp *app) {
    if (app->surface != VK_NULL_HANDLE) {
        return true;
    }
    VkAndroidSurfaceCreateInfoKHR surfaceInfo = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = app->window
    };
    VkResult result = vkCreateAndroidSurfaceKHR(app->instance, &surfaceInfo, NULL, &app->surface);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateAndroidSurfaceKHR failed: %d", result);
        return false;
    }
    return true;
}

/* ============================================================================
 * Android lifecycle & input
 * ============================================================================ */

static void handle_cmd(struct android_app *app, int32_t cmd) {
    VulkanApp *vk = (VulkanApp *)app->userData;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            vk->window = app->window;
            if (app->activity && vk->assetManager == NULL) {
                vk->assetManager = app->activity->assetManager;
                js_quickjs_set_asset_manager(vk->assetManager);
            }
            if (vk->window) {
                update_density_scale(app, vk);
                if (!init_instance(vk)) {
                    LOGE("init_instance failed");
                    break;
                }
                if (!create_surface(vk)) {
                    LOGE("create_surface failed");
                    break;
                }
                ShaderBlob vert = read_asset(vk->assetManager, "shaders/triangle.vert.spv");
                ShaderBlob frag = read_asset(vk->assetManager, "shaders/triangle.frag.spv");
                if (!vert.data || !frag.data) {
                    LOGE("Failed to load shaders");
                    free_shader(&vert);
                    free_shader(&frag);
                    break;
                }
                if (!vulkan_ui_init(vk, vk->instance, vk->surface,
                                    vert.data, vert.size,
                                    frag.data, frag.size)) {
                    LOGE("vulkan_ui_init failed");
                }
                free_shader(&vert);
                free_shader(&frag);
            }
            break;
        case APP_CMD_CONFIG_CHANGED:
            update_density_scale(app, vk);
            if (vk->ready) {
                update_text_vertices(vk);
            }
            break;
        case APP_CMD_TERM_WINDOW:
            vk->window = NULL;
            vk->ready = false;
            if (vk->device != VK_NULL_HANDLE) {
                vkDeviceWaitIdle(vk->device);
            }
            cleanup_swapchain(vk);
            if (vk->surface != VK_NULL_HANDLE && vk->instance != VK_NULL_HANDLE) {
                vkDestroySurfaceKHR(vk->instance, vk->surface, NULL);
                vk->surface = VK_NULL_HANDLE;
            }
            break;
        default:
            break;
    }
}

static int32_t handle_input(struct android_app *app, AInputEvent *event) {
    (void)app;
    (void)event;
    return 0;
}

void android_main(struct android_app *app) {
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;
    VulkanApp vk = {0};
    g_app = &vk;
    vk.androidApp = app;
    vk.assetManager = NULL;
    vk.javaVm = NULL;
    pthread_mutex_init(&vk.uiMutex, NULL);
    vk.urlInput[0] = '\0';
    vk.urlLen = 0;
    vk.progress = 0.0f;
    vk.workerRunning = false;
    vk.inputActive = false;
    vk.keyboardHeightPx = 0.0f;
    vk.uiScrollOffsetY = 0.0f;
    vk.uiScrollNeedsFocus = false;
    snprintf(vk.statusText, sizeof(vk.statusText), "Idle");
    app->userData = &vk;

    while (true) {
        int events;
        struct android_poll_source *source;
        while (ALooper_pollAll(0, NULL, &events, (void **)&source) >= 0) {
            if (source) {
                source->process(app, source);
            }
            if (app->destroyRequested) {
                pthread_mutex_destroy(&vk.uiMutex);
                g_app = NULL;
                cleanup_device(&vk);
                return;
            }
        }
        if (vk.ready) {
            draw_frame(&vk);
        }
    }
}

/* ============================================================================
 * JNI callbacks
 * ============================================================================ */

extern "C" {

JNIEXPORT void JNICALL
Java_com_bgmdwldr_vulkan_MainActivity_nativeOnTextChanged(JNIEnv *env, jclass clazz,
                                                          jstring text) {
    (void)env;
    (void)clazz;
    (void)text;
}

JNIEXPORT void JNICALL
Java_com_bgmdwldr_vulkan_MainActivity_nativeOnSubmit(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
}

JNIEXPORT void JNICALL
Java_com_bgmdwldr_vulkan_MainActivity_nativeOnFocus(JNIEnv *env, jclass clazz,
                                                    jboolean focused) {
    (void)env;
    (void)clazz;
    (void)focused;
}

JNIEXPORT void JNICALL
Java_com_bgmdwldr_vulkan_MainActivity_nativeOnKeyboardHeight(JNIEnv *env, jclass clazz,
                                                             jint heightPx) {
    (void)env;
    (void)clazz;
    if (!g_app) return;
    g_app->keyboardHeightPx = (float)heightPx;
    if (g_input.keyboardVisible && g_input.focus != FOCUS_NONE) {
        g_app->uiScrollNeedsFocus = true;
        ui_scroll_to_focus(g_input.focus);
    }
}

#define ACTION_DOWN     0
#define ACTION_UP       1
#define ACTION_MOVE     2
#define ACTION_CANCEL   3
#define KEY_ACTION_DOWN 0
#define KEY_ACTION_UP   1
#define KEYCODE_ENTER   66
#define KEYCODE_DEL     67
#define KEYCODE_ESCAPE  111
#define KEYCODE_SPACE   62
#define KEYCODE_DPAD_LEFT  21
#define KEYCODE_DPAD_RIGHT 22

JNIEXPORT void JNICALL
Java_com_bgmdwldr_vulkan_MainActivity_nativeOnTouch(JNIEnv *env, jclass clazz,
                                                    jint action, jfloat x, jfloat y) {
    (void)env;
    (void)clazz;
    if (!g_app) {
        LOGI("nativeOnTouch: g_app is NULL!");
        return;
    }
    float scaledX = x;
    float scaledY = y;
    if (g_app->windowWidth > 0 && g_app->windowHeight > 0) {
        float swapW = (float)g_app->swapchainExtent.width;
        float swapH = (float)g_app->swapchainExtent.height;
        float winW = (float)g_app->windowWidth;
        float winH = (float)g_app->windowHeight;
        scaledX = x * (swapW / winW);
        scaledY = y * (swapH / winH);
    }
    update_input_bounds();

    static float touchStartX = 0.0f, touchStartY = 0.0f, lastTouchY = 0.0f;
    static bool touchMoved = false;
    static bool touchPanning = false;
    const float TAP_SLOP = 16.0f;

    switch (action) {
        case ACTION_DOWN:
            touchStartX = scaledX;
            touchStartY = scaledY;
            lastTouchY = scaledY;
            touchMoved = false;
            touchPanning = false;
            break;
        case ACTION_MOVE:
            if (!touchPanning) {
                float dx = scaledX - touchStartX;
                float dy = scaledY - touchStartY;
                if (dx * dx + dy * dy > TAP_SLOP * TAP_SLOP) {
                    touchMoved = true;
                    touchPanning = true;
                }
            }
            if (touchPanning) {
                float deltaY = scaledY - lastTouchY;
                /* Panning moves content opposite to finger (drag) */
                ui_apply_scroll_delta(deltaY);
                lastTouchY = scaledY;
            }
            break;
        case ACTION_UP:
        case ACTION_CANCEL:
            if (!touchPanning) {
                /* Tap: check controls */
                if (is_inside_filename_box(scaledX, scaledY)) {
                    g_input.focus = FOCUS_FILENAME;
                    g_input.filenameActive = true;
                    g_input.inputActive = false;
                    g_app->inputActive = false;
                    g_input.filenameField.cursorPos = tf_char_pos_from_mouse(
                        &g_input.filenameField, scaledX, scaledY,
                        g_app->uiFilenameTextX, g_app->uiFilenameTextY,
                        g_app->uiFilenameGlyphW, g_app->uiFilenameGlyphH,
                        g_app->uiFilenameCharsPerLine);
                    g_input.filenameField.selStart = -1;
                    g_input.filenameField.selEnd = -1;
                    show_soft_keyboard();
                    ui_set_status(g_app, "Enter filename...");
                    g_app->uiScrollNeedsFocus = true;
                    ui_scroll_to_focus(FOCUS_FILENAME);
                } else if (is_inside_url_box(scaledX, scaledY)) {
                    g_input.focus = FOCUS_URL;
                    g_input.inputActive = true;
                    g_app->inputActive = true;
                    g_input.filenameActive = false;
                    g_input.urlField.cursorPos = tf_char_pos_from_mouse(
                        &g_input.urlField, scaledX, scaledY,
                        g_app->uiUrlTextX, g_app->uiUrlTextY,
                        g_app->uiUrlGlyphW, g_app->uiUrlGlyphH,
                        g_app->uiUrlCharsPerLine);
                    g_input.urlField.selStart = -1;
                    g_input.urlField.selEnd = -1;
                    show_soft_keyboard();
                    ui_set_status(g_app, "Enter URL...");
                    g_app->uiScrollNeedsFocus = true;
                    ui_scroll_to_focus(FOCUS_URL);
                } else if (is_inside_keep_video_checkbox(scaledX, scaledY)) {
                    g_app->keepVideo = !g_app->keepVideo;
                    enforce_media_extension(&g_input.filenameField);
                    sync_input_to_app();
                } else {
                    /* Tap outside any control: clear focus */
                    if (g_input.focus != FOCUS_NONE) {
                        g_input.focus = FOCUS_NONE;
                        g_input.inputActive = false;
                        g_app->inputActive = false;
                        g_input.filenameActive = false;
                        hide_soft_keyboard();
                        ui_set_status(g_app, "Idle");
                        g_app->uiScrollOffsetY = 0.0f;
                    }
                }
            }
            touchPanning = false;
            touchMoved = false;
            break;
    }
}

JNIEXPORT void JNICALL
Java_com_bgmdwldr_vulkan_MainActivity_nativeOnKey(JNIEnv *env, jclass clazz,
                                                  jint keyCode, jint unicodeChar, jint action) {
    (void)env;
    (void)clazz;
    if (action != KEY_ACTION_DOWN) {
        return;
    }
    /* If no field is focused, a printable key starts URL editing; Enter submits if URL exists */
    if (g_input.focus == FOCUS_NONE) {
        if (unicodeChar >= 32 && unicodeChar < 127) {
            g_input.focus = FOCUS_URL;
            g_input.inputActive = true;
            if (g_app) {
                g_app->inputActive = true;
                g_app->uiScrollNeedsFocus = true;
            }
            show_soft_keyboard();
            ui_set_status(g_app, "Enter URL...");
        } else if (keyCode == KEYCODE_ENTER && g_app && g_app->urlInput[0] != '\0') {
            handle_submit();
            return;
        } else {
            return;
        }
    }

    switch (keyCode) {
        case KEYCODE_ENTER:
            handle_submit();
            return;
        case KEYCODE_ESCAPE:
            g_input.focus = FOCUS_NONE;
            g_input.inputActive = false;
            g_input.filenameActive = false;
            if (g_app) {
                g_app->inputActive = false;
                g_app->uiScrollOffsetY = 0.0f;
            }
            hide_soft_keyboard();
            ui_set_status(g_app, "Idle");
            return;
    }

    /* Route editing commands to the focused field */
    if (g_input.focus == FOCUS_URL) {
        switch (keyCode) {
            case KEYCODE_DEL:
                handle_backspace();
                break;
            case KEYCODE_DPAD_LEFT:
                handle_cursor_left();
                break;
            case KEYCODE_DPAD_RIGHT:
                handle_cursor_right();
                break;
            default:
                if (unicodeChar > 0 && unicodeChar < 128) {
                    handle_character_input((char)unicodeChar);
                }
                break;
        }
    } else if (g_input.focus == FOCUS_FILENAME) {
        switch (keyCode) {
            case KEYCODE_DEL:
                handle_filename_backspace();
                break;
            case KEYCODE_DPAD_LEFT:
                handle_filename_cursor_left();
                break;
            case KEYCODE_DPAD_RIGHT:
                handle_filename_cursor_right();
                break;
            default:
                if (unicodeChar > 0 && unicodeChar < 128) {
                    handle_filename_character((char)unicodeChar);
                }
                break;
        }
    }

    sync_input_to_app();
}

JNIEXPORT void JNICALL
Java_com_bgmdwldr_vulkan_MainActivity_nativeOnKeyboardShown(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    g_input.keyboardVisible = true;
    LOGI("Keyboard shown notification");
    if (g_app && g_input.focus != FOCUS_NONE) {
        g_app->uiScrollNeedsFocus = true;
        ui_scroll_to_focus(g_input.focus);
    }
}

JNIEXPORT void JNICALL
Java_com_bgmdwldr_vulkan_MainActivity_nativeOnKeyboardHidden(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    g_input.keyboardVisible = false;
    g_input.inputActive = false;
    g_input.filenameActive = false;
    if (g_app) {
        g_app->inputActive = false;
        g_app->uiScrollOffsetY = 0.0f;
    }
    LOGI("Keyboard hidden notification");
}

JNIEXPORT void JNICALL
Java_com_bgmdwldr_vulkan_MainActivity_nativeOnPaste(JNIEnv *env, jclass clazz, jstring text) {
    (void)clazz;
    if (!g_app || !text) return;
    const char *utf = env->GetStringUTFChars(text, NULL);
    if (!utf) return;
    size_t len = strlen(utf);
    LOGI("Paste received: %zu chars", len);
    file_log("Paste received: %zu chars", len);

    /* Default to URL field if nothing is focused */
    if (g_input.focus == FOCUS_NONE) {
        g_input.focus = FOCUS_URL;
        g_input.inputActive = true;
        g_app->inputActive = true;
        show_soft_keyboard();
        ui_set_status(g_app, "Enter URL...");
    }

    if (g_app) g_app->uiScrollNeedsFocus = true;

    if (g_input.focus == FOCUS_URL) {
        handle_paste(utf);
    } else if (g_input.focus == FOCUS_FILENAME) {
        handle_filename_paste(utf);
    }

    sync_input_to_app();
    ui_scroll_to_focus(g_input.focus);
    env->ReleaseStringUTFChars(text, utf);
}

} /* extern "C" */

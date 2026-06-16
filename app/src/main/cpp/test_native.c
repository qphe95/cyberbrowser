// Standalone native test for URL analysis
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>
#include "url_analyzer.h"
#include "js_quickjs.h"

#define LOG_TAG "native_test"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Test function that can be called from JNI
JNIEXPORT void JNICALL
Java_com_bgmdwldr_vulkan_MainActivity_nativeTestUrl(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    
    const char *test_url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
    LOGI("=== Native URL Test Starting ===");
    LOGI("Testing URL: %s", test_url);
    
    MediaUrl media;
    char err[512] = {0};
    
    LOGI("Calling url_analyze...");
    bool result = url_analyze(test_url, &media, err, sizeof(err));
    
    if (result) {
        LOGI("SUCCESS! Media URL: %s", media.url);
        if (media.mime[0]) {
            LOGI("MIME type: %s", media.mime);
        }
    } else {
        LOGE("FAILED: %s", err);
    }
    
    LOGI("=== Native URL Test Complete ===");
}

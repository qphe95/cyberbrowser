LOCAL_PATH := $(call my-dir)
BROWSER_EMULATOR_PATH := $(LOCAL_PATH)/../../../../browser-emulator

# Browser Emulator Module
include $(CLEAR_VARS)
LOCAL_MODULE := browser-emulator
LOCAL_CPP_EXTENSION := .cpp

# QuickJS sources
LOCAL_SRC_FILES := \
    $(BROWSER_EMULATOR_PATH)/third_party/quickjs/quickjs.cpp \
    $(BROWSER_EMULATOR_PATH)/third_party/quickjs/libregexp.c \
    $(BROWSER_EMULATOR_PATH)/third_party/quickjs/libunicode.c \
    $(BROWSER_EMULATOR_PATH)/third_party/quickjs/cutils.c \
    $(BROWSER_EMULATOR_PATH)/third_party/quickjs/dtoa.c \
    $(BROWSER_EMULATOR_PATH)/third_party/quickjs/quickjs_gc_unified.cpp \
    $(BROWSER_EMULATOR_PATH)/third_party/quickjs/js_atom_cache.c \
    $(BROWSER_EMULATOR_PATH)/third_party/quickjs/js_fast_dispatch.c

# Browser emulator sources
LOCAL_SRC_FILES += \
    $(BROWSER_EMULATOR_PATH)/src/html_media_extract.cpp \
    $(BROWSER_EMULATOR_PATH)/src/js_quickjs.cpp \
    $(BROWSER_EMULATOR_PATH)/src/browser_api_impl.cpp \
    $(BROWSER_EMULATOR_PATH)/src/html_dom.cpp \
    $(BROWSER_EMULATOR_PATH)/src/preorder_compaction_array.cpp \
    $(BROWSER_EMULATOR_PATH)/src/css_parser.cpp \
    $(BROWSER_EMULATOR_PATH)/src/url_analyzer.c \
    $(BROWSER_EMULATOR_PATH)/src/tls_client.c \
    $(BROWSER_EMULATOR_PATH)/src/http_download.c \
    $(BROWSER_EMULATOR_PATH)/src/jobs.c \
    $(BROWSER_EMULATOR_PATH)/src/audio_extract.c \
    $(BROWSER_EMULATOR_PATH)/src/platform/platform.c \
    $(BROWSER_EMULATOR_PATH)/src/platform/platform_android.c

# mbedtls sources
MBEDTLS_PATH := $(BROWSER_EMULATOR_PATH)/third_party/mbedtls
MBEDTLS_SRC := $(wildcard $(MBEDTLS_PATH)/library/*.c)
MBEDTLS_SRC := $(filter-out %mbedtls_config.c,$(MBEDTLS_SRC))
TF_PSA_PATH := $(MBEDTLS_PATH)/tf-psa-crypto
TF_PSA_CORE := $(wildcard $(TF_PSA_PATH)/core/*.c)
TF_PSA_DRIVERS := $(wildcard $(TF_PSA_PATH)/drivers/*.c)
TF_PSA_BUILTIN := $(wildcard $(TF_PSA_PATH)/drivers/builtin/*.c)
TF_PSA_BUILTIN_SRC := $(wildcard $(TF_PSA_PATH)/drivers/builtin/src/*.c)

LOCAL_SRC_FILES += $(MBEDTLS_SRC) $(TF_PSA_CORE) $(TF_PSA_DRIVERS) $(TF_PSA_BUILTIN) $(TF_PSA_BUILTIN_SRC)

# Include paths
LOCAL_C_INCLUDES := \
    $(BROWSER_EMULATOR_PATH)/include \
    $(BROWSER_EMULATOR_PATH)/src \
    $(BROWSER_EMULATOR_PATH)/third_party/quickjs \
    $(MBEDTLS_PATH)/include \
    $(MBEDTLS_PATH) \
    $(TF_PSA_PATH)/include \
    $(TF_PSA_PATH) \
    $(TF_PSA_PATH)/drivers/builtin/include \
    $(TF_PSA_PATH)/drivers/builtin/src \
    $(TF_PSA_PATH)/core

# Compiler flags
LOCAL_CFLAGS := -DCONFIG_VERSION=\"2024-02-14\" -O2 -g -DBE_PLATFORM_ANDROID

# Libraries
LOCAL_LDLIBS := -landroid -llog -lvulkan -lmediandk -lm

include $(BUILD_STATIC_LIBRARY)

# Main Application
include $(CLEAR_VARS)
LOCAL_MODULE := minimalvulkan

LOCAL_SRC_FILES := \
    vulkan_ui.cpp \
    ui_layout.cpp \
    mp4_metadata.cpp \
    default_album_art.c \
    main_android.cpp \
    media_store.cpp

LOCAL_C_INCLUDES := \
    $(BROWSER_EMULATOR_PATH)/include \
    $(BROWSER_EMULATOR_PATH)/src \
    $(BROWSER_EMULATOR_PATH)/third_party/quickjs

LOCAL_CFLAGS := -O2 -g -DBE_PLATFORM_ANDROID
LOCAL_LDFLAGS := 
LOCAL_LDLIBS := -landroid -llog -lvulkan -lmediandk -lm
LOCAL_STATIC_LIBRARIES := browser-emulator android_native_app_glue

include $(BUILD_SHARED_LIBRARY)

$(call import-module,android/native_app_glue)

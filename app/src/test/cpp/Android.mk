# Test executable for BGMDWLDR native tests
LOCAL_PATH := $(call my-dir)
BROWSER_EMULATOR_PATH := $(LOCAL_PATH)/../../../../browser-emulator

include $(CLEAR_VARS)
LOCAL_MODULE := bgmdwnldr_tests

# QuickJS sources
LOCAL_SRC_FILES := \
    $(BROWSER_EMULATOR_PATH)/third_party/quickjs/quickjs.c \
    $(BROWSER_EMULATOR_PATH)/third_party/quickjs/libregexp.c \
    $(BROWSER_EMULATOR_PATH)/third_party/quickjs/libunicode.c \
    $(BROWSER_EMULATOR_PATH)/third_party/quickjs/cutils.c \
    $(BROWSER_EMULATOR_PATH)/third_party/quickjs/dtoa.c \
    $(BROWSER_EMULATOR_PATH)/third_party/quickjs/quickjs_gc_unified.c

# Browser emulator sources
LOCAL_SRC_FILES += \
    $(BROWSER_EMULATOR_PATH)/src/html_media_extract.c \
    $(BROWSER_EMULATOR_PATH)/src/js_quickjs.c \
    $(BROWSER_EMULATOR_PATH)/src/browser_api_impl.c \
    $(BROWSER_EMULATOR_PATH)/src/html_dom.c \
    $(BROWSER_EMULATOR_PATH)/src/url_analyzer.c \
    $(BROWSER_EMULATOR_PATH)/src/tls_client.c \
    $(BROWSER_EMULATOR_PATH)/src/http_download.c \
    $(BROWSER_EMULATOR_PATH)/src/platform/platform.c \
    $(BROWSER_EMULATOR_PATH)/src/platform/platform_android.c

# mbedtls sources
MBEDTLS_PATH := $(LOCAL_PATH)/../../main/cpp/third_party/mbedtls
MBEDTLS_SRC := $(wildcard $(MBEDTLS_PATH)/library/*.c)
MBEDTLS_SRC := $(filter-out $(MBEDTLS_PATH)/library/mbedtls_config.c,$(MBEDTLS_SRC))
MBEDTLS_SRC := $(patsubst $(LOCAL_PATH)/%,%,$(MBEDTLS_SRC))
TF_PSA_PATH := $(MBEDTLS_PATH)/tf-psa-crypto
TF_PSA_CORE := $(wildcard $(TF_PSA_PATH)/core/*.c)
TF_PSA_CORE := $(patsubst $(LOCAL_PATH)/%,%,$(TF_PSA_CORE))
TF_PSA_DRIVERS := $(wildcard $(TF_PSA_PATH)/drivers/*.c)
TF_PSA_DRIVERS := $(patsubst $(LOCAL_PATH)/%,%,$(TF_PSA_DRIVERS))
TF_PSA_BUILTIN := $(wildcard $(TF_PSA_PATH)/drivers/builtin/*.c)
TF_PSA_BUILTIN := $(patsubst $(LOCAL_PATH)/%,%,$(TF_PSA_BUILTIN))
TF_PSA_BUILTIN_SRC := $(wildcard $(TF_PSA_PATH)/drivers/builtin/src/*.c)
TF_PSA_BUILTIN_SRC := $(patsubst $(LOCAL_PATH)/%,%,$(TF_PSA_BUILTIN_SRC))

LOCAL_SRC_FILES += $(MBEDTLS_SRC) $(TF_PSA_CORE) $(TF_PSA_DRIVERS) $(TF_PSA_BUILTIN) $(TF_PSA_BUILTIN_SRC)

# Test sources
LOCAL_SRC_FILES += \
    test_main.c \
    test_browser_api_impl.c

# Include paths
LOCAL_C_INCLUDES := \
    $(LOCAL_PATH) \
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
LOCAL_CFLAGS := -DCONFIG_VERSION=\"2024-02-14\" -O0 -g -DTEST_BUILD -DBE_PLATFORM_ANDROID

# Libraries
LOCAL_LDLIBS := -landroid -llog -lm

# Build as executable
include $(BUILD_EXECUTABLE)

# Makefile for browser-emulator on macOS
# Usage: make
# For tests: make run

CC = clang
CXX = clang++
CFLAGS = -Wall -Wextra -O2 -g -DBE_PLATFORM_MACOS \
    -DCONFIG_VERSION=\"2024-02-14\" \
    -I./include \
    -I./src \
    -I./third_party/quickjs \
    -I./third_party/mbedtls/include \
    -I./third_party/mbedtls \
    -I./third_party/mbedtls/tf-psa-crypto/include \
    -I./third_party/mbedtls/tf-psa-crypto \
    -I./third_party/mbedtls/tf-psa-crypto/drivers/builtin/include \
    -I./third_party/mbedtls/tf-psa-crypto/drivers/builtin/src \
    -I./third_party/mbedtls/tf-psa-crypto/core

CXXFLAGS = $(CFLAGS) -std=c++17

LDFLAGS = -L/opt/homebrew/opt/curl/lib -lcurl -lpthread -lm -lstdc++
CFLAGS += -I/opt/homebrew/opt/curl/include

# Platform abstraction sources (using curl for HTTP)
PLATFORM_SRC = src/platform/platform.c src/platform/platform_posix.c src/platform/http_curl.c

# Core browser-emulator sources (C and C++)
CORE_SRC_C = src/url_analyzer.c src/http_download.c src/tls_client.c
CORE_SRC_CPP = src/html_media_extract.cpp src/js_quickjs.cpp src/browser_api_impl.cpp \
    src/html_dom.cpp

# QuickJS sources (C and C++)
QUICKJS_SRC_C = third_party/quickjs/libregexp.c \
    third_party/quickjs/libunicode.c third_party/quickjs/cutils.c \
    third_party/quickjs/dtoa.c
QUICKJS_SRC_CPP = third_party/quickjs/quickjs.cpp third_party/quickjs/quickjs_gc_unified.cpp

# mbedtls directories
MBEDTLS_SRC_DIR = third_party/mbedtls
TF_PSA_DIR = $(MBEDTLS_SRC_DIR)/tf-psa-crypto

# mbedtls library sources
MBEDTLS_SRC = \
    $(MBEDTLS_SRC_DIR)/library/ssl_tls.c \
    $(MBEDTLS_SRC_DIR)/library/ssl_tls12_client.c \
    $(MBEDTLS_SRC_DIR)/library/ssl_tls12_server.c \
    $(MBEDTLS_SRC_DIR)/library/ssl_tls13_client.c \
    $(MBEDTLS_SRC_DIR)/library/ssl_tls13_generic.c \
    $(MBEDTLS_SRC_DIR)/library/ssl_tls13_keys.c \
    $(MBEDTLS_SRC_DIR)/library/ssl_tls13_server.c \
    $(MBEDTLS_SRC_DIR)/library/ssl_client.c \
    $(MBEDTLS_SRC_DIR)/library/ssl_debug_helpers_generated.c \
    $(MBEDTLS_SRC_DIR)/library/net_sockets.c \
    $(MBEDTLS_SRC_DIR)/library/debug.c \
    $(MBEDTLS_SRC_DIR)/library/ssl_msg.c \
    $(MBEDTLS_SRC_DIR)/library/ssl_ciphersuites.c \
    $(MBEDTLS_SRC_DIR)/library/ssl_cache.c \
    $(MBEDTLS_SRC_DIR)/library/ssl_cookie.c \
    $(MBEDTLS_SRC_DIR)/library/ssl_ticket.c \
    $(MBEDTLS_SRC_DIR)/library/x509.c \
    $(MBEDTLS_SRC_DIR)/library/x509_crt.c \
    $(MBEDTLS_SRC_DIR)/library/x509_csr.c \
    $(MBEDTLS_SRC_DIR)/library/x509_oid.c \
    $(MBEDTLS_SRC_DIR)/library/pk.c \
    $(MBEDTLS_SRC_DIR)/library/pk_wrap.c \
    $(MBEDTLS_SRC_DIR)/library/pkparse.c \
    $(MBEDTLS_SRC_DIR)/library/rsa.c \
    $(MBEDTLS_SRC_DIR)/library/ecp.c \
    $(MBEDTLS_SRC_DIR)/library/ecp_curves.c \
    $(MBEDTLS_SRC_DIR)/library/ecdh.c \
    $(MBEDTLS_SRC_DIR)/library/ecdsa.c \
    $(MBEDTLS_SRC_DIR)/library/cipher.c \
    $(MBEDTLS_SRC_DIR)/library/cipher_wrap.c \
    $(MBEDTLS_SRC_DIR)/library/aes.c \
    $(MBEDTLS_SRC_DIR)/library/sha256.c \
    $(MBEDTLS_SRC_DIR)/library/sha512.c \
    $(MBEDTLS_SRC_DIR)/library/md.c \
    $(MBEDTLS_SRC_DIR)/library/md5.c \
    $(MBEDTLS_SRC_DIR)/library/entropy.c \
    $(MBEDTLS_SRC_DIR)/library/entropy_poll.c \
    $(MBEDTLS_SRC_DIR)/library/ctr_drbg.c \
    $(MBEDTLS_SRC_DIR)/library/hmac_drbg.c \
    $(MBEDTLS_SRC_DIR)/library/gcm.c \
    $(MBEDTLS_SRC_DIR)/library/ccm.c \
    $(MBEDTLS_SRC_DIR)/library/bignum.c \
    $(MBEDTLS_SRC_DIR)/library/bignum_core.c \
    $(MBEDTLS_SRC_DIR)/library/bignum_mod.c \
    $(MBEDTLS_SRC_DIR)/library/oid.c \
    $(MBEDTLS_SRC_DIR)/library/asn1parse.c \
    $(MBEDTLS_SRC_DIR)/library/asn1write.c \
    $(MBEDTLS_SRC_DIR)/library/platform.c \
    $(MBEDTLS_SRC_DIR)/library/platform_util.c \
    $(MBEDTLS_SRC_DIR)/library/memory_buffer_alloc.c \
    $(MBEDTLS_SRC_DIR)/library/timing.c \
    $(MBEDTLS_SRC_DIR)/library/version.c \
    $(MBEDTLS_SRC_DIR)/library/error.c \
    $(MBEDTLS_SRC_DIR)/library/base64.c \
    $(MBEDTLS_SRC_DIR)/library/pem.c \
    $(MBEDTLS_SRC_DIR)/library/constant_time.c \
    $(MBEDTLS_SRC_DIR)/library/chacha20.c \
    $(MBEDTLS_SRC_DIR)/library/chachapoly.c \
    $(MBEDTLS_SRC_DIR)/library/poly1305.c \
    $(MBEDTLS_SRC_DIR)/library/sha1.c \
    $(MBEDTLS_SRC_DIR)/library/nist_kw.c \
    $(MBEDTLS_SRC_DIR)/library/cmac.c \
    $(TF_PSA_DIR)/core/psa_crypto.c \
    $(TF_PSA_DIR)/core/psa_crypto_slot_management.c \
    $(TF_PSA_DIR)/core/psa_crypto_client.c \
    $(TF_PSA_DIR)/core/psa_its_file.c \
    $(TF_PSA_DIR)/core/psa_crypto_storage.c \
    $(TF_PSA_DIR)/core/psa_crypto_driver_wrappers_no_static.c \
    $(TF_PSA_DIR)/core/tf_psa_crypto_config.c \
    $(TF_PSA_DIR)/core/tf_psa_crypto_version.c \
    $(wildcard $(TF_PSA_DIR)/drivers/builtin/src/*.c)

# Test sources
TEST_SRC_C =
TEST_SRC_CPP = tests/test_main.cpp tests/test_browser_api_impl.cpp tests/test_gc_unified.cpp tests/test_logical_operators.cpp tests/test_exception_debug.cpp tests/test_nested_iife.cpp

# All C sources
ALL_SRC_C = $(PLATFORM_SRC) $(CORE_SRC_C) $(QUICKJS_SRC_C) $(MBEDTLS_SRC)

# All C++ sources
ALL_SRC_CPP = $(CORE_SRC_CPP) $(QUICKJS_SRC_CPP)

# Build directory
BUILD_DIR = build
OBJDIR = $(BUILD_DIR)/obj

# Object files
OBJS_C = $(patsubst %.c,$(OBJDIR)/%.o,$(notdir $(ALL_SRC_C)))
OBJS_CPP = $(patsubst %.cpp,$(OBJDIR)/%.o,$(notdir $(ALL_SRC_CPP)))
TEST_OBJS_C = $(patsubst %.c,$(OBJDIR)/%.o,$(notdir $(TEST_SRC_C)))
TEST_OBJS_CPP = $(patsubst %.cpp,$(OBJDIR)/%.o,$(notdir $(TEST_SRC_CPP)))
TEST_OBJS = $(TEST_OBJS_C) $(TEST_OBJS_CPP)
OBJS = $(OBJS_C) $(OBJS_CPP)

# VPATH for finding source files
VPATH = src:src/platform:third_party/quickjs:tests
VPATH += :$(MBEDTLS_SRC_DIR)/library
VPATH += :$(TF_PSA_DIR)/core
VPATH += :$(TF_PSA_DIR)/drivers/builtin/src

# Target executable
TARGET = $(BUILD_DIR)/browser-emulator-tests

.PHONY: all clean dirs run

all: dirs $(TARGET)

dirs:
	@mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: %.c
	@echo "Compiling $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.cpp
	@echo "Compiling $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(OBJS) $(TEST_OBJS)
	@echo "Linking $@"
	@$(CXX) -o $@ $^ $(LDFLAGS)
	@echo "Build complete: $@"

clean:
	rm -rf $(BUILD_DIR)

run: all
	@echo "Running tests..."
	@./$(TARGET)

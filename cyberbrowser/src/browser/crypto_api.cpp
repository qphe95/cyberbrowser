/* Auto-generated split from browser_api_impl.cpp */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_types.h"
#include "browser_api_impl_handles.h"
#include "browser_api_impl_internal.h"
#include "html_dom.h"
#include "css_parser.h"
#include "gc_value_helpers.h"
#include "platform.h"
// mbedtls includes for Crypto API
#include "mbedtls/md.h"

// Define macro to access private GCM functions
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS 1
#include "mbedtls/private/gcm.h"

/* ============================================================================
 * Crypto API Implementation
 * ============================================================================ */

// crypto.getRandomValues(typedArray)
GCValue js_crypto_get_random_values(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "getRandomValues requires 1 argument");
    
    GCValue typed_array = argv[0];
    if (!JS_IsObject(typed_array)) return JS_ThrowTypeError(ctx, "getRandomValues argument must be a TypedArray");
    
    // Get the ArrayBuffer info from the TypedArray
    size_t byte_offset = 0;
    size_t byte_length = 0;
    size_t bytes_per_element = 0;
    GCValue buffer = JS_GetTypedArrayBuffer(ctx, typed_array, &byte_offset, &byte_length, &bytes_per_element);
    if (JS_IsException(buffer)) return JS_EXCEPTION;
    
    size_t buf_size;
    uint8_t *data = JS_GetArrayBuffer(ctx, &buf_size, buffer);
    if (!data) return JS_ThrowTypeError(ctx, "getRandomValues: failed to get ArrayBuffer");
    
    // Fill with random bytes using arc4random on macOS/Linux
    #if defined(__APPLE__) || defined(__linux__)
        arc4random_buf(data + byte_offset, byte_length);
    #else
        // Fallback to rand() - not cryptographically secure but sufficient for emulation
        for (size_t i = 0; i < byte_length; i++) {
            data[byte_offset + i] = (uint8_t)(rand() % 256);
        }
    #endif
    
    return typed_array;
}

// SubtleCrypto.digest(algorithm, data)
GCValue js_subtle_digest(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "digest requires 2 arguments");
    
    // Get algorithm name
    const char *algo = NULL;
    if (JS_IsString(argv[0])) {
        algo = JS_ToCString(ctx, argv[0]);
    } else if (JS_IsObject(argv[0])) {
        GCValue name = JS_GetPropertyStr(ctx, argv[0], "name");
        algo = JS_ToCString(ctx, name);
    }
    if (!algo) return JS_ThrowTypeError(ctx, "digest: invalid algorithm");
    
    // Get data ArrayBuffer
    size_t data_len;
    uint8_t *data = JS_GetArrayBuffer(ctx, &data_len, argv[1]);
    if (!data) return JS_ThrowTypeError(ctx, "digest: data must be an ArrayBuffer");
    
    // Map algorithm to mbedtls digest type
    mbedtls_md_type_t md_type;
    size_t hash_len;
    
    if (strcmp(algo, "SHA-1") == 0) {
        md_type = MBEDTLS_MD_SHA1;
        hash_len = 20;
    } else if (strcmp(algo, "SHA-256") == 0) {
        md_type = MBEDTLS_MD_SHA256;
        hash_len = 32;
    } else if (strcmp(algo, "SHA-384") == 0) {
        md_type = MBEDTLS_MD_SHA384;
        hash_len = 48;
    } else if (strcmp(algo, "SHA-512") == 0) {
        md_type = MBEDTLS_MD_SHA512;
        hash_len = 64;
    } else {
        return JS_ThrowTypeError(ctx, "digest: unsupported algorithm '%s'", algo);
    }
    
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(md_type);
    if (!md_info) return JS_ThrowInternalError(ctx, "digest: failed to get md_info");
    
    uint8_t hash[64];  // Max hash size
    mbedtls_md(md_info, data, data_len, hash);
    
    return JS_NewArrayBufferCopy(ctx, hash, hash_len);
}

// Helper to extract raw key bytes from a CryptoKey object
uint8_t* get_key_bytes(JSContextHandle ctx, GCValue key_obj, size_t *key_len) {
    // Try to get the key data from a "__keyData" hidden property
    GCValue key_data = JS_GetPropertyStr(ctx, key_obj, "__keyData");
    if (JS_IsUndefined(key_data) || JS_IsNull(key_data)) {
        return NULL;
    }
    
    // Get the ArrayBuffer from the key data
    size_t len;
    uint8_t *data = JS_GetArrayBuffer(ctx, &len, key_data);
    if (!data) return NULL;
    
    *key_len = len;
    return data;
}

// Helper to get algorithm IV/nonce
uint8_t* get_iv(JSContextHandle ctx, GCValue algo_obj, size_t *iv_len) {
    GCValue iv_val = JS_GetPropertyStr(ctx, algo_obj, "iv");
    if (JS_IsUndefined(iv_val) || JS_IsNull(iv_val)) {
        // Try "nonce" as alternative
        iv_val = JS_GetPropertyStr(ctx, algo_obj, "nonce");
        if (JS_IsUndefined(iv_val) || JS_IsNull(iv_val)) {
            return NULL;
        }
    }
    
    size_t len;
    uint8_t *data = JS_GetArrayBuffer(ctx, &len, iv_val);
    if (!data) return NULL;
    
    *iv_len = len;
    return data;
}

// SubtleCrypto.encrypt(algorithm, key, data) - AES-GCM support
GCValue js_subtle_encrypt(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 3) return JS_ThrowTypeError(ctx, "encrypt requires 3 arguments");
    
    // Get algorithm name
    const char *algo_name = NULL;
    if (JS_IsString(argv[0])) {
        algo_name = JS_ToCString(ctx, argv[0]);
    } else if (JS_IsObject(argv[0])) {
        GCValue name = JS_GetPropertyStr(ctx, argv[0], "name");
        algo_name = JS_ToCString(ctx, name);
    }
    if (!algo_name) return JS_ThrowTypeError(ctx, "encrypt: invalid algorithm");
    
    // Get key bytes
    size_t key_len;
    uint8_t *key_bytes = get_key_bytes(ctx, argv[1], &key_len);
    if (!key_bytes) return JS_ThrowTypeError(ctx, "encrypt: invalid key");
    
    // Get IV/nonce
    size_t iv_len;
    uint8_t *iv = get_iv(ctx, argv[0], &iv_len);
    if (!iv) return JS_ThrowTypeError(ctx, "encrypt: missing IV/nonce");
    
    // Get plaintext data
    size_t plaintext_len;
    uint8_t *plaintext = JS_GetArrayBuffer(ctx, &plaintext_len, argv[2]);
    if (!plaintext) return JS_ThrowTypeError(ctx, "encrypt: data must be an ArrayBuffer");
    
    // For AES-GCM
    if (strcmp(algo_name, "AES-GCM") == 0) {
        // Validate key size
        if (key_len != 16 && key_len != 24 && key_len != 32) {
            return JS_ThrowTypeError(ctx, "encrypt: invalid key size for AES");
        }
        if (iv_len != 12) {
            return JS_ThrowTypeError(ctx, "encrypt: IV must be 12 bytes for GCM");
        }
        
        // Output buffer: ciphertext + 16-byte tag
        size_t output_len = plaintext_len + 16;
        uint8_t *output = (uint8_t*)malloc(output_len);
        if (!output) return JS_ThrowOutOfMemory(ctx);
        
        // Use mbedtls GCM context
        mbedtls_gcm_context gcm;
        mbedtls_gcm_init(&gcm);
        
        int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key_bytes, key_len * 8);
        if (ret != 0) {
            mbedtls_gcm_free(&gcm);
            free(output);
            return JS_ThrowInternalError(ctx, "encrypt: failed to set GCM key");
        }
        
        // Encrypt and generate tag
        ret = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plaintext_len,
                                         iv, iv_len, NULL, 0,
                                         plaintext, output,
                                         16, output + plaintext_len);
        
        mbedtls_gcm_free(&gcm);
        
        if (ret != 0) {
            free(output);
            return JS_ThrowInternalError(ctx, "encrypt: GCM encryption failed");
        }
        
        GCValue result = JS_NewArrayBufferCopy(ctx, output, output_len);
        free(output);
        return result;
    }
    
    // Unsupported algorithm
    LOG_INFO("Crypto", "subtle.encrypt: unsupported algorithm %s", algo_name);
    return JS_ThrowTypeError(ctx, "encrypt: unsupported algorithm '%s'", algo_name);
}

// SubtleCrypto.decrypt(algorithm, key, data) - AES-GCM support  
GCValue js_subtle_decrypt(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 3) return JS_ThrowTypeError(ctx, "decrypt requires 3 arguments");
    
    // Get algorithm name
    const char *algo_name = NULL;
    if (JS_IsString(argv[0])) {
        algo_name = JS_ToCString(ctx, argv[0]);
    } else if (JS_IsObject(argv[0])) {
        GCValue name = JS_GetPropertyStr(ctx, argv[0], "name");
        algo_name = JS_ToCString(ctx, name);
    }
    if (!algo_name) return JS_ThrowTypeError(ctx, "decrypt: invalid algorithm");
    
    // Get key bytes
    size_t key_len;
    uint8_t *key_bytes = get_key_bytes(ctx, argv[1], &key_len);
    if (!key_bytes) return JS_ThrowTypeError(ctx, "decrypt: invalid key");
    
    // Get IV/nonce
    size_t iv_len;
    uint8_t *iv = get_iv(ctx, argv[0], &iv_len);
    if (!iv) return JS_ThrowTypeError(ctx, "decrypt: missing IV/nonce");
    
    // Get ciphertext data (includes tag at the end for GCM)
    size_t ciphertext_len;
    uint8_t *ciphertext = JS_GetArrayBuffer(ctx, &ciphertext_len, argv[2]);
    if (!ciphertext) return JS_ThrowTypeError(ctx, "decrypt: data must be an ArrayBuffer");
    
    // For AES-GCM
    if (strcmp(algo_name, "AES-GCM") == 0) {
        // Validate key size
        if (key_len != 16 && key_len != 24 && key_len != 32) {
            return JS_ThrowTypeError(ctx, "decrypt: invalid key size for AES");
        }
        if (iv_len != 12) {
            return JS_ThrowTypeError(ctx, "decrypt: IV must be 12 bytes for GCM");
        }
        
        // Ciphertext must be at least 16 bytes (for the tag)
        if (ciphertext_len < 16) {
            return JS_ThrowTypeError(ctx, "decrypt: ciphertext too short");
        }
        
        // Output buffer: plaintext (ciphertext_len - tag_len)
        size_t plaintext_len = ciphertext_len - 16;
        uint8_t *output = (uint8_t*)malloc(plaintext_len);
        if (!output) return JS_ThrowOutOfMemory(ctx);
        
        // Use mbedtls GCM context
        mbedtls_gcm_context gcm;
        mbedtls_gcm_init(&gcm);
        
        int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key_bytes, key_len * 8);
        if (ret != 0) {
            mbedtls_gcm_free(&gcm);
            free(output);
            return JS_ThrowInternalError(ctx, "decrypt: failed to set GCM key");
        }
        
        // Decrypt and verify tag
        ret = mbedtls_gcm_auth_decrypt(&gcm, plaintext_len,
                                        iv, iv_len, NULL, 0,
                                        ciphertext + plaintext_len, 16,
                                        ciphertext, output);
        
        mbedtls_gcm_free(&gcm);
        
        if (ret != 0) {
            free(output);
            return JS_ThrowInternalError(ctx, "decrypt: authentication failed");
        }
        
        GCValue result = JS_NewArrayBufferCopy(ctx, output, plaintext_len);
        free(output);
        return result;
    }
    
    // Unsupported algorithm
    LOG_INFO("Crypto", "subtle.decrypt: unsupported algorithm %s", algo_name);
    return JS_ThrowTypeError(ctx, "decrypt: unsupported algorithm '%s'", algo_name);
}

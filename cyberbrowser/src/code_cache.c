/* code_cache.c - the bytecode cache (code_cache.h). */
#include "code_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int mbedtls_sha256(const unsigned char *input, size_t ilen, unsigned char *output, int is224);

/* Scripts smaller than this parse about as fast as their entry reads. */
#define CC_MIN_SOURCE 4096
#define CC_MAX_SOURCE (32u << 20)
#define CC_MAX_PENDING 64

/* An entry: magic, the engine build (NUL-terminated), the source's SHA-256,
 * the bytecode's length, the bytecode. */
static const char k_magic[8] = "CBCODE2";   /* 2: functions' source shared with the script's */

static const CodeCacheStore *g_store;
static CodeCacheStats g_stats;

typedef struct Pending {
    char key[33];
    uint8_t sha[32];
    char *src;
    size_t len;
} Pending;
static Pending g_pending[CC_MAX_PENDING];
static int g_npending;

void code_cache_set_store(const CodeCacheStore *store) {
    g_store = store;
}

void code_cache_stats(CodeCacheStats *out) {
    *out = g_stats;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void key_of(const uint8_t sha[32], char key[33]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        key[2 * i] = hex[sha[i] >> 4];
        key[2 * i + 1] = hex[sha[i] & 15];
    }
    key[32] = 0;
}

static void note(const uint8_t sha[32], const char *key, const char *src, size_t len) {
    if (g_npending >= CC_MAX_PENDING) return;
    for (int i = 0; i < g_npending; i++)
        if (!memcmp(g_pending[i].sha, sha, 32)) return;
    char *copy = (char *)malloc(len + 1);
    if (!copy) return;
    memcpy(copy, src, len);
    copy[len] = 0;
    Pending *p = &g_pending[g_npending++];
    memcpy(p->sha, sha, 32);
    memcpy(p->key, key, 33);
    p->src = copy;
    p->len = len;
}

GCValue code_cache_lookup(JSContextHandle ctx, const char *src, size_t len) {
    if (!g_store || len < CC_MIN_SOURCE || len > CC_MAX_SOURCE) return JS_UNDEFINED;
    double t0 = now_ms();
    uint8_t sha[32];
    if (mbedtls_sha256((const unsigned char *)src, len, sha, 0) != 0) return JS_UNDEFINED;
    char key[33];
    key_of(sha, key);
    double t1 = now_ms();
    g_stats.hash_ms += t1 - t0;
    uint8_t *data = NULL;
    size_t dlen = 0;
    if (!g_store->load(key, &data, &dlen)) {
        g_stats.misses++;
        note(sha, key, src, len);
        return JS_UNDEFINED;
    }
    /* Checked before a byte of bytecode is read: this build, this source. */
    const char *build = JS_GetBuildId();
    size_t blen = strlen(build) + 1, head = sizeof(k_magic) + blen + 32 + 4;
    GCValue fn = JS_UNDEFINED;
    uint32_t bclen = 0;
    if (dlen >= head) memcpy(&bclen, data + head - 4, 4);
    if (dlen >= head && !memcmp(data, k_magic, sizeof(k_magic)) && !memcmp(data + sizeof(k_magic), build, blen) &&
        !memcmp(data + sizeof(k_magic) + blen, sha, 32) && bclen == dlen - head) {
        fn = JS_ReadObjectWithSource(ctx, data + head, bclen, JS_READ_OBJ_BYTECODE, src, len);
        if (JS_IsException(fn)) {
            JS_GetException(ctx);
            fn = JS_UNDEFINED;
        }
    }
    free(data);
    g_stats.read_ms += now_ms() - t1;
    if (JS_IsUndefined(fn)) {
        g_stats.rejected++;
        note(sha, key, src, len);       /* (written anew: a stale or foreign entry) */
    } else {
        g_stats.hits++;
    }
    return fn;
}

bool code_cache_flush(JSContextHandle ctx, int max) {
    while (g_store && g_npending > 0 && max-- > 0) {
        Pending p = g_pending[--g_npending];
        GCValue fn = JS_Eval(ctx, p.src, p.len, "<cached>",
                             JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_FLAG_NO_LAZY);
        free(p.src);
        if (JS_IsException(fn)) {
            JS_GetException(ctx);
            continue;
        }
        size_t bclen = 0;
        uint8_t *bc = JS_WriteObject(ctx, &bclen, fn, JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_SHARE_SOURCE);
        if (!bc) {
            if (JS_HasException(ctx)) JS_GetException(ctx);
            continue;
        }
        const char *build = JS_GetBuildId();
        size_t blen = strlen(build) + 1, head = sizeof(k_magic) + blen + 32 + 4;
        uint8_t *entry = bclen <= 0xffffffffu ? (uint8_t *)malloc(head + bclen) : NULL;
        if (entry) {
            uint32_t n = (uint32_t)bclen;
            memcpy(entry, k_magic, sizeof(k_magic));
            memcpy(entry + sizeof(k_magic), build, blen);
            memcpy(entry + sizeof(k_magic) + blen, p.sha, 32);
            memcpy(entry + head - 4, &n, 4);
            memcpy(entry + head, bc, bclen);
            g_store->save(p.key, entry, head + bclen);
            g_stats.saved++;
            free(entry);
        }
        free(bc);
    }
    return g_store && g_npending > 0;
}

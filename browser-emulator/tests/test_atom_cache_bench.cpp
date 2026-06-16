/*
 * Benchmark for atom cache optimization
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "platform.h"
#include "quickjs.h"
#include "quickjs_gc_unified.h"
#include "js_atom_cache.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    printf("========================================\n");
    printf("Atom Cache Benchmark\n");
    printf("========================================\n\n");
    
    if (!platform_init() || !gc_init()) {
        printf("Failed to initialize\n");
        return 1;
    }
    
    JSRuntimeHandle rt = JS_NewRuntime();
    JSContextHandle ctx = JS_NewContext(rt);
    
    /* Test string - simulates identifier-heavy code */
    const char *identifiers[] = {
        "function", "var", "let", "const", "if", "else", "for", "while",
        "return", "break", "continue", "switch", "case", "default",
        "try", "catch", "finally", "throw", "new", "delete", "typeof",
        "instanceof", "in", "of", "void", "this", "true", "false",
        "null", "undefined", "NaN", "Infinity", "Object", "Array",
        "String", "Number", "Boolean", "Function", "Date", "RegExp",
        "Error", "Math", "JSON", "console", "window", "document",
        "loadPlayer", "initControls", "renderComments", "onClick",
        "handleEvent", "updateView", "fetchData", "processResponse",
        /* ... repeat to create many unique identifiers ... */
    };
    
    int num_identifiers = sizeof(identifiers) / sizeof(identifiers[0]);
    
    printf("Testing with %d unique identifiers\n", num_identifiers);
    printf("Running 100,000 atom lookups...\n\n");
    
    clock_t start = clock();
    
    /* Simulate parsing by looking up atoms multiple times */
    for (int iter = 0; iter < 10000; iter++) {
        for (int i = 0; i < num_identifiers; i++) {
            JSAtom atom = JS_NewAtom(ctx, identifiers[i]);
            (void)atom; // Suppress unused warning
        }
    }
    
    clock_t end = clock();
    double elapsed = (end - start) / (double)CLOCKS_PER_SEC;
    
    /* Get cache stats */
    JSContext *p = (JSContext*)gc_deref(ctx.handle());
    uint32_t hits, misses;
    js_atom_cache_stats(&p->atom_cache, &hits, &misses);
    
    printf("Results:\n");
    printf("  Time: %.3f seconds\n", elapsed);
    printf("  Lookups: 1,000,000\n");
    printf("  Cache hits: %u\n", hits);
    printf("  Cache misses: %u\n", misses);
    printf("  Hit rate: %.1f%%\n", (hits * 100.0) / (hits + misses));
    printf("  Lookups/sec: %.0f\n\n", 1000000.0 / elapsed);
    
    JS_FreeRuntime(rt);
    platform_cleanup();
    
    return 0;
}

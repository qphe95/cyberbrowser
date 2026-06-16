/*
 * Parsing speed benchmark for QuickJS optimizations
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "platform.h"
#include "quickjs.h"
#include "quickjs_gc_unified.h"

/* Generate a script with many variables to test array growth */
static char *generate_var_script(int num_vars) {
    size_t size = num_vars * 50 + 100;
    char *script = (char*)malloc(size);
    char *p = script;
    
    p += sprintf(p, "'use strict';\n");
    p += sprintf(p, "function test() {\n");
    
    for (int i = 0; i < num_vars; i++) {
        p += sprintf(p, "  var var%d = %d;\n", i, i);
    }
    
    p += sprintf(p, "  return var0;\n");
    p += sprintf(p, "}\n");
    
    return script;
}

/* Generate a script with many functions */
static char *generate_func_script(int num_funcs) {
    size_t size = num_funcs * 100 + 100;
    char *script = (char*)malloc(size);
    char *p = script;
    
    p += sprintf(p, "'use strict';\n");
    
    for (int i = 0; i < num_funcs; i++) {
        p += sprintf(p, "function func%d(x) { return x + %d; }\n", i, i);
    }
    
    p += sprintf(p, "func0(1);\n");
    
    return script;
}

static double measure_compile(JSContextHandle ctx, const char *script, const char *name) {
    clock_t start = clock();
    
    GCValue result = JS_Eval(ctx, script, strlen(script), name,
                             JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    
    clock_t end = clock();
    
    if (JS_IsException(result)) {
        GCValue exc = JS_GetException(ctx);
        const char *str = JS_ToCString(ctx, exc);
        printf("COMPILE ERROR: %s\n", str ? str : "unknown");
        return -1;
    }
    
    return (end - start) / (double)CLOCKS_PER_SEC;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    printf("========================================\n");
    printf("QuickJS Parsing Speed Benchmark\n");
    printf("========================================\n\n");
    
    if (!platform_init() || !gc_init()) {
        printf("Failed to initialize\n");
        return 1;
    }
    
    JSRuntimeHandle rt = JS_NewRuntime();
    JSContextHandle ctx = JS_NewContext(rt);
    JS_AddIntrinsicEval(ctx);
    
    /* Test 1: Variable declarations (tests array growth) */
    printf("Test 1: Variable Declaration Performance\n");
    printf("-----------------------------------------\n");
    
    int var_counts[] = {100, 500, 1000, 2000};
    for (size_t i = 0; i < sizeof(var_counts)/sizeof(var_counts[0]); i++) {
        int n = var_counts[i];
        char *script = generate_var_script(n);
        
        double time = measure_compile(ctx, script, "var_test");
        double vars_per_sec = n / time;
        
        printf("  %5d vars: %.3f sec (%.0f vars/sec)\n", n, time, vars_per_sec);
        
        free(script);
        
        /* Clear atom cache between tests for fair comparison */
        JSContext *p = (JSContext*)gc_deref(ctx.handle());
        js_atom_cache_reset(&p->atom_cache);
    }
    
    printf("\n");
    
    /* Test 2: Function declarations */
    printf("Test 2: Function Declaration Performance\n");
    printf("-----------------------------------------\n");
    
    int func_counts[] = {100, 500, 1000, 2000};
    for (size_t i = 0; i < sizeof(func_counts)/sizeof(func_counts[0]); i++) {
        int n = func_counts[i];
        char *script = generate_func_script(n);
        
        double time = measure_compile(ctx, script, "func_test");
        double funcs_per_sec = n / time;
        
        printf("  %5d funcs: %.3f sec (%.0f funcs/sec)\n", n, time, funcs_per_sec);
        
        free(script);
        
        JSContext *p = (JSContext*)gc_deref(ctx.handle());
        js_atom_cache_reset(&p->atom_cache);
    }
    
    printf("\n");
    
    /* Test 3: Real YouTube script if available */
    printf("Test 3: Real Script Performance\n");
    printf("--------------------------------\n");
    
    FILE *f = fopen("youtube_data/youtube_script_024_external.js", "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        char *script = (char*)malloc(size + 1);
        fread(script, 1, size, f);
        fclose(f);
        script[size] = '\0';
        
        printf("  Script size: %.2f KB\n", size / 1024.0);
        printf("  Compiling (this may take a while)...\n");
        fflush(stdout);
        
        /* Use fresh context - note: contexts are freed with runtime in this version */
        /* Just create a new context, old one will be cleaned up */
        ctx = JS_NewContext(rt);
        JS_AddIntrinsicEval(ctx);
        
        double time = measure_compile(ctx, script, "youtube_024");
        double kb_per_sec = (size / 1024.0) / time;
        
        printf("  Time: %.2f sec\n", time);
        printf("  Speed: %.1f KB/sec\n", kb_per_sec);
        
        /* Show atom cache stats */
        JSContext *p = (JSContext*)gc_deref(ctx.handle());
        uint32_t hits, misses;
        js_atom_cache_stats(&p->atom_cache, &hits, &misses);
        printf("  Atom cache: %u hits, %u misses (%.1f%% hit rate)\n",
               hits, misses, (hits * 100.0) / (hits + misses));
        
        free(script);
    } else {
        printf("  youtube_script_024_external.js not found\n");
    }
    
    printf("\n========================================\n");
    printf("Benchmark complete\n");
    printf("========================================\n");
    
    JS_FreeRuntime(rt);
    platform_cleanup();
    
    return 0;
}

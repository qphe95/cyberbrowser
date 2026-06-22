/* Test cooperative interrupt handler with a simple infinite loop. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "quickjs.h"
#include "quickjs_gc_unified.h"

int main() {
    gc_init();
    JSRuntimeHandle rt = JS_NewRuntime();
    JS_SetMemoryLimit(rt, 256 * 1024 * 1024);
    JS_SetMaxStackSize(rt, 8 * 1024 * 1024);
    
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    JS_SetInterruptHandler(rt, [](JSRuntimeHandle rt, void *opaque) -> int {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        struct timespec *s = (struct timespec *)opaque;
        double elapsed = (now.tv_sec - s->tv_sec) + (now.tv_nsec - s->tv_nsec) / 1e9;
        return elapsed > 1.0 ? 1 : 0;
    }, &start);
    
    JSContextHandle ctx = JS_NewContext(rt);
    JS_AddIntrinsicEval(ctx);
    printf("Running infinite loop...\n"); fflush(stdout);
    GCValue res = JS_Eval(ctx, "while(true) {}", 14, "<loop>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(res)) {
        GCValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        printf("Interrupted as expected: %s\n", msg ? msg : "(none)");
    } else {
        printf("ERROR: loop completed without interruption\n");
    }
    return 0;
}

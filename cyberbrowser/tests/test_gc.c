/*
 * Garbage collector tests (design: CONCURRENT_GC.md).
 *
 *   - heap and handles: header size, alignment, regions, humongous objects,
 *     realloc, explicit free, handle reuse
 *   - stop-the-world collection: roots, unreachable objects, compaction,
 *     pinning by raw pointers on the stack
 *   - collection while JS code is running
 *   - weak references and finalizers
 *   - concurrent cycles: boundary steps, dirty tracking, minor and major
 *     cycles, verification against a full re-mark
 *   - engine structures built on the same atomics (hash tables, job queue,
 *     thread pool)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#include "platform.h"
#include "test_runner.h"
#include "quickjs.h"
#include "quickjs_gc_unified.h"
#include "quickjs-internal.h"
#include "lockfree_hash_table.h"
#include "browser_api_impl_handle_helpers.h"

#include "gc_core.h"
#include "gc_collector.h"

uint32_t JSRuntime_get_shape_hash_count(JSRuntimeHandle rt);
uint32_t JSRuntime_get_shape_hash_size(JSRuntimeHandle rt);
uint32_t JSRuntime_get_atom_hash_count(JSRuntimeHandle rt);
uint32_t JSRuntime_get_atom_hash_size(JSRuntimeHandle rt);

/* Shared QuickJS context accessor declared in test_main.c */
JSContextHandle get_shared_test_context(void);

/* ============================================================================
 * Test Helpers
 * ============================================================================ */

/* Create a simple object graph for testing */
static JSContextHandle get_test_ctx(void) {
    return get_shared_test_context();
}

static GCValue eval_test(JSContextHandle ctx, const char *script) {
    return JS_Eval(ctx, script, strlen(script), "<test>", JS_EVAL_TYPE_GLOBAL);
}

static bool is_exception_free(JSContextHandle ctx, GCValue v) {
    if (JS_IsException(v)) {
        GCValue exc = JS_GetException(ctx);
        (void)exc;
        return false;
    }
    return true;
}

/* ============================================================================
 * Test Cases
 * ============================================================================ */

/* ============================================================================
 * Thread Pool Tests
 * ============================================================================ */

static volatile int g_thread_pool_counter = 0;
static volatile int g_thread_pool_job_count = 0;

static void thread_pool_increment_job(void *arg) {
    int value = *(int *)arg;
    __sync_fetch_and_add(&g_thread_pool_counter, value);
    __sync_fetch_and_add(&g_thread_pool_job_count, 1);
}

TEST(test_gc_thread_pool_basic_job) {
    g_thread_pool_counter = 0;
    g_thread_pool_job_count = 0;
    
    int value = 42;
    ASSERT_TRUE(gc_thread_pool_submit_test_job(thread_pool_increment_job, &value));
    
    gc_thread_pool_wait_empty();
    
    ASSERT_EQ(42, g_thread_pool_counter);
    ASSERT_EQ(1, g_thread_pool_job_count);
    
    return true;
}

TEST(test_gc_thread_pool_multiple_jobs) {
    g_thread_pool_counter = 0;
    g_thread_pool_job_count = 0;
    
    const int job_count = 100;
    int values[job_count];
    for (int i = 0; i < job_count; i++) {
        values[i] = 1;
        ASSERT_TRUE(gc_thread_pool_submit_test_job(thread_pool_increment_job, &values[i]));
    }
    
    gc_thread_pool_wait_empty();
    
    ASSERT_EQ(job_count, g_thread_pool_counter);
    ASSERT_EQ(job_count, g_thread_pool_job_count);
    
    return true;
}

/* ============================================================================
 * Lock-free handle free-list tests
 * ============================================================================ */

/* ============================================================================
 * Type bucket tests
 * ============================================================================ */

/* ============================================================================
 * Lock-free hash table tests
 * ============================================================================ */

static uint32_t test_hash_u32(uint32_t key) {
    /* Knuth's multiplicative hash */
    return key * 2654435761u;
}

TEST(test_lf_hash_basic) {
    LFHashTable *t = lf_hash_create(16);
    ASSERT_TRUE(t != NULL);
    ASSERT_EQ(16, (int)t->bucket_count);

    ASSERT_TRUE(lf_hash_insert(t, test_hash_u32(42), 42, 142));
    ASSERT_EQ(142, (int)lf_hash_lookup(t, test_hash_u32(42), 42));
    ASSERT_EQ((int)GC_HANDLE_NULL, (int)lf_hash_lookup(t, test_hash_u32(43), 43));

    lf_hash_destroy(t);
    return true;
}

TEST(test_lf_hash_update) {
    LFHashTable *t = lf_hash_create(16);
    ASSERT_TRUE(lf_hash_insert(t, test_hash_u32(7), 7, 100));
    ASSERT_EQ(100, (int)lf_hash_lookup(t, test_hash_u32(7), 7));

    ASSERT_TRUE(lf_hash_insert(t, test_hash_u32(7), 7, 200));
    ASSERT_EQ(200, (int)lf_hash_lookup(t, test_hash_u32(7), 7));
    ASSERT_EQ(1, (int)lf_hash_count(t));

    lf_hash_destroy(t);
    return true;
}

TEST(test_lf_hash_remove) {
    LFHashTable *t = lf_hash_create(16);
    ASSERT_TRUE(lf_hash_insert(t, test_hash_u32(5), 5, 55));
    ASSERT_EQ(55, (int)lf_hash_lookup(t, test_hash_u32(5), 5));

    ASSERT_TRUE(lf_hash_remove(t, test_hash_u32(5), 5));
    ASSERT_EQ((int)GC_HANDLE_NULL, (int)lf_hash_lookup(t, test_hash_u32(5), 5));
    ASSERT_EQ(0, (int)lf_hash_count(t));

    lf_hash_destroy(t);
    return true;
}

TEST(test_lf_hash_resize) {
    LFHashTable *t = lf_hash_create(128);
    for (int i = 1; i <= 64; i++) {
        ASSERT_TRUE(lf_hash_insert(t, test_hash_u32((uint32_t)i), (GCHandle)i, (GCHandle)(i * 2)));
    }
    ASSERT_EQ(64, (int)lf_hash_count(t));

    ASSERT_TRUE(lf_hash_resize(&t, 256));
    ASSERT_TRUE(t->bucket_count >= 256);
    ASSERT_EQ(64, (int)lf_hash_count(t));

    for (int i = 1; i <= 64; i++) {
        ASSERT_EQ(i * 2, (int)lf_hash_lookup(t, test_hash_u32((uint32_t)i), (GCHandle)i));
    }

    lf_hash_destroy(t);
    return true;
}

typedef struct LFHashThreadArg LFHashThreadArg;
struct LFHashThreadArg {
    LFHashTable *t;
    uint32_t base;
    uint32_t count;
};

static void lf_hash_insert_job(void *arg) {
    LFHashThreadArg *a = (LFHashThreadArg *)arg;
    for (uint32_t i = 0; i < a->count; i++) {
        GCHandle key = a->base + i;
        lf_hash_insert(a->t, test_hash_u32((uint32_t)key), key, key * 10);
    }
}

TEST(test_lf_hash_threaded) {
    LFHashTable *t = lf_hash_create(4096);
    ASSERT_TRUE(t != NULL);

    static const int THREADS = 4;
    static const int PER_THREAD = 256;
    LFHashThreadArg args[THREADS];
    for (int i = 0; i < THREADS; i++) {
        args[i].t = t;
        args[i].base = (uint32_t)(i * PER_THREAD + 1);
        args[i].count = PER_THREAD;
        ASSERT_TRUE(gc_thread_pool_submit_test_job(lf_hash_insert_job, &args[i]));
    }
    gc_thread_pool_wait_empty();

    ASSERT_EQ(THREADS * PER_THREAD, (int)lf_hash_count(t));
    for (int i = 0; i < THREADS; i++) {
        for (int j = 0; j < PER_THREAD; j++) {
            GCHandle key = (GCHandle)(i * PER_THREAD + j + 1);
            ASSERT_EQ((int)(key * 10), (int)lf_hash_lookup(t, test_hash_u32((uint32_t)key), key));
        }
    }

    lf_hash_destroy(t);
    return true;
}

/* ============================================================================
 * QuickJS Integration Tests
 * ============================================================================ */

/* Forward declaration for shared context accessor */
JSContextHandle get_shared_test_context(void);

/* ============================================================================
 * Write-barrier path tests
 *
 * These exercises the code paths that now emit write barriers.  They run in
 * the shared QuickJS context and verify the operations still produce correct
 * results; the underlying barriers are exercised by every store below.
 * ============================================================================ */

/* ============================================================================
 * Collection while code is running
 *
 * A collection requested from inside a script (by a native function, as the
 * HTMLElement constructor does) must keep everything the running code can
 * still reach: locals, arguments and operand-stack temporaries of running
 * frames, values native functions hold in C locals, and suspended generator
 * frames.  It must not move objects that running code holds raw pointers to.
 * Each script below returns "ok" or a description of what went wrong.
 * ============================================================================ */

static GCValue js_test_run_gc(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

static JSContextHandle get_gc_test_ctx(void) {
    JSContextHandle ctx = get_test_ctx();
    if (!SAFE_VALID(ctx)) return ctx;
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue existing = JS_GetPropertyStr(ctx, global, "__test_gc");
    if (!JS_IsFunction(ctx, existing))
        JS_SetPropertyStr(ctx, global, "__test_gc",
                          JS_NewCFunction(ctx, js_test_run_gc, "__test_gc", 0));
    return ctx;
}

/* Evaluate a script that returns "ok" on success; print what it returned
 * otherwise. */
static bool eval_expect_ok(JSContextHandle ctx, const char *script) {
    GCValue r = eval_test(ctx, script);
    if (JS_IsException(r)) {
        GCValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        printf("    exception: %s\n", msg ? msg : "?");
        return false;
    }
    const char *str = JS_ToCString(ctx, r);
    if (!str || strcmp(str, "ok") != 0) {
        printf("    script returned: %s\n", str ? str : "(null)");
        return false;
    }
    return true;
}

#define GC_RUNNING_TEST(name, script) \
    TEST(name) { \
        JSContextHandle ctx = get_gc_test_ctx(); \
        if (!SAFE_VALID(ctx)) { printf("    (skipped - no context)"); return true; } \
        ASSERT_TRUE(eval_expect_ok(ctx, script)); \
        ASSERT_EQ(0, (int)gc_test_heap_check()); \
        return true; \
    }

GC_RUNNING_TEST(test_gc_running_keeps_locals,
    "(function(){ var o = {a: {b: 42}}; __test_gc(); "
    "  return o.a.b === 42 ? 'ok' : 'local lost: ' + o.a.b; })()")

GC_RUNNING_TEST(test_gc_running_keeps_arguments,
    "(function(a, b){ __test_gc(); "
    "  return a.v + b.v === 3 ? 'ok' : 'arguments lost'; })({v: 1}, {v: 2})")

GC_RUNNING_TEST(test_gc_running_keeps_operand_stack,
    "(function(){ var r = [{x: 1}, {x: 2}, (__test_gc(), {x: 3}), {x: 4}]"
    "  .map(function(o){ return o.x; }).join(); "
    "  return r === '1,2,3,4' ? 'ok' : 'temporaries lost: ' + r; })()")

/* Call frames are reused from call to call at the same depth unless
 * something took the frame.  Closures made between other calls at the
 * same depth still see their own frame's values. */
GC_RUNNING_TEST(test_gc_frame_reuse_keeps_captured_frames,
    "(function(){"
    "  function mk(x, y) { var z = {v: x}; return function(){ return z.v + y; }; }"
    "  function noise(a, b, c) { var t = [a, b, c]; return t.length + (a | 0); }"
    "  var fs = [];"
    "  for (var i = 0; i < 3000; i++) { fs.push(mk('v' + i, i)); noise(i, {o: i}, 'n' + i); }"
    "  __test_gc(); var g = []; for (var j = 0; j < 20000; j++) g.push({x: j}); __test_gc();"
    "  for (var i = 0; i < 3000; i++)"
    "    if (fs[i]() !== 'v' + i + i) return 'closure ' + i + ' got ' + fs[i]();"
    "  return 'ok'; })()")

/* A lazily parsed inner function keeps its parent's frame to compile
 * against: first called after the parent returned and its depth was used
 * by other calls. */
GC_RUNNING_TEST(test_gc_frame_reuse_keeps_lazy_parent_frames,
    "(function(){"
    "  function outer(n, s) { function inner() { return s + n * 2; } return inner; }"
    "  function noise(a, b) { return [a, b, {a: a}].length; }"
    "  var fs = [];"
    "  for (var i = 0; i < 2000; i++) { fs.push(outer(i, 'p')); noise(i, 'q' + i); }"
    "  __test_gc(); var g = []; for (var j = 0; j < 20000; j++) g.push({x: j}); __test_gc();"
    "  for (var i = 0; i < 2000; i++)"
    "    if (fs[i]() !== 'p' + i * 2) return 'inner ' + i + ' got ' + fs[i]();"
    "  return 'ok'; })()")

/* Deeper than the frame pool, and unwinding by exceptions, which leave
 * through the same exit. */
GC_RUNNING_TEST(test_gc_frame_reuse_past_pool_depth_and_throws,
    "(function(){"
    "  function rec(n, o) { if (n === 0) { __test_gc(); return o.v; } return rec(n - 1, {v: o.v + 1}); }"
    "  function thrower(n) { if (n === 0) throw new Error('x'); return thrower(n - 1) + 1; }"
    "  for (var k = 0; k < 3; k++) {"
    "    try { thrower(300); return 'no throw'; } catch (e) { if (e.message !== 'x') return 'wrong error'; }"
    "    var r = rec(400, {v: 0}); if (r !== 400) return 'deep ' + k + ' got ' + r;"
    "  }"
    "  return 'ok'; })()")

/* Arrays, regexps and arguments objects share their context's shape, and
 * objects past 32 properties keep a private shape that grows in place:
 * changing one object must never show through another. */
GC_RUNNING_TEST(test_gc_shared_and_dictionary_shapes,
    "(function(){"
    "  var a = [1, 2], b = [3]; a.extra = 'x';"
    "  if (b.extra !== undefined || b.length !== 1) return 'array shape leaked';"
    "  Object.defineProperty(a, 'length', {writable: false});"
    "  b.push(4); if (b.length !== 2) return 'array length frozen through sharing';"
    "  var r1 = /a/g, r2 = /b/g; r1.lastIndex = 5;"
    "  if (r2.lastIndex !== 0) return 'regexp shape leaked';"
    "  function f() { arguments.tag = 1; return arguments; } function g() { return arguments; }"
    "  if (g(1).tag !== undefined || f(1).tag !== 1) return 'arguments shape leaked';"
    "  var d = {}, e = {};"
    "  for (var i = 0; i < 200; i++) { d['k' + i] = i; if (i < 40) e['k' + i] = -i; }"
    "  for (var i = 0; i < 200; i += 3) delete d['k' + i];"
    "  Object.defineProperty(d, 'k1', {enumerable: false});"
    "  __test_gc();"
    "  for (var i = 0; i < 200; i++) {"
    "    var want = i % 3 === 0 ? undefined : i;"
    "    if (d['k' + i] !== want) return 'dict k' + i + ' = ' + d['k' + i];"
    "  }"
    "  for (var i = 0; i < 40; i++) if (e['k' + i] !== -i) return 'second dict k' + i;"
    "  if (Object.keys(d).indexOf('k1') >= 0) return 'flag change lost';"
    "  if (Object.keys(e).length !== 40) return 'second dict keys';"
    "  return 'ok'; })()")

/* Objects with up to 4 property slots keep them inside the object and move
 * them to a separate array when they grow: values survive the move and
 * collections on either side of it. */
GC_RUNNING_TEST(test_gc_inline_properties_grow_and_survive,
    "(function(){"
    "  var objs = [];"
    "  for (var n = 0; n < 300; n++) {"
    "    var o = {};"
    "    for (var k = 0; k < (n % 12); k++) {"
    "      o['p' + k] = {v: n * 100 + k};"
    "      if (k === 2 && n % 5 === 0) __test_gc();"
    "    }"
    "    if (n % 7 === 0) Object.defineProperty(o, 'g', {get: function(){ return this.p0; }, configurable: true});"
    "    if (n % 9 === 0 && (n % 12) > 1) { delete o.p1; o.q = 'q' + n; }"
    "    objs.push(o);"
    "  }"
    "  __test_gc(); var junk = []; for (var j = 0; j < 20000; j++) junk.push({j: j}); __test_gc();"
    "  for (var n = 0; n < 300; n++) {"
    "    var o = objs[n];"
    "    for (var k = 0; k < (n % 12); k++) {"
    "      if (k === 1 && n % 9 === 0 && (n % 12) > 1) { if ('p1' in o) return 'delete lost ' + n; continue; }"
    "      if (!o['p' + k] || o['p' + k].v !== n * 100 + k) return 'obj ' + n + ' p' + k;"
    "    }"
    "    if (n % 7 === 0 && (n % 12) > 0 && o.g !== o.p0) return 'getter ' + n;"
    "    if (n % 9 === 0 && (n % 12) > 1 && o.q !== 'q' + n) return 'added after delete ' + n;"
    "  }"
    "  var f = Object.freeze({a: 1, b: 2}); try { f.c = 3; } catch (e) {}"
    "  if (f.c !== undefined || f.a !== 1) return 'freeze';"
    "  function P(x, y) { this.x = x; this.y = y; } var ps = [];"
    "  for (var i = 0; i < 1000; i++) ps.push(new P(i, -i));"
    "  __test_gc();"
    "  for (var i = 0; i < 1000; i++) if (ps[i].x !== i || ps[i].y !== -i) return 'ctor ' + i;"
    "  return 'ok'; })()")

GC_RUNNING_TEST(test_gc_running_keeps_native_temporaries,
    "(function(){ var r = [1, 2, 3].map(function(x){ __test_gc(); return {v: x * 10}; }); "
    "  return (r.length === 3 && r[0].v === 10 && r[2].v === 30) ? 'ok' "
    "    : 'map result lost: ' + JSON.stringify(r); })()")

GC_RUNNING_TEST(test_gc_running_keeps_json_state,
    "(function(){ var src = JSON.parse(JSON.stringify({a: [1, 2, {b: 'c'}], d: {e: {f: 'g'}}})); "
    "  var r = JSON.stringify(src, function(k, v){ if (k === 'b') __test_gc(); return v; }); "
    "  return r === '{\"a\":[1,2,{\"b\":\"c\"}],\"d\":{\"e\":{\"f\":\"g\"}}}' ? 'ok' : r; })()")

GC_RUNNING_TEST(test_gc_running_deep_recursion,
    "(function(){ function rec(n){ var o = {n: n, s: 's' + n}; "
    "    if (n === 0) { __test_gc(); return 0; } "
    "    return rec(n - 1) + (o.s === 's' + n ? 1 : 1000); } "
    "  var r = rec(150); return r === 150 ? 'ok' : 'frames lost: ' + r; })()")

GC_RUNNING_TEST(test_gc_running_in_generator,
    "(function(){ function* g(){ var o = {z: 5}; yield 1; __test_gc(); yield o.z; } "
    "  var it = g(); it.next(); __test_gc(); var r = it.next().value; "
    "  return r === 5 ? 'ok' : 'generator frame lost: ' + r; })()")

GC_RUNNING_TEST(test_gc_running_repeated,
    "(function(){ var keep = []; var sum = 0; "
    "  for (var k = 0; k < 30; k++) { var tmp = []; "
    "    for (var i = 0; i < 200; i++) tmp.push({k: k, i: i, s: 'x' + i}); "
    "    if (k % 3 === 0) __test_gc(); keep.push(tmp[k]); "
    "    sum += tmp.reduce(function(a, o){ return a + o.i; }, 0); } "
    "  var ok = sum === 30 * 19900 && keep.every(function(o, i){ return o.k === i && o.s === 'x' + o.i; }); "
    "  return ok ? 'ok' : 'repeated collection lost data: ' + sum; })()")

/* An async function suspended at an await keeps its locals and the values
 * on its operand stack (f and a in f(a, await p, b)) while collections and
 * compactions run, and finds them intact when it resumes.  YouTube's modern
 * build awaits everywhere; a lost value surfaced as a crash reading a
 * string fetch() was called with. */
static void run_jobs(JSContextHandle ctx) {
    JSContextHandle pctx;
    while (JS_ExecutePendingJob(JS_GetRuntime(ctx), &pctx) > 0) { }
}

TEST(test_gc_async_frame_survives_await) {
    JSContextHandle ctx = get_gc_test_ctx();
    if (!SAFE_VALID(ctx)) { printf("    (skipped - no context)"); return true; }
    GCValue r = eval_test(ctx,
        "globalThis.__async_res = 'pending'; globalThis.__async_resolve = null;"
        "(async function () {"
        "  var rnd = String(Math.random());"
        "  var s = ['a', 'b', rnd].join('-');"
        "  var o = { k: s, arr: [1, 2, 3], nested: { t: 'n' + rnd } };"
        "  var big = []; for (var i = 0; i < 200; i++) big.push({ i: i, s: 'v' + i + rnd });"
        "  function combine(x, y, z) { return x + '|' + y + '|' + z; }"
        "  await null;"
        "  var mid = combine('pre' + rnd, await new Promise(function (res) { __async_resolve = res; }), 'post' + rnd);"
        "  var ok = o.k === s && o.arr.length === 3 && o.nested.t === 'n' + rnd &&"
        "           big[199].s === 'v199' + rnd && mid === 'pre' + rnd + '|middle|post' + rnd;"
        "  __async_res = ok ? 'ok' : 'lost: ' + mid;"
        "})(); 'started'");
    (void)r;
    run_jobs(ctx);
    /* Suspended at the inner await: collect, churn and compact around it. */
    for (int round = 0; round < 4; round++) {
        eval_test(ctx, "(function(){ var g = []; for (var i = 0; i < 20000; i++) g.push({ x: 'garbage' + i }); return g.length; })()");
        JS_RunGC(JS_GetRuntime(ctx));
    }
    ASSERT_TRUE(eval_expect_ok(ctx, "typeof __async_resolve === 'function' ? 'ok' : 'not suspended'"));
    eval_test(ctx, "__async_resolve('middle'); 1");
    run_jobs(ctx);
    ASSERT_TRUE(eval_expect_ok(ctx, "__async_res"));
    ASSERT_EQ(0, (int)gc_test_heap_check());
    return true;
}

/* A closure keeps the arguments of an async function that already returned
 * (YouTube: async function(T,P){ ... .then(k=>{P(k)}) }).  The frame's
 * var_refs were never closed and a completed frame's arguments were not
 * marked, so P was swept and the closure called freed memory. */
TEST(test_gc_async_closure_keeps_args_after_return) {
    JSContextHandle ctx = get_gc_test_ctx();
    if (!SAFE_VALID(ctx)) { printf("    (skipped - no context)"); return true; }
    GCValue r = eval_test(ctx,
        "globalThis.__ac_res = []; globalThis.__ac_hits = 0;"
        "(function () {"
        "  async function gg(P, p) { return p.then(k => { P(k); return k; }); }"
        "  for (var i = 0; i < 50; i++) {"
        "    var p = new Promise(function (res) { __ac_res.push(res); });"
        "    gg(function (k) { __ac_hits += k; }, p);"
        "  }"
        "})(); 'started'");
    (void)r;
    run_jobs(ctx);
    for (int round = 0; round < 4; round++) {
        eval_test(ctx, "(function(){ var g = []; for (var i = 0; i < 20000; i++) g.push({ x: 'garbage' + i }); return g.length; })()");
        JS_RunGC(JS_GetRuntime(ctx));
    }
    eval_test(ctx, "__ac_res.forEach(function (res) { res(2); }); 1");
    run_jobs(ctx);
    ASSERT_TRUE(eval_expect_ok(ctx, "__ac_hits === 100 ? 'ok' : 'hits ' + __ac_hits"));
    ASSERT_EQ(0, (int)gc_test_heap_check());
    return true;
}

/* The frame chain must be unlinked when a generator yields; otherwise the
 * next call links back to the suspended frame and the chain becomes a cycle
 * (the collector then never finishes walking it, and direct eval asserts). */
GC_RUNNING_TEST(test_gc_frame_chain_after_generator,
    "(function(){ function* g(){ yield 1; yield 2; } var it = g(); it.next(); "
    "  [1, 2].forEach(function(){ it.next(); }); __test_gc(); "
    "  return eval('1 + 2') === 3 ? 'ok' : 'eval after generator'; })()")

TEST(test_gc_bigint_survives) {
    JSContextHandle ctx = get_gc_test_ctx();
    if (!SAFE_VALID(ctx)) { printf("    (skipped - no context)"); return true; }
    ASSERT_TRUE(is_exception_free(ctx, eval_test(ctx,
        "var __big_a = 2n ** 100n; var __big_o = {k: 3n ** 50n}; 0")));
    JS_RunGC(JS_GetRuntime(ctx));
    ASSERT_TRUE(is_exception_free(ctx, eval_test(ctx,
        "(function(){ var j = []; for (var i = 0; i < 2000; i++) j.push(BigInt(i) * 7n ** 30n); return j.length; })()")));
    JS_RunGC(JS_GetRuntime(ctx));
    ASSERT_TRUE(eval_expect_ok(ctx,
        "(String(__big_a) === '1267650600228229401496703205376' && "
        " String(__big_o.k) === '717897987691852588770249') ? 'ok' "
        " : 'BigInt corrupted: ' + String(__big_a)"));
    ASSERT_TRUE(eval_expect_ok(ctx,
        "(function(){ var b = 5n ** 40n; __test_gc(); "
        "  return String(b) === '9094947017729282379150390625' ? 'ok' : 'local BigInt: ' + String(b); })()"));
    ASSERT_EQ(0, (int)gc_test_heap_check());
    return true;
}

/* ============================================================================
 * Shape hash table integration tests
 * ============================================================================ */

TEST(test_shape_hash_empty_shape_sharing) {
    JSContextHandle ctx = get_test_ctx();
    if (!SAFE_VALID(ctx)) { printf("    (skipped - no context)"); return true; }
    JSRuntimeHandle rt = (JSRuntimeHandle)(JS_GetRuntime(ctx));

    /* Create objects directly so we do not pollute the global object and
       create unrelated shape changes. */
    GCValue o1 = JS_NewObject(ctx);
    ASSERT_TRUE(JS_IsObject(o1));
    uint32_t count_after_first = JSRuntime_get_shape_hash_count(rt);

    GCValue o2 = JS_NewObject(ctx);
    ASSERT_TRUE(JS_IsObject(o2));
    uint32_t count_after_second = JSRuntime_get_shape_hash_count(rt);

    /* The second object must reuse the hashed empty shape; the entry count
       should not grow. */
    ASSERT_EQ(count_after_first, count_after_second);
    return true;
}

TEST(test_shape_hash_property_shape_sharing) {
    JSContextHandle ctx = get_test_ctx();
    if (!SAFE_VALID(ctx)) { printf("    (skipped - no context)"); return true; }
    JSRuntimeHandle rt = (JSRuntimeHandle)(JS_GetRuntime(ctx));

    GCValue o1 = JS_NewObject(ctx);
    ASSERT_TRUE(JS_IsObject(o1));
    ASSERT_TRUE(JS_SetPropertyStr(ctx, o1, "x", JS_NewInt32(ctx, 1)) >= 0);
    ASSERT_TRUE(JS_SetPropertyStr(ctx, o1, "y", JS_NewInt32(ctx, 2)) >= 0);
    uint32_t count_after_first = JSRuntime_get_shape_hash_count(rt);

    GCValue o2 = JS_NewObject(ctx);
    ASSERT_TRUE(JS_IsObject(o2));
    ASSERT_TRUE(JS_SetPropertyStr(ctx, o2, "x", JS_NewInt32(ctx, 3)) >= 0);
    ASSERT_TRUE(JS_SetPropertyStr(ctx, o2, "y", JS_NewInt32(ctx, 4)) >= 0);
    uint32_t count_after_second = JSRuntime_get_shape_hash_count(rt);

    /* The {x,y} shape chain should be shared, so the count should not grow
       by another full property chain. */
    ASSERT_EQ(count_after_first, count_after_second);
    return true;
}

TEST(test_shape_hash_resize) {
    JSContextHandle ctx = get_test_ctx();
    if (!SAFE_VALID(ctx)) { printf("    (skipped - no context)"); return true; }
    JSRuntimeHandle rt = (JSRuntimeHandle)(JS_GetRuntime(ctx));
    /* Remove the dead shapes of earlier tests first: a background cycle
     * during the eval below would otherwise remove them and the count could
     * go down. */
    JS_RunGC(rt);
    uint32_t count_before = JSRuntime_get_shape_hash_count(rt);

    GCValue r = eval_test(ctx,
        "var __sh_arr = [];"
        "for (var i = 0; i < 300; i++) {"
        "  var o = {}; o['prop' + i] = i; __sh_arr.push(o);"
        "}"
        "__sh_arr.length;");
    ASSERT_TRUE(is_exception_free(ctx, r));
    ASSERT_EQ(300, JS_VALUE_GET_INT(r));

    uint32_t count_after = JSRuntime_get_shape_hash_count(rt);
    ASSERT_TRUE(count_after > count_before);
    return true;
}

TEST(test_shape_hash_gc_cleanup) {
    JSContextHandle ctx = get_test_ctx();
    if (!SAFE_VALID(ctx)) { printf("    (skipped - no context)"); return true; }
    GCValue r = eval_test(ctx,
        "var __sh_e = {a:1, b:2, c:3}; __sh_e");
    ASSERT_TRUE(is_exception_free(ctx, r));
    JSObjectHandle o = JS_VALUE_GET_OBJ_HANDLE(r);
    ASSERT_TRUE(SAFE_VALID(o));

    JSRuntimeHandle rt = (JSRuntimeHandle)(JS_GetRuntime(ctx));
    uint32_t count_before = JSRuntime_get_shape_hash_count(rt);
    ASSERT_TRUE(count_before > 0);

    /* Drop the object and run GC.  Dead shapes should be removed by the
       cleanup pass without crashing. */
    r = eval_test(ctx, "__sh_e = null; 42");
    ASSERT_TRUE(is_exception_free(ctx, r));
    JS_RunGC(rt);

    /* The exact count is non-deterministic because other tests may have
       populated the table, but the runtime must stay consistent. */
    (void)JSRuntime_get_shape_hash_count(rt);
    return true;
}

/* ============================================================================
 * Lock-free atom hash + atomic class ID tests
 * ============================================================================ */

TEST(test_atom_hash_interning) {
    JSContextHandle ctx = get_test_ctx();
    if (!SAFE_VALID(ctx)) { printf("    (skipped - no context)"); return true; }
    JSAtom a1 = JS_NewAtom(ctx, "__atom_hash_intern_test");
    JSAtom a2 = JS_NewAtom(ctx, "__atom_hash_intern_test");
    ASSERT_TRUE(a1 != JS_ATOM_NULL);
    ASSERT_EQ(a1, a2);
    JS_FreeAtom(ctx, a1);
    JS_FreeAtom(ctx, a2);
    return true;
}

TEST(test_atom_hash_resize) {
    JSContextHandle ctx = get_test_ctx();
    if (!SAFE_VALID(ctx)) { printf("    (skipped - no context)"); return true; }
    JSRuntimeHandle rt = (JSRuntimeHandle)(JS_GetRuntime(ctx));
    uint32_t count_before = JSRuntime_get_atom_hash_count(rt);

    JSAtom atoms[300];
    char buf[64];
    for (int i = 0; i < 300; i++) {
        snprintf(buf, sizeof(buf), "__atom_resize_%d", i);
        atoms[i] = JS_NewAtom(ctx, buf);
        ASSERT_TRUE(atoms[i] != JS_ATOM_NULL);
    }

    uint32_t count_after = JSRuntime_get_atom_hash_count(rt);
    ASSERT_TRUE(count_after > count_before);

    for (int i = 0; i < 300; i++) {
        JS_FreeAtom(ctx, atoms[i]);
    }
    return true;
}

TEST(test_atom_hash_gc_cleanup) {
    JSContextHandle ctx = get_test_ctx();
    if (!SAFE_VALID(ctx)) { printf("    (skipped - no context)"); return true; }
    JSRuntimeHandle rt = (JSRuntimeHandle)(JS_GetRuntime(ctx));

    JSAtom a = JS_NewAtom(ctx, "__atom_gc_cleanup_temp");
    ASSERT_TRUE(a != JS_ATOM_NULL);
    JS_FreeAtom(ctx, a);
    JS_RunGC(rt);

    /* Re-creating the same string should still work after GC cleanup. */
    JSAtom b = JS_NewAtom(ctx, "__atom_gc_cleanup_temp");
    ASSERT_TRUE(b != JS_ATOM_NULL);
    JS_FreeAtom(ctx, b);
    return true;
}

static GCValue int32_value(int32_t v) {
    GCValue r = GC_UNDEFINED;
    r.tag = JS_TAG_INT;
    r.u.int32 = v;
    return r;
}

TEST(test_property_array_prealloc) {
    JSContextHandle ctx = get_test_ctx();
    GCValue obj_val = JS_NewObject(ctx);
    ASSERT_TRUE(JS_IsObject(obj_val));
    JSObjectHandle obj = JS_VALUE_GET_OBJ_HANDLE(obj_val);
    ASSERT_TRUE(SAFE_VALID(obj));

    /* Version starts stable (even). */
    uint32_t v0 = js_obj_prop_version(obj);
    ASSERT_TRUE((v0 & 1) == 0);

    ASSERT_TRUE(js_object_prealloc_properties(ctx, SAFE_HANDLE(obj), 16));

    /* After pre-sizing the version is still stable. */
    uint32_t v1 = js_obj_prop_version(obj);
    ASSERT_TRUE((v1 & 1) == 0);

    JSShapeHandle sh = js_obj_shape_handle(obj);
    ASSERT_TRUE(SAFE_VALID(sh));
    ASSERT_TRUE(SAFE_GET_OR(sh, JSShapeHandle, prop_size, 0) >= 16);

    GCValue val = int32_value(42);
    ASSERT_TRUE(js_object_set_property_value_atomic(ctx, SAFE_HANDLE(obj), 0, val));
    GCValue got = js_object_get_property_value_atomic(ctx, SAFE_HANDLE(obj), 0);
    ASSERT_TRUE(JS_IsNumber(got));
    ASSERT_EQ(JS_VALUE_GET_INT(got), 42);
    return true;
}

typedef struct PropArrayThreadArgs {
    JSContextHandle ctx;
    GCHandle obj;
    int iters;
} PropArrayThreadArgs;

static void *prop_array_cas_worker(void *arg)
{
    PropArrayThreadArgs *a = (PropArrayThreadArgs *)arg;
    for (int i = 0; i < a->iters; i++) {
        for (;;) {
            GCValue cur = js_object_get_property_value_atomic(a->ctx, a->obj, 0);
            int cur_i = JS_VALUE_GET_INT(cur);
            GCValue next = int32_value(cur_i + 1);
            GCValue actual = GC_UNDEFINED;
            if (js_object_cas_property_value_atomic(a->ctx, a->obj, 0,
                                                    cur, next, &actual))
                break;
        }
    }
    return NULL;
}

TEST(test_property_array_atomic_cas) {
    JSContextHandle ctx = get_test_ctx();
    GCValue obj_val = JS_NewObject(ctx);
    ASSERT_TRUE(JS_IsObject(obj_val));
    JSObjectHandle obj = JS_VALUE_GET_OBJ_HANDLE(obj_val);
    ASSERT_TRUE(SAFE_VALID(obj));
    ASSERT_TRUE(js_object_prealloc_properties(ctx, SAFE_HANDLE(obj), 16));

    /* Keep the object and its property array alive and prevent GC from being
     * triggered by allocations during the threaded portion of the test. */
    gc_add_root(SAFE_HANDLE(obj));
    GCHandle prop_handle = js_obj_prop_handle(obj);
    gc_add_root(prop_handle);


    const int iters = 500;
    const int nthreads = 4;
    pthread_t threads[nthreads];
    PropArrayThreadArgs args[nthreads];

    for (int i = 0; i < nthreads; i++) {
        args[i].ctx = ctx;
        args[i].obj = SAFE_HANDLE(obj);
        args[i].iters = iters;
        pthread_create(&threads[i], NULL, prop_array_cas_worker, &args[i]);
    }
    for (int i = 0; i < nthreads; i++) {
        pthread_join(threads[i], NULL);
    }


    GCValue final_val = js_object_get_property_value_atomic(ctx, SAFE_HANDLE(obj), 0);
    ASSERT_EQ(JS_VALUE_GET_INT(final_val), nthreads * iters);

    gc_remove_root(prop_handle);
    gc_remove_root(SAFE_HANDLE(obj));
    return true;
}

TEST(test_class_id_atomic) {
    JSClassID id1 = 0, id2 = 0;
    JSClassID r1 = JS_NewClassID(&id1);
    JSClassID r2 = JS_NewClassID(&id2);
    ASSERT_TRUE(r1 != 0);
    ASSERT_TRUE(r2 != 0);
    ASSERT_TRUE(r1 != r2);
    ASSERT_EQ((int)id1, (int)r1);
    ASSERT_EQ((int)id2, (int)r2);
    return true;
}

TEST(test_class_array_atomic) {
    JSContextHandle ctx = get_test_ctx();
    JSRuntimeHandle rt = SAFE_GET_OR(ctx, JSContextHandle, rt_handle, GC_HANDLE_NULL);
    ASSERT_TRUE(SAFE_VALID(rt));

    /* Version must be stable (even) after init. */
    uint32_t ver = js_rt_class_array_version(rt);
    ASSERT_TRUE((ver & 1) == 0);

    /* Register a custom class and read the descriptor back through the
       atomic accessor (JS_IsRegisteredClass uses rt_class_array). */
    JSClassID class_id = 0;
    ASSERT_TRUE(JS_NewClassID(&class_id) != 0);
    JSClassDef class_def = {};
    class_def.class_name = "TestClassArrayAtomic";
    ASSERT_TRUE(JS_NewClass(rt, class_id, &class_def) == 0);
    ASSERT_TRUE(JS_IsRegisteredClass(rt, class_id));
    return true;
}

TEST(test_class_proto_atomic) {
    JSContextHandle ctx = get_test_ctx();

    /* Register a custom class so we have a free prototype slot to play with. */
    JSClassID class_id = 0;
    ASSERT_TRUE(JS_NewClassID(&class_id) != 0);
    JSClassDef class_def = {};
    class_def.class_name = "TestClassProtoAtomic";
    ASSERT_TRUE(JS_NewClass((JSRuntimeHandle)(SAFE_GET_OR(ctx, JSContextHandle, rt_handle, GC_HANDLE_NULL)), class_id, &class_def) == 0);

    GCValue proto = JS_NewObject(ctx);
    ASSERT_TRUE(JS_IsObject(proto));

    /* Atomically install and read back the class prototype. */
    js_ctx_class_proto_set(ctx, class_id, proto);
    GCValue got = js_ctx_class_proto_get(ctx, class_id);
    ASSERT_TRUE(JS_IsObject(got));
    ASSERT_EQ((int64_t)JS_VALUE_GET_HANDLE(got),
              (int64_t)JS_VALUE_GET_HANDLE(proto));

    ASSERT_TRUE((js_ctx_class_proto_version(ctx) & 1) == 0);
    return true;
}

typedef struct ProtoThreadArgs {
    JSContextHandle ctx;
    GCHandle obj;
    GCHandle proto_a;
    GCHandle proto_b;
    int iters;
} ProtoThreadArgs;

static void *proto_reader_worker(void *arg)
{
    ProtoThreadArgs *a = (ProtoThreadArgs *)arg;
    intptr_t errors = 0;
    for (int i = 0; i < a->iters; i++) {
        GCValue proto = JS_GetPrototype(a->ctx, GC_MKHANDLE(JS_TAG_OBJECT, a->obj));
        if (JS_IsNull(proto))
            continue;
        if (!JS_IsObject(proto)) {
            errors++;
            break;
        }
        GCHandle h = JS_VALUE_GET_HANDLE(proto);
        if (h != a->proto_a && h != a->proto_b) {
            errors++;
            break;
        }
    }
    return (void *)errors;
}

static void *proto_writer_worker(void *arg)
{
    ProtoThreadArgs *a = (ProtoThreadArgs *)arg;
    intptr_t errors = 0;
    for (int i = 0; i < a->iters; i++) {
        GCValue target = GC_MKHANDLE(JS_TAG_OBJECT, (i & 1) ? a->proto_b : a->proto_a);
        int ret = JS_SetPrototype(a->ctx, GC_MKHANDLE(JS_TAG_OBJECT, a->obj), target);
        if (ret != TRUE) {
            errors++;
            break;
        }
    }
    return (void *)errors;
}

TEST(test_object_prototype_atomic) {
    JSContextHandle ctx = get_test_ctx();

    GCValue a_val = JS_NewObjectProto(ctx, JS_NULL);
    GCValue b_val = JS_NewObjectProto(ctx, JS_NULL);
    GCValue obj_val = JS_NewObjectProto(ctx, JS_NULL);
    ASSERT_TRUE(JS_IsObject(a_val));
    ASSERT_TRUE(JS_IsObject(b_val));
    ASSERT_TRUE(JS_IsObject(obj_val));

    GCHandle a = JS_VALUE_GET_HANDLE(a_val);
    GCHandle b = JS_VALUE_GET_HANDLE(b_val);
    GCHandle obj = JS_VALUE_GET_HANDLE(obj_val);

    gc_add_root(a);
    gc_add_root(b);
    gc_add_root(obj);


    const int iters = 500;
    const int nreaders = 3;
    pthread_t threads[nreaders + 1];
    ProtoThreadArgs args[nreaders + 1];

    for (int i = 0; i < nreaders; i++) {
        args[i].ctx = ctx;
        args[i].obj = obj;
        args[i].proto_a = a;
        args[i].proto_b = b;
        args[i].iters = iters;
        pthread_create(&threads[i], NULL, proto_reader_worker, &args[i]);
    }
    args[nreaders].ctx = ctx;
    args[nreaders].obj = obj;
    args[nreaders].proto_a = a;
    args[nreaders].proto_b = b;
    args[nreaders].iters = iters;
    pthread_create(&threads[nreaders], NULL, proto_writer_worker, &args[nreaders]);

    intptr_t total_errors = 0;
    for (int i = 0; i <= nreaders; i++) {
        void *ret;
        pthread_join(threads[i], &ret);
        total_errors += (intptr_t)ret;
    }

    gc_remove_root(obj);
    gc_remove_root(b);
    gc_remove_root(a);

    ASSERT_EQ((int64_t)0, (int64_t)total_errors);
    return true;
}

/* ============================================================================
 * Lock-free job queue tests
 * ============================================================================ */

#define JOB_QUEUE_MAX_PRODUCERS 8
static uint32_t g_job_counters[JOB_QUEUE_MAX_PRODUCERS];
static volatile uint32_t g_job_producers_done;
static volatile uint64_t g_job_consumed_total;

static GCValue test_job_counter_func(JSContextHandle ctx, int argc, GCValue *argv)
{
    (void)ctx;
    if (argc > 0) {
        int id = JS_VALUE_GET_INT(argv[0]);
        if (id >= 0 && id < JOB_QUEUE_MAX_PRODUCERS)
            atomic_fetch_add_u32(&g_job_counters[id], 1);
    }
    return JS_UNDEFINED;
}

TEST(test_job_queue_basic) {
    JSContextHandle ctx = get_test_ctx();
    JSRuntimeHandle rt = SAFE_GET_OR(ctx, JSContextHandle, rt_handle, GC_HANDLE_NULL);

    /* Drain any leftover jobs from previous tests before counting. */
    for (;;) {
        int ret = JS_ExecutePendingJob(rt, NULL);
        if (ret == 0) break;
    }

    memset(g_job_counters, 0, sizeof(g_job_counters));

    const int njobs = 10;
    for (int i = 0; i < njobs; i++) {
        GCValue argv[1] = { JS_NewInt32(ctx, 0) };
        ASSERT_TRUE(JS_EnqueueJob(ctx, test_job_counter_func, 1, argv) == 0);
    }

    int executed = 0;
    for (;;) {
        int ret = JS_ExecutePendingJob(rt, NULL);
        if (ret == 0)
            break;
        ASSERT_TRUE(ret == 1);
        executed++;
    }

    ASSERT_EQ(executed, njobs);
    ASSERT_EQ((int)g_job_counters[0], njobs);
    return true;
}

TEST(test_job_queue_gc_marking) {
    JSContextHandle ctx = get_test_ctx();
    JSRuntimeHandle rt = SAFE_GET_OR(ctx, JSContextHandle, rt_handle, GC_HANDLE_NULL);

    memset(g_job_counters, 0, sizeof(g_job_counters));

    for (int i = 0; i < 5; i++) {
        GCValue argv[1] = { JS_NewInt32(ctx, 0) };
        ASSERT_TRUE(JS_EnqueueJob(ctx, test_job_counter_func, 1, argv) == 0);
    }

    /* Run GC; the queued jobs must remain reachable. */
    JS_RunGC(rt);

    int executed = 0;
    for (;;) {
        int ret = JS_ExecutePendingJob(rt, NULL);
        if (ret == 0)
            break;
        ASSERT_TRUE(ret == 1);
        executed++;
    }

    ASSERT_EQ(executed, 5);
    ASSERT_EQ((int)g_job_counters[0], 5);
    return true;
}

/* ============================================================================
 * Heap and handles
 * ============================================================================ */

/* Run with the background collector stopped, for tests that look at
 * addresses and counts the GC thread would otherwise change under them. */
typedef struct BgState { GCConfig saved; } BgState;
static void bg_pause(BgState *st) {
    gc_get_config(&st->saved);
    GCConfig c = st->saved;
    c.background = false;
    gc_set_config(&c);
}
static void bg_resume(BgState *st) { gc_set_config(&st->saved); }

/* Overwrite the stack below the caller so stale copies of handles in dead
 * frames do not keep objects alive in the conservative stack scan. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void clobber_stack(void) {
    volatile uint8_t buf[32 * 1024];
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
}

#define HIDE_MASK 0x5a5a5a5au

/* Allocate n unreachable objects, remembering their handles only in a
 * masked form the stack scan cannot recognise. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void alloc_hidden(uint32_t *hidden, int n, size_t size) {
    for (int i = 0; i < n; i++) {
        GCHandle h = gc_alloc(size, JS_GC_OBJ_TYPE_DATA);
        uint8_t *p = (uint8_t *)gc_deref(h);
        if (p) memset(p, 0xab, size);
        hidden[i] = h ^ HIDE_MASK;
    }
}

TEST(test_gc_header_is_8_bytes) {
    ASSERT_EQ(8, (int)sizeof(GCHeader));
    ASSERT_EQ(8, (int)GC_HEADER_SIZE);
    /* Every payload is 16-byte aligned and the header in front of it
     * records the handle and a size that covers the request. */
    for (size_t size = 1; size <= 300; size += 7) {
        GCHandle h = gc_alloc(size, JS_GC_OBJ_TYPE_DATA);
        ASSERT_TRUE(h != GC_HANDLE_NULL);
        uint8_t *p = (uint8_t *)gc_deref(h);
        ASSERT_TRUE(p != NULL);
        ASSERT_EQ(0, (int)((uintptr_t)p & 15));
        GCHeader *hdr = gc_header(p);
        ASSERT_EQ((int)h, (int)hdr->handle);
        ASSERT_EQ((int)JS_GC_OBJ_TYPE_DATA, (int)gc_header_type(hdr));
        ASSERT_TRUE(gc_header_payload_size(hdr) >= size);
        ASSERT_TRUE(gc_header_payload_size(hdr) < size + 16);
        ASSERT_EQ(0, (int)(gc_header_total_size(hdr) & 15));
        for (size_t k = 0; k < size; k++) ASSERT_EQ(0, p[k]);   /* zeroed */
        gc_free(h);
    }
    return true;
}

/* Atoms live in permanent regions: no cycle includes them, so marking never
 * visits them, and they survive every kind of collection. */
TEST(test_gc_atoms_are_permanent) {
    JSContextHandle ctx = get_gc_test_ctx();
    if (!SAFE_VALID(ctx)) return true;
    JSAtom a = JS_NewAtom(ctx, "__gc_permanent_atom_test");
    ASSERT_TRUE(a != JS_ATOM_NULL);
    /* An atom made from an existing (young) string is copied, not reused. */
    GCValue str = JS_NewString(ctx, "__gc_atom_from_string");
    JSAtom b = JS_ValueToAtom(ctx, str);
    ASSERT_TRUE(b != JS_ATOM_NULL);
    GCValue sa = JS_AtomToString(ctx, a), sb = JS_AtomToString(ctx, b);
    GCHeader *ha = gc_header(gc_deref(JS_VALUE_GET_HANDLE(sa)));
    GCHeader *hb = gc_header(gc_deref(JS_VALUE_GET_HANDLE(sb)));
    ASSERT_TRUE(ha && hb);
    ASSERT_TRUE(gc_core_region_of(ha)->permanent);
    ASSERT_TRUE(gc_core_region_of(hb)->permanent);
    gc_run();
    ASSERT_TRUE(eval_expect_ok(ctx,
        "(function(){ var o = {}; o['__gc_permanent_atom_test'] = 1; o.__gc_atom_from_string = 2;"
        "  return o.__gc_permanent_atom_test + o['__gc_atom_from_string'] === 3 ? 'ok' : 'atoms lost'; })()"));
    return true;
}

TEST(test_gc_ptr_to_handle_interior) {
    GCHandle h = gc_alloc(200, JS_GC_OBJ_TYPE_DATA);
    uint8_t *p = (uint8_t *)gc_deref(h);
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ((int)h, (int)gc_ptr_to_handle(p));
    ASSERT_EQ((int)h, (int)gc_ptr_to_handle(p + 100));
    ASSERT_EQ((int)h, (int)gc_ptr_to_handle(p + 199));
    ASSERT_TRUE(gc_ptr_in_heap(p));
    int local = 0;
    ASSERT_EQ((int)GC_HANDLE_NULL, (int)gc_ptr_to_handle(&local));
    gc_free(h);
    return true;
}

TEST(test_gc_humongous_object_never_moves) {
    BgState st;
    bg_pause(&st);
    size_t size = GC_HUMONGOUS_MIN + 12345;
    GCHandle h = gc_alloc(size, JS_GC_OBJ_TYPE_DATA);
    ASSERT_TRUE(h != GC_HANDLE_NULL);
    gc_add_root(h);
    uint8_t *p = (uint8_t *)gc_deref(h);
    ASSERT_TRUE(p != NULL);
    GCRegion *r = gc_core_region_of(p);
    ASSERT_EQ(GC_RS_HUMONGOUS, (int)atomic_load(&r->state));
    p[0] = 1; p[size - 1] = 2;
    uintptr_t before = (uintptr_t)p;
    gc_run();
    gc_run();
    p = (uint8_t *)gc_deref(h);
    ASSERT_EQ(before, (uintptr_t)p);
    ASSERT_EQ(1, p[0]);
    ASSERT_EQ(2, p[size - 1]);
    gc_remove_root(h);
    gc_run();
    ASSERT_EQ(0, (int)gc_test_heap_check());
    bg_resume(&st);
    return true;
}

/* Objects never move: a realloc is a new object with the old contents; the
 * old one stays, valid, until the collector finds it unreachable. */
TEST(test_gc_realloc_copies_into_a_new_object) {
    BgState st;
    bg_pause(&st);
    GCHandle h = gc_alloc(64, JS_GC_OBJ_TYPE_DATA);
    uint8_t *p = (uint8_t *)gc_deref(h);
    for (int i = 0; i < 64; i++) p[i] = (uint8_t)i;
    GCHandle h2 = gc_realloc(h, 5000);
    ASSERT_TRUE(h2 != GC_HANDLE_NULL);
    ASSERT_TRUE(h2 != h);
    ASSERT_TRUE(gc_handle_live(h));
    p = (uint8_t *)gc_deref(h2);
    for (int i = 0; i < 64; i++) ASSERT_EQ(i, p[i]);
    ASSERT_TRUE(gc_handle_get_size(h2) >= 5000);
    GCHandle h3 = gc_realloc(h2, 16);
    gc_free(h);
    gc_free(h2);
    p = (uint8_t *)gc_deref(h3);
    for (int i = 0; i < 16; i++) ASSERT_EQ(i, p[i]);
    gc_free(h3);
    bg_resume(&st);
    return true;
}

/* A handle is its object's address: freed, it dereferences to NULL, and
 * the memory waits for a sweep to make it a hole before it is reused. */
TEST(test_gc_free_kills_handle) {
    BgState st;
    bg_pause(&st);
    GCHandle h = gc_alloc(32, JS_GC_OBJ_TYPE_DATA);
    ASSERT_TRUE(gc_handle_of_header(gc_header(gc_deref(h))) == h);
    gc_free(h);
    ASSERT_TRUE(!gc_handle_live(h));
    gc_free(h);   /* double free is ignored */
    GCHandle h2 = gc_alloc(32, JS_GC_OBJ_TYPE_DATA);
    ASSERT_TRUE(h2 != h);   /* not reused before a sweep */
    ASSERT_TRUE(!gc_handle_live(h));
    gc_free(h2);
    bg_resume(&st);
    return true;
}

/* ============================================================================
 * Stop-the-world collection
 * ============================================================================ */

TEST(test_gc_unreachable_objects_freed) {
    BgState st;
    bg_pause(&st);
    uint32_t hidden[64];
    alloc_hidden(hidden, 64, 48);
    clobber_stack();
    gc_run();
    int freed = 0;
    for (int i = 0; i < 64; i++)
        if (!gc_handle_live(hidden[i] ^ HIDE_MASK)) freed++;
    /* A few may survive through stale words the conservative scan sees. */
    ASSERT_TRUE(freed >= 56);
    ASSERT_EQ(0, (int)gc_test_heap_check());
    bg_resume(&st);
    return true;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void alloc_rooted_hidden(uint32_t *hidden, int n) {
    for (int i = 0; i < n; i++) {
        GCHandle h = gc_alloc(40, JS_GC_OBJ_TYPE_DATA);
        memset(gc_deref(h), 0x42, 40);
        gc_add_root(h);
        hidden[i] = h ^ HIDE_MASK;
    }
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static int count_rooted_intact(const uint32_t *hidden, int n) {
    int ok = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *p = (const uint8_t *)gc_deref(hidden[i] ^ HIDE_MASK);
        if (p && p[0] == 0x42 && p[39] == 0x42) ok++;
    }
    return ok;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void unroot_hidden(const uint32_t *hidden, int n) {
    for (int i = 0; i < n; i++) gc_remove_root(hidden[i] ^ HIDE_MASK);
}

TEST(test_gc_roots_keep_objects) {
    BgState st;
    bg_pause(&st);
    uint32_t hidden[32];
    alloc_rooted_hidden(hidden, 32);
    clobber_stack();
    gc_run();
    ASSERT_EQ(32, count_rooted_intact(hidden, 32));   /* all kept, contents intact */
    unroot_hidden(hidden, 32);
    clobber_stack();
    gc_run();
    int freed = 0;
    for (int i = 0; i < 32; i++)
        if (!gc_handle_live(hidden[i] ^ HIDE_MASK)) freed++;
    ASSERT_TRUE(freed >= 28);   /* stale stack words may keep a few */
    bg_resume(&st);
    return true;
}

static int g_finalized[64];
static uint32_t g_fin_hidden[64];
static void count_finalizer(JSRuntimeHandle rt, GCHandle h, void *payload) {
    (void)rt; (void)payload;
    for (int i = 0; i < 64; i++)
        if ((g_fin_hidden[i] ^ HIDE_MASK) == h) g_finalized[i]++;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void alloc_with_finalizers(void) {
    for (int i = 0; i < 64; i++) {
        GCHandle h = gc_alloc(24, JS_GC_OBJ_TYPE_DATA);
        gc_set_handle_finalizer(h, count_finalizer);
        g_fin_hidden[i] = h ^ HIDE_MASK;
    }
}

TEST(test_gc_finalizers_run_once) {
    BgState st;
    bg_pause(&st);
    memset(g_finalized, 0, sizeof(g_finalized));
    alloc_with_finalizers();
    clobber_stack();
    gc_run();
    gc_run();
    int ran = 0;
    for (int i = 0; i < 64; i++) {
        ASSERT_TRUE(g_finalized[i] <= 1);
        if (g_finalized[i] == 1) {
            ran++;
            ASSERT_TRUE(!gc_handle_live(g_fin_hidden[i] ^ HIDE_MASK));
        }
    }
    ASSERT_TRUE(ran >= 56);
    bg_resume(&st);
    return true;
}

/* Fill old regions, free most objects and keep every `stride`th, collect.
 * Returns false on a failed check.  Nothing moves: the survivors stay put
 * and, with hole reuse, the free lines between them take new objects. */
static bool sparse_regions_case(int stride) {
    BgState st;
    bg_pause(&st);
    enum { N = 20000 };
    static GCHandle keep[N];
    static void *where[N];
    int nk = 0;
    for (int i = 0; i < N; i++) {
        GCHandle h = gc_alloc_old(100, JS_GC_OBJ_TYPE_DATA);
        uint32_t *p = (uint32_t *)gc_deref(h);
        p[0] = (uint32_t)i;
        p[24] = (uint32_t)i * 7u;
        if (i % stride == 0) {
            where[nk] = p;
            keep[nk++] = h;
            gc_add_root(h);
        } else {
            gc_free(h);
        }
    }
    GCStats s0, s1;
    gc_get_stats(&s0);
    gc_run();
    gc_get_stats(&s1);
    ASSERT_EQ((int)s0.objects_copied, (int)s1.objects_copied);
    if (g_core.holes) {
        int reused = 0;
        for (int i = 0; i < N / 4; i++) {
            GCHandle h = gc_alloc_old(100, JS_GC_OBJ_TYPE_DATA);
            /* Below its region's top: in a hole, not bump-allocated. */
            void *at = gc_deref(h);
            if ((uintptr_t)at < atomic_load(&gc_core_region_of(at)->top)) reused++;
            gc_free(h);
        }
        ASSERT_TRUE(reused > N / 8);
    }
    for (int k = 0; k < nk; k++) {
        uint32_t *p = (uint32_t *)gc_deref(keep[k]);
        ASSERT_TRUE(p != NULL);
        ASSERT_TRUE((void *)p == where[k]);
        ASSERT_EQ(k * stride, (int)p[0]);
        ASSERT_EQ((uint32_t)(k * stride) * 7u, p[24]);
        gc_remove_root(keep[k]);
    }
    ASSERT_EQ(0, (int)gc_test_heap_check());
    bg_resume(&st);
    return true;
}

/* A tenth live, and a third. */
TEST(test_gc_sparse_regions_keep_survivors) {
    return sparse_regions_case(10);
}

TEST(test_gc_denser_regions_keep_survivors) {
    return sparse_regions_case(3);
}

/* An object a raw pointer on the stack points into is pinned: compaction
 * leaves it where it is. */
TEST(test_gc_raw_pointer_pins_object) {
    BgState st;
    bg_pause(&st);
    /* A sparse region, so it would be compacted if not pinned. */
    GCHandle target = GC_HANDLE_NULL;
    for (int i = 0; i < 4000; i++) {
        GCHandle h = gc_alloc(200, JS_GC_OBJ_TYPE_DATA);
        if (i == 2000) target = h;
        else gc_free(h);
    }
    volatile uint8_t *raw = (volatile uint8_t *)gc_deref(target) + 50;
    raw[0] = 0x77;
    gc_run();
    ASSERT_TRUE((uint8_t *)gc_deref(target) + 50 == (uint8_t *)raw);
    ASSERT_EQ(0x77, raw[0]);
    gc_free(target);
    bg_resume(&st);
    return true;
}

/* ============================================================================
 * Dirty tracking
 * ============================================================================ */

TEST(test_gc_write_helpers_set_dirty_flags) {
#if GC_SOFTWARE_DIRTY
    BgState st;
    bg_pause(&st);
    GCHandle h = gc_alloc(4 * GC_CARD_SIZE, JS_GC_OBJ_TYPE_DATA);
    GCValue *slots = (GCValue *)gc_deref(h);
    uintptr_t base = (uintptr_t)g_gc.heap_base;
    size_t n = 4 * GC_CARD_SIZE / sizeof(GCValue);
    /* Clear the cards of the object, then store through each helper. */
    for (uintptr_t a = (uintptr_t)slots; a < (uintptr_t)(slots + n); a += GC_CARD_SIZE)
        g_gc.cards[(a - base) >> GC_CARD_SHIFT] = 0;
    g_gc.cards[((uintptr_t)(slots + n - 1) - base) >> GC_CARD_SHIFT] = 0;

    GCValue v = JS_NewInt32(GC_HANDLE_NULL, 5);
    GC_SET(slots[3], v);
    ASSERT_EQ(1, g_gc.cards[((uintptr_t)&slots[3] - base) >> GC_CARD_SHIFT]);
    size_t far_idx = n - 2;
    ASSERT_EQ(0, g_gc.cards[((uintptr_t)&slots[far_idx] - base) >> GC_CARD_SHIFT]);
    gc_memcpy(&slots[far_idx], &v, sizeof(v));
    ASSERT_EQ(1, g_gc.cards[((uintptr_t)&slots[far_idx] - base) >> GC_CARD_SHIFT]);

    /* Off-heap stores do nothing (and do not crash). */
    GCValue local;
    GC_SET(local, v);
    gc_dirty_slot(&local);
    gc_dirty_range(&local, sizeof(local));
    gc_free(h);
    bg_resume(&st);
#endif
    return true;
}

static uint32_t g_ww_pages;
static void count_page(void *page, void *opaque) { (void)page; (void)opaque; g_ww_pages++; }

/* The OS layer's write watch (Windows).  Elsewhere the call must report
 * that it is unavailable. */
TEST(test_gc_os_write_watch) {
    if (!gc_os_has_write_watch()) {
        ASSERT_EQ(1, GC_SOFTWARE_DIRTY);
        return true;
    }
    size_t size = 16 * GC_OS_PAGE_SIZE;
    uint8_t *mem = (uint8_t *)gc_os_reserve(size, true);
    ASSERT_TRUE(mem != NULL);
    ASSERT_TRUE(gc_os_commit(mem, size));
    gc_os_write_watch_reset(mem, size);
    mem[0] = 1;
    mem[5 * GC_OS_PAGE_SIZE + 9] = 1;
    g_ww_pages = 0;
    ASSERT_TRUE(gc_os_write_watch_pull(mem, size, count_page, NULL));
    ASSERT_EQ(2, (int)g_ww_pages);
    /* The pull reset the state. */
    g_ww_pages = 0;
    ASSERT_TRUE(gc_os_write_watch_pull(mem, size, count_page, NULL));
    ASSERT_EQ(0, (int)g_ww_pages);
    gc_os_release(mem, size);
    return true;
}

/* ============================================================================
 * Concurrent cycles
 * ============================================================================ */

/* Run the concurrent collector with a given configuration for the length
 * of one test. */
typedef struct ConcState { GCConfig saved; GCStats before; } ConcState;
static bool conc_begin(ConcState *st, uint32_t stress, uint32_t reject, size_t nursery_kb) {
    gc_get_config(&st->saved);
    GCConfig c = st->saved;
    c.background = true;
    c.nursery = true;
    c.stress = stress;
    c.reject = reject;
    c.verify = true;
    if (nursery_kb) c.nursery_size = nursery_kb << 10;
    gc_set_config(&c);
    gc_get_stats(&st->before);
    return gc_is_background();
}
static bool conc_end(ConcState *st, GCStats *after) {
    GCConfig c;
    gc_get_config(&c);
    c.stress = 0;
    gc_set_config(&c);
    bool idle = gc_wait_idle(20000);
    gc_get_stats(after);
    gc_set_config(&st->saved);
    return idle;
}

TEST(test_gc_concurrent_major_cycle) {
    JSContextHandle ctx = get_gc_test_ctx();
    if (!SAFE_VALID(ctx)) return true;
    ConcState st;
    ASSERT_TRUE(conc_begin(&st, 0, GC_REJECT_NORMAL, 0));
    ASSERT_TRUE(eval_expect_ok(ctx,
        "globalThis.__cm = []; for (var i = 0; i < 5000; i++) __cm.push({i: i, s: 'v' + i}); 'ok'"));
    gc_request_cycle(true);
    ASSERT_TRUE(gc_wait_idle(20000));
    GCStats s;
    ASSERT_TRUE(conc_end(&st, &s));
    ASSERT_TRUE(s.major_cycles > st.before.major_cycles);
    ASSERT_EQ(st.before.verify_failures, s.verify_failures);
    ASSERT_TRUE(eval_expect_ok(ctx,
        "(function(){ for (var i = 0; i < 5000; i++) if (__cm[i].i !== i || __cm[i].s !== 'v' + i) return 'lost ' + i;"
        " __cm = null; return 'ok'; })()"));
    ASSERT_EQ(0, (int)gc_test_heap_check());
    return true;
}

/* Async functions resuming and suspending while concurrent cycles run:
 * each resume stores fresh objects in the frame (no write barrier: the
 * interpreter writes frame slots directly) and suspends again, so a frame
 * the collector scanned while suspended must be seen again before the
 * cycle frees anything it holds. */
TEST(test_gc_async_frames_during_concurrent_cycles) {
    JSContextHandle ctx = get_gc_test_ctx();
    if (!SAFE_VALID(ctx)) return true;
    ConcState st;
    ASSERT_TRUE(conc_begin(&st, 1, GC_REJECT_NORMAL, 256));
    ASSERT_TRUE(eval_expect_ok(ctx,
        "globalThis.__aw = { done: 0, bad: '' };"
        "for (var w = 0; w < 16; w++) (async function (w) {"
        "  for (var i = 0; i < 300; i++) {"
        "    var o = { w: w, i: i, s: 'w' + w + 'i' + i, a: [i, i + 1] };"
        "    var t = 'tmp' + i;"
        "    await null;"
        "    var keep = [o, t];"
        "    await null;"
        "    if (o.i !== i || o.s !== 'w' + w + 'i' + i || o.a[1] !== i + 1 || t !== 'tmp' + i || keep[0] !== o)"
        "      { __aw.bad = 'worker ' + w + ' lost round ' + i; return; }"
        "  }"
        "  __aw.done++;"
        "})(w); 'ok'"));
    JSContextHandle pctx;
    for (int guard = 0; guard < 2000000; guard++) {
        int r = JS_ExecutePendingJob(JS_GetRuntime(ctx), &pctx);
        if (r <= 0) break;
        if ((guard & 63) == 0) gc_tick();
    }
    GCStats s;
    ASSERT_TRUE(conc_end(&st, &s));
    ASSERT_EQ(st.before.verify_failures, s.verify_failures);
    ASSERT_TRUE(eval_expect_ok(ctx,
        "__aw.bad ? __aw.bad : __aw.done === 16 ? 'ok' : 'only ' + __aw.done + ' finished'"));
    ASSERT_EQ(0, (int)gc_test_heap_check());
    return true;
}

/* A WeakMap keeps a value alive for as long as its key is (an ephemeron):
 * values reachable only through the map must survive full and concurrent
 * cycles while their keys live.  url_api.js keeps each URL's state this
 * way; a lost state crashed in URL.toString. */
static const char *k_weakmap_setup =
    "globalThis.__wm = new WeakMap(); globalThis.__wk = [];"
    "for (var i = 0; i < 3000; i++) { var k = { id: i }; __wk.push(k);"
    "  __wm.set(k, { s: 'v' + i, inner: { n: i, t: 'inner' + i } }); }"
    "'ok'";
static const char *k_weakmap_check =
    "(function(){ for (var i = 0; i < 3000; i++) { var v = __wm.get(__wk[i]);"
    "  if (!v || v.s !== 'v' + i || v.inner.n !== i || v.inner.t !== 'inner' + i) return 'lost value ' + i; }"
    " __wm = null; __wk = null; return 'ok'; })()";

/* A WeakMap whose key dies drops the entry, but its value stays alive while
 * anything else holds it: the fetch polyfill reads a Request's state with
 * reqState.get(new Request(...)), so the key dies at once while the state
 * (and its url string) is still in use. */
static const char *k_weakmap_dead_key_setup =
    "globalThis.__wv = []; globalThis.__wm2 = new WeakMap();"
    "for (var i = 0; i < 3000; i++) { var v = { u: 'url' + i, nested: { n: i, s: 'n' + i } };"
    "  __wm2.set({ key: i }, v); __wv.push(v); }"
    "'ok'";
static const char *k_weakmap_dead_key_check =
    "(function(){ for (var i = 0; i < 3000; i++) { var v = __wv[i];"
    "  if (!v || v.u !== 'url' + i || v.nested.n !== i || v.nested.s !== 'n' + i) return 'lost held value ' + i; }"
    " __wv = null; __wm2 = null; return 'ok'; })()";

TEST(test_gc_weakmap_dead_key_value_survives_full_cycles) {
    JSContextHandle ctx = get_gc_test_ctx();
    if (!SAFE_VALID(ctx)) return true;
    ASSERT_TRUE(eval_expect_ok(ctx, k_weakmap_dead_key_setup));
    for (int i = 0; i < 3; i++) {
        eval_test(ctx, "(function(){ var g = []; for (var i = 0; i < 20000; i++) g.push({ x: 'g' + i }); return 1; })()");
        JS_RunGC(JS_GetRuntime(ctx));
    }
    ASSERT_TRUE(eval_expect_ok(ctx, k_weakmap_dead_key_check));
    ASSERT_EQ(0, (int)gc_test_heap_check());
    return true;
}

TEST(test_gc_weakmap_dead_key_value_survives_concurrent_cycles) {
    JSContextHandle ctx = get_gc_test_ctx();
    if (!SAFE_VALID(ctx)) return true;
    ConcState st;
    ASSERT_TRUE(conc_begin(&st, 1, GC_REJECT_NORMAL, 256));
    ASSERT_TRUE(eval_expect_ok(ctx, k_weakmap_dead_key_setup));
    for (int round = 0; round < 200; round++) {
        eval_test(ctx, "(function(){ var g = []; for (var i = 0; i < 2000; i++) g.push({ x: 'g' + i }); return 1; })()");
        gc_tick();
    }
    GCStats s;
    ASSERT_TRUE(conc_end(&st, &s));
    ASSERT_TRUE(eval_expect_ok(ctx, k_weakmap_dead_key_check));
    ASSERT_EQ(0, (int)gc_test_heap_check());
    return true;
}

/* A property name read back as a string (Object.keys, for...in) stays a
 * valid string after the property is deleted and the atom has no other
 * use: YouTube's icon cache queues fetches keyed by URL, takes the key with
 * Object.keys, deletes the entry and then fetch()es the key. */
GC_RUNNING_TEST(test_gc_property_key_string_outlives_delete,
    "(function(){ var keys = [];"
    "  for (var i = 0; i < 500; i++) {"
    "    var o = {}; o['https://example.test/icon/' + i + '/' + Math.random()] = i;"
    "    var k = Object.keys(o)[0]; delete o[k]; keys.push(k);"
    "  }"
    "  __test_gc(); var g = []; for (var j = 0; j < 20000; j++) g.push({ x: 'g' + j }); __test_gc();"
    "  for (var i = 0; i < 500; i++) {"
    "    var k = keys[i];"
    "    if (typeof k !== 'string' || k.indexOf('https://example.test/icon/' + i + '/') !== 0) return 'key ' + i + ' lost';"
    "  }"
    "  return 'ok'; })()")

TEST(test_gc_weakmap_values_survive_full_cycles) {
    JSContextHandle ctx = get_gc_test_ctx();
    if (!SAFE_VALID(ctx)) return true;
    ASSERT_TRUE(eval_expect_ok(ctx, k_weakmap_setup));
    for (int i = 0; i < 3; i++) {
        eval_test(ctx, "(function(){ var g = []; for (var i = 0; i < 20000; i++) g.push({ x: 'g' + i }); return 1; })()");
        JS_RunGC(JS_GetRuntime(ctx));
    }
    ASSERT_TRUE(eval_expect_ok(ctx, k_weakmap_check));
    ASSERT_EQ(0, (int)gc_test_heap_check());
    return true;
}

TEST(test_gc_weakmap_values_survive_concurrent_cycles) {
    JSContextHandle ctx = get_gc_test_ctx();
    if (!SAFE_VALID(ctx)) return true;
    ConcState st;
    ASSERT_TRUE(conc_begin(&st, 1, GC_REJECT_NORMAL, 256));
    ASSERT_TRUE(eval_expect_ok(ctx, k_weakmap_setup));
    for (int round = 0; round < 200; round++) {
        eval_test(ctx, "(function(){ var g = []; for (var i = 0; i < 2000; i++) g.push({ x: 'g' + i }); return 1; })()");
        gc_tick();
    }
    GCStats s;
    ASSERT_TRUE(conc_end(&st, &s));
    ASSERT_TRUE(eval_expect_ok(ctx, k_weakmap_check));
    ASSERT_EQ(0, (int)gc_test_heap_check());
    return true;
}

/* Young objects (unmarked) that survive a minor cycle become old (marked)
 * where they are: nothing moves. */
TEST(test_gc_minor_cycle_promotes_survivors) {
    JSContextHandle ctx = get_gc_test_ctx();
    if (!SAFE_VALID(ctx)) return true;
    ConcState st;
    ASSERT_TRUE(conc_begin(&st, 0, GC_REJECT_NORMAL, 0));
    GCValue r = eval_test(ctx, "globalThis.__young = {tag: 'young-object'}; __young");
    ASSERT_TRUE(is_exception_free(ctx, r));
    GCHandle h = JS_VALUE_GET_HANDLE(r);
    void *at = gc_deref(h);
    GCHeader *hdr = gc_header(at);
    ASSERT_TRUE(!gc_col_is_marked(gc_core_region_of(hdr), hdr));
    gc_request_cycle(false);
    ASSERT_TRUE(gc_wait_idle(20000));
    GCStats s;
    ASSERT_TRUE(conc_end(&st, &s));
    ASSERT_TRUE(s.minor_cycles > st.before.minor_cycles);
    ASSERT_TRUE(gc_deref(h) == at);
    ASSERT_TRUE(gc_col_is_marked(gc_core_region_of(hdr), hdr));
    ASSERT_TRUE(eval_expect_ok(ctx, "__young.tag === 'young-object' ? (__young = null, 'ok') : 'lost'"));
    ASSERT_EQ(st.before.verify_failures, s.verify_failures);
    return true;
}

/* Old objects that gain references to young ones (the remembered set),
 * including through property arrays, array storage and closures. */
static const char *k_remset_script =
    "(function(){"
    "  var old = globalThis.__old || (globalThis.__old = {arr: [], map: new Map(), fns: []});"
    "  for (var round = 0; round < 60; round++) {"
    "    for (var i = 0; i < 300; i++) {"
    "      var y = {round: round, i: i, s: 'r' + round + '_' + i};"
    "      if (i % 50 === 0) { old.arr.push(y); old.map.set(y.s, y); }"
    "      if (i === 7) old.fns.push((function(c){ return function(){ return c.s; }; })(y));"
    "      old['p' + (i % 20)] = y;"
    "    }"
    "  }"
    "  for (var k = 0; k < old.arr.length; k++) {"
    "    var o = old.arr[k];"
    "    if (o.s !== 'r' + o.round + '_' + o.i) return 'array entry ' + k + ' corrupted';"
    "    var mo = old.map.get(o.s); if (!mo || mo.s !== o.s) return 'map entry ' + k + ' lost';"
    "  }"
    "  for (var f = 0; f < old.fns.length; f++) if (old.fns[f]() !== 'r' + (f % 60) + '_7') return 'closure ' + f;"
    "  for (var p = 0; p < 20; p++) if (old['p' + p].i % 20 !== p) return 'property p' + p;"
    "  return 'ok';"
    "})()";

TEST(test_gc_remembered_set_keeps_young_objects) {
    JSContextHandle ctx = get_gc_test_ctx();
    if (!SAFE_VALID(ctx)) return true;
    ConcState st;
    ASSERT_TRUE(conc_begin(&st, 1, GC_REJECT_NORMAL, 64));
    /* Promote the container first, so later stores are old-to-young. */
    ASSERT_TRUE(eval_expect_ok(ctx, "globalThis.__old = {arr: [], map: new Map(), fns: []}; 'ok'"));
    gc_request_cycle(false);
    for (int i = 0; i < 4; i++) ASSERT_TRUE(eval_expect_ok(ctx, k_remset_script));
    GCStats s;
    ASSERT_TRUE(conc_end(&st, &s));
    ASSERT_TRUE(eval_expect_ok(ctx, k_remset_script));
    ASSERT_TRUE(eval_expect_ok(ctx, "__old = null, 'ok'"));
    ASSERT_TRUE(s.minor_cycles > st.before.minor_cycles);
    ASSERT_EQ(st.before.verify_failures, s.verify_failures);
    ASSERT_EQ(0, (int)gc_test_heap_check());
    return true;
}

/* A mixed workload while cycles run back to back: every boundary step
 * happens at an interpreter poll, every cycle is verified. */
static const char *k_workload_script =
    "(function(){"
    "  var acc = [], m = new Map(), s = new Set(), sum = 0;"
    "  function* gen(n){ for (var i = 0; i < n; i++) yield {v: i}; }"
    "  for (var r = 0; r < 40; r++) {"
    "    var tmp = [];"
    "    for (var i = 0; i < 400; i++) tmp.push({r: r, i: i, s: String(i) + ':' + r, a: [i, i * 2]});"
    "    acc.push(tmp[r * 7 % 400]);"
    "    m.set('k' + r, tmp[3]); s.add(tmp[5]);"
    "    for (var g of gen(20)) sum += g.v;"
    "    var str = ''; for (var c = 0; c < 50; c++) str += c; sum += str.length;"
    "    var big = new Array(2000).fill(r); sum += big[1999];"
    "    JSON.parse(JSON.stringify(tmp.slice(0, 20)));"
    "  }"
    "  for (var k = 0; k < acc.length; k++) {"
    "    var o = acc[k];"
    "    if (o.r !== k || o.s !== String(o.i) + ':' + k || o.a[1] !== o.i * 2) return 'acc ' + k + ': ' + JSON.stringify(o);"
    "    if (m.get('k' + k).i !== 3) return 'map ' + k;"
    "  }"
    "  if (s.size !== 40) return 'set ' + s.size;"
    "  var expect = 40 * 190 + 40 * 90 + 39 * 40 / 2;"
    "  return sum === expect ? 'ok' : 'sum ' + sum + ' expected ' + expect;"
    "})()";

TEST(test_gc_concurrent_workload_stress) {
    JSContextHandle ctx = get_gc_test_ctx();
    if (!SAFE_VALID(ctx)) return true;
    ConcState st;
    ASSERT_TRUE(conc_begin(&st, 1, GC_REJECT_NORMAL, 128));
    /* A major cycle runs across the workload whatever old space earlier
     * tests left behind (its trigger is growth, which depends on them). */
    for (int i = 0; i < 3; i++) {
        ASSERT_TRUE(eval_expect_ok(ctx, k_workload_script));
        if (i == 0) gc_request_cycle(true);
    }
    GCStats s;
    ASSERT_TRUE(conc_end(&st, &s));
    ASSERT_TRUE(s.minor_cycles > st.before.minor_cycles);
    ASSERT_TRUE(s.major_cycles > st.before.major_cycles);
    ASSERT_TRUE(s.boundary_steps > st.before.boundary_steps);
    ASSERT_EQ(st.before.verify_failures, s.verify_failures);
    ASSERT_EQ(0, (int)gc_test_heap_check());
    return true;
}

/* gc_run() in the middle of concurrent cycles abandons them cleanly. */
TEST(test_gc_full_collection_aborts_cycle) {
    JSContextHandle ctx = get_gc_test_ctx();
    if (!SAFE_VALID(ctx)) return true;
    ConcState st;
    ASSERT_TRUE(conc_begin(&st, 1, GC_REJECT_NORMAL, 128));
    ASSERT_TRUE(eval_expect_ok(ctx,
        "(function(){ var keep = [];"
        "  for (var r = 0; r < 30; r++) {"
        "    for (var i = 0; i < 300; i++) keep.push({r: r, i: i});"
        "    if (r % 3 === 0) __test_gc();"
        "    keep = keep.filter(function(o){ return o.i % 2 === 0; });"
        "  }"
        "  return keep.every(function(o){ return o.i % 2 === 0; }) ? 'ok' : 'lost'; })()"));
    GCStats s;
    ASSERT_TRUE(conc_end(&st, &s));
    ASSERT_TRUE(s.full_collections > st.before.full_collections);
    ASSERT_EQ(st.before.verify_failures, s.verify_failures);
    ASSERT_EQ(0, (int)gc_test_heap_check());
    return true;
}

/* A script that never returns to the event loop still lets cycles finish:
 * the interpreter polls at every opcode. */
TEST(test_gc_cycles_progress_inside_long_script) {
    JSContextHandle ctx = get_gc_test_ctx();
    if (!SAFE_VALID(ctx)) return true;
    ConcState st;
    ASSERT_TRUE(conc_begin(&st, 0, GC_REJECT_NORMAL, 64));
    GCStats s0;
    gc_get_stats(&s0);
    ASSERT_TRUE(eval_expect_ok(ctx,
        "(function(){ var x = 0; for (var i = 0; i < 300000; i++) { var o = {i: i}; x += o.i & 1; }"
        "  return x === 150000 ? 'ok' : 'x=' + x; })()"));
    GCStats s1;
    gc_get_stats(&s1);
    /* Several minor cycles completed during the script. */
    ASSERT_TRUE(s1.minor_cycles >= s0.minor_cycles + 2);
    GCStats s;
    ASSERT_TRUE(conc_end(&st, &s));
    return true;
}

/* The boundary budget: with a tiny budget the remark declines and the GC
 * thread does more work first, but cycles still finish. */
TEST(test_gc_boundary_budget_declines) {
    JSContextHandle ctx = get_gc_test_ctx();
    if (!SAFE_VALID(ctx)) return true;
    ConcState st;
    ASSERT_TRUE(conc_begin(&st, 1, GC_REJECT_NORMAL, 128));
    GCConfig c;
    gc_get_config(&c);
    c.boundary_budget_us = 1;
    gc_set_config(&c);
    ASSERT_TRUE(eval_expect_ok(ctx, k_workload_script));
    GCStats s;
    ASSERT_TRUE(conc_end(&st, &s));
    ASSERT_TRUE(s.boundary_declines > st.before.boundary_declines);
    ASSERT_EQ(st.before.verify_failures, s.verify_failures);
    ASSERT_EQ(0, (int)gc_test_heap_check());
    return true;
}

/* ArrayBuffer storage is never moved: the buffer and its typed arrays
 * cache its address.  Cycles that move everything else (stress 2) must
 * leave it alone. */
TEST(test_gc_array_buffer_storage_never_moves) {
    JSContextHandle ctx = get_gc_test_ctx();
    if (!SAFE_VALID(ctx)) return true;
    ConcState st;
    ASSERT_TRUE(conc_begin(&st, 2, GC_REJECT_NORMAL, 128));
    ASSERT_TRUE(eval_expect_ok(ctx,
        "(function(){ var tas = [];"
        "  for (var k = 0; k < 50; k++) { var t = new Uint32Array(64);"
        "    for (var i = 0; i < 64; i++) t[i] = k * 1000 + i; tas.push(t); }"
        "  for (var r = 0; r < 100; r++) { var junk = []; for (var i = 0; i < 1000; i++) junk.push({r: r}); }"
        "  __test_gc();"
        "  for (var k = 0; k < 50; k++) for (var i = 0; i < 64; i++)"
        "    if (tas[k][i] !== k * 1000 + i) return 'typed array ' + k + ' changed';"
        "  for (var k = 0; k < 50; k++) tas[k][3] = 7;"
        "  for (var r = 0; r < 100; r++) { var junk = []; for (var i = 0; i < 1000; i++) junk.push({r: r}); }"
        "  var dv = new DataView(tas[9].buffer); dv.setUint32(8, 99, true);"
        "  for (var k = 0; k < 50; k++) if (tas[k][3] !== 7) return 'write lost ' + k;"
        "  return tas[9][2] === 99 ? 'ok' : 'dataview write lost'; })()"));
    GCStats s;
    ASSERT_TRUE(conc_end(&st, &s));
    ASSERT_EQ(st.before.verify_failures, s.verify_failures);
    ASSERT_EQ(0, (int)gc_test_heap_check());
    return true;
}

/* WeakRef and WeakMap entries to unreachable young objects are cleared by
 * minor cycles. */
TEST(test_gc_weak_references_cleared) {
    JSContextHandle ctx = get_gc_test_ctx();
    if (!SAFE_VALID(ctx)) return true;
    ConcState st;
    ASSERT_TRUE(conc_begin(&st, 0, GC_REJECT_NORMAL, 0));
    ASSERT_TRUE(eval_expect_ok(ctx,
        "globalThis.__wrs = []; globalThis.__wm = new WeakMap(); globalThis.__wkeep = [];"
        "(function(){ for (var i = 0; i < 100; i++) { var o = {i: i}; __wrs.push(new WeakRef(o));"
        "  __wm.set(o, i); if (i % 10 === 0) __wkeep.push(o); } })(); 'ok'"));
    for (int i = 0; i < 3; i++) {
        gc_request_cycle(i == 2);
        ASSERT_TRUE(gc_wait_idle(20000));
        clobber_stack();
    }
    GCStats s;
    ASSERT_TRUE(conc_end(&st, &s));
    /* Kept objects stay; most others are gone (a few may be kept alive by
     * stale words in the conservative stack scan). */
    ASSERT_TRUE(eval_expect_ok(ctx,
        "(function(){ var alive = 0;"
        "  for (var i = 0; i < 100; i++) { var t = __wrs[i].deref();"
        "    if (i % 10 === 0 && (!t || t.i !== i || __wm.get(t) !== i)) return 'kept object lost ' + i;"
        "    if (t) alive++; }"
        "  __wrs = __wm = __wkeep = null;"
        "  return alive <= 30 ? 'ok' : 'weak refs not cleared: ' + alive; })()"));
    return true;
}

/* ============================================================================
 * Test Runner
 * ============================================================================ */

void run_gc_tests(void) {
    printf("\n----------------------------------------\n");
    printf("GC Tests\n");
    printf("----------------------------------------\n");
    
    /* Initialize GC first (required for all tests) */
    printf("  Initializing GC...\n");
    if (!gc_is_initialized()) {
        if (!gc_init()) {
            printf("  FAILED: Could not initialize GC\n");
            return;
        }
    }
    printf("  GC initialized successfully\n\n");
    
    /* Basic tests that don't require QuickJS runtime */
    RUN_TEST(test_gc_thread_pool_basic_job);
    RUN_TEST(test_gc_thread_pool_multiple_jobs);
    RUN_TEST(test_lf_hash_basic);
    RUN_TEST(test_lf_hash_update);
    RUN_TEST(test_lf_hash_remove);
    RUN_TEST(test_lf_hash_resize);
    RUN_TEST(test_lf_hash_threaded);

    RUN_TEST(test_shape_hash_empty_shape_sharing);
    RUN_TEST(test_shape_hash_property_shape_sharing);
    RUN_TEST(test_shape_hash_resize);
    RUN_TEST(test_shape_hash_gc_cleanup);

    RUN_TEST(test_atom_hash_interning);
    RUN_TEST(test_atom_hash_resize);
    RUN_TEST(test_atom_hash_gc_cleanup);
    RUN_TEST(test_class_id_atomic);
    RUN_TEST(test_class_array_atomic);
    RUN_TEST(test_class_proto_atomic);
    RUN_TEST(test_object_prototype_atomic);
    RUN_TEST(test_job_queue_basic);
    RUN_TEST(test_job_queue_gc_marking);
    RUN_TEST(test_property_array_prealloc);
    RUN_TEST(test_property_array_atomic_cas);

    /* Run QuickJS integration tests using shared context */
    printf("\n  QuickJS integration tests use shared context from test_main.c\n");
    

    printf("\n=== Collection While Code Is Running ===\n");
    RUN_TEST(test_gc_running_keeps_locals);
    RUN_TEST(test_gc_running_keeps_arguments);
    RUN_TEST(test_gc_running_keeps_operand_stack);
    RUN_TEST(test_gc_running_keeps_native_temporaries);
    RUN_TEST(test_gc_running_keeps_json_state);
    RUN_TEST(test_gc_running_deep_recursion);
    RUN_TEST(test_gc_running_in_generator);
    RUN_TEST(test_gc_frame_reuse_keeps_captured_frames);
    RUN_TEST(test_gc_frame_reuse_keeps_lazy_parent_frames);
    RUN_TEST(test_gc_frame_reuse_past_pool_depth_and_throws);
    RUN_TEST(test_gc_shared_and_dictionary_shapes);
    RUN_TEST(test_gc_inline_properties_grow_and_survive);
    RUN_TEST(test_gc_running_repeated);
    RUN_TEST(test_gc_frame_chain_after_generator);
    RUN_TEST(test_gc_bigint_survives);

    printf("\n=== Heap and Handles ===\n");
    RUN_TEST(test_gc_header_is_8_bytes);
    RUN_TEST(test_gc_ptr_to_handle_interior);
    RUN_TEST(test_gc_atoms_are_permanent);
    RUN_TEST(test_gc_humongous_object_never_moves);
    RUN_TEST(test_gc_realloc_copies_into_a_new_object);
    RUN_TEST(test_gc_free_kills_handle);

    printf("\n=== Stop-the-World Collection ===\n");
    RUN_TEST(test_gc_unreachable_objects_freed);
    RUN_TEST(test_gc_roots_keep_objects);
    RUN_TEST(test_gc_finalizers_run_once);
    RUN_TEST(test_gc_sparse_regions_keep_survivors);
    RUN_TEST(test_gc_denser_regions_keep_survivors);
    RUN_TEST(test_gc_raw_pointer_pins_object);

    printf("\n=== Dirty Tracking ===\n");
    RUN_TEST(test_gc_write_helpers_set_dirty_flags);
    RUN_TEST(test_gc_os_write_watch);

    printf("\n=== Concurrent Cycles ===\n");
    RUN_TEST(test_gc_concurrent_major_cycle);
    RUN_TEST(test_gc_minor_cycle_promotes_survivors);
    RUN_TEST(test_gc_remembered_set_keeps_young_objects);
    RUN_TEST(test_gc_concurrent_workload_stress);
    RUN_TEST(test_gc_full_collection_aborts_cycle);
    RUN_TEST(test_gc_cycles_progress_inside_long_script);
    RUN_TEST(test_gc_boundary_budget_declines);
    RUN_TEST(test_gc_weak_references_cleared);
    RUN_TEST(test_gc_array_buffer_storage_never_moves);
    /* After the cycle-count tests: these grow old space. */
    RUN_TEST(test_gc_async_frame_survives_await);
    RUN_TEST(test_gc_async_closure_keeps_args_after_return);
    RUN_TEST(test_gc_async_frames_during_concurrent_cycles);
    RUN_TEST(test_gc_weakmap_values_survive_full_cycles);
    RUN_TEST(test_gc_weakmap_values_survive_concurrent_cycles);
    RUN_TEST(test_gc_weakmap_dead_key_value_survives_full_cycles);
    RUN_TEST(test_gc_weakmap_dead_key_value_survives_concurrent_cycles);
    RUN_TEST(test_gc_property_key_string_outlives_delete);
}

/* ============================================================================
 * Allocation microbenchmark (`cyberbrowser-tests gc_alloc_bench`, not part of
 * the default run): old-space allocation filling holes against bump
 * allocation in fresh regions, in the same process.  "raw" is the allocator
 * alone (no handle, so the memory is garbage at the next collection);
 * "gc_alloc_old" adds the handle and table write every object pays today.
 * ============================================================================ */

enum { BENCH_PREP_BYTES = 64 << 20, BENCH_RUN_BYTES = 32 << 20, BENCH_MAX_N = 1 << 20 };
static GCHandle g_bench_handles[BENCH_MAX_N];
static GCHandle *g_bench_kept;
static size_t g_bench_nkept;

static size_t bench_total(size_t size) { return (size + GC_HEADER_SIZE + 15) & ~(size_t)15; }

/* Old regions with a live object every ~1 KB and holes between. */
static void bench_make_holes(size_t size) {
    size_t total = bench_total(size), n = BENCH_PREP_BYTES / total;
    size_t every = 1024 / total < 3 ? 3 : 1024 / total;
    g_bench_kept = (GCHandle *)malloc((n / every + 1) * sizeof(GCHandle));
    g_bench_nkept = 0;
    for (size_t i = 0; i < n; i++) {
        GCHandle h = gc_alloc_old(size, JS_GC_OBJ_TYPE_DATA);
        if (i % every == 0) {
            gc_add_root(h);
            g_bench_kept[g_bench_nkept++] = h;
        } else {
            gc_free(h);
        }
    }
    gc_run();
}

static void bench_drop_holes(void) {
    for (size_t i = 0; i < g_bench_nkept; i++) gc_remove_root(g_bench_kept[i]);
    free(g_bench_kept);
    g_bench_kept = NULL;
    g_bench_nkept = 0;
    gc_run();
}

static double bench_raw_ns(size_t size) {
    size_t n = BENCH_RUN_BYTES / bench_total(size);
    if (n > BENCH_MAX_N) n = BENCH_MAX_N;
    GCAllocCtx a;
    memset(&a, 0, sizeof(a));
    a.space = GC_RS_OLD;
    a.owner = GC_OWNER_JS;
    a.recycles = true;
    GCAllocCtx o;
    memset(&o, 0, sizeof(o));
    o.space = GC_RS_OLD;
    o.owner = GC_OWNER_JS;
    a.overflow = &o;
    uint64_t t0 = gc_os_now_ns();
    for (size_t i = 0; i < n; i++) {
        GCHeader *hdr = gc_core_alloc_object_memory(&a, size, JS_GC_OBJ_TYPE_DATA);
        ((uint32_t *)gc_header_payload(hdr))[0] = (uint32_t)i;
    }
    uint64_t t1 = gc_os_now_ns();
    gc_core_alloc_ctx_retire(&a);
    return (double)(t1 - t0) / (double)n;
}

static double bench_handle_ns(size_t size) {
    size_t n = BENCH_RUN_BYTES / bench_total(size);
    if (n > BENCH_MAX_N) n = BENCH_MAX_N;
    GCStats s0, s1;
    gc_get_stats(&s0);
    uint64_t t0 = gc_os_now_ns();
    for (size_t i = 0; i < n; i++) {
        GCHandle h = gc_alloc_old(size, JS_GC_OBJ_TYPE_DATA);
        ((uint32_t *)gc_deref(h))[0] = (uint32_t)i;
        g_bench_handles[i] = h;
    }
    uint64_t t1 = gc_os_now_ns();
    gc_get_stats(&s1);
    uint64_t cycles = s1.full_collections + s1.minor_cycles + s1.major_cycles -
                      (s0.full_collections + s0.minor_cycles + s0.major_cycles);
    if (cycles) printf("  (size %zu: %llu collections during the timed loop)\n", size, (unsigned long long)cycles);
    for (size_t i = 0; i < n; i++) gc_free(g_bench_handles[i]);
    return (double)(t1 - t0) / (double)n;
}

void run_gc_alloc_bench(void) {
    static const size_t sizes[] = { 24, 56, 120, 248, 1000 };
    BgState st;
    bg_pause(&st);
    printf("\nGC allocation benchmark (ns per object, best of 3; holes %s)\n", g_core.holes ? "on" : "off");
    printf("  %6s  %10s %10s  %14s %14s\n", "size", "raw bump", "raw holes", "handle bump", "handle holes");
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        size_t size = sizes[s];
        double best[4] = { 1e9, 1e9, 1e9, 1e9 };
        for (int rep = 0; rep < 3; rep++) {
            double v;
            gc_run();
            if ((v = bench_raw_ns(size)) < best[0]) best[0] = v;
            gc_run();
            if ((v = bench_handle_ns(size)) < best[2]) best[2] = v;
            bench_make_holes(size);
            if ((v = bench_raw_ns(size)) < best[1]) best[1] = v;
            bench_drop_holes();
            bench_make_holes(size);
            if ((v = bench_handle_ns(size)) < best[3]) best[3] = v;
            bench_drop_holes();
        }
        printf("  %6zu  %10.2f %10.2f  %14.2f %14.2f\n", size, best[0], best[1], best[2], best[3]);
    }
    fflush(stdout);
    bg_resume(&st);
}

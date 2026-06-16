# Implementation Plan: Deeply Nested IIFE Pattern Support

## Overview

This document outlines the implementation plan for supporting deeply nested IIFE (Immediately Invoked Function Expression) patterns in the QuickJS interpreter, specifically to fix the "closure creation failed" error observed when executing YouTube's Web Animations polyfill (script 006).

## Table of Contents

1. [Problem Analysis](#problem-analysis)
2. [Current Architecture](#current-architecture)
3. [Root Cause](#root-cause)
4. [Proposed Solution](#proposed-solution)
5. [Implementation Steps](#implementation-steps)
6. [Testing Strategy](#testing-strategy)
7. [Risk Assessment](#risk-assessment)
8. [Alternative Approaches](#alternative-approaches)
9. [Estimated Effort](#estimated-effort)

---

## Problem Analysis

### Failing Pattern

```javascript
(function(y, v) {      // Parent IIFE creates stack frame
  function k() { ... } // Child function tries to capture from parent
  function f() { ... } // Another child function
  // ... many more nested functions
})(G, null);
```

### Error Manifestation

- **Error**: `InternalError: closure creation failed`
- **Location**: `js_closure2()` in `quickjs.cpp:18386`
- **Affected Scripts**: Script 006 (Web Animations polyfill), Script 008, and potentially others
- **Impact**: 33 out of 51 YouTube scripts fail with various closure-related errors

---

## Current Architecture

### Stack Frame Structure

```cpp
// JSStackFrame (from quickjs_types.h:492)
typedef struct JSStackFrame {
    JSStackFrame *prev_frame;        // Pointer to previous stack frame
    GCValue cur_func;                // Current function
    GCValue *arg_buf;                // Arguments (C stack pointer)
    GCValue *var_buf;                // Local variables (C stack pointer)
    JSVarRefHandle *var_refs;        // Captured variable references
    const uint8_t *cur_pc;           // Program counter
    int arg_count;
    int js_mode;
    GCValue *cur_sp;                 // Stack pointer (for generators)
} JSStackFrame;
```

### Closure Creation Flow

1. **Function Definition**: Parser emits `OP_fclosure` opcode
2. **Closure Creation**: `js_closure()` → `js_closure2()` creates function object
3. **Variable Resolution**: `js_closure2()` resolves closure variables based on type:
   - `JS_CLOSURE_LOCAL`: Local variable from parent
   - `JS_CLOSURE_ARG`: Argument from parent
   - `JS_CLOSURE_REF`: Reference to parent's closure
   - `JS_CLOSURE_GLOBAL`: Global variable
   - `JS_CLOSURE_GLOBAL_DECL`: Global declaration
   - `JS_CLOSURE_GLOBAL_REF`: Global reference
   - `JS_CLOSURE_MODULE_IMPORT`: Module import
   - `JS_CLOSURE_MODULE_DECL`: Module declaration

### Key Code Paths

**`js_closure2()`** (`quickjs.cpp:18190-18335`):
```cpp
static GCValue js_closure2(JSContextHandle ctx, GCValue func_obj,
                           JSFunctionBytecodeHandle b,
                           GCHandle cur_var_refs_handle,
                           JSStackFrame *sf,
                           BOOL is_eval, JSModuleDefHandle m)
```

**`get_var_ref()`** (`quickjs.cpp:17903-17940`):
```cpp
static JSVarRefHandle get_var_ref(JSContextHandle ctx, JSStackFrame *sf, 
                                  int var_idx, BOOL is_arg)
```

---

## Root Cause

### Issue 1: Timing Problem with var_refs Initialization

When `js_closure2()` is called for nested functions inside an IIFE:

1. Parent IIFE's stack frame is created with `var_refs` array allocated
2. Child function definitions are encountered immediately (at parse time)
3. `js_closure2()` tries to capture parent variables via `get_var_ref()`
4. `sf->var_refs[var_ref_idx]` may be NULL because parent hasn't executed enough to initialize them

### Issue 2: Missing Stack Chain Traversal

For deeply nested IIFEs (3+ levels), the current code doesn't properly walk up the stack frame chain. It only looks at the immediate parent's `var_refs`, not grandparent or higher.

### Issue 3: Incomplete Fallback Logic

When parent `var_refs` entry is not available, the fallback logic:
```cpp
if (!var_ref) {
    var_ref = js_create_var_ref(ctx, b.closure_var_is_lexical(i));
    if (var_ref) {
        var_ref.set_value(JS_UNDEFINED);
    }
}
```
This creates a detached var_ref, but doesn't properly link it to the actual stack location.

---

## Proposed Solution

### Phase 1: Enhanced `get_var_ref()` with Lazy Initialization

**File**: `browser-emulator/third_party/quickjs/quickjs.cpp`  
**Location**: Lines 17903-17940

**Changes**:
- Add `create_if_missing` parameter for controlled creation
- Handle non-captured variables gracefully
- Add validation for `var_ref_idx` bounds

```cpp
static JSVarRefHandle get_var_ref_enhanced(JSContextHandle ctx, JSStackFrame *sf, 
                                           int var_idx, BOOL is_arg, 
                                           BOOL create_if_missing) {
    JSObjectHandle p;
    JSFunctionBytecodeHandle b;
    JSVarRefHandle var_ref;
    int var_ref_idx;
    JSBytecodeVarDef *vd;
    
    p = JS_VALUE_GET_OBJ(sf->cur_func);
    b = JSFunctionBytecodeHandle(p.func_bytecode_handle());
    
    // Get variable definition
    {
        JSBytecodeVarDef *vardefs = b_vardefs(b);
        if (is_arg) {
            vd = &vardefs[var_idx];
        } else {
            vd = &vardefs[b.arg_count() + var_idx];
        }
    }
    
    if (!vd->is_captured) {
        // Variable is not captured - create a detached var_ref
        var_ref = js_create_var_ref(ctx, FALSE);
        if (!var_ref) return JSVarRefHandle();
        
        var_ref.set_value(is_arg ? sf->arg_buf[var_idx] : sf->var_buf[var_idx]);
        return var_ref;
    }
    
    var_ref_idx = vd->var_ref_idx;
    if (var_ref_idx < 0 || var_ref_idx >= b.var_ref_count()) {
        return JSVarRefHandle(); // Invalid index
    }
    
    // Try to get existing var_ref
    var_ref = sf->var_refs[var_ref_idx];
    if (var_ref) {
        return var_ref;
    }
    
    if (!create_if_missing) {
        return JSVarRefHandle();
    }
    
    // Create new var_ref for this captured variable
    var_ref = js_create_var_ref(ctx, vd->is_lexical);
    if (!var_ref) return JSVarRefHandle();
    
    // Initialize with current value from stack
    GCValue initial_value = is_arg ? sf->arg_buf[var_idx] : sf->var_buf[var_idx];
    var_ref.set_value(initial_value);
    
    sf->var_refs[var_ref_idx] = var_ref;
    return var_ref;
}
```

### Phase 2: Stack Frame Chain Traversal

**File**: `browser-emulator/third_party/quickjs/quickjs.cpp`  
**Location**: After `get_var_ref()` (~line 17950)

**New Function**: `js_closure_resolve_from_stack()`

```cpp
/*
 * Resolve a closure variable by walking up the stack frame chain.
 * This handles deeply nested IIFEs where the immediate parent might not
 * have the variable in its var_refs yet.
 * 
 * Parameters:
 *   ctx - JS context
 *   parent_sf - Starting stack frame (immediate parent)
 *   var_idx - Variable index in parent
 *   is_arg - TRUE if variable is an argument, FALSE for local
 *   max_depth - Maximum traversal depth (safety limit)
 * 
 * Returns:
 *   Valid JSVarRefHandle on success, null handle on failure
 */
static JSVarRefHandle js_closure_resolve_from_stack(JSContextHandle ctx, 
                                                     JSStackFrame *parent_sf,
                                                     int var_idx, 
                                                     BOOL is_arg,
                                                     int max_depth) {
    JSStackFrame *sf = parent_sf;
    int current_depth = 0;
    
    while (sf && current_depth < max_depth) {
        JSObjectHandle p = JS_VALUE_GET_OBJ(sf->cur_func);
        JSFunctionBytecodeHandle b = JSFunctionBytecodeHandle(p.func_bytecode_handle());
        
        // Check if this frame has the variable
        int var_count = is_arg ? b.arg_count() : b.var_count();
        if (var_idx >= 0 && var_idx < var_count) {
            // Try to get or create var_ref for this variable
            JSVarRefHandle var_ref = get_var_ref_enhanced(ctx, sf, var_idx, is_arg, TRUE);
            if (var_ref) {
                return var_ref;
            }
        }
        
        // Move up to parent frame
        sf = sf->prev_frame;
        current_depth++;
    }
    
    // Fallback: create a detached var_ref initialized to undefined
    JSVarRefHandle var_ref = js_create_var_ref(ctx, FALSE);
    if (var_ref) {
        var_ref.set_value(JS_UNDEFINED);
    }
    return var_ref;
}
```

### Phase 3: Enhanced `js_closure2()`

**File**: `browser-emulator/third_party/quickjs/quickjs.cpp`  
**Location**: Lines 18190-18335

**Modified Switch Cases**:

```cpp
static GCValue js_closure2(JSContextHandle ctx, GCValue func_obj,
                           JSFunctionBytecodeHandle b,
                           GCHandle cur_var_refs_handle,
                           JSStackFrame *sf,
                           BOOL is_eval, JSModuleDefHandle m) {
    JSObjectHandle p;
    GCHandle var_refs_handle;
    int i;
    BOOL has_failures = FALSE;

    p = JS_VALUE_GET_OBJ(func_obj);
    p.set_func_bytecode_handle(b.handle());
    p.set_func_home_object_handle(GC_HANDLE_NULL);
    p.set_func_var_refs_handle(GC_HANDLE_NULL);
    
    if (b.closure_var_count()) {
        var_refs_handle = gc_allocz(sizeof(GCHandle) * b.closure_var_count(), 
                                    JS_GC_OBJ_TYPE_DATA);
        if (!var_refs_handle)
            goto fail;
        p.set_func_var_refs_handle(var_refs_handle);
        
        if (is_eval) {
            // Existing eval check code...
            for(i = 0; i < b.closure_var_count(); i++) {
                if (b.closure_var_closure_type(i) == JS_CLOSURE_GLOBAL_DECL) {
                    int flags = 0;
                    if (b.closure_var_is_lexical(i))
                        flags |= DEFINE_GLOBAL_LEX_VAR;
                    if (b.closure_var_var_kind(i) == JS_VAR_GLOBAL_FUNCTION_DECL)
                        flags |= DEFINE_GLOBAL_FUNC_VAR;
                    if (JS_CheckDefineGlobalVar(ctx, b.closure_var_var_name(i), flags))
                        goto fail;
                }
            }
        }
        
        for(i = 0; i < b.closure_var_count(); i++) {
            JSVarRefHandle var_ref;
            BOOL resolved = FALSE;
            
            switch(b.closure_var_closure_type(i)) {
            case JS_CLOSURE_MODULE_IMPORT:
                // imported from other modules - skip
                continue;
                
            case JS_CLOSURE_MODULE_DECL:
                var_ref = js_create_var_ref(ctx, b.closure_var_is_lexical(i));
                if (var_ref) resolved = TRUE;
                break;
                
            case JS_CLOSURE_GLOBAL_DECL:
                {
                    JSClosureVar cv;
                    cv.closure_type = JS_CLOSURE_GLOBAL_DECL;
                    cv.is_lexical = b.closure_var_is_lexical(i);
                    cv.is_const = b.closure_var_is_const(i);
                    cv.var_kind = b.closure_var_var_kind(i);
                    cv.var_idx = 0;
                    cv.var_name = b.closure_var_var_name(i);
                    var_ref = js_closure_define_global_var(ctx, &cv, 
                                                           b.is_direct_or_indirect_eval());
                    if (var_ref) resolved = TRUE;
                }
                break;
                
            case JS_CLOSURE_GLOBAL:
                {
                    JSClosureVar cv;
                    cv.closure_type = JS_CLOSURE_GLOBAL;
                    cv.is_lexical = b.closure_var_is_lexical(i);
                    cv.is_const = b.closure_var_is_const(i);
                    cv.var_kind = b.closure_var_var_kind(i);
                    cv.var_idx = 0;
                    cv.var_name = b.closure_var_var_name(i);
                    var_ref = js_closure_global_var(ctx, &cv);
                    if (var_ref) resolved = TRUE;
                }
                break;
                
            case JS_CLOSURE_LOCAL:
                // Enhanced resolution with fallback
                if (sf && sf->var_refs) {
                    var_ref = get_var_ref_enhanced(ctx, sf, b.closure_var_var_idx(i), 
                                                   FALSE, FALSE);
                    if (var_ref) {
                        resolved = TRUE;
                    } else {
                        // Try stack chain traversal with up to 5 levels
                        var_ref = js_closure_resolve_from_stack(ctx, sf, 
                                                                b.closure_var_var_idx(i), 
                                                                FALSE, 5);
                        if (var_ref) resolved = TRUE;
                    }
                } else {
                    // No parent frame - create detached var_ref
                    var_ref = js_create_var_ref(ctx, b.closure_var_is_lexical(i));
                    if (var_ref) {
                        var_ref.set_value(JS_UNDEFINED);
                        resolved = TRUE;
                    }
                }
                break;
                
            case JS_CLOSURE_ARG:
                // Enhanced resolution with fallback
                if (sf && sf->var_refs) {
                    var_ref = get_var_ref_enhanced(ctx, sf, b.closure_var_var_idx(i), 
                                                   TRUE, FALSE);
                    if (var_ref) {
                        resolved = TRUE;
                    } else {
                        // Try stack chain traversal
                        var_ref = js_closure_resolve_from_stack(ctx, sf,
                                                                b.closure_var_var_idx(i),
                                                                TRUE, 5);
                        if (var_ref) resolved = TRUE;
                    }
                } else {
                    var_ref = js_create_var_ref(ctx, b.closure_var_is_lexical(i));
                    if (var_ref) {
                        var_ref.set_value(JS_UNDEFINED);
                        resolved = TRUE;
                    }
                }
                break;
                
            case JS_CLOSURE_REF:
                // Try inherited var_refs first
                if (cur_var_refs_handle != GC_HANDLE_NULL) {
                    var_ref = JSVarRefHandle::from_array_handle(
                        cur_var_refs_handle, b.closure_var_var_idx(i));
                    if (var_ref.handle() > 1) {
                        resolved = TRUE;
                    }
                }
                
                if (!resolved && sf && sf->var_refs) {
                    // Try to get from parent frame directly
                    var_ref = get_var_ref_enhanced(ctx, sf, b.closure_var_var_idx(i), 
                                                   FALSE, TRUE);
                    if (var_ref) resolved = TRUE;
                }
                
                if (!resolved) {
                    // Final fallback: create detached var_ref
                    var_ref = js_create_var_ref(ctx, b.closure_var_is_lexical(i));
                    if (var_ref) {
                        var_ref.set_value(JS_UNDEFINED);
                        resolved = TRUE;
                    }
                }
                break;
                
            case JS_CLOSURE_GLOBAL_REF:
                // Try inherited var_refs first
                if (cur_var_refs_handle != GC_HANDLE_NULL) {
                    var_ref = JSVarRefHandle::from_array_handle(
                        cur_var_refs_handle, b.closure_var_var_idx(i));
                    if (var_ref.handle() > 1) {
                        resolved = TRUE;
                    }
                }
                
                if (!resolved) {
                    // Look up in global object
                    JSClosureVar cv;
                    cv.closure_type = JS_CLOSURE_GLOBAL;
                    cv.is_lexical = b.closure_var_is_lexical(i);
                    cv.is_const = b.closure_var_is_const(i);
                    cv.var_kind = b.closure_var_var_kind(i);
                    cv.var_idx = 0;
                    cv.var_name = b.closure_var_var_name(i);
                    var_ref = js_closure_global_var(ctx, &cv);
                    if (var_ref) resolved = TRUE;
                }
                break;
                
            default:
                abort();
            }
            
            if (!resolved || !var_ref) {
                // Log detailed failure info for debugging
                fprintf(stderr, "js_closure2: Failed to resolve closure var %d/%d, "
                        "type=%d, idx=%d, sf=%p\n", 
                        i, b.closure_var_count(), 
                        b.closure_var_closure_type(i),
                        b.closure_var_var_idx(i),
                        (void*)sf);
                has_failures = TRUE;
                // Continue to process remaining vars for debugging
                // but will fail at the end
            } else {
                JSVarRefHandle::set_in_array_handle(var_refs_handle, i, var_ref);
            }
        }
        
        if (has_failures) {
            goto fail;
        }
    }
    return func_obj;
    
 fail:
    /* bfunc is freed when func_obj is freed */
    return JS_EXCEPTION;
}
```

### Phase 4: Proactive var_refs Initialization

**File**: `browser-emulator/third_party/quickjs/quickjs.cpp`  
**Location**: `JS_CallInternal()` around line 18860

**Enhanced Frame Setup**:

```cpp
    // After: sf->var_refs = (JSVarRefHandle*)(stack_buf + b.stack_size());
    // After: for(i = 0; i < b.var_ref_count(); i++) sf->var_refs[i] = NULL;

    // NEW: Pre-initialize var_refs for captured arguments and variables
    // This ensures nested functions defined early in parent can capture them
    
    // First, initialize captured arguments
    for (i = 0; i < b.arg_count(); i++) {
        JSVarDefHandle vd = b_args.at(i);
        if (vd.is_captured() && vd.var_ref_idx() < b.var_ref_count()) {
            JSVarRefHandle var_ref = js_create_var_ref(ctx, FALSE);
            if (var_ref) {
                var_ref.set_value(arg_buf[i]);
                var_ref.set_is_detached(FALSE);
                var_ref.set_var_idx(i);
                var_ref.set_is_arg(TRUE);
                sf->var_refs[vd.var_ref_idx()] = var_ref;
            }
        }
    }

    // Then, initialize captured local variables
    for (i = 0; i < b.var_count(); i++) {
        JSVarDefHandle vd = b_vars.at(i);
        if (vd.is_captured() && vd.var_ref_idx() < b.var_ref_count()) {
            JSVarRefHandle var_ref = js_create_var_ref(ctx, vd.is_lexical());
            if (var_ref) {
                // Variable not initialized yet - set to undefined
                var_ref.set_value(JS_UNDEFINED);
                var_ref.set_is_detached(FALSE);
                var_ref.set_var_idx(i);
                var_ref.set_is_arg(FALSE);
                sf->var_refs[vd.var_ref_idx()] = var_ref;
            }
        }
    }
```

### Phase 5: Debug Logging (Optional)

**File**: `browser-emulator/third_party/quickjs/quickjs.cpp`  
**Location**: Beginning of file, after includes

```cpp
// Debug logging for closure creation (define to enable)
// #define DEBUG_CLOSURE_CREATION 1

#ifdef DEBUG_CLOSURE_CREATION
    #define DEBUG_CLOSURE(fmt, ...) \
        fprintf(stderr, "[CLOSURE %s:%d] " fmt "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)
#else
    #define DEBUG_CLOSURE(fmt, ...) ((void)0)
#endif
```

---

## Implementation Steps

### Step 1: Enhanced `get_var_ref()`
**File**: `quickjs.cpp`  
**Lines**: 17903-17940  
**Estimated Time**: 2 hours  
**Complexity**: Medium

- Add `create_if_missing` parameter
- Handle non-captured variables gracefully
- Add bounds checking for `var_ref_idx`

### Step 2: Add Stack Chain Traversal Function
**File**: `quickjs.cpp`  
**Lines**: After line 17950  
**Estimated Time**: 3 hours  
**Complexity**: Medium

- Implement `js_closure_resolve_from_stack()`
- Add depth limit for safety
- Ensure proper fallback to detached var_refs

### Step 3: Enhance `js_closure2()`
**File**: `quickjs.cpp`  
**Lines**: 18190-18335  
**Estimated Time**: 4 hours  
**Complexity**: High

- Replace existing switch case handlers
- Add stack chain traversal on failure
- Add detailed error logging
- Ensure all paths set `var_ref` or go to `fail`

### Step 4: Add Proactive var_refs Initialization
**File**: `quickjs.cpp`  
**Lines**: ~18860 in `JS_CallInternal()`  
**Estimated Time**: 2 hours  
**Complexity**: Medium

- Pre-initialize captured arguments
- Pre-initialize captured local variables
- Ensure proper cleanup on error

### Step 5: Add Debug Logging (Optional)
**File**: `quickjs.cpp`  
**Estimated Time**: 30 minutes  
**Complexity**: Low

- Add conditional debug macros
- Instrument key closure creation paths

---

## Testing Strategy

### Unit Tests

**New Test File**: `browser-emulator/tests/test_nested_iife.cpp`

```cpp
/*
 * Unit tests for deeply nested IIFE patterns
 */
#include "test_runner.h"
#include "platform.h"
#include "quickjs.h"

TEST(test_simple_iife) {
    const char *js = "(function() { return 42; })();";
    GCValue result = JS_Eval(ctx, js, strlen(js), "<test>", JS_EVAL_TYPE_GLOBAL);
    ASSERT_FALSE(JS_IsException(result));
    ASSERT_EQ(JS_VALUE_GET_INT(result), 42);
}

TEST(test_nested_function_in_iife) {
    const char *js = 
        "(function(y) {"
        "  function k() { return y; }"
        "  return k();"
        "})(42);";
    GCValue result = JS_Eval(ctx, js, strlen(js), "<test>", JS_EVAL_TYPE_GLOBAL);
    ASSERT_FALSE(JS_IsException(result));
    ASSERT_EQ(JS_VALUE_GET_INT(result), 42);
}

TEST(test_deeply_nested_iife) {
    const char *js =
        "(function(a) {"
        "  return (function(b) {"
        "    return (function(c) {"
        "      function inner() { return a + b + c; }"
        "      return inner();"
        "    })(3);"
        "  })(2);"
        "})(1);";
    GCValue result = JS_Eval(ctx, js, strlen(js), "<test>", JS_EVAL_TYPE_GLOBAL);
    ASSERT_FALSE(JS_IsException(result));
    ASSERT_EQ(JS_VALUE_GET_INT(result), 6);
}

TEST(test_web_animations_like_pattern) {
    // Simplified pattern from actual Web Animations polyfill
    const char *js =
        "(function(G, F) {"
        "  function k() { this._duration = 0; }"
        "  k.prototype = {"
        "    get duration() { return this._duration; },"
        "    set duration(p) { this._duration = p; }"
        "  };"
        "  G.Timing = k;"
        "})({}, {});";
    GCValue result = JS_Eval(ctx, js, strlen(js), "<test>", JS_EVAL_TYPE_GLOBAL);
    ASSERT_FALSE(JS_IsException(result));
}

TEST(test_closure_with_multiple_captures) {
    const char *js =
        "var result = (function(a, b) {"
        "  function inner1() { return a; }"
        "  function inner2() { return b; }"
        "  function inner3() { return a + b; }"
        "  return inner1() + inner2() + inner3();"
        "})(10, 20);";
    GCValue result = JS_Eval(ctx, js, strlen(js), "<test>", JS_EVAL_TYPE_GLOBAL);
    ASSERT_FALSE(JS_IsException(result));
    // 10 + 20 + 30 = 60
}
```

### Integration Tests

1. **Full Test Suite**: Run all existing tests to ensure no regression
2. **YouTube Data Scripts**: Verify scripts 006, 008, and others execute without "closure creation failed"
3. **Performance Benchmark**: Compare execution time before/after changes

### Manual Testing

```bash
# Build and run tests
cd browser-emulator && ./build.sh
./build/tests/browser-emulator-tests

# Check specific script execution
./build/tests/browser-emulator-tests 2>&1 | grep -E "Script (6|8)"
```

---

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Memory leaks in var_ref creation | Medium | High | Careful tracking of `js_create_var_ref()` calls; ensure all paths free on error; run valgrind |
| Stack overflow with deep recursion | Low | High | Limit stack chain traversal depth (max 5-10 levels); use iterative approach |
| Performance degradation | Medium | Medium | Benchmark before/after; optimize hot paths; lazy initialization |
| Breaking existing closure behavior | Low | High | Comprehensive test suite; gradual rollout; feature flag |
| Infinite loops in stack traversal | Low | High | Strict depth limit; check for NULL prev_frame |
| Incorrect variable capture | Medium | High | Extensive unit tests; compare with reference implementation |

---

## Alternative Approaches

### Alternative 1: Deferred Closure Resolution

Instead of resolving closures immediately, defer until first function call:

**Pros**:
- Avoids initialization order issues
- Simpler implementation

**Cons**:
- More complex runtime
- Potential performance overhead on first call
- Requires bytecode modification

### Alternative 2: Simplified Fallback

Always fall back to creating detached var_refs initialized to `undefined`:

**Pros**:
- Simple and safe
- Minimal code changes

**Cons**:
- May break edge cases
- Not semantically correct for all patterns
- Could mask real errors

### Alternative 3: Bytecode Modification

Modify parser to emit different bytecode for nested functions in IIFEs:

**Pros**:
- Potentially cleanest solution
- Can optimize at compile time

**Cons**:
- Most invasive
- Requires deep understanding of parser
- May break other patterns

### Recommendation

Proceed with **Proposed Solution** (Phases 1-4) as it provides the best balance of correctness, performance, and maintainability. If issues arise, consider **Alternative 2** as a fallback.

---

## Estimated Effort

| Phase | Lines of Code | Complexity | Time Estimate |
|-------|--------------|------------|---------------|
| Step 1: Enhanced `get_var_ref()` | ~40 | Medium | 2 hours |
| Step 2: Stack chain traversal | ~60 | Medium | 3 hours |
| Step 3: Enhanced `js_closure2()` | ~80 | High | 4 hours |
| Step 4: Proactive initialization | ~40 | Medium | 2 hours |
| Step 5: Debug logging | ~20 | Low | 30 min |
| Unit tests | ~100 | Medium | 3 hours |
| Integration testing | - | - | 3 hours |
| Documentation | - | Low | 1 hour |
| **Total** | **~340** | - | **~18-20 hours** |

---

## References

### Code Locations

| Function | File | Line Range | Purpose |
|----------|------|------------|---------|
| `js_closure2()` | `quickjs.cpp` | 18190-18335 | Main closure creation |
| `js_closure()` | `quickjs.cpp` | 18365-18420 | Wrapper for js_closure2 |
| `get_var_ref()` | `quickjs.cpp` | 17903-17940 | Get/create variable reference |
| `js_create_var_ref()` | `quickjs.cpp` | 17879-17902 | Create new var_ref |
| `JS_CallInternal()` | `quickjs.cpp` | 18745-... | Main interpreter loop |
| `OP_fclosure` | `quickjs.cpp` | 19161-19169 | Opcode handler |

### Related Issues

- YouTube Script 006: Web Animations polyfill
- YouTube Script 008: Related closure patterns
- Test: `test_youtube_data_scripts.cpp`

---

## Appendix: Closure Variable Types

```cpp
// From quickjs_types.h
typedef enum {
    JS_CLOSURE_LOCAL,        // Local variable from parent frame
    JS_CLOSURE_ARG,          // Argument from parent frame
    JS_CLOSURE_VAR_REF,      // Reference to another closure variable
    JS_CLOSURE_GLOBAL,       // Global variable access
    JS_CLOSURE_GLOBAL_DECL,  // Global declaration
    JS_CLOSURE_GLOBAL_REF,   // Reference to global
    JS_CLOSURE_MODULE_IMPORT,// Module import
    JS_CLOSURE_MODULE_DECL,  // Module declaration
} JSClosureTypeEnum;
```

---

*Document Version: 1.0*  
*Created: 2026-03-27*  
*Author: Kimi Code Assistant*

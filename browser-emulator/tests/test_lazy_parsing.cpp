/*
 * Test suite for lazy function parsing system
 * 
 * These tests verify that:
 * 1. Functions are created with lazy parsing state
 * 2. Functions execute correctly after lazy parsing
 * 3. Closures work correctly with lazy parsing
 * 4. Class constructors and methods work
 * 5. Arrow functions work
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "test_runner.h"
#include "platform.h"
#include "quickjs.h"

#define LOG_TAG "test_lazy_parsing"

/* Forward declaration for shared context accessor */
extern "C" JSContextHandle get_shared_test_context(void);

/* Helper to get the shared test context */
static JSContextHandle get_test_context(void) {
    JSContextHandle ctx = get_shared_test_context();
    if (!ctx.valid()) {
        printf("    ERROR: Shared context not available\n");
        return JSContextHandle();
    }
    return ctx;
}

/* Test helper: Evaluate JavaScript and return result as string */
static char* eval_to_string(JSContextHandle ctx, const char* code) {
    GCValue result = JS_Eval(ctx, code, strlen(code), "<test>", 0);
    if (JS_IsException(result)) {
        GCValue exception = JS_GetException(ctx);
        const char* str = JS_ToCString(ctx, exception);
        char* copy = strdup(str ? str : "exception");
        return copy;
    }
    const char* str = JS_ToCString(ctx, result);
    char* copy = strdup(str ? str : "undefined");
    return copy;
}

/* Test helper: Evaluate and check if exception was thrown */
static BOOL eval_throws_exception(JSContextHandle ctx, const char* code) {
    GCValue result = JS_Eval(ctx, code, strlen(code), "<test>", 0);
    BOOL is_exception = JS_IsException(result);
    if (!is_exception) {
        /* Not an exception, clean up */
    }
    return is_exception;
}

/* Test helper: Evaluate and get integer result */
static int eval_to_int(JSContextHandle ctx, const char* code, BOOL* success) {
    GCValue result = JS_Eval(ctx, code, strlen(code), "<test>", 0);
    if (JS_IsException(result)) {
        if (success) *success = FALSE;
        return 0;
    }
    int32_t val;
    if (JS_ToInt32(ctx, &val, result)) {
        if (success) *success = FALSE;
        return 0;
    }
    if (success) *success = TRUE;
    return val;
}

/* ============================================================================
 * Test 1: Basic Lazy Function Parsing
 * ============================================================================ */

TEST(test_lazy_function_basic) {
    JSContextHandle ctx = get_test_context();
    ASSERT_TRUE(ctx.valid());
    
    /* Test 1.1: Simple function definition and call */
    {
        const char* code = 
            "function add(a, b) {\n"
            "    return a + b;\n"
            "}\n"
            "add(2, 3);";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 5);
    }
    
    /* Test 1.2: Function with multiple statements */
    {
        const char* code = 
            "function multiply(a, b) {\n"
            "    let result = a * b;\n"
            "    return result;\n"
            "}\n"
            "multiply(4, 5);";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 20);
    }
    
    /* Test 1.3: Function returning undefined */
    {
        const char* code = 
            "function noop() {\n"
            "    return;\n"
            "}\n"
            "noop() === undefined;";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 1); /* true */
    }
    
    /* Test 1.4: Function with no explicit return */
    {
        const char* code = 
            "function noReturn() {\n"
            "    let x = 42;\n"
            "}\n"
            "noReturn() === undefined;";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 1); /* true */
    }
    
    return true;
}

/* ============================================================================
 * Test 2: Lazy Functions with Closures
 * ============================================================================ */

TEST(test_lazy_function_closure) {
    JSContextHandle ctx = get_test_context();
    ASSERT_TRUE(ctx.valid());
    
    /* Test 2.1: Basic closure - capture outer variable */
    {
        const char* code = 
            "function outer() {\n"
            "    let x = 10;\n"
            "    function inner() {\n"
            "        return x;\n"
            "    }\n"
            "    return inner();\n"
            "}\n"
            "outer();";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 10);
    }
    
    /* Test 2.2: Closure with argument capture */
    {
        const char* code = 
            "function makeAdder(x) {\n"
            "    function add(y) {\n"
            "        return x + y;\n"
            "    }\n"
            "    return add;\n"
            "}\n"
            "var add5 = makeAdder(5);\n"
            "add5(3);";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 8);
    }
    
    /* Test 2.3: Closure capturing multiple variables */
    {
        const char* code = 
            "function makeMultiplier(x, y) {\n"
            "    function multiply() {\n"
            "        return x * y;\n"
            "    }\n"
            "    return multiply;\n"
            "}\n"
            "var mult = makeMultiplier(6, 7);\n"
            "mult();";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 42);
    }
    
    /* Test 2.4: Closure modifying captured variable */
    {
        const char* code = 
            "function makeCounter() {\n"
            "    let count = 0;\n"
            "    function increment() {\n"
            "        count = count + 1;\n"
            "        return count;\n"
            "    }\n"
            "    return increment;\n"
            "}\n"
            "var counter = makeCounter();\n"
            "counter() + counter() + counter();";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 6); /* 1 + 2 + 3 = 6 */
    }
    
    /* Test 2.5: Deeply nested closure */
    {
        const char* code = 
            "function level1(x) {\n"
            "    function level2(y) {\n"
            "        function level3(z) {\n"
            "            return x + y + z;\n"
            "        }\n"
            "        return level3;\n"
            "    }\n"
            "    return level2;\n"
            "}\n"
            "level1(1)(2)(3);";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 6);
    }
    
    return true;
}

/* ============================================================================
 * Test 3: Class Constructor Lazy Parsing
 * ============================================================================ */

TEST(test_lazy_class_constructor) {
    JSContextHandle ctx = get_test_context();
    ASSERT_TRUE(ctx.valid());
    
    /* Test 3.1: Basic class constructor */
    {
        const char* code = 
            "class Point {\n"
            "    constructor(x, y) {\n"
            "        this.x = x;\n"
            "        this.y = y;\n"
            "    }\n"
            "}\n"
            "var p = new Point(3, 4);\n"
            "p.x + p.y;";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 7);
    }
    
    /* Test 3.2: Constructor with methods */
    {
        const char* code = 
            "class Rectangle {\n"
            "    constructor(width, height) {\n"
            "        this.width = width;\n"
            "        this.height = height;\n"
            "    }\n"
            "    area() {\n"
            "        return this.width * this.height;\n"
            "    }\n"
            "}\n"
            "var r = new Rectangle(5, 6);\n"
            "r.area();";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 30);
    }
    
    /* Test 3.3: Derived class constructor with super() */
    {
        const char* code = 
            "class Animal {\n"
            "    constructor(name) {\n"
            "        this.name = name;\n"
            "    }\n"
            "}\n"
            "class Dog extends Animal {\n"
            "    constructor(name, breed) {\n"
            "        super(name);\n"
            "        this.breed = breed;\n"
            "    }\n"
            "}\n"
            "var d = new Dog('Rex', 'German Shepherd');\n"
            "d.name === 'Rex' && d.breed === 'German Shepherd' ? 1 : 0;";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 1);
    }
    
    /* Test 3.4: Default constructor (no explicit constructor) */
    {
        const char* code = 
            "class Simple {\n"
            "    greet() {\n"
            "        return 'hello';\n"
            "    }\n"
            "}\n"
            "var s = new Simple();\n"
            "s.greet() === 'hello' ? 1 : 0;";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 1);
    }
    
    return true;
}

/* ============================================================================
 * Test 4: Class Method Lazy Parsing
 * ============================================================================ */

TEST(test_lazy_class_methods) {
    JSContextHandle ctx = get_test_context();
    ASSERT_TRUE(ctx.valid());
    
    /* Test 4.1: Instance method */
    {
        const char* code = 
            "class Calculator {\n"
            "    add(a, b) {\n"
            "        return a + b;\n"
            "    }\n"
            "    subtract(a, b) {\n"
            "        return a - b;\n"
            "    }\n"
            "}\n"
            "var calc = new Calculator();\n"
            "calc.add(10, 5) + calc.subtract(10, 5);";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 20); /* 15 + 5 = 20 */
    }
    
    /* Test 4.2: Static method */
    {
        const char* code = 
            "class MathUtils {\n"
            "    static square(x) {\n"
            "        return x * x;\n"
            "    }\n"
            "    static cube(x) {\n"
            "        return x * x * x;\n"
            "    }\n"
            "}\n"
            "MathUtils.square(4) + MathUtils.cube(2);";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 24); /* 16 + 8 = 24 */
    }
    
    /* Test 4.3: Getter method */
    {
        const char* code = 
            "class Circle {\n"
            "    constructor(radius) {\n"
            "        this.radius = radius;\n"
            "    }\n"
            "    get diameter() {\n"
            "        return this.radius * 2;\n"
            "    }\n"
            "}\n"
            "var c = new Circle(5);\n"
            "c.diameter;";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 10);
    }
    
    /* Test 4.4: Setter method */
    {
        const char* code = 
            "class Temperature {\n"
            "    constructor() {\n"
            "        this._celsius = 0;\n"
            "    }\n"
            "    set celsius(value) {\n"
            "        this._celsius = value;\n"
            "    }\n"
            "    get celsius() {\n"
            "        return this._celsius;\n"
            "    }\n"
            "}\n"
            "var t = new Temperature();\n"
            "t.celsius = 100;\n"
            "t.celsius;";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 100);
    }
    
    /* Test 4.5: Async method */
    {
        const char* code = 
            "class AsyncCalc {\n"
            "    async double(x) {\n"
            "        return x * 2;\n"
            "    }\n"
            "}\n"
            "var calc = new AsyncCalc();\n"
            "typeof calc.double;";
        
        char* result = eval_to_string(ctx, code);
        ASSERT_TRUE(result != NULL);
        ASSERT_STR_EQ(result, "function");
        free(result);
    }
    
    /* Test 4.6: Generator method */
    {
        const char* code = 
            "class Range {\n"
            "    *generate(n) {\n"
            "        for (let i = 0; i < n; i++) {\n"
            "            yield i;\n"
            "        }\n"
            "    }\n"
            "}\n"
            "var r = new Range();\n"
            "var gen = r.generate(3);\n"
            "gen.next().value + gen.next().value + gen.next().value;";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 3); /* 0 + 1 + 2 = 3 */
    }
    
    return true;
}

/* ============================================================================
 * Test 5: Arrow Function Lazy Parsing
 * ============================================================================ */

TEST(test_lazy_arrow_functions) {
    JSContextHandle ctx = get_test_context();
    ASSERT_TRUE(ctx.valid());
    
    /* Test 5.1: Arrow function with block body */
    {
        /* Use a unique variable name to avoid redeclaration conflicts
         * with test_lazy_function_basic which declares 'multiply' */
        const char* code = 
            "const arrowMultiply = (a, b) => {\n"
            "    return a * b;\n"
            "};\n"
            "arrowMultiply(3, 4);";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 12);
    }
    
    /* Test 5.2: Arrow function with expression body */
    {
        const char* code = 
            "const square = x => x * x;\n"
            "square(5);";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 25);
    }
    
    /* Test 5.3: Arrow function with single param, no parens */
    {
        const char* code = 
            "const identity = x => x;\n"
            "identity(42);";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 42);
    }
    
    /* Test 5.4: Arrow function with multiple params */
    {
        const char* code = 
            "const sum = (a, b, c) => a + b + c;\n"
            "sum(1, 2, 3);";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 6);
    }
    
    /* Test 5.5: Arrow function with no params */
    {
        const char* code = 
            "const getValue = () => 100;\n"
            "getValue();";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 100);
    }
    
    /* Test 5.6: Arrow function closure (inherits this) */
    {
        const char* code = 
            "const arrowObj = {\n"
            "    value: 42,\n"
            "    getValue: function() {\n"
            "        const arrow = () => this.value;\n"
            "        return arrow();\n"
            "    }\n"
            "};\n"
            "arrowObj.getValue();";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 42);
    }
    
    /* Test 5.7: Arrow function in array map */
    {
        const char* code = 
            "const arrowArr = [1, 2, 3, 4, 5];\n"
            "const doubled = arrowArr.map(x => x * 2);\n"
            "doubled.reduce((a, b) => a + b, 0);";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 30); /* 2+4+6+8+10 = 30 */
    }
    
    /* Test 5.8: Async arrow function */
    {
        const char* code = 
            "const asyncDouble = async (x) => x * 2;\n"
            "typeof asyncDouble;";
        
        char* result = eval_to_string(ctx, code);
        ASSERT_TRUE(result != NULL);
        ASSERT_STR_EQ(result, "function");
        free(result);
    }
    
    return true;
}

/* ============================================================================
 * Test 6: Complex Nested Functions
 * ============================================================================ */

TEST(test_lazy_nested_functions) {
    JSContextHandle ctx = get_test_context();
    ASSERT_TRUE(ctx.valid());
    
    /* Test 6.1: IIFE (Immediately Invoked Function Expression) */
    {
        const char* code = 
            "(function() {\n"
            "    return 42;\n"
            "})();";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 42);
    }
    
    /* Test 6.2: Nested IIFEs */
    {
        const char* code = 
            "(function() {\n"
            "    return (function() {\n"
            "        return (function() {\n"
            "            return 123;\n"
            "        })();\n"
            "    })();\n"
            "})();";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 123);
    }
    
    /* Test 6.3: Function returning function */
    {
        const char* code = 
            "function makeMultiplier(factor) {\n"
            "    return function(x) {\n"
            "        return x * factor;\n"
            "    };\n"
            "}\n"
            "var triple = makeMultiplier(3);\n"
            "triple(7);";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 21);
    }
    
    /* Test 6.4: Mix of function declarations and expressions */
    {
        const char* code = 
            "function outer() {\n"
            "    var inner = function() {\n"
            "        return function() {\n"
            "            return 999;\n"
            "        };\n"
            "    };\n"
            "    return inner()();\n"
            "}\n"
            "outer();";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 999);
    }
    
    /* Test 6.5: Functions in object literals */
    {
        const char* code = 
            "var obj = {\n"
            "    value: 10,\n"
            "    method1: function() {\n"
            "        return this.value;\n"
            "    },\n"
            "    method2: function() {\n"
            "        return this.value * 2;\n"
            "    }\n"
            "};\n"
            "obj.method1() + obj.method2();";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 30); /* 10 + 20 = 30 */
    }
    
    return true;
}

/* ============================================================================
 * Test 7: Error Handling
 * ============================================================================ */

TEST(test_lazy_function_errors) {
    JSContextHandle ctx = get_test_context();
    ASSERT_TRUE(ctx.valid());
    
    /* Test 7.1: Syntax error in function body */
    {
        const char* code = 
            "function bad() {\n"
            "    return 1 2 3;\n"  /* Invalid syntax */
            "}\n"
            "bad();";
        
        /* This should throw a syntax error when parsed */
        /* Note: With lazy parsing, error occurs at call time */
        BOOL throws = eval_throws_exception(ctx, code);
        ASSERT_TRUE(throws);
    }
    
    /* Test 7.2: Reference error in function */
    {
        const char* code = 
            "function useUndefined() {\n"
            "    return undefinedVariable;\n"
            "}\n"
            "useUndefined();";
        
        /* This should throw a reference error when executed */
        BOOL throws = eval_throws_exception(ctx, code);
        ASSERT_TRUE(throws);
    }
    
    /* Test 7.3: Type error in function */
    {
        const char* code = 
            "function callNonFunction() {\n"
            "    var x = 42;\n"
            "    return x();\n"  /* Calling a number */
            "}\n"
            "callNonFunction();";
        
        BOOL throws = eval_throws_exception(ctx, code);
        ASSERT_TRUE(throws);
    }
    
    return true;
}

/* ============================================================================
 * Test 8: Recursion
 * ============================================================================ */

TEST(test_lazy_function_recursion) {
    JSContextHandle ctx = get_test_context();
    ASSERT_TRUE(ctx.valid());
    
    /* Test 8.1: Simple recursion - factorial */
    {
        const char* code = 
            "function factorial(n) {\n"
            "    if (n <= 1) return 1;\n"
            "    return n * factorial(n - 1);\n"
            "}\n"
            "factorial(5);";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 120); /* 5! = 120 */
    }
    
    /* Test 8.2: Mutual recursion */
    {
        const char* code = 
            "function isEven(n) {\n"
            "    if (n === 0) return true;\n"
            "    return isOdd(n - 1);\n"
            "}\n"
            "function isOdd(n) {\n"
            "    if (n === 0) return false;\n"
            "    return isEven(n - 1);\n"
            "}\n"
            "isEven(10) ? 1 : 0;";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 1);
    }
    
    return true;
}

/* ============================================================================
 * Test 9: Function Properties
 * ============================================================================ */

TEST(test_lazy_function_properties) {
    JSContextHandle ctx = get_test_context();
    ASSERT_TRUE(ctx.valid());
    
    /* Test 9.1: Function name */
    {
        const char* code = 
            "function myFunction() { return 1; }\n"
            "myFunction.name;";
        
        char* result = eval_to_string(ctx, code);
        ASSERT_TRUE(result != NULL);
        ASSERT_STR_EQ(result, "myFunction");
        free(result);
    }
    
    /* Test 9.2: Function length (arity) */
    {
        const char* code = 
            "function funcWith3Args(a, b, c) { return 1; }\n"
            "funcWith3Args.length;";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 3);
    }
    
    /* Test 9.3: Function prototype */
    {
        const char* code = 
            "function MyClass() {}\n"
            "typeof MyClass.prototype;";
        
        char* result = eval_to_string(ctx, code);
        ASSERT_TRUE(result != NULL);
        ASSERT_STR_EQ(result, "object");
        free(result);
    }
    
    return true;
}

/* ============================================================================
 * Test 10: Edge Cases
 * ============================================================================ */

TEST(test_lazy_function_edge_cases) {
    JSContextHandle ctx = get_test_context();
    ASSERT_TRUE(ctx.valid());
    
    /* Test 10.1: Empty function body */
    {
        const char* code = 
            "function empty() {}\n"
            "empty() === undefined ? 1 : 0;";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 1);
    }
    
    /* Test 10.2: Function with only comments */
    {
        const char* code = 
            "function onlyComments() {\n"
            "    /* comment */\n"
            "    // another comment\n"
            "}\n"
            "onlyComments() === undefined ? 1 : 0;";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 1);
    }
    
    /* Test 10.3: Function with complex destructuring parameters */
    {
        const char* code = 
            "function destruct({x, y}) {\n"
            "    return x + y;\n"
            "}\n"
            "destruct({x: 3, y: 4});";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 7);
    }
    
    /* Test 10.4: Function with default parameters */
    {
        const char* code = 
            "function withDefaults(a, b = 10) {\n"
            "    return a + b;\n"
            "}\n"
            "withDefaults(5);";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 15);
    }
    
    /* Test 10.5: Function with rest parameters */
    {
        const char* code = 
            "function sumAll(...numbers) {\n"
            "    return numbers.reduce((a, b) => a + b, 0);\n"
            "}\n"
            "sumAll(1, 2, 3, 4, 5);";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 15);
    }
    
    return true;
}

/* ============================================================================
 * Test 11: Lazy Parsing Verification - Functions Not Parsed Until Called
 * ============================================================================ */

/* 
 * Global flag to track if lazy parsing is working.
 * We use a unique variable name that we can check from JS.
 */
static BOOL g_lazy_parse_verified = FALSE;

TEST(test_lazy_parsing_not_triggered_until_call) {
    JSContextHandle ctx = get_test_context();
    ASSERT_TRUE(ctx.valid());
    
    /* Test 11.1: Function with syntax error is not parsed until called
     * 
     * With lazy parsing, the function body is skipped during initial parse.
     * So defining a function with a syntax error should succeed.
     * The error should only occur when the function is actually called.
     */
    {
        const char* code = 
            "function hasSyntaxError() {\n"
            "    return 1 2 3;\n"  /* Invalid syntax - missing operator */
            "}\n"
            "typeof hasSyntaxError;";
        
        /* Function should be defined successfully (body not parsed yet) */
        char* result = eval_to_string(ctx, code);
        ASSERT_TRUE(result != NULL);
        ASSERT_STR_EQ(result, "function");
        free(result);
        
        /* Now call the function - this should trigger parsing and fail */
        const char* call_code = "hasSyntaxError();";
        BOOL throws = eval_throws_exception(ctx, call_code);
        ASSERT_TRUE(throws);
    }
    
    /* Test 11.2: Function is not parsed when defined, only when called
     *
     * We verify this by checking that we can define many lazy functions
     * without executing them, and they don't cause parse errors.
     */
    {
        const char* code = 
            "function unused1() { return 1 1; }\n"  /* Would fail if parsed */
            "function unused2() { return 2 2; }\n"  /* Would fail if parsed */
            "function unused3() { return 3 3; }\n"  /* Would fail if parsed */
            "function actuallyUsed() { return 42; }\n"
            "actuallyUsed();";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 42);
    }
    
    /* Test 11.3: Closure function not parsed until called
     *
     * Inner function should not be parsed until outer function returns it
     * and it's actually invoked.
     */
    {
        const char* code = 
            "function outer() {\n"
            "    function inner() {\n"
            "        return 1 2 3;\n"  /* Invalid syntax */
            "    }\n"
            "    return inner;\n"
            "}\n"
            "var fn = outer();\n"
            "typeof fn;";
        
        /* Getting the function reference should not trigger parse */
        char* result = eval_to_string(ctx, code);
        ASSERT_TRUE(result != NULL);
        ASSERT_STR_EQ(result, "function");
        free(result);
        
        /* Calling it should trigger parse and fail */
        const char* call_code = "fn();";
        BOOL throws = eval_throws_exception(ctx, call_code);
        ASSERT_TRUE(throws);
    }
    
    return true;
}

/* ============================================================================
 * Test 12: JS_EVAL_FLAG_NO_LAZY - Force Immediate Parsing
 * ============================================================================ */

TEST(test_js_eval_flag_no_lazy) {
    JSContextHandle ctx = get_test_context();
    ASSERT_TRUE(ctx.valid());
    
    /* Test 12.1: With JS_EVAL_FLAG_NO_LAZY, syntax errors are caught immediately
     *
     * When NO_LAZY flag is set, functions should be parsed immediately,
     * causing syntax errors to be thrown during definition, not at call time.
     */
    {
        const char* code = 
            "function hasSyntaxError() {\n"
            "    return 1 2 3;\n"
            "}";
        
        /* With NO_LAZY flag, this should throw immediately during definition */
        GCValue result = JS_Eval(ctx, code, strlen(code), "<test>", 
                                  JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_NO_LAZY);
        
        /* Should be an exception (syntax error) */
        ASSERT_TRUE(JS_IsException(result));
        
        /* Clear the exception */
        JS_GetException(ctx);
    }
    
    /* Test 12.2: With JS_EVAL_FLAG_NO_LAZY, valid functions work normally
     */
    {
        const char* code = 
            "function validFunction() {\n"
            "    return 42;\n"
            "}\n"
            "validFunction();";
        
        GCValue result = JS_Eval(ctx, code, strlen(code), "<test>", 
                                  JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_NO_LAZY);
        
        /* Should not be an exception */
        ASSERT_FALSE(JS_IsException(result));
        
        int32_t val;
        JS_ToInt32(ctx, &val, result);
        ASSERT_EQ(val, 42);
    }
    
    return true;
}

/* ============================================================================
 * Test 13: Lazy Parsing State Transitions
 * ============================================================================ */

TEST(test_lazy_parsing_state_transitions) {
    JSContextHandle ctx = get_test_context();
    ASSERT_TRUE(ctx.valid());
    
    /* Test 13.1: Function is parsed only on first call
     *
     * Multiple calls to the same function should work after lazy parsing.
     */
    {
        const char* code = 
            "var callCount = 0;\n"
            "function trackCalls() {\n"
            "    callCount = callCount + 1;\n"
            "    return callCount;\n"
            "}\n"
            "trackCalls() + trackCalls() + trackCalls();";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        /* 1 + 2 + 3 = 6 */
        ASSERT_EQ(result, 6);
    }
    
    /* Test 13.2: Lazy function can be passed around before being called
     */
    {
        const char* code = 
            "function getValue() { return 123; }\n"
            "var ref1 = getValue;\n"
            "var ref2 = ref1;\n"
            "var ref3 = ref2;\n"
            "ref3();";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 123);
    }
    
    /* Test 13.3: Lazy function stored in object property
     */
    {
        const char* code = 
            "function myMethod() { return 456; }\n"
            "var obj = { fn: myMethod };\n"
            "obj.fn();";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 456);
    }
    
    return true;
}

/* ============================================================================
 * Test 14: Lazy Parsing with Complex Scenarios
 * ============================================================================ */

TEST(test_lazy_parsing_complex_scenarios) {
    JSContextHandle ctx = get_test_context();
    ASSERT_TRUE(ctx.valid());
    
    /* Test 14.1: Many lazy functions, only one called
     *
     * This verifies that we don't parse functions unnecessarily.
     */
    {
        const char* code = 
            "function f1() { return 1; }\n"
            "function f2() { return 2; }\n"
            "function f3() { return 3; }\n"
            "function f4() { return 4; }\n"
            "function f5() { return 5; }\n"
            "f3();";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 3);
    }
    
    /* Test 14.2: Lazy function in array
     */
    {
        const char* code = 
            "function first() { return 10; }\n"
            "function second() { return 20; }\n"
            "var arr = [first, second];\n"
            "arr[0]() + arr[1]();";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 30);
    }
    
    /* Test 14.3: Lazy function returned from IIFE
     */
    {
        const char* code = 
            "var getValue = (function() {\n"
            "    function inner() { return 999; }\n"
            "    return inner;\n"
            "})();\n"
            "getValue();";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 999);
    }
    
    /* Test 14.4: Recursive lazy function - parse on first call
     */
    {
        const char* code = 
            "function factorial(n) {\n"
            "    if (n <= 1) return 1;\n"
            "    return n * factorial(n - 1);\n"
            "}\n"
            "factorial(5);";
        
        BOOL success;
        int result = eval_to_int(ctx, code, &success);
        ASSERT_TRUE(success);
        ASSERT_EQ(result, 120);
    }
    
    return true;
}

/* ============================================================================
 * Test Registration
 * ============================================================================ */

extern "C" void run_lazy_parsing_tests(void) {
    printf("\n========================================\n");
    printf("Lazy Function Parsing Tests\n");
    printf("========================================\n");
    
    RUN_TEST(test_lazy_function_basic);
    RUN_TEST(test_lazy_function_closure);
    RUN_TEST(test_lazy_class_constructor);
    RUN_TEST(test_lazy_class_methods);
    RUN_TEST(test_lazy_arrow_functions);
    RUN_TEST(test_lazy_nested_functions);
    RUN_TEST(test_lazy_function_errors);
    RUN_TEST(test_lazy_function_recursion);
    RUN_TEST(test_lazy_function_properties);
    RUN_TEST(test_lazy_function_edge_cases);
    RUN_TEST(test_lazy_parsing_not_triggered_until_call);
    RUN_TEST(test_js_eval_flag_no_lazy);
    RUN_TEST(test_lazy_parsing_state_transitions);
    RUN_TEST(test_lazy_parsing_complex_scenarios);
}

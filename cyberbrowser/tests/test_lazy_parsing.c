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
JSContextHandle get_shared_test_context(void);

/* Helper to get the shared test context */
static JSContextHandle get_test_context(void) {
    JSContextHandle ctx = get_shared_test_context();
    if (!SAFE_VALID(ctx)) {
        printf("    ERROR: Shared context not available\n");
        return GC_HANDLE_NULL;
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
    ASSERT_TRUE(SAFE_VALID(ctx));
    
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
    ASSERT_TRUE(SAFE_VALID(ctx));
    
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
    ASSERT_TRUE(SAFE_VALID(ctx));
    
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
    ASSERT_TRUE(SAFE_VALID(ctx));
    
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
    ASSERT_TRUE(SAFE_VALID(ctx));
    
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
    ASSERT_TRUE(SAFE_VALID(ctx));
    
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
    ASSERT_TRUE(SAFE_VALID(ctx));
    
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
    ASSERT_TRUE(SAFE_VALID(ctx));
    
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
    ASSERT_TRUE(SAFE_VALID(ctx));
    
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
    ASSERT_TRUE(SAFE_VALID(ctx));
    
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
    ASSERT_TRUE(SAFE_VALID(ctx));
    
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
    ASSERT_TRUE(SAFE_VALID(ctx));
    
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
    ASSERT_TRUE(SAFE_VALID(ctx));
    
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
    ASSERT_TRUE(SAFE_VALID(ctx));
    
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
 * Test 15: Lazy parsing conformance suite
 *
 * Scoping cases that a lazily compiled function must get exactly as an
 * eagerly compiled one: closures over loop bindings, this / arguments /
 * new.target / super in arrows, private names, TDZ through closures,
 * regexp / template / division in skipped bodies, Function.prototype.toString
 * and single evaluation of arrow bodies.  The expected output was produced
 * by Node.js from the same source.
 * ============================================================================ */

static const char *lazy_suite_js =
    "var out=[];\n"
    "function T(name, f){ try{ var v=f(); out.push(name+'='+(typeof v==='string'?v:JSON.stringify(v))); }catch(e){ out.push(name+'!'+e.name+':'+e.message); } }\n"
    "T('arrow-arguments', function(){ function outer(){ var g=()=>{ return arguments[0]; }; return g(9); } return outer(5); });\n"
    "T('arrow-this', function(){ var o={v:3,m:function(){ var g=()=>{ return this.v; }; return g(); }}; return o.m(); });\n"
    "T('arrow-newtarget', function(){ function F(){ var g=()=>{ return typeof new.target; }; this.r=g(); } return new F().r; });\n"
    "T('regex-brace', function(){ function f(){ return \"a}b\".replace(/}/g,'x'); } return f(); });\n"
    "T('regex-after-paren', function(){ function f(s){ if (s) /}/.test(s); return 1; } return f('}'); });\n"
    "T('regex-class-brace', function(){ function f(){ var r=/[{]/; return r.test('{'); } return f(); });\n"
    "T('div-not-regex', function(){ function f(a,b){ var x=a/b; var y=a/ 2; return [x,y,'}']; } return f(4,2); });\n"
    "T('template-nested', function(){ function f(a){ return `x${a ? `{${a}}` : '}'}y`; } return [f(1), f(0)]; });\n"
    "T('template-string-in-interp', function(){ function f(){ return `${\"}\"}${'{'}`; } return f(); });\n"
    "T('template-comment-in-interp', function(){ function f(){ return `${ /* } */ 1 }`; } return f(); });\n"
    "T('regex-after-keyword', function(){ function f(x){ switch(x){ case 1: return typeof /}/; } return void /}/; } return [f(1),f(2)]; });\n"
    "T('regex-flags-dv', function(){ function f(){ var r=/a\\}/d; return r.flags; } return f(); });\n"
    "T('regex-after-plus', function(){ function f(){ return 'x'+/}/.source; } return f(); });\n"
    "T('regex-after-arrow', function(){ function f(){ var g=x=>/}/.test(x); return g('}'); } return f(); });\n"
    "T('closure-capture', function(){ function outer(){ var a=1, b=2; function inner(){ return a+b; } return inner; } return outer()(); });\n"
    "T('closure-late', function(){ function outer(){ var fs=[]; for(let i=0;i<3;i++) fs.push(function(){ return i; }); return fs.map(f=>f()); } return outer(); });\n"
    "T('nested-lazy', function(){ function a(){ var x=1; function b(){ var y=2; function c(){ return x+y; } return c(); } return b(); } return a(); });\n"
    "T('default-params', function(){ function f(a=1,b=a+1){ return function(){ return a+b; }(); } return f(); });\n"
    "T('destructure-params', function(){ function f({a},[b]){ return function(){ return a+b; }(); } return f({a:1},[2]); });\n"
    "T('eval-in-lazy', function(){ function f(){ var q=7; return eval('q'); } return f(); });\n"
    "T('arguments-in-lazy', function(){ function f(){ return arguments.length; } return f(1,2,3); });\n"
    "T('async-lazy', function(){ async function f(){ return 1; } return typeof f().then; });\n"
    "T('generator-lazy', function(){ function* g(){ yield 1; yield 2; } return [...g()]; });\n"
    "T('class-method', function(){ class A{ m(){ return 1; } static s(){ return 2; } get g(){ return 3; } } return [new A().m(), A.s(), new A().g]; });\n"
    "T('super-in-lazy', function(){ class A{ m(){ return 1; } } class B extends A{ m(){ return super.m()+1; } } return new B().m(); });\n"
    "T('strict-inherit', function(){ 'use strict'; function f(){ return this; } return typeof f(); });\n"
    "T('fn-name-binding', function(){ var f=function g(){ return typeof g; }; return f(); });\n"
    "T('shadow-arguments', function(){ function f(){ var arguments=5; return arguments; } return f(); });\n"
    "T('hoisted-before', function(){ return h(); function h(){ return 'h'; } });\n"
    "T('getter-in-object', function(){ var o={ get x(){ return 4; }, y(){ return 5; } }; return [o.x,o.y()]; });\n"
    "T('label-brace', function(){ function f(){ a: { break a; } return '}'; } return f(); });\n"
    "T('string-escape', function(){ function f(){ return '\\\\'+\"\\\"}\"+'\\''; } return f(); });\n"
    "T('unicode-escape-ident', function(){ function f(){ var \\u0061b=1; return ab; } return f(); });\n"
    "T('html-comment', function(){ return 1; });\n"
    "T('regex-in-template-interp', function(){ function f(){ return `${'a}'.replace(/}/,'')}`; } return f(); });\n"
    "T('obj-literal-after-return', function(){ function f(){ return {a:'}'}; } return f().a; });\n"
    "T('with-stmt', function(){ return new Function(\"var o={z:1}; with(o){ return (function(){ return z; })(); }\")(); });\n"
    "T('new-function', function(){ return new Function('a','return function(){return a*2}()')(3); });\n"
    "T('nested3', function(){ function a(x){ function b(y){ function c(z){ return x+y+z; } return c; } return b; } return a(1)(2)(3); });\n"
    "T('loop-let-nested', function(){ var r=[]; function mk(){ var fs=[]; for(let i=0;i<3;i++){ for(let j=0;j<2;j++){ fs.push(function(){ return function(){ return i*10+j; }; }); } } return fs; } mk().forEach(function(f){ r.push(f()()); }); return r; });\n"
    "T('for-of-const', function(){ function mk(){ var fs=[]; for(const v of ['a','b','c']) fs.push(()=>{ return v; }); return fs.map(f=>f()); } return mk(); });\n"
    "T('catch-param', function(){ function mk(){ try{ throw 7; }catch(e){ return function(){ return e; }; } } return mk()(); });\n"
    "T('same-stub-many', function(){ function mk(n){ return function(){ return n*2; }; } var fs=[mk(1),mk(2),mk(3)]; return [fs[2](),fs[0](),fs[1](),fs[0]()]; });\n"
    "T('call-before-return', function(){ function o(){ var a=1; function g(){ return a; } var r1=g(); a=2; var r2=g(); return [r1,r2,g]; } var r=o(); return [r[0],r[1],r[2]()]; });\n"
    "T('mutate-after', function(){ function o(){ var a=1; var g=function(){ return a; }; a=5; return g; } return o()(); });\n"
    "T('write-outer', function(){ function o(){ var a=1; function inc(){ a++; } inc(); inc(); return a; } return o(); });\n"
    "T('hoisted-later', function(){ function o(){ function f(){ return later; } var later=5; return f(); } return o(); });\n"
    "T('tdz', function(){ function o(){ function f(){ return x; } try { f(); } catch(e) { return e.name; } let x=1; } return o(); });\n"
    "T('const-assign', function(){ function o(){ const c=1; function f(){ c=2; } try { f(); } catch(e) { return e.name; } } return o(); });\n"
    "T('typeof-undeclared', function(){ function f(){ return typeof notDeclaredAnywhere123; } return f(); });\n"
    "T('sloppy-global-assign', function(){ function f(){ implicitGlobal987 = 4; } f(); return globalThis.implicitGlobal987; });\n"
    "T('global-read', function(){ globalThis.gv1 = 11; function f(){ return gv1; } return f(); });\n"
    "T('global-later-defined', function(){ function f(){ return gvLate; } globalThis.gvLate = 3; return f(); });\n"
    "T('arrow-super-method', function(){ class A{ m(){ return 'A'; } } class B extends A{ m(){ var g=()=>{ return super.m()+'B'; }; return g(); } } return new B().m(); });\n"
    "T('arrow-super-ctor', function(){ class A{ constructor(){ this.x=1; } } class B extends A{ constructor(){ var g=()=>{ super(); }; g(); this.y=2; } } var b=new B(); return [b.x,b.y]; });\n"
    "T('arrow-newtarget2', function(){ function F(){ var g=()=>{ return (()=>{ return new.target===F; })(); }; this.r=g(); } return new F().r; });\n"
    "T('arrow-arguments2', function(){ function f(){ var g=()=>{ return (()=>{ return arguments.length; })(); }; return g(); } return f(1,2,3,4); });\n"
    "T('arrow-this-nested', function(){ var o={v:9, m(){ return [1].map(()=>{ return [2].map(()=>{ return this.v; })[0]; })[0]; }}; return o.m(); });\n"
    "T('private-fields', function(){ class C{ #x=3; #m(){ return this.#x; } get(){ return this.#m()+1; } has(o){ return #x in o; } } var c=new C(); return [c.get(), c.has(c), c.has({})]; });\n"
    "T('static-private', function(){ class C{ static #s=5; static get(){ return C.#s; } } return C.get(); });\n"
    "T('generator-closure', function(){ function mk(k){ return function*(){ yield k; yield k+1; }; } return [...mk(5)()]; });\n"
    "T('async-closure', function(){ function mk(k){ return async function(){ return k; }; } var r; mk(8)().then(v=>{ r=v; }); return typeof mk(8)().then; });\n"
    "T('async-arrow', function(){ var o={v:2, m(){ return (async()=>{ return this.v; })(); }}; return typeof o.m().then; });\n"
    "T('recursion-named', function(){ var f=function fact(n){ return n<=1?1:n*fact(n-1); }; return f(5); });\n"
    "T('recursion-decl', function(){ function fib(n){ return n<2?n:fib(n-1)+fib(n-2); } return fib(15); });\n"
    "T('block-func-annexB', function(){ function o(){ { function inner(){ return 'in'; } } return inner(); } return o(); });\n"
    "T('use-strict-body', function(){ function f(){ 'use strict'; return this; } return typeof f(); });\n"
    "T('use-strict-body-nested', function(){ function f(){ 'use strict'; function g(){ return this; } return g(); } return typeof f(); });\n"
    "T('sloppy-this', function(){ function f(){ return this === globalThis; } return f(); });\n"
    "T('eval-in-lazy2', function(){ function o(){ var q=3; function f(){ var r=4; return eval('q+r'); } return f(); } return o(); });\n"
    "T('eval-in-parent', function(){ function o(){ var q=3; var g=function(){ return q; }; eval('q=6'); return g(); } return o(); });\n"
    "T('with-sloppy', function(){ return new Function(\"var x='outer'; var o={y:'wy'}; function f(){ with(o){ return x+','+y; } } return f();\")(); });\n"
    "T('label-break', function(){ function f(){ outer: for(var i=0;i<3;i++){ for(var j=0;j<3;j++){ if(j==1) continue outer; if(i==2) break outer; } } return i; } return f(); });\n"
    "T('regex-if', function(){ function f(s){ if (s) /}/.test(s); return 'ok'; } return f('x'); });\n"
    "T('regex-after-block', function(){ function f(s){ if(s){ s+=1 } /}/.test(s); return 'ok2'; } return f('x'); });\n"
    "T('obj-div', function(){ function f(){ var x = {a:4}.a / 2; return x; } return f(); });\n"
    "T('template-deep', function(){ function f(a){ return `${`${`${a}}`}`}{`; } return f(1); });\n"
    "T('template-regex', function(){ function f(){ return `${'x}'.replace(/[}]/g,'y')}`; } return f(); });\n"
    "T('comment-braces', function(){ function f(){ // }\n"
    "  /* } */ return 'c'; } return f(); });\n"
    "T('string-braces', function(){ function f(){ return \"}\" + '{' + \"\\\"}\"; } return f(); });\n"
    "T('div-ident', function(){ function f(a,b,g){ return a / b / g; } return f(8,2,2); });\n"
    "T('keyword-prop-div', function(){ function f(){ var o={default:8, new:4}; return o.default / o.new; } return f(); });\n"
    "T('getter-setter', function(){ var o={ _v:1, get v(){ return this._v; }, set v(x){ this._v=x*2; } }; o.v=5; return o.v; });\n"
    "T('class-methods', function(){ class K{ static s(){ return 's'; } m(){ return 'm'; } get g(){ return 'g'; } *gen(){ yield 'y'; } async am(){ return 1; } } var k=new K(); return [K.s(), k.m(), k.g, [...k.gen()][0], typeof k.am().then]; });\n"
    "T('class-name-binding', function(){ class Q{ m(){ return Q.name; } } return new Q().m(); });\n"
    "T('class-expr-name', function(){ var C = class Named { m(){ return typeof Named; } }; return new C().m(); });\n"
    "T('fn-length-name', function(){ function abc(a,b,c){ return 1; } var g=function(x){}; return [abc.length, abc.name, g.name, (()=>{}).name]; });\n"
    "T('tostring-variants', function(){ var o={ m(a){ return a; }, get g(){ return 1; }, async *ag(){ } }; class C{ static s(){ return 2; } } return [o.m.toString(), Object.getOwnPropertyDescriptor(o,'g').get.toString(), o.ag.toString(), C.s.toString(), (async function nm(){ }).toString()]; });\n"
    "T('arguments-mapped', function(){ function f(a){ arguments[0]=9; return a; } return f(1); });\n"
    "T('arguments-strict', function(){ function f(a){ 'use strict'; arguments[0]=9; return a; } return f(1); });\n"
    "T('default-param-closure', function(){ function f(a, g=()=>a){ return g(); } return f(7); });\n"
    "T('param-shadow', function(){ var x='outer'; function f(x){ return function(){ return x; }; } return f('param')(); });\n"
    "T('func-expr-self', function(){ var outer=function self(n){ return n? self(n-1): typeof self; }; return outer(3); });\n"
    "T('closure-counter-many', function(){ var cs=[]; for(var i=0;i<100;i++){ cs.push((function(k){ var c=k; return function(){ return ++c; }; })(i)); } cs[5](); return [cs[5](), cs[99](), cs[0]()]; });\n"
    "T('new-function-closure', function(){ var F=new Function('a','return function(b){ return function(){ return a+b; }; }'); return F(1)(2)(); });\n"
    "T('json-reviver', function(){ return JSON.parse('{\"a\":1}', function(k,v){ return typeof v==='number'? v+1 : v; }).a; });\n"
    "T('sort-comparator', function(){ return [3,1,2].sort(function(a,b){ return a-b; }); });\n"
    "T('settimeout-like', function(){ var q=[]; function later(cb){ q.push(cb); } var v=1; later(function(){ return v; }); v=2; return q[0](); });\n"
    "T('expr-arrow-once', function(){ var n=0; var f = x => (n++, x*2); f(3); f(4); var g=()=>{ n+=10; }; g(); return n; });\n"
    "T('expr-arrow-counter', function(){ function mk(){ var c=0; return () => ++c; } var h=mk(); h(); h(); return h(); });\n"
    "T('tostring-decl', function(){ function foo(a, b) { return a + b; } return String(foo); });\n"
    "T('tostring-arrow', function(){ return String(() => { return 1; }) + '|' + String(x => x + 1); });\n"
    "T('tdz-closure-decl', function(){ function f(){ return x; } try { f(); } catch(e) { return e.name; } let x=1; });\n"
    "T('tdz-closure-arrow', function(){ var g=()=>{ return z; }; try { g(); } catch(e) { return e.name; } const z=1; });\n"
    "T('tdz-closure-class', function(){ class C { m(){ return K; } } try { new C().m(); } catch(e) { return e.name; } let K=1; });\n"
    "out.join('\\n');\n";

static const char *lazy_suite_expected =
    "arrow-arguments=5\n"
    "arrow-this=3\n"
    "arrow-newtarget=function\n"
    "regex-brace=axb\n"
    "regex-after-paren=1\n"
    "regex-class-brace=true\n"
    "div-not-regex=[2,2,\"}\"]\n"
    "template-nested=[\"x{1}y\",\"x}y\"]\n"
    "template-string-in-interp=}{\n"
    "template-comment-in-interp=1\n"
    "regex-after-keyword=[\"object\",null]\n"
    "regex-flags-dv=d\n"
    "regex-after-plus=x}\n"
    "regex-after-arrow=true\n"
    "closure-capture=3\n"
    "closure-late=[0,1,2]\n"
    "nested-lazy=3\n"
    "default-params=3\n"
    "destructure-params=3\n"
    "eval-in-lazy=7\n"
    "arguments-in-lazy=3\n"
    "async-lazy=function\n"
    "generator-lazy=[1,2]\n"
    "class-method=[1,2,3]\n"
    "super-in-lazy=2\n"
    "strict-inherit=undefined\n"
    "fn-name-binding=function\n"
    "shadow-arguments=5\n"
    "hoisted-before=h\n"
    "getter-in-object=[4,5]\n"
    "label-brace=}\n"
    "string-escape=\\\"}'\n"
    "unicode-escape-ident=1\n"
    "html-comment=1\n"
    "regex-in-template-interp=a\n"
    "obj-literal-after-return=}\n"
    "with-stmt=1\n"
    "new-function=6\n"
    "nested3=6\n"
    "loop-let-nested=[0,1,10,11,20,21]\n"
    "for-of-const=[\"a\",\"b\",\"c\"]\n"
    "catch-param=7\n"
    "same-stub-many=[6,2,4,2]\n"
    "call-before-return=[1,2,2]\n"
    "mutate-after=5\n"
    "write-outer=3\n"
    "hoisted-later=5\n"
    "tdz=ReferenceError\n"
    "const-assign=TypeError\n"
    "typeof-undeclared=undefined\n"
    "sloppy-global-assign=4\n"
    "global-read=11\n"
    "global-later-defined=3\n"
    "arrow-super-method=AB\n"
    "arrow-super-ctor=[1,2]\n"
    "arrow-newtarget2=true\n"
    "arrow-arguments2=4\n"
    "arrow-this-nested=9\n"
    "private-fields=[4,true,false]\n"
    "static-private=5\n"
    "generator-closure=[5,6]\n"
    "async-closure=function\n"
    "async-arrow=function\n"
    "recursion-named=120\n"
    "recursion-decl=610\n"
    "block-func-annexB=in\n"
    "use-strict-body=undefined\n"
    "use-strict-body-nested=undefined\n"
    "sloppy-this=true\n"
    "eval-in-lazy2=7\n"
    "eval-in-parent=6\n"
    "with-sloppy=outer,wy\n"
    "label-break=2\n"
    "regex-if=ok\n"
    "regex-after-block=ok2\n"
    "obj-div=2\n"
    "template-deep=1}{\n"
    "template-regex=xy\n"
    "comment-braces=c\n"
    "string-braces=}{\"}\n"
    "div-ident=2\n"
    "keyword-prop-div=2\n"
    "getter-setter=10\n"
    "class-methods=[\"s\",\"m\",\"g\",\"y\",\"function\"]\n"
    "class-name-binding=Q\n"
    "class-expr-name=function\n"
    "fn-length-name=[3,\"abc\",\"g\",\"\"]\n"
    "tostring-variants=[\"m(a){ return a; }\",\"get g(){ return 1; }\",\"async *ag(){ }\",\"s(){ return 2; }\",\"async function nm(){ }\"]\n"
    "arguments-mapped=9\n"
    "arguments-strict=1\n"
    "default-param-closure=7\n"
    "param-shadow=param\n"
    "func-expr-self=function\n"
    "closure-counter-many=[7,100,1]\n"
    "new-function-closure=3\n"
    "json-reviver=2\n"
    "sort-comparator=[1,2,3]\n"
    "settimeout-like=2\n"
    "expr-arrow-once=12\n"
    "expr-arrow-counter=3\n"
    "tostring-decl=function foo(a, b) { return a + b; }\n"
    "tostring-arrow=() => { return 1; }|x => x + 1\n"
    "tdz-closure-decl=ReferenceError\n"
    "tdz-closure-arrow=ReferenceError\n"
    "tdz-closure-class=ReferenceError";

static BOOL lazy_suite_check(JSContextHandle ctx, const char *label)
{
    char *got = eval_to_string(ctx, lazy_suite_js);
    BOOL ok = got && strcmp(got, lazy_suite_expected) == 0;
    if (!ok && got) {
        /* report the first differing line */
        const char *a = got, *b = lazy_suite_expected;
        while (*a && *a == *b) { a++; b++; }
        while (a > got && a[-1] != '\n') { a--; b--; }
        printf("    [%s] first mismatch:\n      got:      %.120s\n      expected: %.120s\n",
               label, a, b);
    }
    free(got);
    return ok;
}

TEST(test_lazy_conformance_suite) {
    JSContextHandle ctx = get_test_context();
    ASSERT_TRUE(SAFE_VALID(ctx));
    ASSERT_TRUE(lazy_suite_check(ctx, "lazy"));
    return true;
}

/* ============================================================================
 * Test Registration
 * ============================================================================ */

void run_lazy_parsing_tests(void) {
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
    RUN_TEST(test_lazy_conformance_suite);
}

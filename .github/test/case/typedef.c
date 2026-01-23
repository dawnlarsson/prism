// Test typedef tracking for zero-init

#include <stdio.h>
#include <string.h>

static int passed = 0;
static int total = 0;

#define CHECK(cond, name) do { \
    total++; \
    if (cond) { printf("[PASS] %s\n", name); passed++; } \
    else { printf("[FAIL] %s\n", name); } \
} while(0)

// Test 1: Simple typedef
typedef int MyInt;

void test_simple_typedef(void) {
    MyInt x;
    CHECK(x == 0, "simple typedef zero-init");
}

// Test 2: Multiple typedef names
typedef int IntA, *IntPtrA;

void test_multiple_typedef_names(void) {
    IntA a;
    IntPtrA p;
    CHECK(a == 0, "multiple typedef - first name");
    CHECK(p == NULL, "multiple typedef - pointer name");
}

// Test 3: Struct typedef
typedef struct {
    int x;
    int y;
} Point;

void test_struct_typedef(void) {
    Point p;
    CHECK(p.x == 0 && p.y == 0, "struct typedef zero-init");
}

// Test 4: Named struct typedef
typedef struct Vec2 {
    float x;
    float y;
} Vec2;

void test_named_struct_typedef(void) {
    Vec2 v;
    CHECK(v.x == 0.0f && v.y == 0.0f, "named struct typedef zero-init");
}

// Test 5: Pointer typedef
typedef char *String;

void test_pointer_typedef(void) {
    String s;
    CHECK(s == NULL, "pointer typedef zero-init");
}

// Test 6: Double pointer typedef
typedef int **IntPtrPtr;

void test_double_pointer_typedef(void) {
    IntPtrPtr pp;
    CHECK(pp == NULL, "double pointer typedef zero-init");
}

// Test 7: Array typedef
typedef char Name[64];

void test_array_typedef(void) {
    Name n;
    int all_zero = 1;
    for (int i = 0; i < 64; i++) {
        if (n[i] != 0) all_zero = 0;
    }
    CHECK(all_zero, "array typedef zero-init");
}

// Test 8: Function pointer typedef
typedef int (*Callback)(int, int);

void test_func_ptr_typedef(void) {
    Callback cb;
    CHECK(cb == NULL, "function pointer typedef zero-init");
}

// Test 9: Typedef of typedef (chained)
typedef MyInt ChainedInt;

void test_chained_typedef(void) {
    ChainedInt c;
    CHECK(c == 0, "chained typedef zero-init");
}

// Test 10: Typedef with const qualifier
typedef const int ConstInt;

void test_const_typedef(void) {
    // const vars must be initialized, so we can't test zero-init directly
    // Just verify it compiles with explicit init
    ConstInt ci = 0;
    CHECK(ci == 0, "const typedef (explicit init)");
}

// Test 11: Typedef with unsigned
typedef unsigned long ULong;

void test_unsigned_typedef(void) {
    ULong u;
    CHECK(u == 0, "unsigned typedef zero-init");
}

// Test 12: Nested struct typedef
typedef struct {
    struct {
        int a;
        int b;
    } inner;
    int c;
} Nested;

void test_nested_struct_typedef(void) {
    Nested n;
    CHECK(n.inner.a == 0 && n.inner.b == 0 && n.c == 0, "nested struct typedef zero-init");
}

// Test 13: Union typedef
typedef union {
    int i;
    float f;
} IntOrFloat;

void test_union_typedef(void) {
    IntOrFloat u;
    CHECK(u.i == 0, "union typedef zero-init");
}

// Test 14: Block-scoped typedef (should only be valid in scope)
void test_block_scoped_typedef(void) {
    {
        typedef int LocalInt;
        LocalInt x;
        CHECK(x == 0, "block-scoped typedef zero-init");
    }
    // LocalInt should not be visible here
    int y;  // fallback to regular int
    CHECK(y == 0, "after block-scoped typedef");
}

// Test 15: Shadowing typedef
typedef int ShadowType;

void test_shadowing_typedef(void) {
    ShadowType outer;
    CHECK(outer == 0, "outer typedef zero-init");
    {
        typedef float ShadowType;  // Shadow the outer typedef
        ShadowType inner;
        CHECK(inner == 0.0f, "shadowed typedef zero-init");
    }
    // After block, ShadowType should be int again
    ShadowType after;
    CHECK(after == 0, "typedef after shadow scope");
}

// Test 16: Typedef with __attribute__ (if supported)
#ifdef __GNUC__
typedef int AlignedInt __attribute__((aligned(16)));

void test_attributed_typedef(void) {
    AlignedInt a;
    CHECK(a == 0, "attributed typedef zero-init");
}
#else
void test_attributed_typedef(void) {
    printf("[SKIP] attributed typedef (not GCC)\n");
    total++;
    passed++;
}
#endif

// Test 17: Complex function pointer typedef
typedef void (*Handler)(int code, const char *msg);

void test_complex_func_ptr_typedef(void) {
    Handler h;
    CHECK(h == NULL, "complex func ptr typedef zero-init");
}

// Test 18: Array of function pointers typedef
typedef int (*OpArray[4])(int, int);

void test_func_ptr_array_typedef(void) {
    OpArray ops;
    int all_null = 1;
    for (int i = 0; i < 4; i++) {
        if (ops[i] != NULL) all_null = 0;
    }
    CHECK(all_null, "func ptr array typedef zero-init");
}

// Test 19: Typedef with struct containing pointer
typedef struct {
    char *data;
    int len;
} Buffer;

void test_struct_with_ptr_typedef(void) {
    Buffer buf;
    CHECK(buf.data == NULL && buf.len == 0, "struct with pointer typedef zero-init");
}

// Test 20: Multiple variables of typedef type
void test_multiple_vars_typedef(void) {
    MyInt a, b, c;  // Note: we warn about multi-declarators, but they compile
    // Multi-declarators don't get auto zero-init, need explicit
    a = 0; b = 0; c = 0;
    CHECK(a == 0 && b == 0 && c == 0, "multiple vars of typedef (explicit init)");
}

// Test 21: Typedef in function parameter context (shouldn't affect anything)
typedef struct {
    int value;
} Wrapper;

Wrapper make_wrapper(void) {
    Wrapper w;
    return w;
}

void test_typedef_return(void) {
    Wrapper w = make_wrapper();
    CHECK(w.value == 0, "typedef return zero-init");
}

// Test 22: Long typedef chain
typedef int T1;
typedef T1 T2;
typedef T2 T3;
typedef T3 T4;

void test_long_typedef_chain(void) {
    T4 x;
    CHECK(x == 0, "long typedef chain zero-init");
}

// Test 23: Typedef enum
typedef enum { RED, GREEN, BLUE } Color;

void test_enum_typedef(void) {
    Color c;
    CHECK(c == RED, "enum typedef zero-init");
}

// Test 24: Typedef with volatile
typedef volatile int VolatileInt;

void test_volatile_typedef(void) {
    VolatileInt v;
    CHECK(v == 0, "volatile typedef zero-init");
}

// Test 25: Struct typedef with array member
typedef struct {
    int values[10];
    int count;
} IntList;

void test_struct_with_array_typedef(void) {
    IntList list;
    int all_zero = (list.count == 0);
    for (int i = 0; i < 10; i++) {
        if (list.values[i] != 0) all_zero = 0;
    }
    CHECK(all_zero, "struct with array typedef zero-init");
}

int main(void) {
    printf("=== Typedef Tracking Tests ===\n\n");

    test_simple_typedef();
    test_multiple_typedef_names();
    test_struct_typedef();
    test_named_struct_typedef();
    test_pointer_typedef();
    test_double_pointer_typedef();
    test_array_typedef();
    test_func_ptr_typedef();
    test_chained_typedef();
    test_const_typedef();
    test_unsigned_typedef();
    test_nested_struct_typedef();
    test_union_typedef();
    test_block_scoped_typedef();
    test_shadowing_typedef();
    test_attributed_typedef();
    test_complex_func_ptr_typedef();
    test_func_ptr_array_typedef();
    test_struct_with_ptr_typedef();
    test_multiple_vars_typedef();
    test_typedef_return();
    test_long_typedef_chain();
    test_enum_typedef();
    test_volatile_typedef();
    test_struct_with_array_typedef();

    printf("\n=== Results: %d/%d tests passed ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}

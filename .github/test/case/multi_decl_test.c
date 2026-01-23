// Test multi-declarator zero-initialization
#include <stdio.h>
#include <string.h>

static int passed = 0;
static int total = 0;

#define CHECK(cond, name) do { \
    total++; \
    if (cond) { printf("[PASS] %s\n", name); passed++; } \
    else { printf("[FAIL] %s\n", name); } \
} while(0)

// Test 1: Basic int multi-declarator
void test_basic_int_multi(void) {
    int a, b, c;
    CHECK(a == 0 && b == 0 && c == 0, "int a, b, c");
}

// Test 2: Mixed pointers and values
void test_mixed_ptr_val(void) {
    int *p, x, *q;
    CHECK(p == NULL && x == 0 && q == NULL, "int *p, x, *q");
}

// Test 3: Arrays in multi-declarator
void test_arrays_multi(void) {
    int a[5], b, c[3];
    int all_zero = 1;
    for (int i = 0; i < 5; i++) if (a[i] != 0) all_zero = 0;
    for (int i = 0; i < 3; i++) if (c[i] != 0) all_zero = 0;
    CHECK(all_zero && b == 0, "int a[5], b, c[3]");
}

// Test 4: Char arrays (common pattern)
void test_char_arrays(void) {
    char buf1[64], buf2[128], buf3[256];
    CHECK(buf1[0] == 0 && buf2[0] == 0 && buf3[0] == 0, "char buf1[64], buf2[128], buf3[256]");
}

// Test 5: With explicit initializers mixed in
void test_partial_init(void) {
    int a, b = 42, c;
    CHECK(a == 0 && b == 42 && c == 0, "int a, b = 42, c");
}

// Test 6: Pointer to pointer
void test_ptr_ptr(void) {
    int **pp, *p, x;
    CHECK(pp == NULL && p == NULL && x == 0, "int **pp, *p, x");
}

// Test 7: Qualifiers in multi
void test_qualifiers(void) {
    const int a = 0;  // const needs explicit init
    volatile int b, c;
    CHECK(a == 0 && b == 0 && c == 0, "const int a, volatile int b, c");
}

// Test 8: Long types
void test_long_types(void) {
    long a, b;
    long long c, d;
    unsigned long e, f;
    CHECK(a == 0 && b == 0 && c == 0 && d == 0 && e == 0 && f == 0, 
          "long a,b; long long c,d; unsigned long e,f");
}

// Test 9: Float/double multi
void test_float_multi(void) {
    float a, b;
    double c, d;
    CHECK(a == 0.0f && b == 0.0f && c == 0.0 && d == 0.0, "float a,b; double c,d");
}

// Test 10: Struct type multi-declarator
void test_struct_multi(void) {
    struct Point { int x, y; };
    struct Point p1, p2;
    CHECK(p1.x == 0 && p1.y == 0 && p2.x == 0 && p2.y == 0, "struct Point p1, p2");
}

// Test 11: Typedef multi-declarator
typedef int MyInt;
void test_typedef_multi(void) {
    MyInt a, b, c;
    CHECK(a == 0 && b == 0 && c == 0, "MyInt a, b, c");
}

// Test 12: Function pointer multi-declarator
void test_func_ptr_multi(void) {
    int (*fp1)(int), (*fp2)(int);
    CHECK(fp1 == NULL && fp2 == NULL, "int (*fp1)(int), (*fp2)(int)");
}

// Test 13: Very long multi-declarator
void test_long_multi(void) {
    int a, b, c, d, e, f, g, h;
    CHECK(a == 0 && b == 0 && c == 0 && d == 0 && 
          e == 0 && f == 0 && g == 0 && h == 0, 
          "int a,b,c,d,e,f,g,h");
}

// Test 14: 2D arrays
void test_2d_array_multi(void) {
    int a[2][3], b[3][2];
    int all_zero = 1;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 3; j++)
            if (a[i][j] != 0) all_zero = 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 2; j++)
            if (b[i][j] != 0) all_zero = 0;
    CHECK(all_zero, "int a[2][3], b[3][2]");
}

// Test 15: Unsigned char buffers (common I/O pattern)
void test_unsigned_char_bufs(void) {
    unsigned char in[1024], out[1024];
    CHECK(in[0] == 0 && out[0] == 0, "unsigned char in[1024], out[1024]");
}

int main(void) {
    printf("=== Multi-Declarator Zero-Init Tests ===\n\n");
    
    test_basic_int_multi();
    test_mixed_ptr_val();
    test_arrays_multi();
    test_char_arrays();
    test_partial_init();
    test_ptr_ptr();
    test_qualifiers();
    test_long_types();
    test_float_multi();
    test_struct_multi();
    test_typedef_multi();
    test_func_ptr_multi();
    test_long_multi();
    test_2d_array_multi();
    test_unsigned_char_bufs();
    
    printf("\n=== Results: %d/%d tests passed ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}

// Tests for zero-initialization feature

#include <stdio.h>
#include <string.h>

static int passed = 0;
static int total = 0;

#define CHECK(cond, test_name)                \
    do                                        \
    {                                         \
        total++;                              \
        if (cond)                             \
        {                                     \
            printf("[PASS] %s\n", test_name); \
            passed++;                         \
        }                                     \
        else                                  \
        {                                     \
            printf("[FAIL] %s\n", test_name); \
        }                                     \
    } while (0)

#define CHECK_EQ(got, expected, test_name)               \
    do                                                   \
    {                                                    \
        total++;                                         \
        if ((got) == (expected))                         \
        {                                                \
            printf("[PASS] %s\n", test_name);            \
            passed++;                                    \
        }                                                \
        else                                             \
        {                                                \
            printf("[FAIL] %s\n", test_name);            \
            printf("  Expected: %d\n", (int)(expected)); \
            printf("  Got:      %d\n", (int)(got));      \
        }                                                \
    } while (0)

// Test 1: Basic int zero-init
void test_basic_int(void)
{
    int x;
    CHECK_EQ(x, 0, "basic int zero-init");
}

// Test 2: Basic char zero-init
void test_basic_char(void)
{
    char c;
    CHECK_EQ(c, 0, "basic char zero-init");
}

// Test 3: Basic short zero-init
void test_basic_short(void)
{
    short s;
    CHECK_EQ(s, 0, "basic short zero-init");
}

// Test 4: Basic long zero-init
void test_basic_long(void)
{
    long l;
    CHECK_EQ(l, 0, "basic long zero-init");
}

// Test 5: Basic float zero-init
void test_basic_float(void)
{
    float f;
    CHECK(f == 0.0f, "basic float zero-init");
}

// Test 6: Basic double zero-init
void test_basic_double(void)
{
    double d;
    CHECK(d == 0.0, "basic double zero-init");
}

// Test 7: Unsigned int zero-init
void test_unsigned_int(void)
{
    unsigned int u;
    CHECK_EQ(u, 0, "unsigned int zero-init");
}

// Test 8: Unsigned char zero-init
void test_unsigned_char(void)
{
    unsigned char uc;
    CHECK_EQ(uc, 0, "unsigned char zero-init");
}

// Test 9: Pointer zero-init (should be NULL)
void test_pointer(void)
{
    int *p;
    CHECK(p == NULL, "pointer zero-init");
}

// Test 10: Char pointer zero-init
void test_char_pointer(void)
{
    char *s;
    CHECK(s == NULL, "char pointer zero-init");
}

// Test 11: Array of ints zero-init
void test_int_array(void)
{
    int arr[5];
    int all_zero = 1;
    for (int i = 0; i < 5; i++)
    {
        if (arr[i] != 0)
            all_zero = 0;
    }
    CHECK(all_zero, "int array zero-init");
}

// Test 12: Array of chars zero-init
void test_char_array(void)
{
    char arr[10];
    int all_zero = 1;
    for (int i = 0; i < 10; i++)
    {
        if (arr[i] != 0)
            all_zero = 0;
    }
    CHECK(all_zero, "char array zero-init");
}

// Test 13: Struct zero-init
void test_struct(void)
{
    struct
    {
        int a;
        char b;
        float c;
    } s;
    CHECK(s.a == 0 && s.b == 0 && s.c == 0.0f, "struct zero-init");
}

// Test 14: Named struct zero-init
void test_named_struct(void)
{
    struct Point
    {
        int x;
        int y;
    };
    struct Point p;
    CHECK(p.x == 0 && p.y == 0, "named struct zero-init");
}

// Test 15: Nested struct zero-init
void test_nested_struct(void)
{
    struct Outer
    {
        struct
        {
            int a;
            int b;
        } inner;
        int c;
    };
    struct Outer o;
    CHECK(o.inner.a == 0 && o.inner.b == 0 && o.c == 0, "nested struct zero-init");
}

// Test 16: Struct with pointer member
void test_struct_with_pointer(void)
{
    struct
    {
        int *ptr;
        int val;
    } s;
    CHECK(s.ptr == NULL && s.val == 0, "struct with pointer zero-init");
}

// Test 17: Array of structs zero-init
void test_struct_array(void)
{
    struct
    {
        int x;
        int y;
    } arr[3];
    int all_zero = 1;
    for (int i = 0; i < 3; i++)
    {
        if (arr[i].x != 0 || arr[i].y != 0)
            all_zero = 0;
    }
    CHECK(all_zero, "struct array zero-init");
}

// Test 18: Union zero-init
void test_union(void)
{
    union
    {
        int i;
        float f;
    } u;
    CHECK(u.i == 0, "union zero-init");
}

// Test 19: Long long zero-init
void test_long_long(void)
{
    long long ll;
    CHECK(ll == 0LL, "long long zero-init");
}

// Test 20: Size_t zero-init
void test_size_t(void)
{
    size_t sz;
    CHECK_EQ(sz, 0, "size_t zero-init");
}

// Test 21: Multiple declarations - should NOT be zero-init (not handled)
// This test documents current behavior
void test_explicit_init_preserved(void)
{
    int x = 42;
    CHECK_EQ(x, 42, "explicit init preserved");
}

// Test 22: Const qualifier with init
void test_const_with_init(void)
{
    const int c = 100;
    CHECK_EQ(c, 100, "const with init preserved");
}

// Test 23: Static local (already zero by C standard)
void test_static_local(void)
{
    static int s;
    CHECK_EQ(s, 0, "static local zero");
}

// Test 24: Zero-init in nested scope
void test_nested_scope(void)
{
    int outer;
    {
        int inner;
        CHECK_EQ(inner, 0, "nested scope inner zero-init");
    }
    CHECK_EQ(outer, 0, "nested scope outer zero-init");
}

// Test 25: Zero-init in if block
void test_if_block(void)
{
    if (1)
    {
        int x;
        CHECK_EQ(x, 0, "if block zero-init");
    }
}

// Test 26: Zero-init in else block
void test_else_block(void)
{
    if (0)
    {
        // not executed
    }
    else
    {
        int x;
        CHECK_EQ(x, 0, "else block zero-init");
    }
}

// Test 27: Zero-init in for loop
void test_for_loop(void)
{
    for (int i = 0; i < 1; i++)
    {
        int x;
        CHECK_EQ(x, 0, "for loop zero-init");
    }
}

// Test 28: Zero-init in while loop
void test_while_loop(void)
{
    int count = 0;
    while (count < 1)
    {
        int x;
        CHECK_EQ(x, 0, "while loop zero-init");
        count++;
    }
}

// Test 29: Zero-init in do-while loop
void test_do_while_loop(void)
{
    int count = 0;
    do
    {
        int x;
        CHECK_EQ(x, 0, "do-while loop zero-init");
        count++;
    } while (count < 1);
}

// Test 30: Zero-init in switch case
void test_switch_case(void)
{
    int val = 1;
    switch (val)
    {
    case 1:
    {
        int x;
        CHECK_EQ(x, 0, "switch case zero-init");
        break;
    }
    }
}

// Test 31: 2D array zero-init
void test_2d_array(void)
{
    int arr[3][3];
    int all_zero = 1;
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            if (arr[i][j] != 0)
                all_zero = 0;
        }
    }
    CHECK(all_zero, "2D array zero-init");
}

// Test 32: Void pointer zero-init
void test_void_pointer(void)
{
    void *vp;
    CHECK(vp == NULL, "void pointer zero-init");
}

// Test 33: Function pointer zero-init
void test_function_pointer(void)
{
    void (*fp)(void);
    CHECK(fp == NULL, "function pointer zero-init");
}

// Test 34: Double pointer zero-init
void test_double_pointer(void)
{
    int **pp;
    CHECK(pp == NULL, "double pointer zero-init");
}

// Test 35: Signed char zero-init
void test_signed_char(void)
{
    signed char sc;
    CHECK_EQ(sc, 0, "signed char zero-init");
}

// Test 36: Unsigned long zero-init
void test_unsigned_long(void)
{
    unsigned long ul;
    CHECK(ul == 0UL, "unsigned long zero-init");
}

// Test 37: Struct with array member
void test_struct_with_array(void)
{
    struct
    {
        int arr[4];
        int len;
    } s;
    int all_zero = (s.len == 0);
    for (int i = 0; i < 4; i++)
    {
        if (s.arr[i] != 0)
            all_zero = 0;
    }
    CHECK(all_zero, "struct with array member zero-init");
}

// Test 38: Typedef'd type zero-init
// This now works! The transpiler tracks typedef aliases.
void test_typedef_type(void)
{
    typedef int MyInt;
    MyInt mi; // Auto zero-init now works for typedef types!
    CHECK_EQ(mi, 0, "typedef type zero-init");
}

// Test 39: _Bool zero-init
void test_bool(void)
{
    _Bool b;
    CHECK_EQ(b, 0, "_Bool zero-init");
}

// Test 40: Register hint (should still zero-init)
void test_register(void)
{
    register int r;
    CHECK_EQ(r, 0, "register int zero-init");
}

// Test 41: Volatile zero-init
void test_volatile(void)
{
    volatile int v;
    CHECK_EQ(v, 0, "volatile int zero-init");
}

// Test 42: Const volatile zero-init
// Note: const without init needs special handling, testing with volatile
void test_const_volatile(void)
{
    const volatile int cv = 0; // explicit init needed for const
    CHECK_EQ(cv, 0, "const volatile zero-init");
}

// Test 43: Pointer to const zero-init
void test_pointer_to_const(void)
{
    const int *pc;
    CHECK(pc == NULL, "pointer to const zero-init");
}

// Test 44: Const pointer zero-init
void test_const_pointer(void)
{
    int *const cp = NULL; // const pointer needs explicit init
    CHECK(cp == NULL, "const pointer explicit null");
}

// Test 45: Array of pointers zero-init
void test_pointer_array(void)
{
    int *arr[5];
    int all_null = 1;
    for (int i = 0; i < 5; i++)
    {
        if (arr[i] != NULL)
            all_null = 0;
    }
    CHECK(all_null, "pointer array zero-init");
}

// Test 46: Enum type zero-init
void test_enum(void)
{
    enum Color
    {
        RED,
        GREEN,
        BLUE
    };
    enum Color c;
    CHECK_EQ(c, RED, "enum zero-init (should be 0/first value)");
}

// Test 47: Large struct zero-init
void test_large_struct(void)
{
    struct
    {
        int a[100];
        char b[100];
        double c[10];
    } large;
    int all_zero = 1;
    for (int i = 0; i < 100; i++)
    {
        if (large.a[i] != 0 || large.b[i] != 0)
            all_zero = 0;
    }
    for (int i = 0; i < 10; i++)
    {
        if (large.c[i] != 0.0)
            all_zero = 0;
    }
    CHECK(all_zero, "large struct zero-init");
}

// Test 48: Zero-init works with defer
void test_with_defer(void)
{
    int result;
    {
        int x;
        defer result = x;
    }
    CHECK_EQ(result, 0, "zero-init with defer");
}

// Test 49: Multiple vars in function
void test_multiple_vars(void)
{
    int a;
    char b;
    float c;
    int *d;
    CHECK(a == 0 && b == 0 && c == 0.0f && d == NULL, "multiple vars zero-init");
}

// Test 50: Sequential blocks
void test_sequential_blocks(void)
{
    {
        int x;
        CHECK_EQ(x, 0, "sequential block 1 zero-init");
    }
    {
        int y;
        CHECK_EQ(y, 0, "sequential block 2 zero-init");
    }
}

int main(void)
{
    printf("=== Zero-Init Tests ===\n\n");

    test_basic_int();
    test_basic_char();
    test_basic_short();
    test_basic_long();
    test_basic_float();
    test_basic_double();
    test_unsigned_int();
    test_unsigned_char();
    test_pointer();
    test_char_pointer();
    test_int_array();
    test_char_array();
    test_struct();
    test_named_struct();
    test_nested_struct();
    test_struct_with_pointer();
    test_struct_array();
    test_union();
    test_long_long();
    test_size_t();
    test_explicit_init_preserved();
    test_const_with_init();
    test_static_local();
    test_nested_scope();
    test_if_block();
    test_else_block();
    test_for_loop();
    test_while_loop();
    test_do_while_loop();
    test_switch_case();
    test_2d_array();
    test_void_pointer();
    test_function_pointer();
    test_double_pointer();
    test_signed_char();
    test_unsigned_long();
    test_struct_with_array();
    test_typedef_type();
    test_bool();
    test_register();
    test_volatile();
    test_const_volatile();
    test_pointer_to_const();
    test_const_pointer();
    test_pointer_array();
    test_enum();
    test_large_struct();
    test_with_defer();
    test_multiple_vars();
    test_sequential_blocks();

    printf("\n=== Results: %d/%d tests passed ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}
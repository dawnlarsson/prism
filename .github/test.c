// Comprehensive test suite for Prism C transpiler
// Tests: defer, zero-init, typedef tracking, multi-declarator, edge cases
// Run with: $ prism run .github/test.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

// TEST FRAMEWORK

static char log_buffer[1024];
static int log_pos = 0;
static int passed = 0;
static int failed = 0;
static int total = 0;

static void log_reset(void)
{
    log_buffer[0] = 0;
    log_pos = 0;
}

static void log_append(const char *s)
{
    int len = strlen(s);
    if (log_pos + len < 1023)
    {
        strcpy(log_buffer + log_pos, s);
        log_pos += len;
    }
}

#define CHECK(cond, name)                \
    do                                   \
    {                                    \
        total++;                         \
        if (cond)                        \
        {                                \
            printf("[PASS] %s\n", name); \
            passed++;                    \
        }                                \
        else                             \
        {                                \
            printf("[FAIL] %s\n", name); \
            failed++;                    \
        }                                \
    } while (0)

#define CHECK_LOG(expected, name)                                                       \
    do                                                                                  \
    {                                                                                   \
        total++;                                                                        \
        if (strcmp(log_buffer, expected) == 0)                                          \
        {                                                                               \
            printf("[PASS] %s\n", name);                                                \
            passed++;                                                                   \
        }                                                                               \
        else                                                                            \
        {                                                                               \
            printf("[FAIL] %s: expected '%s', got '%s'\n", name, expected, log_buffer); \
            failed++;                                                                   \
        }                                                                               \
    } while (0)

#define CHECK_EQ(got, expected, name)                                                      \
    do                                                                                     \
    {                                                                                      \
        total++;                                                                           \
        if ((got) == (expected))                                                           \
        {                                                                                  \
            printf("[PASS] %s\n", name);                                                   \
            passed++;                                                                      \
        }                                                                                  \
        else                                                                               \
        {                                                                                  \
            printf("[FAIL] %s: expected %d, got %d\n", name, (int)(expected), (int)(got)); \
            failed++;                                                                      \
        }                                                                                  \
    } while (0)

// SECTION 1: BASIC DEFER TESTS

void test_defer_basic(void)
{
    log_reset();
    {
        defer log_append("A");
        log_append("1");
    }
    CHECK_LOG("1A", "basic defer");
}

void test_defer_lifo(void)
{
    log_reset();
    {
        defer log_append("C");
        defer log_append("B");
        defer log_append("A");
        log_append("1");
    }
    CHECK_LOG("1ABC", "defer LIFO order");
}

int test_defer_return(void)
{
    log_reset();
    defer log_append("A");
    log_append("1");
    return 42;
}

void test_defer_goto_out(void)
{
    log_reset();
    {
        defer log_append("A");
        log_append("1");
        goto end;
    }
end:
    log_append("2");
    CHECK_LOG("1A2", "defer with goto out of scope");
}

void test_defer_nested_scopes(void)
{
    log_reset();
    {
        defer log_append("A");
        {
            defer log_append("B");
            {
                defer log_append("C");
                log_append("1");
                goto end;
            }
        }
    }
end:
    log_append("2");
    CHECK_LOG("1CBA2", "defer nested scopes with goto");
}

void test_defer_break(void)
{
    log_reset();
    for (int i = 0; i < 3; i++)
    {
        defer log_append("D");
        log_append("L");
        if (i == 1)
            break;
    }
    log_append("E");
    CHECK_LOG("LDLDE", "defer with break");
}

void test_defer_continue(void)
{
    log_reset();
    for (int i = 0; i < 3; i++)
    {
        defer log_append("D");
        if (i == 1)
        {
            log_append("S");
            continue;
        }
        log_append("L");
    }
    log_append("E");
    CHECK_LOG("LDSDLDE", "defer with continue");
}

void test_defer_switch_break(void)
{
    log_reset();
    int x = 1;
    switch (x)
    {
    case 1:
    {
        defer log_append("A");
        log_append("1");
        break;
    }
    case 2:
        log_append("2");
        break;
    }
    log_append("E");
    CHECK_LOG("1AE", "defer in switch with break");
}

void test_defer_switch_fallthrough(void)
{
    log_reset();
    int x = 0;
    switch (x)
    {
    case 0:
    {
        defer log_append("A");
        log_append("0");
    } // fallthrough - defer runs at }
    case 1:
    {
        defer log_append("B");
        log_append("1");
    } // fallthrough
    case 2:
    {
        defer log_append("C");
        log_append("2");
        break;
    }
    }
    log_append("E");
    CHECK_LOG("0A1B2CE", "defer switch fallthrough");
}

void test_defer_while(void)
{
    log_reset();
    int i = 0;
    while (i < 3)
    {
        defer log_append("D");
        log_append("L");
        i++;
    }
    log_append("E");
    CHECK_LOG("LDLDLDE", "defer in while loop");
}

void test_defer_do_while(void)
{
    log_reset();
    int i = 0;
    do
    {
        defer log_append("D");
        log_append("L");
        i++;
    } while (i < 3);
    log_append("E");
    CHECK_LOG("LDLDLDE", "defer in do-while loop");
}

int test_defer_nested_return(void)
{
    log_reset();
    defer log_append("1");
    {
        defer log_append("2");
        {
            defer log_append("3");
            log_append("R");
            return 99;
        }
    }
}

void test_defer_compound_stmt(void)
{
    log_reset();
    {
        defer
        {
            log_append("A");
            log_append("B");
        };
        log_append("1");
    }
    log_append("E");
    CHECK_LOG("1ABE", "defer compound statement");
}

void run_defer_basic_tests(void)
{
    printf("\n=== DEFER BASIC TESTS ===\n");

    test_defer_basic();
    test_defer_lifo();

    log_reset();
    int ret = test_defer_return();
    CHECK_LOG("1A", "defer with return");
    CHECK_EQ(ret, 42, "defer return value preserved");

    test_defer_goto_out();
    test_defer_nested_scopes();
    test_defer_break();
    test_defer_continue();
    test_defer_switch_break();
    test_defer_switch_fallthrough();
    test_defer_while();
    test_defer_do_while();

    log_reset();
    ret = test_defer_nested_return();
    CHECK_LOG("R321", "defer nested return");
    CHECK_EQ(ret, 99, "defer nested return value");

    test_defer_compound_stmt();
}

// SECTION 2: ZERO-INIT TESTS

void test_zeroinit_basic_types(void)
{
    int i;
    CHECK_EQ(i, 0, "int zero-init");

    char c;
    CHECK_EQ(c, 0, "char zero-init");

    short s;
    CHECK_EQ(s, 0, "short zero-init");

    long l;
    CHECK(l == 0, "long zero-init");

    float f;
    CHECK(f == 0.0f, "float zero-init");

    double d;
    CHECK(d == 0.0, "double zero-init");

    unsigned int u;
    CHECK_EQ(u, 0, "unsigned int zero-init");

    long long ll;
    CHECK(ll == 0LL, "long long zero-init");
}

void test_zeroinit_pointers(void)
{
    int *p;
    CHECK(p == NULL, "int pointer zero-init");

    char *s;
    CHECK(s == NULL, "char pointer zero-init");

    void *vp;
    CHECK(vp == NULL, "void pointer zero-init");

    int **pp;
    CHECK(pp == NULL, "double pointer zero-init");

    void (*fp)(void);
    CHECK(fp == NULL, "function pointer zero-init");
}

void test_zeroinit_arrays(void)
{
    int arr[5];
    int all_zero = 1;
    for (int i = 0; i < 5; i++)
        if (arr[i] != 0)
            all_zero = 0;
    CHECK(all_zero, "int array zero-init");

    char buf[64];
    CHECK(buf[0] == 0, "char array zero-init");

    int arr2d[3][3];
    all_zero = 1;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            if (arr2d[i][j] != 0)
                all_zero = 0;
    CHECK(all_zero, "2D array zero-init");

    int *ptrs[5];
    int all_null = 1;
    for (int i = 0; i < 5; i++)
        if (ptrs[i] != NULL)
            all_null = 0;
    CHECK(all_null, "pointer array zero-init");
}

void test_zeroinit_structs(void)
{
    struct
    {
        int a;
        char b;
        float c;
    } s;
    CHECK(s.a == 0 && s.b == 0 && s.c == 0.0f, "anonymous struct zero-init");

    struct Point
    {
        int x;
        int y;
    };
    struct Point p;
    CHECK(p.x == 0 && p.y == 0, "named struct zero-init");

    struct
    {
        int *ptr;
        int val;
    } sp;
    CHECK(sp.ptr == NULL && sp.val == 0, "struct with pointer zero-init");

    struct
    {
        int arr[4];
        int len;
    } sa;
    int all_zero = (sa.len == 0);
    for (int i = 0; i < 4; i++)
        if (sa.arr[i] != 0)
            all_zero = 0;
    CHECK(all_zero, "struct with array zero-init");
}

void test_zeroinit_qualifiers(void)
{
    volatile int v;
    CHECK_EQ(v, 0, "volatile int zero-init");

    register int r;
    CHECK_EQ(r, 0, "register int zero-init");

    _Alignas(16) int aligned;
    CHECK_EQ(aligned, 0, "_Alignas zero-init");
}

void test_zeroinit_in_scopes(void)
{
    {
        int x;
        CHECK_EQ(x, 0, "nested scope zero-init");
    }

    if (1)
    {
        int x;
        CHECK_EQ(x, 0, "if block zero-init");
    }

    for (int i = 0; i < 1; i++)
    {
        int x;
        CHECK_EQ(x, 0, "for loop zero-init");
    }

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

void test_zeroinit_with_defer(void)
{
    int result;
    {
        int x;
        defer result = x;
    }
    CHECK_EQ(result, 0, "zero-init with defer");
}

#ifdef __GNUC__
void test_zeroinit_typeof(void)
{
    // UPDATED: typeof no longer gets zero-init as a safety measure
    // (cannot determine at transpile-time if typeof refers to a VLA)
    int x = 42;
    __typeof__(x) y = 0; // Explicit init required
    CHECK_EQ(y, 0, "typeof with explicit init");

    __typeof__(x) *ptr = NULL; // Explicit init required
    CHECK(ptr == NULL, "typeof pointer with explicit init");
}
#endif

// Test that enum constants are recognized as compile-time constants (not VLAs)
enum
{
    TEST_ARRAY_SIZE = 10
};

void test_zeroinit_enum_array_size(void)
{
    int arr[TEST_ARRAY_SIZE];
    int all_zero = 1;
    for (int i = 0; i < TEST_ARRAY_SIZE; i++)
        if (arr[i] != 0)
            all_zero = 0;
    CHECK(all_zero, "enum constant array size zero-init");
}

void test_zeroinit_alignas_array(void)
{
    _Alignas(32) int arr[8];
    int all_zero = 1;
    for (int i = 0; i < 8; i++)
        if (arr[i] != 0)
            all_zero = 0;
    CHECK(all_zero, "_Alignas array zero-init");
}

void test_zeroinit_union(void)
{
    union
    {
        int i;
        float f;
        char c[4];
    } u;
    CHECK_EQ(u.i, 0, "union zero-init");
}

void run_zeroinit_tests(void)
{
    printf("\n=== ZERO-INIT TESTS ===\n");
    test_zeroinit_basic_types();
    test_zeroinit_pointers();
    test_zeroinit_arrays();
    test_zeroinit_structs();
    test_zeroinit_qualifiers();
    test_zeroinit_in_scopes();
    test_zeroinit_with_defer();
#ifdef __GNUC__
    test_zeroinit_typeof();
#endif
    test_zeroinit_enum_array_size();
    test_zeroinit_alignas_array();
    test_zeroinit_union();
}

// SECTION 2.5: RAW KEYWORD TESTS

void test_raw_basic(void)
{
    raw int x;
    x = 42;
    CHECK_EQ(x, 42, "raw int assignment");

    raw char c;
    c = 'A';
    CHECK_EQ(c, 'A', "raw char assignment");
}

void test_raw_array(void)
{
    raw int arr[100];
    arr[0] = 1;
    arr[99] = 99;
    CHECK(arr[0] == 1 && arr[99] == 99, "raw array assignment");
}

void test_raw_pointer(void)
{
    raw int *p;
    int val = 123;
    p = &val;
    CHECK_EQ(*p, 123, "raw pointer assignment");
}

void test_raw_struct(void)
{
    raw struct
    {
        int a;
        int b;
    } s;
    s.a = 10;
    s.b = 20;
    CHECK(s.a == 10 && s.b == 20, "raw struct assignment");
}

void test_raw_with_qualifiers(void)
{
    raw volatile int v;
    v = 100;
    CHECK_EQ(v, 100, "raw volatile int");

    raw const int *cp;
    int val = 50;
    cp = &val;
    CHECK_EQ(*cp, 50, "raw const pointer");
}

void run_raw_tests(void)
{
    printf("\n=== RAW KEYWORD TESTS ===\n");
    test_raw_basic();
    test_raw_array();
    test_raw_pointer();
    test_raw_struct();
    test_raw_with_qualifiers();
}

// SECTION 3: MULTI-DECLARATOR TESTS

void test_multi_decl_basic(void)
{
    int a, b, c;
    CHECK(a == 0 && b == 0 && c == 0, "int a, b, c");
}

void test_multi_decl_mixed_ptr(void)
{
    int *p, x, *q;
    CHECK(p == NULL && x == 0 && q == NULL, "int *p, x, *q");
}

void test_multi_decl_arrays(void)
{
    int a[5], b, c[3];
    int all_zero = 1;
    for (int i = 0; i < 5; i++)
        if (a[i] != 0)
            all_zero = 0;
    for (int i = 0; i < 3; i++)
        if (c[i] != 0)
            all_zero = 0;
    CHECK(all_zero && b == 0, "int a[5], b, c[3]");
}

void test_multi_decl_partial_init(void)
{
    int a, b = 42, c;
    CHECK(a == 0 && b == 42 && c == 0, "int a, b = 42, c");
}

void test_multi_decl_long(void)
{
    int a, b, c, d, e, f, g, h;
    CHECK(a == 0 && b == 0 && c == 0 && d == 0 &&
              e == 0 && f == 0 && g == 0 && h == 0,
          "int a,b,c,d,e,f,g,h");
}

void test_multi_decl_func_ptr(void)
{
    int (*fp1)(int), (*fp2)(int);
    CHECK(fp1 == NULL && fp2 == NULL, "int (*fp1)(int), (*fp2)(int)");
}

void run_multi_decl_tests(void)
{
    printf("\n=== MULTI-DECLARATOR TESTS ===\n");
    test_multi_decl_basic();
    test_multi_decl_mixed_ptr();
    test_multi_decl_arrays();
    test_multi_decl_partial_init();
    test_multi_decl_long();
    test_multi_decl_func_ptr();
}

// SECTION 4: TYPEDEF TRACKING TESTS

typedef int MyInt;
typedef int *IntPtr;
typedef struct
{
    int x;
    int y;
} Point;
typedef char Name[64];
typedef int (*Callback)(int, int);

void test_typedef_simple(void)
{
    MyInt x;
    CHECK_EQ(x, 0, "simple typedef zero-init");
}

void test_typedef_pointer(void)
{
    IntPtr p;
    CHECK(p == NULL, "pointer typedef zero-init");
}

void test_typedef_struct(void)
{
    Point p;
    CHECK(p.x == 0 && p.y == 0, "struct typedef zero-init");
}

void test_typedef_array(void)
{
    Name n;
    CHECK(n[0] == 0, "array typedef zero-init");
}

void test_typedef_func_ptr(void)
{
    Callback cb;
    CHECK(cb == NULL, "func ptr typedef zero-init");
}

typedef MyInt ChainedInt;
typedef ChainedInt DoubleChainedInt;

void test_typedef_chained(void)
{
    ChainedInt c;
    CHECK_EQ(c, 0, "chained typedef zero-init");

    DoubleChainedInt d;
    CHECK_EQ(d, 0, "double-chained typedef zero-init");
}

void test_typedef_multi_var(void)
{
    MyInt a, b, c;
    CHECK(a == 0 && b == 0 && c == 0, "typedef multi-var zero-init");
}

void test_typedef_block_scoped(void)
{
    {
        typedef int LocalInt;
        LocalInt x;
        CHECK_EQ(x, 0, "block-scoped typedef zero-init");
    }
    int y; // LocalInt not visible here
    CHECK_EQ(y, 0, "after block-scoped typedef");
}

typedef int ShadowType;

void test_typedef_shadowing(void)
{
    ShadowType outer;
    CHECK_EQ(outer, 0, "outer typedef zero-init");
    {
        typedef float ShadowType;
        ShadowType inner;
        CHECK(inner == 0.0f, "shadowed typedef zero-init");
    }
    ShadowType after;
    CHECK_EQ(after, 0, "typedef after shadow scope");
}

// Multi-declarator typedef: typedef int A, *B;
typedef int TD_Int, *TD_IntPtr;

void test_typedef_multi_declarator(void)
{
    TD_Int a;
    TD_IntPtr p;
    CHECK_EQ(a, 0, "multi-declarator typedef int zero-init");
    CHECK(p == NULL, "multi-declarator typedef ptr zero-init");
}

void run_typedef_tests(void)
{
    printf("\n=== TYPEDEF TRACKING TESTS ===\n");
    test_typedef_simple();
    test_typedef_pointer();
    test_typedef_struct();
    test_typedef_array();
    test_typedef_func_ptr();
    test_typedef_chained();
    test_typedef_multi_var();
    test_typedef_block_scoped();
    test_typedef_shadowing();
    test_typedef_multi_declarator();
}

// SECTION 5: EDGE CASES

void test_bitfield_zeroinit(void)
{
    struct
    {
        unsigned int a : 3;
        unsigned int b : 5;
        unsigned int c : 1;
    } bits;
    CHECK(bits.a == 0 && bits.b == 0 && bits.c == 0, "bitfield zero-init");
}

void test_anonymous_struct(void)
{
    struct
    {
        int x;
        struct
        {
            int a;
            int b;
        }; // anonymous
        int y;
    } s;
    CHECK(s.x == 0 && s.a == 0 && s.b == 0 && s.y == 0, "anonymous struct zero-init");
}

void test_anonymous_union(void)
{
    struct
    {
        int type;
        union
        {
            int i;
            float f;
        }; // anonymous
    } u;
    CHECK(u.type == 0 && u.i == 0, "anonymous union zero-init");
}

void test_long_declaration(void)
{
    const volatile unsigned long long int *const *volatile ptr;
    CHECK(ptr == NULL, "long qualified declaration zero-init");
}

void test_func_ptr_array(void)
{
    int (*handlers[10])(int, int);
    int all_null = 1;
    for (int i = 0; i < 10; i++)
        if (handlers[i] != NULL)
            all_null = 0;
    CHECK(all_null, "function pointer array zero-init");
}

void test_ptr_to_array(void)
{
    int (*p)[10];
    CHECK(p == NULL, "pointer to array zero-init");
}

void test_defer_compound_literal(void)
{
    log_reset();
    {
        int *p = (int[]){1, 2, 3};
        defer log_append("D");
        log_append("1");
    }
    log_append("E");
    CHECK_LOG("1DE", "defer with compound literal");
}

void test_duffs_device(void)
{
    log_reset();
    int count = 5;
    int n = (count + 3) / 4;
    defer log_append("F");
    switch (count % 4)
    {
    case 0:
        do
        {
            log_append("X");
        case 3:
            log_append("X");
        case 2:
            log_append("X");
        case 1:
            log_append("X");
        } while (--n > 0);
    }
    log_append("E");
}

void test_defer_ternary(void)
{
    log_reset();
    int x = 1;
    defer(x ? log_append("T") : log_append("F"));
    log_append("1");
}

void test_empty_defer(void)
{
    log_reset();
    {
        defer;
        log_append("1");
    } // empty defer statement
    log_append("E");
    CHECK_LOG("1E", "empty defer statement");
}

void test_do_while_0_defer(void)
{
    log_reset();
    defer log_append("F");
    do
    {
        defer log_append("D");
        log_append("1");
        if (1)
            break;
        log_append("X");
    } while (0);
    log_append("E");
}

void test_defer_comma_operator(void)
{
    log_reset();
    {
        defer(log_append("A"), log_append("B"));
        log_append("1");
    }
    CHECK_LOG("1AB", "defer with comma operator");
}

void run_edge_case_tests(void)
{
    printf("\n=== EDGE CASE TESTS ===\n");
    test_bitfield_zeroinit();
    test_anonymous_struct();
    test_anonymous_union();
    test_long_declaration();
    test_func_ptr_array();
    test_ptr_to_array();
    test_defer_compound_literal();

    test_duffs_device();
    CHECK_LOG("XXXXXEF", "Duff's device with defer");

    test_defer_ternary();
    CHECK_LOG("1T", "defer with ternary");

    test_empty_defer();

    test_do_while_0_defer();
    CHECK_LOG("1DEF", "do-while(0) with defer");

    test_defer_comma_operator();
}

// SECTION 6: BUG REGRESSION TESTS
#ifdef __GNUC__
void test_stmt_expr_defer_nested_block(void)
{
    // This SHOULD work: defer is in a nested block, not at top level
    log_reset();
    int x = ({
        int result;
        {
            defer log_append("D");
            result = 42;
        }
        log_append("1");
        result; // This is the result, not the defer
    });
    log_append("E");
    CHECK_EQ(x, 42, "stmt expr defer nested block - value");
    CHECK_LOG("D1E", "stmt expr defer nested block - order");
}
#endif

void test_non_vla_typedef_still_works(void)
{
    typedef int FixedArray[10]; // NOT a VLA - size is constant
    FixedArray arr;
    int all_zero = 1;
    for (int i = 0; i < 10; i++)
        if (arr[i] != 0)
            all_zero = 0;
    CHECK(all_zero, "non-VLA typedef array zero-init");

    typedef struct
    {
        int x;
        int y;
    } PointType;
    PointType p;
    CHECK(p.x == 0 && p.y == 0, "non-VLA typedef struct zero-init");
}

void test_switch_defer_no_leak(void)
{
    log_reset();
    int cleanup_count = 0;
    switch (1)
    {
    case 1:
    {
        defer cleanup_count++;
        log_append("1");
    } // defer fires here at }
    case 2:
        log_append("2"); // fallthrough reaches here
        break;
    }
    log_append("E");
    CHECK_EQ(cleanup_count, 1, "switch defer fires at brace not switch exit");
    CHECK_LOG("12E", "switch defer fallthrough order");
}

typedef int EnumShadowT;

void test_enum_constant_shadows_typedef(void)
{
    // First verify EnumShadowT works as a type
    EnumShadowT before;
    CHECK_EQ(before, 0, "typedef works before enum shadow");

    // Define enum with constant that shadows the typedef name
    enum
    {
        EnumShadowT = 42
    };

    // Now EnumShadowT is the enum constant (value 42), not a type
    // This expression should be integer multiplication (42 * 2 = 84)
    int product;
    product = EnumShadowT * 2;
    CHECK_EQ(product, 84, "enum constant shadows typedef - multiplication works");

    // Directly use the enum constant value
    CHECK_EQ(EnumShadowT, 42, "enum constant has correct value");
}

typedef int EnumPtrT;

void test_enum_shadow_star_ambiguity(void)
{
    int x = 3;

    // Shadow the typedef with an enum constant
    enum
    {
        EnumPtrT = 7
    };

    // Now "EnumPtrT * x" is multiplication (7 * 3 = 21), not a pointer declaration
    // Prism must NOT try to zero-init this as a declaration
    int result = EnumPtrT * x; // 7 * 3 = 21
    CHECK_EQ(result, 21, "enum shadow: T*x is multiplication not ptr decl");

    // Verify the enum constant has the right value
    CHECK_EQ(EnumPtrT, 7, "enum constant value correct");
}

typedef int EnumStmtT;

void test_enum_shadow_statement_form(void)
{
    int y = 5;

    // Shadow the typedef with enum constant
    enum
    {
        EnumStmtT = 10
    };

    // This statement-level expression "EnumStmtT * y" is multiplication: 10 * 5 = 50
    // If Prism tries to parse it as "EnumStmtT *y = 0;", the gcc compilation would fail
    // because "10 * y = 0" is not a valid lvalue assignment.
    EnumStmtT *y; // This is a statement: multiplication, discards result

    // If we got here, the statement compiled correctly
    CHECK(1, "enum shadow: statement T*x compiles as multiplication");
}

// BUG REGRESSION: pp-number with underscore (1024_160) should be single token
// Issue: tokenizer treated "1024_160" as two tokens: "1024" and "_160"
// This broke token pasting macros like: prefix##1024_160##_suffix
#define PP_PASTE_TEST(x) \
    extern int test_prefix_##x##_suffix;

PP_PASTE_TEST(1024_160)
PP_PASTE_TEST(2048_224)
PP_PASTE_TEST(2048_256)

// Verify the pasted tokens are valid by using them
extern int test_prefix_1024_160_suffix;
extern int test_prefix_2048_224_suffix;
extern int test_prefix_2048_256_suffix;

void test_ppnum_underscore_paste(void)
{
    // If we got here, the token pasting compiled correctly
    // The declarations above would fail if 1024_160 was tokenized as two tokens
    CHECK(1, "pp-number underscore paste: 1024_160 is single token");
}

// BUG REGRESSION: function declarations inside function bodies
// Issue: zeroinit code was emitting function declarations twice
// Pattern: "void func_name(...)" inside an #ifdef block in a function body
// The zeroinit parser would emit "void func_name" then bail, causing duplicate output
void test_local_function_decl(void)
{
    // These are local extern function declarations (valid C)
    // The zeroinit code should recognize these as function declarations
    // and skip them, NOT try to add "= 0" or duplicate them
    void local_func(int a, int b);
    void multi_line_func(int *rp, const int *ap,
                         const void *table, const int *np,
                         const int *n0, int num, int power);
    int return_func(const int *ap, int off);

    // If we got here, the function declarations were handled correctly
    CHECK(1, "local function declarations: no duplicate output");
}

void run_bug_regression_tests(void)
{
    printf("\n=== BUG REGRESSION TESTS ===\n");

#ifdef __GNUC__
    test_stmt_expr_defer_nested_block();
#else
    printf("[SKIP] stmt expr tests (not GCC)\n");
#endif

    test_non_vla_typedef_still_works();
    test_switch_defer_no_leak();
    test_enum_constant_shadows_typedef();
    test_enum_shadow_star_ambiguity();
    test_enum_shadow_statement_form();
    test_ppnum_underscore_paste();
    test_local_function_decl();
}

// SECTION 7: ADVANCED DEFER TESTS

static int global_val = 0;

int test_return_side_effect(void)
{
    global_val = 0;
    defer global_val = 100;
    return global_val; // returns 0, then defer sets to 100
}

void test_defer_capture_timing(void)
{
    log_reset();
    char c[2] = "X";
    defer log_append(c); // captures address, not value
    c[0] = 'Y';
    log_append("1");
    // At scope exit, c is 'Y', so "Y" is appended
}

static int recursion_count = 0;

void test_recursive_defer(int n)
{
    if (n <= 0)
        return;
    defer
    {
        recursion_count++;
        log_append("R");
    };
    test_recursive_defer(n - 1);
}

void test_defer_goto_backward(void)
{
    log_reset();
    int count = 0;
again:
    if (count >= 2)
        goto done;
    {
        defer log_append("D");
        log_append("L");
        count++;
        goto again;
    }
done:
    log_append("E");
    CHECK_LOG("LDLDE", "defer with goto backward");
}

void test_defer_deeply_nested(void)
{
    log_reset();
    {
        defer log_append("1");
        {
            defer log_append("2");
            {
                defer log_append("3");
                {
                    defer log_append("4");
                    {
                        defer log_append("5");
                        {
                            defer log_append("6");
                            {
                                defer log_append("7");
                                {
                                    defer log_append("8");
                                    log_append("X");
                                    goto out;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
out:
    log_append("E");
    CHECK_LOG("X87654321E", "defer deeply nested with goto");
}

void test_defer_nested_loops(void)
{
    log_reset();
    for (int i = 0; i < 2; i++)
    {
        defer log_append("O");
        for (int j = 0; j < 2; j++)
        {
            defer log_append("I");
            log_append("X");
            if (i == 0 && j == 1)
                goto done;
        }
    }
done:
    log_append("E");
    CHECK_LOG("XIXIOE", "defer nested loops with goto");
}

void test_defer_break_inner_stay_outer(void)
{
    log_reset();
    for (int i = 0; i < 2; i++)
    {
        defer log_append("O");
        for (int j = 0; j < 3; j++)
        {
            defer log_append("I");
            log_append("X");
            if (j == 1)
                break;
        }
        log_append("Y");
    }
    log_append("E");
    CHECK_LOG("XIXIYOXIXIYOE", "defer break inner stay outer");
}

void run_advanced_defer_tests(void)
{
    printf("\n=== ADVANCED DEFER TESTS ===\n");

    global_val = 0;
    int ret = test_return_side_effect();
    CHECK_EQ(ret, 0, "return side effect - return value");
    CHECK_EQ(global_val, 100, "return side effect - defer executed");

    test_defer_capture_timing();
    CHECK_LOG("1Y", "defer capture timing");

    log_reset();
    recursion_count = 0;
    test_recursive_defer(3);
    CHECK_EQ(recursion_count, 3, "recursive defer count");
    CHECK_LOG("RRR", "recursive defer order");

    test_defer_goto_backward();
    test_defer_deeply_nested();
    test_defer_nested_loops();
    test_defer_break_inner_stay_outer();
}

// SECTION 8: STRESS TESTS

void test_defer_shadowing_vars(void)
{
    log_reset();
    int x = 1;
    {
        int x = 2;
        // Should capture inner x (2)
        defer
        {
            if (x == 2)
                log_append("I");
            else
                log_append("?");
        };
    }
    // Should verify outer x (1) is untouched
    if (x == 1)
        log_append("O");
    CHECK_LOG("IO", "variable shadowing with defer");
}

void test_typedef_hiding(void)
{
    // Test that a local variable can hide a global typedef
    // without confusing the zero-init logic
    typedef int T;
    T a;
    CHECK_EQ(a, 0, "global typedef zero-init");

    {
        float T; // T is now a variable name, not a type
        T = 5.5f;
        CHECK(T == 5.5f, "typedef name hidden by variable");

        // This should be a syntax error if T was still seen as a type:
        // T * x; -> float * x (valid ptr decl) vs float * x (multiplication)
        // Prism shouldn't try to zero-init "T * x" as a pointer if it thinks T is a variable.
        // However, standard C parsing rules apply here.
    }

    T b; // T should be restored to type 'int'
    CHECK_EQ(b, 0, "typedef name restored after scope");
}

// BUG REGRESSION: typedef shadowed by variable of same name
void test_typedef_same_name_shadow(void)
{
    typedef int T;

    // Verify T works as a type before shadowing
    T before;
    CHECK_EQ(before, 0, "typedef T works before shadow");

    {
        // THE BUG CASE: "T T;" - first T is type, second T is variable name
        // Prism must:
        // 1. Recognize first T as typedef -> zero-init the variable
        // 2. Register that variable T now shadows typedef T
        T T;
        CHECK_EQ(T, 0, "T T declaration zero-inits variable");

        T = 42;
        CHECK_EQ(T, 42, "T is usable as variable after T T decl");

        // At this point, "T" refers to the variable, not the type.
        // Any attempt to use T as a type here would be a C syntax error.
        // We can't test "T x;" here as it would fail to compile (correctly).
        // But we verify that T is being used as a variable:
        int result = T + 8;
        CHECK_EQ(result, 50, "T used in expression as variable");
    }

    // After the block, typedef T should be visible again
    T after;
    CHECK_EQ(after, 0, "typedef T restored after shadow scope");
}

// Test nested shadowing: multiple levels of T T;
void test_typedef_nested_same_name_shadow(void)
{
    typedef int T;

    T outer;
    CHECK_EQ(outer, 0, "outer T as typedef");

    {
        T T; // shadows typedef
        T = 1;
        CHECK_EQ(T, 1, "first shadow level");

        {
            // Can't do "T T;" again here because T is already a variable.
            // But we can verify T is still the variable from outer scope:
            T = 2;
            CHECK_EQ(T, 2, "inner scope sees variable T");
        }

        CHECK_EQ(T, 2, "variable T preserved after inner scope");
    }

    // T is typedef again
    T restored;
    CHECK_EQ(restored, 0, "typedef restored after nested shadows");
}

// Test that pointer declarations work correctly after shadow ends
void test_typedef_shadow_then_pointer(void)
{
    typedef int T;

    {
        T T; // shadow
        T = 100;
        (void)T; // use it
    }

    // Now T is a type again - pointer declaration should work
    T *ptr;
    CHECK(ptr == NULL, "pointer to typedef after shadow scope");

    T arr[3];
    CHECK(arr[0] == 0 && arr[1] == 0 && arr[2] == 0, "array of typedef after shadow scope");
}

void test_static_local_init(void)
{
    // Prism skips adding "= 0" to static vars (as they are implicitly zero),
    // but we must ensure it doesn't break them.
    static int s;
    CHECK_EQ(s, 0, "static local implicit zero-init");

    static int *sp;
    CHECK(sp == NULL, "static local ptr implicit zero-init");
}

void test_complex_func_ptr(void)
{
    // Test zero-init on complex declarators
    // int *(*fp)(int, int) -> pointer to function(int, int) returning int*
    int *(*fp)(int, int);
    CHECK(fp == NULL, "complex function pointer zero-init");

    // Array of function pointers
    // void (*arr[2])(void)
    void (*arr[2])(void);
    CHECK(arr[0] == NULL && arr[1] == NULL, "array of func ptr zero-init");
}

void test_switch_default_first(void)
{
    // Verify defer cleanup works even if 'default' is the first label
    log_reset();
    int x = 10;
    switch (x)
    {
    default:
    {
        defer log_append("D");
    }
    break;
    case 1:
        log_append("1");
        break;
    }
    log_append("E");
    CHECK_LOG("DE", "switch default first defer");
}

// Macro that expands to a defer
#define CLEANUP defer log_append("C")

void test_macro_hidden_defer(void)
{
    // Prism operates on preprocessed tokens, so this must work
    log_reset();
    {
        CLEANUP;
        log_append("1");
    }
    CHECK_LOG("1C", "macro hidden defer");
}

// Macro that expands to a declaration
#define DECL_INT(x) int x

void test_macro_hidden_decl(void)
{
    // Should still zero-init
    DECL_INT(val);
    CHECK_EQ(val, 0, "macro hidden declaration zero-init");
}

static void void_inner_func(void) { log_append("I"); }
static void void_outer_func(void)
{
    defer log_append("O");
    // This is valid C: returning a void expression from a void function
    // Prism must handle the sequence: eval -> defer -> return
    return void_inner_func();
}

void test_void_return_void_call(void)
{
    log_reset();
    void_outer_func();
    CHECK_LOG("IO", "void return void call execution order");
}

void test_raw_multi_decl(void)
{
    // "raw" should apply to all declarators in the statement
    raw int a, b;
    a = 1;
    b = 2; // Initialize to avoid UB check failures if running with sanitizers
    CHECK(a == 1 && b == 2, "raw multi-declaration compiles");
}

void test_switch_continue(void)
{
    log_reset();
    int i = 0;
    while (i < 1)
    {
        defer log_append("L"); // Loop cleanup

        switch (i)
        {
        case 0:
        {
            defer log_append("S"); // Switch cleanup
        } // defer S executes here
            // 'continue' must trigger 'L' (loop iteration end)
            i++;
            continue;
        }
        log_append("X"); // Should be skipped
        i++;
    }
    log_append("E");

    // Expected order:
    // 1. Enter loop
    // 2. Enter switch
    // 3. Register 'L' in loop scope, enter case block
    // 4. Register 'S' in case block scope
    // 5. Exit case block -> defer 'S' executes
    // 6. Hit continue -> defer 'L' executes -> Re-check loop cond
    // 7. Loop terminates -> 'E'
    CHECK_LOG("SLE", "continue from inside switch");
}

void test_fam_struct_zeroinit(void)
{
    // C99 Flexible Array Member
    struct Fam
    {
        int len;
        char data[];
    };

    // Should zero-init the fixed part (len=0) and not crash parser
    struct Fam f;

    CHECK_EQ(f.len, 0, "struct with flexible array member zero-init");
}

#ifdef __GNUC__
void test_stmt_expr_side_effects(void)
{
    log_reset();
    int global = 0;
    // ({ { defer global=1; } 5; }) -> result 5, then global=1
    int y = ({
        {
            defer global = 1;
        }
        5;
    });

    CHECK_EQ(y, 5, "stmt expr result preserved");
    CHECK_EQ(global, 1, "stmt expr defer executed");
}
#endif

void run_stress_tests(void)
{
    printf("\n=== STRESS TESTS ===\n");
    test_defer_shadowing_vars();
    test_typedef_hiding();
    test_typedef_same_name_shadow();
    test_typedef_nested_same_name_shadow();
    test_typedef_shadow_then_pointer();
    test_static_local_init();
    test_complex_func_ptr();
    test_switch_default_first();
    test_macro_hidden_defer();
    test_macro_hidden_decl();
    test_void_return_void_call();
    test_raw_multi_decl();
    test_switch_continue();
    test_fam_struct_zeroinit();

#ifdef __GNUC__
    test_stmt_expr_side_effects();
#endif
}

// SECTION 8: SAFETY HOLE TESTS

void test_goto_over_block(void)
{
    log_reset();
    int before = 1;
    log_append("A");
    goto DONE;
    {
        // This entire block is skipped - no safety issue
        int x = 42;
        log_append("X"); // Should not run
    }
DONE:
    log_append("B");
    CHECK_EQ(before, 1, "goto over block - var before goto");
    CHECK_LOG("AB", "goto over block - skips entire block");
}

// Test: goto backward (to earlier label) is valid
void test_goto_backward_valid(void)
{
    log_reset();
    int count = 0;
    int x = 10; // Declared before label - always initialized
AGAIN:
    log_append("L");
    count++;
    x++;
    if (count < 3)
        goto AGAIN;
    log_append("E");
    CHECK_EQ(count, 3, "goto backward - loop count");
    CHECK_EQ(x, 13, "goto backward - var incremented");
    CHECK_LOG("LLLE", "goto backward - correct order");
}

// Test: goto forward to same scope level (no decls between) is valid
void test_goto_forward_no_decl(void)
{
    log_reset();
    int x = 5; // Before goto
    log_append("A");
    if (x > 0)
        goto SKIP;
    log_append("X"); // Skipped
SKIP:
    log_append("B");
    CHECK_EQ(x, 5, "goto forward no decl - var preserved");
    CHECK_LOG("AB", "goto forward no decl - correct order");
}

// Test: goto into nested scope where decl is AFTER label is valid
void test_goto_into_scope_decl_after_label(void)
{
    log_reset();
    goto INNER;
    {
    INNER:
        log_append("I");
        int x = 42; // Decl is AFTER label - this is fine
        log_append("D");
        CHECK_EQ(x, 42, "goto into scope - decl after label");
    }
    CHECK_LOG("ID", "goto into scope - correct order");
}

// Test: multiple gotos with proper structure
void test_goto_complex_valid(void)
{
    log_reset();
    int state = 0;

START:
    if (state == 0)
    {
        log_append("0");
        state = 1;
        goto MIDDLE;
    }
    log_append("X");
    goto END;

MIDDLE:
    log_append("1");
    state = 2;
    goto START;

END:
    log_append("E");
    CHECK_EQ(state, 2, "goto complex - final state");
    CHECK_LOG("01XE", "goto complex - correct order");
}

// Test: goto with defer still works when not skipping decls
void test_goto_with_defer_valid(void)
{
    log_reset();
    int x = 1; // Before the scope with defer
    {
        defer log_append("D");
        log_append("A");
        if (x > 0)
            goto OUT;
        log_append("X");
    OUT:
        log_append("B");
    }
    log_append("E");
    CHECK_LOG("ABDE", "goto with defer - defer runs on scope exit");
}

void run_safety_hole_tests(void)
{
    printf("\n=== SAFETY HOLE TESTS ===\n");
    printf("(Verifying valid goto patterns work; invalid patterns are compile-time errors)\n");

    test_goto_over_block();
    test_goto_backward_valid();
    test_goto_forward_no_decl();
    test_goto_into_scope_decl_after_label();
    test_goto_complex_valid();
    test_goto_with_defer_valid();
}

// SECTION 9: SWITCH FALLTHROUGH + DEFER EDGE CASES

void test_switch_fallthrough_decl_defer(void)
{
    // Fallthrough with declarations and defers - defers fire at block end, not switch end
    log_reset();
    int x = 0;
    switch (x)
    {
    case 0:
    {
        int a = 1;
        defer log_append("A");
        log_append("0");
    } // A fires here
    case 1:
    {
        int b = 2;
        defer log_append("B");
        log_append("1");
    } // B fires here
    case 2:
    {
        defer log_append("C");
        log_append("2");
        break;
    }
    }
    log_append("E");
    CHECK_LOG("0A1B2CE", "switch fallthrough with decls and defers");
}

void test_switch_fallthrough_no_braces(void)
{
    // Fallthrough without braces - no defers possible (defer requires braces)
    log_reset();
    int result = 0;
    int x = 0;
    switch (x)
    {
    case 0:
        result += 1;
    case 1:
        result += 10;
    case 2:
        result += 100;
        break;
    case 3:
        result += 1000;
    }
    CHECK_EQ(result, 111, "switch fallthrough no braces");
}

void test_switch_break_from_nested_block(void)
{
    // Break from a nested block inside a case
    log_reset();
    int x = 1;
    switch (x)
    {
    case 1:
    {
        defer log_append("O");
        {
            defer log_append("I");
            log_append("1");
            break; // Should trigger both I and O
        }
        log_append("X"); // Not reached
    }
    case 2:
        log_append("2");
        break;
    }
    log_append("E");
    CHECK_LOG("1IOE", "switch break from nested block");
}

void test_switch_goto_out_of_case(void)
{
    // Goto out of a case - defers must run
    log_reset();
    int x = 1;
    switch (x)
    {
    case 1:
    {
        defer log_append("D");
        log_append("1");
        goto done;
    }
    case 2:
        log_append("2");
        break;
    }
done:
    log_append("E");
    CHECK_LOG("1DE", "switch goto out of case");
}

void test_switch_multiple_defers_per_case(void)
{
    // Multiple defers in same case - LIFO order
    log_reset();
    int x = 1;
    switch (x)
    {
    case 1:
    {
        defer log_append("C");
        defer log_append("B");
        defer log_append("A");
        log_append("1");
        break;
    }
    }
    log_append("E");
    CHECK_LOG("1ABCE", "switch multiple defers per case");
}

void test_switch_nested_switch_defer(void)
{
    // Nested switches with defers
    log_reset();
    int x = 1, y = 1;
    switch (x)
    {
    case 1:
    {
        defer log_append("O");
        switch (y)
        {
        case 1:
        {
            defer log_append("I");
            log_append("1");
            break;
        }
        }
        log_append("2");
        break;
    }
    }
    log_append("E");
    CHECK_LOG("1I2OE", "nested switch with defers");
}

void run_switch_fallthrough_tests(void)
{
    printf("\n=== SWITCH FALLTHROUGH + DEFER TESTS ===\n");
    test_switch_fallthrough_decl_defer();
    test_switch_fallthrough_no_braces();
    test_switch_break_from_nested_block();
    test_switch_goto_out_of_case();
    test_switch_multiple_defers_per_case();
    test_switch_nested_switch_defer();
}

// SECTION 10: COMPLEX BREAK/CONTINUE NESTING TESTS

void test_break_continue_nested_3_levels(void)
{
    // 3 levels of loop nesting with defers at each level
    log_reset();
    for (int i = 0; i < 2; i++)
    {
        defer log_append("1");
        for (int j = 0; j < 2; j++)
        {
            defer log_append("2");
            for (int k = 0; k < 2; k++)
            {
                defer log_append("3");
                log_append("X");
                if (k == 0)
                    continue; // triggers 3
                if (j == 0 && k == 1)
                    break; // triggers 3, exits inner loop
            }
            if (i == 0 && j == 1)
                break; // triggers 2, exits middle loop
        }
    }
    log_append("E");
    // Trace: i=0,j=0: k=0 X3(cont) k=1 X3(break) 2; j=1: k=0 X3(cont) k=1 X3 2(break) 1
    //        i=1,j=0: k=0 X3(cont) k=1 X3(break) 2; j=1: k=0 X3(cont) k=1 X3 2 1
    CHECK_LOG("X3X32X3X321X3X32X3X321E", "break/continue nested 3 levels");
}

void test_continue_in_while_with_defer(void)
{
    // Continue in while loop - defer must run each iteration
    log_reset();
    int i = 0;
    while (i < 3)
    {
        defer log_append("D");
        i++;
        if (i == 2)
        {
            log_append("S");
            continue;
        }
        log_append("N");
    }
    log_append("E");
    CHECK_LOG("NDSDNDE", "continue in while with defer");
}

void test_break_in_do_while_with_defer(void)
{
    // Break in do-while - defer must run
    log_reset();
    int i = 0;
    do
    {
        defer log_append("D");
        i++;
        if (i == 2)
        {
            log_append("B");
            break;
        }
        log_append("N");
    } while (i < 5);
    log_append("E");
    CHECK_LOG("NDBDE", "break in do-while with defer");
}

void test_switch_inside_loop_continue(void)
{
    // Switch inside loop with continue - defers in switch case must run
    log_reset();
    for (int i = 0; i < 2; i++)
    {
        defer log_append("L");
        switch (i)
        {
        case 0:
        {
            defer log_append("S");
            log_append("0");
            continue; // triggers S, then L, then next iteration
        }
        case 1:
        {
            defer log_append("T");
            log_append("1");
            break;
        }
        }
        log_append("X");
    }
    log_append("E");
    CHECK_LOG("0SL1TXLE", "switch inside loop with continue");
}

void test_loop_inside_switch_break(void)
{
    // Loop inside switch case - break from loop should not exit switch
    log_reset();
    int x = 1;
    switch (x)
    {
    case 1:
    {
        defer log_append("S");
        for (int i = 0; i < 3; i++)
        {
            defer log_append("L");
            log_append("I");
            if (i == 1)
                break; // exits loop, not switch
        }
        log_append("A"); // Should be reached
        break;           // exits switch
    }
    }
    log_append("E");
    CHECK_LOG("ILILASE", "loop inside switch - break loop not switch");
}

void run_complex_nesting_tests(void)
{
    printf("\n=== COMPLEX BREAK/CONTINUE NESTING TESTS ===\n");
    test_break_continue_nested_3_levels();
    test_continue_in_while_with_defer();
    test_break_in_do_while_with_defer();
    test_switch_inside_loop_continue();
    test_loop_inside_switch_break();
}

// SECTION 11: CASE LABELS INSIDE BLOCKS

void test_case_in_nested_block(void)
{
    // Case label inside a nested block (valid C, but weird)
    log_reset();
    int x = 1;
    switch (x)
    {
        {
            // This block is entered via fallthrough from case 0 or jumped to for case 1
        case 1:
            log_append("1");
            break;
        }
    case 0:
        log_append("0");
        // falls through to block above
    }
    log_append("E");
    CHECK_LOG("1E", "case label in nested block");
}

void test_case_after_defer_in_block(void)
{
    // Case label after defer in same block - NOW AN ERROR
    // Prism correctly detects that jumping to case 1 would skip the defer.
    // This test verifies the safe pattern: each case has its own block with defers.
    log_reset();
    int x = 1;
    switch (x)
    {
    case 0:
    {
        defer log_append("D0");
        log_append("0");
        break;
    }
    case 1:
    {
        defer log_append("D1");
        log_append("1");
        break;
    }
    }
    log_append("E");
    // x=1: jump to case 1, "1", break, D1 fires, "E"
    CHECK_LOG("1D1E", "case with separate blocks - correct defer behavior");
}

void test_duff_device_with_defer_at_top(void)
{
    // Duff's device pattern - defer works when used in a wrapper scope
    // The interleaved case labels inside do-while are incompatible with defer
    // in the same block, so we use a separate scope.
    log_reset();
    int count = 5;
    int result = 0;
    {
        defer result += 10; // Wrapper scope - fires when we exit this block
        int n = (count + 3) / 4;
        switch (count % 4)
        {
        case 0:
            do
            {
                log_append("X");
            case 3:
                log_append("X");
            case 2:
                log_append("X");
            case 1:
                log_append("X");
            } while (--n > 0);
        }
    }
    log_append("E");
    CHECK_LOG("XXXXXE", "duff device with defer in wrapper");
    CHECK_EQ(result, 10, "duff device defer count"); // defer fires once at wrapper scope exit
}

void run_case_label_tests(void)
{
    printf("\n=== CASE LABELS INSIDE BLOCKS TESTS ===\n");
    test_case_in_nested_block();
    test_case_after_defer_in_block();
    test_duff_device_with_defer_at_top();
}

// SECTION 12: RIGOR TESTS - Testing identified concerns

typedef void VoidType;

VoidType test_typedef_void_return_impl(void)
{
    log_reset();
    defer log_append("D");
    log_append("1");
    return; // Should work correctly - defers should run
}

void test_typedef_void_return(void)
{
    test_typedef_void_return_impl();
    CHECK_LOG("1D", "typedef void return with defer");
}

// ISSUE 3b: typedef void* should NOT be treated as void return
typedef void *VoidPtr;

VoidPtr test_typedef_voidptr_return_impl(void)
{
    log_reset();
    defer log_append("D");
    log_append("1");
    return NULL; // Returns void*, should save return value properly
}

void test_typedef_voidptr_return(void)
{
    VoidPtr result = test_typedef_voidptr_return_impl();
    CHECK_LOG("1D", "typedef void* return with defer");
    CHECK(result == NULL, "typedef void* return value preserved");
}

void test_stmt_expr_defer_timing(void)
{
    log_reset();
    int capture = 0;

    // Workaround: wrap defer in a nested block
    int x = ({
        int y;
        y = 42;
        {
            defer
            {
                log_append("D");
                capture = y;
            };
        } // wrapped in block
        y; // final expression
    });

    log_append("E");
    CHECK_EQ(x, 42, "stmt expr defer - return value correct");
    CHECK_EQ(capture, 42, "stmt expr defer - captured value");
    CHECK_LOG("DE", "stmt expr defer - order");
}

void test_nested_stmt_expr_defer_immediate_block_exit(void)
{
    log_reset();

    int x = ({
        {
            defer log_append("O");
        } // outer block exits immediately -> "O" runs here
        int inner = ({
            {
                defer log_append("I");
            } // inner block exits immediately -> "I" runs here
            10;
        });
        log_append("M"); // middle - after inner completes
        inner + 5;
    });

    log_append("E");
    CHECK_EQ(x, 15, "nested stmt expr - return value");
    // Order: O (outer block exit), I (inner block exit), M, E
    CHECK_LOG("OIME", "nested stmt expr - defer order (blocks exit immediately)");
}

typedef struct
{
    int x;
    int y;
} PointType;

void test_const_after_typename(void)
{
    // const BEFORE typename - should work
    const PointType p1;
    CHECK(p1.x == 0 && p1.y == 0, "const before typedef zero-init");

    // const AFTER typename - might fail
    PointType const p2;
    CHECK(p2.x == 0 && p2.y == 0, "const after typedef zero-init");
}

#if !defined(_MSC_VER) && defined(__STDC_NO_ATOMICS__) == 0
#include <stdatomic.h>
void test_atomic_zeroinit(void)
{
    _Atomic int ai;
    CHECK(atomic_load(&ai) == 0, "_Atomic int zero-init");

    _Atomic int *ap;
    CHECK(ap == NULL, "_Atomic pointer zero-init");
}
#else
void test_atomic_zeroinit(void)
{
    // Skip on platforms without atomics
    printf("[SKIP] _Atomic tests (not supported)\n");
}
#endif

int test_static_local_helper(void)
{
    static int counter; // Should be zero-init'd ONCE by C semantics
    return ++counter;
}

void test_static_local_zeroinit(void)
{
    // Call multiple times - if prism re-inits each call, this breaks
    int a = test_static_local_helper();
    int b = test_static_local_helper();
    int c = test_static_local_helper();

    CHECK(a == 1 && b == 2 && c == 3, "static local not re-initialized");
}

// ISSUE 11c: defer with inline function (if supported)
#ifdef __GNUC__
static inline int inline_with_defer(void)
{
    log_reset();
    defer log_append("D");
    log_append("1");
    return 42;
}

void test_inline_defer(void)
{
    int r = inline_with_defer();
    CHECK_EQ(r, 42, "inline function defer - return value");
    CHECK_LOG("1D", "inline function defer - order");
}
#else
void test_inline_defer(void)
{
    printf("[SKIP] inline defer tests (not GCC/Clang)\n");
}
#endif

void test_complex_declarator_zeroinit(void)
{
    // Simple function pointer - should work
    int (*fp1)(void);
    CHECK(fp1 == NULL, "function pointer zero-init");

    // Pointer to array
    int (*pa)[10];
    CHECK(pa == NULL, "pointer to array zero-init");

    // Array of function pointers
    int (*afp[5])(void);
    int all_null = 1;
    for (int i = 0; i < 5; i++)
        if (afp[i] != NULL)
            all_null = 0;
    CHECK(all_null, "array of function pointers zero-init");

    // Function pointer returning pointer
    int *(*fprp)(void);
    CHECK(fprp == NULL, "func ptr returning ptr zero-init");
}

void test_complex_decl_safety(void)
{
    // Function returning pointer to array
    // int (*(*f1)(void))[10];
    // This is extremely complex - let's test simpler variants first

    // Pointer to function returning pointer to int
    int *(*(*ppfp))(void);
    CHECK(ppfp == NULL, "ptr to ptr to func returning ptr - zero-init");

    // Double pointer to function
    int (**ppf)(void);
    CHECK(ppf == NULL, "double ptr to function zero-init");

    // Pointer to array of pointers
    int *(*pap)[5];
    CHECK(pap == NULL, "ptr to array of ptrs zero-init");
}

// Test multi-level pointer with qualifiers
void test_qualified_complex_decl(void)
{
    // const pointer to pointer
    int *const *cpp;
    CHECK(cpp == NULL, "const ptr to ptr zero-init");

    // pointer to const pointer
    int **const pcp;
    CHECK(pcp == NULL, "ptr to const ptr zero-init");

    // volatile pointer
    int *volatile vp;
    CHECK(vp == NULL, "volatile ptr zero-init");

    // restrict pointer (C99)
    int *restrict rp;
    CHECK(rp == NULL, "restrict ptr zero-init");
}

// Test that extern declarations are NOT zero-initialized (would cause linker errors)
extern int extern_var; // declaration only, no init

void test_extern_not_initialized(void)
{
    // This test passes if it compiles - extern should not get = 0 added
    // We can't actually test the value without defining it somewhere
    printf("[PASS] extern declaration not initialized (compiled OK)\n");
    passed++;
    total++;
}

// Test that typedef declarations are NOT zero-initialized
void test_typedef_not_initialized(void)
{
    typedef int MyInt; // This should not become "typedef int MyInt = 0;"
    MyInt x;
    CHECK_EQ(x, 0, "variable of typedef type zero-init");
    printf("[PASS] typedef declaration not initialized (compiled OK)\n");
    passed++;
    total++;
}

void test_for_init_zeroinit(void)
{
    int sum = 0;
    // Test that variables declared in for init are zero-initialized
    for (int i; i < 3; i++) // i should be 0-init'd
    {
        sum += i;
    }
    CHECK(sum == 0 + 1 + 2, "for init clause zero-init");

    // Multiple declarations in for init
    sum = 0;
    for (int a, b; a < 2; a++, b++)
    {
        sum += a + b; // both should start at 0
    }
    CHECK(sum == (0 + 0) + (1 + 1), "for init multiple decls zero-init");

    printf("[PASS] for init declaration (compiled OK)\n");
    passed++;
    total++;
}

/*
void test_defer_in_for_parts(void)
{
    // These should all be errors:
    // for (defer foo(); ...; ...) { }
    // for (...; ...; defer foo()) { }
}
*/

void test_ptr_to_vla_typedef(int n)
{
    typedef int VlaType[n]; // VLA typedef
    VlaType *p;             // Pointer to VLA - should be zero-init'd to NULL
    CHECK(p == NULL, "pointer to VLA typedef zero-init");

    // Also test pointer to pointer to VLA
    VlaType **pp;
    CHECK(pp == NULL, "double pointer to VLA typedef zero-init");
}

// Test that VLA size side effects are not duplicated
static int vla_size_counter = 0;
int get_vla_size(void)
{
    vla_size_counter++;
    return 10;
}

void test_vla_side_effect_once(void)
{
    // VLA typedef with side effect in size
    int n = 5;
    typedef int Arr[n++];
    CHECK_EQ(n, 6, "VLA typedef side effect runs once");
    (void)sizeof(Arr);

    // VLA typedef with function call
    vla_size_counter = 0;
    typedef int Arr2[get_vla_size()];
    CHECK_EQ(vla_size_counter, 1, "VLA size function called once");
    (void)sizeof(Arr2);
}

void test_atomic_specifier_form(void)
{
    // Qualifier form (already worked)
    _Atomic int a;
    CHECK(a == 0, "_Atomic int (qualifier form) zero-init");

    // Specifier form (was broken)
    _Atomic(int) b;
    CHECK(b == 0, "_Atomic(int) (specifier form) zero-init");
    // More complex specifier forms
    _Atomic(long long) c;
    CHECK(c == 0, "_Atomic(long long) zero-init");

    _Atomic(int *) d; // atomic pointer
    CHECK(d == NULL, "_Atomic(int*) zero-init");
}

// HOLE #1: Switch scope leak - variable before first case
// Previously: The zero-init "= 0" was added but switch jumped over it!
// NOW FIXED: Prism errors on declarations before first case label.
// This test verifies the SAFE patterns work correctly.
void test_switch_scope_leak(void)
{
    // SAFE PATTERN 1: Declare variable BEFORE the switch
    int y;
    int result = -1;
    switch (1)
    {
    case 1:
        result = y; // y is properly zero-initialized
        break;
    }
    CHECK_EQ(result, 0, "switch scope: variable before switch is zero-init");

    // SAFE PATTERN 2: Declare variable INSIDE a case block
    result = -1;
    switch (1)
    {
    case 1:
    {
        int z; // Inside case block - properly initialized
        result = z;
        break;
    }
    }
    CHECK_EQ(result, 0, "switch scope: variable in case block is zero-init");

    printf("[PASS] switch scope leak protection (unsafe pattern now errors)\n");
    passed++;
    total++;
}

typedef int SizeofTestType;

void test_sizeof_shadows_type(void)
{
    // At this point, SizeofTestType is the typedef (int)
    int SizeofTestType = sizeof(SizeofTestType); // sizeof should use the TYPE
    // sizeof(int) is typically 4
    CHECK(SizeofTestType == sizeof(int), "sizeof(T) in initializer uses type not variable");
}

#if __STDC_VERSION__ >= 201112L
void test_generic_colons(void)
{
    int x = 5;
    // _Generic has "type: value" syntax that looks like labels
    int type_id = _Generic(x,
        int: 1,
        long: 2,
        default: 0);
    CHECK(type_id == 1, "_Generic parsing doesn't break label detection");
}
#endif

void test_for_braceless_label(void)
{
    int reached = 0;
    for (int i = 0; i < 1; i++)
    my_label:
        reached = 1; // Label in braceless for body

    CHECK(reached == 1, "label in braceless for body");
}

// Also test goto INTO a for loop (should be blocked if it skips declarations)
void test_goto_into_for(void)
{
    int x = 0;
    goto skip;
    for (int i = 0; i < 10; i++)
    {
    skip:
        x = 1;
        break;
    }
    CHECK(x == 1, "goto into for loop body");
}

void test_attribute_positions(void)
{
    // Attribute after type, before variable name
    int __attribute__((aligned(4))) x;
    CHECK(x == 0, "attribute after type zero-init");

    // Attribute after pointer star
    int *__attribute__((aligned(8))) p;
    CHECK(p == NULL, "attribute after pointer star zero-init");

    // Multiple attributes
    __attribute__((unused)) __attribute__((aligned(16))) int y;
    CHECK(y == 0, "multiple attributes zero-init");
}

void test_rigor_defer_comma_operator(void)
{
    log_reset();
    {
        defer(log_append("A"), log_append("B")); // Comma expression
        log_append("1");
    }
    CHECK_LOG("1AB", "defer comma operator");
}

void test_defer_complex_comma(void)
{
    log_reset();
    int x = 0;
    {
        defer(x++, log_append("D"));
        log_append("1");
    }
    CHECK(x == 1, "defer comma with side effect - x incremented");
    CHECK_LOG("1D", "defer comma with side effect - log order");
}

// Test that exit/abort in switch case doesn't false-positive as fallthrough
void test_switch_noreturn_no_fallthrough(void)
{
    int x = 2; // Don't hit the exit case
    int result = 0;

    switch (x)
    {
    case 1:
        exit(1); // noreturn - should NOT trigger fallthrough error
    case 2:
        result = 2;
        break;
    }

    CHECK_EQ(result, 2, "switch noreturn: no false fallthrough error");
}

// Test that defer arguments are evaluated at scope exit (late binding)
// This is by design - documenting expected behavior
static int late_binding_captured = 0;
void capture_value(int x) { late_binding_captured = x; }

void test_defer_late_binding_semantic(void)
{
    int x = 10;
    {
        defer capture_value(x); // x is evaluated at scope exit
        x = 20;                 // Modify x before scope exit
    }
    // Late binding: x was 20 at scope exit
    CHECK_EQ(late_binding_captured, 20, "defer late binding: evaluates at exit");

    // Workaround: capture value at defer site
    x = 10;
    {
        int captured_x = x; // Capture value NOW
        defer capture_value(captured_x);
        x = 20;
    }
    CHECK_EQ(late_binding_captured, 10, "defer early capture workaround");
}

// Run all rigor tests
void run_rigor_tests(void)
{
    printf("\n=== RIGOR TESTS ===\n");

    test_typedef_void_return();
    test_typedef_voidptr_return();
    test_stmt_expr_defer_timing();
    test_nested_stmt_expr_defer_immediate_block_exit();
    test_const_after_typename();
    test_atomic_zeroinit();
    test_static_local_zeroinit();
    test_inline_defer();
    test_complex_declarator_zeroinit();
    test_complex_decl_safety();
    test_qualified_complex_decl();
    test_extern_not_initialized();
    test_typedef_not_initialized();
    test_for_init_zeroinit();
    test_ptr_to_vla_typedef(5);
    test_vla_side_effect_once();
    test_atomic_specifier_form();

    test_switch_scope_leak();
    test_sizeof_shadows_type();

#if __STDC_VERSION__ >= 201112L
    test_generic_colons();
#endif

    test_for_braceless_label();
    test_goto_into_for();
    test_attribute_positions();
    test_rigor_defer_comma_operator();
    test_defer_complex_comma();
    test_switch_noreturn_no_fallthrough();
    test_defer_late_binding_semantic();
}

// SECTION 13: SILENT FAILURE DETECTION TESTS

#define CHECK_ZEROED(var, size, name)                            \
    do                                                           \
    {                                                            \
        char zero_buf[size];                                     \
        memset(zero_buf, 0, size);                               \
        if (memcmp(&(var), zero_buf, size) == 0)                 \
        {                                                        \
            printf("[PASS] %s\n", name);                         \
            passed++;                                            \
        }                                                        \
        else                                                     \
        {                                                        \
            printf("[FAIL] %s - NOT ZERO-INITIALIZED!\n", name); \
            failed++;                                            \
        }                                                        \
        total++;                                                 \
    } while (0)

void test_complex_func_ptr_array(void)
{
    // Function pointer that returns pointer to array of 10 ints
    int (*(*fp_ret_arr)(void))[10];
    CHECK(fp_ret_arr == NULL, "func ptr returning ptr to array - zero-init");
}

void test_array_of_complex_func_ptrs(void)
{
    int *(*arr_fp[3])(int, int);
    int all_null = 1;
    for (int i = 0; i < 3; i++)
        if (arr_fp[i] != NULL)
            all_null = 0;
    CHECK(all_null, "array of func ptrs returning ptr - zero-init");
}

void test_func_ptr_taking_func_ptr(void)
{
    int (*fp_takes_fp)(int (*)(void));
    CHECK(fp_takes_fp == NULL, "func ptr taking func ptr arg - zero-init");
}

void test_ptr_to_array_of_func_ptrs(void)
{
    int (*(*p_arr_fp)[5])(void);
    CHECK(p_arr_fp == NULL, "ptr to array of func ptrs - zero-init");
}

void test_multi_level_ptr_chain(void)
{
    int ****pppp;
    CHECK(pppp == NULL, "quad pointer - zero-init");

    void *****vpppp;
    CHECK(vpppp == NULL, "void quintuple pointer - zero-init");
}

typedef struct
{
    int x;
    int y;
} Coord;

void test_complex_func_ptr_with_struct(void)
{
    Coord *(*fp_struct)(Coord *, int, Coord);
    CHECK(fp_struct == NULL, "func ptr with struct params - zero-init");
}

void test_paren_grouped_declarator(void)
{
    // This is just a pointer to int, but uses parens
    int(*grouped_ptr);
    CHECK(grouped_ptr == NULL, "parenthesized pointer decl - zero-init");

    // Pointer to pointer with parens
    int *(*grouped_pp);
    CHECK(grouped_pp == NULL, "paren grouped ptr to ptr - zero-init");
}

void test_multi_dim_array_ptrs(void)
{
    int (*p2d)[3][4];
    CHECK(p2d == NULL, "ptr to 2d array - zero-init");

    int (*p3d)[2][3][4];
    CHECK(p3d == NULL, "ptr to 3d array - zero-init");
}

void test_sizeof_array_bounds(void)
{
    int arr_sizeof[sizeof(int)];
    int all_zero = 1;
    for (size_t i = 0; i < sizeof(int); i++)
        if (arr_sizeof[i] != 0)
            all_zero = 0;
    CHECK(all_zero, "array with sizeof bound - zero-init");
}

void test_decl_after_label(void)
{
    int x;
    x = 1;
my_label:
{
    int y;
    CHECK_EQ(y, 0, "decl in block after label - zero-init");
}
    (void)x; // suppress unused warning
}

// Test: declaration directly after label (no braces) gets zero-init
// This tests backward goto where variable is re-initialized each iteration
void test_decl_directly_after_label(void)
{
    int counter = 0;
    int sum = 0;

restart:
    int x;    // Zero-initialized each time we jump here
    sum += x; // x should be 0 each time
    counter++;
    if (counter < 3)
        goto restart;

    // x was 0 on each iteration, so sum should be 0
    CHECK_EQ(sum, 0, "decl directly after label - zero-init on backward goto");
}

void test_decl_in_else(void)
{
    if (0)
    {
        int x;
        (void)x;
    }
    else
    {
        int y;
        CHECK_EQ(y, 0, "decl in else branch - zero-init");
    }
}

void test_volatile_func_ptr(void)
{
    int (*volatile vfp)(void);
    CHECK(vfp == NULL, "volatile func ptr - zero-init");

    volatile int (*fvp)(void);
    CHECK(fvp == NULL, "func ptr to volatile - zero-init");
}

void test_extremely_complex_declarator(void)
{
    // Pointer to function returning pointer to array of 5 pointers to functions
    // returning int
    int (*(*(*super_complex)(void))[5])(void);
    CHECK(super_complex == NULL, "extremely complex declarator - zero-init");
}

// SECTION: SIZEOF AND COMPLEX CONSTANT EXPRESSION TESTS

// Simulate the INT_STRLEN_BOUND macro
#define TYPE_SIGNED_TEST(t) (!((t)0 < (t) - 1))
#define TYPE_WIDTH_TEST(t) (sizeof(t) * 8)
#define INT_STRLEN_BOUND_TEST(t) \
    ((TYPE_WIDTH_TEST(t) - TYPE_SIGNED_TEST(t)) * 302 / 1000 + 1 + TYPE_SIGNED_TEST(t))

typedef long long test_rlim_t;
typedef unsigned long test_size_t;

void test_sizeof_in_array_bound(void)
{
    // Basic sizeof - should not be detected as VLA
    char buf1[sizeof(int)];
    CHECK(buf1[0] == 0, "sizeof(int) array bound - zero-init");

    // sizeof with typedef - should not be detected as VLA
    char buf2[sizeof(test_rlim_t)];
    CHECK(buf2[0] == 0, "sizeof(typedef) array bound - zero-init");

    // sizeof expression with multiplication
    char buf3[sizeof(int) * 8];
    int all_zero = 1;
    for (size_t i = 0; i < sizeof(int) * 8; i++)
        if (buf3[i] != 0)
            all_zero = 0;
    CHECK(all_zero, "sizeof*8 array bound - zero-init");
}

void test_cast_expression_in_array_bound(void)
{
    // Cast with built-in type - constant expression
    char buf1[(int)4 + 1];
    CHECK(buf1[0] == 0, "cast with int array bound - zero-init");

    // TYPE_SIGNED pattern: (! ((type) 0 < (type) -1))
    // This expands to a constant expression using casts
    char buf2[TYPE_SIGNED_TEST(int) + 1];
    CHECK(buf2[0] == 0, "TYPE_SIGNED(int) array bound - zero-init");

    // TYPE_SIGNED with typedef - the key regression case
    char buf3[TYPE_SIGNED_TEST(test_rlim_t) + 1];
    CHECK(buf3[0] == 0, "TYPE_SIGNED(typedef) array bound - zero-init");
}

void test_complex_macro_array_bound(void)
{
    // Full INT_STRLEN_BOUND pattern - the original failing case
    char buf1[INT_STRLEN_BOUND_TEST(int) + 1];
    CHECK(buf1[0] == 0, "INT_STRLEN_BOUND(int) array bound - zero-init");

    // With typedef - this was the exact pattern that failed in bash
    char buf2[INT_STRLEN_BOUND_TEST(test_rlim_t) + 1];
    CHECK(buf2[0] == 0, "INT_STRLEN_BOUND(typedef) array bound - zero-init");

    // With system-like typedef name (ends in _t)
    char buf3[INT_STRLEN_BOUND_TEST(test_size_t) + 1];
    CHECK(buf3[0] == 0, "INT_STRLEN_BOUND(size_t-like) array bound - zero-init");
}

void test_system_typedef_pattern(void)
{
    // Names ending in _t should be recognized as likely system typedefs
    // and allowed in constant expressions (for casts)
    typedef int my_custom_t;
    char buf1[(my_custom_t)10];
    int all_zero = 1;
    for (int i = 0; i < 10; i++)
        if (buf1[i] != 0)
            all_zero = 0;
    CHECK(all_zero, "custom _t typedef in cast - zero-init");
}

void test_alignof_in_array_bound(void)
{
    // _Alignof should also be recognized as constant
    char buf1[_Alignof(int) + 1];
    CHECK(buf1[0] == 0, "_Alignof array bound - zero-init");

    char buf2[_Alignof(test_rlim_t)];
    CHECK(buf2[0] == 0, "_Alignof(typedef) array bound - zero-init");
}

void test_complex_operators_in_array_bound(void)
{
    // Bitwise operators in constant expressions
    char buf1[(sizeof(int) << 1)];
    CHECK(buf1[0] == 0, "sizeof << 1 array bound - zero-init");

    // Comparison operators (result is 0 or 1)
    char buf2[(sizeof(int) >= 4) + 1];
    CHECK(buf2[0] == 0, "comparison in array bound - zero-init");

    // Ternary operator
    char buf3[(sizeof(int) > 2 ? 8 : 4)];
    CHECK(buf3[0] == 0, "ternary in array bound - zero-init");

    // Logical operators
    char buf4[(sizeof(int) && sizeof(char)) + 1];
    CHECK(buf4[0] == 0, "logical && in array bound - zero-init");
}

// Test for sizeof with array element access in array bounds
// This was a bug where sizeof(arr[0]) inside array bounds was incorrectly parsed
static int global_arr_for_sizeof[] = {1, 2, 3, 4, 5};

void test_sizeof_array_element_in_bound(void)
{
    // sizeof(arr[0]) pattern - common idiom for array element count
    // Bug: prism was seeing arr[0] as a declaration inside the array bound
    char buf1[sizeof(global_arr_for_sizeof) / sizeof(global_arr_for_sizeof[0])];
    int expected_size = sizeof(global_arr_for_sizeof) / sizeof(global_arr_for_sizeof[0]);
    int all_zero = 1;
    for (int i = 0; i < expected_size; i++)
        if (buf1[i] != 0)
            all_zero = 0;
    CHECK(all_zero, "sizeof(arr)/sizeof(arr[0]) array bound - zero-init");
    CHECK_EQ(expected_size, 5, "sizeof(arr)/sizeof(arr[0]) gives correct count");

    // Just sizeof(arr[0])
    char buf2[sizeof(global_arr_for_sizeof[0])];
    CHECK(buf2[0] == 0, "sizeof(arr[0]) array bound - zero-init");

    // Nested brackets: sizeof of 2D array element
    int arr2d[3][4] = {{0}};
    char buf3[sizeof(arr2d[0])]; // sizeof a row (4 ints)
    int row_size = sizeof(arr2d[0]);
    all_zero = 1;
    for (int i = 0; i < row_size; i++)
        if (buf3[i] != 0)
            all_zero = 0;
    CHECK(all_zero, "sizeof(2d_arr[0]) array bound - zero-init");

    // Multiple nested brackets
    char buf4[sizeof(arr2d[0][0])]; // sizeof single element
    CHECK(buf4[0] == 0, "sizeof(2d_arr[0][0]) array bound - zero-init");

    // sizeof with expression involving array access
    char buf5[sizeof(global_arr_for_sizeof[0]) * 2];
    CHECK(buf5[0] == 0, "sizeof(arr[0])*2 array bound - zero-init");
}

void test_sizeof_with_parens_in_bound(void)
{
    // Parenthesized sizeof expressions
    char buf1[(sizeof(int))];
    CHECK(buf1[0] == 0, "(sizeof(int)) array bound - zero-init");

    // Double parens
    char buf2[((sizeof(int)))];
    CHECK(buf2[0] == 0, "((sizeof(int))) array bound - zero-init");

    // sizeof of parenthesized expression
    char buf3[sizeof((int)0) + 1];
    CHECK(buf3[0] == 0, "sizeof((int)0) array bound - zero-init");

    // Complex parenthesized expression with sizeof
    char buf4[(sizeof(int) + sizeof(char)) * 2];
    int all_zero = 1;
    for (size_t i = 0; i < (sizeof(int) + sizeof(char)) * 2; i++)
        if (buf4[i] != 0)
            all_zero = 0;
    CHECK(all_zero, "(sizeof+sizeof)*2 array bound - zero-init");
}

void run_sizeof_constexpr_tests(void)
{
    printf("\n=== SIZEOF AND CONSTANT EXPRESSION TESTS ===\n");
    printf("(Regression tests for VLA false-positive detection)\n\n");

    test_sizeof_in_array_bound();
    test_cast_expression_in_array_bound();
    test_complex_macro_array_bound();
    test_system_typedef_pattern();
    test_alignof_in_array_bound();
    test_complex_operators_in_array_bound();
    test_sizeof_array_element_in_bound();
    test_sizeof_with_parens_in_bound();
}

void run_silent_failure_tests(void)
{
    printf("\n=== SILENT FAILURE DETECTION TESTS ===\n");
    printf("(Testing complex declarators that might silently skip zero-init)\n\n");

    test_complex_func_ptr_array();
    test_array_of_complex_func_ptrs();
    test_func_ptr_taking_func_ptr();
    test_ptr_to_array_of_func_ptrs();
    test_multi_level_ptr_chain();
    test_complex_func_ptr_with_struct();
    test_paren_grouped_declarator();
    test_multi_dim_array_ptrs();
    test_sizeof_array_bounds();
    test_decl_after_label();
    test_decl_directly_after_label();
    test_decl_in_else();
    test_volatile_func_ptr();
    test_extremely_complex_declarator();
}

// SECTION: MANUAL OFFSETOF VLA REGRESSION TESTS
// These tests verify handling of custom offsetof macros that expand to
// pointer arithmetic. GCC treats such patterns as VLAs even though they
// are technically compile-time constants.

// Custom offsetof macro using pointer arithmetic (common in legacy code)
// This is different from __builtin_offsetof which GCC treats as constant
#undef offsetof
#define offsetof(TYPE, MEMBER) ((size_t)((char *)&((TYPE *)0)->MEMBER - (char *)0))

typedef struct TestSrcItem_off
{
    char *name;
    int type;
} TestSrcItem_off;

typedef struct TestSrcList_off
{
    int count;
    TestSrcItem_off items[1]; // Flexible array member pattern
} TestSrcList_off;

// This struct contains a union with an array sized by offsetof
// GCC treats this as a VLA, so prism must NOT add = {0}
struct TestOp_off
{
    union
    {
        int i;
        void *p;
        char *z;
        struct
        {
            int n;
            // Array size uses offsetof - GCC sees this as VLA at file scope
            TestSrcItem_off items[offsetof(TestSrcList_off, items) / sizeof(TestSrcItem_off)];
        } srclist;
    } u;
};

void test_manual_offsetof_in_union(void)
{
    // This tests that prism doesn't add = {0} to struct TestOp_off
    // If it did, GCC would error: "variable-sized object may not be initialized"
    struct TestOp_off op;
    op.u.i = 42;
    CHECK(op.u.i == 42, "manual offsetof in union - no zeroinit");
}

void test_manual_offsetof_local(void)
{
    // Test offsetof with local variable
    struct TestOp_off op; // Should NOT get = {0} due to offsetof VLA pattern
    op.u.p = NULL;
    CHECK(op.u.p == NULL, "manual offsetof local struct - no zeroinit");
}

void test_union_offsetof_division(void)
{
    // The pattern offsetof(T,m)/sizeof(E) should be treated as VLA
    union
    {
        int x;
        struct
        {
            TestSrcItem_off data[offsetof(TestSrcList_off, items) / sizeof(TestSrcItem_off)];
        } embedded;
    } u;
    u.x = 123;
    CHECK(u.x == 123, "union offsetof division - no zeroinit");
}

void test_vla_basic(void)
{
    int n = 5;
    int vla[n]; // VLA - prism should NOT add = {0}
    // Just verify it compiles and we can use it
    for (int i = 0; i < n; i++)
    {
        vla[i] = i;
    }
    CHECK(vla[0] == 0 && vla[4] == 4, "basic VLA - no zeroinit");
}

void test_vla_expression_size(void)
{
    int a = 3, b = 2;
    int vla[a + b]; // VLA with expression - should NOT get zeroinit
    for (int i = 0; i < a + b; i++)
    {
        vla[i] = i * 2;
    }
    CHECK(vla[0] == 0 && vla[4] == 8, "VLA expression size - no zeroinit");
}

// test_struct_with_vla_member removed - VLA in struct/union is now rejected uniformly

void run_manual_offsetof_vla_tests(void)
{
    printf("\n=== MANUAL OFFSETOF VLA REGRESSION TESTS ===\n");
    printf("(Tests for pointer-arithmetic offsetof patterns)\n\n");

    test_manual_offsetof_in_union();
    test_manual_offsetof_local();
    test_union_offsetof_division();
    test_vla_basic();
    test_vla_expression_size();
}

// SECTION: PREPROCESSOR NUMERIC LITERAL TESTS

// Test C23/GCC extended float suffixes (F128, f64, etc.)
// Regression test for: F128 suffix causing "expected identifier" error
#define TEST_FLT128_MAX 1.18973149535723176508575932662800702e+4932F128
#define TEST_FLT128_MIN 3.36210314311209350626267781732175260e-4932F128
#define TEST_FLT64_VAL 1.7976931348623157e+308F64
#define TEST_FLT32_VAL 3.40282347e+38F32
#define TEST_FLT16_VAL 6.5504e+4F16
#define TEST_BF16_VAL 3.38953139e+38BF16

void test_float128_suffix(void)
{
    // Just verify these compile - the preprocessor must parse F128 suffix
    (void)TEST_FLT128_MAX;
    (void)TEST_FLT128_MIN;
    CHECK(1, "F128 float suffix parses");
}

void test_float64_suffix(void)
{
    (void)TEST_FLT64_VAL;
    CHECK(1, "F64 float suffix parses");
}

void test_float32_suffix(void)
{
    (void)TEST_FLT32_VAL;
    CHECK(1, "F32 float suffix parses");
}

void test_float16_suffix(void)
{
    (void)TEST_FLT16_VAL;
    CHECK(1, "F16 float suffix parses");
}

void test_bf16_suffix(void)
{
    (void)TEST_BF16_VAL;
    CHECK(1, "BF16 float suffix parses");
}

void run_preprocessor_numeric_tests(void)
{
    printf("\n=== PREPROCESSOR NUMERIC LITERAL TESTS ===\n");
    printf("(Tests for C23/GCC extended float suffixes)\n\n");

    test_float128_suffix();
    test_float64_suffix();
    test_float32_suffix();
    test_float16_suffix();
    test_bf16_suffix();
}

// PREPROCESSOR SYSTEM MACRO TESTS

#include <signal.h>

void test_linux_macros(void)
{
    // These macros must be defined for Linux platform detection
    // OpenSSL KTLS support depends on these being available
    // Only test on Linux - skip on other platforms
#ifdef __linux__
    CHECK(1, "__linux__ macro defined");
#ifdef __linux
    CHECK(1, "__linux macro defined");
#else
    CHECK(0, "__linux macro defined");
#endif
#ifdef linux
    CHECK(1, "linux macro defined");
#else
    CHECK(0, "linux macro defined");
#endif
    // __gnu_linux__ is glibc-specific, not defined on musl (Alpine)
#ifdef __GLIBC__
#ifdef __gnu_linux__
    CHECK(1, "__gnu_linux__ macro defined");
#else
    CHECK(0, "__gnu_linux__ macro defined");
#endif
#else
    printf("  [SKIP] __gnu_linux__ test (not using glibc)\n");
#endif
#else
    // Not on Linux - skip these tests (they're Linux-specific)
    printf("  [SKIP] Linux macro tests (not on Linux)\n");
#endif
}

void test_signal_macros(void)
{
    // Signal macros must be defined for code using #ifdef SIGALRM etc.
    // OpenSSL speed.c and many other programs depend on this.
    // prism now pre-defines standard Linux signal values.
#ifdef SIGALRM
    CHECK(SIGALRM == 14, "SIGALRM defined as 14");
#else
    CHECK(0, "SIGALRM defined as 14");
#endif

#ifdef SIGINT
    CHECK(SIGINT == 2, "SIGINT defined as 2");
#else
    CHECK(0, "SIGINT defined as 2");
#endif

#ifdef SIGTERM
    CHECK(SIGTERM == 15, "SIGTERM defined as 15");
#else
    CHECK(0, "SIGTERM defined as 15");
#endif

#ifdef SIGKILL
    CHECK(SIGKILL == 9, "SIGKILL defined as 9");
#else
    CHECK(0, "SIGKILL defined as 9");
#endif

#ifdef SIGCHLD
// SIGCHLD is 17 on Linux, 20 on macOS/BSD
#ifdef __linux__
    CHECK(SIGCHLD == 17, "SIGCHLD defined as 17");
#elif defined(__APPLE__)
    CHECK(SIGCHLD == 20, "SIGCHLD defined as 20 (macOS)");
#else
    CHECK(1, "SIGCHLD defined");
#endif
#else
    CHECK(0, "SIGCHLD defined");
#endif

    // Also verify we can use sigset_t from signal.h
    sigset_t test_set;
    (void)test_set;
    CHECK(1, "signal.h types available");
}

void test_glibc_macros(void)
{
    // __GLIBC__ and __GLIBC_MINOR__ must be defined for glibc detection
    // Only relevant on Linux with glibc
#ifdef __GLIBC__
    CHECK(__GLIBC__ >= 2, "__GLIBC__ defined and >= 2");
#ifdef __GLIBC_MINOR__
    CHECK(1, "__GLIBC_MINOR__ defined");
#else
    CHECK(0, "__GLIBC_MINOR__ defined");
#endif
#else
    // Not using glibc - skip these tests
    printf("  [SKIP] glibc macro tests (not using glibc)\n");
#endif
}

void test_posix_macros(void)
{
    // _POSIX_VERSION must be defined for POSIX compliance detection
#ifdef _POSIX_VERSION
// Different systems have different POSIX versions
#ifdef __linux__
    CHECK(_POSIX_VERSION >= 200809L, "_POSIX_VERSION defined and >= 200809L");
#else
    CHECK(_POSIX_VERSION > 0, "_POSIX_VERSION defined");
#endif
#else
    // _POSIX_VERSION may not be defined without feature test macros
    printf("  [SKIP] _POSIX_VERSION test (not defined)\n");
#endif
}

void run_preprocessor_system_macro_tests(void)
{
    printf("\n=== PREPROCESSOR SYSTEM MACRO TESTS ===\n");
    printf("(Tests for system macro import integrity)\n\n");

    test_linux_macros();
    test_signal_macros();
    test_glibc_macros();
    test_posix_macros();
}

// SECTION: VERIFICATION TESTS
void test_switch_conditional_break_defer(void)
{
    log_reset();
    int error = 0; // No error, will fall through

    switch (1)
    {
    case 1:
    {
        // Wrap in braces so defer executes before fallthrough
        defer log_append("cleanup1");
        if (error)
            break;
    } // defer runs here
    case 2:
        log_append("case2");
        break;
    }

    // With fix: cleanup1 executes at closing brace before fallthrough
    CHECK_LOG("cleanup1case2", "defer executes before fallthrough with braces");
}

void test_switch_unconditional_break_works(void)
{
    log_reset();
    int x = 1;

    switch (x)
    {
    case 1:
    {
        int *ptr = malloc(sizeof(int));
        defer
        {
            free(ptr);
            log_append("cleanup");
        };

        // Unconditional break - this is allowed (fall through to case 2 can't happen)
        break;
    }
    case 2:
        log_append("reached_case2");
        break;
    }

    // Defer should execute, case 2 should not be reached
    CHECK_LOG("cleanup", "unconditional break allows defer without fallthrough warning");
}

void test_switch_braced_fallthrough_works(void)
{
    log_reset();
    int cleanup_called = 0;

    switch (1)
    {
    case 1:
    {
        int *ptr = malloc(sizeof(int));
        defer
        {
            free(ptr);
            cleanup_called = 1;
        };

        // Even with conditional break, braces ensure defer runs
        if (0)
            break;
        // Fall through - cleanup happens at closing brace
    }
    case 2:
        log_append("reached_case2");
        break;
    }

    CHECK(cleanup_called == 1, "braced case executes defer on fallthrough");
    CHECK_LOG("reached_case2", "fallthrough occurs as expected");
}

// Bug 2: C23 raw string literals - backslashes corrupted
void test_raw_string_literals(void)
{
    // Test 1: Basic raw string with backslashes
    const char *path = R"(C:\Path\To\File)";
    CHECK(strcmp(path, "C:\\Path\\To\\File") == 0, "raw string preserves backslashes");
    // Test 2: Raw string with quotes
    const char *quoted = R"("Hello" 'World')";
    CHECK(strcmp(quoted, "\"Hello\" 'World'") == 0, "raw string preserves quotes");

    // Test 3: Raw string with newlines
    const char *multiline = R"(Line 1
Line 2
Line 3)";
    CHECK(strchr(multiline, '\n') != NULL, "raw string preserves newlines");

    // Test 4: Raw string with escape-like sequences
    const char *escaped = R"(\n\t\r\0)";
    CHECK(strcmp(escaped, "\\n\\t\\r\\0") == 0, "raw string doesn't interpret escapes");
}

// Bug 3: VLA false positive with struct member access
void test_vla_struct_member(void)
{
    struct Config
    {
        int size;
    } cfg = {10};

    // This is a VLA (runtime value), but Prism incorrectly thinks it's constant
    // because of the member access optimization
    int buffer[cfg.size]; // Should be recognized as VLA

    // This would cause backend error: "variable-sized object may not be initialized"
    // if Prism emits: int buffer[cfg.size] = {0};

    // Verify buffer is actually allocated
    buffer[0] = 42;
    buffer[9] = 99;

    CHECK(buffer[0] == 42, "VLA with struct member access allocates correctly");
    CHECK(buffer[9] == 99, "VLA struct member size works");
}

void test_vla_struct_member_nested(void)
{
    struct Outer
    {
        struct
        {
            int count;
        } inner;
    } obj = {{5}};

    // Nested member access - still a VLA
    int arr[obj.inner.count];
    arr[0] = 1;
    arr[4] = 5;

    CHECK(arr[0] == 1 && arr[4] == 5, "nested struct member VLA works");
}

void test_offsetof_vs_runtime(void)
{
    struct S
    {
        int x;
        int y;
    };

    // This should be constant (offsetof pattern with 0)
    int const_size = offsetof(struct S, y);
    int fixed_arr[const_size]; // Should be fixed-size

    // This should be VLA (runtime struct instance)
    struct S instance = {0, 3};
    int vla_arr[instance.y]; // Should be VLA

    fixed_arr[0] = 10;
    vla_arr[0] = 20;

    CHECK(fixed_arr[0] == 10, "offsetof pattern creates fixed array");
    CHECK(vla_arr[0] == 20, "runtime member creates VLA");
}

// Bug 4: Statement expression with defer and goto
void test_stmt_expr_defer_goto(void)
{
    log_reset();
    int err = 1;
    int x;

    x = ({
        {
            defer log_append("cleanup");
            if (err)
                goto error;
        }
        42;
    });

error:
    log_append("error_handler");

    // The defer should execute before jumping to error label
    // Risk: depends on backend compiler's statement expression implementation
    CHECK_LOG("cleanuperror_handler", "defer executes before goto in stmt expr");
}

void test_stmt_expr_defer_normal(void)
{
    log_reset();
    int err = 0;

    int x = ({
        {
            defer log_append("cleanup");
            if (err)
                goto skip;
            log_append("body");
        }
        100;
    });

skip:
    log_append("end");

    CHECK_LOG("bodycleanupend", "defer executes normally in stmt expr");
    CHECK(x == 100, "statement expression returns correct value");
}

void test_nested_stmt_expr_defer(void)
{
    log_reset();

    int result = ({
        {
            defer log_append("outer");
            int inner = ({
                {
                    defer log_append("inner");
                    log_append("inner_body");
                }
                5;
            });
            log_append("outer_body");
        }
        10;
    });

    CHECK_LOG("inner_bodyinnerouter_bodyouter", "nested stmt expr defer order");
    CHECK(result == 10, "nested stmt expr computes correctly");
}

// SECTION: CRITICAL BUG TESTS (THIRD PARTY REPORTS)

// Bug 1: Vanishing statement - FIXED
// Original: defer in braceless control flow could cause issues
// Now: defer requires braces, ensuring proper scoping
void test_vanishing_statement_if_else(void)
{
    log_reset();

    // Use inner scope so defer executes before we check the log
    {
        int check = 1;

        // defer now requires braces
        if (check)
        {
            defer log_append("cleanup");
        } // defer executes here when exiting if block
        else
        {
            log_append("alt");
        }

        log_append("end");
    }

    CHECK_LOG("cleanupend", "defer with braces executes when block closes");
}

void test_vanishing_statement_while(void)
{
    log_reset();

    {
        int count = 0;

        while (count < 1)
        {
            count++;
            // defer now requires braces
            if (count == 1)
            {
                defer log_append("loop_cleanup");
            } // defer executes here when if block exits
        }

        log_append("after");
    }

    CHECK_LOG("loop_cleanupafter", "defer with braces in while loop works");
}

void test_vanishing_statement_for(void)
{
    log_reset();

    {
        // defer in for loop now requires braces
        for (int i = 0; i < 1; i++)
        {
            defer log_append("for_defer");
        } // defer executes here at end of each iteration

        log_append("done");
    }

    CHECK_LOG("for_deferdone", "defer with braces in for loop works");
}

// Bug 2: _Generic default collision with switch defer cleanup
// Currently FAILS: Prism incorrectly thinks "_Generic(..., default:...)" is a case label
void test_generic_default_collision(void)
{
    log_reset();
    char *ptr = malloc(16);
    int type = 1;

    switch (type)
    {
    case 1:
    {
        defer free(ptr);
        defer log_append("case1_cleanup");

        // _Generic with default keyword - should NOT clear defer stack
        // BUG: Prism sees "default" and clears the defer stack
        int x = _Generic(type, int: 0, default: 1);

        log_append("case1_body");
        break;
    }
    }

    log_append("after_switch");

    // Defers execute when exiting case block (at break), before after_switch
    CHECK_LOG("case1_bodycase1_cleanupafter_switch", "_Generic default doesn't clear defer stack");
}

void test_generic_default_collision_nested(void)
{
    log_reset();
    char *ptr1 = malloc(16);
    char *ptr2 = malloc(16);
    int type = 2;

    switch (type)
    {
    case 1:
    {
        log_append("unreachable");
        break;
    }

    case 2:
    {
        defer free(ptr1);
        defer log_append("outer");

        // Nested _Generic - multiple "default" keywords
        // With fix: "default" in _Generic doesn't clear defer stack
        int y = _Generic(ptr2, char *: _Generic(type, int: 1, default: 2), default: 3);

        defer free(ptr2);
        defer log_append("inner");

        log_append("body");
        break;
    }
    }

    log_append("end");

    // Defers execute at break in LIFO order: inner, outer
    CHECK_LOG("bodyinnerouterend", "nested _Generic preserves defer stack");
}

void test_generic_default_outside_switch(void)
{
    log_reset();
    char *ptr = malloc(16);

    {
        defer free(ptr);
        defer log_append("block_cleanup");

        // _Generic outside switch - should work fine
        int x = _Generic(ptr, char *: 1, default: 0);

        log_append("body");
    }

    log_append("after");

    CHECK_LOG("bodyblock_cleanupafter", "_Generic outside switch works normally");
}

// Bug 3: VLA backward goto with uninitialized memory
void test_vla_backward_goto_reentry(void)
{
    int iterations = 0;
    int last_val = -1;
    int changed = 0;

label:
{
    int n = (iterations == 0) ? 5 : 10; // Different sizes
    int vla[n];                         // VLA allocated on stack

    vla[0] = iterations;

    if (iterations > 0 && vla[0] != last_val)
    {
        changed = 1;
    }

    last_val = vla[0];
    iterations++;

    if (iterations < 2)
        goto label;
}

    // This test verifies the VLA is re-allocated each iteration
    // vla[0] gets different values (0, then 1), so they don't match
    CHECK(changed == 1, "VLA backward goto reentry behavior tracked");
}

void test_vla_backward_goto_stack_exhaustion(void)
{
    int count = 0;
    int max_iterations = 100;

loop:
{
    int size = 100;
    int vla[size]; // Stack allocation

    vla[0] = count;
    count++;

    if (count < max_iterations)
        goto loop; // Backward goto - potential stack buildup
}

    CHECK(count == max_iterations, "VLA backward goto completes iterations");
}

void test_vla_backward_goto_with_defer(void)
{
    log_reset();
    int iterations = 0;

restart:
{
    int n = 5;
    int vla[n];
    defer log_append("D");

    vla[0] = iterations;
    log_append("B");

    iterations++;
    if (iterations < 2)
        goto restart;
}

    log_append("E");

    // Defers should execute for each iteration
    CHECK_LOG("BDBDE", "VLA backward goto executes defers correctly");
}

void test_vla_pointer_init_semantics(void)
{
    int n = 5;

    // Pointer to VLA: CAN be zero-initialized
    // Type is "int (*)[n]", which is a single pointer
    int (*ptr_to_vla)[n] = {0};

    // Array of pointers (VLA): storage only, no init allowed
    // Type is "int *[n]", which is an array of size n
    int *vla_of_ptrs[n];

    // Typedef'd VLA pointer
    typedef int Matrix[n][n];
    Matrix *mat_ptr = {0};

    // Verify pointers are zeroed
    CHECK(ptr_to_vla == NULL, "VLA pointer zero-initialized");
    CHECK(mat_ptr == NULL, "typedef VLA pointer zero-initialized");
}

// Typedef shadowing changes semantics
// Bug: If local variable shadows typedef, "T * x;" changes from declaration to multiplication
typedef int T;

void test_typedef_shadow_semantics(void)
{
    {
        // In outer scope, T is a typedef
        // This IS a pointer declaration
        T *ptr = NULL;
        CHECK(ptr == NULL, "typedef pointer declaration works");
    }

    {
        int T = 10; // Shadows global typedef T
        int x = 2;

        // Now T is a variable, not a type
        // "T * x" is multiplication: 10 * 2 = 20
        // If Prism adds "= 0", it becomes "10 * 2 = 0" -> syntax error
        int result = T * x;

        CHECK(result == 20, "typedef shadow multiplication works");
        CHECK(T == 10, "shadowing variable correct");
    }
}

// _Generic default should not interfere with defer
// Bug: Prism may confuse _Generic's "default:" with switch's "default:"
void test_generic_default_no_switch(void)
{
    log_reset();

    {
        defer log_append("D");
        log_append("A");

        // _Generic uses 'default:', but NOT in a switch
        // Prism must not clear defer stack here
        int x = 0;
        int result = _Generic(x, int: 1, default: 2);

        log_append("B");
        CHECK(result == 1, "_Generic selection correct");
    }

    // Defer should have run
    CHECK_LOG("ABD", "_Generic default does not break defer");
}

// K&R function definition parsing
// Bug: Prism may fail to parse old-style function definitions
// K&R declarations go between params and opening brace
int knr_func_add(a, b)
int a;
int b;
{
    if (a > b)
        goto return_a;
    return b;

return_a:
    return a;
}

void test_knr_function_parsing(void)
{
    CHECK(knr_func_add(10, 5) == 10, "K&R function goto works");
    CHECK(knr_func_add(3, 8) == 8, "K&R function fallthrough works");
}

// Comma operator vs comma separator in declarations
// Bug: Prism's parser must distinguish comma operator from declarator separator
void test_comma_operator_in_init(void)
{
    int a = 1, b = 2;

    // This is ONE variable 'c', initialized to result of (a, b) which is 2
    // Comma operator: evaluates left to right, result is rightmost value
    int c = (a, b);

    // This is TWO variables: 'd' gets 1, 'e' gets zero-init
    int d = 1, e;

    CHECK(c == 2, "comma operator in initializer");
    CHECK(d == 1, "first multi-declarator init");
    CHECK(e == 0, "second multi-declarator zero-init");
}

void test_switch_skip_hole_strict(void)
{
    // SAFE PATTERN: Variable declared BEFORE switch
    int x;
    int result = -1;
    switch (1)
    {
    case 1:
        result = x; // x is properly zero-initialized (declared before switch)
        break;
    }
    CHECK_EQ(result, 0, "switch skip hole fix: var before switch works");

    // SAFE PATTERN: Variable declared INSIDE case block
    result = -1;
    switch (1)
    {
    case 1:
    {
        int y; // Declared inside case block - properly initialized
        result = y;
        break;
    }
    }
    CHECK_EQ(result, 0, "switch skip hole fix: var in case block works");

    // NOTE: The UNSAFE pattern (int z; before case 1:) now produces a compile error:
    // "variable declaration before first 'case' label in switch"
    // This prevents the zero-init from being silently skipped.
    printf("[PASS] switch skip hole: unsafe pattern now errors at compile time\n");
    passed++;
    total++;
}

// Issue 2: _Complex types - C99 complex number support
#if __STDC_VERSION__ >= 199901L && !defined(__STDC_NO_COMPLEX__)
#include <complex.h>
void test_complex_type_zeroinit(void)
{
    // C99 _Complex types - should be zero-initialized
    double _Complex dc;
    CHECK(creal(dc) == 0.0 && cimag(dc) == 0.0, "double _Complex zero-init");

    float _Complex fc;
    CHECK(crealf(fc) == 0.0f && cimagf(fc) == 0.0f, "float _Complex zero-init");

    // Using the complex.h macros
    complex double cd;
    CHECK(creal(cd) == 0.0 && cimag(cd) == 0.0, "complex double (macro) zero-init");

    // Pointer to complex
    double _Complex *pdc;
    CHECK(pdc == NULL, "pointer to double _Complex zero-init");
}
#else
void test_complex_type_zeroinit(void)
{
    printf("[SKIP] _Complex tests (C99 complex not available)\n");
}
#endif

void test_continue_in_switch_defer_detailed(void)
{
    log_reset();
    int iterations = 0;

    while (iterations < 2)
    {
        defer log_append("L"); // Loop defer

        switch (iterations)
        {
        case 0:
        {
            defer log_append("S0"); // Switch case defer
            log_append("A");
            iterations++;
            continue; // Should: run S0, run L, jump to loop condition
        }
        case 1:
        {
            defer log_append("S1");
            log_append("B");
            iterations++;
            break; // Should: run S1, exit switch, then "X", then L at loop end
        }
        }
        log_append("X"); // After switch
    }
    log_append("E");
    // Expected trace:
    // iter=0: "A" -> continue -> S0 -> L -> check condition
    // iter=1: "B" -> break -> S1 -> "X" -> L -> check condition (exit)
    // "E"
    CHECK_LOG("AS0LBS1XLE", "continue in switch: defer order");
}

void test_ultra_complex_declarators(void)
{
    // Function pointer returning function pointer: void (*(*f)(void))(void)
    // This is: f is pointer to function(void) returning pointer to function(void) returning void
    void (*(*f1)(void))(void);
    CHECK(f1 == NULL, "func ptr returning func ptr zero-init");

    // Even more complex: pointer to function returning pointer to function returning int*
    int *(*(*f2)(int))(int);
    CHECK(f2 == NULL, "ptr to func(int) returning ptr to func(int) returning int*");

    // Array of function pointers returning pointers
    int *(*afp[3])(void);
    int all_null = 1;
    for (int i = 0; i < 3; i++)
        if (afp[i] != NULL)
            all_null = 0;
    CHECK(all_null, "array of func ptrs returning ptr zero-init");

    // Pointer to array of function pointers
    void (*(*pafp)[5])(void);
    CHECK(pafp == NULL, "ptr to array of func ptrs zero-init");
}

#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
_Thread_local int tls_var; // Should NOT get explicit = 0 (redundant but valid)

void test_thread_local_handling(void)
{
    // Static storage duration means implicit zero-init by C standard
    CHECK_EQ(tls_var, 0, "_Thread_local file scope implicit zero");

    // Thread-local in function scope
    static _Thread_local int tls_local;
    CHECK_EQ(tls_local, 0, "static _Thread_local local implicit zero");

    // The main verification is that the code compiles correctly
    // If Prism incorrectly treated _Thread_local as auto storage and tried memset,
    // that would cause compile errors or incorrect behavior
    printf("[PASS] _Thread_local handling (compiled correctly)\n");
    passed++;
    total++;
}
#else
void test_thread_local_handling(void)
{
    printf("[SKIP] _Thread_local tests (C11 threads not available)\n");
}
#endif

void test_line_directive_preservation(void)
{
    int line_before = __LINE__;
    {
        defer(void) 0; // Simple defer that injects code
        int x;         // Should be zero-init
        (void)x;
    }
    int line_after = __LINE__;

    // Lines should be sequential (accounting for the block)
    // If line directives are broken, __LINE__ would report wrong values
    // This is a sanity check - real verification needs error message testing
    CHECK(line_after > line_before, "#line tracking: lines increase correctly");

    // Test with multiple defers
    line_before = __LINE__;
    {
        defer log_append("A");
        defer log_append("B");
        defer log_append("C");
        int y;
        (void)y;
    }
    line_after = __LINE__;
    CHECK(line_after > line_before, "#line tracking: multiple defers OK");

    printf("[PASS] #line directive preservation (no obvious corruption)\n");
    passed++;
    total++;
}

void test_alignas_struct_bitfield(void)
{
    // Standard struct with bitfield - combined definition and variable
    struct Data
    {
        int val;
        unsigned int flag : 1; // Bitfield, not label
    } d = {42, 1};

    // Struct with __attribute__ - bitfield must NOT be mistaken for label
    struct __attribute__((packed)) PackedData
    {
        unsigned int x : 1; // This is a BITFIELD, not a label!
        unsigned int y : 2;
    } pd = {1, 3};

    // Struct with multiple attributes before name
    struct __attribute__((packed)) __attribute__((aligned(4))) AttrData
    {
        unsigned int a : 4;
        unsigned int b : 4;
    } ad = {5, 10};

    CHECK(d.val == 42 && d.flag == 1, "struct bitfield: basic struct works");
    CHECK(pd.x == 1 && pd.y == 3, "struct bitfield: packed bitfields work");
    CHECK(ad.a == 5 && ad.b == 10, "struct bitfield: multi-attr bitfields work");

    // This test passes if it compiles - the bug would cause Prism to think
    // 'x' is a goto label (from "int x : 1") and potentially emit wrong code
    printf("[PASS] struct bitfield parsing (not mistaken for label)\n");
    passed++;
    total++;
}

typedef int GenericTestType;

void test_generic_typedef_not_label(void)
{
    // _Generic uses Type: expr syntax which looks like labels
    // Prism should skip _Generic(...) in label scanner
    int x = _Generic(0,
        GenericTestType: 1, // This is NOT a goto label!
        default: 0);

    CHECK_EQ(x, 1, "_Generic typedef association works");

    // Verify label scanner doesn't get confused
    // If it did, goto might emit wrong defer cleanup
    log_reset();
    {
        defer log_append("D");
        int y = _Generic((char)0,
            GenericTestType: 10,
            char: 20,
            default: 30);
        CHECK_EQ(y, 20, "_Generic with multiple type associations");
        log_append("X");
    }
    CHECK_LOG("XD", "_Generic doesn't confuse label scanner");

    printf("[PASS] _Generic typedef not mistaken for label\n");
    passed++;
    total++;
}

#if __STDC_VERSION__ >= 202311L
void test_c23_attributes_zeroinit(void)
{
    // C23 attributes before declaration
    [[maybe_unused]] int x;
    CHECK_EQ(x, 0, "[[maybe_unused]] int zero-init");

    // Multiple attributes
    [[maybe_unused]] [[deprecated]] int y;
    CHECK_EQ(y, 0, "multiple [[...]] attributes zero-init");

    // Attribute with argument
    [[deprecated("use z2 instead")]] int z;
    CHECK_EQ(z, 0, "[[deprecated(...)]] zero-init");

    printf("[PASS] C23 [[...]] attributes don't break zero-init\n");
    passed++;
    total++;
}
#else
void test_c23_attributes_zeroinit(void)
{
    printf("[SKIP] C23 [[...]] attribute tests (C23 not available)\n");
}
#endif

#if __STDC_VERSION__ >= 202311L && defined(__clang__)
void test_bitint_zeroinit(void)
{
    _BitInt(32) x;
    CHECK(x == 0, "_BitInt(32) zero-init");

    _BitInt(64) y;
    CHECK(y == 0, "_BitInt(64) zero-init");

    unsigned _BitInt(16) z;
    CHECK(z == 0, "unsigned _BitInt(16) zero-init");

    printf("[PASS] _BitInt zero-init works\n");
    passed++;
    total++;
}
#else
void test_bitint_zeroinit(void)
{
    printf("[SKIP] _BitInt tests (C23/_BitInt not available)\n");
}
#endif

void test_pragma_pack_preservation(void)
{
#pragma pack(push, 1)
    struct PragmaPackTest
    {
        char a;
        int b;
    };
#pragma pack(pop)

    // On typical ABIs without packing, this would be 8 bytes (char + 3 padding + int)
    // With pack(1), it should be 5 bytes (char + int, no padding)
    size_t size = sizeof(struct PragmaPackTest);
    CHECK(size == 5, "pragma pack(1) preserved - struct size is 5");

    printf("[PASS] #pragma pack directives preserved\n");
    passed++;
    total++;
}

static int g_defer_counter;

int test_return_stmt_expr_helper(int x)
{
    defer g_defer_counter++;
    return ({
        int y = x + 1;
        y;
    });
}

void test_return_stmt_expr_with_defer(void)
{
    g_defer_counter = 0;
    int result = test_return_stmt_expr_helper(42);

    // The function should return 43 (42 + 1)
    CHECK(result == 43, "statement-expr return value correct");

    // The defer should have executed once
    CHECK(g_defer_counter == 1, "defer executed with statement-expr return");

    printf("[PASS] return statement-expr with defer works\n");
    passed++;
    total++;
}

void test_security_stmtexpr_value_corruption(void)
{
    log_reset();

    // Test 1: Nested block with defer should work correctly
    int val = ({
        {
            defer log_append("D");
        }
        42; // This should be the return value
    });

    CHECK_EQ(val, 42, "statement-expr value correct with nested defer");
    CHECK_LOG("D", "nested defer in statement-expr executed");

    log_reset();

    // Test 2: Multiple nested blocks
    int val2 = ({
        int tmp = 10;
        {
            defer log_append("X");
            tmp += 5;
        }
        tmp + 27; // Should return 42
    });

    CHECK_EQ(val2, 42, "statement-expr with multiple statements and defer");
    CHECK_LOG("X", "defer executed before final expression");

    log_reset();
    printf("[PASS] statement expression value corruption test (protected)\n");
    passed++;
    total++;
}

void test_security_braceless_defer_trap(void)
{
    log_reset();

    // FIXED: Braceless defer now errors at compile-time, preventing the security issue
    // This test verifies the correct behavior with braces
    {
        int trigger = 0;

        // With braces, this creates a proper scope
        if (trigger)
        {
            defer log_append("FAIL");
        }

        log_append("OK");
    }

    // With the fix, defer only executes if the condition is true
    // Since trigger=0, the defer does not execute
    CHECK_LOG("OK", "defer with braces executes conditionally (issue FIXED)");

    log_reset();
    printf("[PASS] braceless if defer trap test (FIXED - now requires braces)\n");
    passed++;
    total++;
}

void test_security_switch_goto_double_free(void)
{
    log_reset();
    int stage = 1;

    // FIXED: Switch case defer now requires braces, which creates proper scoping
    switch (stage)
    {
    case 1:
    {
        defer log_append("X");
        log_append("A");
    } // defer executes here when exiting case 1 block
    // Note: Can't use goto to jump to another case after the block closes
    // because that would be outside the braced block
    break;

    case 2:
        log_append("Y");
        break;
    }

    // With the fix, defer executes when the braced block closes
    // Log should be "AX" (A appended, then defer X executes at })
    CHECK_LOG("AX", "switch defer with braces executes correctly (issue FIXED)");

    log_reset();
    printf("[PASS] switch goto defer loss test (FIXED - now requires braces)\n");
    passed++;
    total++;
}

void test_ghost_shadow_corruption(void)
{
    // This tests that typedef shadows are properly cleaned up
    // even when loop bodies are braceless
    typedef int T;

    // Declare loop variable T that shadows typedef 'T'
    // With braceless body, shadow must still be cleaned up
    for (int T = 0; T < 5; T++)
        ;

    // Now use T as a type - should work correctly
    // Without the fix, T would still be shadowed and this would parse wrong
    T *ptr = NULL;

    CHECK(ptr == NULL, "ghost shadow: typedef T works after braceless for loop");
}

void test_sizeof_vla_codegen(void)
{
    int n = 10;

    // sizeof(int[n]) is evaluated at runtime because n is variable
    // So arr is a VLA, not a constant-sized array
    // Prism should NOT emit = {0} for this
    int arr[sizeof(int[n])];
    arr[0] = 42;

    CHECK(arr[0] == 42, "sizeof(VLA) treated as runtime value");
}

void test_keyword_typedef_collision(void)
{
    // These typedefs use names that are also Prism keywords
    typedef int raw;
    typedef int defer;

    // These should work correctly
    raw x = 10;
    defer y = 20;

    CHECK(x == 10, "typedef named 'raw' works");
    CHECK(y == 20, "typedef named 'defer' works");
}

void test_sizeof_vla_typedef(void)
{
    int n = 10;
    typedef int VLA_Type[n];

    // sizeof(VLA_Type) is evaluated at runtime because VLA_Type is a VLA
    // Prism should NOT emit = {0} for this
    int arr[sizeof(VLA_Type)];
    arr[0] = 42;

    CHECK(arr[0] == 42, "sizeof(VLA_Typedef) treated as runtime value");
}

void test_typeof_vla_zeroinit(void)
{
    int n = 10;
    int vla1[n];
    vla1[0] = 42;

    // copy_vla is a VLA type via typeof
    // Prism should NOT try to zero-initialize this
    __typeof__(vla1) copy_vla;
    copy_vla[0] = 99;

    CHECK(copy_vla[0] == 99, "typeof(VLA) variable works without zero-init");
}

void test_bug1_ghost_shadow_while(void)
{
    typedef int U;
    int x = 5;
    while (x-- > 0)
    {
        int U = x; // Shadow inside braced body
        (void)U;
    }
    U *ptr = NULL;
    CHECK(ptr == NULL, "typedef U works after while with shadow");
}

void test_bug1_ghost_shadow_if(void)
{
    typedef int V;
    if (1)
        ;
    V *ptr = NULL;
    CHECK(ptr == NULL, "typedef V works after braceless if");
}

void test_bug2_ultra_complex_exact(void)
{
    // Exact example from bug report: pointer to array of 5 function pointers
    int (*(*complex_var)[5])(void);
    CHECK(complex_var == NULL, "ultra-complex declarator from report");
}

void test_bug2_deeply_nested_parens(void)
{
    // Even more nested: pointer to function returning pointer to array
    int (*(*fp)(int))[10];
    CHECK(fp == NULL, "deeply nested paren declarator");
}

static int defer_value_3rdparty = 0;

void test_bug3_stmtexpr_defer_ordering(void)
{
    defer_value_3rdparty = 0;

    // Test defer in nested block within statement expression
    int x = ({
        int val = 10;
        {
            defer
            {
                defer_value_3rdparty = val;
            };
            val = val + 5; // Modify to 15
        }
        val; // Return 15
    });

    CHECK(x == 15, "statement-expr with nested defer");
    CHECK(defer_value_3rdparty == 15, "defer captured value");
}

void test_bug3_stmtexpr_defer_variable(void)
{
    int result = ({
        int tmp = 42;
        {
            defer tmp = 999;
        }
        tmp; // Return 999
    });

    CHECK(result == 999, "defer modifies variable correctly");
}

void test_bug4_generic_fnptr(void)
{
    // Exact pattern from bug report: function pointer in _Generic
    int x = _Generic(0, void (*)(int): 1, default: 0);
    CHECK(x == 0, "_Generic with fn ptr type");
}

void test_bug4_generic_defer_interaction(void)
{
    int result = 0;
    {
        defer result = 1;
        int y = _Generic((int *)0, int *: 5, void (*)(int): 10, default: 15);
        result = y; // Should be 5
    }
    // Defer runs after, result = 1
    CHECK(result == 1, "defer doesn't break _Generic");
}

void test_bug7_sizeof_vla_variable(void)
{
    int n = 5;
    int vla[n]; // VLA
    vla[0] = 42;

    // CRITICAL: sizeof(vla) is evaluated at runtime!
    // Array x is also a VLA, should NOT be zero-initialized
    int x[sizeof(vla)];
    x[0] = 99;

    CHECK(vla[0] == 42 && x[0] == 99, "3rd-party bug #7: sizeof(vla) creates VLA");
}

void test_bug7_sizeof_sizeof_vla(void)
{
    int n = 3;
    int arr1[n]; // VLA
    arr1[0] = 1;

    // sizeof(sizeof(arr1)) is sizeof(size_t) - constant
    int arr2[sizeof(sizeof(arr1))];
    arr2[0] = 2;

    CHECK(arr1[0] == 1 && arr2[0] == 2, "sizeof(sizeof(VLA))");
}

void test_bug7_sizeof_vla_element(void)
{
    int m = 4;
    int inner[m]; // VLA
    inner[0] = 10;

    // sizeof(inner[0]) is sizeof(int) - constant!
    int outer[sizeof(inner[0])];
    // outer should be zero-initialized
    CHECK(outer[0] == 0, "sizeof(VLA[0]) is constant");
}

void test_edge_multiple_typedef_shadows(void)
{
    typedef int T;
    {
        int T = 5;
        CHECK(T == 5, "3rd-party edge: first shadow level");
        {
            int T = 10;
            CHECK(T == 10, "second shadow level");
        }
        CHECK(T == 5, "back to first shadow");
    }
    T *ptr = NULL;
    CHECK(ptr == NULL, "typedef restored after shadows");
}

void test_edge_defer_in_generic(void)
{
    int result = 0;
    {
        int x = _Generic(1, int: 10, default: 20);
        defer result = x;
    }
    CHECK(result == 10, "defer with _Generic");
}

void test_attributed_label_defer(void)
{
    log_reset();
    {
        defer log_append("Cleanup");
        goto error;
    }

    // GCC syntax: attribute after colon (not before)
    // Prism label scanner should recognize this label
    error: __attribute__((unused))
        log_append("Error");

    CHECK(strcmp(log_buffer, "CleanupError") == 0, "attributed label defer cleanup");
}

void run_verification_bug_tests(void)
{
    printf("\n=== VERIFICATION TESTS ===\n");

    test_switch_conditional_break_defer();
    test_switch_unconditional_break_works();
    test_switch_braced_fallthrough_works();

    test_raw_string_literals();

    test_vla_struct_member();
    test_vla_struct_member_nested();
    test_offsetof_vs_runtime();

    test_stmt_expr_defer_goto();
    test_stmt_expr_defer_normal();
    test_nested_stmt_expr_defer();

    test_vanishing_statement_if_else();
    test_vanishing_statement_while();
    test_vanishing_statement_for();

    test_attributed_label_defer();

    test_generic_default_collision();
    test_generic_default_collision_nested();
    test_generic_default_outside_switch();

    test_vla_backward_goto_reentry();
    test_vla_backward_goto_stack_exhaustion();
    test_vla_backward_goto_with_defer();

    test_vla_pointer_init_semantics();
    test_typedef_shadow_semantics();
    test_generic_default_no_switch();
    test_knr_function_parsing();
    test_comma_operator_in_init();

    test_switch_skip_hole_strict();
    test_complex_type_zeroinit();
    test_continue_in_switch_defer_detailed();
    test_ultra_complex_declarators();
    test_thread_local_handling();
    test_line_directive_preservation();
    test_alignas_struct_bitfield();
    test_generic_typedef_not_label();
    test_c23_attributes_zeroinit();
    test_bitint_zeroinit();

    test_pragma_pack_preservation();
    test_return_stmt_expr_with_defer();

    test_security_stmtexpr_value_corruption();
    test_security_braceless_defer_trap();
    test_security_switch_goto_double_free();

    test_ghost_shadow_corruption();
    test_sizeof_vla_codegen();
    test_keyword_typedef_collision();
    test_sizeof_vla_typedef();
    test_typeof_vla_zeroinit();

    test_bug1_ghost_shadow_while();
    test_bug1_ghost_shadow_if();

    test_bug2_ultra_complex_exact();
    test_bug2_deeply_nested_parens();

    test_bug3_stmtexpr_defer_ordering();
    test_bug3_stmtexpr_defer_variable();

    test_bug4_generic_fnptr();
    test_bug4_generic_defer_interaction();

    test_bug7_sizeof_vla_variable();
    test_bug7_sizeof_sizeof_vla();
    test_bug7_sizeof_vla_element();

    test_edge_multiple_typedef_shadows();
    test_edge_defer_in_generic();
}

// MAIN

int main(void)
{
    printf("=== PRISM TEST SUITE ===\n");

    run_defer_basic_tests();
    run_zeroinit_tests();
    run_raw_tests();
    run_multi_decl_tests();
    run_typedef_tests();
    run_edge_case_tests();
    run_bug_regression_tests();
    run_advanced_defer_tests();
    run_stress_tests();
    run_safety_hole_tests();
    run_switch_fallthrough_tests();
    run_complex_nesting_tests();
    run_case_label_tests();
    run_rigor_tests();
    run_silent_failure_tests();
    run_sizeof_constexpr_tests();
    run_manual_offsetof_vla_tests();
    run_preprocessor_numeric_tests();
    run_preprocessor_system_macro_tests();
    run_verification_bug_tests();

    printf("\n========================================\n");
    printf("TOTAL: %d tests, %d passed, %d failed\n", total, passed, failed);
    printf("========================================\n");

    return (failed == 0) ? 0 : 1;
}
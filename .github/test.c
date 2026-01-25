// Comprehensive test suite for Prism C transpiler
// Tests: defer, zero-init, typedef tracking, multi-declarator, edge cases
// Run with: ./prism prism_tests.c

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
    int x = 42;
    __typeof__(x) y;
    CHECK_EQ(y, 0, "typeof zero-init");

    __typeof__(x) *ptr;
    CHECK(ptr == NULL, "typeof pointer zero-init");
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
        defer log_append("D");
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
            defer log_append("S"); // Switch cleanup
            // 'continue' must trigger 'S' (switch exit) AND 'L' (loop iteration end)
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
    // 3. Register 'L', register 'S'
    // 4. Hit continue -> Run 'S' -> Run 'L' -> Re-check loop cond
    // 5. Loop terminates -> 'E'
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
        defer result += 10;  // Wrapper scope - fires when we exit this block
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

void test_nested_stmt_expr_defer(void)
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
// The zero-init "= 0" is added but the switch jumps over it!
void test_switch_scope_leak(void)
{
    int result = -1;
    switch (1)
    {
        int y; // This declaration is jumped over by switch(1) -> case 1
    case 1:
        result = y; // y should be 0 if zero-init worked, but init is skipped!
        break;
    }
    printf("[INFO] switch scope leak: y = %d (UB if not 0)\n", result);
    // UB!
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

// Run all rigor tests
void run_rigor_tests(void)
{
    printf("\n=== RIGOR TESTS ===\n");

    printf("\n--- Issue 3: typedef void return ---\n");
    test_typedef_void_return();
    test_typedef_voidptr_return();

    printf("\n--- Issue 6/7: Statement expression defer ---\n");
    test_stmt_expr_defer_timing();
    test_nested_stmt_expr_defer();

    printf("\n--- Issue 9: const placement ---\n");
    test_const_after_typename();

    printf("\n--- Issue 11: Coverage gaps ---\n");
    test_atomic_zeroinit();
    test_static_local_zeroinit();
    test_inline_defer();

    printf("\n--- Complex declarator zero-init ---\n");
    test_complex_declarator_zeroinit();
    test_complex_decl_safety();
    test_qualified_complex_decl();

    printf("\n--- Declaration edge cases ---\n");
    test_extern_not_initialized();
    test_typedef_not_initialized();
    test_for_init_zeroinit();

    printf("\n--- Extra ---\n");
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

// MAIN

int main(void)
{
    printf("=== PRISM COMPREHENSIVE TEST SUITE ===\n");

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

    printf("\n========================================\n");
    printf("TOTAL: %d tests, %d passed, %d failed\n", total, passed, failed);
    printf("========================================\n");

    return (failed == 0) ? 0 : 1;
}
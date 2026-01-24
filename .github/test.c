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
// These tests verify that valid goto patterns still work correctly,
// while invalid patterns (goto skipping declarations) are caught at compile time.

// Test: goto jumping OVER an entire block is valid
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

    printf("\n========================================\n");
    printf("TOTAL: %d tests, %d passed, %d failed\n", total, passed, failed);
    printf("========================================\n");

    return (failed == 0) ? 0 : 1;
}
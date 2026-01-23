// Comprehensive edge case tests for prism
// Testing obscure C patterns, defer interactions, and zero-init edge cases

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

static char log_buffer[1024];
static int log_pos = 0;
static int passed = 0;
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

static int check_log(const char *expected, const char *test_name)
{
    total++;
    if (strcmp(log_buffer, expected) == 0)
    {
        printf("[PASS] %s\n", test_name);
        passed++;
        return 1;
    }
    else
    {
        printf("[FAIL] %s\n", test_name);
        printf("  Expected: '%s'\n", expected);
        printf("  Got:      '%s'\n", log_buffer);
        return 0;
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
        }                                \
    } while (0)

//=============================================================================
// SECTION 1: Bitfield zero-init (tricky - can't use {0} directly)
//=============================================================================

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

void test_mixed_bitfield_regular(void)
{
    struct
    {
        int regular;
        unsigned int flag : 1;
        unsigned int value : 7;
        int another_regular;
    } mixed;
    CHECK(mixed.regular == 0 && mixed.flag == 0 &&
              mixed.value == 0 && mixed.another_regular == 0,
          "mixed bitfield/regular zero-init");
}

//=============================================================================
// SECTION 2: Anonymous struct/union zero-init
//=============================================================================

void test_anonymous_struct(void)
{
    struct
    {
        int x;
        struct
        { // anonymous inner struct
            int a;
            int b;
        };
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
        { // anonymous union
            int i;
            float f;
            char c;
        };
    } u;
    CHECK(u.type == 0 && u.i == 0, "anonymous union zero-init");
}

//=============================================================================
// SECTION 3: Flexible array member (can't be zero-init)
//=============================================================================

// Note: flexible array members can't be stack allocated or zero-initialized
// This just tests that we handle the struct definition correctly
struct FlexArray
{
    int count;
    char data[]; // flexible array member
};

void test_flexible_array_ptr(void)
{
    struct FlexArray *fa; // pointer to struct with flexible array
    CHECK(fa == NULL, "flexible array struct pointer zero-init");
}

//=============================================================================
// SECTION 4: Compound literal interaction with defer
//=============================================================================

void test_defer_with_compound_literal(void)
{
    log_reset();
    {
        int *p = (int[]){1, 2, 3}; // compound literal
        defer log_append("D");
        log_append("1");
    }
    log_append("E");
}

//=============================================================================
// SECTION 5: Statement expressions (GCC extension) with defer
// NOTE: defer inside statement expressions is NOT supported (complex scoping)
//=============================================================================

#if 0 // KNOWN LIMITATION: defer doesn't work inside statement expressions
#ifdef __GNUC__
void test_defer_in_stmt_expr(void) {
    log_reset();
    int result = ({
        defer log_append("D");  // defer inside statement expression
        log_append("1");
        42;
    });
    log_append("E");
    // Note: defer should run when stmt expr scope ends
}
#endif
#endif

//=============================================================================
// SECTION 6: Comma operator vs declaration disambiguation
//=============================================================================

void test_comma_operator(void)
{
    int a;
    int b;
    a = 1, b = 2; // comma operator, NOT declaration
    CHECK(a == 1 && b == 2, "comma operator (not declaration)");
}

void test_comma_in_for(void)
{
    int sum;
    for (int i = 0, j = 10; i < j; i++, j--)
    {
        sum += i;
    }
    // Just verify it compiles and runs
    CHECK(1, "comma in for loop");
}

//=============================================================================
// SECTION 7: sizeof with declarations (not executed)
//=============================================================================

void test_sizeof_with_vla(void)
{
    int n = 10;
    size_t s = sizeof(int[n]); // VLA in sizeof - n IS evaluated
    CHECK(s == 10 * sizeof(int), "sizeof VLA");
}

void test_sizeof_regular(void)
{
    size_t s = sizeof(struct { int x; int y; });
    CHECK(s >= 2 * sizeof(int), "sizeof anonymous struct");
}

//=============================================================================
// SECTION 8: _Alignas qualified variables
//=============================================================================

void test_alignas_zeroinit(void)
{
    _Alignas(16) int aligned;
    _Alignas(32) char buf[64];
    CHECK(aligned == 0 && buf[0] == 0, "_Alignas zero-init");
}

//=============================================================================
// SECTION 9: Defer with Duff's device (interleaved switch/loop)
//=============================================================================

void test_duffs_device_defer(void)
{
    log_reset();
    int count = 5;
    int n = (count + 3) / 4;

    defer log_append("F"); // function-level defer

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
    // Expected: XXXXXEF (5 X's, E, then F at function end)
}

//=============================================================================
// SECTION 10: Multiple returns with different defer states
//=============================================================================

int test_multi_return_defer(int x)
{
    log_reset();
    defer log_append("A");

    if (x == 1)
    {
        defer log_append("B");
        log_append("1");
        return 1; // Should emit BA
    }

    if (x == 2)
    {
        log_append("2");
        return 2; // Should emit A only
    }

    defer log_append("C");
    log_append("3");
    return 3; // Should emit CA
}

//=============================================================================
// SECTION 11: Defer with conditional operator
//=============================================================================

void test_defer_ternary_complex(void)
{
    log_reset();
    int x = 1;
    defer(x ? log_append("T") : log_append("F"));
    log_append("1");
}

//=============================================================================
// SECTION 12: Labels with same name as types/keywords
//=============================================================================

void test_tricky_labels(void)
{
    log_reset();
    int x = 1;
    defer log_append("D");

    // Labels that look like they could be keywords or types
    if (x)
        goto int_label;
    log_append("X");

int_label:
    log_append("1");
    if (x)
    {
        x = 0;
        goto char_label;
    }

char_label:
    log_append("2");
}

//=============================================================================
// SECTION 13: Nested ternary with labels (parser stress test)
//=============================================================================

void test_nested_ternary(void)
{
    int a, b, c;
    int x = 1, y = 2, z = 3;
    // Deeply nested ternary - : is NOT a label!
    a = x ? (y ? 1 : 2) : (z ? 3 : 4);
    b = x ? y ? z ? 1 : 2 : 3 : 4;
    CHECK(a == 1 && b == 1, "nested ternary (no label confusion)");
}

//=============================================================================
// SECTION 14: typeof and __auto_type
//=============================================================================

#ifdef __GNUC__
void test_typeof_zeroinit(void)
{
    int template_var = 42;
    typeof(template_var) x; // Should be zero-init
    CHECK(x == 0, "typeof zero-init");
}

void test_auto_type_init(void)
{
    __auto_type x = 42; // Has initializer, shouldn't add = 0
    CHECK(x == 42, "__auto_type with init preserved");
}
#endif

//=============================================================================
// SECTION 15: Macro expanding to defer
//=============================================================================

#define SCOPED_LOG(msg) defer log_append(msg)

void test_defer_via_macro(void)
{
    log_reset();
    {
        SCOPED_LOG("M");
        log_append("1");
    }
    log_append("E");
}

#define MULTI_DEFER()          \
    do                         \
    {                          \
        defer log_append("A"); \
        defer log_append("B"); \
    } while (0)

void test_multi_defer_macro(void)
{
    log_reset();
    {
        MULTI_DEFER(); // defers run when do-while scope exits (immediately)
        log_append("1");
    }
    log_append("E");
    // Note: do{}while(0) creates a scope, so defers run when that scope exits
    // Result: BA (defers in LIFO), then 1, then E = "BA1E"
}

//=============================================================================
// SECTION 16: goto into block (valid but tricky)
//=============================================================================

void test_goto_into_block(void)
{
    log_reset();
    int x = 1;
    defer log_append("F");

    if (x)
        goto inside;

    {
        log_append("X"); // skipped
    inside:
        log_append("1");
    }
    log_append("E");
    // Expected: 1EF - the goto skips into the block
}

//=============================================================================
// SECTION 17: switch with default in middle
//=============================================================================

void test_switch_default_middle(void)
{
    log_reset();
    int x = 5; // No matching case

    switch (x)
    {
    case 1:
        log_append("1");
        break;
    default:
        defer log_append("D");
        log_append("X");
        break;
    case 2:
        log_append("2");
        break;
    }
    log_append("E");
    // Expected: XDE
}

//=============================================================================
// SECTION 18: Empty defer (just semicolon)
//=============================================================================

void test_defer_empty_stmt(void)
{
    log_reset();
    {
        defer; // Empty statement - should be valid
        log_append("1");
    }
    log_append("E");
    CHECK(strcmp(log_buffer, "1E") == 0, "empty defer statement");
}

//=============================================================================
// SECTION 19: Defer with function returning struct
//=============================================================================

struct Point
{
    int x, y;
};

struct Point make_point(int x, int y)
{
    return (struct Point){x, y};
}

void log_point(struct Point p)
{
    if (p.x == 0 && p.y == 0)
        log_append("O");
    else
        log_append("P");
}

void test_defer_struct_return(void)
{
    log_reset();
    {
        defer log_point(make_point(0, 0));
        log_append("1");
    }
    log_append("E");
}

//=============================================================================
// SECTION 20: Very long declaration (stress test)
//=============================================================================

void test_long_declaration(void)
{
    const volatile unsigned long long int *const *volatile ptr;
    CHECK(ptr == NULL, "long qualified declaration zero-init");
}

//=============================================================================
// SECTION 21: Array of function pointers
//=============================================================================

void test_func_ptr_array(void)
{
    int (*handlers[10])(int, int);
    int all_null = 1;
    for (int i = 0; i < 10; i++)
    {
        if (handlers[i] != NULL)
            all_null = 0;
    }
    CHECK(all_null, "function pointer array zero-init");
}

//=============================================================================
// SECTION 22: Pointer to array
//=============================================================================

void test_ptr_to_array(void)
{
    int (*p)[10]; // pointer to array of 10 ints
    CHECK(p == NULL, "pointer to array zero-init");
}

//=============================================================================
// SECTION 23: Complex return with defer
//=============================================================================

struct Point test_return_struct_defer(void)
{
    log_reset();
    defer log_append("D");
    log_append("1");
    return (struct Point){10, 20};
}

//=============================================================================
// SECTION 24: break inside nested if inside switch
//=============================================================================

void test_break_nested_if_switch(void)
{
    log_reset();
    int x = 1;
    switch (x)
    {
    case 1:
        defer log_append("A");
        if (1)
        {
            if (1)
            {
                log_append("1");
                break; // break from switch, through nested ifs
            }
        }
        log_append("X");
        break;
    }
    log_append("E");
}

//=============================================================================
// SECTION 25: do-while(0) pattern with defer
//=============================================================================

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

//=============================================================================
// Main test runner
//=============================================================================

int main(void)
{
    printf("=== Edge Case Tests ===\n\n");

    printf("--- Bitfields ---\n");
    test_bitfield_zeroinit();
    test_mixed_bitfield_regular();

    printf("\n--- Anonymous struct/union ---\n");
    test_anonymous_struct();
    test_anonymous_union();

    printf("\n--- Flexible array ---\n");
    test_flexible_array_ptr();

    printf("\n--- Compound literal + defer ---\n");
    test_defer_with_compound_literal();
    check_log("1DE", "defer_with_compound_literal");

    printf("\n--- Comma operator ---\n");
    test_comma_operator();
    test_comma_in_for();

    printf("\n--- sizeof ---\n");
    test_sizeof_with_vla();
    test_sizeof_regular();

    printf("\n--- _Alignas ---\n");
    test_alignas_zeroinit();

    printf("\n--- Duff's device ---\n");
    test_duffs_device_defer();
    check_log("XXXXXEF", "duffs_device_defer");

    printf("\n--- Multiple returns ---\n");
    test_multi_return_defer(1);
    check_log("1BA", "multi_return_defer(1)");
    test_multi_return_defer(2);
    check_log("2A", "multi_return_defer(2)");
    test_multi_return_defer(3);
    check_log("3CA", "multi_return_defer(3)");

    printf("\n--- Ternary + defer ---\n");
    test_defer_ternary_complex();
    check_log("1T", "defer_ternary_complex");

    printf("\n--- Tricky labels ---\n");
    test_tricky_labels();
    check_log("12D", "tricky_labels");

    printf("\n--- Nested ternary ---\n");
    test_nested_ternary();

#ifdef __GNUC__
    printf("\n--- GCC extensions ---\n");
    test_typeof_zeroinit();
    test_auto_type_init();
#endif

    printf("\n--- Macro defer ---\n");
    test_defer_via_macro();
    check_log("1ME", "defer_via_macro");

    test_multi_defer_macro();
    check_log("BA1E", "multi_defer_macro (do-while scope)"); // defers run at do-while exit

    printf("\n--- goto into block ---\n");
    test_goto_into_block();
    check_log("1EF", "goto_into_block");

    printf("\n--- switch default middle ---\n");
    test_switch_default_middle();
    check_log("XDE", "switch_default_middle");

    printf("\n--- Struct return + defer ---\n");
    struct Point p = test_return_struct_defer();
    check_log("1D", "return_struct_defer");
    CHECK(p.x == 10 && p.y == 20, "return_struct_value_preserved");

    printf("\n--- Complex declarations ---\n");
    test_long_declaration();
    test_func_ptr_array();
    test_ptr_to_array();

    printf("\n--- break nested if switch ---\n");
    test_break_nested_if_switch();
    check_log("1AE", "break_nested_if_switch");

    printf("\n--- do-while(0) defer ---\n");
    test_do_while_0_defer();
    check_log("1DEF", "do_while_0_defer");

    printf("\n=== Results: %d/%d tests passed ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}
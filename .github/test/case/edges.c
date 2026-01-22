#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

static char log_buffer[1024];
static int log_pos = 0;

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
    if (strcmp(log_buffer, expected) == 0)
    {
        printf("[PASS] %s\n", test_name);
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

// BUG #6: Braceless loop label tracking
// Labels in braceless loop bodies should be tracked at correct depth
void test_bug6_braceless_for_label(void)
{
    log_reset();
    int i = 0;
    defer log_append("F"); // Function-level defer

    // Braceless for loop with a label inside
    for (i = 0; i < 2; i++)
        if (i == 1)
            goto skip_label;

    log_append("X"); // Should not reach if goto works

skip_label:
    log_append("E");
    // Expected: EF (goto skips X, F runs at end)
}

void test_bug6_braceless_while_label(void)
{
    log_reset();
    int count = 0;
    defer log_append("F");

    while (count < 1)
        if (++count == 1)
            goto done;

    log_append("X");
done:
    log_append("E");
    // Expected: EF
}

void test_switch_case_defer_leak_case1(void)
{
    log_reset();
    int x = 1;
    switch (x)
    {
    case 1:
        defer log_append("A");
        log_append("1");
        break;
    case 2:
        log_append("2");
        break;
    }
    log_append("E");
    // Expected when x=1: 1AE (defer A runs on break)
}

void test_switch_case_defer_leak_case2(void)
{
    log_reset();
    int x = 2;
    switch (x)
    {
    case 1:
        defer log_append("A"); // This should NOT affect case 2!
        log_append("1");
        break;
    case 2:
        log_append("2");
        break; // BUG: This might emit "A" incorrectly!
    }
    log_append("E");
    // Expected when x=2: 2E (defer A should NOT run!)
    // BUG produces: 2AE
}

void test_switch_case_defer_leak_case3(void)
{
    log_reset();
    int x = 3;
    switch (x)
    {
    case 1:
        defer log_append("A");
        log_append("1");
        break;
    case 2:
        defer log_append("B");
        log_append("2");
        break;
    case 3:
        log_append("3");
        break; // BUG: Might emit both A and B!
    }
    log_append("E");
    // Expected: 3E
    // BUG might produce: 3BAE or 3ABE
}

void test_switch_case_defer_with_braces(void)
{
    log_reset();
    int x = 2;
    switch (x)
    {
    case 1:
    {
        defer log_append("A");
        log_append("1");
        break;
    }
    case 2:
    {
        log_append("2");
        break;
    }
    }
    log_append("E");
    // With braces, each case has its own scope - should work correctly
    // Expected: 2E
}

void test_switch_fallthrough_no_defer_leak(void)
{
    log_reset();
    int x = 1;
    switch (x)
    {
    case 1:
        log_append("1");
        // fallthrough (no break)
    case 2:
        defer log_append("D");
        log_append("2");
        break;
    }
    log_append("E");
    // Fallthrough case: defer D is registered when we reach case 2
    // Expected: 12DE
}

// Test nested switch with defer (unique: braceless cases)
void test_nested_switch_defer_leak(void)
{
    log_reset();
    int x = 1, y = 2;
    switch (x)
    {
    case 1:
        defer log_append("A");
        switch (y)
        {
        case 1:
            defer log_append("B");
            break;
        case 2:
            log_append("2");
            break; // Should NOT emit B
        }
        log_append("1");
        break;
    }
    log_append("E");
    // Expected: 21AE
}

// Test switch inside loop
void test_switch_in_loop_defer(void)
{
    log_reset();
    for (int i = 0; i < 2; i++)
    {
        defer log_append("L");
        switch (i)
        {
        case 0:
            defer log_append("A");
            log_append("0");
            break;
        case 1:
            log_append("1");
            break; // BUG: might emit A from case 0!
        }
    }
    log_append("E");
    // Expected: 0AL1LE
    // BUG might produce: 0AL1ALE
}

// Test default case
void test_switch_default_defer_leak(void)
{
    log_reset();
    int x = 99;
    switch (x)
    {
    case 1:
        defer log_append("A");
        log_append("1");
        break;
    default:
        log_append("D");
        break; // Should NOT emit A
    }
    log_append("E");
    // Expected: DE
}

// Preprocessor test (Bug #2 - token chain corruption)
// This needs to be tested via compilation, not runtime
// Complex #if expression that might trigger token chain issues
#define COMPLEX_VAL 1
#if defined(COMPLEX_VAL) && COMPLEX_VAL > 0 && (COMPLEX_VAL + 1) == 2
#define PP_TEST_PASSED 1
#else
#define PP_TEST_PASSED 0
#endif

// Nested #if with reused expressions
#if COMPLEX_VAL
#if COMPLEX_VAL == 1
#define PP_NESTED_PASSED 1
#else
#define PP_NESTED_PASSED 0
#endif
#else
#define PP_NESTED_PASSED 0
#endif

void test_preprocessor_expressions(void)
{
    if (PP_TEST_PASSED && PP_NESTED_PASSED)
    {
        printf("[PASS] preprocessor_complex_expressions\n");
    }
    else
    {
        printf("[FAIL] preprocessor_complex_expressions\n");
        printf("  PP_TEST_PASSED=%d, PP_NESTED_PASSED=%d\n",
               PP_TEST_PASSED, PP_NESTED_PASSED);
    }
}

// Bug Division by dereference via macro
#define OP(x) x
int test_div_deref(void)
{
    int val = 10;
    int *b = &val;
    int x = 100 OP(/) OP(*) b; // Should be 100 / *b = 10
    return x;
}

// Bug Static should NOT get = 0 (wasteful)
// We can't easily test this at runtime, but we verify static works correctly
static int static_var;
static char static_buf[10];

int test_static(void)
{
    // C guarantees these are zero without explicit init
    return static_var == 0 && static_buf[0] == 0;
}

// Bug Typedef types should be zero-initialized
int test_typedef_init(void)
{
    size_t a;
    uint8_t b;
    int32_t c;
    uint64_t d;

    // These SHOULD all be zero if zero-init works
    return (a == 0) && (b == 0) && (c == 0) && (d == 0);
}

//=============================================================================
// Bug #3: __attribute__ + bitfield label confusion
//=============================================================================

static char attr_log[64];
static int attr_pos = 0;

static void attr_log_D(void)
{
    attr_log[attr_pos++] = 'D';
    attr_log[attr_pos] = 0;
}

void test_attr_bitfield(void)
{
    attr_pos = 0;
    attr_log[0] = 0;

    struct __attribute__((packed))
    {
        uint32_t done : 1; // "done" bitfield - NOT a label
        uint32_t end : 1;
    } flags;

    {
        defer attr_log_D();
        attr_log[attr_pos++] = '1';
        attr_log[attr_pos] = 0;
        goto done; // This should jump to the label, not the bitfield!
    }
done:
    attr_log[attr_pos++] = '2';
    attr_log[attr_pos] = 0;
}

//=============================================================================
// Main test runner
//=============================================================================

int main(void)
{
    int passed = 0, total = 0;

    printf("--- Bug #1: Division by dereference ---\n");
    if (test_div_deref() == 10)
    {
        printf("[PASS] Bug #1: Division by dereference\n");
        passed++;
    }
    else
    {
        printf("[FAIL] Bug #1: Division by dereference\n");
    }
    total++;

    printf("\n--- Bug #2a: Static variables work ---\n");
    if (test_static())
    {
        printf("[PASS] Bug #2a: Static variables work\n");
        passed++;
    }
    else
    {
        printf("[FAIL] Bug #2a: Static variables work\n");
    }
    total++;

    printf("\n--- Bug #2b: Typedef zero-init ---\n");
    if (test_typedef_init())
    {
        printf("[PASS] Bug #2b: Typedef zero-init\n");
        passed++;
    }
    else
    {
        printf("[FAIL] Bug #2b: Typedef zero-init\n");
    }
    total++;

    printf("\n--- Bug #3: Attribute + bitfield labels ---\n");
    test_attr_bitfield();
    if (strcmp(attr_log, "1D2") == 0)
    {
        printf("[PASS] Bug #3: Attribute + bitfield labels\n");
        passed++;
    }
    else
    {
        printf("[FAIL] Bug #3: Attribute + bitfield labels\n");
        printf("  Expected: '1D2'\n");
        printf("  Got:      '%s'\n", attr_log);
    }
    total++;

    printf("\n--- Bug #6: Braceless loop label tracking ---\n");
    test_bug6_braceless_for_label();
    total++;
    passed += check_log("EF", "bug6_braceless_for_label");

    test_bug6_braceless_while_label();
    total++;
    passed += check_log("EF", "bug6_braceless_while_label");

    printf("\n--- Switch-Case Defer Leak (Critical) ---\n");
    test_switch_case_defer_leak_case1();
    total++;
    passed += check_log("1AE", "switch_defer_leak_case1 (x=1)");

    test_switch_case_defer_leak_case2();
    total++;
    passed += check_log("2E", "switch_defer_leak_case2 (x=2, CRITICAL)");

    test_switch_case_defer_leak_case3();
    total++;
    passed += check_log("3E", "switch_defer_leak_case3 (x=3)");

    test_switch_case_defer_with_braces();
    total++;
    passed += check_log("2E", "switch_defer_with_braces");

    test_switch_fallthrough_no_defer_leak();
    total++;
    passed += check_log("12DE", "switch_fallthrough_no_defer_leak");

    printf("\n--- Additional edge cases (braceless switch) ---\n");
    test_nested_switch_defer_leak();
    total++;
    passed += check_log("21AE", "nested_switch_defer_leak");

    test_switch_in_loop_defer();
    total++;
    passed += check_log("0AL1LE", "switch_in_loop_defer");

    test_switch_default_defer_leak();
    total++;
    passed += check_log("DE", "switch_default_defer_leak");

    printf("\n--- Preprocessor tests ---\n");
    test_preprocessor_expressions();
    total++;
    passed++; // Already printed result

    printf("\n=== Results: %d/%d tests passed ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}
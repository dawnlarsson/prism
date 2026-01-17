// Tests for goto, return, break, continue, and edge cases

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// =============================================================================
// Test 1: Basic defer (should already work)
// =============================================================================
void test_basic_defer(void)
{
    log_reset();
    {
        defer log_append("A");
        log_append("1");
    }
    // Expected order: 1, then A on scope exit
}

// =============================================================================
// Test 2: Multiple defers in LIFO order (should already work)
// =============================================================================
void test_lifo_order(void)
{
    log_reset();
    {
        defer log_append("C");
        defer log_append("B");
        defer log_append("A");
        log_append("1");
    }
    // Expected: 1ABC (LIFO order for defers)
}

// =============================================================================
// Test 3: Return with defer (should already work)
// =============================================================================
int test_return_defer(void)
{
    log_reset();
    defer log_append("A");
    log_append("1");
    return 42;
}

// =============================================================================
// Test 4: Goto forward - jumping OUT of a scope with defer
// THIS IS THE BUG - defer should execute before goto!
// =============================================================================
void test_goto_forward_out_of_scope(void)
{
    log_reset();
    {
        defer log_append("A");
        log_append("1");
        goto end;
        log_append("X"); // should not execute
    }
end:
    log_append("2");
    // Expected: 1A2 (defer A should run before jumping out)
    // Bug: Currently produces 12 (defer not run)
}

// =============================================================================
// Test 5: Goto forward - same scope level (no scope exit)
// =============================================================================
void test_goto_forward_same_scope(void)
{
    log_reset();
    defer log_append("A");
    log_append("1");
    goto skip;
    log_append("X"); // should not execute
skip:
    log_append("2");
    // Expected: 12A (defer runs at function end, not at goto)
}

// =============================================================================
// Test 6: Goto backward (loop simulation)
// =============================================================================
void test_goto_backward(void)
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
        goto again; // Should emit D before jumping back
    }
done:
    log_append("E");
    // Expected: LDLDE (each loop iteration's defer should run)
    // Bug: Currently produces LLE (defers not run on backward goto)
}

// =============================================================================
// Test 7: Goto out of multiple nested scopes
// =============================================================================
void test_goto_nested_scopes(void)
{
    log_reset();
    {
        defer log_append("A");
        {
            defer log_append("B");
            {
                defer log_append("C");
                log_append("1");
                goto end; // Should emit C, B, A in order
            }
        }
    }
end:
    log_append("2");
    // Expected: 1CBA2 (all nested defers in LIFO order)
    // Bug: Currently produces 12 (no defers run)
}

// =============================================================================
// Test 8: Break with defer (should already work)
// =============================================================================
void test_break_defer(void)
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
    // Expected: LDLDE
}

// =============================================================================
// Test 9: Continue with defer (should already work)
// =============================================================================
void test_continue_defer(void)
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
    // Expected: LDSDLDE
}

// =============================================================================
// Test 10: Goto in switch (jumping out of switch with defer)
// =============================================================================
void test_goto_in_switch(void)
{
    log_reset();
    int x = 1;
    switch (x)
    {
    case 1:
    {
        defer log_append("A");
        log_append("1");
        goto outside;
    }
    case 2:
        log_append("2");
        break;
    }
outside:
    log_append("E");
    // Expected: 1AE
    // Bug: Currently produces 1E
}

// =============================================================================
// Test 11: Nested loops with goto
// =============================================================================
void test_nested_loops_goto(void)
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
    // Expected: XIXIOE (inner defer, then jump out with both defers)
    // Bug: Currently produces XXIE or similar
}

// =============================================================================
// Test 12: Return in nested scope with multiple defers
// =============================================================================
int test_return_nested(void)
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
    // Expected: R321 (all defers in LIFO from innermost)
}

// =============================================================================
// Test 13: Goto across sibling scopes (exit one, don't enter another)
// =============================================================================
void test_goto_sibling_scopes(void)
{
    log_reset();
    {
        defer log_append("A");
        log_append("1");
        goto middle;
    }
middle:
{
    defer log_append("B");
    log_append("2");
}
    log_append("3");
    // Expected: 1A2B3 (A runs when exiting first block, B runs at end of second, then 3)
}

// =============================================================================
// Test 14: Multiple gotos in same function
// =============================================================================
void test_multiple_gotos(void)
{
    log_reset();
    {
        defer log_append("A");
        goto step1;
    }
step1:
    log_append("1");
    {
        defer log_append("B");
        goto step2;
    }
step2:
    log_append("2");
    // Expected: A1B2
}

// =============================================================================
// Test 15: Goto in deeply nested loop (function-level defer runs at return)
// =============================================================================
void test_goto_deep_loop(void)
{
    log_reset();
    defer log_append("F");
    for (int i = 0; i < 1; i++)
    {
        defer log_append("O");
        for (int j = 0; j < 1; j++)
        {
            defer log_append("I");
            log_append("X");
            goto out;
        }
    }
out:
    log_append("E");
    // Expected: XIOEF (inner/outer loop defers on goto, F at implicit function return)
}

// =============================================================================
// Test 16: Conditional goto with defer
// =============================================================================
void test_conditional_goto(void)
{
    log_reset();
    int x = 1;
    {
        defer log_append("A");
        if (x)
        {
            defer log_append("B");
            log_append("1");
            goto done;
        }
        log_append("X");
    }
done:
    log_append("2");
    // Expected: 1BA2 (both defers run when jumping out)
}

// =============================================================================
// Test 17: Goto jumping over defer registration
// NOTE: This pattern is now a compile-time error. Goto skipping over a defer
// is a potential bug (resource leak) and prism rejects it.
// =============================================================================
// void test_goto_over_defer(void) - REMOVED (compile error expected)

// =============================================================================
// Test 18: Multiple defers with only some skippable
// NOTE: This pattern is now a compile-time error (same reason as test 17)
// =============================================================================
// void test_partial_skip(void) - REMOVED (compile error expected)

// =============================================================================
// Test 17: Goto backward doesn't skip forward defer
// =============================================================================
void test_goto_backward_no_skip(void)
{
    log_reset();
    int i = 0;
again:
    if (i >= 2)
        goto done;
    {
        log_append("L");
        defer log_append("D"); // Always runs (goto is after, not before)
        i++;
        goto again;
    }
done:
    log_append("E");
    // Expected: LDLDE (defers always run because goto doesn't skip them)
}

// =============================================================================
// Main - run all tests
// =============================================================================
int main(void)
{
    int passed = 0;
    int total = 0;

    printf("=== Defer Test Suite ===\n\n");

    // Test 1: Basic defer
    test_basic_defer();
    total++;
    passed += check_log("1A", "test_basic_defer");

    // Test 2: LIFO order
    test_lifo_order();
    total++;
    passed += check_log("1ABC", "test_lifo_order");

    // Test 3: Return with defer
    test_return_defer();
    total++;
    passed += check_log("1A", "test_return_defer");

    // Test 4: Goto forward out of scope (THE BUG)
    test_goto_forward_out_of_scope();
    total++;
    passed += check_log("1A2", "test_goto_forward_out_of_scope");

    // Test 5: Goto forward same scope
    test_goto_forward_same_scope();
    total++;
    passed += check_log("12A", "test_goto_forward_same_scope");

    // Test 6: Goto backward
    test_goto_backward();
    total++;
    passed += check_log("LDLDE", "test_goto_backward");

    // Test 7: Goto nested scopes
    test_goto_nested_scopes();
    total++;
    passed += check_log("1CBA2", "test_goto_nested_scopes");

    // Test 8: Break with defer
    test_break_defer();
    total++;
    passed += check_log("LDLDE", "test_break_defer");

    // Test 9: Continue with defer
    test_continue_defer();
    total++;
    passed += check_log("LDSDLDE", "test_continue_defer");

    // Test 10: Goto in switch
    test_goto_in_switch();
    total++;
    passed += check_log("1AE", "test_goto_in_switch");

    // Test 11: Nested loops with goto
    test_nested_loops_goto();
    total++;
    passed += check_log("XIXIOE", "test_nested_loops_goto");

    // Test 12: Return in nested scope
    test_return_nested();
    total++;
    passed += check_log("R321", "test_return_nested");

    // Test 13: Goto across sibling scopes
    test_goto_sibling_scopes();
    total++;
    passed += check_log("1A2B3", "test_goto_sibling_scopes");

    // Test 14: Multiple gotos
    test_multiple_gotos();
    total++;
    passed += check_log("A1B2", "test_multiple_gotos");

    // Test 15: Goto in deep loop
    test_goto_deep_loop();
    total++;
    passed += check_log("XIOEF", "test_goto_deep_loop");

    // Test 16: Conditional goto
    test_conditional_goto();
    total++;
    passed += check_log("1BA2", "test_conditional_goto");

    // Tests 17-18: Goto skipping over defer is now a compile-time error
    // (removed - these patterns are rejected by prism)

    // Test 17: Goto backward doesn't skip forward defer
    test_goto_backward_no_skip();
    total++;
    passed += check_log("LDLDE", "test_goto_backward_no_skip");

    printf("\n=== Results: %d/%d tests passed ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}

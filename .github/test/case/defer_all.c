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
// Test 17: Normal while loop with defer
// =============================================================================
void test_while_loop_defer(void)
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
    // Expected: LDLDLDE
}

// =============================================================================
// Test 18: do-while loop with defer
// =============================================================================
void test_do_while_defer(void)
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
    // Expected: LDLDLDE
}

// =============================================================================
// Test 19: while(1) with break and defer
// =============================================================================
void test_while_break_defer(void)
{
    log_reset();
    int i = 0;
    while (1)
    {
        defer log_append("D");
        log_append("L");
        i++;
        if (i >= 2)
            break;
    }
    log_append("E");
    // Expected: LDLDE
}

// =============================================================================
// Test 20: Nested switch with defer
// =============================================================================
void test_nested_switch_defer(void)
{
    log_reset();
    int x = 1, y = 2;
    switch (x)
    {
    case 1:
    {
        defer log_append("A");
        switch (y)
        {
        case 2:
        {
            defer log_append("B");
            log_append("1");
            break;
        }
        }
        log_append("2");
        break;
    }
    }
    log_append("E");
    // Expected: 1B2AE
}

// =============================================================================
// Test 21: Return inside switch case with defer
// =============================================================================
int test_return_in_switch(void)
{
    log_reset();
    defer log_append("F");
    int x = 1;
    switch (x)
    {
    case 1:
    {
        defer log_append("A");
        log_append("1");
        return 42;
    }
    case 2:
        log_append("2");
        break;
    }
    log_append("E");
    return 0;
    // Expected: 1AF (defer A then F, return 42)
}

// =============================================================================
// Test 22: Break from nested switch inside loop
// =============================================================================
void test_break_nested_switch_in_loop(void)
{
    log_reset();
    for (int i = 0; i < 2; i++)
    {
        defer log_append("L");
        switch (i)
        {
        case 0:
        {
            defer log_append("A");
            log_append("0");
            break; // breaks switch, not loop
        }
        case 1:
        {
            defer log_append("B");
            log_append("1");
            break;
        }
        }
        log_append("X");
    }
    log_append("E");
    // Expected: 0AXLB1XLE (note: B before 1X due to switch break, wait no...)
    // Actually: case 0: log "0", defer A, break switch -> "A", log "X", defer L -> "L"
    //          case 1: log "1", defer B, break switch -> "B", log "X", defer L -> "L"
    // Expected: 0AXL1BXLE
}

// =============================================================================
// Test 23: Continue from inside switch inside loop
// =============================================================================
void test_continue_in_switch_in_loop(void)
{
    log_reset();
    for (int i = 0; i < 3; i++)
    {
        defer log_append("L");
        switch (i)
        {
        case 1:
        {
            defer log_append("S");
            log_append("1");
            continue; // continue the for loop
        }
        default:
            log_append("D");
            break;
        }
        log_append("X");
    }
    log_append("E");
    // i=0: D, X, L
    // i=1: 1, S, L (continue skips X)
    // i=2: D, X, L
    // Expected: DXLSL1DXLE - wait, order is wrong
    // Actually: i=0: "D", "X", defer "L" -> DXL
    //          i=1: "1", defer "S", continue triggers S then L -> 1SL
    //          i=2: "D", "X", defer "L" -> DXL
    // Expected: DXL1SLDXLE
}

// =============================================================================
// Test 24: Defer in if without else
// =============================================================================
void test_defer_in_if_only(void)
{
    log_reset();
    int x = 1;
    {
        defer log_append("O");
        if (x)
        {
            defer log_append("I");
            log_append("1");
        }
        log_append("2");
    }
    log_append("E");
    // Expected: 1I2OE
}

// =============================================================================
// Test 25: Defer in else only
// =============================================================================
void test_defer_in_else_only(void)
{
    log_reset();
    int x = 0;
    {
        defer log_append("O");
        if (x)
        {
            log_append("T");
        }
        else
        {
            defer log_append("E");
            log_append("F");
        }
        log_append("2");
    }
    log_append("X");
    // Expected: FE2OX
}

// =============================================================================
// Test 26: Chained gotos
// =============================================================================
void test_chained_gotos(void)
{
    log_reset();
    {
        defer log_append("A");
        goto step1;
    }
step1:
{
    defer log_append("B");
    goto step2;
}
step2:
{
    defer log_append("C");
    goto step3;
}
step3:
    log_append("E");
    // Expected: ABCE
}

// =============================================================================
// Test 27: Goto to label immediately after (effectively no-op)
// =============================================================================
void test_goto_noop(void)
{
    log_reset();
    {
        defer log_append("A");
        log_append("1");
        goto here;
    here:
        log_append("2");
    }
    log_append("E");
    // Expected: 12AE (goto doesn't exit scope, defer runs at block end)
}

// =============================================================================
// Test 28: Label at very end of function
// =============================================================================
void test_label_at_end(void)
{
    log_reset();
    defer log_append("F");
    {
        defer log_append("A");
        log_append("1");
        goto end;
    }
end:;
    // Expected: 1AF (A runs on goto out, F runs at function end)
}

// =============================================================================
// Test 29: Multiple returns with different defer states
// =============================================================================
int test_multiple_returns(int path)
{
    log_reset();
    defer log_append("F");
    if (path == 1)
    {
        defer log_append("A");
        log_append("1");
        return 1;
    }
    if (path == 2)
    {
        defer log_append("B");
        log_append("2");
        return 2;
    }
    log_append("3");
    return 3;
    // path=1: 1AF, path=2: 2BF, path=3: 3F
}

// =============================================================================
// Test 30: Deeply nested breaks
// =============================================================================
void test_deeply_nested_break(void)
{
    log_reset();
    for (int i = 0; i < 1; i++)
    {
        defer log_append("1");
        for (int j = 0; j < 1; j++)
        {
            defer log_append("2");
            for (int k = 0; k < 1; k++)
            {
                defer log_append("3");
                log_append("X");
                break;
            }
            log_append("Y");
            break;
        }
        log_append("Z");
        break;
    }
    log_append("E");
    // X, defer 3, Y, defer 2, Z, defer 1
    // Expected: X3Y2Z1E
}

// =============================================================================
// Test 31: Mix of break and continue in same iteration
// =============================================================================
void test_break_continue_same_loop(void)
{
    log_reset();
    for (int i = 0; i < 5; i++)
    {
        defer log_append("D");
        if (i == 1)
        {
            log_append("C");
            continue;
        }
        if (i == 3)
        {
            log_append("B");
            break;
        }
        log_append("L");
    }
    log_append("E");
    // i=0: L, D
    // i=1: C, D (continue)
    // i=2: L, D
    // i=3: B, D (break)
    // Expected: LDCDLDBDE
}

// =============================================================================
// Test 32: for(;;) infinite loop with break
// =============================================================================
void test_for_infinite_break(void)
{
    log_reset();
    int i = 0;
    for (;;)
    {
        defer log_append("D");
        log_append("L");
        i++;
        if (i >= 2)
            break;
    }
    log_append("E");
    // Expected: LDLDE
}

// =============================================================================
// Test 33: Defer with compound statement
// =============================================================================
void test_defer_compound(void)
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
    // Expected: 1ABE
}

// =============================================================================
// Test 34: Empty scope with defer
// =============================================================================
void test_empty_scope_defer(void)
{
    log_reset();
    log_append("1");
    {
        defer log_append("A");
    }
    log_append("2");
    // Expected: 1A2
}

// =============================================================================
// Test 35: Defer with break in nested loops (break inner, stay in outer)
// =============================================================================
void test_break_inner_stay_outer(void)
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
    // i=0: X, I, X, I (break), Y, O
    // i=1: X, I, X, I (break), Y, O
    // Expected: XIXIYOXIXIYOE
}

// =============================================================================
// Test 36: Switch fallthrough with defer in each case
// =============================================================================
void test_switch_fallthrough_defer(void)
{
    log_reset();
    int x = 0;
    switch (x)
    {
    case 0:
    {
        defer log_append("A");
        log_append("0");
    }
    // fallthrough
    case 1:
    {
        defer log_append("B");
        log_append("1");
    }
    // fallthrough
    case 2:
    {
        defer log_append("C");
        log_append("2");
        break;
    }
    }
    log_append("E");
    // case 0 block ends -> A, then falls to case 1
    // case 1 block ends -> B, then falls to case 2
    // case 2 block ends -> C, then break
    // Expected: 0A1B2CE
}

// =============================================================================
// Test 37: Goto into switch from outside (label in case)
// =============================================================================
void test_goto_into_switch(void)
{
    log_reset();
    int x = 1;
    if (x)
    {
        goto inside;
    }
    switch (1)
    {
    case 1:
    inside:
    {
        defer log_append("A");
        log_append("1");
        break;
    }
    }
    log_append("E");
    // x=1, so we goto inside, skipping switch dispatch
    // Expected: 1AE
}

// =============================================================================
// Test 38: Nested defer with same cleanup action
// =============================================================================
void test_nested_same_defer(void)
{
    log_reset();
    {
        defer log_append("X");
        {
            defer log_append("X");
            {
                defer log_append("X");
                log_append("1");
            }
        }
    }
    log_append("E");
    // Expected: 1XXXE
}

// =============================================================================
// Test 39: Defer in loop condition scope
// =============================================================================
void test_defer_loop_body_only(void)
{
    log_reset();
    for (int i = 0; i < 2; i++)
    {
        log_append("L");
        {
            defer log_append("D");
            log_append("I");
        }
    }
    log_append("E");
    // Expected: LIDLIDE
}

// =============================================================================
// Test 40: Return value depends on deferred side effect
// =============================================================================
int global_val = 0;
int test_return_side_effect(void)
{
    global_val = 0;
    defer global_val = 100; // This runs AFTER return value is captured
    return global_val;      // Returns 0, but global_val becomes 100 after
}

// =============================================================================
// Test 41: Goto out of deeply nested if/else
// =============================================================================
void test_goto_nested_if_else(void)
{
    log_reset();
    int a = 1, b = 1, c = 1;
    if (a)
    {
        defer log_append("A");
        if (b)
        {
            defer log_append("B");
            if (c)
            {
                defer log_append("C");
                log_append("1");
                goto out;
            }
        }
    }
out:
    log_append("E");
    // Expected: 1CBAE
}

// =============================================================================
// Test 42: Defer with ternary operator in statement
// =============================================================================
void test_defer_ternary(void)
{
    log_reset();
    int x = 1;
    {
        defer log_append(x ? "T" : "F");
        log_append("1");
    }
    log_append("E");
    // Expected: 1TE
}

// =============================================================================
// Test 43: Break out of inner loop, continue outer
// =============================================================================
void test_break_inner_continue_outer(void)
{
    log_reset();
    for (int i = 0; i < 2; i++)
    {
        defer log_append("O");
        for (int j = 0; j < 3; j++)
        {
            defer log_append("I");
            log_append("X");
            break; // only runs once per outer iteration
        }
        if (i == 0)
            continue;
        log_append("Y");
    }
    log_append("E");
    // i=0: X, I (break inner), continue -> O
    // i=1: X, I (break inner), Y, O (loop ends)
    // Expected: XIOXIYOE
}

// =============================================================================
// Test 44: Switch with no matching case (default case with defer)
// =============================================================================
void test_switch_no_match_default_defer(void)
{
    log_reset();
    int x = 99; // No case matches
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
        defer log_append("B");
        log_append("2");
        break;
    }
    default:
    {
        defer log_append("D");
        log_append("X");
        break;
    }
    }
    log_append("E");
    // Expected: XDE
}

// =============================================================================
// Test 45: Nested function calls with defer (each has own scope)
// =============================================================================
void helper_with_defer(void)
{
    defer log_append("H");
    log_append("h");
}

void test_nested_function_defer(void)
{
    log_reset();
    defer log_append("M");
    log_append("1");
    helper_with_defer();
    log_append("2");
    // Expected: 1hH2M
}

// =============================================================================
// Test 46: Defer with function call
// =============================================================================
void log_X(void) { log_append("X"); }

void test_defer_function_call(void)
{
    log_reset();
    {
        defer log_X();
        log_append("1");
    }
    log_append("E");
    // Expected: 1XE
}

// =============================================================================
// Test 47: Multiple scopes at same level
// =============================================================================
void test_multiple_scopes_same_level(void)
{
    log_reset();
    {
        defer log_append("A");
        log_append("1");
    }
    {
        defer log_append("B");
        log_append("2");
    }
    {
        defer log_append("C");
        log_append("3");
    }
    log_append("E");
    // Expected: 1A2B3CE
}

// =============================================================================
// Test 48: Goto from one case to another in switch
// =============================================================================
void test_goto_between_cases(void)
{
    log_reset();
    int x = 1;
    switch (x)
    {
    case 1:
    {
        defer log_append("A");
        log_append("1");
        goto case2;
    }
    case 2:
    case2:
    {
        defer log_append("B");
        log_append("2");
        break;
    }
    }
    log_append("E");
    // case 1: "1", goto case2 -> defer A runs
    // at case2: "2", break -> defer B runs
    // Expected: 1A2BE
}

// =============================================================================
// Test 49: Very deep nesting (stress test)
// =============================================================================
void test_very_deep_nesting(void)
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
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    log_append("E");
    // Expected: X87654321E
}

// =============================================================================
// Test 50: Return void function with defer
// =============================================================================
void test_return_void(void)
{
    log_reset();
    defer log_append("A");
    log_append("1");
    if (1)
    {
        defer log_append("B");
        log_append("2");
        return;
    }
    log_append("X");
}

// =============================================================================
// Test 51: Defer in all branches of if/else if/else
// =============================================================================
void test_defer_all_branches(int path)
{
    log_reset();
    defer log_append("O");
    if (path == 1)
    {
        defer log_append("A");
        log_append("1");
    }
    else if (path == 2)
    {
        defer log_append("B");
        log_append("2");
    }
    else
    {
        defer log_append("C");
        log_append("3");
    }
    log_append("E");
}

// =============================================================================
// Test 52: Break vs goto out of loop (same behavior expected)
// =============================================================================
void test_break_vs_goto_out(void)
{
    log_reset();

    // Using break
    for (int i = 0; i < 5; i++)
    {
        defer log_append("A");
        log_append("1");
        break;
    }

    // Using goto
    for (int i = 0; i < 5; i++)
    {
        defer log_append("B");
        log_append("2");
        goto out;
    }
out:
    log_append("E");
    // Expected: 1A2BE
}

// =============================================================================
// Test 53: Defer with pointer dereference
// =============================================================================
void test_defer_pointer(void)
{
    log_reset();
    const char *p = "P";
    {
        defer log_append(p);
        log_append("1");
    }
    log_append("E");
    // Expected: 1PE
}

// =============================================================================
// Test 54: Defer with array index
// =============================================================================
void test_defer_array(void)
{
    log_reset();
    const char *arr[] = {"A", "B", "C"};
    {
        defer log_append(arr[1]);
        log_append("1");
    }
    log_append("E");
    // Expected: 1BE
}

// =============================================================================
// Test 55: Complex expression in defer
// =============================================================================
void test_defer_complex_expr(void)
{
    log_reset();
    int x = 1;
    {
        defer log_append(x > 0 ? (x > 1 ? "B" : "A") : "C");
        log_append("1");
    }
    log_append("E");
    // x=1, so x>0 is true, x>1 is false, result is "A"
    // Expected: 1AE
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

    // Test 17: Normal while loop with defer
    test_while_loop_defer();
    total++;
    passed += check_log("LDLDLDE", "test_while_loop_defer");

    // Test 18: do-while loop with defer
    test_do_while_defer();
    total++;
    passed += check_log("LDLDLDE", "test_do_while_defer");

    // Test 19: while(1) with break
    test_while_break_defer();
    total++;
    passed += check_log("LDLDE", "test_while_break_defer");

    // Test 20: Nested switch with defer
    test_nested_switch_defer();
    total++;
    passed += check_log("1B2AE", "test_nested_switch_defer");

    // Test 21: Return inside switch
    test_return_in_switch();
    total++;
    passed += check_log("1AF", "test_return_in_switch");

    // Test 22: Break from nested switch in loop
    test_break_nested_switch_in_loop();
    total++;
    passed += check_log("0AXL1BXLE", "test_break_nested_switch_in_loop");

    // Test 23: Continue in switch in loop
    test_continue_in_switch_in_loop();
    total++;
    passed += check_log("DXL1SLDXLE", "test_continue_in_switch_in_loop");

    // Test 24: Defer in if only
    test_defer_in_if_only();
    total++;
    passed += check_log("1I2OE", "test_defer_in_if_only");

    // Test 25: Defer in else only
    test_defer_in_else_only();
    total++;
    passed += check_log("FE2OX", "test_defer_in_else_only");

    // Test 26: Chained gotos
    test_chained_gotos();
    total++;
    passed += check_log("ABCE", "test_chained_gotos");

    // Test 27: Goto noop
    test_goto_noop();
    total++;
    passed += check_log("12AE", "test_goto_noop");

    // Test 28: Label at end
    test_label_at_end();
    total++;
    passed += check_log("1AF", "test_label_at_end");

    // Test 29: Multiple returns (test each path)
    test_multiple_returns(1);
    total++;
    passed += check_log("1AF", "test_multiple_returns(1)");
    test_multiple_returns(2);
    total++;
    passed += check_log("2BF", "test_multiple_returns(2)");
    test_multiple_returns(3);
    total++;
    passed += check_log("3F", "test_multiple_returns(3)");

    // Test 30: Deeply nested breaks
    test_deeply_nested_break();
    total++;
    passed += check_log("X3Y2Z1E", "test_deeply_nested_break");

    // Test 31: Break and continue same loop
    test_break_continue_same_loop();
    total++;
    passed += check_log("LDCDLDBDE", "test_break_continue_same_loop");

    // Test 32: for(;;) with break
    test_for_infinite_break();
    total++;
    passed += check_log("LDLDE", "test_for_infinite_break");

    // Test 33: Defer with compound statement
    test_defer_compound();
    total++;
    passed += check_log("1ABE", "test_defer_compound");

    // Test 34: Empty scope with defer
    test_empty_scope_defer();
    total++;
    passed += check_log("1A2", "test_empty_scope_defer");

    // Test 35: Break inner loop, stay in outer
    test_break_inner_stay_outer();
    total++;
    passed += check_log("XIXIYOXIXIYOE", "test_break_inner_stay_outer");

    // Test 36: Switch fallthrough with defer
    test_switch_fallthrough_defer();
    total++;
    passed += check_log("0A1B2CE", "test_switch_fallthrough_defer");

    // Test 37: Goto into switch
    test_goto_into_switch();
    total++;
    passed += check_log("1AE", "test_goto_into_switch");

    // Test 38: Nested same defer
    test_nested_same_defer();
    total++;
    passed += check_log("1XXXE", "test_nested_same_defer");

    // Test 39: Defer in loop body only
    test_defer_loop_body_only();
    total++;
    passed += check_log("LIDLIDE", "test_defer_loop_body_only");

    // Test 40: Return side effect
    int ret40 = test_return_side_effect();
    total++;
    if (ret40 == 0 && global_val == 100)
    {
        printf("[PASS] test_return_side_effect\n");
        passed++;
    }
    else
    {
        printf("[FAIL] test_return_side_effect\n");
        printf("  Expected: ret=0, global_val=100\n");
        printf("  Got:      ret=%d, global_val=%d\n", ret40, global_val);
    }

    // Test 41: Goto nested if/else
    test_goto_nested_if_else();
    total++;
    passed += check_log("1CBAE", "test_goto_nested_if_else");

    // Test 42: Defer with ternary
    test_defer_ternary();
    total++;
    passed += check_log("1TE", "test_defer_ternary");

    // Test 43: Break inner continue outer
    test_break_inner_continue_outer();
    total++;
    passed += check_log("XIOXIYOE", "test_break_inner_continue_outer");

    // Test 44: Switch with no matching case (uses default with defer)
    test_switch_no_match_default_defer();
    total++;
    passed += check_log("XDE", "test_switch_no_match_default_defer");

    // Test 45: Nested function defer
    test_nested_function_defer();
    total++;
    passed += check_log("1hH2M", "test_nested_function_defer");

    // Test 46: Defer function call
    test_defer_function_call();
    total++;
    passed += check_log("1XE", "test_defer_function_call");

    // Test 47: Multiple scopes same level
    test_multiple_scopes_same_level();
    total++;
    passed += check_log("1A2B3CE", "test_multiple_scopes_same_level");

    // Test 48: Goto between cases
    test_goto_between_cases();
    total++;
    passed += check_log("1A2BE", "test_goto_between_cases");

    // Test 49: Very deep nesting
    test_very_deep_nesting();
    total++;
    passed += check_log("X87654321E", "test_very_deep_nesting");

    // Test 50: Return void
    test_return_void();
    total++;
    passed += check_log("12BA", "test_return_void");

    // Test 51: Defer in all branches
    test_defer_all_branches(1);
    total++;
    passed += check_log("1AEO", "test_defer_all_branches(1)");
    test_defer_all_branches(2);
    total++;
    passed += check_log("2BEO", "test_defer_all_branches(2)");
    test_defer_all_branches(3);
    total++;
    passed += check_log("3CEO", "test_defer_all_branches(3)");

    // Test 52: Break vs goto
    test_break_vs_goto_out();
    total++;
    passed += check_log("1A2BE", "test_break_vs_goto_out");

    // Test 53: Defer with pointer
    test_defer_pointer();
    total++;
    passed += check_log("1PE", "test_defer_pointer");

    // Test 54: Defer with array
    test_defer_array();
    total++;
    passed += check_log("1BE", "test_defer_array");

    // Test 55: Defer complex expr
    test_defer_complex_expr();
    total++;
    passed += check_log("1AE", "test_defer_complex_expr");

    printf("\n=== Results: %d/%d tests passed ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}
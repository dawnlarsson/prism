#include <stdio.h>
#include <stdlib.h>

// Test: Multiple defers execute in LIFO order
void test_lifo_order(void)
{
    printf("=== Test: LIFO order ===\n");
    printf("Expected: 3 2 1\nActual:   ");
    {
        defer printf("1\n");
        defer printf("2 ");
        defer printf("3 ");
    }
}

// Test: Nested scopes with defer
void test_nested_scopes(void)
{
    printf("=== Test: Nested scopes ===\n");
    defer printf("outer-end\n");
    printf("outer-start\n");

    {
        defer printf("inner-end\n");
        printf("inner-start\n");
    }

    printf("back-to-outer\n");
    // Expected: outer-start, inner-start, inner-end, back-to-outer, outer-end
}

// Test: Return value captured BEFORE defer runs
int test_return_ordering_helper(void)
{
    int x = 42;
    defer printf("defer runs after x captured\n");
    return x + 1; // Should capture 43 BEFORE defer runs
}

void test_return_ordering(void)
{
    printf("=== Test: Return ordering ===\n");
    int result = test_return_ordering_helper();
    printf("result = %d (should be 43)\n", result);
}

// Test: Defer executes before return
int test_return_helper(void)
{
    defer printf("Cleanup done\n");
    printf("Before return\n");
    return 42;
}

void test_return(void)
{
    printf("=== Test: Defer with return ===\n");
    int result = test_return_helper();
    printf("Result: %d (expected 42)\n", result);
}

// Test: Void return with defer
void test_void_return(void)
{
    printf("=== Test: Void return ===\n");
    defer printf("void defer\n");
    printf("before return\n");
    return;
}

// Test: Early return with defer cleanup
int test_early_return_helper(int should_fail)
{
    int *data = malloc(100);
    defer free(data);
    defer printf("Cleanup: freeing data\n");

    if (should_fail)
    {
        printf("Early return due to failure\n");
        return -1;
    }

    printf("Processing succeeded\n");
    return 0;
}

void test_early_return(void)
{
    printf("=== Test: Early return ===\n");
    printf("--- With failure ---\n");
    test_early_return_helper(1);
    printf("--- Without failure ---\n");
    test_early_return_helper(0);
}

// Test: Break with defer
void test_break(void)
{
    printf("=== Test: Break with defer ===\n");
    for (int i = 0; i < 5; i++)
    {
        defer printf("loop defer %d\n", i);
        if (i == 2)
        {
            printf("breaking at %d\n", i);
            break; // Should emit defer first
        }
        printf("iteration %d\n", i);
    }
    printf("after loop\n");
}

// Test: Continue with defer
void test_continue(void)
{
    printf("=== Test: Continue with defer ===\n");
    for (int i = 0; i < 4; i++)
    {
        defer printf("loop defer %d\n", i);
        if (i == 1)
        {
            printf("continuing at %d\n", i);
            continue; // Should emit defer first
        }
        printf("iteration %d\n", i);
    }
}

// Test: Nested scopes with break
void test_nested_break(void)
{
    printf("=== Test: Nested break ===\n");
    for (int i = 0; i < 3; i++)
    {
        defer printf("outer defer %d\n", i);
        {
            defer printf("inner defer %d\n", i);
            if (i == 1)
            {
                printf("breaking at %d\n", i);
                break;
            }
        }
    }
}

// Test: switch with break - use explicit blocks for defer
void test_switch_helper(int val)
{
    switch (val)
    {
    case 1:
    {
        defer printf("case 1 defer\n");
        printf("in case 1\n");
        break; // Should emit defer
    }
    case 2:
    {
        printf("in case 2\n");
        // fallthrough (close block first)
    }
    case 3:
    {
        defer printf("case 3 defer\n");
        printf("in case 3\n");
        break; // Should emit defer
    }
    default:
        printf("default\n");
    }
    printf("after switch\n");
}

void test_switch(void)
{
    printf("=== Test: Switch (val=1) ===\n");
    test_switch_helper(1);
    printf("\n=== Test: Switch (val=2) ===\n");
    test_switch_helper(2);
    printf("\n=== Test: Switch (val=3) ===\n");
    test_switch_helper(3);
}

// Test: Memory cleanup with defer
void test_memory_cleanup(void)
{
    printf("=== Test: Memory cleanup ===\n");
    int *ptr = malloc(sizeof(int) * 10);
    if (!ptr)
        return;
    defer free(ptr);

    ptr[0] = 100;
    ptr[9] = 999;
    printf("ptr[0] = %d, ptr[9] = %d\n", ptr[0], ptr[9]);
    printf("Memory will be freed by defer\n");
}

// Test: File handling with defer
void test_file_handling(void)
{
    printf("=== Test: File handling ===\n");
    FILE *f = fopen("/tmp/prism_defer_test.txt", "w");
    if (!f)
    {
        printf("Could not create test file\n");
        return;
    }
    defer fclose(f);

    fprintf(f, "Hello from prism!\n");
    printf("Wrote to file, defer will close it\n");
}

// Test: Cleanup function with defer
void cleanup(void *p)
{
    printf("cleanup(%p)\n", p);
    free(p);
}

void test_cleanup_function(void)
{
    printf("=== Test: Cleanup function ===\n");
    int *a = malloc(sizeof(int));
    int *b = malloc(sizeof(int));

    defer cleanup(a);
    defer cleanup(b);

    *a = 10;
    *b = 20;
    printf("a=%d, b=%d\n", *a, *b);
}

// Test: Defer with complex expressions
void test_complex_expressions(void)
{
    printf("=== Test: Complex expressions ===\n");
    // Defer with nested parentheses
    defer printf("Value: %d\n", (1 + 2) * 3);
    printf("About to exit scope...\n");
}

// Test: Defer in comments should NOT be processed
void test_defer_in_comments(void)
{
    printf("=== Test: Defer in comments ===\n");

    // defer printf("This is in a line comment\n");

    /* defer printf("This is in a block comment\n"); */

    /*
     * defer printf("Multi-line block comment\n");
     */

    defer printf("This is a REAL defer\n");
    printf("Only 'This is a REAL defer' should print after this\n");
}

// Test: Defer keyword in strings should NOT be processed
void test_defer_in_strings(void)
{
    printf("=== Test: Defer in strings ===\n");

    // These should print literally, not be treated as defer statements
    printf("The word defer appears here\n");
    printf("defer is a keyword\n");

    char *str = "defer should not trigger";
    printf("%s\n", str);

    defer printf("This IS a real defer\n");
    printf("End of test\n");
}

int main(void)
{
    // Section 1: Basic functionality
    test_lifo_order();
    printf("\n");

    test_nested_scopes();
    printf("\n");

    // Section 2: Return with defer
    test_return_ordering();
    printf("\n");

    test_return();
    printf("\n");

    test_void_return();
    printf("\n");

    test_early_return();
    printf("\n");

    // Section 3: Loops
    test_break();
    printf("\n");

    test_continue();
    printf("\n");

    test_nested_break();
    printf("\n");

    // Section 4: Switch
    test_switch();
    printf("\n");

    // Section 5: Resource management
    test_memory_cleanup();
    printf("\n");

    test_file_handling();
    printf("\n");

    test_cleanup_function();
    printf("\n");

    // Section 6: Complex expressions
    test_complex_expressions();
    printf("\n");

    // Section 7: Edge cases
    test_defer_in_comments();
    printf("\n");

    test_defer_in_strings();
    printf("\n");

    printf("=== All defer tests completed ===\n");
    return 0;
}

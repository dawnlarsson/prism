// Test: Defer executes before return

#include <stdio.h>

int test_return()
{
    defer printf("Cleanup done\n");
    printf("Before return\n");
    return 42;
}

int main()
{
    printf("Test: Defer with return\n");
    int result = test_return();
    printf("Result: %d\n", result);
    printf("Expected: Before return, Cleanup done, Result: 42\n");
    return 0;
}

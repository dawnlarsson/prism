// Test: Basic defer functionality
// Defer should execute when leaving scope

#include <stdio.h>

int main()
{
    printf("Test: Basic defer\n");

    {
        defer printf("3\n");
        defer printf("2\n");
        defer printf("1\n");
        printf("Start: ");
    }

    printf("Expected: Start: 1 2 3 (LIFO order)\n");
    return 0;
}

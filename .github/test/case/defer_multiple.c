// Test: Multiple defers in same scope (LIFO order)

#include <stdio.h>

int main()
{
    printf("Test: Multiple defers (LIFO order)\n");
    printf("Order should be: 5 4 3 2 1\n");
    printf("Actual: ");

    {
        defer printf("1 ");
        defer printf("2 ");
        defer printf("3 ");
        defer printf("4 ");
        defer printf("5 ");
    }

    printf("\n");
    return 0;
}

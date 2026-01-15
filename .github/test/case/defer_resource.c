// Test: Resource cleanup pattern with defer

#include <stdio.h>
#include <stdlib.h>

int main()
{
    printf("Test: Resource cleanup with defer\n");

    int *ptr = malloc(sizeof(int) * 10);
    if (!ptr)
        return 1;
    defer free(ptr);

    ptr[0] = 100;
    ptr[9] = 999;

    printf("ptr[0] = %d, ptr[9] = %d\n", ptr[0], ptr[9]);
    printf("Memory will be freed by defer\n");

    return 0;
}

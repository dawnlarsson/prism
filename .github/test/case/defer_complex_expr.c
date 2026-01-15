// Test: Defer with complex expressions

#include <stdio.h>
#include <stdlib.h>

void cleanup(void *p)
{
    printf("cleanup(%p)\n", p);
    free(p);
}

int main()
{
    printf("Test: Defer with complex expressions\n");

    int *a = malloc(sizeof(int));
    int *b = malloc(sizeof(int));

    defer cleanup(a);
    defer cleanup(b);

    // Defer with function call that has nested parens
    defer printf("Value: %d\n", (1 + 2) * 3);

    *a = 10;
    *b = 20;

    printf("a=%d, b=%d\n", *a, *b);
    printf("About to exit scope...\n");

    return 0;
}

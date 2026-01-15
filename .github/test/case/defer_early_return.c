// Test: Early return with defer cleanup

#include <stdio.h>
#include <stdlib.h>

int process(int should_fail)
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

int main()
{
    printf("Test: Early return with defer\n\n");

    printf("--- Call with failure ---\n");
    process(1);

    printf("\n--- Call without failure ---\n");
    process(0);

    return 0;
}

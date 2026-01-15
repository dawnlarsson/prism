// Test: Defer in comments should NOT be processed

#include <stdio.h>

int main()
{
    printf("Test: 'defer' in comments should be ignored\n");

    // defer printf("This is in a line comment\n");

    /* defer printf("This is in a block comment\n"); */

    /*
     * defer printf("Multi-line block comment\n");
     */

    defer printf("This is a REAL defer\n");

    printf("Only 'This is a REAL defer' should print after this\n");
    return 0;
}

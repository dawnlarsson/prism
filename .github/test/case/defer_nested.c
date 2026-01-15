// Test: Nested scopes with defer

#include <stdio.h>

int main()
{
    printf("Test: Nested defer scopes\n");

    defer printf("outer-end\n");
    printf("outer-start\n");

    {
        defer printf("inner-end\n");
        printf("inner-start\n");
    }

    printf("back-to-outer\n");

    // Expected output:
    // outer-start
    // inner-start
    // inner-end
    // back-to-outer
    // outer-end

    return 0;
}

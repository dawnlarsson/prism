// Test: File handling with defer

#include <stdio.h>

int main()
{
    printf("Test: File handling with defer\n");

    FILE *f = fopen("/tmp/prism_test.txt", "w");
    if (!f)
    {
        printf("Could not create test file\n");
        return 1;
    }
    defer fclose(f);

    fprintf(f, "Hello from prism!\n");
    printf("Wrote to file, defer will close it\n");

    return 0;
}

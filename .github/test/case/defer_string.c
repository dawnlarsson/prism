// Test: Defer keyword in strings should NOT be processed

#include <stdio.h>

int main()
{
    printf("Test: 'defer' in strings should be literal\n");

    // These should print literally, not be treated as defer statements
    printf("The word defer appears here\n");
    printf("defer is a keyword\n");

    char *str = "defer should not trigger";
    printf("%s\n", str);

    // This is an actual defer
    defer printf("This IS a real defer\n");

    printf("End of main\n");
    return 0;
}

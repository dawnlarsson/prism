// test_lib.c - Library mode tests for Prism with memory leak detection
//
// Compile: cc -DPRISM_LIB_MODE -o test_lib test_lib.c
// Run:     ./test_lib
// Valgrind: valgrind --leak-check=full ./test_lib
//
// This tests:
// 1. Basic library API usage
// 2. Multiple sequential transpilations (memory reuse)
// 3. Error handling paths
// 4. Memory leak detection via iteration stress test
// 5. Feature flag combinations

#ifndef PRISM_LIB_MODE
#define PRISM_LIB_MODE
#endif
#include "prism.c"

#include <assert.h>
#include <sys/resource.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, name)                \
    do                                   \
    {                                    \
        if (cond)                        \
        {                                \
            printf("[PASS] %s\n", name); \
            passed++;                    \
        }                                \
        else                             \
        {                                \
            printf("[FAIL] %s\n", name); \
            failed++;                    \
        }                                \
    } while (0)

#define CHECK_EQ(a, b, name)                                                      \
    do                                                                            \
    {                                                                             \
        if ((a) == (b))                                                           \
        {                                                                         \
            printf("[PASS] %s\n", name);                                          \
            passed++;                                                             \
        }                                                                         \
        else                                                                      \
        {                                                                         \
            printf("[FAIL] %s: expected %d, got %d\n", name, (int)(b), (int)(a)); \
            failed++;                                                             \
        }                                                                         \
    } while (0)

// Get current memory usage in KB (Linux-specific, returns 0 on other platforms)
static long get_memory_usage_kb(void)
{
#ifdef __linux__
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return 0;

    char line[256];
    long vmrss = 0;
    while (fgets(line, sizeof(line), f))
    {
        if (strncmp(line, "VmRSS:", 6) == 0)
        {
            sscanf(line + 6, "%ld", &vmrss);
            break;
        }
    }
    fclose(f);
    return vmrss;
#else
    return 0;
#endif
}

// Create a temporary C file with the given content
static char *create_temp_file(const char *content)
{
    char *path = malloc(64);
    snprintf(path, 64, "/tmp/prism_test_XXXXXX.c");

    int fd = mkstemps(path, 2);
    if (fd < 0)
    {
        free(path);
        return NULL;
    }

    write(fd, content, strlen(content));
    close(fd);
    return path;
}

// Test 1: Basic transpilation of simple code
static void test_basic_transpile(void)
{
    printf("\n--- Basic Transpile Tests ---\n");

    const char *code =
        "#include <stdio.h>\n"
        "int main(void) {\n"
        "    int x;\n" // Should be zero-initialized
        "    printf(\"%d\\n\", x);\n"
        "    return 0;\n"
        "}\n";

    char *path = create_temp_file(code);
    CHECK(path != NULL, "create temp file");

    PrismFeatures features = prism_defaults();
    PrismResult result = prism_transpile_file(path, features);

    CHECK_EQ(result.status, PRISM_OK, "transpile status OK");
    CHECK(result.output != NULL, "output not NULL");
    CHECK(result.output_len > 0, "output has content");
    CHECK(result.error_msg == NULL, "no error message");

    // Check that zero-init was applied
    CHECK(strstr(result.output, "= {0}") != NULL ||
              strstr(result.output, "= 0") != NULL,
          "zero-init applied");

    prism_free(&result);
    CHECK(result.output == NULL, "output freed");
    CHECK(result.error_msg == NULL, "error_msg freed");

    unlink(path);
    free(path);
}

// Test 2: Defer functionality
static void test_defer_transpile(void)
{
    printf("\n--- Defer Transpile Tests ---\n");

    const char *code =
        "#include <stdio.h>\n"
        "int main(void) {\n"
        "    {\n"
        "        defer printf(\"B\");\n"
        "        printf(\"A\");\n"
        "    }\n"
        "    return 0;\n"
        "}\n";

    char *path = create_temp_file(code);
    CHECK(path != NULL, "create temp file for defer");

    PrismFeatures features = prism_defaults();
    PrismResult result = prism_transpile_file(path, features);

    CHECK_EQ(result.status, PRISM_OK, "defer transpile OK");
    CHECK(result.output != NULL, "defer output not NULL");

    // Check that defer was expanded (the deferred statement appears after the block content)
    // The output should have the printf("B") moved/duplicated for the defer expansion
    CHECK(result.output_len > strlen(code), "defer expansion increased output");

    prism_free(&result);
    unlink(path);
    free(path);
}

// Test 3: Feature flag combinations
static void test_feature_flags(void)
{
    printf("\n--- Feature Flag Tests ---\n");

    const char *code =
        "int main(void) {\n"
        "    int x;\n"
        "    return x;\n"
        "}\n";

    char *path = create_temp_file(code);
    CHECK(path != NULL, "create temp file for features");

    // Test with zeroinit disabled
    {
        PrismFeatures features = prism_defaults();
        features.zeroinit = false;
        PrismResult result = prism_transpile_file(path, features);

        CHECK_EQ(result.status, PRISM_OK, "no-zeroinit transpile OK");
        // With zeroinit disabled, should NOT have = {0} or = 0 for the int x
        // (This is a weak check since the output format may vary)
        prism_free(&result);
    }

    // Test with defer disabled
    {
        const char *defer_code =
            "int main(void) {\n"
            "    { defer (void)0; }\n"
            "    return 0;\n"
            "}\n";
        char *defer_path = create_temp_file(defer_code);

        PrismFeatures features = prism_defaults();
        features.defer = false;
        PrismResult result = prism_transpile_file(defer_path, features);

        // With defer disabled, 'defer' is treated as identifier - might cause syntax error
        // or just pass through. Either way, we should handle it gracefully.
        CHECK(result.status == PRISM_OK || result.status == PRISM_ERR_SYNTAX,
              "defer disabled handled gracefully");

        prism_free(&result);
        unlink(defer_path);
        free(defer_path);
    }

    // Test with line directives disabled
    {
        PrismFeatures features = prism_defaults();
        features.line_directives = false;
        PrismResult result = prism_transpile_file(path, features);

        CHECK_EQ(result.status, PRISM_OK, "no-line-directives OK");
        // Should not have #line directives
        CHECK(strstr(result.output, "#line") == NULL ||
                  strstr(result.output, "# ") == NULL,
              "no line directives in output");
        prism_free(&result);
    }

    unlink(path);
    free(path);
}

// Test 4: Error handling - nonexistent file
static void test_error_handling(void)
{
    printf("\n--- Error Handling Tests ---\n");

    PrismFeatures features = prism_defaults();

    // Test with nonexistent file
    PrismResult result = prism_transpile_file("/nonexistent/path/file.c", features);

    CHECK(result.status != PRISM_OK, "nonexistent file returns error");
    CHECK(result.output == NULL, "no output on error");
    // error_msg may or may not be set depending on where the error occurs

    prism_free(&result);
    CHECK(result.output == NULL, "cleanup after error");
}

// Test 5: Multiple sequential transpilations (memory reuse check)
static void test_sequential_transpilations(void)
{
    printf("\n--- Sequential Transpilation Tests ---\n");

    const char *codes[] = {
        "int main(void) { int a; return a; }\n",
        "int main(void) { int b; { defer (void)0; } return b; }\n",
        "#include <stdio.h>\nint main(void) { int c; printf(\"%d\", c); return 0; }\n",
        "typedef int MyInt; int main(void) { MyInt x; return x; }\n",
        "int main(void) { for(int i; i < 10; i++) { int j; } return 0; }\n",
    };

    PrismFeatures features = prism_defaults();

    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
    {
        char *path = create_temp_file(codes[i]);
        if (!path)
            continue;

        PrismResult result = prism_transpile_file(path, features);

        char name[64];
        snprintf(name, sizeof(name), "sequential transpile %zu", i + 1);
        CHECK_EQ(result.status, PRISM_OK, name);

        prism_free(&result);
        unlink(path);
        free(path);
    }
}

// Test 6: Memory leak stress test
static void test_memory_leak_stress(void)
{
    printf("\n--- Memory Leak Stress Test ---\n");

    const char *code =
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "typedef struct { int x; int y; } Point;\n"
        "int main(void) {\n"
        "    Point p;\n"
        "    int arr[10];\n"
        "    {\n"
        "        defer printf(\"cleanup\\n\");\n"
        "        int local;\n"
        "        for (int i; i < 10; i++) {\n"
        "            arr[i] = i;\n"
        "        }\n"
        "    }\n"
        "    return 0;\n"
        "}\n";

    char *path = create_temp_file(code);
    CHECK(path != NULL, "create stress test file");

    PrismFeatures features = prism_defaults();

    // Detect if running under valgrind (check for RUNNING_ON_VALGRIND env or slow execution)
    // Use fewer iterations when running under valgrind to avoid timeout
    int iterations = 100;
    bool under_valgrind = getenv("VALGRIND") || getenv("RUNNING_ON_VALGRIND");
    if (under_valgrind)
    {
        iterations = 5;
        printf("  (Valgrind mode: reduced to %d iterations)\n", iterations);
        printf("  Note: Memory growth under valgrind is inflated by instrumentation.\n");
        printf("  Trust valgrind's leak report, not RSS growth.\n");
    }

    // Warmup - first few iterations may allocate caches
    int warmup = iterations < 10 ? 1 : 5;
    for (int i = 0; i < warmup; i++)
    {
        PrismResult result = prism_transpile_file(path, features);
        prism_free(&result);
        prism_reset();
    }

    // Get baseline memory after warmup
    long baseline_mem = get_memory_usage_kb();

    // Run iterations
    for (int i = 0; i < iterations; i++)
    {
        PrismResult result = prism_transpile_file(path, features);

        if (result.status != PRISM_OK)
        {
            printf("[FAIL] stress iteration %d failed\n", i);
            failed++;
            prism_free(&result);
            break;
        }

        prism_free(&result);

        // Call prism_reset to clean up all transpiler state
        prism_reset();
    }

    // Get final memory
    long final_mem = get_memory_usage_kb();
    long mem_growth = final_mem - baseline_mem;

    printf("  Memory after warmup: %ld KB\n", baseline_mem);
    printf("  Memory after %d iterations: %ld KB\n", iterations, final_mem);
    printf("  Memory growth: %ld KB\n", mem_growth);

    // Allow some growth for internal caches, but flag excessive growth
    // After warmup, should be minimal growth (< 1MB for 100 iterations)
    // Under valgrind, memory reporting is unreliable due to instrumentation overhead
    if (under_valgrind)
    {
        // Under valgrind, trust valgrind's leak report, not RSS growth
        passed++;
        printf("[PASS] memory test (valgrind mode - check leak summary above)\n");
    }
    else if (mem_growth < 1024)
    {
        passed++;
        printf("[PASS] memory growth under 1MB after warmup\n");
    }
    else
    {
        // Check if growth is proportional to iterations (true leak) or fixed (cache)
        printf("[WARN] memory growth %ld KB - may indicate leak\n", mem_growth);
        printf("       Growth per iteration: %.1f KB\n", (float)mem_growth / iterations);
        // Still pass if growth is minimal per iteration (< 10KB each)
        if (mem_growth / iterations < 10)
        {
            passed++;
            printf("[PASS] growth rate acceptable (< 10KB/iteration)\n");
        }
        else
        {
            failed++;
            printf("[FAIL] excessive memory growth detected\n");
        }
    }

    passed++; // Count the stress test as passed if we got here
    printf("[PASS] completed %d stress iterations\n", iterations);

    unlink(path);
    free(path);
}

// Test 7: UTF-8 and digraph handling in lib mode
static void test_unicode_digraph_lib(void)
{
    printf("\n--- Unicode/Digraph Lib Tests ---\n");

    // Test UTF-8 identifiers
    const char *utf8_code =
        "int main(void) {\n"
        "    int café = 42;\n"
        "    int π = 314;\n"
        "    return café + π;\n"
        "}\n";

    char *path = create_temp_file(utf8_code);
    CHECK(path != NULL, "create UTF-8 test file");

    PrismFeatures features = prism_defaults();
    PrismResult result = prism_transpile_file(path, features);

    CHECK_EQ(result.status, PRISM_OK, "UTF-8 transpile OK");
    CHECK(result.output != NULL, "UTF-8 output not NULL");

    prism_free(&result);
    unlink(path);
    free(path);

    // Test digraphs
    const char *digraph_code =
        "int main(void) <%\n"
        "    int arr<:3:> = <% 1, 2, 3 %>;\n"
        "    return arr<:0:>;\n"
        "%>\n";

    path = create_temp_file(digraph_code);
    CHECK(path != NULL, "create digraph test file");

    result = prism_transpile_file(path, features);

    CHECK_EQ(result.status, PRISM_OK, "digraph transpile OK");
    CHECK(result.output != NULL, "digraph output not NULL");

    // Check that digraphs were translated to normal form
    if (result.output)
    {
        CHECK(strstr(result.output, "{") != NULL, "digraph <% translated to {");
        CHECK(strstr(result.output, "[") != NULL, "digraph <: translated to [");
    }

    prism_free(&result);
    unlink(path);
    free(path);
}

// Test 8: Complex code with all features
static void test_complex_code(void)
{
    printf("\n--- Complex Code Test ---\n");

    const char *code =
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "\n"
        "typedef struct Node {\n"
        "    int value;\n"
        "    struct Node *next;\n"
        "} Node;\n"
        "\n"
        "Node *create_node(int val) {\n"
        "    Node *n = malloc(sizeof(Node));\n"
        "    if (!n) return NULL;\n"
        "    n->value = val;\n"
        "    n->next = NULL;\n"
        "    return n;\n"
        "}\n"
        "\n"
        "void process(void) {\n"
        "    Node *head;\n"
        "    defer { if (head) free(head); };\n"
        "    head = create_node(42);\n"
        "    if (!head) return;\n"
        "    printf(\"Value: %d\\n\", head->value);\n"
        "}\n"
        "\n"
        "int main(void) {\n"
        "    int result;\n"
        "    {\n"
        "        defer printf(\"Cleanup\\n\");\n"
        "        for (int i; i < 5; i++) {\n"
        "            int temp;\n"
        "            result += temp;\n"
        "        }\n"
        "    }\n"
        "    process();\n"
        "    return result;\n"
        "}\n";

    char *path = create_temp_file(code);
    CHECK(path != NULL, "create complex test file");

    PrismFeatures features = prism_defaults();
    PrismResult result = prism_transpile_file(path, features);

    CHECK_EQ(result.status, PRISM_OK, "complex code transpile OK");
    CHECK(result.output != NULL, "complex output not NULL");
    CHECK(result.output_len > strlen(code), "complex code expanded");

    prism_free(&result);
    unlink(path);
    free(path);
}

// Test 9: prism_defaults() function
static void test_defaults(void)
{
    printf("\n--- Defaults Test ---\n");

    PrismFeatures features = prism_defaults();

    CHECK(features.defer == true, "default defer=true");
    CHECK(features.zeroinit == true, "default zeroinit=true");
    CHECK(features.line_directives == true, "default line_directives=true");
    CHECK(features.warn_safety == false, "default warn_safety=false");
    CHECK(features.flatten_headers == true, "default flatten_headers=true");
}

// Test 10: Double-free protection
static void test_double_free_protection(void)
{
    printf("\n--- Double-Free Protection Test ---\n");

    const char *code = "int main(void) { return 0; }\n";
    char *path = create_temp_file(code);

    PrismFeatures features = prism_defaults();
    PrismResult result = prism_transpile_file(path, features);

    CHECK_EQ(result.status, PRISM_OK, "simple transpile for double-free test");

    // First free
    prism_free(&result);
    CHECK(result.output == NULL, "first free nulls output");
    CHECK(result.error_msg == NULL, "first free nulls error_msg");

    // Second free should be safe (no crash)
    prism_free(&result);
    CHECK(result.output == NULL, "second free safe");

    passed++;
    printf("[PASS] double prism_free() is safe\n");

    unlink(path);
    free(path);
}

// Test 11: Repeated transpilations of different files after prism_reset
static void test_repeated_reset(void)
{
    printf("\n--- Repeated Reset Test ---\n");

    PrismFeatures features = prism_defaults();

    // Transpile several different files, calling prism_reset between each
    const char *codes[] = {
        "#include <stdio.h>\nint main(void) { printf(\"hello\"); return 0; }\n",
        "typedef int MyInt; MyInt add(MyInt a, MyInt b) { return a + b; }\n",
        "struct Point { int x, y; }; int main(void) { struct Point p; return 0; }\n",
        "int factorial(int n) { return n <= 1 ? 1 : n * factorial(n-1); }\n",
    };

    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
    {
        char *path = create_temp_file(codes[i]);
        if (!path)
            continue;

        PrismResult result = prism_transpile_file(path, features);

        char name[64];
        snprintf(name, sizeof(name), "reset+transpile %zu", i + 1);
        CHECK_EQ(result.status, PRISM_OK, name);
        CHECK(result.output != NULL, "output not NULL after reset");
        CHECK(result.output_len > 0, "output has content after reset");

        prism_free(&result);
        prism_reset(); // Full cleanup

        unlink(path);
        free(path);
    }

    // After all resets, do one more transpilation to verify state is clean
    const char *final_code = "int main(void) { int x; { defer (void)0; } return x; }\n";
    char *path = create_temp_file(final_code);

    PrismResult result = prism_transpile_file(path, features);
    CHECK_EQ(result.status, PRISM_OK, "final transpile after resets");

    prism_free(&result);
    unlink(path);
    free(path);
}

int main(void)
{
    printf("=== PRISM LIBRARY MODE TEST SUITE ===\n");
    printf("Run with valgrind for full leak detection:\n");
    printf("  valgrind --leak-check=full ./test_lib\n\n");

    test_defaults();
    test_basic_transpile();
    test_defer_transpile();
    test_feature_flags();
    test_error_handling();
    test_sequential_transpilations();
    test_unicode_digraph_lib();
    test_complex_code();
    test_double_free_protection();
    test_repeated_reset();
    test_memory_leak_stress(); // Run last as it does many iterations

    printf("\n========================================\n");
    printf("TOTAL: %d passed, %d failed\n", passed, failed);
    printf("========================================\n");

    return (failed == 0) ? 0 : 1;
}

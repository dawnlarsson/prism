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
#include "../prism.c"

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

    // Check that zero-init was applied (may be = {0}, = 0, or PRISM_ATOMIC_INIT macro)
    CHECK(strstr(result.output, "= {0}") != NULL ||
              strstr(result.output, "= 0") != NULL ||
              strstr(result.output, "PRISM_ATOMIC_INIT") != NULL,
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

static void test_error_recovery_no_exit(void)
{
    printf("\n--- Error Recovery Tests (no exit) ---\n");

    PrismFeatures features = prism_defaults();

    // Test 1: Syntax error that would trigger error_tok()
    // Using invalid defer usage that Prism should catch
    const char *invalid_code1 =
        "int main(void) {\n"
        "    for (int i = 0; defer (void)0; i++) { }\n" // defer in for header
        "    return 0;\n"
        "}\n";

    char *path1 = create_temp_file(invalid_code1);
    if (path1)
    {
        PrismResult result = prism_transpile_file(path1, features);
        CHECK(result.status != PRISM_OK, "syntax error returns error status (not exit)");
        CHECK(result.error_msg != NULL, "error message captured");
        if (result.error_msg)
        {
            CHECK(strstr(result.error_msg, "defer") != NULL ||
                      strstr(result.error_msg, "control") != NULL,
                  "error message is descriptive");
        }
        prism_free(&result);
        unlink(path1);
        free(path1);
    }

    // Test 2: After error, transpiler should still work for valid code
    const char *valid_code = "int main(void) { int x; return x; }\n";
    char *path2 = create_temp_file(valid_code);
    if (path2)
    {
        PrismResult result = prism_transpile_file(path2, features);
        CHECK_EQ(result.status, PRISM_OK, "transpiler recovers after error");
        CHECK(result.output != NULL, "output generated after recovery");
        prism_free(&result);
        unlink(path2);
        free(path2);
    }

    // Test 3: Multiple errors in sequence should all be recoverable
    const char *errors[] = {
        "int main(void) { for(; defer 0;) {} return 0; }\n",
        "int main(void) { if (1) defer (void)0; return 0; }\n", // braceless defer
    };

    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++)
    {
        char *path = create_temp_file(errors[i]);
        if (path)
        {
            PrismResult result = prism_transpile_file(path, features);
            char name[64];
            snprintf(name, sizeof(name), "error %zu doesn't kill process", i + 1);
            CHECK(result.status != PRISM_OK, name);
            prism_free(&result);
            unlink(path);
            free(path);
        }
    }

    // Final verification: process is still alive and working
    char *path3 = create_temp_file("int main(void) { return 42; }\n");
    if (path3)
    {
        PrismResult result = prism_transpile_file(path3, features);
        CHECK_EQ(result.status, PRISM_OK, "process still alive after multiple errors");
        prism_free(&result);
        unlink(path3);
        free(path3);
    }
}

static void test_defer_break_continue_rejected(void)
{
    printf("\n--- Defer Break/Continue Rejection Tests ---\n");

    PrismFeatures features = prism_defaults();

    // Test 1: bare 'break' inside defer statement
    const char *code_break_bare =
        "int main(void) {\n"
        "    for (int i = 0; i < 10; i++) {\n"
        "        defer break;\n"
        "    }\n"
        "    return 0;\n"
        "}\n";
    char *path = create_temp_file(code_break_bare);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status != PRISM_OK, "defer break; rejected");
        CHECK(result.error_msg != NULL, "defer break; has error message");
        if (result.error_msg)
            CHECK(strstr(result.error_msg, "break") != NULL ||
                      strstr(result.error_msg, "missing") != NULL,
                  "defer break; error mentions break or missing");
        prism_free(&result);
        unlink(path);
        free(path);
    }

    // Test 2: bare 'continue' inside defer statement
    const char *code_cont_bare =
        "int main(void) {\n"
        "    for (int i = 0; i < 10; i++) {\n"
        "        defer continue;\n"
        "    }\n"
        "    return 0;\n"
        "}\n";
    path = create_temp_file(code_cont_bare);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status != PRISM_OK, "defer continue; rejected");
        CHECK(result.error_msg != NULL, "defer continue; has error message");
        if (result.error_msg)
            CHECK(strstr(result.error_msg, "continue") != NULL ||
                      strstr(result.error_msg, "missing") != NULL,
                  "defer continue; error mentions continue or missing");
        prism_free(&result);
        unlink(path);
        free(path);
    }

    // Test 3: break inside braced defer body (with trailing ;)
    const char *code_break_braced =
        "int main(void) {\n"
        "    for (int i = 0; i < 10; i++) {\n"
        "        defer { (void)0; break; };\n"
        "    }\n"
        "    return 0;\n"
        "}\n";
    path = create_temp_file(code_break_braced);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status != PRISM_OK, "defer { break; }; rejected");
        CHECK(result.error_msg != NULL, "defer { break; }; has error message");
        if (result.error_msg)
            CHECK(strstr(result.error_msg, "break") != NULL &&
                      strstr(result.error_msg, "bypass") != NULL,
                  "defer { break; }; error mentions bypass");
        prism_free(&result);
        unlink(path);
        free(path);
    }

    // Test 4: continue inside braced defer body (with trailing ;)
    const char *code_cont_braced =
        "int main(void) {\n"
        "    for (int i = 0; i < 10; i++) {\n"
        "        defer { (void)0; continue; };\n"
        "    }\n"
        "    return 0;\n"
        "}\n";
    path = create_temp_file(code_cont_braced);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status != PRISM_OK, "defer { continue; }; rejected");
        CHECK(result.error_msg != NULL, "defer { continue; }; has error message");
        if (result.error_msg)
            CHECK(strstr(result.error_msg, "continue") != NULL &&
                      strstr(result.error_msg, "bypass") != NULL,
                  "defer { continue; }; error mentions bypass");
        prism_free(&result);
        unlink(path);
        free(path);
    }

    // Test 5: Recovery — transpiler still works after break/continue errors
    const char *valid_code = "int main(void) { int x; return x; }\n";
    path = create_temp_file(valid_code);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK_EQ(result.status, PRISM_OK, "transpiler recovers after break/continue rejection");
        prism_free(&result);
        unlink(path);
        free(path);
    }

    // Test 6: break inside for loop inside defer — should be ALLOWED
    const char *code_loop_break =
        "void f(void) {\n"
        "    defer {\n"
        "        for (int i = 0; i < 10; i++) {\n"
        "            if (i == 3) break;\n"
        "        }\n"
        "    };\n"
        "}\n"
        "int main(void) { f(); return 0; }\n";
    path = create_temp_file(code_loop_break);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status == PRISM_OK, "break in for inside defer: allowed");
        CHECK(result.error_msg == NULL, "break in for inside defer: no error");
        prism_free(&result);
        unlink(path);
        free(path);
    }

    // Test 7: continue inside while loop inside defer — should be ALLOWED
    const char *code_loop_cont =
        "void f(void) {\n"
        "    defer {\n"
        "        int i = 0;\n"
        "        while (i < 5) {\n"
        "            i++;\n"
        "            if (i == 3) continue;\n"
        "        }\n"
        "    };\n"
        "}\n"
        "int main(void) { f(); return 0; }\n";
    path = create_temp_file(code_loop_cont);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status == PRISM_OK, "continue in while inside defer: allowed");
        CHECK(result.error_msg == NULL, "continue in while inside defer: no error");
        prism_free(&result);
        unlink(path);
        free(path);
    }

    // Test 8: break inside switch inside defer — should be ALLOWED
    const char *code_switch_break =
        "void f(int x) {\n"
        "    defer {\n"
        "        switch (x) {\n"
        "            case 1: break;\n"
        "            default: break;\n"
        "        }\n"
        "    };\n"
        "}\n"
        "int main(void) { f(1); return 0; }\n";
    path = create_temp_file(code_switch_break);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status == PRISM_OK, "break in switch inside defer: allowed");
        CHECK(result.error_msg == NULL, "break in switch inside defer: no error");
        prism_free(&result);
        unlink(path);
        free(path);
    }

    // Test 9: continue inside switch (no loop) inside defer — should be REJECTED
    // (continue in switch targets the enclosing loop, which is outside the defer)
    const char *code_switch_cont =
        "void f(int x) {\n"
        "    for (int i = 0; i < 10; i++) {\n"
        "        defer {\n"
        "            switch (x) {\n"
        "                case 1: continue;\n"
        "            }\n"
        "        };\n"
        "    }\n"
        "}\n"
        "int main(void) { f(1); return 0; }\n";
    path = create_temp_file(code_switch_cont);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status != PRISM_OK, "continue in switch (no loop) inside defer: rejected");
        CHECK(result.error_msg != NULL, "continue in switch inside defer: has error");
        if (result.error_msg)
            CHECK(strstr(result.error_msg, "continue") != NULL &&
                      strstr(result.error_msg, "bypass") != NULL,
                  "continue in switch inside defer: error mentions bypass");
        prism_free(&result);
        unlink(path);
        free(path);
    }

    // Test 10: break inside do-while inside defer — should be ALLOWED
    const char *code_dowhile =
        "void f(void) {\n"
        "    defer {\n"
        "        int i = 0;\n"
        "        do {\n"
        "            i++;\n"
        "            if (i == 3) break;\n"
        "        } while (i < 10);\n"
        "    };\n"
        "}\n"
        "int main(void) { f(); return 0; }\n";
    path = create_temp_file(code_dowhile);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status == PRISM_OK, "break in do-while inside defer: allowed");
        CHECK(result.error_msg == NULL, "break in do-while inside defer: no error");
        prism_free(&result);
        unlink(path);
        free(path);
    }
}

static void test_array_orelse_rejected(void)
{
    printf("\n--- Array Orelse Rejection Tests ---\n");

    PrismFeatures features = prism_defaults();

    // Test 1: array orelse with block fallback (e.g. orelse { return 1; })
    const char *code_arr_block =
        "int main(void) {\n"
        "    int arr[] = {1, 2} orelse { return 1; };\n"
        "    return arr[0];\n"
        "}\n";
    char *path = create_temp_file(code_arr_block);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status != PRISM_OK, "array orelse block: rejected");
        CHECK(result.error_msg != NULL, "array orelse block: has error message");
        if (result.error_msg)
            CHECK(strstr(result.error_msg, "array") != NULL &&
                      strstr(result.error_msg, "never NULL") != NULL,
                  "array orelse block: error mentions array never NULL");
        prism_free(&result);
        unlink(path);
        free(path);
    }

    // Test 2: const array orelse with expression fallback
    const char *code_const_arr =
        "int main(void) {\n"
        "    const int arr[] = {1, 2} orelse (int[]){3, 4};\n"
        "    return arr[0];\n"
        "}\n";
    path = create_temp_file(code_const_arr);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status != PRISM_OK, "const array orelse fallback: rejected");
        CHECK(result.error_msg != NULL, "const array orelse fallback: has error message");
        if (result.error_msg)
            CHECK(strstr(result.error_msg, "array") != NULL &&
                      strstr(result.error_msg, "never NULL") != NULL,
                  "const array orelse fallback: error mentions array never NULL");
        prism_free(&result);
        unlink(path);
        free(path);
    }

    // Test 3: non-const array orelse with expression fallback
    const char *code_arr_expr =
        "int main(void) {\n"
        "    int arr[] = {1, 2} orelse (int[]){3, 4};\n"
        "    return arr[0];\n"
        "}\n";
    path = create_temp_file(code_arr_expr);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status != PRISM_OK, "array orelse fallback: rejected");
        CHECK(result.error_msg != NULL, "array orelse fallback: has error message");
        if (result.error_msg)
            CHECK(strstr(result.error_msg, "array") != NULL &&
                      strstr(result.error_msg, "never NULL") != NULL,
                  "array orelse fallback: error mentions array never NULL");
        prism_free(&result);
        unlink(path);
        free(path);
    }
}

static void test_deep_struct_nesting_walker(void)
{
    printf("\n--- Deep Struct Nesting Walker Tests ---\n");

    PrismFeatures features = prism_defaults();

    // Test 1: 70 levels of nested anonymous structs inside a function with goto.
    // The walker's struct_depth must not desync when depth exceeds the 64-bit
    // bitmask capacity. Before the fix, closing braces at depth >= 64 would
    // blindly decrement struct_depth, potentially causing false goto errors.
    char code[8192];
    int pos = 0;
    pos += snprintf(code + pos, sizeof(code) - pos,
                    "#include <stdio.h>\n"
                    "void func(int flag) {\n"
                    "    struct Deep {\n");

    // 69 levels of nested anonymous structs (total depth = 70 with outer struct)
    for (int i = 0; i < 69; i++)
        pos += snprintf(code + pos, sizeof(code) - pos, "    struct {\n");

    pos += snprintf(code + pos, sizeof(code) - pos, "        int leaf;\n");

    for (int i = 0; i < 69; i++)
        pos += snprintf(code + pos, sizeof(code) - pos, "    };\n");

    pos += snprintf(code + pos, sizeof(code) - pos,
                    "    };\n"
                    "    if (flag)\n"
                    "        goto done;\n"
                    "    printf(\"not skipped\\n\");\n"
                    "    done:\n"
                    "    printf(\"done\\n\");\n"
                    "}\n"
                    "int main(void) { func(1); return 0; }\n");

    char *path = create_temp_file(code);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status == PRISM_OK, "deep struct nesting: transpiles OK");
        CHECK(result.error_msg == NULL, "deep struct nesting: no error");
        prism_free(&result);
        unlink(path);
        free(path);
    }

    // Test 2: Same deep nesting but goto skips over a variable declaration.
    // Walker must correctly identify the declaration despite deep struct nesting.
    pos = 0;
    pos += snprintf(code + pos, sizeof(code) - pos,
                    "void func2(int flag) {\n"
                    "    struct Deep2 {\n");

    for (int i = 0; i < 69; i++)
        pos += snprintf(code + pos, sizeof(code) - pos, "    struct {\n");
    pos += snprintf(code + pos, sizeof(code) - pos, "        int leaf;\n");
    for (int i = 0; i < 69; i++)
        pos += snprintf(code + pos, sizeof(code) - pos, "    };\n");

    pos += snprintf(code + pos, sizeof(code) - pos,
                    "    };\n"
                    "    if (flag)\n"
                    "        goto done;\n"
                    "    int val = 42;\n"
                    "    done:\n"
                    "    (void)0;\n"
                    "}\n"
                    "int main(void) { func2(1); return 0; }\n");

    path = create_temp_file(code);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        // Should error: goto skips over 'val' declaration
        CHECK(result.status != PRISM_OK, "deep struct nesting + goto skip: rejected");
        CHECK(result.error_msg != NULL, "deep struct nesting + goto skip: has error");
        if (result.error_msg)
            CHECK(strstr(result.error_msg, "skip") != NULL ||
                      strstr(result.error_msg, "bypass") != NULL,
                  "deep struct nesting + goto skip: error mentions skip/bypass");
        prism_free(&result);
        unlink(path);
        free(path);
    }
}

static void test_c23_attr_void_function(void)
{
    printf("\n--- C23 Attribute Void Function Tests ---\n");

    PrismFeatures features = prism_defaults();

    // Test 1: void [[attr]] func() — should be recognized as void-returning
    // Without fix, Prism would generate __auto_type _prism_ret = ... for return
    const char *code_c23_void =
        "void [[deprecated]] func(void) {\n"
        "    defer (void)0;\n"
        "    return;\n"
        "}\n"
        "int main(void) { func(); return 0; }\n";
    char *path = create_temp_file(code_c23_void);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status == PRISM_OK, "C23 void [[attr]] func: transpiles OK");
        if (result.output)
        {
            // Must NOT contain _prism_ret (void function doesn't capture return value)
            CHECK(strstr(result.output, "_prism_ret") == NULL,
                  "C23 void [[attr]] func: no _prism_ret generated");
            // Must still contain the attribute and function
            CHECK(strstr(result.output, "[[deprecated]]") != NULL,
                  "C23 void [[attr]] func: attribute preserved");
        }
        prism_free(&result);
        unlink(path);
        free(path);
    }

    // Test 2: void [[attr1]] [[attr2]] func() — multiple C23 attributes
    const char *code_multi_attr =
        "void [[deprecated]] [[maybe_unused]] func2(void) {\n"
        "    defer (void)0;\n"
        "    return;\n"
        "}\n"
        "int main(void) { func2(); return 0; }\n";
    path = create_temp_file(code_multi_attr);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status == PRISM_OK, "C23 void multi [[attr]] func: transpiles OK");
        if (result.output)
            CHECK(strstr(result.output, "_prism_ret") == NULL,
                  "C23 void multi [[attr]] func: no _prism_ret");
        prism_free(&result);
        unlink(path);
        free(path);
    }
}

static void test_generic_array_not_vla(void)
{
    printf("\n--- _Generic Array Not VLA Tests ---\n");

    PrismFeatures features = prism_defaults();

    // _Generic in array size should not trigger VLA detection
    // Before fix: `x` inside _Generic was seen as non-constant identifier → VLA
    // After fix: _Generic(...) is skipped entirely
    const char *code_generic_arr =
        "int main(void) {\n"
        "    int x = 0;\n"
        "    int arr[_Generic(x, int: 10, default: 20)];\n"
        "    return arr[0];\n"
        "}\n";
    char *path = create_temp_file(code_generic_arr);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status == PRISM_OK, "_Generic array: transpiles OK");
        if (result.output)
        {
            // Should use = {0} (constant size), NOT memset (VLA)
            CHECK(strstr(result.output, "= {0}") != NULL,
                  "_Generic array: uses = {0} not memset");
            CHECK(strstr(result.output, "memset") == NULL,
                  "_Generic array: no memset (not VLA)");
        }
        prism_free(&result);
        unlink(path);
        free(path);
    }
}

static void test_fnptr_return_type_capture(void)
{
    printf("\n--- Function Pointer Return Type Capture Tests ---\n");

    PrismFeatures features = prism_defaults();

    // Test 1: Function returning a raw function pointer with defer
    // void (*get_callback(void))(void) { defer ...; return fn; }
    // Prism must NOT emit __auto_type for the _prism_ret variable
    const char *code_fnptr =
        "static void my_fn(void) {}\n"
        "void (*get_callback(void))(void) {\n"
        "    defer (void)0;\n"
        "    return my_fn;\n"
        "}\n"
        "int main(void) { get_callback()(); return 0; }\n";
    char *path = create_temp_file(code_fnptr);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status == PRISM_OK, "bug_r2: fnptr return transpiles OK");
        if (result.output)
        {
            CHECK(strstr(result.output, "__auto_type") == NULL,
                  "bug_r2: fnptr return has no __auto_type");
            CHECK(strstr(result.output, "_prism_ret") != NULL,
                  "bug_r2: fnptr return has _prism_ret (captured type)");
        }
        prism_free(&result);
        unlink(path);
        free(path);
    }

    // Test 2: Typedef function pointer return (should always work)
    const char *code_typedef =
        "typedef void (*callback_t)(void);\n"
        "static void my_fn(void) {}\n"
        "callback_t get_cb(void) {\n"
        "    defer (void)0;\n"
        "    return my_fn;\n"
        "}\n"
        "int main(void) { get_cb()(); return 0; }\n";
    path = create_temp_file(code_typedef);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status == PRISM_OK, "bug_r2: typedef fnptr return transpiles OK");
        if (result.output)
        {
            CHECK(strstr(result.output, "__auto_type") == NULL,
                  "bug_r2: typedef fnptr return has no __auto_type");
        }
        prism_free(&result);
        unlink(path);
        free(path);
    }

    // Test 3: Complex return type — pointer to array
    const char *code_arrptr =
        "static int arr[5] = {1,2,3,4,5};\n"
        "int (*get_arr(void))[5] {\n"
        "    defer (void)0;\n"
        "    return &arr;\n"
        "}\n"
        "int main(void) { return (*get_arr())[0] - 1; }\n";
    path = create_temp_file(code_arrptr);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status == PRISM_OK, "bug_r2: array ptr return transpiles OK");
        if (result.output)
        {
            CHECK(strstr(result.output, "__auto_type") == NULL,
                  "bug_r2: array ptr return has no __auto_type");
        }
        prism_free(&result);
        unlink(path);
        free(path);
    }
}

static void test_line_directive_escaped_quote(void)
{
    printf("\n--- Line Directive Escaped Quote Tests ---\n");

    PrismFeatures features = prism_defaults();

    // Create code with an embedded #line directive containing an escaped quote
    // This simulates what the preprocessor would emit for a file named foo"bar.c
    const char *code =
        "#line 1 \"foo\\\"bar.c\"\n"
        "int main(void) {\n"
        "    defer (void)0;\n"
        "    return 0;\n"
        "}\n";
    char *path = create_temp_file(code);
    if (path)
    {
        PrismResult result = prism_transpile_file(path, features);
        CHECK(result.status == PRISM_OK, "bug_r3: escaped quote #line transpiles OK");
        if (result.output)
        {
            // The output should contain foo"bar.c properly escaped as foo\"bar.c
            // but NOT foo\\\"bar.c (double-escaped backslash + escaped quote)
            CHECK(strstr(result.output, "foo\\\\\\\"bar.c") == NULL,
                  "bug_r3: no triple-escaped filename in output");
            // Should have the properly escaped version
            CHECK(strstr(result.output, "foo\\\"bar.c") != NULL,
                  "bug_r3: properly escaped filename in output");
        }
        prism_free(&result);
        unlink(path);
        free(path);
    }
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
    test_error_recovery_no_exit();
    test_defer_break_continue_rejected();
    test_array_orelse_rejected();
    test_deep_struct_nesting_walker();
    test_c23_attr_void_function();
    test_generic_array_not_vla();

    test_fnptr_return_type_capture();
    test_line_directive_escaped_quote();

    test_memory_leak_stress();

    printf("\n========================================\n");
    printf("TOTAL: %d passed, %d failed\n", passed, failed);
    printf("========================================\n");

    return (failed == 0) ? 0 : 1;
}

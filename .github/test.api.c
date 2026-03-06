
static void test_basic_transpile(void) {
	printf("\n--- Basic Transpile Tests ---\n");

	const char *code =
	    "#include <stdio.h>\n"
	    "int main(void) {\n"
	    "    int x;\n"
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

static void test_defer_transpile(void) {
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
	CHECK(result.output_len > strlen(code), "defer expansion increased output");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_feature_flags(void) {
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
		CHECK(strstr(result.output, "#line") == NULL ||
		          strstr(result.output, "# ") == NULL,
		      "no line directives in output");
		prism_free(&result);
	}

	unlink(path);
	free(path);
}

static void test_error_handling(void) {
	printf("\n--- Error Handling Tests ---\n");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file("/nonexistent/path/file.c", features);

	CHECK(result.status != PRISM_OK, "nonexistent file returns error");
	CHECK(result.output == NULL, "no output on error");

	prism_free(&result);
	CHECK(result.output == NULL, "cleanup after error");
}

static void test_sequential_transpilations(void) {
	printf("\n--- Sequential Transpilation Tests ---\n");

	const char *codes[] = {
	    "int main(void) { int a; return a; }\n",
	    "int main(void) { int b; { defer (void)0; } return b; }\n",
	    "#include <stdio.h>\nint main(void) { int c; printf(\"%d\", c); return 0; }\n",
	    "typedef int MyInt; int main(void) { MyInt x; return x; }\n",
	    "int main(void) { for(int i; i < 10; i++) { int j; } return 0; }\n",
	};

	PrismFeatures features = prism_defaults();

	for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
		char *path = create_temp_file(codes[i]);
		if (!path) continue;

		PrismResult result = prism_transpile_file(path, features);

		char name[64];
		snprintf(name, sizeof(name), "sequential transpile %zu", i + 1);
		CHECK_EQ(result.status, PRISM_OK, name);

		prism_free(&result);
		unlink(path);
		free(path);
	}
}

static void test_memory_leak_stress(void) {
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

	int iterations = 100;
	bool under_valgrind = getenv("VALGRIND") || getenv("RUNNING_ON_VALGRIND");
	bool under_emulation = is_emulated();
	if (under_valgrind) {
		iterations = 5;
		printf("  (Valgrind mode: reduced to %d iterations)\n", iterations);
		printf("  Note: Memory growth under valgrind is inflated by instrumentation.\n");
		printf("  Trust valgrind's leak report, not RSS growth.\n");
	}
	if (under_emulation) {
		printf("  (QEMU emulation detected: memory thresholds are informational only)\n");
	}

	// Verify the full pipeline works (preprocess + transpile via fork).
	{
		PrismResult result = prism_transpile_file(path, features);
		CHECK_EQ(result.status, PRISM_OK, "full pipeline sanity check");
		prism_free(&result);
		prism_reset();
	}

	unlink(path);
	free(path);

	const char *stress_src =
	    "typedef struct { int x; int y; } Point;\n"
	    "int main(void) {\n"
	    "    Point p;\n"
	    "    int arr[10];\n"
	    "    {\n"
	    "        defer arr[0] = 0;\n"
	    "        int local;\n"
	    "        for (int i; i < 10; i++) {\n"
	    "            arr[i] = i;\n"
	    "        }\n"
	    "    }\n"
	    "    return 0;\n"
	    "}\n";

	// Warmup
	int warmup = iterations < 10 ? 1 : 10;
	for (int i = 0; i < warmup; i++) {
		PrismResult result = prism_transpile_source(stress_src, "stress.c", features);
		prism_free(&result);
		prism_reset();
	}

	long baseline_mem = get_memory_usage_kb();

	for (int i = 0; i < iterations; i++) {
		PrismResult result = prism_transpile_source(stress_src, "stress.c", features);
		if (result.status != PRISM_OK) {
			printf("[FAIL] stress iteration %d failed: %s\n", i,
			       result.error_msg ? result.error_msg : "unknown");
			failed++;
			prism_free(&result);
			break;
		}
		prism_free(&result);
		prism_reset();
	}

	long final_mem = get_memory_usage_kb();
	long mem_growth = final_mem - baseline_mem;

	printf("  Memory after warmup: %ld KB\n", baseline_mem);
	printf("  Memory after %d iterations: %ld KB\n", iterations, final_mem);
	printf("  Memory growth: %ld KB\n", mem_growth);

	bool mem_unreliable = under_valgrind || under_emulation;

	total++;
	if (mem_unreliable) {
		if (under_valgrind)
			printf("[PASS] memory test (valgrind mode - check leak summary above)\n");
		else
			printf("[PASS] memory test (emulation mode - RSS is unreliable under QEMU)\n");
		if (mem_growth >= 1024)
			printf("  [info] RSS grew %ld KB — expected under %s\n",
			       mem_growth, under_valgrind ? "valgrind" : "QEMU emulation");
		passed++;
	} else if (mem_growth < 1024) {
		passed++;
		printf("[PASS] memory growth under 1MB after warmup\n");
	} else {
		printf("[WARN] memory growth %ld KB - may indicate leak\n", mem_growth);
		printf("       Growth per iteration: %.1f KB\n", (float)mem_growth / iterations);
		if (mem_growth / iterations < 10) {
			passed++;
			printf("[PASS] growth rate acceptable (< 10KB/iteration)\n");
		} else {
			failed++;
			printf("[FAIL] excessive memory growth detected\n");
		}
	}

	passed++;
	total++;
	printf("[PASS] completed %d stress iterations\n", iterations);
}

#ifndef _WIN32
static int run_exec_argv(char *const argv[]) {
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}

	int status = 0;
	if (waitpid(pid, &status, 0) < 0) return -1;
	if (!WIFEXITED(status)) return -1;
	return WEXITSTATUS(status);
}

static void check_transpiled_output_compiles(const char *output, const char *stdflag,
					 const char *name) {
	char *out_path = create_temp_file(output);
	CHECK(out_path != NULL, "write transpiled output");
	if (!out_path) return;

	int status = -1;
	const char *cc = getenv("CC");
	if (!cc || !*cc) cc = "cc";
	char *argv[] = {(char *)cc, (char *)stdflag, "-fsyntax-only", out_path, NULL};
	status = run_exec_argv(argv);
	CHECK_EQ(status, 0, name);
	unlink(out_path);
	free(out_path);
}
#endif

static void test_cli_large_passthrough_argv(void) {
	printf("\n--- CLI Large Passthrough Argv ---\n");

#ifdef _WIN32
	passed++;
	total++;
	printf("[PASS] cli large argv regression skipped on Windows\n");
#else
	char tmpdir[] = "/tmp/prism_cli_argv_XXXXXX";
	char *dir = mkdtemp(tmpdir);
	CHECK(dir != NULL, "create temp dir for cli argv regression");
	if (!dir) return;

	char prism_bin[PATH_MAX];
	char empty_src[PATH_MAX];
	char empty_obj[PATH_MAX];
	char output_obj[PATH_MAX];
	enum { OBJ_REPEAT = 640 };
	char **argv = NULL;
	int status = -1;
	const char *cc = getenv("CC");
	if (!cc || !*cc) cc = "cc";

	snprintf(prism_bin, sizeof(prism_bin), "%s/prism_cli_test", dir);
	snprintf(empty_src, sizeof(empty_src), "%s/empty.c", dir);
	snprintf(empty_obj, sizeof(empty_obj), "%s/empty.o", dir);
	snprintf(output_obj, sizeof(output_obj), "%s/out.o", dir);

	FILE *f = fopen(empty_src, "w");
	CHECK(f != NULL, "create empty source for cli argv regression");
	if (f) {
		fputs("/* empty translation unit */\n", f);
		fclose(f);

		char *build_prism[] = {(char *)cc, "prism.c", "-O0", "-g", "-o", prism_bin, NULL};
		status = run_exec_argv(build_prism);
		CHECK_EQ(status, 0, "build prism binary for cli argv regression");

		if (status == 0) {
			char *build_obj[] = {(char *)cc, "-c", empty_src, "-o", empty_obj, NULL};
			status = run_exec_argv(build_obj);
			CHECK_EQ(status, 0, "build empty object for cli argv regression");
		}

		if (status == 0) {
			argv = calloc((size_t)OBJ_REPEAT + 6, sizeof(*argv));
			CHECK(argv != NULL, "allocate cli argv regression command");
			if (argv) {
				argv[0] = prism_bin;
				argv[1] = "-r";
				argv[2] = "-o";
				argv[3] = output_obj;
				for (int i = 0; i < OBJ_REPEAT; i++) argv[4 + i] = empty_obj;
				argv[4 + OBJ_REPEAT] = NULL;

				status = run_exec_argv(argv);
				CHECK_EQ(status, 0, "large passthrough argv exits cleanly");
				CHECK(access(output_obj, F_OK) == 0,
				      "large passthrough argv produced output");
			}
		}
	}
	free(argv);
	remove(output_obj);
	remove(empty_obj);
	remove(empty_src);
	remove(prism_bin);
	rmdir(dir);
#endif
}

static void test_unicode_digraph_lib(void) {
	printf("\n--- Unicode/Digraph Lib Tests ---\n");

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

	if (result.output) {
		CHECK(strstr(result.output, "{") != NULL, "digraph <% translated to {");
		CHECK(strstr(result.output, "[") != NULL, "digraph <: translated to [");
	}

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_complex_code(void) {
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

static void test_lib_defaults(void) {
	printf("\n--- Defaults Test ---\n");

	PrismFeatures features = prism_defaults();

	CHECK(features.defer == true, "default defer=true");
	CHECK(features.zeroinit == true, "default zeroinit=true");
	CHECK(features.line_directives == true, "default line_directives=true");
	CHECK(features.warn_safety == false, "default warn_safety=false");
	CHECK(features.flatten_headers == true, "default flatten_headers=true");
}

static void test_double_free_protection(void) {
	printf("\n--- Double-Free Protection Test ---\n");

	const char *code = "int main(void) { return 0; }\n";
	char *path = create_temp_file(code);

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);

	CHECK_EQ(result.status, PRISM_OK, "simple transpile for double-free test");

	prism_free(&result);
	CHECK(result.output == NULL, "first free nulls output");
	CHECK(result.error_msg == NULL, "first free nulls error_msg");

	prism_free(&result);
	CHECK(result.output == NULL, "second free safe");

	passed++;
	total++;
	printf("[PASS] double prism_free() is safe\n");

	unlink(path);
	free(path);
}

static void test_repeated_reset(void) {
	printf("\n--- Repeated Reset Test ---\n");

	PrismFeatures features = prism_defaults();

	const char *codes[] = {
	    "#include <stdio.h>\nint main(void) { printf(\"hello\"); return 0; }\n",
	    "typedef int MyInt; MyInt add(MyInt a, MyInt b) { return a + b; }\n",
	    "struct Point { int x, y; }; int main(void) { struct Point p; return 0; }\n",
	    "int factorial(int n) { return n <= 1 ? 1 : n * factorial(n-1); }\n",
	};

	for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
		char *path = create_temp_file(codes[i]);
		if (!path) continue;

		PrismResult result = prism_transpile_file(path, features);

		char name[64];
		snprintf(name, sizeof(name), "reset+transpile %zu", i + 1);
		CHECK_EQ(result.status, PRISM_OK, name);
		CHECK(result.output != NULL, "output not NULL after reset");
		CHECK(result.output_len > 0, "output has content after reset");

		prism_free(&result);
		prism_reset();

		unlink(path);
		free(path);
	}

	const char *final_code = "int main(void) { int x; { defer (void)0; } return x; }\n";
	char *path = create_temp_file(final_code);

	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "final transpile after resets");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_error_recovery_no_exit(void) {
	printf("\n--- Error Recovery Tests (no exit) ---\n");

	PrismFeatures features = prism_defaults();

	const char *invalid_code1 =
	    "int main(void) {\n"
	    "    for (int i = 0; defer (void)0; i++) { }\n"
	    "    return 0;\n"
	    "}\n";

	char *path1 = create_temp_file(invalid_code1);
	if (path1) {
		PrismResult result = prism_transpile_file(path1, features);
		CHECK(result.status != PRISM_OK, "syntax error returns error status (not exit)");
		CHECK(result.error_msg != NULL, "error message captured");
		if (result.error_msg) {
			CHECK(strstr(result.error_msg, "defer") != NULL ||
			          strstr(result.error_msg, "control") != NULL,
			      "error message is descriptive");
		}
		prism_free(&result);
		unlink(path1);
		free(path1);
	}

	const char *valid_code = "int main(void) { int x; return x; }\n";
	char *path2 = create_temp_file(valid_code);
	if (path2) {
		PrismResult result = prism_transpile_file(path2, features);
		CHECK_EQ(result.status, PRISM_OK, "transpiler recovers after error");
		CHECK(result.output != NULL, "output generated after recovery");
		prism_free(&result);
		unlink(path2);
		free(path2);
	}

	const char *errors[] = {
	    "int main(void) { for(; defer 0;) {} return 0; }\n",
	    "int main(void) { if (1) defer (void)0; return 0; }\n",
	};

	for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
		char *path = create_temp_file(errors[i]);
		if (path) {
			PrismResult result = prism_transpile_file(path, features);
			char name[64];
			snprintf(name, sizeof(name), "error %zu doesn't kill process", i + 1);
			CHECK(result.status != PRISM_OK, name);
			prism_free(&result);
			unlink(path);
			free(path);
		}
	}

	char *path3 = create_temp_file("int main(void) { return 42; }\n");
	if (path3) {
		PrismResult result = prism_transpile_file(path3, features);
		CHECK_EQ(result.status, PRISM_OK, "process still alive after multiple errors");
		prism_free(&result);
		unlink(path3);
		free(path3);
	}
}

static void test_lib_defer_break_continue_rejected(void) {
	printf("\n--- Defer Break/Continue Rejection Tests ---\n");

	PrismFeatures features = prism_defaults();

	// Test 1: bare 'break' inside defer
	const char *code_break_bare =
	    "int main(void) {\n"
	    "    for (int i = 0; i < 10; i++) {\n"
	    "        defer break;\n"
	    "    }\n"
	    "    return 0;\n"
	    "}\n";
	char *path = create_temp_file(code_break_bare);
	if (path) {
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

	// Test 2: bare 'continue' inside defer
	const char *code_cont_bare =
	    "int main(void) {\n"
	    "    for (int i = 0; i < 10; i++) {\n"
	    "        defer continue;\n"
	    "    }\n"
	    "    return 0;\n"
	    "}\n";
	path = create_temp_file(code_cont_bare);
	if (path) {
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

	// Test 3: break inside braced defer body
	const char *code_break_braced =
	    "int main(void) {\n"
	    "    for (int i = 0; i < 10; i++) {\n"
	    "        defer { (void)0; break; };\n"
	    "    }\n"
	    "    return 0;\n"
	    "}\n";
	path = create_temp_file(code_break_braced);
	if (path) {
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

	// Test 4: continue inside braced defer body
	const char *code_cont_braced =
	    "int main(void) {\n"
	    "    for (int i = 0; i < 10; i++) {\n"
	    "        defer { (void)0; continue; };\n"
	    "    }\n"
	    "    return 0;\n"
	    "}\n";
	path = create_temp_file(code_cont_braced);
	if (path) {
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

	// Test 5: recovery after break/continue errors
	const char *valid_code = "int main(void) { int x; return x; }\n";
	path = create_temp_file(valid_code);
	if (path) {
		PrismResult result = prism_transpile_file(path, features);
		CHECK_EQ(result.status, PRISM_OK, "transpiler recovers after break/continue rejection");
		prism_free(&result);
		unlink(path);
		free(path);
	}

	// Test 6: break inside for loop inside defer — ALLOWED
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
	if (path) {
		PrismResult result = prism_transpile_file(path, features);
		CHECK(result.status == PRISM_OK, "break in for inside defer: allowed");
		CHECK(result.error_msg == NULL, "break in for inside defer: no error");
		prism_free(&result);
		unlink(path);
		free(path);
	}

	// Test 7: continue inside while loop inside defer — ALLOWED
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
	if (path) {
		PrismResult result = prism_transpile_file(path, features);
		CHECK(result.status == PRISM_OK, "continue in while inside defer: allowed");
		CHECK(result.error_msg == NULL, "continue in while inside defer: no error");
		prism_free(&result);
		unlink(path);
		free(path);
	}

	// Test 8: break inside switch inside defer — ALLOWED
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
	if (path) {
		PrismResult result = prism_transpile_file(path, features);
		CHECK(result.status == PRISM_OK, "break in switch inside defer: allowed");
		CHECK(result.error_msg == NULL, "break in switch inside defer: no error");
		prism_free(&result);
		unlink(path);
		free(path);
	}

	// Test 9: continue inside switch (no loop) inside defer — REJECTED
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
	if (path) {
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

	// Test 10: break inside do-while inside defer — ALLOWED
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
	if (path) {
		PrismResult result = prism_transpile_file(path, features);
		CHECK(result.status == PRISM_OK, "break in do-while inside defer: allowed");
		CHECK(result.error_msg == NULL, "break in do-while inside defer: no error");
		prism_free(&result);
		unlink(path);
		free(path);
	}
}

static void test_lib_array_orelse_rejected(void) {
	printf("\n--- Array Orelse Rejection Tests ---\n");

	PrismFeatures features = prism_defaults();

	const char *code_arr_block =
	    "int main(void) {\n"
	    "    int arr[] = {1, 2} orelse { return 1; };\n"
	    "    return arr[0];\n"
	    "}\n";
	char *path = create_temp_file(code_arr_block);
	if (path) {
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

	const char *code_const_arr =
	    "int main(void) {\n"
	    "    const int arr[] = {1, 2} orelse (int[]){3, 4};\n"
	    "    return arr[0];\n"
	    "}\n";
	path = create_temp_file(code_const_arr);
	if (path) {
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

	const char *code_arr_expr =
	    "int main(void) {\n"
	    "    int arr[] = {1, 2} orelse (int[]){3, 4};\n"
	    "    return arr[0];\n"
	    "}\n";
	path = create_temp_file(code_arr_expr);
	if (path) {
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

static void test_deep_struct_nesting_walker(void) {
	printf("\n--- Deep Struct Nesting Walker Tests ---\n");

	PrismFeatures features = prism_defaults();

	char code[8192];
	int pos = 0;
	pos += snprintf(code + pos, sizeof(code) - pos,
	                "#include <stdio.h>\n"
	                "void func(int flag) {\n"
	                "    struct Deep {\n");

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
	if (path) {
		PrismResult result = prism_transpile_file(path, features);
		CHECK(result.status == PRISM_OK, "deep struct nesting: transpiles OK");
		CHECK(result.error_msg == NULL, "deep struct nesting: no error");
		prism_free(&result);
		unlink(path);
		free(path);
	}

	// Test 2: goto skips over a variable declaration with deep nesting
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
	if (path) {
		PrismResult result = prism_transpile_file(path, features);
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

static void test_lib_c23_attr_void_function(void) {
	printf("\n--- C23 Attribute Void Function Tests ---\n");

	PrismFeatures features = prism_defaults();

	const char *code_c23_void =
	    "void [[deprecated]] func(void) {\n"
	    "    defer (void)0;\n"
	    "    return;\n"
	    "}\n"
	    "int main(void) { func(); return 0; }\n";
	char *path = create_temp_file(code_c23_void);
	if (path) {
		PrismResult result = prism_transpile_file(path, features);
		CHECK(result.status == PRISM_OK, "C23 void [[attr]] func: transpiles OK");
		if (result.output) {
			CHECK(strstr(result.output, "_Prism_ret") == NULL,
			      "C23 void [[attr]] func: no _prism_ret generated");
			CHECK(strstr(result.output, "[[deprecated]]") != NULL,
			      "C23 void [[attr]] func: attribute preserved");
		}
		prism_free(&result);
		unlink(path);
		free(path);
	}

	const char *code_multi_attr =
	    "void [[deprecated]] [[maybe_unused]] func2(void) {\n"
	    "    defer (void)0;\n"
	    "    return;\n"
	    "}\n"
	    "int main(void) { func2(); return 0; }\n";
	path = create_temp_file(code_multi_attr);
	if (path) {
		PrismResult result = prism_transpile_file(path, features);
		CHECK(result.status == PRISM_OK, "C23 void multi [[attr]] func: transpiles OK");
		if (result.output)
			CHECK(strstr(result.output, "_Prism_ret") == NULL,
			      "C23 void multi [[attr]] func: no _prism_ret");
		prism_free(&result);
		unlink(path);
		free(path);
	}
}

static void test_lib_generic_array_not_vla(void) {
	printf("\n--- _Generic Array Not VLA Tests ---\n");

	PrismFeatures features = prism_defaults();

	const char *code_generic_arr =
	    "int main(void) {\n"
	    "    int x = 0;\n"
	    "    int arr[_Generic(x, int: 10, default: 20)];\n"
	    "    return arr[0];\n"
	    "}\n";
	char *path = create_temp_file(code_generic_arr);
	if (path) {
		PrismResult result = prism_transpile_file(path, features);
		CHECK(result.status == PRISM_OK, "_Generic array: transpiles OK");
		if (result.output) {
			// _Generic in array bounds is conservatively treated as VLA
			// to prevent fatal = {0} when a branch selects a runtime var.
			CHECK(strstr(result.output, "memset") != NULL,
			      "_Generic array: uses memset (conservative VLA)");
			CHECK(strstr(result.output, "= {0}") == NULL,
			      "_Generic array: no brace-init (would fail if VLA)");
		}
		prism_free(&result);
		unlink(path);
		free(path);
	}
}

static void test_fnptr_return_type_capture(void) {
	printf("\n--- Function Pointer Return Type Capture Tests ---\n");

	PrismFeatures features = prism_defaults();

	const char *code_fnptr =
	    "static void my_fn(void) {}\n"
	    "void (*get_callback(void))(void) {\n"
	    "    defer (void)0;\n"
	    "    return my_fn;\n"
	    "}\n"
	    "int main(void) { get_callback()(); return 0; }\n";
	char *path = create_temp_file(code_fnptr);
	if (path) {
		PrismResult result = prism_transpile_file(path, features);
		CHECK(result.status == PRISM_OK, "bug_r2: fnptr return transpiles OK");
		if (result.output) {
			CHECK(strstr(result.output, "__auto_type") == NULL,
			      "bug_r2: fnptr return has no __auto_type");
			CHECK(strstr(result.output, "_Prism_ret") != NULL,
			      "bug_r2: fnptr return has _prism_ret (captured type)");
		}
		prism_free(&result);
		unlink(path);
		free(path);
	}

	const char *code_typedef =
	    "typedef void (*callback_t)(void);\n"
	    "static void my_fn(void) {}\n"
	    "callback_t get_cb(void) {\n"
	    "    defer (void)0;\n"
	    "    return my_fn;\n"
	    "}\n"
	    "int main(void) { get_cb()(); return 0; }\n";
	path = create_temp_file(code_typedef);
	if (path) {
		PrismResult result = prism_transpile_file(path, features);
		CHECK(result.status == PRISM_OK, "bug_r2: typedef fnptr return transpiles OK");
		if (result.output) {
			CHECK(strstr(result.output, "__auto_type") == NULL,
			      "bug_r2: typedef fnptr return has no __auto_type");
		}
		prism_free(&result);
		unlink(path);
		free(path);
	}

	const char *code_arrptr =
	    "static int arr[5] = {1,2,3,4,5};\n"
	    "int (*get_arr(void))[5] {\n"
	    "    defer (void)0;\n"
	    "    return &arr;\n"
	    "}\n"
	    "int main(void) { return (*get_arr())[0] - 1; }\n";
	path = create_temp_file(code_arrptr);
	if (path) {
		PrismResult result = prism_transpile_file(path, features);
		CHECK(result.status == PRISM_OK, "bug_r2: array ptr return transpiles OK");
		if (result.output) {
			CHECK(strstr(result.output, "__auto_type") == NULL,
			      "bug_r2: array ptr return has no __auto_type");
		}
		prism_free(&result);
		unlink(path);
		free(path);
	}
}

static void test_line_directive_escaped_quote(void) {
	printf("\n--- Line Directive Escaped Quote Tests ---\n");

	PrismFeatures features = prism_defaults();

	const char *code =
	    "#line 1 \"foo\\\"bar.c\"\n"
	    "int main(void) {\n"
	    "    defer (void)0;\n"
	    "    return 0;\n"
	    "}\n";
	char *path = create_temp_file(code);
	if (path) {
		PrismResult result = prism_transpile_file(path, features);
		CHECK(result.status == PRISM_OK, "bug_r3: escaped quote #line transpiles OK");
		if (result.output) {
			CHECK(strstr(result.output, "foo\\\\\\\"bar.c") == NULL,
			      "bug_r3: no triple-escaped filename in output");
			CHECK(strstr(result.output, "foo\\\"bar.c") != NULL,
			      "bug_r3: properly escaped filename in output");
		}
		prism_free(&result);
		unlink(path);
		free(path);
	}
}

static void test_nested_fnptr_return_type(void) {
	printf("\n--- Nested Function Pointer Return Type ---\n");

	// Two-level nested function pointer: foo returns pointer-to-function(int)
	// returning pointer-to-function(double) returning int.
	// Without the fix, try_capture_func_return_type fails to recurse through
	// the nested parenthesized declarator and falls back to __auto_type.
	const char *code =
	    "int (*(*foo(void))(int))(double) {\n"
	    "    defer (void)0;\n"
	    "    return 0;\n"
	    "}\n";

	PrismResult r = prism_transpile_source(code, "nested_fnptr.c", prism_defaults());
	CHECK(r.status == PRISM_OK, "nested fnptr ret: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "__auto_type") == NULL,
		      "nested fnptr ret: no __auto_type (type captured)");
		CHECK(strstr(r.output, "_Prism_ret_t_") != NULL,
		      "nested fnptr ret: has typedef for return type");
	}
	prism_free(&r);
}

static void test_generic_defer_passthrough(void) {
	printf("\n--- _Generic Defer Passthrough ---\n");

	// defer inside _Generic must NOT be processed by the transpiler.
	// Without the fix, handle_defer_keyword fires, corrupts the stream,
	// and emits cleanup code at end of scope.
	const char *code =
	    "void clean_int(int x) { (void)x; }\n"
	    "#define CLEAN(x) _Generic((x), int: defer clean_int(x), default: 0)\n"
	    "void test(void) {\n"
	    "    int val = 42;\n"
	    "    CLEAN(val);\n"
	    "}\n";

	PrismResult r = prism_transpile_source(code, "generic_defer.c", prism_defaults());
	CHECK(r.status == PRISM_OK, "generic defer: transpiles OK");
	if (r.output) {
		// The defer should pass through untouched — no brace wrapping
		CHECK(strstr(r.output, "unterminated") == NULL,
		      "generic defer: no unterminated error");
		// The _Generic expression should remain intact in the output
		CHECK(strstr(r.output, "_Generic") != NULL,
		      "generic defer: _Generic preserved in output");
	}
	prism_free(&r);
}

static void test_generic_decl_parenthesized_passthrough(void) {
	printf("\n--- _Generic Parenthesized Decl Passthrough ---\n");

	const char *code =
	    "#include <stddef.h>\n"
	    "#include <wchar.h>\n"
	    "#define __glibc_const_generic(PTR, CTYPE, CALL) \\\n"
	    "    _Generic(0 ? (PTR) : (void *)1, const void *: (CTYPE)(CALL), default: CALL)\n"
	    "#define bsearch(KEY, BASE, NMEMB, SIZE, COMPAR) \\\n"
	    "    __glibc_const_generic((BASE), const void *, bsearch(KEY, BASE, NMEMB, SIZE, COMPAR))\n"
	    "#define wmemchr(S, C, N) \\\n"
	    "    __glibc_const_generic((S), const wchar_t *, wmemchr(S, C, N))\n"
	    "extern void *(bsearch)(const void *key, const void *base, size_t nmemb, size_t size,\n"
	    "                       int (*compar)(const void *, const void *));\n"
	    "extern wchar_t *(wmemchr)(const wchar_t *s, wchar_t c, size_t n);\n";

	PrismResult r = prism_transpile_source(code, "generic_decl_passthrough.c", prism_defaults());
	CHECK(r.status == PRISM_OK, "generic decl passthrough: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "extern void *(bsearch)(") != NULL,
		      "generic decl passthrough: keeps grouped bsearch name");
		CHECK(strstr(r.output, "extern wchar_t *(wmemchr)(") != NULL,
		      "generic decl passthrough: keeps grouped wmemchr name");
		CHECK(strstr(r.output, "extern void *bsearch(") == NULL,
		      "generic decl passthrough: does not flatten bsearch declarator");
		CHECK(strstr(r.output, "extern wchar_t *wmemchr(") == NULL,
		      "generic decl passthrough: does not flatten wmemchr declarator");
	}
	prism_free(&r);
}

static void test_file_c23_generic_decl_parentheses(void) {
	printf("\n--- C23 Generic Decl Parentheses ---\n");

#ifdef _WIN32
	passed++;
	total++;
	printf("[PASS] C23 generic decl regression skipped on Windows\n");
#else
	const char *code =
	    "#include <stdlib.h>\n"
	    "#include <wchar.h>\n"
	    "#define _GL_EXTERN_C extern\n"
	    "#define _GL_FUNCDECL_SYS_NAME(func) (func)\n"
	    "_GL_EXTERN_C void *_GL_FUNCDECL_SYS_NAME(bsearch)\n"
	    "  (const void *key, const void *base, size_t nmemb, size_t size,\n"
	    "   int (*compar)(const void *, const void *));\n"
	    "_GL_EXTERN_C wchar_t *_GL_FUNCDECL_SYS_NAME(wmemchr)\n"
	    "  (const wchar_t *s, wchar_t c, size_t n);\n"
	    "int main(void) { return 0; }\n";
	char *path = create_temp_file(code);
	CHECK(path != NULL, "c23 generic decl: create temp file");
	if (path) {
		PrismResult r = prism_transpile_file(path, prism_defaults());
		CHECK(r.status == PRISM_OK, "c23 generic decl: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "extern void *(bsearch)") != NULL,
			      "c23 generic decl: keeps grouped bsearch name in file path");
			CHECK(strstr(r.output, "extern wchar_t *(wmemchr)") != NULL,
			      "c23 generic decl: keeps grouped wmemchr name in file path");
			CHECK(strstr(r.output, "extern void *bsearch(") == NULL,
			      "c23 generic decl: no flattened bsearch in file path");
			CHECK(strstr(r.output, "extern wchar_t *wmemchr(") == NULL,
			      "c23 generic decl: no flattened wmemchr in file path");
			check_transpiled_output_compiles(
			    r.output, "-std=gnu2x",
			    "c23 generic decl: transpiled output compiles in gnu2x");
		}
		prism_free(&r);
		unlink(path);
		free(path);
	}
#endif
}

static void test_file_c23_generic_decl_macro_layers(void) {
	printf("\n--- C23 Generic Decl Macro Layers ---\n");

#ifdef _WIN32
	passed++;
	total++;
	printf("[PASS] C23 generic decl macro layers skipped on Windows\n");
#else
	const char *code =
	    "#include <stddef.h>\n"
	    "#include <wchar.h>\n"
	    "#define _GL_EXTERN_C_FUNC\n"
	    "#define _GL_FUNCDECL_SYS_NAME(func) (func)\n"
	    "#define _GL_FUNCDECL_SYS(func, rettype, parameters, ...) \\\n"
	    "  _GL_EXTERN_C_FUNC __VA_ARGS__ rettype _GL_FUNCDECL_SYS_NAME(func) parameters\n"
	    "#define _GL_ARG_NONNULL(args) __attribute__((nonnull args))\n"
	    "#define _GL_ATTRIBUTE_NOTHROW __attribute__((nothrow))\n"
	    "#define __glibc_const_generic(PTR,CTYPE,CALL) \\\n"
	    "  _Generic(0 ? (PTR) : (void *)1, const void *: (CTYPE)(CALL), default: CALL)\n"
	    "#define bsearch(KEY,BASE,NMEMB,SIZE,COMPAR) \\\n"
	    "  __glibc_const_generic((BASE), const void *, bsearch(KEY, BASE, NMEMB, SIZE, COMPAR))\n"
	    "#define wmemchr(S,C,N) \\\n"
	    "  __glibc_const_generic((S), const wchar_t *, wmemchr(S, C, N))\n"
	    "_GL_FUNCDECL_SYS (bsearch, void *,\n"
	    "                  (const void *key, const void *base, size_t nmemb, size_t size,\n"
	    "                   int (*compar) (const void *, const void *)),\n"
	    "                  _GL_ARG_NONNULL ((5)))\n"
	    "                  _GL_ATTRIBUTE_NOTHROW;\n"
	    "_GL_FUNCDECL_SYS (wmemchr, wchar_t *,\n"
	    "                  (const wchar_t *s, wchar_t c, size_t n),\n"
	    "                  _GL_ARG_NONNULL ((1)))\n"
	    "                  _GL_ATTRIBUTE_NOTHROW;\n";
	char *path = create_temp_file(code);
	CHECK(path != NULL, "c23 generic decl macro layers: create temp file");
	if (path) {
		PrismResult r = prism_transpile_file(path, prism_defaults());
		CHECK(r.status == PRISM_OK, "c23 generic decl macro layers: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "(bsearch)") != NULL,
			      "c23 generic decl macro layers: keeps grouped bsearch name");
			CHECK(strstr(r.output, "(wmemchr)") != NULL,
			      "c23 generic decl macro layers: keeps grouped wmemchr name");
			CHECK(strstr(r.output, "__attribute__((nonnull") != NULL ||
			          strstr(r.output, "__attribute__ ((__nonnull__") != NULL,
			      "c23 generic decl macro layers: attribute survives preprocessing");
			check_transpiled_output_compiles(
			    r.output, "-std=gnu2x",
			    "c23 generic decl macro layers: transpiled output compiles in gnu2x");
		}
		prism_free(&r);
		unlink(path);
		free(path);
	}
#endif
}

static void test_file_c23_generic_decl_macro_indirection(void) {
	printf("\n--- C23 Generic Decl Macro Indirection ---\n");

#ifdef _WIN32
	passed++;
	total++;
	printf("[PASS] C23 generic decl macro indirection skipped on Windows\n");
#else
	const char *code =
	    "#include <stddef.h>\n"
	    "#include <wchar.h>\n"
	    "#define _GL_EXTERN_C_FUNC\n"
	    "#define _GL_FUNCDECL_SYS_NAME(func) (func)\n"
	    "#define _GL_FUNCDECL_SYS_1(func, rettype, parameters, ...) \\\n"
	    "  _GL_EXTERN_C_FUNC __VA_ARGS__ rettype _GL_FUNCDECL_SYS_NAME(func) parameters\n"
	    "#define _GL_FUNCDECL_SYS(func, rettype, parameters, ...) \\\n"
	    "  _GL_FUNCDECL_SYS_1(func, rettype, parameters, __VA_ARGS__)\n"
	    "#define _GL_ARG_NONNULL(args) __attribute__((nonnull args))\n"
	    "#define _GL_ATTRIBUTE_NOTHROW __attribute__((nothrow))\n"
	    "#define __glibc_const_generic(PTR,CTYPE,CALL) \\\n"
	    "  _Generic(0 ? (PTR) : (void *)1, const void *: (CTYPE)(CALL), default: CALL)\n"
	    "#define wmemchr(S,C,N) \\\n"
	    "  __glibc_const_generic((S), const wchar_t *, wmemchr(S, C, N))\n"
	    "_GL_FUNCDECL_SYS (wmemchr, wchar_t *,\n"
	    "                  (const wchar_t *s, wchar_t c, size_t n),\n"
	    "                  _GL_ARG_NONNULL ((1)))\n"
	    "                  _GL_ATTRIBUTE_NOTHROW;\n";
	char *path = create_temp_file(code);
	CHECK(path != NULL, "c23 generic decl macro indirection: create temp file");
	if (path) {
		PrismResult r = prism_transpile_file(path, prism_defaults());
		CHECK(r.status == PRISM_OK, "c23 generic decl macro indirection: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "(wmemchr)") != NULL,
			      "c23 generic decl macro indirection: keeps grouped wmemchr name");
			CHECK(strstr(r.output, "wchar_t *_Generic") == NULL,
			      "c23 generic decl macro indirection: no genericized wmemchr declarator");
			check_transpiled_output_compiles(
			    r.output, "-std=gnu2x",
			    "c23 generic decl macro indirection: transpiled output compiles in gnu2x");
		}
		prism_free(&r);
		unlink(path);
		free(path);
	}
#endif
}

static void test_file_c23_generic_decl_plain_redeclaration(void) {
	printf("\n--- C23 Generic Decl Plain Redeclaration ---\n");

#ifdef _WIN32
	passed++;
	total++;
	printf("[PASS] C23 generic decl plain redeclaration skipped on Windows\n");
#else
	const char *code =
	    "#include <stdlib.h>\n"
	    "#include <wchar.h>\n"
	    "extern void *bsearch (const void *__key, const void *__base, size_t __nmemb, size_t __size,\n"
	    "                      int (*__compare) (const void *, const void *));\n"
	    "extern wchar_t *wmemchr (const wchar_t *__s, wchar_t __wc, size_t __n);\n"
	    "int main(void) { return 0; }\n";
	char *path = create_temp_file(code);
	CHECK(path != NULL, "c23 generic decl plain redecl: create temp file");
	if (path) {
		PrismResult r = prism_transpile_file(path, prism_defaults());
		CHECK(r.status == PRISM_OK, "c23 generic decl plain redecl: transpiles OK");
		if (r.output) {
			bool valid_bsearch_decl =
			    strstr(r.output, "extern void *(bsearch)") != NULL ||
			    strstr(r.output, "extern void *bsearch (") != NULL ||
			    strstr(r.output, "extern void *bsearch(") != NULL;
			bool valid_wmemchr_decl =
			    strstr(r.output, "extern wchar_t *(wmemchr)") != NULL ||
			    strstr(r.output, "extern wchar_t *wmemchr (") != NULL ||
			    strstr(r.output, "extern wchar_t *wmemchr(") != NULL;
			CHECK(strstr(r.output, "extern void *_Generic") == NULL,
			      "c23 generic decl plain redecl: no genericized bsearch declarator");
			CHECK(strstr(r.output, "extern wchar_t *_Generic") == NULL,
			      "c23 generic decl plain redecl: no genericized wmemchr declarator");
			CHECK(valid_bsearch_decl,
			      "c23 generic decl plain redecl: keeps bsearch in valid declarator form");
			CHECK(valid_wmemchr_decl,
			      "c23 generic decl plain redecl: keeps wmemchr in valid declarator form");
			check_transpiled_output_compiles(
			    r.output, "-std=gnu2x",
			    "c23 generic decl plain redecl: transpiled output compiles in gnu2x");
		}
		prism_free(&r);
		unlink(path);
		free(path);
	}
#endif
}

static void test_raw_c23_attr_interleave(void) {
	printf("\n--- Raw C23 Attr Interleave ---\n");

	// 'raw [[maybe_unused]] int x;' must consume the raw keyword,
	// preserve the C23 attribute, and suppress zero-init.
	const char *code =
	    "void f(void) {\n"
	    "    raw [[maybe_unused]] int x;\n"
	    "}\n";

	PrismResult r = prism_transpile_source(code, "c23_attr_decl.c", prism_defaults());
	CHECK(r.status == PRISM_OK, "raw c23 attr: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "raw") == NULL,
		      "raw c23 attr: 'raw' keyword consumed");
		CHECK(strstr(r.output, "[[maybe_unused]]") != NULL,
		      "raw c23 attr: attribute preserved");
		CHECK(strstr(r.output, "= 0") == NULL,
		      "raw c23 attr: no zero-init");
	}
	prism_free(&r);
}

static void test_c23_auto_orelse(void) {
	printf("\n--- C23 Auto Orelse ---\n");

	// C23 'auto' used for type inference with orelse must be processed
	// through the declaration pipeline, not the bare expression path.
	const char *code =
	    "int *try_fn(void) { return 0; }\n"
	    "int test(void) {\n"
	    "    auto ptr = try_fn() orelse return -1;\n"
	    "    return *ptr;\n"
	    "}\n";

	PrismResult r = prism_transpile_source(code, "c23_auto_decl.c", prism_defaults());
	CHECK(r.status == PRISM_OK, "c23 auto orelse: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "if (!ptr)") != NULL,
		      "c23 auto orelse: orelse expanded correctly");
		// Must not contain the raw orelse keyword in output
		CHECK(strstr(r.output, "orelse") == NULL,
		      "c23 auto orelse: orelse keyword consumed");
	}
	prism_free(&r);
}

static void test_cross_compile_msvc_ret_type(void) {
	printf("\n--- Cross-Compile MSVC Ret Type ---\n");

	// Anonymous struct return with defer forces the ret type fallback path.
	// When compiler is "cl", output must use "void *" instead of __auto_type.
	const char *code =
	    "struct { int x; } make(void) {\n"
	    "    struct { int x; } s;\n"
	    "    s.x = 1;\n"
	    "    defer (void)0;\n"
	    "    return s;\n"
	    "}\n";

	PrismFeatures feat = prism_defaults();
	feat.compiler = "cl";
	PrismResult r = prism_transpile_source(code, "cross_msvc.c", feat);
	CHECK(r.status == PRISM_OK, "cross-compile msvc: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "__auto_type") == NULL,
		      "cross-compile msvc: no __auto_type emitted");
		CHECK(strstr(r.output, "void *") != NULL,
		      "cross-compile msvc: uses void * for ret type");
	}
	prism_free(&r);
}

static void test_typeof_memset_no_shadow(void) {
	printf("\n--- Typeof Memset No Shadow ---\n");

	// When the transpiler emits typeof-based memset loops
	// for VLA zero-init, the loop variables must not shadow user variables.
	const char *code =
	    "void f(int n) {\n"
	    "    char *_p = \"hello\";\n"
	    "    int _i = 42;\n"
	    "    volatile typeof(int[n]) vla;\n"
	    "}\n";

	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "typeof_shadow.c", feat);
	CHECK(r.status == PRISM_OK, "typeof memset shadow: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "_Prism_p_") != NULL,
		      "typeof memset shadow: uses _prism_p_ prefix");
		CHECK(strstr(r.output, "_Prism_i_") != NULL,
		      "typeof memset shadow: uses _prism_i_ prefix");
		// Make sure the old bare _p / _i loop vars are not emitted
		// (user's own _p/_i assignments are fine, but the memset loop
		// must not add another bare 'char *_p' or 'unsigned long _i').
		char *loop_shadow = strstr(r.output, "char *_p =");
		// There should be exactly one occurrence (user's), not two.
		if (loop_shadow) {
			CHECK(strstr(loop_shadow + 1, "char *_p =") == NULL,
			      "typeof memset shadow: no duplicate char *_p");
		}
	}
	prism_free(&r);
}

static void test_c23_constexpr_thread_local(void) {
	printf("\n--- C23 constexpr/thread_local ---\n");

	// constexpr and thread_local must not be fast-rejected by
	// try_zero_init_decl. They need TT_QUALIFIER | TT_SKIP_DECL tags
	// so the declaration pipeline processes them.
	const char *code =
	    "int get(void) { return 42; }\n"
	    "void f(void) {\n"
	    "    thread_local int y;\n"
	    "    int *p = &y orelse return;\n"
	    "}\n";

	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "c23kw.c", feat);
	CHECK(r.status == PRISM_OK, "c23 constexpr/thread_local: transpiles OK");
	if (r.output) {
		// orelse must have been expanded, not rejected
		CHECK(strstr(r.output, "orelse") == NULL,
		      "c23 constexpr/thread_local: orelse consumed");
	}
	prism_free(&r);

	// constexpr declaration should also pass through
	const char *code2 =
	    "void g(void) {\n"
	    "    constexpr int x = 10;\n"
	    "}\n";
	PrismResult r2 = prism_transpile_source(code2, "c23kw2.c", feat);
	CHECK(r2.status == PRISM_OK, "c23 constexpr: transpiles OK");
	if (r2.output) {
		CHECK(strstr(r2.output, "constexpr") != NULL,
		      "c23 constexpr: keyword preserved in output");
	}
	prism_free(&r2);
}

static void test_orelse_backtrack_desync(void) {
	printf("\n--- Orelse Backtracking Desync ---\n");

	// Multi-declarator: variable with orelse + function prototype.
	// The orelse must be expanded for 'a', and the function prototype
	// 'f(void)' must survive as a raw declaration — not get swallowed
	// into the fallback or cause a rollback desync.
	const char *code =
	    "int get(void) { return 42; }\n"
	    "void test(void) {\n"
	    "    int a = get() orelse 0, f(void);\n"
	    "}\n";

	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "bt_desync.c", feat);
	CHECK(r.status == PRISM_OK, "orelse backtrack desync: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "orelse") == NULL,
		      "orelse backtrack desync: orelse consumed");
		// The orelse expansion should produce an if-check for a
		CHECK(strstr(r.output, "if (!") != NULL,
		      "orelse backtrack desync: orelse expanded to if-check");
		// Function prototype must appear as a separate declaration
		CHECK(strstr(r.output, "int f(void);") != NULL,
		      "orelse backtrack desync: prototype preserved");
	}
	prism_free(&r);
}

static void test_orelse_nested_paren_rejected(void) {
	printf("\n--- Orelse Nested Paren Rejected ---\n");

	// orelse inside parentheses cannot be expanded to statement-level
	// if-checks. Must produce a clear error, not silently emit invalid C.
	const char *code =
	    "int get(void) { return 0; }\n"
	    "void f(void) {\n"
	    "    int x = (get() orelse 0);\n"
	    "}\n";

	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "paren_orelse.c", feat);
	CHECK(r.status != PRISM_OK, "orelse nested paren: rejected");
	if (r.error_msg) {
		CHECK(strstr(r.error_msg, "orelse") != NULL,
		      "orelse nested paren: error mentions orelse");
		CHECK(strstr(r.error_msg, "parenthes") != NULL,
		      "orelse nested paren: error mentions parentheses");
	}
	prism_free(&r);
}

static void test_vla_var_not_typedef(void) {
	printf("\n--- VLA Variable Not Typedef ---\n");

	// VLA multiplication misidentified as declaration.
	// After "int x[n];", Prism registered x as TDK_VLA_VAR which fell through
	// to TDF_TYPEDEF in typedef_flags. This made "x * y;" look like a pointer
	// declaration, causing try_zero_init_decl to emit "x * y = 0;".
	{
		const char *code =
		    "void f(int n) {\n"
		    "    int x[n];\n"
		    "    int y = 1;\n"
		    "    x * y;\n"
		    "}\n";
		PrismFeatures feat = prism_defaults();
		PrismResult r = prism_transpile_source(code, "vla_mul.c", feat);
		CHECK(r.status == PRISM_OK, "vla mul: transpiles OK");
		if (r.output) {
			// The multiplication must survive as-is, not become "x * y = 0;"
			CHECK(strstr(r.output, "= 0") == NULL,
			      "vla mul: no spurious zero-init");
			CHECK(strstr(r.output, "x * y") != NULL,
			      "vla mul: multiplication preserved");
		}
		prism_free(&r);
	}

	// Shadow overwrite when VLA variable shadows a typedef.
	// register_decl_shadows first added a shadow (is_shadow=true), then
	// typedef_add_vla_var added a second entry (is_shadow=false) that replaced
	// the shadow in the hash map, making the typedef visible again.
	{
		const char *code =
		    "typedef int T;\n"
		    "void f(int n) {\n"
		    "    int T[n];\n"
		    "    T * p;\n"		// must be multiplication, not pointer decl
		    "}\n";
		PrismFeatures feat = prism_defaults();
		PrismResult r = prism_transpile_source(code, "vla_shadow.c", feat);
		CHECK(r.status == PRISM_OK, "vla shadow: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "= 0") == NULL,
			      "vla shadow: no spurious zero-init");
			CHECK(strstr(r.output, "T * p") != NULL,
			      "vla shadow: multiplication preserved");
		}
		prism_free(&r);
	}
}

static void test_orelse_typedef_cast_comma(void) {
	printf("\n--- Orelse Typedef Cast Comma ---\n");

	// A typedef cast like (size_t)0 after a comma operator must not be
	// mistaken for a parenthesized declarator boundary.
	const char *code =
	    "#include <stddef.h>\n"
	    "int get(void);\n"
	    "int fallback(void);\n"
	    "void f(void) {\n"
	    "    int x = get() orelse fallback(), (size_t)0;\n"
	    "}\n";

	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "td_cast_comma.c", feat);
	CHECK(r.status == PRISM_OK, "typedef cast comma: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "orelse") == NULL,
		      "typedef cast comma: orelse consumed");
		// The (size_t)0 must appear in the output as part of the comma expression
		CHECK(strstr(r.output, "(size_t)0") != NULL,
		      "typedef cast comma: cast expression preserved");
	}
	prism_free(&r);
}

static void test_atomic_typedef_qualifier(void) {
	printf("\n--- Atomic Typedef Qualifier ---\n");

	// _Atomic as a bare qualifier must not short-circuit type parsing.
	// Previously, _Atomic set saw_type=true (via TT_TYPE), so the had_type
	// guard broke out before consuming the typedef name T.
	const char *code =
	    "typedef int T;\n"
	    "void f(void) {\n"
	    "    _Atomic T x;\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "atomic_td.c", feat);
	CHECK(r.status == PRISM_OK, "atomic typedef: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "_Atomic") != NULL,
		      "atomic typedef: _Atomic preserved");
		CHECK(strstr(r.output, "= 0") != NULL || strstr(r.output, "memset") != NULL,
		      "atomic typedef: zero-init applied");
	}
	prism_free(&r);
}

static void test_pragma_in_declarator(void) {
	printf("\n--- Pragma In Declarator ---\n");

	// _Pragma can appear anywhere whitespace can (C99 6.10.9).
	// parse_declarator must skip _Pragma(...) in the pointer/qualifier prefix.
	{
		const char *code =
		    "void f(void) {\n"
		    "    int _Pragma(\"GCC diagnostic ignored \\\"-Wpadded\\\"\") *x;\n"
		    "}\n";
		PrismFeatures feat = prism_defaults();
		PrismResult r = prism_transpile_source(code, "pragma_decl.c", feat);
		CHECK(r.status == PRISM_OK, "pragma decl: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "= 0") != NULL,
			      "pragma decl: zero-init applied");
		}
		prism_free(&r);
	}

	// _Pragma after a comma in multi-declarator must not break find_boundary_comma.
	{
		const char *code =
		    "int get(void) { return 42; }\n"
		    "void f(void) {\n"
		    "    int a = get() orelse 0, _Pragma(\"GCC unroll 4\") b;\n"
		    "}\n";
		PrismFeatures feat = prism_defaults();
		PrismResult r = prism_transpile_source(code, "pragma_comma.c", feat);
		CHECK(r.status == PRISM_OK, "pragma comma: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "pragma comma: orelse consumed");
		}
		prism_free(&r);
	}
}

static void test_chained_sizeof_not_vla(void) {
	printf("\n--- Chained sizeof Not VLA ---\n");

	// "sizeof sizeof x" is a compile-time constant expression (the outer sizeof
	// evaluates the type size_t).  The inner sizeof's operand must not cause
	// the array to be flagged as a VLA.
	const char *code =
	    "int x;\n"
	    "void f(void) {\n"
	    "    int arr[sizeof sizeof x];\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "chain_sizeof.c", feat);
	CHECK(r.status == PRISM_OK, "chained sizeof: transpiles OK");
	if (r.output) {
		// Fixed-size array must get = {0}, not memset (VLA path)
		CHECK(strstr(r.output, "= {0}") != NULL,
		      "chained sizeof: brace-init, not memset");
		CHECK(strstr(r.output, "memset") == NULL,
		      "chained sizeof: no spurious memset");
	}
	prism_free(&r);
}

static void test_pragma_return_type_capture(void) {
	printf("\n--- Pragma Return Type Capture ---\n");

	// _Pragma before a function definition must not prevent return type capture.
	// try_capture_func_return_type must skip _Pragma(...) like it skips attributes.
	const char *code =
	    "_Pragma(\"pack(push,1)\")\n"
	    "int f(void) {\n"
	    "    defer (void)0;\n"
	    "    return 1;\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "pragma_ret.c", feat);
	CHECK(r.status == PRISM_OK, "pragma ret: transpiles OK");
	if (r.output) {
		// If return type was captured correctly, the defer variable should use "int"
		// not __auto_type or void*
		CHECK(strstr(r.output, "int _Prism_ret") != NULL,
		      "pragma ret: exact return type captured");
	}
	prism_free(&r);
}

static void test_c23_attr_comma_boundary(void) {
	printf("\n--- C23 Attr Comma Boundary ---\n");

	// [[maybe_unused]] after a comma must be recognized as starting a new
	// declarator, not as part of a comma operator expression.
	const char *code =
	    "int get(void) { return 42; }\n"
	    "void f(void) {\n"
	    "    int a = get() orelse 0, [[maybe_unused]] b = 1;\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "attr_comma.c", feat);
	CHECK(r.status == PRISM_OK, "c23 attr comma: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "orelse") == NULL,
		      "c23 attr comma: orelse consumed");
		CHECK(strstr(r.output, "[[maybe_unused]]") != NULL,
		      "c23 attr comma: attribute preserved");
	}
	prism_free(&r);
}

static void test_thread_local_goto_exempt(void) {
	printf("\n--- Thread Local Goto Exempt ---\n");

	// thread_local / _Thread_local variables have thread storage duration,
	// so jumping over them with goto is safe (like static).
	{
		const char *code =
		    "void f(void) {\n"
		    "    goto L;\n"
		    "    _Thread_local int x = 5;\n"
		    "    L:;\n"
		    "}\n";
		PrismFeatures feat = prism_defaults();
		PrismResult r = prism_transpile_source(code, "tl_goto1.c", feat);
		CHECK(r.status == PRISM_OK, "goto over _Thread_local: OK");
		prism_free(&r);
	}
	{
		const char *code =
		    "void f(void) {\n"
		    "    goto L;\n"
		    "    thread_local int x = 5;\n"
		    "    L:;\n"
		    "}\n";
		PrismFeatures feat = prism_defaults();
		PrismResult r = prism_transpile_source(code, "tl_goto2.c", feat);
		CHECK(r.status == PRISM_OK, "goto over thread_local: OK");
		prism_free(&r);
	}
}

static void test_vla_heuristic_zombie(void) {
	printf("\n--- VLA Heuristic Zombie ---\n");

	// A VLA variable whose name matches the _t heuristic (e.g. size_t)
	// must not be resurrected as a typedef by is_typedef_like.
	const char *code =
	    "void f(int n) {\n"
	    "    int size_t[n];\n"
	    "    size_t * y;\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "vla_zombie.c", feat);
	CHECK(r.status == PRISM_OK, "vla zombie: transpiles OK");
	if (r.output) {
		// size_t * y must be multiplication, not a pointer declaration
		CHECK(strstr(r.output, "= 0") == NULL,
		      "vla zombie: no spurious zero-init");
		CHECK(strstr(r.output, "size_t * y") != NULL,
		      "vla zombie: multiplication preserved");
	}
	prism_free(&r);
}

static void test_pragma_declarator_zeroinit(void) {
	printf("\n--- Pragma Declarator Zero-Init ---\n");

	// _Pragma between pointer and name: parse_declarator must skip it
	// (emitting when emit=true) without losing the variable or zero-init.
	const char *code =
	    "void f(void) {\n"
	    "    int * _Pragma(\"GCC unroll 4\") x;\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "pragma_zinit.c", feat);
	CHECK(r.status == PRISM_OK, "pragma decl zeroinit: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "= 0") != NULL,
		      "pragma decl zeroinit: zero-init applied");
		CHECK(strstr(r.output, "_Pragma") != NULL,
		      "pragma decl zeroinit: _Pragma preserved");
	}
	prism_free(&r);
}

static void test_sizeof_vla_var_not_skipped(void) {
	printf("\n--- sizeof VLA Variable Not Skipped ---\n");

	// sizeof vla (unparenthesized) where vla is a VLA variable:
	// sizeof evaluates at runtime, so arr[sizeof vla] is itself a VLA.
	const char *code =
	    "void f(int n) {\n"
	    "    int vla[n];\n"
	    "    int arr[sizeof vla];\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "sizeof_vla_var.c", feat);
	CHECK(r.status == PRISM_OK, "sizeof vla var: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "memset(&arr") != NULL,
		      "sizeof vla var: arr gets memset (is VLA)");
		CHECK(strstr(r.output, "arr[sizeof vla] = {0}") == NULL,
		      "sizeof vla var: no brace-init (would fail to compile)");
	}
	prism_free(&r);
}

static void test_c23_attr_cast_comma_boundary(void) {
	printf("\n--- C23 Attr Cast Comma Boundary ---\n");

	// ([[maybe_unused]] int)0 after comma must be recognized as a cast,
	// not a grouped declarator, so find_boundary_comma doesn't stop early.
	const char *code =
	    "int get(void) { return 42; }\n"
	    "int fallback(void) { return 0; }\n"
	    "void f(void) {\n"
	    "    int x = get() orelse fallback(), ([[maybe_unused]] int)0;\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "attr_cast_comma.c", feat);
	CHECK(r.status == PRISM_OK, "c23 attr cast comma: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "orelse") == NULL,
		      "c23 attr cast comma: orelse consumed");
		CHECK(strstr(r.output, "[[maybe_unused]]") != NULL,
		      "c23 attr cast comma: attribute preserved");
	}
	prism_free(&r);
}

static void test_subscript_vla_in_brackets(void) {
	printf("\n--- Subscript VLA In Brackets ---\n");

	// "hello"[x] inside array bounds: the nested [x] contains a runtime
	// variable, so the outer array is a VLA. array_size_is_vla must
	// recursively check nested brackets, not blindly skip them.
	const char *code =
	    "void f(int x) {\n"
	    "    int arr[\"hello\"[x]];\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "subscript_vla.c", feat);
	CHECK(r.status == PRISM_OK, "subscript vla: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "memset") != NULL,
		      "subscript vla: gets memset (is VLA)");
		CHECK(strstr(r.output, "= {0}") == NULL,
		      "subscript vla: no brace-init");
	}
	prism_free(&r);
}

static void test_typeof_index_not_vla(void) {
	printf("\n--- typeof Index Not VLA ---\n");

	// typeof(arr[x]) is an expression index, not a type dimension.
	// The resulting type is a plain int, not a VLA type. With register,
	// the variable must still get zero-init (= {0} or = 0).
	const char *code =
	    "void f(void) {\n"
	    "    int arr[10];\n"
	    "    int x = 3;\n"
	    "    register typeof(arr[x]) temp;\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "typeof_idx.c", feat);
	CHECK(r.status == PRISM_OK, "typeof index: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "temp =") != NULL ||
		      strstr(r.output, "temp;") == NULL,
		      "typeof index: temp gets zero-init");
	}
	prism_free(&r);
}

static void test_pragma_in_array_bounds(void) {
	printf("\n--- Pragma In Array Bounds ---\n");

	// _Pragma inside array bounds must not be treated as a variable.
	// Also, _Pragma between sizeof and its operand must not consume the
	// operand's primary token slot.
	{
		const char *code =
		    "void f(void) {\n"
		    "    int arr[_Pragma(\"GCC unroll 4\") 10];\n"
		    "}\n";
		PrismFeatures feat = prism_defaults();
		PrismResult r = prism_transpile_source(code, "pragma_arr.c", feat);
		CHECK(r.status == PRISM_OK, "pragma in bounds: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "= {0}") != NULL,
			      "pragma in bounds: brace-init (not VLA)");
			CHECK(strstr(r.output, "memset") == NULL,
			      "pragma in bounds: no memset");
		}
		prism_free(&r);
	}
	{
		const char *code =
		    "int x;\n"
		    "void f(void) {\n"
		    "    int arr2[sizeof _Pragma(\"GCC ivdep\") x];\n"
		    "}\n";
		PrismFeatures feat = prism_defaults();
		PrismResult r = prism_transpile_source(code, "pragma_sizeof.c", feat);
		CHECK(r.status == PRISM_OK, "pragma sizeof: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "= {0}") != NULL,
			      "pragma sizeof: brace-init (not VLA)");
			CHECK(strstr(r.output, "memset") == NULL,
			      "pragma sizeof: no memset");
		}
		prism_free(&r);
	}
}

static void test_asm_register_zeroinit(void) {
	printf("\n--- ASM Register Zero-Init ---\n");

	// GNU asm register pinning: parse_declarator must skip asm("reg")
	// so that is_var_declaration sees '=' or ';', not '__asm__'.
	const char *code =
	    "void f(void) {\n"
	    "    register int core_id __asm__(\"r12\");\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "asm_reg.c", feat);
	CHECK(r.status == PRISM_OK, "asm reg: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "= 0") != NULL,
		      "asm reg: zero-init applied");
	}
	prism_free(&r);
}

static void test_sizeof_compound_literal_not_vla(void) {
	printf("\n--- sizeof Compound Literal Not VLA ---\n");

	// sizeof (int){x} — the (int) is a cast, {x} is the initializer.
	// The whole compound literal is a compile-time sizeof operand.
	const char *code =
	    "int x = 5;\n"
	    "void f(void) {\n"
	    "    int arr[sizeof (int){x}];\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "cl_sizeof.c", feat);
	CHECK(r.status == PRISM_OK, "sizeof compound literal: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "= {0}") != NULL,
		      "sizeof compound literal: brace-init, not memset");
		CHECK(strstr(r.output, "memset") == NULL,
		      "sizeof compound literal: no spurious memset");
	}
	prism_free(&r);
}

static void test_sizeof_unary_paren_not_vla(void) {
	printf("\n--- sizeof Unary Paren Not VLA ---\n");

	// sizeof +(x) — the unary + is skipped, then the primary (x) must be
	// skipped as a balanced group, not just the opening '('.
	const char *code =
	    "int x;\n"
	    "void f(void) {\n"
	    "    int arr[sizeof +(x)];\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "sz_uparen.c", feat);
	CHECK(r.status == PRISM_OK, "sizeof unary paren: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "= {0}") != NULL,
		      "sizeof unary paren: brace-init, not memset");
		CHECK(strstr(r.output, "memset") == NULL,
		      "sizeof unary paren: no spurious memset");
	}
	prism_free(&r);
}

static void test_pointer_to_vla_sizeof_erasure(void) {
	printf("\n--- Pointer-to-VLA sizeof Erasure ---\n");

	// int (*p)[x] is a pointer to a VLA. sizeof *p evaluates at runtime
	// to x * sizeof(int), so arr[sizeof *p] is itself a VLA.
	// Prism must register p as a VLA type so array_size_is_vla sees it.
	const char *code =
	    "void f(int x) {\n"
	    "    int (*p)[x];\n"
	    "    int arr[sizeof *p];\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "ptr_vla_sizeof.c", feat);
	CHECK(r.status == PRISM_OK, "ptr-to-VLA sizeof: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "memset(&arr") != NULL,
		      "ptr-to-VLA sizeof: arr gets memset (is VLA)");
		CHECK(strstr(r.output, "arr[sizeof *p] = {0}") == NULL,
		      "ptr-to-VLA sizeof: no brace-init (would fail to compile)");
	}
	prism_free(&r);
}

static void test_generic_vla_black_hole(void) {
	printf("\n--- _Generic VLA Black Hole ---\n");

	// _Generic(1, int: x, float: 10) may select x (a runtime variable),
	// making the array a VLA. Prism must not skip _Generic blindly;
	// it should conservatively assume VLA and use memset.
	const char *code =
	    "void f(int x) {\n"
	    "    int arr[_Generic(1, int: x, float: 10)];\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "generic_vla.c", feat);
	CHECK(r.status == PRISM_OK, "_Generic VLA: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "memset(&arr") != NULL,
		      "_Generic VLA: arr gets memset (is VLA)");
		CHECK(strstr(r.output, "= {0}") == NULL,
		      "_Generic VLA: no brace-init (would fail to compile)");
	}
	prism_free(&r);
}

static void test_sizeof_expr_index_not_vla(void) {
	printf("\n--- sizeof Expression Indexing Not VLA ---\n");

	// sizeof(ptr[x]) is always sizeof(int) — compile-time constant.
	// The [x] is expression indexing (subscript), not an array dimension.
	// Prism must not treat arr as a VLA.
	const char *code =
	    "int *ptr;\n"
	    "int x = 5;\n"
	    "void f(void) {\n"
	    "    int arr[ sizeof(ptr[x]) ];\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "sizeof_expr_idx.c", feat);
	CHECK(r.status == PRISM_OK, "sizeof expr index: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "= {0}") != NULL,
		      "sizeof expr index: brace-init (static array)");
		CHECK(strstr(r.output, "memset") == NULL,
		      "sizeof expr index: no memset (not VLA)");
	}
	prism_free(&r);
}

static void test_ghost_enum_cast_initializer(void) {
	printf("\n--- Ghost Enum (Cast Initializer) ---\n");

	// Enum defined in a cast expression initializer: the enum constants
	// must be registered so the subsequent array dimension is recognized
	// as a compile-time constant, not a VLA.
	const char *code =
	    "void f(void) {\n"
	    "    int val = (enum { FLAG = 10 })0;\n"
	    "    int arr[FLAG];\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "ghost_enum_cast.c", feat);
	CHECK(r.status == PRISM_OK, "ghost enum cast: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "arr[FLAG] = {0}") != NULL ||
		      strstr(r.output, "arr[10] = {0}") != NULL,
		      "ghost enum cast: arr uses brace-init (static)");
		CHECK(strstr(r.output, "memset(&arr") == NULL,
		      "ghost enum cast: no memset (not VLA)");
	}
	prism_free(&r);
}

static void test_ghost_enum_fnptr_param(void) {
	printf("\n--- Ghost Enum (Function Pointer Param) ---\n");

	// Enum defined inside a function pointer parameter list.
	const char *code =
	    "void g(void) {\n"
	    "    void (*fp)(enum { MAGIC = 42 } e);\n"
	    "    int arr[MAGIC];\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "ghost_enum_fnptr.c", feat);
	CHECK(r.status == PRISM_OK, "ghost enum fnptr: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "arr[MAGIC] = {0}") != NULL ||
		      strstr(r.output, "arr[42] = {0}") != NULL,
		      "ghost enum fnptr: arr uses brace-init (static)");
		CHECK(strstr(r.output, "memset(&arr") == NULL,
		      "ghost enum fnptr: no memset (not VLA)");
	}
	prism_free(&r);
}

static void test_bare_orelse_comma_boundary(void) {
	printf("\n--- Bare Orelse Comma Boundary ---\n");

	// Bare orelse must break on comma at depth 0.
	// y = 2 is after the comma and must not be swallowed into the orelse fallback.
	const char *code =
	    "int get(void) { return 0; }\n"
	    "int fallback(void) { return 42; }\n"
	    "void f(void) {\n"
	    "    int x, y = 0;\n"
	    "    x = get() orelse fallback(), y = 2;\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "bare_comma.c", feat);
	CHECK(r.status == PRISM_OK, "bare orelse comma: transpiles OK");
	if (r.output) {
		// y = 2 must appear OUTSIDE the if block
		CHECK(strstr(r.output, "y = 2") != NULL,
		      "bare orelse comma: y = 2 preserved");
		// The fallback block should end with fallback(); not fallback(), y = 2;
		CHECK(strstr(r.output, "fallback(), y") == NULL,
		      "bare orelse comma: comma not swallowed into fallback");
	}
	prism_free(&r);
}

static void test_c23_attr_label_defer(void) {
	printf("\n--- C23 Attributed Label Defer ---\n");

	// A label with [[maybe_unused]] placed between identifier and colon
	// (GCC-extension form) must be recognized by the label scanner so
	// goto fires the correct defer cleanup when exiting a scope.
	const char *code =
	    "void f(void) {\n"
	    "    int x;\n"
	    "    {\n"
	    "        defer x = 1;\n"
	    "        goto done;\n"
	    "    }\n"
	    "    done [[maybe_unused]]:\n"
	    "    (void)x;\n"
	    "}\n";
	PrismFeatures feat = prism_defaults();
	PrismResult r = prism_transpile_source(code, "c23_label.c", feat);
	CHECK(r.status == PRISM_OK, "c23 attr label: transpiles OK");
	if (r.output) {
		// The defer cleanup (x = 1) must appear before the goto
		char *goto_pos = strstr(r.output, "goto done");
		char *cleanup = strstr(r.output, "x = 1");
		CHECK(goto_pos != NULL && cleanup != NULL && cleanup < goto_pos,
		      "c23 attr label: defer fires before goto");
	}
	prism_free(&r);
}

void run_api_tests(void) {
	printf("\n=== API TESTS ===\n");

	test_lib_defaults();
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
	test_lib_defer_break_continue_rejected();
	test_lib_array_orelse_rejected();
	test_deep_struct_nesting_walker();
	test_lib_c23_attr_void_function();
	test_lib_generic_array_not_vla();
	test_fnptr_return_type_capture();
	test_nested_fnptr_return_type();
	test_generic_defer_passthrough();
	test_generic_decl_parenthesized_passthrough();
	test_file_c23_generic_decl_parentheses();
	test_file_c23_generic_decl_macro_layers();
	test_file_c23_generic_decl_macro_indirection();
	test_file_c23_generic_decl_plain_redeclaration();
	test_raw_c23_attr_interleave();
	test_c23_auto_orelse();
	test_line_directive_escaped_quote();
	test_cross_compile_msvc_ret_type();
	test_typeof_memset_no_shadow();
	test_c23_constexpr_thread_local();
	test_orelse_backtrack_desync();
	test_orelse_nested_paren_rejected();
	test_vla_var_not_typedef();
	test_orelse_typedef_cast_comma();
	test_atomic_typedef_qualifier();
	test_pragma_in_declarator();
	test_chained_sizeof_not_vla();
	test_pragma_return_type_capture();
	test_c23_attr_comma_boundary();
	test_thread_local_goto_exempt();
	test_asm_register_zeroinit();
	test_sizeof_unary_paren_not_vla();
	test_sizeof_compound_literal_not_vla();
	test_vla_heuristic_zombie();
	test_pragma_declarator_zeroinit();
	test_sizeof_vla_var_not_skipped();
	test_c23_attr_cast_comma_boundary();
	test_subscript_vla_in_brackets();
	test_typeof_index_not_vla();
	test_pragma_in_array_bounds();
	test_pointer_to_vla_sizeof_erasure();
	test_generic_vla_black_hole();
	test_sizeof_expr_index_not_vla();
	test_ghost_enum_cast_initializer();
	test_ghost_enum_fnptr_param();
	test_bare_orelse_comma_boundary();
	test_c23_attr_label_defer();
	test_cli_large_passthrough_argv();

	test_memory_leak_stress();
}

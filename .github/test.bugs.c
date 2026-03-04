static void test_vla_pessimization_inline_enum_in_sizeof(void) {
	printf("\n--- VLA Pessimization: Inline Enum in sizeof ---\n");

	const char *code =
	    "int main(void) {\n"
	    "    int arr[sizeof(enum { A = 5 }) + A];\n"
	    "    return 0;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "vla inline enum: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "vla inline enum: transpiles OK");
	CHECK(result.output != NULL, "vla inline enum: output not NULL");

	CHECK(strstr(result.output, "= {0}") != NULL,
	      "vla inline enum: uses = {0} not memset (array size is compile-time constant)");
	CHECK(strstr(result.output, "memset") == NULL,
	      "vla inline enum: no memset (A is enum constant, not runtime variable)");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_indirect_longjmp_bypasses_defer_safety(void) {
	printf("\n--- Indirect longjmp Bypasses Defer Safety ---\n");

	const char *code =
	    "#include <setjmp.h>\n"
	    "static jmp_buf buf;\n"
	    "static void my_jump(jmp_buf b, int v) { longjmp(b, v); }\n"
	    "void bad(void) {\n"
	    "    defer (void)0;\n"
	    "    my_jump(buf, 1);\n"
	    "}\n"
	    "int main(void) { if (setjmp(buf) == 0) bad(); return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "indirect longjmp: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);

	CHECK(result.status != PRISM_OK,
	      "indirect longjmp: should reject defer in function calling longjmp via wrapper");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_prism_oe_temp_var_namespace_collision(void) {
	printf("\n--- Temp Variable Namespace Collision (_prism_oe_) ---\n");

	const char *code =
	    "#include <stdio.h>\n"
	    "int *get_val(void) { static int v = 42; return &v; }\n"
	    "int *get_fb(void) { static int v = 99; return &v; }\n"
	    "int main(void) {\n"
	    "    int *_prism_oe_0 = (int*)0xDEAD;\n"
	    "    const int *x = get_val() orelse get_fb();\n"
	    "    (void)x;\n"
	    "    printf(\"%p\\n\", (void*)_prism_oe_0);\n"
	    "    return 0;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "prism_oe collision: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "prism_oe collision: transpiles OK");

	// After fix: generated temps use reserved _Prism_oe_ prefix, not _prism_oe_.
	// User's _prism_oe_0 should coexist with generated _Prism_oe_0.
	CHECK(strstr(result.output, "_Prism_oe_") != NULL,
	      "prism_oe collision: transpiler should use reserved _Prism_ prefix for generated temps");
	// Verify no generated variable uses the user-accessible _prism_oe_ prefix
	// (only user's own code should contain _prism_oe_0)
	const char *gen = strstr(result.output, " _prism_oe_");
	bool gen_uses_old_prefix = false;
	while (gen) {
		// Skip if this is inside the user's code (user wrote "_prism_oe_0")
		if (gen > result.output && *(gen - 1) == '*') { gen = strstr(gen + 1, " _prism_oe_"); continue; }
		// Check if this looks like a generated declaration (has " = (" after it)
		const char *after = gen + 11;
		while (*after == ' ' || (*after >= '0' && *after <= '9')) after++;
		if (*after == '=' || strncmp(after, " = (", 4) == 0) { gen_uses_old_prefix = true; break; }
		gen = strstr(gen + 1, " _prism_oe_");
	}
	CHECK(!gen_uses_old_prefix,
	      "prism_oe collision: generated temp must use _Prism_ prefix, not _prism_");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_prism_p_temp_var_namespace_collision(void) {
	printf("\n--- Temp Variable Namespace Collision (_prism_p_) ---\n");

	const char *code =
	    "#include <stdio.h>\n"
	    "int main(void) {\n"
	    "    char *_prism_p_0 = \"Gotcha\";\n"
	    "    volatile typeof(struct { int a; int b; }) x;\n"
	    "    printf(\"p=%s a=%d b=%d\\n\", _prism_p_0, x.a, x.b);\n"
	    "    return 0;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "prism_p collision: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "prism_p collision: transpiles OK");

	// After fix: generated temps use reserved _Prism_p_ prefix, not _prism_p_.
	// User's _prism_p_0 should coexist with generated _Prism_p_0.
	CHECK(strstr(result.output, "_Prism_p_") != NULL || strstr(result.output, "_Prism_i_") != NULL,
	      "prism_p collision: transpiler should use reserved _Prism_ prefix for volatile memset temps");
	CHECK(strstr(result.output, "volatile char *_prism_p_") == NULL,
	      "prism_p collision: generated volatile memset temp must not use _prism_p_ prefix");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_auto_type_fallback_requires_gnu_extensions(void) {
	printf("\n--- __auto_type Fallback Requires GNU Extensions ---\n");

	const char *code =
	    "#include <stdio.h>\n"
	    "struct { int x; } anon_fn(void) {\n"
	    "    defer printf(\"deferred\\n\");\n"
	    "    return (struct { int x; }){42};\n"
	    "}\n"
	    "int main(void) { return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "auto_type fallback: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "auto_type fallback: transpiles OK");
	CHECK(result.output != NULL, "auto_type fallback: output not NULL");

	// __auto_type is the only correct approach for anonymous struct returns with defer.
	// Each anonymous struct definition creates a unique type in C, so __typeof__ and
	// re-emitting the struct tokens would both produce incompatible types.
	// __auto_type uses the reserved __ prefix and works in all C standard modes
	// (including -std=c99 and -std=c11) with GCC and Clang.
	CHECK(strstr(result.output, "__auto_type") != NULL,
	      "auto_type fallback: anonymous struct return with defer should use __auto_type");
	// Verify the defer is still emitted correctly
	CHECK(strstr(result.output, "printf") != NULL,
	      "auto_type fallback: deferred printf should be present in output");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_raw_multi_declarator_second_var_uninitialized(void) {
	printf("\n--- raw Propagates Across Multi-Declarators ---\n");

	const char *code =
	    "int main(void) {\n"
	    "    raw int x, y;\n"
	    "    return 0;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "raw multi-decl: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "raw multi-decl: transpiles OK");
	CHECK(result.output != NULL, "raw multi-decl: output not NULL");

	bool x_has_init = strstr(result.output, "x = 0") != NULL ||
	                  strstr(result.output, "x = {0}") != NULL;
	bool y_has_init = strstr(result.output, "y = 0") != NULL ||
	                  strstr(result.output, "y = {0}") != NULL;

	CHECK(!x_has_init, "raw multi-decl: x is raw (no init) as expected");
	CHECK(y_has_init,
	      "raw multi-decl: y should be zero-initialized (raw should bind per-variable, not per-declaration)");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_c23_attr_misparsed_as_vla(void) {
	printf("\n--- C23 Attributes [[...]] Misparsed as VLA ---\n");

	const char *code =
	    "int main(void) {\n"
	    "    int x [[maybe_unused]];\n"
	    "    return 0;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "c23 attr vla: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "c23 attr vla: transpiles OK");
	CHECK(result.output != NULL, "c23 attr vla: output not NULL");

	// [[maybe_unused]] is a C23 attribute, not an array dimension.
	// Should produce = 0 (scalar init), NOT memset (VLA treatment).
	CHECK(strstr(result.output, "memset") == NULL,
	      "c23 attr vla: no memset (not a VLA, it's a C23 attribute)");
	CHECK(strstr(result.output, "= 0") != NULL,
	      "c23 attr vla: scalar should get = 0 init");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_forward_goto_into_block_skipped_defer(void) {
	printf("\n--- Forward goto Into Block Executes Skipped Defers ---\n");

	// If goto jumps into a block past "lock(); defer unlock();",
	// unlock() fires at scope exit even though lock() was never called.
	// The transpiler should reject this: the defer is between goto and label.
	const char *code =
	    "void lock(void);\n"
	    "void unlock(void);\n"
	    "int main(void) {\n"
	    "    goto target;\n"
	    "    {\n"
	    "        lock();\n"
	    "        defer unlock();\n"
	    "        target:;\n"
	    "    }\n"
	    "    return 0;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "goto block defer: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);

	// The goto jumps past "defer unlock()" into the block.
	// unlock() would fire at block exit, but lock() was never called.
	// This should be an error (skipping a defer).
	CHECK(result.status != PRISM_OK,
	      "goto block defer: should reject goto that jumps into block past defer");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_vla_pointer_array_no_init(void) {
	printf("\n--- Arrays of Pointers to VLAs Skip Initialization ---\n");

	const char *code =
	    "int main(void) {\n"
	    "    int n = 5;\n"
	    "    int *p[n];\n"
	    "    return 0;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "vla ptr array: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "vla ptr array: transpiles OK");
	CHECK(result.output != NULL, "vla ptr array: output not NULL");

	// int *p[n] is a VLA of pointers. decl.is_pointer=true AND decl.is_array=true.
	// The !decl.is_pointer guard in needs_memset wrongly skips init for this case.
	// It should get memset (VLA can't use = {0}).
	CHECK(strstr(result.output, "memset") != NULL,
	      "vla ptr array: VLA of pointers should be memset-initialized");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_ternary_cast_corrupts_label_detection(void) {
	printf("\n--- Ternary Casts Corrupt goto Label Detection ---\n");

	// In "cond ? (int)done : 0", the cast puts ')' before 'done',
	// so the "prev == '?'" ternary filter fails and 'done:' is
	// falsely registered as a label. This causes goto_skips_check
	// to return early, missing the defer between the false and real label.
	const char *code =
	    "void cleanup(void);\n"
	    "int done = 42;\n"
	    "void f(void) {\n"
	    "    goto done;\n"
	    "    (void)(1 ? (int)done : 0);\n"
	    "    defer cleanup();\n"
	    "    done:\n"
	    "    (void)0;\n"
	    "}\n"
	    "int main(void) { f(); return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "ternary cast label: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);

	// The goto jumps past "defer cleanup()" to reach "done:".
	// The transpiler should error about skipping the defer.
	// With the bug, the cast in "(int)done" causes false label detection,
	// making goto_skips_check return early and miss the defer.
	CHECK(result.status != PRISM_OK,
	      "ternary cast label: goto skipping defer should be caught even with ternary cast");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_c23_extended_float_x_suffixes(void) {
	printf("\n--- Missing C23 Extended Float x Suffixes ---\n");

	// C23 defines _Float32x (≈double), _Float64x (≈long double), _Float128x.
	// Their literal suffixes f32x, f64x, f128x should be recognized.
	const char *code =
	    "int main(void) {\n"
	    "    double a = 1.0f32x;\n"
	    "    long double b = 2.0f64x;\n"
	    "    return 0;\n"
	    "}\n";

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_source(code, "test_float_x.c", features);
	CHECK_EQ(result.status, PRISM_OK, "float x suffix: transpiles OK");
	CHECK(result.output != NULL, "float x suffix: output not NULL");

	// f32x maps to double (no suffix needed), f64x maps to long double (L suffix)
	// The transpiler should replace f32x with nothing and f64x with L.
	CHECK(strstr(result.output, "1.0f32x") == NULL,
	      "float x suffix: f32x should be replaced (not left as-is)");
	CHECK(strstr(result.output, "2.0f64x") == NULL,
	      "float x suffix: f64x should be replaced (not left as-is)");

	prism_free(&result);
}

static void test_stmt_expr_initializer_features(void) {
	printf("\n--- Statement Expression Initializer Features ---\n");

	// Regression: ensure zeroinit and defer work inside statement expression initializers.
	// The initializer loop in process_declarators emits tokens that the main loop processes,
	// so features like zeroinit and defer must work inside ({ ... }) initializers.

	// Test 1: zeroinit inside stmt expr initializer
	{
		const char *code =
		    "void f(void) {\n"
		    "    int x = ({ int y; y = 1; y; });\n"
		    "    (void)x;\n"
		    "}\n"
		    "int main(void) { f(); return 0; }\n";

		char *path = create_temp_file(code);
		CHECK(path != NULL, "stmt expr zeroinit: create temp file");

		PrismFeatures features = prism_defaults();
		PrismResult result = prism_transpile_file(path, features);
		CHECK_EQ(result.status, PRISM_OK, "stmt expr zeroinit: transpiles OK");
		CHECK(result.output != NULL, "stmt expr zeroinit: output not NULL");
		CHECK(strstr(result.output, "y = 0") != NULL,
		      "stmt expr zeroinit: variable inside stmt expr initializer should be zero-initialized");

		prism_free(&result);
		unlink(path);
		free(path);
	}

	// Test 2: defer inside block inside stmt expr initializer
	{
		const char *code =
		    "#include <stdio.h>\n"
		    "void f(void) {\n"
		    "    int x = ({ { defer (void)0; } 1; });\n"
		    "    (void)x;\n"
		    "}\n"
		    "int main(void) { f(); return 0; }\n";

		char *path = create_temp_file(code);
		CHECK(path != NULL, "stmt expr defer: create temp file");

		PrismFeatures features = prism_defaults();
		PrismResult result = prism_transpile_file(path, features);
		CHECK_EQ(result.status, PRISM_OK, "stmt expr defer: transpiles OK");
		CHECK(result.output != NULL, "stmt expr defer: output not NULL");
		CHECK(strstr(result.output, "defer") == NULL,
		      "stmt expr defer: 'defer' keyword must be processed, not emitted raw");

		prism_free(&result);
		unlink(path);
		free(path);
	}
}

static void test_pragma_breaks_type_specifier(void) {
	printf("\n--- _Pragma Breaks Type Specifier Parsing ---\n");

	// _Pragma between qualifiers and the type keyword causes parse_type_specifier
	// to bail out early, so zeroinit never happens.
	const char *code =
	    "void f(void) {\n"
	    "    const _Pragma(\"GCC diagnostic ignored \\\"-Wunused\\\"\") int x;\n"
	    "    (void)x;\n"
	    "}\n"
	    "int main(void) { f(); return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "pragma type spec: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "pragma type spec: transpiles OK");
	CHECK(result.output != NULL, "pragma type spec: output not NULL");

	// The variable should get zero-init even with _Pragma in the type specifier.
	CHECK(strstr(result.output, "= 0") != NULL || strstr(result.output, "= {0}") != NULL,
	      "pragma type spec: variable should be zero-initialized despite _Pragma in type");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_static_const_raw_ordering(void) {
	printf("\n--- static const raw Ordering Inconsistency ---\n");

	// "static const raw int x;" should strip 'raw' and not zero-init.
	// Currently handle_storage_raw fails to skip qualifiers, leaving 'raw' in output.
	const char *code =
	    "void f(void) {\n"
	    "    static const raw int x;\n"
	    "    (void)x;\n"
	    "}\n"
	    "int main(void) { f(); return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "static const raw: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "static const raw: transpiles OK");
	CHECK(result.output != NULL, "static const raw: output not NULL");

	// 'raw' must be stripped from the output (it's not valid C).
	CHECK(strstr(result.output, " raw ") == NULL,
	      "static const raw: 'raw' keyword must be consumed, not emitted");
	// Should NOT have zero-init (raw suppresses it).
	CHECK(strstr(result.output, "= 0") == NULL && strstr(result.output, "= {0}") == NULL,
	      "static const raw: variable should NOT be zero-initialized (raw suppresses it)");

	prism_free(&result);
	unlink(path);
	free(path);
}

void run_bug_report_tests(void) {
	test_vla_pessimization_inline_enum_in_sizeof();
	test_indirect_longjmp_bypasses_defer_safety();
	test_prism_oe_temp_var_namespace_collision();
	test_prism_p_temp_var_namespace_collision();
	test_auto_type_fallback_requires_gnu_extensions();
	test_raw_multi_declarator_second_var_uninitialized();
	test_c23_attr_misparsed_as_vla();
	test_vla_pointer_array_no_init();
	test_ternary_cast_corrupts_label_detection();
	test_c23_extended_float_x_suffixes();
	test_stmt_expr_initializer_features();
	test_pragma_breaks_type_specifier();
	test_static_const_raw_ordering();
}

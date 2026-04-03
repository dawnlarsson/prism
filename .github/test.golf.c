// Prism false-positive VLA detection on decayed param sizeof.
// sizeof(arr) where arr is int arr[n][n] parameter decays to int(*)[n],
// sizeof(pointer) is constant. Prism should use = {0}, not memset.
static void test_golf_param_multidim_vla(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(int n, int arr[n][n]) {\n"
	    "    int local[sizeof(arr)];\n"
	    "    (void)local;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "golf_pm.c", f);
	CHECK_EQ(r.status, PRISM_OK, "golf param multidim: transpiles OK");
	CHECK(r.output != NULL, "golf param multidim: output not NULL");
	if (r.output) {
		// sizeof(arr) where arr is a parameter decays to sizeof(pointer),
		// which is a constant — Prism should use = {0}, not memset.
		CHECK(strstr(r.output, "memset") == NULL,
		      "golf param multidim: no memset (sizeof decayed param is constant)");
	}
	prism_free(&r);
}

// Complex return type typedef synthesis consumes __attribute__.
// int (*f(void))[5] __attribute__((unused)) with defer — Prism's
// return-type typedef copies the attribute and entire body into the typedef.
static void test_golf_defer_complex_return_gnu_attr(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "static int g[5];\n"
	    "int (*f(void))[5] __attribute__((unused)) {\n"
	    "    defer { }\n"
	    "    return &g;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "golf_ra.c", f);
	CHECK_EQ(r.status, PRISM_OK, "golf complex return + gnu attr: transpiles OK");
	CHECK(r.output != NULL, "golf complex return + gnu attr: output not NULL");
	if (r.output) {
		// The bug was that the return-type typedef consumed __attribute__.
		// Check the typedef doesn't contain it.
		const char *td = strstr(r.output, "typedef");
		CHECK(td != NULL, "golf typedef exists");
		if (td) {
			const char *semi = strchr(td, ';');
			CHECK(semi != NULL, "golf typedef has semicolon");
			if (semi) {
				// Extract typedef text and check no attribute leaked in
				size_t len = (size_t)(semi - td);
				char *tdbuf = malloc(len + 1);
				memcpy(tdbuf, td, len);
				tdbuf[len] = '\0';
				CHECK(strstr(tdbuf, "__attribute__") == NULL,
				      "golf typedef must not contain __attribute__");
				free(tdbuf);
			}
		}
	}
	prism_free(&r);
}

void run_golf_tests(void) {
	printf("\n=== GOLF TESTS ===\n");
	test_golf_param_multidim_vla();
	test_golf_defer_complex_return_gnu_attr();
}

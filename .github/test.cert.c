// CERT: certification roadmap — flag cubes, cross-feature slices, -fno-safety hooks.
// Run: prism run .github/test.c -- cert   or   PRISM_SUITE_ONLY=cert prism run .github/test.c

static PrismFeatures cert_zd_or(void) {
	PrismFeatures f = prism_defaults();
	f.zeroinit = false;
	/* defer + orelse stay default true */
	return f;
}

// CERT:defer CERT:orelse — zeroinit off; defer + orelse in same function must transpile.
static void cert_flag_cube_defer_orelse_decl(void) {
	PrismFeatures f = cert_zd_or();
	const char *code =
	    "#include <stdio.h>\n"
	    "int main(void) {\n"
	    "    int seed = 0;\n"
	    "    {\n"
	    "        defer printf(\"D\");\n"
	    "        int v = (seed orelse 7);\n"
	    "        (void)v;\n"
	    "    }\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "cert_cube1.c", f);
	CHECK_EQ(r.status, PRISM_OK, "cert flag cube: defer + orelse decl transpiles");
	CHECK(r.output != NULL, "cert flag cube: output");
	if (r.output) {
		CHECK(strstr(r.output, " orelse ") == NULL,
		      "cert flag cube: orelse keyword not left in emission");
	}
	UNIX_ONLY(if (r.output) check_transpiled_output_compiles_and_runs(
		      r.output, "cert flag cube: compile", "cert flag cube: run"));
	prism_free(&r);
}

// CERT:defer CERT:orelse — two defers; orelse between them (zeroinit off). Exercises ordering + lowering outside defer body.
static void cert_flag_cube_defer_pair_orelse_between(void) {
	PrismFeatures f = cert_zd_or();
	const char *code =
	    "#include <stdio.h>\n"
	    "int main(void) {\n"
	    "    int z = 0;\n"
	    "    defer printf(\"B\");\n"
	    "    int y = (z orelse 3);\n"
	    "    defer printf(\"A\");\n"
	    "    (void)y;\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "cert_cube2.c", f);
	CHECK_EQ(r.status, PRISM_OK, "cert flag cube: dual defer + mid orelse transpiles");
	if (r.output) {
		CHECK(strstr(r.output, " orelse ") == NULL,
		      "cert flag cube: orelse lowered (mid-stmt)");
	}
	UNIX_ONLY(if (r.output) check_transpiled_output_compiles_and_runs(
		      r.output, "cert dual defer: compile", "cert dual defer: run"));
	prism_free(&r);
}

// CERT:orelse — multi-declarator split with zeroinit off (pipeline still sound).
static void cert_flag_cube_multi_decl_orelse(void) {
	PrismFeatures f = cert_zd_or();
	const char *code =
	    "int main(void) {\n"
	    "    int a = 0 orelse 1, b = 3;\n"
	    "    (void)a;\n"
	    "    (void)b;\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "cert_cube3.c", f);
	CHECK_EQ(r.status, PRISM_OK, "cert flag cube: multi-decl orelse + zeroinit off");
	if (r.output)
		CHECK(strstr(r.output, " orelse ") == NULL,
		      "cert flag cube: multi-decl orelse lowered");
	UNIX_ONLY(if (r.output) check_transpiled_output_compiles_and_runs(
		      r.output, "cert multi-decl: compile", "cert multi-decl: run"));
	prism_free(&r);
}

// CERT:defer CERT:zeroinit — -fno-safety: switch unbraced decl still OK with defer in scope.
static void cert_nosafety_defer_switch_unbraced(void) {
	PrismFeatures f = prism_defaults();
	f.warn_safety = true;
	const char *code =
	    "void f(int x) {\n"
	    "    defer { (void)0; }\n"
	    "    switch (x) {\n"
	    "        int y;\n"
	    "    case 0:\n"
	    "        y = 1;\n"
	    "        break;\n"
	    "    default:\n"
	    "        break;\n"
	    "    }\n"
	    "}\n"
	    "int main(void) { f(0); return 0; }\n";
	PrismResult r = prism_transpile_source(code, "cert_nosafe1.c", f);
	CHECK_EQ(r.status, PRISM_OK,
		 "cert -fno-safety: switch unbraced decl + defer transpiles");
	UNIX_ONLY(if (r.output) check_transpiled_output_compiles_and_runs(
		      r.output, "cert nosafe+defer: compile", "cert nosafe+defer: run"));
	prism_free(&r);
}

// CERT:bounds CERT:orelse — declarator `[n orelse k]` + default bounds + zeroinit (memset on VLA path).
static void cert_bounds_bracket_orelse_dim(void) {
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;
	const char *code =
	    "int main(void) {\n"
	    "    int n = 3;\n"
	    "    int arr[n orelse 4];\n"
	    "    (void)arr;\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "cert_bbo.c", f);
	CHECK_EQ(r.status, PRISM_OK,
		 "cert bounds+bracket orelse dim: transpiles with default bounds");
	if (r.output)
		CHECK(strstr(r.output, " orelse ") == NULL,
		      "cert bounds+bracket orelse dim: declarator orelse lowered");
	UNIX_ONLY(if (r.output) check_transpiled_output_compiles_and_runs(
		      r.output, "cert bracket dim orelse: compile",
		      "cert bracket dim orelse: run"));
	prism_free(&r);
}

// CERT: Gap — F_ZEROINIT off + orelse decl inside defer body must not emit
// `__typeof__(int t)` (bare-orelse mis-lowering). Prefer process_declarators.
static void cert_zeroinit_off_orelse_in_defer_body(void) {
	PrismFeatures f = cert_zd_or();
	const char *code =
	    "static int get(void) { return 1; }\n"
	    "int main(void) {\n"
	    "    defer {\n"
	    "        int t = get() orelse 0;\n"
	    "        (void)t;\n"
	    "    }\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "cert_zi_off_oe_defer.c", f);
	CHECK_EQ(r.status, PRISM_OK,
		 "cert zi-off+orelse-in-defer: transpiles");
	CHECK(r.output != NULL, "cert zi-off+orelse-in-defer: output");
	if (r.output) {
		CHECK(strstr(r.output, " orelse ") == NULL,
		      "cert zi-off+orelse-in-defer: orelse lowered");
		CHECK(strstr(r.output, "__typeof__(\nint t)") == NULL &&
			  strstr(r.output, "__typeof__(int t)") == NULL,
		      "cert zi-off+orelse-in-defer: no invalid typeof(decl)");
	}
	UNIX_ONLY(if (r.output) check_transpiled_output_compiles_and_runs(
		      r.output, "cert zi-off+orelse-in-defer: compile",
		      "cert zi-off+orelse-in-defer: run"));
	prism_free(&r);
}

// CERT:zeroinit CERT:autostatic — zeroinit off + auto-static: const literal array still hoisted per SPEC.
static void cert_autostatic_zeroinit_off(void) {
	PrismFeatures f = cert_zd_or();
	f.auto_static = true;
	f.bounds_check = false;
	const char *code =
	    "int main(void) {\n"
	    "    const int k[2] = {1, 2};\n"
	    "    (void)k;\n"
	    "    return 0;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "cert_as_zi.c", f);
	CHECK_EQ(r.status, PRISM_OK,
		 "cert auto-static + zeroinit off: const array transpiles");
	if (r.output)
		CHECK(strstr(r.output, "static") != NULL,
		      "cert auto-static + zeroinit off: static injected");
	UNIX_ONLY(if (r.output) check_transpiled_output_compiles_and_runs(
		      r.output, "cert autostatic offzi: compile",
		      "cert autostatic offzi: run"));
	prism_free(&r);
}

void run_cert_tests(void) {
	printf("\n=== CERT (roadmap) TESTS ===\n");
	cert_flag_cube_defer_orelse_decl();
	cert_flag_cube_defer_pair_orelse_between();
	cert_flag_cube_multi_decl_orelse();
	cert_nosafety_defer_switch_unbraced();
	cert_bounds_bracket_orelse_dim();
	cert_zeroinit_off_orelse_in_defer_body();
	cert_autostatic_zeroinit_off();
}

// Repro + regression tests for externally reported Prism issues.
// Run: prism run .github/test.c  (suite "reported")
//
// Each test names an issue id (see run_reported_issues_tests titles).

static void ri_issue_01_skip_stmt_snapshot_cap(void) {
	/* Report: silent cap on do_snap_buf → inconsistent if_trail_snap restore → OOB.
	 * Expected after fix: hard error with stable substring (no silent truncation). */
	size_t cap = 16000;
	char *code = malloc(cap);
	CHECK(code != NULL, "issue-01: malloc code buffer");
	if (!code)
		return;
	int pos = snprintf(code, cap,
			   "void f(void) {\n"
			   "  for (;0;)\n");
	for (int i = 0; i < 1025; i++)
		pos += snprintf(code + pos, cap - pos, "if (1) ");
	pos += snprintf(code + pos, cap - pos, "do ; while(0);\n}\n");
	PrismResult r = prism_transpile_source(code, "ri_skip_snap.c", prism_defaults());
	free(code);
	CHECK_EQ(r.status, PRISM_ERR_SYNTAX,
		 "issue-01: pathological do/if nesting must error (no silent snap cap)");
	if (r.error_msg)
		CHECK(strstr(r.error_msg, "trail snapshot") != NULL,
		      "issue-01: error mentions trail snapshot buffer");
	prism_free(&r);
}

static void ri_issue_02_struct_field_vs_typedef_name(void) {
	/* Report: typedef_lookup on field name `v_int` collides with volatile typedef. */
	const char *src =
	    "typedef volatile int v_int;\n"
	    "struct Safe { int v_int; };\n"
	    "void f(void) {\n"
	    "    const struct Safe obj;\n"
	    "    (void)obj;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(src, "ri_field_ns.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
		 "issue-02: field named like volatile typedef must transpile");
	if (r.output)
		CHECK(strstr(r.output, "cannot be safely zero-initialized") == NULL,
		      "issue-02: no false positive const+memset rejection");
	prism_free(&r);
}

static void ri_issue_03_bounds_rank_ptr_to_array_stack(void) {
	/* Report: declarator `[` counting crossed (*...) boundaries so rank was too
	 * high and inner `p[0][i]` wrapped with sizeof(p[0])/sizeof(p[0][0])
	 * (= sizeof(ptr)/sizeof(int)). Provable minimal repro (same as bc-rank). */
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;
	PrismResult r = prism_transpile_source(
	    "int main(void) {\n"
	    "    int *p[10];\n"
	    "    int i = 0;\n"
	    "    return p[0][i];\n"
	    "}\n",
	    "ri_ptr_arr_rank.c", f);
	CHECK_EQ(r.status, PRISM_OK, "issue-03: transpiles with -fbounds-check");
	if (r.output) {
		CHECK(strstr(r.output, "sizeof(p[0])/sizeof(p[0][0])") == NULL,
		      "issue-03: inner subscript must not use ptr/element sizeof ratio");
	}
	prism_free(&r);
}

static void ri_issue_04_auto_static_multi_decl(void) {
	/* Report: static promoted for whole declaration when only first declarator qualifies. */
	PrismFeatures f = prism_defaults();
	f.auto_static = true;
	PrismResult r = prism_transpile_source(
	    "volatile int r(void);\n"
	    "void f(void) {\n"
	    "    const int a[2] = { 1, 2 }, b = r();\n"
	    "    (void)a; (void)b;\n"
	    "}\n",
	    "ri_autostatic_multi.c", f);
	CHECK_EQ(r.status, PRISM_OK, "issue-04: transpiles");
	if (r.output)
		CHECK(strstr(r.output, "static const int a[2] = { 1, 2 }, b = r()") == NULL,
		      "issue-04: must not prefix static for multi-decl with runtime scalar");
	prism_free(&r);
}

static void ri_issue_05_incomplete_outer_array_bounds(void) {
	/* Report: treating any sized [] as complete; extern int m[][10] still incomplete. */
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;
	PrismResult r = prism_transpile_source(
	    "extern int m[][10];\n"
	    "int main(void) {\n"
	    "    return m[1][2];\n"
	    "}\n",
	    "ri_inc_outer.c", f);
	CHECK_EQ(r.status, PRISM_OK, "issue-05: transpiles");
	if (r.output)
		CHECK(strstr(r.output, "m[__prism_bchk") == NULL,
		      "issue-05: incomplete outer dim — must not emit sizeof(m)/sizeof wrap");
	prism_free(&r);
}

static void ri_issue_06_typeof_unqual_union_const(void) {
	/* Report: typeof_unqual skipped inner SUE scan → is_union false → const memset slip. */
	PrismResult r = prism_transpile_source(
	    "void f(void) {\n"
	    "    const typeof_unqual(union { int a; char b[64]; }) payload;\n"
	    "    (void)payload;\n"
	    "}\n",
	    "ri_tuq_union_const.c", prism_defaults());
	if (r.status == PRISM_OK && r.output) {
		CHECK(strstr(r.output, "__builtin_memset((void*)&payload") == NULL &&
			      strstr(r.output, "__builtin_memset((void *)&payload") == NULL,
		      "issue-06: must not memset through const union (UB)");
	} else {
		CHECK(r.status != PRISM_OK ||
			      (r.error_msg && strstr(r.error_msg, "const") != NULL),
		      "issue-06: reject or safe emit for const union typeof_unqual");
	}
	prism_free(&r);
}

static void ri_issue_07_bounds_commutative_nested(void) {
	/* Report: idx[arr[0]] bypassed commutative guard (only bare arr checked). */
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;
	PrismResult r = prism_transpile_source(
	    "int main(void) {\n"
	    "    int arr[10];\n"
	    "    int idx = 5;\n"
	    "    arr[0] = 0;\n"
	    "    int x = idx[arr[0]];\n"
	    "    return x;\n"
	    "}\n",
	    "ri_comm_nested.c", f);
	CHECK(r.status != PRISM_OK, "issue-07: idx[arr[0]] must be rejected (-fbounds-check)");
	if (r.error_msg)
		CHECK(strstr(r.error_msg, "commutative") != NULL,
		      "issue-07: diagnostic mentions commutative subscript");
	prism_free(&r);
}

void run_reported_issues_tests(void) {
	printf("\n=== REPORTED ISSUES (repro / regression) ===\n");
	ri_issue_01_skip_stmt_snapshot_cap();
	ri_issue_02_struct_field_vs_typedef_name();
	ri_issue_03_bounds_rank_ptr_to_array_stack();
	ri_issue_04_auto_static_multi_decl();
	ri_issue_05_incomplete_outer_array_bounds();
	ri_issue_06_typeof_unqual_union_const();
	ri_issue_07_bounds_commutative_nested();
}

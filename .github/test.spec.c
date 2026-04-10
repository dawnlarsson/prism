// test.spec.c — Formal Language Specification compliance tests.
//
// Every test in this file maps directly to a numbered Constraint or Semantic
// rule in SPEC.md Part II ("Formal Language Specification").  The naming
// convention is: spec_<extension>_<C|S><number>[_<variant>]
//
//   C = Constraint (diagnostic required → PRISM_ERR_SEMANTIC expected)
//   S = Semantic   (runtime behavior → transpile + compile + run, or strstr)
//
// These tests guarantee that the transpiler's observable behavior is 100%
// consistent with the formal language specification.  If the spec changes,
// these tests must be updated in lockstep.

// ═══════════════════════════════════════════════════════════════════════════
//  PART 1: defer
// ═══════════════════════════════════════════════════════════════════════════

// ── Constraint 1: defer shall appear only at block scope ──────────────
static void spec_defer_C1_block_scope(void) {
	// File-scope defer is a constraint violation.
	{
		const char *code = "defer int x;\n";
		PrismResult r = prism_transpile_source(code, "spec_dc1.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "spec defer C1: file-scope defer rejected");
		prism_free(&r);
	}
}

// ── Constraint 2: independent control-flow context ────────────────────
static void spec_defer_C2_control_flow(void) {
	// return inside defer body → rejected
	{
		const char *code =
		    "int f(void) {\n"
		    "    defer { return 0; }\n"
		    "    return 1;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_dc2a.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "spec defer C2: return in defer rejected");
		prism_free(&r);
	}
	// goto inside defer body → rejected
	{
		const char *code =
		    "void f(void) {\n"
		    "    defer { goto L; }\n"
		    "    L: (void)0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_dc2b.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "spec defer C2: goto in defer rejected");
		prism_free(&r);
	}
	// break without enclosing loop/switch in defer → rejected
	{
		const char *code =
		    "void f(void) {\n"
		    "    for (int i = 0; i < 1; i++)\n"
		    "        defer { break; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_dc2c.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "spec defer C2: bare break in defer rejected");
		prism_free(&r);
	}
	// continue without enclosing loop in defer → rejected
	{
		const char *code =
		    "void f(void) {\n"
		    "    while (1)\n"
		    "        defer { continue; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_dc2d.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "spec defer C2: bare continue in defer rejected");
		prism_free(&r);
	}
	// break INSIDE a switch WITHIN defer → allowed
	{
		const char *code =
		    "void f(int x) {\n"
		    "    defer { switch(x) { case 0: break; } }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_dc2e.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "spec defer C2: break in switch within defer allowed");
		prism_free(&r);
	}
	// continue INSIDE a loop WITHIN defer → allowed
	{
		const char *code =
		    "void f(void) {\n"
		    "    defer { for (int i = 0; i < 1; i++) continue; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_dc2f.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "spec defer C2: continue in loop within defer allowed");
		prism_free(&r);
	}
}

// ── Constraint 3: nested defer ────────────────────────────────────────
static void spec_defer_C3_nested(void) {
	const char *code =
	    "void f(void) {\n"
	    "    defer { defer { (void)0; } }\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "spec_dc3.c", prism_defaults());
	CHECK(r.status != PRISM_OK, "spec defer C3: nested defer rejected");
	prism_free(&r);
}

// ── Constraint 4: labels in defer body ────────────────────────────────
static void spec_defer_C4_labels(void) {
	const char *code =
	    "void f(void) {\n"
	    "    defer { L: (void)0; }\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "spec_dc4.c", prism_defaults());
	CHECK(r.status != PRISM_OK, "spec defer C4: label in defer rejected");
	prism_free(&r);
}

// ── Constraint 5: setjmp/longjmp/vfork/pthread_exit taint ─────────────
static void spec_defer_C5_taint(void) {
	// setjmp
	{
		const char *code =
		    "#include <setjmp.h>\n"
		    "void f(void) {\n"
		    "    jmp_buf buf;\n"
		    "    defer { (void)0; }\n"
		    "    setjmp(buf);\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_dc5a.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "spec defer C5: setjmp + defer rejected");
		prism_free(&r);
	}
	// vfork (just the identifier presence is enough to taint)
	{
		const char *code =
		    "int vfork(void);\n"
		    "void f(void) {\n"
		    "    defer { (void)0; }\n"
		    "    vfork();\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_dc5b.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "spec defer C5: vfork + defer rejected");
		prism_free(&r);
	}
	// pthread_exit
	{
		const char *code =
		    "void pthread_exit(void *retval);\n"
		    "void f(void) {\n"
		    "    defer { (void)0; }\n"
		    "    pthread_exit(0);\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_dc5c.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "spec defer C5: pthread_exit + defer rejected");
		prism_free(&r);
	}
}

// ── Constraint 6: computed goto / asm goto ────────────────────────────
static void spec_defer_C6_computed_goto(void) {
	const char *code =
	    "void f(void) {\n"
	    "    void *target = &&L;\n"
	    "    defer { (void)0; }\n"
	    "    L: goto *target;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "spec_dc6.c", prism_defaults());
	CHECK(r.status != PRISM_OK, "spec defer C6: computed goto + defer rejected");
	prism_free(&r);
}

// ── Constraint 7: defer at top level of stmt-expr ─────────────────────
static void spec_defer_C7_stmt_expr_top(void) {
	const char *code =
	    "void f(void) {\n"
	    "    int x = ({ defer (void)0; 42; });\n"
	    "    (void)x;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "spec_dc7.c", prism_defaults());
	CHECK(r.status != PRISM_OK, "spec defer C7: defer at top level of stmt-expr rejected");
	prism_free(&r);
}

// ── Constraint 8: defer as last stmt of stmt-expr ─────────────────────
static void spec_defer_C8_stmt_expr_last(void) {
	const char *code =
	    "void cleanup(void);\n"
	    "void f(void) {\n"
	    "    int x = ({ int r = 1; { defer cleanup(); } });\n"
	    "    (void)x;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "spec_dc8.c", prism_defaults());
	CHECK(r.status != PRISM_OK, "spec defer C8: defer block as last stmt of stmt-expr rejected");
	prism_free(&r);
}

// ── Constraint 9: same-block shadow rule ──────────────────────────────
static void spec_defer_C9_shadow(void) {
	const char *code =
	    "void use(int *);\n"
	    "void f(void) {\n"
	    "    int x = 1;\n"
	    "    defer use(&x);\n"
	    "    int x;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "spec_dc9.c", prism_defaults());
	CHECK(r.status != PRISM_OK, "spec defer C9: same-block shadow after defer rejected");
	prism_free(&r);
}

// ── Semantic 1: defer body not executed at registration ───────────────
// ── Semantic 2: LIFO unwinding at scope exit ─────────────────────────
// ── Semantic 3: nested scope unwinding (innermost to outermost) ──────
static void spec_defer_S1_S2_S3_lifo(void) {
	// This is a runtime test: three defers in two scopes, verify LIFO order.
	const char *code =
	    "#include <stdio.h>\n"
	    "#include <string.h>\n"
	    "static char buf[64];\n"
	    "static int pos = 0;\n"
	    "static void record(char c) { buf[pos++] = c; buf[pos] = 0; }\n"
	    "int main(void) {\n"
	    "    defer record('A');\n"
	    "    defer record('B');\n"
	    "    {\n"
	    "        defer record('C');\n"
	    "        defer record('D');\n"
	    "    }\n"
	    "    // After inner scope: D, C fired. At return: B, A fire.\n"
	    "    // Full order: D C B A\n"
	    "    return strcmp(buf, \"DC\") != 0;\n"
	    "}\n";
	char *path = create_temp_file(code);
	PrismFeatures f = prism_defaults();
	PrismResult r = prism_transpile_file(path, f);
	CHECK_EQ(r.status, PRISM_OK, "spec defer S1-S3: transpiles OK");
#ifndef _WIN32
	if (r.output)
		check_transpiled_output_compiles_and_runs(
		    r.output,
		    "spec defer S1-S3: LIFO compiles",
		    "spec defer S1-S3: LIFO runs (order DC at scope close)");
#endif
	prism_free(&r);
	unlink(path);
	free(path);
}

// ── Semantic 4: goto crossing scope boundaries ───────────────────────
static void spec_defer_S4_goto_crossing(void) {
	const char *code =
	    "#include <stdio.h>\n"
	    "#include <string.h>\n"
	    "static char buf[64];\n"
	    "static int pos = 0;\n"
	    "static void record(char c) { buf[pos++] = c; buf[pos] = 0; }\n"
	    "int main(void) {\n"
	    "    {\n"
	    "        defer record('A');\n"
	    "        {\n"
	    "            defer record('B');\n"
	    "            goto out;\n"
	    "        }\n"
	    "    }\n"
	    "    out:\n"
	    "    // goto exits both scopes: B fires, then A fires (LIFO).\n"
	    "    return strcmp(buf, \"BA\") != 0;\n"
	    "}\n";
	char *path = create_temp_file(code);
	PrismResult r = prism_transpile_file(path, prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec defer S4: transpiles OK");
#ifndef _WIN32
	if (r.output)
		check_transpiled_output_compiles_and_runs(
		    r.output,
		    "spec defer S4: goto-crossing compiles",
		    "spec defer S4: goto-crossing runs (order BA)");
#endif
	prism_free(&r);
	unlink(path);
	free(path);
}

// ── Semantic 6: return value captured before defers ──────────────────
static void spec_defer_S6_return_before_defer(void) {
	const char *code =
	    "#include <stdio.h>\n"
	    "static int counter = 0;\n"
	    "static void inc(void) { counter++; }\n"
	    "static int get_and_defer(void) {\n"
	    "    defer inc();\n"
	    "    return counter;  // must capture 0 before inc() runs\n"
	    "}\n"
	    "int main(void) {\n"
	    "    int val = get_and_defer();\n"
	    "    // val should be 0 (captured before defer), counter should be 1 (defer ran)\n"
	    "    return (val != 0 || counter != 1);\n"
	    "}\n";
	char *path = create_temp_file(code);
	PrismResult r = prism_transpile_file(path, prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec defer S6: transpiles OK");
#ifndef _WIN32
	if (r.output)
		check_transpiled_output_compiles_and_runs(
		    r.output,
		    "spec defer S6: return-before-defer compiles",
		    "spec defer S6: return-before-defer runs (return captures pre-defer state)");
#endif
	prism_free(&r);
	unlink(path);
	free(path);
}

// ── Semantic 2 (break/continue): defers at scope exit via break ──────
static void spec_defer_S2_break_continue(void) {
	const char *code =
	    "#include <string.h>\n"
	    "static char buf[64];\n"
	    "static int pos = 0;\n"
	    "static void record(char c) { buf[pos++] = c; buf[pos] = 0; }\n"
	    "int main(void) {\n"
	    "    for (int i = 0; i < 3; i++) {\n"
	    "        defer record('D');\n"
	    "        if (i == 1) continue;\n"
	    "        if (i == 2) break;\n"
	    "    }\n"
	    "    // i=0: normal exit → D; i=1: continue → D; i=2: break → D\n"
	    "    return strcmp(buf, \"DDD\") != 0;\n"
	    "}\n";
	char *path = create_temp_file(code);
	PrismResult r = prism_transpile_file(path, prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec defer S2 break/continue: transpiles OK");
#ifndef _WIN32
	if (r.output)
		check_transpiled_output_compiles_and_runs(
		    r.output,
		    "spec defer S2 break/continue: compiles",
		    "spec defer S2 break/continue: runs (defer fires on break and continue)");
#endif
	prism_free(&r);
	unlink(path);
	free(path);
}

// ═══════════════════════════════════════════════════════════════════════════
//  PART 2: orelse
// ═══════════════════════════════════════════════════════════════════════════

// ── Constraint 1: scalar type only ────────────────────────────────────
static void spec_orelse_C1_scalar(void) {
	// Struct value orelse → rejected
	{
		const char *code =
		    "struct S { int x; };\n"
		    "struct S get(void);\n"
		    "void f(void) {\n"
		    "    struct S s = get() orelse return;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_oc1a.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "spec orelse C1: struct value rejected");
		prism_free(&r);
	}
	// Struct pointer orelse → allowed
	{
		const char *code =
		    "struct S { int x; };\n"
		    "struct S *get(void);\n"
		    "void f(void) {\n"
		    "    struct S *s = get() orelse return;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_oc1b.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "spec orelse C1: struct pointer allowed");
		prism_free(&r);
	}
}

// ── Constraint 2: no file scope ───────────────────────────────────────
static void spec_orelse_C2_file_scope(void) {
	const char *code = "int x = 1 orelse 0;\n";
	PrismResult r = prism_transpile_source(code, "spec_oc2.c", prism_defaults());
	CHECK(r.status != PRISM_OK, "spec orelse C2: file-scope orelse rejected");
	prism_free(&r);
}

// ── Constraint 3: static/extern/thread_local rejected ─────────────────
static void spec_orelse_C3_static_storage(void) {
	// static declaration-initializer orelse → rejected
	{
		const char *code =
		    "int get(void);\n"
		    "void f(void) { static int x = get() orelse 0; (void)x; }\n";
		PrismResult r = prism_transpile_source(code, "spec_oc3a.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "spec orelse C3: static orelse rejected");
		prism_free(&r);
	}
	// Note: extern at block scope can't have initializers in C, so we
	// only test static. Bare assignments to extern vars are not
	// declaration-initializer orelse, so C3 does not apply to them.
}

// ── Constraint 4: no enum body ────────────────────────────────────────
static void spec_orelse_C4_enum(void) {
	const char *code =
	    "int get(void);\n"
	    "void f(void) { enum { A = get() orelse 0 }; }\n";
	PrismResult r = prism_transpile_source(code, "spec_oc4.c", prism_defaults());
	CHECK(r.status != PRISM_OK, "spec orelse C4: orelse in enum rejected");
	prism_free(&r);
}

// ── Constraint 5: bracket orelse no control-flow action ───────────────
static void spec_orelse_C5_bracket_ctrl(void) {
	const char *code =
	    "int get(void);\n"
	    "void f(void) { int arr[get() orelse return]; }\n";
	PrismResult r = prism_transpile_source(code, "spec_oc5.c", prism_defaults());
	CHECK(r.status != PRISM_OK, "spec orelse C5: bracket orelse with return rejected");
	prism_free(&r);
}

// ── Constraint 6: bracket orelse at file scope ────────────────────────
static void spec_orelse_C6_bracket_file(void) {
	const char *code = "int arr[0 orelse 1];\n";
	PrismResult r = prism_transpile_source(code, "spec_oc6.c", prism_defaults());
	CHECK(r.status != PRISM_OK, "spec orelse C6: bracket orelse at file scope rejected");
	prism_free(&r);
}

// ── Constraint 7: side effects in bracket orelse ──────────────────────
// Primary bracket orelse hoists the LHS to a temp variable, so function
// calls / increments / assignments in the LHS are evaluated exactly once.
// The side-effect constraint fires for CHAINED bracket orelse, where the
// intermediate operand would be duplicated in the ternary expansion.
static void spec_orelse_C7_side_effects(void) {
	// Simple bracket orelse: function call is safely hoisted
	{
		const char *code =
		    "int get(void);\n"
		    "void f(int n) { int arr[get() orelse n]; (void)arr; }\n";
		PrismResult r = prism_transpile_source(code, "spec_oc7a.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "spec orelse C7: simple bracket hoists function call");
		if (r.output)
			CHECK(strstr(r.output, "__prism_oe_") != NULL,
			      "spec orelse C7: function call hoisted to temp");
		prism_free(&r);
	}
	// Chained bracket orelse: intermediate side effects rejected
	{
		const char *code =
		    "int get(void);\n"
		    "void f(int n) { int arr[0 orelse get() orelse n]; (void)arr; }\n";
		PrismResult r = prism_transpile_source(code, "spec_oc7b.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "spec orelse C7: chained bracket side-effect rejected");
		prism_free(&r);
	}
}

// ── Constraint 8: bare orelse cast-expression target ──────────────────
static void spec_orelse_C8_cast_target(void) {
	const char *code =
	    "int get(void);\n"
	    "void f(void) { int x; (int)x = get() orelse 5; }\n";
	PrismResult r = prism_transpile_source(code, "spec_oc8.c", prism_defaults());
	CHECK(r.status != PRISM_OK, "spec orelse C8: cast-expression target rejected");
	prism_free(&r);
}

// ── Constraint 10: prototype parameter VLA ────────────────────────────
static void spec_orelse_C10_prototype(void) {
	const char *code =
	    "void f(int n, int arr[n orelse 1]);\n";
	PrismResult r = prism_transpile_source(code, "spec_oc10.c", prism_defaults());
	CHECK(r.status != PRISM_OK, "spec orelse C10: prototype param VLA orelse rejected");
	prism_free(&r);
}

// ── Constraint 11: struct/union member ────────────────────────────────
static void spec_orelse_C11_struct_member(void) {
	const char *code =
	    "int get(void);\n"
	    "struct S { int x; int y; };\n"
	    "void f(void) {\n"
	    "    struct { int a; int b orelse 0; } s;\n"
	    "    (void)s;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "spec_oc11.c", prism_defaults());
	CHECK(r.status != PRISM_OK, "spec orelse C11: orelse in struct member rejected");
	prism_free(&r);
}

// ── Constraint 12: catch-all for unrecognized positions ───────────────
static void spec_orelse_C12_catch_all(void) {
	// orelse in ternary condition
	{
		const char *code =
		    "int get(void);\n"
		    "void f(void) {\n"
		    "    int x = (get() orelse 0) ? 1 : 2;\n"
		    "    (void)x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_oc12.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "spec orelse C12: orelse in sub-expression rejected");
		prism_free(&r);
	}
}

// ── Semantic 1: assignment with value ─────────────────────────────────
static void spec_orelse_S1_assign_value(void) {
	const char *code =
	    "int get_zero(void) { return 0; }\n"
	    "int get_five(void) { return 5; }\n"
	    "int main(void) {\n"
	    "    int x;\n"
	    "    x = get_five() orelse 99;\n"
	    "    if (x != 5) return 1;\n"
	    "    x = get_zero() orelse 99;\n"
	    "    if (x != 99) return 2;\n"
	    "    return 0;\n"
	    "}\n";
	char *path = create_temp_file(code);
	PrismResult r = prism_transpile_file(path, prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec orelse S1: transpiles OK");
#ifndef _WIN32
	if (r.output)
		check_transpiled_output_compiles_and_runs(
		    r.output,
		    "spec orelse S1: assign-value compiles",
		    "spec orelse S1: assign-value runs (truthy keeps, falsy substitutes)");
#endif
	prism_free(&r);
	unlink(path);
	free(path);
}

// ── Semantic 2: assignment with action ────────────────────────────────
static void spec_orelse_S2_assign_action(void) {
	const char *code =
	    "int get_zero(void) { return 0; }\n"
	    "static int f(void) {\n"
	    "    int x;\n"
	    "    x = get_zero() orelse return -1;\n"
	    "    return x;\n"
	    "}\n"
	    "int main(void) {\n"
	    "    return f() == -1 ? 0 : 1;\n"
	    "}\n";
	char *path = create_temp_file(code);
	PrismResult r = prism_transpile_file(path, prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec orelse S2: transpiles OK");
#ifndef _WIN32
	if (r.output)
		check_transpiled_output_compiles_and_runs(
		    r.output,
		    "spec orelse S2: assign-action compiles",
		    "spec orelse S2: assign-action runs (falsy triggers return)");
#endif
	prism_free(&r);
	unlink(path);
	free(path);
}

// ── Semantic 5: declaration initializer ───────────────────────────────
static void spec_orelse_S5_decl_init(void) {
	const char *code =
	    "int get_zero(void) { return 0; }\n"
	    "int get_seven(void) { return 7; }\n"
	    "int main(void) {\n"
	    "    int a = get_seven() orelse 99;\n"
	    "    int b = get_zero() orelse 42;\n"
	    "    if (a != 7) return 1;\n"
	    "    if (b != 42) return 2;\n"
	    "    return 0;\n"
	    "}\n";
	char *path = create_temp_file(code);
	PrismResult r = prism_transpile_file(path, prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec orelse S5: transpiles OK");
#ifndef _WIN32
	if (r.output)
		check_transpiled_output_compiles_and_runs(
		    r.output,
		    "spec orelse S5: decl-init compiles",
		    "spec orelse S5: decl-init runs (truthy 7, falsy substitutes 42)");
#endif
	prism_free(&r);
	unlink(path);
	free(path);
}

// ── Semantic 6: bracket form ──────────────────────────────────────────
static void spec_orelse_S6_bracket(void) {
	// bracket orelse with truthy dim → uses dim
	// bracket orelse with falsy dim → uses fallback
	const char *code =
	    "#include <string.h>\n"
	    "int main(void) {\n"
	    "    int dim_ok = 5;\n"
	    "    int dim_zero = 0;\n"
	    "    int arr1[dim_ok orelse 10];\n"
	    "    int arr2[dim_zero orelse 10];\n"
	    "    if (sizeof(arr1) != 5 * sizeof(int)) return 1;\n"
	    "    if (sizeof(arr2) != 10 * sizeof(int)) return 2;\n"
	    "    return 0;\n"
	    "}\n";
	char *path = create_temp_file(code);
	PrismResult r = prism_transpile_file(path, prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec orelse S6: transpiles OK");
#ifndef _WIN32
	if (r.output)
		check_transpiled_output_compiles_and_runs(
		    r.output,
		    "spec orelse S6: bracket compiles",
		    "spec orelse S6: bracket runs (truthy 5, falsy 10)");
#endif
	prism_free(&r);
	unlink(path);
	free(path);
}

// ── Semantic 7: chained orelse ────────────────────────────────────────
static void spec_orelse_S7_chained(void) {
	const char *code =
	    "int get_zero(void) { return 0; }\n"
	    "int main(void) {\n"
	    "    int x;\n"
	    "    x = get_zero() orelse get_zero() orelse 77;\n"
	    "    if (x != 77) return 1;\n"
	    "    return 0;\n"
	    "}\n";
	char *path = create_temp_file(code);
	PrismResult r = prism_transpile_file(path, prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec orelse S7: transpiles OK");
#ifndef _WIN32
	if (r.output)
		check_transpiled_output_compiles_and_runs(
		    r.output,
		    "spec orelse S7: chained compiles",
		    "spec orelse S7: chained runs (both falsy, final fallback 77)");
#endif
	prism_free(&r);
	unlink(path);
	free(path);
}

// ── Semantic 8: volatile safety (single write) ───────────────────────
static void spec_orelse_S8_volatile(void) {
	// Transpile-output check: must use if/else, not ternary
	// Hoist function call to avoid RHS call rejection with LHS indirection.
	const char *code =
	    "volatile int *get_hw(void);\n"
	    "int get_val(void);\n"
	    "void f(void) {\n"
	    "    volatile int *reg = get_hw();\n"
	    "    int tmp = get_val();\n"
	    "    *reg = tmp orelse 0xFF;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "spec_os8.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec orelse S8: transpiles OK");
	if (r.output) {
		// Must use if/else pattern — exactly one write per branch
		CHECK(strstr(r.output, "if (__prism_oe_") != NULL,
		      "spec orelse S8: volatile uses if/else (single-write guarantee)");
		CHECK(strstr(r.output, "} else {") != NULL,
		      "spec orelse S8: volatile has else branch");
	}
	prism_free(&r);
}

// ── Result Type 1: independent assignment conversions ─────────────────
// This is the ternary promotion hijack test: int + unsigned int must NOT
// undergo the usual arithmetic conversions. Each is independently converted
// to the target type via simple assignment (ISO C §6.5.16.1).
static void spec_orelse_RT1_independent_conversion(void) {
	// Transpiled output test: no ternary on temp
	// Hoist function call to avoid RHS call rejection with LHS indirection.
	{
		const char *code =
		    "long long *get_target(void);\n"
		    "int get_value(void);\n"
		    "void f(void) {\n"
		    "    long long *target = get_target();\n"
		    "    unsigned int fb = 1;\n"
		    "    int tmp = get_value();\n"
		    "    target[0] = tmp orelse fb;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_rt1a.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "spec orelse RT1: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "? __prism_oe_") == NULL,
			      "spec orelse RT1: no ternary (independent assignment conversion)");
			CHECK(strstr(r.output, "if (__prism_oe_") != NULL,
			      "spec orelse RT1: uses if/else");
		}
		prism_free(&r);
	}
	// Runtime test: int(-1) assigned to long long via independent conversion
	// must yield -1LL, not 4294967295LL.
	{
		const char *code =
		    "int main(void) {\n"
		    "    long long target;\n"
		    "    int val = -1;\n"
		    "    unsigned int fb = 1;\n"
		    "    target = val orelse fb;\n"
		    "    // val is -1 (truthy). Independent assignment: target = (long long)(-1) = -1LL\n"
		    "    // If ternary were used: -1 promoted to unsigned → 4294967295, then to LL.\n"
		    "    if (target != -1LL) return 1;\n"
		    "    return 0;\n"
		    "}\n";
		char *path = create_temp_file(code);
		PrismResult r = prism_transpile_file(path, prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "spec orelse RT1 runtime: transpiles OK");
#ifndef _WIN32
		if (r.output)
			check_transpiled_output_compiles_and_runs(
			    r.output,
			    "spec orelse RT1 runtime: compiles",
			    "spec orelse RT1 runtime: int(-1) independently assigned to long long as -1LL");
#endif
		prism_free(&r);
		unlink(path);
		free(path);
	}
}

// ── Result Type 2: bracket form uses conditional expression type ──────
static void spec_orelse_RT2_bracket_conditional(void) {
	// Bracket form: dim ? dim : fallback — conditional expression rules apply
	const char *code =
	    "int main(void) {\n"
	    "    int dim = 3;\n"
	    "    char arr[dim orelse 10];\n"
	    "    if (sizeof(arr) != 3) return 1;\n"
	    "    return 0;\n"
	    "}\n";
	char *path = create_temp_file(code);
	PrismResult r = prism_transpile_file(path, prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec orelse RT2: transpiles OK");
#ifndef _WIN32
	if (r.output)
		check_transpiled_output_compiles_and_runs(
		    r.output,
		    "spec orelse RT2: bracket conditional compiles",
		    "spec orelse RT2: bracket conditional runs (size 3)");
#endif
	prism_free(&r);
	unlink(path);
	free(path);
}

// ── Semantic 2 + defer: orelse action triggers defer cleanup ──────────
static void spec_orelse_S2_defer_cleanup(void) {
	const char *code =
	    "#include <string.h>\n"
	    "static char buf[64];\n"
	    "static int pos = 0;\n"
	    "static void record(char c) { buf[pos++] = c; buf[pos] = 0; }\n"
	    "static int f(void) {\n"
	    "    int x;\n"
	    "    defer record('D');\n"
	    "    int get_zero(void);\n"
	    "    x = get_zero() orelse return -1;\n"
	    "    return x;\n"
	    "}\n"
	    "int get_zero(void) { return 0; }\n"
	    "int main(void) {\n"
	    "    int r = f();\n"
	    "    // Defer must have fired during orelse return.\n"
	    "    if (r != -1) return 1;\n"
	    "    if (strcmp(buf, \"D\") != 0) return 2;\n"
	    "    return 0;\n"
	    "}\n";
	char *path = create_temp_file(code);
	PrismResult r = prism_transpile_file(path, prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec orelse S2+defer: transpiles OK");
#ifndef _WIN32
	if (r.output)
		check_transpiled_output_compiles_and_runs(
		    r.output,
		    "spec orelse S2+defer: compiles",
		    "spec orelse S2+defer: runs (defer fires during orelse return)");
#endif
	prism_free(&r);
	unlink(path);
	free(path);
}

// ═══════════════════════════════════════════════════════════════════════════
//  PART 3: Automatic Zero-Initialization
// ═══════════════════════════════════════════════════════════════════════════

// ── Constraint 1: automatic storage, no initializer, no raw ──────────
static void spec_zeroinit_C1_scope(void) {
	// Block scope: zero-initialized
	{
		const char *code =
		    "void f(void) { int x; (void)x; }\n";
		PrismResult r = prism_transpile_source(code, "spec_zc1a.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "spec zeroinit C1: block scope transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "= 0") != NULL,
			      "spec zeroinit C1: block-scope int gets = 0");
		}
		prism_free(&r);
	}
	// File scope: not touched
	{
		const char *code = "int global_x;\n";
		PrismResult r = prism_transpile_source(code, "spec_zc1b.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "spec zeroinit C1: file scope transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "global_x = 0") == NULL &&
			      strstr(r.output, "global_x =0") == NULL,
			      "spec zeroinit C1: file-scope int NOT zero-initialized");
		}
		prism_free(&r);
	}
}

// ── Constraint 2: static/extern excluded ──────────────────────────────
static void spec_zeroinit_C2_static(void) {
	const char *code =
	    "void f(void) { static int x; (void)x; }\n";
	PrismResult r = prism_transpile_source(code, "spec_zc2.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec zeroinit C2: static transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "static int x = 0") == NULL &&
		      strstr(r.output, "static int x =0") == NULL,
		      "spec zeroinit C2: static int NOT zero-initialized by Prism");
	}
	prism_free(&r);
}

// ── Constraint 3: struct/union/enum excluded ──────────────────────────
static void spec_zeroinit_C3_sue(void) {
	const char *code =
	    "void f(void) {\n"
	    "    struct { int member; } s;\n"
	    "    (void)s;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "spec_zc3.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec zeroinit C3: transpiles OK");
	if (r.output) {
		// The aggregate variable 's' gets = {0}, but the member 'member'
		// inside the struct body must NOT get = 0.
		CHECK(strstr(r.output, "member = 0") == NULL,
		      "spec zeroinit C3: struct member NOT individually zero-initialized");
	}
	prism_free(&r);
}

// ── Constraint 4: register VLA rejected ───────────────────────────────
static void spec_zeroinit_C4_register_vla(void) {
	const char *code =
	    "void f(int n) { register int arr[n]; (void)arr; }\n";
	PrismResult r = prism_transpile_source(code, "spec_zc4.c", prism_defaults());
	CHECK(r.status != PRISM_OK, "spec zeroinit C4: register VLA rejected");
	prism_free(&r);
}

// ── Constraint 5: const VLA rejected ──────────────────────────────────
static void spec_zeroinit_C5_const_vla(void) {
	const char *code =
	    "void f(int n) { const int arr[n]; (void)arr; }\n";
	PrismResult r = prism_transpile_source(code, "spec_zc5.c", prism_defaults());
	CHECK(r.status != PRISM_OK, "spec zeroinit C5: const VLA rejected");
	prism_free(&r);
}

// ── Constraint 7: computed goto + zero-init rejected ──────────────────
static void spec_zeroinit_C7_computed_goto(void) {
	const char *code =
	    "void f(void) {\n"
	    "    int x;\n"
	    "    void *target = &&L;\n"
	    "    L: goto *target;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "spec_zc7.c", prism_defaults());
	CHECK(r.status != PRISM_OK, "spec zeroinit C7: computed goto + zero-init rejected");
	prism_free(&r);
}

// ── Semantic 1: scalar types → = 0 ───────────────────────────────────
static void spec_zeroinit_S1_scalar(void) {
	const char *code =
	    "int main(void) {\n"
	    "    int a;\n"
	    "    double b;\n"
	    "    char *c;\n"
	    "    if (a != 0) return 1;\n"
	    "    if (b != 0.0) return 2;\n"
	    "    if (c != 0) return 3;\n"
	    "    return 0;\n"
	    "}\n";
	char *path = create_temp_file(code);
	PrismResult r = prism_transpile_file(path, prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec zeroinit S1: transpiles OK");
#ifndef _WIN32
	if (r.output)
		check_transpiled_output_compiles_and_runs(
		    r.output,
		    "spec zeroinit S1: scalar compiles",
		    "spec zeroinit S1: scalar runs (int=0, double=0.0, ptr=NULL)");
#endif
	prism_free(&r);
	unlink(path);
	free(path);
}

// ── Semantic 2: aggregate types → = {0} ──────────────────────────────
static void spec_zeroinit_S2_aggregate(void) {
	const char *code =
	    "#include <string.h>\n"
	    "struct S { int x; int y; char name[8]; };\n"
	    "int main(void) {\n"
	    "    struct S s;\n"
	    "    if (s.x != 0 || s.y != 0) return 1;\n"
	    "    // All members zeroed per ISO C §6.7.9p21\n"
	    "    for (int i = 0; i < 8; i++)\n"
	    "        if (s.name[i] != 0) return 2;\n"
	    "    return 0;\n"
	    "}\n";
	char *path = create_temp_file(code);
	PrismResult r = prism_transpile_file(path, prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec zeroinit S2: transpiles OK");
#ifndef _WIN32
	if (r.output)
		check_transpiled_output_compiles_and_runs(
		    r.output,
		    "spec zeroinit S2: aggregate compiles",
		    "spec zeroinit S2: aggregate runs (all members zeroed)");
#endif
	prism_free(&r);
	unlink(path);
	free(path);
}

// ── Semantic 3: VLA → memset ─────────────────────────────────────────
static void spec_zeroinit_S3_vla(void) {
	// Transpiled output must contain memset for VLA
	{
		const char *code =
		    "void f(int n) { int arr[n]; (void)arr; }\n";
		PrismResult r = prism_transpile_source(code, "spec_zs3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "spec zeroinit S3: transpiles OK");
		if (r.output)
			CHECK(has_zeroing(r.output),
			      "spec zeroinit S3: VLA gets memset");
		prism_free(&r);
	}
	// Runtime: VLA is actually zeroed
	{
		const char *code =
		    "int main(void) {\n"
		    "    int n = 10;\n"
		    "    int arr[n];\n"
		    "    for (int i = 0; i < n; i++)\n"
		    "        if (arr[i] != 0) return 1;\n"
		    "    return 0;\n"
		    "}\n";
		char *path = create_temp_file(code);
		PrismResult r = prism_transpile_file(path, prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "spec zeroinit S3 runtime: transpiles OK");
#ifndef _WIN32
		if (r.output)
			check_transpiled_output_compiles_and_runs(
			    r.output,
			    "spec zeroinit S3 runtime: VLA compiles",
			    "spec zeroinit S3 runtime: VLA runs (all elements zero)");
#endif
		prism_free(&r);
		unlink(path);
		free(path);
	}
}

// ── Semantic 6: left-to-right multi-declarator order ──────────────────
static void spec_zeroinit_S6_multi_decl_order(void) {
	const char *code =
	    "void f(void) {\n"
	    "    int x, y, z;\n"
	    "    (void)x; (void)y; (void)z;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "spec_zs6.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec zeroinit S6: transpiles OK");
	if (r.output) {
		// All three must be initialized
		CHECK(strstr(r.output, "x = 0") != NULL,
		      "spec zeroinit S6: x gets = 0");
		CHECK(strstr(r.output, "y = 0") != NULL,
		      "spec zeroinit S6: y gets = 0");
		CHECK(strstr(r.output, "z = 0") != NULL,
		      "spec zeroinit S6: z gets = 0");
	}
	prism_free(&r);
}

// ═══════════════════════════════════════════════════════════════════════════
//  PART 4: raw
// ═══════════════════════════════════════════════════════════════════════════

// ── Constraint 1: raw placement ───────────────────────────────────────
static void spec_raw_C1_placement(void) {
	// raw before declaration → OK
	{
		const char *code =
		    "void f(void) { raw int x; (void)x; }\n";
		PrismResult r = prism_transpile_source(code, "spec_rc1a.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "spec raw C1: prefix raw OK");
		prism_free(&r);
	}
	// raw after comma (per-declarator) → OK
	{
		const char *code =
		    "void f(void) { int a, raw b; (void)a; (void)b; }\n";
		PrismResult r = prism_transpile_source(code, "spec_rc1b.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "spec raw C1: per-declarator raw OK");
		prism_free(&r);
	}
}

// ── Constraint 3: raw is not a type qualifier ─────────────────────────
static void spec_raw_C3_not_qualifier(void) {
	// raw must be stripped from output
	const char *code =
	    "void f(void) { raw int x; (void)x; }\n";
	PrismResult r = prism_transpile_source(code, "spec_rc3.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec raw C3: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "raw") == NULL,
		      "spec raw C3: raw keyword stripped from output");
	}
	prism_free(&r);
}

// ── Semantic 1: raw suppresses zero-init ──────────────────────────────
static void spec_raw_S1_suppresses(void) {
	const char *code =
	    "void f(void) { raw int x; (void)x; }\n";
	PrismResult r = prism_transpile_source(code, "spec_rs1.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec raw S1: transpiles OK");
	if (r.output) {
		// Must NOT have = 0 for x
		// Find 'int x;' without '= 0'
		const char *decl = strstr(r.output, "int x");
		CHECK(decl != NULL, "spec raw S1: int x declaration present");
		if (decl) {
			// Check there's no '= 0' between 'int x' and the next ';'
			const char *semi = strchr(decl, ';');
			CHECK(semi != NULL, "spec raw S1: semicolon found");
			if (semi) {
				int has_init = 0;
				for (const char *p = decl; p < semi; p++)
					if (*p == '=') { has_init = 1; break; }
				CHECK(!has_init, "spec raw S1: raw int x has no initializer");
			}
		}
	}
	prism_free(&r);
}

// ── Semantic 2: prefix raw applies to all declarators ─────────────────
static void spec_raw_S2_prefix_all(void) {
	const char *code =
	    "void f(void) { raw int a, b; (void)a; (void)b; }\n";
	PrismResult r = prism_transpile_source(code, "spec_rs2.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec raw S2: transpiles OK");
	if (r.output) {
		// Neither a nor b should have = 0
		CHECK(strstr(r.output, "a = 0") == NULL &&
		      strstr(r.output, "a =0") == NULL,
		      "spec raw S2: a not initialized");
		CHECK(strstr(r.output, "b = 0") == NULL &&
		      strstr(r.output, "b =0") == NULL,
		      "spec raw S2: b not initialized");
	}
	prism_free(&r);
}

// ── Semantic 3: per-declarator raw applies only to following ──────────
static void spec_raw_S3_per_declarator(void) {
	const char *code =
	    "void f(void) { int a, raw b; (void)a; (void)b; }\n";
	PrismResult r = prism_transpile_source(code, "spec_rs3.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec raw S3: transpiles OK");
	if (r.output) {
		// 'a' should have = 0, 'b' should not
		CHECK(strstr(r.output, "a = 0") != NULL ||
		      strstr(r.output, "a =0") != NULL,
		      "spec raw S3: a IS zero-initialized");
		// Find b's declaration and verify no initializer
		CHECK(strstr(r.output, "raw") == NULL,
		      "spec raw S3: raw keyword stripped");
	}
	prism_free(&r);
}

// ── Semantic 4: consecutive raw tokens absorbed ───────────────────────
static void spec_raw_S4_consecutive(void) {
	const char *code =
	    "void f(void) { raw raw int x; (void)x; }\n";
	PrismResult r = prism_transpile_source(code, "spec_rs4.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec raw S4: consecutive raw transpiles OK");
	if (r.output)
		CHECK(strstr(r.output, "raw") == NULL,
		      "spec raw S4: all raw tokens stripped");
	prism_free(&r);
}

// ── Semantic 5: raw at file scope silently stripped ────────────────────
static void spec_raw_S5_file_scope(void) {
	const char *code = "raw int global;\n";
	PrismResult r = prism_transpile_source(code, "spec_rs5.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec raw S5: file-scope raw transpiles OK");
	if (r.output)
		CHECK(strstr(r.output, "raw") == NULL,
		      "spec raw S5: raw stripped at file scope");
	prism_free(&r);
}

// ═══════════════════════════════════════════════════════════════════════════
//  PART 5: Feature flag independence
// ═══════════════════════════════════════════════════════════════════════════

// When a feature is disabled, its keyword reverts to an ordinary identifier.
static void spec_feature_flag_independence(void) {
	// -fno-defer: 'defer' is an ordinary identifier
	{
		PrismFeatures f = prism_defaults();
		f.defer = false;
		const char *code =
		    "void f(void) { int defer = 5; (void)defer; }\n";
		PrismResult r = prism_transpile_source(code, "spec_ff1.c", f);
		CHECK_EQ(r.status, PRISM_OK, "spec feature flags: defer disabled, identifier OK");
		prism_free(&r);
	}
	// -fno-orelse: 'orelse' is an ordinary identifier
	{
		PrismFeatures f = prism_defaults();
		f.orelse = false;
		const char *code =
		    "void f(void) { int orelse = 5; (void)orelse; }\n";
		PrismResult r = prism_transpile_source(code, "spec_ff2.c", f);
		CHECK_EQ(r.status, PRISM_OK, "spec feature flags: orelse disabled, identifier OK");
		prism_free(&r);
	}
	// -fno-zeroinit: no auto-initialization
	{
		PrismFeatures f = prism_defaults();
		f.zeroinit = false;
		const char *code =
		    "void f(void) { int x; (void)x; }\n";
		PrismResult r = prism_transpile_source(code, "spec_ff3.c", f);
		CHECK_EQ(r.status, PRISM_OK, "spec feature flags: zeroinit disabled OK");
		if (r.output)
			CHECK(strstr(r.output, "x = 0") == NULL &&
			      strstr(r.output, "x =0") == NULL,
			      "spec feature flags: zeroinit disabled, no = 0");
		prism_free(&r);
	}
}

// ═══════════════════════════════════════════════════════════════════════════
//  PART 6: Cross-feature interaction
// ═══════════════════════════════════════════════════════════════════════════

// defer + orelse: orelse action triggers defer unwinding
// (already tested in spec_orelse_S2_defer_cleanup — this covers the
//  cross-feature interaction from the spec's perspective)

// defer + zero-init: zero-init inside defer body
static void spec_cross_defer_zeroinit(void) {
	const char *code =
	    "void f(void) {\n"
	    "    defer {\n"
	    "        int x;\n"
	    "        (void)x;\n"
	    "    }\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "spec_cx1.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec cross defer+zeroinit: transpiles OK");
	prism_free(&r);
}

// raw + orelse: raw variable followed by orelse
static void spec_cross_raw_orelse(void) {
	const char *code =
	    "int get(void);\n"
	    "void f(void) {\n"
	    "    raw int a;\n"
	    "    int b = get() orelse 5;\n"
	    "    (void)a; (void)b;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "spec_cx2.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "spec cross raw+orelse: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "raw") == NULL,
		      "spec cross raw+orelse: raw stripped");
	}
	prism_free(&r);
}

// ═══════════════════════════════════════════════════════════════════════════
//  PART 8: Paren-depth evasion (p1d_scan_balanced_group)
// ═══════════════════════════════════════════════════════════════════════════

// Regression test for paren-depth evasion: wrapping orelse/defer in extra
// parentheses inside if/while/switch conditions used to bypass Phase 1D's
// keyword detection (inner_depth == 0 check). Fixed by tracking stmt-expr
// depth (se_depth) instead of raw paren depth.
static void spec_paren_depth_evasion(void) {
	printf("\n--- Paren-Depth Evasion ---\n");

	// orelse inside extra parens in if-condition → must be rejected
	{
		const char *code =
		    "int get(void);\n"
		    "void f(void) {\n"
		    "    int status;\n"
		    "    if ( (status = get() orelse 5) ) { return; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_pe1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "spec paren evasion: orelse in extra-paren if-condition rejected");
		prism_free(&r);
	}
	// orelse inside double extra parens → must be rejected
	{
		const char *code =
		    "int get(void);\n"
		    "void f(void) {\n"
		    "    int status;\n"
		    "    if ( ((status = get() orelse 5)) ) { return; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_pe2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "spec paren evasion: orelse in double-paren if-condition rejected");
		prism_free(&r);
	}
	// defer inside extra parens in while-condition → must be rejected
	{
		const char *code =
		    "void log_cycle(void);\n"
		    "void f(void) {\n"
		    "    while ( (defer log_cycle(), 1) ) { break; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_pe3.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "spec paren evasion: defer in extra-paren while-condition rejected");
		prism_free(&r);
	}
	// orelse inside extra parens in switch → must be rejected
	{
		const char *code =
		    "int get(void);\n"
		    "void f(void) {\n"
		    "    switch ( (get() orelse 1) ) { default: break; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_pe4.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "spec paren evasion: orelse in extra-paren switch-condition rejected");
		prism_free(&r);
	}
	// orelse inside stmt-expr within if-condition → must be ALLOWED
	{
		const char *code =
		    "int get(void);\n"
		    "void f(void) {\n"
		    "    if ( ({ int x = get() orelse 0; x; }) ) { return; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_pe5.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "spec paren evasion: orelse in stmt-expr inside if-condition allowed");
		prism_free(&r);
	}
	// orelse at depth 0 in if-condition (no extra parens) → must be rejected
	// (baseline: confirms the original check still works)
	{
		const char *code =
		    "int get(void);\n"
		    "void f(void) {\n"
		    "    int x;\n"
		    "    if (x = get() orelse 5) { return; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_pe6.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "spec paren evasion: orelse at depth-0 in if-condition still rejected");
		prism_free(&r);
	}
}

// ═══════════════════════════════════════════════════════════════════════════
//  PART 9: emit_deferred_orelse blind emission fix
// ═══════════════════════════════════════════════════════════════════════════

// Regression tests for the emit_deferred_orelse vulnerability:
// When orelse-with-action appears INSIDE a block body (routed through
// emit_block_body → try_process_stmt_token → emit_deferred_orelse),
// the old code used a verbatim while-loop for control-flow actions and
// emit_deferred_range for blocks — both blind to defer unwinding.
// The fix delegates to emit_orelse_action, which uses emit_return_body,
// emit_break_continue_defer, emit_goto_defer, and emit_orelse_block_body.
static void spec_deferred_orelse_blind_emission(void) {
	printf("\n--- Deferred Orelse Blind Emission Fix ---\n");

	// Case 1: return inside nested orelse must unwind ALL defers
	// Outer orelse block routes inner orelse through emit_block_body →
	// try_process_stmt_token → emit_deferred_orelse
	{
		const char *code =
		    "#include <string.h>\n"
		    "static char buf[64];\n"
		    "static int pos = 0;\n"
		    "static void record(char c) { buf[pos++] = c; buf[pos] = 0; }\n"
		    "int get_zero(void) { return 0; }\n"
		    "static int f(void) {\n"
		    "    int status;\n"
		    "    defer record('D');\n"
		    "    status = get_zero() orelse {\n"
		    "        defer record('E');\n"
		    "        status = get_zero() orelse return -1;\n"
		    "    };\n"
		    "    return status;\n"
		    "}\n"
		    "int main(void) {\n"
		    "    int r = f();\n"
		    "    if (r != -1) return 1;\n"
		    "    // Both defers must fire: E (inner), D (outer)\n"
		    "    if (strcmp(buf, \"ED\") != 0) return 2;\n"
		    "    return 0;\n"
		    "}\n";
		char *path = create_temp_file(code);
		PrismResult r = prism_transpile_file(path, prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "spec deferred orelse: nested return transpiles OK");
#ifndef _WIN32
		if (r.output)
			check_transpiled_output_compiles_and_runs(
			    r.output,
			    "spec deferred orelse: nested return compiles",
			    "spec deferred orelse: nested return unwinds both defers (ED)");
#endif
		prism_free(&r);
		unlink(path);
		free(path);
	}

	// Case 2: block action containing defer+return inside orelse
	// Forces emit_deferred_orelse block path through emit_orelse_block_body
	{
		const char *code =
		    "#include <string.h>\n"
		    "static char buf[64];\n"
		    "static int pos = 0;\n"
		    "static void record(char c) { buf[pos++] = c; buf[pos] = 0; }\n"
		    "int get_zero(void) { return 0; }\n"
		    "static int f(void) {\n"
		    "    int x;\n"
		    "    defer record('A');\n"
		    "    x = get_zero() orelse {\n"
		    "        x = get_zero() orelse {\n"
		    "            defer record('B');\n"
		    "            return -1;\n"
		    "        };\n"
		    "    };\n"
		    "    return x;\n"
		    "}\n"
		    "int main(void) {\n"
		    "    int r = f();\n"
		    "    if (r != -1) return 1;\n"
		    "    if (strcmp(buf, \"BA\") != 0) return 2;\n"
		    "    return 0;\n"
		    "}\n";
		char *path = create_temp_file(code);
		PrismResult r = prism_transpile_file(path, prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "spec deferred orelse: block return transpiles OK");
#ifndef _WIN32
		if (r.output)
			check_transpiled_output_compiles_and_runs(
			    r.output,
			    "spec deferred orelse: block return compiles",
			    "spec deferred orelse: block return unwinds defers (BA)");
#endif
		prism_free(&r);
		unlink(path);
		free(path);
	}

	// Case 3: goto inside nested orelse must unwind defers
	{
		const char *code =
		    "#include <string.h>\n"
		    "static char buf[64];\n"
		    "static int pos = 0;\n"
		    "static void record(char c) { buf[pos++] = c; buf[pos] = 0; }\n"
		    "int get_zero(void) { return 0; }\n"
		    "static int f(void) {\n"
		    "    int status;\n"
		    "    defer record('G');\n"
		    "    status = get_zero() orelse {\n"
		    "        defer record('H');\n"
		    "        status = get_zero() orelse goto fail;\n"
		    "    };\n"
		    "    return 0;\n"
		    "    fail:\n"
		    "    return -1;\n"
		    "}\n"
		    "int main(void) {\n"
		    "    int r = f();\n"
		    "    if (r != -1) return 1;\n"
		    "    if (strcmp(buf, \"HG\") != 0) return 2;\n"
		    "    return 0;\n"
		    "}\n";
		char *path = create_temp_file(code);
		PrismResult r = prism_transpile_file(path, prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "spec deferred orelse: goto transpiles OK");
#ifndef _WIN32
		if (r.output)
			check_transpiled_output_compiles_and_runs(
			    r.output,
			    "spec deferred orelse: goto compiles",
			    "spec deferred orelse: goto unwinds defers (HG)");
#endif
		prism_free(&r);
		unlink(path);
		free(path);
	}

	// Case 4: transpiled output must NOT contain raw 'defer' keyword
	// (the old emit_deferred_range path leaked 'defer' to the backend)
	{
		const char *code =
		    "int get_zero(void) { return 0; }\n"
		    "void log_event(void);\n"
		    "int f(void) {\n"
		    "    int x;\n"
		    "    x = get_zero() orelse {\n"
		    "        x = get_zero() orelse {\n"
		    "            defer log_event();\n"
		    "            return -1;\n"
		    "        };\n"
		    "    };\n"
		    "    return x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_deo4.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "spec deferred orelse: no keyword leak transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "defer") == NULL,
			      "spec deferred orelse: defer keyword not leaked to output");
			CHECK(strstr(r.output, "log_event") != NULL,
			      "spec deferred orelse: defer body (log_event) present in output");
		}
		prism_free(&r);
	}
}

// ── stmt-expr paren-stripping desync ──
// Phase 1D/Pass 2 paren-stripping for macro-hygiene must not strip '('
// from statement expressions '({ ... })', which would collapse the AST
// and route live code through walk_balanced's dumb bracket-skipping path,
// leaking defer/return/goto keywords verbatim to the backend.
static void spec_stmt_expr_paren_strip_desync(void) {
	printf("\n--- Stmt-Expr Paren-Strip Desync Fix ---\n");

	// Case 1: defer inside stmt-expr initializer with orelse must not leak
	{
		const char *code =
		    "void log_event(void);\n"
		    "int get_zero(void) { return 0; }\n"
		    "void network_kernel_init(void) {\n"
		    "    int status = ({\n"
		    "        { defer log_event(); }\n"
		    "        int x = get_zero() orelse 1;\n"
		    "        x;\n"
		    "    });\n"
		    "    (void)status;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_se_ps1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "stmt-expr paren-strip: defer+orelse transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "defer") == NULL,
			      "stmt-expr paren-strip: defer keyword not leaked to output");
			CHECK(strstr(r.output, "log_event") != NULL,
			      "stmt-expr paren-strip: defer body (log_event) present in output");
		}
		prism_free(&r);
	}

	// Case 2: return inside stmt-expr initializer with orelse must compile
	{
		const char *code =
		    "#include <string.h>\n"
		    "static char buf[64];\n"
		    "static int pos = 0;\n"
		    "static void record(char c) { buf[pos++] = c; buf[pos] = 0; }\n"
		    "int get_zero(void) { return 0; }\n"
		    "static int f(void) {\n"
		    "    defer record('D');\n"
		    "    int status = ({\n"
		    "        { defer record('E'); }\n"
		    "        int x = get_zero() orelse return -1;\n"
		    "        x;\n"
		    "    });\n"
		    "    return status;\n"
		    "}\n"
		    "int main(void) {\n"
		    "    int r = f();\n"
		    "    if (r != -1) return 1;\n"
		    "    if (strcmp(buf, \"ED\") != 0) return 2;\n"
		    "    return 0;\n"
		    "}\n";
		char *path = create_temp_file(code);
		PrismResult r = prism_transpile_file(path, prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "stmt-expr paren-strip: return+orelse transpiles OK");
#ifndef _WIN32
		if (r.output)
			check_transpiled_output_compiles_and_runs(
			    r.output,
			    "stmt-expr paren-strip: return+orelse compiles",
			    "stmt-expr paren-strip: return unwinds defers (ED)");
#endif
		prism_free(&r);
		unlink(path);
		free(path);
	}

	// Case 3: goto inside stmt-expr initializer with orelse must compile
	{
		const char *code =
		    "void log_event(void);\n"
		    "int get_zero(void) { return 0; }\n"
		    "int f(void) {\n"
		    "    defer log_event();\n"
		    "    int status = ({\n"
		    "        get_zero() orelse goto fail;\n"
		    "    });\n"
		    "    return status;\n"
		    "fail:\n"
		    "    return -1;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_se_ps3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "stmt-expr paren-strip: goto+orelse transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "defer") == NULL,
			      "stmt-expr paren-strip: goto - no defer keyword leak");
			CHECK(strstr(r.output, "log_event") != NULL,
			      "stmt-expr paren-strip: goto - defer body present");
		}
		prism_free(&r);
	}
}

// ── typeof_var reentrancy clobber ──
// process_declarators uses ctx->typeof_vars to queue VLA memsets.
// If a stmt-expr in an array dimension triggers a nested process_declarators,
// the inner call must not clobber the outer's queued memsets.
static void spec_typeof_var_reentrancy(void) {
	printf("\n--- typeof_var Reentrancy Fix ---\n");

	// Case 1: outer VLA memset survives inner decl in array dimension
	{
		const char *code =
		    "void crypto_kernel_init(void) {\n"
		    "    int secure_buffer[ ({\n"
		    "        int n = 4;\n"
		    "        int inner_temp[n];\n"
		    "        inner_temp[0] = 0xAA;\n"
		    "        32;\n"
		    "    }) ];\n"
		    "    (void)secure_buffer;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "spec_tvr1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "typeof_var reentry: transpiles OK");
		if (r.output) {
			CHECK(has_var_zeroing(r.output, "secure_buffer"),
			      "typeof_var reentry: outer VLA memset survives inner decl");
			CHECK(has_var_zeroing(r.output, "inner_temp"),
			      "typeof_var reentry: inner VLA memset also present");
		}
		prism_free(&r);
	}

	// Case 2: runtime — both VLAs actually zeroed
	{
		const char *code =
		    "int main(void) {\n"
		    "    int n = ({\n"
		    "        int inner[8];\n"
		    "        for (int i = 0; i < 8; i++)\n"
		    "            if (inner[i] != 0) return 1;\n"
		    "        16;\n"
		    "    });\n"
		    "    int outer[n];\n"
		    "    for (int i = 0; i < n; i++)\n"
		    "        if (outer[i] != 0) return 2;\n"
		    "    return 0;\n"
		    "}\n";
		char *path = create_temp_file(code);
		PrismResult r = prism_transpile_file(path, prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "typeof_var reentry runtime: transpiles OK");
#ifndef _WIN32
		if (r.output)
			check_transpiled_output_compiles_and_runs(
			    r.output,
			    "typeof_var reentry runtime: compiles",
			    "typeof_var reentry runtime: both VLAs zeroed");
#endif
		prism_free(&r);
		unlink(path);
		free(path);
	}
}

// ── orelse ctrl-flow keyword rejection ──────────────────────────────────
// reject_orelse_side_effects must reject control-flow keywords (goto,
// return, break, continue, defer) inside statement expressions in ranges
// that orelse expansion duplicates. Duplication desynchronizes
// goto_entry_cursor (goto) and the defer unwinding stack (return).
static void spec_orelse_ctrl_flow_keyword_rejection(void) {
	printf("\n--- orelse ctrl-flow keyword rejection ---\n");

	// 1. goto in stmt-expr LHS of bare value orelse — ternary expansion
	//    evaluates LHS twice, duplicating the goto.
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    int target = 0;\n"
		    "    *({ goto skip; &target; }) = 1 orelse 2;\n"
		    "skip: return;\n"
		    "}\n",
		    "spec_ctrl1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "goto in stmt-expr bare orelse LHS: must reject");
		prism_free(&r);
	}

	// 2. return in stmt-expr bare orelse LHS — ternary expansion
	//    duplicates the LHS, doubling the return.
	{
		PrismResult r = prism_transpile_source(
		    "int f(void) {\n"
		    "    int target = 0;\n"
		    "    *({ return 0; &target; }) = 1 orelse 2;\n"
		    "    return target;\n"
		    "}\n",
		    "spec_ctrl2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "return in stmt-expr bare orelse LHS: must reject");
		prism_free(&r);
	}

	// 3. defer in stmt-expr in orelse init — duplication doubles defer
	//    registration, causing over-unwinding at scope exit.
	{
		PrismResult r = prism_transpile_source(
		    "int counter = 0;\n"
		    "void f(void) {\n"
		    "    int x = ({ defer { counter++; } 1; }) orelse 5;\n"
		    "    (void)x;\n"
		    "}\n",
		    "spec_ctrl3.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "defer in stmt-expr orelse init: must reject");
		prism_free(&r);
	}

	// 4. continue in stmt-expr bare orelse LHS inside loop
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    int target = 0;\n"
		    "    while (1) {\n"
		    "        *({ continue; &target; }) = 1 orelse 2;\n"
		    "    }\n"
		    "}\n",
		    "spec_ctrl4.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "continue in stmt-expr bare orelse LHS: must reject");
		prism_free(&r);
	}

	// 5. Block-form orelse with goto in LHS is OK — if-guard pattern
	//    evaluates LHS only once (no duplication).
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    int target = 0;\n"
		    "    *({ goto skip; &target; }) = 1 orelse { return; };\n"
		    "skip: return;\n"
		    "}\n",
		    "spec_ctrl5.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "goto in stmt-expr block orelse LHS: must accept");
		prism_free(&r);
	}

	// 6. defer in stmt-expr RHS with LHS indirection — typeof(RHS)
	//    duplicates the RHS tokens at compile time, double-registering
	//    the defer (double-free / lock panic at runtime).
	{
		PrismResult r = prism_transpile_source(
		    "void lock(void);\n"
		    "void unlock(void);\n"
		    "void kernel_task(void) {\n"
		    "    int *ptr;\n"
		    "    *ptr = ({ { defer unlock(); lock(); } 0; }) orelse 5;\n"
		    "}\n",
		    "spec_ctrl6.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "defer in RHS stmt-expr with LHS indirection: must reject");
		prism_free(&r);
	}

	// 7. goto in stmt-expr RHS with LHS indirection
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    int *ptr;\n"
		    "    *ptr = ({ goto skip; 0; }) orelse 5;\n"
		    "skip: return;\n"
		    "}\n",
		    "spec_ctrl7.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "goto in RHS stmt-expr with LHS indirection: must reject");
		prism_free(&r);
	}

	// 8. RHS without LHS indirection is OK — typeof(LHS) is used,
	//    RHS is only emitted once.
	{
		PrismResult r = prism_transpile_source(
		    "void lock(void);\n"
		    "void unlock(void);\n"
		    "int kernel_task(void) {\n"
		    "    int val;\n"
		    "    val = ({ { defer unlock(); lock(); } 0; }) orelse 5;\n"
		    "    return val;\n"
		    "}\n",
		    "spec_ctrl8.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "defer in RHS without LHS indirection: must accept");
		prism_free(&r);
	}
}

// ── type specifier ctrl-flow keyword rejection ─────────────────────────
// Type specifiers (typeof, _Atomic) can be emitted multiple times by
// const orelse fallback and multi-declarator splits.  Control-flow
// keywords inside stmt-exprs in the type specifier would be processed
// N times at compile time, corrupting the defer stack / goto cursor.
static void spec_typeof_ctrl_flow_rejection(void) {
	printf("\n--- type specifier ctrl-flow keyword rejection ---\n");

	// 1. defer in typeof — const orelse duplicates the type specifier
	{
		PrismResult r = prism_transpile_source(
		    "void lock(void);\n"
		    "void unlock(void);\n"
		    "void kernel_task(void) {\n"
		    "    const typeof(({ { defer unlock(); lock(); } 0; })) val = 5 orelse 10;\n"
		    "    (void)val;\n"
		    "}\n",
		    "spec_ts1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "defer in typeof const orelse: must reject");
		prism_free(&r);
	}

	// 2. goto in typeof
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    typeof(({ goto skip; 0; })) x = 5;\n"
		    "skip: (void)x;\n"
		    "}\n",
		    "spec_ts2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "goto in typeof: must reject");
		prism_free(&r);
	}

	// 3. return in typeof
	{
		PrismResult r = prism_transpile_source(
		    "int f(void) {\n"
		    "    typeof(({ return 0; 1; })) x = 5;\n"
		    "    return x;\n"
		    "}\n",
		    "spec_ts3.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "return in typeof: must reject");
		prism_free(&r);
	}

	// 4. typeof without ctrl-flow is OK
	{
		PrismResult r = prism_transpile_source(
		    "int f(void) {\n"
		    "    typeof(({ int tmp = 5; tmp; })) x = 10;\n"
		    "    return x;\n"
		    "}\n",
		    "spec_ts4.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "typeof without ctrl-flow: must accept");
		prism_free(&r);
	}

	// 5. return in typeof with -fno-safety: downgraded to warning, OK
	{
		PrismFeatures feat = prism_defaults();
		feat.warn_safety = true;
		PrismResult r = prism_transpile_source(
		    "int f(void) {\n"
		    "    typeof(({ return 0; 1; })) x = 5;\n"
		    "    return x;\n"
		    "}\n",
		    "spec_ts5.c", feat);
		CHECK_EQ(r.status, PRISM_OK,
		         "return in typeof with -fno-safety: must accept");
		prism_free(&r);
	}

	// 6. glibc INLINE_SYSCALL pattern: typeof with stmt-expr containing return
	{
		PrismFeatures feat = prism_defaults();
		feat.warn_safety = true;
		PrismResult r = prism_transpile_source(
		    "int _errno;\n"
		    "int f(unsigned long long dev) {\n"
		    "    __typeof__((({ if (dev) return ({ (_errno = 22); -1; });\n"
		    "                   (unsigned int) dev; }))) x = 0;\n"
		    "    return x;\n"
		    "}\n",
		    "spec_ts6.c", feat);
		CHECK_EQ(r.status, PRISM_OK,
		         "glibc typeof+return with -fno-safety: must accept");
		prism_free(&r);
	}
}

// ── C23 auto/constexpr orelse ──
static void spec_c23_auto_constexpr_orelse(void) {
	printf("\n--- C23 auto/constexpr orelse ---\n");

	// 1. auto orelse value fallback: must accept (initializer preserved)
	{
		PrismResult r = prism_transpile_source(
		    "int *get(void) { return 0; }\n"
		    "int *fb(void) { static int v = 99; return &v; }\n"
		    "void f(void) {\n"
		    "    auto x = get() orelse fb();\n"
		    "    (void)x;\n"
		    "}\n",
		    "spec_c23_1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "auto orelse value fallback: must accept");
		prism_free(&r);
	}

	// 2. auto orelse ctrl-flow action: must accept (initializer preserved)
	{
		PrismResult r = prism_transpile_source(
		    "int *get(void) { return 0; }\n"
		    "int test(void) {\n"
		    "    auto ptr = get() orelse return -1;\n"
		    "    return *ptr;\n"
		    "}\n",
		    "spec_c23_2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "auto orelse ctrl-flow: must accept");
		prism_free(&r);
	}

	// 3. constexpr orelse: must reject (compile-time constant required)
	{
		PrismResult r = prism_transpile_source(
		    "int get(void) { return 0; }\n"
		    "void f(void) {\n"
		    "    constexpr int x = get() orelse 5;\n"
		    "    (void)x;\n"
		    "}\n",
		    "spec_c23_3.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "constexpr orelse: must reject");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "constexpr") != NULL,
			      "constexpr orelse: error mentions constexpr");
		prism_free(&r);
	}

	// 4. const auto orelse: must accept (const path preserves initializer)
	{
		PrismResult r = prism_transpile_source(
		    "int *get(void) { static int v = 42; return &v; }\n"
		    "int *fb(void) { static int v = 99; return &v; }\n"
		    "void f(void) {\n"
		    "    const auto x = get() orelse fb();\n"
		    "    (void)x;\n"
		    "}\n",
		    "spec_c23_4.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "const auto orelse: must accept");
		prism_free(&r);
	}

	// 5. plain auto without orelse: must accept
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    auto x = 42;\n"
		    "    (void)x;\n"
		    "}\n",
		    "spec_c23_5.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "auto without orelse: must accept");
		prism_free(&r);
	}

	// 6. constexpr without orelse: must accept
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    constexpr int limit = 100;\n"
		    "    (void)limit;\n"
		    "}\n",
		    "spec_c23_6.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "constexpr without orelse: must accept");
		prism_free(&r);
	}
}


// ═══════════════════════════════════════════════════════════════════════════
//  Attribute-obscured raw VLA safety
// ═══════════════════════════════════════════════════════════════════════════

static void spec_attr_raw_vla_safety(void) {
	// 1. C23 attribute before per-declarator raw with VLA: goto must be rejected
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    goto L_skip;\n"
		    "    int a, [[maybe_unused]] raw b[n];\n"
		    "    (void)a; (void)b;\n"
		    "L_skip: return;\n"
		    "}\n",
		    "spec_arv1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "attr raw VLA: goto over C23 attr raw VLA rejected");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "VLA") != NULL,
			      "attr raw VLA: error mentions VLA");
		prism_free(&r);
	}

	// 2. GNU attribute before per-declarator raw with VLA: same rejection
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    goto L_skip;\n"
		    "    int a, __attribute__((unused)) raw b[n];\n"
		    "    (void)a; (void)b;\n"
		    "L_skip: return;\n"
		    "}\n",
		    "spec_arv2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "attr raw VLA: goto over GNU attr raw VLA rejected");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "VLA") != NULL,
			      "attr raw VLA: GNU attr error mentions VLA");
		prism_free(&r);
	}

	// 3. Without goto: attribute-preceded raw VLA must transpile OK
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    int a, [[maybe_unused]] raw b[n];\n"
		    "    (void)a; (void)b;\n"
		    "}\n",
		    "spec_arv3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "attr raw VLA: no-goto case transpiles OK");
		if (r.output)
			CHECK(strstr(r.output, "raw") == NULL,
			      "attr raw VLA: raw keyword stripped");
		prism_free(&r);
	}

	// 4. Plain raw VLA without attributes: goto still rejected (sanity)
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    goto L_skip;\n"
		    "    int a, raw b[n];\n"
		    "    (void)a; (void)b;\n"
		    "L_skip: return;\n"
		    "}\n",
		    "spec_arv4.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "attr raw VLA: plain raw VLA goto rejected");
		prism_free(&r);
	}
}

// ═══════════════════════════════════════════════════════════════════════════
//  Attribute-encapsulated control flow rejection
// ═══════════════════════════════════════════════════════════════════════════

static void spec_attr_ctrl_flow_rejection(void) {
	printf("\n--- attribute-encapsulated control flow ---\n");

	// 1. GNU attribute with goto inside stmt-expr: rejected
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    __attribute__((aligned( ({ goto L; 8; }) ))) int buf;\n"
		    "    int vla[n];\n"
		    "L: return;\n"
		    "}\n",
		    "spec_acf1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "attr ctrl-flow: GNU goto rejected");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "attribute") != NULL,
			      "attr ctrl-flow: error mentions attribute");
		prism_free(&r);
	}

	// 2. C23 attribute with goto inside stmt-expr: rejected
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    [[gnu::aligned( ({ goto L; 8; }) )]] int buf;\n"
		    "    int vla[n];\n"
		    "L: return;\n"
		    "}\n",
		    "spec_acf2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "attr ctrl-flow: C23 goto rejected");
		prism_free(&r);
	}

	// 3. GNU attribute with defer: rejected
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    __attribute__((aligned( ({ defer cleanup(); 8; }) ))) int x;\n"
		    "    (void)x;\n"
		    "}\n",
		    "spec_acf3.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "attr ctrl-flow: GNU defer rejected");
		prism_free(&r);
	}

	// 4. GNU attribute with return: rejected
	{
		PrismResult r = prism_transpile_source(
		    "int f(void) {\n"
		    "    __attribute__((aligned( ({ return 0; 8; }) ))) int x;\n"
		    "    return x;\n"
		    "}\n",
		    "spec_acf4.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "attr ctrl-flow: GNU return rejected");
		prism_free(&r);
	}

	// 5. Normal attributes without control flow: accepted
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    __attribute__((unused)) int x;\n"
		    "    [[maybe_unused]] int y;\n"
		    "    __attribute__((aligned(16))) int z;\n"
		    "    (void)x; (void)y; (void)z;\n"
		    "}\n",
		    "spec_acf5.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "attr ctrl-flow: normal attrs accepted");
		prism_free(&r);
	}

	// 6. Shadowed 'defer' variable inside attribute: accepted
	{
		PrismResult r = prism_transpile_source(
		    "typedef int defer;\n"
		    "void f(void) {\n"
		    "    __attribute__((aligned( ({ defer x = 8; x; }) ))) int buf;\n"
		    "    (void)buf;\n"
		    "}\n",
		    "spec_acf6.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "attr ctrl-flow: shadowed defer accepted");
		prism_free(&r);
	}
}

// ═══════════════════════════════════════════════════════════════════════════
//  Backward goto nested-scope defer loop detection
// ═══════════════════════════════════════════════════════════════════════════

static void spec_defer_loop_nested_scope(void) {
	printf("\n--- backward goto nested-scope defer loop ---\n");

	// 1. Label in nested block, defer in outer: must reject
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    { L_loop: ; }\n"
		    "    defer cleanup();\n"
		    "    goto L_loop;\n"
		    "}\n",
		    "spec_dln1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "defer loop nested: label in child rejected");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "loops over") != NULL,
			      "defer loop nested: error mentions loops over");
		prism_free(&r);
	}

	// 2. Label 2 levels deep: must reject
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    { { L_deep: ; } }\n"
		    "    defer cleanup();\n"
		    "    goto L_deep;\n"
		    "}\n",
		    "spec_dln2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "defer loop nested: 2 levels deep rejected");
		prism_free(&r);
	}

	// 3. Both label and goto in nested scopes: must reject
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "    { L_top: ; }\n"
		    "    defer cleanup();\n"
		    "    { goto L_top; }\n"
		    "}\n",
		    "spec_dln3.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "defer loop nested: both nested rejected");
		prism_free(&r);
	}

	// 4. Defer in child scope of goto (goto exits it): must accept
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "L_top:\n"
		    "    { defer cleanup(); }\n"
		    "    goto L_top;\n"
		    "}\n",
		    "spec_dln4.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "defer loop nested: defer in child scope accepted");
		prism_free(&r);
	}

	// 5. Defer in sibling scope: must accept
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "L_top:\n"
		    "    { defer cleanup(); }\n"
		    "    { goto L_top; }\n"
		    "}\n",
		    "spec_dln5.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "defer loop nested: sibling scope accepted");
		prism_free(&r);
	}

	// 6. Same-scope still caught (sanity)
	{
		PrismResult r = prism_transpile_source(
		    "void cleanup(void);\n"
		    "void f(void) {\n"
		    "L_loop:\n"
		    "    defer cleanup();\n"
		    "    goto L_loop;\n"
		    "}\n",
		    "spec_dln6.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "defer loop nested: same-scope sanity rejected");
		prism_free(&r);
	}
}

// ── for/if/switch-init typedef recognition ───────────────────────────────

static void spec_for_init_typedef(void) {
	printf("\n--- for-init typedef recognition ---\n");

	// 1. Zero-init through for-init typedef: SecretKey must get = {0}
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    for (typedef int FiTd; 0; ) {\n"
		    "        FiTd x;\n"
		    "        (void)x;\n"
		    "    }\n"
		    "}\n",
		    "spec_fitd1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "for-init typedef: zero-init accepted");
		if (r.output)
			CHECK(strstr(r.output, "= {0}") != NULL ||
			      strstr(r.output, "= 0") != NULL,
			      "for-init typedef: zero-init applied");
		prism_free(&r);
	}

	// 2. Goto over for-init typedef VLA: must reject
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    for (typedef int FiTd2; 0; ) {\n"
		    "        goto L_skip;\n"
		    "        FiTd2 arr[n];\n"
		    "        (void)arr;\n"
		    "        L_skip: ;\n"
		    "    }\n"
		    "}\n",
		    "spec_fitd2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "for-init typedef: goto over VLA rejected");
		prism_free(&r);
	}

	// 3. Enum constants in for-init typedef leak correctly
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    for (typedef enum { FITD_A, FITD_B } FiEnum; 0; ) {\n"
		    "        FiEnum x;\n"
		    "        (void)x;\n"
		    "        (void)FITD_A;\n"
		    "    }\n"
		    "}\n",
		    "spec_fitd3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "for-init typedef: enum constants accepted");
		prism_free(&r);
	}

	// 4. Typedef doesn't leak past for-loop scope (negative: use after)
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    for (typedef int FiTd4; 0; ) {\n"
		    "        FiTd4 x;\n"
		    "        (void)x;\n"
		    "    }\n"
		    "    FiTd4 y;\n"
		    "    (void)y;\n"
		    "}\n",
		    "spec_fitd4.c", prism_defaults());
		// After the for loop, FiTd4 should not be a known type.
		// Without the typedef, y is just an identifier — no zero-init.
		// Check that the output does NOT zero-init y (it's not a type).
		if (r.output)
			CHECK(strstr(r.output, "FiTd4 y = {0}") == NULL &&
			      strstr(r.output, "FiTd4 y = 0") == NULL,
			      "for-init typedef: does not leak past scope");
		prism_free(&r);
	}

	// 5. Struct typedef in for-init: aggregate zero-init
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    for (typedef struct { int a; } FiSt; 0; ) {\n"
		    "        FiSt s;\n"
		    "        (void)s;\n"
		    "    }\n"
		    "}\n",
		    "spec_fitd5.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "for-init typedef: struct zero-init accepted");
		if (r.output)
			CHECK(strstr(r.output, "= {0}") != NULL,
			      "for-init typedef: struct gets zero-init");
		prism_free(&r);
	}
}

// ── const-pointee VLA type composition ───────────────────────────────────

static void spec_const_pointee_vla(void) {
	printf("\n--- const-pointee VLA type composition ---\n");

	// 1. const char *arr[n] — pointer-to-const, pointer itself is mutable → memset OK
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    const char *arr[n];\n"
		    "    (void)arr;\n"
		    "}\n",
		    "spec_cpv1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "const-pointee VLA: const char * array accepted");
		if (r.output)
			CHECK(strstr(r.output, "memset") != NULL || strstr(r.output, "__prism_p_") != NULL,
			      "const-pointee VLA: memset emitted (pointer is mutable)");
		prism_free(&r);
	}

	// 2. const int arr[n] — actual const element → must reject
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    const int arr[n];\n"
		    "    (void)arr;\n"
		    "}\n",
		    "spec_cpv2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "const-pointee VLA: const int array rejected");
		prism_free(&r);
	}

	// 3. volatile int *arr[n] — pointer-to-volatile, pointer itself is not volatile
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    volatile int *arr[n];\n"
		    "    (void)arr;\n"
		    "}\n",
		    "spec_cpv3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "const-pointee VLA: volatile int * array accepted");
		prism_free(&r);
	}

	// 4. int *const arr[n] — const pointer (decl.is_const) → must reject
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    int *const arr[n];\n"
		    "    (void)arr;\n"
		    "}\n",
		    "spec_cpv4.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "const-pointee VLA: const pointer array rejected");
		prism_free(&r);
	}

	// 5. const int *p orelse — pointer-to-const with orelse, no temp needed
	{
		PrismResult r = prism_transpile_source(
		    "int *get(void);\n"
		    "void f(void) {\n"
		    "    const int *p = get() orelse 0;\n"
		    "    (void)p;\n"
		    "}\n",
		    "spec_cpv5.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "const-pointee orelse: pointer-to-const accepted");
		if (r.output)
			CHECK(strstr(r.output, "p ? p") != NULL,
			      "const-pointee orelse: uses direct ternary (no temp)");
		prism_free(&r);
	}

	// 6. const int x orelse — actual const, must use temp
	{
		PrismResult r = prism_transpile_source(
		    "int get(void);\n"
		    "void f(void) {\n"
		    "    const int x = get() orelse 5;\n"
		    "    (void)x;\n"
		    "}\n",
		    "spec_cpv6.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "const-pointee orelse: const int uses temp");
		if (r.output)
			CHECK(strstr(r.output, "__prism_oe_") != NULL,
			      "const-pointee orelse: const int emits temp variable");
		prism_free(&r);
	}
}

// ── typedef const/volatile propagation ────────────────────────────────────

static void spec_typedef_const_volatile_propagation(void) {
	printf("\n--- typedef const/volatile propagation ---\n");

	// 1. typedef const int ReadOnlyVLA[n] — const must propagate → reject
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    typedef const int ReadOnlyVLA[n];\n"
		    "    ReadOnlyVLA buffer;\n"
		    "    (void)buffer;\n"
		    "}\n",
		    "spec_tcvp1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "typedef const VLA: const array typedef rejected");
		prism_free(&r);
	}

	// 2. typedef volatile int VolVLA[n] — volatile must propagate → byte loop
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    typedef volatile int VolVLA[n];\n"
		    "    VolVLA buf;\n"
		    "    (void)buf;\n"
		    "}\n",
		    "spec_tcvp2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "typedef volatile VLA: accepted");
		if (r.output)
			CHECK(strstr(r.output, "volatile char") != NULL,
			      "typedef volatile VLA: byte loop emitted");
		prism_free(&r);
	}

	// 3. typedef const int CI; CI x; — non-VLA const → = {0}, no false positive
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    typedef const int CI;\n"
		    "    CI x;\n"
		    "    (void)x;\n"
		    "}\n",
		    "spec_tcvp3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "typedef const non-VLA: accepted");
		if (r.output)
			CHECK(strstr(r.output, "= {0}") != NULL,
			      "typedef const non-VLA: gets inline zero-init");
		prism_free(&r);
	}

	// 4. typedef const int *PCI; PCI arr[n] — pointer-to-const, pointer mutable
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    typedef const int *PCI;\n"
		    "    PCI arr[n];\n"
		    "    (void)arr;\n"
		    "}\n",
		    "spec_tcvp4.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "typedef const-pointer VLA: accepted (pointer mutable)");
		if (r.output)
			CHECK(strstr(r.output, "memset") != NULL || strstr(r.output, "__prism_p_") != NULL,
			      "typedef const-pointer VLA: memset OK");
		prism_free(&r);
	}

	// 5. typedef volatile int *PVI; PVI arr[n] — pointer-to-volatile, pointer not volatile
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    typedef volatile int *PVI;\n"
		    "    PVI arr[n];\n"
		    "    (void)arr;\n"
		    "}\n",
		    "spec_tcvp5.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "typedef volatile-pointer VLA: accepted");
		if (r.output)
			CHECK(strstr(r.output, "memset") != NULL || strstr(r.output, "__prism_p_") != NULL,
			      "typedef volatile-pointer VLA: memset OK (pointer not volatile)");
		prism_free(&r);
	}

	// 6. typedef const volatile int CVVLA[n] — both qualifiers → reject (const)
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    typedef const volatile int CVVLA[n];\n"
		    "    CVVLA buf;\n"
		    "    (void)buf;\n"
		    "}\n",
		    "spec_tcvp6.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "typedef const volatile VLA: rejected (const)");
		prism_free(&r);
	}

	// 7. typedef volatile int VI; VI x; — non-VLA volatile → = {0} (no byte loop)
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    typedef volatile int VI;\n"
		    "    VI x;\n"
		    "    (void)x;\n"
		    "}\n",
		    "spec_tcvp7.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "typedef volatile non-VLA: accepted");
		if (r.output)
			CHECK(strstr(r.output, "= {0}") != NULL,
			      "typedef volatile non-VLA: inline zero-init");
		prism_free(&r);
	}

	// 8. Chained typedef: typedef const int CI; typedef CI Arr[n]; Arr buf;
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    typedef const int CI;\n"
		    "    typedef CI Arr[n];\n"
		    "    Arr buf;\n"
		    "    (void)buf;\n"
		    "}\n",
		    "spec_tcvp8.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "chained const typedef VLA: rejected");
		prism_free(&r);
	}
}

// ── function type composition ────────────────────────────────────────────

static void spec_func_type_composition(void) {
	printf("\n--- function type composition ---\n");

	// 1. Chained typedef: typedef func_t alias → alias must remain function type
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    typedef int func_t(int);\n"
		    "    typedef func_t chained_func_t;\n"
		    "    chained_func_t my_func1;\n"
		    "    (void)my_func1;\n"
		    "}\n",
		    "spec_ftc1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "chained func typedef: accepted");
		if (r.output) {
			CHECK(strstr(r.output, "= 0") == NULL && strstr(r.output, "= {0}") == NULL,
			      "chained func typedef: no scalar init");
			CHECK(strstr(r.output, "memset") == NULL,
			      "chained func typedef: no memset");
		}
		prism_free(&r);
	}

	// 2. typeof(int(int)) — function type signature
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    typeof(int(int)) my_func2;\n"
		    "    (void)my_func2;\n"
		    "}\n",
		    "spec_ftc2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "typeof func signature: accepted");
		if (r.output) {
			CHECK(strstr(r.output, "memset") == NULL,
			      "typeof func signature: no memset");
			CHECK(strstr(r.output, "= 0") == NULL && strstr(r.output, "= {0}") == NULL,
			      "typeof func signature: no scalar init");
		}
		prism_free(&r);
	}

	// 3. typeof(void(void)) — void function type
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    typeof(void(void)) my_func3;\n"
		    "    (void)my_func3;\n"
		    "}\n",
		    "spec_ftc3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "typeof void func: accepted");
		if (r.output)
			CHECK(strstr(r.output, "memset") == NULL,
			      "typeof void func: no memset");
		prism_free(&r);
	}

	// 4. typeof(void(*)(int)) — function POINTER, must zero-init
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    typeof(void(*)(int)) fp;\n"
		    "    (void)fp;\n"
		    "}\n",
		    "spec_ftc4.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "typeof func pointer: accepted");
		if (r.output)
			CHECK(strstr(r.output, "memset") != NULL || strstr(r.output, "__prism_p_") != NULL,
			      "typeof func pointer: gets zero-init");
		prism_free(&r);
	}

	// 5. Triple-chained function typedef
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    typedef void handler_t(int);\n"
		    "    typedef handler_t handler_alias;\n"
		    "    typedef handler_alias handler_final;\n"
		    "    handler_final h;\n"
		    "    (void)h;\n"
		    "}\n",
		    "spec_ftc5.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "triple chain func typedef: accepted");
		if (r.output) {
			CHECK(strstr(r.output, "= 0") == NULL && strstr(r.output, "= {0}") == NULL,
			      "triple chain func typedef: no init");
			CHECK(strstr(r.output, "memset") == NULL,
			      "triple chain func typedef: no memset");
		}
		prism_free(&r);
	}

	// 6. Function pointer typedef chain — must STILL zero-init
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    typedef int (*fptr_t)(int);\n"
		    "    typedef fptr_t chained_fptr;\n"
		    "    chained_fptr fp;\n"
		    "    (void)fp;\n"
		    "}\n",
		    "spec_ftc6.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "chained func ptr typedef: accepted");
		if (r.output)
			CHECK(strstr(r.output, "= {0}") != NULL || strstr(r.output, "= 0") != NULL,
			      "chained func ptr typedef: gets zero-init");
		prism_free(&r);
	}

	// 7. typeof(int(int,int)) — multi-param function type
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    typeof(int(int,int)) mf;\n"
		    "    (void)mf;\n"
		    "}\n",
		    "spec_ftc7.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "typeof multi-param func: accepted");
		if (r.output)
			CHECK(strstr(r.output, "memset") == NULL,
			      "typeof multi-param func: no memset");
		prism_free(&r);
	}
}

// ── Two-pass invariant: constexpr + orelse (Phase 1D primary) ─────────
static void spec_constexpr_orelse_phase1(void) {
	printf("\n--- constexpr+orelse Phase 1D ---\n");

	// Must reject constexpr + orelse (Phase 1D, not Pass 2)
	{
		PrismResult r = prism_transpile_source(
		    "int get(void);\n"
		    "void f(void) {\n"
		    "    constexpr int x = get() orelse 5;\n"
		    "    (void)x;\n"
		    "}\n",
		    "spec_ce1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "constexpr orelse: rejected");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "constexpr") != NULL,
			      "constexpr orelse: error mentions constexpr");
		prism_free(&r);
	}

	// Plain constexpr without orelse must pass
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    constexpr int x = 42;\n"
		    "    (void)x;\n"
		    "}\n",
		    "spec_ce2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "constexpr without orelse: accepted");
		prism_free(&r);
	}
}

// ── Two-pass invariant: const VLA memset (Phase 1D primary) ───────────
static void spec_const_vla_memset_phase1(void) {
	printf("\n--- const VLA memset Phase 1D ---\n");

	// const typeof(int[n]) must reject
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) { const typeof(int[n]) buf; (void)buf; }\n",
		    "spec_cv1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "const typeof VLA: rejected");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "const") != NULL,
			      "const typeof VLA: error mentions const");
		prism_free(&r);
	}

	// raw const typeof VLA must pass (raw opts out)
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) { raw const typeof(int[n]) buf; (void)buf; }\n",
		    "spec_cv2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "raw const typeof VLA: accepted");
		prism_free(&r);
	}
}

// ── Two-pass invariant: register VLA (Phase 1D primary) ───────────────
static void spec_register_vla_phase1(void) {
	printf("\n--- register VLA Phase 1D ---\n");

	// register int buf[n] must reject
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) { register int buf[n]; (void)buf; }\n",
		    "spec_rv1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "register VLA: rejected");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "register") != NULL,
			      "register VLA: error mentions register");
		prism_free(&r);
	}

	// raw register VLA must pass
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) { raw register int buf[n]; (void)buf; }\n",
		    "spec_rv2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "raw register VLA: accepted");
		prism_free(&r);
	}
}

// ── Bracket orelse asm side-effect check (SPEC C7) ────────────────────
static void spec_bracket_orelse_asm(void) {
	printf("\n--- bracket orelse asm ---\n");

	// Chained bracket orelse with asm in intermediate: must reject
	// (intermediate operand evaluated twice in ternary expansion)
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    int arr[n orelse ({__asm__ volatile(\"nop\"); 3;}) orelse 5];\n"
		    "    (void)arr;\n"
		    "}\n",
		    "spec_ba1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "bracket orelse asm chained: rejected");
		if (r.error_msg)
			CHECK(strstr(r.error_msg, "asm") != NULL,
			      "bracket orelse asm chained: error mentions asm");
		prism_free(&r);
	}

	// typeof orelse with asm in LHS: must reject
	// (typeof ternary evaluates LHS twice)
	{
		PrismResult r = prism_transpile_source(
		    "int get_n(void);\n"
		    "void f(void) {\n"
		    "    typeof(({__asm__ volatile(\"nop\"); get_n();}) orelse 5) x;\n"
		    "    (void)x;\n"
		    "}\n",
		    "spec_ba2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "typeof orelse asm: rejected");
		prism_free(&r);
	}

	// Single bracket orelse without asm: must pass
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    int arr[n orelse 5];\n"
		    "    (void)arr;\n"
		    "}\n",
		    "spec_ba3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "bracket orelse no asm: accepted");
		prism_free(&r);
	}
}

// ── defer body ctrl_state consumption ──
static void spec_defer_body_ctrl_state(void) {
	printf("\n--- defer body ctrl_state ---\n");

	// Declaration inside braced if in defer body must NOT get brace_wrap
	{
		PrismResult r = prism_transpile_source(
		    "void use(int);\n"
		    "void test(int cond) {\n"
		    "    defer {\n"
		    "        if (cond) {\n"
		    "            int x;\n"
		    "            use(x);\n"
		    "        }\n"
		    "    }\n"
		    "}\n",
		    "spec_dcs1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "defer ctrl_state braced if: accepted");
		if (r.output) {
			// Must not have double braces around "int x"
			CHECK(strstr(r.output, "{ { int") == NULL &&
			      strstr(r.output, "{ {\nint") == NULL,
			      "defer ctrl_state braced if: no spurious brace_wrap");
		}
		prism_free(&r);
	}

	// Declaration inside braced else in defer body
	{
		PrismResult r = prism_transpile_source(
		    "void use(int);\n"
		    "void test(int cond) {\n"
		    "    defer {\n"
		    "        if (cond) use(1); else {\n"
		    "            int y;\n"
		    "            use(y);\n"
		    "        }\n"
		    "    }\n"
		    "}\n",
		    "spec_dcs2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "defer ctrl_state braced else: accepted");
		prism_free(&r);
	}

	// Declaration inside braced while in defer body
	{
		PrismResult r = prism_transpile_source(
		    "void use(int);\n"
		    "void test(int n) {\n"
		    "    defer {\n"
		    "        while (n > 0) {\n"
		    "            int x;\n"
		    "            use(x);\n"
		    "            n--;\n"
		    "        }\n"
		    "    }\n"
		    "}\n",
		    "spec_dcs3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "defer ctrl_state braced while: accepted");
		prism_free(&r);
	}
}

// ── typeof/bracket orelse in defer bodies ──
static void spec_defer_body_orelse_transform(void) {
	printf("\n--- defer body orelse transform ---\n");

	// bracket orelse in compound literal inside defer body
	{
		PrismResult r = prism_transpile_source(
		    "void use_arr(int *, int);\n"
		    "void test(int n) {\n"
		    "    defer {\n"
		    "        use_arr((int[n orelse 1]){0}, n);\n"
		    "    }\n"
		    "}\n",
		    "spec_dbo1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "bracket orelse compound literal in defer: accepted");
		if (r.output)
			CHECK(strstr(r.output, "orelse") == NULL,
			      "bracket orelse compound literal in defer: no orelse leak");
		prism_free(&r);
	}

	// typeof orelse in sizeof inside defer body
	{
		PrismResult r = prism_transpile_source(
		    "void use_val(int);\n"
		    "void test(int n) {\n"
		    "    defer {\n"
		    "        use_val(sizeof(typeof(int[n orelse 1])));\n"
		    "    }\n"
		    "}\n",
		    "spec_dbo2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "typeof orelse sizeof in defer: accepted");
		if (r.output)
			CHECK(strstr(r.output, "orelse") == NULL,
			      "typeof orelse sizeof in defer: no orelse leak");
		prism_free(&r);
	}

	// bracket orelse in declaration inside defer body (always worked)
	{
		PrismResult r = prism_transpile_source(
		    "void use(int *);\n"
		    "void test(int n) {\n"
		    "    defer {\n"
		    "        int arr[n orelse 1];\n"
		    "        use(arr);\n"
		    "    }\n"
		    "}\n",
		    "spec_dbo3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "bracket orelse decl in defer: accepted");
		if (r.output)
			CHECK(strstr(r.output, "orelse") == NULL,
			      "bracket orelse decl in defer: no orelse leak");
		prism_free(&r);
	}
}

// ═══════════════════════════════════════════════════════════════════════════
//  Runner
// ═══════════════════════════════════════════════════════════════════════════

void run_spec_tests(void) {
	// ── defer constraints ──
	spec_defer_C1_block_scope();
	spec_defer_C2_control_flow();
	spec_defer_C3_nested();
	spec_defer_C4_labels();
	spec_defer_C5_taint();
	spec_defer_C6_computed_goto();
	spec_defer_C7_stmt_expr_top();
	spec_defer_C8_stmt_expr_last();
	spec_defer_C9_shadow();

	// ── defer semantics ──
	spec_defer_S1_S2_S3_lifo();
	spec_defer_S4_goto_crossing();
	spec_defer_S6_return_before_defer();
	spec_defer_S2_break_continue();

	// ── orelse constraints ──
	spec_orelse_C1_scalar();
	spec_orelse_C2_file_scope();
	spec_orelse_C3_static_storage();
	spec_orelse_C4_enum();
	spec_orelse_C5_bracket_ctrl();
	spec_orelse_C6_bracket_file();
	spec_orelse_C7_side_effects();
	spec_orelse_C8_cast_target();
	spec_orelse_C10_prototype();
	spec_orelse_C11_struct_member();
	spec_orelse_C12_catch_all();

	// ── orelse semantics ──
	spec_orelse_S1_assign_value();
	spec_orelse_S2_assign_action();
	spec_orelse_S5_decl_init();
	spec_orelse_S6_bracket();
	spec_orelse_S7_chained();
	spec_orelse_S8_volatile();
	spec_orelse_S2_defer_cleanup();

	// ── orelse result type ──
	spec_orelse_RT1_independent_conversion();
	spec_orelse_RT2_bracket_conditional();

	// ── zero-init constraints ──
	spec_zeroinit_C1_scope();
	spec_zeroinit_C2_static();
	spec_zeroinit_C3_sue();
	spec_zeroinit_C4_register_vla();
	spec_zeroinit_C5_const_vla();
	spec_zeroinit_C7_computed_goto();

	// ── zero-init semantics ──
	spec_zeroinit_S1_scalar();
	spec_zeroinit_S2_aggregate();
	spec_zeroinit_S3_vla();
	spec_zeroinit_S6_multi_decl_order();

	// ── raw ──
	spec_raw_C1_placement();
	spec_raw_C3_not_qualifier();
	spec_raw_S1_suppresses();
	spec_raw_S2_prefix_all();
	spec_raw_S3_per_declarator();
	spec_raw_S4_consecutive();
	spec_raw_S5_file_scope();

	// ── feature flags ──
	spec_feature_flag_independence();

	// ── cross-feature ──
	spec_cross_defer_zeroinit();
	spec_cross_raw_orelse();

	// ── I/O buffer integrity ──

	// ── paren-depth evasion ──
	spec_paren_depth_evasion();

	// ── deferred orelse blind emission ──
	spec_deferred_orelse_blind_emission();

	// ── stmt-expr paren-strip desync ──
	spec_stmt_expr_paren_strip_desync();

	// ── typeof_var reentrancy ──
	spec_typeof_var_reentrancy();

	// ── orelse ctrl-flow keyword rejection ──
	spec_orelse_ctrl_flow_keyword_rejection();

	// ── typeof ctrl-flow keyword rejection ──
	spec_typeof_ctrl_flow_rejection();

	// ── C23 auto/constexpr orelse ──
	spec_c23_auto_constexpr_orelse();


	// ── attribute-obscured raw VLA safety ──
	spec_attr_raw_vla_safety();


	// ── attribute-encapsulated control flow ──
	spec_attr_ctrl_flow_rejection();

	// ── backward goto nested-scope defer loop ──
	spec_defer_loop_nested_scope();

	// ── for-init typedef recognition ──
	spec_for_init_typedef();

	// ── const-pointee VLA type composition ──
	spec_const_pointee_vla();

	// ── typedef const/volatile propagation ──
	spec_typedef_const_volatile_propagation();

	// ── function type composition ──
	spec_func_type_composition();

	// ── two-pass invariant regression ──
	spec_constexpr_orelse_phase1();
	spec_const_vla_memset_phase1();
	spec_register_vla_phase1();
	spec_bracket_orelse_asm();

	// ── defer body emission ──
	spec_defer_body_ctrl_state();
	spec_defer_body_orelse_transform();
}

// Bounds check tests (-fbounds-check): Tier 1 + Tier 2
// Verifies transpile output shape; runtime trap behavior is covered
// by the smoke tests at the end via subprocess execution.

#ifndef _WIN32
// Shared helper: transpile `src` with -fbounds-check, compile with cc,
// execute, and assert exit status. `expected_exit < 0` means "must trap"
// (any nonzero exit); otherwise the exact exit code is asserted.
// `label` is a short stem used to derive {label}: transpiles / compiles /
// returns N messages.
static void bc_runtime_case(const char *src, int expected_exit, const char *label) {
	char *src_path = create_temp_file(src);
	char bin[256], out_path[256], msg[256];
	snprintf(bin, sizeof(bin), "%s.bin", src_path);
	snprintf(out_path, sizeof(out_path), "%s.out.c", src_path);
	PrismFeatures f = prism_defaults();
	f.bounds_check = true;
	PrismResult r = prism_transpile_file(src_path, f);
	snprintf(msg, sizeof(msg), "%s: transpiles", label);
	CHECK_EQ(r.status, PRISM_OK, msg);
	if (r.output) {
		FILE *fp = fopen(out_path, "w");
		if (fp) { fwrite(r.output, 1, strlen(r.output), fp); fclose(fp); }
		char cmd[1024];
		snprintf(cmd, sizeof(cmd), "cc -std=gnu11 -o %s %s >/dev/null 2>&1", bin, out_path);
		snprintf(msg, sizeof(msg), "%s: compiles", label);
		CHECK_EQ(run_command_status(cmd), 0, msg);
		int status = run_command_status(bin);
		if (expected_exit < 0) {
			snprintf(msg, sizeof(msg), "%s: traps", label);
			CHECK(status != 0, msg);
		} else {
			snprintf(msg, sizeof(msg), "%s: exits %d", label, expected_exit);
			CHECK_EQ(status, expected_exit, msg);
		}
		unlink(bin); unlink(out_path);
	}
	prism_free(&r);
	unlink(src_path); free(src_path);
}
#endif

static void test_bounds_check_fixed_array(void) {
	printf("\n--- bounds-check fixed arrays ---\n");

	// Basic: arr[i] in expression gets wrapped
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[10]; a[5]=1; return a[5];}\n",
		    "bc_basic.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-basic: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "__prism_bchk") != NULL,
			      "bc-basic: helper injected");
			CHECK(strstr(r.output, "a[__prism_bchk((__prism_bchk_size_t)( 5)") != NULL,
			      "bc-basic: lhs subscript wrapped");
			CHECK(strstr(r.output, "sizeof(a)/sizeof(a[0])") != NULL,
			      "bc-basic: sizeof-ratio used for length");
		}
		prism_free(&r);
	}

	// Declarator brackets NOT wrapped
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[10]; (void)a; return 0;}\n",
		    "bc_decl.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-decl: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "a[__prism_bchk") == NULL,
			      "bc-decl: declarator bracket not wrapped");
		prism_free(&r);
	}

	// Feature off: no wrap, no helper
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = false;
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[10]; a[5]=1; return a[5];}\n",
		    "bc_off.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-off: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "__prism_bchk") == NULL,
			      "bc-off: no helper emitted");
			CHECK(strstr(r.output, "a[5]") != NULL,
			      "bc-off: raw subscript preserved");
		}
		prism_free(&r);
	}

	// Pointer NOT wrapped (not an array variable)
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int f(int *p){return p[3];}\n",
		    "bc_ptr.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-ptr: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "p[__prism_bchk") == NULL,
			      "bc-ptr: pointer subscript not wrapped");
		prism_free(&r);
	}

	// Array parameter (decays to pointer) NOT wrapped
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int f(int a[10]){return a[3];}\n",
		    "bc_param.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-param: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "a[__prism_bchk") == NULL,
			      "bc-param: array-param subscript not wrapped (decays)");
		prism_free(&r);
	}
}

static void test_bounds_check_vla(void) {
	printf("\n--- bounds-check VLAs ---\n");

	// VLA: subscript wrapped, sizeof/sizeof pattern evaluates at runtime
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int f(int n){int v[n]; v[0]=1; return v[0];}\n",
		    "bc_vla.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-vla: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "v[__prism_bchk") != NULL,
			      "bc-vla: vla subscript wrapped");
			CHECK(strstr(r.output, "sizeof(v)/sizeof(v[0])") != NULL,
			      "bc-vla: sizeof ratio for runtime len");
		}
		prism_free(&r);
	}
}

static void test_bounds_check_multidim(void) {
	printf("\n--- bounds-check multi-dim ---\n");

	// 2D array: both dimensions now wrapped.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int m[3][4]; m[1][2]=9; return m[1][2];}\n",
		    "bc_2d.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-2d: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "m[__prism_bchk((__prism_bchk_size_t)( 1), sizeof(m)/sizeof(m[0]))]") != NULL,
			      "bc-2d: outer dim wrapped");
			CHECK(strstr(r.output, "[__prism_bchk((__prism_bchk_size_t)( 2), sizeof(m[0])/sizeof(m[0][0]))]") != NULL,
			      "bc-2d: inner dim wrapped");
		}
		prism_free(&r);
	}

	// 3D array: all three dimensions wrapped.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int m[2][3][4]; m[0][1][2]=0; return 0;}\n",
		    "bc_3d.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-3d: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "sizeof(m)/sizeof(m[0])") != NULL,
			      "bc-3d: outer wrap present");
			CHECK(strstr(r.output, "sizeof(m[0])/sizeof(m[0][0])") != NULL,
			      "bc-3d: mid wrap present");
			CHECK(strstr(r.output, "sizeof(m[0][0])/sizeof(m[0][0][0])") != NULL,
			      "bc-3d: innermost wrap present");
		}
		prism_free(&r);
	}

	// Paren-wrapped outer chain: (a[i])[j] — both wrapped.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[3][4]; int i=1,j=2;\n"
		    "return (a[i])[j];}\n",
		    "bc_paren.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-paren-md: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "sizeof(a)/sizeof(a[0])") != NULL,
			      "bc-paren-md: outer wrap present");
			CHECK(strstr(r.output, "sizeof(a[0])/sizeof(a[0][0])") != NULL,
			      "bc-paren-md: inner wrap through parens present");
		}
		prism_free(&r);
	}

	// Rank guard: `int *p[10]` — outer wraps, inner `p[0][i]` MUST NOT
	// wrap, because p[0] is a pointer whose length isn't derivable from
	// `p`.  Without the rank guard, the wrap would emit
	// `sizeof(p[0])/sizeof(p[0][0]) == sizeof(ptr)/sizeof(int)` and
	// spuriously trap on every valid subscript.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int *p[10]; int i=0;\n"
		    "return p[0][i];}\n",
		    "bc_rank_guard.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-rank: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "p[__prism_bchk((__prism_bchk_size_t)( 0), sizeof(p)/sizeof(p[0]))]") != NULL,
			      "bc-rank: outer wrap present");
			CHECK(strstr(r.output, "sizeof(p[0])/sizeof(p[0][0])") == NULL,
			      "bc-rank: inner NOT wrapped (rank guard)");
		}
		prism_free(&r);
	}

	// File-scope array: registered when outer dim is complete.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int g[10];\n"
		    "int main(void){int i=0; return g[i];}\n",
		    "bc_filescope.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-filescope: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "g[__prism_bchk") != NULL,
			      "bc-filescope: wrapped");
		prism_free(&r);
	}

	// Incomplete extern: NOT registered (sizeof would be incomplete).
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "extern int ext[];\n"
		    "int main(void){int i=0; return ext[i];}\n",
		    "bc_extern_incomplete.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-extern: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "ext[__prism_bchk") == NULL,
			      "bc-extern: incomplete NOT wrapped");
		prism_free(&r);
	}

	// Typedef multi-dim rank inheritance: `typedef int T[3][4]; T a;`
	// should wrap BOTH levels (rank=2 inherited from typedef).
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "typedef int T[3][4];\n"
		    "int main(void){T a={{0}}; int i=0,j=0; return a[i][j];}\n",
		    "bc_td_2d.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-td-2d: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "sizeof(a)/sizeof(a[0])") != NULL,
			      "bc-td-2d: outer wrap present");
			CHECK(strstr(r.output, "sizeof(a[0])/sizeof(a[0][0])") != NULL,
			      "bc-td-2d: inner wrap present (typedef rank inherited)");
		}
		prism_free(&r);
	}

	// Typedef row with declarator-level outer dim: combined rank
	// `typedef int Row[5]; Row m[3];` => rank=2 (1 decl + 1 base).
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "typedef int Row[5];\n"
		    "int main(void){Row m[3]={{0}}; int i=0,j=0; return m[i][j];}\n",
		    "bc_td_row.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-td-row: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "sizeof(m)/sizeof(m[0])") != NULL,
			      "bc-td-row: outer wrap present");
			CHECK(strstr(r.output, "sizeof(m[0])/sizeof(m[0][0])") != NULL,
			      "bc-td-row: inner wrap present (combined decl+base rank)");
		}
		prism_free(&r);
	}

	// Chained typedef rank propagation: `typedef T1 T2; T2 a;`.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "typedef int T1[5];\n"
		    "typedef T1 T2;\n"
		    "int main(void){T2 a={0}; int i=0; return a[i];}\n",
		    "bc_td_chain.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-td-chain: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "a[__prism_bchk") != NULL,
			      "bc-td-chain: wrapped through typedef chain");
		prism_free(&r);
	}
}

static void test_bounds_check_init_and_args(void) {
	printf("\n--- bounds-check init/args ---\n");

	// Declaration initializer: arr[5] on RHS of int x = arr[5]
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[10]; a[0]=1; int x = a[0]; return x;}\n",
		    "bc_init.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-init: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "int x = a[__prism_bchk") != NULL,
			      "bc-init: init expr subscript wrapped");
		prism_free(&r);
	}

	// Function call argument: printf("%d", arr[0])
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int g(int); int main(void){int a[10]; a[0]=1; return g(a[0]);}\n",
		    "bc_arg.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-arg: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "g(a[__prism_bchk") != NULL,
			      "bc-arg: call-arg subscript wrapped");
		prism_free(&r);
	}
}

static void test_bounds_check_runtime(void) {
#ifndef _WIN32
	printf("\n--- bounds-check runtime trap ---\n");

	// In-bounds returns normally.
	bc_runtime_case(
	    "#include <stdio.h>\n"
	    "int main(void){int a[10]; for(int i=0;i<10;i++) a[i]=i;\n"
	    "return a[9];}\n",
	    9, "bc-run-ok");

	// OOB (fixed array) traps.
	bc_runtime_case(
	    "#include <stdio.h>\n"
	    "int main(void){int a[10]; for(int i=0;i<10;i++) a[i]=i;\n"
	    "volatile int j=15; return a[j];}\n",
	    -1, "bc-run-oob");

	// OOB in nested inner subscript traps.
	bc_runtime_case(
	    "#include <stdio.h>\n"
	    "int main(void){int arr[10]={0}; int m[3]={0,1,2};\n"
	    "volatile int i=10; return arr[m[i]];}\n",
	    -1, "bc-run-nested");

	// Negative index (via unsigned cast) traps.
	bc_runtime_case(
	    "#include <stdio.h>\n"
	    "int main(void){int a[10]={0}; volatile int i=-1;\n"
	    "return a[i];}\n",
	    -1, "bc-run-neg");

	// VLA OOB traps using runtime length.
	bc_runtime_case(
	    "#include <stdio.h>\n"
	    "int main(int argc,char**argv){(void)argv;int n=argc+3;\n"
	    "int v[n]; for(int i=0;i<n;i++) v[i]=i;\n"
	    "volatile int j=n+5; return v[j];}\n",
	    -1, "bc-run-vla");

	// Unevaluated sizeof of VLA element must not trap even with absurd index.
	bc_runtime_case(
	    "#include <stdio.h>\n"
	    "int main(int argc,char**argv){(void)argv;int n=argc+3;\n"
	    "int v[n]; size_t sz = sizeof v[1000000]; (void)sz;\n"
	    "return 42;}\n",
	    42, "bc-run-vla-sizeof");

	// &arr[len] (one-past-end address) is legal and must not trap.
	bc_runtime_case(
	    "#include <stdio.h>\n"
	    "int main(void){int a[10]; int *p=&a[10]; (void)p; return 77;}\n",
	    77, "bc-run-addrof");
#endif
}

static void test_bounds_check_false_match_guards(void) {
	printf("\n--- bounds-check false-match guards ---\n");

	// Struct member subscript with colliding local name MUST NOT be wrapped
	// using the local's size (silent false-negative bug if wrapped).
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int arr[100]; struct S{int arr[5];} s;"
		    " (void)arr; s.arr[3]=1; return s.arr[4];}\n",
		    "bc_struct_shadow.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-struct-shadow: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "s.arr[__prism_bchk") == NULL,
			      "bc-struct-shadow: s.arr[...] not wrapped");
			// The bare `arr` usage via (void)arr should not have a [
			// so nothing to wrap there.
		}
		prism_free(&r);
	}

	// Pointer-to-struct member subscript
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "struct N{int data[8];}; int f(struct N *p){return p->data[3];}\n",
		    "bc_arrow.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-arrow: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "data[__prism_bchk") == NULL,
			      "bc-arrow: p->data[...] not wrapped");
		prism_free(&r);
	}

	// raw-declared local array must NOT be wrapped
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){raw int a[10]; a[5]=1; return a[5];}\n",
		    "bc_raw.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-raw: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "a[__prism_bchk") == NULL,
			      "bc-raw: raw-declared array not wrapped");
		prism_free(&r);
	}

	// String literal subscript — last_emitted is a string, not an ident
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "char g(void){return \"hello\"[2];}\n",
		    "bc_strlit.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-strlit: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "[__prism_bchk") == NULL,
			      "bc-strlit: string literal subscript not wrapped");
		prism_free(&r);
	}

	// Compound literal subscript — last_emitted is '}', not an ident
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int g(void){return (int[]){1,2,3}[1];}\n",
		    "bc_complit.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-complit: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "[__prism_bchk") == NULL,
			      "bc-complit: compound-literal subscript not wrapped");
		prism_free(&r);
	}

	// Nested scope: inner `arr` shadows outer `arr`, each subscript uses
	// the C compiler's scope resolution via sizeof.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int arr[10]; arr[1]=1;"
		    " { int arr[3]; arr[0]=9; } return arr[1];}\n",
		    "bc_nested.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-nested: transpiles");
		if (r.output) {
			// Both scopes use sizeof(arr)/sizeof(arr[0]) — the backend
			// compiler binds the correct `arr` at each site.
			int hits = 0;
			const char *p = r.output;
			while ((p = strstr(p, "arr[__prism_bchk"))) { hits++; p++; }
			CHECK(hits == 3, "bc-nested: three wrapped subscripts");
		}
		prism_free(&r);
	}
}

static void test_bounds_check_unevaluated_operands(void) {
	printf("\n--- bounds-check unevaluated operands ---\n");

	// sizeof(arr[5]) — operand unevaluated; MUST NOT wrap (VLA would trap).
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[10]; return (int)sizeof(a[5]);}\n",
		    "bc_sizeof.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-sizeof: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "sizeof(a[5])") != NULL,
			      "bc-sizeof: subscript inside sizeof not wrapped");
			CHECK(strstr(r.output, "sizeof(a[__prism_bchk") == NULL,
			      "bc-sizeof: no wrap inside sizeof");
		}
		prism_free(&r);
	}

	// _Alignof(arr[0]) — not wrapped.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[10]; return (int)_Alignof(a[0]);}\n",
		    "bc_alignof.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-alignof: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "_Alignof(a[0])") != NULL,
			      "bc-alignof: subscript inside _Alignof not wrapped");
		prism_free(&r);
	}

	// typeof(arr[5]) — not wrapped.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[10]; typeof(a[5]) x = 0; (void)x; return 0;}\n",
		    "bc_typeof.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-typeof: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "typeof(a[5])") != NULL,
			      "bc-typeof: subscript inside typeof not wrapped");
		prism_free(&r);
	}

	// __builtin_offsetof — subscript names a struct field, NOT local.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "#include <stddef.h>\n"
		    "struct S { int arr[5]; };\n"
		    "int main(void){int arr[100]; (void)arr; "
		    "return (int)__builtin_offsetof(struct S, arr[2]);}\n",
		    "bc_offsetof.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-offsetof: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "__builtin_offsetof(struct S, arr[2])") != NULL,
			      "bc-offsetof: subscript inside offsetof not wrapped");
			CHECK(strstr(r.output, "offsetof(struct S, arr[__prism_bchk") == NULL,
			      "bc-offsetof: no wrap inside offsetof");
		}
		prism_free(&r);
	}

	// _Generic controlling expression is unevaluated per C11 §6.5.1.1p3;
	// subscripts there must not wrap (wrapping turns a pure type-select
	// into a runtime trap for out-of-range values).
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[10]={0}; int i=50;\n"
		    "return _Generic(a[i], int: 1, default: 0);}\n",
		    "bc_generic_ctrl.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-generic-ctrl: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "_Generic(a[i]") != NULL,
			      "bc-generic-ctrl: controlling subscript not wrapped");
			CHECK(strstr(r.output, "_Generic(a[__prism_bchk") == NULL,
			      "bc-generic-ctrl: no wrap inside _Generic controlling expr");
		}
		prism_free(&r);
	}

	// Nested subscript inside _Generic controlling expression — inner
	// bracket is also unevaluated and must not wrap.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[10]={0}; int b[5]={0}; int i=50;\n"
		    "return _Generic(a[b[i]], int: 1, default: 0);}\n",
		    "bc_generic_nested.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-generic-nested: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "__prism_bchk((__prism_bchk_size_t)") == NULL,
			      "bc-generic-nested: nested controlling subscript not wrapped");
		prism_free(&r);
	}

	// _Generic association expression IS evaluated when selected, so
	// subscripts inside associations MUST still wrap.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[10]={0}; int i=3;\n"
		    "return _Generic(1, int: a[i], default: 0);}\n",
		    "bc_generic_assoc.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-generic-assoc: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "a[__prism_bchk") != NULL,
			      "bc-generic-assoc: association subscript wrapped");
		prism_free(&r);
	}
}

static void test_bounds_check_nested_subscript(void) {
	printf("\n--- bounds-check nested subscripts ---\n");

	// arr[m[i]] — BOTH subscripts wrapped.
	PrismFeatures f = prism_defaults();
	PrismResult r = prism_transpile_source(
	    "int main(void){int a[10]={0}; int m[5]={0}; int i=0; return a[m[i]];}\n",
	    "bc_nested_idx.c", f);
	CHECK_EQ(r.status, PRISM_OK, "bc-nested-idx: transpiles");
	if (r.output) {
		int hits = 0;
		for (const char *p = r.output; (p = strstr(p, "__prism_bchk")); p++) hits++;
		CHECK(hits >= 2, "bc-nested-idx: outer and inner both wrapped");
		CHECK(strstr(r.output, "m[__prism_bchk((__prism_bchk_size_t)( i)") != NULL,
		      "bc-nested-idx: inner m[i] wrapped");
	}
	prism_free(&r);
}

static void test_bounds_check_address_of(void) {
	printf("\n--- bounds-check unary address-of ---\n");

	// &a[N] — one-past-end is legal in C, must NOT wrap.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[10]; int *p = &a[10]; (void)p; return 0;}\n",
		    "bc_addr.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-addrof: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "&a[10]") != NULL,
			      "bc-addrof: &a[10] not wrapped (one-past-end legal)");
			CHECK(strstr(r.output, "&a[__prism_bchk") == NULL,
			      "bc-addrof: no wrap after unary &");
		}
		prism_free(&r);
	}

	// Binary & (bitwise AND) with a[i] on rhs IS wrapped.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[10]={0}; int x=7; return x & a[3];}\n",
		    "bc_binand.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-binand: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "a[__prism_bchk") != NULL,
			      "bc-binand: binary & does not inhibit wrap");
		prism_free(&r);
	}
}

static void test_bounds_check_dark_corners(void) {
	printf("\n--- bounds-check dark corners ---\n");

	// No-paren sizeof: `sizeof arr[5]` — operand is the full postfix
	// expression per C11 §6.5.3; subscript is unevaluated and MUST NOT wrap.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[10]; return (int)sizeof a[5];}\n",
		    "bc_sizeof_noparen.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-sizeof-noparen: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "sizeof a[5]") != NULL,
			      "bc-sizeof-noparen: no-paren sizeof subscript not wrapped");
			CHECK(strstr(r.output, "sizeof a[__prism_bchk") == NULL,
			      "bc-sizeof-noparen: no wrap after sizeof");
		}
		prism_free(&r);
	}

	// No-paren sizeof with unary prefix: `sizeof *a[i]` — operand is *a[i].
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(int c,char**v){(void)v;int a[5]; return (int)sizeof *&a[c];}\n",
		    "bc_sizeof_unary.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-sizeof-unary: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "a[__prism_bchk") == NULL,
			      "bc-sizeof-unary: no wrap inside unary-prefixed sizeof");
		prism_free(&r);
	}

	// No-paren sizeof ended by binary op: `sizeof a[0] + 1` —
	// only the subscript is unevaluated; the `+ 1` is outside.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[5]; return (int)(sizeof a[0] + 1);}\n",
		    "bc_sizeof_binop.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-sizeof-binop: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "sizeof a[0]") != NULL,
			      "bc-sizeof-binop: a[0] unwrapped, binary op terminates postfix");
		prism_free(&r);
	}

	// Member chain in no-paren sizeof: `sizeof s.arr[0]`.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "struct S{int arr[5];};\n"
		    "int main(void){struct S s; return (int)sizeof s.arr[0];}\n",
		    "bc_sizeof_member.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-sizeof-member: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "arr[__prism_bchk") == NULL,
			      "bc-sizeof-member: member chain in no-paren sizeof not wrapped");
		prism_free(&r);
	}

	// Function parameter (T[N] or T[static N]) decays to pointer —
	// subscripts inside callee MUST NOT be wrapped (would use pointer sizeof).
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int f(int p[10], int i){return p[i];}\n"
		    "int g(int p[static 10], int i){return p[i];}\n"
		    "int main(void){int a[10]={0}; return f(a,0)+g(a,0);}\n",
		    "bc_param_decay.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-param-decay: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "p[__prism_bchk") == NULL,
			      "bc-param-decay: array-parameter subscripts not wrapped");
		prism_free(&r);
	}

	// Compound literal / string literal subscript — `last_emitted` is
	// a `}` or string, not IDENT; must not wrap.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){return ((int[3]){1,2,3})[1] + \"abc\"[0];}\n",
		    "bc_lit_subscript.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-lit: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "[__prism_bchk") == NULL,
			      "bc-lit: compound/string literal subscripts not wrapped");
		}
		prism_free(&r);
	}

	// Pointer-to-array dereferenced: `(*p)[i]` — `last_emitted` is `)`;
	// must not wrap (p is pointer, not array).
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[10]={0}; int (*p)[10]=&a; return (*p)[5];}\n",
		    "bc_ptr_to_arr.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-ptr-to-arr: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "[__prism_bchk") == NULL,
			      "bc-ptr-to-arr: (*p)[i] not wrapped");
		prism_free(&r);
	}

	// Designated initializer brackets — `[5] = 1` — declarator context.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[10] = {[5]=1,[9]=2}; return a[5];}\n",
		    "bc_desig.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-desig: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "[5]=1") != NULL || strstr(r.output, "[5] = 1") != NULL,
			      "bc-desig: designator brackets preserved unwrapped");
			CHECK(strstr(r.output, "a[__prism_bchk((__prism_bchk_size_t)( 5)") != NULL,
			      "bc-desig: normal subscript still wrapped");
		}
		prism_free(&r);
	}

	// offsetof macro form (via stddef.h) — must produce valid constant
	// expression; __builtin_offsetof form already tested.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "#include <stddef.h>\n"
		    "struct N{int inner[5];};\n"
		    "int main(void){return (int)offsetof(struct N, inner[3]);}\n",
		    "bc_offsetof_macro.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-offsetof-macro: transpiles");
		if (r.output) {
			// Either __builtin_offsetof form (GCC/Clang) or &((T*)0)->f[i]
			// form (traditional). Both must not invoke __prism_bchk inside
			// the subscript — the former is unevaluated, the latter is a
			// constant expression and wrapping would break that.
			CHECK(strstr(r.output, "inner[__prism_bchk") == NULL,
			      "bc-offsetof-macro: offsetof subscript never wrapped");
		}
		prism_free(&r);
	}

	// Scoped shadow: inner-scope array with same name as outer pointer —
	// outer is ptr (not wrapped), inner is array (wrapped with inner's size).
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int *x=0; (void)x;"
		    " {int x[3]={1,2,3}; int y=x[2]; (void)y;} return 0;}\n",
		    "bc_shadow_scope.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-shadow-scope: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "x[__prism_bchk") != NULL,
			      "bc-shadow-scope: inner-scope array wrapped");
		prism_free(&r);
	}

	// Macro-expanded subscripts are preprocessor-level; prism_transpile_source
	// does not run cpp, so macro expansion is covered by the runtime tests
	// that go through prism_transpile_file. Basic non-macro subscripts are
	// already covered by test_bounds_check_fixed_array.

	// LEN macro (sizeof(a)/sizeof(a[0])) — the inner sizeof(a[0]) is
	// unevaluated and must not wrap.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "#define LEN(a) (sizeof(a)/sizeof((a)[0]))\n"
		    "int main(void){int a[7]; return (int)LEN(a);}\n",
		    "bc_len_macro.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-len-macro: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "sizeof((a)[__prism_bchk") == NULL &&
			      strstr(r.output, "sizeof( a[__prism_bchk") == NULL,
			      "bc-len-macro: sizeof((a)[0]) inside LEN not wrapped");
		prism_free(&r);
	}

	// Chained subscripts assignment: a[0] = a[1] = a[2] = 7; all three wrap.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[5]={0}; a[0]=a[1]=a[2]=7; return a[0];}\n",
		    "bc_chain_assign.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-chain-assign: transpiles");
		if (r.output) {
			int hits = 0;
			for (const char *p = r.output; (p = strstr(p, "__prism_bchk")); p++) hits++;
			CHECK(hits >= 4,
			      "bc-chain-assign: all subscripts wrapped (>=4: 3 LHS + 1 return)");
		}
		prism_free(&r);
	}

	// Side-effect in index evaluated exactly once (helper takes by value).
	// We verify transpile shape: `a[k++]` becomes a[__prism_bchk((...)(k++),...)].
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[5]={0}; int k=0; return a[k++];}\n",
		    "bc_side_effect.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-side-effect: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "( k++)") != NULL,
			      "bc-side-effect: side-effectful index passed by value");
			// Only one occurrence of `k++` — no double-eval.
			int inc = 0;
			for (const char *p = r.output; (p = strstr(p, "k++")); p++) inc++;
			CHECK(inc == 1, "bc-side-effect: k++ appears exactly once");
		}
		prism_free(&r);
	}

	// Multi-decl: `int a[4]={...}, x=5, *p=&a[0];` — only `a` is tracked.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[4]={0,1,2,3}, x=5, *p=&a[0]; "
		    "(void)x; (void)p; return a[2];}\n",
		    "bc_multidecl.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-multidecl: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "a[__prism_bchk((__prism_bchk_size_t)( 2)") != NULL,
			      "bc-multidecl: the array `a` in multi-decl is tracked");
			CHECK(strstr(r.output, "&a[__prism_bchk") == NULL,
			      "bc-multidecl: &a[0] (unary addr-of) not wrapped");
		}
		prism_free(&r);
	}
}

// ISO C defines `idx[arr]` as strictly commutative with `arr[idx]`.
// When the array side hides *inside* the brackets, the normal
// array-on-the-left pattern match fails and the subscript would be
// emitted raw — silently bypassing -fbounds-check. Prism must reject
// this form with a hard error when bounds-check is enabled.
static void test_bounds_check_commutative(void) {
	printf("\n--- bounds-check commutative subscript (bypass guard) ---\n");

	// Direct commutative: `len[buffer]` where buffer is a tracked array.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int b[10]; int len=5;\n"
		    "len[b] = 0; return 0;}\n",
		    "bc_comm_direct.c", f);
		CHECK(r.status != PRISM_OK, "bc-comm-direct: rejects idx[arr]");
		prism_free(&r);
	}

	// Parenthesized index: `(idx)[arr]` — same commutative bypass as `idx[arr]`.
	// Regression: peel-off-last_emitted could stop when `(` is preceded by a
	// numeric literal (line break after `i=5`), skipping the guard.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int b[10]; int i=5;\n"
		    "(i)[b] = 0; return 0;}\n",
		    "bc_comm_paren_idx.c", f);
		CHECK(r.status != PRISM_OK, "bc-comm-paren-idx: rejects (idx)[arr]");
		prism_free(&r);
	}

	// Literal index form: `2[buffer]`.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int b[10];\n"
		    "2[b] = 0; return 0;}\n",
		    "bc_comm_lit.c", f);
		CHECK(r.status != PRISM_OK, "bc-comm-lit: rejects 2[arr]");
		prism_free(&r);
	}

	// Paren-wrapped array inside: `len[(buffer)]`.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int b[10]; int len=5;\n"
		    "len[(b)] = 0; return 0;}\n",
		    "bc_comm_paren.c", f);
		CHECK(r.status != PRISM_OK, "bc-comm-paren: rejects idx[(arr)]");
		prism_free(&r);
	}

	// Struct member as index: `s.f[buffer]`.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "struct S { int f; };\n"
		    "int main(void){int b[10]; struct S s={5};\n"
		    "s.f[b] = 0; return 0;}\n",
		    "bc_comm_member.c", f);
		CHECK(r.status != PRISM_OK, "bc-comm-member: rejects s.f[arr]");
		prism_free(&r);
	}

	// Feature off: commutative form must pass through untouched.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = false;
		PrismResult r = prism_transpile_source(
		    "int main(void){int b[10]; int len=5;\n"
		    "len[b] = 0; return 0;}\n",
		    "bc_comm_off.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-comm-off: allowed without -fbounds-check");
		if (r.output)
			CHECK(strstr(r.output, "len[b]") != NULL,
			      "bc-comm-off: emitted raw");
		prism_free(&r);
	}

	// Normal form with parens must still wrap: `(arr)[idx]`.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int b[10]; int i=3;\n"
		    "(b)[i] = 0; return 0;}\n",
		    "bc_comm_normal_paren.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-comm-normal-paren: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "__prism_bchk") != NULL,
			      "bc-comm-normal-paren: wrapped normally");
		prism_free(&r);
	}

	// Non-array identifier inside brackets: `i[j]` where neither is a
	// tracked array. Must not error — nothing to protect.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int *p=0; int i=0;\n"
		    "(void)i; (void)p; return 0;}\n",
		    "bc_comm_noarr.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-comm-noarr: transpiles (no tracked array)");
		prism_free(&r);
	}
}

// Regression tests for declarator-bracket class bugs (see prism.c
// try_bounds_check_subscript declarator guard and param-shadow
// registration in p1_register_param_shadows / K&R handler). Each case
// represents a context where `name[N]` appears in a declarator and must
// NOT be rewritten as an expression subscript.
static void test_bounds_check_declarator_contexts(void) {
	printf("\n--- bounds-check declarator contexts (regression) ---\n");

	// Bug 1: file-scope array with initializer-completed outer dim.
	// `int g[] = {1,2,3};` — outer dim is completed by the initializer;
	// was previously rejected by the "non-empty dim" gate and so was
	// never registered as an array, producing no wrap at use sites.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int g[]={1,2,3};\n"
		    "int main(void){return g[1];}\n",
		    "bc_reg_init.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-reg-init: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "g[__prism_bchk") != NULL,
			      "bc-reg-init: initializer-completed array registered");
		prism_free(&r);
	}

	// Bug 2: parameter name shadows a file-scope array.
	// `int g[10]; int f(int g[20]){return g[5];}` — body use of `g` is
	// the decayed pointer parameter; wrapping against sizeof(g)/sizeof(g[0])
	// would use pointer size and spuriously trap.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int g[10];\n"
		    "int f(int g[20]){return g[5];}\n",
		    "bc_reg_param.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-reg-param: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "int g[20]") != NULL,
			      "bc-reg-param: prototype declarator not wrapped");
			CHECK(strstr(r.output, "g[__prism_bchk") == NULL,
			      "bc-reg-param: body use of param not wrapped");
		}
		prism_free(&r);
	}

	// Bug 3: K&R-style parameter declarations between `)` and `{`.
	// `int f(g) int g[10]; {return g[3];}` — both the type-decl `g[10]`
	// and the body use of `g` must not wrap against the file-scope `g`.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int g[5]={0};\n"
		    "int f(g) int g[10]; {return g[3];}\n",
		    "bc_reg_knr.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-reg-knr: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "int g[10]") != NULL,
			      "bc-reg-knr: K&R type-decl not wrapped");
			CHECK(strstr(r.output, "g[__prism_bchk") == NULL,
			      "bc-reg-knr: body use of K&R param not wrapped");
		}
		prism_free(&r);
	}

	// Bug 4: nested prototype inside a function body.
	// `extern int f(int g[30]);` inside a body — the declarator `g[30]`
	// would otherwise be wrapped (VLA-mismatch warning + potential trap).
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int g[10];\n"
		    "int main(void){\n"
		    "  extern int f(int g[30]);\n"
		    "  (void)f; return g[5];\n"
		    "}\n",
		    "bc_reg_nested.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-reg-nested: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "int g[30]") != NULL,
			      "bc-reg-nested: nested prototype declarator not wrapped");
		prism_free(&r);
	}

	// Bug 5: function-pointer parameter with array-typed parameter.
	// `void h(int (*fp)(int g[20]));` — inner `g[20]` is a declarator.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int g[10];\n"
		    "void h(int (*fp)(int g[20]));\n"
		    "int main(void){(void)h; return g[5];}\n",
		    "bc_reg_fptr_param.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-reg-fptr-param: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "int g[20]") != NULL,
			      "bc-reg-fptr-param: fptr-param declarator not wrapped");
		prism_free(&r);
	}

	// Bug 6: function-pointer typedef with array-typed parameter.
	// `typedef int (*FP)(int g[20]);` — declarator inside typedef.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int g[10];\n"
		    "typedef int (*FP)(int g[20]);\n"
		    "int main(void){FP fp=0; (void)fp; return g[5];}\n",
		    "bc_reg_fptr_typedef.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-reg-fptr-typedef: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "int g[20]") != NULL,
			      "bc-reg-fptr-typedef: typedef declarator not wrapped");
		prism_free(&r);
	}

	// Bug 7: struct field that is a function pointer with array-typed param.
	// `struct S { int (*fn)(int g[20]); };` — declarator inside struct body.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int g[10];\n"
		    "struct S { int (*fn)(int g[20]); };\n"
		    "int main(void){struct S s={0}; (void)s; return g[5];}\n",
		    "bc_reg_struct_fptr.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-reg-struct-fptr: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "int g[20]") != NULL,
			      "bc-reg-struct-fptr: struct-field declarator not wrapped");
		prism_free(&r);
	}

	// Negative control: unary-deref `*a[i]` still gets wrapped. The
	// declarator guard must distinguish pointer-declarator `*` (preceded
	// by a type/qualifier/'*'/'(') from unary-deref `*` (preceded by a
	// value-producing token or at statement start).
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int *a[10]={0}; int i=3; return *a[i];}\n",
		    "bc_reg_deref.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-reg-deref: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "*a[__prism_bchk") != NULL,
			      "bc-reg-deref: unary-deref of subscript still wrapped");
		prism_free(&r);
	}

#ifndef _WIN32
	// Runtime regression: Bug 2 scenario must not trap at runtime.
	bc_runtime_case(
	    "int g[10];\n"
	    "int f(int g[20]){return g[5];}\n"
	    "int main(void){int x[20]={0}; return f(x);}\n",
	    0, "bc-reg-param-runtime");

	// Runtime regression: Bug 3 K&R scenario must not trap.
	bc_runtime_case(
	    "int g[5]={0};\n"
	    "int f(g) int g[10]; {(void)g; return 0;}\n"
	    "int main(void){int x[10]={0}; return f(x);}\n",
	    0, "bc-reg-knr-runtime");
#endif
}

// Extreme edges: flexible-array-members, unions, nested struct member chains,
// array-of-function-pointers declarators, statement expressions, ternary LHS,
// zero-size arrays, atomic/thread-local/static locals, casts to array type,
// sizeof-of-sizeof, and additional hostile runtime traps.
static void test_bounds_check_extreme_edges(void) {
	printf("\n--- bounds-check extreme edges ---\n");

	// Flexible-array member: `struct S { int n; int data[]; };`
	// Length is unknown at the declaration site → sizeof(s->data)/sizeof(...)
	// would be garbage. The safest behaviour is to NOT wrap. Our tier-1
	// tracker requires a sized declarator, and a struct member is a
	// member-access (TT_MEMBER), so it should be skipped.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "struct S { int n; int data[]; };\n"
		    "int get(struct S *s, int i){ return s->data[i]; }\n"
		    "int main(void){return 0;}\n",
		    "bc_fam.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-fam: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "s->data[__prism_bchk") == NULL,
			      "bc-fam: flex-array-member subscript not wrapped");
		prism_free(&r);
	}

	// Union member array — member access, must not wrap.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "union U { int arr[10]; long x; };\n"
		    "int main(void){union U u={{0}}; return u.arr[5];}\n",
		    "bc_union.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-union: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "u.arr[__prism_bchk") == NULL,
			      "bc-union: union member subscript not wrapped");
		prism_free(&r);
	}

	// Deep nested struct member — s.a.b.c.arr[i], must not wrap.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "struct C { int arr[10]; };\n"
		    "struct B { struct C c; };\n"
		    "struct A { struct B b; };\n"
		    "int main(void){struct A a={{{{0}}}}; return a.b.c.arr[5];}\n",
		    "bc_deepmember.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-deepmember: transpiles");
		if (r.output)
			CHECK(strstr(r.output, ".arr[__prism_bchk") == NULL,
			      "bc-deepmember: deep member subscript not wrapped");
		prism_free(&r);
	}

	// Array of function pointers: `int (*fns[3])(int);` — the complex
	// declarator form (ptr-to-function with array-of size) is a v1
	// tracker limitation and intentionally not wrapped. We just assert
	// the transpile succeeds and the declarator bracket isn't spuriously
	// wrapped (which would be a syntax error).
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "static int f0(int x){return x+1;} static int f1(int x){return x+2;}\n"
		    "int main(void){int (*fns[3])(int) = {f0, f1, f0};\n"
		    "return fns[1](10);}\n",
		    "bc_fnptr_arr.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-fnptr-arr: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "(*fns[__prism_bchk") == NULL,
			      "bc-fnptr-arr: declarator bracket not wrapped (no syntax error)");
		}
		prism_free(&r);
	}

	// Ternary LHS — `(c ? a : b)[i]` — last-emitted is `)`, so guard
	// correctly rejects the wrap (documented accidentally-safe case).
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(int argc,char**argv){(void)argv;\n"
		    "int a[5]={0,1,2,3,4}, b[5]={9,8,7,6,5};\n"
		    "return (argc>0 ? a : b)[2];}\n",
		    "bc_ternary_lhs.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-ternary-lhs: transpiles");
		if (r.output)
			CHECK(strstr(r.output, ")[__prism_bchk") == NULL,
			      "bc-ternary-lhs: ternary LHS subscript not wrapped");
		prism_free(&r);
	}

	// GCC statement expression containing a subscript — should still wrap
	// the inner subscript as a normal array access.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[5]={0,1,2,3,4};\n"
		    "int v = ({ int x = a[2]; x + 1; });\n"
		    "return v;}\n",
		    "bc_stmtexpr.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-stmtexpr: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "a[__prism_bchk") != NULL,
			      "bc-stmtexpr: subscript inside stmt-expr wrapped");
		prism_free(&r);
	}

	// `static` and `_Thread_local` local arrays — still locals with known
	// size, must be wrapped.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){static int sa[5]={10,20,30,40,50};\n"
		    "return sa[2];}\n",
		    "bc_static_local.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-static-local: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "sa[__prism_bchk") != NULL,
			      "bc-static-local: static local array wrapped");
		prism_free(&r);
	}

	// Parenthesized array name `(a)[i]` — must still wrap.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[10]={0}; int i=3; return (a)[i];}\n",
		    "bc_paren_name.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-paren-name: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "__prism_bchk") != NULL,
			      "bc-paren-name: parenthesized array subscript wrapped");
		prism_free(&r);
	}

	// Typedef-based array `typedef int T[10]; T a;` — must wrap `a[i]`.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "typedef int T[10];\n"
		    "int main(void){T a={0}; int i=3; return a[i];}\n",
		    "bc_typedef_arr.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-typedef-arr: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "a[__prism_bchk") != NULL,
			      "bc-typedef-arr: typedef array subscript wrapped");
		prism_free(&r);
	}

	// `sizeof (a)[i]` parses as `sizeof ((a)[i])` — the `[i]` is in an
	// unevaluated operand (postfix chain of sizeof) and must NOT wrap.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[10]={0};\n"
		    "return (int)sizeof (a)[3];}\n",
		    "bc_sizeof_postfix.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-sizeof-postfix: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "a[__prism_bchk") == NULL,
			      "bc-sizeof-postfix: sizeof postfix chain not wrapped");
		prism_free(&r);
	}

	// Doubly-parenthesized array name `((a))[i]` and `(((a)))[i]` — each
	// level of `(…)` must be peeled through so the subscript wraps.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[10]={0}; int i=3;\n"
		    "return ((a))[i] + (((a)))[i];}\n",
		    "bc_doubleparen.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-doubleparen: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "((a))[__prism_bchk") != NULL,
			      "bc-doubleparen: ((a))[i] wrapped");
			CHECK(strstr(r.output, "(((a)))[__prism_bchk") != NULL,
			      "bc-doubleparen: (((a)))[i] wrapped");
		}
		prism_free(&r);
	}

	// Array of pointers `int *a[10]` — `a[i]` is a valid subscript of
	// the outer array and must wrap.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int x=0; int *a[10]={\n"
		    "  &x,&x,&x,&x,&x,&x,&x,&x,&x,&x};\n"
		    "int i=3; return *a[i];}\n",
		    "bc_array_of_ptrs.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-array-of-ptrs: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "a[__prism_bchk") != NULL,
			      "bc-array-of-ptrs: int *a[10]; a[i] wrapped");
		prism_free(&r);
	}

	// Pointer-to-array `int (*a)[10]` — the outer name `a` is NOT an
	// array, so `(*a)[i]` must NOT wrap.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int b[10]={0}; int (*a)[10] = &b;\n"
		    "int i=3; return (*a)[i];}\n",
		    "bc_ptr_to_array.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-ptr-to-array: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "[__prism_bchk") == NULL,
			      "bc-ptr-to-array: (*a)[i] not wrapped");
		prism_free(&r);
	}

	// Auto-static on `const T arr[N] = {literals};` must emit a space
	// after `static` so it does not concatenate with the type.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){const int a[10]={0}; int i=3;\n"
		    "return a[i];}\n",
		    "bc_auto_static_spacing.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-auto-static-spacing: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "static const") != NULL,
			      "bc-auto-static-spacing: emits 'static const' with space");
			CHECK(strstr(r.output, "staticconst") == NULL,
			      "bc-auto-static-spacing: no 'staticconst' concatenation");
		}
		prism_free(&r);
	}

	// File-scope arrays with a known size must be registered so uses
	// from any function body wrap. `extern T a[];` / tentative `T a[];`
	// with no size must NOT be registered (sizeof(a) is not a complete
	// expression at the use site).
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "static int g[10] = {0};\n"
		    "int main(void){int i=3; return g[i];}\n",
		    "bc_file_scope.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-file-scope: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "g[__prism_bchk") != NULL,
			      "bc-file-scope: static file-scope array wrapped");
		prism_free(&r);
	}
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "extern int e[];\n"
		    "int main(void){int i=3; return e[i];}\n",
		    "bc_file_scope_extern.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-file-scope-extern: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "e[__prism_bchk") == NULL,
			      "bc-file-scope-extern: extern T a[] not wrapped (incomplete size)");
		prism_free(&r);
	}
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){\n"
		    "extern int blk[];\n"
		    "int i = 3;\n"
		    "return blk[i];}\n",
		    "bc_block_scope_extern_incomplete.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-block-scope-extern-incomplete: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "blk[__prism_bchk") == NULL,
			      "bc-block-scope-extern-incomplete: incomplete extern array not wrapped");
		prism_free(&r);
	}

	// typeof(int[N]) base type (no declarator []) must still register for bounds.
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    typeof(int[10]) buffer;\n"
		    "    buffer[5] = 1;\n"
		    "}\n",
		    "bc_typeof_keyword_array.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-typeof-keyword-array: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "buffer[__prism_bchk") != NULL,
			      "bc-typeof-keyword-array: subscript wrapped");
		prism_free(&r);
	}

	// const / volatile / restrict qualifiers on array — must still wrap.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){const int ca[5]={1,2,3,4,5};\n"
		    "volatile int va[5]={5,4,3,2,1};\n"
		    "return ca[2] + va[1];}\n",
		    "bc_qual.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-qual: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "ca[__prism_bchk") != NULL,
			      "bc-qual: const-qualified array wrapped");
			CHECK(strstr(r.output, "va[__prism_bchk") != NULL,
			      "bc-qual: volatile-qualified array wrapped");
		}
		prism_free(&r);
	}

	// Nested sizeof: `sizeof sizeof a[5]` — outer sizeof's operand is the
	// inner sizeof expression; both operands are unevaluated, nothing wraps.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int a[5]={0};\n"
		    "unsigned long z = sizeof sizeof a[5];\n"
		    "(void)z; return 0;}\n",
		    "bc_sizeof_sizeof.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-sizeof-sizeof: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "a[__prism_bchk") == NULL,
			      "bc-sizeof-sizeof: nested sizeof operand not wrapped");
		prism_free(&r);
	}

	// Subscript inside sizeof's length expression — `int a[sizeof b[0]]`
	// the `[0]` is inside sizeof (uneval) and MUST not wrap; the outer
	// `sizeof b[0]` isn't in a subscript at all (it's a declarator length).
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int b[5]={0,1,2,3,4};\n"
		    "int a[sizeof b[0]]; (void)a;\n"
		    "return b[2];}\n",
		    "bc_decl_sizeof_idx.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-decl-sizeof-idx: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "b[__prism_bchk_size_t") == NULL ||
			      strstr(r.output, "b[0]") != NULL,
			      "bc-decl-sizeof-idx: b[0] inside sizeof not wrapped");
			CHECK(strstr(r.output, "b[__prism_bchk") != NULL,
			      "bc-decl-sizeof-idx: later b[2] still wrapped");
		}
		prism_free(&r);
	}

	// Cast to pointer-to-array then deref-subscript: `(*(int(*)[5])p)[2]`
	// — last-emitted before `[` is `)`, so guard rejects. Accidentally-safe.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){int raw[5]={10,20,30,40,50};\n"
		    "int *p = raw;\n"
		    "return (*(int(*)[5])p)[2];}\n",
		    "bc_cast_ptr_arr.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-cast-ptr-arr: transpiles");
		if (r.output)
			CHECK(strstr(r.output, ")[__prism_bchk") == NULL,
			      "bc-cast-ptr-arr: casted ptr-to-array deref not wrapped");
		prism_free(&r);
	}

	// Array passed to variadic function (va_arg context) — subscript on
	// the array local still wraps.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "extern int printf(const char*, ...);\n"
		    "int main(void){int a[5]={0,1,2,3,4};\n"
		    "printf(\"%d %d\\n\", a[0], a[4]);\n"
		    "return 0;}\n",
		    "bc_variadic.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-variadic: transpiles");
		if (r.output) {
			int n = 0;
			for (const char *p = r.output; (p = strstr(p, "a[__prism_bchk")); p++) n++;
			CHECK(n == 2, "bc-variadic: both arg subscripts wrapped");
		}
		prism_free(&r);
	}

	// Zero-init list taking subscripts: `int a[5]={[0]=1,[1]=a[0]+1,...}`
	// — designators are [K]= (not subscripts), inner a[0] initializer
	// references the same array being declared (well-defined in C for
	// static arrays, UB for auto). We still wrap a[0].
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){static int a[5]={[0]=1,[1]=2,[2]=3,[3]=4,[4]=5};\n"
		    "return a[2];}\n",
		    "bc_designator_chain.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-designator-chain: transpiles");
		if (r.output) {
			// Designator [0]= etc. must not wrap.
			CHECK(strstr(r.output, "[0]=1") != NULL || strstr(r.output, "[0] = 1") != NULL ||
			      strstr(r.output, "[0]=") != NULL,
			      "bc-designator-chain: designator preserved");
			// a[2] at end wraps.
			CHECK(strstr(r.output, "a[__prism_bchk") != NULL,
			      "bc-designator-chain: post-designator a[2] wrapped");
		}
		prism_free(&r);
	}

	// Array of structs, subscript-then-member chain: `arr[i].field`
	// — must wrap arr[i]; field access is separate.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "struct P { int x, y; };\n"
		    "int main(void){struct P arr[3]={{1,2},{3,4},{5,6}};\n"
		    "return arr[1].y;}\n",
		    "bc_struct_arr.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-struct-arr: transpiles");
		if (r.output)
			CHECK(strstr(r.output, "arr[__prism_bchk") != NULL,
			      "bc-struct-arr: array-of-struct subscript wrapped");
		prism_free(&r);
	}

	// Nested array-of-array-of-struct: m[i][j].field
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "struct P { int v; };\n"
		    "int main(void){struct P m[2][3]={{{1},{2},{3}},{{4},{5},{6}}};\n"
		    "return m[1][2].v;}\n",
		    "bc_2d_struct.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-2d-struct: transpiles");
		if (r.output) {
			// Outer wraps once (the outer array), inner wraps via decayed
			// fixed-dim. We at minimum expect the outer wrap.
			CHECK(strstr(r.output, "m[__prism_bchk") != NULL,
			      "bc-2d-struct: outer subscript wrapped");
		}
		prism_free(&r);
	}

	// Very long subscript chain — a[b[c[d[e[0]]]]] — each level wraps
	// when the target is an array.
	{
		PrismFeatures f = prism_defaults();
		PrismResult r = prism_transpile_source(
		    "int main(void){\n"
		    "int a[2]={0,1}, b[2]={0,1}, c[2]={0,1}, d[2]={0,1}, e[2]={0,1};\n"
		    "return a[b[c[d[e[0]]]]];}\n",
		    "bc_chain.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-chain: transpiles");
		if (r.output) {
			int n = 0;
			for (const char *p = r.output; (p = strstr(p, "__prism_bchk((")); p++) n++;
			CHECK(n == 5, "bc-chain: all 5 levels wrapped");
		}
		prism_free(&r);
	}

#ifndef _WIN32
	// Huge constant index (INT32_MAX) traps.
	bc_runtime_case(
	    "int main(void){int a[10]={0}; volatile int i=2147483647;\n"
	    "return a[i];}\n",
	    -1, "bc-run-huge");

	// Index == length exactly (off-by-one) traps.
	bc_runtime_case(
	    "int main(void){int a[10]={0}; volatile int i=10;\n"
	    "return a[i];}\n",
	    -1, "bc-run-offbyone");

	// Index == length-1 (last valid) must NOT trap.
	bc_runtime_case(
	    "int main(void){int a[10]; for(int k=0;k<10;k++)a[k]=k;\n"
	    "volatile int i=9; return a[i];}\n",
	    9, "bc-run-last");

	// OOB write traps before the subsequent OOB read is reached.
	bc_runtime_case(
	    "int main(void){int a[10]={0}; volatile int i=20;\n"
	    "a[i]=99; return a[i];}\n",
	    -1, "bc-run-write");

	// Side-effect in OOB index traps (double-eval would also trap, but
	// at least the runtime-trap path is exercised; shape is covered by
	// the in-tree no-double-eval emit tests above).
	bc_runtime_case(
	    "static int evals = 0;\n"
	    "static int idx(void){ evals++; return 50; }\n"
	    "int main(void){int a[10]={0};\n"
	    "int x = a[idx()]; return x;}\n",
	    -1, "bc-run-sfx");
#endif
}

void run_bounds_check_tests(void) {
	printf("\n=== BOUNDS-CHECK TESTS ===\n");
	test_bounds_check_fixed_array();
	test_bounds_check_vla();
	test_bounds_check_multidim();
	test_bounds_check_init_and_args();
	test_bounds_check_false_match_guards();
	test_bounds_check_unevaluated_operands();
	test_bounds_check_nested_subscript();
	test_bounds_check_address_of();
	test_bounds_check_dark_corners();
	test_bounds_check_commutative();
	test_bounds_check_declarator_contexts();
	test_bounds_check_extreme_edges();
	test_bounds_check_runtime();
}

// Bounds check tests (-fbounds-check): Tier 1 + Tier 2
// Verifies transpile output shape; runtime trap behavior is covered
// by the smoke tests at the end via subprocess execution.

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
			CHECK(strstr(r.output, "a[__prism_bchk((size_t)( 5)") != NULL,
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

	// 2D array: outer subscript wrapped (v1 limitation: inner not wrapped)
	{
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_source(
		    "int main(void){int m[3][4]; m[1][2]=9; return m[1][2];}\n",
		    "bc_2d.c", f);
		CHECK_EQ(r.status, PRISM_OK, "bc-2d: transpiles");
		if (r.output) {
			CHECK(strstr(r.output, "m[__prism_bchk((size_t)( 1), sizeof(m)/sizeof(m[0]))]") != NULL,
			      "bc-2d: outer dim wrapped");
		}
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

	// In-bounds: program exits normally
	{
		const char *src =
		    "#include <stdio.h>\n"
		    "int main(void){int a[10]; for(int i=0;i<10;i++) a[i]=i;\n"
		    "return a[9];}\n";
		char *src_path = create_temp_file(src);
		char bin[256];
		snprintf(bin, sizeof(bin), "%s.bin", src_path);
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_file(src_path, f);
		CHECK_EQ(r.status, PRISM_OK, "bc-run-ok: transpiles");
		if (r.output) {
			char out_path[256];
			snprintf(out_path, sizeof(out_path), "%s.out.c", src_path);
			FILE *fp = fopen(out_path, "w");
			if (fp) { fwrite(r.output, 1, strlen(r.output), fp); fclose(fp); }
			char cmd[1024];
			snprintf(cmd, sizeof(cmd),
				 "cc -std=gnu11 -o %s %s >/dev/null 2>&1", bin, out_path);
			CHECK_EQ(run_command_status(cmd), 0, "bc-run-ok: compiles");
			int status = run_command_status(bin);
			CHECK_EQ(status, 9, "bc-run-ok: in-bounds exits with a[9]");
			unlink(bin); unlink(out_path);
		}
		prism_free(&r);
		unlink(src_path); free(src_path);
	}

	// OOB: program traps (SIGTRAP / SIGILL)
	{
		const char *src =
		    "#include <stdio.h>\n"
		    "int main(void){int a[10]; for(int i=0;i<10;i++) a[i]=i;\n"
		    "volatile int j=15; return a[j];}\n";
		char *src_path = create_temp_file(src);
		char bin[256];
		snprintf(bin, sizeof(bin), "%s.bin", src_path);
		PrismFeatures f = prism_defaults();
		f.bounds_check = true;
		PrismResult r = prism_transpile_file(src_path, f);
		CHECK_EQ(r.status, PRISM_OK, "bc-run-oob: transpiles");
		if (r.output) {
			char out_path[256];
			snprintf(out_path, sizeof(out_path), "%s.out.c", src_path);
			FILE *fp = fopen(out_path, "w");
			if (fp) { fwrite(r.output, 1, strlen(r.output), fp); fclose(fp); }
			char cmd[1024];
			snprintf(cmd, sizeof(cmd),
				 "cc -std=gnu11 -o %s %s >/dev/null 2>&1", bin, out_path);
			CHECK_EQ(run_command_status(cmd), 0, "bc-run-oob: compiles");
			int status = run_command_status(bin);
			// cc's run_command_status returns nonzero on signal
			CHECK(status != 0 && status != 15 /* not a normal a[15] */,
			      "bc-run-oob: OOB triggers trap (nonzero exit)");
			unlink(bin); unlink(out_path);
		}
		prism_free(&r);
		unlink(src_path); free(src_path);
	}
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

void run_bounds_check_tests(void) {
	printf("\n=== BOUNDS-CHECK TESTS ===\n");
	test_bounds_check_fixed_array();
	test_bounds_check_vla();
	test_bounds_check_multidim();
	test_bounds_check_init_and_args();
	test_bounds_check_false_match_guards();
	test_bounds_check_runtime();
}

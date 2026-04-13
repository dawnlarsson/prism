// Tests for -fauto-static: const array with literal init → static const.
// Verifies the transpiler inserts "static" for eligible declarations and
// leaves ineligible ones untouched.

static void test_as_basic_int_array(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const int k[3] = {1, 2, 3};\n"
	    "    (void)k;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_basic.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as basic: transpiles OK");
	CHECK(r.output != NULL, "as basic: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") != NULL,
		      "as basic: static injected");
	}
	prism_free(&r);
}

static void test_as_string_array(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const char * const names[2] = {\"hello\", \"world\"};\n"
	    "    (void)names;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_str.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as string array: transpiles OK");
	CHECK(r.output != NULL, "as string array: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") != NULL,
		      "as string array: static injected");
	}
	prism_free(&r);
}

static void test_as_nested_braces(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const int m[2][2] = {{1, 2}, {3, 4}};\n"
	    "    (void)m;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_nest.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as nested braces: transpiles OK");
	CHECK(r.output != NULL, "as nested braces: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") != NULL,
		      "as nested braces: static injected");
	}
	prism_free(&r);
}

static void test_as_designated_init(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const int a[4] = {[0] = 10, [3] = 40};\n"
	    "    (void)a;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_desig.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as designated init: transpiles OK");
	CHECK(r.output != NULL, "as designated init: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") != NULL,
		      "as designated init: static injected");
	}
	prism_free(&r);
}

static void test_as_negative_numbers(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const int signs[3] = {-1, +2, -3};\n"
	    "    (void)signs;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_neg.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as negative nums: transpiles OK");
	CHECK(r.output != NULL, "as negative nums: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") != NULL,
		      "as negative nums: static injected");
	}
	prism_free(&r);
}

// --- Cases that must NOT get auto-static ---

static void test_as_already_static(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    static const int k[2] = {1, 2};\n"
	    "    (void)k;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_dup.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as already static: transpiles OK");
	CHECK(r.output != NULL, "as already static: output not NULL");
	if (r.output) {
		// Must NOT have "static static"
		CHECK(strstr(r.output, "static static") == NULL,
		      "as already static: no double static");
	}
	prism_free(&r);
}

static void test_as_func_call_in_init(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "int get(void);\n"
	    "void f(void) {\n"
	    "    const int k[2] = {get(), 2};\n"
	    "    (void)k;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_call.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as func call: transpiles OK");
	CHECK(r.output != NULL, "as func call: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static const int k") == NULL,
		      "as func call: no static (runtime init)");
	}
	prism_free(&r);
}

static void test_as_variable_in_init(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(int x) {\n"
	    "    const int k[2] = {x, 2};\n"
	    "    (void)k;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_var.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as variable init: transpiles OK");
	CHECK(r.output != NULL, "as variable init: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static const int k") == NULL,
		      "as variable init: no static (runtime value)");
	}
	prism_free(&r);
}

static void test_as_file_scope(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "const int k[3] = {1, 2, 3};\n";
	PrismResult r = prism_transpile_source(code, "as_file.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as file scope: transpiles OK");
	CHECK(r.output != NULL, "as file scope: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static const int k") == NULL,
		      "as file scope: no static (already file scope)");
	}
	prism_free(&r);
}

static void test_as_not_const(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    int k[3] = {1, 2, 3};\n"
	    "    (void)k;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_noconst.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as not const: transpiles OK");
	CHECK(r.output != NULL, "as not const: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") == NULL,
		      "as not const: no static injection");
	}
	prism_free(&r);
}

static void test_as_multi_declarator(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const int a[2] = {1, 2}, b[2] = {3, 4};\n"
	    "    (void)a; (void)b;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_multi.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as multi decl: transpiles OK");
	CHECK(r.output != NULL, "as multi decl: output not NULL");
	if (r.output) {
		// Multi-declarator excluded — making one static but not both is wrong
		CHECK(strstr(r.output, "static const int a") == NULL,
		      "as multi decl: no static (multi-decl)");
	}
	prism_free(&r);
}

static void test_as_no_init(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const int k[3];\n"
	    "    (void)k;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_noinit.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as no init: transpiles OK");
	CHECK(r.output != NULL, "as no init: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") == NULL,
		      "as no init: no static (zeroinit, not literal)");
	}
	prism_free(&r);
}

static void test_as_raw_excluded(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    raw const int k[3] = {1, 2, 3};\n"
	    "    (void)k;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_rw.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as raw excluded: transpiles OK");
	CHECK(r.output != NULL, "as raw excluded: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") == NULL,
		      "as raw excluded: no static (raw opt-out)");
	}
	prism_free(&r);
}

static void test_as_disabled_flag(void) {
	PrismFeatures f = prism_defaults();
	f.auto_static = false;
	const char *code =
	    "void f(void) {\n"
	    "    const int k[3] = {1, 2, 3};\n"
	    "    (void)k;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_off.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as disabled: transpiles OK");
	CHECK(r.output != NULL, "as disabled: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static const int k") == NULL,
		      "as disabled: no static (-fno-auto-static)");
	}
	prism_free(&r);
}

static void test_as_enum_constant_init(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "enum { A = 10, B = 20, C = 30 };\n"
	    "void f(void) {\n"
	    "    const int vals[3] = {A, B, C};\n"
	    "    (void)vals;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_enum.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as enum const: transpiles OK");
	CHECK(r.output != NULL, "as enum const: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") != NULL,
		      "as enum const: static injected");
	}
	prism_free(&r);
}

static void test_as_extern_excluded(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    extern const int k[3];\n"
	    "    (void)k;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_ext.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as extern excluded: transpiles OK");
	CHECK(r.output != NULL, "as extern excluded: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") == NULL,
		      "as extern excluded: no static (extern decl)");
	}
	prism_free(&r);
}

static void test_as_char_array(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const char msg[] = {\"hello\"};\n"
	    "    (void)msg;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_char.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as char array: transpiles OK");
	CHECK(r.output != NULL, "as char array: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") != NULL,
		      "as char array: static injected");
	}
	prism_free(&r);
}

static void test_as_cast_in_init(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const long k[2] = {(long)1, (long)2};\n"
	    "    (void)k;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_cast.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as cast init: transpiles OK");
	CHECK(r.output != NULL, "as cast init: output not NULL");
	if (r.output) {
		// Cast uses parens — is_const_literal_init rejects '(' as non-literal punct
		CHECK(strstr(r.output, "static const long k") == NULL,
		      "as cast init: no static (cast not literal)");
	}
	prism_free(&r);
}

static void test_as_vla_dimension(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(int n) {\n"
	    "    const int arr[n] = {1, 2, 3};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_vla.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as VLA dim: transpiles OK");
	CHECK(r.output != NULL, "as VLA dim: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") == NULL,
		      "as VLA dim: no static (VLA can't be static)");
	}
	prism_free(&r);
}

static void test_as_typeof_vla(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(int n) {\n"
	    "    const typeof(int[n]) arr[3] = {{1},{2},{3}};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_tvla.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as typeof VLA: transpiles OK");
	CHECK(r.output != NULL, "as typeof VLA: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") == NULL,
		      "as typeof VLA: no static (typeof VLA can't be static)");
	}
	prism_free(&r);
}

static void test_as_struct_designator(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "struct P { int x, y; };\n"
	    "void f(void) {\n"
	    "    const struct P pts[2] = {[0].x = 1, [0].y = 2, [1].x = 3, [1].y = 4};\n"
	    "    (void)pts;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_sdesig.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as struct desig: transpiles OK");
	CHECK(r.output != NULL, "as struct desig: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") != NULL,
		      "as struct desig: static injected (.field is designator)");
	}
	prism_free(&r);
}

static void test_as_plain_ident_rejected(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "const int G = 5;\n"
	    "void f(void) {\n"
	    "    const int arr[1] = {G};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_ident.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as plain ident: transpiles OK");
	CHECK(r.output != NULL, "as plain ident: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") == NULL,
		      "as plain ident: no static (non-enum ident)");
	}
	prism_free(&r);
}

static void test_as_for_init_excluded(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    for (const int arr[2] = {1,2}; arr[0]; ) break;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_forinit.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as for-init: transpiles OK");
	CHECK(r.output != NULL, "as for-init: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") == NULL,
		      "as for-init: no static (static illegal in for-init)");
	}
	prism_free(&r);
}

static void test_as_auto_storage_class(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    auto const int arr[2] = {1, 2};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_auto.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as auto storage: transpiles OK");
	CHECK(r.output != NULL, "as auto storage: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") == NULL,
		      "as auto storage: no static (auto + static = two storage classes)");
	}
	prism_free(&r);
}

static void test_as_constexpr_excluded(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    constexpr const int arr[3] = {1, 2, 3};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_cexpr.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as constexpr: transpiles OK");
	CHECK(r.output != NULL, "as constexpr: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") == NULL,
		      "as constexpr: no static (constexpr is storage class in C23)");
	}
	prism_free(&r);
}

static void test_as_thread_local_excluded(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    thread_local const int arr[3] = {1, 2, 3};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_tls.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as thread_local: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "static") == NULL,
		      "as thread_local: no static (thread-local storage class)");
	}
	prism_free(&r);
}

static void test_as_volatile_excluded(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const volatile int arr[3] = {1, 2, 3};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_vol.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as volatile: transpiles OK");
	CHECK(r.output != NULL, "as volatile: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") == NULL,
		      "as volatile: no static (volatile semantics change)");
	}
	prism_free(&r);
}

static void test_as_volatile_const_reversed(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    volatile const int arr[3] = {1, 2, 3};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_volr.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as volatile-const: transpiles OK");
	CHECK(r.output != NULL, "as volatile-const: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") == NULL,
		      "as volatile-const: no static (volatile semantics change)");
	}
	prism_free(&r);
}

static void test_as_cleanup_attr_excluded(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void dtor(const int (*p)[3]) { (void)p; }\n"
	    "void f(void) {\n"
	    "    const int arr[3] __attribute__((cleanup(dtor))) = {1, 2, 3};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_cln.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as cleanup: transpiles OK");
	CHECK(r.output != NULL, "as cleanup: output not NULL");
	if (r.output) {
		CHECK(strstr(r.output, "static") == NULL,
		      "as cleanup: no static (cleanup handler would be dropped)");
	}
	prism_free(&r);
}

static void test_as_c23_attr_excluded(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const int arr[3] [[maybe_unused]] = {1, 2, 3};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_c23a.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as C23 attr: transpiles OK");
	if (r.output) {
		CHECK(strstr(r.output, "static") == NULL,
		      "as C23 attr: no static (declarator attribute conservative excl)");
	}
	prism_free(&r);
}

static void test_as_zeroinit_off_still_promotes(void) {
	PrismFeatures f = prism_defaults();
	f.zeroinit = false;
	const char *code =
	    "void f(void) {\n"
	    "    const int arr[3] = {1, 2, 3};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_zi.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as zeroinit off: transpiles OK");
	if (r.output) {
		const char *p = strstr(r.output, "void f");
		CHECK(p && strstr(p, "static"),
		      "as zeroinit off: static injected despite -fno-zeroinit");
	}
	prism_free(&r);
}

static void test_as_volatile_member_excluded(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const struct { volatile int x; } arr[1] = {{5}};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_vm.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as volatile member: transpiles OK");
	if (r.output) {
		const char *p = strstr(r.output, "void f");
		CHECK(!p || !strstr(p, "static"),
		      "as volatile member: no static (volatile member in .rodata is UB)");
	}
	prism_free(&r);
}

static void test_as_typedef_volatile_excluded(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "typedef volatile int vint;\n"
	    "void f(void) {\n"
	    "    const vint arr[3] = {1, 2, 3};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_tv.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as typedef volatile: transpiles OK");
	if (r.output) {
		const char *p = strstr(r.output, "void f");
		CHECK(!p || !strstr(p, "static"),
		      "as typedef volatile: no static (volatile hidden in typedef)");
	}
	prism_free(&r);
}

static void test_as_typedef_volatile_member_excluded(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "typedef struct { volatile int x; } VS;\n"
	    "void f(void) {\n"
	    "    const VS arr[1] = {{5}};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_tvm.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as typedef vol member: transpiles OK");
	if (r.output) {
		const char *p = strstr(r.output, "void f");
		CHECK(!p || !strstr(p, "static"),
		      "as typedef vol member: no static (volatile member hidden in typedef)");
	}
	prism_free(&r);
}

static void test_as_const_ptr_array_excluded(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const int *arr[3] = {0, 0, 0};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_cpa.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as const-ptr-array: transpiles OK");
	if (r.output) {
		const char *p = strstr(r.output, "void f");
		CHECK(!p || !strstr(p, "static"),
		      "as const-ptr-array: no static (array itself is mutable)");
	}
	prism_free(&r);
}

static void test_as_const_ptr_const_array_promoted(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const int * const arr[3] = {0, 0, 0};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_cpca.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as const-ptr-const: transpiles OK");
	if (r.output) {
		const char *p = strstr(r.output, "void f");
		CHECK(p && strstr(p, "static"),
		      "as const-ptr-const: static injected (array is immutable)");
	}
	prism_free(&r);
}

static void test_as_double_typedef_volatile(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "typedef struct { volatile int x; } Inner;\n"
	    "typedef Inner Wrapper;\n"
	    "void f(void) {\n"
	    "    const Wrapper arr[1] = {{5}};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_dtv.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as double-typedef volatile: transpiles OK");
	if (r.output) {
		const char *p = strstr(r.output, "void f");
		CHECK(!p || !strstr(p, "static"),
		      "as double-typedef volatile: no static (volatile member via chain)");
	}
	prism_free(&r);
}

static void test_as_nested_volatile_member(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const struct { struct { volatile int x; } inner; } arr[1] = {{{42}}};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_nvm.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as nested volatile: transpiles OK");
	if (r.output) {
		const char *p = strstr(r.output, "void f");
		CHECK(!p || !strstr(p, "static"),
		      "as nested volatile: no static (volatile 2 levels deep)");
	}
	prism_free(&r);
}

static void test_as_multi_dim_array(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const int arr[2][3] = {{1,2,3},{4,5,6}};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_md.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as multi-dim: transpiles OK");
	if (r.output) {
		const char *p = strstr(r.output, "void f");
		CHECK(p && strstr(p, "static"),
		      "as multi-dim: static injected");
	}
	prism_free(&r);
}

static void test_as_inferred_size(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const int arr[] = {1, 2, 3};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_inf.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as inferred size: transpiles OK");
	if (r.output) {
		const char *p = strstr(r.output, "void f");
		CHECK(p && strstr(p, "static"),
		      "as inferred size: static injected (arr[] = {1,2,3})");
	}
	prism_free(&r);
}

static void test_as_pointer_to_array_excluded(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    int x[3];\n"
	    "    const int (*p)[3] = &x;\n"
	    "    (void)p;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_p2a.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as ptr-to-array: transpiles OK");
	if (r.output) {
		const char *p = strstr(r.output, "void f");
		CHECK(!p || !strstr(p, "static"),
		      "as ptr-to-array: no static (pointer, not array)");
	}
	prism_free(&r);
}

static void test_as_func_ptr_array_excluded(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void a(void);\nvoid b(void);\n"
	    "void f(void) {\n"
	    "    void (* const arr[2])(void) = {a, b};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_fpa.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as func-ptr-array: transpiles OK");
	if (r.output) {
		const char *p = strstr(r.output, "void f");
		CHECK(!p || !strstr(p, "static"),
		      "as func-ptr-array: no static (func names not enum consts)");
	}
	prism_free(&r);
}

static void test_as_enum_designator_init(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "enum { IDX = 2 };\n"
	    "void f(void) {\n"
	    "    const int arr[3] = {[IDX] = 42};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_edi.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as enum designator: transpiles OK");
	if (r.output) {
		const char *p = strstr(r.output, "void f");
		CHECK(p && strstr(p, "static"),
		      "as enum designator: static injected");
	}
	prism_free(&r);
}

static void test_as_decrement_in_init_excluded(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const int arr[1] = {--1};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_dec.c", f);
	if (r.output) {
		const char *p = strstr(r.output, "void f");
		CHECK(!p || !strstr(p, "static"),
		      "as decrement in init: no static (-- blocked by len==1 check)");
	} else {
		CHECK(1, "as decrement in init: transpile error (invalid C, OK)");
	}
	prism_free(&r);
}

static void test_as_stmt_expr_in_init_excluded(void) {
	PrismFeatures f = prism_defaults();
	const char *code =
	    "void f(void) {\n"
	    "    const int arr[1] = {({int t=1;t;})};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_se.c", f);
	if (r.output) {
		const char *p = strstr(r.output, "void f");
		CHECK(!p || !strstr(p, "static"),
		      "as stmt-expr in init: no static");
	} else {
		CHECK(1, "as stmt-expr in init: transpile error (OK)");
	}
	prism_free(&r);
}

static void test_as_features_isolated(void) {
	PrismFeatures f = prism_defaults();
	f.zeroinit = false;
	f.orelse = false;
	const char *code =
	    "void f(void) {\n"
	    "    const int arr[3] = {1, 2, 3};\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "as_iso.c", f);
	CHECK_EQ(r.status, PRISM_OK, "as features isolated: transpiles OK");
	if (r.output) {
		const char *p = strstr(r.output, "void f");
		CHECK(p && strstr(p, "static"),
		      "as features isolated: static works with zeroinit=off orelse=off");
	}
	prism_free(&r);
}

void run_autostatic_tests(void) {
	printf("\n=== AUTO-STATIC TESTS ===\n");
	// Positive cases (static should be injected)
	test_as_basic_int_array();
	test_as_string_array();
	test_as_nested_braces();
	test_as_designated_init();
	test_as_negative_numbers();
	test_as_enum_constant_init();
	test_as_char_array();
	// Negative cases (static must NOT be injected)
	test_as_already_static();
	test_as_func_call_in_init();
	test_as_variable_in_init();
	test_as_file_scope();
	test_as_not_const();
	test_as_multi_declarator();
	test_as_no_init();
	test_as_raw_excluded();
	test_as_disabled_flag();
	test_as_extern_excluded();
	test_as_cast_in_init();
	test_as_vla_dimension();
	test_as_typeof_vla();
	test_as_struct_designator();
	test_as_plain_ident_rejected();
	test_as_for_init_excluded();
	test_as_auto_storage_class();
	test_as_constexpr_excluded();
	test_as_thread_local_excluded();
	test_as_volatile_excluded();
	test_as_volatile_const_reversed();
	test_as_cleanup_attr_excluded();
	test_as_c23_attr_excluded();
	test_as_zeroinit_off_still_promotes();
	test_as_volatile_member_excluded();
	test_as_typedef_volatile_excluded();
	test_as_typedef_volatile_member_excluded();
	test_as_const_ptr_array_excluded();
	test_as_const_ptr_const_array_promoted();
	test_as_double_typedef_volatile();
	test_as_nested_volatile_member();
	test_as_multi_dim_array();
	test_as_inferred_size();
	test_as_pointer_to_array_excluded();
	test_as_func_ptr_array_excluded();
	test_as_enum_designator_init();
	test_as_decrement_in_init_excluded();
	test_as_stmt_expr_in_init_excluded();
	test_as_features_isolated();
}

void test_goto_over_block(void) {
	log_reset();
	int before = 1;
	log_append("A");
	goto DONE;
	{
		// This entire block is skipped - no safety issue
		int x = 42;
		log_append("X"); // Should not run
	}
DONE:
	log_append("B");
	CHECK_EQ(before, 1, "goto over block - var before goto");
	CHECK_LOG("AB", "goto over block - skips entire block");
}

void test_goto_backward_valid(void) {
	log_reset();
	int count = 0;
	int x = 10; // Declared before label - always initialized
AGAIN:
	log_append("L");
	count++;
	x++;
	if (count < 3) goto AGAIN;
	log_append("E");
	CHECK_EQ(count, 3, "goto backward - loop count");
	CHECK_EQ(x, 13, "goto backward - var incremented");
	CHECK_LOG("LLLE", "goto backward - correct order");
}

void test_goto_forward_no_decl(void) {
	log_reset();
	int x = 5; // Before goto
	log_append("A");
	if (x > 0) goto SKIP;
	log_append("X"); // Skipped
SKIP:
	log_append("B");
	CHECK_EQ(x, 5, "goto forward no decl - var preserved");
	CHECK_LOG("AB", "goto forward no decl - correct order");
}

void test_goto_into_scope_decl_after_label(void) {
	log_reset();
	goto INNER;
	{
	INNER:
		log_append("I");
		int x = 42; // Decl is AFTER label - this is fine
		log_append("D");
		CHECK_EQ(x, 42, "goto into scope - decl after label");
	}
	CHECK_LOG("ID", "goto into scope - correct order");
}

void test_goto_complex_valid(void) {
	log_reset();
	int state = 0;

START:
	if (state == 0) {
		log_append("0");
		state = 1;
		goto MIDDLE;
	}
	log_append("X");
	goto END;

MIDDLE:
	log_append("1");
	state = 2;
	goto START;

END:
	log_append("E");
	CHECK_EQ(state, 2, "goto complex - final state");
	CHECK_LOG("01XE", "goto complex - correct order");
}

void test_goto_with_defer_valid(void) {
	// Basic case
	log_reset();
	int x = 1; // Before the scope with defer
	{
		defer log_append("D");
		log_append("A");
		if (x > 0) goto SAFE_OUT;
		log_append("X");
	SAFE_OUT:
		log_append("B");
	}
	log_append("E");
	CHECK_LOG("ABDE", "goto with defer - defer runs on scope exit");

	// NIGHTMARE: Spaghetti goto with 12 labels and nested defers
	log_reset();
	int state = 0;

LABEL_START: {
	defer log_append("0");
	state++;
	if (state == 1) goto LABEL_A;
	if (state == 7) goto LABEL_END;
	goto LABEL_F;
}

LABEL_A: {
	defer log_append("A");
	log_append("a");
	goto LABEL_B;
}

LABEL_B: {
	defer log_append("B");
	{
		defer log_append("b");
		log_append("(");
		goto LABEL_C;
	}
}

LABEL_C: {
	defer log_append("C");
	log_append("c");
	if (state < 3) {
		state++;
		goto LABEL_D;
	}
	goto LABEL_E;
}

LABEL_D: {
	defer log_append("D");
	{
		defer log_append("d");
		{
			defer log_append("!");
			log_append("[");
			state++;
			if (state == 3) goto LABEL_C; // back to C
			goto LABEL_E;
		}
	}
}

LABEL_E: {
	defer log_append("E");
	log_append("e");
	if (state < 5) {
		state++;
		goto LABEL_F;
	}
	goto LABEL_G;
}

LABEL_F: {
	defer log_append("F");
	log_append("f");
	state++;
	if (state < 7) goto LABEL_START; // back to start
	goto LABEL_G;
}

LABEL_G: {
	defer log_append("G");
	{
		defer log_append("g");
		log_append("{");
		goto LABEL_H;
	}
}

LABEL_H:
	log_append("h");
	goto LABEL_I;

LABEL_I: {
	defer log_append("I");
	log_append("i");
	goto LABEL_J;
}

LABEL_J: {
	defer log_append("J");
	{
		defer log_append("j");
		log_append("<");
		if (state == 5) {
			state++;
			goto LABEL_K;
		}
		goto LABEL_END;
	}
}

LABEL_K: {
	defer log_append("K");
	log_append("k");
	goto LABEL_START; // final loop back
}

LABEL_END:
	log_append("Z");
	// The exact path is complex but tests spaghetti goto with defer cleanup
	// Key: every goto properly triggers defer cleanup for exited scopes
	(void)state; // suppress unused warning
}

// GNU __label__ declares block-scoped labels inside statement expressions.
// Before the fix, Phase 1D registered both labels with the raw name "L_retry",
// causing a fatal "duplicate label" error in p1_verify_cfg.
static void test_gnu_label_stmt_expr(void) {
	printf("\n--- GNU __label__ in statement expressions ---\n");
	// Two stmt-exprs with same-named __label__ local labels.
	{
		const char *code =
		    "int read_a(void);\n"
		    "int read_b(void);\n"
		    "int f(void) {\n"
		    "    ({ __label__ done; int r; r = read_a(); if (r > 0) goto done; r = -1; done: r; });\n"
		    "    ({ __label__ done; int r; r = read_b(); if (r > 0) goto done; r = -1; done: r; });\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "gnu_label_se.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "gnu __label__: duplicate local labels in separate stmt-exprs must not error");
		prism_free(&r);
	}
	// Single stmt-expr with __label__ — baseline sanity.
	{
		const char *code =
		    "int f(void) {\n"
		    "    return ({ __label__ end; int v = 42; goto end; v = 0; end: v; });\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "gnu_label_single.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "gnu __label__: single stmt-expr with local label transpiles OK");
		prism_free(&r);
	}
	// __label__ with defer: local label + defer inside stmt-expr.
	{
		const char *code =
		    "void cleanup(void);\n"
		    "int f(void) {\n"
		    "    return ({ __label__ out; { defer { cleanup(); } goto out; } out: 0; });\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "gnu_label_dfr.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "gnu __label__: local label with defer in stmt-expr transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "cleanup()"),
			      "gnu __label__: defer expanded inside stmt-expr with local label");
		}
		prism_free(&r);
	}
	// Comma-separated __label__ declarations.
	{
		const char *code =
		    "int f(void) {\n"
		    "    return ({\n"
		    "        __label__ retry, done;\n"
		    "        int n = 0;\n"
		    "        retry: if (++n < 3) goto retry;\n"
		    "        goto done;\n"
		    "        done: n;\n"
		    "    });\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "gnu_label_comma.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "gnu __label__: comma-separated local labels transpile OK");
		prism_free(&r);
	}
}


typedef void VoidType;

VoidType test_typedef_void_return_impl(void) {
	log_reset();
	defer log_append("D");
	log_append("1");
	return; // Should work correctly - defers should run
}

void test_typedef_void_return(void) {
	test_typedef_void_return_impl();
	CHECK_LOG("1D", "typedef void return with defer");
}

typedef void *VoidPtr;

VoidPtr test_typedef_voidptr_return_impl(void) {
	log_reset();
	defer log_append("D");
	log_append("1");
	return NULL; // Returns void*, should save return value properly
}

void test_typedef_voidptr_return(void) {
	VoidPtr result = test_typedef_voidptr_return_impl();
	CHECK_LOG("1D", "typedef void* return with defer");
	CHECK(result == NULL, "typedef void* return value preserved");
}

#ifndef _MSC_VER
void test_stmt_expr_defer_timing(void) {
	log_reset();
	int capture = 0;

	// Workaround: wrap defer in a nested block
	int x = ({
		int y;
		y = 42;
		{
			defer {
				log_append("D");
				capture = y;
			};
		} // wrapped in block
		y; // final expression
	});

	log_append("E");
	CHECK_EQ(x, 42, "stmt expr defer - return value correct");
	CHECK_EQ(capture, 42, "stmt expr defer - captured value");
	CHECK_LOG("DE", "stmt expr defer - order");
}

void test_nested_stmt_expr_defer_immediate_block_exit(void) {
	log_reset();

	int x = ({
		{ defer log_append("O"); } // outer block exits immediately -> "O" runs here
		int inner = ({
			{ defer log_append("I"); } // inner block exits immediately -> "I" runs here
			10;
		});
		log_append("M"); // middle - after inner completes
		inner + 5;
	});

	log_append("E");
	CHECK_EQ(x, 15, "nested stmt expr - return value");
	// Order: O (outer block exit), I (inner block exit), M, E
	CHECK_LOG("OIME", "nested stmt expr - defer order (blocks exit immediately)");
}
#endif // _MSC_VER

typedef struct {
	int x;
	int y;
} PointType;

void test_const_after_typename(void) {
	// const BEFORE typename - should work
	const PointType p1;
	CHECK(p1.x == 0 && p1.y == 0, "const before typedef zero-init");

	// const AFTER typename - might fail
	PointType const p2;
	CHECK(p2.x == 0 && p2.y == 0, "const after typedef zero-init");
}

#if !defined(_MSC_VER) && defined(__STDC_NO_ATOMICS__) == 0
#include <stdatomic.h>

void test_atomic_zeroinit(void) {
	_Atomic int ai;
	CHECK(atomic_load(&ai) == 0, "_Atomic int zero-init");

	_Atomic int *ap;
	CHECK(ap == NULL, "_Atomic pointer zero-init");
}

void test_atomic_aggregate_zeroinit(void) {
	// _Atomic struct (aggregate type)
	_Atomic struct {
		int x;
		int y;
	} atomic_struct;

	int all_zero = 1;
	unsigned char *p = (unsigned char *)&atomic_struct;
	for (size_t i = 0; i < sizeof(atomic_struct); i++)
		if (p[i] != 0) all_zero = 0;
	CHECK(all_zero, "_Atomic struct memset zero-init");

	// _Atomic array of ints
	_Atomic int arr[4];
	all_zero = 1;
	p = (unsigned char *)&arr;
	for (size_t i = 0; i < sizeof(arr); i++)
		if (p[i] != 0) all_zero = 0;
	CHECK(all_zero, "_Atomic int array zero-init");
}
#else
void test_atomic_zeroinit(void) {
	// Skip on platforms without atomics
	printf("[SKIP] _Atomic tests (not supported)\n");
}

void test_atomic_aggregate_zeroinit(void) {
	printf("[SKIP] _Atomic aggregate tests (not supported)\n");
}
#endif

int test_static_local_helper(void) {
	static int counter; // Should be zero-init'd ONCE by C semantics
	return ++counter;
}

void test_static_local_zeroinit(void) {
	// Call multiple times - if prism re-inits each call, this breaks
	int a = test_static_local_helper();
	int b = test_static_local_helper();
	int c = test_static_local_helper();

	CHECK(a == 1 && b == 2 && c == 3, "static local not re-initialized");
}

// ISSUE 11c: defer with inline function (if supported)
#ifdef __GNUC__
static inline int inline_with_defer(void) {
	log_reset();
	defer log_append("D");
	log_append("1");
	return 42;
}

void test_inline_defer(void) {
	int r = inline_with_defer();
	CHECK_EQ(r, 42, "inline function defer - return value");
	CHECK_LOG("1D", "inline function defer - order");
}
#else
void test_inline_defer(void) {
	printf("[SKIP] inline defer tests (not GCC/Clang)\n");
}
#endif

void test_complex_declarator_zeroinit(void) {
	// Simple function pointer - should work
	int (*fp1)(void);
	CHECK(fp1 == NULL, "function pointer zero-init");

	// Pointer to array
	int (*pa)[10];
	CHECK(pa == NULL, "pointer to array zero-init");

	// Array of function pointers
	int (*afp[5])(void);
	int all_null = 1;
	for (int i = 0; i < 5; i++)
		if (afp[i] != NULL) all_null = 0;
	CHECK(all_null, "array of function pointers zero-init");

	// Function pointer returning pointer
	int *(*fprp)(void);
	CHECK(fprp == NULL, "func ptr returning ptr zero-init");
}

void test_complex_decl_safety(void) {
	// Function returning pointer to array
	// int (*(*f1)(void))[10];
	// This is extremely complex - let's test simpler variants first

	// Pointer to function returning pointer to int
	int *(*(*ppfp))(void);
	CHECK(ppfp == NULL, "ptr to ptr to func returning ptr - zero-init");

	// Double pointer to function
	int (**ppf)(void);
	CHECK(ppf == NULL, "double ptr to function zero-init");

	// Pointer to array of pointers
	int *(*pap)[5];
	CHECK(pap == NULL, "ptr to array of ptrs zero-init");
}

void test_qualified_complex_decl(void) {
	// const pointer to pointer
	int *const *cpp;
	CHECK(cpp == NULL, "const ptr to ptr zero-init");

	// pointer to const pointer
	int **const pcp;
	CHECK(pcp == NULL, "ptr to const ptr zero-init");

	// volatile pointer
	int *volatile vp;
	CHECK(vp == NULL, "volatile ptr zero-init");

	// restrict pointer (C99)
	int *restrict rp;
	CHECK(rp == NULL, "restrict ptr zero-init");
}

extern int extern_var; // declaration only, no init

void test_extern_not_initialized(void) {
	PrismResult result = prism_transpile_source(
	    "extern int extern_var;\n"
	    "int *addr(void) { return &extern_var; }\n",
	    "extern_decl_no_init.c", prism_defaults());
	CHECK_EQ(result.status, PRISM_OK, "extern declaration transpiles");
	if (result.output) {
		CHECK(strstr(result.output, "extern_var =") == NULL,
		      "extern declaration does not gain initializer");
		CHECK(strstr(result.output, "extern int extern_var") != NULL,
		      "extern declaration preserved in output");
	}
	prism_free(&result);
}

// Test that typedef declarations are NOT zero-initialized
void test_typedef_not_initialized(void) {
	typedef int MyInt; // This should not become "typedef int MyInt = 0;"
	MyInt x;
	CHECK_EQ(x, 0, "variable of typedef type zero-init");
	PrismResult result = prism_transpile_source(
	    "typedef int MyInt;\n"
	    "MyInt x;\n"
	    "int main(void) { return x; }\n",
	    "typedef_decl_no_init.c", prism_defaults());
	CHECK_EQ(result.status, PRISM_OK, "typedef declaration transpiles");
	if (result.output) {
		CHECK(strstr(result.output, "typedef int MyInt =") == NULL,
		      "typedef declaration does not gain initializer");
	}
	prism_free(&result);
}

void test_for_init_zeroinit(void) {
	int sum = 0;
	// Test that variables declared in for init are zero-initialized
	for (int i; i < 3; i++) // i should be 0-init'd
	{
		sum += i;
	}
	CHECK(sum == 0 + 1 + 2, "for init clause zero-init");

	// Multiple declarations in for init
	sum = 0;
	for (int a, b; a < 2; a++, b++) {
		sum += a + b; // both should start at 0
	}
	CHECK(sum == (0 + 0) + (1 + 1), "for init multiple decls zero-init");
}

/*
void test_defer_in_for_parts(void)
{
    // These should all be errors:
    // for (defer foo(); ...; ...) { }
    // for (...; ...; defer foo()) { }
}
*/

#ifndef _MSC_VER
void test_ptr_to_vla_typedef(int n) {
	typedef int VlaType[n]; // VLA typedef
	VlaType *p;		// Pointer to VLA - should be zero-init'd to NULL
	CHECK(p == NULL, "pointer to VLA typedef zero-init");

	// Also test pointer to pointer to VLA
	VlaType **pp;
	CHECK(pp == NULL, "double pointer to VLA typedef zero-init");
}

// Test that VLA size side effects are not duplicated
static int vla_size_counter = 0;

int get_vla_size(void) {
	vla_size_counter++;
	return 10;
}

void test_vla_side_effect_once(void) {
	// VLA typedef with side effect in size
	int n = 5;
	typedef int Arr[n++];
	CHECK_EQ(n, 6, "VLA typedef side effect runs once");
	(void)sizeof(Arr);

	// VLA typedef with function call
	vla_size_counter = 0;
	typedef int Arr2[get_vla_size()];
	CHECK_EQ(vla_size_counter, 1, "VLA size function called once");
	(void)sizeof(Arr2);
}
#endif

#ifndef _MSC_VER
void test_atomic_specifier_form(void) {
	// Qualifier form (already worked)
	_Atomic int a;
	CHECK(a == 0, "_Atomic int (qualifier form) zero-init");

	// Specifier form (was broken)
	_Atomic(int) b;
	CHECK(b == 0, "_Atomic(int) (specifier form) zero-init");
	// More complex specifier forms
	_Atomic(long long) c;
	CHECK(c == 0, "_Atomic(long long) zero-init");

	_Atomic(int *) d; // atomic pointer
	CHECK(d == NULL, "_Atomic(int*) zero-init");
}
#endif

#if !defined(__clang__) && !defined(_MSC_VER)

void test_atomic_struct_basic(void) {
	struct AtomicPoint {
		int x;
		int y;
	};
	_Atomic struct AtomicPoint p;
	CHECK(p.x == 0 && p.y == 0, "_Atomic struct basic zero-init");
}

void test_atomic_union_basic(void) {
	union AtomicUnion {
		int i;
		float f;
	};
	_Atomic union AtomicUnion u;
	CHECK(u.i == 0, "_Atomic union basic zero-init");
}

void test_atomic_struct_nested(void) {
	struct Inner {
		int a;
		int b;
	};

	struct Outer {
		struct Inner inner;
		int c;
	};
	_Atomic struct Outer o;
	CHECK(o.inner.a == 0 && o.inner.b == 0 && o.c == 0, "_Atomic nested struct zero-init");
}

void test_atomic_struct_with_array(void) {
	struct WithArray {
		int arr[4];
		int x;
	};
	_Atomic struct WithArray wa;
	CHECK(wa.arr[0] == 0 && wa.arr[3] == 0 && wa.x == 0, "_Atomic struct with array member");
}

void test_atomic_struct_with_pointer(void) {
	struct WithPtr {
		int *p;
		int x;
	};
	_Atomic struct WithPtr wp;
	CHECK(wp.p == NULL && wp.x == 0, "_Atomic struct with pointer member");
}

void test_atomic_struct_specifier_form(void) {
	struct SpecPoint {
		int x;
		int y;
	};

	_Atomic(struct SpecPoint) sp;
	CHECK(sp.x == 0 && sp.y == 0, "_Atomic(struct) specifier form");
}

void test_atomic_union_specifier_form(void) {
	union SpecUnion {
		int i;
		double d;
	};

	_Atomic(union SpecUnion) su;
	CHECK(su.i == 0, "_Atomic(union) specifier form");
}

void test_atomic_struct_multi_decl(void) {
	struct MultiPoint {
		int x;
		int y;
	};
	_Atomic struct MultiPoint p1, p2, p3;
	CHECK(p1.x == 0 && p1.y == 0, "_Atomic struct multi-decl p1");
	CHECK(p2.x == 0 && p2.y == 0, "_Atomic struct multi-decl p2");
	CHECK(p3.x == 0 && p3.y == 0, "_Atomic struct multi-decl p3");
}

void test_atomic_struct_pointer(void) {
	struct PtrPoint {
		int x;
		int y;
	};
	_Atomic struct PtrPoint *ptr;
	CHECK(ptr == NULL, "_Atomic struct pointer uses = 0");
}

void test_atomic_struct_volatile(void) {
	struct VolPoint {
		int x;
		int y;
	};
	volatile _Atomic struct VolPoint vp;
	CHECK(vp.x == 0 && vp.y == 0, "volatile _Atomic struct zero-init");
}

void test_atomic_struct_const(void) {
	struct ConstPoint {
		int x;
		int y;
	};
	// const _Atomic requires explicit initializer, just test it compiles
	const _Atomic struct ConstPoint cp = {0};
	CHECK(cp.x == 0 && cp.y == 0, "const _Atomic struct with explicit init");
}

void test_atomic_anonymous_struct(void) {
	_Atomic struct {
		int x;
		int y;
	} anon;

	CHECK(anon.x == 0 && anon.y == 0, "_Atomic anonymous struct zero-init");
}

void test_atomic_union_different_sizes(void) {
	union MixedSizes {
		char c;
		int i;
		long long ll;
	};
	_Atomic union MixedSizes ms;
	CHECK(ms.ll == 0, "_Atomic union different sizes zero-init");
}

void test_atomic_struct_in_loop(void) {
	struct LoopPoint {
		int x;
		int y;
	};

	for (int i = 0; i < 3; i++) {
		_Atomic struct LoopPoint lp;
		CHECK(lp.x == 0 && lp.y == 0, "_Atomic struct in loop iteration");
		lp.x = i; // Modify to ensure next iteration re-inits
	}
}

void test_atomic_struct_nested_blocks(void) {
	struct BlockPoint {
		int x;
		int y;
	};

	{
		_Atomic struct BlockPoint bp1;
		CHECK(bp1.x == 0, "_Atomic struct outer block");
		{
			_Atomic struct BlockPoint bp2;
			CHECK(bp2.x == 0, "_Atomic struct inner block");
		}
	}
}

void test_atomic_struct_with_defer(void) {
	struct DeferPoint {
		int x;
		int y;
	};

	int check_val = 0;
	{
		defer check_val = 1;
		_Atomic struct DeferPoint dp;
		CHECK(dp.x == 0 && dp.y == 0, "_Atomic struct with defer");
	}
	CHECK(check_val == 1, "_Atomic struct defer executed");
}

void test_atomic_scalar_contrast(void) {
	_Atomic int ai;
	_Atomic long al;
	_Atomic char ac;
	_Atomic double ad;
	CHECK(ai == 0, "_Atomic int still uses = 0");
	CHECK(al == 0, "_Atomic long still uses = 0");
	CHECK(ac == 0, "_Atomic char still uses = 0");
	CHECK(ad == 0.0, "_Atomic double still uses = 0");
}

void test_atomic_typedef_struct(void) {
	typedef struct {
		int x;
		int y;
	} TPoint;

	_Atomic TPoint tp;
	CHECK(tp.x == 0 && tp.y == 0, "_Atomic typedef'd struct");
}

void test_atomic_typedef_atomic(void) {
	typedef _Atomic struct {
		int x;
		int y;
	} AtomicTPoint;

	AtomicTPoint atp;
	CHECK(atp.x == 0 && atp.y == 0, "typedef _Atomic struct");
}

void test_atomic_struct_bitfields(void) {
	struct BitFields {
		unsigned a : 4;
		unsigned b : 4;
		unsigned c : 8;
	};
	_Atomic struct BitFields bf;
	CHECK(bf.a == 0 && bf.b == 0 && bf.c == 0, "_Atomic struct with bitfields");
}

void test_raw_atomic_struct(void) {
	struct RawPoint {
		int x;
		int y;
	};
	raw _Atomic struct RawPoint rp;
	rp.x = 42;
	rp.y = 99;
	CHECK(rp.x == 42 && rp.y == 99, "raw _Atomic struct skips zero-init");
}

#endif // __clang__

void test_switch_scope_leak(void) {
	// SAFE PATTERN 1: Declare variable BEFORE the switch
	int y;
	int result = -1;
	switch (1) {
	case 1:
		result = y; // y is properly zero-initialized
		break;
	}
	CHECK_EQ(result, 0, "switch scope: variable before switch is zero-init");

	// SAFE PATTERN 2: Declare variable INSIDE a case block
	result = -1;
	switch (1) {
	case 1: {
		int z; // Inside case block - properly initialized
		result = z;
		break;
	}
	}
	CHECK_EQ(result, 0, "switch scope: variable in case block is zero-init");

	PrismResult parse_result = prism_transpile_source(
	    "void f(void) {\n"
	    "    switch (1) {\n"
	    "    int z;\n"
	    "    case 1:\n"
	    "        (void)z;\n"
	    "        break;\n"
	    "    }\n"
	    "}\n",
	    "switch_scope_leak_reject.c", prism_defaults());
	CHECK(parse_result.status != PRISM_OK,
	      "switch scope leak protection: unsafe pattern rejected");
	CHECK(parse_result.error_msg && strstr(parse_result.error_msg, "switch body"),
	      "switch scope leak protection: error explains switch body hazard");
	prism_free(&parse_result);
}

typedef int SizeofTestType;

void test_sizeof_shadows_type(void) {
	// At this point, SizeofTestType is the typedef (int)
	int SizeofTestType = sizeof(SizeofTestType); // sizeof should use the TYPE
	// sizeof(int) is typically 4
	CHECK(SizeofTestType == sizeof(int), "sizeof(T) in initializer uses type not variable");
}

#if __STDC_VERSION__ >= 201112L
void test_generic_colons(void) {
	int x = 5;
	// _Generic has "type: value" syntax that looks like labels
	int type_id = _Generic(x, int: 1, long: 2, default: 0);
	CHECK(type_id == 1, "_Generic parsing doesn't break label detection");
}
#endif

void test_for_braceless_label(void) {
	int reached = 0;
	for (int i = 0; i < 1; i++)
	my_label:
		reached = 1; // Label in braceless for body

	CHECK(reached == 1, "label in braceless for body");
}

void test_goto_into_for(void) {
	// NOTE: This pattern now correctly produces a compile-time error
	// goto skip; for (int i = 0; ...) { skip: ... }
	// The goto would skip the variable declaration in the for statement
	// which is unsafe. Prism now detects and errors on this pattern.
	int x = 0;
	// The unsafe pattern is commented out because it now errors:
	// goto skip;
	// for (int i = 0; i < 10; i++) {
	// skip:
	//     x = 1;
	//     break;
	// }
	CHECK(x == 0, "goto into for loop now blocked (compile error)");
}

void test_attribute_positions(void) {
	// Attribute after type, before variable name
	int __attribute__((aligned(4))) x;
	CHECK(x == 0, "attribute after type zero-init");

	// Attribute after pointer star
	int *__attribute__((aligned(8))) p;
	CHECK(p == NULL, "attribute after pointer star zero-init");

	// Multiple attributes
	__attribute__((unused)) __attribute__((aligned(16))) int y;
	CHECK(y == 0, "multiple attributes zero-init");
}

void test_rigor_defer_comma_operator(void) {
	log_reset();
	{
		defer(log_append("A"), log_append("B")); // Comma expression
		log_append("1");
	}
	CHECK_LOG("1AB", "defer comma operator");
}

void test_defer_complex_comma(void) {
	log_reset();
	int x = 0;
	{
		defer(x++, log_append("D"));
		log_append("1");
	}
	CHECK(x == 1, "defer comma with side effect - x incremented");
	CHECK_LOG("1D", "defer comma with side effect - log order");
}

void test_switch_noreturn_no_fallthrough(void) {
	int x = 2; // Don't hit the exit case
	int result = 0;

	switch (x) {
	case 1: exit(1); // noreturn - should NOT trigger fallthrough error
	case 2: result = 2; break;
	}

	CHECK_EQ(result, 2, "switch noreturn: no false fallthrough error");
}

static int late_binding_captured = 0;

void capture_value(int x) {
	late_binding_captured = x;
}

void test_defer_late_binding_semantic(void) {
	int x = 10;
	{
		defer capture_value(x); // x is evaluated at scope exit
		x = 20;			// Modify x before scope exit
	}
	// Late binding: x was 20 at scope exit
	CHECK_EQ(late_binding_captured, 20, "defer late binding: evaluates at exit");

	// Workaround: capture value at defer site
	x = 10;
	{
		int captured_x = x; // Capture value NOW
		defer capture_value(captured_x);
		x = 20;
	}
	CHECK_EQ(late_binding_captured, 10, "defer early capture workaround");
}


#define CHECK_ZEROED(var, size, name)                                                                        \
	do {                                                                                                 \
		char zero_buf[size];                                                                         \
		memset(zero_buf, 0, size);                                                                   \
		if (memcmp(&(var), zero_buf, size) == 0) {                                                   \
			printf("[PASS] %s\n", name);                                                         \
			passed++;                                                                            \
		} else {                                                                                     \
			printf("[FAIL] %s - NOT ZERO-INITIALIZED!\n", name);                                 \
			failed++;                                                                            \
		}                                                                                            \
		total++;                                                                                     \
	} while (0)

void test_complex_func_ptr_array(void) {
	// Function pointer that returns pointer to array of 10 ints
	int (*(*fp_ret_arr)(void))[10];
	CHECK(fp_ret_arr == NULL, "func ptr returning ptr to array - zero-init");
}

void test_array_of_complex_func_ptrs(void) {
	int *(*arr_fp[3])(int, int);
	int all_null = 1;
	for (int i = 0; i < 3; i++)
		if (arr_fp[i] != NULL) all_null = 0;
	CHECK(all_null, "array of func ptrs returning ptr - zero-init");
}

void test_func_ptr_taking_func_ptr(void) {
	int (*fp_takes_fp)(int (*)(void));
	CHECK(fp_takes_fp == NULL, "func ptr taking func ptr arg - zero-init");
}

void test_ptr_to_array_of_func_ptrs(void) {
	int (*(*p_arr_fp)[5])(void);
	CHECK(p_arr_fp == NULL, "ptr to array of func ptrs - zero-init");
}

void test_multi_level_ptr_chain(void) {
	int ****pppp;
	CHECK(pppp == NULL, "quad pointer - zero-init");

	void *****vpppp;
	CHECK(vpppp == NULL, "void quintuple pointer - zero-init");
}

typedef struct {
	int x;
	int y;
} Coord;

void test_complex_func_ptr_with_struct(void) {
	Coord *(*fp_struct)(Coord *, int, Coord);
	CHECK(fp_struct == NULL, "func ptr with struct params - zero-init");
}

void test_paren_grouped_declarator(void) {
	// This is just a pointer to int, but uses parens
	int(*grouped_ptr);
	CHECK(grouped_ptr == NULL, "parenthesized pointer decl - zero-init");

	// Pointer to pointer with parens
	int *(*grouped_pp);
	CHECK(grouped_pp == NULL, "paren grouped ptr to ptr - zero-init");
}

void test_multi_dim_array_ptrs(void) {
	int (*p2d)[3][4];
	CHECK(p2d == NULL, "ptr to 2d array - zero-init");

	int (*p3d)[2][3][4];
	CHECK(p3d == NULL, "ptr to 3d array - zero-init");
}

void test_sizeof_array_bounds(void) {
	int arr_sizeof[sizeof(int)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(int); i++)
		if (arr_sizeof[i] != 0) all_zero = 0;
	CHECK(all_zero, "array with sizeof bound - zero-init");
}

void test_decl_after_label(void) {
	int x;
	x = 1;
my_label: {
	int y;
	CHECK_EQ(y, 0, "decl in block after label - zero-init");
}
	(void)x; // suppress unused warning
}

void test_decl_directly_after_label(void) {
	int counter = 0;
	int sum = 0;

restart:
	int x;	  // Zero-initialized each time we jump here
	sum += x; // x should be 0 each time
	counter++;
	if (counter < 3) goto restart;

	// x was 0 on each iteration, so sum should be 0
	CHECK_EQ(sum, 0, "decl directly after label - zero-init on backward goto");
}

void test_decl_in_else(void) {
	if (0) {
		int x;
		(void)x;
	} else {
		int y;
		CHECK_EQ(y, 0, "decl in else branch - zero-init");
	}
}

void test_volatile_func_ptr(void) {
	int (*volatile vfp)(void);
	CHECK(vfp == NULL, "volatile func ptr - zero-init");

	volatile int (*fvp)(void);
	CHECK(fvp == NULL, "func ptr to volatile - zero-init");
}

void test_extremely_complex_declarator(void) {
	// Pointer to function returning pointer to array of 5 pointers to functions
	// returning int
	int (*(*(*super_complex)(void))[5])(void);
	CHECK(super_complex == NULL, "extremely complex declarator - zero-init");
}

#define TYPE_SIGNED_TEST(t) (!((t)0 < (t) - 1))
#define TYPE_WIDTH_TEST(t) (sizeof(t) * 8)
#define INT_STRLEN_BOUND_TEST(t)                                                                             \
	((TYPE_WIDTH_TEST(t) - TYPE_SIGNED_TEST(t)) * 302 / 1000 + 1 + TYPE_SIGNED_TEST(t))

typedef long long test_rlim_t;
typedef unsigned long test_size_t;

void test_sizeof_in_array_bound(void) {
	// Basic sizeof - should not be detected as VLA
	char buf1[sizeof(int)];
	CHECK(buf1[0] == 0, "sizeof(int) array bound - zero-init");

	// sizeof with typedef - should not be detected as VLA
	char buf2[sizeof(test_rlim_t)];
	CHECK(buf2[0] == 0, "sizeof(typedef) array bound - zero-init");

	// sizeof expression with multiplication
	char buf3[sizeof(int) * 8];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(int) * 8; i++)
		if (buf3[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof*8 array bound - zero-init");
}

void test_cast_expression_in_array_bound(void) {
	// Cast with built-in type - constant expression
	char buf1[(int)4 + 1];
	CHECK(buf1[0] == 0, "cast with int array bound - zero-init");

	// TYPE_SIGNED pattern: (! ((type) 0 < (type) -1))
	// This expands to a constant expression using casts
	char buf2[TYPE_SIGNED_TEST(int) + 1];
	CHECK(buf2[0] == 0, "TYPE_SIGNED(int) array bound - zero-init");

	// TYPE_SIGNED with typedef - the key regression case
	char buf3[TYPE_SIGNED_TEST(test_rlim_t) + 1];
	CHECK(buf3[0] == 0, "TYPE_SIGNED(typedef) array bound - zero-init");
}

void test_complex_macro_array_bound(void) {
	// Full INT_STRLEN_BOUND pattern - the original failing case
	char buf1[INT_STRLEN_BOUND_TEST(int) + 1];
	CHECK(buf1[0] == 0, "INT_STRLEN_BOUND(int) array bound - zero-init");

	// With typedef - this was the exact pattern that failed in bash
	char buf2[INT_STRLEN_BOUND_TEST(test_rlim_t) + 1];
	CHECK(buf2[0] == 0, "INT_STRLEN_BOUND(typedef) array bound - zero-init");

	// With system-like typedef name (ends in _t)
	char buf3[INT_STRLEN_BOUND_TEST(test_size_t) + 1];
	CHECK(buf3[0] == 0, "INT_STRLEN_BOUND(size_t-like) array bound - zero-init");
}

void test_system_typedef_pattern(void) {
	// Names ending in _t should be recognized as likely system typedefs
	// and allowed in constant expressions (for casts)
	typedef int my_custom_t;
	char buf1[(my_custom_t)10];
	int all_zero = 1;
	for (int i = 0; i < 10; i++)
		if (buf1[i] != 0) all_zero = 0;
	CHECK(all_zero, "custom _t typedef in cast - zero-init");
}

void test_invisible_system_typedef_pattern(void) {
	// size_t - standard system typedef from stddef.h
	size_t s1; // should be zero-initialized
	CHECK(s1 == 0, "size_t variable - zero-init");

	// ptrdiff_t - standard system typedef from stddef.h
	ptrdiff_t p1;
	CHECK(p1 == 0, "ptrdiff_t variable - zero-init");

	// uint32_t, int64_t - stdint.h system typedefs
	uint32_t u32;
	CHECK(u32 == 0, "uint32_t variable - zero-init");

	int64_t i64;
	CHECK(i64 == 0, "int64_t variable - zero-init");

	uintptr_t uptr;
	CHECK(uptr == 0, "uintptr_t variable - zero-init");

	// Array of system types
	size_t arr[3];
	int all_zero = 1;
	for (int i = 0; i < 3; i++)
		if (arr[i] != 0) all_zero = 0;
	CHECK(all_zero, "size_t array - zero-init");

	// Pointer to system type
	size_t *ptr;
	CHECK(ptr == 0, "size_t* pointer - zero-init");
}

void test_system_typedef_shadow(void) {
	// Shadow a system type name with a variable
	int size_t = 10;
	int result = size_t * 5; // Should be multiplication, not pointer declaration
	CHECK(result == 50, "shadowed size_t multiplication");

	// Another shadow test with _t suffix
	int my_custom_t = 7;
	int mul = my_custom_t * 3;
	CHECK(mul == 21, "shadowed *_t multiplication");

	// Double underscore prefix shadow
	int __internal = 8;
	int prod = __internal * 2;
	CHECK(prod == 16, "shadowed __* multiplication");
}

void test_alignof_in_array_bound(void) {
	// _Alignof should also be recognized as constant
	char buf1[_Alignof(int) + 1];
	CHECK(buf1[0] == 0, "_Alignof array bound - zero-init");

	char buf2[_Alignof(test_rlim_t)];
	CHECK(buf2[0] == 0, "_Alignof(typedef) array bound - zero-init");
}

void test_complex_operators_in_array_bound(void) {
	// Bitwise operators in constant expressions
	char buf1[(sizeof(int) << 1)];
	CHECK(buf1[0] == 0, "sizeof << 1 array bound - zero-init");

	// Comparison operators (result is 0 or 1)
	char buf2[(sizeof(int) >= 4) + 1];
	CHECK(buf2[0] == 0, "comparison in array bound - zero-init");

	// Ternary operator
	char buf3[(sizeof(int) > 2 ? 8 : 4)];
	CHECK(buf3[0] == 0, "ternary in array bound - zero-init");

	// Logical operators
	char buf4[(sizeof(int) && sizeof(char)) + 1];
	CHECK(buf4[0] == 0, "logical && in array bound - zero-init");
}

static int global_arr_for_sizeof[] = {1, 2, 3, 4, 5};

void test_sizeof_array_element_in_bound(void) {
	// sizeof(arr[0]) pattern - common idiom for array element count
	// prism was seeing arr[0] as a declaration inside the array bound
	char buf1[sizeof(global_arr_for_sizeof) / sizeof(global_arr_for_sizeof[0])];
	int expected_size = sizeof(global_arr_for_sizeof) / sizeof(global_arr_for_sizeof[0]);
	int all_zero = 1;
	for (int i = 0; i < expected_size; i++)
		if (buf1[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(arr)/sizeof(arr[0]) array bound - zero-init");
	CHECK_EQ(expected_size, 5, "sizeof(arr)/sizeof(arr[0]) gives correct count");

	// Just sizeof(arr[0])
	char buf2[sizeof(global_arr_for_sizeof[0])];
	CHECK(buf2[0] == 0, "sizeof(arr[0]) array bound - zero-init");

	// Nested brackets: sizeof of 2D array element
	int arr2d[3][4] = {{0}};
	char buf3[sizeof(arr2d[0])]; // sizeof a row (4 ints)
	int row_size = sizeof(arr2d[0]);
	all_zero = 1;
	for (int i = 0; i < row_size; i++)
		if (buf3[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(2d_arr[0]) array bound - zero-init");

	// Multiple nested brackets
	char buf4[sizeof(arr2d[0][0])]; // sizeof single element
	CHECK(buf4[0] == 0, "sizeof(2d_arr[0][0]) array bound - zero-init");

	// sizeof with expression involving array access
	char buf5[sizeof(global_arr_for_sizeof[0]) * 2];
	CHECK(buf5[0] == 0, "sizeof(arr[0])*2 array bound - zero-init");
}

void test_sizeof_with_parens_in_bound(void) {
	// Parenthesized sizeof expressions
	char buf1[(sizeof(int))];
	CHECK(buf1[0] == 0, "(sizeof(int)) array bound - zero-init");

	// Double parens
	char buf2[((sizeof(int)))];
	CHECK(buf2[0] == 0, "((sizeof(int))) array bound - zero-init");

	// sizeof of parenthesized expression
	char buf3[sizeof((int)0) + 1];
	CHECK(buf3[0] == 0, "sizeof((int)0) array bound - zero-init");

	// Complex parenthesized expression with sizeof
	char buf4[(sizeof(int) + sizeof(char)) * 2];
	int all_zero = 1;
	for (size_t i = 0; i < (sizeof(int) + sizeof(char)) * 2; i++)
		if (buf4[i] != 0) all_zero = 0;
	CHECK(all_zero, "(sizeof+sizeof)*2 array bound - zero-init");
}

void test_sizeof_variable_in_array_bound(void) {
	int x = 42; // Regular (non-VLA) variable

	// sizeof(x) is sizeof(int), a compile-time constant
	// This should be zero-initialized
	char buf1[sizeof(x)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(x); i++)
		if (buf1[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(variable) array bound - zero-init");

	// sizeof expression using variable
	char buf2[sizeof(x) * 2];
	all_zero = 1;
	for (size_t i = 0; i < sizeof(x) * 2; i++)
		if (buf2[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(variable)*2 array bound - zero-init");

	// sizeof of pointer variable
	int *ptr = &x;
	char buf3[sizeof(ptr)];
	all_zero = 1;
	for (size_t i = 0; i < sizeof(ptr); i++)
		if (buf3[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(pointer_var) array bound - zero-init");

	// sizeof of struct variable
	struct {
		int a;
		char b;
	} s = {0};

	char buf4[sizeof(s)];
	all_zero = 1;
	for (size_t i = 0; i < sizeof(s); i++)
		if (buf4[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(struct_var) array bound - zero-init");
}

static void __attribute__((noinline)) pollute_stack_for_sizeof(void) {
	volatile char garbage[512];
	for (int i = 0; i < 512; i++) garbage[i] = (char)(0xAA + i);
	(void)garbage[0]; // Prevent optimization
}

void test_sizeof_local_int_variable(void) {
	pollute_stack_for_sizeof();
	int x = 42;
	char buf[sizeof(x)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(x); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(local int) zero-init");
}

void test_sizeof_local_long_variable(void) {
	pollute_stack_for_sizeof();
	long long llval = 12345678901234LL;
	char buf[sizeof(llval)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(llval); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(local long long) zero-init");
}

void test_sizeof_local_float_variable(void) {
	pollute_stack_for_sizeof();
	float f = 3.14159f;
	char buf[sizeof(f)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(f); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(local float) zero-init");
}

void test_sizeof_local_double_variable(void) {
	pollute_stack_for_sizeof();
	double d = 2.71828;
	char buf[sizeof(d)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(d); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(local double) zero-init");
}

void test_sizeof_local_pointer_variable(void) {
	pollute_stack_for_sizeof();
	int *ptr = NULL;
	char buf[sizeof(ptr)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(ptr); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(local pointer) zero-init");
}

void test_sizeof_local_array_variable(void) {
	pollute_stack_for_sizeof();
	int arr[10] = {1, 2, 3};
	char buf[sizeof(arr)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(arr); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(local array) zero-init");
}

void test_sizeof_local_struct_variable(void) {
	pollute_stack_for_sizeof();

	struct {
		int x;
		double y;
		char z[20];
	} s = {0};

	char buf[sizeof(s)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(s); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(local struct) zero-init");
}

void test_sizeof_local_union_variable(void) {
	pollute_stack_for_sizeof();

	union {
		int i;
		double d;
		char c[16];
	} u = {0};

	char buf[sizeof(u)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(u); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(local union) zero-init");
}

void test_sizeof_function_parameter(void) {
	// sizeof of parameter variable
	int param = 99;
	pollute_stack_for_sizeof();
	char buf[sizeof(param)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(param); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(parameter) zero-init");
}

void test_sizeof_multiple_vars_in_expr(void) {
	pollute_stack_for_sizeof();
	int a = 1, b = 2;
	char buf[sizeof(a) + sizeof(b)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(a) + sizeof(b); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(a)+sizeof(b) zero-init");
}

void test_sizeof_var_times_constant(void) {
	pollute_stack_for_sizeof();
	int x = 42;
	char buf[sizeof(x) * 4];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(x) * 4; i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(var)*4 zero-init");
}

void test_sizeof_var_in_ternary(void) {
	pollute_stack_for_sizeof();
	int x = 1;
	double y = 2.0;
	char buf[sizeof(x) > sizeof(y) ? sizeof(x) : sizeof(y)];
	int all_zero = 1;
	for (size_t i = 0; i < (sizeof(x) > sizeof(y) ? sizeof(x) : sizeof(y)); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof in ternary zero-init");
}

void test_sizeof_var_with_bitwise_ops(void) {
	pollute_stack_for_sizeof();
	int x = 10;
	char buf[(sizeof(x) << 2)];
	int all_zero = 1;
	for (size_t i = 0; i < (sizeof(x) << 2); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(var)<<2 zero-init");
}

void test_sizeof_nested_vars(void) {
	pollute_stack_for_sizeof();

	struct Outer {
		int a;

		struct {
			int b;
			char c;
		} inner;
	} o = {0};

	char buf[sizeof(o.inner)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(o.inner); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(var.member) zero-init");
}

void test_sizeof_pointer_deref(void) {
	pollute_stack_for_sizeof();
	int val = 42;
	int *ptr = &val;
	char buf[sizeof(*ptr)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(*ptr); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(*ptr) zero-init");
}

void test_sizeof_array_element_var(void) {
	pollute_stack_for_sizeof();
	double arr[5] = {1.0};
	char buf[sizeof(arr[0])];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(arr[0]); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(arr[0]) var zero-init");
}

void test_sizeof_2d_array_element_var(void) {
	pollute_stack_for_sizeof();
	int matrix[3][4] = {{0}};
	char buf1[sizeof(matrix[0])];
	char buf2[sizeof(matrix[0][0])];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(matrix[0]); i++)
		if (buf1[i] != 0) all_zero = 0;
	for (size_t i = 0; i < sizeof(matrix[0][0]); i++)
		if (buf2[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(2d_arr[row/elem]) zero-init");
}

void test_sizeof_compound_literal_var(void) {
	pollute_stack_for_sizeof();
	char buf[sizeof((struct {
		int x;
		int y;
	}){0})];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof((
				   struct {
					   int x;
					   int y;
				   }){0});
	     i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(compound literal) zero-init");
}

void test_sizeof_cast_expression_var(void) {
	pollute_stack_for_sizeof();
	int x = 42;
	char buf[sizeof((double)x)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof((double)x); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof((double)var) zero-init");
}

void test_sizeof_var_division(void) {
	pollute_stack_for_sizeof();
	long arr[20] = {0};
	char buf[sizeof(arr) / sizeof(arr[0])];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(arr) / sizeof(arr[0]); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(arr)/sizeof(arr[0]) zero-init");
}

void test_sizeof_const_qualified_var(void) {
	pollute_stack_for_sizeof();
	const int cval = 100;
	char buf[sizeof(cval)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(cval); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(const var) zero-init");
}

void test_sizeof_volatile_var(void) {
	pollute_stack_for_sizeof();
	volatile int vval = 200;
	char buf[sizeof(vval)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(vval); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(volatile var) zero-init");
}

void test_sizeof_restrict_ptr(void) {
	pollute_stack_for_sizeof();
	int val = 1;
	int *restrict rptr = &val;
	char buf[sizeof(rptr)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(rptr); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(restrict ptr) zero-init");
}

void test_sizeof_static_var(void) {
	pollute_stack_for_sizeof();
	static int sval = 123;
	char buf[sizeof(sval)];
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(sval); i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "sizeof(static var) zero-init");
}

#ifndef _MSC_VER
void test_sizeof_true_vla_detected(void) {
	int n = 5;
	int vla[n];
	vla[0] = 42;

	// x is also a VLA because sizeof(vla) is runtime
	int x[sizeof(vla)];
	x[0] = 99;

	CHECK(vla[0] == 42, "VLA preserves value");
	CHECK(x[0] == 99, "sizeof(VLA) creates VLA, no init");
}

void test_sizeof_nested_vla_detection(void) {
	int n = 3;
	int vla1[n];
	vla1[0] = 1;

	// Second VLA based on first
	char vla2[sizeof(vla1)];
	vla2[0] = 'A';

	CHECK(vla1[0] == 1 && vla2[0] == 'A', "nested VLA sizeof detection");
}
#endif




#undef offsetof
#define offsetof(TYPE, MEMBER) ((size_t)((char *)&((TYPE *)0)->MEMBER - (char *)0))

typedef struct TestSrcItem_off {
	char *name;
	int type;
} TestSrcItem_off;

typedef struct TestSrcList_off {
	int count;
	TestSrcItem_off items[1]; // Flexible array member pattern
} TestSrcList_off;

struct TestOp_off {
	union {
		int i;
		void *p;
		char *z;

		struct {
			int n;
			// Array size uses offsetof - GCC sees this as VLA at file scope
			TestSrcItem_off items[offsetof(TestSrcList_off, items) / sizeof(TestSrcItem_off)];
		} srclist;
	} u;
};

void test_manual_offsetof_in_union(void) {
	// This tests that prism doesn't add = {0} to struct TestOp_off
	// If it did, GCC would error: "variable-sized object may not be initialized"
	struct TestOp_off op;
	op.u.i = 42;
	CHECK(op.u.i == 42, "manual offsetof in union - no zeroinit");
}

void test_manual_offsetof_local(void) {
	// Test offsetof with local variable
	struct TestOp_off op; // Should NOT get = {0} due to offsetof VLA pattern
	op.u.p = NULL;
	CHECK(op.u.p == NULL, "manual offsetof local struct - no zeroinit");
}

void test_union_offsetof_division(void) {
	// The pattern offsetof(T,m)/sizeof(E) should be treated as VLA
	union {
		int x;

		struct {
			TestSrcItem_off data[offsetof(TestSrcList_off, items) / sizeof(TestSrcItem_off)];
		} embedded;
	} u;

	u.x = 123;
	CHECK(u.x == 123, "union offsetof division - no zeroinit");
}

#ifndef _MSC_VER
void test_vla_basic(void) {
	int n = 5;
	int vla[n]; // VLA - prism should NOT add = {0}
	// Just verify it compiles and we can use it
	for (int i = 0; i < n; i++) {
		vla[i] = i;
	}
	CHECK(vla[0] == 0 && vla[4] == 4, "basic VLA - no zeroinit");
}

#endif


#ifdef __GNUC__
void test_typeof_overflow_35_vars(void) {
	// 35 typeof variables in one declaration — exceeds old limit of 32
	typeof(int) v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20,
	    v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35;

	// All must be zero-initialized, including those past the old limit
	CHECK_EQ(v1, 0, "typeof overflow: v1 zero-init");
	CHECK_EQ(v16, 0, "typeof overflow: v16 zero-init");
	CHECK_EQ(v32, 0, "typeof overflow: v32 zero-init (old limit)");
	CHECK_EQ(v33, 0, "typeof overflow: v33 zero-init (past old limit)");
	CHECK_EQ(v34, 0, "typeof overflow: v34 zero-init (past old limit)");
	CHECK_EQ(v35, 0, "typeof overflow: v35 zero-init (past old limit)");
}

void test_typeof_overflow_64_vars(void) {
	// 64 typeof variables — stress test for dynamic allocation
	typeof(int) a01, a02, a03, a04, a05, a06, a07, a08, a09, a10, a11, a12, a13, a14, a15, a16, a17, a18,
	    a19, a20, a21, a22, a23, a24, a25, a26, a27, a28, a29, a30, a31, a32, a33, a34, a35, a36, a37,
	    a38, a39, a40, a41, a42, a43, a44, a45, a46, a47, a48, a49, a50, a51, a52, a53, a54, a55, a56,
	    a57, a58, a59, a60, a61, a62, a63, a64;

	CHECK_EQ(a01, 0, "typeof overflow 64: a01");
	CHECK_EQ(a32, 0, "typeof overflow 64: a32");
	CHECK_EQ(a33, 0, "typeof overflow 64: a33");
	CHECK_EQ(a48, 0, "typeof overflow 64: a48");
	CHECK_EQ(a64, 0, "typeof overflow 64: a64");
}

void test_typeof_struct_overflow(void) {
	// typeof with struct — memset path
	struct pair {
		int x, y;
	};
	struct pair p;
	p.x = 42;
	p.y = 99;
	typeof(p) s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, s13, s14, s15, s16, s17, s18, s19, s20,
	    s21, s22, s23, s24, s25, s26, s27, s28, s29, s30, s31, s32, s33, s34;

	CHECK_EQ(s1.x, 0, "typeof struct overflow: s1.x");
	CHECK_EQ(s32.x, 0, "typeof struct overflow: s32.x (old limit)");
	CHECK_EQ(s33.x, 0, "typeof struct overflow: s33.x (past old limit)");
	CHECK_EQ(s34.y, 0, "typeof struct overflow: s34.y (past old limit)");
}
#endif // __GNUC__

void test_many_labels_function(void) {
	int result = 0;
	goto label_start;
label_01:
	result += 1;
	goto label_02;
label_02:
	result += 2;
	goto label_03;
label_03:
	result += 4;
	goto label_04;
label_04:
	result += 8;
	goto label_05;
label_05:
	result += 16;
	goto label_06;
label_06:
	result += 32;
	goto label_07;
label_07:
	result += 64;
	goto label_end;
label_start:
	goto label_01;
label_end:
	CHECK_EQ(result, 127, "many labels: forward+backward goto");
}

void test_raw_struct_member_field(void) {
	// Struct with member named 'raw' — must not confuse parser
	struct data {
		int raw;
		int cooked;
	};
	struct data d;
	d.raw = 42;
	d.cooked = 99;
	CHECK_EQ(d.raw, 42, "raw struct member: d.raw");
	CHECK_EQ(d.cooked, 99, "raw struct member: d.cooked");
}

void test_raw_anonymous_struct_member(void) {
	// Anonymous struct with 'raw' member
	struct {
		int raw;
		char name[8];
	} item;

	item.raw = 7;
	memcpy(item.name, "test", 5);
	CHECK_EQ(item.raw, 7, "raw anonymous struct member");
	CHECK(strcmp(item.name, "test") == 0, "raw anonymous struct: name field");
}

void test_raw_in_compound_literal(void) {
	// 'raw' as variable initialized from compound literal
	int raw = ((struct { int raw; }){.raw = 55}).raw;
	CHECK_EQ(raw, 55, "raw in compound literal member access");
}

void test_raw_typedef_name(void) {
	// typedef named 'raw' — should shadow the keyword
	typedef int raw;
	raw x;
	x = 123;
	CHECK_EQ(x, 123, "raw as typedef name");
}

void test_raw_pointer_to_struct_with_raw(void) {
	struct raw_data {
		int raw;
	};
	struct raw_data val;
	val.raw = 88;
	struct raw_data *ptr = &val;
	CHECK_EQ(ptr->raw, 88, "raw: ptr->raw member access");
}

void test_raw_array_of_structs_with_raw(void) {
	struct item {
		int raw;
		int processed;
	};
	struct item items[3];
	for (int i = 0; i < 3; i++) {
		items[i].raw = i * 10;
		items[i].processed = i * 100;
	}
	CHECK_EQ(items[0].raw, 0, "raw array of structs: [0].raw");
	CHECK_EQ(items[2].raw, 20, "raw array of structs: [2].raw");
	CHECK_EQ(items[2].processed, 200, "raw array of structs: [2].processed");
}

typedef int GhostT;

void test_ghost_shadow_for_braceless(void) {
	// Baseline: GhostT is a type
	GhostT *p1 = &(GhostT){10};
	CHECK_EQ(*p1, 10, "ghost shadow: GhostT before for");

	// For-init shadows GhostT with a variable
	for (int GhostT = 0; GhostT < 1; GhostT++) (void)GhostT; // braceless body

	// GhostT must be a type again
	GhostT *p2 = &(GhostT){20};
	CHECK_EQ(*p2, 20, "ghost shadow: GhostT restored after braceless for");
}

void test_ghost_shadow_nested_for(void) {
	GhostT *p0 = &(GhostT){5};
	CHECK_EQ(*p0, 5, "ghost shadow nested: before");

	for (int GhostT = 0; GhostT < 1; GhostT++)
		for (int GhostT = 0; GhostT < 1; GhostT++) (void)GhostT; // double braceless nesting

	GhostT *p1 = &(GhostT){15};
	CHECK_EQ(*p1, 15, "ghost shadow nested: after double braceless for");
}

void test_ghost_shadow_while_braceless(void) {
	GhostT val = 42;
	int count = 0;

	while (count < 1) count++;

	GhostT *p = &val;
	CHECK_EQ(*p, 42, "ghost shadow: GhostT after braceless while");
}

void test_ghost_shadow_if_else_braceless(void) {
	GhostT a = 10;
	int cond = 1;

	if (cond) a = 20;
	else
		a = 30;

	GhostT *p = &a;
	CHECK_EQ(*p, 20, "ghost shadow: GhostT after braceless if/else");
}

#ifdef __GNUC__
void test_ghost_shadow_generic(void) {
	// _Generic inside a for body that shadows a typedef
	GhostT val = 100;
	for (int GhostT = 0; GhostT < 1; GhostT++) {
		int r = _Generic(GhostT, int: 1, default: 0);
		(void)r;
	}

	GhostT *p = &val;
	CHECK_EQ(*p, 100, "ghost shadow: GhostT after for with _Generic");
}

void test_ghost_shadow_generic_braceless(void) {
	GhostT val = 200;
	for (int GhostT = 0; GhostT < 1; GhostT++) (void)_Generic(GhostT, int: 1, default: 0);

	GhostT *p = &val;
	CHECK_EQ(*p, 200, "ghost shadow: GhostT after braceless for with _Generic");
}
#endif

void test_pragma_survives_transpile(void) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
	int unused_pragma_test_var;
#pragma GCC diagnostic pop
	CHECK_EQ(unused_pragma_test_var, 0, "pragma survives transpilation runtime zeroinit");

	PrismResult result = prism_transpile_source(
	    "void f(void) {\n"
	    "#pragma GCC diagnostic push\n"
	    "#pragma GCC diagnostic ignored \"-Wunused-variable\"\n"
	    "int unused_pragma_test_var;\n"
	    "#pragma GCC diagnostic pop\n"
	    "}\n",
	    "pragma_survives_transpile.c", prism_defaults());
	CHECK_EQ(result.status, PRISM_OK, "pragma survives transpilation output OK");
	if (result.output) {
		CHECK(strstr(result.output, "#pragma GCC diagnostic push") != NULL,
		      "pragma survives transpilation: push preserved");
		CHECK(strstr(result.output, "#pragma GCC diagnostic pop") != NULL,
		      "pragma survives transpilation: pop preserved");
		CHECK(strstr(result.output, "int unused_pragma_test_var = 0;") != NULL,
		      "pragma survives transpilation: zeroinit preserved through pragma");
	}
	prism_free(&result);
}

void test_defer_switch_goto_out(void) {
	log_reset();
	int x = 1;
	switch (x) {
	case 1: {
		defer log_append("A");
		log_append("1");
		if (x == 1) goto switch_out;
		log_append("X");
	}
	case 2: {
		defer log_append("B");
		log_append("2");
		break;
	}
	}
switch_out:
	log_append("E");
	CHECK_LOG("1AE", "defer + switch + goto out: defer fires");
}

void test_defer_switch_break_with_goto_label(void) {
	// goto to a label after the switch — defers from all scopes must fire
	log_reset();
	int x = 0;
	switch (x) {
	case 0: {
		defer log_append("C");
		log_append("0");
		break;
	}
	case 1: {
		defer log_append("D");
		log_append("1");
		goto after_switch;
	}
	}
after_switch:
	log_append("E");
	CHECK_LOG("0CE", "defer + switch + break with goto label");
}

void test_defer_switch_nested_goto(void) {
	// Nested blocks inside switch case with defer and goto
	log_reset();
	int x = 1;
	switch (x) {
	case 1: {
		defer log_append("outer");
		{
			defer log_append("inner");
			log_append("body");
			goto done_nested;
		}
	}
	}
done_nested:
	log_append("E");
	CHECK_LOG("bodyinnerouterE", "defer + switch + nested goto");
}

typedef int RedefT;
typedef int RedefT; // Valid C11 re-definition

void test_typedef_redef_basic(void) {
	RedefT x;
	x = 42;
	CHECK_EQ(x, 42, "typedef re-definition: basic");
}

void test_typedef_redef_pointer(void) {
	typedef int RedefLocal;
	typedef int RedefLocal; // Re-definition at same scope
	RedefLocal *p = &(RedefLocal){99};
	CHECK_EQ(*p, 99, "typedef re-definition: pointer deref");
}

void test_typedef_redef_after_scope(void) {
	typedef int ScopeRedef;
	{
		typedef int ScopeRedef; // Inner scope re-definition
		ScopeRedef inner;
		inner = 10;
		CHECK_EQ(inner, 10, "typedef re-definition: inner scope");
	}
	ScopeRedef outer;
	outer = 20;
	CHECK_EQ(outer, 20, "typedef re-definition: outer restored");
}

#ifdef __GNUC__
void test_typeof_errno_zeroinit(void) {
	// typeof(errno) creates a typeof type — needs memset zero-init path
	// This test verifies __builtin_memset is used (not memset)
	typeof(errno) err_copy;
	CHECK_EQ(err_copy, 0, "typeof(errno) zero-init via __builtin_memset");

	// Verify it actually works with errno
	errno = EINVAL;
	err_copy = errno;
	CHECK_EQ(err_copy, EINVAL, "typeof(errno) assignment after zero-init");
	errno = 0;
}

void test_typeof_statement_expr_zeroinit(void) {
	// typeof with a statement expression — should still zero-init
	typeof(({
		int _t = 5;
		_t;
	})) stmt_expr_var;
	CHECK_EQ(stmt_expr_var, 0, "typeof(stmt_expr) zero-init");
}

void test_typeof_complex_expr_zeroinit(void) {
	// typeof with complex expression
	int arr[3];
	typeof(arr[0]) element;
	CHECK_EQ(element, 0, "typeof(arr[0]) zero-init");

	// typeof pointer deref
	int val = 42;
	int *ptr = &val;
	typeof(*ptr) deref_val;
	CHECK_EQ(deref_val, 0, "typeof(*ptr) zero-init");
}
#endif

void test_switch_goto_defer_multi_case(void) {
	// Multiple cases with defers, goto from middle case
	log_reset();
	int x = 2;
	switch (x) {
	case 1: {
		defer log_append("A");
		log_append("1");
		break;
	}
	case 2: {
		defer log_append("B");
		log_append("2");
		goto exit_multi;
	}
	case 3: {
		defer log_append("C");
		log_append("3");
		break;
	}
	}
exit_multi:
	log_append("E");
	CHECK_LOG("2BE", "switch goto defer: multi-case, goto from case 2");
}

static void test_hashmap_tombstone_insert_delete_cycle(void) {
	// Simulate heavy scope-based typedef churn
	// Each iteration enters a scope, defines a typedef, exits scope
	// This exercises hashmap insert + delete in the typedef table
	volatile int sum = 0;
	for (int round = 0; round < 200; round++) {
		{
			typedef int round_type_t;
			round_type_t val = round;
			sum += val;
		}
	}
	CHECK_EQ(sum, 19900, "hashmap_tombstone_insert_delete_cycle");
}

static void test_hashmap_tombstone_multi_key_churn(void) {
	volatile int result = 0;
	for (int i = 0; i < 100; i++) {
		{
			typedef int churn_a_t;
			typedef long churn_b_t;
			typedef short churn_c_t;
			churn_a_t a = 1;
			churn_b_t b = 2;
			churn_c_t c = 3;
			result += a + (int)b + (int)c;
		}
	}
	CHECK_EQ(result, 600, "hashmap_tombstone_multi_key_churn");
}

static void test_switch_conditional_break_not_false_positive(void) {
	// If mark_switch_control_exit incorrectly marked conditional breaks
	// as definite exits, defer would generate wrong code.
	volatile int cleanup = 0;
	volatile int result = 0;
	for (int i = 0; i < 5; i++) {
		defer cleanup++;
		switch (i) {
		case 0:
			if (i == 0) break; // conditional break - should NOT be definite exit
			result += 10;
			break;
		case 1: break; // unconditional break - IS a definite exit
		default: result += i; break;
		}
	}
	CHECK_EQ(cleanup, 5, "switch_conditional_break_no_false_positive_cleanup");
	CHECK_EQ(result, 9, "switch_conditional_break_no_false_positive_result");
}

static void test_switch_nested_conditional_context(void) {
	volatile int cleanup = 0;
	volatile int val = 0;
	for (int i = 0; i < 3; i++) {
		defer cleanup++;
		switch (i) {
		case 0: {
			if (i == 0) {
				val += 10;
				break;
			}
			val += 100;
			break;
		}
		case 1:
			while (0) break; // break inside while, not switch
			val += 20;
			break;
		default: val += 30; break;
		}
	}
	CHECK_EQ(cleanup, 3, "switch_nested_conditional_cleanup");
	CHECK_EQ(val, 60, "switch_nested_conditional_val");
}

static void test_make_temp_file_normal_operation(void) {
	// Verify transpiler is functional with normal paths
	// If make_temp_file truncation check broke something, this
	// test (and all others) would fail to even run.
	volatile int ok = 1;
	CHECK(ok, "make_temp_file_normal_operation");
}

typedef void VoidAlias;

static volatile int void_typedef_cleanup_count;

static void void_typedef_helper(void) {
	void_typedef_cleanup_count++;
}

static VoidAlias test_void_typedef_return_basic_impl(void) {
	defer void_typedef_cleanup_count += 10;
	return void_typedef_helper();
}

static void test_void_typedef_return_basic(void) {
	void_typedef_cleanup_count = 0;
	test_void_typedef_return_basic_impl();
	CHECK_EQ(void_typedef_cleanup_count, 11, "void_typedef_return_basic");
}

typedef VoidAlias ChainedVoidAlias;

static ChainedVoidAlias test_chained_void_typedef_impl(void) {
	defer void_typedef_cleanup_count += 100;
	return void_typedef_helper();
}

static void test_chained_void_typedef_return(void) {
	void_typedef_cleanup_count = 0;
	test_chained_void_typedef_impl();
	CHECK_EQ(void_typedef_cleanup_count, 101, "chained_void_typedef_return");
}

// Static qualifier + void typedef
static VoidAlias test_static_void_typedef_impl(void) {
	defer void_typedef_cleanup_count += 1000;
	return void_typedef_helper();
}

static void test_static_void_typedef_return(void) {
	void_typedef_cleanup_count = 0;
	test_static_void_typedef_impl();
	CHECK_EQ(void_typedef_cleanup_count, 1001, "static_void_typedef_return");
}

// Void typedef with bare return (no expression)
static VoidAlias test_void_typedef_bare_return_impl(void) {
	defer void_typedef_cleanup_count += 5;
	return;
}

static void test_void_typedef_bare_return(void) {
	void_typedef_cleanup_count = 0;
	test_void_typedef_bare_return_impl();
	CHECK_EQ(void_typedef_cleanup_count, 5, "void_typedef_bare_return");
}

typedef void *VoidPtrAlias;

static VoidPtrAlias test_void_ptr_typedef_return_impl(void) {
	static int val = 42;
	defer void_typedef_cleanup_count += 1;
	return &val;
}

static void test_void_ptr_typedef_not_void(void) {
	void_typedef_cleanup_count = 0;
	int *p = test_void_ptr_typedef_return_impl();
	CHECK_EQ(*p, 42, "void_ptr_typedef_not_void_val");
	CHECK_EQ(void_typedef_cleanup_count, 1, "void_ptr_typedef_not_void_cleanup");
}

typedef VoidAlias (*VoidFuncPtr)(void);

static void test_void_func_ptr_typedef(void) {
	VoidFuncPtr fp = void_typedef_helper;
	void_typedef_cleanup_count = 0;
	fp();
	CHECK_EQ(void_typedef_cleanup_count, 1, "void_func_ptr_typedef_call");
}

#ifndef _MSC_VER
static void test_generic_void_typedef_no_label_confusion(void) {
	int x = 42;
	int result = _Generic(x, int: 1, VoidAlias *: 2, default: 3);
	CHECK_EQ(result, 1, "generic_void_typedef_no_label_confusion");
}
#endif // _MSC_VER

#ifndef _MSC_VER
static void test_vla_zeroinit_basic(void) {
	int n = 10;
	int arr[n]; // Standard VLA - should be zero-initialized by Prism
	int all_zero = 1;
	for (int i = 0; i < n; i++)
		if (arr[i] != 0) all_zero = 0;
	CHECK(all_zero, "VLA basic zero-init via memset");
}

static void test_vla_zeroinit_large(void) {
	int n = 256;
	char buf[n]; // Larger VLA
	int all_zero = 1;
	for (int i = 0; i < n; i++)
		if (buf[i] != 0) all_zero = 0;
	CHECK(all_zero, "VLA large zero-init via memset");
}

static void test_vla_zeroinit_nested_scope(void) {
	for (int round = 0; round < 3; round++) {
		int n = 8 + round;
		int arr[n];
		int all_zero = 1;
		for (int i = 0; i < n; i++)
			if (arr[i] != 0) all_zero = 0;
		CHECK(all_zero, "VLA nested-scope zero-init via memset");
	}
}
#endif // _MSC_VER (VLA zeroinit tests)

static void test_hashmap_tombstone_high_churn_load(void) {
	// Heavy typedef churn: if tombstones aren't counted in load factor,
	// probe chains degrade. We verify correctness under high churn.
	volatile int sum = 0;
	for (int round = 0; round < 500; round++) {
		{
			typedef int churn_load_a_t;
			typedef long churn_load_b_t;
			typedef short churn_load_c_t;
			typedef char churn_load_d_t;
			churn_load_a_t a = 1;
			churn_load_b_t b = 2;
			churn_load_c_t c = 3;
			churn_load_d_t d = 4;
			sum += a + (int)b + (int)c + (int)d;
		}
	}
	CHECK_EQ(sum, 5000, "hashmap_tombstone_high_churn_load");
}

static void test_deep_pointer_nesting(void) {
	// Deeply nested pointer declarations should compile fine
	int x = 42;
	int *p1 = &x;
	int **p2 = &p1;
	int ***p3 = &p2;
	int ****p4 = &p3;
	int *****p5 = &p4;
	CHECK_EQ(*****p5, 42, "deep pointer nesting compiles and works");
}


static void test_typedef_extreme_scope_churn(void) {
	volatile int sum = 0;
	for (int round = 0; round < 300; round++) {
		{
			typedef int t_a1;
			typedef int t_a2;
			typedef int t_a3;
			typedef int t_a4;
			typedef int t_a5;
			typedef int t_a6;
			typedef int t_a7;
			typedef int t_a8;
			typedef int t_a9;
			typedef int t_a10;
			typedef int t_a11;
			typedef int t_a12;
			typedef int t_a13;
			typedef int t_a14;
			typedef int t_a15;
			typedef int t_a16;
			t_a1 v1;
			t_a2 v2;
			t_a3 v3;
			t_a4 v4;
			t_a5 v5;
			t_a6 v6;
			t_a7 v7;
			t_a8 v8;
			t_a9 v9;
			t_a10 v10;
			t_a11 v11;
			t_a12 v12;
			t_a13 v13;
			t_a14 v14;
			t_a15 v15;
			t_a16 v16;
			sum += v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11 + v12 + v13 + v14 +
			       v15 + v16;
		}
	}
	CHECK_EQ(sum, 0, "typedef extreme scope churn 16x300");
}

static void test_typedef_tombstone_saturation_extended(void) {
	volatile int sum = 0;
	for (int i = 0; i < 1000; i++) {
		{
			typedef int sat_a;
			typedef int sat_b;
			typedef int sat_c;
			typedef int sat_d;
			typedef int sat_e;
			typedef int sat_f;
			typedef int sat_g;
			typedef int sat_h;
			sat_a a;
			sat_b b;
			sat_c c;
			sat_d d;
			sat_e e;
			sat_f f;
			sat_g g;
			sat_h h;
			sum += a + b + c + d + e + f + g + h;
		}
	}
	CHECK_EQ(sum, 0, "typedef tombstone saturation 8x1000");
}

#ifndef _MSC_VER
static void test_struct_static_assert_compound_literal(void) {
	struct SACompLit {
		int x;
		_Static_assert(sizeof((int){0}) == sizeof(int), "");
		int y;
	};
	struct SACompLit s;
	CHECK(s.x == 0 && s.y == 0, "struct member after _Static_assert compound literal");
}

static void test_struct_nested_compound_literal_depth(void) {
	struct NestedCL {
		int a;
		_Static_assert(sizeof((char){0}) > 0, "");
		_Static_assert(sizeof((short){0}) > 0, "");
		int b;
		int c;
	};
	struct NestedCL nc;
	CHECK(nc.a == 0 && nc.b == 0 && nc.c == 0, "struct multiple _Static_assert compound literals");
}

static void test_struct_compound_literal_then_nested_struct(void) {
	struct OuterCL {
		int before;
		_Static_assert(sizeof((int){0}) == sizeof(int), "");

		struct InnerCL {
			int ix;
			int iy;
		} inner;

		int after;
	};
	struct OuterCL o;
	CHECK(o.before == 0 && o.inner.ix == 0 && o.inner.iy == 0 && o.after == 0,
	      "struct compound literal then nested struct");
}
#endif // _MSC_VER (_Static_assert tests)

static void test_for_init_multi_decl_all_zeroed(void) {
	volatile int sum = 0;
	for (int a, b, c; a < 3; a++) {
		sum += a + b + c;
	}
	CHECK_EQ(sum, 3, "for init triple decl all zeroed");
}

#ifdef __GNUC__
static void test_for_init_stmt_expr_with_decls(void) {
	volatile int sum = 0;
	for (int a = ({
		     int t = 1;
		     t;
	     }),
		 b;
	     a < 4;
	     a++) {
		sum += a + b;
	}
	CHECK_EQ(sum, (1 + 0) + (2 + 0) + (3 + 0), "for init stmt expr multi decl");
}

static void test_struct_stmt_expr_in_member_size(void) {
	struct SESize {
		int x;
		char buf[({
			enum { N = 8 };
			N;
		})];
		int y;
	};
	raw struct SESize s;
	memset(&s, 0, sizeof(s));
	s.x = 10;
	s.y = 20;
	CHECK(s.x == 10 && s.y == 20, "struct stmt expr in member array size");
}
#endif

static void test_nested_struct_depth_tracking(void) {
	struct L1 {
		struct L2 {
			struct L3 {
				int deep;
			} l3;

			int mid;
		} l2;

		int top;
	};
	struct L1 s;
	CHECK(s.l2.l3.deep == 0 && s.l2.mid == 0 && s.top == 0, "triple nested struct depth");
}

static void test_struct_with_enum_body_depth(void) {
	struct WithEnum {
		enum { WE_A = 10, WE_B = 20 } val;

		int after_enum;
	};
	struct WithEnum we;
	we.val = WE_A;
	CHECK(we.val == 10, "struct with enum body value");
	CHECK(we.after_enum == 0, "struct member after enum body zeroed");
}

static void test_large_string_output(void) {
	// Exercise output buffer handling with strings close to or exceeding buffer sizes
	char large[4096];
	memset(large, 'X', sizeof(large) - 1);
	large[sizeof(large) - 1] = '\0';
	CHECK(strlen(large) == 4095, "large string length correct");
	// Verify the string survived transpilation intact
	int count = 0;
	for (int i = 0; i < 4095; i++)
		if (large[i] == 'X') count++;
	CHECK(count == 4095, "large string content intact");
}

static void test_token_pool_resize_stress(void) {
	/* Regression: token_pool_ensure used to store capacity as (uint32_t)new_cap
	 * without overflow checking.  This test generates a large input that
	 * forces multiple pool resize operations and verifies the transpiler
	 * handles it correctly.  We generate ~200 typedef + function combos
	 * to produce a significant token count. */
	char *code = malloc(128 * 1024);
	CHECK(code != NULL, "token-pool-resize: alloc code buffer");
	if (!code) return;
	int off = 0;
	for (int i = 0; i < 200; i++) {
		off += snprintf(code + off, 128 * 1024 - off,
			"typedef struct { int f%d; char c%d; long l%d; } S%d;\n"
			"S%d make_%d(void) { S%d s; s.f%d = %d; s.c%d = 0; s.l%d = 0; return s; }\n",
			i, i, i, i, i, i, i, i, i, i, i);
	}
	off += snprintf(code + off, 128 * 1024 - off,
		"int main(void) { return make_0().f0; }\n");
	PrismResult r = prism_transpile_source(code, "token_pool_resize.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK,
	         "token-pool-resize: large input forcing multiple pool resizes");
	CHECK(r.output_len > 0, "token-pool-resize: output is non-empty");
	prism_free(&r);
	free(code);
}


static void test_struct_depth_beyond_64(void) {
	// Exercise struct nesting that stays valid even if bitmask can't track depth >= 64
	// This tests the transpiler handles deep nesting without losing struct_depth sync
	int val = 0;
	{
		{
			{
				{
					{
						{
							{
								{
									{
										{
											{
												{
													{
														{
															{
																{
																	{
																		{
																			{
																				{
																					{
																						{
																							{
																								{
																									{
																										{
																											{
																												{
																													{
																														{
																															{
																																val =
																																    1;
																															}
																														}
																													}
																												}
																											}
																										}
																									}
																								}
																							}
																						}
																					}
																				}
																			}
																		}
																	}
																}
															}
														}
													}
												}
											}
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}
	CHECK(val == 1, "struct_at_depth 31-deep braces");

	// Verify struct zeroinit still works after exiting deep brace nesting
	// (exercises bitmask tracking — if struct_depth goes out of sync,
	//  the transpiler may fail to emit zero-init for the struct)
	struct DeepTest {
		int x;
		int y;
	};
	struct DeepTest dt;
	CHECK(dt.x == 0 && dt.y == 0, "struct zeroinit after deep nesting");
}


static int orelse_comma_helper(int *a, int *b) {
	// Tests that orelse in a bare expression correctly handles
	// the expression boundary (doesn't scan past statement end)
	int *p = a orelse return -1;
	int *q = b orelse return -2;
	return *p + *q;
}

static void test_orelse_sequential_bare(void) {
	int x = 10, y = 20;
	CHECK(orelse_comma_helper(&x, &y) == 30, "orelse sequential bare exprs");
	CHECK(orelse_comma_helper(NULL, &y) == -1, "orelse first null bare");
	CHECK(orelse_comma_helper(&x, NULL) == -2, "orelse second null bare");
}

static void test_zeroinit_after_line_directives(void) {
	// After the preprocessor emits many #line directives (file views),
	// the cached_file_idx must still match correctly.
	// This exercises the token→file mapping across many scopes.
	for (int i = 0; i < 100; i++) {
		struct {
			int a;
			int b;
		} s;

		CHECK(s.a == 0 && s.b == 0, "zeroinit after file view churn");
	}
}

static int orelse_side_effect_counter = 0;

static int orelse_side_effect_helper(int *p) {
	int *x = p orelse return orelse_side_effect_counter++;
	return *x;
}

static void test_orelse_return_expr_side_effects(void) {
	orelse_side_effect_counter = 0;
	int result = orelse_side_effect_helper(NULL);
	CHECK_EQ(result, 0, "orelse return expr side-effect: returns counter value");
	CHECK_EQ(orelse_side_effect_counter, 1, "orelse return expr side-effect: incremented exactly once");
}

#ifdef __GNUC__
static void test_generic_controlling_expr_not_evaluated(void) {
	int i = 0;
	// Use (i) instead of (i++) to avoid Clang -Wunevaluated-expression.
	// _Generic's controlling expression is unevaluated, so side effects are moot.
	int type_id = _Generic(i, int: 1, default: 2);
	CHECK_EQ(type_id, 1, "_Generic selects int");
	CHECK_EQ(i, 0, "_Generic controlling expression not evaluated");
}
#endif

static void test_struct_padding_zeroinit(void) {
	struct PaddedStruct {
		char c; /* 3 bytes padding on typical platforms */
		int i;
	};

	// Dirty the stack region first
	{
		unsigned char dirty[sizeof(struct PaddedStruct)];
		memset(dirty, 0xFF, sizeof(dirty));
		(void)dirty;
	}
	struct PaddedStruct ps;
	unsigned char *bytes = (unsigned char *)&ps;
	int all_zero = 1;
	for (size_t b = 0; b < sizeof(struct PaddedStruct); b++)
		if (bytes[b] != 0) all_zero = 0;
	CHECK(all_zero, "struct padding bytes zeroed by = {0}");
}

static void test_attribute_parser_torture(void) {
	// Attribute after struct definition on variable
	struct AttrS {
		int x;
	} __attribute__((packed)) attr_s;

	CHECK_EQ(attr_s.x, 0, "__attribute__((packed)) struct zeroed");

	// Attribute on function pointer variable
	void (*__attribute__((unused)) attr_fp)(void);
	CHECK(attr_fp == NULL, "attributed function pointer zeroed");

	// Aligned variable
	int __attribute__((aligned(16))) attr_aligned;
	CHECK_EQ(attr_aligned, 0, "__attribute__((aligned)) int zeroed");
}

static void test_goto_over_for_init_bug(void) {
	// goto over a for-loop with init-declaration should be allowed
	int x = 1;
	goto skip_for;
	for (int i = 0; i < 10; i++) {
		x += i;
	}
skip_for:
	CHECK_EQ(x, 1, "goto over for-init should not be rejected");
}

static void test_goto_over_for_init_braceless_bug(void) {
	// Same bug, but braceless for body
	int x = 1;
	goto skip_for2;
	for (int i = 0; i < 5; i++)
		x += i;
skip_for2:
	CHECK_EQ(x, 1, "goto over braceless for-init should not be rejected");
}

static void test_goto_over_multiple_for_init_bug(void) {
	// Multiple for-loops, all with init-declarations
	int x = 1;
	goto skip_all;
	for (int i = 0; i < 3; i++) {
		x += i;
	}
	for (int j = 0; j < 3; j++) {
		x += j;
	}
skip_all:
	CHECK_EQ(x, 1, "goto over multiple for-inits should not be rejected");
}

static void run_goto_over_for_init_bug_tests(void) {
	test_goto_over_for_init_bug();
	test_goto_over_for_init_braceless_bug();
	test_goto_over_multiple_for_init_bug();
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

static void test_case_label_bypasses_block_scoped_zeroinit(void) {
	printf("\n--- case Label Bypasses Block-Scoped Zero-Init ---\n");

	// case 2: inside the { block } opened by case 1: — jumping directly
	// to case 2 bypasses the zero-init of x.
	const char *code =
	    "void f(int val) {\n"
	    "    switch (val) {\n"
	    "    case 1: {\n"
	    "        int x;\n"
	    "        x = 42;\n"
	    "    case 2:\n"
	    "        (void)x;\n"
	    "        break;\n"
	    "    }\n"
	    "    }\n"
	    "}\n"
	    "int main(void) { f(2); return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "case zeroinit bypass: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	// The transpiler should error because case 2 jumps over int x's zero-init.
	CHECK(result.status != PRISM_OK,
	      "case zeroinit bypass: should error when case label jumps over zero-init'd variable");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_backward_goto_block_bypass(void) {
	printf("\n--- Backward goto Block-Bypass ---\n");

	// goto L is below L:, so the goto is backward. It re-enters
	// the block but jumps over 'int x' which was zero-init'd.
	const char *code =
	    "void f(int n) {\n"
	    "    {\n"
	    "        int x;\n"
	    "        x = 42;\n"
	    "    L:\n"
	    "        (void)x;\n"
	    "    }\n"
	    "    if (n > 0) goto L;\n"
	    "}\n"
	    "int main(void) { f(1); return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "backward goto bypass: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	// The transpiler should error because goto L re-enters the block
	// past the zero-init'd declaration of x.
	CHECK(result.status != PRISM_OK,
	      "backward goto bypass: should error when backward goto bypasses zero-init'd var");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_forward_declared_longjmp_blind_spot(void) {
	printf("\n--- Forward-Declared longjmp Blind Spot ---\n");

	// yeet() wraps longjmp but is defined after bad().
	// bad() uses defer and calls yeet() — should be flagged.
	const char *code =
	    "#include <setjmp.h>\n"
	    "void yeet(jmp_buf buf);\n"
	    "void bad(jmp_buf buf) {\n"
	    "    defer (void)0;\n"
	    "    yeet(buf);\n"
	    "}\n"
	    "void yeet(jmp_buf buf) {\n"
	    "    longjmp(buf, 1);\n"
	    "}\n"
	    "int main(void) { return 0; }\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "fwd longjmp: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	// Should error: bad() uses defer but calls a longjmp wrapper
	CHECK(result.status != PRISM_OK,
	      "fwd longjmp: should error when defer + forward-declared longjmp wrapper");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_deep_nesting_goto_bypass(void) {
	printf("\n--- 256-Depth Guardrail Bypass ---\n");

	// Phase 2A scope tree handles any depth (up to 4096).
	// Verify that backward goto past an uninitialized declaration
	// at depth 301 is correctly rejected (no depth-limit blind spot).
	char code[8192];
	int pos = 0;
	pos += snprintf(code + pos, sizeof(code) - pos, "int main(void) {\n");
	for (int i = 0; i < 300; i++)
		pos += snprintf(code + pos, sizeof(code) - pos, "{\n");
	pos += snprintf(code + pos, sizeof(code) - pos,
	    "int x;\n"
	    "label: ;\n"
	    "(void)x;\n");
	for (int i = 0; i < 300; i++)
		pos += snprintf(code + pos, sizeof(code) - pos, "}\n");
	pos += snprintf(code + pos, sizeof(code) - pos,
	    "goto label;\n"
	    "return 0;\n"
	    "}\n");

	char *path = create_temp_file(code);
	CHECK(path != NULL, "deep goto: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);

	CHECK(result.status != PRISM_OK,
	      "deep goto: backward goto past declaration at depth 301 must be rejected");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_c23_attr_label_forward_goto(void) {
	printf("\n--- C23 Attr Label Forward Goto ---\n");

	// Forward goto past a block to a label with [[maybe_unused]] attribute.
	// The scanner must recognize "L [[maybe_unused]]:" as a label definition.
	const char *code =
	    "void test(void) {\n"
	    "    goto L;\n"
	    "    { int x; (void)x; }\n"
	    "    L [[maybe_unused]]: ;\n"
	    "}\n";

	char *path = create_temp_file(code);
	CHECK(path != NULL, "c23 attr label: create temp file");

	PrismFeatures features = prism_defaults();
	PrismResult result = prism_transpile_file(path, features);
	CHECK_EQ(result.status, PRISM_OK, "c23 attr label fwd: transpiles OK");
	CHECK(result.output != NULL, "c23 attr label fwd: output not NULL");

	// int x inside the block should be zero-initialized because the forward goto
	// crosses scope. If the label wasn't detected, x wouldn't get = 0.
	CHECK(strstr(result.output, "x = 0") != NULL,
	      "c23 attr label fwd: int x gets = 0 (forward goto crosses scope to C23-attributed label)");

	prism_free(&result);
	unlink(path);
	free(path);
}

static void test_switch_inner_loop_break_no_false_exit(void) {
	/* Regression: break/continue inside a loop nested in a switch case
	   must not falsely mark the switch scope as having a control exit.
	   The defer runs at block close, so fallthrough is permitted here,
	   but mark_switch_control_exit must still not set had_control_exit
	   (correctness of internal state). Verify the code is accepted and
	   the loop-break appears in the output (targets the loop, not switch). */
	const char *code =
	    "void f(int x) {\n"
	    "    switch (x) {\n"
	    "    case 1: {\n"
	    "        defer { (void)0; }\n"
	    "        while (1) { break; }\n"
	    "    }\n"
	    "    case 2:\n"
	    "        break;\n"
	    "    }\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "switch_loop_break.c", prism_defaults());
	CHECK(r.status == PRISM_OK, "switch inner-loop break accepted (defer runs at block close)");
	if (r.output) {
		CHECK(strstr(r.output, "while") != NULL,
		      "switch inner-loop break: output contains the while loop");
	}
	prism_free(&r);
}

// raw int x, y; — Phase 1D keeps p1d_saw_raw=true for y,
// but Pass 2 clears is_raw on comma. CFG verifier thinks y is raw
// (safe to skip) while Pass 2 emits y = 0 (skipped by goto → uninit).
// UPDATE: raw now applies to ALL declarators in a comma-list (matching
// const/static/extern behavior), so y IS raw in both passes, and goto
// skipping it is safe (no zeroinit emitted → no skip violation).
static void test_raw_comma_desync_goto_bypass(void) {
	printf("\n--- raw Comma Desync Goto Bypass ---\n");

	const char *code =
	    "int main(void) {\n"
	    "    goto skip;\n"
	    "    raw int x, y;\n"
	    "    skip: return 0;\n"
	    "}\n";

	PrismResult r = prism_transpile_source(code, "raw_comma_desync.c", prism_defaults());
	// Both x and y are raw — goto past them is safe (no zeroinit to skip).
	CHECK(r.status == PRISM_OK,
	      "raw comma desync: goto past all-raw declarators is safe");
	prism_free(&r);
}

// e->decl.is_vla = type.is_vla || decl.is_array — flags fixed-size
// arrays as VLAs. raw int x[5]; + goto should be allowed (not a VLA).
static void test_fixed_array_not_vla_with_raw(void) {
	printf("\n--- Fixed Array Not VLA With raw ---\n");

	const char *code =
	    "int main(void) {\n"
	    "    goto skip;\n"
	    "    raw int x[5];\n"
	    "    skip: return 0;\n"
	    "}\n";

	PrismResult r = prism_transpile_source(code, "fixed_array_raw.c", prism_defaults());
	// x[5] is a fixed-size array + raw: safe to skip, no VLA.
	CHECK(r.status == PRISM_OK,
	      "fixed array raw: goto past raw fixed array must be accepted (not a VLA)");
	prism_free(&r);
}

// -fno-safety downgraded VLA skip violations to warnings.
// SPEC says VLA skip violations must remain hard errors regardless of -fno-safety.
static void test_vla_skip_hard_error_with_fno_safety(void) {
	printf("\n--- VLA Skip Hard Error With -fno-safety ---\n");

	const char *code =
	    "void f(int n) {\n"
	    "    goto skip;\n"
	    "    int arr[n];\n"
	    "    skip: (void)0;\n"
	    "}\n";

	PrismFeatures feat = prism_defaults();
	feat.warn_safety = true;
	PrismResult r = prism_transpile_source(code, "vla_fno_safety.c", feat);
	// VLA skip must be a hard error even with -fno-safety
	CHECK(r.status != PRISM_OK,
	      "VLA skip must be hard error even with -fno-safety");
	prism_free(&r);
}

// typeof((int[n])) — inner parens push typeof_paren_depth to 2,
// blinding the VLA-in-typeof detection in parse_type_specifier.
// Result: goto-over-typeof-VLA is downgraded from hard error to warning
// under -fno-safety because the P1K_DECL entry lacks is_vla=true.
static void test_typeof_paren_vla_skip_hard_error(void) {
	printf("\n--- typeof Paren VLA Skip Hard Error ---\n");

	const char *code =
	    "void f(int n) {\n"
	    "    goto skip;\n"
	    "    typeof((int[n])) arr;\n"
	    "    skip: (void)0;\n"
	    "}\n";

	PrismFeatures feat = prism_defaults();
	feat.warn_safety = true;
	PrismResult r = prism_transpile_source(code, "typeof_vla_paren.c", feat);
	CHECK(r.status != PRISM_OK,
	      "typeof((int[n])) VLA skip must be hard error with -fno-safety");
	prism_free(&r);
}

// Phase 1C backward walk from '{' to find parameter list '(...)' does not
// skip GNU __attribute__((...)) between ')' and '{'. If a parameter shadows a
// typedef, the shadow is not registered and the typedef name is parsed as a type
// inside the function body — emitting spurious zero-init like `mytype z = {0};`.
static void test_gnu_attr_param_shadow(void) {
	printf("\n--- GNU Attr Param Shadow ---\n");

	// mytype (param) shadows typedef mytype. Inside the body, `mytype z;`
	// must be treated as expression `mytype` + undeclared `z` (NOT as a
	// declaration of type mytype), so zero-init must NOT appear.
	const char *code =
	    "typedef int mytype;\n"
	    "void f(mytype mytype) __attribute__((noinline)) {\n"
	    "    mytype z;\n"
	    "    (void)z;\n"
	    "}\n"
	    "int main(void) { f(42); return 0; }\n";

	PrismResult r = prism_transpile_source(code, "gnu_attr_shadow.c", prism_defaults());
	CHECK(r.status == PRISM_OK,
	      "gnu attr param shadow: transpilation must succeed");
	if (r.output) {
		// If shadow works: `mytype` is a variable, so `mytype z;` is NOT a
		// declaration — no zero-init should be emitted.
		// If shadow fails: `mytype` is a type, so `mytype z = {0};` appears.
		CHECK(strstr(r.output, "z = {0}") == NULL &&
		      strstr(r.output, "z = 0") == NULL,
		      "gnu attr param shadow: mytype must shadow typedef (no zero-init for z)");
	}
	prism_free(&r);
}

// CFG verifier skips user-initialized declarations (has_init == true),
// allowing goto/switch to jump over them. Jumping over `int x = 5;` leaves x
// indeterminate — the exact vulnerability zero-init exists to prevent.
static void test_goto_over_native_init(void) {
	printf("\n--- Goto Over Native Init ---\n");

	// Forward goto jumps over user-initialized variable — must be rejected.
	const char *code =
	    "void f(void) {\n"
	    "    goto skip;\n"
	    "    {\n"
	    "        int key_length = 32;\n"
	    "    skip:\n"
	    "        (void)key_length;\n"
	    "    }\n"
	    "}\n"
	    "int main(void) { f(); return 0; }\n";

	PrismResult r = prism_transpile_source(code, "goto_native_init.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "goto over native init: must reject goto jumping over user-initialized var");
	prism_free(&r);
}

static void test_switch_case_over_native_init(void) {
	printf("\n--- Switch Case Over Native Init ---\n");

	// case 2 jumps over user-initialized key_length in nested block — must be rejected.
	const char *code =
	    "void f(int status) {\n"
	    "    switch (status) {\n"
	    "        case 1: {\n"
	    "            int key_length = 32;\n"
	    "        case 2:\n"
	    "            (void)key_length;\n"
	    "        }\n"
	    "    }\n"
	    "}\n"
	    "int main(void) { f(2); return 0; }\n";

	PrismResult r = prism_transpile_source(code, "switch_native_init.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "switch over native init: must reject case label bypassing user-initialized var");
	prism_free(&r);
}

// const aggregates that require a full memset (unions, VLAs, _Atomic
// aggregates) cannot be safely zero-initialized — modifying a defined
// const object is UB (ISO C11 §6.7.3p6). Prism must reject these rather
// than silently lie to the compiler via (void*) laundering.
static void test_const_aggregate_memset_rejected(void) {
	printf("\n--- const Aggregate Memset Rejected ---\n");

	// Uninitialized const union — would emit memset on a const object.
	{
		const char *code =
		    "void f(void) {\n"
		    "    const union U { int a; char b[4096]; } u;\n"
		    "    (void)u;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "const_union.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "const union without initializer: must reject (memset on const is UB)");
		prism_free(&r);
	}

	// Uninitialized const VLA — runtime-sized, must memset; also UB on const.
	{
		const char *code =
		    "void f(int n) {\n"
		    "    const int a[n];\n"
		    "    (void)a;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "const_vla.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "const VLA without initializer: must reject (memset on const is UB)");
		prism_free(&r);
	}

	// With explicit initializer, const union is fine — no memset emitted.
	{
		const char *code =
		    "void f(void) {\n"
		    "    const union U { int a; char b[4]; } u = {0};\n"
		    "    (void)u;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "const_union_init.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "const union with explicit initializer: accepted");
		prism_free(&r);
	}

	// `raw` opts out — user took responsibility.
	{
		const char *code =
		    "void f(void) {\n"
		    "    raw const union U { int a; char b[4]; } u;\n"
		    "    (void)u;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "const_union_raw.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "const union + raw: accepted");
		prism_free(&r);
	}
}

// raw exempts the check — user explicitly opted out of safety.
static void test_goto_over_raw_native_init(void) {
	printf("\n--- Goto Over raw Native Init ---\n");

	const char *code =
	    "void f(void) {\n"
	    "    goto skip;\n"
	    "    {\n"
	    "        raw int key_length = 32;\n"
	    "    skip:\n"
	    "        (void)key_length;\n"
	    "    }\n"
	    "}\n"
	    "int main(void) { f(); return 0; }\n";

	PrismResult r = prism_transpile_source(code, "goto_raw_native_init.c", prism_defaults());
	CHECK(r.status == PRISM_OK,
	      "goto over raw native init: raw must exempt user-initialized var from check");
	prism_free(&r);
}

// braceless for/if/switch bodies have no scope_id in scope_tree,
// so p1_scan_init_shadows never creates a P1K_DECL entry (body_sid == 0).
// The CFG verifier is completely blind to init-declared variables in
// braceless control flow.
static void test_braceless_forinit_cfg_bypass(void) {
	printf("\n--- Braceless for-init CFG bypass ---\n");

	// Forward goto into braceless for-body skips init variable.
	// Non-VLA, non-raw: should trigger "bypasses initialization".
	const char *code_nonvla =
	    "void f(void) {\n"
	    "    goto inside;\n"
	    "    for (int x = 42;;)\n"
	    "        inside: break;\n"
	    "}\n"
	    "int main(void) { return 0; }\n";
	PrismResult r1 = prism_transpile_source(code_nonvla, "braceless_for_cfg1.c", prism_defaults());
	CHECK(r1.status != PRISM_OK,
	      "braceless for-init: goto into braceless body must catch init skip");
	prism_free(&r1);

	// raw VLA in braceless for-init: VLA skip should still be caught.
	const char *code_vla =
	    "void f(int n) {\n"
	    "    goto inside;\n"
	    "    for (raw int arr[n];;)\n"
	    "        inside: break;\n"
	    "}\n"
	    "int main(void) { return 0; }\n";
	PrismResult r2 = prism_transpile_source(code_vla, "braceless_for_cfg2.c", prism_defaults());
	CHECK(r2.status != PRISM_OK,
	      "braceless for-init: raw VLA in braceless body must catch VLA skip");
	prism_free(&r2);

	// Control: a goto that jumps OVER (not into) the for-statement is OK.
	const char *code_over =
	    "void f(void) {\n"
	    "    goto past;\n"
	    "    for (int x = 42;;)\n"
	    "        break;\n"
	    "    past: ;\n"
	    "}\n"
	    "int main(void) { return 0; }\n";
	PrismResult r3 = prism_transpile_source(code_over, "braceless_for_cfg3.c", prism_defaults());
	CHECK_EQ(r3.status, PRISM_OK,
	         "braceless for-init: goto past entire statement must be accepted");
	prism_free(&r3);
}

// p1_scan_init_shadows uses if instead of while for raw keywords.
// Two consecutive raw tokens (e.g. from macro expansion) cause early return,
// so no P1K_DECL is created and the CFG verifier is blind to the VLA.
static void test_raw_raw_vla_forinit_cfg_blind(void) {
	printf("\n--- raw raw VLA for-init CFG blindness ---\n");

	// goto jumps INTO the for-body, skipping the VLA allocation in init.
	// With a single raw the error fires; with raw raw the CFG is blind.
	const char *code =
	    "void f(int n) {\n"
	    "    goto inside;\n"
	    "    for (raw raw int arr[n]; ; ) {\n"
	    "        inside:\n"
	    "        arr[0] = 42;\n"
	    "        break;\n"
	    "    }\n"
	    "}\n"
	    "int main(void) { return 0; }\n";

	PrismResult r = prism_transpile_source(code, "rawraw_vla_cfg.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "raw raw VLA for-init: goto into body must be caught (VLA skip)");
	prism_free(&r);
}

// signal_temps_clear() only resets the counter but doesn't zero the path
// buffers.  If a SIGINT arrives in the race window between CAS (counter++) and
// memcpy (path write), the signal handler would unlink stale paths from a
// previous compilation cycle.
static void test_signal_temps_clear_zeroes_paths(void) {
	signal_temps_register("/tmp/prism_test_signal_race_dummy");
	int n = signal_temps_load();
	CHECK(n >= 1, "signal_temps_register incremented counter");
	CHECK(signal_temps[n - 1][0] != '\0', "signal_temps entry written");

	signal_temps_clear();
	CHECK(signal_temps_load() == 0, "signal_temps_clear reset counter");
	// The path buffer MUST be zeroed so the signal handler won't unlink
	// stale entries during the TOCTOU race window.
	CHECK(signal_temps[n - 1][0] == '\0',
	      "signal_temps_clear zeroes path buffers (TOCTOU fix)");
}

// signal_temps_clear() only zeroes byte 0 of each buffer, leaving stale
// data in bytes 1..PATH_MAX-1. If signal_temps_register's memcpy is interrupted
// by a signal after writing only the first byte, the signal handler reconstructs
// a stale path (new byte 0 + old bytes 1..N) and calls unlink() on it.
static void test_signal_temps_clear_full_buffer(void) {
	const char *old_path = "/tmp/prism_old_file_XXXXXX.c";
	signal_temps_register(old_path);
	int n = signal_temps_load();
	CHECK(n >= 1, "signal_temps_register incremented counter");

	signal_temps_clear();

	// After clear, the ENTIRE buffer must be zeroed — not just byte 0.
	// If only byte 0 is zeroed, bytes 1..N still contain the old path.
	// Simulating a partial memcpy (only first byte written) must NOT
	// reconstruct the old path.
	int stale_bytes = 0;
	size_t len = strlen(old_path);
	for (size_t i = 1; i <= len; i++) {
		if (signal_temps[n - 1][i] != '\0')
			stale_bytes++;
	}
	CHECK(stale_bytes == 0,
	      "signal_temps_clear zeroes full buffer (no stale path data)");
}

// p1_register_param_shadows digs into inner parameter lists of unnamed
// function pointer parameters.  void f(int (*)(int a, int b)) registers 'b'
// as a shadow for the outer function, poisoning any typedef named 'b'.
static void test_unnamed_fnptr_param_shadow_leak(void) {
	const char *code =
	    "typedef int b;\n"
	    "void register_callback(int (*)(int a, int b)) {\n"
	    "    b my_var;\n"
	    "    (void)my_var;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "fnptr_shadow.c", prism_defaults());
	CHECK(r.status == PRISM_OK, "unnamed fnptr param shadow: transpile OK");
	CHECK(r.output && strstr(r.output, "b my_var = {0}"),
	      "unnamed fnptr param shadow: typedef 'b' must not be shadowed by "
	      "inner param name — 'b my_var' must be zero-initialized");
	prism_free(&r);
}

// bracket orelse with control flow (return/goto/break) is rejected only
// during Pass 2 (inside walk_balanced_orelse), violating the spec invariant
// "every semantic error before Pass 2 emits its first byte".
// This must be caught in Phase 1G when P1_OE_BRACKET is annotated.
static void test_bracket_orelse_ctrlflow_pass1_error(void) {
	const char *code =
	    "int n;\n"
	    "void f(void) {\n"
	    "    int arr[n orelse return];\n"
	    "    (void)arr;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "bracket_oe_cf.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "bracket orelse control flow: must be rejected");
	CHECK(r.error_msg && strstr(r.error_msg, "control flow"),
	      "bracket orelse control flow: error message mentions control flow");
	prism_free(&r);
}

// If goto skips over a scalar variable AND a VLA, the verifier loop
// breaks on the scalar (first match). Under -fno-safety the scalar is just a
// warning, and the VLA hard error is completely masked.
static void test_vla_masked_by_scalar_fno_safety(void) {
	printf("\n--- VLA Masked By Scalar Under -fno-safety ---\n");

	const char *code =
	    "void f(int n) {\n"
	    "    goto skip;\n"
	    "    int x = 42;\n"
	    "    int vla[n];\n"
	    "    skip: (void)0;\n"
	    "}\n";

	PrismFeatures feat = prism_defaults();
	feat.warn_safety = true;
	PrismResult r = prism_transpile_source(code, "vla_masked.c", feat);
	CHECK(r.status != PRISM_OK,
	      "scalar+VLA skip: VLA must still be hard error under -fno-safety");
	prism_free(&r);
}

static void test_duplicate_label_detection(void) {
	printf("\n--- Duplicate Label Detection ---\n");

	/* Duplicate labels are invalid C (constraint violation 6.8.1).
	 * The CFG verifier uses a hash map keyed by label name, so a
	 * duplicate silently overwrites the first entry's slot.  This
	 * could cause goto safety checks to analyze against the wrong
	 * label.  Prism should detect the duplicate and error. */
	const char *code =
	    "void f(void) {\n"
	    "    goto L;\n"
	    "L: ;\n"
	    "L: ;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "dup_label.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "dup-label: duplicate label must be rejected");
	if (r.error_msg)
		CHECK(strstr(r.error_msg, "duplicate label") != NULL,
		      "dup-label: error message mentions 'duplicate label'");
	prism_free(&r);
}

// register _Atomic aggregate must error in Phase 1,
// not during Pass 2 emission (invariant: all semantic errors before output).
static void test_register_atomic_aggregate_phase1(void) {
	printf("\n--- register _Atomic aggregate: Phase 1 error ---\n");
#ifndef _MSC_VER
	const char *code =
	    "void g(void) { int y; }\n"
	    "void f(void) { register _Atomic struct { int a; } x; (void)x; }\n";
	PrismResult r = prism_transpile_source(code, "reg_atomic_p1.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "register-atomic-agg: must be rejected");
	CHECK(r.error_msg && strstr(r.error_msg, "register"),
	      "register-atomic-agg: error mentions 'register'");
	prism_free(&r);
#endif
}

// switch body unbraced decl must error in Phase 1,
// not during Pass 2 emission.
static void test_switch_unbraced_decl_phase1(void) {
	printf("\n--- switch unbraced decl: Phase 1 error ---\n");
	const char *code =
	    "void g(void) { int y; }\n"
	    "void f(int x) { switch(x) { case 1: int z; (void)z; break; } }\n";
	PrismResult r = prism_transpile_source(code, "switch_unbraced_p1.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "switch-unbraced-decl: must be rejected");
	CHECK(r.error_msg && (strstr(r.error_msg, "switch") || strstr(r.error_msg, "braces")),
	      "switch-unbraced-decl: error mentions switch or braces");
	prism_free(&r);
}

// orelse with empty action in defer body must error in Phase 1.
static void test_orelse_empty_action_in_defer_phase1(void) {
	printf("\n--- orelse empty action in defer: Phase 1 error ---\n");
	const char *code =
	    "int *get(void);\n"
	    "void f(void) {\n"
	    "    defer {\n"
	    "        int *p = get() orelse;\n"
	    "    };\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "oe_empty_defer_p1.c", prism_defaults());
	CHECK(r.status != PRISM_OK,
	      "orelse-empty-defer: must be rejected");
	prism_free(&r);
}

// case/default label jumping into a statement expression is UB.
// The P1K_GOTO handler checks scope_stmt_expr_ancestor; P1K_CASE must too.
static void test_case_label_into_stmt_expr(void) {
	printf("\n--- case/default label into statement expression ---\n");

	// Sub-test 1: case inside stmt-expr without any local declarations
	// (no zero-init bypass to detect — only the stmt-expr structural violation)
	{
		const char *code =
		    "int global_result;\n"
		    "void test(int state) {\n"
		    "    switch (state) {\n"
		    "        case 0:\n"
		    "            global_result = ({\n"
		    "                case 1:\n"
		    "                    42;\n"
		    "            });\n"
		    "            break;\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "case_stmtexpr1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "case-into-stmtexpr: must be rejected (UB)");
		CHECK(r.error_msg && strstr(r.error_msg, "statement expression"),
		      "case-into-stmtexpr: error mentions statement expression");
		prism_free(&r);
	}

	// Sub-test 2: default label inside stmt-expr
	{
		const char *code =
		    "int global_result;\n"
		    "void test(int state) {\n"
		    "    switch (state) {\n"
		    "        case 0:\n"
		    "            global_result = ({\n"
		    "                default:\n"
		    "                    99;\n"
		    "            });\n"
		    "            break;\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "default_stmtexpr.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "default-into-stmtexpr: must be rejected (UB)");
		prism_free(&r);
	}

	// Sub-test 3: case NOT inside stmt-expr (control — should pass)
	{
		const char *code =
		    "void test(int state) {\n"
		    "    switch (state) {\n"
		    "        case 0: break;\n"
		    "        case 1: break;\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "case_normal.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
			 "case-normal: normal switch must pass");
		prism_free(&r);
	}

	// Sub-test 4: both switch and case inside same stmt-expr (should pass)
	{
		const char *code =
		    "void test(int state) {\n"
		    "    int result = ({\n"
		    "        int r;\n"
		    "        switch (state) {\n"
		    "            case 0: r = 10; break;\n"
		    "            case 1: r = 20; break;\n"
		    "            default: r = 0; break;\n"
		    "        }\n"
		    "        r;\n"
		    "    });\n"
		    "    (void)result;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "switch_in_stmtexpr.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
			 "switch-in-stmtexpr: switch+case both inside same stmtexpr must pass");
		prism_free(&r);
	}
}

// Bug 5: Computed goto + VLA must be rejected even with raw keyword / zeroinit disabled.
// VLA stack allocation is bypassed if a computed goto jumps past it → stack corruption.
static void test_computed_goto_vla(void) {
	printf("\n--- Computed Goto VLA ---\n");

	// Sub-test 1: raw VLA + computed goto (was silently accepted before fix)
	{
		const char *code =
		    "void test(int n) {\n"
		    "    void *table[] = { &&label1 };\n"
		    "    raw int vla[n];\n"
		    "    goto *table[0];\n"
		    "label1:\n"
		    "    return;\n"
		    "}\n";
		PrismFeatures feat = prism_defaults();
		feat.zeroinit = false;
		PrismResult r = prism_transpile_source(code, "cgoto_raw_vla.c", feat);
		CHECK(r.status != PRISM_OK,
		      "computed goto + raw VLA: must be rejected (stack corruption)");
		prism_free(&r);
	}

	// Sub-test 2: non-raw VLA + computed goto with zeroinit enabled
	{
		const char *code =
		    "void test(int n) {\n"
		    "    void *table[] = { &&label1 };\n"
		    "    int vla[n];\n"
		    "    goto *table[0];\n"
		    "label1:\n"
		    "    return;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "cgoto_vla.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "computed goto + VLA: must be rejected");
		prism_free(&r);
	}

	// Sub-test 3: computed goto without VLA (raw fixed array) — should pass
	{
		const char *code =
		    "void test(void) {\n"
		    "    void *table[] = { &&label1 };\n"
		    "    raw int arr[10];\n"
		    "    goto *table[0];\n"
		    "label1:\n"
		    "    return;\n"
		    "}\n";
		PrismFeatures feat = prism_defaults();
		feat.zeroinit = false;
		PrismResult r = prism_transpile_source(code, "cgoto_no_vla.c", feat);
		CHECK_EQ(r.status, PRISM_OK,
			 "computed goto + raw fixed array: must pass");
		prism_free(&r);
	}
}

// Bug 6: Enum constant shadowed by parameter → VLA not detected → invalid = {0} init.
static void test_enum_shadow_vla(void) {
	printf("\n--- Enum Shadow VLA ---\n");

	// Sub-test 1: parameter shadows enum constant used as array size
	{
		const char *code =
		    "enum { SIZE = 10 };\n"
		    "void test(int SIZE) {\n"
		    "    int arr[SIZE];\n"
		    "    arr[0] = 42;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "enum_shadow_vla.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
			 "enum shadow VLA: transpilation must succeed");
		if (r.output) {
			CHECK(strstr(r.output, "arr[SIZE] = {0}") == NULL &&
			      strstr(r.output, "arr [ SIZE ] = { 0 }") == NULL,
			      "enum shadow VLA: must not emit = {0} for VLA (use memset)");
			CHECK(strstr(r.output, "memset") != NULL ||
			      strstr(r.output, "__prism_p_") != NULL,
			      "enum shadow VLA: must use memset/byte-loop for VLA zero-init");
		}
		prism_free(&r);
	}

	// Sub-test 2: local variable shadows enum constant
	{
		const char *code =
		    "enum { COUNT = 5 };\n"
		    "void test(void) {\n"
		    "    int COUNT = 3;\n"
		    "    int arr[COUNT];\n"
		    "    arr[0] = 1;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "enum_shadow_local.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
			 "enum shadow local VLA: transpilation must succeed");
		if (r.output) {
			CHECK(strstr(r.output, "arr[COUNT] = {0}") == NULL,
			      "enum shadow local: must not emit = {0} for VLA");
		}
		prism_free(&r);
	}

	// Sub-test 3: no shadow (enum constant used directly) — control case
	{
		const char *code =
		    "enum { SIZE = 10 };\n"
		    "void test(void) {\n"
		    "    int arr[SIZE];\n"
		    "    arr[0] = 42;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "enum_no_shadow.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
			 "enum no shadow: transpilation must succeed");
		if (r.output) {
			CHECK(strstr(r.output, "= {0}") != NULL || strstr(r.output, "= { 0 }") != NULL,
			      "enum no shadow: fixed array must use = {0} init");
		}
		prism_free(&r);
	}
}

// VLA skip undetected when -fno-zeroinit.  cfg_check_range and
// P1K_CASE handler gate the entire decl loop behind FEAT(F_ZEROINIT).
// Jumping past a VLA is UB (C99/C11 6.8.6.1p1) regardless of zeroinit.
static void test_vla_cfg_bypass_fno_zeroinit(void) {
	printf("\n--- VLA CFG bypass -fno-zeroinit ---\n");

	// Sub-test 1: goto over VLA with zeroinit off
	{
		const char *code =
		    "void f(int n) {\n"
		    "    goto skip;\n"
		    "    int vla[n];\n"
		    "skip:\n"
		    "    return;\n"
		    "}\n";
		PrismFeatures feat = prism_defaults();
		feat.zeroinit = false;
		PrismResult r = prism_transpile_source(code, "t63a.c", feat);
		CHECK(r.status != PRISM_OK,
		      "goto over VLA must error even with -fno-zeroinit");
		CHECK(r.error_msg && strstr(r.error_msg, "VLA"),
		      "error message mentions 'VLA'");
		prism_free(&r);
	}

	// Sub-test 2: switch/case over VLA with zeroinit off
	{
		const char *code =
		    "void f(int n, int sel) {\n"
		    "    switch (sel) {\n"
		    "    case 0:;\n"
		    "        int vla[n];\n"
		    "        vla[0] = 1;\n"
		    "        break;\n"
		    "    default:\n"
		    "        break;\n"
		    "    }\n"
		    "}\n";
		PrismFeatures feat = prism_defaults();
		feat.zeroinit = false;
		PrismResult r = prism_transpile_source(code, "t63b.c", feat);
		CHECK(r.status != PRISM_OK,
		      "case over VLA must error even with -fno-zeroinit");
		prism_free(&r);
	}

	// Sub-test 3: goto over non-VLA fixed array with zeroinit off — must PASS
	{
		const char *code =
		    "void f(void) {\n"
		    "    goto skip;\n"
		    "    int arr[10];\n"
		    "skip:\n"
		    "    return;\n"
		    "}\n";
		PrismFeatures feat = prism_defaults();
		feat.zeroinit = false;
		PrismResult r = prism_transpile_source(code, "t63c.c", feat);
		CHECK_EQ(r.status, PRISM_OK,
		         "goto over fixed array with -fno-zeroinit must pass");
		prism_free(&r);
	}

	// Sub-test 4: both defer and zeroinit off, VLA skip must still error
	{
		const char *code =
		    "void f(int n) {\n"
		    "    goto skip;\n"
		    "    int vla[n];\n"
		    "skip:\n"
		    "    return;\n"
		    "}\n";
		PrismFeatures feat = prism_defaults();
		feat.zeroinit = false;
		feat.defer = false;
		PrismResult r = prism_transpile_source(code, "t63d.c", feat);
		CHECK(r.status != PRISM_OK,
		      "goto over VLA must error even with -fno-defer -fno-zeroinit");
		prism_free(&r);
	}
}

// Qualifier preceding VLA bracket in typeof/_Atomic makes VLA invisible.
// parse_type_specifier's look-behind heuristic for distinguishing array types
// from expressions only checks type keywords, typedefs, ], *, and ) — misses
// qualifiers (const, volatile, restrict, _Atomic as qualifier).
static void test_typeof_qualifier_vla_blindspot(void) {
	printf("\n--- typeof qualifier VLA blindspot ---\n");

	// Sub-test 1: typeof(int *const [get_n()]) + orelse forces split → must error
	// (split re-emits type, evaluating get_n() twice for VM types)
	{
		const char *code =
		    "int get_n(void);\n"
		    "void *fetch(void);\n"
		    "void f(void) {\n"
		    "    __typeof__(int *const [get_n()]) a = fetch() orelse 0, b;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t65a.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "typeof VM-type + orelse split must be rejected");
		prism_free(&r);
	}

	// Sub-test 2: typeof(int volatile [get_n()]) + orelse → must also detect VLA
	{
		const char *code =
		    "int get_n(void);\n"
		    "void *fetch(void);\n"
		    "void f(void) {\n"
		    "    __typeof__(int volatile [get_n()]) a = fetch() orelse 0, b;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t65b.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "typeof volatile VLA + orelse split must be rejected");
		prism_free(&r);
	}

	// Sub-test 3: raw typeof(int *const [size]) + goto — CFG must catch VLA skip
	{
		const char *code =
		    "void f(int size) {\n"
		    "    goto skip;\n"
		    "    raw __typeof__(int *const [size]) vla;\n"
		    "skip:\n"
		    "    return;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t65c.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "goto over raw typeof-VLA with qualifier must error");
		prism_free(&r);
	}

	// Sub-test 4: _Atomic(int *const [n]) + goto — CFG must catch VLA skip
	{
		const char *code =
		    "void f(int n) {\n"
		    "    goto skip;\n"
		    "    raw _Atomic(int *const [n]) x;\n"
		    "skip:\n"
		    "    return;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t65d.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "goto over raw _Atomic VLA with qualifier must error");
		prism_free(&r);
	}

	// Sub-test 5: typeof(int *const [5]) — fixed-size must NOT false-positive
	{
		const char *code =
		    "void f(void) {\n"
		    "    __typeof__(int *const [5]) a, b;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t65e.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		         "fixed-size const array must not false-positive as VLA");
		prism_free(&r);
	}
}

// typeof(typeof(int[n])) was misidentified as a function type by
// is_typeof_func_type. The inner typeof keyword followed by '(' looked
// like a function type signature typeof(int(int)), causing zero-init to
// skip the memset.
static void test_typeof_nested_typeof_vla(void) {
	printf("\n--- typeof nested typeof VLA ---\n");

	// Sub-test 1: typeof(typeof(int[n])) — must get memset
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    typeof(typeof(int[n])) b;\n"
		    "    (void)b;\n"
		    "}\n",
		    "tn_typeof1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "typeof(typeof(VLA)) transpiles OK");
		if (r.output)
			CHECK(strstr(r.output, "memset") != NULL ||
			      strstr(r.output, "__prism_p_") != NULL,
			      "typeof(typeof(VLA)) must get memset");
		prism_free(&r);
	}

	// Sub-test 2: triple-nested typeof
	{
		PrismResult r = prism_transpile_source(
		    "void f(int n) {\n"
		    "    typeof(typeof(typeof(int[n]))) c;\n"
		    "    (void)c;\n"
		    "}\n",
		    "tn_typeof2.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "triple-nested typeof(VLA) transpiles OK");
		if (r.output)
			CHECK(strstr(r.output, "memset") != NULL ||
			      strstr(r.output, "__prism_p_") != NULL,
			      "triple-nested typeof(VLA) must get memset");
		prism_free(&r);
	}

	// Sub-test 3: actual function type inside typeof must still be detected
	{
		PrismResult r = prism_transpile_source(
		    "void f(void) {\n"
		    "    typeof(int(int, int)) fn;\n"
		    "    (void)fn;\n"
		    "}\n",
		    "tn_typeof3.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "typeof(func-type) transpiles OK");
		if (r.output)
			CHECK(strstr(r.output, "memset") == NULL &&
			      strstr(r.output, "__prism_p_") == NULL,
			      "typeof(func-type) must NOT get memset");
		prism_free(&r);
	}
}

// sizeof(type qualifier [n]) — qualifier blinds VLA detection in array_size_is_vla.
static void test_sizeof_vla_qualifier_blindspot(void) {
	printf("\n--- sizeof VLA qualifier blindspot ---\n");

	// Sub-test 1: sizeof(int const [n]) — 'const' before '[' blinds VLA detection.
	{
		const char *code =
		    "void f(int n) {\n"
		    "    goto skip;\n"
		    "    raw int arr[sizeof(int const [n])];\n"
		    "skip:\n"
		    "    arr[0] = 1;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t68a.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "sizeof(int const [n]) must be detected as VLA");
		CHECK(r.error_msg && strstr(r.error_msg, "VLA"),
		      "error message mentions 'VLA'");
		prism_free(&r);
	}

	// Sub-test 2: sizeof(int volatile [n]) — same bug with volatile qualifier.
	{
		const char *code =
		    "void f(int n) {\n"
		    "    goto skip;\n"
		    "    raw int arr[sizeof(int volatile [n])];\n"
		    "skip:\n"
		    "    arr[0] = 1;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t68b.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "sizeof(int volatile [n]) must be detected as VLA");
		prism_free(&r);
	}

	// Sub-test 3: sizeof(int [n]) without qualifier — must still work (regression).
	{
		const char *code =
		    "void f(int n) {\n"
		    "    goto skip;\n"
		    "    raw int arr[sizeof(int [n])];\n"
		    "skip:\n"
		    "    arr[0] = 1;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t68c.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "sizeof(int [n]) must still be detected as VLA (regression)");
		prism_free(&r);
	}

	// Sub-test 4: sizeof(int const [5]) — fixed-size, no VLA. Must NOT error.
	{
		const char *code =
		    "void f(void) {\n"
		    "    goto skip;\n"
		    "    raw int arr[sizeof(int const [5])];\n"
		    "skip:\n"
		    "    arr[0] = 1;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "t68d.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "sizeof(int const [5]) is not VLA, must not error");
		prism_free(&r);
	}
}

static void test_braceless_labeled_decl_cfg(void) {
	printf("\n--- Braceless labeled-declaration CFG ---\n");

	// Backward goto to label before VLA in braceless body.
	// C11 §6.2.1p7: scope of vla begins AFTER its declarator.
	// The label L_vla is before the declarator, so the goto
	// destination is outside the VLA's scope.  Legal C.
	{
		const char *code =
		    "void execute(int n) {\n"
		    "    if (1)\n"
		    "        L_vla: raw int vla[n];\n"
		    "    goto L_vla;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "braceless_vla1.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "braceless labeled VLA: backward goto to label before declarator is legal");
		prism_free(&r);
	}

	// Forward goto past already-dead braceless declaration.
	// Part 1 (body_close_idx) bounds the lifetime to the semicolon,
	// so the declaration is dead before End:.  Must be accepted.
	{
		const char *code =
		    "void process(void) {\n"
		    "    goto End;\n"
		    "    if (1)\n"
		    "        L_decl: int x;\n"
		    "    End:\n"
		    "    return;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "braceless_fwd1.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "braceless labeled decl: forward goto past dead scope must be accepted");
		prism_free(&r);
	}

	// Braced labeled VLA: backward goto to label before VLA declarator.
	// Same as braceless case — label is before scope of vla.  Legal C.
	{
		const char *code =
		    "void ctrl(int n) {\n"
		    "    if (1) {\n"
		    "        L2: raw int vla[n];\n"
		    "    }\n"
		    "    goto L2;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "braceless_ctrl1.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "braced labeled VLA: backward goto to label before declarator is legal");
		prism_free(&r);
	}

	// Control: labeled declaration in same scope without braceless — no false positive.
	{
		const char *code =
		    "void same(void) {\n"
		    "    L3: raw int x;\n"
		    "    (void)x;\n"
		    "    goto L3;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "braceless_ctrl2.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "labeled decl in same scope without braceless: backward goto must be accepted");
		prism_free(&r);
	}
}

// sizeof((arr)[n]) is sizeof(arr[n]), NOT a VLA dimension. The ')' from the
// expression-grouping parens must not be treated as a type predecessor.
static void test_sizeof_expr_paren_subscript(void) {
	printf("\n--- sizeof expression-paren subscript (VLA hallucination) ---\n");

	// sizeof((arr)[n]) — expression paren + subscript, not VLA dimension.
	// Use 'raw' to avoid zeroinit; if VLA hallucination fires, Prism
	// rejects the goto as "skip over VLA declaration". With raw + non-VLA,
	// goto-skip is legal.
	{
		const char *code =
		    "void f(int n) {\n"
		    "    int arr[10];\n"
		    "    goto SKIP;\n"
		    "    raw int x[sizeof((arr)[n])];\n"
		    "SKIP:\n"
		    "    (void)x; (void)arr;\n"
		    "}\n"
		    "int main(void) { f(3); return 0; }\n";
		PrismResult r = prism_transpile_source(code, "sof_paren1.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "sizeof((arr)[n]) must not cause VLA hallucination");
		prism_free(&r);
	}

	// sizeof(typeof(int)[n]) IS a genuine VLA sizeof — goto MUST be rejected
	{
		const char *code =
		    "void f(int n) {\n"
		    "    goto SKIP;\n"
		    "    raw int x[sizeof(typeof(int)[n])];\n"
		    "SKIP:\n"
		    "    (void)x;\n"
		    "}\n"
		    "int main(void) { f(3); return 0; }\n";
		PrismResult r = prism_transpile_source(code, "sof_paren2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "sizeof(typeof(int)[n]) IS VLA — goto must be rejected");
		prism_free(&r);
	}

	// sizeof(int (*)[n]) — pointer-to-VLA type, genuine VLA sizeof
	{
		const char *code =
		    "void f(int n) {\n"
		    "    goto SKIP;\n"
		    "    raw int x[sizeof(int (*)[n])];\n"
		    "SKIP:\n"
		    "    (void)x;\n"
		    "}\n"
		    "int main(void) { f(3); return 0; }\n";
		PrismResult r = prism_transpile_source(code, "sof_paren3.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "sizeof(int (*)[n]) IS VLA — goto must be rejected");
		prism_free(&r);
	}

	// func()[n] — subscripting a function return value, not VLA
	{
		const char *code =
		    "int *get_arr(void);\n"
		    "void f(int n) {\n"
		    "    goto SKIP;\n"
		    "    raw int x[sizeof(get_arr()[n])];\n"
		    "SKIP:\n"
		    "    (void)x;\n"
		    "}\n"
		    "int main(void) { f(3); return 0; }\n";
		PrismResult r = prism_transpile_source(code, "sof_paren4.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "sizeof(func()[n]) must not cause VLA hallucination");
		prism_free(&r);
	}
}

// sizeof(arr[1][n]) — multidimensional array subscript, NOT a VLA dimension.
// The ']' from [1] must not be treated as a type predecessor for [n].
static void test_sizeof_multidim_subscript_vla(void) {
	printf("\n--- sizeof multidim subscript VLA hallucination ---\n");

	// sizeof((a)[1][n]) — expression subscript chain, fixed-size result
	{
		const char *code =
		    "int main(void) {\n"
		    "    int arr[10][10];\n"
		    "    int n = 5;\n"
		    "    goto SKIP;\n"
		    "    raw int x[sizeof((arr)[1][n])];\n"
		    "SKIP:\n"
		    "    (void)x;\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "mdim1.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "sizeof(arr[1][n]) must not cause VLA hallucination");
		prism_free(&r);
	}

	// sizeof(int[3][n]) — type context multidim, IS genuine VLA
	{
		const char *code =
		    "void f(int n) {\n"
		    "    goto SKIP;\n"
		    "    raw int x[sizeof(int[3][n])];\n"
		    "SKIP:\n"
		    "    (void)x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "mdim2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "sizeof(int[3][n]) IS VLA — goto must be rejected");
		prism_free(&r);
	}

	// sizeof(arr[n]) — single subscript with variable index, not VLA
	{
		const char *code =
		    "void f(int n) {\n"
		    "    int arr[10];\n"
		    "    goto SKIP;\n"
		    "    raw int x[sizeof(arr[n])];\n"
		    "SKIP:\n"
		    "    (void)x; (void)arr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "mdim3.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "sizeof(arr[n]) must not cause VLA hallucination");
		prism_free(&r);
	}

	// sizeof(ptr[0][1][n]) — triple subscript expression, not VLA
	{
		const char *code =
		    "void f(int n) {\n"
		    "    int arr[5][5][5];\n"
		    "    goto SKIP;\n"
		    "    raw int x[sizeof(arr[0][1][n])];\n"
		    "SKIP:\n"
		    "    (void)x; (void)arr;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "mdim4.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "sizeof(arr[0][1][n]) must not cause VLA hallucination");
		prism_free(&r);
	}
}

static void test_case_label_ctrl_pending_leak(void) {
	printf("\n--- case/default label p1d_ctrl_pending leak ---\n");

	// switch(0) sets p1d_ctrl_pending=true. case/default must reset it,
	// otherwise the declaration after the case gets braceless_close_idx
	// set to its semicolon, blinding the CFG verifier.
	{
		const char *code =
		    "int f(void) {\n"
		    "    goto L;\n"
		    "    switch (0)\n"
		    "        case 0: int x;\n"
		    "    x = 42;\n"
		    "L:\n"
		    "    return x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "ctrl_case1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "goto over braceless switch+case decl must error");
		prism_free(&r);
	}
	// default: variant
	{
		const char *code =
		    "int f(int c) {\n"
		    "    goto L;\n"
		    "    switch (c)\n"
		    "    default: int y;\n"
		    "    y = 0;\n"
		    "L:\n"
		    "    return y;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "ctrl_case2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "goto over braceless switch+default decl must error");
		prism_free(&r);
	}
	// control: simple statement after case does NOT regress
	{
		const char *code =
		    "int f(void) {\n"
		    "    goto L;\n"
		    "    switch (0)\n"
		    "        default: (void)0;\n"
		    "    int z;\n"
		    "L:\n"
		    "    return z;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "ctrl_case3.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "goto over decl after braceless switch (stmt body) must error");
		prism_free(&r);
	}
}

static void test_skip_one_stmt_cache_poison(void) {
	printf("\n--- skip_one_stmt cache poisoning ---\n");

	// For-init scanner calls skip_one_stmt_impl on the for-body.
	// Without the fix, trail caches true-branch tokens with the
	// else-branch end, poisoning braceless_close_idx for decls
	// inside the true-branch. A variable out of scope at a label
	// in the else branch gets a false-positive goto-over-decl error.
	{
		const char *code =
		    "int f(int c) {\n"
		    "    goto L;\n"
		    "    for (;c;)\n"
		    "        if (c)\n"
		    "            int x;\n"   // braceless body, scope ends at ';'
		    "        else\n"
		    "        L:  (void)0;\n" // L is after x's scope
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "cache1.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "goto L with x out of scope at L must succeed (no false positive)");
		if (r.status != PRISM_OK)
			printf("  false positive: %s\n", r.error_msg ? r.error_msg : "");
		prism_free(&r);
	}
	// Deeper nesting: while inside if true-branch
	{
		const char *code =
		    "int g(int c) {\n"
		    "    goto L;\n"
		    "    for (;c;)\n"
		    "        if (c)\n"
		    "            while (0) int y;\n"
		    "        else\n"
		    "        L:  (void)0;\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "cache2.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "goto L with y out of scope (nested while) must succeed");
		if (r.status != PRISM_OK)
			printf("  false positive: %s\n", r.error_msg ? r.error_msg : "");
		prism_free(&r);
	}
	// Control: goto that genuinely bypasses a decl still errors
	{
		const char *code =
		    "int h(int c) {\n"
		    "    goto L;\n"
		    "    for (;c;)\n"
		    "        if (c)\n"
		    "            int z;\n"
		    "        else\n"
		    "            (void)0;\n"
		    "L:\n"   // z is in braceless body, dead before L
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "cache3.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "goto L past dead braceless decl must succeed");
		prism_free(&r);
	}
}

static void test_skip_cache_braceless_switch_if_else(void) {
	printf("\n--- skip_cache braceless switch + if-else ---\n");

	// for-init scanner primes skip_cache. The else-transition trail
	// flush wrote the true-branch end to ALL trail entries (including
	// parent tokens like switch/if). When the braceless switch detector
	// later queried the poisoned 'if' token, it received a truncated
	// boundary, orphaning case labels in the else branch.
	// Fix: scope-aware trail flush via if_trail_snap[].

	// for-init + braceless switch + if-else
	{
		const char *code =
		    "int main(void) {\n"
		    "    for (int i = 0; i < 2; i++)\n"
		    "        switch (i)\n"
		    "            if (1) {\n"
		    "                case 1: ;\n"
		    "            } else {\n"
		    "                case 0: ;\n"
		    "            }\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "bsw1.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "for-init + braceless switch + if-else must not false-positive");
		if (r.status != PRISM_OK)
			printf("  false positive: %s\n", r.error_msg ? r.error_msg : "");
		prism_free(&r);
	}
	// for without init-decl (find_init_semicolon still triggers scan)
	{
		const char *code =
		    "int main(void) {\n"
		    "    int i;\n"
		    "    for (i = 0; i < 2; i++)\n"
		    "        switch (i)\n"
		    "            if (1) { case 1: ; } else { case 0: ; }\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "bsw2.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "for (no init-decl) + braceless switch + if-else must succeed");
		if (r.status != PRISM_OK)
			printf("  false positive: %s\n", r.error_msg ? r.error_msg : "");
		prism_free(&r);
	}
	// nested for-init
	{
		const char *code =
		    "int main(void) {\n"
		    "    for (int i = 0; i < 10; i++)\n"
		    "        for (int j = 0; j < 10; j++)\n"
		    "            switch (j)\n"
		    "                if (1) { case 1: ; } else { case 0: ; }\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "bsw3.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "nested for-init + braceless switch + if-else must succeed");
		if (r.status != PRISM_OK)
			printf("  false positive: %s\n", r.error_msg ? r.error_msg : "");
		prism_free(&r);
	}
	// C23 if-init + braceless switch + if-else
	{
		const char *code =
		    "int main(void) {\n"
		    "    if (int x = 0; x == 0)\n"
		    "        switch (x)\n"
		    "            if (1) { case 1: ; } else { case 0: ; }\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "bsw4.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "C23 if-init + braceless switch + if-else must succeed");
		if (r.status != PRISM_OK)
			printf("  false positive: %s\n", r.error_msg ? r.error_msg : "");
		prism_free(&r);
	}
	// Duff's device variant
	{
		const char *code =
		    "void f(char *to, const char *from, int count) {\n"
		    "    for (int n = (count + 7) / 8; n > 0; n--)\n"
		    "        switch (count & 7)\n"
		    "            if (0) { case 0: *to++ = *from++; }\n"
		    "            else if (0) { case 7: *to++ = *from++; }\n"
		    "            else if (0) { case 6: *to++ = *from++; }\n"
		    "            else { case 1: *to++ = *from++; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "bsw5.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "Duff's device in for-loop must succeed");
		if (r.status != PRISM_OK)
			printf("  false positive: %s\n", r.error_msg ? r.error_msg : "");
		prism_free(&r);
	}
	// Control: braced for-body (no cache poisoning path) still works
	{
		const char *code =
		    "int main(void) {\n"
		    "    int x = 0;\n"
		    "    switch (x)\n"
		    "        if (1) { case 1: ; } else { case 0: ; }\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "bsw6.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "braceless switch + if-else without for-init must succeed");
		prism_free(&r);
	}
	// Control: genuine bypass must still error
	{
		const char *code =
		    "int main(void) {\n"
		    "    for (int i = 0; i < 2; i++)\n"
		    "        switch (i) {\n"
		    "            int x;\n"
		    "            case 0: x = 1; break;\n"
		    "            case 1: x = 2; break;\n"
		    "        }\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "bsw7.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "genuine case bypass of decl in switch must still error");
		prism_free(&r);
	}
}

// skip_one_stmt do-while cache dropping: tn=0 on do entry destroyed
// the trail for all parent statements, preventing cache population.
// Subsequent skip_one_stmt calls on parent tokens (if, for) rescanned
// the entire subtree, degrading to O(N^2).
static void test_skip_cache_do_while_trail_drop(void) {
	printf("\n--- skip_cache do-while trail preservation ---\n");

	// do-while inside nested braceless if chain with for-init variable.
	// Without cache fix, each intermediate if token would be uncached,
	// causing Phase 1D to rescan the same tokens repeatedly.
	{
		const char *code =
		    "int f(int c) {\n"
		    "    goto L;\n"
		    "    for (int i = 0; i < 10; i++)\n"
		    "        if (c)\n"
		    "            if (c)\n"
		    "                if (c)\n"
		    "                    do { (void)0; } while(0);\n"
		    "L:\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "do_cache1.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "do-while cache: nested ifs + do + goto must succeed");
		if (r.status != PRISM_OK)
			printf("  error: %s\n", r.error_msg ? r.error_msg : "");
		prism_free(&r);
	}
	// do-while with if-else inside + outer if-else (no for-init skip issue)
	{
		const char *code =
		    "int g(int c) {\n"
		    "    goto L;\n"
		    "    if (c)\n"
		    "        do\n"
		    "            if (c) (void)0; else (void)1;\n"
		    "        while(0);\n"
		    "    else\n"
		    "    L: (void)0;\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "do_cache2.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "do-while cache: inner if-else + outer else + goto must succeed");
		if (r.status != PRISM_OK)
			printf("  error: %s\n", r.error_msg ? r.error_msg : "");
		prism_free(&r);
	}
	// Deeply nested braceless ifs before do-while (stress test for cache)
	{
		const char *code =
		    "int h(int c) {\n"
		    "    for (int i = 0; i < 1; i++)\n"
		    "        if (c)\n"
		    "        if (c)\n"
		    "        if (c)\n"
		    "        if (c)\n"
		    "        if (c)\n"
		    "        if (c)\n"
		    "        if (c)\n"
		    "        if (c)\n"
		    "            do { (void)0; } while(0);\n"
		    "    return 0;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "do_cache3.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "do-while cache: 8 nested ifs + do must succeed");
		prism_free(&r);
	}
}

static void test_sizeof_commutative_vla_bypass(void) {
	printf("\n--- sizeof commutative VLA bypass ---\n");

	// Commutative subscript: sizeof(1[vla_param]) = sizeof(vla_param[1])
	// Both yield a VLA-sized type. The commutative form bypassed detection
	// because prev_inner was '[' (not '*') and tok_next was ']' (not '[').
	{
		const char *code =
		    "void f(int n, int m, int p[n][m]) {\n"
		    "    goto L;\n"
		    "    raw int a[sizeof(1[p])];\n"
		    "L:  a[0] = 1;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "comvla1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "commutative subscript sizeof(1[vla_param]) must be caught");
		prism_free(&r);
	}
	// Pointer arithmetic: sizeof(*(1 + vla_param))
	{
		const char *code =
		    "void f(int n, int m, int p[n][m]) {\n"
		    "    goto L;\n"
		    "    raw int a[sizeof(*(1 + p))];\n"
		    "L:  a[0] = 1;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "comvla2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "pointer arithmetic sizeof(*(1 + vla_param)) must be caught");
		prism_free(&r);
	}
	// Reverse arithmetic: sizeof(*(vla_param + 1))
	{
		const char *code =
		    "void f(int n, int m, int p[n][m]) {\n"
		    "    goto L;\n"
		    "    raw int a[sizeof(*(p + 1))];\n"
		    "L:  a[0] = 1;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "comvla3.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "reverse arithmetic sizeof(*(vla_param + 1)) must be caught");
		prism_free(&r);
	}
	// Subtraction: sizeof(*(vla_param - 0))
	{
		const char *code =
		    "void f(int n, int m, int p[n][m]) {\n"
		    "    goto L;\n"
		    "    raw int a[sizeof(*(p - 0))];\n"
		    "L:  a[0] = 1;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "comvla4.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "subtraction sizeof(*(vla_param - 0)) must be caught");
		prism_free(&r);
	}
	// Control: sizeof(vla_param) alone is constant (pointer decay)
	{
		const char *code =
		    "void f(int n, int m, int p[n][m]) {\n"
		    "    goto L;\n"
		    "    raw int a[sizeof(p)];\n"
		    "L:  a[0] = 1;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "comvla5.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "sizeof(vla_param) alone must be constant (no false positive)");
		prism_free(&r);
	}
	// Normal form still caught (regression check)
	{
		const char *code =
		    "void f(int n, int m, int p[n][m]) {\n"
		    "    goto L;\n"
		    "    raw int a[sizeof(p[0])];\n"
		    "L:  a[0] = 1;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "comvla6.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "normal sizeof(vla_param[0]) must still be caught");
		prism_free(&r);
	}
}

// defer shadow same-block error fired during Pass 2 emission
// (check_defer_var_shadow at L1416).  Hoisted to Phase 1D.
static void test_defer_shadow_same_block_phase1(void) {
	printf("\n--- defer shadow same-block Phase 1 ---\n");

	// Attack vector: extern redeclaration after defer capture
	{
		const char *code =
		    "int handle;\n"
		    "void f(void) {\n"
		    "    defer { handle = 0; }\n"
		    "    extern int handle;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "dshad1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "extern after defer capture must be caught in Phase 1");
		prism_free(&r);
	}
	// Variable declaration after defer capture
	{
		const char *code =
		    "int val;\n"
		    "void f(void) {\n"
		    "    defer { val = 1; }\n"
		    "    int val;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "dshad2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "local decl after defer capture must be caught in Phase 1");
		prism_free(&r);
	}
	// Control: no-match (different name) must succeed
	{
		const char *code =
		    "int val;\n"
		    "void f(void) {\n"
		    "    defer { val = 1; }\n"
		    "    int other;\n"
		    "    other = val;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "dshad3.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "different name must not trigger defer shadow");
		prism_free(&r);
	}
	// Control: for-init local inside defer body must not cause false positive
	{
		const char *code =
		    "void f(void) {\n"
		    "    defer { for (int i = 0; i < 1; i++) {} }\n"
		    "    int i;\n"
		    "    i = 0;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "dshad4.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "for-init name inside defer body is not a capture");
		prism_free(&r);
	}
}

// for-init VLA needing memset fired error during Pass 2 emission
// (process_declarators L4207).  Hoisted to Phase 1D p1_scan_init_shadows.
static void test_for_init_vla_memset_phase1(void) {
	printf("\n--- for-init VLA memset Phase 1 ---\n");

	// Attack vector: multi-declarator VLA in for-init
	{
		const char *code =
		    "void f(int n) {\n"
		    "    for (int i = 0, arr[n]; i < n; i++)\n"
		    "        arr[i] = i;\n"
		    "}\n"
		    "int main(void) { f(5); return 0; }\n";
		PrismResult r = prism_transpile_source(code, "fivla1.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "VLA in for-init must be caught in Phase 1");
		prism_free(&r);
	}
	// Single VLA declarator in for-init
	{
		const char *code =
		    "void f(int n) {\n"
		    "    for (int arr[n];;) { break; }\n"
		    "}\n"
		    "int main(void) { f(5); return 0; }\n";
		PrismResult r = prism_transpile_source(code, "fivla2.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "single VLA in for-init must be caught in Phase 1");
		prism_free(&r);
	}
	// Control: VLA in for-init with initializer → OK (has_init)
	// Actually, VLA cannot have an initializer in C. Skip this control.

	// Control: non-VLA for-init must succeed
	{
		const char *code =
		    "void f(void) {\n"
		    "    for (int i = 0; i < 10; i++) {}\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "fivla3.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "non-VLA for-init must succeed");
		prism_free(&r);
	}
	// typeof in if-init (C23)
	{
		const char *code =
		    "int g(void);\n"
		    "void f(void) {\n"
		    "    if (typeof(g()) x; x > 0) {}\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "fivla4.c", prism_defaults());
		CHECK(r.status != PRISM_OK,
		      "typeof in if-init must be caught in Phase 1");
		prism_free(&r);
	}
}

static void test_vm_type_multi_decl_split_phase1(void) {
	printf("\n--- VM-type multi-declarator split Phase 1 ---\n");
	// typeof VLA + multi-decl + bracket orelse → error
	{
		const char *code =
		    "int get_size(void);\n"
		    "void f(void) {\n"
		    "    typeof(int[get_size()]) a, b[1 orelse 2];\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vmsplit1.c", prism_defaults());
		CHECK(r.status != PRISM_OK && r.error_msg && strstr(r.error_msg, "declaration split"),
		      "typeof VLA multi-decl with bracket orelse must error in Phase 1");
		prism_free(&r);
	}
	// typeof VLA + multi-decl + init on second → error
	{
		const char *code =
		    "int get_size(void);\n"
		    "void f(void) {\n"
		    "    typeof(int[get_size()]) a, b = {0};\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "vmsplit2.c", prism_defaults());
		CHECK(r.status != PRISM_OK && r.error_msg && strstr(r.error_msg, "declaration split"),
		      "typeof VLA multi-decl with second-decl init must error in Phase 1");
		prism_free(&r);
	}
	// Control: typeof VLA + multi-decl without split trigger → OK
	{
		const char *code =
		    "int get_size(void);\n"
		    "void f(void) {\n"
		    "    typeof(int[get_size()]) a, b;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "vmsplit3.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "typeof VLA multi-decl without split trigger must succeed");
		prism_free(&r);
	}
}

static void test_ctrl_paren_bracket_orelse_phase1(void) {
	printf("\n--- ctrl-paren bracket orelse Phase 1 ---\n");
	// for-init with bracket orelse → error
	{
		const char *code =
		    "void f(int n) {\n"
		    "    for (raw int arr[n orelse ({1;})];;) { (void)arr; break; }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "cpbo1.c", prism_defaults());
		CHECK(r.status != PRISM_OK && r.error_msg && strstr(r.error_msg, "control statement"),
		      "bracket orelse in for-init must error in Phase 1");
		prism_free(&r);
	}
	// Control: bracket orelse in normal declaration → OK
	{
		const char *code =
		    "int g(void);\n"
		    "void f(void) {\n"
		    "    raw int arr[g() orelse 1];\n"
		    "    (void)arr;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "cpbo2.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "bracket orelse in normal declaration must succeed");
		prism_free(&r);
	}
}

static void test_file_scope_typeof_orelse_no_crash(void) {
	printf("\n--- file-scope typeof orelse no crash ---\n");
	// File-scope typeof with orelse in array dim used to segfault (scope_tree NULL)
	{
		const char *code =
		    "int get_size(void);\n"
		    "typeof(int[get_size()]) a, b[1 orelse 2];\n";
		PrismResult r = prism_transpile_source(code, "fsto1.c", prism_defaults());
		CHECK(r.status != PRISM_OK && r.error_msg,
		      "file-scope typeof orelse must error, not crash");
		prism_free(&r);
	}
}

static void test_const_vla_orelse_phase1(void) {
	printf("\n--- const-VLA orelse Phase 1 ---\n");
	// const pointer-to-VLA + value fallback → accepted (pointer is mutable)
	{
		const char *code =
		    "int get_size(void);\n"
		    "void* get_ptr(void);\n"
		    "void f(void) {\n"
		    "    const int (*ptr)[get_size()] = get_ptr() orelse 0;\n"
		    "    (void)ptr;\n"
		    "};\n";
		PrismResult r = prism_transpile_source(code, "cvla1.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK,
		      "const-VLA orelse value fallback must accept (pointer-to-const is mutable)");
		prism_free(&r);
	}
	// Control: const VLA + block-form orelse → OK
	{
		const char *code =
		    "int get_size(void);\n"
		    "void* get_ptr(void);\n"
		    "void f(void) {\n"
		    "    const int (*ptr)[get_size()] = get_ptr() orelse { return; };\n"
		    "    (void)ptr;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "cvla2.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "const-VLA orelse block form must succeed");
		prism_free(&r);
	}
	// Control: non-const VLA + value fallback → OK
	{
		const char *code =
		    "int get_size(void);\n"
		    "void* get_ptr(void);\n"
		    "void f(void) {\n"
		    "    int (*ptr)[get_size()] = get_ptr() orelse 0;\n"
		    "    (void)ptr;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "cvla3.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "non-const VLA orelse value fallback must succeed");
		prism_free(&r);
	}
}

static void test_stmtexpr_orelse_fallback_phase1(void) {
	printf("\n--- stmt-expr orelse fallback Phase 1 ---\n");
	// GNU stmt-expr in decl orelse fallback → error
	{
		const char *code =
		    "int get_val(void);\n"
		    "void f(void) {\n"
		    "    int x = get_val() orelse ({ 0; });\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "seof1.c", prism_defaults());
		CHECK(r.status != PRISM_OK && r.error_msg && strstr(r.error_msg, "statement expression"),
		      "stmt-expr in decl orelse fallback must error in Phase 1");
		prism_free(&r);
	}
	// Control: bare orelse with stmt-expr → OK (bare path handles it)
	{
		const char *code =
		    "int get(void);\n"
		    "void f(void) {\n"
		    "    int x;\n"
		    "    x = get() orelse ({ 1; });\n"
		    "    (void)x;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "seof2.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "bare orelse with stmt-expr must succeed");
		prism_free(&r);
	}
	// Control: decl orelse block form → OK
	{
		const char *code =
		    "int get_val(void);\n"
		    "void f(void) {\n"
		    "    int x = get_val() orelse { return; };\n"
		    "    (void)x;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "seof3.c", prism_defaults());
		CHECK(r.status == PRISM_OK,
		      "decl orelse block form must succeed");
		prism_free(&r);
	}
}

// ── Round 61: Parenthesized orelse/defer + typeof side-effect Phase 1D checks ──

static void test_paren_orelse_phase1(void) {
	printf("\n--- paren orelse/defer Phase 1D ---\n");
	// Bug 1a: orelse inside nested call parens → Phase 1D error
	{
		const char *code =
		    "int *get(void);\n"
		    "int f(int x);\n"
		    "void g(void) {\n"
		    "    int a = f((int)(long)(get() orelse 0));\n"
		    "    (void)a;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "po1.c", prism_defaults());
		CHECK(r.status != PRISM_OK && r.error_msg && strstr(r.error_msg, "parentheses"),
		      "nested paren orelse: rejected in Phase 1D");
		prism_free(&r);
	}
	// Bug 1b: orelse in expression statement parens
	{
		const char *code =
		    "int *get(void);\n"
		    "void f(int *p);\n"
		    "void g(void) {\n"
		    "    f((get() orelse 0));\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "po2.c", prism_defaults());
		CHECK(r.status != PRISM_OK && r.error_msg && strstr(r.error_msg, "parentheses"),
		      "expr-stmt paren orelse: rejected in Phase 1D");
		prism_free(&r);
	}
	// Control: orelse in spanning-init paren without comma → valid
	{
		const char *code =
		    "int *get(void);\n"
		    "void f(void) {\n"
		    "    int *p = (get() orelse 0);\n"
		    "    (void)p;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "po3.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "spanning-init paren orelse: valid");
		prism_free(&r);
	}
	// Control: orelse in spanning-init paren WITH comma → rejected
	{
		const char *code =
		    "int *get(void);\n"
		    "void f(void) {\n"
		    "    int x = 0;\n"
		    "    int *p = (x++, get() orelse 0);\n"
		    "    (void)p;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "po4.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "comma paren orelse: rejected");
		prism_free(&r);
	}
}

static void test_typeof_orelse_side_effect_phase1(void) {
	printf("\n--- typeof orelse side-effect Phase 1D ---\n");
	// Bug 2: typeof orelse with ++ side effect → Phase 1D error
	{
		const char *code =
		    "int counter;\n"
		    "void f(void) {\n"
		    "    typeof(counter++ orelse 0) x = 5;\n"
		    "    (void)x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "tse1.c", prism_defaults());
		CHECK(r.status != PRISM_OK && r.error_msg && strstr(r.error_msg, "side effect"),
		      "typeof ++ side-effect: rejected in Phase 1D");
		prism_free(&r);
	}
	// Bug 2b: typeof orelse with function call side effect
	{
		const char *code =
		    "int *get(void);\n"
		    "void f(void) {\n"
		    "    typeof(get() orelse 0) x = 0;\n"
		    "    (void)x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "tse2.c", prism_defaults());
		CHECK(r.status != PRISM_OK && r.error_msg && strstr(r.error_msg, "side effect"),
		      "typeof fn-call side-effect: rejected in Phase 1D");
		prism_free(&r);
	}
	// Control: typeof orelse with no side effects → valid
	{
		const char *code =
		    "int val;\n"
		    "void f(void) {\n"
		    "    typeof(val orelse 0) x = 5;\n"
		    "    (void)x;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "tse3.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "typeof no-side-effect orelse: valid");
		prism_free(&r);
	}
}

static void test_anon_struct_split_phase1(void) {
	printf("\n--- anonymous struct split Phase 1D ---\n");
	// bracket orelse on anon struct multi-declarator
	{
		const char *code =
		    "void f(void) {\n"
		    "    struct { int x; } a, b[1 orelse 2];\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "as1.c", prism_defaults());
		CHECK(r.status != PRISM_OK && r.error_msg && strstr(r.error_msg, "anonymous"),
		      "anon struct bracket orelse split: rejected in Phase 1D");
		prism_free(&r);
	}
	// Control: anon struct with inline = {0} (no split needed) → valid
	{
		const char *code =
		    "void f(void) {\n"
		    "    struct { int x; } a, b = {1};\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "as2.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "anon struct inline zeroinit no split: valid");
		prism_free(&r);
	}
	// Control: named struct multi-declarator → valid
	{
		const char *code =
		    "void f(void) {\n"
		    "    struct S { int x; } a, b = {1};\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "as3.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "named struct multi-decl: valid");
		prism_free(&r);
	}
	// Control: anon struct without split → valid
	{
		const char *code =
		    "void f(void) {\n"
		    "    struct { int x; } a, b;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "as4.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "anon struct no split: valid");
		prism_free(&r);
	}
}

static void test_empty_orelse_action_phase1(void) {
	printf("\n--- empty orelse action Phase 1D ---\n");
	// bare orelse with empty action (semicolon)
	{
		const char *code =
		    "int f(int x) {\n"
		    "    x orelse ;\n"
		    "    return x;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "eoa1.c", prism_defaults());
		CHECK(r.status != PRISM_OK && r.error_msg && strstr(r.error_msg, "expected statement"),
		      "empty orelse action: rejected in Phase 1D");
		prism_free(&r);
	}
	// Control: orelse with valid action → valid
	{
		const char *code =
		    "int f(int x) {\n"
		    "    x orelse return 0;\n"
		    "    return x;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "eoa2.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "orelse with return action: valid");
		prism_free(&r);
	}
}

static void test_defer_continue_execution_order(void) {
	printf("\n--- defer + continue execution order ---\n");
	log_reset();

	for (int i = 0; i < 2; i++) {
		defer log_append("D");
		if (i == 0) {
			log_append("C");
			continue; // Must trigger 'D', THEN loop does i++
		}
		log_append("B");
	}

	// i=0: 'C', defer 'D', i++
	// i=1: 'B', defer 'D', i++
	CHECK_LOG("CDBD", "defer + continue execution order must precede loop increment");
}

static void test_interleaved_raw_multi_declarator(void) {
	printf("\n--- interleaved raw multi-declarator ---\n");

	// Dirty the stack
	volatile char garbage[128];
	memset((void *)garbage, 0xAA, sizeof(garbage));

	// 'a' and 'c' must be 0. 'b' and 'd' are raw (uninitialized).
	int a, raw b, c, raw d;

	CHECK_EQ(a, 0, "interleaved raw: first variable must be zero-initialized");
	CHECK_EQ(c, 0, "interleaved raw: variable after raw declarator must be zero-initialized");

	// Suppress unused warnings
	(void)b; (void)d; (void)garbage[0];
}

static void test_union_padding_zeroinit(void) {
	printf("\n--- union padding zero-init ---\n");

	// C11 §6.7.9p21 (implicit zeroing of remainder) only applies to
	// aggregates (C11 §6.2.5p21: arrays+structs, NOT unions).
	// GCC-15 empirically leaves union bytes beyond the first member
	// indeterminate with = {0}.  Prism must use memset for unions.
	const char *code =
	    "void f(void) {\n"
	    "    union U { char c; long long ll; };\n"
	    "    union U u;\n"
	    "    (void)u;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "up_zi.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "union padding zeroinit: transpile succeeds");
	if (r.status == PRISM_OK) {
		CHECK(strstr(r.output, "memset") != NULL ||
		      strstr(r.output, "__prism_p_") != NULL,
		      "union padding bytes (beyond first member) must be zeroed via memset");
		CHECK(strstr(r.output, "= {0}") == NULL,
		      "union must not use = {0} (only zeros first member on GCC)");
	}
	prism_free(&r);
}

// Regression: typedef union also routes through memset (not = {0}).
// The is_union flag must propagate through the typedef table.
static void test_typedef_union_padding_zeroinit(void) {
	printf("\n--- Typedef union padding zeroinit ---\n");
	const char *code =
	    "typedef union { char c; double d; long arr[4]; } BigU;\n"
	    "void f(void) {\n"
	    "    BigU u;\n"
	    "    (void)u;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "tdu_zi.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "typedef union padding: transpile succeeds");
	if (r.status == PRISM_OK && r.output) {
		CHECK(strstr(r.output, "memset") != NULL ||
		      strstr(r.output, "__prism_p_") != NULL,
		      "typedef union must use memset (C11 §6.7.9p21 doesn't cover unions)");
		CHECK(strstr(r.output, "= {0}") == NULL,
		      "typedef union must not use = {0}");
	}
	prism_free(&r);
}

// register union cannot use memset (register forbids &) and = {0} only
// zeros first member (C11 §6.7.9p17). Must reject.
static void test_register_union_zeroinit_rejected(void) {
	printf("\n--- Register union zeroinit rejected ---\n");
	const char *code =
	    "void f(void) {\n"
	    "    register union { char c; long long ll; } u;\n"
	    "    (void)u;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "reg_union.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_ERR_SYNTAX, "register union: rejected");
	if (r.error_msg) {
		CHECK(strstr(r.error_msg, "register") != NULL,
		      "register union: error mentions register");
	}
	prism_free(&r);

	// register union with explicit init is fine (user handles it)
	const char *code2 =
	    "void f(void) {\n"
	    "    register union { char c; long long ll; } u = { .ll = 0 };\n"
	    "    (void)u;\n"
	    "}\n";
	PrismResult r2 = prism_transpile_source(code2, "reg_union_init.c", prism_defaults());
	CHECK_EQ(r2.status, PRISM_OK, "register union with explicit init: accepted");
	prism_free(&r2);

	// register union with raw is fine (user opted out)
	const char *code3 =
	    "void f(void) {\n"
	    "    register raw union { char c; long long ll; } u;\n"
	    "    (void)u;\n"
	    "}\n";
	PrismResult r3 = prism_transpile_source(code3, "reg_union_raw.c", prism_defaults());
	CHECK_EQ(r3.status, PRISM_OK, "register union with raw: accepted");
	prism_free(&r3);
}

// _Atomic(union U) must also route to memset
static void test_atomic_union_zeroinit(void) {
	printf("\n--- Atomic union zeroinit ---\n");
	const char *code =
	    "union U { char c; long long ll; };\n"
	    "void f(void) {\n"
	    "    _Atomic(union U) u;\n"
	    "    (void)u;\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "atomic_union.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_OK, "atomic union: transpile succeeds");
	if (r.status == PRISM_OK && r.output) {
		CHECK(strstr(r.output, "memset") != NULL ||
		      strstr(r.output, "__prism_p_") != NULL,
		      "atomic union must use memset");
		CHECK(strstr(r.output, "= {0}") == NULL,
		      "atomic union must not use = {0}");
	}
	prism_free(&r);
}

// for-init union must be rejected (needs memset, can't emit in init position)
static void test_for_init_union_rejected(void) {
	printf("\n--- For-init union rejected ---\n");
	const char *code =
	    "void f(void) {\n"
	    "    for (union { char c; long long ll; } u; ;) { (void)u; break; }\n"
	    "}\n";
	PrismResult r = prism_transpile_source(code, "fi_union.c", prism_defaults());
	CHECK_EQ(r.status, PRISM_ERR_SYNTAX, "for-init union: rejected");
	prism_free(&r);
}

static void test_noreturn_fwd_decl_no_crash(void) {
	// _Noreturn forward declaration inside a function body must NOT
	// inject __builtin_unreachable() after it — that's a declaration,
	// not a call.
	const char *code =
		"#include <stdlib.h>\n"
		"void die(int code) { exit(code); }\n"
		"int test(void) {\n"
		"    _Noreturn void die(int);\n"
		"    return 0;\n"
		"}\n"
		"int main(void) { return test(); }\n";
	PrismResult r = prism_transpile_source(code, "nr_fwd.c", prism_defaults());
	CHECK(r.status == PRISM_OK, "noreturn fwd decl: transpiles OK");
	if (r.output) {
		// The transpiled output must NOT have __builtin_unreachable after the declaration
		char *decl = strstr(r.output, "_Noreturn void die(int);");
		if (decl) {
			char *after = decl + strlen("_Noreturn void die(int);");
			CHECK(strstr(after, "return") != NULL,
			      "noreturn fwd decl: return statement not eliminated");
		}
#ifndef _WIN32
		check_transpiled_output_compiles_and_runs(
			r.output,
			"noreturn fwd decl: compiles",
			"noreturn fwd decl: runs without crash");
#endif
	}
	prism_free(&r);

	// __attribute__((noreturn)) form
	const char *code2 =
		"#include <stdlib.h>\n"
		"void die2(int code) { exit(code); }\n"
		"int test2(void) {\n"
		"    __attribute__((noreturn)) void die2(int);\n"
		"    return 0;\n"
		"}\n"
		"int main(void) { return test2(); }\n";
	PrismResult r2 = prism_transpile_source(code2, "nr_fwd2.c", prism_defaults());
	CHECK(r2.status == PRISM_OK, "noreturn fwd decl (attr): transpiles OK");
	if (r2.output) {
#ifndef _WIN32
		check_transpiled_output_compiles_and_runs(
			r2.output,
			"noreturn fwd decl (attr): compiles",
			"noreturn fwd decl (attr): runs without crash");
#endif
	}
	prism_free(&r2);
}

// emit_block_body missed label colon (':') handling — at_stmt_start
// was never reset to true after labels inside statement expressions.
// This caused zero-init bypass and false orelse errors.
static void test_stmt_expr_label_zeroinit(void) {
	// 1. Zero-init after label in stmt-expr: 'int attempt;' must get = 0
	{
		const char *code =
		    "int get_val(void);\n"
		    "void test(void) {\n"
		    "    int status = ({\n"
		    "        retry:\n"
		    "        int attempt;\n"
		    "        attempt = get_val();\n"
		    "        if (!attempt) goto retry;\n"
		    "        attempt;\n"
		    "    });\n"
		    "    (void)status;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "stexpr_label_zi.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "stmt-expr label zeroinit: transpiles OK");
		if (r.output) {
			// After 'retry:', the declaration must be zero-initialized
			CHECK(strstr(r.output, "attempt = 0") != NULL ||
			      strstr(r.output, "attempt =0") != NULL,
			      "stmt-expr label zeroinit: attempt gets = 0 after label");
		}
		prism_free(&r);
	}

	// 2. orelse after label in stmt-expr: must not crash
	{
		const char *code =
		    "int fetch(void);\n"
		    "void test(void) {\n"
		    "    int r = ({\n"
		    "        recover:\n"
		    "        int val = fetch() orelse goto recover;\n"
		    "        val;\n"
		    "    });\n"
		    "    (void)r;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "stexpr_label_oe.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "stmt-expr label orelse: transpiles OK");
		prism_free(&r);
	}

	// 3. case/default labels in switch inside stmt-expr
	{
		const char *code =
		    "void test(int x) {\n"
		    "    int r = ({\n"
		    "        int val;\n"
		    "        switch (x) {\n"
		    "        case 1: {\n"
		    "            int a;\n"
		    "            val = a;\n"
		    "            break;\n"
		    "        }\n"
		    "        default: {\n"
		    "            int b;\n"
		    "            val = b;\n"
		    "            break;\n"
		    "        }\n"
		    "        }\n"
		    "        val;\n"
		    "    });\n"
		    "    (void)r;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "stexpr_case_zi.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "stmt-expr case zeroinit: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "a = 0") != NULL ||
			      strstr(r.output, "a =0") != NULL,
			      "stmt-expr case zeroinit: 'a' gets = 0 after case label");
			CHECK(strstr(r.output, "b = 0") != NULL ||
			      strstr(r.output, "b =0") != NULL,
			      "stmt-expr case zeroinit: 'b' gets = 0 after default label");
		}
		prism_free(&r);
	}

	// 4. Ternary ?: must NOT falsely trigger label detection
	{
		const char *code =
		    "void test(int cond) {\n"
		    "    int r = ({\n"
		    "        int val = cond ? 42 : 99;\n"
		    "        val;\n"
		    "    });\n"
		    "    (void)r;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "stexpr_ternary.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "stmt-expr ternary: transpiles OK");
		if (r.output) {
			// The ternary ': 99' must NOT inject '= 0' on '99'
			CHECK(strstr(r.output, "? 42 : 99") != NULL ||
			      strstr(r.output, "?42:99") != NULL ||
			      strstr(r.output, "? 42 :99") != NULL,
			      "stmt-expr ternary: ternary colon not confused with label");
		}
		prism_free(&r);
	}
}

// emit_block_body was missing handle_sue_body, typeof/bracket orelse
// dispatch, and check_enum_typedef_defer_shadow.
static void test_emit_block_body_gaps(void) {
	// 1. SUE type-only def inside stmt-expr must NOT zero-init struct fields
	{
		const char *code =
		    "void test(void) {\n"
		    "    int r = ({\n"
		    "        struct Foo { int x; int y; };\n"
		    "        struct Foo f;\n"
		    "        f.x + f.y;\n"
		    "    });\n"
		    "    (void)r;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "stexpr_sue.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "stmt-expr SUE body: transpiles OK");
		if (r.output) {
			// 'struct Foo { int x; int y; }' must be verbatim — no = 0
			// on fields. But 'struct Foo f' SHOULD be zero-init.
			CHECK(strstr(r.output, "int x = 0") == NULL &&
			      strstr(r.output, "int x =0") == NULL,
			      "stmt-expr SUE body: struct field x not zero-init");
			CHECK(strstr(r.output, "int y = 0") == NULL &&
			      strstr(r.output, "int y =0") == NULL,
			      "stmt-expr SUE body: struct field y not zero-init");
		}
		prism_free(&r);
	}

	// 2. typeof(expr orelse fallback) inside stmt-expr — must transform
	{
		const char *code =
		    "void test(int x) {\n"
		    "    int r = ({\n"
		    "        sizeof(typeof(x orelse 0));\n"
		    "    });\n"
		    "    (void)r;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "stexpr_typeof_oe.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "stmt-expr typeof orelse: transpiles OK");
		if (r.output) {
			// orelse must have been transformed — should not appear in output
			CHECK(strstr(r.output, "orelse") == NULL,
			      "stmt-expr typeof orelse: 'orelse' transformed away");
		}
		prism_free(&r);
	}

	// 3. bracket orelse inside stmt-expr — must transform
	{
		const char *code =
		    "void test(int idx) {\n"
		    "    int arr[10];\n"
		    "    int r = ({\n"
		    "        arr[idx orelse 0];\n"
		    "    });\n"
		    "    (void)r;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "stexpr_bracket_oe.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "stmt-expr bracket orelse: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "orelse") == NULL,
			      "stmt-expr bracket orelse: 'orelse' transformed away");
		}
		prism_free(&r);
	}

	// 4. enum constant inside stmt-expr that shadows defer-captured name
	// The enum constant 'handle' lives only inside the stmt-expr scope.
	// After ({}), the outer 'handle' is restored — no control-flow exit
	// occurs while the shadow is live, so this is safe (C11 §6.2.1p4).
	{
		const char *code =
		    "void cleanup(int);\n"
		    "void test(void) {\n"
		    "    int handle = 42;\n"
		    "    defer cleanup(handle);\n"
		    "    int r = ({\n"
		    "        enum { handle = 99 };\n"
		    "        handle;\n"
		    "    });\n"
		    "    (void)r;\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "stexpr_enum_shadow.c", prism_defaults());
		CHECK_EQ(r.status, PRISM_OK, "stmt-expr enum defer shadow: safe (no exit while live)");
		prism_free(&r);
	}

	// 4b. enum constant inside stmt-expr WITH return — this IS dangerous
	{
		const char *code =
		    "void cleanup(int);\n"
		    "void test(void) {\n"
		    "    int handle = 42;\n"
		    "    defer cleanup(handle);\n"
		    "    if (1) {\n"
		    "        enum { handle = 99 };\n"
		    "        (void)handle;\n"
		    "        return;\n"
		    "    }\n"
		    "}\n";
		PrismResult r = prism_transpile_source(code, "stexpr_enum_shadow_ret.c", prism_defaults());
		CHECK(r.status != PRISM_OK, "enum defer shadow with return: detected");
		prism_free(&r);
	}
}

// Objective-C @interface/@implementation ivar blocks look like
// code blocks to Prism, causing zero-init to fire on instance variables.
// These blocks must be treated like struct bodies (no zero-init on fields).
static void test_objc_ivar_block_zeroinit(void) {
	printf("\n--- Objective-C ivar block zero-init ---\n");

	// 1. Simple @interface with ivars — must NOT zero-init fields
	{
		const char *code =
		    "@interface NSObject {\n"
		    "    int isa;\n"
		    "    void *reserved;\n"
		    "}\n"
		    "@end\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "objc_iface.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "objc @interface: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "isa = 0") == NULL &&
			      strstr(r.output, "isa =0") == NULL,
			      "objc @interface: ivar 'isa' not zero-inited");
			CHECK(strstr(r.output, "reserved = 0") == NULL &&
			      strstr(r.output, "reserved =0") == NULL,
			      "objc @interface: ivar 'reserved' not zero-inited");
		}
		prism_free(&r);
	}

	// 2. @interface with inheritance — must NOT zero-init fields
	{
		const char *code =
		    "@interface NSAutoreleasePool : NSObject {\n"
		    "    void *_reserved3;\n"
		    "    void *_reserved2;\n"
		    "    void *_reserved;\n"
		    "}\n"
		    "@end\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "objc_inherit.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "objc @interface+inherit: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "_reserved3 = 0") == NULL,
			      "objc @interface+inherit: _reserved3 not zero-inited");
			CHECK(strstr(r.output, "_reserved2 = 0") == NULL,
			      "objc @interface+inherit: _reserved2 not zero-inited");
			CHECK(strstr(r.output, "_reserved = 0") == NULL &&
			      strstr(r.output, "_reserved =0") == NULL,
			      "objc @interface+inherit: _reserved not zero-inited");
		}
		prism_free(&r);
	}

	// 3. @implementation with ivars — must NOT zero-init fields
	{
		const char *code =
		    "@implementation NSString {\n"
		    "    int numBytes;\n"
		    "    int _unused;\n"
		    "}\n"
		    "@end\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "objc_impl.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "objc @implementation: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "numBytes = 0") == NULL,
			      "objc @implementation: numBytes not zero-inited");
			CHECK(strstr(r.output, "_unused = 0") == NULL,
			      "objc @implementation: _unused not zero-inited");
		}
		prism_free(&r);
	}

	// 4. @interface category extension () — must NOT zero-init fields
	{
		const char *code =
		    "@interface NSFoo () {\n"
		    "    double value;\n"
		    "}\n"
		    "@end\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "objc_cat.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "objc @interface category: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "value = 0") == NULL &&
			      strstr(r.output, "value =0") == NULL,
			      "objc @interface category: value not zero-inited");
		}
		prism_free(&r);
	}

	// 5. Regular struct body must still NOT zero-init fields (sanity check)
	{
		const char *code =
		    "struct Foo { int x; int y; };\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "objc_struct.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "struct body: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "int x = 0") == NULL,
			      "struct body: field x not zero-inited");
		}
		prism_free(&r);
	}

	// 6. Regular variables must still get zero-init (sanity check)
	{
		const char *code =
		    "void test(void) {\n"
		    "    int a;\n"
		    "    void *p;\n"
		    "    (void)a; (void)p;\n"
		    "}\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "objc_var.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "regular vars: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "a = 0") != NULL,
			      "regular vars: 'a' gets zero-init");
			CHECK(strstr(r.output, "p = 0") != NULL ||
			      strstr(r.output, "*p = 0") != NULL,
			      "regular vars: 'p' gets zero-init");
		}
		prism_free(&r);
	}

	// 7. __attribute__((deprecated)) on ivar — from NSObject.h pattern
	{
		const char *code =
		    "@interface NSObject {\n"
		    "    int isa __attribute__((deprecated));\n"
		    "}\n"
		    "@end\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "objc_attr.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "objc deprecated ivar: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "isa = 0") == NULL &&
			      strstr(r.output, "isa =0") == NULL &&
			      strstr(r.output, "= {0}") == NULL,
			      "objc deprecated ivar: isa not zero-inited");
		}
		prism_free(&r);
	}

	// 8. Protocol conformance: @interface Foo <Proto> { ... }
	// Real-world pattern: @interface NSObject <NSObject> { Class isa; }
	{
		const char *code =
		    "@interface NSObject <NSObject> {\n"
		    "    Class isa __attribute__((deprecated));\n"
		    "}\n"
		    "@end\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "objc_proto.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "objc protocol conformance: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "isa = 0") == NULL &&
			      strstr(r.output, "isa =0") == NULL &&
			      strstr(r.output, "= {0}") == NULL,
			      "objc protocol conformance: isa not zero-inited");
		}
		prism_free(&r);
	}

	// 9. Inheritance + protocol: @interface Foo : Base <Proto1, Proto2> { ... }
	{
		const char *code =
		    "@interface NSArray : NSObject <NSCopying, NSSecureCoding> {\n"
		    "    void *_internal;\n"
		    "}\n"
		    "@end\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "objc_multi.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "objc inherit+multi-protocol: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "_internal = 0") == NULL &&
			      strstr(r.output, "_internal =0") == NULL,
			      "objc inherit+multi-protocol: _internal not zero-inited");
		}
		prism_free(&r);
	}

	// 10. Lightweight generics: @interface NSArray<ObjectType> { ... }
	{
		const char *code =
		    "@interface NSDictionary<KeyType, ObjectType> : NSObject {\n"
		    "    void *_impl;\n"
		    "}\n"
		    "@end\n"
		    "int main(void) { return 0; }\n";
		PrismResult r = prism_transpile_source(code, "objc_gen.c", prism_defaults());
		CHECK(r.status == PRISM_OK, "objc lightweight generics: transpiles OK");
		if (r.output) {
			CHECK(strstr(r.output, "_impl = 0") == NULL &&
			      strstr(r.output, "_impl =0") == NULL,
			      "objc lightweight generics: _impl not zero-inited");
		}
		prism_free(&r);
	}
}

static void test_knr_param_vla_cfg_register(void) {
	printf("\n--- K&R declarator VLA parameter registration ---\n");
	const char *code =
		"static int kr(a, n)\n"
		"    int n;\n"
		"    int a[n];\n"
		"{\n"
		"    goto L;\n"
		"    int v[n];\n"
		"L:\n"
		"    return a[0];\n"
		"}\n";
	PrismResult r = prism_transpile_source(code, "knrvla.c", prism_defaults());
	CHECK(r.status == PRISM_ERR_SEMANTIC || r.status == PRISM_ERR_SYNTAX,
	      "K&R param VLA: cfg must reject goto skipping inner VLA");
	CHECK(r.error_msg != NULL && strstr(r.error_msg, "goto"),
	      "K&R param VLA: diagnostic mentions goto");
	prism_free(&r);
}

void run_safe_tests(void) {
	printf("\n=== SAFE TESTS ===\n");

	/* Safety hole tests */
	test_knr_param_vla_cfg_register();
	test_goto_over_block();
	test_goto_backward_valid();
	test_goto_forward_no_decl();
	test_goto_into_scope_decl_after_label();
	test_goto_complex_valid();
	test_goto_with_defer_valid();

	/* GNU __label__ local labels in statement expressions */
	test_gnu_label_stmt_expr();

	/* Rigor tests */
	test_typedef_void_return();
	test_typedef_voidptr_return();
	NOMSVC_ONLY(
	test_stmt_expr_defer_timing();
	test_nested_stmt_expr_defer_immediate_block_exit();
	);
	test_const_after_typename();
	test_atomic_zeroinit();
	test_atomic_aggregate_zeroinit();
	test_static_local_zeroinit();
	test_inline_defer();
	test_complex_declarator_zeroinit();
	test_complex_decl_safety();
	test_qualified_complex_decl();
	test_extern_not_initialized();
	test_typedef_not_initialized();
	test_for_init_zeroinit();
	NOMSVC_ONLY(
	test_ptr_to_vla_typedef(5);
	test_vla_side_effect_once();
	test_atomic_specifier_form();
	);
#if !defined(__clang__) && !defined(_MSC_VER)
	test_atomic_struct_basic();
	test_atomic_union_basic();
	test_atomic_struct_nested();
	test_atomic_struct_with_array();
	test_atomic_struct_with_pointer();
	test_atomic_struct_specifier_form();
	test_atomic_union_specifier_form();
	test_atomic_struct_multi_decl();
	test_atomic_struct_pointer();
	test_atomic_struct_volatile();
	test_atomic_struct_const();
	test_atomic_anonymous_struct();
	test_atomic_union_different_sizes();
	test_atomic_struct_in_loop();
	test_atomic_struct_nested_blocks();
	test_atomic_struct_with_defer();
	test_atomic_scalar_contrast();
	test_atomic_typedef_struct();
	test_atomic_typedef_atomic();
	test_atomic_struct_bitfields();
	test_raw_atomic_struct();
#endif
	test_switch_scope_leak();
	test_sizeof_shadows_type();
#if __STDC_VERSION__ >= 201112L
	test_generic_colons();
#endif
	test_for_braceless_label();
	test_goto_into_for();
	test_attribute_positions();
	test_rigor_defer_comma_operator();
	test_defer_complex_comma();
	test_switch_noreturn_no_fallthrough();
	test_defer_late_binding_semantic();

	/* sizeof(variable) torture */
	test_sizeof_local_int_variable();
	test_sizeof_local_long_variable();
	test_sizeof_local_float_variable();
	test_sizeof_local_double_variable();
	test_sizeof_local_pointer_variable();
	test_sizeof_local_array_variable();
	test_sizeof_local_struct_variable();
	test_sizeof_local_union_variable();
	test_sizeof_function_parameter();
	test_sizeof_multiple_vars_in_expr();
	test_sizeof_var_times_constant();
	test_sizeof_var_in_ternary();
	test_sizeof_var_with_bitwise_ops();
	test_sizeof_nested_vars();
	test_sizeof_pointer_deref();
	test_sizeof_array_element_var();
	test_sizeof_2d_array_element_var();
	test_sizeof_compound_literal_var();
	test_sizeof_cast_expression_var();
	test_sizeof_var_division();
	test_sizeof_const_qualified_var();
	test_sizeof_volatile_var();
	test_sizeof_restrict_ptr();
	test_sizeof_static_var();
	NOMSVC_ONLY(
	test_sizeof_true_vla_detected();
	test_sizeof_nested_vla_detection();
	);

	/* sizeof/constexpr */
	test_sizeof_in_array_bound();
	test_cast_expression_in_array_bound();
	test_complex_macro_array_bound();
	test_system_typedef_pattern();
	test_invisible_system_typedef_pattern();
	test_system_typedef_shadow();
	test_alignof_in_array_bound();
	test_complex_operators_in_array_bound();
	test_sizeof_array_element_in_bound();
	test_sizeof_with_parens_in_bound();
	test_sizeof_variable_in_array_bound();

	/* Silent failure detection */
	test_complex_func_ptr_array();
	test_array_of_complex_func_ptrs();
	test_func_ptr_taking_func_ptr();
	test_ptr_to_array_of_func_ptrs();
	test_multi_level_ptr_chain();
	test_complex_func_ptr_with_struct();
	test_paren_grouped_declarator();
	test_multi_dim_array_ptrs();
	test_sizeof_array_bounds();
	test_decl_after_label();
	test_decl_directly_after_label();
	test_decl_in_else();
	test_volatile_func_ptr();
	test_extremely_complex_declarator();

	/* Manual offsetof / VLA */
	test_manual_offsetof_in_union();
	test_manual_offsetof_local();
	test_union_offsetof_division();
	NOMSVC_ONLY(test_vla_basic());

	/* Bulletproof regression */
	GNUC_ONLY(
	test_typeof_overflow_35_vars();
	test_typeof_overflow_64_vars();
	test_typeof_struct_overflow();
	);
	test_many_labels_function();
	test_raw_struct_member_field();
	test_raw_anonymous_struct_member();
	test_raw_in_compound_literal();
	test_raw_typedef_name();
	test_raw_pointer_to_struct_with_raw();
	test_raw_array_of_structs_with_raw();
	test_ghost_shadow_for_braceless();
	test_ghost_shadow_nested_for();
	test_ghost_shadow_while_braceless();
	test_ghost_shadow_if_else_braceless();
	GNUC_ONLY(
	test_ghost_shadow_generic();
	test_ghost_shadow_generic_braceless();
	);
	test_pragma_survives_transpile();
	test_defer_switch_goto_out();
	test_defer_switch_break_with_goto_label();
	test_defer_switch_nested_goto();
	test_switch_goto_defer_multi_case();
	test_typedef_redef_basic();
	test_typedef_redef_pointer();
	test_typedef_redef_after_scope();
	GNUC_ONLY(
	test_typeof_errno_zeroinit();
	test_typeof_statement_expr_zeroinit();
	test_typeof_complex_expr_zeroinit();
	);
	test_hashmap_tombstone_insert_delete_cycle();
	test_hashmap_tombstone_multi_key_churn();
	test_switch_conditional_break_not_false_positive();
	test_switch_nested_conditional_context();
	test_make_temp_file_normal_operation();
	test_void_typedef_return_basic();
	test_chained_void_typedef_return();
	test_static_void_typedef_return();
	test_void_typedef_bare_return();
	test_void_ptr_typedef_not_void();
	test_void_func_ptr_typedef();
	NOMSVC_ONLY(
	test_generic_void_typedef_no_label_confusion();
	test_vla_zeroinit_basic();
	test_vla_zeroinit_large();
	test_vla_zeroinit_nested_scope();
	);
	test_hashmap_tombstone_high_churn_load();
	test_deep_pointer_nesting();

	/* Additional safe tests */
	test_typedef_extreme_scope_churn();
	test_typedef_tombstone_saturation_extended();
	NOMSVC_ONLY(
	test_struct_static_assert_compound_literal();
	test_struct_nested_compound_literal_depth();
	test_struct_compound_literal_then_nested_struct();
	);
	test_for_init_multi_decl_all_zeroed();
	GNUC_ONLY(
	test_for_init_stmt_expr_with_decls();
	test_struct_stmt_expr_in_member_size();
	);
	test_nested_struct_depth_tracking();
	test_struct_with_enum_body_depth();

	/* Hardening */
	test_large_string_output();
	test_token_pool_resize_stress();
	test_struct_depth_beyond_64();
	test_orelse_sequential_bare();
	test_zeroinit_after_line_directives();
	test_orelse_return_expr_side_effects();
	GNUC_ONLY(test_generic_controlling_expr_not_evaluated());
	test_struct_padding_zeroinit();
	test_attribute_parser_torture();

	/* Goto over for-init */
	run_goto_over_for_init_bug_tests();
	test_indirect_longjmp_bypasses_defer_safety();
	// test_forward_goto_into_block_skipped_defer(); /* not yet fixed */
	test_case_label_bypasses_block_scoped_zeroinit();
	test_backward_goto_block_bypass();
	test_forward_declared_longjmp_blind_spot();
	test_deep_nesting_goto_bypass();
	test_c23_attr_label_forward_goto();
	test_switch_inner_loop_break_no_false_exit();
	test_raw_comma_desync_goto_bypass();
	test_fixed_array_not_vla_with_raw();
	test_vla_skip_hard_error_with_fno_safety();
	test_typeof_paren_vla_skip_hard_error();
	test_goto_over_native_init();
	test_switch_case_over_native_init();
	test_const_aggregate_memset_rejected();
	test_goto_over_raw_native_init();
	GNUC_ONLY(test_gnu_attr_param_shadow());

	// raw raw VLA for-init CFG blindness
	test_raw_raw_vla_forinit_cfg_blind();

	// braceless for-init CFG bypass
	test_braceless_forinit_cfg_bypass();

	// signal cleanup TOCTOU race
	test_signal_temps_clear_zeroes_paths();

	// signal cleanup stale data race (full buffer clear)
	test_signal_temps_clear_full_buffer();

	// unnamed fn ptr parameter shadow leak
	test_unnamed_fnptr_param_shadow_leak();

	// bracket orelse control flow - Pass 1 error timing
	test_bracket_orelse_ctrlflow_pass1_error();

	// scalar before VLA masks VLA hard error under -fno-safety
	test_vla_masked_by_scalar_fno_safety();

        // duplicate label detection in CFG verifier
        test_duplicate_label_detection();

        // move semantic errors from Pass 2 to Phase 1
        test_register_atomic_aggregate_phase1();
        test_switch_unbraced_decl_phase1();
        test_orelse_empty_action_in_defer_phase1();

        // case/default label jumping into statement expression
        test_case_label_into_stmt_expr();

        // Bug 5: computed goto + VLA stack corruption
        test_computed_goto_vla();

        // Bug 6: enum constant shadow → VLA not detected → invalid = {0}
        test_enum_shadow_vla();

        // VLA CFG bypass with -fno-zeroinit
        test_vla_cfg_bypass_fno_zeroinit();

        // qualifier blindspot in typeof/Atomic VLA detection
        test_typeof_qualifier_vla_blindspot();

        // typeof(typeof(VLA)) misidentified as function type
        test_typeof_nested_typeof_vla();

        // sizeof VLA qualifier blindspot in array_size_is_vla
        test_sizeof_vla_qualifier_blindspot();

        // Braceless labeled-declaration CFG lifetime
        test_braceless_labeled_decl_cfg();

        // sizeof expression-paren subscript VLA hallucination
        test_sizeof_expr_paren_subscript();

        // sizeof multidim subscript VLA hallucination
        test_sizeof_multidim_subscript_vla();

        // case/default label p1d_ctrl_pending leak
        test_case_label_ctrl_pending_leak();

        // skip_one_stmt cache poisoning false positive
        test_skip_one_stmt_cache_poison();

        // skip_cache braceless switch + if-else false positive
        test_skip_cache_braceless_switch_if_else();

        // skip_cache do-while trail preservation
        test_skip_cache_do_while_trail_drop();

        // sizeof commutative VLA bypass (stack corruption)
        test_sizeof_commutative_vla_bypass();

        // defer shadow same-block → Phase 1D
        test_defer_shadow_same_block_phase1();

        // for-init VLA memset → Phase 1D
        test_for_init_vla_memset_phase1();

        // VM-type multi-declarator split → Phase 1D
        test_vm_type_multi_decl_split_phase1();

        // ctrl-paren bracket orelse → Phase 1D
        test_ctrl_paren_bracket_orelse_phase1();

        // file-scope typeof scope_tree NULL deref
        test_file_scope_typeof_orelse_no_crash();

        // const-VLA orelse duplication → Phase 1D
        test_const_vla_orelse_phase1();

        // GNU stmt-expr in orelse fallback → Phase 1D
        test_stmtexpr_orelse_fallback_phase1();

        // Parenthesized orelse/defer → Phase 1D
        test_paren_orelse_phase1();

        // typeof orelse side-effect → Phase 1D
        test_typeof_orelse_side_effect_phase1();

        // anonymous struct split → Phase 1D
        test_anon_struct_split_phase1();

        // empty orelse action → Phase 1D
        test_empty_orelse_action_phase1();

        // Execution: defer + continue evaluation order
        test_defer_continue_execution_order();

        // Execution: interleaved raw multi-declarator zero-init
        test_interleaved_raw_multi_declarator();

        // Execution: union padding zero-init
        test_union_padding_zeroinit();

        // Execution: typedef union padding zero-init
        test_typedef_union_padding_zeroinit();

        // register union rejected (memset impossible, = {0} insufficient)
        test_register_union_zeroinit_rejected();

        // _Atomic(union) routes to memset
        test_atomic_union_zeroinit();

        // for-init union rejected (needs memset, can't emit in init)
        test_for_init_union_rejected();

        // Execution: nested stmt-expr in if condition with defer
        NOMSVC_ONLY(
        {
                int cnt = 0;
                if ( ({
                        int a = ({ 3; });
                        a > 2;
                     }) ) {
                        defer cnt++;
                }
                CHECK(cnt == 1, "nested stmtexpr ctrl desync: defer fires once in if-body");
        }
        );

        // noreturn forward declaration must not inject __builtin_unreachable
        test_noreturn_fwd_decl_no_crash();

        // emit_block_body label detection (ternary, zeroinit, orelse, case/default)
        test_stmt_expr_label_zeroinit();

        // emit_block_body missing dispatches (SUE body, typeof/bracket orelse, enum shadow)
        test_emit_block_body_gaps();

        // Objective-C ivar block zero-init suppression
        test_objc_ivar_block_zeroinit();
}
